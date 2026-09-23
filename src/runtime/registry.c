/* The type registry: a name in, a descriptor out.
 *
 * Reflection on a value you are holding needs none of this. The descriptor is a
 * static object, you have its address, and that is the whole of it. What this
 * file is for is the other direction, where a program has a type's name and
 * nothing else: encoding/gob reading a name off a connection, net/rpc being
 * told the type of an argument, text/template resolving a method.
 *
 * TWO WAYS IN, AND WHY THE FIRST ONE IS FREE
 *
 * BURROW_REGISTER_TYPE puts one pointer in a section of its own. The linker
 * gathers them, puts a symbol either side, and the registry reads the range the
 * first time anybody looks a name up. Nothing runs before main, nothing is
 * allocated for a program that never asks, and the pointers cost what they are,
 * which is eight bytes each in a part of the image that is already there.
 *
 * type_register is the other way, for a descriptor that arrived after startup
 * in a module loaded at runtime. The section of a shared library is not in the
 * range the main image's symbols cover, so a module that wants its types known
 * calls this for them.
 *
 * WHAT THE TABLE IS
 *
 * Open addressed, linear probing, power of two, built once under a Once and
 * then read under a read lock. The read lock is there for the second way in
 * rather than the first: without runtime registration this would be built once
 * and never written again, and the lock could go.
 *
 * The key is the qualified name and it is never materialised. A descriptor
 * holds the package path and the name separately, joining them would be a
 * string allocated per type for a question almost nobody asks, and a lookup can
 * split the name it was given instead. The split is at the last dot, because a
 * package path can contain dots and a type name cannot.
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
 * The symbols either side of it. On ELF the linker defines __start_NAME and
 * __stop_NAME for any section whose name is a C identifier, and they are
 * declared weak here so that a program registering nothing still links. On
 * Mach-O the same service is spelled as a pair of magic symbol names that have
 * to be attached with an assembler label. On PE there is no such service, so
 * the two ends are ordinary objects in sections that sort either side of the
 * one the entries go in.
 *
 * The anchor at the bottom of this file is what makes sure the section exists
 * at all, which matters on Mach-O, where asking for the bounds of a section
 * that is not there does not link. */

#if defined(__APPLE__) && defined(__GNUC__)

__extension__ extern const Type *const
    burrow__type_sec_start[] __asm("section$start$__DATA$__burrowtype");
__extension__ extern const Type *const
    burrow__type_sec_stop[] __asm("section$end$__DATA$__burrowtype");
#define BURROW__HAVE_TYPE_SECTION 1

#elif defined(_WIN32) && (defined(__GNUC__) || defined(_MSC_VER))

/* PE has no start and stop symbols, so the two ends are objects of our own in
 * sections that sort either side of the one the entries land in. The linker
 * orders sections whose names differ only after a dollar sign and then drops
 * the suffix, so .brwt$a, .brwt$b and .brwt$z become one .brwt section with
 * everything in that order. This is how the C runtime's own initialiser lists
 * have worked for thirty years. */
#if defined(_MSC_VER)
#pragma section(".brwt$a", read)
#pragma section(".brwt$z", read)
__declspec(allocate(".brwt$a")) static const Type *const burrow__type_sec_first = NULL;
__declspec(allocate(".brwt$z")) static const Type *const burrow__type_sec_last = NULL;
#else
/* No redzone on these two either, for the reason given at BURROW__TYPE_SECTION.
 * The start bound is one past the first object, so padding after it would put
 * the walk in the wrong place before it had read anything. */
BURROW__TYPE_NO_REDZONE __attribute__((
    used, section(".brwt$a"))) static const Type *const burrow__type_sec_first = NULL;
BURROW__TYPE_NO_REDZONE __attribute__((
    used, section(".brwt$z"))) static const Type *const burrow__type_sec_last = NULL;
#endif
#define burrow__type_sec_start (&burrow__type_sec_first + 1)
#define burrow__type_sec_stop (&burrow__type_sec_last)
#define BURROW__HAVE_TYPE_SECTION 1

