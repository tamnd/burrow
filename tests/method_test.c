/* Methods on a declared type, and calling one whose signature you do not know.
 *
 * Two halves. The first is that the signature list produced the right data:
 * the right number of methods, the right parameter and result types, the
 * positions in the right places. The second is that the thunks actually work,
 * which is checked by calling every method through method_call with nothing but
 * a name and a descriptor in hand, the way net/rpc would.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/type.h"

#include "check.h"

#include <stdint.h>

/* ------------------------------------------------------------ a type with
 * four methods, covering every shape a signature comes in: no arguments and no
 * result, arguments and no result, no arguments and a result, and both. */

#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")

BURROW_STRUCT_DECL(Point, POINT_FIELDS);

static void point_reset(Point *p) {
    p->X = 0;
    p->Y = 0;
}

static void point_move(Point *p, Int dx, Int dy) {
    p->X += dx;
    p->Y += dy;
}

static Int point_sum(Point *p) {
    return p->X + p->Y;
}

static Str point_label(Point *p, Str fallback) {
    return p->X == 0 && p->Y == 0 ? fallback : BURROW_S("somewhere");
}

#define POINT_SIG_Label(IN, OUT)                                                       \
    IN(0, Str)                                                                         \
    OUT(Str)
#define POINT_SIG_Move(IN, OUT)                                                        \
    IN(0, Int)                                                                         \
    IN(1, Int)
#define POINT_SIG_Reset(IN, OUT)
#define POINT_SIG_Sum(IN, OUT) OUT(Int)

/* In name order, which is the order Go enumerates methods in. */
#define POINT_METHODS(M, T)                                                            \
    M(T, Label, point_label, POINT_SIG_Label)                                          \
    M(T, Move, point_move, POINT_SIG_Move)                                             \
    M(T, Reset, point_reset, POINT_SIG_Reset)                                          \
    M(T, Sum, point_sum, POINT_SIG_Sum)

BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);

/* A second type with methods, in the same translation unit, because the names
 * the macro invents are built from the type's name and a collision between two
 * types would only show up when there were two. */

#define COUNTER_FIELDS(F, T) F(T, Int, N, "")

BURROW_STRUCT_DECL(Counter, COUNTER_FIELDS);

static void counter_add(Counter *c, Int n) {
    c->N += n;
}

static Int counter_get(Counter *c) {
    return c->N;
}

#define COUNTER_SIG_Add(IN, OUT) IN(0, Int)
#define COUNTER_SIG_Get(IN, OUT) OUT(Int)

#define COUNTER_METHODS(M, T)                                                          \
    M(T, Add, counter_add, COUNTER_SIG_Add)                                            \
    M(T, Get, counter_get, COUNTER_SIG_Get)

BURROW_STRUCT_DEFINE_METHODS(Counter, COUNTER_FIELDS, COUNTER_METHODS);

/* A type with no methods at all, which is the common case and has to answer
 * every one of these questions without tripping over a NULL array. */

#define PLAIN_FIELDS(F, T) F(T, Int, V, "")

BURROW_STRUCT(Plain, PLAIN_FIELDS);

/* A method array written by hand, out of order and with a hole in it, for the
 * two things the macro cannot produce: an unsorted list and a method with no
 * thunk behind it. */

static const Method jumbled_methods[] = {
    {BURROW_S_INIT("Zulu"), NULL, NULL},
    {BURROW_S_INIT("Alpha"), NULL, NULL},
    {BURROW_S_INIT("Mike"), NULL, NULL},
};

