/* Derived from Go's src/net/http/request.go and clone.go: forms, Request.Clone
 * and ProtocolError.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http.h"

#include "http_internal.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mime.h"
#include "burrow/mime/multipart.h"
#include "burrow/net/url.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------- ProtocolError */

static const Type hf_protocol_error_desc = {
    {(const Byte *)"ProtocolError", 13},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(HttpProtocolError),
    (uint16_t)_Alignof(HttpProtocolError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68707265U, /* "hpre" */
    NULL,
};

const Type *const TYPE_HTTP_PROTOCOL_ERROR = &hf_protocol_error_desc;

static const HttpProtocolError hf_not_supported = {
    BURROW_S_INIT("feature not supported")};
static const HttpProtocolError hf_unexpected_trailer = {
    BURROW_S_INIT("trailer header without chunked transfer encoding")};
static const HttpProtocolError hf_missing_boundary = {
    BURROW_S_INIT("no multipart boundary param in Content-Type")};
static const HttpProtocolError hf_not_multipart = {
    BURROW_S_INIT("request Content-Type isn't multipart/form-data")};
static const HttpProtocolError hf_header_too_long = {BURROW_S_INIT("header too long")};
static const HttpProtocolError hf_short_body = {BURROW_S_INIT("entity body too short")};
static const HttpProtocolError hf_missing_content_length = {
    BURROW_S_INIT("missing ContentLength in HEAD response")};

Str http_protocol_error_error(const HttpProtocolError *pe) {
    return pe->error_string;
}

bool http_protocol_error_is(const HttpProtocolError *pe, Error err) {
    return pe == &hf_not_supported && err.vt == errors_err_unsupported.vt &&
           err.data == errors_err_unsupported.data;
}

static Str hf_protocol_error_message(const void *self) {
    return http_protocol_error_error((const HttpProtocolError *)self);
}

static bool hf_protocol_error_is(const void *self, Error target) {
    return http_protocol_error_is((const HttpProtocolError *)self, target);
}

static const ErrorVT hf_protocol_error_vt = {
    &hf_protocol_error_desc,
    hf_protocol_error_message,
    NULL,
    NULL,
    hf_protocol_error_is,
    NULL,
    NULL,
};

const Error http_err_not_supported = {&hf_protocol_error_vt, &hf_not_supported};
const Error http_err_unexpected_trailer = {&hf_protocol_error_vt,
                                           &hf_unexpected_trailer};
const Error http_err_missing_boundary = {&hf_protocol_error_vt, &hf_missing_boundary};
const Error http_err_not_multipart = {&hf_protocol_error_vt, &hf_not_multipart};
const Error http_err_header_too_long = {&hf_protocol_error_vt, &hf_header_too_long};
const Error http_err_short_body = {&hf_protocol_error_vt, &hf_short_body};
const Error http_err_missing_content_length = {&hf_protocol_error_vt,
                                               &hf_missing_content_length};

BURROW_SENTINEL_ERROR(http_err_missing_file, "http: no such file");
BURROW_SENTINEL_ERROR(http_err_write_after_flush, "unused");

/* ------------------------------------------------------------------- errors */

static const Str hf_text_missing_body = BURROW_S_INIT("missing form body");
static const Str hf_text_post_too_large = BURROW_S_INIT("http: POST too large");
static const Str hf_text_by_reader =
    BURROW_S_INIT("http: multipart handled by MultipartReader");
static const Str hf_text_by_parse =
    BURROW_S_INIT("http: multipart handled by ParseMultipartForm");
static const Str hf_text_reader_twice =
    BURROW_S_INIT("http: MultipartReader called twice");

static Error hf_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

/* The most memory http_request_form_value and http_request_form_file give a
 * multipart form, defaultMaxMemory. */
#define HF_DEFAULT_MAX_MEMORY ((int64_t)32 << 20)

/* multipartByReader. Its presence in multipart_form says the body was handed
 * to a MultipartReader instead of http_request_parse_multipart_form. */
static const MultipartForm hf_by_reader = {NULL, NULL};

static MultipartForm *hf_multipart_by_reader(void) {
    return (MultipartForm *)(uintptr_t)&hf_by_reader;
}

/* --------------------------------------------------------------------- Clone */

/* cloneMultipartFileHeader. The header is a copy, and the rest is shared. */
static MultipartFileHeader *hf_clone_file_header(Alloc *a,
                                                 const MultipartFileHeader *fh) {
    if (fh == NULL)
        return NULL;
    MultipartFileHeader *fh2 =
        (MultipartFileHeader *)mem_alloc(a, sizeof *fh2, _Alignof(MultipartFileHeader));
    if (fh2 == NULL)
        return NULL;
    *fh2 = *fh;
    fh2->header = http_header_clone(a, fh->header);
    if (fh->header != NULL && fh2->header == NULL)
        return NULL;
    return fh2;
}

/* cloneMultipartForm. */
static bool hf_clone_multipart_form(Alloc *a, const MultipartForm *f,
                                    MultipartForm **out) {
    *out = NULL;
    if (f == NULL)
        return true;
    MultipartForm *f2 =
        (MultipartForm *)mem_alloc(a, sizeof *f2, _Alignof(MultipartForm));
    if (f2 == NULL)
        return false;
    f2->value = http_header_clone(a, f->value);
    f2->file = NULL;
    if (f->value != NULL && f2->value == NULL)
        return false;
    if (f->file != NULL) {
        Map *m =
            map_make(a, map_key_type(f->file), map_val_type(f->file), map_len(f->file));
        if (m == NULL)
            return false;
        const void *k;
        void *v;
        for (MapIter it = map_iter(f->file); map_next(&it, &k, &v);) {
            const Slice *vv = (const Slice *)v;
            Slice vv2 = *vv;
            vv2.cap = vv->len;
            if (vv->len > 0) {
                MultipartFileHeader **p = (MultipartFileHeader **)mem_alloc(
                    a, (size_t)vv->len * sizeof *p, _Alignof(MultipartFileHeader *));
                if (p == NULL)
                    return false;
                MultipartFileHeader *const *src = (MultipartFileHeader *const *)vv->p;
                for (Int i = 0; i < vv->len; i++) {
                    p[i] = hf_clone_file_header(a, src[i]);
                    if (src[i] != NULL && p[i] == NULL)
                        return false;
                }
                vv2.p = p;
            }
            if (!map_set(m, k, &vv2))
                return false;
        }
        f2->file = m;
    }
    *out = f2;
    return true;
}

HttpRequest *http_request_clone(const HttpRequest *r, Alloc *a, Context ctx) {
    if (ctx.vt == NULL)
        runtime_panic(BURROW_S("nil context"));
    HttpRequest *r2 = (HttpRequest *)mem_alloc(a, sizeof *r2, _Alignof(HttpRequest));
    if (r2 == NULL)
        return NULL;
    *r2 = *r;
    r2->ctx = ctx;
    /* The copy owns nothing of r's. */
    r2->a = a;
    arena_init(&r2->arena, a, 0);
    r2->wire = NULL;
    Alloc *ra = arena_allocator(&r2->arena);

    bool ok = true;
    r2->url = url_clone(r->url, ra);
    ok = ok && (r->url == NULL || r2->url != NULL);
    r2->header = http_header_clone(ra, r->header);
    ok = ok && (r->header == NULL || r2->header != NULL);
    r2->trailer = http_header_clone(ra, r->trailer);
    ok = ok && (r->trailer == NULL || r2->trailer != NULL);
    if (ok && r->transfer_encoding.p != NULL) {
        Slice s = r->transfer_encoding;
        s.cap = s.len;
        if (s.len > 0) {
            Str *p = (Str *)mem_alloc(ra, (size_t)s.len * sizeof(Str), _Alignof(Str));
            ok = p != NULL;
            if (ok)
                memcpy(p, s.p, (size_t)s.len * sizeof(Str));
            s.p = p;
        }
        r2->transfer_encoding = s;
    }
    r2->form = http_header_clone(ra, r->form);
    ok = ok && (r->form == NULL || r2->form != NULL);
    r2->post_form = http_header_clone(ra, r->post_form);
    ok = ok && (r->post_form == NULL || r2->post_form != NULL);
    ok = ok && hf_clone_multipart_form(ra, r->multipart_form, &r2->multipart_form);

    /* Copy matches and other_values. See issue 61410. */
    if (ok && r->matches.p != NULL) {
        Slice s = r->matches;
        s.cap = s.len;
        if (s.len > 0) {
            Str *p = (Str *)mem_alloc(ra, (size_t)s.len * sizeof(Str), _Alignof(Str));
            ok = p != NULL;
            if (ok)
                memcpy(p, s.p, (size_t)s.len * sizeof(Str));
            s.p = p;
        }
        r2->matches = s;
    }
    if (ok && r->other_values != NULL) {
        r2->other_values = map_clone(ra, r->other_values);
        ok = r2->other_values != NULL;
    }
    if (!ok) {
        http_request_free(r2);
        return NULL;
    }
    return r2;
}

/* --------------------------------------------------------------------- forms */

/* copyValues. False when an allocator says no. */
static bool hf_copy_values(UrlValues dst, UrlValues src) {
    const void *k;
    void *v;
    for (MapIter it = map_iter(src); map_next(&it, &k, &v);) {
        const Slice *vs = (const Slice *)v;
        const Str *p = (const Str *)vs->p;
        for (Int i = 0; i < vs->len; i++)
            if (!url_values_add(dst, *(const Str *)k, p[i]))
                return false;
    }
    return true;
}

static bool hf_method_is(const HttpRequest *r, Str m) {
    return str_eq(r->method, m);
}

/* parsePostForm. *vs is left NULL unless the body is a form that was read. */
static Error hf_parse_post_form(HttpRequest *r, Alloc *ra, UrlValues *vs) {
    *vs = NULL;
    if (r->body.vt == NULL)
        return hf_error(&hf_text_missing_body);
    Str ct = http_header_get(r->header, BURROW_S("Content-Type"));
    /* RFC 7231, section 3.1.1.5 - empty type MAY be treated as
     * application/octet-stream */
    if (ct.len == 0)
        ct = BURROW_S("application/octet-stream");
    Error err;
    ct = mime_parse_media_type(ra, ct, NULL, &err);
    if (str_eq(ct, BURROW_S("application/x-www-form-urlencoded"))) {
        IoReader reader = io_read_closer_as_io_reader(r->body);
        int64_t max_form_size = INT64_MAX;
        IoLimitedReader lr;
        if (!burrow__http_is_max_bytes_reader(r->body)) {
            max_form_size = (int64_t)10 << 20; /* 10 MB is a lot of text. */
            lr = io_limit_reader(reader, max_form_size + 1);
            reader = io_limited_reader_as_io_reader(&lr);
        }
        Error e;
        Slice b = io_read_all(ra, reader, &e);
        if (BURROW_FAILED(e)) {
            if (BURROW_OK(err))
                err = e;
            return err;
        }
        if ((int64_t)b.len > max_form_size)
            return hf_error(&hf_text_post_too_large);
        *vs = url_parse_query(ra, str_from_bytes((const Byte *)b.p, b.len), &e);
        if (BURROW_OK(err))
            err = e;
    }
    /* multipart/form-data is handled by http_request_parse_multipart_form,
     * which is calling this, or should be. */
    return err;
}

Error http_request_parse_form(HttpRequest *r) {
    Alloc *ra = arena_allocator(&r->arena);
    Error err = BURROW_NO_ERROR;
    if (r->post_form == NULL) {
        if (hf_method_is(r, HTTP_METHOD_POST) || hf_method_is(r, HTTP_METHOD_PUT) ||
            hf_method_is(r, HTTP_METHOD_PATCH))
            err = hf_parse_post_form(r, ra, &r->post_form);
        if (r->post_form == NULL)
            r->post_form = url_values_make(ra);
        if (r->post_form == NULL)
            return burrow_err_out_of_memory;
    }
    if (r->form == NULL) {
        if (map_len(r->post_form) > 0) {
            r->form = url_values_make(ra);
            if (r->form == NULL || !hf_copy_values(r->form, r->post_form))
                return burrow_err_out_of_memory;
        }
        UrlValues new_values = NULL;
        if (r->url != NULL) {
            Error e;
            new_values = url_parse_query(ra, r->url->raw_query, &e);
            if (BURROW_OK(err))
                err = e;
        }
        if (new_values == NULL)
            new_values = url_values_make(ra);
        if (new_values == NULL)
            return burrow_err_out_of_memory;
        if (r->form == NULL)
            r->form = new_values;
        else if (!hf_copy_values(r->form, new_values))
            return burrow_err_out_of_memory;
    }
    return err;
}

/* multipartReader. */
static MultipartReader *hf_multipart_reader(HttpRequest *r, bool allow_mixed,
                                            Error *err) {
    Str v = http_header_get(r->header, BURROW_S("Content-Type"));
    if (v.len == 0) {
        *err = http_err_not_multipart;
        return NULL;
    }
    if (r->body.vt == NULL) {
        *err = hf_error(&hf_text_missing_body);
        return NULL;
    }
    Alloc *ra = arena_allocator(&r->arena);
    Map *params = NULL;
    Error e;
    Str d = mime_parse_media_type(ra, v, &params, &e);
    if (BURROW_FAILED(e) ||
        !(str_eq(d, BURROW_S("multipart/form-data")) ||
          (allow_mixed && str_eq(d, BURROW_S("multipart/mixed"))))) {
        *err = http_err_not_multipart;
        return NULL;
    }
    Str key = BURROW_S("boundary");
    const Str *boundary = params != NULL ? (const Str *)map_get(params, &key) : NULL;
    if (boundary == NULL) {
        *err = http_err_missing_boundary;
        return NULL;
    }
    MultipartReader *mr =
        multipart_new_reader(ra, io_read_closer_as_io_reader(r->body), *boundary);
    if (mr == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return mr;
}

MultipartReader *http_request_multipart_reader(HttpRequest *r, Error *err) {
    if (r->multipart_form == hf_multipart_by_reader()) {
        *err = hf_error(&hf_text_reader_twice);
        return NULL;
    }
    if (r->multipart_form != NULL) {
        *err = hf_error(&hf_text_by_parse);
        return NULL;
    }
    r->multipart_form = hf_multipart_by_reader();
    return hf_multipart_reader(r, true, err);
}

Error http_request_parse_multipart_form(HttpRequest *r, int64_t max_memory) {
    if (r->multipart_form == hf_multipart_by_reader())
        return hf_error(&hf_text_by_reader);
    Error parse_form_err = BURROW_NO_ERROR;
    if (r->form == NULL) {
        /* Let errors in ParseForm fall through, and just return it at the
         * end. */
        parse_form_err = http_request_parse_form(r);
        if (r->form == NULL)
            return parse_form_err;
    }
    if (r->multipart_form != NULL)
        return BURROW_NO_ERROR;

    Error err;
    MultipartReader *mr = hf_multipart_reader(r, false, &err);
    if (mr == NULL)
        return err;
    MultipartForm *f = multipart_reader_read_form(mr, max_memory, &err);
    multipart_reader_free(mr);
    if (f == NULL)
        return err;

    if (r->post_form == NULL)
        r->post_form = url_values_make(arena_allocator(&r->arena));
    if (r->post_form == NULL)
        return burrow_err_out_of_memory;
    /* post_form gets the values as well, issue 9305. */
    if (!hf_copy_values(r->form, f->value) || !hf_copy_values(r->post_form, f->value))
        return burrow_err_out_of_memory;
    r->multipart_form = f;
    return parse_form_err;
}

Str http_request_form_value(HttpRequest *r, Str key) {
    if (r->form == NULL)
        (void)http_request_parse_multipart_form(r, HF_DEFAULT_MAX_MEMORY);
    return url_values_get(r->form, key);
}

Str http_request_post_form_value(HttpRequest *r, Str key) {
    if (r->post_form == NULL)
        (void)http_request_parse_multipart_form(r, HF_DEFAULT_MAX_MEMORY);
    return url_values_get(r->post_form, key);
}

MultipartFile *http_request_form_file(HttpRequest *r, Str key, MultipartFileHeader **fh,
                                      Error *err) {
    if (fh != NULL)
        *fh = NULL;
    if (r->multipart_form == hf_multipart_by_reader()) {
        *err = hf_error(&hf_text_by_reader);
        return NULL;
    }
    if (r->multipart_form == NULL) {
        Error e = http_request_parse_multipart_form(r, HF_DEFAULT_MAX_MEMORY);
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }
    }
    if (r->multipart_form != NULL && r->multipart_form->file != NULL) {
        const Slice *fhs = (const Slice *)map_get(r->multipart_form->file, &key);
        if (fhs != NULL && fhs->len > 0) {
            MultipartFileHeader *h = ((MultipartFileHeader *const *)fhs->p)[0];
            if (fh != NULL)
                *fh = h;
            return multipart_file_header_open(h, arena_allocator(&r->arena), err);
        }
    }
    *err = http_err_missing_file;
    return NULL;
}
