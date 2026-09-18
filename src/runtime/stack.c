/* Mapping a stack with a guard under it, and saying so when something lands on
 * the guard.
 *
 * See burrow/stack.h for what the pieces are for. Two implementations: mmap and
 * a POSIX signal handler, or VirtualAlloc and a vectored exception handler.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Before every include, because a feature macro only counts if nothing has been
 * included yet. C11 on its own gives signal() and raise() and nothing else, so
 * sigaction, siginfo_t, sigaltstack and SA_ONSTACK are all invisible to a strict
 * build without this, on glibc and on musl alike.
 *
 * The other two are there because MAP_ANONYMOUS is not in POSIX, and asking a
 * libc for POSIX is also telling it that is all you want. Every one of them
 * hands the flag over without being asked and takes it away again the moment
 * _XOPEN_SOURCE appears, which is a strange way to lose an argument to mmap and
 * took a while to work out the first time. _DEFAULT_SOURCE puts it back on
 * glibc and _DARWIN_C_SOURCE puts it back on macOS. Neither of them is in the
 * way of the other, and musl wants neither.
 *
 * glibc 2.36 hides it and glibc 2.41 does not, so the build that finds this is
 * whichever machine has the older one. Here that was 32 bit Debian, after the
 * same code had already gone green on two newer distributions.
 *
 * _WIN32 rather than BURROW_OS_WINDOWS for the reason rand.c gives: platform.h
 * is what defines that, and platform.h is an include. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/stack.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Which stack the calling thread says it is on. The handler needs bounds to
 * compare a faulting address against, and this is where they come from. */
static BURROW_THREAD_LOCAL burrow__Stack *current;

burrow__Stack *burrow__stack_set_current(burrow__Stack *stack) {
    burrow__Stack *was = current;
    current = stack;
    return was;
}

burrow__Stack *burrow__stack_current(void) {
    return current;
}

/* Whether the faulting address is in the guard of the stack the faulting thread
 * said it was on. Everything else about the fault is somebody else's business.
 *
 * Called from a signal handler, so it reads a thread local and does arithmetic
 * and nothing else. */
static bool in_guard(const void *addr) {
    const burrow__Stack *s = current;
    if (s == NULL || s->lo == NULL || s->guard == 0)
        return false;

    uintptr_t where = (uintptr_t)addr;
    uintptr_t lo = (uintptr_t)s->lo;
    return where < lo && where >= lo - s->guard;
}

/* What both handlers say. Go prints exactly this and the prefix comes from
 * runtime_throw, so the whole line is "fatal error: stack overflow".
 *
 * runtime_throw is not async signal safe. It formats and it writes to stderr,
 * neither of which a handler is supposed to do, and doing it anyway is the
 * right trade here: the alternative is a program that dies with no message at
 * all, and the process is ending either way. It is written down rather than
 * left to be discovered. */
BURROW_NORETURN static void report_overflow(void) {
    runtime_throw(BURROW_S("stack overflow"));
}

/* Rounds up to a whole page, or gives back what it was handed if that would
 * wrap, since a size that close to the top is not a real request and the
 * allocator below is about to refuse it anyway. */
static size_t round_up(size_t bytes, size_t page) {
    if (page == 0)
        return bytes;

    size_t over = bytes % page;
    if (over == 0)
        return bytes;
    if (bytes > SIZE_MAX - (page - over))
        return bytes;

    return bytes + (page - over);
}

#if defined(BURROW_OS_WINDOWS)

/* ------------------------------------------------------------------ windows */

size_t burrow__stack_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwPageSize;
}

bool burrow__stack_alloc(burrow__Stack *stack, size_t size) {
    if (stack == NULL)
        return false;

    stack->lo = NULL;
    stack->hi = NULL;
    stack->guard = 0;

    size_t page = burrow__stack_page_size();
    if (size < BURROW_STACK_MIN)
        size = BURROW_STACK_MIN;
    size = round_up(size, page);
    if (size < BURROW_STACK_MIN || size > SIZE_MAX - page)
        return false;

    /* Reserve the guard and the stack in one go so they are next to each other,
     * then commit only the stack. The guard stays reserved and uncommitted,
     * which on Windows is memory that is spoken for and has nothing behind it,
     * so touching it is an access violation. That is the same outcome as
     * PAGE_NOACCESS and it does not cost a page of commit charge. */
    size_t total = page + size;
    void *base = VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_READWRITE);
    if (base == NULL)
        return false;

    void *lo = (unsigned char *)base + page;
    if (VirtualAlloc(lo, size, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        (void)VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }

    stack->lo = lo;
    stack->hi = (unsigned char *)lo + size;
    stack->guard = page;
    return true;
}

