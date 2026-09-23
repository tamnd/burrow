/* Mapping a stack with a guard under it, and saying so when something lands on
 * the guard.
 *
 * See burrow/stack.h for what the pieces are for. This file used to be two of
 * everything, one over mmap and a POSIX signal handler and one over VirtualAlloc
 * and a vectored exception handler, with the system headers included right here
 * and three feature test macros above them. Both halves are in the platform
 * layer now, in src/pal/vm_posix.c and its Windows twin for the mapping and in
 * src/pal/signal_posix.c and its twin for the fault, and what is left is one
 * implementation of the thing this file is actually about: where the guard goes
 * and what it means when something touches it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/stack.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

size_t burrow__stack_page_size(void) {
    int64_t page = pal_page_size();

    /* The platform layer asks the system once and keeps the answer, so this is
     * a memory read rather than a system call and the header is right to say it
     * is cheap. Zero is not an answer any system gives and the fallback is here
     * so that the arithmetic below never divides by one. */
    if (page <= 0)
        return 4096;
    return (size_t)page;
}

/* Rounds up to a whole page, or gives back what it was handed if that would
 * wrap, since a size that close to the top is not a real request and the
 * allocator below is about to refuse it anyway. */
static size_t stack_round_up(size_t bytes, size_t page) {
    if (page == 0)
        return bytes;

    size_t over = bytes % page;
    if (over == 0)
        return bytes;
    if (bytes > SIZE_MAX - (page - over))
        return bytes;

    return bytes + (page - over);
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
    size = stack_round_up(size, page);
    if (size < BURROW_STACK_MIN || size > SIZE_MAX - page)
        return false;

    size_t total = page + size;
    if (total > (size_t)INT64_MAX)
        return false;

    /* Reserve the guard and the stack in one go so they are next to each other
     * and nothing can be mapped in between, then commit only the stack. What is
     * left over is a reservation with nothing behind it, which is address space
     * that is spoken for and cannot be touched, so a goroutine that runs off the
     * bottom faults on it. That is the guard, and it costs no memory at all.
     *
     * This is the Win32 shape, which is the one the platform layer offers,
     * because it is the one that can be emulated on POSIX and not the other way
     * round. On POSIX the reservation is a PROT_NONE mapping and committing is
     * an mprotect on part of it, so the guard is a page this process has never
     * made readable rather than one it made readable and took back. One system
     * call more than the mmap this file used to do, and one fewer way for the
     * two pieces to end up somewhere other than next to each other. */
    void *base = pal_vm_reserve((int64_t)total, NULL);
    if (base == NULL)
        return false;

    void *lo = (unsigned char *)base + page;
    if (!pal_vm_commit(lo, (int64_t)size, NULL)) {
        (void)pal_vm_release(base, (int64_t)total, NULL);
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

    /* The whole reservation, from the bottom of the guard, because that is the
     * address it started at and Windows insists on being given that one. */
    void *base = (unsigned char *)stack->lo - stack->guard;
    size_t total = stack->guard +
                   (size_t)((unsigned char *)stack->hi - (unsigned char *)stack->lo);
    (void)pal_vm_release(base, (int64_t)total, NULL);

    stack->lo = NULL;
    stack->hi = NULL;
    stack->guard = 0;
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

/* What the handler says. Go prints exactly this and the prefix comes from
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

/* Never returns when the fault is ours, and says no when it is not, which is
 * what sends it back to whatever handler the program had before burrow was
 * linked into it. */
static bool on_fault(int32_t sig, void *info, void *ctx) {
    (void)sig;
    (void)ctx;

    if (in_guard(pal_signal_fault_addr(info)))
        report_overflow();

    return false;
}

/* 0 nobody has armed, 1 somebody is in the middle of it, 2 done. Three states
 * rather than two because the thread that loses the race has to wait for the
 * handler to actually be installed before it says yes, and a plain flag cannot
 * tell "not yet" from "in progress". */
#define ARM_IDLE 0U
#define ARM_BUSY 1U
#define ARM_DONE 2U

static uint32_t armed;

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

    /* One call, and PAL_SIGFAULT is the one that is not a signal. On POSIX it
     * is SIGSEGV and SIGBUS both, because which one a guard page raises is not
     * the same on Linux as it is on macOS, and on Windows it is not a signal at
     * all. Knowing which of those three this build is was the last thing in
     * this file that cared. */
    bool ok = pal_signal_install(PAL_SIGFAULT, on_fault, NULL);

    burrow__atomic_store_release_u32(&armed, ok ? ARM_DONE : ARM_IDLE);
    return ok;
}

bool burrow__stack_guard_arm_thread(void) {
    return pal_signal_stack_install(NULL);
}

void burrow__stack_guard_disarm_thread(void) {
    pal_signal_stack_remove();
}
