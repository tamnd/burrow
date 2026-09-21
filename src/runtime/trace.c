/* Following the frame pointers, and asking Windows to do it instead.
 *
 * Two implementations of one loop. The interesting half is the checking rather
 * than the walking: a frame pointer chain is a linked list built by the code
 * that is currently failing, so by the time anybody wants to walk it, it may
 * well be the thing that is wrong. Every address it hands out is therefore
 * checked against the bounds of the stack it claims to be on, every step has to
 * move up that stack, and the walk stops the moment either of those is false.
 * A trace that stops early is a trace with the top frames in it, which is where
 * the answer usually is. A walk that faults is a second crash on top of the
 * first one, with the original message already gone.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/trace.h"

#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/stack.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#endif

/* Where the pair of words lives, when it lives anywhere.
 *
 * amd64 and 386 keep the caller's frame pointer at the address the frame
 * pointer holds and the return address one word above it, which is the same
 * shape at both widths. arm64 keeps the same pair in the same order, because
 * the procedure call standard says the frame record is exactly that and says
 * the frame pointer points at it. Those are the three burrow is built and
 * tested on. Anything else gets no walk rather than a layout read off a
 * reference manual by somebody who could not run it: riscv64 and loong64 put
 * the pair below the frame pointer instead, ppc64 and s390x use a back chain
 * with the link register saved somewhere else again, and each of those is a few
 * lines to add for whoever has the hardware to check them on. */
#if defined(BURROW_ARCH_AMD64) || defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_386)
#define TRACE_FRAME_PAIR 1
#endif

#if !defined(BURROW_OS_WINDOWS) && defined(TRACE_FRAME_PAIR)

/* Room for the pair, and on a boundary a frame pointer could actually be on.
 *
 * The size check is what keeps the read inside the mapping, since a frame
 * pointer one word below the top of the stack has its return address slot off
 * the end of it. The alignment check is not about faulting, both of these
 * architectures read an unaligned word without complaining, it is about being
 * cheap evidence that this value was ever a frame pointer at all. */
static bool readable(const void *p, const void *lo, const void *hi) {
    const char *c = (const char *)p;

    if (c < (const char *)lo || c + 2 * sizeof(void *) > (const char *)hi)
        return false;
    return (uintptr_t)c % sizeof(void *) == 0;
}

/* Which stack the walk is allowed to touch.
 *
 * A goroutine runs on a stack burrow mapped and a thread runs on one the system
 * gave it, and the frame that is asking decides which of the two this is: the
 * scheduler tells stack.c which goroutine stack this thread is on, but that
 * answer is still there while the thread is back on its own stack between
 * goroutines. So the goroutine stack is used when the asking frame is on it and
 * the thread's own is used otherwise, and a frame on neither is a frame this
 * refuses to walk from. */

/* Asked for once per thread and kept, because asking is not cheap and the
 * answer does not change.
 *
 * Not cheap is the operative word. On Linux the call behind this reads
 * /proc/self/maps, and on macOS the size half of it goes through getrlimit for
 * the main thread, so a walk that asked every time would spend all of its time
 * in the kernel and none of it on the stack. Measured on Linux: fifty
 * microseconds a walk asking every time against forty seven nanoseconds asking
 * once, which is three orders of magnitude for a value that cannot change.
 *
 * It cannot change because a thread is given its stack before it runs any of
 * this and keeps it until it exits, and thread local storage is per thread by
 * definition, so there is nothing to invalidate and no thread to invalidate it
 * for. */
static BURROW_THREAD_LOCAL void *thread_lo;
static BURROW_THREAD_LOCAL void *thread_hi;
static BURROW_THREAD_LOCAL bool thread_asked;

static bool bounds_for(const void *frame, void **lo, void **hi) {
    burrow__Stack *s = burrow__stack_current();

    if (s != NULL && s->lo != NULL && readable(frame, s->lo, s->hi)) {
        *lo = s->lo;
        *hi = s->hi;
        return true;
    }
    if (!thread_asked) {
        thread_asked = true;
        if (!burrow__thread_stack_bounds(&thread_lo, &thread_hi))
            thread_lo = NULL;
    }
    if (thread_lo == NULL)
        return false;

    *lo = thread_lo;
    *hi = thread_hi;
    return readable(frame, *lo, *hi);
}

Int burrow__callers(void *from, Int skip, Uintptr *pcs, Int max) {
    void **frame = (void **)from;
    void *lo;
    void *hi;
    Int n = 0;

    if (frame == NULL || pcs == NULL || max <= 0 || skip < 0)
        return 0;
    if (!bounds_for((const void *)frame, &lo, &hi))
        return 0;

    while (n < max) {
        void **next = (void **)frame[0];
        Uintptr pc = (Uintptr)frame[1];

        /* The outermost frame on a thread has a zero return address in it,
         * which is how the system says the stack ends. Some libcs leave a zero
         * frame pointer there instead and the step below catches that one. */
        if (pc == 0)
            break;
        if (skip > 0)
            skip--;
        else
            pcs[n++] = pc;

        /* Up, by more than nothing, and onto a frame that is still on this
         * stack. The first of those three is what guarantees this loop ends:
         * the stack is finite and every turn moves towards the top of it. */
        if (next <= frame || !readable((const void *)next, lo, hi))
            break;
        frame = next;
    }
    return n;
}

