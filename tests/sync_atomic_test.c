/* Tests for sync/atomic.
 *
 * Single threaded, like atomic_test.c underneath it and for the same reason:
 * what one thread can find is a conversion that went wrong, and this layer is
 * almost entirely conversions. Every signed operation goes out to unsigned and
 * back, add returns the new value where the instruction returns the old one,
 * and the typed structs hand their field to the plain functions. All three of
 * those are the sort of thing that is off by a sign or an operand and passes
 * anyway until somebody uses a number with the top bit set.
 *
 * The contention is in sync_atomic_concurrent_test.c, which is where Value's
 * first store gets more than one thread pointed at it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync/atomic.h"

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdint.h>

#ifndef SYNC_ATOMIC_SUITE
#define SYNC_ATOMIC_SUITE "sync/atomic"
#endif

/* gcc's -Wclobbered fires on a local that is live across a setjmp, which in a
 * test that catches panics is every local it can see. The same note is in
 * panic_test.c, and the answer is the same: what a catch block reads lives at
 * file scope. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wclobbered"
#endif

/* Negative, awkward in every byte, and nothing any arithmetic here arrives at
 * by accident. */
#define I32_ODD (-0x12345678)
#define I64_ODD (-INT64_C(0x123456789abcdef))
#define U32_ODD 0x8f1e2d3cu
#define U64_ODD UINT64_C(0x8f1e2d3c4b5a6978)

/* ------------------------------------------------------------------- add */

TEST(add_returns_the_new_value) {
    int32_t i32 = 40;
    int64_t i64 = 40;
    uint32_t u32 = 40;
    uint64_t u64 = 40;
    Uintptr up = 40;

    CHECK_INT_EQ(sync_atomic_add_int32(&i32, 2), 42);
    CHECK_INT_EQ(i32, 42);
    CHECK_INT_EQ(sync_atomic_add_int64(&i64, 2), 42);
    CHECK_INT_EQ(i64, 42);
    CHECK_INT_EQ(sync_atomic_add_uint32(&u32, 2), 42);
    CHECK_INT_EQ(u32, 42);
    CHECK(sync_atomic_add_uint64(&u64, 2) == 42);
    CHECK(u64 == 42);
    CHECK(sync_atomic_add_uintptr(&up, 2) == 42);
    CHECK(up == 42);
}

TEST(add_counts_down_as_well) {
    int32_t i32 = 10;
    int64_t i64 = 10;
    uint32_t u32 = 10;
    uint64_t u64 = 10;

    CHECK_INT_EQ(sync_atomic_add_int32(&i32, -3), 7);
    CHECK_INT_EQ(sync_atomic_add_int64(&i64, -3), 7);

    /* The unsigned ones have no negative to add, so a decrement is the two's
     * complement, which is what Go's own documentation tells you to write. */
    CHECK_INT_EQ(sync_atomic_add_uint32(&u32, ~(uint32_t)0), 9);
    CHECK(sync_atomic_add_uint64(&u64, ~(uint64_t)0) == 9);
}

TEST(add_wraps_where_the_type_wraps) {
    int32_t i32 = INT32_MAX;
    int64_t i64 = INT64_MAX;
    uint32_t u32 = UINT32_MAX;
    uint64_t u64 = UINT64_MAX;

    CHECK_INT_EQ(sync_atomic_add_int32(&i32, 1), INT32_MIN);
    CHECK(sync_atomic_add_int64(&i64, 1) == INT64_MIN);
    CHECK_INT_EQ(sync_atomic_add_uint32(&u32, 1), 0);
    CHECK(sync_atomic_add_uint64(&u64, 1) == 0);
}

/* ------------------------------------------------------------ and, and or */

