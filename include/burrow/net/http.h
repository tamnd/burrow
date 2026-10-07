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
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
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

/* http.CookieJar, which keeps the cookies a client is sent and picks the ones
 * to send back. set_cookies is handed the cookies of a response from u, a
 * Slice of HttpCookie, and may keep them or not as its rules say. cookies
 * gives the ones a request to u should carry, as a Slice of HttpCookie from a,
 * and it is up to the jar to keep to the rules of RFC 6265 about which those
 * are. Neither keeps u or anything in the cookies it is given. A jar has to be
 * safe to use from more than one goroutine at once. net/http/cookiejar has
 * one. */
typedef struct HttpCookieJarVT {
    const Type *self_type;
    void (*set_cookies)(void *self, const Url *u, Slice cookies);
    Slice (*cookies)(void *self, Alloc *a, const Url *u);
} HttpCookieJarVT;

typedef struct HttpCookieJar {
    const HttpCookieJarVT *vt;
    void *data;
} HttpCookieJar;

/* CookieJar.SetCookies. */
static inline void http_cookie_jar_set_cookies(HttpCookieJar j, const Url *u,
                                               Slice cookies) {
    j.vt->set_cookies(j.data, u, cookies);
}

/* CookieJar.Cookies. */
BURROW_OWNS(ret) static inline Slice http_cookie_jar_cookies(HttpCookieJar j, Alloc *a,
                                                             const Url *u) {
    return j.vt->cookies(j.data, a, u);
}

/* ------------------------------------------------------------------ Request */

/* http.NoBody, a body with no bytes in it. Reading gives io_eof at once and
 * closing does nothing. A request or response read from the wire has it as
 * its body when there is no body to read, and comparing body.vt with
 * http_no_body.vt says whether that is so. */
extern const IoReadCloser http_no_body;

/* http.ErrBodyReadAfterClose, from reading a body after it was closed. */
extern const Error http_err_body_read_after_close;

/* Request.GetBody. A new reader of the same body, for a client that has to
 * send the request again, such as after a redirect. */
BURROW_FUNC(HttpGetBodyFunc, IoReadCloser, Error *err);

/* http.Request, as far as reading one from the wire and writing one to it go:
 * a request a server got, or one a client is about to send.
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
 * get_body, which may be nil, makes a new copy of the body, and
 * http_new_request sets it for a body it knows how to copy. ctx is the
 * request's context, read with http_request_context, and nil means
 * context_background. response is the redirect response that made an
 * HttpClient send this request, and NULL for the first request it sends.
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
    HttpGetBodyFunc get_body;
    Context ctx;
    struct HttpResponse *response;
    bool close;

    /* The request's own. */
    Alloc *a;
    Arena arena;
    void *wire; /* the body that reads from the wire, freed with the request */

    /* What a ServeMux matched, for http_request_path_value. */
    const struct burrow__HttpPattern *pat;
    Slice matches;     /* of Str, one for each named wildcard */
    Map *other_values; /* of Str to Str, set by http_request_set_path_value */
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

/* http.ErrNoCookie, from http_request_cookie when there is no such cookie. */
extern const Error http_err_no_cookie;

/* Request.Cookies. The cookies of the request's Cookie fields, as a Slice of
 * HttpCookie from a, with any that do not parse left out. Their strings point
 * into the header. */
BURROW_OWNS(ret) Slice http_request_cookies(const HttpRequest *r, Alloc *a);

/* Request.CookiesNamed. http_request_cookies with only the ones called name,
 * and none for an empty name. */
BURROW_OWNS(ret) Slice http_request_cookies_named(const HttpRequest *r, Alloc *a,
                                                  Str name);

/* Request.Cookie. The first cookie called name, and http_err_no_cookie with a
 * zero cookie when there is none. */
HttpCookie http_request_cookie(const HttpRequest *r, Alloc *a, Str name, Error *err);

/* Request.AddCookie. Adds c's name and value to the request's Cookie field,
 * after the cookies it has, as RFC 6265 says to keep them all in one field. A
 * CR or LF in the name becomes "-", and the value loses the bytes a cookie
 * cannot hold, as http_cookie_string does with it. Nothing else of c is used.
 * The field is made in a, which has to last as long as the header. False when
 * a says no. */
bool http_request_add_cookie(HttpRequest *r, Alloc *a, const HttpCookie *c);

/* http.NewRequest and NewRequestWithContext. A request for a client to send,
 * made in a, with url parsed into it, method "GET" when it is "", protocol
 * HTTP/1.1, an empty header and host taken from the URL. ctx may not be nil.
 * Give it back with http_request_free.
 *
 * body is read when the request is written, and may be nil. Go keeps a body
 * that is an io.ReadCloser as it is, so that writing the request closes it,
 * but an IoReader cannot say whether it is one, so here the body is always
 * wrapped in a closer that does nothing. To have it closed, set the request's
 * body to the IoReadCloser after this.
 *
 * When body is a BytesBuffer, a BytesReader or a StringsReader, content_length
 * is the number of bytes left in it and get_body reads those same bytes again,
 * and a body with nothing left in it is http_no_body. The BytesBuffer's bytes
 * are not copied, so they have to stay as they are. For any other body
 * content_length is 0, which for a request that is not known to be empty
 * means unknown.
 *
 * The errors are a method that is not a token, a nil ctx, and any from
 * url_parse. */
BURROW_OWNS(ret) HttpRequest *http_new_request(Alloc *a, Str method, Str url,
                                               IoReader body, Error *err);
BURROW_OWNS(ret) HttpRequest *http_new_request_with_context(Alloc *a, Context ctx,
                                                            Str method, Str url,
                                                            IoReader body, Error *err);

/* Request.Context. The request's context, and context_background when it has
 * none. */
Context http_request_context(const HttpRequest *r);

/* Request.WithContext. A shallow copy of r, made in a, with its context set to
 * ctx, which may not be nil. The copy shares everything else with r, so r has
 * to outlive it. Give it back with http_request_free, which frees only the
 * copy. NULL when a says no. */
BURROW_OWNS(ret) HttpRequest *http_request_with_context(const HttpRequest *r, Alloc *a,
                                                        Context ctx);

/* Request.Write. Writes r to w as an HTTP/1.1 request a client sends: the
 * request line, the header and the body. The host is r's host, or the URL's
 * when that is "", and a host that is not valid in a Host field is sent as ""
 * rather than altered. User-Agent is "Go-http-client/1.1" unless the header
 * has one, and an empty one leaves it out. Content-Length or
 * Transfer-Encoding, and Trailer, come from content_length,
 * transfer_encoding and trailer, and the same fields in the header are not
 * written. The body is closed, even on an error.
 *
 * A request with a body of unknown length, content_length 0 and a body that
 * is not nil or http_no_body, and a method such as GET that usually has no
 * body, has its first byte read to see whether there is one. In a goroutine
 * that read is given 200 milliseconds and the request goes out with a chunked
 * body if it takes longer. Outside of one only a body in memory, as listed at
 * http_new_request, is read, and any other is taken as one that took too
 * long.
 *
 * Writes go to w as they are when w is a BufioWriter, a StringsBuilder or a
 * BytesBuffer, or has a WriteByte method, and through a BufioWriter of its own
 * otherwise. */
