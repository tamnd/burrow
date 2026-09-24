/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/gc.h"
#include "burrow/mem/heap.h"

#include <stdint.h>

static bool all_zero(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0)
            return false;
    }
    return true;
}

static bool aligned_to(const void *p, size_t align) {
    return ((uintptr_t)p & ((uintptr_t)align - 1u)) == 0;
}

/* The rules that belong to the interface rather than to any one backend, so
 * they are checked against every backend rather than against a favourite. */

static void TestInterfaceRejectsNonsense(TestingT *t) {
    Alloc *a = heap_allocator();
    CHECK(mem_alloc(NULL, 8, 1) == NULL);
    CHECK(mem_alloc(a, 0, 1) == NULL);
    CHECK(mem_alloc(a, 8, 0) == NULL);
    CHECK(mem_alloc(a, 8, 3) == NULL); /* alignment has to be a power of two */

    /* The multiplication that has to be checked, because an element count that
     * wraps is the oldest heap overflow there is. */
    CHECK(mem_alloc_array(a, SIZE_MAX, 2, 1) == NULL);
    CHECK(mem_alloc_array(a, SIZE_MAX / 4 + 1, 4, 1) == NULL);

    /* And the case just under the line, which has to still work. */
    void *p = mem_alloc_array(a, 4, 4, 4);
    CHECK(p != NULL);
    mem_free(a, p, 16, 4);

    /* Freeing nothing is not an error, so callers do not have to check. */
    mem_free(a, NULL, 0, 1);
    mem_free(NULL, NULL, 0, 1);
}

static void TestInterfaceZeroes(TestingT *t) {
    Alloc *a = heap_allocator();
    /* Sizes chosen to land in a few different size classes, since a fresh
     * malloc often happens to return zeroed memory and we want the case where
     * it does not. */
    size_t sizes[] = {1, 7, 64, 4096, 100000};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned char *p = (unsigned char *)mem_alloc(a, sizes[i], 1);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(all_zero(p, sizes[i]));
        for (size_t j = 0; j < sizes[i]; j++)
            p[j] = 0xAB;
        mem_free(a, p, sizes[i], 1);
    }
}

static void TestInterfaceReallocZeroesTheNewTail(TestingT *t) {
    Alloc *a = heap_allocator();
    unsigned char *p = (unsigned char *)mem_alloc(a, 16, 1);
    CHECK(p != NULL);
    for (size_t i = 0; i < 16; i++)
        p[i] = 0x7F;

    p = (unsigned char *)mem_realloc(a, p, 16, 64, 1);
    CHECK(p != NULL);
    if (p == NULL)
        return;
    for (size_t i = 0; i < 16; i++)
        CHECK_INT_EQ(p[i], 0x7F);
    CHECK(all_zero(p + 16, 48));

    /* A realloc to nothing is a free, and it returns NULL rather than a
     * pointer nobody should use. */
    CHECK(mem_realloc(a, p, 64, 0, 1) == NULL);
}

static void TestHeapBasics(TestingT *t) {
    Alloc *a = heap_allocator();
    CHECK(a == heap_allocator());
    CHECK(!mem_can_reset(a));

    /* Reset on an allocator that cannot reset does nothing rather than crash,
     * so that generic code does not have to ask first. */
    mem_reset(a);

    AllocStats s = mem_stats(a);
    CHECK_INT_EQ(s.allocs, 0); /* the heap does not count, by design */
}

static void TestHeapOverAlignment(TestingT *t) {
    Alloc *a = heap_allocator();
    size_t aligns[] = {32, 64, 128, 4096};
    for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        size_t al = aligns[i];
        unsigned char *p = (unsigned char *)mem_alloc(a, 100, al);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(aligned_to(p, al));
        CHECK(all_zero(p, 100));
        for (size_t j = 0; j < 100; j++)
            p[j] = (unsigned char)(j & 0xFF);

        p = (unsigned char *)mem_realloc(a, p, 100, 300, al);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(aligned_to(p, al));
        for (size_t j = 0; j < 100; j++)
            CHECK_INT_EQ(p[j], (unsigned char)(j & 0xFF));
        CHECK(all_zero(p + 100, 200));
        mem_free(a, p, 300, al);
    }
}

static void TestArenaHandsOutZeroedMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (int round = 0; round < 3; round++) {
        unsigned char *p = (unsigned char *)mem_alloc(a, 512, 1);
        CHECK(p != NULL);
        if (p == NULL)
            break;
        CHECK(all_zero(p, 512));
        for (size_t i = 0; i < 512; i++)
            p[i] = 0xEE;
        /* The reset is the interesting part. The second round gets the same
         * bytes back, which have been written, so the arena has to clear them
         * even though it skipped clearing them the first time. */
        arena_reset(&ar);
    }

    arena_free(&ar);
}

static void TestArenaAlignmentAndSeparation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    size_t aligns[] = {1, 2, 8, 16, 64, 256};
    unsigned char *seen[6];
    for (size_t i = 0; i < 6; i++) {
        seen[i] = (unsigned char *)mem_alloc(a, 40, aligns[i]);
        CHECK(seen[i] != NULL);
        if (seen[i] != NULL)
            CHECK(aligned_to(seen[i], aligns[i]));
    }

    /* Write a distinct byte into each and read them all back, which is how a
     * bump allocator that overlaps two allocations gets caught. */
    for (size_t i = 0; i < 6; i++) {
        if (seen[i] != NULL)
            memset(seen[i], (int)('a' + i), 40);
    }
    for (size_t i = 0; i < 6; i++) {
        if (seen[i] != NULL)
            CHECK_INT_EQ(seen[i][39], 'a' + (int)i);
    }

    arena_free(&ar);
}

static void TestArenaResetKeepsItsChunks(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 4096);
    Alloc *a = arena_allocator(&ar);

    for (int i = 0; i < 40; i++)
        CHECK(mem_alloc(a, 600, 8) != NULL);

    uint64_t blocks = mem_stats(a).blocks;
    CHECK(blocks > 1); /* 40 times 600 does not fit in one 4096 byte chunk */

    arena_reset(&ar);
    CHECK_INT_EQ(mem_stats(a).bytes_live, 0);

    for (int i = 0; i < 40; i++)
        CHECK(mem_alloc(a, 600, 8) != NULL);

    /* The whole point of the reset. The second round asked the parent for
     * nothing at all. */
    CHECK_INT_EQ(mem_stats(a).blocks, blocks);

    arena_free(&ar);
    CHECK_INT_EQ(mem_stats(a).blocks, 0);
}

static void TestArenaReleaseGoesBackToTheMark(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 4096);
    Alloc *a = arena_allocator(&ar);

    unsigned char *before = (unsigned char *)mem_alloc(a, 100, 8);
    CHECK(before != NULL);
    memset(before, 0xab, 100);

    ArenaMark m = arena_mark(&ar);
    for (int i = 0; i < 40; i++)
        CHECK(mem_alloc(a, 600, 8) != NULL); /* several chunks' worth */
    uint64_t blocks = mem_stats(a).blocks;
    CHECK(blocks > 1);

    arena_release(&ar, m);

    /* What came before the mark is untouched, and the next allocation lands
     * straight after it, in the first chunk, as if the forty never happened. */
    CHECK(before[0] == 0xab && before[99] == 0xab);
    unsigned char *next = (unsigned char *)mem_alloc(a, 8, 8);
    CHECK(next == before + 104 || next == before + 100);

    /* And the forty again cost the parent nothing, because the chunks were
     * kept. */
    for (int i = 0; i < 40; i++)
        CHECK(mem_alloc(a, 600, 8) != NULL);
    CHECK_INT_EQ(mem_stats(a).blocks, blocks);

    arena_free(&ar);
}

static void TestArenaMarksNestAndGoStaleSafely(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 1024);
    Alloc *a = arena_allocator(&ar);

    ArenaMark outer = arena_mark(&ar); /* taken on an empty arena */
    CHECK(mem_alloc(a, 700, 8) != NULL);
    ArenaMark inner = arena_mark(&ar);
    CHECK(mem_alloc(a, 700, 8) != NULL);

    /* The inner release is skipped, the way an early return would skip it,
     * and the outer one takes everything anyway. */
    arena_release(&ar, outer);
    CHECK_INT_EQ(mem_stats(a).bytes_live, 0);

    /* The inner mark is now behind where the arena is, and releasing it
     * afterwards changes nothing. */
    unsigned char *p = (unsigned char *)mem_alloc(a, 16, 8);
    CHECK(p != NULL);
    arena_release(&ar, inner);
    CHECK(mem_alloc(a, 16, 8) == p + 16);

    arena_free(&ar);
}

