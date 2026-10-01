/* What asn1.c has inside, for tests/asn1_test.c, which reaches into the pieces
 * the way Go's tests reach into the unexported ones they are named after.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_ASN1_INTERNAL_H
#define BURROW_SRC_ENCODING_ASN1_INTERNAL_H

#include "burrow/encoding/asn1.h"

#include "burrow/core.h"
#include "burrow/math/big.h"
#include "burrow/time.h"

#include <stdint.h>

/* tagAndLength. */
typedef struct Asn1TagAndLength {
    Int cls;
    Int tag;
    Int length;
    bool is_compound;
} Asn1TagAndLength;

/* fieldParameters. Go's two pointers are a flag and a value each here. */
typedef struct Asn1FieldParameters {
    bool optional;
    bool is_explicit;
    bool application;
    bool is_private;
    bool has_default;
    int64_t default_value;
    bool has_tag;
    Int tag;
    Int string_type;
    Int time_type;
    bool set;
    bool omit_empty;
} Asn1FieldParameters;

Asn1FieldParameters burrow__asn1_parse_field_parameters(Str s);

/* parseTagAndLength: the offset after the tag and length, with them in *ret. */
Int burrow__asn1_parse_tag_and_length(Slice b, Int offset, Asn1TagAndLength *ret,
                                      Error *err);

bool burrow__asn1_parse_bool(Slice b, Error *err);
int64_t burrow__asn1_parse_int64(Slice b, Error *err);
int32_t burrow__asn1_parse_int32(Slice b, Error *err);
BURROW_OWNS(ret) BigInt *burrow__asn1_parse_big_int(Alloc *a, Slice b, Error *err);
BURROW_BORROWS(ret, b) Asn1BitString burrow__asn1_parse_bit_string(Slice b, Error *err);
BURROW_OWNS(ret) Asn1ObjectIdentifier burrow__asn1_parse_object_identifier(Alloc *a,
                                                                           Slice b,
                                                                           Error *err);
Time burrow__asn1_parse_utc_time(Alloc *a, Slice b, Error *err);
Time burrow__asn1_parse_generalized_time(Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Str burrow__asn1_parse_bmp_string(Alloc *a, Slice b, Error *err);

/* makeBigInt, encoded straight away, from a. */
BURROW_OWNS(ret) Slice burrow__asn1_make_big_int(Alloc *a, const BigInt *n, Error *err);

#endif /* BURROW_SRC_ENCODING_ASN1_INTERNAL_H */
