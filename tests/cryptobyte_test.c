/* Derived from golang.org/x/crypto/cryptobyte's cryptobyte_test.go and
 * asn1_test.go, the versions vendored into go1.27.1.
 *
 * Go's continuations report an error by panicking with a BuildError, which
 * the Builder recovers. C has nothing to recover with, so TestContinuationError
 * has the continuation call cryptobyte_builder_set_error on its child instead.
 * TestGeneratedPanic is not here: it writes through a nil pointer and checks
 * that the panic gets out, which in C is a crash. The panics the others
 * expect from inside a continuation are caught with BURROW_TRY around the
 * single call, as Go's deferred recovers do.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/encoding/asn1.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/time.h"

#include "../src/crypto/cryptobyte.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

static Slice bs(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* The bytes listed, as a Slice. */
#define BS(...)                                                                        \
    bs((const Byte[]){__VA_ARGS__}, (Int)sizeof((const Byte[]){__VA_ARGS__}))

/* An empty but not nil Slice, which is []byte{} in Go. */
static Byte empty_buf[1];
#define BS_EMPTY bs(empty_buf, 0)

static CryptobyteBuilder zero_builder(void) {
    return (CryptobyteBuilder){0};
}

/* builderBytesEq: whether b holds want, false with a message when not. */
static bool builder_bytes_eq(TestingT *t, CryptobyteBuilder *b, Slice want) {
    Slice got = cryptobyte_builder_bytes_or_panic(b);
    if (!bytes_equal(got, want)) {
        testing_t_errorf_v(t, "Bytes() = %v, want %v", got, want);
        return false;
    }
    return true;
}

/* Whether f(env) panics. */
static bool panics(void (*f)(void *), void *env) {
    volatile bool p = false;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        (void)r;
        p = true;
    }
    BURROW_TRY_END;
    return p;
}

#define CONT(fn, env) BURROW_FN(CryptobyteBuilderContinuation, fn, env)

/* ------------------------------------------------------------- Builder */

static void continuation_error(void *env, CryptobyteBuilder *b) {
    (void)env;
    cryptobyte_builder_add_uint8(b, 1);
    cryptobyte_builder_set_error(
        b, errors_new(error_allocator(), BURROW_S("TestContinuationError")));
}

static void TestContinuationError(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(continuation_error, NULL));

    Error err;
    Slice ret = cryptobyte_builder_bytes(&b, &err);
    if (!slice_is_nil(ret))
        testing_t_error_v(t, "expected nil result");
    if (!BURROW_FAILED(err))
        testing_t_fatal_v(t, "unexpected nil error");
    Str s = error_text(err);
    if (!str_eq(s, BURROW_S("TestContinuationError")))
        testing_t_errorf_v(t, "expected error %q, got %v", "TestContinuationError", s);
    cryptobyte_builder_free(&b);
}

static void continuation_panic(void *env, CryptobyteBuilder *b) {
    (void)env;
    cryptobyte_builder_add_uint8(b, 1);
    panic_str(BURROW_S("1"));
}

static void add_panicking_child(void *env) {
    CryptobyteBuilder *b = env;
    cryptobyte_builder_add_uint8_length_prefixed(b, CONT(continuation_panic, NULL));
}

static void TestContinuationNonError(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    if (!panics(add_panicking_child, &b))
        testing_t_error_v(t, "Builder did not panic");
    cryptobyte_builder_free(&b);
}

