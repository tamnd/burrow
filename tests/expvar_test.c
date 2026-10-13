/* Tests for expvar.
 *
 * Go's expvar_test.go, test for test and in the same order, then what C adds:
 * the two variables Go's init publishes, the panic on a name published twice,
 * the handler on the default mux, a map that is freed, and maps under
 * goroutines, which Go only runs as benchmarks. Where Go's test unmarshals a
 * map's JSON to check it, the keys come out sorted, so these compare the whole
 * text as well.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/expvar.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include "../src/expvar/expvar_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK_S(got, want)                                                             \
    do {                                                                               \
        Str g_ = (got), w_ = BURROW_S(want);                                           \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "%s = %q, want %q", #got, g_, w_);                   \
    } while (0)

static void TestPublishedByDefault(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__expvar_publish_defaults();

    ExpvarVar cmdline = expvar_get(BURROW_S("cmdline"));
    CHECK(cmdline.vt != NULL);
    if (cmdline.vt != NULL) {
        Str s = expvar_var_string(cmdline, a);
        CHECK(json_valid(slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE)));
        CHECK(strings_has_prefix(s, BURROW_S("[\"")));
    }

    ExpvarVar memstats = expvar_get(BURROW_S("memstats"));
    CHECK(memstats.vt != NULL);
    if (memstats.vt != NULL) {
        Str s = expvar_var_string(memstats, a);
        CHECK(json_valid(slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE)));
        CHECK(strings_has_prefix(s, BURROW_S("{\"Alloc\":")));
        CHECK(strings_contains(s, BURROW_S("\"Mallocs\":")));
        CHECK(strings_contains(s, BURROW_S("\"BySize\":[{\"Size\":0,")));
    }
    arena_free(&ar);
}

static void TestNil(TestingT *t) {
    burrow__expvar_remove_all();
    ExpvarVar val = expvar_get(BURROW_S("missing"));
    if (val.vt != NULL)
        testing_t_errorf_v(t, "got a Var, want nil");
}

static void TestInt(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarInt *reqs = expvar_new_int(BURROW_S("requests"));
    CHECK_INT_EQ(expvar_int_value(reqs), 0);
    ExpvarVar got = expvar_get(BURROW_S("requests"));
    if (got.vt == NULL || got.vt->self_type != TYPE_EXPVAR_INT || got.data != reqs)
        testing_t_errorf_v(t, "Get() failed.");

    expvar_int_add(reqs, 1);
    expvar_int_add(reqs, 3);
    CHECK_INT_EQ(expvar_int_value(reqs), 4);
    CHECK_S(expvar_int_string(reqs, a), "4");

    expvar_int_set(reqs, -2);
    CHECK_INT_EQ(expvar_int_value(reqs), -2);
    arena_free(&ar);
}

static void TestFloat(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarFloat *reqs = expvar_new_float(BURROW_S("requests-float"));
    CHECK(expvar_float_value(reqs) == 0.0);
    ExpvarVar got = expvar_get(BURROW_S("requests-float"));
    if (got.vt == NULL || got.vt->self_type != TYPE_EXPVAR_FLOAT || got.data != reqs)
        testing_t_errorf_v(t, "Get() failed.");

    expvar_float_add(reqs, 1.5);
    expvar_float_add(reqs, 1.25);
    CHECK(expvar_float_value(reqs) == 2.75);
    CHECK_S(expvar_float_string(reqs, a), "2.75");

    expvar_float_add(reqs, -2);
    CHECK(expvar_float_value(reqs) == 0.75);
    arena_free(&ar);
}

static void TestString(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarString *name = expvar_new_string(BURROW_S("my-name"));
    CHECK_S(expvar_string_value(name, a), "");

    CHECK(expvar_string_set(name, BURROW_S("Mike")));
    CHECK_S(expvar_string_string(name, a), "\"Mike\"");
    CHECK_S(expvar_string_value(name, a), "Mike");

    /* Make sure we produce safe JSON output. */
    CHECK(expvar_string_set(name, BURROW_S("<")));
    CHECK_S(expvar_string_string(name, a), "\"\\u003c\"");
    arena_free(&ar);
}

