/* net/url, URLs and query strings.
 *
 * Go's net/url. url_parse splits a URL into its parts, url_string puts one
 * back together, and the escape functions do the percent encoding that paths
 * and query strings need. A Url is the parsed form, a UrlValues is a query
 * string as a map from each key to its values.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err;
 *     Url *u = url_parse(a, BURROW_S("https://go.dev/search?q=url&m=text"), &err);
 *     if (BURROW_OK(err)) {
 *         UrlValues q = url_query(u, a);
 *         Str what = url_values_get(q, BURROW_S("q"));   // "url"
 *         ...
 *     }
 *     arena_free(&ar);
 *
 * The parse follows RFC 3986 where Go does and departs from it where Go does,
 * so what Go accepts this accepts, and it gives the same fields.
 *
 * A Url that comes out of this package owns its text. url_parse,
 * url_parse_ref, url_resolve_reference, url_join_path and url_clone each make
 * one allocation that holds the Url and every string in it, so the input can go
 * away while the Url lives, and url_free gives it back. A Url you fill in
 * yourself is yours to manage, and the functions here only read it.
 *
 * A UrlValues is a Map, which is many small allocations and strings that may
 * point into the query they came from. An arena is the easy way to hold one.
 *
 * Two of Go's GODEBUG settings apply. urlstrictcolons=0 lets an http or https
 * host have more than one colon, and urlmaxqueryparams=N changes the cap of
 * 10000 parameters that url_parse_query puts on a query, with 0 for no cap.
 * GODEBUG is read the first time either is needed and not again.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/url */

#ifndef BURROW_NET_URL_H
#define BURROW_NET_URL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- errors */

/* url.Error, which is what url_parse and url_parse_request_uri fail with. op
 * is "parse", url is the input and err is the reason. errors_is sees through
 * to err, and errors_as with TYPE_URL_ERROR gets you the struct. */
typedef struct UrlError {
    Str op;
    Str url;
    Error err;
} UrlError;

extern const Type *const TYPE_URL_ERROR;

/* The text Go's Error method gives, such as
 *
 *     parse "http://[::1": missing ']' in host
 *
 * built in a. For an error that came out of a parse, error_text gives the same
 * text with no allocator. */
