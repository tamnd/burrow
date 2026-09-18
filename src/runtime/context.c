/* The two backends that are not assembly, and the trampoline all three share.
 *
 * See burrow/context.h for what a context is. What is here is the portable
 * fallback in its two flavours, Fibers on Windows and ucontext everywhere else,
 * plus burrow__context_start, which is where every backend's entry trampoline
 * ends up once the new stack is live.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Before every include, because a feature macro only counts if nothing has
 * been included yet. ucontext was taken out of POSIX in 2008, so both glibc and
 * musl keep it behind _XOPEN_SOURCE, and macOS refuses to declare it at all
 * without this. _WIN32 rather than BURROW_OS_WINDOWS for the reason rand.c
 * gives: platform.h is what defines that, and platform.h is an include. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#endif

#include "burrow/context.h"

#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(BURROW_CONTEXT_FIBERS)
#include <windows.h>
#elif defined(BURROW_CONTEXT_UCONTEXT)
#include <ucontext.h>
#endif

/* macOS has shipped ucontext since 10.6 with a deprecation warning on all four
 * functions, and there is no replacement it is pointing at. The warning is real
 * information the first time you read it and noise every time after, and this
 * is the only file in the tree that is allowed to hear it. macOS on arm64 and
 * amd64 takes the assembly path anyway, so the only way to get here is to ask
 * for the fallback on purpose. */
#if defined(BURROW_CONTEXT_UCONTEXT) && defined(BURROW_OS_DARWIN) &&                   \
    defined(BURROW_CC_CLANG)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

/* musl does not implement ucontext. It is not hidden behind a feature macro the
 * way getentropy is, the functions are simply not there, and a build that gets
 * this far fails at the link line with four undefined references and no hint
 * about why. Saying it here turns that into something you can act on. */
#if defined(BURROW_CONTEXT_UCONTEXT) && defined(BURROW_OS_LINUX) && !defined(__GLIBC__)
#error "this libc has no ucontext, so this machine needs assembly"
#endif

#if defined(BURROW_CONTEXT_UCONTEXT)
/* If either of these fires, raise BURROW_CONTEXT_STORAGE in burrow/context.h to
 * the number the compiler is complaining about and put the platform in the
 * comment next to it. Catching this here is the entire reason the storage is a
 * fixed size array rather than a guess nobody checks. */
_Static_assert(sizeof(ucontext_t) <= BURROW_CONTEXT_STORAGE,
               "BURROW_CONTEXT_STORAGE is too small for this platform's ucontext_t");
_Static_assert(_Alignof(ucontext_t) <= 16,
               "this platform's ucontext_t wants more alignment than the storage has");
#endif

/* -------------------------------------------------------- the sanitizers */

/* The address sanitizer keeps its own idea of where the thread's stack is and
 * what is live on it. Switching stacks behind its back leaves that idea
 * pointing at memory the thread is no longer using, and everything it says
 * afterwards is about the wrong stack. It has a pair of calls for this, and
 * this section is the only place we make them, so the backends below can just
 * call these without knowing anything about it.
 *
 * See burrow/context.h for why the thread sanitizer does not get the same
 * treatment. None of this is compiled unless the address sanitizer is on: the
 * helpers still exist so that every backend calls them unconditionally, and
 * with it off they have empty bodies and cost nothing. */

#if defined(BURROW_CONTEXT_ANNOTATE)

#include <sanitizer/asan_interface.h>

/* Which context this thread was on immediately before the switch it is in the
 * middle of. Written on the old stack and read on the new one, on the same
 * thread, which is the same trick the ucontext trampoline uses further down and
 * is safe for the same reason.
 *
 * It is here for one job. A thread's own context does not know where its stack
 * is, because nobody handed the thread its stack and asking the operating
 * system is a different question on every platform. The sanitizer already
 * knows, and it tells us: the finish call on the far side of a switch answers
 * with the bounds of the stack that was just left. So the first switch away
 * from a thread is what fills that thread's context in, and this is how the far
 * side knows whose context to fill. */
static BURROW_THREAD_LOCAL burrow__Context *annotate_prev;

static void annotate_make(burrow__Context *ctx, void *stack, size_t size) {
    ctx->stack = stack;
    ctx->stack_size = size;
    ctx->fake_stack = NULL;
}

static void annotate_attach(burrow__Context *self) {
    /* No bounds, because nobody handed this thread its stack. The comment on
     * annotate_prev says where they come from instead. */
    self->stack = NULL;
    self->stack_size = 0;
    self->fake_stack = NULL;
}

/* `final` says this context is finished and nothing will switch to it again,
 * which the sanitizer wants to know: passing NULL where the fake stack would be
 * saved is what tells it to throw that fake stack away instead of keeping it
 * for a return that is not coming. */
static void annotate_leave(burrow__Context *from, burrow__Context *to, bool final) {
    annotate_prev = from;
    __sanitizer_start_switch_fiber(final ? NULL : &from->fake_stack, to->stack,
                                   to->stack_size);
}

