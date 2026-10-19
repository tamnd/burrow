/* The heap profile's records. See burrow/mprof.h for what it is for.
 *
 * Derived from Go's src/runtime/mprof.go, src/runtime/malloc.go and
 * src/runtime/fastlog2.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mprof.h"

#include "burrow/atomic.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

enum {
    /* size of bucket hash table */
    MPROF_BUCK_HASH_SIZE = 179999,

    /* How many frames a sample keeps. Not in Go, where it is
     * debug.profstackdepth, 128 unless GODEBUG says otherwise. The first few
     * are the allocator's own. */
    MPROF_STACK_DEPTH = 64,

    /* The sampled pointer filter has this many counters. */
    MPROF_FILTER_BITS = 16,

    /* The length of a RuntimeMemProfileRecord's stack0. */
    MPROF_STACK0 = 32,
};

_Static_assert(sizeof(((RuntimeMemProfileRecord *)0)->stack0) ==
                   MPROF_STACK0 * sizeof(Uintptr),
               "stack0 is MPROF_STACK0 long");

/* A bucket holds per-call-stack profiling information.
 *
 * None of the fields in this bucket header are modified after creation,
 * including its next and allnext links.
 *
 * Go keeps a memRecord after the stack, with the published counts and three
 * cycles of counts waiting to be published, so that a profile is as of the end
 * of a collection rather than skewed toward allocations whose frees the
 * collector has not swept yet. Here a free is counted when it happens, so
 * there is only ever the one set of counts. */
typedef struct MprofBucket {
    struct MprofBucket *next;
    struct MprofBucket *allnext;
    Uintptr hash;
    Uintptr size;
    Int nstk;
    uint64_t allocs, frees;
    Uintptr stk[];
} MprofBucket;

/* A sampled allocation that has not been freed, and the bucket it counts in.
 * This is Go's setprofilebucket, which hangs the bucket off the object's
 * span. */
typedef struct MprofSampled {
    Uintptr p;
    MprofBucket *b;
} MprofSampled;

typedef struct MprofState {
    burrow__Lock lock;
    MprofBucket **buckhash;
    MprofBucket *mbuckets; /* every bucket, newest first */
    /* Open addressing on the pointer, a power of two long, and never more than
     * half full. */
    MprofSampled *sampled;
    Int nsampled;
    Int cap_sampled;
    /* How many sampled pointers hash to each counter, so that a free can tell
     * without the lock that its pointer was not sampled. Written with the lock
     * held, read without it. */
    void *filter;
} MprofState;

static MprofState mprof;

/* MemProfileRate, as an atomic so that every allocation can read it. */
static uint64_t mprof_rate = (uint64_t)512 * 1024;

Int runtime_mem_profile_rate(void) {
    return (Int)burrow__atomic_load_relaxed_u64(&mprof_rate);
}

void runtime_set_mem_profile_rate(Int rate) {
    burrow__atomic_store_relaxed_u64(&mprof_rate, (uint64_t)rate);
}

/* fastlog2Table, from mkfastlog2table.go. */
enum { MPROF_FASTLOG_NUM_BITS = 5 };

static const double mprof_fastlog2_table[(1 << MPROF_FASTLOG_NUM_BITS) + 1] = {
    0,
    0.0443941193584535,
    0.08746284125033943,
    0.12928301694496647,
    0.16992500144231248,
    0.2094533656289499,
    0.24792751344358555,
    0.28540221886224837,
    0.3219280948873623,
    0.3575520046180837,
    0.39231742277876036,
    0.4262647547020979,
    0.4594316186372973,
    0.4918530963296748,
    0.5235619560570128,
    0.5545888516776374,
    0.5849625007211563,
    0.6147098441152082,
    0.6438561897747247,
    0.6724253419714956,
    0.7004397181410922,
    0.7279204545631992,
    0.7548875021634686,
    0.7813597135246596,
    0.8073549220576042,
    0.8328900141647417,
    0.8579809951275721,
    0.8826430493618412,
    0.9068905956085185,
    0.9307373375628862,
    0.9541963103868752,
    0.9772799234999164,
    1,
};

/* fastlog2 implements a fast approximation to the base 2 log of a float64.
 * This is used to compute a geometric distribution for heap sampling, without
 * introducing dependencies into package math. This uses a very rough
 * approximation using the float64 exponent and the first 25 bits of the
 * mantissa. The top 5 bits of the mantissa are used to load limits from a table
 * of constants and the rest are used to scale linearly between them. */
