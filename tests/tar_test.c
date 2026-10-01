/* Derived from Go's src/archive/tar/reader_test.go, strconv_test.go,
 * tar_test.go and writer_test.go.
 * Go source: go1.27.1.
 *
 * The tables come over through tests/tar_test_gen.h, which
 * tools/gen-archive-tar-tests.sh has Go's own tests write out as they run,
 * along with every file in Go's testdata. The tests that are not tables are
 * ported by hand. Where Go stats testdata/small.txt or a directory on disk,
 * the port stats the same thing in a testing/fstest MapFS instead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/archive/tar.h"
#include "burrow/burrow.h"
#include "burrow/compress/gzip.h"
#include "burrow/hash/crc32.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/testing/fstest.h"

#include "../src/archive/tar_internal.h"
#include "tar_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static const char *gerr_label(GErr g) {
    static const char *const names[] = {
        "nil",
        "ErrHeader",
        "ErrWriteTooLong",
        "ErrFieldTooLong",
        "ErrWriteAfterClose",
        "ErrInsecurePath",
        "errMissData",
        "errUnrefData",
        "errWriteHole",
        "errSparseTooLong",
        "io.EOF",
        "io.ErrUnexpectedEOF",
        "headerError",
        "another error",
    };
    return names[g];
}

/* Which of the errors the tables name err is. */
static GErr gerr_of(Error err) {
    if (BURROW_OK(err))
        return GERR_NIL;
    static const struct {
        const Error *e;
        GErr g;
    } known[] = {
        {&tar_err_header, GERR_HEADER},
        {&tar_err_write_too_long, GERR_WRITE_TOO_LONG},
        {&tar_err_field_too_long, GERR_FIELD_TOO_LONG},
        {&tar_err_write_after_close, GERR_WRITE_AFTER_CLOSE},
        {&tar_err_insecure_path, GERR_INSECURE_PATH},
        {&burrow__tar_err_miss_data, GERR_MISS_DATA},
        {&burrow__tar_err_unref_data, GERR_UNREF_DATA},
        {&burrow__tar_err_write_hole, GERR_WRITE_HOLE},
        {&burrow__tar_err_sparse_too_long, GERR_SPARSE_TOO_LONG},
        {&io_eof, GERR_EOF},
        {&io_err_unexpected_eof, GERR_UNEXPECTED_EOF},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (same_error(err, *known[i].e))
            return known[i].g;
    if (err.vt->self_type == burrow__TYPE_TAR_HEADER_ERROR)
        return GERR_HEADER_ERROR;
    return GERR_OTHER;
}

static Slice const_bytes(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Str slice_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static Slice str_slice(Str s) {
    return const_bytes(s.p, s.len);
}

static Slice gunzip(Alloc *a, const char *gz, int64_t gz_len, int64_t n) {
    BytesReader br;
    bytes_reader_reset(&br, const_bytes(gz, (Int)gz_len));
    Error err;
    GzipReader *zr = gzip_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    if (zr == NULL)
        abort();
    Slice out = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    if (BURROW_FAILED(err) || out.len != (Int)n)
        abort();
    gzip_reader_free(zr);
    return out;
}

/* A string or []byte from the tables. */
static Slice gbytes(Alloc *a, GStr g) {
    if (g.zn < 0) {
        fprintf(stderr, "a string the generator left out\n");
        abort();
    }
    if (g.zn > 0)
        return gunzip(a, g.p, g.zn, g.n);
    return const_bytes(g.n == 0 ? "" : g.p, (Int)g.n);
}

static Str gstr(Alloc *a, GStr g) {
    return slice_str(gbytes(a, g));
}

/* A file from Go's testdata, by the name the tests open it with. */
static Slice testdata(Alloc *a, const char *name) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", name);
    size_t n = strlen(buf);
    if (n > 7 && strcmp(buf + n - 7, ".base64") == 0)
        buf[n -= 7] = 0;
    if (n > 4 && strcmp(buf + n - 4, ".bz2") == 0)
        buf[n - 4] = 0;
    for (size_t i = 0; i < sizeof gen_testdata / sizeof gen_testdata[0]; i++) {
        const GFile *f = &gen_testdata[i];
        if (strcmp(f->name, buf) == 0)
            return gunzip(a, f->gz, f->gz_len, f->len);
    }
    fprintf(stderr, "no testdata file %s\n", name);
    abort();
}

static Time gtime(GTime t) {
    return t.zero ? (Time){0} : time_from_unix(t.sec, t.nsec);
}

static Map *gmap(Alloc *a, GMap m) {
    if (m.nil)
        return NULL;
    Map *r = burrow__tar_str_map(a);
    for (int64_t i = 0; i < m.n; i++)
        burrow__tar_str_map_set(a, r, gstr(a, m.kv[2 * i]), gstr(a, m.kv[2 * i + 1]));
    return r;
}

/* The header a table has, from a. */
static TarHeader *gheader(Alloc *a, const GHeader *g) {
    TarHeader *h = (TarHeader *)mem_alloc(a, sizeof *h, _Alignof(TarHeader));
    h->typeflag = (Byte)g->Typeflag;
    h->name = gstr(a, g->Name);
    h->linkname = gstr(a, g->Linkname);
    h->size = g->Size;
    h->mode = g->Mode;
    h->uid = (Int)g->Uid;
    h->gid = (Int)g->Gid;
    h->uname = gstr(a, g->Uname);
    h->gname = gstr(a, g->Gname);
    h->mod_time = gtime(g->ModTime);
    h->access_time = gtime(g->AccessTime);
    h->change_time = gtime(g->ChangeTime);
    h->devmajor = g->Devmajor;
    h->devminor = g->Devminor;
    h->xattrs = gmap(a, g->Xattrs);
    h->pax_records = gmap(a, g->PAXRecords);
    h->format = (TarFormat)g->Format;
    return h;
}

static Str map_value(Map *m, Str k, bool *ok) {
    Str *v = m == NULL ? NULL : (Str *)map_get(m, &k);
    *ok = v != NULL;
    return v != NULL ? *v : (Str){NULL, 0};
}

/* maps.Equal. */
static bool str_maps_equal(Map *a, Map *b) {
    if (map_len(a) != map_len(b))
        return false;
    if (a == NULL)
        return true;
    const void *k;
    void *v;
    for (MapIter it = map_iter(a); map_next(&it, &k, &v);) {
        bool ok;
        Str w = map_value(b, *(const Str *)k, &ok);
        if (!ok || !str_eq(w, *(Str *)v))
            return false;
    }
    return true;
}

/* reflect.DeepEqual on maps, where nil and empty differ. */
static bool maps_deep_equal(Map *a, Map *b) {
    return (a == NULL) == (b == NULL) && str_maps_equal(a, b);
}

static bool times_equal(Time a, Time b) {
    if (time_is_zero(a) || time_is_zero(b))
        return time_is_zero(a) && time_is_zero(b);
    return time_equal(a, b);
}

/* reflect.DeepEqual on two headers, with the first field that differs in
 * *why. */
static bool headers_equal(const TarHeader *a, const TarHeader *b, const char **why) {
#define TAR_DIFF(cond, name)                                                           \
    if (!(cond)) {                                                                     \
        *why = name;                                                                   \
        return false;                                                                  \
    }
    TAR_DIFF(a->typeflag == b->typeflag, "Typeflag");
    TAR_DIFF(str_eq(a->name, b->name), "Name");
    TAR_DIFF(str_eq(a->linkname, b->linkname), "Linkname");
    TAR_DIFF(a->size == b->size, "Size");
    TAR_DIFF(a->mode == b->mode, "Mode");
    TAR_DIFF(a->uid == b->uid, "Uid");
    TAR_DIFF(a->gid == b->gid, "Gid");
    TAR_DIFF(str_eq(a->uname, b->uname), "Uname");
    TAR_DIFF(str_eq(a->gname, b->gname), "Gname");
    TAR_DIFF(times_equal(a->mod_time, b->mod_time), "ModTime");
    TAR_DIFF(times_equal(a->access_time, b->access_time), "AccessTime");
    TAR_DIFF(times_equal(a->change_time, b->change_time), "ChangeTime");
    TAR_DIFF(a->devmajor == b->devmajor, "Devmajor");
    TAR_DIFF(a->devminor == b->devminor, "Devminor");
    TAR_DIFF(maps_deep_equal(a->xattrs, b->xattrs), "Xattrs");
    TAR_DIFF(maps_deep_equal(a->pax_records, b->pax_records), "PAXRecords");
    TAR_DIFF(a->format == b->format, "Format");
#undef TAR_DIFF
    *why = NULL;
    return true;
}

static bool sparse_equal(const TarSparseEntry *got, Int n, GSlice_GsparseEntry want) {
    if (n != (Int)want.n)
        return false;
    for (Int i = 0; i < n; i++)
        if (got[i].offset != want.p[i].Offset || got[i].length != want.p[i].Length)
            return false;
    return true;
}

static TarSparseEntry *sparse_copy(Alloc *a, GSlice_GsparseEntry s) {
    TarSparseEntry *p = (TarSparseEntry *)mem_alloc(a, sizeof *p * (size_t)(s.n + 1),
                                                    _Alignof(TarSparseEntry));
    for (int64_t i = 0; i < s.n; i++)
        p[i] = (TarSparseEntry){s.p[i].Offset, s.p[i].Length};
    return p;
}

static Str repeat(Alloc *a, const char *s, Int n) {
    return strings_repeat(a, str_from_bytes(s, (Int)strlen(s)), n);
}

/* ------------------------------------------------------------ allocators */

/* An allocator that counts what is live and can be told to fail after a
 * number of calls. */
typedef struct Budget {
    long long left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(heap_allocator(), p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

/* --------------------------------------------------------------- readers */

/* A reader with only Read, Go's struct{ io.Reader }. */
typedef struct PlainReader {
    IoReader r;
} PlainReader;

static Int plain_read(void *self, Slice p, Error *err) {
    PlainReader *pr = (PlainReader *)self;
    return pr->r.vt->read(pr->r.data, p, err);
}

static const IoReaderVT plain_reader_vt = {NULL, plain_read};

/* readBadSeeker: a reader whose Seek always fails. */
typedef struct BadSeeker {
    IoReader r;
} BadSeeker;

BURROW_SENTINEL_ERROR(bad_seek_err, "illegal seek");

static int64_t bad_seeker_seek(BadSeeker *s, int64_t off, Int whence, IoErrorArg err) {
    (void)s;
    (void)off;
    (void)whence;
    *err = bad_seek_err;
    return 0;
}

#define BAD_SEEKER_METHODS(M, T) M(T, Seek, bad_seeker_seek, IO_SIG_SEEK)
BURROW_METHODS_DEFINE(BadSeeker, BAD_SEEKER_METHODS);

static const Type bad_seeker_type = {
    {(const Byte *)"readbadseeker", 13},
    {(const Byte *)"tar_test", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BadSeeker),
    (uint16_t)_Alignof(BadSeeker),
    0,
    (uint16_t)(sizeof burrow__methods_BadSeeker / sizeof burrow__methods_BadSeeker[0]),
    NULL,
    burrow__methods_BadSeeker,
    NULL,
    NULL,
    0,
    0x74726273U,
    NULL,
};

static Int bad_seeker_read(void *self, Slice p, Error *err) {
    BadSeeker *s = (BadSeeker *)self;
    return s->r.vt->read(s->r.data, p, err);
}

static const IoReaderVT bad_seeker_vt = {&bad_seeker_type, bad_seeker_read};

/* testNonEmptyReader and testNonEmptyWriter. */
BURROW_SENTINEL_ERROR(empty_read_err, "unexpected empty Read call");
BURROW_SENTINEL_ERROR(empty_write_err, "unexpected empty Write call");

static Int non_empty_read(void *self, Slice b, Error *err) {
    if (b.len == 0) {
        *err = empty_read_err;
        return 0;
    }
    IoReader *r = (IoReader *)self;
    return r->vt->read(r->data, b, err);
}

static const IoReaderVT non_empty_reader_vt = {NULL, non_empty_read};

static Int non_empty_write(void *self, Slice b, Error *err) {
    if (b.len == 0) {
        *err = empty_write_err;
        return 0;
    }
    IoWriter *w = (IoWriter *)self;
    return w->vt->write(w->data, b, err);
}

static const IoWriterVT non_empty_writer_vt = {NULL, non_empty_write};

/* iotest.TruncateWriter: writes the first n bytes and drops the rest, saying
 * it wrote them. */
typedef struct TruncateWriter {
    IoWriter w;
    int64_t n;
} TruncateWriter;

static Int truncate_write(void *self, Slice p, Error *err) {
    TruncateWriter *t = (TruncateWriter *)self;
    *err = BURROW_NO_ERROR;
    if (t->n <= 0)
        return p.len;
    Int n = p.len;
    if ((int64_t)n > t->n)
        n = (Int)t->n;
    n = t->w.vt->write(t->w.data, slice_sub(p, 0, n), err);
    t->n -= n;
    if (BURROW_OK(*err))
        n = p.len;
    return n;
}

static const IoWriterVT truncate_writer_vt = {NULL, truncate_write};

/* ------------------------------------------------------------- testFile */

/* testFile: a ReadWriteSeeker whose calls have to match ops, each a string
 * to read or write or an offset to seek by. */
typedef struct TestFileOp {
    bool is_str;
    Str s;
    int64_t off;
} TestFileOp;

typedef struct TestFile {
    TestFileOp *ops;
    Int n;
    int64_t pos;
    bool test_err; /* testError: a call that did not match */
    char why[160];
} TestFile;

BURROW_SENTINEL_ERROR(test_file_read_err, "unexpected Read operation");
BURROW_SENTINEL_ERROR(test_file_write_err, "unexpected Write operation");
BURROW_SENTINEL_ERROR(test_file_seek_err, "unexpected Seek operation");
BURROW_SENTINEL_ERROR(test_file_mismatch_err, "testError");

static void test_file_init(Alloc *a, TestFile *f, GSlice_GAny ops) {
    memset(f, 0, sizeof *f);
    f->ops = (TestFileOp *)mem_alloc(a, sizeof *f->ops * (size_t)(ops.n + 1),
                                     _Alignof(TestFileOp));
    for (int64_t i = 0; i < ops.n; i++) {
        if (strcmp(ops.p[i].type, "GStr") == 0)
            f->ops[i] = (TestFileOp){true, gstr(a, *(const GStr *)ops.p[i].v), 0};
        else
            f->ops[i] = (TestFileOp){false, {NULL, 0}, *(const int64_t *)ops.p[i].v};
    }
    f->n = (Int)ops.n;
}

static void test_file_pop(TestFile *f) {
    f->ops++;
    f->n--;
}

static Int test_file_read(void *self, Slice b, Error *err) {
    TestFile *f = (TestFile *)self;
    *err = BURROW_NO_ERROR;
    if (b.len == 0)
        return 0;
    if (f->n == 0) {
        *err = io_eof;
        return 0;
    }
    if (!f->ops[0].is_str) {
        *err = test_file_read_err;
        return 0;
    }
    Str s = f->ops[0].s;
    Int n = s.len < b.len ? s.len : b.len;
    if (n > 0)
        memcpy(b.p, s.p, (size_t)n);
    if (s.len > n)
        f->ops[0].s = (Str){s.p + n, s.len - n};
    else
        test_file_pop(f);
    f->pos += b.len;
    return n;
}

static Int test_file_write(void *self, Slice b, Error *err) {
    TestFile *f = (TestFile *)self;
    *err = BURROW_NO_ERROR;
    if (b.len == 0)
        return 0;
    if (f->n == 0 || !f->ops[0].is_str) {
        *err = test_file_write_err;
        return 0;
    }
    Str s = f->ops[0].s;
    if (!strings_has_prefix(s, slice_str(b))) {
        f->test_err = true;
        snprintf(f->why, sizeof f->why, "got Write of %d bytes, not a prefix of %d",
                 (int)b.len, (int)s.len);
        *err = test_file_mismatch_err;
        return 0;
    }
    if (s.len > b.len)
        f->ops[0].s = (Str){s.p + b.len, s.len - b.len};
    else
        test_file_pop(f);
    f->pos += b.len;
    return b.len;
}

static int64_t test_file_seek(TestFile *f, int64_t pos, Int whence, IoErrorArg err) {
    *err = BURROW_NO_ERROR;
    if (pos == 0 && whence == BURROW_IO_SEEK_CURRENT)
        return f->pos;
    if (f->n == 0 || f->ops[0].is_str) {
        *err = test_file_seek_err;
        return 0;
    }
    int64_t s = f->ops[0].off;
    if (s != pos || whence != BURROW_IO_SEEK_CURRENT) {
        f->test_err = true;
        snprintf(f->why, sizeof f->why, "got Seek(%lld, %d), want Seek(%lld, 1)",
                 (long long)pos, (int)whence, (long long)s);
        *err = test_file_mismatch_err;
        return 0;
    }
    f->pos += s;
    test_file_pop(f);
    return f->pos;
}

#define TEST_FILE_METHODS(M, T) M(T, Seek, test_file_seek, IO_SIG_SEEK)
BURROW_METHODS_DEFINE(TestFile, TEST_FILE_METHODS);

static const Type test_file_type = {
    {(const Byte *)"testfile", 8},
    {(const Byte *)"tar_test", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TestFile),
    (uint16_t)_Alignof(TestFile),
    0,
    (uint16_t)(sizeof burrow__methods_TestFile / sizeof burrow__methods_TestFile[0]),
    NULL,
    burrow__methods_TestFile,
    NULL,
    NULL,
    0,
    0x74667466U,
    NULL,
};

static const IoReaderVT test_file_reader_vt = {&test_file_type, test_file_read};
static const IoWriterVT test_file_writer_vt = {&test_file_type, test_file_write};

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* ---------------------------------------------------------------- strconv */

static void TestFitsInBase256(TestingT *t) {
    for (Int i = 0; i < LEN(gen_TestFitsInBase256); i++) {
        const TestFitsInBase256_v *v = &gen_TestFitsInBase256[i];
        bool ok = burrow__tar_fits_in_base256((Int)v->width, v->in);
        if (ok != v->ok)
            testing_t_errorf_v(t, "fitsInBase256(%d, %d): got %v, want %v", v->in,
                               v->width, ok, v->ok);
    }
}

static void TestParseNumeric(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestParseNumeric); i++) {
        const TestParseNumeric_v *v = &gen_TestParseNumeric[i];
        Slice in = gbytes(a, v->in);
        Error perr = BURROW_NO_ERROR;
        int64_t got = burrow__tar_parse_numeric(in.p, in.len, &perr);
        bool ok = BURROW_OK(perr);
        if (ok != v->ok) {
            if (v->ok)
                testing_t_errorf_v(
                    t, "parseNumeric(%q): got parsing failure, want success",
                    slice_str(in));
            else
                testing_t_errorf_v(
                    t, "parseNumeric(%q): got parsing success, want failure",
                    slice_str(in));
        }
        if (ok && got != v->want)
            testing_t_errorf_v(t, "parseNumeric(%q): got %d, want %d", slice_str(in),
                               got, v->want);
    }
    arena_free(&ar);
}

static void TestFormatNumeric(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestFormatNumeric); i++) {
        const TestFormatNumeric_v *v = &gen_TestFormatNumeric[i];
        Str want = gstr(a, v->want);
        Byte *got = (Byte *)mem_alloc(a, (size_t)want.len + 1, 1);
        Error ferr = BURROW_NO_ERROR;
        burrow__tar_format_numeric(got, want.len, v->in, &ferr);
        bool ok = BURROW_OK(ferr);
        if (ok != v->ok) {
            if (v->ok)
                testing_t_errorf_v(
                    t, "formatNumeric(%d): got formatting failure, want success",
                    v->in);
            else
                testing_t_errorf_v(
                    t, "formatNumeric(%d): got formatting success, want failure",
                    v->in);
        }
        if (!str_eq(str_from_bytes(got, want.len), want))
            testing_t_errorf_v(t, "formatNumeric(%d): got %q, want %q", v->in,
                               str_from_bytes(got, want.len), want);
    }
    arena_free(&ar);
}

