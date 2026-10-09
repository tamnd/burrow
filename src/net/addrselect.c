/* The order to try a host's addresses in, from RFC 6724.
 *
 * Derived from Go's src/net/addrselect.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/slice.h"
#include "burrow/slices.h"

#include <stdint.h>
#include <string.h>

/* ipAttr */
typedef struct RaAttr {
    uint8_t scope;
    uint8_t precedence;
    uint8_t label;
} RaAttr;

/* byRFC6724Info */
typedef struct RaInfo {
    NetIPAddr addr;
    NetipAddr src;
    RaAttr addr_attr;
    RaAttr src_attr;
} RaInfo;

BURROW__NET_ELEM_DESC(ra_info_type, "byRFC6724Info", RaInfo, 0x72613669U);

/* -------------------------------------------------------- the policy table */

/* rfc6724policyTable, as the prefix bytes, the length, the precedence and
 * the label. */
static const struct {
    Byte prefix[16];
    uint8_t bits;
    uint8_t precedence;
    uint8_t label;
} ra_policy[] = {
    /* ::1/128 */
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}, 128, 50, 0},
    /* ::ffff:0:0/96, IPv4-compatible */
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, 96, 35, 4},
    /* ::/96 */
    {{0}, 96, 1, 3},
    /* 2001::/32, Teredo */
    {{0x20, 0x01}, 32, 5, 5},
    /* 2002::/16, 6to4 */
    {{0x20, 0x02}, 16, 30, 2},
    /* 3ffe::/16, 6bone */
    {{0x3f, 0xfe}, 16, 1, 12},
    /* fec0::/10, site-local */
    {{0xfe, 0xc0}, 10, 1, 11},
    /* fc00::/7, unique local */
    {{0xfc}, 7, 3, 13},
    /* ::/0 */
    {{0}, 0, 40, 1},
};

burrow__NetPolicyEntry burrow__net_policy_classify(NetipAddr ip) {
    if (netip_addr_is4(ip)) {
        NetipAddrAs16Ret b = netip_addr_as16(ip);
        ip = netip_addr_from16(b.a);
    }
    for (size_t i = 0; i < sizeof ra_policy / sizeof ra_policy[0]; i++) {
        NetipPrefix p = netip_prefix_from(netip_addr_from16(ra_policy[i].prefix),
                                          ra_policy[i].bits);
        if (netip_prefix_contains(p, ip)) {
            burrow__NetPolicyEntry e = {p, ra_policy[i].precedence, ra_policy[i].label};
            return e;
        }
    }
    burrow__NetPolicyEntry none;
    memset(&none, 0, sizeof none);
    return none;
}

uint8_t burrow__net_classify_scope(NetipAddr ip) {
    if (netip_addr_is_loopback(ip) || netip_addr_is_link_local_unicast(ip))
        return BURROW__NET_SCOPE_LINK_LOCAL;
    bool ipv6 = netip_addr_is6(ip) && !netip_addr_is4_in6(ip);
    NetipAddrAs16Ret b = netip_addr_as16(ip);
    if (ipv6 && netip_addr_is_multicast(ip))
        return (uint8_t)(b.a[1] & 0xf);
    /* Site-local addresses are deprecated, and RFC 6724 still gives them a
     * scope of their own. */
    if (ipv6 && b.a[0] == 0xfe && (b.a[1] & 0xc0) == 0xc0)
        return BURROW__NET_SCOPE_SITE_LOCAL;
    return BURROW__NET_SCOPE_GLOBAL;
}

static RaAttr ra_attr_of(NetipAddr ip) {
    RaAttr r = {0, 0, 0};
    if (!netip_addr_is_valid(ip))
        return r;
    burrow__NetPolicyEntry m = burrow__net_policy_classify(ip);
    r.scope = burrow__net_classify_scope(ip);
    r.precedence = m.precedence;
    r.label = m.label;
    return r;
}

Int burrow__net_common_prefix_len(NetipAddr a, NetIP b) {
    NetIP b4 = net_ip_to4(b);
    if (b4.len > 0)
        b = b4;
    Byte ab[16];
    Int alen = 0;
    if (netip_addr_is4(a)) {
        NetipAddrAs4Ret x = netip_addr_as4(a);
        memcpy(ab, x.a, 4);
        alen = 4;
    } else if (netip_addr_is_valid(a)) {
        NetipAddrAs16Ret x = netip_addr_as16(a);
        memcpy(ab, x.a, 16);
        alen = 16;
    }
    if (alen != b.len)
        return 0;
    /* Only the 64 bit prefix of an IPv6 address counts. */
    if (alen > 8)
        alen = 8;
    const Byte *bb = (const Byte *)b.p;
    Int cpl = 0;
    for (Int i = 0; i < alen; i++) {
        if (ab[i] == bb[i]) {
            cpl += 8;
            continue;
        }
        Int bits = 8;
        Byte x = ab[i], y = bb[i];
        for (;;) {
            x >>= 1;
            y >>= 1;
            bits--;
            if (x == y)
                return cpl + bits;
        }
    }
    return cpl;
}

/* ------------------------------------------------------------------ sorting */

