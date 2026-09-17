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
Alloc *heap_allocator(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_HEAP_H */
