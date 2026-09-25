/* Derived from Go's src/container/list/list_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/container/list.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/track.h"

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))
#define I(n) BURROW_ANY_OF((Int)(n))

static bool check_list_len(TestingT *t, const List *l, Int len) {
    Int n = list_len(l);
    if (n != len) {
        testing_t_errorf_v(t, "l.Len() = %d, want %d", n, len);
        return false;
    }
    return true;
}

static void check_list_pointers(TestingT *t, const List *l, ListElement *const *es,
                                Int n) {
    const ListElement *root = &l->root;

    if (!check_list_len(t, l, n))
        return;

    /* zero length lists must be the zero value or properly initialized
     * (sentinel circle) */
    if (n == 0) {
        if ((l->root.next != NULL && l->root.next != root) ||
            (l->root.prev != NULL && l->root.prev != root))
            testing_t_errorf_v(
                t, "l.root.next = %p, l.root.prev = %p; both should be nil or %p",
                (void *)l->root.next, (void *)l->root.prev, (const void *)root);
        return;
    }

    /* check internal and external prev/next connections */
    for (Int i = 0; i < n; i++) {
        ListElement *e = es[i];
        const ListElement *prev = root;
        const ListElement *Prev = NULL;
        if (i > 0) {
            prev = es[i - 1];
            Prev = prev;
        }
        if (e->prev != prev)
            testing_t_errorf_v(t, "elt[%d].prev = %p, want %p", i, (void *)e->prev,
                               (const void *)prev);
        if (list_element_prev(e) != Prev)
            testing_t_errorf_v(t, "elt[%d].Prev() = %p, want %p", i,
                               (void *)list_element_prev(e), (const void *)Prev);

        const ListElement *next = root;
        const ListElement *Next = NULL;
        if (i < n - 1) {
            next = es[i + 1];
            Next = next;
        }
        if (e->next != next)
            testing_t_errorf_v(t, "elt[%d].next = %p, want %p", i, (void *)e->next,
                               (const void *)next);
        if (list_element_next(e) != Next)
            testing_t_errorf_v(t, "elt[%d].Next() = %p, want %p", i,
                               (void *)list_element_next(e), (const void *)Next);
    }
}

#define CHECK_POINTERS(l, ...)                                                         \
    do {                                                                               \
        ListElement *const es_[] = {__VA_ARGS__};                                      \
        check_list_pointers(t, (l), es_, LEN(es_));                                    \
    } while (0)

#define CHECK_EMPTY(l) check_list_pointers(t, (l), NULL, 0)

static void TestList(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    List *l = list_new(arena_allocator(&ar));
    CHECK_EMPTY(l);

    /* Single element list */
    ListElement *e = list_push_front(l, BURROW_ANY_OF("a"));
    CHECK_POINTERS(l, e);
    list_move_to_front(l, e);
    CHECK_POINTERS(l, e);
    list_move_to_back(l, e);
    CHECK_POINTERS(l, e);
    list_remove(l, e);
    CHECK_EMPTY(l);

    /* Bigger list */
    ListElement *e2 = list_push_front(l, I(2));
    ListElement *e1 = list_push_front(l, I(1));
    ListElement *e3 = list_push_back(l, I(3));
    ListElement *e4 = list_push_back(l, BURROW_ANY_OF("banana"));
    CHECK_POINTERS(l, e1, e2, e3, e4);

    list_remove(l, e2);
    CHECK_POINTERS(l, e1, e3, e4);

    list_move_to_front(l, e3); /* move from middle */
    CHECK_POINTERS(l, e3, e1, e4);

    list_move_to_front(l, e1);
    list_move_to_back(l, e3); /* move from middle */
    CHECK_POINTERS(l, e1, e4, e3);

    list_move_to_front(l, e3); /* move from back */
    CHECK_POINTERS(l, e3, e1, e4);
    list_move_to_front(l, e3); /* should be no-op */
    CHECK_POINTERS(l, e3, e1, e4);

    list_move_to_back(l, e3); /* move from front */
    CHECK_POINTERS(l, e1, e4, e3);
    list_move_to_back(l, e3); /* should be no-op */
    CHECK_POINTERS(l, e1, e4, e3);

    e2 = list_insert_before(l, I(2), e1); /* insert before front */
    CHECK_POINTERS(l, e2, e1, e4, e3);
    list_remove(l, e2);
    e2 = list_insert_before(l, I(2), e4); /* insert before middle */
    CHECK_POINTERS(l, e1, e2, e4, e3);
    list_remove(l, e2);
    e2 = list_insert_before(l, I(2), e3); /* insert before back */
    CHECK_POINTERS(l, e1, e4, e2, e3);
    list_remove(l, e2);

    e2 = list_insert_after(l, I(2), e1); /* insert after front */
    CHECK_POINTERS(l, e1, e2, e4, e3);
    list_remove(l, e2);
    e2 = list_insert_after(l, I(2), e4); /* insert after middle */
    CHECK_POINTERS(l, e1, e4, e2, e3);
    list_remove(l, e2);
    e2 = list_insert_after(l, I(2), e3); /* insert after back */
    CHECK_POINTERS(l, e1, e4, e3, e2);
    list_remove(l, e2);

    /* Check standard iteration. */
    Int sum = 0;
    for (ListElement *x = list_front(l); x != NULL; x = list_element_next(x)) {
        const Int *i = any_assert(x->value, TYPE_INT);
        if (i != NULL)
            sum += *i;
    }
    if (sum != 4)
        testing_t_errorf_v(t, "sum over l = %d, want 4", sum);

    /* Clear all elements by iterating */
    ListElement *next;
    for (ListElement *x = list_front(l); x != NULL; x = next) {
        next = list_element_next(x);
        list_remove(l, x);
    }
    CHECK_EMPTY(l);
    arena_free(&ar);
}

