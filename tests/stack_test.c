/* Stacks, their guard pages, and what happens when something lands on one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Before every include, for the same reason src/runtime/stack.c does it. The
 * two fault tests leave a signal handler sideways, which needs sigsetjmp and
 * siglongjmp, and musl declares neither of them to a strict C11 build. glibc
 * happens to declare them anyway, which is exactly the kind of difference that
 * turns into somebody else's build failure. _DARWIN_C_SOURCE goes with it on
 * macOS, which otherwise reads a POSIX feature macro as a request for POSIX
 * and nothing else. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/stack.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/thread.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Whether a sanitizer is watching, which two tests below have to know.
 *
 * Address sanitizer and thread sanitizer put their own handler on SIGSEGV and
 * get there first, so a test of what burrow does with that signal is not
 * testing burrow. Address sanitizer also moves locals into a shadow frame of
 * its own so that it can catch a pointer to one outliving the call, which means
 * the address of a local stops being a way to find out where the stack pointer
 * is. Both of those are the sanitizer doing its job, so the tests that depend
 * on neither being true step aside rather than being worked around. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||             \
    __has_feature(memory_sanitizer)
#define SANITIZED 1
#endif
#endif

static size_t usable(const burrow__Stack *s) {
    return (size_t)((unsigned char *)s->hi - (unsigned char *)s->lo);
}

TEST(a_stack_is_at_least_the_size_that_was_asked_for) {
    /* Every one of these is a different case. 0 and 1 are below the minimum,
     * BURROW_STACK_MIN - 1 is just below it, 100000 is not a multiple of any
     * page size, and a megabyte is an ordinary request. */
    static const size_t asks[] = {
        0, 1, BURROW_STACK_MIN - 1, BURROW_STACK_MIN, 100000, 1024u * 1024u};

    size_t page = burrow__stack_page_size();
    CHECK(page >= 4096);
    CHECK((page & (page - 1)) == 0);

    for (size_t i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        burrow__Stack s;
        CHECK(burrow__stack_alloc(&s, asks[i]));
        if (s.lo == NULL)
            continue;

        size_t got = usable(&s);

        /* At least what was asked for, at least the minimum, and a whole
         * number of pages, since anything else would put lo or hi in the
         * middle of one. */
        CHECK(got >= asks[i]);
        CHECK(got >= BURROW_STACK_MIN);
        CHECK(got % page == 0);
        CHECK((uintptr_t)s.lo % page == 0);
        CHECK((uintptr_t)s.hi % page == 0);

        /* The guard is real and is not counted in what the caller can use. */
        CHECK(s.guard >= page);
        CHECK((unsigned char *)s.hi - (unsigned char *)s.lo == (ptrdiff_t)got);

        burrow__stack_free(&s);
    }
}

TEST(a_freed_stack_looks_like_one_that_was_never_allocated) {
    burrow__Stack s;
    CHECK(burrow__stack_alloc(&s, 64u * 1024u));
    burrow__stack_free(&s);

    CHECK(s.lo == NULL);
    CHECK(s.hi == NULL);
    CHECK(s.guard == 0);

    /* Which is what makes the second free do nothing rather than unmap
     * somebody else's memory. */
    burrow__stack_free(&s);
    CHECK(s.lo == NULL);

    burrow__Stack never;
    memset(&never, 0, sizeof never);
    burrow__stack_free(&never);
    CHECK(never.lo == NULL);

    /* A caller that passes nothing gets nothing, rather than a crash inside
     * the library. */
    burrow__stack_free(NULL);
    CHECK(!burrow__stack_alloc(NULL, 64u * 1024u));
}

TEST(every_byte_between_lo_and_hi_can_be_written_and_read_back) {
    burrow__Stack s;
    CHECK(burrow__stack_alloc(&s, 256u * 1024u));

    size_t size = usable(&s);
    unsigned char *p = (unsigned char *)s.lo;

    /* Both ends included, because an off by one page in the mapping would
     * leave exactly one of them unreadable. */
    for (size_t i = 0; i < size; i++)
        p[i] = (unsigned char)(i * 7u + 3u);

    size_t wrong = 0;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != (unsigned char)(i * 7u + 3u))
            wrong++;
    }
    CHECK_INT_EQ((Int)wrong, 0);

    burrow__stack_free(&s);
}

