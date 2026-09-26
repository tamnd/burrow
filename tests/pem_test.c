/* Derived from Go's src/encoding/pem/pem_test.go.
 * Go source: go1.27.1.
 *
 * TestGetLine and TestLineBreaker test two internal helpers and are not here.
 * The encoder has no line breaker, and TestEncode and the round trips cover
 * the lines it writes. TestFuzz, which uses testing/quick, is FuzzRoundtrip.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/pem.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/strings.h"

#include "check.h"
#include "pem_test_data.h"

#include <string.h>

#define S BURROW_S

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

#define CHECK_STR(got, want)                                                           \
    do {                                                                               \
        Str got_ = (got), want_ = (want);                                              \
        if (!str_eq(got_, want_))                                                      \
            testing_t_errorf_v(t, "%s = %q, want %q", #got, got_, want_);              \
    } while (0)

static Slice bytes_of(const void *p, size_t n) {
    return slice_from((void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE);
}

static Slice cbytes(const char *s) {
    return bytes_of(s, strlen(s));
}

static bool bytes_eq(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

typedef struct Header {
    const char *key;
    const char *val;
} Header;

/* reflect.DeepEqual(got, want) for a block, with want given in parts. */
static bool block_is(const PemBlock *got, const char *type, const Header *hs, Int nh,
                     Slice bytes) {
    if (got == NULL || !str_eq(got->type, str_from_cstr(type)) ||
        got->headers == NULL || map_len(got->headers) != nh ||
        !bytes_eq(got->bytes, bytes))
        return false;
    for (Int i = 0; i < nh; i++) {
        const Str *v = BURROW_MAP_GET(Str, Str, got->headers, str_from_cstr(hs[i].key));
        if (v == NULL || !str_eq(*v, str_from_cstr(hs[i].val)))
            return false;
    }
    return true;
}

static const Header private_key_headers[] = {
    {"DEK-Info", "DES-EDE3-CBC,80C7C7A09690757A"},
    {"Proc-Type", "4,ENCRYPTED"},
};

static const Header private_key2_headers[] = {
    {"Proc-Type", "4,ENCRYPTED"},
    {"DEK-Info", "AES-128-CBC,BFCD243FEDBB40A4AA6DDAA1335473A4"},
    {"Content-Domain", "RFC822"},
};

static bool is_empty(const PemBlock *b) {
    return b != NULL && str_eq(b->type, S("EMPTY")) && map_len(b->headers) == 0 &&
           b->bytes.len == 0;
}

static void TestDecode(TestingT *t) {
    Slice rest;
    PemBlock *result = pem_decode(a, cbytes(pem_data), &rest);
    if (!block_is(result, "CERTIFICATE", NULL, 0,
                  bytes_of(certificate_bytes, sizeof certificate_bytes)))
        testing_t_errorf_v(t, "#0 got the wrong block");

    result = pem_decode(a, rest, &rest);
    if (!block_is(result, "RSA PRIVATE KEY", private_key_headers, 2,
                  bytes_of(private_key_bytes, sizeof private_key_bytes)))
        testing_t_errorf_v(t, "#1 got the wrong block");

    for (int i = 2; i <= 4; i++) {
        result = pem_decode(a, rest, &rest);
        if (!is_empty(result))
            testing_t_errorf_v(t, "#%d should be empty", i);
    }

    result = pem_decode(a, rest, &rest);
    if (result == NULL || !str_eq(result->type, S("VALID HEADERS")) ||
        map_len(result->headers) != 1)
        testing_t_errorf_v(t, "#5 expected single header block");

    if (rest.len != 0)
        testing_t_errorf_v(t, "expected nothing remaining of pemData, but found %q",
                           str_from_bytes(rest.p, rest.len));

    result = pem_decode(a, cbytes(pem_private_key2), NULL);
    if (!block_is(result, "RSA PRIVATE KEY", private_key2_headers, 3,
                  bytes_of(private_key2_bytes, sizeof private_key2_bytes)))
        testing_t_errorf_v(t, "#2 got the wrong block");
}

typedef struct BadTest {
    const char *name;
    const char *input;
} BadTest;

