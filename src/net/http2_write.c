/* net/http/internal/http2's write.go: the frames the server writes, each
 * with what it takes to write it.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"
#include "http_ascii.h"

#include "../xnet/httpguts.h"

#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <string.h>

/* The minimum MAX_FRAME_SIZE every peer has to take. Go sends header blocks
 * in frames this big whatever the peer allows, as there is little point in
 * more. */
enum { HW_MAX_HEADER_FRAG = 16384 };

/* At most this many keys are sorted on the stack. */
#define HW_SORT_STACK 64

static Http2WriteFramer hw_framer(Http2WriteKind kind) {
    Http2WriteFramer w;
    memset(&w, 0, sizeof w);
    w.kind = kind;
    return w;
}

Http2WriteFramer burrow__http2_write_flush(void) {
    return hw_framer(HTTP2_WRITE_FLUSH);
}

Http2WriteFramer burrow__http2_write_settings(const Http2Setting *settings, Int n) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_SETTINGS);
    w.u.settings.p = settings;
    w.u.settings.n = n;
    return w;
}

Http2WriteFramer burrow__http2_write_go_away(uint32_t max_stream_id,
                                             Http2ErrCode code) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_GO_AWAY);
    w.u.go_away.max_stream_id = max_stream_id;
    w.u.go_away.code = code;
    return w;
}

Http2WriteFramer burrow__http2_write_data(uint32_t stream_id, Slice p,
                                          bool end_stream) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_DATA);
    w.u.data.stream_id = stream_id;
    w.u.data.p = p;
    w.u.data.end_stream = end_stream;
    return w;
}

Http2WriteFramer burrow__http2_write_handler_panic_rst(uint32_t stream_id) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_HANDLER_PANIC_RST);
    w.u.stream_id = stream_id;
    return w;
}

Http2WriteFramer burrow__http2_write_stream_error(Http2StreamError se) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_STREAM_ERROR);
    w.u.stream_error = se;
    return w;
}

Http2WriteFramer burrow__http2_write_ping(const Byte data[8]) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_PING);
    memcpy(w.u.ping, data, sizeof w.u.ping);
    return w;
}

Http2WriteFramer burrow__http2_write_ping_ack(const Byte data[8]) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_PING_ACK);
    memcpy(w.u.ping, data, sizeof w.u.ping);
    return w;
}

Http2WriteFramer burrow__http2_write_settings_ack(void) {
    return hw_framer(HTTP2_WRITE_SETTINGS_ACK);
}

Http2WriteFramer burrow__http2_write_res_headers(Http2WriteResHeaders *rh) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_RES_HEADERS);
    w.u.res_headers = rh;
    return w;
}

Http2WriteFramer burrow__http2_write_push_promise(Http2WritePushPromise *pp) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_PUSH_PROMISE);
    w.u.push_promise = pp;
    return w;
}

Http2WriteFramer burrow__http2_write_100_continue(uint32_t stream_id) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_100_CONTINUE);
    w.u.stream_id = stream_id;
    return w;
}

Http2WriteFramer burrow__http2_write_window_update(uint32_t stream_id, uint32_t n) {
    Http2WriteFramer w = hw_framer(HTTP2_WRITE_WINDOW_UPDATE);
    w.u.window_update.stream_id = stream_id;
    w.u.window_update.n = n;
    return w;
}

bool burrow__http2_write_ends_stream(const Http2WriteFramer *w) {
    if (w->kind == HTTP2_WRITE_DATA)
        return w->u.data.end_stream;
    if (w->kind == HTTP2_WRITE_RES_HEADERS)
        return w->u.res_headers->end_stream;
    /* Only a caller that reuses a writer after reply_to_writer cleared it
     * gets here. */
    if (w->kind == HTTP2_WRITE_NIL)
        panic_str(BURROW_S("writeEndsStream called on nil writeFramer"));
    return false;
}

