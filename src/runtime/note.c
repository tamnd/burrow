/* The three ways a thread goes to sleep. See burrow/note.h for what a note is
 * and what the rules for using one are.
 *
 * All three keep the same shape, and it is worth saying what it is once rather
 * than three times. The open flag is written with a release store and read with
 * an acquire load, so that everything the waker did before it opened the gate is
 * visible to the sleeper once the sleeper sees it open. The sleeper always
 * checks the flag before it waits and again every time it wakes, so a wake that
 * arrives before the sleep is not lost and a wakeup nobody asked for is not
 * mistaken for one. That loop is the whole of the correctness argument and the
 * rest is which system call does the sleeping.
 *
 * The timed sleep adds one more thing to that shape and it is also worth saying
 * once. Every backend loops, and every time round the loop it works out what is
 * left of the timeout from burrow__nanotime rather than waiting the original
 * amount again, so a sleep that is interrupted nine times still waits the length
 * it was asked for. Each individual wait is capped at NOTE_MAX_WAIT_NS below,
 * which costs one extra system call every quarter of an hour and in exchange
 * makes every conversion from nanoseconds into whatever the platform counts in
 * something that cannot overflow rather than something that usually does not.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* For CLOCK_MONOTONIC and pthread_condattr_setclock, which the portable backend
 * needs and which are POSIX rather than C. Not on macOS, where the default
 * visibility is everything and asking for POSIX instead takes away the _np call
 * that backend uses, and not on Linux, which never reaches that backend and
 * whose futex path deliberately declares syscall itself. */
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "burrow/note.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#elif defined(BURROW_OS_LINUX)
#include <sys/syscall.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* The longest any single wait below may be, a thousand seconds. See the note at
 * the top of the file for why there is a cap at all. */
#define NOTE_MAX_WAIT_NS 1000000000000LL

/* The sleeper count, which all three backends now keep and all three read before
 * they go near the kernel.
 *
 * Waking a note used to go into the kernel every single time, on the reasoning
 * that there is no way to know whether anybody is queued without asking, and
 * that asking costs what telling costs. That is true of the kernel and it is not
 * true of a word the sleepers write to on their way past. A wake can read that
 * word, find nobody, and stay in user space, which is the common case by a long
 * way: a scheduler opens gates nobody is standing at all day long. Pinned on an
 * EPYC, a wake and a walk through the gate went from 352 nanoseconds to single
 * digits. Go's runtime does not do this for its own notes, and Go's
 * sync.WaitGroup does exactly this in the word next to its counter.
 *
 * Nothing is lost by doing it, and the argument is worth writing out once
 * because it is the only subtle thing in this file.
 *
 * A sleeper joins the count and then reads the flag. A waker sets the flag and
 * then reads the count. All four of those are sequentially consistent, so they
 * all appear in one order that every thread agrees on, and in any such order the
 * two writes cannot both come after the two reads. Whichever write goes first is
 * seen by the read that follows it. So either the sleeper sees an open gate and
 * does not sleep, or the waker sees a sleeper and calls it, and possibly both.
 * Never neither. This is Dekker's algorithm with the two flags being the gate
 * and the count, and it is the only place in burrow that needs a sequentially
 * consistent ordering rather than an acquire and release pair.
 *
 * The count is a second word rather than spare bits in the flag, which costs
 * four bytes and is worth them. The flag is what a futex compares against, and a
 * futex that finds a different value than the caller expected returns rather
 * than sleeping. Sharing the word would mean every thread arriving at the gate
 * changed the value every thread already asleep was waiting on, so a crowd of
 * sixty four threads would wake each other up for no reason a few thousand
 * times on the way in. A flag that only ever holds nought or one cannot do
 * that. */

#if defined(BURROW_OS_WINDOWS)

/* ------------------------------------------------------------------ windows */

/* A manual reset event, which is a one shot gate with a different name. Set
 * opens it and leaves it open, reset closes it, and a wait on an open one
 * returns without blocking, which is the behaviour a note wants from every
 * operation without any help. It is also what Go uses here.
 *
 * WaitOnAddress would be the closer match to a futex and is deliberately not
 * used: it lives in synchronization.lib rather than kernel32, and burrow links
 * nothing today, which is worth more than the handle this saves.
 *
 * The state word sits in front of the event and answers everything the event
 * does not have to be asked. Every operation here was a system call before it
 * arrived, including asking a gate whether it is open, and the gate is usually
 * open and usually has nobody at it. Now the event is only touched when a
 * thread really does have to wait and when somebody really is waiting. */

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;
    n->waiters = 0;
    /* No security attributes, manual reset, starts closed, no name. */
    n->event = (void *)CreateEventW(NULL, TRUE, FALSE, NULL);
    return n->event != NULL;
}

