/* Derived from Go's src/strings/builder_test.go.
 * Go source: go1.27.1.
 *
 * Go's var b Builder is STRINGS_BUILDER(a) here, since a builder needs an
 * allocator. Go counts allocations with testing.AllocsPerRun, and these tests
 * count them with a fixed allocator instead, which knows how many it made.
 *
 * TestBuilderGrowSizeclasses is skipped: it checks that Go's allocator rounds
 * Grow(18) up to a 24 byte size class, and an allocator here gives exactly
 * what it is asked for. The two tests at the end are not from Go.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/panic.h"

#include "check.h"

#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static void check(TestingT *t, StringsBuilder *b, Str want) {
    Str got = strings_builder_string(b);
    if (!str_eq(got, want)) {
        testing_t_errorf_v(t, "String: got %#q; want %#q", got, want);
        return;
    }
    Int n = strings_builder_len(b);
    if (n != got.len)
        testing_t_errorf_v(t, "Len: got %d; but len(String()) is %d", n, got.len);
    n = strings_builder_cap(b);
    if (n < got.len)
        testing_t_errorf_v(t, "Cap: got %d; but len(String()) is %d", n, got.len);
}

static void TestBuilder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    check(t, &b, S(""));
    Error err;
    Int n = strings_builder_write_string(&b, S("hello"), &err);
    if (BURROW_FAILED(err) || n != 5)
        testing_t_errorf_v(t, "WriteString: got %d,%v; want 5,nil", n, err);
    check(t, &b, S("hello"));
    err = strings_builder_write_byte(&b, ' ');
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "WriteByte: %s", err);
    check(t, &b, S("hello "));
    n = strings_builder_write_string(&b, S("world"), &err);
    if (BURROW_FAILED(err) || n != 5)
        testing_t_errorf_v(t, "WriteString: got %d,%v; want 5,nil", n, err);
    check(t, &b, S("hello world"));
    arena_free(&ar);
}

static void TestBuilderString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    strings_builder_write_string(&b, S("alpha"), NULL);
    check(t, &b, S("alpha"));
    Str s1 = strings_builder_string(&b);
    strings_builder_write_string(&b, S("beta"), NULL);
    check(t, &b, S("alphabeta"));
    Str s2 = strings_builder_string(&b);
    strings_builder_write_string(&b, S("gamma"), NULL);
    check(t, &b, S("alphabetagamma"));
    Str s3 = strings_builder_string(&b);

    /* Check that subsequent operations didn't change the returned strings. */
    if (!str_eq(s1, S("alpha")))
        testing_t_errorf_v(t, "first String result is now %q; want %q", s1, S("alpha"));
    if (!str_eq(s2, S("alphabeta")))
        testing_t_errorf_v(t, "second String result is now %q; want %q", s2,
                           S("alphabeta"));
    if (!str_eq(s3, S("alphabetagamma")))
        testing_t_errorf_v(t, "third String result is now %q; want %q", s3,
                           S("alphabetagamma"));
    arena_free(&ar);
}

static void TestBuilderReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    check(t, &b, S(""));
    strings_builder_write_string(&b, S("aaa"), NULL);
    Str s = strings_builder_string(&b);
    check(t, &b, S("aaa"));
    strings_builder_reset(&b);
    check(t, &b, S(""));

    /* Ensure that writing after Reset doesn't alter previously returned
     * strings. */
    strings_builder_write_string(&b, S("bbb"), NULL);
    check(t, &b, S("bbb"));
    if (!str_eq(s, S("aaa")))
        testing_t_errorf_v(
            t, "previous String result changed after Reset: got %q; want %q", s,
            S("aaa"));
    arena_free(&ar);
}

static unsigned char grow_buf[1 << 18];

typedef struct GrowCall {
    StringsBuilder *b;
    Int n;
} GrowCall;

static void call_grow(void *env) {
    GrowCall *c = (GrowCall *)env;
    strings_builder_grow(c->b, c->n);
}

