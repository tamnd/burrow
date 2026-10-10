/* net/http, HTTP clients and servers.
 *
 * Go's net/http, which is coming in pieces. So far it is what the rest of the
 * package stands on: the HTTP status codes and their text, HttpHeader and the
 * way it is written on the wire, the date formats a header can carry,
 * DetectContentType, HttpProtocols, cookies, and reading a request or a
 * response off a BufioReader with http_read_request and http_read_response.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     HttpHeader h = http_header_make(a);
 *     http_header_set(h, BURROW_S("content-type"), BURROW_S("text/plain"));
 *     http_header_add(h, BURROW_S("Vary"), BURROW_S("Accept"));
 *     Str ct = http_header_get(h, BURROW_S("Content-Type"));   // "text/plain"
 *     arena_free(&ar);
 *
 * An HttpHeader is a TextprotoMIMEHeader, a Map from each canonical key to a
 * Slice of its values, so the textproto and map functions work on it too.
 * Values are kept as they are given and not copied, and the keys and slices
 * come from the allocator the header was made with. An arena is the easy way
 * to hold one.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http */

#ifndef BURROW_NET_HTTP_H
#define BURROW_NET_HTTP_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- Status */

/* The HTTP status codes registered with IANA, as Go names them. See
 * https://www.iana.org/assignments/http-status-codes. */
enum {
    HTTP_STATUS_CONTINUE = 100,            /* RFC 9110, 15.2.1 */
    HTTP_STATUS_SWITCHING_PROTOCOLS = 101, /* RFC 9110, 15.2.2 */
    HTTP_STATUS_PROCESSING = 102,          /* RFC 2518, 10.1 */
    HTTP_STATUS_EARLY_HINTS = 103,         /* RFC 8297 */

    HTTP_STATUS_OK = 200,                     /* RFC 9110, 15.3.1 */
    HTTP_STATUS_CREATED = 201,                /* RFC 9110, 15.3.2 */
    HTTP_STATUS_ACCEPTED = 202,               /* RFC 9110, 15.3.3 */
    HTTP_STATUS_NON_AUTHORITATIVE_INFO = 203, /* RFC 9110, 15.3.4 */
    HTTP_STATUS_NO_CONTENT = 204,             /* RFC 9110, 15.3.5 */
    HTTP_STATUS_RESET_CONTENT = 205,          /* RFC 9110, 15.3.6 */
    HTTP_STATUS_PARTIAL_CONTENT = 206,        /* RFC 9110, 15.3.7 */
    HTTP_STATUS_MULTI_STATUS = 207,           /* RFC 4918, 11.1 */
    HTTP_STATUS_ALREADY_REPORTED = 208,       /* RFC 5842, 7.1 */
    HTTP_STATUS_IM_USED = 226,                /* RFC 3229, 10.4.1 */

    HTTP_STATUS_MULTIPLE_CHOICES = 300,   /* RFC 9110, 15.4.1 */
    HTTP_STATUS_MOVED_PERMANENTLY = 301,  /* RFC 9110, 15.4.2 */
    HTTP_STATUS_FOUND = 302,              /* RFC 9110, 15.4.3 */
    HTTP_STATUS_SEE_OTHER = 303,          /* RFC 9110, 15.4.4 */
    HTTP_STATUS_NOT_MODIFIED = 304,       /* RFC 9110, 15.4.5 */
    HTTP_STATUS_USE_PROXY = 305,          /* RFC 9110, 15.4.6 */
    HTTP_STATUS_TEMPORARY_REDIRECT = 307, /* RFC 9110, 15.4.8 */
    HTTP_STATUS_PERMANENT_REDIRECT = 308, /* RFC 9110, 15.4.9 */

