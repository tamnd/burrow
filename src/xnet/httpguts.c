/* Derived from Go's src/vendor/golang.org/x/net/http/httpguts/httplex.go.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * guts.go is here too, in ValidTrailerHeader.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "httpguts.h"

#include "idna.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stdint.h>

static const bool hg_token[256] = {
    ['!'] = true, ['#'] = true, ['$'] = true, ['%'] = true, ['&'] = true, ['\''] = true,
    ['*'] = true, ['+'] = true, ['-'] = true, ['.'] = true, ['0'] = true, ['1'] = true,
    ['2'] = true, ['3'] = true, ['4'] = true, ['5'] = true, ['6'] = true, ['7'] = true,
    ['8'] = true, ['9'] = true, ['A'] = true, ['B'] = true, ['C'] = true, ['D'] = true,
    ['E'] = true, ['F'] = true, ['G'] = true, ['H'] = true, ['I'] = true, ['J'] = true,
    ['K'] = true, ['L'] = true, ['M'] = true, ['N'] = true, ['O'] = true, ['P'] = true,
    ['Q'] = true, ['R'] = true, ['S'] = true, ['T'] = true, ['U'] = true, ['W'] = true,
    ['V'] = true, ['X'] = true, ['Y'] = true, ['Z'] = true, ['^'] = true, ['_'] = true,
    ['`'] = true, ['a'] = true, ['b'] = true, ['c'] = true, ['d'] = true, ['e'] = true,
    ['f'] = true, ['g'] = true, ['h'] = true, ['i'] = true, ['j'] = true, ['k'] = true,
    ['l'] = true, ['m'] = true, ['n'] = true, ['o'] = true, ['p'] = true, ['q'] = true,
    ['r'] = true, ['s'] = true, ['t'] = true, ['u'] = true, ['v'] = true, ['w'] = true,
    ['x'] = true, ['y'] = true, ['z'] = true, ['|'] = true, ['~'] = true,
};

/* ----------------------------------------------------------------- guts.go */

static const Str hg_bad_trailer[] = {
    BURROW_S_INIT("Authorization"),
    BURROW_S_INIT("Cache-Control"),
    BURROW_S_INIT("Connection"),
    BURROW_S_INIT("Content-Encoding"),
    BURROW_S_INIT("Content-Length"),
    BURROW_S_INIT("Content-Range"),
    BURROW_S_INIT("Content-Type"),
    BURROW_S_INIT("Expect"),
    BURROW_S_INIT("Host"),
    BURROW_S_INIT("Keep-Alive"),
    BURROW_S_INIT("Max-Forwards"),
    BURROW_S_INIT("Pragma"),
    BURROW_S_INIT("Proxy-Authenticate"),
    BURROW_S_INIT("Proxy-Authorization"),
    BURROW_S_INIT("Proxy-Connection"),
    BURROW_S_INIT("Range"),
    BURROW_S_INIT("Realm"),
    BURROW_S_INIT("Te"),
    BURROW_S_INIT("Trailer"),
    BURROW_S_INIT("Transfer-Encoding"),
    BURROW_S_INIT("Www-Authenticate"),
};

/* textproto.CanonicalMIMEHeaderKey leaves a name alone when it has a byte
 * that isn't a token byte, and otherwise only changes the case of letters, so
 * two names of tokens have the same canonical form exactly when they are
 * equal ignoring ASCII case. Every name in the table is canonical already. */
bool burrow__httpguts_valid_trailer_header(Str name) {
    bool canon = true;
    for (Int i = 0; i < name.len; i++)
        if (!hg_token[name.p[i]])
            canon = false;
    bool (*eq)(Str, Str) = canon ? strings_equal_fold : str_eq;
    if (name.len >= 3 && eq(str_from_bytes(name.p, 3), BURROW_S("If-")))
        return false;
    for (size_t i = 0; i < sizeof hg_bad_trailer / sizeof hg_bad_trailer[0]; i++)
        if (eq(name, hg_bad_trailer[i]))
            return false;
    return true;
}

/* ------------------------------------------------------------- httplex.go */

bool burrow__httpguts_is_token_rune(int32_t r) {
    return r >= 0 && r < 0x80 && hg_token[r];
}

/* isOWS */
static bool hg_is_ows(Byte b) {
    return b == ' ' || b == '\t';
}

static Str hg_trim_ows(Str x) {
    while (x.len > 0 && hg_is_ows(x.p[0]))
        x = str_from_bytes(x.p + 1, x.len - 1);
    while (x.len > 0 && hg_is_ows(x.p[x.len - 1]))
        x = str_from_bytes(x.p, x.len - 1);
    return x;
}

static Byte hg_lower_ascii(Byte b) {
    if ('A' <= b && b <= 'Z')
        return (Byte)(b + ('a' - 'A'));
    return b;
}

/* tokenEqual. Go ranges over t1 by rune, so a multi-byte t1 is never equal:
 * its first byte at or above 0x80 says no before the lengths could line up. */
