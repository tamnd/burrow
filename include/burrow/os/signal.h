/* os/signal: hearing about signals the program is sent.
 *
 *     Chan *c = chan_make(a, TYPE_OS_SIGNAL, 1);
 *     signal_notify_v(c, 2, os_interrupt, os_signal_from_syscall(SYSCALL_SIGTERM));
 *
 *     OsSignal s;
 *     chan_recv(c, &s);    // blocks until Ctrl-C or a kill
 *
 * Without this package a signal does what it does to any C program: SIGINT and
 * SIGTERM end it, SIGWINCH is ignored, and so on. signal_notify changes that
 * for the signals it is given, and from then on they are sent to the channel
 * instead. The channel is sent to without blocking, so a full channel misses
 * a signal rather than holding up every other channel, and one with a buffer
 * of one is enough for a program that waits for a single signal. Two of the
 * same signal that arrive before the first is handed over are one signal, as
 * they always are.
 *
 * signal_stop takes a channel back out, signal_reset gives signals their old
 * behaviour back, and signal_ignore has them ignored. A signal that was
 * ignored when the program started, which is what nohup does with SIGHUP,
 * stays ignored until signal_notify asks for it, and signal_ignored says so.
 *
 * signal_notify_context is the other way to use it: a context that is
 * cancelled when one of the signals arrives, with context_cause saying which.
 *
 *     ContextCancelFunc stop;
 *     Context ctx = signal_notify_context_v(a, context_background(), &stop, 1,
 *                                           os_interrupt);
 *     ...                        // ctx is done after Ctrl-C
 *     BURROW_CALLF0(stop);
 *     context_release(ctx);
 *
 * The signals are delivered by a thread this package starts the first time
 * signal_notify is called, not by a goroutine, so it all works from a plain
 * main as well as under runtime_main. Sending on a channel and cancelling a
 * context both happen on that thread.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package os/signal */

#ifndef BURROW_OS_SIGNAL_H
#define BURROW_OS_SIGNAL_H

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SIGKILL and SIGSTOP cannot be caught, and the signals the program raises by
 * faulting, SIGSEGV, SIGBUS, SIGFPE and SIGILL among them, belong to the
 * runtime, which turns them into panics. SIGPROF is the profiler's. Asking for
 * any of those is not an error, and the channel never hears about them. On
 * Linux the real time signals glibc and musl keep for themselves, 32 and 33
 * and the ones up to SIGRTMIN, are left alone the same way.
 *
 * On Windows there are no signals, and Ctrl-C and Ctrl-Break arrive as
 * os_interrupt while closing the console, logging off and shutting down arrive
 * as SIGTERM, which is how Go maps them. For the last three the process ends
 * when the console handler returns, so the handler does not return: the
 * program has the few seconds Windows gives it to finish up. signal_ignore
 * stops the channels getting a signal there but cannot stop Ctrl-C ending the
 * program, which is how Go behaves on Windows as well.
 *
 * Go's NotifyContext returns a context whose String names the signals. burrow
 * contexts have no String, so that part is not here. */

/* signal.Notify: sends the signals in sigs, a Slice of OsSignal, to c, which
 * has to be a channel made with TYPE_OS_SIGNAL. No signals means every signal.
 * Calling it again for the same channel adds to what it gets. A NULL channel
 * panics, as in Go. */
void signal_notify(Chan *c, Slice sigs);

/* signal_notify without a Slice. Every argument after n must be an OsSignal,
 * and n of 0 means every signal. */
void signal_notify_v(Chan *c, int n, ...);

/* signal.Stop: c gets no more signals once this returns, and a signal no
 * channel wants any more goes back to what it did before. */
void signal_stop(Chan *c);

/* signal.Reset: undoes signal_notify for these signals, for every channel, and
 * they do what they did before. None means all of them. */
void signal_reset(Slice sigs);
void signal_reset_v(int n, ...);

/* signal.Ignore: these signals are ignored from now on, and no channel gets
 * them. None means all of them. */
void signal_ignore(Slice sigs);
void signal_ignore_v(int n, ...);

/* signal.Ignored: whether sig is ignored, by signal_ignore or because it was
 * ignored when the program started. */
bool signal_ignored(OsSignal sig);

/* signal.NotifyContext: a context made from parent that is cancelled when one
 * of the signals in sigs arrives, or when stop is called, or when parent is
 * done, whichever is first. context_cause then gives "interrupt signal
 * received" or the like, an error that errors_is matches with
 * context_canceled. None means every signal.
 *
 * Call stop as soon as the signals do not need catching any more, which is
 * when they go back to what they did before. It can be called more than once.
 * context_release stops it as well, and stop must not be called after that.
 * The zero Context when a refuses. */
BURROW_OWNS(ret) Context signal_notify_context(Alloc *a, Context parent,
                                               ContextCancelFunc *stop, Slice sigs);
BURROW_OWNS(ret) Context signal_notify_context_v(Alloc *a, Context parent,
                                                 ContextCancelFunc *stop, int n, ...);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_OS_SIGNAL_H */
