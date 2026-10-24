/* Derived from Go's src/crypto/internal/fips140test/alias_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"

#include "../src/crypto/alias_internal.h"

static Byte a[100], b[100];

/* Not in Go: arr[lo:hi:max]. */
static Slice sl(Byte *arr, Int lo, Int hi, Int max) {
    return slice_from(arr + lo, hi - lo, max - lo, TYPE_BYTE);
}

typedef struct AliasingTest {
    Slice x, y;
    bool anyOverlap, inexactOverlap;
} AliasingTest;

static void testAliasing(TestingT *t, Int i, Slice x, Slice y, bool anyOverlap,
                         bool inexactOverlap) {
    bool any = burrow__alias_any_overlap(x, y);
    if (any != anyOverlap)
        testing_t_errorf_v(t, "%d: wrong AnyOverlap result, expected %v, got %v", i,
                           anyOverlap, any);
    bool inexact = burrow__alias_inexact_overlap(x, y);
    if (inexact != inexactOverlap)
        testing_t_errorf_v(t, "%d: wrong InexactOverlap result, expected %v, got %v", i,
                           inexactOverlap, any);
}

static void TestAliasing(TestingT *t) {
    Slice nil = slice_nil(TYPE_BYTE);
    AliasingTest aliasingTests[] = {
        {sl(a, 0, 100, 100), sl(b, 0, 100, 100), false, false},
        {sl(a, 0, 100, 100), sl(b, 0, 0, 100), false, false},
        {sl(a, 0, 100, 100), sl(b, 0, 50, 100), false, false},
        {sl(a, 40, 50, 100), sl(a, 50, 60, 100), false, false},
        {sl(a, 40, 50, 100), sl(a, 60, 70, 100), false, false},
        {sl(a, 0, 51, 100), sl(a, 50, 100, 100), true, true},
        {sl(a, 0, 100, 100), sl(a, 0, 100, 100), true, false},
        {sl(a, 0, 50, 100), sl(a, 0, 60, 100), true, false},
        {sl(a, 0, 100, 100), nil, false, false},
        {nil, nil, false, false},
        {sl(a, 0, 100, 100), sl(a, 0, 0, 100), false, false},
        {sl(a, 0, 10, 100), sl(a, 0, 10, 20), true, false},
        {sl(a, 0, 10, 100), sl(a, 5, 10, 20), true, true},
    };
    for (Int i = 0; i < (Int)(sizeof aliasingTests / sizeof aliasingTests[0]); i++) {
        AliasingTest tt = aliasingTests[i];
        testAliasing(t, i, tt.x, tt.y, tt.anyOverlap, tt.inexactOverlap);
        testAliasing(t, i, tt.y, tt.x, tt.anyOverlap, tt.inexactOverlap);
    }
}

#define TESTS(X) X(TestAliasing)

TESTING_MAIN(TESTS)