#elif defined(__ELF__) && defined(__GNUC__)

extern const Type *const __start_burrowtype[] __attribute__((weak));
extern const Type *const __stop_burrowtype[] __attribute__((weak));
#define burrow__type_sec_start __start_burrowtype
#define burrow__type_sec_stop __stop_burrowtype
#define BURROW__HAVE_TYPE_SECTION 1

#endif

/* Takes an address the compiler thinks it understands and hands back the same
 * address with nothing known about it.
 *
 * The whole point of the section is that the linker puts things between the two
 * ends, which happens after the compiler has stopped looking. On PE the start
 * bound is one past an eight byte object of ours, so a compiler reading the
 * loop below sees a walk off the end of that object and says so. gcc 12 and
 * later say it as -Warray-bounds, which under -Werror is a build that fails at
 * a walk that is correct.
 *
 * An empty asm with the pointer as an in and out operand is the usual way to
 * say this. It emits nothing, and the compiler has to assume the value came
 * back different, so everything it knew about where the address points stops
 * being true. The other spellings do not work: a volatile local still has its
 * provenance, and turning the warning off for the file turns it off for code
 * that would deserve it. */
#if defined(__GNUC__)
#define BURROW__SEC_BOUND(p)                                                           \
    (__extension__({                                                                   \
        const Type *const *burrow__b = (p);                                            \
        __asm__("" : "+r"(burrow__b));                                                 \
        burrow__b;                                                                     \
    }))
#else
#define BURROW__SEC_BOUND(p) (p)
#endif

/* --------------------------------------------------------------- the table */

typedef struct Slot {
    const Type *t;
    uint64_t h;
} Slot;

static SyncOnce registry_once;
static SyncRWMutex registry_lock;
static Slot *registry_slots;
static Int registry_cap; /* a power of two, or zero when there is no table */
static Int registry_len;

/* FNV-1a over the package path, a dot, and the name.
 *
 * Not the hash the rest of burrow uses, and deliberately. That one wants a
 * contiguous buffer, the two halves of a qualified name are never contiguous,
 * and copying them together to hash them would undo the reason they are kept
 * apart. FNV is a byte at a time, which is slower per byte and does not matter:
 * this runs once per name looked up, names are short, and the thing on the
 * other side of the call is usually a network decoder.
 *
 * The empty package path still hashes the dot, so that a type named "Point"
 * with no package and one named "" in package "Point" are different, which they
 * are. */
static uint64_t name_hash(Str pkg, Str name) {
    uint64_t h = 1469598103934665603U;

    for (Int i = 0; i < pkg.len; i++) {
        h ^= pkg.p[i];
        h *= 1099511628211U;
    }
    h ^= (unsigned char)'.';
    h *= 1099511628211U;
    for (Int i = 0; i < name.len; i++) {
        h ^= name.p[i];
        h *= 1099511628211U;
    }

    return h;
}

/* Split "image.Point" into "image" and "Point", at the last dot.
 *
 * The last and not the first, because a real package path is
 * "github.com/x/y" and has dots of its own. A type name cannot contain one, so
 * the last dot is always the separator. */
static void split_name(Str q, Str *pkg, Str *name) {
    Int dot = -1;

    for (Int i = q.len - 1; i >= 0; i--) {
        if (q.p[i] == '.') {
            dot = i;
            break;
        }
    }

    if (dot < 0) {
        *pkg = BURROW_STR_EMPTY;
        *name = q;
        return;
    }

    pkg->p = q.p;
    pkg->len = dot;
    name->p = q.p + dot + 1;
    name->len = q.len - dot - 1;
}

static bool slot_matches(const Slot *s, Str pkg, Str name) {
    return s->t != NULL && str_eq(s->t->pkg_path, pkg) && str_eq(s->t->name, name);
}

/* Put t in, or find that it is already there. Callers hold the write lock or
 * are still building the table and have not published it. */
