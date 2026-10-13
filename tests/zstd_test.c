/* Derived from Go's src/internal/zstd/zstd_test.go and fuzz_test.go. Go source:
 * go1.27.1.
 *
 * tools/gen-zstd-tests.sh ran every stream here through Go's reader and wrote
 * down how many bytes came out, their SHA-256 and the error. The streams are
 * Go's own samples and bad strings, its testdata, what zstd(1) makes of text,
 * noise and long runs at several levels and block sizes, and damaged copies of
 * the small ones. Each test reads them a different way and has to get what Go
 * got.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"

#include "../src/compress/zstd_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct ZstdCase {
    const char *name;
    const char *const *stream;
    long long n;
    const char *sum;
    const char *err;
} ZstdCase;

#include "zstd_test_gen.h"

#define NCASES (sizeof zstd_cases / sizeof zstd_cases[0])

static Slice gen_bytes(Alloc *a, const char *const *lines) {
    Int n = 0;
    for (const char *const *l = lines; *l != NULL; l++)
        n += (Int)strlen(*l);
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    if (p == NULL)
        abort();
    Int off = 0;
    for (const char *const *l = lines; *l != NULL; l++) {
        size_t k = strlen(*l);
        memcpy(p + off, *l, k);
        off += (Int)k;
    }
    Error err = BURROW_NO_ERROR;
    Slice out = base64_encoding_decode_string(base64_std_encoding, a,
                                              str_from_bytes(p, n), &err);
    if (BURROW_FAILED(err))
        abort();
    return out;
}

/* Checks what came out of a stream against what Go got. how says which way it
 * was read, for the message. */
static void check_case(TestingT *t, Alloc *a, const ZstdCase *c, const char *how,
                       Slice got, Error err) {
    Sha256Sum256Ret h = sha256_sum256(got);
    Str sum = hex_encode_to_string(a, (Slice){h.a, 32, 32, TYPE_BYTE});
    Str e = BURROW_OK(err) ? BURROW_STR_EMPTY : error_text(err);
    if (got.len != c->n || !str_eq(sum, str_from_cstr(c->sum)))
        testing_t_errorf_v(t, "%s, %s: got %d bytes with sum %.16s, want %d with %.16s",
                           c->name, how, got.len, sum, (int64_t)c->n, c->sum);
    if (!str_eq(e, str_from_cstr(c->err)))
        testing_t_errorf_v(t, "%s, %s: got error %q, want %q", c->name, how, e, c->err);
}

static void TestCases(TestingT *t) {
    for (size_t i = 0; i < NCASES; i++) {
        const ZstdCase *c = &zstd_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesReader *in = bytes_new_reader(a, gen_bytes(a, c->stream));
        ZstdReader *r = burrow__zstd_new_reader(a, bytes_reader_as_io_reader(in));
        Error err = BURROW_NO_ERROR;
        Slice got = io_read_all(a, burrow__zstd_reader_as_io_reader(r), &err);
        check_case(t, a, c, "read all", got, err);
        burrow__zstd_reader_free(r);
        arena_free(&ar);
    }
    CHECK(NCASES > 700);
}

/* One reader for every stream, with Reset between them, after streams that
 * failed part of the way through as much as after ones that did not. */
static void TestReset(TestingT *t) {
    Arena keep;
    arena_init(&keep, NULL, 0);
    Alloc *ka = arena_allocator(&keep);
    BytesReader *empty = bytes_new_reader(ka, (Slice){NULL, 0, 0, TYPE_BYTE});
    ZstdReader *r = burrow__zstd_new_reader(ka, bytes_reader_as_io_reader(empty));
    for (size_t i = 0; i < NCASES; i++) {
        const ZstdCase *c = &zstd_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesReader *in = bytes_new_reader(a, gen_bytes(a, c->stream));
        burrow__zstd_reader_reset(r, bytes_reader_as_io_reader(in));
        Error err = BURROW_NO_ERROR;
        Slice got = io_read_all(a, burrow__zstd_reader_as_io_reader(r), &err);
        check_case(t, a, c, "after reset", got, err);
        arena_free(&ar);
    }
    burrow__zstd_reader_free(r);
    arena_free(&keep);
}

/* ReadByte, up to where Go's reader stopped. */
static void TestReadByte(TestingT *t) {
    for (size_t i = 0; i < NCASES; i++) {
        const ZstdCase *c = &zstd_cases[i];
        if (c->n > 70000)
            continue;
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesReader *in = bytes_new_reader(a, gen_bytes(a, c->stream));
        ZstdReader *r = burrow__zstd_new_reader(a, bytes_reader_as_io_reader(in));
        Slice got = slice_make(a, TYPE_BYTE, 0, (Int)c->n);
        Error err = BURROW_NO_ERROR;
        for (;;) {
            Byte b = burrow__zstd_reader_read_byte(r, &err);
            if (BURROW_FAILED(err))
                break;
            got = slice_append(a, got, &b, 1);
        }
        if (errors_is(err, io_eof))
            err = BURROW_NO_ERROR;
        check_case(t, a, c, "byte by byte", got, err);
        burrow__zstd_reader_free(r);
        arena_free(&ar);
    }
}

/* Reads into buffers of 1 to 13 bytes in turn, which ends them everywhere in a
 * block and a frame. */
static void TestSmallReads(TestingT *t) {
    for (size_t i = 0; i < NCASES; i++) {
        const ZstdCase *c = &zstd_cases[i];
        if (c->n > 70000)
            continue;
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesReader *in = bytes_new_reader(a, gen_bytes(a, c->stream));
        ZstdReader *r = burrow__zstd_new_reader(a, bytes_reader_as_io_reader(in));
        Slice got = slice_make(a, TYPE_BYTE, 0, (Int)c->n);
        Byte buf[13];
        Error err = BURROW_NO_ERROR;
        for (Int k = 0; BURROW_OK(err); k++) {
            Int size = k % 13 + 1;
            Int n =
                burrow__zstd_reader_read(r, (Slice){buf, size, size, TYPE_BYTE}, &err);
            if (n < 0 || n > size) {
                testing_t_errorf_v(t, "%s: Read of %d returned %d", c->name, size, n);
                break;
            }
            got = slice_append(a, got, buf, n);
        }
        if (errors_is(err, io_eof))
            err = BURROW_NO_ERROR;
        check_case(t, a, c, "small reads", got, err);
        burrow__zstd_reader_free(r);
        arena_free(&ar);
    }
}

#define TESTS(X)                                                                       \
    X(TestCases)                                                                       \
    X(TestReset)                                                                       \
    X(TestReadByte)                                                                    \
    X(TestSmallReads)

TESTING_MAIN(TESTS)
