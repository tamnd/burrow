/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "harness.h"

#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
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

TEST(interface_rejects_nonsense) {
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

TEST(interface_zeroes) {
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

TEST(interface_realloc_zeroes_the_new_tail) {
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

TEST(heap_basics) {
    Alloc *a = heap_allocator();
    CHECK(a == heap_allocator());
    CHECK(!mem_can_reset(a));

    /* Reset on an allocator that cannot reset does nothing rather than crash,
     * so that generic code does not have to ask first. */
    mem_reset(a);

    AllocStats s = mem_stats(a);
    CHECK_INT_EQ(s.allocs, 0); /* the heap does not count, by design */
}

TEST(heap_over_alignment) {
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

TEST(arena_hands_out_zeroed_memory) {
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

TEST(arena_alignment_and_separation) {
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

TEST(arena_reset_keeps_its_chunks) {
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

TEST(arena_grows_the_last_allocation_in_place) {
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

TEST(arena_takes_allocations_larger_than_a_chunk) {
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

TEST(arena_nests) {
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

TEST(arena_free_is_safe_twice_and_when_unused) {
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

TEST(fixed_stays_inside_the_budget) {
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

TEST(fixed_handles_an_empty_buffer) {
    Fixed fx;
    fixed_init(&fx, NULL, 0);
    Alloc *a = fixed_allocator(&fx);
    CHECK(mem_alloc(a, 1, 1) == NULL);
    CHECK_INT_EQ(fixed_available(&fx), 0);
    CHECK(mem_can_reset(a));
}

int main(void) {
    RUN(interface_rejects_nonsense);
    RUN(interface_zeroes);
    RUN(interface_realloc_zeroes_the_new_tail);
    RUN(heap_basics);
    RUN(heap_over_alignment);
    RUN(arena_hands_out_zeroed_memory);
    RUN(arena_alignment_and_separation);
    RUN(arena_reset_keeps_its_chunks);
    RUN(arena_grows_the_last_allocation_in_place);
    RUN(arena_takes_allocations_larger_than_a_chunk);
    RUN(arena_nests);
    RUN(arena_free_is_safe_twice_and_when_unused);
    RUN(fixed_stays_inside_the_budget);
    RUN(fixed_handles_an_empty_buffer);
    return harness_report("mem");
}
