/* The netpoller: how a goroutine waits for a socket without holding a thread.
 *
 * This is Go's runtime/netpoll.go and the backend files next to it. Nothing
 * above it exists yet, which is the point of building it now: net, net/http and
 * everything that reads a descriptor is a goroutine per connection, and that is
 * only affordable if a goroutine waiting costs a stack and not an OS thread.
 *
 * The deal a caller makes is the same one Go's internal/poll makes. Put the
 * descriptor into non-blocking mode, hand it here once, and from then on do the
 * read or the write yourself. When it answers that it would block, call
 * burrow__poll_wait and the goroutine parks until the kernel says the
 * descriptor is ready again. No data is copied here and no read or write is
 * made here. This layer only answers when it is worth trying again.
 *
 * Registration is edge triggered and it happens once, for reading and writing
 * together, for as long as the descriptor is open. That is Go's choice and it
 * is why a busy connection makes no system calls beyond the reads and writes
 * themselves. What it asks in exchange is the discipline every edge triggered
 * poller asks for: read or write until the answer is that it would block, since
 * a caller that stops early has consumed an edge that will not come again.
 *
 * Windows inverts that deal and the difference is not a small one. IOCP reports
 * finished work, so there a caller hands the kernel the read itself along with
 * a burrow__PollOp and then parks, and what comes back is bytes and an error
 * rather than an invitation to try. BURROW_NETPOLL_READINESS and
 * BURROW_NETPOLL_COMPLETION say which build this is, and a caller has to look.
 * Go does not hide it either: internal/poll has a file per family.
 *
 * The scheduler is the other half of this file. There is no poller thread. A
 * thread that runs out of goroutines to run looks here on its way past, and the
 * last thread with nothing left to do goes to sleep inside the poller rather
 * than on its own note, so waiting for work and waiting for the network are one
 * wait with one wakeup. burrow__netpoll and burrow__netpoll_break are the two
 * calls the scheduler makes for that, and they are not for anybody else.
 *
 * What is not here yet: io_uring, and a backend for the two web targets. There
 * burrow__netpoll_inited answers false and burrow__poll_open refuses.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_NETPOLL_H
#define BURROW_NETPOLL_H

#include "burrow/lock.h"
#include "burrow/own.h"
#include "burrow/platform.h"
#include "burrow/sched.h"
#include "burrow/timer.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which backend this build has. One of these is defined and the other two are
 * not, and every file that implements one is wrapped in its own macro so that a
 * backend which is not wanted on this target compiles to nothing rather than
 * needing the build to leave the file out.
 *
 * io_uring is not a fourth entry here. It is a Linux backend that would sit
 * beside epoll and be chosen at run time rather than at compile time, which is
 * a different kind of decision from this one, and it is worth having only once
 * there is file I/O to point at it. */
#if defined(BURROW_OS_LINUX)
#define BURROW_NETPOLL_EPOLL 1
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || BURROW_BSD
#define BURROW_NETPOLL_KQUEUE 1
#elif defined(BURROW_OS_WINDOWS)
#define BURROW_NETPOLL_IOCP 1
#else
#define BURROW_NETPOLL_NONE 1
#endif

/* Whether this backend reports readiness or completion, which is the one
 * difference a caller cannot be written without knowing. epoll and kqueue say a
 * descriptor is worth trying; IOCP says an operation the caller already handed
 * to the kernel has finished. See burrow__poll_wait, which is where it shows. */
#if defined(BURROW_NETPOLL_IOCP)
#define BURROW_NETPOLL_COMPLETION 1
#elif !defined(BURROW_NETPOLL_NONE)
#define BURROW_NETPOLL_READINESS 1
#endif

/* What a descriptor is on this platform.
 *
 * An int everywhere Unix, and pointer sized on Windows, where a SOCKET is an
 * opaque handle that is not an int and is not always small. The name is here
 * rather than being spelled out at every call so that the signatures in this
 * file do not have to change when the completion backend lands. */
#if defined(BURROW_OS_WINDOWS)
typedef uintptr_t burrow__PollFd;
#else
typedef int burrow__PollFd;
#endif

/* Reading, writing, or both. A bit each rather than Go's 'r' and 'w' characters
 * added together, because the case that carries both is the interesting one and
 * adding two letters to mean it is a trick that has to be explained every time
 * it is read.
 *
 * A wait is for exactly one of the two. Both together is what a backend reports
 * when one event says the descriptor is ready in both directions, which happens
 * whenever a connection is closed at the far end. */
#define BURROW_POLL_READ 1U
#define BURROW_POLL_WRITE 2U