static void TestFitsInOctal(TestingT *t) {
    for (Int i = 0; i < LEN(gen_TestFitsInOctal); i++) {
        const TestFitsInOctal_v *v = &gen_TestFitsInOctal[i];
        bool ok = burrow__tar_fits_in_octal((Int)v->width, v->input);
        if (ok != v->ok)
            testing_t_errorf_v(t, "checkOctal(%d, %d): got %v, want %v", v->input,
                               v->width, ok, v->ok);
    }
}

static void TestParsePAXTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestParsePAXTime); i++) {
        const TestParsePAXTime_v *v = &gen_TestParsePAXTime[i];
        Str in = gstr(a, v->in);
        Error err = BURROW_NO_ERROR;
        Time ts = burrow__tar_parse_pax_time(in, &err);
        bool ok = BURROW_OK(err);
        if (v->ok != ok) {
            if (v->ok)
                testing_t_errorf_v(
                    t, "parsePAXTime(%q): got parsing failure, want success", in);
            else
                testing_t_errorf_v(
                    t, "parsePAXTime(%q): got parsing success, want failure", in);
        }
        Time want = gtime(v->want);
        if (ok && !time_equal(ts, want))
            testing_t_errorf_v(t, "parsePAXTime(%q): got (%ds %dns), want (%ds %dns)",
                               in, time_unix(ts), (int64_t)time_nanosecond(ts),
                               time_unix(want), (int64_t)time_nanosecond(want));
    }
    arena_free(&ar);
}

static void TestFormatPAXTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestFormatPAXTime); i++) {
        const TestFormatPAXTime_v *v = &gen_TestFormatPAXTime[i];
        Str got = burrow__tar_format_pax_time(a, time_from_unix(v->sec, v->nsec));
        Str want = gstr(a, v->want);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "formatPAXTime(%ds, %dns): got %q, want %q", v->sec,
                               v->nsec, got, want);
    }
    arena_free(&ar);
}

static void TestParsePAXRecord(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestParsePAXRecord); i++) {
        const TestParsePAXRecord_v *v = &gen_TestParsePAXRecord[i];
        Str in = gstr(a, v->in);
        Str key = {NULL, 0}, val = {NULL, 0};
        Error err = BURROW_NO_ERROR;
        Str res = burrow__tar_parse_pax_record(in, &key, &val, &err);
        bool ok = BURROW_OK(err);
        if (ok != v->ok) {
            if (v->ok)
                testing_t_errorf_v(
                    t, "parsePAXRecord(%q): got parsing failure, want success", in);
            else
                testing_t_errorf_v(
                    t, "parsePAXRecord(%q): got parsing success, want failure", in);
        }
        Str wk = gstr(a, v->wantKey), wv = gstr(a, v->wantVal);
        if (v->ok && (!str_eq(key, wk) || !str_eq(val, wv)))
            testing_t_errorf_v(t, "parsePAXRecord(%q): got (%q: %q), want (%q: %q)", in,
                               key, val, wk, wv);
        Str wr = gstr(a, v->wantRes);
        if (!str_eq(res, wr))
            testing_t_errorf_v(t,
                               "parsePAXRecord(%q): got residual %q, want residual %q",
                               in, res, wr);
    }
    arena_free(&ar);
}

static void TestFormatPAXRecord(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestFormatPAXRecord); i++) {
        const TestFormatPAXRecord_v *v = &gen_TestFormatPAXRecord[i];
        Str k = gstr(a, v->inKey), val = gstr(a, v->inVal);
        Error err = BURROW_NO_ERROR;
        Str got = burrow__tar_format_pax_record(a, k, val, &err);
        bool ok = BURROW_OK(err);
        if (ok != v->ok) {
            if (v->ok)
                testing_t_errorf_v(
                    t, "formatPAXRecord(%q, %q): got format failure, want success", k,
                    val);
            else
                testing_t_errorf_v(
                    t, "formatPAXRecord(%q, %q): got format success, want failure", k,
                    val);
        }
        Str want = gstr(a, v->want);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "formatPAXRecord(%q, %q): got %q, want %q", k, val,
                               got, want);
    }
    arena_free(&ar);
}

