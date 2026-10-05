/* Derived from Go's src/crypto/rand/rand_test.go, util_test.go and
 * text_test.go.
 * Go source: go1.27.1.
 *
 * TestAllocations is not here: crypto_rand_read takes no allocator, so there
 * is nothing for it to allocate from. TestSynctest is not either, because
 * testing/synctest is not in burrow. TestReadError checks the fatal error in
 * the same process, through runtime_set_fatal_handler, where Go has to run
 * itself again and read the crash from the child's output. TestDefaultReader
 * keeps the half that is about which reader it is and leaves out the half that
 * asks reflect about the type's exported methods.
 *
 * TestPrimeCustomReader is burrow's: it checks that Prime ignores its reader
 * unless GODEBUG says cryptocustomrand=1, which Go leaves to its GODEBUG
 * tests.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/flate.h"
#include "burrow/crypto/rand.h"
#include "burrow/math/big.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "../src/crypto/rand_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------- Read and Reader */

typedef Int (*ReadFunc)(Slice b, Error *err);

static Int reader_read(Slice b, Error *err) {
    return crypto_rand_reader.vt->read(crypto_rand_reader.data, b, err);
}

typedef struct ReadCase {
    void (*f)(TestingT *t, ReadFunc read);
    ReadFunc read;
} ReadCase;

static void read_case(void *env, TestingT *t) {
    ReadCase *c = env;
    c->f(t, c->read);
}

/* These tests are mostly duplicates of the tests in crypto/internal/sysrand,
 * and testing both the Reader and Read is pretty redundant when one calls the
 * other, but better safe than sorry. */
static void test_read_and_reader(TestingT *t, void (*f)(TestingT *, ReadFunc)) {
    ReadCase r = {f, crypto_rand_read};
    testing_t_run(t, BURROW_S("Read"), BURROW_FN(TestingTFunc, read_case, &r));
    ReadCase rr = {f, reader_read};
    testing_t_run(t, BURROW_S("Reader.Read"), BURROW_FN(TestingTFunc, read_case, &rr));
}

