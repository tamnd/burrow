/* What the machine is, on Windows.
 *
 * Both answers come from GetSystemInfo, which fills a struct in one call, and
 * both are cached for the reason machine_posix.c gives.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

static uint32_t cached_page;
static uint32_t cached_cpus;

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
    uint32_t got = burrow__atomic_load_relaxed_u32(&cached_cpus);
    if (got != 0)
        return (int64_t)got;

    /* dwNumberOfProcessors is the count in this process's processor group, and
     * a group holds at most sixty four. A machine with more than that has
     * several groups and this answers the size of ours, which is the same
     * answer Go gives and is right for a process that has not gone out of its
     * way to span them.
     *
     * The affinity mask would be more precise, and GetProcessAffinityMask is
     * the call for it. It arrives when there is something above this layer that
     * can act on the difference. */
    SYSTEM_INFO info;
    GetSystemInfo(&info);

    DWORD n = info.dwNumberOfProcessors;
    if (n == 0)
        n = 1;

    burrow__atomic_store_relaxed_u32(&cached_cpus, (uint32_t)n);
    return (int64_t)n;
}

#endif /* BURROW_OS_WINDOWS */
