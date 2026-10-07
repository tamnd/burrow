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
#include "burrow/func.h"
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

/* The cookie errors, which Go does not export but which its tests compare
 * against. */
extern const Error burrow__http_err_blank_cookie;
extern const Error burrow__http_err_equal_not_found_in_cookie;
extern const Error burrow__http_err_invalid_cookie_name;
extern const Error burrow__http_err_invalid_cookie_value;
extern const Error burrow__http_err_cookie_num_limit_exceeded;

/* defaultCookieMaxNum. */
#define BURROW__HTTP_DEFAULT_COOKIE_MAX_NUM 3000

/* readSetCookies. The cookies of every Set-Cookie value in h, with the ones
 * that do not parse left out, as a Slice of HttpCookie from a. Empty when
 * there are more of them than the cookie limit allows, and when a says no. */
BURROW_OWNS(ret) Slice burrow__http_read_set_cookies(Alloc *a, HttpHeader h);

/* readCookies. The cookies of every Cookie value in h, and only those named
 * filter when filter is not empty, as readSetCookies has them. */
BURROW_OWNS(ret) Slice burrow__http_read_cookies(Alloc *a, HttpHeader h, Str filter);

/* sanitizeCookieValue and sanitizeCookiePath. The result is v when nothing
 * changes, and from a otherwise. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) Str
burrow__http_sanitize_cookie_value(Alloc *a, Str v, bool quoted);
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) Str burrow__http_sanitize_cookie_path(Alloc *a,
                                                                              Str v);

/* htmlEscape, which is htmlReplacer: s with the five characters HTML cares
 * about as entities, &#34; and &#39; for the quotes. s itself when it has none
 * of them, and from a otherwise. Panics when a says no. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__http_html_escape(Alloc *a, Str s);

/* Sets GODEBUG as net/http sees it, for tests, the way t.Setenv does in Go.
 * NULL goes back to reading the environment. */
void burrow__http_godebug_set(const char *value);

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

/* ------------------------------------------------------------ transfer
 *
 * The reading half of transfer.go, which works out from a header how long a
 * body is and how it comes, and the body that reads it. */

/* badStringError, fmt.Errorf("%s %q", what, val). */
BURROW_BORROWS(ret) Error burrow__http_bad_string_error(Str what, Str val);

/* errTooLarge, "http: request too large". */
extern const Error burrow__http_err_too_large;

/* maxPostHandlerReadBytes, the most of a body a server reads past what its
 * handler did, to keep the connection. */
#define BURROW__HTTP_MAX_POST_HANDLER_READ_BYTES ((int64_t)256 << 10)

/* httplaxcontentlength=1 in GODEBUG, which takes an empty Content-Length as no
 * Content-Length at all. */
bool burrow__http_godebug_lax_content_length(void);

/* parseContentLength. The length in the first of the Content-Length values cl,
 * a Slice of Str, or -1 when there are none. */
int64_t burrow__http_parse_content_length(Slice cl, Error *err);

/* shouldClose. Whether the connection closes after a message with this
 * version and header. With remove_close_header, a "Connection: close" field
 * is taken out of an HTTP/1.1 header. */
bool burrow__http_should_close(Int major, Int minor, HttpHeader header,
                               bool remove_close_header);

/* transferReader.parseTransferEncoding. Sets chunked from the
 * Transfer-Encoding field of header, which it takes out of header. */
BURROW_BORROWS(ret) Error burrow__http_parse_transfer_encoding(HttpHeader header,
                                                               Int major, Int minor,
                                                               bool *chunked);

/* isUnsupportedTEError. Whether err is the error readTransfer gives for a
 * Transfer-Encoding it does not do. */
bool burrow__http_is_unsupported_te_error(Error err);

/* readTransfer. Works out the body of req or resp, whichever is not NULL,
 * from its header, and sets its body, content_length, transfer_encoding,
 * close and trailer. The body reads from r. A chunked body reads its trailer
 * with at most max_trailer_headers fields. */
BURROW_BORROWS(ret) Error burrow__http_read_transfer(HttpRequest *req,
                                                     HttpResponse *resp, BufioReader *r,
                                                     int64_t max_trailer_headers);

/* Frees the body read_transfer made, which is the wire field of the message.
 * NULL is fine. */
void burrow__http_body_free(void *body);

/* A body as Go's tests make one, reading src, and the trailer from r into
 * *trailer after src ends when trailer is not NULL. Its data is the body to
 * free, and it is zero when a says no. */
