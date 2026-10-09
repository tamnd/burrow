/* Derived from Go's src/net/http/cookie.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "http_ascii.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/netip.h"
#include "burrow/net/textproto.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const Type hc_cookie_desc = {
    {(const Byte *)"Cookie", 6},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(HttpCookie),
    (uint16_t)_Alignof(HttpCookie),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6874636bU, /* "htck" */
    NULL,
};

const Type *const TYPE_HTTP_COOKIE = &hc_cookie_desc;

BURROW_SENTINEL_ERROR(burrow__http_err_blank_cookie, "http: blank cookie");
BURROW_SENTINEL_ERROR(burrow__http_err_equal_not_found_in_cookie,
                      "http: '=' not found in cookie");
BURROW_SENTINEL_ERROR(burrow__http_err_invalid_cookie_name,
                      "http: invalid cookie name");
BURROW_SENTINEL_ERROR(burrow__http_err_invalid_cookie_value,
                      "http: invalid cookie value");
BURROW_SENTINEL_ERROR(burrow__http_err_cookie_num_limit_exceeded,
                      "http: number of cookies exceeded limit");

/* The errors Valid makes with errors.New each time. */
static const Str hc_text_nil = BURROW_S_INIT("http: nil Cookie");
static const Str hc_text_name = BURROW_S_INIT("http: invalid Cookie.Name");
static const Str hc_text_expires = BURROW_S_INIT("http: invalid Cookie.Expires");
static const Str hc_text_domain = BURROW_S_INIT("http: invalid Cookie.Domain");
static const Str hc_text_partitioned =
    BURROW_S_INIT("http: partitioned cookies must be set with Secure");

static Error hc_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

/* ------------------------------------------------------------------ GODEBUG
 *
 * The settings net/http reads from GODEBUG: httpcookiemaxnum here,
 * httplaxcontentlength for http_transfer.c, httpmuxgo121 for the mux and
 * httpservecontentkeepheaders for http_fs.c. */

enum {
    HC_DEBUG_KNOWN = 1 << 0,
    HC_DEBUG_MAX_SET = 1 << 1,      /* httpcookiemaxnum is a number */
    HC_DEBUG_LAX_CL = 1 << 2,       /* httplaxcontentlength=1 */
    HC_DEBUG_MUX121 = 1 << 3,       /* httpmuxgo121=1 */
    HC_DEBUG_KEEP_HEADERS = 1 << 4, /* httpservecontentkeepheaders=1 */
};

static uint32_t hc_debug_flags;
static uint64_t hc_debug_max;

/* The value of key in GODEBUG, the last one when it is there twice, as Go's
 * internal/godebug reads it. */
static bool hc_godebug(const char *env, const char *key, Str *val) {
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
static uint32_t hc_debug_parse(const char *v) {
    uint32_t f = HC_DEBUG_KNOWN;
    uint64_t max = 0;
    Str s;
    if (v != NULL && hc_godebug(v, "httpcookiemaxnum", &s) && s.len > 0) {
        /* strconv.Atoi, and a value that is not a number leaves the
         * default in place. */
        Error err;
        Int n = strconv_atoi(s, &err);
        if (BURROW_OK(err)) {
            f |= HC_DEBUG_MAX_SET;
            max = (uint64_t)n;
        }
    }
    if (v != NULL && hc_godebug(v, "httplaxcontentlength", &s) &&
        str_eq(s, BURROW_S("1")))
        f |= HC_DEBUG_LAX_CL;
    if (v != NULL && hc_godebug(v, "httpmuxgo121", &s) && str_eq(s, BURROW_S("1")))
        f |= HC_DEBUG_MUX121;
    if (v != NULL && hc_godebug(v, "httpservecontentkeepheaders", &s) &&
        str_eq(s, BURROW_S("1")))
        f |= HC_DEBUG_KEEP_HEADERS;
    burrow__atomic64_store(&hc_debug_max, max);
    burrow__atomic_store_relaxed_u32(&hc_debug_flags, f);
    return f;
}

static uint32_t hc_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&hc_debug_flags);
    if ((f & HC_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return hc_debug_parse(v);
}

void burrow__http_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&hc_debug_flags, 0);
    else
        (void)hc_debug_parse(value);
}

bool burrow__http_godebug_lax_content_length(void) {
    return (hc_debug_load() & HC_DEBUG_LAX_CL) != 0;
}

bool burrow__http_godebug_mux121(void) {
    return (hc_debug_load() & HC_DEBUG_MUX121) != 0;
}

