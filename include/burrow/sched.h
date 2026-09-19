/* The three things Go's scheduler is made of, and the queues that connect them.
 *
 * G is a goroutine, M is an operating system thread, P is a scheduling context
 * that owns a run queue. There are GOMAXPROCS Ps, as many Ms as there are
 * goroutines stuck in system calls plus a few, and as many Gs as the program
 * asks for. An M runs a G only while holding a P, which bounds the number of
 * goroutines running at once without bounding the number of threads.
 *
 * The design is Go's and the names are Go's, kept letter for letter rather than
 * improved on. Anybody who has read runtime/proc.go should be able to read this
 * and anybody who has not should be able to go and read that. The details are
 * copied for the same reason: the search order in findrunnable and the fact
 * that a thief takes half of a victim's queue are tuned rather than arbitrary,
 * and a port that changes them has changed how programs schedule.
 *
 * A runnable goroutine is in exactly one of three places:
 *
 *   p->runnext        one goroutine, the one that just woke somebody up
 *   p->runq           a ring of 256, owned by the P, stolen from by others
 *   the global queue  a linked list under a lock, for everything that overflows
 *
 * The order is deliberate. runnext is a single slot because the common case in a
 * channel handoff is a goroutine readying exactly one other and then blocking,
 * and running that one next keeps the value it just wrote in this core's cache.
 * The ring is lock free at one end for the owner and at the other for a thief,
 * so a P taking its own work is a load and a store with no atomic read modify
 * write at all. The global queue takes the overflow and is the only part with a
 * lock on it, which is why half a ring goes there at a time.
 *
 * What is here is the structures and the queues. The scheduler that drives
 * them, which is findrunnable and the loop around it, is the next file along.
 * The split is so that the queues can be tested with the goroutines standing
 * still, which is the only way to find out whether stealing takes the half it
 * says it takes.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SCHED_H
#define BURROW_SCHED_H

#include "burrow/context.h"
#include "burrow/lock.h"
#include "burrow/note.h"
#include "burrow/own.h"
#include "burrow/platform.h"
#include "burrow/stack.h"
#include "burrow/thread.h"
#include "burrow/timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How far apart two things have to be to stop sharing a cache line.
 *
 * 64 on most things and 128 on arm64, ppc64 and s390x, which is the same split
 * Go makes in internal/cpu for the same reason: those three either have 128 byte
 * lines or prefetch in pairs, so 64 bytes of padding leaves two fields sharing a
 * line anyway. Getting this wrong is not a correctness problem, it is a P's
 * queue head being written by a thief in the same line as the tail this P writes
 * on every put, which costs a cache line transfer per goroutine. */
#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_PPC64) ||                        \
    defined(BURROW_ARCH_S390X)
#define BURROW_CACHELINE 128
#else
#define BURROW_CACHELINE 64
#endif

/* How many goroutines a P holds before the overflow goes to the global queue.
 *
 * Go's number. It is a power of two so the wrap is a mask, and it is large
 * enough that a goroutine spawning a few hundred children never touches the
 * lock, and small enough that the array is 2 kilobytes on a 64 bit machine and
 * fits in the P alongside everything else. */
#define BURROW_RUNQ_SIZE 256

typedef struct burrow__G burrow__G;
typedef struct burrow__P burrow__P;
typedef struct burrow__M burrow__M;

/* What a goroutine is doing. Go's names and Go's meanings.
 *
 * The transitions that matter: RUNNABLE to RUNNING when an M picks it up,
 * RUNNING to WAITING when it parks and to RUNNABLE when it yields, WAITING to
 * RUNNABLE when somebody readies it, and RUNNING to DEAD when it returns. IDLE
 * is a G that has been allocated and not started, which is the state a fresh one
 * is in for the few instructions between coming off the free list and going on a
 * run queue. */
typedef enum burrow__GStatus {
    BURROW_GIDLE = 0,
    BURROW_GRUNNABLE = 1,
    BURROW_GRUNNING = 2,
    BURROW_GSYSCALL = 3,
    BURROW_GWAITING = 4,
    BURROW_GDEAD = 5
} burrow__GStatus;

/* What a P is doing. Go's names again, minus the one for the phase of garbage
 * collection where every P is stopped, since there is no collector here to stop
 * them.
 *
 * IDLE is a P with no M holding it, which is a P on the idle list waiting for a
 * thread to come and pick it up. SYSCALL is a P whose M has gone into the
 * kernel and may not come back for a while, which is the state sysmon looks for
 * when it decides to hand the P to somebody else. DEAD is a P that GOMAXPROCS
 * has shrunk away. */