static bool table_insert(Slot *slots, Int cap, Int *len, const Type *t) {
    uint64_t h = name_hash(t->pkg_path, t->name);
    Int mask = cap - 1;
    Int i = (Int)(h & (uint64_t)mask);

    for (;;) {
        if (slots[i].t == NULL) {
            slots[i].t = t;
            slots[i].h = h;
            (*len)++;
            return true;
        }
        if (slots[i].h == h && slot_matches(&slots[i], t->pkg_path, t->name)) {
            /* Already registered. The same descriptor is the ordinary case, the
             * same type through two shared libraries is the interesting one,
             * and a different type under one name is the one to refuse. */
            return type_same(slots[i].t, t);
        }
        i = (i + 1) & mask;
    }
}

static Int round_up_pow2(Int n) {
    Int c = 8;

    while (c < n)
        c *= 2;
    return c;
}

/* Build a table of at least want slots and move what is there into it. */
static bool table_grow(Int want) {
    Int cap = round_up_pow2(want);
    Slot *slots = BURROW_NEW_N(heap_allocator(), Slot, (size_t)cap);
    if (slots == NULL)
        return false;

    Int len = 0;
    for (Int i = 0; i < registry_cap; i++) {
        if (registry_slots[i].t != NULL)
            (void)table_insert(slots, cap, &len, registry_slots[i].t);
    }

    Slot *old = registry_slots;
    Int old_cap = registry_cap;
    registry_slots = slots;
    registry_cap = cap;
    registry_len = len;
    if (old != NULL)
        mem_free(heap_allocator(), old, (size_t)old_cap * sizeof(Slot), _Alignof(Slot));

    return true;
}

/* Read the section and fill the table. Runs at most once, under the Once. */
static void registry_build(void *env) {
    (void)env;

#if defined(BURROW__HAVE_TYPE_SECTION)
    const Type *const *first = BURROW__SEC_BOUND(burrow__type_sec_start);
    const Type *const *last = BURROW__SEC_BOUND(burrow__type_sec_stop);
    if (first == NULL || last == NULL)
        return;

    Int n = (Int)(last - first);
    if (n <= 0)
        return;

    /* Half full at most. Linear probing gets slow before it gets full, and the
     * table is built once and read for the life of the program, so the memory
     * is the cheap side of the trade. */
    if (!table_grow(n * 2))
        return;

    for (Int i = 0; i < n; i++) {
        if (first[i] != NULL)
            (void)table_insert(registry_slots, registry_cap, &registry_len, first[i]);
    }
#endif
}

static void registry_ensure(void) {
    sync_once_do(&registry_once, BURROW_FN(Func, registry_build, NULL));
}

/* ------------------------------------------------------------------ the wall
 *
 * Three sentinels, so that a caller who gets nothing back learns which nothing
 * it was. They cost two words of read only data each and no code, and none of
 * the three can fail to be constructed, which matters because two of them are
 * produced on paths a program under memory pressure is already having a bad
 * day on. */

BURROW_SENTINEL_ERROR(type_err_not_registered, "reflect: type not registered");
BURROW_SENTINEL_ERROR(type_err_name_invalid, "reflect: not a type name");
BURROW_SENTINEL_ERROR(
    type_err_conflict,
    "reflect: a different type is already registered under this name");

/* ------------------------------------------------------------------- lookup */

BURROW_STATIC(ret) const Type *type_by_name(Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);

    if (name.p == NULL || name.len <= 0) {
        BURROW_OUT(err, type_err_name_invalid);
        return NULL;
    }

    registry_ensure();

    Str pkg;
    Str base;
    split_name(name, &pkg, &base);

    /* A dot that separates nothing from something, or something from nothing.
     * "image." names no type and ".Point" claims a package and then does not
     * name one, and both of those arrive from a decoder that read a length
     * wrong rather than from a program that has a type in mind. Telling them
     * apart from a name that is merely unknown is the difference between a
     * corrupt stream and a version mismatch, and those get fixed by different
     * people. */
    if (base.len <= 0 || (base.len < name.len && pkg.len <= 0)) {
        BURROW_OUT(err, type_err_name_invalid);
        return NULL;
    }

    uint64_t h = name_hash(pkg, base);

    sync_rw_mutex_r_lock(&registry_lock);

    const Type *found = NULL;
    if (registry_cap > 0) {
        Int mask = registry_cap - 1;
        for (Int i = (Int)(h & (uint64_t)mask); registry_slots[i].t != NULL;
             i = (i + 1) & mask) {
            if (registry_slots[i].h == h &&
                slot_matches(&registry_slots[i], pkg, base)) {
                found = registry_slots[i].t;
                break;
            }
        }
    }

    sync_rw_mutex_r_unlock(&registry_lock);

    if (found == NULL)
        BURROW_OUT(err, type_err_not_registered);

    return found;
}

