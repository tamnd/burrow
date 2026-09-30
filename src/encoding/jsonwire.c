/* The wire level half of encoding/json: consuming, quoting and unquoting
 * strings, numbers and literals, with no state kept between calls. Go keeps
 * this in an internal package so that jsontext and json v1 can share it, and
 * here it is a set of functions json_internal.h declares for the same files.
 *
 * Derived from Go's src/encoding/json/internal/jsonwire/decode.go, encode.go
 * and wire.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "json_internal.h"

#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/unicode.h"
#include "burrow/unicode/utf16.h"
#include "burrow/utf8.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------- buffer */

bool burrow__jsonbuf_grow(JsonBuf *b, Int n) {
    if (b->failed)
        return false;
    Int want = b->len + n;
    Int ncap = b->cap * 2;
    if (ncap < want)
        ncap = want;
    if (ncap < 64)
        ncap = 64;
    Byte *p;
    if (b->owned && b->p != NULL) {
        p = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)ncap, 1);
        if (p == NULL) {
            b->failed = true;
            return false;
        }
    } else {
        p = (Byte *)mem_alloc_nozero(b->a, (size_t)ncap, 1);
        if (p == NULL) {
            b->failed = true;
            return false;
        }
        if (b->len > 0 && b->p != NULL)
            memcpy(p, b->p, (size_t)b->len);
    }
    b->p = p;
    b->cap = ncap;
    b->owned = true;
    return true;
}

void burrow__jsonbuf_free(JsonBuf *b) {
    if (b->owned && b->p != NULL)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->owned = false;
    b->failed = false;
}

/* ------------------------------------------------------------------ helpers */

/* An empty Str or Slice may carry a NULL pointer, and C leaves even NULL + 0
 * undefined, so the functions that index into their input start from this. */
static const Byte *jw_nonnull(const Byte *b) {
    return b != NULL ? b : (const Byte *)"";
}

static Rune jw_decode(const Byte *b, Int n, Int *size) {
    return utf8_decode_rune_in_string(str_from_bytes(b, n), size);
}

static bool jw_full_rune(const Byte *b, Int n) {
    return utf8_full_rune_in_string(str_from_bytes(b, n));
}

static bool jw_is_invalid_utf8(Rune r, Int rn) {
    return r == UTF8_RUNE_ERROR && rn == 1;
}

static void jw_put_rune(JsonBuf *dst, Rune r) {
    if (!jsonbuf_reserve(dst, 4))
        return;
    Slice room = {dst->p + dst->len, 4, 4, TYPE_BYTE};
    dst->len += utf8_encode_rune(room, r);
}

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(burrow__jsonwire_err_invalid_utf8, "invalid UTF-8");

/* What an invalid text error falls back to when the error arena is full. */
BURROW_SENTINEL_ERROR(jw_err_invalid_text, "invalid text");

static Str jw_invalid_text_message(const void *self) {
    return ((const JsonwireInvalidTextError *)self)->message;
}

