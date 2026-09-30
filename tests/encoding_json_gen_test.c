/* Derived from Go's src/encoding/json/v2_decode_test.go. Go source: go1.27.1.
 *
 * tests/encoding_json_test_gen.h, from tools/gen-json-tests.sh, holds the
 * cases of Go's v1 TestUnmarshal whose types this port can describe, with what
 * Go did with each: whether Valid accepted the input, the error text and a
 * dump of the value from Unmarshal and from a Decoder with the case's
 * options, and what Marshal wrote for the decoded value. The test here runs
 * the same calls and compares.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/encoding/jsonv2_internal.h"

#include "burrow/bytes.h"
#include "burrow/encoding/json.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "encoding_json_test_gen.h"
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

#include "json_dump.h"

static void *gen_alloc(Alloc *a, const Type *t) {
    return mem_alloc(a, t->size > 0 ? t->size : 1, t->align > 0 ? t->align : 1);
}

static bool str_eq_q(Str s, QStr q) {
    return s.len == (Int)q.n && (q.n == 0 || memcmp(s.p, q.p, (size_t)q.n) == 0);
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static void check_err(TestingT *t, const JCase *c, const char *what, Error err,
                      const char *want) {
    Str got = err_str(err);
    Str w = cstr(want == NULL ? "<nil>" : want);
    if (!str_eq(got, w))
        testing_t_errorf_v(t, "%s: %s error = %q, want %q", c->name, what, got, w);
}

static void check_dump(TestingT *t, const JCase *c, const char *what, void *v,
                       const char *want) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    DumpPath path;
    path.n = 0;
    dump(&b, c->type, v, &path);
    Str got = {b.p, b.len};
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "%s: %s value = %s, want %s", c->name, what, got,
                           cstr(want));
    burrow__jsonbuf_free(&b);
}

static void run_case(TestingT *t, const JCase *c) {
    Slice in = {(void *)(uintptr_t)c->in.p, (Int)c->in.n, (Int)c->in.n, TYPE_BYTE};
    if (json_valid(in) != (c->valid != 0))
        testing_t_errorf_v(t, "%s: Valid(%q) = %s, want %s", c->name, qstr(c->in),
                           cstr(c->valid ? "false" : "true"),
                           cstr(c->valid ? "true" : "false"));
    if (c->type == NULL)
        return;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    void *u = gen_alloc(a, c->type);
    Error err = json_unmarshal(a, in, BURROW_ANY(c->type, u));
    check_err(t, c, "Unmarshal", err, c->unmarshal_err);
    check_dump(t, c, "Unmarshal", u, c->unmarshal_dump);

    BytesReader br;
    bytes_reader_reset(&br, in);
    JsonDecoder *dec = json_new_decoder(a, bytes_reader_as_io_reader(&br));
    if (c->use_number)
        json_decoder_use_number(dec);
    if (c->disallow)
        json_decoder_disallow_unknown_fields(dec);
    void *d = gen_alloc(a, c->type);
    err = json_decoder_decode(dec, BURROW_ANY(c->type, d));
    check_err(t, c, "Decode", err, c->decode_err);
    check_dump(t, c, "Decode", d, c->decode_dump);
    json_decoder_free(dec);

    if (c->marshalled) {
        Error merr = BURROW_NO_ERROR;
        Slice out = json_marshal(a, BURROW_ANY(c->type, d), &merr);
        Str got = {(const Byte *)out.p, out.len};
        if (!str_eq_q(got, c->marshal_out))
            testing_t_errorf_v(t, "%s: Marshal = %q, want %q", c->name, got,
                               qstr(c->marshal_out));
        check_err(t, c, "Marshal", merr, c->marshal_err);
    }
    arena_free(&ar);
}

static void TestUnmarshal(TestingT *t) {
    gen_init();
    for (size_t i = 0; i < LEN(j_cases); i++)
        run_case(t, &j_cases[i]);
}

#define TESTS(X) X(TestUnmarshal)

TESTING_MAIN(TESTS)