typedef enum burrow__PStatus {
    BURROW_PIDLE = 0,
    BURROW_PRUNNING = 1,
    BURROW_PSYSCALL = 2,
    BURROW_PDEAD = 3
} burrow__PStatus;

/* A goroutine.
 *
 * One allocation and one stack. The context is where it was when it last
 * stopped, which for a goroutine that has never run is the trampoline that will
 * call its entry function.
 *
 * `next` is how this ends up on a list, and it is a plain field rather than an
 * atomic because every list that uses it is either owned by one P or held under
 * the scheduler lock. The per P ring does not use it: that one holds pointers in
 * an array, so a goroutine can be in the ring and have `next` left over from the
 * last list it was on, and nothing reads it there.
 *
 * The status is atomic because a thief reads it while the owner writes it, and
 * because it is the field a debugger and a future traceback will want without
 * stopping the world for it. */
struct burrow__G {
    /* Where to resume. First, because the switch touches it on every schedule
     * and a goroutine that is about to run is a cache miss either way. */
    burrow__Context ctx;

    /* The memory ctx runs on, from burrow/stack.h. Kept when the goroutine dies
     * so that the next one on this P can have it without a system call. */
    burrow__Stack stack;

    /* What it was asked to run. Kept after the call starts so that a traceback
     * can say what this goroutine is, which is the first question anybody asks
     * of a stuck one. */
    void (*entry)(void *);
    void *arg;

    /* Monotonic and never reused, unlike Go's, which is also monotonic. Zero is
     * not a valid id, so a zeroed G is recognisably not a live one. */
    uint64_t id;

    /* burrow__GStatus. Atomic: read by whoever is stealing or reporting, written
     * by whoever is running it. */
    uint32_t status;

    /* Set while this goroutine is on a free list or a linked queue. Not used by
     * the per P ring. */
    burrow__G *next;

    /* The timer time_sleep parks on, made the first time this goroutine sleeps
     * and kept from then on, including while it is dead on a free list. Go's g
     * has the same field for the same two reasons.
     *
     * It cannot live on the sleeping goroutine's stack, because a timer that has
     * fired stays in its P's heap until that P throws it out, which is after the
     * sleep has returned and the frame holding it has gone. And it is kept
     * rather than freed at every wake because a goroutine that sleeps once
     * usually sleeps again, and this way a loop with a sleep in it allocates
     * nothing after the first turn. */
    BURROW_OWNS(1) burrow__Timer *timer;

    /* Every G ever created, in one list under the scheduler lock. Go calls it
     * allgs and keeps it for the same two reasons: a traceback has to be able to
     * name every goroutine, and shutting the runtime down has to be able to give
     * back the stack of a goroutine that parked and never woke up. A G is on
     * this list from the moment it exists until the runtime stops, dead or
     * alive, so `next` and `allnext` are never the same list. */
    burrow__G *allnext;
};