/* ----------------------------------------------------------------- reader */

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    for (Int vi = 0; vi < LEN(gen_TestReader); vi++) {
        const TestReader_v *v = &gen_TestReader[vi];
        Str file = gstr(scratch, v->file);
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)file.len, (const char *)file.p);
        Slice data = testdata(scratch, name);

        /* Go reads an os.File, which can seek, except through bzip2. */
        BytesReader br;
        bytes_reader_reset(&br, data);
        PlainReader pr = {bytes_reader_as_io_reader(&br)};
        IoReader src = pr.r;
        if (has_suffix(name, ".bz2") || has_suffix(name, ".bz2.base64"))
            src = (IoReader){&plain_reader_vt, &pr};

        Budget b = {1LL << 40, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        TarReader *tr = tar_new_reader(&al, src);
        Byte rdbuf[8];
        Error err = BURROW_NO_ERROR;
        Int nh = 0, ns = 0;
        bool failed = false;
        for (;;) {
            TarHeader *hdr = tar_reader_next(tr, &err);
            if (BURROW_FAILED(err)) {
                if (same_error(err, io_eof))
                    err = BURROW_NO_ERROR;
                tar_header_free(&al, hdr);
                break;
            }
            if (nh >= v->headers.n) {
                testing_t_errorf_v(t, "%s: entry %d: unexpected header %q", name, nh,
                                   hdr->name);
                failed = true;
            } else {
                const char *why;
                TarHeader *want = gheader(scratch, v->headers.p[nh]);
                if (!headers_equal(hdr, want, &why)) {
                    testing_t_errorf_v(t, "%s: entry %d: incorrect header: %s differs",
                                       name, nh, str_from_bytes(why, (Int)strlen(why)));
                    failed = true;
                }
            }
            nh++;
            tar_header_free(&al, hdr);
            if (failed)
                break;
            if (v->chksums.nil)
                continue;
            uint32_t crc = 0;
            for (;;) {
                Error re = BURROW_NO_ERROR;
                Int n = tar_reader_read(tr, const_bytes(rdbuf, 8), &re);
                if (n > 0)
                    crc = crc32_update(crc, crc32_ieee_table, const_bytes(rdbuf, n));
                if (same_error(re, io_eof))
                    break;
                if (BURROW_FAILED(re)) {
                    err = re;
                    break;
                }
            }
            if (BURROW_FAILED(err))
                break;
            char sum[16];
            snprintf(sum, sizeof sum, "%08x", (unsigned)crc);
            if (ns >= v->chksums.n) {
                testing_t_errorf_v(t, "%s: entry %d: unexpected sum: got %s", name, ns,
                                   str_from_bytes(sum, 8));
            } else {
                Str want = gstr(scratch, v->chksums.p[ns]);
                if (!str_eq(str_from_bytes(sum, 8), want))
                    testing_t_errorf_v(
                        t, "%s: entry %d: incorrect checksum: got %s, want %s", name,
                        ns, str_from_bytes(sum, 8), want);
            }
            ns++;
        }
        if (!failed && nh != v->headers.n)
            testing_t_errorf_v(t, "%s: got %d headers, want %d headers", name, nh,
                               v->headers.n);
        if (!failed && gerr_of(err) != v->err)
            testing_t_errorf_v(
                t, "%s: unexpected error: got %v, want %s", name, err,
                str_from_bytes(gerr_label(v->err), (Int)strlen(gerr_label(v->err))));
        tar_reader_free(tr);
        if (b.live != 0)
            testing_t_errorf_v(t, "%s: %d bytes leaked", name, (int64_t)b.live);
    }
    arena_free(&ar);
}

static void TestPartialRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int vi = 0; vi < LEN(gen_TestPartialRead); vi++) {
        const TestPartialRead_v *v = &gen_TestPartialRead[vi];
        Str file = gstr(a, v->file);
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)file.len, (const char *)file.p);
        BytesReader br;
        bytes_reader_reset(&br, testdata(a, name));
        TarReader *tr = tar_new_reader(a, bytes_reader_as_io_reader(&br));
        for (int64_t i = 0; i < v->cases.n; i++) {
            const TestPartialRead_testCase *tc = &v->cases.p[i];
            Error err;
            TarHeader *hdr = tar_reader_next(tr, &err);
            if (BURROW_FAILED(err) || hdr == NULL) {
                testing_t_errorf_v(t, "%s: entry %d, Next(): got %v, want nil", name, i,
                                   err);
                break;
            }
            Slice buf = slice_from(mem_alloc(a, (size_t)tc->cnt + 1, 1), (Int)tc->cnt,
                                   (Int)tc->cnt, TYPE_BYTE);
            io_read_full(tar_reader_as_io_reader(tr), buf, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: entry %d, ReadFull(): got %v, want nil",
                                   name, i, err);
                break;
            }
            Str want = gstr(a, tc->output);
            if (!str_eq(slice_str(buf), want))
                testing_t_errorf_v(t, "%s: entry %d, ReadFull(): got %q, want %q", name,
                                   i, slice_str(buf), want);
        }
        Error err;
        tar_reader_next(tr, &err);
        if (!same_error(err, io_eof))
            testing_t_errorf_v(t, "%s: Next(): got %v, want EOF", name, err);
    }
    arena_free(&ar);
}

static void TestUninitializedRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, testdata(a, "testdata/gnu.tar"));
    TarReader *tr = tar_new_reader(a, bytes_reader_as_io_reader(&br));
    Error err;
    tar_reader_read(tr, const_bytes("", 0), &err);
    if (!same_error(err, io_eof))
        testing_t_errorf_v(t, "Unexpected error: %v, wanted %v", err, io_eof);
    arena_free(&ar);
}

static Slice cat(Alloc *a, Slice x, Slice y) {
    Slice r = slice_from(mem_alloc(a, (size_t)(x.len + y.len) + 1, 1), x.len + y.len,
                         x.len + y.len, TYPE_BYTE);
    if (x.len > 0 && x.p != NULL)
        memcpy(r.p, x.p, (size_t)x.len);
    if (y.len > 0 && y.p != NULL)
        memcpy((Byte *)r.p + x.len, y.p, (size_t)y.len);
    return r;
}

static void TestReadTruncation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data1 = testdata(a, "testdata/gnu.tar");
    Slice data2 = testdata(a, "testdata/ustar-file-reg.tar");
    Slice pax = testdata(a, "testdata/pax-path-hdr.tar");
    Slice sparse = testdata(a, "testdata/sparse-formats.tar");
    Byte *zeros = (Byte *)mem_alloc(a, (size_t)10 * 512, 1);
    data2 = cat(a, data2, const_bytes(zeros, (Int)10 * 512));
    Slice trash = str_slice(repeat(a, "garbage ", 64));
    Slice none = const_bytes("", 0);

#define D1(n) slice_sub(data1, 0, n)
#define D2(n) slice_sub(data2, 0, n)
#define TR(n) slice_sub(trash, 0, n)
#define SP(n) slice_sub(sparse, 0, n)
    const struct {
        Slice input;
        Int cnt;
        Error err;
    } vectors[] = {
        {none, 0, io_eof},
        {D1(511), 0, io_err_unexpected_eof},
        {D1(512), 1, io_err_unexpected_eof},
        {D1(1024), 1, io_eof},
        {D1(1536), 2, io_err_unexpected_eof},
        {D1(2048), 2, io_eof},
        {data1, 2, io_eof},
        {cat(a, D1(2048), D2(1536)), 3, io_eof},
        {D2(511), 0, io_err_unexpected_eof},
        {D2(512), 1, io_err_unexpected_eof},
        {D2(1195), 1, io_err_unexpected_eof},
        {D2(1196), 1, io_eof},
        {D2(1200), 1, io_eof},
        {D2(1535), 1, io_eof},
        {D2(1536), 1, io_eof},
        {cat(a, D2(1536), TR(1)), 1, io_err_unexpected_eof},
        {cat(a, D2(1536), TR(511)), 1, io_err_unexpected_eof},
        {cat(a, D2(1536), trash), 1, tar_err_header},
        {D2(2048), 1, io_eof},
        {cat(a, D2(2048), TR(1)), 1, io_err_unexpected_eof},
        {cat(a, D2(2048), TR(511)), 1, io_err_unexpected_eof},
        {cat(a, D2(2048), trash), 1, tar_err_header},
        {D2(2560), 1, io_eof},
        {cat(a, D2(2560), TR(1)), 1, io_eof},
        {cat(a, D2(2560), TR(511)), 1, io_eof},
        {cat(a, D2(2560), trash), 1, io_eof},
        {D2(3072), 1, io_eof},
        {pax, 0, io_eof},
        {cat(a, pax, TR(1)), 0, io_err_unexpected_eof},
        {cat(a, pax, TR(511)), 0, io_err_unexpected_eof},
        {SP(511), 0, io_err_unexpected_eof},
        {SP(512), 0, io_err_unexpected_eof},
        {SP(3584), 1, io_eof},
        {SP(9200), 1, io_eof},
        {SP(9216), 1, io_eof},
        {SP(9728), 2, io_err_unexpected_eof},
        {SP(10240), 2, io_eof},
        {SP(11264), 2, io_err_unexpected_eof},
        {sparse, 5, io_eof},
        {cat(a, sparse, trash), 5, io_eof},
    };
#undef D1
#undef D2
#undef TR
#undef SP

    static const char *const kinds[] = {"io.Reader", "io.ReadSeeker", "ReadBadSeeker"};
    for (Int i = 0; i < LEN(vectors); i++) {
        for (int j = 0; j < 6; j++) {
            BytesReader br;
            bytes_reader_reset(&br, vectors[i].input);
            PlainReader pr = {bytes_reader_as_io_reader(&br)};
            BadSeeker bs = {bytes_reader_as_io_reader(&br)};
            IoReader src;
            switch (j / 2) {
            case 0:
                src = (IoReader){&plain_reader_vt, &pr};
                break;
            case 1:
                src = pr.r;
                break;
            default:
                src = (IoReader){&bad_seeker_vt, &bs};
                break;
            }
            bool manual = j % 2 == 1;
            Budget b = {1LL << 40, 0};
            Alloc al = {&budget_vt, &b, NULL, NULL};
            TarReader *tr = tar_new_reader(&al, src);
            Int cnt = 0;
            Error err;
            for (;;) {
                TarHeader *h = tar_reader_next(tr, &err);
                tar_header_free(&al, h);
                if (BURROW_FAILED(err))
                    break;
                cnt++;
                if (manual) {
                    burrow__tar_reader_write_to(tr, io_discard, &err);
                    if (BURROW_FAILED(err))
                        break;
                }
            }
            Str kind = str_from_bytes(kinds[j / 2], (Int)strlen(kinds[j / 2]));
            Str how = manual ? BURROW_S("manual") : BURROW_S("auto");
            if (!same_error(err, vectors[i].err))
                testing_t_errorf_v(
                    t, "test %d, NewReader(%s) with %s discard: got %v, want %v", i,
                    kind, how, err, vectors[i].err);
            if (cnt != vectors[i].cnt)
                testing_t_errorf_v(
                    t,
                    "test %d, NewReader(%s) with %s discard: got %d headers, "
                    "want %d headers",
                    i, kind, how, cnt, vectors[i].cnt);
            tar_reader_free(tr);
            if (b.live != 0)
                testing_t_errorf_v(t, "test %d, %s, %s: %d bytes leaked", i, kind, how,
                                   (int64_t)b.live);
        }
    }
    arena_free(&ar);
}

