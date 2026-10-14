/* Derived from Go's src/net/http/server.go, the handlers and ServeMux.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"
#include "http_routing.h"

#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const Type mx_handler_func_desc = {
    {(const Byte *)"HandlerFunc", 11},
    {(const Byte *)"net/http", 8},
    KIND_FUNC,
    (uint32_t)sizeof(HttpHandlerFunc),
    (uint16_t)_Alignof(HttpHandlerFunc),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68686675U, /* "hhfu" */
    NULL,
};

const Type *const TYPE_HTTP_HANDLER_FUNC = &mx_handler_func_desc;

BURROW_SENTINEL_ERROR(
    http_err_body_not_allowed,
    "http: request method or response status code does not allow body");

static const Type mx_redirect_handler_desc = {
    {(const Byte *)"redirectHandler", 15},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(burrow__HttpRedirectHandler),
    (uint16_t)_Alignof(burrow__HttpRedirectHandler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68726468U, /* "hrdh" */
    NULL,
};

static const Str mx_text_invalid_pattern = BURROW_S_INIT("http: invalid pattern");
static const Str mx_text_nil_handler = BURROW_S_INIT("http: nil handler");
static const Str mx_text_no_memory = BURROW_S_INIT("net/http: out of memory");

static Error mx_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

BURROW_NORETURN static void mx_out_of_memory(void) {
    panic_str(mx_text_no_memory);
}

/* x and y in one string from a, false when a says no. */
static bool mx_cat(Alloc *a, Str x, Str y, Str *out) {
    if (y.len == 0) {
        *out = x;
        return true;
    }
    if (x.len == 0) {
        *out = y;
        return true;
    }
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len), 1);
    if (p == NULL)
        return false;
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    *out = str_from_bytes(p, x.len + y.len);
    return true;
}

static Str mx_cat_or_panic(Alloc *a, Str x, Str y) {
    Str s;
    if (!mx_cat(a, x, y, &s))
        mx_out_of_memory();
    return s;
}

static bool mx_has_suffix_slash(Str s) {
    return s.len > 0 && s.p[s.len - 1] == '/';
}

static void mx_arena_free(void *ar) {
    arena_free((Arena *)ar);
}

/* ------------------------------------------------------------------ Handler */

IoWriter http_response_writer_as_io_writer(HttpResponseWriter w) {
    return (IoWriter){&w.vt->writer, w.data};
}

static void mx_func_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const HttpHandlerFunc *f = (const HttpHandlerFunc *)self;
    f->f(f->env, w, r);
}

static const HttpHandlerVT mx_func_vt = {&mx_handler_func_desc, mx_func_serve};

HttpHandler http_handler_func_as_handler(HttpHandlerFunc *f) {
    return (HttpHandler){&mx_func_vt, f};
}

void http_error(HttpResponseWriter w, Str error, Int code) {
    HttpHeader h = http_response_writer_header(w);

    /* Content-Length may be for a body that is not this one. */
    http_header_del(h, BURROW_S("Content-Length"));

    /* Plain text, and no sniffing that would take it for something else. */
    (void)http_header_set(h, BURROW_S("Content-Type"),
                          BURROW_S("text/plain; charset=utf-8"));
    (void)http_header_set(h, BURROW_S("X-Content-Type-Options"), BURROW_S("nosniff"));
    http_response_writer_write_header(w, code);
    (void)fmt_fprintln_v(http_response_writer_as_io_writer(w), error);
}

void http_not_found(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_error(w, BURROW_S("404 page not found"), HTTP_STATUS_NOT_FOUND);
}

static void mx_not_found(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    http_not_found(w, r);
}

static const HttpHandlerFunc mx_not_found_func = {mx_not_found, NULL};

HttpHandler http_not_found_handler(void) {
    /* The func is never written through the handler. */
    return (HttpHandler){&mx_func_vt, (void *)(uintptr_t)&mx_not_found_func};
}

/* StripPrefix's handler, with the prefix's bytes after it. */
typedef struct mx_StripPrefix {
    Str prefix;
    HttpHandler h;
} mx_StripPrefix;