static void count_kv(void *env, ExpvarKeyValue kv) {
    (void)kv;
    (*(int *)env)++;
}

static int count_entries(ExpvarMap *m) {
    int n = 0;
    expvar_map_do(m, BURROW_FN(ExpvarDoFunc, count_kv, &n));
    return n;
}

static void TestMapInit(TestingT *t) {
    burrow__expvar_remove_all();
    ExpvarMap *colors = expvar_new_map(BURROW_S("bike-shed-colors"));
    expvar_map_add(colors, BURROW_S("red"), 1);
    expvar_map_add(colors, BURROW_S("blue"), 1);
    expvar_map_add(colors, BURROW_S("chartreuse"), 1);

    int n = count_entries(colors);
    if (n != 3)
        testing_t_errorf_v(t,
                           "after three Add calls with distinct keys, Do should invoke "
                           "f 3 times; got %d",
                           n);

    expvar_map_init(colors);

    n = count_entries(colors);
    if (n != 0)
        testing_t_errorf_v(t, "after Init, Do should invoke f 0 times; got %d", n);
}

static void TestMapDelete(TestingT *t) {
    burrow__expvar_remove_all();
    ExpvarMap *colors = expvar_new_map(BURROW_S("bike-shed-colors"));

    expvar_map_add(colors, BURROW_S("red"), 1);
    expvar_map_add(colors, BURROW_S("red"), 2);
    expvar_map_add(colors, BURROW_S("blue"), 4);

    int n = count_entries(colors);
    if (n != 2)
        testing_t_errorf_v(t,
                           "after two Add calls with distinct keys, Do should invoke f "
                           "2 times; got %d",
                           n);

    expvar_map_delete(colors, BURROW_S("red"));
    if (expvar_map_get(colors, BURROW_S("red")).vt != NULL)
        testing_t_errorf_v(t, "removed red, Get should return nil");
    n = count_entries(colors);
    if (n != 1)
        testing_t_errorf_v(t, "removed red, Do should invoke f 1 times; got %d", n);

    expvar_map_delete(colors, BURROW_S("notfound"));
    n = count_entries(colors);
    if (n != 1)
        testing_t_errorf_v(t,
                           "attempted to remove notfound, Do should invoke f 1 times; "
                           "got %d",
                           n);

    expvar_map_delete(colors, BURROW_S("blue"));
    expvar_map_delete(colors, BURROW_S("blue"));
    if (expvar_map_get(colors, BURROW_S("blue")).vt != NULL)
        testing_t_errorf_v(t, "removed blue, Get should return nil");
    n = count_entries(colors);
    if (n != 0)
        testing_t_errorf_v(t, "all keys removed, Do should invoke f 0 times; got %d",
                           n);
}

static bool valid_json(Str s) {
    return json_valid(slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE));
}

static void TestMapCounter(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ExpvarMap *colors = expvar_new_map(BURROW_S("bike-shed-colors"));

    expvar_map_add(colors, BURROW_S("red"), 1);
    expvar_map_add(colors, BURROW_S("red"), 2);
    expvar_map_add(colors, BURROW_S("blue"), 4);
    expvar_map_add_float(colors, BURROW_S("green \"midori\""), 4.125);

    ExpvarVar red = expvar_map_get(colors, BURROW_S("red"));
    ExpvarVar blue = expvar_map_get(colors, BURROW_S("blue"));
    ExpvarVar green = expvar_map_get(colors, BURROW_S("green \"midori\""));
    if (red.vt == NULL || red.vt->self_type != TYPE_EXPVAR_INT)
        testing_t_fatalf_v(t, "colors.m[\"red\"] is not an Int");
    if (blue.vt == NULL || blue.vt->self_type != TYPE_EXPVAR_INT)
        testing_t_fatalf_v(t, "colors.m[\"blue\"] is not an Int");
    if (green.vt == NULL || green.vt->self_type != TYPE_EXPVAR_FLOAT)
        testing_t_fatalf_v(t, "colors.m[`green \"midori\"] is not a Float");
    CHECK_INT_EQ(expvar_int_value(red.data), 3);
    CHECK_INT_EQ(expvar_int_value(blue.data), 4);
    CHECK(expvar_float_value(green.data) == 4.125);

    Str s = expvar_map_string(colors, a);
    if (!valid_json(s))
        testing_t_errorf_v(t, "colors.String() isn't valid JSON: %q", s);
    CHECK_S(s, "{\"blue\": 4, \"green \\\"midori\\\"\": 4.125, \"red\": 3}");

    /* A key that holds something other than an Int is left alone. */
    expvar_map_add(colors, BURROW_S("green \"midori\""), 1);
    CHECK(expvar_float_value(green.data) == 4.125);
    expvar_map_add_float(colors, BURROW_S("red"), 1);
    CHECK_INT_EQ(expvar_int_value(red.data), 3);
    arena_free(&ar);
}