TEST(and_and_or_return_the_old_value) {
    int32_t i32 = -1;
    int64_t i64 = -1;
    uint32_t u32 = U32_ODD;
    uint64_t u64 = U64_ODD;
    Uintptr up = 0;

    CHECK_INT_EQ(sync_atomic_and_int32(&i32, 0x0f0f0f0f), -1);
    CHECK_INT_EQ(i32, 0x0f0f0f0f);
    CHECK_INT_EQ(sync_atomic_or_int32(&i32, 0x70000000), 0x0f0f0f0f);
    CHECK_INT_EQ(i32, 0x7f0f0f0f);

    CHECK(sync_atomic_and_int64(&i64, INT64_C(0x0f0f0f0f0f0f0f0f)) == -1);
    CHECK(i64 == INT64_C(0x0f0f0f0f0f0f0f0f));
    CHECK(sync_atomic_or_int64(&i64, INT64_C(0x7000000000000000)) ==
          INT64_C(0x0f0f0f0f0f0f0f0f));
    CHECK(i64 == INT64_C(0x7f0f0f0f0f0f0f0f));

    CHECK_INT_EQ(sync_atomic_and_uint32(&u32, 0xffffu), U32_ODD);
    CHECK_INT_EQ(u32, U32_ODD & 0xffffu);
    CHECK_INT_EQ(sync_atomic_or_uint32(&u32, 0xffff0000u), U32_ODD & 0xffffu);
    CHECK_INT_EQ(u32, (U32_ODD & 0xffffu) | 0xffff0000u);

    CHECK(sync_atomic_and_uint64(&u64, UINT64_C(0xffffffff)) == U64_ODD);
    CHECK(u64 == (U64_ODD & UINT64_C(0xffffffff)));
    CHECK(sync_atomic_or_uint64(&u64, UINT64_C(0xff) << 56) ==
          (U64_ODD & UINT64_C(0xffffffff)));

    CHECK(sync_atomic_or_uintptr(&up, 0x81) == 0);
    CHECK(up == 0x81);
    CHECK(sync_atomic_and_uintptr(&up, 0x80) == 0x81);
    CHECK(up == 0x80);
}

/* And keeps its sign, which is the one thing a mask on a signed type can get
 * wrong: the conversion out and back has to leave the top bit where it was. */
TEST(and_of_a_negative_stays_negative) {
    int32_t i32 = I32_ODD;
    int64_t i64 = I64_ODD;

    CHECK_INT_EQ(sync_atomic_and_int32(&i32, INT32_MIN), I32_ODD);
    CHECK_INT_EQ(i32, INT32_MIN);
    CHECK(sync_atomic_and_int64(&i64, INT64_MIN) == I64_ODD);
    CHECK(i64 == INT64_MIN);
}

/* ------------------------------------------------------ load, store, swap */

TEST(load_and_store_round_trip_the_awkward_values) {
    int32_t i32 = 0;
    int64_t i64 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    Uintptr up = 0;

    sync_atomic_store_int32(&i32, I32_ODD);
    CHECK_INT_EQ(sync_atomic_load_int32(&i32), I32_ODD);
    sync_atomic_store_int32(&i32, INT32_MIN);
    CHECK_INT_EQ(sync_atomic_load_int32(&i32), INT32_MIN);

    sync_atomic_store_int64(&i64, I64_ODD);
    CHECK(sync_atomic_load_int64(&i64) == I64_ODD);
    sync_atomic_store_int64(&i64, INT64_MIN);
    CHECK(sync_atomic_load_int64(&i64) == INT64_MIN);

    sync_atomic_store_uint32(&u32, U32_ODD);
    CHECK_INT_EQ(sync_atomic_load_uint32(&u32), U32_ODD);

    sync_atomic_store_uint64(&u64, U64_ODD);
    CHECK(sync_atomic_load_uint64(&u64) == U64_ODD);

    sync_atomic_store_uintptr(&up, UINTPTR_MAX);
    CHECK(sync_atomic_load_uintptr(&up) == UINTPTR_MAX);
}

TEST(swap_returns_what_was_there) {
    int32_t i32 = I32_ODD;
    int64_t i64 = I64_ODD;
    uint32_t u32 = U32_ODD;
    uint64_t u64 = U64_ODD;
    Uintptr up = 7;

    CHECK_INT_EQ(sync_atomic_swap_int32(&i32, 1), I32_ODD);
    CHECK_INT_EQ(i32, 1);
    CHECK(sync_atomic_swap_int64(&i64, 1) == I64_ODD);
    CHECK(i64 == 1);
    CHECK_INT_EQ(sync_atomic_swap_uint32(&u32, 1), U32_ODD);
    CHECK_INT_EQ(u32, 1);
    CHECK(sync_atomic_swap_uint64(&u64, 1) == U64_ODD);
    CHECK(u64 == 1);
    CHECK(sync_atomic_swap_uintptr(&up, 1) == 7);
    CHECK(up == 1);
}