static void check_list(TestingT *t, const List *l, const Int *es, Int n) {
    if (!check_list_len(t, l, n))
        return;

    Int i = 0;
    for (ListElement *e = list_front(l); e != NULL; e = list_element_next(e)) {
        const Int *le = any_assert(e->value, TYPE_INT);
        if (le == NULL) {
            testing_t_errorf_v(t, "elt[%d].Value is not an int", i);
        } else if (*le != es[i]) {
            testing_t_errorf_v(t, "elt[%d].Value = %d, want %d", i, *le, es[i]);
        }
        i++;
    }
}

#define CHECK_LIST(l, ...)                                                             \
    do {                                                                               \
        const Int want_[] = {__VA_ARGS__};                                             \
        check_list(t, (l), want_, LEN(want_));                                         \
    } while (0)

static void TestExtending(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    List *l1 = list_new(a);
    List *l2 = list_new(a);

    list_push_back(l1, I(1));
    list_push_back(l1, I(2));
    list_push_back(l1, I(3));

    list_push_back(l2, I(4));
    list_push_back(l2, I(5));

    List *l3 = list_new(a);
    CHECK(list_push_back_list(l3, l1));
    CHECK_LIST(l3, 1, 2, 3);
    CHECK(list_push_back_list(l3, l2));
    CHECK_LIST(l3, 1, 2, 3, 4, 5);

    l3 = list_new(a);
    CHECK(list_push_front_list(l3, l2));
    CHECK_LIST(l3, 4, 5);
    CHECK(list_push_front_list(l3, l1));
    CHECK_LIST(l3, 1, 2, 3, 4, 5);

    CHECK_LIST(l1, 1, 2, 3);
    CHECK_LIST(l2, 4, 5);

    l3 = list_new(a);
    CHECK(list_push_back_list(l3, l1));
    CHECK_LIST(l3, 1, 2, 3);
    CHECK(list_push_back_list(l3, l3));
    CHECK_LIST(l3, 1, 2, 3, 1, 2, 3);

    l3 = list_new(a);
    CHECK(list_push_front_list(l3, l1));
    CHECK_LIST(l3, 1, 2, 3);
    CHECK(list_push_front_list(l3, l3));
    CHECK_LIST(l3, 1, 2, 3, 1, 2, 3);

    l3 = list_new(a);
    CHECK(list_push_back_list(l1, l3));
    CHECK_LIST(l1, 1, 2, 3);
    CHECK(list_push_front_list(l1, l3));
    CHECK_LIST(l1, 1, 2, 3);
    arena_free(&ar);
}

static void TestRemove(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    List *l = list_new(arena_allocator(&ar));
    ListElement *e1 = list_push_back(l, I(1));
    ListElement *e2 = list_push_back(l, I(2));
    CHECK_POINTERS(l, e1, e2);
    ListElement *e = list_front(l);
    list_remove(l, e);
    CHECK_POINTERS(l, e2);
    list_remove(l, e);
    CHECK_POINTERS(l, e2);
    arena_free(&ar);
}

