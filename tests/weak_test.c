/* weak's tests, from Go's pointer_test.go, with runtime.GC replaced by giving
 * the memory back, which is what makes a value go away under every allocator
 * but gc. The finalizer and cleanup tests wait for runtime.SetFinalizer and
 * runtime.AddCleanup. TestIssue69210 and TestPointerTiny are about how Go's
 * collector marks and batches objects, which has no counterpart here. The rest
 * of the tests are for the allocators, which Go does not have.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/weak.h"

#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/gc.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include "check.h"

#include <string.h>

typedef struct T {
    struct T *t;
    Int a;
    Int b;
} T;

static T *new_t(Alloc *a) {
    return BURROW_NEW(a, T);
}

static void free_t(Alloc *a, T *p) {
    mem_free(a, p, sizeof(T), _Alignof(T));
}

static void TestPointer(TestingT *t) {
    WeakPointer zero = {0};
    if (weak_pointer_value(zero) != NULL)
        testing_t_errorf_v(t, "Value of zero value of weak.Pointer is not nil");
    WeakPointer zero_nil = weak_make(NULL);
    if (weak_pointer_value(zero_nil) != NULL)
        testing_t_errorf_v(t, "Value of weak.Make[T](nil) is not nil");

    Alloc *a = heap_allocator();
    T *bt = new_t(a);
    WeakPointer wt = weak_make(bt);
    if (weak_pointer_value(wt) != bt)
        testing_t_fatalf_v(t, "weak pointer is not the same as strong pointer");
    /* bt is still allocated. */
    if (weak_pointer_value(wt) != bt)
        testing_t_fatalf_v(
            t, "weak pointer is not the same as strong pointer after a while");
    free_t(a, bt);
    if (weak_pointer_value(wt) != NULL)
        testing_t_fatalf_v(t, "expected weak pointer to be nil");
}

enum { NEQ = 10 };

static void TestPointerEquality(TestingT *t) {
    WeakPointer zero = {0};
    if (!weak_pointer_eq(zero, weak_make(NULL)))
        testing_t_errorf_v(t, "weak.Make[T](nil) != zero value of weak.Pointer[T]");

    Alloc *a = heap_allocator();
    T *bt[NEQ];
    WeakPointer wt[NEQ];
    WeakPointer wo[NEQ];
    for (int i = 0; i < NEQ; i++) {
        bt[i] = new_t(a);
        wt[i] = weak_make(bt[i]);
        wo[i] = weak_make(&bt[i]->a);
    }
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < NEQ; i++) {
            T *st = weak_pointer_value(wt[i]);
            if (st != bt[i])
                testing_t_fatalf_v(t, "weak pointer is not the same as strong pointer");
            if (!weak_pointer_eq(weak_make(st), wt[i]))
                testing_t_fatalf_v(
                    t, "new weak pointer not equal to existing weak pointer");
            if (!weak_pointer_eq(weak_make(&st->a), wo[i]))
                testing_t_fatalf_v(
                    t, "new weak pointer not equal to existing weak pointer");
            if (weak_pointer_eq(wt[i], wo[i]))
                testing_t_fatalf_v(t,
                                   "pointers to two offsets in one object are equal");
            if (i > 0 && weak_pointer_eq(wt[i], wt[i - 1]))
                testing_t_fatalf_v(
                    t, "expected weak pointers to not be equal to each other");
        }
    }
    for (int i = 0; i < NEQ; i++)
        free_t(a, bt[i]);
    for (int i = 0; i < NEQ; i++) {
        if (weak_pointer_value(wt[i]) != NULL)
            testing_t_fatalf_v(t, "expected weak pointer to be nil");
        if (weak_pointer_value(wo[i]) != NULL)
            testing_t_fatalf_v(t,
                               "expected weak pointer to an interior field to be nil");
        if (i > 0 && weak_pointer_eq(wt[i], wt[i - 1]))
            testing_t_fatalf_v(t,
                               "expected weak pointers to not be equal to each other");
    }
}

static void TestPointerSize(TestingT *t) {
    if (sizeof(WeakPointer) != 8)
        testing_t_errorf_v(t, "sizeof(WeakPointer) = %v, want 8",
                           (Int)sizeof(WeakPointer));
}

static void TestIssue70739(TestingT *t) {
    Alloc *a = heap_allocator();
    size_t n = (size_t)4 << 16;
    Int **x = mem_alloc_array(a, n, sizeof(Int *), _Alignof(Int *));
    if (x == NULL)
        testing_t_fatalf_v(t, "out of memory");
    WeakPointer wx1 = weak_make(&x[1 << 16]);
    WeakPointer wx2 = weak_make(&x[1 << 16]);
    if (!weak_pointer_eq(wx1, wx2))
        testing_t_fatalf_v(t,
                           "failed to look up special and made duplicate weak handle");
    mem_free(a, x, n * sizeof(Int *), _Alignof(Int *));
    if (weak_pointer_value(wx1) != NULL)
        testing_t_fatalf_v(t, "weak pointer into a freed array is not nil");
}

static T immortal;

