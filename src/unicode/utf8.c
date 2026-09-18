/* Derived from Go's src/unicode/utf8/utf8.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/utf8.h"
#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

/* The port is close to the letter, because this package is a table and a set of
 * comparisons against it and there is nothing to gain by rearranging either.
 * The one structural change is that Go's two copies of every function, one for
 * string and one for []byte, are one function over a pointer and a length here,
 * with the two public spellings on top. Go needs the copies because a string
 * and a []byte are different types and generics arrived after this package was
 * written. In C they are the same two words.
 *
 * Go's ASCII fast paths are written the way they are to get the function past
 * the inliner's cost limit, which is a fact about Go's compiler and not about
 * the encoding. They are kept anyway, spelled naturally, because the branch
 * they take is the one almost every input takes. */

/* Runes in the surrogate range are not valid UTF-8. They exist because UTF-16
 * needs a way to spell a code point above 0xFFFF, and a program that converts
 * from UTF-16 badly is the usual way one turns up in a byte stream. */
#define SURROGATE_MIN 0xD800
#define SURROGATE_MAX 0xDFFF

#define TX 0x80 /* 0b10000000, the tag on a continuation byte */
#define T2 0xC0 /* 0b11000000, the tag on a two byte starter */
#define T3 0xE0 /* 0b11100000 */
#define T4 0xF0 /* 0b11110000 */

#define MASKX 0x3F /* the six payload bits of a continuation byte */
#define MASK2 0x1F
#define MASK3 0x0F
#define MASK4 0x07

#define RUNE1_MAX ((1 << 7) - 1)
#define RUNE2_MAX ((1 << 11) - 1)
#define RUNE3_MAX ((1 << 16) - 1)

/* The lowest and highest continuation byte, which is the range four of the five
 * accept ranges below are built out of. */
#define LOCB 0x80
#define HICB 0xBF

/* What the first byte of a sequence tells you, in one table lookup.
 *
 * The high nibble is an index into accept_ranges below, or 0xF for the two one
 * byte cases. The low nibble is the length of the sequence, or the status for
 * those one byte cases. The names are two characters each because that is what
 * makes the table below line up in columns, which is how it is checked against
 * Go's by eye. */
#define XX 0xF1 /* invalid, and it consumes one byte */
#define AS 0xF0 /* ASCII, one byte */
#define S1 0x02 /* accept range 0, two bytes */
#define S2 0x13 /* accept range 1, three bytes */
#define S3 0x03 /* accept range 0, three bytes */
#define S4 0x23 /* accept range 2, three bytes */
#define S5 0x34 /* accept range 3, four bytes */
#define S6 0x04 /* accept range 0, four bytes */
#define S7 0x44 /* accept range 4, four bytes */

static const uint8_t first[256] = {
    /*  1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x00-0x0F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x10-0x1F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x20-0x2F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x30-0x3F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x40-0x4F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x50-0x5F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x60-0x6F */
    AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, AS, /* 0x70-0x7F */
    /*  1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, /* 0x80-0x8F */
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, /* 0x90-0x9F */
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, /* 0xA0-0xAF */
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, /* 0xB0-0xBF */
    XX, XX, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, /* 0xC0-0xCF */
    S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, S1, /* 0xD0-0xDF */
    S2, S3, S3, S3, S3, S3, S3, S3, S3, S3, S3, S3, S3, S4, S3, S3, /* 0xE0-0xEF */
    S5, S6, S6, S6, S7, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, /* 0xF0-0xFF */
};

/* What the second byte is allowed to be, given the first. The four ranges that
 * are not the default are what rejects surrogates and overlong encodings
 * without a second pass. Sixteen entries rather than five so that the index out
 * of the table above cannot be out of range. */
typedef struct AcceptRange {
    uint8_t lo, hi;
} AcceptRange;

/* clang-format off */
static const AcceptRange accept_ranges[16] = {
    {LOCB, HICB},  /* 0: the default, any continuation byte */
    {0xA0, HICB},  /* 1: after 0xE0, which rejects the overlong three byte forms */
    {LOCB, 0x9F},  /* 2: after 0xED, which rejects the surrogate range */
    {0x90, HICB},  /* 3: after 0xF0, which rejects the overlong four byte forms */
    {LOCB, 0x8F},  /* 4: after 0xF4, which rejects anything above U+10FFFF */
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0},
};
/* clang-format on */

/* Go writes the size through a second return value. Here it is an out parameter
 * the caller is allowed to throw away, which is the library's rule, so every
 * function that has a size to report goes through this. */
static Rune ret(Rune r, Int n, Int *size) {
    BURROW_OUT(size, n);
    return r;
}

/* ------------------------------------------------------------------ decoding */