BURROW_BORROWS(ret) Error http_request_write(HttpRequest *r, IoWriter w);

/* Request.WriteProxy. http_request_write in the form a proxy wants, with the
 * whole URL in the request line. */
BURROW_BORROWS(ret) Error http_request_write_proxy(HttpRequest *r, IoWriter w);

/* Request.PathValue. The value of the wildcard called name in the ServeMux
 * pattern that matched r, or one set with http_request_set_path_value, and ""
 * when there is neither. */
BURROW_BORROWS(ret, r) Str http_request_path_value(const HttpRequest *r, Str name);

/* Request.SetPathValue. Sets what http_request_path_value gives for name to
 * value. Neither is copied, and neither is unescaped. a is where the map for a
 * name that is not one of the pattern's goes, and has to last as long as r.
 * False when a says no. */
bool http_request_set_path_value(HttpRequest *r, Alloc *a, Str name, Str value);

/* http.ParseHTTPVersion. The numbers of an HTTP version such as "HTTP/1.0",
 * which gives 1 and 0. A version without a minor number, such as "HTTP/2", is
 * not one. False for anything that is not a version. */
bool http_parse_http_version(Str vers, Int *major, Int *minor);

/* ----------------------------------------------------------------- Response */

/* http.ErrNoLocation, from http_response_location when there is no Location
 * field. */
extern const Error http_err_no_location;

/* http.Response, as far as reading one from the wire and writing one go. status is the
 * status line after the protocol, such as "200 OK", and status_code is its
 * number. The rest is as in HttpRequest. request is the request this is the
 * response to, which is borrowed, and uncompressed says the transport took
 * gzip off the body.
 *
 * on_free is for whatever made the response, such as HttpTransport, and runs
 * first in http_response_free, to let go of what it keeps for the response. A
 * round tripper that wraps another and wants a hook of its own keeps the one
 * it found and calls it from its own.
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
    Func on_free;

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

/* Gives back a response http_read_response made, and its body, after running
 * its on_free. NULL is fine. A response from a client or a transport goes back
 * this way too, and closes its body first if it is still open. */
void http_response_free(HttpResponse *r);

/* Response.Write. Writes r to w as an HTTP/1.x response a server sends: the
 * status line from proto_major, proto_minor, status_code and status, the
 * header, and the body, which is closed after. A content_length of 0 with a
 * body that has bytes in it is taken as unknown, and a body of unknown length
 * that is not chunked closes the connection after it, which an HTTP/1.1
 * response says with "Connection: close". Content-Length, Transfer-Encoding
 * and Trailer come from the fields, not the header. */
BURROW_BORROWS(ret) Error http_response_write(HttpResponse *r, IoWriter w);

/* Response.ProtoAtLeast. */
bool http_response_proto_at_least(const HttpResponse *r, Int major, Int minor);

/* Response.Location. The URL in the Location field, made in a, and relative
 * to the request's URL when there is a request. http_err_no_location when
 * there is no Location field. */
BURROW_OWNS(ret) Url *http_response_location(const HttpResponse *r, Alloc *a,
                                             Error *err);

/* Response.Cookies. The cookies of the response's Set-Cookie fields, as a
 * Slice of HttpCookie from a, with any that do not parse left out. */
BURROW_OWNS(ret) Slice http_response_cookies(const HttpResponse *r, Alloc *a);

/* ------------------------------------------------------------------ Handler */

/* http.ResponseWriter, what a handler writes its response with.
 *
 * header is the header the response will have. Changing it after write_header,
 * or after the first write, changes nothing unless the keys were named in a
 * Trailer field first. write writes body bytes, and calls write_header with
 * HTTP_STATUS_OK first when nobody has. write_header sends the status line and
 * the header, and is called at most once, with a code from 100 to 999.
 *
 * The writer embeds an IoWriter, so http_response_writer_as_io_writer is what
 * the fmt and io functions take.
 *
 * The rest are the methods Go finds on a writer with a type assertion, and
 * each is NULL in a writer that does not have it, which a zeroed member is.
 * HttpResponseController is the way to call them. flush is Go's FlushError,
 * or Flush with no error to give. hijack, set_read_deadline,
 * set_write_deadline, enable_full_duplex and close_notify are the methods of
 * the same names, and unwrap gives the writer this one wraps, for a
 * middleware writer that wants the controller to reach the methods of the one
 * under it. */
struct HttpResponseWriter;

typedef struct HttpResponseWriterVT {
    IoWriterVT writer;
    HttpHeader (*header)(void *self);
    void (*write_header)(void *self, Int status_code);
    Error (*flush)(void *self);
    NetConn (*hijack)(void *self, BufioReadWriter *buf, Error *err);
    Error (*set_read_deadline)(void *self, Time deadline);
    Error (*set_write_deadline)(void *self, Time deadline);
    Error (*enable_full_duplex)(void *self);
    Chan *(*close_notify)(void *self);
    struct HttpResponseWriter (*unwrap)(void *self);
} HttpResponseWriterVT;

typedef struct HttpResponseWriter {
    const HttpResponseWriterVT *vt;
    void *data;
} HttpResponseWriter;

/* The writer as the IoWriter it embeds. */
IoWriter http_response_writer_as_io_writer(HttpResponseWriter w);

/* ResponseWriter.Header, Write and WriteHeader. */
BURROW_BORROWS(ret, w) static inline HttpHeader
http_response_writer_header(HttpResponseWriter w) {
    return w.vt->header(w.data);
}
static inline Int http_response_writer_write(HttpResponseWriter w, Slice p,
                                             Error *err) {
    return w.vt->writer.write(w.data, p, err);
}
static inline void http_response_writer_write_header(HttpResponseWriter w,
                                                     Int status_code) {
    w.vt->write_header(w.data, status_code);
}

/* http.ErrBodyNotAllowed, from writing a body after a status that does not
 * have one, such as 204 or 304, or for a request method that does not, such
 * as HEAD. */
extern const Error http_err_body_not_allowed;

/* http.TrailerPrefix. A header key that starts with it is a trailer, under the
 * rest of the key, rather than a header. It is for a trailer whose name was
 * not known before the header went out, so it could not be named in a Trailer
 * field. */
#define HTTP_TRAILER_PREFIX BURROW_S("Trailer:")

/* http.Handler, which answers a request. serve_http writes the response to w
 * and returns when it is done, and neither w nor r is to be used after that. A
 * handler that wants the body has to read it before it writes, since a server
 * may not be able to read it once the response has begun. */
typedef struct HttpHandlerVT {
    const Type *self_type;
    void (*serve_http)(void *self, HttpResponseWriter w, HttpRequest *r);
} HttpHandlerVT;

typedef struct HttpHandler {
    const HttpHandlerVT *vt;
    void *data;
} HttpHandler;

