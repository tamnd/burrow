/* golang.org/x/net/http/httpproxy, from the copy Go vendors.
 *
 * Go source: go1.27.1, src/vendor/golang.org/x/net/http/httpproxy/proxy.go.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "httpproxy.h"

#include "idna.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PX_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

BURROW_SENTINEL_ERROR(burrow__httpproxy_err_cgi,
                      "refusing to use HTTP_PROXY value in CGI environment; see "
                      "golang.org/s/cgihttpproxy");

/* ---------------------------------------------------------- the environment */

/* getEnvAny. The first of the names that is set to something. */
static Str px_getenv_any(Alloc *a, const char *upper, const char *lower) {
    Str v = os_getenv(a, str_from_cstr(upper));
    if (v.len != 0)
        return v;
    return os_getenv(a, str_from_cstr(lower));
}

burrow__HttpproxyConfig burrow__httpproxy_from_environment(Alloc *a) {
    burrow__HttpproxyConfig c;
    c.http_proxy = px_getenv_any(a, "HTTP_PROXY", "http_proxy");
    c.https_proxy = px_getenv_any(a, "HTTPS_PROXY", "https_proxy");
    c.no_proxy = px_getenv_any(a, "NO_PROXY", "no_proxy");
    c.cgi = os_getenv(a, PX_LIT("REQUEST_METHOD")).len != 0;
    return c;
}

/* ---------------------------------------------------------------- matchers */

typedef enum { PX_ALL, PX_CIDR, PX_IP, PX_DOMAIN } PxKind;

/* matcher: allMatch, cidrMatch, ipMatch or domainMatch. */
typedef struct PxMatcher {
    NetIPNet *cidr;
    NetIP ip;
    Str host;
    Str port;
    PxKind kind;
    bool match_host;
} PxMatcher;

struct burrow__Httpproxy {
    bool cgi;
    Url *https_proxy;
    Url *http_proxy;
    PxMatcher *ip_matchers;
    Int n_ip;
    PxMatcher *domain_matchers;
    Int n_domain;
};

static bool px_match(const PxMatcher *m, Str host, Str port, NetIP ip) {
    switch (m->kind) {
    case PX_ALL:
        return true;
    case PX_CIDR:
        return net_ip_net_contains(m->cidr, ip);
    case PX_IP:
        if (net_ip_equal(m->ip, ip))
            return m->port.len == 0 || str_eq(m->port, port);
        return false;
    case PX_DOMAIN:
        if (ip.len != 0)
            return false;
        if (strings_has_suffix(host, m->host) ||
            (m->match_host &&
             str_eq(host, str_from_bytes(m->host.p + 1, m->host.len - 1))))
            return m->port.len == 0 || str_eq(m->port, port);
        return false;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ parsing */

static bool px_is_ascii(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 0x80)
            return false;
    return true;
}

/* idnaASCII. v itself when it is ASCII, and false when it does not convert. */
static bool px_idna_ascii(Alloc *a, Str v, Str *out) {
    if (px_is_ascii(v)) {
        *out = v;
        return true;
    }
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Str r = burrow__idna_profile_to_ascii(a, &burrow__idna_lookup, v, &err);
    bool ok = BURROW_OK(err);
    error_release(m);
    if (ok)
        *out = r;
    return ok;
}

/* parseProxy. NULL for no proxy, and for one that does not parse, whose error
 * Go's init drops. */
static Url *px_parse_proxy(Alloc *a, Str proxy) {
    if (proxy.len == 0)
        return NULL;
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, proxy, &err);
    if (BURROW_FAILED(err) || u == NULL || u->scheme.len == 0 || u->host.len == 0) {
        Int n = 7 + proxy.len;
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
        if (p != NULL) {
            memcpy(p, "http://", 7);
            memcpy(p + 7, proxy.p, (size_t)proxy.len);
            Error err2 = BURROW_NO_ERROR;
            Url *u2 = url_parse(a, str_from_bytes(p, n), &err2);
            if (BURROW_OK(err2) && u2 != NULL) {
                error_release(m);
                return u2;
            }
        }
    }
    error_release(m);
    return BURROW_OK(err) ? u : NULL;
}

/* Lower case ASCII, in a, or s when it has none to change. */
static Str px_lower(Alloc *a, Str s) {
    return strings_to_lower(a, s);
}