void burrow__note_free(burrow__Note *n) {
    n->state = 0;
    n->waiters = 0;

    if (n->event != NULL) {
        (void)CloseHandle((HANDLE)n->event);
        n->event = NULL;
    }
}

void burrow__note_clear(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 0);
    /* The event has to be reset too, because a wake with a sleeper on it set
     * the event and a manual reset one stays set until it is told not to be. */
    (void)ResetEvent((HANDLE)n->event);
}

void burrow__note_wake(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 1);

    /* Same argument as the other two backends, written out at the top of the
     * file. A sleeper on its way in joins the count before it looks at the
     * flag, so a count of nothing here means nothing is in the kernel. */
    if (burrow__atomic_load_u32(&n->waiters) != 0)
        (void)SetEvent((HANDLE)n->event);
}

void burrow__note_sleep(burrow__Note *n) {
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return;

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    /* Read again now that the count is joined, because the wake this is racing
     * may have looked and found nobody in between. */
    while (burrow__atomic_load_u32(&n->state) == 0) {
        /* Manual reset means every waiter is released and stays released. A
         * failure return would be a closed handle, which is a caller bug rather
         * than something to retry, and the loop above would spin on it, so the
         * result is looked at. */
        if (WaitForSingleObject((HANDLE)n->event, INFINITE) != WAIT_OBJECT_0)
            break;
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);
}

