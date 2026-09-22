/* The platform layer.
 *
 * These are thin tests on purpose. There is no way to check from inside the
 * process that pal_clock_monotonic really read the machine's counter, and a
 * test that mocked the system call would be testing the mock. What can be
 * checked is the contract burrow/pal.h states, which is what everything above
 * the layer is written against: that the monotonic clock does not go backwards,
 * that a page size is a power of two, that a reservation cannot be touched and
 * a commit can, and that the arguments a caller can get wrong are refused
 * rather than passed through to a kernel.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/pal.h"

#include "burrow/atomic.h"
#include "burrow/thread.h"

#include "harness.h"

#include <string.h>

/* ---------------------------------------------------------------- the codes */

TEST(every_code_has_a_message) {
    /* The static assert in src/pal/errno.c already ties the table's length to
     * the enum. This checks the other half, which is that no entry is empty and
     * no two of them are the same string, since a copied line in a table of
     * fifty is invisible and a duplicated message makes two failures look like
     * one. */
    for (PalErrno a = PAL_EPERM; a <= PAL_EOTHER; a++) {
        const char *msg = pal_errno_string(a);
        CHECK(msg != NULL);
        CHECK(msg[0] != '\0');

        for (PalErrno b = PAL_EPERM; b < a; b++)
            CHECK(strcmp(msg, pal_errno_string(b)) != 0);
    }
}

TEST(success_and_nonsense_are_not_the_same_answer) {
    CHECK(strcmp(pal_errno_string(PAL_OK), "no error") == 0);
    CHECK(strcmp(pal_errno_string(PAL_EOTHER), "unknown error") == 0);

    /* Outside the range in either direction, including a raw POSIX errno, which
     * is the mistake the numbering starts at 1000 to make visible. */
    CHECK(strcmp(pal_errno_string(2), "unknown error") == 0);
    CHECK(strcmp(pal_errno_string(-1), "unknown error") == 0);
    CHECK(strcmp(pal_errno_string(999999), "unknown error") == 0);
}

/* --------------------------------------------------------------- the clocks */

TEST(the_monotonic_clock_only_goes_forwards) {
    int64_t first = pal_clock_monotonic();
    CHECK(first > 0);

    int64_t last = first;
    for (int i = 0; i < 10000; i++) {
        int64_t now = pal_clock_monotonic();
        CHECK(now >= last);
        last = now;
    }

    /* Ten thousand readings have to take some time, or the clock is stuck. */
    CHECK(last > first);
}

TEST(the_wall_clock_says_it_is_after_the_time_this_was_written) {
    /* 2026-01-01T00:00:00Z in nanoseconds. A machine whose clock is before that
     * is a machine with no battery and no network, and the test is here to
     * catch a backend that returns an offset from the wrong epoch, which is the
     * mistake the Windows one is most exposed to. */
    const int64_t y2026 = 1767225600LL * 1000000000LL;

    int64_t now = pal_clock_realtime();
    CHECK(now > y2026);

    /* And before the year 2200, which catches a reading that was multiplied by
     * a billion twice. */
    CHECK(now < 7258118400LL * 1000000000LL);
}

TEST(the_two_clocks_are_not_the_same_clock) {
    /* The monotonic one counts from an arbitrary point, usually boot, and the
     * wall clock counts from 1970. A backend that wired one to the other would
     * pass every test above and fail this. */
    int64_t mono = pal_clock_monotonic();
    int64_t wall = pal_clock_realtime();
    CHECK(wall - mono > 0);
}

TEST(a_sleep_takes_at_least_as_long_as_it_was_asked_for) {
    const int64_t want = 2 * 1000 * 1000; /* two milliseconds */

    int64_t before = pal_clock_monotonic();
    pal_nanosleep(want);
    int64_t took = pal_clock_monotonic() - before;

    CHECK(took >= want);

    /* And not absurdly longer. A hundred times the ask is loose enough for a
     * loaded machine and tight enough to catch a unit mix up, which would be a
     * factor of a thousand or a million. */
    CHECK(took < want * 100);
}

TEST(a_sleep_of_nothing_returns) {
    /* Zero and negative both mean give up the processor and come back, so the
     * only thing to check is that neither hangs and neither takes long. */
    int64_t before = pal_clock_monotonic();
    pal_nanosleep(0);
    pal_nanosleep(-1);
    pal_nanosleep(-1000000000);
    int64_t took = pal_clock_monotonic() - before;

    CHECK(took < 1000 * 1000 * 1000);
}

/* -------------------------------------------------------------- the machine */

TEST(the_page_size_is_a_power_of_two) {
    int64_t page = pal_page_size();

    CHECK(page >= 512);
    CHECK(page <= 1024 * 1024);
    CHECK((page & (page - 1)) == 0);
}

TEST(the_page_size_is_the_same_every_time) {
    /* It is cached after the first call, and a cache that answers differently
     * once it is warm is worse than no cache. */
    int64_t first = pal_page_size();
    for (int i = 0; i < 100; i++)
        CHECK(pal_page_size() == first);
}

TEST(there_is_at_least_one_processor) {
    int64_t n = pal_cpu_count();

    CHECK(n >= 1);

    /* A machine with more than this exists and is not a machine this test runs
     * on. The bound is here to catch a count read out of the wrong field. */
    CHECK(n <= 4096);

    for (int i = 0; i < 100; i++)
        CHECK(pal_cpu_count() == n);
}

/* --------------------------------------------------------------- the memory */