/* A scheduling context. GOMAXPROCS of these and no more, which is what makes
 * the number of goroutines running at once a number the program chose.
 *
 * The three padded blocks are not decoration. `runqhead` is written by every
 * thief and `runqtail` by the owner on every put, so the two of them in one
 * cache line turns a lock free queue into a contended one.
 *
 * That padding costs a few hundred bytes per P and a linter will offer to pack
 * it away, which is why the suppression is here. There are GOMAXPROCS of these
 * for the life of the process, so the whole cost on a 64 core machine is around
 * 30 kilobytes, and what it buys is not having a cache line bounce between two
 * cores on every goroutine that gets scheduled. */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct burrow__P {
    /* Which P this is, and what it is doing. The status is atomic and is read by
     * a thief deciding whether to take this P's runnext, so it is not purely the
     * scheduler file's business even though the scheduler file is what writes
     * it. */
    int32_t id;
    uint32_t status;

    /* The M running this P, or NULL when it is idle. Borrowed: an M owns
     * itself. */
    BURROW_BORROWS(1) burrow__M *m;

    /* Next on the idle P list, under the scheduler lock. A P is on that list
     * exactly when no M is holding it, which is the same thing as `m` being
     * NULL, and the two are set together with the lock held so they cannot
     * disagree. */
    BURROW_BORROWS(1) burrow__P *link;

    /* Taken by a thief with a compare and swap, so it has to be atomic and it
     * has to be alone. This is the goroutine a channel send just made runnable
     * and the one the receiver should run next, so it is worth a slot of its
     * own and worth the thief having to work for it. */
    _Alignas(BURROW_CACHELINE) burrow__G *runnext;

    /* Consumer end. The owner moves it when it takes its own work and a thief
     * moves it when it steals, both with a compare and swap, so this line is
     * contended by design and is padded away from the tail for that reason. */
    _Alignas(BURROW_CACHELINE) uint32_t runqhead;

    /* Producer end. Only ever written by the owner, and with a release store
     * rather than a read modify write, which is what makes putting a goroutine
     * on your own queue as cheap as it is. */
    _Alignas(BURROW_CACHELINE) uint32_t runqtail;

    /* The ring itself. Indices are free running and masked, so the queue is
     * empty when head equals tail and full when tail minus head is the size,
     * and neither of those needs a separate count. */
    burrow__G *runq[BURROW_RUNQ_SIZE];

    /* This P's timers. The set has its own lock, because a thread with nothing
     * to run looks at other Ps' timers before it parks, the same way it looks
     * at their run queues. See burrow/timer.h. */
    burrow__Timers timers;

    /* Dead goroutines with their stacks still attached, owned by this P alone so
     * that taking one needs no atomic at all. Bounded, because a P that spawned
     * a million goroutines once should not hold a million stacks forever: the
     * overflow goes to a central list the scheduler file owns. */
    BURROW_OWNS(1) burrow__G *gfree;
    int32_t gfree_count;
};

/* An operating system thread, and the bookkeeping that belongs to the thread
 * rather than to the work it is doing.
 *
 * g0 is the goroutine the scheduler itself runs on, which exists because
 * choosing what to run next cannot happen on the stack of the thing that just
 * stopped running. Go calls it g0 and so does this. It has a real stack and no
 * entry function, because nothing ever starts it: it is what an M is already on
 * when it is not running anybody. */
struct burrow__M {
    /* The scheduler's own goroutine on this thread. Not on any queue, never
     * runnable, and the context every switch goes through. */
    burrow__G g0;

    /* What this M is running now, or NULL if it is between goroutines. */
    BURROW_BORROWS(1) burrow__G *curg;

    /* The P this M is holding, or NULL if it has none, which is what an M
     * without work or an M in a system call looks like. */
    BURROW_BORROWS(1) burrow__P *p;

    /* The P this M is to pick up when it wakes, handed over by whoever woke it.
     * Go has the same field for the same reason: the waker holds the scheduler
     * lock and knows which P is free, and making the sleeper go and look again
     * after it wakes is a second trip through the lock and a chance for somebody
     * else to take the P in between. */
    BURROW_BORROWS(1) burrow__P *nextp;

    /* What to run on g0 after the next switch back to it, and the goroutine that
     * asked for it.
     *
     * This pair is Go's mcall, which is a call that happens on the scheduler's
     * stack rather than on the caller's. Everything that stops a goroutine needs
     * it. Parking a goroutine means publishing it somewhere another M can find
     * it, and doing that while still standing on its stack is a race with that
     * other M picking it up, so the switch has to come first and the bookkeeping
     * has to come after. Go writes this in assembly. Here the switch lands back
     * in the scheduler loop, which reads these two and makes the call, which
     * needs no assembly and costs one branch per schedule.
     *
     * The call answers the goroutine to run next, or NULL for go and find one.
     * That is Go's tail call out of park_m into execute, spelled as a return
     * value because a C function that returns cannot tail call the thing that
     * called it. */
    burrow__G *(*mcall)(burrow__M *m, burrow__G *g);
    BURROW_BORROWS(1) burrow__G *mcallg;

    /* What sched_park was asked to unlock once this goroutine is off its stack,
     * and the pointer to hand it. Set by sched_park and read by the call it
     * leaves behind, both on this thread with a stack switch in between, which
     * is the only reason they have to live here rather than in a local.
     *
     * Spelled out longhand rather than through SchedUnlockFn so that this header
     * does not have to include burrow/proc.h. The two types are the same type. */
    bool (*parkunlock)(burrow__G *g, void *lock);
    void *parklock;

