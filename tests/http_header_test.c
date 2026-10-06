/* Derived from Go's src/net/http/header_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests build a Header as a map literal, which can hold keys that are not
 * canonical, nil values and empty ones. These build the same headers with
 * map_set. TestHeaderWriteSubsetAllocs counts allocations with a Track where
 * Go uses testing.AllocsPerRun. TestHeaderWriteManyKeys is burrow's own, for
 * the headers with too many keys to sort on the stack.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/track.h"
#include "burrow/net/http.h"
#include "burrow/panic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

/* A key and its values as a Go map literal has them. n is -1 for a nil
 * value. */
typedef struct HtKV {
    const char *key;
    const char *v[3];
    int n;
} HtKV;

/* Sets key to the n values in v as they are, with no canonical form. */
static void ht_put(HttpHeader h, Alloc *a, const HtKV *kv) {
    Slice s = slice_from(NULL, 0, 0, TYPE_STRING);
    if (kv->n >= 0) {
        Str *vs = (Str *)mem_alloc(a, 3 * sizeof(Str), _Alignof(Str));
        if (vs == NULL)
            panic_str(S("out of memory"));
        for (int i = 0; i < kv->n; i++)
            vs[i] = str_from_cstr(kv->v[i]);
        s = slice_from(vs, kv->n, kv->n, TYPE_STRING);
    }
    Str key = str_from_cstr(kv->key);
    if (!map_set(h, &key, &s))
        panic_str(S("out of memory"));
}

static HttpHeader ht_header(Alloc *a, const HtKV *kvs) {
    HttpHeader h = http_header_make(a);
    if (h == NULL)
        panic_str(S("out of memory"));
    for (const HtKV *kv = kvs; kv->key != NULL; kv++)
        ht_put(h, a, kv);
    return h;
}

static Str ht_buffer_str(BytesBuffer *b) {
    Slice s = bytes_buffer_bytes(b);
    return str_from_bytes((const Byte *)s.p, s.len);
}

static void TestHeaderWrite(TestingT *t) {
    static const struct {
        HtKV h[10];
        const char *exclude[4];
        const char *expected;
    } tests[] = {
        {{{NULL, {NULL}, 0}}, {NULL}, ""},
        {{{"Content-Type", {"text/html; charset=UTF-8"}, 1},
          {"Content-Length", {"0"}, 1},
          {NULL, {NULL}, 0}},
         {NULL},
         "Content-Length: 0\r\nContent-Type: text/html; charset=UTF-8\r\n"},
        {{{"Content-Length", {"0", "1", "2"}, 3}, {NULL, {NULL}, 0}},
         {NULL},
         "Content-Length: 0\r\nContent-Length: 1\r\nContent-Length: 2\r\n"},
        {{{"Expires", {"-1"}, 1},
          {"Content-Length", {"0"}, 1},
          {"Content-Encoding", {"gzip"}, 1},
          {NULL, {NULL}, 0}},
         {"Content-Length", NULL},
         "Content-Encoding: gzip\r\nExpires: -1\r\n"},
        {{{"Expires", {"-1"}, 1},
          {"Content-Length", {"0", "1", "2"}, 3},
          {"Content-Encoding", {"gzip"}, 1},
          {NULL, {NULL}, 0}},
         {"Content-Length", NULL},
         "Content-Encoding: gzip\r\nExpires: -1\r\n"},
        {{{"Expires", {"-1"}, 1},
          {"Content-Length", {"0"}, 1},
          {"Content-Encoding", {"gzip"}, 1},
          {NULL, {NULL}, 0}},
         {"Content-Length", "Expires", "Content-Encoding", NULL},
         ""},
        {{{"Nil", {NULL}, -1},
          {"Empty", {NULL}, 0},
          {"Blank", {""}, 1},
          {"Double-Blank", {"", ""}, 2},
          {NULL, {NULL}, 0}},
         {NULL},
         "Blank: \r\nDouble-Blank: \r\nDouble-Blank: \r\n"},
        /* Go's test of sorting past its insertion sort threshold. */
        {{{"k1", {"1a", "1b"}, 2},
          {"k2", {"2a", "2b"}, 2},
          {"k3", {"3a", "3b"}, 2},
          {"k4", {"4a", "4b"}, 2},
          {"k5", {"5a", "5b"}, 2},
          {"k6", {"6a", "6b"}, 2},
          {"k7", {"7a", "7b"}, 2},
          {"k8", {"8a", "8b"}, 2},
          {"k9", {"9a", "9b"}, 2},
          {NULL, {NULL}, 0}},
         {"k5", NULL},
         "k1: 1a\r\nk1: 1b\r\nk2: 2a\r\nk2: 2b\r\nk3: 3a\r\nk3: 3b\r\n"
         "k4: 4a\r\nk4: 4b\r\nk6: 6a\r\nk6: 6b\r\n"
         "k7: 7a\r\nk7: 7b\r\nk8: 8a\r\nk8: 8b\r\nk9: 9a\r\nk9: 9b\r\n"},
        /* Invalid characters in headers. */
        {{{"Content-Type", {"text/html; charset=UTF-8"}, 1},
          {"NewlineInValue", {"1\r\nBar: 2"}, 1},
          {"NewlineInKey\r\n", {"1"}, 1},
          {"Colon:InKey", {"1"}, 1},
          {"Evil: 1\r\nSmuggledValue", {"1"}, 1},
          {NULL, {NULL}, 0}},
         {NULL},
         "Content-Type: text/html; charset=UTF-8\r\n"
         "NewlineInValue: 1  Bar: 2\r\n"},
    };
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpHeader h = ht_header(a, tests[i].h);
        Map *exclude = NULL;
        if (tests[i].exclude[0] != NULL) {
            exclude = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
            CHECK(exclude != NULL);
            for (const char *const *e = tests[i].exclude; *e != NULL; e++)
                CHECK(BURROW_MAP_SET(Str, bool, exclude, str_from_cstr(*e), true));
        }
        Error err =
            http_header_write_subset(h, bytes_buffer_as_io_writer(&buf), exclude);
        Str got = ht_buffer_str(&buf);
        if (BURROW_FAILED(err) || !str_eq(got, str_from_cstr(tests[i].expected)))
            testing_t_errorf_v(t, "#%d:\n got: %q\nwant: %q", (Int)i, got,
                               tests[i].expected);
        bytes_buffer_reset(&buf);
    }
    ARENA_END;
}

