/* Derived from Go's src/net/url/url.go and encoding_table.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/url.h"

#include "burrow/atomic.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net/netip.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/type.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- the table */

/* Go's encoding modes. A bit set in the table means the byte is left alone in
 * that mode, and the top bit marks the hex digits. */
enum {
    ENC_PATH = 1 << 0,
    ENC_PATH_SEGMENT = 1 << 1,
    ENC_HOST = 1 << 2,
    ENC_ZONE = 1 << 3,
    ENC_USER_PASSWORD = 1 << 4,
    ENC_QUERY_COMPONENT = 1 << 5,
    ENC_FRAGMENT = 1 << 6,
    HEX_CHAR = 1 << 7,
};

/* Go's table, from gen_encoding_table.go. */
static const Byte url_table[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x0c, 0x00, 0x5f, 0x00, 0x5f,
    0x0c, 0x4c, 0x4c, 0x4c, 0x5f, 0x5d, 0x7f, 0x7f, 0x41, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x4f, 0x5d, 0x0c, 0x5f, 0x0c, 0x40, 0x43,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x0c, 0x00, 0x0c, 0x00, 0x7f, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x7f,
};

static const char url_upperhex[] = "0123456789ABCDEF";

static bool url_ishex(Byte c) {
    return (url_table[c] & HEX_CHAR) != 0;
}

/* Only for a byte url_ishex said yes to. */
static Byte url_unhex(Byte c) {
    return (Byte)(9 * (c >> 6) + (c & 15));
}

static bool url_should_escape(Byte c, int mode) {
    return (url_table[c] & mode) == 0;
}

static Str url_sub(Str s, Int i, Int j) {
    return i == j ? BURROW_STR_EMPTY : str_from_bytes(s.p + i, j - i);
}

static Int url_index_byte(Str s, Byte c) {
    if (s.len <= 0)
        return -1;
    const Byte *q = (const Byte *)memchr(s.p, c, (size_t)s.len);
    return q == NULL ? -1 : (Int)(q - s.p);
}

static Int url_last_index_byte(Str s, Byte c) {
    for (Int i = s.len - 1; i >= 0; i--)
        if (s.p[i] == c)
            return i;
    return -1;
}

static bool url_has_prefix(Str s, const char *pre, Int n) {
    return s.len >= n && memcmp(s.p, pre, (size_t)n) == 0;
}

static bool url_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

#define URL_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* ------------------------------------------------------------------- errors */

/* The UrlError first, so errors_as hands back a pointer to it, and the
 * message after, built once because the message slot cannot allocate. */
typedef struct UrlErrorBox {
    UrlError e;
    Str message;
} UrlErrorBox;

static Str url_error_message(const void *self) {
    return ((const UrlErrorBox *)self)->message;
}

static Error url_error_unwrap_slot(const void *self) {
    return ((const UrlError *)self)->err;
}

static const Type url_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"net/url", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(UrlError),
    (uint16_t)_Alignof(UrlError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x75726c65U, /* "urle" */
    NULL,
};

const Type *const TYPE_URL_ERROR = &url_error_desc;

static Error url_error_clone(const void *self, Alloc *a);

static const ErrorVT url_error_vt = {
    &url_error_desc, url_error_message, url_error_unwrap_slot, NULL, NULL, NULL,
    url_error_clone,
};

static Byte *url_put(Byte *p, const void *src, Int n) {
    if (n > 0)
        memcpy(p, src, (size_t)n);
    return p + n;
}

/* op + " " + Quote(url) + ": " + text. */
static Int url_error_message_len(Str op, Str url, Str text) {
    return op.len + 1 + burrow__strconv_quote_into(NULL, url) + 2 + text.len;
}

static void url_error_write_message(Byte *p, Str op, Str url, Str text) {
    p = url_put(p, op.p, op.len);
    *p++ = ' ';
    p += burrow__strconv_quote_into(p, url);
    *p++ = ':';
    *p++ = ' ';
    url_put(p, text.p, text.len);
}

