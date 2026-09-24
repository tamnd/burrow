/* Derived from Go's src/hash/crc32/crc32_test.go.
 * Go source: go1.27.1.
 *
 * TestSimple and TestSlicing call Go's unexported table builders directly. The
 * same two paths are reached here through the public calls: the package's own
 * IEEE and Castagnoli tables take the slicing-by-8 path, and a copy of either
 * table at another address takes the simple one, so the cross checks compare
 * those two. Go's cross check also runs Koopman and one more polynomial through
 * a slicing table, which only a package with its hands on the internals can
 * build for an arbitrary polynomial, so those two are left out.
 *
 * TestArchIEEE and TestArchCastagnoli wait for the hardware paths, and
 * TestGoldenMarshal and TestMarshalTableMismatch for hash state marshaling (#185). The
 * marshaled states are dropped from the golden table until then.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/hash/crc32.h"
#include "burrow/mem/arena.h"

static void make_castagnoli(void *env) {
    (void)env;
    (void)crc32_make_table(NULL, CRC32_CASTAGNOLI);
}

/* First test, so that it can be the one to initialize castagnoliTable. */
static void TestCastagnoliRace(TestingT *t) {
    /* MakeTable(Castagnoli) lazily initializes castagnoliTable in Go, which
     * races with the switch on tab during Write. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash ieee = hash_hash32_as_hash(crc32_new_ieee(a));
    if (ieee.vt == NULL)
        testing_t_fail_now(t);
    go(BURROW_FN(Func, make_castagnoli, NULL));
    char hello[] = "hello";
    hash_write(ieee, slice_from(hello, 5, 5, TYPE_BYTE), NULL);
    arena_free(&ar);
}

static Hash make_ieee(Alloc *a) {
    return hash_hash32_as_hash(crc32_new_ieee(a));
}

static void TestHashInterface(TestingT *t) {
    testhash_without_clone(t, make_ieee);
}

typedef struct Golden {
    uint32_t ieee, castagnoli;
    const char *rep;
    Int rep_len;
    Int count;
} Golden;

#define G(ieee, castagnoli, rep, count)                                                \
    {(ieee), (castagnoli), (rep), (Int)sizeof(rep) - 1, (count)}

static const Golden golden[] = {
    G(0x0, 0x0, "", 1),
    G(0xe8b7be43, 0xc1d04330, "a", 1),
    G(0x9e83486d, 0xe2a22936, "ab", 1),
    G(0x352441c2, 0x364b3fb7, "abc", 1),
    G(0xed82cd11, 0x92c80a31, "abcd", 1),
    G(0x8587d865, 0xc450d697, "abcde", 1),
    G(0x4b8e39ef, 0x53bceff1, "abcdef", 1),
    G(0x312a6aa6, 0xe627f441, "abcdefg", 1),
    G(0xaeef2a50, 0xa9421b7, "abcdefgh", 1),
    G(0x8da988af, 0x2ddc99fc, "abcdefghi", 1),
    G(0x3981703a, 0xe6599437, "abcdefghij", 1),
    G(0x6b9cdfe7, 0xb2cc01fe, "Discard medicine more than two years old.", 1),
    G(0xc90ef73f, 0xe28207f,
      "He who has a shady past knows that nice guys finish last.", 1),
    G(0xb902341f, 0xbe93f964, "I wouldn't marry him with a ten foot pole.", 1),
    G(0x42080e8, 0x9e3be0c3,
      "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave", 1),
    G(0x154c6d11, 0xf505ef04,
      "The days of the digital watch are numbered.  -Tom Stoppard", 1),
    G(0x4c418325, 0x85d3dc82, "Nepal premier won't resign.", 1),
    G(0x33955150, 0xc5142380,
      "For every action there is an equal and opposite government program.", 1),
    G(0x26216a4b, 0x75eb77dd,
      "His money is twice tainted: 'taint yours and 'taint mine.", 1),
    G(0x1abbe45e, 0x91ebe9f7,
      "There is no reason for any individual to have a computer in their home. -Ken "
      "Olsen, 1977",
      1),
    G(0xc89a94f7, 0xf0b1168e,
      "It's a tiny change to the code and not completely disgusting. - Bob Manchek", 1),
    G(0xab3abe14, 0x572b74e2, "size:  a.out:  bad magic", 1),
    G(0xbab102b6, 0x8a58a6d5, "The major problem is with sendmail.  -Mark Horton", 1),
    G(0x999149d7, 0x9c426c50,
      "Give me a rock, paper and scissors and I will move the world.  CCFestoon", 1),
    G(0x6d52a33c, 0x735400a4, "If the enemy is within range, then so are you.", 1),
    G(0x90631e8d, 0xbec49c95,
      "It's well we cannot hear the screams/That we create in others' dreams.", 1),
    G(0x78309130, 0xa95a2079,
      "You remind me of a TV show, but that's all right: I watch it anyway.", 1),
    G(0x7d0a377f, 0xde2e65c5, "C is as portable as Stonehedge!!", 1),
    G(0x8c79fd79, 0x297a88ed,
      "Even if I could be Shakespeare, I think I should still choose to be Faraday. - "
      "A. Huxley",
      1),
    G(0xa20b7167, 0x66ed1d8b,
      "The fugacity of a constituent in a mixture of gases at a given temperature is "
      "proportional to its mole fraction.  Lewis-Randall Rule",
      1),
    G(0x8e0bb443, 0xdcded527,
      "How can you write a big system without C++?  -Paul Glick", 1),
    G(0x1010dab0, 0x8a11661f, "01234567", 1024),
    G(0x772d04d7, 0x5a6f5c45, "a", 1089),
};

enum { ngolden = sizeof golden / sizeof golden[0] };

/* A checksum function under test, with whatever it needs in env. */
typedef uint32_t (*CrcFunc)(const void *env, Slice b);

/* testGoldenIEEE verifies that the given function returns correct IEEE
 * checksums. */
static void test_golden_ieee(TestingT *t, CrcFunc f, const void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < ngolden; i++) {
        const Golden *g = &golden[i];
        Slice in = testhash_repeat(a, g->rep, g->rep_len, g->count, "");
        uint32_t crc = f(env, in);
        if (crc != g->ieee)
            testing_t_errorf_v(t, "IEEE(%s) = 0x%x want 0x%x", testhash_show(a, in),
                               crc, g->ieee);
    }
    arena_free(&ar);
}