static void mx_strip_prefix_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const mx_StripPrefix *s = (const mx_StripPrefix *)self;
    if (r->url == NULL) {
        http_not_found(w, r);
        return;
    }
    Str p = strings_trim_prefix(r->url->path, s->prefix);
    Str rp = strings_trim_prefix(r->url->raw_path, s->prefix);
    if (p.len < r->url->path.len &&
        (r->url->raw_path.len == 0 || rp.len < r->url->raw_path.len)) {
        HttpRequest r2 = *r;
        Url u2 = *r->url;
        u2.path = p;
        u2.raw_path = rp;
        /* The copies own nothing, so a mux further on uses an arena of its
         * own for r2 rather than a copy of r's. */
        u2.mem = NULL;
        u2.size = 0;
        r2.url = &u2;
        r2.a = NULL;
        arena_init(&r2.arena, NULL, 0);
        r2.wire = NULL;
        http_handler_serve_http(s->h, w, &r2);
    } else {
        http_not_found(w, r);
    }
}

static const Type mx_strip_prefix_desc = {
    {(const Byte *)"HandlerFunc", 11},
    {(const Byte *)"net/http", 8},
    KIND_FUNC,
    (uint32_t)sizeof(mx_StripPrefix),
    (uint16_t)_Alignof(mx_StripPrefix),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68737470U, /* "hstp" */
    NULL,
};

static const HttpHandlerVT mx_strip_prefix_vt = {&mx_strip_prefix_desc,
                                                 mx_strip_prefix_serve};

HttpHandler http_strip_prefix(Alloc *a, Str prefix, HttpHandler h) {
    if (prefix.len == 0)
        return h;
    mx_StripPrefix *s = (mx_StripPrefix *)mem_alloc(a, sizeof *s + (size_t)prefix.len,
                                                    _Alignof(mx_StripPrefix));
    if (s == NULL)
        return (HttpHandler){&mx_strip_prefix_vt, NULL};
    Byte *p = (Byte *)(s + 1);
    memcpy(p, prefix.p, (size_t)prefix.len);
    s->prefix = str_from_bytes(p, prefix.len);
    s->h = h;
    return (HttpHandler){&mx_strip_prefix_vt, s};
}

Str burrow__http_html_escape(Alloc *a, Str s) {
    Str out = {0};
    Int from = 0;
    for (Int i = 0; i < s.len; i++) {
        Str rep;
        switch (s.p[i]) {
        case '&':
            rep = BURROW_S("&amp;");
            break;
        case '<':
            rep = BURROW_S("&lt;");
            break;
        case '>':
            rep = BURROW_S("&gt;");
            break;
        case '"':
            rep = BURROW_S("&#34;");
            break;
        case '\'':
            rep = BURROW_S("&#39;");
            break;
        default:
            continue;
        }
        out = mx_cat_or_panic(a, out, str_from_bytes(s.p + from, i - from));
        out = mx_cat_or_panic(a, out, rep);
        from = i + 1;
    }
    if (from == 0)
        return s;
    return mx_cat_or_panic(a, out, str_from_bytes(s.p + from, s.len - from));
}

typedef struct mx_RedirectEnv {
    HttpResponseWriter w;
    HttpRequest *r;
    Str url;
    Int code;
    Alloc *t;
} mx_RedirectEnv;