BURROW_OWNS(ret) IoReadCloser burrow__http_new_body(Alloc *a, IoReader src,
                                                    HttpHeader *trailer,
                                                    BufioReader *r);

/* body.didEarlyClose, body.bodyRemains and doEarlyClose, for the server. */
bool burrow__http_body_did_early_close(void *body);
bool burrow__http_body_remains(void *body);
void burrow__http_body_set_do_early_close(void *body, bool on);

/* body.Close, which is nothing for a NULL body. */
BURROW_BORROWS(ret) Error burrow__http_body_close(void *body);

/* body.registerOnHitEOF, which does nothing for a NULL body. fn runs, with the body's lock held, once a read has
 * come to the end of the body. */
void burrow__http_body_register_on_hit_eof(void *body, Func fn);

/* What the server asks of a request body when the handler is done: whether it
 * was closed, whether its end was read, and how much of its declared length is
 * left, which is -1 when it has no length (body.unreadDataSizeLocked). */
void burrow__http_body_state(void *body, bool *closed, bool *saw_eof, int64_t *unread);

/* bodyAllowedForStatus. Whether a response with this status may have a body,
 * which 1xx, 204 and 304 may not. */
bool burrow__http_body_allowed_for_status(Int status);

/* ------------------------------------------------------ transfer writer
 *
 * The writing half of transfer.go, which looks at a request or a response
 * about to go out, works out from its body, content_length and
 * transfer_encoding which framing it gets, and writes that framing and the
 * body. */

/* validateHeaders. "" when every key in hdrs is a valid field name and every
 * value a valid field value, and otherwise what is wrong, made in a, such as
 * `field name "X\r\n"`. A NULL hdrs is fine. */
BURROW_OWNS(ret) Str burrow__http_validate_headers(Alloc *a, HttpHeader hdrs);

/* What the probe of a request body found, for the body that hands it back. */
typedef struct burrow__HttpProbed {
    struct burrow__HttpProbe *async; /* a read still going on in a goroutine */
    IoReader rest;                   /* the reader to go on with */
    Error tail;                      /* given instead of reading rest, if set */
    bool have_byte;
    bool has_tail;
    Byte b;
} burrow__HttpProbed;

/* transferWriter. Made by burrow__http_new_transfer_writer, which needs no
 * freeing beyond burrow__http_transfer_writer_done. */
typedef struct burrow__HttpTransferWriter {
    Str method;
    IoReader body;            /* nil when there is none to write */
    IoReadCloser body_closer; /* closed by write_body, nil when none */
    int64_t content_length;   /* -1 for unknown, 0 for exactly none */
    Slice transfer_encoding;  /* of Str */
    HttpHeader header;
    HttpHeader trailer;
    Error body_read_error; /* any non-EOF error from copying body */
    burrow__HttpProbed probed;
    struct burrow__HttpProbe *probe; /* set when probeRequestBody gave up */
    bool response_to_head;
    bool close;
    bool is_response;
    bool flush_headers; /* flush the header to the network before the body */
} burrow__HttpTransferWriter;

/* newTransferWriter, from exactly one of req and resp. Probing a request body
 * may read its first byte, which the writer keeps and writes. Scratch memory
 * comes from a. */
BURROW_BORROWS(ret) Error burrow__http_new_transfer_writer(
    burrow__HttpTransferWriter *t, Alloc *a, HttpRequest *req, HttpResponse *resp);

/* Lets go of what probing the body left behind. Safe to call more than once,
 * and on a writer that failed to be made. */
void burrow__http_transfer_writer_done(burrow__HttpTransferWriter *t);

/* shouldSendContentLength, writeHeader and writeBody. write_body always closes
 * body_closer. */
bool burrow__http_transfer_writer_should_send_content_length(
    const burrow__HttpTransferWriter *t);
BURROW_BORROWS(ret) Error burrow__http_transfer_writer_write_header(
    burrow__HttpTransferWriter *t, Alloc *a, IoWriter w);
BURROW_BORROWS(ret) Error burrow__http_transfer_writer_write_body(
    burrow__HttpTransferWriter *t, Alloc *a, IoWriter w);

/* chunked and isIdentity, on a Slice of Str. */
bool burrow__http_chunked(Slice te);
bool burrow__http_is_identity(Slice te);

/* The writer behind w when it is a BufioWriter, and NULL otherwise. */
BURROW_BORROWS(ret, w) BufioWriter *burrow__http_bufio_writer_of(IoWriter w);