/* One allocation: the box, then op, url and the message. */
static Error url_error_build(Alloc *a, Str op, Str url, Error inner) {
    Str text = error_text(inner);
    Int mlen = url_error_message_len(op, url, text);
    size_t size = sizeof(UrlErrorBox) + (size_t)op.len + (size_t)url.len + (size_t)mlen;
    UrlErrorBox *b = (UrlErrorBox *)mem_alloc_nozero(a, size, _Alignof(UrlErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.op = str_from_bytes(p, op.len);
    p = url_put(p, op.p, op.len);
    b->e.url = str_from_bytes(p, url.len);
    p = url_put(p, url.p, url.len);
    b->e.err = inner;
    url_error_write_message(p, op, url, text);
    b->message = str_from_bytes(p, mlen);
    return (Error){&url_error_vt, b};
}

Error url_error_as_error(const UrlError *e, Alloc *a) {
    return url_error_build(a, e->op, e->url, e->err);
}

static Error url_error_clone(const void *self, Alloc *a) {
    UrlError copy = *(const UrlError *)self;
    copy.err = error_retain(a, copy.err);
    return url_error_as_error(&copy, a);
}

Str url_error_error(const UrlError *e, Alloc *a) {
    Str text = error_text(e->err);
    Int mlen = url_error_message_len(e->op, e->url, text);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    url_error_write_message(p, e->op, e->url, text);
    return str_from_bytes(p, mlen);
}

Error url_error_unwrap(const UrlError *e) {
    return e->err;
}

/* Go asks e.Err for a Timeout() bool or Temporary() bool method. Here the
 * methods are on the descriptor of the error's type, when it has one. */
static bool url_error_ask(const UrlError *e, Str name) {
    if (e == NULL || e->err.vt == NULL || e->err.vt->self_type == NULL)
        return false;
    const Method *m = type_method_by_name(e->err.vt->self_type, name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    if (type_num_in(m->ftype) != 0 || type_num_out(m->ftype) != 1 ||
        type_out(m->ftype, 0) != TYPE_BOOL)
        return false;
    bool out = false;
    void *rets[1] = {&out};
    method_call(m, (void *)(uintptr_t)e->err.data, NULL, rets);
    return out;
}

bool url_error_timeout(const UrlError *e) {
    return url_error_ask(e, URL_LIT("Timeout"));
}

bool url_error_temporary(const UrlError *e) {
    return url_error_ask(e, URL_LIT("Temporary"));
}

/* EscapeError and InvalidHostError are strings, and the box keeps the string
 * first so errors_as gives a Str *. */
typedef struct UrlStrErrorBox {
    Str s;
    Str message;
} UrlStrErrorBox;

static Str url_str_error_message(const void *self) {
    return ((const UrlStrErrorBox *)self)->message;
}

static const Type url_escape_error_desc = {
    {(const Byte *)"EscapeError", 11},
    {(const Byte *)"net/url", 7},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x75726c78U, /* "urlx" */
    NULL,
};

static const Type url_invalid_host_error_desc = {
    {(const Byte *)"InvalidHostError", 16},
    {(const Byte *)"net/url", 7},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x75726c68U, /* "urlh" */
    NULL,
};

const Type *const TYPE_URL_ESCAPE_ERROR = &url_escape_error_desc;
const Type *const TYPE_URL_INVALID_HOST_ERROR = &url_invalid_host_error_desc;

static bool url_str_error_is(const void *self, Error target);
static Error url_escape_error_clone(const void *self, Alloc *a);
static Error url_invalid_host_error_clone(const void *self, Alloc *a);

static const ErrorVT url_escape_error_vt = {
    &url_escape_error_desc, url_str_error_message, NULL, NULL, url_str_error_is, NULL,
    url_escape_error_clone,
};

static const ErrorVT url_invalid_host_error_vt = {
    &url_invalid_host_error_desc,
    url_str_error_message,
    NULL,
    NULL,
    url_str_error_is,
    NULL,
    url_invalid_host_error_clone,
};

/* Go compares the two values with ==, which for these is the text. */
static bool url_str_error_is(const void *self, Error target) {
    if (target.vt == NULL || target.data == NULL)
        return false;
    const ErrorVT *vt = target.vt;
    if (vt != &url_escape_error_vt && vt != &url_invalid_host_error_vt)
        return false;
    const UrlStrErrorBox *b = (const UrlStrErrorBox *)self;
    const UrlStrErrorBox *t = (const UrlStrErrorBox *)target.data;
    /* The message says which type it is, and it has the text in it. */
    return url_str_eq(b->message, t->message) && url_str_eq(b->s, t->s);
}

static const char url_msg_escape[] = "invalid URL escape ";
static const char url_msg_host1[] = "invalid character ";
static const char url_msg_host2[] = " in host name";

#define URL_MSG_LEN(s) ((Int)sizeof(s) - 1)

static Int url_str_error_len(bool host, Str s) {
    Int q = burrow__strconv_quote_into(NULL, s);
    return host ? URL_MSG_LEN(url_msg_host1) + q + URL_MSG_LEN(url_msg_host2)
                : URL_MSG_LEN(url_msg_escape) + q;
}

static void url_str_error_write(Byte *p, bool host, Str s) {
    if (host) {
        p = url_put(p, url_msg_host1, URL_MSG_LEN(url_msg_host1));
        p += burrow__strconv_quote_into(p, s);
        url_put(p, url_msg_host2, URL_MSG_LEN(url_msg_host2));
    } else {
        p = url_put(p, url_msg_escape, URL_MSG_LEN(url_msg_escape));
        burrow__strconv_quote_into(p, s);
    }
}

static Error url_str_error_build(Alloc *a, bool host, Str s) {
    Int mlen = url_str_error_len(host, s);
    size_t size = sizeof(UrlStrErrorBox) + (size_t)s.len + (size_t)mlen;
    UrlStrErrorBox *b =
        (UrlStrErrorBox *)mem_alloc_nozero(a, size, _Alignof(UrlStrErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->s = str_from_bytes(p, s.len);
    p = url_put(p, s.p, s.len);
    url_str_error_write(p, host, s);
    b->message = str_from_bytes(p, mlen);
    return (Error){host ? &url_invalid_host_error_vt : &url_escape_error_vt, b};
}

static Error url_escape_error_clone(const void *self, Alloc *a) {
    return url_str_error_build(a, false, ((const UrlStrErrorBox *)self)->s);
}

static Error url_invalid_host_error_clone(const void *self, Alloc *a) {
    return url_str_error_build(a, true, ((const UrlStrErrorBox *)self)->s);
}

static Str url_str_error_text(Alloc *a, bool host, Str s) {
    Int mlen = url_str_error_len(host, s);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    url_str_error_write(p, host, s);
    return str_from_bytes(p, mlen);
}

Str url_escape_error_error(UrlEscapeError e, Alloc *a) {
    return url_str_error_text(a, false, e);
}

Error url_escape_error_as_error(UrlEscapeError e, Alloc *a) {
    return url_str_error_build(a, false, e);
}

Str url_invalid_host_error_error(UrlInvalidHostError e, Alloc *a) {
    return url_str_error_text(a, true, e);
}

Error url_invalid_host_error_as_error(UrlInvalidHostError e, Alloc *a) {
    return url_str_error_build(a, true, e);
}

static Error url_errors_new(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

/* ------------------------------------------------------------------ GODEBUG */

enum {
    URL_DEBUG_KNOWN = 1 << 0,
    URL_DEBUG_LAX_COLONS = 1 << 1, /* urlstrictcolons=0 */
    URL_DEBUG_MAX_SET = 1 << 2,    /* urlmaxqueryparams is a number */
};

static uint32_t url_debug_flags;
static uint64_t url_debug_max;

/* The value of key in GODEBUG, the last one when it is there twice, as Go's
 * internal/godebug reads it. */
static bool url_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

/* The settings from a GODEBUG value, or the defaults for NULL. */
static uint32_t url_debug_parse(const char *v) {
    uint32_t f = URL_DEBUG_KNOWN;
    uint64_t max = 0;
    Str s;
    if (v != NULL && url_godebug(v, "urlstrictcolons", &s) && s.len == 1 &&
        s.p[0] == '0')
        f |= URL_DEBUG_LAX_COLONS;
    if (v != NULL && url_godebug(v, "urlmaxqueryparams", &s) && s.len > 0) {
        /* strconv.Atoi, and a value that is not a number leaves the
         * default in place. */
        Error err;
        int64_t n = strconv_atoi(s, &err);
        if (BURROW_OK(err)) {
            f |= URL_DEBUG_MAX_SET;
            max = (uint64_t)n;
        }
    }
    burrow__atomic64_store(&url_debug_max, max);
    burrow__atomic_store_relaxed_u32(&url_debug_flags, f);
    return f;
}

static uint32_t url_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&url_debug_flags);
    if ((f & URL_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return url_debug_parse(v);
}

void burrow__url_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&url_debug_flags, 0);
    else
        (void)url_debug_parse(value);
}

#define URL_DEFAULT_MAX_PARAMS 10000

static bool url_params_within_max(Int params) {
    bool within_default = params <= URL_DEFAULT_MAX_PARAMS;
    uint32_t f = url_debug_load();
    if ((f & URL_DEBUG_MAX_SET) == 0)
        return within_default;
    int64_t custom = (int64_t)burrow__atomic64_load(&url_debug_max);
    return custom == 0 || (int64_t)params < custom;
}

/* ----------------------------------------------------------------- unescape */

enum { UNESC_OK, UNESC_BAD_ESCAPE, UNESC_BAD_HOST };

/* The checking half of Go's unescape. Says whether s is good in mode, how long
 * it is once unescaped, and whether that differs from s. On a problem, *bad is
 * the piece of s the error names. */
static int url_unescape_check(Str s, int mode, Int *out_len, bool *changed, Str *bad) {
    Int n = 0;
    bool has_plus = false;
    const Byte *p = s.p;
    for (Int i = 0; i < s.len;) {
        Byte c = p[i];
        if (c == '%') {
            n++;
            if (i + 2 >= s.len || !url_ishex(p[i + 1]) || !url_ishex(p[i + 2])) {
                Int e = s.len - i > 3 ? i + 3 : s.len;
                *bad = url_sub(s, i, e);
                return UNESC_BAD_ESCAPE;
            }
            bool pct25 = p[i + 1] == '2' && p[i + 2] == '5';
            if (mode == ENC_HOST && url_unhex(p[i + 1]) < 8 && !pct25) {
                *bad = url_sub(s, i, i + 3);
                return UNESC_BAD_ESCAPE;
            }
            if (mode == ENC_ZONE) {
                Byte v = (Byte)(url_unhex(p[i + 1]) << 4 | url_unhex(p[i + 2]));
                if (!pct25 && v != ' ' && url_should_escape(v, ENC_HOST)) {
                    *bad = url_sub(s, i, i + 3);
                    return UNESC_BAD_ESCAPE;
                }
            }
            i += 3;
        } else if (c == '+') {
            has_plus = mode == ENC_QUERY_COMPONENT;
            i++;
        } else {
            if ((mode == ENC_HOST || mode == ENC_ZONE) && c < 0x80 &&
                url_should_escape(c, mode)) {
                *bad = url_sub(s, i, i + 1);
                return UNESC_BAD_HOST;
            }
            i++;
        }
    }
    *out_len = s.len - 2 * n;
    *changed = n > 0 || has_plus;
    return UNESC_OK;
}

/* The writing half, for an s the check passed. Returns the length. */
static Int url_unescape_write(Byte *dst, Str s, int mode) {
    Byte plus = mode == ENC_QUERY_COMPONENT ? ' ' : '+';
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == '%') {
            dst[j++] = (Byte)(url_unhex(s.p[i + 1]) << 4 | url_unhex(s.p[i + 2]));
            i += 2;
        } else if (c == '+') {
            dst[j++] = plus;
        } else {
            dst[j++] = c;
        }
    }
    return j;
}

static Error url_unescape_error(int code, Str bad) {
    return url_str_error_build(error_allocator(), code == UNESC_BAD_HOST, bad);
}

static Str url_unescape_alloc(Alloc *a, Str s, int mode, Error *err) {
    Int n;
    bool changed;
    Str bad;
    int code = url_unescape_check(s, mode, &n, &changed, &bad);
    if (code != UNESC_OK) {
        BURROW_OUT(err, url_unescape_error(code, bad));
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!changed)
        return s;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    url_unescape_write(p, s, mode);
    return str_from_bytes(p, n);
}

Str url_query_unescape(Alloc *a, Str s, Error *err) {
    return url_unescape_alloc(a, s, ENC_QUERY_COMPONENT, err);
}

Str url_path_unescape(Alloc *a, Str s, Error *err) {
    return url_unescape_alloc(a, s, ENC_PATH_SEGMENT, err);
}

/* Whether unescape(esc, mode) would succeed and give want, with no copy. */
static bool url_unescape_equals(Str esc, int mode, Str want) {
    Int n;
    bool changed;
    Str bad;
    if (url_unescape_check(esc, mode, &n, &changed, &bad) != UNESC_OK || n != want.len)
        return false;
    if (!changed)
        return url_str_eq(esc, want);
    Byte plus = mode == ENC_QUERY_COMPONENT ? ' ' : '+';
    Int j = 0;
    for (Int i = 0; i < esc.len; i++, j++) {
        Byte c = esc.p[i];
        if (c == '%') {
            c = (Byte)(url_unhex(esc.p[i + 1]) << 4 | url_unhex(esc.p[i + 2]));
            i += 2;
        } else if (c == '+') {
            c = plus;
        }
        if (want.p[j] != c)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------- escape */

/* The length of s escaped in mode. */
static Int url_escape_len(Str s, int mode) {
    Int hex = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (url_should_escape(c, mode) && !(c == ' ' && mode == ENC_QUERY_COMPONENT))
            hex++;
    }
    return s.len + 2 * hex;
}

static bool url_needs_escape(Str s, int mode) {
    for (Int i = 0; i < s.len; i++)
        if (url_should_escape(s.p[i], mode))
            return true;
    return false;
}

static Int url_escape_write(Byte *dst, Str s, int mode) {
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == ' ' && mode == ENC_QUERY_COMPONENT) {
            dst[j++] = '+';
        } else if (url_should_escape(c, mode)) {
            dst[j] = '%';
            dst[j + 1] = (Byte)url_upperhex[c >> 4];
            dst[j + 2] = (Byte)url_upperhex[c & 15];
            j += 3;
        } else {
            dst[j++] = c;
        }
    }
    return j;
}

/* Whether escape(s, mode) == want, with no copy. */
static bool url_escape_equals(Str s, int mode, Str want) {
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == ' ' && mode == ENC_QUERY_COMPONENT) {
            if (j >= want.len || want.p[j] != '+')
                return false;
            j++;
        } else if (url_should_escape(c, mode)) {
            if (j + 3 > want.len || want.p[j] != '%' ||
                want.p[j + 1] != (Byte)url_upperhex[c >> 4] ||
                want.p[j + 2] != (Byte)url_upperhex[c & 15])
                return false;
            j += 3;
        } else {
            if (j >= want.len || want.p[j] != c)
                return false;
            j++;
        }
    }
    return j == want.len;
}

