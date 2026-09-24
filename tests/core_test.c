/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"

static void TestStrLiteralsCarryTheirOwnLength(TestingT *t) {
    Str empty = BURROW_S("");
    CHECK_INT_EQ(empty.len, 0);
    CHECK(str_is_empty(empty));

    Str hello = BURROW_S("hello");
    CHECK_INT_EQ(hello.len, 5);
    CHECK_INT_EQ(hello.p[0], 'h');

    /* The whole reason Str exists. A Go string can hold a NUL and so can this
     * one, and any version of it built on char * would say the length is one. */
    Str nul = BURROW_S("a\0b");
    CHECK_INT_EQ(nul.len, 3);
    CHECK_INT_EQ(nul.p[1], 0);
    CHECK(str_has_nul(nul));
    CHECK(!str_has_nul(hello));
}

static void TestStrAtTheCStringBoundary(TestingT *t) {
    Str s = str_from_cstr("hello");
    CHECK_INT_EQ(s.len, 5);
    CHECK(str_eq(s, BURROW_S("hello")));

    /* NULL comes back from other people's code all day, so it is the empty
     * string rather than a crash. */
    CHECK(str_is_empty(str_from_cstr(NULL)));
    CHECK(str_is_empty(str_from_cstr("")));

    /* Borrowed, not copied. The result has to point at the caller's bytes. */
    const char *src = "borrowed";
    Str b = str_from_cstr(src);
    CHECK((const char *)b.p == src);
}

static void TestStrFromBytesTakesAnything(TestingT *t) {
    Byte raw[4] = {0xDE, 0xAD, 0x00, 0xBE};
    Str s = str_from_bytes(raw, 4);
    CHECK_INT_EQ(s.len, 4);
    CHECK_INT_EQ(s.p[0], 0xDE);
    CHECK(str_has_nul(s));

    CHECK(str_is_empty(str_from_bytes(NULL, 10)));
    CHECK(str_is_empty(str_from_bytes(raw, 0)));
    CHECK(str_is_empty(str_from_bytes(raw, -1)));
}

static void TestStrToCstrAllocatesAndTerminates(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    char *c = str_to_cstr(a, BURROW_S("hello"));
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK_STR_EQ(c, "hello");
        CHECK_INT_EQ(c[5], 0);
    }

    /* The empty string is still a valid C string and still needs the byte. */
    char *e = str_to_cstr(a, BURROW_STR_EMPTY);
    CHECK(e != NULL);
    if (e != NULL)
        CHECK_INT_EQ(e[0], 0);

    /* A Str with a NUL in it converts, and the result is a C string that says
     * something shorter than the truth. That is why str_has_nul exists and why
     * this is the caller's decision rather than ours. */
    Str sneaky = BURROW_S("safe\0/../../etc/passwd");
    char *s = str_to_cstr(a, sneaky);
    CHECK(s != NULL);
    if (s != NULL)
        CHECK_STR_EQ(s, "safe");
    CHECK(str_has_nul(sneaky));

    arena_free(&ar);
}

static void TestStrComparesByByte(TestingT *t) {
    CHECK(str_eq(BURROW_S("abc"), BURROW_S("abc")));
    CHECK(!str_eq(BURROW_S("abc"), BURROW_S("abd")));
    CHECK(!str_eq(BURROW_S("abc"), BURROW_S("abcd")));

    /* A zeroed Str and a pointer to no bytes are both the empty string. */
    Str zeroed = {NULL, 0};
    CHECK(str_eq(zeroed, BURROW_S("")));
    CHECK(str_eq(BURROW_S(""), zeroed));
    CHECK_INT_EQ(str_cmp(zeroed, BURROW_S("")), 0);

    CHECK(str_cmp(BURROW_S("a"), BURROW_S("b")) < 0);
    CHECK(str_cmp(BURROW_S("b"), BURROW_S("a")) > 0);
    CHECK_INT_EQ(str_cmp(BURROW_S("a"), BURROW_S("a")), 0);

    /* A prefix sorts before what it is a prefix of, which is what Go's < does
     * and what sort.Strings depends on. */
    CHECK(str_cmp(BURROW_S("ab"), BURROW_S("abc")) < 0);
    CHECK(str_cmp(BURROW_S("abc"), BURROW_S("ab")) > 0);
    CHECK(str_cmp(BURROW_S(""), BURROW_S("a")) < 0);

    /* Bytes are unsigned, so a byte above 0x7F sorts after every ASCII one.
     * Getting this wrong is the classic way a C sort disagrees with Go's. */
    CHECK(str_cmp(BURROW_S("\xff"), BURROW_S("a")) > 0);
    CHECK(str_cmp(BURROW_S("\x80"), BURROW_S("\x7f")) > 0);

    /* Case is not folded, because Go does not fold it either. */
    CHECK(str_cmp(BURROW_S("A"), BURROW_S("a")) < 0);
}

