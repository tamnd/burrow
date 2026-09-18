/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/runtime.h"

#include <string.h>

/* Everything here is at the boundary between a Str and something that is not a
 * Str. The interesting operations on strings live in the strings and bytes
 * packages, where Go put them, and they are ports rather than inventions. This
 * file is only the handful of things that have no Go equivalent because Go does
 * not have a C string to convert to.
 *
 * Note that memcmp and memcpy are never called with a NULL pointer here even
 * when the length is zero. That combination is undefined behaviour in C, it is
 * a real diagnostic under UndefinedBehaviorSanitizer rather than a theoretical
 * one, and a zero length Str with a NULL pointer is the most ordinary value in
 * this library. */

Str str_from_cstr(const char *s) {
    if (s == NULL)
        return BURROW_STR_EMPTY;
    size_t n = strlen(s);
    /* Unreachable on a 64 bit platform, since there is no address space for a
     * string that long. On a 32 bit one Int is the same width as size_t's
     * signed half, so this is the honest clamp rather than a wrap. */
    if (n > (size_t)BURROW_INT_MAX)
        n = (size_t)BURROW_INT_MAX;
    Str out = {(const Byte *)s, (Int)n};
    return out;
}

Str str_from_bytes(const void *p, Int n) {
    if (p == NULL || n <= 0)
        return BURROW_STR_EMPTY;
    Str out = {(const Byte *)p, n};
    return out;
}

char *str_to_cstr(Alloc *a, Str s) {
    if (s.len < 0)
        return NULL;
    char *out = (char *)mem_alloc(a, (size_t)s.len + 1, 1);
    if (out == NULL)
        return NULL;
    if (s.len > 0)
        memcpy(out, s.p, (size_t)s.len);
    out[s.len] = '\0';
    return out;
}

bool str_has_nul(Str s) {
    if (s.p == NULL || s.len <= 0)
        return false;
    return memchr(s.p, 0, (size_t)s.len) != NULL;
}

int str_cmp(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    if (n > 0) {
        int r = memcmp(a.p, b.p, (size_t)n);
        if (r != 0)
            return r;
    }
    if (a.len == b.len)
        return 0;
    return a.len < b.len ? -1 : 1;
}

bool str_eq(Str a, Str b) {
    if (a.len != b.len)
        return false;
    if (a.len <= 0)
        return true;
    return memcmp(a.p, b.p, (size_t)a.len) == 0;
}

Str str_clone(Alloc *a, Str s) {
    if (s.len <= 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    Str out = {p, s.len};
    return out;
}

bool str_is_empty(Str s) {
    return s.len <= 0;
}

Byte str_at(Str s, Int i) {
    /* Both ends, spelled out. The usual trick is to compare as unsigned so that
     * a negative index becomes an enormous one and a single branch covers both,
     * and that is what Go's compiler emits, but it is only sound because Go's
     * len can never be negative. Str is a struct anybody can fill in by hand, so
     * here it can be, and the unsigned version would turn that into a read
     * through a NULL pointer instead of a message. Two compares the branch
     * predictor gets right every time is not a cost worth arguing about. */
    if (i < 0 || i >= s.len)
        runtime_index_out_of_range(i, s.len);
    return s.p[i];
}
