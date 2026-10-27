/* Derived from Go's src/net/http/filetransport.go.
 * Go source: go1.27.3.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http.h"

#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* fileTransport implements RoundTripper for the 'file' protocol. */
typedef struct ft_FileTransport {
    HttpHandler fh;
} ft_FileTransport;

static HttpResponse *ft_round_trip(void *self, HttpRequest *req, Error *err);

static const HttpRoundTripperVT ft_file_transport_vt = {NULL, ft_round_trip};

HttpRoundTripper http_new_file_transport(Alloc *a, HttpFileSystem fs) {
    ft_FileTransport *t =
        (ft_FileTransport *)mem_alloc(a, sizeof *t, _Alignof(ft_FileTransport));
    if (t == NULL)
        return (HttpRoundTripper){&ft_file_transport_vt, NULL};
    /* Not in Go: http_file_server makes the fileHandler{fs}. */
    t->fh = http_file_server(a, fs);
    if (t->fh.data == NULL) {
        mem_free(a, t, sizeof *t, _Alignof(ft_FileTransport));
        return (HttpRoundTripper){&ft_file_transport_vt, NULL};
    }
    return (HttpRoundTripper){&ft_file_transport_vt, t};
}

HttpRoundTripper http_new_file_transport_fs(Alloc *a, Fs fsys) {
    HttpFileSystem fs = http_fs(a, fsys);
    if (fs.data == NULL)
        return (HttpRoundTripper){&ft_file_transport_vt, NULL};
    return http_new_file_transport(a, fs);
}

/* populateResponse is a ResponseWriter that populates the *Response
 * in res, and writes its body to a pipe connected to the response
 * body. Once writes begin or finish() is called, the response is sent
 * on ch. */
typedef struct ft_PopulateResponse {
    HttpResponse *res;
    Chan *ch; /* bool, for the *Response in res */
    bool wrote_header;
    bool has_content;
    bool sent_response;
    IoPipeWriter *pw;

    /* Not in Go: the reader end, to free the pipe with, what the goroutine
     * serves, and done, closed once the goroutine has finished with all of
     * it. */
    IoPipeReader *pr;
    HttpHandler fh;
    HttpRequest *req;
    Chan *done;
} ft_PopulateResponse;

static void ft_pr_write_header(void *self, Int code);

static void ft_pr_send_response(ft_PopulateResponse *pr) {
    if (pr->sent_response)
        return;
    pr->sent_response = true;

    if (pr->has_content)
        pr->res->content_length = -1;
    bool yes = true;
    chan_send(pr->ch, &yes);
}

static void ft_pr_finish(ft_PopulateResponse *pr) {
    if (!pr->wrote_header)
        ft_pr_write_header(pr, 500);
    if (!pr->sent_response)
        ft_pr_send_response(pr);
    (void)io_pipe_writer_close(pr->pw);
}

static HttpHeader ft_pr_header(void *self) {
    ft_PopulateResponse *pr = (ft_PopulateResponse *)self;
    return pr->res->header;
}

static void ft_pr_write_header(void *self, Int code) {
    ft_PopulateResponse *pr = (ft_PopulateResponse *)self;
    if (pr->wrote_header)
        return;
    pr->wrote_header = true;

    pr->res->status_code = code;
    pr->res->status = fmt_sprintf_v(arena_allocator(&pr->res->arena), "%d %s", code,
                                    http_status_text(code));
}

static Int ft_pr_write(void *self, Slice p, Error *err) {
    ft_PopulateResponse *pr = (ft_PopulateResponse *)self;
    if (!pr->wrote_header)
        ft_pr_write_header(pr, HTTP_STATUS_OK);
    pr->has_content = true;
    if (!pr->sent_response)
        ft_pr_send_response(pr);
    return io_pipe_writer_write(pr->pw, p, err);
}