static Str url_escape_alloc(Alloc *a, Str s, int mode) {
    if (!url_needs_escape(s, mode))
        return s;
    Int n = url_escape_len(s, mode);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    url_escape_write(p, s, mode);
    return str_from_bytes(p, n);
}

Str url_query_escape(Alloc *a, Str s) {
    return url_escape_alloc(a, s, ENC_QUERY_COMPONENT);
}

Str url_path_escape(Alloc *a, Str s) {
    return url_escape_alloc(a, s, ENC_PATH_SEGMENT);
}

/* ------------------------------------------------------------------- writer */

/* Text built in two passes: one with p NULL to count, one to write. */
typedef struct UrlW {
    Byte *p;
    Int n;
} UrlW;

static void w_bytes(UrlW *w, const void *src, Int n) {
    if (w->p != NULL && n > 0)
        memcpy(w->p + w->n, src, (size_t)n);
    w->n += n;
}

static void w_str(UrlW *w, Str s) {
    w_bytes(w, s.p, s.len);
}

static void w_byte(UrlW *w, Byte c) {
    if (w->p != NULL)
        w->p[w->n] = c;
    w->n++;
}

static void w_escape(UrlW *w, Str s, int mode) {
    if (w->p != NULL)
        w->n += url_escape_write(w->p + w->n, s, mode);
    else
        w->n += url_escape_len(s, mode);
}

/* Runs build twice, counting and then writing into memory from a. */
typedef void (*UrlBuildFn)(UrlW *w, const void *arg);

static Str url_build(Alloc *a, UrlBuildFn build, const void *arg) {
    UrlW w = {NULL, 0};
    build(&w, arg);
    if (w.n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)w.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    UrlW w2 = {p, 0};
    build(&w2, arg);
    return str_from_bytes(p, w2.n);
}

/* ----------------------------------------------------------------- Userinfo */

UrlUserinfo *url_user(Alloc *a, Str username) {
    UrlUserinfo *u = (UrlUserinfo *)mem_alloc(a, sizeof *u, _Alignof(UrlUserinfo));
    if (u != NULL)
        u->username = username;
    return u;
}

UrlUserinfo *url_user_password(Alloc *a, Str username, Str password) {
    UrlUserinfo *u = (UrlUserinfo *)mem_alloc(a, sizeof *u, _Alignof(UrlUserinfo));
    if (u != NULL) {
        u->username = username;
        u->password = password;
        u->password_set = true;
    }
    return u;
}

Str url_userinfo_username(const UrlUserinfo *u) {
    return u == NULL ? BURROW_STR_EMPTY : u->username;
}

Str url_userinfo_password(const UrlUserinfo *u, bool *ok) {
    if (u == NULL) {
        BURROW_OUT(ok, false);
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(ok, u->password_set);
    return u->password;
}

static void w_userinfo(UrlW *w, const UrlUserinfo *u) {
    w_escape(w, u->username, ENC_USER_PASSWORD);
    if (u->password_set) {
        w_byte(w, ':');
        w_escape(w, u->password, ENC_USER_PASSWORD);
    }
}

static void url_userinfo_build(UrlW *w, const void *arg) {
    w_userinfo(w, (const UrlUserinfo *)arg);
}

Str url_userinfo_string(const UrlUserinfo *u, Alloc *a) {
    if (u == NULL)
        return BURROW_STR_EMPTY;
    if (!u->password_set && !url_needs_escape(u->username, ENC_USER_PASSWORD))
        return u->username;
    return url_build(a, url_userinfo_build, u);
}

/* -------------------------------------------------------------------- block */

/* A Url from this package lives in one block: the Url, room for a Userinfo,
 * then its text. The writes go through a bump pointer. */
typedef struct UrlBlock {
    Byte *p;
    Byte *end;
} UrlBlock;

static Str blk_put(UrlBlock *b, const void *src, Int n) {
    Byte *p = b->p;
    if (n > 0)
        memcpy(p, src, (size_t)n);
    b->p += n;
    return str_from_bytes(p, n);
}

static Str blk_copy(UrlBlock *b, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    return blk_put(b, s.p, s.len);
}

/* An allocation for a Url and extra bytes of text. */
static Url *url_block_new(Alloc *a, Int extra, UrlBlock *b) {
    size_t size = sizeof(Url) + sizeof(UrlUserinfo) + (size_t)extra;
    Url *u = (Url *)mem_alloc_nozero(a, size, _Alignof(Url));
    if (u == NULL)
        return NULL;
    memset(u, 0, sizeof(Url) + sizeof(UrlUserinfo));
    u->mem = u;
    u->size = size;
    b->p = (Byte *)u + sizeof(Url) + sizeof(UrlUserinfo);
    b->end = (Byte *)u + size;
    return u;
}

static UrlUserinfo *url_block_user(Url *u) {
    return (UrlUserinfo *)(void *)((Byte *)u + sizeof(Url));
}

void url_free(Alloc *a, Url *u) {
    if (u == NULL || u->mem == NULL)
        return;
    mem_free(a, u->mem, u->size, _Alignof(Url));
}

/* Unescapes s into the block when it changes, and returns s itself when it
 * does not. The check has to have passed. */
static Str blk_unescape(UrlBlock *b, Str s, int mode) {
    Int n;
    bool changed;
    Str bad;
    (void)url_unescape_check(s, mode, &n, &changed, &bad);
    if (!changed)
        return s;
    Byte *p = b->p;
    b->p += url_unescape_write(p, s, mode);
    return str_from_bytes(p, n);
}

/* Go's unescape as a step of the parse: the check, then the block. */
static bool url_unescape_step(UrlBlock *b, Str s, int mode, Str *out, Error *err) {
    Int n;
    bool changed;
    Str bad;
    int code = url_unescape_check(s, mode, &n, &changed, &bad);
    if (code != UNESC_OK) {
        *err = url_unescape_error(code, bad);
        return false;
    }
    *out = blk_unescape(b, s, mode);
    return true;
}

/* ---------------------------------------------------------------- the parse */

/* setPath: path is p unescaped, and raw_path is p when escaping path again
 * would not give p back. */
static bool url_set_path(Url *u, UrlBlock *b, Str p, Error *err) {
    Str path;
    if (!url_unescape_step(b, p, ENC_PATH, &path, err))
        return false;
    u->path = path;
    u->raw_path = url_escape_equals(path, ENC_PATH, p) ? BURROW_STR_EMPTY : p;
    return true;
}

static bool url_set_fragment(Url *u, UrlBlock *b, Str f, Error *err) {
    Str frag;
    if (!url_unescape_step(b, f, ENC_FRAGMENT, &frag, err))
        return false;
    u->fragment = frag;
    u->raw_fragment = url_escape_equals(frag, ENC_FRAGMENT, f) ? BURROW_STR_EMPTY : f;
    return true;
}

/* getScheme. Returns false for the "missing protocol scheme" error. */
static bool url_get_scheme(Str raw, Str *scheme, Str *rest) {
    for (Int i = 0; i < raw.len; i++) {
        Byte c = raw.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            continue;
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            if (i == 0)
                break;
            continue;
        }
        if (c == ':') {
            if (i == 0)
                return false;
            *scheme = url_sub(raw, 0, i);
            *rest = url_sub(raw, i + 1, raw.len);
            return true;
        }
        break;
    }
    *scheme = BURROW_STR_EMPTY;
    *rest = raw;
    return true;
}