static void TestBytes(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    Slice v = bs("foobarbaz", 9);
    cryptobyte_builder_add_bytes(&b, slice_sub(v, 0, 3));
    cryptobyte_builder_add_bytes(&b, slice_sub(v, 3, 4));
    cryptobyte_builder_add_bytes(&b, slice_sub(v, 4, 9));
    builder_bytes_eq(t, &b, v);
    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    const char *ws[] = {"foo", "bar", "baz"};
    for (int i = 0; i < 3; i++) {
        Slice got = slice_nil(TYPE_BYTE);
        if (!cryptobyte_string_read_bytes(&s, &got, 3))
            testing_t_errorf_v(t, "ReadBytes() = false, want true (w = %v)", ws[i]);
        Slice want = bs(ws[i], 3);
        if (!bytes_equal(got, want))
            testing_t_errorf_v(t, "ReadBytes(): got = %v, want %v", got, want);
    }
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint8(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8(&b, 42);
    builder_bytes_eq(t, &b, BS(42));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint8_t v = 0;
    if (!cryptobyte_string_read_uint8(&s, &v))
        testing_t_error_v(t, "ReadUint8() = false, want true");
    if (v != 42)
        testing_t_errorf_v(t, "v = %d, want 42", v);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint16(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint16(&b, 65534);
    builder_bytes_eq(t, &b, BS(255, 254));
    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint16_t v = 0;
    if (!cryptobyte_string_read_uint16(&s, &v))
        testing_t_error_v(t, "ReadUint16() == false, want true");
    if (v != 65534)
        testing_t_errorf_v(t, "v = %d, want 65534", v);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint24(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint24(&b, 0xfffefd);
    builder_bytes_eq(t, &b, BS(255, 254, 253));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint32_t v = 0;
    if (!cryptobyte_string_read_uint24(&s, &v))
        testing_t_error_v(t, "ReadUint24() = false, want true");
    if (v != 0xfffefd)
        testing_t_errorf_v(t, "v = %d, want fffefd", v);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint24Truncation(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint24(&b, 0x10111213);
    builder_bytes_eq(t, &b, BS(0x11, 0x12, 0x13));
    cryptobyte_builder_free(&b);
}

static void TestUint32(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint32(&b, 0xfffefdfc);
    builder_bytes_eq(t, &b, BS(255, 254, 253, 252));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint32_t v = 0;
    if (!cryptobyte_string_read_uint32(&s, &v))
        testing_t_error_v(t, "ReadUint32() = false, want true");
    if (v != 0xfffefdfc)
        testing_t_errorf_v(t, "v = %x, want fffefdfc", v);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint48(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    uint64_t u = UINT64_C(0xfefcff3cfdfc);
    cryptobyte_builder_add_uint48(&b, u);
    builder_bytes_eq(t, &b, BS(254, 252, 255, 60, 253, 252));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint64_t v = 0;
    if (!cryptobyte_string_read_uint48(&s, &v))
        testing_t_error_v(t, "ReadUint48() = false, want true");
    if (v != u)
        testing_t_errorf_v(t, "v = %x, want %x", v, u);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUint64(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint64(&b, UINT64_C(0xf2fefefcff3cfdfc));
    builder_bytes_eq(t, &b, BS(242, 254, 254, 252, 255, 60, 253, 252));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint64_t v = 0;
    if (!cryptobyte_string_read_uint64(&s, &v))
        testing_t_error_v(t, "ReadUint64() = false, want true");
    if (v != UINT64_C(0xf2fefefcff3cfdfc))
        testing_t_errorf_v(t, "v = %x, want f2fefefcff3cfdfc", v);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void TestUMultiple(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8(&b, 23);
    cryptobyte_builder_add_uint32(&b, 0xfffefdfc);
    cryptobyte_builder_add_uint16(&b, 42);
    builder_bytes_eq(t, &b, BS(23, 255, 254, 253, 252, 0, 42));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    uint8_t x = 0;
    uint32_t y = 0;
    uint16_t z = 0;
    if (!cryptobyte_string_read_uint8(&s, &x) ||
        !cryptobyte_string_read_uint32(&s, &y) ||
        !cryptobyte_string_read_uint16(&s, &z))
        testing_t_error_v(t, "ReadUint8() = false, want true");
    if (x != 23 || y != 0xfffefdfc || z != 42)
        testing_t_errorf_v(t, "x, y, z = %d, %d, %d; want 23, 4294901244, 5", x, y, z);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    cryptobyte_builder_free(&b);
}

static void add_23_42(void *env, CryptobyteBuilder *c) {
    (void)env;
    cryptobyte_builder_add_uint8(c, 23);
    cryptobyte_builder_add_uint8(c, 42);
}

static void add_123_234(void *env, CryptobyteBuilder *c) {
    (void)env;
    cryptobyte_builder_add_uint8(c, 123);
    cryptobyte_builder_add_uint8(c, 234);
}

static void TestUint8LengthPrefixedSimple(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(add_23_42, NULL));
    builder_bytes_eq(t, &b, BS(2, 23, 42));

    CryptobyteString base = cryptobyte_builder_bytes_or_panic(&b);
    CryptobyteString child = slice_nil(TYPE_BYTE);
    uint8_t x = 0, y = 0;
    if (!cryptobyte_string_read_uint8_length_prefixed(&base, &child) ||
        !cryptobyte_string_read_uint8(&child, &x) ||
        !cryptobyte_string_read_uint8(&child, &y))
        testing_t_error_v(t, "parsing failed");
    if (x != 23 || y != 42)
        testing_t_errorf_v(t, "want x, y == 23, 42; got %d, %d", x, y);
    if (base.len != 0)
        testing_t_errorf_v(t, "len(base) = %d, want 0", base.len);
    if (child.len != 0)
        testing_t_errorf_v(t, "len(child) = %d, want 0", child.len);
    cryptobyte_builder_free(&b);
}

static void TestUint8LengthPrefixedMulti(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(add_23_42, NULL));
    cryptobyte_builder_add_uint8(&b, 5);
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(add_123_234, NULL));
    builder_bytes_eq(t, &b, BS(2, 23, 42, 5, 2, 123, 234));

    CryptobyteString s = cryptobyte_builder_bytes_or_panic(&b);
    CryptobyteString child = slice_nil(TYPE_BYTE);
    uint8_t u = 0, v = 0, w = 0, x = 0, y = 0;
    if (!cryptobyte_string_read_uint8_length_prefixed(&s, &child) ||
        !cryptobyte_string_read_uint8(&child, &u) ||
        !cryptobyte_string_read_uint8(&child, &v) ||
        !cryptobyte_string_read_uint8(&s, &w) ||
        !cryptobyte_string_read_uint8_length_prefixed(&s, &child) ||
        !cryptobyte_string_read_uint8(&child, &x) ||
        !cryptobyte_string_read_uint8(&child, &y))
        testing_t_error_v(t, "parsing failed");
    if (u != 23 || v != 42 || w != 5 || x != 123 || y != 234)
        testing_t_errorf_v(
            t, "u, v, w, x, y = %d, %d, %d, %d, %d; want 23, 42, 5, 123, 234", u, v, w,
            x, y);
    if (s.len != 0)
        testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
    if (child.len != 0)
        testing_t_errorf_v(t, "len(child) = %d, want 0", child.len);
    cryptobyte_builder_free(&b);
}

static void nested_outer(void *env, CryptobyteBuilder *c) {
    (void)env;
    cryptobyte_builder_add_uint8(c, 5);
    cryptobyte_builder_add_uint8_length_prefixed(c, CONT(add_23_42, NULL));
    cryptobyte_builder_add_uint8(c, 123);
}

static void TestUint8LengthPrefixedNested(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(nested_outer, NULL));
    builder_bytes_eq(t, &b, BS(5, 5, 2, 23, 42, 123));

    CryptobyteString base = cryptobyte_builder_bytes_or_panic(&b);
    CryptobyteString child1 = slice_nil(TYPE_BYTE), child2 = slice_nil(TYPE_BYTE);
    uint8_t u = 0, v = 0, w = 0, x = 0;
    if (!cryptobyte_string_read_uint8_length_prefixed(&base, &child1))
        testing_t_error_v(t, "parsing base failed");
    if (!cryptobyte_string_read_uint8(&child1, &u) ||
        !cryptobyte_string_read_uint8_length_prefixed(&child1, &child2) ||
        !cryptobyte_string_read_uint8(&child1, &x))
        testing_t_error_v(t, "parsing child1 failed");
    if (!cryptobyte_string_read_uint8(&child2, &v) ||
        !cryptobyte_string_read_uint8(&child2, &w))
        testing_t_error_v(t, "parsing child2 failed");
    if (u != 5 || v != 23 || w != 42 || x != 123)
        testing_t_errorf_v(t, "u, v, w, x = %d, %d, %d, %d, want 5, 23, 42, 123", u, v,
                           w, x);
    if (base.len != 0)
        testing_t_errorf_v(t, "len(base) = %d, want 0", base.len);
    if (child1.len != 0)
        testing_t_errorf_v(t, "len(child1) = %d, want 0", child1.len);
    if (child2.len != 0)
        testing_t_errorf_v(t, "len(child2) = %d, want 0", child2.len);
    cryptobyte_builder_free(&b);
}

static void add_3_4(void *env, CryptobyteBuilder *c) {
    (void)env;
    cryptobyte_builder_add_uint8(c, 3);
    cryptobyte_builder_add_uint8(c, 4);
}

static void TestPreallocatedBuffer(TestingT *t) {
    Byte buf[5] = {0};
    CryptobyteBuilder b = cryptobyte_new_builder(NULL, bs(buf, 0));
    b.result.cap = 5;
    cryptobyte_builder_add_uint8(&b, 1);
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(add_3_4, NULL));
    cryptobyte_builder_add_uint16(&b, 1286); /* Outgrow buf by one byte. */
    Slice want = BS(1, 2, 3, 4, 0);
    if (!bytes_equal(bs(buf, 5), want))
        testing_t_errorf_v(t, "buf = %v want %v", bs(buf, 5), want);
    builder_bytes_eq(t, &b, BS(1, 2, 3, 4, 5, 6));
    cryptobyte_builder_free(&b);
}

typedef struct PendingEnv {
    TestingT *t;
    CryptobyteBuilder *b;
    CryptobyteBuilder *c;
} PendingEnv;

static void write_two(void *env) {
    cryptobyte_builder_add_uint8(env, 2);
}

static void pending_inner(void *env, CryptobyteBuilder *d) {
    (void)d;
    PendingEnv *e = env;
    if (!panics(write_two, e->c))
        testing_t_errorf_v(e->t,
                           "recover() = nil, want error; c.AddUint8() did not panic");
    if (!panics(write_two, e->b))
        testing_t_errorf_v(e->t,
                           "recover() = nil, want error; b.AddUint8() did not panic");
}

static void pending_outer(void *env, CryptobyteBuilder *c) {
    PendingEnv *e = env;
    e->c = c;
    cryptobyte_builder_add_uint8_length_prefixed(c, CONT(pending_inner, e));
    if (!panics(write_two, e->b))
        testing_t_errorf_v(e->t,
                           "recover() = nil, want error; b.AddUint8() did not panic");
}

static void TestWriteWithPendingChild(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    PendingEnv e = {t, &b, NULL};
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(pending_outer, &e));
    cryptobyte_builder_free(&b);
}

static void TestSetError(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_set_error(
        &b, errors_new(error_allocator(), BURROW_S("TestSetError")));

    Error err;
    Slice ret = cryptobyte_builder_bytes(&b, &err);
    if (!slice_is_nil(ret))
        testing_t_error_v(t, "expected nil result");
    if (!BURROW_FAILED(err))
        testing_t_fatal_v(t, "unexpected nil error");
    Str s = error_text(err);
    if (!str_eq(s, BURROW_S("TestSetError")))
        testing_t_errorf_v(t, "expected error %q, got %v", "TestSetError", s);
}

typedef struct UnwriteEnv {
    CryptobyteBuilder *b;
    Int n;
} UnwriteEnv;

static void unwrite(void *env) {
    UnwriteEnv *u = env;
    cryptobyte_builder_unwrite(u->b, u->n);
}

typedef struct UnwriteCont {
    TestingT *t;
    CryptobyteBuilder *outer;
} UnwriteCont;

static void unwrite_in_child(void *env, CryptobyteBuilder *b) {
    UnwriteCont *u = env;
    cryptobyte_builder_add_bytes(b, BS(1, 2, 3, 4, 5));
    UnwriteEnv ue = {b, 6};
    if (!panics(unwrite, &ue))
        testing_t_errorf_v(u->t,
                           "recover() = nil, want error; b.Unwrite() did not panic");
}

static void unwrite_outer(void *env, CryptobyteBuilder *c) {
    (void)c;
    UnwriteCont *u = env;
    /* panics (attempted unwrite while child is pending) */
    UnwriteEnv ue = {u->outer, 2};
    if (!panics(unwrite, &ue))
        testing_t_errorf_v(u->t,
                           "recover() = nil, want error; b.Unwrite() did not panic");
}

static void TestUnwrite(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_bytes(&b, BS(1, 2, 3, 4, 5));
    cryptobyte_builder_unwrite(&b, 2);
    builder_bytes_eq(t, &b, BS(1, 2, 3));

    UnwriteEnv ue = {&b, 4};
    if (!panics(unwrite, &ue))
        testing_t_errorf_v(t, "recover() = nil, want error; b.Unwrite() did not panic");
    cryptobyte_builder_free(&b);

    b = zero_builder();
    cryptobyte_builder_add_bytes(&b, BS(1, 2, 3, 4, 5));
    UnwriteCont uc = {t, &b};
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(unwrite_in_child, &uc));
    cryptobyte_builder_free(&b);

    b = zero_builder();
    cryptobyte_builder_add_bytes(&b, BS(1, 2, 3, 4, 5));
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(unwrite_outer, &uc));
    cryptobyte_builder_free(&b);
}

