/* Freeing memory that a reader without a lock might still be looking at.
 *
 * Go solves this with the collector and never talks about it. A lock free map
 * unlinks a node, the last reader eventually drops its pointer, and the node is
 * collected some time later. Nobody writes the reclamation code because nobody
 * has to. burrow has no collector in its own libraries, so it has to write that
 * code, and this file is it.
 *
 * The shape is the one everybody arrives at. A reader announces that it is
 * about to follow pointers, follows them, and announces that it has stopped. A
 * writer that unlinks something does not free it, it hands it over. The
 * handover list is freed once every reader that could have been holding a
 * pointer to it has announced that it stopped.
 *
 *     burrow__pin();
 *     Node *n = burrow__atomic_load_ptr(&head);
 *     use(n);
 *     burrow__unpin();
 *
 *     // the writer
 *     if (burrow__atomic_cas_ptr(&head, &n, replacement))
 *         burrow__retire(&n->retired, free_node, n);
 *
 * The "eventually" is a counter. There is one epoch number for the process, and
 * a pinned reader publishes the epoch it pinned at. The epoch can move forward
 * whenever every pinned reader is at the current one, and something handed over
 * during epoch N is safe to free once the epoch reaches N plus two. Two rather
 * than one because a reader pinned at N may have picked up its pointer before
 * the unlink, and it takes one more step to be sure every such reader has gone.
 * This is Fraser's scheme and it is what crossbeam and most lock free C++
 * libraries use.
 *
 * Internal. This is the floor that sync.Map stands on, and later the netpoller
 * and sync.Pool. Nothing outside burrow should reach for it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_RECLAIM_H
#define BURROW_RECLAIM_H

#include "burrow/core.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What it costs. A pin is one sequentially consistent store, which is an xchg
 * on x86 and a store release followed by a fence on arm64. An unpin is a store
 * release, which is free on x86. A retire is a few stores into a list the
 * calling thread owns. Every so often one of those retires also walks the
 * participant slots and frees a batch, and that cost is spread over the batch.
 *
 * What it does not do. It is not a collector: it will not find a cycle, it has
 * no idea what your object points at, and it frees exactly what you hand it. It
 * also does not bound how long a handover waits. A thread that pins and then
 * blocks for a second holds the epoch still for a second, and everything
 * retired in that time waits. That is the deal with every epoch scheme and it
 * is where the one rule here comes from.
 *
 * The rule is: never park while pinned. Not with a channel, not on a
 * sync.Mutex, not on anything that can put the goroutine to sleep. A pin
 * belongs to the thread and not to the goroutine, so a goroutine that parks in
 * the middle of one leaves the pin behind for whatever runs next on that
 * thread. Take the pin, walk the structure, drop the pin. It is the same rule
 * Go's runtime has for acquirem, for the same reason. */

/* The bookkeeping one handed over object needs, embedded in the object itself.
 *
 * Embedded rather than allocated, because a retire happens on a path that holds
 * no lock and has nowhere to report a failure, and an allocator that can fail is
 * not something that path can call. The three words are the price, and they are
 * paid by the node type rather than by this file.
 *
 * Put one in whatever you intend to hand over and do not look at the fields. */
typedef struct burrow__Retired burrow__Retired;
struct burrow__Retired {
    burrow__Retired *next;
    void (*free)(void *obj);
    void *obj;
};

/* Says that this thread is about to follow pointers into a structure that
 * somebody else may be unlinking from.
 *
 * Nests. A pin inside a pin costs a counter and the epoch is published once, by
 * the outermost one, which is what makes it safe for a function that pins to
 * call another function that pins.
 *
 * The caller must not park before the matching unpin. See the note at the top
 * of this file, which is the one rule here that a mistake does not report. */
void burrow__pin(void);

/* Says that this thread has stopped following those pointers, and that anything
 * it was holding may now be freed once the epoch has moved twice.
 *
 * Every pin needs exactly one of these, on the same thread, and a path that can
 * return early needs one on every way out. */
void burrow__unpin(void);

/* Hands `obj` over to be freed once no reader can still be holding it.
 *
 * `r` has to live inside `obj`, or at least outlive it, because this list is
 * threaded through the objects on it. `free` is called with `obj` some time
 * later, on a thread that is not necessarily this one, and with no lock held.
 *
 * The object must already be unlinked. This does not unlink anything and has no
 * way to tell whether you did: it is the step after the compare and swap that
 * took the object out, not a substitute for it. Handing over an object that is
 * still reachable frees memory that is still in use.
 *
 * Cannot fail and does not allocate. */
void burrow__retire(burrow__Retired *r, void (*free)(void *obj), void *obj);

/* Pushes whatever this thread has retired but not yet handed to the shared list,
 * and tries to move the epoch on.
 *
 * Retires do this by themselves every so often, so nothing has to call this to
 * make progress. It is here for a test that wants the reclamation to have
 * happened by the time the next line runs, and for a caller that has just
 * finished a burst of deletes and would rather pay for them now. */
void burrow__reclaim_flush(void);

/* Frees everything outstanding, epoch or no epoch.
 *
 * Only correct when nothing is pinned and nothing can pin, which in practice
 * means the process is shutting the runtime down or a test is finishing. It
 * exists so that the last few objects retired before the end do not look like a
 * leak to a sanitizer, which is the only reason the end of a process ever needs
 * to free anything. */
void burrow__reclaim_drain(void);

/* How many objects have been handed over and not yet freed.
 *
 * A snapshot, and on a running system it is out of date immediately. For tests
 * and for a debugger, not for a decision. */
Int burrow__reclaim_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_RECLAIM_H */
