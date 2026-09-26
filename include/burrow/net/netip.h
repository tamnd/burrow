/* net/netip, IP addresses as small values.
 *
 * Go's net/netip. A NetipAddr is an IPv4 or IPv6 address, with an IPv6 zone
 * when there is one, in 24 bytes and no allocation. A NetipAddrPort is an
 * address and a port, and a NetipPrefix is an address and a prefix length,
 * which is what CIDR notation such as 10.0.0.0/8 writes down.
 *
 *     Error err;
 *     NetipPrefix lan = netip_parse_prefix(BURROW_S("192.168.0.0/16"), &err);
 *     NetipAddr ip = netip_parse_addr(BURROW_S("192.168.1.20"), &err);
 *     if (BURROW_OK(err) && netip_prefix_contains(lan, ip))
 *         ...
 *
 * All three are passed and returned by value. C has no == on structs, so
 * compare them with netip_addr_eq and friends, or with the compare functions
 * when order matters. They work as Map keys too, through their descriptors,
 * since a zone is kept as a unique handle and two equal addresses are equal
 * byte for byte.
 *
 * The zero NetipAddr is not a valid address, and is not 0.0.0.0 or :: either.
 * The parse functions put their errors in the calling goroutine's error arena
 * and need no allocator. The functions that build text take one.
 *
 * A zone goes through unique_make, so it is kept for as long as the process
 * runs. Zones are interface names in practice and there are few of them, but
 * do not parse addresses with zones out of untrusted input by the million.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/netip */

#ifndef BURROW_NET_NETIP_H
#define BURROW_NET_NETIP_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/unique.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- NetipAddr */

/* netip.Addr. The fields are here so the struct has a size, and are not for
 * touching.
 *
 * hi and lo are the 128 bits of the IPv6 form, most significant first, and an
 * IPv4 address is kept as its IPv4-mapped IPv6 form. z says what the address
 * is: nil for the zero Addr, one of two sentinels for IPv4 and for IPv6 with
 * no zone, and otherwise the unique handle of the zone string. */
typedef struct NetipAddr {
    uint64_t hi;
    uint64_t lo;
    UniqueHandle z;
} NetipAddr;

/* Not API. The sentinels z points at for IPv4 and for IPv6 without a zone.
 * Both are empty strings, so a zone read through either is "". */
extern const Str burrow__netip_z4;
extern const Str burrow__netip_z6noz;

/* netip.AddrFrom4 and netip.AddrFrom16. The bytes are in network order. An
 * IPv4-mapped IPv6 address from netip_addr_from16 stays IPv6. */
NetipAddr netip_addr_from4(const Byte addr[4]);
NetipAddr netip_addr_from16(const Byte addr[16]);

/* netip.AddrFromSlice. The address in a 4 or 16 byte slice. Any other length
 * gives the zero Addr and false in *ok, which may be NULL. */
NetipAddr netip_addr_from_slice(Slice slice, bool *ok);

/* netip.ParseAddr. Takes dotted decimal ("192.0.2.1"), IPv6 ("2001:db8::68")
 * and IPv6 with a zone ("fe80::1%eth0"). Bad input gives the zero Addr and an
 * error whose text says what was wrong and where, as Go's does. */
NetipAddr netip_parse_addr(Str s, Error *err);

/* netip.MustParseAddr. netip_parse_addr, panicking on an error. For tests and
 * constants. */
NetipAddr netip_must_parse_addr(Str s);

/* The well known addresses: ff02::1, ff02::2, ::1, :: and 0.0.0.0. */
NetipAddr netip_ipv6_link_local_all_nodes(void);
NetipAddr netip_ipv6_link_local_all_routers(void);
NetipAddr netip_ipv6_loopback(void);
NetipAddr netip_ipv6_unspecified(void);
NetipAddr netip_ipv4_unspecified(void);

/* Go's ip == ip2. The zone counts. */
static inline bool netip_addr_eq(NetipAddr ip, NetipAddr ip2) {
    return ip.hi == ip2.hi && ip.lo == ip2.lo && ip.z.value == ip2.z.value;
}

/* netip.Addr.IsValid. False only for the zero Addr. */
static inline bool netip_addr_is_valid(NetipAddr ip) {
    return ip.z.value != NULL;
}

/* netip.Addr.Is4. False for an IPv4-mapped IPv6 address. */
static inline bool netip_addr_is4(NetipAddr ip) {
    return ip.z.value == &burrow__netip_z4;
}