static const Type jw_invalid_text_desc = {
    {(const Byte *)"InvalidTextError", 16},
    {(const Byte *)"jsonwire", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsonwireInvalidTextError),
    (uint16_t)_Alignof(JsonwireInvalidTextError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a776974U, /* "jwit" */
    NULL,
};

static Error jw_invalid_text_clone(const void *self, Alloc *a);

const ErrorVT burrow__jsonwire_invalid_text_error_vt = {
    &jw_invalid_text_desc, jw_invalid_text_message, NULL, NULL, NULL, NULL,
    jw_invalid_text_clone,
};

/* Whether InvalidTextError.Error would quote what with strconv.Quote rather
 * than backquotes. */
static bool jw_what_needs_escape(Str s) {
    Int i = 0;
    while (i < s.len) {
        Int rn;
        Rune r = jw_decode(s.p + i, s.len - i, &rn);
        if (r == '`' || r == UTF8_RUNE_ERROR || unicode_is_space(r) ||
            !unicode_is_print(r))
            return true;
        i += rn;
    }
    return false;
}

/* The error itself, what and where copied in after it, then the message. */
static Error jw_invalid_text_build(Alloc *a, Str label, Str what, Str where) {
    Byte qr[16];
    Str quoted = BURROW_STR_EMPTY;
    Int qlen;
    int form;
    if (utf8_rune_count_in_string(what) == 1) {
        form = 0;
        quoted = burrow__jsonwire_quote_rune(what.p, what.len, qr);
        qlen = quoted.len;
    } else if (jw_what_needs_escape(what)) {
        form = 1;
        qlen = burrow__strconv_quote_into(NULL, what);
    } else {
        form = 2;
        qlen = what.len + 2;
    }
    Int mlen = 8 + label.len + 1 + qlen + 1 + where.len;
    if (where.len == 0)
        mlen--;
    size_t size = sizeof(JsonwireInvalidTextError) + (size_t)label.len +
                  (size_t)what.len + (size_t)where.len + (size_t)mlen;
    JsonwireInvalidTextError *e = (JsonwireInvalidTextError *)mem_alloc_nozero(
        a, size, _Alignof(JsonwireInvalidTextError));
    if (e == NULL)
        return BURROW_NO_ERROR;
    Byte *p = (Byte *)(e + 1);
    if (label.len > 0)
        memcpy(p, label.p, (size_t)label.len);
    e->label = str_from_bytes(p, label.len);
    p += label.len;
    if (what.len > 0)
        memcpy(p, what.p, (size_t)what.len);
    e->what = str_from_bytes(p, what.len);
    p += what.len;
    if (where.len > 0)
        memcpy(p, where.p, (size_t)where.len);
    e->where = str_from_bytes(p, where.len);
    p += where.len;
    Byte *m = p;
    memcpy(p, "invalid ", 8);
    p += 8;
    if (label.len > 0)
        memcpy(p, label.p, (size_t)label.len);
    p += label.len;
    *p++ = ' ';
    if (form == 0) {
        memcpy(p, quoted.p, (size_t)quoted.len);
    } else if (form == 1) {
        burrow__strconv_quote_into(p, what);
    } else {
        p[0] = '`';
        if (what.len > 0)
            memcpy(p + 1, what.p, (size_t)what.len);
        p[what.len + 1] = '`';
    }
    p += qlen;
    if (where.len > 0) {
        *p++ = ' ';
        memcpy(p, where.p, (size_t)where.len);
    }
    e->message = str_from_bytes(m, mlen);
    return (Error){&burrow__jsonwire_invalid_text_error_vt, e};
}

static Error jw_invalid_text_clone(const void *self, Alloc *a) {
    const JsonwireInvalidTextError *e = (const JsonwireInvalidTextError *)self;
    Error made = jw_invalid_text_build(a, e->label, e->what, e->where);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

static Error jw_invalid_text_new(Str label, Str what, Str where) {
    Error made = jw_invalid_text_build(error_allocator(), label, what, where);
    return BURROW_FAILED(made) ? made : jw_err_invalid_text;
}

Error burrow__jsonwire_new_invalid_character_error(const Byte *prefix, Int n,
                                                   Str where) {
    Int rn = 0;
    jw_decode(prefix, n, &rn);
    return jw_invalid_text_new(BURROW_S("character"), str_from_bytes(prefix, rn),
                               where);
}

Error burrow__jsonwire_new_invalid_escape_sequence_error(const Byte *what, Int n) {
    if (n > 6)
        return jw_invalid_text_new(BURROW_S("surrogate pair"), str_from_bytes(what, n),
                                   BURROW_S("in string"));
    return jw_invalid_text_new(BURROW_S("escape sequence"), str_from_bytes(what, n),
                               BURROW_S("in string"));
}

/* ------------------------------------------------------------------- decode */

Int burrow__jsonwire_consume_literal(const Byte *b, Int n, Str lit, Error *err) {
    *err = BURROW_NO_ERROR;
    for (Int i = 0; i < n && i < lit.len; i++) {
        if (b[i] != lit.p[i]) {
            /* "in literal " + lit + " (expecting " + QuoteRune(lit[i]) + ")" */
            Byte where[48];
            Int w = 0;
            memcpy(where, "in literal ", 11);
            w = 11;
            memcpy(where + w, lit.p, (size_t)lit.len);
            w += lit.len;
            memcpy(where + w, " (expecting '", 13);
            w += 13;
            where[w++] = lit.p[i];
            where[w++] = '\'';
            where[w++] = ')';
            *err = burrow__jsonwire_new_invalid_character_error(
                b + i, n - i, str_from_bytes(where, w));
            return i;
        }
    }
    if (n < lit.len) {
        *err = io_err_unexpected_eof;
        return n;
    }
    return lit.len;
}

bool burrow__jsonwire_parse_hex_uint16(const Byte *b, Int n, uint16_t *v) {
    if (n != 4)
        return false;
    uint16_t x = 0;
    for (int i = 0; i < 4; i++) {
        Byte c = b[i];
        if (c >= '0' && c <= '9')
            c = (Byte)(c - '0');
        else if (c >= 'a' && c <= 'f')
            c = (Byte)(10 + c - 'a');
        else if (c >= 'A' && c <= 'F')
            c = (Byte)(10 + c - 'A');
        else
            return false;
        x = (uint16_t)(x * 16 + c);
    }
    *v = x;
    return true;
}

/* hasEscapedUTF16Prefix. */
static bool jw_has_escaped_utf16_prefix(const Byte *b, Int n, bool lower_half) {
    for (Int i = 0; i < n; i++) {
        Byte c = b[i];
        bool hex =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (i == 0 && c != '\\')
            return false;
        if (i == 1 && c != 'u')
            return false;
        if (i == 2 && lower_half && c != 'd' && c != 'D')
            return false;
        if (i == 3 && lower_half && !(c >= 'c' && c <= 'f') && !(c >= 'C' && c <= 'F'))
            return false;
        if (i >= 2 && i < 6 && !hex)
            return false;
    }
    return true;
}

static bool jw_no_escape(Byte c) {
    return c < 0x80 && c >= ' ' && c != '\\' && c != '"';
}

Int burrow__jsonwire_consume_string_resumable(unsigned *flags, const Byte *b, Int blen,
                                              Int resume, bool validate_utf8,
                                              Error *err) {
    *err = BURROW_NO_ERROR;
    Int n = 0;
    if (resume > 0) {
        n = resume;
    } else if (blen == 0) {
        *err = io_err_unexpected_eof;
        return 0;
    } else if (b[0] == '"') {
        n++;
    } else {
        *err = burrow__jsonwire_new_invalid_character_error(
            b, blen, BURROW_S("at start of string (expecting '\"')"));
        return 0;
    }

    while (blen > n) {
        while (blen > n && jw_no_escape(b[n]))
            n++;
        if (blen <= n) {
            *err = io_err_unexpected_eof;
            return n;
        }
        if (b[n] == '"')
            return n + 1;

        Int rn;
        Rune r = jw_decode(b + n, blen - n, &rn);
        if (rn > 1) {
            n += rn;
        } else if (r == '\\') {
            *flags |= JSONWIRE_STRING_NON_VERBATIM;
            resume = n;
            if (blen < n + 2) {
                *err = io_err_unexpected_eof;
                return resume;
            }
            Byte c = b[n + 1];
            if (c == '/') {
                *flags |= JSONWIRE_STRING_NON_CANONICAL;
                n += 2;
            } else if (c == '"' || c == '\\' || c == 'b' || c == 'f' || c == 'n' ||
                       c == 'r' || c == 't') {
                n += 2;
            } else if (c == 'u') {
                if (blen < n + 6) {
                    if (jw_has_escaped_utf16_prefix(b + n, blen - n, false)) {
                        *err = io_err_unexpected_eof;
                        return resume;
                    }
                    *flags |= JSONWIRE_STRING_NON_CANONICAL;
                    *err = burrow__jsonwire_new_invalid_escape_sequence_error(b + n,
                                                                              blen - n);
                    return n;
                }
                uint16_t v1;
                if (!burrow__jsonwire_parse_hex_uint16(b + n + 2, 4, &v1)) {
                    *flags |= JSONWIRE_STRING_NON_CANONICAL;
                    *err = burrow__jsonwire_new_invalid_escape_sequence_error(b + n, 6);
                    return n;
                }
                if (v1 == '\b' || v1 == '\f' || v1 == '\n' || v1 == '\r' ||
                    v1 == '\t' || v1 >= ' ') {
                    *flags |= JSONWIRE_STRING_NON_CANONICAL;
                } else {
                    for (Int k = n + 2; k < n + 6; k++)
                        if (b[k] >= 'A' && b[k] <= 'F')
                            *flags |= JSONWIRE_STRING_NON_CANONICAL;
                }
                n += 6;

                Rune r1 = (Rune)v1;
                if (validate_utf8 && utf16_is_surrogate(r1)) {
                    uint16_t v2;
                    if (blen < n + 6) {
                        if (jw_has_escaped_utf16_prefix(b + n, blen - n, true)) {
                            *err = io_err_unexpected_eof;
                            return resume;
                        }
                        *flags |= JSONWIRE_STRING_NON_CANONICAL;
                        *err = burrow__jsonwire_new_invalid_escape_sequence_error(
                            b + n - 6, blen - n + 6);
                        return n - 6;
                    }
                    if (b[n] != '\\' || b[n + 1] != 'u' ||
                        !burrow__jsonwire_parse_hex_uint16(b + n + 2, 4, &v2) ||
                        utf16_decode_rune(r1, (Rune)v2) == UTF8_RUNE_ERROR) {
                        *flags |= JSONWIRE_STRING_NON_CANONICAL;
                        *err = burrow__jsonwire_new_invalid_escape_sequence_error(
                            b + n - 6, 12);
                        return n - 6;
                    }
                    n += 6;
                }
            } else {
                *flags |= JSONWIRE_STRING_NON_CANONICAL;
                *err = burrow__jsonwire_new_invalid_escape_sequence_error(b + n, 2);
                return n;
            }
        } else if (r == UTF8_RUNE_ERROR) {
            if (!jw_full_rune(b + n, blen - n)) {
                *err = io_err_unexpected_eof;
                return n;
            }
            *flags |= JSONWIRE_STRING_NON_VERBATIM | JSONWIRE_STRING_NON_CANONICAL;
            if (validate_utf8) {
                *err = burrow__jsonwire_err_invalid_utf8;
                return n;
            }
            n++;
        } else if (r < ' ') {
            *flags |= JSONWIRE_STRING_NON_VERBATIM | JSONWIRE_STRING_NON_CANONICAL;
            *err = burrow__jsonwire_new_invalid_character_error(
                b + n, blen - n,
                BURROW_S("in string (expecting non-control character)"));
            return n;
        } else {
            panic_str(BURROW_S("BUG: unhandled character"));
        }
    }
    *err = io_err_unexpected_eof;
    return n;
}

Error burrow__jsonwire_append_unquote(JsonBuf *dst, const Byte *src, Int slen) {
    jsonbuf_reserve(dst, slen);

    Int i, n;
    if (slen == 0)
        return io_err_unexpected_eof;
    if (src[0] != '"')
        return burrow__jsonwire_new_invalid_character_error(
            src, slen, BURROW_S("at start of string (expecting '\"')"));
    i = n = 1;

    Error err = BURROW_NO_ERROR;
    while (slen > n) {
        while (slen > n && jw_no_escape(src[n]))
            n++;
        if (slen <= n) {
            jsonbuf_put(dst, src + i, n - i);
            return io_err_unexpected_eof;
        }
        if (src[n] == '"') {
            jsonbuf_put(dst, src + i, n - i);
            n++;
            if (n < slen)
                err = burrow__jsonwire_new_invalid_character_error(
                    src + n, slen - n, BURROW_S("after string value"));
            return err;
        }

        Int rn;
        Rune r = jw_decode(src + n, slen - n, &rn);
        if (rn > 1) {
            n += rn;
        } else if (r == '\\') {
            jsonbuf_put(dst, src + i, n - i);
            if (slen < n + 2)
                return io_err_unexpected_eof;
            Byte c = src[n + 1];
            switch (c) {
            case '"':
            case '\\':
            case '/':
                jsonbuf_byte(dst, c);
                n += 2;
                break;
            case 'b':
                jsonbuf_byte(dst, '\b');
                n += 2;
                break;
            case 'f':
                jsonbuf_byte(dst, '\f');
                n += 2;
                break;
            case 'n':
                jsonbuf_byte(dst, '\n');
                n += 2;
                break;
            case 'r':
                jsonbuf_byte(dst, '\r');
                n += 2;
                break;
            case 't':
                jsonbuf_byte(dst, '\t');
                n += 2;
                break;
            case 'u': {
                if (slen < n + 6) {
                    if (jw_has_escaped_utf16_prefix(src + n, slen - n, false))
                        return io_err_unexpected_eof;
                    return burrow__jsonwire_new_invalid_escape_sequence_error(src + n,
                                                                              slen - n);
                }
                uint16_t v1;
                if (!burrow__jsonwire_parse_hex_uint16(src + n + 2, 4, &v1))
                    return burrow__jsonwire_new_invalid_escape_sequence_error(src + n,
                                                                              6);
                n += 6;
                Rune rr = (Rune)v1;
                if (utf16_is_surrogate(rr)) {
                    Rune r1 = rr;
                    uint16_t v2;
                    rr = UTF8_RUNE_ERROR;
                    if (slen < n + 6) {
                        if (jw_has_escaped_utf16_prefix(src + n, slen - n, true)) {
                            jw_put_rune(dst, rr);
                            return io_err_unexpected_eof;
                        }
                        err = burrow__jsonwire_new_invalid_escape_sequence_error(
                            src + n - 6, slen - n + 6);
                    } else if (src[n] != '\\' || src[n + 1] != 'u' ||
                               !burrow__jsonwire_parse_hex_uint16(src + n + 2, 4,
                                                                  &v2) ||
                               utf16_decode_rune(r1, (Rune)v2) == UTF8_RUNE_ERROR) {
                        err = burrow__jsonwire_new_invalid_escape_sequence_error(
                            src + n - 6, 12);
                    } else {
                        rr = utf16_decode_rune(r1, (Rune)v2);
                        n += 6;
                    }
                }
                jw_put_rune(dst, rr);
                break;
            }
            default:
                return burrow__jsonwire_new_invalid_escape_sequence_error(src + n, 2);
            }
            i = n;
        } else if (r == UTF8_RUNE_ERROR) {
            jsonbuf_put(dst, src + i, n - i);
            if (!jw_full_rune(src + n, slen - n))
                return io_err_unexpected_eof;
            jsonbuf_put(dst, "\xef\xbf\xbd", 3);
            n += rn;
            i = n;
            err = burrow__jsonwire_err_invalid_utf8;
        } else if (r < ' ') {
            jsonbuf_put(dst, src + i, n - i);
            return burrow__jsonwire_new_invalid_character_error(
                src + n, slen - n,
                BURROW_S("in string (expecting non-control character)"));
        } else {
            panic_str(BURROW_S("BUG: unhandled character"));
        }
    }
    jsonbuf_put(dst, src + i, n - i);
    return io_err_unexpected_eof;
}

Str burrow__jsonwire_unquote_may_copy(const Byte *b, Int n, bool is_verbatim,
                                      JsonBuf *scratch) {
    if (is_verbatim)
        return str_from_bytes(b + 1, n - 2);
    Int start = scratch->len;
    burrow__jsonwire_append_unquote(scratch, b, n);
    return str_from_bytes(scratch->p + start, scratch->len - start);
}

Int burrow__jsonwire_consume_number_resumable(const Byte *b, Int blen, Int resume,
                                              int *state, Error *err) {
    *err = BURROW_NO_ERROR;
    Int n = resume;
    int st = *state;
    if (st > JSONWIRE_NUMBER_INIT) {
        if (st == JSONWIRE_WITHIN_INTEGER_DIGITS ||
            st == JSONWIRE_WITHIN_FRACTIONAL_DIGITS ||
            st == JSONWIRE_WITHIN_EXPONENT_DIGITS) {
            while (blen > n && b[n] >= '0' && b[n] <= '9')
                n++;
            if (blen <= n)
                return n;
            st++;
        }
        if (st == JSONWIRE_BEFORE_INTEGER_DIGITS)
            goto before_integer;
        if (st == JSONWIRE_BEFORE_FRACTIONAL_DIGITS)
            goto before_fractional;
        if (st == JSONWIRE_BEFORE_EXPONENT_DIGITS)
            goto before_exponent;
        *state = st;
        return n;
    }

before_integer:
    resume = n;
    if (blen > 0 && b[0] == '-')
        n++;
    if (blen <= n) {
        *state = JSONWIRE_BEFORE_INTEGER_DIGITS;
        *err = io_err_unexpected_eof;
        return resume;
    }
    if (b[n] == '0') {
        n++;
        st = JSONWIRE_BEFORE_FRACTIONAL_DIGITS;
    } else if (b[n] >= '1' && b[n] <= '9') {
        n++;
        while (blen > n && b[n] >= '0' && b[n] <= '9')
            n++;
        st = JSONWIRE_WITHIN_INTEGER_DIGITS;
    } else {
        *state = st;
        *err = burrow__jsonwire_new_invalid_character_error(
            b + n, blen - n, BURROW_S("in number (expecting digit)"));
        return n;
    }

before_fractional:
    if (blen > n && b[n] == '.') {
        resume = n;
        n++;
        if (blen <= n) {
            *state = JSONWIRE_BEFORE_FRACTIONAL_DIGITS;
            *err = io_err_unexpected_eof;
            return resume;
        }
        if (b[n] >= '0' && b[n] <= '9') {
            n++;
        } else {
            *state = st;
            *err = burrow__jsonwire_new_invalid_character_error(
                b + n, blen - n, BURROW_S("in number (expecting digit)"));
            return n;
        }
        while (blen > n && b[n] >= '0' && b[n] <= '9')
            n++;
        st = JSONWIRE_WITHIN_FRACTIONAL_DIGITS;
    }

before_exponent:
    if (blen > n && (b[n] == 'e' || b[n] == 'E')) {
        resume = n;
        n++;
        if (blen > n && (b[n] == '-' || b[n] == '+'))
            n++;
        if (blen <= n) {
            *state = JSONWIRE_BEFORE_EXPONENT_DIGITS;
            *err = io_err_unexpected_eof;
            return resume;
        }
        if (b[n] >= '0' && b[n] <= '9') {
            n++;
        } else {
            *state = st;
            *err = burrow__jsonwire_new_invalid_character_error(
                b + n, blen - n, BURROW_S("in number (expecting digit)"));
            return n;
        }
        while (blen > n && b[n] >= '0' && b[n] <= '9')
            n++;
        st = JSONWIRE_WITHIN_EXPONENT_DIGITS;
    }

    *state = st;
    return n;
}

Int burrow__jsonwire_consume_number(const Byte *b, Int n, Error *err) {
    int state = JSONWIRE_NUMBER_INIT;
    return burrow__jsonwire_consume_number_resumable(b, n, 0, &state, err);
}

bool burrow__jsonwire_parse_uint(const Byte *b, Int blen, uint64_t *out) {
    uint64_t v = 0;
    Int n = 0;
    for (; blen > n && b[n] >= '0' && b[n] <= '9'; n++)
        v = 10 * v + (uint64_t)(b[n] - '0');
    if (n == 0 || blen != n || (b[0] == '0' && blen != 1)) {
        *out = 0;
        return false;
    }
    if (n >= 20 && (b[0] != '1' || v < 10000000000000000000ULL || n > 20)) {
        *out = UINT64_MAX;
        return false;
    }
    *out = v;
    return true;
}

/* ------------------------------------------------------------------- encode */

const Byte burrow__jsonwire_escape_ascii[128] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* control characters */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* control characters */
    0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* '"' and '&' */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, /* '<' and '>' */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, /* '\\' */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

bool burrow__jsonwire_need_escape(const Byte *src, Int slen) {
    Int i = 0;
    while (slen > i) {
        Byte c = src[i];
        if (c < 0x80) {
            if (burrow__jsonwire_escape_ascii[c] > 0)
                return true;
            i++;
        } else {
            Int rn;
            Rune r = jw_decode(src + i, slen - i, &rn);
            if (r == UTF8_RUNE_ERROR || r == 0x2028 || r == 0x2029)
                return true;
            i += rn;
        }
    }
    return false;
}

static void jw_append_escaped_utf16(JsonBuf *dst, uint16_t x) {
    static const char hex[] = "0123456789abcdef";
    Byte e[6] = {'\\',
                 'u',
                 (Byte)hex[(x >> 12) & 0xf],
                 (Byte)hex[(x >> 8) & 0xf],
                 (Byte)hex[(x >> 4) & 0xf],
                 (Byte)hex[x & 0xf]};
    jsonbuf_put(dst, e, 6);
}

static void jw_append_escaped_ascii(JsonBuf *dst, Byte c) {
    switch (c) {
    case '"':
    case '\\': {
        Byte e[2] = {'\\', c};
        jsonbuf_put(dst, e, 2);
        break;
    }
    case '\b':
        jsonbuf_put(dst, "\\b", 2);
        break;
    case '\f':
        jsonbuf_put(dst, "\\f", 2);
        break;
    case '\n':
        jsonbuf_put(dst, "\\n", 2);
        break;
    case '\r':
        jsonbuf_put(dst, "\\r", 2);
        break;
    case '\t':
        jsonbuf_put(dst, "\\t", 2);
        break;
    default:
        jw_append_escaped_utf16(dst, c);
    }
}

static void jw_append_escaped_unicode(JsonBuf *dst, Rune r) {
    Rune r2;
    Rune r1 = utf16_encode_rune(r, &r2);
    if (r1 != 0xfffd && r2 != 0xfffd) {
        jw_append_escaped_utf16(dst, (uint16_t)r1);
        jw_append_escaped_utf16(dst, (uint16_t)r2);
    } else {
        jw_append_escaped_utf16(dst, (uint16_t)r);
    }
}

Error burrow__jsonwire_append_quote(JsonBuf *dst, const Byte *src, Int slen,
                                    const JsontextOptions *flags) {
    Int i = 0, n = 0;
    bool has_invalid_utf8 = false;
    src = jw_nonnull(src);
    jsonbuf_reserve(dst, 1 + slen + 1);
    jsonbuf_byte(dst, '"');
    while (slen > n) {
        Byte c = src[n];
        if (c < 0x80) {
            n++;
            if (burrow__jsonwire_escape_ascii[c] == 0)
                continue;
            if (!(c == '<' || c == '>' || c == '&') ||
                jsonflags_get(flags, JSONFLAG_ESCAPE_FOR_HTML)) {
                jsonbuf_put(dst, src + i, n - 1 - i);
                jw_append_escaped_ascii(dst, c);
                i = n;
            }
        } else {
            Int rn;
            Rune r = jw_decode(src + n, slen - n, &rn);
            n += rn;
            if (r != UTF8_RUNE_ERROR && r != 0x2028 && r != 0x2029)
                continue;
            if (jw_is_invalid_utf8(r, rn)) {
                has_invalid_utf8 = true;
                jsonbuf_put(dst, src + i, n - rn - i);
                jsonbuf_put(dst, "\xef\xbf\xbd", 3);
                i = n;
            } else if ((r == 0x2028 || r == 0x2029) &&
                       jsonflags_get(flags, JSONFLAG_ESCAPE_FOR_JS)) {
                jsonbuf_put(dst, src + i, n - rn - i);
                jw_append_escaped_unicode(dst, r);
                i = n;
            }
        }
    }
    jsonbuf_put(dst, src + i, n - i);
    jsonbuf_byte(dst, '"');
    if (has_invalid_utf8 && !jsonflags_get(flags, JSONFLAG_ALLOW_INVALID_UTF8))
        return burrow__jsonwire_err_invalid_utf8;
    return BURROW_NO_ERROR;
}

Int burrow__jsonwire_reformat_string(JsonBuf *dst, const Byte *src, Int slen,
                                     const JsontextOptions *flags, Error *err) {
    unsigned vflags = 0;
    Int n = burrow__jsonwire_consume_string_resumable(
        &vflags, src, slen, 0, !jsonflags_get(flags, JSONFLAG_ALLOW_INVALID_UTF8), err);
    if (BURROW_FAILED(*err))
        return n;

    if (!jsonflags_get(flags, JSONFLAG_ANY_ESCAPE) &&
        ((vflags & JSONWIRE_STRING_NON_CANONICAL) == 0 ||
         jsonflags_get(flags, JSONFLAG_PRESERVE_RAW_STRINGS))) {
        jsonbuf_put(dst, src, n);
        return n;
    }

    if (jsonflags_get(flags, JSONFLAG_PRESERVE_RAW_STRINGS)) {
        Int i = 0, last = 0;
        while (i < n) {
            Byte c = src[i];
            if (c < 0x80) {
                if ((c == '<' || c == '>' || c == '&') &&
                    jsonflags_get(flags, JSONFLAG_ESCAPE_FOR_HTML)) {
                    jsonbuf_put(dst, src + last, i - last);
                    jw_append_escaped_ascii(dst, c);
                    last = i + 1;
                }
                i++;
            } else {
                Int rn;
                Rune r = jw_decode(src + i, n - i, &rn);
                if ((r == 0x2028 || r == 0x2029) &&
                    jsonflags_get(flags, JSONFLAG_ESCAPE_FOR_JS)) {
                    jsonbuf_put(dst, src + last, i - last);
                    jw_append_escaped_unicode(dst, r);
                    last = i + rn;
                }
                i += rn;
            }
        }
        jsonbuf_put(dst, src + last, n - last);
        return n;
    }

    JsonBuf tmp = {NULL, 0, 0, heap_allocator(), false, false};
    burrow__jsonwire_append_unquote(&tmp, src, n);
    if (tmp.failed)
        dst->failed = true;
    else
        burrow__jsonwire_append_quote(dst, tmp.p, tmp.len, flags);
    burrow__jsonbuf_free(&tmp);
    return n;
}

void burrow__jsonwire_append_float(JsonBuf *dst, double src, int bits) {
    if (bits == 32)
        src = (double)(float)src;
    double abs = src < 0 ? -src : src;
    Byte fmt = 'f';
    if (abs != 0) {
        if ((bits == 64 && (abs < 1e-6 || abs >= 1e21)) ||
            (bits == 32 && ((float)abs < 1e-6F || (float)abs >= 1e21F)))
            fmt = 'e';
    }
    Byte room[64];
    Slice s = {room, 0, (Int)sizeof(room), TYPE_BYTE};
    s = strconv_append_float(heap_allocator(), s, src, fmt, -1, bits);
    Byte *p = (Byte *)s.p;
    Int n = s.len;
    if (p == NULL) {
        dst->failed = true;
        return;
    }
    if (fmt == 'e' && n >= 4 && p[n - 4] == 'e' && p[n - 3] == '-' && p[n - 2] == '0') {
        p[n - 2] = p[n - 1];
        n--;
    }
    jsonbuf_put(dst, p, n);
    if (p != room)
        mem_free(heap_allocator(), p, (size_t)s.cap, 1);
}

Int burrow__jsonwire_reformat_number(JsonBuf *dst, const Byte *src, Int slen,
                                     const JsontextOptions *flags, Error *err) {
    Int n = burrow__jsonwire_consume_number(src, slen, err);
    if (BURROW_FAILED(*err))
        return n;
    if (!jsonflags_get(flags, JSONFLAG_CANONICALIZE_NUMBERS)) {
        jsonbuf_put(dst, src, n);
        return n;
    }

    bool is_float = false;
    for (Int k = 0; k < n; k++) {
        if (src[k] == '.' || src[k] == 'e' || src[k] == 'E') {
            is_float = true;
            break;
        }
    }

    if (n == 2 && src[0] == '-' && src[1] == '0') {
        /* -0 is 0 whatever kind it is */
    } else if (is_float) {
        if (!jsonflags_get(flags, JSONFLAG_CANONICALIZE_RAW_FLOATS)) {
            jsonbuf_put(dst, src, n);
            return n;
        }
    } else {
        if (!jsonflags_get(flags, JSONFLAG_CANONICALIZE_RAW_INTS) || n < 16) {
            jsonbuf_put(dst, src, n);
            return n;
        }
    }

    Error perr = BURROW_NO_ERROR;
    double fv = strconv_parse_float(str_from_bytes(src, n), 64, &perr);
    if (fv == 0)
        fv = 0;
    else if (burrow__json_isinf(fv) && fv > 0)
        fv = DBL_MAX;
    else if (burrow__json_isinf(fv))
        fv = -DBL_MAX;
    burrow__jsonwire_append_float(dst, fv, 64);
    return n;
}

/* --------------------------------------------------------------------- wire */

Int burrow__jsonwire_trim_suffix_whitespace(const Byte *b, Int n) {
    Int i = n - 1;
    while (i >= 0 && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
        i--;
    return i + 1;
}

Int burrow__jsonwire_trim_suffix_string(const Byte *b, Int n) {
    if (n > 0 && b[n - 1] == '"')
        n--;
    while (n >= 2 && !(b[n - 1] == '"' && b[n - 2] != '\\'))
        n--;
    if (n > 0 && b[n - 1] == '"')
        n--;
    return n;
}

Str burrow__jsonwire_quote_rune(const Byte *b, Int n, Byte out[16]) {
    Int rn;
    Rune r = jw_decode(b, n, &rn);
    if (r == UTF8_RUNE_ERROR && rn == 1) {
        static const char hex[] = "0123456789abcdef";
        Int k = 0;
        out[k++] = '\'';
        out[k++] = '\\';
        out[k++] = 'x';
        if (b[0] >= 16)
            out[k++] = (Byte)hex[b[0] >> 4];
        out[k++] = (Byte)hex[b[0] & 0xf];
        out[k++] = '\'';
        return str_from_bytes(out, k);
    }
    Slice s = {out, 0, 16, TYPE_BYTE};
    s = strconv_append_quote_rune(heap_allocator(), s, r);
    return str_from_bytes(s.p, s.len);
}

static bool jw_is_utf16_self(Rune r) {
    return (r >= 0 && r <= 0xd7ff) || (r >= 0xe000 && r <= 0xffff);
}

int burrow__jsonwire_compare_utf16(const Byte *x, Int nx, const Byte *y, Int ny) {
    for (;;) {
        if (nx == 0 || ny == 0)
            return nx < ny ? -1 : nx > ny ? 1 : 0;
        if (x[0] < 0x80 || y[0] < 0x80) {
            if (x[0] != y[0])
                return x[0] < y[0] ? -1 : 1;
            x++, nx--;
            y++, ny--;
            continue;
        }
        Int sx, sy;
        Rune rx = jw_decode(x, nx, &sx);
        Rune ry = jw_decode(y, ny, &sy);
        bool selfx = jw_is_utf16_self(rx);
        bool selfy = jw_is_utf16_self(ry);
        if (selfx && !selfy)
            ry = utf16_encode_rune(ry, NULL);
        else if (selfy && !selfx)
            rx = utf16_encode_rune(rx, NULL);
        if (rx != ry)
            return rx < ry ? -1 : 1;
        if (jw_is_invalid_utf8(rx, sx) || jw_is_invalid_utf8(ry, sy)) {
            if (x[0] != y[0])
                return x[0] < y[0] ? -1 : 1;
        }
        x += sx, nx -= sx;
        y += sy, ny -= sy;
    }
}

void burrow__jsonwire_truncate_pointer(JsonBuf *dst, Str s, Int n) {
    if (s.len <= n) {
        jsonbuf_str(dst, s);
        return;
    }
    Int i = n / 2;
    Int j = s.len - n / 2;

    for (Int k = i - 1; k > 0; k--) {
        if (s.p[k] == '/') {
            i = k;
            break;
        }
    }
    for (Int k = j; k < s.len; k++) {
        if (s.p[k] == '/') {
            j = k + 1;
            break;
        }
    }

    for (;;) {
        Int rn;
        if (i <= 0)
            break;
        Rune r = utf8_decode_last_rune_in_string(str_from_bytes(s.p, i), &rn);
        if (!jw_is_invalid_utf8(r, rn))
            break;
        i--;
    }
    for (;;) {
        Int rn;
        if (j >= s.len)
            break;
        Rune r = jw_decode(s.p + j, s.len - j, &rn);
        if (!jw_is_invalid_utf8(r, rn))
            break;
        j++;
    }

    Int slashes = 0;
    for (Int k = i; k < j; k++)
        if (s.p[k] == '/')
            slashes++;
    static const char ell[] = "\xe2\x80\xa6";
    JsonBuf mid = {NULL, 0, 0, NULL, false, false};
    Byte room[16];
    mid.p = room;
    mid.cap = (Int)sizeof(room);
    if (slashes == 0) {
        jsonbuf_put(&mid, ell, 3);
    } else {
        bool lead = j > i && s.p[i] == '/';
        bool trail = j > i && s.p[j - 1] == '/';
        if (!lead)
            jsonbuf_put(&mid, ell, 3);
        jsonbuf_byte(&mid, '/');
        if (slashes > 1) {
            jsonbuf_put(&mid, ell, 3);
            jsonbuf_byte(&mid, '/');
        }
        if (!trail)
            jsonbuf_put(&mid, ell, 3);
    }
    jsonbuf_put(dst, s.p, i);
    jsonbuf_put(dst, mid.p, mid.len);
    jsonbuf_put(dst, s.p + j, s.len - j);
}
