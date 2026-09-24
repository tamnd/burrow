/* Derived from Go's src/unicode/graphic.go and src/unicode/digit.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/unicode.h"

#include "burrow/core.h"
#include "burrow/slice.h"

#include "tables.h"

/* The properties table has an entry for every Latin-1 code point. */
static uint8_t unicode_props(Rune r) {
    return burrow__unicode_properties[(uint8_t)r];
}

bool unicode_is_graphic(Rune r) {
    /* We convert to uint32 to avoid the extra test for negative, and in the
     * index we convert to uint8 to avoid the range check. */
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PG) != 0;
    return unicode_in(r, unicode_graphic_ranges);
}

bool unicode_is_print(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PPRINT) != 0;
    return unicode_in(r, unicode_print_ranges);
}

bool unicode_is_one_of(Slice ranges, Rune r) {
    return unicode_in(r, ranges);
}

bool unicode_in(Rune r, Slice ranges) {
    const UnicodeRangeTable *const *p = (const UnicodeRangeTable *const *)ranges.p;
    for (Int i = 0; i < ranges.len; i++) {
        if (unicode_is(p[i], r))
            return true;
    }
    return false;
}

bool unicode_is_control(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PC) != 0;
    /* All control characters are < MaxLatin1. */
    return false;
}

bool unicode_is_letter(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PLMASK) != 0;
    return burrow__unicode_is_excluding_latin(unicode_letter, r);
}

bool unicode_is_mark(Rune r) {
    /* There are no mark characters in Latin-1. */
    return burrow__unicode_is_excluding_latin(unicode_mark, r);
}

bool unicode_is_number(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PN) != 0;
    return burrow__unicode_is_excluding_latin(unicode_number, r);
}

bool unicode_is_punct(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PP) != 0;
    return unicode_is(unicode_punct, r);
}

bool unicode_is_space(Rune r) {
    /* This property isn't the same as Z; special-case it. */
    if ((uint32_t)r <= UNICODE_MAX_LATIN1) {
        switch (r) {
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case ' ':
        case 0x85:
        case 0xA0:
            return true;
        default:
            break;
        }
        return false;
    }
    return burrow__unicode_is_excluding_latin(unicode_white_space, r);
}

bool unicode_is_symbol(Rune r) {
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (unicode_props(r) & BURROW_UNICODE_PS) != 0;
    return burrow__unicode_is_excluding_latin(unicode_symbol, r);
}

bool unicode_is_digit(Rune r) {
    if (r <= UNICODE_MAX_LATIN1)
        return '0' <= r && r <= '9';
    return burrow__unicode_is_excluding_latin(unicode_digit, r);
}
