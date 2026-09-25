/* Derived from Go's src/encoding/base64/base64.go.
 * Go source: go1.27.1.
 *
 * A straight port. The four standard encodings are static tables here rather
 * than built when the package starts, and the Encoding values that Go hands
 * out as pointers are returned by value, since they hold no pointers and
 * nothing would free them.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/base64.h"

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
#define BASE64_STD_DECODE_MAP \
    { \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, 0xff, 0x3f, \
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, \
        0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, \
        0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
    }

#define BASE64_URL_DECODE_MAP \
    { \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, \
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, \
        0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0x3f, \
        0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, \
        0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, \
    }

#define BASE64_STD_ALPHABET \
    { \
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', \
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', \
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', \
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', \
    }

#define BASE64_URL_ALPHABET \
    { \
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', \
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', \
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', \
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_', \
    }
/* clang-format on */

static const Base64Encoding base64_std = {BASE64_STD_ALPHABET, BASE64_STD_DECODE_MAP,
                                          BASE64_STD_PADDING, false};
static const Base64Encoding base64_url = {BASE64_URL_ALPHABET, BASE64_URL_DECODE_MAP,
                                          BASE64_STD_PADDING, false};
static const Base64Encoding base64_raw_std = {
    BASE64_STD_ALPHABET, BASE64_STD_DECODE_MAP, BASE64_NO_PADDING, false};
static const Base64Encoding base64_raw_url = {
    BASE64_URL_ALPHABET, BASE64_URL_DECODE_MAP, BASE64_NO_PADDING, false};

const Base64Encoding *const base64_std_encoding = &base64_std;
const Base64Encoding *const base64_url_encoding = &base64_url;
const Base64Encoding *const base64_raw_std_encoding = &base64_raw_std;
const Base64Encoding *const base64_raw_url_encoding = &base64_raw_url;

#define BASE64_INVALID 0xff

Base64Encoding base64_new_encoding(Str encoder) {
    if (encoder.len != 64)
        panic_str(BURROW_S("encoding alphabet is not 64-bytes long"));

    Base64Encoding e;
    e.pad_char = BASE64_STD_PADDING;
    e.strict = false;
    memcpy(e.encode, encoder.p, 64);
    memset(e.decode_map, BASE64_INVALID, sizeof e.decode_map);

    for (int i = 0; i < 64; i++) {
        /* The padding character is not checked against the alphabet here,
         * since the caller may be about to change it, which is Go's reason. */
        Byte c = encoder.p[i];
        if (c == '\n' || c == '\r')
            panic_str(BURROW_S("encoding alphabet contains newline character"));
        if (e.decode_map[c] != BASE64_INVALID)
            panic_str(BURROW_S("encoding alphabet includes duplicate symbols"));
        e.decode_map[c] = (Byte)i;
    }
    return e;
}

Base64Encoding base64_encoding_with_padding(const Base64Encoding *enc, Rune padding) {
    if (padding < BASE64_NO_PADDING || padding == '\r' || padding == '\n' ||
        padding > 0xff)
        panic_str(BURROW_S("invalid padding"));
    if (padding != BASE64_NO_PADDING &&
        enc->decode_map[(Byte)padding] != BASE64_INVALID)
        panic_str(BURROW_S("padding contained in alphabet"));
    Base64Encoding e = *enc;
    e.pad_char = padding;
    return e;
}

Base64Encoding base64_encoding_strict(const Base64Encoding *enc) {
    Base64Encoding e = *enc;
    e.strict = true;
    return e;
}

/* ------------------------------------------------------------------ errors */

/* The offset first, so the data pointer of the Error is a pointer to a
 * Base64CorruptInputError and errors_as hands it straight back. */
typedef struct Base64CorruptBox {
    Base64CorruptInputError off;
    Str message;
} Base64CorruptBox;

