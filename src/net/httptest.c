/* net/http/httptest: NewRequest and ResponseRecorder.
 * Go source: go1.27.1, src/net/http/httptest/httptest.go and recorder.go.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httptest.h"

#include "../xnet/httpguts.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const Str hh_text_no_memory = BURROW_S_INIT("net/http/httptest: out of memory");

BURROW_NORETURN static void hh_out_of_memory(void) {
    panic_str(hh_text_no_memory);
}

/* ---------------------------------------------------------------- NewRequest */

HttpRequest *httptest_new_request(Alloc *a, Str method, Str target, IoReader body) {
    return httptest_new_request_with_context(a, context_background(), method, target,
                                             body);
}

/* Whether r is http_no_body seen as an IoReader. */
static bool hh_is_no_body(IoReader r) {
    IoReader nb = io_read_closer_as_io_reader(http_no_body);
    return r.vt == nb.vt && r.data == nb.data;
}

HttpRequest *httptest_new_request_with_context(Alloc *a, Context ctx, Str method,
                                               Str target, IoReader body) {
    if (method.len == 0)
        method = BURROW_S("GET");

    /* The request line says HTTP/1.0 so that it needs no Host field, and the
     * request is made HTTP/1.1 below. The request copies what it keeps out of
     * the reader, so the reader can go once it is read. */
    Arena tmp;
    arena_init(&tmp, a, 0);
    Alloc *ta = arena_allocator(&tmp);
    Str line = fmt_sprintf_v(ta, "%s %s HTTP/1.0\r\n\r\n", method, target);
    StringsReader *sr =
        (StringsReader *)mem_alloc(ta, sizeof *sr, _Alignof(StringsReader));
    if (sr == NULL)
        hh_out_of_memory();
    strings_reader_reset(sr, line);
    BufioReader *br = bufio_new_reader(ta, strings_reader_as_io_reader(sr));
    if (br == NULL)
        hh_out_of_memory();
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_read_request(a, br, &err);
    arena_free(&tmp);
    if (req == NULL) {
        panic_str(
            fmt_sprintf_v(a, "invalid NewRequest arguments; %s", error_text(err)));
    }

    /* Request.WithContext, without the copy. */
    if (ctx.vt == NULL)
        panic_str(BURROW_S("nil context"));
    req->ctx = ctx;

    req->proto = BURROW_S("HTTP/1.1");
    req->proto_minor = 1;
    req->close = false;

    if (body.vt != NULL) {
        const Type *t = body.vt->self_type;
        if (t != NULL && t == TYPE_BYTES_BUFFER)
            req->content_length = bytes_buffer_len((BytesBuffer *)body.data);
        else if (t != NULL && t == TYPE_BYTES_READER)
            req->content_length = bytes_reader_len((BytesReader *)body.data);
        else if (t != NULL && t == TYPE_STRINGS_READER)
            req->content_length = strings_reader_len((StringsReader *)body.data);
        else
            req->content_length = -1;
        if (hh_is_no_body(body)) {
            req->content_length = 0;
            req->body = http_no_body;
        } else {
            IoNopCloser *nc = (IoNopCloser *)mem_alloc(
                arena_allocator(&req->arena), sizeof *nc, _Alignof(IoNopCloser));
            if (nc == NULL)
                hh_out_of_memory();
            *nc = io_nop_closer(body);
            req->body = io_nop_closer_as_io_read_closer(nc);
        }
    }

    /* 192.0.2.0/24 is TEST-NET-1, which RFC 5737 keeps for documentation and
     * examples, so it is never anyone's real address. */
    req->remote_addr = BURROW_S("192.0.2.1:1234");

    if (req->host.len == 0)
        req->host = BURROW_S("example.com");
    return req;
}

/* ---------------------------------------------------------- ResponseRecorder */

