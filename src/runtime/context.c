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

/* Two sanitizers have to be told that a stack switch happened, for two
 * unrelated reasons, and this section is the only place in the tree that talks
 * to either of them. The backends below call four helpers and know nothing else
 * about it. With both sanitizers off the helpers have empty bodies and the
 * compiler deletes them, so an ordinary build is unchanged.
 *
 * The address sanitizer keeps its own idea of where the thread's stack is and
 * what is live on it. Switching stacks behind its back leaves that idea
 * pointing at memory the thread is no longer using, and everything it says
 * afterwards is about the wrong stack.
 *
 * The thread sanitizer keeps a call stack per thread in a buffer it never
 * grows, pushed and popped by code it puts in every function. A goroutine that
 * parks on one thread and carries on from another pushes on the first and pops
 * on the second, and enough of that walks off the end of the buffer and kills
 * the process from inside the sanitizer. Its answer to this is a fiber, which
 * is a call stack the switch hands over along with the stack itself.
 *
 * burrow/context.h has the longer version of both. */

/* ------------------------------------------------ the address sanitizer */

#if defined(BURROW_CONTEXT_ASAN)

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

static void asan_make(burrow__Context *ctx, void *stack, size_t size) {
    ctx->stack = stack;
    ctx->stack_size = size;
    ctx->fake_stack = NULL;
}