/* How a wait ended.
 *
 * These are Go's four codes from internal/poll, and they are the ones a caller
 * has to tell apart: ready means try the read again, closed means the
 * descriptor went away underneath the wait, timeout means a deadline passed,
 * and unpollable means this descriptor is not one the kernel will report
 * readiness for and the caller should fall back to a blocking call on a thread. */
typedef enum burrow__PollStatus {
    BURROW_POLL_READY = 0,
    BURROW_POLL_CLOSED = 1,
    BURROW_POLL_TIMEOUT = 2,
    BURROW_POLL_UNPOLLABLE = 3
} burrow__PollStatus;

/* Everything the poller knows about one descriptor.
 *
 * One of these is made by burrow__poll_open and lives until burrow__poll_close,
 * and the memory it is in is never given back to the allocator. That is not
 * thrift, it is the only way the kernel's side of this is safe: an event for a
 * descriptor that has just been closed can still be sitting in the kernel's
 * ready list, and a thread draining that list has to be able to look at whatever
 * the event points at without wondering whether it is still there. Go does the
 * same thing and for the same reason, in pollCache.
 *
 * `rg` and `wg` are the two words the whole design turns on. Each is a one
 * place semaphore holding one of four things: nothing, a readiness notification
 * that nobody has taken yet, a goroutine that is on its way to parking, or the
 * goroutine that has parked. Every transition between those is a compare and
 * swap and there is no lock on the path a busy connection takes. */
typedef struct burrow__PollDesc {
    /* Set when the descriptor is taken out of the cache and not touched again
     * while it is in use. The backend needs it to unregister. */
    burrow__PollFd fd;

    /* Where this descriptor sits in the cache. Set when the slot is made and
     * never changed after that, since the slot never moves and is never given
     * back to the allocator. */
    uint32_t index;

    /* What the backend was told to hand back when this descriptor is ready.
     * The index above and the generation count below, packed into one word,
     * because a pointer sized word is all a kernel event carries. Constant
     * while the descriptor is open. */
    uintptr_t handle;

    /* Atomic. Which generation this slot is on. Raised every time the slot goes
     * back to the free list, which is what makes an event that arrives after a
     * close recognisably stale. */
    uint32_t gen;

    /* Atomic. The bits burrow__poll_wait has to look at before it parks, kept
     * out here rather than read under the lock because the read happens on
     * every wait and the writes happen when a connection is being closed. */
    uint32_t info;

    /* Atomic. The reader and the writer, one each. See the note above. */
    uintptr_t rg;
    uintptr_t wg;

    /* The rest is under the lock, and the lock is only ever taken by a caller
     * that is closing the descriptor or setting a deadline on it. Nothing on a
     * read or a write path comes here. */
    burrow__Lock mu;
    bool closing;

    /* The two deadlines, as burrow__nanotime readings. Zero is no deadline at
     * all, a positive value is one that has not arrived yet, and -1 is one that
     * has, which is the state every later wait in that direction answers
     * BURROW_POLL_TIMEOUT from until somebody sets a new one. Go's rd and wd,
     * with Go's three meanings for the sign. */
    int64_t rd;
    int64_t wd;

    /* The timers those deadlines are armed on, and whether each is armed.
     *
     * Two of them rather than one because the two deadlines move independently,
     * and only one of them is used when they happen to be equal, which is the
     * common case since SetDeadline sets both to the same instant. The timer
     * that covers both is the read one. */
    burrow__Timer rt;
    burrow__Timer wt;
    bool rt_armed;
    bool wt_armed;

    /* The free list in the cache, under the cache's own lock and not this one.
     * NULL while the descriptor is open. */
    struct burrow__PollDesc *link;
} burrow__PollDesc;

/* ------------------------------------------------------------ for a caller
 *
 * The four calls something like net.Conn is built out of.
 *
 * Takes a descriptor into the poller and answers a place to wait on it. The
 * answer is 0, or an errno saying why not.
 *
 * The descriptor has to be non-blocking already, and it has to stay open until
 * burrow__poll_close. What comes back is not tied to the file table entry in
 * any way the kernel knows about, so closing the descriptor without coming back
 * here leaves this pointing at a number that now means something else.
 *
 * EPERM from Linux is the answer worth expecting: it is what epoll says about a
 * regular file, which is a descriptor the kernel will not report readiness for
 * because a regular file is always ready in the only sense it has. A caller
 * that gets an error here has not done anything wrong, it has a descriptor that
 * has to be read on a thread instead. */
int burrow__poll_open(burrow__PollFd fd, burrow__PollDesc **out);