TEST(compare_and_swap_stores_only_on_a_match) {
    int32_t i32 = I32_ODD;
    int64_t i64 = I64_ODD;
    uint32_t u32 = U32_ODD;
    uint64_t u64 = U64_ODD;
    Uintptr up = 7;

    CHECK(!sync_atomic_compare_and_swap_int32(&i32, 0, 1));
    CHECK_INT_EQ(i32, I32_ODD);
    CHECK(sync_atomic_compare_and_swap_int32(&i32, I32_ODD, 1));
    CHECK_INT_EQ(i32, 1);

    CHECK(!sync_atomic_compare_and_swap_int64(&i64, 0, 1));
    CHECK(i64 == I64_ODD);
    CHECK(sync_atomic_compare_and_swap_int64(&i64, I64_ODD, 1));
    CHECK(i64 == 1);

    CHECK(!sync_atomic_compare_and_swap_uint32(&u32, 0, 1));
    CHECK(sync_atomic_compare_and_swap_uint32(&u32, U32_ODD, 1));
    CHECK_INT_EQ(u32, 1);

    CHECK(!sync_atomic_compare_and_swap_uint64(&u64, 0, 1));
    CHECK(sync_atomic_compare_and_swap_uint64(&u64, U64_ODD, 1));
    CHECK(u64 == 1);

    CHECK(!sync_atomic_compare_and_swap_uintptr(&up, 0, 1));
    CHECK(sync_atomic_compare_and_swap_uintptr(&up, 7, 1));
    CHECK(up == 1);
}

/* A retry loop is the shape every caller writes, and it has to terminate on the
 * first turn when nobody else is here. */
TEST(a_compare_and_swap_loop_terminates_on_its_own) {
    int64_t n = 21;
    int turns = 0;

    for (;;) {
        int64_t old = sync_atomic_load_int64(&n);

        turns++;
        if (sync_atomic_compare_and_swap_int64(&n, old, old * 2))
            break;
    }
    CHECK(n == 42);
    CHECK_INT_EQ(turns, 1);
}

/* -------------------------------------------------------------- pointers */

TEST(the_pointer_functions_round_trip) {
    int left = 1;
    int right = 2;
    void *p = NULL;

    CHECK(sync_atomic_load_pointer(&p) == NULL);

    sync_atomic_store_pointer(&p, &left);
    CHECK(sync_atomic_load_pointer(&p) == &left);
    CHECK(sync_atomic_swap_pointer(&p, &right) == &left);
    CHECK(p == &right);

    CHECK(!sync_atomic_compare_and_swap_pointer(&p, &left, NULL));
    CHECK(p == &right);
    CHECK(sync_atomic_compare_and_swap_pointer(&p, &right, NULL));
    CHECK(p == NULL);
}

/* ----------------------------------------------------------- the structs */

TEST(the_zero_value_of_every_type_is_ready_to_use) {
    SyncAtomicBool b = {0};
    SyncAtomicInt32 i32 = {0};
    SyncAtomicInt64 i64 = {0};
    SyncAtomicUint32 u32 = {0};
    SyncAtomicUint64 u64 = {0};
    SyncAtomicUintptr up = {0};
    SyncAtomicPointer p = {0};
    SyncAtomicValue v = {0, 0};

    CHECK(!sync_atomic_bool_load(&b));
    CHECK_INT_EQ(sync_atomic_int32_load(&i32), 0);
    CHECK(sync_atomic_int64_load(&i64) == 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&u32), 0);
    CHECK(sync_atomic_uint64_load(&u64) == 0);
    CHECK(sync_atomic_uintptr_load(&up) == 0);
    CHECK(sync_atomic_pointer_load(&p) == NULL);
    CHECK(BURROW_ANY_IS_NIL(sync_atomic_value_load(&v)));
}

TEST(the_bool_type_has_the_four_operations) {
    SyncAtomicBool b = {0};

    sync_atomic_bool_store(&b, true);
    CHECK(sync_atomic_bool_load(&b));
    CHECK(sync_atomic_bool_swap(&b, false));
    CHECK(!sync_atomic_bool_load(&b));

    CHECK(!sync_atomic_bool_compare_and_swap(&b, true, true));
    CHECK(!sync_atomic_bool_load(&b));
    CHECK(sync_atomic_bool_compare_and_swap(&b, false, true));
    CHECK(sync_atomic_bool_load(&b));
}

