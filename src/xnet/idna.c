/* Derived from Go's src/vendor/golang.org/x/net/idna/idna.go, punycode.go,
 * trie.go and trieval.go.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * Go builds its idna with unicode16 true, so only the Unicode 16 side of each
 * code16 choice and each unicode16 test is here.
 *
 * Everything a conversion makes along the way goes in a scratch arena over a,
 * which takes nothing from a until something is put in it, and the result and
 * the error are copied out into a before the arena goes. An error is kept as a
 * pending record until then, because Go makes it from a label that may only
 * live in the arena.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "idna.h"
#include "idna_tables.h"

#include "../xtext/bidi.h"
#include "../xtext/bidirule.h"
#include "../xtext/norm.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

#define IDNA_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* ------------------------------------------------------------------ trie */

/* sparseBlocks.lookup. */
static uint16_t idna_sparse_lookup(uint32_t n, Byte b) {
    uint16_t offset = burrow__idna_sparse_offset[n];
    IdnaValueRange header = burrow__idna_sparse_values[offset];
    uint16_t lo = (uint16_t)(offset + 1);
    uint16_t hi = (uint16_t)(lo + header.lo);
    while (lo < hi) {
        uint16_t m = (uint16_t)(lo + (hi - lo) / 2);
        IdnaValueRange r = burrow__idna_sparse_values[m];
        if (r.lo <= b && b <= r.hi)
            return (uint16_t)(r.value + (uint16_t)(b - r.lo) * header.value);
        if (b < r.lo)
            hi = m;
        else
            lo = (uint16_t)(m + 1);
    }
    return 0;
}

static uint16_t idna_lookup_value(uint32_t n, Byte b) {
    if (n < IDNA_CUTOFF)
        return burrow__idna_values[(n << 6) + b];
    return idna_sparse_lookup(n - IDNA_CUTOFF, b);
}

/* idnaTrie.lookupString: the value of the first rune of s, which is not
 * empty, and the bytes it takes. An illegal sequence gives 0 and the bytes up
 * to the one that made it illegal, and one cut short gives 0 and 0. */
static uint16_t idna_trie_lookup(Str s, Int *sz) {
    Byte c0 = s.p[0];
    if (c0 < 0x80) {
        *sz = 1;
        return burrow__idna_values[c0];
    }
    if (c0 < 0xC2) {
        *sz = 1;
        return 0;
    }
    Int need = c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : c0 < 0xF8 ? 4 : 0;
    if (need == 0) {
        *sz = 1;
        return 0;
    }
    if (s.len < need) {
        *sz = 0;
        return 0;
    }
    uint32_t i = burrow__idna_index[c0];
    for (Int k = 1; k < need; k++) {
        Byte c = s.p[k];
        if (c < 0x80 || 0xC0 <= c) {
            *sz = k;
            return 0;
        }
        if (k == need - 1) {
            *sz = need;
            return idna_lookup_value(i, c);
        }
        i = burrow__idna_index[(i << 6) + c];
    }
    *sz = 1;
    return 0;
}

uint16_t burrow__idna_lookup_string(Str s, Int *size) {
    return idna_trie_lookup(s, size);
}

/* --------------------------------------------------------------- trieval */

enum {
    IDNA_CAT_SMALL_MASK = 0x3,
    IDNA_CAT_BIG_MASK = 0xF8,
    IDNA_INDEX_SHIFT = 3,
    IDNA_XOR_BIT = 0x4,
    IDNA_INLINE_XOR = 0xE000,

    IDNA_JOIN_SHIFT = 8,
    IDNA_JOIN_MASK = 0x07,

    IDNA_ATTRIBUTES_MASK = 0x1800,
    IDNA_VIRAMA_MODIFIER = 0x1800,
    IDNA_MODIFIER = 0x1000,
    IDNA_RTL = 0x0800,

    IDNA_MAY_NEED_NORM = 0x2000,
};

/* The categories. */
enum {
    IDNA_UNKNOWN = 0,
    IDNA_MAPPED = 1,
    IDNA_DISALLOWED_STD3_MAPPED = 2,
    IDNA_DEVIATION = 3,
    IDNA_VALID = 0x08,
    IDNA_VALID_NV8 = 0x18,
    IDNA_VALID_XV8 = 0x28,
    IDNA_DISALLOWED = 0x40,
    IDNA_DISALLOWED_STD3_VALID = 0x80,
    IDNA_IGNORED = 0xC0,
};

/* The join types, and the ones worked out along the way. */
enum {
    IDNA_JOINING_L = 1,
    IDNA_JOINING_D,
    IDNA_JOINING_T,
    IDNA_JOINING_R,
    IDNA_JOIN_ZWJ,
    IDNA_JOIN_ZWNJ,
    IDNA_JOIN_VIRAMA,
    IDNA_NUM_JOIN_TYPES,
};

static bool idna_is_mapped(uint16_t c) {
    return (c & 0x3) != 0;
}

static unsigned idna_category(uint16_t c) {
    unsigned small = c & IDNA_CAT_SMALL_MASK;
    if (small != 0)
        return small;
    return c & IDNA_CAT_BIG_MASK;
}

static unsigned idna_join_type(uint16_t c) {
    if (idna_is_mapped(c))
        return 0;
    return (unsigned)(c >> IDNA_JOIN_SHIFT) & IDNA_JOIN_MASK;
}

static bool idna_is_modifier(uint16_t c) {
    return (c & (IDNA_MODIFIER | IDNA_CAT_SMALL_MASK)) == IDNA_MODIFIER;
}

static bool idna_is_virama_modifier(uint16_t c) {
    return (c & (IDNA_ATTRIBUTES_MASK | IDNA_CAT_SMALL_MASK)) == IDNA_VIRAMA_MODIFIER;
}

/* info.isBidi, for the rune at the start of s. */
static bool idna_is_bidi(uint16_t c, Str s) {
    if (!idna_is_mapped(c))
        return (c & IDNA_ATTRIBUTES_MASK) == IDNA_RTL;
    Int size;
    BidiClass k = burrow__bidi_class(burrow__bidi_lookup_string(s, &size));
    return k == BIDI_R || k == BIDI_AL || k == BIDI_AN;
}

/* ----------------------------------------------------------------- state */

