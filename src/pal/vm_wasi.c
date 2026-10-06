/* Reserving and committing memory on wasip1, where neither means anything.
 *
 * A wasm module has one linear memory that only grows, and every byte of it can
 * be read and written by any code in the module. There is nothing to reserve
 * without committing and nothing to protect, so a reservation is an allocation
 * from the C heap, zeroed as a fresh mapping would be, and committing it is
 * already done. Decommitting zeroes it, which is what the next touch after
 * MADV_DONTNEED sees on Linux.
 *
 * pal_vm_guard is the one that cannot be kept, and it says PAL_ENOTSUP. The
 * runtime asks for no guard under a goroutine stack here, for the same reason.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool page_aligned(const void *addr, int64_t bytes) {
    int64_t page = pal_page_size();
    return bytes > 0 && bytes % page == 0 && ((uintptr_t)addr % (uintptr_t)page) == 0;
}

void *pal_vm_reserve(int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    int64_t page = pal_page_size();
    if (bytes <= 0 || bytes % page != 0 || (uint64_t)bytes > SIZE_MAX) {
        BURROW_OUT(err, PAL_EINVAL);
        return NULL;
    }

    void *p = aligned_alloc((size_t)page, (size_t)bytes);
    if (p == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        return NULL;
    }
    memset(p, 0, (size_t)bytes);
    return p;
}

bool pal_vm_commit(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return true;
}

bool pal_vm_decommit(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    memset(addr, 0, (size_t)bytes);
    return true;
}

bool pal_vm_release(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    free(addr);
    return true;
}

bool pal_vm_guard(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* BURROW_OS_WASI */