static double mprof_fastlog2(double x) {
    enum { FASTLOG_SCALE_BITS = 20 };
    const double fastlog_scale_ratio = 1.0 / (1 << FASTLOG_SCALE_BITS);

    uint64_t x_bits;
    memcpy(&x_bits, &x, sizeof x_bits);
    /* Extract the exponent from the IEEE float64, and index a constant table
     * with the first 10 bits from the mantissa. */
    int64_t x_exp = (int64_t)((x_bits >> 52) & 0x7FF) - 1023;
    uint64_t x_man_index =
        (x_bits >> (52 - MPROF_FASTLOG_NUM_BITS)) % (1 << MPROF_FASTLOG_NUM_BITS);
    uint64_t x_man_scale =
        (x_bits >> (52 - MPROF_FASTLOG_NUM_BITS - FASTLOG_SCALE_BITS)) %
        (1 << FASTLOG_SCALE_BITS);

    double low = mprof_fastlog2_table[x_man_index];
    double high = mprof_fastlog2_table[x_man_index + 1];
    return (double)x_exp + low + (high - low) * (double)x_man_scale * fastlog_scale_ratio;
}

/* fastexprand returns a random number from an exponential distribution with the
 * specified mean. */
static int32_t mprof_fastexprand(Int mean) {
    /* Avoid overflow. Maximum possible step is -ln(1/(1<<randomBitCount)) *
     * mean, approximately 20 * mean. */
    if (mean > 0x7000000)
        mean = 0x7000000;
    else if (mean == 0)
        return 0;

    /* Take a random sample of the exponential distribution exp(-mean*x). The
     * probability distribution function is mean*exp(-mean*x), so the CDF is
     *     p = 1 - exp(-mean*x), so
     *     q = 1 - p == exp(-mean*x)
     *     log_e(q) = -mean*x
     *     -log_e(q)/mean = x
     *     x = -log_e(q) * mean
     *     x = log_2(q) * (-log_e(2)) * mean    ; Using log_2 for efficiency */
    enum { RANDOM_BIT_COUNT = 26 };
    /* cheaprandn(1<<randomBitCount) */
    uint32_t r = (uint32_t)(runtime_rand64() >> 32);
    uint32_t q = (uint32_t)(((uint64_t)r * (1U << RANDOM_BIT_COUNT)) >> 32) + 1;
    double qlog = mprof_fastlog2((double)q) - (double)RANDOM_BIT_COUNT;
    if (qlog > 0)
        qlog = 0;
    const double minus_log2 = -0.6931471805599453; /* -ln(2) */
    return (int32_t)(qlog * (minus_log2 * (double)mean)) + 1;
}

/* nextSample returns the next sampling point for heap profiling. The goal is to
 * sample allocations on average every MemProfileRate bytes, but with a
 * completely random distribution over the allocation timeline; this corresponds
 * to a Poisson process with parameter MemProfileRate. In Poisson processes, the
 * distance between two samples follows the exponential distribution
 * (exp(MemProfileRate)), so the best return value is a random number taken from
 * an exponential distribution whose mean is MemProfileRate. */
int64_t burrow__mprof_next_sample(Int rate) {
    if (rate == 0)
        return INT64_MAX; /* Basically never sample. */
    if (rate == 1)
        return 0; /* Sample immediately. */
    return (int64_t)mprof_fastexprand(rate);
}

static bool mprof_eqslice(const Uintptr *x, Int nx, const Uintptr *y, Int ny) {
    if (nx != ny)
        return false;
    for (Int i = 0; i < nx; i++)
        if (x[i] != y[i])
            return false;
    return true;
}

/* Return the bucket for stk[0:nstk], allocating new bucket if needed. NULL when
 * memory has run out. Called with the lock held. */
static MprofBucket *mprof_stkbucket(Uintptr size, const Uintptr *stk, Int nstk) {
    Alloc *a = burrow__heap_unprofiled();
    if (mprof.buckhash == NULL) {
        mprof.buckhash = (MprofBucket **)mem_alloc_array(
            a, MPROF_BUCK_HASH_SIZE, sizeof(MprofBucket *), _Alignof(MprofBucket *));
        if (mprof.buckhash == NULL)
            return NULL;
    }

    /* Hash stack. */
    Uintptr h = 0;
    for (Int i = 0; i < nstk; i++) {
        h += stk[i];
        h += h << 10;
        h ^= h >> 6;
    }
    /* hash in size */
    h += size;
    h += h << 10;
    h ^= h >> 6;
    /* finalize */
    h += h << 3;
    h ^= h >> 11;

    Uintptr i = h % MPROF_BUCK_HASH_SIZE;
    for (MprofBucket *b = mprof.buckhash[i]; b != NULL; b = b->next)
        if (b->hash == h && b->size == size && mprof_eqslice(b->stk, b->nstk, stk, nstk))
            return b;

    /* Create new bucket. */
    MprofBucket *b = (MprofBucket *)mem_alloc(
        a, sizeof(MprofBucket) + (size_t)nstk * sizeof(Uintptr), _Alignof(MprofBucket));
    if (b == NULL)
        return NULL;
    memcpy(b->stk, stk, (size_t)nstk * sizeof(Uintptr));
    b->nstk = nstk;
    b->hash = h;
    b->size = size;

    b->next = mprof.buckhash[i];
    b->allnext = mprof.mbuckets;
    mprof.buckhash[i] = b;
    mprof.mbuckets = b;
    return b;
}

