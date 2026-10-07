/* Derived from Go's src/net/http/request_test.go: the tests of forms,
 * Request.Clone and ErrNotSupported.
 * Go source: go1.27.1.
 *
 * Go's validateTestMultipartContents tells a file in memory from one on disk
 * by whether multipart.File is an *os.File. Here that is whether the file's
 * header has a tmpfile.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime/multipart.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* io.NopCloser(strings.NewReader(s)). */
static IoReadCloser body_of(Alloc *a, Str s) {
    StringsReader *r = strings_new_reader(a, s);
    IoNopCloser *c = (IoNopCloser *)mem_alloc(a, sizeof *c, _Alignof(IoNopCloser));
    if (r == NULL || c == NULL)
        return (IoReadCloser){NULL, NULL};
    *c = io_nop_closer(strings_reader_as_io_reader(r));
    return io_nop_closer_as_io_read_closer(c);
}

/* NewRequest with a strings.Reader body made in a. */
static HttpRequest *new_request(Alloc *a, const char *method, const char *url,
                                const char *body) {
    IoReader r = {NULL, NULL};
    if (body != NULL) {
        StringsReader *sr = strings_new_reader(a, cs(body));
        if (sr == NULL)
            return NULL;
        r = strings_reader_as_io_reader(sr);
    }
    Error err;
    return http_new_request(heap_allocator(), cs(method), cs(url), r, &err);
}

/* A request made by hand, as Go's &Request{...}: method, a Content-Type when
 * ct is not NULL, and body. Free its arena with arena_free. */
static void hand_request(HttpRequest *req, Alloc *a, const char *method, const char *ct,
                         IoReadCloser body) {
    memset(req, 0, sizeof *req);
    req->method = cs(method);
    req->header = http_header_make(a);
    if (ct != NULL)
        (void)http_header_set(req->header, cs("Content-Type"), cs(ct));
    req->body = body;
}

/* url.Values from key, value pairs ending in NULL. */
static UrlValues values_of(Alloc *a, const char *const *kv) {
    UrlValues v = url_values_make(a);
    for (Int i = 0; kv[i] != NULL; i += 2)
        (void)url_values_add(v, cs(kv[i]), cs(kv[i + 1]));
    return v;
}

/* reflect.DeepEqual for two url.Values, through Encode, which sorts the keys
 * and keeps each key's values in order. */
static bool values_equal(Alloc *a, UrlValues got, UrlValues want) {
    if ((got == NULL) != (want == NULL))
        return false;
    if (map_len(got) != map_len(want))
        return false;
    return str_eq(url_values_encode(got, a), url_values_encode(want, a));
}

/* slices.Equal(v[key], want), with want ending in NULL. */
static bool all_equal(UrlValues v, const char *key, const char *const *want) {
    Slice got = url_values_get_all(v, cs(key));
    Int n = 0;
    while (want[n] != NULL)
        n++;
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++)
        if (!str_eq(((const Str *)got.p)[i], cs(want[i])))
            return false;
    return true;
}

static void TestQuery(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest req;
    memset(&req, 0, sizeof req);
    req.method = cs("GET");
    Error err;
    req.url = url_parse(a, cs("http://www.google.com/search?q=foo&q=bar"), &err);
    Str q = http_request_form_value(&req, cs("q"));
    if (!str_eq(q, cs("foo")))
        testing_t_errorf_v(t, "req.FormValue(\"q\") = %q, want \"foo\"", q);
    arena_free(&req.arena);
    arena_free(&ar);
}

/* Issue #25192: Test that ParseForm fails but still parses the form when a URL
 * containing a semicolon is provided. */
static void TestParseFormSemicolonSeparator(TestingT *t) {
    static const char *const methods[] = {"POST", "PATCH", "PUT", "GET"};
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        HttpRequest *req = new_request(
            a, methods[i], "http://www.google.com/search?q=foo;q=bar&a=1", "q");
        if (req == NULL) {
            arena_free(&ar);
            FATALF("NewRequest failed");
        }
        Error err = http_request_parse_form(req);
        bool bad = false;
        if (BURROW_OK(err)) {
            testing_t_errorf_v(
                t, "for method %s, ParseForm expected an error, got success",
                cs(methods[i]));
            bad = true;
        }
        static const char *const want[] = {"a", "1", NULL};
        if (!bad && !values_equal(a, req->form, values_of(a, want))) {
            testing_t_errorf_v(
                t, "for method %s, ParseForm expected req.Form = %s, want %s",
                cs(methods[i]), url_values_encode(req->form, a), cs("a=1"));
            bad = true;
        }
        http_request_free(req);
        arena_free(&ar);
        if (bad)
            return;
    }
}