static void mx_redirect(mx_RedirectEnv *e) {
    Alloc *t = e->t;
    Str url = e->url;
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(t, url, &err);
    if (u != NULL && u->scheme.len == 0 && u->host.len == 0) {
        /* A path, which is relative to the request's. */
        Url zero;
        memset(&zero, 0, sizeof zero);
        Str oldpath = url_escaped_path(e->r->url != NULL ? e->r->url : &zero, t);
        if (oldpath.len == 0)
            oldpath = BURROW_S("/");

        if (url.len == 0 || url.p[0] != '/') {
            Str file;
            Str olddir = path_split(oldpath, &file);
            url = mx_cat_or_panic(t, olddir, url);
        }

        Str query = {0};
        Int i = strings_index_byte(url, '?');
        if (i != -1) {
            query = str_from_bytes(url.p + i, url.len - i);
            url = str_from_bytes(url.p, i);
        }

        /* path_clean takes the trailing slash off, and it has to stay. */
        bool trailing = mx_has_suffix_slash(url);
        url = path_clean(t, url);
        if (trailing && !mx_has_suffix_slash(url))
            url = mx_cat_or_panic(t, url, BURROW_S("/"));
        url = mx_cat_or_panic(t, url, query);
    }

    HttpHeader h = http_response_writer_header(e->w);

    /* An empty Content-Type, with no values at all, still means no
     * sniffing, so it is the key that counts. */
    bool had_ct = burrow__http_header_has(h, BURROW_S("Content-Type"));

    Str loc =
        str_clone(burrow__map_allocator(h), burrow__http_hex_escape_non_ascii(t, url));
    if (loc.p == NULL && url.len > 0)
        mx_out_of_memory();
    (void)http_header_set(h, BURROW_S("Location"), loc);
    bool get = str_eq(e->r->method, BURROW_S("GET"));
    if (!had_ct && (get || str_eq(e->r->method, BURROW_S("HEAD"))))
        (void)http_header_set(h, BURROW_S("Content-Type"),
                              BURROW_S("text/html; charset=utf-8"));
    http_response_writer_write_header(e->w, e->code);

    /* A short body for GET, which is what browsers did not always do
     * without. */
    if (!had_ct && get) {
        Str body = BURROW_S("<a href=\"");
        body = mx_cat_or_panic(t, body, burrow__http_html_escape(t, url));
        body = mx_cat_or_panic(t, body, BURROW_S("\">"));
        body = mx_cat_or_panic(t, body, http_status_text(e->code));
        body = mx_cat_or_panic(t, body, BURROW_S("</a>.\n"));
        (void)fmt_fprintln_v(http_response_writer_as_io_writer(e->w), body);
    }
}

void http_redirect(HttpResponseWriter w, HttpRequest *r, Str url, Int code) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    mx_RedirectEnv e = {w, r, url, code, arena_allocator(&scratch)};
    BURROW_SCOPE {
        BURROW_DEFER(mx_arena_free, &scratch);
        mx_redirect(&e);
    }
    BURROW_SCOPE_END;
}

static void mx_redirect_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const burrow__HttpRedirectHandler *rh = (const burrow__HttpRedirectHandler *)self;
    http_redirect(w, r, rh->url, rh->code);
}

const HttpHandlerVT burrow__http_redirect_handler_vt = {&mx_redirect_handler_desc,
                                                        mx_redirect_serve};

HttpHandler http_redirect_handler(Alloc *a, Str url, Int code) {
    burrow__HttpRedirectHandler *rh = (burrow__HttpRedirectHandler *)mem_alloc(
        a, sizeof *rh, _Alignof(burrow__HttpRedirectHandler));
    if (rh != NULL) {
        rh->url = url;
        rh->code = code;
    }
    return (HttpHandler){&burrow__http_redirect_handler_vt, rh};
}

static HttpHandler mx_redirect_or_panic(Alloc *a, Str url, Int code) {
    HttpHandler h = http_redirect_handler(a, url, code);
    if (h.data == NULL)
        mx_out_of_memory();
    return h;
}

/* ----------------------------------------------------------------- ServeMux */

static HttpServeMux mx_default_serve_mux;

HttpServeMux *const http_default_serve_mux = &mx_default_serve_mux;

HttpServeMux *http_new_serve_mux(Alloc *a) {
    HttpServeMux *mux =
        (HttpServeMux *)mem_alloc(a, sizeof *mux, _Alignof(HttpServeMux));
    if (mux != NULL)
        mux->a = a;
    return mux;
}

void http_serve_mux_free(HttpServeMux *mux) {
    if (mux == NULL)
        return;
    Alloc *a = mux->a;
    if (mux->ready)
        arena_free(&mux->arena);
    memset(mux, 0, sizeof *mux);
    if (a != NULL)
        mem_free(a, mux, sizeof *mux, _Alignof(HttpServeMux));
}

Alloc *burrow__http_serve_mux_alloc(HttpServeMux *mux) {
    if (!mux->ready) {
        arena_init(&mux->arena, mux->a, 0);
        mux->ready = true;
    }
    return arena_allocator(&mux->arena);
}

/* cleanPath is burrow__http_clean_path, which the patterns use too. */

Str burrow__http_strip_host_port(Str h) {
    /* A host with no port is the usual case, and needs no work. */
    if (strings_index_byte(h, ':') < 0)
        return h;
    Str port;
    Error err = BURROW_NO_ERROR;
    Str host = net_split_host_port(h, &port, &err);
    if (BURROW_FAILED(err))
        return h;
    return host;
}