void burrow__stack_free(burrow__Stack *stack) {
    if (stack == NULL || stack->lo == NULL)
        return;

    /* MEM_RELEASE frees the whole reservation and insists the address is the
     * one the reservation started at, which is the guard rather than lo. */
    void *base = (unsigned char *)stack->lo - stack->guard;
    (void)VirtualFree(base, 0, MEM_RELEASE);

    stack->lo = NULL;
    stack->hi = NULL;
    stack->guard = 0;
}

/* The handle from AddVectoredExceptionHandler, kept so that arming twice can
 * tell it has already happened. */
static void *veh;

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info) {
    if (info == NULL || info->ExceptionRecord == NULL)
        return EXCEPTION_CONTINUE_SEARCH;

    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;

    /* ExceptionInformation[0] is read or write and [1] is the address. */
    if (!in_guard((const void *)rec->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_SEARCH;

    report_overflow();
}

bool burrow__stack_guard_arm(void) {
    if (veh != NULL)
        return true;

    /* First rather than last, so that a debugger's own handler does not get
     * there before this one and decide the access violation is worth stopping
     * on. A real access violation is passed on untouched. */
    veh = AddVectoredExceptionHandler(1, on_exception);
    return veh != NULL;
}

bool burrow__stack_guard_arm_thread(void) {
    /* A vectored handler runs on whatever stack faulted, so there is no signal
     * stack to set up and nothing to do here. It is also why the header says
     * Windows is best effort: the stack that just overflowed is exactly the one
     * with no room on it. */
    return true;
}

void burrow__stack_guard_disarm_thread(void) {}

#else

/* -------------------------------------------------------------------- posix */

size_t burrow__stack_page_size(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return 4096;
    return (size_t)page;
}

bool burrow__stack_alloc(burrow__Stack *stack, size_t size) {
    if (stack == NULL)
        return false;

    stack->lo = NULL;
    stack->hi = NULL;
    stack->guard = 0;

    size_t page = burrow__stack_page_size();
    if (size < BURROW_STACK_MIN)
        size = BURROW_STACK_MIN;
    size = round_up(size, page);
    if (size < BURROW_STACK_MIN || size > SIZE_MAX - page)
        return false;

    /* One mapping for the guard and the stack together, so that the guard
     * cannot end up somewhere else and something cannot be mapped in between.
     * Read and write to start with, and then the guard is taken away again,
     * which is one system call more than mapping them separately and one race
     * less. */
    size_t total = page + size;
    void *base =
        mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return false;

    if (mprotect(base, page, PROT_NONE) != 0) {
        (void)munmap(base, total);
        return false;
    }

    stack->lo = (unsigned char *)base + page;
    stack->hi = (unsigned char *)base + total;
    stack->guard = page;
    return true;
}

void burrow__stack_free(burrow__Stack *stack) {
    if (stack == NULL || stack->lo == NULL)
        return;

    void *base = (unsigned char *)stack->lo - stack->guard;
    size_t total = stack->guard +
                   (size_t)((unsigned char *)stack->hi - (unsigned char *)stack->lo);
    (void)munmap(base, total);

    stack->lo = NULL;
    stack->hi = NULL;
    stack->guard = 0;
}

/* 0 nobody has armed, 1 somebody is in the middle of it, 2 done. Three states
 * rather than two because the thread that loses the race has to wait for the
 * handler to actually be installed before it says yes, and a plain flag cannot
 * tell "not yet" from "in progress". */
#define ARM_IDLE 0U
#define ARM_BUSY 1U
#define ARM_DONE 2U

static uint32_t armed;

/* What was there before, so a program with its own SIGSEGV handler keeps it.
 * Written once by the thread that wins the arm and read by the handler, which
 * only runs after the write is published. */
static struct sigaction prev_segv;
static struct sigaction prev_bus;

/* The signal stack this thread is using, in the same struct as a goroutine
 * stack because it is the same thing: a mapping with a size. Its guard is zero,
 * since a signal stack that overflows has nowhere better to go anyway. */
static BURROW_THREAD_LOCAL burrow__Stack signal_stack;

/* Hands the signal back to whoever had it and lets it happen again.
 *
 * A handler that returns from a fault on a guard page would fault again at the
 * same instruction forever, so the only way out of "this one is not mine" is to
 * put the old disposition back first. If that was SIG_DFL the program then dies
 * the way it would have, core file and all, and if it was another handler that
 * handler sees it. */
static void not_ours(int sig, siginfo_t *info, void *uc) {
    const struct sigaction *prev = (sig == SIGBUS) ? &prev_bus : &prev_segv;

    /* Both sides cast, because sa_flags is a signed int and so is SA_SIGINFO on
     * every libc here, and masking one signed thing with another is the kind of
     * thing a linter is right to ask about even when the bit in question is a
     * small positive constant. The types are the libc's to choose, so the cast
     * is the only end of it we own. */
    if (((unsigned int)prev->sa_flags & (unsigned int)SA_SIGINFO) != 0U &&
        prev->sa_sigaction != NULL) {
        prev->sa_sigaction(sig, info, uc);
        return;
    }

    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN &&
        prev->sa_handler != NULL) {
        prev->sa_handler(sig);
        return;
    }

    (void)sigaction(sig, prev, NULL);
}

static void on_fault(int sig, siginfo_t *info, void *uc) {
    /* SIGBUS as well as SIGSEGV because which one a guard page produces is not
     * the same everywhere. Linux says SIGSEGV, macOS says SIGBUS for some of
     * these, and the address is in the same place in both. */
    if (info != NULL && in_guard(info->si_addr))
        report_overflow();

    not_ours(sig, info, uc);
}

bool burrow__stack_guard_arm(void) {
    uint32_t state = burrow__atomic_load_acquire_u32(&armed);
    if (state == ARM_DONE)
        return true;

    uint32_t idle = ARM_IDLE;
    if (!burrow__atomic_cas_u32(&armed, &idle, ARM_BUSY)) {
        /* Somebody else got there first. Wait for them to finish rather than
         * answering yes while the handler is still not installed, because a
         * caller that gets true and then overflows would get no message. */
        while (burrow__atomic_load_acquire_u32(&armed) != ARM_DONE)
            burrow__atomic_spin_hint();
        return true;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    /* SA_ONSTACK is the whole point: it says run this on the signal stack,
     * which is the only stack left when the ordinary one has overflowed.
     * SA_SIGINFO is what makes the faulting address available at all. */
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    (void)sigemptyset(&sa.sa_mask);

    bool ok = sigaction(SIGSEGV, &sa, &prev_segv) == 0;
    if (ok && sigaction(SIGBUS, &sa, &prev_bus) != 0) {
        (void)sigaction(SIGSEGV, &prev_segv, NULL);
        ok = false;
    }

    burrow__atomic_store_release_u32(&armed, ok ? ARM_DONE : ARM_IDLE);
    return ok;
}

/* How big a signal stack has to be.
 *
 * MINSIGSTKSZ is 5120 on glibc, 6144 on musl and 32768 on macOS arm64, and on
 * glibc since 2.34 it is not a constant at all but a call into the dynamic
 * loader, because a machine with wider vector registers needs a bigger signal
 * frame. So the number is asked for at runtime and a floor is applied under it,
 * and the floor is 64 kilobytes because the handler formats a message and
 * writes it, which MINSIGSTKSZ does not account for. */
static size_t signal_stack_size(void) {
    size_t want = (size_t)64 * 1024;

#if defined(_SC_SIGSTKSZ)
    long answer = sysconf(_SC_SIGSTKSZ);
    if (answer > 0 && (size_t)answer > want)
        want = (size_t)answer;
#endif

    if ((size_t)MINSIGSTKSZ > want)
        want = (size_t)MINSIGSTKSZ;

    return round_up(want, burrow__stack_page_size());
}

bool burrow__stack_guard_arm_thread(void) {
    if (signal_stack.lo != NULL)
        return true;

    size_t size = signal_stack_size();
    void *base =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return false;

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = base;
    ss.ss_size = size;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, NULL) != 0) {
        (void)munmap(base, size);
        return false;
    }

    signal_stack.lo = base;
    signal_stack.hi = (unsigned char *)base + size;
    signal_stack.guard = 0;
    return true;
}

void burrow__stack_guard_disarm_thread(void) {
    if (signal_stack.lo == NULL)
        return;

    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    (void)sigaltstack(&off, NULL);

    size_t size =
        (size_t)((unsigned char *)signal_stack.hi - (unsigned char *)signal_stack.lo);
    (void)munmap(signal_stack.lo, size);

    signal_stack.lo = NULL;
    signal_stack.hi = NULL;
}

#endif