static void TestIssue4103(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    List *l1 = list_new(a);
    list_push_back(l1, I(1));
    list_push_back(l1, I(2));

    List *l2 = list_new(a);
    list_push_back(l2, I(3));
    list_push_back(l2, I(4));

    ListElement *e = list_front(l1);
    list_remove(l2, e); /* l2 should not change because e is not an element of l2 */
    if (list_len(l2) != 2)
        testing_t_errorf_v(t, "l2.Len() = %d, want 2", list_len(l2));

    list_insert_before(l1, I(8), e);
    if (list_len(l1) != 3)
        testing_t_errorf_v(t, "l1.Len() = %d, want 3", list_len(l1));
    arena_free(&ar);
}

static void TestIssue6349(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    List *l = list_new(arena_allocator(&ar));
    list_push_back(l, I(1));
    list_push_back(l, I(2));

    ListElement *e = list_front(l);
    Any v = list_remove(l, e);
    if (!any_equal(e->value, I(1)) || !any_equal(v, I(1)))
        testing_t_errorf_v(t, "e.value = %v, want 1", e->value);
    if (list_element_next(e) != NULL)
        testing_t_errorf_v(t, "e.Next() != nil");
    if (list_element_prev(e) != NULL)
        testing_t_errorf_v(t, "e.Prev() != nil");
    arena_free(&ar);
}

static void TestMove(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    List *l = list_new(arena_allocator(&ar));
    ListElement *e1 = list_push_back(l, I(1));
    ListElement *e2 = list_push_back(l, I(2));
    ListElement *e3 = list_push_back(l, I(3));
    ListElement *e4 = list_push_back(l, I(4));
    ListElement *tmp;

    list_move_after(l, e3, e3);
    CHECK_POINTERS(l, e1, e2, e3, e4);
    list_move_before(l, e2, e2);
    CHECK_POINTERS(l, e1, e2, e3, e4);

    list_move_after(l, e3, e2);
    CHECK_POINTERS(l, e1, e2, e3, e4);
    list_move_before(l, e2, e3);
    CHECK_POINTERS(l, e1, e2, e3, e4);

    list_move_before(l, e2, e4);
    CHECK_POINTERS(l, e1, e3, e2, e4);
    tmp = e2, e2 = e3, e3 = tmp;

    list_move_before(l, e4, e1);
    CHECK_POINTERS(l, e4, e1, e2, e3);
    tmp = e1, e1 = e4, e4 = e3, e3 = e2, e2 = tmp;

    list_move_after(l, e4, e1);
    CHECK_POINTERS(l, e1, e4, e2, e3);
    tmp = e2, e2 = e4, e4 = e3, e3 = tmp;

    list_move_after(l, e2, e3);
    CHECK_POINTERS(l, e1, e3, e2, e4);
    arena_free(&ar);
}

/* Test PushFront, PushBack, PushFrontList, PushBackList with uninitialized
 * List. A zeroed List allocates from the heap, so these give it back. */
static void TestZeroList(TestingT *t) {
    List l1 = {0};
    list_push_front(&l1, I(1));
    CHECK_LIST(&l1, 1);

    List l2 = {0};
    list_push_back(&l2, I(1));
    CHECK_LIST(&l2, 1);

    List l3 = {0};
    CHECK(list_push_front_list(&l3, &l1));
    CHECK_LIST(&l3, 1);

    List l4 = {0};
    CHECK(list_push_back_list(&l4, &l2));
    CHECK_LIST(&l4, 1);

    list_free(&l1);
    list_free(&l2);
    list_free(&l3);
    list_free(&l4);
}

/* Test that a list l is not modified when calling InsertBefore with a mark
 * that is not an element of l. */
static void TestInsertBeforeUnknownMark(TestingT *t) {
    List l = {0};
    list_push_back(&l, I(1));
    list_push_back(&l, I(2));
    list_push_back(&l, I(3));
    ListElement mark = {0};
    CHECK(list_insert_before(&l, I(1), &mark) == NULL);
    CHECK_LIST(&l, 1, 2, 3);
    list_free(&l);
}

/* Test that a list l is not modified when calling InsertAfter with a mark
 * that is not an element of l. */