/* Handler.ServeHTTP. */
static inline void http_handler_serve_http(HttpHandler h, HttpResponseWriter w,
                                           HttpRequest *r) {
    h.vt->serve_http(h.data, w, r);
}

/* http.HandlerFunc, a function used as a handler. */
BURROW_FUNC(HttpHandlerFunc, void, HttpResponseWriter w, HttpRequest *r);

/* HandlerFunc.ServeHTTP, which calls f. */
static inline void http_handler_func_serve_http(HttpHandlerFunc f, HttpResponseWriter w,
                                                HttpRequest *r) {
    f.f(f.env, w, r);
}

extern const Type *const TYPE_HTTP_HANDLER_FUNC;

/* HandlerFunc(f) as a Handler, which calls f. The handler points at f, so f has
 * to outlive it. */
BURROW_BORROWS(ret, f) HttpHandler http_handler_func_as_handler(HttpHandlerFunc *f);

/* http.Error. Replies to the request with error, a plain text message, and
 * code, after taking Content-Length out of w's header, which may be for some
 * other body, and setting Content-Type to "text/plain; charset=utf-8" and
 * X-Content-Type-Options to "nosniff". The caller should not write more to w
 * after this. */
void http_error(HttpResponseWriter w, Str error, Int code);

/* http.NotFound, http_error with "404 page not found" and 404. */
void http_not_found(HttpResponseWriter w, HttpRequest *r);

/* http.NotFoundHandler, a handler that calls http_not_found. */
HttpHandler http_not_found_handler(void);

/* http.Redirect. Replies to r with a redirect to url, which may be relative to
 * the request's path, and code, which should be a 3xx such as
 * HTTP_STATUS_FOUND. Characters in url that are not ASCII are percent-encoded,
 * and any encoding it already has is kept.
 *
 * When w's header has no Content-Type, a GET or HEAD gets
 * "text/html; charset=utf-8", and a GET a short HTML body with the link. A
 * Content-Type, even one with no values, leaves both out. The Location value
 * is made with the header's allocator. */
void http_redirect(HttpResponseWriter w, HttpRequest *r, Str url, Int code);

/* http.RedirectHandler, a handler that redirects every request to url with
 * code. It is made in a, and keeps url as it is, so url has to outlive it. NULL
 * data, which is no handler, when a says no. */
BURROW_OWNS(ret) HttpHandler http_redirect_handler(Alloc *a, Str url, Int code);

/* http.StripPrefix. A handler that takes prefix off the request URL's path,
 * and its raw path when it has one, and passes the request to h. A path that
 * does not start with prefix, or a raw path that does not, gets a 404. The
 * request h sees is a copy, with a copy of the URL, so the request is left as
 * it was. An empty prefix gives h itself. Made in a, and NULL data when a says
 * no. */
BURROW_OWNS(ret) HttpHandler http_strip_prefix(Alloc *a, Str prefix, HttpHandler h);

/* ----------------------------------------------------------------- ServeMux */

/* http.ServeMux, which sends each request to the handler for the pattern that
 * matches it best.
 *
 * A pattern is [METHOD ][HOST]/[PATH], such as "/index.html", "GET /static/",
 * "example.com/" or "/b/{bucket}/o/{objectname...}". A method matches only
 * that method, except that GET matches HEAD too, and a host matches only that
 * host. Each segment of the path is a literal, which matches itself
 * unescaped, or a wildcard: "{name}" matches one segment, "{name...}" the rest
 * of the path and has to be last, and "{$}" only the end of a path that ends
 * in a slash. A pattern ending in a slash matches every path that starts with
 * it, so "/" matches anything. http_request_path_value gives the segment a
 * wildcard matched.
 *
 * When two patterns match a request the more specific one wins, the one that
 * matches a strict subset of the other's requests, and a pattern with a host
 * wins over one without. Two patterns that match some request each and
 * neither is more specific conflict, and registering the second panics.
 *
 * A request for a path such as "/tree" when only "/tree/" is registered is
 * redirected there, and so is one whose path is not clean, such as "/a/../b"
 * or "//b", to the clean one. A request that matches no pattern gets a 404, or
 * a 405 with an Allow field when a pattern matches it with another method.
 *
 * The zero value is ready to use, and http_new_serve_mux makes one. The
 * patterns and the tree they go into come from the mux's own arena, which
 * http_serve_mux_free gives back. Registering and serving can go on at once
 * from any number of goroutines. GODEBUG=httpmuxgo121=1, read once, gives the
 * mux of Go 1.21 instead, with no methods, hosts only as a prefix, no
 * wildcards and no conflicts. */
typedef struct HttpServeMux {
    SyncRWMutex mu;
    struct burrow__HttpRoutingNode *tree;
    struct burrow__HttpRoutingIndex *index;
    struct burrow__HttpMux121 *mux121;
    Alloc *a; /* what made the mux, or NULL for one that was not made */
    Arena arena;
    bool ready;
} HttpServeMux;

/* http.NewServeMux. A new mux made in a, NULL when a says no. Give it back
 * with http_serve_mux_free. */
BURROW_OWNS(ret) HttpServeMux *http_new_serve_mux(Alloc *a);

/* Gives back what the mux holds, and the mux itself if http_new_serve_mux
 * made it. Not while it is being used. NULL is fine. */
void http_serve_mux_free(HttpServeMux *mux);

/* http.DefaultServeMux, the mux the server uses when it is given no handler,
 * and that http_handle and http_handle_func register with. */
extern HttpServeMux *const http_default_serve_mux;

/* ServeMux.Handle. Registers h for pattern, and panics when the pattern is not
 * valid, h is nil, or the pattern conflicts with one already registered. The
 * message for a conflict says where both were registered, which is why this is
 * a macro: it passes the caller's file and line. */
#define http_serve_mux_handle(mux, pattern, h)                                         \
    burrow__http_serve_mux_handle_at((mux), (pattern), (h), __FILE__, __LINE__)

/* ServeMux.HandleFunc. http_serve_mux_handle with a function, which is copied
 * into the mux, so the copy lives as long as it does. Its env has to as
 * well. */
#define http_serve_mux_handle_func(mux, pattern, f)                                    \
    burrow__http_serve_mux_handle_func_at((mux), (pattern), (f), __FILE__, __LINE__)

/* http.Handle and http.HandleFunc, on http_default_serve_mux. */
#define http_handle(pattern, h)                                                        \
    burrow__http_serve_mux_handle_at(http_default_serve_mux, (pattern), (h), __FILE__, \
                                     __LINE__)
#define http_handle_func(pattern, f)                                                   \
    burrow__http_serve_mux_handle_func_at(http_default_serve_mux, (pattern), (f),      \
                                          __FILE__, __LINE__)

void burrow__http_serve_mux_handle_at(HttpServeMux *mux, Str pattern, HttpHandler h,
                                      const char *file, Int line);
void burrow__http_serve_mux_handle_func_at(HttpServeMux *mux, Str pattern,
                                           HttpHandlerFunc f, const char *file,
                                           Int line);