bool burrow__http2_write_stays_within_buffer(const Http2WriteFramer *w, Int max) {
    switch (w->kind) {
    case HTTP2_WRITE_SETTINGS:
        /* A setting is a uint16 and a uint32. */
        return HTTP2_FRAME_HEADER_LEN + 6 * w->u.settings.n <= max;
    case HTTP2_WRITE_DATA:
        return HTTP2_FRAME_HEADER_LEN + w->u.data.p.len <= max;
    case HTTP2_WRITE_HANDLER_PANIC_RST:
    case HTTP2_WRITE_STREAM_ERROR:
    case HTTP2_WRITE_WINDOW_UPDATE:
        return HTTP2_FRAME_HEADER_LEN + 4 <= max;
    case HTTP2_WRITE_PING:
    case HTTP2_WRITE_PING_ACK:
        return HTTP2_FRAME_HEADER_LEN + 8 <= max;
    case HTTP2_WRITE_SETTINGS_ACK:
        return HTTP2_FRAME_HEADER_LEN <= max;
    case HTTP2_WRITE_100_CONTINUE:
        /* Sloppy but on the safe side, as Go's is. */
        return 9 + 2 * (Int)(sizeof ":status" - 1 + sizeof "100" - 1) <= max;
    case HTTP2_WRITE_NIL:
    case HTTP2_WRITE_FLUSH:
    case HTTP2_WRITE_GO_AWAY:
    case HTTP2_WRITE_RES_HEADERS:
    case HTTP2_WRITE_PUSH_PROMISE:
    default:
        /* FLUSH and GO_AWAY flush, and Go does not work out the size of a
         * header block here, as that would cost more than it saves. */
        return false;
    }
}

/* encKV. */
static Error hw_enc_kv(HpackEncoder *enc, Str k, Str v) {
    if (burrow__http2_verbose_logs()) {
        Arena ar;
        memset(&ar, 0, sizeof ar);
        burrow__http2_log(fmt_sprintf_v(arena_allocator(&ar),
                                        "http2: server encoding header %q = %q", k, v));
        arena_free(&ar);
    }
    HpackHeaderField f;
    memset(&f, 0, sizeof f);
    f.name = k;
    f.value = v;
    return burrow__hpack_encoder_write_field(enc, f);
}

/* validWireHeaderFieldName: a token, with no capitals. */
static bool hw_valid_wire_header_field_name(Str v) {
    if (v.len == 0)
        return false;
    for (Int i = 0; i < v.len; i++) {
        Byte c = v.p[i];
        if (!burrow__httpguts_is_token_rune((int32_t)c))
            return false;
        if ('A' <= c && c <= 'Z')
            return false;
    }
    return true;
}

static int hw_key_cmp(void *env, const void *x, const void *y) {
    (void)env;
    return (int)strings_compare(*(const Str *)x, *(const Str *)y);
}

/* encodeHeaders, with the keys at keys, or all of h's in sorted order when
 * keys is NULL. Copies of names in lower case go in a. */
static Error hw_encode_headers(HpackEncoder *enc, HttpHeader h, const Str *keys,
                               Int nkeys, Alloc *a) {
    Str stack[HW_SORT_STACK];
    if (keys == NULL) {
        Int n = h == NULL ? 0 : map_len(h);
        Str *all = stack;
        if (n > HW_SORT_STACK) {
            all = (Str *)mem_alloc_nozero(a, (size_t)n * sizeof(Str), _Alignof(Str));
            if (all == NULL)
                return burrow_err_out_of_memory;
        }
        Int nk = 0;
        if (n > 0) {
            MapIter it = map_iter(h);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v))
                all[nk++] = *(const Str *)k;
        }
        slices_sort_func(slice_from(all, nk, nk, TYPE_STRING),
                         BURROW_FN(SlicesCmpFunc, hw_key_cmp, NULL));
        keys = all;
        nkeys = nk;
    }
    for (Int i = 0; i < nkeys; i++) {
        Str key = keys[i];
        const Slice *vv = h == NULL ? NULL : (const Slice *)map_get(h, &key);
        bool ascii = false;
        Str k = burrow__http_ascii_to_lower(a, key, &ascii);
        if (!ascii) {
            if (k.len == 0 && key.len != 0 && burrow__http_ascii_is_print(key))
                return burrow_err_out_of_memory;
            /* Header field names have to be ASCII, RFC 7540 section 8.1.2,
             * so the field is left out. */
            continue;
        }
        /* These should have been turned down long before, but just in
         * case. golang.org/issue/14048. */
        if (!hw_valid_wire_header_field_name(k))
            continue;
        if (vv == NULL)
            continue;
        bool is_te = str_eq(k, BURROW_S("transfer-encoding"));
        const Str *values = (const Str *)vv->p;
        for (Int j = 0; j < vv->len; j++) {
            Str v = values[j];
            /* Go leaves a bad value out rather than fail. */
            if (!burrow__httpguts_valid_header_field_value(v))
                continue;
            if (is_te && !str_eq(v, BURROW_S("trailers")))
                continue;
            Error err = hw_enc_kv(enc, k, v);
            if (BURROW_FAILED(err))
                return err;
        }
    }
    return BURROW_NO_ERROR;
}