static void TestInsertAfterUnknownMark(TestingT *t) {
    List l = {0};
    list_push_back(&l, I(1));
    list_push_back(&l, I(2));
    list_push_back(&l, I(3));
    ListElement mark = {0};
    CHECK(list_insert_after(&l, I(1), &mark) == NULL);
    CHECK_LIST(&l, 1, 2, 3);
    list_free(&l);
}

/* Test that a list l is not modified when calling MoveAfter or MoveBefore with
 * a mark that is not an element of l. */
static void TestMoveUnknownMark(TestingT *t) {
    List l1 = {0};
    ListElement *e1 = list_push_back(&l1, I(1));

    List l2 = {0};
    ListElement *e2 = list_push_back(&l2, I(2));

    list_move_after(&l1, e1, e2);
    CHECK_LIST(&l1, 1);
    CHECK_LIST(&l2, 2);

    list_move_before(&l1, e1, e2);
    CHECK_LIST(&l1, 1);
    CHECK_LIST(&l2, 2);
    list_free(&l1);
    list_free(&l2);
}

/* The rest are about what C adds: where the values live and who frees what. */

/* A pushed value is a copy, so the original can go out of scope or change. */
static void TestPushCopiesTheValue(TestingT *t) {
    List l = {0};
    Int n = 7;
    ListElement *e = list_push_back(&l, BURROW_ANY(TYPE_INT, &n));
    n = 8;
    CHECK(e->value.data != &n);
    CHECK(any_equal(e->value, I(7)));

    Str s = BURROW_S("hello");
    e = list_push_back(&l, BURROW_ANY(TYPE_STRING, &s));
    s = BURROW_S("bye");
    CHECK(any_equal(e->value, BURROW_ANY_OF("hello")));

    /* nil stays nil and a zero sized value gets no storage, like any_box. */
    e = list_push_back(&l, (Any){0});
    CHECK(BURROW_ANY_IS_NIL(e->value));
    CHECK_INT_EQ(list_len(&l), 3);
    list_free(&l);
}

/* Every element comes back to the allocator, whichever way it leaves. */
static void TestFreeGivesEverythingBack(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);

    List *l = list_new(a);
    ListElement *keep = NULL;
    for (Int i = 0; i < 10; i++) {
        ListElement *e = list_push_back(l, I(i));
        if (i == 4)
            keep = e;
    }
    list_push_front(l, BURROW_ANY_OF(1.5));
    list_push_front(l, BURROW_ANY_OF("text"));

    Any v = list_remove(l, keep);
    CHECK(any_equal(v, I(4)));
    list_free_element(l, keep);
    list_free_element(l, list_front(l));
    CHECK_INT_EQ(list_len(l), 10);

    list_free(l);
    CHECK_INT_EQ(list_len(l), 0);
    CHECK(list_front(l) == NULL);
    list_push_back(l, I(1));
    CHECK_LIST(l, 1);
    list_free(l);
    mem_free(a, l, sizeof *l, _Alignof(List));

    CHECK_INT_EQ(track_live(&tr), 0);
    CHECK_INT_EQ(track_faults(&tr), 0);
    track_free(&tr);
}

/* Out of memory is a NULL element and an unchanged list. */
static void TestOutOfMemory(TestingT *t) {
    unsigned char buf[512];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    List l = LIST(a);
    Int pushed = 0;
    while (list_push_back(&l, I(pushed)) != NULL)
        pushed++;
    CHECK(pushed > 0);
    CHECK_INT_EQ(list_len(&l), pushed);
    CHECK(list_push_front(&l, I(0)) == NULL);
    CHECK(list_insert_after(&l, I(0), list_front(&l)) == NULL);
    CHECK(!list_push_back_list(&l, &l));
    CHECK_INT_EQ(list_len(&l), pushed);

    CHECK(list_new(a) == NULL);
}

#define TESTS(X)                                                                       \
    X(TestList)                                                                        \
    X(TestExtending)                                                                   \
    X(TestRemove)                                                                      \
    X(TestIssue4103)                                                                   \
    X(TestIssue6349)                                                                   \
    X(TestMove)                                                                        \
    X(TestZeroList)                                                                    \
    X(TestInsertBeforeUnknownMark)                                                     \
    X(TestInsertAfterUnknownMark)                                                      \
    X(TestMoveUnknownMark)                                                             \
    X(TestPushCopiesTheValue)                                                          \
    X(TestFreeGivesEverythingBack)                                                     \
    X(TestOutOfMemory)

TESTING_MAIN(TESTS)
