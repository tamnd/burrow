/* Putting a thread to sleep and waking it up again.
 *
 * This is the one place in burrow where a thread genuinely blocks in the
 * kernel. Everything else that looks like blocking, a goroutine waiting on a
 * channel or on a mutex, parks the goroutine and hands the thread to somebody
 * else, and that machinery is built on top of this. An M with no work left runs
 * out of things to do eventually, and when it does it sleeps here.
 *
 * A note is a one shot gate. It starts closed, a wake opens it, and anybody who
 * sleeps on a closed one waits until it opens. Opening a note that is already
 * open does nothing, and sleeping on one that is already open returns straight
 * away, which is the property that makes it usable without a lock around it:
 * the waker never has to know whether the sleeper has arrived yet.
 *
 *     burrow__note_clear(&n);
 *     burrow__thread_start(&t, worker, &n, 0);
 *     burrow__note_sleep(&n);    // until worker calls burrow__note_wake(&n)
 *
 * The name is Go's. runtime.note is the same object with the same three
 * operations, and this is a port, so somebody reading Go's scheduler next to
 * this one should find the same word for the same thing.
 *
 * One implementation, over pal_futex_wait and pal_futex_wake. There used to be
 * three, one per platform, and the three of them agreed on everything except
 * which call does the sleeping, which is exactly the thing the platform layer
 * is for. A note is now three words and a flag on every platform, it owns
 * nothing the system has to hand out, and the init that used to be able to fail
 * on two of the three cannot fail anywhere.
 *
 * There is a timed sleep as well, and it waits on burrow's monotonic clock
 * rather than on the system time. That distinction is the whole reason it waited
 * for burrow/clock.h: a timeout measured against a clock somebody can set is a
 * timeout that fires twice or never on the day the machine syncs with ntp.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_NOTE_H
#define BURROW_NOTE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A note. The fields are here so that one can sit inside an M without an
 * allocation, and they are not part of the interface. */
typedef struct burrow__Note {
    /* 0 while the gate is closed and 1 once it is open. The address of this
     * word is what sleepers are queued against. */
    uint32_t state;
    /* How many threads are about to sleep or are asleep, which is what lets a
     * wake with nobody waiting stay out of the kernel. A separate word from the
     * gate because the gate is what a wait compares against, and a value that
     * changed every time somebody arrived would keep waking the ones who are
     * already there. */
    uint32_t waiters;
    /* How many threads are inside a wake right now, and whether anybody is
     * counting. See burrow__note_init_transient. */
    uint32_t wakers;
    bool transient;
} burrow__Note;

/* Gets the note ready to use and leaves it closed.
 *
 * It returns a bool and the bool is always true. A note used to own a kernel
 * object on two of the three platforms and the system could refuse to hand one
 * over; it owns nothing now, on any platform, so there is nothing left to
 * refuse. The result is still there because every caller already checks it and
 * because a call that cannot fail today is not a promise about tomorrow.
 *
 * Every note has to be initialised once before anything else touches it. */
bool burrow__note_init(burrow__Note *n);

/* The same, for a note that may be freed the moment its sleeper wakes up.
 *
 * A wake does not let go of the note at the instant the sleeper is released. It
 * opens the gate first, because opening it last would let a sleeper arrive in
 * between and never be seen, and then it has a little more work to do on a note
 * whose sleeper is already running. For a note inside a long lived structure
 * that is nothing to think about. For a note in the stack frame of the thread
 * that just woke up, the frame is gone.
 *
 * A note initialised this way keeps count of the wakes inside it, and a free
 * waits for that count to reach nought. It costs two atomic additions on every
 * wake, which is why it is a separate call rather than the default: the
 * scheduler's own notes live as long as the thread does and should not pay for
 * a problem they cannot have.
 *
 * Use this for a note that lives on a stack, and for anything holding a note
 * that some other thread can free. Everything that blocks one thread on another
 * and then returns wants it. */
bool burrow__note_init_transient(burrow__Note *n);

/* Finishes with the note. It holds nothing, so this frees nothing, and it is
 * still the call that ends a note's life because the transient case above has
 * something to wait for. The note must not have a sleeper on it, which is the
 * caller's problem the same way it is with a mutex: freeing something a thread
 * is asleep inside cannot be made safe here.
 *
 * On a transient note this also waits for any wake still in flight to let go,
 * which is what makes it safe to sleep on a local and free it on the next line.
 * On any other note it does not, and freeing one while a wake is in flight is
 * the same kind of bug as freeing one with a sleeper on it. */
void burrow__note_free(burrow__Note *n);

/* Closes the gate again so the note can be used for the next round.
 *
 * Only safe when nobody is asleep on it and nobody is about to be, which in
 * practice means the thread that owns the note does this before it hands the
 * note out. A clear that races a wake loses the wake, and a sleeper that then
 * arrives waits for a wakeup that already happened. */
void burrow__note_clear(burrow__Note *n);

/* Opens the gate and releases everybody waiting on it.
 *
 * Safe to call before anybody sleeps, which is the whole point: the waker never
 * has to know whether the sleeper got there first. Safe to call more than once
 * per clear as well, and the second one does nothing. */
void burrow__note_wake(burrow__Note *n);

/* Waits until the gate is open. Returns immediately if it already is.
 *
 * Spurious wakeups are handled here rather than by the caller, so this returns
 * when the note is open and at no other time. */
void burrow__note_sleep(burrow__Note *n);

/* Waits until the gate is open or until ns nanoseconds have gone by, whichever
 * comes first. True means the note is open, false means the time ran out.
 *
 * ns is a duration and not a deadline, and it is counted on burrow's monotonic
 * clock, so nothing anybody does to the system time can make this return early
 * or late. Zero or less does not wait at all and just reports the state, which
 * makes a poll and a timeout the same call.
 *
 * Coming back early is not possible and coming back late is. A spurious wakeup
 * is absorbed here the same way the untimed sleep absorbs it, with the time
 * remaining recomputed each round rather than restarted, so a thread that is
 * interrupted nine times still waits ns in total and not ten times ns. How late
 * it can be is the scheduler's business: a timeout of one nanosecond is a
 * request to be woken as soon as possible and on every platform here that means
 * tens of microseconds at best.
 *
 * This is Go's notetsleep, and the M park loop is what wants it: a thread with
 * no work should not sleep forever when there is a timer due in a millisecond. */
bool burrow__note_sleep_timeout(burrow__Note *n, int64_t ns);

/* Whether the gate is open, without waiting. For a caller that has something
 * better to do than block, and for tests. */
bool burrow__note_is_open(const burrow__Note *n);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NOTE_H */
