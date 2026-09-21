/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* panic: stop here, unwind, and land somewhere that can cope.
 *
 *     BURROW_TRY {
 *         parse(input);
 *     }
 *     BURROW_CATCH(p) {
 *         log_bad_input(panic_text(p));
 *         err = errors_new(a, BURROW_S("bad input"));
 *     }
 *     BURROW_TRY_END;
 *
 * panic is for the condition a caller cannot have wanted to hear about through
 * a return value, because the call was already wrong: an index past the end, a
 * nil map written to, a state machine in a state it has no case for. Everything
 * that can fail in the ordinary course of business still returns an Error, and
 * that is not a stylistic preference, it is the same line Go draws.
 *
 * On the way out, every deferred call of every scope between the panic and the
 * catch runs, innermost scope first and last in first out inside each scope.
 * That is the whole reason this is worth having over returning early: a panic
 * closes the files.
 *
 * ---------------------------------------------------------------- the block
 *
 * Three macros, the same shape as BURROW_SCOPE and for the same reason. The
 * bookkeeping has to be declared before your first statement, a macro cannot
 * reach inside braces that come after it, so BURROW_TRY opens a block of its
 * own and BURROW_TRY_END closes it.
 *
 * BURROW_CATCH names the value that was panicked with. It runs only when the
 * block above it panicked, and the value is an Any, which is Go's any, which is
 * what recover returns there.
 *
 * A BURROW_TRY is not a scope. If you want deferred calls inside it, open a
 * BURROW_SCOPE inside it, the same as anywhere else. The two features are kept
 * apart because most scopes never want a recovery point and a recovery point
 * costs more than a scope does.
 *
 * ------------------------------------------------- where this differs from Go
 *
 * Go recovers inside a deferred function, and the function that deferred it
 * returns normally afterwards:
 *
 *     func parse(b []byte) (err error) {
 *         defer func() {
 *             if r := recover(); r != nil { err = fmt.Errorf("%v", r) }
 *         }()
 *         ...
 *     }
 *
 * burrow recovers in a catch block instead, and there is no recover function to
 * call from a deferred call. The reason is C and not taste. Resuming in the
 * frame that recovered means that frame has to have saved somewhere to come
 * back to, which is a setjmp, and a setjmp is a few hundred bytes of jmp_buf in
 * the frame plus a call plus a compiler that stops keeping your locals in
 * registers across it. Go's rule needs one in every function with a defer in
 * it. Ours needs one in the functions that actually recover, which in a program
 * of any size is a handful of them, and for everybody else a scope stays two
 * stores and a call.
 *
 * What you lose is the shape above, where the recovery is written next to the
 * thing being guarded rather than around it. What you get is the catch block,
 * which is where a C programmer looks for it anyway, and a defer that stays
 * cheap enough to use everywhere.
 *
 * Everything else is Go's. Deferred calls run on the way out, innermost first.
 * A panic inside a deferred call chains onto the one already running rather
 * than replacing it, and the rest of that scope's calls still run. An
 * unrecovered panic prints the value and ends the process with status 2. A
 * panic does not cross a goroutine: a goroutine that panics unwinds its own
 * stack and finds its own catch blocks, and the one that started it is not
 * involved, the same as Go.
 *
 * ---------------------------------------------------------------- the value
 *
 * panic takes an Any, which is a type descriptor and a pointer. The pointer is
 * the part to be careful with, because the frame it points into is gone by the
 * time the catch block runs:
 *
 *     panic(BURROW_ANY_VAL(TYPE_INT, Int, 42));
 *
 * That one is fine, and so is every other value of thirty two bytes or less,
 * because panic copies what it points at into the catching frame before it
 * jumps. Str, Error, Any, every number and every small struct fit. Something
 * larger than that keeps pointing where it pointed, so it has to live somewhere
 * that outlives the jump: static storage, an allocation, or a frame outside the
 * BURROW_TRY.
 *
 * Copying the value is not copying what the value points at in turn, and that
 * is the part worth reading twice. A panicked Str arrives in the catch block
 * with its pointer and length intact, and the bytes are still wherever they
 * were, so a message formatted into a buffer in the panicking frame is a
 * message the catch block cannot read. Panic with a literal, or copy the bytes
 * somewhere that outlives the jump first. Same rule as everywhere else in
 * burrow, a Str borrows, but the jump makes it easier to forget.
 *
 * panic_str is there because panic("some text") is what most panics in Go are,
 * and going through Any for a string literal reads badly. It borrows the same
 * way, so the paragraph above is about it too.
 *
 * Panicking with a nil Any gets you a panic whose value says that nil was
 * panicked with, which is what Go does now and for the reason Go gives: a catch
 * block that receives nil cannot tell that it caught anything.
 *
 * ------------------------------------------------------------ setjmp's rule
 *
 * A local variable of the function containing BURROW_TRY, that is modified
 * inside the try block and read in the catch block or after it, has an
 * indeterminate value unless it is volatile. That is C's rule for setjmp and
 * not something burrow can paper over. In practice it bites the accumulator
 * pattern and nothing else:
 *
 *     volatile Int done = 0;
 *     BURROW_TRY {
 *         for (...) { step(); done++; }
 *     }
 *     BURROW_CATCH(p) {
 *         printf("stopped after %lld\n", (long long)done);
 *     }
 *     BURROW_TRY_END;
 *
 * gcc warns about the ones it notices under -Wextra, as -Wclobbered, and it
 * warns about plenty it should not, so read what it says rather than obeying
 * it.
 *
 * ---------------------------------------------------------------- what it costs
 *
 * A BURROW_TRY is a jmp_buf in the frame, a call that saves registers into it,
 * and four stores. A jmp_buf is two hundred bytes on glibc and a hundred and
 * something everywhere else, which is the whole reason this is a separate
 * construct rather than something every scope carries. The time is a few
 * nanoseconds and there is no system call in it, which took getting right: see
 * BURROW_SETJMP below for the libc that disagreed.
 *
 * Code with no BURROW_TRY in it pays nothing at all. panic is a call nobody
 * makes and the defer chain is walked by the panic rather than by the scopes,
 * so a scope does not get more expensive because panic exists.
 *
 * Panicking costs a walk of the open scopes, the deferred calls in them, and
 * one longjmp. Nothing here allocates, because running out of memory is one of
 * the things that gets here.
 */

