/* The tables behind the fast paths in letter.c and graphic.c, which Go keeps
 * private to package unicode. tables.c defines them along with the exported
 * ones.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_UNICODE_TABLES_H
#define BURROW_SRC_UNICODE_TABLES_H

#include "burrow/unicode.h"

#include <stdint.h>

/* The property bits for each Latin-1 code point. */
enum {
    BURROW_UNICODE_PC = 1 << 0,     /* a control character */
    BURROW_UNICODE_PP = 1 << 1,     /* a punctuation character */
    BURROW_UNICODE_PN = 1 << 2,     /* a numeral */
    BURROW_UNICODE_PS = 1 << 3,     /* a symbolic character */
    BURROW_UNICODE_PZ = 1 << 4,     /* a spacing character */
    BURROW_UNICODE_PLU = 1 << 5,    /* an upper case letter */
    BURROW_UNICODE_PLL = 1 << 6,    /* a lower case letter */
    BURROW_UNICODE_PPRINT = 1 << 7, /* printable in Go's sense */
    BURROW_UNICODE_PG = BURROW_UNICODE_PPRINT | BURROW_UNICODE_PZ, /* graphic */
    BURROW_UNICODE_PLO = BURROW_UNICODE_PLL | BURROW_UNICODE_PLU,  /* a letter */
    BURROW_UNICODE_PLMASK = BURROW_UNICODE_PLO,
};

/* One step of a case orbit: from folds to to. */
typedef struct BurrowUnicodeFoldPair {
    uint16_t from;
    uint16_t to;
} BurrowUnicodeFoldPair;

extern const uint8_t burrow__unicode_properties[256];
extern const uint16_t burrow__unicode_ascii_fold[128];
extern const BurrowUnicodeFoldPair burrow__unicode_case_orbit[];
extern const Int burrow__unicode_case_orbit_len;

/* Is for a table whose Latin-1 part the caller has already checked another
 * way, so the ranges below latin_offset can be skipped. */
bool burrow__unicode_is_excluding_latin(const UnicodeRangeTable *table, Rune r);

#endif /* BURROW_SRC_UNICODE_TABLES_H */
