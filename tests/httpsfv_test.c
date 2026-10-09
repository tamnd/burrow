/* Derived from golang.org/x/net/internal/httpsfv/httpsfv_test.go, the package Go
 * bundles as net/http/internal/httpsfv without its test.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * Go's TestConsumeParameter and TestParseParameter loop over tests[len(tests)-1:],
 * so they only ever run their last case. These run every case.
 *
 * TestParseDisplayStringOutOfMemory is burrow's own.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/httpsfv.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S(x) BURROW_S_INIT(x)

/* The most members, items or parameters a case wants, and one more for the
 * entry that ends the list, which is the one whose p is NULL. */
#define MAX_PIECES 5

static Int count(const Str *want) {
    Int n = 0;
    while (n < MAX_PIECES && want[n].p != NULL)
        n++;
    return n;
}

typedef struct Pieces {
    Int n;
    Str items[MAX_PIECES];
    Str params[MAX_PIECES];
} Pieces;

static void collect(void *env, Str item, Str param) {
    Pieces *g = env;
    if (g->n < MAX_PIECES) {
        g->items[g->n] = item;
        g->params[g->n] = param;
    }
    g->n++;
}

static bool pieces_equal(const Str *got, Int n, const Str *want) {
    if (n != count(want))
        return false;
    for (Int i = 0; i < n; i++)
        if (!str_eq(got[i], want[i]))
            return false;
    return true;
}

/* Whether consumed followed by rest is in. */
static bool split_of(Str consumed, Str rest, Str in) {
    if (consumed.len + rest.len != in.len)
        return false;
    if (consumed.len > 0 && memcmp(in.p, consumed.p, (size_t)consumed.len) != 0)
        return false;
    if (rest.len > 0 && memcmp(in.p + consumed.len, rest.p, (size_t)rest.len) != 0)
        return false;
    return true;
}

static void TestParseList(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        Str want_members[MAX_PIECES];
        Str want_params[MAX_PIECES];
        bool want_ok;
    } tests[] = {
        {
            .name = "valid list",
            .in = S("a, b,c"),
            .want_members = {S("a"), S("b"), S("c")},
            .want_params = {S(""), S(""), S("")},
            .want_ok = true,
        },
        {
            .name = "valid list with params",
            .in = S("a;foo=bar, b,c; baz=baz"),
            .want_members = {S("a"), S("b"), S("c")},
            .want_params = {S(";foo=bar"), S(""), S("; baz=baz")},
            .want_ok = true,
        },
        {
            .name = "valid list with fake commas",
            .in = S("a;foo=\",\", (\",\")"),
            .want_members = {S("a"), S("(\",\")")},
            .want_params = {S(";foo=\",\""), S("")},
            .want_ok = true,
        },
        {
            .name = "valid list with inner list member",
            .in = S("(a b c); foo, bar;baz"),
            .want_members = {S("(a b c)"), S("bar")},
            .want_params = {S("; foo"), S(";baz")},
            .want_ok = true,
        },
        {
            .name = "invalid list with trailing comma",
            .in = S("a;foo=bar, b,c; baz=baz,"),
            .want_members = {S("a"), S("b"), S("c")},
            .want_params = {S(";foo=bar"), S(""), S("; baz=baz")},
        },
        {
            .name = "invalid list with unclosed string",
            .in = S("\", b, c,d"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Pieces got = {0};
        bool ok = burrow__httpsfv_parse_list(tests[i].in,
                                             BURROW_FN(HttpsfvItemFunc, collect, &got));
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!pieces_equal(got.items, got.n, tests[i].want_members)) {
            testing_t_fatalf_v(t, "test %q: members mismatch", tests[i].name);
            return;
        }
        if (!pieces_equal(got.params, got.n, tests[i].want_params)) {
            testing_t_fatalf_v(t, "test %q: params mismatch", tests[i].name);
            return;
        }
    }
}

static void TestConsumeBareInnerList(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        Str want_bare_items[MAX_PIECES];
        Str want_params[MAX_PIECES];
        bool want_ok;
    } tests[] = {
        {
            .name = "valid inner list without param",
            .in = S("(a b c)"),
            .want_bare_items = {S("a"), S("b"), S("c")},
            .want_params = {S(""), S(""), S("")},
            .want_ok = true,
        },
        {
            .name = "valid inner list with param",
            .in = S("(a;d b c;e)"),
            .want_bare_items = {S("a"), S("b"), S("c")},
            .want_params = {S(";d"), S(""), S(";e")},
            .want_ok = true,
        },
        {
            .name = "valid inner list with fake ending parenthesis",
            .in = S("(\")\";foo=\")\")"),
            .want_bare_items = {S("\")\"")},
            .want_params = {S(";foo=\")\"")},
            .want_ok = true,
        },
        {
            .name = "valid inner list with list parameter",
            .in = S("(a b;c); d"),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
            .want_ok = true,
        },
        {
            .name = "valid inner list with more content after",
            .in = S("(a b;c); d, a"),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
            .want_ok = true,
        },
        {
            .name = "invalid inner list",
            .in = S("(a b;c "),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Pieces got = {0};
        Str consumed = BURROW_STR_EMPTY;
        Str rest = BURROW_STR_EMPTY;
        bool ok = burrow__httpsfv_consume_bare_inner_list(
            tests[i].in, BURROW_FN(HttpsfvItemFunc, collect, &got), &consumed, &rest);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!pieces_equal(got.items, got.n, tests[i].want_bare_items)) {
            testing_t_fatalf_v(t, "test %q: bare items mismatch", tests[i].name);
            return;
        }
        if (!pieces_equal(got.params, got.n, tests[i].want_params)) {
            testing_t_fatalf_v(t, "test %q: params mismatch", tests[i].name);
            return;
        }
        if (!split_of(consumed, rest, tests[i].in)) {
            testing_t_fatalf_v(t, "test %q: %q + %q != %q", tests[i].name, consumed,
                               rest, tests[i].in);
            return;
        }
    }
}