static void TestReadHeaderOnly(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, testdata(a, "testdata/hdr-only.tar"));
    TarReader *tr = tar_new_reader(a, bytes_reader_as_io_reader(&br));
    TarHeader *hdrs[32];
    int n = 0;
    for (;;) {
        Error err;
        TarHeader *hdr = tar_reader_next(tr, &err);
        if (same_error(err, io_eof))
            break;
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Next(): got %v, want nil", err);
            continue;
        }
        if (n < 32)
            hdrs[n] = hdr;
        n++;
        Byte one[1] = {0};
        Int cnt = io_read_full(tar_reader_as_io_reader(tr), const_bytes(one, 1), &err);
        if (cnt > 0 && hdr->typeflag != TAR_TYPE_REG)
            testing_t_errorf_v(t, "ReadFull(...): got %d bytes, want 0 bytes", cnt);
    }
    if (n != 16) {
        testing_t_errorf_v(t, "len(hdrs): got %d, want %d", n, 16);
        arena_free(&ar);
        return;
    }
    for (int i = 0; i < 8; i++) {
        TarHeader *h1 = hdrs[i], *h2 = hdrs[i + 8];
        h1->size = h2->size = 0;
        const char *why;
        if (!headers_equal(h1, h2, &why))
            testing_t_errorf_v(t, "incorrect header %d: %s differs", i,
                               str_from_bytes(why, (Int)strlen(why)));
    }
    arena_free(&ar);
}

static void TestMergePAX(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestMergePAX); i++) {
        const TestMergePAX_v *v = &gen_TestMergePAX[i];
        TarHeader *got = (TarHeader *)mem_alloc(a, sizeof *got, _Alignof(TarHeader));
        Error err = burrow__tar_merge_pax(a, got, gmap(a, v->in));
        const char *why;
        if (v->ok && !headers_equal(got, gheader(a, v->want), &why))
            testing_t_errorf_v(t, "test %d, mergePAX(...): %s differs", i,
                               str_from_bytes(why, (Int)strlen(why)));
        bool ok = BURROW_OK(err);
        if (ok != v->ok)
            testing_t_errorf_v(t, "test %d, mergePAX(...): got %v, want %v", i, ok,
                               v->ok);
    }
    arena_free(&ar);
}

static void TestParsePAX(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestParsePAX); i++) {
        const TestParsePAX_v *v = &gen_TestParsePAX[i];
        StringsReader sr;
        strings_reader_reset(&sr, gstr(a, v->in));
        Error err;
        Map *got = burrow__tar_parse_pax(a, strings_reader_as_io_reader(&sr), &err);
        Map *want = gmap(a, v->want);
        if (!str_maps_equal(got, want) && !(map_len(got) == 0 && map_len(want) == 0))
            testing_t_errorf_v(t, "test %d, parsePAX(): maps differ", i);
        bool ok = BURROW_OK(err);
        if (ok != v->ok)
            testing_t_errorf_v(t, "test %d, parsePAX(): got %v, want %v", i, ok, v->ok);
    }
    arena_free(&ar);
}

/* makeInput(FormatGNU, "", makeSparseStrings(entries)...) from Go's test, for
 * the input too long for the generator to write out: a GNU header with the
 * first four entries and then blocks of 21 more. */
static Slice old_gnu_sparse_input(Alloc *a, Int count) {
    Int blocks = 1 + (count - 4 + 20) / 21;
    Byte *p = (Byte *)mem_alloc(a, (size_t)blocks * TAR_BLOCK_SIZE, 1);
    Int e = 0;
    for (Int blk = 0; blk < blocks; blk++) {
        Byte *b = p + blk * TAR_BLOCK_SIZE;
        Byte *sp = blk == 0 ? b + TAR_GNU_SPARSE : b;
        Int max = blk == 0 ? 4 : 21;
        for (Int i = 0; i < max && e < count; i++, e++) {
            Error ferr = BURROW_NO_ERROR;
            burrow__tar_format_numeric(sp + i * 24, 12, (int64_t)e * 2, &ferr);
            burrow__tar_format_numeric(sp + i * 24 + 12, 12, (int64_t)e * 2 + 1, &ferr);
        }
        if (e < count)
            sp[max * 24] = 0x80;
        if (blk == 0)
            burrow__tar_block_set_format(b, TAR_FORMAT_GNU);
    }
    return const_bytes(p, blocks * TAR_BLOCK_SIZE);
}

static void TestReadOldGNUSparseMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestReadOldGNUSparseMap); i++) {
        const TestReadOldGNUSparseMap_v *v = &gen_TestReadOldGNUSparseMap[i];
        Slice input;
        if (v->input.zn < 0) {
            input = old_gnu_sparse_input(a, 1 << 20);
            if (input.len != (Int)v->input.n) {
                testing_t_errorf_v(t, "test %d: made %d bytes of input, want %d", i,
                                   input.len, v->input.n);
                continue;
            }
        } else {
            input = gbytes(a, v->input);
        }
        Byte blk[TAR_BLOCK_SIZE] = {0};
        Int n = input.len < TAR_BLOCK_SIZE ? input.len : TAR_BLOCK_SIZE;
        if (n > 0)
            memcpy(blk, input.p, (size_t)n);
        BytesReader br;
        bytes_reader_reset(&br, slice_sub(input, n, input.len));
        Budget b = {1LL << 40, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        TarReader tr;
        memset(&tr, 0, sizeof tr);
        tr.a = &al;
        tr.r = bytes_reader_as_io_reader(&br);
        TarHeader hdr;
        memset(&hdr, 0, sizeof hdr);
        TarSparseList got = {NULL, 0, 0};
        Error err = burrow__tar_read_old_gnu_sparse_map(&tr, &hdr, blk, &got);
        if (!sparse_equal(got.p, got.len, v->wantMap))
            testing_t_errorf_v(
                t, "test %d, readOldGNUSparseMap(): got %d entries, want %d", i,
                got.len, v->wantMap.n);
        if (gerr_of(err) != v->wantErr)
            testing_t_errorf_v(t, "test %d, readOldGNUSparseMap() = %v, want %s", i,
                               err,
                               str_from_bytes(gerr_label(v->wantErr),
                                              (Int)strlen(gerr_label(v->wantErr))));
        if (hdr.size != v->wantSize)
            testing_t_errorf_v(t, "test %d, Header.Size = %d, want %d", i, hdr.size,
                               v->wantSize);
        burrow__tar_sparse_list_free(&al, &got);
        if (b.live != 0)
            testing_t_errorf_v(t, "test %d: %d bytes leaked", i, (int64_t)b.live);
    }
    arena_free(&ar);
}

static void TestReadGNUSparsePAXHeaders(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestReadGNUSparsePAXHeaders); i++) {
        const TestReadGNUSparsePAXHeaders_v *v = &gen_TestReadGNUSparsePAXHeaders[i];
        Budget b = {1LL << 40, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        TarHeader hdr;
        memset(&hdr, 0, sizeof hdr);
        hdr.pax_records = gmap(&al, v->inputHdrs);
        Slice in = cat(a, gbytes(a, v->inputData), const_bytes("#", 1));
        BytesReader br;
        bytes_reader_reset(&br, in);
        TarReader tr;
        memset(&tr, 0, sizeof tr);
        tr.a = &al;
        tr.r = bytes_reader_as_io_reader(&br);
        tr.curr = (TarFileReader){tr.r, (int64_t)in.len, NULL, 0, 0};
        TarSparseList got = {NULL, 0, 0};
        Error err;
        burrow__tar_read_gnu_sparse_pax_headers(&tr, &hdr, &got, &err);
        if (!sparse_equal(got.p, got.len, v->wantMap))
            testing_t_errorf_v(
                t, "test %d, readGNUSparsePAXHeaders(): got %d entries, want %d", i,
                got.len, v->wantMap.n);
        if (gerr_of(err) != v->wantErr)
            testing_t_errorf_v(t, "test %d, readGNUSparsePAXHeaders() = %v, want %s", i,
                               err,
                               str_from_bytes(gerr_label(v->wantErr),
                                              (Int)strlen(gerr_label(v->wantErr))));
        if (hdr.size != v->wantSize)
            testing_t_errorf_v(t, "test %d, Header.Size = %d, want %d", i, hdr.size,
                               v->wantSize);
        Str wn = gstr(a, v->wantName);
        if (!str_eq(hdr.name, wn))
            testing_t_errorf_v(t, "test %d, Header.Name = %s, want %s", i, hdr.name,
                               wn);
        if (v->wantErr == GERR_NIL && bytes_reader_len(&br) == 0)
            testing_t_errorf_v(t, "test %d, canary byte unexpectedly consumed", i);
        burrow__tar_sparse_list_free(&al, &got);
        if (hdr.name.len > 0)
            mem_free(&al, (void *)(uintptr_t)hdr.name.p, (size_t)hdr.name.len, 1);
        burrow__tar_str_map_free(&al, hdr.pax_records);
        if (b.live != 0)
            testing_t_errorf_v(t, "test %d: %d bytes leaked", i, (int64_t)b.live);
    }
    arena_free(&ar);
}

static void TestFileReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestFileReader); i++) {
        const TestFileReader_v *v = &gen_TestFileReader[i];
        StringsReader sr;
        IoReader inner = strings_reader_as_io_reader(&sr);
        IoReader r = {&non_empty_reader_vt, &inner};
        TarFileReader fr;
        if (strcmp(v->maker.type, "TestFileReader_makeReg") == 0) {
            const TestFileReader_makeReg *m =
                (const TestFileReader_makeReg *)v->maker.v;
            strings_reader_reset(&sr, gstr(a, m->str));
            fr = (TarFileReader){r, m->size, NULL, 0, 0};
        } else {
            const TestFileReader_makeSparse *m =
                (const TestFileReader_makeSparse *)v->maker.v;
            TarSparseEntry *sp = sparse_copy(a, m->spd);
            if (!burrow__tar_validate_sparse_entries(sp, (Int)m->spd.n, m->size)) {
                testing_t_errorf_v(t, "test %d, invalid sparse map", i);
                continue;
            }
            Int nh = burrow__tar_invert_sparse_entries(sp, (Int)m->spd.n, m->size);
            strings_reader_reset(&sr, gstr(a, m->makeReg.str));
            fr = (TarFileReader){r, m->makeReg.size, sp, nh, 0};
        }
        for (int64_t j = 0; j < v->tests.n; j++) {
            const GAny *tf = &v->tests.p[j];
            if (strcmp(tf->type, "TestFileReader_testRead") == 0) {
                const TestFileReader_testRead *op =
                    (const TestFileReader_testRead *)tf->v;
                Slice b = slice_from(mem_alloc(a, (size_t)op->cnt + 1, 1), (Int)op->cnt,
                                     (Int)op->cnt, TYPE_BYTE);
                Error err;
                Int n = burrow__tar_file_reader_read(&fr, b, &err);
                Str got = str_from_bytes(b.p, n), want = gstr(a, op->wantStr);
                if (!str_eq(got, want) || gerr_of(err) != op->wantErr)
                    testing_t_errorf_v(
                        t, "test %d.%d, Read(%d): got (%q, %v), want (%q, %s)", i, j,
                        op->cnt, got, err, want,
                        str_from_bytes(gerr_label(op->wantErr),
                                       (Int)strlen(gerr_label(op->wantErr))));
            } else if (strcmp(tf->type, "TestFileReader_testWriteTo") == 0) {
                const TestFileReader_testWriteTo *op =
                    (const TestFileReader_testWriteTo *)tf->v;
                TestFile f;
                test_file_init(a, &f, op->ops);
                Error err;
                int64_t got = burrow__tar_file_reader_write_to(
                    &fr, a, (IoWriter){&test_file_writer_vt, &f}, &err);
                if (f.test_err)
                    testing_t_errorf_v(t, "test %d.%d, WriteTo(): %s", i, j,
                                       str_from_bytes(f.why, (Int)strlen(f.why)));
                else if (got != op->wantCnt || gerr_of(err) != op->wantErr)
                    testing_t_errorf_v(
                        t, "test %d.%d, WriteTo() = (%d, %v), want (%d, %s)", i, j, got,
                        err, op->wantCnt,
                        str_from_bytes(gerr_label(op->wantErr),
                                       (Int)strlen(gerr_label(op->wantErr))));
                if (f.n > 0)
                    testing_t_errorf_v(t, "test %d.%d, expected %d more operations", i,
                                       j, f.n);
            } else if (strcmp(tf->type, "TestFileReader_testRemaining") == 0) {
                const TestFileReader_testRemaining *op =
                    (const TestFileReader_testRemaining *)tf->v;
                int64_t got = burrow__tar_file_reader_logical_remaining(&fr);
                if (got != op->wantLCnt)
                    testing_t_errorf_v(t,
                                       "test %d.%d, logicalRemaining() = %d, want %d",
                                       i, j, got, op->wantLCnt);
                got = burrow__tar_file_reader_physical_remaining(&fr);
                if (got != op->wantPCnt)
                    testing_t_errorf_v(t,
                                       "test %d.%d, physicalRemaining() = %d, want %d",
                                       i, j, got, op->wantPCnt);
            } else {
                testing_t_errorf_v(t, "test %d.%d, unknown test operation", i, j);
            }
        }
    }
    arena_free(&ar);
}