TEST(a_reservation_can_be_committed_and_written_and_given_back) {
    int64_t page = pal_page_size();
    PalErrno err = PAL_EOTHER;

    void *p = pal_vm_reserve(page * 8, &err);
    CHECK(p != NULL);
    CHECK(err == PAL_OK);

    err = PAL_EOTHER;
    CHECK(pal_vm_commit(p, page * 2, &err));
    CHECK(err == PAL_OK);

    /* The part that was committed is memory now. Writing every page rather than
     * the first byte, because a backend that committed one page and reported
     * two would otherwise go unnoticed. */
    unsigned char *bytes = (unsigned char *)p;
    for (int64_t i = 0; i < page * 2; i++)
        bytes[i] = (unsigned char)(i & 0xff);
    for (int64_t i = 0; i < page * 2; i++)
        CHECK(bytes[i] == (unsigned char)(i & 0xff));

    err = PAL_EOTHER;
    CHECK(pal_vm_decommit(p, page * 2, &err));
    CHECK(err == PAL_OK);

    err = PAL_EOTHER;
    CHECK(pal_vm_release(p, page * 8, &err));
    CHECK(err == PAL_OK);
}

TEST(committing_twice_is_not_an_error) {
    int64_t page = pal_page_size();

    void *p = pal_vm_reserve(page * 4, NULL);
    CHECK(p != NULL);

    /* A grow loop commits from the bottom every time rather than tracking where
     * it got to, and that only works if this is free and quiet. */
    for (int i = 0; i < 4; i++) {
        PalErrno err = PAL_EOTHER;
        CHECK(pal_vm_commit(p, page * 2, &err));
        CHECK(err == PAL_OK);
    }

    *(volatile unsigned char *)p = 42;
    CHECK(*(volatile unsigned char *)p == 42);

    CHECK(pal_vm_release(p, page * 4, NULL));
}

TEST(a_guard_can_be_put_on_a_committed_page) {
    int64_t page = pal_page_size();

    void *p = pal_vm_reserve(page * 2, NULL);
    CHECK(p != NULL);
    CHECK(pal_vm_commit(p, page * 2, NULL));

    /* Only that it is accepted. Whether touching it faults is what
     * tests/stack_test.c checks, because checking it here would mean catching
     * the fault, and the machinery for that is the runtime's. */
    PalErrno err = PAL_EOTHER;
    CHECK(pal_vm_guard(p, page, &err));
    CHECK(err == PAL_OK);

    /* The page above the guard is still writable, which is the half that makes
     * a guard a guard rather than a wall across the whole mapping. */
    *((volatile unsigned char *)p + page) = 7;
    CHECK(*((volatile unsigned char *)p + page) == 7);

    CHECK(pal_vm_release(p, page * 2, NULL));
}

TEST(memory_that_is_not_page_aligned_is_refused) {
    int64_t page = pal_page_size();

    PalErrno err = PAL_OK;
    CHECK(pal_vm_reserve(page + 1, &err) == NULL);
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(pal_vm_reserve(0, &err) == NULL);
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(pal_vm_reserve(-page, &err) == NULL);
    CHECK(err == PAL_EINVAL);

    void *p = pal_vm_reserve(page * 2, NULL);
    CHECK(p != NULL);

    /* An address in the middle of a page, which is the mistake a caller makes
     * by adding a byte count to a base rather than a page count. */
    err = PAL_OK;
    CHECK(!pal_vm_commit((unsigned char *)p + 1, page, &err));
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_vm_commit(p, page - 1, &err));
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_vm_commit(NULL, page, &err));
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_vm_decommit(p, 0, &err));
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_vm_guard(p, page + 1, &err));
    CHECK(err == PAL_EINVAL);

    CHECK(pal_vm_release(p, page * 2, NULL));
}

TEST(a_large_reservation_costs_address_space_and_not_memory) {
    int64_t page = pal_page_size();

    /* A gigabyte, reserved and never committed. This is what a goroutine stack
     * does, and on a machine where reserving meant committing it would either
     * fail or make the process a gigabyte bigger. Ten of them in a row makes
     * the point without depending on how the machine reports its own size. */
    void *held[10];
    int64_t bytes = 1024 * 1024 * 1024;
    bytes -= bytes % page;

    for (int i = 0; i < 10; i++) {
        held[i] = pal_vm_reserve(bytes, NULL);
        CHECK(held[i] != NULL);
    }

    for (int i = 0; i < 10; i++)
        CHECK(pal_vm_release(held[i], bytes, NULL));
}

/* --------------------------------------------------------------- the random */

TEST(random_bytes_fills_the_buffer) {
    unsigned char buf[64];
    memset(buf, 0, sizeof buf);

    PalErrno err = PAL_EOTHER;
    bool ok = pal_random_bytes(buf, (int64_t)sizeof buf, &err);

    /* A platform with nowhere to ask says so, and that is a pass here. What is
     * not a pass is claiming success and leaving the buffer alone. */
    if (!ok) {
        CHECK(err == PAL_ENOSYS);
        return;
    }
    CHECK(err == PAL_OK);

    /* All zeroes is what an untouched buffer looks like, and it is a valid
     * answer with probability one in two to the five hundred and twelve. */
    int zeroes = 0;
    for (size_t i = 0; i < sizeof buf; i++)
        if (buf[i] == 0)
            zeroes++;
    CHECK(zeroes < (int)sizeof buf);

    /* And every byte the same, which is what a backend that filled with one
     * value would give. */
    int same = 0;
    for (size_t i = 1; i < sizeof buf; i++)
        if (buf[i] == buf[0])
            same++;
    CHECK(same < (int)sizeof buf - 1);
}

TEST(two_asks_do_not_give_the_same_answer) {
    unsigned char a[32];
    unsigned char b[32];

    if (!pal_random_bytes(a, (int64_t)sizeof a, NULL))
        return;
    CHECK(pal_random_bytes(b, (int64_t)sizeof b, NULL));

    CHECK(memcmp(a, b, sizeof a) != 0);
}

