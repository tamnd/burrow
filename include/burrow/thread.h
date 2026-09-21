/* The OS threads burrow's scheduler runs on.
 *
 * An M in Go's terminology is an OS thread, and this is the layer that starts
 * one. It is the smallest thing that can be called a thread abstraction: start,
 * join, detach, yield, who am I, and how many processors are there. That is
 * everything the scheduler needs from the operating system and nothing else,
 * because every function added here is one more thing that has to be right on
 * six systems rather than on the one in front of whoever added it.
 *
 * There is no mutex and no condition variable here, on purpose. A goroutine
 * that blocks has to park the goroutine and free the thread, so a pthread mutex
 * is the wrong tool at every level above this one, and the one place the
 * runtime really does have to put a thread to sleep gets a dedicated primitive
 * built on futexes rather than a general purpose lock. That primitive is the
 * next piece and it is not this file.
 *
 *     static void worker(void *arg) { ... }
 *
 *     burrow__Thread t;
 *     if (!burrow__thread_start(&t, worker, &state, 0))
 *         return false;
 *     burrow__thread_join(&t);
 *
 * The handle carries the function and the argument, which is how the thread
 * gets both of them through an interface that has room for one pointer. So the
 * handle has to outlive the thread. A handle on the stack of a function that
 * returns before the thread does is a use after free, and the same rule the
 * whole library follows applies here: the caller owns the memory and the
 * library never allocates behind the caller's back.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_THREAD_H
#define BURROW_THREAD_H

#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(BURROW_OS_WINDOWS)
/* Everywhere except Windows this is pthreads, and pthread_t has to be a real
 * type here rather than a blob of bytes, because a blob would have to guess a
 * size and an alignment for a type the standard deliberately leaves opaque. */
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What a thread runs.
 *
 * No return value, because the only thing above this is the scheduler and a
 * scheduler thread that returns a value has nobody to give it to. A thread that
 * wants to report something writes it where the starter can see it. */
typedef void (*burrow__ThreadFn)(void *arg);

/* A started thread, and the argument it was started with.
 *
 * The fields are here rather than behind an opaque pointer so that a handle can
 * live on a stack or inside an M without an allocation. They are not part of
 * the interface: read them and the next platform added here breaks you. */
typedef struct burrow__Thread {
    burrow__ThreadFn fn;
    void *arg;
#if defined(BURROW_OS_WINDOWS)
    /* HANDLE, spelled void * so that <windows.h> is not dragged into every file
     * that includes this one. It is a void * in the Windows headers too. */
    void *handle;
    unsigned long id;
#else
    pthread_t id;
#endif
    bool started;
} burrow__Thread;

/* Starts a thread running fn(arg) and returns whether it started.
 *
 * stack_bytes of 0 asks for the platform's default, which is eight megabytes of
 * reserved address space on Linux and one on Windows, and is the right answer
 * for anything that is not counting threads in the thousands. A size below the
 * platform's minimum is raised to the minimum rather than rejected, because the
 * minimum is different on every system and a caller asking for 16 kilobytes
 * means "small" rather than "exactly this". It is 16 kilobytes on macOS and 128
 * on glibc arm64, so that is not a small difference, and the size is also
 * rounded up to a whole page because macOS refuses one that is not.
 *
 * False means the system said no, which in practice is a thread limit or an
 * address space that has no room for another stack. It is not a panic, because
 * the scheduler can run on the threads it already has and a library that ends
 * the process on a resource limit is a library nobody can build a server on. */
bool burrow__thread_start(burrow__Thread *t, burrow__ThreadFn fn, void *arg,
                          size_t stack_bytes);

/* Waits for the thread to return and releases what the system was holding for
 * it. False means the handle was never started or was already joined, which is
 * a bug in the caller rather than a condition to handle.
 *
 * Every started thread has to be joined or detached exactly once. A thread that
 * is neither leaks a handle on Windows and a stack on Linux. */
bool burrow__thread_join(burrow__Thread *t);

/* Says the thread will never be joined, so the system can release what it was
 * holding as soon as the thread returns. The handle is dead afterwards, but the
 * running thread still reads fn and arg out of it, so the memory has to stay
 * valid until the thread is finished with it. */
bool burrow__thread_detach(burrow__Thread *t);

/* An identity for the calling thread, stable for as long as that thread runs
 * and different from every other running thread's.
 *
 * It is not the thread id a debugger shows and it is not an index into
 * anything. It is what a thread compares against to answer "is this me", which
 * is what LockOSThread and the panic printer need. The system may hand the same
 * value to a new thread once the old one is gone. */
uint64_t burrow__thread_self(void);

/* Gives up the rest of this thread's time slice. This is the last thing a spin
 * loop does before it gives up and parks, and on a machine with one processor
 * it is the only way the thread being waited on gets to run. */
void burrow__thread_yield(void);

/* Where the calling thread's own stack begins and ends.
 *
 * lo is the lowest address on it and hi is one past the highest, which is the
 * same way burrow__Stack describes a stack the library mapped itself. The
 * difference is whose stack it is: this one is the one the operating system
 * gave the thread, so it is the answer for the main thread and for any thread
 * that is not currently running a goroutine.
 *
 * The stack walker is the only caller and the bounds are what keep it safe. A
 * walk follows saved frame pointers, one of which is eventually a value that
 * was never a frame pointer, and the only thing standing between that and a
 * fault is knowing which addresses are stack.
 *
 * False means the system has no way to ask, which is the honest answer on a
 * platform nobody has written this for rather than a failure. A caller that
 * gets false does less rather than guesses: the walker returns no frames.
 *
 * Only ever call it for the thread you are on. Every system underneath offers
 * this for the current thread and several of them offer nothing else, and a
 * caller asking about another thread is asking about a stack that is moving. */
bool burrow__thread_stack_bounds(void **lo, void **hi);

/* How many processors there are, at least 1, never 0.
 *
 * This is the number of processors that exist, which is not always the number
 * this process is allowed to use. A cpuset or a container cpu limit makes those
 * two different, and Go gets that right through affinity and cgroups. That
 * belongs with GOMAXPROCS, which decides how many Ps to make and is where a
 * caller can override it anyway, so it is not decided here. */
int burrow__thread_ncpu(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_THREAD_H */