/* Writes a header for each name and then reads the archive back, with
 * GODEBUG as given. */
static void TestInsecurePaths(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__tar_godebug_set("tarinsecurepath=0");
    static const char *const paths[] = {"../foo", "/foo", "a/b/../../../c"};
    for (Int i = 0; i < LEN(paths); i++) {
        Str path = str_from_bytes(paths[i], (Int)strlen(paths[i]));
        BytesBuffer buf = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
        TarHeader h1 = {.name = path};
        (void)tar_writer_write_header(tw, &h1);
        Str secure = BURROW_S("secure");
        TarHeader h2 = {.name = secure};
        (void)tar_writer_write_header(tw, &h2);
        (void)tar_writer_close(tw);

        TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buf));
        Error err;
        TarHeader *h = tar_reader_next(tr, &err);
        if (!same_error(err, tar_err_insecure_path)) {
            testing_t_errorf_v(
                t, "tr.Next for file %q: got err %v, want ErrInsecurePath", path, err);
            continue;
        }
        if (!str_eq(h->name, path))
            testing_t_errorf_v(t, "tr.Next for file %q: got name %q, want %q", path,
                               h->name, path);
        h = tar_reader_next(tr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "tr.Next for file %q: got err %v, want nil", secure,
                               err);
            continue;
        }
        if (!str_eq(h->name, secure))
            testing_t_errorf_v(t, "tr.Next for file %q: got name %q, want %q", secure,
                               h->name, secure);
    }
    burrow__tar_godebug_set(NULL);
    arena_free(&ar);
}

static void TestDisableInsecurePathCheck(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__tar_godebug_set("tarinsecurepath=1");
    BytesBuffer buf = BYTES_BUFFER(a);
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Str name = BURROW_S("/foo");
    TarHeader h1 = {.name = name};
    (void)tar_writer_write_header(tw, &h1);
    (void)tar_writer_close(tw);
    TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buf));
    Error err;
    TarHeader *h = tar_reader_next(tr, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "tr.Next with tarinsecurepath=1: got err %v, want nil",
                           err);
    else if (!str_eq(h->name, name))
        testing_t_errorf_v(t, "tr.Next with tarinsecurepath=1: got name %q, want %q",
                           h->name, name);
    burrow__tar_godebug_set(NULL);
    arena_free(&ar);
}

/* ------------------------------------------------------------ tar_test.go */

static void TestSparseEntries(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestSparseEntries); i++) {
        const TestSparseEntries_v *v = &gen_TestSparseEntries[i];
        TarSparseEntry *in = sparse_copy(a, v->in);
        bool got_valid = burrow__tar_validate_sparse_entries(in, (Int)v->in.n, v->size);
        if (got_valid != v->wantValid)
            testing_t_errorf_v(t, "test %d, validateSparseEntries() = %v, want %v", i,
                               got_valid, v->wantValid);
        if (!v->wantValid)
            continue;
        TarSparseEntry *aligned = sparse_copy(a, v->in);
        Int n = burrow__tar_align_sparse_entries(aligned, (Int)v->in.n, v->size);
        if (!sparse_equal(aligned, n, v->wantAligned))
            testing_t_errorf_v(t,
                               "test %d, alignSparseEntries(): got %d entries, want %d",
                               i, n, v->wantAligned.n);
        TarSparseEntry *inverted = sparse_copy(a, v->in);
        n = burrow__tar_invert_sparse_entries(inverted, (Int)v->in.n, v->size);
        if (!sparse_equal(inverted, n, v->wantInverted))
            testing_t_errorf_v(
                t, "test %d, inverseSparseEntries(): got %d entries, want %d", i, n,
                v->wantInverted.n);
    }
    arena_free(&ar);
}

/* os.Stat on testdata/small.txt and testdata, from a MapFS standing in for
 * the directory. */
static FstestMapFS small_fs(Alloc *a) {
    static FstestMapFile dir = {
        {NULL, 0, 0, NULL}, FS_MODE_DIR | 0755, {0}, {NULL, NULL}};
    static FstestMapFile small = {{NULL, 0, 0, NULL}, 0644, {0}, {NULL, NULL}};
    small.data = const_bytes("Kilts", 5);
    small.mod_time = time_from_unix(1244428340, 0);
    dir.mod_time = time_from_unix(1244428340, 0);
    FstestMapFS fsys = fstest_map_fs_make(a);
    fstest_map_fs_set(fsys, BURROW_S("testdata"), &dir);
    fstest_map_fs_set(fsys, BURROW_S("testdata/small.txt"), &small);
    return fsys;
}

static TarHeader *small_header(TestingT *t, Alloc *a) {
    Error err;
    FsFileInfo fi =
        fstest_map_fs_stat(small_fs(a), a, BURROW_S("testdata/small.txt"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "os.Stat: %v", err);
        return NULL;
    }
    TarHeader *hdr = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "FileInfoHeader: %v", err);
        return NULL;
    }
    return hdr;
}

static void TestFileInfoHeader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    FsFileInfo fi =
        fstest_map_fs_stat(small_fs(a), a, BURROW_S("testdata/small.txt"), &err);
    TarHeader *h = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "FileInfoHeader: %v", err);
        arena_free(&ar);
        return;
    }
    if (!str_eq(h->name, BURROW_S("small.txt")))
        testing_t_errorf_v(t, "Name = %q; want %q", h->name, BURROW_S("small.txt"));
    int64_t perm = (int64_t)(fi.vt->mode(fi.data) & 0777);
    if (h->mode != perm)
        testing_t_errorf_v(t, "Mode = %#o; want %#o", h->mode, perm);
    if (h->size != 5)
        testing_t_errorf_v(t, "Size = %v; want %v", h->size, (int64_t)5);
    if (!time_equal(h->mod_time, fi.vt->mod_time(fi.data)))
        testing_t_errorf_v(t, "ModTime differs");
    tar_file_info_header(a, (FsFileInfo){NULL, NULL}, (Str){NULL, 0}, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "Expected error when passing nil to FileInfoHeader");
    arena_free(&ar);
}

static void TestFileInfoHeaderDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    FsFileInfo fi = fstest_map_fs_stat(small_fs(a), a, BURROW_S("testdata"), &err);
    TarHeader *h = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "FileInfoHeader: %v", err);
        arena_free(&ar);
        return;
    }
    if (!str_eq(h->name, BURROW_S("testdata/")))
        testing_t_errorf_v(t, "Name = %q; want %q", h->name, BURROW_S("testdata/"));
    int64_t perm = (int64_t)(fi.vt->mode(fi.data) & 0777);
    if ((h->mode & ~(int64_t)02000) != perm)
        testing_t_errorf_v(t, "Mode = %#o; want %#o", h->mode, perm);
    if (h->size != 0)
        testing_t_errorf_v(t, "Size = %v; want %v", h->size, (int64_t)0);
    if (!time_equal(h->mod_time, fi.vt->mod_time(fi.data)))
        testing_t_errorf_v(t, "ModTime differs");
    arena_free(&ar);
}

static void TestFileInfoHeaderSymlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str target = BURROW_S("/tmp/tardir");
    FstestMapFile link = {str_slice(target), FS_MODE_SYMLINK | 0777, {0}, {NULL, NULL}};
    FstestMapFS fsys = fstest_map_fs_make(a);
    fstest_map_fs_set(fsys, BURROW_S("link"), &link);
    Error err;
    FsFileInfo fi = fstest_map_fs_lstat(fsys, a, BURROW_S("link"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Lstat: %v", err);
        arena_free(&ar);
        return;
    }
    TarHeader *h = tar_file_info_header(a, fi, target, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "FileInfoHeader: %v", err);
        arena_free(&ar);
        return;
    }
    if (!str_eq(h->name, fi.vt->name(fi.data)))
        testing_t_errorf_v(t, "Name = %q; want %q", h->name, fi.vt->name(fi.data));
    if (!str_eq(h->linkname, target))
        testing_t_errorf_v(t, "Linkname = %q; want %q", h->linkname, target);
    if (h->typeflag != TAR_TYPE_SYMLINK)
        testing_t_errorf_v(t, "Typeflag = %v; want %v", (int64_t)h->typeflag,
                           (int64_t)TAR_TYPE_SYMLINK);
    arena_free(&ar);
}

static void TestRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = const_bytes("some file contents", 18);
    BytesBuffer b = BYTES_BUFFER(a);
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
    Map *pax = burrow__tar_str_map(a);
    burrow__tar_str_map_set(a, pax, BURROW_S("uid"), BURROW_S("2097152"));
    TarHeader hdr = {
        .name = BURROW_S("file.txt"),
        .uid = 1 << 21,
        .size = data.len,
        .mod_time = time_round(time_now(), TIME_SECOND),
        .pax_records = pax,
        .format = TAR_FORMAT_PAX,
        .typeflag = TAR_TYPE_REG,
    };
    Error err = tar_writer_write_header(tw, &hdr);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "tw.WriteHeader: %v", err);
    tar_writer_write(tw, data, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "tw.Write: %v", err);
    err = tar_writer_close(tw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "tw.Close: %v", err);

    TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&b));
    TarHeader *rhdr = tar_reader_next(tr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "tr.Next: %v", err);
        arena_free(&ar);
        return;
    }
    const char *why;
    if (!headers_equal(rhdr, &hdr, &why))
        testing_t_errorf_v(t, "Header mismatch: %s differs",
                           str_from_bytes(why, (Int)strlen(why)));
    Slice rdata = io_read_all(a, tar_reader_as_io_reader(tr), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read: %v", err);
    if (!str_eq(slice_str(rdata), slice_str(data)))
        testing_t_errorf_v(t, "Data mismatch.\n got %q\nwant %q", slice_str(rdata),
                           slice_str(data));
    arena_free(&ar);
}

static Str join2(Alloc *a, Str x, Str y) {
    Byte *p = mem_alloc(a, (size_t)(x.len + y.len), 1);
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static void TestHeaderRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestHeaderRoundTrip); i++) {
        const TestHeaderRoundTrip_headerRoundTripTest *v = &gen_TestHeaderRoundTrip[i];
        TarHeader *h = gheader(a, v->h);
        FsFileInfo fi = tar_header_file_info(h);
        Error err;
        TarHeader *h2 = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            continue;
        }
        Str fname = fi.vt->name(fi.data);
        if (strings_contains(fname, BURROW_S("/")))
            testing_t_errorf_v(t, "FileInfo of %q contains slash: %q", h->name, fname);
        Str name = path_base(h->name);
        if (fi.vt->is_dir(fi.data))
            name = join2(a, name, BURROW_S("/"));
        if (!str_eq(h2->name, name))
            testing_t_errorf_v(t, "i=%d: Name: got %v, want %v", i, h2->name, name);
        if (h2->size != h->size)
            testing_t_errorf_v(t, "i=%d: Size: got %v, want %v", i, h2->size, h->size);
        if (h2->uid != h->uid)
            testing_t_errorf_v(t, "i=%d: Uid: got %d, want %d", i, h2->uid, h->uid);
        if (h2->gid != h->gid)
            testing_t_errorf_v(t, "i=%d: Gid: got %d, want %d", i, h2->gid, h->gid);
        if (!str_eq(h2->uname, h->uname))
            testing_t_errorf_v(t, "i=%d: Uname: got %q, want %q", i, h2->uname,
                               h->uname);
        if (!str_eq(h2->gname, h->gname))
            testing_t_errorf_v(t, "i=%d: Gname: got %q, want %q", i, h2->gname,
                               h->gname);
        if (!str_eq(h2->linkname, h->linkname))
            testing_t_errorf_v(t, "i=%d: Linkname: got %v, want %v", i, h2->linkname,
                               h->linkname);
        if (h2->typeflag != h->typeflag)
            testing_t_errorf_v(t, "i=%d: Typeflag: got %q, want %q", i,
                               (Rune)h2->typeflag, (Rune)h->typeflag);
        if (h2->mode != h->mode)
            testing_t_errorf_v(t, "i=%d: Mode: got %o, want %o", i, h2->mode, h->mode);
        FsFileMode fm = fi.vt->mode(fi.data);
        if ((int64_t)fm != v->fm)
            testing_t_errorf_v(t, "i=%d: fi.Mode: got %o, want %o", i, (int64_t)fm,
                               v->fm);
        if (!times_equal(h2->access_time, h->access_time))
            testing_t_errorf_v(t, "i=%d: AccessTime differs", i);
        if (!times_equal(h2->change_time, h->change_time))
            testing_t_errorf_v(t, "i=%d: ChangeTime differs", i);
        if (!times_equal(h2->mod_time, h->mod_time))
            testing_t_errorf_v(t, "i=%d: ModTime differs", i);
        Any sys = fi.vt->sys(fi.data);
        if (sys.t != TYPE_TAR_HEADER || sys.data != h)
            testing_t_errorf_v(t, "i=%d: Sys didn't return original *Header", i);
    }
    arena_free(&ar);
}

