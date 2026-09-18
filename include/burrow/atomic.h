/* The atomics burrow is built on.
 *
 * This is the layer underneath everything two threads can touch at once: the
 * scheduler's run queues, the futex word inside a Mutex, the state field in a
 * channel. It is not sync/atomic. That is a Go package with a public surface
 * and sequentially consistent semantics, and it will be written on top of this
 * one. These names carry the internal prefix, and the memory order is part of
 * every name, because the only caller is code that has already thought about
 * which order it wants.
 *
 *     uint32_t state = burrow__atomic_load_acquire_u32(&m->state);
 *     while (!burrow__atomic_cas_u32(&m->state, &state, state | LOCKED))
 *         ;
 *
 * Four widths, u32, u64, uptr and ptr. A scheduler needs a word, a counter, an
 * address and a pointer, and every width past those is another set of functions
 * nobody calls that every backend still has to get right.
 *
 * Three backends. GCC and Clang get the __atomic builtins, which operate on
 * ordinary objects and are the reason a caller can hand us a plain uint32_t *
 * instead of a wrapper struct. MSVC gets Interlocked and the __iso_volatile
 * loads and stores, because it has no _Atomic. Anything else falls back to
 * <stdatomic.h> through a cast. Nothing in CI selects that last one on its own,
 * so it is forced and run on every platform that can compile it, which is all
 * of them except MSVC. MSVC keeps C11 atomics behind /experimental:c11atomics
 * and including <stdatomic.h> without that switch is a hard error, so the
 * forced build is skipped there rather than turning an experimental flag on.
 *
 * Where 64 bit atomics are not lock free, which here means every 32 bit
 * machine, the u64 operations go through a table of spin locks keyed on the
 * address, the way Go does it. Build with -DBURROW_ATOMIC_FORCE_LOCK64=1 to
 * take that path on a machine that would not otherwise need it, which is how it
 * gets tested on the machines we actually have.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_ATOMIC_H
#define BURROW_ATOMIC_H

#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- which backend */

/* Set BURROW_ATOMIC_BACKEND_C11 to 1 to force the last resort path. It exists
 * so the fallback can be compiled and run somewhere, since a backend nothing
 * builds is a backend that is already broken. On MSVC it needs
 * /experimental:c11atomics as well, or <stdatomic.h> refuses to compile. */
#if defined(BURROW_ATOMIC_BACKEND_C11) && BURROW_ATOMIC_BACKEND_C11
#define BURROW__ATOMIC_C11 1
#elif BURROW_CC_MSVC
#define BURROW__ATOMIC_MSVC 1
#elif defined(__ATOMIC_SEQ_CST)
#define BURROW__ATOMIC_BUILTIN 1
#else
#define BURROW__ATOMIC_C11 1
#endif

#ifndef BURROW__ATOMIC_C11
#define BURROW__ATOMIC_C11 0
#endif
#ifndef BURROW__ATOMIC_MSVC
#define BURROW__ATOMIC_MSVC 0
#endif
#ifndef BURROW__ATOMIC_BUILTIN
#define BURROW__ATOMIC_BUILTIN 0
#endif

/* The lock table is for 64 bit operations on machines where they are not a
 * single instruction. Deciding that on pointer width rather than on whatever
 * the compiler claims about compare and swap is deliberate: on 32 bit x86 the
 * builtins will happily compile a 64 bit load into a call into libatomic, and
 * then the library has a link time dependency nobody asked for that only shows
 * up on one platform. Pointer width gets the same answer with no dependency. */
#if defined(BURROW_ATOMIC_FORCE_LOCK64) && BURROW_ATOMIC_FORCE_LOCK64
#define BURROW__ATOMIC_LOCK64 1
#elif BURROW_PTR_BITS >= 64
#define BURROW__ATOMIC_LOCK64 0
#else
#define BURROW__ATOMIC_LOCK64 1
#endif

/* ------------------------------------------------------------------ orders */

/* Four of the six C11 orders. Consume is missing because no compiler
 * implements it as anything other than acquire, and acq_rel is missing because
 * every operation that would want it is a read modify write that this file
 * already gives a full barrier. */
