/* Derived from Go's src/net/http/csrf.go, CrossOriginProtection.
 * Go source: go1.27.3.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/url.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

HttpCrossOriginProtection *http_new_cross_origin_protection(Alloc *a) {
    HttpCrossOriginProtection *c = (HttpCrossOriginProtection *)mem_alloc(
        a, sizeof(HttpCrossOriginProtection), _Alignof(HttpCrossOriginProtection));
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof *c);
    c->a = a;
    return c;
}

void http_cross_origin_protection_free(HttpCrossOriginProtection *c) {
    if (c == NULL)
        return;
    http_serve_mux_free(&c->bypass_mux);
    arena_free(&c->arena);
    if (c->a != NULL)
        mem_free(c->a, c, sizeof(HttpCrossOriginProtection),
                 _Alignof(HttpCrossOriginProtection));
}

Error http_cross_origin_protection_add_trusted_origin(HttpCrossOriginProtection *c,
                                                      Str origin) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(arena_allocator(&scratch), origin, &err);
    if (BURROW_FAILED(err)) {
        err = fmt_errorf_v("invalid origin %q: %w", origin, err);
    } else if (u->scheme.len == 0) {
        err = fmt_errorf_v("invalid origin %q: scheme is required", origin);
    } else if (u->host.len == 0) {
        err = fmt_errorf_v("invalid origin %q: host is required", origin);
    } else if (u->path.len != 0 || u->raw_query.len != 0 || u->fragment.len != 0) {
        err = fmt_errorf_v(
            "invalid origin %q: path, query, and fragment are not allowed", origin);
    }
    arena_free(&scratch);
    if (BURROW_FAILED(err))
        return err;
    sync_rw_mutex_lock(&c->mu);
    Alloc *a = arena_allocator(&c->arena);
    if (c->trusted == NULL)
        c->trusted = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    bool ok = c->trusted != NULL;
    if (ok && map_get(c->trusted, &origin) == NULL) {
        /* Not in Go: the map keeps the bytes of its keys elsewhere, so the
         * origin is copied into c's arena first. */
        Byte *p = (Byte *)mem_alloc(a, (size_t)origin.len, 1);
        ok = p != NULL || origin.len == 0;
        if (ok) {
            if (origin.len != 0)
                memcpy(p, origin.p, (size_t)origin.len);
            Str key = str_from_bytes(p, origin.len);
            bool yes = true;
            ok = map_set(c->trusted, &key, &yes);
        }
    }
    sync_rw_mutex_unlock(&c->mu);
    return ok ? BURROW_NO_ERROR : burrow_err_out_of_memory;
}

static void coh_noop_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    (void)self;
    (void)w;
    (void)r;
}

