/* What the files of net share with each other and with nothing else.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net */

#ifndef BURROW_SRC_NET_INTERNAL_H
#define BURROW_SRC_NET_INTERNAL_H

#include "burrow/error.h"
#include "burrow/fdmutex.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/own.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#include "../xnet/dnsmessage.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ poll.FD
 *
 * Go's internal/poll.FD for a socket: the descriptor, the fdMutex that keeps
 * it alive while a call is using it, and its place in the netpoller. Every
 * call below takes the lock it needs, makes the system call, and when the
 * answer is that it would block, parks the goroutine in the poller until the
 * descriptor is ready and tries again. So a goroutine blocked on a connection
 * holds a stack and no thread, and a Close from another goroutine wakes it
 * with net_err_closed.
 *
 * The errors are Go's. A system call that fails gives its Errno, which net
 * wraps in an os.SyscallError and then a net.OpError. A call on a closed FD
 * gives net_err_closed, and one that ran past its deadline gives
 * os_err_deadline_exceeded.
 *
 * Only the readiness half is here, which covers everything but Windows. On
 * Windows a socket is read by handing the kernel the read and waiting for it
 * to finish, which is a different file in Go and will be one here, and until
 * then burrow__pfd_init answers ENOSYS there.
 *
 * Every call that can wait has to be made from a goroutine. */
typedef struct burrow__PollFD {
    burrow__FdMutex mu;
    int64_t sysfd;
    burrow__PollDesc *pd;
    uint32_t csema;

    /* Go's IsStream, which caps one system call at a gigabyte, and
     * ZeroReadIsEOF, which makes a read of nothing io_eof. A TCP or Unix
     * stream socket has both. A datagram socket has neither, since an empty
     * datagram is a datagram. */
    bool stream;
    bool zero_read_is_eof;
} burrow__PollFD;

/* poll.ErrNotPollable, for a descriptor the poller turned down. */
extern const Error burrow__net_err_not_pollable;

/* poll.FD.Init: takes sysfd, which has to be non blocking already, into the
 * poller. On an error the FD is not usable and sysfd is still the caller's to
 * close. */
Error burrow__pfd_init(burrow__PollFD *fd, int64_t sysfd, bool stream,
                       bool zero_read_is_eof);

/* poll.FD.Close: marks it closed, wakes every goroutine blocked in it, and
 * waits until the last of them has let go before closing the descriptor.
 * net_err_closed when it was closed already, and the close's own error
 * otherwise. */
Error burrow__pfd_close(burrow__PollFD *fd);

/* The reference a call that neither reads nor writes takes, and gives back.
 * incref gives net_err_closed after a close, and decref gives the close error
 * when it was the last reference after one. */
Error burrow__pfd_incref(burrow__PollFD *fd);
Error burrow__pfd_decref(burrow__PollFD *fd);

/* poll.FD.Read and ReadFrom. A read of an empty p does nothing, and a read
 * of nothing at all on a stream is io_eof. from may be NULL. */
Int burrow__pfd_read(burrow__PollFD *fd, Slice p, Error *err);
Int burrow__pfd_read_from(burrow__PollFD *fd, Slice p, PalSockAddr *from, Error *err);

/* poll.FD.Write, which writes all of p unless something fails, and WriteTo,
 * which sends one datagram to `to`. */
Int burrow__pfd_write(burrow__PollFD *fd, Slice p, Error *err);
Int burrow__pfd_write_to(burrow__PollFD *fd, Slice p, const PalSockAddr *to,
                         Error *err);

/* poll.FD.Accept: the next connection, non blocking and not inherited, with
 * the address of the other end in peer, which may be NULL. -1 on an error. A
 * connection that was aborted before it could be accepted is skipped, as Go
 * does. */
int64_t burrow__pfd_accept(burrow__PollFD *fd, PalSockAddr *peer, Error *err);

/* poll.FD.Shutdown, with PAL_SHUT_RD, PAL_SHUT_WR or PAL_SHUT_RDWR. */
Error burrow__pfd_shutdown(burrow__PollFD *fd, int32_t how);

/* SetDeadline, SetReadDeadline and SetWriteDeadline in one, with mode
 * BURROW_POLL_READ, BURROW_POLL_WRITE or both. The zero Time clears it. */
Error burrow__pfd_set_deadline(burrow__PollFD *fd, Time t, uint32_t mode);

/* poll.FD.WaitWrite: waits until the descriptor is writable, which is how a
 * non blocking connect is waited for. Takes no lock, as in Go, because the
 * caller is the only one who has the FD yet. */
Error burrow__pfd_wait_write(burrow__PollFD *fd);

/* poll.FD.RawControl, RawRead and RawWrite, which hand f the descriptor
 * while holding it open. RawRead calls f until it says it is done, waiting
 * for the descriptor to be readable in between, and RawWrite does the same
 * for writing. A descriptor not in the poller yet, such as the one a
 * Dialer's Control sees, cannot be waited for, and gives an error saying
 * so instead. */
extern const Error burrow__net_err_unsupported_wait;
Error burrow__pfd_raw_control(burrow__PollFD *fd, SyscallFdFunc f);
Error burrow__pfd_raw_read(burrow__PollFD *fd, SyscallFdDoneFunc f);
Error burrow__pfd_raw_write(burrow__PollFD *fd, SyscallFdDoneFunc f);

/* ------------------------------------------------------------------- netFD
 *
 * Go's netFD: a poll FD with what net knows about the socket around it, the
 * family and type it was made with, the network name the caller asked for,
 * and the two addresses. TCPConn, TCPListener and the rest are a netFD and
 * the methods that turn its answers into Go's errors.
 *
 * The errors that come out of here are what Go's netFD gives: an Errno from
 * a system call is wrapped in an os.SyscallError naming the call, and
 * net_err_closed, os_err_deadline_exceeded and io_eof come through as they
 * are. The OpError around all of it is the caller's to add, because the
 * caller knows the operation's name. */
