/* The public side of symbolisation: Caller, CallersFrames, FuncForPC, Stack.
 *
 * All four are thin. burrow/symtab.h already answers the only hard question,
 * which is what function an address is in, and src/runtime/trace.c already
 * walks the stack and already knows how to print a frame. What is left is
 * Go's four shapes on top of those two, and getting the frame counting right.
 *
 * The counting is the part worth reading twice. runtime_callers numbers its own
 * frame zero and runtime_caller numbers its caller zero, which is a difference
 * of one for no reason anybody can defend. It is Go's difference, though, and
 * it is in every piece of Go code that skips frames, so burrow has it too.
 * Every skip in this file is therefore adjusted rather than passed through, and
 * every function that a skip is counted from is marked so the compiler cannot
 * fold it into its caller and move the numbering by one.
 *
 * Nothing here allocates and nothing here waits for a lock, because the callers
 * are logging frameworks and failing programs, and one of those two is having a
 * bad day already.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime.h"

#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/sched.h"
#include "burrow/slice.h"
#include "burrow/symtab.h"
#include "burrow/trace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* How many frames one call to runtime_stack walks, and how many goroutines it
 * names when it was asked for all of them.
 *
 * Both are caps on stack space rather than on anything interesting: the two
 * arrays together are a little over a kilobyte, which a goroutine stack can
 * spare and which a thread that is about to die can certainly spare. Sixty four
 * frames is what BURROW_TRACEBACK_MAX is and what an uncaught panic prints. A
 * program with more than sixty four goroutines gets a line saying how many were
 * left out, which is the number that actually matters in that case. */
#define STACK_FRAMES BURROW_TRACEBACK_MAX
#define STACK_GOROUTINES 64

/* -------------------------------------------------------------- one address */

bool runtime_func_for_pc(Uintptr pc, RuntimeFunc *out) {
    burrow__Frame f;

    if (out == NULL)
        return false;

    out->name = BURROW_STR_EMPTY;
    out->entry = 0;

    /* Given exactly as it came, which is Go's rule for FuncForPC and is why
     * Go's own documentation warns about handing it a return address. The
     * subtraction belongs to whoever knows the address came off a stack, and
     * runtime_frames_next below is the one that knows. */
    if (!burrow__symbolise(pc, &f))
        return false;

    out->name = f.name;
    out->entry = f.entry;
    return true;
}

BURROW_NOINLINE bool runtime_caller(Int skip, Uintptr *pc, Str *file, Int *line) {
    Uintptr buf[1];
    Slice one = {buf, 1, 1, NULL};

    if (skip < 0)
        skip = 0;

    /* Two, not one. runtime_callers gives its own frame number zero and this
     * function's frame number one, so the caller this one was asked about is
     * number two. */
    if (runtime_callers(skip + 2, one) <= 0)
        return false;

    BURROW_OUT(pc, buf[0]);
    BURROW_OUT(file, BURROW_STR_EMPTY);
    BURROW_OUT(line, 0);
    return true;
}

/* ----------------------------------------------------------- a slice of them */

RuntimeFrames runtime_callers_frames(Slice pcs) {
    RuntimeFrames it;

    it.pcs = (const Uintptr *)pcs.p;
    it.len = pcs.p != NULL && pcs.len > 0 ? pcs.len : 0;
    it.at = 0;
    return it;
}

bool runtime_frames_next(RuntimeFrames *it, RuntimeFrame *out) {
    burrow__Frame f;
    Uintptr pc;

    if (out == NULL)
        return false;

    out->pc = 0;
    out->entry = 0;
    out->function = BURROW_STR_EMPTY;
    out->file = BURROW_STR_EMPTY;
    out->line = 0;

    if (it == NULL || it->pcs == NULL || it->at < 0 || it->at >= it->len)
        return false;

    pc = it->pcs[it->at++];
    out->pc = pc;

    /* One below, for the reason burrow__frame_text gives: these are return
     * addresses and a return address is the instruction after the call. A frame
     * with no name still counts as a frame, so this is the one place a failed
     * lookup does not become a false answer. */
    if (pc != 0 && burrow__symbolise(pc - 1, &f)) {
        out->function = f.name;
        out->entry = f.entry;
    }
    return true;
}

