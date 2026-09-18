/* Tests for context switching.
 *
 * The hard part to test is not that control arrives somewhere, it is that
 * everything the ABI promised would survive the trip actually did. A switch
 * that forgets a register passes any test that only checks where control went,
 * and then fails months later inside somebody else's arithmetic. So there is a
 * test here that holds a pile of integers and a pile of doubles live across a
 * switch on both sides of it, which is what makes a compiler put them in the
 * callee saved registers in the first place.
 *
 * The rest is the shapes the scheduler will use: run and come back, resume
 * where it stopped, hand control between two contexts, and nest three deep.
 *
 * Contexts and stacks are file scope because a running context reads its entry
 * and argument out of its own context, the same reason thread_test.c gives for
 * its handles.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/context.h"

#include "harness.h"

#include <stdint.h>

/* Comfortably more than anything here needs, and a long way above
 * BURROW_CONTEXT_STACK_MIN so that the fallback paths have room for whatever
 * their signal frames want. */
#define STACK_SIZE (64 * 1024)

/* The context for the thread's own stack. Everything switches back to this. */
static burrow__Context main_ctx;

/* ------------------------------------------------------ run, and come back */

static burrow__Context ran_ctx;
static _Alignas(16) unsigned char ran_stack[STACK_SIZE];
static int ran_count;
static void *ran_arg;

static void note_that_it_ran(void *arg) {
    ran_count++;
    ran_arg = arg;
}

TEST(a_context_runs_its_entry_and_returns_to_its_link) {
    int marker = 7;

    CHECK(burrow__context_attach(&main_ctx));
    ran_count = 0;
    ran_arg = NULL;

    CHECK(burrow__context_make(&ran_ctx, ran_stack, sizeof ran_stack, note_that_it_ran,
                               &marker, &main_ctx));

    /* Making a context runs none of it. */
    CHECK(ran_count == 0);

    burrow__context_switch(&main_ctx, &ran_ctx);

    /* The entry ran and then returned, and returning is what sent control back
     * here, since nothing in note_that_it_ran switches anywhere. */
    CHECK(ran_count == 1);
    CHECK(ran_arg == &marker);

    burrow__context_free(&ran_ctx);
    burrow__context_detach(&main_ctx);
}

/* ------------------------------------------------- stopping and continuing */

static burrow__Context step_ctx;
static _Alignas(16) unsigned char step_stack[STACK_SIZE];
static int step_stage;

static void three_steps(void *arg) {
    (void)arg;

    /* A local, so it lives on the context's own stack. If a switch loses the
     * stack pointer this is the value that comes back wrong. */
    int mine = 100;

    step_stage = mine + 1;
    burrow__context_switch(&step_ctx, &main_ctx);

    step_stage = mine + 2;
    burrow__context_switch(&step_ctx, &main_ctx);

    step_stage = mine + 3;
}

TEST(a_context_carries_on_where_it_stopped) {
    CHECK(burrow__context_attach(&main_ctx));
    step_stage = 0;

    CHECK(burrow__context_make(&step_ctx, step_stack, sizeof step_stack, three_steps,
                               NULL, &main_ctx));

    burrow__context_switch(&main_ctx, &step_ctx);
    CHECK(step_stage == 101);

    burrow__context_switch(&main_ctx, &step_ctx);
    CHECK(step_stage == 102);

    /* The third one runs off the end of three_steps, so this return is the link
     * being taken rather than a switch. */
    burrow__context_switch(&main_ctx, &step_ctx);
    CHECK(step_stage == 103);

    burrow__context_free(&step_ctx);
    burrow__context_detach(&main_ctx);
}

/* ------------------------------------------------------------- the registers */

/* The point of the whole exercise. Both sides of the switch hold more live
 * values than there are caller saved registers, which is what makes a compiler
 * reach for the callee saved ones, which is what this is trying to break.
 *
 * The seeds are volatile so that none of it folds into constants at compile
 * time, which would leave the test checking nothing at all. */
static volatile uint64_t int_seed = 0x0123456789abcdefU;
static volatile double fp_seed = 3.0;

static burrow__Context reg_ctx;
static _Alignas(16) unsigned char reg_stack[STACK_SIZE];
static int reg_ok;

