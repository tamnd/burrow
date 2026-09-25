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

#include "check.h"

#include <string.h>

/* ---------------------------------------------------------------- the codes */

static void TestEveryCodeHasAMessage(TestingT *t) {
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

static void TestSuccessAndNonsenseAreNotTheSameAnswer(TestingT *t) {
    CHECK(strcmp(pal_errno_string(PAL_OK), "no error") == 0);
    CHECK(strcmp(pal_errno_string(PAL_EOTHER), "unknown error") == 0);

    /* Outside the range in either direction, including a raw POSIX errno, which
     * is the mistake the numbering starts at 1000 to make visible. */
    CHECK(strcmp(pal_errno_string(2), "unknown error") == 0);
    CHECK(strcmp(pal_errno_string(-1), "unknown error") == 0);
    CHECK(strcmp(pal_errno_string(999999), "unknown error") == 0);
}

/* --------------------------------------------------------------- the clocks */

static void TestTheMonotonicClockOnlyGoesForwards(TestingT *t) {
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

static void TestTheWallClockSaysItIsAfterTheTimeThisWasWritten(TestingT *t) {
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

static void TestTheTwoClocksAreNotTheSameClock(TestingT *t) {
    /* The monotonic one counts from an arbitrary point, usually boot, and the
     * wall clock counts from 1970. A backend that wired one to the other would
     * pass every test above and fail this. */
    int64_t mono = pal_clock_monotonic();
    int64_t wall = pal_clock_realtime();
    CHECK(wall - mono > 0);
}

static void TestASleepTakesAtLeastAsLongAsItWasAskedFor(TestingT *t) {
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

static void TestASleepOfNothingReturns(TestingT *t) {
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

static void TestThePageSizeIsAPowerOfTwo(TestingT *t) {
    int64_t page = pal_page_size();

    CHECK(page >= 512);
    CHECK(page <= 1024 * 1024);
    CHECK((page & (page - 1)) == 0);
}

static void TestThePageSizeIsTheSameEveryTime(TestingT *t) {
    /* It is cached after the first call, and a cache that answers differently
     * once it is warm is worse than no cache. */
    int64_t first = pal_page_size();
    for (int i = 0; i < 100; i++)
        CHECK(pal_page_size() == first);
}

static void TestThereIsAtLeastOneProcessor(TestingT *t) {
    int64_t n = pal_cpu_count();

    CHECK(n >= 1);

    /* A machine with more than this exists and is not a machine this test runs
     * on. The bound is here to catch a count read out of the wrong field. */
    CHECK(n <= 4096);

    for (int i = 0; i < 100; i++)
        CHECK(pal_cpu_count() == n);
}

static void TestCPUFeaturesAreTheMachines(TestingT *t) {
    uint32_t f = pal_cpu_features();
    for (int i = 0; i < 100; i++)
        CHECK(pal_cpu_features() == f);

    /* A bit is only set on the architecture it names. */
    uint32_t x86 = PAL_CPU_X86_SSSE3 | PAL_CPU_X86_SSE41 | PAL_CPU_X86_SHA;
    uint32_t arm64 = PAL_CPU_ARM64_SHA1 | PAL_CPU_ARM64_SHA2 | PAL_CPU_ARM64_SHA512 |
                     PAL_CPU_ARM64_SHA3;
#if !defined(BURROW_ARCH_AMD64) && !defined(BURROW_ARCH_386)
    CHECK((f & x86) == 0);
#endif
#if !defined(BURROW_ARCH_ARM64)
    CHECK((f & arm64) == 0);
#endif
    CHECK((f & ~(x86 | arm64)) == 0);

    /* make test runs this again with GODEBUG=cpu.all=off, which has to leave
     * nothing. */
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++)
        if (strcmp(*env, "GODEBUG=cpu.all=off") == 0)
            CHECK(f == 0);
}

/* --------------------------------------------------------------- the memory */

static void TestAReservationCanBeCommittedAndWrittenAndGivenBack(TestingT *t) {
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

static void TestCommittingTwiceIsNotAnError(TestingT *t) {
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

static void TestAGuardCanBePutOnACommittedPage(TestingT *t) {
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

static void TestMemoryThatIsNotPageAlignedIsRefused(TestingT *t) {
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

static void TestALargeReservationCostsAddressSpaceAndNotMemory(TestingT *t) {
    int64_t page = pal_page_size();

    /* A gigabyte, reserved and never committed. This is what a goroutine stack
     * does, and on a machine where reserving meant committing it would either
     * fail or make the process a gigabyte bigger. Ten of them in a row makes
     * the point without depending on how the machine reports its own size. A
     * 32 bit process has 4 gigabytes of address space in all, so there it is
     * ten slices of 128 megabytes instead. */
    void *held[10];
    int64_t bytes = sizeof(void *) >= 8 ? 1024 * 1024 * 1024 : 128 * 1024 * 1024;
    bytes -= bytes % page;

    for (int i = 0; i < 10; i++) {
        held[i] = pal_vm_reserve(bytes, NULL);
        CHECK(held[i] != NULL);
    }

    for (int i = 0; i < 10; i++)
        CHECK(pal_vm_release(held[i], bytes, NULL));
}

/* --------------------------------------------------------------- the random */

static void TestRandomBytesFillsTheBuffer(TestingT *t) {
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

static void TestTwoAsksDoNotGiveTheSameAnswer(TestingT *t) {
    unsigned char a[32];
    unsigned char b[32];

    if (!pal_random_bytes(a, (int64_t)sizeof a, NULL))
        return;
    CHECK(pal_random_bytes(b, (int64_t)sizeof b, NULL));

    CHECK(memcmp(a, b, sizeof a) != 0);
}

static void TestRandomWritesExactlyAsManyBytesAsItWasAskedFor(TestingT *t) {
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

static void TestAskingForNothingIsNotAFailure(TestingT *t) {
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

static void TestTheErrorIsOptionalLikeEveryOtherOutParameter(TestingT *t) {
    /* Every entry point takes the error last and every one of them has to
     * accept NULL there, because a caller that only wants to know whether it
     * worked should not have to declare a variable to throw away. */
    int64_t page = pal_page_size();

    CHECK(pal_vm_reserve(page + 1, NULL) == NULL);

    void *p = pal_vm_reserve(page, NULL);
    CHECK(p != NULL);
    CHECK(pal_vm_commit(p, page, NULL));
    /* Guard before decommit, because a guard is made from committed memory.
     * POSIX will protect a page that has nothing behind it and Windows will
     * not, so the other order passes on one and fails on the other. */
    CHECK(pal_vm_guard(p, page, NULL));
    CHECK(pal_vm_decommit(p, page, NULL));
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
 * A target with no poller at all, which is Cosmopolitan for now, has none of
 * these to run.
 *
 * There is one poller per process and these share it, so the order below
 * matters: the first test makes it and the rest use it. */
#if defined(BURROW_NETPOLL_READINESS)

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

static void TestThereIsOnePollerAndASecondAskIsRefused(TestingT *t) {
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

static void TestADescriptorWithSomethingOnItComesBackReady(TestingT *t) {
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

static void TestALookWithNothingToSeeComesBackEmpty(TestingT *t) {
    PalPollEvent events[8];
    PalErrno err = PAL_ETIMEDOUT;

    /* Zero is the non blocking look the scheduler does when it is hunting for
     * work, and finding nothing is not a failure. */
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 0, &err), 0);
    CHECK_INT_EQ(err, PAL_OK);
}

static void TestABreakEndsAWaitAndIsNeverReportedAsAnEvent(TestingT *t) {
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

static void TestManyBreaksAtOnceCostOneWakeup(TestingT *t) {
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

static void TestAHandleThatIsNotThePollerIsRefused(TestingT *t) {
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

static void TestTheWakeupsOwnTagCannotBeUsedAsAUserPointer(TestingT *t) {
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

static void TestAWaitWithNowhereToPutTheAnswerIsRefused(TestingT *t) {
    PalPollEvent events[8];
    PalErrno err = PAL_OK;

    CHECK_INT_EQ(pal_poll_wait(poller, NULL, 8, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_poll_wait(poller, events, 0, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_poll_wait(poller, events, -1, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);
}

static void TestThePollerTakesANullErrorLikeEverythingElse(TestingT *t) {
    PalPollEvent events[8];

    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 0, NULL), 0);
    CHECK(pal_poll_break(poller, NULL));
    CHECK_INT_EQ(pal_poll_wait(poller, events, 8, 1000000000, NULL), 0);
    CHECK(!pal_poll_add(PAL_INVALID_HANDLE, 0, (void *)1, NULL));
}

#endif /* BURROW_NETPOLL_READINESS */

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

static void TestAWordThatAlreadyDiffersDoesNotWait(TestingT *t) {
    uint32_t word = 7;
    PalErrno err = PAL_ENOSYS;

    /* No timeout and no waker, so if this did wait the test would hang rather
     * than fail, which is the honest way to check it. */
    CHECK(pal_futex_wait(&word, 1, -1, &err));
    CHECK_INT_EQ(err, PAL_OK);
}

static void TestAWaitWithNoTimeAndNoChangeTimesOut(TestingT *t) {
    uint32_t word = 0;
    PalErrno err = PAL_OK;

    CHECK(!pal_futex_wait(&word, 0, 0, &err));
    CHECK_INT_EQ(err, PAL_ETIMEDOUT);
}

static void TestAWaitWithADeadlineWaitsAtLeastThatLong(TestingT *t) {
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

static void TestASleeperIsWokenByAWake(TestingT *t) {
    FutexParty p = {0, 0, 0, -1};
    burrow__Thread th;

    CHECK(burrow__thread_start(&th, futex_sleeper, &p, 0));

    /* The store is what the wait is really waiting for, and the wake is only
     * what tells the kernel to go and look. Ordered this way round because the
     * other way round is the lost wakeup every futex user writes once. */
    burrow__atomic_store_u32(&p.word, 1);

    PalErrno err = PAL_ENOSYS;
    CHECK(pal_futex_wake(&p.word, INT64_MAX, &err) >= 0);
    CHECK_INT_EQ(err, PAL_OK);

    CHECK(burrow__thread_join(&th));
    CHECK_INT_EQ(p.woke, 1);
    CHECK_INT_EQ(p.timedout, 0);
}

static void TestEverybodyWaitingCanBeReleasedAtOnce(TestingT *t) {
    enum { SLEEPERS = 8 };

    FutexParty p = {0, 0, 0, -1};
    burrow__Thread th[SLEEPERS];

    for (int i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_start(&th[i], futex_sleeper, &p, 0));

    burrow__atomic_store_u32(&p.word, 1);
    CHECK(pal_futex_wake(&p.word, INT64_MAX, NULL) >= 0);

    for (int i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_join(&th[i]));

    /* Every one of them, and none of them by a timeout, since they were started
     * without one. A backend that woke only the first would hang here instead,
     * which is the same answer said more slowly. */
    CHECK_INT_EQ(p.woke, SLEEPERS);
    CHECK_INT_EQ(p.timedout, 0);
}

static void TestASleeperOnOneWordIsNotReleasedByAWakeOnAnother(TestingT *t) {
    FutexParty mine = {0, 0, 0, -1};
    uint32_t other = 0;
    burrow__Thread th;

    CHECK(burrow__thread_start(&th, futex_sleeper, &mine, 0));

    /* A wake on a word nobody is waiting on. It may share a bucket with the one
     * that does have a sleeper on it, which is allowed to wake that sleeper
     * spuriously, and the sleeper's loop is what makes that not matter: it
     * looks at its own word and goes back down. */
    CHECK_INT_EQ(pal_futex_wake(&other, INT64_MAX, NULL), 0);

    burrow__atomic_store_u32(&mine.word, 1);
    CHECK(pal_futex_wake(&mine.word, INT64_MAX, NULL) >= 0);

    CHECK(burrow__thread_join(&th));
    CHECK_INT_EQ(mine.woke, 1);
}

static void TestAWakeWithNobodyWaitingIsFreeAndNotAnError(TestingT *t) {
    uint32_t word = 0;
    PalErrno err = PAL_ENOSYS;

    CHECK_INT_EQ(pal_futex_wake(&word, INT64_MAX, &err), 0);
    CHECK_INT_EQ(err, PAL_OK);

    CHECK_INT_EQ(pal_futex_wake(&word, 1, NULL), 0);
    CHECK_INT_EQ(pal_futex_wake(&word, 0, NULL), 0);
}

static void TestASleeperWithADeadlineGivesUpWhenNobodyComes(TestingT *t) {
    FutexParty p = {0, 0, 0, 20000000};
    burrow__Thread th;

    CHECK(burrow__thread_start(&th, futex_sleeper, &p, 0));
    CHECK(burrow__thread_join(&th));

    CHECK_INT_EQ(p.timedout, 1);
    CHECK_INT_EQ(p.woke, 0);
}

static void TestAWaitOrWakeOnNothingIsRefused(TestingT *t) {
    PalErrno err = PAL_OK;

    CHECK(!pal_futex_wait(NULL, 0, 0, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);

    CHECK_INT_EQ(pal_futex_wake(NULL, 1, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    uint32_t word = 0;
    CHECK_INT_EQ(pal_futex_wake(&word, -1, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);
}

static void TestTheFutexTakesANullErrorLikeEverythingElse(TestingT *t) {
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

static void TestAThreadRunsTheFunctionItWasGiven(TestingT *t) {
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

static void TestTheArgumentArrivesAtTheNewThreadUnchanged(TestingT *t) {
    ThreadParty p = {0, NULL, 0, 0};

    int64_t h = pal_thread_create(thread_marker, &p, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK(p.arg_seen == &p);
}

static void TestAStackSmallerThanTheSystemAllowsIsRaisedAndNotRefused(TestingT *t) {
    ThreadParty p = {0, NULL, 0, 0};

    /* A kilobyte is below every platform's minimum. The layer raises it to the
     * minimum rather than handing the caller an EINVAL that means something
     * different on every system. */
    int64_t h = pal_thread_create(thread_marker, &p, 1024, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

static void TestAStackBiggerThanTheDefaultIsTaken(TestingT *t) {
    ThreadParty p = {0, NULL, 0, 0};

    int64_t h = pal_thread_create(thread_marker, &p, 2 * 1024 * 1024, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));

    CHECK_INT_EQ(burrow__atomic_load_u32(&p.ran), 1);
}

static void TestAThreadCanBeDetachedInsteadOfJoined(TestingT *t) {
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

static void TestSixteenThreadsAllStartAndAllFinish(TestingT *t) {
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

static void TestEveryThreadRunningAtOnceHasItsOwnIdentity(TestingT *t) {
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

static void TestMyOwnIdentityIsTheSameEveryTimeIAsk(TestingT *t) {
    CHECK_INT_EQ(pal_thread_self(), pal_thread_self());
}

static void TestTheStackBoundsHoldAVariableThatIsOnTheStack(TestingT *t) {
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

static void TestYieldingIsAllowedAsOftenAsYouLike(TestingT *t) {
    for (int i = 0; i < 1000; i++)
        pal_thread_yield();

    /* There is nothing to observe. It is here because a yield that faulted or
     * took a lock it should not have would show up under the sanitizers, which
     * is what this suite is run under. */
    CHECK(true);
}

static void TestStartingOrJoiningNothingIsRefused(TestingT *t) {
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

static void TestTheThreadsTakeANullErrorLikeEverythingElse(TestingT *t) {
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

static void TestASignalStackCanBeInstalledAndGivenBack(TestingT *t) {
    PalErrno err = PAL_EOTHER;

    CHECK(pal_signal_stack_install(&err));
    CHECK_INT_EQ(err, PAL_OK);

    /* Twice is not an error and does not map a second one, which is what lets
     * every thread call it on the way in without checking first. */
    CHECK(pal_signal_stack_install(NULL));

    pal_signal_stack_remove();
    /* And removing one that has already gone is not an error either. */
    pal_signal_stack_remove();

    /* Back again afterwards, because the tests below and the testing package
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

static void TestEveryThreadInstallsItsOwnSignalStack(TestingT *t) {
    uint32_t ok = 0;

    int64_t h = pal_thread_create(signal_stack_on_a_thread, &ok, 0, NULL);
    CHECK(h != PAL_INVALID_HANDLE);
    CHECK(pal_thread_join(h, NULL));
    CHECK_INT_EQ(ok, 1);

    /* The thread removed its own and this one still has the one the test above
     * put back, which is the whole claim: they are not the same mapping. */
    CHECK(pal_signal_stack_install(NULL));
}

static void TestAHandlerThatIsNotThereIsRefused(TestingT *t) {
    PalErrno err = PAL_OK;

    CHECK(!pal_signal_install(PAL_SIGFAULT, NULL, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
}

static void TestANumberThatIsNotASignalIsRefused(TestingT *t) {
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

static void TestAFaultAddressNeedsAFaultToReadItFrom(TestingT *t) {
    CHECK(pal_signal_fault_addr(NULL) == NULL);
}

#if !defined(_WIN32)

static void TestAHandlerRunsWhenTheSignalArrives(TestingT *t) {
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

static void TestABlockedSignalWaitsAndArrivesWhenItIsLetThrough(TestingT *t) {
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

static void
TestInstallingTwiceReplacesTheHandlerAndKeepsWhatWasThereFirst(TestingT *t) {
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

static void TestTheSignalsTakeANullErrorLikeEverythingElse(TestingT *t) {
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

#if defined(BURROW_NETPOLL_READINESS)
#define TESTS_1(X)                                                                     \
    X(TestThereIsOnePollerAndASecondAskIsRefused)                                      \
    X(TestADescriptorWithSomethingOnItComesBackReady)                                  \
    X(TestALookWithNothingToSeeComesBackEmpty)                                         \
    X(TestABreakEndsAWaitAndIsNeverReportedAsAnEvent)                                  \
    X(TestManyBreaksAtOnceCostOneWakeup)                                               \
    X(TestAHandleThatIsNotThePollerIsRefused)                                          \
    X(TestTheWakeupsOwnTagCannotBeUsedAsAUserPointer)                                  \
    X(TestAWaitWithNowhereToPutTheAnswerIsRefused)                                     \
    X(TestThePollerTakesANullErrorLikeEverythingElse)
#else
#define TESTS_1(X)
#endif

#if !defined(_WIN32)
#define TESTS_2(X)                                                                     \
    X(TestAHandlerRunsWhenTheSignalArrives)                                            \
    X(TestABlockedSignalWaitsAndArrivesWhenItIsLetThrough)                             \
    X(TestInstallingTwiceReplacesTheHandlerAndKeepsWhatWasThereFirst)
#else
#define TESTS_2(X)
#endif

#define TESTS(X)                                                                       \
    X(TestEveryCodeHasAMessage)                                                        \
    X(TestSuccessAndNonsenseAreNotTheSameAnswer)                                       \
    X(TestTheMonotonicClockOnlyGoesForwards)                                           \
    X(TestTheWallClockSaysItIsAfterTheTimeThisWasWritten)                              \
    X(TestTheTwoClocksAreNotTheSameClock)                                              \
    X(TestASleepTakesAtLeastAsLongAsItWasAskedFor)                                     \
    X(TestASleepOfNothingReturns)                                                      \
    X(TestThePageSizeIsAPowerOfTwo)                                                    \
    X(TestThePageSizeIsTheSameEveryTime)                                               \
    X(TestThereIsAtLeastOneProcessor)                                                  \
    X(TestCPUFeaturesAreTheMachines)                                                   \
    X(TestAReservationCanBeCommittedAndWrittenAndGivenBack)                            \
    X(TestCommittingTwiceIsNotAnError)                                                 \
    X(TestAGuardCanBePutOnACommittedPage)                                              \
    X(TestMemoryThatIsNotPageAlignedIsRefused)                                         \
    X(TestALargeReservationCostsAddressSpaceAndNotMemory)                              \
    X(TestRandomBytesFillsTheBuffer)                                                   \
    X(TestTwoAsksDoNotGiveTheSameAnswer)                                               \
    X(TestRandomWritesExactlyAsManyBytesAsItWasAskedFor)                               \
    X(TestAskingForNothingIsNotAFailure)                                               \
    X(TestTheErrorIsOptionalLikeEveryOtherOutParameter)                                \
    X(TestAWordThatAlreadyDiffersDoesNotWait)                                          \
    X(TestAWaitWithNoTimeAndNoChangeTimesOut)                                          \
    X(TestAWaitWithADeadlineWaitsAtLeastThatLong)                                      \
    X(TestASleeperIsWokenByAWake)                                                      \
    X(TestEverybodyWaitingCanBeReleasedAtOnce)                                         \
    X(TestASleeperOnOneWordIsNotReleasedByAWakeOnAnother)                              \
    X(TestAWakeWithNobodyWaitingIsFreeAndNotAnError)                                   \
    X(TestASleeperWithADeadlineGivesUpWhenNobodyComes)                                 \
    X(TestAWaitOrWakeOnNothingIsRefused)                                               \
    X(TestTheFutexTakesANullErrorLikeEverythingElse)                                   \
    X(TestAThreadRunsTheFunctionItWasGiven)                                            \
    X(TestTheArgumentArrivesAtTheNewThreadUnchanged)                                   \
    X(TestAStackSmallerThanTheSystemAllowsIsRaisedAndNotRefused)                       \
    X(TestAStackBiggerThanTheDefaultIsTaken)                                           \
    X(TestAThreadCanBeDetachedInsteadOfJoined)                                         \
    X(TestSixteenThreadsAllStartAndAllFinish)                                          \
    X(TestEveryThreadRunningAtOnceHasItsOwnIdentity)                                   \
    X(TestMyOwnIdentityIsTheSameEveryTimeIAsk)                                         \
    X(TestTheStackBoundsHoldAVariableThatIsOnTheStack)                                 \
    X(TestYieldingIsAllowedAsOftenAsYouLike)                                           \
    X(TestStartingOrJoiningNothingIsRefused)                                           \
    X(TestTheThreadsTakeANullErrorLikeEverythingElse)                                  \
    X(TestASignalStackCanBeInstalledAndGivenBack)                                      \
    X(TestEveryThreadInstallsItsOwnSignalStack)                                        \
    X(TestAHandlerThatIsNotThereIsRefused)                                             \
    X(TestANumberThatIsNotASignalIsRefused)                                            \
    X(TestAFaultAddressNeedsAFaultToReadItFrom)                                        \
    X(TestTheSignalsTakeANullErrorLikeEverythingElse)                                  \
    TESTS_1(X)                                                                         \
    TESTS_2(X)

TESTING_MAIN_BARE(TESTS)
