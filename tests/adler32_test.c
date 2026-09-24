/* Derived from Go's src/hash/adler32/adler32_test.go.
 * Go source: go1.27.1.
 *
 * TestGoldenMarshal is not here yet. It needs the hash state to marshal, which
 * waits on encoding.BinaryMarshaler and a way to ask a Hash for it (#185). Its golden states are dropped from the
 * table until then.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/hash/adler32.h"
#include "burrow/mem/arena.h"

static Hash make_adler32(Alloc *a) {
    return hash_hash32_as_hash(adler32_new(a));
}

static void TestHashInterface(TestingT *t) {
    testhash_without_clone(t, make_adler32);
}

/* The input is rep written count times and then tail, which is how the table
 * says strings.Repeat("\xff", 5548) + "8" without 5 KiB of literal. */
typedef struct Golden {
    uint32_t out;
    const char *rep;
    Int rep_len;
    Int count;
    const char *tail;
} Golden;

#define G(out, rep, count, tail) {(out), (rep), (Int)sizeof(rep) - 1, (count), (tail)}

static const Golden golden[] = {
    G(0x00000001, "", 1, ""),
    G(0x00620062, "a", 1, ""),
    G(0x012600c4, "ab", 1, ""),
    G(0x024d0127, "abc", 1, ""),
    G(0x03d8018b, "abcd", 1, ""),
    G(0x05c801f0, "abcde", 1, ""),
    G(0x081e0256, "abcdef", 1, ""),
    G(0x0adb02bd, "abcdefg", 1, ""),
    G(0x0e000325, "abcdefgh", 1, ""),
    G(0x118e038e, "abcdefghi", 1, ""),
    G(0x158603f8, "abcdefghij", 1, ""),
    G(0x3f090f02, "Discard medicine more than two years old.", 1, ""),
    G(0x46d81477, "He who has a shady past knows that nice guys finish last.", 1, ""),
    G(0x40ee0ee1, "I wouldn't marry him with a ten foot pole.", 1, ""),
    G(0x16661315, "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave", 1, ""),
    G(0x5b2e1480, "The days of the digital watch are numbered.  -Tom Stoppard", 1, ""),
    G(0x8c3c09ea, "Nepal premier won't resign.", 1, ""),
    G(0x45ac18fd, "For every action there is an equal and opposite government program.",
      1, ""),
    G(0x53c61462, "His money is twice tainted: 'taint yours and 'taint mine.", 1, ""),
    G(0x7e511e63,
      "There is no reason for any individual to have a computer in their home. -Ken "
      "Olsen, 1977",
      1, ""),
    G(0xe4801a6a,
      "It's a tiny change to the code and not completely disgusting. - Bob Manchek", 1,
      ""),
    G(0x61b507df, "size:  a.out:  bad magic", 1, ""),
    G(0xb8631171, "The major problem is with sendmail.  -Mark Horton", 1, ""),
    G(0x8b5e1904,
      "Give me a rock, paper and scissors and I will move the world.  CCFestoon", 1,
      ""),
    G(0x7cc6102b, "If the enemy is within range, then so are you.", 1, ""),
    G(0x700318e7,
      "It's well we cannot hear the screams/That we create in others' dreams.", 1, ""),
    G(0x1e601747,
      "You remind me of a TV show, but that's all right: I watch it anyway.", 1, ""),
    G(0xb55b0b09, "C is as portable as Stonehedge!!", 1, ""),
    G(0x39111dd0,
      "Even if I could be Shakespeare, I think I should still choose to be Faraday. - "
      "A. Huxley",
      1, ""),
    G(0x91dd304f,
      "The fugacity of a constituent in a mixture of gases at a given temperature is "
      "proportional to its mole fraction.  Lewis-Randall Rule",
      1, ""),
    G(0x2e5d1316, "How can you write a big system without C++?  -Paul Glick", 1, ""),
    G(0xd0201df6,
      "'Invariant assertions' is the most elegant programming technique!  -Tom "
      "Szymanski",
      1, ""),
    G(0x211297c8, "\xff", 5548, "8"),
    G(0xbaa198c8, "\xff", 5549, "9"),
    G(0x553499be, "\xff", 5550, "0"),
    G(0xf0c19abe, "\xff", 5551, "1"),
    G(0x8d5c9bbe, "\xff", 5552, "2"),
    G(0x2af69cbe, "\xff", 5553, "3"),
    G(0xc9809dbe, "\xff", 5554, "4"),
    G(0x69189ebe, "\xff", 5555, "5"),
    G(0x86af0001, "\x00", 100000, ""),
    G(0x79660b4d, "a", 100000, ""),
    G(0x110588ee, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 10000, ""),
};

/* checksum is a slow but simple implementation of the Adler-32 checksum. It is
 * a straight port of the sample code in RFC 1950 section 9. */
static uint32_t checksum(Slice p) {
    uint32_t s1 = 1, s2 = 0;
    const Byte *b = (const Byte *)p.p;
    for (Int i = 0; i < p.len; i++) {
        s1 = (s1 + b[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return s2 << 16 | s1;
}

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof golden / sizeof golden[0]; i++) {
        const Golden *g = &golden[i];
        Slice p = testhash_repeat(a, g->rep, g->rep_len, g->count, g->tail);
        Str in = testhash_show(a, p);
        uint32_t got = checksum(p);
        if (got != g->out) {
            testing_t_errorf_v(t,
                               "simple implementation: checksum(%q) = 0x%x want 0x%x",
                               in, got, g->out);
            continue;
        }
        got = adler32_checksum(p);
        if (got != g->out) {
            testing_t_errorf_v(
                t, "optimized implementation: Checksum(%q) = 0x%x want 0x%x", in, got,
                g->out);
            continue;
        }
    }
    arena_free(&ar);
}

static void BenchmarkAdler32KB(TestingB *b) {
    testing_b_set_bytes(b, 1024);
    Byte data[1024];
    for (int i = 0; i < 1024; i++)
        data[i] = (Byte)i;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HashHash32 h = adler32_new(a);
    Hash hh = hash_hash32_as_hash(h);
    if (hh.vt == NULL)
        testing_b_fail_now(b);
    Slice in = slice_make(a, TYPE_BYTE, 0, hash_size(hh));
    Slice p = slice_from(data, 1024, 1024, TYPE_BYTE);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_reset(hh);
        hash_write(hh, p, NULL);
        hash_sum(a, hh, in);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHashInterface)                                                               \
    X(TestGolden)                                                                      \
    X(BenchmarkAdler32KB)

TESTING_MAIN(TESTS)
