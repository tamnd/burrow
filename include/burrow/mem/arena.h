/* The default allocator, and the one to reach for unless you have a reason not
 * to.
 *
 * An arena hands out memory by moving a pointer forward through a large chunk
 * and gives all of it back at once. Allocation is a bounds check and an add.
 * There is no per object bookkeeping, so there is no per object cost and no way
 * to leak one object, and there is no fragmentation because nothing is ever
 * returned to a pool for reuse at a different size.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *
 *     Str body = http_get(a, url);
 *     Json *doc = json_parse(a, body);
 *
 *     arena_free(&ar);
 *
 * That shape fits most of what people write. A request handler, a parse, a
 * build step and a test all have a point where everything the work produced
 * stops being interesting at the same moment, and that moment is the reset.
 *
 * Where it does not fit is a long lived structure with individual eviction, a
 * cache being the obvious one, because nothing comes back until the whole arena
 * does. Use heap_allocator for those.
 *
 * An Arena is not thread safe and is not meant to be. Give each goroutine its
 * own, which costs one chunk, or put a lock in front of one if you really want
 * sharing. Making the fast path atomic would make every single threaded program
 * pay for something most of them do not do.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_ARENA_H
#define BURROW_MEM_ARENA_H

#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArenaChunk ArenaChunk;

/* The fields are here because C needs the size to put one on the stack, which
 * is the point. Do not read or write them. */
typedef struct Arena {
    Alloc alloc;
    Alloc *parent;
    ArenaChunk *live;  /* chunks in use, newest first */
    ArenaChunk *spare; /* chunks a reset kept, waiting to be used again */
    size_t chunk_size; /* what the next ordinary chunk will ask for */
    uint64_t bytes_live;
    uint64_t bytes_peak;
    uint64_t bytes_total;
    uint64_t allocs;
    uint64_t frees;
    uint64_t blocks;
} Arena;

/* parent is where chunks come from. NULL means heap_allocator, which is what
 * you want almost always. Passing another arena's allocator nests them, so a
 * request arena can live inside a connection arena and be discarded without
 * touching the connection.
 *
 * chunk_size is a hint for how much to ask the parent for at a time. Zero
 * means a default that suits ordinary work. Allocations larger than a chunk get
 * a chunk of their own, so a big one is not a failure, just a chunk you did not
 * plan for.
 *
 * Takes no memory from the parent yet. An arena that is never used costs
 * nothing but the struct. */
void arena_init(Arena *ar, Alloc *parent, size_t chunk_size);

/* The allocator to pass to everything else. Valid until arena_free, and tied to
 * this exact Arena, so do not copy the Arena after calling this. */
Alloc *arena_allocator(Arena *ar);

/* Everything allocated becomes invalid, and the chunks are kept for reuse, so
 * the next round of work does no allocation from the parent at all. This is the
 * call that makes a per request arena cheap.
 *
 * Reachable through mem_reset too, for code that only has the Alloc. */
void arena_reset(Arena *ar);

/* Everything allocated becomes invalid and every chunk goes back to the parent.
 * Safe on an arena that was initialised and never used, and safe to call twice,
 * which leaves the Arena usable again as if freshly initialised. */
void arena_free(Arena *ar);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_ARENA_H */