/* ServeMux.Handler. The handler for r, by its method, host and URL path,
 * which is never nil, and in *pattern the pattern it was registered with.
 * pattern may be NULL. A host's port is left out when matching.
 *
 * A path that is not clean, or that needs a slash on the end, gets a handler
 * that redirects, and the pattern that will match after the redirect. A
 * request that nothing matches gets a 404 or 405 handler and "". CONNECT
 * requests are matched with the path and host as they are.
 *
 * r is left as it is, so http_request_path_value does not see the wildcards.
 * The handlers made here, and the strings they hold, come from a. */
BURROW_BORROWS(ret, mux) HttpHandler http_serve_mux_handler(HttpServeMux *mux,
                                                            const HttpRequest *r,
                                                            Alloc *a, Str *pattern);

/* ServeMux.ServeHTTP. Sends r to the handler for it, after setting r's pattern
 * and what its wildcards matched. A request for "*" gets a 400, with
 * Connection: close for HTTP/1.1 and later. What the match needs is made in
 * r's arena, so for a request http_read_request or http_new_request made it
 * lasts as long as r. A request with no allocator of its own gets a scratch
 * arena that goes when the handler returns, and its pattern and wildcards are
 * cleared again then. */
void http_serve_mux_serve_http(HttpServeMux *mux, HttpResponseWriter w, HttpRequest *r);

/* The mux as a Handler, which calls http_serve_mux_serve_http. */
BURROW_BORROWS(ret, mux) HttpHandler http_serve_mux_as_handler(HttpServeMux *mux);

/* -------------------------------------------------------------------- Files */

/* http.File, what an HttpFileSystem's open gives and a file server serves. Its
 * methods are an OsFile's: read and close, seek, readdir, which gives up to
 * count entries as a Slice of FsFileInfo and everything left when count <= 0,
 * and stat.
 *
 * read_dir is fs.ReadDirFile's method, which Go finds with a type assertion,
 * and may be NULL. A directory listing uses it when it is there, since it does
 * not need a stat of every entry, and readdir when it is not. It gives a Slice
 * of FsDirEntry the way readdir gives FsFileInfo.
 *
 * What a file needs comes from the allocator open was given, and closing it
 * gives back whatever it holds that is not memory, such as a descriptor. The
 * memory goes with the allocator, so an arena is the usual one, and the file
 * server opens every file in an arena it frees when it is done. */
typedef struct HttpFileVT {
    IoReadCloserVT read_closer;
    int64_t (*seek)(void *self, int64_t offset, int whence, Error *err);
    Slice (*readdir)(void *self, Alloc *a, Int count, Error *err); /* of FsFileInfo */
    FsFileInfo (*stat)(void *self, Alloc *a, Error *err);
    Slice (*read_dir)(void *self, Alloc *a, Int count, Error *err); /* of FsDirEntry */
} HttpFileVT;

typedef struct HttpFile {
    const HttpFileVT *vt;
    void *data;
} HttpFile;

/* http.FileSystem, a set of files by name. The names are separated by '/'
 * whatever the system's separator is. open makes the file in a. */
typedef struct HttpFileSystemVT {
    const Type *self_type;
    HttpFile (*open)(void *self, Alloc *a, Str name, Error *err);
} HttpFileSystemVT;

typedef struct HttpFileSystem {
    const HttpFileSystemVT *vt;
    void *data;
} HttpFileSystem;

/* http.Dir, the files under a directory of the system's own, named with its
 * own separator. An empty Dir is ".".
 *
 * A Dir follows symbolic links, including ones that lead out of it, and serves
 * names that start with a dot, such as .git and .htpasswd, so it is not for a
 * directory where somebody else can make files. */
typedef Str HttpDir;

extern const Type *const TYPE_HTTP_DIR;

/* Dir.Open. name, cleaned as a '/' path from the root so that ".." cannot
 * climb out, under d, opened with os_open. The file is the OsFile, made in a,
 * and closing it closes and frees the OsFile, so it is not to be used again.
 * A name the system cannot have, such as one with a NUL or, on Windows, a
 * reserved name, fails with an error saying "http: invalid or unsafe file
 * path". When a file that is not a directory is on the way to name, the error
 * is fs_err_not_exist rather than whatever the system said. */
HttpFile http_dir_open(HttpDir d, Alloc *a, Str name, Error *err);

/* d as an HttpFileSystem, borrowing it. */
BURROW_BORROWS(ret, d) HttpFileSystem http_dir_as_file_system(const HttpDir *d);

/* http.FS. fsys as an HttpFileSystem, made in a, with NULL data when a says no.
 * A name of "/" is "." and any other loses its leading '/' on the way to fsys.
 * The files have to have a Seek method, which is looked for in the method set
 * of the type that their reader vtable names. A file without one serves an
 * error rather than its bytes. */
BURROW_OWNS(ret) HttpFileSystem http_fs(Alloc *a, Fs fsys);

/* http.FileServer. A handler that serves the files in root, made in a, with
 * NULL data when a says no.
 *
 * The request path is cleaned and served from root. A directory is served as
 * its index.html when it has one and as a listing of its entries when it does
 * not, and a path that ends in "/index.html" is redirected to the directory.
 * Directories are redirected to their path with a '/' at the end, and files
 * to theirs without. Everything else is http_serve_content's.
 *
 * A request path that does not start with '/' gets one. When the request has
 * its own arena, from http_read_request or a mux, the new path stays in the
 * request as it does in Go, and otherwise the path is put back as it was once
 * the handler is done. */
BURROW_OWNS(ret) HttpHandler http_file_server(Alloc *a, HttpFileSystem root);

/* http.FileServerFS, http_file_server(a, http_fs(a, root)). */
BURROW_OWNS(ret) HttpHandler http_file_server_fs(Alloc *a, Fs root);

/* http.ServeContent. Replies to r with what content has, which is found by
 * seeking to its end and back to the start, and handles Range, If-Match,
 * If-Unmodified-Since, If-None-Match, If-Modified-Since and If-Range.
 *
 * A response with no Content-Type gets one from the extension of name, or
 * from http_detect_content_type of the first 512 bytes when the extension says
 * nothing. A Content-Type key with no values stops that. name is not used
 * otherwise and can be empty. A modtime that is neither zero nor the Unix
 * epoch goes out as Last-Modified, and decides If-Modified-Since. An ETag set
 * in w's header, in RFC 7232's form, decides If-Match, If-None-Match and
 * If-Range.
 *
 * Several ranges go out as multipart/byteranges. Go makes those in a goroutine
 * that writes into a pipe, and they are written straight to w here, with the
 * same bytes. When serving fails, such as on a range that does not fit,
 * Cache-Control, Content-Encoding, Etag and Last-Modified come out of w's
 * header before the error is written, unless GODEBUG has
 * httpservecontentkeepheaders=1. */
void http_serve_content(HttpResponseWriter w, HttpRequest *r, Str name, Time modtime,
                        IoReadSeeker content);