/* compareByRFC6724, with DA and DB the two destinations. The rules RFC 6724
 * has that need to know more than this does, about the addresses that are
 * deprecated, the home addresses and the interfaces, are left out, as in
 * Go. */
static int ra_compare(void *env, const void *pa, const void *pb) {
    (void)env;
    const RaInfo *a = (const RaInfo *)pa;
    const RaInfo *b = (const RaInfo *)pb;
    const RaAttr *attr_da = &a->addr_attr;
    const RaAttr *attr_db = &b->addr_attr;
    const RaAttr *attr_src_da = &a->src_attr;
    const RaAttr *attr_src_db = &b->src_attr;
    enum { PREFER_DA = -1, PREFER_DB = 1 };

    /* Rule 1: avoid unusable destinations. */
    bool va = netip_addr_is_valid(a->src);
    bool vb = netip_addr_is_valid(b->src);
    if (!va && !vb)
        return 0;
    if (!vb)
        return PREFER_DA;
    if (!va)
        return PREFER_DB;

    /* Rule 2: prefer matching scope. */
    if (attr_da->scope == attr_src_da->scope && attr_db->scope != attr_src_db->scope)
        return PREFER_DA;
    if (attr_da->scope != attr_src_da->scope && attr_db->scope == attr_src_db->scope)
        return PREFER_DB;

    /* Rule 5: prefer matching label. */
    if (attr_src_da->label == attr_da->label && attr_src_db->label != attr_db->label)
        return PREFER_DA;
    if (attr_src_da->label != attr_da->label && attr_src_db->label == attr_db->label)
        return PREFER_DB;

    /* Rule 6: prefer higher precedence. */
    if (attr_da->precedence > attr_db->precedence)
        return PREFER_DA;
    if (attr_da->precedence < attr_db->precedence)
        return PREFER_DB;

    /* Rule 8: prefer smaller scope. */
    if (attr_da->scope < attr_db->scope)
        return PREFER_DA;
    if (attr_da->scope > attr_db->scope)
        return PREFER_DB;

    /* Rule 9: use the longest matching prefix, for IPv6 only. */
    if (net_ip_to4(a->addr.ip).len == 0 && net_ip_to4(b->addr.ip).len == 0) {
        Int common_a = burrow__net_common_prefix_len(a->src, a->addr.ip);
        Int common_b = burrow__net_common_prefix_len(b->src, b->addr.ip);
        if (common_a > common_b)
            return PREFER_DA;
        if (common_a < common_b)
            return PREFER_DB;
    }

    /* Rule 10: otherwise, leave the order unchanged. */
    return 0;
}

void burrow__net_sort_by_rfc6724_with_srcs(NetIPAddr *addrs, const NetipAddr *srcs,
                                           Int n) {
    if (n < 2)
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    RaInfo *infos = (RaInfo *)mem_alloc(arena_allocator(&ar),
                                        sizeof(RaInfo) * (size_t)n, _Alignof(RaInfo));
    if (infos == NULL) {
        /* Out of memory, and the addresses keep the order they came in. */
        arena_free(&ar);
        return;
    }
    for (Int i = 0; i < n; i++) {
        bool ok;
        NetipAddr ip = netip_addr_from_slice(addrs[i].ip, &ok);
        infos[i].addr = addrs[i];
        infos[i].addr_attr = ra_attr_of(ip);
        infos[i].src = srcs[i];
        infos[i].src_attr = ra_attr_of(srcs[i]);
    }
    slices_sort_stable_func(slice_from(infos, n, n, &ra_info_type),
                            BURROW_FN(SlicesCmpFunc, ra_compare, NULL));
    for (Int i = 0; i < n; i++)
        addrs[i] = infos[i].addr;
    arena_free(&ar);
}

/* srcAddrs: the address each destination would be reached from, which a
 * UDP socket connected to it says without sending anything. The zero
 * NetipAddr for one that cannot be reached. */
static void ra_src_addrs(const NetIPAddr *addrs, NetipAddr *srcs, Int n) {
    for (Int i = 0; i < n; i++) {
        memset(&srcs[i], 0, sizeof srcs[i]);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        NetUDPAddr dst = {addrs[i].ip, 53, addrs[i].zone};
        NetUDPConn *c =
            net_dial_udp(arena_allocator(&ar), BURROW_S("udp"), NULL, &dst, &err);
        if (BURROW_OK(err) && c != NULL) {
            NetAddr la = net_udp_conn_local_addr(c);
            if (la.vt != NULL && la.vt->self_type == TYPE_NET_UDP_ADDR) {
                const NetUDPAddr *src = (const NetUDPAddr *)la.data;
                bool ok;
                srcs[i] = netip_addr_from_slice(src->ip, &ok);
            }
            net_udp_conn_free(c);
        }
        arena_free(&ar);
    }
}

void burrow__net_sort_by_rfc6724(NetIPAddr *addrs, Int n) {
    if (n < 2)
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    NetipAddr *srcs = (NetipAddr *)mem_alloc(
        arena_allocator(&ar), sizeof(NetipAddr) * (size_t)n, _Alignof(NetipAddr));
    if (srcs != NULL) {
        ra_src_addrs(addrs, srcs, n);
        burrow__net_sort_by_rfc6724_with_srcs(addrs, srcs, n);
    }
    arena_free(&ar);
}