static void TestParseBareInnerList(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        Str want_bare_items[MAX_PIECES];
        Str want_params[MAX_PIECES];
        bool want_ok;
    } tests[] = {
        {
            .name = "valid inner list",
            .in = S("(a b;c)"),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
            .want_ok = true,
        },
        {
            .name = "valid inner list with list parameter",
            .in = S("(a b;c); d"),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
        },
        {
            .name = "invalid inner list",
            .in = S("(a b;c "),
            .want_bare_items = {S("a"), S("b")},
            .want_params = {S(""), S(";c")},
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Pieces got = {0};
        bool ok = burrow__httpsfv_parse_bare_inner_list(
            tests[i].in, BURROW_FN(HttpsfvItemFunc, collect, &got));
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!pieces_equal(got.items, got.n, tests[i].want_bare_items)) {
            testing_t_fatalf_v(t, "test %q: bare items mismatch", tests[i].name);
            return;
        }
        if (!pieces_equal(got.params, got.n, tests[i].want_params)) {
            testing_t_fatalf_v(t, "test %q: params mismatch", tests[i].name);
            return;
        }
    }
}

typedef struct ItemCase {
    const char *name;
    Str in;
    Str want_bare_item;
    Str want_param;
    bool want_ok;
} ItemCase;

/* The last bare item and parameter f was given. */
static void keep_last(void *env, Str item, Str param) {
    Str *g = env;
    g[0] = item;
    g[1] = param;
}

static void TestConsumeItem(TestingT *t) {
    static const ItemCase tests[] = {
        {
            .name = "valid bare item",
            .in = S("fookey"),
            .want_bare_item = S("fookey"),
            .want_ok = true,
        },
        {
            .name = "valid bare item and param",
            .in = S("fookey; a=\"123\""),
            .want_bare_item = S("fookey"),
            .want_param = S("; a=\"123\""),
            .want_ok = true,
        },
        {
            .name = "valid item with content after",
            .in = S("fookey; a=\"123\", otheritem; otherparam=1"),
            .want_bare_item = S("fookey"),
            .want_param = S("; a=\"123\""),
            .want_ok = true,
        },
        {
            .name = "invalid just param",
            .in = S(";a=\"123\""),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got[2] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        Str consumed = BURROW_STR_EMPTY;
        Str rest = BURROW_STR_EMPTY;
        bool ok = burrow__httpsfv_consume_item(
            tests[i].in, BURROW_FN(HttpsfvItemFunc, keep_last, got), &consumed, &rest);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!str_eq(tests[i].want_bare_item, got[0])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[0], tests[i].want_bare_item);
            return;
        }
        if (!str_eq(tests[i].want_param, got[1])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[1], tests[i].want_param);
            return;
        }
        if (!split_of(consumed, rest, tests[i].in)) {
            testing_t_fatalf_v(t, "test %q: %q + %q != %q", tests[i].name, consumed,
                               rest, tests[i].in);
            return;
        }
    }
}

static void TestParseItem(TestingT *t) {
    static const ItemCase tests[] = {
        {
            .name = "valid bare item",
            .in = S("fookey"),
            .want_bare_item = S("fookey"),
            .want_ok = true,
        },
        {
            .name = "valid bare item and param",
            .in = S("fookey; a=\"123\""),
            .want_bare_item = S("fookey"),
            .want_param = S("; a=\"123\""),
            .want_ok = true,
        },
        {
            .name = "valid item with content after",
            .in = S("fookey; a=\"123\", otheritem; otherparam=1"),
            .want_bare_item = S("fookey"),
            .want_param = S("; a=\"123\""),
        },
        {
            .name = "invalid just param",
            .in = S(";a=\"123\""),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got[2] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        bool ok = burrow__httpsfv_parse_item(
            tests[i].in, BURROW_FN(HttpsfvItemFunc, keep_last, got));
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!str_eq(tests[i].want_bare_item, got[0])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[0], tests[i].want_bare_item);
            return;
        }
        if (!str_eq(tests[i].want_param, got[1])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[1], tests[i].want_param);
            return;
        }
    }
}

/* The value and parameters of the key "want". */
static void keep_want(void *env, Str key, Str val, Str param) {
    Str *g = env;
    if (str_eq(key, BURROW_S("want"))) {
        g[0] = val;
        g[1] = param;
    }
}

