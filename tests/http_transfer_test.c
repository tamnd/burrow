/* Derived from Go's src/net/http/transfer_test.go, the two tests of the
 * transferWriter's body. The others in that file are with the tests of reading
 * a request and a response.
 * Go source: go1.27.1.
 *
 * Go's mockTransferWriter is a type with a ReadFrom method here, which is how
 * io_copy finds one. Go's test unwraps os's fileWithoutWriteTo to the File
 * inside it, and the one burrow uses for that, fileWithoutReadFrom, is
 * unwrapped the same way.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/rand.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static void TestDetectInMemoryReaders(TestingT *t) {
    ARENA_BEGIN;
    IoPipeReader *pr;
    IoPipeWriter *pw;
    io_pipe(a, &pr, &pw);
    BytesReader br;
    bytes_reader_reset(&br, slice_nil(TYPE_BYTE));
    BytesBuffer bb = BYTES_BUFFER(a);
    StringsReader sr;
    strings_reader_reset(&sr, S(""));
    BytesReader br2;
    bytes_reader_reset(&br2, slice_nil(TYPE_BYTE));
    BytesBuffer bb2 = BYTES_BUFFER(a);
    StringsReader sr2;
    strings_reader_reset(&sr2, S(""));
    IoNopCloser n_pr = io_nop_closer(io_pipe_reader_as_io_reader(pr));
    IoNopCloser n_br = io_nop_closer(bytes_reader_as_io_reader(&br2));
    IoNopCloser n_bb = io_nop_closer(bytes_buffer_as_io_reader(&bb2));
    IoNopCloser n_sr = io_nop_closer(strings_reader_as_io_reader(&sr2));

    const struct {
        IoReader r;
        bool want;
    } tests[] = {
        {io_pipe_reader_as_io_reader(pr), false},

        {bytes_reader_as_io_reader(&br), true},
        {bytes_buffer_as_io_reader(&bb), true},
        {strings_reader_as_io_reader(&sr), true},

        {io_read_closer_as_io_reader(io_nop_closer_as_io_read_closer(&n_pr)), false},

        {io_read_closer_as_io_reader(io_nop_closer_as_io_read_closer(&n_br)), true},
        {io_read_closer_as_io_reader(io_nop_closer_as_io_read_closer(&n_bb)), true},
        {io_read_closer_as_io_reader(io_nop_closer_as_io_read_closer(&n_sr)), true},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        bool got = burrow__http_is_known_in_memory_reader(tests[i].r);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d: got = %t; want %t", i, got, tests[i].want);
    }
    io_pipe_free(pr);
    ARENA_END;
}

/* ---------------------------------------- TestTransferWriterWriteBodyReaderTypes */

typedef struct MockTransferWriter {
    IoReader called_reader;
    /* What called_reader reads from when it is a LimitedReader. Go looks at
     * that afterwards, but here the LimitedReader was on write_body's stack
     * and is gone by then. */
    IoReader called_limited_r;
    bool write_called;
} MockTransferWriter;

static int64_t mock_transfer_writer_read_from(MockTransferWriter *w, IoReader r,
                                              Error *err) {
    w->called_reader = r;
    IoLimitedReader probe = io_limit_reader(r, 0);
    if (r.vt == io_limited_reader_as_io_reader(&probe).vt)
        w->called_limited_r = ((IoLimitedReader *)r.data)->r;
    return io_copy(heap_allocator(), io_discard, r, err);
}

static Int mock_transfer_writer_write(void *self, Slice p, Error *err) {
    MockTransferWriter *w = self;
    w->write_called = true;
    return BURROW_CALL(io_discard, write, p, err);
}

#define MOCK_TRANSFER_WRITER_METHODS(M, T)                                             \
    M(T, ReadFrom, mock_transfer_writer_read_from, IO_SIG_READ_FROM)
BURROW_METHODS_DEFINE(MockTransferWriter, MOCK_TRANSFER_WRITER_METHODS);

