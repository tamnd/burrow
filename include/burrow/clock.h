/* The clock the runtime measures time with.
 *
 * One function, and the reason it needs a header of its own is that C has no
 * such clock. `clock()` counts processor time, `time()` has a resolution of a
 * second, and `timespec_get(TIME_UTC)` is the wall clock, which moves when
 * somebody sets the date and moves backwards when ntp corrects a drift. A timer
 * built on a clock that can go backwards fires twice or never, and a sleep
 * built on one can wait for an hour because a server in another timezone was
 * rebooted.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CLOCK_H
#define BURROW_CLOCK_H

#include "burrow/platform.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds on a clock that only goes forwards.
 *
 * This is Go's `nanotime`, and like Go's it measures from an arbitrary point
 * that means nothing on its own. Only the difference between two readings is a
 * fact, and that difference is a duration in nanoseconds however long the
 * process has been running and whatever anybody does to the system time.
 *
 * Never goes backwards and never jumps forwards, which is what every timer and
 * every timeout in the runtime rests on. It is `CLOCK_MONOTONIC` on Linux and
 * the BSDs, `mach_absolute_time` on macOS, and `QueryPerformanceCounter` on
 * Windows, which are the same four calls Go makes.
 *
 * What it does over a suspend is not the same everywhere and is not something
 * this can fix. A laptop that sleeps for an hour comes back with this clock an
 * hour further on under Linux and a few milliseconds further on under macOS and
 * Windows, because the last two stop counting while the machine is asleep. Go
 * has exactly this difference for exactly this reason, so a timer that was due
 * during the suspend fires on the way out of it on all three, just with a
 * different idea of how late it is. Anything that needs the real elapsed time
 * across a suspend needs the wall clock and has to accept what comes with it.
 *
 * Resolution is whatever the platform offers, which is a nanosecond on Linux,
 * about 40 nanoseconds on arm64 macOS and 100 nanoseconds on Windows. Cost is a
 * handful of nanoseconds everywhere, because all three go through the vdso or
 * its equivalent rather than into the kernel.
 *
 * Callable from any thread, including one the runtime knows nothing about, and
 * from a signal handler. */
int64_t burrow__nanotime(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CLOCK_H */
