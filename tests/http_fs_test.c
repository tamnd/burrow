/* Derived from Go's src/net/http/fs_test.go and range_test.go.
 * Go source: go1.27.1.
 *
 * Go runs most of these against a test server and a client, and the client
 * follows redirects. There is no Server here yet, so they run the handler
 * against a ResponseRecorder, ask for where a redirect would have gone, and
 * check the redirect itself on the way. What only a server adds, such as the
 * Content-Length of an error page, is not checked. The tests that need a
 * server for what they test (sendfile, the gzip writer, the relative paths
 * that need a working directory) wait for it.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mime.h"
#include "burrow/mime/multipart.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/testing/fstest.h"
#include "burrow/time.h"

#include "../src/net/http_internal.h"

#include <string.h>

#define S BURROW_S

static Arena ar;
static Alloc *a;

/* t.TempDir for the whole run, with Go's testdata in it: root/testdata. */
static Str root;
static Str testdata;

static const Str file_contents = BURROW_S_INIT("0123456789\n");
static const Str index_contents = BURROW_S_INIT("index.html says hello\n");
static const Str style_contents = BURROW_S_INIT("body {}\n");

#define TEST_FILE_LEN 11

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static Str td(const char *name) {
    return filepath_join_v(a, 2, testdata, cs(name));
}

static HttpRequest *new_req_s(const char *method, Str target) {
    return httptest_new_request(a, cs(method), target, (IoReader){0});
}

static HttpRequest *new_req(const char *method, const char *target) {
    return new_req_s(method, cs(target));
}

static void set_req(HttpRequest *r, const char *key, const char *value) {
    (void)http_header_set(r->header, cs(key), cs(value));
}

static HttptestResponseRecorder *new_rec(void) {
    return httptest_new_recorder(a);
}

static HttpResponseWriter rw(HttptestResponseRecorder *rec) {
    return httptest_response_recorder_as_response_writer(rec);
}

static void set_rec(HttptestResponseRecorder *rec, const char *key, const char *value) {
    (void)http_header_set(rec->header_map, cs(key), cs(value));
}

static HttptestResponseRecorder *serve(HttpHandler h, HttpRequest *r) {
    HttptestResponseRecorder *rec = new_rec();
    http_handler_serve_http(h, rw(rec), r);
    return rec;
}

static Str body_of(HttptestResponseRecorder *rec) {
    return str_of(bytes_buffer_bytes(rec->body));
}

static HttpHeader res_header(HttptestResponseRecorder *rec) {
    return httptest_response_recorder_result(rec)->header;
}

static Str res_get(HttptestResponseRecorder *rec, const char *key) {
    return http_header_get(res_header(rec), cs(key));
}

static void done(HttptestResponseRecorder *rec, HttpRequest *r) {
    httptest_response_recorder_free(rec);
    http_request_free(r);
}

/* What a client that follows redirects would have been sent to: a 301 to
 * want, checked here. */
static void check_redirect(TestingT *t, HttptestResponseRecorder *rec, const char *what,
                           Str want) {
    if (rec->code != HTTP_STATUS_MOVED_PERMANENTLY)
        testing_t_errorf_v(t, "%s: code = %d; want 301", cs(what), rec->code);
    Str loc = http_header_get(rec->header_map, S("Location"));
    if (!str_eq(loc, want))
        testing_t_errorf_v(t, "%s: Location = %q; want %q", cs(what), loc, want);
}

/* Read seekers over a StringsReader and an OsFile. */
static IoReadSeekerVT strings_rs_vt;
static IoReadSeekerVT os_rs_vt;

static IoReadSeeker strings_rs(Str s) {
    StringsReader *sr = strings_new_reader(a, s);
    strings_rs_vt.reader = *strings_reader_as_io_reader(sr).vt;
    strings_rs_vt.seeker = *strings_reader_as_io_seeker(sr).vt;
    return (IoReadSeeker){&strings_rs_vt, sr};
}

static IoReadSeeker os_rs(OsFile *f) {
    os_rs_vt.reader = *os_file_as_io_reader(f).vt;
    os_rs_vt.seeker = *os_file_as_io_seeker(f).vt;
    return (IoReadSeeker){&os_rs_vt, f};
}

/* panicOnSeek{nil}: anything but holding it panics. */
static Int panic_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    panic_str(S("read of panicOnSeek"));
}

static int64_t panic_seek(void *self, int64_t offset, int whence, Error *err) {
    (void)self;
    (void)offset;
    (void)whence;
    (void)err;
    panic_str(S("seek of panicOnSeek"));
}

static const IoReadSeekerVT panic_rs_vt = {{NULL, panic_read}, {NULL, panic_seek}};

/* --------------------------------------------------------------- FakeFS */

typedef struct FakeInfo FakeInfo;
struct FakeInfo {
    const char *basename;
    const char *contents;
    FakeInfo *const *ents;
    Int nents;
    Time modtime;
    Error err;
    bool dir;
};

static Str fi_name(void *self) {
    return cs(((const FakeInfo *)self)->basename);
}

static int64_t fi_size(void *self) {
    const FakeInfo *f = (const FakeInfo *)self;
    return f->contents != NULL ? (int64_t)strlen(f->contents) : 0;
}

static FsFileMode fi_mode(void *self) {
    return ((const FakeInfo *)self)->dir ? (FsFileMode)0755 | FS_MODE_DIR : 0644;
}

static Time fi_mod_time(void *self) {
    return ((const FakeInfo *)self)->modtime;
}

static bool fi_is_dir(void *self) {
    return ((const FakeInfo *)self)->dir;
}

static Any fi_sys(void *self) {
    (void)self;
    return (Any){NULL, NULL};
}

static const FsFileInfoVT fake_info_vt = {NULL,        fi_name,   fi_size, fi_mode,
                                          fi_mod_time, fi_is_dir, fi_sys};

typedef struct FakeFile {
    StringsReader *r;
    FakeInfo *fi;
    Int entpos;
} FakeFile;

static Int ff_read(void *self, Slice p, Error *err) {
    return strings_reader_read(((FakeFile *)self)->r, p, err);
}

static Error ff_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static int64_t ff_seek(void *self, int64_t offset, int whence, Error *err) {
    return strings_reader_seek(((FakeFile *)self)->r, offset, whence, err);
}

static Slice ff_readdir(void *self, Alloc *al, Int count, Error *err) {
    FakeFile *f = (FakeFile *)self;
    Slice fis = slice_nil(TYPE_FS_FILE_INFO);
    *err = BURROW_NO_ERROR;
    if (!f->fi->dir) {
        *err = fs_err_invalid;
        return fis;
    }
    Int limit = f->entpos + count;
    if (count <= 0 || limit > f->fi->nents)
        limit = f->fi->nents;
    for (; f->entpos < limit; f->entpos++) {
        FsFileInfo fi = {&fake_info_vt, f->fi->ents[f->entpos]};
        fis = slice_append(al, fis, &fi, 1);
    }
    if (fis.len == 0 && count > 0)
        *err = io_eof;
    return fis;
}

static FsFileInfo ff_stat(void *self, Alloc *al, Error *err) {
    (void)al;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&fake_info_vt, ((FakeFile *)self)->fi};
}

/* No ReadDir, as Go's fakeFile has none, so the listing goes through
 * Readdir. */
static const HttpFileVT fake_file_vt = {
    {{NULL, ff_read}, {NULL, ff_close}}, ff_seek, ff_readdir, ff_stat, NULL,
};

typedef struct FakeEntry {
    Str path;
    FakeInfo *fi;
} FakeEntry;

typedef struct FakeFS {
    const FakeEntry *ents;
    Int n;
} FakeFS;

static HttpFile fake_open(void *self, Alloc *al, Str name, Error *err) {
    const FakeFS *fsys = (const FakeFS *)self;
    HttpFile none = {NULL, NULL};
    name = path_clean(al, name);
    for (Int i = 0; i < fsys->n; i++) {
        if (!str_eq(fsys->ents[i].path, name))
            continue;
        FakeInfo *fi = fsys->ents[i].fi;
        if (BURROW_FAILED(fi->err)) {
            *err = fi->err;
            return none;
        }
        FakeFile *f = BURROW_NEW(al, FakeFile);
        f->r = strings_new_reader(al, cs(fi->contents));
        f->fi = fi;
        f->entpos = 0;
        *err = BURROW_NO_ERROR;
        return (HttpFile){&fake_file_vt, f};
    }
    *err = fs_err_not_exist;
    return none;
}

static const HttpFileSystemVT fake_fs_vt = {NULL, fake_open};

static HttpFileSystem fake_fs(FakeFS *fsys) {
    return (HttpFileSystem){&fake_fs_vt, fsys};
}

/* ------------------------------------------------------------ TestParseRange */

typedef struct WantRange {
    int64_t start;
    int64_t length;
} WantRange;

