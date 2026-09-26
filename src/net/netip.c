/* Derived from Go's src/net/netip/netip.go and uint128.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/netip.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/unique.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Go keeps the family and the zone in a unique.Handle[addrDetail], with two
 * handles made at start up for IPv4 and for IPv6 without a zone. Here those
 * two are static objects, and a zone is the handle of the zone string, which
 * is never empty, so the three kinds of pointer never meet. The two point at
 * different bytes so that a linker folding identical constants cannot merge
 * them, and both have length zero so a zone read through either is "". */
const Str burrow__netip_z4 = {(const Byte *)"4", 0};
const Str burrow__netip_z6noz = {(const Byte *)"6", 0};

#define NIP_Z4 ((UniqueHandle){&burrow__netip_z4})
#define NIP_Z6NOZ ((UniqueHandle){&burrow__netip_z6noz})

static bool nip_has_zone(NetipAddr ip) {
    return ip.z.value != NULL && ip.z.value != &burrow__netip_z4 &&
           ip.z.value != &burrow__netip_z6noz;
}

/* ------------------------------------------------------------------ uint128
 *
 * Go's shifts by 64 or more give 0, and C's are undefined, so the mask is
 * built with the edge cases spelled out. */

/* mask6: the top n bits of 128 set, for n from 0 to 128. */
static void nip_mask6(Int n, uint64_t *hi, uint64_t *lo) {
    *hi = n >= 64 ? ~(uint64_t)0 : ~(~(uint64_t)0 >> n);
    *lo = n <= 64 ? 0 : ~(uint64_t)0 << (128 - n);
}

/* --------------------------------------------------------------- errors */

typedef struct NipPart {
    Str s;
    bool quote;
} NipPart;

/* The parts put together in the error arena, with the quoted ones quoted the
 * way strconv.Quote does. */
static Error nip_error(const NipPart *parts, int n) {
    Int len = 0;
    for (int i = 0; i < n; i++)
        len += parts[i].quote ? burrow__strconv_quote_into(NULL, parts[i].s)
                              : parts[i].s.len;
    Alloc *a = error_allocator();
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)(len > 0 ? len : 1), 1);
    if (buf == NULL)
        return burrow_err_out_of_memory;
    Byte *p = buf;
    for (int i = 0; i < n; i++) {
        if (parts[i].quote) {
            p += burrow__strconv_quote_into(p, parts[i].s);
        } else if (parts[i].s.len > 0) {
            memcpy(p, parts[i].s.p, (size_t)parts[i].s.len);
            p += parts[i].s.len;
        }
    }
    Error e = errors_new(a, str_from_bytes(buf, len));
    mem_free(a, buf, (size_t)(len > 0 ? len : 1), 1);
    return e;
}

#define NIP_LIT(s) ((NipPart){BURROW_S(s), false})
#define NIP_STR(s) ((NipPart){(s), false})
#define NIP_Q(s) ((NipPart){(s), true})

/* Go's parseAddrError, whose text is built here rather than on demand. */
static Error nip_parse_error(Str in, const char *msg, Str at) {
    NipPart parts[] = {
        NIP_LIT("ParseAddr("), NIP_Q(in), NIP_LIT("): "), NIP_STR(str_from_cstr(msg)),
        NIP_LIT(" (at "),      NIP_Q(at), NIP_LIT(")"),
    };
    return nip_error(parts, at.len > 0 ? 7 : 4);
}