/* The method io_write_string looks for. */
#define HH_RECORDER_METHODS(M, T)                                                      \
    M(T, WriteString, httptest_response_recorder_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(HttptestResponseRecorder, HH_RECORDER_METHODS);

static const Type hh_recorder_desc = {
    {(const Byte *)"ResponseRecorder", 16},
    {(const Byte *)"net/http/httptest", 17},
    KIND_STRUCT,
    (uint32_t)sizeof(HttptestResponseRecorder),
    (uint16_t)_Alignof(HttptestResponseRecorder),
    0,
    (uint16_t)(sizeof burrow__methods_HttptestResponseRecorder /
               sizeof burrow__methods_HttptestResponseRecorder[0]),
    NULL,
    burrow__methods_HttptestResponseRecorder,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_HTTPTEST_RESPONSE_RECORDER = &hh_recorder_desc;

static Int hh_vt_write(void *self, Slice p, Error *err) {
    return httptest_response_recorder_write((HttptestResponseRecorder *)self, p, err);
}

static HttpHeader hh_vt_header(void *self) {
    return httptest_response_recorder_header((HttptestResponseRecorder *)self);
}

static void hh_vt_write_header(void *self, Int code) {
    httptest_response_recorder_write_header((HttptestResponseRecorder *)self, code);
}

static const HttpResponseWriterVT hh_writer_vt = {
    {&hh_recorder_desc, hh_vt_write}, hh_vt_header, hh_vt_write_header};

HttpResponseWriter
httptest_response_recorder_as_response_writer(HttptestResponseRecorder *rw) {
    return (HttpResponseWriter){&hh_writer_vt, rw};
}

HttptestResponseRecorder *httptest_new_recorder(Alloc *a) {
    HttptestResponseRecorder *rw = (HttptestResponseRecorder *)mem_alloc(
        a, sizeof *rw, _Alignof(HttptestResponseRecorder));
    if (rw == NULL)
        return NULL;
    memset(rw, 0, sizeof *rw);
    rw->a = a;
    arena_init(&rw->arena, a, 0);
    rw->code = HTTP_STATUS_OK;
    rw->header_map = http_header_make(arena_allocator(&rw->arena));
    rw->own_body = bytes_new_buffer(a, slice_from(NULL, 0, 0, TYPE_BYTE));
    rw->body = rw->own_body;
    if (rw->header_map == NULL || rw->own_body == NULL) {
        httptest_response_recorder_free(rw);
        return NULL;
    }
    return rw;
}

void httptest_response_recorder_free(HttptestResponseRecorder *rw) {
    if (rw == NULL)
        return;
    if (rw->own_body != NULL)
        bytes_buffer_free(rw->own_body);
    arena_free(&rw->arena);
    mem_free(rw->a, rw, sizeof *rw, _Alignof(HttptestResponseRecorder));
}

HttpHeader httptest_response_recorder_header(HttptestResponseRecorder *rw) {
    if (rw->header_map == NULL) {
        rw->header_map = http_header_make(arena_allocator(&rw->arena));
        if (rw->header_map == NULL)
            hh_out_of_memory();
    }
    return rw->header_map;
}

/* Go's writeHeader: the 200 and the Content-Type a first write sends, with b
 * the start of the body. */
static void hh_write_header_for(HttptestResponseRecorder *rw, Slice b) {
    if (rw->wrote_header)
        return;
    if (b.len > 512)
        b.len = 512;

    HttpHeader m = httptest_response_recorder_header(rw);
    Str ct = BURROW_S("Content-Type");
    bool has_type = map_get(m, &ct) != NULL;
    bool has_te = http_header_get(m, BURROW_S("Transfer-Encoding")).len != 0;
    if (!has_type && !has_te) {
        if (!http_header_set(m, ct, http_detect_content_type(b)))
            hh_out_of_memory();
    }
    httptest_response_recorder_write_header(rw, HTTP_STATUS_OK);
}

/* bodyAllowedForStatus, RFC 9110 section 6.4.1. */
static bool hh_body_allowed(Int status) {
    if (status >= 100 && status <= 199)
        return false;
    return status != 204 && status != 304;
}

Int httptest_response_recorder_write(HttptestResponseRecorder *rw, Slice buf,
                                     Error *err) {
    /* The bytes are kept even when the result is an error. */
    hh_write_header_for(rw, buf);
    if (rw->body != NULL)
        (void)bytes_buffer_write(rw->body, buf, NULL);
    if (!hh_body_allowed(rw->code)) {
        BURROW_OUT(err, http_err_body_not_allowed);
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return buf.len;
}

Int httptest_response_recorder_write_string(HttptestResponseRecorder *rw, Str str,
                                            Error *err) {
    hh_write_header_for(
        rw, slice_from((void *)(uintptr_t)str.p, str.len, str.len, TYPE_BYTE));
    if (rw->body != NULL)
        (void)bytes_buffer_write_string(rw->body, str, NULL);
    if (!hh_body_allowed(rw->code)) {
        BURROW_OUT(err, http_err_body_not_allowed);
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str.len;
}

void httptest_response_recorder_write_header(HttptestResponseRecorder *rw, Int code) {
    if (rw->wrote_header)
        return;

    /* Issue 22880: a code has to be three digits, as a server insists. */
    if (code < 100 || code > 999)
        panic_str(fmt_sprintf_v(arena_allocator(&rw->arena),
                                "invalid WriteHeader code %d", code));
    rw->code = code;
    rw->wrote_header = true;
    HttpHeader m = httptest_response_recorder_header(rw);
    rw->snap_header = http_header_clone(arena_allocator(&rw->arena), m);
    if (rw->snap_header == NULL)
        hh_out_of_memory();
}

void httptest_response_recorder_flush(HttptestResponseRecorder *rw) {
    if (!rw->wrote_header)
        httptest_response_recorder_write_header(rw, HTTP_STATUS_OK);
    rw->flushed = true;
}

/* The trailers a Trailer field in the snapshot names, with their values as
 * header_map has them now. False when the allocator says no. */
static bool hh_declared_trailers(HttptestResponseRecorder *rw, HttpResponse *res,
                                 Alloc *ra) {
    Str tk = BURROW_S("Trailer");
    Slice *trailers =
        rw->snap_header != NULL ? (Slice *)map_get(rw->snap_header, &tk) : NULL;
    if (trailers == NULL)
        return true;
    res->trailer = http_header_make(ra);
    if (res->trailer == NULL)
        return false;
    for (Int i = 0; i < trailers->len; i++) {
        Str v = ((const Str *)trailers->p)[i];
        Int start = 0;
        for (Int j = 0; j <= v.len; j++) {
            if (j < v.len && v.p[j] != ',')
                continue;
            Str part = start < v.len ? (Str){v.p + start, j - start} : BURROW_STR_EMPTY;
            start = j + 1;
            Str k = http_canonical_header_key(ra, textproto_trim_string(part));
            /* RFC 9110 section 6.5.1 forbids these as trailers. */
            if (!burrow__httpguts_valid_trailer_header(k))
                continue;
            Slice *vv =
                rw->header_map != NULL ? (Slice *)map_get(rw->header_map, &k) : NULL;
            if (vv == NULL)
                continue;
            Slice vv2 = slice_make(ra, TYPE_STRING, vv->len, vv->len);
            if (vv->len > 0) {
                if (vv2.p == NULL)
                    return false;
                memcpy(vv2.p, vv->p, (size_t)vv->len * sizeof(Str));
            }
            if (!map_set(res->trailer, &k, &vv2))
                return false;
        }
    }
    return true;
}

HttpResponse *httptest_response_recorder_result(HttptestResponseRecorder *rw) {
    if (rw->result != NULL)
        return rw->result;
    Alloc *ra = arena_allocator(&rw->arena);
    if (rw->snap_header == NULL && rw->header_map != NULL) {
        rw->snap_header = http_header_clone(ra, rw->header_map);
        if (rw->snap_header == NULL)
            return NULL;
    }
    HttpResponse *res =
        (HttpResponse *)mem_alloc(ra, sizeof *res, _Alignof(HttpResponse));
    if (res == NULL)
        return NULL;
    memset(res, 0, sizeof *res);
    res->proto = BURROW_S("HTTP/1.1");
    res->proto_major = 1;
    res->proto_minor = 1;
    res->status_code = rw->code;
    res->header = rw->snap_header;
    if (res->status_code == 0)
        res->status_code = HTTP_STATUS_OK;
    res->status = fmt_sprintf_v(ra, "%03d %s", res->status_code,
                                http_status_text(res->status_code));
    if (res->status.len == 0)
        return NULL;
    if (rw->body != NULL) {
        BytesReader *br =
            (BytesReader *)mem_alloc(ra, sizeof *br, _Alignof(BytesReader));
        IoNopCloser *nc =
            (IoNopCloser *)mem_alloc(ra, sizeof *nc, _Alignof(IoNopCloser));
        if (br == NULL || nc == NULL)
            return NULL;
        bytes_reader_reset(br, bytes_buffer_bytes(rw->body));
        *nc = io_nop_closer(bytes_reader_as_io_reader(br));
        res->body = io_nop_closer_as_io_read_closer(nc);
    } else {
        res->body = http_no_body;
    }
    res->content_length = burrow__httptest_parse_content_length(
        http_header_get(res->header, BURROW_S("Content-Length")));

    if (!hh_declared_trailers(rw, res, ra))
        return NULL;
    if (rw->header_map != NULL) {
        MapIter it = map_iter(rw->header_map);
        const void *kp;
        void *vp;
        while (map_next(&it, &kp, &vp)) {
            Str k = *(const Str *)kp;
            if (!strings_has_prefix(k, HTTP_TRAILER_PREFIX))
                continue;
            if (res->trailer == NULL) {
                res->trailer = http_header_make(ra);
                if (res->trailer == NULL)
                    return NULL;
            }
            Str name = {k.p + HTTP_TRAILER_PREFIX.len, k.len - HTTP_TRAILER_PREFIX.len};
            const Slice *vv = (const Slice *)vp;
            for (Int i = 0; i < vv->len; i++) {
                if (!http_header_add(res->trailer, name, ((const Str *)vv->p)[i]))
                    return NULL;
            }
        }
    }
    rw->result = res;
    return res;
}

int64_t burrow__httptest_parse_content_length(Str s) {
    s = textproto_trim_string(s);
    if (s.len == 0)
        return -1;
    Error err = BURROW_NO_ERROR;
    uint64_t n = strconv_parse_uint(s, 10, 63, &err);
    if (!BURROW_OK(err))
        return -1;
    return (int64_t)n;
}