/* httpCodeString. */
static Str hw_http_code_string(Int code, char buf[HTTP2_STRING_MAX]) {
    int n = snprintf(buf, HTTP2_STRING_MAX, "%lld", (long long)code);
    return str_from_bytes(buf, n);
}

/* splitHeaderBlock with writeResHeaders' writeHeaderBlock, or
 * writePushPromise's when pp is set. */
static Error hw_split_header_block(Http2WriteContext ctx, Slice block,
                                   uint32_t stream_id, bool end_stream,
                                   const Http2WritePushPromise *pp) {
    Http2Framer *fr = ctx.vt->framer(ctx.self);
    bool first = true;
    while (block.len > 0) {
        Slice frag = block;
        if (frag.len > HW_MAX_HEADER_FRAG)
            frag = slice_sub(frag, 0, HW_MAX_HEADER_FRAG);
        block = slice_sub(block, frag.len, block.len);
        bool last = block.len == 0;
        Error err;
        if (!first) {
            err = burrow__http2_framer_write_continuation(fr, stream_id, last, frag);
        } else if (pp != NULL) {
            Http2PushPromiseParam p;
            memset(&p, 0, sizeof p);
            p.stream_id = stream_id;
            p.promise_id = pp->promised_id;
            p.block_fragment = frag;
            p.end_headers = last;
            err = burrow__http2_framer_write_push_promise(fr, p);
        } else {
            Http2HeadersFrameParam p;
            memset(&p, 0, sizeof p);
            p.stream_id = stream_id;
            p.block_fragment = frag;
            p.end_stream = end_stream;
            p.end_headers = last;
            err = burrow__http2_framer_write_headers(fr, p);
        }
        if (BURROW_FAILED(err))
            return err;
        first = false;
    }
    return BURROW_NO_ERROR;
}

static Error hw_write_res_headers(const Http2WriteResHeaders *w, Http2WriteContext ctx,
                                  Alloc *a) {
    BytesBuffer *buf = NULL;
    HpackEncoder *enc = ctx.vt->header_encoder(ctx.self, &buf);
    bytes_buffer_reset(buf);
    Error err = BURROW_NO_ERROR;
    if (w->http_res_code != 0) {
        char code[HTTP2_STRING_MAX];
        err = hw_enc_kv(enc, BURROW_S(":status"),
                        hw_http_code_string(w->http_res_code, code));
    }
    if (BURROW_OK(err))
        err = hw_encode_headers(enc, w->h, w->trailers, w->ntrailers, a);
    if (BURROW_OK(err) && w->content_type.len != 0)
        err = hw_enc_kv(enc, BURROW_S("content-type"), w->content_type);
    if (BURROW_OK(err) && w->content_length.len != 0)
        err = hw_enc_kv(enc, BURROW_S("content-length"), w->content_length);
    if (BURROW_OK(err) && w->date.len != 0)
        err = hw_enc_kv(enc, BURROW_S("date"), w->date);
    if (BURROW_FAILED(err))
        return err;
    Slice block = bytes_buffer_bytes(buf);
    if (block.len == 0 && w->trailers == NULL)
        panic_str(BURROW_S("unexpected empty hpack"));
    return hw_split_header_block(ctx, block, w->stream_id, w->end_stream, NULL);
}

static Error hw_write_push_promise(const Http2WritePushPromise *w,
                                   Http2WriteContext ctx, Alloc *a) {
    BytesBuffer *buf = NULL;
    HpackEncoder *enc = ctx.vt->header_encoder(ctx.self, &buf);
    bytes_buffer_reset(buf);
    Error err = hw_enc_kv(enc, BURROW_S(":method"), w->method);
    if (BURROW_OK(err))
        err = hw_enc_kv(enc, BURROW_S(":scheme"), w->url->scheme);
    if (BURROW_OK(err))
        err = hw_enc_kv(enc, BURROW_S(":authority"), w->url->host);
    if (BURROW_OK(err))
        err = hw_enc_kv(enc, BURROW_S(":path"), url_request_uri(w->url, a));
    if (BURROW_OK(err))
        err = hw_encode_headers(enc, w->h, NULL, 0, a);
    if (BURROW_FAILED(err))
        return err;
    Slice block = bytes_buffer_bytes(buf);
    if (block.len == 0)
        panic_str(BURROW_S("unexpected empty hpack"));
    return hw_split_header_block(ctx, block, w->stream_id, false, w);
}

