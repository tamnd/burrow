/* Derived from Go's src/hash/crc64/crc64_test.go.
 * Go source: go1.27.1.
 *
 * TestGoldenMarshal and TestMarshalTableMismatch wait for hash state
 * marshaling (#185), and the marshaled states are dropped from the golden table until
 * then. TestSlicing is not in Go: it checks the eight bytes at a time path
 * against the byte at a time one, which Go's golden inputs are mostly too short
 * to reach.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/hash/crc64.h"
#include "burrow/mem/arena.h"

#include <string.h>

static Hash make_iso(Alloc *a) {
    return hash_hash64_as_hash(crc64_new(a, crc64_make_table(NULL, CRC64_ISO)));
}

static void TestCRC64Hash(TestingT *t) {
    testhash_without_clone(t, make_iso);
}

typedef struct Golden {
    uint64_t out_iso;
    uint64_t out_ecma;
    const char *in;
} Golden;

static const Golden golden[] = {
    {0x0, 0x0, ""},
    {0x3420000000000000, 0x330284772e652b05, "a"},
    {0x36c4200000000000, 0xbc6573200e84b046, "ab"},
    {0x3776c42000000000, 0x2cd8094a1a277627, "abc"},
    {0x336776c420000000, 0x3c9d28596e5960ba, "abcd"},
    {0x32d36776c4200000, 0x40bdf58fb0895f2, "abcde"},
    {0x3002d36776c42000, 0xd08e9f8545a700f4, "abcdef"},
    {0x31b002d36776c420, 0xec20a3a8cc710e66, "abcdefg"},
    {0xe21b002d36776c4, 0x67b4f30a647a0c59, "abcdefgh"},
    {0x8b6e21b002d36776, 0x9966f6c89d56ef8e, "abcdefghi"},
    {0x7f5b6e21b002d367, 0x32093a2ecd5773f4, "abcdefghij"},
    {0x8ec0e7c835bf9cdf, 0x8a0825223ea6d221,
     "Discard medicine more than two years old."},
    {0xc7db1759e2be5ab4, 0x8562c0ac2ab9a00d,
     "He who has a shady past knows that nice guys finish last."},
    {0xfbf9d9603a6fa020, 0x3ee2a39c083f38b4,
     "I wouldn't marry him with a ten foot pole."},
    {0xeafc4211a6daa0ef, 0x1f603830353e518a,
     "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave"},
    {0x3e05b21c7a4dc4da, 0x2fd681d7b2421fd,
     "The days of the digital watch are numbered.  -Tom Stoppard"},
    {0x5255866ad6ef28a6, 0x790ef2b16a745a41, "Nepal premier won't resign."},
    {0x8a79895be1e9c361, 0x3ef8f06daccdcddf,
     "For every action there is an equal and opposite government program."},
    {0x8878963a649d4916, 0x49e41b2660b106d,
     "His money is twice tainted: 'taint yours and 'taint mine."},
    {0xa7b9d53ea87eb82f, 0x561cc0cfa235ac68,
     "There is no reason for any individual to have a computer in their home. -Ken "
     "Olsen, 1977"},
    {0xdb6805c0966a2f9c, 0xd4fe9ef082e69f59,
     "It's a tiny change to the code and not completely disgusting. - Bob Manchek"},
    {0xf3553c65dacdadd2, 0xe3b5e46cd8d63a4d, "size:  a.out:  bad magic"},
    {0x9d5e034087a676b9, 0x865aaf6b94f2a051,
     "The major problem is with sendmail.  -Mark Horton"},
    {0xa6db2d7f8da96417, 0x7eca10d2f8136eb4,
     "Give me a rock, paper and scissors and I will move the world.  CCFestoon"},
    {0x325e00cd2fe819f9, 0xd7dd118c98e98727,
     "If the enemy is within range, then so are you."},
    {0x88c6600ce58ae4c6, 0x70fb33c119c29318,
     "It's well we cannot hear the screams/That we create in others' dreams."},
    {0x28c4a3f3b769e078, 0x57c891e39a97d9b7,
     "You remind me of a TV show, but that's all right: I watch it anyway."},
    {0xa698a34c9d9f1dca, 0xa1f46ba20ad06eb7, "C is as portable as Stonehedge!!"},
    {0xf6c1e2a8c26c5cfc, 0x7ad25fafa1710407,
     "Even if I could be Shakespeare, I think I should still choose to be Faraday. - "
     "A. Huxley"},
    {0xd402559dfe9b70c, 0x73cef1666185c13f,
     "The fugacity of a constituent in a mixture of gases at a given temperature is "
     "proportional to its mole fraction.  Lewis-Randall Rule"},
    {0xdb6efff26aa94946, 0xb41858f73c389602,
     "How can you write a big system without C++?  -Paul Glick"},
    {0xe7fcf1006b503b61, 0x27db187fc15bbc72,
     "This is a test of the emergency broadcast system."},
};

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Crc64Table *tab_iso = crc64_make_table(NULL, CRC64_ISO);
    const Crc64Table *tab_ecma = crc64_make_table(NULL, CRC64_ECMA);
    for (size_t i = 0; i < sizeof golden / sizeof golden[0]; i++) {
        const Golden *g = &golden[i];
        Str in = str_from_cstr(g->in);
        Slice p = slice_from_str(a, in);
        HashHash64 c = crc64_new(a, tab_iso);
        hash_write(hash_hash64_as_hash(c), p, NULL);
        uint64_t s = hash_hash64_sum64(c);
        if (s != g->out_iso)
            testing_t_fatalf_v(t, "ISO crc64(%s) = 0x%x want 0x%x", in, s, g->out_iso);
        c = crc64_new(a, tab_ecma);
        hash_write(hash_hash64_as_hash(c), p, NULL);
        s = hash_hash64_sum64(c);
        if (s != g->out_ecma)
            testing_t_fatalf_v(t, "ECMA crc64(%s) = 0x%x want 0x%x", in, s,
                               g->out_ecma);
    }
    arena_free(&ar);
}

static void TestSlicing(TestingT *t) {
    static const Int lengths[] = {0,  1,   8,   9,   63,   64,   65,   71,   72,
                                  73, 127, 128, 129, 1000, 1024, 2047, 2048, 4099};
    static const uint64_t polys[] = {CRC64_ISO, CRC64_ECMA};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand rng = testhash_new_rand(t);
    for (size_t k = 0; k < 2; k++) {
        const Crc64Table *fast = crc64_make_table(NULL, polys[k]);
        /* The same table at another address goes a byte at a time. */
        Crc64Table *slow = BURROW_NEW(a, Crc64Table);
        *slow = *fast;
        for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
            Slice p = testhash_bytes(a, lengths[i]);
            testhash_read(&rng, p);
            uint64_t init;
            testhash_read(&rng, slice_from(&init, 8, 8, TYPE_BYTE));
            uint64_t c1 = crc64_update(init, fast, p);
            uint64_t c2 = crc64_update(init, slow, p);
            if (c1 != c2)
                testing_t_errorf_v(
                    t, "poly 0x%x: mismatch: 0x%x vs 0x%x (buffer length %d)", polys[k],
                    c1, c2, lengths[i]);
        }
    }
    arena_free(&ar);
}

