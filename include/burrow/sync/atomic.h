/* Go's sync/atomic, spelled for C.
 *
 * Two halves, the same two Go has. The plain functions operate on an ordinary
 * object you already have, so a counter is an int64_t in your own struct and
 * stays one:
 *
 *     static int64_t hits;
 *     sync_atomic_add_int64(&hits, 1);
 *
 * The types are the same operations bound to a value that can only be reached
 * through them, which is the form to reach for when the field is shared and you
 * want the compiler to stop anybody reading it the lazy way:
 *
 *     SyncAtomicInt64 hits;                     // the zero value is ready
 *     sync_atomic_int64_add(&hits, 1);
 *
 * Everything here is sequentially consistent, because that is the only ordering
 * Go's package offers and this is a port of it. Code that wants an acquire load
 * or a release store wants burrow/atomic.h, which is where the orderings live
 * and which this is written on top of.
 *
 * The prefix is the one place the name differs from Go's. `atomic_` followed by
 * a lowercase letter is reserved to <stdatomic.h> for names it has not thought
 * of yet, so taking it would be borrowing something the standard may want back.
 * `sync_atomic_` is the import path with the slash turned into an underscore,
 * and it is the whole of the difference.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package sync/atomic */

#ifndef BURROW_SYNC_ATOMIC_H
#define BURROW_SYNC_ATOMIC_H

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/num.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- signed and unsigned
 *
 * The operations underneath are unsigned, and every signed function here is the
 * unsigned one with a conversion on each end. That is not a shortcut, it is the
 * only way to write it: a signed addition that overflows is undefined
 * behaviour, and a counter that wraps is a counter somebody will wrap on
 * purpose, so the arithmetic happens in the type where wrapping is defined.
 *
 * Coming back the other way is burrow__int32_wrap and burrow__int64_wrap out of
 * burrow/num.h, which are the same conversion the rest of the library does its
 * integer arithmetic through and which fold into the instruction that produced
 * their argument.
 *
 * ---------------------------------------------------------------------- add
 *
 * Returns the new value, which is Go's answer and not the hardware's. Every
 * instruction underneath hands back what was there before, so the delta goes
 * back on afterwards. Go made that choice because a counter that returns its
 * new total is the one people want to print, and a caller who wanted the old
 * one can subtract, which is what they would have done anyway.
 *
 * Subtraction is addition of a negative, and on the unsigned functions that
 * means adding the two's complement: `sync_atomic_add_uint64(&n, ~(uint64_t)0)`
 * takes one off, the same as it does in Go. */

static inline int32_t sync_atomic_add_int32(int32_t *p, int32_t delta) {
    uint32_t d = (uint32_t)delta;

    return burrow__int32_wrap(burrow__atomic_add_u32((uint32_t *)p, d) + d);
}

static inline int64_t sync_atomic_add_int64(int64_t *p, int64_t delta) {
    uint64_t d = (uint64_t)delta;

    return burrow__int64_wrap(burrow__atomic_add_u64((uint64_t *)p, d) + d);
}

static inline uint32_t sync_atomic_add_uint32(uint32_t *p, uint32_t delta) {
    return burrow__atomic_add_u32(p, delta) + delta;
}

static inline uint64_t sync_atomic_add_uint64(uint64_t *p, uint64_t delta) {
    return burrow__atomic_add_u64(p, delta) + delta;
}

static inline Uintptr sync_atomic_add_uintptr(Uintptr *p, Uintptr delta) {
    return burrow__atomic_add_uptr(p, delta) + delta;
}

/* ------------------------------------------------------------- and, and or
 *
 * These return the old value rather than the new one, which is the opposite of
 * add and is Go's rule as well. It is the useful answer for a bitmask: the bit
 * you just set is a bit you already know about, and what you wanted to find out
 * was whether somebody else had set it first. */

static inline int32_t sync_atomic_and_int32(int32_t *p, int32_t mask) {
    return burrow__int32_wrap(burrow__atomic_and_u32((uint32_t *)p, (uint32_t)mask));
}

static inline int64_t sync_atomic_and_int64(int64_t *p, int64_t mask) {
    return burrow__int64_wrap(burrow__atomic_and_u64((uint64_t *)p, (uint64_t)mask));
}

static inline uint32_t sync_atomic_and_uint32(uint32_t *p, uint32_t mask) {
    return burrow__atomic_and_u32(p, mask);
}

static inline uint64_t sync_atomic_and_uint64(uint64_t *p, uint64_t mask) {
    return burrow__atomic_and_u64(p, mask);
}

