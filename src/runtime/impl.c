/* The implements registry: a type and an interface in, the vtable out.
 *
 * Go makes the table a value needs to sit in an interface when it compiles the
 * conversion, and reflect makes one at run time from the method set when it
 * has to. Neither is open to C. A vtable is a struct of function pointers with
 * the interface's own signatures, somebody wrote it by hand, and nothing about
 * a type says where its vtables are. BURROW_IMPLEMENTS is how a program says.
 *
 * It works the way BURROW_REGISTER_TYPE does, and this file is registry.c over
 * again for a second section: one pointer per record in a section of its own,
 * read once the first time anybody asks, into an open addressed table keyed by
 * the two descriptor addresses. type_register_impl is the runtime way in for a
 * module loaded after startup, which is why the table has a lock.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/type.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- the section
 *
 * The same three spellings registry.c uses for its section, for the reasons
 * given there. */

#if defined(__APPLE__) && defined(__GNUC__)

__extension__ extern const TypeImpl *const
    burrow__impl_sec_start[] __asm("section$start$__DATA$__burrowimpl");
__extension__ extern const TypeImpl *const
    burrow__impl_sec_stop[] __asm("section$end$__DATA$__burrowimpl");
#define BURROW__HAVE_IMPL_SECTION 1

#elif defined(_WIN32) && (defined(__GNUC__) || defined(_MSC_VER))

#if defined(_MSC_VER)
#pragma section(".brwi$a", read)
#pragma section(".brwi$z", read)
__declspec(allocate(".brwi$a")) static const TypeImpl *const impl_sec_first = NULL;
__declspec(allocate(".brwi$z")) static const TypeImpl *const impl_sec_last = NULL;
#else
BURROW__TYPE_NO_REDZONE __attribute__((
    used, section(".brwi$a"))) static const TypeImpl *const impl_sec_first = NULL;
BURROW__TYPE_NO_REDZONE __attribute__((
    used, section(".brwi$z"))) static const TypeImpl *const impl_sec_last = NULL;
#endif
#define burrow__impl_sec_start (&impl_sec_first + 1)
#define burrow__impl_sec_stop (&impl_sec_last)
#define BURROW__HAVE_IMPL_SECTION 1

#elif defined(__ELF__) && defined(__GNUC__)

extern const TypeImpl *const __start_burrowimpl[] __attribute__((weak));
extern const TypeImpl *const __stop_burrowimpl[] __attribute__((weak));
#define burrow__impl_sec_start __start_burrowimpl
#define burrow__impl_sec_stop __stop_burrowimpl
#define BURROW__HAVE_IMPL_SECTION 1

#endif

/* The address with nothing known about it, as BURROW__SEC_BOUND in
 * registry.c, and for the same -Warray-bounds. */
#if defined(__GNUC__)
#define BURROW__IMPL_BOUND(p)                                                          \
    (__extension__({                                                                   \
        const TypeImpl *const *burrow__b = (p);                                        \
        __asm__("" : "+r"(burrow__b));                                                 \
        burrow__b;                                                                     \
    }))
#else
#define BURROW__IMPL_BOUND(p) (p)
#endif

/* --------------------------------------------------------------- the table */

static SyncOnce impl_once;
static SyncRWMutex impl_lock;
static const TypeImpl **impl_slots;
static Int impl_cap; /* a power of two, or zero when there is no table */
static Int impl_len;

/* The two addresses mixed. The low bits of a descriptor's address are the same
 * for every descriptor, so they are multiplied up before anything is masked. */
static uint64_t impl_hash(const Type *t, const Type *iface) {
    uint64_t h = (uint64_t)(uintptr_t)t * 0x9E3779B97F4A7C15U;
    h ^= (uint64_t)(uintptr_t)iface * 0xC2B2AE3D27D4EB4FU;
    return h ^ (h >> 31);
}

/* Put r in, or find its pair already there. True when the pair now has r's
 * vtable. Callers hold the write lock or have not published the table yet. */
