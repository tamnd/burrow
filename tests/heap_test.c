/* Derived from Go's src/container/heap/heap_test.go and
 * example_intheap_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/container/heap.h"

/* Go's myHeap, a []int. The array is big enough for every test, and pop
 * returns a pointer to the element it just dropped, which stays valid until
 * the next push. */
typedef struct MyHeap {
    Int xs[256];
    Int n;
} MyHeap;

static Int my_len(void *self) {
    return ((MyHeap *)self)->n;
}

static bool my_less(void *self, Int i, Int j) {
    MyHeap *h = self;
    return h->xs[i] < h->xs[j];
}

static void my_swap(void *self, Int i, Int j) {
    MyHeap *h = self;
    Int t = h->xs[i];
    h->xs[i] = h->xs[j];
    h->xs[j] = t;
}

static void my_push(void *self, Any x) {
    MyHeap *h = self;
    const Int *v = any_assert(x, TYPE_INT);
    if (v != NULL && h->n < (Int)(sizeof h->xs / sizeof h->xs[0]))
        h->xs[h->n++] = *v;
}

static Any my_pop(void *self) {
    MyHeap *h = self;
    h->n--;
    return BURROW_ANY(TYPE_INT, &h->xs[h->n]);
}

static const HeapInterfaceVT my_vt = {
    {NULL, my_len, my_less, my_swap}, my_push, my_pop};

static HeapInterface my_heap(MyHeap *h) {
    return (HeapInterface){&my_vt, h};
}

static Int as_int(Any v) {
    const Int *i = any_assert(v, TYPE_INT);
    return i != NULL ? *i : -1;
}

static void verify(TestingT *t, MyHeap *h, Int i) {
    Int n = h->n;
    Int j1 = 2 * i + 1;
    Int j2 = 2 * i + 2;
    if (j1 < n) {
        if (my_less(h, j1, i)) {
            testing_t_errorf_v(t, "heap invariant invalidated [%d] = %d > [%d] = %d", i,
                               h->xs[i], j1, h->xs[j1]);
            return;
        }
        verify(t, h, j1);
    }
    if (j2 < n) {
        if (my_less(h, j2, i)) {
            testing_t_errorf_v(t, "heap invariant invalidated [%d] = %d > [%d] = %d", i,
                               h->xs[i], j1, h->xs[j2]);
            return;
        }
        verify(t, h, j2);
    }
}

static void TestInit0(TestingT *t) {
    MyHeap h = {{0}, 0};
    for (Int i = 20; i > 0; i--)
        my_push(&h, BURROW_ANY_OF((Int)0)); /* all elements are the same */
    heap_init(my_heap(&h));
    verify(t, &h, 0);

    for (Int i = 1; h.n > 0; i++) {
        Int x = as_int(heap_pop(my_heap(&h)));
        verify(t, &h, 0);
        if (x != 0)
            testing_t_errorf_v(t, "%d.th pop got %d; want %d", i, x, 0);
    }
}

static void TestInit1(TestingT *t) {
    MyHeap h = {{0}, 0};
    for (Int i = 20; i > 0; i--)
        my_push(&h, BURROW_ANY_OF(i)); /* all elements are different */
    heap_init(my_heap(&h));
    verify(t, &h, 0);

    for (Int i = 1; h.n > 0; i++) {
        Int x = as_int(heap_pop(my_heap(&h)));
        verify(t, &h, 0);
        if (x != i)
            testing_t_errorf_v(t, "%d.th pop got %d; want %d", i, x, i);
    }
}

static void Test(TestingT *t) {
    MyHeap h = {{0}, 0};
    verify(t, &h, 0);

    for (Int i = 20; i > 10; i--)
        my_push(&h, BURROW_ANY_OF(i));
    heap_init(my_heap(&h));
    verify(t, &h, 0);

    for (Int i = 10; i > 0; i--) {
        heap_push(my_heap(&h), BURROW_ANY_OF(i));
        verify(t, &h, 0);
    }

    for (Int i = 1; h.n > 0; i++) {
        Int x = as_int(heap_pop(my_heap(&h)));
        if (i < 20)
            heap_push(my_heap(&h), BURROW_ANY_OF(20 + i));
        verify(t, &h, 0);
        if (x != i)
            testing_t_errorf_v(t, "%d.th pop got %d; want %d", i, x, i);
    }
}

