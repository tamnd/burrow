/* The arenas errors are made in, which burrow/error.h describes from the
 * outside.
 *
 * Not derived from Go's source, because Go has a garbage collector and no need
 * for any of this. docs/design/05-memory.md has the reasoning, under the first
 * of its carve-outs.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/error.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/platform.h"
#include "burrow/sched.h"

#include <stdbool.h>
#include <stddef.h>

/* Errors are small, a struct and a short message, and most goroutines make
 * none or one. A kilobyte holds a dozen of them and costs nothing to hand
 * back. The arena doubles from there if a goroutine turns out to fail a lot. */
#define ERROR_CHUNK ((size_t)1024)

/* The arena for a thread that is not running a goroutine, which is every
 * thread in a program that never starts the runtime. Thread local, for the
 * reason the panic state is: only the thread inside can reach it. It lives
 * until the process ends, since there is no hook for a thread ending that
 * works on every platform, and a thread that makes errors in a loop should
 * release a mark in the loop. */
static BURROW_THREAD_LOCAL Arena thread_errors;
static BURROW_THREAD_LOCAL bool thread_errors_ready;

static Arena *error_arena(void) {
    burrow__G *g = burrow__curg();

    if (g == NULL) {
        if (!thread_errors_ready) {
            arena_init(&thread_errors, NULL, ERROR_CHUNK);
            thread_errors_ready = true;
        }
        return &thread_errors;
    }

    if (g->errors == NULL) {
        /* If even the struct cannot be had, the thread's arena stands in. That
         * is the wrong lifetime, longer rather than shorter, which is the safe
         * direction to be wrong in. */
        Arena *ar =
            (Arena *)mem_alloc(heap_allocator(), sizeof(Arena), _Alignof(Arena));
        if (ar == NULL) {
            if (!thread_errors_ready) {
                arena_init(&thread_errors, NULL, ERROR_CHUNK);
                thread_errors_ready = true;
            }
            return &thread_errors;
        }
        arena_init(ar, NULL, ERROR_CHUNK);
        g->errors = ar;
    }
    return g->errors;
}

Alloc *error_allocator(void) {
    return arena_allocator(error_arena());
}

ArenaMark error_mark(void) {
    return arena_mark(error_arena());
}

void error_release(ArenaMark m) {
    arena_release(error_arena(), m);
}