static void TestMapNil(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarMap *m = expvar_new_map(BURROW_S("issue527719"));
    CHECK(expvar_map_set(m, BURROW_S("key"), (ExpvarVar){NULL, NULL}));
    Str s = expvar_map_string(m, a);
    if (!valid_json(s))
        testing_t_fatalf_v(t, "m.String() == %q isn't valid JSON", s);
    CHECK_S(s, "{\"key\": null}");
    arena_free(&ar);
}

BURROW_SLICE_TYPE(ExpvarTestStrs, Str);

static Any func_x;

static Any return_x(void *env, Alloc *a) {
    (void)env;
    (void)a;
    return func_x;
}

static void TestFunc(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str ab[] = {BURROW_S("a"), BURROW_S("b")};
    Slice strs = slice_from(ab, 2, 2, TYPE_STRING);
    func_x = (Any){TYPE_OF(ExpvarTestStrs), &strs};
    ExpvarFunc f = BURROW_FN(ExpvarFunc, return_x, NULL);
    CHECK_S(expvar_func_string(f, a), "[\"a\",\"b\"]");
    Any v = expvar_func_value(f, a);
    CHECK(v.t == func_x.t && v.data == func_x.data);

    Int seventeen = 17;
    func_x = (Any){TYPE_OF(Int), &seventeen};
    CHECK_S(expvar_func_string(f, a), "17");
    arena_free(&ar);
}

static Str serve(Alloc *a, HttpHandler h, Str method, Str target, Int *code,
                 Str *content_type) {
    HttpRequest *r = httptest_new_request(a, method, target, (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec), r);
    *code = rec->code;
    *content_type =
        str_clone(a, http_header_get(rec->header_map, BURROW_S("Content-Type")));
    Slice body = bytes_buffer_bytes(rec->body);
    Str s = str_clone(a, str_from_bytes(body.p, body.len));
    httptest_response_recorder_free(rec);
    http_request_free(r);
    return s;
}

static void TestHandler(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarMap *m = expvar_new_map(BURROW_S("map1"));
    expvar_map_add(m, BURROW_S("a"), 1);
    expvar_map_add(m, BURROW_S("z"), 2);
    ExpvarMap *m2 = expvar_new_map(BURROW_S("map2"));
    for (int i = 0; i < 9; i++) {
        char k[4];
        snprintf(k, sizeof k, "%d", i);
        expvar_map_add(m2, str_from_cstr(k), i);
    }

    Int code = 0;
    Str ct = BURROW_STR_EMPTY;
    Str got = serve(a, expvar_handler(), BURROW_S("GET"), BURROW_S("/debug/vars"),
                    &code, &ct);
    CHECK_S(got,
            "{\n"
            "\"map1\": {\"a\": 1, \"z\": 2},\n"
            "\"map2\": {\"0\": 0, \"1\": 1, \"2\": 2, \"3\": 3, \"4\": 4, \"5\": 5, "
            "\"6\": 6, \"7\": 7, \"8\": 8}\n"
            "}\n");
    CHECK_INT_EQ(code, 200);
    CHECK_S(ct, "application/json; charset=utf-8");
    arena_free(&ar);
}

