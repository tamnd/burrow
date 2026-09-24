/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"

#include <stdint.h>

/* Every test here does the wrong thing on purpose and then asks whether it was
 * noticed, which means the thing under test has to be told about the fault
 * rather than allowed to shout about it. That is what the reporter is for: the
 * last event lands in a struct and the test reads fields out of it, so a failure
 * says which fault was expected and which one arrived instead of leaving
 * somebody to read a log. */

typedef struct Log {
    int n;
    TrackEvent last;
} Log;

static void on_fault(void *ctx, const TrackEvent *ev) {
    Log *log = (Log *)ctx;
    log->n++;
    log->last = *ev;
}

static void start(Track *tr, Log *log) {
    log->n = 0;
    track_init(tr, heap_allocator());
    track_on_fault(tr, on_fault, log);
}

/* The happy path first, because a checker that reports faults on correct code is
 * worse than no checker at all. Nobody keeps running one that cries wolf. */

static void TestTrackIsQuietWhenNothingIsWrong(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 64, 8);
    void *q = mem_alloc(a, 3, 1);
    CHECK(p != NULL);
    CHECK(q != NULL);
    CHECK_INT_EQ(track_live(&tr), 2);

    mem_free(a, p, 64, 8);
    mem_free(a, q, 3, 1);
    CHECK_INT_EQ(track_live(&tr), 0);

    CHECK_INT_EQ(track_check(&tr), 0);
    CHECK_INT_EQ(log.n, 0);
    track_free(&tr);
}

/* The allocator underneath is still doing its job, so the interface rules have
 * to survive the wrapping. Zeroing is the one most easily lost, since a wrapper
 * that only implements alloc and not alloc_zeroed silently stops arenas from
 * skipping the memset. */

static void TestTrackPassesTheInterfaceThrough(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    unsigned char *p = (unsigned char *)mem_alloc(a, 128, 16);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p & 15u) == 0);
    for (int i = 0; i < 128; i++)
        CHECK(p[i] == 0);

    /* Grown past the old size, so the tail has to come back zeroed too. */
    memset(p, 0xAB, 128);
    p = (unsigned char *)mem_realloc(a, p, 128, 256, 16);
    CHECK(p != NULL);
    CHECK(p[0] == 0xAB);
    CHECK(p[127] == 0xAB);
    for (int i = 128; i < 256; i++)
        CHECK(p[i] == 0);

    mem_free(a, p, 256, 16);
    CHECK_INT_EQ(track_check(&tr), 0);
    track_free(&tr);
}

static void TestTrackReportsALeak(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *keep = mem_alloc(TRACK_HERE(a), 40, 8);
    void *give_back = mem_alloc(a, 40, 8);
    mem_free(a, give_back, 40, 8);

    CHECK_INT_EQ(track_check(&tr), 1);
    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_LEAK);
    CHECK(log.last.p == keep);
    CHECK_INT_EQ(log.last.size, 40);
    CHECK_INT_EQ(log.last.seq, 1);

    /* TRACK_HERE was on that call and nothing else, so the leak knows where it
     * came from and the block that was freed correctly never asked. */
    CHECK(log.last.file != NULL);
    CHECK(log.last.line > 0);

    /* The leaked block is still the caller's, so cleaning up after the test is
     * the caller's job and doing it here would be the double free the next test
     * is about. */
    mem_free(a, keep, 40, 8);
    track_free(&tr);
}

static void TestTrackReportsADoubleFree(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 32, 8);
    mem_free(a, p, 32, 8);
    mem_free(a, p, 32, 8);

    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_DOUBLE_FREE);
    CHECK(log.last.p == p);
    CHECK_INT_EQ(track_check(&tr), 1);
    track_free(&tr);
}