static void TestArenaGrowsTheLastAllocationInPlace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    unsigned char *buf = (unsigned char *)mem_alloc(a, 1, 1);
    CHECK(buf != NULL);
    if (buf == NULL) {
        arena_free(&ar);
        return;
    }
    buf[0] = 1;

    /* Append one byte at a time, which is the pattern that would otherwise
     * leave a dead copy of the buffer behind on every iteration. */
    unsigned char *first = buf;
    for (size_t n = 1; n < 2000; n++) {
        unsigned char *next = (unsigned char *)mem_realloc(a, buf, n, n + 1, 1);
        CHECK(next != NULL);
        if (next == NULL)
            break;
        buf = next;
        buf[n] = (unsigned char)((n + 1) & 0xFF);
    }
    CHECK(buf == first); /* never moved, because it was always the last one */
    for (size_t i = 0; i < 2000; i++)
        CHECK_INT_EQ(buf[i], (unsigned char)((i + 1) & 0xFF));

    /* And the same unwinding through free, which is what makes a scratch
     * buffer inside a loop cost nothing. */
    uint64_t before = mem_stats(a).bytes_live;
    for (int i = 0; i < 1000; i++) {
        void *scratch = mem_alloc(a, 256, 8);
        CHECK(scratch != NULL);
        mem_free(a, scratch, 256, 8);
    }
    CHECK_INT_EQ(mem_stats(a).bytes_live, before);

    arena_free(&ar);
}

static void TestArenaTakesAllocationsLargerThanAChunk(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 1024);
    Alloc *a = arena_allocator(&ar);

    unsigned char *big = (unsigned char *)mem_alloc(a, 300000, 64);
    CHECK(big != NULL);
    if (big != NULL) {
        CHECK(aligned_to(big, 64));
        CHECK(all_zero(big, 300000));
        big[299999] = 9;
    }

    /* Ordinary allocations still work afterwards. */
    CHECK(mem_alloc(a, 16, 8) != NULL);
    arena_free(&ar);
}

static void TestArenaNests(TestingT *t) {
    Arena outer;
    arena_init(&outer, NULL, 0);

    Arena inner;
    arena_init(&inner, arena_allocator(&outer), 8192);
    Alloc *ia = arena_allocator(&inner);

    for (int i = 0; i < 100; i++) {
        unsigned char *p = (unsigned char *)mem_alloc(ia, 300, 8);
        CHECK(p != NULL);
        if (p != NULL)
            CHECK(all_zero(p, 300));
    }
    CHECK(mem_stats(arena_allocator(&outer)).allocs > 0);

    arena_free(&inner);
    arena_free(&outer);
}

static void TestArenaFreeIsSafeTwiceAndWhenUnused(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    arena_free(&ar);
    arena_free(&ar);

    /* Still usable afterwards, because a freed arena is just an empty one. */
    Alloc *a = arena_allocator(&ar);
    CHECK(mem_alloc(a, 32, 8) != NULL);
    arena_free(&ar);

    arena_init(&ar, NULL, 0);
    CHECK(mem_can_reset(arena_allocator(&ar)));
    arena_free(&ar);
}

static void TestFixedStaysInsideTheBudget(TestingT *t) {
    unsigned char buf[512];
    memset(buf, 0xFF, sizeof(buf)); /* nothing here is zero to begin with */

    Fixed fx;
    fixed_init(&fx, buf, sizeof(buf));
    Alloc *a = fixed_allocator(&fx);

    CHECK_INT_EQ(fixed_available(&fx), 512);

    unsigned char *p = (unsigned char *)mem_alloc(a, 200, 8);
    CHECK(p != NULL);
    CHECK(all_zero(p, 200)); /* cleared even though the buffer was not */
    CHECK(p >= buf && p + 200 <= buf + sizeof(buf));

    CHECK(mem_alloc(a, 200, 8) != NULL);
    CHECK(mem_alloc(a, 200, 8) == NULL); /* out of budget, and it says so */
    CHECK(fixed_available(&fx) < 200);

    fixed_reset(&fx);
    CHECK_INT_EQ(fixed_available(&fx), 512);
    CHECK(mem_alloc(a, 400, 8) != NULL);

    /* Three succeeded and one failed, and the failure is not counted as an
     * allocation, because a number that includes attempts is not a number
     * anybody can use. */
    AllocStats s = mem_stats(a);
    CHECK_INT_EQ(s.allocs, 3);
    CHECK(s.bytes_peak >= 400);
}

