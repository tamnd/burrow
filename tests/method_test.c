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

#include "harness.h"

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

TEST(a_type_knows_how_many_methods_it_has) {
    CHECK_INT_EQ(TYPE_OF(Point)->nmethod, 4);
    CHECK_INT_EQ(TYPE_OF(Counter)->nmethod, 2);
    CHECK_INT_EQ(TYPE_OF(Plain)->nmethod, 0);
    CHECK(TYPE_OF(Plain)->methods == NULL);
}

TEST(the_methods_are_in_the_order_they_were_listed) {
    const Type *t = TYPE_OF(Point);

    CHECK(str_eq(t->methods[0].name, BURROW_S("Label")));
    CHECK(str_eq(t->methods[1].name, BURROW_S("Move")));
    CHECK(str_eq(t->methods[2].name, BURROW_S("Reset")));
    CHECK(str_eq(t->methods[3].name, BURROW_S("Sum")));
}

TEST(a_list_written_in_name_order_says_so) {
    CHECK(type_methods_sorted(TYPE_OF(Point)));
    CHECK(type_methods_sorted(TYPE_OF(Counter)));

    /* Nothing to be out of order. */
    CHECK(type_methods_sorted(TYPE_OF(Plain)));
    CHECK(type_methods_sorted(NULL));

    CHECK(!type_methods_sorted(&jumbled_type));
}

TEST(a_method_is_found_by_name) {
    const Type *t = TYPE_OF(Point);

    CHECK(type_method_by_name(t, BURROW_S("Label")) == &t->methods[0]);
    CHECK(type_method_by_name(t, BURROW_S("Move")) == &t->methods[1]);
    CHECK(type_method_by_name(t, BURROW_S("Reset")) == &t->methods[2]);
    CHECK(type_method_by_name(t, BURROW_S("Sum")) == &t->methods[3]);
}

TEST(a_method_that_is_not_there_is_not_found) {
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
TEST(an_unsorted_list_is_still_searched_correctly) {
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Zulu")) == &jumbled_methods[0]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Alpha")) == &jumbled_methods[1]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Mike")) == &jumbled_methods[2]);
    CHECK(type_method_by_name(&jumbled_type, BURROW_S("Nope")) == NULL);
}

/* ------------------------------------------------------------- the signature
 */

TEST(a_signature_counts_its_parameters_and_its_result) {
    const Type *t = TYPE_OF(Point);

    CHECK_INT_EQ(type_num_in(type_method_by_name(t, BURROW_S("Reset"))->ftype), 0);
    CHECK_INT_EQ(type_num_out(type_method_by_name(t, BURROW_S("Reset"))->ftype), 0);

    CHECK_INT_EQ(type_num_in(type_method_by_name(t, BURROW_S("Move"))->ftype), 2);
    CHECK_INT_EQ(type_num_out(type_method_by_name(t, BURROW_S("Move"))->ftype), 0);

    CHECK_INT_EQ(type_num_in(type_method_by_name(t, BURROW_S("Sum"))->ftype), 0);
    CHECK_INT_EQ(type_num_out(type_method_by_name(t, BURROW_S("Sum"))->ftype), 1);

    CHECK_INT_EQ(type_num_in(type_method_by_name(t, BURROW_S("Label"))->ftype), 1);
    CHECK_INT_EQ(type_num_out(type_method_by_name(t, BURROW_S("Label"))->ftype), 1);
}

TEST(a_signature_names_the_types) {
    const Type *t = TYPE_OF(Point);
    const Type *move = type_method_by_name(t, BURROW_S("Move"))->ftype;
    const Type *label = type_method_by_name(t, BURROW_S("Label"))->ftype;

    CHECK(type_in(move, 0) == TYPE_OF(Int));
    CHECK(type_in(move, 1) == TYPE_OF(Int));

    CHECK(type_in(label, 0) == TYPE_OF(Str));
    CHECK(type_out(label, 0) == TYPE_OF(Str));

    CHECK(type_out(type_method_by_name(t, BURROW_S("Sum"))->ftype, 0) == TYPE_OF(Int));
}