/* urlFromEscaped, as the string the redirect goes to. */
static Str mx_url_from_escaped(Alloc *a, Str escaped, Str raw_query) {
    Url u;
    memset(&u, 0, sizeof u);
    u.path = burrow__http_path_unescape(a, escaped);
    u.raw_path = escaped;
    u.raw_query = raw_query;
    Str s = url_string(&u, a);
    if (s.p == NULL)
        mx_out_of_memory();
    return s;
}

bool burrow__http_exact_match(const burrow__HttpRoutingNode *n, Str path) {
    if (n == NULL)
        return false;

    /* Not a multi at the end, so the pattern matched the path itself. */
    if (!burrow__http_pattern_last_segment(n->pattern).multi)
        return true;

    /* The multi matched something, unless the path ends in a slash and has
     * as many segments as the pattern. */
    if (path.len > 0 && path.p[path.len - 1] != '/')
        return false;
    return n->pattern->nsegments == strings_count(path, BURROW_S("/"));
}

/* matchOrRedirect. The node for the request, and when the path with a slash
 * on the end matches exactly and the path does not, *redirect set to that
 * path. u is NULL when no redirect is wanted. */
static burrow__HttpRoutingNode *mx_match_or_redirect(HttpServeMux *mux, Alloc *a,
                                                     Str host, Str method, Str path,
                                                     const Url *u, Slice *matches,
                                                     Str *redirect) {
    *redirect = (Str){0};
    Str slashed = {0};
    bool want = u != NULL && path.len > 0 && !mx_has_suffix_slash(path);
    if (want && !mx_cat(a, path, BURROW_S("/"), &slashed))
        mx_out_of_memory();

    burrow__HttpRoutingNode *n = NULL;
    *matches = slice_from(NULL, 0, 0, TYPE_STRING);
    sync_rw_mutex_r_lock(&mux->mu);
    if (mux->tree != NULL) {
        n = burrow__http_routing_match(mux->tree, a, host, method, path, matches);
        if (want && !burrow__http_exact_match(n, path)) {
            Slice m2;
            burrow__HttpRoutingNode *n2 =
                burrow__http_routing_match(mux->tree, a, host, method, slashed, &m2);
            if (burrow__http_exact_match(n2, slashed)) {
                n = n2;
                *matches = slice_from(NULL, 0, 0, TYPE_STRING);
                *redirect = slashed;
            }
        }
    }
    sync_rw_mutex_r_unlock(&mux->mu);
    return n;
}

/* matchingMethods, joined with ", " for an Allow field, and "" for none. */
static Str mx_allowed_methods(HttpServeMux *mux, Alloc *a, Str host, Str path) {
    Map *ms = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    if (ms == NULL)
        mx_out_of_memory();

    /* A path that redirects to the one with a slash is allowed the methods
     * that one has. */
    Str slashed = {0};
    bool also = !mx_has_suffix_slash(path);
    if (also)
        slashed = mx_cat_or_panic(a, path, BURROW_S("/"));

    bool ok = true;
    sync_rw_mutex_r_lock(&mux->mu);
    if (mux->tree != NULL) {
        ok = burrow__http_routing_matching_methods(mux->tree, a, host, path, ms);
        if (ok && also)
            ok = burrow__http_routing_matching_methods(mux->tree, a, host, slashed, ms);
    }
    sync_rw_mutex_r_unlock(&mux->mu);
    if (!ok)
        mx_out_of_memory();
    if (map_len(ms) == 0)
        return (Str){0};

    Slice keys = slice_from(NULL, 0, 0, TYPE_STRING);
    const void *k;
    for (MapIter it = map_iter(ms); map_next(&it, &k, NULL);) {
        keys = slice_append(a, keys, k, 1);
        if (keys.p == NULL)
            mx_out_of_memory();
    }
    slices_sort(keys);
    Str allow = strings_join(a, keys, BURROW_S(", "));
    if (allow.p == NULL)
        mx_out_of_memory();
    return allow;
}

static void mx_not_allowed(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    const Str *allow = (const Str *)env;
    HttpHeader h = http_response_writer_header(w);
    Str v = str_clone(burrow__map_allocator(h), *allow);
    if (v.p == NULL)
        mx_out_of_memory();
    (void)http_header_set(h, BURROW_S("Allow"), v);
    http_error(w, http_status_text(HTTP_STATUS_METHOD_NOT_ALLOWED),
               HTTP_STATUS_METHOD_NOT_ALLOWED);
}