static const Type jumbled_type = {
    BURROW_S_INIT("Jumbled"),
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    3,
    NULL,
    jumbled_methods,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------------- the data
 */

static void TestATypeKnowsHowManyMethodsItHas(TestingT *t) {
    CHECK_INT_EQ(TYPE_OF(Point)->nmethod, 4);
    CHECK_INT_EQ(TYPE_OF(Counter)->nmethod, 2);
    CHECK_INT_EQ(TYPE_OF(Plain)->nmethod, 0);
    CHECK(TYPE_OF(Plain)->methods == NULL);
}

static void TestTheMethodsAreInTheOrderTheyWereListed(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    CHECK(str_eq(ty->methods[0].name, BURROW_S("Label")));
    CHECK(str_eq(ty->methods[1].name, BURROW_S("Move")));
    CHECK(str_eq(ty->methods[2].name, BURROW_S("Reset")));
    CHECK(str_eq(ty->methods[3].name, BURROW_S("Sum")));
}

static void TestAListWrittenInNameOrderSaysSo(TestingT *t) {
    CHECK(type_methods_sorted(TYPE_OF(Point)));
    CHECK(type_methods_sorted(TYPE_OF(Counter)));

    /* Nothing to be out of order. */
    CHECK(type_methods_sorted(TYPE_OF(Plain)));
    CHECK(type_methods_sorted(NULL));

    CHECK(!type_methods_sorted(&jumbled_type));
}

static void TestAMethodIsFoundByName(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    CHECK(type_method_by_name(ty, BURROW_S("Label")) == &ty->methods[0]);
    CHECK(type_method_by_name(ty, BURROW_S("Move")) == &ty->methods[1]);
    CHECK(type_method_by_name(ty, BURROW_S("Reset")) == &ty->methods[2]);
    CHECK(type_method_by_name(ty, BURROW_S("Sum")) == &ty->methods[3]);
}

static void TestAMethodThatIsNotThereIsNotFound(TestingT *t) {
    CHECK(type_method_by_name(TYPE_OF(Point), BURROW_S("Nope")) == NULL);
    CHECK(type_method_by_name(TYPE_OF(Plain), BURROW_S("Sum")) == NULL);
    CHECK(type_method_by_name(NULL, BURROW_S("Sum")) == NULL);

    /* A prefix of a real name is not that name. */
    CHECK(type_method_by_name(TYPE_OF(Point), BURROW_S("Su")) == NULL);
    CHECK(type_method_by_name(TYPE_OF(Point), BURROW_S("Sums")) == NULL);
    CHECK(type_method_by_name(TYPE_OF(Point), BURROW_S("")) == NULL);
}

/* The reason the lookup stopped being a binary search. Every one of these is
 * found; a binary search over this array finds Mike and misses both others. */
static void TestAnUnsortedListIsStillSearchedCorrectly(TestingT *t) {
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Zulu")) == &jumbled_methods[0]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Alpha")) == &jumbled_methods[1]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Mike")) == &jumbled_methods[2]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Nope")) == NULL);
}

/* ------------------------------------------------------------- the signature
 */

static void TestASignatureCountsItsParametersAndItsResult(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    CHECK_INT_EQ(type_num_in(type_method_by_name(ty, BURROW_S("Reset"))->ftype), 0);
    CHECK_INT_EQ(type_num_out(type_method_by_name(ty, BURROW_S("Reset"))->ftype), 0);

    CHECK_INT_EQ(type_num_in(type_method_by_name(ty, BURROW_S("Move"))->ftype), 2);
    CHECK_INT_EQ(type_num_out(type_method_by_name(ty, BURROW_S("Move"))->ftype), 0);

    CHECK_INT_EQ(type_num_in(type_method_by_name(ty, BURROW_S("Sum"))->ftype), 0);
    CHECK_INT_EQ(type_num_out(type_method_by_name(ty, BURROW_S("Sum"))->ftype), 1);

    CHECK_INT_EQ(type_num_in(type_method_by_name(ty, BURROW_S("Label"))->ftype), 1);
    CHECK_INT_EQ(type_num_out(type_method_by_name(ty, BURROW_S("Label"))->ftype), 1);
}

