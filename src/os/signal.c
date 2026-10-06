/* os/signal: the registry of who wants which signal, and the thread that hands
 * signals out.
 *
 * The runtime half is src/runtime/sigqueue.c, which queues a signal from the
 * handler and gives it to whoever is waiting in burrow__signal_recv. This half
 * is the waiting, and the table it consults. Both are Go's, line for line where
 * a line would do, with two changes that C needs.
 *
 * The first is the reader. Go starts a goroutine for it the first time Notify
 * enables a signal. A goroutine here needs runtime_main, and a program that
 * wants Ctrl-C to close a file cleanly should not need that, so this starts a
 * thread instead, at the same moment, and the thread never ends.
 *
 * The second is NotifyContext. Go makes a channel for it and starts a goroutine
 * that waits on the channel and the context and cancels the one from the
 * other. Here the entry in the table has the context's cancel function rather
 * than a channel, and the reader calls it, which is the same thing with no
 * goroutine and no channel. Go's goroutine stops waiting once the context is
 * done and its channel goes on swallowing signals until stop, and calling a
 * cancel that has already happened does nothing, so that part is the same as
 * well.
 *
 * Derived from Go's src/os/signal/signal.go and signal_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/signal.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/sigqueue.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/thread.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Go's numSig, the most signals any system it runs on has, plus one. */
enum { NOTIFY_NSIG = 65, NOTIFY_WORDS = (NOTIFY_NSIG + 31) / 32 };

typedef struct NotifyCtx NotifyCtx;

/* Go's handler, with the map key beside it since this is a list and not a map.
 * key is the channel for signal_notify and the NotifyCtx for
 * signal_notify_context, and exactly one of c and ctx is set. */
typedef struct NotifyEntry {
    const void *key;
    Chan *c;
    NotifyCtx *ctx;
    uint32_t mask[NOTIFY_WORDS];
} NotifyEntry;

/* What signal_notify_context keeps beside the context it makes. */
struct NotifyCtx {
    Alloc *a;
    ContextCancelCauseFunc cancel;
};

/* Go's handlers struct. Everything is under mu. ref counts how many entries
 * want each signal, so that the first one enables it and the last one to go
 * disables it, and stopping is the entries signal_stop has taken out and is
 * waiting to see the last of, which still get what was already on its way. */
typedef struct NotifyTable {
    SyncMutex mu;
    NotifyEntry *m;
    Int n;
    Int cap;
    int64_t ref[NOTIFY_NSIG];
    NotifyEntry *stopping;
    Int nstopping;
    Int capstopping;
    /* Go's watchSignalLoopOnce. Under mu, since every caller holds it. */
    bool watching;
} NotifyTable;

static NotifyTable notify_table;

static bool notify_want(const NotifyEntry *h, int sig) {
    return ((h->mask[sig / 32] >> (unsigned)(sig & 31)) & 1U) != 0;
}

static void notify_set(NotifyEntry *h, int sig) {
    h->mask[sig / 32] |= 1U << (unsigned)(sig & 31);
}

static void notify_clear(NotifyEntry *h, int sig) {
    h->mask[sig / 32] &= ~(1U << (unsigned)(sig & 31));
}

static bool notify_empty(const NotifyEntry *h) {
    for (int i = 0; i < NOTIFY_WORDS; i++)
        if (h->mask[i] != 0)
            return false;
    return true;
}

/* Go's signum: the number of a signal this package can do anything with, or
 * -1. Only syscall signals count, as in Go, where the type switch has one
 * case. */
static int notify_signum(OsSignal sig) {
    SyscallSignal n = os_signal_to_syscall(sig);
    if (n < 0 || n >= NOTIFY_NSIG)
        return -1;
    return (int)n;
}

/* Room for one more in a list that grows. Go's map and append cannot fail
 * short of the runtime giving up, and this gives up the same way. */
static NotifyEntry *notify_grow(NotifyEntry *list, Int n, Int *cap) {
    if (n < *cap)
        return list;

    Int want = *cap == 0 ? 4 : *cap * 2;
    NotifyEntry *bigger = (NotifyEntry *)mem_alloc(
        heap_allocator(), sizeof(NotifyEntry) * (size_t)want, _Alignof(NotifyEntry));
    if (bigger == NULL)
        runtime_throw(BURROW_S("os/signal: out of memory"));
    if (n > 0)
        memcpy(bigger, list, sizeof(NotifyEntry) * (size_t)n);
    if (list != NULL)
        mem_free(heap_allocator(), list, sizeof(NotifyEntry) * (size_t)*cap,
                 _Alignof(NotifyEntry));
    *cap = want;
    return bigger;
}