static bool impl_insert(const TypeImpl **slots, Int cap, Int *len, const TypeImpl *r) {
    Int mask = cap - 1;
    Int i = (Int)(impl_hash(r->type, r->iface) & (uint64_t)mask);

    for (;;) {
        const TypeImpl *s = slots[i];
        if (s == NULL) {
            slots[i] = r;
            (*len)++;
            return true;
        }
        if (s->type == r->type && s->iface == r->iface)
            return s->vt == r->vt;
        i = (i + 1) & mask;
    }
}

/* Build a table of at least want slots and move what is there into it. */
static bool impl_grow(Int want) {
    Int cap = 8;
    while (cap < want)
        cap *= 2;
    const TypeImpl **slots =
        BURROW_NEW_N(heap_allocator(), const TypeImpl *, (size_t)cap);
    if (slots == NULL)
        return false;

    Int len = 0;
    for (Int i = 0; i < impl_cap; i++) {
        if (impl_slots[i] != NULL)
            (void)impl_insert(slots, cap, &len, impl_slots[i]);
    }

    const TypeImpl **old = impl_slots;
    Int old_cap = impl_cap;
    impl_slots = slots;
    impl_cap = cap;
    impl_len = len;
    if (old != NULL)
        mem_free(heap_allocator(), (void *)old, (size_t)old_cap * sizeof *old,
                 _Alignof(const TypeImpl *));
    return true;
}

/* Read the section and fill the table. Runs at most once, under the Once. Two
 * records for one pair with different vtables keep whichever came first, which
 * is as good an answer as there is to a program that said both. */
static void impl_build(void *env) {
    (void)env;

#if defined(BURROW__HAVE_IMPL_SECTION)
    const TypeImpl *const *first = BURROW__IMPL_BOUND(burrow__impl_sec_start);
    const TypeImpl *const *last = BURROW__IMPL_BOUND(burrow__impl_sec_stop);
    if (first == NULL || last == NULL)
        return;

    Int n = (Int)(last - first);
    if (n <= 0 || !impl_grow(n * 2))
        return;

    for (Int i = 0; i < n; i++) {
        const TypeImpl *r = first[i];
        if (r != NULL && r->type != NULL && r->iface != NULL && r->vt != NULL)
            (void)impl_insert(impl_slots, impl_cap, &impl_len, r);
    }
#endif
}

static void impl_ensure(void) {
    sync_once_do(&impl_once, BURROW_FN(Func, impl_build, NULL));
}

BURROW_SENTINEL_ERROR(
    type_err_impl_incomplete,
    "reflect: implements record without a type, an interface or a vtable");

BURROW_STATIC(ret) const void *type_vtable(const Type *t, const Type *iface) {
    if (t == NULL || iface == NULL)
        return NULL;

    impl_ensure();

    sync_rw_mutex_r_lock(&impl_lock);

    const void *vt = NULL;
    if (impl_cap > 0) {
        Int mask = impl_cap - 1;
        for (Int i = (Int)(impl_hash(t, iface) & (uint64_t)mask); impl_slots[i] != NULL;
             i = (i + 1) & mask) {
            if (impl_slots[i]->type == t && impl_slots[i]->iface == iface) {
                vt = impl_slots[i]->vt;
                break;
            }
        }
    }

    sync_rw_mutex_r_unlock(&impl_lock);
    return vt;
}

bool type_register_impl(const TypeImpl *impl, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);

    if (impl == NULL || impl->type == NULL || impl->iface == NULL || impl->vt == NULL) {
        BURROW_OUT(err, type_err_impl_incomplete);
        return false;
    }

    impl_ensure();

    sync_rw_mutex_lock(&impl_lock);

    bool room = true;
    if (impl_cap == 0 || (impl_len + 1) * 2 > impl_cap)
        room = impl_grow((impl_len + 1) * 4);
    bool ok = room && impl_insert(impl_slots, impl_cap, &impl_len, impl);

    sync_rw_mutex_unlock(&impl_lock);

    if (!room)
        BURROW_OUT(err, burrow_err_out_of_memory);
    else if (!ok)
        BURROW_OUT(err, type_err_conflict);
    return ok;
}

/* ------------------------------------------------------------------ anchor
 *
 * One entry so the section is never empty, as in registry.c. */
#if defined(BURROW__IMPL_SECTION) && !defined(_WIN32)
BURROW__IMPL_SECTION static const TypeImpl *const impl_anchor = NULL;
#endif