static void add_slice(void *env, CryptobyteBuilder *b) {
    cryptobyte_builder_add_bytes(b, *(Slice *)env);
}

static void TestFixedBuilderLengthPrefixed(TestingT *t) {
    enum { buf_cap = 10 };
    Byte inner_buf[buf_cap - 2];
    memset(inner_buf, 0xff, sizeof inner_buf);
    Slice inner = bs(inner_buf, buf_cap - 2);
    Byte buf[buf_cap];
    CryptobyteBuilder b =
        cryptobyte_new_fixed_builder(slice_from(buf, 0, buf_cap, TYPE_BYTE));
    cryptobyte_builder_add_uint16_length_prefixed(&b, CONT(add_slice, &inner));
    Slice got = cryptobyte_builder_bytes_or_panic(&b);
    if (got.len != buf_cap)
        testing_t_errorf_v(t, "Expected output length to be %d, got %d", (Int)buf_cap,
                           got.len);
}

static Byte realloc_buf[10];

static void replace_child(void *env, CryptobyteBuilder *b) {
    (void)env;
    *b = cryptobyte_new_fixed_builder(slice_from(realloc_buf, 0, 10, TYPE_BYTE));
}

static void add_replacing_child(void *env) {
    cryptobyte_builder_add_uint16_length_prefixed(env, CONT(replace_child, NULL));
}

static void TestFixedBuilderPanicReallocate(TestingT *t) {
    Byte buf[10];
    CryptobyteBuilder b =
        cryptobyte_new_fixed_builder(slice_from(buf, 0, 10, TYPE_BYTE));
    if (!panics(add_replacing_child, &b))
        testing_t_error_v(t, "Builder did not panic");
}

/* --------------------------------------------------------------- ASN.1 */

static void TestASN1Int64(TestingT *t) {
    static const struct {
        int64_t in;
        Byte want[6];
        Int n;
    } tests[] = {
        {-0x800000, {2, 3, 128, 0, 0}, 5},
        {-256, {2, 2, 255, 0}, 4},
        {-129, {2, 2, 255, 127}, 4},
        {-128, {2, 1, 128}, 3},
        {-1, {2, 1, 255}, 3},
        {0, {2, 1, 0}, 3},
        {1, {2, 1, 1}, 3},
        {2, {2, 1, 2}, 3},
        {127, {2, 1, 127}, 3},
        {128, {2, 2, 0, 128}, 4},
        {256, {2, 2, 1, 0}, 4},
        {0x800000, {2, 4, 0, 128, 0, 0}, 6},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteBuilder b = zero_builder();
        cryptobyte_builder_add_asn1_int64(&b, tests[i].in);
        Slice want = bs(tests[i].want, tests[i].n);
        Slice got = cryptobyte_builder_bytes_or_panic(&b);
        if (!bytes_equal(got, want))
            testing_t_errorf_v(t, "Bytes() = %v, want %v, (i = %d; in = %v)", got, want,
                               i, tests[i].in);

        int64_t n = 0;
        CryptobyteString s = got;
        bool ok = cryptobyte_string_read_asn1_integer_int64(&s, &n);
        if (!ok || n != tests[i].in)
            testing_t_errorf_v(
                t,
                "s.ReadASN1Integer(&n) = %v, n = %d; want true, n = %d (i "
                "= %d)",
                ok, n, tests[i].in, i);
        if (s.len != 0)
            testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
        cryptobyte_builder_free(&b);
    }
}

static void TestASN1Uint64(TestingT *t) {
    static const struct {
        uint64_t in;
        Byte want[11];
        Int n;
    } tests[] = {
        {0, {2, 1, 0}, 3},
        {1, {2, 1, 1}, 3},
        {2, {2, 1, 2}, 3},
        {127, {2, 1, 127}, 3},
        {128, {2, 2, 0, 128}, 4},
        {256, {2, 2, 1, 0}, 4},
        {0x800000, {2, 4, 0, 128, 0, 0}, 6},
        {UINT64_C(0x7fffffffffffffff),
         {2, 8, 127, 255, 255, 255, 255, 255, 255, 255},
         10},
        {UINT64_C(0x8000000000000000), {2, 9, 0, 128, 0, 0, 0, 0, 0, 0, 0}, 11},
        {UINT64_C(0xffffffffffffffff),
         {2, 9, 0, 255, 255, 255, 255, 255, 255, 255, 255},
         11},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteBuilder b = zero_builder();
        cryptobyte_builder_add_asn1_uint64(&b, tests[i].in);
        Slice want = bs(tests[i].want, tests[i].n);
        Slice got = cryptobyte_builder_bytes_or_panic(&b);
        if (!bytes_equal(got, want))
            testing_t_errorf_v(t, "Bytes() = %v, want %v, (i = %d; in = %v)", got, want,
                               i, tests[i].in);

        uint64_t n = 0;
        CryptobyteString s = got;
        bool ok = cryptobyte_string_read_asn1_integer_uint64(&s, &n);
        if (!ok || n != tests[i].in)
            testing_t_errorf_v(
                t,
                "s.ReadASN1Integer(&n) = %v, n = %d; want true, n = %d (i "
                "= %d)",
                ok, n, tests[i].in, i);
        if (s.len != 0)
            testing_t_errorf_v(t, "len(s) = %d, want 0", s.len);
        cryptobyte_builder_free(&b);
    }
}

