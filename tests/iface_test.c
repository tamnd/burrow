/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "check.h"
#include "fatal.h"

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

/* ------------------------------------------------------- two toy interfaces
 *
 * Written the way a ported package writes one, because that is what is being
 * tested. A Stringer and a Counter, and two types that implement them. */

typedef struct StringerVT {
    const Type *self_type;
    Str (*string)(void *self);
} StringerVT;

typedef struct Stringer {
    const StringerVT *vt;
    void *data;
} Stringer;

typedef struct CounterVT {
    const Type *self_type;
    Int (*add)(void *self, Int n);
} CounterVT;

typedef struct Counter {
    const CounterVT *vt;
    void *data;
} Counter;

/* A type that is both, with its vtables side by side, which is the embedding
 * shape: a Both value converts to either one by the address of a member. */
typedef struct BothVT {
    StringerVT stringer;
    CounterVT counter;
} BothVT;

typedef struct Both {
    const BothVT *vt;
    void *data;
} Both;

typedef struct Tally {
    Int n;
    Str name;
} Tally;

static const Type tally_type = {
    {(const Byte *)"Tally", 5},
    {(const Byte *)"ifacetest", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(Tally),
    (uint16_t)_Alignof(Tally),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74616c79U,
    NULL,
};

static Str tally_string(void *self) {
    return ((Tally *)self)->name;
}

static Int tally_add(void *self, Int n) {
    Tally *t = (Tally *)self;
    t->n += n;
    return t->n;
}

static const BothVT tally_both_vt = {
    {&tally_type, tally_string},
    {&tally_type, tally_add},
};

static Both tally_as_both(Tally *t) {
    Both b = {&tally_both_vt, t};
    return b;
}

static Stringer both_as_stringer(Both b) {
    Stringer s = {NULL, NULL};
    if (b.vt == NULL)
        return s;
    s.vt = &b.vt->stringer;
    s.data = b.data;
    return s;
}

static Counter both_as_counter(Both b) {
    Counter c = {NULL, NULL};
    if (b.vt == NULL)
        return c;
    c.vt = &b.vt->counter;
    c.data = b.data;
    return c;
}

/* A second implementation of Stringer, so that the dispatch has something to
 * choose between and the type assertion has something to say no to. */
typedef struct Label {
    Str text;
} Label;

static const Type label_type = {
    {(const Byte *)"Label", 5},
    {(const Byte *)"ifacetest", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(Label),
    (uint16_t)_Alignof(Label),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6c61626cU,
    NULL,
};

static Str label_string(void *self) {
    return ((Label *)self)->text;
}

static const StringerVT label_stringer_vt = {&label_type, label_string};

static Stringer label_as_stringer(Label *l) {
    Stringer s = {&label_stringer_vt, l};
    return s;
}

/* A type that declines to be asserted to, which is what a NULL self_type means
 * and what an unexported type gets you in Go. */
static const StringerVT anonymous_stringer_vt = {NULL, label_string};

/* []int, for the one thing that cannot be compared. elem is NULL because a
 * static initialiser cannot name TYPE_INT and because nothing here looks past
 * the kind to decide that a slice is uncomparable. */
static const Type slice_of_int = {
    {(const Byte *)"[]int", 5},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736c6369U,
    NULL,
};

/* ------------------------------------------------------------ the interface */

static void TestAZeroedInterfaceValueIsNil(TestingT *t) {
    Stringer s = {NULL, NULL};
    Stringer z;
    memset(&z, 0, sizeof z);

    CHECK(BURROW_IFACE_IS_NIL(s));
    CHECK(BURROW_IFACE_IS_NIL(z));
    CHECK(iface_type(BURROW_IFACE(s)) == NULL);
}

static void TestAStructFieldOfInterfaceTypeStartsOutNil(TestingT *t) {
    /* The reason the vtable pointer is the first member. A struct that is
     * zeroed, which is every struct out of an allocator, has nil interfaces in
     * it without anybody writing a line to say so. */
    struct Holder {
        Int before;
        Stringer s;
        Int after;
    };
    struct Holder *h = BURROW_NEW(a, struct Holder);

    CHECK(h != NULL);
    if (h != NULL)
        CHECK(BURROW_IFACE_IS_NIL(h->s));
}

static void TestACallGoesToTheImplementationBehindTheValue(TestingT *t) {
    Label l = {{(const Byte *)"label", 5}};
    Tally tally = {0, {(const Byte *)"tally", 5}};
    Stringer from_label = label_as_stringer(&l);
    Stringer from_tally = both_as_stringer(tally_as_both(&tally));

    CHECK(str_eq(BURROW_CALL0(from_label, string), str_from_cstr("label")));
    CHECK(str_eq(BURROW_CALL0(from_tally, string), str_from_cstr("tally")));
}

static void TestACallWithArgumentsPassesTheReceiverFirst(TestingT *t) {
    Tally tally = {0, {(const Byte *)"tally", 5}};
    Counter c = both_as_counter(tally_as_both(&tally));

    CHECK_INT_EQ(BURROW_CALL(c, add, 3), 3);
    CHECK_INT_EQ(BURROW_CALL(c, add, 4), 7);
    CHECK_INT_EQ(tally.n, 7);
}

static void TestAnEmbeddedInterfaceIsReachedByTheAddressOfAMember(TestingT *t) {
    Tally tally = {0, {(const Byte *)"tally", 5}};
    Both b = tally_as_both(&tally);
    Stringer s = both_as_stringer(b);
    Counter c = both_as_counter(b);

    /* Both narrow values point at the same object and at their own part of the
     * one vtable. The second of the two is the case a pointer cast would get
     * wrong, which is the whole argument for doing it this way. */
    CHECK(s.data == &tally);
    CHECK(c.data == &tally);
    CHECK((const void *)s.vt == (const void *)&tally_both_vt.stringer);
    CHECK((const void *)c.vt == (const void *)&tally_both_vt.counter);
    CHECK((const void *)c.vt != (const void *)&tally_both_vt);
}

static void TestNarrowingANilValueGivesANilValue(TestingT *t) {
    Both b = {NULL, NULL};

    CHECK(BURROW_IFACE_IS_NIL(both_as_stringer(b)));
    CHECK(BURROW_IFACE_IS_NIL(both_as_counter(b)));
}

static void TestTheDynamicTypeSurvivesTheConversionToAnInterface(TestingT *t) {
    Label l = {{(const Byte *)"label", 5}};
    Tally tally = {0, {(const Byte *)"tally", 5}};

    CHECK(iface_type(BURROW_IFACE(label_as_stringer(&l))) == &label_type);
    CHECK(iface_type(BURROW_IFACE(both_as_counter(tally_as_both(&tally)))) ==
          &tally_type);
}

static void TestAnAssertionToTheRightTypeGivesTheValueBack(TestingT *t) {
    Label l = {{(const Byte *)"label", 5}};
    Stringer s = label_as_stringer(&l);
    void *got = iface_assert(BURROW_IFACE(s), &label_type);

    CHECK(got == &l);
}

static void TestAnAssertionToTheWrongTypeGivesNull(TestingT *t) {
    Label l = {{(const Byte *)"label", 5}};
    Stringer s = label_as_stringer(&l);

    CHECK(iface_assert(BURROW_IFACE(s), &tally_type) == NULL);
    CHECK(iface_assert(BURROW_IFACE(s), TYPE_INT) == NULL);
    CHECK(iface_assert(BURROW_IFACE(s), NULL) == NULL);
}

static void TestAnAssertionOnANilValueGivesNullRatherThanStopping(TestingT *t) {
    Stringer s = {NULL, NULL};

    CHECK(iface_assert(BURROW_IFACE(s), &label_type) == NULL);
}

static void TestAVtableThatDeclinesToSayItsTypeIsNeverAssertedTo(TestingT *t) {
    Label l = {{(const Byte *)"anonymous", 9}};
    Stringer s = {&anonymous_stringer_vt, &l};

    CHECK(iface_type(BURROW_IFACE(s)) == NULL);
    CHECK(iface_assert(BURROW_IFACE(s), &label_type) == NULL);
    /* It still works as an interface, which is the point of allowing it. */
    CHECK(str_eq(BURROW_CALL0(s, string), str_from_cstr("anonymous")));
}

static void TestTwoTypesWithTheSameNameAreStillTwoTypes(TestingT *t) {
    /* Pointer identity on the descriptor and not a comparison of names, which
     * is what Go's PkgPath exists to make sure of. */
    static const Type other_label_type = {
        {(const Byte *)"Label", 5},
        {(const Byte *)"othertest", 9},
        KIND_STRUCT,
        (uint32_t)sizeof(Label),
        (uint16_t)_Alignof(Label),
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0,
        0x6f746872U,
        NULL,
    };
    Label l = {{(const Byte *)"label", 5}};
    Stringer s = label_as_stringer(&l);

    CHECK(iface_assert(BURROW_IFACE(s), &other_label_type) == NULL);
}

/* -------------------------------------------------------------------- any */

static void TestAZeroedAnyIsNil(TestingT *t) {
    Any v = {NULL, NULL};

    CHECK(BURROW_ANY_IS_NIL(v));
    CHECK(any_assert(v, TYPE_INT) == NULL);
}

static void TestAnAnyFromAPointerPointsAtWhatItWasGiven(TestingT *t) {
    Int n = 42;
    Any v = BURROW_ANY(TYPE_INT, &n);
    Int *got = (Int *)any_assert(v, TYPE_INT);

    CHECK(v.t == TYPE_INT);
    CHECK(got == &n);
    if (got != NULL)
        CHECK_INT_EQ(*got, 42);
}

static void TestAnAnyFromAValueCarriesACopyOfIt(TestingT *t) {
    Any v = BURROW_ANY_VAL(TYPE_INT, Int, 7);
    Int *got = (Int *)any_assert(v, TYPE_INT);

    CHECK(got != NULL);
    if (got != NULL)
        CHECK_INT_EQ(*got, 7);
}

static void TestAnAssertionOnAnAnyAnswersForTheDynamicTypeOnly(TestingT *t) {
    Int n = 1;
    Any v = BURROW_ANY(TYPE_INT, &n);

    CHECK(any_assert(v, TYPE_INT) == &n);
    CHECK(any_assert(v, TYPE_INT64) == NULL);
    CHECK(any_assert(v, TYPE_UINT) == NULL);
    CHECK(any_assert(v, NULL) == NULL);
}

static void TestBoxingCopiesTheValueOutOfTheCallersFrame(TestingT *t) {
    Any boxed;
    {
        Int n = 99;
        Any v = BURROW_ANY(TYPE_INT, &n);
        boxed = any_box(a, v);
        n = 0; /* the original is gone as far as boxed is concerned */
    }

    CHECK(!BURROW_ANY_IS_NIL(boxed));
    CHECK(boxed.t == TYPE_INT);
    CHECK(boxed.data != NULL);
    if (boxed.data != NULL)
        CHECK_INT_EQ(*(Int *)boxed.data, 99);
}

static void TestBoxingGoesThroughTheDescriptorSoAStrStaysEqual(TestingT *t) {
    Str s = str_from_cstr("hello");
    Any v = BURROW_ANY(TYPE_STRING, &s);
    Any boxed = any_box(a, v);

    CHECK(boxed.data != NULL);
    CHECK(boxed.data != &s);
    if (boxed.data != NULL)
        CHECK(str_eq(*(Str *)boxed.data, str_from_cstr("hello")));
    CHECK(any_equal(v, boxed));
}

static void TestBoxingNothingGivesNothing(TestingT *t) {
    Any nil = {NULL, NULL};
    Any no_data = {TYPE_INT, NULL};

    CHECK(BURROW_ANY_IS_NIL(any_box(a, nil)));
    CHECK(BURROW_ANY_IS_NIL(any_box(a, no_data)));
}

static void TestBoxingReportsAnAllocatorThatSaysNo(TestingT *t) {
    /* A fixed allocator with nothing in it, which is the honest way to ask what
     * happens when the memory is not there. */
    Fixed fx;
    Alloc *small;
    Int n = 5;
    Any v = BURROW_ANY(TYPE_INT, &n);
    static Byte buf[8];

    fixed_init(&fx, buf, sizeof buf);
    small = fixed_allocator(&fx);
    /* Take everything, so the box below has nowhere to go. */
    (void)mem_alloc(small, sizeof buf, 1);

    CHECK(BURROW_ANY_IS_NIL(any_box(small, v)));
}

static void TestTwoAnysHoldingTheSameValueAreEqual(TestingT *t) {
    Int x = 3, y = 3;
    Any a1 = BURROW_ANY(TYPE_INT, &x);
    Any a2 = BURROW_ANY(TYPE_INT, &y);

    CHECK(any_equal(a1, a2));
    CHECK(any_equal(a1, a1));
}

static void TestTwoAnysOfDifferentTypesAreNeverEqual(TestingT *t) {
    Int i = 3;
    int64_t j = 3;
    Any a1 = BURROW_ANY(TYPE_INT, &i);
    Any a2 = BURROW_ANY(TYPE_INT64, &j);

    /* The same bytes and the same number, and Go says they are different, which
     * is the rule that stops a map[any]int treating int(3) and int64(3) as one
     * key. */
    CHECK(!any_equal(a1, a2));
}

static void TestNilAnysAreEqualToEachOtherAndToNothingElse(TestingT *t) {
    Int n = 0;
    Any nil1 = {NULL, NULL};
    Any nil2 = {NULL, NULL};
    Any some = BURROW_ANY(TYPE_INT, &n);

    CHECK(any_equal(nil1, nil2));
    CHECK(!any_equal(nil1, some));
    CHECK(!any_equal(some, nil2));
}

static void TestComparingUncomparableValuesStopsTheProgramTheWayGoDoes(TestingT *t) {
    Slice s1 = slice_make(a, TYPE_INT, 2, 2);
    Slice s2 = slice_make(a, TYPE_INT, 2, 2);
    Any a1 = BURROW_ANY(&slice_of_int, &s1);
    Any a2 = BURROW_ANY(&slice_of_int, &s2);

    CHECK_RUNTIME_ERROR(any_equal(a1, a2),
                        "runtime error: comparing uncomparable type []int");
}

static void TestTheAnyDescriptorDescribesAnInterface(TestingT *t) {
    CHECK(TYPE_ANY != NULL);
    CHECK_INT_EQ(TYPE_ANY->kind, KIND_INTERFACE);
    CHECK_INT_EQ(TYPE_ANY->size, (uint32_t)sizeof(Any));
    CHECK(str_eq(type_name(TYPE_ANY), str_from_cstr("interface {}")));
    CHECK(type_is_comparable(TYPE_ANY));
}

static void TestTheAnyDescriptorComparesWithTheSameRulesAnyEqualDoes(TestingT *t) {
    Int x = 3;
    int64_t y = 3;
    Any a1 = BURROW_ANY(TYPE_INT, &x);
    Any a2 = BURROW_ANY(TYPE_INT, &x);
    Any a3 = BURROW_ANY(TYPE_INT64, &y);

    CHECK(type_equal(TYPE_ANY, &a1, &a2));
    CHECK(!type_equal(TYPE_ANY, &a1, &a3));
}

static void TestTheAnyDescriptorHashesTheTypeAlongWithTheValue(TestingT *t) {
    Int x = 3;
    int64_t y = 3;
    Any as_int = BURROW_ANY(TYPE_INT, &x);
    Any as_int64 = BURROW_ANY(TYPE_INT64, &y);
    Any nil = {NULL, NULL};

    CHECK(type_hash(TYPE_ANY, &as_int, 1) == type_hash(TYPE_ANY, &as_int, 1));
    CHECK(type_hash(TYPE_ANY, &as_int, 1) != type_hash(TYPE_ANY, &as_int64, 1));
    CHECK(type_hash(TYPE_ANY, &nil, 1) == 1);
}

#define TESTS(X)                                                                       \
    X(TestAZeroedInterfaceValueIsNil)                                                  \
    X(TestAStructFieldOfInterfaceTypeStartsOutNil)                                     \
    X(TestACallGoesToTheImplementationBehindTheValue)                                  \
    X(TestACallWithArgumentsPassesTheReceiverFirst)                                    \
    X(TestAnEmbeddedInterfaceIsReachedByTheAddressOfAMember)                           \
    X(TestNarrowingANilValueGivesANilValue)                                            \
    X(TestTheDynamicTypeSurvivesTheConversionToAnInterface)                            \
    X(TestAnAssertionToTheRightTypeGivesTheValueBack)                                  \
    X(TestAnAssertionToTheWrongTypeGivesNull)                                          \
    X(TestAnAssertionOnANilValueGivesNullRatherThanStopping)                           \
    X(TestAVtableThatDeclinesToSayItsTypeIsNeverAssertedTo)                            \
    X(TestTwoTypesWithTheSameNameAreStillTwoTypes)                                     \
    X(TestAZeroedAnyIsNil)                                                             \
    X(TestAnAnyFromAPointerPointsAtWhatItWasGiven)                                     \
    X(TestAnAnyFromAValueCarriesACopyOfIt)                                             \
    X(TestAnAssertionOnAnAnyAnswersForTheDynamicTypeOnly)                              \
    X(TestBoxingCopiesTheValueOutOfTheCallersFrame)                                    \
    X(TestBoxingGoesThroughTheDescriptorSoAStrStaysEqual)                              \
    X(TestBoxingNothingGivesNothing)                                                   \
    X(TestBoxingReportsAnAllocatorThatSaysNo)                                          \
    X(TestTwoAnysHoldingTheSameValueAreEqual)                                          \
    X(TestTwoAnysOfDifferentTypesAreNeverEqual)                                        \
    X(TestNilAnysAreEqualToEachOtherAndToNothingElse)                                  \
    X(TestComparingUncomparableValuesStopsTheProgramTheWayGoDoes)                      \
    X(TestTheAnyDescriptorDescribesAnInterface)                                        \
    X(TestTheAnyDescriptorComparesWithTheSameRulesAnyEqualDoes)                        \
    X(TestTheAnyDescriptorHashesTheTypeAlongWithTheValue)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