bool burrow__note_sleep_timeout(burrow__Note *n, int64_t ns) {
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return true;

    if (ns <= 0)
        return false;

    int64_t deadline = burrow__nanotime() + ns;
    int64_t left = ns;

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    for (;;) {
        if (burrow__atomic_load_u32(&n->state) != 0)
            break;

        if (left > NOTE_MAX_WAIT_NS)
            left = NOTE_MAX_WAIT_NS;

        /* Rounded up rather than down, because a wait of zero milliseconds does
         * not wait and a loop of those is a spin with the word timeout on it.
         * The resolution here is the timer tick, which is about sixteen
         * milliseconds by default, so a short timeout on Windows is late by a
         * lot. That is a thing about Windows and not about this function. */
        DWORD ms = (DWORD)((left + 999999) / 1000000);

        DWORD r = WaitForSingleObject((HANDLE)n->event, ms);
        if (r == WAIT_OBJECT_0)
            break;

        /* Anything other than a timeout is a handle that is closed or not an
         * event, which is a caller bug rather than something to spin on. */
        if (r != WAIT_TIMEOUT)
            break;

        left = deadline - burrow__nanotime();
        if (left <= 0)
            break;
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);

    /* Read rather than returning what the loop decided, because the wake can
     * land between the last check and the deadline passing, and a note that is
     * open is open however late the thread noticed. */
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

bool burrow__note_is_open(const burrow__Note *n) {
    /* The flag rather than the event, which used to be a wait of zero
     * milliseconds and therefore a system call to read one bit. */
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

#elif defined(BURROW_OS_LINUX)

/* -------------------------------------------------------------------- linux */

/* A futex. The whole note is one 32 bit word of the caller's own memory, and
 * the kernel only learns it exists when a thread actually has to sleep on it,
 * which means an uncontended note costs two atomic operations and no system
 * call at all. That is why this backend exists rather than using the portable
 * one everywhere.
 *
 * The constants are written out rather than included. FUTEX_WAIT and FUTEX_WAKE
 * are 0 and 1, and the PRIVATE flag is 128, which says the futex is never shared
 * between processes and lets the kernel skip looking the page up in the shared
 * mapping table. <linux/futex.h> has them, and pulling a kernel header into a
 * userspace build is the kind of thing that works until the day somebody builds
 * against a different kernel's headers. These three numbers are part of the
 * system call interface and cannot change. */
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* Which system call number to use, and what shape the timeout that goes with it
 * has. Architectures added after 2019 have no 32 bit time_t and so have only the
 * time64 variant, while everything older has both and the plain one is what its
 * libc uses.
 *
 * The timespec is written out here rather than taken from <time.h> because the
 * two do not have to agree. SYS_futex wants the kernel's old timespec, whose
 * fields are both a long, and a 32 bit build with _TIME_BITS=64 has a libc
 * timespec whose seconds field is eight bytes wide. Handing the second to the
 * first is a struct the kernel reads the wrong way, and it is the sort of
 * mistake that only appears on the one platform nobody builds for. Writing the
 * layout that goes with the chosen system call number keeps the two together. */
#if defined(SYS_futex)
#define NOTE_SYS_FUTEX SYS_futex
typedef long NoteTime;
#elif defined(SYS_futex_time64)
#define NOTE_SYS_FUTEX SYS_futex_time64
typedef int64_t NoteTime;
#else
#error "no futex system call number on this Linux architecture"
#endif

typedef struct NoteTimespec {
    NoteTime tv_sec;
    NoteTime tv_nsec;
} NoteTimespec;

/* Declared here rather than taken from <unistd.h>, because both glibc and musl
 * hide syscall behind _GNU_SOURCE and burrow is built as strict C11. The
 * prototype is the same one both of them use and it is fixed by the ABI, so
 * writing it out is not a guess. This is the same reasoning as the feature
 * macro note in rand.c: pick what is declared without a feature macro, and
 * where nothing is, say what the ABI already says. */
extern long syscall(long number, ...);

static void futex_wait(uint32_t *addr, uint32_t expected) {
    /* Returns an error for a value that changed, a signal, or a timeout, and
     * the caller's loop handles all three the same way by looking at the flag
     * again. There is nothing here worth branching on. */
    (void)syscall(NOTE_SYS_FUTEX, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

/* The same wait with a deadline on it. The kernel reads the timespec as a
 * duration and measures it on CLOCK_MONOTONIC, which is the clock
 * burrow__nanotime reads on this platform, so the two agree about how long a
 * second is without anybody having to convert between them. */
static void futex_wait_for(uint32_t *addr, uint32_t expected, int64_t ns) {
    NoteTimespec ts;

    ts.tv_sec = (NoteTime)(ns / 1000000000);
    ts.tv_nsec = (NoteTime)(ns % 1000000000);

    (void)syscall(NOTE_SYS_FUTEX, addr, FUTEX_WAIT_PRIVATE, expected, &ts, NULL, 0);
}

static void futex_wake_all(uint32_t *addr) {
    (void)syscall(NOTE_SYS_FUTEX, addr, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0);
}

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;
    n->waiters = 0;
    return true;
}

void burrow__note_free(burrow__Note *n) {
    /* Nothing was ever allocated. The stores are here so that a use after free
     * finds a closed gate with nobody at it and hangs where a debugger can see
     * it, rather than finding whatever the memory is reused for and carrying
     * on. */
    n->state = 0;
    n->waiters = 0;
}

void burrow__note_clear(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 0);
}

void burrow__note_wake(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 1);

    /* Only if somebody said they were going to sleep. See the top of the file
     * for why reading the count here cannot miss a sleeper on its way in. */
    if (burrow__atomic_load_u32(&n->waiters) != 0)
        futex_wake_all(&n->state);
}

void burrow__note_sleep(burrow__Note *n) {
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return;

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    while (burrow__atomic_load_u32(&n->state) == 0) {
        /* The race between the load above and this call is what the expected
         * value argument is for: the kernel rechecks the word under its own
         * lock and returns straight away if the waker got there in between. */
        futex_wait(&n->state, 0);
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);
}

bool burrow__note_sleep_timeout(burrow__Note *n, int64_t ns) {
    /* The note is read before the clock is. The caller here is a thread that
     * has just failed to find work and is parking with a deadline on it, and by
     * then the wake it was racing has usually already landed, so the common
     * case returns without ever asking what time it is. Asking first put a vdso
     * call in front of every one of those, which is what the note_timeout_hit
     * row in burrow-bench is there to catch. */
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return true;

    if (ns <= 0)
        return false;

    int64_t deadline = burrow__nanotime() + ns;

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    while (burrow__atomic_load_u32(&n->state) == 0) {
        int64_t left = deadline - burrow__nanotime();
        if (left <= 0)
            break;

        if (left > NOTE_MAX_WAIT_NS)
            left = NOTE_MAX_WAIT_NS;

        futex_wait_for(&n->state, 0, left);
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);

    /* Read again rather than returning what the loop decided, because the wake
     * can land between the last check and the deadline passing, and a note that
     * is open is open however late the thread noticed. */
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

bool burrow__note_is_open(const burrow__Note *n) {
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

#else

/* -------------------------------------------------------------------- posix */

/* A mutex and a condition variable, which is what everything without a futex
 * has. macOS is the one that matters, and it is what Go uses there too.
 *
 * The flag lives outside the mutex here, the same as it does on the other two
 * backends, and that is what lets is_open and an already open sleep answer
 * without taking the lock. The broadcast is still inside the mutex, because
 * that is what stops a wake slipping between a sleeper's last look at the flag
 * and the moment it is queued on the condition variable: the sleeper holds the
 * lock across both, so a waker that gets as far as broadcasting has either
 * already been seen or is still waiting for the lock.
 *
 * The timed wait is the one place where this backend is two backends. A
 * condition variable measures an absolute deadline on CLOCK_REALTIME unless it
 * is told otherwise, and CLOCK_REALTIME is the wall clock, which is the clock a
 * timeout must not be measured on. Every POSIX system since 2001 can be told
 * otherwise with pthread_condattr_setclock, and macOS is the exception: it has
 * never implemented that call and offers pthread_cond_timedwait_relative_np
 * instead, which takes a duration and measures it on the monotonic clock
 * already. Go does exactly this split for exactly this reason. */

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define NOTE_COND_RELATIVE 1
#else
#define NOTE_COND_RELATIVE 0
#endif

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;
    n->waiters = 0;

    if (pthread_mutex_init(&n->mu, NULL) != 0)
        return false;

#if NOTE_COND_RELATIVE
    if (pthread_cond_init(&n->cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&n->mu);
        return false;
    }
#else
    pthread_condattr_t attr;

    if (pthread_condattr_init(&attr) != 0) {
        (void)pthread_mutex_destroy(&n->mu);
        return false;
    }

    /* A failure here is a system that has the call and will not do it, which
     * leaves the condvar on the wall clock. Better to refuse to make the note
     * than to hand back one whose timeouts are wrong twice a year. */
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&n->cv, &attr) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&n->mu);
        return false;
    }

    (void)pthread_condattr_destroy(&attr);