static void TestBadDecode(TestingT *t) {
    StringsBuilder rb = STRINGS_BUILDER(a);
    for (int i = 0; i < 10; i++)
        strings_builder_write_string(&rb, S("-----BEGIN \n"), NULL);
    Str repeating = strings_builder_string(&rb);

    const BadTest tests[] = {
        {"too few trailing dashes",
         "\n-----BEGIN FOO-----\ndGVzdA==\n-----END FOO----"},
        {"too many trailing dashes",
         "\n-----BEGIN FOO-----\ndGVzdA==\n-----END FOO------"},
        {"trailing non-whitespace",
         "\n-----BEGIN FOO-----\ndGVzdA==\n-----END FOO----- ."},
        {"incorrect ending type", "\n-----BEGIN FOO-----\ndGVzdA==\n-----END BAR-----"},
        {"missing ending space", "\n-----BEGIN FOO-----\ndGVzdA==\n-----ENDBAR-----"},
        {"repeating begin", NULL},
        {"missing end line", "\n-----BEGIN FOO-----\nHeader: 1"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice in = tests[i].input != NULL
                       ? cbytes(tests[i].input)
                       : bytes_of(repeating.p, (size_t)repeating.len);
        Slice rest;
        PemBlock *result = pem_decode(a, in, &rest);
        if (result != NULL)
            testing_t_errorf_v(t, "unexpected success while parsing %q", tests[i].name);
        if (rest.p != in.p || rest.len != in.len)
            testing_t_errorf_v(t, "unexpected rest for %q", tests[i].name);
    }
}

static void TestCVE202224675(TestingT *t) {
    /* Before the fix this input overflowed the stack. */
    const Int n = 10000000;
    Byte *p = mem_alloc_nozero(heap_allocator(), (size_t)(n * 12), 1);
    if (p == NULL) {
        testing_t_skipf_v(t, "no memory for the input");
        return;
    }
    static const Byte line[12] = {'-', '-', '-', '-', '-', 'B',
                                  'E', 'G', 'I', 'N', ' ', '\n'};
    for (Int i = 0; i < n; i++)
        memcpy(p + i * 12, line, sizeof line);
    Slice in = bytes_of(p, (size_t)(n * 12));
    Slice rest;
    PemBlock *result = pem_decode(a, in, &rest);
    if (result != NULL || rest.p != in.p || rest.len != in.len)
        testing_t_errorf_v(t, "decoded something out of %d BEGIN lines", n);
    mem_free(heap_allocator(), p, (size_t)(n * 12), 1);
}

static Map *headers_of(const Header *hs, Int n) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, n);
    for (Int i = 0; i < n; i++)
        BURROW_MAP_SET(Str, Str, m, str_from_cstr(hs[i].key), str_from_cstr(hs[i].val));
    return m;
}

static void TestEncode(TestingT *t) {
    PemBlock b = {S("RSA PRIVATE KEY"), headers_of(private_key2_headers, 3),
                  bytes_of(private_key2_bytes, sizeof private_key2_bytes), 0};
    Slice r = pem_encode_to_memory(a, &b);
    CHECK_STR(str_from_bytes(r.p, r.len), str_from_cstr(pem_private_key2));
}

static void TestBadEncode(TestingT *t) {
    static const Header bad[] = {{"X:Y", "Z"}};
    PemBlock b = {S("BAD"), headers_of(bad, 1), slice_nil(TYPE_BYTE), 0};
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = pem_encode(bytes_buffer_as_io_writer(&buf), &b);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Encode did not report invalid header");
    CHECK_STR(error_text(err),
              S("pem: cannot encode a header key that contains a colon"));
    if (bytes_buffer_len(&buf) != 0)
        testing_t_fatalf_v(t, "Encode wrote data before reporting invalid header");
    Slice data = pem_encode_to_memory(a, &b);
    if (data.p != NULL)
        testing_t_fatalf_v(t, "EncodeToMemory returned non-nil data");
}

typedef struct StrangeCase {
    const char *name;
    const char *pem;
} StrangeCase;

static const StrangeCase strange_cases[] = {
    {"invalid section (not base64)",
     "-----BEGIN COMMENT-----\nfoo foo foo\n-----END "
     "COMMENT-----\n-----BEGIN TEST BLOCK-----\naGVsbG8=\n"
     "-----END TEST BLOCK-----"},
    {"leading garbage on block",
     "foo foo foo-----BEGIN CERTIFICATE-----\n"
     "MCowBQYDK2VwAyEApVjJeLW5MoP6uR3+OeITokM+rBDng6dgl1vvhcy+wws=\n"
     "-----END PUBLIC KEY-----\n-----BEGIN TEST BLOCK-----\naGVsbG8=\n"
     "-----END TEST BLOCK-----"},
    {"leading garbage",
     "foo foo foo\n-----BEGIN TEST BLOCK-----\naGVsbG8=\n-----END TEST BLOCK-----"},
    {"leading partial block", "foo foo foo\n-----END COMMENT-----\n-----BEGIN TEST "
                              "BLOCK-----\naGVsbG8=\n-----END TEST BLOCK-----"},
    {"multiple BEGIN",
     "-----BEGIN TEST BLOCK-----\n-----BEGIN TEST BLOCK-----\n-----BEGIN "
     "TEST BLOCK-----\naGVsbG8=\n-----END TEST BLOCK-----"},
    {"multiple END",
     "-----BEGIN TEST BLOCK-----\naGVsbG8=\n-----END TEST BLOCK-----\n-----"
     "END TEST BLOCK-----\n-----END TEST BLOCK-----"},
    {"leading malformed BEGIN",
     "-----BEGIN PUBLIC KEY\naGVsbG8=\n-----END PUBLIC KEY-----\n-----BEGIN TEST "
     "BLOCK-----\naGVsbG8=\n-----END TEST BLOCK-----"},
};