/* The 405 handler, a HandlerFunc as Go's is, that sends allow. */
static HttpHandler mx_not_allowed_handler(Alloc *a, Str allow) {
    typedef struct {
        HttpHandlerFunc f;
        Str allow;
    } NotAllowed;
    NotAllowed *n = (NotAllowed *)mem_alloc(a, sizeof *n, _Alignof(NotAllowed));
    if (n == NULL)
        mx_out_of_memory();
    n->allow = allow;
    n->f = BURROW_FN(HttpHandlerFunc, mx_not_allowed, &n->allow);
    return http_handler_func_as_handler(&n->f);
}

HttpHandler burrow__http_serve_mux_find_handler(HttpServeMux *mux, const HttpRequest *r,
                                                Alloc *a, Str *pattern,
                                                const burrow__HttpPattern **pat,
                                                Slice *matches) {
    *pattern = (Str){0};
    *pat = NULL;
    *matches = slice_from(NULL, 0, 0, TYPE_STRING);

    Url zero;
    memset(&zero, 0, sizeof zero);
    const Url *url = r->url != NULL ? r->url : &zero;

    burrow__HttpRoutingNode *n;
    Slice m;
    Str redirect;
    Str host = url->host;
    Str escaped = url_escaped_path(url, a);
    Str path = escaped;

    if (str_eq(r->method, BURROW_S("CONNECT"))) {
        /* CONNECT has no path to clean, and its host is the one in the
         * URL, which is where the tunnel goes. A redirect is still made
         * for the slash. */
        (void)mx_match_or_redirect(mux, a, host, r->method, path, url, &m, &redirect);
        if (redirect.len > 0) {
            Str target = mx_url_from_escaped(a, redirect, url->raw_query);
            *pattern = burrow__http_path_unescape(a, redirect);
            return mx_redirect_or_panic(a, target, HTTP_STATUS_TEMPORARY_REDIRECT);
        }
        /* Go matches the request's host here, port and all. */
        n = mx_match_or_redirect(mux, a, r->host, r->method, path, NULL, &m, &redirect);
    } else {
        /* All other methods have the port taken off the host and the path
         * cleaned. */
        host = burrow__http_strip_host_port(r->host);
        path = burrow__http_clean_path(a, path);

        /* A path that matches only with a slash on the end, and a path that
         * is not clean, go where they would match. */
        n = mx_match_or_redirect(mux, a, host, r->method, path, url, &m, &redirect);
        if (redirect.len > 0) {
            *pattern = n->pattern->str;
            return mx_redirect_or_panic(
                a, mx_url_from_escaped(a, redirect, url->raw_query),
                HTTP_STATUS_TEMPORARY_REDIRECT);
        }
        if (!str_eq(path, escaped)) {
            if (n != NULL)
                *pattern = n->pattern->str;
            return mx_redirect_or_panic(a, mx_url_from_escaped(a, path, url->raw_query),
                                        HTTP_STATUS_TEMPORARY_REDIRECT);
        }
    }

    if (n == NULL) {
        /* Some pattern may match with another method, which makes this a
         * 405 and not a 404. */
        Str allow = mx_allowed_methods(mux, a, host, path);
        if (allow.len > 0)
            return mx_not_allowed_handler(a, allow);
        return http_not_found_handler();
    }
    *pattern = n->pattern->str;
    *pat = n->pattern;
    *matches = m;
    return *(const HttpHandler *)n->handler;
}

HttpHandler http_serve_mux_handler(HttpServeMux *mux, const HttpRequest *r, Alloc *a,
                                   Str *pattern) {
    Str p;
    HttpHandler h;
    if (burrow__http_godebug_mux121()) {
        h = burrow__http_mux121_find_handler(mux, r, a, &p);
    } else {
        const burrow__HttpPattern *pat;
        Slice m;
        h = burrow__http_serve_mux_find_handler(mux, r, a, &p, &pat, &m);
    }
    if (pattern != NULL)
        *pattern = p;
    return h;
}