static Error nip_errors_new(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

/* ---------------------------------------------------------- constructors */

NetipAddr netip_addr_from4(const Byte addr[4]) {
    NetipAddr ip = {0,
                    0xffff00000000ULL | (uint64_t)addr[0] << 24 |
                        (uint64_t)addr[1] << 16 | (uint64_t)addr[2] << 8 |
                        (uint64_t)addr[3],
                    NIP_Z4};
    return ip;
}

static uint64_t nip_be64(const Byte *b) {
    return (uint64_t)b[0] << 56 | (uint64_t)b[1] << 48 | (uint64_t)b[2] << 40 |
           (uint64_t)b[3] << 32 | (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 |
           (uint64_t)b[6] << 8 | (uint64_t)b[7];
}

static void nip_put_be64(Byte *b, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        b[i] = (Byte)v;
        v >>= 8;
    }
}

NetipAddr netip_addr_from16(const Byte addr[16]) {
    NetipAddr ip = {nip_be64(addr), nip_be64(addr + 8), NIP_Z6NOZ};
    return ip;
}

NetipAddr netip_addr_from_slice(Slice slice, bool *ok) {
    NetipAddr ip = {0, 0, {NULL}};
    bool good = true;
    if (slice.p == NULL)
        good = false;
    else if (slice.len == 4)
        ip = netip_addr_from4((const Byte *)slice.p);
    else if (slice.len == 16)
        ip = netip_addr_from16((const Byte *)slice.p);
    else
        good = false;
    if (ok != NULL)
        *ok = good;
    return ip;
}

NetipAddr netip_ipv6_link_local_all_nodes(void) {
    NetipAddr ip = {0xff02ULL << 48, 1, NIP_Z6NOZ};
    return ip;
}

NetipAddr netip_ipv6_link_local_all_routers(void) {
    NetipAddr ip = {0xff02ULL << 48, 2, NIP_Z6NOZ};
    return ip;
}

NetipAddr netip_ipv6_loopback(void) {
    NetipAddr ip = {0, 1, NIP_Z6NOZ};
    return ip;
}

NetipAddr netip_ipv6_unspecified(void) {
    NetipAddr ip = {0, 0, NIP_Z6NOZ};
    return ip;
}

NetipAddr netip_ipv4_unspecified(void) {
    NetipAddr ip = {0, 0xffff00000000ULL, NIP_Z4};
    return ip;
}

NetipAddr netip_addr_with_zone(NetipAddr ip, Str zone) {
    if (!netip_addr_is6(ip))
        return ip;
    if (zone.len == 0) {
        ip.z = NIP_Z6NOZ;
        return ip;
    }
    ip.z = unique_make(TYPE_STRING, &zone);
    return ip;
}

static NetipAddr nip_without_zone(NetipAddr ip) {
    if (netip_addr_is6(ip))
        ip.z = NIP_Z6NOZ;
    return ip;
}

/* ---------------------------------------------------------------- parsing */

/* parseIPv4Fields: the dotted quad in in[off:end] into fields. */
static BURROW_INLINE Error nip_parse4_fields(Str in, Int off, Int end, Byte *fields) {
    Int val = 0, pos = 0, dig_len = 0;
    const Byte *s = in.p + off;
    Int n = end - off;
    for (Int i = 0; i < n; i++) {
        Byte c = s[i];
        if (c >= '0' && c <= '9') {
            if (dig_len == 1 && val == 0)
                return nip_parse_error(in, "IPv4 field has octet with leading zero",
                                       BURROW_STR_EMPTY);
            val = val * 10 + (Int)(c - '0');
            dig_len++;
            if (val > 255)
                return nip_parse_error(in, "IPv4 field has value >255",
                                       BURROW_STR_EMPTY);
        } else if (c == '.') {
            if (i == 0 || i == n - 1 || s[i - 1] == '.')
                return nip_parse_error(in, "IPv4 field must have at least one digit",
                                       str_from_bytes(s + i, n - i));
            if (pos == 3)
                return nip_parse_error(in, "IPv4 address too long", BURROW_STR_EMPTY);
            fields[pos++] = (Byte)val;
            val = 0;
            dig_len = 0;
        } else {
            return nip_parse_error(in, "unexpected character",
                                   str_from_bytes(s + i, n - i));
        }
    }
    if (pos < 3)
        return nip_parse_error(in, "IPv4 address too short", BURROW_STR_EMPTY);
    fields[3] = (Byte)val;
    return BURROW_NO_ERROR;
}

static BURROW_INLINE NetipAddr nip_parse4(Str s, Error *err) {
    Byte fields[4] = {0};
    Error e = nip_parse4_fields(s, 0, s.len, fields);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (NetipAddr){0, 0, {NULL}};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_addr_from4(fields);
}

static NetipAddr nip_fail(Error *err, Error e) {
    BURROW_OUT(err, e);
    return (NetipAddr){0, 0, {NULL}};
}

static Str nip_rest(const Byte *p, Int n) {
    return str_from_bytes(p, n);
}

static NetipAddr nip_parse6(Str in, Error *err) {
    const Byte *s = in.p;
    Int n = in.len;

    /* The zone is split off first, as Go does. */
    Str zone = BURROW_STR_EMPTY;
    const Byte *pct = n > 0 ? (const Byte *)memchr(s, '%', (size_t)n) : NULL;
    if (pct != NULL) {
        zone = str_from_bytes(pct + 1, n - (Int)(pct - s) - 1);
        n = (Int)(pct - s);
        if (zone.len == 0)
            return nip_fail(err, nip_parse_error(in, "zone must be a non-empty string",
                                                 BURROW_STR_EMPTY));
    }

    Byte ip[16];
    memset(ip, 0, sizeof ip);
    Int ellipsis = -1;

    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        ellipsis = 0;
        s += 2;
        n -= 2;
        if (n == 0) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return netip_addr_with_zone(netip_ipv6_unspecified(), zone);
        }
    }

    Int i = 0;
    while (i < 16) {
        Int off = 0;
        uint32_t acc = 0;
        for (; off < n; off++) {
            Byte c = s[off];
            if (c >= '0' && c <= '9')
                acc = (acc << 4) + (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                acc = (acc << 4) + (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                acc = (acc << 4) + (uint32_t)(c - 'A' + 10);
            else
                break;
            if (off > 3)
                return nip_fail(
                    err, nip_parse_error(in, "each group must have 4 or less digits",
                                         nip_rest(s, n)));
            if (acc > 0xffff)
                return nip_fail(err, nip_parse_error(in, "IPv6 field has value >=2^16",
                                                     nip_rest(s, n)));
        }
        if (off == 0)
            return nip_fail(
                err, nip_parse_error(
                         in, "each colon-separated field must have at least one digit",
                         nip_rest(s, n)));

        /* A dot after the digits means a trailing IPv4 address. */
        if (off < n && s[off] == '.') {
            if (ellipsis < 0 && i != 12)
                return nip_fail(
                    err, nip_parse_error(in,
                                         "embedded IPv4 address must replace the "
                                         "final 2 fields of the address",
                                         nip_rest(s, n)));
            if (i + 4 > 16)
                return nip_fail(
                    err, nip_parse_error(in,
                                         "too many hex fields to fit an embedded "
                                         "IPv4 at the end of the address",
                                         nip_rest(s, n)));
            Int end = in.len;
            if (zone.len > 0)
                end -= zone.len + 1;
            Error e = nip_parse4_fields(in, end - n, end, ip + i);
            if (BURROW_FAILED(e))
                return nip_fail(err, e);
            n = 0;
            i += 4;
            break;
        }

        ip[i] = (Byte)(acc >> 8);
        ip[i + 1] = (Byte)acc;
        i += 2;

        s += off;
        n -= off;
        if (n == 0)
            break;

        if (s[0] != ':')
            return nip_fail(err, nip_parse_error(in, "unexpected character, want colon",
                                                 nip_rest(s, n)));
        if (n == 1)
            return nip_fail(
                err, nip_parse_error(in, "colon must be followed by more characters",
                                     nip_rest(s, n)));
        s++;
        n--;

        if (s[0] == ':') {
            if (ellipsis >= 0)
                return nip_fail(
                    err, nip_parse_error(in, "multiple :: in address", nip_rest(s, n)));
            ellipsis = i;
            s++;
            n--;
            if (n == 0)
                break;
        }
    }

    if (n != 0)
        return nip_fail(
            err, nip_parse_error(in, "trailing garbage after address", nip_rest(s, n)));

    if (i < 16) {
        if (ellipsis < 0)
            return nip_fail(
                err, nip_parse_error(in, "address string too short", BURROW_STR_EMPTY));
        Int k = 16 - i;
        for (Int j = i - 1; j >= ellipsis; j--)
            ip[j + k] = ip[j];
        memset(ip + ellipsis, 0, (size_t)k);
    } else if (ellipsis >= 0) {
        return nip_fail(
            err,
            nip_parse_error(in, "the :: must expand to at least one field of zeros",
                            BURROW_STR_EMPTY));
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_addr_with_zone(netip_addr_from16(ip), zone);
}

NetipAddr netip_parse_addr(Str s, Error *err) {
    for (Int i = 0; i < s.len; i++) {
        switch (s.p[i]) {
        case '.':
            return nip_parse4(s, err);
        case ':':
            return nip_parse6(s, err);
        case '%':
            return nip_fail(
                err, nip_parse_error(s, "missing IPv6 address", BURROW_STR_EMPTY));
        default:
            break;
        }
    }
    return nip_fail(err, nip_parse_error(s, "unable to parse IP", BURROW_STR_EMPTY));
}

static BURROW_NORETURN void nip_panic(Error e) {
    panic(BURROW_ANY(TYPE_ERROR, &e));
}

NetipAddr netip_must_parse_addr(Str s) {
    Error err;
    NetipAddr ip = netip_parse_addr(s, &err);
    if (BURROW_FAILED(err))
        nip_panic(err);
    return ip;
}

/* ---------------------------------------------------------------- queries */

static BURROW_INLINE Byte nip_v4(NetipAddr ip, int i) {
    return (Byte)(ip.lo >> ((3 - i) * 8));
}

static BURROW_INLINE uint16_t nip_v6u16(NetipAddr ip, int i) {
    uint64_t half = i < 4 ? ip.hi : ip.lo;
    return (uint16_t)(half >> ((3 - i % 4) * 16));
}

static int nip_cmp_u64(uint64_t a, uint64_t b) {
    return (a > b) - (a < b);
}

Int netip_addr_compare(NetipAddr ip, NetipAddr ip2) {
    Int f1 = netip_addr_bit_len(ip), f2 = netip_addr_bit_len(ip2);
    if (f1 != f2)
        return f1 < f2 ? -1 : 1;
    int c = nip_cmp_u64(ip.hi, ip2.hi);
    if (c != 0)
        return c;
    c = nip_cmp_u64(ip.lo, ip2.lo);
    if (c != 0)
        return c;
    if (netip_addr_is6(ip) && ip.z.value != ip2.z.value) {
        Str za = netip_addr_zone(ip), zb = netip_addr_zone(ip2);
        Int m = za.len < zb.len ? za.len : zb.len;
        int d = m > 0 ? memcmp(za.p, zb.p, (size_t)m) : 0;
        if (d != 0)
            return d < 0 ? -1 : 1;
        if (za.len != zb.len)
            return za.len < zb.len ? -1 : 1;
    }
    return 0;
}

bool netip_addr_less(NetipAddr ip, NetipAddr ip2) {
    return netip_addr_compare(ip, ip2) == -1;
}

NetipAddr netip_addr_unmap(NetipAddr ip) {
    if (netip_addr_is4_in6(ip))
        ip.z = NIP_Z4;
    return ip;
}

bool netip_addr_is_link_local_unicast(NetipAddr ip) {
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip))
        return nip_v4(ip, 0) == 169 && nip_v4(ip, 1) == 254;
    if (netip_addr_is6(ip))
        return (nip_v6u16(ip, 0) & 0xffc0) == 0xfe80;
    return false;
}