static void TestTrackReportsAWildFree(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    /* A block from a different allocator, which is the shape this bug has in
     * real code: two allocators in one function and the wrong one at the end. */
    void *stranger = mem_alloc(heap_allocator(), 16, 8);
    mem_free(a, stranger, 16, 8);

    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_WILD_FREE);
    CHECK(log.last.p == stranger);
    CHECK_INT_EQ(log.last.claimed_size, 16);
    CHECK_INT_EQ(log.last.size, 0); /* there is no record, so nothing to report */

    mem_free(heap_allocator(), stranger, 16, 8);
    track_free(&tr);
}

/* The size and alignment a caller passes to free are not decoration. The arena
 * moves its bump pointer back by that number and the heap picks which of two
 * platform functions to call from it, so a wrong one is a real bug that happens
 * to work under malloc and corrupts an arena. */

static void TestTrackReportsASizeMismatch(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 100, 8);
    mem_free(a, p, 99, 8);

    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_SIZE_MISMATCH);
    CHECK_INT_EQ(log.last.size, 100);
    CHECK_INT_EQ(log.last.claimed_size, 99);

    /* The block was still freed, using the size that was recorded rather than
     * the one that was claimed, so this is one fault and not a leak as well. */
    CHECK_INT_EQ(track_live(&tr), 0);
    CHECK_INT_EQ(track_check(&tr), 1);
    track_free(&tr);
}

static void TestTrackReportsAnAlignMismatch(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 64, 32);
    mem_free(a, p, 64, 8);

    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_ALIGN_MISMATCH);
    CHECK_INT_EQ(log.last.align, 32);
    CHECK_INT_EQ(log.last.claimed_align, 8);
    CHECK_INT_EQ(track_check(&tr), 1);
    track_free(&tr);
}

/* Writing to a freed block is the one fault that cannot be noticed when it
 * happens, only later, when the block leaves the quarantine and the poison it
 * was filled with turns out to have a hole in it. */

static void TestTrackReportsAWriteAfterFree(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);
    track_set_quarantine(&tr, 4096);

    unsigned char *p = (unsigned char *)mem_alloc(a, 64, 8);
    mem_free(a, p, 64, 8);
    CHECK_INT_EQ(log.n, 0); /* nothing is known yet, which is the point */

    p[7] = 0x11;

    /* Push it out of the quarantine, which is what makes the poison get read.
     * The blocks doing the pushing are freed correctly, so the only fault that
     * can come out of this loop is the one being tested for. */
    for (int i = 0; i < 80; i++) {
        void *q = mem_alloc(a, 64, 8);
        mem_free(a, q, 64, 8);
    }

    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_WRITE_AFTER_FREE);
    CHECK(log.last.p == p);
    CHECK_INT_EQ(log.last.size, 64);
    track_free(&tr);
}

/* Turning the quarantine off has to actually turn it off, since a long running
 * program under this allocator should be able to trade the check for the memory
 * back. */

static void TestTrackWithoutAQuarantineFreesStraightAway(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);
    track_set_quarantine(&tr, 0);

    for (int i = 0; i < 1000; i++) {
        void *p = mem_alloc(a, 256, 8);
        CHECK(p != NULL);
        mem_free(a, p, 256, 8);
    }

    CHECK_INT_EQ(mem_stats(a).bytes_live, 0);
    CHECK_INT_EQ(track_check(&tr), 0);
    track_free(&tr);
}

/* Realloc is a new block and a copy here rather than a grow in place, which is
 * deliberate: it means the old pointer becomes poisoned and held, so code that
 * kept it gets caught the same way any other write after free does. */