static NotifyEntry *notify_find(const void *key) {
    for (Int i = 0; i < notify_table.n; i++)
        if (notify_table.m[i].key == key)
            return &notify_table.m[i];
    return NULL;
}

static void notify_delete(NotifyEntry *h) {
    Int i = (Int)(h - notify_table.m);
    notify_table.m[i] = notify_table.m[notify_table.n - 1];
    notify_table.n--;
}

/* ---------------------------------------------------------- the reader */

/* The cause NotifyContext cancels with, Go's signalError: the signal's name
 * and " signal received", and an Is that says yes to context_canceled so that
 * a caller asking whether the context was cancelled hears that it was. One per
 * signal, built the first time that signal cancels something and kept, since a
 * cause has to outlive the context it is the cause of. Written under the
 * table's lock, before the cancel that publishes it. */
typedef struct NotifyCause {
    Str text;
    Byte buf[64];
} NotifyCause;

static NotifyCause notify_causes[NOTIFY_NSIG];

static Str notify_cause_message(const void *self) {
    return ((const NotifyCause *)self)->text;
}

static bool notify_cause_is(const void *self, Error target) {
    (void)self;
    return target.vt == context_canceled.vt && target.data == context_canceled.data;
}

static const ErrorVT notify_cause_vt = {
    NULL, notify_cause_message, NULL, NULL, notify_cause_is, NULL, NULL,
};

static Error notify_cause(int n) {
    NotifyCause *c = &notify_causes[n];

    if (c->text.len == 0) {
        static const char tail[] = " signal received";
        Str name = syscall_signal_string((SyscallSignal)n, heap_allocator());
        size_t len = (size_t)name.len;
        if (len > sizeof c->buf - (sizeof tail - 1))
            len = sizeof c->buf - (sizeof tail - 1);
        if (len > 0)
            memcpy(c->buf, name.p, len);
        memcpy(c->buf + len, tail, sizeof tail - 1);
        if (name.len > 0)
            mem_free(heap_allocator(), (void *)(uintptr_t)name.p, (size_t)name.len, 1);
        c->text.p = c->buf;
        c->text.len = (Int)(len + sizeof tail - 1);
    }

    Error e = {&notify_cause_vt, c};
    return e;
}

/* Hands one signal to everybody who wants it. A channel that is full misses it,
 * as in Go, so that one slow reader does not hold up the rest. */
static void notify_deliver(const NotifyEntry *h, int n, OsSignal sig) {
    if (h->c != NULL) {
        (void)chan_try_send(h->c, &sig);
        return;
    }
    BURROW_CALLF(h->ctx->cancel, notify_cause(n));
}

static void notify_process(int n) {
    if (n < 0 || n >= NOTIFY_NSIG)
        return;

    OsSignal sig = os_signal_from_syscall((SyscallSignal)n);

    sync_mutex_lock(&notify_table.mu);
    for (Int i = 0; i < notify_table.n; i++)
        if (notify_want(&notify_table.m[i], n))
            notify_deliver(&notify_table.m[i], n, sig);

    /* Taken out by a stop that is still waiting for what was already queued,
     * and that is exactly what these are. */
    for (Int i = 0; i < notify_table.nstopping; i++)
        if (notify_want(&notify_table.stopping[i], n))
            notify_deliver(&notify_table.stopping[i], n, sig);
    sync_mutex_unlock(&notify_table.mu);
}

/* Go's loop, on a thread rather than a goroutine. */
static void notify_loop(void *arg) {
    (void)arg;
    for (;;)
        notify_process((int)burrow__signal_recv());
}

static void notify_watch(void) {
    if (notify_table.watching)
        return;
    notify_table.watching = true;

#if defined(BURROW_OS_WASI)
    /* No signal ever arrives on wasip1, where Go's signal_enable does nothing,
     * and there is only the one thread. A reader would wait for ever and take
     * the thread every goroutine runs on with it. */
    (void)notify_loop;
    return;
#else
    burrow__Thread t;
    if (!burrow__thread_start(&t, notify_loop, NULL, 0))
        runtime_throw(
            BURROW_S("os/signal: cannot start the thread that delivers signals"));
    (void)burrow__thread_detach(&t);
#endif
}

/* ------------------------------------------------------------- Notify */

/* Go's Notify after the nil check, for a channel or a context. sigs is n
 * signals, and n of 0 is all of them. */
