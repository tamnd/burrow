/* uuid, documented in burrow/uuid.h.
 *
 * Derived from Go's src/uuid/uuid.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/uuid.h"

#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/lock.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/slice.h"

#include <string.h>

BURROW_SENTINEL_ERROR(burrow__uuid_err_invalid, "invalid uuid");

/* ------------------------------------------------------------------ parsing */

static int uu_unhex(Byte c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* hex.Decode of 2n digits at src into n bytes at dst, false on a bad digit. */
static bool uu_decode(Byte *dst, const Byte *src, Int n) {
    for (Int i = 0; i < n; i++) {
        int hi = uu_unhex(src[2 * i]);
        int lo = uu_unhex(src[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        dst[i] = (Byte)(hi << 4 | lo);
    }
    return true;
}

static bool uu_has_prefix(const Byte *p, Int n, const char *pre) {
    Int k = (Int)strlen(pre);
    return n >= k && memcmp(p, pre, (size_t)k) == 0;
}

Error uuid_unmarshal_text(Uuid *u, Slice text) {
    const Byte *b = text.p;
    Int n = text.len;
    Uuid dst;
    memset(&dst, 0, sizeof(dst));
    switch (n) {
    case 9 + 36:
        /* urn:uuid:00000000-0000-0000-0000-000000000000 */
        if (uu_has_prefix(b, n, "urn:uuid:")) {
            b += 9;
            n -= 9;
        }
        break;
    case 2 + 36:
        /* {00000000-0000-0000-0000-000000000000}. Go trims each brace on its
         * own, so a string with only one of them keeps 37 bytes and fails the
         * length check below. */
        if (b[0] == '{') {
            b++;
            n--;
        }
        if (n > 0 && b[n - 1] == '}')
            n--;
        break;
    case 32:
        /* 00000000000000000000000000000000 */
        if (!uu_decode(dst.b, b, 16))
            return burrow__uuid_err_invalid;
        *u = dst;
        return BURROW_NO_ERROR;
    default:
        break;
    }
    /* 00000000-0000-0000-0000-000000000000 */
    if (n != 36)
        return burrow__uuid_err_invalid;
    if (b[8] != '-' || b[13] != '-' || b[18] != '-' || b[23] != '-')
        return burrow__uuid_err_invalid;
    if (!uu_decode(dst.b, b, 4) || !uu_decode(dst.b + 4, b + 9, 2) ||
        !uu_decode(dst.b + 6, b + 14, 2) || !uu_decode(dst.b + 8, b + 19, 2) ||
        !uu_decode(dst.b + 10, b + 24, 6))
        return burrow__uuid_err_invalid;
    *u = dst;
    return BURROW_NO_ERROR;
}

Uuid uuid_parse_str(Str s, Error *err) {
    Uuid u;
    memset(&u, 0, sizeof(u));
    Error e = uuid_unmarshal_text(
        &u, (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE});
    BURROW_OUT(err, e);
    return u;
}

Uuid uuid_must_parse(Str s) {
    Error err;
    Uuid u = uuid_parse_str(s, &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return u;
}

/* ------------------------------------------------------------------ text */

static void uu_encode(Byte *dst, const Uuid *u) {
    static const char hexd[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            dst[j++] = '-';
        dst[j++] = (Byte)hexd[u->b[i] >> 4];
        dst[j++] = (Byte)hexd[u->b[i] & 15];
    }
}

Slice uuid_append_text(Uuid u, Alloc *a, Slice b, Error *err) {
    Byte text[36];
    uu_encode(text, &u);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    if (b.cap - b.len >= 36) {
        memcpy((Byte *)b.p + b.len, text, 36);
        b.len += 36;
    } else {
        b = slice_append(a, b, text, 36);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

Slice uuid_marshal_text(Uuid u, Alloc *a, Error *err) {
    return uuid_append_text(u, a, slice_make(a, TYPE_BYTE, 0, 36), err);
}

Str uuid_string(Uuid u, Alloc *a) {
    Byte text[36];
    uu_encode(text, &u);
    return str_clone(a, str_from_bytes(text, 36));
}

Int uuid_cmp(Uuid u, Uuid v) {
    for (int i = 0; i < 16; i++) {
        if (u.b[i] != v.b[i])
            return u.b[i] < v.b[i] ? -1 : 1;
    }
    return 0;
}

/* ------------------------------------------------------------- generating */

Uuid uuid_nil(void) {
    Uuid u;
    memset(&u, 0, sizeof(u));
    return u;
}

Uuid uuid_max(void) {
    Uuid u;
    memset(&u, 0xff, sizeof(u));
    return u;
}

static void uu_set_version(Uuid *u, Byte version) {
    u->b[6] = (Byte)((u->b[6] & 0x0f) | (version << 4));
}

static void uu_set_variant(Uuid *u, Byte variant) {
    u->b[8] = (Byte)((u->b[8] & 0x3f) | (variant << 6));
}

/* crypto/rand.Read, which Go documents as never failing: it crashes the
 * program rather than hand back bytes that are not random. */
static void uu_random(Byte *p, Int n) {
    if (!pal_random_bytes(p, n, NULL))
        runtime_throw(BURROW_S("uuid: the system random number generator failed"));
}

Uuid uuid_new_v4(void) {
    Uuid u;
    uu_random(u.b, 16);
    uu_set_version(&u, 4);
    uu_set_variant(&u, 2);
    return u;
}

Uuid uuid_new(void) {
    return uuid_new_v4();
}

static burrow__Lock v7mu;
static uint64_t v7last_secs;
static uint64_t v7last_timestamp;

/* time.Now as nanoseconds since 1970. Inside a synctest bubble that is the
 * bubble's clock, which starts at midnight on 1 January 2000 the way Go's
 * does, so a UUID made in a bubble carries the bubble's time. */
static int64_t uu_now(void) {
    burrow__Bubble *b = burrow__curbubble();
    return b != NULL ? burrow__bubble_now(b) : pal_clock_realtime();
}

Uuid uuid_new_v7(void) {
    /* RFC 9562 section 5.7 lays version 7 out as 48 bits of Unix milliseconds,
     * 4 bits of version, 12 bits of rand_a, 2 bits of variant and 62 bits of
     * rand_b. rand_a holds a 12 bit fraction of a millisecond, which the RFC
     * allows. */
    burrow__lock(&v7mu);

    /* 48 bits of milliseconds and then 12 bits of 1/4096 of a millisecond. */
    int64_t now = uu_now();
    int64_t s = now / 1000000000;
    int64_t ns = now % 1000000000;
    if (ns < 0) {
        ns += 1000000000;
        s--;
    }
    uint64_t secs = (uint64_t)s;
    uint64_t nanos = (uint64_t)ns;
    uint64_t msecs = nanos / 1000000;
    uint64_t frac = nanos - 1000000 * msecs;
    uint64_t timestamp = (1000 * secs + msecs) << 12;
    timestamp += (frac * 4096) / 1000000;

    if (v7last_secs > secs) {
        /* The clock went backwards, presumably because somebody set it. The
         * UUIDs made before no longer say anything about the order. */
    } else if (timestamp <= v7last_timestamp) {
        /* The same instant as the last one. Stay in order by using the next
         * 1/4096 of a millisecond after it. */
        timestamp = v7last_timestamp + 1;
    }

    v7last_secs = secs;
    v7last_timestamp = timestamp;
    burrow__unlock(&v7mu);

    /* A gap for the 4 version bits. */
    uint64_t hibits =
        ((timestamp << 4) & UINT64_C(0xffffffffffff0000)) | (timestamp & 0x0ffff);

    Uuid u;
    for (int i = 0; i < 8; i++)
        u.b[i] = (Byte)(hibits >> (56 - 8 * i));
    uu_random(u.b + 8, 8);
    uu_set_version(&u, 7);
    uu_set_variant(&u, 2);
    return u;
}

/* ------------------------------------------------------------ descriptor */

#define UU_SIG_STRING(IN, OUT) OUT(Str)

static Slice uu_m_append_text(Uuid *self, Alloc *a, Slice b, Error *err) {
    return uuid_append_text(*self, a, b, err);
}

static Slice uu_m_marshal_text(Uuid *self, Alloc *a, Error *err) {
    return uuid_marshal_text(*self, a, err);
}

/* String has no allocator to take, so its text goes in the goroutine's error
 * arena, as net/netip's does. */
static Str uu_m_string(Uuid *self) {
    return uuid_string(*self, error_allocator());
}

static Error uu_m_unmarshal_text(Uuid *self, Alloc *a, Slice data) {
    (void)a;
    return uuid_unmarshal_text(self, data);
}

#define UU_METHODS(M, T)                                                               \
    M(T, AppendText, uu_m_append_text, ENCODING_SIG_APPEND_TEXT)                       \
    M(T, MarshalText, uu_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                    \
    M(T, String, uu_m_string, UU_SIG_STRING)                                           \
    M(T, UnmarshalText, uu_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(Uuid, UU_METHODS);

const Type burrow_type_Uuid = {
    {(const Byte *)"UUID", 4},
    {(const Byte *)"uuid", 4},
    KIND_ARRAY,
    (uint32_t)sizeof(Uuid),
    (uint16_t)_Alignof(Uuid),
    0,
    (uint16_t)(sizeof(burrow__methods_Uuid) / sizeof(burrow__methods_Uuid[0])),
    NULL,
    burrow__methods_Uuid,
    &burrow_type_uint8_t,
    NULL,
    16,
    0x75756964U, /* "uuid" */
    NULL,
};
