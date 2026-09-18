/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/track.h"

#include "burrow/mem/heap.h"

#include <string.h>

/* Three decisions in here are worth reading before changing anything.
 *
 * The bookkeeping does not come from the allocator being tracked. It comes from
 * a separate one, the heap by default, because a record has to outlive a reset
 * of the thing underneath and because an allocator's own statistics should not
 * include the cost of watching it.
 *
 * A freed block is poisoned and held rather than released. That is the only way
 * to notice a write after free without the page tables, and it is why this
 * allocator holds on to a mebibyte the caller thinks it gave back.
 *
 * Realloc is always a fresh block and a copy, never a grow in place, even when
 * the allocator underneath could have grown it. Growing in place would leave the
 * old pointer valid, which hides the bug where somebody kept it, and the whole
 * point of this file is to stop hiding that class of thing. */

#define POISON 0xDF
#define MIN_CAP 64
#define DEFAULT_QUARANTINE ((size_t)1024 * 1024)

typedef struct Rec {
    void *p;
    size_t size;
    size_t align;
    uint64_t seq;
    const char *file;
    int line;
    bool freed; /* in the quarantine rather than live */
    struct Rec *qnext;
} Rec;

/* A deleted slot has to be distinguishable from an empty one, or the probe that
 * walks past it stops early and loses everything behind it. The marker is the
 * address of a real object rather than a small integer cast to a pointer,
 * because that is defined and the cast is not. */
static Rec tomb;
#define TOMB (&tomb)

/* ---------------------------------------------------------------- the table */

static size_t hash_ptr(const void *p) {
    /* The finaliser from MurmurHash3, which is the cheapest thing that makes
     * the low bits of a pointer worth indexing on. Allocations differ in the
     * low bits by their alignment and in the high bits by almost nothing, so
     * the raw value clusters badly in a power of two table. */
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

static Rec *table_get(const Track *tr, const void *p) {
    if (tr->cap == 0)
        return NULL;

    Rec *const *slots = (Rec *const *)tr->slots;
    size_t mask = tr->cap - 1;
    size_t i = hash_ptr(p) & mask;

    for (size_t n = 0; n <= mask; n++) {
        Rec *r = slots[i];
        if (r == NULL)
            return NULL;
        if (r != TOMB && r->p == p)
            return r;
        i = (i + 1) & mask;
    }
    return NULL;
}

/* The caller has already established that this pointer is not in the table, and
 * table_reserve has already made room, so this cannot fail and does not loop
 * forever. */
static void table_put(Track *tr, Rec *rec) {
    Rec **slots = (Rec **)tr->slots;
    size_t mask = tr->cap - 1;
    size_t i = hash_ptr(rec->p) & mask;

    while (slots[i] != NULL && slots[i] != TOMB)
        i = (i + 1) & mask;
    if (slots[i] == TOMB)
        tr->tombs--;
    slots[i] = rec;
    tr->used++;
}

static void table_del(Track *tr, const void *p) {
    if (tr->cap == 0)
        return;

    Rec **slots = (Rec **)tr->slots;
    size_t mask = tr->cap - 1;
    size_t i = hash_ptr(p) & mask;

    for (size_t n = 0; n <= mask; n++) {
        Rec *r = slots[i];
        if (r == NULL)
            return;
        if (r != TOMB && r->p == p) {
            slots[i] = TOMB;
            tr->used--;
            tr->tombs++;
            return;
        }
        i = (i + 1) & mask;
    }
}

/* Makes room for one more entry, rebuilding when three quarters of the table is
 * spoken for. Tombstones count towards that, because a probe walks past them
 * and a table full of nothing but deleted slots is a linear scan wearing a hash
 * table's clothes. Rebuilding is what gets rid of them, and it can land on a
 * smaller table than the one before it when almost everything was a tombstone.
 * Returns false only when the bookkeeping allocator is out of memory. */
static bool table_reserve(Track *tr) {
    if (tr->cap != 0 && (tr->used + tr->tombs + 1) * 4 < tr->cap * 3)
        return true;

    size_t want = MIN_CAP;
    while (want < (tr->used + 1) * 2)
        want *= 2;

    Rec **fresh =
        (Rec **)mem_alloc_array(tr->meta, want, sizeof(Rec *), _Alignof(Rec *));
    if (fresh == NULL)
        return false;

    Rec **old = (Rec **)tr->slots;
    size_t old_cap = tr->cap;

    tr->slots = (void *)fresh;
    tr->cap = want;
    tr->used = 0;
    tr->tombs = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i] != NULL && old[i] != TOMB)
            table_put(tr, old[i]);
    }
    if (old != NULL)
        mem_free(tr->meta, (void *)old, old_cap * sizeof(Rec *), _Alignof(Rec *));
    return true;
}