static bool panics(Func f) {
    volatile bool panicked = false;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        if (r.t != TYPE_STRING)
            panic(r);
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static void TestBuilderGrow(TestingT *t) {
    static const Int grow_lens[] = {0, 100, 1000, 10000, 100000};
    for (Int i = 0; i < LEN(grow_lens); i++) {
        Int grow_len = grow_lens[i];
        Fixed fx;
        fixed_init(&fx, grow_buf, sizeof grow_buf);
        Alloc *a = fixed_allocator(&fx);
        Byte *p = (Byte *)mem_alloc(a, (size_t)grow_len + 1, 1);
        memset(p, 'a', (size_t)grow_len);
        uint64_t before = fx.allocs;

        StringsBuilder b = STRINGS_BUILDER(a);
        strings_builder_grow(&b, grow_len); /* should be only alloc, when growLen > 0 */
        if (strings_builder_cap(&b) < grow_len)
            testing_t_fatalf_v(t, "growLen=%d: Cap() is lower than growLen", grow_len);
        strings_builder_write(&b, slice_from(p, grow_len, grow_len, TYPE_BYTE), NULL);
        if (!str_eq(strings_builder_string(&b), str_from_bytes(p, grow_len)))
            testing_t_fatalf_v(t, "growLen=%d: bad data written after Grow", grow_len);

        uint64_t want_allocs = grow_len == 0 ? 0 : 1;
        if (fx.allocs - before != want_allocs)
            testing_t_errorf_v(t, "growLen=%d: got %d allocs during Write; want %v",
                               grow_len, fx.allocs - before, want_allocs);
    }

    /* when growLen < 0, should panic */
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder a = STRINGS_BUILDER(arena_allocator(&ar));
    GrowCall c = {&a, -1};
    if (!panics(BURROW_FN(Func, call_grow, &c)))
        testing_t_errorf_v(t, "a.Grow(%d) should panic()", c.n);
    arena_free(&ar);
}

static const Str s0 = BURROW_S_INIT("hello 世界");

static Int w2_write(StringsBuilder *b, Error *err) {
    return strings_builder_write(
        b, slice_from((void *)(uintptr_t)s0.p, s0.len, s0.len, TYPE_BYTE), err);
}

static Int w2_write_rune(StringsBuilder *b, Error *err) {
    return strings_builder_write_rune(b, 'a', err);
}

static Int w2_write_rune_wide(StringsBuilder *b, Error *err) {
    return strings_builder_write_rune(b, 0x4E16, err);
}

static Int w2_write_string(StringsBuilder *b, Error *err) {
    return strings_builder_write_string(b, s0, err);
}

typedef struct Write2Case {
    const char *name;
    Int (*fn)(StringsBuilder *b, Error *err);
    Int n;
    Str want;
} Write2Case;

static void write2(void *env, TestingT *t) {
    const Write2Case *tt = (const Write2Case *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err;
    Int n = tt->fn(&b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "first call: got %s", err);
    if (n != tt->n)
        testing_t_errorf_v(t, "first call: got n=%d; want %d", n, tt->n);
    check(t, &b, tt->want);
    n = tt->fn(&b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "second call: got %s", err);
    if (n != tt->n)
        testing_t_errorf_v(t, "second call: got n=%d; want %d", n, tt->n);
    Str parts[] = {tt->want, tt->want};
    check(t, &b,
          strings_join(a, slice_from(parts, 2, 2, TYPE_STRING), BURROW_STR_EMPTY));
    arena_free(&ar);
}

static void TestBuilderWrite2(TestingT *t) {
    static const Write2Case tests[] = {
        {"Write", w2_write, 12, BURROW_S_INIT("hello 世界")},
        {"WriteRune", w2_write_rune, 1, BURROW_S_INIT("a")},
        {"WriteRuneWide", w2_write_rune_wide, 3, BURROW_S_INIT("世")},
        {"WriteString", w2_write_string, 12, BURROW_S_INIT("hello 世界")},
    };
    for (Int i = 0; i < LEN(tests); i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, write2, (void *)(uintptr_t)&tests[i]));
}

static void TestBuilderWriteByte(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    Error err = strings_builder_write_byte(&b, 'a');
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    err = strings_builder_write_byte(&b, 0);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    check(t, &b, str_from_bytes("a\0", 2));
    arena_free(&ar);
}

static void TestBuilderAllocs(TestingT *t) {
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    StringsBuilder b = STRINGS_BUILDER(fixed_allocator(&fx));
    strings_builder_grow(&b, 5);
    strings_builder_write_string(&b, S("abcde"), NULL);
    Str s = strings_builder_string(&b);
    if (fx.allocs != 1)
        testing_t_errorf_v(t, "Builder allocs = %v; want 1", fx.allocs);
    CHECK(str_eq(s, S("abcde")));
}

/* Each of these writes to a builder, copies it, and then uses the copy. */
static Alloc *copy_alloc;

static void cp_string(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_byte(&a, 'x');
    StringsBuilder b = a;
    (void)strings_builder_string(&b);
}

static void cp_len(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_byte(&a, 'x');
    StringsBuilder b = a;
    strings_builder_len(&b);
}

static void cp_cap(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_byte(&a, 'x');
    StringsBuilder b = a;
    strings_builder_cap(&b);
}

static void cp_reset(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_byte(&a, 'x');
    (void)strings_builder_string(&a); /* so the reset below leaves a's buffer alone */
    StringsBuilder b = a;
    strings_builder_reset(&b);
    strings_builder_write_byte(&b, 'y');
}

static void cp_write(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    Byte x[] = {'x'};
    Byte y[] = {'y'};
    strings_builder_write(&a, slice_from(x, 1, 1, TYPE_BYTE), NULL);
    StringsBuilder b = a;
    strings_builder_write(&b, slice_from(y, 1, 1, TYPE_BYTE), NULL);
}

static void cp_write_byte(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_byte(&a, 'x');
    StringsBuilder b = a;
    strings_builder_write_byte(&b, 'y');
}

static void cp_write_string(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_string(&a, S("x"), NULL);
    StringsBuilder b = a;
    strings_builder_write_string(&b, S("y"), NULL);
}

static void cp_write_rune(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_write_rune(&a, 'x', NULL);
    StringsBuilder b = a;
    strings_builder_write_rune(&b, 'y', NULL);
}

static void cp_grow(void *env) {
    (void)env;
    StringsBuilder a = STRINGS_BUILDER(copy_alloc);
    strings_builder_grow(&a, 1);
    StringsBuilder b = a;
    strings_builder_grow(&b, 2);
}

static void TestBuilderCopyPanic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    copy_alloc = arena_allocator(&ar);
    static const struct {
        const char *name;
        void (*fn)(void *env);
        bool want_panic;
    } tests[] = {
        {"String", cp_string, false},
        {"Len", cp_len, false},
        {"Cap", cp_cap, false},
        {"Reset", cp_reset, false},
        {"Write", cp_write, true},
        {"WriteByte", cp_write_byte, true},
        {"WriteString", cp_write_string, true},
        {"WriteRune", cp_write_rune, true},
        {"Grow", cp_grow, true},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        bool got = panics(BURROW_FN(Func, tests[i].fn, NULL));
        if (got != tests[i].want_panic)
            testing_t_errorf_v(t, "%s: panicked = %v; want %v", tests[i].name, got,
                               tests[i].want_panic);
    }
    arena_free(&ar);
}

