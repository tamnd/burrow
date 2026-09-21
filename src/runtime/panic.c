/* The one file in burrow that is allowed to end the host's process, and the one
 * that is allowed to call setjmp.
 *
 * tools/check-banned.sh enforces both, and the reason the first is worth
 * enforcing is that a library which calls exit has taken a decision away from
 * the program that linked it. Everything in burrow that can fail returns an
 * Error. What is left over is the class of condition where there is nothing to
 * return to, because the caller's belief about its own state was wrong, and
 * that is what this file handles.
 *
 * There are two ways out of that class and they are not the same thing. A throw
 * is the runtime saying its own invariants are broken, and it ends the process,
 * which is Go's runtime.throw. A panic is a program saying a call was wrong,
 * and a program that wants to can catch it, which is Go's panic and recover.
 * The first half of this file is the throw and the second is the panic.
 *
 * The panic half is three moving parts. The chain of open defer scopes, which
 * burrow/defer.h builds and this walks. A chain of recovery points, one per
 * BURROW_TRY block, living in the frames that wrote them. And a chain of
 * panics, one per panic call that has not finished, so that a panic raised by a
 * deferred call while another is unwinding can be printed next to the one it
 * interrupted rather than instead of it. All three belong to the goroutine,
 * because a goroutine that parks inside a scope wakes up on whatever thread
 * takes it next.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/panic.h"

#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/type.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Read on the way out and written once during startup. It is a plain pointer
 * rather than an atomic because the atomics layer does not exist yet and
 * because the documented contract is to install it before there is a second
 * thread. When sync/atomic lands this becomes an atomic load and the contract
 * gets weaker, which is a change nobody has to notice. */
static RuntimeFatalFunc fatal_handler;

void runtime_set_fatal_handler(RuntimeFatalFunc fn) {
    fatal_handler = fn;
}

/* Go's format, down to the leading "fatal error: " and the newline, because
 * that prefix is what somebody pastes into a search box. */
static void write_message(Str msg) {
    fputs("fatal error: ", stderr);
    if (msg.p != NULL && msg.len > 0)
        fwrite(msg.p, 1, (size_t)msg.len, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

void runtime_throw(Str msg) {
    RuntimeFatalFunc fn = fatal_handler;

    /* A handler that returns is a handler that has misunderstood the deal, so
     * the default path runs afterwards and the process still ends. Letting it
     * return would mean resuming code that has already been told its
     * assumptions do not hold, which is how a caught bug becomes a corrupted
     * file. */
    if (fn != NULL)
        fn(msg);

    write_message(msg);

    /* Status 2 is what a Go program exits with when a panic goes unrecovered,
     * and scripts around burrow should not have to care which of the two
     * produced the failure.
     *
     * _Exit rather than exit, because exit runs atexit handlers and flushes
     * every stream, and we have just finished saying that the program's state
     * is not what the program thinks it is. stderr is flushed above, by hand,
     * because that one message is worth more than the rest of the buffers put
     * together. */
    _Exit(2);
}

/* Both of the specific ones format into a stack buffer. Nothing here allocates,
 * on purpose: running out of memory is one of the things that will eventually
 * get here, and a reporting path that needs an allocator is a reporting path
 * that stops working exactly when it is needed.
 *
 * 128 bytes holds the longest either of these can produce, since the text is
 * fixed and the three numbers are at most twenty digits each. snprintf
 * truncates rather than overflowing if that arithmetic is ever wrong. */
#define MSG_MAX 128

void runtime_index_out_of_range(Int i, Int len) {
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
                     "runtime error: index out of range [%lld] with length %lld",
                     (long long)i, (long long)len);
    if (n < 0)
        runtime_throw(BURROW_S("runtime error: index out of range"));
    runtime_throw(
        str_from_bytes(buf, (Int)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)));
}