#endif

    return true;
}

void burrow__note_free(burrow__Note *n) {
    (void)pthread_cond_destroy(&n->cv);
    (void)pthread_mutex_destroy(&n->mu);
    n->state = 0;
    n->waiters = 0;
}

void burrow__note_clear(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 0);
}

void burrow__note_wake(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 1);

    /* Nobody sleeping means nothing to do, and in particular no mutex to take.
     * See the top of the file for why reading the count here cannot miss a
     * sleeper on its way in.
     *
     * The mutex is still what makes the condition variable work, and the wait
     * below is still inside it. This only skips the case where there is nothing
     * to signal, and taking a lock in order to shout into an empty room is the
     * cost being removed. */
    if (burrow__atomic_load_u32(&n->waiters) == 0)
        return;

    (void)pthread_mutex_lock(&n->mu);
    /* Broadcast rather than signal, because a note releases everybody. Signal
     * here would leave every sleeper but one waiting for a second wake that is
     * never coming. */
    (void)pthread_cond_broadcast(&n->cv);
    (void)pthread_mutex_unlock(&n->mu);
}

void burrow__note_sleep(burrow__Note *n) {
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return;

    /* Joining the count happens before the mutex is taken and leaving it after
     * the mutex is dropped, so that a waker deciding whether to call never has
     * to wait for this thread to get out of the way first. */
    (void)burrow__atomic_add_u32(&n->waiters, 1);

    (void)pthread_mutex_lock(&n->mu);
    while (burrow__atomic_load_u32(&n->state) == 0)
        (void)pthread_cond_wait(&n->cv, &n->mu);
    (void)pthread_mutex_unlock(&n->mu);

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);
}

bool burrow__note_sleep_timeout(burrow__Note *n, int64_t ns) {
    /* Same order as the futex version above, and here it saves the mutex as
     * well as the clock. A note that is already open is a fact the word can
     * report on its own, and a thread that is not going to wait has no business
     * joining the count or taking the lock. */
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return true;

    if (ns <= 0)
        return false;

    int64_t deadline = burrow__nanotime() + ns;

    (void)burrow__atomic_add_u32(&n->waiters, 1);
    (void)pthread_mutex_lock(&n->mu);

    while (burrow__atomic_load_u32(&n->state) == 0) {
        int64_t left = deadline - burrow__nanotime();
        if (left <= 0)
            break;

        if (left > NOTE_MAX_WAIT_NS)
            left = NOTE_MAX_WAIT_NS;

#if NOTE_COND_RELATIVE
        struct timespec wait;
        wait.tv_sec = (time_t)(left / 1000000000);
        wait.tv_nsec = (long)(left % 1000000000);

        (void)pthread_cond_timedwait_relative_np(&n->cv, &n->mu, &wait);
#else
        /* Absolute, and on the same clock the condvar was given in init, which
         * is the clock burrow__nanotime reads on every platform that gets here.
         * So the deadline can be handed over as it stands rather than being
         * converted through anything. */
        int64_t at = burrow__nanotime() + left;

        struct timespec wait;
        wait.tv_sec = (time_t)(at / 1000000000);
        wait.tv_nsec = (long)(at % 1000000000);

        (void)pthread_cond_timedwait(&n->cv, &n->mu, &wait);
#endif
    }

    bool open = burrow__atomic_load_acquire_u32(&n->state) != 0;

    (void)pthread_mutex_unlock(&n->mu);
    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);
    return open;
}

bool burrow__note_is_open(const burrow__Note *n) {
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

#endif