static const char *recovered_text(Func fn, char *buf, size_t size) {
    volatile bool panicked = false;
    BURROW_TRY {
        BURROW_CALLF0(fn);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        size_t n = s.len < (Int)size ? (size_t)s.len : size - 1;
        memcpy(buf, s.p, n);
        buf[n] = 0;
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked ? buf : NULL;
}

static void publish_dup(void *env) {
    (void)env;
    (void)expvar_new_int(BURROW_S("dup"));
}

static void TestPublishTwice(TestingT *t) {
    burrow__expvar_remove_all();
    char buf[256];
    CHECK(recovered_text(BURROW_FN(Func, publish_dup, NULL), buf, sizeof buf) == NULL);
    const char *msg =
        recovered_text(BURROW_FN(Func, publish_dup, NULL), buf, sizeof buf);
    if (msg == NULL)
        testing_t_fatalf_v(t, "publishing dup twice did not panic");
    if (strncmp(msg, "Reuse of exported var name: dup", 31) != 0)
        testing_t_errorf_v(t, "panic = %q, want Reuse of exported var name: dup", msg);
}

static void TestRegister(TestingT *t) {
    burrow__expvar_remove_all();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    expvar_register();
    expvar_register();
    ExpvarInt *n = expvar_new_int(BURROW_S("n"));
    expvar_int_set(n, 7);

    HttpHandler mux = http_serve_mux_as_handler(http_default_serve_mux);
    Int code = 0;
    Str ct = BURROW_STR_EMPTY;
    Str got = serve(a, mux, BURROW_S("GET"), BURROW_S("/debug/vars"), &code, &ct);
    CHECK_INT_EQ(code, 200);
    CHECK_S(ct, "application/json; charset=utf-8");
    CHECK_S(got, "{\n\"n\": 7\n}\n");

    /* The pattern has GET in it, so other methods are refused, as in Go. */
    (void)serve(a, mux, BURROW_S("POST"), BURROW_S("/debug/vars"), &code, &ct);
    CHECK_INT_EQ(code, 405);
    arena_free(&ar);
}

static void TestMapFree(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    ExpvarMap m = EXPVAR_MAP(heap_allocator());
    ExpvarInt hits = {0};
    ExpvarString who = {0};
    CHECK(expvar_string_set(&who, BURROW_S("gopher")));
    for (int round = 0; round < 2; round++) {
        CHECK(expvar_map_set(&m, BURROW_S("hits"), expvar_int_as_var(&hits)));
        CHECK(expvar_map_set(&m, BURROW_S("who"), expvar_string_as_var(&who)));
        CHECK(expvar_map_add(&m, BURROW_S("adds"), 2));
        CHECK(expvar_map_add_float(&m, BURROW_S("ratio"), 0.5));
        expvar_map_delete(&m, BURROW_S("adds"));
        CHECK(expvar_map_add(&m, BURROW_S("adds"), 3));
        expvar_int_add(&hits, 1);
        Str s = expvar_map_string(&m, a);
        if (round == 0)
            CHECK_S(s,
                    "{\"adds\": 3, \"hits\": 1, \"ratio\": 0.5, \"who\": \"gopher\"}");
        else
            CHECK_S(s,
                    "{\"adds\": 3, \"hits\": 2, \"ratio\": 0.5, \"who\": \"gopher\"}");
        expvar_map_free(&m);
        CHECK_INT_EQ(count_entries(&m), 0);
    }
    expvar_string_free(&who);

    /* The zero map works as well, from heap_allocator. */
    ExpvarMap z = {0};
    CHECK(expvar_map_add(&z, BURROW_S(""), 1));
    CHECK_S(expvar_map_string(&z, a), "{\"\": 1}");
    expvar_map_free(&z);
    arena_free(&ar);
}

/* Maps under goroutines: adders on a few shared keys, setters and deleters on
 * keys of their own, and a walker writing the JSON the whole time. */
enum { RACE_WORKERS = 4, RACE_ROUNDS = 500 };

static ExpvarMap race_map;
static SyncAtomicInt64 race_running;
static SyncAtomicUint32 race_stop;
static SyncAtomicInt64 race_bad;

/* The Int each worker sets under a key of its own. Not on the worker's stack,
 * since the walker can still be reading it after the worker has gone. */
static ExpvarInt race_own[RACE_WORKERS];

static Str race_key(char *buf, size_t size, int64_t i) {
    snprintf(buf, size, "%lld", (long long)i);
    return str_from_cstr(buf);
}

static void race_adder(void *arg) {
    int64_t w = (int64_t)(Uintptr)arg;
    char buf[24];
    for (int64_t i = 0; i < RACE_ROUNDS; i++) {
        if (!expvar_map_add(&race_map, race_key(buf, sizeof buf, i % 8), 1))
            sync_atomic_int64_add(&race_bad, 1);
        Str own = race_key(buf, sizeof buf, 100 + w);
        if (!expvar_map_set(&race_map, own, expvar_int_as_var(&race_own[w])))
            sync_atomic_int64_add(&race_bad, 1);
        expvar_map_delete(&race_map, own);
        runtime_gosched();
    }
    sync_atomic_int64_add(&race_running, -1);
}

static void race_walker(void *arg) {
    (void)arg;
    while (sync_atomic_uint32_load(&race_stop) == 0) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str s = expvar_map_string(&race_map, arena_allocator(&ar));
        if (!valid_json(s))
            sync_atomic_int64_add(&race_bad, 1);
        arena_free(&ar);
        runtime_gosched();
    }
    sync_atomic_int64_add(&race_running, -1);
}