static void TestParseDictionary(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        Str want_val;
        Str want_param;
        bool want_ok;
    } tests[] = {
        {
            .name = "valid dictionary with simple value",
            .in = S("a=b, want=foo, c=d"),
            .want_val = S("foo"),
            .want_ok = true,
        },
        {
            .name = "valid dictionary with implicit value",
            .in = S("a, want, c=d"),
            .want_val = S("?1"),
            .want_ok = true,
        },
        {
            .name = "valid dictionary with parameter",
            .in = S("a, want=foo;bar=baz, c=d"),
            .want_val = S("foo"),
            .want_param = S(";bar=baz"),
            .want_ok = true,
        },
        {
            .name = "valid dictionary with inner list",
            .in = S("a, want=(a b c d;e;f);g=h, c=d"),
            .want_val = S("(a b c d;e;f)"),
            .want_param = S(";g=h"),
            .want_ok = true,
        },
        {
            .name = "valid dictionary with fake commas",
            .in = S("a=(\";\");b=\";\",want=foo;bar"),
            .want_val = S("foo"),
            .want_param = S(";bar"),
            .want_ok = true,
        },
        {
            .name = "invalid dictionary with bad key",
            .in = S("UPPERCASEKEY=BAD, want=foo, c=d"),
        },
        {
            .name = "invalid dictionary with trailing comma",
            .in = S("trailing=comma,"),
        },
        {
            .name = "invalid dictionary with unclosed string",
            .in = S("a=\"\"\",want=foo;bar"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got[2] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        bool ok = burrow__httpsfv_parse_dictionary(
            tests[i].in, BURROW_FN(HttpsfvDictFunc, keep_want, got));
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!str_eq(tests[i].want_val, got[0])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[0], tests[i].want_val);
            return;
        }
        if (!str_eq(tests[i].want_param, got[1])) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got[1], tests[i].want_param);
            return;
        }
    }
}

/* The any the parameter tests keep the value of "want" in. */
typedef enum ParamKind {
    PARAM_NIL,
    PARAM_BOOL,
    PARAM_STRING,
    PARAM_INT,
    PARAM_FLOAT,
} ParamKind;

typedef struct ParamValue {
    double f;
    Str s;
    Int i;
    ParamKind kind;
    bool b;
} ParamValue;

static bool param_value_equal(ParamValue x, ParamValue y) {
    if (x.kind != y.kind)
        return false;
    switch (x.kind) {
    case PARAM_NIL:
        return true;
    case PARAM_BOOL:
        return x.b == y.b;
    case PARAM_STRING:
        return str_eq(x.s, y.s);
    case PARAM_INT:
        return x.i == y.i;
    case PARAM_FLOAT:
        return x.f == y.f;
    default:
        return false;
    }
}

static void keep_want_param(void *env, Str key, Str val) {
    ParamValue *got = env;
    if (!str_eq(key, BURROW_S("want")))
        return;
    if (strings_has_prefix(val, BURROW_S("?"))) { /* Bool */
        got->kind = PARAM_BOOL;
        got->b = str_eq(val, BURROW_S("?1"));
    } else if (strings_has_prefix(val, BURROW_S("\"")) || /* String */
               strings_has_prefix(val, BURROW_S(":"))) {  /* Byte sequence */
        got->kind = PARAM_STRING;
        got->s = str_from_bytes(val.p + 1, val.len - 2);
    } else if (strings_has_prefix(val, BURROW_S("*"))) { /* Token */
        got->kind = PARAM_STRING;
        got->s = val;
    } else {
        Error err = BURROW_NO_ERROR;
        Int n = strconv_atoi(val, &err);
        if (BURROW_OK(err)) { /* Integer */
            got->kind = PARAM_INT;
            got->i = n;
            return;
        }
        err = BURROW_NO_ERROR;
        double f = strconv_parse_float(val, 64, &err);
        if (BURROW_OK(err)) { /* Float */
            got->kind = PARAM_FLOAT;
            got->f = f;
        }
    }
}

typedef struct ParamCase {
    const char *name;
    Str in;
    ParamValue want;
    bool want_ok;
} ParamCase;

#define WANT_STRING(x) {.kind = PARAM_STRING, .s = S(x)}