/* http.ServeFile. Replies to r with the named file or directory of the
 * system, the way http_file_server does, except that a directory is not
 * redirected to its path with a '/' and a file is not redirected to its path
 * without one. name is relative to the current directory when it is not
 * absolute, and may climb out of it, so a name from the request has to be
 * checked first. A request path with a ".." element is refused with a 400, in
 * case name was made from it. A request path that ends in "/index.html" is
 * redirected as http_file_server does. */
void http_serve_file(HttpResponseWriter w, HttpRequest *r, Str name);

/* http.ServeFileFS. http_serve_file with the named file of fsys, whose files
 * have to have a Seek method as http_fs says. */
void http_serve_file_fs(HttpResponseWriter w, HttpRequest *r, Fs fsys, Str name);

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

/* ------------------------------------------------------------------- Server */

/* http.ErrServerClosed, from http_server_serve and
 * http_server_listen_and_serve once the server is closed or shutting down. */
extern const Error http_err_server_closed;

/* http.ErrHijacked, from writing to a response whose connection a handler has
 * hijacked, and from hijacking it again. */
extern const Error http_err_hijacked;

/* http.ErrContentLength, from writing more than the Content-Length the
 * handler set. */
extern const Error http_err_content_length;

/* http.ErrAbortHandler. A handler that panics with it, as an Error, ends the
 * response and the connection without the server logging the panic:
 *
 *     panic(BURROW_ANY(TYPE_ERROR, &http_err_abort_handler));
 */
extern const Error http_err_abort_handler;

/* http.ErrNotSupported, which HttpResponseController's functions wrap when the
 * writer does not have the method. Its text is "feature not supported". */
extern const Error http_err_not_supported;

/* http.DefaultMaxHeaderBytes and DefaultMaxHeaderValueCount, the most bytes a
 * request's first line and header may have, and the most header values, when
 * the server does not say. */
#define HTTP_DEFAULT_MAX_HEADER_BYTES ((Int)1 << 20)
#define HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT ((Int)500)

/* http.ConnState, where a connection to a server is in its life. A connection
 * is new when it has been accepted and nothing has been read from it, active
 * once a byte of a request has come, idle when it is waiting for the next
 * request, hijacked when a handler has taken it, and closed. Hijacked and
 * closed are where a connection ends. */
typedef enum HttpConnState {
    HTTP_STATE_NEW,
    HTTP_STATE_ACTIVE,
    HTTP_STATE_IDLE,
    HTTP_STATE_HIJACKED,
    HTTP_STATE_CLOSED,
} HttpConnState;

/* ConnState.String, such as "idle", and "" for a state that is not one. */
BURROW_STATIC(ret) Str http_conn_state_string(HttpConnState s);

/* Server.ConnState, called each time a connection changes state. */
BURROW_FUNC(HttpConnStateFunc, void, NetConn c, HttpConnState state);

/* Server.BaseContext, the context the requests on l start from. It may not
 * give the nil Context, and what it gives has to outlive every connection l
 * accepts. */
BURROW_FUNC(HttpBaseContextFunc, Context, NetListener l);

/* Server.ConnContext, the context for the requests on c, made from ctx. It may
 * not give the nil Context, and what it gives has to outlive c. */
BURROW_FUNC(HttpConnContextFunc, Context, Context ctx, NetConn c);

/* http.Server, an HTTP/1 server. Fill in the fields wanted and leave the rest
 * zero, which is Go's &http.Server{...}:
 *
 *     HttpServer srv = {.addr = BURROW_S(":8080"), .handler = h};
 *     Error err = http_server_listen_and_serve(&srv);
 *     http_server_free(&srv);
 *
 * addr is where http_server_listen_and_serve listens, ":http" when it is "".
 * handler answers every request, and with no vt it is
 * http_default_serve_mux. An OPTIONS request for "*" gets a 200 with no body
 * instead, unless disable_general_options_handler is set.
 *
 * read_timeout is how long reading a request may take, body and all, and
 * read_header_timeout the time for the first line and the header, which is
 * read_timeout when it is zero. write_timeout is how long writing a response
 * may take, from the end of reading the request's header. idle_timeout is how
 * long a connection waits for the next request, read_timeout when it is zero.
 * Zero, or less, is no limit for each of them.
 *
 * max_header_bytes is the most a request's first line and header may have,
 * HTTP_DEFAULT_MAX_HEADER_BYTES when it is zero or less, and a request with
 * more gets a 431. max_header_value_count is the most header values,
 * HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT when it is zero or less.
 *
 * conn_state, base_context and conn_context are the hooks of those names, and
 * error_log is where the server logs what goes wrong with a connection, and
 * log's standard logger when it is NULL. protocols is the protocols the
 * server speaks, and only HTTP/1 is here so far, so a server whose protocols
 * leave it out serves nothing. NULL, or the empty set, is HTTP/1.
 *
 * a is where the server makes each connection and request, and the heap when
 * it is NULL. It has to be one that any goroutine can use at once.
 *
 * A connection is served on a goroutine of its own and freed when it is done,
 * and so is a connection a NetTCPListener or a NetUnixListener accepted. One
 * from another kind of listener is closed but stays the listener's to free.
 * The rest of the struct is the server's own, and the server is not to be
 * copied once it has started. */
typedef struct HttpServer {
    Str addr;
    HttpHandler handler;
    bool disable_general_options_handler;
    Duration read_timeout;
    Duration read_header_timeout;
    Duration write_timeout;
    Duration idle_timeout;
    Int max_header_bytes;
    Int max_header_value_count;
    HttpConnStateFunc conn_state;
    LogLogger *error_log;
    HttpBaseContextFunc base_context;
    HttpConnContextFunc conn_context;
    const HttpProtocols *protocols;
    Alloc *a;

    /* The server's own. */
    SyncAtomicBool in_shutdown;
    SyncAtomicBool disable_keep_alives;
    SyncMutex mu;
    struct burrow__HttpServeListener *listeners;
    struct burrow__HttpServeConn *active_conn;
    Slice on_shutdown; /* of Func */
    SyncWaitGroup listener_group;
    SyncWaitGroup conn_group;
} HttpServer;

/* Server.Serve. Accepts connections on l and serves each on a goroutine of
 * its own, until l fails. A failure that is temporary is tried again after a
 * pause. Closes l before it returns. The result is http_err_server_closed after
 * http_server_close or http_server_shutdown, and otherwise the error from the
 * accept. */
BURROW_BORROWS(ret) Error http_server_serve(HttpServer *s, NetListener l);

/* Server.ListenAndServe. Listens on TCP at the server's addr and serves the
 * connections with http_server_serve. A host in addr is an IP address or a
 * name from the hosts file, and the port a number or a service such as "http".
 * Its result is never BURROW_NO_ERROR, and is http_err_server_closed after
 * http_server_close or http_server_shutdown. */
BURROW_OWNS(ret) Error http_server_listen_and_serve(HttpServer *s);