static void race_run(void) {
    sync_atomic_int64_store(&race_running, RACE_WORKERS + 1);
    for (int64_t i = 0; i < RACE_WORKERS; i++) {
        if (!go(BURROW_FN(Func, race_adder, (void *)(Uintptr)i)))
            sync_atomic_int64_add(&race_running, -1);
    }
    if (!go(BURROW_FN(Func, race_walker, NULL)))
        sync_atomic_int64_add(&race_running, -1);
    while (sync_atomic_int64_load(&race_running) > 1)
        runtime_gosched();
    sync_atomic_uint32_store(&race_stop, 1);
    while (sync_atomic_int64_load(&race_running) > 0)
        runtime_gosched();
}

static void sum_kv(void *env, ExpvarKeyValue kv) {
    if (kv.value.vt != NULL && kv.value.vt->self_type == TYPE_EXPVAR_INT)
        *(int64_t *)env += expvar_int_value(kv.value.data);
}

static void TestMapGoroutines(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    race_map = EXPVAR_MAP(NULL);
    sync_atomic_uint32_store(&race_stop, 0);
    sync_atomic_int64_store(&race_bad, 0);

    race_run();

    CHECK_INT_EQ(sync_atomic_int64_load(&race_bad), 0);
    CHECK_INT_EQ(count_entries(&race_map), 8);
    int64_t sum = 0;
    expvar_map_do(&race_map, BURROW_FN(ExpvarDoFunc, sum_kv, &sum));
    CHECK_INT_EQ(sum, RACE_WORKERS * RACE_ROUNDS);
    expvar_map_free(&race_map);
}

#define TESTS(X)                                                                       \
    X(TestPublishedByDefault)                                                          \
    X(TestNil)                                                                         \
    X(TestInt)                                                                         \
    X(TestFloat)                                                                       \
    X(TestString)                                                                      \
    X(TestMapInit)                                                                     \
    X(TestMapDelete)                                                                   \
    X(TestMapCounter)                                                                  \
    X(TestMapNil)                                                                      \
    X(TestFunc)                                                                        \
    X(TestHandler)                                                                     \
    X(TestPublishTwice)                                                                \
    X(TestRegister)                                                                    \
    X(TestMapFree)                                                                     \
    X(TestMapGoroutines)
TESTING_MAIN(TESTS)