static Rune decode(const Byte *p, Int n, Int *size) {
    uint8_t x, b1, b2, b3;
    Int sz;
    AcceptRange accept;

    if (n < 1)
        return ret(UTF8_RUNE_ERROR, 0, size);

    if (p[0] < (Byte)UTF8_RUNE_SELF)
        return ret((Rune)p[0], 1, size);

    x = first[p[0]];

    /* AS and XX are the two values with the high nibble set, so one compare
     * separates the one byte cases from everything else. Go folds the two into
     * a mask to avoid a branch here. Spelled out, because the branch predictor
     * has no trouble with this and the mask arithmetic is unreadable. */
    if (x >= AS)
        return ret(x == XX ? UTF8_RUNE_ERROR : (Rune)p[0], 1, size);

    sz = (Int)(x & 7);
    accept = accept_ranges[x >> 4];

    /* Everything from here down returns a width of one on failure rather than
     * the width the first byte claimed. That is deliberate in Go and it is what
     * makes a loop over corrupt input make progress without skipping bytes that
     * might be the start of something valid. */
    if (n < sz)
        return ret(UTF8_RUNE_ERROR, 1, size);

    b1 = p[1];
    if (b1 < accept.lo || accept.hi < b1)
        return ret(UTF8_RUNE_ERROR, 1, size);
    if (sz <= 2)
        return ret(((Rune)(p[0] & MASK2) << 6) | (Rune)(b1 & MASKX), 2, size);

    b2 = p[2];
    if (b2 < LOCB || HICB < b2)
        return ret(UTF8_RUNE_ERROR, 1, size);
    if (sz <= 3)
        return ret(((Rune)(p[0] & MASK3) << 12) | ((Rune)(b1 & MASKX) << 6) |
                       (Rune)(b2 & MASKX),
                   3, size);

    b3 = p[3];
    if (b3 < LOCB || HICB < b3)
        return ret(UTF8_RUNE_ERROR, 1, size);

    return ret(((Rune)(p[0] & MASK4) << 18) | ((Rune)(b1 & MASKX) << 12) |
                   ((Rune)(b2 & MASKX) << 6) | (Rune)(b3 & MASKX),
               4, size);
}

Rune utf8_decode_rune_in_string(Str s, Int *size) {
    return decode(s.p, s.len, size);
}

Rune utf8_decode_rune(Slice p, Int *size) {
    return decode((const Byte *)p.p, p.len, size);
}

/* The last rune, found by walking back at most UTF8_UTF_MAX bytes for something
 * that could be a starter and decoding forwards from there. The limit is what
 * keeps this O(1): without it, a string ending in a long run of continuation
 * bytes would make a backwards loop quadratic. */
static Rune decode_last(const Byte *p, Int n, Int *size) {
    Int start, lim, got = 0;
    Rune r;

    if (n == 0)
        return ret(UTF8_RUNE_ERROR, 0, size);

    start = n - 1;
    if (p[start] < (Byte)UTF8_RUNE_SELF)
        return ret((Rune)p[start], 1, size);

    lim = n - UTF8_UTF_MAX;
    if (lim < 0)
        lim = 0;

    for (start--; start >= lim; start--) {
        if (utf8_rune_start(p[start]))
            break;
    }
    if (start < 0)
        start = 0;

    r = decode(p + start, n - start, &got);

    /* The decode found a rune that does not reach the end of the input, which
     * means the bytes after it are not part of it and the last rune is
     * therefore broken. */
    if (start + got != n)
        return ret(UTF8_RUNE_ERROR, 1, size);

    return ret(r, got, size);
}

Rune utf8_decode_last_rune_in_string(Str s, Int *size) {
    return decode_last(s.p, s.len, size);
}

Rune utf8_decode_last_rune(Slice p, Int *size) {
    return decode_last((const Byte *)p.p, p.len, size);
}

static bool full_rune(const Byte *p, Int n) {
    uint8_t x;
    AcceptRange accept;

    if (n == 0)
        return false;

    x = first[p[0]];
    if (n >= (Int)(x & 7))
        return true; /* ASCII, invalid, or a complete sequence */

    /* Short. It is still a full rune if what is there is already wrong, since
     * more input cannot make an invalid sequence valid. */
    accept = accept_ranges[x >> 4];
    if (n > 1 && (p[1] < accept.lo || accept.hi < p[1]))
        return true;
    if (n > 2 && (p[2] < LOCB || HICB < p[2]))
        return true;

    return false;
}

bool utf8_full_rune_in_string(Str s) {
    return full_rune(s.p, s.len);
}

bool utf8_full_rune(Slice p) {
    return full_rune((const Byte *)p.p, p.len);
}

/* ------------------------------------------------------------------ encoding */

Int utf8_rune_len(Rune r) {
    if (r < 0)
        return -1;
    if (r <= RUNE1_MAX)
        return 1;
    if (r <= RUNE2_MAX)
        return 2;
    if (SURROGATE_MIN <= r && r <= SURROGATE_MAX)
        return -1;
    if (r <= RUNE3_MAX)
        return 3;
    if (r <= UTF8_MAX_RUNE)
        return 4;
    return -1;
}

/* Writes r into p, which has to be big enough, and answers how many bytes that
 * took. The unsigned copy of r is what makes the negative case fall through to
 * the error rune without a separate test, which is Go's trick and a good one. */