static bool url_valid_optional_port(Str port) {
    if (port.len == 0)
        return true;
    if (port.p[0] != ':')
        return false;
    for (Int i = 1; i < port.len; i++)
        if (port.p[i] < '0' || port.p[i] > '9')
            return false;
    return true;
}

static bool url_valid_userinfo(Str s) {
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            continue;
        switch (c) {
        case '-':
        case '.':
        case '_':
        case ':':
        case '~':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case '%':
        case '@':
            continue;
        default:
            /* Go ranges over runes, and anything not ASCII is not allowed
             * either, so looking at bytes gives the same answer. */
            return false;
        }
    }
    return true;
}

static bool url_contains_ctl(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] < ' ' || s.p[i] == 0x7f)
            return true;
    return false;
}

static Error url_err_invalid_port(Str colon_port) {
    return fmt_errorf_v("invalid port %q after host", colon_port);
}

/* parseHost, into *out. */
static bool url_parse_host(UrlBlock *b, Str scheme, Str host, Str *out, Error *err) {
    Int open = url_last_index_byte(host, '[');
    if (open > 0) {
        *err = url_errors_new("invalid IP-literal");
        return false;
    }
    if (open == 0) {
        Int close = url_last_index_byte(host, ']');
        if (close < 0) {
            *err = url_errors_new("missing ']' in host");
            return false;
        }
        Str colon_port = url_sub(host, close + 1, host.len);
        if (!url_valid_optional_port(colon_port)) {
            *err = url_err_invalid_port(colon_port);
            return false;
        }
        Int n;
        bool changed;
        Str bad;
        int code = url_unescape_check(colon_port, ENC_HOST, &n, &changed, &bad);
        if (code != UNESC_OK) {
            *err = url_unescape_error(code, bad);
            return false;
        }

        Str hostname = url_sub(host, 1, close);
        Int zone = -1;
        for (Int i = 0; i + 2 < hostname.len; i++) {
            if (hostname.p[i] == '%' && hostname.p[i + 1] == '2' &&
                hostname.p[i + 2] == '5') {
                zone = i;
                break;
            }
        }
        Str host_part = zone >= 0 ? url_sub(hostname, 0, zone) : hostname;
        Str zone_part =
            zone >= 0 ? url_sub(hostname, zone, hostname.len) : BURROW_STR_EMPTY;
        code = url_unescape_check(host_part, ENC_HOST, &n, &changed, &bad);
        if (code != UNESC_OK) {
            *err = url_unescape_error(code, bad);
            return false;
        }
        if (zone >= 0) {
            code = url_unescape_check(zone_part, ENC_ZONE, &n, &changed, &bad);
            if (code != UNESC_OK) {
                *err = url_unescape_error(code, bad);
                return false;
            }
        }

        /* "[" + host + zone + "]" + port, all unescaped. With no '%' in it
         * that is host as it came. */
        Str unescaped_hostname;
        Str result;
        if (url_index_byte(host, '%') < 0) {
            unescaped_hostname = hostname;
            result = host;
        } else {
            Byte *start = b->p;
            *b->p++ = '[';
            Byte *hs = b->p;
            b->p += url_unescape_write(b->p, host_part, ENC_HOST);
            b->p += url_unescape_write(b->p, zone_part, ENC_ZONE);
            unescaped_hostname = str_from_bytes(hs, (Int)(b->p - hs));
            *b->p++ = ']';
            b->p += url_unescape_write(b->p, colon_port, ENC_HOST);
            result = str_from_bytes(start, (Int)(b->p - start));
        }

        /* Only an IPv6 address can go in brackets, which leaves out IPv4 but
         * not an IPv4-mapped IPv6 address. */
        Error perr;
        NetipAddr addr = netip_parse_addr(unescaped_hostname, &perr);
        if (BURROW_FAILED(perr)) {
            *err = fmt_errorf_v("invalid host: %w", perr);
            return false;
        }
        if (netip_addr_is4(addr)) {
            *err = url_errors_new("invalid IP-literal");
            return false;
        }
        *out = result;
        return true;
    }

    Int i = url_index_byte(host, ':');
    if (i >= 0) {
        Int last = url_last_index_byte(host, ':');
        if (last != i) {
            /* RFC 3986 has no colons in a host, but databases such as
             * PostgreSQL and MongoDB put a list of hosts there, so Go allows
             * them except for http and https. */
            bool http = url_str_eq(scheme, URL_LIT("http")) ||
                        url_str_eq(scheme, URL_LIT("https"));
            if (!http || (url_debug_load() & URL_DEBUG_LAX_COLONS) != 0)
                i = last;
        }
        Str colon_port = url_sub(host, i, host.len);
        if (!url_valid_optional_port(colon_port)) {
            *err = url_err_invalid_port(colon_port);
            return false;
        }
    }
    return url_unescape_step(b, host, ENC_HOST, out, err);
}

static bool url_parse_authority(Url *u, UrlBlock *b, Str authority, Error *err) {
    Int i = url_last_index_byte(authority, '@');
    Str host_in = i < 0 ? authority : url_sub(authority, i + 1, authority.len);
    if (!url_parse_host(b, u->scheme, host_in, &u->host, err))
        return false;
    if (i < 0)
        return true;
    Str userinfo = url_sub(authority, 0, i);
    if (!url_valid_userinfo(userinfo)) {
        *err = url_errors_new("net/url: invalid userinfo");
        return false;
    }
    UrlUserinfo *ui = url_block_user(u);
    Int colon = url_index_byte(userinfo, ':');
    if (colon < 0) {
        if (!url_unescape_step(b, userinfo, ENC_USER_PASSWORD, &ui->username, err))
            return false;
    } else {
        Str name = url_sub(userinfo, 0, colon);
        Str pass = url_sub(userinfo, colon + 1, userinfo.len);
        if (!url_unescape_step(b, name, ENC_USER_PASSWORD, &ui->username, err))
            return false;
        if (!url_unescape_step(b, pass, ENC_USER_PASSWORD, &ui->password, err))
            return false;
        ui->password_set = true;
    }
    u->user = ui;
    return true;
}

static Int url_count_byte(Str s, Byte c) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += s.p[i] == c;
    return n;
}

/* parse, on raw, which is already in the block u heads. */
static bool url_parse_into(Url *u, UrlBlock *b, Str raw, bool via_request, Error *err) {
    if (url_contains_ctl(raw)) {
        *err = url_errors_new("net/url: invalid control character in URL");
        return false;
    }
    if (raw.len == 0 && via_request) {
        *err = url_errors_new("empty url");
        return false;
    }
    if (raw.len == 1 && raw.p[0] == '*') {
        u->path = raw;
        return true;
    }

    Str scheme, rest;
    if (!url_get_scheme(raw, &scheme, &rest)) {
        *err = url_errors_new("missing protocol scheme");
        return false;
    }
    /* The scheme is ours to change, since raw is a copy. */
    Byte *sp = (Byte *)(uintptr_t)scheme.p;
    for (Int i = 0; i < scheme.len; i++)
        if (sp[i] >= 'A' && sp[i] <= 'Z')
            sp[i] = (Byte)(sp[i] + ('a' - 'A'));
    u->scheme = scheme;

    if (rest.len > 0 && rest.p[rest.len - 1] == '?' && url_count_byte(rest, '?') == 1) {
        u->force_query = true;
        rest.len--;
    } else {
        Int q = url_index_byte(rest, '?');
        if (q >= 0) {
            u->raw_query = url_sub(rest, q + 1, rest.len);
            rest = url_sub(rest, 0, q);
        }
    }

    bool slash = rest.len > 0 && rest.p[0] == '/';
    if (!slash) {
        if (scheme.len != 0) {
            /* A rootless path after a scheme is opaque, per RFC 3986. */
            u->opaque = rest;
            return true;
        }
        if (via_request) {
            *err = url_errors_new("invalid URI for request");
            return false;
        }
        /* A relative path cannot have a colon in its first segment, which
         * keeps cache_object:foo/bar from reading as one. */
        Int s = url_index_byte(rest, '/');
        Str segment = s < 0 ? rest : url_sub(rest, 0, s);
        if (url_index_byte(segment, ':') >= 0) {
            *err = url_errors_new("first path segment in URL cannot contain colon");
            return false;
        }
    }

    if ((scheme.len != 0 || (!via_request && !url_has_prefix(rest, "///", 3))) &&
        url_has_prefix(rest, "//", 2)) {
        Str authority = url_sub(rest, 2, rest.len);
        rest = BURROW_STR_EMPTY;
        Int i = url_index_byte(authority, '/');
        if (i >= 0) {
            rest = url_sub(authority, i, authority.len);
            authority = url_sub(authority, 0, i);
        }
        if (!url_parse_authority(u, b, authority, err))
            return false;
    } else if (scheme.len != 0 && slash) {
        u->omit_host = true;
    }
    return url_set_path(u, b, rest, err);
}