static void TestHeaderAllowedFormats(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestHeaderAllowedFormats); i++) {
        const TestHeaderAllowedFormats_v *v = &gen_TestHeaderAllowedFormats[i];
        TarHeader *h = gheader(a, v->header);
        Map *pax = NULL;
        Error err = BURROW_NO_ERROR;
        TarFormat formats = burrow__tar_allowed_formats(a, h, &pax, &err);
        if (formats != (TarFormat)v->formats)
            testing_t_errorf_v(t, "test %d, allowedFormats(): got %s, want %s", i,
                               tar_format_string(formats, a),
                               tar_format_string((TarFormat)v->formats, a));
        Map *want = gmap(a, v->paxHdrs);
        if ((formats & TAR_FORMAT_PAX) != 0 && !str_maps_equal(pax, want) &&
            !(map_len(pax) == 0 && map_len(want) == 0))
            testing_t_errorf_v(t, "test %d, allowedFormats(): PAX headers differ", i);
        if (formats != TAR_FORMAT_UNKNOWN && BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d, unexpected error: %v", i, err);
        if (formats == TAR_FORMAT_UNKNOWN && BURROW_OK(err))
            testing_t_errorf_v(t, "test %d, got nil-error, want non-nil error", i);
    }
    arena_free(&ar);
}

/* fileInfoNames: a FileInfo with the Uname and Gname methods. */
typedef struct FileInfoNames {
    int unused;
} FileInfoNames;

static Str fin_gname(FileInfoNames *fi, IoErrorArg err) {
    (void)fi;
    *err = BURROW_NO_ERROR;
    return BURROW_S("Gname");
}

static Str fin_uname(FileInfoNames *fi, IoErrorArg err) {
    (void)fi;
    *err = BURROW_NO_ERROR;
    return BURROW_S("Uname");
}

#define FILE_INFO_NAMES_METHODS(M, T)                                                  \
    M(T, Gname, fin_gname, TAR_SIG_FILE_INFO_NAME)                                     \
    M(T, Uname, fin_uname, TAR_SIG_FILE_INFO_NAME)
BURROW_METHODS_DEFINE(FileInfoNames, FILE_INFO_NAMES_METHODS);

static const Type file_info_names_type = {
    {(const Byte *)"fileInfoNames", 13},
    {(const Byte *)"tar_test", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(FileInfoNames),
    (uint16_t)_Alignof(FileInfoNames),
    0,
    (uint16_t)(sizeof burrow__methods_FileInfoNames /
               sizeof burrow__methods_FileInfoNames[0]),
    NULL,
    burrow__methods_FileInfoNames,
    NULL,
    NULL,
    0,
    0x66696e6eU,
    NULL,
};

static Str fin_name(void *self) {
    (void)self;
    return BURROW_S("tmp");
}
static int64_t fin_size(void *self) {
    (void)self;
    return 0;
}
static FsFileMode fin_mode(void *self) {
    (void)self;
    return 0777;
}
static Time fin_mod_time(void *self) {
    (void)self;
    return (Time){0};
}
static bool fin_is_dir(void *self) {
    (void)self;
    return false;
}
static Any fin_sys(void *self) {
    (void)self;
    return (Any){NULL, NULL};
}

static const FsFileInfoVT file_info_names_vt = {
    &file_info_names_type, fin_name,   fin_size, fin_mode,
    fin_mod_time,          fin_is_dir, fin_sys,
};

static void TestFileInfoHeaderUseFileInfoNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FileInfoNames info = {0};
    Error err;
    TarHeader *h = tar_file_info_header(a, (FsFileInfo){&file_info_names_vt, &info},
                                        (Str){NULL, 0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
    } else {
        if (!str_eq(h->uname, BURROW_S("Uname")))
            testing_t_fatalf_v(t, "header.Uname: got %s, want %s", h->uname,
                               BURROW_S("Uname"));
        if (!str_eq(h->gname, BURROW_S("Gname")))
            testing_t_fatalf_v(t, "header.Gname: got %s, want %s", h->gname,
                               BURROW_S("Gname"));
    }
    arena_free(&ar);
}

/* --------------------------------------------------------- writer_test.go */

/* equalError: headerErrors are all alike and the rest compare as values. */
static bool equal_error(Error err, GErr want) {
    GErr got = gerr_of(err);
    if (got == GERR_HEADER_ERROR || want == GERR_HEADER_ERROR)
        return got == want;
    return got == want && got != GERR_OTHER;
}

static Str gerr_str(GErr g) {
    const char *s = gerr_label(g);
    return str_from_bytes(s, (Int)strlen(s));
}

static void writer_case(TestingT *t, const TestWriter_v *v) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    TruncateWriter trunc = {bytes_buffer_as_io_writer(&buf), 10 << 10};
    TarWriter *tw = tar_new_writer(a, (IoWriter){&truncate_writer_vt, &trunc});
    bool fatal = false;
    for (int64_t i = 0; i < v->tests.n && !fatal; i++) {
        const GAny *tf = &v->tests.p[i];
        if (strcmp(tf->type, "TestWriter_testHeader") == 0) {
            const TestWriter_testHeader *op = (const TestWriter_testHeader *)tf->v;
            Error err = tar_writer_write_header(tw, gheader(a, &op->hdr));
            if (!equal_error(err, op->wantErr)) {
                testing_t_errorf_v(t, "test %d, WriteHeader() = %v, want %s", i, err,
                                   gerr_str(op->wantErr));
                fatal = true;
            }
        } else if (strcmp(tf->type, "TestWriter_testWrite") == 0) {
            const TestWriter_testWrite *op = (const TestWriter_testWrite *)tf->v;
            Error err;
            Int got = tar_writer_write(tw, gbytes(a, op->str), &err);
            if (got != op->wantCnt || !equal_error(err, op->wantErr)) {
                testing_t_errorf_v(t, "test %d, Write() = (%d, %v), want (%d, %s)", i,
                                   got, err, op->wantCnt, gerr_str(op->wantErr));
                fatal = true;
            }
        } else if (strcmp(tf->type, "TestWriter_testClose") == 0) {
            const TestWriter_testClose *op = (const TestWriter_testClose *)tf->v;
            Error err = tar_writer_close(tw);
            if (!equal_error(err, op->wantErr)) {
                testing_t_errorf_v(t, "test %d, Close() = %v, want %s", i, err,
                                   gerr_str(op->wantErr));
                fatal = true;
            }
        } else {
            testing_t_errorf_v(t, "test %d, unknown test operation", i);
            fatal = true;
        }
    }
    if (!fatal && v->file.n > 0) {
        Str file = gstr(a, v->file);
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)file.len, (const char *)file.p);
        Slice want = testdata(a, name);
        Slice got = bytes_buffer_bytes(&buf);
        if (!str_eq(slice_str(got), slice_str(want))) {
            Int off = 0;
            while (off < got.len && off < want.len &&
                   ((Byte *)got.p)[off] == ((Byte *)want.p)[off])
                off++;
            testing_t_errorf_v(
                t, "incorrect result: got %d bytes, want %d, first difference at %d",
                got.len, want.len, off);
        }
    }
    tar_writer_free(tw);
    arena_free(&ar);
}

static void writer_subtest(void *env, TestingT *t) {
    writer_case(t, (const TestWriter_v *)env);
}

static void TestWriter(TestingT *t) {
    for (Int i = 0; i < LEN(gen_TestWriter); i++) {
        Str file = {(const Byte *)gen_TestWriter[i].file.p,
                    (Int)gen_TestWriter[i].file.n};
        Str base = path_base(file);
        if (strings_has_suffix(base, BURROW_S(".base64")))
            base.len -= 7;
        testing_t_run(t, base,
                      BURROW_FN(TestingTFunc, writer_subtest,
                                (void *)(uintptr_t)&gen_TestWriter[i]));
    }
}

/* Writes small.txt's header, changed by edit, and contents, and gives back the
 * archive. */
static Slice write_small(TestingT *t, Alloc *a, TarHeader *hdr, Slice contents) {
    BytesBuffer buf = BYTES_BUFFER(a);
    TarWriter *w = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err = tar_writer_write_header(w, hdr);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        return (Slice){NULL, 0, 0, NULL};
    }
    if (contents.len > 0) {
        tar_writer_write(w, contents, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%v", err);
            return (Slice){NULL, 0, 0, NULL};
        }
    }
    err = tar_writer_close(w);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        return (Slice){NULL, 0, 0, NULL};
    }
    return bytes_buffer_bytes(&buf);
}

static TarHeader *read_first(TestingT *t, Alloc *a, Slice archive, bool insecure_ok) {
    BytesReader br;
    bytes_reader_reset(&br, archive);
    TarReader *tr = tar_new_reader(a, bytes_reader_as_io_reader(&br));
    Error err;
    TarHeader *h = tar_reader_next(tr, &err);
    if (BURROW_FAILED(err) &&
        !(insecure_ok && same_error(err, tar_err_insecure_path))) {
        testing_t_fatalf_v(t, "%v", err);
        return NULL;
    }
    return h;
}

static bool has_pax_header(Slice archive) {
    return bytes_contains(archive, const_bytes("PaxHeaders.0", 12));
}

static void TestPax(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        Str long_name = repeat(a, "ab", 100);
        Str contents = repeat(a, " ", (Int)hdr->size);
        hdr->name = long_name;
        Slice out = write_small(t, a, hdr, str_slice(contents));
        if (out.p != NULL) {
            if (!has_pax_header(out))
                testing_t_fatalf_v(t,
                                   "Expected at least one PAX header to be written.");
            TarHeader *h = read_first(t, a, out, false);
            if (h != NULL && !str_eq(h->name, long_name))
                testing_t_fatalf_v(t, "Couldn't recover long file name");
        }
    }
    arena_free(&ar);
}

static void TestPaxSymlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        hdr->typeflag = TAR_TYPE_SYMLINK;
        Str long_linkname = repeat(a, "1234567890/1234567890", 10);
        hdr->linkname = long_linkname;
        hdr->size = 0;
        Slice out = write_small(t, a, hdr, (Slice){NULL, 0, 0, NULL});
        if (out.p != NULL) {
            if (!has_pax_header(out))
                testing_t_fatalf_v(t,
                                   "Expected at least one PAX header to be written.");
            TarHeader *h = read_first(t, a, out, false);
            if (h != NULL && !str_eq(h->linkname, long_linkname))
                testing_t_fatalf_v(t, "Couldn't recover long link name");
        }
    }
    arena_free(&ar);
}

