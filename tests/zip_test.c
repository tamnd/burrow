/* Derived from Go's src/archive/zip/reader_test.go, writer_test.go,
 * zip_test.go, zip64_test.go and zip64_sparse_test.go.
 * Go source: go1.27.1.
 *
 * The tables, the archives the tests spell out byte by byte, and Go's
 * testdata come over through tests/zip_test_gen.h, which
 * tools/gen-archive-zip-tests.sh has Go's own tests write out as they run.
 * The rest is ported by hand. Where Go runs a table entry's reads from five
 * goroutines at once, the port runs them five times over, one after another.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/archive/zip.h"
#include "burrow/burrow.h"
#include "burrow/compress/flate.h"
#include "burrow/compress/gzip.h"
#include "burrow/hash/crc32.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/testing/fstest.h"

#include "../src/archive/zip_internal.h"
#include "zip_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static const char *gerr_label(GErr g) {
    static const char *const names[] = {
        "nil",           "ErrFormat",       "ErrAlgorithm",
        "ErrChecksum",   "ErrInsecurePath", "errLongName",
        "errLongExtra",  "io.EOF",          "io.ErrUnexpectedEOF",
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
        {&zip_err_format, GERR_FORMAT},
        {&zip_err_algorithm, GERR_ALGORITHM},
        {&zip_err_checksum, GERR_CHECKSUM},
        {&zip_err_insecure_path, GERR_INSECURE_PATH},
        {&burrow__zip_err_long_name, GERR_LONG_NAME},
        {&burrow__zip_err_long_extra, GERR_LONG_EXTRA},
        {&io_eof, GERR_EOF},
        {&io_err_unexpected_eof, GERR_UNEXPECTED_EOF},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (same_error(err, *known[i].e))
            return known[i].g;
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

static bool same_bytes(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
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

static const GFile *find_testdata(const char *name) {
    for (size_t i = 0; i < sizeof gen_testdata / sizeof gen_testdata[0]; i++)
        if (strcmp(gen_testdata[i].name, name) == 0)
            return &gen_testdata[i];
    fprintf(stderr, "no testdata file %s\n", name);
    abort();
}

/* A file from Go's testdata, by the name the tests open it with, in memory
 * of its own that the test may change. */
static Slice testdata(Alloc *a, const char *name) {
    const GFile *f = find_testdata(name);
    return gunzip(a, f->gz, f->gz_len, f->len);
}

static Slice testdata_str(Alloc *a, Str name) {
    char buf[256];
    snprintf(buf, sizeof buf, "testdata/%.*s", (int)name.len, (const char *)name.p);
    return testdata(a, buf);
}

/* time.Time's Equal, and the zone's name and offset the same as well, Go's
 * equalTimeAndZone. */
static bool time_matches(Time got, GTime want) {
    Int off = 0;
    Str zone = time_zone(got, &off);
    return time_equal(got, time_from_unix(want.sec, want.nsec)) &&
           str_eq(zone, str_from_cstr(want.zone)) && off == want.off;
}

static ZipFile *zfile(ZipReader *r, Int i) {
    return ((ZipFile **)r->file.p)[i];
}

static Error rc_close(IoReadCloser rc) {
    return rc.vt->closer.close(rc.data);
}

static Error fs_close(FsFile f) {
    return f.vt->read_closer.closer.close(f.data);
}

static IoReader rc_reader(IoReadCloser rc) {
    return (IoReader){&rc.vt->reader, rc.data};
}

/* NewReader on data. br has to outlive the reader. */
static ZipReader *new_reader(Alloc *a, BytesReader *br, Slice data, Error *err) {
    bytes_reader_reset(br, data);
    return zip_new_reader(a, bytes_reader_as_io_reader_at(br), data.len, err);
}

static Str repeat(Alloc *a, const char *s, Int n) {
    return strings_repeat(a, str_from_cstr(s), n);
}

static void free_bytes(Alloc *a, Slice *b) {
    mem_free(a, b->p, (size_t)b->cap, 1);
    *b = slice_nil(TYPE_BYTE);
}

static Str concat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        abort();
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
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

/* ------------------------------------------------------------ temp files */

static const char *temp_root(void) {
    static char buf[400];
    PalErrno e = PAL_OK;
    if (pal_temp_dir(buf, (int64_t)sizeof buf, &e) < 0)
        return "/tmp";
    return buf;
}

/* Writes data to a new file in the temporary directory and puts its name in
 * path, for OpenReader. The caller removes it. */
static void write_temp(TestingT *t, const char *base, Slice data, char *path,
                       size_t cap) {
    uint32_t r = 0;
    pal_random_bytes(&r, sizeof r, NULL);
#if defined(BURROW_OS_WINDOWS)
    snprintf(path, cap, "%s\\burrow-zip-%08x-%s", temp_root(), (unsigned)r, base);
#else
    snprintf(path, cap, "%s/burrow-zip-%08x-%s", temp_root(), (unsigned)r, base);
#endif
    PalErrno e = PAL_OK;
    int64_t fd = pal_open(path, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_TRUNC, 0600, &e);
    if (fd < 0)
        testing_t_fatalf_v(t, "create %s: %s", path, pal_errno_string(e));
    int64_t off = 0;
    while (off < (int64_t)data.len) {
        int64_t n =
            pal_write(fd, (const Byte *)data.p + off, (int64_t)data.len - off, &e);
        if (n <= 0)
            testing_t_fatalf_v(t, "write %s: %s", path, pal_errno_string(e));
        off += n;
    }
    pal_close(fd, &e);
}

static void remove_temp(const char *path) {
    PalErrno e = PAL_OK;
    pal_unlink(path, &e);
}

/* ------------------------------------------------------------- TestReader */

static Slice big_zip_bytes(Alloc *a) {
    Slice b = gbytes(a, gen_biggestZipBytes);
    for (int i = 0; i < 2; i++) {
        BytesReader br;
        Error err;
        ZipReader *r = new_reader(a, &br, b, &err);
        if (r == NULL)
            abort();
        IoReadCloser rc = zip_file_open(zfile(r, 0), a, &err);
        if (BURROW_FAILED(err))
            abort();
        b = io_read_all(a, rc_reader(rc), &err);
        if (BURROW_FAILED(err))
            abort();
        (void)rc_close(rc);
        zip_reader_free(r);
    }
    return b;
}

/* The archive a table entry's Source makes, by the name of the function. */
static Slice table_source(Alloc *a, const char *name) {
    if (strcmp(name, "returnRecursiveZip") == 0)
        return gbytes(a, gen_rZipBytes);
    if (strcmp(name, "returnCorruptCRC32Zip") == 0) {
        Slice b = testdata(a, "testdata/go-with-datadesc-sig.zip");
        ((Byte *)b.p)[0x2d]++;
        return b;
    }
    if (strcmp(name, "returnCorruptNotStreamedZip") == 0) {
        Slice b = testdata(a, "testdata/crc32-not-streamed.zip");
        ((Byte *)b.p)[0x11]++;
        ((Byte *)b.p)[0x9d]++;
        return b;
    }
    if (strcmp(name, "returnBigZipBytes") == 0)
        return big_zip_bytes(a);
    fprintf(stderr, "no source %s\n", name);
    abort();
}

static void test_file_mode(TestingT *t, ZipFile *f, int64_t want) {
    FsFileMode mode = zip_file_mode(f);
    if (want == 0)
        testing_t_errorf_v(t, "%q mode: got %v, want none", f->file_header.name, mode);
    else if (mode != (FsFileMode)want)
        testing_t_errorf_v(t, "%q mode: want %v, got %v", f->file_header.name,
                           (FsFileMode)want, mode);
}

/* readTestFile. Files open with al and everything else comes from scratch. */
static void read_test_file(TestingT *t, const GZipTestFile *ft, ZipFile *f, Slice raw,
                           Alloc *al, Alloc *scratch) {
    const ZipFileHeader *h = &f->file_header;
    Str want_name = gstr(scratch, ft->Name);
    if (!str_eq(h->name, want_name))
        testing_t_errorf_v(t, "name=%q, want %q", h->name, want_name);
    if (!ft->Modified.zero && !time_matches(h->modified, ft->Modified))
        testing_t_errorf_v(t, "%q: Modified=%d, want %d", h->name,
                           time_unix(h->modified), ft->Modified.sec);
    if (!ft->ModTime.zero && !time_matches(zip_file_mod_time(f), ft->ModTime))
        testing_t_errorf_v(t, "%q: ModTime=%d, want %d", h->name,
                           time_unix(zip_file_mod_time(f)), ft->ModTime.sec);

    test_file_mode(t, f, ft->Mode);

    uint64_t size = h->uncompressed_size;
    if (size == ZIP_UINT32_MAX)
        size = h->uncompressed_size64;
    else if (size != h->uncompressed_size64)
        testing_t_errorf_v(
            t, "%q: UncompressedSize=%#x does not match UncompressedSize64=%#x",
            h->name, size, h->uncompressed_size64);

    /* OpenRaw gives the right bytes. */
    Error err;
    IoSectionReader rw = zip_file_open_raw(f, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%q: OpenRaw error=%v", h->name, err);
        return;
    }
    int64_t start = zip_file_data_offset(f, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%q: DataOffset error=%v", h->name, err);
        return;
    }
    Slice got = io_read_all(scratch, io_section_reader_as_io_reader(&rw), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%q: OpenRaw ReadAll error=%v", h->name, err);
        return;
    }
    uint64_t end = (uint64_t)start + h->compressed_size64;
    if (end > (uint64_t)raw.len || got.len != (Int)(end - (uint64_t)start) ||
        (got.len > 0 &&
         memcmp(got.p, (const Byte *)raw.p + start, (size_t)got.len) != 0)) {
        testing_t_errorf_v(t, "%q: OpenRaw returned unexpected bytes", h->name);
        return;
    }

    IoReadCloser r = zip_file_open(f, al, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        return;
    }

    /* For very large files, only the size. The contents are zeros. */
    if (ft->Content.nil && ft->File.n == 0 && ft->Size > 0) {
        if (size != (uint64_t)ft->Size)
            testing_t_errorf_v(t, "%q: uncompressed size %#x, want %#x", want_name,
                               size, ft->Size);
        (void)rc_close(r);
        return;
    }

    BytesBuffer b = BYTES_BUFFER(scratch);
    io_copy(scratch, bytes_buffer_as_io_writer(&b), rc_reader(r), &err);
    if (gerr_of(err) != ft->ContentErr)
        testing_t_errorf_v(t, "copying contents: %v (want %s)", err,
                           gerr_label(ft->ContentErr));
    (void)rc_close(r);
    if (BURROW_FAILED(err))
        return;

    Slice bb = bytes_buffer_bytes(&b);
    if ((uint64_t)bb.len != size)
        testing_t_errorf_v(t, "%q: read %d bytes but f.UncompressedSize == %d", h->name,
                           bb.len, size);

    Slice c = !ft->Content.nil ? gbytes(scratch, ft->Content)
                               : testdata_str(scratch, gstr(scratch, ft->File));
    if (bb.len != c.len) {
        testing_t_errorf_v(t, "%q: len=%d, want %d", h->name, bb.len, c.len);
        return;
    }
    for (Int i = 0; i < bb.len; i++) {
        Byte x = ((const Byte *)bb.p)[i], y = ((const Byte *)c.p)[i];
        if (x != y) {
            testing_t_errorf_v(t, "%q: content[%d]=%q want %q", h->name, i, (Rune)x,
                               (Rune)y);
            return;
        }
    }
}

static void read_test_zip(TestingT *t, const GZipTest *zt, Alloc *al, Alloc *scratch) {
    ZipReader *z = NULL;
    ZipReadCloser *rc = NULL;
    Error err = BURROW_NO_ERROR;
    Slice raw;
    BytesReader br;
    char path[600] = {0};
    if (zt->Source != NULL) {
        raw = table_source(scratch, zt->Source);
        z = new_reader(al, &br, raw, &err);
    } else {
        Str name = gstr(scratch, zt->Name);
        raw = testdata_str(scratch, name);
        char base[128];
        snprintf(base, sizeof base, "%.*s", (int)name.len, (const char *)name.p);
        write_temp(t, base, raw, path, sizeof path);
        rc = zip_open_reader(al, str_from_cstr(path), &err);
        if (rc != NULL)
            z = &rc->reader;
    }
    if (gerr_of(err) != zt->Error) {
        testing_t_errorf_v(t, "error=%v, want %s", err, gerr_label(zt->Error));
        goto done;
    }
    if (zt->Error == GERR_FORMAT || zt->File.nil || z == NULL)
        goto done;

    Str comment = gstr(scratch, zt->Comment);
    if (!str_eq(z->comment, comment))
        testing_t_errorf_v(t, "comment=%q, want %q", z->comment, comment);
    if (z->file.len != (Int)zt->File.n) {
        testing_t_errorf_v(t, "file count=%d, want %d", z->file.len, zt->File.n);
        goto done;
    }
    for (Int i = 0; i < z->file.len; i++)
        read_test_file(t, &zt->File.p[i], zfile(z, i), raw, al, scratch);
    if (testing_t_failed(t))
        goto done;
    for (int rep = 0; rep < 5; rep++)
        for (Int i = 0; i < z->file.len; i++)
            read_test_file(t, &zt->File.p[i], zfile(z, i), raw, al, scratch);

done:
    if (rc != NULL)
        (void)zip_read_closer_close(rc);
    else
        zip_reader_free(z);
    if (path[0] != 0)
        remove_temp(path);
}