typedef enum IdnaErrKind {
    IDNA_ERR_NONE,
    IDNA_ERR_LABEL,
    IDNA_ERR_RUNE,
} IdnaErrKind;

/* An error not made yet: Go's labelError or runeError. */
typedef struct IdnaPending {
    IdnaErrKind kind;
    Str code;
    Str label;
    Rune r;
} IdnaPending;

/* One conversion: the scratch arena, and whether it ran out. */
typedef struct IdnaCtx {
    Alloc *sc;
    bool oom;
} IdnaCtx;

static IdnaPending idna_label_error(Str label, Str code) {
    IdnaPending e = {IDNA_ERR_LABEL, code, label, 0};
    return e;
}

static IdnaPending idna_rune_error(Rune r, Str code) {
    IdnaPending e = {IDNA_ERR_RUNE, code, {NULL, 0}, r};
    return e;
}

static bool idna_failed(const IdnaPending *e) {
    return e->kind != IDNA_ERR_NONE;
}

/* A growing byte string in the scratch arena. */
typedef struct IdnaBuf {
    Byte *p;
    Int len, cap;
} IdnaBuf;

static bool idna_buf_reserve(IdnaCtx *c, IdnaBuf *b, Int n) {
    if (b->cap - b->len >= n)
        return true;
    Int cap = b->cap * 2;
    if (cap < b->len + n)
        cap = b->len + n;
    if (cap < 16)
        cap = 16;
    Byte *p = (Byte *)mem_alloc_nozero(c->sc, (size_t)cap, 1);
    if (p == NULL) {
        c->oom = true;
        return false;
    }
    if (b->len > 0)
        memcpy(p, b->p, (size_t)b->len);
    b->p = p;
    b->cap = cap;
    return true;
}

static void idna_buf_put(IdnaCtx *c, IdnaBuf *b, const Byte *p, Int n) {
    if (n <= 0 || !idna_buf_reserve(c, b, n))
        return;
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void idna_buf_put_str(IdnaCtx *c, IdnaBuf *b, Str s) {
    idna_buf_put(c, b, s.p, s.len);
}

static void idna_buf_put_byte(IdnaCtx *c, IdnaBuf *b, Byte x) {
    idna_buf_put(c, b, &x, 1);
}

static void idna_buf_put_rune(IdnaCtx *c, IdnaBuf *b, Rune r) {
    Byte tmp[4];
    Int n = utf8_encode_rune((Slice){tmp, 4, 4, TYPE_BYTE}, r);
    idna_buf_put(c, b, tmp, n);
}

static Str idna_buf_str(const IdnaBuf *b) {
    return (Str){b->p, b->len};
}

static Str idna_sub(Str s, Int from, Int to) {
    return (Str){s.p + from, to - from};
}

static bool idna_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static bool idna_has_prefix(Str s, Str prefix) {
    return s.len >= prefix.len && memcmp(s.p, prefix.p, (size_t)prefix.len) == 0;
}

static bool idna_contains(Str s, Str sub) {
    for (Int i = 0; i + sub.len <= s.len; i++)
        if (memcmp(s.p + i, sub.p, (size_t)sub.len) == 0)
            return true;
    return false;
}

static Int idna_index_byte(Str s, Byte b) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == b)
            return i;
    return -1;
}

static bool idna_is_ascii(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 0x80)
            return false;
    return true;
}

/* norm.NFC.String, with an allocation failure noted. */
static Str idna_nfc(IdnaCtx *c, Str s) {
    Str r = burrow__norm_string(c->sc, NORM_NFC, s);
    if (r.len == 0 && s.len > 0)
        c->oom = true;
    return r;
}

/* --------------------------------------------------------------- punycode */

enum {
    IDNA_BASE = 36,
    IDNA_DAMP = 700,
    IDNA_INITIAL_BIAS = 72,
    IDNA_INITIAL_N = 128,
    IDNA_SKEW = 38,
    IDNA_TMAX = 26,
    IDNA_TMIN = 1,
};

