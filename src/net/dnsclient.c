/* DNSError and the parts of the DNS client that do not talk to a server.
 *
 * Derived from Go's src/net/dnsclient.go and the DNSError, notFoundError and
 * temporaryError parts of net.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/context.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ND_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define ND_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

static Byte *nd_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* ----------------------------------------------------------------- DNSError */

/* The struct first, so errors_as hands back a pointer to it, and the message
 * after, built once since the message slot cannot allocate. */
typedef struct NetDNSErrorBox {
    NetDNSError e;
    Str message;
} NetDNSErrorBox;

static const char nd_msg_lookup[] = "lookup ";
static const char nd_msg_on[] = " on ";

/* "lookup " + name, " on " + server when there is one, ": " + err. */
static Int nd_error_len(const NetDNSError *e) {
    Int n = (Int)sizeof nd_msg_lookup - 1 + e->name.len;
    if (e->server.len != 0)
        n += (Int)sizeof nd_msg_on - 1 + e->server.len;
    return n + 2 + e->err.len;
}

static void nd_error_write(Byte *p, const NetDNSError *e) {
    p = nd_put(p, ND_LIT(nd_msg_lookup));
    p = nd_put(p, e->name);
    if (e->server.len != 0) {
        p = nd_put(p, ND_LIT(nd_msg_on));
        p = nd_put(p, e->server);
    }
    *p++ = ':';
    *p++ = ' ';
    nd_put(p, e->err);
}

