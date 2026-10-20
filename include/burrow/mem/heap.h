/* malloc and free, behind the allocator interface.
 *
 * The least interesting backend and the one people reach for first, which is
 * fine. It is the right choice for a long lived cache with per item eviction,
 * which is exactly the workload an arena is bad at, and it is the right choice
 * when you are handing memory to something outside burrow that is going to call
 * free on it.
 *
 * It is a singleton because malloc is. There is no state to carry and nothing
 * to initialise.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_HEAP_H
#define BURROW_MEM_HEAP_H

#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The process allocator. Never NULL, never needs initialising, and the same
 * pointer every time, so comparing against it is a fair way to ask whether some
 * Alloc you were handed is the heap.
 *
 * It does not report statistics, because there is no portable way to ask the C
 * library for them and counting in the wrapper would make the common path pay
 * for something almost nobody reads. Wrap it in the tracking allocator when you
 * want numbers. */
BURROW_STATIC(ret) Alloc *heap_allocator(void);

/* For testing's allocs/op and B/op. Counting is off until a benchmark run turns
 * it on, and the counts only ever go up, so a benchmark reads them before and
 * after and subtracts. A realloc that grows counts as one allocation. */
void burrow__heap_count(bool on);
void burrow__heap_counts(uint64_t *allocs, uint64_t *bytes);

/* heap_allocator without the counting above and without the heap profile's
 * sampling, for the profiler's own tables, which must not be sampled while
 * they are being written. */
BURROW_STATIC(ret) Alloc *burrow__heap_unprofiled(void);

/* The heap profile's slow path, where an allocation that is due to be sampled
 * goes. Internal to heap.c, and declared here only because it is not static,
 * so that the profile's stacks can tell it by name. */
void burrow__heap_sample(void *p, size_t size);

/* The rest of the way down from mem_alloc to burrow__heap_sample: heap_allocator's
 * alloc, alloc_zeroed and realloc, and the check every allocation makes for
 * whether it is due to be sampled. Internal to heap.c and global for the same
 * reason. */
void burrow__heap_note(void *p, size_t size);
BURROW_OWNS(ret) void *burrow__heap_alloc(void *self, size_t size, size_t align);
BURROW_OWNS(ret) void *burrow__heap_alloc_zeroed(void *self, size_t size, size_t align);
BURROW_OWNS(ret) void *burrow__heap_realloc(void *self, void *p, size_t old, size_t nsz,
                                            size_t align);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_HEAP_H */