/* Takes it out again and gives the slot back.
 *
 * burrow__poll_unblock has to have been called first, which is Go's rule too.
 * Closing a descriptor that a goroutine is parked on without waking it first is
 * a goroutine that waits forever for a file that no longer exists, so this
 * refuses rather than allowing it.
 *
 * Does not close the descriptor. The caller opened it and the caller closes it,
 * and on every backend here the close itself is what removes the registration,
 * so the order of the two does not matter. */
void burrow__poll_close(burrow__PollDesc *pd);

/* Parks the calling goroutine until the descriptor is ready in `mode`, which is
 * BURROW_POLL_READ or BURROW_POLL_WRITE and not both.
 *
 * BURROW_POLL_READY means the kernel has said there is something to be had, and
 * also means try again, because between the notification and the read the data
 * can have been taken by somebody else. Anything else is a reason to stop.
 *
 * On a completion backend the same call parks the same way and the same four
 * answers come back, but BURROW_POLL_READY means the operation the caller
 * handed to the kernel has finished, and what it produced is in the
 * burrow__PollOp the caller submitted rather than waiting to be fetched. There
 * is nothing to try again. A caller that submits nothing and waits anyway waits
 * until its deadline, because on Windows nothing arrives unasked.
 *
 * One goroutine per descriptor per direction. Two goroutines reading one
 * connection is a program with a bug in it whatever this layer does, and rather
 * than pick a winner quietly, the second one stops the program.
 *
 * Callable only from a goroutine, since parking is the whole of what it does. */
burrow__PollStatus burrow__poll_wait(burrow__PollDesc *pd, uint32_t mode);

/* Sets, moves or clears a deadline. `mode` is BURROW_POLL_READ,
 * BURROW_POLL_WRITE, or both together, and this is the one call where both
 * together is the ordinary thing to ask for.
 *
 * `when` is a burrow__nanotime reading. Zero clears the deadline and leaves the
 * direction with no deadline at all. Anything already in the past, which
 * includes any negative number, expires at once: whoever is parked comes back
 * with BURROW_POLL_TIMEOUT and so does every wait after it, until a later
 * deadline or a zero is set. That last part is Go's rule and it is what makes a
 * timed out connection stay timed out rather than quietly working again.
 *
 * An expired deadline is not an error on the descriptor. The connection is
 * still open, the poller is still watching it, and moving the deadline forward
 * makes waits work again.
 *
 * Answers false only when arming the timer needed the P's heap to grow and the
 * allocator said no, in which case the deadline is recorded but will not fire
 * by itself. Go cannot fail here because Go's heap grows by panicking. The
 * deadline still applies to every wait that starts after it has passed, since
 * that check reads the clock rather than waiting for a timer.
 *
 * Callable only from a goroutine, because arming a timer needs a P. */
bool burrow__poll_set_deadline(burrow__PollDesc *pd, int64_t when, uint32_t mode);

/* Wakes whoever is parked on the descriptor and makes every later wait answer
 * BURROW_POLL_CLOSED at once.
 *
 * This is what a Close on a connection calls first. The goroutine blocked in a
 * read on it has to come back and find out that the connection went away, and
 * it has to do that before the descriptor number is handed back to the kernel
 * and handed out again to something else. */
void burrow__poll_unblock(burrow__PollDesc *pd);

#if defined(BURROW_NETPOLL_COMPLETION)

/* ------------------------------------------------------ submitting an operation
 *
 * Only on a completion backend, which today is Windows and IOCP. A caller
 * hands the kernel the read or the write itself, along with one of these, and
 * the kernel hands this back when the operation finishes. Everything the
 * poller needs to find its way from a completion to the parked goroutine is in
 * here, and so is what the operation produced.
 *
 * This is Go's internal/poll.operation, and it is in the runtime's header for
 * the same reason Go declares net_op in the runtime: the poller has to read the
 * fields, so the poller has to know the layout. What sits on top of this,
 * eventually net, embeds one of these in whatever else it wants to keep with it
 * and passes the address of the embedded one. */

/* An OVERLAPPED, laid out by hand so that this header does not have to drag
 * windows.h in behind it.
 *
 * The field names are lower case because these are burrow's names for
 * somebody else's structure, and nothing here reads or writes them: the kernel
 * fills them in. src/runtime/netpoll_completion.c checks at compile time that
 * this matches PalOverlapped, and src/pal/poll_windows.c checks that
 * PalOverlapped matches the real thing, so the two asserts together are what
 * says this is right. */