typedef struct ReadAsn1Test {
    const char *name;
    Slice in;
    CryptobyteAsn1Tag tag;
    bool ok;
    Slice out;
} ReadAsn1Test;

typedef struct ReadAsn1Env {
    const ReadAsn1Test *test;
} ReadAsn1Env;

static void read_asn1_case(void *env, TestingT *t) {
    const ReadAsn1Test *test = env;
    CryptobyteString in = test->in, out = slice_nil(TYPE_BYTE);
    bool ok = cryptobyte_string_read_asn1(&in, &out, test->tag);
    if (ok != test->ok || (ok && !bytes_equal(out, test->out)))
        testing_t_errorf_v(t, "in.ReadASN1() = %v, want %v; out = %v, want %v", ok,
                           test->ok, out, test->out);
}

static void TestReadASN1(TestingT *t) {
    Byte non_minimal[4 + 0x80] = {0x30, 0x82, 0, 0x80};
    ReadAsn1Test tests[] = {
        {"valid", BS(0x30, 2, 1, 2), 0x30, true, BS(1, 2)},
        {"truncated", BS(0x30, 3, 1, 2), 0x30, false, {0}},
        {"zero length of length", BS(0x30, 0x80), 0x30, false, {0}},
        {"invalid long form length", BS(0x30, 0x81, 1, 1), 0x30, false, {0}},
        {"non-minimal length", bs(non_minimal, sizeof non_minimal), 0x30, false, {0}},
        {"invalid tag", BS(0xa1, 3, 0x4, 1, 1), 31, false, {0}},
        /* The tag is actually 0x4001, but tag is uint8. */
        {"high tag", BS(0x1f, 0x81, 0x80, 0x01, 2, 1, 2), 0xff, false, {0}},
        {"2**31 - 1 length", BS(0x30, 0x84, 0x7f, 0xff, 0xff, 0xff), 0x30, false, {0}},
        {"2**32 - 1 length", BS(0x30, 0x84, 0xff, 0xff, 0xff, 0xff), 0x30, false, {0}},
        {"2**63 - 1 length",
         BS(0x30, 0x88, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff),
         0x30,
         false,
         {0}},
        {"2**64 - 1 length",
         BS(0x30, 0x88, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff),
         0x30,
         false,
         {0}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, read_asn1_case, &tests[i]));
}

static void TestReadASN1Optional(TestingT *t) {
    CryptobyteString empty = slice_nil(TYPE_BYTE);
    bool present = false;
    bool ok = cryptobyte_string_read_optional_asn1(&empty, NULL, &present, 0xa0);
    if (!ok || present)
        testing_t_errorf_v(
            t,
            "empty.ReadOptionalASN1() = %v, want true; present = %v want "
            "false",
            ok, present);

    CryptobyteString in = BS(0xa1, 3, 0x4, 1, 1), out = slice_nil(TYPE_BYTE);
    ok = cryptobyte_string_read_optional_asn1(&in, &out, &present, 0xa0);
    if (!ok || present)
        testing_t_errorf_v(t,
                           "in.ReadOptionalASN1() = %v, want true, present = %v, want "
                           "false",
                           ok, present);
    ok = cryptobyte_string_read_optional_asn1(&in, &out, &present, 0xa1);
    Slice want_bytes = BS(4, 1, 1);
    if (!ok || !present || !bytes_equal(out, want_bytes))
        testing_t_errorf_v(t,
                           "in.ReadOptionalASN1() = %v, want true; present = %v, want "
                           "true; out = %v, want = %v",
                           ok, present, out, want_bytes);
}

typedef struct OptionalOctetTest {
    ReadAsn1Test r;
    bool present;
} OptionalOctetTest;

static void optional_octet_case(void *env, TestingT *t) {
    const OptionalOctetTest *test = env;
    CryptobyteString in = test->r.in;
    Slice out = slice_nil(TYPE_BYTE);
    bool present = false;
    bool ok = cryptobyte_string_read_optional_asn1_octet_string(&in, &out, &present,
                                                                test->r.tag);
    if (ok != test->r.ok || present != test->present || !bytes_equal(out, test->r.out))
        testing_t_errorf_v(
            t,
            "in.ReadOptionalASN1OctetString() = %v, want %v; present = %v "
            "want %v; out = %v, want %v",
            ok, test->r.ok, present, test->present, out, test->r.out);
}

static void TestReadASN1OptionalOctetString(TestingT *t) {
    OptionalOctetTest tests[] = {
        {{"empty", BS_EMPTY, 0xa0, true, BS_EMPTY}, false},
        {{"invalid", BS(0xa1, 3, 0x4, 2, 1), 0xa1, false, BS_EMPTY}, true},
        {{"missing", BS(0xa1, 3, 0x4, 1, 1), 0xa0, true, BS_EMPTY}, false},
        {{"present", BS(0xa1, 3, 0x4, 1, 1), 0xa1, true, BS(1)}, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].r.name),
                      BURROW_FN(TestingTFunc, optional_octet_case, &tests[i]));
}

enum { default_int = -1 };

typedef struct OptionalIntTest {
    const char *name;
    Slice in;
    CryptobyteAsn1Tag tag;
    bool ok;
    Int out;
} OptionalIntTest;

static void optional_int_case(void *env, TestingT *t) {
    const OptionalIntTest *test = env;
    CryptobyteString in = test->in;
    Int out = 0;
    bool ok = cryptobyte_string_read_optional_asn1_integer_int(&in, &out, test->tag,
                                                               default_int);
    if (ok != test->ok || (ok && out != test->out))
        testing_t_errorf_v(
            t, "in.ReadOptionalASN1Integer() = %v, want %v; out = %v, want %v", ok,
            test->ok, out, test->out);
}

static void TestReadASN1OptionalInteger(TestingT *t) {
    OptionalIntTest tests[] = {
        {"empty", BS_EMPTY, 0xa0, true, default_int},
        {"invalid", BS(0xa1, 3, 0x2, 2, 127), 0xa1, false, 0},
        {"missing", BS(0xa1, 3, 0x2, 1, 127), 0xa0, true, default_int},
        {"present", BS(0xa1, 3, 0x2, 1, 42), 0xa1, true, 42},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, optional_int_case, &tests[i]));
}

typedef struct OptionalBoolTest {
    const char *name;
    Slice in;
    CryptobyteAsn1Tag tag;
    bool ok;
    bool out;
} OptionalBoolTest;