typedef struct burrow__NetFD {
    burrow__PollFD pfd;

    /* Whether pfd went through burrow__pfd_init. Go makes the socket first
     * and hands it to the poller only once it is bound and listening or
     * connected, and a socket that fails before then is closed by hand. */
    bool polled;
    bool is_connected;
    int32_t family;
    int32_t sotype;

    /* The network the caller asked for, such as "tcp4". A literal, since the
     * caller's Str may not live as long as the socket does. */
    Str net;

    /* The addresses, family PAL_AF_UNSPEC when there is none. */
    PalSockAddr laddr;
    PalSockAddr raddr;
} burrow__NetFD;

/* Go's sockaddr interface, cut down to the one method socket needs: the
 * address addr for a socket of family, in out. Fails with a NetAddrError for
 * an address that family cannot hold, such as an IPv6 address on an IPv4
 * socket. addr is never NULL when this is called. */
typedef Error (*burrow__NetToSockaddr)(const void *addr, int32_t family,
                                       PalSockAddr *out);

/* The text of an address that a burrow__NetToSockaddr takes, as a Control
 * function is given it, for a socket of family. */
typedef Str (*burrow__NetAddrText)(const void *addr, int32_t family, Alloc *a);

/* Go's ctrlCtxFn: a Dialer's ControlContext, or its Control or a
 * ListenConfig's Control made to look like one. It sees the socket after it
 * is made and before it is bound or connected, with network being "tcp4",
 * "udp6", "unix" and the like, and address the text of the remote address,
 * or of the local one when there is no remote. */
BURROW_FUNC(burrow__NetCtrlFn, Error, Context ctx, Str network, Str address,
            SyscallRawConn c);

/* What a dial or a listen brings to a socket besides its addresses: the
 * context a connect waits under, which a nil Context leaves with no deadline
 * and nothing to cancel it, and the control function, which is left out
 * when its f is NULL. */
typedef struct burrow__NetSockCtl {
    Context ctx;
    burrow__NetCtrlFn ctrl;
} burrow__NetSockCtl;

/* What a burrow__NetToSockaddr gives for an address the system call would
 * turn down with EINVAL, such as a port past 65535, so that the caller can
 * report it as that call's error, the way Go's syscall package does. */
extern const Error burrow__net_err_sockaddr_einval;

/* Go's socket: makes a socket and then, with laddr and no raddr, binds to
 * laddr and on a stream listens too, and otherwise binds to laddr if there is
 * one and connects to raddr. laddr and raddr are whatever to_sockaddr takes,
 * and either may be NULL. group says that a datagram socket's laddr is a
 * multicast group, for which the socket gets the options that let others
 * listen to the group too. A connect waits, from a goroutine, until it is
 * done or deadline passes, and the zero Time is no deadline. On an error
 * nothing is left open. */
Error burrow__netfd_socket(burrow__NetFD *fd, const burrow__NetSockCtl *ctl, Str net,
                           int32_t family, int32_t sotype, int32_t proto, bool ipv6only,
                           const void *laddr, const void *raddr, bool group,
                           burrow__NetToSockaddr to_sockaddr,
                           burrow__NetAddrText to_text);

/* Go's rawConn and rawListener: the syscall.RawConn a Control function and
 * SyscallConn hand out, over fd. The addresses are what its errors name, nil
 * before the socket has any. A listener's can only be controlled, and its
 * read and write give EINVAL. */
typedef struct burrow__NetRawConn {
    burrow__NetFD *fd;
    NetAddr laddr;
    NetAddr raddr;
    bool listener;
} burrow__NetRawConn;

SyscallRawConn burrow__net_raw_conn(burrow__NetRawConn *rc);

/* netFD.accept: the next connection, in out, with both its addresses. */
Error burrow__netfd_accept(burrow__NetFD *fd, burrow__NetFD *out);

/* netFD.Close. A socket that never reached the poller is closed at once. */
Error burrow__netfd_close(burrow__NetFD *fd);

/* netFD.Read and Write. */
Int burrow__netfd_read(burrow__NetFD *fd, Slice p, Error *err);
Int burrow__netfd_write(burrow__NetFD *fd, Slice p, Error *err);

/* netFD.readFromInet4 and the rest, and writeToInet4 and the rest, for any
 * family, from or to holding the other end's address. */
Int burrow__netfd_read_from(burrow__NetFD *fd, Slice p, PalSockAddr *from, Error *err);
Int burrow__netfd_write_to(burrow__NetFD *fd, Slice p, const PalSockAddr *to,
                           Error *err);

/* What making the sockaddr for a write_to failed with, as Go gives it, which
 * for burrow__net_err_sockaddr_einval is the send's EINVAL. */
Error burrow__netfd_write_to_error(Error err);

/* netFD.shutdown, with PAL_SHUT_RD or PAL_SHUT_WR. */
Error burrow__netfd_shutdown(burrow__NetFD *fd, int32_t how);

/* poll.FD.SetsockoptInt with the error wrapped the way net's setters wrap
 * it, as "setsockopt". */
Error burrow__netfd_setsockopt(burrow__NetFD *fd, int32_t opt, int64_t value);
Error burrow__netfd_setsockopt_mreq(burrow__NetFD *fd, int32_t opt, const PalMreq *m);

/* Whether this machine can make IPv4 sockets, IPv6 sockets, and IPv6 sockets
 * that take IPv4 as well, which decides the family of a listener on every
 * address. Probed once, as Go does. */
bool burrow__net_supports_ipv4(void);
bool burrow__net_supports_ipv6(void);
bool burrow__net_supports_ipv4map(void);

