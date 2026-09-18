/* The contention tests again, with the 64 bit lock table forced on. See
 * atomic_concurrent_test.c, which is the whole of it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#define BURROW_ATOMIC_FORCE_LOCK64 1
#define CONCURRENT_SUITE "atomic/concurrent/lock64"

#include "atomic_concurrent_test.c"
