/* Starting an OS thread on Windows.
 *
 * This was src/runtime/thread.c's Windows half until the platform layer
 * existed. Read src/pal/thread_posix.c for the handshake that carries the
 * function and the argument through an entry point with room for one pointer:
 * it is the same handshake and the same reasoning, with different spelling.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Windows 7, which is a floor no machine anybody compiles for today is below.
 * Before every include, because a Windows header that has already been read has
 * already decided. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <limits.h>
#include <stdint.h>

#include <process.h>
#include <windows.h>

typedef struct Start {
    void (*fn)(void *);
    void *arg;
    /* 0 until the new thread has both of the above in registers of its own. */
    uint32_t taken;
} Start;

/* _beginthreadex rather than CreateThread, because a thread created the second
 * way has no CRT state of its own, and the first thing it calls that keeps
 * something per thread leaks it. Nothing in burrow depends on that today and
 * everything a caller runs on one of these threads might. */
static unsigned __stdcall entry(void *p) {
    Start *s = (Start *)p;

    void (*fn)(void *) = s->fn;
    void *arg = s->arg;

    /* See the same two lines in src/pal/thread_posix.c for why this store is
     * sequentially consistent and why the wake is safe once the starter's frame
     * has gone. */
    burrow__atomic_store_u32(&s->taken, 1);
    (void)pal_futex_wake(&s->taken, INT64_MAX, NULL);

    fn(arg);
    return 0;
}

/* Windows has no stack minimum of its own worth the name: a size below one page
 * is rounded up to one and nothing is refused. The floor is applied anyway so
 * that a caller asking for a small stack gets the same small stack on every
 * system rather than a different one on each. It is macOS's number. */
#define STACK_FLOOR ((int64_t)16384)

int64_t pal_thread_create(void (*fn)(void *), void *arg, int64_t stack_bytes,
                          PalErrno *err) {
    if (fn == NULL || stack_bytes < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return PAL_INVALID_HANDLE;
    }

    if (stack_bytes != 0 && stack_bytes < STACK_FLOOR)
        stack_bytes = STACK_FLOOR;
    if (stack_bytes > (int64_t)UINT_MAX)
        stack_bytes = (int64_t)UINT_MAX;

    Start s;
    s.fn = fn;
    s.arg = arg;
    s.taken = 0;

    unsigned id = 0;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_bytes, entry, &s, 0, &id);
    if (h == 0) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return PAL_INVALID_HANDLE;
    }

    while (burrow__atomic_load_acquire_u32(&s.taken) == 0)
        (void)pal_futex_wait(&s.taken, 0, -1, NULL);

    BURROW_OUT(err, PAL_OK);
    return (int64_t)h;
}

bool pal_thread_join(int64_t thread, PalErrno *err) {
    if (thread == PAL_INVALID_HANDLE || thread == 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* INFINITE is right here and a timeout would not be. A join that gives up
     * leaves a thread running against memory the caller is about to reuse, and
     * there is no way to make that safe after the fact. */
    if (WaitForSingleObject((HANDLE)(uintptr_t)thread, INFINITE) != WAIT_OBJECT_0) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    if (!CloseHandle((HANDLE)(uintptr_t)thread)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_thread_detach(int64_t thread, PalErrno *err) {
    if (thread == PAL_INVALID_HANDLE || thread == 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Closing the handle is the whole of detaching on Windows. The thread keeps
     * running and the system releases it when it returns. */
    if (!CloseHandle((HANDLE)(uintptr_t)thread)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

int64_t pal_thread_self(void) {
    return (int64_t)GetCurrentThreadId();
}

void pal_thread_yield(void) {
    /* SwitchToThread only considers threads on this processor and returns
     * without sleeping when there are none, which is what a spin loop wants.
     * Sleep(0) is the other spelling and it will not yield to a lower priority
     * thread, which is exactly the thread a spin loop is usually waiting on. */
    (void)SwitchToThread();
}

/* A warning about the mingw header rather than about anything here.
 *
 * NtCurrentTeb is a read through the gs segment at a fixed offset, which the
 * header spells as a dereference of a null pointer with a segment override on
 * it. That is the only way to say it in C and it is correct, but gcc's array
 * bounds pass does not model segment overrides, so it sees a load from address
 * zero, says so, and -Werror turns saying so into a failed build. It has been
 * doing this since gcc 12. Nothing burrow writes differently makes it stop,
 * including not comparing the result against null, which was the first guess.
 *
 * The alternative is GetCurrentThreadStackLimits and the comment below says why
 * that is not an alternative here. clang compiles the same header without
 * complaining, so it is left to say what it thinks. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

bool pal_thread_stack_bounds(void **lo, void **hi) {
    /* Both ends are in the thread information block, which every thread has and
     * which NtCurrentTeb hands back with no call at all: it is a register read.
     * GetCurrentThreadStackLimits is the documented spelling of the same two
     * words and it arrived in Windows 8, which is above the floor this file is
     * built to, so the block it would have read is read here instead. NT_TIB is
     * the first member of the block and winnt.h describes it in full, which is
     * what makes the cast the usual way to say this.
     *
     * StackLimit is the lowest page committed so far rather than the lowest the
     * stack is allowed to reach. That is the tighter of the two bounds and it
     * is the right one for a walk, since a frame below it is a frame that was
     * never written. */
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();

    if (tib == NULL || tib->StackLimit == NULL || tib->StackBase == NULL)
        return false;
    if ((const char *)tib->StackLimit >= (const char *)tib->StackBase)
        return false;

    BURROW_OUT(lo, tib->StackLimit);
    BURROW_OUT(hi, tib->StackBase);
    return true;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif /* BURROW_OS_WINDOWS */