/* Go does this with int32s, which wrap. */
static int32_t idna_wrap_add(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static IdnaPending idna_puny_error(Str s) {
    return idna_label_error(s, IDNA_LIT("P4"));
}

/* madd: a + b*c, and whether that overflows. */
static bool idna_madd(int32_t a, int32_t b, int32_t c, int32_t *next) {
    int64_t p = (int64_t)b * (int64_t)c;
    if (p > (int64_t)INT32_MAX - (int64_t)a) {
        *next = 0;
        return true;
    }
    *next = (int32_t)((uint32_t)a + (uint32_t)(uint64_t)p);
    return false;
}

static bool idna_decode_digit(Byte x, int32_t *digit) {
    if ('0' <= x && x <= '9') {
        *digit = (int32_t)x - ('0' - 26);
        return true;
    }
    if ('A' <= x && x <= 'Z') {
        *digit = (int32_t)x - 'A';
        return true;
    }
    if ('a' <= x && x <= 'z') {
        *digit = (int32_t)x - 'a';
        return true;
    }
    *digit = 0;
    return false;
}

/* encodeDigit. Go panics outside 0 to 35, which encode never asks for. */
static Byte idna_encode_digit(int32_t digit) {
    if (0 <= digit && digit < 26)
        return (Byte)(digit + 'a');
    return (Byte)(digit + ('0' - 26));
}

static int32_t idna_adapt(int32_t delta, int32_t num_points, bool first_time) {
    if (first_time)
        delta /= IDNA_DAMP;
    else
        delta /= 2;
    delta += delta / num_points;
    int32_t k = 0;
    while (delta > ((IDNA_BASE - IDNA_TMIN) * IDNA_TMAX) / 2) {
        delta /= IDNA_BASE - IDNA_TMIN;
        k += IDNA_BASE;
    }
    return k + (IDNA_BASE - IDNA_TMIN + 1) * delta / (delta + IDNA_SKEW);
}

static int32_t idna_threshold(int32_t k, int32_t bias) {
    if (k <= bias)
        return IDNA_TMIN;
    if (k >= bias + IDNA_TMAX)
        return IDNA_TMAX;
    return k - bias;
}

/* decode, section 6.2. The result is in the arena or is part of encoded. */
static Str idna_decode(IdnaCtx *c, Str encoded, IdnaPending *err) {
    if (encoded.len == 0)
        return encoded;
    Int pos = 0;
    for (Int j = encoded.len - 1; j >= 0; j--) {
        if (encoded.p[j] == '-') {
            pos = j + 1;
            break;
        }
    }
    if (pos == 1) {
        *err = idna_puny_error(encoded);
        return (Str){NULL, 0};
    }
    if (pos == encoded.len)
        return idna_sub(encoded, 0, encoded.len - 1);
    Rune *output = (Rune *)mem_alloc_array(c->sc, (size_t)encoded.len, sizeof(Rune),
                                           _Alignof(Rune));
    if (output == NULL) {
        c->oom = true;
        return (Str){NULL, 0};
    }
    Int nout = 0;
    if (pos != 0) {
        Str pre = idna_sub(encoded, 0, pos - 1);
        for (Int j = 0; j < pre.len;) {
            Int size;
            output[nout++] =
                utf8_decode_rune_in_string(idna_sub(pre, j, pre.len), &size);
            j += size;
        }
    }
    int32_t i = 0, n = IDNA_INITIAL_N, bias = IDNA_INITIAL_BIAS;
    while (pos < encoded.len) {
        int32_t old_i = i, w = 1;
        for (int32_t k = IDNA_BASE;; k += IDNA_BASE) {
            if (pos == encoded.len) {
                *err = idna_puny_error(encoded);
                return (Str){NULL, 0};
            }
            int32_t digit;
            if (!idna_decode_digit(encoded.p[pos], &digit)) {
                *err = idna_puny_error(encoded);
                return (Str){NULL, 0};
            }
            pos++;
            if (idna_madd(i, digit, w, &i)) {
                *err = idna_puny_error(encoded);
                return (Str){NULL, 0};
            }
            int32_t t = idna_threshold(k, bias);
            if (digit < t)
                break;
            if (idna_madd(0, w, IDNA_BASE - t, &w)) {
                *err = idna_puny_error(encoded);
                return (Str){NULL, 0};
            }
        }
        if (nout >= 1024) {
            *err = idna_puny_error(encoded);
            return (Str){NULL, 0};
        }
        int32_t x = (int32_t)(nout + 1);
        bias = idna_adapt(i - old_i, x, old_i == 0);
        n = idna_wrap_add(n, i / x);
        i %= x;
        if (n < 0 || n > UTF8_MAX_RUNE) {
            *err = idna_puny_error(encoded);
            return (Str){NULL, 0};
        }
        if (nout > i)
            memmove(output + i + 1, output + i, (size_t)(nout - i) * sizeof(Rune));
        output[i] = n;
        nout++;
        i++;
    }
    IdnaBuf b = {0};
    for (Int j = 0; j < nout; j++)
        idna_buf_put_rune(c, &b, output[j]);
    return idna_buf_str(&b);
}

/* encode, section 6.3, with prefix in front. On an error the result is what
 * Go's is: s for a replacement character in it, and empty otherwise. */
static Str idna_encode(IdnaCtx *c, Str prefix, Str s, IdnaPending *err) {
    IdnaBuf out = {0};
    idna_buf_reserve(c, &out, prefix.len + 1 + 2 * s.len);
    idna_buf_put_str(c, &out, prefix);
    int32_t delta = 0, n = IDNA_INITIAL_N, bias = IDNA_INITIAL_BIAS;
    int32_t b = 0, remaining = 0;
    for (Int j = 0; j < s.len;) {
        Int size;
        Rune r = utf8_decode_rune_in_string(idna_sub(s, j, s.len), &size);
        j += size;
        if (r == 0xFFFD) {
            *err = idna_label_error(s, IDNA_LIT("A3"));
            return s;
        }
        if (r < 0x80) {
            b++;
            idna_buf_put_byte(c, &out, (Byte)r);
        } else {
            remaining++;
        }
    }
    int32_t h = b;
    if (b > 0)
        idna_buf_put_byte(c, &out, '-');
    while (remaining != 0) {
        int32_t m = INT32_MAX;
        for (Int j = 0; j < s.len;) {
            Int size;
            Rune r = utf8_decode_rune_in_string(idna_sub(s, j, s.len), &size);
            j += size;
            if (m > r && r >= n)
                m = r;
        }
        if (idna_madd(delta, m - n, h + 1, &delta)) {
            *err = idna_puny_error(s);
            return (Str){NULL, 0};
        }
        n = m;
        for (Int j = 0; j < s.len;) {
            Int size;
            Rune r = utf8_decode_rune_in_string(idna_sub(s, j, s.len), &size);
            j += size;
            if (r < n) {
                delta = idna_wrap_add(delta, 1);
                if (delta < 0) {
                    *err = idna_puny_error(s);
                    return (Str){NULL, 0};
                }
                continue;
            }
            if (r > n)
                continue;
            int32_t q = delta;
            for (int32_t k = IDNA_BASE;; k += IDNA_BASE) {
                int32_t t = idna_threshold(k, bias);
                if (q < t)
                    break;
                idna_buf_put_byte(c, &out,
                                  idna_encode_digit(t + (q - t) % (IDNA_BASE - t)));
                q = (q - t) / (IDNA_BASE - t);
            }
            idna_buf_put_byte(c, &out, idna_encode_digit(q));
            bias = idna_adapt(delta, h + 1, h == b);
            delta = 0;
            h++;
            remaining--;
        }
        delta = idna_wrap_add(delta, 1);
        n = idna_wrap_add(n, 1);
    }
    return idna_buf_str(&out);
}

/* ---------------------------------------------------------------- profile */

static const Str idna_ace_prefix = {(const Byte *)"xn--", 4};

/* Profile.simplify. */
static unsigned idna_simplify(const IdnaProfile *p, unsigned cat) {
    switch (cat) {
    case IDNA_DISALLOWED_STD3_MAPPED:
        return p->use_std3_rules ? IDNA_DISALLOWED : IDNA_MAPPED;
    case IDNA_DISALLOWED_STD3_VALID:
        return p->use_std3_rules ? IDNA_DISALLOWED : IDNA_VALID;
    case IDNA_DEVIATION:
        return p->transitional ? cat : IDNA_VALID;
    case IDNA_VALID_NV8:
    case IDNA_VALID_XV8:
        return IDNA_VALID;
    default:
        return cat;
    }
}

static bool idna_allowed_std3(Rune r) {
    return r >= 0x80 || ('a' <= r && r <= 'z') || ('0' <= r && r <= '9') || r == '-' ||
           r == '.';
}

/* info.appendMapping. */
static void idna_append_mapping(IdnaCtx *c, IdnaBuf *b, uint16_t v, Str s) {
    uint32_t index = (uint32_t)v >> IDNA_INDEX_SHIFT;
    if ((v & IDNA_XOR_BIT) == 0) {
        uint16_t from = burrow__idna_mapping_index[index];
        uint16_t to = burrow__idna_mapping_index[index + 1];
        idna_buf_put(c, b, burrow__idna_mappings + from, (Int)to - (Int)from);
        return;
    }
    Int start = b->len;
    idna_buf_put_str(c, b, s);
    if (s.len == 0 || b->len != start + s.len)
        return;
    if ((v & IDNA_INLINE_XOR) == IDNA_INLINE_XOR) {
        b->p[b->len - 1] ^= (Byte)index;
    } else {
        for (Int q = b->len - (Int)burrow__idna_xor_data[index]; q < b->len; q++) {
            index++;
            b->p[q] ^= burrow__idna_xor_data[index];
        }
    }
}

/* normalize. */
static Str idna_normalize(IdnaCtx *c, Str s, bool *is_bidi) {
    Str mapped = idna_nfc(c, s);
    *is_bidi = burrow__bidirule_direction_string(mapped) == BIDI_RIGHT_TO_LEFT;
    return mapped;
}

/* validateRegistration. */
static Str idna_validate_registration(const IdnaProfile *p, Str s, bool *bidi,
                                      IdnaPending *err) {
    *bidi = false;
    if (!burrow__norm_is_normal_string(NORM_NFC, s)) {
        *err = idna_label_error(s, IDNA_LIT("V1"));
        return s;
    }
    for (Int i = 0; i < s.len;) {
        Int sz;
        uint16_t v = idna_trie_lookup(idna_sub(s, i, s.len), &sz);
        if (sz == 0) {
            *err = idna_rune_error(UTF8_RUNE_ERROR, IDNA_LIT("P1"));
            return s;
        }
        *bidi = *bidi || idna_is_bidi(v, idna_sub(s, i, s.len));
        switch (idna_simplify(p, idna_category(v))) {
        case IDNA_VALID:
        case IDNA_DEVIATION:
            if (sz == 1 && p->use_std3_rules && !idna_allowed_std3((Rune)s.p[i])) {
                *err = idna_rune_error((Rune)s.p[i], IDNA_LIT("P1"));
                return s;
            }
            break;
        case IDNA_DISALLOWED:
        case IDNA_MAPPED:
        case IDNA_UNKNOWN:
        case IDNA_IGNORED: {
            Int size;
            Rune r = utf8_decode_rune_in_string(idna_sub(s, i, s.len), &size);
            *err = idna_rune_error(r, IDNA_LIT("P1"));
            return s;
        }
        default:
            break;
        }
        i += sz;
    }
    return s;
}

/* validateAndMap. */
static Str idna_validate_and_map(IdnaCtx *c, const IdnaProfile *p, Str s, bool *bidi,
                                 IdnaPending *err) {
    static const Byte fffd[] = {0xEF, 0xBF, 0xBD};
    static const Byte sharp_s[] = {0xE1, 0xBA, 0x9E}; /* U+1E9E */
    IdnaBuf b = {0};
    Int k = 0;
    uint16_t combined = 0;
    *bidi = false;
    for (Int i = 0; i < s.len;) {
        Int sz;
        uint16_t v = idna_trie_lookup(idna_sub(s, i, s.len), &sz);
        if (sz == 0) {
            idna_buf_put_str(c, &b, idna_sub(s, k, i));
            idna_buf_put(c, &b, fffd, 3);
            k = s.len;
            if (!idna_failed(err))
                *err = idna_rune_error(UTF8_RUNE_ERROR, IDNA_LIT("P1"));
            break;
        }
        combined |= v;
        *bidi = *bidi || idna_is_bidi(v, idna_sub(s, i, s.len));
        Int start = i;
        i += sz;
        switch (idna_simplify(p, idna_category(v))) {
        case IDNA_VALID:
        case IDNA_DISALLOWED:
            /* Unicode 16 leaves a disallowed rune to validateLabel. */
            continue;
        case IDNA_DEVIATION:
        case IDNA_MAPPED:
            /* A deviation is valid here unless the profile is transitional,
             * and simplify has made it valid already when it is not. */
            idna_buf_put_str(c, &b, idna_sub(s, k, start));
            if (p->transitional && sz == 3 && memcmp(s.p + start, sharp_s, 3) == 0)
                idna_buf_put_str(c, &b, IDNA_LIT("ss"));
            else
                idna_append_mapping(c, &b, v, idna_sub(s, start, i));
            break;
        case IDNA_IGNORED:
            idna_buf_put_str(c, &b, idna_sub(s, k, start));
            break;
        case IDNA_UNKNOWN:
            idna_buf_put_str(c, &b, idna_sub(s, k, start));
            idna_buf_put(c, &b, fffd, 3);
            break;
        default:
            break;
        }
        k = i;
    }
    if (k == 0) {
        if ((combined & IDNA_MAY_NEED_NORM) != 0)
            s = idna_nfc(c, s);
        return s;
    }
    idna_buf_put_str(c, &b, idna_sub(s, k, s.len));
    if (c->oom)
        return s;
    Slice bs = {b.p, b.len, b.cap, TYPE_BYTE};
    if (burrow__norm_quick_span(NORM_NFC, bs) != b.len) {
        Slice nb = burrow__norm_bytes(c->sc, NORM_NFC, bs);
        if (nb.len == 0 && b.len > 0) {
            c->oom = true;
            return s;
        }
        return (Str){(const Byte *)nb.p, nb.len};
    }
    return idna_buf_str(&b);
}

/* validateFromPunycode. */
static void idna_validate_from_punycode(const IdnaProfile *p, Str s, IdnaPending *err) {
    if (!burrow__norm_is_normal_string(NORM_NFC, s)) {
        *err = idna_label_error(s, IDNA_LIT("V1"));
        return;
    }
    for (Int i = 0; i < s.len;) {
        Int sz;
        uint16_t v = idna_trie_lookup(idna_sub(s, i, s.len), &sz);
        if (sz == 0) {
            *err = idna_rune_error(UTF8_RUNE_ERROR, IDNA_LIT("P1"));
            return;
        }
        unsigned k = idna_simplify(p, idna_category(v));
        if (k != IDNA_VALID && k != IDNA_DEVIATION) {
            *err = idna_label_error(s, IDNA_LIT("V7"));
            return;
        }
        i += sz;
    }
}

enum {
    IDNA_STATE_START,
    IDNA_STATE_VIRAMA,
    IDNA_STATE_BEFORE,
    IDNA_STATE_BEFORE_VIRAMA,
    IDNA_STATE_AFTER,
    IDNA_STATE_FAIL,
};

/* joinStates. What Go leaves out of a row is stateStart. */
static const uint8_t idna_join_states[6][IDNA_NUM_JOIN_TYPES] = {
    [IDNA_STATE_START] =
        {
            [IDNA_JOINING_L] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_D] = IDNA_STATE_BEFORE,
            [IDNA_JOIN_ZWNJ] = IDNA_STATE_FAIL,
            [IDNA_JOIN_ZWJ] = IDNA_STATE_FAIL,
            [IDNA_JOIN_VIRAMA] = IDNA_STATE_VIRAMA,
        },
    [IDNA_STATE_VIRAMA] =
        {
            [IDNA_JOINING_L] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_D] = IDNA_STATE_BEFORE,
        },
    [IDNA_STATE_BEFORE] =
        {
            [IDNA_JOINING_L] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_D] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_T] = IDNA_STATE_BEFORE,
            [IDNA_JOIN_ZWNJ] = IDNA_STATE_AFTER,
            [IDNA_JOIN_ZWJ] = IDNA_STATE_FAIL,
            [IDNA_JOIN_VIRAMA] = IDNA_STATE_BEFORE_VIRAMA,
        },
    [IDNA_STATE_BEFORE_VIRAMA] =
        {
            [IDNA_JOINING_L] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_D] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_T] = IDNA_STATE_BEFORE,
        },
    [IDNA_STATE_AFTER] =
        {
            [IDNA_JOINING_L] = IDNA_STATE_FAIL,
            [IDNA_JOINING_D] = IDNA_STATE_BEFORE,
            [IDNA_JOINING_T] = IDNA_STATE_AFTER,
            [IDNA_JOINING_R] = IDNA_STATE_START,
            [IDNA_JOIN_ZWNJ] = IDNA_STATE_FAIL,
            [IDNA_JOIN_ZWJ] = IDNA_STATE_FAIL,
            [IDNA_JOIN_VIRAMA] = IDNA_STATE_AFTER,
        },
    [IDNA_STATE_FAIL] =
        {
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
            IDNA_STATE_FAIL,
        },
};