static void TestTrackReallocRetiresTheOldPointer(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);
    track_set_quarantine(&tr, 4096);

    unsigned char *p = (unsigned char *)mem_alloc(a, 32, 8);
    memset(p, 0x5A, 32);
    unsigned char *q = (unsigned char *)mem_realloc(a, p, 32, 64, 8);
    CHECK(q != NULL);
    CHECK(q[0] == 0x5A);
    CHECK(q[31] == 0x5A);
    CHECK_INT_EQ(track_live(&tr), 1);
    CHECK_INT_EQ(log.n, 0);

    /* The stale pointer, used the way somebody who forgot the return value
     * would use it. */
    p[0] = 0x01;
    for (int i = 0; i < 80; i++) {
        void *blk = mem_alloc(a, 64, 8);
        mem_free(a, blk, 64, 8);
    }
    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_WRITE_AFTER_FREE);

    mem_free(a, q, 64, 8);
    track_free(&tr);
}

static void TestTrackReallocChecksTheOldSize(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 48, 8);
    void *q = mem_realloc(a, p, 16, 96, 8);
    CHECK(q != NULL);
    CHECK_INT_EQ(log.n, 1);
    CHECK_INT_EQ(log.last.fault, TRACK_SIZE_MISMATCH);
    CHECK_INT_EQ(log.last.size, 48);
    CHECK_INT_EQ(log.last.claimed_size, 16);

    mem_free(a, q, 96, 8);
    track_free(&tr);
}

/* Over an arena the wrapper has to say it can be reset, and the reset has to
 * count as giving everything back rather than as a thousand leaks. Over the
 * heap it has to say it cannot, because claiming a reset that does nothing
 * would turn a real leak into silence. */

static void TestTrackFollowsTheAllocatorUnderneathOnReset(TestingT *t) {
    Track heap_tr;
    Log heap_log;
    start(&heap_tr, &heap_log);
    CHECK(!mem_can_reset(track_allocator(&heap_tr)));
    track_free(&heap_tr);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Track tr;
    Log log;
    log.n = 0;
    track_init(&tr, arena_allocator(&ar));
    track_on_fault(&tr, on_fault, &log);
    Alloc *a = track_allocator(&tr);
    CHECK(mem_can_reset(a));

    for (int i = 0; i < 200; i++)
        CHECK(mem_alloc(a, 64, 8) != NULL);
    CHECK_INT_EQ(track_live(&tr), 200);

    mem_reset(a);
    CHECK_INT_EQ(track_live(&tr), 0);
    CHECK_INT_EQ(track_check(&tr), 0);
    CHECK_INT_EQ(log.n, 0);

    track_free(&tr);
    arena_free(&ar);
}

/* The table behind all of this is open addressed with tombstones, so the case
 * that breaks it is a long run of allocate and free at a steady live count,
 * which fills every slot with a deleted marker without ever growing. If that is
 * mishandled the lookups quietly become a linear scan or, worse, stop finding
 * blocks that are there and start calling correct frees wild. */

static void TestTrackSurvivesALongRunOfChurn(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);
    track_set_quarantine(&tr, 0);

    void *live[16];
    for (int i = 0; i < 16; i++) {
        live[i] = mem_alloc(a, 32, 8);
        CHECK(live[i] != NULL);
    }
    for (int i = 0; i < 20000; i++) {
        int slot = i % 16;
        mem_free(a, live[slot], 32, 8);
        live[slot] = mem_alloc(a, 32, 8);
        CHECK(live[slot] != NULL);
    }
    CHECK_INT_EQ(track_live(&tr), 16);
    CHECK_INT_EQ(log.n, 0);

    for (int i = 0; i < 16; i++)
        mem_free(a, live[i], 32, 8);
    CHECK_INT_EQ(track_check(&tr), 0);
    track_free(&tr);
}

/* And the other shape, where the live count keeps climbing, which is what makes
 * the table grow and rehash rather than recycle tombstones. */