/* Server.Close. Closes every listener and every connection now, including
 * those with a request being handled, but not the ones hijacked. Waits for
 * the listeners' http_server_serve calls to stop accepting. The result is the
 * first error from closing a listener. */
BURROW_BORROWS(ret) Error http_server_close(HttpServer *s);

/* Server.Shutdown. Closes every listener, then closes the idle connections
 * and waits, checking more and more slowly, for the others to go idle. Gives
 * the first error from closing a listener once every connection is closed,
 * or ctx's error if ctx is done first, which leaves the rest open. Calls the
 * functions given to http_server_register_on_shutdown, each on a goroutine of
 * its own. http_server_serve returns at once with http_err_server_closed, so
 * a program has to wait for this to return before it ends. Hijacked
 * connections are not waited for. */
BURROW_BORROWS(ret) Error http_server_shutdown(HttpServer *s, Context ctx);

/* Server.RegisterOnShutdown. f runs when http_server_shutdown is called, to
 * close what a connection that has been hijacked, or one with some other
 * protocol, has open. False when the server's allocator says no. */
bool http_server_register_on_shutdown(HttpServer *s, Func f);

/* Server.SetKeepAlivesEnabled. Whether a connection is kept open for more
 * than one request, which is the default. Turning it off closes the idle
 * connections. */
void http_server_set_keep_alives_enabled(HttpServer *s, bool v);

/* Waits for the goroutine of every connection the server has served to end,
 * and gives back what the server holds. Not while it is serving, so after
 * http_server_serve has returned and http_server_close or
 * http_server_shutdown has closed the connections, or the wait is for clients
 * to hang up. A handler still running keeps it waiting. */
void http_server_free(HttpServer *s);

/* http.Serve and http.ListenAndServe, with a server that has only a handler,
 * and the connections made in the heap. A handler with no vt is
 * http_default_serve_mux. */
BURROW_BORROWS(ret) Error http_serve(NetListener l, HttpHandler handler);
BURROW_OWNS(ret) Error http_listen_and_serve(Str addr, HttpHandler handler);

/* http.ServerContextKey and LocalAddrContextKey, the keys of the request
 * context's values for the HttpServer, as TYPE_HTTP_SERVER and a pointer to
 * it, and for the local address of the connection the request came on, as the
 * NetAddr's type and data. */
extern const Any http_server_context_key;
extern const Any http_local_addr_context_key;

extern const Type *const TYPE_HTTP_SERVER;

/* http.ResponseController, which reaches the methods a ResponseWriter may
 * have beyond the three every one has, by way of unwrap when the writer
 * wraps another. Each function gives an error wrapping
 * http_err_not_supported when no writer in the chain has the method. */
typedef struct HttpResponseController {
    HttpResponseWriter rw;
} HttpResponseController;

/* http.NewResponseController. */
HttpResponseController http_new_response_controller(HttpResponseWriter rw);

/* ResponseController.Flush, which sends what has been written to the client. */
BURROW_BORROWS(ret) Error http_response_controller_flush(HttpResponseController *c);

/* ResponseController.Hijack. Takes the connection from the server, which
 * neither writes to it nor reads from it after this, and gives it with *buf,
 * the server's reader, which may already hold bytes from the client, and its
 * writer. They are the caller's to close and free: buf->reader with
 * bufio_reader_free, buf->writer with bufio_writer_free, and the connection the
 * way its listener's connections are, such as net_tcp_conn_free on
 * net_conn_as_tcp_conn. The request is the server's still, and is gone once
 * the handler returns. */
BURROW_OWNS(ret) NetConn http_response_controller_hijack(HttpResponseController *c,
                                                         BufioReadWriter *buf,
                                                         Error *err);

/* ResponseController.SetReadDeadline and SetWriteDeadline. The time by which
 * the rest of the request's body has to be read, or the response written. A
 * deadline in the past makes the reads or the writes fail at once, and the
 * zero Time is none. */
BURROW_BORROWS(ret) Error
http_response_controller_set_read_deadline(HttpResponseController *c, Time deadline);
BURROW_BORROWS(ret) Error
http_response_controller_set_write_deadline(HttpResponseController *c, Time deadline);

/* ResponseController.EnableFullDuplex. Lets the handler read the request's
 * body while it writes the response, which an HTTP/1 server otherwise does
 * not, since a client may not read until it has sent everything. */
BURROW_BORROWS(ret) Error
http_response_controller_enable_full_duplex(HttpResponseController *c);

/* http.TimeoutHandler. A handler that gives h dt to answer, and replies with
 * a 503 and msg when it takes longer, or a short HTML page when msg is "".
 * h writes to a writer of the handler's own, which keeps what h writes until
 * h returns and then sends it, and after the time is up gives h
 * http_err_handler_timeout. h can see the time running out in its request's
 * context. It does not have the methods HttpResponseController reaches.
 *
 * Go's goes on to the next request while h is still running. Here nothing
 * collects h's request once it is done with, so after sending the 503 the
 * handler waits for h to return. Made in a, and NULL data when a says no. msg
 * is not copied. */
BURROW_OWNS(ret) HttpHandler http_timeout_handler(Alloc *a, HttpHandler h, Duration dt,
                                                  Str msg);

/* http.ErrHandlerTimeout, which a handler under http_timeout_handler gets from
 * writing once its time is up. */
extern const Error http_err_handler_timeout;

/* http.MaxBytesError, what reading past the limit of http_max_bytes_reader
 * gives. Its text is "http: request body too large", and errors_as with
 * TYPE_HTTP_MAX_BYTES_ERROR gives the limit. */
typedef struct HttpMaxBytesError {
    int64_t limit;
} HttpMaxBytesError;

extern const Type *const TYPE_HTTP_MAX_BYTES_ERROR;

/* http.MaxBytesReader. A reader of r that gives an HttpMaxBytesError after n
 * bytes, or after none when n is below zero, and tells the server w belongs to
 * to close the connection after the response. Closing it closes r. Made in a,
 * which has to last as long as it does, so the request's arena is the place
 * for one made in a handler. Nil when a says no. */
BURROW_OWNS(ret) IoReadCloser http_max_bytes_reader(Alloc *a, HttpResponseWriter w,
                                                    IoReadCloser r, int64_t n);

/* http.MaxBytesHandler. A handler that calls h with the request's body wrapped
 * by http_max_bytes_reader, with n. Made in a, and NULL data when a says no. */
BURROW_OWNS(ret) HttpHandler http_max_bytes_handler(Alloc *a, HttpHandler h, int64_t n);

/* http.AllowQuerySemicolons. A handler that calls h with each ";" in the
 * request URL's raw query turned into "&", for the old way of separating query
 * parameters. Made in a, and NULL data when a says no. */
BURROW_OWNS(ret) HttpHandler http_allow_query_semicolons(Alloc *a, HttpHandler h);

/* ------------------------------------------------------------ RoundTripper */