static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_tests / sizeof gen_tests[0]; i++) {
        const GZipTest *zt = &gen_tests[i];
        Budget b = {1LL << 40, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        bool before = testing_t_failed(t);
        read_test_zip(t, zt, &al, scratch);
        if (b.live != 0)
            testing_t_errorf_v(t, "%q: %d bytes leaked", gstr(scratch, zt->Name),
                               (int64_t)b.live);
        if (!before && testing_t_failed(t))
            testing_t_logf_v(t, "in %q", gstr(scratch, zt->Name));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestInvalidFiles(TestingT *t) {
    enum { size = 1024 * 70 };
    static Byte b[size];
    memset(b, 0, sizeof b);
    BytesReader br;
    Error err;

    /* zeroes */
    ZipReader *r = new_reader(heap_allocator(), &br, const_bytes(b, size), &err);
    if (!same_error(err, zip_err_format))
        testing_t_errorf_v(t, "zeroes: error=%v, want %v", err, zip_err_format);
    zip_reader_free(r);

    /* repeated directoryEndSignatures */
    for (int i = 0; i < size - 4; i += 4) {
        b[i] = 'P';
        b[i + 1] = 'K';
        b[i + 2] = 5;
        b[i + 3] = 6;
    }
    r = new_reader(heap_allocator(), &br, const_bytes(b, size), &err);
    if (!same_error(err, zip_err_format))
        testing_t_errorf_v(t, "sigs: error=%v, want %v", err, zip_err_format);
    zip_reader_free(r);

    /* negative size */
    bytes_reader_reset(&br, const_bytes("foobar", 6));
    r = zip_new_reader(heap_allocator(), bytes_reader_as_io_reader_at(&br), -1, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t, "archive/zip.NewReader: expected error when negative size is "
               "passed");
    zip_reader_free(r);
}

static void TestIssue8186(TestingT *t) {
    for (size_t i = 0;
         i < sizeof gen_TestIssue8186_dirEnts / sizeof gen_TestIssue8186_dirEnts[0];
         i++) {
        BytesReader br;
        bytes_reader_reset(&br, gbytes(heap_allocator(), gen_TestIssue8186_dirEnts[i]));
        Error err = burrow__zip_read_directory_header(heap_allocator(),
                                                      bytes_reader_as_io_reader(&br));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "error reading #%d: %v", (int64_t)i, err);
    }
}

/* ErrUnexpectedEOF when the length is short. */
static void TestIssue10957(TestingT *t) {
    Alloc *a = heap_allocator();
    BytesReader br;
    Error err;
    ZipReader *z = new_reader(a, &br, gbytes(a, gen_TestIssue10957_data), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    for (Int i = 0; i < z->file.len; i++) {
        ZipFile *f = zfile(z, i);
        IoReadCloser r = zip_file_open(f, a, &err);
        if (BURROW_FAILED(err))
            continue;
        if (f->file_header.uncompressed_size64 < 1000000) {
            int64_t n = io_copy(a, io_discard, rc_reader(r), &err);
            if (i == 3 && !same_error(err, io_err_unexpected_eof))
                testing_t_errorf_v(t, "File[3] error = %v; want io.ErrUnexpectedEOF",
                                   err);
            if (BURROW_OK(err) && (uint64_t)n != f->file_header.uncompressed_size64)
                testing_t_errorf_v(t, "file %d: bad size: copied=%d; want=%d", i, n,
                                   f->file_header.uncompressed_size64);
        }
        (void)rc_close(r);
    }
    zip_reader_free(z);
}

/* This malformed archive is turned down. */
static void TestIssue10956(TestingT *t) {
    BytesReader br;
    Error err;
    ZipReader *r = new_reader(heap_allocator(), &br,
                              gbytes(heap_allocator(), gen_TestIssue10956_data), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "got nil error, want ErrFormat");
    if (r != NULL)
        testing_t_errorf_v(t, "got non-nil Reader, want nil");
    zip_reader_free(r);
}

/* ErrUnexpectedEOF reading a cut off data descriptor. */
static void TestIssue11146(TestingT *t) {
    Alloc *a = heap_allocator();
    BytesReader br;
    Error err;
    ZipReader *z = new_reader(a, &br, gbytes(a, gen_TestIssue11146_data), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    IoReadCloser r = zip_file_open(zfile(z, 0), a, &err);
    if (BURROW_FAILED(err)) {
        zip_reader_free(z);
        testing_t_fatalf_v(t, "%v", err);
    }
    Slice got = io_read_all(a, rc_reader(r), &err);
    if (!same_error(err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "File[0] error = %v; want io.ErrUnexpectedEOF", err);
    free_bytes(a, &got);
    (void)rc_close(r);
    zip_reader_free(z);
}

/* An archive that is not ZIP64 is not taken for one. */
static void TestIssue12449(TestingT *t) {
    BytesReader br;
    Error err;
    ZipReader *r = new_reader(heap_allocator(), &br,
                              gbytes(heap_allocator(), gen_TestIssue12449_data), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error reading the archive: %v", err);
    zip_reader_free(r);
}

/* OpenReader on a file from testdata, through a copy in the temporary
 * directory, which is removed when the reader is closed with close_testdata. */
typedef struct Opened {
    ZipReadCloser *rc;
    char path[600];
} Opened;

static void open_testdata(TestingT *t, Alloc *a, Opened *o, const char *name) {
    const char *base = strrchr(name, '/');
    write_temp(t, base != NULL ? base + 1 : name, testdata(a, name), o->path,
               sizeof o->path);
    Error err;
    o->rc = zip_open_reader(a, str_from_cstr(o->path), &err);
    if (BURROW_FAILED(err)) {
        remove_temp(o->path);
        testing_t_fatalf_v(t, "%v", err);
    }
}

static void close_testdata(Opened *o) {
    (void)zip_read_closer_close(o->rc);
    remove_temp(o->path);
}

/* What there is of fstest.TestFS until burrow has it: every file the test
 * names opens, reads the same as through the archive, and is found walking the
 * tree, and every file the walk finds stats as the same kind of thing. */
typedef struct FsCheckEnv {
    TestingT *t;
    Alloc *a;
    Fs fsys;
    Str *seen;
    Int nseen;
} FsCheckEnv;

static Error fs_check_visit(void *env, Str path, FsDirEntry d, Error err) {
    FsCheckEnv *e = (FsCheckEnv *)env;
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(e->t, "%q: %v", path, err);
        return err;
    }
    Error serr;
    FsFileInfo fi = fs_stat(e->a, e->fsys, path, &serr);
    if (BURROW_FAILED(serr))
        testing_t_errorf_v(e->t, "stat %q: %v", path, serr);
    else if (fi.vt->is_dir(fi.data) != d.vt->is_dir(d.data))
        testing_t_errorf_v(e->t, "%q: stat and dir entry disagree on IsDir", path);
    if (e->nseen < 64)
        e->seen[e->nseen++] = path;
    return BURROW_NO_ERROR;
}

static void TestFS(TestingT *t) {
    static const struct {
        const char *file;
        const char *want[3];
        int nwant;
    } tests[] = {
        {"testdata/unix.zip", {"hello", "dir/bar", "readonly"}, 3},
        {"testdata/subdir.zip", {"a/b/c"}, 1},
    };
    for (size_t ti = 0; ti < sizeof tests / sizeof tests[0]; ti++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Opened o;
        open_testdata(t, a, &o, tests[ti].file);
        Fs fsys = zip_reader_as_fs(&o.rc->reader);
        Str seen[64];
        FsCheckEnv env = {t, a, fsys, seen, 0};
        Error err = fs_walk_dir(a, fsys, BURROW_S("."),
                                BURROW_FN(FsWalkDirFunc, fs_check_visit, &env));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: walk: %v", tests[ti].file, err);
        for (int wi = 0; wi < tests[ti].nwant; wi++) {
            Str name = str_from_cstr(tests[ti].want[wi]);
            bool found = false;
            for (Int k = 0; k < env.nseen; k++)
                found = found || str_eq(seen[k], name);
            if (!found)
                testing_t_errorf_v(t, "%s: walk did not find %q", tests[ti].file, name);
            Slice data = fs_read_file(a, fsys, name, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: ReadFile %q: %v", tests[ti].file, name, err);
                continue;
            }
            for (Int k = 0; k < o.rc->reader.file.len; k++) {
                ZipFile *f = zfile(&o.rc->reader, k);
                if (!str_eq(f->file_header.name, name))
                    continue;
                IoReadCloser rc = zip_file_open(f, a, &err);
                Slice want = io_read_all(a, rc_reader(rc), &err);
                (void)rc_close(rc);
                if (!same_bytes(want, data))
                    testing_t_errorf_v(t, "%s: %q reads differently through the FS",
                                       tests[ti].file, name);
            }
        }
        close_testdata(&o);
        arena_free(&ar);
    }
}

typedef struct WalkEnv {
    TestingT *t;
    bool want_err;
    bool saw_err;
    Str files[16];
    Int n;
} WalkEnv;

static Error walk_visit(void *env, Str path, FsDirEntry d, Error err) {
    (void)d;
    WalkEnv *e = (WalkEnv *)env;
    if (BURROW_FAILED(err)) {
        if (!e->want_err)
            testing_t_errorf_v(e->t, "%q: %v", path, err);
        e->saw_err = true;
        return BURROW_NO_ERROR;
    }
    if (e->n < 16)
        e->files[e->n++] = path;
    return BURROW_NO_ERROR;
}

static void TestFSWalk(TestingT *t) {
    static const struct {
        const char *file;
        const char *want[6];
        int nwant;
        bool want_err;
    } tests[] = {
        {"testdata/unix.zip",
         {".", "dir", "dir/bar", "dir/empty", "hello", "readonly"},
         6,
         false},
        {"testdata/subdir.zip", {".", "a", "a/b", "a/b/c"}, 4, false},
        {"testdata/dupdir.zip", {NULL}, -1, true},
    };
    for (size_t ti = 0; ti < sizeof tests / sizeof tests[0]; ti++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Opened o;
        open_testdata(t, a, &o, tests[ti].file);
        WalkEnv env = {t, tests[ti].want_err, false, {{NULL, 0}}, 0};
        Error err = fs_walk_dir(a, zip_reader_as_fs(&o.rc->reader), BURROW_S("."),
                                BURROW_FN(FsWalkDirFunc, walk_visit, &env));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: fs.WalkDir error: %v", tests[ti].file, err);
        if (tests[ti].want_err && !env.saw_err)
            testing_t_errorf_v(t, "%s: succeeded but want error", tests[ti].file);
        else if (!tests[ti].want_err && env.saw_err)
            testing_t_errorf_v(t, "%s: unexpected error", tests[ti].file);
        if (tests[ti].nwant >= 0) {
            bool same = env.n == tests[ti].nwant;
            for (Int k = 0; same && k < env.n; k++)
                same = str_eq(env.files[k], str_from_cstr(tests[ti].want[k]));
            if (!same) {
                testing_t_errorf_v(t, "%s: got %d files:", tests[ti].file, env.n);
                for (Int k = 0; k < env.n; k++)
                    testing_t_logf_v(t, "  %q", env.files[k]);
            }
        }
        close_testdata(&o);
        arena_free(&ar);
    }
}

BURROW_SENTINEL_ERROR(err_repeat, "repeated call to path");

static Error bad_file_visit(void *env, Str path, FsDirEntry d, Error err) {
    (void)path;
    (void)d;
    int *count = (int *)env;
    (*count)++;
    if (*count > 2) /* once for the directory read, once for the error */
        return err_repeat;
    return err;
}

static void TestFSWalkBadFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *zw = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    ZipFileHeader hdr = {.name = BURROW_S(".")};
    zip_file_header_set_mode(&hdr, FS_MODE_DIR | 0755);
    Error err;
    IoWriter w = zip_writer_create_header(zw, &hdr, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "create zip header: %v", err);
    w.vt->write(w.data, const_bytes("some data", 9), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "write zip contents: %v", err);
    err = zip_writer_close(zw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "close zip writer: %v", err);

    BytesReader br;
    ZipReader *zr = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "create zip reader: %v", err);
    int count = 0;
    err = fs_walk_dir(a, zip_reader_as_fs(zr), BURROW_S("."),
                      BURROW_FN(FsWalkDirFunc, bad_file_visit, &count));
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error from invalid file name");
    else if (errors_is(err, err_repeat))
        testing_t_fatalf_v(t, "%v", err);
    arena_free(&ar);
}

static void TestFSModTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Opened o;
    open_testdata(t, a, &o, "testdata/subdir.zip");
    TimeLocation *tz = burrow__zip_time_zone(a, -7 * TIME_HOUR);
    static const struct {
        const char *name;
        Int sec;
    } tests[] = {{"a", 56}, {"a/b/c", 59}};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Time want = time_utc(time_date(2021, 4, 19, 12, 29, tests[i].sec, 0, tz));
        Error err;
        FsFileInfo fi = fs_stat(a, zip_reader_as_fs(&o.rc->reader),
                                str_from_cstr(tests[i].name), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %v", tests[i].name, err);
            continue;
        }
        Time got = fi.vt->mod_time(fi.data);
        if (!time_equal(got, want))
            testing_t_errorf_v(t, "%s: got modtime %d, want %d", tests[i].name,
                               time_unix(got), time_unix(want));
    }
    time_location_free(tz);
    close_testdata(&o);
    arena_free(&ar);
}

/* The archive with only "../test.txt" in it, read with NewReader or with
 * OpenReader. */