/* The parse error, as a UrlError round the reason. */
static Url *url_parse_fail(Alloc *a, Url *u, Str input, Error reason, Error *err) {
    url_free(a, u);
    BURROW_OUT(err,
               url_error_build(error_allocator(), URL_LIT("parse"), input, reason));
    return NULL;
}

/* The text that unescaping writes is never longer than what it came from, and
 * the only thing that writes to the block past the copy is unescaping, so
 * twice the input is always enough, and the input alone is when there is no
 * '%' in it. */
static Int url_parse_room(Str raw) {
    return url_index_byte(raw, '%') >= 0 ? 2 * raw.len : raw.len;
}

static Url *url_parse_common(Alloc *a, Str raw_url, bool via_request, Error *err) {
    UrlBlock b;
    Url *u = url_block_new(a, url_parse_room(raw_url), &b);
    if (u == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return NULL;
    }
    Str raw = blk_copy(&b, raw_url);

    Str before = raw;
    Str frag = BURROW_STR_EMPTY;
    bool has_frag = false;
    if (!via_request) {
        Int h = url_index_byte(raw, '#');
        if (h >= 0) {
            before = url_sub(raw, 0, h);
            frag = url_sub(raw, h + 1, raw.len);
            has_frag = true;
        }
    }

    Error reason = BURROW_NO_ERROR;
    if (!url_parse_into(u, &b, before, via_request, &reason))
        return url_parse_fail(a, u, url_sub(raw_url, 0, before.len), reason, err);
    if (has_frag && frag.len > 0 && !url_set_fragment(u, &b, frag, &reason))
        return url_parse_fail(a, u, raw_url, reason, err);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return u;
}

Url *url_parse(Alloc *a, Str raw_url, Error *err) {
    return url_parse_common(a, raw_url, false, err);
}

Url *url_parse_request_uri(Alloc *a, Str raw_url, Error *err) {
    return url_parse_common(a, raw_url, true, err);
}

/* ----------------------------------------------------------------- packing */

static Int url_text_len(const Url *u) {
    Int n = u->scheme.len + u->opaque.len + u->host.len + u->path.len +
            u->fragment.len + u->raw_query.len + u->raw_path.len + u->raw_fragment.len;
    if (u->user != NULL)
        n += u->user->username.len + u->user->password.len;
    return n;
}

/* A copy of src's fields in a new block with extra bytes spare after them. */
static Url *url_pack(Alloc *a, const Url *src, Int extra, UrlBlock *b) {
    Url *u = url_block_new(a, url_text_len(src) + extra, b);
    if (u == NULL)
        return NULL;
    void *mem = u->mem;
    size_t size = u->size;
    u->scheme = blk_copy(b, src->scheme);
    u->opaque = blk_copy(b, src->opaque);
    u->host = blk_copy(b, src->host);
    u->path = blk_copy(b, src->path);
    u->fragment = blk_copy(b, src->fragment);
    u->raw_query = blk_copy(b, src->raw_query);
    u->raw_path = blk_copy(b, src->raw_path);
    u->raw_fragment = blk_copy(b, src->raw_fragment);
    u->force_query = src->force_query;
    u->omit_host = src->omit_host;
    if (src->user != NULL) {
        UrlUserinfo *ui = url_block_user(u);
        ui->username = blk_copy(b, src->user->username);
        ui->password = blk_copy(b, src->user->password);
        ui->password_set = src->user->password_set;
        u->user = ui;
    }
    u->mem = mem;
    u->size = size;
    return u;
}

Url *url_clone(const Url *u, Alloc *a) {
    if (u == NULL)
        return NULL;
    UrlBlock b;
    return url_pack(a, u, 0, &b);
}

/* ------------------------------------------------------------------ String */

/* EscapedPath's choice: raw_path when it is a valid encoding of path. */
static bool url_raw_path_ok(const Url *u) {
    if (u->raw_path.len == 0)
        return false;
    Str s = u->raw_path;
    for (Int i = 0; i < s.len; i++) {
        switch (s.p[i]) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case ':':
        case '@':
        case '[':
        case ']':
        case '%':
            continue;
        default:
            if (url_should_escape(s.p[i], ENC_PATH))
                return false;
        }
    }
    return url_unescape_equals(s, ENC_PATH, u->path);
}

static bool url_raw_fragment_ok(const Url *u) {
    if (u->raw_fragment.len == 0)
        return false;
    Str s = u->raw_fragment;
    for (Int i = 0; i < s.len; i++) {
        switch (s.p[i]) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case ':':
        case '@':
        case '[':
        case ']':
        case '%':
            continue;
        default:
            if (url_should_escape(s.p[i], ENC_FRAGMENT))
                return false;
        }
    }
    return url_unescape_equals(s, ENC_FRAGMENT, u->fragment);
}

/* The escaped path as a piece of text, either a Str to copy as it is or path
 * to escape on the way out. */
typedef struct UrlPathOut {
    Str s;
    bool escape;
} UrlPathOut;

static UrlPathOut url_path_out(const Url *u) {
    if (url_raw_path_ok(u))
        return (UrlPathOut){u->raw_path, false};
    if (u->path.len == 1 && u->path.p[0] == '*')
        return (UrlPathOut){u->path, false};
    return (UrlPathOut){u->path, url_needs_escape(u->path, ENC_PATH)};
}

static Int url_path_out_len(UrlPathOut po) {
    return po.escape ? url_escape_len(po.s, ENC_PATH) : po.s.len;
}

/* The first byte of the escaped path, or -1 when it is empty. */
static int url_path_out_byte(UrlPathOut po, Int i) {
    if (!po.escape)
        return i < po.s.len ? po.s.p[i] : -1;
    /* Escaping only ever turns one byte into three starting with '%', so the
     * first two bytes of the output are easy to find. */
    Int j = 0;
    for (Int k = 0; k < po.s.len; k++) {
        Byte c = po.s.p[k];
        if (url_should_escape(c, ENC_PATH)) {
            if (i < j + 3)
                return i == j
                           ? '%'
                           : (i == j + 1 ? url_upperhex[c >> 4] : url_upperhex[c & 15]);
            j += 3;
        } else {
            if (i == j)
                return c;
            j++;
        }
    }
    return -1;
}

static void w_path_out(UrlW *w, UrlPathOut po, Int from) {
    if (!po.escape) {
        w_str(w, url_sub(po.s, from, po.s.len));
        return;
    }
    if (from == 0) {
        w_escape(w, po.s, ENC_PATH);
        return;
    }
    /* Only ever from 1, when the first byte is a '/', which is not escaped. */
    w_escape(w, url_sub(po.s, from, po.s.len), ENC_PATH);
}

/* Whether the first segment of the escaped path has a colon in it. */
static bool url_first_segment_colon(UrlPathOut po) {
    Int n = url_path_out_len(po);
    for (Int i = 0; i < n; i++) {
        int c = url_path_out_byte(po, i);
        if (c == '/')
            return false;
        if (c == ':')
            return true;
    }
    return false;
}

