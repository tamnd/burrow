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
 * TestArchIEEE and TestArchCastagnoli wait for the hardware paths.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/encoding.h"
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
    testhash_hash(t, make_ieee);
}

typedef struct Golden {
    uint32_t ieee, castagnoli;
    const char *rep;
    Int rep_len;
    Int count;
    Str half_state_ieee, half_state_castagnoli;
} Golden;

#define G(ieee, castagnoli, rep, count, half_ieee, half_cast)                          \
    {(ieee), (castagnoli), (rep), (Int)sizeof(rep) - 1, (count), half_ieee, half_cast}

static const Golden golden[] = {
    G(0x0, 0x0, "", 1, BURROW_S_INIT("crc\x01\xca\x87\x91M\x00\x00\x00\x00"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x00\x00\x00\x00")),
    G(0xe8b7be43, 0xc1d04330, "a", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x00\x00\x00\x00"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x00\x00\x00\x00")),
    G(0x9e83486d, 0xe2a22936, "ab", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xe8\xb7\xbe\x43"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xc1\xd0\x43\x30")),
    G(0x352441c2, 0x364b3fb7, "abc", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xe8\xb7\xbe\x43"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xc1\xd0\x43\x30")),
    G(0xed82cd11, 0x92c80a31, "abcd", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x9e\x83Hm"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xe2\xa2)6")),
    G(0x8587d865, 0xc450d697, "abcde", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x9e\x83Hm"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xe2\xa2)6")),
    G(0x4b8e39ef, 0x53bceff1, "abcdef", 1, BURROW_S_INIT("crc\x01\xca\x87\x91M5$A\xc2"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x36K\?\xb7")),
    G(0x312a6aa6, 0xe627f441, "abcdefg", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M5$A\xc2"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x36K\?\xb7")),
    G(0xaeef2a50, 0xa9421b7, "abcdefgh", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xed\x82\xcd\x11"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x92\xc8\x0a\x31")),
    G(0x8da988af, 0x2ddc99fc, "abcdefghi", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xed\x82\xcd\x11"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x92\xc8\x0a\x31")),
    G(0x3981703a, 0xe6599437, "abcdefghij", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x85\x87\xd8\x65"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xc4P\xd6\x97")),
    G(0x6b9cdfe7, 0xb2cc01fe, "Discard medicine more than two years old.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xfd\xe5\xc2J"),
      BURROW_S_INIT("crc\x01wB\x84\x81S\"(\xe0")),
    G(0xc90ef73f, 0xe28207f,
      "He who has a shady past knows that nice guys finish last.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x01\xc7\x8b+"),
      BURROW_S_INIT("crc\x01wB\x84\x81'\xdaR\x15")),
    G(0xb902341f, 0xbe93f964, "I wouldn't marry him with a ten foot pole.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x9d\x13\xce\x10"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xc3\xed\xabG")),
    G(0x42080e8, 0x9e3be0c3,
      "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M-\xed\xf7\x94"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xce\xce\x62\x81")),
    G(0x154c6d11, 0xf505ef04,
      "The days of the digital watch are numbered.  -Tom Stoppard", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91MOa\xa5\x0d"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xd3s\x9dP")),
    G(0x4c418325, 0x85d3dc82, "Nepal premier won't resign.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xa8S9\x85"),
      BURROW_S_INIT("crc\x01wB\x84\x81{\x90\x8a\x14")),
    G(0x33955150, 0xc5142380,
      "For every action there is an equal and opposite government program.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91Ma\xe9>\x86"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xaa@\xc4\x1c")),
    G(0x26216a4b, 0x75eb77dd,
      "His money is twice tainted: 'taint yours and 'taint mine.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\\\x1an\x88"),
      BURROW_S_INIT("crc\x01wB\x84\x81W\x07\x38Z")),
    G(0x1abbe45e, 0x91ebe9f7,
      "There is no reason for any individual to have a computer in their home. -Ken "
      "Olsen, 1977",
      1, BURROW_S_INIT("crc\x01\xca\x87\x91M\xb7\xf5\xf2\xca"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xc4o\x9d\x85")),
    G(0xc89a94f7, 0xf0b1168e,
      "It's a tiny change to the code and not completely disgusting. - Bob Manchek", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x84g1\xe8"),
      BURROW_S_INIT("crc\x01wB\x84\x81#\x98\x0c\xab")),
    G(0xab3abe14, 0x572b74e2, "size:  a.out:  bad magic", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x8a\x0f\xad\x08"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x80\xc9n\xd8")),
    G(0xbab102b6, 0x8a58a6d5, "The major problem is with sendmail.  -Mark Horton", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x07\xf0\xb3\x15"),
      BURROW_S_INIT("crc\x01wB\x84\x81liS\xcc")),
    G(0x999149d7, 0x9c426c50,
      "Give me a rock, paper and scissors and I will move the world.  CCFestoon", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x0f\x61\xbc."),
      BURROW_S_INIT("crc\x01wB\x84\x81\xdb\xcd\x8f\x43")),
    G(0x6d52a33c, 0x735400a4, "If the enemy is within range, then so are you.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91My\x1b\x99\xf8"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xaa\x42\x03\x37")),
    G(0x90631e8d, 0xbec49c95,
      "It's well we cannot hear the screams/That we create in others' dreams.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x08qfY"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x16y\xa1\xd2")),
    G(0x78309130, 0xa95a2079,
      "You remind me of a TV show, but that's all right: I watch it anyway.", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xbdO,\xc2"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x66&\xc5\xe4")),
    G(0x7d0a377f, 0xde2e65c5, "C is as portable as Stonehedge!!", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xf7\xd6\x00\xd5"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x64\x65\\\xf8")),
    G(0x8c79fd79, 0x297a88ed,
      "Even if I could be Shakespeare, I think I should still choose to be Faraday. - "
      "A. Huxley",
      1, BURROW_S_INIT("crc\x01\xca\x87\x91Ml+\xb8\xa7"),
      BURROW_S_INIT("crc\x01wB\x84\x81\xbf\xd6S\xdd")),
    G(0xa20b7167, 0x66ed1d8b,
      "The fugacity of a constituent in a mixture of gases at a given temperature is "
      "proportional to its mole fraction.  Lewis-Randall Rule",
      1, BURROW_S_INIT("crc\x01\xca\x87\x91M<lR["),
      BURROW_S_INIT("crc\x01wB\x84\x81{\xaco\xb1")),
    G(0x8e0bb443, 0xdcded527,
      "How can you write a big system without C++?  -Paul Glick", 1,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x0e\x88\x89\xed"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x33\xd7\x43\x7f")),
    G(0x1010dab0, 0x8a11661f, "01234567", 1024,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\x92\xe5\xba\xf3"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x1a\x02\x88Y")),
    G(0x772d04d7, 0x5a6f5c45, "a", 1089,
      BURROW_S_INIT("crc\x01\xca\x87\x91M\xe7\xd0\x89\xd1"),
      BURROW_S_INIT("crc\x01wB\x84\x81\x95\x42\xa9(")),
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

