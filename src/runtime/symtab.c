/* Finding a name for an address.
 *
 * See burrow/symtab.h for what the table is and where it comes from. This file
 * is the lookup, which is a sorted array and a binary search, and the only two
 * interesting things about it are both about when it runs.
 *
 * The first is that it runs on the way out of a crash. The program asking for a
 * name is usually a program that has just panicked, so nothing here reads
 * through the address it was given, nothing here can loop forever on a table
 * that is somehow wrong, and running out of memory costs speed rather than the
 * answer.
 *
 * The second is that the sort cannot be done ahead of time. The generator knows
 * the names, and the linker decides the addresses, and those are two different
 * programs run at two different times. So the table arrives in whatever order
 * nm printed it and gets sorted here, once, the first time anybody asks.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/symtab.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How far past the start of a function an address is still allowed to be.
 *
 * The table has holes in it. Static functions are not in it, the code the
 * linker put between burrow's objects and the C library's is not in it, and
 * neither is the program that linked burrow in. Without a bound, an address
 * anywhere above the last function in the table would come back named after it,
 * which is a confident wrong answer and worse than no answer at all.
 *
 * So a hit has to be within a quarter of a megabyte of the name it got. Real
 * function bodies are nowhere near that, and where the next name in the table
 * is closer than this the gap between them is used instead, which is the
 * ordinary case and is exact. This only does any work at the edges. */
#define SYMBOL_MAX_SPAN ((Uintptr)1 << 18)

typedef struct SymtabEntry {
    Uintptr addr;
    const char *name;
} SymtabEntry;

/* The sorted copy, built once. NULL means either that nobody has asked yet or
 * that building it failed, and the lookup below reads it once into a local so
 * that it cannot see both answers in one call. */
static SyncOnce symtab_once;
static SymtabEntry *symtab_sorted;
static Int symtab_sorted_len;

/* Shell sort, with Knuth's gaps.
 *
 * Four hundred entries sorted once in the life of a process is not a place
 * where the choice of algorithm shows up in any measurement. What does show up
 * is that this one needs no second array and no stack, which is the property
 * worth having in code that a panicking program is about to run. */
static void sort_entries(SymtabEntry *a, Int n) {
    Int gap = 1;

    while (gap < n / 3)
        gap = gap * 3 + 1;

    for (; gap >= 1; gap /= 3) {
        for (Int i = gap; i < n; i++) {
            SymtabEntry v = a[i];
            Int j = i;

            while (j >= gap && a[j - gap].addr > v.addr) {
                a[j] = a[j - gap];
                j -= gap;
            }
            a[j] = v;
        }
    }
}

static void symtab_build(void *env) {
    (void)env;

    Int n = (Int)burrow__symbol_count;
    if (n <= 0)
        return;

    SymtabEntry *a = BURROW_NEW_N(heap_allocator(), SymtabEntry, (size_t)n);
    if (a == NULL)
        return;

    for (Int i = 0; i < n; i++) {
        a[i].addr = (Uintptr)burrow__symbol_addrs[i];
        a[i].name = burrow__symbol_names[i];
    }

    sort_entries(a, n);

    symtab_sorted = a;
    symtab_sorted_len = n;
}

static void symtab_ensure(void) {
    /* No retry if it failed. A table that could not be allocated on the way out
     * of memory is not going to allocate on the next line of the same
     * traceback, and the walk below answers the same question without it. */
    sync_once_do(&symtab_once, BURROW_FN(Func, symtab_build, NULL));
}

/* The last entry at or below pc, and where the one after it starts.
 *
 * Both halves come back together because the caller needs both to decide
 * whether the hit is real, and finding them apart would mean two searches. next
 * is zero when there is nothing above pc in the table.
 *
 * Duplicate addresses are stepped over rather than treated as a zero length
 * function. Two names for one address is an alias, which the toolchain produces
 * for things like a weak symbol and its real definition, and either name is a
 * correct answer. */
static bool find_sorted(const SymtabEntry *a, Int n, Uintptr pc, Int *at,
                        Uintptr *next) {
    Int lo = 0;
    Int hi = n;

    /* The first entry strictly above pc, so lo - 1 is the last one at or below
     * it. Written this way rather than as a search for pc itself because pc is
     * almost never the exact start of a function. */
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (a[mid].addr <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo == 0)
        return false;

    Int i = lo - 1;
    Int j = lo;
    while (j < n && a[j].addr == a[i].addr)
        j++;

    *at = i;
    *next = j < n ? a[j].addr : 0;
    return true;
}

/* The same question asked of the unsorted table, one entry at a time.
 *
 * This is what a build that could not allocate the sorted copy uses. It is the
 * whole table per frame rather than eight comparisons, which for the sixty four
 * frames of a traceback is a few tens of microseconds on a program that is
 * ending anyway. */
static bool find_linear(Uintptr pc, Uintptr *entry, const char **name, Uintptr *next) {
    Int n = (Int)burrow__symbol_count;
    bool found = false;

    *entry = 0;
    *name = NULL;
    *next = 0;

    for (Int i = 0; i < n; i++) {
        Uintptr addr = (Uintptr)burrow__symbol_addrs[i];

        if (addr <= pc) {
            if (!found || addr > *entry) {
                found = true;
                *entry = addr;
                *name = burrow__symbol_names[i];
            }
        } else if (*next == 0 || addr < *next) {
            *next = addr;
        }
    }

    return found;
}

bool burrow__symbolise(Uintptr pc, burrow__Frame *out) {
    Uintptr entry = 0;
    Uintptr next = 0;
    const char *name = NULL;

    if (out != NULL) {
        out->name = BURROW_STR_EMPTY;
        out->entry = 0;
    }
    if (out == NULL || pc == 0)
        return false;

    symtab_ensure();

    const SymtabEntry *a = symtab_sorted;
    Int n = symtab_sorted_len;

    if (a != NULL) {
        Int at = 0;
        if (!find_sorted(a, n, pc, &at, &next))
            return false;
        entry = a[at].addr;
        name = a[at].name;
    } else if (!find_linear(pc, &entry, &name, &next)) {
        return false;
    }

    /* How far the answer is allowed to be wrong by. The next name in the table
     * is the real end of this function whenever there is one, and the cap is
     * what stands in for it at the top of the table and across a gap the
     * linker left. */
    Uintptr limit = entry + SYMBOL_MAX_SPAN;
    if (limit < entry)
        limit = UINTPTR_MAX; /* the table's last function sits at the very top */
    if (next != 0 && next < limit)
        limit = next;

    if (pc >= limit)
        return false;

    out->name = str_from_cstr(name);
    out->entry = entry;
    return true;
}

Int burrow__symtab_len(void) {
    return (Int)burrow__symbol_count;
}