static const struct {
    const char *s;
    int64_t length;
    Int n; /* -1 for nil */
    WantRange r[3];
} parse_range_tests[] = {
    {"", 0, -1, {{0, 0}}},
    {"", 1000, -1, {{0, 0}}},
    {"foo", 0, -1, {{0, 0}}},
    {"bytes=", 0, -1, {{0, 0}}},
    {"bytes=7", 10, -1, {{0, 0}}},
    {"bytes= 7 ", 10, -1, {{0, 0}}},
    {"bytes=1-", 0, -1, {{0, 0}}},
    {"bytes=5-4", 10, -1, {{0, 0}}},
    {"bytes=0-2,5-4", 10, -1, {{0, 0}}},
    {"bytes=2-5,4-3", 10, -1, {{0, 0}}},
    {"bytes=--5,4--3", 10, -1, {{0, 0}}},
    {"bytes=A-", 10, -1, {{0, 0}}},
    {"bytes=A- ", 10, -1, {{0, 0}}},
    {"bytes=A-Z", 10, -1, {{0, 0}}},
    {"bytes= -Z", 10, -1, {{0, 0}}},
    {"bytes=5-Z", 10, -1, {{0, 0}}},
    {"bytes=Ran-dom, garbage", 10, -1, {{0, 0}}},
    {"bytes=0x01-0x02", 10, -1, {{0, 0}}},
    {"bytes=         ", 10, -1, {{0, 0}}},
    {"bytes= , , ,   ", 10, -1, {{0, 0}}},

    {"bytes=0-9", 10, 1, {{0, 10}}},
    {"bytes=0-", 10, 1, {{0, 10}}},
    {"bytes=5-", 10, 1, {{5, 5}}},
    {"bytes=0-20", 10, 1, {{0, 10}}},
    {"bytes=15-,0-5", 10, 1, {{0, 6}}},
    {"bytes=1-2,5-", 10, 2, {{1, 2}, {5, 5}}},
    {"bytes=-2 , 7-", 11, 2, {{9, 2}, {7, 4}}},
    {"bytes=0-0 ,2-2, 7-", 11, 3, {{0, 1}, {2, 1}, {7, 4}}},
    {"bytes=-5", 10, 1, {{5, 5}}},
    {"bytes=-15", 10, 1, {{0, 10}}},
    {"bytes=0-499", 10000, 1, {{0, 500}}},
    {"bytes=500-999", 10000, 1, {{500, 500}}},
    {"bytes=-500", 10000, 1, {{9500, 500}}},
    {"bytes=9500-", 10000, 1, {{9500, 500}}},
    {"bytes=0-0,-1", 10000, 2, {{0, 1}, {9999, 1}}},
    {"bytes=500-600,601-999", 10000, 2, {{500, 101}, {601, 399}}},
    {"bytes=500-700,601-999", 10000, 2, {{500, 201}, {601, 399}}},

    /* Match Apache laxity: */
    {"bytes=   1 -2   ,  4- 5, 7 - 8 , ,,", 11, 3, {{1, 2}, {4, 2}, {7, 2}}},
};

static void TestParseRange(TestingT *t) {
    for (size_t i = 0; i < sizeof parse_range_tests / sizeof parse_range_tests[0];
         i++) {
        Str s = cs(parse_range_tests[i].s);
        Int want = parse_range_tests[i].n < 0 ? 0 : parse_range_tests[i].n;
        burrow__HttpRange *ranges;
        Error e = BURROW_NO_ERROR;
        Int n =
            burrow__http_parse_range(a, s, parse_range_tests[i].length, &ranges, &e);
        if (BURROW_FAILED(e) && parse_range_tests[i].n >= 0)
            testing_t_errorf_v(t, "parseRange(%q) returned error %q", s, error_text(e));
        if (n != want) {
            testing_t_errorf_v(t, "len(parseRange(%q)) = %d, want %d", s, n, want);
            continue;
        }
        for (Int j = 0; j < want; j++) {
            const WantRange *r = &parse_range_tests[i].r[j];
            if (ranges[j].start != r->start)
                testing_t_errorf_v(t, "parseRange(%q)[%d].start = %d, want %d", s, j,
                                   ranges[j].start, r->start);
            if (ranges[j].length != r->length)
                testing_t_errorf_v(t, "parseRange(%q)[%d].length = %d, want %d", s, j,
                                   ranges[j].length, r->length);
        }
    }
}

/* ------------------------------------------------------------- TestServeFile */

static const struct {
    const char *r;
    Int code;
    Int n;
    struct {
        int64_t start;
        int64_t end; /* range [start,end) */
    } ranges[2];
} serve_file_range_tests[] = {
    {"", 200, 0, {{0, 0}}},
    {"bytes=0-4", 206, 1, {{0, 5}}},
    {"bytes=2-", 206, 1, {{2, TEST_FILE_LEN}}},
    {"bytes=-5", 206, 1, {{TEST_FILE_LEN - 5, TEST_FILE_LEN}}},
    {"bytes=3-7", 206, 1, {{3, 8}}},
    {"bytes=0-0,-2", 206, 2, {{0, 1}, {TEST_FILE_LEN - 2, TEST_FILE_LEN}}},
    {"bytes=0-1,5-8", 206, 2, {{0, 2}, {5, 9}}},
    {"bytes=0-1,5-", 206, 2, {{0, 2}, {5, TEST_FILE_LEN}}},
    {"bytes=5-1000", 206, 1, {{5, TEST_FILE_LEN}}},
    {"bytes=0-,1-,2-,3-,4-", 200, 0, {{0, 0}}}, /* ignore wasteful range request */
    {"bytes=0-9", 206, 1, {{0, TEST_FILE_LEN - 1}}},
    {"bytes=0-10", 206, 1, {{0, TEST_FILE_LEN}}},
    {"bytes=0-11", 206, 1, {{0, TEST_FILE_LEN}}},
    {"bytes=10-11", 206, 1, {{TEST_FILE_LEN - 1, TEST_FILE_LEN}}},
    {"bytes=10-", 206, 1, {{TEST_FILE_LEN - 1, TEST_FILE_LEN}}},
    {"bytes=11-", 416, 0, {{0, 0}}},
    {"bytes=11-12", 416, 0, {{0, 0}}},
    {"bytes=12-12", 416, 0, {{0, 0}}},
    {"bytes=11-100", 416, 0, {{0, 0}}},
    {"bytes=12-100", 416, 0, {{0, 0}}},
    {"bytes=100-", 416, 0, {{0, 0}}},
    {"bytes=100-1000", 416, 0, {{0, 0}}},
};

static Str sub(Str s, int64_t start, int64_t end) {
    return str_from_bytes(s.p + start, (Int)(end - start));
}

/* The parts of a multipart/byteranges body against the ranges of test i. */
static void check_parts(TestingT *t, size_t i, HttptestResponseRecorder *rec) {
    Str r = cs(serve_file_range_tests[i].r);
    Str ct = res_get(rec, "Content-Type");
    Map *params = NULL;
    Error e = BURROW_NO_ERROR;
    Str typ = mime_parse_media_type(a, ct, &params, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "range=%q content-type = %q; %v", r, ct, e);
        return;
    }
    if (!str_eq(typ, S("multipart/byteranges"))) {
        testing_t_errorf_v(t, "range=%q content-type = %q; want multipart/byteranges",
                           r, typ);
        return;
    }
    Str bkey = S("boundary");
    const Str *boundary = params != NULL ? (const Str *)map_get(params, &bkey) : NULL;
    if (boundary == NULL || boundary->len == 0) {
        testing_t_errorf_v(t, "range=%q content-type = %q; lacks boundary", r, ct);
        return;
    }
    Str body = body_of(rec);
    int64_t cl = httptest_response_recorder_result(rec)->content_length;
    if (cl != body.len) {
        testing_t_errorf_v(t, "range=%q Content-Length = %d; want %d", r, cl, body.len);
        return;
    }
    MultipartReader *mr = multipart_new_reader(
        a, strings_reader_as_io_reader(strings_new_reader(a, body)), *boundary);
    for (Int ri = 0; ri < serve_file_range_tests[i].n; ri++) {
        int64_t start = serve_file_range_tests[i].ranges[ri].start;
        int64_t end = serve_file_range_tests[i].ranges[ri].end;
        MultipartPart *part = multipart_reader_next_part(mr, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "range=%q, reading part index %d: %v", r, ri, e);
            multipart_reader_free(mr);
            return;
        }
        Str want_cr = fmt_sprintf_v(a, "bytes %d-%d/%d", start, end - 1, TEST_FILE_LEN);
        Str got_cr =
            textproto_mime_header_get(multipart_part_header(part), S("Content-Range"));
        if (!str_eq(got_cr, want_cr))
            testing_t_errorf_v(t, "range=%q: part Content-Range = %q; want %q", r,
                               got_cr, want_cr);
        Slice got = io_read_all(a, multipart_part_as_io_reader(part), &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "range=%q, reading part index %d body: %v", r, ri, e);
            multipart_reader_free(mr);
            return;
        }
        Str want = sub(file_contents, start, end);
        if (!str_eq(str_of(got), want))
            testing_t_errorf_v(t, "range=%q: body = %q, want %q", r, str_of(got), want);
    }
    (void)multipart_reader_next_part(mr, &e);
    if (!errors_is(e, io_eof))
        testing_t_errorf_v(t, "range=%q; expected final error io.EOF; got %v", r, e);
    multipart_reader_free(mr);
}

