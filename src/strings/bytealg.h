/* Derived from Go's src/internal/bytealg.
 * Go source: go1.27.1.
 *
 * The byte searching that strings and bytes share, over a Str because a Str is
 * exactly a pointer and a length. Go writes most of this in assembly per
 * architecture. Here IndexByte is memchr, which every libc vectorises, and the
 * rest is Go's portable code with the arm64 cutover constants.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_STRINGS_BYTEALG_H
#define BURROW_SRC_STRINGS_BYTEALG_H

#include "burrow/strings.h"

#include "burrow/core.h"

#include <string.h>

/* Needles up to this long go to burrow__bytealg_index_string, and haystacks up
 * to MAX_BRUTE_FORCE long skip straight to it. */
#define BURROW__BYTEALG_MAX_LEN 32
#define BURROW__BYTEALG_MAX_BRUTE_FORCE 16

/* The number of IndexByte false positives to put up with before switching to
 * IndexString, n being the bytes looked at so far. */
static inline Int burrow__bytealg_cutover(Int n) {
    return 4 + (n >> 4);
}

static inline Int burrow__bytealg_index_byte(Str s, Byte c) {
    if (s.len <= 0)
        return -1;
    const Byte *p = (const Byte *)memchr(s.p, c, (size_t)s.len);
    return p == NULL ? -1 : (Int)(p - s.p);
}

static inline bool burrow__bytealg_equal(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

Int burrow__bytealg_last_index_byte(Str s, Byte c);
Int burrow__bytealg_count(Str s, Byte c);

/* The first sep in s, for 2 <= len(sep) <= MAX_LEN. */
Int burrow__bytealg_index_string(Str s, Str sep);

Int burrow__bytealg_index_rabin_karp(Str s, Str sep);
Int burrow__bytealg_last_index_rabin_karp(Str s, Str sep);

#endif /* BURROW_SRC_STRINGS_BYTEALG_H */
