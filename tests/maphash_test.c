/* Derived from Go's src/hash/maphash/maphash_test.go.
 * Go source: go1.27.1.
 *
 * Go's generic helpers, testComparable and friends, become one function taking
 * a descriptor and two pointers, since that is what maphash_comparable takes.
 * Go runs each case in a subtest named after the type, and so does this.
 *
 * What is left out: the chan cases, because channels arrive with the scheduler
 * in P3; stackGrow, because a C stack does not move and so cannot change the
 * address a pointer hashes to; and TestComparableAllocations, because nothing
 * here takes an allocator and so nothing can allocate.
 *
 * Copyright 2019 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/hash/maphash.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static Slice bytes_of(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* len(m) for a map[uint64]struct{} filled from these. */
static Int distinct(uint64_t *v, Int n) {
    qsort(v, (size_t)n, sizeof v[0], cmp_u64);
    Int d = n > 0;
    for (Int i = 1; i < n; i++)
        d += v[i] != v[i - 1];
    return d;
}

static void TestUnseededHash(TestingT *t) {
    static uint64_t m[1000];
    for (Int i = 0; i < 1000; i++) {
        MaphashHash h = {0};
        m[i] = maphash_hash_sum64(&h);
    }
    Int n = distinct(m, 1000);
    if (n < 900)
        testing_t_errorf_v(t, "empty hash not sufficiently random: got %d, want 1000",
                           n);
}

static void TestSeededHash(TestingT *t) {
    MaphashSeed s = maphash_make_seed();
    static uint64_t m[1000];
    for (Int i = 0; i < 1000; i++) {
        MaphashHash h = {0};
        maphash_hash_set_seed(&h, s);
        m[i] = maphash_hash_sum64(&h);
    }
    Int n = distinct(m, 1000);
    if (n != 1)
        testing_t_errorf_v(t, "seeded hash is random: got %d, want 1", n);
}

static void write_byte(TestingT *t, MaphashHash *h, Byte b) {
    Error err = maphash_hash_write_byte(h, b);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WriteByte: %v", err);
}