/* http.RoundTripper, which sends one request and gives back its response,
 * without following redirects or handling cookies or authentication, which is
 * what an HttpClient does on top of one.
 *
 * round_trip gives a response or an error, never both, and a response with a
 * status that is not 2xx is not an error. It does not change the request,
 * though it may read and close its body, which it closes even on an error,
 * and may do so on another goroutine after it has returned. The request has to
 * outlive the response, which points to it, and the response is the caller's
 * to free with http_response_free, after reading and closing the body for the
 * connection to be used again. The error is in the caller's
 * error_allocator. */
typedef struct HttpRoundTripperVT {
    const Type *self_type;
    HttpResponse *(*round_trip)(void *self, HttpRequest *req, Error *err);
} HttpRoundTripperVT;

typedef struct HttpRoundTripper {
    const HttpRoundTripperVT *vt;
    void *data;
} HttpRoundTripper;

/* RoundTripper.RoundTrip. */
BURROW_OWNS(ret) static inline HttpResponse *
http_round_tripper_round_trip(HttpRoundTripper rt, HttpRequest *req, Error *err) {
    return rt.vt->round_trip(rt.data, req, err);
}

/* http.ErrSkipAltProtocol, which a round tripper registered with
 * http_transport_register_protocol gives to have the transport send the
 * request itself. */
extern const Error http_err_skip_alt_protocol;

/* ---------------------------------------------------------------- Transport */

/* http.DefaultMaxIdleConnsPerHost, the idle connections a transport keeps for
 * each host when max_idle_conns_per_host is zero. */
#define HTTP_DEFAULT_MAX_IDLE_CONNS_PER_HOST ((Int)2)

/* Transport.Proxy. The proxy for req, made in a, or NULL for none. Only an
 * "http" proxy can be used so far, and "https" and "socks5" ones give an
 * error when a request goes to them. */
BURROW_FUNC(HttpProxyFunc, Url *, HttpRequest *req, Alloc *a, Error *err);

/* Transport.DialContext, DialTLSContext, Dial and DialTLS. The connection is
 * the transport's from then on, and it closes it and gives it back with the
 * transport's free_conn. */
BURROW_FUNC(HttpDialContextFunc, NetConn, Context ctx, Str network, Str addr,
            Error *err);
BURROW_FUNC(HttpDialFunc, NetConn, Str network, Str addr, Error *err);

/* Transport.OnProxyConnectResponse, called with the response to each CONNECT
 * request to a proxy. An error ends the request with that error. */
BURROW_FUNC(HttpOnProxyConnectResponseFunc, Error, Context ctx, const Url *proxy_url,
            HttpRequest *connect_req, HttpResponse *connect_res);

/* Transport.GetProxyConnectHeader. The header to send in a CONNECT request to
 * proxy_url for target, made in a. */
BURROW_FUNC(HttpGetProxyConnectHeaderFunc, HttpHeader, Context ctx, Alloc *a,
            const Url *proxy_url, Str target, Error *err);

/* What gives back a connection the transport is done with, after closing it. */
BURROW_FUNC(HttpFreeConnFunc, void, NetConn c);

/* http.Transport, an HTTP/1 round tripper that keeps connections open for
 * more requests and reuses them. Fill in the fields wanted and leave the rest
 * zero, as with Go's &http.Transport{...}, and free it with
 * http_transport_free. It may be used from any number of goroutines at once,
 * and is not to be copied once it has been used.
 *
 * proxy picks the proxy for a request, and none is used when it is nil.
 * http_proxy_from_environment is the one http_default_transport has.
 * proxy_connect_header is sent in each CONNECT request, and
 * get_proxy_connect_header, when it is set, makes the header instead.
 *
 * dial_context makes the TCP connections, and dial when that is nil, and
 * net_dialer_dial_context with a zero NetDialer when both are. dial_tls_context
 * and dial_tls make the connections for an "https" request without a proxy,
 * and the transport does nothing more with them, so they are where TLS comes
 * from until crypto/tls is here. Without them an "https" request fails.
 * free_conn gives back each connection, and is net_conn_free when it is nil.
 *
 * disable_keep_alives sends each request on a new connection, and closes it
 * after. disable_compression stops the transport asking for gzip. When it asks
 * on its own, it takes the gzip off a gzipped response, and the response says
 * so in uncompressed.
 *
 * max_idle_conns is the most idle connections over all hosts, and zero is no
 * limit. max_idle_conns_per_host is the most for one host, with
 * HTTP_DEFAULT_MAX_IDLE_CONNS_PER_HOST for zero. max_conns_per_host is the
 * most for one host whatever they are doing, and requests past it wait for
 * one. idle_conn_timeout is how long an idle connection is kept.
 * response_header_timeout is how long the transport waits for a response's
 * header once the request is written, and expect_continue_timeout how long it
 * waits for a "100 Continue" before sending a body after "Expect:
 * 100-continue". Zero is no limit for each of them, and for
 * expect_continue_timeout it means the body is sent at once.
 *
 * max_response_header_bytes is the most a response's header may have, and 10
 * MiB when it is zero. write_buffer_size and read_buffer_size are the sizes of
 * the buffers on each connection, 4 KiB when they are zero. protocols is the
 * set of protocols to use. NULL is HTTP/1, and HTTP/2 is not here yet, so a set
 * without HTTP/1 can send nothing.
 *
 * a is where the transport makes its connections and the responses, the heap
 * when it is NULL, and has to be one any goroutine can use at once. The rest is
 * the transport's own. */
typedef struct HttpTransport {
    HttpProxyFunc proxy;
    HttpOnProxyConnectResponseFunc on_proxy_connect_response;
    HttpDialContextFunc dial_context;
    HttpDialFunc dial;
    HttpDialContextFunc dial_tls_context;
    HttpDialFunc dial_tls;
    HttpFreeConnFunc free_conn;
    bool disable_keep_alives;
    bool disable_compression;
    Int max_idle_conns;
    Int max_idle_conns_per_host;
    Int max_conns_per_host;
    Duration idle_conn_timeout;
    Duration response_header_timeout;
    Duration expect_continue_timeout;
    HttpHeader proxy_connect_header;
    HttpGetProxyConnectHeaderFunc get_proxy_connect_header;
    int64_t max_response_header_bytes;
    Int write_buffer_size;
    Int read_buffer_size;
    const HttpProtocols *protocols;
    Alloc *a;

    /* The transport's own. */
    SyncMutex idle_mu;
    bool close_idle;
    struct burrow__HttpIdleBucket *idle;
    struct burrow__HttpPConn *lru_head, *lru_tail;
    Int lru_len;
    SyncMutex req_mu;
    struct burrow__HttpCall *calls;
    SyncMutex alt_mu;
    struct burrow__HttpAltProto *alt;
    SyncMutex conns_per_host_mu;
    struct burrow__HttpHostBucket *conns_per_host;
    struct burrow__HttpWant *dials_head, *dials_tail;
    SyncWaitGroup live;
} HttpTransport;