static void scribble_over_everything(void *arg) {
    (void)arg;

    /* Different values from the ones the caller is holding, so that a switch
     * which fails to save a register hands the caller one of these instead and
     * the mismatch is visible rather than a coincidence. */
    uint64_t a = int_seed ^ 0x11U, b = int_seed ^ 0x22U, c = int_seed ^ 0x33U;
    uint64_t d = int_seed ^ 0x44U, e = int_seed ^ 0x55U, f = int_seed ^ 0x66U;
    uint64_t g = int_seed ^ 0x77U, h = int_seed ^ 0x88U;

    double p = fp_seed * 11.0, q = fp_seed * 12.0, r = fp_seed * 13.0;
    double s = fp_seed * 14.0, t = fp_seed * 15.0, u = fp_seed * 16.0;
    double v = fp_seed * 17.0, w = fp_seed * 18.0;

    burrow__context_switch(&reg_ctx, &main_ctx);

    reg_ok = a == (int_seed ^ 0x11U) && b == (int_seed ^ 0x22U) &&
             c == (int_seed ^ 0x33U) && d == (int_seed ^ 0x44U) &&
             e == (int_seed ^ 0x55U) && f == (int_seed ^ 0x66U) &&
             g == (int_seed ^ 0x77U) && h == (int_seed ^ 0x88U) &&
             p == fp_seed * 11.0 && q == fp_seed * 12.0 && r == fp_seed * 13.0 &&
             s == fp_seed * 14.0 && t == fp_seed * 15.0 && u == fp_seed * 16.0 &&
             v == fp_seed * 17.0 && w == fp_seed * 18.0;
}

TEST(everything_the_abi_promised_survives_a_switch) {
    uint64_t a = int_seed + 1U, b = int_seed + 2U, c = int_seed + 3U;
    uint64_t d = int_seed + 4U, e = int_seed + 5U, f = int_seed + 6U;
    uint64_t g = int_seed + 7U, h = int_seed + 8U;

    double p = fp_seed + 1.0, q = fp_seed + 2.0, r = fp_seed + 3.0;
    double s = fp_seed + 4.0, t = fp_seed + 5.0, u = fp_seed + 6.0;
    double v = fp_seed + 7.0, w = fp_seed + 8.0;

    CHECK(burrow__context_attach(&main_ctx));
    reg_ok = 0;

    CHECK(burrow__context_make(&reg_ctx, reg_stack, sizeof reg_stack,
                               scribble_over_everything, NULL, &main_ctx));

    /* Out to a context that fills every callee saved register with something
     * else, and back. */
    burrow__context_switch(&main_ctx, &reg_ctx);

    CHECK(a == int_seed + 1U);
    CHECK(b == int_seed + 2U);
    CHECK(c == int_seed + 3U);
    CHECK(d == int_seed + 4U);
    CHECK(e == int_seed + 5U);
    CHECK(f == int_seed + 6U);
    CHECK(g == int_seed + 7U);
    CHECK(h == int_seed + 8U);

    CHECK(p == fp_seed + 1.0);
    CHECK(q == fp_seed + 2.0);
    CHECK(r == fp_seed + 3.0);
    CHECK(s == fp_seed + 4.0);
    CHECK(t == fp_seed + 5.0);
    CHECK(u == fp_seed + 6.0);
    CHECK(v == fp_seed + 7.0);
    CHECK(w == fp_seed + 8.0);

    /* And the same question asked from the other side. */
    burrow__context_switch(&main_ctx, &reg_ctx);
    CHECK(reg_ok == 1);

    burrow__context_free(&reg_ctx);
    burrow__context_detach(&main_ctx);
}

/* ----------------------------------------------------------------- ping pong */

/* What the scheduler does all day. Also the shape that finds a switch which
 * works once and corrupts something on the way back, because a single bad
 * volley out of a thousand stops the count dead. */
#define VOLLEYS 1000

static burrow__Context pong_ctx;
static _Alignas(16) unsigned char pong_stack[STACK_SIZE];
static int volleys_seen;

static void pong(void *arg) {
    (void)arg;
    for (int i = 0; i < VOLLEYS; i++) {
        volleys_seen++;
        burrow__context_switch(&pong_ctx, &main_ctx);
    }
}

TEST(two_contexts_pass_control_back_and_forth) {
    CHECK(burrow__context_attach(&main_ctx));
    volleys_seen = 0;

    CHECK(burrow__context_make(&pong_ctx, pong_stack, sizeof pong_stack, pong, NULL,
                               &main_ctx));

    for (int i = 0; i < VOLLEYS; i++) {
        burrow__context_switch(&main_ctx, &pong_ctx);
        CHECK(volleys_seen == i + 1);
    }

    CHECK(volleys_seen == VOLLEYS);

    burrow__context_free(&pong_ctx);
    burrow__context_detach(&main_ctx);
}

/* -------------------------------------------------------------------- nesting */

/* A context switching to a third context rather than back to whoever resumed
 * it. Nothing here is a stack of callers, and proving that is the point: the
 * scheduler hands control sideways constantly. */