typedef struct burrow__PollOverlapped {
    uintptr_t internal;
    uintptr_t internal_high;
    uint32_t offset;
    uint32_t offset_high;
    void *event;
} burrow__PollOverlapped;

typedef struct burrow__PollOp {
    /* First, and it has to stay first: the kernel is given the address of this
     * structure as the address of an OVERLAPPED, and the poller casts what comes
     * back the other way. */
    burrow__PollOverlapped o;

    /* Which descriptor and which direction, filled in by burrow__poll_op_init
     * and not to be touched afterwards. The descriptor is the poller's own
     * handle for it rather than a pointer, so that a completion arriving for an
     * operation on a connection that has since been closed and whose slot has
     * been reused is dropped rather than believed. */
    uintptr_t desc;
    uint32_t mode;

    /* What the operation produced, written by the poller before the goroutine
     * is readied and worth reading once burrow__poll_wait has answered
     * BURROW_POLL_READY.
     *
     * `qty` is bytes transferred and is zero whenever `status` is not.
     *
     * `status` is the kernel's own status word for the operation, zero on
     * success, and it is an NTSTATUS rather than a winsock error number. That
     * is deliberate and it is the one place this structure stops short. The
     * poller has the status for free, because it arrives with the completion,
     * and it does not have the socket, because the socket belongs to whoever
     * submitted the operation. Winsock's numbering for the same failures is a
     * different set of numbers, so a layer that wants WSAECONNRESET rather than
     * STATUS_CONNECTION_RESET calls WSAGetOverlappedResult on the socket it is
     * holding, which is what Go's internal/poll does. Doing that here would
     * mean the poller keeping a copy of a socket it may be looking at after the
     * owner closed it, and one stale handle is worse than one conversion. */
    uint32_t qty;
    int32_t status;
} burrow__PollOp;

/* Points an operation at a descriptor and a direction, and clears the rest.
 *
 * Call this before handing the operation to the kernel, once per operation
 * rather than once per descriptor, since an operation is in flight from the
 * submission until the completion and two in flight at once in one direction is
 * the same mistake as two goroutines reading one connection.
 *
 * `mode` is BURROW_POLL_READ or BURROW_POLL_WRITE and not both, because a single
 * operation is one or the other.
 *
 * ------------------------------------------------------------- the one rule
 *
 * An operation has to stay alive until its completion has arrived. Not until
 * the read finishes, not until the goroutine stops waiting for it: until the
 * kernel has handed the thing back. A submitted operation is a pointer the
 * kernel is holding, and the kernel gives every submitted operation back
 * exactly once, whatever happened to it.
 *
 * Every way out of burrow__poll_wait other than BURROW_POLL_READY leaves an
 * operation still in flight. A deadline that passes, a descriptor that is
 * closed under a waiter, a connection that turns out to be unpollable: the
 * goroutine has its answer and the kernel still has the pointer. A caller that
 * returns at that point, with the operation on its stack, is leaving the kernel
 * to write into a frame that no longer exists, and it does not look like that
 * when it goes wrong. It looks like a completion for neither reading nor
 * writing, arriving on whichever thread was next in the poller.
 *
 * So there is a second half to a wait that did not come back ready, and it is
 * three steps. Cancel the operation with CancelIoEx, which is allowed to fail
 * with ERROR_NOT_FOUND and means the operation finished first. Call
 * burrow__poll_wait_canceled below, which waits for the completion and nothing
 * else. Only then let the operation go. Go does the same three steps in
 * internal/poll.execIO and for the same reason. */
void burrow__poll_op_init(burrow__PollOp *op, burrow__PollDesc *pd, uint32_t mode);

/* Waits for the completion of an operation that is being given up on.
 *
 * The second half of the rule above. It parks until the kernel hands the
 * operation back and it has no other way out: a closed descriptor does not end
 * it and neither does a deadline, because both of those are the state it is
 * called in. When it returns, the operation is the caller's again and the
 * kernel is not holding a pointer to it.
 *
 * `status` on the operation is usually STATUS_CANCELLED afterwards, and it is
 * worth looking at rather than assuming, because an operation can finish
 * normally in the moment between the wait giving up and the cancel landing. The
 * bytes in `qty` are real bytes that came off the connection when that happens,
 * and a caller that throws them away has lost data that the other side believes
 * it sent.
 *
 * `mode` is the direction the operation was submitted for. */
void burrow__poll_wait_canceled(burrow__PollDesc *pd, uint32_t mode);

#endif /* BURROW_NETPOLL_COMPLETION */

