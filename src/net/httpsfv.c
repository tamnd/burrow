/* Derived from Go's src/net/http/internal/httpsfv/httpsfv.go, which Go bundles
 * from golang.org/x/net/internal/httpsfv.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "httpsfv.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool sf_is_lc_alpha(Byte b) {
    return b >= 'a' && b <= 'z';
}

static bool sf_is_alpha(Byte b) {
    return sf_is_lc_alpha(b) || (b >= 'A' && b <= 'Z');
}

static bool sf_is_digit(Byte b) {
    return b >= '0' && b <= '9';
}

static bool sf_is_vchar(Byte b) {
    return b >= 0x21 && b <= 0x7E;
}

static bool sf_is_sp(Byte b) {
    return b == 0x20;
}

static bool sf_is_tchar(Byte b) {
    if (sf_is_alpha(b) || sf_is_digit(b))
        return true;
    switch (b) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

/* s[i:]. An empty s may have a NULL p, which takes no offset, not even 0. */
static Str sf_from(Str s, Int i) {
    return i == 0 ? s : str_from_bytes(s.p + i, s.len - i);
}

static Str sf_skip_whitespace(Str s) {
    Int i = 0;
    while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t'))
        i++;
    return sf_from(s, i);
}

/* The (consumed, rest, ok) a consume function gives back. */
static bool sf_fail(Str s, Str *consumed, Str *rest) {
    *consumed = BURROW_STR_EMPTY;
    *rest = s;
    return false;
}