TEST(two_stacks_are_two_separate_pieces_of_memory) {
    burrow__Stack a;
    burrow__Stack b;
    CHECK(burrow__stack_alloc(&a, 64u * 1024u));
    CHECK(burrow__stack_alloc(&b, 64u * 1024u));

    /* Including the guards, since two mappings that share a guard page would
     * mean an overflow of one landing in the other. */
    unsigned char *a_base = (unsigned char *)a.lo - a.guard;
    unsigned char *b_base = (unsigned char *)b.lo - b.guard;
    CHECK(a_base >= (unsigned char *)b.hi || b_base >= (unsigned char *)a.hi);

    /* And writing to one does not show up in the other. */
    memset(a.lo, 0xAA, usable(&a));
    memset(b.lo, 0xBB, usable(&b));
    CHECK(((unsigned char *)a.lo)[0] == 0xAA);
    CHECK(((unsigned char *)a.hi)[-1] == 0xAA);
    CHECK(((unsigned char *)b.lo)[0] == 0xBB);
    CHECK(((unsigned char *)b.hi)[-1] == 0xBB);

    burrow__stack_free(&a);
    burrow__stack_free(&b);
}

/* ----------------------------------------------------------- current stack */

TEST(the_current_stack_starts_as_nothing_and_nests) {
    CHECK(burrow__stack_current() == NULL);

    burrow__Stack outer;
    burrow__Stack inner;
    memset(&outer, 0, sizeof outer);
    memset(&inner, 0, sizeof inner);

    burrow__Stack *was = burrow__stack_set_current(&outer);
    CHECK(was == NULL);
    CHECK(burrow__stack_current() == &outer);

    /* The setter answering with the previous value is what lets this be put
     * back without anybody keeping a copy of it. */
    burrow__Stack *saved = burrow__stack_set_current(&inner);
    CHECK(saved == &outer);
    CHECK(burrow__stack_current() == &inner);

    CHECK(burrow__stack_set_current(saved) == &inner);
    CHECK(burrow__stack_current() == &outer);

    CHECK(burrow__stack_set_current(NULL) == &outer);
    CHECK(burrow__stack_current() == NULL);
}

static burrow__Stack other_thread_saw_this;
static bool other_thread_saw_null;

static void look_at_current(void *arg) {
    (void)arg;
    other_thread_saw_null = burrow__stack_current() == NULL;
    (void)burrow__stack_set_current(&other_thread_saw_this);
}

TEST(the_current_stack_belongs_to_one_thread) {
    burrow__Stack mine;
    memset(&mine, 0, sizeof mine);
    (void)burrow__stack_set_current(&mine);

    static burrow__Thread t;
    other_thread_saw_null = false;
    CHECK(burrow__thread_start(&t, look_at_current, NULL, 0));
    CHECK(burrow__thread_join(&t));

    /* The new thread saw nothing rather than this thread's stack, and setting
     * its own did not touch this one. */
    CHECK(other_thread_saw_null);
    CHECK(burrow__stack_current() == &mine);

    (void)burrow__stack_set_current(NULL);
}

/* ------------------------------------------------------ running on a stack */

static burrow__Context ran_here;
static burrow__Context back_to;
static bool it_ran;
static bool sp_was_inside;

static void note_where_i_am(void *arg) {
    burrow__Stack *s = (burrow__Stack *)arg;
    unsigned char local = 0;

    it_ran = true;

    /* The address of a local is where the stack pointer is, near enough, and
     * it has to be on the stack that was handed over rather than the one the
     * thread started on. */
    sp_was_inside = (unsigned char *)&local >= (unsigned char *)s->lo &&
                    (unsigned char *)&local < (unsigned char *)s->hi;
    (void)local;
}

TEST(a_context_runs_on_a_stack_this_file_allocated) {
    burrow__Stack s;
    CHECK(burrow__stack_alloc(&s, 64u * 1024u));

    it_ran = false;
    sp_was_inside = false;

    CHECK(burrow__context_attach(&back_to));
    CHECK(burrow__context_make(&ran_here, s.lo, usable(&s), note_where_i_am, &s,
                               &back_to));
    burrow__context_switch(&back_to, &ran_here);

    CHECK(it_ran);
#if !defined(BURROW_CONTEXT_FIBERS) && !defined(SANITIZED)
    /* Not on Windows, where a fiber brings its own stack and the one above is
     * deliberately not the memory it runs on. burrow/context.h says so. And not
     * under a sanitizer, for the reason at the top of this file. */
    CHECK(sp_was_inside);
#endif

    burrow__context_free(&ran_here);
    burrow__context_detach(&back_to);
    burrow__stack_free(&s);
}