static void optional_bool_case(void *env, TestingT *t) {
    const OptionalBoolTest *test = env;
    CryptobyteString in = test->in;
    bool out = false;
    bool ok = cryptobyte_string_read_optional_asn1_boolean(&in, &out, test->tag, false);
    if (ok != test->ok || (ok && out != test->out))
        testing_t_errorf_v(
            t, "in.ReadOptionalASN1Boolean() = %v, want %v; out = %v, want %v", ok,
            test->ok, out, test->out);
}

static void TestReadASN1OptionalBoolean(TestingT *t) {
    OptionalBoolTest tests[] = {
        {"empty", BS_EMPTY, 0xa0, true, false},
        {"invalid", BS(0xa1, 0x3, 0x1, 0x2, 0x7f), 0xa1, false, false},
        {"missing", BS(0xa1, 0x3, 0x1, 0x1, 0x7f), 0xa0, true, false},
        {"present", BS(0xa1, 0x3, 0x1, 0x1, 0xff), 0xa1, true, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, optional_bool_case, &tests[i]));
}

static const struct {
    Byte in[6];
    Int n;
    int64_t out;
} signed_tests[] = {
    {{2, 3, 128, 0, 0}, 5, -0x800000},
    {{2, 2, 255, 0}, 4, -256},
    {{2, 2, 255, 127}, 4, -129},
    {{2, 1, 128}, 3, -128},
    {{2, 1, 255}, 3, -1},
    {{2, 1, 0}, 3, 0},
    {{2, 1, 1}, 3, 1},
    {{2, 1, 2}, 3, 2},
    {{2, 1, 127}, 3, 127},
    {{2, 2, 0, 128}, 4, 128},
    {{2, 2, 1, 0}, 4, 256},
    {{2, 4, 0, 128, 0, 0}, 6, 0x800000},
};

enum { n_signed_tests = sizeof signed_tests / sizeof signed_tests[0] };

/* Repeat the same cases, reading into a big.Int. */
static void signed_big(void *env, TestingT *t) {
    (void)env;
    for (Int i = 0; i < n_signed_tests; i++) {
        CryptobyteString in = bs(signed_tests[i].in, signed_tests[i].n);
        BigInt out = BIG_INT(NULL);
        bool ok = cryptobyte_string_read_asn1_integer_big(&in, &out);
        if (!ok || big_int_int64(&out) != signed_tests[i].out)
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = %v, want true; out = %d, want %d", i,
                ok, big_int_int64(&out), signed_tests[i].out);
        big_int_free(&out);
    }
}

/* Repeat the same cases, reading into a []byte. */
static void signed_bytes(void *env, TestingT *t) {
    (void)env;
    for (Int i = 0; i < n_signed_tests; i++) {
        CryptobyteString in = bs(signed_tests[i].in, signed_tests[i].n);
        Slice out = slice_nil(TYPE_BYTE);
        bool ok = cryptobyte_string_read_asn1_integer_bytes(&in, &out);
        if (signed_tests[i].out < 0) {
            if (ok)
                testing_t_errorf_v(t, "#%d: in.ReadASN1Integer(%d) = %v, want false", i,
                                   signed_tests[i].out, ok);
            continue;
        }
        if (!ok) {
            testing_t_errorf_v(t, "#%d: in.ReadASN1Integer() = %v, want true", i, ok);
            continue;
        }
        BigInt b = BIG_INT(NULL);
        int64_t n = big_int_int64(big_int_set_bytes(&b, out));
        big_int_free(&b);
        if (n != signed_tests[i].out)
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = %v, want true; out = %x, want %d", i,
                ok, out, signed_tests[i].out);
        if (((const Byte *)out.p)[0] == 0 && out.len > 1)
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = %v; out = %x, has leading zeroes", i,
                ok, out);
    }
}

/* Repeat with the implicit-tagging functions. */
static void signed_with_tag(void *env, TestingT *t) {
    (void)env;
    for (Int i = 0; i < n_signed_tests; i++) {
        CryptobyteAsn1Tag tag =
            cryptobyte_asn1_tag_context_specific((CryptobyteAsn1Tag)((i * 3) % 32));

        Byte test_data[6];
        memcpy(test_data, signed_tests[i].in, sizeof test_data);
        /* Alter the tag of the test case. */
        test_data[0] = tag;
        Slice want = bs(test_data, signed_tests[i].n);

        CryptobyteString in = want;
        int64_t out = 0;
        bool ok = cryptobyte_string_read_asn1_int64_with_tag(&in, &out, tag);
        if (!ok || out != signed_tests[i].out)
            testing_t_errorf_v(
                t,
                "#%d: in.ReadASN1Int64WithTag() = %v, want true; out = %d, "
                "want %d",
                i, ok, out, signed_tests[i].out);

        CryptobyteBuilder b = zero_builder();
        cryptobyte_builder_add_asn1_int64_with_tag(&b, signed_tests[i].out, tag);
        Error err;
        Slice result = cryptobyte_builder_bytes(&b, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: AddASN1Int64WithTag failed: %s", i, err);
            cryptobyte_builder_free(&b);
            continue;
        }
        if (!bytes_equal(result, want))
            testing_t_errorf_v(t, "#%d: AddASN1Int64WithTag: got %x, want %x", i,
                               result, want);
        cryptobyte_builder_free(&b);
    }
}

static void TestReadASN1IntegerSigned(TestingT *t) {
    for (Int i = 0; i < n_signed_tests; i++) {
        CryptobyteString in = bs(signed_tests[i].in, signed_tests[i].n);
        int64_t out = 0;
        bool ok = cryptobyte_string_read_asn1_integer_int64(&in, &out);
        if (!ok || out != signed_tests[i].out)
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = %v, want true; out = %d, want %d", i,
                ok, out, signed_tests[i].out);
    }

    testing_t_run(t, BURROW_S("big.Int"), BURROW_FN(TestingTFunc, signed_big, NULL));
    testing_t_run(t, BURROW_S("bytes"), BURROW_FN(TestingTFunc, signed_bytes, NULL));
    testing_t_run(t, BURROW_S("WithTag"),
                  BURROW_FN(TestingTFunc, signed_with_tag, NULL));
}

static void TestReadASN1IntegerUnsigned(TestingT *t) {
    static const struct {
        Byte in[11];
        Int n;
        uint64_t out;
    } tests[] = {
        {{2, 1, 0}, 3, 0},
        {{2, 1, 1}, 3, 1},
        {{2, 1, 2}, 3, 2},
        {{2, 1, 127}, 3, 127},
        {{2, 2, 0, 128}, 4, 128},
        {{2, 2, 1, 0}, 4, 256},
        {{2, 4, 0, 128, 0, 0}, 6, 0x800000},
        {{2, 8, 127, 255, 255, 255, 255, 255, 255, 255},
         10,
         UINT64_C(0x7fffffffffffffff)},
        {{2, 9, 0, 128, 0, 0, 0, 0, 0, 0, 0}, 11, UINT64_C(0x8000000000000000)},
        {{2, 9, 0, 255, 255, 255, 255, 255, 255, 255, 255},
         11,
         UINT64_C(0xffffffffffffffff)},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteString in = bs(tests[i].in, tests[i].n);
        uint64_t out = 0;
        bool ok = cryptobyte_string_read_asn1_integer_uint64(&in, &out);
        if (!ok || out != tests[i].out)
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = %v, want true; out = %d, want %d", i,
                ok, out, tests[i].out);
    }
}