static bool sf_done(Str s, Int n, Str *consumed, Str *rest) {
    *consumed = str_from_bytes(s.p, n);
    *rest = sf_from(s, n);
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc4648#section-8, lower case only. */
static bool sf_dec_base16(Byte in, Byte *out) {
    if (sf_is_digit(in)) {
        *out = (Byte)(in - '0');
        return true;
    }
    if (in >= 'a' && in <= 'f') {
        *out = (Byte)(in - 'a' + 10);
        return true;
    }
    return false;
}

static bool sf_dec_octet_hex(Byte ch1, Byte ch2, Byte *ch) {
    Byte hi = 0;
    Byte lo = 0;
    if (!sf_dec_base16(ch1, &hi) || !sf_dec_base16(ch2, &lo))
        return false;
    *ch = (Byte)(hi << 4 | lo);
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-list. */
bool burrow__httpsfv_parse_list(Str s, HttpsfvItemFunc f) {
    while (s.len != 0) {
        Str member = BURROW_STR_EMPTY;
        Str param = BURROW_STR_EMPTY;
        bool ok = false;
        if (s.p[0] == '(')
            ok = burrow__httpsfv_consume_bare_inner_list(s, (HttpsfvItemFunc){0},
                                                         &member, &s);
        else
            ok = burrow__httpsfv_consume_bare_item(s, &member, &s);
        if (!ok)
            return false;
        if (!burrow__httpsfv_consume_parameter(s, (HttpsfvParamFunc){0}, &param, &s))
            return false;
        if (!BURROW_FUNC_IS_NIL(f))
            BURROW_CALLF(f, member, param);

        s = sf_skip_whitespace(s);
        if (s.len == 0)
            break;
        if (s.p[0] != ',')
            return false;
        s = sf_skip_whitespace(sf_from(s, 1));
        if (s.len == 0)
            return false;
    }
    return true;
}

/* An inner list, https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-inner-list,
 * less the parameters after it. Given (a;b c;d);e, it takes (a;b c;d). */
bool burrow__httpsfv_consume_bare_inner_list(Str s, HttpsfvItemFunc f, Str *consumed,
                                             Str *rest) {
    if (s.len == 0 || s.p[0] != '(')
        return sf_fail(s, consumed, rest);
    Str r = sf_from(s, 1);
    while (r.len != 0) {
        Str item = BURROW_STR_EMPTY;
        Str param = BURROW_STR_EMPTY;
        r = sf_skip_whitespace(r);
        if (r.len != 0 && r.p[0] == ')') {
            r = sf_from(r, 1);
            break;
        }
        if (!burrow__httpsfv_consume_bare_item(r, &item, &r))
            return sf_fail(s, consumed, rest);
        if (!burrow__httpsfv_consume_parameter(r, (HttpsfvParamFunc){0}, &param, &r))
            return sf_fail(s, consumed, rest);
        if (r.len == 0 || (r.p[0] != ')' && !sf_is_sp(r.p[0])))
            return sf_fail(s, consumed, rest);
        if (!BURROW_FUNC_IS_NIL(f))
            BURROW_CALLF(f, item, param);
    }
    return sf_done(s, s.len - r.len, consumed, rest);
}

bool burrow__httpsfv_parse_bare_inner_list(Str s, HttpsfvItemFunc f) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    bool ok = burrow__httpsfv_consume_bare_inner_list(s, f, &consumed, &rest);
    return rest.len == 0 && ok;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-item. */
bool burrow__httpsfv_consume_item(Str s, HttpsfvItemFunc f, Str *consumed, Str *rest) {
    Str item = BURROW_STR_EMPTY;
    Str param = BURROW_STR_EMPTY;
    Str r = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_bare_item(s, &item, &r))
        return sf_fail(s, consumed, rest);
    if (!burrow__httpsfv_consume_parameter(r, (HttpsfvParamFunc){0}, &param, &r))
        return sf_fail(s, consumed, rest);
    if (!BURROW_FUNC_IS_NIL(f))
        BURROW_CALLF(f, item, param);
    return sf_done(s, s.len - r.len, consumed, rest);
}

bool burrow__httpsfv_parse_item(Str s, HttpsfvItemFunc f) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    bool ok = burrow__httpsfv_consume_item(s, f, &consumed, &rest);
    return rest.len == 0 && ok;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-dictionary. */
bool burrow__httpsfv_parse_dictionary(Str s, HttpsfvDictFunc f) {
    while (s.len != 0) {
        Str key = BURROW_STR_EMPTY;
        Str val = BURROW_S("?1"); /* Default value for empty val is boolean true. */
        Str param = BURROW_STR_EMPTY;
        if (!burrow__httpsfv_consume_key(s, &key, &s))
            return false;
        if (s.len != 0 && s.p[0] == '=') {
            bool ok = false;
            s = sf_from(s, 1);
            if (s.len != 0 && s.p[0] == '(')
                ok = burrow__httpsfv_consume_bare_inner_list(s, (HttpsfvItemFunc){0},
                                                             &val, &s);
            else
                ok = burrow__httpsfv_consume_bare_item(s, &val, &s);
            if (!ok)
                return false;
        }
        if (!burrow__httpsfv_consume_parameter(s, (HttpsfvParamFunc){0}, &param, &s))
            return false;
        if (!BURROW_FUNC_IS_NIL(f))
            BURROW_CALLF(f, key, val, param);
        s = sf_skip_whitespace(s);
        if (s.len == 0)
            break;
        if (s.p[0] == ',')
            s = sf_from(s, 1);
        s = sf_skip_whitespace(s);
        if (s.len == 0)
            return false;
    }
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#parse-param. */
bool burrow__httpsfv_consume_parameter(Str s, HttpsfvParamFunc f, Str *consumed,
                                       Str *rest) {
    Str r = s;
    while (r.len != 0) {
        Str key = BURROW_STR_EMPTY;
        Str val = BURROW_S("?1"); /* Default value for empty val is boolean true. */
        if (r.p[0] != ';')
            break;
        r = sf_skip_whitespace(sf_from(r, 1));
        if (!burrow__httpsfv_consume_key(r, &key, &r))
            return sf_fail(s, consumed, rest);
        if (r.len != 0 && r.p[0] == '=') {
            r = sf_from(r, 1);
            if (!burrow__httpsfv_consume_bare_item(r, &val, &r))
                return sf_fail(s, consumed, rest);
        }
        if (!BURROW_FUNC_IS_NIL(f))
            BURROW_CALLF(f, key, val);
    }
    return sf_done(s, s.len - r.len, consumed, rest);
}

bool burrow__httpsfv_parse_parameter(Str s, HttpsfvParamFunc f) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    bool ok = burrow__httpsfv_consume_parameter(s, f, &consumed, &rest);
    return rest.len == 0 && ok;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-key. */
bool burrow__httpsfv_consume_key(Str s, Str *consumed, Str *rest) {
    if (s.len == 0 || (!sf_is_lc_alpha(s.p[0]) && s.p[0] != '*'))
        return sf_fail(s, consumed, rest);
    Int i = 0;
    for (; i < s.len; i++) {
        Byte ch = s.p[i];
        if (!sf_is_lc_alpha(ch) && !sf_is_digit(ch) && ch != '_' && ch != '-' &&
            ch != '.' && ch != '*')
            break;
    }
    return sf_done(s, i, consumed, rest);
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-integer-or-decim. */
bool burrow__httpsfv_consume_integer_or_decimal(Str s, Str *consumed, Str *rest) {
    Int i = 0;
    Int sign_offset = 0;
    Int period_index = 0;
    bool is_decimal = false;
    if (i < s.len && s.p[i] == '-') {
        i++;
        sign_offset++;
    }
    if (i >= s.len)
        return sf_fail(s, consumed, rest);
    if (!sf_is_digit(s.p[i]))
        return sf_fail(s, consumed, rest);
    while (i < s.len) {
        Byte ch = s.p[i];
        if (sf_is_digit(ch)) {
            i++;
            continue;
        }
        if (!is_decimal && ch == '.') {
            if (i - sign_offset > 12)
                return sf_fail(s, consumed, rest);
            period_index = i;
            is_decimal = true;
            i++;
            continue;
        }
        break;
    }
    if (!is_decimal && i - sign_offset > 15)
        return sf_fail(s, consumed, rest);
    if (is_decimal) {
        if (i - sign_offset > 16)
            return sf_fail(s, consumed, rest);
        if (s.p[i - 1] == '.')
            return sf_fail(s, consumed, rest);
        if (i - period_index - 1 > 3)
            return sf_fail(s, consumed, rest);
    }
    return sf_done(s, i, consumed, rest);
}

bool burrow__httpsfv_parse_integer(Str s, int64_t *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = 0;
    if (!burrow__httpsfv_consume_integer_or_decimal(s, &consumed, &rest) ||
        rest.len != 0)
        return false;
    Error err = BURROW_NO_ERROR;
    int64_t n = strconv_parse_int(s, 10, 64, &err);
    if (BURROW_FAILED(err))
        return false;
    *out = n;
    return true;
}

bool burrow__httpsfv_parse_decimal(Str s, double *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = 0;
    if (!burrow__httpsfv_consume_integer_or_decimal(s, &consumed, &rest) ||
        rest.len != 0)
        return false;
    if (strings_index_byte(s, '.') < 0)
        return false;
    Error err = BURROW_NO_ERROR;
    double n = strconv_parse_float(s, 64, &err);
    if (BURROW_FAILED(err))
        return false;
    *out = n;
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-string. */
bool burrow__httpsfv_consume_string(Str s, Str *consumed, Str *rest) {
    if (s.len == 0 || s.p[0] != '"')
        return sf_fail(s, consumed, rest);
    for (Int i = 1; i < s.len; i++) {
        Byte ch = s.p[i];
        switch (ch) {
        case '\\':
            if (i + 1 >= s.len)
                return sf_fail(s, consumed, rest);
            i++;
            ch = s.p[i];
            if (ch != '"' && ch != '\\')
                return sf_fail(s, consumed, rest);
            break;
        case '"':
            return sf_done(s, i + 1, consumed, rest);
        default:
            if (!sf_is_vchar(ch) && !sf_is_sp(ch))
                return sf_fail(s, consumed, rest);
            break;
        }
    }
    return sf_fail(s, consumed, rest);
}

bool burrow__httpsfv_parse_string(Str s, Str *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_string(s, &consumed, &rest) || rest.len != 0)
        return false;
    *out = str_from_bytes(s.p + 1, s.len - 2);
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-token */
bool burrow__httpsfv_consume_token(Str s, Str *consumed, Str *rest) {
    if (s.len == 0 || (!sf_is_alpha(s.p[0]) && s.p[0] != '*'))
        return sf_fail(s, consumed, rest);
    Int i = 0;
    for (; i < s.len; i++) {
        Byte ch = s.p[i];
        if (!sf_is_tchar(ch) && ch != ':' && ch != '/')
            break;
    }
    return sf_done(s, i, consumed, rest);
}

bool burrow__httpsfv_parse_token(Str s, Str *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_token(s, &consumed, &rest) || rest.len != 0)
        return false;
    *out = s;
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-byte-sequence. */
bool burrow__httpsfv_consume_byte_sequence(Str s, Str *consumed, Str *rest) {
    if (s.len == 0 || s.p[0] != ':')
        return sf_fail(s, consumed, rest);
    for (Int i = 1; i < s.len; i++) {
        Byte ch = s.p[i];
        if (ch == ':')
            return sf_done(s, i + 1, consumed, rest);
        if (!sf_is_alpha(ch) && !sf_is_digit(ch) && ch != '+' && ch != '/' && ch != '=')
            return sf_fail(s, consumed, rest);
    }
    return sf_fail(s, consumed, rest);
}

bool burrow__httpsfv_parse_byte_sequence(Str s, Str *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_byte_sequence(s, &consumed, &rest) || rest.len != 0)
        return false;
    *out = str_from_bytes(s.p + 1, s.len - 2);
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-boolean. */
bool burrow__httpsfv_consume_boolean(Str s, Str *consumed, Str *rest) {
    if (s.len >= 2 && s.p[0] == '?' && (s.p[1] == '0' || s.p[1] == '1'))
        return sf_done(s, 2, consumed, rest);
    return sf_fail(s, consumed, rest);
}

bool burrow__httpsfv_parse_boolean(Str s, bool *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = false;
    if (!burrow__httpsfv_consume_boolean(s, &consumed, &rest) || rest.len != 0)
        return false;
    *out = str_eq(s, BURROW_S("?1"));
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-date. */
bool burrow__httpsfv_consume_date(Str s, Str *consumed, Str *rest) {
    if (s.len == 0 || s.p[0] != '@')
        return sf_fail(s, consumed, rest);
    Str num = BURROW_STR_EMPTY;
    Str r = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_integer_or_decimal(sf_from(s, 1), &num, &r))
        return sf_fail(s, consumed, rest);
    if (strings_index_byte(num, '.') >= 0)
        return sf_fail(s, consumed, rest);
    return sf_done(s, s.len - r.len, consumed, rest);
}

bool burrow__httpsfv_parse_date(Str s, Time *out) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    *out = (Time){0};
    if (!burrow__httpsfv_consume_date(s, &consumed, &rest) || rest.len != 0)
        return false;
    int64_t n = 0;
    if (!burrow__httpsfv_parse_integer(sf_from(s, 1), &n))
        return false;
    *out = time_from_unix(n, 0);
    return true;
}

/* To prevent excessive allocation, especially when input is large, we keep a
 * buffer of 4 bytes with the last rune we've seen. This way, we can validate
 * that the display string conforms to UTF-8 without building the whole
 * string. */
typedef struct SfRune {
    Int len;
    Byte b[4];
} SfRune;

static bool sf_is_part_of_valid_rune(SfRune *r, Byte ch) {
    r->b[r->len] = ch;
    r->len++;
    Str have = str_from_bytes(r->b, r->len);
    if (utf8_full_rune_in_string(have)) {
        Int size = 0;
        if (utf8_decode_rune_in_string(have, &size) == UTF8_RUNE_ERROR)
            return false;
        memmove(r->b, r->b + size, (size_t)(r->len - size));
        r->len -= size;
        return true;
    }
    return r->len <= 4;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-display-string. */
bool burrow__httpsfv_consume_display_string(Str s, Str *consumed, Str *rest) {
    SfRune last = {0};
    if (s.len <= 1 || s.p[0] != '%' || s.p[1] != '"')
        return sf_fail(s, consumed, rest);
    Int i = 2;
    while (i < s.len) {
        Byte ch = s.p[i];
        if (!sf_is_vchar(ch) && !sf_is_sp(ch))
            return sf_fail(s, consumed, rest);
        switch (ch) {
        case '"':
            if (last.len > 0)
                return sf_fail(s, consumed, rest);
            return sf_done(s, i + 1, consumed, rest);
        case '%':
            if (i + 2 >= s.len)
                return sf_fail(s, consumed, rest);
            if (!sf_dec_octet_hex(s.p[i + 1], s.p[i + 2], &ch))
                return sf_fail(s, consumed, rest);
            if (!sf_is_part_of_valid_rune(&last, ch))
                return sf_fail(s, consumed, rest);
            i += 3;
            break;
        default:
            if (!sf_is_part_of_valid_rune(&last, ch))
                return sf_fail(s, consumed, rest);
            i++;
            break;
        }
    }
    return sf_fail(s, consumed, rest);
}

bool burrow__httpsfv_parse_display_string(Alloc *a, Str s, Str *out, Error *err) {
    Str consumed = BURROW_STR_EMPTY;
    Str rest = BURROW_STR_EMPTY;
    BURROW_OUT(err, BURROW_NO_ERROR);
    *out = BURROW_STR_EMPTY;
    if (!burrow__httpsfv_consume_display_string(s, &consumed, &rest) || rest.len != 0)
        return false;
    /* consume_display_string already checked that this is a valid display
     * string, so it can be decoded without checking again. Decoding only ever
     * makes it shorter. */
    s = str_from_bytes(s.p + 2, s.len - 3);
    if (s.len == 0)
        return true;
    Byte *b = mem_alloc(a, (size_t)s.len, 1);
    if (b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return false;
    }
    Int n = 0;
    for (Int i = 0; i < s.len;) {
        if (s.p[i] == '%') {
            Byte decoded = 0;
            (void)sf_dec_octet_hex(s.p[i + 1], s.p[i + 2], &decoded);
            b[n++] = decoded;
            i += 3;
            continue;
        }
        b[n++] = s.p[i];
        i++;
    }
    *out = str_from_bytes(b, n);
    return true;
}

/* https://www.rfc-editor.org/rfc/rfc9651.html#parse-bare-item. */
bool burrow__httpsfv_consume_bare_item(Str s, Str *consumed, Str *rest) {
    if (s.len == 0)
        return sf_fail(s, consumed, rest);
    Byte ch = s.p[0];
    if (ch == '-' || sf_is_digit(ch))
        return burrow__httpsfv_consume_integer_or_decimal(s, consumed, rest);
    if (ch == '"')
        return burrow__httpsfv_consume_string(s, consumed, rest);
    if (ch == '*' || sf_is_alpha(ch))
        return burrow__httpsfv_consume_token(s, consumed, rest);
    if (ch == ':')
        return burrow__httpsfv_consume_byte_sequence(s, consumed, rest);
    if (ch == '?')
        return burrow__httpsfv_consume_boolean(s, consumed, rest);
    if (ch == '@')
        return burrow__httpsfv_consume_date(s, consumed, rest);
    if (ch == '%')
        return burrow__httpsfv_consume_display_string(s, consumed, rest);
    return sf_fail(s, consumed, rest);
}
