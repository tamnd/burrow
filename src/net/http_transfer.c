/* Derived from Go's src/net/http/transfer.go, the reading half: how long a
 * body is and how it comes, from the header of the message it is in, and the
 * body that reads it from the wire.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "../xnet/httpguts.h"
#include "http_ascii.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(http_err_body_read_after_close,
                      "http: invalid Read on closed Body");

static const Str hb_text_trailer_eof =
    BURROW_S_INIT("http: unexpected EOF reading trailer");
static const Str hb_text_long_trailer =
    BURROW_S_INIT("http: suspiciously long trailer after chunked body");

static Error hb_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

static bool hb_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

Error burrow__http_bad_string_error(Str what, Str val) {
    return fmt_errorf_v("%s %q", what, val);
}

/* header[key], the values as they are without making key canonical. */
static Slice hb_values(HttpHeader h, Str key) {
    Slice *v = h != NULL ? (Slice *)map_get(h, &key) : NULL;
    return v != NULL ? *v : slice_from(NULL, 0, 0, TYPE_STRING);
}

/* ------------------------------------------------------------------ NoBody */

static Int hb_no_body_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_eof;
    return 0;
}

static Error hb_no_body_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT hb_no_body_vt = {{NULL, hb_no_body_read},
                                             {NULL, hb_no_body_close}};

const IoReadCloser http_no_body = {&hb_no_body_vt, NULL};

/* ------------------------------------------------------- unsupportedTEError */

/* The message follows the Str in the same block. */
static Str hb_te_message(const void *self) {
    return *(const Str *)self;
}

static Error hb_te_clone(const void *self, Alloc *a);

static const ErrorVT hb_te_error_vt = {
    NULL, hb_te_message, NULL, NULL, NULL, NULL, hb_te_clone,
};