static void write_single_byte(TestingT *t, MaphashHash *h, Byte b) {
    Error err = BURROW_NO_ERROR;
    maphash_hash_write(h, bytes_of(&b, 1), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write single byte: %v", err);
}

static void write_string_single_byte(TestingT *t, MaphashHash *h, Byte b) {
    Error err = BURROW_NO_ERROR;
    maphash_hash_write_string(h, (Str){&b, 1}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WriteString single byte: %v", err);
}

static void TestHashGrouping(TestingT *t) {
    Byte b[300];
    for (int i = 0; i < 300; i++)
        b[i] = (Byte)(i % 3 == 0 ? 'f' : 'o');
    MaphashHash hh[7];
    memset(hh, 0, sizeof hh);
    for (int i = 1; i < 7; i++)
        maphash_hash_set_seed(&hh[i], maphash_hash_seed(&hh[0]));
    maphash_hash_write(&hh[0], bytes_of(b, 300), NULL);
    maphash_hash_write_string(&hh[1], (Str){b, 300}, NULL);

    for (int i = 0; i < 300; i++) {
        Byte x = b[i];
        write_byte(t, &hh[2], x);
        write_single_byte(t, &hh[3], x);
        if (i == 0)
            write_byte(t, &hh[4], x);
        else
            write_single_byte(t, &hh[4], x);
        write_string_single_byte(t, &hh[5], x);
        if (i == 0)
            write_byte(t, &hh[6], x);
        else
            write_string_single_byte(t, &hh[6], x);
    }

    uint64_t sum = maphash_hash_sum64(&hh[0]);
    for (int i = 0; i < 7; i++) {
        if (sum != maphash_hash_sum64(&hh[i]))
            testing_t_errorf_v(t, "hash %d not identical to a single Write", i);
    }
    if (maphash_bytes(maphash_hash_seed(&hh[0]), bytes_of(b, 300)) !=
        maphash_hash_sum64(&hh[0]))
        testing_t_errorf_v(t, "hash using Bytes not identical to a single Write");
    if (maphash_string(maphash_hash_seed(&hh[0]), (Str){b, 300}) !=
        maphash_hash_sum64(&hh[0]))
        testing_t_errorf_v(t, "hash using String not identical to a single Write");
}

static void TestHashBytesVsString(TestingT *t) {
    Str s = BURROW_S("foo");
    Byte b[3] = {'f', 'o', 'o'};
    MaphashHash h1 = {0}, h2 = {0};
    maphash_hash_set_seed(&h2, maphash_hash_seed(&h1));
    Error err1 = BURROW_NO_ERROR, err2 = BURROW_NO_ERROR;
    Int n1 = maphash_hash_write_string(&h1, s, &err1);
    if (n1 != s.len || BURROW_FAILED(err1))
        testing_t_fatalf_v(t, "WriteString(s) = %d, %v, want %d, nil", n1, err1, s.len);
    Int n2 = maphash_hash_write(&h2, bytes_of(b, 3), &err2);
    if (n2 != 3 || BURROW_FAILED(err2))
        testing_t_fatalf_v(t, "Write(b) = %d, %v, want %d, nil", n2, err2, 3);
    if (maphash_hash_sum64(&h1) != maphash_hash_sum64(&h2))
        testing_t_errorf_v(t, "hash of string and bytes not identical");
}

/* See Go issue 34925. */
static void TestHashHighBytes(TestingT *t) {
    enum { N = 10 };
    uint64_t m[N];
    for (int i = 0; i < N; i++) {
        MaphashHash h = {0};
        maphash_hash_write_string(&h, BURROW_S("foo"), NULL);
        m[i] = maphash_hash_sum64(&h) >> 32;
    }
    Int n = distinct(m, N);
    if (n < N / 2)
        testing_t_errorf_v(t,
                           "from %d seeds, wanted at least %d different hashes; got %d",
                           N, N / 2, n);
}

static void TestRepeat(TestingT *t) {
    MaphashHash h1 = {0};
    maphash_hash_write_string(&h1, BURROW_S("testing"), NULL);
    uint64_t sum1 = maphash_hash_sum64(&h1);

    maphash_hash_reset(&h1);
    maphash_hash_write_string(&h1, BURROW_S("testing"), NULL);
    uint64_t sum2 = maphash_hash_sum64(&h1);

    if (sum1 != sum2)
        testing_t_errorf_v(t, "different sum after resetting: %#x != %#x", sum1, sum2);

    MaphashHash h2 = {0};
    maphash_hash_set_seed(&h2, maphash_hash_seed(&h1));
    maphash_hash_write_string(&h2, BURROW_S("testing"), NULL);
    uint64_t sum3 = maphash_hash_sum64(&h2);

    if (sum1 != sum3)
        testing_t_errorf_v(t, "different sum on the same seed: %#x != %#x", sum1, sum3);
}

static void same_as_reseeded(TestingT *t, MaphashHash *h1, uint64_t x, Slice b) {
    MaphashHash h2 = {0};
    maphash_hash_set_seed(&h2, maphash_hash_seed(h1));
    maphash_hash_write(&h2, b, NULL);
    uint64_t y = maphash_hash_sum64(&h2);
    if (x != y)
        testing_t_errorf_v(t, "hashes don't match: want %x, got %x", x, y);
}

static void TestSeedFromSum64(TestingT *t) {
    MaphashHash h1 = {0};
    maphash_hash_write_string(&h1, BURROW_S("foo"), NULL);
    uint64_t x = maphash_hash_sum64(&h1); /* seed generated here */
    Byte foo[3] = {'f', 'o', 'o'};
    same_as_reseeded(t, &h1, x, bytes_of(foo, 3));
}

static void TestSeedFromSeed(TestingT *t) {
    MaphashHash h1 = {0};
    maphash_hash_write_string(&h1, BURROW_S("foo"), NULL);
    (void)maphash_hash_seed(&h1); /* seed generated here */
    uint64_t x = maphash_hash_sum64(&h1);
    Byte foo[3] = {'f', 'o', 'o'};
    same_as_reseeded(t, &h1, x, bytes_of(foo, 3));
}

static void TestSeedFromFlush(TestingT *t) {
    Byte b[65] = {0};
    MaphashHash h1 = {0};
    maphash_hash_write(&h1, bytes_of(b, 65), NULL); /* seed generated here */
    uint64_t x = maphash_hash_sum64(&h1);
    same_as_reseeded(t, &h1, x, bytes_of(b, 65));
}

static void TestSeedFromReset(TestingT *t) {
    MaphashHash h1 = {0};
    maphash_hash_write_string(&h1, BURROW_S("foo"), NULL);
    maphash_hash_reset(&h1); /* seed generated here */
    maphash_hash_write_string(&h1, BURROW_S("foo"), NULL);
    uint64_t x = maphash_hash_sum64(&h1);
    Byte foo[3] = {'f', 'o', 'o'};
    same_as_reseeded(t, &h1, x, bytes_of(foo, 3));
}

/* ------------------------------------------------------------ the types */

BURROW_PTR_TYPE(DoublePtr, double);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_SLICE_TYPE(ByteSlice, Byte);
BURROW_ARRAY_TYPE(StrArray2, Str, 2);

#define S_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT(S, S_FIELDS);

#define STR_PAIR_FIELDS(F, T) F(T, Str, a, "") F(T, Str, b, "")
BURROW_STRUCT(StrPair, STR_PAIR_FIELDS);

/* struct{}. */
static const Type empty_struct = {
    {(const Byte *)"struct {}", 9},
    {NULL, 0},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    9101,
    NULL,
};

/* The two with a field of type any are built at startup, because TYPE_ANY is a
 * variable and a static initialiser cannot read one. */
typedef struct Mixed {
    Int i;
    Uint u;
    bool b;
    double f;
    Int *p;
    Any a;
} Mixed;

typedef struct AnyPair {
    Any a, b;
} AnyPair;

static Field mixed_fields[6];
static Field any_pair_fields[2];
static Type mixed_type;
static Type any_pair_type;

static void make_types(void) {
    mixed_fields[0] = (Field){BURROW_S("i"), {NULL, 0}, TYPE_INT, offsetof(Mixed, i)};
    mixed_fields[1] = (Field){BURROW_S("u"), {NULL, 0}, TYPE_UINT, offsetof(Mixed, u)};
    mixed_fields[2] = (Field){BURROW_S("b"), {NULL, 0}, TYPE_BOOL, offsetof(Mixed, b)};
    mixed_fields[3] =
        (Field){BURROW_S("f"), {NULL, 0}, TYPE_FLOAT64, offsetof(Mixed, f)};
    mixed_fields[4] =
        (Field){BURROW_S("p"), {NULL, 0}, TYPE_OF(IntPtr), offsetof(Mixed, p)};
    mixed_fields[5] = (Field){BURROW_S("a"), {NULL, 0}, TYPE_ANY, offsetof(Mixed, a)};
    mixed_type = (Type){{NULL, 0},
                        {NULL, 0},
                        KIND_STRUCT,
                        (uint32_t)sizeof(Mixed),
                        (uint16_t)_Alignof(Mixed),
                        6,
                        0,
                        mixed_fields,
                        NULL,
                        NULL,
                        NULL,
                        0,
                        9102,
                        NULL};
    any_pair_fields[0] =
        (Field){BURROW_S("a"), {NULL, 0}, TYPE_ANY, offsetof(AnyPair, a)};
    any_pair_fields[1] =
        (Field){BURROW_S("b"), {NULL, 0}, TYPE_ANY, offsetof(AnyPair, b)};
    any_pair_type = (Type){{NULL, 0},
                           {NULL, 0},
                           KIND_STRUCT,
                           (uint32_t)sizeof(AnyPair),
                           (uint16_t)_Alignof(AnyPair),
                           2,
                           0,
                           any_pair_fields,
                           NULL,
                           NULL,
                           NULL,
                           0,
                           9103,
                           NULL};
}

/* ------------------------------------------------------------ comparable */

typedef struct Case {
    const char *name;
    const Type *t;
    const void *a, *b;
} Case;

static void run_comparable(void *env, TestingT *t) {
    const Case *c = env;
    MaphashSeed seed = maphash_make_seed();
    if (maphash_comparable(seed, c->t, c->a) != maphash_comparable(seed, c->t, c->b))
        testing_t_fatalf_v(t, "Comparable(seed, a) != Comparable(seed, b) for %s",
                           c->name);
    /* Comparable(seed, pa), where pa is a *T, hashes the pointer and not what
     * it points at. */
    const void *pa = c->a;
    uint64_t old = maphash_comparable(seed, TYPE_UNSAFE_POINTER, (const void *)&pa);
    uint64_t now = maphash_comparable(seed, TYPE_UNSAFE_POINTER, (const void *)&pa);
    if (old != now)
        testing_t_fatalf_v(t, "Comparable(seed, ptr) != Comparable(seed, ptr)");
}

static void run_write_comparable(void *env, TestingT *t) {
    const Case *c = env;
    MaphashHash h1 = {0}, h2 = {0};
    maphash_hash_set_seed(&h1, maphash_make_seed());
    maphash_hash_set_seed(&h2, maphash_hash_seed(&h1));
    maphash_write_comparable(&h1, c->t, c->a);
    maphash_write_comparable(&h2, c->t, c->b);
    if (maphash_hash_sum64(&h1) != maphash_hash_sum64(&h2))
        testing_t_fatalf_v(t, "WriteComparable(h, a) != WriteComparable(h, b) for %s",
                           c->name);
    const void *pa = c->a;
    maphash_write_comparable(&h1, TYPE_UNSAFE_POINTER, (const void *)&pa);
    uint64_t old = maphash_hash_sum64(&h1);
    maphash_write_comparable(&h2, TYPE_UNSAFE_POINTER, (const void *)&pa);
    uint64_t now = maphash_hash_sum64(&h2);
    if (old != now)
        testing_t_fatalf_v(t,
                           "WriteComparable(seed, ptr) != WriteComparable(seed, ptr)");
}

static void no_equal(TestingT *t, const Case *c) {
    MaphashSeed seed = maphash_make_seed();
    if (maphash_comparable(seed, c->t, c->a) == maphash_comparable(seed, c->t, c->b))
        testing_t_fatalf_v(t, "Comparable(seed, a) == Comparable(seed, b) for %s",
                           c->name);
}

static void write_no_equal(TestingT *t, const Case *c) {
    MaphashHash h1 = {0}, h2 = {0};
    maphash_hash_set_seed(&h1, maphash_make_seed());
    maphash_hash_set_seed(&h2, maphash_hash_seed(&h1));
    maphash_write_comparable(&h1, c->t, c->a);
    maphash_write_comparable(&h2, c->t, c->b);
    if (maphash_hash_sum64(&h1) == maphash_hash_sum64(&h2))
        testing_t_fatalf_v(
            t, "WriteComparable(seed, a) == WriteComparable(seed, b) for %s", c->name);
}

/* Two strings with the same bytes at different addresses, which is what Go's
 * heapStr makes. */
static Byte heap1[] = "aTestString";
static Byte heap2[] = "aTestString";

static int64_t v_i64 = 2;
static uint64_t v_u64 = 8;
static Uintptr v_uptr = 12;
static Str v_s = {(const Byte *)"s", 1};
static Str v_s_other = {(const Byte[]){'s'}, 1};
static Any v_any_s1, v_any_s2;
static bool v_true = true;
static double v_pointee;
static double *v_pf = &v_pointee;
static double v_f64 = 9;
static Complex128 v_c128;
static Byte v_empty;
static Int v_pointee_int;
static Mixed v_mixed;
static S v_s1, v_s2;
static float v_f32z = 0.0F, v_f32nz;
static double v_f64z = 0.0, v_f64nz;
static double v_nan1, v_nan2;
static StrArray2 v_arr1, v_arr2;
static StrPair v_sp1, v_sp2;
static Int v_zero_int;
static AnyPair v_ap1, v_ap2;

static void make_values(void) {
    v_any_s1 = BURROW_ANY(TYPE_STRING, &v_s);
    v_any_s2 = BURROW_ANY(TYPE_STRING, &v_s_other);
    v_c128 = (Complex128){1, 9};
    v_mixed =
        (Mixed){9, 1, true, 9.9, &v_pointee_int, BURROW_ANY(TYPE_INT, &v_zero_int)};
    v_s1.s = (Str){heap1, 11};
    v_s2.s = (Str){heap2, 11};
    v_f32nz = -v_f32z;
    v_f64nz = -v_f64z;
    v_nan1 = (double)NAN;
    v_nan2 = (double)NAN;
    v_arr1 = (StrArray2){{BURROW_S("a"), BURROW_S("")}};
    v_arr2 = (StrArray2){{BURROW_S(""), BURROW_S("a")}};
    v_sp1 = (StrPair){BURROW_S("foo"), BURROW_S("")};
    v_sp2 = (StrPair){BURROW_S(""), BURROW_S("foo")};
    v_ap1 = (AnyPair){BURROW_ANY(TYPE_INT, &v_zero_int),
                      BURROW_ANY(&empty_struct, &v_empty)};
    v_ap2 = (AnyPair){BURROW_ANY(&empty_struct, &v_empty),
                      BURROW_ANY(TYPE_INT, &v_zero_int)};
}

#define EQUAL_CASES(X)                                                                 \
    X("int64", TYPE_INT64, &v_i64, &v_i64)                                             \
    X("uint64", TYPE_UINT64, &v_u64, &v_u64)                                           \
    X("uintptr", TYPE_UINTPTR, &v_uptr, &v_uptr)                                       \
    X("interface {}", TYPE_ANY, &v_any_s1, &v_any_s2)                                  \
    X("string", TYPE_STRING, &v_s, &v_s_other)                                         \
    X("bool", TYPE_BOOL, &v_true, &v_true)                                             \
    X("*float64", TYPE_OF(DoublePtr), (const void *)&v_pf, (const void *)&v_pf)        \
    X("float64", TYPE_FLOAT64, &v_f64, &v_f64)                                         \
    X("complex128", TYPE_COMPLEX128, &v_c128, &v_c128)                                 \
    X("struct {}", &empty_struct, &v_empty, &v_empty)                                  \
    X("struct { i int; u uint; b bool; f float64; p *int; a interface {} }",           \
      &mixed_type, &v_mixed, &v_mixed)                                                 \
    X("maphash.S", TYPE_OF(S), &v_s1, &v_s2)                                           \
    X("string/heap", TYPE_STRING, &v_s1.s, &v_s2.s)                                    \
    X("float32", TYPE_FLOAT32, &v_f32z, &v_f32nz)                                      \
    X("float64/zero", TYPE_FLOAT64, &v_f64z, &v_f64nz)

#define NO_EQUAL_CASES(X)                                                              \
    X("NaN", TYPE_FLOAT64, &v_nan1, &v_nan2)                                           \
    X("[2]string", TYPE_OF(StrArray2), &v_arr1, &v_arr2)                               \
    X("struct { a, b string }", TYPE_OF(StrPair), &v_sp1, &v_sp2)                      \
    X("struct { a, b any }", &any_pair_type, &v_ap1, &v_ap2)

#define AS_CASE(n, ty, x, y) {n, ty, x, y},

static void check_cases(TestingT *t, void (*fn)(void *, TestingT *)) {
    const Case cases[] = {EQUAL_CASES(AS_CASE)};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, fn, (void *)(uintptr_t)&cases[i]));
}

