/* Waiting on an address on Windows.
 *
 * Windows has WaitOnAddress, which is the futex under another name and would be
 * a shorter file than this one. It is not used, for one reason: it lives in
 * synchronization.lib rather than kernel32, and burrow links nothing today. A
 * library that deploys as one C file and needs no link line is worth more than
 * the few dozen lines below.
 *
 * So this is the same table of buckets src/pal/futex_posix.c builds, over an
 * SRWLOCK and a CONDITION_VARIABLE, both of which are in kernel32 and have been
 * since Vista. Read the comments in that file for the shape and the reasoning;
 * they are the same shape and the same reasoning, with different spelling and
 * one real difference, which is that these two primitives have static
 * initialisers and need no setup pass.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>

#include <windows.h>

/* A power of two so the hash can mask, and 64 for the same reason as the POSIX
 * backend: a collision costs a spurious wakeup and nothing else. */
#define BUCKETS 64

/* The longest any single wait may be, a thousand seconds. SleepConditionVariable
 * counts milliseconds in a DWORD, and INFINITE is a value rather than a very
 * long wait, so a cap is what keeps the conversion from being a conversion that
 * usually works. */
#define FUTEX_MAX_WAIT_NS 1000000000000LL

typedef struct Waiter {
    /* Compared and never read through, which is what lets a wake outlive the
     * memory the address names. */
    const uint32_t *addr;
    struct Waiter *next;
    /* Set by a wake that chose this waiter, under the bucket lock. */
    bool signalled;
} Waiter;

typedef struct Bucket {
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
    Waiter *head;
} Bucket;

/* SRWLOCK_INIT and CONDITION_VARIABLE_INIT are both a zeroed structure, so the
 * whole table is initialised by being static and there is nothing to run
 * first. */
static Bucket buckets[BUCKETS] = {{SRWLOCK_INIT, CONDITION_VARIABLE_INIT, NULL}};

static Bucket *bucket_of(const uint32_t *addr) {
    uint64_t x = (uint64_t)(uintptr_t)addr >> 2;

    x *= 0x9E3779B97F4A7C15ULL;
    return &buckets[(x >> 58) & (BUCKETS - 1u)];
}

/* When a wait of ns nanoseconds starting now runs out. See the same function in
 * src/pal/futex_posix.c for why the addition is guarded. */
static int64_t deadline_from(int64_t ns) {
    int64_t now = pal_clock_monotonic();

    if (ns > INT64_MAX - now)
        return INT64_MAX;
    return now + ns;
}

static void unlink_waiter(Bucket *b, Waiter *w) {
    Waiter **link = &b->head;

    while (*link != NULL) {
        if (*link == w) {
            *link = w->next;
            return;
        }
        link = &(*link)->next;
    }
}

bool pal_futex_wait(uint32_t *addr, uint32_t expect, int64_t timeout_ns,
                    PalErrno *err) {
    if (addr == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Before the lock, because a word that has already changed is the common
     * case for a caller that got here by losing a race. */
    if (burrow__atomic_load_acquire_u32(addr) != expect) {
        BURROW_OUT(err, PAL_OK);
        return true;
    }

    if (timeout_ns == 0) {
        BURROW_OUT(err, PAL_ETIMEDOUT);
        return false;
    }

    int64_t deadline = timeout_ns > 0 ? deadline_from(timeout_ns) : -1;

    Bucket *b = bucket_of(addr);

    AcquireSRWLockExclusive(&b->mu);

    /* Again under the lock, which is what makes a wake that lands between the
     * check above and the queueing below impossible to lose. */
    if (burrow__atomic_load_acquire_u32(addr) != expect) {
        ReleaseSRWLockExclusive(&b->mu);
        BURROW_OUT(err, PAL_OK);
        return true;
    }

    Waiter w;
    w.addr = addr;
    w.signalled = false;
    w.next = b->head;
    b->head = &w;

    bool woken = false;

    for (;;) {
        DWORD ms = INFINITE;

        if (deadline >= 0) {
            int64_t left = deadline - pal_clock_monotonic();
            if (left <= 0)
                break;

            if (left > FUTEX_MAX_WAIT_NS)
                left = FUTEX_MAX_WAIT_NS;

            /* Rounded up, because a wait short by less than a millisecond comes
             * straight back and goes round again, which is how a deadline turns
             * into a spin. */
            ms = (DWORD)((left + 999999) / 1000000);
        }

        /* The result is ignored on purpose. The loop recomputes what is left of
         * the deadline every time round and decides from that, so whether this
         * came back because of a wake, a timeout or nothing at all makes no
         * difference to what happens next. */
        (void)SleepConditionVariableSRW(&b->cv, &b->mu, ms, 0);

        if (w.signalled) {
            woken = true;
            break;
        }

        if (burrow__atomic_load_acquire_u32(addr) != expect) {
            woken = true;
            break;
        }
    }

    unlink_waiter(b, &w);
    ReleaseSRWLockExclusive(&b->mu);

    if (!woken) {
        BURROW_OUT(err, PAL_ETIMEDOUT);
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

int64_t pal_futex_wake(uint32_t *addr, int64_t n, PalErrno *err) {
    if (addr == NULL || n < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    Bucket *b = bucket_of(addr);

    AcquireSRWLockExclusive(&b->mu);

    int64_t woken = 0;
    for (Waiter *w = b->head; w != NULL && woken < n; w = w->next) {
        if (w->addr != addr || w->signalled)
            continue;

        w->signalled = true;
        woken++;
    }

    /* Wake all rather than one, even when only one record was marked, because
     * the condition variable has no idea which sleeper holds which record. The
     * ones that were not marked look at their record, look at their word, and
     * go straight back down. */
    if (woken > 0)
        WakeAllConditionVariable(&b->cv);

    ReleaseSRWLockExclusive(&b->mu);

    BURROW_OUT(err, PAL_OK);
    return woken;
}

#endif /* BURROW_OS_WINDOWS */
