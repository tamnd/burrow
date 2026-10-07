/* net/http/cookiejar, an in-memory cookie jar that keeps to RFC 6265.
 *
 * A CookiejarJar keeps the cookies a client is sent and gives back the ones a
 * request should carry, the way a browser does. It is an HttpCookieJar, so it
 * goes wherever one is wanted.
 *
 *     Error err;
 *     CookiejarJar *jar = cookiejar_new(a, NULL, &err);
 *     cookiejar_jar_set_cookies(jar, u, cookies); // from a response from u
 *     Slice send = cookiejar_jar_cookies(jar, a, u2); // for a request to u2
 *     cookiejar_jar_free(jar);
 *
 * A jar with no public suffix list, as that one is, lets a server for
 * foo.co.uk set a cookie for every other site under co.uk, so a client that
 * talks to sites it does not trust wants one. Go's is in
 * golang.org/x/net/publicsuffix and burrow does not have it yet.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/cookiejar */

#ifndef BURROW_NET_HTTP_COOKIEJAR_H
#define BURROW_NET_HTTP_COOKIEJAR_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/own.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cookiejar.PublicSuffixList, which gives the public suffix of a domain. That
 * of "example.com" is "com", that of "foo1.foo2.foo3.co.uk" is "co.uk" and
 * that of "bar.pvt.k12.ma.us" is "pvt.k12.ma.us". public_suffix may give a
 * part of domain or a string that lives as long as the list does. string
 * describes where the list came from, such as its date or version, in a. A
 * list has to be safe to use from more than one goroutine at once.
 *
 * A list that always says "" is allowed, and can do for a test, but it lets
 * the server for foo.com set a cookie for bar.com. */
typedef struct CookiejarPublicSuffixListVT {
    const Type *self_type;
    Str (*public_suffix)(void *self, Str domain);
    Str (*string)(void *self, Alloc *a);
} CookiejarPublicSuffixListVT;

typedef struct CookiejarPublicSuffixList {
    const CookiejarPublicSuffixListVT *vt;
    void *data;
} CookiejarPublicSuffixList;

/* PublicSuffixList.PublicSuffix. */
BURROW_BORROWS(ret, domain) static inline Str
cookiejar_public_suffix_list_public_suffix(CookiejarPublicSuffixList l, Str domain) {
    return l.vt->public_suffix(l.data, domain);
}

/* PublicSuffixList.String. */
BURROW_OWNS(ret) static inline Str
cookiejar_public_suffix_list_string(CookiejarPublicSuffixList l, Alloc *a) {
    return l.vt->string(l.data, a);
}

/* cookiejar.Options. A list with no vt is no list, which is allowed and can
 * do for a test, but it lets the server for foo.co.uk set a cookie for
 * bar.co.uk. */
typedef struct CookiejarOptions {
    CookiejarPublicSuffixList public_suffix_list;
} CookiejarOptions;

/* cookiejar.Jar. Safe to use from more than one goroutine at once. */
typedef struct CookiejarJar CookiejarJar;

extern const Type *const TYPE_COOKIEJAR_JAR;

/* cookiejar.New. An empty jar, which keeps its cookies in a. NULL options are
 * the zero CookiejarOptions. The only error is a saying no, which Go does not
 * have, and gives NULL. */
BURROW_OWNS(ret) CookiejarJar *cookiejar_new(Alloc *a, const CookiejarOptions *o,
                                             Error *err);

/* Gives the jar and every cookie in it back to the allocator it came from.
 * NULL is fine. */
void cookiejar_jar_free(CookiejarJar *j);

/* Jar.Cookies. The cookies a request to u should carry, as a Slice of
 * HttpCookie from a, with only name, value and quoted set. Those with the
 * longest path come first and, among them, those that were set first. Empty
 * when u is neither http nor https. Asking counts as using the cookies, and a
 * cookie that has expired is thrown away when it is found. */
BURROW_OWNS(ret) Slice cookiejar_jar_cookies(CookiejarJar *j, Alloc *a, const Url *u);

/* Jar.SetCookies. Keeps those of cookies, a Slice of HttpCookie from a
 * response from u, that u was allowed to set, and drops the ones a cookie
 * deletes. Does nothing when u is neither http nor https. The jar copies what
 * it keeps. A cookie the jar's allocator has no room for is dropped. */
void cookiejar_jar_set_cookies(CookiejarJar *j, const Url *u, Slice cookies);

/* The jar as the HttpCookieJar a client takes. Its self_type is
 * TYPE_COOKIEJAR_JAR. */
BURROW_BORROWS(ret, j) HttpCookieJar cookiejar_jar_as_cookie_jar(CookiejarJar *j);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_COOKIEJAR_H */
