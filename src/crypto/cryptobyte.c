/* golang.org/x/crypto/cryptobyte, as vendored into the standard library: a
 * String for reading length prefixed and DER encoded values and a Builder for
 * writing them.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "cryptobyte.h"

#include "burrow/core.h"
#include "burrow/encoding/asn1.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

static Error cb_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static Alloc *cb_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* ------------------------------------------------------------------ String */

/* read: the next n bytes, or false. A nil string has no bytes to give, not
 * even zero of them, as slicing nil gives nil in Go. */
static bool cb_read(CryptobyteString *s, Int n, Slice *v) {
    if (s->len < n || n < 0 || s->p == NULL)
        return false;
    /* Go takes the bytes before it moves s, and so must this, since x509 reads
     * a string into itself, as in input.ReadASN1(&input, ...). */
    Slice r = {s->p, n, n, TYPE_BYTE};
    s->p = (Byte *)s->p + n;
    s->len -= n;
    s->cap -= n;
    if (v != NULL)
        *v = r;
    return true;
}

bool cryptobyte_string_skip(CryptobyteString *s, Int n) {
    return cb_read(s, n, NULL);
}

static bool cb_read_be(CryptobyteString *s, Int n, uint64_t *out) {
    Slice v;
    if (!cb_read(s, n, &v))
        return false;
    const Byte *p = (const Byte *)v.p;
    uint64_t r = 0;
    for (Int i = 0; i < n; i++)
        r = r << 8 | p[i];
    *out = r;
    return true;
}

bool cryptobyte_string_read_uint8(CryptobyteString *s, uint8_t *out) {
    uint64_t v;
    if (!cb_read_be(s, 1, &v))
        return false;
    *out = (uint8_t)v;
    return true;
}

bool cryptobyte_string_read_uint16(CryptobyteString *s, uint16_t *out) {
    uint64_t v;
    if (!cb_read_be(s, 2, &v))
        return false;
    *out = (uint16_t)v;
    return true;
}

