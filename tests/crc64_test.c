/* Derived from Go's src/hash/crc64/crc64_test.go.
 * Go source: go1.27.1.
 *
 * TestSlicing is not in Go: it checks the eight bytes at a time path
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
#include "burrow/encoding.h"
#include "burrow/hash/crc64.h"
#include "burrow/mem/arena.h"

#include <string.h>

static Hash make_iso(Alloc *a) {
    return hash_hash64_as_hash(crc64_new(a, crc64_make_table(NULL, CRC64_ISO)));
}

static void TestCRC64Hash(TestingT *t) {
    testhash_hash(t, make_iso);
}

typedef struct Golden {
    uint64_t out_iso;
    uint64_t out_ecma;
    const char *in;
    Str half_state_iso, half_state_ecma;
} Golden;

static const Golden golden[] = {
    {0x0, 0x0, "",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x00\x00\x00\x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x00\x00\x00\x00\x00\x00\x00\x00")},
    {0x3420000000000000, 0x330284772e652b05, "a",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x00\x00\x00\x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x00\x00\x00\x00\x00\x00\x00\x00")},
    {0x36c4200000000000, 0xbc6573200e84b046, "ab",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x34 \x00\x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x33\x02\x84w.e+\x05")},
    {0x3776c42000000000, 0x2cd8094a1a277627, "abc",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x34 \x00\x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x33\x02\x84w.e+\x05")},
    {0x336776c420000000, 0x3c9d28596e5960ba, "abcd",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x36\xc4 \x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xbc\x65s \x0e\x84\xb0\x46")},
    {0x32d36776c4200000, 0x40bdf58fb0895f2, "abcde",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x36\xc4 \x00\x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xbc\x65s \x0e\x84\xb0\x46")},
    {0x3002d36776c42000, 0xd08e9f8545a700f4, "abcdef",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x37v\xc4 \x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65,\xd8\x09J\x1a'v'")},
    {0x31b002d36776c420, 0xec20a3a8cc710e66, "abcdefg",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x37v\xc4 \x00\x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65,\xd8\x09J\x1a'v'")},
    {0xe21b002d36776c4, 0x67b4f30a647a0c59, "abcdefgh",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x33gv\xc4 \x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65<\x9d(YnY`\xba")},
    {0x8b6e21b002d36776, 0x9966f6c89d56ef8e, "abcdefghi",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x33gv\xc4 \x00\x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65<\x9d(YnY`\xba")},
    {0x7f5b6e21b002d367, 0x32093a2ecd5773f4, "abcdefghij",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x32\xd3gv\xc4 \x00\x00"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x04\x0b\xdfX\xfb\x08\x95\xf2")},
    {0x8ec0e7c835bf9cdf, 0x8a0825223ea6d221,
     "Discard medicine more than two years old.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xc6\xc0\x0c\xac'\x11\x12\xd5"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xfd%\xc0&\xa0R\xef\x95")},
    {0xc7db1759e2be5ab4, 0x8562c0ac2ab9a00d,
     "He who has a shady past knows that nice guys finish last.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x09\xcb\xd1\x35X[r\x09"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x07\x02\xe8|+\xc1\x06\xe3")},
    {0xfbf9d9603a6fa020, 0x3ee2a39c083f38b4,
     "I wouldn't marry him with a ten foot pole.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x19\xc8\x64\xbe\x84\x14\x87_"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xcb\xb7\xd3\xeeG\xdc\x45\x8c")},
    {0xeafc4211a6daa0ef, 0x1f603830353e518a,
     "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xad\x1b*\xc0\xb1\xf3i("),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xa7\x8a\xdb\xf6\xd2R\x09\x96")},
    {0x3e05b21c7a4dc4da, 0x2fd681d7b2421fd,
     "The days of the digital watch are numbered.  -Tom Stoppard",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xefv78\x1ak\x02\x8f\xff"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65T\xcbl\x10\xfb\x87K*")},
    {0x5255866ad6ef28a6, 0x790ef2b16a745a41, "Nepal premier won't resign.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xcb\x66\x11R\xbfh\xde\xc9"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x36\x13\xd9\x8f\x06_\xbd\x9a")},
    {0x8a79895be1e9c361, 0x3ef8f06daccdcddf,
     "For every action there is an equal and opposite government program.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xf3pV\x01\x63_Wu"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xe7\xc6\x0a\x08\x12\x46L\xa0")},
    {0x8878963a649d4916, 0x49e41b2660b106d,
     "His money is twice tainted: 'taint yours and 'taint mine.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xc3\xb1\xff\xf1\xe0/\xce\x94"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65OL/\xb1\xec\xa2\x14\x87")},
    {0xa7b9d53ea87eb82f, 0x561cc0cfa235ac68,
     "There is no reason for any individual to have a computer in their home. -Ken "
     "Olsen, 1977",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xdd\xb8\x61\xe1\xb5\xf8\xb9W"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x87)GQ\x03\xf4K\x09")},
    {0xdb6805c0966a2f9c, 0xd4fe9ef082e69f59,
     "It's a tiny change to the code and not completely disgusting. - Bob Manchek",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xefV\xba\x12\x91\x81\x1fNU"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x0a\xb8\x81v\?\xdeL\xcb")},
    {0xf3553c65dacdadd2, 0xe3b5e46cd8d63a4d, "size:  a.out:  bad magic",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xefG\xad\xbc\xb2\xa8y\xc9\xdc"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xcc\xce\xe5\xe6\x89p\x01\xb8")},
    {0x9d5e034087a676b9, 0x865aaf6b94f2a051,
     "The major problem is with sendmail.  -Mark Horton",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xa2\xacn\x8aT;&\xd5"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x46\x66\x9c\x1f\xc9x\xbf\x61")},
    {0xa6db2d7f8da96417, 0x7eca10d2f8136eb4,
     "Give me a rock, paper and scissors and I will move the world.  CCFestoon",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xeb\x18\xbf\xf9}\x91\xe5|"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x61\x9e\x05:\xce[\xe7\x19")},
    {0x325e00cd2fe819f9, 0xd7dd118c98e98727,
     "If the enemy is within range, then so are you.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef^5k\xd0\x41j_{"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\x0b#\x99\xa8r\x83YR")},
    {0x88c6600ce58ae4c6, 0x70fb33c119c29318,
     "It's well we cannot hear the screams/That we create in others' dreams.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef|\xb5\x02\xdcw\x18/\x86"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65]\x9d-\xed\x8c\xf9r9")},
    {0x28c4a3f3b769e078, 0x57c891e39a97d9b7,
     "You remind me of a TV show, but that's all right: I watch it anyway.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x03\x8b\x64\x1c\xb0_\x16\x98"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xafW\x98\xaa\"\xe7\xd7|")},
    {0xa698a34c9d9f1dca, 0xa1f46ba20ad06eb7, "C is as portable as Stonehedge!!",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef.P\xe1I\xc6pi\xdc"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xd6\x9a\x06\x01(\xc0\x1e\x8b")},
    {0xf6c1e2a8c26c5cfc, 0x7ad25fafa1710407,
     "Even if I could be Shakespeare, I think I should still choose to be Faraday. - "
     "A. Huxley",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\xf7\xa0\x34\x8a\xf2o\xe0;"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65<[\xd2%\x9em\x94\x04")},
    {0xd402559dfe9b70c, 0x73cef1666185c13f,
     "The fugacity of a constituent in a mixture of gases at a given temperature is "
     "proportional to its mole fraction.  Lewis-Randall Rule",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x7f\xae\xb9\xbaX=\x19v"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xb2\xcb\xa6Y\xc5\xd0G\x03")},
    {0xdb6efff26aa94946, 0xb41858f73c389602,
     "How can you write a big system without C++?  -Paul Glick",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef\x61\xed$js\xb9\xa5\x41"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65Zm\x96\x8a\xe2\xaf\x13p")},
    {0xe7fcf1006b503b61, 0x27db187fc15bbc72,
     "This is a test of the emergency broadcast system.",
     BURROW_S_INIT("crc\x02s\xba\x84\x84\xbb\xcd]\xef}\xee[q\x16\xcb\xe4\x8d"),
     BURROW_S_INIT("crc\x02`&\x9aR\xe1\xb7\xfe\x65\xb1\x93] \xeb\xa9\x61m")},
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

/* Not in Go: Go writes TestGoldenMarshal's two subtests out in full. They differ
 * only in the table, the golden state and the name in the messages. */