static void TestComparable(TestingT *t) {
    if (v_s1.s.p == v_s2.s.p)
        testing_t_fatalf_v(t, "unexpected two heapStr ptr equal");
    if (!str_eq(v_s1.s, v_s2.s))
        testing_t_fatalf_v(t, "unexpected two heapStr value not equal");
    check_cases(t, run_comparable);
    const Case ne[] = {NO_EQUAL_CASES(AS_CASE)};
    for (size_t i = 0; i < sizeof ne / sizeof ne[0]; i++)
        no_equal(t, &ne[i]);
}

static void TestWriteComparable(TestingT *t) {
    check_cases(t, run_write_comparable);
    const Case ne[] = {NO_EQUAL_CASES(AS_CASE)};
    for (size_t i = 0; i < sizeof ne / sizeof ne[0]; i++)
        write_no_equal(t, &ne[i]);
}

static Byte panic_buf[256];

static Str recovered(Func f) {
    volatile Int n = -1;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < (Int)sizeof panic_buf ? m.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0)
        return (Str){NULL, 0};
    return (Str){panic_buf, n};
}

static void hash_any_bytes(void *env) {
    (void)env;
    Byte s[1] = {'s'};
    Slice b = bytes_of(s, 1);
    Any a = BURROW_ANY(TYPE_OF(ByteSlice), &b);
    (void)maphash_comparable(maphash_make_seed(), TYPE_ANY, &a);
}