bool cryptobyte_string_read_uint24(CryptobyteString *s, uint32_t *out) {
    uint64_t v;
    if (!cb_read_be(s, 3, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool cryptobyte_string_read_uint32(CryptobyteString *s, uint32_t *out) {
    uint64_t v;
    if (!cb_read_be(s, 4, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool cryptobyte_string_read_uint48(CryptobyteString *s, uint64_t *out) {
    return cb_read_be(s, 6, out);
}

bool cryptobyte_string_read_uint64(CryptobyteString *s, uint64_t *out) {
    return cb_read_be(s, 8, out);
}

static bool cb_read_length_prefixed(CryptobyteString *s, Int len_len,
                                    CryptobyteString *out) {
    uint64_t length;
    Slice v;
    /* A failure after the length leaves it read, as in Go. */
    if (!cb_read_be(s, len_len, &length) || !cb_read(s, (Int)length, &v))
        return false;
    *out = v;
    return true;
}

bool cryptobyte_string_read_uint8_length_prefixed(CryptobyteString *s,
                                                  CryptobyteString *out) {
    return cb_read_length_prefixed(s, 1, out);
}

bool cryptobyte_string_read_uint16_length_prefixed(CryptobyteString *s,
                                                   CryptobyteString *out) {
    return cb_read_length_prefixed(s, 2, out);
}

bool cryptobyte_string_read_uint24_length_prefixed(CryptobyteString *s,
                                                   CryptobyteString *out) {
    return cb_read_length_prefixed(s, 3, out);
}

bool cryptobyte_string_read_bytes(CryptobyteString *s, Slice *out, Int n) {
    return cb_read(s, n, out);
}

bool cryptobyte_string_copy_bytes(CryptobyteString *s, Slice out) {
    Slice v;
    if (!cb_read(s, out.len, &v))
        return false;
    if (out.len > 0)
        memmove(out.p, v.p, (size_t)out.len);
    return true;
}

bool cryptobyte_string_empty(CryptobyteString s) {
    return s.len == 0;
}

/* --------------------------------------------------------- String, ASN.1 */

/* readASN1: an element of any tag, with or without its header. */
static bool cb_read_asn1(CryptobyteString *s, CryptobyteString *out,
                         CryptobyteAsn1Tag *out_tag, bool skip_header) {
    if (s->len < 2)
        return false;
    const Byte *p = (const Byte *)s->p;
    Byte tag = p[0], len_byte = p[1];
    if ((tag & 0x1f) == 0x1f) {
        /* ITU-T X.690 section 8.1.2: the high tag number form, which nothing
         * here uses. */
        return false;
    }
    if (out_tag != NULL)
        *out_tag = (CryptobyteAsn1Tag)tag;

    /* length counts the header too. */
    uint32_t length, header_len;
    if ((len_byte & 0x80) == 0) {
        /* The short form, 8.1.3.4. */
        length = (uint32_t)len_byte + 2;
        header_len = 2;
    } else {
        /* The long form, 8.1.3.5. */
        Byte len_len = len_byte & 0x7f;
        if (len_len == 0 || len_len > 4 || s->len < 2 + (Int)len_len)
            return false;
        uint32_t len32 = 0;
        for (Byte i = 0; i < len_len; i++)
            len32 = len32 << 8 | p[2 + i];
        /* DER wants the short form for lengths under 128, 10.1. */
        if (len32 < 128)
            return false;
        /* And no zero bytes in front of the length. */
        if (len32 >> ((len_len - 1) * 8) == 0)
            return false;
        header_len = 2 + (uint32_t)len_len;
        if (header_len + len32 < len32)
            return false;
        length = header_len + len32;
    }

    /* Go's int(length) < 0, which can only happen with a 32 bit Int. */
#if BURROW_INT_MAX < UINT32_MAX
    if (length > (uint32_t)BURROW_INT_MAX)
        return false;
#endif
    if (!cryptobyte_string_read_bytes(s, out, (Int)length))
        return false;
    if (skip_header && !cryptobyte_string_skip(out, (Int)header_len))
        panic_str(BURROW_S("cryptobyte: internal error"));
    return true;
}

bool cryptobyte_string_read_any_asn1(CryptobyteString *s, CryptobyteString *out,
                                     CryptobyteAsn1Tag *out_tag) {
    return cb_read_asn1(s, out, out_tag, true);
}

bool cryptobyte_string_read_any_asn1_element(CryptobyteString *s, CryptobyteString *out,
                                             CryptobyteAsn1Tag *out_tag) {
    return cb_read_asn1(s, out, out_tag, false);
}

bool cryptobyte_string_read_asn1(CryptobyteString *s, CryptobyteString *out,
                                 CryptobyteAsn1Tag tag) {
    CryptobyteAsn1Tag t;
    return cryptobyte_string_read_any_asn1(s, out, &t) && t == tag;
}

bool cryptobyte_string_read_asn1_element(CryptobyteString *s, CryptobyteString *out,
                                         CryptobyteAsn1Tag tag) {
    CryptobyteAsn1Tag t;
    return cryptobyte_string_read_any_asn1_element(s, out, &t) && t == tag;
}

bool cryptobyte_string_read_asn1_bytes(CryptobyteString *s, Slice *out,
                                       CryptobyteAsn1Tag tag) {
    return cryptobyte_string_read_asn1(s, out, tag);
}

bool cryptobyte_string_peek_asn1_tag(CryptobyteString s, CryptobyteAsn1Tag tag) {
    if (s.len == 0)
        return false;
    return ((const Byte *)s.p)[0] == tag;
}

bool cryptobyte_string_skip_asn1(CryptobyteString *s, CryptobyteAsn1Tag tag) {
    CryptobyteString unused;
    return cryptobyte_string_read_asn1(s, &unused, tag);
}

bool cryptobyte_string_read_optional_asn1(CryptobyteString *s, CryptobyteString *out,
                                          bool *out_present, CryptobyteAsn1Tag tag) {
    bool present = cryptobyte_string_peek_asn1_tag(*s, tag);
    if (out_present != NULL)
        *out_present = present;
    CryptobyteString unused;
    if (present && !cryptobyte_string_read_asn1(s, out != NULL ? out : &unused, tag))
        return false;
    return true;
}

bool cryptobyte_string_skip_optional_asn1(CryptobyteString *s, CryptobyteAsn1Tag tag) {
    if (!cryptobyte_string_peek_asn1_tag(*s, tag))
        return true;
    CryptobyteString unused;
    return cryptobyte_string_read_asn1(s, &unused, tag);
}

bool cryptobyte_string_read_asn1_boolean(CryptobyteString *s, bool *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_BOOLEAN) ||
        bytes.len != 1)
        return false;
    switch (((const Byte *)bytes.p)[0]) {
    case 0:
        *out = false;
        break;
    case 0xff:
        *out = true;
        break;
    default:
        return false;
    }
    return true;
}

/* checkASN1Integer: not empty and minimally encoded, 8.3.2. */
static bool cb_check_asn1_integer(Slice bytes) {
    if (bytes.len == 0)
        return false;
    if (bytes.len == 1)
        return true;
    const Byte *p = (const Byte *)bytes.p;
    if ((p[0] == 0 && (p[1] & 0x80) == 0) || (p[0] == 0xff && (p[1] & 0x80) == 0x80))
        return false;
    return true;
}

/* asn1Signed: n as a two's complement number of up to 8 bytes. */
static bool cb_asn1_signed(int64_t *out, Slice n) {
    Int length = n.len;
    if (length > 8)
        return false;
    const Byte *p = (const Byte *)n.p;
    uint64_t r = (uint64_t)*out;
    for (Int i = 0; i < length; i++)
        r = r << 8 | p[i];
    /* Shift up and back down to carry the sign bit across. */
    unsigned shift = 64 - (unsigned)length * 8;
    if (shift < 64)
        r <<= shift;
    int64_t v = (int64_t)r;
    if (shift < 64)
        v >>= shift;
    else
        v = 0;
    *out = v;
    return true;
}

/* asn1Unsigned: n as an unsigned number, which may need a ninth byte of zero
 * in front. */
static bool cb_asn1_unsigned(uint64_t *out, Slice n) {
    Int length = n.len;
    const Byte *p = (const Byte *)n.p;
    if (length > 9 || (length == 9 && p[0] != 0)) {
        /* Too large for a uint64_t. */
        return false;
    }
    if ((p[0] & 0x80) != 0) {
        /* Negative. */
        return false;
    }
    uint64_t r = *out;
    for (Int i = 0; i < length; i++)
        r = r << 8 | p[i];
    *out = r;
    return true;
}

static bool cb_read_asn1_int64(CryptobyteString *s, int64_t *out) {
    CryptobyteString bytes;
    return cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_INTEGER) &&
           cb_check_asn1_integer(bytes) && cb_asn1_signed(out, bytes);
}

static bool cb_read_asn1_uint64(CryptobyteString *s, uint64_t *out) {
    CryptobyteString bytes;
    return cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_INTEGER) &&
           cb_check_asn1_integer(bytes) && cb_asn1_unsigned(out, bytes);
}

bool cryptobyte_string_read_asn1_integer_int(CryptobyteString *s, Int *out) {
    int64_t i = 0;
    if (!cb_read_asn1_int64(s, &i) || i < (int64_t)BURROW_INT_MIN ||
        i > (int64_t)BURROW_INT_MAX)
        return false;
    *out = (Int)i;
    return true;
}

bool cryptobyte_string_read_asn1_integer_int64(CryptobyteString *s, int64_t *out) {
    int64_t i = 0;
    if (!cb_read_asn1_int64(s, &i))
        return false;
    *out = i;
    return true;
}

bool cryptobyte_string_read_asn1_integer_uint64(CryptobyteString *s, uint64_t *out) {
    uint64_t u = 0;
    if (!cb_read_asn1_uint64(s, &u))
        return false;
    *out = u;
    return true;
}

bool cryptobyte_string_read_asn1_integer_big(CryptobyteString *s, BigInt *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_INTEGER) ||
        !cb_check_asn1_integer(bytes))
        return false;
    const Byte *p = (const Byte *)bytes.p;
    if ((p[0] & 0x80) == 0x80) {
        /* Negative: the bytes flipped are -n-1. */
        Alloc *h = heap_allocator();
        Byte *neg = mem_alloc_nozero(h, (size_t)bytes.len, 1);
        if (neg == NULL)
            panic_str(BURROW_S("cryptobyte: out of memory"));
        for (Int i = 0; i < bytes.len; i++)
            neg[i] = (Byte)~p[i];
        big_int_set_bytes(out, (Slice){neg, bytes.len, bytes.len, TYPE_BYTE});
        mem_free(h, neg, (size_t)bytes.len, 1);
        BigInt one = BIG_INT(h);
        big_int_set_int64(&one, 1);
        big_int_add(out, out, &one);
        big_int_neg(out, out);
        big_int_free(&one);
    } else {
        big_int_set_bytes(out, bytes);
    }
    return true;
}