/* netip.Addr.Is6. True for IPv4-mapped IPv6 addresses too. */
static inline bool netip_addr_is6(NetipAddr ip) {
    return ip.z.value != NULL && ip.z.value != &burrow__netip_z4;
}

/* netip.Addr.Is4In6. Whether ip is in ::ffff:0:0/96. */
static inline bool netip_addr_is4_in6(NetipAddr ip) {
    return netip_addr_is6(ip) && ip.hi == 0 && ip.lo >> 32 == 0xffff;
}

/* netip.Addr.BitLen. 32 for IPv4, 128 for IPv6 and 0 for the zero Addr. */
static inline Int netip_addr_bit_len(NetipAddr ip) {
    if (ip.z.value == NULL)
        return 0;
    return ip.z.value == &burrow__netip_z4 ? 32 : 128;
}

/* netip.Addr.Zone. The zone, or "" when there is none. It lives as long as the
 * process does. */
static inline BURROW_STATIC(ret) Str netip_addr_zone(NetipAddr ip) {
    return ip.z.value == NULL ? BURROW_STR_EMPTY : *(const Str *)ip.z.value;
}

/* netip.Addr.Compare and Less. Addresses sort by bit length, then by value,
 * and an IPv6 address with a zone sorts just after the same one without. */
Int netip_addr_compare(NetipAddr ip, NetipAddr ip2);
bool netip_addr_less(NetipAddr ip, NetipAddr ip2);

/* netip.Addr.Unmap. The IPv4 address inside an IPv4-mapped IPv6 one, and any
 * other address as it is. */
NetipAddr netip_addr_unmap(NetipAddr ip);

/* netip.Addr.WithZone. ip with the zone replaced, or removed when zone is
 * empty. An IPv4 address comes back as it is. The zone is copied. */
NetipAddr netip_addr_with_zone(NetipAddr ip, Str zone);

/* The address classes, as Go defines them. An IPv4-mapped address is judged
 * by the IPv4 address inside it, except by is_interface_local_multicast, which
 * is IPv6 only. is_global_unicast is true for private addresses too, and
 * is_private says nothing about security. */
bool netip_addr_is_link_local_unicast(NetipAddr ip);
bool netip_addr_is_loopback(NetipAddr ip);
bool netip_addr_is_multicast(NetipAddr ip);
bool netip_addr_is_interface_local_multicast(NetipAddr ip);
bool netip_addr_is_link_local_multicast(NetipAddr ip);
bool netip_addr_is_global_unicast(NetipAddr ip);
bool netip_addr_is_private(NetipAddr ip);
bool netip_addr_is_unspecified(NetipAddr ip);

typedef struct NetipPrefix NetipPrefix;

/* netip.Addr.As16 and As4, whose results are arrays. As16 gives an IPv4
 * address in its mapped form and drops any zone. As4 panics on the zero Addr
 * and on an IPv6 address that is not IPv4-mapped. */
typedef struct NetipAddrAs16Ret {
    Byte a[16];
} NetipAddrAs16Ret;
typedef struct NetipAddrAs4Ret {
    Byte a[4];
} NetipAddrAs4Ret;
NetipAddrAs16Ret netip_addr_as16(NetipAddr ip);
NetipAddrAs4Ret netip_addr_as4(NetipAddr ip);

/* netip.Addr.AsSlice. The 4 or 16 bytes in a new slice from a, or the nil
 * slice for the zero Addr. */
BURROW_OWNS(ret) Slice netip_addr_as_slice(NetipAddr ip, Alloc *a);

/* netip.Addr.Next and Prev. The address after or before ip, or the zero Addr
 * when there is none. The zone is kept. */
NetipAddr netip_addr_next(NetipAddr ip);
NetipAddr netip_addr_prev(NetipAddr ip);

/* netip.Addr.String. "192.0.2.1", "2001:db8::1", "::ffff:192.0.2.1" for an
 * IPv4-mapped address, "fe80::1%eth0" with a zone, and "invalid IP" for the
 * zero Addr. IPv6 follows RFC 5952, with the longest run of zero groups
 * written as :: and lower case hex. */
BURROW_OWNS(ret) Str netip_addr_string(NetipAddr ip, Alloc *a);

/* netip.Addr.StringExpanded. Like netip_addr_string, but IPv6 is written in
 * full: "2001:0db8:0000:0000:0000:0000:0000:0001". */
BURROW_OWNS(ret) Str netip_addr_string_expanded(NetipAddr ip, Alloc *a);

