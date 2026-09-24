/* Struct tags, which is Go's format and therefore Go's edge cases.
 *
 * The parser is small and almost all of it is refusals, so most of what is here
 * is malformed tags that have to come back as "no such key" rather than as a
 * wrong answer or a read off the end. The escape cases are in because a tag is
 * a Go string literal and a string literal has all of them.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/type.h"

#include "check.h"

#include <stdint.h>

/* Every test runs on one arena and nothing frees anything, because that is what
 * an arena is for and because a test that got the ownership wrong would show up
 * under the leak checker rather than here. */
static Alloc *a;

static bool lookup_is(Str tag, const char *key, const char *want) {
    Str got = BURROW_STR_EMPTY;

    if (!tag_lookup(a, tag, str_from_cstr(key), &got))
        return false;

    return str_eq(got, str_from_cstr(want));
}

static bool has_no(Str tag, const char *key) {
    Str got = BURROW_S("untouched");

    if (tag_lookup(a, tag, str_from_cstr(key), &got))
        return false;

    /* A miss clears the out parameter rather than leaving whatever was in it,
     * so that a caller who ignores the bool still sees an empty value. */
    return got.len == 0;
}

/* ------------------------------------------------------------- the ordinary
 */

static void TestOneKeyIsFound(TestingT *t) {
    CHECK(lookup_is(BURROW_S("json:\"id\""), "json", "id"));
}

static void TestOneKeyAmongSeveralIsFound(TestingT *t) {
    Str tag = BURROW_S("json:\"id,omitempty\" xml:\"id,attr\" db:\"user_id\"");

    CHECK(lookup_is(tag, "json", "id,omitempty"));
    CHECK(lookup_is(tag, "xml", "id,attr"));
    CHECK(lookup_is(tag, "db", "user_id"));
}

/* The options after the comma come back with the value. What they mean is the
 * asking package's business, which is the position Go takes too. */
static void TestTheValueIsNotSplitOnCommas(TestingT *t) {
    CHECK(lookup_is(BURROW_S("json:\"a,b,c\""), "json", "a,b,c"));
    CHECK(lookup_is(BURROW_S("json:\",omitempty\""), "json", ",omitempty"));
}

static void TestAKeyThatIsNotThereIsNotFound(TestingT *t) {
    CHECK(has_no(BURROW_S("json:\"id\""), "xml"));
    CHECK(has_no(BURROW_S("json:\"id\""), "jso"));
    CHECK(has_no(BURROW_S("json:\"id\""), "jsonn"));
    CHECK(has_no(BURROW_S("json:\"id\""), ""));
    CHECK(has_no(BURROW_STR_EMPTY, "json"));
}

/* Present and empty is not absent, and this is the one thing tag_get cannot
 * tell you, which is why tag_lookup exists. */
static void TestAnEmptyValueIsStillAKeyThatIsThere(TestingT *t) {
    Str got = BURROW_S("untouched");

    CHECK(tag_lookup(a, BURROW_S("json:\"\""), BURROW_S("json"), &got));
    CHECK_INT_EQ(got.len, 0);
}

static void TestTheFirstOfTwoIdenticalKeysWins(TestingT *t) {
    CHECK(lookup_is(BURROW_S("json:\"first\" json:\"second\""), "json", "first"));
}

static void TestACallerThatDoesNotWantTheValueCanSaySo(TestingT *t) {
    CHECK(tag_lookup(a, BURROW_S("json:\"id\""), BURROW_S("json"), NULL));
    CHECK(!tag_lookup(a, BURROW_S("json:\"id\""), BURROW_S("xml"), NULL));
}

static void TestTagGetGivesTheValueOrNothing(TestingT *t) {
    CHECK(
        str_eq(tag_get(a, BURROW_S("json:\"id\""), BURROW_S("json")), BURROW_S("id")));
    CHECK_INT_EQ(tag_get(a, BURROW_S("json:\"id\""), BURROW_S("xml")).len, 0);
}

/* ------------------------------------------------------------------ spacing
 */

static void TestExtraSpacesBetweenPairsAreAllowed(TestingT *t) {
    CHECK(lookup_is(BURROW_S("  json:\"id\"   xml:\"x\"  "), "json", "id"));
    CHECK(lookup_is(BURROW_S("  json:\"id\"   xml:\"x\"  "), "xml", "x"));
}

/* Go separates pairs on a space and on nothing else, so a tab ends the scan and
 * everything after it is invisible. This is a real Go behaviour that surprises
 * people, and copying it is the point. */