static const HttpResponseWriterVT ft_populate_response_vt = {
    {NULL, ft_pr_write},
    ft_pr_header,
    ft_pr_write_header,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* Not in Go, where the collector frees all of it: what http_response_free
 * runs. Closing the body makes the handler's writes fail, so the goroutine
 * finishes, and once it has, nothing else uses the pipe or the channels. */
static void ft_pr_free(void *env) {
    ft_PopulateResponse *pr = (ft_PopulateResponse *)env;
    (void)io_pipe_reader_close(pr->pr);
    bool v;
    (void)chan_recv(pr->done, &v);
    io_pipe_free(pr->pr);
    chan_free(pr->ch);
    chan_free(pr->done);
    mem_free(heap_allocator(), pr, sizeof *pr, _Alignof(ft_PopulateResponse));
}

/* Not in Go: gives back what ft_new_populate_response_writer made, when the
 * goroutine never started. */
static void ft_pr_discard(ft_PopulateResponse *pr) {
    Alloc *a = heap_allocator();
    if (pr->res != NULL) {
        arena_free(&pr->res->arena);
        mem_free(a, pr->res, sizeof *pr->res, _Alignof(HttpResponse));
    }
    if (pr->pr != NULL)
        io_pipe_free(pr->pr);
    chan_free(pr->ch);
    chan_free(pr->done);
    mem_free(a, pr, sizeof *pr, _Alignof(ft_PopulateResponse));
}

static ft_PopulateResponse *ft_new_populate_response_writer(HttpRequest *req) {
    Alloc *a = heap_allocator();
    ft_PopulateResponse *rw =
        (ft_PopulateResponse *)mem_alloc(a, sizeof *rw, _Alignof(ft_PopulateResponse));
    if (rw == NULL)
        return NULL;
    memset(rw, 0, sizeof *rw);
    io_pipe(a, &rw->pr, &rw->pw);
    rw->ch = chan_make(a, TYPE_BOOL, 0);
    rw->done = chan_make(a, TYPE_BOOL, 0);
    rw->res = (HttpResponse *)mem_alloc(a, sizeof *rw->res, _Alignof(HttpResponse));
    if (rw->pr == NULL || rw->ch == NULL || rw->done == NULL || rw->res == NULL) {
        ft_pr_discard(rw);
        return NULL;
    }
    HttpResponse *res = rw->res;
    memset(res, 0, sizeof *res);
    res->a = a;
    arena_init(&res->arena, a, 0);
    res->proto = BURROW_S("HTTP/1.0");
    res->proto_major = 1;
    res->header = http_header_make(arena_allocator(&res->arena));
    res->close = true;
    res->body = io_pipe_reader_as_io_read_closer(rw->pr);
    res->request = req;
    res->on_free = BURROW_FN(Func, ft_pr_free, rw);
    if (res->header == NULL) {
        ft_pr_discard(rw);
        return NULL;
    }
    return rw;
}

static void ft_serve(void *env) {
    ft_PopulateResponse *rw = (ft_PopulateResponse *)env;
    http_handler_serve_http(rw->fh, (HttpResponseWriter){&ft_populate_response_vt, rw},
                            rw->req);
    ft_pr_finish(rw);
    chan_close(rw->done);
}

static HttpResponse *ft_round_trip(void *self, HttpRequest *req, Error *err) {
    ft_FileTransport *t = (ft_FileTransport *)self;
    if (t == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    /* We start ServeHTTP in a goroutine, which may take a long
     * time if the file is large. The newPopulateResponseWriter
     * call returns a channel which either ServeHTTP or finish()
     * sends our *Response on, once the *Response itself has been
     * populated (even if the body itself is still being
     * written to the res.Body, a pipe) */
    ft_PopulateResponse *rw = ft_new_populate_response_writer(req);
    if (rw == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    rw->fh = t->fh;
    rw->req = req;
    if (!go(BURROW_FN(Func, ft_serve, rw))) {
        ft_pr_discard(rw);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    bool v;
    (void)chan_recv(rw->ch, &v);
    *err = BURROW_NO_ERROR;
    return rw->res;
}