bool cryptobyte_string_read_asn1_integer_bytes(CryptobyteString *s, Slice *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_INTEGER) ||
        !cb_check_asn1_integer(bytes))
        return false;
    if ((((const Byte *)bytes.p)[0] & 0x80) == 0x80)
        return false;
    while (bytes.len > 1 && ((const Byte *)bytes.p)[0] == 0)
        cryptobyte_string_skip(&bytes, 1);
    *out = bytes;
    return true;
}

bool cryptobyte_string_read_asn1_int64_with_tag(CryptobyteString *s, int64_t *out,
                                                CryptobyteAsn1Tag tag) {
    CryptobyteString bytes;
    return cryptobyte_string_read_asn1(s, &bytes, tag) &&
           cb_check_asn1_integer(bytes) && cb_asn1_signed(out, bytes);
}

bool cryptobyte_string_read_asn1_enum(CryptobyteString *s, Int *out) {
    CryptobyteString bytes;
    int64_t i = 0;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_ENUM) ||
        !cb_check_asn1_integer(bytes) || !cb_asn1_signed(&i, bytes))
        return false;
    if (i < (int64_t)BURROW_INT_MIN || i > (int64_t)BURROW_INT_MAX)
        return false;
    *out = (Int)i;
    return true;
}

/* readBase128Int: an arc of an OID, which must fit in 31 bits. */
static bool cb_read_base128_int(CryptobyteString *s, Int *out) {
    Int ret = 0;
    for (Int i = 0; s->len > 0; i++) {
        if (i == 5)
            return false;
        /* Stop before the next shift would go past 31 bits. */
        if (ret >= (Int)1 << (31 - 7))
            return false;
        ret <<= 7;
        Byte b = ((const Byte *)s->p)[0];
        cryptobyte_string_skip(s, 1);

        /* ITU-T X.690 section 8.19.2: the first byte of an arc is never 0x80,
         * which would be a zero in front. */
        if (i == 0 && b == 0x80)
            return false;

        ret |= (Int)(b & 0x7f);
        if ((b & 0x80) == 0) {
            *out = ret;
            return true;
        }
    }
    return false; /* truncated */
}

bool cryptobyte_string_read_asn1_object_identifier(CryptobyteString *s, Alloc *a,
                                                   Asn1ObjectIdentifier *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER) ||
        bytes.len == 0)
        return false;

    /* There are at most len(bytes)+1 arcs, since the first byte holds two. */
    a = cb_alloc(a);
    Int n = bytes.len + 1;
    Int *components = mem_alloc(a, (size_t)n * sizeof(Int), _Alignof(Int));
    if (components == NULL)
        panic_str(BURROW_S("cryptobyte: out of memory"));

    /* The first byte is 40*value1 + value2. */
    Int v;
    if (!cb_read_base128_int(&bytes, &v))
        goto fail;
    if (v < 80) {
        components[0] = v / 40;
        components[1] = v % 40;
    } else {
        components[0] = 2;
        components[1] = v - 80;
    }

    Int i = 2;
    for (; bytes.len > 0; i++) {
        if (!cb_read_base128_int(&bytes, &v))
            goto fail;
        components[i] = v;
    }
    *out = slice_from(components, i, n, TYPE_INT);
    return true;

fail:
    mem_free(a, components, (size_t)n * sizeof(Int), _Alignof(Int));
    return false;
}

static const char generalized_time_format[] = "20060102150405Z0700";
static const char default_utc_time_format[] = "060102150405Z0700";

/* t in layout, into the 64 bytes at buf, which every time in these layouts
 * fits in. */
static Str cb_format_time(Time t, Byte *buf, Str layout) {
    Slice out =
        time_append_format(t, heap_allocator(), (Slice){buf, 0, 64, TYPE_BYTE}, layout);
    if (out.p != buf)
        panic_str(BURROW_S("cryptobyte: internal error"));
    return str_from_bytes(out.p, out.len);
}