static void TestServeFile(TestingT *t) {
    static const char *const methods[] = {"GET",    "POST",    "PUT",  "PATCH",
                                          "DELETE", "OPTIONS", "TRACE"};
    Str file = td("file");

    /* Get contents via various methods. */
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        HttpRequest *r = new_req(methods[i], "/");
        HttptestResponseRecorder *rec = new_rec();
        http_serve_file(rw(rec), r, file);
        if (!str_eq(body_of(rec), file_contents))
            testing_t_errorf_v(t, "body mismatch for %v request: got %q, want %q",
                               cs(methods[i]), body_of(rec), file_contents);
        done(rec, r);
    }

    /* HEAD request. */
    HttpRequest *r = new_req("HEAD", "/");
    HttptestResponseRecorder *rec = new_rec();
    http_serve_file(rw(rec), r, file);
    if (body_of(rec).len != 0)
        testing_t_errorf_v(t, "body mismatch for HEAD request: got %q, want empty",
                           body_of(rec));
    if (!str_eq(res_get(rec, "Content-Length"), S("11")))
        testing_t_errorf_v(t,
                           "Content-Length mismatch for HEAD request: got %v, want 11",
                           res_get(rec, "Content-Length"));
    done(rec, r);

    /* Range tests. */
    for (size_t i = 0;
         i < sizeof serve_file_range_tests / sizeof serve_file_range_tests[0]; i++) {
        Str rt = cs(serve_file_range_tests[i].r);
        Int n = serve_file_range_tests[i].n;
        r = new_req("GET", "/");
        if (rt.len > 0)
            set_req(r, "Range", serve_file_range_tests[i].r);
        rec = new_rec();
        http_serve_file(rw(rec), r, file);
        if (rec->code != serve_file_range_tests[i].code)
            testing_t_errorf_v(t, "range=%q: StatusCode=%d, want %d", rt, rec->code,
                               serve_file_range_tests[i].code);
        if (serve_file_range_tests[i].code != 416) {
            Str want_cr = BURROW_STR_EMPTY;
            if (n == 1)
                want_cr = fmt_sprintf_v(
                    a, "bytes %d-%d/%d", serve_file_range_tests[i].ranges[0].start,
                    serve_file_range_tests[i].ranges[0].end - 1, TEST_FILE_LEN);
            Str cr = res_get(rec, "Content-Range");
            if (!str_eq(cr, want_cr))
                testing_t_errorf_v(t, "range=%q: Content-Range = %q, want %q", rt, cr,
                                   want_cr);
            Str ct = res_get(rec, "Content-Type");
            if (n == 1) {
                Str want = sub(file_contents, serve_file_range_tests[i].ranges[0].start,
                               serve_file_range_tests[i].ranges[0].end);
                if (!str_eq(body_of(rec), want))
                    testing_t_errorf_v(t, "range=%q: body = %q, want %q", rt,
                                       body_of(rec), want);
                if (strings_has_prefix(ct, S("multipart/byteranges")))
                    testing_t_errorf_v(
                        t,
                        "range=%q content-type = %q; unexpected multipart/byteranges",
                        rt, ct);
            }
            if (n > 1)
                check_parts(t, i, rec);
        }
        done(rec, r);
    }
}

static void TestServeFile_DotDot(TestingT *t) {
    static const struct {
        const char *req;
        Int want_status;
    } tests[] = {
        {"/testdata/file", 200}, {"/../file", 400},  {"/..", 400},
        {"/../", 400},           {"/../foo", 400},   {"/..\\foo", 400},
        {"/file/a", 200},        {"/file/a..", 200}, {"/file/a/..", 400},
        {"/file/a\\..", 400},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str text =
            fmt_sprintf_v(a, "GET %s HTTP/1.1\r\nHost: foo\r\n\r\n", cs(tests[i].req));
        BufioReader *br = bufio_new_reader(
            a, strings_reader_as_io_reader(strings_new_reader(a, text)));
        Error e = BURROW_NO_ERROR;
        HttpRequest *req = http_read_request(a, br, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "bad request %q: %v", cs(tests[i].req), e);
            bufio_reader_free(br);
            continue;
        }
        HttptestResponseRecorder *rec = new_rec();
        http_serve_file(rw(rec), req, td("file"));
        if (rec->code != tests[i].want_status)
            testing_t_errorf_v(t, "for request %q, status = %d; want %d",
                               cs(tests[i].req), rec->code, tests[i].want_status);
        done(rec, req);
        bufio_reader_free(br);
    }
}

/* Tests that this doesn't panic. (Issue 30165) */
static void TestServeFileDirPanicEmptyPath(TestingT *t) {
    HttptestResponseRecorder *rec = new_rec();
    HttpRequest *req = new_req("GET", "/");
    req->url->path = BURROW_STR_EMPTY;
    http_serve_file(rw(rec), req, testdata);
    HttpResponse *res = httptest_response_recorder_result(rec);
    if (res->status_code != 301)
        testing_t_errorf_v(t, "code = %v; want 301", res->status);
    done(rec, req);
}

/* Tests that ranges are ignored with serving empty content. (Issue 54794) */
static void TestServeContentWithEmptyContentIgnoreRanges(TestingT *t) {
    static const char *const ranges[] = {"bytes=0-128", "bytes=1-"};
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        HttptestResponseRecorder *rec = new_rec();
        HttpRequest *req = new_req("GET", "/");
        set_req(req, "Range", ranges[i]);
        http_serve_content(rw(rec), req, S("nothing"), time_now(),
                           strings_rs(BURROW_STR_EMPTY));
        HttpResponse *res = httptest_response_recorder_result(rec);
        if (res->status_code != 200)
            testing_t_errorf_v(t, "code = %v; want 200", res->status);
        if (bytes_buffer_len(rec->body) != 0)
            testing_t_errorf_v(t, "body.Len() = %v; want 0", res->status);
        done(rec, req);
    }
}

/* ---------------------------------------------------------- FileServer */

static Str cleans_opened;

static HttpFile cleans_open(void *self, Alloc *al, Str name, Error *err) {
    (void)self;
    cleans_opened = str_clone(a, name);
    *err = errors_new(al, S("file does not exist"));
    return (HttpFile){NULL, NULL};
}

static const HttpFileSystemVT cleans_fs_vt = {NULL, cleans_open};

static void TestFileServerCleans(TestingT *t) {
    static const struct {
        const char *req_path;
        const char *open_arg;
    } tests[] = {
        {"/foo.txt", "/foo.txt"},
        {"//foo.txt", "/foo.txt"},
        {"/../foo.txt", "/foo.txt"},
    };
    HttpHandler fs = http_file_server(a, (HttpFileSystem){&cleans_fs_vt, NULL});
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest *req = new_req("GET", "http://example.com");
        req->url->path = cs(tests[i].req_path);
        cleans_opened = BURROW_STR_EMPTY;
        HttptestResponseRecorder *rec = serve(fs, req);
        if (!str_eq(cleans_opened, cs(tests[i].open_arg)))
            testing_t_errorf_v(t, "test %d: got %q, want %q", (Int)i, cleans_opened,
                               cs(tests[i].open_arg));
        done(rec, req);
    }
}

