/* Derived from Go's src/unicode/letter.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/unicode.h"

#include "burrow/core.h"

#include "tables.h"

/* A table with no more ranges than this is searched in a line rather than by
 * halves. */
enum { unicode_linear_max = 18 };

/* Whether r is in a range of a stride that has already been found to hold
 * it between lo and hi. */
static bool unicode_on_stride16(const UnicodeRange16 *range_, uint16_t r) {
    return range_->stride == 1 || (uint16_t)(r - range_->lo) % range_->stride == 0;
}

static bool unicode_on_stride32(const UnicodeRange32 *range_, uint32_t r) {
    return range_->stride == 1 || (r - range_->lo) % range_->stride == 0;
}

static bool unicode_is16(const UnicodeRange16 *ranges, Int n, uint16_t r) {
    if (n <= unicode_linear_max || r <= UNICODE_MAX_LATIN1) {
        for (Int i = 0; i < n; i++) {
            const UnicodeRange16 *range_ = &ranges[i];
            if (r < range_->lo)
                return false;
            if (r <= range_->hi)
                return unicode_on_stride16(range_, r);
        }
        return false;
    }

    /* binary search over ranges */
    Int lo = 0, hi = n;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        const UnicodeRange16 *range_ = &ranges[m];
        if (range_->lo <= r && r <= range_->hi)
            return unicode_on_stride16(range_, r);
        if (r < range_->lo)
            hi = m;
        else
            lo = m + 1;
    }
    return false;
}

static bool unicode_is32(const UnicodeRange32 *ranges, Int n, uint32_t r) {
    if (n <= unicode_linear_max) {
        for (Int i = 0; i < n; i++) {
            const UnicodeRange32 *range_ = &ranges[i];
            if (r < range_->lo)
                return false;
            if (r <= range_->hi)
                return unicode_on_stride32(range_, r);
        }
        return false;
    }

    /* binary search over ranges */
    Int lo = 0, hi = n;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        const UnicodeRange32 *range_ = &ranges[m];
        if (range_->lo <= r && r <= range_->hi)
            return unicode_on_stride32(range_, r);
        if (r < range_->lo)
            hi = m;
        else
            lo = m + 1;
    }
    return false;
}

bool unicode_is(const UnicodeRangeTable *range_tab, Rune r) {
    const UnicodeRange16 *r16 = range_tab->r16;
    Int n16 = range_tab->r16_len;
    /* Compare as uint32 to correctly handle negative runes. */
    if (n16 > 0 && (uint32_t)r <= (uint32_t)r16[n16 - 1].hi)
        return unicode_is16(r16, n16, (uint16_t)r);
    const UnicodeRange32 *r32 = range_tab->r32;
    Int n32 = range_tab->r32_len;
    if (n32 > 0 && r >= (Rune)r32[0].lo)
        return unicode_is32(r32, n32, (uint32_t)r);
    return false;
}

bool burrow__unicode_is_excluding_latin(const UnicodeRangeTable *range_tab, Rune r) {
    const UnicodeRange16 *r16 = range_tab->r16;
    Int n16 = range_tab->r16_len;
    /* Compare as uint32 to correctly handle negative runes. */
    Int off = range_tab->latin_offset;
    if (n16 > off && (uint32_t)r <= (uint32_t)r16[n16 - 1].hi)
        return unicode_is16(r16 + off, n16 - off, (uint16_t)r);
    const UnicodeRange32 *r32 = range_tab->r32;
    Int n32 = range_tab->r32_len;
    if (n32 > 0 && r >= (Rune)r32[0].lo)
        return unicode_is32(r32, n32, (uint32_t)r);
    return false;
}

bool unicode_is_upper(Rune r) {
    /* See comment in unicode_is_graphic. */
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (burrow__unicode_properties[(uint8_t)r] & BURROW_UNICODE_PLMASK) ==
               BURROW_UNICODE_PLU;
    return burrow__unicode_is_excluding_latin(unicode_upper, r);
}

bool unicode_is_lower(Rune r) {
    /* See comment in unicode_is_graphic. */
    if ((uint32_t)r <= UNICODE_MAX_LATIN1)
        return (burrow__unicode_properties[(uint8_t)r] & BURROW_UNICODE_PLMASK) ==
               BURROW_UNICODE_PLL;
    return burrow__unicode_is_excluding_latin(unicode_lower, r);
}

bool unicode_is_title(Rune r) {
    if (r <= UNICODE_MAX_LATIN1)
        return false;
    return burrow__unicode_is_excluding_latin(unicode_title, r);
}

/* The CaseRange holding r, or NULL. */
static const UnicodeCaseRange *
unicode_lookup_case_range(Rune r, UnicodeSpecialCase case_range) {
    /* binary search over ranges */
    Int lo = 0, hi = case_range.len;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        const UnicodeCaseRange *cr = &case_range.p[m];
        if ((Rune)cr->lo <= r && r <= (Rune)cr->hi)
            return cr;
        if (r < (Rune)cr->lo)
            hi = m;
        else
            lo = m + 1;
    }
    return NULL;
}