BURROW_OWNS(ret) Str url_error_error(const UrlError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error url_error_unwrap(const UrlError *e);

/* url.Error.Timeout and Temporary. Whether e->err has a Timeout or a Temporary
 * method returning true, found the way errors_as finds things, through the
 * methods on the error's type. */
bool url_error_timeout(const UrlError *e);
bool url_error_temporary(const UrlError *e);

/* An Error for a UrlError you filled in yourself. op, url and the message are
 * copied into a, and err is kept as it is, so it has to live as long as the
 * result does. An allocation failure gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error url_error_as_error(const UrlError *e, Alloc *a);

/* url.EscapeError, the bad escape itself, such as "%zz". Its text is
 * "invalid URL escape " and the escape quoted. Two with the same text are
 * errors_is each other, as the Go values are ==. */
typedef Str UrlEscapeError;

extern const Type *const TYPE_URL_ESCAPE_ERROR;

BURROW_OWNS(ret) Str url_escape_error_error(UrlEscapeError e, Alloc *a);

/* The Error for e, which is what Go gets by converting it to error. e is
 * copied into a. */
BURROW_OWNS(ret) Error url_escape_error_as_error(UrlEscapeError e, Alloc *a);

/* url.InvalidHostError, the character a host cannot have. Its text is
 * "invalid character " and the character quoted, then " in host name". */
typedef Str UrlInvalidHostError;

extern const Type *const TYPE_URL_INVALID_HOST_ERROR;

BURROW_OWNS(ret) Str url_invalid_host_error_error(UrlInvalidHostError e, Alloc *a);
BURROW_OWNS(ret) Error url_invalid_host_error_as_error(UrlInvalidHostError e, Alloc *a);

/* ----------------------------------------------------------------- escaping */

/* url.QueryEscape. s with everything a query component cannot hold escaped,
 * and a space as "+". Like Go, s itself comes back when nothing needs
 * escaping. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str url_query_escape(Alloc *a, Str s);

/* url.PathEscape. s escaped for one path segment, so "/" becomes "%2F" and a
 * space "%20". */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str url_path_escape(Alloc *a, Str s);

/* url.QueryUnescape. Each "%AB" turned into the byte 0xAB and each "+" into a
 * space. A "%" without two hex digits after it gives a UrlEscapeError and the
 * empty string. s itself comes back when there is nothing to undo. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str url_query_unescape(Alloc *a, Str s,
                                                               Error *err);

/* url.PathUnescape, which is url_query_unescape leaving "+" alone. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str url_path_unescape(Alloc *a, Str s,
                                                              Error *err);

/* ----------------------------------------------------------------- Userinfo */

/* url.Userinfo, a user name and maybe a password. The fields are here so the
 * struct has a size. Read them with the functions below, which take NULL as
 * Go's methods take a nil *Userinfo. */
typedef struct UrlUserinfo {
    Str username;
    Str password;
    bool password_set;
} UrlUserinfo;

/* url.User and url.UserPassword. One allocation of sizeof(UrlUserinfo) from
 * a, and the strings are kept as they are, not copied. NULL when a says no.
 * Passwords in URLs are a bad idea, as Go's documentation says at length. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, username) UrlUserinfo *url_user(Alloc *a,
                                                                     Str username);
BURROW_OWNS(ret) BURROW_BORROWS(ret, username, password) UrlUserinfo *
url_user_password(Alloc *a, Str username, Str password);

/* url.Userinfo.Username and Password. *ok says whether a password is set, and
 * may be NULL. */
BURROW_BORROWS(ret, u) Str url_userinfo_username(const UrlUserinfo *u);
BURROW_BORROWS(ret, u) Str url_userinfo_password(const UrlUserinfo *u, bool *ok);

/* url.Userinfo.String. "user" or "user:password", escaped. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, u) Str url_userinfo_string(const UrlUserinfo *u,
                                                                Alloc *a);

/* ---------------------------------------------------------------------- URL */

/* url.URL, a parsed URL, which is to say a URI reference. The general form is
 *
 *     [scheme:][//[userinfo@]host][/]path[?query][#fragment]
 *
 * and a URL that has no slash after the scheme is
 *
 *     scheme:opaque[?query][#fragment]
 *
 * host is "host" or "host:port", with brackets round an IPv6 address.
 * path and fragment are decoded, so "/%47%6f%2f" is "/Go/". raw_path and
 * raw_fragment keep the encoding when it is not the one url_escaped_path
 * would pick, and url_string uses them. raw_query is still encoded, and
 * url_query decodes it.
 *
 * mem and size are not yours. They say where the block a Url from this package
 * lives, and stay zero in one you fill in yourself. */
typedef struct Url {
    Str scheme;
    Str opaque;        /* encoded opaque data */
    UrlUserinfo *user; /* user name and password, or NULL */
    Str host;          /* "host" or "host:port" */
    Str path;          /* relative paths may leave out the leading slash */
    Str fragment;      /* after the '#' */
    Str raw_query;     /* encoded, without the '?' */
    Str raw_path;      /* the encoded path, as a hint; see url_escaped_path */
    Str raw_fragment;  /* the encoded fragment, as a hint */
    bool force_query;  /* a '?' with nothing after it */
    bool omit_host;    /* an empty host, left out when printing */

    void *mem;
    size_t size;
} Url;

extern const Type burrow_type_Url;
extern const Type *const TYPE_URL;

/* url.Parse. The URL may be relative, a path with no host, or absolute, with a
 * scheme. A host and path with no scheme is not valid, though the ambiguity
 * means it may not be reported. On an error the result is NULL and err is a
 * UrlError. NULL with no error means a said no. */
BURROW_OWNS(ret) Url *url_parse(Alloc *a, Str raw_url, Error *err);

/* url.ParseRequestURI. raw_url is taken to have come in an HTTP request, so
 * it has to be an absolute URL or an absolute path, and it has no fragment,
 * so a '#' is part of the path. */
BURROW_OWNS(ret) Url *url_parse_request_uri(Alloc *a, Str raw_url, Error *err);

/* Gives back a Url from this package, strings and all. NULL is fine, and so is
 * a Url you filled in yourself, which is left alone. */
void url_free(Alloc *a, Url *u);

/* url.URL.Clone. A deep copy in one allocation from a. NULL for NULL. */
BURROW_OWNS(ret) Url *url_clone(const Url *u, Alloc *a);

/* url.URL.String. The URL put back together: scheme:opaque?query#fragment
 * when there is opaque data and scheme://userinfo@host/path?query#fragment
 * otherwise, leaving out the parts that are empty. */
BURROW_OWNS(ret) Str url_string(const Url *u, Alloc *a);

/* url.URL.Redacted. url_string with the password, if there is one, as
 * "xxxxx". NULL gives the empty string. */
BURROW_OWNS(ret) Str url_redacted(const Url *u, Alloc *a);

/* url.URL.EscapedPath. raw_path when it is a valid encoding of path, and
 * path escaped otherwise. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, u) Str url_escaped_path(const Url *u, Alloc *a);

/* url.URL.EscapedFragment, the same for the fragment. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, u) Str url_escaped_fragment(const Url *u,
                                                                 Alloc *a);

/* url.URL.IsAbs. Whether there is a scheme. */
bool url_is_abs(const Url *u);

/* url.URL.Parse. ref parsed and then resolved against u with
 * url_resolve_reference. */
BURROW_OWNS(ret) Url *url_parse_ref(const Url *u, Alloc *a, Str ref, Error *err);

/* url.URL.ResolveReference. ref, which may be relative, made absolute against
 * the base u, as RFC 3986 section 5.2 says. Always a new Url, even when ref
 * was absolute and comes back unchanged. */
BURROW_OWNS(ret) Url *url_resolve_reference(const Url *u, Alloc *a, const Url *ref);

/* url.URL.RequestURI. The encoded path?query or opaque?query to put in an
 * HTTP request for u. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, u) Str url_request_uri(const Url *u, Alloc *a);

/* url.URL.Hostname and Port. host without the port and the brackets, and the
 * port without the colon. A port that is not all digits is not a port, so
 * Hostname keeps it and Port is empty. */
BURROW_BORROWS(ret, u) Str url_hostname(const Url *u);
BURROW_BORROWS(ret, u) Str url_port(const Url *u);

/* url.URL.MarshalBinary, AppendBinary and UnmarshalBinary. The binary form is
 * url_string. Unmarshal parses into a block from a and points *u at it, so
 * give it back with url_free. */
BURROW_OWNS(ret) Slice url_marshal_binary(const Url *u, Alloc *a, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice url_append_binary(const Url *u, Alloc *a,
                                                                Slice b, Error *err);
BURROW_BORROWS(ret) Error url_unmarshal_binary(Url *u, Alloc *a, Slice text);

/* url.URL.JoinPath. A new Url with the elements, a Slice of Str already
 * escaped, joined on to u's path, and the result cleaned of "." and ".."
 * elements and runs of slashes. A trailing slash on the last element stays. */
BURROW_OWNS(ret) Url *url_join_path(const Url *u, Alloc *a, Slice elem);

/* url_join_path with the elements as arguments:
 *
 *     Url *v = url_join_path_v(u, a, 2, BURROW_S("a"), BURROW_S("b/"));
 */
BURROW_OWNS(ret) Url *url_join_path_v(const Url *u, Alloc *a, int n, ...);

/* url.JoinPath, the function. base parsed, joined as url_join_path does and
 * printed. A base that does not parse gives its error and the empty string. */
BURROW_OWNS(ret) Str url_join_path_str(Alloc *a, Str base, Slice elem, Error *err);

/* ------------------------------------------------------------------- Values */

/* url.Values, map[string][]string: each key to its values, in order. Keys are
 * case sensitive. It is a Map from Str to a Slice of Str, so map_len,
 * map_iter and the rest work on it too, and NULL reads as empty. */
typedef Map *UrlValues;

/* make(url.Values). NULL when a says no. */
BURROW_OWNS(ret) UrlValues url_values_make(Alloc *a);

/* url.Values.Get. The first value for key, or "" when there is none. */
BURROW_BORROWS(ret, v) Str url_values_get(UrlValues v, Str key);

/* v[key], all the values for key, and an empty Slice when there are none. It
 * points into v and is good until v next changes. */
BURROW_BORROWS(ret, v) Slice url_values_get_all(UrlValues v, Str key);

/* url.Values.Set and Add. Set makes value the only value for key, and Add puts
 * it after the ones there are. key and value are kept as they are, not copied,
 * and the Slice comes from the allocator v was made with. False when that
 * allocator says no, and then v is as it was. */
bool url_values_set(UrlValues v, Str key, Str value);
bool url_values_add(UrlValues v, Str key, Str value);

/* url.Values.Del and Has. */
void url_values_del(UrlValues v, Str key);
bool url_values_has(UrlValues v, Str key);

/* url.Values.Clone. A copy made with a whose slices are copies too. The strings
 * are shared, as in Go. NULL for NULL. */
BURROW_OWNS(ret) UrlValues url_values_clone(UrlValues v, Alloc *a);

/* url.Values.Encode. "bar=baz&foo=quux", sorted by key and escaped. */
BURROW_OWNS(ret) Str url_values_encode(UrlValues v, Alloc *a);

/* url.ParseQuery. The pairs in query, which is key=value settings joined by
 * '&'. A setting with no '=' is a key with an empty value, and one with a bare
 * ';' in it is an error. The result has every pair that decoded, and err is
 * the first problem, so check both. Strings point into query or come from a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, query) UrlValues url_parse_query(Alloc *a,
                                                                      Str query,
                                                                      Error *err);

/* url.URL.Query. raw_query decoded, with the pairs that do not decode left out.
 * url_parse_query says what they were. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, u) UrlValues url_query(const Url *u, Alloc *a);

/* Not API. Uses value as GODEBUG from now on, for tests, or with NULL goes
 * back to reading the environment. */
void burrow__url_godebug_set(const char *value);

/* Not API. Go's resolvePath and shouldEscape, which its tests call directly.
 * The mode is one of Go's encoding values, 1 for a path up to 0x40 for a
 * fragment. */
BURROW_OWNS(ret) Str burrow__url_resolve_path(Alloc *a, Str base, Str ref);
bool burrow__url_should_escape(Byte c, int mode);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_URL_H */