/* time.Parse, and true only when formatting the time gives s back. */
static bool cb_parse_time_exact(Alloc *a, Str layout, Str s, Time *out) {
    Error err = BURROW_NO_ERROR;
    Time res = time_parse(a, layout, s, &err);
    if (BURROW_FAILED(err))
        return false;
    Byte buf[64];
    if (s.len > (Int)sizeof buf || !str_eq(cb_format_time(res, buf, layout), s))
        return false;
    *out = res;
    return true;
}

bool cryptobyte_string_read_asn1_generalized_time(CryptobyteString *s, Alloc *a,
                                                  Time *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_GENERALIZED_TIME))
        return false;
    Str t = str_from_bytes(bytes.p, bytes.len);
    Time res;
    if (!cb_parse_time_exact(cb_alloc(a), str_from_cstr(generalized_time_format), t,
                             &res))
        return false;
    *out = res;
    return true;
}

bool cryptobyte_string_read_asn1_utc_time(CryptobyteString *s, Alloc *a, Time *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_UTC_TIME))
        return false;
    Str t = str_from_bytes(bytes.p, bytes.len);
    a = cb_alloc(a);
    Str format = str_from_cstr(default_utc_time_format);
    Error err = BURROW_NO_ERROR;
    Time res = time_parse(a, format, t, &err);
    if (BURROW_FAILED(err)) {
        /* Seconds are optional in a UTCTime, and Go tries again without. */
        format = BURROW_S("0601021504Z0700");
        err = BURROW_NO_ERROR;
        res = time_parse(a, format, t, &err);
    }
    if (BURROW_FAILED(err))
        return false;
    Byte buf[64];
    if (t.len > (Int)sizeof buf || !str_eq(cb_format_time(res, buf, format), t))
        return false;
    if (time_year(res) >= 2050) {
        /* UTCTime only has two digits of year, and RFC 5280 section
         * 4.1.2.5.1 says 50 and up are the 1900s. */
        res = time_add_date(res, -100, 0, 0);
    }
    *out = res;
    return true;
}

bool cryptobyte_string_read_asn1_bit_string(CryptobyteString *s, Asn1BitString *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_BIT_STRING) ||
        bytes.len == 0 || bytes.len > BURROW_INT_MAX / 8)
        return false;
    Byte padding_bits = ((const Byte *)bytes.p)[0];
    cryptobyte_string_skip(&bytes, 1);
    if (padding_bits > 7 || (bytes.len == 0 && padding_bits != 0) ||
        (bytes.len > 0 &&
         (((const Byte *)bytes.p)[bytes.len - 1] & ((1U << padding_bits) - 1)) != 0))
        return false;
    out->bit_length = bytes.len * 8 - (Int)padding_bits;
    out->bytes = bytes;
    return true;
}

bool cryptobyte_string_read_asn1_bit_string_as_bytes(CryptobyteString *s, Slice *out) {
    CryptobyteString bytes;
    if (!cryptobyte_string_read_asn1(s, &bytes, CRYPTOBYTE_ASN1_BIT_STRING) ||
        bytes.len == 0)
        return false;
    if (((const Byte *)bytes.p)[0] != 0)
        return false;
    cryptobyte_string_skip(&bytes, 1);
    *out = bytes;
    return true;
}

/* The INTEGER inside an optional element with tag, or false and nothing
 * consumed when it is not there. present says which. */
static bool cb_read_optional_child(CryptobyteString *s, CryptobyteString *child,
                                   bool *present, CryptobyteAsn1Tag tag) {
    *child = (CryptobyteString){0};
    return cryptobyte_string_read_optional_asn1(s, child, present, tag);
}

bool cryptobyte_string_read_optional_asn1_integer_int(CryptobyteString *s, Int *out,
                                                      CryptobyteAsn1Tag tag, Int def) {
    bool present;
    CryptobyteString i;
    if (!cb_read_optional_child(s, &i, &present, tag))
        return false;
    if (!present) {
        *out = def;
        return true;
    }
    return cryptobyte_string_read_asn1_integer_int(&i, out) &&
           cryptobyte_string_empty(i);
}

bool cryptobyte_string_read_optional_asn1_integer_int64(CryptobyteString *s,
                                                        int64_t *out,
                                                        CryptobyteAsn1Tag tag,
                                                        int64_t def) {
    bool present;
    CryptobyteString i;
    if (!cb_read_optional_child(s, &i, &present, tag))
        return false;
    if (!present) {
        *out = def;
        return true;
    }
    return cryptobyte_string_read_asn1_integer_int64(&i, out) &&
           cryptobyte_string_empty(i);
}

bool cryptobyte_string_read_optional_asn1_integer_uint64(CryptobyteString *s,
                                                         uint64_t *out,
                                                         CryptobyteAsn1Tag tag,
                                                         uint64_t def) {
    bool present;
    CryptobyteString i;
    if (!cb_read_optional_child(s, &i, &present, tag))
        return false;
    if (!present) {
        *out = def;
        return true;
    }
    return cryptobyte_string_read_asn1_integer_uint64(&i, out) &&
           cryptobyte_string_empty(i);
}

bool cryptobyte_string_read_optional_asn1_integer_big(CryptobyteString *s, BigInt *out,
                                                      CryptobyteAsn1Tag tag,
                                                      const BigInt *def) {
    bool present;
    CryptobyteString i;
    if (!cb_read_optional_child(s, &i, &present, tag))
        return false;
    if (!present) {
        big_int_set(out, def);
        return true;
    }
    return cryptobyte_string_read_asn1_integer_big(&i, out) &&
           cryptobyte_string_empty(i);
}

