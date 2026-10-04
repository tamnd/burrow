/* The queue between a signal handler and os/signal.
 *
 * A handler can do almost nothing, so it does the least it can: it sets one bit
 * in a mask and, if somebody is waiting, wakes them. Somebody is os/signal's
 * reader, a thread that sleeps in burrow__signal_recv and hands each number it
 * gets back to the channels that asked for it. A signal that arrives twice
 * before the reader looks is delivered once, which is the deal a POSIX signal
 * has always offered and the one Go's os/signal documents.
 *
 * This is runtime/sigqueue.go, and the names are Go's with the burrow prefix:
 * signal_enable, signal_disable, signal_ignore and signal_ignored are the four
 * os/signal reaches through linkname, and sigsend is the handler's half.
 *
 * Signal numbers here are the platform's own, the ones syscall.Signal holds,
 * and the queue has room for 0 to 95, the three words Go's has on Linux.
 * Numbers past that are ignored by all of it.
 *
 * Internal. The only callers are src/os/signal.c and the platform layer.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SIGQUEUE_H
#define BURROW_SIGQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many signal numbers the queue covers, Go's _NSIG on Linux. */
enum { BURROW_SIGQUEUE_NSIG = 96 };

/* Queues a signal for the reader, from a signal handler or from anywhere else.
 * False if nobody has enabled it, which tells the handler to do what it would
 * have done without os/signal. Async signal safe. */
bool burrow__signal_send(uint32_t sig);

/* Waits for the next queued signal and returns its number. One reader at a
 * time, and it is os/signal's. */
uint32_t burrow__signal_recv(void);

/* Starts catching a signal and queueing it. The first call sets the platform
 * up to relay signals here, and nothing turns that off again. */
void burrow__signal_enable(uint32_t sig);

/* Stops catching it, and the signal goes back to what it did before the
 * program first asked for it. */
void burrow__signal_disable(uint32_t sig);

/* Stops catching it and has it ignored. */
void burrow__signal_ignore(uint32_t sig);

/* Whether the signal is ignored, either because burrow__signal_ignore said so
 * or because it was SIGHUP or SIGINT and already ignored when the program
 * started, which is what nohup does. */
bool burrow__signal_ignored(uint32_t sig);

/* Returns once every signal the handler has queued so far has been taken by
 * the reader. os/signal's Stop uses it so that a channel it has just removed
 * gets nothing afterwards. */
void burrow__signal_wait_until_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SIGQUEUE_H */