/* Every method is the plain function with the field filled in, so what this
 * checks is that each one got handed the right field and the right operand. A
 * copy and paste error between two of these is invisible to a compiler and
 * obvious here. */
TEST(the_integer_types_have_the_seven_operations) {
    SyncAtomicInt32 i32 = {0};
    SyncAtomicInt64 i64 = {0};
    SyncAtomicUint32 u32 = {0};
    SyncAtomicUint64 u64 = {0};
    SyncAtomicUintptr up = {0};

    sync_atomic_int32_store(&i32, I32_ODD);
    CHECK_INT_EQ(sync_atomic_int32_load(&i32), I32_ODD);
    CHECK_INT_EQ(sync_atomic_int32_add(&i32, 8), I32_ODD + 8);
    CHECK_INT_EQ(sync_atomic_int32_swap(&i32, -1), I32_ODD + 8);
    CHECK_INT_EQ(sync_atomic_int32_and(&i32, 0x0f0f0f0f), -1);
    CHECK_INT_EQ(sync_atomic_int32_or(&i32, 0x70000000), 0x0f0f0f0f);
    CHECK(!sync_atomic_int32_compare_and_swap(&i32, 0, 5));
    CHECK(sync_atomic_int32_compare_and_swap(&i32, 0x7f0f0f0f, 5));
    CHECK_INT_EQ(sync_atomic_int32_load(&i32), 5);

    sync_atomic_int64_store(&i64, I64_ODD);
    CHECK(sync_atomic_int64_load(&i64) == I64_ODD);
    CHECK(sync_atomic_int64_add(&i64, 8) == I64_ODD + 8);
    CHECK(sync_atomic_int64_swap(&i64, -1) == I64_ODD + 8);
    CHECK(sync_atomic_int64_and(&i64, INT64_C(0x0f0f0f0f0f0f0f0f)) == -1);
    CHECK(sync_atomic_int64_or(&i64, INT64_C(0x7000000000000000)) ==
          INT64_C(0x0f0f0f0f0f0f0f0f));
    CHECK(!sync_atomic_int64_compare_and_swap(&i64, 0, 5));
    CHECK(sync_atomic_int64_compare_and_swap(&i64, INT64_C(0x7f0f0f0f0f0f0f0f), 5));
    CHECK(sync_atomic_int64_load(&i64) == 5);

    sync_atomic_uint32_store(&u32, U32_ODD);
    CHECK_INT_EQ(sync_atomic_uint32_load(&u32), U32_ODD);
    CHECK_INT_EQ(sync_atomic_uint32_add(&u32, 8), U32_ODD + 8);
    CHECK_INT_EQ(sync_atomic_uint32_swap(&u32, 0xffffffffu), U32_ODD + 8);
    CHECK_INT_EQ(sync_atomic_uint32_and(&u32, 0xffffu), 0xffffffffu);
    CHECK_INT_EQ(sync_atomic_uint32_or(&u32, 0xffff0000u), 0xffffu);
    CHECK(!sync_atomic_uint32_compare_and_swap(&u32, 0, 5));
    CHECK(sync_atomic_uint32_compare_and_swap(&u32, 0xffffffffu, 5));
    CHECK_INT_EQ(sync_atomic_uint32_load(&u32), 5);

    sync_atomic_uint64_store(&u64, U64_ODD);
    CHECK(sync_atomic_uint64_load(&u64) == U64_ODD);
    CHECK(sync_atomic_uint64_add(&u64, 8) == U64_ODD + 8);
    CHECK(sync_atomic_uint64_swap(&u64, UINT64_MAX) == U64_ODD + 8);
    CHECK(sync_atomic_uint64_and(&u64, UINT64_C(0xffffffff)) == UINT64_MAX);
    CHECK(sync_atomic_uint64_or(&u64, UINT64_C(0xffffffff) << 32) ==
          UINT64_C(0xffffffff));
    CHECK(!sync_atomic_uint64_compare_and_swap(&u64, 0, 5));
    CHECK(sync_atomic_uint64_compare_and_swap(&u64, UINT64_MAX, 5));
    CHECK(sync_atomic_uint64_load(&u64) == 5);

    sync_atomic_uintptr_store(&up, 40);
    CHECK(sync_atomic_uintptr_load(&up) == 40);
    CHECK(sync_atomic_uintptr_add(&up, 2) == 42);
    CHECK(sync_atomic_uintptr_swap(&up, 0x81) == 42);
    CHECK(sync_atomic_uintptr_and(&up, 0x80) == 0x81);
    CHECK(sync_atomic_uintptr_or(&up, 1) == 0x80);
    CHECK(!sync_atomic_uintptr_compare_and_swap(&up, 0, 5));
    CHECK(sync_atomic_uintptr_compare_and_swap(&up, 0x81, 5));
    CHECK(sync_atomic_uintptr_load(&up) == 5);
}

