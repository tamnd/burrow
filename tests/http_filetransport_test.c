/* Derived from Go's src/net/http/filetransport_test.go.
 * Go source: go1.27.3.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/testing.h"
#include "burrow/testing/fstest.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

static Str cs(const char *s) {
    return str_from_cstr(s);
}

/* checker. Fails the test and says false when err is not nil. */
static bool check(TestingT *t, Str call, Error err) {
    if (BURROW_OK(err))
        return true;
    testing_t_fatalf_v(t, "%s: %v", call, err);
    return false;
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str get_call(Alloc *a, Str urlstr) {
    return fmt_sprintf_v(a, "Get %s", urlstr);
}

static Str read_all_call(Alloc *a, Str urlstr) {
    return fmt_sprintf_v(a, "ReadAll %s", urlstr);
}

static void TestFileTransport(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Not in Go: os_mkdir_temp and os_remove_all in place of t.TempDir. */
    Error err = BURROW_NO_ERROR;
    Str dname = os_mkdir_temp(a, S(""), S("burrow-filetransport-test-*"), &err);
    if (!check(t, S("MkdirTemp"), err)) {
        arena_free(&ar);
        return;
    }
    Str fname = filepath_join_v(a, 2, dname, S("foo.txt"));
    err = os_write_file(fname, bytes_of(S("Bar")), 0644);
    if (!check(t, S("WriteFile"), err))
        goto out;

    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    HttpDir dir = dname;
    (void)http_transport_register_protocol(
        &tr, S("file"), http_new_file_transport(a, http_dir_as_file_system(&dir)));
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = http_transport_as_round_tripper(&tr);

    static const char *const foo_urls[] = {"file:///foo.txt", "file://../foo.txt"};
    for (size_t i = 0; i < sizeof foo_urls / sizeof foo_urls[0]; i++) {
        const char *urlstr = foo_urls[i];
        HttpResponse *res = http_client_get(&c, cs(urlstr), &err);
        if (!check(t, get_call(a, cs(urlstr)), err))
            goto done;
        if (res->status_code != 200)
            testing_t_errorf_v(t, "for %s, StatusCode = %d, want 200", cs(urlstr),
                               res->status_code);
        if (res->content_length != -1)
            testing_t_errorf_v(t, "for %s, ContentLength = %d, want -1", cs(urlstr),
                               res->content_length);
        if (res->body.vt == NULL) {
            testing_t_fatalf_v(t, "for %s, nil Body", cs(urlstr));
            http_response_free(res);
            goto done;
        }
        Slice slurp = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        (void)res->body.vt->closer.close(res->body.data);
        if (!check(t, read_all_call(a, cs(urlstr)), err)) {
            http_response_free(res);
            goto done;
        }
        Str got = str_from_bytes(slurp.p, slurp.len);
        if (!str_eq(got, S("Bar")))
            testing_t_errorf_v(t, "for %s, got content %q, want %q", cs(urlstr), got,
                               S("Bar"));
        Str u = url_string(res->request->url, a);
        if (!str_eq(u, cs(urlstr)))
            testing_t_errorf_v(t, "for %s, Response.Request.URL = %s, want = %s",
                               cs(urlstr), u, cs(urlstr));
        http_response_free(res);
    }

    {
        static const char bad_url[] = "file://../no-exist.txt";
        HttpResponse *res = http_client_get(&c, cs(bad_url), &err);
        if (!check(t, get_call(a, cs(bad_url)), err))
            goto done;
        if (res->status_code != 404)
            testing_t_errorf_v(t, "for %s, StatusCode = %d, want 404", cs(bad_url),
                               res->status_code);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
    }

done:
    http_transport_free(&tr);
    (void)os_remove(fname);
out:
    (void)os_remove_all(dname);
    arena_free(&ar);
}

static void TestFileTransportFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    FstestMapFile index_html;
    memset(&index_html, 0, sizeof index_html);
    index_html.data = bytes_of(S("index.html says hello"));
    FstestMapFS fsys = fstest_map_fs_make(a);
    (void)fstest_map_fs_set(fsys, S("index.html"), &index_html);

    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    (void)http_transport_register_protocol(
        &tr, S("file"), http_new_file_transport_fs(a, fstest_map_fs_as_fs(fsys)));
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = http_transport_as_round_tripper(&tr);

    /* Not in Go: fsys has the one file, so this is the range over it. */
    static const char *const fnames[] = {"index.html"};
    const FstestMapFile *mfiles[] = {&index_html};
    for (size_t i = 0; i < sizeof fnames / sizeof fnames[0]; i++) {
        Str urlstr_s = fmt_sprintf_v(a, "file:///%s", cs(fnames[i]));
        HttpResponse *res = http_client_get(&c, urlstr_s, &err);
        if (!check(t, get_call(a, urlstr_s), err))
            goto done;
        if (res->status_code != 200)
            testing_t_errorf_v(t, "for %s, StatusCode = %d, want 200", urlstr_s,
                               res->status_code);
        if (res->content_length != -1)
            testing_t_errorf_v(t, "for %s, ContentLength = %d, want -1", urlstr_s,
                               res->content_length);
        if (res->body.vt == NULL) {
            testing_t_fatalf_v(t, "for %s, nil Body", urlstr_s);
            http_response_free(res);
            goto done;
        }
        Slice slurp = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        (void)res->body.vt->closer.close(res->body.data);
        if (!check(t, read_all_call(a, urlstr_s), err)) {
            http_response_free(res);
            goto done;
        }
        Str got = str_from_bytes(slurp.p, slurp.len);
        Str want = str_from_bytes(mfiles[i]->data.p, mfiles[i]->data.len);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "for %s, got content %q, want %q", urlstr_s, got,
                               S("Bar"));
        http_response_free(res);
    }

    {
        static const char bad_url[] = "file://../no-exist.txt";
        HttpResponse *res = http_client_get(&c, cs(bad_url), &err);
        if (!check(t, get_call(a, cs(bad_url)), err))
            goto done;
        if (res->status_code != 404)
            testing_t_errorf_v(t, "for %s, StatusCode = %d, want 404", cs(bad_url),
                               res->status_code);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
    }

done:
    http_transport_free(&tr);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFileTransport)                                                               \
    X(TestFileTransportFS)

TESTING_MAIN(TESTS)