static void asan_attach(burrow__Context *self) {
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
static void asan_leave(burrow__Context *from, burrow__Context *to, bool final) {
    annotate_prev = from;
    __sanitizer_start_switch_fiber(final ? NULL : &from->fake_stack, to->stack,
                                   to->stack_size);
}

static void asan_enter(burrow__Context *self) {
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

static void asan_make(burrow__Context *ctx, void *stack, size_t size) {
    (void)ctx;
    (void)stack;
    (void)size;
}

static void asan_attach(burrow__Context *self) {
    (void)self;
}

/* The other two only when something else is annotating. With every sanitizer
 * off the three functions that would call them are inline nothing in the header
 * and this file does not define them at all, so a no-op here would be a static
 * function nobody calls, which this tree treats as an error. */
#if defined(BURROW_CONTEXT_ANNOTATE)

static void asan_leave(burrow__Context *from, burrow__Context *to, bool final) {
    (void)from;
    (void)to;
    (void) final;
}

static void asan_enter(burrow__Context *self) {
    (void)self;
}

#endif

#endif

/* ------------------------------------------------- the thread sanitizer */

#if defined(BURROW_CONTEXT_TSAN)

#include "burrow/lock.h"

#include <sanitizer/tsan_interface.h>

/* How many contexts one fiber is handed to before it is thrown away and a fresh
 * one takes its place.
 *
 * A fiber comes back to the pool with whatever was on its call stack when the
 * goroutine stopped for the last time, and nothing ever pops that, because the
 * frames belonged to a goroutine that is not going to return from them. It is a
 * handful of entries each time, five or six for the usual exit path, and the
 * buffer they go in holds sixty four thousand. Retiring a fiber after five
 * hundred and twelve goroutines keeps the worst case an order of magnitude
 * under that and costs one identifier per five hundred and twelve goroutines,
 * which against the budget below is about four million goroutines per process.
 * Nothing in the tests comes close and neither does anything else that would
 * still be running under this sanitizer. */
#define TSAN_FIBER_USES 512

/* How many fibers the pool holds. Past this they are destroyed on the way back
 * instead of kept, which costs identifiers, so it is set well above any number
 * of goroutines that can be alive at once in a sanitizer build. */
#define TSAN_FIBER_POOL 4096

/* How many fibers this process may create at all, ever.
 *
 * The sanitizer's limit is kMaxTid, which is eight thousand one hundred and
 * ninety two, and asking for one past it is not an error you can catch: the
 * process dies inside the sanitizer with a SEGV and a stack trace that points
 * at nothing useful. So we stop short of it and say what happened instead. */
#define TSAN_FIBER_BUDGET 8000

/* A fiber that is not in use, and how many contexts have already had it. The
 * count travels with the fiber rather than resetting, because what it is
 * counting is how much the frames left behind by all of them add up to. */
typedef struct {
    void *fiber;
    uint32_t uses;
} Spare;

static burrow__Lock tsan_pool_lock;
static Spare tsan_pool[TSAN_FIBER_POOL];
static int32_t tsan_pool_len;
static int32_t tsan_fibers_made;

static void tsan_make(burrow__Context *ctx) {
    burrow__lock(&tsan_pool_lock);
    if (tsan_pool_len > 0) {
        tsan_pool_len--;
        ctx->tsan_fiber = tsan_pool[tsan_pool_len].fiber;
        ctx->tsan_uses = tsan_pool[tsan_pool_len].uses + 1;
        burrow__unlock(&tsan_pool_lock);
        return;
    }
    if (tsan_fibers_made >= TSAN_FIBER_BUDGET) {
        burrow__unlock(&tsan_pool_lock);
        runtime_throw(BURROW_S("this build has run the thread sanitizer out of fibers, "
                               "which takes more goroutines alive at once than it can "
                               "follow. See include/burrow/context.h."));
    }
    tsan_fibers_made++;
    burrow__unlock(&tsan_pool_lock);

    /* Outside the lock because it is the slow half, about fourteen microseconds,
     * and the count above has already reserved the slot it is going to use. */
    ctx->tsan_fiber = __tsan_create_fiber(0);
    ctx->tsan_uses = 1;

    if (ctx->tsan_fiber == NULL)
        runtime_throw(BURROW_S("the thread sanitizer would not give out a fiber"));
}

static void tsan_free(burrow__Context *ctx) {
    void *fiber = ctx->tsan_fiber;
    uint32_t uses = ctx->tsan_uses;

    /* Nothing to give back on a context that was never made, and nothing we are
     * allowed to give back on one that borrowed the calling thread's own fiber,
     * which is what a use count of zero means. */
    if (fiber == NULL || uses == 0)
        return;

    ctx->tsan_fiber = NULL;
    ctx->tsan_uses = 0;

    if (uses < TSAN_FIBER_USES) {
        burrow__lock(&tsan_pool_lock);
        if (tsan_pool_len < TSAN_FIBER_POOL) {
            tsan_pool[tsan_pool_len].fiber = fiber;
            tsan_pool[tsan_pool_len].uses = uses;
            tsan_pool_len++;
            burrow__unlock(&tsan_pool_lock);
            return;
        }
        burrow__unlock(&tsan_pool_lock);
    }

    /* Destroying the fiber a thread is currently on is not allowed, and this is
     * never that: a context is only freed once nothing can switch to it, which
     * is what the header asks the caller for. */
    __tsan_destroy_fiber(fiber);
}

static void tsan_attach(burrow__Context *self) {
    /* The thread already has one and it is the one its own stack belongs to.
     * Zero uses says it is borrowed, so freeing this context leaves it alone. */
    self->tsan_fiber = __tsan_get_current_fiber();
    self->tsan_uses = 0;
}

/* Called on the old stack, immediately before the switch, because from here on
 * the frames being pushed belong to the context being switched to.
 *
 * The flag argument is zero rather than __tsan_switch_to_fiber_no_sync, which
 * says the fiber being switched to inherits everything the one being switched
 * away from knows. That is not a shortcut, it is the truth: one thread ran the
 * first and is about to run the second, in that order, so everything the first
 * did really did happen before everything the second is going to do. Saying
 * otherwise reports the runtime's own handover as a race, because a context is
 * filled in by the goroutine that started it and read by the goroutine that
 * runs it, and the only thing in between is a run queue that the scheduler
 * touches on its own stack and not on either of theirs. */
static BURROW_NO_TSAN void tsan_switch(burrow__Context *to) {
    if (to->tsan_fiber != NULL)
        __tsan_switch_to_fiber(to->tsan_fiber, 0);
}

#else

static void tsan_make(burrow__Context *ctx) {
    (void)ctx;
}

static void tsan_free(burrow__Context *ctx) {
    (void)ctx;
}

static void tsan_attach(burrow__Context *self) {
    (void)self;
}

/* Gated for the reason the address sanitizer's pair above is gated. */
#if defined(BURROW_CONTEXT_ANNOTATE)

static void tsan_switch(burrow__Context *to) {
    (void)to;
}

#endif

#endif

/* ------------------------------------------------- what the backends call */

static void annotate_make(burrow__Context *ctx, void *stack, size_t size) {
    asan_make(ctx, stack, size);
    tsan_make(ctx);
}

static void annotate_attach(burrow__Context *self) {
    asan_attach(self);
    tsan_attach(self);
}

static void annotate_free(burrow__Context *ctx) {
    tsan_free(ctx);
}

#if defined(BURROW_CONTEXT_ANNOTATE)

/* The fiber goes first, because once the address sanitizer has been told a
 * switch has started it wants the switch and nothing much else.
 *
 * Both halves of leave are outside the thread sanitizer, and this is the whole
 * reason BURROW_NO_TSAN exists. That sanitizer puts a push at the top of every
 * function it compiles and a pop at the bottom, counted against whichever stack
 * it believes the thread is on. A function that changes that belief halfway
 * through pushes on the fiber it was called on and pops on the one it switched
 * to, and the two counts drift apart by one on each side of every switch, which
 * is the exact thing the fibers are here to stop. Left instrumented this makes
 * the crash worse rather than better, which is how it was found.
 *
 * Nothing is being hidden by that. What these read is two pointers out of a
 * context, and the switch itself is assembly the sanitizer never sees either
 * way. Enter is ordinary instrumented code because it does not move: it is
 * called on the far side of the switch and pushes and pops on one fiber. */

BURROW_NO_TSAN void burrow__context_leave(burrow__Context *from, burrow__Context *to) {
    tsan_switch(to);
    asan_leave(from, to, false);
}

BURROW_NO_TSAN void burrow__context_leave_final(burrow__Context *from,
                                                burrow__Context *to) {
    tsan_switch(to);
    asan_leave(from, to, true);
}

void burrow__context_enter(burrow__Context *self) {
    asan_enter(self);
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
    /* The caller owns the stack and always did, so the only thing here is
     * whatever a sanitizer was holding, which in an ordinary build is nothing. */
    if (ctx == NULL)
        return;

    annotate_free(ctx);
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
    if (ctx == NULL)
        return;

    annotate_free(ctx);

    if (ctx->fiber == NULL || !ctx->owns_fiber)
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
    /* Nothing of the context's own, since a ucontext holds no kernel object and
     * the stack belongs to the caller. A sanitizer may still have something. */
    if (ctx == NULL)
        return;

    annotate_free(ctx);
}

void burrow__context_switch_raw(burrow__Context *from, burrow__Context *to) {
    resuming = to;
    (void)swapcontext(as_ucontext(from), as_ucontext(to));
}

#endif
