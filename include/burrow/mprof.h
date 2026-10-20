/* runtime.MemProfileRate and runtime.MemProfile: the heap profile, and
 * runtime.SetBlockProfileRate and runtime.SetMutexProfileFraction: the block
 * and mutex profiles.
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
 * The block and mutex profiles are described further down.
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

/* The block profile is where goroutines wait: on a channel, in a select, for a
 * sync_mutex, a sync_rw_mutex, a sync_cond or a sync_wait_group. The mutex
 * profile is who made them wait, charged to the unlock that let a contended
 * sync_mutex or sync_rw_mutex go. Both are off until a program turns them on:
 *
 *     runtime_set_block_profile_rate(1);
 *     runtime_set_mutex_profile_fraction(1);
 *
 * Time in both is counted in ticks of burrow__nanotime, so a tick is a
 * nanosecond and there are 1e9 of them a second, where Go counts CPU cycles. */

/* Go's runtime.SetBlockProfileRate: the profiler aims to sample an average of
 * one blocking event per rate nanoseconds spent blocked. 1 records every
 * event, and 0 or less turns the profile off. */
void runtime_set_block_profile_rate(Int rate);

/* Go's runtime.SetMutexProfileFraction: on average 1/rate contention events
 * are reported. 0 turns the profile off, and less than 0 only reads the rate.
 * The rate before the call is returned. */
Int runtime_set_mutex_profile_fraction(Int rate);

/* Go's runtime.BlockProfileRecord: the blocking events from one stack. */
typedef struct RuntimeBlockProfileRecord {
    int64_t count;
    int64_t cycles;
    Uintptr stack0[32]; /* stack trace for this record; ends at first 0 entry */
} RuntimeBlockProfileRecord;

/* Stack returns the stack trace associated with the record, a prefix of
 * stack0. The length of that prefix is returned. */
Int runtime_block_profile_record_stack(const RuntimeBlockProfileRecord *r);

/* Go's runtime.BlockProfile: the number of records in the current blocking
 * profile. When len is at least that, the records are copied into p and *ok is
 * true. Otherwise p is left alone and *ok is false. ok may be NULL. */
Int runtime_block_profile(RuntimeBlockProfileRecord *p, Int len, bool *ok);

/* Go's runtime.MutexProfile, the same for the mutex profile. */
Int runtime_mutex_profile(RuntimeBlockProfileRecord *p, Int len, bool *ok);

/* Go's profilerecord.BlockProfileRecord, for runtime/pprof. stack lives as long
 * as the program does. */
typedef struct burrow__BlockProfileRecord {
    int64_t count, cycles;
    BURROW_BORROWS(1) const Uintptr *stack;
    Int nstack;
} burrow__BlockProfileRecord;

typedef void (*burrow__BlockProfileFn)(void *env, const burrow__BlockProfileRecord *r);

/* Go's blockProfileInternal and mutexProfileInternal: the number of records,
 * and when that is no more than size, a call to fn for each of them and *ok
 * set to true. fn is called with the profile's lock held, so it must not
 * allocate from or free to the heap. */
Int burrow__block_profile(Int size, burrow__BlockProfileFn fn, void *env, bool *ok);
Int burrow__mutex_profile(Int size, burrow__BlockProfileFn fn, void *env, bool *ok);

/* Go's cputicks and ticksPerSecond. A tick is a nanosecond here. */
int64_t burrow__cputicks(void);
int64_t burrow__ticks_per_second(void);

/* Whether the block and mutex profiles are on, Go's blockprofilerate > 0 and
 * mutexprofilerate > 0, for the code that times a wait only when one is. */
bool burrow__block_profiling(void);
bool burrow__mutex_profiling(void);

/* Go's blockevent: a wait of cycles ticks has ended. skip is the number of
 * frames to leave off the stack, 1 for one that starts at the caller. */
void burrow__blockevent(int64_t cycles, Int skip);

/* Go's mutexevent: an unlock let waiters go that had waited cycles ticks
 * between them. skip is as for burrow__blockevent. */
void burrow__mutexevent(int64_t cycles, Int skip);

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
