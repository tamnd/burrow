/* golang.org/x/text/secure/bidirule, the copy Go vendors.
 *
 * The Bidi Rule of RFC 5893, which says whether a domain name label that mixes
 * directions can be shown without being misread. The IDNA code checks every
 * label with it, which is why Go's standard library carries it.
 *
 * Go's New returns a pointer. Here the Transformer is a value the caller keeps,
 * zeroed or set up with burrow__bidirule_new, and burrow__bidirule_transformer
 * wraps a pointer to it as a transform.SpanningTransformer.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xtext/secure/bidirule */

#ifndef BURROW_SRC_XTEXT_BIDIRULE_H
#define BURROW_SRC_XTEXT_BIDIRULE_H

#include "bidi.h"
#include "transform.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"

#include <stdint.h>

/* ErrInvalid: the label breaks the rule. */
extern const Error burrow__bidirule_err_invalid;

/* Direction and DirectionString: right to left when the label has a rune of
 * class R, AL or AN, and left to right otherwise. */
BidiDirection burrow__bidirule_direction(Slice b);
BidiDirection burrow__bidirule_direction_string(Str s);

/* Valid and ValidString. */
bool burrow__bidirule_valid(Slice b);
bool burrow__bidirule_valid_string(Str s);

/* The Transformer. It has state, so reset it between labels. */
typedef struct BidiruleTransformer {
    uint8_t state;
    bool has_rtl;
    uint16_t seen;
} BidiruleTransformer;

BidiruleTransformer burrow__bidirule_new(void);
void burrow__bidirule_reset(BidiruleTransformer *t);
Int burrow__bidirule_transform(BidiruleTransformer *t, Slice dst, Slice src,
                               bool at_eof, Int *n_src, Error *err);
Int burrow__bidirule_span(BidiruleTransformer *t, Slice src, bool at_eof, Error *err);

/* t as a transform.SpanningTransformer, borrowing it. */
BURROW_BORROWS(ret, t) TransformSpanningTransformer
burrow__bidirule_transformer(BidiruleTransformer *t);

#endif /* BURROW_SRC_XTEXT_BIDIRULE_H */
