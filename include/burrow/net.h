/* net: the addresses, the interfaces, Pipe, the sockets, the resolver,
 * Dial and Listen.
 *
 * Go's net/ip.go, the interfaces and errors from net.go, SplitHostPort and
 * JoinHostPort from ipsock.go, pipe.go, the TCP, UDP and Unix sockets, the
 * resolver, and dial.go. IPConn, ListenPacket, Interface and the rest of
 * the package come later.
 *
 * A NetIP is a byte slice, 4 bytes for an IPv4 address or 16 for IPv6, as in
 * Go. Functions take either length, and the ones that make an address give
 * back the 16 byte form, so an IPv4 address usually has its IPv4-mapped IPv6
 * form:
 *
 *     NetIP ip = net_parse_ip(a, BURROW_S("192.0.2.1"));
 *     NetIP v4 = net_ip_to4(ip);           // the last 4 bytes of ip
 *     Str s = net_ip_string(ip, a);        // "192.0.2.1"
 *
 * The nil slice is no address at all, and is what the parse functions give
 * for text that is not one. net/netip's NetipAddr is the better type for new
 * code. NetIP is for the APIs that were written around it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net */

#ifndef BURROW_NET_H
#define BURROW_NET_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net/netip.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- errors */

/* net.ParseError, text that was not the address it was meant to be. type is
 * what was expected, such as "IP address" or "CIDR address", and text is what
 * came instead. Go's Error method gives "invalid " + type + ": " + text. */
typedef struct NetParseError {
    Str type;
    Str text;
} NetParseError;

extern const Type *const TYPE_NET_PARSE_ERROR;

BURROW_OWNS(ret) Str net_parse_error_error(const NetParseError *e, Alloc *a);

/* Timeout and Temporary are always false, as in Go. */
bool net_parse_error_timeout(const NetParseError *e);
bool net_parse_error_temporary(const NetParseError *e);

/* The Error for e. type and text are copied into a, and errors_as with
 * TYPE_NET_PARSE_ERROR gets the struct back. */
BURROW_OWNS(ret) Error net_parse_error_as_error(const NetParseError *e, Alloc *a);

/* net.AddrError, an address that is wrong for the reason in err. Go's Error
 * method gives "address " + addr + ": " + err, or just err when addr is empty,
 * and "<nil>" for a NULL e. */
typedef struct NetAddrError {
    Str err;
    Str addr;
} NetAddrError;

extern const Type *const TYPE_NET_ADDR_ERROR;

BURROW_OWNS(ret) Str net_addr_error_error(const NetAddrError *e, Alloc *a);
bool net_addr_error_timeout(const NetAddrError *e);
bool net_addr_error_temporary(const NetAddrError *e);
BURROW_OWNS(ret) Error net_addr_error_as_error(const NetAddrError *e, Alloc *a);

/* ----------------------------------------------------------------------- IP */

/* net.IPv4len and net.IPv6len. */
#define NET_IPV4_LEN 4
#define NET_IPV6_LEN 16

/* net.IP, a []byte of 4 or 16 bytes. */
typedef Slice NetIP;

/* net.IPMask, a []byte of 4 or 16 bytes with the network bits set. */
typedef Slice NetIPMask;

/* net.IPNet, a network number and its mask. */
typedef struct NetIPNet {
    NetIP ip;
    NetIPMask mask;
} NetIPNet;

/* net.IPv4: a.b.c.d in its 16 byte form. */
BURROW_OWNS(ret) NetIP net_ipv4(Alloc *a, Byte a0, Byte b, Byte c, Byte d);

/* net.IPv4Mask: the 4 byte mask a.b.c.d. */
BURROW_OWNS(ret) NetIPMask net_ipv4_mask(Alloc *a, Byte a0, Byte b, Byte c, Byte d);

/* net.CIDRMask: ones 1 bits and then 0 bits, bits in all, which has to be 32
 * or 128. The nil slice for anything else, or for ones out of range. */
BURROW_OWNS(ret) NetIPMask net_cidr_mask(Alloc *a, Int ones, Int bits);

/* Go's well-known addresses, in their 16 byte forms. Go has them as
 * variables. Here they are constant, and pointing at bytes you cannot write
 * to, so copy one before you change it. */
extern const NetIP net_ipv4_bcast;                  /* 255.255.255.255 */
extern const NetIP net_ipv4_allsys;                 /* 224.0.0.1 */
extern const NetIP net_ipv4_allrouter;              /* 224.0.0.2 */
extern const NetIP net_ipv4_zero;                   /* 0.0.0.0 */
extern const NetIP net_ipv6_zero;                   /* :: */
extern const NetIP net_ipv6_unspecified;            /* :: */
extern const NetIP net_ipv6_loopback;               /* ::1 */
extern const NetIP net_ipv6_interfacelocalallnodes; /* ff01::1 */
extern const NetIP net_ipv6_linklocalallnodes;      /* ff02::1 */
extern const NetIP net_ipv6_linklocalallrouters;    /* ff02::2 */

/* IP.IsUnspecified: 0.0.0.0 or ::. */
bool net_ip_is_unspecified(NetIP ip);

/* IP.IsLoopback: 127.0.0.0/8 or ::1. */
bool net_ip_is_loopback(NetIP ip);

/* IP.IsPrivate: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 or fc00::/7, the
 * private ranges of RFC 1918 and RFC 4193. Not a security property, and not
 * for access control. */
bool net_ip_is_private(NetIP ip);

/* IP.IsMulticast, IsInterfaceLocalMulticast, IsLinkLocalMulticast and
 * IsLinkLocalUnicast. */
bool net_ip_is_multicast(NetIP ip);
bool net_ip_is_interface_local_multicast(NetIP ip);
bool net_ip_is_link_local_multicast(NetIP ip);
bool net_ip_is_link_local_unicast(NetIP ip);

/* IP.IsGlobalUnicast: a 4 or 16 byte address that is not the broadcast
 * address, unspecified, loopback, multicast or link-local unicast. Private
 * addresses count as global unicast. */
bool net_ip_is_global_unicast(NetIP ip);

/* IP.To4: ip as 4 bytes, or the nil slice when it is not an IPv4 address. A
 * 16 byte ip gives a view of its last 4 bytes. */
BURROW_BORROWS(ret, ip) NetIP net_ip_to4(NetIP ip);

/* IP.To16: ip as 16 bytes, or the nil slice when it is neither length. A 4
 * byte ip is copied into a, and a 16 byte one comes back as it is. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, ip) NetIP net_ip_to16(NetIP ip, Alloc *a);

/* IP.DefaultMask: the class A, B or C mask for an IPv4 address, or the nil
 * slice for anything else. The result is static, as Go's are shared. */
BURROW_STATIC(ret) NetIPMask net_ip_default_mask(NetIP ip);

/* IP.Mask: ip with mask applied, in a. A 16 byte mask for an IPv4 address,
 * or a 4 byte mask for an IPv4-mapped one, is fitted to the address the way
 * Go does it. The nil slice when the two lengths still differ. */
BURROW_OWNS(ret) NetIP net_ip_mask(NetIP ip, Alloc *a, NetIPMask mask);

/* IP.String: "<nil>" for an empty ip, dotted decimal for IPv4 and
 * IPv4-mapped addresses, RFC 5952 for IPv6, and "?" and the bytes in hex for
 * any other length. */
BURROW_OWNS(ret) Str net_ip_string(NetIP ip, Alloc *a);

/* IP.AppendText: b with the text of ip on the end, as String gives it, but
 * nothing at all for an empty ip. A length other than 4 or 16 is a
 * NetAddrError, "address <hex>: invalid IP address", and b comes back as it
 * was. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice net_ip_append_text(NetIP ip, Alloc *a,
                                                                 Slice b, Error *err);

/* IP.MarshalText: the same text in a new slice, empty for an empty ip, and
 * the nil slice on an error. */
BURROW_OWNS(ret) Slice net_ip_marshal_text(NetIP ip, Alloc *a, Error *err);

/* IP.UnmarshalText: *ip set from text in the forms net_parse_ip takes, in a.
 * Empty text sets *ip to the nil slice. Text that is not an address is a
 * NetParseError and leaves *ip alone. */
BURROW_BORROWS(ret) BURROW_OWNS(ip) Error net_ip_unmarshal_text(NetIP *ip, Alloc *a,
                                                                Slice text);

/* IP.Equal: whether ip and x are the same address. An IPv4 address and its
 * IPv4-mapped IPv6 form are the same. */
bool net_ip_equal(NetIP ip, NetIP x);