static void TestPaxNonAscii(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        Str file_name = BURROW_S("\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d");
        Str group_name = BURROW_S("\xe7\xb5\x84");
        Str user_name = BURROW_S("\xe7\x94\xa8\xe6\x88\xb6\xe5\x90\x8d");
        hdr->name = file_name;
        hdr->gname = group_name;
        hdr->uname = user_name;
        Str contents = repeat(a, " ", (Int)hdr->size);
        Slice out = write_small(t, a, hdr, str_slice(contents));
        if (out.p != NULL) {
            if (!has_pax_header(out))
                testing_t_fatalf_v(t,
                                   "Expected at least one PAX header to be written.");
            TarHeader *h = read_first(t, a, out, false);
            if (h != NULL) {
                if (!str_eq(h->name, file_name))
                    testing_t_fatalf_v(t, "Couldn't recover unicode name");
                if (!str_eq(h->gname, group_name))
                    testing_t_fatalf_v(t, "Couldn't recover unicode group");
                if (!str_eq(h->uname, user_name))
                    testing_t_fatalf_v(t, "Couldn't recover unicode user");
            }
        }
    }
    arena_free(&ar);
}

static void TestPaxXattrs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        Map *xattrs = burrow__tar_str_map(a);
        burrow__tar_str_map_set(a, xattrs, BURROW_S("user.key"), BURROW_S("value"));
        hdr->xattrs = xattrs;
        Slice out = write_small(t, a, hdr, const_bytes("Kilts", 5));
        if (out.p != NULL) {
            TarHeader *h = read_first(t, a, out, false);
            if (h != NULL && !str_maps_equal(h->xattrs, xattrs))
                testing_t_fatalf_v(t, "xattrs did not survive round trip");
        }
    }
    arena_free(&ar);
}

static void TestPaxHeadersSorted(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        Str contents = repeat(a, " ", (Int)hdr->size);
        Map *xattrs = burrow__tar_str_map(a);
        static const char *const kv[] = {"foo", "bar", "baz", "qux"};
        for (int i = 0; i < 4; i++)
            burrow__tar_str_map_set(a, xattrs, str_from_bytes(kv[i], 3),
                                    str_from_bytes(kv[i], 3));
        hdr->xattrs = xattrs;
        Slice out = write_small(t, a, hdr, str_slice(contents));
        if (out.p != NULL) {
            if (!has_pax_header(out))
                testing_t_fatalf_v(t,
                                   "Expected at least one PAX header to be written.");
            Int idx[4] = {
                bytes_index(out, const_bytes("bar=bar", 7)),
                bytes_index(out, const_bytes("baz=baz", 7)),
                bytes_index(out, const_bytes("foo=foo", 7)),
                bytes_index(out, const_bytes("qux=qux", 7)),
            };
            for (int i = 1; i < 4; i++)
                if (idx[i - 1] > idx[i]) {
                    testing_t_fatalf_v(t, "PAX headers are not sorted");
                    break;
                }
        }
    }
    arena_free(&ar);
}

static void TestUSTARLongName(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TarHeader *hdr = small_header(t, a);
    if (hdr != NULL) {
        hdr->typeflag = TAR_TYPE_DIR;
        Str long_name = BURROW_S("/0000_0000000/00000-000000000/0000_0000000/"
                                 "00000-0000000000000/0000_0000000/"
                                 "00000-0000000-00000000/0000_0000000/00000000/"
                                 "0000_0000000/000/0000_0000000/"
                                 "00000000v00/0000_0000000/000000/0000_0000000/0000000/"
                                 "0000_0000000/00000y-00/"
                                 "0000/0000/00000000/0x000000/");
        hdr->name = long_name;
        hdr->size = 0;
        Slice out = write_small(t, a, hdr, (Slice){NULL, 0, 0, NULL});
        if (out.p != NULL) {
            TarHeader *h = read_first(t, a, out, true);
            if (h != NULL && !str_eq(h->name, long_name))
                testing_t_fatalf_v(t, "Couldn't recover long name");
        }
    }
    arena_free(&ar);
}

static void TestValidTypeflagWithPAXHeader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buffer = BYTES_BUFFER(a);
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buffer));
    TarHeader hdr = {.name = repeat(a, "ab", 100), .size = 4, .typeflag = 0};
    Error err = tar_writer_write_header(tw, &hdr);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Failed to write header: %v", err);
    tar_writer_write(tw, const_bytes("fooo", 4), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Failed to write the file's data: %v", err);
    (void)tar_writer_close(tw);
    TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buffer));
    for (;;) {
        TarHeader *h = tar_reader_next(tr, &err);
        if (same_error(err, io_eof))
            break;
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "Failed to read header: %v", err);
            break;
        }
        if (h->typeflag != TAR_TYPE_REG) {
            testing_t_fatalf_v(t, "Typeflag should've been %d, found %d",
                               (int64_t)TAR_TYPE_REG, (int64_t)h->typeflag);
            break;
        }
    }
    arena_free(&ar);
}

/* failOnceWriter, which as Go's is written fails every time. */
static Int fail_once_write(void *self, Slice b, Error *err) {
    (void)self;
    (void)b;
    *err = io_err_short_write;
    return 0;
}

static const IoWriterVT fail_once_writer_vt = {NULL, fail_once_write};

static void TestWriterErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Str small = BURROW_S("small.txt");

    /* HeaderOnly */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = BURROW_S("dir/"), .typeflag = TAR_TYPE_DIR};
        err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "HeaderOnly: WriteHeader() = %v, want nil", err);
        tar_writer_write(tw, const_bytes("\0", 1), &err);
        if (!same_error(err, tar_err_write_too_long))
            testing_t_errorf_v(t, "HeaderOnly: Write() = %v, want %v", err,
                               tar_err_write_too_long);
    }
    /* NegativeSize */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = small, .size = -1};
        if (BURROW_OK(tar_writer_write_header(tw, &hdr)))
            testing_t_errorf_v(t,
                               "NegativeSize: WriteHeader() = nil, want non-nil error");
    }
    /* BeforeHeader */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        tar_writer_write(tw, const_bytes("Kilts", 5), &err);
        if (!same_error(err, tar_err_write_too_long))
            testing_t_errorf_v(t, "BeforeHeader: Write() = %v, want %v", err,
                               tar_err_write_too_long);
    }
    /* AfterClose */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = small};
        err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "AfterClose: WriteHeader() = %v, want nil", err);
        err = tar_writer_close(tw);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "AfterClose: Close() = %v, want nil", err);
        tar_writer_write(tw, const_bytes("Kilts", 5), &err);
        if (!same_error(err, tar_err_write_after_close))
            testing_t_errorf_v(t, "AfterClose: Write() = %v, want %v", err,
                               tar_err_write_after_close);
        err = tar_writer_flush(tw);
        if (!same_error(err, tar_err_write_after_close))
            testing_t_errorf_v(t, "AfterClose: Flush() = %v, want %v", err,
                               tar_err_write_after_close);
        err = tar_writer_close(tw);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "AfterClose: Close() = %v, want nil", err);
    }
    /* PrematureFlush */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = small, .size = 5};
        err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "PrematureFlush: WriteHeader() = %v, want nil", err);
        if (BURROW_OK(tar_writer_flush(tw)))
            testing_t_errorf_v(t, "PrematureFlush: Flush() = nil, want non-nil error");
    }
    /* PrematureClose */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = small, .size = 5};
        err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "PrematureClose: WriteHeader() = %v, want nil", err);
        if (BURROW_OK(tar_writer_close(tw)))
            testing_t_errorf_v(t, "PrematureClose: Close() = nil, want non-nil error");
    }
    /* Persistence */
    {
        TarWriter *tw = tar_new_writer(a, (IoWriter){&fail_once_writer_vt, NULL});
        TarHeader empty = {0};
        err = tar_writer_write_header(tw, &empty);
        if (!same_error(err, io_err_short_write))
            testing_t_errorf_v(t, "Persistence: WriteHeader() = %v, want %v", err,
                               io_err_short_write);
        TarHeader hdr = {.name = small};
        if (BURROW_OK(tar_writer_write_header(tw, &hdr)))
            testing_t_errorf_v(t,
                               "Persistence: WriteHeader() = nil, want non-nil error");
        tar_writer_write(tw, (Slice){NULL, 0, 0, NULL}, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Persistence: Write() = nil, want non-nil error");
        if (BURROW_OK(tar_writer_flush(tw)))
            testing_t_errorf_v(t, "Persistence: Flush() = nil, want non-nil error");
        if (BURROW_OK(tar_writer_close(tw)))
            testing_t_errorf_v(t, "Persistence: Close() = nil, want non-nil error");
    }
    arena_free(&ar);
}

static void TestSplitUSTARPath(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestSplitUSTARPath); i++) {
        const TestSplitUSTARPath_v *v = &gen_TestSplitUSTARPath[i];
        Str in = gstr(a, v->input), wp = gstr(a, v->prefix), ws = gstr(a, v->suffix);
        Str prefix = {NULL, 0}, suffix = {NULL, 0};
        bool ok = burrow__tar_split_ustar_path(in, &prefix, &suffix);
        if (!str_eq(prefix, wp) || !str_eq(suffix, ws) || ok != v->ok)
            testing_t_errorf_v(
                t, "splitUSTARPath(%q):\ngot  (%q, %q, %v)\nwant (%q, %q, %v)", in,
                prefix, suffix, ok, wp, ws, v->ok);
    }
    arena_free(&ar);
}

static void TestIssue12594(TestingT *t) {
    static const char *const names[] = {
        "0/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/"
        "29/30/file.txt",
        "0/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/"
        "29/30/31/32/33/file.txt",
        "0/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/"
        "29/30/31/32/333/file.txt",
        "0/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/"
        "29/30/31/32/33/34/35/36/37/38/39/40/file.txt",
        "000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000/file.txt",
        "/home/support/.openoffice.org/3/user/uno_packages/cache/registry/"
        "com.sun.star.comp.deployment.executable.PackageRegistryBackend",
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(names); i++) {
        Str name = str_from_bytes(names[i], (Int)strlen(names[i]));
        BytesBuffer b = BYTES_BUFFER(a);
        TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&b));
        TarHeader hdr = {.name = name, .uid = 1 << 25};
        Error err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d, unexpected WriteHeader error: %v", i, err);
        err = tar_writer_close(tw);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d, unexpected Close error: %v", i, err);

        Byte blk[TAR_BLOCK_SIZE] = {0};
        Slice out = bytes_buffer_bytes(&b);
        memcpy(blk, out.p, out.len < TAR_BLOCK_SIZE ? (size_t)out.len : TAR_BLOCK_SIZE);
        Str prefix = burrow__tar_parse_string(blk + TAR_USTAR_PREFIX, TAR_PREFIX_SIZE);
        if (burrow__tar_block_get_format(blk) == TAR_FORMAT_GNU && prefix.len > 0 &&
            strings_has_prefix(name, prefix))
            testing_t_errorf_v(t, "test %d, found prefix in GNU format: %s", i, prefix);

        TarHeader *h = read_first(t, a, out, true);
        if (h != NULL && !str_eq(h->name, name))
            testing_t_errorf_v(t, "test %d, hdr.Name = %s, want %s", i, h->name, name);
    }
    arena_free(&ar);
}

static void TestWriteLongHeader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str long_str = repeat(a, "a", TAR_MAX_SPECIAL_FILE_SIZE);
    Map *pax = burrow__tar_str_map(a);
    burrow__tar_str_map_set(a, pax, BURROW_S("GOLANG.x"), long_str);
    const struct {
        const char *name;
        TarHeader h;
    } tests[] = {
        {"name too long", {.name = long_str}},
        {"linkname too long", {.linkname = long_str}},
        {"uname too long", {.uname = long_str}},
        {"gname too long", {.gname = long_str}},
        {"PAX header too long", {.pax_records = pax}},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        TarWriter *w = tar_new_writer(a, io_discard);
        Error err = tar_writer_write_header(w, &tests[i].h);
        if (!same_error(err, tar_err_field_too_long))
            testing_t_errorf_v(
                t, "%s: w.WriteHeader() = %v, want ErrFieldTooLong",
                str_from_bytes(tests[i].name, (Int)strlen(tests[i].name)), err);
    }
    arena_free(&ar);
}

