/* Derived from Go's src/net/ip.go, the ParseError and AddrError parts of
 * net.go, SplitHostPort and JoinHostPort from ipsock.go and dtoi from
 * parse.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define NET_LIT(s) str_from_bytes(s, (Int)sizeof(s) - 1)

static Byte *net_put(Byte *p, const void *src, Int n) {
    if (n > 0)
        memcpy(p, src, (size_t)n);
    return p + n;
}

static Str net_copy(Alloc *a, const void *p, Int n) {
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (q == NULL)
        return BURROW_STR_EMPTY;
    net_put(q, p, n);
    return str_from_bytes(q, n);
}

static const Byte *net_bytes(Slice s) {
    return (const Byte *)s.p;
}

static Slice net_view(const Byte *p, Int n) {
    return (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE};
}

static Slice net_make(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTE, n, n);
}

/* ------------------------------------------------------------------- errors */

/* Each error is a box with the struct first, so errors_as hands back a
 * pointer to it, and the message after, built once since the message slot
 * cannot allocate. */
typedef struct NetParseErrorBox {
    NetParseError e;
    Str message;
} NetParseErrorBox;

typedef struct NetAddrErrorBox {
    NetAddrError e;
    Str message;
} NetAddrErrorBox;

static Str net_parse_error_message(const void *self) {
    return ((const NetParseErrorBox *)self)->message;
}

static Str net_addr_error_message(const void *self) {
    return ((const NetAddrErrorBox *)self)->message;
}

bool net_parse_error_timeout(const NetParseError *e) {
    (void)e;
    return false;
}

bool net_parse_error_temporary(const NetParseError *e) {
    (void)e;
    return false;
}

bool net_addr_error_timeout(const NetAddrError *e) {
    (void)e;
    return false;
}

bool net_addr_error_temporary(const NetAddrError *e) {
    (void)e;
    return false;
}

#define NET_SIG_BOOL(IN, OUT) OUT(bool)

#define NET_PARSE_ERROR_METHODS(M, T)                                                  \
    M(T, Temporary, net_parse_error_temporary, NET_SIG_BOOL)                           \
    M(T, Timeout, net_parse_error_timeout, NET_SIG_BOOL)

#define NET_ADDR_ERROR_METHODS(M, T)                                                   \
    M(T, Temporary, net_addr_error_temporary, NET_SIG_BOOL)                            \
    M(T, Timeout, net_addr_error_timeout, NET_SIG_BOOL)

BURROW_METHODS_DEFINE(NetParseError, NET_PARSE_ERROR_METHODS);
BURROW_METHODS_DEFINE(NetAddrError, NET_ADDR_ERROR_METHODS);

#define NET_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

static const Field net_parse_error_fields[] = {
    {BURROW_S_INIT("Type"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetParseError, type)},
    {BURROW_S_INIT("Text"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetParseError, text)},
};

static const Field net_addr_error_fields[] = {
    {BURROW_S_INIT("Err"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetAddrError, err)},
    {BURROW_S_INIT("Addr"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetAddrError, addr)},
};

