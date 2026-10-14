/* Derived from Go's src/net/http/transfer.go, the transferWriter, which writes
 * the framing and the body of a request or a response.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "../xnet/httpguts.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const Str hw_chunked_te[1] = {BURROW_S_INIT("chunked")};

/* err == io.EOF, which is the error itself and not one wrapping it. */
static bool hw_is_eof(Error err) {
    return err.vt == io_eof.vt && err.data == io_eof.data;
}

bool burrow__http_chunked(Slice te) {
    return te.len > 0 && str_eq(((const Str *)te.p)[0], BURROW_S("chunked"));
}

bool burrow__http_is_identity(Slice te) {
    return te.len == 1 && str_eq(((const Str *)te.p)[0], BURROW_S("identity"));
}

BufioWriter *burrow__http_bufio_writer_of(IoWriter w) {
    if (w.vt == NULL || w.vt->self_type != TYPE_BUFIO_WRITER)
        return NULL;
    return (BufioWriter *)w.data;
}

Str burrow__http_validate_headers(Alloc *a, HttpHeader hdrs) {
    if (hdrs == NULL)
        return BURROW_STR_EMPTY;
    MapIter it = map_iter(hdrs);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str key = *(const Str *)k;
        if (!burrow__httpguts_valid_header_field_name(key))
            return fmt_sprintf_v(a, "field name %q", key);
        const Slice *vv = (const Slice *)v;
        const Str *vs = (const Str *)vv->p;
        for (Int i = 0; i < vv->len; i++) {
            /* The value is left out of the message, since it may be secret. */
            if (!burrow__httpguts_valid_header_field_value(vs[i]))
                return fmt_sprintf_v(a, "field value for %q", key);
        }
    }
    return BURROW_STR_EMPTY;
}

/* ---------------------------------------------------------------- the probe
 *
 * probeRequestBody reads a byte of the body in a goroutine and waits for it a
 * while. When the wait runs out the goroutine is still reading, so what it
 * shares with the writer is counted, and whichever of them is last frees it. */

typedef struct burrow__HttpProbe {
    IoReader body;
    Chan *done; /* gets one value once n, b and err are set */
    Error err;
    Int n;
    int32_t refs;
    Byte b;
} burrow__HttpProbe;

static void hw_probe_release(burrow__HttpProbe *p) {
    if (sync_atomic_add_int32(&p->refs, -1) != 0)
        return;
    chan_free(p->done);
    mem_free(heap_allocator(), p, sizeof *p, _Alignof(burrow__HttpProbe));
}

static void hw_probe_run(void *env) {
    burrow__HttpProbe *p = (burrow__HttpProbe *)env;
    Byte buf[1];
    Error err = BURROW_NO_ERROR;
    p->n = BURROW_CALL(p->body, read, slice_from(buf, 1, 1, TYPE_BYTE), &err);
    p->err = err;
    if (p->n == 1)
        p->b = buf[0];
    uint8_t one = 1;
    chan_send(p->done, &one);
    hw_probe_release(p);
}

/* The body after the probe: the byte it read, then either the error it got or
 * the rest of the body. While the probe is still going, the first read waits
 * for it, which is Go's finishAsyncByteRead. */
static Int hw_probed_read(void *self, Slice p, Error *err) {
    burrow__HttpProbed *pr = (burrow__HttpProbed *)self;
    *err = BURROW_NO_ERROR;
    if (pr->async != NULL) {
        if (p.len == 0)
            return 0;
        burrow__HttpProbe *probe = pr->async;
        chan_recv(probe->done, NULL);
        Int n = probe->n;
        Error e = probe->err;
        if (n == 1)
            ((Byte *)p.p)[0] = probe->b;
        pr->async = NULL;
        if (BURROW_FAILED(e) && !hw_is_eof(e)) {
            /* MultiReader stops here, and the next read finds the channel
             * drained and moves on to the body. */
            *err = e;
            return n;
        }
        if (n > 0)
            return n;
    }
    if (pr->have_byte) {
        if (p.len == 0)
            return 0;
        ((Byte *)p.p)[0] = pr->b;
        pr->have_byte = false;
        return 1;
    }
    if (pr->has_tail) {
        *err = pr->tail;
        return 0;
    }
    return BURROW_CALL(pr->rest, read, p, err);
}