bool netip_addr_is_loopback(NetipAddr ip) {
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip))
        return nip_v4(ip, 0) == 127;
    if (netip_addr_is6(ip))
        return ip.hi == 0 && ip.lo == 1;
    return false;
}

bool netip_addr_is_multicast(NetipAddr ip) {
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip))
        return (nip_v4(ip, 0) & 0xf0) == 0xe0;
    if (netip_addr_is6(ip))
        return ip.hi >> 56 == 0xff;
    return false;
}

bool netip_addr_is_interface_local_multicast(NetipAddr ip) {
    if (netip_addr_is6(ip) && !netip_addr_is4_in6(ip))
        return (nip_v6u16(ip, 0) & 0xff0f) == 0xff01;
    return false;
}

bool netip_addr_is_link_local_multicast(NetipAddr ip) {
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip))
        return nip_v4(ip, 0) == 224 && nip_v4(ip, 1) == 0 && nip_v4(ip, 2) == 0;
    if (netip_addr_is6(ip))
        return (nip_v6u16(ip, 0) & 0xff0f) == 0xff02;
    return false;
}

bool netip_addr_is_global_unicast(NetipAddr ip) {
    if (ip.z.value == NULL)
        return false;
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip) &&
        (ip.lo == 0xffff00000000ULL || ip.lo == 0xffffffffffffULL))
        return false;
    return !netip_addr_eq(ip, netip_ipv6_unspecified()) &&
           !netip_addr_is_loopback(ip) && !netip_addr_is_multicast(ip) &&
           !netip_addr_is_link_local_unicast(ip);
}

bool netip_addr_is_private(NetipAddr ip) {
    ip = netip_addr_unmap(ip);
    if (netip_addr_is4(ip))
        return nip_v4(ip, 0) == 10 ||
               (nip_v4(ip, 0) == 172 && (nip_v4(ip, 1) & 0xf0) == 16) ||
               (nip_v4(ip, 0) == 192 && nip_v4(ip, 1) == 168);
    if (netip_addr_is6(ip))
        return (ip.hi >> 56 & 0xfe) == 0xfc;
    return false;
}

bool netip_addr_is_unspecified(NetipAddr ip) {
    return netip_addr_eq(ip, netip_ipv4_unspecified()) ||
           netip_addr_eq(ip, netip_ipv6_unspecified());
}

NetipAddrAs16Ret netip_addr_as16(NetipAddr ip) {
    NetipAddrAs16Ret r;
    nip_put_be64(r.a, ip.hi);
    nip_put_be64(r.a + 8, ip.lo);
    return r;
}

NetipAddrAs4Ret netip_addr_as4(NetipAddr ip) {
    if (netip_addr_is4(ip) || netip_addr_is4_in6(ip)) {
        NetipAddrAs4Ret r = {
            {nip_v4(ip, 0), nip_v4(ip, 1), nip_v4(ip, 2), nip_v4(ip, 3)}};
        return r;
    }
    if (ip.z.value == NULL)
        panic_str(BURROW_S("As4 called on IP zero value"));
    panic_str(BURROW_S("As4 called on IPv6 address"));
}

Slice netip_addr_as_slice(NetipAddr ip, Alloc *a) {
    if (ip.z.value == NULL)
        return slice_nil(TYPE_BYTE);
    Int n = netip_addr_is4(ip) ? 4 : 16;
    Slice s = slice_make(a, TYPE_BYTE, n, n);
    Byte *p = (Byte *)s.p;
    if (n == 4) {
        for (int i = 0; i < 4; i++)
            p[i] = nip_v4(ip, i);
    } else {
        nip_put_be64(p, ip.hi);
        nip_put_be64(p + 8, ip.lo);
    }
    return s;
}

NetipAddr netip_addr_next(NetipAddr ip) {
    ip.lo++;
    if (ip.lo == 0)
        ip.hi++;
    if (netip_addr_is4(ip)) {
        if ((uint32_t)ip.lo == 0)
            return (NetipAddr){0, 0, {NULL}};
    } else if ((ip.hi | ip.lo) == 0) {
        return (NetipAddr){0, 0, {NULL}};
    }
    return ip;
}

NetipAddr netip_addr_prev(NetipAddr ip) {
    if (netip_addr_is4(ip)) {
        if ((uint32_t)ip.lo == 0)
            return (NetipAddr){0, 0, {NULL}};
    } else if ((ip.hi | ip.lo) == 0) {
        return (NetipAddr){0, 0, {NULL}};
    }
    if (ip.lo == 0)
        ip.hi--;
    ip.lo--;
    return ip;
}

/* ------------------------------------------------------------- formatting
 *
 * Every text form is some fixed text, the zone with its %, and some more fixed
 * text, and only the zone has no bound on its length. So the fixed parts are
 * built on the stack and the result is put together with one allocation. */