/* testGoldenCastagnoli verifies that the given function returns correct
 * CRC-32C checksums. */
static void test_golden_castagnoli(TestingT *t, CrcFunc f, const void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < ngolden; i++) {
        const Golden *g = &golden[i];
        Slice in = testhash_repeat(a, g->rep, g->rep_len, g->count, "");
        uint32_t crc = f(env, in);
        if (crc != g->castagnoli)
            testing_t_errorf_v(t, "Castagnoli(%s) = 0x%x want 0x%x",
                               testhash_show(a, in), crc, g->castagnoli);
    }
    arena_free(&ar);
}

/* testCrossCheck generates random buffers of various lengths and verifies that
 * the two tables give the same crc. */
static void test_cross_check(TestingT *t, const Crc32Table *tab1,
                             const Crc32Table *tab2) {
    static const Int lengths[] = {
        0,    1,    2,    3,    4,    5,    10,   16,   50,   63,   64,   65,
        100,  127,  128,  129,  255,  256,  257,  300,  312,  384,  416,  448,
        480,  500,  501,  502,  503,  504,  505,  512,  513,  1000, 1024, 2000,
        4030, 4031, 4032, 4033, 4036, 4040, 4048, 4096, 5000, 10000};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand rng = testhash_new_rand(t);
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        Slice p = testhash_bytes(a, lengths[i]);
        testhash_read(&rng, p);
        uint32_t crc_init;
        testhash_read(&rng, slice_from(&crc_init, 4, 4, TYPE_BYTE));
        uint32_t crc1 = crc32_update(crc_init, tab1, p);
        uint32_t crc2 = crc32_update(crc_init, tab2, p);
        if (crc1 != crc2)
            testing_t_errorf_v(t, "mismatch: 0x%x vs 0x%x (buffer length %d)", crc1,
                               crc2, lengths[i]);
    }
    arena_free(&ar);
}

static uint32_t update_with(const void *env, Slice b) {
    return crc32_update(0, (const Crc32Table *)env, b);
}

/* A copy of one of the package's tables, which is the same polynomial at an
 * address the package does not recognise, so it goes the simple way. */
static Crc32Table *copy_table(Alloc *a, uint32_t poly) {
    Crc32Table *t = BURROW_NEW(a, Crc32Table);
    *t = *crc32_make_table(NULL, poly);
    return t;
}

static void TestSimple(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_golden_ieee(t, update_with, copy_table(a, CRC32_IEEE));
    test_golden_castagnoli(t, update_with, copy_table(a, CRC32_CASTAGNOLI));
    arena_free(&ar);
}

static void TestSlicing(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Crc32Table *ieee = crc32_ieee_table;
    const Crc32Table *castagnoli = crc32_make_table(NULL, CRC32_CASTAGNOLI);
    test_golden_ieee(t, update_with, ieee);
    test_golden_castagnoli(t, update_with, castagnoli);
    test_cross_check(t, copy_table(a, CRC32_IEEE), ieee);
    test_cross_check(t, copy_table(a, CRC32_CASTAGNOLI), castagnoli);
    arena_free(&ar);
}

/* The simple table for a polynomial nobody names, built by hand, against what
 * MakeTable builds. Not in Go, which has no need to check that its MakeTable
 * and its simpleMakeTable agree, since one calls the other. */