static const Type base64_corrupt_desc = {
    {(const Byte *)"CorruptInputError", 17},
    {(const Byte *)"encoding/base64", 15},
    KIND_INT64,
    (uint32_t)sizeof(Base64CorruptInputError),
    (uint16_t)_Alignof(Base64CorruptInputError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x62366369U, /* "b6ci" */
    NULL,
};

const Type *const TYPE_BASE64_CORRUPT_INPUT_ERROR = &base64_corrupt_desc;

static const char base64_corrupt_prefix[] = "illegal base64 data at input byte ";

#define BASE64_PREFIX_LEN ((Int)sizeof(base64_corrupt_prefix) - 1)

/* Writes the message for e to p, or only counts it when p is NULL, and
 * returns its length. */
static Int base64_corrupt_message(Byte *p, Base64CorruptInputError e) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = e < 0 ? (uint64_t)0 - (uint64_t)e : (uint64_t)e;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = BASE64_PREFIX_LEN + (e < 0) + n;
    if (p != NULL) {
        memcpy(p, base64_corrupt_prefix, (size_t)BASE64_PREFIX_LEN);
        p += BASE64_PREFIX_LEN;
        if (e < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str base64_corrupt_text(const void *self) {
    return ((const Base64CorruptBox *)self)->message;
}

static Error base64_corrupt_clone(const void *self, Alloc *a) {
    return base64_corrupt_input_error_as_error(((const Base64CorruptBox *)self)->off,
                                               a);
}

static const ErrorVT base64_corrupt_vt = {
    .self_type = &base64_corrupt_desc,
    .message = base64_corrupt_text,
    .clone = base64_corrupt_clone,
};

Error base64_corrupt_input_error_as_error(Base64CorruptInputError e, Alloc *a) {
    Int mlen = base64_corrupt_message(NULL, e);
    Base64CorruptBox *b = (Base64CorruptBox *)mem_alloc_nozero(
        a, sizeof(Base64CorruptBox) + (size_t)mlen, _Alignof(Base64CorruptBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    base64_corrupt_message(p, e);
    b->off = e;
    b->message = str_from_bytes(p, mlen);
    return (Error){&base64_corrupt_vt, b};
}

Str base64_corrupt_input_error_error(Base64CorruptInputError e, Alloc *a) {
    Int mlen = base64_corrupt_message(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    base64_corrupt_message(p, e);
    return str_from_bytes(p, mlen);
}

/* What decoding reports, in the error arena. */
static Error base64_corrupt(Int off) {
    return base64_corrupt_input_error_as_error((int64_t)off, error_allocator());
}

/* ----------------------------------------------------------------- lengths */

Int base64_encoding_encoded_len(const Base64Encoding *enc, Int n) {
    /* Go's int wraps on overflow and C's may not, so the arithmetic that can
     * overflow is done unsigned to give Go's answer. */
    if (enc->pad_char == BASE64_NO_PADDING) /* the fewest 6 bit characters */
        return (Int)((uint64_t)(n / 3) * 4 + (uint64_t)((n % 3 * 8 + 5) / 6));
    Int m = (Int)((uint64_t)n + 2) / 3; /* the fewest 4 character blocks */
    return (Int)((uint64_t)m * 4);
}

static Int base64_decoded_len(Int n, Rune pad_char) {
    if (pad_char == BASE64_NO_PADDING)
        return n / 4 * 3 + n % 4 * 6 / 8; /* may end with 2 or 3 characters */
    return n / 4 * 3;                     /* always a multiple of 4 */
}

Int base64_encoding_decoded_len(const Base64Encoding *enc, Int n) {
    return base64_decoded_len(n, enc->pad_char);
}

/* ------------------------------------------------------------------ encode */

/* Encodes n bytes of src into dst, which has room for all of it. */
static void base64_encode_raw(const Base64Encoding *enc, Byte *dst, const Byte *src,
                              Int n) {
    const Byte *e = enc->encode;
    while (n >= 3) {
        uint32_t val = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];
        dst[0] = e[val >> 18 & 0x3f];
        dst[1] = e[val >> 12 & 0x3f];
        dst[2] = e[val >> 6 & 0x3f];
        dst[3] = e[val & 0x3f];
        src += 3;
        dst += 4;
        n -= 3;
    }
    if (n == 1) {
        uint32_t val = (uint32_t)src[0] << 16;
        dst[0] = e[val >> 18 & 0x3f];
        dst[1] = e[val >> 12 & 0x3f];
        if (enc->pad_char != BASE64_NO_PADDING) {
            dst[2] = (Byte)enc->pad_char;
            dst[3] = (Byte)enc->pad_char;
        }
    } else if (n == 2) {
        uint32_t val = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8;
        dst[0] = e[val >> 18 & 0x3f];
        dst[1] = e[val >> 12 & 0x3f];
        dst[2] = e[val >> 6 & 0x3f];
        if (enc->pad_char != BASE64_NO_PADDING)
            dst[3] = (Byte)enc->pad_char;
    }
}

void base64_encoding_encode(const Base64Encoding *enc, Slice dst, Slice src) {
    if (src.len == 0)
        return;
    Int need = base64_encoding_encoded_len(enc, src.len);
    if (dst.len < need) {
        /* Where Go's writes run off the end: the check of dst[3] in the block
         * that does not fit, or the first write past the end in the tail. */
        Int full = src.len / 3;
        if (dst.len < full * 4) {
            Int left = dst.len - dst.len / 4 * 4;
            runtime_index_out_of_range(3, left);
        }
        Int left = dst.len - full * 4;
        runtime_index_out_of_range(left, left);
    }
    base64_encode_raw(enc, (Byte *)dst.p, (const Byte *)src.p, src.len);
}

/* Grows dst by n and returns where the new bytes start, or NULL when it could
 * not. A zero Slice is Go's nil []byte. */
static Byte *base64_grow(Alloc *a, Slice *dst, Int n) {
    if (dst->elem == NULL)
        *dst = slice_nil(TYPE_BYTE);
    Int old = dst->len;
    *dst = slice_append(a, *dst, NULL, n);
    return dst->p == NULL ? NULL : (Byte *)dst->p + old;
}

Slice base64_encoding_append_encode(const Base64Encoding *enc, Alloc *a, Slice dst,
                                    Slice src) {
    Int n = base64_encoding_encoded_len(enc, src.len);
    Byte *p = base64_grow(a, &dst, n);
    if (p != NULL)
        base64_encode_raw(enc, p, (const Byte *)src.p, src.len);
    return dst;
}

Str base64_encoding_encode_to_string(const Base64Encoding *enc, Alloc *a, Slice src) {
    Int n = base64_encoding_encoded_len(enc, src.len);
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    base64_encode_raw(enc, p, (const Byte *)src.p, src.len);
    return str_from_bytes(p, n);
}

/* ------------------------------------------------------------------ decode */

/* binary.BigEndian.PutUint64 and PutUint32, as one store each. */
static inline void base64_put_be64(Byte *p, uint64_t v) {
#if BURROW_LITTLE_ENDIAN
    v = bits_reverse_bytes64(v);
#endif
    memcpy(p, &v, 8);
}

static inline void base64_put_be32(Byte *p, uint32_t v) {
#if BURROW_LITTLE_ENDIAN
    v = bits_reverse_bytes32(v);
#endif
    memcpy(p, &v, 4);
}

/* Decodes up to four characters of src from si into dst, which has room for
 * cap bytes. Sets *nsi to where it stopped reading and returns how many bytes
 * it wrote. */
static Int base64_decode_quantum(const Base64Encoding *enc, Byte *dst, Int cap,
                                 const Byte *src, Int len, Int si, Int *nsi,
                                 Error *err) {
    Byte dbuf[4] = {0, 0, 0, 0};
    Int dlen = 4;
    Error bad = BURROW_NO_ERROR;

    for (Int j = 0; j < 4; j++) {
        if (len == si) {
            if (j == 0) {
                *nsi = si;
                *err = BURROW_NO_ERROR;
                return 0;
            }
            if (j == 1 || enc->pad_char != BASE64_NO_PADDING) {
                *nsi = si;
                *err = base64_corrupt(si - j);
                return 0;
            }
            dlen = j;
            break;
        }
        Byte in = src[si];
        si++;

        Byte out = enc->decode_map[in];
        if (out != BASE64_INVALID) {
            dbuf[j] = out;
            continue;
        }

        if (in == '\n' || in == '\r') {
            j--;
            continue;
        }

        if ((Rune)in != enc->pad_char) {
            *nsi = si;
            *err = base64_corrupt(si - 1);
            return 0;
        }

        /* The end, with padding. */
        if (j == 0 || j == 1) {
            *nsi = si;
            *err = base64_corrupt(si - 1);
            return 0;
        }
        if (j == 2) {
            /* "==" is expected and the first '=' is read already. */
            while (si < len && (src[si] == '\n' || src[si] == '\r'))
                si++;
            if (si == len) {
                *nsi = si;
                *err = base64_corrupt(len);
                return 0;
            }
            if ((Rune)src[si] != enc->pad_char) {
                *nsi = si;
                *err = base64_corrupt(si - 1);
                return 0;
            }
            si++;
        }

        while (si < len && (src[si] == '\n' || src[si] == '\r'))
            si++;
        if (si < len)
            bad = base64_corrupt(si); /* trailing garbage */
        dlen = j;
        break;
    }

    /* Four 6 bit values into three bytes. */
    uint32_t val = (uint32_t)dbuf[0] << 18 | (uint32_t)dbuf[1] << 12 |
                   (uint32_t)dbuf[2] << 6 | (uint32_t)dbuf[3];
    Byte b0 = (Byte)(val >> 16), b1 = (Byte)(val >> 8), b2 = (Byte)val;
    /* Go writes dst[dlen-2] first, so that is where a short dst panics. */
    if (cap < dlen - 1)
        runtime_index_out_of_range(dlen - 2, cap);
    switch (dlen) {
    case 4:
        dst[2] = b2;
        b2 = 0;
        /* fall through */
    case 3:
        dst[1] = b1;
        if (enc->strict && b2 != 0) {
            *nsi = si;
            *err = base64_corrupt(si - 1);
            return 0;
        }
        b1 = 0;
        /* fall through */
    default: /* 2 */
        dst[0] = b0;
        if (enc->strict && (b1 != 0 || b2 != 0)) {
            *nsi = si;
            *err = base64_corrupt(si - 2);
            return 0;
        }
    }

    *nsi = si;
    *err = bad;
    return dlen - 1;
}

static Int base64_decode_raw(const Base64Encoding *enc, Byte *dst, Int cap,
                             const Byte *src, Int len, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = 0, si = 0;
    const Byte *m = enc->decode_map;

    while (len - si >= 8 && cap - n >= 8) {
        const Byte *s = src + si;
        Byte d0 = m[s[0]], d1 = m[s[1]], d2 = m[s[2]], d3 = m[s[3]];
        Byte d4 = m[s[4]], d5 = m[s[5]], d6 = m[s[6]], d7 = m[s[7]];
        /* An invalid character maps to 0xff, which makes the OR 0xff. */
        if ((d0 | d1 | d2 | d3 | d4 | d5 | d6 | d7) != 0xff) {
            uint64_t v = (uint64_t)d0 << 58 | (uint64_t)d1 << 52 | (uint64_t)d2 << 46 |
                         (uint64_t)d3 << 40 | (uint64_t)d4 << 34 | (uint64_t)d5 << 28 |
                         (uint64_t)d6 << 22 | (uint64_t)d7 << 16;
            /* Go stores all eight bytes, the last two of them zero. */
            base64_put_be64(dst + n, v);
            n += 6;
            si += 8;
        } else {
            n += base64_decode_quantum(enc, dst + n, cap - n, src, len, si, &si, &e);
            if (BURROW_FAILED(e))
                goto out;
        }
    }

    while (len - si >= 4 && cap - n >= 4) {
        const Byte *s = src + si;
        Byte d0 = m[s[0]], d1 = m[s[1]], d2 = m[s[2]], d3 = m[s[3]];
        if ((d0 | d1 | d2 | d3) != 0xff) {
            uint32_t v = (uint32_t)d0 << 26 | (uint32_t)d1 << 20 | (uint32_t)d2 << 14 |
                         (uint32_t)d3 << 8;
            base64_put_be32(dst + n, v);
            n += 3;
            si += 4;
        } else {
            n += base64_decode_quantum(enc, dst + n, cap - n, src, len, si, &si, &e);
            if (BURROW_FAILED(e))
                goto out;
        }
    }

    while (si < len) {
        n += base64_decode_quantum(enc, dst == NULL ? NULL : dst + n, cap - n, src, len,
                                   si, &si, &e);
        if (BURROW_FAILED(e))
            goto out;
    }
out:
    BURROW_OUT(err, e);
    return n;
}

Int base64_encoding_decode(const Base64Encoding *enc, Slice dst, Slice src,
                           Error *err) {
    return base64_decode_raw(enc, (Byte *)dst.p, dst.len, (const Byte *)src.p, src.len,
                             err);
}

Slice base64_encoding_append_decode(const Base64Encoding *enc, Alloc *a, Slice dst,
                                    Slice src, Error *err) {
    /* The size without the padding, so as not to ask for more than needed. */
    const Byte *s = (const Byte *)src.p;
    Int n = src.len;
    while (n > 0 && (Rune)s[n - 1] == enc->pad_char)
        n--;
    n = base64_decoded_len(n, BASE64_NO_PADDING);

    Int old = dst.len;
    Byte *p = base64_grow(a, &dst, n);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    Int got = base64_decode_raw(enc, p, n, s, src.len, err);
    dst.len = old + got;
    return dst;
}

Slice base64_encoding_decode_string(const Base64Encoding *enc, Alloc *a, Str s,
                                    Error *err) {
    Int n = base64_encoding_decoded_len(enc, s.len);
    Slice dst = slice_make(a, TYPE_BYTE, n, n);
    if (n > 0 && dst.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    dst.len = base64_decode_raw(enc, (Byte *)dst.p, n, s.p, s.len, err);
    return dst;
}

/* --------------------------------------------------------------- streaming */

#define BASE64_OUT_SIZE 1024

typedef struct Base64Encoder {
    Error err;
    const Base64Encoding *enc;
    IoWriter w;
    Byte buf[3]; /* input waiting for a whole block */
    Int nbuf;
    Byte out[BASE64_OUT_SIZE];
} Base64Encoder;

static Error base64_put(IoWriter w, Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_from(p, n, n, TYPE_BYTE), &err);
    return err;
}

static Int base64_encoder_write(void *self, Slice p, Error *err) {
    Base64Encoder *e = (Base64Encoder *)self;
    if (BURROW_FAILED(e->err)) {
        BURROW_OUT(err, e->err);
        return 0;
    }
    const Byte *src = (const Byte *)p.p;
    Int left = p.len, n = 0;

    /* Top up a block started by the last write. */
    if (e->nbuf > 0) {
        Int i;
        for (i = 0; i < left && e->nbuf < 3; i++)
            e->buf[e->nbuf++] = src[i];
        n += i;
        src += i;
        left -= i;
        if (e->nbuf < 3) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
        base64_encode_raw(e->enc, e->out, e->buf, 3);
        e->err = base64_put(e->w, e->out, 4);
        if (BURROW_FAILED(e->err)) {
            BURROW_OUT(err, e->err);
            return n;
        }
        e->nbuf = 0;
    }

    /* Whole blocks, a buffer at a time. */
    while (left >= 3) {
        Int nn = (Int)BASE64_OUT_SIZE / 4 * 3;
        if (nn > left) {
            nn = left;
            nn -= nn % 3;
        }
        base64_encode_raw(e->enc, e->out, src, nn);
        e->err = base64_put(e->w, e->out, nn / 3 * 4);
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

static Error base64_encoder_close(void *self) {
    Base64Encoder *e = (Base64Encoder *)self;
    if (BURROW_OK(e->err) && e->nbuf > 0) {
        base64_encode_raw(e->enc, e->out, e->buf, e->nbuf);
        e->err = base64_put(e->w, e->out, base64_encoding_encoded_len(e->enc, e->nbuf));
        e->nbuf = 0;
    }
    return e->err;
}

static const IoWriteCloserVT base64_encoder_vt = {
    {NULL, base64_encoder_write},
    {NULL, base64_encoder_close},
};

IoWriteCloser base64_new_encoder(Alloc *a, const Base64Encoding *enc, IoWriter w) {
    Base64Encoder *e = BURROW_NEW(a, Base64Encoder);
    if (e == NULL)
        return (IoWriteCloser){NULL, NULL};
    e->enc = enc;
    e->w = w;
    return (IoWriteCloser){&base64_encoder_vt, e};
}

typedef struct Base64Decoder {
    Error err;
    Error read_err; /* what r last said */
    const Base64Encoding *enc;
    IoReader r;
    Byte buf[1024]; /* input not yet decoded */
    Int nbuf;
    Int out_off, out_len; /* decoded output not yet read, in outbuf */
    Byte outbuf[1024 / 4 * 3];
} Base64Decoder;

/* r.Read with the newlines taken out, reading again when a read was nothing
 * but newlines. The decoder counts on its input having none. */
static Int base64_read_filtered(IoReader r, Byte *p, Int len, Error *err) {
    Int n = r.vt->read(r.data, slice_from(p, len, len, TYPE_BYTE), err);
    while (n > 0) {
        Int off = 0;
        for (Int i = 0; i < n; i++) {
            Byte b = p[i];
            if (b != '\r' && b != '\n') {
                if (i != off)
                    p[off] = b;
                off++;
            }
        }
        if (off > 0)
            return off;
        n = r.vt->read(r.data, slice_from(p, len, len, TYPE_BYTE), err);
    }
    return n;
}

static bool base64_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

static Int base64_take_out(Base64Decoder *d, Slice p) {
    Int n = p.len < d->out_len ? p.len : d->out_len;
    if (n > 0)
        memcpy(p.p, d->outbuf + d->out_off, (size_t)n);
    d->out_off += n;
    d->out_len -= n;
    return n;
}

static Int base64_decoder_read(void *self, Slice p, Error *err) {
    Base64Decoder *d = (Base64Decoder *)self;

    /* Output decoded last time and not yet read. */
    if (d->out_len > 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return base64_take_out(d, p);
    }

    if (BURROW_FAILED(d->err)) {
        BURROW_OUT(err, d->err);
        return 0;
    }

    /* Refill. */
    while (d->nbuf < 4 && BURROW_OK(d->read_err)) {
        Int nn = p.len / 3 * 4;
        if (nn < 4)
            nn = 4;
        if (nn > (Int)sizeof d->buf)
            nn = (Int)sizeof d->buf;
        Error rerr = BURROW_NO_ERROR;
        nn = base64_read_filtered(d->r, d->buf + d->nbuf, nn - d->nbuf, &rerr);
        d->read_err = rerr;
        d->nbuf += nn;
    }

    if (d->nbuf < 4) {
        if (d->enc->pad_char == BASE64_NO_PADDING && d->nbuf > 0) {
            /* The last few characters, with no padding to finish them. */
            Int nw = base64_decode_raw(d->enc, d->outbuf, (Int)sizeof d->outbuf, d->buf,
                                       d->nbuf, &d->err);
            d->nbuf = 0;
            d->out_off = 0;
            d->out_len = nw;
            Int n = base64_take_out(d, p);
            if (n > 0 || (p.len == 0 && d->out_len > 0)) {
                BURROW_OUT(err, BURROW_NO_ERROR);
                return n;
            }
            if (BURROW_FAILED(d->err)) {
                BURROW_OUT(err, d->err);
                return 0;
            }
        }
        d->err = d->read_err;
        if (base64_is_eof(d->err) && d->nbuf > 0)
            d->err = io_err_unexpected_eof;
        BURROW_OUT(err, d->err);
        return 0;
    }

    /* Decode into p, or into outbuf and then p when p is too small. */
    Int nr = d->nbuf / 4 * 4;
    Int nw = d->nbuf / 4 * 3;
    Int n;
    if (nw > p.len) {
        nw = base64_decode_raw(d->enc, d->outbuf, (Int)sizeof d->outbuf, d->buf, nr,
                               &d->err);
        d->out_off = 0;
        d->out_len = nw;
        n = base64_take_out(d, p);
    } else {
        n = base64_decode_raw(d->enc, (Byte *)p.p, p.len, d->buf, nr, &d->err);
    }
    d->nbuf -= nr;
    memmove(d->buf, d->buf + nr, (size_t)d->nbuf);
    BURROW_OUT(err, d->err);
    return n;
}

static const IoReaderVT base64_decoder_vt = {NULL, base64_decoder_read};

IoReader base64_new_decoder(Alloc *a, const Base64Encoding *enc, IoReader r) {
    Base64Decoder *d = BURROW_NEW(a, Base64Decoder);
    if (d == NULL)
        return (IoReader){NULL, NULL};
    d->enc = enc;
    d->r = r;
    return (IoReader){&base64_decoder_vt, d};
}