static void TestStrCloneOutlivesWhatItCameFrom(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Byte buf[5] = {'h', 'e', 'l', 'l', 'o'};
    Str borrowed = str_from_bytes(buf, 5);
    Str owned = str_clone(a, borrowed);

    CHECK(str_eq(owned, BURROW_S("hello")));
    CHECK(owned.p != borrowed.p);

    /* Scribble on the original. The clone must not notice. */
    buf[0] = 'j';
    CHECK(str_eq(borrowed, BURROW_S("jello")));
    CHECK(str_eq(owned, BURROW_S("hello")));

    /* Cloning nothing allocates nothing. */
    Str nothing = str_clone(a, BURROW_STR_EMPTY);
    CHECK(str_is_empty(nothing));

    arena_free(&ar);
}

static void TestStrPrintsWithPrintf(TestingT *t) {
    /* Not a check of our code so much as a check that the macro pair is
     * spelled in a way that compiles under -Wformat=2, which is the thing that
     * will actually break. */
    char out[32];
    int n = snprintf(out, sizeof(out), BURROW_STR_FMT, BURROW_STR_ARG(BURROW_S("hi")));
    CHECK_INT_EQ(n, 2);
    CHECK_STR_EQ(out, "hi");
}

static void TestNumbersAreTheWidthGoSays(TestingT *t) {
    /* Int follows the pointer, because Go's int does, and because the overflow
     * behaviour of a 32 bit int is visible in Go's own tests. */
    CHECK_INT_EQ(sizeof(Int), BURROW_PTR_BITS / 8);
    CHECK_INT_EQ(sizeof(Uint), BURROW_PTR_BITS / 8);
    CHECK_INT_EQ(sizeof(Uintptr), sizeof(void *));
    CHECK_INT_EQ(sizeof(Byte), 1);
    CHECK_INT_EQ(sizeof(Rune), 4);

    /* Rune is signed, because Go's rune is int32 and code that decodes UTF-8
     * returns a negative length alongside it. */
    Rune r = -1;
    CHECK(r < 0);

    /* Byte is not, because Go's byte is uint8. */
    Byte b = 0xFF;
    CHECK_INT_EQ(b, 255);

    Complex128 z = {1.0, -2.0};
    CHECK(z.re == 1.0 && z.im == -2.0);
}

static void TestThePlatformKnowsWhatItIs(TestingT *t) {
    const char *os = burrow_os_name();
    const char *arch = burrow_arch_name();
    CHECK(os != NULL && os[0] != 0);
    CHECK(arch != NULL && arch[0] != 0);
    CHECK_STR_EQ(os, BURROW_OS_NAME);
    CHECK_STR_EQ(arch, BURROW_ARCH_NAME);

    /* Exactly one of the two, and the pointer width has to agree with the
     * architecture the header picked. */
    CHECK_INT_EQ(BURROW_BIG_ENDIAN + BURROW_LITTLE_ENDIAN, 1);
    CHECK_INT_EQ(sizeof(void *) * 8, BURROW_PTR_BITS);

    /* And the byte order claim has to match what memory actually does, since a
     * wrong answer here is silent everywhere until it corrupts a wire format. */
    uint32_t word = 0x01020304u;
    Byte first = *(const Byte *)&word;
    CHECK_INT_EQ(first, BURROW_BIG_ENDIAN ? 0x01 : 0x04);
}

/* --------------------------------------------------------- the zero value
 *
 * Go promises that the zero value of every type is a useful one, and a good
 * deal of Go code is written on that promise. C cannot check this at compile
 * time, since none of the predicates below are constant expressions, so the
 * check is this test and it grows by a few lines every time a public type
 * arrives. */