static void TestASignatureNamesTheTypes(TestingT *t) {
    const Type *ty = TYPE_OF(Point);
    const Type *move = type_method_by_name(ty, BURROW_S("Move"))->ftype;
    const Type *label = type_method_by_name(ty, BURROW_S("Label"))->ftype;

    CHECK(type_in(move, 0) == TYPE_OF(Int));
    CHECK(type_in(move, 1) == TYPE_OF(Int));

    CHECK(type_in(label, 0) == TYPE_OF(Str));
    CHECK(type_out(label, 0) == TYPE_OF(Str));

    CHECK(type_out(type_method_by_name(ty, BURROW_S("Sum"))->ftype, 0) == TYPE_OF(Int));
}

static void TestASignatureIsAFunctionType(TestingT *t) {
    const Type *sum = type_method_by_name(TYPE_OF(Point), BURROW_S("Sum"))->ftype;

    CHECK(sum->kind == KIND_FUNC);
    CHECK_INT_EQ((Int)sum->size, (Int)sizeof(Func));
    CHECK_INT_EQ((Int)sum->align, (Int) _Alignof(Func));

    /* Unnamed, the same as []int is. A signature is not a declared type. */
    CHECK_INT_EQ(sum->name.len, 0);
    CHECK_INT_EQ(sum->pkg_path.len, 0);
}

/* The one thing in the list that is written twice, so it is the one thing worth
 * checking against itself. */
static void TestTheParameterPositionsAreTheOnesThatWereWritten(TestingT *t) {
    const Type *move = type_method_by_name(TYPE_OF(Point), BURROW_S("Move"))->ftype;

    for (Int i = 0; i < type_num_in(move); i++)
        CHECK_INT_EQ((Int)move->fields[i].offset, i);
}

static void TestAskingATypeThatIsNotAFunctionGivesNothing(TestingT *t) {
    CHECK_INT_EQ(type_num_in(TYPE_OF(Point)), 0);
    CHECK_INT_EQ(type_num_out(TYPE_OF(Point)), 0);
    CHECK(type_in(TYPE_OF(Point), 0) == NULL);
    CHECK(type_out(TYPE_OF(Point), 0) == NULL);

    CHECK_INT_EQ(type_num_in(NULL), 0);
    CHECK_INT_EQ(type_num_out(NULL), 0);
    CHECK(type_in(NULL, 0) == NULL);
    CHECK(type_out(NULL, 0) == NULL);
}

static void TestAPositionOffEitherEndGivesNothing(TestingT *t) {
    const Type *move = type_method_by_name(TYPE_OF(Point), BURROW_S("Move"))->ftype;

    CHECK(type_in(move, -1) == NULL);
    CHECK(type_in(move, 2) == NULL);
    CHECK(type_out(move, 0) == NULL);
}

/* --------------------------------------------------------------- the calling
 */

static void TestAMethodWithNothingEitherSideIsCalled(TestingT *t) {
    Point p = {3, 4};

    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Reset")), &p, NULL,
                      NULL));
    CHECK_INT_EQ(p.X, 0);
    CHECK_INT_EQ(p.Y, 0);
}

static void TestAMethodWithArgumentsGetsThemInOrder(TestingT *t) {
    Point p = {0, 0};
    Int dx = 3;
    Int dy = 40;
    void *args[] = {&dx, &dy};

    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Move")), &p, args,
                      NULL));
    CHECK_INT_EQ(p.X, 3);
    CHECK_INT_EQ(p.Y, 40);

    /* Again, to show the arguments are read rather than remembered. */
    dx = 100;
    dy = 200;
    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Move")), &p, args,
                      NULL));
    CHECK_INT_EQ(p.X, 103);
    CHECK_INT_EQ(p.Y, 240);
}

static void TestAMethodWithAResultWritesItWhereItWasTold(TestingT *t) {
    Point p = {7, 5};
    Int got = -1;
    void *rets[] = {&got};

    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Sum")), &p, NULL,
                      rets));
    CHECK_INT_EQ(got, 12);
}