typedef struct NipText {
    Byte pre[64];
    Int npre;
    Str zone; /* written with a % in front when not empty */
    Byte suf[16];
    Int nsuf;
} NipText;

/* Where the text after the zone goes. With no zone it follows the address in
 * pre, so the usual case is copied out in one piece. Call nip_tail_done with
 * how much was written. */
static BURROW_INLINE Byte *nip_tail(NipText *t) {
    return t->zone.len == 0 ? t->pre + t->npre : t->suf + t->nsuf;
}

static BURROW_INLINE void nip_tail_done(NipText *t, Int n) {
    if (t->zone.len == 0)
        t->npre += n;
    else
        t->nsuf += n;
}

static const char nip_digits[] = "0123456789abcdef";

static const char nip_dec2[] = "00010203040506070809"
                               "10111213141516171819"
                               "20212223242526272829"
                               "30313233343536373839"
                               "40414243444546474849"
                               "50515253545556575859"
                               "60616263646566676869"
                               "70717273747576777879"
                               "80818283848586878889"
                               "90919293949596979899";

/* x, which is below 1000, in decimal. */
static BURROW_INLINE Int nip_put_dec(Byte *p, unsigned x) {
    if (x < 10) {
        p[0] = (Byte)('0' + x);
        return 1;
    }
    if (x < 100) {
        memcpy(p, nip_dec2 + 2 * (size_t)x, 2);
        return 2;
    }
    p[0] = (Byte)('0' + x / 100);
    memcpy(p + 1, nip_dec2 + 2 * (size_t)(x % 100), 2);
    return 3;
}

static BURROW_INLINE Int nip_put_hex(Byte *p, unsigned x) {
    Int n = 0;
    if (x >= 0x1000)
        p[n++] = (Byte)nip_digits[x >> 12];
    if (x >= 0x100)
        p[n++] = (Byte)nip_digits[x >> 8 & 0xf];
    if (x >= 0x10)
        p[n++] = (Byte)nip_digits[x >> 4 & 0xf];
    p[n++] = (Byte)nip_digits[x & 0xf];
    return n;
}

static BURROW_INLINE Int nip_put4(Byte *p, NetipAddr ip) {
    Int n = nip_put_dec(p, nip_v4(ip, 0));
    p[n++] = '.';
    n += nip_put_dec(p + n, nip_v4(ip, 1));
    p[n++] = '.';
    n += nip_put_dec(p + n, nip_v4(ip, 2));
    p[n++] = '.';
    n += nip_put_dec(p + n, nip_v4(ip, 3));
    return n;
}

/* appendTo6 without the zone: RFC 5952, the longest run of two or more zero
 * groups as ::, the first one on a tie. */
static BURROW_INLINE Int nip_put6(Byte *p, NetipAddr ip) {
    int zero_start = 255, zero_end = 255;
    for (int i = 0; i < 8; i++) {
        int j = i;
        while (j < 8 && nip_v6u16(ip, j) == 0)
            j++;
        int l = j - i;
        if (l >= 2 && l > zero_end - zero_start) {
            zero_start = i;
            zero_end = j;
        }
    }
    Int n = 0;
    for (int i = 0; i < 8; i++) {
        if (i == zero_start) {
            p[n++] = ':';
            p[n++] = ':';
            i = zero_end;
            if (i >= 8)
                break;
        } else if (i > 0) {
            p[n++] = ':';
        }
        n += nip_put_hex(p + n, nip_v6u16(ip, i));
    }
    return n;
}

static Int nip_put_lit(Byte *p, const char *s) {
    Int n = (Int)strlen(s);
    memcpy(p, s, (size_t)n);
    return n;
}

/* The address as AppendTo writes it, for a valid address, into t->pre and
 * t->zone. */
static BURROW_INLINE void nip_text_addr(NipText *t, NetipAddr ip) {
    if (netip_addr_is4(ip)) {
        t->npre += nip_put4(t->pre + t->npre, ip);
        return;
    }
    if (netip_addr_is4_in6(ip)) {
        t->npre += nip_put_lit(t->pre + t->npre, "::ffff:");
        t->npre += nip_put4(t->pre + t->npre, ip);
    } else {
        t->npre += nip_put6(t->pre + t->npre, ip);
    }
    if (nip_has_zone(ip))
        t->zone = *(const Str *)ip.z.value;
}

static BURROW_INLINE Int nip_text_len(const NipText *t) {
    return t->npre + (t->zone.len > 0 ? 1 + t->zone.len : 0) + t->nsuf;
}

static BURROW_INLINE void nip_text_write(const NipText *t, Byte *p) {
    memcpy(p, t->pre, (size_t)t->npre);
    p += t->npre;
    if (t->zone.len > 0) {
        *p++ = '%';
        memcpy(p, t->zone.p, (size_t)t->zone.len);
        p += t->zone.len;
    }
    if (t->nsuf > 0)
        memcpy(p, t->suf, (size_t)t->nsuf);
}

static BURROW_INLINE Str nip_text_str(const NipText *t, Alloc *a) {
    Int n = nip_text_len(t);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    nip_text_write(t, p);
    return str_from_bytes(p, n);
}

static Slice nip_bytes_append(Alloc *a, Slice b, const Byte *p, Int n) {
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    if (n <= b.cap - b.len) {
        if (n > 0)
            memcpy((Byte *)b.p + b.len, p, (size_t)n);
        b.len += n;
        return b;
    }
    return slice_append(a, b, p, n);
}

/* Room for n more bytes at the end of b, returned through *at. */
static BURROW_INLINE Slice nip_bytes_grow(Alloc *a, Slice b, Int n, Byte **at) {
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    if (n <= b.cap - b.len) {
        *at = (Byte *)b.p + b.len;
        b.len += n;
        return b;
    }
    b = slice_append(a, b, NULL, n);
    *at = (Byte *)b.p + b.len - n;
    return b;
}

static BURROW_INLINE Slice nip_text_append(const NipText *t, Alloc *a, Slice b) {
    Byte *at;
    b = nip_bytes_grow(a, b, nip_text_len(t), &at);
    nip_text_write(t, at);
    return b;
}

static Str nip_copy_lit(Alloc *a, const char *s) {
    Int n = (Int)strlen(s);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s, (size_t)n);
    return str_from_bytes(p, n);
}

Str netip_addr_string(NetipAddr ip, Alloc *a) {
    if (ip.z.value == NULL)
        return nip_copy_lit(a, "invalid IP");
    NipText t;
    t.npre = t.nsuf = 0;
    t.zone = BURROW_STR_EMPTY;
    nip_text_addr(&t, ip);
    return nip_text_str(&t, a);
}