/* Returns the mapping of r into which case, using cr. */
static Rune unicode_convert_case(Int which, Rune r, const UnicodeCaseRange *cr) {
    Rune delta = cr->delta[which];
    if (delta > UNICODE_MAX_RUNE) {
        /* In an Upper-Lower sequence, which always starts with an upper case
         * letter, the real deltas always look like: {0, 1, 0} upper case
         * (lower is next), {-1, 0, -1} lower case (upper, title are
         * previous). The characters at even offsets from the beginning of
         * the sequence are upper case; the ones at odd offsets are lower. The
         * correct mapping can be done by clearing or setting the low bit in
         * the sequence offset. The constants UNICODE_UPPER_CASE and
         * UNICODE_TITLE_CASE are even while UNICODE_LOWER_CASE is odd, so we
         * take the low bit from which. */
        Rune lo = (Rune)cr->lo;
        return lo + (((r - lo) & ~(Rune)1) | (Rune)(which & 1));
    }
    return r + delta;
}

/* Maps the rune using the specified case mapping, and says whether
 * case_range had a mapping for it. */
static Rune unicode_to_case(Int which, Rune r, UnicodeSpecialCase case_range,
                            bool *found_mapping) {
    if (which < 0 || UNICODE_MAX_CASE <= which) {
        *found_mapping = false;
        return UNICODE_REPLACEMENT_CHAR; /* as reasonable an error as any */
    }
    const UnicodeCaseRange *cr = unicode_lookup_case_range(r, case_range);
    if (cr != NULL) {
        *found_mapping = true;
        return unicode_convert_case(which, r, cr);
    }
    *found_mapping = false;
    return r;
}

Rune unicode_to(Int which, Rune r) {
    bool found;
    return unicode_to_case(which, r, unicode_case_ranges, &found);
}

Rune unicode_to_upper(Rune r) {
    if (r <= UNICODE_MAX_ASCII) {
        if ('a' <= r && r <= 'z')
            r -= 'a' - 'A';
        return r;
    }
    return unicode_to(UNICODE_UPPER_CASE, r);
}

Rune unicode_to_lower(Rune r) {
    if (r <= UNICODE_MAX_ASCII) {
        if ('A' <= r && r <= 'Z')
            r += 'a' - 'A';
        return r;
    }
    return unicode_to(UNICODE_LOWER_CASE, r);
}

Rune unicode_to_title(Rune r) {
    if (r <= UNICODE_MAX_ASCII) {
        if ('a' <= r && r <= 'z') /* title case is upper case for ASCII */
            r -= 'a' - 'A';
        return r;
    }
    return unicode_to(UNICODE_TITLE_CASE, r);
}

Rune unicode_special_case_to_upper(UnicodeSpecialCase special, Rune r) {
    bool had_mapping;
    Rune r1 = unicode_to_case(UNICODE_UPPER_CASE, r, special, &had_mapping);
    if (r1 == r && !had_mapping)
        r1 = unicode_to_upper(r);
    return r1;
}

Rune unicode_special_case_to_title(UnicodeSpecialCase special, Rune r) {
    bool had_mapping;
    Rune r1 = unicode_to_case(UNICODE_TITLE_CASE, r, special, &had_mapping);
    if (r1 == r && !had_mapping)
        r1 = unicode_to_title(r);
    return r1;
}

Rune unicode_special_case_to_lower(UnicodeSpecialCase special, Rune r) {
    bool had_mapping;
    Rune r1 = unicode_to_case(UNICODE_LOWER_CASE, r, special, &had_mapping);
    if (r1 == r && !had_mapping)
        r1 = unicode_to_lower(r);
    return r1;
}

Rune unicode_simple_fold(Rune r) {
    if (r < 0 || r > UNICODE_MAX_RUNE)
        return r;

    if (r < 128)
        return (Rune)burrow__unicode_ascii_fold[r];

    /* Consult caseOrbit table for special cases. */
    Int lo = 0, hi = burrow__unicode_case_orbit_len;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        if ((Rune)burrow__unicode_case_orbit[m].from < r)
            lo = m + 1;
        else
            hi = m;
    }
    if (lo < burrow__unicode_case_orbit_len &&
        (Rune)burrow__unicode_case_orbit[lo].from == r)
        return (Rune)burrow__unicode_case_orbit[lo].to;

    /* No folding specified. This is a one- or two-element equivalence class
     * containing rune and ToLower(rune) and ToUpper(rune) if they are
     * different from rune. */
    const UnicodeCaseRange *cr = unicode_lookup_case_range(r, unicode_case_ranges);
    if (cr != NULL) {
        Rune l = unicode_convert_case(UNICODE_LOWER_CASE, r, cr);
        if (l != r)
            return l;
        return unicode_convert_case(UNICODE_UPPER_CASE, r, cr);
    }
    return r;
}

/* Not from Go, whose maps need no code to look in. */

const UnicodeRangeTable *unicode_table_map_get(UnicodeTableMap m, Str name) {
    Int lo = 0, hi = m.len;
    while (lo < hi) {
        Int mid = (Int)((Uint)(lo + hi) >> 1);
        int c = str_cmp(m.p[mid].name, name);
        if (c == 0)
            return m.p[mid].table;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

bool unicode_alias_map_get(UnicodeAliasMap m, Str name, Str *target) {
    Int lo = 0, hi = m.len;
    while (lo < hi) {
        Int mid = (Int)((Uint)(lo + hi) >> 1);
        int c = str_cmp(m.p[mid].name, name);
        if (c == 0) {
            if (target != NULL)
                *target = m.p[mid].target;
            return true;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}