static inline Uintptr sync_atomic_and_uintptr(Uintptr *p, Uintptr mask) {
    return burrow__atomic_and_uptr(p, mask);
}

static inline int32_t sync_atomic_or_int32(int32_t *p, int32_t mask) {
    return burrow__int32_wrap(burrow__atomic_or_u32((uint32_t *)p, (uint32_t)mask));
}

static inline int64_t sync_atomic_or_int64(int64_t *p, int64_t mask) {
    return burrow__int64_wrap(burrow__atomic_or_u64((uint64_t *)p, (uint64_t)mask));
}

static inline uint32_t sync_atomic_or_uint32(uint32_t *p, uint32_t mask) {
    return burrow__atomic_or_u32(p, mask);
}

static inline uint64_t sync_atomic_or_uint64(uint64_t *p, uint64_t mask) {
    return burrow__atomic_or_u64(p, mask);
}

static inline Uintptr sync_atomic_or_uintptr(Uintptr *p, Uintptr mask) {
    return burrow__atomic_or_uptr(p, mask);
}

/* ------------------------------------------------------- compare and swap
 *
 * Stores desired if what is there is old, and says whether it did.
 *
 * Go takes the expected value rather than a pointer to it, so a failed swap
 * says no and nothing else, and a retry loop reloads for itself. The layer
 * underneath writes back what it saw, the way C11 does, and these throw that
 * away to keep Go's shape. The loop reads the same either way:
 *
 *     for (;;) {
 *         int64_t old = sync_atomic_load_int64(&n);
 *         if (sync_atomic_compare_and_swap_int64(&n, old, old * 2))
 *             break;
 *     } */

static inline bool sync_atomic_compare_and_swap_int32(int32_t *p, int32_t old,
                                                      int32_t desired) {
    uint32_t seen = (uint32_t)old;

    return burrow__atomic_cas_u32((uint32_t *)p, &seen, (uint32_t)desired);
}

static inline bool sync_atomic_compare_and_swap_int64(int64_t *p, int64_t old,
                                                      int64_t desired) {
    uint64_t seen = (uint64_t)old;

    return burrow__atomic_cas_u64((uint64_t *)p, &seen, (uint64_t)desired);
}

static inline bool sync_atomic_compare_and_swap_uint32(uint32_t *p, uint32_t old,
                                                       uint32_t desired) {
    uint32_t seen = old;

    return burrow__atomic_cas_u32(p, &seen, desired);
}

static inline bool sync_atomic_compare_and_swap_uint64(uint64_t *p, uint64_t old,
                                                       uint64_t desired) {
    uint64_t seen = old;

    return burrow__atomic_cas_u64(p, &seen, desired);
}

static inline bool sync_atomic_compare_and_swap_uintptr(Uintptr *p, Uintptr old,
                                                        Uintptr desired) {
    Uintptr seen = old;

    return burrow__atomic_cas_uptr(p, &seen, desired);
}

BURROW_RETAINS(desired) static inline bool
sync_atomic_compare_and_swap_pointer(void **p, void *old, void *desired) {
    void *seen = old;

    return burrow__atomic_cas_ptr(p, &seen, desired);
}

/* --------------------------------------------------------------- load
 *
 * Go's take a plain pointer because Go has no const. These take a pointer to
 * const, because C does and because a function that promises not to write is a
 * function you can hand a field of something you were only lent. */

static inline int32_t sync_atomic_load_int32(const int32_t *p) {
    return burrow__int32_wrap(burrow__atomic_load_u32((const uint32_t *)p));
}

static inline int64_t sync_atomic_load_int64(const int64_t *p) {
    return burrow__int64_wrap(burrow__atomic_load_u64((const uint64_t *)p));
}

static inline uint32_t sync_atomic_load_uint32(const uint32_t *p) {
    return burrow__atomic_load_u32(p);
}

static inline uint64_t sync_atomic_load_uint64(const uint64_t *p) {
    return burrow__atomic_load_u64(p);
}

static inline Uintptr sync_atomic_load_uintptr(const Uintptr *p) {
    return burrow__atomic_load_uptr(p);
}

BURROW_BORROWS(ret, p) static inline void *sync_atomic_load_pointer(void *const *p) {
    return burrow__atomic_load_ptr(p);
}

/* -------------------------------------------------------------- store */

static inline void sync_atomic_store_int32(int32_t *p, int32_t v) {
    burrow__atomic_store_u32((uint32_t *)p, (uint32_t)v);
}