static void url_string_build(UrlW *w, const void *arg) {
    const Url *u = (const Url *)arg;
    Int start = w->n;
    if (u->scheme.len != 0) {
        w_str(w, u->scheme);
        w_byte(w, ':');
    }
    if (u->opaque.len != 0) {
        w_str(w, u->opaque);
    } else {
        if (u->scheme.len != 0 || u->host.len != 0 || u->user != NULL) {
            if (u->omit_host && u->host.len == 0 && u->user == NULL) {
                /* the empty host is left out */
            } else {
                if (u->host.len != 0 || u->path.len != 0 || u->user != NULL) {
                    w_byte(w, '/');
                    w_byte(w, '/');
                }
                if (u->user != NULL) {
                    w_userinfo(w, u->user);
                    w_byte(w, '@');
                }
                if (u->host.len != 0)
                    w_escape(w, u->host, ENC_HOST);
            }
        }
        UrlPathOut po = url_path_out(u);
        Int from = 0;
        if (u->omit_host && u->host.len == 0 && u->user == NULL &&
            url_path_out_byte(po, 0) == '/' && url_path_out_byte(po, 1) == '/') {
            /* A path starting "//" with no authority would parse back as one,
             * so the first slash is escaped. */
            w_str(w, URL_LIT("%2F"));
            from = 1;
        }
        int first = url_path_out_byte(po, from);
        if (first != -1 && first != '/' && u->host.len != 0)
            w_byte(w, '/');
        if (w->n == start && url_first_segment_colon(po)) {
            /* RFC 3986 section 4.2: a first segment with a colon in it would
             * read as a scheme, so it gets "./" in front. */
            w_byte(w, '.');
            w_byte(w, '/');
        }
        w_path_out(w, po, from);
    }
    if (u->force_query || u->raw_query.len != 0) {
        w_byte(w, '?');
        w_str(w, u->raw_query);
    }
    if (u->fragment.len != 0) {
        w_byte(w, '#');
        if (url_raw_fragment_ok(u))
            w_str(w, u->raw_fragment);
        else
            w_escape(w, u->fragment, ENC_FRAGMENT);
    }
}

Str url_string(const Url *u, Alloc *a) {
    return url_build(a, url_string_build, u);
}

Str url_redacted(const Url *u, Alloc *a) {
    if (u == NULL)
        return BURROW_STR_EMPTY;
    Url ru = *u;
    UrlUserinfo red;
    if (u->user != NULL && u->user->password_set) {
        red.username = u->user->username;
        red.password = URL_LIT("xxxxx");
        red.password_set = true;
        ru.user = &red;
    }
    return url_string(&ru, a);
}

Str url_escaped_path(const Url *u, Alloc *a) {
    UrlPathOut po = url_path_out(u);
    if (!po.escape)
        return po.s;
    return url_escape_alloc(a, po.s, ENC_PATH);
}

Str url_escaped_fragment(const Url *u, Alloc *a) {
    if (url_raw_fragment_ok(u))
        return u->raw_fragment;
    return url_escape_alloc(a, u->fragment, ENC_FRAGMENT);
}

bool url_is_abs(const Url *u) {
    return u->scheme.len != 0;
}

static void url_request_uri_build(UrlW *w, const void *arg) {
    const Url *u = (const Url *)arg;
    if (u->opaque.len == 0) {
        UrlPathOut po = url_path_out(u);
        if (url_path_out_len(po) == 0)
            w_byte(w, '/');
        else
            w_path_out(w, po, 0);
    } else {
        if (url_has_prefix(u->opaque, "//", 2)) {
            w_str(w, u->scheme);
            w_byte(w, ':');
        }
        w_str(w, u->opaque);
    }
    if (u->force_query || u->raw_query.len != 0) {
        w_byte(w, '?');
        w_str(w, u->raw_query);
    }
}

Str url_request_uri(const Url *u, Alloc *a) {
    if (!u->force_query && u->raw_query.len == 0) {
        if (u->opaque.len == 0) {
            UrlPathOut po = url_path_out(u);
            if (!po.escape && po.s.len != 0)
                return po.s;
        } else if (!url_has_prefix(u->opaque, "//", 2)) {
            return u->opaque;
        }
    }
    return url_build(a, url_request_uri_build, u);
}

/* splitHostPort. */
static void url_split_host_port(Str host_port, Str *host, Str *port) {
    *host = host_port;
    *port = BURROW_STR_EMPTY;
    Int colon = url_last_index_byte(host_port, ':');
    if (colon >= 0 &&
        url_valid_optional_port(url_sub(host_port, colon, host_port.len))) {
        *host = url_sub(host_port, 0, colon);
        *port = url_sub(host_port, colon + 1, host_port.len);
    }
    if (host->len >= 2 && host->p[0] == '[' && host->p[host->len - 1] == ']')
        *host = url_sub(*host, 1, host->len - 1);
    else if (host->len == 1 && host->p[0] == '[' && host->p[0] == ']')
        *host = BURROW_STR_EMPTY;
}

Str url_hostname(const Url *u) {
    Str host, port;
    url_split_host_port(u->host, &host, &port);
    return host;
}

Str url_port(const Url *u) {
    Str host, port;
    url_split_host_port(u->host, &host, &port);
    return port;
}

Slice url_append_binary(const Url *u, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    UrlW w = {NULL, 0};
    url_string_build(&w, u);
    if (b.elem == NULL)
        b.elem = TYPE_BYTE;
    if (b.cap - b.len < w.n) {
        Int want = b.len + w.n;
        Int cap = b.cap * 2 > want ? b.cap * 2 : want;
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
        if (p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return b;
        }
        if (b.len > 0)
            memcpy(p, b.p, (size_t)b.len);
        b = slice_from(p, b.len, cap, TYPE_BYTE);
    }
    UrlW w2 = {(Byte *)b.p + b.len, 0};
    url_string_build(&w2, u);
    b.len += w2.n;
    return b;
}

Slice url_marshal_binary(const Url *u, Alloc *a, Error *err) {
    return url_append_binary(u, a, slice_from(NULL, 0, 0, TYPE_BYTE), err);
}