Slice netip_addr_append_to(NetipAddr ip, Alloc *a, Slice b) {
    if (ip.z.value == NULL)
        return b.elem == NULL ? slice_nil(TYPE_BYTE) : b;
    NipText t;
    t.npre = t.nsuf = 0;
    t.zone = BURROW_STR_EMPTY;
    nip_text_addr(&t, ip);
    return nip_text_append(&t, a, b);
}

Str netip_addr_string_expanded(NetipAddr ip, Alloc *a) {
    if (!netip_addr_is6(ip))
        return netip_addr_string(ip, a);
    NipText t;
    t.npre = t.nsuf = 0;
    t.zone = BURROW_STR_EMPTY;
    for (int i = 0; i < 8; i++) {
        if (i > 0)
            t.pre[t.npre++] = ':';
        unsigned x = nip_v6u16(ip, i);
        t.pre[t.npre++] = (Byte)nip_digits[x >> 12];
        t.pre[t.npre++] = (Byte)nip_digits[x >> 8 & 0xf];
        t.pre[t.npre++] = (Byte)nip_digits[x >> 4 & 0xf];
        t.pre[t.npre++] = (Byte)nip_digits[x & 0xf];
    }
    if (nip_has_zone(ip))
        t.zone = *(const Str *)ip.z.value;
    return nip_text_str(&t, a);
}

/* A new empty byte slice with room for cap, which is what Go's MarshalText
 * starts from. */
static Slice nip_bytes_make(Alloc *a, Int cap) {
    return slice_make(a, TYPE_BYTE, 0, cap);
}

Slice netip_addr_append_text(NetipAddr ip, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_addr_append_to(ip, a, b);
}

Slice netip_addr_marshal_text(NetipAddr ip, Alloc *a, Error *err) {
    Int cap = 0;
    if (netip_addr_is4(ip))
        cap = 15;
    else if (netip_addr_is4_in6(ip))
        cap = 29;
    else if (ip.z.value != NULL)
        cap = 46;
    return netip_addr_append_text(ip, a, nip_bytes_make(a, cap), err);
}

Error netip_addr_unmarshal_text(NetipAddr *ip, Slice text) {
    if (text.len == 0) {
        *ip = (NetipAddr){0, 0, {NULL}};
        return BURROW_NO_ERROR;
    }
    Error err;
    *ip = netip_parse_addr(str_from_bytes((const Byte *)text.p, text.len), &err);
    return err;
}

Slice netip_addr_append_binary(NetipAddr ip, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (ip.z.value == NULL)
        return b.elem == NULL ? slice_nil(TYPE_BYTE) : b;
    if (netip_addr_is4(ip)) {
        Byte v[4] = {nip_v4(ip, 0), nip_v4(ip, 1), nip_v4(ip, 2), nip_v4(ip, 3)};
        return nip_bytes_append(a, b, v, 4);
    }
    Str zone = netip_addr_zone(ip);
    Byte *at;
    b = nip_bytes_grow(a, b, 16 + zone.len, &at);
    nip_put_be64(at, ip.hi);
    nip_put_be64(at + 8, ip.lo);
    if (zone.len > 0)
        memcpy(at + 16, zone.p, (size_t)zone.len);
    return b;
}

static Int nip_binary_size(NetipAddr ip) {
    if (ip.z.value == NULL)
        return 0;
    if (netip_addr_is4(ip))
        return 4;
    return 16 + netip_addr_zone(ip).len;
}

Slice netip_addr_marshal_binary(NetipAddr ip, Alloc *a, Error *err) {
    return netip_addr_append_binary(ip, a, nip_bytes_make(a, nip_binary_size(ip)), err);
}