static inline void sync_atomic_store_int64(int64_t *p, int64_t v) {
    burrow__atomic_store_u64((uint64_t *)p, (uint64_t)v);
}

static inline void sync_atomic_store_uint32(uint32_t *p, uint32_t v) {
    burrow__atomic_store_u32(p, v);
}

static inline void sync_atomic_store_uint64(uint64_t *p, uint64_t v) {
    burrow__atomic_store_u64(p, v);
}

static inline void sync_atomic_store_uintptr(Uintptr *p, Uintptr v) {
    burrow__atomic_store_uptr(p, v);
}

BURROW_RETAINS(v) static inline void sync_atomic_store_pointer(void **p, void *v) {
    burrow__atomic_store_ptr(p, v);
}

/* --------------------------------------------------------------- swap
 *
 * Stores v and returns what was there. */

static inline int32_t sync_atomic_swap_int32(int32_t *p, int32_t v) {
    return burrow__int32_wrap(burrow__atomic_swap_u32((uint32_t *)p, (uint32_t)v));
}

static inline int64_t sync_atomic_swap_int64(int64_t *p, int64_t v) {
    return burrow__int64_wrap(burrow__atomic_swap_u64((uint64_t *)p, (uint64_t)v));
}

static inline uint32_t sync_atomic_swap_uint32(uint32_t *p, uint32_t v) {
    return burrow__atomic_swap_u32(p, v);
}

static inline uint64_t sync_atomic_swap_uint64(uint64_t *p, uint64_t v) {
    return burrow__atomic_swap_u64(p, v);
}

static inline Uintptr sync_atomic_swap_uintptr(Uintptr *p, Uintptr v) {
    return burrow__atomic_swap_uptr(p, v);
}

BURROW_BORROWS(ret, p) BURROW_RETAINS(v) static inline void *
sync_atomic_swap_pointer(void **p, void *v) {
    return burrow__atomic_swap_ptr(p, v);
}

/* ------------------------------------------------------------- the types
 *
 * One struct per width, and the field is spelled out rather than hidden because
 * the whole point of these is that you can put one in a struct of your own and
 * it costs what the integer costs. The zero value is a zero, ready to use, with
 * nothing to initialise.
 *
 * Reach through the field and the atomicity is gone, which is the one thing
 * these cannot stop you doing in C. Go stops it by not exporting the field; the
 * nearest C has is that a member called v inside a type called SyncAtomicInt64
 * is not something anybody touches by accident.
 *
 * Copying one after it has been used is Go's rule and it is ours: the copy and
 * the original are separate words, and code that swaps on one and loads from
 * the other is code that will read a value nobody stored. Pass the address. */

typedef struct SyncAtomicBool {
    uint32_t v;
} SyncAtomicBool;

typedef struct SyncAtomicInt32 {
    int32_t v;
} SyncAtomicInt32;

typedef struct SyncAtomicInt64 {
    int64_t v;
} SyncAtomicInt64;

typedef struct SyncAtomicUint32 {
    uint32_t v;
} SyncAtomicUint32;

typedef struct SyncAtomicUint64 {
    uint64_t v;
} SyncAtomicUint64;

typedef struct SyncAtomicUintptr {
    Uintptr v;
} SyncAtomicUintptr;

/* Go's Pointer[T] is one type per T, and this is one type for all of them,
 * because C's void * already converts both ways without a cast. What is lost is
 * the compiler noticing that you stored a Foo and loaded a Bar, which in Go is
 * the whole reason the type is generic. What is gained is that a linked list
 * whose nodes point at each other needs no instantiation and no macro. */
typedef struct SyncAtomicPointer {
    void *v;
} SyncAtomicPointer;

/* Go's Value: any value of any one type, stored and loaded whole.
 *
 * The two words are an Any taken apart, and they are Uintptr rather than
 * pointers so that the type descriptor can be stored without casting the const
 * off it. Neither is anything to read directly. The zero value loads as nil,
 * the first store decides the type, and a later store of a different type
 * panics, which is Go's contract and is checked here rather than by a compiler
 * that has no way to see it. */
typedef struct SyncAtomicValue {
    Uintptr typ;
    Uintptr data;
} SyncAtomicValue;

/* ------------------------------------------------------- their operations */

static inline bool sync_atomic_bool_load(const SyncAtomicBool *b) {
    return burrow__atomic_load_u32(&b->v) != 0;
}

static inline void sync_atomic_bool_store(SyncAtomicBool *b, bool v) {
    burrow__atomic_store_u32(&b->v, v ? 1u : 0u);
}