bool type_register(const Type *t, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);

    /* No name, nothing to register it under. Saying so beats returning true
     * for a call that put nothing in the table. */
    if (t == NULL || t->name.len <= 0 || t->name.p == NULL) {
        BURROW_OUT(err, type_err_name_invalid);
        return false;
    }

    registry_ensure();

    sync_rw_mutex_lock(&registry_lock);

    bool room;
    if (registry_cap == 0 || (registry_len + 1) * 2 > registry_cap)
        room = table_grow((registry_len + 1) * 4);
    else
        room = true;

    bool ok = room && table_insert(registry_slots, registry_cap, &registry_len, t);

    sync_rw_mutex_unlock(&registry_lock);

    /* The two failures are not the same failure. Out of memory is worth
     * retrying and a conflict is worth fixing, and a plugin loader that got a
     * bare false could do nothing sensible with either. table_insert only
     * fails by refusing, since the grow above guarantees it a free slot. */
    if (!room)
        BURROW_OUT(err, burrow_err_out_of_memory);
    else if (!ok)
        BURROW_OUT(err, type_err_conflict);

    return ok;
}

Int type_registry_len(void) {
    registry_ensure();

    sync_rw_mutex_r_lock(&registry_lock);
    Int n = registry_len;
    sync_rw_mutex_r_unlock(&registry_lock);

    return n;
}

/* ----------------------------------------------------------------- identity */

bool type_same(const Type *a, const Type *b) {
    if (a == b)
        return true;
    if (a == NULL || b == NULL)
        return false;

    /* Two addresses and no name to go on. Nothing more can be said: an unnamed
     * type has no identity outside the program it was compiled into. */
    if (a->name.len <= 0 || b->name.len <= 0)
        return false;

    return a->kind == b->kind && a->size == b->size && str_eq(a->name, b->name) &&
           str_eq(a->pkg_path, b->pkg_path);
}

BURROW_BORROWS(ret, buf) Str type_qualified_name(const Type *t, Byte *buf, Int cap) {
    if (buf == NULL || cap <= 0)
        return BURROW_STR_EMPTY;
    if (t == NULL)
        return BURROW_STR_EMPTY;

    /* Measured first, because half a name is a name. "image.Point" cut to
     * "image.Poi" is still something type_by_name will take, and it will either
     * find nothing or find the wrong thing, with no way for anyone downstream
     * to tell that the buffer was the problem. Nothing at all is the answer a
     * caller can act on. */
    Int want = t->name.len + (t->pkg_path.len > 0 ? t->pkg_path.len + 1 : 0);
    if (want > cap)
        return BURROW_STR_EMPTY;

    Int n = 0;
    for (Int i = 0; i < t->pkg_path.len; i++)
        buf[n++] = t->pkg_path.p[i];
    if (t->pkg_path.len > 0)
        buf[n++] = (Byte)'.';
    for (Int i = 0; i < t->name.len; i++)
        buf[n++] = t->name.p[i];

    return (Str){buf, n};
}

/* ------------------------------------------------------------------ anchor
 *
 * One entry so the section is never empty, which is what makes asking for its
 * bounds safe. It is a NULL and the walk skips NULLs. Not needed on PE, where
 * the two ends above are objects and the section exists because they do. */
#if defined(BURROW__TYPE_SECTION) && !defined(_WIN32)
BURROW__TYPE_SECTION static const Type *const burrow__type_anchor = NULL;
#endif
