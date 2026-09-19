/* Putting one stack down and picking another one up.
 *
 * A goroutine is a stack and a place to resume on it. Switching between two of
 * them means saving the registers the ABI says a function is allowed to expect
 * to survive a call, pointing the stack pointer at the other stack, and loading
 * that one's registers back. That is the whole idea, and on every machine it is
 * about thirty instructions.
 *
 * It is written in assembly because the alternatives are worse. `swapcontext`
 * does not exist on Windows, it makes a `sigprocmask` system call on every
 * switch where it does exist, and musl does not implement it at all. Fibers are
 * Windows only. So each ABI gets its own file and the portable paths are the
 * fallback rather than the plan.
 *
 * Three backends, and which one you get is decided by the machine:
 *
 *   amd64 and arm64 on anything but Windows   hand written assembly
 *   Windows                                   Fibers
 *   everything else                           ucontext
 *
 * `-DBURROW_PORTABLE_CONTEXT=1` forces the fallback everywhere it exists, which
 * is how you find out whether a bug is in the assembly or above it. The tests
 * are built both ways for that reason.
 *
 * Nothing here allocates. The caller owns the stack, passes it in, and keeps it
 * alive until the context is finished with. Guard pages are not here yet, so an
 * overflow today runs off the end of the buffer rather than hitting a page that
 * faults. That arrives with stack allocation, which is the next piece.
 *
 * Address sanitizer is told about the switch, because a sanitizer that is not
 * told believes the thread is still on the stack it was on before. Thread
 * sanitizer is not told, which is a decision and is argued for further down.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CONTEXT_H
#define BURROW_CONTEXT_H

#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which backend. BURROW_PORTABLE_CONTEXT is the switch a caller sets, and the
 * two below it are what the rest of the code actually tests, so that no file has
 * to repeat the reasoning about which portable path belongs to which system. */
#if !defined(BURROW_PORTABLE_CONTEXT)
#if defined(BURROW_OS_WINDOWS)
/* Win64 has its own ABI, its own register set to save, and a thread information
 * block whose stack bounds have to be updated on every switch or the guard page
 * machinery misfires. That assembly is worth writing and is not written yet, so
 * Windows takes Fibers, which are the operating system doing the same job. */
#define BURROW_PORTABLE_CONTEXT 1
#elif !defined(BURROW_ARCH_AMD64) && !defined(BURROW_ARCH_ARM64)
#define BURROW_PORTABLE_CONTEXT 1
#endif
#endif

#if !defined(BURROW_PORTABLE_CONTEXT)
#define BURROW_CONTEXT_ASM 1
#elif defined(BURROW_OS_WINDOWS)
#define BURROW_CONTEXT_FIBERS 1
#else
#define BURROW_CONTEXT_UCONTEXT 1
#endif

/* Whether the sanitizer annotations are compiled in.
 *
 * There are two sets of them and they answer different questions. The address
 * sanitizer gets one `__sanitizer_start_switch_fiber` on the way out of a stack
 * and one matching finish on the way in, which is how it is told that the
 * thread has moved. The thread sanitizer gets a fiber per context and one
 * `__tsan_switch_to_fiber` at those same two points, which is how it is told
 * whose call stack it is looking at. Neither is compiled unless the sanitizer it
 * belongs to is on, so an ordinary build is the same machine code it was before.
 *
 * Both are off on Fibers whatever else is true. For the address sanitizer the
 * calls want the bottom and the size of the stack being switched to, and a fiber
 * allocates its own stack somewhere Windows does not tell us about, so the only
 * numbers we could pass there are the caller's buffer, which is not the memory
 * the context runs on. Wrong numbers are worse than none. The thread sanitizer
 * does not run on Windows at all. Nothing that runs a sanitizer targets Windows
 * today, so this costs nothing, and the day it does the fix is to ask the
 * operating system for the fiber's real bounds.
 *
 * The thread sanitizer half is not a nicety. It keeps a call stack per thread,
 * pushed and popped by code it compiles into the prologue and epilogue of every
 * function it sees. A goroutine that parks on one thread and carries on from
 * another pushes on the first and pops on the second, so the two counts drift
 * apart, and in the C and C++ runtime that buffer is a fixed size with no code
 * anywhere to grow it. Enough drift and one of them walks off the end and the
 * process dies inside the sanitizer with a SEGV and no report worth reading.
 * That is what a fiber fixes: the call stack follows the goroutine rather than
 * staying with the thread.
 *
 * It is not free. A fiber there is a whole thread state, and the identifier
 * space is one way. Destroying a fiber does not give its slot back, and the
 * eight thousand one hundred and ninety third one a process asks for takes the
 * process down. That is kMaxTid in the sanitizer's own source and it is what
 * measuring it on gcc 13 and clang 18 gives. So src/runtime/context.c keeps a
 * pool and hands the same fiber to one context after another, which turns the
 * number needed into the most goroutines alive at once rather than the number
 * ever started. Creating one costs about fourteen microseconds and switching
 * about twenty five nanoseconds, so only the first is worth avoiding. */