/* Header.writeSubset with the keys in exclude, which are canonical, left out. */
BURROW_BORROWS(ret) Error burrow__http_header_write_except(HttpHeader h, IoWriter w,
                                                           const Str *exclude,
                                                           Int nexclude);

/* requestBodyReadError. Request.write gives an error from reading the body
 * wrapped in one, so that the transport can tell it from a network error. Its
 * message is the wrapped error's, and it does not unwrap, as in Go. */
BURROW_OWNS(ret) Error burrow__http_request_body_read_error(Error inner);
bool burrow__http_is_request_body_read_error(Error err, Error *inner);

/* waitForContinue, which says whether to go on and send the body. */
BURROW_FUNC0(burrow__HttpWaitFunc, bool);

/* Request.write. extra may be NULL and wait nil. */
BURROW_BORROWS(ret) Error burrow__http_request_write(HttpRequest *r, IoWriter w,
                                                     bool using_proxy, HttpHeader extra,
                                                     burrow__HttpWaitFunc wait);

/* outgoingLength. The Content-Length of a client request, with 0 taken as
 * unknown, -1, when the body is not nil or http_no_body. */
int64_t burrow__http_request_outgoing_length(const HttpRequest *r);

/* requestMethodUsuallyLacksBody. */
bool burrow__http_request_method_usually_lacks_body(Str method);

/* readRequestLimit. readRequest, which is http_read_request without taking
 * Host out of the header, with a limit on the header fields. */
BURROW_OWNS(ret) HttpRequest *burrow__http_read_request_limit(Alloc *a, BufioReader *b,
                                                              int64_t max_headers,
                                                              Error *err);

/* fixPragmaCacheControl. "Pragma: no-cache" means "Cache-Control: no-cache"
 * when there is no Cache-Control. False when the header's allocator says no. */
bool burrow__http_fix_pragma_cache_control(HttpHeader header);

/* parseBasicAuth. The user name and password in "Basic " and their base64,
 * decoded into a. */
bool burrow__http_parse_basic_auth(Alloc *a, Str auth, Str *username, Str *password);

/* The parts of Protocols that are not exported. */
bool burrow__http_protocols_http3(HttpProtocols p);
void burrow__http_protocols_set_http3(HttpProtocols *p, bool ok);
bool burrow__http_protocols_empty(HttpProtocols p);

/* ----------------------------------------------------------- ServeMux
 *
 * What Go's tests for the mux reach inside it for. */

struct burrow__HttpRoutingNode;
struct burrow__HttpPattern;

/* httpmuxgo121=1 in GODEBUG, which gives every mux the one Go 1.21 had. */
bool burrow__http_godebug_mux121(void);

/* redirectHandler, what http_redirect_handler makes. */
typedef struct burrow__HttpRedirectHandler {
    Str url;
    Int code;
} burrow__HttpRedirectHandler;

extern const HttpHandlerVT burrow__http_redirect_handler_vt;

/* registerErr. ServeMux.Handle without the panic, the pattern registered as
 * being at file and line. */
BURROW_BORROWS(ret) Error burrow__http_serve_mux_register_err(
    HttpServeMux *mux, Str pattern, HttpHandler h, const char *file, Int line);

/* findHandler. The handler for r, with the pattern's text, the pattern itself
 * and what its wildcards matched, the last two only when r is to go to a
 * handler that was registered. What is made is made in a. */
BURROW_BORROWS(ret, mux) HttpHandler burrow__http_serve_mux_find_handler(
    HttpServeMux *mux, const HttpRequest *r, Alloc *a, Str *pattern,
    const struct burrow__HttpPattern **pat, Slice *matches);

/* exactMatch. Whether n, which may be NULL, matched path by its own pattern
 * and not by the multi wildcard at its end. */
bool burrow__http_exact_match(const struct burrow__HttpRoutingNode *n, Str path);

/* The Go 1.21 mux, from servemux121.go, kept in the mux's arena. */
typedef struct burrow__HttpMux121 burrow__HttpMux121;

/* handle and findHandler. handle panics as Go's does. */
void burrow__http_mux121_handle(HttpServeMux *mux, Str pattern, HttpHandler h);
BURROW_BORROWS(ret, mux) HttpHandler burrow__http_mux121_find_handler(
    HttpServeMux *mux, const HttpRequest *r, Alloc *a, Str *pattern);

/* The mux's arena, set up the first time it is wanted. Call with mux->mu held
 * for writing. */
