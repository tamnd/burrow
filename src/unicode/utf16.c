/* Derived from Go's src/unicode/utf16/utf16.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/unicode/utf16.h"
#include "burrow/core.h"
#include "burrow/slice.h"
#include "burrow/type.h"

enum {
    replacement_char = 0xFFFD, /* Unicode replacement character */
    max_rune = 0x10FFFF,       /* Maximum valid Unicode code point. */
};

/* 0xd800-0xdc00 encodes the high 10 bits of a pair.
 * 0xdc00-0xe000 encodes the low 10 bits of a pair.
 * the value is those 20 bits plus 0x10000. */
enum {
    surr1 = 0xd800,
    surr2 = 0xdc00,
    surr3 = 0xe000,
    surr_self = 0x10000,
};

bool utf16_is_surrogate(Rune r) {
    return surr1 <= r && r < surr3;
}

Rune utf16_decode_rune(Rune r1, Rune r2) {
    if (surr1 <= r1 && r1 < surr2 && surr2 <= r2 && r2 < surr3)
        return (((r1 - surr1) << 10) | (r2 - surr2)) + surr_self;
    return replacement_char;
}

Rune utf16_encode_rune(Rune r, Rune *r2) {
    if (r < surr_self || r > max_rune) {
        BURROW_OUT(r2, replacement_char);
        return replacement_char;
    }
    r -= surr_self;
    BURROW_OUT(r2, surr2 + (r & 0x3ff));
    return surr1 + ((r >> 10) & 0x3ff);
}

Int utf16_rune_len(Rune r) {
    if ((0 <= r && r < surr1) || (surr3 <= r && r < surr_self))
        return 1;
    if (surr_self <= r && r <= max_rune)
        return 2;
    return -1;
}

/* Writes the encoding of r to out, which has room for two units, and returns
 * how many it wrote. Encode and AppendRune share this. */
static Int encode_units(uint16_t *out, Rune r) {
    switch (utf16_rune_len(r)) {
    case 1: /* normal rune */
        out[0] = (uint16_t)r;
        return 1;
    case 2: { /* needs surrogate sequence */
        Rune r2;
        Rune r1 = utf16_encode_rune(r, &r2);
        out[0] = (uint16_t)r1;
        out[1] = (uint16_t)r2;
        return 2;
    }
    default:
        out[0] = replacement_char;
        return 1;
    }
}

Slice utf16_encode(Alloc *a, Slice s) {
    const Rune *in = s.p;
    Int n = s.len;
    for (Int i = 0; i < s.len; i++) {
        if (in[i] >= surr_self)
            n++;
    }

    Slice out = slice_make(a, TYPE_UINT16, n, n);
    if (out.p == NULL && n > 0)
        return out;
    uint16_t *u = out.p;
    n = 0;
    for (Int i = 0; i < s.len; i++)
        n += encode_units(u + n, in[i]);
    out.len = n;
    return out;
}

Slice utf16_append_rune(Alloc *a, Slice p, Rune r) {
    uint16_t buf[2];
    Int n = encode_units(buf, r);

    /* Go inlines this function and the append in it, so appending into spare
     * capacity costs a store. Writing the units directly when they fit is the
     * nearest thing here, and slice_append takes everything else, including
     * the growth and a slice of the wrong element type. */
    if (p.elem == TYPE_UINT16 && p.p != NULL && p.len <= p.cap - n) {
        uint16_t *u = (uint16_t *)p.p + p.len;
        u[0] = buf[0];
        if (n == 2)
            u[1] = buf[1];
        p.len += n;
        return p;
    }
    if (p.elem == NULL)
        p = slice_nil(TYPE_UINT16);
    return slice_append(a, p, buf, n);
}

Slice utf16_decode(Alloc *a, Slice s) {
    const uint16_t *in = s.p;

    /* Every unit becomes at most one rune, so one allocation of s.len is
     * enough. Go starts from a 64 rune buffer on the stack instead, because
     * its escape analysis can keep that off the heap. An empty result is
     * still not nil, as in Go, since slice_make never gives nil for a zero
     * capacity. */
    Slice out = slice_make(a, TYPE_RUNE, 0, s.len);
    if (out.p == NULL)
        return out;
    Rune *buf = out.p;
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        Rune ar;
        Rune r = in[i];
        if (r < surr1 || surr3 <= r) {
            /* normal rune */
            ar = r;
        } else if (surr1 <= r && r < surr2 && i + 1 < s.len && surr2 <= in[i + 1] &&
                   in[i + 1] < surr3) {
            /* valid surrogate sequence */
            ar = utf16_decode_rune(r, in[i + 1]);
            i++;
        } else {
            /* invalid surrogate sequence */
            ar = replacement_char;
        }
        buf[n++] = ar;
    }
    out.len = n;
    return out;
}
