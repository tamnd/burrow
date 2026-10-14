/* net/http/httptest, for testing HTTP handlers.
 *
 * Go's net/http/httptest without its Server, which needs the net/http server
 * and comes with it. What is here is enough to test a handler without a
 * network: httptest_new_request makes a request the way a server would hand
 * one to a handler, and an HttptestResponseRecorder is a response writer that
 * keeps what the handler did with it.
 *
 *     HttpRequest *r = httptest_new_request(a, BURROW_S("GET"),
 *                                           BURROW_S("/hello"), (IoReader){0});
 *     HttptestResponseRecorder *rec = httptest_new_recorder(a);
 *     http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec), r);
 *     HttpResponse *res = httptest_response_recorder_result(rec);
 *     // res->status_code, res->header and res->body are what h wrote
 *     httptest_response_recorder_free(rec);
 *     http_request_free(r);
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/httptest */

#ifndef BURROW_NET_HTTP_HTTPTEST_H
#define BURROW_NET_HTTP_HTTPTEST_H

#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/own.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* httptest.DefaultRemoteAddr, the remote address Go's test server gives a
 * request when nothing else says what it is. */
#define HTTPTEST_DEFAULT_REMOTE_ADDR BURROW_S("1.2.3.4")

/* ---------------------------------------------------------------- NewRequest */

/* httptest.NewRequestWithContext. A request for a handler under test, as a
 * server would have read it: target is read as the target of a request line,
 * so it is a path such as "/x?y=1" or an absolute URL such as
 * "http://foo.com/x", and an empty method is GET. The request is HTTP/1.1, its
 * remote_addr is "192.0.2.1:1234", from the range RFC 5737 keeps for examples,
 * and its host is example.com unless target names one.
 *
 * A body that is a BytesBuffer, a BytesReader or a StringsReader gives the
 * request a content_length of what is left in it, http_no_body gives 0, and any
 * other reader gives -1. The body is closed through an io_nop_closer, since an
 * IoReader cannot say whether it can be closed as well, so set body yourself
 * when the handler has to close something. A nil body is http_no_body.
 *
 * Go's request also has TLS state when target starts with "https://". There is
 * no crypto/tls here yet, so it does not.
 *
 * Panics with "invalid NewRequest arguments; " and the reason when method and
 * target do not make a request line, as Go's does. The request is made in a
 * and goes back with http_request_free. */
BURROW_OWNS(ret) HttpRequest *httptest_new_request_with_context(Alloc *a, Context ctx,
                                                                Str method, Str target,
                                                                IoReader body);

/* httptest.NewRequest, with context_background() for the context. */
BURROW_OWNS(ret) HttpRequest *httptest_new_request(Alloc *a, Str method, Str target,
                                                   IoReader body);

/* ---------------------------------------------------------- ResponseRecorder */

/* httptest.ResponseRecorder. A response writer that records what a handler
 * does with it, for the test to look at afterwards.
 *
 * code is the status the handler wrote, and 200 when it wrote none. It can be
 * 0 only when the test sets it so, and httptest_response_recorder_result gives
 * 200 for that. header_map is the header as the handler has left it, which Go
 * keeps only for older tests: the header the response went out with is the
 * one in the result. body is where the writes go, and when it is NULL they go
 * nowhere. flushed says whether the handler called flush.
 *
 * The header keeps values as they were given, as every HttpHeader does, so a
 * value the handler sets has to last as long as the recorder is looked at. */
typedef struct HttptestResponseRecorder {
    Int code;
    HttpHeader header_map;
    BytesBuffer *body;
    bool flushed;

    /* The recorder's own. */
    bool wrote_header;
    Alloc *a;
    Arena arena;
    HttpResponse *result;
    HttpHeader snap_header;
    BytesBuffer *own_body;
} HttptestResponseRecorder;

extern const Type *const TYPE_HTTPTEST_RESPONSE_RECORDER;

/* httptest.NewRecorder. A recorder with an empty header, an empty body and a
 * code of 200, made in a. NULL when a says no. Give it back with
 * httptest_response_recorder_free. */
BURROW_OWNS(ret) HttptestResponseRecorder *httptest_new_recorder(Alloc *a);

/* Gives back the recorder, its body if the recorder made it, and the result.
 * NULL is fine. */
void httptest_response_recorder_free(HttptestResponseRecorder *rw);

/* The recorder as the HttpResponseWriter a handler takes. Its self_type is
 * TYPE_HTTPTEST_RESPONSE_RECORDER, so iface_assert gets the recorder back out
 * of it, and it has the WriteString method io_write_string looks for. */
HttpResponseWriter
httptest_response_recorder_as_response_writer(HttptestResponseRecorder *rw);

/* ResponseRecorder.Header. header_map, which is made first if the test set it
 * to NULL. */
BURROW_BORROWS(ret, rw) HttpHeader
httptest_response_recorder_header(HttptestResponseRecorder *rw);

/* ResponseRecorder.Write and WriteString. The first write sends a status of
 * 200, and a Content-Type found from the bytes by http_detect_content_type when
 * the header has none and has no Transfer-Encoding either. The bytes go to body
 * whatever the status, but after a 1xx, 204 or 304 the result is 0 and
 * http_err_body_not_allowed, since such a response has no body. */
Int httptest_response_recorder_write(HttptestResponseRecorder *rw, Slice buf,
                                     Error *err);
Int httptest_response_recorder_write_string(HttptestResponseRecorder *rw, Str str,
                                            Error *err);

/* ResponseRecorder.WriteHeader. Records code and a copy of the header as it is
 * now, the first time, and does nothing after that. Panics with "invalid
 * WriteHeader code" and the code when code is not from 100 to 999, as a server
 * does. */
void httptest_response_recorder_write_header(HttptestResponseRecorder *rw, Int code);

/* ResponseRecorder.Flush, http.Flusher's method. Sends a 200 if nothing has
 * been written, and sets flushed. */
void httptest_response_recorder_flush(HttptestResponseRecorder *rw);

/* ResponseRecorder.Result. The response the handler made, to be called once
 * the handler has returned. The same response every time, made the first time.
 *
 * Its header is the one as it was at the first write, or at this call when
 * nothing was written, and its trailer is the keys a Trailer field named, and
 * keys that start with HTTP_TRAILER_PREFIX, as they are now. Its body reads
 * what was written to body, or nothing when body is NULL, and gives no error
 * but io_err_eof. content_length is the Content-Length field, or -1 when there
 * is none or it is not a number.
 *
 * The response belongs to the recorder and goes when the recorder does, so it
 * is not for http_response_free. Its body reads the bytes in body, which
 * writing to body after this call can move. NULL when the recorder's allocator
 * says no. */
BURROW_BORROWS(ret, rw) HttpResponse *
httptest_response_recorder_result(HttptestResponseRecorder *rw);

/* httptest's parseContentLength, which Result uses: s without the spaces at
 * either end as a decimal number from 0 to 2^63-1, or -1 when it is not one.
 * Exported for the tests. */
int64_t burrow__httptest_parse_content_length(Str s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_HTTPTEST_H */