static void TestATabBetweenPairsEndsTheScan(TestingT *t) {
    Str tag = BURROW_S("json:\"id\"\txml:\"x\"");

    CHECK(lookup_is(tag, "json", "id"));
    CHECK(has_no(tag, "xml"));
}

/* ---------------------------------------------------------------- malformed
 */

static void TestATagThatIsNotInTheFormatHasNoKeys(TestingT *t) {
    /* No colon. */
    CHECK(has_no(BURROW_S("json"), "json"));
    /* Colon and no quote. */
    CHECK(has_no(BURROW_S("json:id"), "json"));
    /* A quote that never closes. */
    CHECK(has_no(BURROW_S("json:\"id"), "json"));
    /* An empty key. */
    CHECK(has_no(BURROW_S(":\"id\""), ""));
    /* A space where the colon should be. */
    CHECK(has_no(BURROW_S("json :\"id\""), "json"));
    /* Nothing after the key. */
    CHECK(has_no(BURROW_S("json:"), "json"));
    CHECK(has_no(BURROW_S("json"), "json"));
}

/* A pair that does not parse stops the scan, so a good pair behind a bad one is
 * not reachable. Go does the same and it is worth having a test say so, because
 * the alternative reading is that the parser skips the bad pair. */
static void TestABrokenPairHidesEverythingAfterIt(TestingT *t) {
    CHECK(lookup_is(BURROW_S("json:\"id\" broken xml:\"x\""), "json", "id"));
    CHECK(has_no(BURROW_S("json:\"id\" broken xml:\"x\""), "xml"));
}

/* ----------------------------------------------------------------- escapes
 */

static void TestAQuoteInsideTheValueDoesNotEndIt(TestingT *t) {
    CHECK(lookup_is(BURROW_S("json:\"a\\\"b\""), "json", "a\"b"));
    CHECK(lookup_is(BURROW_S("json:\"a\\\"b\" xml:\"x\""), "xml", "x"));
}

static void TestTheSimpleEscapesAreUndone(TestingT *t) {
    CHECK(lookup_is(BURROW_S("k:\"a\\nb\""), "k", "a\nb"));
    CHECK(lookup_is(BURROW_S("k:\"a\\tb\""), "k", "a\tb"));
    CHECK(lookup_is(BURROW_S("k:\"a\\rb\""), "k", "a\rb"));
    CHECK(lookup_is(BURROW_S("k:\"a\\\\b\""), "k", "a\\b"));
    CHECK(lookup_is(BURROW_S("k:\"\\a\\b\\f\\v\""), "k", "\a\b\f\v"));
}

static void TestAHexEscapeIsAByte(TestingT *t) {
    CHECK(lookup_is(BURROW_S("k:\"a\\x41b\""), "k", "aAb"));
    CHECK(lookup_is(BURROW_S("k:\"\\x7f\""), "k", "\x7f"));
}

static void TestAnOctalEscapeIsAByte(TestingT *t) {
    CHECK(lookup_is(BURROW_S("k:\"a\\101b\""), "k", "aAb"));
}

/* A Str is a pointer and a length, so a NUL in the middle of a value is a byte
 * like any other and the value is one byte long, not none. This is checked by
 * hand because there is no C string literal that says it. */
static void TestANulInTheValueIsAByteAndNotTheEnd(TestingT *t) {
    Str got = BURROW_STR_EMPTY;

    CHECK(tag_lookup(a, BURROW_S("k:\"\\000\""), BURROW_S("k"), &got));
    CHECK_INT_EQ(got.len, 1);
    CHECK_INT_EQ(got.p[0], 0);

    CHECK(tag_lookup(a, BURROW_S("k:\"a\\x00b\""), BURROW_S("k"), &got));
    CHECK_INT_EQ(got.len, 3);
    CHECK_INT_EQ(got.p[1], 0);
}

static void TestAUnicodeEscapeBecomesUtf8(TestingT *t) {
    /* Two bytes, three bytes and four bytes, which is every length above one. */
    CHECK(lookup_is(BURROW_S("k:\"\\u00e9\""), "k", "\xc3\xa9"));
    CHECK(lookup_is(BURROW_S("k:\"\\u4e16\""), "k", "\xe4\xb8\x96"));
    CHECK(lookup_is(BURROW_S("k:\"\\U0001F600\""), "k", "\xf0\x9f\x98\x80"));
}

static void TestUtf8AlreadyInTheTagComesBackUnchanged(TestingT *t) {
    CHECK(lookup_is(BURROW_S("k:\"caf\xc3\xa9\""), "k", "caf\xc3\xa9"));
}