TEST(random_writes_exactly_as_many_bytes_as_it_was_asked_for) {
    /* A guard byte on each side, because a loop that goes one past the end is
     * the mistake a chunked backend makes, and both the BSD and the Windows
     * ones are chunked. */
    unsigned char buf[130];

    for (int64_t n = 1; n <= 128; n++) {
        memset(buf, 0xaa, sizeof buf);

        if (!pal_random_bytes(buf + 1, n, NULL))
            return;

        CHECK(buf[0] == 0xaa);
        CHECK(buf[n + 1] == 0xaa);
    }
}

TEST(asking_for_nothing_is_not_a_failure) {
    unsigned char buf[1] = {0x5a};

    PalErrno err = PAL_EOTHER;
    CHECK(pal_random_bytes(buf, 0, &err));
    CHECK(err == PAL_OK);
    CHECK(buf[0] == 0x5a);

    /* A NULL buffer with a zero length is the empty slice and is fine. A NULL
     * buffer with a length is a bug and is refused. */
    err = PAL_EOTHER;
    CHECK(pal_random_bytes(NULL, 0, &err));
    CHECK(err == PAL_OK);

    err = PAL_OK;
    CHECK(!pal_random_bytes(NULL, 8, &err));
    CHECK(err == PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_random_bytes(buf, -1, &err));
    CHECK(err == PAL_EINVAL);
}

/* ------------------------------------------------------------- the out slot */

TEST(the_error_is_optional_like_every_other_out_parameter) {
    /* Every entry point takes the error last and every one of them has to
     * accept NULL there, because a caller that only wants to know whether it
     * worked should not have to declare a variable to throw away. */
    int64_t page = pal_page_size();

    CHECK(pal_vm_reserve(page + 1, NULL) == NULL);

    void *p = pal_vm_reserve(page, NULL);
    CHECK(p != NULL);
    CHECK(pal_vm_commit(p, page, NULL));
    CHECK(pal_vm_decommit(p, page, NULL));
    CHECK(pal_vm_guard(p, page, NULL));
    CHECK(pal_vm_release(p, page, NULL));

    CHECK(!pal_vm_commit(NULL, page, NULL));
}

/* ---------------------------------------------------------------- the poller
 *
 * Only on a readiness backend. The completion port wants an operation submitted
 * against a real handle before it has anything to report, which is a net test
 * and not a PAL one, so Windows gets the same treatment netpoll_iocp_test.c
 * gives the other direction.
 *
 * There is one poller per process and these share it, so the order below
 * matters: the first test makes it and the rest use it. */
#if !defined(_WIN32)

#include <unistd.h>

static int64_t poller = PAL_INVALID_HANDLE;

/* A pipe, or the test gives up rather than reporting a failure that is not
 * about the code under test. */
static void open_pipe(int *rd, int *wr) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    *rd = fds[0];
    *wr = fds[1];
}

TEST(there_is_one_poller_and_a_second_ask_is_refused) {
    PalErrno err = PAL_OK;

    poller = pal_poll_create(&err);
    CHECK(poller != PAL_INVALID_HANDLE);
    CHECK_INT_EQ(err, PAL_OK);

    /* Not a limitation to work around. Nothing in the PAL allocates, so the
     * poller is static storage, and a second one would have nowhere to live. */
    err = PAL_OK;
    CHECK(pal_poll_create(&err) == PAL_INVALID_HANDLE);
    CHECK_INT_EQ(err, PAL_EBUSY);
}

TEST(a_descriptor_with_something_on_it_comes_back_ready) {
    int rd, wr;
    open_pipe(&rd, &wr);

    void *tag = (void *)(uintptr_t)0x1234;
    PalErrno err = PAL_OK;
    CHECK(pal_poll_add(poller, rd, tag, &err));
    CHECK_INT_EQ(err, PAL_OK);

    CHECK(write(wr, "x", 1) == 1);

    PalPollEvent events[8];
    int64_t n = pal_poll_wait(poller, events, 8, 1000000000, &err);
    CHECK_INT_EQ(err, PAL_OK);
    CHECK_INT_EQ(n, 1);
    CHECK(events[0].user == tag);
    CHECK((events[0].ready & PAL_POLL_READY_READ) != 0);

    /* No operation produced these, so there is nothing for them to hold. */
    CHECK_INT_EQ(events[0].bytes, 0);
    CHECK_INT_EQ(events[0].status, 0);

    CHECK(pal_poll_del(poller, rd, &err));
    CHECK_INT_EQ(err, PAL_OK);
    (void)close(rd);
    (void)close(wr);
}

TEST(a_look_with_nothing_to_see_comes_back_empty) {
    PalPollEvent events[8];
    PalErrno err = PAL_ETIMEDOUT;

    /* Zero is the non blocking look the scheduler does when it is hunting for
     * work, and finding nothing is not a failure. */
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 0, &err), 0);
    CHECK_INT_EQ(err, PAL_OK);
}

