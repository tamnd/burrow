/* time.ParseDuration and time.Duration.String.
 *
 * These are the two halves of time that have nothing to do with a calendar or
 * a clock, which is why they can land before the rest of the package. Both are
 * Go's code line for line, overflow checks included, because the edge cases
 * are where a port like this goes wrong: the largest negative duration has to
 * read back as itself, and a fraction with thirty digits has to round the same
 * way Go rounds it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------- String */

/* Go's fmtFrac. Writes the fraction of v/10**prec, such as ".12345", into the
 * tail of buf[0:w], leaving off trailing zeros and the point too when the
 * fraction is zero. Returns where the output starts, and v/10**prec in *v. */
static Int fmt_frac(Byte *buf, Int w, uint64_t *v, int prec) {
    bool print = false;
    for (int i = 0; i < prec; i++) {
        uint64_t digit = *v % 10;
        print = print || digit != 0;
        if (print)
            buf[--w] = (Byte)('0' + digit);
        *v /= 10;
    }
    if (print)
        buf[--w] = '.';
    return w;
}

/* Go's fmtInt. Writes v into the tail of buf[0:w] and returns where it
 * starts. */
static Int fmt_int(Byte *buf, Int w, uint64_t v) {
    if (v == 0) {
        buf[--w] = '0';
        return w;
    }
    while (v > 0) {
        buf[--w] = (Byte)('0' + v % 10);
        v /= 10;
    }
    return w;
}

/* Go's Duration.format, into the end of a 32 byte buffer. Returns where the
 * text starts. */
static Int dur_format(Duration d, Byte *buf) {
    Int w = DURATION_STRING_MAX;
    uint64_t u = (uint64_t)d;
    bool neg = d < 0;
    if (neg)
        u = 0 - u;

    if (u < (uint64_t)TIME_SECOND) {
        /* Under a second the unit gets smaller instead, as in 1.2ms. */
        int prec;
        buf[--w] = 's';
        w--;
        if (u == 0) {
            buf[w] = '0';
            return w;
        }
        if (u < (uint64_t)TIME_MICROSECOND) {
            prec = 0;
            buf[w] = 'n';
        } else if (u < (uint64_t)TIME_MILLISECOND) {
            prec = 3;
            /* U+00B5, the micro sign, is two bytes. */
            w--;
            buf[w] = 0xC2;
            buf[w + 1] = 0xB5;
        } else {
            prec = 6;
            buf[w] = 'm';
        }
        w = fmt_frac(buf, w, &u, prec);
        w = fmt_int(buf, w, u);
    } else {
        buf[--w] = 's';
        w = fmt_frac(buf, w, &u, 9);

        /* u is whole seconds now. */
        w = fmt_int(buf, w, u % 60);
        u /= 60;
        if (u > 0) {
            buf[--w] = 'm';
            w = fmt_int(buf, w, u % 60);
            u /= 60;
            /* Hours are as far as it goes, because a day is not always the
             * same length. */
            if (u > 0) {
                buf[--w] = 'h';
                w = fmt_int(buf, w, u);
            }
        }
    }

    if (neg)
        buf[--w] = '-';
    return w;
}

Int duration_format(Duration d, Byte *buf) {
    Byte tmp[DURATION_STRING_MAX];
    Int w = dur_format(d, tmp);
    Int n = DURATION_STRING_MAX - w;
    memcpy(buf, tmp + w, (size_t)n);
    return n;
}

Str duration_string(Duration d, Alloc *a) {
    Byte tmp[DURATION_STRING_MAX];
    Int w = dur_format(d, tmp);
    return str_clone(a, str_from_bytes(tmp + w, DURATION_STRING_MAX - w));
}

/* ------------------------------------------------------------ ParseDuration */

/* Go's leadingInt. Reads the digits at the front of s into *x and moves s past
 * them. False means the number would not fit. */
static bool leading_int(Str *s, uint64_t *x) {
    Int i = 0;
    *x = 0;
    for (; i < s->len; i++) {
        Byte c = s->p[i];
        if (c < '0' || c > '9')
            break;
        if (*x > ((uint64_t)1 << 63) / 10)
            return false;
        *x = *x * 10 + (uint64_t)(c - '0');
        if (*x > (uint64_t)1 << 63)
            return false;
    }
    s->p += i;
    s->len -= i;
    return true;
}