TEST(a_signature_is_a_function_type) {
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
TEST(the_parameter_positions_are_the_ones_that_were_written) {
    const Type *move = type_method_by_name(TYPE_OF(Point), BURROW_S("Move"))->ftype;

    for (Int i = 0; i < type_num_in(move); i++)
        CHECK_INT_EQ((Int)move->fields[i].offset, i);
}

TEST(asking_a_type_that_is_not_a_function_gives_nothing) {
    CHECK_INT_EQ(type_num_in(TYPE_OF(Point)), 0);
    CHECK_INT_EQ(type_num_out(TYPE_OF(Point)), 0);
    CHECK(type_in(TYPE_OF(Point), 0) == NULL);
    CHECK(type_out(TYPE_OF(Point), 0) == NULL);

    CHECK_INT_EQ(type_num_in(NULL), 0);
    CHECK_INT_EQ(type_num_out(NULL), 0);
    CHECK(type_in(NULL, 0) == NULL);
    CHECK(type_out(NULL, 0) == NULL);
}

TEST(a_position_off_either_end_gives_nothing) {
    const Type *move = type_method_by_name(TYPE_OF(Point), BURROW_S("Move"))->ftype;

    CHECK(type_in(move, -1) == NULL);
    CHECK(type_in(move, 2) == NULL);
    CHECK(type_out(move, 0) == NULL);
}

/* --------------------------------------------------------------- the calling
 */

TEST(a_method_with_nothing_either_side_is_called) {
    Point p = {3, 4};

    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Reset")), &p, NULL,
                      NULL));
    CHECK_INT_EQ(p.X, 0);
    CHECK_INT_EQ(p.Y, 0);
}

TEST(a_method_with_arguments_gets_them_in_order) {
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

TEST(a_method_with_a_result_writes_it_where_it_was_told) {
    Point p = {7, 5};
    Int got = -1;
    void *rets[] = {&got};

    CHECK(method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Sum")), &p, NULL,
                      rets));
    CHECK_INT_EQ(got, 12);
}

TEST(a_method_with_both_is_called) {
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

TEST(the_second_types_methods_are_its_own) {
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

TEST(calling_nothing_says_so_rather_than_crashing) {
    Point p = {1, 2};

    CHECK(!method_call(NULL, &p, NULL, NULL));
    CHECK(!method_call(type_method_by_name(TYPE_OF(Point), BURROW_S("Nope")), &p, NULL,
                       NULL));

    /* A Method with no thunk, which is what a hand written array can hold. */
    CHECK(!method_call(&jumbled_methods[0], &p, NULL, NULL));
}

/* The whole point of the exercise, with nothing in hand but two names. This is
 * the shape of what net/rpc does with a call that arrived over a socket. */
TEST(a_name_and_a_method_name_are_enough_to_make_the_call) {
    Point p = {8, 9};
    Int got = -1;
    void *rets[] = {&got};

    const Type *t = TYPE_OF(Point);
    const Method *m = type_method_by_name(t, BURROW_S("Sum"));

    CHECK(m != NULL);
    CHECK_INT_EQ(type_num_in(m->ftype), 0);
    CHECK_INT_EQ(type_num_out(m->ftype), 1);
    CHECK(type_out(m->ftype, 0) == TYPE_OF(Int));
    CHECK(method_call(m, &p, NULL, rets));
    CHECK_INT_EQ(got, 17);
}

int main(void) {
    RUN(a_type_knows_how_many_methods_it_has);
    RUN(the_methods_are_in_the_order_they_were_listed);
    RUN(a_list_written_in_name_order_says_so);
    RUN(a_method_is_found_by_name);
    RUN(a_method_that_is_not_there_is_not_found);
    RUN(an_unsorted_list_is_still_searched_correctly);

    RUN(a_signature_counts_its_parameters_and_its_result);
    RUN(a_signature_names_the_types);
    RUN(a_signature_is_a_function_type);
    RUN(the_parameter_positions_are_the_ones_that_were_written);
    RUN(asking_a_type_that_is_not_a_function_gives_nothing);
    RUN(a_position_off_either_end_gives_nothing);

    RUN(a_method_with_nothing_either_side_is_called);
    RUN(a_method_with_arguments_gets_them_in_order);
    RUN(a_method_with_a_result_writes_it_where_it_was_told);
    RUN(a_method_with_both_is_called);
    RUN(the_second_types_methods_are_its_own);
    RUN(calling_nothing_says_so_rather_than_crashing);
    RUN(a_name_and_a_method_name_are_enough_to_make_the_call);

    return harness_report("method");
}