/* --------------------------------------------------------------- reporting */

static void fault(Track *tr, TrackFault f, const void *p, const Rec *r,
                  size_t claimed_size, size_t claimed_align) {
    TrackEvent ev;

    ev.fault = f;
    ev.p = p;
    ev.size = r != NULL ? r->size : 0;
    ev.align = r != NULL ? r->align : 0;
    ev.claimed_size = claimed_size;
    ev.claimed_align = claimed_align;
    ev.seq = r != NULL ? r->seq : 0;
    ev.file = r != NULL ? r->file : NULL;
    ev.line = r != NULL ? r->line : 0;

    tr->faults++;
    if (tr->report != NULL)
        tr->report(tr->report_ctx, &ev);
}

/* Counted once however many records it costs us, because the first one is the
 * whole of the news: from here on the report is no longer complete. */
static void note_oom(Track *tr) {
    if (!tr->oom) {
        tr->oom = true;
        tr->faults++;
    }
}

/* ------------------------------------------------------------- quarantine */

static void check_poison(Track *tr, const Rec *r) {
    const unsigned char *b = (const unsigned char *)r->p;
    for (size_t i = 0; i < r->size; i++) {
        if (b[i] != POISON) {
            fault(tr, TRACK_WRITE_AFTER_FREE, r->p, r, 0, 0);
            return;
        }
    }
}

static void drop_rec(Track *tr, Rec *r) {
    table_del(tr, r->p);
    mem_free(tr->meta, r, sizeof *r, _Alignof(Rec));
}

/* Releases the oldest held block for real, after asking whether anybody wrote
 * to it while we were holding it. */
static void evict_oldest(Track *tr) {
    Rec *r = (Rec *)tr->quarantine_head;
    if (r == NULL)
        return;

    tr->quarantine_head = r->qnext;
    if (tr->quarantine_head == NULL)
        tr->quarantine_tail = NULL;
    tr->quarantine_bytes -= r->size;

    check_poison(tr, r);
    mem_free(tr->under, r->p, r->size, r->align);
    drop_rec(tr, r);
}

/* The accounting and the disposal that a free and the free half of a realloc
 * both do, once the record has been found and checked. */
static void release(Track *tr, Rec *r) {
    tr->frees++;
    tr->live--;
    tr->bytes_live -= r->size;

    if (tr->quarantine_limit == 0) {
        mem_free(tr->under, r->p, r->size, r->align);
        drop_rec(tr, r);
        return;
    }

    memset(r->p, POISON, r->size);
    r->freed = true;
    r->qnext = NULL;
    if (tr->quarantine_tail != NULL)
        ((Rec *)tr->quarantine_tail)->qnext = r;
    else
        tr->quarantine_head = r;
    tr->quarantine_tail = r;
    tr->quarantine_bytes += r->size;

    while (tr->quarantine_bytes > tr->quarantine_limit)
        evict_oldest(tr);
}

/* ------------------------------------------------------------- the vtable */

/* Takes the call site left by the last TRACK_HERE and clears it, so that a note
 * belongs to exactly one allocation and a later one without a note is honestly
 * blank rather than quietly inheriting the one before it. */
static void take_site(Track *tr, Rec *r) {
    r->file = tr->site_file;
    r->line = tr->site_line;
    tr->site_file = NULL;
    tr->site_line = 0;
}