bool cryptobyte_string_read_optional_asn1_integer_bytes(CryptobyteString *s, Slice *out,
                                                        CryptobyteAsn1Tag tag,
                                                        Slice def) {
    bool present;
    CryptobyteString i;
    if (!cb_read_optional_child(s, &i, &present, tag))
        return false;
    if (!present) {
        *out = def;
        return true;
    }
    return cryptobyte_string_read_asn1_integer_bytes(&i, out) &&
           cryptobyte_string_empty(i);
}

bool cryptobyte_string_read_optional_asn1_octet_string(CryptobyteString *s, Slice *out,
                                                       bool *out_present,
                                                       CryptobyteAsn1Tag tag) {
    bool present;
    CryptobyteString child;
    if (!cb_read_optional_child(s, &child, &present, tag))
        return false;
    if (out_present != NULL)
        *out_present = present;
    if (present) {
        CryptobyteString oct;
        if (!cryptobyte_string_read_asn1(&child, &oct, CRYPTOBYTE_ASN1_OCTET_STRING) ||
            !cryptobyte_string_empty(child))
            return false;
        *out = oct;
    } else {
        *out = slice_nil(TYPE_BYTE);
    }
    return true;
}

bool cryptobyte_string_read_optional_asn1_boolean(CryptobyteString *s, bool *out,
                                                  CryptobyteAsn1Tag tag, bool def) {
    bool present;
    CryptobyteString child;
    if (!cb_read_optional_child(s, &child, &present, tag))
        return false;
    if (!present) {
        *out = def;
        return true;
    }
    return cryptobyte_string_read_asn1_boolean(&child, out);
}

/* ----------------------------------------------------------------- Builder */

CryptobyteBuilder cryptobyte_new_builder(Alloc *a, Slice buffer) {
    return (CryptobyteBuilder){.result = buffer, .a = a};
}

CryptobyteBuilder cryptobyte_new_fixed_builder(Slice buffer) {
    return (CryptobyteBuilder){.result = buffer, .fixed_size = true};
}

void cryptobyte_builder_free(CryptobyteBuilder *b) {
    if (b->owned && b->result.p != NULL)
        mem_free(cb_alloc(b->a), b->result.p, (size_t)b->result.cap, 1);
    b->result = slice_nil(TYPE_BYTE);
    b->owned = false;
}

void cryptobyte_builder_set_error(CryptobyteBuilder *b, Error err) {
    b->err = err;
}

