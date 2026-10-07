/* net/http/httputil, HTTP helpers that net/http leaves out: dumping requests
 * and responses as they go over the wire, and the chunked encoding on its own.
 *
 * A dump is the bytes of the request or response, for debugging:
 *
 *     Slice dump = httputil_dump_request(a, req, true, &err);
 *     if (!BURROW_FAILED(err))
 *         fmt_printf_v("%s", str_from_bytes(dump.p, dump.len));
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/httputil */

#ifndef BURROW_NET_HTTP_HTTPUTIL_H
#define BURROW_NET_HTTP_HTTPUTIL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/own.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ chunked */

/* httputil.ErrLineTooLong, "header line too long", from a chunked reader
 * given a line longer than it will read. It is the same error net/http gives
 * for the same thing, so errors_is matches either. */
extern const Error httputil_err_line_too_long;

/* httputil.NewChunkedReader. A reader of the data in the chunks that r has,
 * the format of a body sent with Transfer-Encoding: chunked. It gives io_eof
 * after the chunk of no bytes that ends the body, and leaves any trailer after
 * that in r. A BufioReader is read from as it is, and anything else through a
 * new one made in a. net/http does this for itself, so this is only for a
 * program that reads chunks some other way. A nil reader when a says no, and
 * httputil_chunked_reader_free gives the memory back. */
IoReader httputil_new_chunked_reader(Alloc *a, IoReader r);
void httputil_chunked_reader_free(Alloc *a, IoReader r);

/* httputil.NewChunkedWriter. Each write goes to w as a chunk, a write of no
 * bytes writes nothing, and closing it writes the "0\r\n" of the last chunk.
 * The trailer and the "\r\n" after it are left to the caller to write. A nil
 * writer when a says no, and httputil_chunked_writer_free gives the memory
 * back. */
IoWriteCloser httputil_new_chunked_writer(Alloc *a, IoWriter w);
void httputil_chunked_writer_free(Alloc *a, IoWriteCloser w);

/* -------------------------------------------------------------------- dumps
 *
 * The dumps are made in a. When body is true each of them reads the whole
 * body into memory from a, closes it, and sets the body to a reader of those
 * bytes again, so the request or response can still be used after. a has to
 * last as long as that body does, which with an arena is no trouble. A dump
 * is nil on an error. */

/* httputil.DumpRequest. req as a server got it: the request line with the
 * method, which is GET when it is "", request_uri or the URL's own, and
 * proto_major and proto_minor; then the Host field unless request_uri is an
 * absolute URL, Transfer-Encoding, and the header without Host,
 * Transfer-Encoding and Trailer. A body comes after in chunks when
 * transfer_encoding starts with "chunked". A request about to be sent is
 * better dumped with httputil_dump_request_out. */
BURROW_OWNS(ret) Slice httputil_dump_request(Alloc *a, HttpRequest *req, bool body,
                                             Error *err);

/* httputil.DumpRequestOut. req as an HttpTransport would send it, with the
 * User-Agent, Accept-Encoding and other fields the transport adds. It is sent
 * through a transport to a connection that only records it, and an "https"
 * request goes as "http" so nothing tries to start TLS. When body is false a
 * body is still sent, as that many "x" bytes, so the header is right, and is
 * then cut off the dump. */
BURROW_OWNS(ret) Slice httputil_dump_request_out(Alloc *a, HttpRequest *req, bool body,
                                                 Error *err);

/* httputil.DumpResponse. resp as http_response_write writes it. When body is
 * false the body is left out, but content_length stays what it was, so the
 * header has the length the body would have had. */
BURROW_OWNS(ret) Slice httputil_dump_response(Alloc *a, HttpResponse *resp, bool body,
                                              Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_HTTPUTIL_H */