static void TestReadASN1IntegerInvalid(TestingT *t) {
    Slice tests[] = {
        BS(3, 1, 0), /* invalid tag */
        /* truncated */
        BS(2, 1),
        BS(2, 2, 0),
        /* not minimally encoded */
        BS(2, 2, 0, 1),
        BS(2, 2, 0xff, 0xff),
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        int64_t out = 0;
        if (cryptobyte_string_read_asn1_integer_int64(&tests[i], &out))
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Integer() = true, want false (out = %d)", i, out);
    }
}

static void TestASN1ObjectIdentifier(TestingT *t) {
    static const struct {
        Byte in[9];
        Int n;
        bool ok;
        Int out[4];
        Int nout;
    } tests[] = {
        {{0}, 0, false, {0}, 0},
        {{6, 0}, 2, false, {0}, 0},
        {{5, 1, 85}, 3, false, {2, 5}, 2},
        {{6, 1, 85}, 3, true, {2, 5}, 2},
        {{6, 2, 85, 0x02}, 4, true, {2, 5, 2}, 3},
        {{6, 4, 85, 0x02, 0xc0, 0x00}, 6, true, {2, 5, 2, 0x2000}, 4},
        {{6, 3, 0x81, 0x34, 0x03}, 5, true, {2, 100, 3}, 3},
        {{6, 7, 85, 0x02, 0xc0, 0x80, 0x80, 0x80, 0x80}, 9, false, {0}, 0},
        {{6, 7, 85, 0x02, 0x85, 0xc7, 0xcc, 0xfb, 0x01},
         9,
         true,
         {2, 5, 2, 1492336001},
         4},
        /* 2**31-1 */
        {{6, 7, 0x55, 0x02, 0x87, 0xff, 0xff, 0xff, 0x7f},
         9,
         true,
         {2, 5, 2, 2147483647},
         4},
        /* 2**31 */
        {{6, 7, 0x55, 0x02, 0x88, 0x80, 0x80, 0x80, 0x00}, 9, false, {0}, 0},
        /* leading 0x80 octet */
        {{6, 3, 85, 0x80, 0x02}, 5, false, {0}, 0},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Slice in_bytes = tests[i].n == 0 ? BS_EMPTY : bs(tests[i].in, tests[i].n);
        CryptobyteString in = in_bytes;
        Asn1ObjectIdentifier out = slice_nil(TYPE_INT);
        Asn1ObjectIdentifier want = slice_from((void *)(uintptr_t)tests[i].out,
                                               tests[i].nout, tests[i].nout, TYPE_INT);
        bool ok = cryptobyte_string_read_asn1_object_identifier(&in, a, &out);
        if (ok != tests[i].ok || (ok && !asn1_object_identifier_equal(out, want))) {
            testing_t_errorf_v(
                t,
                "#%d: in.ReadASN1ObjectIdentifier() = %v, want %v; out = %v, "
                "want %v",
                i, ok, tests[i].ok, asn1_object_identifier_string(out, a),
                asn1_object_identifier_string(want, a));
            continue;
        }

        CryptobyteBuilder b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
        cryptobyte_builder_add_asn1_object_identifier(&b, out);
        Error err;
        Slice result = cryptobyte_builder_bytes(&b, &err);
        bool builder_ok = !BURROW_FAILED(err);
        if (tests[i].ok != builder_ok) {
            testing_t_errorf_v(t, "#%d: error from Builder.Bytes: %s", i, err);
            continue;
        }
        if (tests[i].ok && !bytes_equal(result, in_bytes)) {
            testing_t_errorf_v(t, "#%d: reserialisation didn't match, got %x, want %x",
                               i, result, in_bytes);
            continue;
        }
    }
    arena_free(&ar);
}

/* reflect.DeepEqual for the times here: the same instant, shown the same way
 * with the same zone. */
static bool time_deep_equal(Alloc *a, Time x, Time y) {
    return time_equal(x, y) && str_eq(time_string(x, a), time_string(y, a));
}

/* tag, length and s, as an element. */
static Slice element(Alloc *a, Byte tag, const char *s) {
    Int n = (Int)strlen(s);
    Byte *p = mem_alloc(a, (size_t)n + 2, 1);
    p[0] = tag;
    p[1] = (Byte)n;
    memcpy(p + 2, s, (size_t)n);
    return bs(p, n + 2);
}

typedef struct TimeTest {
    const char *in;
    bool ok;
    Time out;
} TimeTest;

static void TestReadASN1GeneralizedTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TimeLocation *utc = time_utc_loc;
    Time zero = {0};
    TimeTest tests[] = {
        {"20100102030405Z", true, time_date(2010, 1, 2, 3, 4, 5, 0, utc)},
        {"20100102030405", false, zero},
        {"20100102030405+0607", true,
         time_date(2010, 1, 2, 3, 4, 5, 0,
                   time_fixed_zone(a, BURROW_S(""), 6 * 60 * 60 + 7 * 60))},
        {"20100102030405-0607", true,
         time_date(2010, 1, 2, 3, 4, 5, 0,
                   time_fixed_zone(a, BURROW_S(""), -6 * 60 * 60 - 7 * 60))},
        /* These are invalid times. However, the time package normalises times
         * and they were accepted in some versions. See #11134. */
        {"00000100000000Z", false, zero},
        {"20101302030405Z", false, zero},
        {"20100002030405Z", false, zero},
        {"20100100030405Z", false, zero},
        {"20100132030405Z", false, zero},
        {"20100231030405Z", false, zero},
        {"20100102240405Z", false, zero},
        {"20100102036005Z", false, zero},
        {"20100102030460Z", false, zero},
        {"-20100102030410Z", false, zero},
        {"2010-0102030410Z", false, zero},
        {"2010-0002030410Z", false, zero},
        {"201001-02030410Z", false, zero},
        {"20100102-030410Z", false, zero},
        {"2010010203-0410Z", false, zero},
        {"201001020304-10Z", false, zero},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteString in = element(a, CRYPTOBYTE_ASN1_GENERALIZED_TIME, tests[i].in);
        Time out = zero;
        bool ok = cryptobyte_string_read_asn1_generalized_time(&in, a, &out);
        if (ok != tests[i].ok || (ok && !time_deep_equal(a, out, tests[i].out)))
            testing_t_errorf_v(
                t,
                "#%d: in.ReadASN1GeneralizedTime() = %v, want %v; out = %q, "
                "want %q",
                i, ok, tests[i].ok, time_string(out, a), time_string(tests[i].out, a));
    }
    arena_free(&ar);
}