static void TestFileServerEscapesNames(TestingT *t) {
    static const Str dir_list_prefix =
        BURROW_S_INIT("<!doctype html>\n<meta name=\"viewport\" "
                      "content=\"width=device-width\">\n<pre>\n");
    static const Str dir_list_suffix = BURROW_S_INIT("\n</pre>\n");
    static const struct {
        const char *name;
        const char *escaped;
    } tests[] = {
        {"simple_name", "<a href=\"simple_name\">simple_name</a>"},
        {"\"'<>&", "<a href=\"%22%27%3C%3E&\">&#34;&#39;&lt;&gt;&amp;</a>"},
        {"?foo=bar#baz", "<a href=\"%3Ffoo=bar%23baz\">?foo=bar#baz</a>"},
        {"<combo>?foo", "<a href=\"%3Ccombo%3E%3Ffoo\">&lt;combo&gt;?foo</a>"},
        {"foo:bar", "<a href=\"./foo:bar\">foo:bar</a>"},
    };
    enum { N = sizeof tests / sizeof tests[0] };

    /* We put each test file in its own directory in the fakeFS so we can look
     * at it in isolation. */
    FakeInfo files[N];
    FakeInfo dirs[N];
    FakeInfo *ents[N];
    FakeEntry entries[2 * N];
    memset(files, 0, sizeof files);
    memset(dirs, 0, sizeof dirs);
    for (size_t i = 0; i < N; i++) {
        files[i].basename = tests[i].name;
        ents[i] = &files[i];
        dirs[i].dir = true;
        dirs[i].modtime = time_utc(time_from_unix(1000000000, 0));
        dirs[i].ents = &ents[i];
        dirs[i].nents = 1;
        entries[2 * i] = (FakeEntry){fmt_sprintf_v(a, "/%d", (Int)i), &dirs[i]};
        entries[2 * i + 1] = (FakeEntry){
            fmt_sprintf_v(a, "/%d/%s", (Int)i, cs(tests[i].name)), &files[i]};
    }
    FakeFS fsys = {entries, (Int)(2 * N)};
    HttpHandler h = http_file_server(a, fake_fs(&fsys));

    for (size_t i = 0; i < N; i++) {
        Str name = cs(tests[i].name);
        HttpRequest *req = new_req_s("GET", fmt_sprintf_v(a, "/%d", (Int)i));
        HttptestResponseRecorder *rec = serve(h, req);
        check_redirect(t, rec, "dir without a slash", fmt_sprintf_v(a, "%d/", (Int)i));
        done(rec, req);

        req = new_req_s("GET", fmt_sprintf_v(a, "/%d/", (Int)i));
        rec = serve(h, req);
        Str s = body_of(rec);
        if (!strings_has_prefix(s, dir_list_prefix) ||
            !strings_has_suffix(s, dir_list_suffix))
            testing_t_errorf_v(
                t,
                "test %q: listing dir, full output is %q, want prefix %q "
                "and suffix %q",
                name, s, dir_list_prefix, dir_list_suffix);
        Str trimmed = strings_trim_suffix(strings_trim_prefix(s, dir_list_prefix),
                                          dir_list_suffix);
        if (!str_eq(trimmed, cs(tests[i].escaped)))
            testing_t_errorf_v(t,
                               "test %q: listing dir, filename escaped to %q, want %q",
                               name, trimmed, cs(tests[i].escaped));
        done(rec, req);
    }
}

static void TestFileServerSortsNames(TestingT *t) {
    static const char contents[] = "I am a fake file";
    Time dir_mod = time_utc(time_from_unix(123, 0));
    Time file_mod = time_utc(time_from_unix(1000000000, 0));
    FakeInfo b = {.basename = "b", .contents = contents, .modtime = file_mod};
    FakeInfo fa = {.basename = "a", .contents = contents, .modtime = file_mod};
    FakeInfo *ents[] = {&b, &fa};
    FakeInfo dir = {.ents = ents, .nents = 2, .modtime = dir_mod, .dir = true};
    FakeEntry entries[] = {{S("/"), &dir}};
    FakeFS fsys = {entries, 1};

    HttpRequest *req = new_req("GET", "/");
    HttptestResponseRecorder *rec = serve(http_file_server(a, fake_fs(&fsys)), req);
    Str s = body_of(rec);
    if (!strings_contains(s, S("<a href=\"a\">a</a>\n<a href=\"b\">b</a>")))
        testing_t_errorf_v(t, "output appears to be unsorted:\n%s", s);
    done(rec, req);
}

static void TestFileServerImplicitLeadingSlash(TestingT *t) {
    Error e = os_write_file(filepath_join_v(a, 2, root, S("foo.txt")),
                            bytes_of(S("Hello world")), 0644);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "WriteFile: %v", e);
        return;
    }
    HttpDir dir = root;
    HttpHandler h = http_strip_prefix(
        a, S("/bar/"), http_file_server(a, http_dir_as_file_system(&dir)));
    HttpRequest *req = new_req("GET", "/bar/");
    HttptestResponseRecorder *rec = serve(h, req);
    if (!strings_contains(body_of(rec), S(">foo.txt<")))
        testing_t_errorf_v(t, "expected a directory listing with foo.txt, got %q",
                           body_of(rec));
    if (!str_eq(req->url->path, S("/bar/")))
        testing_t_errorf_v(t, "request path = %q after the handler; want /bar/",
                           req->url->path);
    done(rec, req);

    req = new_req("GET", "/bar/foo.txt");
    rec = serve(h, req);
    if (!str_eq(body_of(rec), S("Hello world")))
        testing_t_errorf_v(t, "expected %q, got %q", S("Hello world"), body_of(rec));
    done(rec, req);
    (void)os_remove(filepath_join_v(a, 2, root, S("foo.txt")));
}

static void dir_join_test(TestingT *t, OsFileInfo wfi, const char *d,
                          const char *name) {
    Error e = BURROW_NO_ERROR;
    HttpFile f = http_dir_open(cs(d), a, cs(name), &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "open of %s: %v", cs(name), e);
        return;
    }
    FsFileInfo gfi = f.vt->stat(f.data, a, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "stat of %s: %v", cs(name), e);
    else if (!os_same_file(gfi, wfi))
        testing_t_errorf_v(t, "%s got different file", cs(name));
    (void)f.vt->read_closer.closer.close(f.data);
}

static void TestDirJoin(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "skipping test on windows");
#endif
    Error e = BURROW_NO_ERROR;
    OsFileInfo wfi = os_stat(a, S("/etc/hosts"), &e);
    if (BURROW_FAILED(e))
        testing_t_skip_v(t, "skipping test; no /etc/hosts file");
    dir_join_test(t, wfi, "/etc/", "/hosts");
    dir_join_test(t, wfi, "/etc/", "hosts");
    dir_join_test(t, wfi, "/etc/", "../../../../hosts");
    dir_join_test(t, wfi, "/etc", "/hosts");
    dir_join_test(t, wfi, "/etc", "hosts");
    dir_join_test(t, wfi, "/etc", "../../../../hosts");

    /* Not really directories, but since we use this trick in ServeFile, test
     * it: */
    dir_join_test(t, wfi, "/etc/hosts", "");
    dir_join_test(t, wfi, "/etc/hosts", "/");
    dir_join_test(t, wfi, "/etc/hosts", "../");
}

static void TestServeFileContentType(TestingT *t) {
    static const Str ctype = BURROW_S_INIT("icecream/chocolate");
    for (int override = 0; override < 3; override++) {
        HttpRequest *req = new_req("GET", "/");
        HttptestResponseRecorder *rec = new_rec();
        if (override == 1) {
            (void)http_header_set(rec->header_map, S("Content-Type"), ctype);
        } else if (override == 2) {
            /* Explicitly inhibit sniffing. */
            Str key = S("Content-Type");
            Slice none = slice_from(NULL, 0, 0, TYPE_STRING);
            if (!map_set(rec->header_map, &key, &none))
                testing_t_fatalf_v(t, "out of memory");
        }
        http_serve_file(rw(rec), req, td("file"));
        Slice h = http_header_values(res_header(rec), S("Content-Type"));
        Str want = override == 0 ? S("text/plain; charset=utf-8") : ctype;
        if (override == 2 ? h.len != 0
                          : h.len != 1 || !str_eq(((const Str *)h.p)[0], want))
            testing_t_errorf_v(t,
                               "override %d: Content-Type mismatch: got %d values, the "
                               "first %q; want %q",
                               (Int) override, h.len,
                               h.len > 0 ? ((const Str *)h.p)[0] : BURROW_STR_EMPTY,
                               override == 2 ? BURROW_STR_EMPTY : want);
        done(rec, req);
    }
}

static void TestServeFileMimeType(TestingT *t) {
    HttpRequest *req = new_req("GET", "/");
    HttptestResponseRecorder *rec = new_rec();
    http_serve_file(rw(rec), req, td("style.css"));
    Str want = S("text/css; charset=utf-8");
    if (!str_eq(res_get(rec, "Content-Type"), want))
        testing_t_errorf_v(t, "Content-Type mismatch: got %q, want %q",
                           res_get(rec, "Content-Type"), want);
    done(rec, req);
}

/* Issue 13996 */
static void TestServeDirWithoutTrailingSlash(TestingT *t) {
    HttpRequest *req = new_req("GET", "/testdata");
    HttptestResponseRecorder *rec = new_rec();
    http_serve_file(rw(rec), req, root);
    check_redirect(t, rec, "/testdata", S("testdata/"));
    done(rec, req);
}

/* Tests that ServeFile doesn't add a Content-Length if a Content-Encoding is
 * specified. */
static void TestServeFileWithContentEncoding(TestingT *t) {
    HttpRequest *req = new_req("GET", "/");
    HttptestResponseRecorder *rec = new_rec();
    set_rec(rec, "Content-Encoding", "foo");
    http_serve_file(rw(rec), req, td("file"));
    httptest_response_recorder_flush(rec);
    int64_t g = httptest_response_recorder_result(rec)->content_length;
    if (g != -1)
        testing_t_errorf_v(t, "Content-Length mismatch: got %d, want %d", g, (Int)-1);
    done(rec, req);
}