#ifndef BURROW_PANIC_H
#define BURROW_PANIC_H

#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/iface.h"
#include "burrow/own.h"
#include "burrow/platform.h"

#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Go's panic. Ends this block and every block between here and the innermost
 * BURROW_TRY on this goroutine, running the deferred calls of each one. With no
 * BURROW_TRY on the stack it prints the value and ends the process. */
BURROW_NORETURN void panic(Any v);

/* panic with a string, which is what most panics are. The bytes are not copied,
 * so pass a literal or something that outlives the catch block. */
BURROW_NORETURN void panic_str(Str s);

/* What is unwinding on this goroutine right now, or nil when nothing is.
 *
 * This is for a deferred call that has to know why it is running, which is the
 * one thing Go's recover is used for that a catch block cannot express. Rolling
 * a transaction back rather than committing it is the example. It borrows: the
 * value belongs to the panic and is gone once the catch block has finished with
 * it. */
BURROW_BORROWS(ret) Any panic_value(void);

/* The text of a panic value, for printing one.
 *
 * Strings and errors give you their own text. Numbers and booleans are
 * formatted. Anything else gives you its type name, because there is no fmt in
 * the tree yet to ask for more. The result either points into the value or into
 * storage belonging to the panic, so copy it if you need it later. */
BURROW_BORROWS(ret, v) Str panic_text(Any v);

/* ------------------------------------------------------------------ internals
 *
 * Public because the macros below declare one of these in your frame, not
 * because there is anything to do with the fields. */

/* One active panic, living in the frame of the panic call that started it. They
 * chain when a deferred call panics while a panic is already unwinding, which
 * is what lets the printer show both. */
typedef struct burrow__Panic {
    Any value;
    struct burrow__Panic *outer;
} burrow__Panic;

/* Room for a panic value in the catching frame, so that panicking with a
 * compound literal works. Thirty two bytes covers every builtin type and most
 * small structs, and the union is how a C11 file asks for the alignment of the
 * widest thing without naming max_align_t, which is in a header this one does
 * not want. */
typedef union burrow__PanicValue {
    Byte bytes[32];
    long double f;
    void *p;
    uint64_t u;
} burrow__PanicValue;

/* One recovery point, living in the frame that wrote BURROW_TRY.
 *
 * `jb` is where a panic jumps to and `value` is what it carries. `scopes` and
 * `panics` are what the chains looked like when this block was entered, which
 * is how the unwinder knows which scopes belong inside it and which panics to
 * throw away when it catches one. `chain` doubles as the closed flag, the same
 * as it does for a defer scope. */
typedef struct burrow__Recover {
    jmp_buf jb;
    Any value;
    burrow__PanicValue storage;
    struct burrow__Recover *outer;
    struct burrow__Recover **chain;
    burrow__DeferScope *scopes;
    burrow__Panic *panics;
} burrow__Recover;