static void TestComparableShouldPanic(TestingT *t) {
    Str got = recovered(BURROW_FN(Func, hash_any_bytes, NULL));
    if (got.p == NULL)
        testing_t_fatalf_v(t, "Comaparable(any([]byte)) should panic");
    Str want = BURROW_S("hash of unhashable type []uint8");
    if (!strings_contains(got, want))
        testing_t_fatalf_v(t, "want %s, got %s", want, got);
}

static void TestWriteComparableNoncommute(TestingT *t) {
    MaphashSeed seed = maphash_make_seed();
    MaphashHash h1 = {0}, h2 = {0};
    maphash_hash_set_seed(&h1, seed);
    maphash_hash_set_seed(&h2, seed);

    Int v = 123;
    maphash_hash_write_string(&h1, BURROW_S("abc"), NULL);
    maphash_write_comparable(&h1, TYPE_INT, &v);
    maphash_write_comparable(&h2, TYPE_INT, &v);
    maphash_hash_write_string(&h2, BURROW_S("abc"), NULL);

    if (maphash_hash_sum64(&h1) == maphash_hash_sum64(&h2))
        testing_t_errorf_v(t, "WriteComparable and WriteString unexpectedly commute");
}

static Hash make_hash(Alloc *a) {
    MaphashHash *h = BURROW_NEW(a, MaphashHash);
    return maphash_hash_as_hash(h);
}

