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
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/note.h"

#include "burrow/atomic.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#elif defined(BURROW_OS_LINUX)
#include <sys/syscall.h>
#else
#include <pthread.h>
#endif

#if defined(BURROW_OS_WINDOWS)

/* ------------------------------------------------------------------ windows */

/* A manual reset event, which is a one shot gate with a different name. Set
 * opens it and leaves it open, reset closes it, and a wait on an open one
 * returns without blocking, which is the behaviour a note wants from every
 * operation without any help. It is also what Go uses here.
 *
 * WaitOnAddress would be the closer match to a futex and is deliberately not
 * used: it lives in synchronization.lib rather than kernel32, and burrow links
 * nothing today, which is worth more than the handle this saves. */

bool burrow__note_init(burrow__Note *n) {
    /* No security attributes, manual reset, starts closed, no name. */
    n->event = (void *)CreateEventW(NULL, TRUE, FALSE, NULL);
    return n->event != NULL;
}

void burrow__note_free(burrow__Note *n) {
    if (n->event != NULL) {
        (void)CloseHandle((HANDLE)n->event);
        n->event = NULL;
    }
}

void burrow__note_clear(burrow__Note *n) {
    (void)ResetEvent((HANDLE)n->event);
}

void burrow__note_wake(burrow__Note *n) {
    (void)SetEvent((HANDLE)n->event);
}

void burrow__note_sleep(burrow__Note *n) {
    /* Manual reset means every waiter is released and stays released, so there
     * is no loop to write here. A failure return would be a closed handle,
     * which is a caller bug rather than something to retry. */
    (void)WaitForSingleObject((HANDLE)n->event, INFINITE);
}

bool burrow__note_is_open(const burrow__Note *n) {
    /* A zero timeout is the documented way to ask an event its state without
     * waiting on it, and on a manual reset event it does not consume anything. */
    return WaitForSingleObject((HANDLE)n->event, 0) == WAIT_OBJECT_0;
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

/* Which system call number to use. Architectures added after 2019 have no 32
 * bit time_t and so have only the time64 variant, while everything older has
 * both and the plain one is what its libc uses. The timeout argument is always
 * NULL here, so the two are interchangeable for this use. */
#if defined(SYS_futex)
#define NOTE_SYS_FUTEX SYS_futex
#elif defined(SYS_futex_time64)
#define NOTE_SYS_FUTEX SYS_futex_time64
#else
#error "no futex system call number on this Linux architecture"
#endif

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

static void futex_wake_all(uint32_t *addr) {
    (void)syscall(NOTE_SYS_FUTEX, addr, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0);
}

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;
    return true;
}

void burrow__note_free(burrow__Note *n) {
    /* Nothing was ever allocated. The store is here so that a use after free
     * finds a closed gate and hangs where a debugger can see it, rather than
     * finding whatever the memory is reused for and carrying on. */
    n->state = 0;
}

void burrow__note_clear(burrow__Note *n) {
    burrow__atomic_store_release_u32(&n->state, 0);
}

void burrow__note_wake(burrow__Note *n) {
    burrow__atomic_store_release_u32(&n->state, 1);
    /* Unconditionally, because there is no way to know whether anybody is
     * queued without asking the kernel, and asking costs the same as telling.
     * A wake with no waiters returns zero and does nothing else. */
    futex_wake_all(&n->state);
}

void burrow__note_sleep(burrow__Note *n) {
    while (burrow__atomic_load_acquire_u32(&n->state) == 0) {
        /* The race between the load above and this call is what the expected
         * value argument is for: the kernel rechecks the word under its own
         * lock and returns straight away if the waker got there in between. */
        futex_wait(&n->state, 0);
    }
}

bool burrow__note_is_open(const burrow__Note *n) {
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

#else

/* -------------------------------------------------------------------- posix */

/* A mutex and a condition variable, which is what everything without a futex
 * has. macOS is the one that matters, and it is what Go uses there too.
 *
 * The flag is still read and written atomically even though every write happens
 * under the mutex. That is not belt and braces, it is what lets is_open answer
 * without taking the lock, which in turn is what keeps it usable from a caller
 * that must not block. */

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;

    if (pthread_mutex_init(&n->mu, NULL) != 0)
        return false;

    if (pthread_cond_init(&n->cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&n->mu);
        return false;
    }

    return true;
}

void burrow__note_free(burrow__Note *n) {
    (void)pthread_cond_destroy(&n->cv);
    (void)pthread_mutex_destroy(&n->mu);
    n->state = 0;
}

void burrow__note_clear(burrow__Note *n) {
    (void)pthread_mutex_lock(&n->mu);
    burrow__atomic_store_release_u32(&n->state, 0);
    (void)pthread_mutex_unlock(&n->mu);
}

void burrow__note_wake(burrow__Note *n) {
    (void)pthread_mutex_lock(&n->mu);
    burrow__atomic_store_release_u32(&n->state, 1);
    /* Broadcast rather than signal, because a note releases everybody. Signal
     * here would leave every sleeper but one waiting for a second wake that is
     * never coming. */
    (void)pthread_cond_broadcast(&n->cv);
    (void)pthread_mutex_unlock(&n->mu);
}

void burrow__note_sleep(burrow__Note *n) {
    (void)pthread_mutex_lock(&n->mu);
    while (burrow__atomic_load_acquire_u32(&n->state) == 0)
        (void)pthread_cond_wait(&n->cv, &n->mu);
    (void)pthread_mutex_unlock(&n->mu);
}

bool burrow__note_is_open(const burrow__Note *n) {
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

#endif