/* -------------------------------------------------------------------- conn
 *
 * Go's conn, the part TCPConn, UDPConn and UnixConn all are: the netFD, the
 * allocator the connection came from, and the two addresses as the NetAddr
 * an OpError holds, nil when there is none. The calls below are conn's
 * methods, with the error each one gives wrapped in the OpError Go's does. */
typedef struct burrow__NetConnCore {
    burrow__NetFD fd;
    Alloc *alloc;
    NetAddr laddr;
    NetAddr raddr;
    /* What SyscallConn hands out, set once the addresses are. */
    burrow__NetRawConn raw;
} burrow__NetConnCore;

/* errMissingAddress, for a dial with no address to dial. */
extern const Error burrow__net_err_missing_address;

/* errTimeout and errCanceled, "i/o timeout" and "operation was canceled",
 * which errors_is matches to context_deadline_exceeded and context_canceled.
 * mapErr turns a context's error into the one of these a lookup or a dial
 * reports and leaves any other error alone. */
extern const Error burrow__net_err_timeout;
extern const Error burrow__net_err_canceled;
Error burrow__net_map_err(Error err);

/* An OpError with these fields, boxed in error_allocator. */
Error burrow__net_op_error(Str op, Str net, NetAddr source, NetAddr addr, Error err);

/* syscall.EINVAL as an error, which is what Go's methods on a nil conn give. */
Error burrow__net_einval(void);

/* conn.Read, Write and Close. A read that reaches the end gives io_eof as it
 * is, and anything else is an OpError naming the operation. */
Int burrow__conn_read(burrow__NetConnCore *c, Slice p, Error *err);
Int burrow__conn_write(burrow__NetConnCore *c, Slice p, Error *err);
Error burrow__conn_close(burrow__NetConnCore *c);

/* conn.SetDeadline and the rest, mode being the netpoll mode, and
 * SetReadBuffer and SetWriteBuffer, opt being PAL_SO_RCVBUF or PAL_SO_SNDBUF. */
Error burrow__conn_set_deadline(burrow__NetConnCore *c, Time t, uint32_t mode);
Error burrow__conn_set_buffer(burrow__NetConnCore *c, int32_t opt, Int bytes);

/* ------------------------------------------------------------ IP sockets
 *
 * What TCPAddr and UDPAddr have in common, which is everything: an IP, a
 * port and a zone. NetTCPAddr and NetUDPAddr have this layout, so a pointer
 * to either is a pointer to one of these. */
typedef struct burrow__NetInetAddr {
    NetIP ip;
    Int port;
    Str zone;
} burrow__NetInetAddr;

/* Room for the bytes of an address read out of a sockaddr, so that the IP
 * and zone can point at something that lives as long as the address does. */
typedef struct burrow__NetInetBytes {
    Byte ip[16];
    Byte zone[12];
} burrow__NetInetBytes;

/* The IP, port and zone of an AF_INET or AF_INET6 sockaddr, with the bytes in
 * b. False for any other family, which leaves everything zero. */
bool burrow__net_inet_from_sockaddr(const PalSockAddr *sa, NetIP *ip, Int *port,
                                    Str *zone, burrow__NetInetBytes *b);

/* TCPAddr.String and UDPAddr.String, "<nil>" for a NULL a. */
BURROW_OWNS(ret) Str burrow__net_inet_addr_string(const burrow__NetInetAddr *a,
                                                  Alloc *al);

/* An address in a with its own copy of ip's iplen bytes and of zone, which
 * burrow__net_inet_addr_free gives back. This is what the TCPAddr and
 * UDPAddr values a caller gets and frees are. */
BURROW_OWNS(ret) burrow__NetInetAddr *
burrow__net_inet_addr_new(Alloc *a, const Byte *ip, Int iplen, Int port, Str zone);
void burrow__net_inet_addr_free(Alloc *a, burrow__NetInetAddr *addr);

/* AddrPort and the FromAddrPort functions of TCPAddr and UDPAddr. */
NetipAddrPort burrow__net_inet_addr_port(const burrow__NetInetAddr *a);
BURROW_OWNS(ret) burrow__NetInetAddr *burrow__net_inet_from_addr_port(Alloc *a,
                                                                      NetipAddrPort ap);

/* The same for an IPAddr, which net_ip_addr_free gives back. */
BURROW_OWNS(ret) NetIPAddr *burrow__net_ip_addr_new(Alloc *a, NetIP ip, Str zone);

/* ipToSockaddr: the sockaddr of family for ip, port and zone. */
Error burrow__net_ip_sockaddr(int32_t family, NetIP ip, Int port, Str zone,
                              PalSockAddr *out);

/* To16, true when ip is 4 or 16 bytes long. */
bool burrow__net_ip_to16(NetIP ip, Byte out[16]);

/* internetSocket: a socket of type sotype and protocol proto for net, of the
 * family Go would choose for these addresses, bound, listening or connected
 * the way burrow__netfd_socket does it. */
Error burrow__net_internet_socket(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                                  Str net, const burrow__NetInetAddr *laddr,
                                  const burrow__NetInetAddr *raddr, int32_t sotype,
                                  int32_t proto, bool listen);

/* The part of a Dialer or a ListenConfig that the sockets of each kind need:
 * the context and control function a socket is made with, and the
 * keep-alive a new TCP connection gets, which is Go's newTCPConn rule. A
 * zero one is what DialTCP and the other functions of each kind use.
 * alloc_mu, when it is not NULL, is held around each use of the allocator,
 * for the two TCP dials a Happy Eyeballs race runs at once. */
typedef struct burrow__NetSysOpts {
    burrow__NetSockCtl ctl;
    SyncMutex *alloc_mu;
    Duration keep_alive;
    NetKeepAliveConfig keep_alive_config;
} burrow__NetSysOpts;