static void TestFixedHandlesAnEmptyBuffer(TestingT *t) {
    Fixed fx;
    fixed_init(&fx, NULL, 0);
    Alloc *a = fixed_allocator(&fx);
    CHECK(mem_alloc(a, 1, 1) == NULL);
    CHECK_INT_EQ(fixed_available(&fx), 0);
    CHECK(mem_can_reset(a));
}

/* The out of memory handler.
 *
 * Everything here uses fixed, because it is the one backend where running out
 * is a normal Tuesday rather than a machine in trouble, and because the size of
 * the hole is decided by the test rather than by the host. */

typedef struct OomSpy {
    int calls;
    size_t size;
    size_t align;
    bool answer;
    Fixed *widen; /* reset this on the way through, if set */
} OomSpy;

static bool oom_spy(void *ctx, size_t size, size_t align) {
    OomSpy *s = (OomSpy *)ctx;
    s->calls++;
    s->size = size;
    s->align = align;
    if (s->widen != NULL)
        fixed_reset(s->widen);
    return s->answer;
}

static void TestOomHandlerFiresAndIsToldWhatWasAskedFor(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    OomSpy spy = {0, 0, 0, false, NULL};
    mem_set_oom(a, oom_spy, &spy);

    CHECK(mem_alloc(a, 4096, 16) == NULL);
    CHECK_INT_EQ(spy.calls, 1);
    CHECK_INT_EQ(spy.size, 4096);
    CHECK_INT_EQ(spy.align, 16);
}

