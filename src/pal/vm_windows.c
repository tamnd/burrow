/* Virtual memory on Windows.
 *
 * This is the platform the reserve and commit split came from, so there is
 * nothing to emulate here and the functions are one call each. The one thing
 * worth knowing is that MEM_RELEASE insists on the address the reservation
 * started at and on a size of zero, which is why pal_vm_release takes a size it
 * then ignores: POSIX needs it and the interface cannot have two shapes.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <windows.h>

static bool page_aligned(const void *addr, int64_t bytes) {
    int64_t page = pal_page_size();
    return bytes > 0 && bytes % page == 0 && ((uintptr_t)addr % (uintptr_t)page) == 0;
}

void *pal_vm_reserve(int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (bytes <= 0 || bytes % pal_page_size() != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return NULL;
    }

    /* PAGE_READWRITE on a MEM_RESERVE is the protection the pages will have
     * once they are committed, not one they have now. A reserved page cannot be
     * touched at all, whatever is written here. */
    void *p = VirtualAlloc(NULL, (SIZE_T)bytes, MEM_RESERVE, PAGE_READWRITE);
    if (p == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return NULL;
    }

    return p;
}

bool pal_vm_commit(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Committing an already committed range succeeds and does nothing, which
     * Windows documents and which the POSIX side matches. */
    if (VirtualAlloc(addr, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
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

    /* MEM_DECOMMIT leaves the reservation in place and gives the memory back,
     * and a reserved page faults when it is touched, so this is both halves of
     * what the POSIX side does with a madvise and an mprotect. */
    if (!VirtualFree(addr, (SIZE_T)bytes, MEM_DECOMMIT)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    return true;
}

bool pal_vm_release(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || bytes <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Size zero, because MEM_RELEASE means the whole reservation and Windows
     * rejects the call outright if it is told how big that is. */
    if (!VirtualFree(addr, 0, MEM_RELEASE)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    return true;
}

bool pal_vm_guard(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* PAGE_NOACCESS rather than PAGE_GUARD. The guard flag is a one shot: the
     * first access raises STATUS_GUARD_PAGE_VIOLATION and clears the flag, so
     * the second access succeeds and the overflow goes unnoticed. That is what
     * Windows wants for growing a thread stack and it is the wrong thing for a
     * wall, which has to keep being a wall. */
    DWORD was;
    if (!VirtualProtect(addr, (SIZE_T)bytes, PAGE_NOACCESS, &was)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    return true;
}

#endif /* BURROW_OS_WINDOWS */