#if BURROW__ATOMIC_BUILTIN
#define BURROW__O_RELAXED __ATOMIC_RELAXED
#define BURROW__O_ACQUIRE __ATOMIC_ACQUIRE
#define BURROW__O_RELEASE __ATOMIC_RELEASE
#define BURROW__O_SEQ_CST __ATOMIC_SEQ_CST
#elif BURROW__ATOMIC_C11
#include <stdatomic.h>
#define BURROW__O_RELAXED memory_order_relaxed
#define BURROW__O_ACQUIRE memory_order_acquire
#define BURROW__O_RELEASE memory_order_release
#define BURROW__O_SEQ_CST memory_order_seq_cst
#else
#define BURROW__O_RELAXED 0
#define BURROW__O_ACQUIRE 1
#define BURROW__O_RELEASE 2
#define BURROW__O_SEQ_CST 3
#endif

/* --------------------------------------------------- the GCC/Clang backend */

#if BURROW__ATOMIC_BUILTIN

#define BURROW__AT_LOAD(sfx, T, p, ord) __atomic_load_n(p, ord)
#define BURROW__AT_STORE(sfx, T, p, v, ord) __atomic_store_n(p, v, ord)
#define BURROW__AT_ADD(sfx, T, p, v) __atomic_fetch_add(p, v, BURROW__O_SEQ_CST)
#define BURROW__AT_AND(sfx, T, p, v) __atomic_fetch_and(p, v, BURROW__O_SEQ_CST)
#define BURROW__AT_OR(sfx, T, p, v) __atomic_fetch_or(p, v, BURROW__O_SEQ_CST)
#define BURROW__AT_SWAP(sfx, T, p, v) __atomic_exchange_n(p, v, BURROW__O_SEQ_CST)
#define BURROW__AT_CAS(sfx, T, p, exp, des, weak, ord)                                 \
    __atomic_compare_exchange_n(p, exp, des, weak, ord, BURROW__O_RELAXED)
#define BURROW__AT_FENCE(ord) __atomic_thread_fence(ord)

/* -------------------------------------------------------- the MSVC backend */

#elif BURROW__ATOMIC_MSVC

/* <intrin.h> rather than <windows.h>, on purpose. It is the compiler's own
 * header, it declares intrinsics and nothing else, and it does not arrive
 * carrying a macro named min.
 *
 * Every Interlocked function below is a full barrier on every target MSVC
 * supports, so acquire and release are both spelled as the full thing. The
 * suffixed _acq and _rel variants exist on ARM only, and a fast path that
 * compiles on one of the two Windows architectures is not a fast path, it is a
 * second bug waiting for a machine to turn up. The relaxed loads and stores are
 * where the cost would actually show, in a spin loop, and those get the cheap
 * intrinsic. */

#include <intrin.h>

static inline uint32_t burrow__ms_load_u32(const uint32_t *p, int ord) {
    if (ord == BURROW__O_RELAXED)
        return (uint32_t)__iso_volatile_load32((const volatile int *)p);
    return (uint32_t)_InterlockedOr((volatile long *)(void *)(uintptr_t)(const void *)p,
                                    0);
}

static inline void burrow__ms_store_u32(uint32_t *p, uint32_t v, int ord) {
    if (ord == BURROW__O_RELAXED) {
        __iso_volatile_store32((volatile int *)p, (int)v);
        return;
    }
    (void)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline uint32_t burrow__ms_add_u32(uint32_t *p, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v);
}

static inline uint32_t burrow__ms_and_u32(uint32_t *p, uint32_t v) {
    return (uint32_t)_InterlockedAnd((volatile long *)p, (long)v);
}

static inline uint32_t burrow__ms_or_u32(uint32_t *p, uint32_t v) {
    return (uint32_t)_InterlockedOr((volatile long *)p, (long)v);
}