static void TestOomHandlerSayingNoLetsTheNullThrough(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    OomSpy spy = {0, 0, 0, false, NULL};
    mem_set_oom(a, oom_spy, &spy);

    CHECK(mem_alloc(a, 4096, 1) == NULL);
    CHECK(mem_alloc_nozero(a, 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 2);
}

static void TestOomHandlerThatMakesRoomGetsTheAllocationThrough(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    /* Fill it, so the next request has nowhere to go. */
    CHECK(mem_alloc(a, 48, 1) != NULL);

    OomSpy spy = {0, 0, 0, true, &fx};
    mem_set_oom(a, oom_spy, &spy);

    void *p = mem_alloc(a, 48, 1);
    CHECK_INT_EQ(spy.calls, 1);
    CHECK(p != NULL);
    CHECK(all_zero(p, 48));
}

static void TestOomHandlerIsAskedOnceAndNotInALoop(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    /* Answers yes every time and frees nothing, which is the handler that would
     * hang the program if the retry were a loop. */
    OomSpy spy = {0, 0, 0, true, NULL};
    mem_set_oom(a, oom_spy, &spy);

    CHECK(mem_alloc(a, 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 1);
}

static void TestOomHandlerIgnoresRequestsNoAllocatorCouldHaveMet(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    OomSpy spy = {0, 0, 0, false, NULL};
    mem_set_oom(a, oom_spy, &spy);

    /* Nothing was asked for, so nothing was refused. */
    CHECK(mem_alloc(a, 0, 1) == NULL);

    /* The element count times the element size does not fit in a size_t, so
     * there is no amount of free memory that would have helped. */
    CHECK(mem_alloc_array(a, SIZE_MAX, 2, 1) == NULL);

    CHECK_INT_EQ(spy.calls, 0);
}

static void TestOomHandlerCoversRealloc(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    void *p = mem_alloc(a, 16, 1);
    CHECK(p != NULL);

    OomSpy spy = {0, 0, 0, false, NULL};
    mem_set_oom(a, oom_spy, &spy);

    CHECK(mem_realloc(a, p, 16, 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 1);
    CHECK_INT_EQ(spy.size, 4096);
}

static void TestOomHandlerCanBeRemovedAndBelongsToOneAllocator(TestingT *t) {
    unsigned char buf_a[64];
    unsigned char buf_b[64];
    Fixed fa;
    Fixed fb;
    fixed_init(&fa, buf_a, sizeof buf_a);
    fixed_init(&fb, buf_b, sizeof buf_b);

    OomSpy spy = {0, 0, 0, false, NULL};
    mem_set_oom(fixed_allocator(&fa), oom_spy, &spy);

    /* The other allocator never heard of it. */
    CHECK(mem_alloc(fixed_allocator(&fb), 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 0);

    CHECK(mem_alloc(fixed_allocator(&fa), 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 1);

    mem_set_oom(fixed_allocator(&fa), NULL, NULL);
    CHECK(mem_alloc(fixed_allocator(&fa), 4096, 1) == NULL);
    CHECK_INT_EQ(spy.calls, 1);
}

/* An allocator with no handler behaves the way it always did, which is the
 * thing that must not have changed. */
static void TestOomWithoutAHandlerIsStillJustNull(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);

    CHECK(mem_alloc(a, 4096, 1) == NULL);
    CHECK(mem_alloc_nozero(a, 4096, 1) == NULL);
    CHECK(mem_alloc_array(a, 4096, 4, 1) == NULL);

    /* And setting one on nothing is not a crash, since every other entry point
     * in this header takes a NULL allocator without complaint. */
    mem_set_oom(NULL, oom_spy, &fx);
}

/* The gc backend is the only part of this library that is not in every build,
 * which makes it the only part whose tests have to ask first. They are written
 * as one set rather than two behind an #ifdef, so the code that uses the
 * collector is compiled by every job in the matrix and only runs in the one that
 * has it. A test that exists in half the builds is a test that rots in the other
 * half without anybody noticing.
 *
 * Nothing here calls mem_free expecting memory back, because not giving it back
 * is what this backend is. */

static void TestGcSaysWhetherThisBuildHasIt(TestingT *t) {
    if (gc_available()) {
        Alloc *a = gc_allocator();
        CHECK(a != NULL);

        /* One collector in a process, so one object, and comparing against it
         * is a fair way to ask whether some Alloc you were handed is this one. */
        CHECK(a == gc_allocator());
        return;
    }

    /* Without it, everything still links and nothing crashes. This is the half
     * that runs on almost every machine, and it is checking that a program
     * written against the collector fails by getting a NULL it can look at
     * rather than by silently leaking through some fallback. */
    CHECK(gc_allocator() == NULL);
    CHECK(mem_alloc(gc_allocator(), 16, 8) == NULL);
    CHECK(mem_realloc(gc_allocator(), NULL, 0, 16, 8) == NULL);
    CHECK(!mem_can_reset(gc_allocator()));
    mem_free(gc_allocator(), NULL, 0, 8);
    gc_collect();
}

static void TestGcHandsOutZeroedMemory(TestingT *t) {
    Alloc *a = gc_allocator();
    if (a == NULL)
        return;

    CHECK(mem_alloc(a, 0, 8) == NULL);
    CHECK(mem_alloc(a, 8, 3) == NULL);

    for (size_t n = 1; n <= 4096; n *= 4) {
        void *p = mem_alloc(a, n, 8);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(aligned_to(p, 8));
        CHECK(all_zero(p, n));
        memset(p, 0xAB, n);
    }
}

static void TestGcRespectsAlignmentItDoesNotGetForFree(TestingT *t) {
    Alloc *a = gc_allocator();
    if (a == NULL)
        return;

    /* 64 is a cache line and 256 is past anything a fundamental type needs, so
     * both of these go down the explicit path rather than the ordinary one. */
    for (size_t align = 1; align <= 256; align *= 2) {
        void *p = mem_alloc(a, 100, align);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(aligned_to(p, align));
        CHECK(all_zero(p, 100));
    }
}

static void TestGcReallocKeepsWhatWasThere(TestingT *t) {
    Alloc *a = gc_allocator();
    if (a == NULL)
        return;

    /* Both paths, since an over aligned block cannot be grown the way an
     * ordinary one can and takes a copy instead. */
    size_t aligns[] = {8, 64};
    for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) {
        size_t align = aligns[i];
        unsigned char *p = (unsigned char *)mem_alloc(a, 32, align);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        for (size_t k = 0; k < 32; k++)
            p[k] = (unsigned char)(k + 1);

        unsigned char *q = (unsigned char *)mem_realloc(a, p, 32, 512, align);
        CHECK(q != NULL);
        if (q == NULL)
            continue;
        CHECK(aligned_to(q, align));
        for (size_t k = 0; k < 32; k++)
            CHECK_INT_EQ(q[k], (int)(k + 1));

        /* Grown memory is zeroed, the same as everywhere else in this
         * interface, because Go's zero value rule does not have exceptions. */
        CHECK(all_zero(q + 32, 512 - 32));

        unsigned char *s = (unsigned char *)mem_realloc(a, q, 512, 16, align);
        CHECK(s != NULL);
        if (s != NULL) {
            for (size_t k = 0; k < 16; k++)
                CHECK_INT_EQ(s[k], (int)(k + 1));
        }
    }
}

static void TestGcFreeDoesNothingAndResetIsNotOffered(TestingT *t) {
    Alloc *a = gc_allocator();
    if (a == NULL)
        return;

    CHECK(!mem_can_reset(a));

    unsigned char *p = (unsigned char *)mem_alloc(a, 64, 8);
    CHECK(p != NULL);
    if (p == NULL)
        return;
    memset(p, 0x5A, 64);

    /* Saying you are finished with a block is not a claim the collector acts
     * on, and while this pointer is still here the block is still reachable, so
     * reading it back is defined and has to give the same bytes. Under any other
     * backend this would be a use after free. That difference is the backend. */
    mem_free(a, p, 64, 8);
    CHECK_INT_EQ(p[0], 0x5A);
    CHECK_INT_EQ(p[63], 0x5A);

    /* And mem_reset on an allocator that cannot reset is a no-op rather than a
     * fault, so generic code does not have to ask first. */
    mem_reset(a);
    CHECK_INT_EQ(p[0], 0x5A);
}

static void TestGcCollectsAndReportsTheCollectorNumbers(TestingT *t) {
    Alloc *a = gc_allocator();
    if (a == NULL)
        return;

    /* Enough garbage that the collector has something to find, and none of it
     * kept, which is the point. */
    for (int i = 0; i < 4096; i++) {
        void *p = mem_alloc(a, 256, 8);
        CHECK(p != NULL);
        if (p == NULL)
            break;
    }

    AllocStats before = mem_stats(a);
    CHECK(before.bytes_total > 0);
    CHECK(before.bytes_live > 0);

    gc_collect();

    AllocStats after = mem_stats(a);
    CHECK(after.bytes_total >= before.bytes_total);
    CHECK(after.blocks > before.blocks); /* one more collection than before */

    /* The fields this backend has nothing true to say about stay at zero rather
     * than being filled in with something plausible. */
    CHECK(after.allocs == 0);
    CHECK(after.frees == 0);
    CHECK(after.bytes_peak == 0);
}

#define TESTS(X)                                                                       \
    X(TestInterfaceRejectsNonsense)                                                    \
    X(TestInterfaceZeroes)                                                             \
    X(TestInterfaceReallocZeroesTheNewTail)                                            \
    X(TestHeapBasics)                                                                  \
    X(TestHeapOverAlignment)                                                           \
    X(TestArenaHandsOutZeroedMemory)                                                   \
    X(TestArenaAlignmentAndSeparation)                                                 \
    X(TestArenaResetKeepsItsChunks)                                                    \
    X(TestArenaReleaseGoesBackToTheMark)                                               \
    X(TestArenaMarksNestAndGoStaleSafely)                                              \
    X(TestArenaGrowsTheLastAllocationInPlace)                                          \
    X(TestArenaTakesAllocationsLargerThanAChunk)                                       \
    X(TestArenaNests)                                                                  \
    X(TestArenaFreeIsSafeTwiceAndWhenUnused)                                           \
    X(TestFixedStaysInsideTheBudget)                                                   \
    X(TestFixedHandlesAnEmptyBuffer)                                                   \
    X(TestOomHandlerFiresAndIsToldWhatWasAskedFor)                                     \
    X(TestOomHandlerSayingNoLetsTheNullThrough)                                        \
    X(TestOomHandlerThatMakesRoomGetsTheAllocationThrough)                             \
    X(TestOomHandlerIsAskedOnceAndNotInALoop)                                          \
    X(TestOomHandlerIgnoresRequestsNoAllocatorCouldHaveMet)                            \
    X(TestOomHandlerCoversRealloc)                                                     \
    X(TestOomHandlerCanBeRemovedAndBelongsToOneAllocator)                              \
    X(TestOomWithoutAHandlerIsStillJustNull)                                           \
    X(TestGcSaysWhetherThisBuildHasIt)                                                 \
    X(TestGcHandsOutZeroedMemory)                                                      \
    X(TestGcRespectsAlignmentItDoesNotGetForFree)                                      \
    X(TestGcReallocKeepsWhatWasThere)                                                  \
    X(TestGcFreeDoesNothingAndResetIsNotOffered)                                       \
    X(TestGcCollectsAndReportsTheCollectorNumbers)

TESTING_MAIN(TESTS)