/* Tests that ServeFile does not generate representation metadata when file
 * has not been modified, as per RFC 7232 section 4.1. */
static void TestServeFileNotModified(TestingT *t) {
    HttpRequest *req = new_req("GET", "/");
    set_req(req, "If-None-Match", "\"123\"");
    HttptestResponseRecorder *rec = new_rec();
    set_rec(rec, "Content-Type", "application/json");
    set_rec(rec, "Content-Encoding", "foo");
    set_rec(rec, "Etag", "\"123\"");
    http_serve_file(rw(rec), req, td("file"));
    httptest_response_recorder_flush(rec);
    if (body_of(rec).len != 0)
        testing_t_errorf_v(t, "non-empty body");
    if (rec->code != HTTP_STATUS_NOT_MODIFIED)
        testing_t_errorf_v(t, "status mismatch: got %d, want %d", rec->code, (Int)304);
    int64_t g = httptest_response_recorder_result(rec)->content_length;
    if (g != -1 && g != 0)
        testing_t_errorf_v(t, "Content-Length mismatch: got %d, want -1 or 0", g);
    if (res_get(rec, "Content-Type").len != 0)
        testing_t_errorf_v(t, "Content-Type present, but it should not be");
    if (res_get(rec, "Content-Encoding").len != 0)
        testing_t_errorf_v(t, "Content-Encoding present, but it should not be");
    done(rec, req);
}

static void TestServeIndexHtml(TestingT *t) {
    HttpDir dir = root;
    for (int i = 0; i < 2; i++) {
        HttpHandler h = i == 0 ? http_file_server(a, http_dir_as_file_system(&dir))
                               : http_file_server(a, http_fs(a, os_dir_fs(a, root)));
        const char *name = i == 0 ? "Dir" : "DirFS";

        HttpRequest *req = new_req("GET", "/testdata/");
        HttptestResponseRecorder *rec = serve(h, req);
        if (!str_eq(body_of(rec), index_contents))
            testing_t_errorf_v(t, "%s: for path %q got %q, want %q", cs(name),
                               S("/testdata/"), body_of(rec), index_contents);
        done(rec, req);

        req = new_req("GET", "/testdata/index.html");
        rec = serve(h, req);
        check_redirect(t, rec, "/testdata/index.html", S("./"));
        done(rec, req);
    }
}

static void TestFileServerZeroByte(TestingT *t) {
    HttpDir dir = root;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    HttpRequest *req = new_req("GET", "/");
    req->url->path = S("/..\0");
    HttptestResponseRecorder *rec = serve(h, req);
    if (rec->code == 200)
        testing_t_errorf_v(t, "got status 200; want an error");
    done(rec, req);
}

static void TestFileServerNullByte(TestingT *t) {
    static const char *const paths[] = {"/file%00", "/%00", "/file/qwe/%00"};
    HttpDir dir = testdata;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        HttpRequest *req = new_req("GET", paths[i]);
        HttptestResponseRecorder *rec = serve(h, req);
        if (rec->code != 404)
            testing_t_errorf_v(t, "Get(%q): got status %v, want 404", cs(paths[i]),
                               rec->code);
        done(rec, req);
    }
}

static void TestFileServerNamesEscape(TestingT *t) {
    static const char *const paths[] = {
        "/../testdata/file", "/NUL", /* don't read from device files on Windows */
    };
    HttpDir dir = testdata;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        HttpRequest *req = new_req("GET", "/");
        req->url->path = cs(paths[i]);
        HttptestResponseRecorder *rec = serve(h, req);
        if (rec->code < 400 || rec->code > 599)
            testing_t_errorf_v(t, "Get(%q): got status %v, want 4xx or 5xx",
                               cs(paths[i]), rec->code);
        done(rec, req);
    }
}

static void TestDirectoryIfNotModified(TestingT *t) {
    static const char index_text[] = "I am a fake index.html file";
    Time file_mod = time_utc(time_from_unix(1000000000, 0));
    Str file_mod_str = time_format(file_mod, a, HTTP_TIME_FORMAT);
    Time dir_mod = time_utc(time_from_unix(123, 0));
    FakeInfo index_file = {
        .basename = "index.html", .contents = index_text, .modtime = file_mod};
    FakeInfo *ents[] = {&index_file};
    FakeInfo dir = {.ents = ents, .nents = 1, .modtime = dir_mod, .dir = true};
    FakeEntry entries[] = {{S("/"), &dir}, {S("/index.html"), &index_file}};
    FakeFS fsys = {entries, 2};
    HttpHandler h = http_file_server(a, fake_fs(&fsys));

    /* Initial fetch of /index.html. */
    HttpRequest *req = new_req("GET", "/");
    HttptestResponseRecorder *rec = serve(h, req);
    if (!str_eq(body_of(rec), cs(index_text)))
        testing_t_fatalf_v(t, "Got body %q; want %q", body_of(rec), cs(index_text));
    Str last_mod = str_clone(a, res_get(rec, "Last-Modified"));
    done(rec, req);
    if (!str_eq(last_mod, file_mod_str))
        testing_t_fatalf_v(t, "initial Last-Modified = %q; want %q", last_mod,
                           file_mod_str);

    /* Fetch /index.html when it has not been modified. */
    req = new_req("GET", "/");
    (void)http_header_set(req->header, S("If-Modified-Since"), last_mod);
    rec = serve(h, req);
    if (rec->code != 304)
        testing_t_errorf_v(t, "Code after If-Modified-Since request = %v; want 304",
                           rec->code);
    httptest_response_recorder_free(rec);

    /* Fetch /index.html after it has been modified. */
    index_file.modtime = time_add(index_file.modtime, TIME_HOUR);
    rec = serve(h, req);
    if (rec->code != 200)
        testing_t_errorf_v(
            t, "Code after second If-Modified-Since request = %v; want 200", rec->code);
    done(rec, req);
}

/* ------------------------------------------------------------ TestServeContent */

enum { MT_ZERO, MT_HTML, MT_2014, MT_2014_NANOS, MT_UNIX0 };
enum { CT_FILE, CT_STRING, CT_PANIC };

/* Header values that start with '$' are made when the test runs: "$html" is
 * the modification time of index.html, and "$html-2" two seconds before it. */
typedef struct ServeContentCase {
    const char *name;
    const char *file;    /* in testdata, for CT_FILE */
    const char *content; /* for CT_STRING */
    const char *serve_etag;
    const char *serve_content_type;
    const char *req_header[2][2];
    const char *want_last_mod;
    const char *want_content_type;
    const char *want_content_range;
    Int want_status;
    int kind;
    int modtime;
} ServeContentCase;