static bool record(Track *tr, void *p, size_t size, size_t align) {
    Rec *r;

    if (!table_reserve(tr))
        return false;
    r = BURROW_NEW(tr->meta, Rec);
    if (r == NULL)
        return false;

    r->p = p;
    r->size = size;
    r->align = align;
    r->seq = ++tr->seq;
    r->freed = false;
    r->qnext = NULL;
    take_site(tr, r);
    table_put(tr, r);

    tr->allocs++;
    tr->live++;
    tr->bytes_live += size;
    tr->bytes_total += size;
    if (tr->bytes_live > tr->bytes_peak)
        tr->bytes_peak = tr->bytes_live;
    return true;
}

/* When the bookkeeping cannot be written the allocation fails, rather than
 * succeeding untracked. An untracked block turns into a wild free later and
 * puts a fault in the report that the caller did not cause, and a report with
 * invented faults in it is worse than an allocator that said no. */
static void *hand_out(Track *tr, size_t size, size_t align, bool zeroed) {
    void *p = zeroed ? mem_alloc(tr->under, size, align)
                     : mem_alloc_nozero(tr->under, size, align);
    if (p == NULL)
        return NULL;
    if (!record(tr, p, size, align)) {
        mem_free(tr->under, p, size, align);
        note_oom(tr);
        return NULL;
    }
    return p;
}

static void *track_vt_alloc(void *self, size_t size, size_t align) {
    return hand_out((Track *)self, size, align, false);
}

static void *track_vt_alloc_zeroed(void *self, size_t size, size_t align) {
    return hand_out((Track *)self, size, align, true);
}

/* Finds the record for a pointer somebody is giving back, and says whether it
 * is usable. Everything that can be wrong with a free is decided here, so that
 * free and realloc cannot disagree about what counts. */
static Rec *claim(Track *tr, void *p, size_t size, size_t align) {
    Rec *r = table_get(tr, p);

    if (r == NULL) {
        fault(tr, TRACK_WILD_FREE, p, NULL, size, align);
        return NULL;
    }
    if (r->freed) {
        fault(tr, TRACK_DOUBLE_FREE, p, r, size, align);
        return NULL;
    }
    /* Both are reported and neither stops the free, because the block is real
     * and the caller's numbers are the only thing wrong with it. The recorded
     * pair is what gets passed down, since that is the one the allocator
     * underneath was told. */
    if (r->size != size)
        fault(tr, TRACK_SIZE_MISMATCH, p, r, size, align);
    if (r->align != align)
        fault(tr, TRACK_ALIGN_MISMATCH, p, r, size, align);
    return r;
}

static void track_vt_free(void *self, void *p, size_t size, size_t align) {
    Track *tr = (Track *)self;
    Rec *r;

    if (p == NULL)
        return;
    r = claim(tr, p, size, align);
    if (r == NULL)
        return;
    release(tr, r);
}

static void *track_vt_realloc(void *self, void *p, size_t old, size_t nsz,
                              size_t align) {
    Track *tr = (Track *)self;
    Rec *r;
    void *q;

    if (p == NULL)
        return hand_out(tr, nsz, align, false);
    if (nsz == 0) {
        track_vt_free(self, p, old, align);
        return NULL;
    }

    r = claim(tr, p, old, align);
    if (r == NULL)
        return NULL;

    /* A failed grow has to leave the old block alone, which is realloc's
     * contract everywhere and is the reason the new block comes first. */
    q = mem_alloc_nozero(tr->under, nsz, align);
    if (q == NULL)
        return NULL;
    memcpy(q, p, r->size < nsz ? r->size : nsz);

    if (!record(tr, q, nsz, align)) {
        mem_free(tr->under, q, nsz, align);
        note_oom(tr);
        return NULL;
    }
    release(tr, r);
    return q;
}

/* A reset is the caller giving everything back at once, which is a legitimate
 * way to free and therefore not a leak. Nothing can be checked against its
 * poison on the way out, because after this the memory is not ours to read. */