/* Everything a goroutine knows about panicking. A field of the goroutine, and a
 * thread local for a thread that is not one, the same arrangement the defer
 * chain has and for the same reason: a goroutine that parks inside a BURROW_TRY
 * can wake up on another thread. */
typedef struct burrow__PanicState {
    burrow__Panic *panics;
    burrow__Recover *recovers;

    /* Set while the unrecovered panic printer is running, so that a fatal
     * handler which panics cannot start the printer again forever. */
    bool printing;

    /* Where panic_text formats a number, since it hands back a Str and a Str
     * has to point at something. Per goroutine rather than per process so that
     * two goroutines printing panic values at once do not meet, and reused by
     * the next call, which is why the header says to copy what you keep. */
    Byte text[48];
} burrow__PanicState;

BURROW_BORROWS(ret) burrow__PanicState *burrow__panic_state(void);

void burrow__recover_open(BURROW_RETAINS(1) burrow__Recover *r);
void burrow__recover_close(burrow__Recover *r);

/* Which pair of jumping functions this platform wants, which is not a detail
 * and was fifty five times the price of the whole feature on one of them.
 *
 * C says nothing about whether setjmp saves the signal mask and every libc
 * answers differently. glibc does not, so setjmp there is a couple of dozen
 * stores. macOS and the BSDs do, so setjmp there is a sigprocmask, and a
 * sigprocmask is a system call. Measured on macOS, setjmp is 140 nanoseconds
 * against 2.5 for _setjmp, which is the POSIX pair that never touches the mask
 * and is what a panic wants, since a panic does not run in a signal handler and
 * has no mask to put back.
 *
 * So the pair is only swapped where the swap buys something, and that is not
 * timidity, it is that glibc declares _setjmp under a strict C11 compiler and
 * does not declare _longjmp, so asking for the pair there is a build failure
 * for no gain. On glibc the two are the same code behind different names.
 *
 * Windows has no mask to save, so its setjmp is already the cheap one, and
 * MinGW spells setjmp as a macro taking an argument that _setjmp does not.
 *
 * Anything not named here gets the portable pair, which is correct everywhere
 * and slow on the systems listed above. Adding one is a line. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_DRAGONFLY)
#define BURROW_SETJMP(buf) _setjmp(buf)
#define BURROW_LONGJMP(buf) _longjmp((buf), 1)
#else
#define BURROW_SETJMP(buf) setjmp(buf)
#define BURROW_LONGJMP(buf) longjmp((buf), 1)
#endif

/* The block.
 *
 * Both halves live inside the cleanup or the __finally, so a return out of the
 * try block or out of the catch block takes the recovery point off the chain on
 * its way. A recovery point left behind would be a frame a later panic jumps
 * into after it has returned, which is the kind of bug that reports as
 * something else entirely.
 *
 * The pragmas are about the fixed name, which BURROW_CATCH needs, and which
 * means a BURROW_TRY inside a BURROW_TRY shadows one. That is the correct
 * reading and not a mistake, so the warning is off for the one declaration. */
#if BURROW_CC_MSVC

#define BURROW_TRY                                                                     \
    {                                                                                  \
        __pragma(warning(push)) __pragma(warning(disable : 4456))                      \
            burrow__Recover burrow__rec;                                               \
        __pragma(warning(pop)) burrow__recover_open(&burrow__rec);                     \
        __try {                                                                        \
            if (BURROW_SETJMP(burrow__rec.jb) == 0) {

#define BURROW_CATCH(p)                                                                \
    }                                                                                  \
    else {                                                                             \
        Any p = burrow__rec.value;

#define BURROW_TRY_END                                                                 \
    }                                                                                  \
    }                                                                                  \
    __finally {                                                                        \
        burrow__recover_close(&burrow__rec);                                           \
    }                                                                                  \
    }                                                                                  \
    (void)0

#else

#define BURROW_TRY                                                                     \
    {                                                                                  \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"")  \
            __attribute__((                                                            \
                cleanup(burrow__recover_close))) burrow__Recover burrow__rec;          \
        _Pragma("GCC diagnostic pop") burrow__recover_open(&burrow__rec);              \
        if (BURROW_SETJMP(burrow__rec.jb) == 0) {

#define BURROW_CATCH(p)                                                                \
    }                                                                                  \
    else {                                                                             \
        Any p = burrow__rec.value;

#define BURROW_TRY_END                                                                 \
    }                                                                                  \
    }                                                                                  \
    (void)0

#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PANIC_H */