BURROW_BORROWS(ret, mux) Alloc *burrow__http_serve_mux_alloc(HttpServeMux *mux);

/* stripHostPort. h without its port, or h as it is when it has none or is not
 * host:port. */
BURROW_BORROWS(ret, h) Str burrow__http_strip_host_port(Str h);

/* --------------------------------------------------------------- File server
 *
 * What Go's tests of fs.go reach inside it for. */

/* httpservecontentkeepheaders=1 in GODEBUG. */
bool burrow__http_godebug_serve_content_keep_headers(void);

/* httpRange, one range of a Range header. */
typedef struct burrow__HttpRange {
    int64_t start;
    int64_t length;
} burrow__HttpRange;

/* parseRange. The ranges of s, a Range header value, for content of size
 * bytes, made in a and put in *out, and how many there are. None and no error
 * when s is empty. An error saying "invalid range" when s does not parse, and
 * one saying "invalid range: failed to overlap" when it does but every range
 * starts past the end. */
Int burrow__http_parse_range(Alloc *a, Str s, int64_t size, burrow__HttpRange **out,
                             Error *err);

/* scanETag. The ETag at the start of s, after its spaces, and in *remain what
 * follows it. Both empty when s does not start with one. */
BURROW_BORROWS(ret, s) Str burrow__http_scan_etag(Str s, Str *remain);

/* serveFile, with redirect saying whether a directory is redirected to a path
 * that ends in '/' and a file to one that does not. */
void burrow__http_serve_file(HttpResponseWriter w, HttpRequest *r, HttpFileSystem fs,
                             Str name, bool redirect);

/* ----------------------------------------------------------------- Transport
 *
 * The errors of transport.go that Go's tests compare against, and what they
 * reach inside it for. */

extern const Error burrow__http_err_keep_alives_disabled;
extern const Error burrow__http_err_conn_broken;
extern const Error burrow__http_err_close_idle;
extern const Error burrow__http_err_too_many_idle;
extern const Error burrow__http_err_too_many_idle_host;
extern const Error burrow__http_err_close_idle_conns;
extern const Error burrow__http_err_read_loop_exiting;
extern const Error burrow__http_err_idle_conn_timeout;
extern const Error burrow__http_err_server_closed_idle;
extern const Error burrow__http_err_caller_owns_conn;
extern const Error burrow__http_err_request_canceled;
extern const Error burrow__http_err_request_canceled_conn;
extern const Error burrow__http_err_request_done;
extern const Error burrow__http_err_read_on_closed_res_body;
extern const Error burrow__http_err_concurrent_read_on_res_body;
extern const Error burrow__http_err_cannot_rewind;
extern const Error burrow__http_err_no_host_in_url;
extern const Error burrow__http_err_no_tls;

/* errTimeout, a net.Error whose Timeout is true. */
extern const Error burrow__http_err_timeout;

/* &timeoutError{text}, errTimeout with text of its own, made in a. */
BURROW_OWNS(ret) Error burrow__http_timeout_error(Alloc *a, Str text);

/* validMethod. */
bool burrow__http_valid_method(Str m);

/* Whether e is the error Request.write gives for a request with no host. */
bool burrow__http_is_err_missing_host(Error e);

/* connectMethodKey.String, for a connection to target_addr by target_scheme
 * through proxy_url, NULL for none, made in a. */
BURROW_OWNS(ret) Str burrow__http_connect_method_key(Alloc *a, const Url *proxy_url,
                                                     Str target_scheme, Str target_addr,
                                                     bool only_h1);

/* connectMethod.proxyAuth. The Proxy-Authorization value for proxy_url's user,
 * made in a, and empty when it has none. */
BURROW_OWNS(ret) Str burrow__http_proxy_auth(Alloc *a, const Url *proxy_url);

/* resetProxyConfig. Makes ProxyFromEnvironment read the environment again. */
void burrow__http_reset_cached_environment(void);

/* The HttpTransport behind rt, or NULL when rt is some other round tripper. */
BURROW_BORROWS(ret, rt) HttpTransport *burrow__http_as_transport(HttpRoundTripper rt);

/* Transport.alternateRoundTripper. Sets *rt to the round tripper registered
 * for req's scheme and gives true, or gives false when the transport sends req
 * itself. */
bool burrow__http_transport_alternate(HttpTransport *t, const HttpRequest *req,
                                      HttpRoundTripper *rt);

#endif /* BURROW_SRC_NET_HTTP_INTERNAL_H */
