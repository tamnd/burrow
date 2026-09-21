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

#if defined(__linux__) && !defined(_GNU_SOURCE)
/* For sched_getaffinity, which burrow__thread_ncpu needs. Also before every
 * include, and for the same reason: a feature test macro set after a header has
 * already been read is a macro nobody looked at. */
#define _GNU_SOURCE
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

/* Where the BSDs keep the call that says how big the current thread's stack is.
 * NetBSD and illumos declare theirs in pthread.h, which is already in through
 * burrow/thread.h, and OpenBSD's answer comes back in a stack_t. */
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_OPENBSD)
#include <pthread_np.h>
#endif
#if defined(BURROW_OS_OPENBSD)
#include <signal.h>
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

bool burrow__thread_stack_bounds(void **lo, void **hi) {
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

    *lo = tib->StackLimit;
    *hi = tib->StackBase;
    return true;
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

/* Asking a pthreads system where the current thread's stack is.
 *
 * There is no POSIX call for this, so there is one of these per system. What
 * they have in common is that all of them answer for the thread that is asking
 * and several of them answer for no other, which is why the header says to only
 * ask about yourself.
 *
 * The two prototypes below are written out rather than taken from a header, the
 * same way src/runtime/note.c writes out syscall and for the same reason: both
 * glibc and musl hide them unless a feature macro is set, burrow is built as
 * strict C11, and these signatures are fixed by an ABI that has not moved in
 * twenty years. Picking what is declared without a feature macro is the rule,
 * and where nothing is, saying what the ABI already says is the fallback. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    /* Darwin gives the top and the size, which is the same pair the other way
     * round. Both calls are declared in pthread.h with no feature macro and
     * neither of them can fail for the calling thread. */
    char *top = (char *)pthread_get_stackaddr_np(pthread_self());
    size_t size = pthread_get_stacksize_np(pthread_self());

    if (top == NULL || size == 0)
        return false;
    *lo = top - size;
    *hi = top;
    return true;
}

#elif defined(BURROW_OS_LINUX)

/* pthread_getattr_np and pthread_attr_getstack used to be declared by hand
 * here, because neither is visible without a feature macro and there was none.
 * The top of this file defines _GNU_SOURCE now, for sched_getaffinity, and with
 * that both come out of pthread.h. Declaring them again is a redundant
 * declaration and the build treats that as an error. */

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;

    /* This fills an attribute block with what the thread actually got rather
     * than with what was asked for, which for the main thread means the current
     * stack limit and not a size anybody passed anywhere. It allocates on some
     * glibc versions, which is why the destroy below is not optional. */
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return false;
    if (pthread_attr_getstack(&attr, &base, &size) != 0) {
        (void)pthread_attr_destroy(&attr);
        return false;
    }
    (void)pthread_attr_destroy(&attr);

    if (base == NULL || size == 0)
        return false;
    *lo = base;
    *hi = (char *)base + size;
    return true;
}

#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                       \
    defined(BURROW_OS_DRAGONFLY) || defined(BURROW_OS_SOLARIS)

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;

    if (pthread_attr_init(&attr) != 0)
        return false;
    if (pthread_attr_get_np(pthread_self(), &attr) != 0) {
        (void)pthread_attr_destroy(&attr);
        return false;
    }
    if (pthread_attr_getstack(&attr, &base, &size) != 0) {
        (void)pthread_attr_destroy(&attr);
        return false;
    }
    (void)pthread_attr_destroy(&attr);

    if (base == NULL || size == 0)
        return false;
    *lo = base;
    *hi = (char *)base + size;
    return true;
}

#elif defined(BURROW_OS_OPENBSD)

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    /* OpenBSD's is the odd one and the odd part is ss_sp, which is the top of
     * the stack here and the bottom of it in the sigaltstack that stack_t was
     * borrowed from. */
    stack_t seg;

    if (pthread_stackseg_np(pthread_self(), &seg) != 0)
        return false;
    if (seg.ss_sp == NULL || seg.ss_size == 0)
        return false;
    *lo = (char *)seg.ss_sp - seg.ss_size;
    *hi = seg.ss_sp;
    return true;
}

#else

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    /* Nobody has written this one. The header says false means exactly that and
     * the stack walker does less rather than guessing. */
    (void)lo;
    (void)hi;
    return false;
}

#endif

int burrow__thread_ncpu(void) {
#if defined(BURROW_OS_LINUX)
    /* How many processors this process may run on, which is not how many the
     * machine has whenever anybody has said otherwise: a container started with
     * a cpuset, a process under taskset, a job placed by a batch scheduler. The
     * difference matters twice over. A program that starts one thread per core
     * on a box where it is allowed two of them spends its life switching
     * between threads that cannot run, and a thread that decides to spin
     * waiting for a lock because the machine has plenty of cores is spinning on
     * the one core it shares with the holder. Go reads the mask here for
     * exactly these reasons and so does this.
     *
     * The fixed size mask covers 1024 processors. Past that the call fails and
     * the answer below is the machine's count, which on a box that large is
     * near enough and is what the code did before. */
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int allowed = CPU_COUNT(&set);
        if (allowed >= 1)
            return allowed;
    }
#endif

    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return 1;
    if (n > INT_MAX)
        return INT_MAX;
    return (int)n;
}

#endif
