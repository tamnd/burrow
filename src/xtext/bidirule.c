/* Derived from Go's src/vendor/golang.org/x/text/secure/bidirule/bidirule.go.
 * Go source: go1.27.1, golang.org/x/text v0.37.0.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "bidirule.h"
#include "bidi.h"
#include "transform.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(burrow__bidirule_err_invalid, "bidirule: failed Bidi Rule");

enum {
    RULE_INITIAL,
    RULE_LTR,
    RULE_LTR_FINAL,
    RULE_RTL,
    RULE_RTL_FINAL,
    RULE_INVALID,
};

typedef struct BidiruleTransition {
    uint8_t next;
    uint16_t mask;
} BidiruleTransition;

#define BIDIRULE_BIT(c) (1U << (c))

static const BidiruleTransition bidirule_transitions[][2] = {
    /* [2.1] The first character must be a character with Bidi property L, R,
     * or AL. If it has the R or AL property, it is an RTL label; if it has
     * the L property, it is an LTR label. */
    [RULE_INITIAL] =
        {
            {RULE_LTR_FINAL, BIDIRULE_BIT(BIDI_L)},
            {RULE_RTL_FINAL, BIDIRULE_BIT(BIDI_R) | BIDIRULE_BIT(BIDI_AL)},
        },
    [RULE_RTL] =
        {
            /* [2.3] In an RTL label, the end of the label must be a character
             * with Bidi property R, AL, EN, or AN, followed by zero or more
             * characters with Bidi property NSM. */
            {RULE_RTL_FINAL, BIDIRULE_BIT(BIDI_R) | BIDIRULE_BIT(BIDI_AL) |
                                 BIDIRULE_BIT(BIDI_EN) | BIDIRULE_BIT(BIDI_AN)},
            /* [2.2] In an RTL label, only characters with the Bidi properties
             * R, AL, AN, EN, ES, CS, ET, ON, BN, or NSM are allowed. We
             * exclude the entries from [2.3] */
            {RULE_RTL, BIDIRULE_BIT(BIDI_ES) | BIDIRULE_BIT(BIDI_CS) |
                           BIDIRULE_BIT(BIDI_ET) | BIDIRULE_BIT(BIDI_ON) |
                           BIDIRULE_BIT(BIDI_BN) | BIDIRULE_BIT(BIDI_NSM)},
        },
    [RULE_RTL_FINAL] =
        {
            /* [2.3] as above. */
            {RULE_RTL_FINAL, BIDIRULE_BIT(BIDI_R) | BIDIRULE_BIT(BIDI_AL) |
                                 BIDIRULE_BIT(BIDI_EN) | BIDIRULE_BIT(BIDI_AN) |
                                 BIDIRULE_BIT(BIDI_NSM)},
            /* [2.2] We exclude the entries from [2.3] and NSM. */
            {RULE_RTL, BIDIRULE_BIT(BIDI_ES) | BIDIRULE_BIT(BIDI_CS) |
                           BIDIRULE_BIT(BIDI_ET) | BIDIRULE_BIT(BIDI_ON) |
                           BIDIRULE_BIT(BIDI_BN)},
        },
    [RULE_LTR] =
        {
            /* [2.6] In an LTR label, the end of the label must be a character
             * with Bidi property L or EN, followed by zero or more characters
             * with Bidi property NSM. */
            {RULE_LTR_FINAL, BIDIRULE_BIT(BIDI_L) | BIDIRULE_BIT(BIDI_EN)},
            /* [2.5] In an LTR label, only characters with the Bidi properties
             * L, EN, ES, CS, ET, ON, BN, or NSM are allowed. We exclude the
             * entries from [2.6]. */
            {RULE_LTR, BIDIRULE_BIT(BIDI_ES) | BIDIRULE_BIT(BIDI_CS) |
                           BIDIRULE_BIT(BIDI_ET) | BIDIRULE_BIT(BIDI_ON) |
                           BIDIRULE_BIT(BIDI_BN) | BIDIRULE_BIT(BIDI_NSM)},
        },
    [RULE_LTR_FINAL] =
        {
            /* [2.6] as above. */
            {RULE_LTR_FINAL,
             BIDIRULE_BIT(BIDI_L) | BIDIRULE_BIT(BIDI_EN) | BIDIRULE_BIT(BIDI_NSM)},
            /* [2.5] We exclude the entries from [2.6]. */
            {RULE_LTR, BIDIRULE_BIT(BIDI_ES) | BIDIRULE_BIT(BIDI_CS) |
                           BIDIRULE_BIT(BIDI_ET) | BIDIRULE_BIT(BIDI_ON) |
                           BIDIRULE_BIT(BIDI_BN)},
        },
    [RULE_INVALID] =
        {
            {RULE_INVALID, 0},
            {RULE_INVALID, 0},
        },
};