static void notify_add_all(const void *key, Chan *c, NotifyCtx *ctx,
                           const OsSignal *sigs, Int n) {
    sync_mutex_lock(&notify_table.mu);

    Int count = n == 0 ? NOTIFY_NSIG : n;
    for (Int i = 0; i < count; i++) {
        int sig = n == 0 ? (int)i : notify_signum(sigs[i]);
        if (sig < 0)
            continue;

        /* Go 1.27 makes the entry here rather than up front, so that a Notify
         * with nothing it can use leaves no entry behind for Stop to find. */
        NotifyEntry *h = notify_find(key);
        if (h == NULL) {
            notify_table.m =
                notify_grow(notify_table.m, notify_table.n, &notify_table.cap);
            h = &notify_table.m[notify_table.n++];
            memset(h, 0, sizeof *h);
            h->key = key;
            h->c = c;
            h->ctx = ctx;
        }

        if (!notify_want(h, sig)) {
            notify_set(h, sig);
            if (notify_table.ref[sig] == 0) {
                burrow__signal_enable((uint32_t)sig);
                notify_watch();
            }
            notify_table.ref[sig]++;
        }
    }

    sync_mutex_unlock(&notify_table.mu);
}

static void notify_check(Chan *c) {
    if (c == NULL)
        panic_str(BURROW_S("os/signal: Notify using nil channel"));
    if (chan_elem(c) != TYPE_OS_SIGNAL)
        panic_str(
            BURROW_S("os/signal: Notify using a channel that is not of OsSignal"));
}

void signal_notify(Chan *c, Slice sigs) {
    notify_check(c);
    notify_add_all(c, c, NULL, (const OsSignal *)sigs.p, sigs.len);
}

/* The variadic forms collect into a buffer and call the Slice ones. Most calls
 * name a signal or two, so the buffer is on the stack unless there are a lot of
 * them, and the program giving up on a refused allocation is what a refused
 * map insert in Go does. */
enum { NOTIFY_SMALL = 16 };

static OsSignal *notify_va(int n, va_list ap, OsSignal *small) {
    OsSignal *store = small;
    if (n > NOTIFY_SMALL) {
        store = (OsSignal *)mem_alloc(heap_allocator(), sizeof(OsSignal) * (size_t)n,
                                      _Alignof(OsSignal));
        if (store == NULL)
            runtime_throw(BURROW_S("os/signal: out of memory"));
    }
    for (int i = 0; i < n; i++)
        store[i] = va_arg(ap, OsSignal);
    return store;
}

static void notify_va_done(int n, OsSignal *store, OsSignal *small) {
    if (store != small)
        mem_free(heap_allocator(), store, sizeof(OsSignal) * (size_t)n,
                 _Alignof(OsSignal));
}

static Slice notify_slice(OsSignal *p, int n) {
    Slice s = {p, (Int)n, (Int)n, TYPE_OS_SIGNAL};
    return s;
}

void signal_notify_v(Chan *c, int n, ...) {
    notify_check(c);
    if (n < 0)
        n = 0;

    OsSignal small[NOTIFY_SMALL];
    va_list ap;
    va_start(ap, n);
    OsSignal *store = notify_va(n, ap, small);
    va_end(ap);

    notify_add_all(c, c, NULL, store, (Int)n);
    notify_va_done(n, store, small);
}

/* --------------------------------------------------- Reset, Ignore, Stop */

/* Go's cancel: takes the signals away from every entry, drops entries that are
 * left wanting nothing, and does action to each signal whether or not anybody
 * had it, which is how Ignore reaches a signal nobody asked for. */
static void notify_cancel(const OsSignal *sigs, Int n, void (*action)(uint32_t)) {
    sync_mutex_lock(&notify_table.mu);

    Int count = n == 0 ? NOTIFY_NSIG : n;
    for (Int k = 0; k < count; k++) {
        int sig = n == 0 ? (int)k : notify_signum(sigs[k]);
        if (sig < 0)
            continue;

        for (Int i = 0; i < notify_table.n;) {
            NotifyEntry *h = &notify_table.m[i];
            if (notify_want(h, sig)) {
                notify_table.ref[sig]--;
                notify_clear(h, sig);
                if (notify_empty(h)) {
                    notify_delete(h);
                    continue;
                }
            }
            i++;
        }

        action((uint32_t)sig);
    }

    sync_mutex_unlock(&notify_table.mu);
}

void signal_reset(Slice sigs) {
    notify_cancel((const OsSignal *)sigs.p, sigs.len, burrow__signal_disable);
}

void signal_reset_v(int n, ...) {
    if (n < 0)
        n = 0;

    OsSignal small[NOTIFY_SMALL];
    va_list ap;
    va_start(ap, n);
    OsSignal *store = notify_va(n, ap, small);
    va_end(ap);

    signal_reset(notify_slice(store, n));
    notify_va_done(n, store, small);
}

void signal_ignore(Slice sigs) {
    notify_cancel((const OsSignal *)sigs.p, sigs.len, burrow__signal_ignore);
}