static Int encode(Byte *p, Rune r) {
    uint32_t i = (uint32_t)r;

    if (i <= RUNE1_MAX) {
        p[0] = (Byte)r;
        return 1;
    }
    if (i <= RUNE2_MAX) {
        p[0] = (Byte)(T2 | (Byte)(r >> 6));
        p[1] = (Byte)(TX | ((Byte)r & MASKX));
        return 2;
    }
    if (i < SURROGATE_MIN || (SURROGATE_MAX < i && i <= RUNE3_MAX)) {
        p[0] = (Byte)(T3 | (Byte)(r >> 12));
        p[1] = (Byte)(TX | ((Byte)(r >> 6) & MASKX));
        p[2] = (Byte)(TX | ((Byte)r & MASKX));
        return 3;
    }
    if (i > RUNE3_MAX && i <= (uint32_t)UTF8_MAX_RUNE) {
        p[0] = (Byte)(T4 | (Byte)(r >> 18));
        p[1] = (Byte)(TX | ((Byte)(r >> 12) & MASKX));
        p[2] = (Byte)(TX | ((Byte)(r >> 6) & MASKX));
        p[3] = (Byte)(TX | ((Byte)r & MASKX));
        return 4;
    }

    /* Out of range, or a surrogate half. Go writes the replacement character
     * rather than reporting anything, and every caller of this is in a loop
     * that would have nowhere to report it to. */
    p[0] = (Byte)(T3 | (UTF8_RUNE_ERROR >> 12));
    p[1] = (Byte)(TX | ((UTF8_RUNE_ERROR >> 6) & MASKX));
    p[2] = (Byte)(TX | (UTF8_RUNE_ERROR & MASKX));
    return 3;
}

Int utf8_encode_rune(Slice p, Rune r) {
    return encode((Byte *)p.p, r);
}

Slice utf8_append_rune(Alloc *a, Slice p, Rune r) {
    Byte buf[UTF8_UTF_MAX];
    Int n = encode(buf, r);

    /* Go's AppendRune(nil, r) works, and the burrow spelling of that nil is
     * slice_nil(TYPE_BYTE). A Slice that was only zeroed has no element type at
     * all, which appending to would silently do nothing, so fill it in. The
     * element type is not in question here: this function appends bytes. */
    if (p.elem == NULL)
        p = slice_nil(TYPE_BYTE);

    return slice_append(a, p, buf, n);
}

/* ------------------------------------------------------------------ counting */

static Int rune_count(const Byte *p, Int n) {
    Int i = 0, count = 0;

    while (i < n) {
        Int size = 1;

        count++;
        if (p[i] < (Byte)UTF8_RUNE_SELF) {
            i++;
            continue;
        }
        decode(p + i, n - i, &size);
        i += size;
    }

    return count;
}

Int utf8_rune_count_in_string(Str s) {
    return rune_count(s.p, s.len);
}

Int utf8_rune_count(Slice p) {
    return rune_count((const Byte *)p.p, p.len);
}

bool utf8_rune_start(Byte b) {
    return (b & 0xC0) != 0x80;
}

/* Valid is a decode loop with the rune thrown away, which is what Go's is once
 * the eight bytes at a time ASCII skip is taken out of it. That skip is worth
 * having and it is not here yet: it reads a word at a time and needs the
 * unaligned load and the endianness question answered first, both of which
 * belong in the platform layer rather than in a copy inside this file. There is
 * a benchmark waiting for it in burrow-bench. */
static bool valid(const Byte *p, Int n) {
    Int i = 0;

    while (i < n) {
        uint8_t x;
        Int size;
        AcceptRange accept;

        if (p[i] < (Byte)UTF8_RUNE_SELF) {
            i++;
            continue;
        }

        x = first[p[i]];
        size = (Int)(x & 7);
        accept = accept_ranges[x >> 4];

        switch (size) {
        case 2:
            if (n - i < 2 || p[i + 1] < accept.lo || accept.hi < p[i + 1])
                return false;
            break;
        case 3:
            if (n - i < 3 || p[i + 1] < accept.lo || accept.hi < p[i + 1] ||
                p[i + 2] < LOCB || HICB < p[i + 2])
                return false;
            break;
        case 4:
            if (n - i < 4 || p[i + 1] < accept.lo || accept.hi < p[i + 1] ||
                p[i + 2] < LOCB || HICB < p[i + 2] || p[i + 3] < LOCB ||
                HICB < p[i + 3])
                return false;
            break;
        default:
            return false; /* a starter byte that cannot start anything */
        }

        i += size;
    }

    return true;
}

bool utf8_valid_string(Str s) {
    return valid(s.p, s.len);
}

bool utf8_valid(Slice p) {
    return valid((const Byte *)p.p, p.len);
}

bool utf8_valid_rune(Rune r) {
    if (0 <= r && r < SURROGATE_MIN)
        return true;
    if (SURROGATE_MAX < r && r <= UTF8_MAX_RUNE)
        return true;
    return false;
}