/* [2.4] In an RTL label, if an EN is present, no AN may be present, and vice
 * versa. */
#define BIDIRULE_EXCLUSIVE_RTL                                                         \
    ((uint16_t)(BIDIRULE_BIT(BIDI_EN) | BIDIRULE_BIT(BIDI_AN)))

/* Go's uint16(1 << c), which is 0 for the classes past 15. */
static uint16_t bidirule_class_bit(BidiClass c) {
    return c < 16 ? (uint16_t)(1U << c) : 0;
}

/* Direction. Go's version of this one goes on after an incomplete encoding
 * with the class of the zero Properties, which is L and so changes nothing;
 * DirectionString skips it. Both come to the same answer. */
static BidiDirection bidirule_direction(const Byte *p, Int len) {
    for (Int i = 0; i < len;) {
        Int sz = 0;
        BidiProperties e =
            burrow__bidi_lookup_string(str_from_bytes(p + i, len - i), &sz);
        if (sz == 0) {
            i++;
            continue;
        }
        BidiClass c = burrow__bidi_class(e);
        if (c == BIDI_R || c == BIDI_AL || c == BIDI_AN)
            return BIDI_RIGHT_TO_LEFT;
        i += sz;
    }
    return BIDI_LEFT_TO_RIGHT;
}

BidiDirection burrow__bidirule_direction(Slice b) {
    return bidirule_direction(b.p, b.len);
}

BidiDirection burrow__bidirule_direction_string(Str s) {
    return bidirule_direction(s.p, s.len);
}

/* A rule can only be violated for "Bidi Domain names", meaning if one of the
 * following categories has been observed. */
static bool bidirule_is_rtl(const BidiruleTransformer *t) {
    return (t->seen & (BIDIRULE_BIT(BIDI_R) | BIDIRULE_BIT(BIDI_AL) |
                       BIDIRULE_BIT(BIDI_AN))) != 0;
}

static bool bidirule_is_final(const BidiruleTransformer *t) {
    return t->state == RULE_LTR_FINAL || t->state == RULE_RTL_FINAL ||
           t->state == RULE_INITIAL;
}

/* advance and advanceString, which are the same code in Go. Go keeps a table
 * of the ASCII bytes' properties for speed, which is what Lookup gives for
 * them anyway. */
static Int bidirule_advance(BidiruleTransformer *t, const Byte *s, Int len, bool *ok) {
    Int n = 0;
    while (n < len) {
        Int sz = 0;
        BidiProperties e =
            burrow__bidi_lookup_string(str_from_bytes(s + n, len - n), &sz);
        if (s[n] >= 0x80) {
            if (sz <= 1) {
                /* Invalid UTF-8 always fails, even before the label is known
                 * to be RTL. An incomplete encoding just stops. */
                *ok = sz != 1;
                return n;
            }
        }
        uint16_t c = bidirule_class_bit(burrow__bidi_class(e));
        t->seen |= c;
        if ((t->seen & BIDIRULE_EXCLUSIVE_RTL) == BIDIRULE_EXCLUSIVE_RTL) {
            t->state = RULE_INVALID;
            *ok = false;
            return n;
        }
        const BidiruleTransition *tr = bidirule_transitions[t->state];
        if ((tr[0].mask & c) != 0) {
            t->state = tr[0].next;
        } else if ((tr[1].mask & c) != 0) {
            t->state = tr[1].next;
        } else {
            t->state = RULE_INVALID;
            if (bidirule_is_rtl(t)) {
                *ok = false;
                return n;
            }
        }
        n += sz;
    }
    *ok = true;
    return n;
}

