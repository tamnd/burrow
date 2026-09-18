/* The allocator that checks the other allocators.
 *
 * It wraps one and passes everything through, and on the way it remembers what
 * it handed out. At the end it tells you what you did wrong.
 *
 *     Track tr;
 *     track_init(&tr, heap_allocator());
 *     Alloc *a = track_allocator(&tr);
 *
 *     ... run the thing under test, passing a ...
 *
 *     if (track_check(&tr) != 0)
 *         t_fail("memory faults");
 *     track_free(&tr);
 *
 * Six things get caught. A block still live when you call track_check is a
 * leak. Freeing a pointer that is not live is a double free. Freeing a pointer
 * this allocator never handed out is a wild free. Passing a size or an
 * alignment to free or realloc that does not match the one you allocated with
 * is a mismatch, and it matters here in a way it does not under malloc, because
 * burrow's allocators are told the size and the fast ones believe it. Writing
 * to a block after freeing it is a write after free.
 *
 * That last one is why freed memory is not handed straight back. A freed block
 * is filled with a poison byte and kept, and only when the quarantine is full
 * does the oldest one get checked against the poison and really released. So a
 * write after free is found late rather than immediately, and a read after free
 * is not found at all, because catching a read needs the page tables and this
 * needs to work everywhere. AddressSanitizer catches reads, this catches the
 * ownership mistakes ASan cannot see, and the two are complementary rather than
 * alternatives.
 *
 * This is a debugging allocator. It is what burrow's own tests run under, which
 * is how the ownership annotations in the headers get checked instead of merely
 * written. It is slow, it holds on to memory, and it does not belong in a
 * program you ship.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_TRACK_H
#define BURROW_MEM_TRACK_H

#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What went wrong. One of these arrives per fault, through the reporter if you
 * set one, and is counted either way. */
typedef enum TrackFault {
    TRACK_LEAK = 1,       /* still live when track_check ran */
    TRACK_DOUBLE_FREE,    /* freed a pointer that was already freed */
    TRACK_WILD_FREE,      /* freed a pointer this allocator never handed out */
    TRACK_SIZE_MISMATCH,  /* the size given to free is not the size allocated */
    TRACK_ALIGN_MISMATCH, /* likewise the alignment */
    TRACK_WRITE_AFTER_FREE
} TrackFault;

/* Everything known about one fault.
 *
 * size and align are what the block was allocated with, and claimed_size and
 * claimed_align are what the caller passed to free or realloc, so a mismatch
 * shows both numbers rather than leaving you to guess which one is wrong. On a
 * wild free there is no record, so the allocated pair is zero.
 *
 * seq is which allocation this was, counting from one. It is the number to put
 * in a conditional breakpoint when a leak report is all you have.
 *
 * file and line are the call site if the caller went through TRACK_HERE, and
 * NULL and zero if not. */
typedef struct TrackEvent {
    TrackFault fault;
    const void *p;
    size_t size;
    size_t align;
    size_t claimed_size;
    size_t claimed_align;
    uint64_t seq;
    const char *file;
    int line;
} TrackEvent;

/* Called once per fault as it is found. Do not allocate from the allocator
 * being tracked inside one. */
typedef void (*TrackReporter)(void *ctx, const TrackEvent *ev);

/* Fields are visible so the struct can live on the stack. Do not touch them. */
typedef struct Track {
    Alloc alloc;
    Alloc *under;
    Alloc *meta;

    void *slots;  /* open addressed, one entry per live or quarantined block */
    size_t cap;   /* a power of two, or zero before the first allocation */
    size_t used;  /* live plus quarantined, which is what the table holds */
    size_t tombs; /* slots a delete passed through, counted for the rehash */
    size_t live;

    void *quarantine_head; /* the oldest freed block still being held */
    void *quarantine_tail;
    size_t quarantine_bytes;
    size_t quarantine_limit;

    TrackReporter report;
    void *report_ctx;
    uint64_t faults;

    uint64_t seq;
    uint64_t bytes_live;
    uint64_t bytes_peak;
    uint64_t bytes_total;
    uint64_t allocs;
    uint64_t frees;

    const char *site_file; /* set by TRACK_HERE, consumed by the next alloc */
    int site_line;

    bool oom; /* the bookkeeping itself could not allocate */
} Track;

/* under is where the memory actually comes from and it has to outlive the
 * Track. It may be any allocator, including another Track, though there is no
 * reason to do that. */
void track_init(Track *tr, Alloc *under);

Alloc *track_allocator(Track *tr);

/* Where the bookkeeping is kept, which is not the allocator being tracked.
 * Records have to survive a reset of the thing underneath and must not show up
 * in its own statistics, so by default they come from the heap.
 *
 * Call this before the first allocation if the heap is not available to you,
 * which on a firmware target it will not be. */
void track_set_meta(Track *tr, Alloc *meta);

/* How much freed memory to hold on to before releasing the oldest, in bytes.
 * Larger finds a write after free that happens further away from the free, at
 * the cost of holding that much more memory. The default is one mebibyte.
 * Zero turns the quarantine off, which turns off write after free detection and
 * makes free a passthrough. */
void track_set_quarantine(Track *tr, size_t bytes);

/* Called for every fault. Without one, faults are still counted, and
 * track_faults is the whole report. */
void track_on_fault(Track *tr, TrackReporter fn, void *ctx);

/* Reports every block that is still live as a leak, then returns the total
 * number of faults seen over the Track's whole life, leaks included.
 *
 * Zero means the code under it allocated and freed correctly. Calling it twice
 * reports the same leaks twice, so call it once, at the end.
 *
 * It is a fault for the bookkeeping itself to run out of memory, because a
 * Track that could not record an allocation cannot honestly say there were no
 * leaks. That shows up in the return value with nothing else to point at. */
uint64_t track_check(Track *tr);

/* The count without the leak sweep, for a test that wants to look partway
 * through. */
uint64_t track_faults(const Track *tr);

/* How many blocks are live right now, which is the other thing a test asks. */
size_t track_live(const Track *tr);

/* Releases the quarantine, checking the poison on the way out, and frees the
 * bookkeeping. Blocks that are still live are not freed, because they are not
 * this allocator's to free and a leak is the caller's to fix. */
void track_free(Track *tr);

/* Records where the next allocation came from.
 *
 * Wrap the allocator at the call site and the leak report gains a file and a
 * line, which is usually the difference between a report you can act on and a
 * number:
 *
 *     Byte *p = mem_alloc(TRACK_HERE(a), n, 1);
 *
 * It returns the allocator it was given and does nothing at all unless that
 * allocator is a Track, so it is safe to leave in code that runs against a
 * plain arena. The note is stored on the Track and consumed by the next
 * allocation from it, which means it is only accurate when one thread is using
 * that Track. That is the normal case for a debugging allocator and it will get
 * better when the runtime brings thread local storage. */
#define TRACK_HERE(a) track_note((a), __FILE__, __LINE__)

Alloc *track_note(Alloc *a, const char *file, int line);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_TRACK_H */
