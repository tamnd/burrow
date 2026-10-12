/* The parts of net/http that are not exported but that the rest of the package
 * and Go's tests use.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_HTTP_INTERNAL_H
#define BURROW_SRC_NET_HTTP_INTERNAL_H

#include "burrow/net/http.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stdint.h>

/* internal.SniffLen, the most bytes DetectContentType looks at. */
#define BURROW__HTTP_SNIFF_LEN 512

/* Header.get and has: the key as it is, without making it canonical first. */
BURROW_BORROWS(ret, h) Str burrow__http_header_get(HttpHeader h, Str key);
bool burrow__http_header_has(HttpHeader h, Str key);

/* cloneOrMakeHeader. http_header_clone, but an empty header and not NULL for a
 * NULL h. NULL only when a says no. */
BURROW_OWNS(ret) HttpHeader burrow__http_clone_or_make_header(Alloc *a, HttpHeader h);

/* hasToken. Whether v, a header value, has token in it, as a whole word with
 * the ASCII letters folded. The words are separated by spaces, tabs and
 * commas. */
bool burrow__http_has_token(Str v, Str token);

/* removePort. host without the ":port" at its end, when it has one. A
 * bracketed IPv6 address keeps its brackets. The result points into host. */
BURROW_BORROWS(ret, host) Str burrow__http_remove_port(Str host);

/* isToken, which is httpguts.ValidHeaderFieldName. */
bool burrow__http_is_token(Str v);

/* stringContainsCTLByte. Whether s has an ASCII control byte, below space or
 * DEL. */
bool burrow__http_string_contains_ctl_byte(Str s);

/* hexEscapeNonASCII. s with each byte from 0x80 up as "%" and its value in
 * lower case hex. s itself when it has none, and a copy in a otherwise, which
 * is empty when a says no. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__http_hex_escape_non_ascii(Alloc *a,
                                                                              Str s);

/* ------------------------------------------------------------- chunked
 *
 * net/http/internal's chunked encoding, the wire format of a body sent with
 * Transfer-Encoding: chunked. */

/* internal.ErrLineTooLong, "header line too long". */
extern const Error burrow__http_err_line_too_long;

/* parseHexUint. A chunk's size from its hex digits, at most 16 of them. */
uint64_t burrow__http_parse_hex_uint(Slice v, Error *err);

/* NewChunkedReader's reader. It reads the chunks from r and hands back the
 * data in them, and gives io_eof after the last chunk, which is the one of no
 * bytes, leaving any trailer that follows it to be read from r. */
typedef struct HttpChunkedReader HttpChunkedReader;

/* NewChunkedReader. A BufioReader r is read from as it is, and anything else
 * through a new one. NULL when a says no. */
BURROW_OWNS(ret) HttpChunkedReader *burrow__http_new_chunked_reader(Alloc *a,
                                                                    IoReader r);
void burrow__http_chunked_reader_free(Alloc *a, HttpChunkedReader *cr);
Int burrow__http_chunked_reader_read(HttpChunkedReader *cr, Slice b, Error *err);
BURROW_BORROWS(ret, cr) IoReader
burrow__http_chunked_reader_as_io_reader(HttpChunkedReader *cr);

/* NewChunkedWriter's writer. Each write goes to wire as one chunk, and closing
 * it writes the chunk of no bytes that ends the body, but not the trailer or
 * the blank line after that. When flush is set, which is Go's
 * FlushAfterChunkWriter, flush is wire's BufioWriter and is flushed after each
 * chunk. Make one with {wire} or {wire, flush}. */
typedef struct HttpChunkedWriter {
    IoWriter wire;
    BufioWriter *flush;
} HttpChunkedWriter;

Int burrow__http_chunked_writer_write(HttpChunkedWriter *cw, Slice data, Error *err);
BURROW_STATIC(ret) Error burrow__http_chunked_writer_close(HttpChunkedWriter *cw);
BURROW_BORROWS(ret, cw) IoWriteCloser
burrow__http_chunked_writer_as_io_write_closer(HttpChunkedWriter *cw);

/* The parts of Protocols that are not exported. */
bool burrow__http_protocols_http3(HttpProtocols p);
void burrow__http_protocols_set_http3(HttpProtocols *p, bool ok);
bool burrow__http_protocols_empty(HttpProtocols p);

#endif /* BURROW_SRC_NET_HTTP_INTERNAL_H */