static const IoReaderVT hw_probed_vt = {NULL, hw_probed_read};

/* What the read of the first byte found. */
static void hw_take_probe(burrow__HttpTransferWriter *t, Int n, Byte b, Error err) {
    burrow__HttpProbed *pr = &t->probed;
    if (n == 0 && hw_is_eof(err)) {
        /* It was empty. */
        t->body = (IoReader){0};
        t->content_length = 0;
    } else if (n == 1) {
        pr->rest = t->body;
        pr->have_byte = true;
        pr->b = b;
        if (BURROW_FAILED(err)) {
            pr->has_tail = true;
            pr->tail = err;
        }
        t->body = (IoReader){&hw_probed_vt, pr};
    } else if (BURROW_FAILED(err)) {
        pr->has_tail = true;
        pr->tail = err;
        t->body = (IoReader){&hw_probed_vt, pr};
    }
}

/* isKnownInMemoryReader. */
bool burrow__http_is_known_in_memory_reader(IoReader r) {
    IoReader inner;
    while (burrow__io_unwrap_nop_closer(r, &inner))
        r = inner;
    if (r.vt == NULL)
        return false;
    const Type *t = r.vt->self_type;
    return t != NULL && (t == TYPE_BYTES_READER || t == TYPE_BYTES_BUFFER ||
                         t == TYPE_STRINGS_READER);
}

/* As if the probe ran out of time without reading anything: the length stays
 * unknown and the header goes out before the body is read. */
static void hw_probe_gave_up(burrow__HttpTransferWriter *t) {
    t->flush_headers = true;
}

static void hw_probe_request_body(burrow__HttpTransferWriter *t) {
    if (burrow__http_is_known_in_memory_reader(t->body)) {
        /* It cannot block, so it needs no goroutine and no timer. */
        Byte buf[1];
        Error err = BURROW_NO_ERROR;
        Int n = BURROW_CALL(t->body, read, slice_from(buf, 1, 1, TYPE_BYTE), &err);
        hw_take_probe(t, n, buf[0], err);
        return;
    }
    if (sched_current() == NULL) {
        hw_probe_gave_up(t);
        return;
    }
    Alloc *ha = heap_allocator();
    burrow__HttpProbe *p =
        (burrow__HttpProbe *)mem_alloc(ha, sizeof *p, _Alignof(burrow__HttpProbe));
    if (p == NULL) {
        hw_probe_gave_up(t);
        return;
    }
    p->body = t->body;
    p->refs = 2;
    p->done = chan_make(ha, TYPE_UINT8, 1);
    if (p->done == NULL || !go(BURROW_FN(Func, hw_probe_run, p))) {
        chan_free(p->done);
        mem_free(ha, p, sizeof *p, _Alignof(burrow__HttpProbe));
        hw_probe_gave_up(t);
        return;
    }
    TimeTimer *timer = time_new_timer(ha, 200 * TIME_MILLISECOND);
    Int got = 0;
    if (timer != NULL) {
        SelectCase cases[2] = {BURROW_RECV(p->done, NULL),
                               BURROW_RECV(time_timer_c(timer), NULL)};
        got = chan_select(cases, 2);
        (void)time_timer_stop(timer);
        time_timer_free(timer);
    } else {
        chan_recv(p->done, NULL);
    }
    if (got == 0) {
        Int n = p->n;
        Byte b = p->b;
        Error err = p->err;
        hw_probe_release(p);
        hw_take_probe(t, n, b, err);
        return;
    }
    /* Too slow. Read it later, and keep assuming that the length is unknown,
     * which means a chunked body. */
    t->probe = p;
    t->probed.async = p;
    t->probed.rest = t->body;
    t->body = (IoReader){&hw_probed_vt, &t->probed};
    t->flush_headers = true;
}

