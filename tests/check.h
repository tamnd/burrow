/* The checks the tests in this directory make, as one line each.
 *
 * They are shorthand for an if and testing_t_errorf_v, and every one of them
 * reports through a TestingT called t, which is the parameter every test has.
 * A helper that checks something takes t as well. A failed check marks the test
 * failed and carries on, like t.Errorf in Go, and the message names the file and
 * line of the check and not of this header.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_CHECK_H
#define BURROW_TESTS_CHECK_H

#include "burrow/testing.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond))                                                                   \
            testing_t_errorf_v(t, "check failed: %s", #cond);                          \
    } while (0)

#define CHECK_STR_EQ(got, want)                                                        \
    do {                                                                               \
        const char *g_ = (got), *w_ = (want);                                          \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0)                           \
            testing_t_errorf_v(t, "got %q, want %q", g_ ? g_ : "(null)",               \
                               w_ ? w_ : "(null)");                                    \
    } while (0)

#define CHECK_INT_EQ(got, want)                                                        \
    do {                                                                               \
        int64_t g_ = (int64_t)(got), w_ = (int64_t)(want);                             \
        if (g_ != w_)                                                                  \
            testing_t_errorf_v(t, "%s = %d, want %d", #got, g_, w_);                   \
    } while (0)

#endif /* BURROW_TESTS_CHECK_H */