/* sysDialer.dialTCP, dialUDP and dialUnix, and sysListener.listenTCP,
 * listenUDP, listenUnix and listenUnixgram: o may be NULL, and the error is
 * the one to put in an OpError, which the caller adds, except that running
 * out of memory is burrow_err_out_of_memory by itself. network has to be
 * one of the kind's networks. */
BURROW_OWNS(ret) NetTCPConn *
burrow__net_sys_dial_tcp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                         const NetTCPAddr *laddr, const NetTCPAddr *raddr, Error *err);
BURROW_OWNS(ret) NetTCPListener *
burrow__net_sys_listen_tcp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                           const NetTCPAddr *laddr, Error *err);
BURROW_OWNS(ret) NetUDPConn *
burrow__net_sys_dial_udp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                         const NetUDPAddr *laddr, const NetUDPAddr *raddr, Error *err);
BURROW_OWNS(ret) NetUDPConn *
burrow__net_sys_listen_udp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                           const NetUDPAddr *laddr, Error *err);
BURROW_OWNS(ret) NetUnixConn *
burrow__net_sys_dial_unix(Alloc *a, const burrow__NetSysOpts *o, Str network,
                          const NetUnixAddr *laddr, const NetUnixAddr *raddr,
                          Error *err);
BURROW_OWNS(ret) NetUnixListener *
burrow__net_sys_listen_unix(Alloc *a, const burrow__NetSysOpts *o, Str network,
                            const NetUnixAddr *laddr, Error *err);
/* Whether c is an end of a net_pipe. */
bool burrow__net_is_pipe(NetConn c);

/* DialTCP, DialUDP and DialUnix with a Dialer's options in o, which may be
 * NULL, and Go's checks and errors. */
BURROW_OWNS(ret) NetTCPConn *burrow__net_dial_tcp(Alloc *a, const burrow__NetSysOpts *o,
                                                  Str network, const NetTCPAddr *laddr,
                                                  const NetTCPAddr *raddr, Error *err);
BURROW_OWNS(ret) NetUDPConn *burrow__net_dial_udp(Alloc *a, const burrow__NetSysOpts *o,
                                                  Str network, const NetUDPAddr *laddr,
                                                  const NetUDPAddr *raddr, Error *err);
BURROW_OWNS(ret) NetUnixConn *
burrow__net_dial_unix(Alloc *a, const burrow__NetSysOpts *o, Str network,
                      const NetUnixAddr *laddr, const NetUnixAddr *raddr, Error *err);

/* partialDeadline: the deadline for the next of addrs_remaining addresses
 * when the whole dial has until deadline, both on the monotonic clock, with
 * 0 for no deadline. burrow__net_err_timeout when the time is up. */
Error burrow__net_partial_deadline(int64_t now, int64_t deadline, Int addrs_remaining,
                                   int64_t *out);

/* parseNetwork: the network without the protocol an "ip:" one has after it,
 * in *afnet, and that protocol's number in *proto. needs_proto turns down an
 * "ip" network that has none. */
Error burrow__net_parse_network(Str network, bool needs_proto, Str *afnet, Int *proto);

/* Resolver.lookupIPAddr, which the default resolver's is for a NULL r: the
 * NetIPAddr values of host in a, for network, whose last byte says whether
 * only IPv4 or only IPv6 is wanted. */
BURROW_OWNS(ret) Slice burrow__net_lookup_ip_addr(NetResolver *r, Alloc *a, Context ctx,
                                                  Str network, Str host, Error *err);

BURROW_OWNS(ret) NetUnixConn *
burrow__net_sys_listen_unixgram(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                const NetUnixAddr *laddr, Error *err);

/* dialIP and listenIP, for an "ip", "ip4" or "ip6" network with its
 * protocol. The errors are not wrapped. */
BURROW_OWNS(ret) NetIPConn *burrow__net_sys_dial_ip(Alloc *a,
                                                    const burrow__NetSysOpts *o,
                                                    Str network, const NetIPAddr *laddr,
                                                    const NetIPAddr *raddr, Error *err);
BURROW_OWNS(ret) NetIPConn *
burrow__net_sys_listen_ip(Alloc *a, const burrow__NetSysOpts *o, Str network,
                          const NetIPAddr *laddr, Error *err);

/* ------------------------------------------------------------------ parse.go
 *
 * Go's file from parse.go: a file read a line at a time through a buffer of
 * 64 KiB, and the small string helpers the resolver's files are read with.
 * data[start:end] is what has been read and not handed out yet. */
typedef struct burrow__NetFile {
    OsFile *file;
    Byte *data;
    Int start;
    Int end;
    bool at_eof;
} burrow__NetFile;

/* open: the file and its buffer in a, or NULL and the error from os_open. */
BURROW_OWNS(ret) burrow__NetFile *burrow__net_open(Alloc *a, Str name, Error *err);

/* close, which also gives back what burrow__net_open took from a. */
void burrow__net_file_close(burrow__NetFile *f, Alloc *a);

/* readLine: the next line without its newline, and false at the end. The line
 * points into f's buffer and lasts until the next call. */
bool burrow__net_file_read_line(burrow__NetFile *f, Str *line);

/* splitAtBytes: the runs of s between bytes of t, and getFields, the runs
 * between spaces, tabs, CRs and newlines. A Slice of Str in a, each pointing
 * into s. */
BURROW_OWNS(ret) Slice burrow__net_split_at_bytes(Alloc *a, Str s, Str t);
BURROW_OWNS(ret) Slice burrow__net_get_fields(Alloc *a, Str s);

/* big, and dtoi: the decimal number at the start of s, how many bytes it
 * took, and whether there was one. It gives up at big. */
#define BURROW__NET_BIG 0xFFFFFF
bool burrow__net_dtoi(Str s, Int *n, Int *used);

