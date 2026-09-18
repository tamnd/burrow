/* Memory for a goroutine to run on, with a page underneath it that faults.
 *
 * burrow/context.h switches between stacks and does not care where one came
 * from. This is where one comes from. A stack here is a mapping the library
 * owns, with one or more unreadable pages immediately below the usable part, so
 * that a goroutine which runs off the bottom hits memory that is not there
 * instead of quietly writing into whatever was mapped next.
 *
 *     burrow__Stack s;
 *     if (!burrow__stack_alloc(&s, 64 * 1024))
 *         return false;
 *     size_t size = (size_t)((char *)s.hi - (char *)s.lo);
 *     burrow__context_make(&ctx, s.lo, size, run, arg, &main_ctx);
 *     burrow__stack_free(&s);
 *
 * Stacks grow down everywhere burrow runs, so `hi` is where a context starts
 * and `lo` is the last address it may touch. The guard sits below `lo`.
 *
 * ------------------------------------------------------------ the second half
 *
 * A guard page on its own turns a silent corruption into a segmentation fault,
 * which is better but is not a message. The rest of this file turns the fault
 * into "fatal error: stack overflow", which is what Go prints. It takes three
 * things, in this order:
 *
 *   burrow__stack_guard_arm()          once per process, installs the handler
 *   burrow__stack_guard_arm_thread()   once per thread, gives it somewhere to run
 *   burrow__stack_set_current(&s)      says which stack this thread is on now
 *
 * The middle one matters more than it looks. A handler for a stack overflow
 * cannot run on the stack that overflowed, so it needs a signal stack of its
 * own, and that is per thread. A thread that skips it gets the fault and no
 * message, which is where it started.
 *
 * The last one is what the scheduler will call on every switch, and until there
 * is a scheduler a caller sets it by hand. What the guard does not catch is
 * written down next to burrow__stack_guard_arm and is worth reading first.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_STACK_H
#define BURROW_STACK_H

#include "burrow/own.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The smallest usable stack burrow__stack_alloc will hand out.
 *
 * The same number as BURROW_CONTEXT_STACK_MIN, and that is on purpose rather
 * than a coincidence: the only reason to allocate one of these is to run a
 * context on it, so a stack this file is willing to produce has to be one that
 * file is willing to accept. */
#define BURROW_STACK_MIN 16384

/* A mapping to run on. Three fields and none of them are derived from the
 * others, so there is nothing here that can disagree with itself.
 *
 * All zeroes is a stack that was never allocated or has been freed. Freeing one
 * of those is allowed and does nothing. */
typedef struct burrow__Stack {
    /* The lowest address a context on this stack may touch. Page aligned. */
    void *lo;
    /* One past the highest. This is where a context starts, because stacks grow
     * down. Page aligned. */
    void *hi;
    /* How many unreadable bytes sit immediately below lo. Always at least one
     * page on a system that has them, and reading or writing any of them is
     * what the overflow handler is looking for. */
    size_t guard;
} burrow__Stack;

/* The system's page size, which is what every size here is rounded to.
 *
 * It is 4096 on most things, 16384 on arm64 macOS, and 65536 on some arm64
 * Linux kernels, which is a big enough spread that rounding to a guessed
 * constant would waste most of a stack on one system or fail to align on
 * another. Cheap to call: it is a memory read on the libcs burrow targets
 * rather than a system call. */
size_t burrow__stack_page_size(void);

/* Maps a stack of at least `size` usable bytes with a guard below it.
 *
 * `size` is rounded up to a whole page and raised to BURROW_STACK_MIN if it is
 * below it, so the stack you get is at least the one you asked for and is
 * usually a little larger. Ask `hi` and `lo` what you actually got.
 *
 * False means the system would not give out the memory, which is an address
 * space or a mapping count limit rather than anything the caller did wrong. It
 * is not fatal, because a scheduler that cannot start one more goroutine can
 * still run the ones it has.
 *
 * The guard is never counted in the usable size. Asking for 64 kilobytes gives
 * 64 kilobytes to run on and costs a page more than that. */
