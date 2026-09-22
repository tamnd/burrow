/* Waiting on an address where the kernel has no such call, which is macOS and
 * the BSDs.
 *
 * Linux has a futex. Windows has WaitOnAddress. macOS has __ulock_wait, which
 * is private, undocumented, and has changed shape between releases, and Go does
 * not use it either. FreeBSD has _umtx_op, OpenBSD has its own futex, NetBSD
 * has __lwp_park, and none of them agree on anything. So this file builds the
 * primitive out of what all of them do have, which is a mutex and a condition
 * variable.
 *
 * The shape is a fixed table of buckets. An address hashes to one bucket, a
 * waiter puts a record on that bucket's list and sleeps on its condition
 * variable, and a wake walks the list marking the records that name the address
 * it was given. Nothing allocates: the record lives in the stack frame of the
 * thread that is about to sleep on it, and it is unlinked before that frame
 * goes away.
 *
 * Two addresses that land in the same bucket wake each other up. That is a
 * handful of wasted instructions and not a correctness problem, because a
 * waiter that finds its own record unmarked and its own word unchanged goes
 * back to sleep with the remaining time recomputed. It is the same trade every
 * futex emulation makes, including the one inside glibc's condition variables.
 *
 * What this file deliberately does not do is take a lock on the fast path,
 * because it is never on the fast path. A caller checks its own word first and
 * only comes here when a thread really has to sleep. src/runtime/note.c is the
 * one caller today and an uncontended note there still costs two atomics and no
 * call into this file at all.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* For CLOCK_MONOTONIC and pthread_condattr_setclock, which are POSIX rather
 * than C. Not on macOS, where the default visibility is everything and asking
 * for POSIX instead takes away the _np call this file uses there. */
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_LINUX)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* A condition variable measures an absolute deadline on CLOCK_REALTIME unless
 * it is told otherwise, and CLOCK_REALTIME is the wall clock, which is the one
 * clock a timeout must not be measured on. Every POSIX system since 2001 can be
 * told otherwise with pthread_condattr_setclock. macOS has never implemented
 * that call and offers pthread_cond_timedwait_relative_np instead, which takes
 * a duration and is already on the monotonic clock. Go does the same split for
 * the same reason. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define FUTEX_COND_RELATIVE 1
#else
#define FUTEX_COND_RELATIVE 0
#endif

/* How many buckets. A power of two so the hash can mask, and 64 because the
 * only thing a collision costs is a spurious wakeup and the number of threads
 * asleep in here at once is the number of Ms, which is the number of cores. */
#define BUCKETS 64

/* The longest any single wait may be, a thousand seconds. It costs one extra
 * call every quarter of an hour, and in exchange every conversion from
 * nanoseconds into a timespec below is something that cannot overflow rather
 * than something that usually does not. */
#define FUTEX_MAX_WAIT_NS 1000000000000LL

typedef struct Waiter {
    /* The address this waiter is queued on. Compared and never read through,
     * which is what lets a wake outlive the memory the address names. */
    const uint32_t *addr;
    struct Waiter *next;
    /* Set by a wake that chose this waiter. Written and read under the bucket
     * lock, so it needs no atomic of its own. */
    bool signalled;
} Waiter;

typedef struct Bucket {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Waiter *head;
} Bucket;

static Bucket buckets[BUCKETS];

/* The table is set up on first use rather than at load time, because burrow has
 * no load time hook and a static initialiser cannot set the condition
 * variable's clock. */
static pthread_once_t setup_once = PTHREAD_ONCE_INIT;

/* Whether that setup worked. A system that will not give out sixty four mutexes
 * is a system where every wait has to fail loudly rather than return as if it
 * had slept. */
static bool setup_ok;

static void setup(void) {
    pthread_condattr_t *ap = NULL;

#if !FUTEX_COND_RELATIVE
    pthread_condattr_t attr;

    if (pthread_condattr_init(&attr) != 0)
        return;

    /* A failure here is a system that has the call and will not do it, which
     * leaves the condition variable on the wall clock. Better to refuse than to
     * hand back a wait whose timeouts are wrong twice a year. */
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        (void)pthread_condattr_destroy(&attr);
        return;
    }

    ap = &attr;
#endif

    int i = 0;
    for (; i < BUCKETS; i++) {
        if (pthread_mutex_init(&buckets[i].mu, NULL) != 0)
            break;
        if (pthread_cond_init(&buckets[i].cv, ap) != 0) {
            (void)pthread_mutex_destroy(&buckets[i].mu);
            break;
        }
        buckets[i].head = NULL;
    }

    if (ap != NULL)
        (void)pthread_condattr_destroy(ap);

    /* Anything already made is left made. This runs once and only once, so a
     * partial table is never reused and never grown: the flag below says the
     * whole group is unavailable and nothing looks at the buckets again. */
    setup_ok = i == BUCKETS;
}

