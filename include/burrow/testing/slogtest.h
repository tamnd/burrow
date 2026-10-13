/* testing/slogtest, checks for an SlogHandler.
 *
 * slogtest_test_handler logs through the handler a fixed set of times, each
 * time in a way that tests one of the rules a handler has to keep, then asks
 * for what came out and checks it:
 *
 *     Error err = slogtest_test_handler(slog_json_handler_as_handler(h),
 *                                       BURROW_FN(SlogtestResultsFunc, parse, &buf));
 *
 * What came out is a Slice of Map *, one for each record, each a
 * map[string]any of the kind encoding/json makes from an object: a Map with
 * Str keys and Any values, made with TYPE_JSONV2_MAP_STRING_ANY's types. A
 * group is a nested map of the same type, and the checks compare values that
 * should be strings as Str, so a JSON handler's output decoded with
 * json_unmarshal into an Any is already in the right shape.
 *
 * slogtest_run is the same with one subtest for each check, a new handler
 * each time and one map back.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package testing/slogtest */

#ifndef BURROW_TESTING_SLOGTEST_H
#define BURROW_TESTING_SLOGTEST_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/log/slog.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/testing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The results function slogtest_test_handler calls once at the end: a Slice
 * of Map *, one for each record the handler was given, in order. */
BURROW_FUNC0(SlogtestResultsFunc, Slice);

/* What slogtest_run calls at the start of each subtest, for the handler to
 * test. */
BURROW_FUNC(SlogtestNewHandlerFunc, SlogHandler, TestingT *t);

/* What slogtest_run calls at the end of each subtest, for the one record the
 * handler was given as a map. */
BURROW_FUNC(SlogtestResultFunc, Map *, TestingT *t);

/* slogtest.TestHandler. Installs h in a logger, logs once for each case,
 * calls results and checks what it returns. The error joins one for each
 * check that failed, and says which rule was broken. The handler has to be
 * enabled at SLOG_LEVEL_INFO and above. A handler that drops an attribute on
 * purpose should have results look for that and put it in the map. */
BURROW_STATIC(ret) Error slogtest_test_handler(SlogHandler h,
                                               SlogtestResultsFunc results);

/* slogtest.Run. Each case in a subtest of t, with a handler from new_handler
 * and its one record from result. A failed check is a testing_t_errorf. */
void slogtest_run(TestingT *t, SlogtestNewHandlerFunc new_handler,
                  SlogtestResultFunc result);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_SLOGTEST_H */