bool burrow__http_godebug_serve_content_keep_headers(void) {
    return (hc_debug_load() & HC_DEBUG_KEEP_HEADERS) != 0;
}

/* cookieNumWithinMax. */
static bool hc_num_within_max(Int n) {
    bool within_default = n <= BURROW__HTTP_DEFAULT_COOKIE_MAX_NUM;
    uint32_t f = hc_debug_load();
    if ((f & HC_DEBUG_MAX_SET) == 0)
        return within_default;
    int64_t custom = (int64_t)burrow__atomic64_load(&hc_debug_max);
    return custom == 0 || (int64_t)n <= custom;
}

/* ------------------------------------------------------------------ helpers */

static bool hc_valid_value_byte(Byte b) {
    return 0x20 <= b && b < 0x7f && b != '"' && b != ';' && b != '\\';
}

static bool hc_valid_path_byte(Byte b) {
    return 0x20 <= b && b < 0x7f && b != ';';
}

/* parseCookieValue. The value with its quotes taken off when allow_quote says
 * it may have them, and false when a byte in it cannot be in a cookie. */
static bool hc_parse_value(Str raw, bool allow_quote, Str *value, bool *quoted) {
    *quoted = false;
    if (allow_quote && raw.len > 1 && raw.p[0] == '"' && raw.p[raw.len - 1] == '"') {
        raw = str_from_bytes(raw.p + 1, raw.len - 2);
        *quoted = true;
    }
    for (Int i = 0; i < raw.len; i++) {
        if (!hc_valid_value_byte(raw.p[i])) {
            *value = BURROW_STR_EMPTY;
            return false;
        }
    }
    *value = raw;
    return true;
}

/* isCookieDomainName. Go's isDomainName from package net, less the '_', and
 * with a leading dot allowed. */
static bool hc_is_domain_name(Str s) {
    if (s.len == 0 || s.len > 255)
        return false;
    if (s.p[0] == '.')
        s = str_from_bytes(s.p + 1, s.len - 1);
    Byte last = '.';
    bool ok = false; /* once there has been a letter */
    Int partlen = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) {
            ok = true;
            partlen++;
        } else if ('0' <= c && c <= '9') {
            partlen++;
        } else if (c == '-') {
            /* The byte before a dash cannot be a dot. */
            if (last == '.')
                return false;
            partlen++;
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
    return ok;
}

/* validCookieDomain. A host name, or an IPv4 address. net.ParseIP and
 * netip_parse_addr agree on everything without a colon, which is all that
 * gets this far. */
static bool hc_valid_domain(Str v) {
    if (hc_is_domain_name(v))
        return true;
    Error err;
    (void)netip_parse_addr(v, &err);
    return BURROW_OK(err) && !strings_contains(v, BURROW_S(":"));
}

/* validCookieExpires. RFC 6265 5.1.1 says the year is 1601 or later. */
static bool hc_valid_expires(Time t) {
    return time_year(t) >= 1601;
}

/* sanitizeOrWarn, with the quoting sanitizeCookieValue does as well. v
 * without the bytes valid says no to, in double quotes when quote says so,
 * with the first bad byte reported on the standard logger. The result is v
 * when nothing changes, and otherwise exactly as long as its allocation from
 * a, which is empty when a says no. */
static Str hc_sanitize(Alloc *a, Str field, bool (*valid)(Byte), Str v, bool quote) {
    Int n = 0;
    bool bad = false;
    for (Int i = 0; i < v.len; i++) {
        if (valid(v.p[i])) {
            n++;
        } else if (!bad) {
            log_printf_v("net/http: invalid byte %q in %s; dropping invalid bytes",
                         v.p[i], field);
            bad = true;
        }
    }
    if (!bad && !quote)
        return v;
    Int size = n + (quote ? 2 : 0);
    if (size == 0)
        return BURROW_STR_EMPTY;
    Byte *buf = (Byte *)mem_alloc(a, (size_t)size, 1);
    if (buf == NULL)
        return BURROW_STR_EMPTY;
    Int j = 0;
    if (quote)
        buf[j++] = '"';
    for (Int i = 0; i < v.len; i++) {
        if (valid(v.p[i]))
            buf[j++] = v.p[i];
    }
    if (quote)
        buf[j++] = '"';
    return str_from_bytes(buf, j);
}