typedef struct MarshalCase {
    const char *name;
    uint64_t poly;
    bool ecma;
} MarshalCase;

static void golden_marshal(void *env, TestingT *t) {
    const MarshalCase *tt = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Crc64Table *table = crc64_make_table(a, tt->poly);
    for (size_t i = 0; i < sizeof golden / sizeof golden[0]; i++) {
        const Golden *g = &golden[i];
        Str want = tt->ecma ? g->half_state_ecma : g->half_state_iso;
        Str in = str_from_cstr(g->in);
        Slice p = slice_from_str(a, in);
        HashHash64 h = crc64_new(a, table);
        HashHash64 h2 = crc64_new(a, table);
        Hash hh = hash_hash64_as_hash(h);
        Hash hh2 = hash_hash64_as_hash(h2);
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
            testing_t_errorf_v(t, "%s crc64(%q) state = %q, want %q", tt->name, in, st,
                               want);
            continue;
        }

        if (!str_eq(sa, want)) {
            testing_t_errorf_v(t, "%s crc64(%q) state = %q, want %q", tt->name, in, sa,
                               want);
            continue;
        }

        err = encoding_unmarshal_binary(a, ha2, state);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not unmarshal: %v", err);
            continue;
        }

        hash_write(hh, slice_sub(p, p.len / 2, p.len), NULL);
        hash_write(hh2, slice_sub(p, p.len / 2, p.len), NULL);

        if (hash_hash64_sum64(h) != hash_hash64_sum64(h2))
            testing_t_errorf_v(t, "%s crc64(%s) = 0x%x != marshaled (0x%x)", tt->name,
                               in, hash_hash64_sum64(h), hash_hash64_sum64(h2));
    }
    arena_free(&ar);
}

static void TestGoldenMarshal(TestingT *t) {
    MarshalCase iso = {"ISO", CRC64_ISO, false};
    MarshalCase ecma = {"ECMA", CRC64_ECMA, true};
    testing_t_run(t, BURROW_S("ISO"), BURROW_FN(TestingTFunc, golden_marshal, &iso));
    testing_t_run(t, BURROW_S("ECMA"), BURROW_FN(TestingTFunc, golden_marshal, &ecma));
}

static void TestMarshalTableMismatch(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h1 = hash_hash64_as_hash(crc64_new(a, crc64_make_table(a, CRC64_ISO)));
    Hash h2 = hash_hash64_as_hash(crc64_new(a, crc64_make_table(a, CRC64_ECMA)));

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
    X(TestGoldenMarshal)                                                               \
    X(TestMarshalTableMismatch)                                                        \
    X(TestSlicing)                                                                     \
    X(BenchmarkCrc64)

TESTING_MAIN(TESTS)