/* -------------------------------------------------------- newTransferWriter */

bool burrow__http_request_method_usually_lacks_body(Str method) {
    static const Str methods[] = {
        BURROW_S_INIT("GET"),     BURROW_S_INIT("HEAD"),     BURROW_S_INIT("DELETE"),
        BURROW_S_INIT("OPTIONS"), BURROW_S_INIT("PROPFIND"), BURROW_S_INIT("SEARCH"),
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (str_eq(method, methods[i]))
            return true;
    }
    return false;
}

int64_t burrow__http_request_outgoing_length(const HttpRequest *r) {
    if (r->body.vt == NULL || r->body.vt == http_no_body.vt)
        return 0;
    if (r->content_length != 0)
        return r->content_length;
    return -1;
}

/* shouldSendChunkedRequestBody. */
static bool hw_should_send_chunked_request_body(burrow__HttpTransferWriter *t) {
    if (t->content_length >= 0 || t->body.vt == NULL)
        return false;
    if (str_eq(t->method, BURROW_S("CONNECT")))
        return false;
    if (burrow__http_request_method_usually_lacks_body(t->method)) {
        /* Only for the methods that confuse servers when they come with a
         * body is the body read to see whether it has anything in it. */
        hw_probe_request_body(t);
        return t->body.vt != NULL;
    }
    return true;
}

static IoReader hw_reader_of(IoReadCloser rc) {
    if (rc.vt == NULL)
        return (IoReader){0};
    return io_read_closer_as_io_reader(rc);
}

Error burrow__http_new_transfer_writer(burrow__HttpTransferWriter *t, Alloc *a,
                                       HttpRequest *req, HttpResponse *resp) {
    memset(t, 0, sizeof *t);
    t->transfer_encoding = slice_from(NULL, 0, 0, TYPE_STRING);

    bool at_least_http11 = false;
    if (req != NULL) {
        if (req->content_length != 0 && req->body.vt == NULL)
            return fmt_errorf_v("http: Request.ContentLength=%d with nil Body",
                                req->content_length);
        t->method = req->method.len > 0 ? req->method : BURROW_S("GET");
        t->close = req->close;
        t->transfer_encoding = req->transfer_encoding;
        t->header = req->header;
        t->trailer = req->trailer;
        t->body = hw_reader_of(req->body);
        t->body_closer = req->body;
        t->content_length = burrow__http_request_outgoing_length(req);
        if (t->content_length < 0 && t->transfer_encoding.len == 0 &&
            hw_should_send_chunked_request_body(t))
            t->transfer_encoding =
                slice_from((void *)(uintptr_t)hw_chunked_te, 1, 1, TYPE_STRING);
        /* With a body, flush the header to a BufioWriter before copying the
         * body, which may block, in case the server needs the header first.
         * Not for the readers in memory, which would only cost a packet. */
        if (t->content_length != 0 && !burrow__http_is_known_in_memory_reader(t->body))
            t->flush_headers = true;
        at_least_http11 = true; /* the transport only sends 1.1 or 2 */
    } else {
        t->is_response = true;
        if (resp->request != NULL)
            t->method = resp->request->method;
        t->body = hw_reader_of(resp->body);
        t->body_closer = resp->body;
        t->content_length = resp->content_length;
        t->close = resp->close;
        t->transfer_encoding = resp->transfer_encoding;
        t->header = resp->header;
        t->trailer = resp->trailer;
        at_least_http11 = http_response_proto_at_least(resp, 1, 1);
        t->response_to_head = str_eq(t->method, BURROW_S("HEAD"));
    }

    /* Sanitize body, content_length and transfer_encoding. */
    if (t->response_to_head) {
        t->body = (IoReader){0};
        if (burrow__http_chunked(t->transfer_encoding))
            t->content_length = -1;
    } else {
        if (!at_least_http11 || t->body.vt == NULL)
            t->transfer_encoding = slice_from(NULL, 0, 0, TYPE_STRING);
        if (burrow__http_chunked(t->transfer_encoding))
            t->content_length = -1;
        else if (t->body.vt == NULL)
            t->content_length = 0;
    }

    if (!burrow__http_chunked(t->transfer_encoding))
        t->trailer = NULL;

    /* The trailer's names go on the Trailer line of the header as they are, so
     * a CR or LF in one would write a header line of its own. */
    Str bad = burrow__http_validate_headers(a, t->trailer);
    if (bad.len > 0)
        return fmt_errorf_v("net/http: invalid trailer %s", bad);
    return BURROW_NO_ERROR;
}