#if BURROW_ASAN && !defined(BURROW_CONTEXT_FIBERS)
#define BURROW_CONTEXT_ASAN 1
#endif

#if BURROW_TSAN && !defined(BURROW_CONTEXT_FIBERS)
#define BURROW_CONTEXT_TSAN 1
#endif

#if defined(BURROW_CONTEXT_ASAN) || defined(BURROW_CONTEXT_TSAN)
#define BURROW_CONTEXT_ANNOTATE 1
#endif

#if defined(BURROW_CONTEXT_UCONTEXT)
/* Room for a ucontext_t without dragging <ucontext.h> into every file that
 * includes this one. That header needs _XOPEN_SOURCE defined before it, which
 * is something only a .c file can promise, and on macOS it also deprecates
 * everything it declares.
 *
 * The number is measured rather than guessed. glibc is 968 bytes on amd64 and
 * 4560 on arm64, where the signal context reserves space for the widest vector
 * registers the architecture might have. src/runtime/context.c asserts at
 * compile time that this is big enough, so a platform that needs more fails to
 * build and says so instead of writing past the end of it. */
#define BURROW_CONTEXT_STORAGE 8192
#endif

/* A saved execution state. One of these per goroutine, plus one per thread for
 * the stack the thread started on.
 *
 * The fields are here so a context can sit inside a G without an allocation.
 * They are not the interface and the assembly knows about exactly one of them,
 * which is that `sp` is the first word. */
typedef struct burrow__Context burrow__Context;

struct burrow__Context {
#if defined(BURROW_CONTEXT_ASM)
    /* The switched out stack pointer, and everything else is saved on the stack
     * it points at. Must stay at offset zero: the assembly hard codes that and
     * nothing else about this struct. */
    void *sp;
#elif defined(BURROW_CONTEXT_FIBERS)
    /* LPVOID from CreateFiber or ConvertThreadToFiber, spelled void * so that
     * <windows.h> stays out of this header. */
    void *fiber;
    /* Whether this context created the fiber, which decides whether tearing it
     * down deletes it or hands the thread back. */
    bool owns_fiber;
#else
    _Alignas(16) unsigned char storage[BURROW_CONTEXT_STORAGE];
#endif

    /* What to run and what to hand it. Read by the entry trampoline once the
     * new stack is live, which is why they live in the struct rather than in
     * registers the assembly would have to shuttle. */
    void (*entry)(void *);
    void *arg;

    /* Where to go when entry returns. */
    burrow__Context *link;

#if defined(BURROW_CONTEXT_ASAN)
    /* What the address sanitizer needs and nothing else does, which is why it
     * is not here in an ordinary build.
     *
     * `stack` and `stack_size` are the buffer make was given, because the
     * sanitizer has to be handed the bounds of the stack being switched to
     * before the switch rather than after. They are NULL and zero on a thread's
     * own context until the first switch away from it fills them in, which is
     * explained where that happens.
     *
     * `fake_stack` is the sanitizer's, written when this context is switched
     * away from and read when it is switched back to. */
    void *stack;
    size_t stack_size;
    void *fake_stack;
#endif

#if defined(BURROW_CONTEXT_TSAN)
    /* The thread sanitizer's fiber for this context, and how many contexts have
     * had it before this one.
     *
     * NULL means there is none, which is every context in a build with that
     * sanitizer off and is also a context that was never made. A count of zero
     * means the fiber is the calling thread's own and was borrowed rather than
     * created, so freeing the context must leave it alone. */
    void *tsan_fiber;
    uint32_t tsan_uses;
#endif
};

/* The smallest stack burrow__context_make will accept.
 *
 * It is larger than it looks like it needs to be because the fallback paths set
 * the floor. A ucontext stack has to hold whatever the platform's signal frame
 * needs, and on arm64 that alone is several kilobytes. Real goroutine stacks are
 * sized by the scheduler and are much bigger than this. */
#define BURROW_CONTEXT_STACK_MIN 16384