static void TestImmortalPointer(TestingT *t) {
    WeakPointer w0 = weak_make(&immortal);
    if (!weak_pointer_eq(weak_make(&immortal), w0))
        testing_t_errorf_v(t, "immortal weak pointers to the same pointer not equal");
    WeakPointer w0a = weak_make(&immortal.a);
    WeakPointer w0b = weak_make(&immortal.b);
    if (weak_pointer_eq(w0a, w0b))
        testing_t_errorf_v(
            t, "separate immortal pointers (same object) have the same pointer");

    /* Other memory coming and going leaves them alone. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (int i = 0; i < 2; i++) {
        (void)weak_make(new_t(arena_allocator(&ar)));
        arena_reset(&ar);
    }
    arena_free(&ar);

    if (weak_pointer_value(w0) != &immortal)
        testing_t_errorf_v(t, "immortal weak pointer has unexpected Value");
    if (weak_pointer_value(w0a) != &immortal.a)
        testing_t_errorf_v(t, "immortal weak pointer has unexpected Value");
    if (weak_pointer_value(w0b) != &immortal.b)
        testing_t_errorf_v(t, "immortal weak pointer has unexpected Value");
}

/* The same address handed out again is a new object, and a WeakPointer to the
 * old one neither sees it nor equals one made from it. */
static void TestAddressReused(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    T *p = new_t(a);
    WeakPointer w1 = weak_make(p);
    arena_reset(&ar);
    T *q = new_t(a);
    if (q != p)
        testing_t_skipf_v(t, "the arena did not hand out the same address again");
    if (weak_pointer_value(w1) != NULL)
        testing_t_errorf_v(t, "weak pointer to the old object sees the new one");
    WeakPointer w2 = weak_make(q);
    if (weak_pointer_eq(w1, w2))
        testing_t_errorf_v(t, "weak pointers to the old and new objects are equal");
    if (weak_pointer_value(w2) != q)
        testing_t_errorf_v(t, "weak pointer to the new object does not see it");
    arena_free(&ar);
    if (weak_pointer_value(w2) != NULL)
        testing_t_errorf_v(t, "weak pointer survives arena_free");
}

static void TestArenaRelease(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 256);
    Alloc *a = arena_allocator(&ar);
    T *keep = new_t(a);
    WeakPointer wk = weak_make(keep);
    ArenaMark m = arena_mark(&ar);
    enum { N = 64 };
    WeakPointer w[N];
    for (int i = 0; i < N; i++)
        w[i] = weak_make(new_t(a));
    arena_release(&ar, m);
    for (int i = 0; i < N; i++)
        if (weak_pointer_value(w[i]) != NULL)
            testing_t_fatalf_v(t, "weak pointer %v made after the mark is not nil", i);
    if (weak_pointer_value(wk) != keep)
        testing_t_errorf_v(t, "weak pointer made before the mark went nil");
    arena_free(&ar);
    if (weak_pointer_value(wk) != NULL)
        testing_t_errorf_v(t, "weak pointer survives arena_free");
}

static void TestFixedReset(TestingT *t) {
    static unsigned char buf[1024];
    Fixed fx;
    fixed_init(&fx, buf, sizeof(buf));
    Alloc *a = fixed_allocator(&fx);
    T *p = new_t(a);
    WeakPointer w = weak_make(p);
    if (weak_pointer_value(w) != p)
        testing_t_fatalf_v(t, "weak pointer is not the same as strong pointer");
    mem_reset(a);
    if (weak_pointer_value(w) != NULL)
        testing_t_errorf_v(t, "weak pointer survives fixed_reset");
}

static void TestRealloc(TestingT *t) {
    Alloc *a = heap_allocator();
    Byte *p = mem_alloc(a, 16, 1);
    if (p == NULL)
        testing_t_fatalf_v(t, "out of memory");
    WeakPointer w = weak_make(p + 3);
    Byte *q = mem_realloc(a, p, 16, 4096, 1);
    if (q == NULL)
        testing_t_fatalf_v(t, "out of memory");
    if (weak_pointer_value(w) != NULL)
        testing_t_errorf_v(t, "weak pointer into a reallocated block is not nil");
    WeakPointer w2 = weak_make(q + 3);
    if (weak_pointer_value(w2) != q + 3)
        testing_t_errorf_v(t, "weak pointer into the new block does not see it");
    mem_free(a, q, 4096, 1);
}

/* Freeing one block leaves the weak pointers into its neighbours alone. */
static void TestNeighbours(TestingT *t) {
    Alloc *a = heap_allocator();
    Byte *blk = mem_alloc(a, 64, 1);
    if (blk == NULL)
        testing_t_fatalf_v(t, "out of memory");
    WeakPointer before = weak_make(blk + 15);
    WeakPointer first = weak_make(blk + 16);
    WeakPointer last = weak_make(blk + 31);
    WeakPointer after = weak_make(blk + 32);
    /* What burrow__mem_forget is told for a block of 16 at blk + 16, without
     * freeing anything, through a Fixed over the same bytes. */
    Fixed fx;
    fixed_init(&fx, blk + 16, 16);
    Alloc *f = fixed_allocator(&fx);
    if (mem_alloc(f, 16, 1) != blk + 16)
        testing_t_fatalf_v(t, "the fixed allocator did not start at its buffer");
    fixed_reset(&fx);
    if (weak_pointer_value(first) != NULL || weak_pointer_value(last) != NULL)
        testing_t_errorf_v(t, "weak pointers into the reset range are not nil");
    if (weak_pointer_value(before) != blk + 15)
        testing_t_errorf_v(t, "weak pointer just before the range went nil");
    if (weak_pointer_value(after) != blk + 32)
        testing_t_errorf_v(t, "weak pointer just after the range went nil");
    mem_free(a, blk, 64, 1);
}