    /* The gate this M sleeps on when there is nothing to run. One per M rather
     * than one shared one, so that waking a thread wakes the thread that was
     * chosen rather than all of them. */
    burrow__Note park;

    /* The thread underneath. Borrowed from whoever started it, which for every M
     * but the first is the scheduler. */
    burrow__Thread thread;

    /* Set while this M is looking for work and has no P, which is how the
     * scheduler avoids waking a second thread to do a search that a first thread
     * is already doing. */
    uint32_t spinning;

    /* Next on the idle list, under the scheduler lock. */
    burrow__M *next;

    /* Monotonic, and mostly for reading in a debugger. */
    int64_t id;
};

/* ------------------------------------------------------------------ the ring
 *
 * Three operations and all three are Go's, including the parts that look like
 * they could be simpler.
 *
 * Nothing here allocates, nothing here blocks, and nothing here takes a lock. A
 * put that finds the ring full is the one case that reaches further, and it
 * reaches into the global queue, which is why it is spelled as a separate
 * function that the scheduler file supplies. */

/* Puts `g` on `p`'s local queue and answers true.
 *
 * `next` asks for the runnext slot, which is for the goroutine this one just
 * made runnable and expects to hand over to. Whatever was in runnext moves to
 * the tail of the ring, so the slot is a one deep queue rather than a place
 * things get lost.
 *
 * False means the ring is full, and a full ring is not an error: it means the
 * caller has to move half of it to the global queue and try again. The caller
 * does that rather than this function, because this file does not know what a
 * global queue is and the test for this file should not need one.
 *
 * On false, `*overflow` is the goroutine that still needs a home, and it is not
 * always the `g` that came in. A put with `next` set swaps into the runnext slot
 * before it goes anywhere near the ring, so the one that overflows is whatever
 * came out of that slot. Handing the wrong one of the two to
 * burrow__runq_put_slow loses a goroutine and runs another one twice, which is
 * why this is an out parameter rather than something the caller is trusted to
 * work out. `overflow` may not be NULL.
 *
 * Only the P's own M may call this. A thief adds to its own queue, never to
 * somebody else's. */
bool burrow__runq_put(burrow__P *p, BURROW_RETAINS(2) burrow__G *g, bool next,
                      BURROW_BORROWS(ret) burrow__G **overflow);

/* Takes the next goroutine off `p`'s queue, or NULL if there is nothing on it.
 *
 * runnext first, then the head of the ring. Only the P's own M may call this,
 * which is what lets the ring half of it be a load and a compare and swap
 * instead of a lock.
 *
 * The compare and swap is there even though only one thread takes from the head
 * of its own queue, because thieves take from that same end. */
BURROW_BORROWS(ret) burrow__G *burrow__runq_get(burrow__P *p);

/* How many runnable goroutines `p` is holding, runnext included.
 *
 * A snapshot, and on a queue somebody else is stealing from it can be out of
 * date before it is returned. It is for deciding whether to bother looking and
 * for tests, not for anything that has to be exact. */
uint32_t burrow__runq_len(const burrow__P *p);

/* Moves up to half of `victim`'s queue into `thief`'s and answers one goroutine
 * to run now, or NULL if there was nothing to take.
 *
 * Half, because taking one at a time means coming back for the next one and
 * taking all of it means the victim goes idle and starts stealing back. Go takes
 * half and so does this.
 *
 * The one it answers with is the last of the batch and it never becomes visible
 * in the thief's ring at all, which is a goroutine that does not have to be
 * pushed and popped for no reason. The rest are in the ring by the time this
 * returns.
 *
 * `steal_runnext` asks to take the victim's runnext slot when there is nothing
 * else to take. It is the last resort, since that goroutine is the one the
 * victim is about to run and taking it is exactly the cache miss runnext exists
 * to avoid, so the scheduler only asks on the final pass of a search that has
 * already failed everywhere else.
 *
 * Runs on the thief's thread and touches the victim's head only with a compare
 * and swap, so the victim never has to know this happened. */
BURROW_BORROWS(ret) burrow__G *burrow__runq_steal(burrow__P *thief, burrow__P *victim,
                                                  bool steal_runnext);

/* ------------------------------------------------------------- the global queue
 *
 * A linked list through `next` with a head and a tail, so that putting a batch
 * on the end is a pointer write rather than a walk. Every operation on it is
 * under the scheduler's lock, which the scheduler file owns, so these take the
 * queue and not the lock and it is the caller's job to be holding it.
 *
 * It is a struct rather than two globals because a P's overflow goes into a
 * temporary one of these on the way, and because a test can then have one
 * without the scheduler being running. */