static const ServeContentCase serve_content_tests[] = {
    {.name = "no_last_modified",
     .file = "style.css",
     .want_content_type = "text/css; charset=utf-8",
     .want_status = 200},
    {.name = "with_last_modified",
     .file = "index.html",
     .want_content_type = "text/html; charset=utf-8",
     .modtime = MT_HTML,
     .want_last_mod = "$html",
     .want_status = 200},
    {.name = "not_modified_modtime",
     .file = "style.css",
     .serve_etag = "\"foo\"", /* Last-Modified sent only when no ETag */
     .modtime = MT_HTML,
     .req_header = {{"If-Modified-Since", "$html"}},
     .want_status = 304},
    {.name = "not_modified_modtime_with_contenttype",
     .file = "style.css",
     .serve_content_type = "text/css", /* explicit content type */
     .serve_etag = "\"foo\"",          /* Last-Modified sent only when no ETag */
     .modtime = MT_HTML,
     .req_header = {{"If-Modified-Since", "$html"}},
     .want_status = 304},
    {.name = "not_modified_etag",
     .file = "style.css",
     .serve_etag = "\"foo\"",
     .req_header = {{"If-None-Match", "\"foo\""}},
     .want_status = 304},
    {.name = "not_modified_etag_no_seek",
     .kind = CT_PANIC,          /* should never be called */
     .serve_etag = "W/\"foo\"", /* If-None-Match uses weak ETag comparison */
     .req_header = {{"If-None-Match", "\"baz\", W/\"foo\""}},
     .want_status = 304},
    {.name = "if_none_match_mismatch",
     .file = "style.css",
     .serve_etag = "\"foo\"",
     .req_header = {{"If-None-Match", "\"Foo\""}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "if_none_match_malformed",
     .file = "style.css",
     .serve_etag = "\"foo\"",
     .req_header = {{"If-None-Match", ","}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "range_good",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"Range", "bytes=0-4"}},
     .want_status = 206,
     .want_content_type = "text/css; charset=utf-8",
     .want_content_range = "bytes 0-4/8"},
    {.name = "range_match",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"Range", "bytes=0-4"}, {"If-Range", "\"A\""}},
     .want_status = 206,
     .want_content_type = "text/css; charset=utf-8",
     .want_content_range = "bytes 0-4/8"},
    {.name = "range_match_weak_etag",
     .file = "style.css",
     .serve_etag = "W/\"A\"",
     .req_header = {{"Range", "bytes=0-4"}, {"If-Range", "W/\"A\""}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "range_no_overlap",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"Range", "bytes=10-20"}},
     .want_status = 416,
     .want_content_type = "text/plain; charset=utf-8",
     .want_content_range = "bytes */8"},
    /* An If-Range resource for entity "A", but entity "B" is now current. The
     * Range request should be ignored. */
    {.name = "range_no_match",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"Range", "bytes=0-4"}, {"If-Range", "\"B\""}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "range_with_modtime",
     .file = "style.css",
     .modtime = MT_2014,
     .req_header = {{"Range", "bytes=0-4"},
                    {"If-Range", "Wed, 25 Jun 2014 17:12:18 GMT"}},
     .want_status = 206,
     .want_content_type = "text/css; charset=utf-8",
     .want_content_range = "bytes 0-4/8",
     .want_last_mod = "Wed, 25 Jun 2014 17:12:18 GMT"},
    {.name = "range_with_modtime_mismatch",
     .file = "style.css",
     .modtime = MT_2014,
     .req_header = {{"Range", "bytes=0-4"},
                    {"If-Range", "Wed, 25 Jun 2014 17:12:19 GMT"}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8",
     .want_last_mod = "Wed, 25 Jun 2014 17:12:18 GMT"},
    {.name = "range_with_modtime_nanos",
     .file = "style.css",
     .modtime = MT_2014_NANOS,
     .req_header = {{"Range", "bytes=0-4"},
                    {"If-Range", "Wed, 25 Jun 2014 17:12:18 GMT"}},
     .want_status = 206,
     .want_content_type = "text/css; charset=utf-8",
     .want_content_range = "bytes 0-4/8",
     .want_last_mod = "Wed, 25 Jun 2014 17:12:18 GMT"},
    {.name = "unix_zero_modtime",
     .kind = CT_STRING,
     .content = "<html>foo",
     .modtime = MT_UNIX0,
     .want_status = 200,
     .want_content_type = "text/html; charset=utf-8"},
    {.name = "ifmatch_matches",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"If-Match", "\"Z\", \"A\""}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "ifmatch_star",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"If-Match", "*"}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8"},
    {.name = "ifmatch_failed",
     .file = "style.css",
     .serve_etag = "\"A\"",
     .req_header = {{"If-Match", "\"B\""}},
     .want_status = 412},
    {.name = "ifmatch_fails_on_weak_etag",
     .file = "style.css",
     .serve_etag = "W/\"A\"",
     .req_header = {{"If-Match", "W/\"A\""}},
     .want_status = 412},
    {.name = "if_unmodified_since_true",
     .file = "style.css",
     .modtime = MT_HTML,
     .req_header = {{"If-Unmodified-Since", "$html"}},
     .want_status = 200,
     .want_content_type = "text/css; charset=utf-8",
     .want_last_mod = "$html"},
    {.name = "if_unmodified_since_false",
     .file = "style.css",
     .modtime = MT_HTML,
     .req_header = {{"If-Unmodified-Since", "$html-2"}},
     .want_status = 412,
     .want_last_mod = "$html"},
};

static Time sc_html_mod_time;

static Str sc_value(const char *v) {
    if (v == NULL)
        return BURROW_STR_EMPTY;
    if (strcmp(v, "$html") == 0)
        return time_format(time_utc(sc_html_mod_time), a, HTTP_TIME_FORMAT);
    if (strcmp(v, "$html-2") == 0)
        return time_format(time_utc(time_add(sc_html_mod_time, -2 * TIME_SECOND)), a,
                           HTTP_TIME_FORMAT);
    return cs(v);
}

static Time sc_modtime(int which) {
    if (which == MT_HTML)
        return sc_html_mod_time;
    if (which == MT_2014)
        return time_date(2014, TIME_JUNE, 25, 17, 12, 18, 0, time_utc_loc);
    if (which == MT_2014_NANOS)
        return time_date(2014, TIME_JUNE, 25, 17, 12, 18, 123, time_utc_loc);
    if (which == MT_UNIX0)
        return time_from_unix(0, 0);
    Time zero;
    memset(&zero, 0, sizeof zero);
    return zero;
}

static void serve_content_case(TestingT *t, const ServeContentCase *tt,
                               const char *method) {
    Str name = cs(tt->name);
    IoReadSeeker content;
    OsFile *f = NULL;
    if (tt->kind == CT_FILE) {
        Error e = BURROW_NO_ERROR;
        f = os_open(a, td(tt->file), &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "test %q: %v", name, e);
            return;
        }
        content = os_rs(f);
    } else if (tt->kind == CT_STRING) {
        content = strings_rs(cs(tt->content));
    } else {
        content = (IoReadSeeker){&panic_rs_vt, NULL};
    }

    HttpRequest *req = new_req(method, "/");
    for (int i = 0; i < 2; i++)
        if (tt->req_header[i][0] != NULL)
            (void)http_header_set(req->header, cs(tt->req_header[i][0]),
                                  sc_value(tt->req_header[i][1]));
    HttptestResponseRecorder *rec = new_rec();
    if (tt->serve_etag != NULL)
        set_rec(rec, "ETag", tt->serve_etag);
    if (tt->serve_content_type != NULL)
        set_rec(rec, "Content-Type", tt->serve_content_type);
    Str base = tt->file != NULL ? filepath_base(cs(tt->file)) : S(".");
    http_serve_content(rw(rec), req, base, sc_modtime(tt->modtime), content);

    if (rec->code != tt->want_status)
        testing_t_errorf_v(t, "test %q using %q: got status = %d; want %d", name,
                           cs(method), rec->code, tt->want_status);
    Str g = res_get(rec, "Content-Type");
    if (!str_eq(g, cs(tt->want_content_type)))
        testing_t_errorf_v(t, "test %q using %q: got content-type = %q, want %q", name,
                           cs(method), g, cs(tt->want_content_type));
    g = res_get(rec, "Content-Range");
    if (!str_eq(g, cs(tt->want_content_range)))
        testing_t_errorf_v(t, "test %q using %q: got content-range = %q, want %q", name,
                           cs(method), g, cs(tt->want_content_range));
    g = res_get(rec, "Last-Modified");
    Str want = sc_value(tt->want_last_mod);
    if (!str_eq(g, want))
        testing_t_errorf_v(t, "test %q using %q: got last-modified = %q, want %q", name,
                           cs(method), g, want);
    done(rec, req);
    if (f != NULL)
        os_file_free(f);
}

static void TestServeContent(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, td("index.html"), &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        return;
    }
    sc_html_mod_time = fi.vt->mod_time(fi.data);
    for (size_t i = 0; i < sizeof serve_content_tests / sizeof serve_content_tests[0];
         i++) {
        serve_content_case(t, &serve_content_tests[i], "GET");
        serve_content_case(t, &serve_content_tests[i], "HEAD");
    }
}

/* ------------------------------------------------------------- Errors */

/* Issue 12991 */
static FsFileInfo issue12991_stat(void *self, Alloc *al, Error *err) {
    (void)self;
    (void)al;
    *err = fs_err_permission;
    return (FsFileInfo){NULL, NULL};
}

static const HttpFileVT issue12991_file_vt = {
    {{NULL, ff_read}, {NULL, ff_close}}, ff_seek, NULL, issue12991_stat, NULL,
};

static HttpFile issue12991_open(void *self, Alloc *al, Str name, Error *err) {
    (void)self;
    (void)al;
    (void)name;
    *err = BURROW_NO_ERROR;
    return (HttpFile){&issue12991_file_vt, NULL};
}

static const HttpFileSystemVT issue12991_fs_vt = {NULL, issue12991_open};

static void TestServerFileStatError(TestingT *t) {
    HttptestResponseRecorder *rec = new_rec();
    HttpRequest *r = new_req("GET", "http://foo/");
    burrow__http_serve_file(rw(rec), r, (HttpFileSystem){&issue12991_fs_vt, NULL},
                            S("file.txt"), false);
    Str body = body_of(rec);
    if (!strings_contains(body, S("403")) || !strings_contains(body, S("Forbidden")))
        testing_t_errorf_v(t, "wanted 403 forbidden message; got: %s", body);
    done(rec, r);
}