void burrow__context_leave(burrow__Context *from, burrow__Context *to) {
    annotate_leave(from, to, false);
}

void burrow__context_leave_final(burrow__Context *from, burrow__Context *to) {
    annotate_leave(from, to, true);
}

void burrow__context_enter(burrow__Context *self) {
    const void *bottom = NULL;
    size_t size = 0;

    __sanitizer_finish_switch_fiber(self->fake_stack, &bottom, &size);
    self->fake_stack = NULL;

    /* bottom and size describe the stack the thread has just left, so this is
     * where a thread's own context learns where its stack is. Once is enough,
     * and after that the answer is the one already stored. */
    if (annotate_prev != NULL && annotate_prev->stack == NULL) {
        /* The cast drops a const the sanitizer put on for the caller's benefit.
         * It is the same address either way and nothing here writes through
         * it. */
        annotate_prev->stack = (void *)(uintptr_t)bottom;
        annotate_prev->stack_size = size;
    }
}

#else

static void annotate_make(burrow__Context *ctx, void *stack, size_t size) {
    (void)ctx;
    (void)stack;
    (void)size;
}

static void annotate_attach(burrow__Context *self) {
    (void)self;
}

#endif

/* ------------------------------------------------------------- the trampoline */

BURROW_NORETURN void burrow__context_start(burrow__Context *self) {
    /* The other half of the switch that got here. On a context that has never
     * run there is nothing saved to hand back, which is what the NULL in its
     * fake_stack means. */
    burrow__context_enter(self);

    self->entry(self->arg);

    /* entry returned, which is allowed as long as make was told where to go
     * next. There is nowhere to return to: the frame underneath this one was
     * built by hand and there is no caller. */
    if (self->link == NULL)
        runtime_throw(BURROW_S("context entry returned and the context has no link"));

    /* The final form of the switch, because this context is done and the stack
     * it is standing on stops meaning anything the moment the switch lands. */
    burrow__context_leave_final(self, self->link);
    burrow__context_switch_raw(self, self->link);

    /* Something switched back into a context that has already finished. Its
     * stack is whatever the entry function left behind, so carrying on would be
     * running through a frame that is not there any more. */
    runtime_throw(BURROW_S("switched into a context that already returned"));
}

#if defined(BURROW_CONTEXT_ASM)

/* ------------------------------------------------------------------ assembly */

/* Both of these are in src/runtime/context_<arch>.S. burrow__context_switch_raw
 * is declared in the header and is entirely assembly. This one is the other half
 * of make: it lays out the frame that switch will pop the first time, which
 * means the frame layout is described once, in the file that also pops it. */
void *burrow__context_make_asm(void *stack_top, burrow__Context *self);

bool burrow__context_attach(burrow__Context *self) {
    /* Nothing for the machine to do. The first switch away from this thread is
     * what fills the context in, and there is no kernel object to ask for. */
    if (self == NULL)
        return false;

    annotate_attach(self);
    return true;
}

void burrow__context_detach(burrow__Context *self) {
    (void)self;
}

bool burrow__context_make(burrow__Context *ctx, void *stack, size_t size,
                          void (*entry)(void *), void *arg, burrow__Context *link) {
    if (ctx == NULL || stack == NULL || entry == NULL ||
        size < BURROW_CONTEXT_STACK_MIN)
        return false;

    ctx->entry = entry;
    ctx->arg = arg;
    ctx->link = link;
    annotate_make(ctx, stack, size);

    /* Stacks grow down on both architectures with assembly here, so the context
     * starts at the top of the buffer. The alignment the ABI wants is applied on
     * the other side, because it is the same file that knows the frame size. */
    ctx->sp = burrow__context_make_asm((unsigned char *)stack + size, ctx);
    return true;
}

void burrow__context_free(burrow__Context *ctx) {
    /* The caller owns the stack and always did. */
    (void)ctx;
}

#elif defined(BURROW_CONTEXT_FIBERS)

/* -------------------------------------------------------------------- fibers */

/* Windows fibers are the operating system's own version of this, which is
 * convenient and costs one thing: a fiber allocates its own stack and there is
 * no call that takes yours. So the buffer the caller passes is not used here,
 * only its size, and that difference is written in the header rather than left
 * for somebody to find with a debugger. */

static void WINAPI fiber_proc(void *param) {
    burrow__context_start((burrow__Context *)param);
}

bool burrow__context_attach(burrow__Context *self) {
    if (self == NULL)
        return false;

    self->entry = NULL;
    self->arg = NULL;
    self->link = NULL;
    self->owns_fiber = false;
    annotate_attach(self);

    /* A thread is not switchable until it is a fiber. Doing this twice on one
     * thread fails with ERROR_ALREADY_FIBERS, which is a caller bug rather than
     * something to paper over, so it is reported like any other failure. */
    self->fiber = ConvertThreadToFiber(NULL);
    return self->fiber != NULL;
}