static inline bool sync_atomic_bool_swap(SyncAtomicBool *b, bool v) {
    return burrow__atomic_swap_u32(&b->v, v ? 1u : 0u) != 0;
}

static inline bool sync_atomic_bool_compare_and_swap(SyncAtomicBool *b, bool old,
                                                     bool desired) {
    uint32_t seen = old ? 1u : 0u;

    return burrow__atomic_cas_u32(&b->v, &seen, desired ? 1u : 0u);
}

static inline int32_t sync_atomic_int32_load(const SyncAtomicInt32 *a) {
    return sync_atomic_load_int32(&a->v);
}

static inline void sync_atomic_int32_store(SyncAtomicInt32 *a, int32_t v) {
    sync_atomic_store_int32(&a->v, v);
}

static inline int32_t sync_atomic_int32_swap(SyncAtomicInt32 *a, int32_t v) {
    return sync_atomic_swap_int32(&a->v, v);
}

static inline bool sync_atomic_int32_compare_and_swap(SyncAtomicInt32 *a, int32_t old,
                                                      int32_t desired) {
    return sync_atomic_compare_and_swap_int32(&a->v, old, desired);
}

static inline int32_t sync_atomic_int32_add(SyncAtomicInt32 *a, int32_t delta) {
    return sync_atomic_add_int32(&a->v, delta);
}

static inline int32_t sync_atomic_int32_and(SyncAtomicInt32 *a, int32_t mask) {
    return sync_atomic_and_int32(&a->v, mask);
}

static inline int32_t sync_atomic_int32_or(SyncAtomicInt32 *a, int32_t mask) {
    return sync_atomic_or_int32(&a->v, mask);
}

static inline int64_t sync_atomic_int64_load(const SyncAtomicInt64 *a) {
    return sync_atomic_load_int64(&a->v);
}

static inline void sync_atomic_int64_store(SyncAtomicInt64 *a, int64_t v) {
    sync_atomic_store_int64(&a->v, v);
}

static inline int64_t sync_atomic_int64_swap(SyncAtomicInt64 *a, int64_t v) {
    return sync_atomic_swap_int64(&a->v, v);
}

static inline bool sync_atomic_int64_compare_and_swap(SyncAtomicInt64 *a, int64_t old,
                                                      int64_t desired) {
    return sync_atomic_compare_and_swap_int64(&a->v, old, desired);
}

static inline int64_t sync_atomic_int64_add(SyncAtomicInt64 *a, int64_t delta) {
    return sync_atomic_add_int64(&a->v, delta);
}

static inline int64_t sync_atomic_int64_and(SyncAtomicInt64 *a, int64_t mask) {
    return sync_atomic_and_int64(&a->v, mask);
}

static inline int64_t sync_atomic_int64_or(SyncAtomicInt64 *a, int64_t mask) {
    return sync_atomic_or_int64(&a->v, mask);
}

static inline uint32_t sync_atomic_uint32_load(const SyncAtomicUint32 *a) {
    return burrow__atomic_load_u32(&a->v);
}

static inline void sync_atomic_uint32_store(SyncAtomicUint32 *a, uint32_t v) {
    burrow__atomic_store_u32(&a->v, v);
}

static inline uint32_t sync_atomic_uint32_swap(SyncAtomicUint32 *a, uint32_t v) {
    return burrow__atomic_swap_u32(&a->v, v);
}

static inline bool sync_atomic_uint32_compare_and_swap(SyncAtomicUint32 *a,
                                                       uint32_t old, uint32_t desired) {
    return sync_atomic_compare_and_swap_uint32(&a->v, old, desired);
}

static inline uint32_t sync_atomic_uint32_add(SyncAtomicUint32 *a, uint32_t delta) {
    return sync_atomic_add_uint32(&a->v, delta);
}

static inline uint32_t sync_atomic_uint32_and(SyncAtomicUint32 *a, uint32_t mask) {
    return burrow__atomic_and_u32(&a->v, mask);
}

static inline uint32_t sync_atomic_uint32_or(SyncAtomicUint32 *a, uint32_t mask) {
    return burrow__atomic_or_u32(&a->v, mask);
}

static inline uint64_t sync_atomic_uint64_load(const SyncAtomicUint64 *a) {
    return burrow__atomic_load_u64(&a->v);
}

static inline void sync_atomic_uint64_store(SyncAtomicUint64 *a, uint64_t v) {
    burrow__atomic_store_u64(&a->v, v);
}