static bool hg_token_equal(Str t1, Str t2) {
    if (t1.len != t2.len)
        return false;
    for (Int i = 0; i < t1.len; i++) {
        if (t1.p[i] >= 0x80)
            return false;
        if (hg_lower_ascii(t1.p[i]) != hg_lower_ascii(t2.p[i]))
            return false;
    }
    return true;
}

static bool hg_header_value_contains_token(Str v, Str token) {
    for (Int comma = strings_index_byte(v, ','); comma != -1;
         comma = strings_index_byte(v, ',')) {
        if (hg_token_equal(hg_trim_ows(str_from_bytes(v.p, comma)), token))
            return true;
        v = str_from_bytes(v.p + comma + 1, v.len - comma - 1);
    }
    return hg_token_equal(hg_trim_ows(v), token);
}

bool burrow__httpguts_header_values_contains_token(const Str *values, Int n,
                                                   Str token) {
    for (Int i = 0; i < n; i++)
        if (hg_header_value_contains_token(values[i], token))
            return true;
    return false;
}

/* isLWS */
static bool hg_is_lws(Byte b) {
    return b == ' ' || b == '\t';
}

/* isCTL */
static bool hg_is_ctl(Byte b) {
    const Byte del = 0x7F; /* a CTL */
    return b < ' ' || b == del;
}

bool burrow__httpguts_valid_header_field_name(Str v) {
    if (v.len == 0)
        return false;
    for (Int i = 0; i < v.len; i++)
        if (!hg_token[v.p[i]])
            return false;
    return true;
}

/* See the ValidHostHeader comment. */
static const bool hg_valid_host_byte[256] = {
    ['0'] = true, ['1'] = true,  ['2'] = true, ['3'] = true, ['4'] = true, ['5'] = true,
    ['6'] = true, ['7'] = true,  ['8'] = true, ['9'] = true,

    ['a'] = true, ['b'] = true,  ['c'] = true, ['d'] = true, ['e'] = true, ['f'] = true,
    ['g'] = true, ['h'] = true,  ['i'] = true, ['j'] = true, ['k'] = true, ['l'] = true,
    ['m'] = true, ['n'] = true,  ['o'] = true, ['p'] = true, ['q'] = true, ['r'] = true,
    ['s'] = true, ['t'] = true,  ['u'] = true, ['v'] = true, ['w'] = true, ['x'] = true,
    ['y'] = true, ['z'] = true,

    ['A'] = true, ['B'] = true,  ['C'] = true, ['D'] = true, ['E'] = true, ['F'] = true,
    ['G'] = true, ['H'] = true,  ['I'] = true, ['J'] = true, ['K'] = true, ['L'] = true,
    ['M'] = true, ['N'] = true,  ['O'] = true, ['P'] = true, ['Q'] = true, ['R'] = true,
    ['S'] = true, ['T'] = true,  ['U'] = true, ['V'] = true, ['W'] = true, ['X'] = true,
    ['Y'] = true, ['Z'] = true,

    ['!'] = true,                /* sub-delims */
    ['$'] = true,                /* sub-delims */
    ['%'] = true,                /* pct-encoded (and used in IPv6 zones) */
    ['&'] = true,                /* sub-delims */
    ['('] = true,                /* sub-delims */
    [')'] = true,                /* sub-delims */
    ['*'] = true,                /* sub-delims */
    ['+'] = true,                /* sub-delims */
    [','] = true,                /* sub-delims */
    ['-'] = true,                /* unreserved */
    ['.'] = true,                /* unreserved */
    [':'] = true,                /* IPv6address + Host expression's optional port */
    [';'] = true,                /* sub-delims */
    ['='] = true,                /* sub-delims */
    ['['] = true, ['\''] = true, /* sub-delims */
    [']'] = true, ['_'] = true,  /* unreserved */
    ['~'] = true,                /* unreserved */
};

bool burrow__httpguts_valid_host_header(Str h) {
    for (Int i = 0; i < h.len; i++)
        if (!hg_valid_host_byte[h.p[i]])
            return false;
    return true;
}

bool burrow__httpguts_valid_header_field_value(Str v) {
    for (Int i = 0; i < v.len; i++) {
        Byte b = v.p[i];
        if (hg_is_ctl(b) && !hg_is_lws(b))
            return false;
    }
    return true;
}

static bool hg_is_ascii(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 0x80)
            return false;
    return true;
}

Str burrow__httpguts_punycode_host_port(Alloc *a, Str v, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (hg_is_ascii(v))
        return v;

    Str port = BURROW_STR_EMPTY;
    Error serr = BURROW_NO_ERROR;
    Str host = net_split_host_port(v, &port, &serr);
    if (BURROW_FAILED(serr)) {
        /* The input 'v' argument was just a "host" argument, without a port.
         * This error should not be returned to the caller. */
        host = v;
        port = BURROW_STR_EMPTY;
    }
    Error ierr = BURROW_NO_ERROR;
    host = burrow__idna_to_ascii(a, host, &ierr);
    if (BURROW_FAILED(ierr)) {
        /* Non-UTF-8? Not representable in Punycode, in any case. */
        BURROW_OUT(err, ierr);
        return BURROW_STR_EMPTY;
    }
    if (port.len == 0)
        return host;
    Str out = net_join_host_port(a, host, port);
    if (out.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}