static void test_read(TestingT *t, ReadFunc read) {
    Int n = 4000000;
    if (testing_short())
        n = 100000;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    Error err;
    n = read(b, &err);
    if (n != b.len || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read(buf) = %d, %s", n, error_text(err));

    BytesBuffer *z = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
    FlateWriter *f = flate_new_writer(a, bytes_buffer_as_io_writer(z), 5, NULL);
    flate_writer_write(f, b, NULL);
    flate_writer_close(f);
    if (bytes_buffer_len(z) < b.len * 99 / 100)
        testing_t_fatalf_v(t, "Compressed %d -> %d", b.len, bytes_buffer_len(z));
    arena_free(&ar);
}

static void TestRead(TestingT *t) {
    test_read_and_reader(t, test_read);
}

static void test_read_byte_values(TestingT *t, ReadFunc read) {
    Byte b[1];
    bool v[256] = {false};
    Int seen = 0;
    for (;;) {
        Error err;
        Int n = read(slice_from(b, 1, 1, TYPE_BYTE), &err);
        if (n != 1 || BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Read(b) = %d, %s", n, error_text(err));
        if (!v[b[0]]) {
            v[b[0]] = true;
            seen++;
        }
        if (seen == 256)
            break;
    }
}

static void TestReadByteValues(TestingT *t) {
    test_read_and_reader(t, test_read_byte_values);
}

static void test_large_read(TestingT *t, ReadFunc read) {
    /* 40MiB, more than the documented maximum of 32Mi-1 on Linux 32-bit. */
    Int len = 40 << 20;
    Alloc *h = heap_allocator();
    Slice b = slice_make(h, TYPE_BYTE, len, len);
    if (b.p == NULL)
        testing_t_skip_v(t, "cannot allocate %d bytes", len);
    Error err;
    Int n = read(b, &err);
    mem_free(h, b.p, (size_t)len, 1);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    else if (n != len)
        testing_t_fatalf_v(t, "Read(b) = %d, want %d", n, len);
}

static void TestLargeRead(TestingT *t) {
    test_read_and_reader(t, test_large_read);
}

static void test_read_empty(TestingT *t, ReadFunc read) {
    Byte b[1];
    Error err;
    Int n = read(slice_from(b, 0, 0, TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read(make([]byte, 0)) = %d, %s", n, error_text(err));
    n = read(slice_nil(TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read(nil) = %d, %s", n, error_text(err));
}

static void TestReadEmpty(TestingT *t) {
    test_read_and_reader(t, test_read_empty);
}

static bool reader_called;

static Int called_read(void *self, Slice b, Error *err) {
    (void)self;
    reader_called = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b.len;
}

static const IoReaderVT called_vt = {NULL, called_read};

static void TestReadUsesReader(TestingT *t) {
    IoReader saved = crypto_rand_reader;
    reader_called = false;
    crypto_rand_reader = (IoReader){&called_vt, NULL};
    Byte b[32];
    Error err;
    Int n = crypto_rand_read(slice_from(b, 32, 32, TYPE_BYTE), &err);
    crypto_rand_reader = saved;
    if (n != 32 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read(make([]byte, 32)) = %d, %s", n, error_text(err));
    if (!reader_called)
        testing_t_errorf_v(t, "Read did not use Reader");
}

typedef struct ConcurrentJob {
    TestingT *t;
    ReadFunc read;
} ConcurrentJob;

static void concurrent_reads(void *env) {
    ConcurrentJob *j = env;
    for (int i = 0; i < 1000; i++) {
        Byte b[32];
        Error err;
        Int n = j->read(slice_from(b, 32, 32, TYPE_BYTE), &err);
        if (n != 32 || BURROW_FAILED(err))
            testing_t_errorf_v(j->t, "Read = %d, %s", n, error_text(err));
    }
}

static void test_concurrent_read(TestingT *t, ReadFunc read) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    SyncWaitGroup wg = {0};
    ConcurrentJob job = {t, read};
    for (int i = 0; i < 100; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_reads, &job));
    sync_wait_group_wait(&wg);
}

static void TestConcurrentRead(TestingT *t) {
    test_read_and_reader(t, test_concurrent_read);
}

static Int error_read(void *self, Slice b, Error *err) {
    (void)self;
    (void)b;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("error")));
    return 0;
}

static const IoReaderVT error_vt = {NULL, error_read};

static void TestReadError(TestingT *t) {
    IoReader saved = crypto_rand_reader;
    crypto_rand_reader = (IoReader){&error_vt, NULL};
    Byte b[32];
    EXPECT_FATAL(crypto_rand_read(slice_from(b, 32, 32, TYPE_BYTE), NULL));
    crypto_rand_reader = saved;
    if (!fatal_did_catch)
        testing_t_fatalf_v(t, "Read did not crash");
    const char *exp = "crypto/rand: failed to read random data";
    if (strstr(fatal_thrown, exp) == NULL)
        testing_t_errorf_v(t, "fatal error does not contain %q: %s", str_from_cstr(exp),
                           str_from_cstr(fatal_thrown));
    CHECK_STR_EQ(fatal_thrown, "crypto/rand: failed to read random data (see "
                               "https://go.dev/issue/66821): error");
}

static void benchmark_read(void *env, TestingB *b) {
    Int size = *(const Int *)env;
    testing_b_set_bytes(b, size);
    Byte buf[4 << 10];
    Slice s = slice_from(buf, size, size, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err;
        crypto_rand_read(s, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
    }
}

static void BenchmarkRead(TestingB *b) {
    static Int four = 4, thirty_two = 32, four_k = 4 << 10;
    testing_b_run(b, BURROW_S("4"), BURROW_FN(TestingBFunc, benchmark_read, &four));
    testing_b_run(b, BURROW_S("32"),
                  BURROW_FN(TestingBFunc, benchmark_read, &thirty_two));
    testing_b_run(b, BURROW_S("4K"), BURROW_FN(TestingBFunc, benchmark_read, &four_k));
}

static void TestDefaultReader(TestingT *t) {
    if (!burrow__crypto_rand_is_default_reader(crypto_rand_reader))
        testing_t_errorf_v(t, "rand.IsDefaultReader(Reader) == False");
}

/* -------------------------------------------------------------------- Text */

static void index_set_table(TestingT *t, Int index_set[26][32]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    char line[4 + 32 * 4 + 1];
    int at = snprintf(line, sizeof line, "   ");
    for (int r = 0; r < 32; r++)
        at += snprintf(line + at, sizeof line - (size_t)at, " %3c", alphabet[r]);
    testing_t_logf_v(t, "%s", str_from_cstr(line));
    for (int i = 0; i < 26; i++) {
        at = snprintf(line, sizeof line, "%2d:", i);
        for (int r = 0; r < 32; r++)
            at += snprintf(line + at, sizeof line - (size_t)at, " %3d",
                           (int)index_set[i][r]);
        testing_t_logf_v(t, "%s", str_from_cstr(line));
    }
}

static int base32_index(Byte r) {
    if (r >= 'A' && r <= 'Z')
        return r - 'A';
    if (r >= '2' && r <= '7')
        return 26 + (r - '2');
    return -1;
}

static void TestText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* hold every string produced */
    static Str set[1000];
    Int set_len = 0;
    /* hold every char produced at every position */
    static Int index_set[26][32];
    memset(index_set, 0, sizeof index_set);

    /* not getting a char in a position: (31/32)¹⁰⁰⁰ = 1.6e-14
     * test completion within 1000 rounds: (1-(31/32)¹⁰⁰⁰)²⁶ = 0.9999999999996
     * empirically, this should complete within 400 rounds = 0.999921 */
    int rounds = 1000;
    bool done = false;
    for (int round = 0; round < rounds; round++) {
        Str s = crypto_rand_text(a);
        if (s.len != 26)
            testing_t_errorf_v(t, "len(Text()) = %d, want = 26", s.len);
        for (Int i = 0; i < s.len; i++) {
            if (base32_index(s.p[i]) < 0)
                testing_t_errorf_v(t, "Text()[%d] = %d, outside of base32 alphabet", i,
                                   (Int)s.p[i]);
        }
        for (Int k = 0; k < set_len; k++) {
            if (str_eq(set[k], s))
                testing_t_errorf_v(
                    t, "Text() = %s, duplicate of previously produced string", s);
        }
        set[set_len++] = s;
        done = true;
        for (Int i = 0; i < s.len; i++) {
            int r = base32_index(s.p[i]);
            if (r < 0)
                continue;
            index_set[i][r]++;
            int distinct = 0;
            for (int k = 0; k < 32; k++)
                distinct += index_set[i][k] != 0;
            if (distinct != 32)
                done = false;
        }
        if (done)
            break;
    }
    if (!done) {
        testing_t_errorf_v(
            t, "failed to produce every char at every index after %d rounds",
            (Int)rounds);
        index_set_table(t, index_set);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- Int, Prime */

static void TestPrimeSmall(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int n = 2; n < 10; n++) {
        Error err;
        BigInt *p = crypto_rand_prime(a, crypto_rand_reader, n, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Can't generate %d-bit prime: %s", n,
                               error_text(err));
        if (big_int_bit_len(p) != n)
            testing_t_fatalf_v(t, "%s is not %d-bit", big_int_string(p, a), n);
        if (!big_int_probably_prime(p, 32))
            testing_t_fatalf_v(t, "%s is not prime", big_int_string(p, a));
    }
    arena_free(&ar);
}

/* Test that passing bits < 2 causes Prime to return nil, error */
static void TestPrimeBitsLt2(TestingT *t) {
    Error err;
    BigInt *p = crypto_rand_prime(NULL, crypto_rand_reader, 1, &err);
    if (p != NULL || BURROW_OK(err))
        testing_t_errorf_v(t,
                           "Prime should return nil, error when called with bits < 2");
    CHECK_STR_EQ((const char *)error_text(err).p,
                 "crypto/rand: prime size must be at least 2-bit");
}

/* math/rand's Rand as an io.Reader, which Go gets for free because Rand has a
 * Read method. */
static Int math_rand_reader_read(void *self, Slice p, Error *err) {
    return math_rand_rand_read(self, p, err);
}

static const IoReaderVT math_rand_reader_vt = {NULL, math_rand_reader_read};

static void TestPrimeNondeterministic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 42));
    IoReader rr = {&math_rand_reader_vt, r};
    Error err;
    BigInt *p0 = crypto_rand_prime(a, rr, 32, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    for (int i = 0; i < 128; i++) {
        math_rand_rand_seed(r, 42);
        BigInt *p = crypto_rand_prime(a, rr, 32, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        if (big_int_cmp(p, p0) != 0) {
            arena_free(&ar);
            return;
        }
    }
    testing_t_errorf_v(t, "Prime always generated the same prime given the same input");
    arena_free(&ar);
}

static void TestInt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* start at 128 so the case of (max.BitLen() % 8) == 0 is covered */
    for (int64_t n = 128; n < 140; n++) {
        BigInt *b = big_new_int(a, n);
        Error err;
        BigInt *i = crypto_rand_int(a, crypto_rand_reader, b, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Can't generate random value: %s, %s",
                               i == NULL ? BURROW_S("<nil>") : big_int_string(i, a),
                               error_text(err));
    }
    arena_free(&ar);
}

typedef struct CountingReader {
    IoReader r;
    Int n;
} CountingReader;

static Int counting_read(void *self, Slice p, Error *err) {
    CountingReader *c = self;
    Int n = c->r.vt->read(c->r.data, p, err);
    c->n += n;
    return n;
}

static const IoReaderVT counting_vt = {NULL, counting_read};

typedef struct IntReadsCase {
    Int i;
    int64_t max;
} IntReadsCase;

static void int_reads(void *env, TestingT *t) {
    const IntReadsCase *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CountingReader reader = {crypto_rand_reader, 0};
    Error err;
    crypto_rand_int(a, (IoReader){&counting_vt, &reader}, big_new_int(a, c->max), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Can't generate random value: %d, %s", c->max,
                           error_text(err));
    Int expected = (c->i + 7) / 8;
    if (reader.n != expected)
        testing_t_errorf_v(t, "Int(reader, %d) should read %d bytes, but it read: %d",
                           c->max, expected, reader.n);
    arena_free(&ar);
}

/* Test that Int reads only the necessary number of bytes from the reader for
 * max at each bit length */
static void TestIntReads(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < 32; i++) {
        IntReadsCase c = {i, (int64_t)1 << i};
        testing_t_run(t, fmt_sprintf_v(a, "max=%d", c.max),
                      BURROW_FN(TestingTFunc, int_reads, &c));
    }
    arena_free(&ar);
}

static void int_mask(void *env, TestingT *t) {
    Int max = *(const Int *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < max; i++) {
        if (testing_short() && i == 0)
            i = max - 1;
        BytesBuffer *b = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
        bytes_buffer_write_byte(b, (Byte)i);
        Error err;
        BigInt *n = crypto_rand_int(a, bytes_buffer_as_io_reader(b),
                                    big_new_int(a, (int64_t)max), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Can't generate random value: %d, %s", max,
                               error_text(err));
        if (big_int_int64(n) != (int64_t)i)
            testing_t_errorf_v(t,
                               "Int(reader, %d) should have returned value of %d, but "
                               "it returned: %s",
                               max, i, big_int_string(n, a));
    }
    arena_free(&ar);
}

/* Test that Int does not mask out valid return values */
static void TestIntMask(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int max = 1; max <= 256; max++) {
        Int m = max;
        testing_t_run(t, fmt_sprintf_v(a, "max=%d", max),
                      BURROW_FN(TestingTFunc, int_mask, &m));
    }
    arena_free(&ar);
}