static void TestMakeTable(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint32_t polys[] = {CRC32_IEEE, CRC32_CASTAGNOLI, CRC32_KOOPMAN, 0xD5828281};
    for (size_t i = 0; i < sizeof polys / sizeof polys[0]; i++) {
        const Crc32Table *tab = crc32_make_table(a, polys[i]);
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t crc = n;
            for (int j = 0; j < 8; j++)
                crc = (crc & 1) == 1 ? (crc >> 1) ^ polys[i] : crc >> 1;
            if (tab->v[n] != crc) {
                testing_t_errorf_v(t, "MakeTable(0x%x)[%d] = 0x%x want 0x%x", polys[i],
                                   n, tab->v[n], crc);
                break;
            }
        }
    }
    arena_free(&ar);
}

static uint32_t checksum_ieee(const void *env, Slice b) {
    (void)env;
    return crc32_checksum_ieee(b);
}

typedef struct Split {
    Alloc *a;
    const Crc32Table *tab;
    Int delta;
} Split;

static uint32_t write_split(const void *env, Slice b) {
    const Split *s = env;
    HashHash32 h = crc32_new(s->a, s->tab);
    Hash hh = hash_hash32_as_hash(h);
    if (hh.vt == NULL)
        return 0;
    Int d = s->delta;
    if (d >= b.len)
        d = b.len;
    hash_write(hh, slice_sub(b, 0, d), NULL);
    hash_write(hh, slice_sub(b, d, b.len), NULL);
    return hash_hash32_sum32(h);
}

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_golden_ieee(t, checksum_ieee, NULL);

    for (Int delta = 1; delta <= 7; delta++) {
        Split s = {a, crc32_ieee_table, delta};
        test_golden_ieee(t, write_split, &s);
    }

    const Crc32Table *castagnoli_tab = crc32_make_table(NULL, CRC32_CASTAGNOLI);
    if (castagnoli_tab == NULL)
        testing_t_errorf_v(t, "nil Castagnoli Table");

    /* A delta past every input writes it all at once. */
    Split whole = {a, castagnoli_tab, 1 << 20};
    test_golden_castagnoli(t, write_split, &whole);

    for (Int delta = 1; delta <= 7; delta++) {
        Split s = {a, castagnoli_tab, delta};
        test_golden_castagnoli(t, write_split, &s);
    }
    arena_free(&ar);
}

typedef struct Bench {
    HashHash32 h;
    Int size;
    Int align;
} Bench;

static void benchmark(void *env, TestingB *b) {
    Bench *bn = env;
    Hash h = hash_hash32_as_hash(bn->h);
    testing_b_set_bytes(b, (int64_t)bn->size);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = testhash_bytes(a, bn->size + bn->align);
    data = slice_sub(data, bn->align, data.len);
    for (Int i = 0; i < data.len; i++)
        ((Byte *)data.p)[i] = (Byte)i;
    Slice in = slice_make(a, TYPE_BYTE, 0, hash_size(h));

    hash_reset(h);
    hash_write(h, data, NULL);
    hash_sum(a, h, in);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_reset(h);
        hash_write(h, data, NULL);
        hash_sum(a, h, in);
    }
    arena_free(&ar);
}

static void benchmark_size(void *env, TestingB *b) {
    Bench *bn = env;
    for (Int align = 0; align <= 1; align++) {
        Bench sub = {bn->h, bn->size, align};
        testing_b_run(b, align == 0 ? BURROW_S("align=0") : BURROW_S("align=1"),
                      BURROW_FN(TestingBFunc, benchmark, &sub));
    }
}

static void benchmark_all(void *env, TestingB *b) {
    static const Int sizes[] = {15, 40, 512, 1 << 10, 4 << 10, 32 << 10};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        Int size = sizes[i];
        Str name = size >= 1024 ? fmt_sprintf_v(a, "size=%dkB", size / 1024)
                                : fmt_sprintf_v(a, "size=%d", size);
        Bench bn = {*(HashHash32 *)env, size, 0};
        testing_b_run(b, name, BURROW_FN(TestingBFunc, benchmark_size, &bn));
    }
    arena_free(&ar);
}

static void BenchmarkCRC32(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HashHash32 ieee = crc32_new_ieee(a);
    HashHash32 castagnoli = crc32_new(a, crc32_make_table(a, CRC32_CASTAGNOLI));
    HashHash32 koopman = crc32_new(a, crc32_make_table(a, CRC32_KOOPMAN));
    testing_b_run(b, BURROW_S("poly=IEEE"),
                  BURROW_FN(TestingBFunc, benchmark_all, &ieee));
    testing_b_run(b, BURROW_S("poly=Castagnoli"),
                  BURROW_FN(TestingBFunc, benchmark_all, &castagnoli));
    testing_b_run(b, BURROW_S("poly=Koopman"),
                  BURROW_FN(TestingBFunc, benchmark_all, &koopman));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCastagnoliRace)                                                              \
    X(TestHashInterface)                                                               \
    X(TestSimple)                                                                      \
    X(TestSlicing)                                                                     \
    X(TestMakeTable)                                                                   \
    X(TestGolden)                                                                      \
    X(BenchmarkCRC32)

TESTING_MAIN(TESTS)