void burrow__http_transfer_writer_done(burrow__HttpTransferWriter *t) {
    if (t->probe != NULL) {
        hw_probe_release(t->probe);
        t->probe = NULL;
        t->probed.async = NULL;
    }
}

/* -------------------------------------------------------------- writeHeader */

bool burrow__http_transfer_writer_should_send_content_length(
    const burrow__HttpTransferWriter *t) {
    if (burrow__http_chunked(t->transfer_encoding))
        return false;
    if (t->content_length > 0)
        return true;
    if (t->content_length < 0)
        return false;
    /* Many servers expect a Content-Length for these methods. */
    if (str_eq(t->method, BURROW_S("POST")) || str_eq(t->method, BURROW_S("PUT")) ||
        str_eq(t->method, BURROW_S("PATCH")))
        return true;
    if (t->content_length == 0 && burrow__http_is_identity(t->transfer_encoding)) {
        if (str_eq(t->method, BURROW_S("GET")) || str_eq(t->method, BURROW_S("HEAD")))
            return false;
        return true;
    }
    return false;
}

Error burrow__http_transfer_writer_write_header(burrow__HttpTransferWriter *t, Alloc *a,
                                                IoWriter w) {
    Error err = BURROW_NO_ERROR;
    if (t->close && !burrow__http_has_token(
                        burrow__http_header_get(t->header, BURROW_S("Connection")),
                        BURROW_S("close"))) {
        (void)io_write_string(w, BURROW_S("Connection: close\r\n"), &err);
        if (BURROW_FAILED(err))
            return err;
    }

    /* Content-Length or Transfer-Encoding, from what the body, content_length
     * and transfer_encoding came to. */
    if (burrow__http_transfer_writer_should_send_content_length(t)) {
        (void)io_write_string(w, BURROW_S("Content-Length: "), &err);
        if (BURROW_FAILED(err))
            return err;
        (void)io_write_string(w, fmt_sprintf_v(a, "%d\r\n", t->content_length), &err);
        if (BURROW_FAILED(err))
            return err;
    } else if (burrow__http_chunked(t->transfer_encoding)) {
        (void)io_write_string(w, BURROW_S("Transfer-Encoding: chunked\r\n"), &err);
        if (BURROW_FAILED(err))
            return err;
    }

    /* The Trailer line. */
    if (t->trailer != NULL) {
        Int n = map_len(t->trailer);
        Str *keys =
            (Str *)mem_alloc(a, (size_t)(n > 0 ? n : 1) * sizeof(Str), _Alignof(Str));
        if (keys == NULL)
            return burrow_err_out_of_memory;
        Int nk = 0;
        MapIter it = map_iter(t->trailer);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            Str key = http_canonical_header_key(a, *(const Str *)k);
            if (str_eq(key, BURROW_S("Transfer-Encoding")) ||
                str_eq(key, BURROW_S("Trailer")) ||
                str_eq(key, BURROW_S("Content-Length")))
                return burrow__http_bad_string_error(BURROW_S("invalid Trailer key"),
                                                     key);
            keys[nk++] = key;
        }
        if (nk > 0) {
            slices_sort(slice_from(keys, nk, nk, TYPE_STRING));
            Str line = fmt_sprintf_v(
                a, "Trailer: %s\r\n",
                strings_join(a, slice_from(keys, nk, nk, TYPE_STRING), BURROW_S(",")));
            (void)io_write_string(w, line, &err);
            if (BURROW_FAILED(err))
                return err;
        }
    }
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- writeBody */