static void TestParseFormQuery(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_request(
        a, "POST",
        "http://www.google.com/search?q=foo&q=bar&both=x&prio=1&orphan=nope&empty=not",
        "z=post&both=y&prio=2&=nokey&orphan&empty=&");
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    (void)http_header_set(req->header, cs("Content-Type"),
                          cs("application/x-www-form-urlencoded; param=value"));

    Str q = http_request_form_value(req, cs("q"));
    if (!str_eq(q, cs("foo")))
        testing_t_errorf_v(t, "req.FormValue(\"q\") = %q, want \"foo\"", q);
    Str z = http_request_form_value(req, cs("z"));
    if (!str_eq(z, cs("post")))
        testing_t_errorf_v(t, "req.FormValue(\"z\") = %q, want \"post\"", z);
    if (url_values_has(req->post_form, cs("q")))
        testing_t_errorf_v(t, "req.PostForm[\"q\"] = %q, want no entry in map",
                           url_values_get(req->post_form, cs("q")));
    Str bz = http_request_post_form_value(req, cs("z"));
    if (!str_eq(bz, cs("post")))
        testing_t_errorf_v(t, "req.PostFormValue(\"z\") = %q, want \"post\"", bz);
    static const char *const qs[] = {"foo", "bar", NULL};
    if (!all_equal(req->form, "q", qs))
        testing_t_errorf_v(t, "req.Form[\"q\"] wrong, want [\"foo\", \"bar\"]");
    static const char *const both[] = {"y", "x", NULL};
    if (!all_equal(req->form, "both", both))
        testing_t_errorf_v(t, "req.Form[\"both\"] wrong, want [\"y\", \"x\"]");
    Str prio = http_request_form_value(req, cs("prio"));
    if (!str_eq(prio, cs("2")))
        testing_t_errorf_v(t, "req.FormValue(\"prio\") = %q, want \"2\" (from body)",
                           prio);
    static const char *const orphan[] = {"", "nope", NULL};
    if (!all_equal(req->form, "orphan", orphan))
        testing_t_errorf_v(t, "req.Form[\"orphan\"] wrong, want [\"\", \"nope\"]");
    static const char *const empty[] = {"", "not", NULL};
    if (!all_equal(req->form, "empty", empty))
        testing_t_errorf_v(t, "req.Form[\"empty\"] wrong, want [\"\", \"not\"]");
    static const char *const nokey[] = {"nokey", NULL};
    if (!all_equal(req->form, "", nokey))
        testing_t_errorf_v(t, "req.Form[\"\"] wrong, want [\"nokey\"]");
    http_request_free(req);
    arena_free(&ar);
}

/* Tests that we only parse the form automatically for certain methods. */
static void TestParseFormQueryMethods(TestingT *t) {
    static const char *const methods[] = {"POST", "PATCH", "PUT", "FOO"};
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        HttpRequest *req =
            new_request(a, methods[i], "http://www.google.com/search", "foo=bar");
        if (req == NULL) {
            arena_free(&ar);
            FATALF("NewRequest failed");
        }
        (void)http_header_set(req->header, cs("Content-Type"),
                              cs("application/x-www-form-urlencoded; param=value"));
        Str want = cs("bar");
        if (strcmp(methods[i], "FOO") == 0)
            want = BURROW_STR_EMPTY;
        Str got = http_request_form_value(req, cs("foo"));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "for method %s, FormValue(\"foo\") = %q; want %q",
                               cs(methods[i]), got, want);
        http_request_free(req);
        arena_free(&ar);
    }
}