Error netip_addr_unmarshal_binary(NetipAddr *ip, Slice b) {
    const Byte *p = (const Byte *)b.p;
    Int n = b.len;
    if (n == 0 || p == NULL) {
        *ip = (NetipAddr){0, 0, {NULL}};
    } else if (n == 4) {
        *ip = netip_addr_from4(p);
    } else if (n == 16) {
        *ip = netip_addr_from16(p);
    } else if (n > 16) {
        *ip =
            netip_addr_with_zone(netip_addr_from16(p), str_from_bytes(p + 16, n - 16));
    } else {
        return nip_errors_new("unexpected slice size");
    }
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- AddrPort */

static int nip_rindex(Str s, Byte c) {
    for (Int i = s.len - 1; i >= 0; i--)
        if (s.p[i] == c)
            return (int)i;
    return -1;
}

static NetipAddrPort nip_port_fail(Error *err, Error e) {
    BURROW_OUT(err, e);
    NetipAddrPort p = {{0, 0, {NULL}}, 0};
    return p;
}

NetipAddrPort netip_parse_addr_port(Str s, Error *err) {
    Int i = nip_rindex(s, ':');
    if (i == -1)
        return nip_port_fail(err, nip_errors_new("not an ip:port"));
    Str ip = str_from_bytes(s.p, i);
    Str port = str_from_bytes(s.p + i + 1, s.len - i - 1);
    if (ip.len == 0)
        return nip_port_fail(err, nip_errors_new("no IP"));
    if (port.len == 0)
        return nip_port_fail(err, nip_errors_new("no port"));
    bool v6 = false;
    if (ip.p[0] == '[') {
        if (ip.len < 2 || ip.p[ip.len - 1] != ']')
            return nip_port_fail(err, nip_errors_new("missing ]"));
        ip = str_from_bytes(ip.p + 1, ip.len - 2);
        v6 = true;
    }

    /* strconv.ParseUint(port, 10, 16): digits only, at most 65535. */
    uint32_t v = 0;
    bool ok = true;
    for (Int k = 0; k < port.len; k++) {
        Byte c = port.p[k];
        if (c < '0' || c > '9') {
            ok = false;
            break;
        }
        v = v * 10 + (uint32_t)(c - '0');
        if (v > 0xffff) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        NipPart parts[] = {NIP_LIT("invalid port "), NIP_Q(port), NIP_LIT(" parsing "),
                           NIP_Q(s)};
        return nip_port_fail(err, nip_error(parts, 4));
    }

    Error e;
    NetipAddrPort r;
    r.ip = netip_parse_addr(ip, &e);
    r.port = (uint16_t)v;
    if (BURROW_FAILED(e))
        return nip_port_fail(err, e);
    if (v6 && netip_addr_is4(r.ip)) {
        NipPart parts[] = {
            NIP_LIT("invalid ip:port "), NIP_Q(s),
            NIP_LIT(", square brackets can only be used with IPv6 addresses")};
        return nip_port_fail(err, nip_error(parts, 3));
    }
    if (!v6 && netip_addr_is6(r.ip)) {
        NipPart parts[] = {
            NIP_LIT("invalid ip:port "), NIP_Q(s),
            NIP_LIT(", IPv6 addresses must be surrounded by square brackets")};
        return nip_port_fail(err, nip_error(parts, 3));
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return r;
}

NetipAddrPort netip_must_parse_addr_port(Str s) {
    Error err;
    NetipAddrPort p = netip_parse_addr_port(s, &err);
    if (BURROW_FAILED(err))
        nip_panic(err);
    return p;
}

Int netip_addr_port_compare(NetipAddrPort p, NetipAddrPort p2) {
    Int c = netip_addr_compare(p.ip, p2.ip);
    if (c != 0)
        return c;
    return (p.port > p2.port) - (p.port < p2.port);
}

static BURROW_INLINE Int nip_put_port(Byte *p, uint16_t port) {
    unsigned v = port;
    if (v < 1000)
        return nip_put_dec(p, v);
    Int n = nip_put_dec(p, v / 100);
    memcpy(p + n, nip_dec2 + 2 * (size_t)(v % 100), 2);
    return n + 2;
}

static BURROW_INLINE void nip_text_addr_port(NipText *t, NetipAddrPort p) {
    t->npre = t->nsuf = 0;
    t->zone = BURROW_STR_EMPTY;
    if (netip_addr_is4(p.ip)) {
        nip_text_addr(t, p.ip);
    } else {
        t->pre[t->npre++] = '[';
        nip_text_addr(t, p.ip);
        *nip_tail(t) = ']';
        nip_tail_done(t, 1);
    }
    Byte *at = nip_tail(t);
    at[0] = ':';
    nip_tail_done(t, 1 + nip_put_port(at + 1, p.port));
}

Str netip_addr_port_string(NetipAddrPort p, Alloc *a) {
    if (p.ip.z.value == NULL)
        return nip_copy_lit(a, "invalid AddrPort");
    NipText t;
    nip_text_addr_port(&t, p);
    return nip_text_str(&t, a);
}

Slice netip_addr_port_append_to(NetipAddrPort p, Alloc *a, Slice b) {
    if (p.ip.z.value == NULL)
        return b.elem == NULL ? slice_nil(TYPE_BYTE) : b;
    NipText t;
    nip_text_addr_port(&t, p);
    return nip_text_append(&t, a, b);
}

Slice netip_addr_port_append_text(NetipAddrPort p, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_addr_port_append_to(p, a, b);
}

Slice netip_addr_port_marshal_text(NetipAddrPort p, Alloc *a, Error *err) {
    Int cap = 0;
    if (netip_addr_is4(p.ip))
        cap = 21;
    else if (p.ip.z.value != NULL)
        cap = 54;
    return netip_addr_port_append_text(p, a, nip_bytes_make(a, cap), err);
}

Error netip_addr_port_unmarshal_text(NetipAddrPort *p, Slice text) {
    if (text.len == 0) {
        *p = (NetipAddrPort){{0, 0, {NULL}}, 0};
        return BURROW_NO_ERROR;
    }
    Error err;
    *p = netip_parse_addr_port(str_from_bytes((const Byte *)text.p, text.len), &err);
    return err;
}

Slice netip_addr_port_append_binary(NetipAddrPort p, Alloc *a, Slice b, Error *err) {
    b = netip_addr_append_binary(p.ip, a, b, err);
    Byte v[2] = {(Byte)p.port, (Byte)(p.port >> 8)};
    return nip_bytes_append(a, b, v, 2);
}

Slice netip_addr_port_marshal_binary(NetipAddrPort p, Alloc *a, Error *err) {
    return netip_addr_port_append_binary(
        p, a, nip_bytes_make(a, nip_binary_size(p.ip) + 2), err);
}

Error netip_addr_port_unmarshal_binary(NetipAddrPort *p, Slice b) {
    if (b.len < 2)
        return nip_errors_new("unexpected slice size");
    NetipAddr addr;
    Error err = netip_addr_unmarshal_binary(&addr, slice_sub(b, 0, b.len - 2));
    if (BURROW_FAILED(err))
        return err;
    const Byte *q = (const Byte *)b.p + b.len - 2;
    *p = netip_addr_port_from(addr, (uint16_t)(q[0] | q[1] << 8));
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- Prefix */

NetipPrefix netip_prefix_from(NetipAddr ip, Int bits) {
    NetipPrefix p;
    p.bits_plus_one = 0;
    if (ip.z.value != NULL && bits >= 0 && bits <= netip_addr_bit_len(ip))
        p.bits_plus_one = (uint8_t)(bits + 1);
    p.ip = nip_without_zone(ip);
    return p;
}

static NetipPrefix nip_prefix_zero(void) {
    NetipPrefix p = {{0, 0, {NULL}}, 0};
    return p;
}

static Error nip_bits_too_large(Int b, const char *fam) {
    Byte num[24];
    Int n = 0;
    uint64_t u = (uint64_t)b;
    Byte tmp[24];
    do {
        tmp[n++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    for (Int i = 0; i < n; i++)
        num[i] = tmp[n - 1 - i];
    NipPart parts[] = {NIP_LIT("prefix length "), NIP_STR(str_from_bytes(num, n)),
                       NIP_LIT(" too large for "), NIP_STR(str_from_cstr(fam))};
    return nip_error(parts, 4);
}

NetipPrefix netip_addr_prefix(NetipAddr ip, Int b, Error *err) {
    if (b < 0) {
        BURROW_OUT(err, nip_errors_new("negative Prefix bits"));
        return nip_prefix_zero();
    }
    Int effective = b;
    if (ip.z.value == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return nip_prefix_zero();
    }
    if (netip_addr_is4(ip)) {
        if (b > 32) {
            BURROW_OUT(err, nip_bits_too_large(b, "IPv4"));
            return nip_prefix_zero();
        }
        effective += 96;
    } else if (b > 128) {
        BURROW_OUT(err, nip_bits_too_large(b, "IPv6"));
        return nip_prefix_zero();
    }
    uint64_t mh, ml;
    nip_mask6(effective, &mh, &ml);
    ip.hi &= mh;
    ip.lo &= ml;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_prefix_from(ip, b);
}

NetipPrefix netip_prefix_masked(NetipPrefix p) {
    return netip_addr_prefix(p.ip, netip_prefix_bits(p), NULL);
}

Int netip_prefix_compare(NetipPrefix p, NetipPrefix p2) {
    Int c = netip_addr_compare(netip_prefix_masked(p).ip, netip_prefix_masked(p2).ip);
    if (c != 0)
        return c;
    Int b1 = netip_prefix_bits(p), b2 = netip_prefix_bits(p2);
    if (b1 != b2)
        return b1 < b2 ? -1 : 1;
    return netip_addr_compare(p.ip, p2.ip);
}

static NetipPrefix nip_prefix_fail(Error *err, Str in, NipPart msg) {
    NipPart parts[] = {NIP_LIT("netip.ParsePrefix("), NIP_Q(in), NIP_LIT("): "), msg};
    BURROW_OUT(err, nip_error(parts, 4));
    return nip_prefix_zero();
}

static NetipPrefix nip_prefix_bad_bits(Error *err, Str in, Str bits) {
    NipPart parts[] = {NIP_LIT("netip.ParsePrefix("), NIP_Q(in), NIP_LIT("): "),
                       NIP_LIT("bad bits after slash: "), NIP_Q(bits)};
    BURROW_OUT(err, nip_error(parts, 5));
    return nip_prefix_zero();
}

NetipPrefix netip_parse_prefix(Str s, Error *err) {
    Int i = nip_rindex(s, '/');
    if (i < 0)
        return nip_prefix_fail(err, s, NIP_LIT("no '/'"));
    Error e;
    NetipAddr ip = netip_parse_addr(str_from_bytes(s.p, i), &e);
    if (BURROW_FAILED(e))
        return nip_prefix_fail(err, s, NIP_STR(error_text(e)));
    if (nip_has_zone(ip))
        return nip_prefix_fail(err, s,
                               NIP_LIT("IPv6 zones cannot be present in a prefix"));

    Str bs = str_from_bytes(s.p + i + 1, s.len - i - 1);
    /* strconv.Atoi takes a sign and leading zeros, and Go refuses both before
     * it gets there. */
    if (bs.len > 1 && (bs.p[0] < '1' || bs.p[0] > '9'))
        return nip_prefix_bad_bits(err, s, bs);
    if (bs.len == 0)
        return nip_prefix_bad_bits(err, s, bs);
    /* What is left is digits or Atoi's syntax error, and a number too big for
     * Int is Atoi's range error. Both are bad bits. */
    uint64_t v = 0;
    for (Int k = 0; k < bs.len; k++) {
        Byte c = bs.p[k];
        if (c < '0' || c > '9')
            return nip_prefix_bad_bits(err, s, bs);
        if (v > ((uint64_t)INT64_MAX - (uint64_t)(c - '0')) / 10)
            return nip_prefix_bad_bits(err, s, bs);
        v = v * 10 + (uint64_t)(c - '0');
    }
    if (sizeof(Int) < 8 && v > (uint64_t)INTPTR_MAX)
        return nip_prefix_bad_bits(err, s, bs);
    uint64_t max_bits = netip_addr_is6(ip) ? 128 : 32;
    if (v > max_bits)
        return nip_prefix_fail(err, s, NIP_LIT("prefix length out of range"));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_prefix_from(ip, (Int)v);
}

NetipPrefix netip_must_parse_prefix(Str s) {
    Error err;
    NetipPrefix p = netip_parse_prefix(s, &err);
    if (BURROW_FAILED(err))
        nip_panic(err);
    return p;
}

bool netip_prefix_contains(NetipPrefix p, NetipAddr ip) {
    if (!netip_prefix_is_valid(p) || nip_has_zone(ip))
        return false;
    Int f1 = netip_addr_bit_len(p.ip), f2 = netip_addr_bit_len(ip);
    if (f1 == 0 || f2 == 0 || f1 != f2)
        return false;
    Int bits = netip_prefix_bits(p);
    if (netip_addr_is4(ip))
        return (uint32_t)((ip.lo ^ p.ip.lo) >> ((32 - bits) & 63)) == 0;
    uint64_t mh, ml;
    nip_mask6(bits, &mh, &ml);
    return (((ip.hi ^ p.ip.hi) & mh) | ((ip.lo ^ p.ip.lo) & ml)) == 0;
}

bool netip_prefix_overlaps(NetipPrefix p, NetipPrefix o) {
    if (!netip_prefix_is_valid(p) || !netip_prefix_is_valid(o))
        return false;
    if (netip_prefix_eq(p, o))
        return true;
    if (netip_addr_is4(p.ip) != netip_addr_is4(o.ip))
        return false;
    Int pb = netip_prefix_bits(p), ob = netip_prefix_bits(o);
    Int min_bits = pb < ob ? pb : ob;
    if (min_bits == 0)
        return true;
    Error e;
    p = netip_addr_prefix(p.ip, min_bits, &e);
    if (BURROW_FAILED(e))
        return false;
    o = netip_addr_prefix(o.ip, min_bits, &e);
    if (BURROW_FAILED(e))
        return false;
    return netip_addr_eq(p.ip, o.ip);
}

static BURROW_INLINE void nip_text_prefix(NipText *t, NetipPrefix p) {
    t->npre = t->nsuf = 0;
    t->zone = BURROW_STR_EMPTY;
    nip_text_addr(t, p.ip);
    t->zone = BURROW_STR_EMPTY; /* a prefix has none, and PrefixFrom drops it */
    t->pre[t->npre++] = '/';
    t->npre += nip_put_dec(t->pre + t->npre, (unsigned)(uint8_t)netip_prefix_bits(p));
}

Str netip_prefix_string(NetipPrefix p, Alloc *a) {
    if (!netip_prefix_is_valid(p))
        return nip_copy_lit(a, "invalid Prefix");
    NipText t;
    nip_text_prefix(&t, p);
    return nip_text_str(&t, a);
}

Slice netip_prefix_append_to(NetipPrefix p, Alloc *a, Slice b) {
    if (netip_prefix_eq(p, nip_prefix_zero()))
        return b.elem == NULL ? slice_nil(TYPE_BYTE) : b;
    if (!netip_prefix_is_valid(p)) {
        static const char invalid[] = "invalid Prefix";
        return nip_bytes_append(a, b, (const Byte *)invalid, (Int)sizeof invalid - 1);
    }
    NipText t;
    nip_text_prefix(&t, p);
    return nip_text_append(&t, a, b);
}

Slice netip_prefix_append_text(NetipPrefix p, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return netip_prefix_append_to(p, a, b);
}

Slice netip_prefix_marshal_text(NetipPrefix p, Alloc *a, Error *err) {
    Int cap = 0;
    if (netip_addr_is4(p.ip))
        cap = 18;
    else if (p.ip.z.value != NULL)
        cap = 50;
    return netip_prefix_append_text(p, a, nip_bytes_make(a, cap), err);
}

Error netip_prefix_unmarshal_text(NetipPrefix *p, Slice text) {
    if (text.len == 0) {
        *p = nip_prefix_zero();
        return BURROW_NO_ERROR;
    }
    Error err;
    *p = netip_parse_prefix(str_from_bytes((const Byte *)text.p, text.len), &err);
    return err;
}

Slice netip_prefix_append_binary(NetipPrefix p, Alloc *a, Slice b, Error *err) {
    b = netip_addr_append_binary(nip_without_zone(p.ip), a, b, err);
    Byte bits = (Byte)netip_prefix_bits(p);
    return nip_bytes_append(a, b, &bits, 1);
}

Slice netip_prefix_marshal_binary(NetipPrefix p, Alloc *a, Error *err) {
    Int cap = nip_binary_size(nip_without_zone(p.ip)) + 1;
    return netip_prefix_append_binary(p, a, nip_bytes_make(a, cap), err);
}

Error netip_prefix_unmarshal_binary(NetipPrefix *p, Slice b) {
    if (b.len < 1)
        return nip_errors_new("unexpected slice size");
    NetipAddr addr;
    Error err = netip_addr_unmarshal_binary(&addr, slice_sub(b, 0, b.len - 1));
    if (BURROW_FAILED(err))
        return err;
    *p = netip_prefix_from(addr, (Int)((const Byte *)b.p)[b.len - 1]);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------ descriptors
 *
 * The methods take the receiver by pointer, which is the shape a descriptor
 * wants, and pass it on by value. String puts its text in the goroutine's
 * error arena, since it has no allocator to take. */

#define NIP_SIG_STRING(IN, OUT) OUT(Str)

/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define NIP_THUNKS(T, pre)                                                             \
    static Slice pre##_m_append_binary(T *self, Alloc *a, Slice b, Error *err) {       \
        return pre##_append_binary(*self, a, b, err);                                  \
    }                                                                                  \
    static Slice pre##_m_append_text(T *self, Alloc *a, Slice b, Error *err) {         \
        return pre##_append_text(*self, a, b, err);                                    \
    }                                                                                  \
    static Slice pre##_m_marshal_binary(T *self, Alloc *a, Error *err) {               \
        return pre##_marshal_binary(*self, a, err);                                    \
    }                                                                                  \
    static Slice pre##_m_marshal_text(T *self, Alloc *a, Error *err) {                 \
        return pre##_marshal_text(*self, a, err);                                      \
    }                                                                                  \
    static Str pre##_m_string(T *self) {                                               \
        return pre##_string(*self, error_allocator());                                 \
    }                                                                                  \
    static Error pre##_m_unmarshal_binary(T *self, Alloc *a, Slice data) {             \
        (void)a;                                                                       \
        return pre##_unmarshal_binary(self, data);                                     \
    }                                                                                  \
    static Error pre##_m_unmarshal_text(T *self, Alloc *a, Slice data) {               \
        (void)a;                                                                       \
        return pre##_unmarshal_text(self, data);                                       \
    }

