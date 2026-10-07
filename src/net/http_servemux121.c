/* Derived from Go's src/net/http/servemux121.go, the ServeMux of Go 1.21 that
 * GODEBUG=httpmuxgo121=1 brings back.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"
#include "http_routing.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* muxEntry. */
typedef struct hm_Entry {
    HttpHandler h;
    Str pattern;
} hm_Entry;

/* serveMux121. m holds every pattern, and es the ones ending in a slash,
 * longest first, which are the ones that match by prefix. */
struct burrow__HttpMux121 {
    Map *m; /* of Str to UnsafePointer, the hm_Entry */
    hm_Entry **es;
    Int nes;
    Int cap;
    bool hosts; /* whether any pattern has a host */
};

static const Str hm_text_no_memory = BURROW_S_INIT("net/http: out of memory");

BURROW_NORETURN static void hm_out_of_memory(void) {
    panic_str(hm_text_no_memory);
}

/* x and y in one string from a. */
static Str hm_cat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        hm_out_of_memory();
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static bool hm_has(const burrow__HttpMux121 *m, Str key) {
    return m != NULL && map_get(m->m, &key) != NULL;
}

/* appendSorted. */
static bool hm_append_sorted(Alloc *a, burrow__HttpMux121 *m, hm_Entry *e) {
    if (m->nes == m->cap) {
        Int cap = m->cap == 0 ? 4 : m->cap * 2;
        hm_Entry **es =
            (hm_Entry **)mem_alloc(a, (size_t)cap * sizeof *es, _Alignof(hm_Entry *));
        if (es == NULL)
            return false;
        if (m->nes > 0)
            memcpy(es, m->es, (size_t)m->nes * sizeof *es);
        m->es = es;
        m->cap = cap;
    }
    Int i = 0;
    while (i < m->nes && m->es[i]->pattern.len >= e->pattern.len)
        i++;
    memmove(m->es + i + 1, m->es + i, (size_t)(m->nes - i) * sizeof *m->es);
    m->es[i] = e;
    m->nes++;
    return true;
}

/* handle, holding the lock. The message for a panic, or "" for none. */
static Str hm_handle_locked(HttpServeMux *mux, Str pattern, HttpHandler h) {
    if (pattern.len == 0)
        return BURROW_S("http: invalid pattern");
    if (h.vt == NULL)
        return BURROW_S("http: nil handler");

    Alloc *a = burrow__http_serve_mux_alloc(mux);
    burrow__HttpMux121 *m = mux->mux121;
    if (m == NULL) {
        m = (burrow__HttpMux121 *)mem_alloc(a, sizeof *m, _Alignof(burrow__HttpMux121));
        if (m == NULL)
            return hm_text_no_memory;
        m->m = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (m->m == NULL)
            return hm_text_no_memory;
        mux->mux121 = m;
    }
    if (hm_has(m, pattern))
        return fmt_sprintf_v(error_allocator(), "http: multiple registrations for %s",
                             pattern);

    hm_Entry *e = (hm_Entry *)mem_alloc(a, sizeof *e, _Alignof(hm_Entry));
    if (e == NULL)
        return hm_text_no_memory;
    e->h = h;
    e->pattern = str_clone(a, pattern);
    if (e->pattern.p == NULL || !map_set(m->m, &e->pattern, &e))
        return hm_text_no_memory;
    if (pattern.p[pattern.len - 1] == '/' && !hm_append_sorted(a, m, e))
        return hm_text_no_memory;
    if (pattern.p[0] != '/')
        m->hosts = true;
    return (Str){0};
}

void burrow__http_mux121_handle(HttpServeMux *mux, Str pattern, HttpHandler h) {
    sync_rw_mutex_lock(&mux->mu);
    Str msg = hm_handle_locked(mux, pattern, h);
    sync_rw_mutex_unlock(&mux->mu);
    if (msg.len > 0)
        panic_str(msg);
}

/* match, holding the lock for reading. An exact match first, then the
 * longest pattern ending in a slash that path starts with. */
