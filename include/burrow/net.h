/* net, the addresses, the interfaces and Pipe so far.
 *
 * Go's net/ip.go, the interfaces and errors from net.go, SplitHostPort and
 * JoinHostPort from ipsock.go, and pipe.go. The sockets, the resolver and the
 * rest of the package come later. These are here first because crypto/x509
 * and crypto/tls use them, and Pipe is what crypto/tls is tested over.
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

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
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

/* Closes l if it is still open and gives its memory back. NULL does
 * nothing. The connections it accepted are their own and are not freed. */
void net_tcp_listener_free(NetTCPListener *l);

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