/* net.ParseIP: s as an address in a, dotted decimal, IPv6 or IPv4-mapped
 * IPv6, always in 16 bytes. The nil slice when s is not one, which includes
 * an IPv6 address with a zone and an IPv4 field with a leading zero. */
BURROW_OWNS(ret) NetIP net_parse_ip(Alloc *a, Str s);

/* net.ParseCIDR: s in CIDR notation, such as "192.0.2.1/24". The result is
 * the address as written, and *net the network it is in, 192.0.2.0/24 in that
 * case, both in a. net may be NULL. Anything else is a NetParseError for
 * "CIDR address" and gives the nil slice and a NULL *net. */
BURROW_OWNS(ret) BURROW_OWNS(net) NetIP net_parse_cidr(Alloc *a, Str s, NetIPNet **net,
                                                       Error *err);

/* ------------------------------------------------------------------- IPMask */

/* IPMask.Size: the number of leading ones, and in *bits the number of bits.
 * bits may be NULL. Both are 0 for a mask that is not ones followed by
 * zeros. */
Int net_ip_mask_size(NetIPMask m, Int *bits);

/* IPMask.String: the bytes in hex, or "<nil>" for an empty mask. */
BURROW_OWNS(ret) Str net_ip_mask_string(NetIPMask m, Alloc *a);

/* -------------------------------------------------------------------- IPNet */

/* IPNet.Contains: whether ip is in n. */
bool net_ip_net_contains(const NetIPNet *n, NetIP ip);

/* IPNet.Network: "ip+net". */
BURROW_STATIC(ret) Str net_ip_net_network(const NetIPNet *n);

/* IPNet.String: CIDR notation such as "192.0.2.0/24", or the address, a slash
 * and the mask in hex when the mask is not ones followed by zeros. "<nil>"
 * for a NULL n or one whose address and mask do not fit together. */
BURROW_OWNS(ret) Str net_ip_net_string(const NetIPNet *n, Alloc *a);

/* -------------------------------------------------------------- host:port */

/* net.SplitHostPort: "host:port", "host%zone:port", "[host]:port" or
 * "[host%zone]:port" split into the host, with any brackets taken off, and
 * *port, which may be NULL. Both point into hostport. A malformed one is a
 * NetAddrError, such as "address ::1: too many colons in address", and gives
 * two empty strings. */
BURROW_BORROWS(ret, hostport) BURROW_BORROWS(port, hostport) Str
net_split_host_port(Str hostport, Str *port, Error *err);

/* net.JoinHostPort: host and port with a colon between, and brackets around
 * a host with a colon in it, which is taken to be an IPv6 literal. */
BURROW_OWNS(ret) Str net_join_host_port(Alloc *a, Str host, Str port);

/* ------------------------------------------------------------- interfaces
 *
 * net.Addr, net.Conn and net.Listener, in the shape io's interfaces have: a
 * vtable whose first word is the concrete type, and a value that is the vtable
 * and a data pointer, with a zeroed value being nil. burrow/iface.h has the
 * rules and burrow/io.h has the worked example. */

/* net.Addr, the address of an end of a connection. network is the name of the
 * network, such as "tcp" or "udp", and is a constant. string is the address
 * the way people write it, such as "192.0.2.1:25" or "[2001:db8::1]:80", made
 * in a. */
typedef struct NetAddrVT {
    const Type *self_type;
    Str (*network)(void *self);
    Str (*string)(void *self, Alloc *a);
} NetAddrVT;

typedef struct NetAddr {
    const NetAddrVT *vt;
    void *data;
} NetAddr;

/* net.Conn, a stream connection, which any number of goroutines may use at
 * once.
 *
 * Read, Write and Close are io's, with io's contracts, and are in the three
 * embedded vtables so that net_conn_as_io_reader and the others below are the
 * address of a member. local_addr and remote_addr give addresses that belong
 * to the connection and live as long as it does.
 *
 * A deadline is a Time after which a read or a write fails with an error that
 * wraps os_err_deadline_exceeded and that net_error_timeout says is a timeout,
 * instead of blocking. It covers calls already blocked when it is set as well
 * as later ones, and the zero Time means no deadline. set_deadline sets both,
 * set_read_deadline and set_write_deadline one each. A deadline is a point in
 * time and not an idle timeout, so a program that wants one of those moves
 * the deadline forward after every successful call. */
typedef struct NetConnVT {
    IoReaderVT reader;
    IoWriterVT writer;
    IoCloserVT closer;
    NetAddr (*local_addr)(void *self);
    NetAddr (*remote_addr)(void *self);
    Error (*set_deadline)(void *self, Time t);
    Error (*set_read_deadline)(void *self, Time t);
    Error (*set_write_deadline)(void *self, Time t);
} NetConnVT;

typedef struct NetConn {
    const NetConnVT *vt;
    void *data;
} NetConn;

/* net.Listener, which accepts stream connections. accept waits for the next
 * one, and a Close makes any accept that is waiting fail. addr is the address
 * it listens on and belongs to the listener. Close is first, as an io.Closer,
 * so that net_listener_as_io_closer is the address of a member. */
typedef struct NetListenerVT {
    IoCloserVT closer;
    NetConn (*accept)(void *self, Error *err);
    NetAddr (*addr)(void *self);
} NetListenerVT;

typedef struct NetListener {
    const NetListenerVT *vt;
    void *data;
} NetListener;

/* Go's implicit conversions from a Conn or a Listener to the io interfaces
 * they satisfy. A nil value in gives a nil value out. */
IoReader net_conn_as_io_reader(NetConn c);
IoWriter net_conn_as_io_writer(NetConn c);
IoCloser net_conn_as_io_closer(NetConn c);
IoCloser net_listener_as_io_closer(NetListener l);

/* net.PacketConn, a packet-oriented connection, which any number of
 * goroutines may use at once.
 *
 * read_from reads one packet into p and says who sent it, with the address
 * made in a. A packet longer than p is cut to fit, and the rest of it is
 * lost. write_to sends p as one packet to addr. Close is first, as an
 * io.Closer, so that net_packet_conn_as_io_closer is the address of a member.
 * local_addr and the deadlines are what a NetConn's are. */
typedef struct NetPacketConnVT {
    IoCloserVT closer;
    Int (*read_from)(void *self, Slice p, Alloc *a, NetAddr *addr, Error *err);
    Int (*write_to)(void *self, Slice p, NetAddr addr, Error *err);
    NetAddr (*local_addr)(void *self);
    Error (*set_deadline)(void *self, Time t);
    Error (*set_read_deadline)(void *self, Time t);
    Error (*set_write_deadline)(void *self, Time t);
} NetPacketConnVT;

typedef struct NetPacketConn {
    const NetPacketConnVT *vt;
    void *data;
} NetPacketConn;

IoCloser net_packet_conn_as_io_closer(NetPacketConn c);

/* --------------------------------------------------------------- net.Error */

/* net.Error, an error that can say whether it was a timeout.
 *
 * In Go it is an interface, error with Timeout and Temporary methods, and a
 * program asks for it with a type assertion. Here it is an Error whose type
 * lists both methods, and the functions below ask the type. Every error this
 * package makes is one, and so are os_err_deadline_exceeded and an Errno from
 * syscall_errno_as_error. */
typedef Error NetError;

/* Whether err is a net.Error at all, which is Go's err.(net.Error) with the
 * comma ok. Only err itself is asked, and not what it wraps, as in Go. */
bool net_is_error(Error err);

/* Error.Timeout and Error.Temporary, and false for an error that is not a
 * net.Error. Temporary is deprecated in Go, because most temporary errors are
 * timeouts and the rest are surprising, and nothing should use it. */
bool net_error_timeout(NetError err);
bool net_error_temporary(NetError err);

/* net.ErrClosed, "use of closed network connection", for a call on a
 * connection that has been closed. Test for it with errors_is. It is a
 * net.Error whose Timeout and Temporary say false. */
extern const Error net_err_closed;

/* net.OpError, what most of this package's calls fail with: the operation,
 * such as "read" or "write", the network, such as "tcp" or "pipe", the two
 * addresses, either of which may be nil, and what went wrong. Go's Error
 * method gives op, then the network, then source->addr or whichever of them
 * is there, then ": " and the text of err, such as
 * "read tcp 192.0.2.1:5000->192.0.2.2:80: i/o timeout". */
typedef struct NetOpError {
    Str op;
    Str net;
    NetAddr source;
    NetAddr addr;
    Error err;
} NetOpError;

extern const Type *const TYPE_NET_OP_ERROR;

