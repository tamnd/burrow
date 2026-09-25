/* Derived from Go's src/container/ring/ring_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/container/ring.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/track.h"

typedef struct Tally {
    Int n;
    Int s;
} Tally;

static void tally(void *env, Any p) {
    Tally *c = env;
    c->n++;
    const Int *i = any_assert(p, TYPE_INT);
    if (i != NULL)
        c->s += *i;
}

static void verify(TestingT *t, Ring *r, Int N, Int sum) {
    /* Len */
    Int n = ring_len(r);
    if (n != N)
        testing_t_errorf_v(t, "r.Len() == %d; expected %d", n, N);

    /* iteration */
    Tally c = {0, 0};
    ring_do(r, BURROW_FN(RingDoFunc, tally, &c));
    if (c.n != N)
        testing_t_errorf_v(t, "number of forward iterations == %d; expected %d", c.n,
                           N);
    if (sum >= 0 && c.s != sum)
        testing_t_errorf_v(t, "forward ring sum = %d; expected %d", c.s, sum);

    if (r == NULL)
        return;

    /* connections */
    if (r->next != NULL) {
        Ring *p = NULL; /* previous element */
        for (Ring *q = r; p == NULL || q != r; q = q->next) {
            if (p != NULL && p != q->prev)
                testing_t_errorf_v(t, "prev = %p, expected q.prev = %p", (void *)p,
                                   (void *)q->prev);
            p = q;
        }
        if (p != r->prev)
            testing_t_errorf_v(t, "prev = %p, expected r.prev = %p", (void *)p,
                               (void *)r->prev);
    }

    /* Next, Prev */
    if (ring_next(r) != r->next)
        testing_t_errorf_v(t, "r.Next() != r.next");
    if (ring_prev(r) != r->prev)
        testing_t_errorf_v(t, "r.Prev() != r.prev");

    /* Move */
    if (ring_move(r, 0) != r)
        testing_t_errorf_v(t, "r.Move(0) != r");
    if (ring_move(r, N) != r)
        testing_t_errorf_v(t, "r.Move(%d) != r", N);
    if (ring_move(r, -N) != r)
        testing_t_errorf_v(t, "r.Move(%d) != r", -N);
    for (Int i = 0; i < 10; i++) {
        Int ni = N + i;
        Int mi = ni % N;
        if (ring_move(r, ni) != ring_move(r, mi))
            testing_t_errorf_v(t, "r.Move(%d) != r.Move(%d)", ni, mi);
        if (ring_move(r, -ni) != ring_move(r, -mi))
            testing_t_errorf_v(t, "r.Move(%d) != r.Move(%d)", -ni, -mi);
    }
}

static void TestCornerCases(TestingT *t) {
    Ring *r0 = NULL;
    Ring r1 = {0};
    /* Basics */
    verify(t, r0, 0, 0);
    verify(t, &r1, 1, 0);
    /* Insert */
    ring_link(&r1, r0);
    verify(t, r0, 0, 0);
    verify(t, &r1, 1, 0);
    /* Insert */
    ring_link(&r1, r0);
    verify(t, r0, 0, 0);
    verify(t, &r1, 1, 0);
    /* Unlink */
    ring_unlink(&r1, 0);
    verify(t, &r1, 1, 0);
}

/* The values are boxed in the same allocator as the ring, since a Ring does not
 * copy what it is given. */
static Ring *make_n(Alloc *a, Int n) {
    Ring *r = ring_new(a, n);
    for (Int i = 1; i <= n; i++) {
        r->value = any_box(a, BURROW_ANY_OF(i));
        r = ring_next(r);
    }
    return r;
}

static Int sum_n(Int n) {
    return (n * n + n) / 2;
}

static void TestNew(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < 10; i++) {
        Ring *r = ring_new(a, i);
        verify(t, r, i, -1);
    }
    for (Int i = 0; i < 10; i++) {
        Ring *r = make_n(a, i);
        verify(t, r, i, sum_n(i));
    }
    arena_free(&ar);
}

static void TestLink1(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Ring *r1a = make_n(a, 1);
    Ring r1b = {0};
    Ring *r2a = ring_link(r1a, &r1b);
    verify(t, r2a, 2, 1);
    if (r2a != r1a)
        testing_t_errorf_v(t, "a) 2-element link failed");

    Ring *r2b = ring_link(r2a, ring_next(r2a));
    verify(t, r2b, 2, 1);
    if (r2b != ring_next(r2a))
        testing_t_errorf_v(t, "b) 2-element link failed");

    Ring *r1c = ring_link(r2b, r2b);
    verify(t, r1c, 1, 1);
    verify(t, r2b, 1, 0);
    arena_free(&ar);
}