static void mx_route(HttpServeMux *mux, HttpResponseWriter w, HttpRequest *r,
                     Alloc *a) {
    HttpHandler h;
    if (burrow__http_godebug_mux121()) {
        Str p;
        h = burrow__http_mux121_find_handler(mux, r, a, &p);
    } else {
        h = burrow__http_serve_mux_find_handler(mux, r, a, &r->pattern, &r->pat,
                                                &r->matches);
    }
    http_handler_serve_http(h, w, r);
}

typedef struct mx_Scratch {
    Arena arena;
    HttpRequest *r;
} mx_Scratch;

/* What the match left in r points into the scratch arena, so it goes when
 * the arena does. */
static void mx_scratch_done(void *env) {
    mx_Scratch *s = (mx_Scratch *)env;
    s->r->pattern = (Str){0};
    s->r->pat = NULL;
    s->r->matches = slice_from(NULL, 0, 0, TYPE_STRING);
    arena_free(&s->arena);
}

void http_serve_mux_serve_http(HttpServeMux *mux, HttpResponseWriter w,
                               HttpRequest *r) {
    if (str_eq(r->request_uri, BURROW_S("*"))) {
        if (http_request_proto_at_least(r, 1, 1))
            (void)http_header_set(http_response_writer_header(w),
                                  BURROW_S("Connection"), BURROW_S("close"));
        http_response_writer_write_header(w, HTTP_STATUS_BAD_REQUEST);
        return;
    }
    if (r->a != NULL) {
        mx_route(mux, w, r, arena_allocator(&r->arena));
        return;
    }
    mx_Scratch s;
    s.r = r;
    arena_init(&s.arena, NULL, 0);
    BURROW_SCOPE {
        BURROW_DEFER(mx_scratch_done, &s);
        mx_route(mux, w, r, arena_allocator(&s.arena));
    }
    BURROW_SCOPE_END;
}

static void mx_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    http_serve_mux_serve_http((HttpServeMux *)self, w, r);
}

static const Type mx_serve_mux_desc = {
    {(const Byte *)"ServeMux", 8},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(HttpServeMux),
    (uint16_t)_Alignof(HttpServeMux),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68736d78U, /* "hsmx" */
    NULL,
};

static const HttpHandlerVT mx_serve_mux_vt = {&mx_serve_mux_desc, mx_serve};

HttpHandler http_serve_mux_as_handler(HttpServeMux *mux) {
    return (HttpHandler){&mx_serve_mux_vt, mux};
}

typedef struct mx_ConflictEnv {
    const burrow__HttpPattern *pat;
} mx_ConflictEnv;

static Error mx_check_conflict(void *env, burrow__HttpPattern *pat2) {
    const burrow__HttpPattern *pat = ((const mx_ConflictEnv *)env)->pat;
    if (!burrow__http_pattern_conflicts_with(pat, pat2))
        return BURROW_NO_ERROR;
    Str d = burrow__http_describe_conflict(error_allocator(), pat, pat2);
    return fmt_errorf_v("pattern %q (registered at %s) conflicts with pattern %q "
                        "(registered at %s):\n%s",
                        pat->str, pat->loc, pat2->str, pat2->loc, d);
}

static bool mx_is_nil_handler(HttpHandler h) {
    if (h.vt == NULL)
        return true;
    if (h.vt->self_type != &mx_handler_func_desc)
        return false;
    const HttpHandlerFunc *f = (const HttpHandlerFunc *)h.data;
    return f == NULL || f->f == NULL;
}

/* registerErr, holding the lock. fn, when it is not NULL, is a HandlerFunc
 * to copy into the mux and register in place of h. */