static void TestConsumeParameter(TestingT *t) {
    static const ParamCase tests[] = {
        {
            .name = "valid string",
            .in = S(";parameter;want=\"wantvalue\""),
            .want = WANT_STRING("wantvalue"),
            .want_ok = true,
        },
        {
            .name = "valid integer",
            .in = S(";parameter;want=123456;something"),
            .want = {.kind = PARAM_INT, .i = 123456},
            .want_ok = true,
        },
        {
            .name = "valid decimal",
            .in = S(";parameter;want=3.14;something"),
            .want = {.kind = PARAM_FLOAT, .f = 3.14},
            .want_ok = true,
        },
        {
            .name = "valid implicit bool",
            .in = S(";parameter;want;something"),
            .want = {.kind = PARAM_BOOL, .b = true},
            .want_ok = true,
        },
        {
            .name = "valid token",
            .in = S(";want=*atoken;something"),
            .want = WANT_STRING("*atoken"),
            .want_ok = true,
        },
        {
            .name = "valid byte sequence",
            .in = S(";want=:eWF5Cg==:;something"),
            .want = WANT_STRING("eWF5Cg=="),
            .want_ok = true,
        },
        {
            .name = "valid repeated key",
            .in = S(";want=:eWF5Cg==:;now;want=1;is;repeated;want=\"overwritten!\""),
            .want = WANT_STRING("overwritten!"),
            .want_ok = true,
        },
        {
            .name = "valid parameter with content after",
            .in = S(";want=:eWF5Cg==:;now;want=1;is;repeated;want=\"overwritten!\", "
                    "some=stuff"),
            .want = WANT_STRING("overwritten!"),
            .want_ok = true,
        },
        {
            .name = "invalid parameter",
            .in = S(";UPPERCASEKEY=NOT_ACCEPTED"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ParamValue got = {0};
        Str consumed = BURROW_STR_EMPTY;
        Str rest = BURROW_STR_EMPTY;
        bool ok = burrow__httpsfv_consume_parameter(
            tests[i].in, BURROW_FN(HttpsfvParamFunc, keep_want_param, &got), &consumed,
            &rest);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!param_value_equal(got, tests[i].want)) {
            testing_t_fatalf_v(t, "test %q: mismatch", tests[i].name);
            return;
        }
        if (!split_of(consumed, rest, tests[i].in)) {
            testing_t_fatalf_v(t, "test %q: %q + %q != %q", tests[i].name, consumed,
                               rest, tests[i].in);
            return;
        }
    }
}

static void TestParseParameter(TestingT *t) {
    static const ParamCase tests[] = {
        {
            .name = "valid parameter",
            .in = S(";parameter;want=\"wantvalue\""),
            .want = WANT_STRING("wantvalue"),
            .want_ok = true,
        },
        {
            .name = "valid parameter with content after",
            .in = S(";want=:eWF5Cg==:;now;want=1;is;repeated;want=\"overwritten!\", "
                    "some=stuff"),
            .want = WANT_STRING("overwritten!"),
        },
        {
            .name = "invalid parameter",
            .in = S(";UPPERCASEKEY=NOT_ACCEPTED"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ParamValue got = {0};
        bool ok = burrow__httpsfv_parse_parameter(
            tests[i].in, BURROW_FN(HttpsfvParamFunc, keep_want_param, &got));
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!param_value_equal(got, tests[i].want)) {
            testing_t_fatalf_v(t, "test %q: mismatch", tests[i].name);
            return;
        }
    }
}

/* A case for a consume function that takes nothing but s. */
typedef struct ConsumeCase {
    const char *name;
    Str in;
    Str want;
    bool want_ok;
} ConsumeCase;

typedef bool (*ConsumeFn)(Str s, Str *consumed, Str *rest);

static void run_consume(TestingT *t, ConsumeFn fn, const ConsumeCase *tests, size_t n) {
    for (size_t i = 0; i < n; i++) {
        Str got = BURROW_STR_EMPTY;
        Str rest = BURROW_STR_EMPTY;
        bool ok = fn(tests[i].in, &got, &rest);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!str_eq(tests[i].want, got)) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got, tests[i].want);
            return;
        }
        if (!split_of(got, rest, tests[i].in)) {
            testing_t_fatalf_v(t, "test %q: %q + %q != %q", tests[i].name, got, rest,
                               tests[i].in);
            return;
        }
    }
}

#define RUN_CONSUME(t, fn, tests)                                                      \
    run_consume(t, fn, tests, sizeof(tests) / sizeof((tests)[0]))

static void TestConsumeKey(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid basic key",
            .in = S("fookey"),
            .want = S("fookey"),
            .want_ok = true,
        },
        {
            .name = "valid basic key with more content after",
            .in = S("fookey,u=7"),
            .want = S("fookey"),
            .want_ok = true,
        },
        {
            .name = "invalid key",
            .in = S("1keycannotstartwithnum"),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_key, tests);
}

static void TestConsumeIntegerOrDecimal(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid integer",
            .in = S("123456"),
            .want = S("123456"),
            .want_ok = true,
        },
        {
            .name = "valid integer with more content after",
            .in = S("123456,12345"),
            .want = S("123456"),
            .want_ok = true,
        },
        {
            .name = "valid max integer",
            .in = S("999999999999999"),
            .want = S("999999999999999"),
            .want_ok = true,
        },
        {
            .name = "valid min integer",
            .in = S("-999999999999999"),
            .want = S("-999999999999999"),
            .want_ok = true,
        },
        {
            .name = "invalid integer too high",
            .in = S("9999999999999999"),
        },
        {
            .name = "invalid integer too low",
            .in = S("-9999999999999999"),
        },
        {
            .name = "valid decimal",
            .in = S("-123456789012.123"),
            .want = S("-123456789012.123"),
            .want_ok = true,
        },
        {
            .name = "invalid decimal integer component too long",
            .in = S("1234567890123.1"),
        },
        {
            .name = "invalid decimal fraction component too long",
            .in = S("1.1234"),
        },
        {
            .name = "invalid decimal trailing dot",
            .in = S("1."),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_integer_or_decimal, tests);
}