static inline uint32_t burrow__ms_swap_u32(uint32_t *p, uint32_t v) {
    return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline bool burrow__ms_cas_u32(uint32_t *p, uint32_t *exp, uint32_t des) {
    long seen = _InterlockedCompareExchange((volatile long *)p, (long)des, (long)*exp);
    if ((uint32_t)seen == *exp)
        return true;
    *exp = (uint32_t)seen;
    return false;
}

static inline void burrow__ms_fence(void) {
    /* A full fence with no architecture in it. The lock prefix on x86 and the
     * dmb on ARM64 both come out of the intrinsic, and the target is a local so
     * that the fence cannot contend with anything real. */
    volatile long here = 0;
    (void)_InterlockedOr(&here, 0);
}

/* The 64 bit helpers are compiled whenever the machine is 64 bit, not only when
 * the lock table is off, because uptr is an alias for them and a forced lock
 * table must not take uptr down with it. */
#if BURROW_PTR_BITS >= 64

static inline uint64_t burrow__ms_load_u64(const uint64_t *p, int ord) {
    if (ord == BURROW__O_RELAXED)
        return (uint64_t)__iso_volatile_load64((const volatile long long *)p);
    return (uint64_t)_InterlockedOr64(
        (volatile __int64 *)(void *)(uintptr_t)(const void *)p, 0);
}

static inline void burrow__ms_store_u64(uint64_t *p, uint64_t v, int ord) {
    if (ord == BURROW__O_RELAXED) {
        __iso_volatile_store64((volatile long long *)p, (long long)v);
        return;
    }
    (void)_InterlockedExchange64((volatile __int64 *)p, (__int64)v);
}

static inline uint64_t burrow__ms_add_u64(uint64_t *p, uint64_t v) {
    return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v);
}

static inline uint64_t burrow__ms_and_u64(uint64_t *p, uint64_t v) {
    return (uint64_t)_InterlockedAnd64((volatile __int64 *)p, (__int64)v);
}

static inline uint64_t burrow__ms_or_u64(uint64_t *p, uint64_t v) {
    return (uint64_t)_InterlockedOr64((volatile __int64 *)p, (__int64)v);
}

static inline uint64_t burrow__ms_swap_u64(uint64_t *p, uint64_t v) {
    return (uint64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v);
}

static inline bool burrow__ms_cas_u64(uint64_t *p, uint64_t *exp, uint64_t des) {
    __int64 seen = _InterlockedCompareExchange64((volatile __int64 *)p, (__int64)des,
                                                 (__int64)*exp);
    if ((uint64_t)seen == *exp)
        return true;
    *exp = (uint64_t)seen;
    return false;
}

#endif /* BURROW_PTR_BITS >= 64 */

BURROW_BORROWS(ret, p) static inline void *burrow__ms_load_ptr(void *const *p,
                                                               int ord) {
    if (ord == BURROW__O_RELAXED) {
#if BURROW_PTR_BITS >= 64
        return (void *)(intptr_t)__iso_volatile_load64((const volatile long long *)p);
#else
        return (void *)(intptr_t)__iso_volatile_load32((const volatile int *)p);
#endif
    }
    return _InterlockedCompareExchangePointer(
        (void *volatile *)(void *)(uintptr_t)(const void *)p, NULL, NULL);
}

BURROW_RETAINS(v) static inline void burrow__ms_store_ptr(void **p, void *v, int ord) {
    if (ord == BURROW__O_RELAXED) {
#if BURROW_PTR_BITS >= 64
        __iso_volatile_store64((volatile long long *)p, (long long)(intptr_t)v);
#else
        __iso_volatile_store32((volatile int *)p, (int)(intptr_t)v);
#endif
        return;
    }
    (void)_InterlockedExchangePointer((void *volatile *)p, v);
}

BURROW_BORROWS(ret, p) BURROW_RETAINS(v) static inline void *
burrow__ms_swap_ptr(void **p, void *v) {
    return _InterlockedExchangePointer((void *volatile *)p, v);
}

BURROW_RETAINS(des) static inline bool burrow__ms_cas_ptr(void **p, void **exp,
                                                          void *des) {
    void *seen = _InterlockedCompareExchangePointer((void *volatile *)p, des, *exp);
    if (seen == *exp)
        return true;
    *exp = seen;
    return false;
}

#define BURROW__AT_LOAD(sfx, T, p, ord) ((T)burrow__ms_load_##sfx(p, ord))
#define BURROW__AT_STORE(sfx, T, p, v, ord) burrow__ms_store_##sfx(p, v, ord)
#define BURROW__AT_ADD(sfx, T, p, v) ((T)burrow__ms_add_##sfx(p, v))
#define BURROW__AT_AND(sfx, T, p, v) ((T)burrow__ms_and_##sfx(p, v))
#define BURROW__AT_OR(sfx, T, p, v) ((T)burrow__ms_or_##sfx(p, v))
#define BURROW__AT_SWAP(sfx, T, p, v) ((T)burrow__ms_swap_##sfx(p, v))
#define BURROW__AT_CAS(sfx, T, p, exp, des, weak, ord) burrow__ms_cas_##sfx(p, exp, des)
#define BURROW__AT_FENCE(ord) burrow__ms_fence()