static const Type net_parse_error_desc = {
    BURROW_S_INIT("ParseError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetParseError),
    (uint16_t)_Alignof(NetParseError),
    NET_COUNT(net_parse_error_fields),
    NET_COUNT(burrow__methods_NetParseError),
    net_parse_error_fields,
    burrow__methods_NetParseError,
    NULL,
    NULL,
    0,
    0x6e657470U, /* "netp" */
    NULL,
};

static const Type net_addr_error_desc = {
    BURROW_S_INIT("AddrError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetAddrError),
    (uint16_t)_Alignof(NetAddrError),
    NET_COUNT(net_addr_error_fields),
    NET_COUNT(burrow__methods_NetAddrError),
    net_addr_error_fields,
    burrow__methods_NetAddrError,
    NULL,
    NULL,
    0,
    0x6e657461U, /* "neta" */
    NULL,
};

const Type *const TYPE_NET_PARSE_ERROR = &net_parse_error_desc;
const Type *const TYPE_NET_ADDR_ERROR = &net_addr_error_desc;

static Error net_parse_error_clone(const void *self, Alloc *a);
static Error net_addr_error_clone(const void *self, Alloc *a);

static const ErrorVT net_parse_error_vt = {
    &net_parse_error_desc, net_parse_error_message, NULL, NULL, NULL, NULL,
    net_parse_error_clone,
};

static const ErrorVT net_addr_error_vt = {
    &net_addr_error_desc, net_addr_error_message, NULL, NULL, NULL, NULL,
    net_addr_error_clone,
};

static const char net_msg_invalid[] = "invalid ";
static const char net_msg_address[] = "address ";

/* "invalid " + type + ": " + text. */
static Int net_parse_error_len(const NetParseError *e) {
    return (Int)sizeof net_msg_invalid - 1 + e->type.len + 2 + e->text.len;
}

static void net_parse_error_write(Byte *p, const NetParseError *e) {
    p = net_put(p, net_msg_invalid, (Int)sizeof net_msg_invalid - 1);
    p = net_put(p, e->type.p, e->type.len);
    *p++ = ':';
    *p++ = ' ';
    net_put(p, e->text.p, e->text.len);
}

/* "address " + addr + ": " + err, or err alone when addr is empty. */
static Int net_addr_error_len(const NetAddrError *e) {
    if (e->addr.len == 0)
        return e->err.len;
    return (Int)sizeof net_msg_address - 1 + e->addr.len + 2 + e->err.len;
}

static void net_addr_error_write(Byte *p, const NetAddrError *e) {
    if (e->addr.len != 0) {
        p = net_put(p, net_msg_address, (Int)sizeof net_msg_address - 1);
        p = net_put(p, e->addr.p, e->addr.len);
        *p++ = ':';
        *p++ = ' ';
    }
    net_put(p, e->err.p, e->err.len);
}

/* One allocation: the box, then the two strings and the message. */
Error net_parse_error_as_error(const NetParseError *e, Alloc *a) {
    Int mlen = net_parse_error_len(e);
    size_t size = sizeof(NetParseErrorBox) + (size_t)e->type.len + (size_t)e->text.len +
                  (size_t)mlen;
    NetParseErrorBox *b =
        (NetParseErrorBox *)mem_alloc_nozero(a, size, _Alignof(NetParseErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.type = str_from_bytes(p, e->type.len);
    p = net_put(p, e->type.p, e->type.len);
    b->e.text = str_from_bytes(p, e->text.len);
    p = net_put(p, e->text.p, e->text.len);
    net_parse_error_write(p, e);
    b->message = str_from_bytes(p, mlen);
    return (Error){&net_parse_error_vt, b};
}

Error net_addr_error_as_error(const NetAddrError *e, Alloc *a) {
    Int mlen = net_addr_error_len(e);
    size_t size = sizeof(NetAddrErrorBox) + (size_t)e->err.len + (size_t)e->addr.len +
                  (size_t)mlen;
    NetAddrErrorBox *b =
        (NetAddrErrorBox *)mem_alloc_nozero(a, size, _Alignof(NetAddrErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.err = str_from_bytes(p, e->err.len);
    p = net_put(p, e->err.p, e->err.len);
    b->e.addr = str_from_bytes(p, e->addr.len);
    p = net_put(p, e->addr.p, e->addr.len);
    net_addr_error_write(p, e);
    b->message = str_from_bytes(p, mlen);
    return (Error){&net_addr_error_vt, b};
}

static Error net_parse_error_clone(const void *self, Alloc *a) {
    return net_parse_error_as_error(&((const NetParseErrorBox *)self)->e, a);
}

static Error net_addr_error_clone(const void *self, Alloc *a) {
    return net_addr_error_as_error(&((const NetAddrErrorBox *)self)->e, a);
}

Str net_parse_error_error(const NetParseError *e, Alloc *a) {
    Int n = net_parse_error_len(e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    net_parse_error_write(p, e);
    return str_from_bytes(p, n);
}

Str net_addr_error_error(const NetAddrError *e, Alloc *a) {
    if (e == NULL)
        return net_copy(a, "<nil>", 5);
    Int n = net_addr_error_len(e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    net_addr_error_write(p, e);
    return str_from_bytes(p, n);
}

/* The errors this file makes go in the calling goroutine's error arena, as
 * the rest of the library's do. */
static Error net_parse_err(Str type, Str text) {
    NetParseError e = {type, text};
    return net_parse_error_as_error(&e, error_allocator());
}

static Error net_addr_err(Str why, Str addr) {
    NetAddrError e = {why, addr};
    return net_addr_error_as_error(&e, error_allocator());
}

/* ----------------------------------------------------------------------- IP */

static const Byte net_v4_in_v6_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

#define NET_V4(a, b, c, d) {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d}

static const Byte net_ipv4_bcast_data[16] = NET_V4(255, 255, 255, 255);
static const Byte net_ipv4_allsys_data[16] = NET_V4(224, 0, 0, 1);
static const Byte net_ipv4_allrouter_data[16] = NET_V4(224, 0, 0, 2);
static const Byte net_ipv4_zero_data[16] = NET_V4(0, 0, 0, 0);
static const Byte net_ipv6_zero_data[16] = {0};
static const Byte net_ipv6_unspecified_data[16] = {0};
static const Byte net_ipv6_loopback_data[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                0, 0, 0, 0, 0, 0, 0, 1};
static const Byte net_ipv6_ilan_data[16] = {0xff, 0x01, 0, 0, 0, 0, 0, 0,
                                            0,    0,    0, 0, 0, 0, 0, 0x01};
static const Byte net_ipv6_llan_data[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0,
                                            0,    0,    0, 0, 0, 0, 0, 0x01};
static const Byte net_ipv6_llar_data[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0,
                                            0,    0,    0, 0, 0, 0, 0, 0x02};

#define NET_IP16(data) {(void *)(uintptr_t)(data), 16, 16, TYPE_BYTE}

const NetIP net_ipv4_bcast = NET_IP16(net_ipv4_bcast_data);
const NetIP net_ipv4_allsys = NET_IP16(net_ipv4_allsys_data);
const NetIP net_ipv4_allrouter = NET_IP16(net_ipv4_allrouter_data);
const NetIP net_ipv4_zero = NET_IP16(net_ipv4_zero_data);
const NetIP net_ipv6_zero = NET_IP16(net_ipv6_zero_data);
const NetIP net_ipv6_unspecified = NET_IP16(net_ipv6_unspecified_data);
const NetIP net_ipv6_loopback = NET_IP16(net_ipv6_loopback_data);
const NetIP net_ipv6_interfacelocalallnodes = NET_IP16(net_ipv6_ilan_data);
const NetIP net_ipv6_linklocalallnodes = NET_IP16(net_ipv6_llan_data);
const NetIP net_ipv6_linklocalallrouters = NET_IP16(net_ipv6_llar_data);

NetIP net_ipv4(Alloc *a, Byte a0, Byte b, Byte c, Byte d) {
    NetIP p = net_make(a, NET_IPV6_LEN);
    if (p.p == NULL)
        return p;
    Byte *q = (Byte *)p.p;
    memcpy(q, net_v4_in_v6_prefix, sizeof net_v4_in_v6_prefix);
    q[12] = a0;
    q[13] = b;
    q[14] = c;
    q[15] = d;
    return p;
}

NetIPMask net_ipv4_mask(Alloc *a, Byte a0, Byte b, Byte c, Byte d) {
    NetIPMask p = net_make(a, NET_IPV4_LEN);
    if (p.p == NULL)
        return p;
    Byte *q = (Byte *)p.p;
    q[0] = a0;
    q[1] = b;
    q[2] = c;
    q[3] = d;
    return p;
}

NetIPMask net_cidr_mask(Alloc *a, Int ones, Int bits) {
    if (bits != (Int)8 * NET_IPV4_LEN && bits != (Int)8 * NET_IPV6_LEN)
        return slice_nil(TYPE_BYTE);
    if (ones < 0 || ones > bits)
        return slice_nil(TYPE_BYTE);
    Int l = bits / 8;
    NetIPMask m = net_make(a, l);
    if (m.p == NULL)
        return m;
    Byte *q = (Byte *)m.p;
    Int n = ones;
    for (Int i = 0; i < l; i++) {
        if (n >= 8) {
            q[i] = 0xff;
            n -= 8;
            continue;
        }
        q[i] = (Byte) ~(0xffU >> n);
        n = 0;
    }
    return m;
}

static bool net_eq(const Byte *x, const Byte *y, Int n) {
    return n == 0 || memcmp(x, y, (size_t)n) == 0;
}

bool net_ip_equal(NetIP ip, NetIP x) {
    const Byte *p = net_bytes(ip);
    const Byte *q = net_bytes(x);
    if (ip.len == x.len)
        return net_eq(p, q, ip.len);
    if (ip.len == NET_IPV4_LEN && x.len == NET_IPV6_LEN)
        return net_eq(q, net_v4_in_v6_prefix, 12) && net_eq(p, q + 12, 4);
    if (ip.len == NET_IPV6_LEN && x.len == NET_IPV4_LEN)
        return net_eq(p, net_v4_in_v6_prefix, 12) && net_eq(p + 12, q, 4);
    return false;
}

static bool net_is_zeros(const Byte *p, Int n) {
    for (Int i = 0; i < n; i++) {
        if (p[i] != 0)
            return false;
    }
    return true;
}

NetIP net_ip_to4(NetIP ip) {
    if (ip.len == NET_IPV4_LEN)
        return ip;
    const Byte *p = net_bytes(ip);
    if (ip.len == NET_IPV6_LEN && net_is_zeros(p, 10) && p[10] == 0xff && p[11] == 0xff)
        return slice_sub(ip, 12, 16);
    return slice_nil(TYPE_BYTE);
}

NetIP net_ip_to16(NetIP ip, Alloc *a) {
    const Byte *p = net_bytes(ip);
    if (ip.len == NET_IPV4_LEN)
        return net_ipv4(a, p[0], p[1], p[2], p[3]);
    if (ip.len == NET_IPV6_LEN)
        return ip;
    return slice_nil(TYPE_BYTE);
}

bool net_ip_is_unspecified(NetIP ip) {
    return net_ip_equal(ip, net_ipv4_zero) || net_ip_equal(ip, net_ipv6_unspecified);
}

bool net_ip_is_loopback(NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    if (ip4.len != 0)
        return net_bytes(ip4)[0] == 127;
    return net_ip_equal(ip, net_ipv6_loopback);
}

bool net_ip_is_private(NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    if (ip4.len != 0) {
        /* RFC 1918, section 3: 10/8, 172.16/12 and 192.168/16. */
        const Byte *p = net_bytes(ip4);
        return p[0] == 10 || (p[0] == 172 && (p[1] & 0xf0) == 16) ||
               (p[0] == 192 && p[1] == 168);
    }
    /* RFC 4193, section 8: fc00::/7. */
    return ip.len == NET_IPV6_LEN && (net_bytes(ip)[0] & 0xfe) == 0xfc;
}

bool net_ip_is_multicast(NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    if (ip4.len != 0)
        return (net_bytes(ip4)[0] & 0xf0) == 0xe0;
    return ip.len == NET_IPV6_LEN && net_bytes(ip)[0] == 0xff;
}

bool net_ip_is_interface_local_multicast(NetIP ip) {
    const Byte *p = net_bytes(ip);
    return ip.len == NET_IPV6_LEN && p[0] == 0xff && (p[1] & 0x0f) == 0x01;
}

bool net_ip_is_link_local_multicast(NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    if (ip4.len != 0) {
        const Byte *p = net_bytes(ip4);
        return p[0] == 224 && p[1] == 0 && p[2] == 0;
    }
    const Byte *p = net_bytes(ip);
    return ip.len == NET_IPV6_LEN && p[0] == 0xff && (p[1] & 0x0f) == 0x02;
}

bool net_ip_is_link_local_unicast(NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    if (ip4.len != 0) {
        const Byte *p = net_bytes(ip4);
        return p[0] == 169 && p[1] == 254;
    }
    const Byte *p = net_bytes(ip);
    return ip.len == NET_IPV6_LEN && p[0] == 0xfe && (p[1] & 0xc0) == 0x80;
}

bool net_ip_is_global_unicast(NetIP ip) {
    return (ip.len == NET_IPV4_LEN || ip.len == NET_IPV6_LEN) &&
           !net_ip_equal(ip, net_ipv4_bcast) && !net_ip_is_unspecified(ip) &&
           !net_ip_is_loopback(ip) && !net_ip_is_multicast(ip) &&
           !net_ip_is_link_local_unicast(ip);
}

static const Byte net_class_a_data[4] = {0xff, 0, 0, 0};
static const Byte net_class_b_data[4] = {0xff, 0xff, 0, 0};
static const Byte net_class_c_data[4] = {0xff, 0xff, 0xff, 0};

NetIPMask net_ip_default_mask(NetIP ip) {
    ip = net_ip_to4(ip);
    if (ip.len == 0)
        return slice_nil(TYPE_BYTE);
    Byte b = net_bytes(ip)[0];
    if (b < 0x80)
        return net_view(net_class_a_data, 4);
    if (b < 0xC0)
        return net_view(net_class_b_data, 4);
    return net_view(net_class_c_data, 4);
}

static bool net_all_ff(const Byte *p, Int n) {
    for (Int i = 0; i < n; i++) {
        if (p[i] != 0xff)
            return false;
    }
    return true;
}

NetIP net_ip_mask(NetIP ip, Alloc *a, NetIPMask mask) {
    if (mask.len == NET_IPV6_LEN && ip.len == NET_IPV4_LEN &&
        net_all_ff(net_bytes(mask), 12))
        mask = slice_sub(mask, 12, 16);
    if (mask.len == NET_IPV4_LEN && ip.len == NET_IPV6_LEN &&
        net_eq(net_bytes(ip), net_v4_in_v6_prefix, 12))
        ip = slice_sub(ip, 12, 16);
    Int n = ip.len;
    if (n != mask.len)
        return slice_nil(TYPE_BYTE);
    NetIP out = net_make(a, n);
    if (out.p == NULL)
        return out;
    Byte *o = (Byte *)out.p;
    const Byte *p = net_bytes(ip);
    const Byte *m = net_bytes(mask);
    for (Int i = 0; i < n; i++)
        o[i] = p[i] & m[i];
    return out;
}

static const char net_hex_digit[] = "0123456789abcdef";

static void net_hex_into(Byte *q, const Byte *p, Int n) {
    for (Int i = 0; i < n; i++) {
        q[2 * i] = (Byte)net_hex_digit[p[i] >> 4];
        q[2 * i + 1] = (Byte)net_hex_digit[p[i] & 0xf];
    }
}

static Str net_hex_string(Alloc *a, Slice b, bool question) {
    Int n = (question ? 1 : 0) + 2 * b.len;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (q == NULL)
        return BURROW_STR_EMPTY;
    Byte *p = q;
    if (question)
        *p++ = '?';
    net_hex_into(p, net_bytes(b), b.len);
    return str_from_bytes(q, n);
}

/* ip.appendTo: through netip, after shrinking an IPv4-mapped address to 4
 * bytes so that it prints in dotted decimal. ip is 4 or 16 bytes. */
static Slice net_ip_append_to(NetIP ip, Alloc *a, Slice b) {
    NetIP p4 = net_ip_to4(ip);
    if (p4.len == NET_IPV4_LEN)
        ip = p4;
    bool ok = false;
    NetipAddr addr = netip_addr_from_slice(ip, &ok);
    return netip_addr_append_to(addr, a, b);
}

Str net_ip_string(NetIP ip, Alloc *a) {
    if (ip.len == 0)
        return net_copy(a, "<nil>", 5);
    if (ip.len != NET_IPV4_LEN && ip.len != NET_IPV6_LEN)
        return net_hex_string(a, ip, true);
    /* "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" is the longest. */
    Slice buf = slice_make(a, TYPE_BYTE, 0, ip.len == NET_IPV4_LEN ? 15 : 39);
    if (buf.p == NULL)
        return BURROW_STR_EMPTY;
    buf = net_ip_append_to(ip, a, buf);
    return str_from_bytes(buf.p, buf.len);
}

Slice net_ip_append_text(NetIP ip, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (ip.len == 0)
        return b;
    if (ip.len != NET_IPV4_LEN && ip.len != NET_IPV6_LEN) {
        Str addr = net_hex_string(error_allocator(), ip, false);
        BURROW_OUT(err, net_addr_err(NET_LIT("invalid IP address"), addr));
        return b;
    }
    return net_ip_append_to(ip, a, b);
}

Slice net_ip_marshal_text(NetIP ip, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    /* 24 holds every IPv4 address and the short IPv6 ones. */
    Slice b = net_ip_append_text(ip, a, slice_make(a, TYPE_BYTE, 0, 24), &e);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return slice_nil(TYPE_BYTE);
    return b;
}

/* parseIP: s through netip, refusing a zone. */
static bool net_parse_ip16(Str s, Byte out[16]) {
    Error err = BURROW_NO_ERROR;
    NetipAddr ip = netip_parse_addr(s, &err);
    if (BURROW_FAILED(err) || netip_addr_zone(ip).len != 0)
        return false;
    NetipAddrAs16Ret r = netip_addr_as16(ip);
    memcpy(out, r.a, 16);
    return true;
}

NetIP net_parse_ip(Alloc *a, Str s) {
    Byte b[16];
    if (!net_parse_ip16(s, b))
        return slice_nil(TYPE_BYTE);
    NetIP ip = net_make(a, NET_IPV6_LEN);
    if (ip.p != NULL)
        memcpy(ip.p, b, 16);
    return ip;
}

Error net_ip_unmarshal_text(NetIP *ip, Alloc *a, Slice text) {
    if (text.len == 0) {
        *ip = slice_nil(TYPE_BYTE);
        return BURROW_NO_ERROR;
    }
    Str s = str_from_bytes(text.p, text.len);
    NetIP x = net_parse_ip(a, s);
    if (x.len == 0)
        return net_parse_err(NET_LIT("IP address"), s);
    *ip = x;
    return BURROW_NO_ERROR;
}

NetIP net_parse_cidr(Alloc *a, Str s, NetIPNet **net_out, Error *err) {
    NetIP r = slice_nil(TYPE_BYTE);
    BURROW_OUT(net_out, NULL);
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int slash = -1;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == '/') {
            slash = i;
            break;
        }
    }
    Str addr = str_from_bytes(s.p, slash < 0 ? s.len : slash);
    Str mask = slash < 0 ? BURROW_STR_EMPTY
                         : str_from_bytes(s.p + slash + 1, s.len - slash - 1);
    Error perr = BURROW_NO_ERROR;
    NetipAddr ip = slash < 0 ? (NetipAddr){0} : netip_parse_addr(addr, &perr);
    Int n = 0;
    Int used = 0;
    bool ok = slash >= 0 && BURROW_OK(perr) && netip_addr_zone(ip).len == 0 &&
              burrow__net_dtoi(mask, &n, &used) && used == mask.len && n >= 0 &&
              n <= netip_addr_bit_len(ip);
    if (!ok) {
        BURROW_OUT(err, net_parse_err(NET_LIT("CIDR address"), s));
        return r;
    }
    Int bits = netip_addr_bit_len(ip);
    NetIPMask m = net_cidr_mask(a, n, bits);
    NetipAddrAs16Ret a16 = netip_addr_as16(ip);
    NetIP full = net_make(a, NET_IPV6_LEN);
    NetIPNet *net = (NetIPNet *)mem_alloc(a, sizeof(NetIPNet), _Alignof(NetIPNet));
    if (m.p == NULL || full.p == NULL || net == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return r;
    }
    memcpy(full.p, a16.a, 16);
    net->ip = net_ip_mask(full, a, m);
    net->mask = m;
    BURROW_OUT(net_out, net);
    return full;
}

/* ------------------------------------------------------------------- IPMask */

/* simpleMaskLength: the number of 1 bits when mask is 1 bits and then 0 bits,
 * and -1 otherwise. */
static Int net_simple_mask_length(NetIPMask mask) {
    const Byte *m = net_bytes(mask);
    Int n = 0;
    for (Int i = 0; i < mask.len; i++) {
        unsigned v = m[i];
        if (v == 0xff) {
            n += 8;
            continue;
        }
        while ((v & 0x80) != 0) {
            n++;
            v = (v << 1) & 0xff;
        }
        if (v != 0)
            return -1;
        for (i++; i < mask.len; i++) {
            if (m[i] != 0)
                return -1;
        }
        break;
    }
    return n;
}

Int net_ip_mask_size(NetIPMask m, Int *bits) {
    Int ones = net_simple_mask_length(m);
    if (ones == -1) {
        BURROW_OUT(bits, 0);
        return 0;
    }
    BURROW_OUT(bits, m.len * 8);
    return ones;
}

Str net_ip_mask_string(NetIPMask m, Alloc *a) {
    if (m.len == 0)
        return net_copy(a, "<nil>", 5);
    return net_hex_string(a, m, false);
}

/* -------------------------------------------------------------------- IPNet */

/* networkNumberAndMask: n's address and mask brought to the same length, or
 * false when they cannot be. */
static bool net_number_and_mask(const NetIPNet *n, NetIP *ip, NetIPMask *m) {
    *ip = net_ip_to4(n->ip);
    if (ip->len == 0) {
        *ip = n->ip;
        if (ip->len != NET_IPV6_LEN)
            return false;
    }
    *m = n->mask;
    switch (m->len) {
    case NET_IPV4_LEN:
        if (ip->len != NET_IPV4_LEN)
            return false;
        break;
    case NET_IPV6_LEN:
        if (ip->len == NET_IPV4_LEN)
            *m = slice_sub(*m, 12, 16);
        break;
    default:
        return false;
    }
    return true;
}

bool burrow__net_number_and_mask(const NetIPNet *n, NetIP *ip, NetIPMask *m);

/* For the tests, which check networkNumberAndMask directly as Go's do. */
bool burrow__net_number_and_mask(const NetIPNet *n, NetIP *ip, NetIPMask *m) {
    if (net_number_and_mask(n, ip, m))
        return true;
    *ip = slice_nil(TYPE_BYTE);
    *m = slice_nil(TYPE_BYTE);
    return false;
}

bool net_ip_net_contains(const NetIPNet *n, NetIP ip) {
    NetIP nn;
    NetIPMask m;
    if (!net_number_and_mask(n, &nn, &m))
        nn = m = slice_nil(TYPE_BYTE);
    NetIP x = net_ip_to4(ip);
    if (x.len != 0)
        ip = x;
    Int l = ip.len;
    if (l != nn.len)
        return false;
    const Byte *p = net_bytes(ip);
    const Byte *q = net_bytes(nn);
    const Byte *mm = net_bytes(m);
    for (Int i = 0; i < l; i++) {
        if ((q[i] & mm[i]) != (p[i] & mm[i]))
            return false;
    }
    return true;
}

Str net_ip_net_network(const NetIPNet *n) {
    (void)n;
    return NET_LIT("ip+net");
}

Str net_ip_net_string(const NetIPNet *n, Alloc *a) {
    NetIP nn;
    NetIPMask m;
    if (n == NULL || !net_number_and_mask(n, &nn, &m) || nn.len == 0 || m.len == 0)
        return net_copy(a, "<nil>", 5);
    Int l = net_simple_mask_length(m);
    /* The address, a slash and either the length or the mask in hex. */
    Slice buf = slice_make(a, TYPE_BYTE, 0, 39 + 1 + 32);
    if (buf.p == NULL)
        return BURROW_STR_EMPTY;
    buf = net_ip_append_to(nn, a, buf);
    Byte tail[1 + 32];
    Int k = 0;
    tail[k++] = '/';
    if (l == -1) {
        net_hex_into(tail + k, net_bytes(m), m.len);
        k += 2 * m.len;
    } else {
        Byte d[4];
        Int nd = 0;
        do {
            d[nd++] = (Byte)('0' + l % 10);
            l /= 10;
        } while (l > 0);
        while (nd > 0)
            tail[k++] = d[--nd];
    }
    buf = slice_append(a, buf, tail, k);
    return str_from_bytes(buf.p, buf.len);
}

/* -------------------------------------------------------------- host:port */

static Int net_index_byte(Str s, Int from, Byte c) {
    for (Int i = from; i < s.len; i++) {
        if (s.p[i] == c)
            return i;
    }
    return -1;
}

Str net_split_host_port(Str hostport, Str *port, Error *err) {
    BURROW_OUT(port, BURROW_STR_EMPTY);
    BURROW_OUT(err, BURROW_NO_ERROR);
    const char *why = NULL;
    Int j = 0;
    Int k = 0;
    Str host = BURROW_STR_EMPTY;

    /* The port starts after the last colon. */
    Int i = -1;
    for (Int x = hostport.len - 1; x >= 0; x--) {
        if (hostport.p[x] == ':') {
            i = x;
            break;
        }
    }
    if (i < 0) {
        why = "missing port in address";
        goto fail;
    }
    if (hostport.p[0] == '[') {
        /* Expect the first ']' just before the last ':'. */
        Int end = net_index_byte(hostport, 0, ']');
        if (end < 0) {
            why = "missing ']' in address";
            goto fail;
        }
        if (end + 1 == hostport.len) {
            /* There can't be a ':' behind the ']' now. */
            why = "missing port in address";
            goto fail;
        }
        if (end + 1 != i) {
            /* Either ']' isn't followed by a colon, or it is followed by a
             * colon that is not the last one. */
            why = hostport.p[end + 1] == ':' ? "too many colons in address"
                                             : "missing port in address";
            goto fail;
        }
        host = str_from_bytes(hostport.p + 1, end - 1);
        j = 1;
        k = end + 1; /* there can't be a '[' resp. ']' before these positions */
    } else {
        host = str_from_bytes(hostport.p, i);
        if (net_index_byte(host, 0, ':') >= 0) {
            why = "too many colons in address";
            goto fail;
        }
    }
    if (net_index_byte(hostport, j, '[') >= 0) {
        why = "unexpected '[' in address";
        goto fail;
    }
    if (net_index_byte(hostport, k, ']') >= 0) {
        why = "unexpected ']' in address";
        goto fail;
    }
    BURROW_OUT(port, str_from_bytes(hostport.p + i + 1, hostport.len - i - 1));
    return host;

fail:
    BURROW_OUT(err, net_addr_err(str_from_cstr(why), hostport));
    return BURROW_STR_EMPTY;
}

Str net_join_host_port(Alloc *a, Str host, Str port) {
    /* A host with a colon in it is taken to be an IPv6 literal. */
    bool v6 = net_index_byte(host, 0, ':') >= 0;
    Int n = host.len + 1 + port.len + (v6 ? 2 : 0);
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (q == NULL)
        return BURROW_STR_EMPTY;
    Byte *p = q;
    if (v6)
        *p++ = '[';
    p = net_put(p, host.p, host.len);
    if (v6)
        *p++ = ']';
    *p++ = ':';
    net_put(p, port.p, port.len);
    return str_from_bytes(q, n);
}

/* ------------------------------------------------------------- descriptors
 *
 * The methods take the receiver by pointer, which is the shape a descriptor
 * wants. String puts its text in the goroutine's error arena, since it has no
 * allocator to take. */

static Slice net_ip_m_append_text(NetIP *self, Alloc *a, Slice b, Error *err) {
    return net_ip_append_text(*self, a, b, err);
}

static Slice net_ip_m_marshal_text(NetIP *self, Alloc *a, Error *err) {
    return net_ip_marshal_text(*self, a, err);
}

static Str net_ip_m_string(NetIP *self) {
    return net_ip_string(*self, error_allocator());
}

static Error net_ip_m_unmarshal_text(NetIP *self, Alloc *a, Slice data) {
    return net_ip_unmarshal_text(self, a, data);
}

static Str net_ip_mask_m_string(NetIPMask *self) {
    return net_ip_mask_string(*self, error_allocator());
}

static Str net_ip_net_m_network(NetIPNet *self) {
    return net_ip_net_network(self);
}

static Str net_ip_net_m_string(NetIPNet *self) {
    return net_ip_net_string(self, error_allocator());
}

#define NET_SIG_STRING(IN, OUT) OUT(Str)

#define NET_IP_METHODS(M, T)                                                           \
    M(T, AppendText, net_ip_m_append_text, ENCODING_SIG_APPEND_TEXT)                   \
    M(T, MarshalText, net_ip_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                \
    M(T, String, net_ip_m_string, NET_SIG_STRING)                                      \
    M(T, UnmarshalText, net_ip_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

#define NET_IP_MASK_METHODS(M, T) M(T, String, net_ip_mask_m_string, NET_SIG_STRING)

#define NET_IP_NET_METHODS(M, T)                                                       \
    M(T, Network, net_ip_net_m_network, NET_SIG_STRING)                                \
    M(T, String, net_ip_net_m_string, NET_SIG_STRING)

BURROW_METHODS_DEFINE(NetIP, NET_IP_METHODS);
BURROW_METHODS_DEFINE(NetIPMask, NET_IP_MASK_METHODS);
BURROW_METHODS_DEFINE(NetIPNet, NET_IP_NET_METHODS);

const Type burrow_type_NetIP = {
    BURROW_S_INIT("IP"),
    BURROW_S_INIT("net"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    NET_COUNT(burrow__methods_NetIP),
    NULL,
    burrow__methods_NetIP,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_NetIPMask = {
    BURROW_S_INIT("IPMask"),
    BURROW_S_INIT("net"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    NET_COUNT(burrow__methods_NetIPMask),
    NULL,
    burrow__methods_NetIPMask,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};

static const Field net_ip_net_fields[] = {
    {BURROW_S_INIT("IP"),
     {NULL, 0},
     &burrow_type_NetIP,
     (uint32_t)offsetof(NetIPNet, ip)},
    {BURROW_S_INIT("Mask"),
     {NULL, 0},
     &burrow_type_NetIPMask,
     (uint32_t)offsetof(NetIPNet, mask)},
};

const Type burrow_type_NetIPNet = {
    BURROW_S_INIT("IPNet"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetIPNet),
    (uint16_t)_Alignof(NetIPNet),
    NET_COUNT(net_ip_net_fields),
    NET_COUNT(burrow__methods_NetIPNet),
    net_ip_net_fields,
    burrow__methods_NetIPNet,
    NULL,
    NULL,
    0,
    0,
    NULL,
};
