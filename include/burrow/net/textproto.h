/* net/textproto, the line based protocols of HTTP, NNTP and SMTP.
 *
 * Go's net/textproto. A TextprotoReader reads lines, numbered responses such
 * as "220 ready", dot encoded blocks that end in a line holding a single "."
 * and MIME style headers. A TextprotoWriter writes lines and dot encoded
 * blocks. A TextprotoPipeline keeps the requests and responses on one
 * connection in order when several goroutines share it, and a TextprotoConn
 * is the three together over one connection.
 *
 *     BufioReader *br = bufio_new_reader(a, conn_reader);
 *     TextprotoReader *r = textproto_new_reader(a, br);
 *     Error err;
 *     Str msg;
 *     Int code = textproto_reader_read_response(r, a, 2, &msg, &err);
 *     TextprotoMIMEHeader h = textproto_reader_read_mime_header(r, a, &err);
 *     Str type = textproto_mime_header_get(h, BURROW_S("content-type"));
 *
 * What a read returns, strings, lines and headers, comes from the allocator
 * the call is given. An arena per message is the easy way to hold it. The
 * reader and writer themselves come from the allocator they were made with.
 *
 * Go's Dial needs the net package, which is not ported yet. textproto_new_conn
 * takes any IoReadWriteCloser in the meantime.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/textproto */

#ifndef BURROW_NET_TEXTPROTO_H
#define BURROW_NET_TEXTPROTO_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- errors */

/* textproto.Error, a response that came with a code the caller did not
 * expect. Its text is the code in three digits and the message quoted, such
 * as `550 "no such user"`. errors_as with TYPE_TEXTPROTO_ERROR gets you the
 * struct. */
typedef struct TextprotoError {
    Int code;
    Str msg;
} TextprotoError;

extern const Type *const TYPE_TEXTPROTO_ERROR;

/* textproto.Error.Error, built in a. */
BURROW_OWNS(ret) Str textproto_error_error(const TextprotoError *e, Alloc *a);

/* The Error for e, with msg copied into a. burrow_err_out_of_memory when a
 * says no. */
BURROW_OWNS(ret) Error textproto_error_as_error(const TextprotoError *e, Alloc *a);

/* textproto.ProtocolError, a response that does not follow the protocol, such
 * as a short line or a header with no colon. It is its own text. Two with the
 * same text are errors_is each other, as the Go values are ==, and errors_as
 * with TYPE_TEXTPROTO_PROTOCOL_ERROR gives a Str *. */
typedef Str TextprotoProtocolError;

extern const Type *const TYPE_TEXTPROTO_PROTOCOL_ERROR;

/* textproto.ProtocolError.Error, which is p. */
BURROW_BORROWS(ret, p) Str textproto_protocol_error_error(TextprotoProtocolError p);

/* The Error for p, copied into a. */
BURROW_OWNS(ret) Error textproto_protocol_error_as_error(TextprotoProtocolError p,
                                                         Alloc *a);

/* ---------------------------------------------------------------- MIMEHeader */

/* textproto.MIMEHeader, map[string][]string: each header key to its values, in
 * the order they came. The keys are in canonical form, "Content-Type" and not
 * "content-type", and the functions here put the key they are given into that
 * form first. It is a Map from Str to a Slice of Str, so map_len, map_iter and
 * the rest work on it too, and NULL reads as empty. */
typedef Map *TextprotoMIMEHeader;

/* make(textproto.MIMEHeader). NULL when a says no. */
BURROW_OWNS(ret) TextprotoMIMEHeader textproto_mime_header_make(Alloc *a);

/* MIMEHeader.Add and Set. Add puts value after the ones key has, and Set makes
 * it the only one. value is kept as it is. key is kept too when it is already
 * canonical, and otherwise the canonical form is made in the allocator h was
 * made with, which the value slices come from as well. False when that
 * allocator says no, and then h is as it was. */
bool textproto_mime_header_add(TextprotoMIMEHeader h, Str key, Str value);
bool textproto_mime_header_set(TextprotoMIMEHeader h, Str key, Str value);

/* MIMEHeader.Get. The first value for key, or "" when there is none. */
BURROW_BORROWS(ret, h) Str textproto_mime_header_get(TextprotoMIMEHeader h, Str key);

/* MIMEHeader.Values. Every value for key, which points into h and is good until
 * h next changes. An empty Slice when there are none. */
BURROW_BORROWS(ret, h) Slice textproto_mime_header_values(TextprotoMIMEHeader h,
                                                          Str key);

/* MIMEHeader.Del. */
void textproto_mime_header_del(TextprotoMIMEHeader h, Str key);