/* Go's leadingFraction. Reads the digits at the front of s as the part after
 * a decimal point, so the value is *x / *scale. Digits past what fits are
 * read and dropped, which is precision nobody can see in nanoseconds. */
static void leading_fraction(Str *s, uint64_t *x, double *scale) {
    Int i = 0;
    bool overflow = false;
    *x = 0;
    *scale = 1;
    for (; i < s->len; i++) {
        Byte c = s->p[i];
        if (c < '0' || c > '9')
            break;
        if (overflow)
            continue;
        if (*x > (((uint64_t)1 << 63) - 1) / 10) {
            overflow = true;
            continue;
        }
        uint64_t y = *x * 10 + (uint64_t)(c - '0');
        if (y > (uint64_t)1 << 63) {
            overflow = true;
            continue;
        }
        *x = y;
        *scale *= 10;
    }
    s->p += i;
    s->len -= i;
}

/* Go's unitMap, which is small enough that a list does the job of a map. */
static uint64_t unit_of(Str u) {
    static const struct {
        const char *name;
        uint64_t n;
    } units[] = {
        {"ns", (uint64_t)TIME_NANOSECOND},
        {"us", (uint64_t)TIME_MICROSECOND},
        {"\xC2\xB5s", (uint64_t)TIME_MICROSECOND}, /* U+00B5, the micro sign */
        {"\xCE\xBCs", (uint64_t)TIME_MICROSECOND}, /* U+03BC, the Greek mu */
        {"ms", (uint64_t)TIME_MILLISECOND},
        {"s", (uint64_t)TIME_SECOND},
        {"m", (uint64_t)TIME_MINUTE},
        {"h", (uint64_t)TIME_HOUR},
    };
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        size_t n = strlen(units[i].name);
        if ((size_t)u.len == n && memcmp(u.p, units[i].name, n) == 0)
            return units[i].n;
    }
    return 0;
}

/* Go's time.quote, which is not strconv.Quote. Anything outside printable
 * ASCII comes out as \x escapes, byte by byte, and a quote or a backslash gets
 * a backslash. Returns the length, and writes only when out is not NULL, so it
 * can be called once to measure and once to fill. */
static Int quote(Str s, Byte *out) {
    static const char hex[] = "0123456789abcdef";
    Int n = 0;
#define PUT(c)                                                                         \
    do {                                                                               \
        if (out)                                                                       \
            out[n] = (Byte)(c);                                                        \
        n++;                                                                           \
    } while (0)
    PUT('"');
    for (Int i = 0; i < s.len;) {
        /* A run of bytes that go out as they are is copied in one go. */
        Int j = i;
        while (j < s.len && s.p[j] >= ' ' && s.p[j] < UTF8_RUNE_SELF && s.p[j] != '"' &&
               s.p[j] != '\\')
            j++;
        if (j > i) {
            if (out)
                memcpy(out + n, s.p + i, (size_t)(j - i));
            n += j - i;
            i = j;
            continue;
        }
        /* ASCII needs no decoding, which is what Go's range over a string
         * does too, and it is nearly every byte a duration has. */
        Int width = 1;
        Rune c = s.p[i];
        if (c >= UTF8_RUNE_SELF)
            c = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &width);
        if (c >= UTF8_RUNE_SELF || c < ' ') {
            for (Int k = 0; k < width; k++) {
                PUT('\\');
                PUT('x');
                PUT(hex[s.p[i + k] >> 4]);
                PUT(hex[s.p[i + k] & 0xF]);
            }
        } else {
            if (c == '"' || c == '\\')
                PUT('\\');
            PUT(c);
        }
        i += width;
    }
    PUT('"');
#undef PUT
    return n;
}

/* Go's parseDurationError: "time: " + message + " " + quote(value). message
 * is either a C string or, for the unknown unit case, the prefix, the quoted
 * unit and the suffix. */
