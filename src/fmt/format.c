/* Derived from Go's src/fmt/format.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/fmt.h"

#include "format.h"

#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <string.h>

/* ------------------------------------------------------------------ buffer */

void burrow__fmt_buf_grow(FmtBuf *b, Int n) {
    if (b->failed)
        return;
    Int want = b->len + n;
    Int cap = b->cap * 2;
    if (cap < want)
        cap = want;
    if (cap < 64)
        cap = 64;
    Byte *p = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
    if (p == NULL) {
        b->failed = true;
        return;
    }
    if (b->len > 0)
        memcpy(p, b->p, (size_t)b->len);
    if (b->heap)
        mem_free(heap_allocator(), b->p, (size_t)b->cap, 1);
    b->p = p;
    b->cap = cap;
    b->heap = true;
}

void burrow__fmt_buf_free(FmtBuf *b) {
    if (b->heap)
        mem_free(heap_allocator(), b->p, (size_t)b->cap, 1);
    b->heap = false;
    b->p = NULL;
    b->len = b->cap = 0;
}

void burrow__fmt_buf_write_rune(FmtBuf *b, Rune r) {
    if ((uint32_t)r < (uint32_t)UTF8_RUNE_SELF) {
        fmt_buf_write_byte(b, (Byte)r);
        return;
    }
    Byte tmp[UTF8_UTF_MAX];
    Int n = utf8_encode_rune(slice_from(tmp, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
    fmt_buf_write(b, tmp, n);
}

/* A scratch buffer that is intbuf when that is big enough and the heap when it
 * is not, for the few verbs whose output grows with the width or precision. */
static Byte *fmt_scratch(Fmt *f, Int n) {
    if (n <= (Int)sizeof f->intbuf)
        return f->intbuf;
    return (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)n, 1);
}

static void fmt_scratch_free(Fmt *f, Byte *p, Int n) {
    if (p != f->intbuf && p != NULL)
        mem_free(heap_allocator(), p, (size_t)n, 1);
}

/* A Slice over intbuf with nothing in it, for the strconv appends, which move
 * to the heap by themselves when intbuf is not enough. */
static Slice intbuf_slice(Fmt *f, Int len) {
    return slice_from(f->intbuf, len, (Int)sizeof f->intbuf, TYPE_BYTE);
}

static void intbuf_slice_free(Fmt *f, Slice s) {
    if (s.p != (void *)f->intbuf && s.p != NULL)
        mem_free(heap_allocator(), s.p, (size_t)s.cap, 1);
}

/* ------------------------------------------------------------------ fmt */

void burrow__fmt_clearflags(Fmt *f) {
    memset(&f->f, 0, sizeof f->f);
    f->wid = 0;
    f->prec = 0;
}

void burrow__fmt_write_padding(Fmt *f, Int n) {
    if (n <= 0)
        return;
    FmtBuf *b = f->buf;
    if (b->cap - b->len < n) {
        burrow__fmt_buf_grow(b, n);
        if (b->failed)
            return;
    }
    Byte pad = (f->f.zero && !f->f.minus) ? '0' : ' ';
    memset(b->p + b->len, pad, (size_t)n);
    b->len += n;
}

void burrow__fmt_pad(Fmt *f, const Byte *b, Int n) {
    if (!f->f.wid_present || f->wid == 0) {
        fmt_buf_write(f->buf, b, n);
        return;
    }
    Int width =
        f->wid - utf8_rune_count(slice_from((void *)(uintptr_t)b, n, n, TYPE_BYTE));
    if (!f->f.minus) {
        burrow__fmt_write_padding(f, width);
        fmt_buf_write(f->buf, b, n);
    } else {
        fmt_buf_write(f->buf, b, n);
        burrow__fmt_write_padding(f, width);
    }
}

void burrow__fmt_pad_string(Fmt *f, Str s) {
    burrow__fmt_pad(f, s.p, s.len);
}

void burrow__fmt_boolean(Fmt *f, bool v) {
    if (v)
        burrow__fmt_pad_string(f, BURROW_S("true"));
    else
        burrow__fmt_pad_string(f, BURROW_S("false"));
}

/* U+0078 or, with #, U+0078 'x'. */
void burrow__fmt_unicode(Fmt *f, uint64_t u) {
    Byte *buf = f->intbuf;
    Int len = (Int)sizeof f->intbuf;

    Int prec = 4;
    if (f->f.prec_present && f->prec > 4) {
        prec = f->prec;
        Int width = 2 + prec + 2 + UTF8_UTF_MAX + 1;
        if (width > len) {
            buf = fmt_scratch(f, width);
            if (buf == NULL)
                return;
            len = width;
        }
    }

    Int i = len;

    if (f->f.sharp && u <= (uint64_t)UTF8_MAX_RUNE && strconv_is_print((Rune)u)) {
        buf[--i] = '\'';
        i -= utf8_rune_len((Rune)u);
        utf8_encode_rune(slice_from(buf + i, len - i, len - i, TYPE_BYTE), (Rune)u);
        buf[--i] = '\'';
        buf[--i] = ' ';
    }
    while (u >= 16) {
        buf[--i] = (Byte)FMT_UDIGITS[u & 0xF];
        prec--;
        u >>= 4;
    }
    buf[--i] = (Byte)FMT_UDIGITS[u];
    prec--;
    while (prec > 0) {
        buf[--i] = '0';
        prec--;
    }
    buf[--i] = '+';
    buf[--i] = 'U';

    bool old_zero = f->f.zero;
    f->f.zero = false;
    burrow__fmt_pad(f, buf + i, len - i);
    f->f.zero = old_zero;
    if (buf != f->intbuf)
        fmt_scratch_free(f, buf, len);
}

void burrow__fmt_integer(Fmt *f, uint64_t u, Int base, bool is_signed, Rune verb,
                         const char *digits) {
    bool negative = is_signed && (int64_t)u < 0;
    if (negative)
        u = 0 - u;

    Byte *buf = f->intbuf;
    Int len = (Int)sizeof f->intbuf;
    if (f->f.wid_present || f->f.prec_present) {
        /* Room for the digits, a sign, and the 0x. */
        Int width = 3 + f->wid + f->prec;
        if (width > len) {
            buf = fmt_scratch(f, width);
            if (buf == NULL)
                return;
            len = width;
        }
    }

    /* Two ways to ask for leading zeros, the precision and the 0 flag with a
     * width, and the precision wins when there are both. */
    Int prec = 0;
    if (f->f.prec_present) {
        prec = f->prec;
        /* A precision of zero and a value of zero means no digits at all. */
        if (prec == 0 && u == 0) {
            bool old_zero = f->f.zero;
            f->f.zero = false;
            burrow__fmt_write_padding(f, f->wid);
            f->f.zero = old_zero;
            if (buf != f->intbuf)
                fmt_scratch_free(f, buf, len);
            return;
        }
    } else if (f->f.zero && !f->f.minus && f->f.wid_present) {
        prec = f->wid;
        if (negative || f->f.plus || f->f.space)
            prec--; /* leave room for the sign */
    }

    Int i = len;
    switch (base) {
    case 10:
        while (u >= 10) {
            uint64_t next = u / 10;
            buf[--i] = (Byte)('0' + u - next * 10);
            u = next;
        }
        break;
    case 16:
        while (u >= 16) {
            buf[--i] = (Byte)digits[u & 0xF];
            u >>= 4;
        }
        break;
    case 8:
        while (u >= 8) {
            buf[--i] = (Byte)('0' + (u & 7));
            u >>= 3;
        }
        break;
    case 2:
        while (u >= 2) {
            buf[--i] = (Byte)('0' + (u & 1));
            u >>= 1;
        }
        break;
    default:
        panic_str(BURROW_S("fmt: unknown base; can't happen"));
    }
    buf[--i] = (Byte)digits[u];
    while (i > 0 && prec > len - i)
        buf[--i] = '0';

    if (f->f.sharp) {
        switch (base) {
        case 2:
            buf[--i] = 'b';
            buf[--i] = '0';
            break;
        case 8:
            if (buf[i] != '0')
                buf[--i] = '0';
            break;
        case 16:
            buf[--i] = (Byte)digits[16];
            buf[--i] = '0';
            break;
        default:
            break;
        }
    }
    if (verb == 'O') {
        buf[--i] = 'o';
        buf[--i] = '0';
    }

    if (negative)
        buf[--i] = '-';
    else if (f->f.plus)
        buf[--i] = '+';
    else if (f->f.space)
        buf[--i] = ' ';

    /* The zeros are already in the digits, so the padding is spaces. */
    bool old_zero = f->f.zero;
    f->f.zero = false;
    burrow__fmt_pad(f, buf + i, len - i);
    f->f.zero = old_zero;
    if (buf != f->intbuf)
        fmt_scratch_free(f, buf, len);
}

/* s cut to the precision, counted in runes. */
static Int truncate_len(Fmt *f, const Byte *s, Int n) {
    if (f->f.prec_present) {
        Int left = f->prec;
        Int i = 0;
        while (i < n) {
            if (--left < 0)
                return i;
            Int w = 1;
            if (s[i] >= UTF8_RUNE_SELF)
                utf8_decode_rune(
                    slice_from((void *)(uintptr_t)(s + i), n - i, n - i, TYPE_BYTE),
                    &w);
            i += w;
        }
    }
    return n;
}

void burrow__fmt_s(Fmt *f, Str s) {
    burrow__fmt_pad(f, s.p, truncate_len(f, s.p, s.len));
}

void burrow__fmt_bs(Fmt *f, const Byte *b, Int n) {
    burrow__fmt_pad(f, b, truncate_len(f, b, n));
}

/* %x and %X of a string or a byte slice, which in C are the same bytes. */
void burrow__fmt_sbx(Fmt *f, const Byte *s, Int n, const char *digits) {
    Int length = n;
    if (f->f.prec_present && f->prec < length)
        length = f->prec;
    Int width = 2 * length;
    if (width > 0) {
        if (f->f.space) {
            if (f->f.sharp)
                width *= 2;
            width += length - 1;
        } else if (f->f.sharp) {
            width += 2;
        }
    } else {
        if (f->f.wid_present)
            burrow__fmt_write_padding(f, f->wid);
        return;
    }
    if (f->f.wid_present && f->wid > width && !f->f.minus)
        burrow__fmt_write_padding(f, f->wid - width);
    FmtBuf *b = f->buf;
    if (f->f.sharp) {
        fmt_buf_write_byte(b, '0');
        fmt_buf_write_byte(b, (Byte)digits[16]);
    }
    for (Int i = 0; i < length; i++) {
        if (f->f.space && i > 0) {
            fmt_buf_write_byte(b, ' ');
            if (f->f.sharp) {
                fmt_buf_write_byte(b, '0');
                fmt_buf_write_byte(b, (Byte)digits[16]);
            }
        }
        Byte c = s[i];
        fmt_buf_write_byte(b, (Byte)digits[c >> 4]);
        fmt_buf_write_byte(b, (Byte)digits[c & 0xF]);
    }
    if (f->f.wid_present && f->wid > width && f->f.minus)
        burrow__fmt_write_padding(f, f->wid - width);
}

void burrow__fmt_q(Fmt *f, Str s) {
    s.len = truncate_len(f, s.p, s.len);
    if (f->f.sharp && strconv_can_backquote(s)) {
        Int n = s.len + 2;
        Byte *p = fmt_scratch(f, n);
        if (p == NULL)
            return;
        p[0] = '`';
        if (s.len > 0)
            memcpy(p + 1, s.p, (size_t)s.len);
        p[n - 1] = '`';
        burrow__fmt_pad(f, p, n);
        fmt_scratch_free(f, p, n);
        return;
    }
    Slice out;
    if (f->f.plus)
        out = strconv_append_quote_to_ascii(heap_allocator(), intbuf_slice(f, 0), s);
    else
        out = strconv_append_quote(heap_allocator(), intbuf_slice(f, 0), s);
    burrow__fmt_pad(f, (const Byte *)out.p, out.len);
    intbuf_slice_free(f, out);
}

void burrow__fmt_c(Fmt *f, uint64_t c) {
    Rune r = (Rune)c;
    if (c > (uint64_t)UTF8_MAX_RUNE)
        r = UTF8_RUNE_ERROR;
    Int n = utf8_encode_rune(intbuf_slice(f, (Int)sizeof f->intbuf), r);
    burrow__fmt_pad(f, f->intbuf, n);
}

void burrow__fmt_qc(Fmt *f, uint64_t c) {
    Rune r = (Rune)c;
    if (c > (uint64_t)UTF8_MAX_RUNE)
        r = UTF8_RUNE_ERROR;
    Slice out;
    if (f->f.plus)
        out =
            strconv_append_quote_rune_to_ascii(heap_allocator(), intbuf_slice(f, 0), r);
    else
        out = strconv_append_quote_rune(heap_allocator(), intbuf_slice(f, 0), r);
    burrow__fmt_pad(f, (const Byte *)out.p, out.len);
    intbuf_slice_free(f, out);
}

static Slice append_byte(Slice s, Byte c) {
    return slice_append(heap_allocator(), s, &c, 1);
}

/* A float, with the sign handled here rather than by strconv so that the 0 flag
 * can put its zeros between the sign and the digits. */
void burrow__fmt_float(Fmt *f, double v, Int size, Rune verb, Int prec) {
    if (f->f.prec_present)
        prec = f->prec;
    /* One byte in front for the sign, which strconv leaves out when it is +. */
    Slice num = strconv_append_float(heap_allocator(), intbuf_slice(f, 1), v,
                                     (Byte)verb, prec, size);
    Byte *n = (Byte *)num.p;
    Int start = 0;
    if (n[1] == '-' || n[1] == '+')
        start = 1;
    else
        n[0] = '+';
    /* A space instead of a + when the space flag asks for one. */
    if (f->f.space && n[start] == '+' && !f->f.plus)
        n[start] = ' ';
    /* Infinities and NaNs are padded with spaces, never zeros. */
    if (n[start + 1] == 'I' || n[start + 1] == 'N') {
        bool old_zero = f->f.zero;
        f->f.zero = false;
        /* No sign on a NaN unless one was asked for. */
        if (n[start + 1] == 'N' && !f->f.space && !f->f.plus)
            start++;
        burrow__fmt_pad(f, n + start, num.len - start);
        f->f.zero = old_zero;
        intbuf_slice_free(f, num);
        return;
    }
    /* The # flag keeps the decimal point, and for %g and %v the trailing zeros
     * too, up to the precision. */
    if (f->f.sharp && verb != 'b') {
        Int digits = 0;
        switch (verb) {
        case 'v':
        case 'g':
        case 'G':
        case 'x':
            digits = prec;
            /* %e is used if the exponent from the conversion is less than -4
             * or greater than or equal to the precision. If the precision was
             * the shortest possible, use a precision of 6 for this decision. */
            if (digits == -1)
                digits = 6;
            break;
        default:
            break;
        }

        /* The exponent, which goes back on after the zeros. Five bytes cover
         * e+308 and six cover p+1023, the longest either can be. */
        Byte tail[8];
        Int ntail = 0;

        bool has_decimal_point = false;
        bool saw_nonzero_digit = false;
        Int end = num.len;
        for (Int i = start + 1; i < end; i++) {
            Byte c = n[i];
            if (c == '.') {
                has_decimal_point = true;
            } else if (c == 'p' || c == 'P' ||
                       ((c == 'e' || c == 'E') && verb != 'x' && verb != 'X')) {
                ntail = end - i;
                memcpy(tail, n + i, (size_t)ntail);
                end = i;
            } else {
                if (c != '0')
                    saw_nonzero_digit = true;
                /* Count significant digits after the first non-zero one. */
                if (saw_nonzero_digit)
                    digits--;
            }
        }
        num.len = end;
        if (!has_decimal_point) {
            /* A leading 0 should be counted as a significant digit. */
            if (end - start == 2 && n[start + 1] == '0')
                digits--;
            num = append_byte(num, '.');
        }
        while (digits > 0) {
            num = append_byte(num, '0');
            digits--;
        }
        if (ntail > 0)
            num = slice_append(heap_allocator(), num, tail, ntail);
        n = (Byte *)num.p;
        if (n == NULL)
            return;
    }
    /* The sign is written when it was asked for or is not a plus. */
    if (f->f.plus || n[start] != '+') {
        /* With zero padding the zeros go after the sign. */
        Int len = num.len - start;
        if (f->f.zero && !f->f.minus && f->f.wid_present && f->wid > len) {
            fmt_buf_write_byte(f->buf, n[start]);
            burrow__fmt_write_padding(f, f->wid - len);
            fmt_buf_write(f->buf, n + start + 1, len - 1);
        } else {
            burrow__fmt_pad(f, n + start, len);
        }
        intbuf_slice_free(f, num);
        return;
    }
    /* No sign to show and the number is positive, so just the digits. */
    burrow__fmt_pad(f, n + start + 1, num.len - start - 1);
    intbuf_slice_free(f, num);
}