/* textproto.CanonicalMIMEHeaderKey. The first letter and each letter after a
 * hyphen in upper case and the rest in lower case, so "accept-encoding" gives
 * "Accept-Encoding". A key with a space or a byte a header name cannot have
 * comes back as it is. The result is s when s is already canonical, a static
 * string for the common headers Go keeps a table of, and a copy in a
 * otherwise. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str
textproto_canonical_mime_header_key(Alloc *a, Str s);

/* ------------------------------------------------------------------- Reader */

/* textproto.Reader. r is the buffered reader underneath, Go's R. The rest is
 * the reader's own, and a zeroed one with r set works, with the heap for the
 * buffer it keeps. */
typedef struct TextprotoReader {
    BufioReader *r;
    Alloc *a;
    Byte *buf; /* a continued line being put together */
    Int buf_len, buf_cap;
    Byte *line; /* a line bufio gave in pieces */
    Int line_len, line_cap;
    int dot_state;
    bool dot_open; /* a dot reader is part way through a block */
    bool own;      /* the struct came from a */
} TextprotoReader;

/* textproto.NewReader. The reader comes from a, and NULL when a says no. To
 * avoid denial of service attacks, r should be reading from something with a
 * limit on its size, such as an IoLimitedReader. */
BURROW_OWNS(ret) TextprotoReader *textproto_new_reader(Alloc *a, BufioReader *r);

/* Gives back the reader and the buffer it keeps, but not r. NULL is fine. */
void textproto_reader_free(TextprotoReader *r);

/* Reader.ReadLine. One line, with the "\n" or "\r\n" taken off, copied into a.
 * At the end of the input it gives "" and io_eof. */
BURROW_OWNS(ret) Str textproto_reader_read_line(TextprotoReader *r, Alloc *a,
                                                Error *err);

/* Reader.ReadLineBytes. The same as a Slice of Byte. */
BURROW_OWNS(ret) Slice textproto_reader_read_line_bytes(TextprotoReader *r, Alloc *a,
                                                        Error *err);

/* Reader.ReadContinuedLine. A line and the lines after it that start with a
 * space or a tab, which continue it, joined by single spaces and trimmed. So
 *
 *     Line 1
 *       continued...
 *     Line 2
 *
 * reads as "Line 1 continued..." and then "Line 2". An empty line has no
 * continuation. */
BURROW_OWNS(ret) Str textproto_reader_read_continued_line(TextprotoReader *r, Alloc *a,
                                                          Error *err);

/* Reader.ReadContinuedLineBytes. The same as a Slice of Byte. */
BURROW_OWNS(ret) Slice textproto_reader_read_continued_line_bytes(TextprotoReader *r,
                                                                  Alloc *a, Error *err);

/* Reader.ReadCodeLine. A response line such as "220 plan9.bell-labs.com ESMTP",
 * whose code it returns and whose message it puts in *message. A code that
 * does not match expect_code is a TextprotoError, and the code and message
 * still come back. expect_code 3 takes any 3xx code, 31 any 31x and 310 only
 * 310, and 0 or less takes any code. A line such as "220-more" that says more
 * lines follow is a TextprotoProtocolError, and read_response is for those. */
Int textproto_reader_read_code_line(TextprotoReader *r, Alloc *a, Int expect_code,
                                    Str *message, Error *err);

/* Reader.ReadResponse. A response of one or more lines, per RFC 959:
 *
 *     code-message line 1
 *     code-message line 2
 *     ...
 *     code message line n
 *
 * The message is the lines joined by "\n" without their codes, and lines in
 * the middle that do not start with the code are kept as they are. expect_code
 * works as in read_code_line, and a mismatch is a TextprotoError holding the
 * whole message. */
Int textproto_reader_read_response(TextprotoReader *r, Alloc *a, Int expect_code,
                                   Str *message, Error *err);

/* Reader.DotReader. A reader for a dot encoded block, the way SMTP sends a
 * message body: lines end in "\r\n", a line starting with "." has another "."
 * put in front, and a line holding just "." ends the block. It undoes that,
 * giving lines ending in "\n", and gives io_eof after the "." line, or
 * io_err_unexpected_eof if the input ends first.
 *
 * The next call on r reads what is left of the block and throws it away. A
 * TextprotoReader has one dot reader, which each call here starts over, so an
 * IoReader from an earlier call reads the newest block and not its own, which
 * is where this differs from Go. */
IoReader textproto_reader_dot_reader(TextprotoReader *r);

/* Reader.ReadDotBytes. A dot encoded block decoded, in a. */
BURROW_OWNS(ret) Slice textproto_reader_read_dot_bytes(TextprotoReader *r, Alloc *a,
                                                       Error *err);

/* Reader.ReadDotLines. A dot encoded block as a Slice of Str, one per line,
 * without the line endings or the final "." line. */
BURROW_OWNS(ret) Slice textproto_reader_read_dot_lines(TextprotoReader *r, Alloc *a,
                                                       Error *err);