static void TestBuilderWriteInvalidRune(TestingT *t) {
    /* Invalid runes, including negative ones, should be written as
     * utf8.RuneError. */
    static const Rune runes[] = {-1, UTF8_MAX_RUNE + 1};
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < LEN(runes); i++) {
        StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
        strings_builder_write_rune(&b, runes[i], NULL);
        check(t, &b, S("\xEF\xBF\xBD"));
    }
    arena_free(&ar);
}

static void TestBuilderGrowSizeclasses(TestingT *t) {
    testing_t_skip_v(t,
                     "no size classes: an allocator gives exactly the bytes asked for");
}

/* ------------------------------------------------------------ not from Go */

/* A write that cannot get memory writes nothing, says so, and leaves the
 * builder as it was. */
static void TestBuilderOutOfMemory(TestingT *t) {
    unsigned char buf[8];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    StringsBuilder b = STRINGS_BUILDER(fixed_allocator(&fx));
    Error err;
    Int n = strings_builder_write_string(&b, S("abc"), &err);
    CHECK(n == 3 && BURROW_OK(err));
    n = strings_builder_write_string(&b, S("a much longer string"), &err);
    CHECK(n == 0 && errors_is(err, burrow_err_out_of_memory));
    check(t, &b, S("abc"));
    CHECK(!strings_builder_grow(&b, 1000));

    /* A zeroed builder has no allocator, so every write fails. */
    StringsBuilder z;
    memset(&z, 0, sizeof z);
    err = strings_builder_write_byte(&z, 'x');
    CHECK(errors_is(err, burrow_err_out_of_memory));
    check(t, &z, S(""));
}

static void TestBuilderIoWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    IoWriter w = strings_builder_as_io_writer(&b);
    Byte hi[] = {'h', 'i'};
    Error err;
    Int n = BURROW_CALL(w, write, slice_from(hi, 2, 2, TYPE_BYTE), &err);
    CHECK(n == 2 && BURROW_OK(err));
    check(t, &b, S("hi"));
    CHECK(w.vt->self_type == TYPE_STRINGS_BUILDER);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestBuilder)                                                                     \
    X(TestBuilderString)                                                               \
    X(TestBuilderReset)                                                                \
    X(TestBuilderGrow)                                                                 \
    X(TestBuilderWrite2)                                                               \
    X(TestBuilderWriteByte)                                                            \
    X(TestBuilderAllocs)                                                               \
    X(TestBuilderCopyPanic)                                                            \
    X(TestBuilderWriteInvalidRune)                                                     \
    X(TestBuilderGrowSizeclasses)                                                      \
    X(TestBuilderOutOfMemory)                                                          \
    X(TestBuilderIoWriter)

TESTING_MAIN(TESTS)