static void TestParseFormUnknownContentType(TestingT *t) {
    static const struct {
        const char *name;
        const char *want_err;
        const char *content_type;
    } tests[] = {
        {"text", "", "text/plain"},
        /* Empty content type is legal - may be treated as
         * application/octet-stream (RFC 7231, section 3.1.1.5) */
        {"empty", "", NULL},
        {"boundary", "mime: invalid media parameter", "text/plain; boundary="},
        {"unknown", "", "application/unknown"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        HttpRequest req;
        hand_request(&req, a, "POST", tests[i].content_type, body_of(a, cs("body")));
        Error err = http_request_parse_form(&req);
        Str want = cs(tests[i].want_err);
        if (BURROW_OK(err) && want.len != 0)
            testing_t_errorf_v(t, "%s: unexpected success; want error %q",
                               cs(tests[i].name), want);
        else if (BURROW_FAILED(err) && want.len == 0)
            testing_t_errorf_v(t, "%s: want success, got error: %v", cs(tests[i].name),
                               err);
        else if (want.len != 0 && !str_eq(want, error_text(err)))
            testing_t_errorf_v(t, "%s: got error %q; want %q", cs(tests[i].name),
                               error_text(err), want);
        arena_free(&req.arena);
        arena_free(&ar);
    }
}

static void TestParseFormInitializeOnError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *nil_body =
        new_request(a, "POST", "http://www.google.com/search?q=foo", NULL);
    HttpRequest by_hand;
    memset(&by_hand, 0, sizeof by_hand);
    by_hand.method = cs("GET");
    HttpRequest *tests[] = {nil_body, &by_hand};
    for (int i = 0; i < 2; i++) {
        HttpRequest *req = tests[i];
        if (req == NULL) {
            testing_t_errorf_v(t, "%d. NewRequest failed", i);
            continue;
        }
        Error err = http_request_parse_form(req);
        if (req->form == NULL)
            testing_t_errorf_v(t, "%d. Form not initialized, error %v", i, err);
        if (req->post_form == NULL)
            testing_t_errorf_v(t, "%d. PostForm not initialized, error %v", i, err);
    }
    http_request_free(nil_body);
    arena_free(&by_hand.arena);
    arena_free(&ar);
}

static void TestMultipartReader(TestingT *t) {
    static const struct {
        bool should_error;
        const char *content_type;
    } tests[] = {
        {false, "multipart/form-data; boundary=\"foo123\""},
        {false, "multipart/mixed; boundary=\"foo123\""},
        {true, "text/plain"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        HttpRequest req;
        hand_request(&req, a, "POST", tests[i].content_type, body_of(a, cs("")));
        Error err;
        MultipartReader *multipart = http_request_multipart_reader(&req, &err);
        if (tests[i].should_error) {
            if (BURROW_OK(err) || multipart != NULL)
                testing_t_errorf_v(t,
                                   "test %d: unexpectedly got nil-error or "
                                   "non-nil-multipart",
                                   (int)i);
        } else if (BURROW_FAILED(err) || multipart == NULL) {
            testing_t_errorf_v(t,
                               "test %d: unexpectedly got error (%v) or nil-multipart",
                               (int)i, err);
        }
        arena_free(&req.arena);
        arena_free(&ar);
    }
}

/* Issue 9305: ParseMultipartForm should populate PostForm too */
static void TestParseMultipartFormPopulatesPostForm(TestingT *t) {
    const char *post_data = "--xxx\n"
                            "Content-Disposition: form-data; name=\"field1\"\n"
                            "\n"
                            "value1\n"
                            "--xxx\n"
                            "Content-Disposition: form-data; name=\"field2\"\n"
                            "\n"
                            "value2\n"
                            "--xxx\n"
                            "Content-Disposition: form-data; name=\"file\"; "
                            "filename=\"file\"\n"
                            "Content-Type: application/octet-stream\n"
                            "Content-Transfer-Encoding: binary\n"
                            "\n"
                            "binary data\n"
                            "--xxx--\n";
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest req;
    hand_request(&req, a, "POST", "multipart/form-data; boundary=xxx",
                 body_of(a, cs(post_data)));

    static const char *const initial[] = {
        "language", "Go",     "name",           "gopher", "skill",
        "go-ing",   "field2", "initial-value2", NULL};
    req.form = values_of(a, initial);

    Error err = http_request_parse_multipart_form(&req, 10000);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected multipart error %v", err);
    } else {
        static const char *const want_form[] = {
            "language", "Go",     "name",   "gopher", "skill",
            "go-ing",   "field1", "value1", "field2", "initial-value2",
            "field2",   "value2", NULL};
        if (!values_equal(a, req.form, values_of(a, want_form)))
            testing_t_errorf_v(t, "req.Form = %s", url_values_encode(req.form, a));
        static const char *const want_post[] = {"field1", "value1", "field2", "value2",
                                                NULL};
        if (!values_equal(a, req.post_form, values_of(a, want_post)))
            testing_t_errorf_v(t, "req.PostForm = %s",
                               url_values_encode(req.post_form, a));
    }
    if (req.multipart_form != NULL)
        (void)multipart_form_remove_all(req.multipart_form);
    arena_free(&req.arena);
    arena_free(&ar);
}

static void TestParseMultipartForm(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest req;
    hand_request(&req, a, "POST", "multipart/form-data; boundary=\"foo123\"",
                 body_of(a, cs("")));
    Error err = http_request_parse_multipart_form(&req, 25);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected multipart EOF, got nil");

    req.header = http_header_make(a);
    (void)http_header_set(req.header, cs("Content-Type"), cs("text/plain"));
    err = http_request_parse_multipart_form(&req, 25);
    if (!same_error(err, http_err_not_multipart))
        testing_t_errorf_v(t, "expected ErrNotMultipart for text/plain");
    arena_free(&req.arena);
    arena_free(&ar);
}