/* Profile.validateLabel, the criteria of section 4.1. */
static void idna_validate_label(const IdnaProfile *p, Str s, Str label_code,
                                IdnaPending *err) {
    static const Str zwj = {(const Byte *)"\xE2\x80\x8D", 3};
    static const Str zwnj = {(const Byte *)"\xE2\x80\x8C", 3};
    if (s.len == 0) {
        if (p->verify_dns_length)
            *err = idna_label_error(s, label_code);
        return;
    }
    if (p->check_hyphens) {
        if (s.len > 4 && s.p[2] == '-' && s.p[3] == '-') {
            *err = idna_label_error(s, IDNA_LIT("V2"));
            return;
        }
        if (s.p[0] == '-' || s.p[s.len - 1] == '-') {
            *err = idna_label_error(s, IDNA_LIT("V3"));
            return;
        }
    }

    /* Unicode 16 checks the runes here, after the label is decoded. */
    if (p->from_puny) {
        for (Int i = 0; i < s.len;) {
            Int sz;
            uint16_t v = idna_trie_lookup(idna_sub(s, i, s.len), &sz);
            if (sz == 0) {
                *err = idna_rune_error(UTF8_RUNE_ERROR, IDNA_LIT("P1"));
                return;
            }
            unsigned k = idna_simplify(p, idna_category(v));
            if (k != IDNA_VALID && (!p->transitional || k != IDNA_DEVIATION)) {
                *err = idna_label_error(s, IDNA_LIT("V7"));
                return;
            }
            if (sz == 1 && p->use_std3_rules && !idna_allowed_std3((Rune)s.p[i])) {
                *err = idna_rune_error((Rune)s.p[i], IDNA_LIT("U1"));
                return;
            }
            i += sz;
        }
    }

    if (!p->check_joiners)
        return;
    Int sz;
    uint16_t x = idna_trie_lookup(s, &sz);
    if (idna_is_modifier(x)) {
        *err = idna_label_error(s, IDNA_LIT("V6"));
        return;
    }
    if (!idna_contains(s, zwj) && !idna_contains(s, zwnj))
        return;
    unsigned st = IDNA_STATE_START;
    for (Int i = 0;;) {
        unsigned jt = idna_join_type(x);
        Str r = idna_sub(s, i, i + sz);
        if (idna_str_eq(r, zwj))
            jt = IDNA_JOIN_ZWJ;
        else if (idna_str_eq(r, zwnj))
            jt = IDNA_JOIN_ZWNJ;
        st = idna_join_states[st][jt];
        if (idna_is_virama_modifier(x))
            st = idna_join_states[st][IDNA_JOIN_VIRAMA];
        i += sz;
        if (i == s.len)
            break;
        x = idna_trie_lookup(idna_sub(s, i, s.len), &sz);
    }
    if (st == IDNA_STATE_FAIL || st == IDNA_STATE_AFTER)
        *err = idna_label_error(s, IDNA_LIT("C"));
}