static const Type mock_transfer_writer_type = {
    {(const Byte *)"mockTransferWriter", 18},
    {(const Byte *)"http", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(MockTransferWriter),
    (uint16_t)_Alignof(MockTransferWriter),
    0,
    (uint16_t)(sizeof burrow__methods_MockTransferWriter /
               sizeof burrow__methods_MockTransferWriter[0]),
    NULL,
    burrow__methods_MockTransferWriter,
    NULL,
    NULL,
    0,
    0x6d747277U, /* "mtrw" */
    NULL,
};

static const IoWriterVT mock_transfer_writer_vt = {&mock_transfer_writer_type,
                                                   mock_transfer_writer_write};

enum { N_BYTES = 1 << 10 };

typedef enum { BODY_FILE, BODY_BUFFER } BodyKind;

typedef struct WriteBodyCase {
    const char *name;
    BodyKind body;
    bool nop_closer;
    const char *method;
    int64_t content_length;
    bool chunked;
    bool limited_reader;
    const Type *const *expected_reader; /* NULL for none */
    bool expected_write;
} WriteBodyCase;

static const Type *const *const file_type = &TYPE_OS_FILE;
static const Type *const *const buffer_type = &TYPE_BYTES_BUFFER;

static const WriteBodyCase write_body_cases[] = {
    {
        .name = "file, non-chunked, size set",
        .body = BODY_FILE,
        .method = "PUT",
        .content_length = N_BYTES,
        .limited_reader = true,
        .expected_reader = file_type,
    },
    {
        .name = "file, non-chunked, size set, nopCloser wrapped",
        .method = "PUT",
        .body = BODY_FILE,
        .nop_closer = true,
        .content_length = N_BYTES,
        .limited_reader = true,
        .expected_reader = file_type,
    },
    {
        .name = "file, non-chunked, negative size",
        .method = "PUT",
        .body = BODY_FILE,
        .content_length = -1,
        .expected_reader = file_type,
    },
    {
        .name = "file, non-chunked, CONNECT, negative size",
        .method = "CONNECT",
        .body = BODY_FILE,
        .content_length = -1,
        .expected_reader = file_type,
    },
    {
        .name = "file, chunked",
        .method = "PUT",
        .body = BODY_FILE,
        .chunked = true,
        .expected_write = true,
    },
    {
        .name = "buffer, non-chunked, size set",
        .body = BODY_BUFFER,
        .method = "PUT",
        .content_length = N_BYTES,
        .limited_reader = true,
        .expected_reader = buffer_type,
    },
    {
        .name = "buffer, non-chunked, size set, nopCloser wrapped",
        .method = "PUT",
        .body = BODY_BUFFER,
        .nop_closer = true,
        .content_length = N_BYTES,
        .limited_reader = true,
        .expected_reader = buffer_type,
    },
    {
        .name = "buffer, non-chunked, negative size",
        .method = "PUT",
        .body = BODY_BUFFER,
        .content_length = -1,
        .expected_write = true,
    },
    {
        .name = "buffer, non-chunked, CONNECT, negative size",
        .method = "CONNECT",
        .body = BODY_BUFFER,
        .content_length = -1,
        .expected_write = true,
    },
    {
        .name = "buffer, chunked",
        .method = "PUT",
        .body = BODY_BUFFER,
        .chunked = true,
        .expected_write = true,
    },
};

/* The name of a type, as reflect.Type's String gives it. */
static Str reader_type_name(Alloc *a, const Type *t) {
    if (t == NULL)
        return S("<nil>");
    return fmt_sprintf_v(a, "%s.%s", t->pkg_path, t->name);
}

static void write_body_case(void *env, TestingT *t) {
    const WriteBodyCase *tc = env;
    ARENA_BEGIN;
    Error err;
    IoReader body;
    OsFile *f = NULL;
    if (tc->body == BODY_FILE) {
        f = os_create_temp(a, S(""), S("net-http-newfilefunc"), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        /* Write some bytes to the file to enable reading. */
        (void)io_copy_n(a, os_file_as_io_writer(f), crypto_rand_reader, N_BYTES, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to write data to file: %v", err);
        (void)os_file_seek(f, 0, 0, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to seek to front: %v", err);
        body = os_file_as_io_reader(f);
    } else {
        Slice b = slice_make(a, TYPE_BYTE, N_BYTES, N_BYTES);
        body = bytes_buffer_as_io_reader(bytes_new_buffer(a, b));
    }
    IoNopCloser nc = io_nop_closer(body);
    if (tc->nop_closer)
        body = io_read_closer_as_io_reader(io_nop_closer_as_io_read_closer(&nc));

    Str chunked = S("chunked");
    MockTransferWriter mw = {0};
    burrow__HttpTransferWriter tw = {0};
    tw.body = body;
    tw.content_length = tc->content_length;
    if (tc->chunked)
        tw.transfer_encoding = slice_from(&chunked, 1, 1, TYPE_STRING);

    err = burrow__http_transfer_writer_write_body(
        &tw, a, (IoWriter){&mock_transfer_writer_vt, &mw});
    burrow__http_transfer_writer_done(&tw);
    if (f != NULL) {
        Str name = str_clone(a, os_file_name(f));
        (void)os_file_close(f);
        (void)os_remove(name);
    }
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    if (tc->expected_reader != NULL) {
        if (mw.called_reader.vt == NULL)
            testing_t_fatal_v(t, "did not call ReadFrom");

        IoLimitedReader probe = io_limit_reader(body, 0);
        const Type *actual_reader;
        bool ok = mw.called_reader.vt == io_limited_reader_as_io_reader(&probe).vt;
        if (ok && tc->limited_reader) {
            actual_reader = mw.called_limited_r.vt->self_type;
        } else {
            actual_reader = mw.called_reader.vt->self_type;
            /* We have to handle this special case for genericWriteTo in os,
             * this struct is introduced to support a zero-copy optimization,
             * check out https://go.dev/issue/58808 for details. */
            if (actual_reader != NULL && str_eq(actual_reader->pkg_path, S("os")) &&
                str_eq(actual_reader->name, S("fileWithoutReadFrom")))
                actual_reader = TYPE_OS_FILE;
        }

        if (*tc->expected_reader != actual_reader)
            testing_t_fatalf_v(t, "got reader %s want %s",
                               reader_type_name(a, actual_reader),
                               reader_type_name(a, *tc->expected_reader));
    }

    if (tc->expected_write && !mw.write_called)
        testing_t_fatal_v(t, "did not invoke Write");
    ARENA_END;
}

static void TestTransferWriterWriteBodyReaderTypes(TestingT *t) {
    for (Int i = 0; i < (Int)(sizeof write_body_cases / sizeof write_body_cases[0]);
         i++)
        testing_t_run(t, str_from_cstr(write_body_cases[i].name),
                      BURROW_FN(TestingTFunc, write_body_case,
                                (void *)(uintptr_t)&write_body_cases[i]));
}

#define TESTS(X)                                                                       \
    X(TestDetectInMemoryReaders)                                                       \
    X(TestTransferWriterWriteBodyReaderTypes)

TESTING_MAIN(TESTS)