static Error hb_te_error(Alloc *a, Str message) {
    Str *box = (Str *)mem_alloc(a, sizeof(Str) + (size_t)message.len, _Alignof(Str));
    if (box == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(box + 1);
    if (message.len > 0)
        memcpy(p, message.p, (size_t)message.len);
    *box = str_from_bytes(p, message.len);
    return (Error){&hb_te_error_vt, box};
}

static Error hb_te_clone(const void *self, Alloc *a) {
    return hb_te_error(a, *(const Str *)self);
}

bool burrow__http_is_unsupported_te_error(Error err) {
    return err.vt == &hb_te_error_vt;
}

/* ---------------------------------------------------------- the transfer */

/* transferReader. */
typedef struct HbTransfer {
    /* In. */
    HttpHeader header;
    Str request_method;
    Int status_code;
    Int proto_major;
    Int proto_minor;
    /* Out. */
    IoReadCloser body;
    int64_t content_length;
    HttpHeader trailer;
    bool chunked;
    bool close;
} HbTransfer;

/* bodyAllowedForStatus. */
bool burrow__http_body_allowed_for_status(Int status) {
    if (status >= 100 && status <= 199)
        return false;
    return status != 204 && status != 304;
}

/* noResponseBodyExpected. */
static bool hb_no_response_body_expected(Str method) {
    return str_eq(method, BURROW_S("HEAD"));
}

Error burrow__http_parse_transfer_encoding(HttpHeader header, Int major, Int minor,
                                           bool *chunked) {
    *chunked = false;
    Str key = BURROW_S("Transfer-Encoding");
    Slice *present = header != NULL ? (Slice *)map_get(header, &key) : NULL;
    if (present == NULL)
        return BURROW_NO_ERROR;
    Slice raw = *present;
    map_del(header, &key);

    /* Issue 12785: Transfer-Encoding is ignored on HTTP/1.0 requests. */
    if (major < 1 || (major == 1 && minor < 1))
        return BURROW_NO_ERROR;

    /* As nginx does, only one Transfer-Encoding field, set to "chunked". This
     * is where request smuggling happens, so it is kept strict and simple. */
    if (raw.len != 1) {
        Str m =
            fmt_sprintf_v(error_allocator(), "too many transfer encodings: %q", raw);
        return hb_te_error(error_allocator(), m);
    }
    Str te = ((const Str *)raw.p)[0];
    if (!burrow__http_ascii_equal_fold(te, BURROW_S("chunked"))) {
        Str m =
            fmt_sprintf_v(error_allocator(), "unsupported transfer encoding: %q", te);
        return hb_te_error(error_allocator(), m);
    }
    *chunked = true;
    return BURROW_NO_ERROR;
}

int64_t burrow__http_parse_content_length(Slice cl, Error *err) {
    *err = BURROW_NO_ERROR;
    if (cl.len == 0)
        return -1;
    Str s = textproto_trim_string(((const Str *)cl.p)[0]);

    /* The Content-Length has to be a number, RFC 2616 section 14.13. */
    if (s.len == 0) {
        if (burrow__http_godebug_lax_content_length())
            return -1;
        *err =
            burrow__http_bad_string_error(BURROW_S("invalid empty Content-Length"), s);
        return 0;
    }
    Error e;
    uint64_t n = strconv_parse_uint(s, 10, 63, &e);
    if (BURROW_FAILED(e)) {
        *err = burrow__http_bad_string_error(BURROW_S("bad Content-Length"), s);
        return 0;
    }
    return (int64_t)n;
}

/* fixLength. The length of the body by RFC 7230 section 3.3, -1 when it is not
 * known. */
static int64_t hb_fix_length(bool is_response, Int status, Str request_method,
                             HttpHeader header, bool chunked, Error *err) {
    Str key = BURROW_S("Content-Length");
    Slice lens = hb_values(header, key);
    int64_t n = 0;
    *err = BURROW_NO_ERROR;

    /* Against request smuggling, RFC 7230 section 3.3.2: more than one
     * Content-Length is an error unless they are all the same, and then they
     * become one. Issue 16490. */
    if (lens.len > 1) {
        const Str *v = (const Str *)lens.p;
        Str first = textproto_trim_string(v[0]);
        for (Int i = 1; i < lens.len; i++) {
            if (!str_eq(first, textproto_trim_string(v[i]))) {
                *err = fmt_errorf_v("http: message cannot contain multiple "
                                    "Content-Length headers; got %q",
                                    lens);
                return 0;
            }
        }
        map_del(header, &key);
        if (!textproto_mime_header_add(header, key, first)) {
            *err = burrow_err_out_of_memory;
            return 0;
        }
        lens = hb_values(header, key);
    }

    /* A Content-Length that is not a number is an error. */
    if (lens.len > 0) {
        n = burrow__http_parse_content_length(lens, err);
        if (BURROW_FAILED(*err))
            return -1;
    }

    if (is_response && hb_no_response_body_expected(request_method))
        return 0;
    if (status / 100 == 1)
        return 0;
    if (status == 204 || status == 304)
        return 0;

    /* RFC 9112: Transfer-Encoding wins over Content-Length, and the message
     * is taken with the Content-Length taken out. */
    if (chunked) {
        map_del(header, &key);
        return -1;
    }
    if (lens.len > 0)
        return n;
    map_del(header, &key);

    /* RFC 7230 neither allows nor forbids a body on a GET, so one is taken
     * when it is declared, and a request with neither chunks nor a length has
     * none. */
    if (!is_response)
        return 0;
    /* The body ends when the connection does. */
    return -1;
}

bool burrow__http_should_close(Int major, Int minor, HttpHeader header,
                               bool remove_close_header) {
    if (major < 1)
        return true;
    Str key = BURROW_S("Connection");
    Slice conv = hb_values(header, key);
    const Str *v = (const Str *)conv.p;
    bool has_close =
        burrow__httpguts_header_values_contains_token(v, conv.len, BURROW_S("close"));
    if (major == 1 && minor == 0) {
        return has_close || !burrow__httpguts_header_values_contains_token(
                                v, conv.len, BURROW_S("keep-alive"));
    }
    if (has_close && remove_close_header)
        map_del(header, &key);
    return has_close;
}

static bool hb_is_bad_trailer_key(Str key) {
    return str_eq(key, BURROW_S("Transfer-Encoding")) ||
           str_eq(key, BURROW_S("Trailer")) || str_eq(key, BURROW_S("Content-Length"));
}

typedef struct HbTrailerKeys {
    Alloc *a;
    HttpHeader trailer;
    Error *err;
    bool oom;
} HbTrailerKeys;

/* The body of fixTrailer's foreachHeaderElement. */
static void hb_trailer_key(void *env, Str f) {
    HbTrailerKeys *k = (HbTrailerKeys *)env;
    if (k->oom)
        return;
    Str key = textproto_canonical_mime_header_key(k->a, f);
    if (hb_is_bad_trailer_key(key)) {
        if (BURROW_OK(*k->err))
            *k->err = burrow__http_bad_string_error(BURROW_S("bad trailer key"), key);
        return;
    }
    Slice none = slice_from(NULL, 0, 0, TYPE_STRING);
    if (!map_set(k->trailer, &key, &none))
        k->oom = true;
}

/* fixTrailer. The keys the Trailer field names, as a header with no values,
 * or NULL when there are none or the body is not chunked. */
static HttpHeader hb_fix_trailer(Alloc *a, HttpHeader header, bool chunked,
                                 Error *err) {
    Str tkey = BURROW_S("Trailer");
    *err = BURROW_NO_ERROR;
    Slice *present = header != NULL ? (Slice *)map_get(header, &tkey) : NULL;
    /* A Trailer without chunks is wrong, but it is left in the header for the
     * caller to judge and no error. Issue 27197. */
    if (present == NULL || !chunked)
        return NULL;
    Slice vv = *present;
    map_del(header, &tkey);

    HttpHeader trailer = http_header_make(a);
    if (trailer == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    HbTrailerKeys keys = {a, trailer, err, false};
    for (Int i = 0; i < vv.len && !keys.oom; i++)
        burrow__http_foreach_header_element(((const Str *)vv.p)[i], hb_trailer_key,
                                            &keys);
    if (keys.oom) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    if (BURROW_FAILED(*err) || map_len(trailer) == 0)
        return NULL;
    return trailer;
}

/* -------------------------------------------------------------- the body */

/* body. It reads src, and Close makes sure the body has all been read and
 * then reads the trailer when there is one. */
typedef struct HbBody {
    IoReader src;
    IoLimitedReader lr;    /* src, when the body has a length */
    HttpChunkedReader *cr; /* src, when the body comes in chunks */
    HttpHeader *trailer;   /* where the trailer goes, NULL when there is none to read */
    BufioReader *r;        /* the wire, which the trailer is read from */
    Alloc *a;              /* the message's arena */
    int64_t max_trailer_headers;
    SyncMutex mu; /* guards the flags below, and Read and Close */
    bool limited;
    bool closing;        /* the connection closes after the body */
    bool do_early_close; /* Close stops early */
    bool saw_eof;
    bool closed;
    bool early_close; /* Close came before the end of src */
    Func on_hit_eof;  /* called when the end has been read, nil for none */
} HbBody;

/* seeUpcomingDoubleCRLF. Peeks until bufio's buffer is full, looking for the
 * blank line that ends a trailer, which keeps a trailer to the buffer's size. */
static bool hb_see_upcoming_double_crlf(BufioReader *r) {
    for (Int size = 4;; size++) {
        Error err;
        Slice buf = bufio_reader_peek(r, size, &err);
        if (buf.len >= 4 &&
            memcmp((const Byte *)buf.p + buf.len - 4, "\r\n\r\n", 4) == 0)
            return true;
        if (BURROW_FAILED(err))
            break;
    }
    return false;
}

/* mergeSetHeader. */
static Error hb_merge_set_header(HttpHeader *dst, HttpHeader src) {
    if (*dst == NULL) {
        *dst = src;
        return BURROW_NO_ERROR;
    }
    const void *k;
    void *v;
    for (MapIter it = map_iter(src); map_next(&it, &k, &v);) {
        if (!map_set(*dst, k, v))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

static Error hb_read_trailer(HbBody *b) {
    /* The usual case, since nobody sends a trailer. */
    Error err;
    Slice buf = bufio_reader_peek(b->r, 2, &err);
    const Byte *p = (const Byte *)buf.p;
    if (buf.len == 2 && p[0] == '\r' && p[1] == '\n') {
        (void)bufio_reader_discard(b->r, 2, &err);
        return BURROW_NO_ERROR;
    }
    if (buf.len < 2)
        return hb_error(&hb_text_trailer_eof);
    if (BURROW_FAILED(err))
        return err;

    /* A trailer of unbounded size would be a denial of service, and there is
     * no limited reader to put in here, so make sure the blank line that ends
     * the trailer is already within the bufio buffer, typically 4 KiB. */
    if (!hb_see_upcoming_double_crlf(b->r))
        return hb_error(&hb_text_long_trailer);

    TextprotoReader tp = {0};
    tp.r = b->r;
    HttpHeader hdr = burrow__textproto_read_mime_header(&tp, b->a, INT64_MAX,
                                                        b->max_trailer_headers, &err);
    textproto_reader_free(&tp);
    if (BURROW_FAILED(err)) {
        if (hb_same_error(err, io_eof))
            return hb_error(&hb_text_trailer_eof);
        return err;
    }
    return hb_merge_set_header(b->trailer, hdr);
}

/* body.readLocked, with b->mu held. */
static Int hb_read_locked(HbBody *b, Slice p, Error *err) {
    if (b->saw_eof) {
        *err = io_eof;
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = b->src.vt->read(b->src.data, p, &e);

    if (hb_same_error(e, io_eof)) {
        b->saw_eof = true;
        if (b->trailer != NULL) {
            /* Chunked, so the trailer comes next. */
            Error te = hb_read_trailer(b);
            if (BURROW_FAILED(te)) {
                /* Nothing more may be read from the body, nor another
                 * request from the connection. Issue 12027. */
                e = te;
                b->saw_eof = false;
                b->closed = true;
            }
            b->trailer = NULL;
        } else if (b->limited && b->lr.n > 0) {
            /* The body has a length, and the end came before it. */
            e = io_err_unexpected_eof;
        }
    }

    /* io_eof with the last of the data, which io.Reader allows, and which lets
     * a transport reuse the connection before the caller reads again. */
    if (BURROW_OK(e) && n > 0 && b->limited && b->lr.n == 0) {
        e = io_eof;
        b->saw_eof = true;
    }
    if (b->saw_eof && !BURROW_FUNC_IS_NIL(b->on_hit_eof))
        BURROW_CALLF0(b->on_hit_eof);
    *err = e;
    return n;
}

static Int hb_body_read(void *self, Slice p, Error *err) {
    HbBody *b = (HbBody *)self;
    sync_mutex_lock(&b->mu);
    Int n = 0;
    if (b->closed)
        *err = http_err_body_read_after_close;
    else
        n = hb_read_locked(b, p, err);
    sync_mutex_unlock(&b->mu);
    return n;
}

/* io.Copy or io.CopyN to io.Discard from bodyLocked, at most limit bytes. The
 * error is io_eof when the body ended before limit, as CopyN has it. */
static int64_t hb_discard_locked(HbBody *b, int64_t limit, Error *err) {
    Byte buf[8192];
    int64_t n = 0;
    *err = BURROW_NO_ERROR;
    while (n < limit) {
        Int want = (Int)sizeof buf;
        if (limit - n < (int64_t)want)
            want = (Int)(limit - n);
        Error e = BURROW_NO_ERROR;
        Int m = 0;
        if (b->closed)
            e = http_err_body_read_after_close;
        else
            m = hb_read_locked(b, slice_from(buf, want, want, TYPE_BYTE), &e);
        n += (int64_t)m;
        if (BURROW_FAILED(e)) {
            *err = e;
            break;
        }
    }
    return n;
}

static Error hb_body_close(void *self) {
    HbBody *b = (HbBody *)self;
    sync_mutex_lock(&b->mu);
    if (b->closed) {
        sync_mutex_unlock(&b->mu);
        return BURROW_NO_ERROR;
    }
    Error err = BURROW_NO_ERROR;
    /* Nothing to do when the end has been seen already, nor when there is no
     * trailer and the connection closes next anyway. */
    if (!b->saw_eof && !(b->trailer == NULL && b->closing)) {
        if (b->do_early_close) {
            /* Read up to maxPostHandlerReadBytes of the body, looking for its
             * end and the trailer, so the connection can be used again. */
            if (b->limited && b->lr.n > BURROW__HTTP_MAX_POST_HANDLER_READ_BYTES) {
                /* The rest of the declared length is more than that. */
                b->early_close = true;
            } else {
                int64_t n = hb_discard_locked(
                    b, BURROW__HTTP_MAX_POST_HANDLER_READ_BYTES + 1, &err);
                b->early_close = true;
                if (hb_same_error(err, io_eof) &&
                    n <= BURROW__HTTP_MAX_POST_HANDLER_READ_BYTES) {
                    b->early_close = false;
                    b->saw_eof = true;
                    /* The end of the body is what was wanted here, and not an
                     * error to give the caller. */
                    err = BURROW_NO_ERROR;
                }
            }
        } else {
            /* Read all of the body, and the trailer after it. */
            (void)hb_discard_locked(b, INT64_MAX, &err);
            if (hb_same_error(err, io_eof))
                err = BURROW_NO_ERROR;
        }
    }
    b->closed = true;
    sync_mutex_unlock(&b->mu);
    return err;
}

static const IoReadCloserVT hb_body_vt = {{NULL, hb_body_read}, {NULL, hb_body_close}};

void burrow__http_body_free(void *body) {
    HbBody *b = (HbBody *)body;
    if (b == NULL)
        return;
    burrow__http_chunked_reader_free(b->a, b->cr);
    mem_free(b->a, b, sizeof *b, _Alignof(HbBody));
}

bool burrow__http_body_did_early_close(void *body) {
    HbBody *b = (HbBody *)body;
    sync_mutex_lock(&b->mu);
    bool v = b->early_close;
    sync_mutex_unlock(&b->mu);
    return v;
}

bool burrow__http_body_remains(void *body) {
    HbBody *b = (HbBody *)body;
    if (b == NULL)
        return false;
    sync_mutex_lock(&b->mu);
    bool v = !b->saw_eof;
    sync_mutex_unlock(&b->mu);
    return v;
}

void burrow__http_body_set_do_early_close(void *body, bool on) {
    ((HbBody *)body)->do_early_close = on;
}

void burrow__http_body_register_on_hit_eof(void *body, Func fn) {
    HbBody *b = (HbBody *)body;
    if (b == NULL)
        return;
    sync_mutex_lock(&b->mu);
    b->on_hit_eof = fn;
    sync_mutex_unlock(&b->mu);
}

Error burrow__http_body_close(void *body) {
    if (body == NULL)
        return BURROW_NO_ERROR;
    return hb_body_close(body);
}

void burrow__http_body_state(void *body, bool *closed, bool *saw_eof, int64_t *unread) {
    HbBody *b = (HbBody *)body;
    sync_mutex_lock(&b->mu);
    *closed = b->closed;
    *saw_eof = b->saw_eof;
    *unread = b->limited ? b->lr.n : -1;
    sync_mutex_unlock(&b->mu);
}

IoReadCloser burrow__http_new_body(Alloc *a, IoReader src, HttpHeader *trailer,
                                   BufioReader *r) {
    HbBody *b = (HbBody *)mem_alloc(a, sizeof *b, _Alignof(HbBody));
    if (b == NULL)
        return (IoReadCloser){0};
    b->a = a;
    b->src = src;
    b->trailer = trailer;
    b->r = r;
    b->max_trailer_headers = INT64_MAX;
    return (IoReadCloser){&hb_body_vt, b};
}

/* ------------------------------------------------------------ readTransfer */

Error burrow__http_read_transfer(HttpRequest *req, HttpResponse *resp, BufioReader *r,
                                 int64_t max_trailer_headers) {
    HbTransfer t = {0};
    t.request_method = BURROW_S("GET");
    bool is_response = resp != NULL;
    Alloc *a;
    if (is_response) {
        t.header = resp->header;
        t.status_code = resp->status_code;
        t.proto_major = resp->proto_major;
        t.proto_minor = resp->proto_minor;
        t.close =
            burrow__http_should_close(t.proto_major, t.proto_minor, t.header, true);
        if (resp->request != NULL)
            t.request_method = resp->request->method;
        a = arena_allocator(&resp->arena);
    } else {
        t.header = req->header;
        t.request_method = req->method;
        t.proto_major = req->proto_major;
        t.proto_minor = req->proto_minor;
        /* A request's body is read as a response's to a GET with status 200
         * would be. */
        t.status_code = 200;
        t.close = req->close;
        a = arena_allocator(&req->arena);
    }

    /* HTTP/1.1 is the default. */
    if (t.proto_major == 0 && t.proto_minor == 0) {
        t.proto_major = 1;
        t.proto_minor = 1;
    }

    /* Transfer-Encoding: chunked, which overrides Content-Length. */
    Error err = burrow__http_parse_transfer_encoding(t.header, t.proto_major,
                                                     t.proto_minor, &t.chunked);
    if (BURROW_FAILED(err))
        return err;

    int64_t real_length = hb_fix_length(is_response, t.status_code, t.request_method,
                                        t.header, t.chunked, &err);
    if (BURROW_FAILED(err))
        return err;
    if (is_response && str_eq(t.request_method, BURROW_S("HEAD"))) {
        t.content_length = burrow__http_parse_content_length(
            hb_values(t.header, BURROW_S("Content-Length")), &err);
        if (BURROW_FAILED(err))
            return err;
    } else {
        t.content_length = real_length;
    }

    t.trailer = hb_fix_trailer(a, t.header, t.chunked, &err);
    if (BURROW_FAILED(err))
        return err;

    /* A response with neither a length nor chunks, and with a status that
     * allows a body, has a body that ends with the connection. RFC 7230
     * section 3.3. */
    if (is_response && real_length == -1 && !t.chunked &&
        burrow__http_body_allowed_for_status(t.status_code))
        t.close = true;

    /* The body. */
    HbBody *b = NULL;
    t.body = http_no_body;
    if (t.chunked) {
        if (!is_response || (!hb_no_response_body_expected(t.request_method) &&
                             burrow__http_body_allowed_for_status(t.status_code))) {
            b = (HbBody *)mem_alloc(a, sizeof *b, _Alignof(HbBody));
            if (b == NULL)
                return burrow_err_out_of_memory;
            b->cr = burrow__http_new_chunked_reader(a, bufio_reader_as_io_reader(r));
            if (b->cr == NULL)
                return burrow_err_out_of_memory;
            b->src = burrow__http_chunked_reader_as_io_reader(b->cr);
            b->trailer = is_response ? &resp->trailer : &req->trailer;
            b->r = r;
            b->max_trailer_headers = max_trailer_headers;
        }
    } else if (real_length > 0 || (real_length < 0 && t.close)) {
        /* A known length, or, with close semantics such as HTTP/1.0's,
         * whatever comes before the connection closes. A persistent
         * connection with no length has no body. */
        b = (HbBody *)mem_alloc(a, sizeof *b, _Alignof(HbBody));
        if (b == NULL)
            return burrow_err_out_of_memory;
        b->src = bufio_reader_as_io_reader(r);
        if (real_length > 0) {
            b->lr = io_limit_reader(b->src, real_length);
            b->src = io_limited_reader_as_io_reader(&b->lr);
            b->limited = true;
        }
    }
    if (b != NULL) {
        b->a = a;
        b->closing = t.close;
        t.body = (IoReadCloser){&hb_body_vt, b};
    }

    Slice te = slice_from(NULL, 0, 0, TYPE_STRING);
    if (t.chunked) {
        Str chunked = BURROW_S("chunked");
        te = slice_append(a, te, &chunked, 1);
        if (te.len != 1) {
            burrow__http_body_free(b);
            return burrow_err_out_of_memory;
        }
    }

    if (is_response) {
        resp->body = t.body;
        resp->wire = b;
        resp->content_length = t.content_length;
        resp->transfer_encoding = te;
        resp->close = t.close;
        resp->trailer = t.trailer;
    } else {
        req->body = t.body;
        req->wire = b;
        req->content_length = t.content_length;
        req->transfer_encoding = te;
        req->close = t.close;
        req->trailer = t.trailer;
    }
    return BURROW_NO_ERROR;
}