Str burrow__http_sanitize_cookie_value(Alloc *a, Str v, bool quoted) {
    /* A space or comma is a valid byte, so it is still there after the
     * others go. Go quotes such a value, since they are common. */
    bool quote = quoted || strings_contains_any(v, BURROW_S(" ,"));
    return hc_sanitize(a, BURROW_S("Cookie.Value"), hc_valid_value_byte, v, quote);
}

Str burrow__http_sanitize_cookie_path(Alloc *a, Str v) {
    return hc_sanitize(a, BURROW_S("Cookie.Path"), hc_valid_path_byte, v, false);
}

/* Gives back what hc_sanitize made, when it made anything. */
static void hc_sanitized_free(Alloc *a, Str s, Str v) {
    if (s.p != v.p && s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* ------------------------------------------------------------------ parsing */

/* Gives back the array of a Slice from slice_make or slice_append. */
static void hc_slice_free(Alloc *a, Slice s) {
    if (s.p != NULL && s.cap > 0)
        mem_free(a, s.p, (size_t)s.cap * s.elem->size, s.elem->align);
}

static Slice hc_make(Alloc *a, Int cap) {
    return slice_make(a, TYPE_HTTP_COOKIE, 0, cap);
}

Slice http_parse_cookie(Alloc *a, Str line, Error *err) {
    Slice none = {0};
    Int nparts = strings_count(line, BURROW_S(";")) + 1;
    if (!hc_num_within_max(nparts)) {
        BURROW_OUT(err, burrow__http_err_cookie_num_limit_exceeded);
        return none;
    }
    if (nparts == 1 && textproto_trim_string(line).len == 0) {
        BURROW_OUT(err, burrow__http_err_blank_cookie);
        return none;
    }
    Slice cookies = hc_make(a, nparts);
    if (cookies.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    Str rest = line;
    for (bool more = true; more;) {
        Str s = strings_cut(rest, BURROW_S(";"), &rest, &more);
        s = textproto_trim_string(s);
        Str value;
        bool found;
        Str name = strings_cut(s, BURROW_S("="), &value, &found);
        Error e = BURROW_NO_ERROR;
        bool quoted = false;
        if (!found)
            e = burrow__http_err_equal_not_found_in_cookie;
        else if (!burrow__http_is_token(name))
            e = burrow__http_err_invalid_cookie_name;
        else if (!hc_parse_value(value, true, &value, &quoted))
            e = burrow__http_err_invalid_cookie_value;
        if (BURROW_FAILED(e)) {
            hc_slice_free(a, cookies);
            BURROW_OUT(err, e);
            return none;
        }
        HttpCookie c = {0};
        c.name = name;
        c.value = value;
        c.quoted = quoted;
        /* The capacity is the number of parts, so this never grows. */
        cookies = slice_append(a, cookies, &c, 1);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return cookies;
}

/* An Expires value, in the first of the two layouts Go tries that reads it,
 * and in UTC. A zone the parse makes comes from a scratch arena that goes
 * when the time is in UTC. */
static bool hc_parse_expires(Alloc *a, Str val, Time *t) {
    static const Str layouts[] = {
        BURROW_S_INIT("Mon, 02 Jan 2006 15:04:05 MST"),
        BURROW_S_INIT("Mon, 02-Jan-2006 15:04:05 MST"),
    };
    Arena ar;
    arena_init(&ar, a, 0);
    bool ok = false;
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0] && !ok; i++) {
        Error err;
        Time p = time_parse(arena_allocator(&ar), layouts[i], val, &err);
        if (BURROW_OK(err)) {
            *t = time_utc(p);
            ok = true;
        }
    }
    arena_free(&ar);
    return ok;
}

/* The attribute name, which is printable ASCII, as one of Go's lower case
 * names. */
static bool hc_attr_is(Str attr, const char *name) {
    return burrow__http_ascii_equal_fold(attr, str_from_cstr(name));
}

static bool hc_add_unparsed(Alloc *a, HttpCookie *c, Str part) {
    Slice u = c->unparsed.elem == NULL ? slice_make(a, TYPE_STRING, 0, 2) : c->unparsed;
    if (u.elem == NULL)
        return false;
    Slice v = slice_append(a, u, &part, 1);
    if (v.len != u.len + 1)
        return false;
    c->unparsed = v;
    return true;
}

HttpCookie http_parse_set_cookie(Alloc *a, Str line, Error *err) {
    HttpCookie c = {0};
    Str rest = textproto_trim_string(line);
    bool more;
    Str first = strings_cut(rest, BURROW_S(";"), &rest, &more);
    if (!more && first.len == 0) {
        BURROW_OUT(err, burrow__http_err_blank_cookie);
        return c;
    }
    first = textproto_trim_string(first);
    Str value;
    bool ok;
    Str name = strings_cut(first, BURROW_S("="), &value, &ok);
    if (!ok) {
        BURROW_OUT(err, burrow__http_err_equal_not_found_in_cookie);
        return c;
    }
    name = textproto_trim_string(name);
    if (!burrow__http_is_token(name)) {
        BURROW_OUT(err, burrow__http_err_invalid_cookie_name);
        return c;
    }
    bool quoted;
    if (!hc_parse_value(value, true, &value, &quoted)) {
        BURROW_OUT(err, burrow__http_err_invalid_cookie_value);
        return c;
    }
    c.name = name;
    c.value = value;
    c.quoted = quoted;
    c.raw = line;
    while (more) {
        Str part = strings_cut(rest, BURROW_S(";"), &rest, &more);
        part = textproto_trim_string(part);
        if (part.len == 0)
            continue;
        Str val;
        Str attr = strings_cut(part, BURROW_S("="), &val, &ok);
        if (!burrow__http_ascii_is_print(attr))
            continue;
        bool q;
        bool known = false;
        if (hc_parse_value(val, false, &val, &q)) {
            known = true;
            if (hc_attr_is(attr, "samesite")) {
                /* Anything else, and anything not ASCII, is the default. */
                c.same_site = HTTP_SAME_SITE_DEFAULT_MODE;
                if (burrow__http_ascii_is_print(val)) {
                    if (hc_attr_is(val, "lax"))
                        c.same_site = HTTP_SAME_SITE_LAX_MODE;
                    else if (hc_attr_is(val, "strict"))
                        c.same_site = HTTP_SAME_SITE_STRICT_MODE;
                    else if (hc_attr_is(val, "none"))
                        c.same_site = HTTP_SAME_SITE_NONE_MODE;
                }
            } else if (hc_attr_is(attr, "secure")) {
                c.secure = true;
            } else if (hc_attr_is(attr, "httponly")) {
                c.http_only = true;
            } else if (hc_attr_is(attr, "domain")) {
                c.domain = val;
            } else if (hc_attr_is(attr, "max-age")) {
                Error e;
                Int secs = strconv_atoi(val, &e);
                if (BURROW_FAILED(e) || (secs != 0 && val.p[0] == '0'))
                    known = false;
                else
                    c.max_age = secs <= 0 ? -1 : secs;
            } else if (hc_attr_is(attr, "expires")) {
                c.raw_expires = val;
                if (!hc_parse_expires(a, val, &c.expires)) {
                    c.expires = (Time){0};
                    known = false;
                }
            } else if (hc_attr_is(attr, "path")) {
                c.path = val;
            } else if (hc_attr_is(attr, "partitioned")) {
                c.partitioned = true;
            } else {
                known = false;
            }
        }
        if (!known && !hc_add_unparsed(a, &c, part)) {
            http_cookie_free(a, &c);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (HttpCookie){0};
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

void http_cookie_free(Alloc *a, HttpCookie *c) {
    if (c == NULL)
        return;
    hc_slice_free(a, c->unparsed);
    c->unparsed = (Slice){0};
}

/* Every value of key in h, without making key canonical, as Go's h[key]. */
static Slice hc_values(HttpHeader h, Str key) {
    if (h == NULL)
        return (Slice){0};
    const Slice *v = (const Slice *)map_get(h, &key);
    return v == NULL ? (Slice){0} : *v;
}

Slice burrow__http_read_set_cookies(Alloc *a, HttpHeader h) {
    Slice lines = hc_values(h, BURROW_S("Set-Cookie"));
    if (lines.len == 0 || !hc_num_within_max(lines.len))
        return hc_make(a, 0);
    Slice cookies = hc_make(a, lines.len);
    if (cookies.p == NULL)
        return cookies;
    for (Int i = 0; i < lines.len; i++) {
        Error err;
        HttpCookie c = http_parse_set_cookie(a, ((const Str *)lines.p)[i], &err);
        if (BURROW_OK(err))
            cookies = slice_append(a, cookies, &c, 1);
    }
    return cookies;
}

Slice burrow__http_read_cookies(Alloc *a, HttpHeader h, Str filter) {
    Slice lines = hc_values(h, BURROW_S("Cookie"));
    if (lines.len == 0)
        return hc_make(a, 0);
    const Str *l = (const Str *)lines.p;
    Int count = 0;
    for (Int i = 0; i < lines.len; i++)
        count += strings_count(l[i], BURROW_S(";")) + 1;
    if (!hc_num_within_max(count))
        return hc_make(a, 0);
    Slice cookies = hc_make(a, lines.len + strings_count(l[0], BURROW_S(";")));
    if (cookies.p == NULL)
        return cookies;
    for (Int i = 0; i < lines.len; i++) {
        Str line = textproto_trim_string(l[i]);
        while (line.len > 0) {
            bool more;
            Str part = strings_cut(line, BURROW_S(";"), &line, &more);
            part = textproto_trim_string(part);
            if (part.len == 0)
                continue;
            Str val;
            bool found;
            Str name = strings_cut(part, BURROW_S("="), &val, &found);
            name = textproto_trim_string(name);
            if (!burrow__http_is_token(name))
                continue;
            if (filter.len > 0 && !str_eq(filter, name))
                continue;
            bool quoted;
            if (!hc_parse_value(val, true, &val, &quoted))
                continue;
            HttpCookie c = {0};
            c.name = name;
            c.value = val;
            c.quoted = quoted;
            Slice next = slice_append(a, cookies, &c, 1);
            if (next.len != cookies.len + 1)
                return cookies;
            cookies = next;
        }
    }
    return cookies;
}

/* ------------------------------------------------------------------ writing */

/* A byte buffer that remembers whether a write failed. */
typedef struct HcBuf {
    Slice b;
    Alloc *a;
    bool oom;
} HcBuf;

static void hc_put(HcBuf *b, Str s) {
    if (b->oom || s.len == 0)
        return;
    Slice n = slice_append(b->a, b->b, s.p, s.len);
    if (n.len != b->b.len + s.len) {
        b->oom = true;
        return;
    }
    b->b = n;
}

Str http_cookie_string(Alloc *a, const HttpCookie *c) {
    if (c == NULL || !burrow__http_is_token(c->name))
        return BURROW_STR_EMPTY;
    /* extraCookieLength, a typical length for the attributes. */
    enum { extra_cookie_length = 110 };
    HcBuf b = {slice_make(a, TYPE_BYTE, 0,
                          c->name.len + c->value.len + c->domain.len + c->path.len +
                              extra_cookie_length),
               a, false};
    b.oom = b.b.p == NULL;
    hc_put(&b, c->name);
    hc_put(&b, BURROW_S("="));
    Str v = burrow__http_sanitize_cookie_value(a, c->value, c->quoted);
    hc_put(&b, v);
    hc_sanitized_free(a, v, c->value);
    if (c->path.len > 0) {
        hc_put(&b, BURROW_S("; Path="));
        Str p = burrow__http_sanitize_cookie_path(a, c->path);
        hc_put(&b, p);
        hc_sanitized_free(a, p, c->path);
    }
    if (c->domain.len > 0) {
        if (hc_valid_domain(c->domain)) {
            /* A domain with bytes that cannot be in one is dropped rather
             * than cleaned, which makes the cookie host only. A leading dot
             * is fine, but it is not sent. */
            Str d = c->domain;
            if (d.p[0] == '.')
                d = str_from_bytes(d.p + 1, d.len - 1);
            hc_put(&b, BURROW_S("; Domain="));
            hc_put(&b, d);
        } else {
            log_printf_v(
                "net/http: invalid Cookie.Domain %q; dropping domain attribute",
                c->domain);
        }
    }
    Byte tmp[32];
    if (hc_valid_expires(c->expires)) {
        hc_put(&b, BURROW_S("; Expires="));
        Slice f = time_append_format(time_utc(c->expires), a,
                                     slice_from(tmp, 0, (Int)sizeof tmp, TYPE_BYTE),
                                     HTTP_TIME_FORMAT);
        hc_put(&b, str_from_bytes(f.p, f.len));
    }
    if (c->max_age > 0) {
        hc_put(&b, BURROW_S("; Max-Age="));
        Slice f = strconv_append_int(a, slice_from(tmp, 0, (Int)sizeof tmp, TYPE_BYTE),
                                     (int64_t)c->max_age, 10);
        hc_put(&b, str_from_bytes(f.p, f.len));
    } else if (c->max_age < 0) {
        hc_put(&b, BURROW_S("; Max-Age=0"));
    }
    if (c->http_only)
        hc_put(&b, BURROW_S("; HttpOnly"));
    if (c->secure)
        hc_put(&b, BURROW_S("; Secure"));
    switch (c->same_site) {
    case HTTP_SAME_SITE_DEFAULT_MODE:
        /* The default comes from leaving the attribute out. */
        break;
    case HTTP_SAME_SITE_NONE_MODE:
        hc_put(&b, BURROW_S("; SameSite=None"));
        break;
    case HTTP_SAME_SITE_LAX_MODE:
        hc_put(&b, BURROW_S("; SameSite=Lax"));
        break;
    case HTTP_SAME_SITE_STRICT_MODE:
        hc_put(&b, BURROW_S("; SameSite=Strict"));
        break;
    default:
        break;
    }
    if (c->partitioned)
        hc_put(&b, BURROW_S("; Partitioned"));
    if (b.oom) {
        hc_slice_free(a, b.b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(b.b.p, b.b.len);
}

Error http_cookie_valid(const HttpCookie *c) {
    if (c == NULL)
        return hc_error(&hc_text_nil);
    if (!burrow__http_is_token(c->name))
        return hc_error(&hc_text_name);
    if (!time_is_zero(c->expires) && !hc_valid_expires(c->expires))
        return hc_error(&hc_text_expires);
    for (Int i = 0; i < c->value.len; i++) {
        if (!hc_valid_value_byte(c->value.p[i]))
            return fmt_errorf_v("http: invalid byte %q in Cookie.Value", c->value.p[i]);
    }
    for (Int i = 0; i < c->path.len; i++) {
        if (!hc_valid_path_byte(c->path.p[i]))
            return fmt_errorf_v("http: invalid byte %q in Cookie.Path", c->path.p[i]);
    }
    if (c->domain.len > 0 && !hc_valid_domain(c->domain))
        return hc_error(&hc_text_domain);
    if (c->partitioned && !c->secure)
        return hc_error(&hc_text_partitioned);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------- Request and Response cookies */

BURROW_SENTINEL_ERROR(http_err_no_cookie, "http: named cookie not present");

Slice http_request_cookies(const HttpRequest *r, Alloc *a) {
    return burrow__http_read_cookies(a, r->header, BURROW_STR_EMPTY);
}

Slice http_request_cookies_named(const HttpRequest *r, Alloc *a, Str name) {
    if (name.len == 0)
        return hc_make(a, 0);
    return burrow__http_read_cookies(a, r->header, name);
}

HttpCookie http_request_cookie(const HttpRequest *r, Alloc *a, Str name, Error *err) {
    HttpCookie c;
    memset(&c, 0, sizeof c);
    if (name.len > 0) {
        Slice cookies = burrow__http_read_cookies(a, r->header, name);
        if (cookies.len > 0) {
            *err = BURROW_NO_ERROR;
            return ((const HttpCookie *)cookies.p)[0];
        }
    }
    *err = http_err_no_cookie;
    return c;
}

/* sanitizeCookieName, which turns each CR and LF into "-". False when a says
 * no. */
static bool hc_sanitize_name(Alloc *a, Str n, Str *out) {
    *out = n;
    if (strings_index_any(n, BURROW_S("\r\n")) < 0)
        return true;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n.len, 1);
    if (p == NULL)
        return false;
    for (Int i = 0; i < n.len; i++)
        p[i] = n.p[i] == '\n' || n.p[i] == '\r' ? (Byte)'-' : n.p[i];
    *out = str_from_bytes(p, n.len);
    return true;
}

bool http_request_add_cookie(HttpRequest *r, Alloc *a, const HttpCookie *c) {
    Str name;
    if (!hc_sanitize_name(a, c->name, &name))
        return false;
    Str value = burrow__http_sanitize_cookie_value(a, c->value, c->quoted);
    Str have = http_header_get(r->header, BURROW_S("Cookie"));
    Str s = have.len > 0 ? fmt_sprintf_v(a, "%s; %s=%s", have, name, value)
                         : fmt_sprintf_v(a, "%s=%s", name, value);
    if (s.len == 0)
        return false;
    return http_header_set(r->header, BURROW_S("Cookie"), s);
}

Slice http_response_cookies(const HttpResponse *r, Alloc *a) {
    return burrow__http_read_set_cookies(a, r->header);
}

bool http_set_cookie(HttpResponseWriter w, const HttpCookie *c) {
    HttpHeader h = http_response_writer_header(w);
    Alloc *a = burrow__map_allocator(h);
    if (c == NULL || !burrow__http_is_token(c->name))
        return true;
    Str v = http_cookie_string(a, c);
    return v.len > 0 && http_header_add(h, BURROW_S("Set-Cookie"), v);
}
