/* Derived from Go's src/hash/fnv/fnv_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/encoding.h"
#include "burrow/hash/fnv.h"
#include "burrow/mem/arena.h"

#include <string.h>

static Hash make32(Alloc *a) {
    return hash_hash32_as_hash(fnv_new32(a));
}

static Hash make32a(Alloc *a) {
    return hash_hash32_as_hash(fnv_new32a(a));
}

static Hash make64(Alloc *a) {
    return hash_hash64_as_hash(fnv_new64(a));
}

static Hash make64a(Alloc *a) {
    return hash_hash64_as_hash(fnv_new64a(a));
}

static void run_testhash(void *env, TestingT *t) {
    testhash_hash(t, *(TesthashMake *)env);
}

static void TestHashInterface(TestingT *t) {
    static const char *names[] = {"32", "32a", "64", "64a", "128", "128a"};
    TesthashMake fns[] = {make32, make32a, make64, make64a, fnv_new128, fnv_new128a};
    for (int i = 0; i < 6; i++)
        testing_t_run(t, str_from_cstr(names[i]),
                      BURROW_FN(TestingTFunc, run_testhash, &fns[i]));
}

typedef struct Golden {
    const char *out;
    Int out_len;
    const char *in;
    Str half_state; /* marshaled hash state after first half of in written */
} Golden;

#define G(out, in, half) {(out), (Int)sizeof(out) - 1, (in), half}

static const Golden golden32[] = {
    G("\x81\x1c\x9d\xc5", "", BURROW_S_INIT("fnv\x01\x81\x1c\x9d\xc5")),
    G("\x05\x0c\x5d\x7e", "a", BURROW_S_INIT("fnv\x01\x81\x1c\x9d\xc5")),
    G("\x70\x77\x2d\x38", "ab", BURROW_S_INIT("fnv\x01\x05\x0c]~")),
    G("\x43\x9c\x2f\x4b", "abc", BURROW_S_INIT("fnv\x01\x05\x0c]~")),
};

static const Golden golden32a[] = {
    G("\x81\x1c\x9d\xc5", "", BURROW_S_INIT("fnv\x02\x81\x1c\x9d\xc5")),
    G("\xe4\x0c\x29\x2c", "a", BURROW_S_INIT("fnv\x02\x81\x1c\x9d\xc5")),
    G("\x4d\x25\x05\xca", "ab", BURROW_S_INIT("fnv\x02\xe4\x0c),")),
    G("\x1a\x47\xe9\x0b", "abc", BURROW_S_INIT("fnv\x02\xe4\x0c),")),
};

static const Golden golden64[] = {
    G("\xcb\xf2\x9c\xe4\x84\x22\x23\x25", "",
      BURROW_S_INIT("fnv\x03\xcb\xf2\x9c\xe4\x84\"#%")),
    G("\xaf\x63\xbd\x4c\x86\x01\xb7\xbe", "a",
      BURROW_S_INIT("fnv\x03\xcb\xf2\x9c\xe4\x84\"#%")),
    G("\x08\x32\x67\x07\xb4\xeb\x37\xb8", "ab",
      BURROW_S_INIT("fnv\x03\xaf\x63\xbdL\x86\x01\xb7\xbe")),
    G("\xd8\xdc\xca\x18\x6b\xaf\xad\xcb", "abc",
      BURROW_S_INIT("fnv\x03\xaf\x63\xbdL\x86\x01\xb7\xbe")),
};

static const Golden golden64a[] = {
    G("\xcb\xf2\x9c\xe4\x84\x22\x23\x25", "",
      BURROW_S_INIT("fnv\x04\xcb\xf2\x9c\xe4\x84\"#%")),
    G("\xaf\x63\xdc\x4c\x86\x01\xec\x8c", "a",
      BURROW_S_INIT("fnv\x04\xcb\xf2\x9c\xe4\x84\"#%")),
    G("\x08\x9c\x44\x07\xb5\x45\x98\x6a", "ab",
      BURROW_S_INIT("fnv\x04\xaf\x63\xdcL\x86\x01\xec\x8c")),
    G("\xe7\x1f\xa2\x19\x05\x41\x57\x4b", "abc",
      BURROW_S_INIT("fnv\x04\xaf\x63\xdcL\x86\x01\xec\x8c")),
};