static void file_server_error_messages(TestingT *t, bool keep_headers) {
    static const char *const hdrs[] = {"Etag", "Last-Modified", "Cache-Control"};
    burrow__http_godebug_set(keep_headers ? "httpservecontentkeepheaders=1" : "");
    FakeInfo e500 = {.err = errors_new(a, S("random error"))};
    FakeInfo e403 = {.err = fs_path_error_new(a, S(""), S(""), fs_err_permission)};
    FakeEntry entries[] = {{S("/500"), &e500}, {S("/403"), &e403}};
    FakeFS fsys = {entries, 2};
    HttpHandler server = http_file_server(a, fake_fs(&fsys));
    static const Int codes[] = {403, 404, 500};
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        HttpRequest *req = new_req_s("GET", fmt_sprintf_v(a, "/%d", codes[i]));
        HttptestResponseRecorder *rec = new_rec();
        set_rec(rec, "Etag", "étude");
        set_rec(rec, "Cache-Control", "yes");
        set_rec(rec, "Content-Type", "awesome");
        set_rec(rec, "Last-Modified", "yesterday");
        http_handler_serve_http(server, rw(rec), req);
        if (rec->code != codes[i])
            testing_t_errorf_v(t, "GET /%d: StatusCode = %d; want %d", codes[i],
                               rec->code, codes[i]);
        for (size_t j = 0; j < sizeof hdrs / sizeof hdrs[0]; j++) {
            bool got = burrow__http_header_has(res_header(rec), cs(hdrs[j]));
            if (got != keep_headers)
                testing_t_errorf_v(t, "GET /%d: Header[%q] = %q, want %s", codes[i],
                                   cs(hdrs[j]), res_get(rec, hdrs[j]),
                                   keep_headers ? S("present") : S("not present"));
        }
        done(rec, req);
    }
    burrow__http_godebug_set(NULL);
}

static void TestFileServerErrorMessages(TestingT *t) {
    file_server_error_messages(t, false);
    file_server_error_messages(t, true);
}

static void not_dir_open(TestingT *t, const char *which, HttpFileSystem fsys,
                         const char *name) {
    Error e = BURROW_NO_ERROR;
    HttpFile f = fsys.vt->open(fsys.data, a, cs(name), &e);
    if (BURROW_OK(e)) {
        (void)f.vt->read_closer.closer.close(f.data);
        testing_t_errorf_v(t, "%s: Open(%q): err == nil; want != nil", cs(which),
                           cs(name));
        return;
    }
    if (!errors_is(e, fs_err_not_exist))
        testing_t_errorf_v(
            t, "%s: err = %v; errors.Is(err, fs.ErrNotExist) = false; want true",
            cs(which), e);
}

static void TestFileServerNotDirError(TestingT *t) {
    HttpDir dir = testdata;
    for (int i = 0; i < 2; i++) {
        const char *which = i == 0 ? "Dir" : "FS";
        HttpFileSystem fsys =
            i == 0 ? http_dir_as_file_system(&dir) : http_fs(a, os_dir_fs(a, testdata));
        HttpRequest *req = new_req("GET", "/index.html/not-a-file");
        HttptestResponseRecorder *rec = serve(http_file_server(a, fsys), req);
        if (rec->code != 404)
            testing_t_errorf_v(t, "%s: StatusCode = %v; want 404", cs(which),
                               rec->code);
        done(rec, req);

        /* Go also opens testdata by its relative path, which needs the working
         * directory to be where testdata is. This is the absolute path. */
        not_dir_open(t, which, fsys, "/index.html/not-a-file");
        not_dir_open(t, which, fsys, "/index.html/not-a-dir/not-a-file");
    }
}

static Str clean_path_log[4];
static Int clean_path_n;

static HttpFile clean_path_open(void *self, Alloc *al, Str path, Error *err) {
    (void)self;
    if (clean_path_n < 4)
        clean_path_log[clean_path_n] = str_clone(a, path);
    clean_path_n++;
    if (str_eq(path, S("/")) || str_eq(path, S("/dir")) || str_eq(path, S("/dir/"))) {
        /* Just return back something that's a directory. */
        return http_dir_open(root, al, S("."), err);
    }
    *err = fs_err_not_exist;
    return (HttpFile){NULL, NULL};
}

static const HttpFileSystemVT clean_path_fs_vt = {NULL, clean_path_open};

static void TestFileServerCleanPath(TestingT *t) {
    static const struct {
        const char *path;
        Int want_code;
        const char *want_open[2];
    } tests[] = {
        {"/", 200, {"/", "/index.html"}},
        {"/dir", 301, {"/dir", NULL}},
        {"/dir/", 200, {"/dir", "/dir/index.html"}},
    };
    HttpHandler h = http_file_server(a, (HttpFileSystem){&clean_path_fs_vt, NULL});
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        clean_path_n = 0;
        HttpRequest *req = new_req_s(
            "GET", fmt_sprintf_v(a, "http://foo.localhost%s", cs(tests[i].path)));
        HttptestResponseRecorder *rec = serve(h, req);
        Int want_n = tests[i].want_open[1] != NULL ? 2 : 1;
        bool same = clean_path_n == want_n;
        for (Int j = 0; same && j < want_n; j++)
            same = str_eq(clean_path_log[j], cs(tests[i].want_open[j]));
        if (!same)
            testing_t_errorf_v(t, "For %s: %d opens, the first %q; want %q and %q",
                               cs(tests[i].path), clean_path_n,
                               clean_path_n > 0 ? clean_path_log[0] : BURROW_STR_EMPTY,
                               cs(tests[i].want_open[0]), cs(tests[i].want_open[1]));
        if (rec->code != tests[i].want_code)
            testing_t_errorf_v(t, "For %s: Response code = %d; want %d",
                               cs(tests[i].path), rec->code, tests[i].want_code);
        done(rec, req);
    }
}

static void TestScanETag(TestingT *t) {
    static const struct {
        const char *in;
        const char *want_etag;
        const char *want_remain;
    } tests[] = {
        {"W/\"etag-1\"", "W/\"etag-1\"", ""},
        {"\"etag-2\"", "\"etag-2\"", ""},
        {"\"etag-1\", \"etag-2\"", "\"etag-1\"", ", \"etag-2\""},
        {"", "", ""},
        {"W/", "", ""},
        {"W/\"truc", "", ""},
        {"w/\"case-sensitive\"", "", ""},
        {"\"spaced etag\"", "", ""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str remain;
        Str etag = burrow__http_scan_etag(cs(tests[i].in), &remain);
        if (!str_eq(etag, cs(tests[i].want_etag)) ||
            !str_eq(remain, cs(tests[i].want_remain)))
            testing_t_errorf_v(t, "scanETag(%q)=%q %q, want %q %q", cs(tests[i].in),
                               etag, remain, cs(tests[i].want_etag),
                               cs(tests[i].want_remain));
    }
}

/* Issue 40940: Ensure that we only accept non-negative suffix-lengths in
 * "Range": "bytes=-N", and should reject "bytes=--2". Go asks for
 * /index.html and follows the redirect to /, with the Range header on both. */
static void TestServeFileRejectsInvalidSuffixLengths(TestingT *t) {
    static const struct {
        const char *r;
        Int want_code;
        const char *want_body;
    } tests[] = {
        {"bytes=--6", 416, "invalid range\n"},
        {"bytes=--0", 416, "invalid range\n"},
        {"bytes=---0", 416, "invalid range\n"},
        {"bytes=-6", 206, "hello\n"},
        {"bytes=6-", 206, "html says hello\n"},
        {"bytes=-6-", 416, "invalid range\n"},
        {"bytes=-0", 206, ""},
        {"bytes=", 200, "index.html says hello\n"},
    };
    HttpDir dir = testdata;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest *req = new_req("GET", "/index.html");
        set_req(req, "Range", tests[i].r);
        HttptestResponseRecorder *rec = serve(h, req);
        check_redirect(t, rec, tests[i].r, S("./"));
        done(rec, req);

        req = new_req("GET", "/");
        set_req(req, "Range", tests[i].r);
        rec = serve(h, req);
        if (rec->code != tests[i].want_code)
            testing_t_errorf_v(t, "%s: StatusCode mismatch: got %d want %d",
                               cs(tests[i].r), rec->code, tests[i].want_code);
        if (!str_eq(body_of(rec), cs(tests[i].want_body)))
            testing_t_errorf_v(t, "%s: Content mismatch:\nGot:  %q\nWant: %q",
                               cs(tests[i].r), body_of(rec), cs(tests[i].want_body));
        done(rec, req);
    }
}

static void TestFileServerMethods(TestingT *t) {
    static const char *const methods[] = {"GET",   "HEAD",   "POST",    "PUT",
                                          "PATCH", "DELETE", "OPTIONS", "TRACE"};
    HttpDir dir = testdata;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        HttpRequest *req = new_req(methods[i], "/file");
        HttptestResponseRecorder *rec = serve(h, req);
        Str want = strcmp(methods[i], "HEAD") == 0 ? BURROW_STR_EMPTY : file_contents;
        if (!str_eq(body_of(rec), want))
            testing_t_errorf_v(t, "%v: got body %q, want %q", cs(methods[i]),
                               body_of(rec), want);
        if (!str_eq(res_get(rec, "Content-Length"), S("11")))
            testing_t_errorf_v(t, "%v: got Content-Length %q, want %q", cs(methods[i]),
                               res_get(rec, "Content-Length"), S("11"));
        done(rec, req);
    }
}