static void TestReadASN1UTCTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TimeLocation *utc = time_utc_loc;
    TimeLocation *east = time_fixed_zone(a, BURROW_S(""), 9 * 60 * 60 + 5 * 60);
    TimeLocation *west = time_fixed_zone(a, BURROW_S(""), -9 * 60 * 60 - 5 * 60);
    Time zero = {0};
    TimeTest tests[] = {
        {"000102030405Z", true, time_date(2000, 1, 2, 3, 4, 5, 0, utc)},
        {"500102030405Z", true, time_date(1950, 1, 2, 3, 4, 5, 0, utc)},
        {"490102030405Z", true, time_date(2049, 1, 2, 3, 4, 5, 0, utc)},
        {"990102030405Z", true, time_date(1999, 1, 2, 3, 4, 5, 0, utc)},
        {"250102030405Z", true, time_date(2025, 1, 2, 3, 4, 5, 0, utc)},
        {"750102030405Z", true, time_date(1975, 1, 2, 3, 4, 5, 0, utc)},
        {"000102030405+0905", true, time_date(2000, 1, 2, 3, 4, 5, 0, east)},
        {"000102030405-0905", true, time_date(2000, 1, 2, 3, 4, 5, 0, west)},
        {"0001020304Z", true, time_date(2000, 1, 2, 3, 4, 0, 0, utc)},
        {"5001020304Z", true, time_date(1950, 1, 2, 3, 4, 0, 0, utc)},
        {"0001020304+0905", true, time_date(2000, 1, 2, 3, 4, 0, 0, east)},
        {"0001020304-0905", true, time_date(2000, 1, 2, 3, 4, 0, 0, west)},
        {"000102030405Z0700", false, zero},
        {"000102030405", false, zero},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteString in = element(a, CRYPTOBYTE_ASN1_UTC_TIME, tests[i].in);
        Time out = zero;
        bool ok = cryptobyte_string_read_asn1_utc_time(&in, a, &out);
        if (ok != tests[i].ok || (ok && !time_deep_equal(a, out, tests[i].out)))
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1UTCTime() = %v, want %v; out = %q, want %q", i, ok,
                tests[i].ok, time_string(out, a), time_string(tests[i].out, a));
    }
    arena_free(&ar);
}

static void TestReadASN1BitString(TestingT *t) {
    static const struct {
        Byte in[2];
        Int n;
        bool ok;
        Byte out[1];
        Int nout;
        Int bit_length;
    } tests[] = {
        {{0}, 0, false, {0}, 0, 0},          {{0x00}, 1, true, {0}, 0, 0},
        {{0x07, 0x00}, 2, true, {0}, 1, 1},  {{0x07, 0x01}, 2, false, {0}, 0, 0},
        {{0x07, 0x40}, 2, false, {0}, 0, 0}, {{0x08, 0x00}, 2, false, {0}, 0, 0},
        {{0xff}, 1, false, {0}, 0, 0},       {{0xfe, 0x00}, 2, false, {0}, 0, 0},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Byte buf[4] = {3, (Byte)tests[i].n};
        memcpy(buf + 2, tests[i].in, (size_t)tests[i].n);
        CryptobyteString in = bs(buf, 2 + tests[i].n);
        Asn1BitString out = {slice_nil(TYPE_BYTE), 0};
        bool ok = cryptobyte_string_read_asn1_bit_string(&in, &out);
        Slice want = bs(tests[i].out, tests[i].nout);
        if (ok != tests[i].ok || (ok && (!bytes_equal(out.bytes, want) ||
                                         out.bit_length != tests[i].bit_length)))
            testing_t_errorf_v(
                t,
                "#%d: in.ReadASN1BitString() = %v, want %v; out = {%v %d}, "
                "want {%v %d}",
                i, ok, tests[i].ok, out.bytes, out.bit_length, want,
                tests[i].bit_length);
    }
}