/* -------------------------------------------------------------- the report */

#if !defined(SANITIZED)

#if defined(BURROW_OS_WINDOWS)
#include <setjmp.h>
#define ESCAPE_BUF jmp_buf
#define ESCAPE_SET(b) setjmp(b)
#define ESCAPE_GO(b) longjmp(b, 1)
#else
#include <setjmp.h>
#define ESCAPE_BUF sigjmp_buf
/* The 1 is what saves and restores the signal mask, and it is the difference
 * between this working once and working twice: a handler runs with its own
 * signal blocked, so leaving sideways without putting the mask back means the
 * next fault of the same kind is never delivered. */
#define ESCAPE_SET(b) sigsetjmp(b, 1)
#define ESCAPE_GO(b) siglongjmp(b, 1)
#endif

static ESCAPE_BUF escape;
static char reported[128];
static bool did_report;

static void catch_it(Str msg) {
    size_t n = (size_t)(msg.len < (Int)sizeof reported - 1 ? msg.len
                                                           : (Int)sizeof reported - 1);
    if (msg.p != NULL && n > 0)
        memcpy(reported, msg.p, n);
    reported[n] = '\0';
    did_report = true;
    ESCAPE_GO(escape);
}

/* Each of these runs on a thread of its own rather than on the main one.
 *
 * Leaving a signal handler by longjmp instead of by returning means the kernel
 * never gets its sigreturn, and on some systems that leaves the thread marked
 * as still being on its signal stack. The next fault on that thread would then
 * be delivered on the ordinary stack, which for an overflow test is the one
 * with no room left on it. A fresh thread has none of that history, so each
 * test gets one and the question never comes up. */
static bool touching_the_guard_said_so;

static void touch_the_guard(void *arg) {
    (void)arg;

    if (!burrow__stack_guard_arm_thread())
        return;

    burrow__Stack s;
    if (!burrow__stack_alloc(&s, 64u * 1024u))
        return;

    burrow__Stack *was = burrow__stack_set_current(&s);
    runtime_set_fatal_handler(catch_it);

    did_report = false;
    memset(reported, 0, sizeof reported);

    if (ESCAPE_SET(escape) == 0) {
        /* One byte below lo, which is the top of the guard. Nothing is running
         * on this stack, so this is the guard being a guard rather than an
         * overflow, and it is the same fault by the same route. */
        volatile unsigned char *just_below = (unsigned char *)s.lo - 1;
        *just_below = 1;
    }

    runtime_set_fatal_handler(NULL);
    touching_the_guard_said_so = did_report && strcmp(reported, "stack overflow") == 0;

    (void)burrow__stack_set_current(was);
    burrow__stack_free(&s);
    burrow__stack_guard_disarm_thread();
}

TEST(a_write_into_the_guard_is_a_stack_overflow) {
    CHECK(burrow__stack_guard_arm());

    static burrow__Thread t;
    touching_the_guard_said_so = false;
    CHECK(burrow__thread_start(&t, touch_the_guard, NULL, 0));
    CHECK(burrow__thread_join(&t));
    CHECK(touching_the_guard_said_so);
}

/* Everything below is left out where a context is a fiber, which today means
 * Windows.
 *
 * A fiber brings its own stack and the mapping allocated here is deliberately
 * not the memory it runs on, which burrow/context.h says and the test above
 * already works around. So a recursion inside a fiber runs off the end of the
 * fiber's stack and hits the operating system's guard page rather than this
 * library's, and the handler correctly decides the fault is none of its
 * business and passes it on. There is nothing to test until Win64 assembly puts
 * a goroutine on a stack from this file, and the guard test above already
 * covers the vectored handler itself.
 *
 * It is also the second caveat next to burrow__stack_guard_arm happening for
 * real: a vectored handler runs on the stack that faulted. */
#if !defined(BURROW_CONTEXT_FIBERS)

/* A frame small enough that it cannot step over a guard page, since the
 * smallest page anywhere here is 4096. This is the thing the header warns
 * about, from the other side: a frame larger than the guard would miss it. */