static Error dur_error(const char *msg, Str unit, const char *tail, Str orig) {
    size_t ml = strlen(msg);
    size_t tl = tail ? strlen(tail) : 0;
    /* A quoted byte is four bytes at most, so short input, which is nearly
     * all of it, is built on the stack in one pass and copied once. */
    Byte small[256];
    Int most =
        6 + (Int)ml + (tail ? 2 + 4 * unit.len + (Int)tl : 0) + 1 + 2 + 4 * orig.len;
    Byte *buf = small;
    Alloc *a = error_allocator();
    if (most > (Int)sizeof small) {
        Int total = 6 + (Int)ml + (tail ? quote(unit, NULL) + (Int)tl : 0) + 1 +
                    quote(orig, NULL);
        buf = mem_alloc(a, (size_t)total, 1);
        if (buf == NULL)
            return burrow_err_out_of_memory;
    }
    Int n = 0;
    memcpy(buf, "time: ", 6);
    n += 6;
    memcpy(buf + n, msg, ml);
    n += (Int)ml;
    if (tail) {
        n += quote(unit, buf + n);
        memcpy(buf + n, tail, tl);
        n += (Int)tl;
    }
    buf[n++] = ' ';
    n += quote(orig, buf + n);
    Error e = errors_new(a, str_from_bytes(buf, n));
    if (buf != small)
        mem_free(a, buf, (size_t)n, 1);
    return e;
}

static Duration dur_fail(Error *err, Error e) {
    if (err)
        *err = e;
    return 0;
}

#define INVALID(orig) dur_fail(err, dur_error("invalid duration", (Str){0}, NULL, orig))

Duration time_parse_duration(Str s, Error *err) {
    Str orig = s;
    uint64_t d = 0;
    bool neg = false;

    if (err)
        *err = (Error){0};
    if (s.len > 0) {
        Byte c = s.p[0];
        if (c == '-' || c == '+') {
            neg = c == '-';
            s.p++;
            s.len--;
        }
    }
    /* A bare zero is the one number that needs no unit. */
    if (s.len == 1 && s.p[0] == '0')
        return 0;
    if (s.len == 0)
        return INVALID(orig);

    while (s.len > 0) {
        uint64_t v;
        uint64_t f = 0;
        double scale = 1;

        /* The next character has to be a digit or a point. */
        if (!(s.p[0] == '.' || ('0' <= s.p[0] && s.p[0] <= '9')))
            return INVALID(orig);

        /* The whole part. */
        Int pl = s.len;
        if (!leading_int(&s, &v))
            return INVALID(orig);
        bool pre = pl != s.len;

        /* The fraction. */
        bool post = false;
        if (s.len > 0 && s.p[0] == '.') {
            s.p++;
            s.len--;
            pl = s.len;
            leading_fraction(&s, &f, &scale);
            post = pl != s.len;
        }
        if (!pre && !post)
            return INVALID(orig);

        /* The unit. */
        Int i = 0;
        for (; i < s.len; i++) {
            Byte c = s.p[i];
            if (c == '.' || ('0' <= c && c <= '9'))
                break;
        }
        if (i == 0)
            return dur_fail(
                err, dur_error("missing unit in duration", (Str){0}, NULL, orig));
        Str u = str_from_bytes(s.p, i);
        s.p += i;
        s.len -= i;
        uint64_t unit = unit_of(u);
        if (unit == 0)
            return dur_fail(err, dur_error("unknown unit ", u, " in duration", orig));
        if (v > ((uint64_t)1 << 63) / unit)
            return INVALID(orig);
        v *= unit;
        if (f > 0) {
            /* float64 is needed so that the fraction is not lost to
             * overflow, as in Go. v is at most 1<<63 here, so this cannot
             * wrap. */
            v += (uint64_t)((double)f * ((double)unit / scale));
            if (v > (uint64_t)1 << 63)
                return INVALID(orig);
        }
        d += v;
        if (d > (uint64_t)1 << 63)
            return INVALID(orig);
    }
    if (neg)
        return (Duration)(0 - d);
    if (d > ((uint64_t)1 << 63) - 1)
        return INVALID(orig);
    return (Duration)d;
}

/* The descriptor. */
static Str duration_m_string(Duration *self) {
    return duration_string(*self, error_allocator());
}

#define DURATION_SIG_STRING(IN, OUT) OUT(Str)
#define DURATION_METHODS(M, T) M(T, String, duration_m_string, DURATION_SIG_STRING)

BURROW_METHODS_DEFINE(Duration, DURATION_METHODS);

const Type burrow_type_Duration = {
    {(const Byte *)"Duration", 8},
    {(const Byte *)"time", 4},
    KIND_INT64,
    (uint32_t)sizeof(Duration),
    (uint16_t)_Alignof(Duration),
    0,
    (uint16_t)(sizeof(burrow__methods_Duration) / sizeof(burrow__methods_Duration[0])),
    NULL,
    burrow__methods_Duration,
    NULL,
    NULL,
    0,
    0x74647572U, /* "tdur" */
    NULL,
};