/* ---------------------------------------------------- the last resort path */

#else

#define BURROW__AT_LOAD(sfx, T, p, ord)                                                \
    atomic_load_explicit((const _Atomic(T) *)(const void *)(p), ord)
#define BURROW__AT_STORE(sfx, T, p, v, ord)                                            \
    atomic_store_explicit((_Atomic(T) *)(void *)(p), v, ord)
#define BURROW__AT_ADD(sfx, T, p, v)                                                   \
    atomic_fetch_add_explicit((_Atomic(T) *)(void *)(p), v, BURROW__O_SEQ_CST)
#define BURROW__AT_AND(sfx, T, p, v)                                                   \
    atomic_fetch_and_explicit((_Atomic(T) *)(void *)(p), v, BURROW__O_SEQ_CST)
#define BURROW__AT_OR(sfx, T, p, v)                                                    \
    atomic_fetch_or_explicit((_Atomic(T) *)(void *)(p), v, BURROW__O_SEQ_CST)
#define BURROW__AT_SWAP(sfx, T, p, v)                                                  \
    atomic_exchange_explicit((_Atomic(T) *)(void *)(p), v, BURROW__O_SEQ_CST)
#define BURROW__AT_CAS(sfx, T, p, exp, des, weak, ord)                                 \
    ((weak) ? atomic_compare_exchange_weak_explicit((_Atomic(T) *)(void *)(p), exp,    \
                                                    des, ord, BURROW__O_RELAXED)       \
            : atomic_compare_exchange_strong_explicit((_Atomic(T) *)(void *)(p), exp,  \
                                                      des, ord, BURROW__O_RELAXED))
#define BURROW__AT_FENCE(ord) atomic_thread_fence(ord)

#endif

/* ---------------------------------------------------------- the lock table */

/* The definitions live in src/runtime/atomic64.c rather than here, because they
 * are a spin lock and a table and neither of those belongs inlined into every
 * translation unit that wants to add one to a counter.
 *
 * They are declared and compiled on every platform, not only on the ones that
 * need them. The table is four kilobytes of bss that a 64 bit build never
 * touches, so it never costs a page, and in exchange the fallback is something
 * the test suite can reach on the machines we own rather than something that
 * only ever runs where nobody is looking. */
uint64_t burrow__atomic64_load(const uint64_t *p);
void burrow__atomic64_store(uint64_t *p, uint64_t v);
uint64_t burrow__atomic64_add(uint64_t *p, uint64_t v);
uint64_t burrow__atomic64_and(uint64_t *p, uint64_t v);
uint64_t burrow__atomic64_or(uint64_t *p, uint64_t v);
uint64_t burrow__atomic64_swap(uint64_t *p, uint64_t v);
bool burrow__atomic64_cas(uint64_t *p, uint64_t *expected, uint64_t desired);

/* ---------------------------------------------------------- the operations */

/* One macro writes the whole set for an integer width. It is a macro because
 * the alternative is the same forty lines typed three times, and the third copy
 * is the one with the typo in it. What comes out is ordinary static inline
 * functions with ordinary prototypes, so a debugger and a compiler error both
 * name something a reader can go and find.
 *
 * CAS takes the expected value by pointer and writes back what it actually saw,
 * which is what C11 does and what a retry loop wants, because the loop needs
 * the current value to compute its next attempt from.
 *
 * The weak variant is allowed to fail when the value did match. Use it when the
 * call is already inside a loop, which on load linked machines saves the inner
 * retry the strong one has to do for you. Use the strong one everywhere else. */

/* Two suppressions, both of which run to the end of the operations below
 * because a diagnostic inside a macro is reported at the line that expanded it.
 *
 * bugprone-macro-parentheses: T is a type and not an expression, so it cannot be
 * wrapped in parentheses the way an argument normally would be.
 *
 * readability-non-const-parameter: it wants a const pointer on store and on the
 * expected value of a compare and swap, because it cannot see through the
 * builtin to the write. A store that took a pointer to const would be a store
 * that could not store, and a compare and swap has to write back what it saw. */