static void TestAMethodWithBothIsCalled(TestingT *t) {
    Point origin = {0, 0};
    Point elsewhere = {1, 1};
    Str fallback = BURROW_S("origin");
    Str got = BURROW_S("");
    void *args[] = {&fallback};
    void *rets[] = {&got};
    const Method *m = type_method_by_name(TYPE_OF(Point), BURROW_S("Label"));

    CHECK(method_call(m, &origin, args, rets));
    CHECK(str_eq(got, BURROW_S("origin")));

    CHECK(method_call(m, &elsewhere, args, rets));
    CHECK(str_eq(got, BURROW_S("somewhere")));
}

static void TestTheSecondTypesMethodsAreItsOwn(TestingT *t) {
    Counter c = {0};
    Int n = 5;
    Int got = -1;
    void *args[] = {&n};
    void *rets[] = {&got};

    CHECK(method_call(type_method_by_name(TYPE_OF(Counter), BURROW_S("Add")), &c, args,
                      NULL));
    CHECK(method_call(type_method_by_name(TYPE_OF(Counter), BURROW_S("Add")), &c, args,
                      NULL));
    CHECK(method_call(type_method_by_name(TYPE_OF(Counter), BURROW_S("Get")), &c, NULL,
                      rets));
    CHECK_INT_EQ(got, 10);
    CHECK_INT_EQ(c.N, 10);
}

static void TestCallingNothingSaysSoRatherThanCrashing(TestingT *t) {
    Point p = {1, 2};

    CHECK(!method_call(NULL, &p, NULL, NULL));
    CHECK(!method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Nope")), &p, NULL,
                       NULL));

    /* A Method with no thunk, which is what a hand written array can hold. */
    CHECK(!method_call(&jumbled_methods[0], &p, NULL, NULL));
}

/* The whole point of the exercise, with nothing in hand but two names. This is
 * the shape of what net/rpc does with a call that arrived over a socket. */
static void TestANameAndAMethodNameAreEnoughToMakeTheCall(TestingT *t) {
    Point p = {8, 9};
    Int got = -1;
    void *rets[] = {&got};

    const Type *ty = TYPE_OF(Point);
    const Method *m = type_method_by_name(ty, BURROW_S("Sum"));

    CHECK(m != NULL);
    CHECK_INT_EQ(type_num_in(m->ftype), 0);
    CHECK_INT_EQ(type_num_out(m->ftype), 1);
    CHECK(type_out(m->ftype, 0) == TYPE_OF(Int));
    CHECK(method_call(m, &p, NULL, rets));
    CHECK_INT_EQ(got, 17);
}

#define TESTS(X)                                                                       \
    X(TestATypeKnowsHowManyMethodsItHas)                                               \
    X(TestTheMethodsAreInTheOrderTheyWereListed)                                       \
    X(TestAListWrittenInNameOrderSaysSo)                                               \
    X(TestAMethodIsFoundByName)                                                        \
    X(TestAMethodThatIsNotThereIsNotFound)                                             \
    X(TestAnUnsortedListIsStillSearchedCorrectly)                                      \
    X(TestASignatureCountsItsParametersAndItsResult)                                   \
    X(TestASignatureNamesTheTypes)                                                     \
    X(TestASignatureIsAFunctionType)                                                   \
    X(TestTheParameterPositionsAreTheOnesThatWereWritten)                              \
    X(TestAskingATypeThatIsNotAFunctionGivesNothing)                                   \
    X(TestAPositionOffEitherEndGivesNothing)                                           \
    X(TestAMethodWithNothingEitherSideIsCalled)                                        \
    X(TestAMethodWithArgumentsGetsThemInOrder)                                         \
    X(TestAMethodWithAResultWritesItWhereItWasTold)                                    \
    X(TestAMethodWithBothIsCalled)                                                     \
    X(TestTheSecondTypesMethodsAreItsOwn)                                              \
    X(TestCallingNothingSaysSoRatherThanCrashing)                                      \
    X(TestANameAndAMethodNameAreEnoughToMakeTheCall)

TESTING_MAIN(TESTS)