/* ----------------------------------------------------------- label iterator */

/* labelIter. slice, once set, holds the labels split out of orig. */
typedef struct IdnaLabels {
    Str orig;
    Str *slice;
    Int nslice;
    Int cur_start, cur_end, i;
} IdnaLabels;

static void idna_labels_reset(IdnaLabels *l) {
    l->cur_start = 0;
    l->cur_end = 0;
    l->i = 0;
}

static bool idna_labels_done(const IdnaLabels *l) {
    return l->cur_start >= l->orig.len;
}

static Str idna_labels_label(IdnaLabels *l) {
    if (l->slice != NULL)
        return l->slice[l->i];
    Int p = idna_index_byte(idna_sub(l->orig, l->cur_start, l->orig.len), '.');
    l->cur_end = p == -1 ? l->orig.len : l->cur_start + p;
    return idna_sub(l->orig, l->cur_start, l->cur_end);
}

static void idna_labels_next(IdnaLabels *l) {
    l->i++;
    if (l->slice != NULL) {
        if (l->i >= l->nslice || (l->i == l->nslice - 1 && l->slice[l->i].len == 0))
            l->cur_start = l->orig.len;
    } else {
        l->cur_start = l->cur_end + 1;
        if (l->cur_start == l->orig.len - 1 && l->orig.p[l->cur_start] == '.')
            l->cur_start = l->orig.len;
    }
}