/* NOLINTBEGIN(bugprone-macro-parentheses, readability-non-const-parameter) */
#define BURROW__ATOMIC_INT_OPS(sfx, T)                                                 \
    static inline T burrow__atomic_load_relaxed_##sfx(const T *p) {                    \
        return BURROW__AT_LOAD(sfx, T, p, BURROW__O_RELAXED);                          \
    }                                                                                  \
    static inline T burrow__atomic_load_acquire_##sfx(const T *p) {                    \
        return BURROW__AT_LOAD(sfx, T, p, BURROW__O_ACQUIRE);                          \
    }                                                                                  \
    static inline T burrow__atomic_load_##sfx(const T *p) {                            \
        return BURROW__AT_LOAD(sfx, T, p, BURROW__O_SEQ_CST);                          \
    }                                                                                  \
    static inline void burrow__atomic_store_relaxed_##sfx(T *p, T v) {                 \
        BURROW__AT_STORE(sfx, T, p, v, BURROW__O_RELAXED);                             \
    }                                                                                  \
    static inline void burrow__atomic_store_release_##sfx(T *p, T v) {                 \
        BURROW__AT_STORE(sfx, T, p, v, BURROW__O_RELEASE);                             \
    }                                                                                  \
    static inline void burrow__atomic_store_##sfx(T *p, T v) {                         \
        BURROW__AT_STORE(sfx, T, p, v, BURROW__O_SEQ_CST);                             \
    }                                                                                  \
    static inline T burrow__atomic_add_##sfx(T *p, T v) {                              \
        return BURROW__AT_ADD(sfx, T, p, v);                                           \
    }                                                                                  \
    static inline T burrow__atomic_and_##sfx(T *p, T v) {                              \
        return BURROW__AT_AND(sfx, T, p, v);                                           \
    }                                                                                  \
    static inline T burrow__atomic_or_##sfx(T *p, T v) {                               \
        return BURROW__AT_OR(sfx, T, p, v);                                            \
    }                                                                                  \
    static inline T burrow__atomic_swap_##sfx(T *p, T v) {                             \
        return BURROW__AT_SWAP(sfx, T, p, v);                                          \
    }                                                                                  \
    static inline bool burrow__atomic_cas_##sfx(T *p, T *expected, T desired) {        \
        return BURROW__AT_CAS(sfx, T, p, expected, desired, false, BURROW__O_SEQ_CST); \
    }                                                                                  \
    static inline bool burrow__atomic_cas_weak_##sfx(T *p, T *expected, T desired) {   \
        return BURROW__AT_CAS(sfx, T, p, expected, desired, true, BURROW__O_SEQ_CST);  \
    }                                                                                  \
    static inline bool burrow__atomic_cas_acquire_##sfx(T *p, T *expected,             \
                                                        T desired) {                   \
        return BURROW__AT_CAS(sfx, T, p, expected, desired, false, BURROW__O_ACQUIRE); \
    }                                                                                  \
    static inline bool burrow__atomic_cas_release_##sfx(T *p, T *expected,             \
                                                        T desired) {                   \
        return BURROW__AT_CAS(sfx, T, p, expected, desired, false, BURROW__O_RELEASE); \
    }
BURROW__ATOMIC_INT_OPS(u32, uint32_t)

#if !BURROW__ATOMIC_LOCK64

BURROW__ATOMIC_INT_OPS(u64, uint64_t)

#else

/* Everything is sequentially consistent here, because a lock does not have a
 * weaker mode and pretending the relaxed load is cheaper than the others would
 * be a lie a caller could go and build on. */