/* Which bucket an address belongs to.
 *
 * The low two or three bits of a uint32_t's address are always the same, so
 * they are shifted off first, and what is left is multiplied by a 64 bit odd
 * constant and read from the top. That is the standard Fibonacci hashing
 * constant and it spreads a run of consecutive words, which is exactly what a
 * table of notes inside one array of Ms looks like. */
static Bucket *bucket_of(const uint32_t *addr) {
    uint64_t x = (uint64_t)(uintptr_t)addr >> 2;

    x *= 0x9E3779B97F4A7C15ULL;
    return &buckets[(x >> 58) & (BUCKETS - 1)];
}

static bool ready(PalErrno *err) {
    (void)pthread_once(&setup_once, setup);
    if (!setup_ok) {
        BURROW_OUT(err, PAL_ENOMEM);
        return false;
    }
    return true;
}

/* One wait of at most left_ns nanoseconds on an already locked bucket. The
 * result is ignored on purpose: the caller recomputes what is left of its own
 * deadline every time round and decides from that, so whether this returned
 * because of a signal, a timeout or nothing at all makes no difference. */
static void wait_for(Bucket *b, int64_t left_ns) {
    struct timespec ts;

#if FUTEX_COND_RELATIVE
    ts.tv_sec = (time_t)(left_ns / 1000000000);
    ts.tv_nsec = (long)(left_ns % 1000000000);

    (void)pthread_cond_timedwait_relative_np(&b->cv, &b->mu, &ts);
#else
    /* Absolute, and on the same clock the condition variable was given in
     * setup, which is the clock pal_clock_monotonic reads, so the deadline goes
     * over as it stands rather than being converted through anything. */
    int64_t at = pal_clock_monotonic() + left_ns;

    ts.tv_sec = (time_t)(at / 1000000000);
    ts.tv_nsec = (long)(at % 1000000000);

    (void)pthread_cond_timedwait(&b->cv, &b->mu, &ts);
#endif
}

/* When a wait of ns nanoseconds starting now runs out.
 *
 * The guard is there because the duration comes from a timer and a timer can be
 * set for the end of the clock. Signed overflow is undefined behaviour, so this
 * is not a wrong answer that gets clamped later, it is a program the compiler
 * may do anything with. Landing on INT64_MAX is also the right answer: each
 * individual wait is capped anyway, so a deadline at the end of the clock is a
 * loop that goes back to sleep for another thousand seconds. */
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

    if (!ready(err))
        return false;

    /* Before the lock, because a word that has already changed is the common
     * case for a caller that got here by losing a race and there is no reason
     * to make it queue first. */
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

    (void)pthread_mutex_lock(&b->mu);

    /* Again, and this time under the lock, which is what makes a wake that
     * lands between the check above and the queueing below impossible to lose.
     * The waker stores its word and then takes this same lock, so it either
     * gets here first and this load sees it, or it gets here second and finds
     * the record already on the list. */
    if (burrow__atomic_load_acquire_u32(addr) != expect) {
        (void)pthread_mutex_unlock(&b->mu);
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
        if (deadline < 0) {
            (void)pthread_cond_wait(&b->cv, &b->mu);
        } else {
            int64_t left = deadline - pal_clock_monotonic();
            if (left <= 0)
                break;

            if (left > FUTEX_MAX_WAIT_NS)
                left = FUTEX_MAX_WAIT_NS;

            wait_for(b, left);
        }

        if (w.signalled) {
            woken = true;
            break;
        }

        /* Somebody else's address shares this bucket, or the condition variable
         * woke for no reason. Either way the word is what decides, and if it
         * has moved on there is nothing left to wait for. */
        if (burrow__atomic_load_acquire_u32(addr) != expect) {
            woken = true;
            break;
        }
    }

    unlink_waiter(b, &w);
    (void)pthread_mutex_unlock(&b->mu);

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

    if (!ready(err))
        return -1;

    Bucket *b = bucket_of(addr);

    (void)pthread_mutex_lock(&b->mu);

    int64_t woken = 0;
    for (Waiter *w = b->head; w != NULL && woken < n; w = w->next) {
        if (w->addr != addr || w->signalled)
            continue;

        w->signalled = true;
        woken++;
    }

    /* Broadcast rather than signal, even when only one waiter was marked,
     * because the condition variable has no idea which record belongs to which
     * sleeper and waking the wrong one would leave the marked one asleep. The
     * ones that were not marked look at their record, look at their word, and
     * go straight back down. */
    if (woken > 0)
        (void)pthread_cond_broadcast(&b->cv);

    (void)pthread_mutex_unlock(&b->mu);

    BURROW_OUT(err, PAL_OK);
    return woken;
}

#endif /* !BURROW_OS_WINDOWS && !BURROW_OS_LINUX */