static const Golden golden128[] = {
    G("\x6c\x62\x27\x2e\x07\xbb\x01\x42\x62\xb8\x21\x75\x62\x95\xc5\x8d", "",
      BURROW_S_INIT("fnv\x05lb'.\x07\xbb\x01\x42\x62\xb8!ub\x95\xc5\x8d")),
    G("\xd2\x28\xcb\x69\x10\x1a\x8c\xaf\x78\x91\x2b\x70\x4e\x4a\x14\x1e", "a",
      BURROW_S_INIT("fnv\x05lb'.\x07\xbb\x01\x42\x62\xb8!ub\x95\xc5\x8d")),
    G("\x08\x80\x94\x5a\xee\xab\x1b\xe9\x5a\xa0\x73\x30\x55\x26\xc0\x88", "ab",
      BURROW_S_INIT("fnv\x05\xd2(\xcbi\x10\x1a\x8c\xafx\x91+pNJ\x14\x1e")),
    G("\xa6\x8b\xb2\xa4\x34\x8b\x58\x22\x83\x6d\xbc\x78\xc6\xae\xe7\x3b", "abc",
      BURROW_S_INIT("fnv\x05\xd2(\xcbi\x10\x1a\x8c\xafx\x91+pNJ\x14\x1e")),
};

static const Golden golden128a[] = {
    G("\x6c\x62\x27\x2e\x07\xbb\x01\x42\x62\xb8\x21\x75\x62\x95\xc5\x8d", "",
      BURROW_S_INIT("fnv\x06lb'.\x07\xbb\x01\x42\x62\xb8!ub\x95\xc5\x8d")),
    G("\xd2\x28\xcb\x69\x6f\x1a\x8c\xaf\x78\x91\x2b\x70\x4e\x4a\x89\x64", "a",
      BURROW_S_INIT("fnv\x06lb'.\x07\xbb\x01\x42\x62\xb8!ub\x95\xc5\x8d")),
    G("\x08\x80\x95\x44\xbb\xab\x1b\xe9\x5a\xa0\x73\x30\x55\xb6\x9a\x62", "ab",
      BURROW_S_INIT("fnv\x06\xd2(\xcbio\x1a\x8c\xafx\x91+pNJ\x89\x64")),
    G("\xa6\x8d\x62\x2c\xec\x8b\x58\x22\x83\x6d\xbc\x79\x77\xaf\x7f\x3b", "abc",
      BURROW_S_INIT("fnv\x06\xd2(\xcbio\x1a\x8c\xafx\x91+pNJ\x89\x64")),
};

#define NGOLD(g) (sizeof(g) / sizeof((g)[0]))

static void test_golden(TestingT *t, Hash hash, const Golden *gold, size_t n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < n; i++) {
        const Golden *g = &gold[i];
        hash_reset(hash);
        Str in = str_from_cstr(g->in);
        Error error = BURROW_NO_ERROR;
        Int done = hash_write(hash, slice_from_str(a, in), &error);
        if (BURROW_FAILED(error))
            testing_t_fatalf_v(t, "write error: %s", error);
        if (done != in.len)
            testing_t_fatalf_v(t, "wrote only %d out of %d bytes", done, in.len);
        Slice actual = hash_sum(a, hash, slice_nil(TYPE_BYTE));
        Str want = {(const Byte *)g->out, g->out_len};
        Slice out = slice_from_str(a, want);
        if (!testhash_equal(out, actual))
            testing_t_errorf_v(t, "hash(%q) = 0x%x want 0x%x", in, actual, out);
    }
    arena_free(&ar);
}

/* Runs one test against a hash made on an arena that lives as long as the
 * test does. */
#define WITH_HASH(t, make, body)                                                       \
    do {                                                                               \
        Arena ar_;                                                                     \
        arena_init(&ar_, NULL, 0);                                                     \
        Hash h_ = make(arena_allocator(&ar_));                                         \
        body;                                                                          \
        arena_free(&ar_);                                                              \
    } while (0)

static void TestGolden32(TestingT *t) {
    WITH_HASH(t, make32, test_golden(t, h_, golden32, NGOLD(golden32)));
}

static void TestGolden32a(TestingT *t) {
    WITH_HASH(t, make32a, test_golden(t, h_, golden32a, NGOLD(golden32a)));
}

static void TestGolden64(TestingT *t) {
    WITH_HASH(t, make64, test_golden(t, h_, golden64, NGOLD(golden64)));
}