static inline uint64_t sync_atomic_uint64_swap(SyncAtomicUint64 *a, uint64_t v) {
    return burrow__atomic_swap_u64(&a->v, v);
}

static inline bool sync_atomic_uint64_compare_and_swap(SyncAtomicUint64 *a,
                                                       uint64_t old, uint64_t desired) {
    return sync_atomic_compare_and_swap_uint64(&a->v, old, desired);
}

static inline uint64_t sync_atomic_uint64_add(SyncAtomicUint64 *a, uint64_t delta) {
    return sync_atomic_add_uint64(&a->v, delta);
}

static inline uint64_t sync_atomic_uint64_and(SyncAtomicUint64 *a, uint64_t mask) {
    return burrow__atomic_and_u64(&a->v, mask);
}

static inline uint64_t sync_atomic_uint64_or(SyncAtomicUint64 *a, uint64_t mask) {
    return burrow__atomic_or_u64(&a->v, mask);
}

static inline Uintptr sync_atomic_uintptr_load(const SyncAtomicUintptr *a) {
    return burrow__atomic_load_uptr(&a->v);
}

static inline void sync_atomic_uintptr_store(SyncAtomicUintptr *a, Uintptr v) {
    burrow__atomic_store_uptr(&a->v, v);
}

static inline Uintptr sync_atomic_uintptr_swap(SyncAtomicUintptr *a, Uintptr v) {
    return burrow__atomic_swap_uptr(&a->v, v);
}

static inline bool sync_atomic_uintptr_compare_and_swap(SyncAtomicUintptr *a,
                                                        Uintptr old, Uintptr desired) {
    return sync_atomic_compare_and_swap_uintptr(&a->v, old, desired);
}

static inline Uintptr sync_atomic_uintptr_add(SyncAtomicUintptr *a, Uintptr delta) {
    return sync_atomic_add_uintptr(&a->v, delta);
}

static inline Uintptr sync_atomic_uintptr_and(SyncAtomicUintptr *a, Uintptr mask) {
    return burrow__atomic_and_uptr(&a->v, mask);
}

static inline Uintptr sync_atomic_uintptr_or(SyncAtomicUintptr *a, Uintptr mask) {
    return burrow__atomic_or_uptr(&a->v, mask);
}

BURROW_BORROWS(ret, a) static inline void *
sync_atomic_pointer_load(const SyncAtomicPointer *a) {
    return burrow__atomic_load_ptr(&a->v);
}

BURROW_RETAINS(v) static inline void sync_atomic_pointer_store(SyncAtomicPointer *a,
                                                               void *v) {
    burrow__atomic_store_ptr(&a->v, v);
}

BURROW_BORROWS(ret, a) BURROW_RETAINS(v) static inline void *
sync_atomic_pointer_swap(SyncAtomicPointer *a, void *v) {
    return burrow__atomic_swap_ptr(&a->v, v);
}

BURROW_RETAINS(desired) static inline bool
sync_atomic_pointer_compare_and_swap(SyncAtomicPointer *a, void *old, void *desired) {
    return sync_atomic_compare_and_swap_pointer(&a->v, old, desired);
}

/* Value is four calls and none of them are one instruction, so they are in
 * src/sync/value.c rather than here. Load on a Value nobody has stored to gives
 * back a nil Any, and Any is two words, so the pair is published by storing the
 * type descriptor last and read by loading it first. */

BURROW_BORROWS(ret, v) Any sync_atomic_value_load(const SyncAtomicValue *v);

/* Panics on a nil value, and on a value whose type is not the type the first
 * store used. Both are Go's, word for word, because they are the two mistakes
 * this type exists to catch and a program that hits one has a bug that a
 * different message would only make harder to find. */
BURROW_RETAINS(val) void sync_atomic_value_store(SyncAtomicValue *v, Any val);

/* Stores val and returns what was there, or a nil Any when nothing was. Panics
 * on the same two things a store does. */
BURROW_BORROWS(ret, v) BURROW_RETAINS(val) Any
sync_atomic_value_swap(SyncAtomicValue *v, Any val);

/* Stores val if what is there equals old, and says whether it did.
 *
 * Equal means Go's ==, through the type descriptor, so this compares the values
 * and not the pointers to them: two Str values with the same bytes in different
 * places are the same value here. A nil old means the swap happens only if
 * nothing has been stored yet. */
BURROW_RETAINS(val) bool sync_atomic_value_compare_and_swap(SyncAtomicValue *v, Any old,
                                                            Any val);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYNC_ATOMIC_H */