static FstestMapFS index_map_fs(FstestMapFile *file) {
    FstestMapFS fsys = fstest_map_fs_make(a);
    file->data = bytes_of(S("index.html says hello"));
    (void)fstest_map_fs_set(fsys, S("index.html"), file);
    return fsys;
}

static void TestFileServerFS(TestingT *t) {
    FstestMapFile file;
    memset(&file, 0, sizeof file);
    FstestMapFS fsys = index_map_fs(&file);
    HttpHandler h = http_file_server_fs(a, fstest_map_fs_as_fs(fsys));

    HttpRequest *req = new_req("GET", "/index.html");
    HttptestResponseRecorder *rec = serve(h, req);
    check_redirect(t, rec, "/index.html", S("./"));
    done(rec, req);

    req = new_req("GET", "/");
    rec = serve(h, req);
    if (!str_eq(body_of(rec), S("index.html says hello")))
        testing_t_errorf_v(t, "for path %q got %q, want %q", S("index.html"),
                           body_of(rec), S("index.html says hello"));
    done(rec, req);
}

static void TestServeFileFS(TestingT *t) {
    FstestMapFile file;
    memset(&file, 0, sizeof file);
    FstestMapFS fsys = index_map_fs(&file);

    HttpRequest *req = new_req("GET", "/index.html");
    HttptestResponseRecorder *rec = new_rec();
    http_serve_file_fs(rw(rec), req, fstest_map_fs_as_fs(fsys), S("index.html"));
    check_redirect(t, rec, "/index.html", S("./"));
    done(rec, req);

    req = new_req("GET", "/");
    rec = new_rec();
    http_serve_file_fs(rw(rec), req, fstest_map_fs_as_fs(fsys), S("index.html"));
    if (!str_eq(body_of(rec), S("index.html says hello")))
        testing_t_errorf_v(t, "for path %q got %q, want %q", S("index.html"),
                           body_of(rec), S("index.html says hello"));
    done(rec, req);
}

/* Issue 63769 */
static void TestFileServerDirWithRootFile(TestingT *t) {
    HttpDir dir = td("index.html");
    for (int i = 0; i < 2; i++) {
        HttpHandler h = i == 0 ? http_file_server(a, http_dir_as_file_system(&dir))
                               : http_file_server_fs(a, os_dir_fs(a, dir));
        HttpRequest *req = new_req("GET", "/");
        HttptestResponseRecorder *rec = serve(h, req);
        if (rec->code != 500)
            testing_t_errorf_v(t, "%s: StatusCode mismatch: got %d, want: %d",
                               i == 0 ? S("FileServer") : S("FileServerFS"), rec->code,
                               (Int)500);
        done(rec, req);
    }
}

static void serve_content_headers_with_error(TestingT *t, bool keep_headers) {
    burrow__http_godebug_set(keep_headers ? "httpservecontentkeepheaders=1" : "");
    HttpRequest *req = new_req("GET", "/");
    set_req(req, "Range", "bytes=100-10000");
    HttptestResponseRecorder *rec = new_rec();
    set_rec(rec, "Content-Type", "application/octet-stream");
    set_rec(rec, "Content-Length", "7");
    set_rec(rec, "Content-Encoding", "gzip");
    set_rec(rec, "Etag", "\"abcdefgh\"");
    set_rec(rec, "Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT");
    set_rec(rec, "Cache-Control", "immutable");
    set_rec(rec, "Other-Header", "test");
    Time zero;
    memset(&zero, 0, sizeof zero);
    http_serve_content(rw(rec), req, BURROW_STR_EMPTY, zero, strings_rs(S("content")));

    static const struct {
        const char *key;
        const char *want;
        bool kept;
    } want[] = {
        {"Content-Type", "text/plain; charset=utf-8", false},
        {"Content-Encoding", "gzip", true},
        {"Etag", "\"abcdefgh\"", true},
        {"Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT", true},
        {"Cache-Control", "immutable", true},
        {"Content-Range", "bytes */7", false},
        {"Other-Header", "test", false},
    };
    if (rec->code != 416)
        testing_t_errorf_v(t, "got status = %d; want %d", rec->code, (Int)416);
    if (!str_eq(body_of(rec), S("invalid range: failed to overlap\n")))
        testing_t_errorf_v(t, "got body = %q; want %q", body_of(rec),
                           S("invalid range: failed to overlap\n"));
    /* Go also checks the Content-Length, which its server adds to the error
     * page after Error took the handler's out. */
    if (res_get(rec, "Content-Length").len != 0)
        testing_t_errorf_v(t, "got content-length = %q, want none from the handler",
                           res_get(rec, "Content-Length"));
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        Str w = want[i].kept && !keep_headers ? BURROW_STR_EMPTY : cs(want[i].want);
        Str g = res_get(rec, want[i].key);
        if (!str_eq(g, w))
            testing_t_errorf_v(t, "got %s = %q, want %q", cs(want[i].key), g, w);
    }
    done(rec, req);
    burrow__http_godebug_set(NULL);
}

static void TestServeContentHeadersWithError(TestingT *t) {
    serve_content_headers_with_error(t, false);
    serve_content_headers_with_error(t, true);
}

/* Not in Go's tests: the file handler puts back the request path it made for
 * a path without a leading slash when the request has no arena for it. */
static void TestFileServerRestoresPath(TestingT *t) {
    HttpDir dir = testdata;
    HttpHandler h = http_file_server(a, http_dir_as_file_system(&dir));
    HttpRequest *req = new_req("GET", "/");
    Url u = *req->url;
    u.path = S("file");
    Url *saved = req->url;
    Alloc *saved_a = req->a;
    req->url = &u;
    req->a = NULL;
    HttptestResponseRecorder *rec = serve(h, req);
    if (!str_eq(body_of(rec), file_contents))
        testing_t_errorf_v(t, "body = %q; want %q", body_of(rec), file_contents);
    if (!str_eq(u.path, S("file")))
        testing_t_errorf_v(t, "path after the handler = %q; want %q", u.path,
                           S("file"));
    req->url = saved;
    req->a = saved_a;
    done(rec, req);
}

static bool setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    root = os_mkdir_temp(a, S(""), S("burrow-http-fs-test-*"), &e);
    if (BURROW_FAILED(e))
        return false;
    testdata = filepath_join_v(a, 2, root, S("testdata"));
    if (BURROW_FAILED(os_mkdir(testdata, 0777)))
        return false;
    return BURROW_OK(os_write_file(td("file"), bytes_of(file_contents), 0644)) &&
           BURROW_OK(os_write_file(td("index.html"), bytes_of(index_contents), 0644)) &&
           BURROW_OK(os_write_file(td("style.css"), bytes_of(style_contents), 0644));
}

#define TESTS(X)                                                                       \
    X(TestParseRange)                                                                  \
    X(TestServeFile)                                                                   \
    X(TestServeFile_DotDot)                                                            \
    X(TestServeFileDirPanicEmptyPath)                                                  \
    X(TestServeContentWithEmptyContentIgnoreRanges)                                    \
    X(TestFileServerCleans)                                                            \
    X(TestFileServerEscapesNames)                                                      \
    X(TestFileServerSortsNames)                                                        \
    X(TestFileServerImplicitLeadingSlash)                                              \
    X(TestDirJoin)                                                                     \
    X(TestServeFileContentType)                                                        \
    X(TestServeFileMimeType)                                                           \
    X(TestServeDirWithoutTrailingSlash)                                                \
    X(TestServeFileWithContentEncoding)                                                \
    X(TestServeFileNotModified)                                                        \
    X(TestServeIndexHtml)                                                              \
    X(TestFileServerZeroByte)                                                          \
    X(TestFileServerNullByte)                                                          \
    X(TestFileServerNamesEscape)                                                       \
    X(TestDirectoryIfNotModified)                                                      \
    X(TestServeContent)                                                                \
    X(TestServerFileStatError)                                                         \
    X(TestFileServerErrorMessages)                                                     \
    X(TestFileServerNotDirError)                                                       \
    X(TestFileServerCleanPath)                                                         \
    X(TestScanETag)                                                                    \
    X(TestServeFileRejectsInvalidSuffixLengths)                                        \
    X(TestFileServerMethods)                                                           \
    X(TestFileServerFS)                                                                \
    X(TestServeFileFS)                                                                 \
    X(TestFileServerDirWithRootFile)                                                   \
    X(TestServeContentHeadersWithError)                                                \
    X(TestFileServerRestoresPath)

static int http_fs_main(TestingM *m) {
    int r = 1;
    if (setup())
        r = testing_m_run(m);
    if (root.len > 0)
        (void)os_remove_all(root);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(http_fs_main, TESTS)