void signal_ignore_v(int n, ...) {
    if (n < 0)
        n = 0;

    OsSignal small[NOTIFY_SMALL];
    va_list ap;
    va_start(ap, n);
    OsSignal *store = notify_va(n, ap, small);
    va_end(ap);

    signal_ignore(notify_slice(store, n));
    notify_va_done(n, store, small);
}

bool signal_ignored(OsSignal sig) {
    int n = notify_signum(sig);
    return n >= 0 && burrow__signal_ignored((uint32_t)n);
}

/* Go's Stop, for a channel or a context. */
static void notify_stop_key(const void *key) {
    sync_mutex_lock(&notify_table.mu);

    NotifyEntry *found = notify_find(key);
    if (found == NULL) {
        sync_mutex_unlock(&notify_table.mu);
        return;
    }
    NotifyEntry h = *found;
    notify_delete(found);

    for (int n = 0; n < NOTIFY_NSIG; n++) {
        if (notify_want(&h, n)) {
            notify_table.ref[n]--;
            if (notify_table.ref[n] == 0)
                burrow__signal_disable((uint32_t)n);
        }
    }

    /* A signal can be on its way to this entry already: queued by the handler
     * and not yet taken by the reader, or taken and waiting for the lock. Go's
     * answer is to keep the entry where the reader can still see it until the
     * queue has drained, so that the signal goes where it was meant to rather
     * than to nobody, and then to take it away. Doing that is what makes Stop
     * a promise: once it returns, the channel gets nothing more. */
    notify_table.stopping = notify_grow(notify_table.stopping, notify_table.nstopping,
                                        &notify_table.capstopping);
    notify_table.stopping[notify_table.nstopping++] = h;

    sync_mutex_unlock(&notify_table.mu);

    burrow__signal_wait_until_idle();

    sync_mutex_lock(&notify_table.mu);
    for (Int i = 0; i < notify_table.nstopping; i++) {
        if (notify_table.stopping[i].key == key) {
            memmove(&notify_table.stopping[i], &notify_table.stopping[i + 1],
                    sizeof(NotifyEntry) * (size_t)(notify_table.nstopping - i - 1));
            notify_table.nstopping--;
            break;
        }
    }
    sync_mutex_unlock(&notify_table.mu);
}

void signal_stop(Chan *c) {
    notify_stop_key(c);
}

/* ------------------------------------------------------- NotifyContext */

/* Go's signalCtx.stop: cancel with no cause, then Stop. Either half on its own
 * can be repeated, so the whole can. */
static void notify_ctx_stop(void *env) {
    NotifyCtx *nc = (NotifyCtx *)env;
    BURROW_CALLF(nc->cancel, BURROW_NO_ERROR);
    notify_stop_key(nc);
}

/* What context_release calls before it lets the context go. The entry is taken
 * out first, which waits for any signal on its way to it, so nothing can be
 * calling the cancel function when the context is freed. */
static void notify_ctx_release(void *env) {
    NotifyCtx *nc = (NotifyCtx *)env;
    notify_stop_key(nc);
    mem_free(nc->a, nc, sizeof(NotifyCtx), _Alignof(NotifyCtx));
}

static void notify_ctx_stop_nothing(void *env) {
    (void)env;
}

Context signal_notify_context(Alloc *a, Context parent, ContextCancelFunc *stop,
                              Slice sigs) {
    Context none = {NULL, NULL};

    if (stop != NULL)
        *stop = BURROW_FN(ContextCancelFunc, notify_ctx_stop_nothing, NULL);

    NotifyCtx *nc = BURROW_NEW(a, NotifyCtx);
    if (nc == NULL)
        return none;
    nc->a = a;

    Context ctx = burrow__context_with_cancel_hook(
        a, parent, &nc->cancel, BURROW_FN(Func, notify_ctx_release, nc));
    if (ctx.vt == NULL) {
        mem_free(a, nc, sizeof(NotifyCtx), _Alignof(NotifyCtx));
        return none;
    }

    notify_add_all(nc, NULL, nc, (const OsSignal *)sigs.p, sigs.len);

    if (stop != NULL)
        *stop = BURROW_FN(ContextCancelFunc, notify_ctx_stop, nc);
    return ctx;
}

Context signal_notify_context_v(Alloc *a, Context parent, ContextCancelFunc *stop,
                                int n, ...) {
    if (n < 0)
        n = 0;

    OsSignal small[NOTIFY_SMALL];
    va_list ap;
    va_start(ap, n);
    OsSignal *store = notify_va(n, ap, small);
    va_end(ap);

    Context ctx = signal_notify_context(a, parent, stop, notify_slice(store, n));
    notify_va_done(n, store, small);
    return ctx;
}