static void TestParseInteger(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        int64_t want;
        bool want_ok;
    } tests[] = {
        {
            .name = "valid integer",
            .in = S("123456"),
            .want = 123456,
            .want_ok = true,
        },
        {
            .name = "valid integer with more content after",
            .in = S("123456,12345"),
        },
        {
            .name = "valid max integer",
            .in = S("999999999999999"),
            .want = 999999999999999,
            .want_ok = true,
        },
        {
            .name = "valid min integer",
            .in = S("-999999999999999"),
            .want = -999999999999999,
            .want_ok = true,
        },
        {
            .name = "invalid integer too high",
            .in = S("9999999999999999"),
        },
        {
            .name = "invalid integer too low",
            .in = S("-9999999999999999"),
        },
        {
            .name = "invalid integer with fraction",
            .in = S("-123456789012.123"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int64_t got = 0;
        bool ok = burrow__httpsfv_parse_integer(tests[i].in, &got);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (tests[i].want != got) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %d\nwant: %d\n",
                               tests[i].name, got, tests[i].want);
            return;
        }
    }
}

static void TestParseDecimal(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        double want;
        bool want_ok;
    } tests[] = {
        {
            .name = "valid decimal",
            .in = S("123456.789"),
            .want = 123456.789,
            .want_ok = true,
        },
        {
            .name = "valid decimal with more content after",
            .in = S("123456.789, 123"),
        },
        {
            .name = "invalid decimal with no fraction",
            .in = S("123456"),
        },
        {
            .name = "invalid decimal integer component too long",
            .in = S("1234567890123.1"),
        },
        {
            .name = "invalid decimal fraction component too long",
            .in = S("1.1234"),
        },
        {
            .name = "invalid decimal trailing dot",
            .in = S("1."),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        double got = 0;
        bool ok = burrow__httpsfv_parse_decimal(tests[i].in, &got);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (tests[i].want != got) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %v\nwant: %v\n",
                               tests[i].name, got, tests[i].want);
            return;
        }
    }
}

static void TestConsumeString(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid basic string",
            .in = S("\"foo bar\""),
            .want = S("\"foo bar\""),
            .want_ok = true,
        },
        {
            .name = "valid basic string with more content after",
            .in = S("\"foo bar\", a=3"),
            .want = S("\"foo bar\""),
            .want_ok = true,
        },
        {
            .name = "valid string with escaped dquote",
            .in = S("\"foo bar \\\"\""),
            .want = S("\"foo bar \\\"\""),
            .want_ok = true,
        },
        {
            .name = "invalid string no starting dquote",
            .in = S("foo bar\""),
        },
        {
            .name = "invalid string no closing dquote",
            .in = S("\"foo bar"),
        },
        {
            .name = "invalid string invalid character",
            .in = S("\"\0\""),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_string, tests);
}

typedef struct StrCase {
    const char *name;
    Str in;
    Str want;
    bool want_ok;
} StrCase;

typedef bool (*ParseStrFn)(Str s, Str *out);

static void run_parse_str(TestingT *t, ParseStrFn fn, const StrCase *tests, size_t n) {
    for (size_t i = 0; i < n; i++) {
        Str got = BURROW_STR_EMPTY;
        bool ok = fn(tests[i].in, &got);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (!str_eq(tests[i].want, got)) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n",
                               tests[i].name, got, tests[i].want);
            return;
        }
    }
}

#define RUN_PARSE_STR(t, fn, tests)                                                    \
    run_parse_str(t, fn, tests, sizeof(tests) / sizeof((tests)[0]))

static void TestParseString(TestingT *t) {
    static const StrCase tests[] = {
        {
            .name = "valid basic string",
            .in = S("\"foo bar\""),
            .want = S("foo bar"),
            .want_ok = true,
        },
        {
            .name = "valid basic string with more content after",
            .in = S("\"foo bar\", a=3"),
        },
        {
            .name = "valid string with escaped dquote",
            .in = S("\"foo bar \\\"\""),
            .want = S("foo bar \\\""),
            .want_ok = true,
        },
        {
            .name = "invalid string no starting dquote",
            .in = S("foo bar\""),
        },
        {
            .name = "invalid string no closing dquote",
            .in = S("\"foo bar"),
        },
        {
            .name = "invalid string invalid character",
            .in = S("\"\0\""),
        },
    };
    RUN_PARSE_STR(t, burrow__httpsfv_parse_string, tests);
}

static void TestConsumeToken(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid token",
            .in = S("*atoken"),
            .want = S("*atoken"),
            .want_ok = true,
        },
        {
            .name = "valid token with more content after",
            .in = S("*atoken something"),
            .want = S("*atoken"),
            .want_ok = true,
        },
        {
            .name = "invalid token",
            .in = S("0invalid"),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_token, tests);
}

