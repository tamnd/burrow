/* runtime.MemStats and runtime.ReadMemStats.
 *
 * The struct has Go's shape, field for field and under Go's names in its
 * descriptor, so that something which marshals it, such as expvar's memstats
 * variable, writes the JSON a tool reading Go's output expects. What goes in
 * the fields is a different matter, because there is no Go heap here to
 * describe. A field is filled when burrow knows a true number for it and is
 * zero otherwise:
 *
 *     RuntimeMemStats m;
 *     runtime_read_mem_stats(&m);
 *     fmt_printf_v("%d collections, %d bytes in use\n", m.num_gc, m.heap_alloc);
 *
 * Mallocs and TotalAlloc count what went through heap_allocator, and only
 * while that counting is on, which is while testing is measuring a benchmark.
 * When the collector from burrow/mem/gc.h is running, Alloc, HeapAlloc and
 * HeapInuse are the bytes in use in its heap, TotalAlloc adds what it has handed
 * out, NumGC is its collection count and EnableGC is true.
 * Without it there is no collector, so NumGC is zero and EnableGC is false.
 * Arenas and the other allocators keep their own numbers, which mem_stats
 * reports for each one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MSTATS_H
#define BURROW_MSTATS_H

#include "burrow/declare.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One element of BySize, which Go declares inline as an anonymous struct. */
#define RUNTIME_MEM_STATS_BY_SIZE_FIELDS(F, T)                                         \
    F(T, uint32_t, size, Size, "")                                                     \
    F(T, uint64_t, mallocs, Mallocs, "")                                               \
    F(T, uint64_t, frees, Frees, "")

BURROW_STRUCT_AS_DECL(RuntimeMemStatsBySize, RUNTIME_MEM_STATS_BY_SIZE_FIELDS);

BURROW_ARRAY_TYPE_DECL(RuntimeMemStatsPauses, uint64_t, 256);
BURROW_ARRAY_TYPE_DECL(RuntimeMemStatsBySizes, RuntimeMemStatsBySize, 61);

#define RUNTIME_MEM_STATS_FIELDS(F, T)                                                 \
    F(T, uint64_t, alloc, Alloc, "")                                                   \
    F(T, uint64_t, total_alloc, TotalAlloc, "")                                        \
    F(T, uint64_t, sys, Sys, "")                                                       \
    F(T, uint64_t, lookups, Lookups, "")                                               \
    F(T, uint64_t, mallocs, Mallocs, "")                                               \
    F(T, uint64_t, frees, Frees, "")                                                   \
    F(T, uint64_t, heap_alloc, HeapAlloc, "")                                          \
    F(T, uint64_t, heap_sys, HeapSys, "")                                              \
    F(T, uint64_t, heap_idle, HeapIdle, "")                                            \
    F(T, uint64_t, heap_inuse, HeapInuse, "")                                          \
    F(T, uint64_t, heap_released, HeapReleased, "")                                    \
    F(T, uint64_t, heap_objects, HeapObjects, "")                                      \
    F(T, uint64_t, stack_inuse, StackInuse, "")                                        \
    F(T, uint64_t, stack_sys, StackSys, "")                                            \
    F(T, uint64_t, mspan_inuse, MSpanInuse, "")                                        \
    F(T, uint64_t, mspan_sys, MSpanSys, "")                                            \
    F(T, uint64_t, mcache_inuse, MCacheInuse, "")                                      \
    F(T, uint64_t, mcache_sys, MCacheSys, "")                                          \
    F(T, uint64_t, buck_hash_sys, BuckHashSys, "")                                     \
    F(T, uint64_t, gc_sys, GCSys, "")                                                  \
    F(T, uint64_t, other_sys, OtherSys, "")                                            \
    F(T, uint64_t, next_gc, NextGC, "")                                                \
    F(T, uint64_t, last_gc, LastGC, "")                                                \
    F(T, uint64_t, pause_total_ns, PauseTotalNs, "")                                   \
    F(T, RuntimeMemStatsPauses, pause_ns, PauseNs, "")                                 \
    F(T, RuntimeMemStatsPauses, pause_end, PauseEnd, "")                               \
    F(T, uint32_t, num_gc, NumGC, "")                                                  \
    F(T, uint32_t, num_forced_gc, NumForcedGC, "")                                     \
    F(T, double, gc_cpu_fraction, GCCPUFraction, "")                                   \
    F(T, bool, enable_gc, EnableGC, "")                                                \
    F(T, bool, debug_gc, DebugGC, "")                                                  \
    F(T, RuntimeMemStatsBySizes, by_size, BySize, "")

/* runtime.MemStats. The fields are Go's, in Go's order, with C names. */
BURROW_STRUCT_AS_DECL(RuntimeMemStats, RUNTIME_MEM_STATS_FIELDS);

#define TYPE_RUNTIME_MEM_STATS TYPE_OF(RuntimeMemStats)

/* runtime.ReadMemStats. Fills *m, all of it, with what is described at the top
 * of this file. Unlike Go's, it does not stop the world. */
void runtime_read_mem_stats(RuntimeMemStats *m);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MSTATS_H */