/* These two carry no numbers, so they are a throw with a constant string and
 * nothing else. They exist as functions rather than as the string written out
 * at each call site because burrow/num.h has forty of those call sites and
 * because the text has to stay identical across all of them. */
void runtime_integer_divide_by_zero(void) {
    runtime_throw(BURROW_S("runtime error: integer divide by zero"));
}

void runtime_negative_shift(void) {
    runtime_throw(BURROW_S("runtime error: negative shift amount"));
}

void runtime_slice_bounds_out_of_range(Int lo, Int hi, Int cap) {
    char buf[MSG_MAX];
    int n = snprintf(
        buf, sizeof(buf),
        "runtime error: slice bounds out of range [%lld:%lld] with capacity %lld",
        (long long)lo, (long long)hi, (long long)cap);
    if (n < 0)
        runtime_throw(BURROW_S("runtime error: slice bounds out of range"));
    runtime_throw(
        str_from_bytes(buf, (Int)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)));
}

/* ------------------------------------------------------------------ panic */

/* The state for a thread that is not running a goroutine, which is the same
 * arrangement burrow/defer.h has and exists for the same reason: these macros
 * have to work before the runtime starts and after it stops. Thread local, so
 * the only thread that can reach it is the one inside the block. */
static BURROW_THREAD_LOCAL burrow__PanicState thread_panic;

burrow__PanicState *burrow__panic_state(void) {
    burrow__G *g = burrow__curg();

    return g != NULL ? &g->panic : &thread_panic;
}

void burrow__recover_open(burrow__Recover *r) {
    burrow__PanicState *st = burrow__panic_state();

    r->value = (Any){NULL, NULL};
    r->outer = st->recovers;
    r->chain = &st->recovers;
    r->panics = st->panics;

    /* Which scopes belong inside this block, recorded as the one scope that
     * does not: everything the defer chain gains from here on is inside, and a
     * panic closes exactly those. */
    r->scopes = *burrow__defer_chain();

    st->recovers = r;

    /* Storage for the value is deliberately not touched, the same as a scope's
     * array of calls. A block that never catches anything writes none of it. */
}

void burrow__recover_close(burrow__Recover *r) {
    if (r->chain == NULL)
        return;

    *r->chain = r->outer;
    r->chain = NULL;
}

Any panic_value(void) {
    burrow__PanicState *st = burrow__panic_state();

    return st->panics != NULL ? st->panics->value : (Any){NULL, NULL};
}

/* Formats a number into the per goroutine scratch. snprintf cannot overflow the
 * buffer and a negative result means it wrote nothing, which is not worth its
 * own message: the caller falls back to the type's name. */
BURROW_PRINTF(2, 3) static Str scratch(burrow__PanicState *st, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf((char *)st->text, sizeof(st->text), fmt, ap);
    va_end(ap);

    if (n < 0)
        return BURROW_S("");
    if (n >= (int)sizeof(st->text))
        n = (int)sizeof(st->text) - 1;
    return str_from_bytes((const char *)st->text, (Int)n);
}

/* True for the kinds this reads as a signed number, with the value in out.
 *
 * An if chain rather than a switch because the build turns on -Wswitch-enum,
 * which wants all twenty six kinds listed in any switch over one, and a switch
 * with twenty of them spelled as a fallthrough to a default says less than
 * this does. */
static bool as_signed(Any v, long long *out) {
    Kind k = v.t->kind;

    if (k == KIND_INT)
        *out = (long long)*(const Int *)v.data;
    else if (k == KIND_INT8)
        /* The cast is spelled out because int8_t is signed char on most
         * machines, and a lint that cannot tell a small number from a character
         * asks for it. This one is a number: the kind says so. */
        /* NOLINTNEXTLINE(bugprone-signed-char-misuse,cert-str34-c) */
        *out = (long long)*(const int8_t *)v.data;
    else if (k == KIND_INT16)
        *out = *(const int16_t *)v.data;
    else if (k == KIND_INT32)
        *out = *(const int32_t *)v.data;
    else if (k == KIND_INT64)
        *out = (long long)*(const int64_t *)v.data;
    else
        return false;
    return true;
}

