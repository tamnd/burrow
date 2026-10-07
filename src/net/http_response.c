/* Derived from Go's src/net/http/response.go, the parts that read a response
 * from the wire, write one to it and look at one.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(http_err_no_location, "http: no Location header in response");

static bool hr_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

bool http_response_proto_at_least(const HttpResponse *r, Int major, Int minor) {
    return r->proto_major > major ||
           (r->proto_major == major && r->proto_minor >= minor);
}

Url *http_response_location(const HttpResponse *r, Alloc *a, Error *err) {
    Str lv = http_header_get(r->header, BURROW_S("Location"));
    if (lv.len == 0) {
        *err = http_err_no_location;
        return NULL;
    }
    if (r->request != NULL && r->request->url != NULL)
        return url_parse_ref(r->request->url, a, lv, err);
    return url_parse(a, lv, err);
}

/* ReadResponse after the response is made. */
static Error hr_read(HttpResponse *resp, TextprotoReader *tp, BufioReader *r) {
    Alloc *a = arena_allocator(&resp->arena);

    /* The status line. */
    Error err;
    Str line = textproto_reader_read_line(tp, a, &err);
    if (BURROW_FAILED(err))
        return err;
    const Byte *p = line.p;
    const Byte *sp =
        line.len > 0 ? (const Byte *)memchr(p, ' ', (size_t)line.len) : NULL;
    if (sp == NULL)
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP response"), line);
    resp->proto = str_from_bytes(p, (Int)(sp - p));
    Str status = str_from_bytes(sp + 1, line.len - (Int)(sp - p) - 1);
    /* strings.TrimLeft(status, " ") */
    while (status.len > 0 && (status.p)[0] == ' ')
        status = str_from_bytes(status.p + 1, status.len - 1);
    resp->status = status;

    const Byte *sp2 =
        status.len > 0 ? (const Byte *)memchr(status.p, ' ', (size_t)status.len) : NULL;
    Str code = sp2 != NULL ? str_from_bytes(status.p, (Int)(sp2 - status.p)) : status;
    if (code.len != 3)
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP status code"),
                                             code);
    Error e;
    resp->status_code = strconv_atoi(code, &e);
    if (BURROW_FAILED(e) || resp->status_code < 0)
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP status code"),
                                             code);
    if (!http_parse_http_version(resp->proto, &resp->proto_major, &resp->proto_minor))
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP version"),
                                             resp->proto);

    /* The header. */
    resp->header = textproto_reader_read_mime_header(tp, a, &err);
    if (BURROW_FAILED(err))
        return err;

    if (!burrow__http_fix_pragma_cache_control(resp->header))
        return burrow_err_out_of_memory;

    return burrow__http_read_transfer(NULL, resp, r, INT64_MAX);
}