Error url_unmarshal_binary(Url *u, Alloc *a, Slice text) {
    Error err;
    Url *got = url_parse(a, str_from_bytes(text.p, text.len), &err);
    if (BURROW_FAILED(err))
        return err;
    if (got == NULL)
        return burrow_err_out_of_memory;
    *u = *got;
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- resolving */

/* resolvePath, written to dst, which has room for base, ref and two more
 * bytes. Returns the length. */
static Int url_resolve_path(Byte *dst, Str base, Str ref) {
    /* full, in dst past where the output goes. The output is never longer
     * than full plus its leading slash, and it is written from the front, so
     * full goes at the back. */
    Str full;
    Byte *tmp;
    Int cap = base.len + ref.len + 2;
    tmp = dst + cap;
    if (ref.len == 0) {
        full = base;
    } else if (ref.p[0] != '/') {
        Int i = url_last_index_byte(base, '/');
        Int n = i + 1;
        url_put(url_put(tmp, base.p, n), ref.p, ref.len);
        full = str_from_bytes(tmp, n + ref.len);
    } else {
        full = ref;
    }
    if (full.len == 0)
        return 0;

    Int n = 0;
    dst[n++] = '/';
    Str elem = BURROW_STR_EMPTY;
    Str remaining = full;
    bool found = true;
    bool first = true;
    while (found) {
        Int i = url_index_byte(remaining, '/');
        if (i < 0) {
            elem = remaining;
            remaining = BURROW_STR_EMPTY;
            found = false;
        } else {
            elem = url_sub(remaining, 0, i);
            remaining = url_sub(remaining, i + 1, remaining.len);
        }
        if (elem.len == 1 && elem.p[0] == '.') {
            first = false;
            continue;
        }
        if (elem.len == 2 && elem.p[0] == '.' && elem.p[1] == '.') {
            /* Back to the last '/' after the leading one, and when there is
             * none, back to the leading one and a fresh start. */
            Int k = n - 1;
            while (k >= 1 && dst[k] != '/')
                k--;
            if (k >= 1) {
                n = k;
            } else {
                n = 1;
                first = true;
            }
            continue;
        }
        if (!first)
            dst[n++] = '/';
        if (elem.len > 0)
            memmove(dst + n, elem.p, (size_t)elem.len);
        n += elem.len;
        first = false;
    }
    if ((elem.len == 1 && elem.p[0] == '.') ||
        (elem.len == 2 && elem.p[0] == '.' && elem.p[1] == '.'))
        dst[n++] = '/';
    if (n > 1 && dst[1] == '/') {
        memmove(dst, dst + 1, (size_t)(n - 1));
        n--;
    }
    return n;
}

Str burrow__url_resolve_path(Alloc *a, Str base, Str ref) {
    Int cap = base.len + ref.len + 2;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(2 * cap), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    return str_from_bytes(p, url_resolve_path(p, base, ref));
}

bool burrow__url_should_escape(Byte c, int mode) {
    return url_should_escape(c, mode);
}

/* The escaped path of u as a Str, escaping into a scratch buffer when it has
 * to be escaped. */
static Str url_escaped_path_into(const Url *u, Byte *scratch) {
    UrlPathOut po = url_path_out(u);
    if (!po.escape)
        return po.s;
    return str_from_bytes(scratch, url_escape_write(scratch, po.s, ENC_PATH));
}

Url *url_resolve_reference(const Url *u, Alloc *a, const Url *ref) {
    Url tmp = *ref;
    tmp.mem = NULL;
    tmp.size = 0;
    if (ref->scheme.len == 0)
        tmp.scheme = u->scheme;

    bool abs = ref->scheme.len != 0 || ref->host.len != 0 || ref->user != NULL;
    if (!abs && ref->opaque.len != 0) {
        tmp.user = NULL;
        tmp.host = BURROW_STR_EMPTY;
        tmp.path = BURROW_STR_EMPTY;
        UrlBlock b;
        return url_pack(a, &tmp, 0, &b);
    }
    if (!abs) {
        if (ref->path.len == 0 && !ref->force_query && ref->raw_query.len == 0) {
            tmp.raw_query = u->raw_query;
            if (ref->fragment.len == 0) {
                tmp.fragment = u->fragment;
                tmp.raw_fragment = u->raw_fragment;
            }
        }
        if (ref->path.len == 0 && u->opaque.len != 0) {
            tmp.opaque = u->opaque;
            tmp.user = NULL;
            tmp.host = BURROW_STR_EMPTY;
            tmp.path = BURROW_STR_EMPTY;
            UrlBlock b;
            return url_pack(a, &tmp, 0, &b);
        }
        tmp.host = u->host;
        tmp.user = u->user;
    }

    /* The escaped paths, the resolved path, and the unescaped copy setPath
     * may make of it. Escaping at most triples a path. */
    Str ref_path_src = ref->path.len > ref->raw_path.len ? ref->path : ref->raw_path;
    Str base_path_src = u->path.len > u->raw_path.len ? u->path : u->raw_path;
    Int esc_room = 3 * ref_path_src.len + (abs ? 0 : 3 * base_path_src.len);
    Int resolved_room = 2 * (esc_room + 2);
    tmp.path = BURROW_STR_EMPTY;
    tmp.raw_path = BURROW_STR_EMPTY;
    UrlBlock b;
    Url *out = url_pack(a, &tmp, esc_room + resolved_room + esc_room + 2, &b);
    if (out == NULL)
        return NULL;

    Byte *scratch = b.p;
    Str ref_esc = url_escaped_path_into(ref, scratch);
    Byte *scratch2 = scratch + (ref_esc.p == scratch ? ref_esc.len : 0);
    Str base_esc = BURROW_STR_EMPTY;
    if (!abs)
        base_esc = url_escaped_path_into(u, scratch2);
    Byte *scratch3 = scratch2 + (base_esc.p == scratch2 ? base_esc.len : 0);
    b.p = scratch3 + esc_room + 2 + esc_room + 2;

    Int n = abs ? url_resolve_path(b.p, ref_esc, BURROW_STR_EMPTY)
                : url_resolve_path(b.p, base_esc, ref_esc);
    Str resolved = str_from_bytes(b.p, n);
    b.p += n;
    /* The path is validly escaped, so setPath cannot fail. */
    Error ignored;
    (void)url_set_path(out, &b, resolved, &ignored);
    return out;
}

Url *url_parse_ref(const Url *u, Alloc *a, Str ref, Error *err) {
    Url *r = url_parse(a, ref, err);
    if (r == NULL)
        return NULL;
    Url *out = url_resolve_reference(u, a, r);
    url_free(a, r);
    return out;
}

/* ----------------------------------------------------------------- joining */

/* path.Clean, from src to dst, which has room for src.len bytes or one when
 * src is empty. Returns the length. */
static Int url_clean(Byte *dst, Str src) {
    if (src.len == 0) {
        dst[0] = '.';
        return 1;
    }
    bool rooted = src.p[0] == '/';
    Int n = src.len;
    Int r = 0, w = 0, dotdot = 0;
    if (rooted) {
        dst[w++] = '/';
        r = 1;
        dotdot = 1;
    }
    while (r < n) {
        if (src.p[r] == '/') {
            r++;
        } else if (src.p[r] == '.' && (r + 1 == n || src.p[r + 1] == '/')) {
            r++;
        } else if (src.p[r] == '.' && src.p[r + 1] == '.' &&
                   (r + 2 == n || src.p[r + 2] == '/')) {
            r += 2;
            if (w > dotdot) {
                w--;
                while (w > dotdot && dst[w] != '/')
                    w--;
            } else if (!rooted) {
                if (w > 0)
                    dst[w++] = '/';
                dst[w++] = '.';
                dst[w++] = '.';
                dotdot = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0))
                dst[w++] = '/';
            for (; r < n && src.p[r] != '/'; r++)
                dst[w++] = src.p[r];
        }
    }
    if (w == 0)
        dst[w++] = '.';
    return w;
}

