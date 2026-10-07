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
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/own.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/time.h"

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
Error burrow__netfd_socket(burrow__NetFD *fd, Str net, int32_t family, int32_t sotype,
                           int32_t proto, bool ipv6only, const void *laddr,
                           const void *raddr, bool group,
                           burrow__NetToSockaddr to_sockaddr, Time deadline);

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

/* Whether this machine can make IPv4 sockets, and IPv6 sockets that take
 * IPv4 as well, which decides the family of a listener on every address.
 * Probed once, as Go does. */
bool burrow__net_supports_ipv4(void);
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
} burrow__NetConnCore;

/* errMissingAddress, for a dial with no address to dial. */
extern const Error burrow__net_err_missing_address;

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

/* ipToSockaddr: the sockaddr of family for ip, port and zone. */
Error burrow__net_ip_sockaddr(int32_t family, NetIP ip, Int port, Str zone,
                              PalSockAddr *out);

/* To16, true when ip is 4 or 16 bytes long. */
bool burrow__net_ip_to16(NetIP ip, Byte out[16]);

/* internetSocket: a socket of type sotype for net, of the family Go would
 * choose for these addresses, bound, listening or connected the way
 * burrow__netfd_socket does it. */
Error burrow__net_internet_socket(burrow__NetFD *fd, Str net,
                                  const burrow__NetInetAddr *laddr,
                                  const burrow__NetInetAddr *raddr, int32_t sotype,
                                  bool listen);

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

/* hasUpperCase, lowerASCIIBytes, stringsEqualFold and stringsHasSuffixFold,
 * all ASCII only. */
bool burrow__net_has_upper_case(Str s);
void burrow__net_lower_ascii_bytes(Byte *x, Int n);
bool burrow__net_equal_fold(Str s, Str t);
bool burrow__net_has_suffix_fold(Str s, Str suffix);

/* -------------------------------------------------------------- dnsclient.go */

/* notFoundError, an error whose text is s and which newDNSError turns into a
 * DNSError with is_not_found set, and temporaryError, a net.Error whose
 * Temporary says true. Both in a. */
BURROW_OWNS(ret) Error burrow__net_not_found_error(Alloc *a, Str s);
BURROW_OWNS(ret) Error burrow__net_temporary_error(Alloc *a, Str s);

/* newDNSError: a DNSError for err, in a. is_timeout and is_temporary are
 * what err says when it is a net.Error, is_not_found is whether it is a
 * notFoundError, and unwrap_err is err when it is or wraps context_canceled
 * or context_deadline_exceeded. */
BURROW_OWNS(ret) Error burrow__net_new_dns_error(Alloc *a, Error err, Str name,
                                                 Str server);

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
