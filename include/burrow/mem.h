/* The allocator interface, which is the one thing you have to understand
 * before anything else in burrow makes sense.
 *
 * Every function in this library that can allocate takes an Alloc * as its
 * first parameter. There are no exceptions, there is no hidden global, and
 * there is no per-object free function to remember. Everything a call
 * allocated belongs to the allocator you handed it, and you get rid of all of
 * it at once.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *
 *     ... call things, pass a down, do not think about freeing ...
 *
 *     arena_free(&ar);
 *
 * Go's API hands back heap objects everywhere because a collector cleans up
 * afterwards. Bolting a tracing collector onto a C library would force a
 * runtime on every consumer and break every FFI host, and writing a matching
 * _free for 23,730 declarations means documenting ownership 23,730 times and
 * being wrong some of those times. One rule that covers the whole surface beats
 * both, and this is Zig's rule because Zig got there first.
 *
 * Naming note, since this is the first header that raises it. Ported symbols
 * carry Go's package name as their first segment and no library prefix, which
 * is why you will read strings_contains rather than burrow_strings_contains.
 * burrow's own additions need a namespace too, so the ones that are part of the
 * API you use get a segment that reads like a package would, which is mem and
 * arena here, and the ones that are about burrow itself rather than about your
 * program get burrow_, which is burrow_version and burrow_license and nothing
 * else so far.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_H
#define BURROW_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What an allocator reports about itself. Every field is cumulative except
 * bytes_live, and an allocator that does not track a particular number leaves
 * it at zero rather than guessing. */
typedef struct AllocStats {
    uint64_t bytes_live;  /* handed out and not yet given back */
    uint64_t bytes_peak;  /* the highest bytes_live has ever been */
    uint64_t bytes_total; /* every byte ever handed out, cumulative */
    uint64_t allocs;
    uint64_t frees;
    uint64_t blocks; /* backend specific: arena chunks, zero for heap */
} AllocStats;

typedef struct AllocVT AllocVT;

/* Deliberately the same shape as an interface value, a vtable and a receiver,
 * so that writing your own allocator needs no machinery this library has to
 * know about. Fill in a vtable, point self at your state, pass it in. */
typedef struct Alloc {
    const AllocVT *vt;
    void *self;
} Alloc;

struct AllocVT {
    /* Sizes are bytes and align is a power of two. Returns NULL on failure,
     * and callers in this library are required to check, which is enforced by
     * a checker rather than by discipline. */
    void *(*alloc)(void *self, size_t size, size_t align);

    /* Optional, and the reason it exists is that Go zeroes everything it
     * allocates and so do we. An arena handing out memory it has never touched
     * already knows it is zero, and malloc has calloc, so a backend that can
     * skip the memset says so here. NULL means mem_alloc does the memset. */
    void *(*alloc_zeroed)(void *self, size_t size, size_t align);

    /* old is what the caller asked for last time, which is what lets an arena
     * grow the most recent allocation in place and what lets the tracking
     * allocator notice a mismatch. */
    void *(*realloc)(void *self, void *p, size_t old, size_t nsz, size_t align);

    /* Also takes the size and alignment, for the same two reasons. */
    void (*free)(void *self, void *p, size_t size, size_t align);

    /* Optional. NULL means this allocator cannot be reset. */
    void (*reset)(void *self);

    /* Optional. NULL means no statistics, and mem_stats returns zeroes. */
    AllocStats (*stats)(void *self);
};

/* Zeroed, because Go's zero value rule is not a convention there, it is the
 * language, and code ported from Go assumes it everywhere. */
void *mem_alloc(Alloc *a, size_t size, size_t align);

/* Not zeroed. Only worth reaching for when the next thing you do is overwrite
 * every byte, which in this library means the copy loops and nothing else. */
void *mem_alloc_nozero(Alloc *a, size_t size, size_t align);

/* n * size with the multiplication checked, because an attacker controlled
 * element count that wraps is the oldest heap overflow there is. Returns NULL
 * on overflow rather than allocating something too small. */
void *mem_alloc_array(Alloc *a, size_t n, size_t size, size_t align);

void *mem_realloc(Alloc *a, void *p, size_t old, size_t nsz, size_t align);
void mem_free(Alloc *a, void *p, size_t size, size_t align);

/* Gives everything back at once. Safe to call on an allocator that does not
 * support it, where it does nothing, so that generic code does not have to
 * ask first. Ask with mem_can_reset if you need to know. */
void mem_reset(Alloc *a);
bool mem_can_reset(Alloc *a);

AllocStats mem_stats(Alloc *a);

/* The obvious two, which is what most call sites want. BURROW_NEW zeroes,
 * matching Go's new(T). */
#define BURROW_NEW(a, T) ((T *)mem_alloc((a), sizeof(T), _Alignof(T)))
#define BURROW_NEW_N(a, T, n) ((T *)mem_alloc_array((a), (n), sizeof(T), _Alignof(T)))

/* Ownership annotations.
 *
 * Even with one allocator convention, one question is left per function: does
 * the Str or Slice coming back alias the input, or is it fresh? Go's collector
 * makes that invisible. In C it decides whether the input can be freed, so
 * every declaration says which it is, in a form three different tools read.
 *
 *     BURROW_OWNS(ret)        Str strings_to_upper(Alloc *a, Str s);
 *     BURROW_BORROWS(ret, s)  Str strings_trim_space(Str s);
 *
 * They expand to nothing. The documentation generator turns them into the
 * lifetime sentence on every reference page, so nobody writes those by hand and
 * nobody gets them wrong in prose. The conformance harness generates an
 * AddressSanitizer test per annotated function which frees the input and
 * touches the output and requires a report exactly when BURROW_BORROWS says the
 * two alias, so a wrong annotation is a failing test rather than a comment
 * somebody will believe. And a clang plugin reads them for people who want the
 * check in their own code. */
#define BURROW_OWNS(...)
#define BURROW_BORROWS(...)
#define BURROW_RETAINS(...)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_H */