bool burrow__stack_alloc(burrow__Stack *stack, size_t size);

/* Unmaps the stack, guard and all, and zeroes the struct.
 *
 * Nothing may be running on it, which is to say the context that was using it
 * has returned or has been switched away from for the last time. Safe on a
 * stack that was never allocated, and safe to call twice, because the first
 * call leaves it looking like the second case. */
void burrow__stack_free(burrow__Stack *stack);

/* Installs the process wide handler that turns a guard page fault into
 * "fatal error: stack overflow".
 *
 * Two things this does not catch, both worth knowing before trusting it.
 *
 * A guard is one page, and a single stack frame larger than a page can step
 * straight over it into whatever is below. Go does not have this problem
 * because the Go compiler puts a bounds check in every function prologue, and
 * there is nothing a library can reach for that does the same.
 * `-fstack-clash-protection` on gcc and clang makes a large frame touch each
 * page on the way down, which closes it for code built with that flag and
 * leaves it open for code built without. A bigger guard raises the frame size
 * it takes to jump the gap, which is a mitigation rather than a fix.
 *
 * On Windows the fault arrives at a vectored exception handler, which runs on
 * the stack that faulted, and the stack that faulted is by definition the one
 * with nothing left on it. It is best effort there in a way it is not
 * elsewhere, and the Win64 context switch assembly is what will fix it, since
 * that is what lets the thread information block describe the goroutine stack
 * and lets Windows' own guard machinery do this job.
 *
 * Once per process. Calling it again does nothing and answers true, so it is
 * safe to call from whatever initialises first rather than having to find a
 * single place for it.
 *
 * On POSIX this is a SIGSEGV and SIGBUS handler. It only claims a fault whose
 * address is inside the guard of the stack the faulting thread said it was on,
 * so a segmentation fault in code that has nothing to do with burrow still
 * reaches whatever was going to handle it and still produces a core file.
 *
 * Whatever was installed before is kept and chained to, which means burrow can
 * be linked into a program that has its own handler without either of them
 * losing. It does mean this has to be called after that program installs its
 * own, not before, and that is the ordinary rule for this kind of thing.
 *
 * False means the system refused to install a handler, which does not stop
 * anything else here from working. The guard pages are still guard pages. */
bool burrow__stack_guard_arm(void);

/* Gives the calling thread a signal stack for the handler to run on.
 *
 * Every thread that will run goroutines calls this once, after
 * burrow__stack_guard_arm. A handler for a stack overflow cannot run on the
 * stack that overflowed, so without this the thread takes the fault and dies
 * with no message, which is the behaviour this whole file exists to replace.
 *
 * It maps a small stack of its own, which is freed by the disarm below. Calling
 * it twice on one thread does nothing the second time and answers true.
 *
 * Does nothing and answers true on Windows, which has no equivalent. */
bool burrow__stack_guard_arm_thread(void);

/* Gives back what the arm above took. Call it before the thread ends, or the
 * mapping stays until the process does. Safe on a thread that never armed. */
void burrow__stack_guard_disarm_thread(void);

/* Tells the handler which stack this thread is running on, and answers with
 * whichever one it was told last.
 *
 * The scheduler will call this on every context switch. Passing NULL says the
 * thread is back on the one the operating system gave it, which has a guard of
 * its own that the operating system looks after.
 *
 * Returning the previous value is what makes it nest without a caller having to
 * keep a copy: set it, run, set back what you were given. */
BURROW_BORROWS(ret) BURROW_RETAINS(stack) burrow__Stack *
burrow__stack_set_current(burrow__Stack *stack);

/* What the calling thread last passed to the setter above, or NULL. */
BURROW_BORROWS(ret) burrow__Stack *burrow__stack_current(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_STACK_H */