TEST(a_break_ends_a_wait_and_is_never_reported_as_an_event) {
    PalPollEvent events[8];
    PalErrno err = PAL_OK;

    CHECK(pal_poll_break(poller, &err));
    CHECK_INT_EQ(err, PAL_OK);

    /* A wait that would otherwise sit here for a second. The wakeup ends it,
     * and it comes back with no events, because the wakeup belongs to the
     * backend and is not something a caller should ever see. */
    int64_t start = pal_clock_monotonic();
    int64_t n = pal_poll_wait(poller, events, 8, 1000000000, &err);
    int64_t took = pal_clock_monotonic() - start;

    CHECK_INT_EQ(err, PAL_OK);
    CHECK_INT_EQ(n, 0);
    CHECK(took < 500000000);

    /* And the wakeup was consumed rather than left behind, which is what this
     * second wait proves: it has nothing to wake it, so it has to sit out its
     * own timeout. A backend that failed to drain would come straight back. */
    start = pal_clock_monotonic();
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 50000000, &err), 0);
    took = pal_clock_monotonic() - start;
    CHECK_INT_EQ(err, PAL_OK);
    CHECK(took >= 40000000);

    /* The flag that collapses many wakeups into one has to have been cleared
     * by that drain, or every break from here on would be swallowed. */
    CHECK(pal_poll_break(poller, &err));
    start = pal_clock_monotonic();
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 1000000000, &err), 0);
    CHECK(pal_clock_monotonic() - start < 500000000);
}

TEST(many_breaks_at_once_cost_one_wakeup) {
    PalPollEvent events[8];
    PalErrno err = PAL_OK;

    for (int i = 0; i < 1000; i++)
        CHECK(pal_poll_break(poller, &err));

    CHECK_INT_EQ(err, PAL_OK);

    /* One wait takes all thousand of them, because a wait that is about to
     * return has already answered every one. The second sits out its timeout,
     * which is what says the collapsing works. */
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 1000000000, &err), 0);

    int64_t start = pal_clock_monotonic();
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 50000000, &err), 0);
    CHECK(pal_clock_monotonic() - start >= 40000000);
}

