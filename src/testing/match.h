/* What match.c gives testing.c: the matcher behind -test.run and -test.skip,
 * which splits a pattern on slashes into one pattern per level of subtest and
 * hands out the unique names that subtests are reported under.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTING_MATCH_H
#define BURROW_TESTING_MATCH_H

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/sync.h"

/* Go's simpleMatch, one pattern per level of the name. */
typedef struct burrow__TestingSimple {
    Str *elem;
    Int n;
} burrow__TestingSimple;

/* Go's filterMatch, which is either one simpleMatch or an alternationMatch of
 * several. An empty -test.run is a simple match with no elements, which
 * matches everything partially, and an empty -test.skip is an alternation with
 * no members, which matches nothing. */
typedef struct burrow__TestingFilter {
    bool alternation;
    burrow__TestingSimple *alt;
    Int n;
} burrow__TestingFilter;

typedef struct burrow__TestingMatcher {
    burrow__TestingFilter filter;
    burrow__TestingFilter skip;
    TestingMatchString match;

    /* Held across calls to match, which Go does with a package level mutex
     * because the function might not be safe to call from two goroutines. One
     * matcher is live per run, so one per matcher does the same job. */
    SyncMutex match_mu;

    /* Guards sub_names, which counts how often each name has been handed out
     * so that two subtests with the same name get different ones. The map and
     * its keys live in names, which fuzzing empties between inputs, and the
     * patterns live in arena. */
    SyncMutex mu;
    Map *sub_names;
    Arena names;
    Arena arena;
} burrow__TestingMatcher;

/* Go's newMatcher. A pattern that does not compile prints Go's message and
 * ends the process with status 1, as Go does. */
BURROW_OWNS(ret) burrow__TestingMatcher *
burrow__testing_matcher_new(TestingMatchString match, Str patterns, Str name,
                            Str skips);
void burrow__testing_matcher_free(burrow__TestingMatcher *m);

/* clearSubNames: forgets every name handed out so far. */
void burrow__testing_clear_sub_names(burrow__TestingMatcher *m);

/* Go's fullName. parent is NULL for a top level test. The name comes back in
 * a heap allocation the caller owns, whether or not it matched. */
BURROW_OWNS(ret) Str burrow__testing_full_name(burrow__TestingMatcher *m,
                                               const Str *parent, Str subname, bool *ok,
                                               bool *partial);

/* Calls match, or the built in matcher when match is nil. */
bool burrow__testing_call_match(TestingMatchString match, Str pat, Str str, Error *err);

#endif /* BURROW_TESTING_MATCH_H */