    HTTP_STATUS_BAD_REQUEST = 400,                     /* RFC 9110, 15.5.1 */
    HTTP_STATUS_UNAUTHORIZED = 401,                    /* RFC 9110, 15.5.2 */
    HTTP_STATUS_PAYMENT_REQUIRED = 402,                /* RFC 9110, 15.5.3 */
    HTTP_STATUS_FORBIDDEN = 403,                       /* RFC 9110, 15.5.4 */
    HTTP_STATUS_NOT_FOUND = 404,                       /* RFC 9110, 15.5.5 */
    HTTP_STATUS_METHOD_NOT_ALLOWED = 405,              /* RFC 9110, 15.5.6 */
    HTTP_STATUS_NOT_ACCEPTABLE = 406,                  /* RFC 9110, 15.5.7 */
    HTTP_STATUS_PROXY_AUTH_REQUIRED = 407,             /* RFC 9110, 15.5.8 */
    HTTP_STATUS_REQUEST_TIMEOUT = 408,                 /* RFC 9110, 15.5.9 */
    HTTP_STATUS_CONFLICT = 409,                        /* RFC 9110, 15.5.10 */
    HTTP_STATUS_GONE = 410,                            /* RFC 9110, 15.5.11 */
    HTTP_STATUS_LENGTH_REQUIRED = 411,                 /* RFC 9110, 15.5.12 */
    HTTP_STATUS_PRECONDITION_FAILED = 412,             /* RFC 9110, 15.5.13 */
    HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE = 413,        /* RFC 9110, 15.5.14 */
    HTTP_STATUS_REQUEST_URI_TOO_LONG = 414,            /* RFC 9110, 15.5.15 */
    HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE = 415,          /* RFC 9110, 15.5.16 */
    HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE = 416, /* RFC 9110, 15.5.17 */
    HTTP_STATUS_EXPECTATION_FAILED = 417,              /* RFC 9110, 15.5.18 */
    HTTP_STATUS_TEAPOT = 418,                          /* RFC 9110, 15.5.19 (Unused) */
    HTTP_STATUS_MISDIRECTED_REQUEST = 421,             /* RFC 9110, 15.5.20 */
    HTTP_STATUS_UNPROCESSABLE_ENTITY = 422,            /* RFC 9110, 15.5.21 */
    HTTP_STATUS_LOCKED = 423,                          /* RFC 4918, 11.3 */
    HTTP_STATUS_FAILED_DEPENDENCY = 424,               /* RFC 4918, 11.4 */
    HTTP_STATUS_TOO_EARLY = 425,                       /* RFC 8470, 5.2. */
    HTTP_STATUS_UPGRADE_REQUIRED = 426,                /* RFC 9110, 15.5.22 */
    HTTP_STATUS_PRECONDITION_REQUIRED = 428,           /* RFC 6585, 3 */
    HTTP_STATUS_TOO_MANY_REQUESTS = 429,               /* RFC 6585, 4 */
    HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE = 431, /* RFC 6585, 5 */
    HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS = 451,   /* RFC 7725, 3 */

    HTTP_STATUS_INTERNAL_SERVER_ERROR = 500,           /* RFC 9110, 15.6.1 */
    HTTP_STATUS_NOT_IMPLEMENTED = 501,                 /* RFC 9110, 15.6.2 */
    HTTP_STATUS_BAD_GATEWAY = 502,                     /* RFC 9110, 15.6.3 */
    HTTP_STATUS_SERVICE_UNAVAILABLE = 503,             /* RFC 9110, 15.6.4 */
    HTTP_STATUS_GATEWAY_TIMEOUT = 504,                 /* RFC 9110, 15.6.5 */
    HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED = 505,      /* RFC 9110, 15.6.6 */
    HTTP_STATUS_VARIANT_ALSO_NEGOTIATES = 506,         /* RFC 2295, 8.1 */
    HTTP_STATUS_INSUFFICIENT_STORAGE = 507,            /* RFC 4918, 11.5 */
    HTTP_STATUS_LOOP_DETECTED = 508,                   /* RFC 5842, 7.2 */
    HTTP_STATUS_NOT_EXTENDED = 510,                    /* RFC 2774, 7 */
    HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED = 511, /* RFC 6585, 6 */
};

/* http.StatusText. The text for an HTTP status code, such as "Not Found" for
 * 404, or "" when the code is not one of the above. */
BURROW_STATIC(ret) Str http_status_text(Int code);

/* ------------------------------------------------------------------- Header */

/* http.Header, map[string][]string. */
typedef TextprotoMIMEHeader HttpHeader;

/* make(http.Header). NULL when a says no. */
BURROW_OWNS(ret) static inline HttpHeader http_header_make(Alloc *a) {
    return textproto_mime_header_make(a);
}

/* Header.Add and Set. key goes into canonical form first, so "accept-encoding"
 * and "Accept-Encoding" are the same key. False when the header's allocator
 * says no, and then h is as it was. To use a key that is not canonical, set it
 * in the Map directly. */
static inline bool http_header_add(HttpHeader h, Str key, Str value) {
    return textproto_mime_header_add(h, key, value);
}
static inline bool http_header_set(HttpHeader h, Str key, Str value) {
    return textproto_mime_header_set(h, key, value);
}

/* Header.Get. The first value for the canonical form of key, or "" when there
 * is none. */
BURROW_BORROWS(ret, h) static inline Str http_header_get(HttpHeader h, Str key) {
    return textproto_mime_header_get(h, key);
}

