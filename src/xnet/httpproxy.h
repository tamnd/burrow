/* golang.org/x/net/http/httpproxy, the copy Go vendors.
 *
 * Which proxy a request goes through, from HTTP_PROXY, HTTPS_PROXY and
 * NO_PROXY and their lower case names. net/http's ProxyFromEnvironment is
 * this, and it is an internal of burrow's for the same reason it is one of
 * Go's, which is that only net/http uses it.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xnet/httpproxy */

#ifndef BURROW_SRC_XNET_HTTPPROXY_H
#define BURROW_SRC_XNET_HTTPPROXY_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net/url.h"

#include <stdbool.h>

/* Config. http_proxy and https_proxy are the proxies for "http" and "https"
 * requests, as a URL or a host and port, and no_proxy the comma list of hosts,
 * domains and networks that go without one, with "*" for all of them. cgi
 * says the program runs under CGI, where HTTP_PROXY can come from the client
 * as the Proxy header, so http_proxy is refused. */
typedef struct burrow__HttpproxyConfig {
    Str http_proxy;
    Str https_proxy;
    Str no_proxy;
    bool cgi;
} burrow__HttpproxyConfig;

/* FromEnvironment, with the strings made in a. An empty variable counts as
 * one that is not set, so the lower case name is looked at then. */
burrow__HttpproxyConfig burrow__httpproxy_from_environment(Alloc *a);

/* The error for an "http" request under CGI with HTTP_PROXY set. */
extern const Error burrow__httpproxy_err_cgi;

/* What Config.ProxyFunc parses the config into. */
typedef struct burrow__Httpproxy burrow__Httpproxy;

/* Config.ProxyFunc. Parses cfg into a, which it lives in, and NULL when a says
 * no. A proxy that does not parse is taken as none, as in Go. */
BURROW_OWNS(ret) burrow__Httpproxy *
burrow__httpproxy_new(Alloc *a, const burrow__HttpproxyConfig *cfg);

/* The function ProxyFunc gives. The proxy for a request to req_url, which is
 * p's and lives as long as p does, or NULL for none. scratch is for the
 * working, and the error is static. */
BURROW_BORROWS(ret, p) const Url *
burrow__httpproxy_proxy_for_url(const burrow__Httpproxy *p, const Url *req_url,
                                Alloc *scratch, Error *err);

/* canonicalAddr. The URL's host and port, with the port from its scheme when
 * it has none, and the host in Punycode, made in a. */
BURROW_OWNS(ret) Str burrow__httpproxy_canonical_addr(Alloc *a, const Url *u);

#endif