static void run_strange_case(void *env, TestingT *t) {
    const StrangeCase *tc = (const StrangeCase *)env;
    PemBlock *block = pem_decode(a, cbytes(tc->pem), NULL);
    if (block == NULL) {
        testing_t_fatalf_v(t, "expected valid block");
        return;
    }
    if (!str_eq(block->type, S("TEST BLOCK")))
        testing_t_fatalf_v(t, "unexpected block returned, got type %q, want type %q",
                           block->type, S("TEST BLOCK"));
    if (!bytes_eq(block->bytes, cbytes("hello")))
        testing_t_fatalf_v(t, "unexpected block content, got %x, want %x", block->bytes,
                           cbytes("hello"));
}

static void TestDecodeStrangeCases(TestingT *t) {
    for (size_t i = 0; i < sizeof strange_cases / sizeof strange_cases[0]; i++)
        testing_t_run(t, str_from_cstr(strange_cases[i].name),
                      BURROW_FN(TestingTFunc, run_strange_case,
                                (void *)(uintptr_t)&strange_cases[i]));
}

static void TestJustEnd(TestingT *t) {
    if (pem_decode(a, cbytes("\n-----END PUBLIC KEY-----"), NULL) != NULL)
        testing_t_fatalf_v(t, "unexpected block");
}

static void TestMissingEndTrailer(TestingT *t) {
    (void)t;
    pem_decode(a, bytes_of(missing_end_trailer, sizeof missing_end_trailer), NULL);
}

/* Not in Go's tests. */

/* Everything pem_decode allocates goes back through pem_block_free, headers,
 * spaces in the data and a block that fails to decode along the way. */
static void TestBlockFree(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *h = track_allocator(&tr);
    const char *in =
        "-----BEGIN X-----\n!!!!\n-----END X-----\n"
        "-----BEGIN KEY-----\nA: 1\nB:  two \nA: 3\n\naGVs bG8=\n"
        "\t\n-----END KEY-----\n-----BEGIN EMPTY-----\n-----END EMPTY-----\n";
    Slice rest = cbytes(in);
    PemBlock *b = pem_decode(h, rest, &rest);
    if (b == NULL) {
        testing_t_fatalf_v(t, "no block");
        return;
    }
    CHECK_STR(b->type, S("KEY"));
    CHECK_INT_EQ(map_len(b->headers), 2);
    const Str *v = BURROW_MAP_GET(Str, Str, b->headers, S("A"));
    CHECK_STR(v != NULL ? *v : S("<none>"), S("3"));
    v = BURROW_MAP_GET(Str, Str, b->headers, S("B"));
    CHECK_STR(v != NULL ? *v : S("<none>"), S("two"));
    CHECK(bytes_eq(b->bytes, cbytes("hello")));
    pem_block_free(h, b);
    b = pem_decode(h, rest, &rest);
    CHECK(is_empty(b));
    CHECK_INT_EQ(rest.len, 0);
    pem_block_free(h, b);
    pem_block_free(h, NULL);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

/* NULL headers and bytes are fine to encode, and lines break at 64. */
static void TestEncodeLines(TestingT *t) {
    Byte data[100];
    for (int i = 0; i < 100; i++)
        data[i] = (Byte)i;
    PemBlock b = {S("DATA"), NULL, bytes_of(data, 48), 0};
    Slice r = pem_encode_to_memory(a, &b);
    CHECK_STR(str_from_bytes(r.p, r.len),
              S("-----BEGIN DATA-----\n"
                "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
                "-----END DATA-----\n"));
    b.bytes = bytes_of(data, 100);
    r = pem_encode_to_memory(a, &b);
    CHECK_STR(str_from_bytes(r.p, r.len),
              S("-----BEGIN DATA-----\n"
                "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
                "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f\n"
                "YGFiYw==\n"
                "-----END DATA-----\n"));
    b.bytes = slice_nil(TYPE_BYTE);
    r = pem_encode_to_memory(a, &b);
    CHECK_STR(str_from_bytes(r.p, r.len),
              S("-----BEGIN DATA-----\n-----END DATA-----\n"));
}

/* ------------------------------------------------------------------ fuzzing */

static void fuzz_decode(void *env, TestingT *t, Slice args) {
    (void)env;
    (void)t;
    Bytes in = testing_fuzz_arg(args, 0, Bytes);
    Arena far;
    arena_init(&far, NULL, 0);
    Slice rest = in;
    while (pem_decode(arena_allocator(&far), rest, &rest) != NULL) {
    }
    arena_free(&far);
}

static void FuzzDecode(TestingF *f) {
    Bytes seed = cbytes(pem_private_key2);
    testing_f_add_v(f, BURROW_ANY(TYPE_BYTES, &seed));
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_decode, NULL), TYPE_BYTES);
}

