/* Derived from Go's src/net/http/response.go, the parts that read a response
 * from the wire and look at one.
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
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
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
    burrow__http_body_free(r->wire);
    arena_free(&r->arena);
    mem_free(r->a, r, sizeof *r, _Alignof(HttpResponse));
}