static void idna_labels_set(IdnaCtx *c, IdnaLabels *l, Str s) {
    if (l->slice == NULL) {
        Int n = 1;
        for (Int j = 0; j < l->orig.len; j++)
            if (l->orig.p[j] == '.')
                n++;
        Str *v = (Str *)mem_alloc_array(c->sc, (size_t)n, sizeof(Str), _Alignof(Str));
        if (v == NULL) {
            c->oom = true;
            return;
        }
        Int from = 0, k = 0;
        for (Int j = 0; j <= l->orig.len; j++) {
            if (j == l->orig.len || l->orig.p[j] == '.') {
                v[k++] = idna_sub(l->orig, from, j);
                from = j + 1;
            }
        }
        l->slice = v;
        l->nslice = n;
    }
    l->slice[l->i] = s;
}

static Str idna_labels_result(IdnaCtx *c, const IdnaLabels *l) {
    if (l->slice == NULL)
        return l->orig;
    IdnaBuf b = {0};
    for (Int j = 0; j < l->nslice; j++) {
        if (j > 0)
            idna_buf_put_byte(c, &b, '.');
        idna_buf_put_str(c, &b, l->slice[j]);
    }
    return idna_buf_str(&b);
}

/* ---------------------------------------------------------------- process */

/* Profile.process, the algorithm of section 4 of UTS #46. */
static Str idna_process(IdnaCtx *c, const IdnaProfile *p, Str s, bool to_ascii,
                        IdnaPending *err) {
    bool is_bidi = false;
    switch (p->mapping) {
    case IDNA_MAP_NORMALIZE:
        s = idna_normalize(c, s, &is_bidi);
        break;
    case IDNA_MAP_VALIDATE_AND_MAP:
        s = idna_validate_and_map(c, p, s, &is_bidi, err);
        break;
    case IDNA_MAP_VALIDATE_REGISTRATION:
        s = idna_validate_registration(p, s, &is_bidi, err);
        break;
    case IDNA_MAP_NONE:
    default:
        break;
    }
    if (c->oom)
        return s;
    if (p->remove_leading_dots)
        while (s.len > 0 && s.p[0] == '.')
            s = idna_sub(s, 1, s.len);
    Str label_code = to_ascii ? IDNA_LIT("A4") : IDNA_LIT("X4_2");
    if (!idna_failed(err) && p->verify_dns_length && s.len == 0)
        *err = idna_label_error(s, label_code);

    IdnaLabels labels = {s, NULL, 0, 0, 0, 0};
    for (; !idna_labels_done(&labels); idna_labels_next(&labels)) {
        Str label = idna_labels_label(&labels);
        if (label.len == 0) {
            /* The iterator skips the last label when it is empty. */
            if (!idna_failed(err) && p->verify_dns_length)
                *err = idna_label_error(s, label_code);
            continue;
        }
        if (idna_has_prefix(label, idna_ace_prefix)) {
            Str enc = idna_sub(label, idna_ace_prefix.len, label.len);
            IdnaPending err2 = {IDNA_ERR_NONE, {NULL, 0}, {NULL, 0}, 0};
            Str u = idna_decode(c, enc, &err2);
            if (c->oom)
                return s;
            if (idna_failed(&err2)) {
                if (!idna_failed(err))
                    *err = err2;
                /* The spec says to keep the old label. */
                continue;
            }
            if (!idna_failed(err) && u.len > 0 && idna_is_ascii(u))
                *err = idna_puny_error(enc);
            is_bidi =
                is_bidi || burrow__bidirule_direction_string(u) != BIDI_LEFT_TO_RIGHT;
            idna_labels_set(c, &labels, u);
            if (c->oom)
                return s;
            if (!idna_failed(err) && p->from_puny)
                idna_validate_from_punycode(p, u, err);
            if (!idna_failed(err))
                idna_validate_label(p, u, label_code, err);
        } else if (!idna_failed(err)) {
            idna_validate_label(p, label, label_code, err);
        }
    }
    if (is_bidi && p->bidirule && !idna_failed(err)) {
        for (idna_labels_reset(&labels); !idna_labels_done(&labels);
             idna_labels_next(&labels)) {
            if (!burrow__bidirule_valid_string(idna_labels_label(&labels))) {
                *err = idna_label_error(s, IDNA_LIT("B"));
                break;
            }
        }
    }
    if (to_ascii) {
        for (idna_labels_reset(&labels); !idna_labels_done(&labels);
             idna_labels_next(&labels)) {
            Str label = idna_labels_label(&labels);
            if (!idna_is_ascii(label)) {
                IdnaPending err2 = {IDNA_ERR_NONE, {NULL, 0}, {NULL, 0}, 0};
                Str a = idna_encode(c, idna_ace_prefix, label, &err2);
                if (c->oom)
                    return s;
                if (!idna_failed(err))
                    *err = err2;
                label = a;
                idna_labels_set(c, &labels, a);
                if (c->oom)
                    return s;
            }
            Int n = label.len;
            if (p->verify_dns_length && !idna_failed(err) && (n == 0 || n > 63))
                *err = idna_label_error(label, label_code);
        }
    }
    s = idna_labels_result(c, &labels);
    if (c->oom)
        return s;
    if (to_ascii && p->verify_dns_length && !idna_failed(err)) {
        if (s.len > 0 && s.p[s.len - 1] == '.')
            *err = idna_label_error(s, label_code);
        /* The length of the name without the root label and its dot. */
        Int n = s.len;
        if (n > 0 && s.p[n - 1] == '.')
            n--;
        if (s.len < 1 || n > 253)
            *err = idna_label_error(s, label_code);
    }
    return s;
}