typedef struct burrow__GQueue {
    BURROW_OWNS(1) burrow__G *head;
    BURROW_BORROWS(1) burrow__G *tail;
    int32_t len;
} burrow__GQueue;

/* Adds one goroutine to the end of the queue. */
void burrow__gqueue_push(burrow__GQueue *q, BURROW_RETAINS(2) burrow__G *g);

/* Adds one goroutine to the front, for a goroutine that was taken off and has to
 * go back, which is what a failed steal and a preempted G both need. */
void burrow__gqueue_push_head(burrow__GQueue *q, BURROW_RETAINS(2) burrow__G *g);

/* Takes one off the front, or NULL when the queue is empty. */
BURROW_BORROWS(ret) burrow__G *burrow__gqueue_pop(burrow__GQueue *q);

/* Moves everything in `src` onto the end of `dst` and leaves `src` empty. Two
 * pointer writes whatever the length, which is the reason the tail is kept. */
void burrow__gqueue_push_all(burrow__GQueue *dst, burrow__GQueue *src);

/* Moves half of `p`'s ring, plus `g`, into `batch`, and answers false if the
 * ring turned out not to be full after all.
 *
 * This is the slow half of a put that found the ring full. It is here rather
 * than in the scheduler because it is the ring's own indices it has to
 * manipulate, and false rather than a retry loop because the caller is the one
 * that knows what to do with a goroutine that now has somewhere to go.
 *
 * `g` goes on the end of the batch, so the goroutines that have been waiting
 * longest are the ones that end up on the global queue and the newest one is
 * the one that stays close to the P that made it. */
bool burrow__runq_put_slow(burrow__P *p, BURROW_RETAINS(2) burrow__G *g,
                           burrow__GQueue *batch);

/* --------------------------------------------------------- the scheduler
 *
 * src/runtime/sched.c, and the part of it the rest of the runtime is allowed to
 * see. Everything a program outside the runtime wants is in burrow/proc.h
 * instead, spelled without the burrow__ and documented for somebody who has not
 * read runtime/proc.go.
 *
 * These four exist so that channels, timers and the netpoller can find the P
 * they are running on, which is where a timer heap lives and where a ready
 * goroutine goes. They are also what the scheduler's own tests look at, since a
 * work stealing scheduler that is only observed through its public API is one
 * whose interesting states are all invisible. */

/* The largest GOMAXPROCS this build will accept.
 *
 * The Ps are a static array, so this is the size of it. That choice is worth a
 * sentence: it is in BSS, so it costs address space and not memory until a P is
 * actually used, and it means the scheduler has no allocator underneath it and
 * cannot fail to start. Go reaches for persistentalloc here, which is the same
 * decision with more moving parts.
 *
 * Define it yourself on a machine with more than 256 processors, or on a small
 * one where even untouched BSS is worth counting. */
#ifndef BURROW_MAXPROCS
#define BURROW_MAXPROCS 256
#endif

/* The M the calling thread is, or NULL if this thread is not one of the
 * scheduler's. */
BURROW_STATIC(ret) burrow__M *burrow__curm(void);

/* The goroutine running on this thread, or NULL. The same thing sched_current
 * answers, under the internal name. */
BURROW_BORROWS(ret) burrow__G *burrow__curg(void);

/* The P at index i, or NULL if i is not in range. Every P exists for as long as
 * the scheduler is running, whether or not an M is holding it. */
BURROW_STATIC(ret) burrow__P *burrow__allp(int32_t i);

/* How many Ps there are. Fixed while the scheduler is running. */
int32_t burrow__gomaxprocs(void);

/* The calling goroutine's own timer, the one time_sleep parks on, made on the
 * first call and handed back on every one after it.
 *
 * NULL on a thread that is not running a goroutine, and NULL if the allocator
 * would not give out the timer, which is the only allocation a sleep ever makes.
 * Callers treat those two the same way and sleep the thread instead.
 *
 * It belongs to the goroutine and the scheduler frees it, so the caller arms it
 * and nothing else. Nobody else may use it, because a goroutine sleeps in one
 * place at a time by definition. */
BURROW_BORROWS(ret) burrow__Timer *burrow__sleep_timer(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SCHED_H */