static void TestGolden64a(TestingT *t) {
    WITH_HASH(t, make64a, test_golden(t, h_, golden64a, NGOLD(golden64a)));
}

static void TestGolden128(TestingT *t) {
    WITH_HASH(t, fnv_new128, test_golden(t, h_, golden128, NGOLD(golden128)));
}

static void TestGolden128a(TestingT *t) {
    WITH_HASH(t, fnv_new128a, test_golden(t, h_, golden128a, NGOLD(golden128a)));
}

static uint64_t be(Slice b) {
    uint64_t v = 0;
    for (Int i = 0; i < b.len; i++)
        v = v << 8 | ((const Byte *)b.p)[i];
    return v;
}

/* Not in Go: the body of one TestGoldenMarshal subtest. */
typedef struct MarshalCase {
    TesthashMake new_hash;
    const Golden *gold;
    size_t n;
} MarshalCase;

static void golden_marshal(void *env, TestingT *t) {
    const MarshalCase *tt = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < tt->n; i++) {
        const Golden *g = &tt->gold[i];
        Str in = str_from_cstr(g->in);
        Slice p = slice_from_str(a, in);
        Hash h = tt->new_hash(a);
        Hash h2 = tt->new_hash(a);
        Any ha = BURROW_ANY(TYPE_OF(Hash), &h);
        Any ha2 = BURROW_ANY(TYPE_OF(Hash), &h2);

        hash_write(h, slice_sub(p, 0, p.len / 2), NULL);

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
        if (!str_eq(st, g->half_state)) {
            testing_t_errorf_v(t, "checksum(%q) state = %q, want %q", in, st,
                               g->half_state);
            continue;
        }

        if (!str_eq(sa, g->half_state)) {
            testing_t_errorf_v(t, "checksum(%q) state = %q, want %q", in, sa,
                               g->half_state);
            continue;
        }

        err = encoding_unmarshal_binary(a, ha2, state);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not unmarshal: %v", err);
            continue;
        }

        hash_write(h, slice_sub(p, p.len / 2, p.len), NULL);
        hash_write(h2, slice_sub(p, p.len / 2, p.len), NULL);

        Slice actual = hash_sum(a, h, slice_nil(TYPE_BYTE));
        Slice actual2 = hash_sum(a, h2, slice_nil(TYPE_BYTE));
        if (!testhash_equal(actual, actual2))
            testing_t_errorf_v(t, "hash(%q) = 0x%x != marshaled 0x%x", in, actual,
                               actual2);
    }
    arena_free(&ar);
}

static void TestGoldenMarshal(TestingT *t) {
    static const char *names[] = {"32", "32a", "64", "64a", "128", "128a"};
    MarshalCase tests[] = {
        {make32, golden32, NGOLD(golden32)},
        {make32a, golden32a, NGOLD(golden32a)},
        {make64, golden64, NGOLD(golden64)},
        {make64a, golden64a, NGOLD(golden64a)},
        {fnv_new128, golden128, NGOLD(golden128)},
        {fnv_new128a, golden128a, NGOLD(golden128a)},
    };
    for (int i = 0; i < 6; i++)
        testing_t_run(t, str_from_cstr(names[i]),
                      BURROW_FN(TestingTFunc, golden_marshal, &tests[i]));
}

/* Go asserts the Hash to Hash32 or Hash64 by its size. There is no assertion
 * between interfaces here yet, so the caller passes whichever it has, and
 * leaves the other nil. */
static void test_integrity(TestingT *t, Hash h, HashHash32 h32, HashHash64 h64) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte bytes[] = {'1', '2', 3, 4, 5};
    Slice data = slice_from(bytes, 5, 5, TYPE_BYTE);
    hash_write(h, data, NULL);
    Slice sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
    if (hash_size(h) != sum.len)
        testing_t_fatalf_v(t, "Size()=%d but len(Sum())=%d", hash_size(h), sum.len);
    Slice s = hash_sum(a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(sum, s))
        testing_t_fatalf_v(t, "first Sum()=0x%x, second Sum()=0x%x", sum, s);

    hash_reset(h);
    hash_write(h, data, NULL);
    s = hash_sum(a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(sum, s))
        testing_t_fatalf_v(t, "Sum()=0x%x, but after Reset() Sum()=0x%x", sum, s);

    hash_reset(h);
    hash_write(h, slice_sub(data, 0, 2), NULL);
    hash_write(h, slice_sub(data, 2, 5), NULL);
    s = hash_sum(a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(sum, s))
        testing_t_fatalf_v(t, "Sum()=0x%x, but with partial writes, Sum()=0x%x", sum,
                           s);

    switch (hash_size(h)) {
    case 4: {
        uint32_t sum32 = hash_hash32_sum32(h32);
        if (sum32 != be(sum))
            testing_t_fatalf_v(t, "Sum()=0x%x, but Sum32()=0x%x", sum, sum32);
        break;
    }
    case 8: {
        uint64_t sum64 = hash_hash64_sum64(h64);
        if (sum64 != be(sum))
            testing_t_fatalf_v(t, "Sum()=0x%x, but Sum64()=0x%x", sum, sum64);
        break;
    }
    default:
        break;
    }
    arena_free(&ar);
}