/* Issue 45789: multipart form should not include directory path in filename */
static void TestParseMultipartFormFilename(TestingT *t) {
    const char *post_data = "--xxx\n"
                            "Content-Disposition: form-data; name=\"file\"; "
                            "filename=\"../usr/foobar.txt/\"\n"
                            "Content-Type: text/plain\n"
                            "\n"
                            "--xxx--\n";
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest req;
    hand_request(&req, a, "POST", "multipart/form-data; boundary=xxx",
                 body_of(a, cs(post_data)));
    MultipartFileHeader *hdr = NULL;
    Error err;
    MultipartFile *f = http_request_form_file(&req, cs("file"), &hdr, &err);
    if (BURROW_FAILED(err) || hdr == NULL)
        testing_t_errorf_v(t, "%v", err);
    else if (!str_eq(hdr->filename, cs("foobar.txt")))
        testing_t_errorf_v(t, "expected only the last element of the path, got %q",
                           hdr->filename);
    if (f != NULL)
        (void)multipart_file_close(f);
    if (req.multipart_form != NULL)
        (void)multipart_form_remove_all(req.multipart_form);
    arena_free(&req.arena);
    arena_free(&ar);
}

/* The handler of TestMaxInt64ForMultipartFormMaxMemoryOverflow. */
static void serve_max_int64(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    /* The combination of:
     *      MaxInt64 + payloadSize + (internal spare of 10MiB)
     * triggers the overflow. See issue https://golang.org/issue/40430/ */
    Error err = http_request_parse_multipart_form(r, INT64_MAX);
    if (BURROW_FAILED(err))
        http_error(w, error_text(err), HTTP_STATUS_BAD_REQUEST);
}

/* Issue #40430: Test that if maxMemory for ParseMultipartForm when combined with
 * the payload size and the internal leeway buffer size of 10MiB overflows, that we
 * correctly return an error. */