static Error hw_write_100_continue(uint32_t stream_id, Http2WriteContext ctx) {
    BytesBuffer *buf = NULL;
    HpackEncoder *enc = ctx.vt->header_encoder(ctx.self, &buf);
    bytes_buffer_reset(buf);
    Error err = hw_enc_kv(enc, BURROW_S(":status"), BURROW_S("100"));
    if (BURROW_FAILED(err))
        return err;
    Http2HeadersFrameParam p;
    memset(&p, 0, sizeof p);
    p.stream_id = stream_id;
    p.block_fragment = bytes_buffer_bytes(buf);
    p.end_stream = false;
    p.end_headers = true;
    return burrow__http2_framer_write_headers(ctx.vt->framer(ctx.self), p);
}

Error burrow__http2_write_frame(const Http2WriteFramer *w, Http2WriteContext ctx) {
    Error err = BURROW_NO_ERROR;
    Slice no_debug_data = {NULL, 0, 0, TYPE_BYTE};
    Arena ar;
    switch (w->kind) {
    case HTTP2_WRITE_NIL:
        panic_str(BURROW_S("http2: writeFrame called on nil writeFramer"));
    case HTTP2_WRITE_FLUSH:
        return ctx.vt->flush(ctx.self);
    case HTTP2_WRITE_SETTINGS:
        return burrow__http2_framer_write_settings(ctx.vt->framer(ctx.self),
                                                   w->u.settings.p, w->u.settings.n);
    case HTTP2_WRITE_GO_AWAY:
        err = burrow__http2_framer_write_go_away(ctx.vt->framer(ctx.self),
                                                 w->u.go_away.max_stream_id,
                                                 w->u.go_away.code, no_debug_data);
        /* The error is not looked at, as the peer is being hung up on. */
        (void)ctx.vt->flush(ctx.self);
        return err;
    case HTTP2_WRITE_DATA:
        return burrow__http2_framer_write_data(ctx.vt->framer(ctx.self),
                                               w->u.data.stream_id,
                                               w->u.data.end_stream, w->u.data.p);
    case HTTP2_WRITE_HANDLER_PANIC_RST:
        return burrow__http2_framer_write_rst_stream(
            ctx.vt->framer(ctx.self), w->u.stream_id, HTTP2_ERR_CODE_INTERNAL);
    case HTTP2_WRITE_STREAM_ERROR:
        return burrow__http2_framer_write_rst_stream(ctx.vt->framer(ctx.self),
                                                     w->u.stream_error.stream_id,
                                                     w->u.stream_error.code);
    case HTTP2_WRITE_PING:
        return burrow__http2_framer_write_ping(ctx.vt->framer(ctx.self), false,
                                               w->u.ping);
    case HTTP2_WRITE_PING_ACK:
        return burrow__http2_framer_write_ping(ctx.vt->framer(ctx.self), true,
                                               w->u.ping);
    case HTTP2_WRITE_SETTINGS_ACK:
        return burrow__http2_framer_write_settings_ack(ctx.vt->framer(ctx.self));
    case HTTP2_WRITE_RES_HEADERS:
    case HTTP2_WRITE_PUSH_PROMISE:
        /* Lower case copies of names, and the push's :path, go here. */
        memset(&ar, 0, sizeof ar);
        if (w->kind == HTTP2_WRITE_RES_HEADERS)
            err = hw_write_res_headers(w->u.res_headers, ctx, arena_allocator(&ar));
        else
            err = hw_write_push_promise(w->u.push_promise, ctx, arena_allocator(&ar));
        arena_free(&ar);
        return err;
    case HTTP2_WRITE_100_CONTINUE:
        return hw_write_100_continue(w->u.stream_id, ctx);
    case HTTP2_WRITE_WINDOW_UPDATE:
        return burrow__http2_framer_write_window_update(ctx.vt->framer(ctx.self),
                                                        w->u.window_update.stream_id,
                                                        w->u.window_update.n);
    default:
        break;
    }
    panic_str(BURROW_S("http2: unknown writeFramer"));
}