static bool bidirule_valid(const Byte *s, Int len) {
    BidiruleTransformer t = {0};
    bool ok = false;
    Int n = bidirule_advance(&t, s, len, &ok);
    if (!ok || n < len)
        return false;
    return bidirule_is_final(&t);
}

bool burrow__bidirule_valid(Slice b) {
    return bidirule_valid(b.p, b.len);
}

bool burrow__bidirule_valid_string(Str s) {
    return bidirule_valid(s.p, s.len);
}

BidiruleTransformer burrow__bidirule_new(void) {
    BidiruleTransformer t = {0};
    return t;
}

void burrow__bidirule_reset(BidiruleTransformer *t) {
    memset(t, 0, sizeof *t);
}

Int burrow__bidirule_span(BidiruleTransformer *t, Slice src, bool at_eof, Error *err) {
    *err = BURROW_NO_ERROR;
    if (t->state == RULE_INVALID && bidirule_is_rtl(t)) {
        *err = burrow__bidirule_err_invalid;
        return 0;
    }
    bool ok = false;
    Int n = bidirule_advance(t, src.p, src.len, &ok);
    if (n < src.len && ok && !at_eof)
        *err = burrow__transform_err_short_src;
    else if (!ok || n < src.len || !bidirule_is_final(t))
        *err = burrow__bidirule_err_invalid;
    return n;
}

Int burrow__bidirule_transform(BidiruleTransformer *t, Slice dst, Slice src,
                               bool at_eof, Int *n_src, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (dst.len < src.len) {
        src.len = dst.len;
        at_eof = false;
        e = burrow__transform_err_short_dst;
    }
    Error e1;
    Int n = burrow__bidirule_span(t, src, at_eof, &e1);
    if (n > 0)
        memmove(dst.p, src.p, (size_t)n);
    if (!BURROW_FAILED(e) ||
        (BURROW_FAILED(e1) &&
         !burrow__transform_err_eq(e1, burrow__transform_err_short_src)))
        e = e1;
    if (n_src != NULL)
        *n_src = n;
    *err = e;
    return n;
}

static const Type bidirule_transformer_desc = {
    {(const Byte *)"Transformer", 11},
    {(const Byte *)"golang.org/x/text/secure/bidirule", 33},
    KIND_STRUCT,
    (uint32_t)sizeof(BidiruleTransformer),
    (uint16_t)_Alignof(BidiruleTransformer),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int bidirule_transform_method(void *self, Slice dst, Slice src, bool at_eof,
                                     Int *n_src, Error *err) {
    return burrow__bidirule_transform(self, dst, src, at_eof, n_src, err);
}

static void bidirule_reset_method(void *self) {
    burrow__bidirule_reset(self);
}

static Int bidirule_span_method(void *self, Slice src, bool at_eof, Error *err) {
    return burrow__bidirule_span(self, src, at_eof, err);
}

static const TransformSpanningTransformerVT bidirule_vt = {
    {&bidirule_transformer_desc, bidirule_transform_method, bidirule_reset_method},
    bidirule_span_method,
};

TransformSpanningTransformer burrow__bidirule_transformer(BidiruleTransformer *t) {
    TransformSpanningTransformer out = {&bidirule_vt, t};
    return out;
}

#undef BIDIRULE_BIT