static void TestTrackHandlesManyLiveBlocks(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);

    enum { N = 5000 };
    static void *blocks[N];
    for (int i = 0; i < N; i++) {
        blocks[i] = mem_alloc(a, (size_t)(i % 64) + 1, 8);
        CHECK(blocks[i] != NULL);
    }
    CHECK_INT_EQ(track_live(&tr), N);
    CHECK_INT_EQ(mem_stats(a).blocks, N);

    /* Freed back to front, so the order records go in is not the order they
     * come out and the probes have to work either way. */
    for (int i = N - 1; i >= 0; i--)
        mem_free(a, blocks[i], (size_t)(i % 64) + 1, 8);

    CHECK_INT_EQ(track_live(&tr), 0);
    CHECK_INT_EQ(track_check(&tr), 0);
    CHECK_INT_EQ(log.n, 0);
    track_free(&tr);
}

/* Statistics come from the wrapper rather than from whatever is underneath,
 * which is the only reason a Track over an arena can tell you how much of the
 * arena you are actually using. */

static void TestTrackCountsBytes(TestingT *t) {
    Track tr;
    Log log;
    start(&tr, &log);
    Alloc *a = track_allocator(&tr);
    track_set_quarantine(&tr, 0);

    void *p = mem_alloc(a, 1000, 8);
    void *q = mem_alloc(a, 24, 8);
    AllocStats s = mem_stats(a);
    CHECK_INT_EQ(s.bytes_live, 1024);
    CHECK_INT_EQ(s.bytes_peak, 1024);
    CHECK_INT_EQ(s.allocs, 2);
    CHECK_INT_EQ(s.frees, 0);

    mem_free(a, p, 1000, 8);
    s = mem_stats(a);
    CHECK_INT_EQ(s.bytes_live, 24);
    CHECK_INT_EQ(s.bytes_peak, 1024);
    CHECK_INT_EQ(s.frees, 1);
    CHECK_INT_EQ(s.bytes_total, 1024);

    mem_free(a, q, 24, 8);
    track_free(&tr);
}

/* TRACK_HERE has to be harmless on an allocator that is not a Track, or it
 * cannot be left in code that runs both ways, and code that has to be edited
 * before it can be checked does not get checked. */

static void TestTrackHereDoesNothingToAPlainAllocator(TestingT *t) {
    Alloc *a = heap_allocator();
    CHECK(TRACK_HERE(a) == a);
    void *p = mem_alloc(TRACK_HERE(a), 16, 8);
    CHECK(p != NULL);
    mem_free(a, p, 16, 8);
    CHECK(track_note(NULL, "x", 1) == NULL);
}

/* Faults are counted whether or not anybody is listening, so a test that only
 * wants a number does not have to write a callback. */

static void TestTrackCountsFaultsWithoutAReporter(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);

    void *p = mem_alloc(a, 8, 8);
    mem_free(a, p, 8, 8);
    mem_free(a, p, 8, 8);
    CHECK_INT_EQ(track_faults(&tr), 1);
    CHECK_INT_EQ(track_check(&tr), 1);
    track_free(&tr);
}

#define TESTS(X)                                                                       \
    X(TestTrackIsQuietWhenNothingIsWrong)                                              \
    X(TestTrackPassesTheInterfaceThrough)                                              \
    X(TestTrackReportsALeak)                                                           \
    X(TestTrackReportsADoubleFree)                                                     \
    X(TestTrackReportsAWildFree)                                                       \
    X(TestTrackReportsASizeMismatch)                                                   \
    X(TestTrackReportsAnAlignMismatch)                                                 \
    X(TestTrackReportsAWriteAfterFree)                                                 \
    X(TestTrackWithoutAQuarantineFreesStraightAway)                                    \
    X(TestTrackReallocRetiresTheOldPointer)                                            \
    X(TestTrackReallocChecksTheOldSize)                                                \
    X(TestTrackFollowsTheAllocatorUnderneathOnReset)                                   \
    X(TestTrackSurvivesALongRunOfChurn)                                                \
    X(TestTrackHandlesManyLiveBlocks)                                                  \
    X(TestTrackCountsBytes)                                                            \
    X(TestTrackHereDoesNothingToAPlainAllocator)                                       \
    X(TestTrackCountsFaultsWithoutAReporter)

TESTING_MAIN(TESTS)