static void TestParseToken(TestingT *t) {
    static const StrCase tests[] = {
        {
            .name = "valid token",
            .in = S("a_b-c.d3:f%00/*"),
            .want = S("a_b-c.d3:f%00/*"),
            .want_ok = true,
        },
        {
            .name = "valid token with uppercase",
            .in = S("FOOBAR"),
            .want = S("FOOBAR"),
            .want_ok = true,
        },
        {
            .name = "valid token with content after",
            .in = S("FOOBAR, foobar"),
        },
        {
            .name = "invalid token",
            .in = S("0invalid"),
        },
    };
    RUN_PARSE_STR(t, burrow__httpsfv_parse_token, tests);
}

static void TestConsumeByteSequence(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid byte sequence",
            .in = S(":aGVsbG8gd29ybGQ=:"),
            .want = S(":aGVsbG8gd29ybGQ=:"),
            .want_ok = true,
        },
        {
            .name = "valid byte sequence with more content after",
            .in = S(":aGVsbG8gd29ybGQ=::aGVsbG8gd29ybGQ=:"),
            .want = S(":aGVsbG8gd29ybGQ=:"),
            .want_ok = true,
        },
        {
            .name = "invalid byte sequence character",
            .in = S(":-:"),
        },
        {
            .name = "invalid byte sequence opening",
            .in = S("aGVsbG8gd29ybGQ=:"),
        },
        {
            .name = "invalid byte sequence closing",
            .in = S(":aGVsbG8gd29ybGQ="),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_byte_sequence, tests);
}

static void TestParseByteSequence(TestingT *t) {
    static const StrCase tests[] = {
        {
            .name = "valid byte sequence",
            .in = S(":aGVsbG8gd29ybGQ=:"),
            .want = S("aGVsbG8gd29ybGQ="),
            .want_ok = true,
        },
        {
            .name = "valid byte sequence with more content after",
            .in = S(":aGVsbG8gd29ybGQ=::aGVsbG8gd29ybGQ=:"),
        },
        {
            .name = "invalid byte sequence character",
            .in = S(":-:"),
        },
        {
            .name = "invalid byte sequence opening",
            .in = S("aGVsbG8gd29ybGQ=:"),
        },
        {
            .name = "invalid byte sequence closing",
            .in = S(":aGVsbG8gd29ybGQ="),
        },
    };
    RUN_PARSE_STR(t, burrow__httpsfv_parse_byte_sequence, tests);
}

static void TestConsumeBoolean(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid boolean",
            .in = S("?0"),
            .want = S("?0"),
            .want_ok = true,
        },
        {
            .name = "valid boolean with more content after",
            .in = S("?1, a=1"),
            .want = S("?1"),
            .want_ok = true,
        },
        {
            .name = "invalid boolean",
            .in = S("!2"),
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_boolean, tests);
}

static void TestParseBoolean(TestingT *t) {
    static const struct {
        const char *name;
        Str in;
        bool want;
        bool want_ok;
    } tests[] = {
        {
            .name = "valid boolean false",
            .in = S("?0"),
            .want = false,
            .want_ok = true,
        },
        {
            .name = "valid boolean true",
            .in = S("?1"),
            .want = true,
            .want_ok = true,
        },
        {
            .name = "valid boolean with more content after",
            .in = S("?1, a=1"),
        },
        {
            .name = "invalid boolean",
            .in = S("?2"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = false;
        bool ok = burrow__httpsfv_parse_boolean(tests[i].in, &got);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        if (tests[i].want != got) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %t\nwant: %t\n",
                               tests[i].name, got, tests[i].want);
            return;
        }
    }
}

static void TestConsumeDate(TestingT *t) {
    static const ConsumeCase tests[] = {
        {
            .name = "valid zero date",
            .in = S("@0"),
            .want = S("@0"),
            .want_ok = true,
        },
        {
            .name = "valid positive date",
            .in = S("@1659578233"),
            .want = S("@1659578233"),
            .want_ok = true,
        },
        {
            .name = "valid negative date",
            .in = S("@-1659578233"),
            .want = S("@-1659578233"),
            .want_ok = true,
        },
        {
            .name = "valid large date",
            .in = S("@25340221440"),
            .want = S("@25340221440"),
            .want_ok = true,
        },
        {
            .name = "valid small date",
            .in = S("@-62135596800"),
            .want = S("@-62135596800"),
            .want_ok = true,
        },
        {
            .name = "invalid decimal date",
            .in = S("@1.2"),
        },
        {
            .name = "valid date with more content after",
            .in = S("@1659578233, foo;bar"),
            .want = S("@1659578233"),
            .want_ok = true,
        },
    };
    RUN_CONSUME(t, burrow__httpsfv_consume_date, tests);
}