static bool hm_match(const burrow__HttpMux121 *m, Str path, HttpHandler *h,
                     Str *pattern) {
    if (m == NULL)
        return false;
    void *const *v = (void *const *)map_get(m->m, &path);
    if (v != NULL) {
        const hm_Entry *e = (const hm_Entry *)*v;
        *h = e->h;
        *pattern = e->pattern;
        return true;
    }
    for (Int i = 0; i < m->nes; i++) {
        if (strings_has_prefix(path, m->es[i]->pattern)) {
            *h = m->es[i]->h;
            *pattern = m->es[i]->pattern;
            return true;
        }
    }
    return false;
}

/* handler. The most specific pattern for host and path, with host only
 * looked at when some pattern has one. */
static HttpHandler hm_handler(HttpServeMux *mux, Alloc *a, Str host, Str path,
                              Str *pattern) {
    Str hostpath = hm_cat(a, host, path);
    HttpHandler h = {0};
    bool found = false;
    sync_rw_mutex_r_lock(&mux->mu);
    const burrow__HttpMux121 *m = mux->mux121;
    if (m != NULL && m->hosts)
        found = hm_match(m, hostpath, &h, pattern);
    if (!found)
        found = hm_match(m, path, &h, pattern);
    sync_rw_mutex_r_unlock(&mux->mu);
    if (!found) {
        *pattern = (Str){0};
        return http_not_found_handler();
    }
    return h;
}

/* shouldRedirectRLocked. Whether path, which is not registered as it is,
 * is registered with a slash on the end. */
static bool hm_should_redirect(HttpServeMux *mux, Alloc *a, Str host, Str path) {
    Str slash = BURROW_S("/");
    Str hostpath = hm_cat(a, host, path);
    Str p1 = hm_cat(a, path, slash);
    Str p2 = hm_cat(a, hostpath, slash);
    bool redirect = false;
    sync_rw_mutex_r_lock(&mux->mu);
    const burrow__HttpMux121 *m = mux->mux121;
    if (!hm_has(m, path) && !hm_has(m, hostpath) && path.len > 0 &&
        (hm_has(m, p1) || hm_has(m, p2)))
        redirect = path.p[path.len - 1] != '/';
    sync_rw_mutex_r_unlock(&mux->mu);
    return redirect;
}

/* A 301 to path, keeping the query, with path as the pattern. */
static HttpHandler hm_redirect(Alloc *a, Str path, const Url *u, Str *pattern) {
    Url to;
    memset(&to, 0, sizeof to);
    to.path = path;
    to.raw_query = u->raw_query;
    Str s = url_string(&to, a);
    if (s.p == NULL)
        hm_out_of_memory();
    HttpHandler h = http_redirect_handler(a, s, HTTP_STATUS_MOVED_PERMANENTLY);
    if (h.data == NULL)
        hm_out_of_memory();
    *pattern = path;
    return h;
}

HttpHandler burrow__http_mux121_find_handler(HttpServeMux *mux, const HttpRequest *r,
                                             Alloc *a, Str *pattern) {
    Url zero;
    memset(&zero, 0, sizeof zero);
    const Url *url = r->url != NULL ? r->url : &zero;
    Str slash = BURROW_S("/");

    /* CONNECT requests are not canonicalized. */
    if (str_eq(r->method, BURROW_S("CONNECT"))) {
        /* A redirect for the slash still applies, but with the host from
         * the URL. */
        if (hm_should_redirect(mux, a, url->host, url->path))
            return hm_redirect(a, hm_cat(a, url->path, slash), url, pattern);
        return hm_handler(mux, a, r->host, url->path, pattern);
    }

    /* All other requests have any port stripped and the path cleaned
     * before looking for a match. */
    Str host = burrow__http_strip_host_port(r->host);
    Str path = burrow__http_clean_path(a, url->path);

    /* A path that is only registered with a slash on the end is sent
     * there. */
    if (hm_should_redirect(mux, a, host, path))
        return hm_redirect(a, hm_cat(a, path, slash), url, pattern);

    if (!str_eq(path, url->path)) {
        Str p;
        (void)hm_handler(mux, a, host, path, &p);
        HttpHandler h = hm_redirect(a, path, url, pattern);
        *pattern = p;
        return h;
    }
    return hm_handler(mux, a, host, url->path, pattern);
}