void burrow__context_detach(burrow__Context *self) {
    if (self == NULL || self->fiber == NULL || self->owns_fiber)
        return;

    (void)ConvertFiberToThread();
    self->fiber = NULL;
}

bool burrow__context_make(burrow__Context *ctx, void *stack, size_t size,
                          void (*entry)(void *), void *arg, burrow__Context *link) {
    /* The stack is checked even though it is not used, so that the contract is
     * the same sentence on every platform. A caller that passes NULL here has a
     * bug on three platforms out of four, and finding it on the fourth as well
     * is worth more than the call it allows. */
    if (ctx == NULL || stack == NULL || entry == NULL ||
        size < BURROW_CONTEXT_STACK_MIN)
        return false;

    /* See the note above: the fiber brings its own stack and the caller's is
     * left alone. */
    (void)stack;

    ctx->entry = entry;
    ctx->arg = arg;
    ctx->link = link;
    ctx->owns_fiber = true;
    annotate_make(ctx, stack, size);

    /* The size is a commit size on Windows and the reserve comes from the
     * image header, which is the opposite way round from every other platform
     * here. It is close enough to mean the same thing to a caller. */
    ctx->fiber = CreateFiber((SIZE_T)size, fiber_proc, ctx);
    return ctx->fiber != NULL;
}

void burrow__context_free(burrow__Context *ctx) {
    if (ctx == NULL || ctx->fiber == NULL || !ctx->owns_fiber)
        return;

    /* Deleting the fiber a thread is currently running would end the thread, so
     * this is only ever called on a context that has been switched away from,
     * which is what the header asks for. */
    DeleteFiber(ctx->fiber);
    ctx->fiber = NULL;
}

void burrow__context_switch_raw(burrow__Context *from, burrow__Context *to) {
    /* A fiber saves itself, so there is nothing to write into `from`. It stays
     * in the signature because every other backend needs it and a caller should
     * not have to know which one it is talking to. */
    (void)from;
    SwitchToFiber(to->fiber);
}

#else

/* ------------------------------------------------------------------ ucontext */

/* The storage is a byte array so that the header does not have to include
 * <ucontext.h>, and it is aligned to 16 with a static assert above saying that
 * is enough, so this cast is the one the array was shaped for. Going through
 * void * is what stops -Wcast-align from objecting to a cast it cannot see the
 * alignment of. */
static ucontext_t *as_ucontext(burrow__Context *c) {
    return (ucontext_t *)(void *)c->storage;
}

/* makecontext hands its function no arguments worth having. The usual way round
 * that is to chop a pointer into two ints and put it back together, which is
 * undefined behaviour dressed up as a calling convention. This is a thread local
 * instead: burrow__context_switch_raw writes it on the way out, and the trampoline
 * is the first thing that runs on the other side of that switch, on the same
 * thread, so it reads back what the switch wrote. */
static BURROW_THREAD_LOCAL burrow__Context *resuming;

static void ucontext_trampoline(void) {
    burrow__context_start(resuming);
}

bool burrow__context_attach(burrow__Context *self) {
    if (self == NULL)
        return false;

    self->entry = NULL;
    self->arg = NULL;
    self->link = NULL;
    annotate_attach(self);

    /* Not strictly needed, since the first swapcontext fills this in anyway.
     * It is here so that a context which is switched away from before it is
     * ever switched to holds a real ucontext_t rather than whatever was in the
     * caller's memory. */
    return getcontext(as_ucontext(self)) == 0;
}

void burrow__context_detach(burrow__Context *self) {
    (void)self;
}

bool burrow__context_make(burrow__Context *ctx, void *stack, size_t size,
                          void (*entry)(void *), void *arg, burrow__Context *link) {
    ucontext_t *uc;

    if (ctx == NULL || stack == NULL || entry == NULL ||
        size < BURROW_CONTEXT_STACK_MIN)
        return false;

    ctx->entry = entry;
    ctx->arg = arg;
    ctx->link = link;
    annotate_make(ctx, stack, size);

    uc = as_ucontext(ctx);
    if (getcontext(uc) != 0)
        return false;

    uc->uc_stack.ss_sp = stack;
    uc->uc_stack.ss_size = size;

    /* NULL rather than the link, because the trampoline never returns. It calls
     * burrow__context_start, which switches to the link itself, so that all
     * three backends do the same thing in the same place. uc_link would do it
     * here and nowhere else, which is one more difference to remember. */
    uc->uc_link = NULL;

    makecontext(uc, ucontext_trampoline, 0);
    return true;
}

void burrow__context_free(burrow__Context *ctx) {
    (void)ctx;
}

void burrow__context_switch_raw(burrow__Context *from, burrow__Context *to) {
    resuming = to;
    (void)swapcontext(as_ucontext(from), as_ucontext(to));
}

#endif