/* Header.Values. Every value for the canonical form of key, pointing into h and
 * good until h next changes. An empty Slice when there are none. */
BURROW_BORROWS(ret, h) static inline Slice http_header_values(HttpHeader h, Str key) {
    return textproto_mime_header_values(h, key);
}

/* Header.Del. */
static inline void http_header_del(HttpHeader h, Str key) {
    textproto_mime_header_del(h, key);
}

/* Header.Write. The header in wire format: each key and value as "Key: value"
 * and "\r\n", the keys in sorted order and each key's values in the order they
 * are in. A key that is not a valid header field name is left out, and in a
 * value each "\r" and "\n" becomes a space, with the space at either end then
 * trimmed, so a header cannot write a line it did not mean to. The error is
 * the first one w gives back. */
BURROW_STATIC(ret) Error http_header_write(HttpHeader h, IoWriter w);

/* Header.WriteSubset. http_header_write without the keys exclude maps to true.
 * exclude is a Map from Str to bool, Go's map[string]bool, and may be NULL.
 * The temporary list of keys comes from the header's allocator when it has more
 * than 64 of them, and a header with fewer writes without allocating. */
BURROW_STATIC(ret) Error http_header_write_subset(HttpHeader h, IoWriter w,
                                                  Map *exclude);

/* Header.Clone. A copy of h made in a, or NULL for a NULL h. The copy has its
 * own map and slices, in one block for all the values, and shares the bytes
 * of every key and value with h. NULL as well when a says no. */
BURROW_OWNS(ret) HttpHeader http_header_clone(Alloc *a, HttpHeader h);

/* http.CanonicalHeaderKey, which is textproto_canonical_mime_header_key. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) static inline Str
http_canonical_header_key(Alloc *a, Str s) {
    return textproto_canonical_mime_header_key(a, s);
}

/* http.TimeFormat, the layout for a time in an HTTP header such as Date. It is
 * like TIME_RFC1123 but with GMT for the zone, and the time it formats has to
 * be in UTC. */
#define HTTP_TIME_FORMAT BURROW_S("Mon, 02 Jan 2006 15:04:05 GMT")

/* http.ParseTime. text in the first of the three formats HTTP/1.1 allows that
 * reads it: HTTP_TIME_FORMAT, TIME_RFC850 and TIME_ANSIC. When none of them
 * does, the error is the one the last of them gave. A zone in the text that
 * is not UTC is made in a, as time_parse does. */
Time http_parse_time(Alloc *a, Str text, Error *err);

/* ------------------------------------------------------------------ Cookies */

/* http.SameSite, the SameSite attribute of a cookie. Zero means the cookie has
 * none, and HTTP_SAME_SITE_DEFAULT_MODE means it has one with no value, or one
 * this package does not know. */
typedef enum HttpSameSite {
    HTTP_SAME_SITE_DEFAULT_MODE = 1,
    HTTP_SAME_SITE_LAX_MODE = 2,
    HTTP_SAME_SITE_STRICT_MODE = 3,
    HTTP_SAME_SITE_NONE_MODE = 4,
} HttpSameSite;

/* http.Cookie, a cookie as it is sent in the Set-Cookie header of a response
 * or the Cookie header of a request. See RFC 6265.
 *
 * The zero value of every field means the attribute is not there. max_age 0
 * means no Max-Age, less than 0 means "delete it now", which is written as
 * Max-Age=0, and more than 0 is the number of seconds. quoted says the value
 * came in double quotes, raw is the whole Set-Cookie line a parsed cookie came
 * from, and unparsed is a Slice of Str holding the attributes that were not
 * understood, as they were written. */
typedef struct HttpCookie {
    Str name;
    Str value;
    Str path;
    Str domain;
    Time expires;
    Str raw_expires; /* only for cookies that were read */
    Int max_age;
    Str raw;
    Slice unparsed; /* of Str */
    HttpSameSite same_site;
    bool quoted;
    bool secure;
    bool http_only;
    bool partitioned;
} HttpCookie;

extern const Type *const TYPE_HTTP_COOKIE;

/* http.ParseCookie. Every cookie in line, the value of a Cookie header, as a
 * Slice of HttpCookie from a. A name can be there more than once, and each one
 * is kept. The strings point into line. An error gives an empty Slice: an
 * empty line, a part with no "=", a name that is not a token and a value with
 * a byte a cookie cannot hold each have their own, as in Go, and so does a line
 * with more than 3000 cookies, a limit GODEBUG=httpcookiemaxnum=N changes and
 * 0 takes away. burrow_err_out_of_memory when a says no. */
BURROW_OWNS(ret) Slice http_parse_cookie(Alloc *a, Str line, Error *err);