/* Makes the calling thread's own stack switchable, recording it in `self` so
 * that something can switch back to it.
 *
 * Every thread that is going to run goroutines calls this once, and false means
 * the system refused, which only happens where a context is a kernel object. On
 * the assembly and ucontext paths it does nothing and cannot fail, because the
 * first switch away from a thread is what fills its context in.
 *
 * It exists for Fibers, where a thread is not schedulable until it has been
 * converted into one, and skipping it there means the first switch quietly does
 * nothing. Call it everywhere anyway, because the platform where it matters is
 * not the platform anybody is testing on. */
bool burrow__context_attach(burrow__Context *self);

/* Undoes attach. The thread must be running on its own stack again, which is to
 * say the last switch has already come back. */
void burrow__context_detach(burrow__Context *self);

/* Arranges `stack` so that switching to `ctx` starts running `entry(arg)` on it.
 *
 * `stack` is the low address of a buffer of `size` bytes that the caller owns
 * and has to keep alive for as long as the context can still be switched to.
 * Nothing here allocates and nothing here frees.
 *
 * `link` is where control goes if `entry` returns, and it must outlive the
 * context too. Passing NULL says entry never returns, and if it does anyway the
 * program stops with a fatal error rather than returning into rubble.
 *
 * False means the size was below BURROW_CONTEXT_STACK_MIN or the system would
 * not give out what the context needs. It does not mean the stack is too small
 * for what entry will actually do, which is not a thing that can be known from
 * here.
 *
 * One difference worth knowing about, on Windows and only on Windows. A fiber
 * allocates its own stack and there is no call that takes one you already have,
 * so `stack` is ignored there and only `size` is used. Passing a real buffer
 * anyway is what keeps the same call working everywhere, and the buffer is
 * simply not the memory the context runs on. */
bool burrow__context_make(burrow__Context *ctx, void *stack, size_t size,
                          void (*entry)(void *), void *arg, burrow__Context *link);

/* Releases whatever the context was holding, which on most platforms is nothing
 * at all, because the caller owns the stack. On Windows it deletes the fiber,
 * and skipping it there leaks a stack per goroutine.
 *
 * The context must not be the one currently running and must not be switched to
 * again. Safe on a context that was never made. */
void burrow__context_free(burrow__Context *ctx);

/* The switch itself, with nothing around it. The assembly defines this one, and
 * so do the two portable backends, and none of them knows what a sanitizer is.
 * Call burrow__context_switch below instead. */
void burrow__context_switch_raw(burrow__Context *from, burrow__Context *to);

/* The two halves of telling a sanitizer that a switch is about to happen and
 * that one has just happened. Leave is called on the old stack and enter on the
 * new one, and between them is the only place a thread is on neither.
 *
 * `burrow__context_leave_final` is leave for a context that is finished and
 * will never be switched to again, which is a different thing to the address
 * sanitizer: it is what says the fake stack can go rather than be kept for a
 * return that is not coming. The thread sanitizer does not care, because a
 * context that is finished still has to hand its fiber over the same way.
 *
 * With no sanitizer on, all three are empty and the compiler deletes them. */
#if defined(BURROW_CONTEXT_ANNOTATE)
void burrow__context_leave(burrow__Context *from, burrow__Context *to);
void burrow__context_leave_final(burrow__Context *from, burrow__Context *to);
void burrow__context_enter(burrow__Context *self);
#else
static inline void burrow__context_leave(burrow__Context *from, burrow__Context *to) {
    (void)from;
    (void)to;
}
static inline void burrow__context_leave_final(burrow__Context *from,
                                               burrow__Context *to) {
    (void)from;
    (void)to;
}
static inline void burrow__context_enter(burrow__Context *self) {
    (void)self;
}
#endif

/* Saves where the caller is into `from` and resumes `to`.
 *
 * Returns when somebody switches back to `from`, and from the caller's point of
 * view it is an ordinary function call that took a long time. Both contexts have
 * to belong to the calling thread: a context is a stack, and two threads on one
 * stack at the same time is not something this or anything else can survive.
 *
 * Switching a context to itself is allowed and is a slow way of doing nothing.
 *
 * It is inline here rather than a function in context.c so that the ordinary
 * build is still one call straight into the assembly. The two annotations
 * around the switch are empty unless a sanitizer is on. */
static inline void burrow__context_switch(burrow__Context *from, burrow__Context *to) {
    burrow__context_leave(from, to);
    burrow__context_switch_raw(from, to);
    burrow__context_enter(from);
}

/* The other end of every trampoline. Runs entry, then goes to link.
 *
 * Not something to call. It is here because the assembly has to name it, and a
 * symbol the assembly reaches for that has no declaration in a header is a
 * symbol that gets renamed one day and fails at link time on one architecture. */
BURROW_NORETURN void burrow__context_start(burrow__Context *self);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CONTEXT_H */