static void TestLink2(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Ring *r0 = NULL;
    Int v42 = 42;
    Int v77 = 77;
    Ring r1a = {.value = BURROW_ANY(TYPE_INT, &v42)};
    Ring r1b = {.value = BURROW_ANY(TYPE_INT, &v77)};
    Ring *r10 = make_n(a, 10);

    ring_link(&r1a, r0);
    verify(t, &r1a, 1, 42);

    ring_link(&r1a, &r1b);
    verify(t, &r1a, 2, 42 + 77);

    ring_link(r10, r0);
    verify(t, r10, 10, sum_n(10));

    ring_link(r10, &r1a);
    verify(t, r10, 12, sum_n(10) + 42 + 77);
    arena_free(&ar);
}

static void TestLink3(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Ring r = {0};
    Int n = 1;
    for (Int i = 1; i < 10; i++) {
        n += i;
        verify(t, ring_link(&r, ring_new(a, i)), n, -1);
    }
    arena_free(&ar);
}

static void TestUnlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Ring *r10 = make_n(a, 10);
    Ring *s10 = ring_move(r10, 6);

    Int sum10 = sum_n(10);

    verify(t, r10, 10, sum10);
    verify(t, s10, 10, sum10);

    Ring *r0 = ring_unlink(r10, 0);
    verify(t, r0, 0, 0);

    Ring *r1 = ring_unlink(r10, 1);
    verify(t, r1, 1, 2);
    verify(t, r10, 9, sum10 - 2);

    Ring *r9 = ring_unlink(r10, 9);
    verify(t, r9, 9, sum10 - 2);
    verify(t, r10, 9, sum10 - 2);
    arena_free(&ar);
}

static void TestLinkUnlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 1; i < 4; i++) {
        Ring *ri = ring_new(a, i);
        for (Int j = 0; j < i; j++) {
            Ring *rj = ring_unlink(ri, j);
            verify(t, rj, j, -1);
            verify(t, ri, i - j, -1);
            ring_link(ri, rj);
            verify(t, ri, i, -1);
        }
    }
    arena_free(&ar);
}

/* Test that calling Move() on an empty Ring initializes it. */
static void TestMoveEmptyRing(TestingT *t) {
    Ring r = {0};

    ring_move(&r, 1);
    verify(t, &r, 1, 0);
}

/* ring_free gives back every element, including after links and unlinks have
 * moved them between rings. */
static void TestFreeGivesEverythingBack(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);

    Ring *r = ring_new(a, 7);
    CHECK_INT_EQ(ring_len(r), 7);
    Ring *s = ring_new(a, 3);
    ring_link(r, s);
    CHECK_INT_EQ(ring_len(r), 10);
    Ring *u = ring_unlink(r, 4);
    CHECK_INT_EQ(ring_len(u), 4);
    CHECK_INT_EQ(ring_len(r), 6);
    ring_free(a, r);
    ring_free(a, u);
    ring_free(a, NULL);

    CHECK_INT_EQ(track_live(&tr), 0);
    CHECK_INT_EQ(track_faults(&tr), 0);
    track_free(&tr);
}

/* A ring_new that runs out partway returns NULL. Fixed does not take memory
 * back, so it is reset before the second try. */
static void TestOutOfMemory(TestingT *t) {
    unsigned char buf[256];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    CHECK(ring_new(a, 1000) == NULL);
    fixed_reset(&fx);
    Ring *r = ring_new(a, 2);
    CHECK(r != NULL);
    CHECK_INT_EQ(ring_len(r), 2);
}

#define TESTS(X)                                                                       \
    X(TestCornerCases)                                                                 \
    X(TestNew)                                                                         \
    X(TestLink1)                                                                       \
    X(TestLink2)                                                                       \
    X(TestLink3)                                                                       \
    X(TestUnlink)                                                                      \
    X(TestLinkUnlink)                                                                  \
    X(TestMoveEmptyRing)                                                               \
    X(TestFreeGivesEverythingBack)                                                     \
    X(TestOutOfMemory)

TESTING_MAIN(TESTS)
