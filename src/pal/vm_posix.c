/* Virtual memory on everything that has mmap, which is every platform burrow
 * builds for except Windows.
 *
 * POSIX has no reserve and commit, it has one call that does both and a
 * protection bit that decides whether the pages can be touched. Reserving is
 * therefore a PROT_NONE mapping, which takes address space and no memory, and
 * committing is an mprotect on part of it. That gives the Win32 shape on a
 * system that does not have it, which is the direction the emulation has to go
 * because the other one cannot be done.
 *
 * The feature test macros come first for the reason src/runtime/stack.c gives:
 * glibc reads them when its first header arrives and ignores one that is defined
 * afterwards, and MAP_ANONYMOUS is an extension that a bare _POSIX_C_SOURCE
 * build does not get. _WIN32 rather than BURROW_OS_WINDOWS because knowing the
 * latter would mean including a header first.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

/* MAP_ANON is the older spelling and the only one on some BSDs. They mean the
 * same thing and every system has at least one of them. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* Both have to be page aligned, and this layer does not round for the caller.
 * A caller that rounds knows what it asked for, and a layer that rounds
 * silently hands back more than was asked for and is then asked to release less
 * than it gave. */
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

    /* PROT_NONE so that nothing can touch it until pal_vm_commit says so, and
     * MAP_NORESERVE where it exists so that a large reservation does not count
     * against the overcommit limit. Linux has MAP_NORESERVE and it is the one
     * that matters, because a goroutine stack reserves far more than it will
     * ever commit. */
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif

    void *p = mmap(NULL, (size_t)bytes, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) {
        BURROW_OUT(err, burrow__pal_errno(errno));
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

    /* Committing a range that is already committed is not an error, and here it
     * is not even a second call: mprotect to the protection it already has is
     * what the kernel does with it. That is what lets a grow loop commit from
     * the bottom every time without tracking where it got to. */
    if (mprotect(addr, (size_t)bytes, PROT_READ | PROT_WRITE) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
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

    /* Two steps, and the order matters. madvise gives the pages back, so the
     * process stops being charged for them, and mprotect makes touching them
     * fault, so the reservation still holds the addresses but nothing can use
     * them by accident.
     *
     * MADV_DONTNEED on Linux drops the pages and the next touch gets zeroes.
     * On macOS and the BSDs the same flag is advisory and MADV_FREE is the one
     * that actually releases, so both are tried in the order that works on the
     * platform. Neither failing is fatal: the worst case is that the memory
     * stays charged to us, and the mprotect below still makes the range
     * unusable, which is the part a caller depends on. */
#if defined(MADV_FREE) && (defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS))
    (void)madvise(addr, (size_t)bytes, MADV_FREE);
#elif defined(MADV_DONTNEED)
    (void)madvise(addr, (size_t)bytes, MADV_DONTNEED);
#endif

    if (mprotect(addr, (size_t)bytes, PROT_NONE) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }

    return true;
}

bool pal_vm_release(void *addr, int64_t bytes, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (addr == NULL || !page_aligned(addr, bytes)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    if (munmap(addr, (size_t)bytes) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
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

    if (mprotect(addr, (size_t)bytes, PROT_NONE) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }

    return true;
}

#endif /* !BURROW_OS_WINDOWS */
