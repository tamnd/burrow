/* The fuzzing engine, Go's internal/fuzz, as the testing package uses it. What
 * testing.c hands over is a list of seeds and the types, and what it gets back
 * is an error, and for a crash the file the failing input went to.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
#ifndef BURROW_TESTING_FUZZ_H
#define BURROW_TESTING_FUZZ_H

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/type.h"

/* A seed: its name, which is "seed#N" for one from F.Add and a path for one
 * read from testdata, and its corpus file text. */
typedef struct burrow__FuzzSeed {
    Str path;
    Str data;
} burrow__FuzzSeed;

/* CoordinateFuzzingOpts, less the log, which is standard error. */
typedef struct burrow__FuzzOpts {
    int64_t timeout;
    int64_t limit;
    int64_t minimize_timeout;
    int64_t minimize_limit;
    int parallel;
    const burrow__FuzzSeed *seed;
    Int nseed;
    const Type *const *types;
    Int ntypes;
    /* What the cache directory held, read by testing.c, which reads
     * testdata the same way. The paths are the files'. */
    const burrow__FuzzSeed *cache;
    Int ncache;
    Str corpus_dir;
    Str cache_dir;
    /* The test binary's own command line, for starting the workers, and its
     * path as one that still works after a change of directory, which is
     * what os.Executable gives Go. Empty exe means argv[0]. */
    int argc;
    char **argv;
    Str exe;
} burrow__FuzzOpts;

/* CoordinateFuzzing. Answers true when fuzzing ended with nothing to report.
 * Otherwise *err is the message and, when an input made the target fail,
 * *crash_path is the file it was written to, both allocated from a. */
bool burrow__fuzz_coordinate(Alloc *a, const burrow__FuzzOpts *opts, Str *err,
                             Str *crash_path);

/* What a worker runs each input through. Answers whether the target passed,
 * and when it did not, *msg is what it printed, allocated from a. */
typedef bool (*burrow__FuzzRun)(void *env, Any *vals, Int n, Alloc *a, Str *msg);

/* RunFuzzWorker. Serves the coordinator until it hangs up. Answers false with
 * *err, allocated from a, when talking to it failed. */
bool burrow__fuzz_worker(burrow__FuzzRun fn, void *env, Alloc *a, Str *err);

/* Duration.String, which testing.c has, into the 32 bytes at buf. */
Str burrow__testing_duration_string(int64_t d, char buf[32]);

/* The pieces, here so that the tests can reach them. */

/* pcgRand. */
typedef struct burrow__FuzzRand {
    uint64_t state;
    uint64_t inc;
} burrow__FuzzRand;

void burrow__fuzz_rand_init(burrow__FuzzRand *r);
uint32_t burrow__fuzz_rand_uint32(burrow__FuzzRand *r);
uint32_t burrow__fuzz_rand_uint32n(burrow__FuzzRand *r, uint32_t n);
Int burrow__fuzz_rand_intn(burrow__FuzzRand *r, Int n);
bool burrow__fuzz_rand_bool(burrow__FuzzRand *r);

/* mutator. The values it hands back point into buffers it owns, which stay
 * good until the next call or burrow__fuzz_mutator_free. */
typedef struct burrow__FuzzMutator {
    burrow__FuzzRand r;
    Byte *scratch;
    Int scratch_cap;
    /* One buffer per value index for the strings, which Go allocates fresh on
     * every mutation and this keeps. */
    Byte **strs;
    Int *str_caps;
    Int nstrs;
    /* Buffers replaced by bigger ones, which a value handed out earlier may
     * still point into. Go's collector would keep them, and this keeps them
     * until burrow__fuzz_mutator_free. */
    Byte **old;
    Int *old_caps;
    Int nold;
} burrow__FuzzMutator;

void burrow__fuzz_mutator_init(burrow__FuzzMutator *m);
void burrow__fuzz_mutator_free(burrow__FuzzMutator *m);
void burrow__fuzz_mutate(burrow__FuzzMutator *m, Any *vals, Int n, Int max_bytes);

/* mutateBytes, on b with room for cap bytes. Answers the new length. */
Int burrow__fuzz_mutate_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap);

/* minimizeBytes. try is given a candidate and answers whether it still
 * fails. On return v holds the smallest input found and *len its length. */
typedef bool (*burrow__FuzzTry)(void *env, const Byte *b, Int n);
typedef bool (*burrow__FuzzShouldStop)(void *env);
void burrow__fuzz_minimize_bytes(Byte *v, Int *len, burrow__FuzzTry try_fn,
                                 burrow__FuzzShouldStop stop, void *env);

/* crypto/sha256's Sum256. */
void burrow__fuzz_sha256(const void *data, Int n, Byte out[32]);

#endif
