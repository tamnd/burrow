/* 64 bit atomics for machines that do not have them.
 *
 * A 32 bit machine can load and store four bytes atomically and cannot do eight.
 * Go solves this with a small table of locks indexed by the address, and so do
 * we, for the same reason: the alternative is either a link time dependency on
 * libatomic, which only some platforms ship, or a rule that says burrow's
 * counters are 32 bit on 32 bit machines, which would change what the library
 * means depending on where it was compiled.
 *
 * The lock is a spin lock rather than a mutex because the critical section is
 * one load or one add. A thread that is preempted while holding one of these
 * will make everybody hashing to the same slot spin, which is the cost of the
 * design and is what Go pays too.
 *
 * This is compiled everywhere, including on machines that will never call it,
 * so that a test can reach it with -DBURROW_ATOMIC_FORCE_LOCK64=1. A fallback
 * that only runs on hardware none of us has is a fallback nobody has tested.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/atomic.h"

/* Prime, so that addresses a fixed stride apart spread over the whole table
 * rather than landing on a handful of slots. Sixty one slots is about four
 * kilobytes, which is nothing, and it is well past the number of threads that
 * will ever be contending for a 64 bit counter on a 32 bit machine. */
#define NSLOTS 61

/* A cache line, give or take. Two slots in one line is two unrelated counters
 * fighting over the same line, which is the whole problem this table exists to
 * spread out. */
#define SLOT_PAD 64

typedef struct {
    uint32_t held;
    char pad[SLOT_PAD - sizeof(uint32_t)];
} Atomic64Slot;

static Atomic64Slot atomic64_locks[NSLOTS];

/* How many slots this thread holds or is spinning for. A profiling signal that
 * lands while it is not zero must not touch a 64 bit atomic, because the slot it
 * hashes to may be the one the interrupted code is holding, and the handler
 * would spin for ever on a lock that cannot be let go until it returns. Go has
 * the same problem on its 32 bit machines and counts those samples as lost,
 * which is what burrow__atomic64_locked_here is for.
 *
 * Raised before the slot is tried and lowered after it is let go, with read
 * modify writes that keep the compiler from moving the lock across them, so
 * there is no moment the slot is held and this says it is not. */
static BURROW_THREAD_LOCAL uint32_t atomic64_depth;

/* Shifted by three because a uint64_t is eight byte aligned when anybody has
 * been careful, so the low three bits carry no information and hashing on them
 * would waste most of the table. */
static Atomic64Slot *slot_for(const void *p) {
    uintptr_t key = (uintptr_t)p >> 3;
    return &atomic64_locks[key % NSLOTS];
}

static void lock(Atomic64Slot *s) {
    (void)burrow__atomic_add_u32(&atomic64_depth, 1);
    for (;;) {
        uint32_t free_ = 0;
        if (burrow__atomic_cas_acquire_u32(&s->held, &free_, 1))
            return;
        while (burrow__atomic_load_relaxed_u32(&s->held) != 0)
            burrow__atomic_spin_hint();
    }
}

static void unlock(Atomic64Slot *s) {
    burrow__atomic_store_release_u32(&s->held, 0);
    (void)burrow__atomic_add_u32(&atomic64_depth, 0U - 1U);
}

bool burrow__atomic64_locked_here(void) {
    return burrow__atomic_load_relaxed_u32(&atomic64_depth) != 0;
}

uint64_t burrow__atomic64_load(const uint64_t *p) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t v = *p;
    unlock(s);
    return v;
}

void burrow__atomic64_store(uint64_t *p, uint64_t v) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    *p = v;
    unlock(s);
}

uint64_t burrow__atomic64_add(uint64_t *p, uint64_t v) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t old = *p;
    *p = old + v;
    unlock(s);
    return old;
}

uint64_t burrow__atomic64_and(uint64_t *p, uint64_t v) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t old = *p;
    *p = old & v;
    unlock(s);
    return old;
}

uint64_t burrow__atomic64_or(uint64_t *p, uint64_t v) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t old = *p;
    *p = old | v;
    unlock(s);
    return old;
}

uint64_t burrow__atomic64_swap(uint64_t *p, uint64_t v) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t old = *p;
    *p = v;
    unlock(s);
    return old;
}

bool burrow__atomic64_cas(uint64_t *p, uint64_t *expected, uint64_t desired) {
    Atomic64Slot *s = slot_for(p);
    lock(s);
    uint64_t seen = *p;
    bool ok = seen == *expected;
    if (ok)
        *p = desired;
    unlock(s);
    if (!ok)
        *expected = seen;
    return ok;
}
