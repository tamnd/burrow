/* Starting an OS thread, on pthreads and on Windows.
 *
 * Two implementations of six functions. They are short because the hard part of
 * threading is not starting one, and everything that is hard lives above this
 * file. What is here is the part that is different on every system, kept in one
 * place so the rest of the runtime never has to ask which system it is on.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Windows 7, which is what GetActiveProcessorCount needs and is a floor no
 * machine anybody compiles for today is below. Before every include, because a
 * Windows header that has already been read has already decided. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/thread.h"

#include "burrow/platform.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#include <process.h>
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

/* The smallest stack we will ask for, and a guessed constant is not good enough
 * for it. glibc on arm64 wants 128 kilobytes where glibc on amd64 wants 16, and
 * it hides PTHREAD_STACK_MIN behind a feature macro that a strict C11 build does
 * not set, so a build that reads the macro on one machine and a fallback on the
 * next gets a number that is right in one place and rejected with EINVAL in the
 * other.
 *
 * sysconf(_SC_THREAD_STACK_MIN) is the same answer asked at runtime, it is
 * declared with no feature macro anywhere, and on glibc it is what the macro
 * expands to these days in any case. The macro is still consulted where it is
 * visible, since a system whose two answers disagree should get the larger one.
 *
 * 16384 is the last resort for a system that answers neither, and it is the
 * value macOS uses. */
#define STACK_FLOOR ((size_t)16384)

#if !defined(BURROW_OS_WINDOWS)
static size_t stack_min(void) {
    size_t min = STACK_FLOOR;

    long answer = sysconf(_SC_THREAD_STACK_MIN);
    if (answer > 0 && (size_t)answer > min)
        min = (size_t)answer;

#if defined(PTHREAD_STACK_MIN)
    if ((size_t)PTHREAD_STACK_MIN > min)
        min = (size_t)PTHREAD_STACK_MIN;
#endif

    return min;
}

/* macOS documents the stack size as having to be a multiple of the page size
 * and rejects one that is not, which no other system here cares about. Rounding
 * up everywhere costs less than one page per thread and means a caller never has
 * to know which system it is on, which is the whole job of this file. */
static size_t round_to_page(size_t bytes) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return bytes;

    size_t size = (size_t)page;
    size_t over = bytes % size;
    if (over == 0)
        return bytes;

    /* A request this close to the top is not a real request, and growing it
     * would wrap. Give back what was asked for and let the system refuse it. */
    if (bytes > SIZE_MAX - (size - over))
        return bytes;

    return bytes + (size - over);
}
#endif

#if defined(BURROW_OS_WINDOWS)

/* _beginthreadex rather than CreateThread, because a thread created the second
 * way has no CRT state of its own, and the first thing it calls that keeps
 * something per thread leaks it. Nothing in burrow depends on that today and
 * everything a caller runs on one of these threads might. */
static unsigned __stdcall win_start(void *p) {
    burrow__Thread *t = (burrow__Thread *)p;
    t->fn(t->arg);
    return 0;
}

bool burrow__thread_start(burrow__Thread *t, burrow__ThreadFn fn, void *arg,
                          size_t stack_bytes) {
    if (t == NULL || fn == NULL)
        return false;

    t->fn = fn;
    t->arg = arg;
    t->handle = NULL;
    t->id = 0;
    t->started = false;

    /* Windows has no minimum of its own worth the name: a size below one page
     * is rounded up to one and nothing is refused. The floor is applied anyway
     * so that a caller asking for a small stack gets the same small stack on
     * every system rather than a different one on each. */
    if (stack_bytes != 0 && stack_bytes < STACK_FLOOR)
        stack_bytes = STACK_FLOOR;
#if SIZE_MAX > UINT_MAX
    /* Only on a 64 bit build, where the two are different. On a 32 bit one they
     * are the same number and the compiler says so. */
    if (stack_bytes > (size_t)UINT_MAX)
        stack_bytes = (size_t)UINT_MAX;
#endif

    unsigned id = 0;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_bytes, win_start, t, 0, &id);
    if (h == 0)
        return false;

    t->handle = (void *)h;
    t->id = id;
    t->started = true;
    return true;
}