static inline uint64_t burrow__atomic_load_relaxed_u64(const uint64_t *p) {
    return burrow__atomic64_load(p);
}
static inline uint64_t burrow__atomic_load_acquire_u64(const uint64_t *p) {
    return burrow__atomic64_load(p);
}
static inline uint64_t burrow__atomic_load_u64(const uint64_t *p) {
    return burrow__atomic64_load(p);
}
static inline void burrow__atomic_store_relaxed_u64(uint64_t *p, uint64_t v) {
    burrow__atomic64_store(p, v);
}
static inline void burrow__atomic_store_release_u64(uint64_t *p, uint64_t v) {
    burrow__atomic64_store(p, v);
}
static inline void burrow__atomic_store_u64(uint64_t *p, uint64_t v) {
    burrow__atomic64_store(p, v);
}
static inline uint64_t burrow__atomic_add_u64(uint64_t *p, uint64_t v) {
    return burrow__atomic64_add(p, v);
}
static inline uint64_t burrow__atomic_and_u64(uint64_t *p, uint64_t v) {
    return burrow__atomic64_and(p, v);
}
static inline uint64_t burrow__atomic_or_u64(uint64_t *p, uint64_t v) {
    return burrow__atomic64_or(p, v);
}
static inline uint64_t burrow__atomic_swap_u64(uint64_t *p, uint64_t v) {
    return burrow__atomic64_swap(p, v);
}
static inline bool burrow__atomic_cas_u64(uint64_t *p, uint64_t *expected,
                                          uint64_t desired) {
    return burrow__atomic64_cas(p, expected, desired);
}
static inline bool burrow__atomic_cas_weak_u64(uint64_t *p, uint64_t *expected,
                                               uint64_t desired) {
    return burrow__atomic64_cas(p, expected, desired);
}
static inline bool burrow__atomic_cas_acquire_u64(uint64_t *p, uint64_t *expected,
                                                  uint64_t desired) {
    return burrow__atomic64_cas(p, expected, desired);
}
static inline bool burrow__atomic_cas_release_u64(uint64_t *p, uint64_t *expected,
                                                  uint64_t desired) {
    return burrow__atomic64_cas(p, expected, desired);
}

#endif /* BURROW__ATOMIC_LOCK64 */

/* uintptr_t gets its own set rather than an alias for u32 or u64, because a
 * caller should not have to know which machine it is on to pick the function
 * name, and because a cast between uintptr_t and a fixed width integer is
 * exactly the sort of thing that compiles everywhere and is wrong in one place.
 * It never needs the lock table: it is pointer sized by definition, and a
 * machine whose pointers do not load atomically cannot run a scheduler. */
#if BURROW__ATOMIC_MSVC
#if BURROW_PTR_BITS >= 64
#define burrow__ms_load_uptr(p, ord) burrow__ms_load_u64((const uint64_t *)(p), ord)
#define burrow__ms_store_uptr(p, v, ord) burrow__ms_store_u64((uint64_t *)(p), v, ord)
#define burrow__ms_add_uptr(p, v) burrow__ms_add_u64((uint64_t *)(p), v)
#define burrow__ms_and_uptr(p, v) burrow__ms_and_u64((uint64_t *)(p), v)
#define burrow__ms_or_uptr(p, v) burrow__ms_or_u64((uint64_t *)(p), v)
#define burrow__ms_swap_uptr(p, v) burrow__ms_swap_u64((uint64_t *)(p), v)
#define burrow__ms_cas_uptr(p, e, d)                                                   \
    burrow__ms_cas_u64((uint64_t *)(p), (uint64_t *)(e), d)
#else
#define burrow__ms_load_uptr(p, ord) burrow__ms_load_u32((const uint32_t *)(p), ord)
#define burrow__ms_store_uptr(p, v, ord) burrow__ms_store_u32((uint32_t *)(p), v, ord)
#define burrow__ms_add_uptr(p, v) burrow__ms_add_u32((uint32_t *)(p), v)
#define burrow__ms_and_uptr(p, v) burrow__ms_and_u32((uint32_t *)(p), v)
#define burrow__ms_or_uptr(p, v) burrow__ms_or_u32((uint32_t *)(p), v)
#define burrow__ms_swap_uptr(p, v) burrow__ms_swap_u32((uint32_t *)(p), v)
#define burrow__ms_cas_uptr(p, e, d)                                                   \
    burrow__ms_cas_u32((uint32_t *)(p), (uint32_t *)(e), d)
#endif
#endif /* BURROW__ATOMIC_MSVC */

BURROW__ATOMIC_INT_OPS(uptr, uintptr_t)

/* void * is written out rather than generated, because const T * for T of
 * void * is a pointer to const void and not a const pointer to void, and a
 * macro that gets that wrong gets it wrong in silence. Add, and and or are not
 * here: arithmetic on a pointer you are racing on is a question with no good
 * answer, and the caller who wants one has uptr. */

