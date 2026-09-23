/* tests/bits_test.c again, with the compiler builtins turned off, so that the
 * portable code in burrow/math/bits.h is what gets checked. That code is what
 * a compiler burrow has never met will run, and without this file nothing
 * would run it on the compilers CI does have.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#define BURROW__BITS_PORTABLE 1
#define BITS_SUITE "bits portable"

#include "bits_test.c"