static void check_insecure_test_txt(TestingT *t, Alloc *a, ZipReader *r, Error err) {
    if (!same_error(err, zip_err_insecure_path) || r == NULL)
        testing_t_fatalf_v(t, "Error reading the archive: %v", err);
    FsFile f = zip_reader_open(r, a, BURROW_S("test.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error reading file: %v", err);
    else
        (void)fs_close(f);
    if (r->file.len != 1)
        testing_t_fatalf_v(t, "No entries in the file list");
    if (!str_eq(zfile(r, 0)->file_header.name, BURROW_S("../test.txt")))
        testing_t_errorf_v(t, "Unexpected entry name: %q",
                           zfile(r, 0)->file_header.name);
    IoReadCloser rc = zip_file_open(zfile(r, 0), a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error opening file: %v", err);
    else
        (void)rc_close(rc);
}

static void TestCVE202127919(TestingT *t) {
    burrow__zip_godebug_set("zipinsecurepath=0");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    Error err;
    ZipReader *r = new_reader(a, &br, gbytes(a, gen_TestCVE202127919_data), &err);
    check_insecure_test_txt(t, a, r, err);
    arena_free(&ar);
    burrow__zip_godebug_set(NULL);
}

static void TestOpenReaderInsecurePath(TestingT *t) {
    burrow__zip_godebug_set("zipinsecurepath=0");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    char path[600];
    write_temp(t, "insecure.zip", gbytes(a, gen_TestOpenReaderInsecurePath_data), path,
               sizeof path);
    Error err;
    ZipReadCloser *rc = zip_open_reader(a, str_from_cstr(path), &err);
    if (rc == NULL) {
        remove_temp(path);
        testing_t_fatalf_v(
            t, "Error reading the archive, we expected ErrInsecurePath but got: %v",
            err);
    }
    check_insecure_test_txt(t, a, &rc->reader, err);
    (void)zip_read_closer_close(rc);
    remove_temp(path);
    arena_free(&ar);
    burrow__zip_godebug_set(NULL);
}

static void TestCVE202133196(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    Error err;
    new_reader(a, &br, gbytes(a, gen_TestCVE202133196_data), &err);
    if (!same_error(err, zip_err_format))
        testing_t_fatalf_v(t, "unexpected error, got: %v, want: %v", err,
                           zip_err_format);

    /* A handful of empty files are fine. */
    BytesBuffer b = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&b));
    for (int i = 0; i < 5; i++) {
        zip_writer_create(w, BURROW_S(""), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Writer.Create failed: %v", err);
    }
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Writer.Close failed: %v", err);
    ZipReader *r = new_reader(a, &br, bytes_buffer_bytes(&b), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewReader failed: %v", err);
    if (r->file.len != 5)
        testing_t_errorf_v(t, "Archive has unexpected number of files, got %d, want 5",
                           r->file.len);
    arena_free(&ar);
}

static void TestCVE202139293(TestingT *t) {
    BytesReader br;
    Error err;
    ZipReader *r =
        new_reader(heap_allocator(), &br,
                   gbytes(heap_allocator(), gen_TestCVE202139293_data), &err);
    if (!same_error(err, zip_err_format))
        testing_t_fatalf_v(t, "unexpected error, got: %v, want: %v", err,
                           zip_err_format);
    zip_reader_free(r);
}

static void TestCVE202141772(TestingT *t) {
    burrow__zip_godebug_set("zipinsecurepath=0");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    Error err;
    ZipReader *r = new_reader(a, &br, gbytes(a, gen_TestCVE202141772_data), &err);
    if (!same_error(err, zip_err_insecure_path))
        testing_t_fatalf_v(t, "Error reading the archive: %v", err);
    size_t nnames = sizeof gen_TestCVE202141772_entryNames /
                    sizeof gen_TestCVE202141772_entryNames[0];
    bool same = r->file.len == (Int)nnames;
    for (Int i = 0; i < r->file.len; i++) {
        ZipFile *f = zfile(r, i);
        if (same)
            same = str_eq(f->file_header.name,
                          gstr(a, gen_TestCVE202141772_entryNames[i]));
        IoReadCloser rc = zip_file_open(f, a, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Error opening %q: %v", f->file_header.name, err);
        else
            (void)rc_close(rc);
        FsFile ff = zip_reader_open(r, a, f->file_header.name, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "Opening %q with fs.FS API succeeded",
                               f->file_header.name);
            (void)fs_close(ff);
        }
    }
    if (!same)
        testing_t_errorf_v(t, "Unexpected file entries");
    FsFile ff = zip_reader_open(r, a, BURROW_S(""), &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "Opening %q with fs.FS API succeeded", BURROW_S(""));
        (void)fs_close(ff);
    }
    ff = zip_reader_open(r, a, BURROW_S("test.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error opening %q with fs.FS API: %v",
                           BURROW_S("test.txt"), err);
    else
        (void)fs_close(ff);
    Slice ents = fs_read_dir(a, zip_reader_as_fs(r), BURROW_S("."), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Error reading the root directory: %v", err);
    FsDirEntry *de = (FsDirEntry *)ents.p;
    if (ents.len != 1 || !str_eq(de[0].vt->name(de[0].data), BURROW_S("test.txt"))) {
        testing_t_errorf_v(t, "Unexpected directory entries");
        for (Int i = 0; i < ents.len; i++)
            testing_t_logf_v(t, "%q", de[i].vt->name(de[i].data));
        testing_t_fail_now(t);
    }
    FsFileInfo info = de[0].vt->info(de[0].data, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Error reading info entry: %v", err);
    Str name = info.vt->name(info.data);
    if (!str_eq(name, BURROW_S("test.txt")))
        testing_t_errorf_v(t, "Inconsistent name in info entry: %q", name);
    arena_free(&ar);
    burrow__zip_godebug_set(NULL);
}

static void TestUnderSize(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Opened o;
    open_testdata(t, a, &o, "testdata/readme.zip");
    ZipReader *z = &o.rc->reader;
    for (Int i = 0; i < z->file.len; i++)
        zfile(z, i)->file_header.uncompressed_size64 = 1;
    for (Int i = 0; i < z->file.len; i++) {
        ZipFile *f = zfile(z, i);
        Error err;
        IoReadCloser rd = zip_file_open(f, a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: %v", f->file_header.name, err);
            continue;
        }
        io_copy(a, io_discard, rc_reader(rd), &err);
        if (!same_error(err, zip_err_format))
            testing_t_errorf_v(t, "%q: Error mismatch\n\tGot:  %v\n\tWant: %v",
                               f->file_header.name, err, zip_err_format);
        (void)rc_close(rd);
    }
    close_testdata(&o);
    arena_free(&ar);
}

static void TestIssue54801(TestingT *t) {
    static const char *const inputs[] = {"testdata/readme.zip", "testdata/dd.zip"};
    for (size_t ii = 0; ii < 2; ii++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Opened o;
        open_testdata(t, a, &o, inputs[ii]);
        ZipReader *z = &o.rc->reader;
        for (Int i = 0; i < z->file.len; i++) {
            ZipFile *f = zfile(z, i);
            /* Make the file a directory. */
            f->file_header.name = concat(a, f->file_header.name, BURROW_S("/"));
            Error err;
            IoReadCloser rd = zip_file_open(f, a, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%q: %v", f->file_header.name, err);
                continue;
            }
            int64_t n = io_copy(a, io_discard, rc_reader(rd), &err);
            if (n != 0 || !same_error(err, zip_err_format))
                testing_t_errorf_v(t, "%q: Error mismatch, got: %d, %v, want: %v",
                                   f->file_header.name, n, err, zip_err_format);
            (void)rc_close(rd);
        }
        close_testdata(&o);
        arena_free(&ar);
    }
}

/* Writes an archive with one empty file called name and reads it back. */
static ZipReader *one_file_archive(TestingT *t, Alloc *a, BytesBuffer *buf,
                                   BytesReader *br, Str name, Error *err) {
    *buf = BYTES_BUFFER(a);
    ZipWriter *zw = zip_new_writer(a, bytes_buffer_as_io_writer(buf));
    zip_writer_create(zw, name, err);
    if (BURROW_FAILED(*err)) {
        testing_t_errorf_v(t, "zw.Create(%q) = %v", name, *err);
        return NULL;
    }
    (void)zip_writer_close(zw);
    return new_reader(a, br, bytes_buffer_bytes(buf), err);
}

static void TestInsecurePaths(TestingT *t) {
    burrow__zip_godebug_set("zipinsecurepath=0");
    static const char *const paths[] = {"../foo", "/foo", "a/b/../../../c", "a\\b"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str path = str_from_cstr(paths[i]);
        BytesBuffer buf;
        BytesReader br;
        Error err;
        ZipReader *zr = one_file_archive(t, a, &buf, &br, path, &err);
        if (!same_error(err, zip_err_insecure_path) || zr == NULL) {
            testing_t_errorf_v(t,
                               "NewReader for archive with file %q: got err %v, want "
                               "ErrInsecurePath",
                               path, err);
        } else if (zr->file.len != 1 || !str_eq(zfile(zr, 0)->file_header.name, path)) {
            testing_t_errorf_v(t, "NewReader for archive with file %q: got %d files",
                               path, zr->file.len);
        }
        arena_free(&ar);
    }
    burrow__zip_godebug_set(NULL);
}

static void TestDisableInsecurePathCheck(TestingT *t) {
    burrow__zip_godebug_set("zipinsecurepath=1");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str name = BURROW_S("/foo");
    BytesBuffer buf;
    BytesReader br;
    Error err;
    ZipReader *zr = one_file_archive(t, a, &buf, &br, name, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewReader with zipinsecurepath=1: got err %v, want nil",
                           err);
    if (zr->file.len != 1 || !str_eq(zfile(zr, 0)->file_header.name, name))
        testing_t_errorf_v(t, "NewReader with zipinsecurepath=1: got %d files, want %q",
                           zr->file.len, name);
    arena_free(&ar);
    burrow__zip_godebug_set(NULL);
}

static void TestCompressedDirectory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    Error err;
    ZipReader *r =
        new_reader(a, &br, gbytes(a, gen_TestCompressedDirectory_data), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    for (Int i = 0; i < r->file.len; i++) {
        IoReadCloser rc = zip_file_open(zfile(r, i), a, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "unexpected error: %v", err);
        io_copy(a, io_discard, rc_reader(rc), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "unexpected error: %v", err);
        (void)rc_close(rc);
    }
    arena_free(&ar);
}

/* directoryOffset > maxInt64 and size-directoryOffset < 0 must not reach
 * outside the data. */
static void TestBaseOffsetPlusOverflow(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    Slice data = gbytes(a, gen_TestBaseOffsetPlusOverflow_data);
    BytesReader br;
    bytes_reader_reset(&br, data);
    Error err;
    ZipReader *r = zip_new_reader(a, bytes_reader_as_io_reader_at(&br),
                                  (int64_t)data.len + 1875, &err);
    zip_reader_free(r);
}

/* ------------------------------------------------------------ writer tests */

/* writeTests, with the random 128 KiB that Go puts in the second. */
typedef struct WriteTest {
    Str name;
    Slice data;
    uint16_t method;
    FsFileMode mode;
} WriteTest;

static WriteTest *write_tests(Alloc *a, Int *n) {
    *n = (Int)(sizeof gen_writeTests / sizeof gen_writeTests[0]);
    WriteTest *wt =
        (WriteTest *)mem_alloc(a, sizeof *wt * (size_t)*n, _Alignof(WriteTest));
    for (Int i = 0; i < *n; i++) {
        const GWriteTest *g = &gen_writeTests[i];
        wt[i].name = gstr(a, g->Name);
        wt[i].method = (uint16_t)g->Method;
        wt[i].mode = (FsFileMode)g->Mode;
        if (g->Data.nil) {
            Byte *p = (Byte *)mem_alloc(a, 1 << 17, 1);
            pal_random_bytes(p, 1 << 17, NULL);
            wt[i].data = const_bytes(p, 1 << 17);
        } else {
            wt[i].data = gbytes(a, g->Data);
        }
    }
    return wt;
}

static void test_create(TestingT *t, ZipWriter *w, const WriteTest *wt) {
    ZipFileHeader header = {.name = wt->name, .method = wt->method};
    if (wt->mode != 0)
        zip_file_header_set_mode(&header, wt->mode);
    Error err;
    IoWriter f = zip_writer_create_header(w, &header, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    f.vt->write(f.data, wt->data, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
}

static void test_read_file(TestingT *t, Alloc *a, ZipFile *f, const WriteTest *wt) {
    if (!str_eq(f->file_header.name, wt->name))
        testing_t_fatalf_v(t, "File name: got %q, want %q", f->file_header.name,
                           wt->name);
    test_file_mode(t, f, wt->mode);
    Error err;
    IoReadCloser rc = zip_file_open(f, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "opening %q: %v", f->file_header.name, err);
    Slice b = io_read_all(a, rc_reader(rc), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "reading %q: %v", f->file_header.name, err);
    err = rc_close(rc);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "closing %q: %v", f->file_header.name, err);
    if (!same_bytes(b, wt->data))
        testing_t_errorf_v(t, "File contents %q, want %q", slice_str(b),
                           slice_str(wt->data));
}

/* Writes the writeTests, after prefix when there is one, and checks what
 * reads back. Leaves the archive in buf. */
static ZipReader *write_and_read(TestingT *t, Alloc *a, BytesBuffer *buf,
                                 BytesReader *br, const WriteTest *wts, Int n,
                                 Slice prefix) {
    *buf = BYTES_BUFFER(a);
    Error err;
    bytes_buffer_write(buf, prefix, &err);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(buf));
    zip_writer_set_offset(w, prefix.len);
    for (Int i = 0; i < n; i++)
        test_create(t, w, &wts[i]);
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    ZipReader *r = new_reader(a, br, bytes_buffer_bytes(buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    for (Int i = 0; i < n; i++)
        test_read_file(t, a, zfile(r, i), &wts[i]);
    return r;
}

static void TestWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n;
    WriteTest *wts = write_tests(a, &n);
    BytesBuffer buf;
    BytesReader br;
    write_and_read(t, a, &buf, &br, wts, n, slice_nil(TYPE_BYTE));
    arena_free(&ar);
}

/* The end of central directory comment, written and read. */
static void TestWriterComment(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Str comment;
        bool ok;
    } tests[] = {
        {BURROW_S("hi, hello"), true},
        {BURROW_S("hi, \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x82\x8f"),
         true},
        {repeat(a, "a", ZIP_UINT16_MAX), true},
        {repeat(a, "a", ZIP_UINT16_MAX + 1), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
        Error err = zip_writer_set_comment(w, tests[i].comment);
        if (BURROW_FAILED(err)) {
            if (tests[i].ok)
                testing_t_fatalf_v(t, "SetComment: unexpected error %v", err);
            zip_writer_free(w);
            continue;
        }
        if (!tests[i].ok)
            testing_t_fatalf_v(t, "SetComment: unexpected success, want error");
        err = zip_writer_close(w);
        if (tests[i].ok == BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        if (w->closed != tests[i].ok)
            testing_t_fatalf_v(t, "Writer.closed: got %v, want %v", w->closed,
                               tests[i].ok);
        if (!tests[i].ok)
            continue;
        BytesReader br;
        ZipReader *r = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        if (!str_eq(r->comment, tests[i].comment))
            testing_t_fatalf_v(t, "Reader.Comment: got %q, want %q", r->comment,
                               tests[i].comment);
    }
    arena_free(&ar);
}

static void TestWriterUTF8(TestingT *t) {
    static const struct {
        const char *name;
        const char *comment;
        bool non_utf8;
        uint16_t flags;
    } tests[] = {
        {"hi, hello", "in the world", false, 0x8},
        {"hi, \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x82\x8f",
         "in the world", false, 0x808},
        {"hi, \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x82\x8f",
         "in the world", true, 0x8},
        {"hi, hello", "in the \xe4\xb8\x96\xe7\x95\x8c", false, 0x808},
        {"hi, \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x82\x8f",
         "in the \xe4\xb8\x96\xe7\x95\x8c", false, 0x808},
        {"the replacement rune is \xef\xbf\xbd", "the replacement rune is \xef\xbf\xbd",
         false, 0x808},
        /* The name is Japanese in Shift JIS, so the UTF-8 flag must not be set. */
        {"\x93\xfa\x96{\x8c\xea.txt", "in the \xe4\xb8\x96\xe7\x95\x8c", false, 0x008},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ZipFileHeader h = {.name = str_from_cstr(tests[i].name),
                           .comment = str_from_cstr(tests[i].comment),
                           .non_utf8 = tests[i].non_utf8,
                           .method = ZIP_DEFLATE};
        Error err;
        IoWriter fw = zip_writer_create_header(w, &h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        fw.vt->write(fw.data, slice_nil(TYPE_BYTE), &err);
    }
    Error err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    BytesReader br;
    ZipReader *r = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        uint16_t flags = zfile(r, (Int)i)->file_header.flags;
        if (flags != tests[i].flags)
            testing_t_errorf_v(
                t,
                "CreateHeader(name=%q comment=%q nonUTF8=%v): flags=%#x, want "
                "%#x",
                str_from_cstr(tests[i].name), str_from_cstr(tests[i].comment),
                tests[i].non_utf8, flags, tests[i].flags);
    }
    arena_free(&ar);
}

static void TestWriterTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TimeLocation *tz = burrow__zip_time_zone(a, -7 * TIME_HOUR);
    ZipFileHeader h = {.name = BURROW_S("test.txt"),
                       .modified = time_date(2017, 10, 31, 21, 11, 57, 0, tz)};
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err;
    zip_writer_create_header(w, &h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected CreateHeader error: %v", err);
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected Close error: %v", err);
    Slice want = testdata(a, "testdata/time-go.zip");
    Slice got = bytes_buffer_bytes(&buf);
    if (!same_bytes(got, want))
        testing_t_errorf_v(t, "contents of time-go.zip differ:\n%x\n%x", got, want);
    time_location_free(tz);
    arena_free(&ar);
}

static void TestWriterOffset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n;
    WriteTest *wts = write_tests(a, &n);
    BytesBuffer buf;
    BytesReader br;
    write_and_read(t, a, &buf, &br, wts, n,
                   gbytes(a, gen_TestWriterOffset_existingData));
    arena_free(&ar);
}

static void TestWriterFlush(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err;
    zip_writer_create(w, BURROW_S("foo"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (bytes_buffer_len(&buf) > 0)
        testing_t_fatalf_v(t, "Unexpected %d bytes already in buffer",
                           bytes_buffer_len(&buf));
    err = zip_writer_flush(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (bytes_buffer_len(&buf) == 0)
        testing_t_fatalf_v(t, "No bytes written after Flush");
    arena_free(&ar);
}

static void TestWriterDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ZipWriter *w = zip_new_writer(a, io_discard);
    Error err;
    IoWriter dw = zip_writer_create(w, BURROW_S("dir/"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    dw.vt->write(dw.data, slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write(nil) to directory: got %v, want nil", err);
    dw.vt->write(dw.data, const_bytes("hello", 5), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t, "Write(\"hello\") to directory: got nil error, want non-nil");
    arena_free(&ar);
}

static Int index_of(Slice b, const Byte *sig, Int n) {
    for (Int i = 0; i + n <= b.len; i++)
        if (memcmp((const Byte *)b.p + i, sig, (size_t)n) == 0)
            return i;
    return -1;
}

static void TestWriterDirAttributes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    ZipFileHeader h = {.name = BURROW_S("dir/"),
                       .method = ZIP_DEFLATE,
                       .compressed_size64 = 1234,
                       .uncompressed_size64 = 5678};
    Error err;
    zip_writer_create_header(w, &h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Slice b = bytes_buffer_bytes(&buf);
    static const Byte lfh[4] = {'P', 'K', 3, 4}, dd[4] = {'P', 'K', 7, 8};
    static const Byte zeros[12];
    Int idx = index_of(b, lfh, 4);
    if (idx == -1)
        testing_t_fatalf_v(t, "file header not found");
    b = slice_sub(b, idx, b.len);
    if (b.len < ZIP_FILE_HEADER_LEN)
        testing_t_fatalf_v(t, "file header cut short");
    const Byte *p = (const Byte *)b.p;
    if (memcmp(p + 6, zeros, 4) != 0) /* flags and method are 0 */
        testing_t_errorf_v(t, "unexpected method and flags: %v", slice_sub(b, 6, 10));
    if (memcmp(p + 14, zeros, 12) != 0) /* CRC-32 and both sizes are 0 */
        testing_t_errorf_v(
            t, "unexpected crc, compress and uncompressed size to be 0 was: %v",
            slice_sub(b, 14, 26));
    if (index_of(b, dd, 4) != -1)
        testing_t_errorf_v(t, "there should be no data descriptor");
    arena_free(&ar);
}

static void TestWriterCopy(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n;
    WriteTest *wts = write_tests(a, &n);
    BytesBuffer buf;
    BytesReader br;
    ZipReader *src = write_and_read(t, a, &buf, &br, wts, n, slice_nil(TYPE_BYTE));

    /* A new archive with the old compressed data copied over. */
    BytesBuffer buf2 = BYTES_BUFFER(a);
    ZipWriter *dst = zip_new_writer(a, bytes_buffer_as_io_writer(&buf2));
    for (Int i = 0; i < src->file.len; i++) {
        Error err = zip_writer_copy(dst, zfile(src, i));
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
    }
    Error err = zip_writer_close(dst);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    BytesReader br2;
    ZipReader *r = new_reader(a, &br2, bytes_buffer_bytes(&buf2), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    for (Int i = 0; i < n; i++)
        test_read_file(t, a, zfile(r, i), &wts[i]);
    arena_free(&ar);
}

static void TestWriterCreateRaw(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Str name;
        Slice content;
        uint16_t method;
        uint16_t flags;
        uint32_t crc32;
        uint64_t uncompressed_size;
        uint64_t compressed_size;
    } files[] = {
        {BURROW_S("small store w desc"), const_bytes("gophers", 7), ZIP_STORE, 0x8, 0,
         0, 0},
        {BURROW_S("small deflate wo desc"), str_slice(repeat(a, "abcdefg", 2048)),
         ZIP_DEFLATE, 0, 0, 0, 0},
    };
    enum { nfiles = sizeof files / sizeof files[0] };

    BytesBuffer archive = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&archive));
    Error err;
    for (int i = 0; i < nfiles; i++) {
        files[i].crc32 = crc32_checksum_ieee(files[i].content);
        uint64_t size = (uint64_t)files[i].content.len;
        files[i].uncompressed_size = size;
        files[i].compressed_size = size;
        Slice compressed = slice_nil(TYPE_BYTE);
        if (files[i].method == ZIP_DEFLATE) {
            BytesBuffer cb = BYTES_BUFFER(a);
            FlateWriter *fw = flate_new_writer(a, bytes_buffer_as_io_writer(&cb),
                                               FLATE_BEST_SPEED, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "flate.NewWriter err = %v", err);
            IoWriter fiw = flate_writer_as_io_writer(fw);
            fiw.vt->write(fiw.data, files[i].content, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "flate Write err = %v", err);
            err = flate_writer_close(fw);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "flate Writer.Close err = %v", err);
            flate_writer_free(fw);
            compressed = bytes_buffer_bytes(&cb);
            files[i].compressed_size = (uint64_t)compressed.len;
        }
        ZipFileHeader h = {.name = files[i].name,
                           .method = files[i].method,
                           .flags = files[i].flags,
                           .crc32 = files[i].crc32,
                           .compressed_size64 = files[i].compressed_size,
                           .uncompressed_size64 = files[i].uncompressed_size};
        IoWriter fw = zip_writer_create_raw(w, &h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        fw.vt->write(fw.data, compressed.p != NULL ? compressed : files[i].content,
                     &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%q Write got %v; want nil", files[i].name, err);
    }
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    BytesReader br;
    ZipReader *r = new_reader(a, &br, bytes_buffer_bytes(&archive), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    for (int i = 0; i < nfiles; i++) {
        ZipFile *got = zfile(r, i);
        const ZipFileHeader *g = &got->file_header;
        Str name = files[i].name;
        if (!str_eq(g->name, name))
            testing_t_errorf_v(t, "got Name %q; want %q", g->name, name);
        if (g->method != files[i].method)
            testing_t_errorf_v(t, "%q: got Method %#x; want %#x", name, g->method,
                               files[i].method);
        if (g->flags != files[i].flags)
            testing_t_errorf_v(t, "%q: got Flags %#x; want %#x", name, g->flags,
                               files[i].flags);
        if (g->crc32 != files[i].crc32)
            testing_t_errorf_v(t, "%q: got CRC32 %#x; want %#x", name, g->crc32,
                               files[i].crc32);
        if (g->compressed_size64 != files[i].compressed_size)
            testing_t_errorf_v(t, "%q: got CompressedSize64 %d; want %d", name,
                               g->compressed_size64, files[i].compressed_size);
        if (g->uncompressed_size64 != files[i].uncompressed_size)
            testing_t_errorf_v(t, "%q: got UncompressedSize64 %d; want %d", name,
                               g->uncompressed_size64, files[i].uncompressed_size);
        IoReadCloser rc = zip_file_open(got, a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: Open err = %v", g->name, err);
            continue;
        }
        Slice b = io_read_all(a, rc_reader(rc), &err);
        (void)rc_close(rc);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: ReadAll err = %v", g->name, err);
            continue;
        }
        if (!same_bytes(b, files[i].content))
            testing_t_errorf_v(t, "%q: ReadAll returned unexpected bytes", g->name);
    }
    arena_free(&ar);
}

static FstestMapFS write_tests_to_fs(Alloc *a, const WriteTest *wts,
                                     FstestMapFile *files, Int n) {
    FstestMapFS fsys = fstest_map_fs_make(a);
    for (Int i = 0; i < n; i++) {
        files[i] = (FstestMapFile){wts[i].data, wts[i].mode, {0}, {NULL, NULL}};
        fstest_map_fs_set(fsys, wts[i].name, &files[i]);
    }
    return fsys;
}

static void TestWriterAddFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    WriteTest tests[] = {
        {BURROW_S("emptyfolder"), slice_nil(TYPE_BYTE), 0, 0755 | FS_MODE_DIR},
        {BURROW_S("file.go"), const_bytes("hello", 5), 0, 0644},
        {BURROW_S("subfolder/another.go"), const_bytes("world", 5), 0, 0644},
        /* "subfolder" is left out on purpose, to see that it is made all
         * the same. */
    };
    FstestMapFile files[3];
    Error err = zip_writer_add_fs(
        w, fstest_map_fs_as_fs(write_tests_to_fs(a, tests, files, 3)));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* What reading the archive gives, with subfolder in it. */
    WriteTest want[] = {
        tests[0],
        tests[1],
        {BURROW_S("subfolder"), slice_nil(TYPE_BYTE), 0, 0555 | FS_MODE_DIR},
        tests[2],
    };
    BytesReader br;
    ZipReader *r = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (r->file.len != 4)
        testing_t_fatalf_v(t, "got %d files, want 4", r->file.len);
    for (int i = 0; i < 4; i++) {
        WriteTest wt = want[i];
        if (fs_file_mode_is_dir(wt.mode))
            wt.name = concat(a, wt.name, BURROW_S("/"));
        test_read_file(t, a, zfile(r, i), &wt);
    }
    arena_free(&ar);
}

static void TestIssue61875(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    WriteTest tests[] = {
        {BURROW_S("symlink"), const_bytes("../link/target", 14), ZIP_DEFLATE,
         0755 | FS_MODE_SYMLINK},
        {BURROW_S("device"), const_bytes("", 0), ZIP_DEFLATE, 0755 | FS_MODE_DEVICE},
    };
    FstestMapFile files[2];
    Error err = zip_writer_add_fs(
        w, fstest_map_fs_as_fs(write_tests_to_fs(a, tests, files, 2)));
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error, got nil");
    zip_writer_free(w);
    arena_free(&ar);
}

/* --------------------------------------------------------- zip_test.go */

static void TestOver65kFiles(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    enum { nfiles = (1 << 16) + 42 };
    char name[32];
    for (int i = 0; i < nfiles; i++) {
        snprintf(name, sizeof name, "%d.dat", i);
        ZipFileHeader h = {.name = str_from_cstr(name), .method = ZIP_STORE};
        Error err;
        zip_writer_create_header(w, &h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "creating file %d: %v", i, err);
    }
    Error err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Writer.Close: %v", err);
    BytesReader br;
    ZipReader *zr = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewReader: %v", err);
    if (zr->file.len != nfiles)
        testing_t_fatalf_v(t, "File contains %d files, want %d", zr->file.len, nfiles);
    for (int i = 0; i < nfiles; i++) {
        snprintf(name, sizeof name, "%d.dat", i);
        if (!str_eq(zfile(zr, i)->file_header.name, str_from_cstr(name)))
            testing_t_fatalf_v(t, "File(%d) = %q, want %q", i,
                               zfile(zr, i)->file_header.name, str_from_cstr(name));
    }
    arena_free(&ar);
}

static void TestModTime(TestingT *t) {
    Time test_time = time_date(2009, TIME_NOVEMBER, 10, 23, 45, 58, 0, time_utc_loc);
    ZipFileHeader fh = {0};
    zip_file_header_set_mod_time(&fh, test_time);
    Time out = zip_file_header_mod_time(&fh);
    if (!time_equal(out, test_time))
        testing_t_errorf_v(t, "times don't match: got %d, want %d", time_unix(out),
                           time_unix(test_time));
}

static void test_header_round_trip(TestingT *t, ZipFileHeader *fh, uint32_t want_size,
                                   uint64_t want_size64) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FsFileInfo fi = zip_file_header_file_info(fh);
    Error err;
    ZipFileHeader *fh2 = zip_file_info_header(a, fi, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (!str_eq(fh2->name, fh->name))
        testing_t_errorf_v(t, "Name: got %q, want %q", fh2->name, fh->name);
    if (fh2->uncompressed_size != want_size)
        testing_t_errorf_v(t, "UncompressedSize: got %d, want %d",
                           fh2->uncompressed_size, want_size);
    if (fh2->uncompressed_size64 != want_size64)
        testing_t_errorf_v(t, "UncompressedSize64: got %d, want %d",
                           fh2->uncompressed_size64, want_size64);
    if (fh2->modified_time != fh->modified_time)
        testing_t_errorf_v(t, "ModifiedTime: got %d, want %d", fh2->modified_time,
                           fh->modified_time);
    if (fh2->modified_date != fh->modified_date)
        testing_t_errorf_v(t, "ModifiedDate: got %d, want %d", fh2->modified_date,
                           fh->modified_date);
    Any sys = fi.vt->sys(fi.data);
    if (sys.data != fh)
        testing_t_errorf_v(t, "Sys didn't return original *FileHeader");
    arena_free(&ar);
}

static void TestFileHeaderRoundTrip(TestingT *t) {
    ZipFileHeader fh = {.name = BURROW_S("foo.txt"),
                        .uncompressed_size = 987654321,
                        .modified_time = 1234,
                        .modified_date = 5678};
    test_header_round_trip(t, &fh, fh.uncompressed_size, fh.uncompressed_size);
}

static void TestFileHeaderRoundTrip64(TestingT *t) {
    ZipFileHeader fh = {.name = BURROW_S("foo.txt"),
                        .uncompressed_size64 = 9876543210ULL,
                        .modified_time = 1234,
                        .modified_date = 5678};
    test_header_round_trip(t, &fh, ZIP_UINT32_MAX, fh.uncompressed_size64);
}

/* Go compares with ==, which takes in the location: the instant has to be the
 * same and in UTC. */
static bool same_utc_time(Time got, Time want) {
    return time_equal(got, want) && time_location(got) == time_utc_loc;
}

static void TestFileHeaderRoundTripModified(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ZipFileHeader fh = {.name = BURROW_S("foo.txt"),
                        .uncompressed_size = 987654321,
                        .modified = time_local(time_now()),
                        .modified_time = 1234,
                        .modified_date = 5678};
    FsFileInfo fi = zip_file_header_file_info(&fh);
    Error err;
    ZipFileHeader *fh2 = zip_file_info_header(a, fi, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Time want = time_utc(fh.modified);
    if (!same_utc_time(fh2->modified, want))
        testing_t_errorf_v(t, "Modified: got %d, want %d",
                           time_unix_nano(fh2->modified), time_unix_nano(want));
    Time mt = fi.vt->mod_time(fi.data);
    if (!same_utc_time(mt, want))
        testing_t_errorf_v(t, "Modified: got %d, want %d", time_unix_nano(mt),
                           time_unix_nano(want));
    arena_free(&ar);
}

static void TestFileHeaderRoundTripWithoutModified(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ZipFileHeader fh = {.name = BURROW_S("foo.txt"),
                        .uncompressed_size = 987654321,
                        .modified_time = 1234,
                        .modified_date = 5678};
    FsFileInfo fi = zip_file_header_file_info(&fh);
    Error err;
    ZipFileHeader *fh2 = zip_file_info_header(a, fi, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Time want = zip_file_header_mod_time(&fh);
    Time got = zip_file_header_mod_time(fh2);
    if (!time_equal(got, want) || time_location(got) != time_location(want))
        testing_t_errorf_v(t, "Modified: got %d, want %d", time_unix(got),
                           time_unix(want));
    got = fi.vt->mod_time(fi.data);
    if (!time_equal(got, want) || time_location(got) != time_location(want))
        testing_t_errorf_v(t, "Modified: got %d, want %d", time_unix(got),
                           time_unix(want));
    arena_free(&ar);
}

/* rleBuffer: a run-length-encoded buffer, written like a bytes.Buffer and
 * read at any offset. */
typedef struct RepeatedByte {
    int64_t off;
    Byte b;
    int64_t n;
} RepeatedByte;

typedef struct RleBuffer {
    RepeatedByte *buf;
    Int len;
    Int cap;
} RleBuffer;

static int64_t rle_size(const RleBuffer *r) {
    if (r->len == 0)
        return 0;
    const RepeatedByte *last = &r->buf[r->len - 1];
    return last->off + last->n;
}

static void rle_append(RleBuffer *r, RepeatedByte rb) {
    if (r->len == r->cap) {
        Int ncap = r->cap == 0 ? 16 : 2 * r->cap;
        r->buf = (RepeatedByte *)mem_realloc(
            heap_allocator(), r->buf, (size_t)r->cap * sizeof *r->buf,
            (size_t)ncap * sizeof *r->buf, _Alignof(RepeatedByte));
        if (r->buf == NULL)
            abort();
        r->cap = ncap;
    }
    r->buf[r->len++] = rb;
}

static Int rle_write(void *self, Slice ps, Error *err) {
    RleBuffer *r = (RleBuffer *)self;
    const Byte *p = (const Byte *)ps.p;
    *err = BURROW_NO_ERROR;
    RepeatedByte *rp = NULL;
    if (r->len > 0) {
        rp = &r->buf[r->len - 1];
        /* The quick way, when p is one byte over and over. */
        if (ps.len > 0 && p[0] == rp->b) {
            Int i = 0;
            while (i < ps.len && p[i] == rp->b)
                i++;
            if (i == ps.len) {
                rp->n += ps.len;
                return ps.len;
            }
        }
    }
    for (Int i = 0; i < ps.len; i++) {
        if (rp == NULL || rp->b != p[i]) {
            rle_append(r, (RepeatedByte){rle_size(r), p[i], 1});
            rp = &r->buf[r->len - 1];
        } else {
            rp->n++;
        }
    }
    return ps.len;
}

static Int rle_read_at(void *self, Slice ps, int64_t off, Error *err) {
    RleBuffer *r = (RleBuffer *)self;
    Byte *p = (Byte *)ps.p;
    *err = BURROW_NO_ERROR;
    if (ps.len == 0)
        return 0;
    /* The first run that ends past off. */
    Int lo = 0, hi = r->len;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (r->buf[mid].off + r->buf[mid].n <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    Int n = 0;
    if (lo < r->len) {
        int64_t skip = off - r->buf[lo].off;
        for (Int i = lo; i < r->len; i++) {
            int64_t rep = r->buf[i].n - skip;
            if (rep > (int64_t)(ps.len - n))
                rep = (int64_t)(ps.len - n);
            memset(p + n, r->buf[i].b, (size_t)rep);
            n += (Int)rep;
            if (n == ps.len)
                return n;
            skip = 0;
        }
    }
    if (n != ps.len)
        *err = io_err_unexpected_eof;
    return n;
}

static const IoWriterVT rle_writer_vt = {NULL, rle_write};
static const IoReaderAtVT rle_reader_at_vt = {NULL, rle_read_at};

static IoWriter rle_as_writer(RleBuffer *r) {
    return (IoWriter){&rle_writer_vt, r};
}

static IoReaderAt rle_as_reader_at(RleBuffer *r) {
    return (IoReaderAt){&rle_reader_at_vt, r};
}

static void rle_free(RleBuffer *r) {
    mem_free(heap_allocator(), r->buf, (size_t)r->cap * sizeof *r->buf,
             _Alignof(RepeatedByte));
    *r = (RleBuffer){0};
}

/* The rleBuffer the ZIP64 tests use, on its own. */
static void TestRLEBuffer(TestingT *t) {
    RleBuffer b = {0};
    char all[64];
    Int nall = 0;
    for (size_t i = 0;
         i < sizeof gen_TestRLEBuffer_writes / sizeof gen_TestRLEBuffer_writes[0];
         i++) {
        Slice w = gbytes(heap_allocator(), gen_TestRLEBuffer_writes[i]);
        Error err;
        rle_write(&b, w, &err);
        memcpy(all + nall, w.p, (size_t)w.len);
        nall += w.len;
    }
    if (b.len != 10)
        testing_t_fatalf_v(t, "len(b.buf) = %d; want 10", b.len);
    char buf[64];
    for (Int i = 0; i < nall; i++) {
        for (Int j = 0; j < nall - i; j++) {
            Error err;
            Int n = rle_read_at(&b, const_bytes(buf, j), i, &err);
            if (BURROW_FAILED(err) || n != j)
                testing_t_errorf_v(t, "ReadAt(%d, %d) = %d, %v; want %d, nil", i, j, n,
                                   err, j);
            if (memcmp(buf, all + i, (size_t)j) != 0)
                testing_t_errorf_v(t, "ReadAt(%d, %d) = %q; want %q", i, j,
                                   str_from_bytes(buf, j), str_from_bytes(all + i, j));
        }
    }
    rle_free(&b);
}

/* zeros: a reader of endless zeros. */
static Int zeros_read(void *self, Slice p, Error *err) {
    (void)self;
    memset(p.p, 0, (size_t)p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT zeros_vt = {NULL, zeros_read};
static const IoReader zeros = {&zeros_vt, NULL};

/* suffixSaver: remembers the last keep bytes written to it. */
typedef struct SuffixSaver {
    Int keep;
    Byte *buf;
    Int len;
    Int start;
    int64_t size;
} SuffixSaver;

BURROW_SENTINEL_ERROR(err_discarded_bytes, "ReadAt of discarded bytes");

/* The bytes kept, oldest first, into out, which has room for keep. */
static Int ss_suffix(const SuffixSaver *ss, Byte *out) {
    if (ss->len < ss->keep) {
        memcpy(out, ss->buf, (size_t)ss->len);
        return ss->len;
    }
    Int n = ss->keep - ss->start;
    memcpy(out, ss->buf + ss->start, (size_t)n);
    memcpy(out + n, ss->buf, (size_t)ss->start);
    return ss->keep;
}

static Int ss_read_at(void *self, Slice p, int64_t off, Error *err) {
    SuffixSaver *ss = (SuffixSaver *)self;
    *err = BURROW_NO_ERROR;
    int64_t back = ss->size - off;
    if (back > (int64_t)ss->keep) {
        *err = err_discarded_bytes;
        return 0;
    }
    Byte *suf = (Byte *)mem_alloc(heap_allocator(), (size_t)ss->keep + 1, 1);
    Int sn = ss_suffix(ss, suf);
    Int n = (Int)back < p.len ? (Int)back : p.len;
    memcpy(p.p, suf + sn - back, (size_t)n);
    mem_free(heap_allocator(), suf, (size_t)ss->keep + 1, 1);
    if (n != p.len)
        *err = io_eof;
    return n;
}

static Int ss_write(void *self, Slice ps, Error *err) {
    SuffixSaver *ss = (SuffixSaver *)self;
    const Byte *p = (const Byte *)ps.p;
    Int left = ps.len;
    *err = BURROW_NO_ERROR;
    ss->size += ps.len;
    if (ss->len < ss->keep) {
        Int add = ss->keep - ss->len;
        if (add > left)
            add = left;
        memcpy(ss->buf + ss->len, p, (size_t)add);
        ss->len += add;
        p += add;
        left -= add;
    }
    while (left > 0) {
        Int n = ss->keep - ss->start;
        if (n > left)
            n = left;
        memcpy(ss->buf + ss->start, p, (size_t)n);
        p += n;
        left -= n;
        ss->start += n;
        if (ss->start == ss->keep)
            ss->start = 0;
    }
    return ps.len;
}

static const IoWriterVT ss_writer_vt = {NULL, ss_write};
static const IoReaderAtVT ss_reader_at_vt = {NULL, ss_read_at};

static SuffixSaver ss_new(Int keep) {
    SuffixSaver ss = {keep, NULL, 0, 0, 0};
    ss.buf = (Byte *)mem_alloc(heap_allocator(), (size_t)keep, 1);
    if (ss.buf == NULL)
        abort();
    return ss;
}

static void ss_free(SuffixSaver *ss) {
    mem_free(heap_allocator(), ss->buf, (size_t)ss->keep, 1);
}

static void TestSuffixSaver(TestingT *t) {
    enum { keep = 10 };
    SuffixSaver ss = ss_new(keep);
    Error err;
    Byte suf[keep];
    ss_write(&ss, const_bytes("abc", 3), &err);
    Int n = ss_suffix(&ss, suf);
    if (!str_eq(str_from_bytes(suf, n), BURROW_S("abc")))
        testing_t_errorf_v(t, "got = %q; want abc", str_from_bytes(suf, n));
    ss_write(&ss, const_bytes("defghijklmno", 12), &err);
    n = ss_suffix(&ss, suf);
    if (!str_eq(str_from_bytes(suf, n), BURROW_S("fghijklmno")))
        testing_t_errorf_v(t, "got = %q; want fghijklmno", str_from_bytes(suf, n));
    if (ss.size != 15)
        testing_t_errorf_v(t, "Size = %d; want %d", ss.size, 15);
    Byte buf[15];
    for (int64_t off = 0; off < ss.size; off++) {
        for (Int size = 1; size <= (Int)(ss.size - off); size++) {
            Int got = ss_read_at(&ss, const_bytes(buf, size), off, &err);
            if (off < ss.size - keep) {
                if (!same_error(err, err_discarded_bytes))
                    testing_t_errorf_v(
                        t, "off %d, size %d = %d, %v; want errDiscardedBytes", off,
                        size, got, err);
                continue;
            }
            Str want = str_from_bytes(&"abcdefghijklmno"[off], size);
            if (BURROW_FAILED(err) || !str_eq(str_from_bytes(buf, got), want))
                testing_t_errorf_v(t, "off %d, size %d = %d, %v (%q); want %q", off,
                                   size, got, err, str_from_bytes(buf, got), want);
        }
    }
    ss_free(&ss);
}

static uint16_t le16(const Byte *b) {
    return (uint16_t)(b[0] | b[1] << 8);
}

static uint32_t le32(const Byte *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static uint64_t le64(const Byte *b) {
    return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32;
}

/* suffixIsZip64: whether the archive of size bytes that r reads ends with a
 * ZIP64 end record. */
static bool suffix_is_zip64(TestingT *t, IoReaderAt r, int64_t size) {
    Byte d[1024];
    Error err;
    r.vt->read_at(r.data, const_bytes(d, sizeof d), size - (int64_t)sizeof d, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAt: %v", err);
    Int sig_off = burrow__zip_find_signature_in_block(d, sizeof d);
    if (sig_off == -1) {
        testing_t_errorf_v(t, "failed to find signature in block");
        return false;
    }
    int64_t dir_off =
        burrow__zip_find_directory64_end(r, size - (int64_t)sizeof d + sig_off, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "findDirectory64End: %v", err);
    if (dir_off == -1)
        return false;
    Byte e[ZIP_DIRECTORY64_END_LEN];
    r.vt->read_at(r.data, const_bytes(e, sizeof e), dir_off, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAt(off=%d): %v", dir_off, err);
    if (le32(e) != ZIP_DIRECTORY64_END_SIGNATURE)
        return false;
    uint64_t rec = le64(e + 4);
    if (rec != ZIP_DIRECTORY64_END_LEN - 12)
        testing_t_errorf_v(t, "expected length of %d, got %d",
                           ZIP_DIRECTORY64_END_LEN - 12, rec);
    return true;
}

/* testZip64: size bytes of '.' and "END\n" in one stored file, read back. */
static void test_zip64(TestingT *t, int64_t size, RleBuffer *buf) {
    enum { chunk_size = 1024 };
    int64_t chunks = size / chunk_size;
    Alloc *a = heap_allocator();
    ZipWriter *w = zip_new_writer(a, rle_as_writer(buf));
    ZipFileHeader h = {.name = BURROW_S("huge.txt"), .method = ZIP_STORE};
    Error err;
    IoWriter f = zip_writer_create_header(w, &h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    burrow__zip_file_writer_fake_crc32(f);
    Byte chunk[chunk_size];
    memset(chunk, '.', sizeof chunk);
    for (int64_t i = 0; i < chunks; i++) {
        f.vt->write(f.data, const_bytes(chunk, chunk_size), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "write chunk: %v", err);
    }
    Int frag = (Int)(size % chunk_size);
    if (frag > 0) {
        f.vt->write(f.data, const_bytes(chunk, frag), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "write chunk: %v", err);
    }
    f.vt->write(f.data, const_bytes("END\n", 4), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "write end: %v", err);
    err = zip_writer_close(w);
    zip_writer_free(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* Read it back and get to the end of it. */
    ZipReader *r = zip_new_reader(a, rle_as_reader_at(buf), rle_size(buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "reader: %v", err);
    ZipFile *f0 = zfile(r, 0);
    IoReadCloser rc = zip_file_open(f0, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "opening: %v", err);
    burrow__zip_checksum_reader_fake_crc32(rc);
    for (int64_t i = 0; i < chunks; i++) {
        io_read_full(rc_reader(rc), const_bytes(chunk, chunk_size), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "read: %v", err);
    }
    if (frag > 0) {
        io_read_full(rc_reader(rc), const_bytes(chunk, frag), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "read: %v", err);
    }
    Slice got_end = io_read_all(a, rc_reader(rc), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "read end: %v", err);
    if (!str_eq(slice_str(got_end), BURROW_S("END\n")))
        testing_t_errorf_v(t, "End of zip64 archive %q, want %q", slice_str(got_end),
                           BURROW_S("END\n"));
    free_bytes(a, &got_end);
    err = rc_close(rc);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "closing: %v", err);
    if (size + 4 >= (1LL << 32) - 1 &&
        f0->file_header.uncompressed_size != ZIP_UINT32_MAX)
        testing_t_errorf_v(t, "UncompressedSize %#x, want %#x",
                           f0->file_header.uncompressed_size, ZIP_UINT32_MAX);
    if (f0->file_header.uncompressed_size64 != (uint64_t)size + 4)
        testing_t_errorf_v(t, "UncompressedSize64 %#x, want %#x",
                           f0->file_header.uncompressed_size64, (uint64_t)size + 4);
    zip_reader_free(r);
}

static void TestZip64(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "slow test; skipping");
    RleBuffer buf = {0};
    test_zip64(t, 1LL << 32, &buf);
    if (!suffix_is_zip64(t, rle_as_reader_at(&buf), rle_size(&buf)))
        testing_t_errorf_v(t, "not a zip64");
    rle_free(&buf);
}

/* An uncompressed size of 0xFFFFFFFF, which is the marker for the 64-bit
 * field, so that field has to be used even though the size fits in 32 bits. */
static void TestZip64EdgeCase(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "slow test; skipping");
    RleBuffer buf = {0};
    test_zip64(t, (1LL << 32) - 1 - 4, &buf);
    if (!suffix_is_zip64(t, rle_as_reader_at(&buf), rle_size(&buf)))
        testing_t_errorf_v(t, "not a zip64");
    rle_free(&buf);
}

typedef struct HookEnv {
    TestingT *t;
    uint64_t want;
    bool size; /* check the size rather than the offset */
} HookEnv;

static void close_hook(void *env, uint64_t size, uint64_t off) {
    HookEnv *e = (HookEnv *)env;
    if (!e->size && off != e->want)
        testing_t_errorf_v(e->t, "central directory offset = %d (%x); want %d", off,
                           off, e->want);
    if (e->size && size != e->want)
        testing_t_errorf_v(e->t, "Close central directory size = %d; want %d", size,
                           e->want);
}

/* The directory at offset 0xFFFFFFFF makes a ZIP64 archive, and not before. */
static bool directory_offset_is_zip64(TestingT *t, uint64_t want_off) {
    Alloc *a = heap_allocator();
    SuffixSaver ss = ss_new(10 << 20);
    ZipWriter *w = zip_new_writer(a, (IoWriter){&ss_writer_vt, &ss});
    HookEnv env = {t, want_off, false};
    w->test_hook_close_size_offset = close_hook;
    w->test_hook_env = &env;
    Str name = BURROW_S("huge.txt");
    ZipFileHeader h = {.name = name, .method = ZIP_STORE};
    Error err;
    IoWriter f = zip_writer_create_header(w, &h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    burrow__zip_file_writer_fake_crc32(f);
    uint64_t size =
        want_off - ZIP_FILE_HEADER_LEN - (uint64_t)name.len - ZIP_DATA_DESCRIPTOR_LEN;
    io_copy_n(a, f, zeros, (int64_t)size, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    err = zip_writer_close(w);
    zip_writer_free(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    bool z = suffix_is_zip64(t, (IoReaderAt){&ss_reader_at_vt, &ss}, ss.size);
    ss_free(&ss);
    return z;
}

static void TestZip64DirectoryOffset(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    if (directory_offset_is_zip64(t, 0xfffffffe))
        testing_t_errorf_v(t, "uint32max-2_NoZip64: unexpected zip64");
    if (!directory_offset_is_zip64(t, 0xffffffff))
        testing_t_errorf_v(t, "uint32max-1_Zip64: expected zip64");
}

static bool many_records_is_zip64(TestingT *t, int nrec) {
    Alloc *a = heap_allocator();
    SuffixSaver ss = ss_new(10 << 20);
    ZipWriter *w = zip_new_writer(a, (IoWriter){&ss_writer_vt, &ss});
    for (int i = 0; i < nrec; i++) {
        ZipFileHeader h = {.name = BURROW_S("a.txt"), .method = ZIP_STORE};
        Error err;
        zip_writer_create_header(w, &h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
    }
    Error err = zip_writer_close(w);
    zip_writer_free(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    bool z = suffix_is_zip64(t, (IoReaderAt){&ss_reader_at_vt, &ss}, ss.size);
    ss_free(&ss);
    return z;
}

/* 0xFFFF records need a ZIP64 archive. */
static void TestZip64ManyRecords(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    if (many_records_is_zip64(t, 0xfffe))
        testing_t_errorf_v(t, "uint16max-1_NoZip64: unexpected zip64");
    if (!many_records_is_zip64(t, 0xffff))
        testing_t_errorf_v(t, "uint16max_Zip64: expected zip64");
}

/* An archive whose central directory is want_len bytes long, as files with
 * 64 KiB names and comments almost as long. */
static void large_directory(TestingT *t, int64_t want_len, bool want_zip64) {
    Alloc *a = heap_allocator();
    RleBuffer buf = {0};
    ZipWriter *w = zip_new_writer(a, rle_as_writer(&buf));
    HookEnv env = {t, (uint64_t)want_len, true};
    w->test_hook_close_size_offset = close_hook;
    w->test_hook_env = &env;
    Byte *dots = (Byte *)mem_alloc(a, ZIP_UINT16_MAX, 1);
    memset(dots, '.', ZIP_UINT16_MAX);
    Str uint16string = str_from_bytes(dots, ZIP_UINT16_MAX);
    int64_t remain = want_len;
    while (remain > 0) {
        Int comment_len = (Int)ZIP_UINT16_MAX - ZIP_DIRECTORY_HEADER_LEN - 1;
        Int this_rec_len = ZIP_DIRECTORY_HEADER_LEN + (Int)ZIP_UINT16_MAX + comment_len;
        if ((int64_t)this_rec_len > remain) {
            Int remove = this_rec_len - (Int)remain;
            comment_len -= remove;
            this_rec_len -= remove;
        }
        remain -= this_rec_len;
        ZipFileHeader h = {.name = uint16string,
                           .comment = str_from_bytes(dots, comment_len)};
        Error err;
        IoWriter f = zip_writer_create_header(w, &h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "CreateHeader: %v", err);
        burrow__zip_file_writer_fake_crc32(f);
    }
    Error err = zip_writer_close(w);
    zip_writer_free(w);
    mem_free(a, dots, ZIP_UINT16_MAX, 1);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Close: %v", err);
    bool z = suffix_is_zip64(t, rle_as_reader_at(&buf), rle_size(&buf));
    if (z != want_zip64)
        testing_t_errorf_v(t, want_zip64 ? "expected zip64" : "unexpected zip64");
    ZipReader *r = zip_new_reader(a, rle_as_reader_at(&buf), rle_size(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewReader: %v", err);
    zip_reader_free(r);
    rle_free(&buf);
}

/* ZIP64 is needed when the records come to 0xFFFFFFFF bytes. */
static void TestZip64LargeDirectory(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    if (sizeof(void *) < 8)
        testing_t_skip_v(t, "skipping on 32-bit platforms");
    large_directory(t, ZIP_UINT32_MAX - 1, false);
    large_directory(t, ZIP_UINT32_MAX, true);
}

/* testValidHeader: h, with "hi" in it, written and read back. */
static void test_valid_header(TestingT *t, ZipFileHeader *h) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *z = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err;
    IoWriter f = zip_writer_create_header(z, h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error creating header: %v", err);
    f.vt->write(f.data, const_bytes("hi", 2), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error writing content: %v", err);
    err = zip_writer_close(z);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error closing zip writer: %v", err);
    BytesReader br;
    ZipReader *zf = new_reader(a, &br, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "got %v, expected nil", err);
    const ZipFileHeader *zh = &zfile(zf, 0)->file_header;
    if (!str_eq(zh->name, h->name) || zh->method != h->method ||
        zh->uncompressed_size64 != 2)
        testing_t_fatalf_v(t, "got %q/%d/%d expected %q/%d/%d", zh->name, zh->method,
                           zh->uncompressed_size64, h->name, h->method, 2);
    arena_free(&ar);
}

/* Issue 4302. */
static void TestHeaderInvalidTagAndSize(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time ts = time_now();
    Str filename = time_format(ts, a, BURROW_S("20060102T150405.000.txt"));
    ZipFileHeader h = {
        .name = filename,
        .method = ZIP_DEFLATE,
        /* No tag or length, but the extra is read on a best effort basis. */
        .extra = str_slice(time_format(ts, a, TIME_RFC3339_NANO)),
    };
    zip_file_header_set_mod_time(&h, ts);
    test_valid_header(t, &h);
    arena_free(&ar);
}

static void TestHeaderTooShort(TestingT *t) {
    static const Byte extra[] = {ZIP_EXTRA_ZIP64};
    ZipFileHeader h = {
        .name = BURROW_S("foo.txt"),
        .method = ZIP_DEFLATE,
        /* No size and half a tag, but the extra is read on a best effort
         * basis. */
        .extra = const_bytes(extra, 1),
    };
    test_valid_header(t, &h);
}

static void TestHeaderTooLongErr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte *ff = (Byte *)mem_alloc(a, 1 << 16, 1);
    memset(ff, 0xff, 1 << 16);
    struct {
        Str name;
        Slice extra;
        const Error *wanterr;
    } tests[] = {
        {repeat(a, "x", 1 << 16), const_bytes("", 0), &burrow__zip_err_long_name},
        {BURROW_S("long_extra"), const_bytes(ff, 1 << 16), &burrow__zip_err_long_extra},
    };
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ZipFileHeader h = {.name = tests[i].name, .extra = tests[i].extra};
        Error err;
        zip_writer_create_header(w, &h, &err);
        if (!same_error(err, *tests[i].wanterr))
            testing_t_errorf_v(t, "error=%v, want %v", err, *tests[i].wanterr);
    }
    Error err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    arena_free(&ar);
}

static void TestHeaderIgnoredSize(TestingT *t) {
    /* The size is wrong but is never looked at. */
    static const Byte extra[] = {ZIP_EXTRA_ZIP64 & 0xff,
                                 ZIP_EXTRA_ZIP64 >> 8,
                                 24,
                                 0,
                                 1,
                                 2,
                                 3,
                                 4,
                                 5,
                                 6,
                                 7,
                                 8,
                                 1,
                                 2,
                                 3,
                                 4,
                                 5,
                                 6,
                                 7,
                                 8,
                                 1,
                                 2,
                                 3,
                                 4,
                                 5,
                                 6,
                                 7,
                                 8,
                                 1,
                                 2,
                                 3,
                                 4,
                                 5,
                                 6,
                                 7,
                                 8};
    ZipFileHeader h = {.name = BURROW_S("foo.txt"),
                       .method = ZIP_DEFLATE,
                       .extra = const_bytes(extra, sizeof extra)};
    test_valid_header(t, &h);
}

/* Issue 4393. An extra field may have no body. */
static void TestZeroLengthHeader(TestingT *t) {
    static const Byte extra[] = {
        85, 84,  5, 0, 3, 154, 144, 195, 77, /* tag 21589 size 5 */
        85, 120, 0, 0,                       /* tag 30805 size 0 */
    };
    ZipFileHeader h = {.name = BURROW_S("extadata.txt"),
                       .method = ZIP_DEFLATE,
                       .extra = const_bytes(extra, sizeof extra)};
    test_valid_header(t, &h);
}

/* ---------------------------------------------------------- zip64_test.go */

/* sparseFile: an archive as the spans of it that are not zero and its size. */
typedef struct SparseSpan {
    int64_t offset;
    Byte *data;
    Int len;
    Int cap;
} SparseSpan;

typedef struct SparseFile {
    int64_t size;
    SparseSpan *spans;
    Int n;
    Int cap;
} SparseFile;

static Int sparse_read_at(void *self, Slice ps, int64_t off, Error *err) {
    SparseFile *f = (SparseFile *)self;
    Byte *p = (Byte *)ps.p;
    *err = BURROW_NO_ERROR;
    if (off < 0) {
        *err = errors_new(error_allocator(), BURROW_S("sparseFile: negative offset"));
        return 0;
    }
    if (off >= f->size) {
        *err = io_eof;
        return 0;
    }
    int64_t end = off + (int64_t)ps.len < f->size ? off + (int64_t)ps.len : f->size;
    Int n = (Int)(end - off);
    memset(p, 0, (size_t)n);
    for (Int i = 0; i < f->n; i++) {
        const SparseSpan *s = &f->spans[i];
        int64_t s_end = s->offset + (int64_t)s->len;
        if (s_end <= off || s->offset >= end)
            continue;
        int64_t from = s->offset > off ? s->offset : off;
        int64_t to = s_end < end ? s_end : end;
        memcpy(p + (from - off), s->data + (from - s->offset), (size_t)(to - from));
    }
    if (n < ps.len)
        *err = io_eof;
    return n;
}

static const IoReaderAtVT sparse_reader_at_vt = {NULL, sparse_read_at};

static IoReaderAt sparse_as_reader_at(SparseFile *f) {
    return (IoReaderAt){&sparse_reader_at_vt, f};
}

static SparseSpan *sparse_add_span(SparseFile *f, int64_t offset) {
    if (f->n == f->cap) {
        Int ncap = f->cap == 0 ? 4 : 2 * f->cap;
        f->spans = (SparseSpan *)mem_realloc(
            heap_allocator(), f->spans, (size_t)f->cap * sizeof *f->spans,
            (size_t)ncap * sizeof *f->spans, _Alignof(SparseSpan));
        if (f->spans == NULL)
            abort();
        f->cap = ncap;
    }
    f->spans[f->n] = (SparseSpan){offset, NULL, 0, 0};
    return &f->spans[f->n++];
}

static void sparse_span_append(SparseSpan *s, const Byte *p, Int n) {
    if (n == 0)
        return;
    if (s->len + n > s->cap) {
        Int ncap = s->cap == 0 ? 4096 : s->cap;
        while (ncap < s->len + n)
            ncap *= 2;
        s->data = (Byte *)mem_realloc(heap_allocator(), s->data, (size_t)s->cap,
                                      (size_t)ncap, 1);
        if (s->data == NULL)
            abort();
        s->cap = ncap;
    }
    memcpy(s->data + s->len, p, (size_t)n);
    s->len += n;
}

static void sparse_free(SparseFile *f) {
    for (Int i = 0; i < f->n; i++)
        mem_free(heap_allocator(), f->spans[i].data, (size_t)f->spans[i].cap, 1);
    mem_free(heap_allocator(), f->spans, (size_t)f->cap * sizeof *f->spans,
             _Alignof(SparseSpan));
    *f = (SparseFile){0};
}

/* The last keep bytes of f, and where they start. */
static Slice sparse_materialize_tail(SparseFile *f, int64_t keep, uint64_t *base_off) {
    if (keep > f->size)
        keep = f->size;
    int64_t base = f->size - keep;
    Byte *buf = (Byte *)mem_alloc(heap_allocator(), (size_t)keep + 1, 1);
    Error err;
    sparse_read_at(f, const_bytes(buf, (Int)keep), base, &err);
    *base_off = (uint64_t)base;
    return const_bytes(buf, (Int)keep);
}

/* readSparse, on a golden that is already out of its gzip. */
static bool read_sparse(Slice b, SparseFile *f) {
    const Byte *p = (const Byte *)b.p;
    Int left = b.len;
    *f = (SparseFile){0};
    if (left < 12)
        return false;
    f->size = (int64_t)le64(p);
    uint32_t n = le32(p + 8);
    p += 12;
    left -= 12;
    for (uint32_t i = 0; i < n; i++) {
        if (left < 12)
            return false;
        int64_t off = (int64_t)le64(p);
        Int sz = (Int)le32(p + 8);
        p += 12;
        left -= 12;
        if (left < sz)
            return false;
        if (f->n > 0 && f->spans[f->n - 1].offset > off)
            return false;
        SparseSpan *s = sparse_add_span(f, off);
        sparse_span_append(s, p, sz);
        p += sz;
        left -= sz;
    }
    return true;
}

/* sparseBuffer: keeps what is written to it as a sparseFile, leaving out
 * every 4096 byte piece that is all zeros. */
typedef struct SparseBuffer {
    SparseFile f;
    bool cur; /* the last span is still being added to */
} SparseBuffer;

static bool all_zero(const Byte *p, Int n) {
    for (Int i = 0; i < n; i++)
        if (p[i] != 0)
            return false;
    return true;
}

static Int sparse_buffer_write(void *self, Slice ps, Error *err) {
    SparseBuffer *t = (SparseBuffer *)self;
    const Byte *p = (const Byte *)ps.p;
    Int left = ps.len;
    *err = BURROW_NO_ERROR;
    while (left > 0) {
        Int k = left < 4096 ? left : 4096;
        if (all_zero(p, k)) {
            t->cur = false;
        } else {
            if (!t->cur) {
                sparse_add_span(&t->f, t->f.size);
                t->cur = true;
            }
            sparse_span_append(&t->f.spans[t->f.n - 1], p, k);
        }
        t->f.size += k;
        p += k;
        left -= k;
    }
    return ps.len;
}

static const IoWriterVT sparse_buffer_vt = {NULL, sparse_buffer_write};

enum { Z64_USIZE = 1, Z64_CSIZE, Z64_OFFSET };

typedef struct CdEntry {
    Str name;
    uint16_t method;
    uint16_t reader_version;
    uint32_t raw_csize;
    uint32_t raw_usize;
    uint32_t raw_offset;
    uint64_t csize64;
    uint64_t usize64;
    uint64_t offset64;
    int z64_fields[3 * 8];
    int nz64;
} CdEntry;

typedef struct CdSnapshot {
    CdEntry entries[8];
    Int n;
    uint16_t eocd_records;
    uint32_t eocd_size;
    uint32_t eocd_offset;
    bool has_eocd64;
    uint64_t eocd64_records;
    uint64_t eocd64_size;
    uint64_t eocd64_offset;
} CdSnapshot;

/* findEOCD: where the end record is, matching the signature and the length
 * of the comment after it, or -1. */
static int64_t find_eocd(Slice data) {
    const Byte *d = (const Byte *)data.p;
    if (data.len < ZIP_DIRECTORY_END_LEN)
        return -1;
    Int hi = data.len - ZIP_DIRECTORY_END_LEN;
    Int lo = hi > (Int)ZIP_UINT16_MAX ? hi - (Int)ZIP_UINT16_MAX : 0;
    for (Int i = hi; i >= lo; i--) {
        if (le32(d + i) != ZIP_DIRECTORY_END_SIGNATURE)
            continue;
        if (i + ZIP_DIRECTORY_END_LEN + le16(d + i + 20) == data.len)
            return i;
    }
    return -1;
}

/* parseCD on the tail of an archive that starts base bytes in. NULL is fine;
 * a description of what is wrong comes back otherwise. */
static const char *parse_cd(Slice data, uint64_t base, CdSnapshot *snap) {
    const Byte *d = (const Byte *)data.p;
    uint64_t dlen = (uint64_t)data.len;
    memset(snap, 0, sizeof *snap);
    int64_t sig_off = find_eocd(data);
    if (sig_off < 0)
        return "zip: EOCD not found";
    snap->eocd_records = le16(d + sig_off + 10);
    snap->eocd_size = le32(d + sig_off + 12);
    snap->eocd_offset = le32(d + sig_off + 16);
    uint64_t dir_offset = snap->eocd_offset;
    uint64_t nrecords = snap->eocd_records;

    if (sig_off >= ZIP_DIRECTORY64_LOC_LEN) {
        int64_t loc = sig_off - ZIP_DIRECTORY64_LOC_LEN;
        if (le32(d + loc) == ZIP_DIRECTORY64_LOC_SIGNATURE) {
            uint64_t eocd64 = le64(d + loc + 8);
            if (eocd64 < base)
                return "zip: EOCD64 before captured tail";
            uint64_t o = eocd64 - base;
            if (o + ZIP_DIRECTORY64_END_LEN > dlen)
                return "zip: EOCD64 offset out of range";
            if (le32(d + o) != ZIP_DIRECTORY64_END_SIGNATURE)
                return "zip: EOCD64 signature mismatch";
            snap->has_eocd64 = true;
            snap->eocd64_records = le64(d + o + 32);
            snap->eocd64_size = le64(d + o + 40);
            snap->eocd64_offset = le64(d + o + 48);
            dir_offset = snap->eocd64_offset;
            nrecords = snap->eocd64_records;
        }
    }
    if (dir_offset < base)
        return "zip: CD before captured tail";
    uint64_t off = dir_offset - base;
    for (uint64_t i = 0; i < nrecords; i++) {
        if (off + ZIP_DIRECTORY_HEADER_LEN > dlen)
            return "zip: CD entry out of range";
        const Byte *rec = d + off;
        if (le32(rec) != ZIP_DIRECTORY_HEADER_SIGNATURE)
            return "zip: bad CD signature";
        if (snap->n == 8)
            return "zip: too many entries for the test";
        CdEntry *e = &snap->entries[snap->n++];
        e->reader_version = le16(rec + 6);
        e->method = le16(rec + 10);
        e->raw_csize = le32(rec + 20);
        e->raw_usize = le32(rec + 24);
        uint64_t name_len = le16(rec + 28), extra_len = le16(rec + 30),
                 comm_len = le16(rec + 32);
        e->raw_offset = le32(rec + 42);
        uint64_t rec_len = ZIP_DIRECTORY_HEADER_LEN + name_len + extra_len + comm_len;
        if (off + rec_len > dlen)
            return "zip: CD entry truncated";
        e->name = str_from_bytes(rec + ZIP_DIRECTORY_HEADER_LEN, (Int)name_len);
        const Byte *extra = rec + ZIP_DIRECTORY_HEADER_LEN + name_len;
        uint64_t elen = extra_len;
        e->csize64 = e->raw_csize;
        e->usize64 = e->raw_usize;
        e->offset64 = e->raw_offset;
        while (elen >= 4) {
            uint16_t tag = le16(extra);
            uint64_t size = le16(extra + 2);
            if (4 + size > elen)
                break;
            const Byte *field = extra + 4;
            uint64_t flen = size;
            extra += 4 + size;
            elen -= 4 + size;
            if (tag != ZIP_EXTRA_ZIP64 ||
                e->nz64 + 3 > (int)(sizeof e->z64_fields / sizeof(int)))
                continue;
            if (e->raw_usize == ZIP_UINT32_MAX && flen >= 8) {
                e->usize64 = le64(field);
                e->z64_fields[e->nz64++] = Z64_USIZE;
                field += 8;
                flen -= 8;
            }
            if (e->raw_csize == ZIP_UINT32_MAX && flen >= 8) {
                e->csize64 = le64(field);
                e->z64_fields[e->nz64++] = Z64_CSIZE;
                field += 8;
                flen -= 8;
            }
            if (e->raw_offset == ZIP_UINT32_MAX && flen >= 8) {
                e->offset64 = le64(field);
                e->z64_fields[e->nz64++] = Z64_OFFSET;
            }
        }
        off += rec_len;
    }
    return NULL;
}

/* checkReaderMatchesSnapshot: NewReader sees the entries parse_cd did. */
static void check_reader_matches_snapshot(TestingT *t, const char *label, SparseFile *f,
                                          const CdSnapshot *snap) {
    Error err;
    ZipReader *zr =
        zip_new_reader(heap_allocator(), sparse_as_reader_at(f), f->size, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: NewReader: %v", label, err);
        return;
    }
    if (zr->file.len != snap->n) {
        testing_t_errorf_v(t, "%s: NewReader returned %d files, parseCD found %d",
                           label, zr->file.len, snap->n);
    } else {
        for (Int i = 0; i < zr->file.len; i++) {
            const ZipFileHeader *g = &zfile(zr, i)->file_header;
            const CdEntry *w = &snap->entries[i];
            if (!str_eq(g->name, w->name))
                testing_t_errorf_v(t, "%s entry %d: Name = %q, want %q", label, i,
                                   g->name, w->name);
            if (g->uncompressed_size64 != w->usize64)
                testing_t_errorf_v(
                    t, "%s entry %d %q: UncompressedSize64 = %d, want %d", label, i,
                    w->name, g->uncompressed_size64, w->usize64);
            if (g->compressed_size64 != w->csize64)
                testing_t_errorf_v(t, "%s entry %d %q: CompressedSize64 = %d, want %d",
                                   label, i, w->name, g->compressed_size64, w->csize64);
        }
    }
    zip_reader_free(zr);
}

/* reproduceCD: the golden's entries through a writer, sizes and all, into a
 * sparse buffer. Stored entries are written for real with the CRC-32 faked;
 * the deflated ones go in raw with the sizes given. */
static void reproduce_cd(TestingT *t, const CdSnapshot *golden, SparseFile *out) {
    Alloc *a = heap_allocator();
    SparseBuffer sb = {{0}, false};
    ZipWriter *w = zip_new_writer(a, (IoWriter){&sparse_buffer_vt, &sb});
    for (Int i = 0; i < golden->n; i++) {
        const CdEntry *e = &golden->entries[i];
        Error err;
        if (e->csize64 == e->usize64) {
            ZipFileHeader fh = {.name = e->name, .method = e->method};
            IoWriter fw = zip_writer_create_header(w, &fh, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "CreateHeader[%d %q]: %v", i, e->name, err);
            burrow__zip_file_writer_fake_crc32(fw);
            io_copy_n(a, fw, zeros, (int64_t)e->usize64, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "CopyN[%d %q]: %v", i, e->name, err);
            continue;
        }
        ZipFileHeader fh = {.name = e->name,
                            .method = e->method,
                            .compressed_size64 = e->csize64,
                            .uncompressed_size64 = e->usize64};
        IoWriter fw = zip_writer_create_raw(w, &fh, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "CreateRaw[%d %q]: %v", i, e->name, err);
        io_copy_n(a, fw, zeros, (int64_t)e->csize64, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "CopyN[%d %q]: %v", i, e->name, err);
    }
    Error err = zip_writer_close(w);
    zip_writer_free(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Close: %v", err);
    *out = sb.f;
}

/* compareCDSnapshots. Each difference goes to t as an error, or when t is
 * NULL is only counted. */
static int compare_cd_snapshots(TestingT *t, const CdSnapshot *want,
                                const CdSnapshot *got) {
    int diffs = 0;
#define CD_DIFF(...)                                                                   \
    do {                                                                               \
        diffs++;                                                                       \
        if (t != NULL)                                                                 \
            testing_t_errorf_v(t, __VA_ARGS__);                                        \
    } while (0)
    if (got->n != want->n) {
        CD_DIFF("entry count = %d, want %d", got->n, want->n);
        return diffs;
    }
    for (Int i = 0; i < want->n; i++) {
        const CdEntry *we = &want->entries[i], *ge = &got->entries[i];
        if (we->raw_csize != ge->raw_csize)
            CD_DIFF("entry %d %q: RawCSize = %#08x, want %#08x", i, we->name,
                    ge->raw_csize, we->raw_csize);
        if (we->raw_usize != ge->raw_usize)
            CD_DIFF("entry %d %q: RawUSize = %#08x, want %#08x", i, we->name,
                    ge->raw_usize, we->raw_usize);
        if (we->csize64 != ge->csize64)
            CD_DIFF("entry %d %q: CSize64 = %d, want %d", i, we->name, ge->csize64,
                    we->csize64);
        if (we->usize64 != ge->usize64)
            CD_DIFF("entry %d %q: USize64 = %d, want %d", i, we->name, ge->usize64,
                    we->usize64);
        if ((we->raw_offset == ZIP_UINT32_MAX) != (ge->raw_offset == ZIP_UINT32_MAX))
            CD_DIFF("entry %d %q: RawOffset placeholder = %#08x, want %#08x", i,
                    we->name, ge->raw_offset, we->raw_offset);
        bool same = we->nz64 == ge->nz64;
        for (int k = 0; same && k < we->nz64; k++)
            same = we->z64_fields[k] == ge->z64_fields[k];
        if (!same)
            CD_DIFF("entry %d %q: Zip64 sub-field order differs (%d fields, want %d)",
                    i, we->name, ge->nz64, we->nz64);
        if (we->nz64 > 0 && ge->reader_version < ZIP_VERSION45)
            CD_DIFF("entry %d %q: ReaderVersion = %d, want >= %d (Zip64 extra present)",
                    i, we->name, ge->reader_version, ZIP_VERSION45);
    }
    if ((want->eocd_records == ZIP_UINT16_MAX) != (got->eocd_records == ZIP_UINT16_MAX))
        CD_DIFF("EOCD records placeholder = %#x, want %#x", got->eocd_records,
                want->eocd_records);
    if ((want->eocd_size == ZIP_UINT32_MAX) != (got->eocd_size == ZIP_UINT32_MAX))
        CD_DIFF("EOCD size placeholder = %#x, want %#x", got->eocd_size,
                want->eocd_size);
    if ((want->eocd_offset == ZIP_UINT32_MAX) != (got->eocd_offset == ZIP_UINT32_MAX))
        CD_DIFF("EOCD offset placeholder = %#x, want %#x", got->eocd_offset,
                want->eocd_offset);
    if (got->has_eocd64 != want->has_eocd64)
        CD_DIFF("EOCD64 present = %v, want %v", got->has_eocd64, want->has_eocd64);
    if (want->has_eocd64 && got->has_eocd64 &&
        got->eocd64_records != want->eocd64_records)
        CD_DIFF("EOCD64 records = %d, want %d", got->eocd64_records,
                want->eocd64_records);
#undef CD_DIFF
    return diffs;
}

/* Where the writer's central directory is meant to differ from the golden's.
 * See Go's test for why each one does. */
static bool expected_diff(const char *name) {
    static const char *const names[] = {
        "infozip-store-4g-minus-1",
        "infozip-offset-eq-4g",
        "libarchive-deflate-zeros-5g",
        "libarchive-store-just-under-4g",
        "go126-store-5g",
        "go126-deflate-zeros-5g",
        "go126-store-4g-minus-1",
        "go126-store-4g-minus-2",
        "go126-store-exact-4g",
        "go126-offset-past-4g",
        "go126-offset-eq-4g",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i], name) == 0)
            return true;
    return false;
}

static void zip64_golden(TestingT *t, const GFile *g, const char *name) {
    enum { tail_keep = 1 << 20 };
    Alloc *a = heap_allocator();
    Slice raw = gunzip(a, g->gz, g->gz_len, g->len);
    SparseFile golden_sf;
    if (!read_sparse(raw, &golden_sf)) {
        testing_t_errorf_v(t, "%s: read golden: bad sparse file", name);
        sparse_free(&golden_sf);
        free_bytes(a, &raw);
        return;
    }
    free_bytes(a, &raw);
    uint64_t golden_base;
    Slice golden_data = sparse_materialize_tail(&golden_sf, tail_keep, &golden_base);
    CdSnapshot golden, got;
    const char *perr = parse_cd(golden_data, golden_base, &golden);
    if (perr != NULL) {
        testing_t_errorf_v(t, "%s: parse golden CD: %s", name, perr);
        goto out_golden;
    }
    check_reader_matches_snapshot(t, "golden", &golden_sf, &golden);

    SparseFile ours_sf;
    reproduce_cd(t, &golden, &ours_sf);
    uint64_t ours_base;
    Slice ours_data = sparse_materialize_tail(&ours_sf, tail_keep, &ours_base);
    perr = parse_cd(ours_data, ours_base, &got);
    if (perr != NULL) {
        testing_t_errorf_v(t, "%s: parse reproduced CD: %s", name, perr);
    } else {
        check_reader_matches_snapshot(t, "reproduced", &ours_sf, &got);
        if (expected_diff(name)) {
            if (compare_cd_snapshots(NULL, &golden, &got) == 0)
                testing_t_errorf_v(
                    t,
                    "%s: expected this golden to fail equivalence, but it "
                    "passed",
                    name);
        } else if (compare_cd_snapshots(t, &golden, &got) > 0) {
            testing_t_logf_v(t, "in %s", name);
        }
    }
    mem_free(a, (void *)(uintptr_t)ours_data.p, (size_t)ours_data.len + 1, 1);
    sparse_free(&ours_sf);
out_golden:
    mem_free(a, (void *)(uintptr_t)golden_data.p, (size_t)golden_data.len + 1, 1);
    sparse_free(&golden_sf);
}

/* The writer's central directory for archives of 4 GiB and more follows the
 * ZIP64 conventions of Info-ZIP, libarchive and Go's own earlier writer,
 * except where it means not to. */
static void TestZip64WriterCDGoldens(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(
            t, "skipping in short mode; each golden replays a multi-GiB write");
    static const char prefix[] = "testdata/zip64/";
    int found = 0;
    for (size_t i = 0; i < sizeof gen_testdata / sizeof gen_testdata[0]; i++) {
        const GFile *g = &gen_testdata[i];
        if (strncmp(g->name, prefix, sizeof prefix - 1) != 0)
            continue;
        char name[128];
        snprintf(name, sizeof name, "%s", g->name + sizeof prefix - 1);
        char *dot = strstr(name, ".zsparse");
        if (dot != NULL)
            *dot = 0;
        zip64_golden(t, g, name);
        found++;
    }
    if (found == 0)
        testing_t_fatalf_v(t, "missing Zip64 goldens in testdata/zip64");
}

/* CreateRaw with no data descriptor and an uncompressed size over 4 GiB: the
 * local header has both 32-bit sizes as placeholders and both 64-bit sizes in
 * its ZIP64 extra, usize first. */
static void TestZip64LFHBothPlaceholders(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *w = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    ZipFileHeader fh = {.name = BURROW_S("x"),
                        .method = ZIP_DEFLATE,
                        .compressed_size64 = 1024,
                        .uncompressed_size64 = 5ULL << 30};
    Error err;
    IoWriter fw = zip_writer_create_raw(w, &fh, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    io_copy_n(a, fw, zeros, (int64_t)fh.compressed_size64, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    err = zip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    const Byte *b = (const Byte *)bytes_buffer_bytes(&buf).p;
    if (le32(b + 14) != fh.crc32)
        testing_t_errorf_v(t, "LFH CRC32 = %#x, want %#x", le32(b + 14), fh.crc32);
    if (le32(b + 18) != ZIP_UINT32_MAX)
        testing_t_errorf_v(t, "LFH CompressedSize = %#x, want %#x (placeholder)",
                           le32(b + 18), ZIP_UINT32_MAX);
    if (le32(b + 22) != ZIP_UINT32_MAX)
        testing_t_errorf_v(t, "LFH UncompressedSize = %#x, want %#x (placeholder)",
                           le32(b + 22), ZIP_UINT32_MAX);
    uint16_t name_len = le16(b + 26), extra_len = le16(b + 28);
    if (extra_len != 20)
        testing_t_fatalf_v(t, "LFH extra length = %d, want %d", extra_len, 20);
    const Byte *extra = b + 30 + name_len;
    if (le16(extra) != ZIP_EXTRA_ZIP64)
        testing_t_errorf_v(t, "Zip64 extra tag = %#x, want %#x", le16(extra),
                           ZIP_EXTRA_ZIP64);
    if (le16(extra + 2) != 16)
        testing_t_errorf_v(t, "Zip64 extra data length = %d, want 16", le16(extra + 2));
    if (le64(extra + 4) != fh.uncompressed_size64)
        testing_t_errorf_v(t, "Zip64 USize64 = %d, want %d", le64(extra + 4),
                           fh.uncompressed_size64);
    if (le64(extra + 12) != fh.compressed_size64)
        testing_t_errorf_v(t, "Zip64 CSize64 = %d, want %d", le64(extra + 12),
                           fh.compressed_size64);
    arena_free(&ar);
}

/* ------------------------------------------------------------ allocation */

/* Reading test.zip and writing a small archive, with each allocation in turn
 * failing, gives an error or works, and leaks nothing either way. */
static void TestNoMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    Slice data = testdata(scratch, "testdata/test.zip");
    for (long long budget = 0;; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesReader br;
        Error err;
        ZipReader *r = new_reader(&al, &br, data, &err);
        bool done = r != NULL;
        if (r != NULL) {
            for (Int i = 0; i < r->file.len; i++) {
                IoReadCloser rc = zip_file_open(zfile(r, i), &al, &err);
                if (BURROW_FAILED(err)) {
                    done = false;
                    continue;
                }
                io_copy(&al, io_discard, rc_reader(rc), &err);
                if (BURROW_FAILED(err))
                    done = false;
                (void)rc_close(rc);
            }
            FsFile f = zip_reader_open(r, &al, BURROW_S("test.txt"), &err);
            if (BURROW_OK(err))
                (void)fs_close(f);
            else
                done = false;
            zip_reader_free(r);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked reading", (int64_t)budget,
                               (int64_t)b.live);
        if (done)
            break;
        if (budget > 100000)
            testing_t_fatalf_v(t, "reading never finished");
    }
    for (long long budget = 0;; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesBuffer buf = BYTES_BUFFER(&al);
        ZipWriter *w = zip_new_writer(&al, bytes_buffer_as_io_writer(&buf));
        bool done = false;
        if (w != NULL) {
            Error err;
            IoWriter fw = zip_writer_create(w, BURROW_S("a.txt"), &err);
            if (BURROW_OK(err))
                fw.vt->write(fw.data, const_bytes("hello, world", 12), &err);
            if (BURROW_OK(err)) {
                ZipFileHeader h = {.name = BURROW_S("b/"), .comment = BURROW_S("dir")};
                zip_writer_create_header(w, &h, &err);
            }
            if (BURROW_OK(err))
                err = zip_writer_set_comment(w, BURROW_S("comment"));
            if (BURROW_OK(err))
                err = zip_writer_close(w);
            done = BURROW_OK(err);
            zip_writer_free(w);
        }
        bytes_buffer_free(&buf);
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked writing", (int64_t)budget,
                               (int64_t)b.live);
        if (done)
            break;
        if (budget > 100000)
            testing_t_fatalf_v(t, "writing never finished");
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestInvalidFiles)                                                                \
    X(TestIssue8186)                                                                   \
    X(TestIssue10957)                                                                  \
    X(TestIssue10956)                                                                  \
    X(TestIssue11146)                                                                  \
    X(TestIssue12449)                                                                  \
    X(TestFS)                                                                          \
    X(TestFSWalk)                                                                      \
    X(TestFSWalkBadFile)                                                               \
    X(TestFSModTime)                                                                   \
    X(TestCVE202127919)                                                                \
    X(TestOpenReaderInsecurePath)                                                      \
    X(TestCVE202133196)                                                                \
    X(TestCVE202139293)                                                                \
    X(TestCVE202141772)                                                                \
    X(TestUnderSize)                                                                   \
    X(TestIssue54801)                                                                  \
    X(TestInsecurePaths)                                                               \
    X(TestDisableInsecurePathCheck)                                                    \
    X(TestCompressedDirectory)                                                         \
    X(TestBaseOffsetPlusOverflow)                                                      \
    X(TestWriter)                                                                      \
    X(TestWriterComment)                                                               \
    X(TestWriterUTF8)                                                                  \
    X(TestWriterTime)                                                                  \
    X(TestWriterOffset)                                                                \
    X(TestWriterFlush)                                                                 \
    X(TestWriterDir)                                                                   \
    X(TestWriterDirAttributes)                                                         \
    X(TestWriterCopy)                                                                  \
    X(TestWriterCreateRaw)                                                             \
    X(TestWriterAddFS)                                                                 \
    X(TestIssue61875)                                                                  \
    X(TestOver65kFiles)                                                                \
    X(TestModTime)                                                                     \
    X(TestFileHeaderRoundTrip)                                                         \
    X(TestFileHeaderRoundTrip64)                                                       \
    X(TestFileHeaderRoundTripModified)                                                 \
    X(TestFileHeaderRoundTripWithoutModified)                                          \
    X(TestRLEBuffer)                                                                   \
    X(TestZip64)                                                                       \
    X(TestZip64EdgeCase)                                                               \
    X(TestZip64DirectoryOffset)                                                        \
    X(TestZip64ManyRecords)                                                            \
    X(TestSuffixSaver)                                                                 \
    X(TestZip64LargeDirectory)                                                         \
    X(TestHeaderInvalidTagAndSize)                                                     \
    X(TestHeaderTooShort)                                                              \
    X(TestHeaderTooLongErr)                                                            \
    X(TestHeaderIgnoredSize)                                                           \
    X(TestZeroLengthHeader)                                                            \
    X(TestZip64WriterCDGoldens)                                                        \
    X(TestZip64LFHBothPlaceholders)                                                    \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
