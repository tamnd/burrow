/* Derived from Go's src/crypto/internal/fips140test/indicator_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"

#include "../src/crypto/fips140_module.h"

static void indicator_approved(void *env) {
    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_approved();
    chan_close(env);
}

static void indicator_non_approved(void *env) {
    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_non_approved();
    chan_close(env);
}

static void TestIndicator(TestingT *t) {
    burrow__fips140_reset_service_indicator();
    if (burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "indicator should be false if no calls are made");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_approved();
    if (!burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "indicator should be true if RecordApproved is called");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_approved();
    burrow__fips140_record_approved();
    if (!burrow__fips140_service_indicator())
        testing_t_errorf_v(
            t, "indicator should be true if RecordApproved is called multiple times");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_non_approved();
    if (burrow__fips140_service_indicator())
        testing_t_errorf_v(t,
                           "indicator should be false if RecordNonApproved is called");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_approved();
    burrow__fips140_record_non_approved();
    if (burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "indicator should be false if both RecordApproved and "
                              "RecordNonApproved are called");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_non_approved();
    burrow__fips140_record_approved();
    if (burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "indicator should be false if both RecordNonApproved and "
                              "RecordApproved are called");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_non_approved();
    Chan *done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    go(BURROW_FN(Func, indicator_approved, done));
    bool v = false;
    chan_recv(done, &v);
    chan_free(done);
    if (burrow__fips140_service_indicator())
        testing_t_errorf_v(t,
                           "indicator should be false if RecordApproved is called in "
                           "a different goroutine");

    burrow__fips140_reset_service_indicator();
    burrow__fips140_record_approved();
    done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    go(BURROW_FN(Func, indicator_non_approved, done));
    chan_recv(done, &v);
    chan_free(done);
    if (!burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "indicator should be true if RecordNonApproved is called "
                              "in a different goroutine");
}

#define TESTS(X) X(TestIndicator)

TESTING_MAIN(TESTS)