#elif defined(BURROW_OS_WINDOWS)

/* As many as one call asks for, which is as many as Windows will give at once.
 *
 * The first call has to reach past the frames burrow put on the stack to find
 * the one that asked, and a few is all that ever takes, so a buffer this size
 * holds the search and every chunk after it. */
#define TRACE_CHUNK 64

Int burrow__callers(void *from, Int skip, Uintptr *pcs, Int max) {
    /* Windows has the walk already and it is the better one here. MSVC keeps no
     * frame pointer on amd64 at all, describing every frame with an unwind
     * table instead, and this call is what reads those tables. It is in ntdll,
     * it needs nothing linked, and it is the same call on the MinGW side, where
     * the tables are there as well.
     *
     * What it does not do is say which of the frames it returned belongs to the
     * function that asked. Counting down to it from the top is the obvious
     * answer and it is wrong: the frames in between are burrow's own, a
     * compiler may turn any of those calls into a jump that reuses a frame, and
     * a count that is one too high quietly removes a frame of the program's.
     * MSVC does exactly that. So from is a return address rather than a frame
     * pointer here, the captured stack is searched for it, and whatever the
     * optimiser did in between, the frame that asked is the frame that is
     * found. */
    PVOID frames[TRACE_CHUNK];
    Uintptr mark = (Uintptr)from;
    ULONG base = 0;
    bool found = false;
    Int n = 0;
    USHORT got;

    if (pcs == NULL || max <= 0 || skip < 0)
        return 0;

    got = RtlCaptureStackBackTrace(0, TRACE_CHUNK, frames, NULL);
    for (USHORT i = 0; i < got; i++) {
        if ((Uintptr)frames[i] == mark) {
            base = i;
            found = true;
            break;
        }
    }

    /* The asking frame is not in there, so there is nothing to be sure of and
     * the header says zero is a real answer. It takes a stack deeper than the
     * search to get here, which is deeper than this ever runs. */
    if (!found)
        return 0;

    while (n < max) {
        Int room = max - n;
        ULONG want = (ULONG)(room < TRACE_CHUNK ? room : TRACE_CHUNK);

        got =
            RtlCaptureStackBackTrace(base + (ULONG)skip + (ULONG)n, want, frames, NULL);
        if (got == 0)
            break;
        for (USHORT i = 0; i < got; i++)
            pcs[n++] = (Uintptr)frames[i];
        if ((ULONG)got < want)
            break;
    }
    return n;
}

#else

Int burrow__callers(void *from, Int skip, Uintptr *pcs, Int max) {
    /* No frame layout known for this architecture. The header says zero is a
     * normal answer and this is the platform that gives it. */
    (void)from;
    (void)skip;
    (void)pcs;
    (void)max;
    return 0;
}

#endif

void burrow__traceback(const Uintptr *pcs, Int n) {
    /* One address a line, indented under the line that said which goroutine
     * this is, which is the shape Go's traceback has. Go puts a function name
     * and a source line on each of them and burrow will too once there is a
     * symbol table to look them up in. Until then the address is what addr2line
     * and atos want anyway. */
    for (Int i = 0; i < n; i++)
        fprintf(stderr, "\t%#llx\n", (unsigned long long)pcs[i]);
}

BURROW_NOINLINE Int runtime_callers(Int skip, Slice pcs) {
    Uintptr *p = (Uintptr *)pcs.p;
    Int max = pcs.len;
    Int n = 0;

    /* The answer comes back through a volatile so that the call below cannot be
     * a jump.
     *
     * A compiler is allowed to turn a call in return position into a jump that
     * reuses this frame, and both halves of the walk need the frame to still be
     * there. The frame pointer handed over would point at a frame that has been
     * taken apart, and the Windows implementation counts frames from where it
     * was called and would count one too few. Reading the answer back out of a
     * stack slot is a couple of instructions and removes the question. MSVC was
     * seen doing the jump. */
    volatile Int got = 0;

    if (p == NULL || max <= 0)
        return 0;
    if (skip < 0)
        skip = 0;

    /* Go numbers the frame for Callers itself zero, so burrow does too, and the
     * address it writes there is one past the entry point of this function.
     * That is the one thing on this path that is not a real program counter,
     * because C has no way to name the instruction a function is currently
     * executing, and it is one past rather than exactly the entry so that the
     * subtraction symbolisation does to turn a return address back into a call
     * site lands inside this function rather than in whatever the linker put
     * before it. It resolves to runtime_callers, which is all frame zero has
     * ever been asked for. */
    if (skip == 0)
        p[n++] = (Uintptr)&runtime_callers + 1;
    else
        skip--;

    got = burrow__callers(BURROW_WALK_FROM, skip, p + n, max - n);

    return n + got;
}
