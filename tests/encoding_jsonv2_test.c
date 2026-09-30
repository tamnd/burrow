/* Derived from Go's src/encoding/json/v2/arshal_test.go. Go source: go1.27.1.
 *
 * tests/encoding_jsonv2_test_gen.h, from tools/gen-jsonv2-tests.sh, holds the
 * cases of Go's TestMarshal and TestUnmarshal whose types this port can
 * describe, with a C struct and descriptor for every Go type they use and a
 * function that builds each case's value. It also holds what Go did with
 * them: the bytes Marshal wrote, the error text, and a dump of the value
 * Unmarshal filled in. The tests here run the same calls and compare.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/encoding/jsonv2_internal.h"

#include "burrow/encoding/json/jsontext.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *gen_alloc(Alloc *a, const Type *t) {
    return mem_alloc(a, t->size > 0 ? t->size : 1, t->align > 0 ? t->align : 1);
}

/* A few of Go's inputs are longer than the 4095 bytes C99 promises. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "encoding_jsonv2_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define LEN(x) (sizeof(x) / sizeof((x)[0]))

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, s == NULL ? 0 : (Int)strlen(s)};
}

static JsontextOptions opts_of(const JvOpts *o) {
    JsontextOptions x;
    memset(&x, 0, sizeof(x));
    x.presence = o->presence;
    x.values = o->values;
    x.indent = qstr(o->indent);
    x.indent_prefix = qstr(o->prefix);
    x.byte_limit = o->byte_limit;
    x.depth_limit = (Int)o->depth_limit;
    return x;
}

/* ------------------------------------------------------------------- dump */

/* The same dump the generator makes of a Go value. */

#include "json_dump.h"

/* ------------------------------------------------------------------ tests */

static bool str_eq_q(Str s, QStr q) {
    return s.len == (Int)q.n && (q.n == 0 || memcmp(s.p, q.p, (size_t)q.n) == 0);
}

static bool err_matches(Error err, const char *want) {
    if (want == NULL)
        return BURROW_OK(err);
    if (BURROW_OK(err))
        return false;
    Str got = error_text(err);
    return got.len == (Int)strlen(want) && memcmp(got.p, want, (size_t)got.len) == 0;
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

/* JV_TRACE=1 names each case on stderr before it runs, for finding the one
 * that crashes. */
static void trace(const JvCase *c) {
    static int on = -1;
    if (on < 0)
        on = getenv("JV_TRACE") != NULL;
    if (on)
        fprintf(stderr, "%s %s\n", c->marshal ? "marshal" : "unmarshal", c->name);
}

static void run_marshal(TestingT *t, const JvCase *c) {
    trace(c);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Any in = {NULL, NULL};
    if (c->type != NULL) {
        void *v = gen_alloc(a, c->type);
        c->mk(a, v);
        in = BURROW_ANY(c->type, v);
    }
    JsontextOptions o = opts_of(&c->opts);
    Error err = BURROW_NO_ERROR;
    Slice out = jsonv2_marshal(a, in, (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS}, &err);
    Str got = {(const Byte *)out.p, out.len};
    if (c->canonicalize && out.len > 0) {
        JsontextValue v = {out.p, out.len, out.cap, TYPE_BYTE};
        (void)jsontext_value_canonicalize(&v, a,
                                          (Slice){NULL, 0, 0, TYPE_JSONTEXT_OPTIONS});
        got = (Str){(const Byte *)v.p, v.len};
    }
    if (!str_eq_q(got, c->out))
        testing_t_errorf_v(t, "%s: Marshal output = %q, want %q", c->name, got,
                           qstr(c->out));
    if (!err_matches(err, c->err))
        testing_t_errorf_v(t, "%s: Marshal error = %q, want %q", c->name, err_str(err),
                           cstr(c->err == NULL ? "<nil>" : c->err));
    arena_free(&ar);
}

static void run_unmarshal(TestingT *t, const JvCase *c) {
    trace(c);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    void *v = gen_alloc(a, c->type);
    c->mk(a, v);
    JsontextOptions o = opts_of(&c->opts);
    Slice in = {(void *)(uintptr_t)c->in.p, (Int)c->in.n, (Int)c->in.n, TYPE_BYTE};
    Error err = jsonv2_unmarshal(a, in, BURROW_ANY(c->type, v),
                                 (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS});
    if (!err_matches(err, c->err))
        testing_t_errorf_v(t, "%s: Unmarshal error = %q, want %q", c->name,
                           err_str(err), cstr(c->err == NULL ? "<nil>" : c->err));
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    DumpPath path;
    path.n = 0;
    dump(&b, c->type, v, &path);
    Str got = {b.p, b.len};
    if (!str_eq_q(got, (QStr){c->dump, (long long)strlen(c->dump)}))
        testing_t_errorf_v(t, "%s: Unmarshal value = %s, want %s", c->name, got,
                           cstr(c->dump));
    burrow__jsonbuf_free(&b);
    arena_free(&ar);
}

/* Go's stacks grow and ours do not, and the cycle cases nest a thousand deep
 * before anything notices, so the cases run on a goroutine with room for it. */
typedef struct Run {
    TestingT *t;
    int marshal;
    SyncWaitGroup wg;
} Run;

static void run_body(void *env) {
    Run *r = env;
    for (size_t i = 0; i < LEN(jv_cases); i++) {
        if (jv_cases[i].marshal != r->marshal)
            continue;
        if (r->marshal)
            run_marshal(r->t, &jv_cases[i]);
        else
            run_unmarshal(r->t, &jv_cases[i]);
    }
    sync_wait_group_done(&r->wg);
}

static void run_all(TestingT *t, int marshal) {
    static Run r;
    memset(&r, 0, sizeof(r));
    r.t = t;
    r.marshal = marshal;
    gen_init();
    sync_wait_group_add(&r.wg, 1);
    if (!go_stack(BURROW_FN(Func, run_body, &r), (size_t)64 << 20))
        testing_t_fatalf_v(t, "go_stack failed");
    sync_wait_group_wait(&r.wg);
}

static void TestMarshal(TestingT *t) {
    run_all(t, 1);
}

static void TestUnmarshal(TestingT *t) {
    run_all(t, 0);
}

#define TESTS(X)                                                                       \
    X(TestMarshal)                                                                     \
    X(TestUnmarshal)

TESTING_MAIN(TESTS)