/* The text, built in a, and "<nil>" for a NULL e. */
BURROW_OWNS(ret) Str net_op_error_error(const NetOpError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error net_op_error_unwrap(const NetOpError *e);

/* Whether err, or the error inside it when it is an OsSyscallError, has a
 * Timeout method that says true. */
bool net_op_error_timeout(const NetOpError *e);

/* The same with Temporary, and true as well for ECONNRESET and ECONNABORTED
 * from an accept, which Go counts as temporary because the next accept is
 * likely to work. */
bool net_op_error_temporary(const NetOpError *e);

/* The Error for e, which errors_as with TYPE_NET_OP_ERROR gives back and
 * which unwraps to e->err. op, net and the text are copied into a, and so is
 * the text of the addresses. The addresses themselves are not copied, and
 * have to live as long as the error does if anything is going to look at
 * them. */
BURROW_OWNS(ret) Error net_op_error_as_error(const NetOpError *e, Alloc *a);

/* ------------------------------------------------------------------- Pipe */

/* net.Pipe: two connected ends of a connection that lives in memory, with
 * what is written to one read from the other, in both directions.
 *
 *     NetConn c1, c2;
 *     net_pipe(a, &c1, &c2);
 *
 * There is no buffer. A Write waits for Reads on the other end to take all
 * of it, and each Read copies straight out of the writer's slice, so a
 * Write and a Read on the same end, with nothing reading the other, wait
 * forever. Closing an end makes its own calls give io_err_closed_pipe and a
 * Read on the other end give io_eof. The deadlines work, and a call that runs
 * past one fails with a NetOpError around os_err_deadline_exceeded. Both ends'
 * addresses are an Addr whose network and text are both "pipe".
 *
 * A deadline is a timer, so setting one has to be done from a goroutine, the
 * way context_with_deadline does.
 *
 * Go leaves the memory to the collector. Here it comes from a and goes back
 * with net_pipe_free, which takes either end and frees both. That has to wait
 * until no goroutine is in a call on either end and none will make one, which
 * usually means after both are closed and every goroutine using them has
 * been waited for. On a failed allocation both ends are nil. */
void net_pipe(Alloc *a, NetConn *c1, NetConn *c2);

/* Frees a pipe from net_pipe, given either of its ends. NULL data does
 * nothing. Stops any deadline timer still armed, and waits for one that is
 * already running. */
void net_pipe_free(NetConn c);

/* -------------------------------------------------------------------- TCP
 *
 * net.TCPAddr, net.TCPConn and net.TCPListener, on the runtime's network
 * poller, so a goroutine blocked in a read or an accept holds a stack and not
 * a thread:
 *
 *     NetTCPAddr any = {0};
 *     NetTCPListener *l = net_listen_tcp(a, BURROW_S("tcp"), &any, &err);
 *     NetTCPConn *c = net_tcp_listener_accept_tcp(l, &err);
 *
 * Every call that can wait, which is a dial, a read, a write and an accept,
 * has to be made from a goroutine. The rest can be made from anywhere.
 *
 * The errors are Go's, word for word: a NetOpError around an OsSyscallError
 * around the Errno, such as "dial tcp 127.0.0.1:1: connect: connection
 * refused", or around net_err_closed or os_err_deadline_exceeded. A read at
 * the end of the stream gives io_eof by itself, as Go's does.
 *
 * Go leaves the memory to the collector. Here a connection and a listener
 * come from the allocator they were made with, and a connection accepted from
 * a listener comes from the listener's, and each goes back with its _free,
 * which closes it first if it is still open. That has to wait until no
 * goroutine is in a call on it and none will make one, which usually means
 * after the goroutines using it have been waited for. The addresses a
 * connection gives belong to it and go with it.
 *
 * Windows reads a socket by handing the kernel the read and waiting for it to
 * finish, which is a poller of a different kind, and until that is here a
 * dial or a listen there fails with ENOSYS. wasip1 has no sockets to make. */

/* net.UnknownNetworkError, the network name a dial or a listen did not know,
 * such as "tcp5". Its text is "unknown network " and the name. It is a
 * net.Error that is neither a timeout nor temporary, and errors_as with
 * TYPE_NET_UNKNOWN_NETWORK_ERROR gives back the name. */
typedef Str NetUnknownNetworkError;

extern const Type *const TYPE_NET_UNKNOWN_NETWORK_ERROR;

/* The Error for the name network, which is copied into a. */
BURROW_OWNS(ret) Error net_unknown_network_error(Alloc *a, Str network);

/* net.TCPAddr: an IP address, a port and, for an IPv6 address that needs one,
 * the zone, which is an interface name or its index in decimal. An empty ip
 * is the unspecified address, which is what a listener that takes
 * connections to every address of the machine binds to, and port 0 asks the
 * system for a free one. A NULL NetTCPAddr * is Go's nil *TCPAddr. */
typedef struct NetTCPAddr {
    NetIP ip;
    Int port;
    Str zone;
} NetTCPAddr;

extern const Type *const TYPE_NET_TCP_ADDR;

/* TCPAddr.Network, which is "tcp". */
BURROW_STATIC(ret) Str net_tcp_addr_network(const NetTCPAddr *a);

/* TCPAddr.String: the host and the port the way net_join_host_port puts them
 * together, such as "192.0.2.1:80" or "[fe80::1%eth0]:443", with an empty
 * host for an empty ip, and "<nil>" for a NULL a. */
BURROW_OWNS(ret) Str net_tcp_addr_string(const NetTCPAddr *a, Alloc *al);

/* a as a NetAddr, whose data is a, and the nil NetAddr for a NULL a. A
 * NetAddr whose vt->self_type is TYPE_NET_TCP_ADDR has a NetTCPAddr behind
 * its data, which is Go's addr.(*TCPAddr). */
NetAddr net_tcp_addr_as_addr(const NetTCPAddr *a);

/* TCPAddr.AddrPort: the zero NetipAddrPort for a NULL a, and an invalid
 * address in it for an ip that is not 4 or 16 bytes long. */
NetipAddrPort net_tcp_addr_addr_port(const NetTCPAddr *a);

/* TCPAddrFromAddrPort, made in a, with its ip and zone in the same block. */
BURROW_OWNS(ret) NetTCPAddr *net_tcp_addr_from_addr_port(Alloc *a, NetipAddrPort addr);

/* ResolveTCPAddr: the address of a TCP end point, made in a. network is
 * "tcp", "tcp4" or "tcp6", and an empty one is "tcp". address is a host and
 * a port, as net_join_host_port gives, where the host may be a name to look
 * up or a literal IP address and the port a number or a service name. A
 * name with more than one address gives the first IPv4 one, or the first
 * IPv6 one when address has a "[" in it, and an empty host is the
 * unspecified address. */
BURROW_OWNS(ret) NetTCPAddr *net_resolve_tcp_addr(Alloc *a, Str network, Str address,
                                                  Error *err);

/* Gives back a NetTCPAddr that this package made in a. NULL does nothing. */
void net_tcp_addr_free(Alloc *a, NetTCPAddr *addr);

/* net.KeepAliveConfig. idle and interval below zero leave the system's
 * setting alone and zero means fifteen seconds, and the same goes for count,
 * where zero means nine. */
typedef struct NetKeepAliveConfig {
    bool enable;
    Duration idle;
    Duration interval;
    Int count;
} NetKeepAliveConfig;

typedef struct NetTCPConn NetTCPConn;
typedef struct NetTCPListener NetTCPListener;

/* net.DialTCP: a connection to raddr, from laddr if it is not NULL, and from
 * an address and a port the system picks if it is. network is "tcp", "tcp4"
 * or "tcp6". Like Go's, a new connection has Nagle's algorithm off and TCP
 * keep-alives on, every fifteen seconds. NULL on an error, which is a
 * NetOpError with the op "dial". */
BURROW_OWNS(ret) NetTCPConn *net_dial_tcp(Alloc *a, Str network,
                                          const NetTCPAddr *laddr,
                                          const NetTCPAddr *raddr, Error *err);

/* net.ListenTCP: a listener on laddr, or on every address of the machine and
 * a port the system picks when laddr is NULL. A listener for "tcp" on the
 * unspecified address takes IPv4 and IPv6 both, where the system allows it.
 * net_tcp_listener_addr says which port it got. NULL on an error, which is a
 * NetOpError with the op "listen". */
BURROW_OWNS(ret) NetTCPListener *net_listen_tcp(Alloc *a, Str network,
                                                const NetTCPAddr *laddr, Error *err);

/* conn.Read and conn.Write, which are io's. A NULL c is EINVAL. */
Int net_tcp_conn_read(NetTCPConn *c, Slice p, Error *err);
Int net_tcp_conn_write(NetTCPConn *c, Slice p, Error *err);

/* conn.Close, which wakes every call blocked on c with net_err_closed, and
 * TCPConn.CloseRead and CloseWrite, which shut down one direction. After a
 * CloseWrite the other end reads io_eof, and after a CloseRead this end
 * does. */
BURROW_STATIC(ret) Error net_tcp_conn_close(NetTCPConn *c);
BURROW_STATIC(ret) Error net_tcp_conn_close_read(NetTCPConn *c);
BURROW_STATIC(ret) Error net_tcp_conn_close_write(NetTCPConn *c);

/* conn.LocalAddr and RemoteAddr, which are NetTCPAddrs that belong to c. */
NetAddr net_tcp_conn_local_addr(NetTCPConn *c);
NetAddr net_tcp_conn_remote_addr(NetTCPConn *c);

/* The deadlines, as NetConn's set_deadline and the others describe them. */
BURROW_STATIC(ret) Error net_tcp_conn_set_deadline(NetTCPConn *c, Time t);
BURROW_STATIC(ret) Error net_tcp_conn_set_read_deadline(NetTCPConn *c, Time t);
BURROW_STATIC(ret) Error net_tcp_conn_set_write_deadline(NetTCPConn *c, Time t);

/* The size of the system's receive and send buffers. */
BURROW_STATIC(ret) Error net_tcp_conn_set_read_buffer(NetTCPConn *c, Int bytes);
BURROW_STATIC(ret) Error net_tcp_conn_set_write_buffer(NetTCPConn *c, Int bytes);

/* TCPConn.SetLinger: what a Close does with data not yet sent. Below zero,
 * which is the default, the system sends it in the background. Zero throws
 * it away. Above zero it is sent in the background too, but on some systems,
 * Linux among them, the Close may wait until it has been sent or thrown away,
 * and on some, what is left after sec seconds is thrown away. */
BURROW_STATIC(ret) Error net_tcp_conn_set_linger(NetTCPConn *c, Int sec);

/* TCPConn.SetNoDelay: whether a small write goes out at once, which is the
 * default, or waits to be sent together with the next (Nagle's algorithm). */
BURROW_STATIC(ret) Error net_tcp_conn_set_no_delay(NetTCPConn *c, bool no_delay);

/* TCPConn.SetKeepAlive, SetKeepAlivePeriod, which sets the idle time and
 * rounds it up to a second, and SetKeepAliveConfig. On a system that cannot
 * set the idle time, the interval or the count, such as OpenBSD, asking for
 * one fails with an Errno saying the option is not supported. */
BURROW_STATIC(ret) Error net_tcp_conn_set_keep_alive(NetTCPConn *c, bool keepalive);
BURROW_STATIC(ret) Error net_tcp_conn_set_keep_alive_period(NetTCPConn *c, Duration d);
BURROW_STATIC(ret) Error net_tcp_conn_set_keep_alive_config(NetTCPConn *c,
                                                            NetKeepAliveConfig config);

/* TCPConn.SyscallConn and TCPListener.SyscallConn: the socket under c or l,
 * which stays the caller's to use only until c or l is freed. A NULL c or l
 * is EINVAL. The raw conn of a listener can only be controlled, and its read
 * and write give EINVAL, as Go's does. */
SyscallRawConn net_tcp_conn_syscall_conn(NetTCPConn *c, Error *err);
SyscallRawConn net_tcp_listener_syscall_conn(NetTCPListener *l, Error *err);

/* c as a NetConn, and back: Go's conversion to net.Conn and its
 * conn.(*TCPConn), which gives NULL for a NetConn that is not a TCPConn. */
NetConn net_tcp_conn_as_conn(NetTCPConn *c);
BURROW_BORROWS(ret) NetTCPConn *net_conn_as_tcp_conn(NetConn c);

/* Closes c if it is still open and gives its memory back. NULL does
 * nothing. */
void net_tcp_conn_free(NetTCPConn *c);

/* TCPListener.AcceptTCP: waits for the next connection, which comes from
 * the listener's allocator and has Nagle's algorithm off and keep-alives on,
 * as a dialed one does. NULL on an error, which is a NetOpError with the op
 * "accept". */
BURROW_OWNS(ret) NetTCPConn *net_tcp_listener_accept_tcp(NetTCPListener *l, Error *err);

/* TCPListener.Close, which makes an accept that is waiting fail with
 * net_err_closed, and Addr, a NetTCPAddr that belongs to l. */
BURROW_STATIC(ret) Error net_tcp_listener_close(NetTCPListener *l);
NetAddr net_tcp_listener_addr(NetTCPListener *l);

/* TCPListener.SetDeadline, for the accepts. */
BURROW_STATIC(ret) Error net_tcp_listener_set_deadline(NetTCPListener *l, Time t);

/* l as a NetListener, whose accept gives a NetConn that
 * net_conn_as_tcp_conn turns back into the NetTCPConn to free. */
NetListener net_tcp_listener_as_listener(NetTCPListener *l);

/* l.(*TCPListener): the listener under l, or NULL when it is not one. */
BURROW_BORROWS(ret) NetTCPListener *net_listener_as_tcp_listener(NetListener l);

/* Closes l if it is still open and gives its memory back. NULL does
 * nothing. The connections it accepted are their own and are not freed. */
void net_tcp_listener_free(NetTCPListener *l);

/* -------------------------------------------------------------------- UDP
 *
 * net.UDPAddr and net.UDPConn, on the same poller as TCP:
 *
 *     NetUDPAddr any = {0};
 *     NetUDPConn *c = net_listen_udp(a, BURROW_S("udp"), &any, &err);
 *     NetipAddrPort from;
 *     Int n = net_udp_conn_read_from_udp_addr_port(c, buf, &from, &err);
 *
 * A connection from net_listen_udp is bound and not connected, and sends
 * with the write_to calls, each to the address it names. One from
 * net_dial_udp is connected, sends with write, and only hears from the
 * address it dialed. A datagram is read whole, and what does not fit in the
 * buffer is gone.
 *
 * The errors are Go's, and so are the rules for the memory, which are the
 * ones TCP has. A NetUDPAddr from read_from_udp or from_addr_port is the
 * caller's, and goes back with net_udp_addr_free.
 *
 * As with TCP, Windows gives ENOSYS until its poller is here and wasip1 has
 * no sockets. ListenMulticastUDP, ReadMsgUDP and WriteMsgUDP are still to
 * come. */

/* net.UDPAddr, laid out as NetTCPAddr is, and read the same way. */
typedef struct NetUDPAddr {
    NetIP ip;
    Int port;
    Str zone;
} NetUDPAddr;

extern const Type *const TYPE_NET_UDP_ADDR;

/* UDPAddr.Network, which is "udp". */
BURROW_STATIC(ret) Str net_udp_addr_network(const NetUDPAddr *a);

/* UDPAddr.String, "<nil>" for a NULL a, such as "[::1%eth0]:53". */
BURROW_OWNS(ret) Str net_udp_addr_string(const NetUDPAddr *a, Alloc *al);

/* a as a NetAddr, which points at a, and nil for a NULL a. */
NetAddr net_udp_addr_as_addr(const NetUDPAddr *a);

/* UDPAddr.AddrPort: the zero NetipAddrPort for a NULL a, and an invalid
 * address in it for an ip that is not 4 or 16 bytes long. */
NetipAddrPort net_udp_addr_addr_port(const NetUDPAddr *a);

/* UDPAddrFromAddrPort, made in a, with its ip and zone in the same block. */
BURROW_OWNS(ret) NetUDPAddr *net_udp_addr_from_addr_port(Alloc *a, NetipAddrPort addr);

/* ResolveUDPAddr, as net_resolve_tcp_addr is for "udp", "udp4" and "udp6". */
BURROW_OWNS(ret) NetUDPAddr *net_resolve_udp_addr(Alloc *a, Str network, Str address,
                                                  Error *err);

/* Gives back a NetUDPAddr that this package made in a. NULL does nothing. */
void net_udp_addr_free(Alloc *a, NetUDPAddr *addr);

/* net.ErrWriteToConnected, for a write_to on a connection that was dialed. */
extern const Error net_err_write_to_connected;

typedef struct NetUDPConn NetUDPConn;

/* DialUDP: a socket connected to raddr, from laddr when that is not NULL.
 * network is "udp", "udp4" or "udp6". Nothing is sent, so a dial to a port
 * where nothing listens works, and the first read or write after it may fail
 * with ECONNREFUSED. */
BURROW_OWNS(ret) NetUDPConn *net_dial_udp(Alloc *a, Str network,
                                          const NetUDPAddr *laddr,
                                          const NetUDPAddr *raddr, Error *err);

/* ListenUDP: a socket bound to laddr, every address of the machine for a
 * NULL laddr or an empty ip, and a free port for port 0. A multicast laddr
 * binds the unspecified address with that port and lets other sockets bind
 * it too, but joins no group. */
BURROW_OWNS(ret) NetUDPConn *net_listen_udp(Alloc *a, Str network,
                                            const NetUDPAddr *laddr, Error *err);

/* conn.Read and Write, for a dialed connection. */
Int net_udp_conn_read(NetUDPConn *c, Slice p, Error *err);
Int net_udp_conn_write(NetUDPConn *c, Slice p, Error *err);

/* ReadFromUDP: a datagram, and in *addr who sent it, made in a. *addr is
 * NULL on an error. addr may be NULL to not ask. */
Int net_udp_conn_read_from_udp(NetUDPConn *c, Slice p, Alloc *a, NetUDPAddr **addr,
                               Error *err);

/* ReadFrom, which is ReadFromUDP with the sender as a NetAddr, nil on an
 * error. */
Int net_udp_conn_read_from(NetUDPConn *c, Slice p, Alloc *a, NetAddr *addr, Error *err);

/* ReadFromUDPAddrPort, which needs no memory for the sender. */
Int net_udp_conn_read_from_udp_addr_port(NetUDPConn *c, Slice p, NetipAddrPort *addr,
                                         Error *err);

/* WriteToUDP, WriteToUDPAddrPort and WriteTo: p as one datagram to addr.
 * WriteTo takes any NetAddr, and fails with EINVAL for one that is not a
 * NetUDPAddr. */
Int net_udp_conn_write_to_udp(NetUDPConn *c, Slice p, const NetUDPAddr *addr,
                              Error *err);
Int net_udp_conn_write_to_udp_addr_port(NetUDPConn *c, Slice p, NetipAddrPort addr,
                                        Error *err);
Int net_udp_conn_write_to(NetUDPConn *c, Slice p, NetAddr addr, Error *err);

/* conn.Close, LocalAddr, RemoteAddr and the setters, as for TCP. A
 * connection from net_listen_udp has no remote address. */
BURROW_STATIC(ret) Error net_udp_conn_close(NetUDPConn *c);
NetAddr net_udp_conn_local_addr(NetUDPConn *c);
NetAddr net_udp_conn_remote_addr(NetUDPConn *c);
BURROW_STATIC(ret) Error net_udp_conn_set_deadline(NetUDPConn *c, Time t);
BURROW_STATIC(ret) Error net_udp_conn_set_read_deadline(NetUDPConn *c, Time t);
BURROW_STATIC(ret) Error net_udp_conn_set_write_deadline(NetUDPConn *c, Time t);
BURROW_STATIC(ret) Error net_udp_conn_set_read_buffer(NetUDPConn *c, Int bytes);
BURROW_STATIC(ret) Error net_udp_conn_set_write_buffer(NetUDPConn *c, Int bytes);

/* UDPConn.SyscallConn, as net_tcp_conn_syscall_conn is. */
SyscallRawConn net_udp_conn_syscall_conn(NetUDPConn *c, Error *err);

/* c as a NetConn or a NetPacketConn, and back. */
NetConn net_udp_conn_as_conn(NetUDPConn *c);
BURROW_BORROWS(ret) NetUDPConn *net_conn_as_udp_conn(NetConn c);
NetPacketConn net_udp_conn_as_packet_conn(NetUDPConn *c);
BURROW_BORROWS(ret) NetUDPConn *net_packet_conn_as_udp_conn(NetPacketConn c);

/* Closes c if it is open and gives its memory back. NULL does nothing. */
void net_udp_conn_free(NetUDPConn *c);

/* ------------------------------------------------------------------- Unix
 *
 * net.UnixAddr, net.UnixConn and net.UnixListener, which are sockets named
 * by a path on this machine, on the same poller as TCP and UDP:
 *
 *     NetUnixAddr addr = {BURROW_S("/tmp/app.sock"), BURROW_S("unix")};
 *     NetUnixListener *l = net_listen_unix(a, BURROW_S("unix"), &addr, &err);
 *     NetUnixConn *c = net_dial_unix(a, BURROW_S("unix"), NULL, &addr, &err);
 *
 * The network is "unix" for a stream, "unixgram" for datagrams, and
 * "unixpacket" for datagrams over a connection, which Linux and some of the
 * BSDs have and macOS does not. Listening makes a file at the path, and
 * closing the listener removes it again. On Linux a name that starts with
 * "@" is in the abstract namespace instead, with no file behind it, and a
 * socket that was never bound is named "@" there, where macOS and the BSDs
 * call it "".
 *
 * The errors are Go's, and so are the rules for the memory, which are the
 * ones TCP has. A NetUnixAddr from net_resolve_unix_addr or read_from_unix
 * is the caller's, and goes back with net_unix_addr_free.
 *
 * As with TCP, Windows gives ENOSYS until its poller is here and wasip1 has
 * no sockets. ReadMsgUnix, WriteMsgUnix and the File methods are still to
 * come. */

/* net.UnixAddr: the path, or the abstract name, and the network it is for,
 * which is "unix", "unixgram" or "unixpacket". */
typedef struct NetUnixAddr {
    Str name;
    Str net;
} NetUnixAddr;

extern const Type *const TYPE_NET_UNIX_ADDR;

/* ResolveUnixAddr: address for network, made in a with both copied. A
 * network other than the three is a NetUnknownNetworkError. */
BURROW_OWNS(ret) NetUnixAddr *net_resolve_unix_addr(Alloc *a, Str network, Str address,
                                                    Error *err);

/* UnixAddr.Network, which is a->net, and empty for a NULL a. */
BURROW_BORROWS(ret) Str net_unix_addr_network(const NetUnixAddr *a);

/* UnixAddr.String, which is the name, made in al, and "<nil>" for a NULL a. */
BURROW_OWNS(ret) Str net_unix_addr_string(const NetUnixAddr *a, Alloc *al);

/* a as a NetAddr, which points at a, and nil for a NULL a. */
NetAddr net_unix_addr_as_addr(const NetUnixAddr *a);

/* Gives back a NetUnixAddr that this package made in a. NULL does nothing. */
void net_unix_addr_free(Alloc *a, NetUnixAddr *addr);

typedef struct NetUnixConn NetUnixConn;
typedef struct NetUnixListener NetUnixListener;

/* DialUnix: a socket connected to raddr, bound to laddr first when that is
 * not NULL. For "unixgram" raddr may be NULL if laddr is not, which gives a
 * socket bound to laddr and not connected. NULL on an error, which is a
 * NetOpError with the op "dial". */
BURROW_OWNS(ret) NetUnixConn *net_dial_unix(Alloc *a, Str network,
                                            const NetUnixAddr *laddr,
                                            const NetUnixAddr *raddr, Error *err);

/* ListenUnix: a listener on laddr, for "unix" or "unixpacket". laddr may not
 * be NULL. An empty name on Linux binds to a fresh abstract name, which
 * net_unix_listener_addr gives. NULL on an error, which is a NetOpError with
 * the op "listen". */
BURROW_OWNS(ret) NetUnixListener *net_listen_unix(Alloc *a, Str network,
                                                  const NetUnixAddr *laddr, Error *err);

/* ListenUnixgram: a datagram socket bound to laddr, for "unixgram" only,
 * which sends with the write_to calls. */
BURROW_OWNS(ret) NetUnixConn *net_listen_unixgram(Alloc *a, Str network,
                                                  const NetUnixAddr *laddr, Error *err);

/* conn.Read and conn.Write. A NULL c is EINVAL. */
Int net_unix_conn_read(NetUnixConn *c, Slice p, Error *err);
Int net_unix_conn_write(NetUnixConn *c, Slice p, Error *err);

/* ReadFromUnix: what was read, and in *addr who sent it, made in a. *addr is
 * NULL when the sender has no name, which is always so on a stream. */
Int net_unix_conn_read_from_unix(NetUnixConn *c, Slice p, Alloc *a, NetUnixAddr **addr,
                                 Error *err);

/* ReadFrom, which is ReadFromUnix with the sender as a NetAddr, nil when it
 * has no name. */
Int net_unix_conn_read_from(NetUnixConn *c, Slice p, Alloc *a, NetAddr *addr,
                            Error *err);

/* WriteToUnix and WriteTo: p to addr, on a socket that is not connected.
 * addr's network has to be the socket's, and WriteTo takes only a
 * NetUnixAddr. */
Int net_unix_conn_write_to_unix(NetUnixConn *c, Slice p, const NetUnixAddr *addr,
                                Error *err);
Int net_unix_conn_write_to(NetUnixConn *c, Slice p, NetAddr addr, Error *err);

/* Close, CloseRead and CloseWrite, as TCP has them, and the rest of
 * NetConn. */
BURROW_STATIC(ret) Error net_unix_conn_close(NetUnixConn *c);
BURROW_STATIC(ret) Error net_unix_conn_close_read(NetUnixConn *c);
BURROW_STATIC(ret) Error net_unix_conn_close_write(NetUnixConn *c);
NetAddr net_unix_conn_local_addr(NetUnixConn *c);
NetAddr net_unix_conn_remote_addr(NetUnixConn *c);
BURROW_STATIC(ret) Error net_unix_conn_set_deadline(NetUnixConn *c, Time t);
BURROW_STATIC(ret) Error net_unix_conn_set_read_deadline(NetUnixConn *c, Time t);
BURROW_STATIC(ret) Error net_unix_conn_set_write_deadline(NetUnixConn *c, Time t);
BURROW_STATIC(ret) Error net_unix_conn_set_read_buffer(NetUnixConn *c, Int bytes);
BURROW_STATIC(ret) Error net_unix_conn_set_write_buffer(NetUnixConn *c, Int bytes);

/* UnixConn.SyscallConn and UnixListener.SyscallConn, as the TCP ones are. */
SyscallRawConn net_unix_conn_syscall_conn(NetUnixConn *c, Error *err);
SyscallRawConn net_unix_listener_syscall_conn(NetUnixListener *l, Error *err);

/* c as a NetConn or a NetPacketConn, and back. */
NetConn net_unix_conn_as_conn(NetUnixConn *c);
BURROW_BORROWS(ret) NetUnixConn *net_conn_as_unix_conn(NetConn c);
NetPacketConn net_unix_conn_as_packet_conn(NetUnixConn *c);
BURROW_BORROWS(ret) NetUnixConn *net_packet_conn_as_unix_conn(NetPacketConn c);

/* Closes c if it is open and gives its memory back. NULL does nothing. */
void net_unix_conn_free(NetUnixConn *c);

/* UnixListener.AcceptUnix: the next connection, made in the listener's
 * allocator. NULL on an error, which is a NetOpError with the op "accept". */
BURROW_OWNS(ret) NetUnixConn *net_unix_listener_accept_unix(NetUnixListener *l,
                                                            Error *err);

/* UnixListener.Close: removes the file the listener made, unless
 * net_unix_listener_set_unlink_on_close said not to, and then closes it. */
BURROW_STATIC(ret) Error net_unix_listener_close(NetUnixListener *l);

/* UnixListener.Addr, a NetUnixAddr that belongs to l, and SetDeadline. */
NetAddr net_unix_listener_addr(NetUnixListener *l);
BURROW_STATIC(ret) Error net_unix_listener_set_deadline(NetUnixListener *l, Time t);

/* UnixListener.SetUnlinkOnClose: whether closing l removes its file, which
 * it does unless this says otherwise. */
void net_unix_listener_set_unlink_on_close(NetUnixListener *l, bool unlink);

/* l as a NetListener. */
NetListener net_unix_listener_as_listener(NetUnixListener *l);

/* l.(*UnixListener): the listener under l, or NULL when it is not one. */
BURROW_BORROWS(ret) NetUnixListener *net_listener_as_unix_listener(NetListener l);

/* Closes l the way net_unix_listener_close does if it is still open, and
 * gives its memory back. NULL does nothing. */
void net_unix_listener_free(NetUnixListener *l);

/* ------------------------------------------------------------------ IPAddr
 *
 * net.IPAddr, an IP address with the zone it is in, which is what
 * net_resolver_lookup_ip_addr gives, and net.IPConn, the raw IP socket that
 * goes with it:
 *
 *     NetIPAddr any = {0};
 *     NetIPConn *c = net_listen_ip(a, BURROW_S("ip4:icmp"), &any, &err);
 *     NetIPAddr *from;
 *     Int n = net_ip_conn_read_from_ip(c, buf, a, &from, &err);
 *
 * The network is "ip", "ip4" or "ip6" with the protocol after a colon, by
 * number or by name, as in "ip4:1" or "ip6:ipv6-icmp". Raw sockets need
 * privileges on most systems, and without them the dial or listen fails
 * with EPERM or EACCES. read_from_ip on an IPv4 socket gives the payload
 * without the IPv4 header in front of it, as Go does, while read gives the
 * packet as the system hands it over. The rest, the errors and the memory,
 * is as UDP has it. ReadMsgIP and WriteMsgIP are still to come. */

typedef struct NetIPAddr {
    NetIP ip;
    Str zone;
} NetIPAddr;

extern const Type *const TYPE_NET_IP_ADDR;

/* IPAddr.Network, which is "ip". */
BURROW_STATIC(ret) Str net_ip_addr_network(const NetIPAddr *a);

/* IPAddr.String, "<nil>" for a NULL a, such as "fe80::1%eth0". */
BURROW_OWNS(ret) Str net_ip_addr_string(const NetIPAddr *a, Alloc *al);

/* a as a NetAddr, which points at a, and nil for a NULL a. */
NetAddr net_ip_addr_as_addr(const NetIPAddr *a);

/* ResolveIPAddr: the address of host, made in a. network is "ip", "ip4" or
 * "ip6", with a protocol after a colon allowed, as in "ip4:icmp", and an
 * empty one is "ip". A name with more than one address gives the first IPv4
 * one, or the first IPv6 one when host has a colon in it. */
BURROW_OWNS(ret) NetIPAddr *net_resolve_ip_addr(Alloc *a, Str network, Str host,
                                                Error *err);

/* Gives back a NetIPAddr that net_resolve_ip_addr made in a. NULL does
 * nothing. */
void net_ip_addr_free(Alloc *a, NetIPAddr *addr);

/* net.IPConn. */
typedef struct NetIPConn NetIPConn;

/* DialIP: a raw socket connected to raddr, made in a. laddr may be NULL. */
BURROW_OWNS(ret) NetIPConn *net_dial_ip(Alloc *a, Str network, const NetIPAddr *laddr,
                                        const NetIPAddr *raddr, Error *err);

/* ListenIP: a raw socket bound to laddr that is not connected, made in a. A
 * NULL laddr is the zero IPAddr, every address. */
BURROW_OWNS(ret) NetIPConn *net_listen_ip(Alloc *a, Str network, const NetIPAddr *laddr,
                                          Error *err);

/* IPConn.SyscallConn. */
SyscallRawConn net_ip_conn_syscall_conn(NetIPConn *c, Error *err);

/* Read and Write, as NetConn's are, one packet at a time. */
Int net_ip_conn_read(NetIPConn *c, Slice p, Error *err);
Int net_ip_conn_write(NetIPConn *c, Slice p, Error *err);

/* ReadFromIP and ReadFrom: one packet, and who sent it, made in a. */
Int net_ip_conn_read_from_ip(NetIPConn *c, Slice p, Alloc *a, NetIPAddr **addr,
                             Error *err);
Int net_ip_conn_read_from(NetIPConn *c, Slice p, Alloc *a, NetAddr *addr, Error *err);

/* WriteToIP and WriteTo: one packet to addr, which for write_to has to be a
 * NetIPAddr. A connection that was dialed gives net_err_write_to_connected. */
Int net_ip_conn_write_to_ip(NetIPConn *c, Slice p, const NetIPAddr *addr, Error *err);
Int net_ip_conn_write_to(NetIPConn *c, Slice p, NetAddr addr, Error *err);

/* Close, the addresses, the deadlines and the buffer sizes, as UDP has
 * them. */
BURROW_STATIC(ret) Error net_ip_conn_close(NetIPConn *c);
NetAddr net_ip_conn_local_addr(NetIPConn *c);
NetAddr net_ip_conn_remote_addr(NetIPConn *c);
BURROW_STATIC(ret) Error net_ip_conn_set_deadline(NetIPConn *c, Time t);
BURROW_STATIC(ret) Error net_ip_conn_set_read_deadline(NetIPConn *c, Time t);
BURROW_STATIC(ret) Error net_ip_conn_set_write_deadline(NetIPConn *c, Time t);
BURROW_STATIC(ret) Error net_ip_conn_set_read_buffer(NetIPConn *c, Int bytes);
BURROW_STATIC(ret) Error net_ip_conn_set_write_buffer(NetIPConn *c, Int bytes);

/* c as a NetConn or a NetPacketConn, and back, which is NULL for anything
 * that is not an IPConn. */
NetConn net_ip_conn_as_conn(NetIPConn *c);
BURROW_BORROWS(ret) NetIPConn *net_conn_as_ip_conn(NetConn c);
NetPacketConn net_ip_conn_as_packet_conn(NetIPConn *c);
BURROW_BORROWS(ret) NetIPConn *net_packet_conn_as_ip_conn(NetPacketConn c);

/* Closes c if it is open and gives it back. NULL does nothing. */
void net_ip_conn_free(NetIPConn *c);

/* -------------------------------------------------------------------- DNS */

/* net.DNSError, a lookup that failed: what went wrong, the name looked for
 * and the server that was asked, which may be empty. Go's Error method gives
 * "lookup " + name, then " on " + server when there is one, then ": " + err,
 * as in "lookup example.invalid on 192.0.2.53:53: no such host", and "<nil>"
 * for a NULL e.
 *
 * unwrap_err is what Unwrap gives, and nil for most of them. The resolver
 * sets it to context_canceled or context_deadline_exceeded when a lookup
 * stopped because its context did, so that errors_is finds those. */
typedef struct NetDNSError {
    Error unwrap_err;
    Str err;
    Str name;
    Str server;
    bool is_timeout;
    bool is_temporary;

    /* The name has no records of the type asked for, or does not exist at
     * all. */
    bool is_not_found;
} NetDNSError;

extern const Type *const TYPE_NET_DNS_ERROR;

/* The text, built in a. */
BURROW_OWNS(ret) Str net_dns_error_error(const NetDNSError *e, Alloc *a);

/* e->unwrap_err. */
BURROW_BORROWS(ret, e) Error net_dns_error_unwrap(const NetDNSError *e);

/* DNSError.Timeout, which is is_timeout, and Temporary, which is is_timeout
 * or is_temporary. Neither is always known, so a lookup that did time out may
 * still say false. */
bool net_dns_error_timeout(const NetDNSError *e);
bool net_dns_error_temporary(const NetDNSError *e);

/* The Error for e, which errors_as with TYPE_NET_DNS_ERROR gives back and
 * which unwraps to e->unwrap_err. The strings are copied into a. */
BURROW_OWNS(ret) Error net_dns_error_as_error(const NetDNSError *e, Alloc *a);

/* net.SRV, net.MX and net.NS, one record each of what LookupSRV, LookupMX
 * and LookupNS give. */
typedef struct NetSRV {
    Str target;
    uint16_t port;
    uint16_t priority;
    uint16_t weight;
} NetSRV;

typedef struct NetMX {
    Str host;
    uint16_t pref;
} NetMX;

typedef struct NetNS {
    Str host;
} NetNS;

/* Their descriptors, the element types of the slices the lookups give. */
extern const Type *const TYPE_NET_SRV;
extern const Type *const TYPE_NET_MX;
extern const Type *const TYPE_NET_NS;

/* --------------------------------------------------------------- Resolver
 *
 * net.Resolver, which looks up names, addresses, ports and records:
 *
 *     Error err;
 *     Slice addrs = net_lookup_host(a, BURROW_S("example.com"), &err);
 *     Slice mx = net_resolver_lookup_mx(NULL, a, ctx, BURROW_S("example.com"), &err);
 *
 * This is Go's own resolver, the one it calls the pure Go one. It reads
 * /etc/hosts, /etc/resolv.conf and /etc/nsswitch.conf the way Go does, looks
 * again at most every five seconds, and asks the name servers over UDP, then
 * TCP for an answer that was cut short. Go can also hand a lookup to the C
 * library through cgo, and burrow never does, so the settings that ask for
 * that, such as GODEBUG=netdns=cgo, act as Go does when cgo is not there.
 * Go on Windows asks the system, and burrow does not yet: it reads the hosts
 * file there and asks 127.0.0.1 and ::1, which is what a missing resolv.conf
 * means, until the name servers of the network adapters can be read.
 *
 * A NULL NetResolver is the default one, as a nil *Resolver is in Go. The
 * results are made in a, the slice and the strings and addresses in it, and
 * an arena is the easy way to give them back. An error is a NetDNSError, or
 * a NetAddrError for an address that is not one, in error_allocator(). A few
 * of them come with results as well: when some of the records in an answer
 * are not well formed, the rest come back with a DNSError that says so, as
 * in Go. */

/* Resolver.Dial: makes the connection to a name server, at address, over
 * network, which is "udp" or "tcp". The NetConn is made in a, which the
 * resolver gives back after it has closed it. *packet starts out false, and
 * the dial sets it for a connection where each read gives one whole message,
 * which Go finds out by asking whether the Conn is a PacketConn. A UDP
 * connection from this package is one either way. The others get a two byte
 * length before each message, the way DNS over TCP does. */
BURROW_FUNC(NetResolverDial, NetConn, Alloc *a, Context ctx, Str network, Str address,
            bool *packet, Error *err);

typedef struct NetResolver {
    /* Go's resolver rather than the system's. burrow only has Go's, so this
     * changes nothing, and is here to keep the struct Go's shape. */
    bool prefer_go;

    /* A temporary error from any one query fails the whole lookup, rather
     * than giving what the other queries found. */
    bool strict_errors;

    /* How to reach a name server, and net_dial_udp or net_dial_tcp when f is
     * NULL. */
    NetResolverDial dial;

    /* Internal: the lookups in flight, so that two of the same name at once
     * share one. A zeroed NetResolver is ready to use. */
    SyncMutex burrow_mu;
    void *burrow_calls;
} NetResolver;

/* net.DefaultResolver, which the net_lookup functions and a NULL resolver
 * use. */
BURROW_STATIC(ret) NetResolver *net_default_resolver(void);

/* LookupHost: the addresses of host, as text, from the hosts file and DNS. */
BURROW_OWNS(ret) Slice net_resolver_lookup_host(NetResolver *r, Alloc *a, Context ctx,
                                                Str host, Error *err);

/* LookupIPAddr: the same as NetIPAddr values. */
BURROW_OWNS(ret) Slice net_resolver_lookup_ip_addr(NetResolver *r, Alloc *a,
                                                   Context ctx, Str host, Error *err);

/* LookupIP: the addresses as NetIP values, for network "ip", "ip4" or "ip6",
 * with only the IPv4 or only the IPv6 ones for the last two. */
BURROW_OWNS(ret) Slice net_resolver_lookup_ip(NetResolver *r, Alloc *a, Context ctx,
                                              Str network, Str host, Error *err);

/* LookupNetIP: the same as NetipAddr values. */
BURROW_OWNS(ret) Slice net_resolver_lookup_net_ip(NetResolver *r, Alloc *a, Context ctx,
                                                  Str network, Str host, Error *err);

/* LookupPort: the port of service on network, which is "tcp", "udp" or one
 * of the "4" and "6" forms of them, or "ip" for either. */
Int net_resolver_lookup_port(NetResolver *r, Context ctx, Str network, Str service,
                             Error *err);

/* LookupCNAME: the canonical name of host, the name its CNAME records lead
 * to, or host itself when it has none. */
BURROW_OWNS(ret) Str net_resolver_lookup_cname(NetResolver *r, Alloc *a, Context ctx,
                                               Str host, Error *err);

/* LookupSRV: the SRV records of _service._proto.name, as NetSRV values,
 * sorted by priority and shuffled by weight, with the name they were found
 * under in *cname, which may be NULL. Empty service and proto look up name
 * itself. */
BURROW_OWNS(ret) Slice net_resolver_lookup_srv(NetResolver *r, Alloc *a, Context ctx,
                                               Str service, Str proto, Str name,
                                               Str *cname, Error *err);

/* LookupMX, LookupNS and LookupTXT: the records of name, as NetMX values
 * sorted by preference, NetNS values and Strs. The TXT strings of one record
 * are joined into one. */
BURROW_OWNS(ret) Slice net_resolver_lookup_mx(NetResolver *r, Alloc *a, Context ctx,
                                              Str name, Error *err);
BURROW_OWNS(ret) Slice net_resolver_lookup_ns(NetResolver *r, Alloc *a, Context ctx,
                                              Str name, Error *err);
BURROW_OWNS(ret) Slice net_resolver_lookup_txt(NetResolver *r, Alloc *a, Context ctx,
                                               Str name, Error *err);

/* LookupAddr: the names of addr, from the hosts file and PTR records. */
BURROW_OWNS(ret) Slice net_resolver_lookup_addr(NetResolver *r, Alloc *a, Context ctx,
                                                Str addr, Error *err);

/* The package functions, which are the default resolver's with
 * context_background(). */
BURROW_OWNS(ret) Slice net_lookup_host(Alloc *a, Str host, Error *err);
BURROW_OWNS(ret) Slice net_lookup_ip(Alloc *a, Str host, Error *err);
Int net_lookup_port(Str network, Str service, Error *err);
BURROW_OWNS(ret) Str net_lookup_cname(Alloc *a, Str host, Error *err);
BURROW_OWNS(ret) Slice net_lookup_srv(Alloc *a, Str service, Str proto, Str name,
                                      Str *cname, Error *err);
BURROW_OWNS(ret) Slice net_lookup_mx(Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) Slice net_lookup_ns(Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) Slice net_lookup_txt(Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) Slice net_lookup_addr(Alloc *a, Str addr, Error *err);

/* ----------------------------------------------------------------- Dialer
 *
 * net.Dial, net.Listen, net.Dialer and net.ListenConfig, which take a
 * network and an address as text and do the rest:
 *
 *     NetConn c = net_dial(a, BURROW_S("tcp"), BURROW_S("example.com:80"), &err);
 *     NetListener l = net_listen(a, BURROW_S("tcp"), BURROW_S(":8080"), &err);
 *
 * The networks are "tcp", "tcp4", "tcp6", "udp", "udp4", "udp6", "ip",
 * "ip4", "ip6", "unix", "unixgram" and "unixpacket", with a protocol after
 * the "ip" ones, as in "ip4:icmp". The TCP and UDP ones take a host and a
 * port, as net_join_host_port gives, where the host may be a name, which the
 * resolver looks up, and the port a service name. A dial to a name with several
 * addresses tries them in turn until one answers, and for "tcp", when the
 * name has both IPv4 and IPv6 addresses, it races the first of each kind the
 * way RFC 6555 says, which Go calls Happy Eyeballs. The "ip" ones take a host
 * with no port. Multipath TCP is still to come, and a dial or listen is
 * always a plain TCP one.
 *
 * The NetConn and NetListener are made in a and go back with net_conn_free
 * and net_listener_free, which close them first. net_conn_as_tcp_conn and
 * the others give the connection under one. The errors are NetOpErrors with
 * the op "dial" or "listen", as in Go, such as "dial tcp: lookup
 * example.invalid: no such host". */

/* Dialer.Control and Dialer.ControlContext, called with the socket made and
 * not yet connected, so that options can be set on it. network is what the
 * socket is for, such as "tcp4", and address the one about to be dialed. An
 * error stops the dial with it. */
BURROW_FUNC(NetDialerControl, Error, Str network, Str address, SyscallRawConn c);
BURROW_FUNC(NetDialerControlContext, Error, Context ctx, Str network, Str address,
            SyscallRawConn c);

/* net.Dialer. The zero value dials with no timeout but the system's, from an
 * address the system picks, with TCP keep-alives every fifteen seconds. */
typedef struct NetDialer {
    /* The longest a dial may take, the lookup included, with zero for no
     * limit. A dial to several addresses splits it between them. */
    Duration timeout;

    /* When a dial has to give up by, with the zero Time for never. The
     * sooner of this, timeout and the context's deadline wins. */
    Time deadline;

    /* Where to dial from, a NetAddr of the same kind as the address dialed,
     * or nil for one the system picks. */
    NetAddr local_addr;

    /* How long the IPv6 attempt runs before the IPv4 one starts as well,
     * with zero for 300ms and anything below zero to turn the race off. */
    Duration fallback_delay;

    /* The TCP keep-alive period, with zero for fifteen seconds and below
     * zero for none, when keep_alive_config.enable is false. When it is
     * true, keep_alive_config is used as it is. */
    Duration keep_alive;
    NetKeepAliveConfig keep_alive_config;

    /* The resolver for names, and the default one when NULL. */
    NetResolver *resolver;

    /* Deprecated in Go for the context: a channel whose closing stops the
     * dial. NULL for none. */
    Chan *cancel;

    /* Called on the socket before it connects. control_context wins when
     * both are set. */
    NetDialerControl control;
    NetDialerControlContext control_context;

    /* Deprecated in Go: the race is on unless fallback_delay is below zero,
     * and this changes nothing. */
    bool dual_stack;
} NetDialer;

/* Dialer.DialContext: a connection to address on network, made in a. A
 * NULL d is the zero Dialer. The context bounds the whole dial, the lookup
 * included, and once the connection is made it no longer matters. */
BURROW_OWNS(ret) NetConn net_dialer_dial_context(const NetDialer *d, Alloc *a,
                                                 Context ctx, Str network, Str address,
                                                 Error *err);

/* Dialer.Dial, which is DialContext with context_background(). */
BURROW_OWNS(ret) NetConn net_dialer_dial(const NetDialer *d, Alloc *a, Str network,
                                         Str address, Error *err);

/* Dial and DialTimeout, which use a Dialer with nothing but the timeout
 * set. */
BURROW_OWNS(ret) NetConn net_dial(Alloc *a, Str network, Str address, Error *err);
BURROW_OWNS(ret) NetConn net_dial_timeout(Alloc *a, Str network, Str address,
                                          Duration timeout, Error *err);

/* Dialer.DialTCP, DialUDP and DialUnix, new in Go 1.27: a dial to an address
 * that needs no lookup, with the Dialer's options, the context and Go's
 * errors. A zero laddr is no local address. */
BURROW_OWNS(ret) NetTCPConn *net_dialer_dial_tcp(const NetDialer *d, Alloc *a,
                                                 Context ctx, Str network,
                                                 NetipAddrPort laddr,
                                                 NetipAddrPort raddr, Error *err);
BURROW_OWNS(ret) NetUDPConn *net_dialer_dial_udp(const NetDialer *d, Alloc *a,
                                                 Context ctx, Str network,
                                                 NetipAddrPort laddr,
                                                 NetipAddrPort raddr, Error *err);
BURROW_OWNS(ret) NetUnixConn *
net_dialer_dial_unix(const NetDialer *d, Alloc *a, Context ctx, Str network,
                     const NetUnixAddr *laddr, const NetUnixAddr *raddr, Error *err);

/* ListenConfig.Control, called on a listener's socket before it is bound. */
typedef NetDialerControl NetListenConfigControl;

/* net.ListenConfig. keep_alive and keep_alive_config are what the
 * connections a TCP listener accepts get, read as the Dialer's are. */
typedef struct NetListenConfig {
    NetListenConfigControl control;
    Duration keep_alive;
    NetKeepAliveConfig keep_alive_config;
} NetListenConfig;

/* ListenConfig.Listen: a listener on address for "tcp", "tcp4", "tcp6",
 * "unix" or "unixpacket", made in a. An empty host, or none at all as in
 * ":8080", is every address of the machine, and port 0 asks for a free one.
 * A host with several addresses listens on the first IPv4 one. A NULL lc is
 * the zero ListenConfig. */
BURROW_OWNS(ret) NetListener net_listen_config_listen(const NetListenConfig *lc,
                                                      Alloc *a, Context ctx,
                                                      Str network, Str address,
                                                      Error *err);

/* Listen, which is the zero ListenConfig's with context_background(). */
BURROW_OWNS(ret) NetListener net_listen(Alloc *a, Str network, Str address, Error *err);

/* ListenConfig.ListenPacket: a packet connection on address for "udp",
 * "udp4", "udp6", "unixgram" or an "ip" network with its protocol, made in
 * a, with the addresses read as net_listen_config_listen reads them. */
BURROW_OWNS(ret) NetPacketConn
net_listen_config_listen_packet(const NetListenConfig *lc, Alloc *a, Context ctx,
                                Str network, Str address, Error *err);

/* ListenPacket, which is the zero ListenConfig's with context_background(). */
BURROW_OWNS(ret) NetPacketConn net_listen_packet(Alloc *a, Str network, Str address,
                                                 Error *err);

/* Closes c if it is open and gives back what this package made for it, for
 * any connection this package made: TCP, UDP, Unix and both ends of a Pipe.
 * Anything else is only closed. The nil NetConn does nothing. */
void net_conn_free(NetConn c);

/* The same for a listener, which is TCP or Unix, and for a packet
 * connection, which is UDP or Unix. */
void net_listener_free(NetListener l);
void net_packet_conn_free(NetPacketConn c);

/* ------------------------------------------------------------- descriptors */

/* The descriptors. NetIP lists AppendText, MarshalText, String and
 * UnmarshalText, NetIPMask lists String and NetIPNet lists Network and
 * String. A String method has no allocator to take, so these put their text
 * in the calling goroutine's error arena. */
extern const Type burrow_type_NetIP;
extern const Type burrow_type_NetIPMask;
extern const Type burrow_type_NetIPNet;
#define TYPE_NET_IP TYPE_OF(NetIP)
#define TYPE_NET_IP_MASK TYPE_OF(NetIPMask)
#define TYPE_NET_IP_NET TYPE_OF(NetIPNet)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_H */
