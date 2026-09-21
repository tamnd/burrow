/* The sync/atomic tests again, with the 64 bit lock table forced on.
 *
 * Every function in the header is inline, so recompiling the test with the
 * macro set is enough to put the whole int64, uint64 and Value surface on the
 * path a 32 bit machine would take. Same reason as atomic_lock64_test.c: the
 * fallback is only exercised by the machines nobody in CI has.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#define BURROW_ATOMIC_FORCE_LOCK64 1
#define SYNC_ATOMIC_SUITE "sync/atomic/lock64"

#include "sync_atomic_test.c"
