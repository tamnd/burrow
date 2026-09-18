/* A collector behind the allocator interface, for people who want Go's
 * ergonomics back.
 *
 * Every other backend asks you to decide when memory goes away. This one does
 * not. You get an Alloc like any other, you pass it down like any other, and you
 * never free anything, which is what writing Go feels like and is the whole
 * reason this file exists. Scripts, prototypes and programs that burrow owns end
 * to end are what it is for.
 *
 *     Alloc *a = gc_allocator();
 *     if (a == NULL)
 *         ... built without the collector, see below ...
 *     Slice parts = strings_split(a, line, S(","));
 *     ... never free anything ...
 *
 * It is the Boehm collector underneath and it is off unless you ask for it, with
 * make BOEHM=1 or with cmake -DBURROW_ENABLE_BOEHM=ON, either of which links
 * -lgc. Without that flag every function here still exists and still links,
 * gc_available answers false, and gc_allocator hands back NULL.
 *
 * NULL rather than quietly falling back to the heap, because a fallback would
 * be a program that allocates in a loop, frees nothing, believes a collector is
 * cleaning up after it, and grows until the machine stops. Ask gc_available once
 * at startup and pick the allocator you want.
 *
 * burrow's own code never uses this and the test suite never needs it, so
 * nothing in the library depends on Boehm being present or portable to wherever
 * you are building. A collector has to see the real stack and has to be started
 * by the host, which is fine in a program you own and is not something a library
 * gets to impose on everybody who embeds it.
 *
 * Two things to know before you turn it on. Call gc_allocator once from the main
 * thread before you start any others, since the collector is started by the
 * thread whose stack it will scan. And if your program has threads, Boehm has to
 * have been built with thread support and told about each one, which is its
 * documentation rather than ours.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_GC_H
#define BURROW_MEM_GC_H

#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this build has the collector in it. Cheap, constant, and safe to call
 * before anything else has happened. */
bool gc_available(void);

/* The collecting allocator, or NULL if gc_available is false.
 *
 * One shared object, like the heap, because there is one collector in a process
 * and no state for a second one to carry. The first call starts the collector,
 * which is why it wants to be the main thread and wants to be early.
 *
 * Freeing through it does nothing at all. mem_free is not an error and not a
 * no-op by accident, it is the collector's answer: memory goes away when nothing
 * can reach it and not when you say so. Resetting is not supported either, since
 * there is no point at which everything is known to be dead.
 *
 * Statistics come from the collector and mean what the collector means by them,
 * which is not quite what the other backends report. See mem_stats and the
 * guide. */
BURROW_STATIC(ret) Alloc *gc_allocator(void);

/* Collect now. This is runtime.GC, and like runtime.GC it is almost always the
 * wrong thing to call: the collector's own pacing beats a guess from the outside
 * except when you know something it cannot, such as having just dropped the last
 * reference to something enormous right before a latency sensitive stretch.
 *
 * Does nothing in a build without the collector. */
void gc_collect(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_GC_H */
