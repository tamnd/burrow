/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "harness.h"

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

/* ------------------------------------------------------- the types under test
 *
 * Declared the way a ported package declares one: a named type per signature,
 * next to the function that takes it. */

BURROW_FUNC(Filter, bool, Str s);
BURROW_FUNC(BinOp, Int, Int x, Int y);
BURROW_FUNC(ReadFn, Int, Slice p, Error *err);

/* --------------------------------------------------------------- the targets
 *
 * Three shapes: one that ignores its env, one that reads it, and one that
 * writes to it, which is what a Go closure capturing a variable by reference
 * comes out as. */

static bool always_true(void *env, Str s) {
    (void)env;
    (void)s;
    return true;
}

static bool is_empty(void *env, Str s) {
    (void)env;
    return s.len == 0;
}

typedef struct PrefixEnv {
    Str prefix;
} PrefixEnv;

static bool has_prefix(void *env, Str s) {
    PrefixEnv *e = (PrefixEnv *)env;
    if (s.len < e->prefix.len)
        return false;
    return str_eq(str_from_bytes(s.p, e->prefix.len), e->prefix);
}

typedef struct CountEnv {
    Int calls;
    Int matched;
} CountEnv;

static bool counts_what_it_saw(void *env, Str s) {
    CountEnv *e = (CountEnv *)env;
    e->calls++;
    if (s.len > 0)
        e->matched++;
    return s.len > 0;
}

static void bump(void *env) {
    CountEnv *e = (CountEnv *)env;
    e->calls++;
}

static Int add(void *env, Int x, Int y) {
    (void)env;
    return x + y;
}

static Int multiply(void *env, Int x, Int y) {
    (void)env;
    return x * y;
}

typedef struct ScaleEnv {
    Int by;
} ScaleEnv;

static Int add_then_scale(void *env, Int x, Int y) {
    ScaleEnv *e = (ScaleEnv *)env;
    return (x + y) * e->by;
}

/* ------------------------------------------------------- a higher order user
 *
 * The point of the whole thing is being able to write this, so the test writes
 * it. Nothing in the library takes a Filter yet, and when strings and slices
 * land they will take types declared exactly like this one. */
static Int count_if(Slice lines, Filter keep) {
    Int n = 0, i;
    for (i = 0; i < lines.len; i++) {
        Str line = *(Str *)slice_at(lines, i);
        if (BURROW_CALLF(keep, line))
            n++;
    }
    return n;
}

static Slice three_lines(void) {
    Slice s = slice_make(a, TYPE_STRING, 3, 3);
    *(Str *)slice_at(s, 0) = BURROW_S("gopher");
    *(Str *)slice_at(s, 1) = BURROW_S("");
    *(Str *)slice_at(s, 2) = BURROW_S("go");
    return s;
}

/* -------------------------------------------------------------------- nil */

TEST(a_zeroed_function_value_is_nil) {
    Filter f = {NULL, NULL};
    Func done = {NULL, NULL};

    CHECK(BURROW_FUNC_IS_NIL(f));
    CHECK(BURROW_FUNC_IS_NIL(done));
}

TEST(a_struct_field_of_function_type_starts_out_nil) {
    struct Holder {
        Int n;
        Filter keep;
        Func on_done;
    };
    struct Holder *h = mem_alloc(a, sizeof(struct Holder), _Alignof(struct Holder));

    CHECK(h != NULL);
    CHECK(BURROW_FUNC_IS_NIL(h->keep));
    CHECK(BURROW_FUNC_IS_NIL(h->on_done));
    CHECK_INT_EQ(h->n, 0);
}

TEST(a_value_with_a_function_and_no_env_is_not_nil) {
    Filter f = BURROW_FN(Filter, always_true, NULL);

    CHECK(!BURROW_FUNC_IS_NIL(f));
    CHECK(f.env == NULL);
}

/* ------------------------------------------------------------------ calling */

TEST(a_call_reaches_the_function_it_was_built_with) {
    Filter yes = BURROW_FN(Filter, always_true, NULL);
    Filter empty = BURROW_FN(Filter, is_empty, NULL);

    CHECK(BURROW_CALLF(yes, BURROW_S("anything")));
    CHECK(!BURROW_CALLF(empty, BURROW_S("anything")));
    CHECK(BURROW_CALLF(empty, BURROW_S("")));
}

TEST(a_call_passes_the_env_first_and_the_arguments_after) {
    ScaleEnv e = {10};
    BinOp plus = BURROW_FN(BinOp, add, NULL);
    BinOp times = BURROW_FN(BinOp, multiply, NULL);
    BinOp scaled = BURROW_FN(BinOp, add_then_scale, &e);

    CHECK_INT_EQ(BURROW_CALLF(plus, 2, 3), 5);
    CHECK_INT_EQ(BURROW_CALLF(times, 2, 3), 6);
    CHECK_INT_EQ(BURROW_CALLF(scaled, 2, 3), 50);
}