static const HashHash32 no32 = {NULL, NULL};
static const HashHash64 no64 = {NULL, NULL};

static void TestIntegrity32(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    HashHash32 h = fnv_new32(arena_allocator(&ar));
    test_integrity(t, hash_hash32_as_hash(h), h, no64);
    arena_free(&ar);
}

static void TestIntegrity32a(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    HashHash32 h = fnv_new32a(arena_allocator(&ar));
    test_integrity(t, hash_hash32_as_hash(h), h, no64);
    arena_free(&ar);
}

static void TestIntegrity64(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    HashHash64 h = fnv_new64(arena_allocator(&ar));
    test_integrity(t, hash_hash64_as_hash(h), no32, h);
    arena_free(&ar);
}

static void TestIntegrity64a(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    HashHash64 h = fnv_new64a(arena_allocator(&ar));
    test_integrity(t, hash_hash64_as_hash(h), no32, h);
    arena_free(&ar);
}

static void TestIntegrity128(TestingT *t) {
    WITH_HASH(t, fnv_new128, test_integrity(t, h_, no32, no64));
}

static void TestIntegrity128a(TestingT *t) {
    WITH_HASH(t, fnv_new128a, test_integrity(t, h_, no32, no64));
}

static void benchmark_kb(TestingB *b, Hash h) {
    testing_b_set_bytes(b, 1024);
    Byte data[1024];
    for (int i = 0; i < 1024; i++)
        data[i] = (Byte)i;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = slice_make(a, TYPE_BYTE, 0, hash_size(h));
    Slice p = slice_from(data, 1024, 1024, TYPE_BYTE);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_reset(h);
        hash_write(h, p, NULL);
        hash_sum(a, h, in);
    }
    arena_free(&ar);
}

static void BenchmarkFnv32KB(TestingB *b) {
    WITH_HASH(b, make32, benchmark_kb(b, h_));
}

static void BenchmarkFnv32aKB(TestingB *b) {
    WITH_HASH(b, make32a, benchmark_kb(b, h_));
}

static void BenchmarkFnv64KB(TestingB *b) {
    WITH_HASH(b, make64, benchmark_kb(b, h_));
}

static void BenchmarkFnv64aKB(TestingB *b) {
    WITH_HASH(b, make64a, benchmark_kb(b, h_));
}

static void BenchmarkFnv128KB(TestingB *b) {
    WITH_HASH(b, fnv_new128, benchmark_kb(b, h_));
}

static void BenchmarkFnv128aKB(TestingB *b) {
    WITH_HASH(b, fnv_new128a, benchmark_kb(b, h_));
}

#define TESTS(X)                                                                       \
    X(TestHashInterface)                                                               \
    X(TestGolden32)                                                                    \
    X(TestGolden32a)                                                                   \
    X(TestGolden64)                                                                    \
    X(TestGolden64a)                                                                   \
    X(TestGolden128)                                                                   \
    X(TestGolden128a)                                                                  \
    X(TestGoldenMarshal)                                                               \
    X(TestIntegrity32)                                                                 \
    X(TestIntegrity32a)                                                                \
    X(TestIntegrity64)                                                                 \
    X(TestIntegrity64a)                                                                \
    X(TestIntegrity128)                                                                \
    X(TestIntegrity128a)                                                               \
    X(BenchmarkFnv32KB)                                                                \
    X(BenchmarkFnv32aKB)                                                               \
    X(BenchmarkFnv64KB)                                                                \
    X(BenchmarkFnv64aKB)                                                               \
    X(BenchmarkFnv128KB)                                                               \
    X(BenchmarkFnv128aKB)

TESTING_MAIN(TESTS)