Str net_dns_error_error(const NetDNSError *e, Alloc *a) {
    if (e == NULL)
        return ND_LIT("<nil>");
    Int n = nd_error_len(e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    nd_error_write(p, e);
    p[n] = 0;
    return str_from_bytes(p, n);
}

Error net_dns_error_unwrap(const NetDNSError *e) {
    return e->unwrap_err;
}

bool net_dns_error_timeout(const NetDNSError *e) {
    return e->is_timeout;
}

bool net_dns_error_temporary(const NetDNSError *e) {
    return e->is_timeout || e->is_temporary;
}

static Str nd_m_error(NetDNSError *self) {
    return ((const NetDNSErrorBox *)self)->message;
}

static bool nd_m_temporary(NetDNSError *self) {
    return net_dns_error_temporary(self);
}

static bool nd_m_timeout(NetDNSError *self) {
    return net_dns_error_timeout(self);
}

static Error nd_m_unwrap(NetDNSError *self) {
    return self->unwrap_err;
}

#define ND_SIG_STRING(IN, OUT) OUT(Str)
#define ND_SIG_BOOL(IN, OUT) OUT(bool)
#define ND_SIG_ERROR(IN, OUT) OUT(Error)

#define ND_DNS_ERROR_METHODS(M, T)                                                     \
    M(T, Error, nd_m_error, ND_SIG_STRING)                                             \
    M(T, Temporary, nd_m_temporary, ND_SIG_BOOL)                                       \
    M(T, Timeout, nd_m_timeout, ND_SIG_BOOL)                                           \
    M(T, Unwrap, nd_m_unwrap, ND_SIG_ERROR)

BURROW_METHODS_DEFINE(NetDNSError, ND_DNS_ERROR_METHODS);

static const Field nd_dns_error_fields[] = {
    {BURROW_S_INIT("UnwrapErr"),
     {NULL, 0},
     &burrow_type_Error,
     (uint32_t)offsetof(NetDNSError, unwrap_err)},
    {BURROW_S_INIT("Err"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetDNSError, err)},
    {BURROW_S_INIT("Name"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetDNSError, name)},
    {BURROW_S_INIT("Server"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetDNSError, server)},
    {BURROW_S_INIT("IsTimeout"),
     {NULL, 0},
     TYPE_BOOL,
     (uint32_t)offsetof(NetDNSError, is_timeout)},
    {BURROW_S_INIT("IsTemporary"),
     {NULL, 0},
     TYPE_BOOL,
     (uint32_t)offsetof(NetDNSError, is_temporary)},
    {BURROW_S_INIT("IsNotFound"),
     {NULL, 0},
     TYPE_BOOL,
     (uint32_t)offsetof(NetDNSError, is_not_found)},
};

static const Type nd_dns_error_desc = {
    BURROW_S_INIT("DNSError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetDNSError),
    (uint16_t)_Alignof(NetDNSError),
    ND_COUNT(nd_dns_error_fields),
    ND_COUNT(burrow__methods_NetDNSError),
    nd_dns_error_fields,
    burrow__methods_NetDNSError,
    NULL,
    NULL,
    0,
    0x6e657464U, /* "netd" */
    NULL,
};

const Type *const TYPE_NET_DNS_ERROR = &nd_dns_error_desc;

static Str nd_dns_error_message(const void *self) {
    return ((const NetDNSErrorBox *)self)->message;
}

static Error nd_dns_error_unwrap_slot(const void *self) {
    return ((const NetDNSError *)self)->unwrap_err;
}

static Error nd_dns_error_clone(const void *self, Alloc *a);

static const ErrorVT nd_dns_error_vt = {
    &nd_dns_error_desc,
    nd_dns_error_message,
    nd_dns_error_unwrap_slot,
    NULL,
    NULL,
    NULL,
    nd_dns_error_clone,
};

/* One allocation: the box, then the three strings and the message. */
Error net_dns_error_as_error(const NetDNSError *e, Alloc *a) {
    Int mlen = nd_error_len(e);
    size_t size = sizeof(NetDNSErrorBox) + (size_t)e->err.len + (size_t)e->name.len +
                  (size_t)e->server.len + (size_t)mlen;
    NetDNSErrorBox *b =
        (NetDNSErrorBox *)mem_alloc_nozero(a, size, _Alignof(NetDNSErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = *e;
    Byte *p = (Byte *)(b + 1);
    b->e.err = str_from_bytes(p, e->err.len);
    p = nd_put(p, e->err);
    b->e.name = str_from_bytes(p, e->name.len);
    p = nd_put(p, e->name);
    b->e.server = str_from_bytes(p, e->server.len);
    p = nd_put(p, e->server);
    nd_error_write(p, e);
    b->message = str_from_bytes(p, mlen);
    return (Error){&nd_dns_error_vt, b};
}

/* The copy keeps what it wraps by way of error_retain, so that nothing in it
 * points back at the original's memory. */
static Error nd_dns_error_clone(const void *self, Alloc *a) {
    NetDNSError e = ((const NetDNSErrorBox *)self)->e;
    e.unwrap_err = error_retain(a, e.unwrap_err);
    return net_dns_error_as_error(&e, a);
}

/* ------------------------------------------ notFoundError, temporaryError */

typedef struct NetDNSTextError {
    Str s;
} NetDNSTextError;

static Str nd_text_message(const void *self) {
    return ((const NetDNSTextError *)self)->s;
}

static Error nd_not_found_clone(const void *self, Alloc *a) {
    return burrow__net_not_found_error(a, ((const NetDNSTextError *)self)->s);
}

static Error nd_temporary_clone(const void *self, Alloc *a) {
    return burrow__net_temporary_error(a, ((const NetDNSTextError *)self)->s);
}

/* notFoundError has no methods newDNSError asks for, and nothing outside net
 * can name it, so it has no type to extract it as. newDNSError knows it by its
 * table. */
static const ErrorVT nd_not_found_vt = {
    NULL, nd_text_message, NULL, NULL, NULL, NULL, nd_not_found_clone,
};

static Str nd_temporary_m_error(NetDNSTextError *self) {
    return self->s;
}

static bool nd_temporary_m_temporary(NetDNSTextError *self) {
    (void)self;
    return true;
}

static bool nd_temporary_m_timeout(NetDNSTextError *self) {
    (void)self;
    return false;
}

#define ND_TEMPORARY_METHODS(M, T)                                                     \
    M(T, Error, nd_temporary_m_error, ND_SIG_STRING)                                   \
    M(T, Temporary, nd_temporary_m_temporary, ND_SIG_BOOL)                             \
    M(T, Timeout, nd_temporary_m_timeout, ND_SIG_BOOL)

BURROW_METHODS_DEFINE(NetDNSTextError, ND_TEMPORARY_METHODS);

/* temporaryError has a type of its own so that net.Error can find its
 * Temporary and Timeout methods. */
static const Type nd_temporary_desc = {
    BURROW_S_INIT("temporaryError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetDNSTextError),
    (uint16_t)_Alignof(NetDNSTextError),
    0,
    ND_COUNT(burrow__methods_NetDNSTextError),
    NULL,
    burrow__methods_NetDNSTextError,
    NULL,
    NULL,
    0,
    0x6e657474U, /* "nett" */
    NULL,
};

static const ErrorVT nd_temporary_vt = {
    &nd_temporary_desc, nd_text_message, NULL, NULL, NULL, NULL, nd_temporary_clone,
};

/* errNoSuchHost and errUnknownPort, which are notFoundErrors, and
 * errServerTemporarilyMisbehaving, a temporaryError. */
#define ND_TEXT_ERROR(name, vt, text)                                                  \
    static const NetDNSTextError name##__v = {                                         \
        {(const Byte *)("" text), (Int)(sizeof(text) - 1)}};                           \
    const Error name = {&vt, &name##__v}

ND_TEXT_ERROR(burrow__net_err_no_such_host, nd_not_found_vt, "no such host");
ND_TEXT_ERROR(burrow__net_err_unknown_port, nd_not_found_vt, "unknown port");
ND_TEXT_ERROR(burrow__net_err_server_temporarily_misbehaving, nd_temporary_vt,
              "server misbehaving");

BURROW_SENTINEL_ERROR(burrow__net_err_no_suitable_address, "no suitable address found");
BURROW_SENTINEL_ERROR(burrow__net_err_lame_referral, "lame referral");
BURROW_SENTINEL_ERROR(burrow__net_err_cannot_unmarshal, "cannot unmarshal DNS message");
BURROW_SENTINEL_ERROR(burrow__net_err_cannot_marshal, "cannot marshal DNS message");
BURROW_SENTINEL_ERROR(burrow__net_err_server_misbehaving, "server misbehaving");
BURROW_SENTINEL_ERROR(burrow__net_err_invalid_dns_response, "invalid DNS response");
BURROW_SENTINEL_ERROR(burrow__net_err_no_answer_from_dns_server,
                      "no answer from DNS server");

static Error nd_text_error(Alloc *a, const ErrorVT *vt, Str s) {
    NetDNSTextError *b = (NetDNSTextError *)mem_alloc_nozero(
        a, sizeof(NetDNSTextError) + (size_t)s.len, _Alignof(NetDNSTextError));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    nd_put(p, s);
    b->s = str_from_bytes(p, s.len);
    return (Error){vt, b};
}

Error burrow__net_not_found_error(Alloc *a, Str s) {
    return nd_text_error(a, &nd_not_found_vt, s);
}

Error burrow__net_temporary_error(Alloc *a, Str s) {
    return nd_text_error(a, &nd_temporary_vt, s);
}

Error burrow__net_new_dns_error(Alloc *a, Error err, Str name, Str server) {
    NetDNSError e = {0};
    if (net_is_error(err)) {
        e.is_timeout = net_error_timeout(err);
        e.is_temporary = net_error_temporary(err);
    }
    /* At this time, the only errors Go wraps are the context's, so that a
     * caller can check for a lookup that was cancelled or timed out. */
    if (errors_is(err, context_deadline_exceeded) || errors_is(err, context_canceled))
        e.unwrap_err = err;
    e.is_not_found = err.vt == &nd_not_found_vt;
    e.err = error_text(err);
    e.name = name;
    e.server = server;
    return net_dns_error_as_error(&e, a);
}

/* -------------------------------------------------------------- reverseaddr */

static const char nd_hex[] = "0123456789abcdef";

static Byte *nd_put_dec(Byte *p, Byte v) {
    if (v >= 100)
        *p++ = (Byte)('0' + v / 100);
    if (v >= 10)
        *p++ = (Byte)('0' + v / 10 % 10);
    *p++ = (Byte)('0' + v % 10);
    return p;
}

Str burrow__net_reverseaddr(Alloc *a, Str addr, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte b[16];
    NetIP ip = net_parse_ip(a, addr);
    bool ok = burrow__net_ip_to16(ip, b);
    if (ip.p != NULL)
        mem_free(a, ip.p, (size_t)ip.cap, 1);
    if (!ok) {
        NetDNSError e = {0};
        e.err = ND_LIT("unrecognized address");
        e.name = addr;
        BURROW_OUT(err, net_dns_error_as_error(&e, error_allocator()));
        return BURROW_STR_EMPTY;
    }
    static const char v4_suffix[] = "in-addr.arpa.";
    static const char v6_suffix[] = "ip6.arpa.";
    if (net_ip_to4(slice_from(b, 16, 16, TYPE_BYTE)).p != NULL) {
        /* Four numbers of up to three digits, each with its dot. */
        Byte buf[16 + sizeof v4_suffix];
        Byte *p = buf;
        for (int i = 15; i >= 12; i--) {
            p = nd_put_dec(p, b[i]);
            *p++ = '.';
        }
        p = nd_put(p, ND_LIT(v4_suffix));
        Str s = str_clone(a, str_from_bytes(buf, p - buf));
        if (s.len == 0)
            BURROW_OUT(err, burrow_err_out_of_memory);
        return s;
    }
    /* Must be IPv6: each nibble, lowest first, with a dot after it. */
    Byte buf[(size_t)16 * 4 + sizeof v6_suffix];
    Byte *p = buf;
    for (int i = 15; i >= 0; i--) {
        *p++ = (Byte)nd_hex[b[i] & 0xF];
        *p++ = '.';
        *p++ = (Byte)nd_hex[b[i] >> 4];
        *p++ = '.';
    }
    p = nd_put(p, ND_LIT(v6_suffix));
    Str s = str_clone(a, str_from_bytes(buf, p - buf));
    if (s.len == 0)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return s;
}

/* ------------------------------------------------------------- domain names */

bool burrow__net_is_domain_name(Str s) {
    /* The root domain name is valid. See golang.org/issue/45715. */
    if (s.len == 1 && s.p[0] == '.')
        return true;

    /* See RFC 1035, RFC 3696. Presentation format has dots before every label
     * except the first, and the terminal empty label is optional here because
     * the input is taken to be fully qualified. Space has to be kept for the
     * first and last labels' length octets in wire format, where they are
     * needed and the most there can be is 255. So the real limit is 253, but
     * 254 is not turned down when the last byte is a dot. */
    Int l = s.len;
    if (l == 0 || l > 254 || (l == 254 && s.p[l - 1] != '.'))
        return false;

    Byte last = '.';
    bool non_numeric = false; /* true once a letter or hyphen is seen */
    Int partlen = 0;
    for (Int i = 0; i < l; i++) {
        Byte c = s.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            non_numeric = true;
            partlen++;
        } else if (c >= '0' && c <= '9') {
            partlen++;
        } else if (c == '-') {
            /* The byte before a dash cannot be a dot. */
            if (last == '.')
                return false;
            partlen++;
            non_numeric = true;
        } else if (c == '.') {
            /* The byte before a dot cannot be a dot or a dash. */
            if (last == '.' || last == '-')
                return false;
            if (partlen > 63 || partlen == 0)
                return false;
            partlen = 0;
        } else {
            return false;
        }
        last = c;
    }
    if (last == '-' || partlen > 63)
        return false;
    return non_numeric;
}

Str burrow__net_abs_domain_name(Alloc *a, Str s) {
    if (s.len == 0 || memchr(s.p, '.', (size_t)s.len) == NULL || s.p[s.len - 1] == '.')
        return s;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len + 2, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    nd_put(p, s);
    p[s.len] = '.';
    p[s.len + 1] = 0;
    return str_from_bytes(p, s.len + 1);
}

/* ------------------------------------------------------------- SRV and MX */

/* randIntn, from the runtime's generator with the sign bit cleared. */
static Int nd_rand_intn(Int n) {
    return (Int)((Uint)runtime_rand64() >> 1) % n;
}

void burrow__net_srv_shuffle_by_weight(NetSRV **addrs, Int n) {
    Int sum = 0;
    for (Int i = 0; i < n; i++)
        sum += addrs[i]->weight;
    while (sum > 0 && n > 1) {
        Int s = 0;
        Int r = nd_rand_intn(sum);
        for (Int i = 0; i < n; i++) {
            s += addrs[i]->weight;
            if (s > r) {
                if (i > 0) {
                    NetSRV *t = addrs[0];
                    addrs[0] = addrs[i];
                    addrs[i] = t;
                }
                break;
            }
        }
        sum -= addrs[0]->weight;
        addrs++;
        n--;
    }
}

static int nd_cmp_u16(uint16_t x, uint16_t y) {
    return (x > y) - (x < y);
}

static int nd_srv_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const NetSRV *a = *(NetSRV *const *)x;
    const NetSRV *b = *(NetSRV *const *)y;
    int r = nd_cmp_u16(a->priority, b->priority);
    if (r != 0)
        return r;
    return nd_cmp_u16(a->weight, b->weight);
}

static int nd_mx_cmp(void *env, const void *x, const void *y) {
    (void)env;
    return nd_cmp_u16((*(NetMX *const *)x)->pref, (*(NetMX *const *)y)->pref);
}

/* Go sorts a []*SRV, which here is n pointers sorted as unsafe.Pointers. */
static Slice nd_pointers(void *p, Int n) {
    return slice_from(p, n, n, TYPE_UNSAFE_POINTER);
}

void burrow__net_srv_sort(NetSRV **addrs, Int n) {
    slices_sort_func(nd_pointers(addrs, n), BURROW_FN(SlicesCmpFunc, nd_srv_cmp, NULL));
    Int i = 0;
    for (Int j = 1; j < n; j++) {
        if (addrs[i]->priority != addrs[j]->priority) {
            burrow__net_srv_shuffle_by_weight(addrs + i, j - i);
            i = j;
        }
    }
    burrow__net_srv_shuffle_by_weight(addrs + i, n - i);
}

void burrow__net_mx_sort(NetMX **s, Int n) {
    for (Int i = 0; i < n; i++) {
        Int j = nd_rand_intn(i + 1);
        NetMX *t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    slices_sort_func(nd_pointers(s, n), BURROW_FN(SlicesCmpFunc, nd_mx_cmp, NULL));
}