/* netip.Addr.AppendTo. The text netip_addr_marshal_text gives, appended to b,
 * which is nothing for the zero Addr. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_addr_append_to(NetipAddr ip,
                                                                   Alloc *a, Slice b);

/* The encoding methods, which are listed in the descriptor too, so encoding
 * and the codecs find them.
 *
 * The text form is netip_addr_string's, except that the zero Addr is the empty
 * string, and unmarshal_text takes the empty string back to the zero Addr. The
 * binary form is nothing for the zero Addr, the 4 bytes of an IPv4 address,
 * and the 16 bytes of an IPv6 one with the zone after them. None of these
 * fail except the unmarshals, but they keep Go's error results. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_addr_append_text(NetipAddr ip,
                                                                     Alloc *a, Slice b,
                                                                     Error *err);
BURROW_OWNS(ret) Slice netip_addr_marshal_text(NetipAddr ip, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error netip_addr_unmarshal_text(NetipAddr *ip, Slice text);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_addr_append_binary(NetipAddr ip,
                                                                       Alloc *a,
                                                                       Slice b,
                                                                       Error *err);
BURROW_OWNS(ret) Slice netip_addr_marshal_binary(NetipAddr ip, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error netip_addr_unmarshal_binary(NetipAddr *ip, Slice b);

/* ------------------------------------------------------------ NetipAddrPort */

/* netip.AddrPort, an address and a port. The fields are Go's unexported ones,
 * and the functions below are the way in. */
typedef struct NetipAddrPort {
    NetipAddr ip;
    uint16_t port;
} NetipAddrPort;

/* netip.AddrPortFrom. */
static inline NetipAddrPort netip_addr_port_from(NetipAddr ip, uint16_t port) {
    NetipAddrPort p = {ip, port};
    return p;
}

/* netip.AddrPort.Addr, Port and IsValid. Every port is valid, including 0. */
static inline NetipAddr netip_addr_port_addr(NetipAddrPort p) {
    return p.ip;
}
static inline uint16_t netip_addr_port_port(NetipAddrPort p) {
    return p.port;
}
static inline bool netip_addr_port_is_valid(NetipAddrPort p) {
    return netip_addr_is_valid(p.ip);
}

/* Go's p == p2. */
static inline bool netip_addr_port_eq(NetipAddrPort p, NetipAddrPort p2) {
    return netip_addr_eq(p.ip, p2.ip) && p.port == p2.port;
}

/* netip.ParseAddrPort. "1.2.3.4:80" or "[::1]:80", with a numeric port. There
 * is no name lookup. IPv6 must be in brackets and IPv4 must not be. */
NetipAddrPort netip_parse_addr_port(Str s, Error *err);

/* netip.MustParseAddrPort. */
NetipAddrPort netip_must_parse_addr_port(Str s);

/* netip.AddrPort.Compare. By address, then by port. */
Int netip_addr_port_compare(NetipAddrPort p, NetipAddrPort p2);

/* netip.AddrPort.String. "1.2.3.4:80", "[::1]:80", and "invalid AddrPort" when
 * the address is the zero Addr. */
BURROW_OWNS(ret) Str netip_addr_port_string(NetipAddrPort p, Alloc *a);

/* netip.AddrPort.AppendTo, which appends nothing for the zero address. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_addr_port_append_to(NetipAddrPort p,
                                                                        Alloc *a,
                                                                        Slice b);

/* The encoding methods. The text form is the string's, empty for an invalid
 * AddrPort. The binary form is the address's with the port after it in two
 * little endian bytes, as Go writes it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
netip_addr_port_append_text(NetipAddrPort p, Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice netip_addr_port_marshal_text(NetipAddrPort p, Alloc *a,
                                                    Error *err);
BURROW_BORROWS(ret) Error netip_addr_port_unmarshal_text(NetipAddrPort *p, Slice text);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
netip_addr_port_append_binary(NetipAddrPort p, Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice netip_addr_port_marshal_binary(NetipAddrPort p, Alloc *a,
                                                      Error *err);
BURROW_BORROWS(ret) Error netip_addr_port_unmarshal_binary(NetipAddrPort *p, Slice b);

/* -------------------------------------------------------------- NetipPrefix */

/* netip.Prefix, an address and a prefix length. bits_plus_one is 0 for an
 * invalid prefix, which is what Go stores too. */
struct NetipPrefix {
    NetipAddr ip;
    uint8_t bits_plus_one;
};