Slice cryptobyte_builder_bytes(CryptobyteBuilder *b, Error *err) {
    if (BURROW_FAILED(b->err)) {
        BURROW_OUT(err, b->err);
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (b->result.p == NULL)
        return slice_nil(TYPE_BYTE);
    return (Slice){(Byte *)b->result.p + b->offset, b->result.len - b->offset,
                   b->result.cap - b->offset, TYPE_BYTE};
}

Slice cryptobyte_builder_bytes_or_panic(CryptobyteBuilder *b) {
    if (BURROW_FAILED(b->err)) {
        Error *box = mem_alloc(error_allocator(), sizeof *box, _Alignof(Error));
        if (box == NULL)
            panic_str(BURROW_S("cryptobyte: out of memory"));
        *box = b->err;
        panic(BURROW_ANY(TYPE_ERROR, box));
    }
    return cryptobyte_builder_bytes(b, NULL);
}

/* add: n bytes from bytes, or n zeros when bytes is NULL. */
static void cb_add(CryptobyteBuilder *b, const Byte *bytes, Int n) {
    if (BURROW_FAILED(b->err))
        return;
    if (b->child != NULL)
        panic_str(BURROW_S("cryptobyte: attempted write while child is pending"));
    if (n > BURROW_INT_MAX - b->result.len) {
        b->err = cb_error("cryptobyte: length overflow");
        return;
    }
    Int need = b->result.len + n;
    if (b->fixed_size && need > b->result.cap) {
        b->err = cb_error("cryptobyte: Builder is exceeding its fixed-size buffer");
        return;
    }
    if (need > b->result.cap) {
        /* append's growth: double while small, then a quarter more. */
        Int cap = b->result.cap;
        Int ncap = cap < 256 ? cap * 2 : cap + (cap + (Int)3 * 256) / 4;
        if (ncap < need)
            ncap = need;
        if (ncap < 8)
            ncap = 8;
        Alloc *a = cb_alloc(b->a);
        Byte *p = mem_alloc_nozero(a, (size_t)ncap, 1);
        if (p == NULL)
            panic_str(BURROW_S("cryptobyte: out of memory"));
        if (b->result.len > 0)
            memcpy(p, b->result.p, (size_t)b->result.len);
        if (b->owned)
            mem_free(a, b->result.p, (size_t)cap, 1);
        b->result.p = p;
        b->result.cap = ncap;
        b->owned = true;
    }
    Byte *dst = (Byte *)b->result.p + b->result.len;
    if (n > 0) {
        if (bytes != NULL)
            memmove(dst, bytes, (size_t)n);
        else
            memset(dst, 0, (size_t)n);
    }
    b->result.len = need;
    if (b->result.elem == NULL)
        b->result.elem = TYPE_BYTE;
}

void cryptobyte_builder_add_uint8(CryptobyteBuilder *b, uint8_t v) {
    Byte x[1] = {v};
    cb_add(b, x, 1);
}

void cryptobyte_builder_add_uint16(CryptobyteBuilder *b, uint16_t v) {
    Byte x[2] = {(Byte)(v >> 8), (Byte)v};
    cb_add(b, x, 2);
}

void cryptobyte_builder_add_uint24(CryptobyteBuilder *b, uint32_t v) {
    Byte x[3] = {(Byte)(v >> 16), (Byte)(v >> 8), (Byte)v};
    cb_add(b, x, 3);
}

void cryptobyte_builder_add_uint32(CryptobyteBuilder *b, uint32_t v) {
    Byte x[4] = {(Byte)(v >> 24), (Byte)(v >> 16), (Byte)(v >> 8), (Byte)v};
    cb_add(b, x, 4);
}

void cryptobyte_builder_add_uint48(CryptobyteBuilder *b, uint64_t v) {
    Byte x[6] = {(Byte)(v >> 40), (Byte)(v >> 32), (Byte)(v >> 24),
                 (Byte)(v >> 16), (Byte)(v >> 8),  (Byte)v};
    cb_add(b, x, 6);
}

void cryptobyte_builder_add_uint64(CryptobyteBuilder *b, uint64_t v) {
    Byte x[8] = {(Byte)(v >> 56), (Byte)(v >> 48), (Byte)(v >> 40), (Byte)(v >> 32),
                 (Byte)(v >> 24), (Byte)(v >> 16), (Byte)(v >> 8),  (Byte)v};
    cb_add(b, x, 8);
}

void cryptobyte_builder_add_bytes(CryptobyteBuilder *b, Slice v) {
    cb_add(b, (const Byte *)v.p, v.len);
}

static void cb_flush_child(CryptobyteBuilder *b) {
    if (b->child == NULL)
        return;
    cb_flush_child(b->child);
    CryptobyteBuilder *child = b->child;
    b->child = NULL;

    /* The child may have grown the storage, and given the old one back, so b
     * takes the new one whatever happens next. Its length only changes when
     * the child's contents are good. */
    void *old_p = b->result.p;
    b->result.p = child->result.p;
    b->result.cap = child->result.cap;
    b->result.elem = child->result.elem;
    b->owned = child->owned;

    if (BURROW_FAILED(child->err)) {
        b->err = child->err;
        return;
    }

    Int length = child->result.len - child->pending_len_len - child->offset;
    if (length < 0)
        panic_str(BURROW_S("cryptobyte: internal error")); /* result shrunk */

    if (child->pending_is_asn1) {
        /* For ASN.1 the length goes in the short form when it fits and in
         * the long form when it does not, 8.1.3. */
        if (child->pending_len_len != 1)
            panic_str(BURROW_S("cryptobyte: internal error"));
        Byte len_len, len_byte;
        if ((int64_t)length > 0xfffffffe) {
            b->err = cb_error("pending ASN.1 child too long");
            return;
        }
        if (length > 0xffffff) {
            len_len = 5;
            len_byte = 0x80 | 4;
        } else if (length > 0xffff) {
            len_len = 4;
            len_byte = 0x80 | 3;
        } else if (length > 0xff) {
            len_len = 3;
            len_byte = 0x80 | 2;
        } else if (length > 0x7f) {
            len_len = 2;
            len_byte = 0x80 | 1;
        } else {
            len_len = 1;
            len_byte = (Byte)length;
            length = 0;
        }

        /* Write the length byte, then make room for the rest of the length
         * and move the contents up past them. */
        ((Byte *)child->result.p)[child->offset] = len_byte;
        Int extra_bytes = (Int)len_len - 1;
        if (extra_bytes != 0) {
            cb_add(child, NULL, extra_bytes);
            Int child_start = child->offset + child->pending_len_len;
            Int count = child->result.len - child_start - extra_bytes;
            if (count > 0)
                memmove((Byte *)child->result.p + child_start + extra_bytes,
                        (Byte *)child->result.p + child_start, (size_t)count);
        }
        child->offset++;
        child->pending_len_len = extra_bytes;
    }

    Int l = length;
    for (Int i = child->pending_len_len - 1; i >= 0; i--) {
        ((Byte *)child->result.p)[child->offset + i] = (Byte)l;
        l >>= 8;
    }
    /* The child's storage may have moved again for the ASN.1 length. */
    b->result.p = child->result.p;
    b->result.cap = child->result.cap;
    b->owned = child->owned;
    if (l != 0) {
        b->err = fmt_errorf_v("cryptobyte: pending child length %d exceeds %d-byte "
                              "length prefix",
                              length, child->pending_len_len);
        return;
    }

    if (b->fixed_size && old_p != child->result.p)
        panic_str(BURROW_S(
            "cryptobyte: BuilderContinuation reallocated a fixed-size buffer"));

    b->result = child->result;
}

static void cb_add_length_prefixed(CryptobyteBuilder *b, Int len_len, bool is_asn1,
                                   CryptobyteBuilderContinuation f) {
    /* Write nothing, not even the placeholder, once there is an error. */
    if (BURROW_FAILED(b->err))
        return;

    Int offset = b->result.len;
    cb_add(b, NULL, len_len);

    /* The child appends to b's storage, after the placeholder for the
     * length, and b takes the storage back when it flushes the child. */
    CryptobyteBuilder child = {
        .result = b->result,
        .a = b->a,
        .owned = b->owned,
        .fixed_size = b->fixed_size,
        .offset = offset,
        .pending_len_len = len_len,
        .pending_is_asn1 = is_asn1,
    };
    b->child = &child;
    BURROW_CALLF(f, &child);
    cb_flush_child(b);
    if (b->child != NULL)
        panic_str(BURROW_S("cryptobyte: internal error"));
}

void cryptobyte_builder_add_uint8_length_prefixed(CryptobyteBuilder *b,
                                                  CryptobyteBuilderContinuation f) {
    cb_add_length_prefixed(b, 1, false, f);
}

void cryptobyte_builder_add_uint16_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f) {
    cb_add_length_prefixed(b, 2, false, f);
}

void cryptobyte_builder_add_uint24_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f) {
    cb_add_length_prefixed(b, 3, false, f);
}