static uint64_t mprof_ptr_hash(Uintptr p) {
    return (uint64_t)p * 0x9E3779B97F4A7C15U;
}

static Int mprof_filter_index(Uintptr p) {
    return (Int)(mprof_ptr_hash(p) >> (64 - MPROF_FILTER_BITS));
}

static void mprof_filter_add(Uintptr p, int32_t delta) {
    uint32_t *c = (uint32_t *)mprof.filter + mprof_filter_index(p);
    burrow__atomic_store_relaxed_u32(c, (uint32_t)((int32_t)*c + delta));
}

/* The slot p is in, or the empty slot it would go in. */
static Int mprof_slot(const MprofSampled *t, Int cap, Uintptr p) {
    Int mask = cap - 1;
    Int i = (Int)(mprof_ptr_hash(p) >> 32) & mask;
    while (t[i].p != 0 && t[i].p != p)
        i = (i + 1) & mask;
    return i;
}

static bool mprof_grow_sampled(void) {
    Alloc *a = burrow__heap_unprofiled();
    if (mprof.filter == NULL) {
        uint32_t *f = (uint32_t *)mem_alloc_array(a, (size_t)1 << MPROF_FILTER_BITS,
                                                  sizeof(uint32_t), _Alignof(uint32_t));
        if (f == NULL)
            return false;
        burrow__atomic_store_release_ptr(&mprof.filter, f);
    }
    Int cap = mprof.cap_sampled == 0 ? 256 : mprof.cap_sampled * 2;
    MprofSampled *t = (MprofSampled *)mem_alloc_array(a, (size_t)cap, sizeof(MprofSampled),
                                                      _Alignof(MprofSampled));
    if (t == NULL)
        return false;
    for (Int i = 0; i < mprof.cap_sampled; i++)
        if (mprof.sampled[i].p != 0)
            t[mprof_slot(t, cap, mprof.sampled[i].p)] = mprof.sampled[i];
    mem_free(a, mprof.sampled, (size_t)mprof.cap_sampled * sizeof(MprofSampled),
             _Alignof(MprofSampled));
    mprof.sampled = t;
    mprof.cap_sampled = cap;
    return true;
}

/* Takes slot i out of the table, moving up whatever later in its run would no
 * longer be found past the hole. */
static void mprof_delete_slot(Int i) {
    MprofSampled *t = mprof.sampled;
    Int mask = mprof.cap_sampled - 1;
    Int j = i;
    for (;;) {
        t[i].p = 0;
        t[i].b = NULL;
        for (;;) {
            j = (j + 1) & mask;
            if (t[j].p == 0)
                return;
            Int k = (Int)(mprof_ptr_hash(t[j].p) >> 32) & mask;
            /* Leave t[j] where it is when its home k lies cyclically in
             * (i, j]. */
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
                continue;
            break;
        }
        t[i] = t[j];
        i = j;
    }
}

/* Called with the lock held. */
static void mprof_unsample(Int i) {
    mprof.sampled[i].b->frees++;
    mprof_filter_add(mprof.sampled[i].p, -1);
    mprof_delete_slot(i);
    mprof.nsampled--;
}