static void TestParseDate(TestingT *t) {
    const struct {
        const char *name;
        Str in;
        Time want;
        bool want_ok;
    } tests[] = {
        {
            .name = "valid zero date",
            .in = S("@0"),
            .want = time_from_unix(0, 0),
            .want_ok = true,
        },
        {
            .name = "valid positive date",
            .in = S("@1659578233"),
            .want =
                time_local(time_date(2022, TIME_AUGUST, 4, 1, 57, 13, 0, time_utc_loc)),
            .want_ok = true,
        },
        {
            .name = "valid negative date",
            .in = S("@-1659578233"),
            .want =
                time_local(time_date(1917, TIME_MAY, 30, 22, 2, 47, 0, time_utc_loc)),
            .want_ok = true,
        },
        {
            .name = "valid max date required",
            .in = S("@253402214400"),
            .want = time_local(
                time_date(9999, TIME_DECEMBER, 31, 0, 0, 0, 0, time_utc_loc)),
            .want_ok = true,
        },
        {
            .name = "valid min date required",
            .in = S("@-62135596800"),
            .want = time_local(time_date(1, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc)),
            .want_ok = true,
        },
        {
            .name = "invalid date with fraction",
            .in = S("@0.123"),
        },
        {
            .name = "valid date with more content after",
            .in = S("@0, @0"),
        },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Time got = {0};
        bool ok = burrow__httpsfv_parse_date(tests[i].in, &got);
        if (ok != tests[i].want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tests[i].name,
                               tests[i].want_ok, ok);
            return;
        }
        /* Go compares with ==, which takes in the location as well. A zero Time
         * is in UTC. */
        bool same = time_equal(got, tests[i].want) &&
                    time_location(got) == time_location(tests[i].want);
        if (!same) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %v\nwant: %v\n",
                               tests[i].name, time_unix(got), time_unix(tests[i].want));
            return;
        }
    }
}

static const ConsumeCase display_consume_tests[] = {
    {
        .name = "valid ascii string",
        .in = S(
            "%\" !%22#$%25&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
            "abcdefghijklmnopqrstuvwxyz{|}~\""),
        .want = S(
            "%\" !%22#$%25&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
            "abcdefghijklmnopqrstuvwxyz{|}~\""),
        .want_ok = true,
    },
    {
        .name = "valid lowercase non-ascii string",
        .in = S("%\"f%c3%bc%c3%bc\""),
        .want = S("%\"f%c3%bc%c3%bc\""),
        .want_ok = true,
    },
    {
        .name = "invalid uppercase non-ascii string",
        .in = S("%\"f%C3%BC%C3%BC\""),
    },
    {
        .name = "invalid unquoted string",
        .in = S("%foo"),
    },
    {
        .name = "invalid string missing initial quote",
        .in = S("%foo\""),
    },
    {
        .name = "invalid string missing closing quote",
        .in = S("%\"foo"),
    },
    {
        .name = "invalid tab in string",
        .in = S("%\"\t\""),
    },
    {
        .name = "invalid newline in string",
        .in = S("%\"\n\""),
    },
    {
        .name = "invalid single quoted string",
        .in = S("%'foo'"),
    },
    {
        .name = "invalid string bad escaping",
        .in = S("%\\\"foo %a\""),
    },
    {
        .name = "valid string with escaped quotes",
        .in = S("%\"foo %22bar%22 \\\\ baz\""),
        .want = S("%\"foo %22bar%22 \\\\ baz\""),
        .want_ok = true,
    },
    {
        .name = "invalid sequence id utf-8 string",
        .in = S("%\"%a0%a1\""),
    },
    {
        .name = "invalid 2 bytes sequence utf-8 string",
        .in = S("%\"%c3%28\""),
    },
    {
        .name = "invalid 3 bytes sequence utf-8 string",
        .in = S("%\"%e2%28%a1\""),
    },
    {
        .name = "invalid 4 bytes sequence utf-8 string",
        .in = S("%\"%f0%28%8c%28\""),
    },
    {
        .name = "invalid hex utf-8 string",
        .in = S("%\"%g0%1w\""),
    },
    {
        .name = "valid byte order mark in display string",
        .in = S("%\"BOM: %ef%bb%bf\""),
        .want = S("%\"BOM: %ef%bb%bf\""),
        .want_ok = true,
    },
    {
        .name = "valid string with content after",
        .in = S("%\"foo\\nbar\", foo;bar"),
        .want = S("%\"foo\\nbar\""),
        .want_ok = true,
    },
    {
        .name = "invalid unfinished 4 bytes rune",
        .in = S("%\"%f0%9f%98\""),
    },
};

static void TestConsumeDisplayString(TestingT *t) {
    RUN_CONSUME(t, burrow__httpsfv_consume_display_string, display_consume_tests);
}

