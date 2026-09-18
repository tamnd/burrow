/* The atomics tests again, with the 64 bit lock table forced on.
 *
 * Every machine in CI is 64 bit, so without this the fallback for 32 bit
 * targets would be compiled by nothing and run by nobody until somebody built
 * burrow for a Raspberry Pi and found out. Forcing the macro and recompiling
 * the same file is the whole trick.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#define BURROW_ATOMIC_FORCE_LOCK64 1
#define ATOMIC_SUITE "atomic/lock64"

#include "atomic_test.c"