static HashCloner make_cloner(Alloc *a) {
    MaphashHash *h = BURROW_NEW(a, MaphashHash);
    return maphash_hash_as_cloner(h);
}

static void TestHashInterface(TestingT *t) {
    testhash_with_clone(t, make_hash, make_cloner);
}

/* Not in Go's test file, where the compiler checks these. */
static void TestSizes(TestingT *t) {
    MaphashHash h = {0};
    CHECK_INT_EQ(maphash_hash_size(&h), 8);
    CHECK_INT_EQ(maphash_hash_block_size(&h), MAPHASH_BUF_SIZE);
    Hash hh = maphash_hash_as_hash(&h);
    CHECK_INT_EQ(hash_size(hh), 8);
    HashHash64 h64 = maphash_hash_as_hash64(&h);
    maphash_hash_write_string(&h, BURROW_S("x"), NULL);
    CHECK(hash_hash64_sum64(h64) == maphash_hash_sum64(&h));
}

static void zero_seed_bytes(void *env) {
    (void)env;
    MaphashSeed zero = {0};
    (void)maphash_string(zero, BURROW_S("x"));
}

static void zero_seed_set(void *env) {
    (void)env;
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, (MaphashSeed){0});
}

/* Not in Go's test file either: the zero Seed is not a seed, and every way of
 * handing one in says so. */