static void TestAnEscapeThatIsNotOneIsATagWithNoKeys(TestingT *t) {
    /* No such escape. */
    CHECK(has_no(BURROW_S("k:\"a\\qb\""), "k"));
    /* Too few hex digits. */
    CHECK(has_no(BURROW_S("k:\"\\x4\""), "k"));
    CHECK(has_no(BURROW_S("k:\"\\u00e\""), "k"));
    /* A surrogate, which is not a rune. */
    CHECK(has_no(BURROW_S("k:\"\\ud800\""), "k"));
    /* Past the last rune there is. */
    CHECK(has_no(BURROW_S("k:\"\\U00110000\""), "k"));
    /* An octal byte that does not fit in one. */
    CHECK(has_no(BURROW_S("k:\"\\777\""), "k"));
    /* A backslash with nothing behind it. */
    CHECK(has_no(BURROW_S("k:\"a\\\""), "k"));
}

/* A byte that is not part of any rune becomes U+FFFD, which is what Go's
 * unquoting does and is three bytes where there was one. The two pass sizing in
 * the implementation exists for exactly this case. */
static void TestAByteThatIsNotUtf8BecomesTheReplacementRune(TestingT *t) {
    /* The literals are split so that the b after the escape is a b and not one
     * more hex digit, which is a thing C does and Go does not. */
    CHECK(lookup_is(BURROW_S("k:\"a\xff"
                             "b\""),
                    "k",
                    "a\xef\xbf\xbd"
                    "b"));
}

/* ---------------------------------------------------------- on a real field
 */

#define TAGGED_FIELDS(F, T)                                                            \
    F(T, Int, ID, "json:\"id,omitempty\" db:\"user_id\"")                              \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Hidden, "")

BURROW_STRUCT(Tagged, TAGGED_FIELDS);

static void TestADeclaredFieldsTagIsTheOneThatWasWritten(TestingT *t) {
    const Type *ty = TYPE_OF(Tagged);

    CHECK(lookup_is(ty->fields[0].tag, "json", "id,omitempty"));
    CHECK(lookup_is(ty->fields[0].tag, "db", "user_id"));
    CHECK(lookup_is(ty->fields[1].tag, "json", "name"));

    CHECK(has_no(ty->fields[1].tag, "db"));
    CHECK(has_no(ty->fields[2].tag, "json"));
}

static void TestAFieldFoundByNameCarriesItsTag(TestingT *t) {
    const Field *f = type_field_by_name(TYPE_OF(Tagged), BURROW_S("Name"));

    CHECK(f != NULL);
    CHECK(str_eq(tag_get(a, f->tag, BURROW_S("json")), BURROW_S("name")));
}

#define TESTS(X)                                                                       \
    X(TestOneKeyIsFound)                                                               \
    X(TestOneKeyAmongSeveralIsFound)                                                   \
    X(TestTheValueIsNotSplitOnCommas)                                                  \
    X(TestAKeyThatIsNotThereIsNotFound)                                                \
    X(TestAnEmptyValueIsStillAKeyThatIsThere)                                          \
    X(TestTheFirstOfTwoIdenticalKeysWins)                                              \
    X(TestACallerThatDoesNotWantTheValueCanSaySo)                                      \
    X(TestTagGetGivesTheValueOrNothing)                                                \
    X(TestExtraSpacesBetweenPairsAreAllowed)                                           \
    X(TestATabBetweenPairsEndsTheScan)                                                 \
    X(TestATagThatIsNotInTheFormatHasNoKeys)                                           \
    X(TestABrokenPairHidesEverythingAfterIt)                                           \
    X(TestAQuoteInsideTheValueDoesNotEndIt)                                            \
    X(TestTheSimpleEscapesAreUndone)                                                   \
    X(TestAHexEscapeIsAByte)                                                           \
    X(TestAnOctalEscapeIsAByte)                                                        \
    X(TestANulInTheValueIsAByteAndNotTheEnd)                                           \
    X(TestAUnicodeEscapeBecomesUtf8)                                                   \
    X(TestUtf8AlreadyInTheTagComesBackUnchanged)                                       \
    X(TestAnEscapeThatIsNotOneIsATagWithNoKeys)                                        \
    X(TestAByteThatIsNotUtf8BecomesTheReplacementRune)                                 \
    X(TestADeclaredFieldsTagIsTheOneThatWasWritten)                                    \
    X(TestAFieldFoundByNameCarriesItsTag)

static int TestMain(TestingM *m) {
    Arena arena;
    arena_init(&arena, NULL, 0);
    a = arena_allocator(&arena);
    int code = testing_m_run(m);
    arena_free(&arena);
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