static Error mx_register_locked(HttpServeMux *mux, Str patstr, HttpHandler h,
                                const HttpHandlerFunc *fn, const char *file, Int line) {
    if (patstr.len == 0)
        return mx_error(&mx_text_invalid_pattern);
    if (mx_is_nil_handler(h))
        return mx_error(&mx_text_nil_handler);

    Alloc *a = burrow__http_serve_mux_alloc(mux);
    Error err = BURROW_NO_ERROR;
    burrow__HttpPattern *pat = burrow__http_parse_pattern(a, patstr, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("parsing %q: %w", patstr, err);
    if (pat == NULL)
        return mx_error(&mx_text_no_memory);
    if (file == NULL)
        pat->loc = BURROW_S("unknown location");
    else
        pat->loc = fmt_sprintf_v(a, "%s:%d", str_from_cstr(file), line);

    mx_ConflictEnv env = {pat};
    if (mux->index != NULL) {
        err = burrow__http_routing_index_possibly_conflicting(mux->index, pat,
                                                              mx_check_conflict, &env);
        if (BURROW_FAILED(err))
            return err;
    }

    HttpHandler *hp = (HttpHandler *)mem_alloc(a, sizeof *hp, _Alignof(HttpHandler));
    if (hp == NULL)
        return mx_error(&mx_text_no_memory);
    *hp = h;
    if (fn != NULL) {
        HttpHandlerFunc *copy =
            (HttpHandlerFunc *)mem_alloc(a, sizeof *copy, _Alignof(HttpHandlerFunc));
        if (copy == NULL)
            return mx_error(&mx_text_no_memory);
        *copy = *fn;
        *hp = http_handler_func_as_handler(copy);
    }
    if (mux->tree == NULL) {
        mux->tree = (burrow__HttpRoutingNode *)mem_alloc(
            a, sizeof *mux->tree, _Alignof(burrow__HttpRoutingNode));
        if (mux->tree == NULL)
            return mx_error(&mx_text_no_memory);
    }
    if (mux->index == NULL) {
        mux->index = (burrow__HttpRoutingIndex *)mem_alloc(
            a, sizeof *mux->index, _Alignof(burrow__HttpRoutingIndex));
        if (mux->index == NULL)
            return mx_error(&mx_text_no_memory);
    }
    if (!burrow__http_routing_add_pattern(a, mux->tree, pat, hp) ||
        !burrow__http_routing_index_add_pattern(a, mux->index, pat))
        return mx_error(&mx_text_no_memory);
    return BURROW_NO_ERROR;
}

static Error mx_register_err(HttpServeMux *mux, Str patstr, HttpHandler h,
                             const HttpHandlerFunc *fn, const char *file, Int line) {
    sync_rw_mutex_lock(&mux->mu);
    Error err = mx_register_locked(mux, patstr, h, fn, file, line);
    sync_rw_mutex_unlock(&mux->mu);
    return err;
}

Error burrow__http_serve_mux_register_err(HttpServeMux *mux, Str pattern, HttpHandler h,
                                          const char *file, Int line) {
    return mx_register_err(mux, pattern, h, NULL, file, line);
}

/* register, which panics with the error. */
static void mx_register(HttpServeMux *mux, Str pattern, HttpHandler h,
                        const HttpHandlerFunc *fn, const char *file, Int line) {
    Error err = mx_register_err(mux, pattern, h, fn, file, line);
    if (BURROW_OK(err))
        return;
    /* In the error arena rather than this frame, which is gone by the time
     * a catch block reads it. */
    Error *box = (Error *)mem_alloc(error_allocator(), sizeof *box, _Alignof(Error));
    if (box == NULL)
        mx_out_of_memory();
    *box = err;
    panic(BURROW_ANY(TYPE_ERROR, box));
}

void burrow__http_serve_mux_handle_at(HttpServeMux *mux, Str pattern, HttpHandler h,
                                      const char *file, Int line) {
    if (burrow__http_godebug_mux121())
        burrow__http_mux121_handle(mux, pattern, h);
    else
        mx_register(mux, pattern, h, NULL, file, line);
}

void burrow__http_serve_mux_handle_func_at(HttpServeMux *mux, Str pattern,
                                           HttpHandlerFunc f, const char *file,
                                           Int line) {
    if (!burrow__http_godebug_mux121()) {
        mx_register(mux, pattern, http_handler_func_as_handler(&f), &f, file, line);
        return;
    }
    if (f.f == NULL)
        panic_str(mx_text_nil_handler);
    sync_rw_mutex_lock(&mux->mu);
    HttpHandlerFunc *copy = (HttpHandlerFunc *)mem_alloc(
        burrow__http_serve_mux_alloc(mux), sizeof *copy, _Alignof(HttpHandlerFunc));
    if (copy != NULL)
        *copy = f;
    sync_rw_mutex_unlock(&mux->mu);
    if (copy == NULL)
        mx_out_of_memory();
    burrow__http_mux121_handle(mux, pattern, http_handler_func_as_handler(copy));
}
