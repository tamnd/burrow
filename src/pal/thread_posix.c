/* Starting an OS thread on pthreads.
 *
 * This was src/runtime/thread.c's POSIX half until the platform layer existed,
 * and most of the reasoning in the comments below came with it. Two things are
 * new. The handle a caller gets back is an int64_t rather than a pthread_t, and
 * the function and argument reach the new thread through a handshake rather
 * than through a structure the caller has to keep alive.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* For pthread_getattr_np on glibc, which is how the stack bounds are found on
 * Linux. Before the first include, because a feature test macro set after a
 * header has already been read is a macro nobody looked at. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Where the BSDs keep the call that says how big the current thread's stack is.
 * NetBSD and illumos declare theirs in pthread.h, and OpenBSD's answer comes
 * back in a stack_t. */
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_OPENBSD)
#include <pthread_np.h>
#endif
#if defined(BURROW_OS_OPENBSD)
#include <signal.h>
#endif

/* A pthread_t has to survive the trip through an int64_t and back, and it is
 * opaque, so the bytes are copied rather than cast. That works because it is a
 * pointer on macOS and an unsigned long on Linux, and this says so out loud
 * rather than assuming it. A system whose pthread_t is larger fails to build
 * here, which is the right place to find out. */
_Static_assert(sizeof(pthread_t) <= sizeof(int64_t),
               "pthread_t does not fit in a PAL handle");

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
 * to know which system it is on, which is the whole job of this layer. */
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

static int64_t to_handle(pthread_t id) {
    int64_t h = 0;

    memcpy(&h, &id, sizeof id);
    return h;
}

static pthread_t from_handle(int64_t h) {
    pthread_t id;

    memcpy(&id, &h, sizeof id);
    return id;
}

/* What the starter hands the new thread, and where it lives: on the starter's
 * own stack, for as long as the handshake below takes.
 *
 * A pthread entry point takes one pointer and this layer needs to pass two, and
 * it does not allocate, so the two go in here and the new thread copies them
 * out before the starter is allowed to leave. */
typedef struct Start {
    void (*fn)(void *);
    void *arg;
    /* 0 until the new thread has both of the above in registers of its own. */
    uint32_t taken;
} Start;

static void *thread_entry(void *p) {
    Start *s = (Start *)p;

    void (*fn)(void *) = s->fn;
    void *arg = s->arg;

    /* Sequentially consistent rather than a release, because the starter's last
     * read of this word is what lets its frame die, and a release store orders
     * what came before it and nothing after.
     *
     * The wake below is the only thing still pointing at that frame once the
     * starter has gone, and it is safe for the reason pal_futex_wake states: it
     * compares the address and never reads through it. */
    burrow__atomic_store_u32(&s->taken, 1);
    (void)pal_futex_wake(&s->taken, INT64_MAX, NULL);

    fn(arg);
    return NULL;
}

int64_t pal_thread_create(void (*fn)(void *), void *arg, int64_t stack_bytes,
                          PalErrno *err) {
    if (fn == NULL || stack_bytes < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return PAL_INVALID_HANDLE;
    }

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(rc));
        return PAL_INVALID_HANDLE;
    }

    if (stack_bytes != 0) {
        size_t want = (size_t)stack_bytes;
        size_t min = stack_min();

        if (want < min)
            want = min;
        want = round_to_page(want);

        rc = pthread_attr_setstacksize(&attr, want);
        if (rc != 0) {
            (void)pthread_attr_destroy(&attr);
            BURROW_OUT(err, burrow__pal_errno(rc));
            return PAL_INVALID_HANDLE;
        }
    }

    Start s;
    s.fn = fn;
    s.arg = arg;
    s.taken = 0;

    pthread_t id;
    rc = pthread_create(&id, &attr, thread_entry, &s);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(rc));
        return PAL_INVALID_HANDLE;
    }

    /* The handshake. pal_futex_wait checks the word itself before it does
     * anything else, so a thread that got started and ran before this line was
     * reached costs a load and no system call at all. */
    while (burrow__atomic_load_acquire_u32(&s.taken) == 0)
        (void)pal_futex_wait(&s.taken, 0, -1, NULL);

    BURROW_OUT(err, PAL_OK);
    return to_handle(id);
}

bool pal_thread_join(int64_t thread, PalErrno *err) {
    if (thread == PAL_INVALID_HANDLE) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    int rc = pthread_join(from_handle(thread), NULL);
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(rc));
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_thread_detach(int64_t thread, PalErrno *err) {
    if (thread == PAL_INVALID_HANDLE) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    int rc = pthread_detach(from_handle(thread));
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(rc));
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

int64_t pal_thread_self(void) {
    /* The same copy the handle uses, for the same reason. It is an identity and
     * not the thread id a debugger shows: that needs a different call on every
     * system and nothing needs it until tracing does. */
    return to_handle(pthread_self());
}

void pal_thread_yield(void) {
    (void)sched_yield();
}

/* Asking a pthreads system where the current thread's stack is.
 *
 * There is no POSIX call for this, so there is one of these per system. What
 * they have in common is that all of them answer for the thread that is asking
 * and several of them answer for no other, which is why the header says to only
 * ask about yourself. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

bool pal_thread_stack_bounds(void **lo, void **hi) {
    /* Darwin gives the top and the size, which is the same pair the other way
     * round. Both calls are declared in pthread.h with no feature macro and
     * neither of them can fail for the calling thread. */
    char *top = (char *)pthread_get_stackaddr_np(pthread_self());
    size_t size = pthread_get_stacksize_np(pthread_self());

    if (top == NULL || size == 0)
        return false;

    BURROW_OUT(lo, (void *)(top - size));
    BURROW_OUT(hi, (void *)top);
    return true;
}

#elif defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO)

/* Cosmopolitan's libc has the glibc call, and answers it on every system one of
 * its binaries runs on. */
bool pal_thread_stack_bounds(void **lo, void **hi) {
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

    BURROW_OUT(lo, base);
    BURROW_OUT(hi, (void *)((char *)base + size));
    return true;
}

#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                       \
    defined(BURROW_OS_DRAGONFLY) || defined(BURROW_OS_SOLARIS)

bool pal_thread_stack_bounds(void **lo, void **hi) {
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

    BURROW_OUT(lo, base);
    BURROW_OUT(hi, (void *)((char *)base + size));
    return true;
}

#elif defined(BURROW_OS_OPENBSD)

bool pal_thread_stack_bounds(void **lo, void **hi) {
    /* OpenBSD's is the odd one and the odd part is ss_sp, which is the top of
     * the stack here and the bottom of it in the sigaltstack that stack_t was
     * borrowed from. */
    stack_t seg;

    if (pthread_stackseg_np(pthread_self(), &seg) != 0)
        return false;
    if (seg.ss_sp == NULL || seg.ss_size == 0)
        return false;

    BURROW_OUT(lo, (void *)((char *)seg.ss_sp - seg.ss_size));
    BURROW_OUT(hi, seg.ss_sp);
    return true;
}

#else

bool pal_thread_stack_bounds(void **lo, void **hi) {
    /* Nobody has written this one. The header says false means exactly that and
     * the stack walker does less rather than guessing. */
    (void)lo;
    (void)hi;
    return false;
}

#endif

#endif /* !BURROW_OS_WINDOWS */
