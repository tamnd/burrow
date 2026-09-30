/* tests/encoding_gob_test_gen.h, from tools/gen-gob-tests.sh, holds what Go's
 * encoding/gob does with a list of values and streams: the bytes of a stream
 * holding each value twice, and for each stream the error from decoding it
 * into a target twice over and a dump of each result. The streams include
 * Go's badDataTests and every cut and a set of corruptions of some of the
 * encoded ones. The test here makes each value, checks that encoding it gives
 * Go's bytes, and decodes Go's bytes and its own and compares.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/encoding/json_internal.h"
#include "../src/encoding/jsonv2_internal.h"

#include "burrow/bytes.h"
#include "burrow/encoding/gob.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *gen_alloc(Alloc *a, const Type *t) {
    return mem_alloc(a, t->size > 0 ? t->size : 1, t->align > 0 ? t->align : 1);
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "encoding_gob_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define LEN(x) (sizeof(x) / sizeof((x)[0]))

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, s == NULL ? 0 : (Int)strlen(s)};
}

#include "json_dump.h"

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static void check_err(TestingT *t, const GCase *c, const char *what, Str got,
                      const char *want) {
    Str w = cstr(want == NULL ? "<nil>" : want);
    if (!str_eq(got, w))
        testing_t_errorf_v(t, "%s: %s error = %q, want %q", c->name, what, got, w);
}

static void check_dump(TestingT *t, const GCase *c, const char *what, void *v,
                       const char *want) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    DumpPath path;
    path.n = 0;
    dump(&b, c->out_type, v, &path);
    Str got = {b.p, b.len};
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "%s: %s value = %s, want %s", c->name, what, got,
                           cstr(want));
    burrow__jsonbuf_free(&b);
}

/* The error of one Decode, or the text of the panic out of it after
 * "panic: ", copied while the panic value is still there to read. */
static char panic_buf[512];

static Str decode_one(GobDecoder *d, Any v, Error *err) {
    volatile Int n = -1;
    BURROW_TRY {
        *err = gob_decoder_decode(d, v);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        Int m = (Int)sizeof panic_buf - 7;
        if (s.len < m)
            m = s.len;
        memcpy(panic_buf, "panic: ", 7);
        memcpy(panic_buf + 7, s.p, (size_t)m);
        n = m + 7;
    }
    BURROW_TRY_END;
    if (n >= 0)
        return str_from_bytes((const Byte *)panic_buf, n);
    return err_str(*err);
}

static void decode_stream(TestingT *t, const GCase *c, const char *what, Slice data,
                          Alloc *a) {
    BytesReader br;
    bytes_reader_reset(&br, data);
    GobDecoder *d = gob_new_decoder(a, bytes_reader_as_io_reader(&br));
    for (int i = 0; i < 2; i++) {
        void *v = c->out_type != NULL ? gen_alloc(a, c->out_type) : NULL;
        Error err = BURROW_NO_ERROR;
        char w[64];
        snprintf(w, sizeof w, "%s decode %d", what, i + 1);
        Str got = decode_one(d, BURROW_ANY(c->out_type, v), &err);
        check_err(t, c, w, got, c->dec_err[i]);
        if (v != NULL)
            check_dump(t, c, w, v, c->dump[i]);
    }
    gob_decoder_free(d);
}

static void run_case(TestingT *t, const GCase *c) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice gs = {(void *)(uintptr_t)c->stream.p, (Int)c->stream.n, (Int)c->stream.n,
                TYPE_BYTE};

    if (c->mk != NULL) {
        void *in = gen_alloc(a, c->in_type);
        c->mk(a, in);
        BytesBuffer buf = BYTES_BUFFER(a);
        GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
        Error err = gob_encoder_encode(e, BURROW_ANY(c->in_type, in));
        if (BURROW_OK(err))
            err = gob_encoder_encode(e, BURROW_ANY(c->in_type, in));
        gob_encoder_free(e);
        check_err(t, c, "Encode", err_str(err), c->enc_err);
        Slice mine = bytes_buffer_bytes(&buf);
        if (c->enc_err == NULL) {
            Str got = {(const Byte *)mine.p, mine.len};
            Str want = {(const Byte *)c->stream.p, (Int)c->stream.n};
            if (c->det && !str_eq(got, want))
                testing_t_errorf_v(t, "%s: Encode = %x, want %x", c->name, got, want);
            decode_stream(t, c, "own", mine, a);
        }
    }
    if (c->enc_err == NULL)
        decode_stream(t, c, "Go's", gs, a);
    arena_free(&ar);
}

static void TestGobGenerated(TestingT *t) {
    gen_init();
    for (size_t i = 0; i < LEN(g_cases); i++)
        run_case(t, &g_cases[i]);
}

#define TESTS(X) X(TestGobGenerated)

TESTING_MAIN(TESTS)