/* xtoi, the same in hex, which gives 0 when it gives up, and xtoi2: the byte
 * in the two hex digits at the start of s, which have to be followed by e or
 * by nothing. */
bool burrow__net_xtoi(Str s, Int *n, Int *used);
bool burrow__net_xtoi2(Str s, Byte e, Byte *b);

/* The errors interface.go keeps for itself. */
extern const Error burrow__net_err_invalid_interface;
extern const Error burrow__net_err_invalid_interface_index;
extern const Error burrow__net_err_invalid_interface_name;
extern const Error burrow__net_err_no_such_interface;
extern const Error burrow__net_err_no_such_multicast_interface;

/* parseProcNetIGMP and parseProcNetIGMP6: the groups in a Linux /proc file,
 * of ifi or of every interface when it is NULL, added to *out, which grows in
 * a. A file that cannot be opened adds nothing. False only when a runs out of
 * memory. They are built everywhere so that the tests can read Go's copies of
 * the files. */
bool burrow__net_parse_proc_net_igmp(Alloc *a, Str path, const NetInterface *ifi,
                                     Slice *out);
bool burrow__net_parse_proc_net_igmp6(Alloc *a, Str path, const NetInterface *ifi,
                                      Slice *out);

/* The zone cache. burrow__net_zone_name is zoneCache.name: the interface
 * called by index, kept for the life of the process, or the index in decimal
 * written into buf, which has room for 24 bytes. burrow__net_zone_index is
 * zoneCache.index. burrow__net_zone_cache_update is zoneCache.update(nil,
 * force), which the tests call. */
Str burrow__net_zone_name(Int index, Byte *buf);
Int burrow__net_zone_index(Str name);
void burrow__net_zone_cache_update(bool force);

/* hasUpperCase, lowerASCIIBytes, stringsEqualFold and stringsHasSuffixFold,
 * all ASCII only. */
bool burrow__net_has_upper_case(Str s);
void burrow__net_lower_ascii_bytes(Byte *x, Int n);
bool burrow__net_equal_fold(Str s, Str t);
bool burrow__net_has_suffix_fold(Str s, Str suffix);

/* The n Strs after n one after the other, in a, with a NUL after them that
 * the length leaves out. Empty when a is out of memory. */
BURROW_OWNS(ret) Str burrow__net_cat(Alloc *a, int n, ...);

/* The value of key in the GODEBUG environment variable, the last one when it
 * is there twice, as Go's internal/godebug reads it. The value points into
 * the environment. */
bool burrow__net_godebug(const char *key, Str *val);

/* A Type for a struct that lives only in this package, enough for a Slice of
 * them: the Go name, the C type and a hash that no other Type in the library
 * has. */
#define BURROW__NET_ELEM_DESC(name, go_name, ctype, hash)                              \
    static const Type name = {                                                         \
        {(const Byte *)(go_name), (Int)(sizeof(go_name) - 1)},                         \
        {(const Byte *)"net", 3},                                                      \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        hash,                                                                          \
        NULL,                                                                          \
    }

/* ------------------------------------------------------------------ port.go */

/* parsePort: service as a decimal port in port, or true when it is not a
 * number and has to be looked up. A number too big comes out as 1<<30-1 or
 * -(1<<30), which nothing takes for a port. "" is port 0. */
bool burrow__net_parse_port(Str service, Int *port);

/* goLookupPort: the port of a service by name, from the table Go has and
 * /etc/services on top of it. The error is a DNSError in error_allocator(). */
Int burrow__net_lookup_port_map(Str network, Str service, Error *err);

/* lookupProtocol: the number of an IP protocol by name, from the table Go has
 * and /etc/protocols under it. The error is an AddrError in
 * error_allocator(). */
Int burrow__net_lookup_protocol(Str name, Error *err);

/* dnsWaitGroup.Wait: waits for every lookup that was started in a goroutine
 * to finish with its resolver. A caller that gives up through its context
 * returns while the lookup goes on with the same NetResolver, so a test with
 * a resolver on its stack calls this before it returns, where Go's tests
 * defer dnsWaitGroup.Wait. */
void burrow__net_dns_wait(void);

/* ------------------------------------------------------------- cgo_unix.go */

/* The lookups Go makes through the C library, made here through the platform
 * layer on threads kept for them (src/net/cgo.c). Errors are in
 * error_allocator() and results in a. */

/* cgoLookupHost: the addresses of name as strings. */
BURROW_OWNS(ret) Slice burrow__net_cgo_lookup_host(Alloc *a, Context ctx, Str name,
                                                   Error *err);

/* cgoLookupIP: the NetIPAddr values of name, of the version network ends in. */
BURROW_OWNS(ret) Slice burrow__net_cgo_lookup_ip(Alloc *a, Context ctx, Str network,
                                                 Str name, Error *err);

/* cgoLookupPort: the port of service for network, which is "ip" or one of
 * the TCP and UDP networks. */
Int burrow__net_cgo_lookup_port(Context ctx, Str network, Str service, Error *err);

/* cgoLookupPTR: the name of addr, as an absolute domain name. */
BURROW_OWNS(ret) Slice burrow__net_cgo_lookup_ptr(Alloc *a, Context ctx, Str addr,
                                                  Error *err);

/* cgoLookupCNAME: the first answer of a CNAME query through res_search. */
BURROW_OWNS(ret) Str burrow__net_cgo_lookup_cname(Alloc *a, Context ctx, Str name,
                                                  Error *err);

/* -------------------------------------------------------------- dnsclient.go */

/* notFoundError, an error whose text is s and which newDNSError turns into a
 * DNSError with is_not_found set, and temporaryError, a net.Error whose
 * Temporary says true. Both in a. */
BURROW_OWNS(ret) Error burrow__net_not_found_error(Alloc *a, Str s);
BURROW_OWNS(ret) Error burrow__net_temporary_error(Alloc *a, Str s);