/* Reader.ReadMIMEHeader. A header block such as
 *
 *     My-Key: Value 1
 *     Long-Key: Even
 *            Longer Value
 *     My-Key: Value 2
 *
 * up to the blank line after it, as a TextprotoMIMEHeader with
 * "My-Key" => {"Value 1", "Value 2"} and "Long-Key" => {"Even Longer Value"}.
 * Keys that are not already canonical are put in canonical form. On an error
 * the header holds the lines read before it. */
BURROW_OWNS(ret) TextprotoMIMEHeader
textproto_reader_read_mime_header(TextprotoReader *r, Alloc *a, Error *err);

/* What net/http calls through Go's linkname: read_mime_header with a limit on
 * the bytes and on the number of headers, past which it stops with an error
 * whose text is "message too large". */
BURROW_OWNS(ret) TextprotoMIMEHeader burrow__textproto_read_mime_header(
    TextprotoReader *r, Alloc *a, int64_t max_memory, int64_t max_headers, Error *err);

/* For the tests, which Go runs inside the package: how many header lines are
 * already buffered, and the i'th entry of the common header table, or "" past
 * its end. */
Int burrow__textproto_upcoming_header_keys(TextprotoReader *r);
BURROW_STATIC(ret) Str burrow__textproto_common_header(Int i);

/* ------------------------------------------------------------------- Writer */

/* textproto.Writer. w is the buffered writer underneath, Go's W. The rest is
 * the writer's own, and a zeroed one with w set works. */
typedef struct TextprotoWriter {
    BufioWriter *w;
    Alloc *a;
    int dot_state;
    bool dot_open;
    bool own;
} TextprotoWriter;

/* textproto.NewWriter. The writer comes from a, and NULL when a says no. */
BURROW_OWNS(ret) TextprotoWriter *textproto_new_writer(Alloc *a, BufioWriter *w);

/* Gives back the writer, but not w. NULL is fine. */
void textproto_writer_free(TextprotoWriter *w);

/* Writer.PrintfLine. The line formatted as fmt_fprintf does, then "\r\n", then
 * a flush. The error is the flush's. */
BURROW_STATIC(ret) Error textproto_writer_printf_line(TextprotoWriter *w, Str format,
                                                      Slice args);

#define textproto_writer_printf_line_v(w, ...)                                         \
    textproto_writer_printf_line((w), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* Writer.DotWriter. A writer that dot encodes what goes through it: "\n"
 * becomes "\r\n", a line starting with "." gets another in front, and closing
 * it ends the line if it is open, writes the "." line and flushes. Close it
 * before the next call on w, or that call closes it for you. A TextprotoWriter
 * has one dot writer, which each call here starts over, and closing one that
 * is already closed does nothing. */
IoWriteCloser textproto_writer_dot_writer(TextprotoWriter *w);

/* ----------------------------------------------------------------- Pipeline */

/* The half of a Pipeline that hands out turns in order. Not for touching. */
typedef struct burrow__TextprotoSequencer {
    SyncMutex mu;
    SyncCond cond;
    Uint id;
    bool ready;
} burrow__TextprotoSequencer;

/* textproto.Pipeline, which keeps the requests and responses on a connection
 * in order when several goroutines take turns on it. A client does
 *
 *     Uint id = textproto_pipeline_next(p);
 *     textproto_pipeline_start_request(p, id);
 *     ... send the request ...
 *     textproto_pipeline_end_request(p, id);
 *     textproto_pipeline_start_response(p, id);
 *     ... read the response ...
 *     textproto_pipeline_end_response(p, id);
 *
 * and a server the same with its reads and writes swapped. The zero value is
 * ready to use. */
typedef struct TextprotoPipeline {
    SyncMutex mu;
    Uint id;
    burrow__TextprotoSequencer request;
    burrow__TextprotoSequencer response;
} TextprotoPipeline;

/* Pipeline.Next, the id of the next request/response pair. */
Uint textproto_pipeline_next(TextprotoPipeline *p);

/* Pipeline.StartRequest and StartResponse wait until it is id's turn to send
 * or to receive, and EndRequest and EndResponse say id is done and hand the
 * turn on. Ending an id whose turn it is not panics, as in Go. */
void textproto_pipeline_start_request(TextprotoPipeline *p, Uint id);
void textproto_pipeline_end_request(TextprotoPipeline *p, Uint id);
void textproto_pipeline_start_response(TextprotoPipeline *p, Uint id);
void textproto_pipeline_end_response(TextprotoPipeline *p, Uint id);

/* --------------------------------------------------------------------- Conn */

/* textproto.Conn, a reader, a writer and a pipeline over one connection. Go
 * embeds the three and calls their methods on the Conn. The textproto_conn_
 * functions after textproto_conn_cmd are those promoted methods, and calling
 * the same function on the field, as textproto_reader_read_line(&c->reader,
 * ...), is the same thing. */
typedef struct TextprotoConn {
    TextprotoReader reader;
    TextprotoWriter writer;
    TextprotoPipeline pipeline;
    IoReadWriteCloser conn;
    Alloc *a;
} TextprotoConn;

/* textproto.NewConn. The Conn and its buffered reader and writer come from a,
 * and NULL when a says no. */
BURROW_OWNS(ret) TextprotoConn *textproto_new_conn(Alloc *a, IoReadWriteCloser conn);

/* Conn.Close, which closes the connection. It does not free c. */
BURROW_STATIC(ret) Error textproto_conn_close(TextprotoConn *c);

/* Gives back c and its buffers without closing the connection. NULL is
 * fine. */
void textproto_conn_free(TextprotoConn *c);

/* Conn.Cmd. Waits for its turn, sends the formatted line, and returns the id
 * to hand to start_response and end_response when reading the answer:
 *
 *     Uint id = textproto_conn_cmd_v(c, &err, "USER %s", user);
 *     textproto_conn_start_response(c, id);
 *     Int code = textproto_conn_read_code_line(c, a, 331, &msg, &err);
 *     textproto_conn_end_response(c, id);
 *
 * On an error it gives 0 and the error. */
Uint textproto_conn_cmd(TextprotoConn *c, Str format, Slice args, Error *err);

#define textproto_conn_cmd_v(c, err, ...)                                              \
    textproto_conn_cmd((c), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__), (err))