static Url *url_join_path_common(const Url *u, Alloc *a, const Str *elem, Int count,
                                 Error *err) {
    /* elem[0] is u's escaped path, with a "/" in front when it has none, and
     * the join is the non-empty elements with "/" between them, cleaned. */
    Str src = u->path.len > u->raw_path.len ? u->path : u->raw_path;
    Int total = 3 * src.len + 1;
    for (Int i = 0; i < count; i++)
        total += elem[i].len + 1;
    Url tmp = *u;
    tmp.path = BURROW_STR_EMPTY;
    tmp.raw_path = BURROW_STR_EMPTY;
    UrlBlock b;
    Url *out = url_pack(a, &tmp, 3 * src.len + total + (total + 2) + (total + 2), &b);
    if (out == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return NULL;
    }

    Byte *scratch = b.p;
    Str first = url_escaped_path_into(u, scratch);
    Byte *joined = scratch + 3 * src.len;
    /* Go puts "/" in front of a relative first element and cuts it off the
     * result, so that the join cannot climb out with "..". */
    bool relative = first.len == 0 || first.p[0] != '/';
    Int j = 0;
    if (relative)
        joined[j++] = '/';
    if (first.len > 0) {
        memcpy(joined + j, first.p, (size_t)first.len);
        j += first.len;
    }
    Str head = str_from_bytes(joined, j);
    for (Int i = 0; i < count; i++) {
        if (elem[i].len == 0)
            continue;
        if (j > 0)
            joined[j++] = '/';
        memcpy(joined + j, elem[i].p, (size_t)elem[i].len);
        j += elem[i].len;
    }
    /* path.Join gives "" when every element is empty. */
    Byte *cleaned = joined + total;
    Int c = j == 0 ? 0 : url_clean(cleaned, str_from_bytes(joined, j));
    Int from = relative ? 1 : 0;
    /* path.Join drops a trailing slash, and one is put back when the last
     * element, which is the first one with no others, had it. */
    Str last = count > 0 ? elem[count - 1] : head;
    if (last.len > 0 && last.p[last.len - 1] == '/' &&
        (c == from || cleaned[c - 1] != '/'))
        cleaned[c++] = '/';
    Str p = str_from_bytes(cleaned + from, c - from);
    b.p = cleaned + total + 2;
    Error e = BURROW_NO_ERROR;
    if (!url_set_path(out, &b, p, &e)) {
        url_free(a, out);
        /* JoinPath drops the error and hands back an unchanged copy. */
        if (err == NULL)
            return BURROW_OK(e) ? NULL : url_clone(u, a);
        *err = e;
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

Url *url_join_path(const Url *u, Alloc *a, Slice elem) {
    return url_join_path_common(u, a, (const Str *)elem.p, elem.len, NULL);
}

Url *url_join_path_v(const Url *u, Alloc *a, int n, ...) {
    Str small[8];
    Str *elem = small;
    if (n > 8) {
        elem = (Str *)mem_alloc_nozero(a, sizeof(Str) * (size_t)n, _Alignof(Str));
        if (elem == NULL)
            return NULL;
    }
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        elem[i] = va_arg(ap, Str);
    va_end(ap);
    Url *out = url_join_path_common(u, a, elem, n > 0 ? n : 0, NULL);
    if (elem != small)
        mem_free(a, elem, sizeof(Str) * (size_t)n, _Alignof(Str));
    return out;
}

Str url_join_path_str(Alloc *a, Str base, Slice elem, Error *err) {
    Url *u = url_parse(a, base, err);
    if (u == NULL)
        return BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    Url *res = url_join_path_common(u, a, (const Str *)elem.p, elem.len, &e);
    url_free(a, u);
    if (res == NULL) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    Str s = url_string(res, a);
    url_free(a, res);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s;
}

/* ------------------------------------------------------------------- Values */

/* []string, the value type of the map. */
static const Type url_strings_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_STRING,
    NULL,
    0,
    0x75726c73U, /* "urls" */
    NULL,
};

UrlValues url_values_make(Alloc *a) {
    return map_make(a, TYPE_STRING, &url_strings_desc, 0);
}

Str url_values_get(UrlValues v, Str key) {
    Slice *vs = (Slice *)map_get(v, &key);
    if (vs == NULL || vs->len == 0)
        return BURROW_STR_EMPTY;
    return ((const Str *)vs->p)[0];
}

Slice url_values_get_all(UrlValues v, Str key) {
    Slice *vs = (Slice *)map_get(v, &key);
    if (vs == NULL)
        return slice_from(NULL, 0, 0, TYPE_STRING);
    return *vs;
}

bool url_values_set(UrlValues v, Str key, Str value) {
    Alloc *a = burrow__map_allocator(v);
    Str *p = (Str *)mem_alloc_nozero(a, sizeof(Str), _Alignof(Str));
    if (p == NULL)
        return false;
    *p = value;
    Slice s = slice_from(p, 1, 1, TYPE_STRING);
    if (!map_set(v, &key, &s)) {
        mem_free(a, p, sizeof(Str), _Alignof(Str));
        return false;
    }
    return true;
}

bool url_values_add(UrlValues v, Str key, Str value) {
    Alloc *a = burrow__map_allocator(v);
    Slice *vs = (Slice *)map_get(v, &key);
    if (vs != NULL) {
        Slice grown = slice_append(a, *vs, &value, 1);
        if (grown.len != vs->len + 1)
            return false;
        *vs = grown;
        return true;
    }
    Slice s = slice_append(a, slice_from(NULL, 0, 0, TYPE_STRING), &value, 1);
    if (s.len != 1)
        return false;
    if (!map_set(v, &key, &s)) {
        mem_free(a, s.p, sizeof(Str) * (size_t)s.cap, _Alignof(Str));
        return false;
    }
    return true;
}

void url_values_del(UrlValues v, Str key) {
    map_del(v, &key);
}

bool url_values_has(UrlValues v, Str key) {
    return map_get(v, &key) != NULL;
}

UrlValues url_values_clone(UrlValues v, Alloc *a) {
    if (v == NULL)
        return NULL;
    UrlValues out = map_make(a, TYPE_STRING, &url_strings_desc, map_len(v));
    if (out == NULL)
        return NULL;
    const void *k;
    void *val;
    for (MapIter it = map_iter(v); map_next(&it, &k, &val);) {
        const Slice *vs = (const Slice *)val;
        Slice c = slice_from(NULL, 0, 0, TYPE_STRING);
        if (vs->p != NULL) {
            /* slices.Clone keeps a nil slice nil and an empty one empty. */
            Str *p = (Str *)mem_alloc_nozero(
                a, sizeof(Str) * (size_t)(vs->len > 0 ? vs->len : 1), _Alignof(Str));
            if (p == NULL)
                return NULL;
            if (vs->len > 0)
                memcpy(p, vs->p, sizeof(Str) * (size_t)vs->len);
            c = slice_from(p, vs->len, vs->len, TYPE_STRING);
        }
        if (!map_set(out, k, &c))
            return NULL;
    }
    return out;
}

typedef struct UrlEncodeArg {
    UrlValues v;
    const Str *keys;
    Int n;
} UrlEncodeArg;

static void url_encode_build(UrlW *w, const void *arg) {
    const UrlEncodeArg *e = (const UrlEncodeArg *)arg;
    Int start = w->n;
    for (Int i = 0; i < e->n; i++) {
        Str k = e->keys[i];
        const Slice *vs = (const Slice *)map_get(e->v, &k);
        for (Int j = 0; j < vs->len; j++) {
            if (w->n > start)
                w_byte(w, '&');
            w_escape(w, k, ENC_QUERY_COMPONENT);
            w_byte(w, '=');
            w_escape(w, ((const Str *)vs->p)[j], ENC_QUERY_COMPONENT);
        }
    }
}

Str url_values_encode(UrlValues v, Alloc *a) {
    Int n = map_len(v);
    if (n == 0)
        return BURROW_STR_EMPTY;
    Str small[16];
    Str *keys = small;
    if (n > 16) {
        keys = (Str *)mem_alloc_nozero(a, sizeof(Str) * (size_t)n, _Alignof(Str));
        if (keys == NULL)
            return BURROW_STR_EMPTY;
    }
    Int i = 0;
    const void *k;
    for (MapIter it = map_iter(v); map_next(&it, &k, NULL);)
        keys[i++] = *(const Str *)k;
    slices_sort(slice_from(keys, n, n, TYPE_STRING));
    UrlEncodeArg arg = {v, keys, n};
    Str out = url_build(a, url_encode_build, &arg);
    if (keys != small)
        mem_free(a, keys, sizeof(Str) * (size_t)n, _Alignof(Str));
    return out;
}

UrlValues url_parse_query(Alloc *a, Str query, Error *err) {
    UrlValues m = url_values_make(a);
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (m == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (!url_params_within_max(url_count_byte(query, '&') + 1)) {
        BURROW_OUT(err,
                   url_errors_new("number of URL query parameters exceeded limit"));
        return m;
    }
    Error first = BURROW_NO_ERROR;
    while (query.len > 0) {
        Str key;
        Int amp = url_index_byte(query, '&');
        if (amp < 0) {
            key = query;
            query = BURROW_STR_EMPTY;
        } else {
            key = url_sub(query, 0, amp);
            query = url_sub(query, amp + 1, query.len);
        }
        if (url_index_byte(key, ';') >= 0) {
            first = url_errors_new("invalid semicolon separator in query");
            continue;
        }
        if (key.len == 0)
            continue;
        Str value = BURROW_STR_EMPTY;
        Int eq = url_index_byte(key, '=');
        if (eq >= 0) {
            value = url_sub(key, eq + 1, key.len);
            key = url_sub(key, 0, eq);
        }
        Error e;
        key = url_unescape_alloc(a, key, ENC_QUERY_COMPONENT, &e);
        if (BURROW_FAILED(e)) {
            if (BURROW_OK(first))
                first = e;
            continue;
        }
        value = url_unescape_alloc(a, value, ENC_QUERY_COMPONENT, &e);
        if (BURROW_FAILED(e)) {
            if (BURROW_OK(first))
                first = e;
            continue;
        }
        if (!url_values_add(m, key, value)) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return m;
        }
    }
    BURROW_OUT(err, first);
    return m;
}

UrlValues url_query(const Url *u, Alloc *a) {
    return url_parse_query(a, u->raw_query, NULL);
}

/* ------------------------------------------------------------ the type */

static Slice url_m_marshal_binary(Url *self, Alloc *a, Error *err) {
    return url_marshal_binary(self, a, err);
}

static Slice url_m_append_binary(Url *self, Alloc *a, Slice b, Error *err) {
    return url_append_binary(self, a, b, err);
}

static Error url_m_unmarshal_binary(Url *self, Alloc *a, Slice data) {
    return url_unmarshal_binary(self, a, data);
}

static Str url_m_string(Url *self) {
    return url_string(self, error_allocator());
}

#define URL_SIG_STRING(IN, OUT) OUT(Str)

#define URL_METHODS(M, T)                                                              \
    M(T, AppendBinary, url_m_append_binary, ENCODING_SIG_APPEND_BINARY)                \
    M(T, MarshalBinary, url_m_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)             \
    M(T, String, url_m_string, URL_SIG_STRING)                                         \
    M(T, UnmarshalBinary, url_m_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)

BURROW_METHODS_DEFINE(Url, URL_METHODS);

const Type burrow_type_Url = {
    {(const Byte *)"URL", 3},
    {(const Byte *)"net/url", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(Url),
    (uint16_t)_Alignof(Url),
    0,
    (uint16_t)(sizeof burrow__methods_Url / sizeof burrow__methods_Url[0]),
    NULL,
    burrow__methods_Url,
    NULL,
    NULL,
    0,
    0x75726c75U, /* "urlu" */
    NULL,
};

const Type *const TYPE_URL = &burrow_type_Url;