bool burrow__thread_join(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;

    /* INFINITE is right here and a timeout would not be. A join that gives up
     * leaves a thread running against memory the caller is about to reuse, and
     * there is no way to make that safe after the fact. */
    if (WaitForSingleObject((HANDLE)t->handle, INFINITE) != WAIT_OBJECT_0)
        return false;

    CloseHandle((HANDLE)t->handle);
    t->handle = NULL;
    t->started = false;
    return true;
}

bool burrow__thread_detach(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;

    /* Closing the handle is the whole of detaching on Windows. The thread keeps
     * running and the system releases it when it returns. */
    CloseHandle((HANDLE)t->handle);
    t->handle = NULL;
    t->started = false;
    return true;
}

uint64_t burrow__thread_self(void) {
    return (uint64_t)GetCurrentThreadId();
}

void burrow__thread_yield(void) {
    /* SwitchToThread only considers threads on this processor and returns
     * without sleeping when there are none, which is what a spin loop wants.
     * Sleep(0) is the other spelling and it will not yield to a lower priority
     * thread, which is exactly the thread a spin loop is usually waiting on. */
    (void)SwitchToThread();
}

int burrow__thread_ncpu(void) {
    /* ALL_PROCESSOR_GROUPS because a machine with more than 64 processors puts
     * them in groups, and the obvious call reports the size of one group. A
     * 128 processor box answering 64 is the kind of wrong that looks right. */
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n == 0)
        return 1;
    if (n > (DWORD)INT_MAX)
        return INT_MAX;
    return (int)n;
}

#else

static void *posix_start(void *p) {
    burrow__Thread *t = (burrow__Thread *)p;
    t->fn(t->arg);
    return NULL;
}

bool burrow__thread_start(burrow__Thread *t, burrow__ThreadFn fn, void *arg,
                          size_t stack_bytes) {
    if (t == NULL || fn == NULL)
        return false;

    t->fn = fn;
    t->arg = arg;
    t->started = false;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return false;

    if (stack_bytes != 0) {
        size_t min = stack_min();
        if (stack_bytes < min)
            stack_bytes = min;
        stack_bytes = round_to_page(stack_bytes);
        if (pthread_attr_setstacksize(&attr, stack_bytes) != 0) {
            (void)pthread_attr_destroy(&attr);
            return false;
        }
    }

    /* t is the argument, which is how the function and the caller's argument
     * reach the new thread through a parameter with room for one pointer. It is
     * also why the handle has to outlive the thread, which the header says. */
    int err = pthread_create(&t->id, &attr, posix_start, t);
    (void)pthread_attr_destroy(&attr);
    if (err != 0)
        return false;

    t->started = true;
    return true;
}

bool burrow__thread_join(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;
    if (pthread_join(t->id, NULL) != 0)
        return false;
    t->started = false;
    return true;
}

bool burrow__thread_detach(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;
    if (pthread_detach(t->id) != 0)
        return false;
    t->started = false;
    return true;
}

uint64_t burrow__thread_self(void) {
    /* pthread_t is opaque and is a pointer on macOS, an integer on Linux and
     * neither of those things somewhere, so the bytes are copied rather than
     * cast. That gives an identity, which is what the header promises, and not
     * the thread id a debugger shows. The real id needs a different call on
     * every system and nothing needs it until tracing does. */
    pthread_t self = pthread_self();
    uint64_t id = 0;
    size_t n = sizeof self < sizeof id ? sizeof self : sizeof id;
    memcpy(&id, &self, n);
    return id;
}

void burrow__thread_yield(void) {
    (void)sched_yield();
}

int burrow__thread_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return 1;
    if (n > INT_MAX)
        return INT_MAX;
    return (int)n;
}

#endif