static bool as_unsigned(Any v, unsigned long long *out) {
    Kind k = v.t->kind;

    if (k == KIND_UINT)
        *out = (unsigned long long)*(const Uint *)v.data;
    else if (k == KIND_UINT8)
        *out = *(const uint8_t *)v.data;
    else if (k == KIND_UINT16)
        *out = *(const uint16_t *)v.data;
    else if (k == KIND_UINT32)
        *out = *(const uint32_t *)v.data;
    else if (k == KIND_UINT64)
        *out = (unsigned long long)*(const uint64_t *)v.data;
    else if (k == KIND_UINTPTR)
        *out = (unsigned long long)*(const Uintptr *)v.data;
    else
        return false;
    return true;
}

Str panic_text(Any v) {
    burrow__PanicState *st = burrow__panic_state();
    long long i;
    unsigned long long u;

    if (v.t == NULL)
        return BURROW_S("nil");
    if (v.data == NULL)
        return v.t->name.len > 0 ? v.t->name : BURROW_S("nil");

    /* The two that carry their own text. An error is much the most common thing
     * to panic with after a string, since it is what a caller gets handed and
     * decides it cannot deal with. */
    if (v.t == TYPE_STRING)
        return *(const Str *)v.data;
    if (v.t == TYPE_ERROR)
        return error_message(*(const Error *)v.data);

    if (v.t->kind == KIND_BOOL)
        return *(const bool *)v.data ? BURROW_S("true") : BURROW_S("false");

    if (as_signed(v, &i))
        return scratch(st, "%lld", i);
    if (as_unsigned(v, &u))
        return scratch(st, "%llu", u);

    if (v.t->kind == KIND_FLOAT64)
        return scratch(st, "%g", *(const double *)v.data);
    if (v.t->kind == KIND_FLOAT32)
        return scratch(st, "%g", (double)*(const float *)v.data);

    /* Everything else gets its type's name, which is as much as anything can
     * say about a value without a formatter. Once fmt lands this is where it
     * gets asked, and the name is what fmt falls back to anyway for a type with
     * no String method. */
    return v.t->name.len > 0 ? v.t->name : BURROW_S("value of unnamed type");
}

/* The buffer the printer builds its message in. Five hundred and twelve bytes
 * because a panic with a message longer than that is a panic whose first line
 * is the part worth reading, and because nothing here allocates: running out of
 * memory is one of the things that ends up in this function. */
#define PANIC_MSG_MAX 512

static Int append(char *buf, Int n, Str s) {
    for (Int i = 0; i < s.len && n < (Int)PANIC_MSG_MAX - 1; i++)
        buf[n++] = (char)s.p[i];
    return n;
}

/* Oldest first, which is the order Go prints them in and the opposite of the
 * order the list is in. Reversing it in place is the one way to do that with no
 * storage, and the records belong to a process that is about to stop. */
static burrow__Panic *oldest_first(burrow__Panic *p) {
    burrow__Panic *head = NULL;

    while (p != NULL) {
        burrow__Panic *next = p->outer;

        p->outer = head;
        head = p;
        p = next;
    }
    return head;
}