/* Transport.RoundTrip. Sends req and gives back its response, as an
 * HttpRoundTripper does. Only HTTP/1 is spoken. A request on a connection
 * that turns out to have been closed by the server is sent again on another
 * one when that is safe, which it is for a request with no body or one
 * get_body can make again, and an idempotent method. */
BURROW_OWNS(ret) HttpResponse *http_transport_round_trip(HttpTransport *t,
                                                         HttpRequest *req, Error *err);

/* The transport as an HttpRoundTripper. */
BURROW_BORROWS(ret, t) HttpRoundTripper
http_transport_as_round_tripper(HttpTransport *t);

/* Transport.CloseIdleConnections. Closes the connections that are idle now,
 * but not ones in use. */
void http_transport_close_idle_connections(HttpTransport *t);

/* Transport.CancelRequest. Cancels a request in flight, which Go deprecates
 * in favour of the request's context, and which is all a round tripper that is
 * not this one sees of a client's timeout. */
void http_transport_cancel_request(HttpTransport *t, HttpRequest *req);

/* Transport.RegisterProtocol. Sends requests for scheme to rt instead, such
 * as "file" ones to http_new_file_transport. rt can give
 * http_err_skip_alt_protocol to hand a request back. The scheme may only be
 * registered once, and false is what doing it again gives, or the allocator
 * saying no. rt has to outlive the transport. */
bool http_transport_register_protocol(HttpTransport *t, Str scheme,
                                      HttpRoundTripper rt);

/* Transport.Clone. A transport with the same settings as t and none of its
 * connections, with proxy_connect_header copied into a. Its proxy_connect_header
 * is nil when a says no. */
HttpTransport http_transport_clone(const HttpTransport *t, Alloc *a);

/* Closes the idle connections and waits for every connection, dial and
 * goroutine the transport has going to end. A response that is not freed yet
 * keeps it waiting, so free them all first. */
void http_transport_free(HttpTransport *t);

/* http.DefaultTransport, which has proxy set to http_proxy_from_environment,
 * a dialer with a 30 second timeout and keep-alive, max_idle_conns of 100, an
 * idle_conn_timeout of 90 seconds and an expect_continue_timeout of 1 second.
 * It lives as long as the program. */
extern HttpTransport *const http_default_transport;

/* http.ProxyFromEnvironment. The proxy for req from the environment:
 * HTTP_PROXY for an "http" request and HTTPS_PROXY for an "https" one, or the
 * same names in lower case, unless NO_PROXY, or no_proxy, says not to. A value
 * may be a whole URL, or a host and port taken as "http". NULL for no proxy,
 * which a request for localhost or a loopback address never has. The
 * environment is read once, the first time it is wanted, and the URL is made
 * in a. */
BURROW_OWNS(ret) Url *http_proxy_from_environment(HttpRequest *req, Alloc *a,
                                                  Error *err);

/* http_proxy_from_environment as the HttpProxyFunc a transport takes. */
HttpProxyFunc http_proxy_from_environment_func(void);

/* http.ProxyURL. A proxy function that gives a copy of u, which has to outlive
 * it, for every request. */
HttpProxyFunc http_proxy_url(const Url *u);

/* ------------------------------------------------------------------- Client */

/* http.ErrUseLastResponse, which check_redirect gives to stop following
 * redirects and hand back the last response, with its body still to read. */
extern const Error http_err_use_last_response;

/* http.ErrSchemeMismatch, which a client gives when an "https" request gets
 * back what looks like an HTTP response. */
extern const Error http_err_scheme_mismatch;

/* Client.CheckRedirect. Says whether to follow the redirect to req, after the
 * requests in via, a Slice of HttpRequest pointers with the oldest first. Its
 * error comes back from http_client_do wrapped in a UrlError, with the last
 * response's body closed, unless it is http_err_use_last_response. */
BURROW_FUNC(HttpCheckRedirectFunc, Error, HttpRequest *req, Slice via);

/* http.Client, which sends requests with its transport and follows the
 * redirects that come back, with the cookies in jar. Fill in the fields wanted
 * and leave the rest zero, as with Go's &http.Client{...}. It may be used from
 * any number of goroutines at once.
 *
 * transport sends each request, and is http_default_transport when it has no
 * vt. check_redirect says whether to follow a redirect, and when it is nil the
 * client follows up to ten of them. jar, when it has a vt, is given the
 * cookies of each response and adds its cookies to each request. timeout is
 * the most a request may take, from sending it to reading the end of the
 * response's body, and zero is no limit. a is where the client makes the
 * requests it sends on redirects, the heap when it is NULL, and has to be one
 * any goroutine can use at once. */
typedef struct HttpClient {
    HttpRoundTripper transport;
    HttpCheckRedirectFunc check_redirect;
    HttpCookieJar jar;
    Duration timeout;
    Alloc *a;
} HttpClient;

/* Client.Do. Sends req and follows redirects, as the client's fields say, and
 * gives back the last response. Redirects of 301, 302 and 303 become a GET, or
 * a HEAD for a HEAD, with no body. 307 and 308 keep the method and the body,
 * when get_body can make it again. The header goes along to the same domain
 * and its subdomains, and Authorization, Cookie and the like no further.
 *
 * The error is a UrlError, in the caller's error_allocator, and there is no
 * response with it, except that check_redirect giving one closes the last
 * response's body and gives that response back with the error. A status that
 * is not 2xx is not an error. req's body is closed, even on an error, and req
 * has to outlive the response, which may point to a request the client made
 * for a redirect and frees with the response. */
BURROW_OWNS(ret) HttpResponse *http_client_do(HttpClient *c, HttpRequest *req,
                                              Error *err);

/* Client.Get, Head, Post and PostForm. A request made with http_new_request
 * and sent with http_client_do, made in a and freed with the response, or on
 * an error. content_type is the Content-Type of a POST, and data is a url's
 * UrlValues, sent as "application/x-www-form-urlencoded". */
BURROW_OWNS(ret) HttpResponse *http_client_get(HttpClient *c, Str url, Error *err);
BURROW_OWNS(ret) HttpResponse *http_client_head(HttpClient *c, Str url, Error *err);
BURROW_OWNS(ret) HttpResponse *
http_client_post(HttpClient *c, Str url, Str content_type, IoReader body, Error *err);
BURROW_OWNS(ret) HttpResponse *http_client_post_form(HttpClient *c, Str url,
                                                     UrlValues data, Error *err);

/* Client.CloseIdleConnections, which closes the idle connections of the
 * transport when it has a way to. */
void http_client_close_idle_connections(HttpClient *c);

/* http.DefaultClient, a client with every field zero, and http.Get, Head,
 * Post and PostForm, which use it. */
extern HttpClient *const http_default_client;
BURROW_OWNS(ret) HttpResponse *http_get(Str url, Error *err);
BURROW_OWNS(ret) HttpResponse *http_head(Str url, Error *err);
BURROW_OWNS(ret) HttpResponse *http_post(Str url, Str content_type, IoReader body,
                                         Error *err);
BURROW_OWNS(ret) HttpResponse *http_post_form(Str url, UrlValues data, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_H */
