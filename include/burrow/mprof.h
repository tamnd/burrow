/* runtime.MemProfileRate and runtime.MemProfile: the heap profile.
 *
 * What is profiled is heap_allocator, the process allocator, which is where a
 * burrow program's memory comes from unless it chose an arena or the
 * collector. One allocation in every so many bytes is picked at random, and the
 * stack it was made from is remembered, along with whether it has been freed
 * yet:
 *
 *     runtime_set_mem_profile_rate(1);
 *     ... allocate ...
 *     RuntimeMemProfileRecord r[64];
 *     bool ok;
 *     Int n = runtime_mem_profile(r, 64, false, &ok);
 *
 * Most programs want pprof's "heap" and "allocs" profiles instead, which are
 * this written out in a form the pprof tool reads.
 *
 * Go learns of a free when its collector sweeps the object, a cycle or two
 * late, and so publishes a profile as of the last collection. Here a free is
 * a call to mem_free, which is counted as it happens, so the profile is always
 * as of now. Memory handed to the C library's free or realloc directly is not
 * seen going, and stays in use in the profile.
 *
 * Derived from Go's src/runtime/mprof.go and src/runtime/malloc.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MPROF_H
#define BURROW_MPROF_H

#include "burrow/core.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Go's runtime.MemProfileRate: the profiler aims to sample an average of one
 * allocation per this many bytes allocated. 1 records every allocation and 0
 * none. The default is 512 * 1024.
 *
 * The tools that read a profile take the rate to have been the same for the
 * whole life of the program, so a program that changes it should do it once,
 * as early as it can, as Go asks too. */
Int runtime_mem_profile_rate(void);
void runtime_set_mem_profile_rate(Int rate);

/* Go's runtime.MemProfileRecord: the objects allocated from one stack. */
typedef struct RuntimeMemProfileRecord {
    int64_t alloc_bytes, free_bytes;     /* number of bytes allocated, freed */
    int64_t alloc_objects, free_objects; /* number of objects allocated, freed */
    Uintptr stack0[32]; /* stack trace for this record; ends at first 0 entry */
} RuntimeMemProfileRecord;

/* InUseBytes returns the number of bytes in use (AllocBytes - FreeBytes). */
int64_t runtime_mem_profile_record_in_use_bytes(const RuntimeMemProfileRecord *r);

/* InUseObjects returns the number of objects in use (AllocObjects -
 * FreeObjects). */
int64_t runtime_mem_profile_record_in_use_objects(const RuntimeMemProfileRecord *r);

/* Stack returns the stack trace associated with the record, a prefix of
 * r.Stack0. Here that is how many of stack0 are in it. */
Int runtime_mem_profile_record_stack(const RuntimeMemProfileRecord *r);

/* Go's runtime.MemProfile: the number of records in the profile, one per stack
 * and size that allocated. When len is at least that, the records are copied
 * into p and *ok is true. Otherwise p is left alone and *ok is false. ok may be
 * NULL.
 *
 * With inuse_zero true the records include stacks all of whose memory has been
 * freed again. The stacks start with the allocator's own frames, which pprof
 * leaves out when it prints them. */
Int runtime_mem_profile(RuntimeMemProfileRecord *p, Int len, bool inuse_zero, bool *ok);

/* Go's profilerecord.MemProfileRecord, for runtime/pprof. stack lives as long
 * as the program does. */
typedef struct burrow__MemProfileRecord {
    int64_t object_size, alloc_objects, free_objects;
    BURROW_BORROWS(1) const Uintptr *stack;
    Int nstack;
} burrow__MemProfileRecord;

typedef void (*burrow__MemProfileFn)(void *env, const burrow__MemProfileRecord *r);

/* Go's memProfileInternal: the number of records, and when that is no more than
 * size, a call to fn for each of them and *ok set to true. fn is called with
 * the profile's lock held, so it must not allocate from or free to the heap. */
Int burrow__mem_profile(Int size, bool inuse_zero, burrow__MemProfileFn fn, void *env,
                        bool *ok);

/* For heap.c. Go's nextSample: how many bytes to allocate before the next
 * sample at rate. */
int64_t burrow__mprof_next_sample(Int rate);

/* For heap.c. Go's mProf_Malloc: record the allocation of p, size bytes, made
 * from the stack this is called on. */
void burrow__mprof_malloc(void *p, size_t size);

/* For heap.c. Whether p might be a sampled allocation, which is false for
 * nearly every pointer and is answered without a lock. */
bool burrow__mprof_maybe_sampled(const void *p);

/* For heap.c. Go's mProf_Free: p is being freed, so if it was sampled, count
 * it as freed. */
void burrow__mprof_free(const void *p);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MPROF_H */