static void TestMaxInt64ForMultipartFormMaxMemoryOverflow(TestingT *t) {
    need_tcp(t);
    Int payload_size = 1 << 10;
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_max_int64, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    if (ts == NULL)
        FATALF("httptest_new_server failed");
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer fbuf = BYTES_BUFFER(a);
    MultipartWriter *mw = multipart_new_writer(a, bytes_buffer_as_io_writer(&fbuf));
    Error err;
    IoWriter mf =
        multipart_writer_create_form_file(mw, cs("file"), cs("myfile.txt"), &err);
    HttpRequest *req = NULL;
    HttpResponse *res = NULL;
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    Slice payload = bytes_repeat(
        a, slice_from((void *)(uintptr_t)"abc", 3, 3, TYPE_BYTE), payload_size);
    (void)mf.vt->write(mf.data, payload, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    err = multipart_writer_close(mw);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    req = http_new_request(heap_allocator(), cs("POST"), ts->url,
                           bytes_buffer_as_io_reader(&fbuf), &err);
    if (req == NULL) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    (void)http_header_set(req->header, cs("Content-Type"),
                          multipart_writer_form_data_content_type(mw, a));
    res = http_client_do(httptest_server_client(ts), req, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    (void)res->body.vt->closer.close(res->body.data);
    if (res->status_code != HTTP_STATUS_OK)
        testing_t_errorf_v(t, "Status code mismatch: got %d, want %d",
                           (int)res->status_code, HTTP_STATUS_OK);
done:
    http_response_free(res);
    http_request_free(req);
    multipart_writer_free(mw);
    arena_free(&ar);
    httptest_server_free(ts);
}

/* ------------------------------------------------------- multipart requests */

#define FILEA_CONTENTS "This is a test file."
#define FILEB_CONTENTS "Another test file."
#define TEXTA_VALUE "foo"
#define TEXTB_VALUE "bar"

static const char message[] =
    "\r\n"
    "--MyBoundary\r\n"
    "Content-Disposition: form-data; name=\"filea\"; filename=\"filea.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n" FILEA_CONTENTS "\r\n"
    "--MyBoundary\r\n"
    "Content-Disposition: form-data; name=\"fileb\"; filename=\"fileb.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n" FILEB_CONTENTS "\r\n"
    "--MyBoundary\r\n"
    "Content-Disposition: form-data; name=\"texta\"\r\n"
    "\r\n" TEXTA_VALUE "\r\n"
    "--MyBoundary\r\n"
    "Content-Disposition: form-data; name=\"textb\"\r\n"
    "\r\n" TEXTB_VALUE "\r\n"
    "--MyBoundary--\r\n";

/* newTestMultipartRequest. Free it with free_multipart_request. */
static HttpRequest *new_test_multipart_request(Alloc *a) {
    HttpRequest *req = new_request(a, "POST", "/", message);
    if (req == NULL)
        return NULL;
    (void)http_header_set(req->header, cs("Content-type"),
                          cs("multipart/form-data; boundary=\"MyBoundary\""));
    return req;
}

static void free_multipart_request(HttpRequest *req) {
    if (req != NULL && req->multipart_form != NULL)
        (void)multipart_form_remove_all(req->multipart_form);
    http_request_free(req);
}

static void test_missing_file(TestingT *t, HttpRequest *req) {
    MultipartFileHeader *fh = NULL;
    Error err;
    MultipartFile *f = http_request_form_file(req, cs("missing"), &fh, &err);
    if (f != NULL) {
        testing_t_errorf_v(t, "FormFile file = %p, want nil", (void *)f);
        (void)multipart_file_close(f);
    }
    if (fh != NULL)
        testing_t_errorf_v(t, "FormFile file header = %p, want nil", (void *)fh);
    if (!same_error(err, http_err_missing_file))
        testing_t_errorf_v(t, "FormFile err = %q, want ErrMissingFile",
                           error_text(err));
}

/* testMultipartFile. The file's header, or NULL after a failure. */
static MultipartFileHeader *test_multipart_file(TestingT *t, Alloc *a, HttpRequest *req,
                                                const char *key,
                                                const char *expect_filename,
                                                const char *expect_content) {
    MultipartFileHeader *fh = NULL;
    Error err;
    MultipartFile *f = http_request_form_file(req, cs(key), &fh, &err);
    if (f == NULL || fh == NULL) {
        testing_t_errorf_v(t, "FormFile(%q): %q", cs(key), error_text(err));
        return NULL;
    }
    if (!str_eq(fh->filename, cs(expect_filename)))
        testing_t_errorf_v(t, "filename = %q, want %q", fh->filename,
                           cs(expect_filename));
    Slice b = io_read_all(a, multipart_file_as_io_reader(f), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "copying contents: %v", err);
    else if (!str_eq(str_from_bytes(b.p, b.len), cs(expect_content)))
        testing_t_errorf_v(t, "contents = %q, want %q", str_from_bytes(b.p, b.len),
                           cs(expect_content));
    (void)multipart_file_close(f);
    return fh;
}

static void validate_test_multipart_contents(TestingT *t, Alloc *a, HttpRequest *req,
                                             bool all_mem) {
    Str g = http_request_form_value(req, cs("texta"));
    if (!str_eq(g, cs(TEXTA_VALUE)))
        testing_t_errorf_v(t, "texta value = %q, want %q", g, cs(TEXTA_VALUE));
    g = http_request_form_value(req, cs("textb"));
    if (!str_eq(g, cs(TEXTB_VALUE)))
        testing_t_errorf_v(t, "textb value = %q, want %q", g, cs(TEXTB_VALUE));
    g = http_request_form_value(req, cs("missing"));
    if (g.len != 0)
        testing_t_errorf_v(t, "missing value = %q, want empty string", g);

    MultipartFileHeader *fha =
        test_multipart_file(t, a, req, "filea", "filea.txt", FILEA_CONTENTS);
    if (fha != NULL && fha->tmpfile != NULL)
        testing_t_errorf_v(t, "filea is *os.File, should not be");
    MultipartFileHeader *fhb =
        test_multipart_file(t, a, req, "fileb", "fileb.txt", FILEB_CONTENTS);
    if (fhb != NULL) {
        if (all_mem && fhb->tmpfile != NULL)
            testing_t_errorf_v(t, "fileb is *os.File, should not be");
        else if (!all_mem && fhb->tmpfile == NULL)
            testing_t_errorf_v(t, "fileb has unexpected underlying type, in memory");
    }

    test_missing_file(t, req);
}

static void TestMultipartRequest(TestingT *t) {
    /* Test that we can read the values and files of a multipart request with
     * FormValue and FormFile, and that ParseMultipartForm can be called
     * multiple times. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_test_multipart_request(a);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err = http_request_parse_multipart_form(req, 25);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ParseMultipartForm first call: %v", err);
    } else {
        validate_test_multipart_contents(t, a, req, false);
        err = http_request_parse_multipart_form(req, 25);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ParseMultipartForm second call: %v", err);
        else
            validate_test_multipart_contents(t, a, req, false);
    }
    free_multipart_request(req);
    arena_free(&ar);
}

/* Issue #25192: Test that ParseMultipartForm fails but still parses the
 * multi-part form when a URL containing a semicolon is provided. */
static void TestParseMultipartFormSemicolonSeparator(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_test_multipart_request(a);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err;
    req->url = url_parse(a, cs("?q=foo;q=bar"), &err);
    if (BURROW_OK(http_request_parse_multipart_form(req, 25)))
        testing_t_errorf_v(t, "ParseMultipartForm expected error due to invalid "
                              "semicolon, got nil");
    else
        validate_test_multipart_contents(t, a, req, false);
    free_multipart_request(req);
    arena_free(&ar);
}

static void TestMultipartRequestAuto(TestingT *t) {
    /* Test that FormValue and FormFile automatically invoke ParseMultipartForm
     * and return the right values. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_test_multipart_request(a);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    validate_test_multipart_contents(t, a, req, true);
    free_multipart_request(req);
    arena_free(&ar);
}

static void TestMissingFileMultipartRequest(TestingT *t) {
    /* Test that FormFile returns an error if the named file is missing. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req = new_test_multipart_request(arena_allocator(&ar));
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    test_missing_file(t, req);
    free_multipart_request(req);
    arena_free(&ar);
}

/* Test that FormValue invokes ParseMultipartForm. */
static void TestFormValueCallsParseMultipartForm(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req =
        new_request(arena_allocator(&ar), "POST", "http://www.google.com/", "z=post");
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    (void)http_header_set(req->header, cs("Content-Type"),
                          cs("application/x-www-form-urlencoded; param=value"));
    if (req->form != NULL)
        testing_t_errorf_v(t, "Unexpected request Form, want nil");
    (void)http_request_form_value(req, cs("z"));
    if (req->form == NULL)
        testing_t_errorf_v(t, "ParseMultipartForm not called by FormValue");
    http_request_free(req);
    arena_free(&ar);
}

/* Test that FormFile invokes ParseMultipartForm. */
static void TestFormFileCallsParseMultipartForm(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req = new_test_multipart_request(arena_allocator(&ar));
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    if (req->form != NULL)
        testing_t_errorf_v(t, "Unexpected request Form, want nil");
    Error err;
    MultipartFile *f = http_request_form_file(req, BURROW_STR_EMPTY, NULL, &err);
    if (f != NULL)
        (void)multipart_file_close(f);
    if (req->form == NULL)
        testing_t_errorf_v(t, "ParseMultipartForm not called by FormFile");
    free_multipart_request(req);
    arena_free(&ar);
}

/* Test that ParseMultipartForm errors if called after MultipartReader on the
 * same request. */
static void TestParseMultipartFormOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req = new_test_multipart_request(arena_allocator(&ar));
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err;
    if (http_request_multipart_reader(req, &err) == NULL)
        testing_t_errorf_v(t, "MultipartReader: %v", err);
    else if (BURROW_OK(http_request_parse_multipart_form(req, 1024)))
        testing_t_errorf_v(t, "expected an error from ParseMultipartForm after call to "
                              "MultipartReader");
    http_request_free(req);
    arena_free(&ar);
}

/* Test that MultipartReader errors if called after ParseMultipartForm on the
 * same request. */
static void TestMultipartReaderOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req = new_test_multipart_request(arena_allocator(&ar));
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err = http_request_parse_multipart_form(req, 25);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseMultipartForm: %v", err);
    else if (http_request_multipart_reader(req, &err) != NULL || BURROW_OK(err))
        testing_t_errorf_v(t, "expected an error from MultipartReader after call to "
                              "ParseMultipartForm");
    free_multipart_request(req);
    arena_free(&ar);
}

/* Test that FormFile errors if called after MultipartReader on the same
 * request. */
static void TestFormFileOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req = new_test_multipart_request(arena_allocator(&ar));
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err;
    if (http_request_multipart_reader(req, &err) == NULL) {
        testing_t_errorf_v(t, "MultipartReader: %v", err);
    } else {
        MultipartFile *f = http_request_form_file(req, BURROW_STR_EMPTY, NULL, &err);
        if (f != NULL)
            (void)multipart_file_close(f);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "expected an error from FormFile after call to "
                                  "MultipartReader");
    }
    http_request_free(req);
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Clone */

static void TestWithContextNilURL(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpRequest *req =
        new_request(arena_allocator(&ar), "POST", "https://golang.org/", NULL);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    /* Issue 20601 */
    req->url = NULL;
    HttpRequest *req_copy =
        http_request_with_context(req, heap_allocator(), context_background());
    if (req_copy == NULL || req_copy->url != NULL)
        testing_t_errorf_v(t, "expected nil URL in cloned request");
    http_request_free(req_copy);
    http_request_free(req);
    arena_free(&ar);
}

/* Ensure that Request.Clone creates a deep copy of TransferEncoding. See issue
 * 41907. */
static void TestRequestCloneTransferEncoding(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_request(a, "POST", "https://example.org/", "body");
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Str *te = (Str *)mem_alloc(a, sizeof(Str), _Alignof(Str));
    if (te == NULL) {
        http_request_free(req);
        arena_free(&ar);
        FATALF("out of memory");
    }
    te[0] = cs("encoding1");
    req->transfer_encoding = slice_from(te, 1, 1, TYPE_STRING);

    HttpRequest *cloned =
        http_request_clone(req, heap_allocator(), context_background());
    if (cloned == NULL) {
        http_request_free(req);
        arena_free(&ar);
        FATALF("Clone failed");
    }
    /* modify original after deep copy */
    te[0] = cs("encoding2");

    if (!str_eq(((const Str *)req->transfer_encoding.p)[0], cs("encoding2")))
        testing_t_errorf_v(t, "expected req.TransferEncoding to be changed");
    if (!str_eq(((const Str *)cloned->transfer_encoding.p)[0], cs("encoding1")))
        testing_t_errorf_v(t, "expected clonedReq.TransferEncoding to be unchanged");
    http_request_free(cloned);
    http_request_free(req);
    arena_free(&ar);
}

/* Ensure that Request.Clone works correctly with PathValue. See issue 64911. */
static void TestRequestClonePathValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_request(a, "GET", "https://example.org/", NULL);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    (void)http_request_set_path_value(req, a, cs("p1"), cs("orig"));

    HttpRequest *cloned =
        http_request_clone(req, heap_allocator(), context_background());
    if (cloned == NULL) {
        http_request_free(req);
        arena_free(&ar);
        FATALF("Clone failed");
    }
    (void)http_request_set_path_value(cloned, a, cs("p2"), cs("copy"));

    /* Ensure that any modifications to the cloned request do not pollute the
     * original request. */
    Str g = http_request_path_value(req, cs("p2"));
    if (g.len != 0)
        testing_t_errorf_v(t, "p2 mismatch got %q, want %q", g, cs(""));
    g = http_request_path_value(req, cs("p1"));
    if (!str_eq(g, cs("orig")))
        testing_t_errorf_v(t, "p1 mismatch got %q, want %q", g, cs("orig"));

    /* Assert on the changes to the cloned request. */
    g = http_request_path_value(cloned, cs("p1"));
    if (!str_eq(g, cs("orig")))
        testing_t_errorf_v(t, "p1 mismatch got %q, want %q", g, cs("orig"));
    g = http_request_path_value(cloned, cs("p2"));
    if (!str_eq(g, cs("copy")))
        testing_t_errorf_v(t, "p2 mismatch got %q, want %q", g, cs("copy"));
    http_request_free(cloned);
    http_request_free(req);
    arena_free(&ar);
}