typedef struct Bench {
    uint64_t poly;
    Int size;
} Bench;

static void bench(void *env, TestingB *b) {
    const Bench *bn = env;
    testing_b_set_bytes(b, (int64_t)bn->size);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = testhash_bytes(a, bn->size);
    for (Int i = 0; i < data.len; i++)
        ((Byte *)data.p)[i] = (Byte)i;
    Hash h = hash_hash64_as_hash(crc64_new(a, crc64_make_table(a, bn->poly)));
    if (h.vt == NULL)
        testing_b_fail_now(b);
    Slice in = slice_make(a, TYPE_BYTE, 0, hash_size(h));

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_reset(h);
        hash_write(h, data, NULL);
        hash_sum(a, h, in);
    }
    arena_free(&ar);
}

static void BenchmarkCrc64(TestingB *b) {
    static Bench iso64 = {CRC64_ISO, 64 << 10}, iso4 = {CRC64_ISO, 4 << 10},
                 iso1 = {CRC64_ISO, 1 << 10}, ecma64 = {CRC64_ECMA, 64 << 10},
                 random64 = {0x777, 64 << 10}, random16 = {0x777, 16 << 10};
    testing_b_run(b, BURROW_S("ISO64KB"), BURROW_FN(TestingBFunc, bench, &iso64));
    testing_b_run(b, BURROW_S("ISO4KB"), BURROW_FN(TestingBFunc, bench, &iso4));
    testing_b_run(b, BURROW_S("ISO1KB"), BURROW_FN(TestingBFunc, bench, &iso1));
    testing_b_run(b, BURROW_S("ECMA64KB"), BURROW_FN(TestingBFunc, bench, &ecma64));
    testing_b_run(b, BURROW_S("Random64KB"), BURROW_FN(TestingBFunc, bench, &random64));
    testing_b_run(b, BURROW_S("Random16KB"), BURROW_FN(TestingBFunc, bench, &random16));
}

#define TESTS(X)                                                                       \
    X(TestCRC64Hash)                                                                   \
    X(TestGolden)                                                                      \
    X(TestSlicing)                                                                     \
    X(BenchmarkCrc64)

TESTING_MAIN(TESTS)