static const Type coh_noop_handler_desc = {
    {(const Byte *)"noopHandler", 11},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const HttpHandlerVT coh_noop_handler_vt = {&coh_noop_handler_desc,
                                                  coh_noop_serve};

/* sentinelHandler. Only its vt is compared. */
static const HttpHandler coh_sentinel_handler = {&coh_noop_handler_vt, NULL};

void http_cross_origin_protection_add_insecure_bypass_pattern(
    HttpCrossOriginProtection *c, Str pattern) {
    /* Not in Go: the mux is part of c, so there is nothing to make, and the
     * pointer only says that there is a pattern to look at. */
    HttpServeMux *bypass = &c->bypass_mux;
    sync_atomic_pointer_store(&c->bypass, bypass);

    http_serve_mux_handle(bypass, pattern, coh_sentinel_handler);
}

bool http_cross_origin_protection_set_deny_handler(HttpCrossOriginProtection *c,
                                                   HttpHandler h) {
    if (h.vt == NULL) {
        sync_atomic_pointer_store(&c->deny, NULL);
        return true;
    }
    sync_rw_mutex_lock(&c->mu);
    HttpHandler *p = (HttpHandler *)mem_alloc(
        arena_allocator(&c->arena), sizeof(HttpHandler), _Alignof(HttpHandler));
    sync_rw_mutex_unlock(&c->mu);
    if (p == NULL)
        return false;
    *p = h;
    sync_atomic_pointer_store(&c->deny, p);
    return true;
}

static const Str coh_err_cross_origin_request_text =
    BURROW_S_INIT("cross-origin request detected from Sec-Fetch-Site header");
static const Error coh_err_cross_origin_request = {&burrow_sentinel_error_vt,
                                                   &coh_err_cross_origin_request_text};
static const Str coh_err_cross_origin_request_from_old_browser_text =
    BURROW_S_INIT("cross-origin request detected, and/or browser is out of date: "
                  "Sec-Fetch-Site is missing, and Origin does not match Host");
static const Error coh_err_cross_origin_request_from_old_browser = {
    &burrow_sentinel_error_vt, &coh_err_cross_origin_request_from_old_browser_text};

static bool coh_is_request_exempt(HttpCrossOriginProtection *c, const HttpRequest *req);

Error http_cross_origin_protection_check(HttpCrossOriginProtection *c,
                                         const HttpRequest *req) {
    if (str_eq(req->method, BURROW_S("GET")) || str_eq(req->method, BURROW_S("HEAD")) ||
        str_eq(req->method, BURROW_S("OPTIONS"))) {
        /* Safe methods are always allowed. */
        return BURROW_NO_ERROR;
    }

    Str site = http_header_get(req->header, BURROW_S("Sec-Fetch-Site"));
    if (site.len == 0) {
        /* No Sec-Fetch-Site header is present.
         * Fallthrough to check the Origin header. */
    } else if (str_eq(site, BURROW_S("same-origin")) ||
               str_eq(site, BURROW_S("none"))) {
        return BURROW_NO_ERROR;
    } else {
        if (coh_is_request_exempt(c, req))
            return BURROW_NO_ERROR;
        return coh_err_cross_origin_request;
    }

    Str origin = http_header_get(req->header, BURROW_S("Origin"));
    if (origin.len == 0) {
        /* Neither Sec-Fetch-Site nor Origin headers are present.
         * Either the request is same-origin or not a browser request. */
        return BURROW_NO_ERROR;
    }

    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Url *o = url_parse(arena_allocator(&scratch), origin, &err);
    bool same = !BURROW_FAILED(err) && str_eq(o->host, req->host);
    arena_free(&scratch);
    if (same) {
        /* The Origin header matches the Host header. Note that the Host header
         * doesn't include the scheme, so we don't know if this might be an
         * HTTP→HTTPS cross-origin request. We fail open, since all modern
         * browsers support Sec-Fetch-Site since 2023, and running an older
         * browser makes a clear security trade-off already. Sites can mitigate
         * this with HTTP Strict Transport Security (HSTS). */
        return BURROW_NO_ERROR;
    }

    if (coh_is_request_exempt(c, req))
        return BURROW_NO_ERROR;
    return coh_err_cross_origin_request_from_old_browser;
}

/* isRequestExempt checks the bypasses which require taking a lock, and should
 * be deferred until the last moment. */
static bool coh_is_request_exempt(HttpCrossOriginProtection *c,
                                  const HttpRequest *req) {
    HttpServeMux *bypass = (HttpServeMux *)sync_atomic_pointer_load(&c->bypass);
    if (bypass != NULL) {
        /* Not in Go: the handlers the mux makes for a redirect or a 404 come
         * from a scratch arena, and only the vt of the one it gives is kept. */
        Arena scratch;
        arena_init(&scratch, NULL, 0);
        HttpHandler h =
            http_serve_mux_handler(bypass, arena_allocator(&scratch), req, NULL);
        bool hit = h.vt == coh_sentinel_handler.vt;
        arena_free(&scratch);
        if (hit) {
            /* The request matches a bypass pattern. */
            return true;
        }
    }

    sync_rw_mutex_r_lock(&c->mu);
    Str origin = http_header_get(req->header, BURROW_S("Origin"));
    /* The request matches a trusted origin. */
    bool *trusted = NULL;
    if (origin.len != 0 && c->trusted != NULL)
        trusted = (bool *)map_get(c->trusted, &origin);
    bool exempt = trusted != NULL && *trusted;
    sync_rw_mutex_r_unlock(&c->mu);
    return exempt;
}

/* What Handler's HandlerFunc closes over. */
typedef struct coh_Handler {
    HttpCrossOriginProtection *c;
    HttpHandler h;
} coh_Handler;

static void coh_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const coh_Handler *x = (const coh_Handler *)self;
    Error err = http_cross_origin_protection_check(x->c, r);
    if (BURROW_FAILED(err)) {
        HttpHandler *deny = (HttpHandler *)sync_atomic_pointer_load(&x->c->deny);
        if (deny != NULL) {
            http_handler_serve_http(*deny, w, r);
            return;
        }
        http_error(w, error_text(err), HTTP_STATUS_FORBIDDEN);
        return;
    }
    http_handler_serve_http(x->h, w, r);
}

static const Type coh_handler_desc = {
    {(const Byte *)"HandlerFunc", 11},
    {(const Byte *)"net/http", 8},
    KIND_FUNC,
    (uint32_t)sizeof(coh_Handler),
    (uint16_t)_Alignof(coh_Handler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const HttpHandlerVT coh_handler_vt = {&coh_handler_desc, coh_handler_serve};

/* Not in Go: the handler there is when there was no memory for one. */
static void coh_no_memory_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    (void)self;
    (void)r;
    http_error(w, error_text(burrow_err_out_of_memory),
               HTTP_STATUS_INTERNAL_SERVER_ERROR);
}

static const HttpHandlerVT coh_no_memory_vt = {&coh_handler_desc, coh_no_memory_serve};

HttpHandler http_cross_origin_protection_handler(HttpCrossOriginProtection *c,
                                                 HttpHandler h) {
    sync_rw_mutex_lock(&c->mu);
    coh_Handler *x = (coh_Handler *)mem_alloc(
        arena_allocator(&c->arena), sizeof(coh_Handler), _Alignof(coh_Handler));
    sync_rw_mutex_unlock(&c->mu);
    if (x == NULL)
        return (HttpHandler){&coh_no_memory_vt, NULL};
    x->c = c;
    x->h = h;
    return (HttpHandler){&coh_handler_vt, x};
}