/* ---------------------------------------------------------- for the scheduler
 *
 * Whether the poller has been started. It starts on the first burrow__poll_open
 * in the process and stays started, so a program that never touches a
 * descriptor never makes a kernel object for this and the scheduler never calls in
 * here at all. */
bool burrow__netpoll_inited(void);

/* How many goroutines are parked on descriptors.
 *
 * The scheduler reads this to decide whether going to sleep inside the poller
 * is worth doing. Zero means every goroutine that is waiting for something is
 * waiting for something else, and the thread should sleep on its own note where
 * a wakeup costs less. */
uint32_t burrow__netpoll_waiters(void);

/* Asks the kernel which descriptors are ready and puts whoever was waiting for
 * them on `out`, which the caller then readies.
 *
 * A negative delay blocks until something happens, zero looks and comes
 * straight back, and a positive one blocks for that many nanoseconds. Those
 * three are the reason the scheduler needs nothing else: the same call is the
 * idle thread's sleep, the busy thread's glance on the way past, and the sleep
 * with a timer deadline on it.
 *
 * It can come back with nothing on `out` for any of the three, including the
 * blocking one, because a wakeup can be somebody calling burrow__netpoll_break
 * and because a signal can end the wait early. The caller goes round again. */
void burrow__netpoll(int64_t delay, burrow__GQueue *out);

/* Ends a blocking burrow__netpoll early.
 *
 * Whoever is asleep in there is the thread that would otherwise be asleep on a
 * note, so this is the poller's half of a note wake: it is what a goroutine
 * being readied by some other thread, or a timer coming due, has to call to get
 * that thread looking at the run queues again. Costs nothing when nobody is
 * inside the poller and nothing when a break is already on its way. */
void burrow__netpoll_break(void);

/* Forgets every goroutine parked on a descriptor, without waking any of them.
 *
 * The scheduler calls this on the way out of runtime_main, after every thread
 * has stopped and before the goroutines are freed. A program that reaches the
 * end of main with a connection still being read is a program with a goroutine
 * parked here forever, which is allowed and is what a leaked connection looks
 * like in Go too. What is not allowed is leaving a pointer to a freed goroutine
 * in a descriptor that the kernel can still produce an event for, and this is
 * what cuts that pointer. */
void burrow__netpoll_drop_waiters(void);

/* ------------------------------------------------------------- for a backend
 *
 * The five calls a backend supplies and the one it makes. Everything above is
 * written once against these, which is what keeps the per platform files down
 * to the shape of their own system call.
 *
 * Starts the poller. Called once, under the lock in burrow__poll_open, and it
 * stops the program if the kernel will not give it what it asks for, which is
 * what Go does: a machine that cannot make an epoll descriptor cannot run a
 * program that needs one, and pretending otherwise puts the failure somewhere
 * further away from the cause. */
void burrow__netpoll_backend_init(void);

/* Registers `fd` for reading and writing, edge triggered, and asks for `handle`
 * back with every event about it. An errno, or 0.
 *
 * A handle is one pointer sized word, because that is what every one of these
 * kernel interfaces carries, and it is never a word of all ones. A backend that
 * needs a value of its own to tell its own wakeup apart from a connection has
 * that one.
 *
 * A completion backend has nothing to arm, since the events it reports are the
 * operations the caller submits rather than anything about the descriptor
 * itself. It attaches the descriptor to whatever it waits on and ignores the
 * handle, which reaches it in the operation instead. */
int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle);

/* Unregisters it. An errno, or 0. All three backends here treat closing the
 * descriptor as unregistering it, so this is allowed to be, and on kqueue and
 * IOCP is, nothing at all. */
int burrow__netpoll_backend_close(burrow__PollFd fd);

/* The wait itself. Reports every ready descriptor by calling
 * burrow__netpoll_ready below, which is where the generic half takes over. */
void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out);

/* The wakeup. Whatever the backend's own way of interrupting its wait is. */
void burrow__netpoll_backend_break(void);

/* What a backend calls for each ready descriptor.
 *
 * `handle` is the one the backend was given in burrow__netpoll_backend_open and
 * has been carrying since. `mode` is BURROW_POLL_READ, BURROW_POLL_WRITE or
 * both. `failed` says the event was an error rather than readiness, which is
 * reported to the reader and not to the writer, because a write that goes on to
 * fail can say what went wrong and a read that has nothing to read cannot.
 *
 * A handle from a use of the slot that has since ended is dropped here, so a
 * backend does not have to know that there is such a thing. */
void burrow__netpoll_ready(burrow__GQueue *out, uintptr_t handle, uint32_t mode,
                           bool failed);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NETPOLL_H */