static void TestZeroSeedPanics(TestingT *t) {
    Str want = BURROW_S("maphash: use of uninitialized Seed");
    Str got = recovered(BURROW_FN(Func, zero_seed_bytes, NULL));
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "String(Seed{}): got %q, want %q", got, want);
    got = recovered(BURROW_FN(Func, zero_seed_set, NULL));
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "SetSeed(Seed{}): got %q, want %q", got, want);
}

/* A map keyed by any panics the same way when handed a slice. */
static Arena map_arena;

static void map_any_bytes(void *env) {
    (void)env;
    Map *m = map_make(arena_allocator(&map_arena), TYPE_ANY, TYPE_INT, 0);
    Byte s[1] = {'s'};
    Slice b = bytes_of(s, 1);
    Any k = BURROW_ANY(TYPE_OF(ByteSlice), &b);
    Int v = 1;
    map_set(m, &k, &v);
}

static void TestMapKeyedByAnyRejectsSlices(TestingT *t) {
    arena_init(&map_arena, heap_allocator(), 0);
    Str got = recovered(BURROW_FN(Func, map_any_bytes, NULL));
    arena_free(&map_arena);
    Str want = BURROW_S("runtime error: hash of unhashable type []uint8");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

/* Go's ComparableHasher has no test of its own beyond the Bloom filter example,
 * which is in docs/examples/maphash/bloom.c. These check the two methods say
 * what WriteComparable and == say. */
static void TestComparableHasher(TestingT *t) {
    MaphashComparableHasher ch = {TYPE_STRING};
    MaphashHasher hr = maphash_comparable_hasher_as_hasher(&ch);
    MaphashSeed seed = maphash_make_seed();

    MaphashHash h1 = {0}, h2 = {0}, h3 = {0};
    maphash_hash_set_seed(&h1, seed);
    maphash_hash_set_seed(&h2, seed);
    maphash_hash_set_seed(&h3, seed);
    maphash_hasher_hash(hr, &h1, &v_s1.s);
    maphash_comparable_hasher_hash(ch, &h2, &v_s2.s);
    maphash_write_comparable(&h3, TYPE_STRING, &v_s1.s);
    if (maphash_hash_sum64(&h1) != maphash_hash_sum64(&h2))
        testing_t_errorf_v(t, "Hash of equal strings differs");
    if (maphash_hash_sum64(&h1) != maphash_hash_sum64(&h3))
        testing_t_errorf_v(t, "Hash differs from WriteComparable");
    CHECK(maphash_hasher_equal(hr, &v_s1.s, &v_s2.s));
    CHECK(!maphash_hasher_equal(hr, &v_s1.s, &v_s));

    MaphashComparableHasher fh = {TYPE_FLOAT64};
    CHECK(maphash_comparable_hasher_equal(fh, &v_f64z, &v_f64nz));
    CHECK(!maphash_comparable_hasher_equal(fh, &v_nan1, &v_nan1));
}

/* The CaseInsensitive Hasher from the Hasher documentation. */
static Str lower(Str s, Byte *buf) {
    for (Int i = 0; i < s.len; i++)
        buf[i] = (Byte)(s.p[i] >= 'A' && s.p[i] <= 'Z' ? s.p[i] + 32 : s.p[i]);
    return (Str){buf, s.len};
}

static void ci_hash(void *self, MaphashHash *h, const void *v) {
    (void)self;
    Byte buf[64];
    maphash_hash_write_string(h, lower(*(const Str *)v, buf), NULL);
}

static bool ci_equal(void *self, const void *x, const void *y) {
    (void)self;
    Byte bx[64], by[64];
    return str_eq(lower(*(const Str *)x, bx), lower(*(const Str *)y, by));
}

static const MaphashHasherVT case_insensitive_vt = {ci_hash, ci_equal};

static void TestHasherInterface(TestingT *t) {
    MaphashHasher hr = {&case_insensitive_vt, NULL};
    Str a = BURROW_S("Hello"), b = BURROW_S("hELLO"), c = BURROW_S("world");
    MaphashSeed seed = maphash_make_seed();
    uint64_t sums[3];
    Str *vs[3] = {&a, &b, &c};
    for (int i = 0; i < 3; i++) {
        MaphashHash h = {0};
        maphash_hash_set_seed(&h, seed);
        maphash_hasher_hash(hr, &h, vs[i]);
        sums[i] = maphash_hash_sum64(&h);
    }
    if (!maphash_hasher_equal(hr, &a, &b) || sums[0] != sums[1])
        testing_t_errorf_v(t, "Hello and hELLO should be equal with equal hashes");
    if (maphash_hasher_equal(hr, &a, &c) || sums[0] == sums[2])
        testing_t_errorf_v(t, "Hello and world should differ");
}

static void hash_slice_hasher(void *env) {
    (void)env;
    MaphashComparableHasher ch = {TYPE_OF(ByteSlice)};
    Slice b = slice_nil(TYPE_BYTE);
    MaphashHash h = {0};
    maphash_comparable_hasher_hash(ch, &h, &b);
}

/* ComparableHasher[[]byte] compiles in Go and panics when used, and so does
 * this. */
static void TestComparableHasherOfSlicePanics(TestingT *t) {
    Str got = recovered(BURROW_FN(Func, hash_slice_hasher, NULL));
    Str want = BURROW_S("runtime error: hash of unhashable type []uint8");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

/* Clone fails the way the allocator does rather than handing back garbage. */
static void TestCloneOutOfMemory(TestingT *t) {
    Byte buf[8];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Alloc *a = fixed_allocator(&fx);
    MaphashHash h = {0};
    Error err = BURROW_NO_ERROR;
    HashCloner c = maphash_hash_clone(&h, a, &err);
    CHECK(c.vt == NULL);
    CHECK(errors_is(err, burrow_err_out_of_memory));
}

static int TestMain(TestingM *m) {
    make_types();
    make_values();
    return testing_m_run(m);
}

#define TESTS(X)                                                                       \
    X(TestUnseededHash)                                                                \
    X(TestSeededHash)                                                                  \
    X(TestHashGrouping)                                                                \
    X(TestHashBytesVsString)                                                           \
    X(TestHashHighBytes)                                                               \
    X(TestRepeat)                                                                      \
    X(TestSeedFromSum64)                                                               \
    X(TestSeedFromSeed)                                                                \
    X(TestSeedFromFlush)                                                               \
    X(TestSeedFromReset)                                                               \
    X(TestComparable)                                                                  \
    X(TestWriteComparable)                                                             \
    X(TestComparableShouldPanic)                                                       \
    X(TestWriteComparableNoncommute)                                                   \
    X(TestHashInterface)                                                               \
    X(TestSizes)                                                                       \
    X(TestZeroSeedPanics)                                                              \
    X(TestMapKeyedByAnyRejectsSlices)                                                  \
    X(TestComparableHasher)                                                            \
    X(TestHasherInterface)                                                             \
    X(TestComparableHasherOfSlicePanics)                                               \
    X(TestCloneOutOfMemory)

TESTING_MAIN_WITH(TestMain, TESTS)