static burrow__Context outer_ctx;
static burrow__Context inner_ctx;
static _Alignas(16) unsigned char outer_stack[STACK_SIZE];
static _Alignas(16) unsigned char inner_stack[STACK_SIZE];
static int trail[8];
static int trail_len;

static void leave_a_mark(int mark) {
    if (trail_len < (int)(sizeof trail / sizeof trail[0]))
        trail[trail_len++] = mark;
}

static void inner_body(void *arg) {
    (void)arg;
    leave_a_mark(3);
    burrow__context_switch(&inner_ctx, &outer_ctx);
    leave_a_mark(5);
}

static void outer_body(void *arg) {
    (void)arg;
    leave_a_mark(2);
    burrow__context_switch(&outer_ctx, &inner_ctx);
    leave_a_mark(4);
    burrow__context_switch(&outer_ctx, &inner_ctx);
    leave_a_mark(6);
}

TEST(three_contexts_hand_control_around) {
    CHECK(burrow__context_attach(&main_ctx));
    trail_len = 0;

    CHECK(burrow__context_make(&outer_ctx, outer_stack, sizeof outer_stack, outer_body,
                               NULL, &main_ctx));
    CHECK(burrow__context_make(&inner_ctx, inner_stack, sizeof inner_stack, inner_body,
                               NULL, &main_ctx));

    leave_a_mark(1);
    burrow__context_switch(&main_ctx, &outer_ctx);

    /* inner runs off the end of inner_body on its second turn, so its link
     * brings control back here, which is why the trail stops at 5 rather than
     * reaching 6. */
    CHECK(trail_len == 5);
    CHECK(trail[0] == 1);
    CHECK(trail[1] == 2);
    CHECK(trail[2] == 3);
    CHECK(trail[3] == 4);
    CHECK(trail[4] == 5);

    burrow__context_free(&inner_ctx);
    burrow__context_free(&outer_ctx);
    burrow__context_detach(&main_ctx);
}

/* -------------------------------------------------------------- many at once */

#define CREW 16

static burrow__Context crew_ctx[CREW];
static _Alignas(16) unsigned char crew_stack[CREW][STACK_SIZE];
static int crew_turns[CREW];

static void crew_body(void *arg) {
    int me = *(int *)arg;
    for (int i = 0; i < 4; i++) {
        crew_turns[me]++;
        burrow__context_switch(&crew_ctx[me], &main_ctx);
    }
}

static int crew_ids[CREW];

TEST(a_round_robin_over_sixteen_contexts) {
    CHECK(burrow__context_attach(&main_ctx));

    for (int i = 0; i < CREW; i++) {
        crew_ids[i] = i;
        crew_turns[i] = 0;
        CHECK(burrow__context_make(&crew_ctx[i], crew_stack[i], STACK_SIZE, crew_body,
                                   &crew_ids[i], &main_ctx));
    }

    for (int round = 0; round < 4; round++)
        for (int i = 0; i < CREW; i++)
            burrow__context_switch(&main_ctx, &crew_ctx[i]);

    for (int i = 0; i < CREW; i++)
        CHECK(crew_turns[i] == 4);

    for (int i = 0; i < CREW; i++)
        burrow__context_free(&crew_ctx[i]);
    burrow__context_detach(&main_ctx);
}

/* ---------------------------------------------------------- what make refuses */

static burrow__Context reject_ctx;
static _Alignas(16) unsigned char reject_stack[STACK_SIZE];

static void never_runs(void *arg) {
    (void)arg;
}

TEST(make_refuses_what_it_cannot_honour) {
    /* Below the floor. A stack this small would not hold a signal frame on the
     * fallback paths, never mind anything the caller wants to do. */
    CHECK(!burrow__context_make(&reject_ctx, reject_stack, 128, never_runs, NULL,
                                &main_ctx));

    /* Nothing to run. */
    CHECK(!burrow__context_make(&reject_ctx, reject_stack, sizeof reject_stack, NULL,
                                NULL, &main_ctx));

    /* No stack. Windows does not use the buffer, and it is still required
     * there, so that this is one rule rather than two. */
    CHECK(!burrow__context_make(&reject_ctx, NULL, sizeof reject_stack, never_runs,
                                NULL, &main_ctx));

    /* A context that was never made is safe to free. */
    burrow__context_free(&reject_ctx);
}

int main(void) {
    RUN(a_context_runs_its_entry_and_returns_to_its_link);
    RUN(a_context_carries_on_where_it_stopped);
    RUN(everything_the_abi_promised_survives_a_switch);
    RUN(two_contexts_pass_control_back_and_forth);
    RUN(three_contexts_hand_control_around);
    RUN(a_round_robin_over_sixteen_contexts);
    RUN(make_refuses_what_it_cannot_honour);
    return harness_report("context");
}
