/* The queue between a signal handler and os/signal.
 *
 * The handler and the reader share three words of mask and one word of state,
 * and the state is the whole protocol. The reader says it is Receiving before
 * it sleeps. A sender that finds it Receiving swaps it to Idle and wakes it, so
 * there is one wake for every sleep. A sender that finds it Idle swaps it to
 * Sending, which the reader sees on its way to sleep and takes as a reason not
 * to. Either way the bit went into the mask first, so the reader finds it.
 *
 * Go puts the sleeping on a runtime note. The note here is the one the platform
 * layer keeps for this, a pipe on POSIX and an event on Windows, because the
 * handler is the waker and a note built on a mutex is not something a handler
 * may touch. Go makes the same choice on macOS for the same reason.
 *
 * Derived from Go's src/runtime/sigqueue.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sigqueue.h"

#include "burrow/atomic.h"
#include "burrow/pal.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <stdbool.h>
#include <stdint.h>

enum { SIGQ_WORDS = BURROW_SIGQUEUE_NSIG / 32 };

enum { SIGQ_IDLE = 0, SIGQ_RECEIVING = 1, SIGQ_SENDING = 2 };

/* Go's sig struct. mask is what the handler has queued and recv is what the
 * reader has taken out of it and not handed back yet, which only the reader
 * touches. wanted and ignored are written by os/signal under its own lock and
 * read from the handler, so they are atomic. */
typedef struct SigQueue {
    uint32_t mask[SIGQ_WORDS];
    uint32_t wanted[SIGQ_WORDS];
    uint32_t ignored[SIGQ_WORDS];
    uint32_t recv[SIGQ_WORDS];
    uint32_t state;
    uint32_t delivering;
    /* 0 until the first enable, 1 while that enable sets the platform up, and
     * 2 once it has. Go's inuse, made safe to read without os/signal's lock. */
    uint32_t inuse;
    /* The same three steps for reading what SIGHUP and SIGINT were doing when
     * the program started, which Go's initsig does before main. */
    uint32_t initial;
} SigQueue;

static SigQueue sigq;

static void sigq_yield(void) {
    if (sched_current() != NULL)
        runtime_gosched();
    else
        pal_thread_yield();
}

/* Go's sigInitIgnored, done the first time anything here is asked rather than
 * before main, since a C program has no before main to put it in. nohup leaves
 * SIGHUP ignored, a shell leaves SIGINT ignored for a background job, and a
 * program that is run that way is supposed to stay that way until it says
 * otherwise. */
static void sigq_init(void) {
    uint32_t want = 0;
    if (burrow__atomic_load_acquire_u32(&sigq.initial) == 2)
        return;
    if (!burrow__atomic_cas_u32(&sigq.initial, &want, 1)) {
        while (burrow__atomic_load_acquire_u32(&sigq.initial) != 2)
            sigq_yield();
        return;
    }

    uint32_t bits = 0;
    if (pal_signal_ignored(1))
        bits |= 1U << 1;
    if (pal_signal_ignored(2))
        bits |= 1U << 2;
    if (bits != 0)
        (void)burrow__atomic_or_u32(&sigq.ignored[0], bits);
    burrow__atomic_store_release_u32(&sigq.initial, 2);
}

bool burrow__signal_send(uint32_t s) {
    if (s >= (uint32_t)BURROW_SIGQUEUE_NSIG)
        return false;

    uint32_t bit = 1U << (s & 31U);
    uint32_t *word = &sigq.mask[s / 32U];

    (void)burrow__atomic_add_u32(&sigq.delivering, 1);

    if ((burrow__atomic_load_u32(&sigq.wanted[s / 32U]) & bit) == 0) {
        (void)burrow__atomic_add_u32(&sigq.delivering, (uint32_t)-1);
        return false;
    }

    /* Set the bit, and stop here if it was already set: the reader has not
     * seen the last one yet and will see this one with it. */
    uint32_t mask = burrow__atomic_load_u32(word);
    for (;;) {
        if ((mask & bit) != 0) {
            (void)burrow__atomic_add_u32(&sigq.delivering, (uint32_t)-1);
            return true;
        }
        if (burrow__atomic_cas_u32(word, &mask, mask | bit))
            break;
    }

    for (;;) {
        uint32_t st = burrow__atomic_load_u32(&sigq.state);
        if (st == SIGQ_IDLE) {
            if (burrow__atomic_cas_u32(&sigq.state, &st, SIGQ_SENDING))
                break;
        } else if (st == SIGQ_SENDING) {
            break;
        } else if (st == SIGQ_RECEIVING) {
            if (burrow__atomic_cas_u32(&sigq.state, &st, SIGQ_IDLE)) {
                pal_signal_note_wake();
                break;
            }
        } else {
            runtime_throw(BURROW_S("sigsend: inconsistent state"));
        }
    }

    (void)burrow__atomic_add_u32(&sigq.delivering, (uint32_t)-1);
    return true;
}