/* ----------------------------------------------------------------- errors */

/* The error, with its code, and its message after it in the same block. */
typedef struct IdnaErrorBox {
    Str code;
    Str message;
} IdnaErrorBox;

static Str idna_error_message(const void *self) {
    return ((const IdnaErrorBox *)self)->message;
}

static Error idna_error_clone(const void *self, Alloc *a);

/* No self_type: Go's labelError and runeError are unexported. */
static const ErrorVT idna_error_vt = {
    NULL, idna_error_message, NULL, NULL, NULL, NULL, idna_error_clone,
};

/* A box for code and message, both copied. */
static Error idna_error_build(Alloc *a, Str code, Str message) {
    size_t size = sizeof(IdnaErrorBox) + (size_t)code.len + (size_t)message.len;
    IdnaErrorBox *b = (IdnaErrorBox *)mem_alloc_nozero(a, size, _Alignof(IdnaErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *q = (Byte *)(b + 1);
    memcpy(q, code.p, (size_t)code.len);
    b->code = (Str){q, code.len};
    q += code.len;
    if (message.len > 0)
        memcpy(q, message.p, (size_t)message.len);
    b->message = (Str){q, message.len};
    return (Error){&idna_error_vt, b};
}

static Error idna_error_clone(const void *self, Alloc *a) {
    const IdnaErrorBox *b = (const IdnaErrorBox *)self;
    return idna_error_build(a, b->code, b->message);
}

static const char idna_hex[] = "0123456789ABCDEF";

/* The error e records, in a: "idna: invalid label %q" or
 * "idna: disallowed rune %U". */
static Error idna_error_make(Alloc *a, const IdnaPending *e) {
    if (e->kind == IDNA_ERR_LABEL) {
        Str head = IDNA_LIT("idna: invalid label ");
        Int n = head.len + burrow__strconv_quote_into(NULL, e->label);
        size_t size = sizeof(IdnaErrorBox) + (size_t)e->code.len + (size_t)n;
        IdnaErrorBox *b =
            (IdnaErrorBox *)mem_alloc_nozero(a, size, _Alignof(IdnaErrorBox));
        if (b == NULL)
            return burrow_err_out_of_memory;
        Byte *q = (Byte *)(b + 1);
        memcpy(q, e->code.p, (size_t)e->code.len);
        b->code = (Str){q, e->code.len};
        q += e->code.len;
        memcpy(q, head.p, (size_t)head.len);
        burrow__strconv_quote_into(q + head.len, e->label);
        b->message = (Str){q, n};
        return (Error){&idna_error_vt, b};
    }
    /* %U: U+ and at least four hex digits. A rune here is never negative. */
    Byte msg[40];
    Str head = IDNA_LIT("idna: disallowed rune U+");
    memcpy(msg, head.p, (size_t)head.len);
    Int n = head.len;
    uint32_t r = (uint32_t)e->r;
    int digits = 4;
    while (digits < 8 && (r >> (4 * digits)) != 0)
        digits++;
    for (int d = digits - 1; d >= 0; d--)
        msg[n++] = (Byte)idna_hex[(r >> (4 * d)) & 0xF];
    return idna_error_build(a, e->code, (Str){msg, n});
}

Str burrow__idna_error_code(Error err) {
    if (err.vt != &idna_error_vt)
        return (Str){NULL, 0};
    return ((const IdnaErrorBox *)err.data)->code;
}

/* ------------------------------------------------------------------ API */

/* The result into a, unless it is s or a part of it, and the error with it.
 * Then the arena goes. */
static Str idna_finish(Alloc *a, Arena *scratch, const IdnaCtx *c, Str s, Str r,
                       const IdnaPending *e, Error *err) {
    Str out = {NULL, 0};
    Error result = BURROW_NO_ERROR;
    if (c->oom) {
        result = burrow_err_out_of_memory;
    } else {
        bool inside =
            r.p != NULL && s.p != NULL && r.p >= s.p && r.p + r.len <= s.p + s.len;
        /* An empty result that is not part of s may point into the arena, so
         * it stays the zero Str. */
        if (inside) {
            out = r;
        } else if (r.len > 0 && r.p != NULL) {
            Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)r.len, 1);
            if (q == NULL) {
                result = burrow_err_out_of_memory;
            } else {
                memcpy(q, r.p, (size_t)r.len);
                out = (Str){q, r.len};
            }
        }
        if (BURROW_OK(result) && idna_failed(e)) {
            result = idna_error_make(a, e);
            if (errors_is(result, burrow_err_out_of_memory))
                out = (Str){NULL, 0};
        }
    }
    arena_free(scratch);
    if (err != NULL)
        *err = result;
    return out;
}

static Str idna_run(Alloc *a, const IdnaProfile *p, Str s, bool to_ascii, Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    IdnaCtx c = {arena_allocator(&scratch), false};
    IdnaPending e = {IDNA_ERR_NONE, {NULL, 0}, {NULL, 0}, 0};
    Str r = idna_process(&c, p, s, to_ascii, &e);
    return idna_finish(a, &scratch, &c, s, r, &e, err);
}

Str burrow__idna_profile_to_ascii(Alloc *a, const IdnaProfile *p, Str s, Error *err) {
    return idna_run(a, p, s, true, err);
}

Str burrow__idna_profile_to_unicode(Alloc *a, const IdnaProfile *p, Str s, Error *err) {
    IdnaProfile pp = *p;
    pp.transitional = false;
    return idna_run(a, &pp, s, false, err);
}

Str burrow__idna_to_ascii(Alloc *a, Str s, Error *err) {
    return idna_run(a, &burrow__idna_punycode, s, true, err);
}

Str burrow__idna_to_unicode(Alloc *a, Str s, Error *err) {
    return idna_run(a, &burrow__idna_punycode, s, false, err);
}

Str burrow__idna_punycode_decode(Alloc *a, Str s, Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    IdnaCtx c = {arena_allocator(&scratch), false};
    IdnaPending e = {IDNA_ERR_NONE, {NULL, 0}, {NULL, 0}, 0};
    Str r = idna_decode(&c, s, &e);
    return idna_finish(a, &scratch, &c, s, r, &e, err);
}