/* The errors the lookups give, by the names Go has for them. no_such_host
 * and unknown_port are notFoundErrors and server_temporarily_misbehaving is a
 * temporaryError. */
extern const Error burrow__net_err_no_such_host;
extern const Error burrow__net_err_unknown_port;
extern const Error burrow__net_err_server_temporarily_misbehaving;
extern const Error burrow__net_err_no_suitable_address;
extern const Error burrow__net_err_lame_referral;
extern const Error burrow__net_err_cannot_unmarshal;
extern const Error burrow__net_err_cannot_marshal;
extern const Error burrow__net_err_server_misbehaving;
extern const Error burrow__net_err_invalid_dns_response;
extern const Error burrow__net_err_no_answer_from_dns_server;

/* newDNSError: a DNSError for err, in a. is_timeout and is_temporary are
 * what err says when it is a net.Error, is_not_found is whether it is a
 * notFoundError, and unwrap_err is err when it is or wraps context_canceled
 * or context_deadline_exceeded. */
BURROW_OWNS(ret) Error burrow__net_new_dns_error(Alloc *a, Error err, Str name,
                                                 Str server);

/* The DNSError newDNSError makes, before it becomes an Error, for a caller
 * that has more to set. The strings are borrowed. */
NetDNSError burrow__net_dns_error_of(Error err, Str name, Str server);

/* err.(*DNSError): the DNSError err is, and not one it wraps, or NULL. */
BURROW_BORROWS(ret, err) const NetDNSError *burrow__net_as_dns_error(Error err);

/* reverseaddr: the in-addr.arpa. or ip6.arpa. name of addr, in a, or empty
 * and a DNSError "unrecognized address" when addr is not an IP address. */
BURROW_OWNS(ret) Str burrow__net_reverseaddr(Alloc *a, Str addr, Error *err);

/* isDomainName: whether s is a domain name of letters, digits, hyphens and
 * underscores that would fit in a DNS message. */
bool burrow__net_is_domain_name(Str s);

/* absDomainName: s with a dot on the end when it has a dot in it and does not
 * end with one, so "localhost" stays as it is. The result is in a when it is
 * new and is s when it is not. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__net_abs_domain_name(Alloc *a,
                                                                        Str s);

/* byPriorityWeight.shuffleByWeight, byPriorityWeight.sort and byPref.sort,
 * on n pointers to records, which is what Go's []*SRV and []*MX are. The
 * order of equal weights and preferences is random, as RFC 2782 and RFC 5321
 * want. */
void burrow__net_srv_shuffle_by_weight(NetSRV **addrs, Int n);
void burrow__net_srv_sort(NetSRV **addrs, Int n);
void burrow__net_mx_sort(NetMX **s, Int n);

/* --------------------------------------------------------------- dnsconfig.go
 *
 * What resolv.conf says, read the way Go's pure resolver reads it. */
typedef struct burrow__DNSConfig {
    Slice servers; /* Str, as host:port */
    Slice search;  /* Str, each ending in a dot */
    Int ndots;     /* dots in a name that make it tried as it is first */
    Duration timeout;
    Int attempts; /* tries per server */
    bool rotate;  /* round robin over the servers */
    bool unknown_opt;
    Slice lookup; /* Str, OpenBSD's lookup order */
    Error err;    /* why the file could not be opened, if it could not */
    Time mtime;
    uint32_t soffset; /* what server_offset counts with */
    bool single_request;
    bool use_tcp;
    bool trust_ad;
    bool no_reload;
} burrow__DNSConfig;

/* defaultNS, the servers to ask when resolv.conf names none. */
extern const Str burrow__net_default_ns[2];

/* isDefaultNS: whether servers is defaultNS itself, which is how Go tells a
 * file with no nameserver line from one that names the same two. */
bool burrow__dns_config_is_default_ns(const burrow__DNSConfig *c);

/* serverOffset: 0 each time, or with rotate one more each time. */
uint32_t burrow__dns_config_server_offset(burrow__DNSConfig *c);

/* What Go's getHostname is, which its tests replace. NULL means
 * os_hostname. */
typedef Str (*burrow__NetHostnameFunc)(Alloc *a, Error *err);

/* dnsReadConfig: filename read into c, with everything in a. A file that
 * will not open gives the defaults and its error in c->err. */
void burrow__dns_read_config(Alloc *a, Str filename, burrow__NetHostnameFunc hostname,
                             burrow__DNSConfig *c);

/* dnsDefaultSearch: the domain of the host's name, rooted, or the nil Slice
 * when the name has no domain or cannot be had. */
BURROW_OWNS(ret) Slice burrow__dns_default_search(Alloc *a,
                                                  burrow__NetHostnameFunc hostname);

/* ensureRooted: s with a dot on the end if it has none. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__net_ensure_rooted(Alloc *a, Str s);

/* avoidDNS: whether name is empty or under .onion, which RFC 7686 says never
 * to ask DNS about. */
bool burrow__net_avoid_dns(Str name);

/* nameList: the names to ask for, in order, with the search list applied the
 * way ndots says. The nil Slice for a name too long to look up. */
BURROW_OWNS(ret) Slice burrow__dns_config_name_list(const burrow__DNSConfig *c,
                                                    Alloc *a, Str name);

/* ------------------------------------------------------------------- nss.go
 *
 * nsswitch.conf, kept the way resolv.conf is. A burrow__NssConf is counted,
 * and whoever has one gives it back with burrow__nss_conf_put. */
typedef struct burrow__NssCriterion {
    Str status; /* such as "success", in lower case */
    Str action; /* such as "return", in lower case */
    bool negate;
} burrow__NssCriterion;