uint32_t burrow__signal_recv(void) {
    for (;;) {
        for (uint32_t i = 0; i < (uint32_t)BURROW_SIGQUEUE_NSIG; i++) {
            uint32_t bit = 1U << (i & 31U);
            if ((sigq.recv[i / 32U] & bit) != 0) {
                sigq.recv[i / 32U] &= ~bit;
                return i;
            }
        }

        for (;;) {
            uint32_t st = burrow__atomic_load_u32(&sigq.state);
            if (st == SIGQ_IDLE) {
                if (burrow__atomic_cas_u32(&sigq.state, &st, SIGQ_RECEIVING)) {
                    pal_signal_note_sleep();
                    break;
                }
            } else if (st == SIGQ_SENDING) {
                if (burrow__atomic_cas_u32(&sigq.state, &st, SIGQ_IDLE))
                    break;
            } else {
                runtime_throw(BURROW_S("signal_recv: inconsistent state"));
            }
        }

        for (int i = 0; i < SIGQ_WORDS; i++)
            sigq.recv[i] = burrow__atomic_swap_u32(&sigq.mask[i], 0);
    }
}

void burrow__signal_wait_until_idle(void) {
    /* Nothing has ever been relayed, so there is no reader to wait for, and
     * Go's version would spin forever here: its reader starts with the
     * package, and this one starts with the first Notify. */
    if (burrow__atomic_load_acquire_u32(&sigq.inuse) != 2)
        return;

#if defined(BURROW_OS_WASI)
    /* os/signal starts no reader on wasip1, because no signal ever arrives
     * there, so the state never gets to RECEIVING and the loop below would
     * give way for ever. With nothing ever sent there is nothing to drain. */
    return;
#endif

    while (burrow__atomic_load_u32(&sigq.delivering) != 0)
        sigq_yield();
    while (burrow__atomic_load_u32(&sigq.state) != SIGQ_RECEIVING)
        sigq_yield();
}

/* What the platform layer calls, which speaks in its own signed numbers. */
static bool sigq_relay(int32_t native) {
    return native >= 0 && burrow__signal_send((uint32_t)native);
}

/* Gives the platform layer the function to call from its handler, once. A
 * platform that cannot set relaying up has nothing os/signal can stand on, so
 * that is fatal, as a runtime that could not start would be. */
static void sigq_start(void) {
    uint32_t want = 0;
    if (burrow__atomic_load_acquire_u32(&sigq.inuse) == 2)
        return;
    if (!burrow__atomic_cas_u32(&sigq.inuse, &want, 1)) {
        while (burrow__atomic_load_acquire_u32(&sigq.inuse) != 2)
            sigq_yield();
        return;
    }

    PalErrno err = PAL_OK;
    if (!pal_signal_relay_init(sigq_relay, &err))
        runtime_throw(BURROW_S("os/signal: cannot relay signals on this system"));
    burrow__atomic_store_release_u32(&sigq.inuse, 2);
}

void burrow__signal_enable(uint32_t s) {
    sigq_init();
    sigq_start();
    if (s >= (uint32_t)BURROW_SIGQUEUE_NSIG)
        return;

    uint32_t bit = 1U << (s & 31U);
    (void)burrow__atomic_or_u32(&sigq.wanted[s / 32U], bit);
    (void)burrow__atomic_and_u32(&sigq.ignored[s / 32U], ~bit);

    /* A signal the platform will not hand over, SIGKILL or a fault, is wanted
     * and never arrives, which is what Go does with it too. */
    PalErrno err = PAL_OK;
    (void)pal_signal_relay((int32_t)s, PAL_RELAY_CATCH, &err);
}

void burrow__signal_disable(uint32_t s) {
    sigq_init();
    if (s >= (uint32_t)BURROW_SIGQUEUE_NSIG)
        return;

    PalErrno err = PAL_OK;
    (void)pal_signal_relay((int32_t)s, PAL_RELAY_DEFAULT, &err);
    (void)burrow__atomic_and_u32(&sigq.wanted[s / 32U], ~(1U << (s & 31U)));
}

void burrow__signal_ignore(uint32_t s) {
    sigq_init();
    if (s >= (uint32_t)BURROW_SIGQUEUE_NSIG)
        return;

    PalErrno err = PAL_OK;
    (void)pal_signal_relay((int32_t)s, PAL_RELAY_IGNORE, &err);

    uint32_t bit = 1U << (s & 31U);
    (void)burrow__atomic_and_u32(&sigq.wanted[s / 32U], ~bit);
    (void)burrow__atomic_or_u32(&sigq.ignored[s / 32U], bit);
}

bool burrow__signal_ignored(uint32_t s) {
    sigq_init();
    if (s >= (uint32_t)BURROW_SIGQUEUE_NSIG)
        return false;
    return (burrow__atomic_load_u32(&sigq.ignored[s / 32U]) & (1U << (s & 31U))) != 0;
}