static BURROW_NORETURN void print_and_exit(burrow__PanicState *st) {
    RuntimeFatalFunc fn = fatal_handler;
    bool again = st->printing;
    char buf[PANIC_MSG_MAX];
    Int n = 0;

    for (burrow__Panic *p = oldest_first(st->panics); p != NULL; p = p->outer) {
        if (n > 0)
            n = append(buf, n, BURROW_S("\n\t"));
        n = append(buf, n, BURROW_S("panic: "));
        n = append(buf, n, panic_text(p->value));
    }
    buf[n] = '\0';

    /* The handler gets the same text the process is about to print, and it gets
     * it once. A handler that panics rather than returning is how the tests
     * survive this path, and the second time around it does not get another go,
     * because a handler that panics with nowhere to land arrives back here. */
    st->printing = true;
    if (fn != NULL && !again)
        fn(str_from_bytes(buf, n));

    fputs(buf, stderr);
    fputc('\n', stderr);

    /* Go prints the goroutine and then its stack. The stack is the next thing
     * to land in the runtime and this is where it goes. */
    burrow__G *g = burrow__curg();
    if (g != NULL)
        fprintf(stderr, "\ngoroutine %llu [running]\n", (unsigned long long)g->id);

    fflush(stderr);

    /* Status 2, the same as a throw and the same as Go. _Exit for the reason
     * runtime_throw gives: the program's state is not what the program thinks
     * it is, so the atexit handlers are not worth running. */
    _Exit(2);
}

/* Moves the value into the catching frame, when it fits.
 *
 * Panicking with a compound literal is the common case and the literal dies
 * with the frame the panic left, so without this the catch block would read
 * storage the compiler has already reused. Thirty two bytes takes every builtin
 * and most small structs. Anything bigger keeps pointing where it pointed and
 * the header says what that means. */
static Any stash(burrow__Recover *r, Any v) {
    if (v.t == NULL || v.data == NULL)
        return v;
    if (v.t->size > sizeof(r->storage.bytes))
        return v;

    type_copy(v.t, r->storage.bytes, v.data);
    v.data = r->storage.bytes;
    return v;
}

void panic(Any v) {
    burrow__PanicState *st = burrow__panic_state();
    burrow__DeferScope **chain = burrow__defer_chain();
    burrow__Recover *r = st->recovers;
    burrow__DeferScope *stop = r != NULL ? r->scopes : NULL;
    burrow__Panic p;
    Str nil_text;

    /* Go turned panic(nil) into a real value in 1.21, because a recover that
     * hands back nil cannot be told apart from a recover that caught nothing,
     * and a program with that bug in it looks like a program without one. The
     * same argument applies to a catch block, so the same substitution happens
     * here. The Str lives in this frame, which outlives the walk below, and
     * stash copies it into the catching frame before the jump. */
    if (v.t == NULL) {
        nil_text = BURROW_S("panic called with nil argument");
        v = BURROW_ANY(TYPE_STRING, &nil_text);
    }

    /* On the chain before anything runs, so that a deferred call can ask what
     * is unwinding and so that a panic raised by one of them chains onto this
     * rather than replacing it. The record lives in this frame, which is still
     * here for as long as the unwinding is. */
    p.value = v;
    p.outer = st->panics;
    st->panics = &p;

    /* Every scope inside the innermost recovery point, innermost first. The
     * head is re-read every turn because a deferred call is allowed to open and
     * close scopes of its own, and closing a scope is what runs its calls.
     *
     * A deferred call that panics does not come back here. It starts its own
     * pass from wherever it is, which leaves the rest of this scope's calls to
     * the close that is already running, and that is Go's rule: a panic in a
     * deferred function does not cancel the deferred functions beside it. */
    while (*chain != NULL && *chain != stop)
        burrow__scope_close(*chain);

    if (r == NULL)
        print_and_exit(st);

    r->value = stash(r, v);

    /* Off both chains before the jump. The panics started inside this block are
     * over, and the block itself cannot catch a second time, so a panic thrown
     * by the catch block goes outward rather than back into the setjmp that is
     * running it. The close at the end of the block writes the same value to the
     * same place again, which is what makes that safe. */
    st->panics = r->panics;
    st->recovers = r->outer;
    st->printing = false;

    BURROW_LONGJMP(r->jb);
}

void panic_str(Str s) {
    /* The Str is copied into the catching frame by stash, so this compound
     * literal only has to outlive the walk, and it does. */
    panic(BURROW_ANY(TYPE_STRING, &s));
}
