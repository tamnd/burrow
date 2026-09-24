/* Derived from Go's src/encoding/base32/base32.go.
 * Go source: go1.27.1.
 *
 * A port, with two changes that do not show from outside. The two standard
 * encodings are static tables rather than built when the package starts, and
 * the Encoding values that Go hands out as pointers are returned by value.
 * Decoding walks past newlines where Go copies the input without them first,
 * so Decode does not allocate, and it takes whole blocks of eight characters
 * at once when it can. The offsets in errors are still counted without the
 * newlines, as Go counts them.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/base32.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

/* -------------------------------------------------------------- encodings */

/* clang-format off */
#define BASE32_STD_DECODE_MAP \
    { \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, \
        0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
    }

#define BASE32_HEX_DECODE_MAP \
    { \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, \
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
    }

#define BASE32_STD_ALPHABET \
    { \
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', \
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '2', '3', '4', '5', '6', '7', \
    }

#define BASE32_HEX_ALPHABET \
    { \
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', \
        'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', \
    }
/* clang-format on */

static const Base32Encoding base32_std = {BASE32_STD_ALPHABET, BASE32_STD_DECODE_MAP,
                                          BASE32_STD_PADDING};
static const Base32Encoding base32_hex = {BASE32_HEX_ALPHABET, BASE32_HEX_DECODE_MAP,
                                          BASE32_STD_PADDING};

const Base32Encoding *const base32_std_encoding = &base32_std;
const Base32Encoding *const base32_hex_encoding = &base32_hex;

#define BASE32_INVALID 0xff

Base32Encoding base32_new_encoding(Str encoder) {
    if (encoder.len != 32)
        panic_str(BURROW_S("encoding alphabet is not 32-bytes long"));

    Base32Encoding e;
    e.pad_char = BASE32_STD_PADDING;
    memcpy(e.encode, encoder.p, 32);
    memset(e.decode_map, BASE32_INVALID, sizeof e.decode_map);

    for (int i = 0; i < 32; i++) {
        /* The padding character is not checked against the alphabet here,
         * since the caller may be about to change it, which is Go's reason. */
        Byte c = encoder.p[i];
        if (c == '\n' || c == '\r')
            panic_str(BURROW_S("encoding alphabet contains newline character"));
        if (e.decode_map[c] != BASE32_INVALID)
            panic_str(BURROW_S("encoding alphabet includes duplicate symbols"));
        e.decode_map[c] = (Byte)i;
    }
    return e;
}

Base32Encoding base32_encoding_with_padding(const Base32Encoding *enc, Rune padding) {
    if (padding < BASE32_NO_PADDING || padding == '\r' || padding == '\n' ||
        padding > 0xff)
        panic_str(BURROW_S("invalid padding"));
    if (padding != BASE32_NO_PADDING &&
        enc->decode_map[(Byte)padding] != BASE32_INVALID)
        panic_str(BURROW_S("padding contained in alphabet"));
    Base32Encoding e = *enc;
    e.pad_char = padding;
    return e;
}

/* ------------------------------------------------------------------ errors */

/* The offset first, so the data pointer of the Error is a pointer to a
 * Base32CorruptInputError and errors_as hands it straight back. */
typedef struct Base32CorruptBox {
    Base32CorruptInputError off;
    Str message;
} Base32CorruptBox;

static const Type base32_corrupt_desc = {
    {(const Byte *)"CorruptInputError", 17},
    {(const Byte *)"encoding/base32", 15},
    KIND_INT64,
    (uint32_t)sizeof(Base32CorruptInputError),
    (uint16_t)_Alignof(Base32CorruptInputError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x62336369U, /* "b3ci" */
    NULL,
};

const Type *const TYPE_BASE32_CORRUPT_INPUT_ERROR = &base32_corrupt_desc;

static const char base32_corrupt_prefix[] = "illegal base32 data at input byte ";

#define BASE32_PREFIX_LEN ((Int)sizeof(base32_corrupt_prefix) - 1)

/* Writes the message for e to p, or only counts it when p is NULL, and
 * returns its length. */
static Int base32_corrupt_message(Byte *p, Base32CorruptInputError e) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = e < 0 ? (uint64_t)0 - (uint64_t)e : (uint64_t)e;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = BASE32_PREFIX_LEN + (e < 0) + n;
    if (p != NULL) {
        memcpy(p, base32_corrupt_prefix, (size_t)BASE32_PREFIX_LEN);
        p += BASE32_PREFIX_LEN;
        if (e < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str base32_corrupt_text(const void *self) {
    return ((const Base32CorruptBox *)self)->message;
}

static Error base32_corrupt_clone(const void *self, Alloc *a) {
    return base32_corrupt_input_error_as_error(((const Base32CorruptBox *)self)->off,
                                               a);
}

static const ErrorVT base32_corrupt_vt = {
    .self_type = &base32_corrupt_desc,
    .message = base32_corrupt_text,
    .clone = base32_corrupt_clone,
};

Error base32_corrupt_input_error_as_error(Base32CorruptInputError e, Alloc *a) {
    Int mlen = base32_corrupt_message(NULL, e);
    Base32CorruptBox *b = (Base32CorruptBox *)mem_alloc_nozero(
        a, sizeof(Base32CorruptBox) + (size_t)mlen, _Alignof(Base32CorruptBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    base32_corrupt_message(p, e);
    b->off = e;
    b->message = str_from_bytes(p, mlen);
    return (Error){&base32_corrupt_vt, b};
}

Str base32_corrupt_input_error_error(Base32CorruptInputError e, Alloc *a) {
    Int mlen = base32_corrupt_message(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    base32_corrupt_message(p, e);
    return str_from_bytes(p, mlen);
}

/* What decoding reports, in the error arena. */
static Error base32_corrupt(Int off) {
    return base32_corrupt_input_error_as_error((int64_t)off, error_allocator());
}

/* ----------------------------------------------------------------- lengths */

Int base32_encoding_encoded_len(const Base32Encoding *enc, Int n) {
    /* Go's int wraps on overflow and C's may not, so the arithmetic that can
     * overflow is done unsigned to give Go's answer. */
    if (enc->pad_char == BASE32_NO_PADDING) /* the fewest 5 bit characters */
        return (Int)((uint64_t)(n / 5) * 8 + (uint64_t)((n % 5 * 8 + 4) / 5));
    Int m = (Int)((uint64_t)n + 4) / 5; /* the fewest 8 character blocks */
    return (Int)((uint64_t)m * 8);
}

static Int base32_decoded_len(Int n, Rune pad_char) {
    if (pad_char == BASE32_NO_PADDING)
        return n / 8 * 5 + n % 8 * 5 / 8; /* may end part way through a block */
    return n / 8 * 5;                     /* always a multiple of 8 */
}

Int base32_encoding_decoded_len(const Base32Encoding *enc, Int n) {
    return base32_decoded_len(n, enc->pad_char);
}

/* ------------------------------------------------------------------ encode */

/* binary.BigEndian.PutUint32, as one store. */
static inline void base32_put_be32(Byte *p, uint32_t v) {
#if BURROW_LITTLE_ENDIAN
    v = bits_reverse_bytes32(v);
#endif
    memcpy(p, &v, 4);
}

static inline void base32_put_be64(Byte *p, uint64_t v) {
#if BURROW_LITTLE_ENDIAN
    v = bits_reverse_bytes64(v);
#endif
    memcpy(p, &v, 8);
}

/* Encodes n bytes of src into dst, which has room for all of it. */
static void base32_encode_raw(const Base32Encoding *enc, Byte *dst, const Byte *src,
                              Int n) {
    const Byte *e = enc->encode;
    while (n >= 5) {
        uint64_t v = (uint64_t)src[0] << 32 | (uint64_t)src[1] << 24 |
                     (uint64_t)src[2] << 16 | (uint64_t)src[3] << 8 | src[4];
        uint64_t out =
            (uint64_t)e[v >> 35 & 0x1f] << 56 | (uint64_t)e[v >> 30 & 0x1f] << 48 |
            (uint64_t)e[v >> 25 & 0x1f] << 40 | (uint64_t)e[v >> 20 & 0x1f] << 32 |
            (uint64_t)e[v >> 15 & 0x1f] << 24 | (uint64_t)e[v >> 10 & 0x1f] << 16 |
            (uint64_t)e[v >> 5 & 0x1f] << 8 | (uint64_t)e[v & 0x1f];
        base32_put_be64(dst, out);
        src += 5;
        dst += 8;
        n -= 5;
    }
    if (n == 0)
        return;

    uint32_t val = 0;
    switch (n) {
    case 4:
        val |= (uint32_t)src[3];
        dst[6] = e[val << 3 & 0x1f];
        dst[5] = e[val >> 2 & 0x1f];
        /* fall through */
    case 3:
        val |= (uint32_t)src[2] << 8;
        dst[4] = e[val >> 7 & 0x1f];
        /* fall through */
    case 2:
        val |= (uint32_t)src[1] << 16;
        dst[3] = e[val >> 12 & 0x1f];
        dst[2] = e[val >> 17 & 0x1f];
        /* fall through */
    default: /* 1 */
        val |= (uint32_t)src[0] << 24;
        dst[1] = e[val >> 22 & 0x1f];
        dst[0] = e[val >> 27 & 0x1f];
    }

    if (enc->pad_char != BASE32_NO_PADDING) {
        for (Int i = n * 8 / 5 + 1; i < 8; i++)
            dst[i] = (Byte)enc->pad_char;
    }
}

void base32_encoding_encode(const Base32Encoding *enc, Slice dst, Slice src) {
    if (src.len == 0)
        return;
    Int need = base32_encoding_encoded_len(enc, src.len);
    if (dst.len < need) {
        /* Where Go's writes run off the end: the check of dst[7] in the block
         * that does not fit, the first write of the tail, which is its
         * highest, or the first padding byte past the end. */
        Int full = src.len / 5;
        if (dst.len < full * 8)
            runtime_index_out_of_range(7, dst.len % 8);
        Int left = dst.len - full * 8;
        static const Int first[5] = {0, 1, 3, 4, 6};
        Int f = first[src.len % 5];
        if (left <= f)
            runtime_index_out_of_range(f, left);
        runtime_index_out_of_range(left, left);
    }
    base32_encode_raw(enc, (Byte *)dst.p, (const Byte *)src.p, src.len);
}

/* Grows dst by n and returns where the new bytes start, or NULL when it could
 * not. A zero Slice is Go's nil []byte. */
static Byte *base32_grow(Alloc *a, Slice *dst, Int n) {
    if (dst->elem == NULL)
        *dst = slice_nil(TYPE_BYTE);
    Int old = dst->len;
    *dst = slice_append(a, *dst, NULL, n);
    return dst->p == NULL ? NULL : (Byte *)dst->p + old;
}

Slice base32_encoding_append_encode(const Base32Encoding *enc, Alloc *a, Slice dst,
                                    Slice src) {
    Int n = base32_encoding_encoded_len(enc, src.len);
    Byte *p = base32_grow(a, &dst, n);
    if (p != NULL)
        base32_encode_raw(enc, p, (const Byte *)src.p, src.len);
    return dst;
}

Str base32_encoding_encode_to_string(const Base32Encoding *enc, Alloc *a, Slice src) {
    Int n = base32_encoding_encoded_len(enc, src.len);
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    base32_encode_raw(enc, p, (const Byte *)src.p, src.len);
    return str_from_bytes(p, n);
}

/* ------------------------------------------------------------------ decode */

static inline bool base32_is_newline(Byte b) {
    return b == '\r' || b == '\n';
}

/* Go's decode, run over src with the newlines in it skipped as it goes. left
 * is Go's len(src), the count of characters other than newlines still to
 * read, and si is where the next one is found, maybe after some newlines.
 * Sets *endp when it saw the end of the message, which is padding, or the end
 * of the input with no padding expected. */
static Int base32_decode(const Base32Encoding *enc, Byte *dst, Int cap, const Byte *src,
                         Int len, bool *endp, Error *err) {
    Int olen = len;
    for (Int i = 0; i < len; i++)
        olen -= base32_is_newline(src[i]);

    const Byte *m = enc->decode_map;
    /* Go compares each input byte with byte(padChar), which for no padding is
     * 0xff. That only matters when 0xff is in the alphabet, and when it is not,
     * no valid character can be taken for padding. */
    Byte pad = (Byte)enc->pad_char;
    bool pad_safe = m[pad] == BASE32_INVALID;
    Int left = olen, si = 0, dsti = 0, n = 0;
    bool end = false;
    Error e = BURROW_NO_ERROR;

    while (left > 0 && !end) {
        /* A whole block with no newline, no bad character and no chance of
         * padding: eight characters to five bytes in one go. */
        if (left >= 8 && (pad_safe || left >= 16) && cap - dsti >= 5 && si + 8 <= len) {
            const Byte *s = src + si;
            Byte d0 = m[s[0]], d1 = m[s[1]], d2 = m[s[2]], d3 = m[s[3]];
            Byte d4 = m[s[4]], d5 = m[s[5]], d6 = m[s[6]], d7 = m[s[7]];
            if ((d0 | d1 | d2 | d3 | d4 | d5 | d6 | d7) != 0xff) {
                uint64_t v = (uint64_t)d0 << 35 | (uint64_t)d1 << 30 |
                             (uint64_t)d2 << 25 | (uint64_t)d3 << 20 |
                             (uint64_t)d4 << 15 | (uint64_t)d5 << 10 |
                             (uint64_t)d6 << 5 | (uint64_t)d7;
                base32_put_be32(dst + dsti, (uint32_t)(v >> 8));
                dst[dsti + 4] = (Byte)v;
                n += 5;
                dsti += 5;
                si += 8;
                left -= 8;
                continue;
            }
        }

        Byte dbuf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        Int dlen = 8;

        for (Int j = 0; j < 8;) {
            if (left == 0) {
                if (enc->pad_char != BASE32_NO_PADDING) {
                    /* Not enough characters for a whole block. */
                    e = base32_corrupt(olen - left - j);
                    goto out;
                }
                /* The end, with no padding expected. */
                dlen = j;
                end = true;
                break;
            }
            while (base32_is_newline(src[si]))
                si++;
            Byte in = src[si++];
            left--;

            if (in == pad && j >= 2 && left < 8) {
                /* The padding at the end: the rest has to be padding too. */
                if (left + j < 8 - 1) {
                    e = base32_corrupt(olen);
                    goto out;
                }
                Int pi = si;
                for (Int k = 0; k < 8 - 1 - j && k < left; k++) {
                    while (base32_is_newline(src[pi]))
                        pi++;
                    if (src[pi++] != pad) {
                        e = base32_corrupt(olen - left + k - 1);
                        goto out;
                    }
                }
                dlen = j;
                end = true;
                /* 7, 5 and 2 are all multiples of 8 bits, so the rest are not
                 * a whole number of bytes. */
                if (dlen == 1 || dlen == 3 || dlen == 6) {
                    e = base32_corrupt(olen - left - 1);
                    goto out;
                }
                break;
            }
            dbuf[j] = m[in];
            if (dbuf[j] == BASE32_INVALID) {
                e = base32_corrupt(olen - left - 1);
                goto out;
            }
            j++;
        }

        /* Eight 5 bit values into five bytes, highest index first as Go
         * writes them, so a short dst panics where Go's does. */
        Int top;
        switch (dlen) {
        case 8:
            top = 4;
            break;
        case 7:
            top = 3;
            break;
        case 5:
            top = 2;
            break;
        case 4:
            top = 1;
            break;
        case 2:
            top = 0;
            break;
        default:
            top = -1;
            break;
        }
        if (top >= 0 && dsti + top >= cap)
            runtime_index_out_of_range(dsti + top, cap);
        switch (dlen) {
        case 8:
            dst[dsti + 4] = (Byte)(dbuf[6] << 5 | dbuf[7]);
            n++;
            /* fall through */
        case 7:
            dst[dsti + 3] = (Byte)(dbuf[4] << 7 | dbuf[5] << 2 | dbuf[6] >> 3);
            n++;
            /* fall through */
        case 5:
            dst[dsti + 2] = (Byte)(dbuf[3] << 4 | dbuf[4] >> 1);
            n++;
            /* fall through */
        case 4:
            dst[dsti + 1] = (Byte)(dbuf[1] << 6 | dbuf[2] << 1 | dbuf[3] >> 4);
            n++;
            /* fall through */
        case 2:
            dst[dsti + 0] = (Byte)(dbuf[0] << 3 | dbuf[1] >> 2);
            n++;
            break;
        default:
            break;
        }
        dsti += 5;
    }
out:
    if (BURROW_FAILED(e))
        end = false;
    *endp = end;
    BURROW_OUT(err, e);
    return n;
}

Int base32_encoding_decode(const Base32Encoding *enc, Slice dst, Slice src,
                           Error *err) {
    bool end;
    return base32_decode(enc, (Byte *)dst.p, dst.len, (const Byte *)src.p, src.len,
                         &end, err);
}

Slice base32_encoding_append_decode(const Base32Encoding *enc, Alloc *a, Slice dst,
                                    Slice src, Error *err) {
    /* The size without the padding, so as not to ask for more than needed. */
    const Byte *s = (const Byte *)src.p;
    Int n = src.len;
    while (n > 0 && (Rune)s[n - 1] == enc->pad_char)
        n--;
    n = base32_decoded_len(n, BASE32_NO_PADDING);

    Int old = dst.len;
    Byte *p = base32_grow(a, &dst, n);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    bool end;
    Int got = base32_decode(enc, p, n, s, src.len, &end, err);
    dst.len = old + got;
    return dst;
}

Slice base32_encoding_decode_string(const Base32Encoding *enc, Alloc *a, Str s,
                                    Error *err) {
    /* Go decodes in place, into a copy of s. Every eight characters give at
     * most five bytes, and the last few at most what a raw decode gives. */
    Int n = base32_decoded_len(s.len, BASE32_NO_PADDING);
    Slice dst = slice_make(a, TYPE_BYTE, n, n);
    if (n > 0 && dst.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    bool end;
    dst.len = base32_decode(enc, (Byte *)dst.p, n, s.p, s.len, &end, err);
    return dst;
}

/* --------------------------------------------------------------- streaming */

#define BASE32_OUT_SIZE 1024

typedef struct Base32Encoder {
    Error err;
    const Base32Encoding *enc;
    IoWriter w;
    Byte buf[5]; /* input waiting for a whole block */
    Int nbuf;
    Byte out[BASE32_OUT_SIZE];
} Base32Encoder;

static Error base32_put(IoWriter w, Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_from(p, n, n, TYPE_BYTE), &err);
    return err;
}

static Int base32_encoder_write(void *self, Slice p, Error *err) {
    Base32Encoder *e = (Base32Encoder *)self;
    if (BURROW_FAILED(e->err)) {
        BURROW_OUT(err, e->err);
        return 0;
    }
    const Byte *src = (const Byte *)p.p;
    Int left = p.len, n = 0;

    /* Top up a block started by the last write. */
    if (e->nbuf > 0) {
        Int i;
        for (i = 0; i < left && e->nbuf < 5; i++)
            e->buf[e->nbuf++] = src[i];
        n += i;
        src += i;
        left -= i;
        if (e->nbuf < 5) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
        base32_encode_raw(e->enc, e->out, e->buf, 5);
        e->err = base32_put(e->w, e->out, 8);
        if (BURROW_FAILED(e->err)) {
            BURROW_OUT(err, e->err);
            return n;
        }
        e->nbuf = 0;
    }

    /* Whole blocks, a buffer at a time. */
    while (left >= 5) {
        Int nn = BASE32_OUT_SIZE / 8 * 5;
        if (nn > left) {
            nn = left;
            nn -= nn % 5;
        }
        base32_encode_raw(e->enc, e->out, src, nn);
        e->err = base32_put(e->w, e->out, nn / 5 * 8);
        if (BURROW_FAILED(e->err)) {
            BURROW_OUT(err, e->err);
            return n;
        }
        n += nn;
        src += nn;
        left -= nn;
    }

    /* Keep what is left for next time. */
    for (Int i = 0; i < left; i++)
        e->buf[i] = src[i];
    e->nbuf = left;
    n += left;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

static Error base32_encoder_close(void *self) {
    Base32Encoder *e = (Base32Encoder *)self;
    if (BURROW_OK(e->err) && e->nbuf > 0) {
        base32_encode_raw(e->enc, e->out, e->buf, e->nbuf);
        Int n = base32_encoding_encoded_len(e->enc, e->nbuf);
        e->nbuf = 0;
        e->err = base32_put(e->w, e->out, n);
    }
    return e->err;
}

static const IoWriteCloserVT base32_encoder_vt = {
    {NULL, base32_encoder_write},
    {NULL, base32_encoder_close},
};

IoWriteCloser base32_new_encoder(Alloc *a, const Base32Encoding *enc, IoWriter w) {
    Base32Encoder *e = BURROW_NEW(a, Base32Encoder);
    if (e == NULL)
        return (IoWriteCloser){NULL, NULL};
    e->enc = enc;
    e->w = w;
    return (IoWriteCloser){&base32_encoder_vt, e};
}

typedef struct Base32Decoder {
    Error err;
    const Base32Encoding *enc;
    IoReader r;
    bool end;       /* saw the end of the message */
    Byte buf[1024]; /* input not yet decoded */
    Int nbuf;
    Int out_off, out_len; /* decoded output not yet read, in outbuf */
    Byte outbuf[1024 / 8 * 5];
} Base32Decoder;

static bool base32_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

/* r.Read with the newlines taken out, reading again when a read was nothing
 * but newlines and came with no error. */
static Int base32_read_filtered(IoReader r, Byte *p, Int len, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = r.vt->read(r.data, slice_from(p, len, len, TYPE_BYTE), &e);
    while (n > 0) {
        Int off = 0;
        for (Int i = 0; i < n; i++) {
            Byte b = p[i];
            if (!base32_is_newline(b)) {
                if (i != off)
                    p[off] = b;
                off++;
            }
        }
        if (BURROW_FAILED(e) || off > 0) {
            BURROW_OUT(err, e);
            return off;
        }
        e = BURROW_NO_ERROR;
        n = r.vt->read(r.data, slice_from(p, len, len, TYPE_BYTE), &e);
    }
    BURROW_OUT(err, e);
    return n;
}

/* Reads until there are at least min bytes or an error, as Go's
 * readEncodedData does. */
static Int base32_read_encoded(IoReader r, Byte *p, Int len, Int min,
                               bool expects_padding, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    while (n < min && BURROW_OK(e))
        n += base32_read_filtered(r, p + n, len - n, &e);
    if (n < min && n > 0 && base32_is_eof(e))
        e = io_err_unexpected_eof;
    if (expects_padding && min < 8 && n == 0 && base32_is_eof(e))
        e = io_err_unexpected_eof;
    *err = e;
    return n;
}

static Int base32_take_out(Base32Decoder *d, Slice p) {
    Int n = p.len < d->out_len ? p.len : d->out_len;
    if (n > 0)
        memcpy(p.p, d->outbuf + d->out_off, (size_t)n);
    d->out_off += n;
    d->out_len -= n;
    return n;
}

static Int base32_decoder_read(void *self, Slice p, Error *err) {
    Base32Decoder *d = (Base32Decoder *)self;

    /* Output decoded last time and not yet read. */
    if (d->out_len > 0) {
        Int n = base32_take_out(d, p);
        BURROW_OUT(err, d->out_len == 0 ? d->err : BURROW_NO_ERROR);
        return n;
    }

    if (BURROW_FAILED(d->err)) {
        BURROW_OUT(err, d->err);
        return 0;
    }

    /* Read a whole number of blocks, at least one. */
    Int nn = (p.len + 4) / 5 * 8;
    if (nn < 8)
        nn = 8;
    if (nn > (Int)sizeof d->buf)
        nn = (Int)sizeof d->buf;

    Int min;
    bool expects_padding;
    if (d->enc->pad_char == BASE32_NO_PADDING) {
        min = 1;
        expects_padding = false;
    } else {
        min = 8 - d->nbuf;
        expects_padding = true;
    }

    nn = base32_read_encoded(d->r, d->buf + d->nbuf, nn - d->nbuf, min, expects_padding,
                             &d->err);
    d->nbuf += nn;
    if (d->nbuf < min) {
        BURROW_OUT(err, d->err);
        return 0;
    }
    if (nn > 0 && d->end) {
        /* More input after the padding. */
        BURROW_OUT(err, base32_corrupt(0));
        return 0;
    }

    /* Decode the whole blocks, or all of it without padding. */
    Int nr = d->enc->pad_char == BASE32_NO_PADDING ? d->nbuf : d->nbuf / 8 * 8;
    Int nw = base32_encoding_decoded_len(d->enc, d->nbuf);
    Int n;
    Error derr = BURROW_NO_ERROR;
    if (nw > p.len) {
        nw = base32_decode(d->enc, d->outbuf, (Int)sizeof d->outbuf, d->buf, nr,
                           &d->end, &derr);
        d->out_off = 0;
        d->out_len = nw;
        n = base32_take_out(d, p);
    } else {
        n = base32_decode(d->enc, (Byte *)p.p, p.len, d->buf, nr, &d->end, &derr);
    }
    d->nbuf -= nr;
    memmove(d->buf, d->buf + nr, (size_t)d->nbuf);

    if (BURROW_FAILED(derr) && (BURROW_OK(d->err) || base32_is_eof(d->err)))
        d->err = derr;

    if (d->out_len > 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return n;
    }
    BURROW_OUT(err, d->err);
    return n;
}

static const IoReaderVT base32_decoder_vt = {NULL, base32_decoder_read};

IoReader base32_new_decoder(Alloc *a, const Base32Encoding *enc, IoReader r) {
    Base32Decoder *d = BURROW_NEW(a, Base32Decoder);
    if (d == NULL)
        return (IoReader){NULL, NULL};
    d->enc = enc;
    d->r = r;
    return (IoReader){&base32_decoder_vt, d};
}