/* Not in Go: Clone copies the forms, the header and the URL, and a change to
 * one of them in the copy leaves the original as it was. */
static void TestRequestCloneForms(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = new_test_multipart_request(a);
    if (req == NULL) {
        arena_free(&ar);
        FATALF("NewRequest failed");
    }
    Error err = http_request_parse_multipart_form(req, 1 << 20);
    if (BURROW_FAILED(err)) {
        free_multipart_request(req);
        arena_free(&ar);
        FATALF("ParseMultipartForm: %v", err);
    }
    HttpRequest *cloned =
        http_request_clone(req, heap_allocator(), context_background());
    if (cloned == NULL) {
        free_multipart_request(req);
        arena_free(&ar);
        FATALF("Clone failed");
    }
    (void)url_values_set(cloned->form, cs("texta"), cs("changed"));
    (void)http_header_set(cloned->header, cs("X-Clone"), cs("yes"));
    cloned->url->path = cs("/changed");

    Str g = http_request_form_value(req, cs("texta"));
    if (!str_eq(g, cs(TEXTA_VALUE)))
        testing_t_errorf_v(t, "original texta = %q, want %q", g, cs(TEXTA_VALUE));
    g = http_request_form_value(cloned, cs("texta"));
    if (!str_eq(g, cs("changed")))
        testing_t_errorf_v(t, "cloned texta = %q, want %q", g, cs("changed"));
    g = http_header_get(req->header, cs("X-Clone"));
    if (g.len != 0)
        testing_t_errorf_v(t, "original X-Clone = %q, want \"\"", g);
    if (!str_eq(req->url->path, cs("/")))
        testing_t_errorf_v(t, "original path = %q, want \"/\"", req->url->path);
    if (cloned->multipart_form == NULL || cloned->multipart_form == req->multipart_form)
        testing_t_errorf_v(t, "MultipartForm not copied");
    else
        (void)test_multipart_file(t, a, cloned, "filea", "filea.txt", FILEA_CONTENTS);
    http_request_free(cloned);
    free_multipart_request(req);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ errors */

static void TestErrNotSupported(TestingT *t) {
    if (!errors_is(http_err_not_supported, errors_err_unsupported))
        testing_t_errorf_v(t,
                           "errors.Is(ErrNotSupported, errors.ErrUnsupported) failed");
}

/* Not in Go: the protocol errors are ProtocolErrors, with their text in
 * ErrorString, and the ones that are not still have the text Go gives them. */
static void TestProtocolErrors(TestingT *t) {
    static const struct {
        const Error *err;
        const char *text;
    } tests[] = {
        {&http_err_not_supported, "feature not supported"},
        {&http_err_unexpected_trailer,
         "trailer header without chunked transfer encoding"},
        {&http_err_missing_boundary, "no multipart boundary param in Content-Type"},
        {&http_err_not_multipart, "request Content-Type isn't multipart/form-data"},
        {&http_err_header_too_long, "header too long"},
        {&http_err_short_body, "entity body too short"},
        {&http_err_missing_content_length, "missing ContentLength in HEAD response"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const HttpProtocolError *pe = (const HttpProtocolError *)errors_as(
            *tests[i].err, TYPE_HTTP_PROTOCOL_ERROR);
        if (pe == NULL) {
            testing_t_errorf_v(t, "%q is not a ProtocolError", cs(tests[i].text));
            continue;
        }
        if (!str_eq(http_protocol_error_error(pe), cs(tests[i].text)))
            testing_t_errorf_v(t, "ErrorString = %q, want %q",
                               http_protocol_error_error(pe), cs(tests[i].text));
        if (!str_eq(error_text(*tests[i].err), cs(tests[i].text)))
            testing_t_errorf_v(t, "Error() = %q, want %q", error_text(*tests[i].err),
                               cs(tests[i].text));
        bool is = http_protocol_error_is(pe, errors_err_unsupported);
        if (is != (i == 0))
            testing_t_errorf_v(t, "%q: Is(ErrUnsupported) = %t", cs(tests[i].text), is);
    }
    if (!str_eq(error_text(http_err_missing_file), cs("http: no such file")))
        testing_t_errorf_v(t, "ErrMissingFile = %q", error_text(http_err_missing_file));
    if (!str_eq(error_text(http_err_write_after_flush), cs("unused")))
        testing_t_errorf_v(t, "ErrWriteAfterFlush = %q",
                           error_text(http_err_write_after_flush));
    if (!errors_is(http_err_line_too_long, burrow__http_err_line_too_long))
        testing_t_errorf_v(t, "ErrLineTooLong is not internal.ErrLineTooLong");
    HttpMaxBytesError mbe = {.limit = 1};
    if (!str_eq(http_max_bytes_error_error(&mbe), cs("http: request body too large")))
        testing_t_errorf_v(t, "MaxBytesError.Error() = %q",
                           http_max_bytes_error_error(&mbe));
}

#define TESTS(X)                                                                       \
    X(TestQuery)                                                                       \
    X(TestParseFormSemicolonSeparator)                                                 \
    X(TestParseFormQuery)                                                              \
    X(TestParseFormQueryMethods)                                                       \
    X(TestParseFormUnknownContentType)                                                 \
    X(TestParseFormInitializeOnError)                                                  \
    X(TestMultipartReader)                                                             \
    X(TestParseMultipartFormPopulatesPostForm)                                         \
    X(TestParseMultipartForm)                                                          \
    X(TestParseMultipartFormFilename)                                                  \
    X(TestMaxInt64ForMultipartFormMaxMemoryOverflow)                                   \
    X(TestMultipartRequest)                                                            \
    X(TestParseMultipartFormSemicolonSeparator)                                        \
    X(TestMultipartRequestAuto)                                                        \
    X(TestMissingFileMultipartRequest)                                                 \
    X(TestFormValueCallsParseMultipartForm)                                            \
    X(TestFormFileCallsParseMultipartForm)                                             \
    X(TestParseMultipartFormOrder)                                                     \
    X(TestMultipartReaderOrder)                                                        \
    X(TestFormFileOrder)                                                               \
    X(TestWithContextNilURL)                                                           \
    X(TestRequestCloneTransferEncoding)                                                \
    X(TestRequestClonePathValue)                                                       \
    X(TestRequestCloneForms)                                                           \
    X(TestErrNotSupported)                                                             \
    X(TestProtocolErrors)

TESTING_MAIN(TESTS)