static void TestFileWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(gen_TestFileWriter); i++) {
        const TestFileWriter_v *v = &gen_TestFileWriter[i];
        BytesBuffer bb = BYTES_BUFFER(a);
        IoWriter inner = bytes_buffer_as_io_writer(&bb);
        IoWriter w = {&non_empty_writer_vt, &inner};
        TarFileWriter fw;
        Str want_str;
        if (strcmp(v->maker.type, "TestFileWriter_makeReg") == 0) {
            const TestFileWriter_makeReg *m =
                (const TestFileWriter_makeReg *)v->maker.v;
            fw = (TarFileWriter){w, m->size, NULL, 0, 0};
            want_str = gstr(a, m->wantStr);
        } else {
            const TestFileWriter_makeSparse *m =
                (const TestFileWriter_makeSparse *)v->maker.v;
            TarSparseEntry *sp = sparse_copy(a, m->sph);
            if (!burrow__tar_validate_sparse_entries(sp, (Int)m->sph.n, m->size)) {
                testing_t_errorf_v(t, "invalid sparse map");
                continue;
            }
            Int nd = burrow__tar_invert_sparse_entries(sp, (Int)m->sph.n, m->size);
            fw = (TarFileWriter){w, m->makeReg.size, sp, nd, 0};
            want_str = gstr(a, m->makeReg.wantStr);
        }
        for (int64_t j = 0; j < v->tests.n; j++) {
            const GAny *tf = &v->tests.p[j];
            if (strcmp(tf->type, "TestFileWriter_testWrite") == 0) {
                const TestFileWriter_testWrite *op =
                    (const TestFileWriter_testWrite *)tf->v;
                Error err;
                Slice in = gbytes(a, op->str);
                Int got = burrow__tar_file_writer_write(&fw, in, &err);
                if (got != op->wantCnt || gerr_of(err) != op->wantErr)
                    testing_t_errorf_v(
                        t, "test %d.%d, Write(%s):\ngot  (%d, %v)\nwant (%d, %s)", i, j,
                        slice_str(in), got, err, op->wantCnt, gerr_str(op->wantErr));
            } else if (strcmp(tf->type, "TestFileWriter_testReadFrom") == 0) {
                const TestFileWriter_testReadFrom *op =
                    (const TestFileWriter_testReadFrom *)tf->v;
                TestFile f;
                test_file_init(a, &f, op->ops);
                Error err;
                int64_t got = burrow__tar_file_writer_read_from(
                    &fw, a, (IoReader){&test_file_reader_vt, &f}, &err);
                if (f.test_err)
                    testing_t_errorf_v(t, "test %d.%d, ReadFrom(): %s", i, j,
                                       str_from_bytes(f.why, (Int)strlen(f.why)));
                else if (got != op->wantCnt || gerr_of(err) != op->wantErr)
                    testing_t_errorf_v(
                        t, "test %d.%d, ReadFrom() = (%d, %v), want (%d, %s)", i, j,
                        got, err, op->wantCnt, gerr_str(op->wantErr));
                if (f.n > 0)
                    testing_t_errorf_v(t, "test %d.%d, expected %d more operations", i,
                                       j, f.n);
            } else if (strcmp(tf->type, "TestFileWriter_testRemaining") == 0) {
                const TestFileWriter_testRemaining *op =
                    (const TestFileWriter_testRemaining *)tf->v;
                int64_t got = burrow__tar_file_writer_logical_remaining(&fw);
                if (got != op->wantLCnt)
                    testing_t_errorf_v(t,
                                       "test %d.%d, logicalRemaining() = %d, want %d",
                                       i, j, got, op->wantLCnt);
                got = burrow__tar_file_writer_physical_remaining(&fw);
                if (got != op->wantPCnt)
                    testing_t_errorf_v(t,
                                       "test %d.%d, physicalRemaining() = %d, want %d",
                                       i, j, got, op->wantPCnt);
            } else {
                testing_t_errorf_v(t, "test %d.%d, unknown test operation", i, j);
            }
        }
        Str got = slice_str(bytes_buffer_bytes(&bb));
        if (!str_eq(got, want_str))
            testing_t_errorf_v(t, "test %d, String() = %q, want %q", i, got, want_str);
    }
    arena_free(&ar);
}

static void TestWriterAddFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFile emptyfolder = {
        {NULL, 0, 0, NULL}, FS_MODE_DIR | 0755, {0}, {NULL, NULL}};
    FstestMapFile file_go = {const_bytes("hello", 5), 0, {0}, {NULL, NULL}};
    FstestMapFile another = {const_bytes("world", 5), 0, {0}, {NULL, NULL}};
    FstestMapFile symlink = {
        const_bytes("file.go", 7), FS_MODE_SYMLINK | 0777, {0}, {NULL, NULL}};
    FstestMapFile subfolder = {
        {NULL, 0, 0, NULL}, FS_MODE_DIR | 0555, {0}, {NULL, NULL}};
    FstestMapFS fsys = fstest_map_fs_make(a);
    fstest_map_fs_set(fsys, BURROW_S("emptyfolder"), &emptyfolder);
    fstest_map_fs_set(fsys, BURROW_S("file.go"), &file_go);
    fstest_map_fs_set(fsys, BURROW_S("subfolder/another.go"), &another);
    fstest_map_fs_set(fsys, BURROW_S("symlink.go"), &symlink);

    BytesBuffer buf = BYTES_BUFFER(a);
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err = tar_writer_add_fs(tw, fstest_map_fs_as_fs(fsys));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    err = tar_writer_close(tw);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    fstest_map_fs_set(fsys, BURROW_S("subfolder"), &subfolder);

    TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buf));
    static const char *const names[] = {"emptyfolder", "file.go", "subfolder",
                                        "subfolder/another.go", "symlink.go"};
    const FstestMapFile *files[] = {&emptyfolder, &file_go, &subfolder, &another,
                                    &symlink};
    Int entries_left = LEN(names);
    for (Int i = 0; i < LEN(names); i++) {
        entries_left--;
        Str name = str_from_bytes(names[i], (Int)strlen(names[i]));
        FsFileInfo info = fstest_map_fs_lstat(fsys, a, name, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "getting entry info error: %v", err);
            break;
        }
        TarHeader *hdr = tar_reader_next(tr, &err);
        if (same_error(err, io_eof))
            break;
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%v", err);
            break;
        }
        Str tmp = name;
        if (info.vt->is_dir(info.data))
            tmp = join2(a, name, BURROW_S("/"));
        if (!str_eq(hdr->name, tmp))
            testing_t_errorf_v(t, "test fs has filename %v; archive header has %v",
                               name, hdr->name);
        FsFileInfo hfi = tar_header_file_info(hdr);
        FsFileMode want_mode = info.vt->mode(info.data),
                   got_mode = hfi.vt->mode(hfi.data);
        if (want_mode != got_mode)
            testing_t_errorf_v(t, "%s: test fs has mode %o; archive header has %o",
                               name, (int64_t)want_mode, (int64_t)got_mode);
        FsFileMode type = want_mode & FS_MODE_TYPE;
        if (type == FS_MODE_DIR) {
        } else if (type == FS_MODE_SYMLINK) {
            Str target = slice_str(files[i]->data);
            if (!str_eq(hdr->linkname, target)) {
                testing_t_fatalf_v(t, "test fs has link content %s; archive header %v",
                                   target, hdr->linkname);
                break;
            }
        } else {
            Slice data = io_read_all(a, tar_reader_as_io_reader(tr), &err);
            if (BURROW_FAILED(err)) {
                testing_t_fatalf_v(t, "%v", err);
                break;
            }
            if (!str_eq(slice_str(data), slice_str(files[i]->data))) {
                testing_t_fatalf_v(t,
                                   "test fs has file content %q; archive header has %q",
                                   slice_str(files[i]->data), slice_str(data));
                break;
            }
        }
    }
    if (entries_left > 0)
        testing_t_fatalf_v(t, "not all entries are in the archive");
    arena_free(&ar);
}

static void TestWriterAddFSNonRegularFiles(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFile device = {
        const_bytes("hello", 5), 0755 | FS_MODE_DEVICE, {0}, {NULL, NULL}};
    FstestMapFile symlink = {
        const_bytes("world", 5), 0755 | FS_MODE_SYMLINK, {0}, {NULL, NULL}};
    FstestMapFS fsys = fstest_map_fs_make(a);
    fstest_map_fs_set(fsys, BURROW_S("device"), &device);
    fstest_map_fs_set(fsys, BURROW_S("symlink"), &symlink);
    BytesBuffer buf = BYTES_BUFFER(a);
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
    if (BURROW_OK(tar_writer_add_fs(tw, fstest_map_fs_as_fs(fsys))))
        testing_t_fatalf_v(t, "expected error, got nil");
    arena_free(&ar);
}

/* Every allocation failing in turn, through a whole write and read, gives an
 * error rather than a crash, and nothing is left behind. */
static void TestNoMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    Slice input = testdata(scratch, "testdata/pax-records.tar");
    for (long long budget = 0; budget < 64; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesReader br;
        bytes_reader_reset(&br, input);
        TarReader *tr = tar_new_reader(&al, bytes_reader_as_io_reader(&br));
        if (tr != NULL) {
            Error err;
            for (;;) {
                TarHeader *h = tar_reader_next(tr, &err);
                tar_header_free(&al, h);
                if (BURROW_FAILED(err))
                    break;
            }
            tar_reader_free(tr);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked reading", (int64_t)budget,
                               (int64_t)b.live);

        b = (Budget){budget, 0};
        BytesBuffer out = BYTES_BUFFER(scratch);
        TarWriter *tw = tar_new_writer(&al, bytes_buffer_as_io_writer(&out));
        if (tw != NULL) {
            Map *pax = burrow__tar_str_map(scratch);
            burrow__tar_str_map_set(scratch, pax, BURROW_S("GOLANG.pkg"),
                                    BURROW_S("tar"));
            TarHeader hdr = {.name = repeat(scratch, "ab", 100),
                             .pax_records = pax,
                             .mod_time = time_from_unix(1, 500)};
            (void)tar_writer_write_header(tw, &hdr);
            (void)tar_writer_close(tw);
            tar_writer_free(tw);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked writing", (int64_t)budget,
                               (int64_t)b.live);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFitsInBase256)                                                               \
    X(TestParseNumeric)                                                                \
    X(TestFormatNumeric)                                                               \
    X(TestFitsInOctal)                                                                 \
    X(TestParsePAXTime)                                                                \
    X(TestFormatPAXTime)                                                               \
    X(TestParsePAXRecord)                                                              \
    X(TestFormatPAXRecord)                                                             \
    X(TestReader)                                                                      \
    X(TestPartialRead)                                                                 \
    X(TestUninitializedRead)                                                           \
    X(TestReadTruncation)                                                              \
    X(TestReadHeaderOnly)                                                              \
    X(TestMergePAX)                                                                    \
    X(TestParsePAX)                                                                    \
    X(TestReadOldGNUSparseMap)                                                         \
    X(TestReadGNUSparsePAXHeaders)                                                     \
    X(TestFileReader)                                                                  \
    X(TestInsecurePaths)                                                               \
    X(TestDisableInsecurePathCheck)                                                    \
    X(TestSparseEntries)                                                               \
    X(TestFileInfoHeader)                                                              \
    X(TestFileInfoHeaderDir)                                                           \
    X(TestFileInfoHeaderSymlink)                                                       \
    X(TestRoundTrip)                                                                   \
    X(TestHeaderRoundTrip)                                                             \
    X(TestHeaderAllowedFormats)                                                        \
    X(TestFileInfoHeaderUseFileInfoNames)                                              \
    X(TestWriter)                                                                      \
    X(TestPax)                                                                         \
    X(TestPaxSymlink)                                                                  \
    X(TestPaxNonAscii)                                                                 \
    X(TestPaxXattrs)                                                                   \
    X(TestPaxHeadersSorted)                                                            \
    X(TestUSTARLongName)                                                               \
    X(TestValidTypeflagWithPAXHeader)                                                  \
    X(TestWriterErrors)                                                                \
    X(TestSplitUSTARPath)                                                              \
    X(TestIssue12594)                                                                  \
    X(TestWriteLongHeader)                                                             \
    X(TestFileWriter)                                                                  \
    X(TestWriterAddFS)                                                                 \
    X(TestWriterAddFSNonRegularFiles)                                                  \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