/* http.ParseSetCookie. The cookie in line, the value of a Set-Cookie header.
 * The strings point into line, and unparsed comes from a. The errors are
 * ParseCookie's, less the limit. An Expires that reads as neither TIME_RFC1123
 * nor "Mon, 02-Jan-2006 15:04:05 MST" leaves expires zero, keeps raw_expires,
 * and goes in unparsed. Give the cookie back with http_cookie_free, or let an
 * arena go. */
BURROW_OWNS(ret) HttpCookie http_parse_set_cookie(Alloc *a, Str line, Error *err);

/* Gives back the unparsed Slice of a cookie from http_parse_set_cookie. */
void http_cookie_free(Alloc *a, HttpCookie *c);

/* Cookie.String. The cookie written for a Cookie header when it has only a
 * name and value, and for a Set-Cookie header when it has more, from a. Empty
 * for a NULL c and for a name that is not a token, and when a says no.
 *
 * Bytes a value or path cannot hold are dropped, a value with a space or comma
 * in it is quoted, and a domain that is not a host name or IPv4 address is left
 * out. Each of these is reported on the standard logger, as Go reports them
 * with log.Printf. */
BURROW_OWNS(ret) Str http_cookie_string(Alloc *a, const HttpCookie *c);

/* Cookie.Valid. No error when c could be sent as it is. The error for a byte
 * that does not belong is made by fmt_errorf, and the others are static. */
BURROW_BORROWS(ret) Error http_cookie_valid(const HttpCookie *c);

/* ------------------------------------------------------------------ Request */

/* http.NoBody, a body with no bytes in it. Reading gives io_eof at once and
 * closing does nothing. A request or response read from the wire has it as
 * its body when there is no body to read, and comparing body.vt with
 * http_no_body.vt says whether that is so. */
extern const IoReadCloser http_no_body;

/* http.ErrBodyReadAfterClose, from reading a body after it was closed. */
extern const Error http_err_body_read_after_close;

/* http.Request, as far as reading one from the wire goes: a request a server
 * got, or one a client is about to send.
 *
 * url is where the request goes, which for a request read by
 * http_read_request is the target in its first line, request_uri, parsed.
 * proto is "HTTP/1.1" or the like, and proto_major and proto_minor are its
 * numbers. header has the header fields, with the keys in canonical form, and
 * for a request read from the wire it does not have Host, which is in host.
 *
 * body is never NULL in a request read from the wire. It is http_no_body when
 * the request has no body, and otherwise reads the body as the header says to,
 * by Content-Length or by chunks, and closing it reads the rest so the next
 * request on the connection can be read. content_length is the length of the
 * body, -1 when it is not known, and transfer_encoding is a Slice of Str that
 * holds "chunked" when the body came in chunks. trailer has the keys the
 * Trailer field named, and once the body has been read to its end it has the
 * values the trailer gave them too.
 *
 * close says whether the connection is to be closed after this request.
 * host is the host the request is for, from the URL or the Host field.
 * remote_addr is the address the request came from, which a server sets, and
 * pattern is the ServeMux pattern that matched it.
 *
 * Everything a request read from the wire has, the strings and the header and
 * the URL, lives in the request's own arena until http_request_free. */
typedef struct HttpRequest {
    Str method;
    Url *url;
    Str proto;
    Int proto_major;
    Int proto_minor;
    HttpHeader header;
    IoReadCloser body;
    int64_t content_length;
    Slice transfer_encoding;
    HttpHeader trailer;
    Str host;
    Str remote_addr;
    Str request_uri;
    Str pattern;
    bool close;

    /* The request's own. */
    Alloc *a;
    Arena arena;
    void *wire; /* the body that reads from the wire, freed with the request */
} HttpRequest;

/* http.ReadRequest. Reads a request from b, its first line and header, and
 * leaves its body in b to be read through the request's body. The request is
 * made in a, and the strings in it are in its arena. On an error the result is
 * NULL. The first line can fail with io_eof, at the end of the input, and an
 * end that comes after that is io_err_unexpected_eof. Give it back with
 * http_request_free. */
BURROW_OWNS(ret) HttpRequest *http_read_request(Alloc *a, BufioReader *b, Error *err);

/* Gives back a request http_read_request made, and the memory of its body, so
 * the body is not to be read after this. NULL is fine. */
void http_request_free(HttpRequest *r);

/* Request.ProtoAtLeast. Whether the request's protocol is at least
 * major.minor. */
bool http_request_proto_at_least(const HttpRequest *r, Int major, Int minor);