static void TestParseTime(TestingT *t) {
    static const struct {
        const char *date;
        bool err;
    } tests[] = {
        {"", true},
        {"invalid", true},
        {"1994-11-06T08:49:37Z00:00", true},
        {"Sun, 06 Nov 1994 08:49:37 GMT", false},
        {"Sunday, 06-Nov-94 08:49:37 GMT", false},
        {"Sun Nov  6 08:49:37 1994", false},
    };
    ARENA_BEGIN;
    Time expect = time_date(1994, TIME_NOVEMBER, 6, 8, 49, 37, 0, time_utc_loc);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpHeader h = http_header_make(a);
        CHECK(h != NULL);
        CHECK(http_header_set(h, S("Date"), str_from_cstr(tests[i].date)));
        Error err = BURROW_NO_ERROR;
        Time d = http_parse_time(a, http_header_get(h, S("Date")), &err);
        if (BURROW_FAILED(err)) {
            if (!tests[i].err)
                testing_t_errorf_v(t, "#%d:\n got err: %s", (Int)i, error_text(err));
            continue;
        }
        if (tests[i].err) {
            testing_t_errorf_v(t, "#%d:\n  should err", (Int)i);
            continue;
        }
        if (!time_equal(expect, d))
            testing_t_errorf_v(t, "#%d:\n got: %s\nwant: %s", (Int)i,
                               time_format(d, a, TIME_RFC3339),
                               time_format(expect, a, TIME_RFC3339));
    }
    ARENA_END;
}

static void TestHasToken(TestingT *t) {
    static const struct {
        const char *header;
        const char *token;
        bool want;
    } tests[] = {
        {"", "", false},
        {"", "foo", false},
        {"foo", "foo", true},
        {"foo ", "foo", true},
        {" foo", "foo", true},
        {" foo ", "foo", true},
        {"foo,bar", "foo", true},
        {"bar,foo", "foo", true},
        {"bar, foo", "foo", true},
        {"bar,foo, baz", "foo", true},
        {"bar, foo,baz", "foo", true},
        {"bar,foo, baz", "foo", true},
        {"bar, foo, baz", "foo", true},
        {"FOO", "foo", true},
        {"FOO ", "foo", true},
        {" FOO", "foo", true},
        {" FOO ", "foo", true},
        {"FOO,BAR", "foo", true},
        {"BAR,FOO", "foo", true},
        {"BAR, FOO", "foo", true},
        {"BAR,FOO, baz", "foo", true},
        {"BAR, FOO,BAZ", "foo", true},
        {"BAR,FOO, BAZ", "foo", true},
        {"BAR, FOO, BAZ", "foo", true},
        {"foobar", "foo", false},
        {"barfoo ", "foo", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = burrow__http_has_token(str_from_cstr(tests[i].header),
                                          str_from_cstr(tests[i].token));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "hasToken(%q, %q) = %t; want %t", tests[i].header,
                               tests[i].token, got, tests[i].want);
    }
}

static void TestNilHeaderClone(TestingT *t) {
    ARENA_BEGIN;
    HttpHeader t2 = http_header_clone(a, NULL);
    if (t2 != NULL)
        testing_t_errorf_v(t, "cloned header does not match original: got a header; "
                              "want nil");
    ARENA_END;
}