static void TestAddASN1BigInt(TestingT *t) {
    BigInt x = BIG_INT(NULL);
    big_int_set_int64(&x, -1);
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_add_asn1_big_int(&b, &x);
    Error err;
    Slice got = cryptobyte_builder_bytes(&b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error adding -1: %v", err);
    CryptobyteString s = got;
    BigInt y = BIG_INT(NULL);
    bool ok = cryptobyte_string_read_asn1_integer_big(&s, &y);
    if (!ok || big_int_cmp(&x, &y) != 0)
        testing_t_errorf_v(t, "unexpected bytes %v, want %v", big_int_string(&y, NULL),
                           big_int_string(&x, NULL));
    big_int_free(&x);
    big_int_free(&y);
    cryptobyte_builder_free(&b);
}

static void TestReadASN1Boolean(TestingT *t) {
    static const struct {
        Byte in[3];
        Int n;
        bool ok;
        bool out;
    } tests[] = {
        {{0}, 0, false, false},
        {{0x01, 0x01, 0x00}, 3, true, false},
        {{0x01, 0x01, 0xff}, 3, true, true},
        {{0x01, 0x01, 0x01}, 3, false, false},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        CryptobyteString in = tests[i].n == 0 ? BS_EMPTY : bs(tests[i].in, tests[i].n);
        bool out = false;
        bool ok = cryptobyte_string_read_asn1_boolean(&in, &out);
        if (ok != tests[i].ok || (ok && out != tests[i].out))
            testing_t_errorf_v(
                t, "#%d: in.ReadASN1Boolean() = %v, want %v; out = %v, want %v", i, ok,
                tests[i].ok, out, tests[i].out);
    }
}

/* ------------------------------------------------------------- burrow */

/* Go has no tests of its own for these. */

static void add_big_utc(void *env, CryptobyteBuilder *c) {
    (void)env;
    Byte zeros[300] = {0};
    cryptobyte_builder_add_bytes(c, bs(zeros, sizeof zeros));
}

static void TestBurrow(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A long ASN.1 child gets the long form of the length, and reads back. */
    CryptobyteBuilder b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1(&b, CRYPTOBYTE_ASN1_SEQUENCE, CONT(add_big_utc, NULL));
    Slice got = cryptobyte_builder_bytes_or_panic(&b);
    if (got.len != 304 || ((const Byte *)got.p)[1] != 0x82 ||
        ((const Byte *)got.p)[2] != 0x01 || ((const Byte *)got.p)[3] != 0x2c)
        testing_t_errorf_v(t, "long SEQUENCE = %x...", slice_sub(got, 0, 4));
    CryptobyteString s = got, inner;
    if (!cryptobyte_string_read_asn1(&s, &inner, CRYPTOBYTE_ASN1_SEQUENCE) ||
        inner.len != 300 || s.len != 0)
        testing_t_errorf_v(t, "ReadASN1 of the long SEQUENCE: len %d, rest %d",
                           inner.len, s.len);

    /* Times write and read back. */
    Time tm = time_date(2024, 2, 29, 23, 59, 58, 0, time_utc_loc);
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1_generalized_time(&b, tm);
    cryptobyte_builder_add_asn1_utc_time(&b, tm);
    s = cryptobyte_builder_bytes_or_panic(&b);
    Time g = {0}, u = {0};
    if (!cryptobyte_string_read_asn1_generalized_time(&s, a, &g) ||
        !cryptobyte_string_read_asn1_utc_time(&s, a, &u) || !time_equal(g, tm) ||
        !time_equal(u, tm))
        testing_t_errorf_v(t, "times read back as %v and %v", time_string(g, a),
                           time_string(u, a));

    /* A UTCTime past 2049 is an error, set on the outer builder. */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1_utc_time(
        &b, time_date(2050, 1, 1, 0, 0, 0, 0, time_utc_loc));
    Error err;
    cryptobyte_builder_bytes(&b, &err);
    Str want =
        BURROW_S("cryptobyte: cannot represent 2050-01-01 00:00:00 +0000 UTC as a "
                 "UTCTime");
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "UTCTime 2050: err = %v, want %q", err, want);

    /* So is an OID with one arc. */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    Int one_arc[] = {1};
    cryptobyte_builder_add_asn1_object_identifier(&b,
                                                  slice_from(one_arc, 1, 1, TYPE_INT));
    cryptobyte_builder_bytes(&b, &err);
    if (!BURROW_FAILED(err) ||
        !str_eq(error_text(err), BURROW_S("cryptobyte: invalid OID: 1")))
        testing_t_errorf_v(t, "OID 1: err = %v", err);

    /* And a high tag number. */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1(&b, 0x1f, CONT(add_3_4, NULL));
    cryptobyte_builder_bytes(&b, &err);
    want =
        BURROW_S("cryptobyte: high-tag number identifier octets not supported: 0x1f");
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "tag 0x1f: err = %v, want %q", err, want);

    /* A child too long for its prefix. */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_uint8_length_prefixed(&b, CONT(add_big_utc, NULL));
    cryptobyte_builder_bytes(&b, &err);
    want =
        BURROW_S("cryptobyte: pending child length 300 exceeds 1-byte length prefix");
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "300 bytes in a uint8 prefix: err = %v, want %q", err,
                           want);

    /* BOOLEAN, NULL, BIT STRING, OCTET STRING and ENUMERATED. */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1_boolean(&b, true);
    cryptobyte_builder_add_asn1_null(&b);
    cryptobyte_builder_add_asn1_bit_string(&b, BS(0xaa));
    cryptobyte_builder_add_asn1_octet_string(&b, BS(1, 2));
    cryptobyte_builder_add_asn1_enum(&b, 3);
    Slice want_bytes = BS(1, 1, 0xff, 5, 0, 3, 2, 0, 0xaa, 4, 2, 1, 2, 10, 1, 3);
    builder_bytes_eq(t, &b, want_bytes);
    s = cryptobyte_builder_bytes_or_panic(&b);
    bool bv = false;
    Slice bits = {0}, oct = {0};
    Int e = 0;
    if (!cryptobyte_string_read_asn1_boolean(&s, &bv) || !bv ||
        !cryptobyte_string_skip_asn1(&s, CRYPTOBYTE_ASN1_NULL) ||
        !cryptobyte_string_read_asn1_bit_string_as_bytes(&s, &bits) ||
        !bytes_equal(bits, BS(0xaa)) ||
        !cryptobyte_string_read_asn1_bytes(&s, &oct, CRYPTOBYTE_ASN1_OCTET_STRING) ||
        !bytes_equal(oct, BS(1, 2)) || !cryptobyte_string_read_asn1_enum(&s, &e) ||
        e != 3 || !cryptobyte_string_empty(s))
        testing_t_error_v(t, "reading the simple types back failed");

    /* The optional integers and the element readers. */
    s = BS(0xa0, 3, 2, 1, 7, 0x30, 0);
    int64_t i64 = 0;
    CryptobyteString el;
    CryptobyteAsn1Tag tag = 0;
    if (!cryptobyte_string_read_optional_asn1_integer_int64(&s, &i64, 0xa0, -1) ||
        i64 != 7 ||
        !cryptobyte_string_read_optional_asn1_integer_int64(&s, &i64, 0xa1, -1) ||
        i64 != -1 || !cryptobyte_string_peek_asn1_tag(s, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_read_any_asn1_element(&s, &el, &tag) ||
        tag != CRYPTOBYTE_ASN1_SEQUENCE || !bytes_equal(el, BS(0x30, 0)) ||
        !cryptobyte_string_skip_optional_asn1(&s, 0xa0) || !cryptobyte_string_empty(s))
        testing_t_error_v(t, "optional integers and elements failed");

    /* A big.Int that is past int64 both ways. */
    BigInt n = BIG_INT(a), back = BIG_INT(a);
    big_int_set_int64(&back, INT64_MIN);
    big_int_add(&n, &back, &back); /* -2^64 */
    b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1_big_int(&b, &n);
    big_int_neg(&n, &n); /* 2^64 */
    cryptobyte_builder_add_asn1_big_int(&b, &n);
    builder_bytes_eq(
        t, &b, BS(2, 9, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 2, 9, 1, 0, 0, 0, 0, 0, 0, 0, 0));
    s = cryptobyte_builder_bytes_or_panic(&b);
    if (!cryptobyte_string_read_asn1_integer_big(&s, &back) ||
        big_int_sign(&back) >= 0 ||
        !cryptobyte_string_read_asn1_integer_big(&s, &back) ||
        big_int_cmp(&back, &n) != 0)
        testing_t_errorf_v(t, "2^64 read back as %v", big_int_string(&back, a));
    uint64_t u64 = 0;
    s = BS(2, 9, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    if (cryptobyte_string_read_asn1_integer_uint64(&s, &u64))
        testing_t_error_v(t, "2^64 read as a uint64");

    /* A fixed builder past its end. */
    Byte fixed[2];
    b = cryptobyte_new_fixed_builder(slice_from(fixed, 0, 2, TYPE_BYTE));
    cryptobyte_builder_add_uint24(&b, 1);
    cryptobyte_builder_bytes(&b, &err);
    want = BURROW_S("cryptobyte: Builder is exceeding its fixed-size buffer");
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "fixed builder: err = %v, want %q", err, want);

    arena_free(&ar);
}

static void bytes_or_panic(void *env) {
    cryptobyte_builder_bytes_or_panic(env);
}

static void TestPanics(TestingT *t) {
    CryptobyteBuilder b = zero_builder();
    cryptobyte_builder_set_error(&b, errors_new(error_allocator(), BURROW_S("boom")));
    if (!panics(bytes_or_panic, &b))
        testing_t_error_v(t, "BytesOrPanic with an error did not panic");
}

#define TESTS(X)                                                                       \
    X(TestContinuationError)                                                           \
    X(TestContinuationNonError)                                                        \
    X(TestBytes)                                                                       \
    X(TestUint8)                                                                       \
    X(TestUint16)                                                                      \
    X(TestUint24)                                                                      \
    X(TestUint24Truncation)                                                            \
    X(TestUint32)                                                                      \
    X(TestUint48)                                                                      \
    X(TestUint64)                                                                      \
    X(TestUMultiple)                                                                   \
    X(TestUint8LengthPrefixedSimple)                                                   \
    X(TestUint8LengthPrefixedMulti)                                                    \
    X(TestUint8LengthPrefixedNested)                                                   \
    X(TestPreallocatedBuffer)                                                          \
    X(TestWriteWithPendingChild)                                                       \
    X(TestSetError)                                                                    \
    X(TestUnwrite)                                                                     \
    X(TestFixedBuilderLengthPrefixed)                                                  \
    X(TestFixedBuilderPanicReallocate)                                                 \
    X(TestASN1Int64)                                                                   \
    X(TestASN1Uint64)                                                                  \
    X(TestReadASN1)                                                                    \
    X(TestReadASN1Optional)                                                            \
    X(TestReadASN1OptionalOctetString)                                                 \
    X(TestReadASN1OptionalInteger)                                                     \
    X(TestReadASN1OptionalBoolean)                                                     \
    X(TestReadASN1IntegerSigned)                                                       \
    X(TestReadASN1IntegerUnsigned)                                                     \
    X(TestReadASN1IntegerInvalid)                                                      \
    X(TestASN1ObjectIdentifier)                                                        \
    X(TestReadASN1GeneralizedTime)                                                     \
    X(TestReadASN1UTCTime)                                                             \
    X(TestReadASN1BitString)                                                           \
    X(TestAddASN1BigInt)                                                               \
    X(TestReadASN1Boolean)                                                             \
    X(TestBurrow)                                                                      \
    X(TestPanics)

TESTING_MAIN(TESTS)