HttpResponse *http_read_response(Alloc *a, BufioReader *r, HttpRequest *req,
                                 Error *err) {
    HttpResponse *resp =
        (HttpResponse *)mem_alloc(a, sizeof *resp, _Alignof(HttpResponse));
    if (resp == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    resp->a = a;
    arena_init(&resp->arena, a, 0);
    resp->request = req;
    resp->body = http_no_body;

    TextprotoReader tp = {0};
    tp.r = r;
    Error e = hr_read(resp, &tp, r);
    textproto_reader_free(&tp);
    if (BURROW_FAILED(e)) {
        if (hr_same_error(e, io_eof))
            e = io_err_unexpected_eof;
        /* The error may be in the response's arena, which is about to go. */
        *err = error_retain(error_allocator(), e);
        http_response_free(resp);
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return resp;
}

void http_response_free(HttpResponse *r) {
    if (r == NULL)
        return;
    if (!BURROW_FUNC_IS_NIL(r->on_free))
        BURROW_CALLF0(r->on_free);
    burrow__http_body_free(r->wire);
    arena_free(&r->arena);
    mem_free(r->a, r, sizeof *r, _Alignof(HttpResponse));
}

/* --------------------------------------------------------------- writing */

/* The fields Response.Write writes itself and so leaves out of the header. */
static const Str hr_resp_write_exclude[] = {
    BURROW_S_INIT("Content-Length"),
    BURROW_S_INIT("Transfer-Encoding"),
    BURROW_S_INIT("Trailer"),
};

/* The body once its first byte has been read to see whether it is empty: that
 * byte and then the rest, closed by closing the body it came from. */
typedef struct hr_Peeked {
    IoReadCloser body;
    bool have_byte;
    Byte b;
} hr_Peeked;

static Int hr_peeked_read(void *self, Slice p, Error *err) {
    hr_Peeked *pk = (hr_Peeked *)self;
    if (pk->have_byte) {
        *err = BURROW_NO_ERROR;
        if (p.len == 0)
            return 0;
        ((Byte *)p.p)[0] = pk->b;
        pk->have_byte = false;
        return 1;
    }
    return pk->body.vt->reader.read(pk->body.data, p, err);
}

static Error hr_peeked_close(void *self) {
    hr_Peeked *pk = (hr_Peeked *)self;
    return pk->body.vt->closer.close(pk->body.data);
}

static const IoReadCloserVT hr_peeked_vt = {
    {NULL, hr_peeked_read},
    {NULL, hr_peeked_close},
};

static Error hr_write(const HttpResponse *r, IoWriter w, Alloc *sa,
                      burrow__HttpTransferWriter *tw, hr_Peeked *pk) {
    /* The status line. */
    Str text = r->status;
    if (text.len == 0) {
        text = http_status_text(r->status_code);
        if (text.len == 0)
            text = fmt_sprintf_v(sa, "status code %d", r->status_code);
    } else {
        /* Only to save saying "200 200 OK" when status is "200 OK". */
        Str prefix = fmt_sprintf_v(sa, "%d ", r->status_code);
        if (text.len >= prefix.len && memcmp(text.p, prefix.p, (size_t)prefix.len) == 0)
            text = str_from_bytes(text.p + prefix.len, text.len - prefix.len);
    }
    Error err = BURROW_NO_ERROR;
    (void)io_write_string(w,
                          fmt_sprintf_v(sa, "HTTP/%d.%d %03d %s\r\n", r->proto_major,
                                        r->proto_minor, r->status_code, text),
                          &err);
    if (BURROW_FAILED(err))
        return err;

    /* A copy, so that r1 can change. */
    HttpResponse r1 = *r;
    if (r1.content_length == 0 && r1.body.vt != NULL) {
        /* Whether it is empty, or its length is just not known. */
        Byte buf[1];
        Int n = r1.body.vt->reader.read(r1.body.data, slice_from(buf, 1, 1, TYPE_BYTE),
                                        &err);
        if (BURROW_FAILED(err) && !(err.vt == io_eof.vt && err.data == io_eof.data))
            return err;
        if (n == 0) {
            /* A known empty reader, in case this one does not like being read
             * again. */
            r1.body = http_no_body;
        } else {
            r1.content_length = -1;
            pk->body = r->body;
            pk->have_byte = true;
            pk->b = buf[0];
            r1.body = (IoReadCloser){&hr_peeked_vt, pk};
        }
    }
    /* A non-chunked HTTP/1.1 response with no Content-Length can only end the
     * HTTP/1.0 way, by closing the connection. */
    if (r1.content_length == -1 && !r1.close &&
        http_response_proto_at_least(&r1, 1, 1) &&
        !burrow__http_chunked(r1.transfer_encoding) && !r1.uncompressed)
        r1.close = true;

    /* The body, content_length, close and trailer. */
    err = burrow__http_new_transfer_writer(tw, sa, NULL, &r1);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__http_transfer_writer_write_header(tw, sa, w, NULL);
    if (BURROW_FAILED(err))
        return err;

    /* The rest of the header. */
    err = burrow__http_header_write_except(
        r->header, w, hr_resp_write_exclude,
        (Int)(sizeof hr_resp_write_exclude / sizeof hr_resp_write_exclude[0]), NULL);
    if (BURROW_FAILED(err))
        return err;

    /* A POST or PUT response may have sent Content-Length already, even when it
     * is 0, Go issue 8180. */
    bool content_length_already_sent =
        burrow__http_transfer_writer_should_send_content_length(tw);
    if (r1.content_length == 0 && !burrow__http_chunked(r1.transfer_encoding) &&
        !content_length_already_sent &&
        burrow__http_body_allowed_for_status(r->status_code)) {
        (void)io_write_string(w, BURROW_S("Content-Length: 0\r\n"), &err);
        if (BURROW_FAILED(err))
            return err;
    }

    /* The end of the header. */
    (void)io_write_string(w, BURROW_S("\r\n"), &err);
    if (BURROW_FAILED(err))
        return err;

    /* The body and the trailer. */
    return burrow__http_transfer_writer_write_body(tw, sa, w);
}

Error http_response_write(HttpResponse *r, IoWriter w) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    burrow__HttpTransferWriter tw;
    memset(&tw, 0, sizeof tw);
    hr_Peeked pk;
    memset(&pk, 0, sizeof pk);
    Error err = hr_write(r, w, arena_allocator(&scratch), &tw, &pk);
    burrow__http_transfer_writer_done(&tw);
    /* An error made in the scratch arena has to outlive it. */
    if (BURROW_FAILED(err))
        err = error_retain(error_allocator(), err);
    arena_free(&scratch);
    return err;
}
