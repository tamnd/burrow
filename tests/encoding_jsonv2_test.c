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

static void put_hex(JsonBuf *b, uint64_t v, int digits) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        jsonbuf_byte(b, (Byte)hexd[(v >> (4 * i)) & 15]);
}

static void put_dec(JsonBuf *b, uint64_t u, bool neg) {
    Byte tmp[24];
    int k = 0;
    if (neg)
        jsonbuf_byte(b, '-');
    do {
        tmp[k++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    while (k > 0)
        jsonbuf_byte(b, tmp[--k]);
}

typedef struct DumpPath {
    const void *p[256];
    int n;
} DumpPath;

static void dump(JsonBuf *b, const Type *t, const void *p, DumpPath *path);

static int cmp_str(const void *x, const void *y) {
    const Str *a = (const Str *)x, *c = (const Str *)y;
    Int n = a->len < c->len ? a->len : c->len;
    int r = n > 0 ? memcmp(a->p, c->p, (size_t)n) : 0;
    if (r != 0)
        return r;
    return a->len < c->len ? -1 : a->len > c->len ? 1 : 0;
}

static void dump(JsonBuf *b, const Type *t, const void *p, DumpPath *path) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        jsonbuf_str(b, *(const bool *)p ? cstr("true") : cstr("false"));
        return;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64: {
        int64_t v = t->size == 1   ? *(const int8_t *)p
                    : t->size == 2 ? *(const int16_t *)p
                    : t->size == 4 ? *(const int32_t *)p
                                   : *(const int64_t *)p;
        put_dec(b, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0);
        return;
    }
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR: {
        uint64_t v = t->size == 1   ? *(const uint8_t *)p
                     : t->size == 2 ? *(const uint16_t *)p
                     : t->size == 4 ? *(const uint32_t *)p
                                    : *(const uint64_t *)p;
        put_dec(b, v, false);
        return;
    }
    case KIND_FLOAT32: {
        uint32_t bits;
        memcpy(&bits, p, 4);
        jsonbuf_byte(b, 'f');
        put_hex(b, bits, 8);
        return;
    }
    case KIND_FLOAT64: {
        uint64_t bits;
        memcpy(&bits, p, 8);
        jsonbuf_byte(b, 'd');
        put_hex(b, bits, 16);
        return;
    }
    case KIND_STRING: {
        Str s = *(const Str *)p;
        jsonbuf_byte(b, 's');
        for (Int i = 0; i < s.len; i++)
            put_hex(b, s.p[i], 2);
        return;
    }
    case KIND_SLICE: {
        const Slice *s = (const Slice *)p;
        if (s->p == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        jsonbuf_byte(b, '[');
        for (Int i = 0; i < s->len; i++) {
            if (i > 0)
                jsonbuf_byte(b, ',');
            dump(b, t->elem, (const Byte *)s->p + (size_t)i * t->elem->size, path);
        }
        jsonbuf_byte(b, ']');
        return;
    }
    case KIND_ARRAY:
        jsonbuf_byte(b, '[');
        for (uint32_t i = 0; i < t->len; i++) {
            if (i > 0)
                jsonbuf_byte(b, ',');
            dump(b, t->elem, (const Byte *)p + (size_t)i * t->elem->size, path);
        }
        jsonbuf_byte(b, ']');
        return;
    case KIND_POINTER: {
        const void *v = *(void *const *)p;
        if (v == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        for (int i = 0; i < path->n; i++) {
            if (path->p[i] == v) {
                jsonbuf_str(b, cstr("cycle"));
                return;
            }
        }
        if (path->n == (int)LEN(path->p)) {
            jsonbuf_str(b, cstr("deep"));
            return;
        }
        path->p[path->n++] = v;
        jsonbuf_byte(b, '&');
        dump(b, t->elem, v, path);
        path->n--;
        return;
    }
    case KIND_MAP: {
        Map *m = *(Map *const *)p;
        if (m == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        Int n = map_len(m);
        Str *ents = (Str *)mem_alloc(
            heap_allocator(), (size_t)(n > 0 ? n : 1) * sizeof(Str), _Alignof(Str));
        Int i = 0;
        MapIter it = map_iter(m);
        const void *k;
        void *v;
        while (i < n && map_next(&it, &k, &v)) {
            JsonBuf e = {NULL, 0, 0, heap_allocator(), true, false};
            dump(&e, t->key, k, path);
            jsonbuf_byte(&e, ':');
            dump(&e, t->elem, v, path);
            ents[i++] = (Str){e.p, e.len};
        }
        qsort(ents, (size_t)i, sizeof(Str), cmp_str);
        jsonbuf_str(b, cstr("map["));
        for (Int j = 0; j < i; j++) {
            if (j > 0)
                jsonbuf_byte(b, ',');
            jsonbuf_str(b, ents[j]);
            mem_free(heap_allocator(), (void *)(uintptr_t)ents[j].p,
                     (size_t)ents[j].len, 1);
        }
        jsonbuf_byte(b, ']');
        mem_free(heap_allocator(), ents, (size_t)(n > 0 ? n : 1) * sizeof(Str),
                 _Alignof(Str));
        return;
    }
    case KIND_STRUCT:
        jsonbuf_byte(b, '{');
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0)
                jsonbuf_byte(b, ',');
            jsonbuf_str(b, f->name);
            jsonbuf_byte(b, ':');
            dump(b, f->type, (const Byte *)p + f->offset, path);
        }
        jsonbuf_byte(b, '}');
        return;
    case KIND_INTERFACE: {
        const Any *a = (const Any *)p;
        if (a->t == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        jsonbuf_byte(b, '(');
        burrow__jsonv2_put_type(b, a->t);
        jsonbuf_byte(b, ')');
        dump(b, a->t, a->data, path);
        return;
    }
    case KIND_CHAN:
    case KIND_FUNC:
        jsonbuf_str(b, *(void *const *)p == NULL ? cstr("nil") : cstr("?"));
        return;
    default:
        return;
    }
}

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