TEST(a_call_with_no_arguments_needs_the_other_macro) {
    CountEnv e = {0, 0};
    Func on_done = BURROW_FN(Func, bump, &e);

    /* A Func returns nothing, so the env is the only evidence the call
     * happened, which is also true of every Func the library will run: what
     * sync.Once.Do takes, what time.AfterFunc fires, what a goroutine starts
     * with. */
    BURROW_CALLF0(on_done);
    BURROW_CALLF0(on_done);
    CHECK_INT_EQ(e.calls, 2);
    CHECK(!BURROW_FUNC_IS_NIL(on_done));
}

/* ------------------------------------------------------------------ the env */

TEST(the_env_is_what_a_captured_variable_becomes) {
    PrefixEnv e = {BURROW_S("go")};
    Filter f = BURROW_FN(Filter, has_prefix, &e);

    CHECK(BURROW_CALLF(f, BURROW_S("gopher")));
    CHECK(BURROW_CALLF(f, BURROW_S("go")));
    CHECK(!BURROW_CALLF(f, BURROW_S("rust")));
    CHECK(!BURROW_CALLF(f, BURROW_S("g")));
}

TEST(changing_the_env_changes_what_the_value_does) {
    PrefixEnv e = {BURROW_S("go")};
    Filter f = BURROW_FN(Filter, has_prefix, &e);

    CHECK(BURROW_CALLF(f, BURROW_S("gopher")));
    e.prefix = BURROW_S("ru");
    CHECK(!BURROW_CALLF(f, BURROW_S("gopher")));
    CHECK(BURROW_CALLF(f, BURROW_S("rust")));
}

TEST(writes_to_the_env_are_visible_after_the_call) {
    CountEnv e = {0, 0};
    Filter f = BURROW_FN(Filter, counts_what_it_saw, &e);

    CHECK(BURROW_CALLF(f, BURROW_S("gopher")));
    CHECK(!BURROW_CALLF(f, BURROW_S("")));
    CHECK(BURROW_CALLF(f, BURROW_S("go")));
    CHECK_INT_EQ(e.calls, 3);
    CHECK_INT_EQ(e.matched, 2);
}

TEST(two_values_sharing_one_env_see_each_others_writes) {
    CountEnv e = {0, 0};
    Filter one = BURROW_FN(Filter, counts_what_it_saw, &e);
    Filter two = BURROW_FN(Filter, counts_what_it_saw, &e);

    CHECK(BURROW_CALLF(one, BURROW_S("a")));
    CHECK(BURROW_CALLF(two, BURROW_S("b")));
    CHECK_INT_EQ(e.calls, 2);
}

TEST(two_values_with_their_own_envs_do_not) {
    CountEnv first = {0, 0};
    CountEnv second = {0, 0};
    Filter one = BURROW_FN(Filter, counts_what_it_saw, &first);
    Filter two = BURROW_FN(Filter, counts_what_it_saw, &second);

    CHECK(BURROW_CALLF(one, BURROW_S("a")));
    CHECK(BURROW_CALLF(one, BURROW_S("b")));
    CHECK(BURROW_CALLF(two, BURROW_S("c")));
    CHECK_INT_EQ(first.calls, 2);
    CHECK_INT_EQ(second.calls, 1);
}

TEST(an_env_in_an_allocator_outlives_the_frame_that_made_it) {
    Filter f;

    {
        PrefixEnv *e = mem_alloc(a, sizeof(PrefixEnv), _Alignof(PrefixEnv));
        CHECK(e != NULL);
        e->prefix = BURROW_S("go");
        f = BURROW_FN(Filter, has_prefix, e);
    }

    CHECK(BURROW_CALLF(f, BURROW_S("gopher")));
    CHECK(!BURROW_CALLF(f, BURROW_S("rust")));
}

/* --------------------------------------------------------------- passing one */

TEST(a_function_value_passed_to_something_else_works_there) {
    Slice lines = three_lines();
    PrefixEnv e = {BURROW_S("go")};

    CHECK_INT_EQ(count_if(lines, BURROW_FN(Filter, always_true, NULL)), 3);
    CHECK_INT_EQ(count_if(lines, BURROW_FN(Filter, is_empty, NULL)), 1);
    CHECK_INT_EQ(count_if(lines, BURROW_FN(Filter, has_prefix, &e)), 2);
}

TEST(a_literal_at_the_call_site_lives_long_enough_for_the_call) {
    Slice lines = three_lines();
    PrefixEnv e = {BURROW_S("gop")};

    /* The value is built in the argument list and the callee finishes before
     * the statement does, which is the case the compound literal is for. */
    CHECK_INT_EQ(count_if(lines, BURROW_FN(Filter, has_prefix, &e)), 1);
}