typedef struct burrow__NssSource {
    Str source;     /* such as "files" or "mdns4_minimal" */
    Slice criteria; /* burrow__NssCriterion */
} burrow__NssSource;

typedef struct burrow__NssDatabase {
    Str name;      /* such as "hosts" */
    Slice sources; /* burrow__NssSource */
} burrow__NssDatabase;

typedef struct burrow__NssConf {
    Arena ar; /* everything below */
    Time mtime;
    Error err;
    Slice dbs; /* burrow__NssDatabase, in the order the file has them */
    int32_t refs;
} burrow__NssConf;

/* parseNSSConfFile: file read into a new conf with one reference, or NULL
 * when there is no memory for one. A file that will not open or parse gives
 * a conf with err set. */
BURROW_OWNS(ret) burrow__NssConf *burrow__nss_parse_file(Str file);
void burrow__nss_conf_put(burrow__NssConf *conf);

/* sources[db], the nil Slice when the file does not name db. */
BURROW_BORROWS(ret, conf) Slice burrow__nss_conf_sources(const burrow__NssConf *conf,
                                                         Str db);

/* nssSource.standardCriteria. */
bool burrow__nss_source_standard_criteria(const burrow__NssSource *s);

/* getSystemNSS: a reference to what /etc/nsswitch.conf says, looked at again
 * when five seconds have gone by. NULL when there was never the memory to
 * read it. */
BURROW_OWNS(ret) burrow__NssConf *burrow__net_system_nss(void);

/* setSystemNSS, for tests: conf, whose reference this takes, is what the
 * system says from now on, and the file is not looked at again until
 * add_dur from now has gone by less five seconds. */
void burrow__net_set_system_nss(burrow__NssConf *conf, Duration add_dur);

/* ------------------------------------------------------------ addrselect.go */

/* policyTableEntry, a row of the RFC 6724 policy table. */
typedef struct burrow__NetPolicyEntry {
    NetipPrefix prefix;
    uint8_t precedence;
    uint8_t label;
} burrow__NetPolicyEntry;

/* policyTable.Classify: the row for ip, the zero one when none matches. */
burrow__NetPolicyEntry burrow__net_policy_classify(NetipAddr ip);

/* scope, the values classifyScope gives that have a name. */
enum {
    BURROW__NET_SCOPE_INTERFACE_LOCAL = 0x1,
    BURROW__NET_SCOPE_LINK_LOCAL = 0x2,
    BURROW__NET_SCOPE_ADMIN_LOCAL = 0x4,
    BURROW__NET_SCOPE_SITE_LOCAL = 0x5,
    BURROW__NET_SCOPE_ORG_LOCAL = 0x8,
    BURROW__NET_SCOPE_GLOBAL = 0xe
};

/* classifyScope */
uint8_t burrow__net_classify_scope(NetipAddr ip);

/* commonPrefixLen: how many leading bits a and b share, counting no more
 * than the first 64 of an IPv6 address, and 0 for two of different
 * families. */
Int burrow__net_common_prefix_len(NetipAddr a, NetIP b);

/* sortByRFC6724: addrs in the order RFC 6724 says to try them, which needs
 * the source address for each, and sortByRFC6724withSrcs with those given.
 * Out of memory leaves addrs as it was. */
void burrow__net_sort_by_rfc6724(NetIPAddr *addrs, Int n);
void burrow__net_sort_by_rfc6724_with_srcs(NetIPAddr *addrs, const NetipAddr *srcs,
                                           Int n);

/* ------------------------------------------------------- resolv.conf, held
 *
 * resolverConfig from dnsclient_unix.go: what /etc/resolv.conf said when it
 * was last read, read again when its modification time moves and looked at
 * no more than every five seconds, or on every lookup while it gives the
 * default name servers. A config from here is counted, and whoever has one
 * gives it back with burrow__dns_config_put. */

/* getSystemDNSConfig. Never NULL: without the memory to read the file it is
 * a config with the defaults in it. */
BURROW_OWNS(ret) burrow__DNSConfig *burrow__net_system_dns_config(void);
void burrow__dns_config_put(burrow__DNSConfig *c);

/* getSystemDNSConfigNamed: the same, with the file looked at being name, for
 * tests. */
BURROW_OWNS(ret) burrow__DNSConfig *burrow__net_system_dns_config_named(Str name);

/* distantFuture, a last check time for the hooks below that means never
 * look at the file again. */
Time burrow__net_distant_future(void);

/* forceUpdateConf, for tests: c, copied, is the config from now on, with
 * last_checked as the time the file was last looked at. What the strings and
 * slices in c point at is borrowed, and has to outlive the config. */
void burrow__net_force_dns_config(const burrow__DNSConfig *c, Time last_checked);

/* forceUpdate, for tests: the same with the config read from filename. */
void burrow__net_force_dns_config_file(Str filename, Time last_checked);

/* ------------------------------------------------------------------ conf.go */

/* hostLookupOrder, the order to look a name up in: the system's resolver
 * (src/net/cgo.c), the hosts file then DNS, DNS then the hosts file, one or
 * the other. */
typedef enum burrow__HostLookupOrder {
    BURROW__HOST_LOOKUP_CGO,
    BURROW__HOST_LOOKUP_FILES_DNS,
    BURROW__HOST_LOOKUP_DNS_FILES,
    BURROW__HOST_LOOKUP_FILES,
    BURROW__HOST_LOOKUP_DNS
} burrow__HostLookupOrder;

/* hostLookupOrder.String, such as "files,dns", in a. */
BURROW_OWNS(ret) Str burrow__host_lookup_order_string(int32_t o, Alloc *a);

/* mdnsTest */
enum {
    BURROW__MDNS_FROM_SYSTEM,
    BURROW__MDNS_ASSUME_EXISTS,
    BURROW__MDNS_ASSUME_DOES_NOT_EXIST
};