static const StrCase display_parse_tests[] = {
    {
        .name = "valid ascii string",
        .in = S(
            "%\" !%22#$%25&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
            "abcdefghijklmnopqrstuvwxyz{|}~\""),
        .want = S(" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                  "abcdefghijklmnopqrstuvwxyz{|}~"),
        .want_ok = true,
    },
    {
        .name = "valid lowercase non-ascii string",
        .in = S("%\"f%c3%bc%c3%bc\""),
        .want = S("f\xc3\xbc\xc3\xbc"),
        .want_ok = true,
    },
    {
        .name = "invalid uppercase non-ascii string",
        .in = S("%\"f%C3%BC%C3%BC\""),
    },
    {
        .name = "invalid unquoted string",
        .in = S("%foo"),
    },
    {
        .name = "invalid string missing initial quote",
        .in = S("%foo\""),
    },
    {
        .name = "invalid string missing closing quote",
        .in = S("%\"foo"),
    },
    {
        .name = "invalid tab in string",
        .in = S("%\"\t\""),
    },
    {
        .name = "invalid newline in string",
        .in = S("%\"\n\""),
    },
    {
        .name = "invalid single quoted string",
        .in = S("%'foo'"),
    },
    {
        .name = "invalid string bad escaping",
        .in = S("%\\\"foo %a\""),
    },
    {
        .name = "valid string with escaped quotes",
        .in = S("%\"foo %22bar%22 \\ baz\""),
        .want = S("foo \"bar\" \\ baz"),
        .want_ok = true,
    },
    {
        .name = "invalid sequence id utf-8 string",
        .in = S("%\"%a0%a1\""),
    },
    {
        .name = "invalid 2 bytes sequence utf-8 string",
        .in = S("%\"%c3%28\""),
    },
    {
        .name = "invalid 3 bytes sequence utf-8 string",
        .in = S("%\"%e2%28%a1\""),
    },
    {
        .name = "invalid 4 bytes sequence utf-8 string",
        .in = S("%\"%f0%28%8c%28\""),
    },
    {
        .name = "invalid hex utf-8 string",
        .in = S("%\"%g0%1w\""),
    },
    {
        .name = "valid byte order mark in display string",
        .in = S("%\"BOM: %ef%bb%bf\""),
        .want = S("BOM: \xef\xbb\xbf"),
        .want_ok = true,
    },
    {
        .name = "valid string with content after",
        .in = S("%\"foo\\nbar\", foo;bar"),
    },
    {
        .name = "invalid unfinished 4 bytes rune",
        .in = S("%\"%f0%9f%98\""),
    },
};

static void TestParseDisplayString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof display_parse_tests / sizeof display_parse_tests[0];
         i++) {
        const StrCase *tc = &display_parse_tests[i];
        Str got = BURROW_STR_EMPTY;
        Error err = BURROW_NO_ERROR;
        bool ok = burrow__httpsfv_parse_display_string(a, tc->in, &got, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "test %q: %s", tc->name, error_text(err));
            break;
        }
        if (ok != tc->want_ok) {
            testing_t_fatalf_v(t, "test %q: want ok to be %t, got: %t", tc->name,
                               tc->want_ok, ok);
            break;
        }
        if (!str_eq(tc->want, got)) {
            testing_t_fatalf_v(t, "test %q: mismatch.\n got: %q\nwant: %q\n", tc->name,
                               got, tc->want);
            break;
        }
    }
    arena_free(&ar);
}

/* An allocator that says no, to see the out of memory path. */
static void *refuse_alloc(void *self, size_t size, size_t align) {
    (void)self;
    (void)size;
    (void)align;
    return NULL;
}

static void *refuse_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    (void)self;
    (void)p;
    (void)old;
    (void)nsz;
    (void)align;
    return NULL;
}

static void refuse_free(void *self, void *p, size_t size, size_t align) {
    (void)self;
    (void)p;
    (void)size;
    (void)align;
}

static const AllocVT refuse_vt = {refuse_alloc, NULL, refuse_realloc,
                                  refuse_free,  NULL, NULL};

static void TestParseDisplayStringOutOfMemory(TestingT *t) {
    Alloc none = {.vt = &refuse_vt};
    Str got = BURROW_S("unchanged");
    Error err = BURROW_NO_ERROR;
    bool ok = burrow__httpsfv_parse_display_string(&none, BURROW_S("%\"f%c3%bc\""),
                                                   &got, &err);
    if (ok || !errors_is(err, burrow_err_out_of_memory) || got.len != 0)
        testing_t_errorf_v(t, "with no memory: ok %t, got %q, error %s", ok, got,
                           error_text(err));

    /* An empty display string needs no memory. */
    err = BURROW_NO_ERROR;
    ok = burrow__httpsfv_parse_display_string(&none, BURROW_S("%\"\""), &got, &err);
    if (!ok || BURROW_FAILED(err) || got.len != 0)
        testing_t_errorf_v(t, "empty with no memory: ok %t, got %q, error %s", ok, got,
                           error_text(err));
}

#define TESTS(X)                                                                       \
    X(TestParseList)                                                                   \
    X(TestConsumeBareInnerList)                                                        \
    X(TestParseBareInnerList)                                                          \
    X(TestConsumeItem)                                                                 \
    X(TestParseItem)                                                                   \
    X(TestParseDictionary)                                                             \
    X(TestConsumeParameter)                                                            \
    X(TestParseParameter)                                                              \
    X(TestConsumeKey)                                                                  \
    X(TestConsumeIntegerOrDecimal)                                                     \
    X(TestParseInteger)                                                                \
    X(TestParseDecimal)                                                                \
    X(TestConsumeString)                                                               \
    X(TestParseString)                                                                 \
    X(TestConsumeToken)                                                                \
    X(TestParseToken)                                                                  \
    X(TestConsumeByteSequence)                                                         \
    X(TestParseByteSequence)                                                           \
    X(TestConsumeBoolean)                                                              \
    X(TestParseBoolean)                                                                \
    X(TestConsumeDate)                                                                 \
    X(TestParseDate)                                                                   \
    X(TestConsumeDisplayString)                                                        \
    X(TestParseDisplayString)                                                          \
    X(TestParseDisplayStringOutOfMemory)

TESTING_MAIN(TESTS)