TEST(a_table_of_function_values_dispatches_on_an_index) {
    BinOp ops[2];
    Int i;
    Int got[2];

    ops[0] = BURROW_FN(BinOp, add, NULL);
    ops[1] = BURROW_FN(BinOp, multiply, NULL);

    for (i = 0; i < 2; i++)
        got[i] = BURROW_CALLF(ops[i], 3, 4);

    CHECK_INT_EQ(got[0], 7);
    CHECK_INT_EQ(got[1], 12);
}

TEST(a_function_value_stored_in_a_struct_survives_the_round_trip) {
    struct Holder {
        Filter keep;
        Int calls;
    } h;
    CountEnv e = {0, 0};

    h.keep = BURROW_FN(Filter, counts_what_it_saw, &e);
    h.calls = 0;

    CHECK(BURROW_CALLF(h.keep, BURROW_S("go")));
    h.calls++;
    CHECK_INT_EQ(e.calls, h.calls);
}

/* ---------------------------------------------------------- the error shape
 *
 * The signature the design document uses as its example, checked here because
 * every reader and writer in the library will have this shape and the out
 * parameter has to survive going through the pair. */

typedef struct ChunkEnv {
    Str data;
    Int pos;
} ChunkEnv;

static Int read_chunk(void *env, Slice p, Error *err) {
    ChunkEnv *e = (ChunkEnv *)env;
    Int left = e->data.len - e->pos;
    Int n, i;

    if (left <= 0) {
        *err = burrow_err_out_of_memory; /* any error will do, this one is free */
        return 0;
    }
    n = left < p.len ? left : p.len;
    for (i = 0; i < n; i++)
        *(Byte *)slice_at(p, i) = e->data.p[e->pos + i];
    e->pos += n;
    return n;
}

TEST(an_out_parameter_survives_the_trip_through_a_function_value) {
    ChunkEnv e = {BURROW_S("gopher"), 0};
    ReadFn read = BURROW_FN(ReadFn, read_chunk, &e);
    Slice buf = slice_make(a, TYPE_BYTE, 4, 4);
    Error err = BURROW_NO_ERROR;
    Int n;

    n = BURROW_CALLF(read, buf, &err);
    CHECK_INT_EQ(n, 4);
    CHECK(BURROW_OK(err));
    CHECK(*(Byte *)slice_at(buf, 0) == 'g');

    n = BURROW_CALLF(read, buf, &err);
    CHECK_INT_EQ(n, 2);
    CHECK(BURROW_OK(err));

    n = BURROW_CALLF(read, buf, &err);
    CHECK_INT_EQ(n, 0);
    CHECK(BURROW_FAILED(err));
}

/* ------------------------------------------------------------------ the size
 *
 * Two words, and the function pointer first. Both are load bearing: the second
 * is why a zeroed value is nil, and the first is what makes passing one cheap
 * enough that no package needs to take a bare function pointer instead. */

TEST(a_function_value_is_two_words_with_the_function_first) {
    Filter f = BURROW_FN(Filter, always_true, NULL);

    CHECK(sizeof(Filter) == 2 * sizeof(void *));
    CHECK(sizeof(Func) == sizeof(Filter));
    CHECK((const void *)&f == (const void *)&f.f);
}

int main(void) {
    setup();
    RUN(a_zeroed_function_value_is_nil);
    RUN(a_struct_field_of_function_type_starts_out_nil);
    RUN(a_value_with_a_function_and_no_env_is_not_nil);
    RUN(a_call_reaches_the_function_it_was_built_with);
    RUN(a_call_passes_the_env_first_and_the_arguments_after);
    RUN(a_call_with_no_arguments_needs_the_other_macro);
    RUN(the_env_is_what_a_captured_variable_becomes);
    RUN(changing_the_env_changes_what_the_value_does);
    RUN(writes_to_the_env_are_visible_after_the_call);
    RUN(two_values_sharing_one_env_see_each_others_writes);
    RUN(two_values_with_their_own_envs_do_not);
    RUN(an_env_in_an_allocator_outlives_the_frame_that_made_it);
    RUN(a_function_value_passed_to_something_else_works_there);
    RUN(a_literal_at_the_call_site_lives_long_enough_for_the_call);
    RUN(a_table_of_function_values_dispatches_on_an_index);
    RUN(a_function_value_stored_in_a_struct_survives_the_round_trip);
    RUN(an_out_parameter_survives_the_trip_through_a_function_value);
    RUN(a_function_value_is_two_words_with_the_function_first);
    teardown();
    return harness_report("func");
}