/* The methods Conn gets from its Reader, Writer and Pipeline. */
BURROW_OWNS(ret) static inline Str textproto_conn_read_line(TextprotoConn *c, Alloc *a,
                                                            Error *err) {
    return textproto_reader_read_line(&c->reader, a, err);
}
BURROW_OWNS(ret) static inline Slice
textproto_conn_read_line_bytes(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_line_bytes(&c->reader, a, err);
}
BURROW_OWNS(ret) static inline Str
textproto_conn_read_continued_line(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_continued_line(&c->reader, a, err);
}
BURROW_OWNS(ret) static inline Slice
textproto_conn_read_continued_line_bytes(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_continued_line_bytes(&c->reader, a, err);
}
static inline Int textproto_conn_read_code_line(TextprotoConn *c, Alloc *a,
                                                Int expect_code, Str *message,
                                                Error *err) {
    return textproto_reader_read_code_line(&c->reader, a, expect_code, message, err);
}
static inline Int textproto_conn_read_response(TextprotoConn *c, Alloc *a,
                                               Int expect_code, Str *message,
                                               Error *err) {
    return textproto_reader_read_response(&c->reader, a, expect_code, message, err);
}
static inline IoReader textproto_conn_dot_reader(TextprotoConn *c) {
    return textproto_reader_dot_reader(&c->reader);
}
BURROW_OWNS(ret) static inline Slice
textproto_conn_read_dot_bytes(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_dot_bytes(&c->reader, a, err);
}
BURROW_OWNS(ret) static inline Slice
textproto_conn_read_dot_lines(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_dot_lines(&c->reader, a, err);
}
static inline TextprotoMIMEHeader
textproto_conn_read_mime_header(TextprotoConn *c, Alloc *a, Error *err) {
    return textproto_reader_read_mime_header(&c->reader, a, err);
}
BURROW_STATIC(ret) static inline Error
textproto_conn_printf_line(TextprotoConn *c, Str format, Slice args) {
    return textproto_writer_printf_line(&c->writer, format, args);
}
#define textproto_conn_printf_line_v(c, ...)                                           \
    textproto_conn_printf_line((c), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
static inline IoWriteCloser textproto_conn_dot_writer(TextprotoConn *c) {
    return textproto_writer_dot_writer(&c->writer);
}
static inline Uint textproto_conn_next(TextprotoConn *c) {
    return textproto_pipeline_next(&c->pipeline);
}
static inline void textproto_conn_start_request(TextprotoConn *c, Uint id) {
    textproto_pipeline_start_request(&c->pipeline, id);
}
static inline void textproto_conn_end_request(TextprotoConn *c, Uint id) {
    textproto_pipeline_end_request(&c->pipeline, id);
}
static inline void textproto_conn_start_response(TextprotoConn *c, Uint id) {
    textproto_pipeline_start_response(&c->pipeline, id);
}
static inline void textproto_conn_end_response(TextprotoConn *c, Uint id) {
    textproto_pipeline_end_response(&c->pipeline, id);
}

/* ------------------------------------------------------------------- helpers */

/* textproto.TrimString and TrimBytes. s without the ASCII spaces, tabs, "\r"
 * and "\n" at either end. The result points into s. */
BURROW_BORROWS(ret, s) Str textproto_trim_string(Str s);
BURROW_BORROWS(ret, b) Slice textproto_trim_bytes(Slice b);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_TEXTPROTO_H */