/* bufioFlushWriter. Each write is flushed when w is a BufioWriter. */
static Int hw_flush_write(void *self, Slice p, Error *err) {
    IoWriter w = *(const IoWriter *)self;
    Int n = BURROW_CALL(w, write, p, err);
    BufioWriter *bw = burrow__http_bufio_writer_of(w);
    if (n > 0 && bw != NULL) {
        Error ferr = bufio_writer_flush(bw);
        if (BURROW_FAILED(ferr) && BURROW_OK(*err))
            *err = ferr;
    }
    return n;
}

static const IoWriterVT hw_flush_writer_vt = {NULL, hw_flush_write};

/* doBodyCopy. An error is kept in body_read_error as well. */
static int64_t hw_do_body_copy(burrow__HttpTransferWriter *t, Alloc *a, IoWriter dst,
                               IoReader src, Error *err) {
    int64_t n = io_copy(a, dst, src, err);
    if (BURROW_FAILED(*err) && !hw_is_eof(*err))
        t->body_read_error = *err;
    return n;
}

/* unwrapBody. The reader inside a body made by io_nop_closer, so that a copy
 * can use what it knows about that reader. */
static IoReader hw_unwrap_body(IoReader body) {
    IoReader inner;
    if (burrow__io_unwrap_nop_closer(body, &inner))
        return inner;
    return body;
}

static Error hw_close_body(IoReadCloser rc) {
    return rc.vt->closer.close(rc.data);
}

Error burrow__http_transfer_writer_write_body(burrow__HttpTransferWriter *t, Alloc *a,
                                              IoWriter w) {
    Error err = BURROW_NO_ERROR;
    int64_t ncopy = 0;

    if (!t->response_to_head && t->body.vt != NULL) {
        IoReader body = hw_unwrap_body(t->body);
        if (burrow__http_chunked(t->transfer_encoding)) {
            HttpChunkedWriter cw = {w, NULL};
            if (!t->is_response)
                cw.flush = burrow__http_bufio_writer_of(w);
            IoWriter cww = io_write_closer_as_io_writer(
                burrow__http_chunked_writer_as_io_write_closer(&cw));
            (void)hw_do_body_copy(t, a, cww, body, &err);
            if (BURROW_OK(err))
                err = burrow__http_chunked_writer_close(&cw);
        } else if (t->content_length == -1) {
            IoWriter dst = w;
            IoWriter inner = w;
            if (str_eq(t->method, BURROW_S("CONNECT")))
                dst = (IoWriter){&hw_flush_writer_vt, &inner};
            ncopy = hw_do_body_copy(t, a, dst, body, &err);
        } else {
            IoLimitedReader lr = io_limit_reader(body, t->content_length);
            ncopy = hw_do_body_copy(t, a, w, io_limited_reader_as_io_reader(&lr), &err);
            if (BURROW_FAILED(err))
                goto fail;
            ncopy += hw_do_body_copy(t, a, io_discard, body, &err);
        }
        if (BURROW_FAILED(err))
            goto fail;
    }
    if (t->body_closer.vt != NULL) {
        IoReadCloser rc = t->body_closer;
        t->body_closer = (IoReadCloser){0};
        Error cerr = hw_close_body(rc);
        if (BURROW_FAILED(cerr))
            return cerr;
    }

    if (!t->response_to_head && t->content_length != -1 && t->content_length != ncopy)
        return fmt_errorf_v("http: ContentLength=%d with Body length %d",
                            t->content_length, ncopy);

    if (!t->response_to_head && burrow__http_chunked(t->transfer_encoding)) {
        /* The trailer, then the blank line that ends it. */
        if (t->trailer != NULL) {
            err = http_header_write(t->trailer, w);
            if (BURROW_FAILED(err))
                return err;
        }
        (void)io_write_string(w, BURROW_S("\r\n"), &err);
    }
    return err;

fail:
    if (t->body_closer.vt != NULL) {
        IoReadCloser rc = t->body_closer;
        t->body_closer = (IoReadCloser){0};
        (void)hw_close_body(rc);
    }
    return err;
}