/* ------------------------------------------------------------- a whole stack */

/* A buffer that fills up and then stops, quietly.
 *
 * Nothing here can fail in a way the caller has to handle, because the caller
 * is writing a crash report and there is nothing useful for it to do about a
 * report that did not fit. What it gets instead is fewer lines, each of them
 * whole. */
typedef struct Out {
    Byte *p;
    Int cap;
    Int len;
    bool full;
} Out;

static void out_put(Out *o, const char *s) {
    size_t n = strlen(s);

    if (o->full)
        return;
    if ((Int)n > o->cap - o->len) {
        o->full = true;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += (Int)n;
}

/* What Go puts in the brackets after the goroutine number.
 *
 * Go's words are more specific than these, because Go knows why a goroutine is
 * waiting and prints "chan receive" or "select" or "sleep". burrow's scheduler
 * records the status and not the reason, so waiting is as far as this goes.
 * Filling in the reason is a field on the G and a line at each park, and it
 * belongs with the work that makes the other goroutines walkable at all. */
static const char *status_name(uint32_t status) {
    switch ((burrow__GStatus)status) {
    case BURROW_GIDLE:
        return "idle";
    case BURROW_GRUNNABLE:
        return "runnable";
    case BURROW_GRUNNING:
        return "running";
    case BURROW_GSYSCALL:
        return "syscall";
    case BURROW_GWAITING:
        return "waiting";
    case BURROW_GDEAD:
        return "dead";
    default:
        return "unknown";
    }
}

/* The goroutines other than this one, as a line each.
 *
 * Split out because runtime_stack is long enough without it and because this
 * half is the half that is going to be replaced: when the scheduler can hand
 * over a parked goroutine's frame pointer, this grows a walk and stops being a
 * list of excuses. */
static void other_goroutines(Out *o, uint64_t self) {
    burrow__GInfo gs[STACK_GOROUTINES];
    char line[BURROW_FRAME_TEXT_MAX];
    int32_t total = 0;
    int32_t got = burrow__allg_snapshot(gs, STACK_GOROUTINES, &total);

    if (got < 0) {
        out_put(o, "\nother goroutines not listed: the scheduler was busy\n");
        return;
    }

    for (int32_t i = 0; i < got; i++) {
        /* This goroutine's frames are already above, and a dead one is a G on a
         * free list waiting to be somebody else, which is noise. Go leaves the
         * dead ones out for the same reason. */
        if (gs[i].id == self || gs[i].status == BURROW_GDEAD)
            continue;

        snprintf(line, sizeof line, "\ngoroutine %llu [%s]:\n\tstack not walked\n",
                 (unsigned long long)gs[i].id, status_name(gs[i].status));
        out_put(o, line);
    }

    if (total > got) {
        snprintf(line, sizeof line, "\n%d more goroutines not listed\n", total - got);
        out_put(o, line);
    }
}

BURROW_NOINLINE Int runtime_stack(Slice buf, bool all) {
    Uintptr pcs[STACK_FRAMES];
    Slice frames = {pcs, STACK_FRAMES, STACK_FRAMES, NULL};
    char line[BURROW_FRAME_TEXT_MAX];
    burrow__G *g;
    Out o;
    Int n;

    o.p = (Byte *)buf.p;
    o.cap = buf.len;
    o.len = 0;
    o.full = false;

    if (o.p == NULL || o.cap <= 0)
        return 0;

    /* A thread that is not running a goroutine has no number to give. Go has no
     * such thread and so has nothing to call this line; burrow does, since a
     * program can use the library without ever starting the scheduler. */
    g = burrow__curg();
    if (g != NULL)
        snprintf(line, sizeof line, "goroutine %llu [running]:\n",
                 (unsigned long long)g->id);
    else
        snprintf(line, sizeof line, "thread [running]:\n");
    out_put(&o, line);

    /* Two again, and for the same reason: frame zero is runtime_callers and
     * frame one is this function, so the first frame worth printing is two. */
    n = runtime_callers(2, frames);
    for (Int i = 0; i < n; i++) {
        if (burrow__frame_text(pcs[i], line, (Int)sizeof line) > 0)
            out_put(&o, line);
    }

    if (all)
        other_goroutines(&o, g != NULL ? g->id : 0);

    return o.len;
}
