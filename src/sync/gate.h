/* internal/gate, a mutex and a condition variable in one, with one bit of
 * state, made of two channels. internal/nettest's Listener is built on it.
 *
 * Derived from Go's src/internal/gate. Go source: go1.27.3.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_SYNC_GATE_H
#define BURROW_SRC_SYNC_GATE_H

/* burrow:package internal/gate */

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"

#include <stdbool.h>

/* A Gate is a monitor (mutex + condition variable) with one bit of state.
 *
 * The condition may be either set or unset.
 * Lock operations may be unconditional, or wait for the condition to be set.
 * Unlock operations record the new state of the condition.
 *
 * When unlocked, exactly one of set or unset contains a value. When locked,
 * neither chan contains a value. */
typedef struct Gate {
    Chan *set;
    Chan *unset;
} Gate;

/* gate.New: a new, unlocked gate, with its two channels from a. False when a
 * cannot spare them, and then g is left with none. */
bool burrow__gate_init(Gate *g, Alloc *a, bool set);

/* Gives the channels back. No goroutine may be waiting on the gate. Go leaves
 * this to the collector. */
void burrow__gate_destroy(Gate *g);

/* Gate.Lock: acquires the gate unconditionally. It reports whether the
 * condition is set. */
bool burrow__gate_lock(Gate *g);

/* Gate.WaitAndLock: waits until the condition is set before acquiring the
 * gate. If the context expires, it returns the context's error and does not
 * acquire the gate. */
Error burrow__gate_wait_and_lock(Gate *g, Context ctx);

/* Gate.LockIfSet: acquires the gate if and only if the condition is set. */
bool burrow__gate_lock_if_set(Gate *g);

/* Gate.Unlock: sets the condition and releases the gate. */
void burrow__gate_unlock(Gate *g, bool set);

#endif /* BURROW_SRC_SYNC_GATE_H */
