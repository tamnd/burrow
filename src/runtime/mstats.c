/* runtime.ReadMemStats, over the numbers burrow's allocators keep.
 *
 * Not derived from Go's source, apart from the shape of the struct, which is
 * Go's src/runtime/mstats.go. Go reads its own heap. There is no such heap
 * here, so this reads the two places that do count: heap_allocator's counters
 * and the collector, when one is running.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mstats.h"

#include "burrow/mem/gc.h"
#include "burrow/mem/heap.h"

#include <string.h>

BURROW_STRUCT_AS_DEFINE(RuntimeMemStatsBySize, RUNTIME_MEM_STATS_BY_SIZE_FIELDS);
BURROW_ARRAY_TYPE_DEFINE(RuntimeMemStatsPauses, uint64_t, 256);
BURROW_ARRAY_TYPE_DEFINE(RuntimeMemStatsBySizes, RuntimeMemStatsBySize, 61);
BURROW_STRUCT_AS_DEFINE(RuntimeMemStats, RUNTIME_MEM_STATS_FIELDS);

void runtime_read_mem_stats(RuntimeMemStats *m) {
    memset(m, 0, sizeof *m);

    uint64_t allocs = 0, bytes = 0;
    burrow__heap_counts(&allocs, &bytes);
    m->mallocs = allocs;
    m->total_alloc = bytes;

    /* gc_allocator would start the collector, which is the host's decision and
     * has to happen on the main thread, so ask first. */
    if (burrow__gc_running()) {
        AllocStats st = mem_stats(gc_allocator());
        m->alloc = st.bytes_live;
        m->heap_alloc = st.bytes_live;
        m->heap_inuse = st.bytes_live;
        m->total_alloc += st.bytes_total;
        m->num_gc = (uint32_t)st.blocks;
        m->enable_gc = true;
    }
}