/* netip.PrefixFrom. Does not mask off the host bits, and drops any zone. Bits
 * outside 0 to the address's bit length make an invalid prefix. */
NetipPrefix netip_prefix_from(NetipAddr ip, Int bits);

/* netip.Addr.Prefix. The prefix of ip's top bits bits, host bits cleared. The
 * zero Addr gives the zero Prefix and no error. Bits below zero or above the
 * address's bit length give an error. */
NetipPrefix netip_addr_prefix(NetipAddr ip, Int bits, Error *err);

/* netip.Prefix.Addr, Bits, IsValid and IsSingleIP. Bits is -1 for an invalid
 * prefix. */
static inline NetipAddr netip_prefix_addr(NetipPrefix p) {
    return p.ip;
}
static inline Int netip_prefix_bits(NetipPrefix p) {
    return (Int)p.bits_plus_one - 1;
}
static inline bool netip_prefix_is_valid(NetipPrefix p) {
    return p.bits_plus_one > 0;
}
static inline bool netip_prefix_is_single_ip(NetipPrefix p) {
    return p.bits_plus_one > 0 && netip_prefix_bits(p) == netip_addr_bit_len(p.ip);
}

/* Go's p == p2. */
static inline bool netip_prefix_eq(NetipPrefix p, NetipPrefix p2) {
    return netip_addr_eq(p.ip, p2.ip) && p.bits_plus_one == p2.bits_plus_one;
}

/* netip.ParsePrefix. "192.168.1.0/24" or "2001:db8::/32". A zone is an error.
 * The host bits are kept, so use netip_prefix_masked to clear them. */
NetipPrefix netip_parse_prefix(Str s, Error *err);

/* netip.MustParsePrefix. */
NetipPrefix netip_must_parse_prefix(Str s);

/* netip.Prefix.Compare. Invalid first, then IPv4 before IPv6, then by masked
 * address, then by length, then by the whole address. */
Int netip_prefix_compare(NetipPrefix p, NetipPrefix p2);

/* netip.Prefix.Masked. p with its host bits cleared, or the zero Prefix when
 * p is invalid. */
NetipPrefix netip_prefix_masked(NetipPrefix p);

/* netip.Prefix.Contains. Whether ip is in p. An IPv4 address is never in an
 * IPv6 prefix or the other way round, including IPv4-mapped ones, and an
 * address with a zone is in no prefix. */
bool netip_prefix_contains(NetipPrefix p, NetipAddr ip);

/* netip.Prefix.Overlaps. Whether p and o have any address in common. */
bool netip_prefix_overlaps(NetipPrefix p, NetipPrefix o);

/* netip.Prefix.String. "10.0.0.0/8", or "invalid Prefix". */
BURROW_OWNS(ret) Str netip_prefix_string(NetipPrefix p, Alloc *a);

/* netip.Prefix.AppendTo. Nothing for the zero Prefix and "invalid Prefix" for
 * any other invalid one. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_prefix_append_to(NetipPrefix p,
                                                                     Alloc *a, Slice b);

/* The encoding methods. The text form is the string's, empty for the zero
 * Prefix. The binary form is the address's with the length in one byte after
 * it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_prefix_append_text(NetipPrefix p,
                                                                       Alloc *a,
                                                                       Slice b,
                                                                       Error *err);
BURROW_OWNS(ret) Slice netip_prefix_marshal_text(NetipPrefix p, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error netip_prefix_unmarshal_text(NetipPrefix *p, Slice text);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice netip_prefix_append_binary(NetipPrefix p,
                                                                         Alloc *a,
                                                                         Slice b,
                                                                         Error *err);
BURROW_OWNS(ret) Slice netip_prefix_marshal_binary(NetipPrefix p, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error netip_prefix_unmarshal_binary(NetipPrefix *p, Slice b);

/* The descriptors, which list String and the encoding methods. A String
 * method has no allocator to take, so the ones here put their text in the
 * calling goroutine's error arena, as fmt's guide suggests. */
extern const Type burrow_type_NetipAddr;
extern const Type burrow_type_NetipAddrPort;
extern const Type burrow_type_NetipPrefix;
#define TYPE_NETIP_ADDR TYPE_OF(NetipAddr)
#define TYPE_NETIP_ADDR_PORT TYPE_OF(NetipAddrPort)
#define TYPE_NETIP_PREFIX TYPE_OF(NetipPrefix)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_NETIP_H */
