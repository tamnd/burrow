/* The atomics tests again, on the last resort <stdatomic.h> backend.
 *
 * GCC and Clang take the builtin path and MSVC takes the Interlocked path, so
 * the C11 path is the one that exists for compilers we have not met. Forcing it
 * here means it is compiled and run everywhere, which is the difference between
 * a fallback and a guess.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#define BURROW_ATOMIC_BACKEND_C11 1
#define ATOMIC_SUITE "atomic/c11"

#include "atomic_test.c"