burrow__Httpproxy *burrow__httpproxy_new(Alloc *a, const burrow__HttpproxyConfig *cfg) {
    burrow__Httpproxy *p =
        (burrow__Httpproxy *)mem_alloc(a, sizeof *p, _Alignof(burrow__Httpproxy));
    if (p == NULL)
        return NULL;
    p->cgi = cfg->cgi;
    p->http_proxy = px_parse_proxy(a, cfg->http_proxy);
    p->https_proxy = px_parse_proxy(a, cfg->https_proxy);

    /* At most one matcher for each comma, and one more. */
    Int max = 1;
    for (Int i = 0; i < cfg->no_proxy.len; i++)
        if (cfg->no_proxy.p[i] == ',')
            max++;
    p->ip_matchers =
        (PxMatcher *)mem_alloc(a, (size_t)max * sizeof(PxMatcher), _Alignof(PxMatcher));
    p->domain_matchers =
        (PxMatcher *)mem_alloc(a, (size_t)max * sizeof(PxMatcher), _Alignof(PxMatcher));
    if (p->ip_matchers == NULL || p->domain_matchers == NULL)
        return NULL;

    Str rest = cfg->no_proxy;
    bool more = true;
    while (more) {
        Str after = BURROW_STR_EMPTY;
        Str item = strings_cut(rest, PX_LIT(","), &after, &more);
        rest = after;
        Str e = px_lower(a, strings_trim_space(item));
        if (e.len == 0)
            continue;
        if (str_eq(e, PX_LIT("*"))) {
            PxMatcher all = {
                NULL, {NULL, 0, 0, NULL}, BURROW_STR_EMPTY, BURROW_STR_EMPTY, PX_ALL,
                false};
            p->ip_matchers[0] = all;
            p->domain_matchers[0] = all;
            p->n_ip = 1;
            p->n_domain = 1;
            return p;
        }
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        NetIPNet *pnet = NULL;
        (void)net_parse_cidr(a, e, &pnet, &err);
        if (BURROW_OK(err) && pnet != NULL) {
            error_release(m);
            PxMatcher *mt = &p->ip_matchers[p->n_ip++];
            memset(mt, 0, sizeof *mt);
            mt->kind = PX_CIDR;
            mt->cidr = pnet;
            continue;
        }
        err = BURROW_NO_ERROR;
        Str pport = BURROW_STR_EMPTY;
        Str phost = net_split_host_port(e, &pport, &err);
        bool split = BURROW_OK(err);
        error_release(m);
        if (split) {
            if (phost.len == 0)
                continue;
            if (phost.p[0] == '[' && phost.p[phost.len - 1] == ']')
                phost = str_from_bytes(phost.p + 1, phost.len - 2);
        } else {
            phost = e;
            pport = BURROW_STR_EMPTY;
        }
        NetIP pip = net_parse_ip(a, phost);
        if (pip.len != 0) {
            PxMatcher *mt = &p->ip_matchers[p->n_ip++];
            memset(mt, 0, sizeof *mt);
            mt->kind = PX_IP;
            mt->ip = pip;
            mt->port = pport;
            continue;
        }
        if (phost.len == 0)
            continue;
        if (strings_has_prefix(phost, PX_LIT("*.")))
            phost = str_from_bytes(phost.p + 1, phost.len - 1);
        bool match_host = false;
        if (phost.p[0] != '.') {
            match_host = true;
            Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)phost.len + 1, 1);
            if (b == NULL)
                return NULL;
            b[0] = '.';
            memcpy(b + 1, phost.p, (size_t)phost.len);
            phost = str_from_bytes(b, phost.len + 1);
        }
        Str v;
        if (px_idna_ascii(a, phost, &v))
            phost = v;
        PxMatcher *mt = &p->domain_matchers[p->n_domain++];
        memset(mt, 0, sizeof *mt);
        mt->kind = PX_DOMAIN;
        mt->host = phost;
        mt->port = pport;
        mt->match_host = match_host;
    }
    return p;
}

/* ------------------------------------------------------------- proxyForURL */

Str burrow__httpproxy_canonical_addr(Alloc *a, const Url *u) {
    Str addr = url_hostname(u);
    Str v;
    if (px_idna_ascii(a, addr, &v))
        addr = v;
    Str port = url_port(u);
    if (port.len == 0) {
        if (str_eq(u->scheme, PX_LIT("http")))
            port = PX_LIT("80");
        else if (str_eq(u->scheme, PX_LIT("https")))
            port = PX_LIT("443");
        else if (str_eq(u->scheme, PX_LIT("socks5")))
            port = PX_LIT("1080");
    }
    return net_join_host_port(a, addr, port);
}

/* useProxy. Whether a request to addr, a host and port, is to go through the
 * proxy. */
static bool px_use_proxy(const burrow__Httpproxy *p, Alloc *a, Str addr) {
    if (addr.len == 0)
        return true;
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Str port = BURROW_STR_EMPTY;
    Str host = net_split_host_port(addr, &port, &err);
    bool split = BURROW_OK(err);
    error_release(m);
    if (!split)
        return false;
    if (str_eq(host, PX_LIT("localhost")))
        return false;
    m = error_mark();
    err = BURROW_NO_ERROR;
    NetipAddr nip = netip_parse_addr(host, &err);
    bool parsed = BURROW_OK(err);
    error_release(m);
    NetIP ip = {NULL, 0, 0, NULL};
    if (parsed) {
        ip = netip_addr_as_slice(nip, a);
        if (ip.len == 0)
            return false; /* the allocator said no */
        if (net_ip_is_loopback(ip))
            return false;
    }
    Str laddr = px_lower(a, strings_trim_space(host));
    if (ip.len != 0)
        for (Int i = 0; i < p->n_ip; i++)
            if (px_match(&p->ip_matchers[i], laddr, port, ip))
                return false;
    for (Int i = 0; i < p->n_domain; i++)
        if (px_match(&p->domain_matchers[i], laddr, port, ip))
            return false;
    return true;
}

const Url *burrow__httpproxy_proxy_for_url(const burrow__Httpproxy *p,
                                           const Url *req_url, Alloc *scratch,
                                           Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    const Url *proxy = NULL;
    if (str_eq(req_url->scheme, PX_LIT("https"))) {
        proxy = p->https_proxy;
    } else if (str_eq(req_url->scheme, PX_LIT("http"))) {
        proxy = p->http_proxy;
        if (proxy != NULL && p->cgi) {
            BURROW_OUT(err, burrow__httpproxy_err_cgi);
            return NULL;
        }
    }
    if (proxy == NULL)
        return NULL;
    if (!px_use_proxy(p, scratch, burrow__httpproxy_canonical_addr(scratch, req_url)))
        return NULL;
    return proxy;
}