static void TestRemove0(TestingT *t) {
    MyHeap h = {{0}, 0};
    for (Int i = 0; i < 10; i++)
        my_push(&h, BURROW_ANY_OF(i));
    verify(t, &h, 0);

    while (h.n > 0) {
        Int i = h.n - 1;
        Int x = as_int(heap_remove(my_heap(&h), i));
        if (x != i)
            testing_t_errorf_v(t, "Remove(%d) got %d; want %d", i, x, i);
        verify(t, &h, 0);
    }
}

static void TestRemove1(TestingT *t) {
    MyHeap h = {{0}, 0};
    for (Int i = 0; i < 10; i++)
        my_push(&h, BURROW_ANY_OF(i));
    verify(t, &h, 0);

    for (Int i = 0; h.n > 0; i++) {
        Int x = as_int(heap_remove(my_heap(&h), 0));
        if (x != i)
            testing_t_errorf_v(t, "Remove(0) got %d; want %d", x, i);
        verify(t, &h, 0);
    }
}

static void TestRemove2(TestingT *t) {
    enum { N = 10 };

    MyHeap h = {{0}, 0};
    for (Int i = 0; i < N; i++)
        my_push(&h, BURROW_ANY_OF(i));
    verify(t, &h, 0);

    /* Go uses a map[int]bool, and the values are 0 to N-1, so an array does. */
    bool m[N] = {false};
    Int len = 0;
    while (h.n > 0) {
        Int x = as_int(heap_remove(my_heap(&h), (h.n - 1) / 2));
        if (x >= 0 && x < N && !m[x]) {
            m[x] = true;
            len++;
        }
        verify(t, &h, 0);
    }

    if (len != N)
        testing_t_errorf_v(t, "len(m) = %d; want %d", len, (Int)N);
    for (Int i = 0; i < N; i++) {
        if (!m[i])
            testing_t_errorf_v(t, "m[%d] doesn't exist", i);
    }
}

/* Go uses math/rand here. Any sequence of indexes tests the same thing. */
static uint64_t rng_state = 1;

static Int rng_intn(Int n) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
    return (Int)((z ^ (z >> 31)) % (uint64_t)n);
}

static void TestFix(TestingT *t) {
    MyHeap h = {{0}, 0};
    verify(t, &h, 0);

    for (Int i = 200; i > 0; i -= 10)
        heap_push(my_heap(&h), BURROW_ANY_OF(i));
    verify(t, &h, 0);

    if (h.xs[0] != 10)
        testing_t_fatalf_v(t, "Expected head to be 10, was %d", h.xs[0]);
    h.xs[0] = 210;
    heap_fix(my_heap(&h), 0);
    verify(t, &h, 0);

    for (Int i = 100; i > 0; i--) {
        Int elem = rng_intn(h.n);
        if ((i & 1) == 0)
            h.xs[elem] *= 2;
        else
            h.xs[elem] /= 2;
        heap_fix(my_heap(&h), elem);
        verify(t, &h, 0);
    }
}

/* Example_intHeap, with the output checked here instead of by go test. */
static void TestExampleIntHeap(TestingT *t) {
    MyHeap h = {{2, 1, 5}, 3};
    heap_init(my_heap(&h));
    heap_push(my_heap(&h), BURROW_ANY_OF((Int)3));
    if (h.xs[0] != 1)
        testing_t_errorf_v(t, "minimum: %d, want 1", h.xs[0]);
    Int want[] = {1, 2, 3, 5};
    for (Int i = 0; h.n > 0; i++) {
        Int x = as_int(heap_pop(my_heap(&h)));
        if (i >= 4 || x != want[i])
            testing_t_errorf_v(t, "pop %d got %d", i, x);
    }
}

#define TESTS(X)                                                                       \
    X(TestInit0)                                                                       \
    X(TestInit1)                                                                       \
    X(Test)                                                                            \
    X(TestRemove0)                                                                     \
    X(TestRemove1)                                                                     \
    X(TestRemove2)                                                                     \
    X(TestFix)                                                                         \
    X(TestExampleIntHeap)

TESTING_MAIN(TESTS)