static void track_vt_reset(void *self) {
    Track *tr = (Track *)self;
    Rec **slots = (Rec **)tr->slots;

    for (size_t i = 0; i < tr->cap; i++) {
        if (slots[i] != NULL && slots[i] != TOMB)
            mem_free(tr->meta, slots[i], sizeof(Rec), _Alignof(Rec));
        slots[i] = NULL;
    }
    tr->used = 0;
    tr->tombs = 0;
    tr->live = 0;
    tr->bytes_live = 0;
    tr->quarantine_head = NULL;
    tr->quarantine_tail = NULL;
    tr->quarantine_bytes = 0;

    mem_reset(tr->under);
}

static AllocStats track_vt_stats(void *self) {
    Track *tr = (Track *)self;
    AllocStats s;

    s.bytes_live = tr->bytes_live;
    s.bytes_peak = tr->bytes_peak;
    s.bytes_total = tr->bytes_total;
    s.allocs = tr->allocs;
    s.frees = tr->frees;
    s.blocks = (uint64_t)tr->live;
    return s;
}

/* Two of them, so that mem_can_reset tells the truth. A Track over an arena can
 * be reset and a Track over the heap cannot, and which one you have is known at
 * init. */
static const AllocVT track_vt = {
    track_vt_alloc, track_vt_alloc_zeroed, track_vt_realloc, track_vt_free,
    NULL,           track_vt_stats,
};

static const AllocVT track_vt_resettable = {
    track_vt_alloc, track_vt_alloc_zeroed, track_vt_realloc,
    track_vt_free,  track_vt_reset,        track_vt_stats,
};

/* ----------------------------------------------------------------- the API */

void track_init(Track *tr, Alloc *under) {
    memset(tr, 0, sizeof *tr);
    tr->under = under;
    tr->meta = heap_allocator();
    tr->quarantine_limit = DEFAULT_QUARANTINE;
    tr->alloc.vt = mem_can_reset(under) ? &track_vt_resettable : &track_vt;
    tr->alloc.self = tr;
}

Alloc *track_allocator(Track *tr) {
    return &tr->alloc;
}

void track_set_meta(Track *tr, Alloc *meta) {
    tr->meta = meta;
}

void track_set_quarantine(Track *tr, size_t bytes) {
    tr->quarantine_limit = bytes;
    while (tr->quarantine_bytes > tr->quarantine_limit)
        evict_oldest(tr);
}

void track_on_fault(Track *tr, TrackReporter fn, void *ctx) {
    tr->report = fn;
    tr->report_ctx = ctx;
}

uint64_t track_faults(const Track *tr) {
    return tr->faults;
}

size_t track_live(const Track *tr) {
    return tr->live;
}

uint64_t track_check(Track *tr) {
    Rec **slots = (Rec **)tr->slots;

    for (size_t i = 0; i < tr->cap; i++) {
        Rec *r = slots[i];
        if (r != NULL && r != TOMB && !r->freed)
            fault(tr, TRACK_LEAK, r->p, r, 0, 0);
    }
    return tr->faults;
}

void track_free(Track *tr) {
    Rec **slots;

    while (tr->quarantine_head != NULL)
        evict_oldest(tr);

    /* What is left is live, and live memory belongs to whoever did not free it.
     * Releasing it here would turn a leak this allocator just reported into a
     * use after free somewhere else, which is a worse bug than the one being
     * complained about. */
    slots = (Rec **)tr->slots;
    for (size_t i = 0; i < tr->cap; i++) {
        if (slots[i] != NULL && slots[i] != TOMB)
            mem_free(tr->meta, slots[i], sizeof(Rec), _Alignof(Rec));
    }
    if (slots != NULL)
        mem_free(tr->meta, (void *)slots, tr->cap * sizeof(Rec *), _Alignof(Rec *));

    tr->slots = NULL;
    tr->cap = 0;
    tr->used = 0;
    tr->tombs = 0;
    tr->live = 0;
}

Alloc *track_note(Alloc *a, const char *file, int line) {
    if (a != NULL && (a->vt == &track_vt || a->vt == &track_vt_resettable)) {
        Track *tr = (Track *)a->self;
        tr->site_file = file;
        tr->site_line = line;
    }
    return a;
}