/* Under the gc allocator the collector decides, as in Go, and mem_free does
 * nothing. The collector scans the stack conservatively, so a stale copy of
 * one pointer can keep its object alive, which is why this makes many and
 * only asks that most go. */
enum { GC_N = 256 };

static BURROW_NOINLINE void gc_make(Alloc *g, WeakPointer *w) {
    for (int i = 0; i < GC_N; i++)
        w[i] = weak_make(new_t(g));
}

static void TestGC(TestingT *t) {
    if (!gc_available())
        testing_t_skipf_v(t, "built without the collector");
    Alloc *g = gc_allocator();
    T *p = new_t(g);
    WeakPointer w = weak_make(p);
    free_t(g, p);
    if (weak_pointer_value(w) != p)
        testing_t_errorf_v(t, "mem_free on the gc allocator cleared a weak pointer");

    static WeakPointer ws[GC_N];
    gc_make(g, ws);
    for (int i = 0; i < 3; i++)
        gc_collect();
    int left = 0;
    for (int i = 0; i < GC_N; i++)
        if (weak_pointer_value(ws[i]) != NULL)
            left++;
    if (left > GC_N / 2)
        testing_t_errorf_v(t, "%v of %v weak pointers survived collection", left, GC_N);
}

enum { CONC_WORKERS = 8, CONC_ROUNDS = 2000 };

typedef struct ConcEnv {
    SyncWaitGroup *wg;
    int bad;
} ConcEnv;

static void conc_worker(void *arg) {
    ConcEnv *e = arg;
    Alloc *a = heap_allocator();
    for (int i = 0; i < CONC_ROUNDS; i++) {
        T *p = new_t(a);
        if (p == NULL)
            continue;
        WeakPointer w = weak_make(p);
        if (weak_pointer_value(w) != p || !weak_pointer_eq(weak_make(p), w))
            e->bad++;
        free_t(a, p);
        if (weak_pointer_value(w) != NULL)
            e->bad++;
    }
    sync_wait_group_done(e->wg);
}

static void TestConcurrent(TestingT *t) {
    static ConcEnv envs[CONC_WORKERS];
    SyncWaitGroup wg = {0};
    for (int w = 0; w < CONC_WORKERS; w++) {
        envs[w].wg = &wg;
        envs[w].bad = 0;
        sync_wait_group_add(&wg, 1);
        if (!go(BURROW_FN(Func, conc_worker, &envs[w])))
            testing_t_fatalf_v(t, "go failed");
    }
    sync_wait_group_wait(&wg);
    for (int w = 0; w < CONC_WORKERS; w++)
        if (envs[w].bad != 0)
            testing_t_errorf_v(t, "worker %v saw %v wrong values", w, envs[w].bad);
}

static void BenchmarkMake(TestingB *b) {
    Alloc *a = heap_allocator();
    T *p = new_t(a);
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)weak_make(p);
    free_t(a, p);
}

static void BenchmarkValue(TestingB *b) {
    Alloc *a = heap_allocator();
    T *p = new_t(a);
    WeakPointer w = weak_make(p);
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)weak_pointer_value(w);
    free_t(a, p);
}

/* What every free costs once weak pointers exist, with the table holding
 * 1000 of them. */
static void BenchmarkFree(TestingB *b) {
    Alloc *a = heap_allocator();
    enum { KEEP = 1000 };
    static T *keep[KEEP];
    for (int i = 0; i < KEEP; i++) {
        keep[i] = new_t(a);
        (void)weak_make(keep[i]);
    }
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        free_t(a, new_t(a));
    testing_b_stop_timer(b);
    for (int i = 0; i < KEEP; i++)
        free_t(a, keep[i]);
}

#define TESTS(X)                                                                       \
    X(TestPointer)                                                                     \
    X(TestPointerEquality)                                                             \
    X(TestPointerSize)                                                                 \
    X(TestIssue70739)                                                                  \
    X(TestImmortalPointer)                                                             \
    X(TestAddressReused)                                                               \
    X(TestArenaRelease)                                                                \
    X(TestFixedReset)                                                                  \
    X(TestRealloc)                                                                     \
    X(TestNeighbours)                                                                  \
    X(TestGC)                                                                          \
    X(TestConcurrent)                                                                  \
    X(BenchmarkMake)                                                                   \
    X(BenchmarkValue)                                                                  \
    X(BenchmarkFree)

TESTING_MAIN(TESTS)
