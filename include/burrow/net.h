/* net, the addresses part so far.
 *
 * Go's net/ip.go, the address errors from net.go, and SplitHostPort and
 * JoinHostPort from ipsock.go. The sockets, the resolver and the rest of the
 * package come later. These are here first because crypto/x509 and
 * crypto/tls use them.
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
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
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