/* Not in Go: Go writes TestGoldenMarshal's two subtests out in full. They differ
 * only in the table, the golden state and the name in the messages. */
typedef struct MarshalCase {
    const char *name;
    uint32_t poly;
    bool castagnoli;
} MarshalCase;

static void golden_marshal(void *env, TestingT *t) {
    const MarshalCase *tt = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Crc32Table *table = crc32_make_table(a, tt->poly);
    for (int i = 0; i < ngolden; i++) {
        const Golden *g = &golden[i];
        Str want = tt->castagnoli ? g->half_state_castagnoli : g->half_state_ieee;
        Slice p = testhash_repeat(a, g->rep, g->rep_len, g->count, "");
        Str in = {(const Byte *)p.p, p.len};
        HashHash32 h = crc32_new(a, table);
        HashHash32 h2 = crc32_new(a, table);
        Hash hh = hash_hash32_as_hash(h);
        Hash hh2 = hash_hash32_as_hash(h2);
        Any ha = BURROW_ANY(TYPE_OF(Hash), &hh);
        Any ha2 = BURROW_ANY(TYPE_OF(Hash), &hh2);

        hash_write(hh, slice_sub(p, 0, p.len / 2), NULL);

        Error err = BURROW_NO_ERROR;
        Slice state = encoding_marshal_binary(a, ha, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not marshal: %v", err);
            continue;
        }

        Slice state_append =
            encoding_append_binary(a, ha, slice_make(a, TYPE_BYTE, 4, 32), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not marshal: %v", err);
            continue;
        }
        state_append = slice_sub(state_append, 4, state_append.len);

        Str st = {(const Byte *)state.p, state.len};
        Str sa = {(const Byte *)state_append.p, state_append.len};
        if (!str_eq(st, want)) {
            testing_t_errorf_v(t, "%s(%q) state = %q, want %q", tt->name, in, st, want);
            continue;
        }

        if (!str_eq(sa, want)) {
            testing_t_errorf_v(t, "%s(%q) state = %q, want %q", tt->name, in, sa, want);
            continue;
        }

        err = encoding_unmarshal_binary(a, ha2, state);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not unmarshal: %v", err);
            continue;
        }

        hash_write(hh, slice_sub(p, p.len / 2, p.len), NULL);
        hash_write(hh2, slice_sub(p, p.len / 2, p.len), NULL);

        if (hash_hash32_sum32(h) != hash_hash32_sum32(h2))
            testing_t_errorf_v(t, "%s(%s) = 0x%x != marshaled 0x%x", tt->name, in,
                               hash_hash32_sum32(h), hash_hash32_sum32(h2));
    }
    arena_free(&ar);
}

static void TestGoldenMarshal(TestingT *t) {
    MarshalCase ieee = {"IEEE", CRC32_IEEE, false};
    MarshalCase castagnoli = {"Castagnoli", CRC32_CASTAGNOLI, true};
    testing_t_run(t, BURROW_S("IEEE"), BURROW_FN(TestingTFunc, golden_marshal, &ieee));
    testing_t_run(t, BURROW_S("Castagnoli"),
                  BURROW_FN(TestingTFunc, golden_marshal, &castagnoli));
}

static void TestMarshalTableMismatch(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h1 = hash_hash32_as_hash(crc32_new(a, crc32_ieee_table));
    Hash h2 = hash_hash32_as_hash(crc32_new(a, crc32_make_table(a, CRC32_CASTAGNOLI)));

    Error err = BURROW_NO_ERROR;
    Slice state1 = encoding_marshal_binary(a, BURROW_ANY(TYPE_OF(Hash), &h1), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "could not marshal: %v", err);

    err = encoding_unmarshal_binary(a, BURROW_ANY(TYPE_OF(Hash), &h2), state1);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "no error when one was expected");
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
    X(TestGoldenMarshal)                                                               \
    X(TestMarshalTableMismatch)                                                        \
    X(TestSlicing)                                                                     \
    X(TestMakeTable)                                                                   \
    X(TestGolden)                                                                      \
    X(BenchmarkCRC32)

TESTING_MAIN(TESTS)
