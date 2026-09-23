/* Derived from Go's src/internal/strconv/atoi.go and atob.go, and the NumError
 * half of src/strconv/number.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strconv.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* Go 1.27 split strconv in two: internal/strconv parses and reports a small
 * integer error code, and strconv turns the code into a *NumError. The same
 * split is here, for the same reason. The parsers only say what went wrong, and
 * the NumError, which costs an allocation and a quoted copy of the input, is
 * built only when the caller passed somewhere to put it. */

/* ----------------------------------------------------------------- NumError */

/* The public struct first, so the data pointer of the Error is a pointer to a
 * StrconvNumError and errors_as can hand it straight back. The message follows
 * because the message slot cannot allocate, so it is built once, up front. */
typedef struct NumErrorBox {
    StrconvNumError e;
    Str message;
} NumErrorBox;

static Str num_error_message(const void *self) {
    return ((const NumErrorBox *)self)->message;
}

static Error num_error_unwrap(const void *self) {
    return ((const StrconvNumError *)self)->err;
}

static const Type num_error_desc = {
    {(const Byte *)"NumError", 8},
    {(const Byte *)"strconv", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(StrconvNumError),
    (uint16_t)_Alignof(StrconvNumError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736e756dU, /* "snum" */
    NULL,
};

const Type *const TYPE_STRCONV_NUM_ERROR = &num_error_desc;

static Error num_error_clone(const void *self, Alloc *a);

static const ErrorVT num_error_vt = {
    &num_error_desc, num_error_message, num_error_unwrap, NULL, NULL, NULL,
    num_error_clone,
};

static const char ne_msg_strconv[] = "strconv.";
static const char ne_msg_parsing[] = ": parsing ";
static const char ne_msg_colon[] = ": ";

#define NE_LIT_LEN(s) ((Int)sizeof(s) - 1)

/* The length of "strconv." + func + ": parsing " + Quote(num) + ": " + text,
 * where quoted is the length of Quote(num). */
static Int ne_message_len(Str func, Int quoted, Str text) {
    return NE_LIT_LEN(ne_msg_strconv) + func.len + NE_LIT_LEN(ne_msg_parsing) + quoted +
           NE_LIT_LEN(ne_msg_colon) + text.len;
}

static Byte *ne_put(Byte *p, const void *src, Int n) {
    if (n > 0)
        memcpy(p, src, (size_t)n);
    return p + n;
}

static void ne_write_message(Byte *p, Str func, Str num, Str text) {
    p = ne_put(p, ne_msg_strconv, NE_LIT_LEN(ne_msg_strconv));
    p = ne_put(p, func.p, func.len);
    p = ne_put(p, ne_msg_parsing, NE_LIT_LEN(ne_msg_parsing));
    p += burrow__strconv_quote_into(p, num);
    p = ne_put(p, ne_msg_colon, NE_LIT_LEN(ne_msg_colon));
    ne_put(p, text.p, text.len);
}

/* One allocation: the box, then func, num and the message after it. A zero
 * Error means the allocation failed and leaves the choice of what to say
 * instead to the caller. */
static Error num_error_build(Alloc *a, Str func, Str num, Error inner) {
    Str text = error_text(inner);
    Int mlen = ne_message_len(func, burrow__strconv_quote_into(NULL, num), text);
    size_t size =
        sizeof(NumErrorBox) + (size_t)func.len + (size_t)num.len + (size_t)mlen;

    NumErrorBox *b = (NumErrorBox *)mem_alloc_nozero(a, size, _Alignof(NumErrorBox));
    if (b == NULL)
        return BURROW_NO_ERROR;

    Byte *p = (Byte *)(b + 1);
    b->e.func = str_from_bytes(p, func.len);
    p = ne_put(p, func.p, func.len);
    b->e.num = str_from_bytes(p, num.len);
    p = ne_put(p, num.p, num.len);
    b->e.err = inner;
    ne_write_message(p, func, num, text);
    b->message = str_from_bytes(p, mlen);
    return (Error){&num_error_vt, b};
}

Error strconv_num_error_as_error(Alloc *a, const StrconvNumError *e) {
    Error made = num_error_build(a, e->func, e->num, e->err);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

/* What error_retain calls. The wrapped error is retained first, because the
 * one a Parse function made for a bad base lives in the error arena too. */
static Error num_error_clone(const void *self, Alloc *a) {
    StrconvNumError copy = *(const StrconvNumError *)self;
    copy.err = error_retain(a, copy.err);
    return strconv_num_error_as_error(a, &copy);
}

Str strconv_num_error_error(Alloc *a, const StrconvNumError *e) {
    Str text = error_text(e->err);
    Int mlen = ne_message_len(e->func, burrow__strconv_quote_into(NULL, e->num), text);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    ne_write_message(p, e->func, e->num, text);
    return str_from_bytes(p, mlen);
}

Error strconv_num_error_unwrap(const StrconvNumError *e) {
    return e->err;
}

/* ------------------------------------------------------------ error codes */

/* internal/strconv's Error. */
typedef enum ParseCode {
    PARSE_OK,
    PARSE_RANGE,
    PARSE_SYNTAX,
    PARSE_BASE,
    PARSE_BIT_SIZE,
} ParseCode;

/* "invalid base -1" and the like, for the errors.New that Go builds. */
static Error parse_invalid_arg(Alloc *a, const char *what, Int v) {
    Byte buf[48];
    Int n = (Int)strlen(what);
    memcpy(buf, what, (size_t)n);

    Byte digits[24];
    Int d = (Int)sizeof(digits);
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        digits[--d] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        buf[n++] = '-';
    memcpy(buf + n, digits + d, (size_t)((Int)sizeof(digits) - d));
    n += (Int)sizeof(digits) - d;
    return errors_new(a, str_from_bytes(buf, n));
}

/* strconv's toError. A NumError that cannot be allocated falls back to the
 * reason alone, which errors_is still matches the way the caller expects. */
static Error parse_to_error(const char *func, Str s, Int base, Int bit_size,
                            ParseCode code) {
    Alloc *a = error_allocator();
    Error inner;

    switch (code) {
    case PARSE_RANGE:
        inner = strconv_err_range;
        break;
    case PARSE_SYNTAX:
        inner = strconv_err_syntax;
        break;
    case PARSE_BASE:
        inner = parse_invalid_arg(a, "invalid base ", base);
        break;
    case PARSE_BIT_SIZE:
        inner = parse_invalid_arg(a, "invalid bit size ", bit_size);
        break;
    case PARSE_OK:
    default:
        return BURROW_NO_ERROR;
    }

    return burrow__strconv_num_error(func, s, inner);
}

Error burrow__strconv_num_error(const char *func, Str s, Error inner) {
    Error e = num_error_build(error_allocator(), str_from_cstr(func), s, inner);
    return BURROW_FAILED(e) ? e : inner;
}

/* Only builds the error when there is somewhere to put it, since a caller that
 * passes NULL has said it does not want one. */
static void parse_report(Error *err, const char *func, Str s, Int base, Int bit_size,
                         ParseCode code) {
    if (err == NULL)
        return;
    *err = code == PARSE_OK ? BURROW_NO_ERROR
                            : parse_to_error(func, s, base, bit_size, code);
}

/* ----------------------------------------------------------------- integers */

/* atoi_lower(c) is a lower case letter if and only if c is that letter in either
 * case. Other bytes can come out as other non-letters. */
static Byte atoi_lower(Byte c) {
    return (Byte)(c | ('x' - 'X'));
}

/* Whether the underscores in s are where Go allows them: only between digits,
 * or between a base prefix and a digit. */
bool burrow__strconv_underscore_ok(Str s) {
    /* What was seen last: ^ for the start, 0 for a digit or base prefix, _ for
     * an underscore and ! for anything else. */
    char saw = '^';
    Int i = 0;

    if (s.len >= 1 && (s.p[0] == '-' || s.p[0] == '+')) {
        s.p++;
        s.len--;
    }

    bool hex = false;
    if (s.len >= 2 && s.p[0] == '0' &&
        (atoi_lower(s.p[1]) == 'b' || atoi_lower(s.p[1]) == 'o' ||
         atoi_lower(s.p[1]) == 'x')) {
        i = 2;
        saw = '0';
        hex = atoi_lower(s.p[1]) == 'x';
    }

    for (; i < s.len; i++) {
        Byte c = s.p[i];
        if (('0' <= c && c <= '9') ||
            (hex && 'a' <= atoi_lower(c) && atoi_lower(c) <= 'f')) {
            saw = '0';
            continue;
        }
        if (c == '_') {
            if (saw != '0')
                return false;
            saw = '_';
            continue;
        }
        if (saw == '_')
            return false;
        saw = '!';
    }
    return saw != '_';
}

static uint64_t parse_uint(Str s, Int base, Int bit_size, ParseCode *code) {
    if (s.len == 0) {
        *code = PARSE_SYNTAX;
        return 0;
    }

    bool base0 = base == 0;
    Str s0 = s;

    if (2 <= base && base <= 36) {
        /* valid, nothing to do */
    } else if (base == 0) {
        base = 10;
        if (s.p[0] == '0') {
            Int skip = 1;
            base = 8;
            if (s.len >= 3 && atoi_lower(s.p[1]) == 'b') {
                base = 2;
                skip = 2;
            } else if (s.len >= 3 && atoi_lower(s.p[1]) == 'o') {
                skip = 2;
            } else if (s.len >= 3 && atoi_lower(s.p[1]) == 'x') {
                base = 16;
                skip = 2;
            }
            s.p += skip;
            s.len -= skip;
        }
    } else {
        *code = PARSE_BASE;
        return 0;
    }

    if (bit_size == 0) {
        bit_size = STRCONV_INT_SIZE;
    } else if (bit_size < 0 || bit_size > 64) {
        *code = PARSE_BIT_SIZE;
        return 0;
    }

    /* The smallest number such that cutoff*base overflows. */
    uint64_t cutoff = UINT64_MAX / (uint64_t)base + 1;
    uint64_t max_val = bit_size == 64 ? UINT64_MAX : ((uint64_t)1 << bit_size) - 1;

    bool underscores = false;
    uint64_t n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        Byte d;
        if (c == '_' && base0) {
            underscores = true;
            continue;
        } else if ('0' <= c && c <= '9') {
            d = (Byte)(c - '0');
        } else if ('a' <= atoi_lower(c) && atoi_lower(c) <= 'z') {
            d = (Byte)(atoi_lower(c) - 'a' + 10);
        } else {
            *code = PARSE_SYNTAX;
            return 0;
        }

        if (d >= base) {
            *code = PARSE_SYNTAX;
            return 0;
        }

        if (n >= cutoff) {
            *code = PARSE_RANGE;
            return max_val;
        }
        n *= (uint64_t)base;

        uint64_t n1 = n + d;
        if (n1 < n || n1 > max_val) {
            *code = PARSE_RANGE;
            return max_val;
        }
        n = n1;
    }

    if (underscores && !burrow__strconv_underscore_ok(s0)) {
        *code = PARSE_SYNTAX;
        return 0;
    }
    return n;
}

static int64_t parse_int(Str s, Int base, Int bit_size, ParseCode *code) {
    if (s.len == 0) {
        *code = PARSE_SYNTAX;
        return 0;
    }

    bool neg = false;
    if (s.p[0] == '+' || s.p[0] == '-') {
        neg = s.p[0] == '-';
        s.p++;
        s.len--;
    }

    uint64_t un = parse_uint(s, base, bit_size, code);
    if (*code != PARSE_OK && *code != PARSE_RANGE)
        return 0;

    if (bit_size == 0)
        bit_size = STRCONV_INT_SIZE;

    /* Written so that the most negative value never passes through a
     * signed negation, which is the one that overflows. */
    uint64_t cutoff = (uint64_t)1 << (bit_size - 1);
    if (!neg && un >= cutoff) {
        *code = PARSE_RANGE;
        return (int64_t)(cutoff - 1);
    }
    if (neg && un > cutoff) {
        *code = PARSE_RANGE;
        return -(int64_t)(cutoff - 1) - 1;
    }
    if (!neg)
        return (int64_t)un;
    return un == 0 ? 0 : -(int64_t)(un - 1) - 1;
}

uint64_t strconv_parse_uint(Str s, Int base, Int bit_size, Error *err) {
    ParseCode code = PARSE_OK;
    uint64_t v = parse_uint(s, base, bit_size, &code);
    parse_report(err, "ParseUint", s, base, bit_size, code);
    return v;
}

int64_t strconv_parse_int(Str s, Int base, Int bit_size, Error *err) {
    ParseCode code = PARSE_OK;
    int64_t v = parse_int(s, base, bit_size, &code);
    parse_report(err, "ParseInt", s, base, bit_size, code);
    return v;
}

Int strconv_atoi(Str s, Error *err) {
    /* Fast path for anything short enough that it cannot overflow Int. */
    Int limit = STRCONV_INT_SIZE == 32 ? 10 : 19;
    if (0 < s.len && s.len < limit) {
        Str digits = s;
        if (s.p[0] == '-' || s.p[0] == '+') {
            digits.p++;
            digits.len--;
            if (digits.len < 1) {
                parse_report(err, "Atoi", s, 0, 0, PARSE_SYNTAX);
                return 0;
            }
        }

        Int n = 0;
        for (Int i = 0; i < digits.len; i++) {
            Byte ch = (Byte)(digits.p[i] - '0');
            if (ch > 9) {
                parse_report(err, "Atoi", s, 0, 0, PARSE_SYNTAX);
                return 0;
            }
            n = n * 10 + ch;
        }
        if (s.p[0] == '-')
            n = -n;
        BURROW_OUT(err, BURROW_NO_ERROR);
        return n;
    }

    /* Slow path for invalid, big or underscored integers. */
    ParseCode code = PARSE_OK;
    int64_t v = parse_int(s, 10, 0, &code);
    parse_report(err, "Atoi", s, 0, 0, code);
    return (Int)v;
}

/* ----------------------------------------------------------------- booleans */

static bool bool_one_of(Str s, const char *const *list) {
    for (; *list != NULL; list++)
        if (str_eq(s, str_from_cstr(*list)))
            return true;
    return false;
}

bool strconv_parse_bool(Str s, Error *err) {
    static const char *const yes[] = {"1", "t", "T", "true", "TRUE", "True", NULL};
    static const char *const no[] = {"0", "f", "F", "false", "FALSE", "False", NULL};

    if (bool_one_of(s, yes)) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return true;
    }
    if (bool_one_of(s, no)) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return false;
    }
    parse_report(err, "ParseBool", s, 0, 0, PARSE_SYNTAX);
    return false;
}

Str strconv_format_bool(bool b) {
    return b ? BURROW_S("true") : BURROW_S("false");
}

Slice strconv_append_bool(Alloc *a, Slice dst, bool b) {
    Str s = strconv_format_bool(b);
    if (dst.elem == NULL)
        dst = slice_nil(TYPE_BYTE);
    return slice_append(a, dst, s.p, s.len);
}