static volatile size_t keep_going = 1;

/* Far past the roughly 256 frames a 64 kilobyte stack has room for, and there
 * so that a build where this does not fault fails the test instead of spinning
 * until somebody kills it. That is not a theoretical worry: it is what gcc -O2
 * on arm64 did to the first version of this function. */
#define TOO_DEEP ((size_t)1000000)

static BURROW_NOINLINE size_t eat_stack(size_t depth) {
    volatile unsigned char pad[256];
    pad[0] = (unsigned char)depth;
    pad[sizeof pad - 1] = (unsigned char)depth;

    /* keep_going is never 0, and the compiler cannot know that because the read
     * is volatile. Without it this is infinite recursion in plain sight and
     * -Winfinite-recursion says so, which is the warning doing exactly what it
     * is for everywhere except here. */
    if (keep_going == 0 || depth > TOO_DEEP)
        return depth;

    /* Reading pad after the call is what keeps this recursion on the stack. An
     * addition before it is not enough: gcc turns `something + f(n + 1)` into a
     * loop with an accumulator and no frames in it, which then never reaches
     * the guard and never comes back. A volatile read of a local after the call
     * cannot move to before it, so the frame has to still be there, so there
     * has to be a frame. */
    size_t below = eat_stack(depth + 1);
    return below + (size_t)pad[sizeof pad - 1];
}

static burrow__Context deep;
static burrow__Context shallow;

static void go_too_deep(void *arg) {
    (void)arg;
    (void)eat_stack(0);
}

static bool overflowing_said_so;

static void run_off_the_bottom(void *arg) {
    (void)arg;

    if (!burrow__stack_guard_arm_thread())
        return;

    burrow__Stack s;
    if (!burrow__stack_alloc(&s, 64u * 1024u))
        return;

    if (!burrow__context_attach(&shallow)) {
        burrow__stack_free(&s);
        return;
    }
    if (!burrow__context_make(&deep, s.lo, usable(&s), go_too_deep, NULL, &shallow)) {
        burrow__context_detach(&shallow);
        burrow__stack_free(&s);
        return;
    }

    burrow__Stack *was = burrow__stack_set_current(&s);
    runtime_set_fatal_handler(catch_it);

    did_report = false;
    memset(reported, 0, sizeof reported);

    if (ESCAPE_SET(escape) == 0)
        burrow__context_switch(&shallow, &deep);

    runtime_set_fatal_handler(NULL);
    overflowing_said_so = did_report && strcmp(reported, "stack overflow") == 0;

    (void)burrow__stack_set_current(was);

    /* The context is abandoned partway down a recursion that will never come
     * back. Nothing on that stack has to be unwound, because the stack is about
     * to stop existing, and that is the whole reason a goroutine stack is a
     * mapping of its own. */
    burrow__context_free(&deep);
    burrow__context_detach(&shallow);
    burrow__stack_free(&s);
    burrow__stack_guard_disarm_thread();
}

TEST(running_off_the_bottom_of_a_stack_is_a_stack_overflow) {
    CHECK(burrow__stack_guard_arm());

    static burrow__Thread t;
    overflowing_said_so = false;
    CHECK(burrow__thread_start(&t, run_off_the_bottom, NULL, 0));
    CHECK(burrow__thread_join(&t));
    CHECK(overflowing_said_so);
}

#endif /* !BURROW_CONTEXT_FIBERS */

#endif /* !SANITIZED */

int main(void) {
    RUN(a_stack_is_at_least_the_size_that_was_asked_for);
    RUN(a_freed_stack_looks_like_one_that_was_never_allocated);
    RUN(every_byte_between_lo_and_hi_can_be_written_and_read_back);
    RUN(two_stacks_are_two_separate_pieces_of_memory);
    RUN(the_current_stack_starts_as_nothing_and_nests);
    RUN(the_current_stack_belongs_to_one_thread);
    RUN(a_context_runs_on_a_stack_this_file_allocated);
#if !defined(SANITIZED)
    RUN(a_write_into_the_guard_is_a_stack_overflow);
#if !defined(BURROW_CONTEXT_FIBERS)
    RUN(running_off_the_bottom_of_a_stack_is_a_stack_overflow);
#endif
#endif
    return harness_report("stack");
}