/* Request.UserAgent and Referer, the User-Agent and Referer fields. */
BURROW_BORROWS(ret, r) Str http_request_user_agent(const HttpRequest *r);
BURROW_BORROWS(ret, r) Str http_request_referer(const HttpRequest *r);

/* Request.BasicAuth. The user name and password from the Authorization field,
 * when it has HTTP basic authentication, decoded into a. False otherwise. */
bool http_request_basic_auth(const HttpRequest *r, Alloc *a, Str *username,
                             Str *password);

/* Request.SetBasicAuth. Sets the Authorization field to basic authentication
 * with username and password, which are not encrypted, so it belongs on HTTPS.
 * The value is made in a, which has to last as long as the header. False when
 * an allocator says no, or when r has no header. */
bool http_request_set_basic_auth(HttpRequest *r, Alloc *a, Str username, Str password);

/* http.ParseHTTPVersion. The numbers of an HTTP version such as "HTTP/1.0",
 * which gives 1 and 0. A version without a minor number, such as "HTTP/2", is
 * not one. False for anything that is not a version. */
bool http_parse_http_version(Str vers, Int *major, Int *minor);

/* ----------------------------------------------------------------- Response */

/* http.ErrNoLocation, from http_response_location when there is no Location
 * field. */
extern const Error http_err_no_location;

/* http.Response, as far as reading one from the wire goes. status is the
 * status line after the protocol, such as "200 OK", and status_code is its
 * number. The rest is as in HttpRequest. request is the request this is the
 * response to, which is borrowed, and uncompressed says the transport took
 * gzip off the body.
 *
 * Read body to its end, or close it, before reading the next response on the
 * same connection. */
typedef struct HttpResponse {
    Str status;
    Int status_code;
    Str proto;
    Int proto_major;
    Int proto_minor;
    HttpHeader header;
    IoReadCloser body;
    int64_t content_length;
    Slice transfer_encoding;
    HttpHeader trailer;
    HttpRequest *request;
    bool close;
    bool uncompressed;

    /* The response's own. */
    Alloc *a;
    Arena arena;
    void *wire;
} HttpResponse;

/* http.ReadResponse. Reads a response from r, its status line and header, and
 * leaves the body in r to be read through the response's body. req is the
 * request it answers, and NULL is taken as a GET. The response is made in a.
 * NULL on an error, and an end of the input is io_err_unexpected_eof. Give it
 * back with http_response_free. */
BURROW_OWNS(ret) HttpResponse *http_read_response(Alloc *a, BufioReader *r,
                                                  HttpRequest *req, Error *err);

/* Gives back a response http_read_response made, and its body. NULL is fine. */
void http_response_free(HttpResponse *r);

/* Response.ProtoAtLeast. */
bool http_response_proto_at_least(const HttpResponse *r, Int major, Int minor);

/* Response.Location. The URL in the Location field, made in a, and relative
 * to the request's URL when there is a request. http_err_no_location when
 * there is no Location field. */
BURROW_OWNS(ret) Url *http_response_location(const HttpResponse *r, Alloc *a,
                                             Error *err);

/* ----------------------------------------------------------------- Sniffing */

/* http.DetectContentType. The Content-Type of data by the algorithm at
 * https://mimesniff.spec.whatwg.org/, which looks at the first 512 bytes at
 * most. Always a valid MIME type, and "application/octet-stream" when nothing
 * more specific fits. data is a Slice of Byte. */
BURROW_STATIC(ret) Str http_detect_content_type(Slice data);

/* ---------------------------------------------------------------- Protocols */

/* http.Protocols, a set of HTTP protocols. The zero value is the empty set. */
typedef struct HttpProtocols {
    uint8_t bits;
} HttpProtocols;

/* Protocols.HTTP1 and SetHTTP1: HTTP/1, over TCP or TLS. */
bool http_protocols_http1(HttpProtocols p);
void http_protocols_set_http1(HttpProtocols *p, bool ok);

/* Protocols.HTTP2 and SetHTTP2: HTTP/2 over TLS. */
bool http_protocols_http2(HttpProtocols p);
void http_protocols_set_http2(HttpProtocols *p, bool ok);

/* Protocols.UnencryptedHTTP2 and SetUnencryptedHTTP2: HTTP/2 over TCP without
 * TLS, prior knowledge in RFC 9113's words, where both ends already know the
 * other speaks it. */
bool http_protocols_unencrypted_http2(HttpProtocols p);
void http_protocols_set_unencrypted_http2(HttpProtocols *p, bool ok);

/* Protocols.String, such as "{HTTP1,HTTP2}". */
BURROW_STATIC(ret) Str http_protocols_string(HttpProtocols p);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_H */