static bool is_bad(Str s) {
    return strings_contains_any(s, S("\r\n")) || !str_eq(strings_trim_space(s), s);
}

/* TestFuzz. The input is cut at NUL bytes into the type, a header key and
 * value, and the data, and whatever encodes has to decode back the same. */
static void fuzz_roundtrip(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes in = testing_fuzz_arg(args, 0, Bytes);
    Str parts[4] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                    BURROW_STR_EMPTY};
    Str s = str_from_bytes(in.p, in.len);
    for (int i = 0; i < 3; i++) {
        Str after;
        bool found;
        parts[i] = strings_cut(s, S("\0"), &after, &found);
        s = found ? after : BURROW_STR_EMPTY;
    }
    parts[3] = s;
    Str type = parts[0], key = parts[1], val = parts[2];
    if (is_bad(type) || strings_contains(type, S(":")))
        return;
    bool with_header = key.len > 0;
    if (with_header && (is_bad(key) || is_bad(val) || strings_contains(key, S(":"))))
        return;

    Arena far;
    arena_init(&far, NULL, 0);
    Alloc *fa = arena_allocator(&far);
    PemBlock b = {type, NULL, bytes_of(parts[3].p, (size_t)parts[3].len), 0};
    if (with_header) {
        b.headers = map_make(fa, TYPE_STRING, TYPE_STRING, 1);
        BURROW_MAP_SET(Str, Str, b.headers, key, val);
    }
    BytesBuffer buf = BYTES_BUFFER(fa);
    Error err = pem_encode(bytes_buffer_as_io_writer(&buf), &b);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Encode of %q resulted in error: %v", type, err);
    } else {
        Slice rest;
        PemBlock *d = pem_decode(fa, bytes_buffer_bytes(&buf), &rest);
        bool ok = d != NULL && str_eq(d->type, type) && bytes_eq(d->bytes, b.bytes) &&
                  map_len(d->headers) == (with_header ? 1 : 0);
        if (ok && with_header) {
            const Str *got = BURROW_MAP_GET(Str, Str, d->headers, key);
            ok = got != NULL && str_eq(*got, val);
        }
        if (!ok)
            testing_t_errorf_v(t, "Encode of %q decoded differently: %q", type,
                               str_from_bytes(bytes_buffer_bytes(&buf).p,
                                              bytes_buffer_bytes(&buf).len));
        else if (rest.len != 0)
            testing_t_errorf_v(
                t, "Encode of %q decoded correctly, but with %x left over", type, rest);
    }
    arena_free(&far);
}

static void FuzzRoundtrip(TestingF *f) {
    static const char seeds[][48] = {
        "EMPTY\0\0\0",
        "RSA PRIVATE KEY\0Proc-Type\0"
        "4,ENCRYPTED\0\x01\x02",
        "\0\0\0",
        "X\0k\0v v\0"
        "0123456789abcdef0123456789abcdef",
    };
    static const size_t lens[] = {8, 40, 3, 40};
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        Bytes b = bytes_of(seeds[i], lens[i]);
        testing_f_add_v(f, BURROW_ANY(TYPE_BYTES, &b));
    }
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_roundtrip, NULL), TYPE_BYTES);
}

static int TestMain(TestingM *m) {
    setup();
    int rc = testing_m_run(m);
    teardown();
    return rc;
}

#define TESTS(X)                                                                       \
    X(TestDecode)                                                                      \
    X(TestBadDecode)                                                                   \
    X(TestCVE202224675)                                                                \
    X(TestEncode)                                                                      \
    X(TestBadEncode)                                                                   \
    X(TestDecodeStrangeCases)                                                          \
    X(TestJustEnd)                                                                     \
    X(TestMissingEndTrailer)                                                           \
    X(TestBlockFree)                                                                   \
    X(TestEncodeLines)                                                                 \
    X(FuzzDecode)                                                                      \
    X(FuzzRoundtrip)
TESTING_MAIN_WITH(TestMain, TESTS)
