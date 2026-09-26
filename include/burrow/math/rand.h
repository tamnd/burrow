/* math/rand, the first version of Go's pseudo random numbers.
 *
 * Go's math/rand. New code should use math/rand/v2, which is in
 * burrow/math/rand/v2.h and has a better generator, a better set of methods
 * and no global seed. This package is here because a lot of Go is written
 * against it and because its seeded sequences are part of what programs
 * depend on: math_rand_new_source(seed) gives exactly the numbers Go's
 * NewSource(seed) does, method for method.
 *
 *     MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 42));
 *     Int n = math_rand_rand_intn(r, 100);
 *
 * Go's type is Rand, and its methods would have the same names as the package
 * level functions, math_rand_intn and the rest, so the type is MathRandRand and
 * the methods are math_rand_rand_intn and so on.
 *
 * The package level functions use a generator seeded from the system. They are
 * safe to call from any number of threads. A Rand made with math_rand_new is
 * not, and neither is a source from math_rand_new_source.
 *
 * None of this is for keys, tokens or anything else that has to be hard to
 * guess. That is what crypto/rand is for.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package math/rand */

#ifndef BURROW_MATH_RAND_H
#define BURROW_MATH_RAND_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ Source */

/* rand.Source: uniformly distributed non negative 63 bit values, and a way to
 * reset the stream. */
typedef struct MathRandSourceVT {
    const Type *self_type;
    int64_t (*int63)(void *self);
    void (*seed)(void *self, int64_t seed);
} MathRandSourceVT;

typedef struct MathRandSource {
    const MathRandSourceVT *vt;
    void *data;
} MathRandSource;

static inline int64_t math_rand_source_int63(MathRandSource s) {
    return s.vt->int63(s.data);
}

static inline void math_rand_source_seed(MathRandSource s, int64_t seed) {
    s.vt->seed(s.data, seed);
}

/* rand.Source64: a Source that can also hand out all 64 bits at once. */
typedef struct MathRandSource64VT {
    MathRandSourceVT source;
    uint64_t (*uint64)(void *self);
} MathRandSource64VT;

typedef struct MathRandSource64 {
    const MathRandSource64VT *vt;
    void *data;
} MathRandSource64;

static inline int64_t math_rand_source64_int63(MathRandSource64 s) {
    return s.vt->source.int63(s.data);
}

static inline void math_rand_source64_seed(MathRandSource64 s, int64_t seed) {
    s.vt->source.seed(s.data, seed);
}

static inline uint64_t math_rand_source64_uint64(MathRandSource64 s) {
    return s.vt->uint64(s.data);
}

static inline MathRandSource math_rand_source64_as_source(MathRandSource64 s) {
    MathRandSource src = {&s.vt->source, s.data};
    return src;
}

/* The signature a source type lists Uint64 under in its method set, so that
 * math_rand_new can find it the way Go's New asserts to Source64:
 *
 *     #define MY_METHODS(M, T) M(T, Uint64, my_uint64, MATH_RAND_SIG_UINT64)
 *
 * The sources this package makes are found without it. */
#define MATH_RAND_SIG_UINT64(IN, OUT) OUT(uint64_t)

/* A source seeded with seed, from a. It is Go's lagged Fibonacci generator,
 * so the values match Go's for the same seed. It is not safe for use from
 * more than one thread at a time, and it also implements Source64, which
 * math_rand_new finds on its own. */
MathRandSource math_rand_new_source(Alloc *a, int64_t seed);

/* -------------------------------------------------------------------- Rand */

/* rand.Rand. The members are the implementation's. */
typedef struct MathRandRand {
    MathRandSource src;
    MathRandSource64 s64;
    const Method *u64;
    int64_t read_val;
    int8_t read_pos;
} MathRandRand;

/* A Rand from a that draws from src. When src has a Uint64 as well, through
 * being one of this package's sources or by listing it in its method set,
 * math_rand_rand_uint64 uses it, as Go's does. */
BURROW_OWNS(ret) MathRandRand *math_rand_new(Alloc *a, MathRandSource src);

/* Resets the source to seed. Only for a source that can be seeded: the shared
 * generator behind the package level functions cannot. */
void math_rand_rand_seed(MathRandRand *r, int64_t seed);