/* Called by malloc to record a profiled block. */
void burrow__mprof_malloc(void *p, size_t size) {
    Uintptr stk[MPROF_STACK_DEPTH];
    /* Skip runtime_callers and this function, so that the stack starts in the
     * allocator that called it. */
    Int nstk = runtime_callers(2, slice_from(stk, MPROF_STACK_DEPTH, MPROF_STACK_DEPTH,
                                             TYPE_UINTPTR));

    burrow__lock(&mprof.lock);
    MprofBucket *b = mprof_stkbucket((Uintptr)size, stk, nstk);
    if (b == NULL) {
        burrow__unlock(&mprof.lock);
        return;
    }
    b->allocs++;

    /* Not in Go: a pointer can be freed behind the profiler's back, by the C
     * library's free, and then come back from malloc. The old record is counted
     * as freed and replaced. */
    if (mprof.nsampled + 1 > mprof.cap_sampled / 2 && !mprof_grow_sampled()) {
        burrow__unlock(&mprof.lock);
        return;
    }
    Int i = mprof_slot(mprof.sampled, mprof.cap_sampled, (Uintptr)p);
    if (mprof.sampled[i].p != 0) {
        mprof_unsample(i);
        i = mprof_slot(mprof.sampled, mprof.cap_sampled, (Uintptr)p);
    }
    mprof.sampled[i].p = (Uintptr)p;
    mprof.sampled[i].b = b;
    mprof.nsampled++;
    mprof_filter_add((Uintptr)p, 1);
    burrow__unlock(&mprof.lock);
}

bool burrow__mprof_maybe_sampled(const void *p) {
    uint32_t *f = (uint32_t *)burrow__atomic_load_acquire_ptr(&mprof.filter);
    if (f == NULL)
        return false;
    return burrow__atomic_load_relaxed_u32(&f[mprof_filter_index((Uintptr)p)]) != 0;
}

/* Called when freeing a profiled block. */
void burrow__mprof_free(const void *p) {
    burrow__lock(&mprof.lock);
    if (mprof.cap_sampled != 0) {
        Int i = mprof_slot(mprof.sampled, mprof.cap_sampled, (Uintptr)p);
        if (mprof.sampled[i].p != 0)
            mprof_unsample(i);
    }
    burrow__unlock(&mprof.lock);
}

/* memProfileInternal returns the number of records n in the profile. If there
 * are less than size records, copyFn is invoked for each record, and ok returns
 * true. */
Int burrow__mem_profile(Int size, bool inuse_zero, burrow__MemProfileFn fn, void *env,
                        bool *ok) {
    Int n = 0;
    burrow__lock(&mprof.lock);
    MprofBucket *head = mprof.mbuckets;
    for (MprofBucket *b = head; b != NULL; b = b->allnext)
        if (inuse_zero || b->allocs != b->frees)
            n++;
    bool done = false;
    if (n <= size) {
        done = true;
        for (MprofBucket *b = head; b != NULL; b = b->allnext) {
            if (inuse_zero || b->allocs != b->frees) {
                burrow__MemProfileRecord r = {
                    .object_size = (int64_t)b->size,
                    .alloc_objects = (int64_t)b->allocs,
                    .free_objects = (int64_t)b->frees,
                    .stack = b->stk,
                    .nstack = b->nstk,
                };
                fn(env, &r);
            }
        }
    }
    burrow__unlock(&mprof.lock);
    if (ok != NULL)
        *ok = done;
    return n;
}

int64_t runtime_mem_profile_record_in_use_bytes(const RuntimeMemProfileRecord *r) {
    return r->alloc_bytes - r->free_bytes;
}

int64_t runtime_mem_profile_record_in_use_objects(const RuntimeMemProfileRecord *r) {
    return r->alloc_objects - r->free_objects;
}

Int runtime_mem_profile_record_stack(const RuntimeMemProfileRecord *r) {
    for (Int i = 0; i < MPROF_STACK0; i++)
        if (r->stack0[i] == 0)
            return i;
    return MPROF_STACK0;
}

static void mprof_copy_record(void *env, const burrow__MemProfileRecord *src) {
    RuntimeMemProfileRecord **next = (RuntimeMemProfileRecord **)env;
    RuntimeMemProfileRecord *dst = (*next)++;
    dst->alloc_bytes = src->alloc_objects * src->object_size;
    dst->free_bytes = src->free_objects * src->object_size;
    dst->alloc_objects = src->alloc_objects;
    dst->free_objects = src->free_objects;
    Int i = src->nstack < MPROF_STACK0 ? src->nstack : MPROF_STACK0;
    memcpy(dst->stack0, src->stack, (size_t)i * sizeof(Uintptr));
    memset(dst->stack0 + i, 0, ((size_t)MPROF_STACK0 - (size_t)i) * sizeof(Uintptr));
}

Int runtime_mem_profile(RuntimeMemProfileRecord *p, Int len, bool inuse_zero,
                        bool *ok) {
    RuntimeMemProfileRecord *next = p;
    return burrow__mem_profile(len, inuse_zero, mprof_copy_record, &next, ok);
}