#define NIP_METHODS(pre, M, T)                                                         \
    M(T, AppendBinary, pre##_m_append_binary, ENCODING_SIG_APPEND_BINARY)              \
    M(T, AppendText, pre##_m_append_text, ENCODING_SIG_APPEND_TEXT)                    \
    M(T, MarshalBinary, pre##_m_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)           \
    M(T, MarshalText, pre##_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                 \
    M(T, String, pre##_m_string, NIP_SIG_STRING)                                       \
    M(T, UnmarshalBinary, pre##_m_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)     \
    M(T, UnmarshalText, pre##_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

NIP_THUNKS(NetipAddr, netip_addr)
NIP_THUNKS(NetipAddrPort, netip_addr_port)
NIP_THUNKS(NetipPrefix, netip_prefix)
/* NOLINTEND(bugprone-macro-parentheses) */

#define NIP_ADDR_METHODS(M, T) NIP_METHODS(netip_addr, M, T)
#define NIP_ADDR_PORT_METHODS(M, T) NIP_METHODS(netip_addr_port, M, T)
#define NIP_PREFIX_METHODS(M, T) NIP_METHODS(netip_prefix, M, T)

BURROW_METHODS_DEFINE(NetipAddr, NIP_ADDR_METHODS);
BURROW_METHODS_DEFINE(NetipAddrPort, NIP_ADDR_PORT_METHODS);
BURROW_METHODS_DEFINE(NetipPrefix, NIP_PREFIX_METHODS);

static const Field nip_addr_fields[] = {
    {{(const Byte *)"hi", 2},
     {NULL, 0},
     &burrow_type_uint64_t,
     (uint32_t)offsetof(NetipAddr, hi)},
    {{(const Byte *)"lo", 2},
     {NULL, 0},
     &burrow_type_uint64_t,
     (uint32_t)offsetof(NetipAddr, lo)},
    {{(const Byte *)"z", 1},
     {NULL, 0},
     &burrow_type_UniqueHandle,
     (uint32_t)offsetof(NetipAddr, z)},
};

static const Field nip_addr_port_fields[] = {
    {{(const Byte *)"ip", 2},
     {NULL, 0},
     &burrow_type_NetipAddr,
     (uint32_t)offsetof(NetipAddrPort, ip)},
    {{(const Byte *)"port", 4},
     {NULL, 0},
     &burrow_type_uint16_t,
     (uint32_t)offsetof(NetipAddrPort, port)},
};

static const Field nip_prefix_fields[] = {
    {{(const Byte *)"ip", 2},
     {NULL, 0},
     &burrow_type_NetipAddr,
     (uint32_t)offsetof(NetipPrefix, ip)},
    {{(const Byte *)"bitsPlusOne", 11},
     {NULL, 0},
     &burrow_type_uint8_t,
     (uint32_t)offsetof(NetipPrefix, bits_plus_one)},
};

#define NIP_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

const Type burrow_type_NetipAddr = {
    {(const Byte *)"Addr", 4},
    {(const Byte *)"net/netip", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(NetipAddr),
    (uint16_t)_Alignof(NetipAddr),
    NIP_COUNT(nip_addr_fields),
    NIP_COUNT(burrow__methods_NetipAddr),
    nip_addr_fields,
    burrow__methods_NetipAddr,
    NULL,
    NULL,
    0,
    0x6e697061U, /* "nipa" */
    NULL,
};

const Type burrow_type_NetipAddrPort = {
    {(const Byte *)"AddrPort", 8},
    {(const Byte *)"net/netip", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(NetipAddrPort),
    (uint16_t)_Alignof(NetipAddrPort),
    NIP_COUNT(nip_addr_port_fields),
    NIP_COUNT(burrow__methods_NetipAddrPort),
    nip_addr_port_fields,
    burrow__methods_NetipAddrPort,
    NULL,
    NULL,
    0,
    0x6e697070U, /* "nipp" */
    NULL,
};

const Type burrow_type_NetipPrefix = {
    {(const Byte *)"Prefix", 6},
    {(const Byte *)"net/netip", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(NetipPrefix),
    (uint16_t)_Alignof(NetipPrefix),
    NIP_COUNT(nip_prefix_fields),
    NIP_COUNT(burrow__methods_NetipPrefix),
    nip_prefix_fields,
    burrow__methods_NetipPrefix,
    NULL,
    NULL,
    0,
    0x6e697066U, /* "nipf" */
    NULL,
};