/* A non negative 63 bit and 31 bit integer, and a non negative Int. */
int64_t math_rand_rand_int63(MathRandRand *r);
int32_t math_rand_rand_int31(MathRandRand *r);
Int math_rand_rand_int(MathRandRand *r);

/* 32 and 64 random bits. */
uint32_t math_rand_rand_uint32(MathRandRand *r);
uint64_t math_rand_rand_uint64(MathRandRand *r);

/* A non negative value below n. They panic when n is not positive. */
int64_t math_rand_rand_int63n(MathRandRand *r, int64_t n);
int32_t math_rand_rand_int31n(MathRandRand *r, int32_t n);
Int math_rand_rand_intn(MathRandRand *r, Int n);

/* A float64 in [0.0, 1.0) and a float32 in [0.0, 1.0). */
double math_rand_rand_float64(MathRandRand *r);
float math_rand_rand_float32(MathRandRand *r);

/* A normally distributed float64 with mean 0 and standard deviation 1, and an
 * exponentially distributed one with rate 1. */
double math_rand_rand_norm_float64(MathRandRand *r);
double math_rand_rand_exp_float64(MathRandRand *r);

/* A permutation of the integers [0, n) as a slice of Int from a. */
BURROW_OWNS(ret) Slice math_rand_rand_perm(MathRandRand *r, Alloc *a, Int n);

/* Shuffles n elements by calling swap. Panics when n is negative. */
void math_rand_rand_shuffle(MathRandRand *r, Int n, SwapFunc swap);

/* Fills p with random bytes, always all of it and never an error. The bytes
 * come seven to a draw, and a read that ends part way into a draw keeps the
 * rest for the next read. It should not be used alongside the other methods on
 * the same Rand, as Go's documentation says. */
Int math_rand_rand_read(MathRandRand *r, Slice p, Error *err);

/* -------------------------------------------------------------------- Zipf */

/* rand.Zipf: P(k) is proportional to (v + k) ** (-s) for k in [0, imax]. */
typedef struct MathRandZipf {
    MathRandRand *r;
    double imax;
    double v;
    double q;
    double s;
    double one_minus_q;
    double one_minus_q_inv;
    double hxm;
    double hx0_minus_hxm;
} MathRandZipf;

/* A Zipf drawing from r with s > 1 and v >= 1, or NULL when either is out of
 * range. r has to outlive it. */
BURROW_OWNS(ret) MathRandZipf *math_rand_new_zipf(Alloc *a, MathRandRand *r, double s,
                                                  double v, uint64_t imax);

/* A value drawn from the distribution. Panics on a NULL z. */
uint64_t math_rand_zipf_uint64(MathRandZipf *z);

/* ----------------------------------------------------- top level functions */

/* The methods above on the shared generator. */
int64_t math_rand_int63(void);
int32_t math_rand_int31(void);
Int math_rand_int(void);
uint32_t math_rand_uint32(void);
uint64_t math_rand_uint64(void);
int64_t math_rand_int63n(int64_t n);
int32_t math_rand_int31n(int32_t n);
Int math_rand_intn(Int n);
double math_rand_float64(void);
float math_rand_float32(void);
double math_rand_norm_float64(void);
double math_rand_exp_float64(void);
BURROW_OWNS(ret) Slice math_rand_perm(Alloc *a, Int n);
void math_rand_shuffle(Int n, SwapFunc swap);

/* Deprecated: as of Go 1.24 this is a no-op, and it stays one here. The
 * shared generator is seeded from the system and cannot be reseeded, and a
 * program that wants a repeatable sequence makes a Rand of its own with
 * math_rand_new(a, math_rand_new_source(a, seed)).
 *
 * GODEBUG=randseednop=0 brings back the old behaviour, where this switches the
 * shared generator to one seeded with seed, and GODEBUG=randautoseed=0 starts
 * it out seeded with 1, both as in Go. */
void math_rand_seed(int64_t seed);

/* Deprecated: for almost every use crypto/rand's Read is more appropriate,
 * as Go's documentation says. Fills p from the shared generator, and like the
 * method it never fails. */
Int math_rand_read(Slice p, Error *err);

/* The GODEBUG settings above, read from the environment on first use. This
 * sets them from value instead, or makes the next use read the environment
 * again when value is NULL, and is for tests. */
void burrow__math_rand_godebug_set(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_RAND_H */