static void TestTheZeroValueOfEveryTypeIsTheUsefulOne(TestingT *t) {
    /* A zeroed Str is the empty string, which is why nothing in the library
     * tests a Str pointer for NULL before reading its length. */
    Str s = BURROW_ZERO(Str);
    CHECK(str_is_empty(s));
    CHECK_INT_EQ(s.len, 0);
    CHECK(str_eq(s, BURROW_S("")));
    CHECK_INT_EQ(str_cmp(s, BURROW_S("a")), -1);
    CHECK(!str_has_nul(s));

    /* A zeroed Error is no error, so a function that returns one without
     * touching it has reported success. */
    CHECK(BURROW_OK(BURROW_ZERO(Error)));
    CHECK(!BURROW_FAILED(BURROW_ZERO(Error)));

    /* A zeroed Slice is the nil slice: no length, no capacity, nothing behind
     * it. It has no element type either, so it is the one zero value that
     * cannot do everything its non zero form can, and slice_nil is there for
     * when you need a nil slice that can be appended to. */
    Slice sl = BURROW_ZERO(Slice);
    CHECK_INT_EQ(sl.len, 0);
    CHECK_INT_EQ(sl.cap, 0);
    CHECK(sl.p == NULL);
    CHECK(slice_is_nil(sl));

    /* A nil map reads as an empty one and only fails on a write, which is
     * Go's behaviour down to the message. Reading one is common in real code:
     * a config that was never populated still answers questions. */
    CHECK_INT_EQ(map_len(NULL), 0);
    CHECK(map_get(NULL, "anything") == NULL);

    /* A zeroed interface value is the nil interface, and a zeroed function
     * value is a nil func. Both are a pair of words that are both NULL, which
     * is the reason the vtable pointer comes first in each. */
    IoReader r = BURROW_ZERO(IoReader);
    CHECK(r.vt == NULL && r.data == NULL);
    CHECK(BURROW_FUNC_IS_NIL(BURROW_ZERO(Func)));

    /* A zeroed Arena is not usable without arena_init, and that is worth
     * stating out loud because it is the one place the rule bends: an
     * allocator holds memory rather than describing it, so it has a
     * constructor. Every type that is a value rather than a resource is above
     * this line. */
    CHECK(sizeof(Arena) > 0);
}

/* ------------------------------------------------------- out parameters
 *
 * The other rule that every header follows. A Go function with two results
 * becomes a C function that returns the first and writes the rest through
 * pointers, and any of those pointers may be NULL. */

static Int divide(Int a, Int b, bool *ok, Error *err) {
    if (b == 0) {
        BURROW_OUT(ok, false);
        BURROW_OUT(err, burrow_err_out_of_memory); /* any error will do here */
        return 0;
    }
    BURROW_OUT(ok, true);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return a / b;
}

static void TestAnOutParameterMayBeNull(TestingT *t) {
    bool ok = false;
    Error err = BURROW_NO_ERROR;

    CHECK_INT_EQ(divide(10, 2, &ok, &err), 5);
    CHECK(ok);
    CHECK(BURROW_OK(err));

    CHECK_INT_EQ(divide(10, 0, &ok, &err), 0);
    CHECK(!ok);
    CHECK(BURROW_FAILED(err));

    /* The point of the rule. A caller who does not want the second result
     * should not have to declare a variable to throw away. */
    CHECK_INT_EQ(divide(10, 2, NULL, NULL), 5);
    CHECK_INT_EQ(divide(10, 0, NULL, NULL), 0);

    /* And one of the two, which is the common shape: take the error, ignore
     * the flag. */
    err = BURROW_NO_ERROR;
    CHECK_INT_EQ(divide(1, 0, NULL, &err), 0);
    CHECK(BURROW_FAILED(err));
}

#define TESTS(X)                                                                       \
    X(TestStrLiteralsCarryTheirOwnLength)                                              \
    X(TestStrAtTheCStringBoundary)                                                     \
    X(TestStrFromBytesTakesAnything)                                                   \
    X(TestStrToCstrAllocatesAndTerminates)                                             \
    X(TestStrComparesByByte)                                                           \
    X(TestStrCloneOutlivesWhatItCameFrom)                                              \
    X(TestStrPrintsWithPrintf)                                                         \
    X(TestNumbersAreTheWidthGoSays)                                                    \
    X(TestThePlatformKnowsWhatItIs)                                                    \
    X(TestTheZeroValueOfEveryTypeIsTheUsefulOne)                                       \
    X(TestAnOutParameterMayBeNull)

TESTING_MAIN(TESTS)
