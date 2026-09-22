/* The runtime's monotonic clock, which is now the PAL's.
 *
 * The three platform readings that used to be in this file are in
 * src/pal/time_windows.c, src/pal/time_darwin.c and src/pal/time_posix.c, which
 * is where anything that touches an operating system lives. What is left here is
 * the name the runtime calls it by.
 *
 * Two names for one reading looks like one too many, and the reason both exist
 * is that they answer to different audiences. pal_clock_monotonic is a
 * platform's monotonic clock, documented in burrow/pal.h with the rest of the
 * boundary. burrow__nanotime is Go's nanotime, documented in burrow/clock.h with
 * what a timer and a deadline may assume of it, and it is what the twenty odd
 * files above this one already call. Collapsing them would mean either dragging
 * pal.h into every one of those or losing the contract clock.h states.
 *
 * It costs a call. In the amalgamated build, which is one translation unit, it
 * costs nothing at all, because the compiler can see straight through this into
 * the reading itself. That is the build this library is meant to be deployed
 * as, so the cost lands only on the build where the timer path is not the thing
 * being measured.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/clock.h"

#include "burrow/pal.h"

int64_t burrow__nanotime(void) {
    return pal_clock_monotonic();
}