static void TestHeaderWriteSubsetAllocs(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping alloc test in short mode");
    }
    static const HtKV test_header[] = {
        {"Content-Length", {"123"}, 1},
        {"Content-Type", {"text/plain"}, 1},
        {"Date", {"some date at some time Z"}, 1},
        {"Server", {"Go-http-client/1.1"}, 1},
        {NULL, {NULL}, 0},
    };
    /* The header and the buffer both come from the Track, so it sees an
     * allocation from either. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Track tr;
    track_init(&tr, arena_allocator(&ar));
    Alloc *a = track_allocator(&tr);
    HttpHeader h = ht_header(a, test_header);
    BytesBuffer buf = BYTES_BUFFER(a);
    /* The buffer grows on the first write and keeps its room after a reset,
     * as Go's package level buffer does between runs. */
    CHECK(
        BURROW_OK(http_header_write_subset(h, bytes_buffer_as_io_writer(&buf), NULL)));
    uint64_t before = tr.allocs;
    for (int i = 0; i < 100; i++) {
        bytes_buffer_reset(&buf);
        (void)http_header_write_subset(h, bytes_buffer_as_io_writer(&buf), NULL);
    }
    if (tr.allocs != before)
        testing_t_errorf_v(t, "allocs = %d; want 0", (Int)(tr.allocs - before));
    track_free(&tr);
    arena_free(&ar);
}

static void TestHeaderWriteManyKeys(TestingT *t) {
    ARENA_BEGIN;
    HttpHeader h = http_header_make(a);
    CHECK(h != NULL);
    BytesBuffer want = BYTES_BUFFER(a);
    /* Set in reverse so the order out is the sort's doing. */
    for (int i = 99; i >= 0; i--) {
        char key[4] = {'k', (char)('0' + i / 10), (char)('0' + i % 10), 0};
        HtKV kv = {key, {"v"}, 1};
        ht_put(h, a, &kv);
    }
    for (int i = 0; i < 100; i++) {
        char line[9] = {
            'k', (char)('0' + i / 10), (char)('0' + i % 10), ':', ' ', 'v', '\r', '\n',
            0};
        CHECK(bytes_buffer_write_string(&want, str_from_cstr(line), NULL) == 8);
    }
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = http_header_write(h, bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err) || !str_eq(ht_buffer_str(&buf), ht_buffer_str(&want)))
        testing_t_errorf_v(t, "Write of 100 keys:\n got: %q\nwant: %q",
                           ht_buffer_str(&buf), ht_buffer_str(&want));
    ARENA_END;
}

/* Go compares with reflect.DeepEqual, which tells a nil value from an empty
 * one, so this does too. */
static bool ht_deep_equal(HttpHeader x, HttpHeader y) {
    if (map_len(x) != map_len(y))
        return false;
    MapIter it = map_iter(x);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        const Slice *xv = (const Slice *)v;
        const Slice *yv = (const Slice *)map_get(y, k);
        if (yv == NULL || xv->len != yv->len || (xv->p == NULL) != (yv->p == NULL))
            return false;
        const Str *xs = (const Str *)xv->p;
        const Str *ys = (const Str *)yv->p;
        for (Int i = 0; xs != NULL && ys != NULL && i < xv->len; i++) {
            if (!str_eq(xs[i], ys[i]))
                return false;
        }
    }
    return true;
}

static void TestCloneOrMakeHeader(TestingT *t) {
    static const struct {
        const char *name;
        bool nil_in;
        HtKV in[2];
        HtKV want[2];
    } tests[] = {
        {"nil", true, {{NULL, {NULL}, 0}}, {{NULL, {NULL}, 0}}},
        {"empty", false, {{NULL, {NULL}, 0}}, {{NULL, {NULL}, 0}}},
        {"non-empty",
         false,
         {{"foo", {"bar"}, 1}, {NULL, {NULL}, 0}},
         {{"foo", {"bar"}, 1}, {NULL, {NULL}, 0}}},
        {"nil value",
         false,
         {{"foo", {NULL}, -1}, {NULL, {NULL}, 0}},
         {{"foo", {NULL}, -1}, {NULL, {NULL}, 0}}},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpHeader in = tests[i].nil_in ? NULL : ht_header(a, tests[i].in);
        HttpHeader want = ht_header(a, tests[i].want);
        HttpHeader got = burrow__http_clone_or_make_header(a, in);
        if (got == NULL) {
            testing_t_errorf_v(t, "%s: unexpected nil Header", tests[i].name);
            continue;
        }
        if (!ht_deep_equal(got, want)) {
            testing_t_errorf_v(t, "%s: the clone is not the header", tests[i].name);
            continue;
        }
        CHECK(http_header_add(got, S("A"), S("B")));
        CHECK(str_eq(http_header_get(got, S("A")), S("B")));
    }
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestHeaderWrite)                                                                 \
    X(TestParseTime)                                                                   \
    X(TestHasToken)                                                                    \
    X(TestNilHeaderClone)                                                              \
    X(TestHeaderWriteSubsetAllocs)                                                     \
    X(TestHeaderWriteManyKeys)                                                         \
    X(TestCloneOrMakeHeader)

TESTING_MAIN(TESTS)