BURROW_BORROWS(ret, p) static inline void *
burrow__atomic_load_relaxed_ptr(void *const *p) {
    return BURROW__AT_LOAD(ptr, void *, p, BURROW__O_RELAXED);
}
BURROW_BORROWS(ret, p) static inline void *
burrow__atomic_load_acquire_ptr(void *const *p) {
    return BURROW__AT_LOAD(ptr, void *, p, BURROW__O_ACQUIRE);
}
BURROW_BORROWS(ret, p) static inline void *burrow__atomic_load_ptr(void *const *p) {
    return BURROW__AT_LOAD(ptr, void *, p, BURROW__O_SEQ_CST);
}
BURROW_RETAINS(v) static inline void burrow__atomic_store_relaxed_ptr(void **p,
                                                                      void *v) {
    BURROW__AT_STORE(ptr, void *, p, v, BURROW__O_RELAXED);
}
BURROW_RETAINS(v) static inline void burrow__atomic_store_release_ptr(void **p,
                                                                      void *v) {
    BURROW__AT_STORE(ptr, void *, p, v, BURROW__O_RELEASE);
}
BURROW_RETAINS(v) static inline void burrow__atomic_store_ptr(void **p, void *v) {
    BURROW__AT_STORE(ptr, void *, p, v, BURROW__O_SEQ_CST);
}
BURROW_BORROWS(ret, p) BURROW_RETAINS(v) static inline void *
burrow__atomic_swap_ptr(void **p, void *v) {
    return BURROW__AT_SWAP(ptr, void *, p, v);
}
BURROW_RETAINS(desired) static inline bool
burrow__atomic_cas_ptr(void **p, void **expected, void *desired) {
    return BURROW__AT_CAS(ptr, void *, p, expected, desired, false, BURROW__O_SEQ_CST);
}
BURROW_RETAINS(desired) static inline bool
burrow__atomic_cas_weak_ptr(void **p, void **expected, void *desired) {
    return BURROW__AT_CAS(ptr, void *, p, expected, desired, true, BURROW__O_SEQ_CST);
}
BURROW_RETAINS(desired) static inline bool
burrow__atomic_cas_acquire_ptr(void **p, void **expected, void *desired) {
    return BURROW__AT_CAS(ptr, void *, p, expected, desired, false, BURROW__O_ACQUIRE);
}
BURROW_RETAINS(desired) static inline bool
burrow__atomic_cas_release_ptr(void **p, void **expected, void *desired) {
    return BURROW__AT_CAS(ptr, void *, p, expected, desired, false, BURROW__O_RELEASE);
}

/* NOLINTEND(bugprone-macro-parentheses, readability-non-const-parameter) */

/* ------------------------------------------------------------------ fences */

/* A fence orders the accesses around it without touching a location of its own.
 * Reach for one when the thing being published is a whole structure rather than
 * a single word, so the release belongs between filling it in and handing over
 * the pointer, not inside either. Most code does not need these and should use
 * an acquire load or a release store instead, which say the same thing about
 * one location and let the compiler put the barrier where it costs least. */

static inline void burrow__atomic_fence_acquire(void) {
    BURROW__AT_FENCE(BURROW__O_ACQUIRE);
}
static inline void burrow__atomic_fence_release(void) {
    BURROW__AT_FENCE(BURROW__O_RELEASE);
}
static inline void burrow__atomic_fence(void) {
    BURROW__AT_FENCE(BURROW__O_SEQ_CST);
}

/* A hint that this thread is spinning and the core could usefully go and do
 * something else for a moment. It is not a fence and it orders nothing. On a
 * hyperthreaded core it is the difference between a spin loop that starves its
 * sibling and one that does not. */
static inline void burrow__atomic_spin_hint(void) {
#if BURROW__ATOMIC_MSVC && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif BURROW__ATOMIC_MSVC && defined(_M_ARM64)
    __yield();
#elif defined(__i386__) || defined(__x86_64__)
    /* Inline assembly, because there is no builtin for this that both GCC and
     * Clang have had for long enough to rely on, and because one instruction
     * with no operands is the case where assembly is the clearer thing to
     * write. */
    /* NOLINTNEXTLINE(portability-no-assembler) */
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    /* NOLINTNEXTLINE(portability-no-assembler) */
    __asm__ __volatile__("yield" ::: "memory");
#else
    /* Nothing portable to say. A spin loop still works, it is just ruder. */
    (void)0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ATOMIC_H */