void cryptobyte_builder_add_uint32_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f) {
    cb_add_length_prefixed(b, 4, false, f);
}

void cryptobyte_builder_unwrite(CryptobyteBuilder *b, Int n) {
    if (BURROW_FAILED(b->err))
        return;
    if (b->child != NULL)
        panic_str(BURROW_S("cryptobyte: attempted unwrite while child is pending"));
    Int length = b->result.len - b->pending_len_len - b->offset;
    if (length < 0)
        panic_str(BURROW_S("cryptobyte: internal error"));
    if (n < 0)
        panic_str(
            BURROW_S("cryptobyte: attempted to unwrite negative number of bytes"));
    if (n > length)
        panic_str(BURROW_S("cryptobyte: attempted to unwrite more than was written"));
    b->result.len -= n;
}

void cryptobyte_builder_add_value(CryptobyteBuilder *b, CryptobyteMarshalingValue v) {
    Error err = BURROW_CALLF(v, b);
    if (BURROW_FAILED(err))
        b->err = err;
}

/* --------------------------------------------------------- Builder, ASN.1 */

static void cb_signed_contents(void *env, CryptobyteBuilder *c) {
    int64_t v = *(const int64_t *)env;
    Int length = 1;
    for (int64_t i = v; i >= 0x80 || i < -0x80; i >>= 8)
        length++;
    for (; length > 0; length--) {
        int64_t i = v >> ((length - 1) * 8) & 0xff;
        cryptobyte_builder_add_uint8(c, (uint8_t)i);
    }
}

static void cb_add_asn1_signed(CryptobyteBuilder *b, CryptobyteAsn1Tag tag, int64_t v) {
    cryptobyte_builder_add_asn1(
        b, tag, BURROW_FN(CryptobyteBuilderContinuation, cb_signed_contents, &v));
}

void cryptobyte_builder_add_asn1_int64(CryptobyteBuilder *b, int64_t v) {
    cb_add_asn1_signed(b, CRYPTOBYTE_ASN1_INTEGER, v);
}

void cryptobyte_builder_add_asn1_int64_with_tag(CryptobyteBuilder *b, int64_t v,
                                                CryptobyteAsn1Tag tag) {
    cb_add_asn1_signed(b, tag, v);
}

void cryptobyte_builder_add_asn1_enum(CryptobyteBuilder *b, int64_t v) {
    cb_add_asn1_signed(b, CRYPTOBYTE_ASN1_ENUM, v);
}

static void cb_unsigned_contents(void *env, CryptobyteBuilder *c) {
    uint64_t v = *(const uint64_t *)env;
    Int length = 1;
    for (uint64_t i = v; i >= 0x80; i >>= 8)
        length++;
    for (; length > 0; length--) {
        /* A shift of 64 or more is 0 in Go and undefined in C, and the ninth
         * byte of a number with the top bit set is that 0. */
        Int shift = (length - 1) * 8;
        uint64_t i = shift >= 64 ? 0 : v >> shift & 0xff;
        cryptobyte_builder_add_uint8(c, (uint8_t)i);
    }
}

void cryptobyte_builder_add_asn1_uint64(CryptobyteBuilder *b, uint64_t v) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_INTEGER,
        BURROW_FN(CryptobyteBuilderContinuation, cb_unsigned_contents, &v));
}

static void cb_big_int_contents(void *env, CryptobyteBuilder *c) {
    const BigInt *n = *(const BigInt **)env;
    Alloc *h = heap_allocator();
    if (big_int_sign(n) < 0) {
        /* A negative number is written in two's complement, which is the
         * bytes of -n-1 flipped, with 0xff in front if the top bit came out
         * clear. */
        BigInt m = BIG_INT(h);
        BigInt one = BIG_INT(h);
        big_int_set_int64(&one, 1);
        big_int_neg(&m, n);
        big_int_sub(&m, &m, &one);
        Slice bytes = big_int_bytes(&m, h);
        Byte *p = (Byte *)bytes.p;
        for (Int i = 0; i < bytes.len; i++)
            p[i] ^= 0xff;
        if (bytes.len == 0 || (p[0] & 0x80) == 0)
            cryptobyte_builder_add_uint8(c, 0xff);
        cb_add(c, p, bytes.len);
        if (bytes.cap > 0)
            mem_free(h, bytes.p, (size_t)bytes.cap, 1);
        big_int_free(&m);
        big_int_free(&one);
    } else if (big_int_sign(n) == 0) {
        cryptobyte_builder_add_uint8(c, 0);
    } else {
        Slice bytes = big_int_bytes(n, h);
        const Byte *p = (const Byte *)bytes.p;
        if ((p[0] & 0x80) != 0)
            cryptobyte_builder_add_uint8(c, 0);
        cb_add(c, p, bytes.len);
        if (bytes.cap > 0)
            mem_free(h, bytes.p, (size_t)bytes.cap, 1);
    }
}

void cryptobyte_builder_add_asn1_big_int(CryptobyteBuilder *b, const BigInt *n) {
    if (BURROW_FAILED(b->err))
        return;
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_INTEGER,
        BURROW_FN(CryptobyteBuilderContinuation, cb_big_int_contents, &n));
}

static void cb_bytes_contents(void *env, CryptobyteBuilder *c) {
    cryptobyte_builder_add_bytes(c, *(const Slice *)env);
}

void cryptobyte_builder_add_asn1_octet_string(CryptobyteBuilder *b, Slice bytes) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_OCTET_STRING,
        BURROW_FN(CryptobyteBuilderContinuation, cb_bytes_contents, &bytes));
}