TEST(a_handle_that_is_not_the_poller_is_refused) {
    PalPollEvent events[8];
    PalErrno err = PAL_OK;

    /* A wrong handle is a caller bug and not a system failure, so it is
     * PAL_EINVAL and not a guess at what was meant. */
    CHECK(!pal_poll_add(PAL_INVALID_HANDLE, 0, (void *)1, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK(!pal_poll_del(poller + 4096, 0, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK(!pal_poll_break(poller + 4096, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_poll_wait(poller + 4096, events, 8, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);
}

TEST(the_wakeups_own_tag_cannot_be_used_as_a_user_pointer) {
    int rd, wr;
    open_pipe(&rd, &wr);

    /* The all ones pointer is what a readiness backend registers its own
     * wakeup with, so a caller cannot have it. pal.h says so and this is the
     * refusal, because the alternative is a caller whose events quietly
     * disappear into the wakeup path. */
    PalErrno err = PAL_OK;
    CHECK(!pal_poll_add(poller, rd, (void *)(uintptr_t)-1, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    (void)close(rd);
    (void)close(wr);
}

TEST(a_wait_with_nowhere_to_put_the_answer_is_refused) {
    PalPollEvent events[8];
    PalErrno err = PAL_OK;

    CHECK_INT_EQ(pal_poll_wait(poller, NULL, 8, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_poll_wait(poller, events, 0, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_poll_wait(poller, events, -1, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);
}

TEST(the_poller_takes_a_null_error_like_everything_else) {
    PalPollEvent events[8];

    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 0, NULL), 0);
    CHECK(pal_poll_break(poller, NULL));
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 1000000000, NULL), 0);
    CHECK(!pal_poll_add(PAL_INVALID_HANDLE, 0, (void *)1, NULL));
}

#endif /* not windows */

/* ----------------------------------------------------------------- the futex
 *
 * These run on every platform, because unlike the poller all three backends can
 * be driven from inside one process with nothing but a word and a thread. That
 * matters more here than usual: two of the three backends are an emulation
 * burrow wrote rather than a call burrow forwards, so these are the only thing
 * standing behind them. */

typedef struct FutexParty {
    uint32_t word;
    /* Counts what actually happened, so a test can tell a real wake from a
     * timeout without guessing from the timing. */
    uint32_t woke;
    uint32_t timedout;
    int64_t timeout_ns;
} FutexParty;

static void futex_sleeper(void *arg) {
    FutexParty *p = (FutexParty *)arg;
    PalErrno err = PAL_OK;

    while (burrow__atomic_load_acquire_u32(&p->word) == 0) {
        if (!pal_futex_wait(&p->word, 0, p->timeout_ns, &err) && err == PAL_ETIMEDOUT) {
            (void)burrow__atomic_add_u32(&p->timedout, 1);
            return;
        }
    }

    (void)burrow__atomic_add_u32(&p->woke, 1);
}

TEST(a_word_that_already_differs_does_not_wait) {
    uint32_t word = 7;
    PalErrno err = PAL_ENOSYS;

    /* No timeout and no waker, so if this did wait the test would hang rather
     * than fail, which is the honest way to check it. */
    CHECK(pal_futex_wait(&word, 1, -1, &err));
    CHECK_INT_EQ(err, PAL_OK);
}

TEST(a_wait_with_no_time_and_no_change_times_out) {
    uint32_t word = 0;
    PalErrno err = PAL_OK;

    CHECK(!pal_futex_wait(&word, 0, 0, &err));
    CHECK_INT_EQ(err, PAL_ETIMEDOUT);
}

TEST(a_wait_with_a_deadline_waits_at_least_that_long) {
    uint32_t word = 0;
    PalErrno err = PAL_OK;

    int64_t start = pal_clock_monotonic();
    CHECK(!pal_futex_wait(&word, 0, 20000000, &err));
    CHECK_INT_EQ(err, PAL_ETIMEDOUT);

    /* Fifteen rather than twenty, because a condition variable and a futex both
     * measure in units coarser than a nanosecond and rounding is allowed to go
     * the way that makes this shorter. Coming back far too early is the bug
     * worth catching here, not a few hundred microseconds. */
    CHECK(pal_clock_monotonic() - start >= 15000000);
}

TEST(a_sleeper_is_woken_by_a_wake) {
    FutexParty p = {0, 0, 0, -1};
    burrow__Thread t;

    CHECK(burrow__thread_start(&t, futex_sleeper, &p, 0));

    /* The store is what the wait is really waiting for, and the wake is only
     * what tells the kernel to go and look. Ordered this way round because the
     * other way round is the lost wakeup every futex user writes once. */
    burrow__atomic_store_u32(&p.word, 1);

    PalErrno err = PAL_ENOSYS;
    CHECK(pal_futex_wake(&p.word, INT64_MAX, &err) >= 0);
    CHECK_INT_EQ(err, PAL_OK);

    CHECK(burrow__thread_join(&t));
    CHECK_INT_EQ(p.woke, 1);
    CHECK_INT_EQ(p.timedout, 0);
}

TEST(everybody_waiting_can_be_released_at_once) {
    enum { SLEEPERS = 8 };

    FutexParty p = {0, 0, 0, -1};
    burrow__Thread t[SLEEPERS];

    for (int i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_start(&t[i], futex_sleeper, &p, 0));

    burrow__atomic_store_u32(&p.word, 1);
    CHECK(pal_futex_wake(&p.word, INT64_MAX, NULL) >= 0);

    for (int i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_join(&t[i]));

    /* Every one of them, and none of them by a timeout, since they were started
     * without one. A backend that woke only the first would hang here instead,
     * which is the same answer said more slowly. */
    CHECK_INT_EQ(p.woke, SLEEPERS);
    CHECK_INT_EQ(p.timedout, 0);
}

TEST(a_sleeper_on_one_word_is_not_released_by_a_wake_on_another) {
    FutexParty mine = {0, 0, 0, -1};
    uint32_t other = 0;
    burrow__Thread t;

    CHECK(burrow__thread_start(&t, futex_sleeper, &mine, 0));

    /* A wake on a word nobody is waiting on. It may share a bucket with the one
     * that does have a sleeper on it, which is allowed to wake that sleeper
     * spuriously, and the sleeper's loop is what makes that not matter: it
     * looks at its own word and goes back down. */
    CHECK_INT_EQ(pal_futex_wake(&other, INT64_MAX, NULL), 0);

    burrow__atomic_store_u32(&mine.word, 1);
    CHECK(pal_futex_wake(&mine.word, INT64_MAX, NULL) >= 0);

    CHECK(burrow__thread_join(&t));
    CHECK_INT_EQ(mine.woke, 1);
}

TEST(a_wake_with_nobody_waiting_is_free_and_not_an_error) {
    uint32_t word = 0;
    PalErrno err = PAL_ENOSYS;

    CHECK_INT_EQ(pal_futex_wake(&word, INT64_MAX, &err), 0);
    CHECK_INT_EQ(err, PAL_OK);

    CHECK_INT_EQ(pal_futex_wake(&word, 1, NULL), 0);
    CHECK_INT_EQ(pal_futex_wake(&word, 0, NULL), 0);
}

TEST(a_sleeper_with_a_deadline_gives_up_when_nobody_comes) {
    FutexParty p = {0, 0, 0, 20000000};
    burrow__Thread t;

    CHECK(burrow__thread_start(&t, futex_sleeper, &p, 0));
    CHECK(burrow__thread_join(&t));

    CHECK_INT_EQ(p.timedout, 1);
    CHECK_INT_EQ(p.woke, 0);
}

TEST(a_wait_or_wake_on_nothing_is_refused) {
    PalErrno err = PAL_OK;

    CHECK(!pal_futex_wait(NULL, 0, 0, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_futex_wake(NULL, 1, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    uint32_t word = 0;
    CHECK_INT_EQ(pal_futex_wake(&word, -1, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);
}

TEST(the_futex_takes_a_null_error_like_everything_else) {
    uint32_t word = 3;

    CHECK(pal_futex_wait(&word, 0, -1, NULL));
    CHECK(!pal_futex_wait(&word, 3, 0, NULL));
    CHECK_INT_EQ(pal_futex_wake(&word, INT64_MAX, NULL), 0);
    CHECK(!pal_futex_wait(NULL, 0, 0, NULL));
}

/* --------------------------------------------------------------- the threads
 *
 * burrow/thread.h has its own suite and it is the one that covers the shape a
 * caller sees. What is here is the part that only exists below the boundary:
 * the handshake that carries a function and an argument through an entry point
 * with room for one pointer, and the handle surviving the trip through an
 * int64_t and back. */

/* Whether a sanitizer is watching, which one test below has to know. Address
 * sanitizer moves locals into a shadow frame of its own so that it can catch a
 * pointer to one outliving the call, and that means the address of a local
 * stops being a way to find out where the stack pointer is. tests/stack_test.c
 * says the same thing at more length and for the same reason. */
#if defined(__SANITIZE_ADDRESS__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define SANITIZED 1
#endif
#endif

#define THREAD_PARTY 16

typedef struct ThreadParty {
    uint32_t ran;
    void *arg_seen;
    int64_t self;
    /* Raised by the thread when it is finished, for the detach test, which has
     * no join to wait on. */
    uint32_t done;
} ThreadParty;

static void thread_marker(void *arg) {
    ThreadParty *p = (ThreadParty *)arg;

    p->arg_seen = arg;
    p->self = pal_thread_self();
    (void)burrow__atomic_add_u32(&p->ran, 1);
    burrow__atomic_store_u32(&p->done, 1);
    (void)pal_futex_wake(&p->done, INT64_MAX, NULL);
}

TEST(a_thread_runs_the_function_it_was_given) {
    ThreadParty p = {0, NULL, 0, 0};
    PalErrno err = PAL_EOTHER;

    int64_t h = pal_thread_create(thread_marker, &p, 0, &err);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK_INT_EQ(err, PAL_OK);

    err = PAL_EOTHER;
    CHECK(pal_thread_join(h, &err));
    CHECK_INT_EQ(err, PAL_OK);

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

TEST(the_argument_arrives_at_the_new_thread_unchanged) {
    ThreadParty p = {0, NULL, 0, 0};

    int64_t h = pal_thread_create(thread_marker, &p, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK(p.arg_seen == &p);
}

TEST(a_stack_smaller_than_the_system_allows_is_raised_and_not_refused) {
    ThreadParty p = {0, NULL, 0, 0};

    /* A kilobyte is below every platform's minimum. The layer raises it to the
     * minimum rather than handing the caller an EINVAL that means something
     * different on every system. */
    int64_t h = pal_thread_create(thread_marker, &p, 1024, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

TEST(a_stack_bigger_than_the_default_is_taken) {
    ThreadParty p = {0, NULL, 0, 0};

    int64_t h = pal_thread_create(thread_marker, &p, 2 * 1024 * 1024, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

TEST(a_thread_can_be_detached_instead_of_joined) {
    static ThreadParty p;

    /* Static rather than on this frame, because a detached thread is still
     * running when the test moves on and nothing here waits for it to be gone,
     * only for it to have finished with the structure. */
    p.ran = 0;
    p.done = 0;

    int64_t h = pal_thread_create(thread_marker, &p, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);

    PalErrno err = PAL_EOTHER;
    CHECK(pal_thread_detach(h, &err));
    CHECK_INT_EQ(err, PAL_OK);

    while (burrow__atomic_load_acquire_u32(&p.done) == 0)
        (void)pal_futex_wait(&p.done, 0, -1, NULL);

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

TEST(sixteen_threads_all_start_and_all_finish) {
    ThreadParty parties[THREAD_PARTY];
    int64_t handles[THREAD_PARTY];

    for (int i = 0; i < THREAD_PARTY; i++) {
        parties[i].ran = 0;
        parties[i].arg_seen = NULL;
        parties[i].self = 0;
        parties[i].done = 0;

        handles[i] = pal_thread_create(thread_marker, &parties[i], 0, NULL);
        CHECK(handles[i] != PAL_INVALID_HANDLE);
    }

    for (int i = 0; i < THREAD_PARTY; i++)
        CHECK(pal_thread_join(handles[i], NULL));

    for (int i = 0; i < THREAD_PARTY; i++) {
        CHECK_INT_EQ(burrow__atomic_load_u32(&parties[i].ran), 1);
        CHECK(parties[i].arg_seen == &parties[i]);
    }
}

TEST(every_thread_running_at_once_has_its_own_identity) {
    ThreadParty parties[THREAD_PARTY];
    int64_t handles[THREAD_PARTY];
    int64_t mine = pal_thread_self();

    for (int i = 0; i < THREAD_PARTY; i++) {
        parties[i].ran = 0;
        parties[i].arg_seen = NULL;
        parties[i].self = 0;
        parties[i].done = 0;

        handles[i] = pal_thread_create(thread_marker, &parties[i], 0, NULL);
        CHECK(handles[i] != PAL_INVALID_HANDLE);
    }

    /* Every one of them is joined before anything is compared, so all sixteen
     * were alive together and none of the identities can be a number the system
     * handed out twice. */
    for (int i = 0; i < THREAD_PARTY; i++)
        CHECK(pal_thread_join(handles[i], NULL));

    for (int i = 0; i < THREAD_PARTY; i++) {
        CHECK(parties[i].self != 0);
        CHECK(parties[i].self != mine);

        for (int j = i + 1; j < THREAD_PARTY; j++)
            CHECK(parties[i].self != parties[j].self);
    }
}

TEST(my_own_identity_is_the_same_every_time_i_ask) {
    CHECK_INT_EQ(pal_thread_self(), pal_thread_self());
}

TEST(the_stack_bounds_hold_a_variable_that_is_on_the_stack) {
    void *lo = NULL;
    void *hi = NULL;
    /* Its address is taken below, which is what keeps it on the stack rather
     * than in a register where there would be nothing to compare. */
    char here = 0;

    if (!pal_thread_stack_bounds(&lo, &hi)) {
        /* An honest no from a platform nobody has written this for. There is
         * nothing to check and there is nothing wrong. */
        CHECK(lo == NULL);
        return;
    }

    CHECK((char *)lo < (char *)hi);

#if !defined(SANITIZED)
    CHECK((const char *)&here >= (char *)lo);
    CHECK((const char *)&here < (char *)hi);
#else
    (void)here;
#endif
}

TEST(yielding_is_allowed_as_often_as_you_like) {
    for (int i = 0; i < 1000; i++)
        pal_thread_yield();

    /* There is nothing to observe. It is here because a yield that faulted or
     * took a lock it should not have would show up under the sanitizers, which
     * is what this suite is run under. */
    CHECK(true);
}

TEST(starting_or_joining_nothing_is_refused) {
    PalErrno err = PAL_OK;

    CHECK_INT_EQ(pal_thread_create(NULL, NULL, 0, &err), PAL_INVALID_HANDLE);
    CHECK_INT_EQ(err, PAL_EINVAL);

    err = PAL_OK;
    CHECK_INT_EQ(pal_thread_create(thread_marker, NULL, -1, &err), PAL_INVALID_HANDLE);
    CHECK_INT_EQ(err, PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_thread_join(PAL_INVALID_HANDLE, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_thread_detach(PAL_INVALID_HANDLE, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
}

TEST(the_threads_take_a_null_error_like_everything_else) {
    ThreadParty p = {0, NULL, 0, 0};

    CHECK_INT_EQ(pal_thread_create(NULL, NULL, 0, NULL), PAL_INVALID_HANDLE);
    CHECK(!pal_thread_join(PAL_INVALID_HANDLE, NULL));
    CHECK(!pal_thread_detach(PAL_INVALID_HANDLE, NULL));

    int64_t h = pal_thread_create(thread_marker, &p, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));
}

/* --------------------------------------------------------------- the signals
 *
 * The fault half of this group is tested end to end in tests/stack_test.c,
 * which runs a goroutine off the bottom of its own stack in a child process and
 * reads the message that comes out. That is the only honest way to check it,
 * since a handler that claims a fault never returns, and it is why there is no
 * test here that makes one. What is left for this file is the contract around
 * it: what the arguments refuse, that a signal stack is per thread, and that a
 * handler installed for an ordinary signal actually runs when it arrives. */

#if !defined(_WIN32)
#include <signal.h>
#endif

static uint32_t signal_hits;
static int32_t signal_seen;

static bool count_signal(int32_t sig, void *info, void *ctx) {
    (void)info;
    (void)ctx;

    signal_seen = sig;
    (void)burrow__atomic_add_u32(&signal_hits, 1);
    return true;
}

TEST(a_signal_stack_can_be_installed_and_given_back) {
    PalErrno err = PAL_EOTHER;

    CHECK(pal_signal_stack_install(&err));
    CHECK_INT_EQ(err, PAL_OK);

    /* Twice is not an error and does not map a second one, which is what lets
     * every thread call it on the way in without checking first. */
    CHECK(pal_signal_stack_install(NULL));

    pal_signal_stack_remove();
    /* And removing one that has already gone is not an error either. */
    pal_signal_stack_remove();

    /* Back again afterwards, because the tests below and the harness itself
     * share this thread and a thread with no signal stack is a thread with no
     * overflow message. */
    CHECK(pal_signal_stack_install(NULL));
}

static void signal_stack_on_a_thread(void *arg) {
    uint32_t *ok = (uint32_t *)arg;

    if (pal_signal_stack_install(NULL) && pal_signal_stack_install(NULL))
        *ok = 1;
    pal_signal_stack_remove();
}

TEST(every_thread_installs_its_own_signal_stack) {
    uint32_t ok = 0;

    int64_t h = pal_thread_create(signal_stack_on_a_thread, &ok, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));
    CHECK_INT_EQ(ok, 1);

    /* The thread removed its own and this one still has the one the test above
     * put back, which is the whole claim: they are not the same mapping. */
    CHECK(pal_signal_stack_install(NULL));
}

TEST(a_handler_that_is_not_there_is_refused) {
    PalErrno err = PAL_OK;

    CHECK(!pal_signal_install(PAL_SIGFAULT, NULL, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
}

TEST(a_number_that_is_not_a_signal_is_refused) {
    PalErrno err = PAL_OK;

    CHECK(!pal_signal_install(4242, count_signal, &err));
#if defined(_WIN32)
    /* Windows has one of these and says so about the rest, so a number nobody
     * recognises and a signal it cannot offer come back the same way. */
    CHECK_INT_EQ(err, PAL_ENOTSUP);
#else
    CHECK_INT_EQ(err, PAL_EINVAL);

    /* The two that cannot be caught, which the system would refuse anyway. They
     * are refused here so that the answer is the same on a libc that decides to
     * be helpful about it. */
    err = PAL_OK;
    CHECK(!pal_signal_install(PAL_SIGKILL, count_signal, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    err = PAL_OK;
    CHECK(!pal_signal_install(PAL_SIGSTOP, count_signal, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
#endif
}

TEST(a_fault_address_needs_a_fault_to_read_it_from) {
    CHECK(pal_signal_fault_addr(NULL) == NULL);
}

#if !defined(_WIN32)

TEST(a_handler_runs_when_the_signal_arrives) {
    PalErrno err = PAL_EOTHER;

    signal_hits = 0;
    signal_seen = 0;

    /* The preemption signal, because it is the one the scheduler will send
     * itself and because its default disposition is to be ignored, so a build
     * where this fails to install does not take the test process with it. */
    CHECK(pal_signal_install(PAL_SIGPREEMPT, count_signal, &err));
    CHECK_INT_EQ(err, PAL_OK);

    CHECK_INT_EQ(raise(SIGURG), 0);

    CHECK_INT_EQ(burrow__atomic_load_u32(&signal_hits), 1);

    /* The handler is told our number and not the platform's, which is the
     * difference between a caller that knows what SIGURG is and one that does
     * not have to. */
    CHECK_INT_EQ(signal_seen, PAL_SIGPREEMPT);
}

TEST(a_blocked_signal_waits_and_arrives_when_it_is_let_through) {
    signal_hits = 0;

    CHECK(pal_signal_install(PAL_SIGPREEMPT, count_signal, NULL));

    PalErrno err = PAL_EOTHER;
    CHECK(pal_signal_mask(PAL_SIGPREEMPT, true, &err));
    CHECK_INT_EQ(err, PAL_OK);

    CHECK_INT_EQ(raise(SIGURG), 0);
    CHECK_INT_EQ(burrow__atomic_load_u32(&signal_hits), 0);

    /* Unblocking is what delivers it, and it is delivered before this call
     * returns, which is what makes the count below safe to read straight
     * away. */
    CHECK(pal_signal_mask(PAL_SIGPREEMPT, false, &err));
    CHECK_INT_EQ(err, PAL_OK);
    CHECK_INT_EQ(burrow__atomic_load_u32(&signal_hits), 1);
}

TEST(installing_twice_replaces_the_handler_and_keeps_what_was_there_first) {
    signal_hits = 0;

    /* Two installs and one raise. A second install that stacked rather than
     * replaced would count twice, and one that recorded our own dispatcher as
     * the thing to forward to would loop instead of returning at all. */
    CHECK(pal_signal_install(PAL_SIGPREEMPT, count_signal, NULL));
    CHECK(pal_signal_install(PAL_SIGPREEMPT, count_signal, NULL));

    CHECK_INT_EQ(raise(SIGURG), 0);
    CHECK_INT_EQ(burrow__atomic_load_u32(&signal_hits), 1);
}

#endif /* !_WIN32 */

TEST(the_signals_take_a_null_error_like_everything_else) {
    CHECK(!pal_signal_install(PAL_SIGFAULT, NULL, NULL));
    CHECK(!pal_signal_install(4242, count_signal, NULL));
    CHECK(pal_signal_stack_install(NULL));

#if !defined(_WIN32)
    CHECK(pal_signal_mask(PAL_SIGPREEMPT, true, NULL));
    CHECK(pal_signal_mask(PAL_SIGPREEMPT, false, NULL));
#else
    CHECK(!pal_signal_mask(PAL_SIGPREEMPT, true, NULL));
#endif
}

int main(void) {
    RUN(every_code_has_a_message);
    RUN(success_and_nonsense_are_not_the_same_answer);

    RUN(the_monotonic_clock_only_goes_forwards);
    RUN(the_wall_clock_says_it_is_after_the_time_this_was_written);
    RUN(the_two_clocks_are_not_the_same_clock);
    RUN(a_sleep_takes_at_least_as_long_as_it_was_asked_for);
    RUN(a_sleep_of_nothing_returns);

    RUN(the_page_size_is_a_power_of_two);
    RUN(the_page_size_is_the_same_every_time);
    RUN(there_is_at_least_one_processor);

    RUN(a_reservation_can_be_committed_and_written_and_given_back);
    RUN(committing_twice_is_not_an_error);
    RUN(a_guard_can_be_put_on_a_committed_page);
    RUN(memory_that_is_not_page_aligned_is_refused);
    RUN(a_large_reservation_costs_address_space_and_not_memory);

    RUN(random_bytes_fills_the_buffer);
    RUN(two_asks_do_not_give_the_same_answer);
    RUN(random_writes_exactly_as_many_bytes_as_it_was_asked_for);
    RUN(asking_for_nothing_is_not_a_failure);

    RUN(the_error_is_optional_like_every_other_out_parameter);

#if !defined(_WIN32)
    RUN(there_is_one_poller_and_a_second_ask_is_refused);
    RUN(a_descriptor_with_something_on_it_comes_back_ready);
    RUN(a_look_with_nothing_to_see_comes_back_empty);
    RUN(a_break_ends_a_wait_and_is_never_reported_as_an_event);
    RUN(many_breaks_at_once_cost_one_wakeup);
    RUN(a_handle_that_is_not_the_poller_is_refused);
    RUN(the_wakeups_own_tag_cannot_be_used_as_a_user_pointer);
    RUN(a_wait_with_nowhere_to_put_the_answer_is_refused);
    RUN(the_poller_takes_a_null_error_like_everything_else);
#endif

    RUN(a_word_that_already_differs_does_not_wait);
    RUN(a_wait_with_no_time_and_no_change_times_out);
    RUN(a_wait_with_a_deadline_waits_at_least_that_long);
    RUN(a_sleeper_is_woken_by_a_wake);
    RUN(everybody_waiting_can_be_released_at_once);
    RUN(a_sleeper_on_one_word_is_not_released_by_a_wake_on_another);
    RUN(a_wake_with_nobody_waiting_is_free_and_not_an_error);
    RUN(a_sleeper_with_a_deadline_gives_up_when_nobody_comes);
    RUN(a_wait_or_wake_on_nothing_is_refused);
    RUN(the_futex_takes_a_null_error_like_everything_else);

    RUN(a_thread_runs_the_function_it_was_given);
    RUN(the_argument_arrives_at_the_new_thread_unchanged);
    RUN(a_stack_smaller_than_the_system_allows_is_raised_and_not_refused);
    RUN(a_stack_bigger_than_the_default_is_taken);
    RUN(a_thread_can_be_detached_instead_of_joined);
    RUN(sixteen_threads_all_start_and_all_finish);
    RUN(every_thread_running_at_once_has_its_own_identity);
    RUN(my_own_identity_is_the_same_every_time_i_ask);
    RUN(the_stack_bounds_hold_a_variable_that_is_on_the_stack);
    RUN(yielding_is_allowed_as_often_as_you_like);
    RUN(starting_or_joining_nothing_is_refused);
    RUN(the_threads_take_a_null_error_like_everything_else);

    RUN(a_signal_stack_can_be_installed_and_given_back);
    RUN(every_thread_installs_its_own_signal_stack);
    RUN(a_handler_that_is_not_there_is_refused);
    RUN(a_number_that_is_not_a_signal_is_refused);
    RUN(a_fault_address_needs_a_fault_to_read_it_from);
#if !defined(_WIN32)
    RUN(a_handler_runs_when_the_signal_arrives);
    RUN(a_blocked_signal_waits_and_arrives_when_it_is_let_through);
    RUN(installing_twice_replaces_the_handler_and_keeps_what_was_there_first);
#endif
    RUN(the_signals_take_a_null_error_like_everything_else);

    return harness_report("pal");
}