Str burrow__idna_punycode_encode(Alloc *a, Str prefix, Str s, Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    IdnaCtx c = {arena_allocator(&scratch), false};
    IdnaPending e = {IDNA_ERR_NONE, {NULL, 0}, {NULL, 0}, 0};
    Str r = idna_encode(&c, prefix, s, &e);
    return idna_finish(a, &scratch, &c, s, r, &e, err);
}

Str burrow__idna_profile_string(Alloc *a, const IdnaProfile *p) {
    Str parts[5];
    Int n = 0;
    Str head = p->transitional ? IDNA_LIT("Transitional") : IDNA_LIT("NonTransitional");
    if (p->use_std3_rules)
        parts[n++] = IDNA_LIT(":UseSTD3Rules");
    if (p->check_hyphens)
        parts[n++] = IDNA_LIT(":CheckHyphens");
    if (p->check_joiners)
        parts[n++] = IDNA_LIT(":CheckJoiners");
    if (p->verify_dns_length)
        parts[n++] = IDNA_LIT(":VerifyDNSLength");
    Int len = head.len;
    for (Int i = 0; i < n; i++)
        len += parts[i].len;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
    if (q == NULL)
        return (Str){NULL, 0};
    memcpy(q, head.p, (size_t)head.len);
    Int at = head.len;
    for (Int i = 0; i < n; i++) {
        memcpy(q + at, parts[i].p, (size_t)parts[i].len);
        at += parts[i].len;
    }
    return (Str){q, len};
}

/* ---------------------------------------------------------------- options */

static IdnaOption idna_option(IdnaOptionKind kind, bool enable) {
    IdnaOption o = {kind, enable};
    return o;
}

IdnaOption burrow__idna_transitional(bool transitional) {
    return idna_option(IDNA_OPT_TRANSITIONAL, transitional);
}

IdnaOption burrow__idna_verify_dns_length(bool verify) {
    return idna_option(IDNA_OPT_VERIFY_DNS_LENGTH, verify);
}

IdnaOption burrow__idna_remove_leading_dots(bool remove) {
    return idna_option(IDNA_OPT_REMOVE_LEADING_DOTS, remove);
}

IdnaOption burrow__idna_validate_labels(bool enable) {
    return idna_option(IDNA_OPT_VALIDATE_LABELS, enable);
}

IdnaOption burrow__idna_check_hyphens(bool enable) {
    return idna_option(IDNA_OPT_CHECK_HYPHENS, enable);
}

IdnaOption burrow__idna_check_joiners(bool enable) {
    return idna_option(IDNA_OPT_CHECK_JOINERS, enable);
}

IdnaOption burrow__idna_strict_domain_name(bool use) {
    return idna_option(IDNA_OPT_STRICT_DOMAIN_NAME, use);
}

IdnaOption burrow__idna_bidi_rule(void) {
    return idna_option(IDNA_OPT_BIDI_RULE, true);
}

IdnaOption burrow__idna_validate_for_registration(void) {
    return idna_option(IDNA_OPT_VALIDATE_FOR_REGISTRATION, true);
}

IdnaOption burrow__idna_map_for_lookup(void) {
    return idna_option(IDNA_OPT_MAP_FOR_LOOKUP, true);
}

/* ValidateLabels. It leaves a mapping alone, but sets normalize when there is
 * none and it is turning the checks on. */
static void idna_apply_validate_labels(IdnaProfile *o, bool enable) {
    if (o->mapping == IDNA_MAP_NONE && enable)
        o->mapping = IDNA_MAP_NORMALIZE;
    o->trie = true;
    o->check_joiners = enable;
    o->check_hyphens = enable;
    o->from_puny = enable;
}

static void idna_apply(IdnaProfile *o, IdnaOption opt) {
    switch (opt.kind) {
    case IDNA_OPT_TRANSITIONAL:
        o->transitional = opt.enable;
        break;
    case IDNA_OPT_VERIFY_DNS_LENGTH:
        o->verify_dns_length = opt.enable;
        break;
    case IDNA_OPT_REMOVE_LEADING_DOTS:
        o->remove_leading_dots = opt.enable;
        break;
    case IDNA_OPT_VALIDATE_LABELS:
        idna_apply_validate_labels(o, opt.enable);
        break;
    case IDNA_OPT_CHECK_HYPHENS:
        o->check_hyphens = opt.enable;
        break;
    case IDNA_OPT_CHECK_JOINERS:
        o->trie = true;
        o->check_joiners = opt.enable;
        break;
    case IDNA_OPT_STRICT_DOMAIN_NAME:
        o->use_std3_rules = opt.enable;
        break;
    case IDNA_OPT_BIDI_RULE:
        o->bidirule = true;
        break;
    case IDNA_OPT_VALIDATE_FOR_REGISTRATION:
        o->mapping = IDNA_MAP_VALIDATE_REGISTRATION;
        o->use_std3_rules = true;
        idna_apply_validate_labels(o, true);
        o->verify_dns_length = true;
        o->bidirule = true;
        break;
    case IDNA_OPT_MAP_FOR_LOOKUP:
        o->mapping = IDNA_MAP_VALIDATE_AND_MAP;
        o->use_std3_rules = true;
        idna_apply_validate_labels(o, true);
        break;
    default:
        break;
    }
}

IdnaProfile burrow__idna_new(const IdnaOption *opts, Int n) {
    IdnaProfile p = {0};
    for (Int i = 0; i < n; i++)
        idna_apply(&p, opts[i]);
    return p;
}

/* Transitional processing has been off by default since Go 1.18
 * (golang.org/issue/47510). */
const IdnaProfile burrow__idna_punycode = {0};

const IdnaProfile burrow__idna_lookup = {
    false, true, true, true, false, false, true, true, IDNA_MAP_VALIDATE_AND_MAP, true,
};

const IdnaProfile burrow__idna_display = {
    false, true, true, true, false, false, true, true, IDNA_MAP_VALIDATE_AND_MAP, true,
};

const IdnaProfile burrow__idna_registration = {
    false, true, true, true, true, false, true, true, IDNA_MAP_VALIDATE_REGISTRATION,
    true,
};