TEST(the_pointer_type_has_the_four_operations) {
    SyncAtomicPointer p = {0};
    int left = 1;
    int right = 2;

    sync_atomic_pointer_store(&p, &left);
    CHECK(sync_atomic_pointer_load(&p) == &left);
    CHECK(sync_atomic_pointer_swap(&p, &right) == &left);
    CHECK(!sync_atomic_pointer_compare_and_swap(&p, &left, NULL));
    CHECK(sync_atomic_pointer_compare_and_swap(&p, &right, NULL));
    CHECK(sync_atomic_pointer_load(&p) == NULL);
}

/* ------------------------------------------------------------------ value */

static Int value_n = 7;
static Int value_m = 9;

TEST(a_value_loads_what_was_stored) {
    SyncAtomicValue v = {0, 0};
    Any got;

    CHECK(BURROW_ANY_IS_NIL(sync_atomic_value_load(&v)));

    sync_atomic_value_store(&v, BURROW_ANY(TYPE_INT, &value_n));
    got = sync_atomic_value_load(&v);
    CHECK(!BURROW_ANY_IS_NIL(got));
    CHECK(got.t == TYPE_INT);
    CHECK(*(Int *)got.data == 7);

    sync_atomic_value_store(&v, BURROW_ANY(TYPE_INT, &value_m));
    CHECK(*(Int *)sync_atomic_value_load(&v).data == 9);
}

TEST(a_value_swap_returns_the_one_before_it) {
    SyncAtomicValue v = {0, 0};
    Any old;

    /* The first swap has nothing to hand back, which is a nil Any and not a
     * zero of the type that is about to be stored. */
    CHECK(
        BURROW_ANY_IS_NIL(sync_atomic_value_swap(&v, BURROW_ANY(TYPE_INT, &value_n))));

    old = sync_atomic_value_swap(&v, BURROW_ANY(TYPE_INT, &value_m));
    CHECK(old.t == TYPE_INT);
    CHECK(*(Int *)old.data == 7);
    CHECK(*(Int *)sync_atomic_value_load(&v).data == 9);
}

/* The comparison is Go's ==, which goes through the type descriptor, so a value
 * equal to the stored one but held somewhere else still matches. That is the
 * one thing Value can do that a compare and swap on a word cannot. */
TEST(a_value_compares_by_value_and_not_by_address) {
    SyncAtomicValue v = {0, 0};
    Int same = 7;
    Int other = 8;

    CHECK(!sync_atomic_value_compare_and_swap(&v, BURROW_ANY(TYPE_INT, &value_n),
                                              BURROW_ANY(TYPE_INT, &value_m)));

    /* Nothing stored yet, so only a nil old matches. */
    CHECK(sync_atomic_value_compare_and_swap(&v, (Any){NULL, NULL},
                                             BURROW_ANY(TYPE_INT, &value_n)));
    CHECK(*(Int *)sync_atomic_value_load(&v).data == 7);

    CHECK(!sync_atomic_value_compare_and_swap(&v, BURROW_ANY(TYPE_INT, &other),
                                              BURROW_ANY(TYPE_INT, &value_m)));
    CHECK(sync_atomic_value_compare_and_swap(&v, BURROW_ANY(TYPE_INT, &same),
                                             BURROW_ANY(TYPE_INT, &value_m)));
    CHECK(*(Int *)sync_atomic_value_load(&v).data == 9);
}

/* ------------------------------------------------------- what value refuses */

static SyncAtomicValue panicking;
static int caught;
static char message[128];

