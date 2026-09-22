/* What the machine is, on Windows.
 *
 * The page size is cached and the processor count is not, for the reasons
 * machine_posix.c gives.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Windows 7, which is what GetActiveProcessorCount needs and is a floor no
 * machine anybody compiles for today is below. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>

#include <windows.h>

static uint32_t cached_page;

int64_t pal_page_size(void) {
    uint32_t got = burrow__atomic_load_relaxed_u32(&cached_page);
    if (got != 0)
        return (int64_t)got;

    SYSTEM_INFO info;
    GetSystemInfo(&info);

    /* dwPageSize and not dwAllocationGranularity. The granularity is 64 KB and
     * is what VirtualAlloc rounds a reservation's base address to, which is a
     * separate fact that nothing above this layer needs: a reservation gets
     * whatever base Windows picks, and every offset into it is a page. */
    DWORD n = info.dwPageSize;
    if (n == 0 || (n & (n - 1)) != 0)
        n = 4096;

    burrow__atomic_store_relaxed_u32(&cached_page, (uint32_t)n);
    return (int64_t)n;
}

int64_t pal_cpu_count(void) {
    /* ALL_PROCESSOR_GROUPS, because a machine with more than sixty four
     * processors puts them in groups and GetSystemInfo reports the size of one
     * group. A hundred and twenty eight processor box answering sixty four is
     * the kind of wrong that looks right.
     *
     * The affinity mask would be more precise still, and GetProcessAffinityMask
     * is the call for it, but it answers for one group as well and stitching
     * the groups back together is a loop over GetLogicalProcessorInformationEx.
     * That arrives when there is a Windows machine to test it on. */
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n == 0)
        n = 1;
    if (n > (DWORD)INT32_MAX)
        n = (DWORD)INT32_MAX;

    return (int64_t)n;
}

#endif /* BURROW_OS_WINDOWS */
