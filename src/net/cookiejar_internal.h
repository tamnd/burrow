/* The parts of net/http/cookiejar that its tests look at and nothing else
 * does.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_COOKIEJAR_INTERNAL_H
#define BURROW_SRC_NET_COOKIEJAR_INTERNAL_H

#include "burrow/net/http/cookiejar.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net/url.h"
#include "burrow/own.h"
#include "burrow/time.h"

#include <stdbool.h>

/* entry, the jar's own copy of a cookie, with the fields of RFC 6265. */
typedef struct burrow__CookiejarEntry {
    Str name;
    Str value;
    Str domain;
    Str path;
    Str same_site;
    Time expires;
    Time creation;
    Time last_access;
    uint64_t seq_num;
    bool quoted;
    bool secure;
    bool http_only;
    bool persistent;
    bool host_only;
} burrow__CookiejarEntry;

/* errIllegalDomain and errMalformedDomain. */
extern const Error burrow__cookiejar_err_illegal_domain;
extern const Error burrow__cookiejar_err_malformed_domain;

/* hasDotSuffix: whether s ends in "." and suffix. */
bool burrow__cookiejar_has_dot_suffix(Str s, Str suffix);

/* canonicalHost: host without its port and its trailing dot, in punycode and
 * lower case, from a or part of host. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, host) Str
burrow__cookiejar_canonical_host(Alloc *a, Str host, Error *err);

/* hasPort: whether host, a name or an IPv4 or IPv6 address, has a port. */
bool burrow__cookiejar_has_port(Str host);

/* jarKey: the key host's cookies are kept under, part of host. */
BURROW_BORROWS(ret, host) Str burrow__cookiejar_jar_key(Str host,
                                                        CookiejarPublicSuffixList psl);

/* isIP: whether host is an IP address. */
bool burrow__cookiejar_is_ip(Str host);

/* defaultPath: the directory part of a URL's path, part of path. */
BURROW_BORROWS(ret, path) Str burrow__cookiejar_default_path(Str path);

/* Jar.domainAndType: the domain of a cookie with the domain attribute domain
 * that came from host, and whether it is a host cookie. Part of host or
 * domain, or from a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, host, domain) Str
burrow__cookiejar_domain_and_type(CookiejarJar *j, Alloc *a, Str host, Str domain,
                                  bool *host_only, Error *err);

/* punycode encode: s in punycode with prefix in front, from a. */
BURROW_OWNS(ret) Str burrow__cookiejar_encode(Alloc *a, Str prefix, Str s, Error *err);

/* toASCII: a domain or one label of one in its ASCII form, so
 * "bücher.example.com" is "xn--bcher-kva.example.com". s itself when it is
 * ASCII already, and from a otherwise. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__cookiejar_to_ascii(Alloc *a, Str s,
                                                                       Error *err);

/* Jar.setCookies and Jar.cookies, with now said rather than read. */
void burrow__cookiejar_set_cookies_at(CookiejarJar *j, const Url *u, Slice cookies,
                                      Time now);
BURROW_OWNS(ret) Slice burrow__cookiejar_cookies_at(CookiejarJar *j, Alloc *a,
                                                    const Url *u, Time now);

/* How many entries the jar has, and in *out a copy of each of them from a.
 * The strings in them belong to the jar. */
BURROW_OWNS(out) Int burrow__cookiejar_entries(CookiejarJar *j, Alloc *a,
                                               burrow__CookiejarEntry **out);

#endif /* BURROW_SRC_NET_COOKIEJAR_INTERNAL_H */