static void remember(Any p) {
    Str s = panic_text(p);
    size_t n = (size_t)s.len < sizeof message - 1 ? (size_t)s.len : sizeof message - 1;

    caught++;
    memcpy(message, s.p, n);
    message[n] = '\0';
}

TEST(storing_nil_into_a_value_panics) {
    caught = 0;
    panicking = (SyncAtomicValue){0, 0};

    BURROW_TRY {
        sync_atomic_value_store(&panicking, (Any){NULL, NULL});
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(caught, 1);
    CHECK_STR_EQ(message, "sync/atomic: store of nil value into Value");
}

TEST(storing_a_second_type_into_a_value_panics) {
    caught = 0;
    panicking = (SyncAtomicValue){0, 0};
    sync_atomic_value_store(&panicking, BURROW_ANY(TYPE_INT, &value_n));

    BURROW_TRY {
        sync_atomic_value_store(&panicking, BURROW_ANY(TYPE_UINT, &value_m));
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(caught, 1);
    CHECK_STR_EQ(message,
                 "sync/atomic: store of inconsistently typed value into Value");
}

TEST(swap_and_compare_and_swap_refuse_the_same_two_things) {
    caught = 0;
    panicking = (SyncAtomicValue){0, 0};

    BURROW_TRY {
        (void)sync_atomic_value_swap(&panicking, (Any){NULL, NULL});
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;
    CHECK_STR_EQ(message, "sync/atomic: swap of nil value into Value");

    BURROW_TRY {
        (void)sync_atomic_value_compare_and_swap(&panicking, (Any){NULL, NULL},
                                                 (Any){NULL, NULL});
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;
    CHECK_STR_EQ(message, "sync/atomic: compare and swap of nil value into Value");

    /* An old and a new of different types is refused before anything is read,
     * so it panics even on a Value nobody has stored to. */
    BURROW_TRY {
        (void)sync_atomic_value_compare_and_swap(&panicking,
                                                 BURROW_ANY(TYPE_INT, &value_n),
                                                 BURROW_ANY(TYPE_UINT, &value_m));
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;
    CHECK_STR_EQ(message,
                 "sync/atomic: compare and swap of inconsistently typed values");

    sync_atomic_value_store(&panicking, BURROW_ANY(TYPE_INT, &value_n));
    BURROW_TRY {
        (void)sync_atomic_value_swap(&panicking, BURROW_ANY(TYPE_UINT, &value_m));
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;
    CHECK_STR_EQ(message, "sync/atomic: swap of inconsistently typed value into Value");

    BURROW_TRY {
        (void)sync_atomic_value_compare_and_swap(&panicking,
                                                 BURROW_ANY(TYPE_UINT, &value_n),
                                                 BURROW_ANY(TYPE_UINT, &value_m));
    }
    BURROW_CATCH(p) {
        remember(p);
    }
    BURROW_TRY_END;
    CHECK_STR_EQ(message,
                 "sync/atomic: compare and swap of inconsistently typed value into "
                 "Value");

    CHECK_INT_EQ(caught, 5);
}

int main(void) {
    RUN(add_returns_the_new_value);
    RUN(add_counts_down_as_well);
    RUN(add_wraps_where_the_type_wraps);
    RUN(and_and_or_return_the_old_value);
    RUN(and_of_a_negative_stays_negative);
    RUN(load_and_store_round_trip_the_awkward_values);
    RUN(swap_returns_what_was_there);
    RUN(compare_and_swap_stores_only_on_a_match);
    RUN(a_compare_and_swap_loop_terminates_on_its_own);
    RUN(the_pointer_functions_round_trip);
    RUN(the_zero_value_of_every_type_is_ready_to_use);
    RUN(the_bool_type_has_the_four_operations);
    RUN(the_integer_types_have_the_seven_operations);
    RUN(the_pointer_type_has_the_four_operations);
    RUN(a_value_loads_what_was_stored);
    RUN(a_value_swap_returns_the_one_before_it);
    RUN(a_value_compares_by_value_and_not_by_address);
    RUN(storing_nil_into_a_value_panics);
    RUN(storing_a_second_type_into_a_value_panics);
    RUN(swap_and_compare_and_swap_refuse_the_same_two_things);
    return harness_report(SYNC_ATOMIC_SUITE);
}