/* conf, with Go's cgoAvailable as a field so that the tests can have it. */
typedef struct burrow__NetConf {
    Str goos;
    int32_t dns_debug_level;
    int32_t mdns_test;
    bool net_go;
    bool net_cgo;
    bool prefer_cgo;
    bool cgo_available;
} burrow__NetConf;

/* systemConf */
BURROW_STATIC(ret) const burrow__NetConf *burrow__net_system_conf(void);

/* goDebugNetDNS: the mode and the debug level in GODEBUG=netdns. */
void burrow__net_go_debug_net_dns(Str *mode, Int *level);

/* mustUseGoResolver */
bool burrow__net_conf_must_use_go_resolver(const burrow__NetConf *c,
                                           const NetResolver *r);

/* hostLookupOrder and addrLookupOrder. *dns_conf is the system's resolv.conf
 * when it was looked at, to be given back with burrow__dns_config_put, and
 * NULL when not. */
burrow__HostLookupOrder
burrow__net_conf_host_lookup_order(const burrow__NetConf *c, const NetResolver *r,
                                   Str hostname, burrow__DNSConfig **dns_conf);
burrow__HostLookupOrder
burrow__net_conf_addr_lookup_order(const burrow__NetConf *c, const NetResolver *r,
                                   Str addr, burrow__DNSConfig **dns_conf);

/* Go's tests set getHostname, and this is how ours do. NULL goes back to
 * os_hostname. */
void burrow__net_set_get_hostname(burrow__NetHostnameFunc fn);

/* getHostname as the tests have it set, which reading resolv.conf uses. */
burrow__NetHostnameFunc burrow__net_get_hostname(void);

/* ------------------------------------------------------- dnsclient_unix.go
 *
 * The lookups that ask DNS themselves. A NULL conf is the system's, looked
 * at when it is needed. r is never NULL here. Results are in a and errors
 * in error_allocator(). */

/* lookup: name asked for, under each name the search list makes of it, with
 * the answer in ma, at the first answer of type qtype, and *server, in ma,
 * the server that gave it. Not finding one is a DNSError named name. */
Error burrow__net_dns_lookup(NetResolver *r, Alloc *ma, Context ctx, Str name,
                             DnsmsgType qtype, burrow__DNSConfig *conf, DnsmsgParser *p,
                             Str *server);

/* exchange: q to server, over UDP and then over TCP when the answer is cut
 * short, or over TCP alone with use_tcp. The answer is in ma, with p just
 * past its question. ad asks for the AD bit. */
Error burrow__net_dns_exchange(NetResolver *r, Alloc *ma, Context ctx, Str server,
                               DnsmsgQuestion q, Duration timeout, bool use_tcp,
                               bool ad, DnsmsgParser *p, DnsmsgHeader *h);

/* tryOneName: name asked of each server in turn, cfg->attempts times over,
 * until one answers. The answer is in ma, *server is borrowed from cfg, and
 * the error is a DNSError in error_allocator(). */
Error burrow__net_dns_try_one_name(NetResolver *r, Alloc *ma, Context ctx,
                                   burrow__DNSConfig *cfg, Str name, DnsmsgType qtype,
                                   DnsmsgParser *p, Str *server);

/* goLookupHostOrder, as a Slice of Str. */
BURROW_OWNS(ret) Slice burrow__net_go_lookup_host_order(NetResolver *r, Alloc *a,
                                                        Context ctx, Str name,
                                                        burrow__HostLookupOrder order,
                                                        burrow__DNSConfig *conf,
                                                        Error *err);

/* goLookupIPFiles: what the hosts file says name is, as NetIPAddr values
 * sorted the way RFC 6724 says, and its canonical name. */
BURROW_OWNS(ret) Slice burrow__net_go_lookup_ip_files(Alloc *a, Str name,
                                                      Str *canonical);

/* goLookupIPCNAMEOrder: the NetIPAddr values of name for network, "ip",
 * "ip4", "ip6" or "CNAME", and the canonical name in *cname, which may be
 * NULL. */
BURROW_OWNS(ret) Slice burrow__net_go_lookup_ip_cname_order(
    NetResolver *r, Alloc *a, Context ctx, Str network, Str name,
    burrow__HostLookupOrder order, burrow__DNSConfig *conf, Str *cname, Error *err);

/* goLookupCNAME and goLookupPTR. */
BURROW_OWNS(ret) Str burrow__net_go_lookup_cname(NetResolver *r, Alloc *a, Context ctx,
                                                 Str host,
                                                 burrow__HostLookupOrder order,
                                                 burrow__DNSConfig *conf, Error *err);
BURROW_OWNS(ret) Slice burrow__net_go_lookup_ptr(NetResolver *r, Alloc *a, Context ctx,
                                                 Str addr,
                                                 burrow__HostLookupOrder order,
                                                 burrow__DNSConfig *conf, Error *err);

/* ------------------------------------------------------------------ hosts.go
 *
 * The hosts file, read again when it changes and at most every five
 * seconds. */

/* lookupStaticHost: host's addresses, as a Slice of Str in a, and its
 * canonical name, also in a. The nil Slice when the file does not list
 * host. */
BURROW_OWNS(ret) Slice burrow__net_lookup_static_host(Alloc *a, Str host,
                                                      Str *canonical);

/* lookupStaticAddr: the names for addr, as a Slice of Str in a, rooted when
 * they have a dot. The nil Slice when the file does not list addr. */
BURROW_OWNS(ret) Slice burrow__net_lookup_static_addr(Alloc *a, Str addr);

/* Go's tests set hostsFilePath, and this is how ours do. The path is copied,
 * and an empty one goes back to the system's. */
void burrow__net_set_hosts_file_path(Str path);

#endif /* BURROW_SRC_NET_INTERNAL_H */