static void test_int_panics(TestingT *t, BigInt *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EXPECT_PANIC(crypto_rand_int(a, crypto_rand_reader, b, NULL));
    if (!fatal_did_catch)
        testing_t_errorf_v(t, "Int should panic when called with max <= 0: %s",
                           big_int_string(b, a));
    CHECK_STR_EQ(fatal_caught, "crypto/rand: argument to Int is <= 0");
    arena_free(&ar);
}

/* Test that passing a new big.Int as max causes Int to panic */
static void TestIntEmptyMaxPanics(TestingT *t) {
    BigInt b = BIG_INT(NULL);
    test_int_panics(t, &b);
}

/* Test that passing a negative value as max causes Int to panic */
static void TestIntNegativeMaxPanics(TestingT *t) {
    BigInt b = BIG_INT(NULL);
    big_int_set_int64(&b, -1);
    test_int_panics(t, &b);
    big_int_free(&b);
}

static void TestPrimeCustomReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CountingReader reader = {crypto_rand_reader, 0};
    IoReader r = {&counting_vt, &reader};
    Error err;

    burrow__crypto_rand_godebug_set("");
    crypto_rand_prime(a, r, 64, &err);
    if (BURROW_FAILED(err) || reader.n != 0)
        testing_t_errorf_v(t, "Prime read %d bytes from its reader, want 0", reader.n);

    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    crypto_rand_prime(a, r, 64, &err);
    if (BURROW_FAILED(err) || reader.n < 8)
        testing_t_errorf_v(t,
                           "with cryptocustomrand=1, Prime read %d bytes from its "
                           "reader, want at least 8",
                           reader.n);

    burrow__crypto_rand_godebug_set(NULL);
    arena_free(&ar);
}

static void BenchmarkPrime(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r =
        math_rand_new(a, math_rand_new_source(a, time_unix_nano(time_now())));
    IoReader rr = {&math_rand_reader_vt, r};
    for (Int i = 0; i < testing_b_n(b); i++)
        crypto_rand_prime(a, rr, 1024, NULL);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestRead)                                                                        \
    X(TestReadByteValues)                                                              \
    X(TestLargeRead)                                                                   \
    X(TestReadEmpty)                                                                   \
    X(TestReadUsesReader)                                                              \
    X(TestConcurrentRead)                                                              \
    X(TestReadError)                                                                   \
    X(TestDefaultReader)                                                               \
    X(TestText)                                                                        \
    X(TestPrimeSmall)                                                                  \
    X(TestPrimeBitsLt2)                                                                \
    X(TestPrimeNondeterministic)                                                       \
    X(TestInt)                                                                         \
    X(TestIntReads)                                                                    \
    X(TestIntMask)                                                                     \
    X(TestIntEmptyMaxPanics)                                                           \
    X(TestIntNegativeMaxPanics)                                                        \
    X(TestPrimeCustomReader)                                                           \
    X(BenchmarkRead)                                                                   \
    X(BenchmarkPrime)

TESTING_MAIN(TESTS)