void cryptobyte_builder_add_asn1_generalized_time(CryptobyteBuilder *b, Time t) {
    Int year = time_year(t);
    if (year < 0 || year > 9999) {
        b->err = fmt_errorf_v("cryptobyte: cannot represent %s as a GeneralizedTime",
                              time_string(t, error_allocator()));
        return;
    }
    Byte buf[64];
    Str s = cb_format_time(t, buf, str_from_cstr(generalized_time_format));
    Slice bytes = {buf, s.len, s.len, TYPE_BYTE}; /* s is in buf */
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_GENERALIZED_TIME,
        BURROW_FN(CryptobyteBuilderContinuation, cb_bytes_contents, &bytes));
}

typedef struct CbUtcTime {
    CryptobyteBuilder *b;
    Time t;
} CbUtcTime;

static void cb_utc_time_contents(void *env, CryptobyteBuilder *c) {
    CbUtcTime *u = env;
    Int year = time_year(u->t);
    if (year < 1950 || year >= 2050) {
        /* Go sets the error on the outer builder here, not on c. */
        u->b->err = fmt_errorf_v("cryptobyte: cannot represent %s as a UTCTime",
                                 time_string(u->t, error_allocator()));
        return;
    }
    Byte buf[64];
    Str s = cb_format_time(u->t, buf, str_from_cstr(default_utc_time_format));
    cb_add(c, s.p, s.len);
}

void cryptobyte_builder_add_asn1_utc_time(CryptobyteBuilder *b, Time t) {
    CbUtcTime u = {b, t};
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_UTC_TIME,
        BURROW_FN(CryptobyteBuilderContinuation, cb_utc_time_contents, &u));
}

static void cb_bit_string_contents(void *env, CryptobyteBuilder *c) {
    cryptobyte_builder_add_uint8(c, 0);
    cryptobyte_builder_add_bytes(c, *(const Slice *)env);
}

void cryptobyte_builder_add_asn1_bit_string(CryptobyteBuilder *b, Slice data) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_BIT_STRING,
        BURROW_FN(CryptobyteBuilderContinuation, cb_bit_string_contents, &data));
}

static void cb_add_base128_int(CryptobyteBuilder *b, int64_t n) {
    Int length = 0;
    if (n == 0) {
        length = 1;
    } else {
        for (int64_t i = n; i > 0; i >>= 7)
            length++;
    }
    for (Int i = length - 1; i >= 0; i--) {
        Byte o = (Byte)(n >> (i * 7));
        o &= 0x7f;
        if (i != 0)
            o |= 0x80;
        cb_add(b, &o, 1);
    }
}

static bool cb_is_valid_oid(Asn1ObjectIdentifier oid) {
    if (oid.len < 2)
        return false;
    const Int *v = (const Int *)oid.p;
    if (v[0] > 2 || (v[0] <= 1 && v[1] >= 40))
        return false;
    for (Int i = 0; i < oid.len; i++)
        if (v[i] < 0)
            return false;
    return true;
}

static void cb_oid_contents(void *env, CryptobyteBuilder *c) {
    Asn1ObjectIdentifier oid = *(const Asn1ObjectIdentifier *)env;
    if (!cb_is_valid_oid(oid)) {
        c->err = fmt_errorf_v("cryptobyte: invalid OID: %s",
                              asn1_object_identifier_string(oid, error_allocator()));
        return;
    }
    const Int *v = (const Int *)oid.p;
    cb_add_base128_int(c, (int64_t)v[0] * 40 + (int64_t)v[1]);
    for (Int i = 2; i < oid.len; i++)
        cb_add_base128_int(c, (int64_t)v[i]);
}

void cryptobyte_builder_add_asn1_object_identifier(CryptobyteBuilder *b,
                                                   Asn1ObjectIdentifier oid) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER,
        BURROW_FN(CryptobyteBuilderContinuation, cb_oid_contents, &oid));
}

static void cb_boolean_contents(void *env, CryptobyteBuilder *c) {
    cryptobyte_builder_add_uint8(c, *(const bool *)env ? 0xff : 0);
}

void cryptobyte_builder_add_asn1_boolean(CryptobyteBuilder *b, bool v) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_BOOLEAN,
        BURROW_FN(CryptobyteBuilderContinuation, cb_boolean_contents, &v));
}

void cryptobyte_builder_add_asn1_null(CryptobyteBuilder *b) {
    Byte x[2] = {CRYPTOBYTE_ASN1_NULL, 0};
    cb_add(b, x, 2);
}

void cryptobyte_builder_marshal_asn1(CryptobyteBuilder *b, Any v) {
    if (BURROW_FAILED(b->err))
        return;
    Alloc *h = heap_allocator();
    Error err = BURROW_NO_ERROR;
    Slice bytes = asn1_marshal(h, v, &err);
    if (BURROW_FAILED(err)) {
        b->err = err;
        return;
    }
    cb_add(b, (const Byte *)bytes.p, bytes.len);
    if (bytes.cap > 0)
        mem_free(h, bytes.p, (size_t)bytes.cap, 1);
}

void cryptobyte_builder_add_asn1(CryptobyteBuilder *b, CryptobyteAsn1Tag tag,
                                 CryptobyteBuilderContinuation f) {
    if (BURROW_FAILED(b->err))
        return;
    /* Identifiers with the low five bits set need the high tag number form,
     * 8.1.2.4, which nothing here writes. */
    if ((tag & 0x1f) == 0x1f) {
        b->err = fmt_errorf_v(
            "cryptobyte: high-tag number identifier octets not supported: 0x%x",
            (Int)tag);
        return;
    }
    cryptobyte_builder_add_uint8(b, tag);
    cb_add_length_prefixed(b, 1, true, f);
}
