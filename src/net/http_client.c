/* net/http's Client: redirects, cookies and a time limit on top of a round
 * tripper.
 *
 * Derived from Go's src/net/http/client.go. Go source: go1.27.1.
 *
 * Go keeps the requests a client makes for redirects, and the responses that
 * led to them, for as long as anything points at them. Here the client keeps
 * them until the response it gives back is freed, and frees them then, which
 * is what the response's on_free is for.
 *
 * What is left out: Request.Cancel, which this port has no field for, and
 * the look at a TLS record header that gives ErrSchemeMismatch, until
 * crypto/tls is here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "../xnet/httpguts.h"
#include "http_ascii.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* How much of a redirect's body is read so that its connection can be used
 * again, and how many redirects the default check_redirect allows. */
#define CL_MAX_BODY_SLURP_SIZE ((int64_t)2 << 10)
#define CL_MAX_REDIRECTS 10

BURROW_SENTINEL_ERROR(http_err_use_last_response, "net/http: use last response");
BURROW_SENTINEL_ERROR(http_err_scheme_mismatch,
                      "http: server gave HTTP response to HTTPS client");
BURROW_SENTINEL_ERROR(cl_err_nil_url, "http: nil Request.URL");
BURROW_SENTINEL_ERROR(cl_err_request_uri,
                      "http: Request.RequestURI can't be set in client requests");
BURROW_SENTINEL_ERROR(cl_err_too_many_redirects, "stopped after 10 redirects");

typedef struct cl_Send cl_Send;

/* One request sent: the copy of it send makes, when it makes one, and what
 * setRequestCancel set up for it, which the response's body is wrapped in when
 * there is a time limit, as cancelTimerBody. */
struct cl_Send {
    cl_Send *next;
    HttpRequest *fork; /* in the client's allocator, or NULL */
    Context ctx;       /* the deadline's, or nil */
    ContextCancelFunc cancel;
    IoReadCloser rc; /* the body inside */
    int64_t deadline;
    SyncAtomicBool stopped;
    bool timed; /* whether didTimeout can say true */
};

/* A call to Client.do, and everything it keeps until the response it gives is
 * freed. */
typedef struct cl_Do {
    HttpClient *c;
    Alloc *a;
    Arena arena;         /* the sends, the copy of the header and the rest */
    HttpRequest **reqs;  /* Go's reqs: the caller's first, then the client's */
    HttpResponse **olds; /* the responses that redirected, which reqs point at */
    Int nreqs, nolds, cap;
    cl_Send *sends;
    HttpHeader ireqhdr; /* makeHeadersCopier's */
    Slice icookies;     /* of HttpCookie */
    bool *idel;         /* which of icookies are gone */
    HttpRequest *owned; /* a request the client made for Get and the rest */
    void *extra;        /* what owned's body reads, freed after it */
    size_t extra_size;
    Func on_free; /* the response's own */
    bool has_icookies;
} cl_Do;

static Alloc *cl_alloc(const HttpClient *c) {
    return c->a != NULL ? c->a : heap_allocator();
}

/* Client.transport. */
static HttpRoundTripper cl_transport(const HttpClient *c) {
    if (c->transport.vt != NULL)
        return c->transport;
    return http_transport_as_round_tripper(http_default_transport);
}

static bool cl_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* Request.closeBody. */
static void cl_close_body(HttpRequest *r) {
    if (r->body.vt != NULL)
        (void)r->body.vt->closer.close(r->body.data);
}

/* Response.closeBody. */
static void cl_close_response_body(HttpResponse *r) {
    if (r->body.vt != NULL)
        (void)r->body.vt->closer.close(r->body.data);
}

/* ------------------------------------------------------------- the strings */

/* s and t one after the other, in a. "" when a says no. */
static Str cl_concat(Alloc *a, Str s, Str t) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(s.len + t.len) + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    if (t.len > 0)
        memcpy(p + s.len, t.p, (size_t)t.len);
    return str_from_bytes(p, s.len + t.len);
}

/* urlErrorOp. */
static Str cl_url_error_op(Alloc *a, Str method) {
    if (method.len == 0)
        return BURROW_S("Get");
    Str rest = strings_to_lower(a, str_from_bytes(method.p + 1, method.len - 1));
    return cl_concat(a, str_from_bytes(method.p, 1), rest);
}

/* stripPassword. */
static Str cl_strip_password(Alloc *a, const Url *u) {
    bool set = false;
    if (u->user != NULL)
        (void)url_userinfo_password(u->user, &set);
    Str s = url_string(u, a);
    if (!set)
        return s;
    Str old = cl_concat(a, url_userinfo_string(u->user, a), BURROW_S("@"));
    Str repl = cl_concat(a, url_userinfo_username(u->user), BURROW_S(":***@"));
    return strings_replace(a, s, old, repl, 1);
}

/* refererForURL. */
static Str cl_referer_for_url(Alloc *a, const Url *last, const Url *next,
                              Str explicit_ref) {
    if (str_eq(last->scheme, BURROW_S("https")) &&
        str_eq(next->scheme, BURROW_S("http")))
        return BURROW_STR_EMPTY;
    if (explicit_ref.len > 0)
        return explicit_ref;
    Str referer = url_string(last, a);
    if (last->user != NULL) {
        Str auth = cl_concat(a, url_userinfo_string(last->user, a), BURROW_S("@"));
        referer = strings_replace(a, referer, auth, BURROW_STR_EMPTY, 1);
    }
    return referer;
}

/* isDomainOrSubdomain. Both are in canonical form already. */
static bool cl_is_domain_or_subdomain(Str sub, Str parent) {
    if (str_eq(sub, parent))
        return true;
    /* An IPv6 address, which is not a host name, and its zone could look like
     * one. "::1%.www.example.com" is not a subdomain of "www.example.com". */
    if (strings_contains_any(sub, BURROW_S(":%")))
        return false;
    if (!strings_has_suffix(sub, parent))
        return false;
    return sub.p[sub.len - parent.len - 1] == '.';
}

/* shouldCopyHeaderOnRedirect. Whether the header the caller set, Cookie and
 * Authorization among it, goes along from initial to dest, which it does for
 * the same domain and its subdomains. */
static bool cl_should_copy_header_on_redirect(Alloc *a, const Url *initial,
                                              const Url *dest) {
    Error e1 = BURROW_NO_ERROR;
    Error e2 = BURROW_NO_ERROR;
    Str ihost = burrow__httpguts_punycode_host_port(a, url_hostname(initial), &e1);
    Str dhost = burrow__httpguts_punycode_host_port(a, url_hostname(dest), &e2);
    if (BURROW_FAILED(e1) || BURROW_FAILED(e2))
        return false;
    bool ok1 = false;
    bool ok2 = false;
    ihost = burrow__http_ascii_to_lower(a, ihost, &ok1);
    dhost = burrow__http_ascii_to_lower(a, dhost, &ok2);
    return ok1 && ok2 && cl_is_domain_or_subdomain(dhost, ihost);
}

/* What %T says of a round tripper, as far as its vtable knows its type. */
static Str cl_type_name(Alloc *a, HttpRoundTripper rt) {
    const Type *t = rt.vt->self_type;
    if (t == NULL)
        return BURROW_S("<nil>");
    Str pkg = t->pkg_path;
    Int slash = strings_last_index_byte(pkg, '/');
    if (slash >= 0)
        pkg = str_from_bytes(pkg.p + slash + 1, pkg.len - slash - 1);
    if (pkg.len == 0)
        return fmt_sprintf_v(a, "*%s", type_name(t));
    return fmt_sprintf_v(a, "*%s.%s", pkg, type_name(t));
}

/* --------------------------------------------------------------- the sends */

/* The stop function setRequestCancel gives, which runs once. */
static void cl_send_stop(cl_Send *s) {
    if (!sync_atomic_bool_compare_and_swap(&s->stopped, false, true))
        return;
    if (!BURROW_FUNC_IS_NIL(s->cancel))
        BURROW_CALLF0(s->cancel);
}

/* setRequestCancel's didTimeout. */
static bool cl_send_did_timeout(const cl_Send *s) {
    return s->timed && burrow_nanotime() > s->deadline;
}

static bool cl_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

/* cancelTimerBody.Read. */
static Int cl_timer_body_read(void *self, Slice p, Error *err) {
    cl_Send *s = (cl_Send *)self;
    Error e = BURROW_NO_ERROR;
    Int n = s->rc.vt->reader.read(s->rc.data, p, &e);
    if (BURROW_FAILED(e) && !cl_is_eof(e) && cl_send_did_timeout(s)) {
        Str text = fmt_sprintf_v(
            error_allocator(),
            "%s (Client.Timeout or context cancellation while reading body)",
            error_text(e));
        e = text.len == 0 ? burrow_err_out_of_memory
                          : burrow__http_timeout_error(error_allocator(), text);
    }
    BURROW_OUT(err, e);
    return n;
}

/* cancelTimerBody.Close. */
static Error cl_timer_body_close(void *self) {
    cl_Send *s = (cl_Send *)self;
    Error e = s->rc.vt->closer.close(s->rc.data);
    cl_send_stop(s);
    return e;
}

static const IoReadCloserVT cl_timer_body_vt = {{NULL, cl_timer_body_read},
                                                {NULL, cl_timer_body_close}};

/* knownRoundTripperImpl. Whether rt is one that cancels a request when its
 * context is done, which here is HttpTransport alone. */
static bool cl_known_round_tripper(HttpRoundTripper rt, const HttpRequest *req) {
    HttpTransport *t = burrow__http_as_transport(rt);
    if (t == NULL)
        return false;
    HttpRoundTripper alt;
    if (burrow__http_transport_alternate(t, req, &alt))
        return cl_known_round_tripper(alt, req);
    return true;
}

/* send. Sends req, or a copy of it with what is missing filled in and the
 * deadline in its context, and gives the response, or NULL and the error with
 * *timed_out saying whether the deadline is why. The body of req is closed on
 * an error. */
static HttpResponse *cl_send_request(cl_Do *d, HttpRequest *req, int64_t deadline,
                                     bool *timed_out, Error *err) {
    Alloc *da = arena_allocator(&d->arena);
    HttpRoundTripper rt = cl_transport(d->c);
    Error e = BURROW_NO_ERROR;
    *timed_out = false;

    if (req->url == NULL) {
        cl_close_body(req);
        BURROW_OUT(err, cl_err_nil_url);
        return NULL;
    }
    if (req->request_uri.len > 0) {
        cl_close_body(req);
        BURROW_OUT(err, cl_err_request_uri);
        return NULL;
    }

    /* The copy, the first time something has to change. The transport is
     * promised a header that is not nil. */
    bool wants_auth =
        req->url->user != NULL &&
        (req->header == NULL ||
         http_header_get(req->header, BURROW_S("Authorization")).len == 0);
    HttpRequest *r = req;
    cl_Send *s = NULL;
    if (req->header == NULL || wants_auth || deadline != 0) {
        s = (cl_Send *)mem_alloc(da, sizeof *s, _Alignof(cl_Send));
        if (s == NULL)
            goto oom;
        s->next = d->sends;
        d->sends = s;
        s->fork = http_request_with_context(req, d->a, http_request_context(req));
        if (s->fork == NULL)
            goto oom;
        r = s->fork;
        Alloc *fa = arena_allocator(&r->arena);
        if (r->header == NULL) {
            r->header = http_header_make(fa);
            if (r->header == NULL)
                goto oom;
        }
        if (wants_auth) {
            HttpHeader h = req->header != NULL ? http_header_clone(fa, req->header)
                                               : http_header_make(fa);
            Str v = burrow__http_proxy_auth(fa, req->url);
            if (h == NULL || v.len == 0 ||
                !http_header_set(h, BURROW_S("Authorization"), v))
                goto oom;
            r->header = h;
        }
    }

    /* setRequestCancel. A context with the deadline, unless the request's own
     * one ends sooner. */
    if (deadline != 0) {
        Context old = http_request_context(r);
        int64_t when = 0;
        bool before = !context_deadline(old, &when) || deadline < when;
        s->deadline = deadline;
        if (before) {
            s->ctx = context_with_deadline(d->a, old, deadline, &s->cancel);
            if (BURROW_CONTEXT_IS_NIL(s->ctx))
                goto oom;
            r->ctx = s->ctx;
            s->timed = true;
        } else {
            /* Go's timer still runs for a round tripper it does not know,
             * and says it timed out once it fires. */
            s->timed = !cl_known_round_tripper(rt, r);
        }
    }

    HttpResponse *resp = http_round_tripper_round_trip(rt, r, &e);
    if (BURROW_FAILED(e)) {
        if (s != NULL)
            cl_send_stop(s);
        if (resp != NULL) {
            log_printf_v("RoundTripper returned a response & error; ignoring response");
            http_response_free(resp);
        }
        *timed_out = s != NULL && cl_send_did_timeout(s);
        BURROW_OUT(err, e);
        return NULL;
    }
    if (resp == NULL) {
        if (s != NULL)
            cl_send_stop(s);
        *timed_out = s != NULL && cl_send_did_timeout(s);
        BURROW_OUT(err,
                   fmt_errorf_v("http: RoundTripper implementation (%s) returned a "
                                "nil *Response with a nil error",
                                cl_type_name(error_allocator(), rt)));
        return NULL;
    }
    if (resp->body.vt == NULL) {
        /* A round tripper that is not this package's may mean an empty body
         * by a nil one, so long as the length lets it be empty. */
        if (resp->content_length > 0 && !str_eq(r->method, BURROW_S("HEAD"))) {
            int64_t n = resp->content_length;
            http_response_free(resp);
            if (s != NULL)
                cl_send_stop(s);
            *timed_out = s != NULL && cl_send_did_timeout(s);
            BURROW_OUT(err,
                       fmt_errorf_v("http: RoundTripper implementation (%s) returned "
                                    "a *Response with content length %d but a nil "
                                    "Body",
                                    cl_type_name(error_allocator(), rt), n));
            return NULL;
        }
        resp->body = http_no_body;
    }
    if (deadline != 0) {
        s->rc = resp->body;
        resp->body = (IoReadCloser){&cl_timer_body_vt, s};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return resp;

oom:
    cl_close_body(req);
    BURROW_OUT(err, burrow_err_out_of_memory);
    return NULL;
}

/* Client.send. send, with the jar's cookies added to req first and the
 * response's given to the jar after. */
static HttpResponse *cl_send(cl_Do *d, HttpRequest *req, int64_t deadline,
                             bool *timed_out, Error *err) {
    HttpClient *c = d->c;
    Alloc *da = arena_allocator(&d->arena);
    *timed_out = false;
    const Url *cookie_url = req->url;
    if (c->jar.vt != NULL && req->url != NULL) {
        if (req->host.len > 0) {
            Url *u = url_clone(req->url, da);
            if (u == NULL) {
                cl_close_body(req);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return NULL;
            }
            u->host = req->host;
            cookie_url = u;
        }
        /* The cookies go in the request's own header, as in Go, made with
         * the header's allocator so that they last as long as it does. Go
         * would stop on a nil header here, and this makes one. */
        Slice cookies = http_cookie_jar_cookies(c->jar, da, cookie_url);
        if (cookies.len > 0 && req->header == NULL) {
            req->header = http_header_make(arena_allocator(&req->arena));
            if (req->header == NULL) {
                cl_close_body(req);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return NULL;
            }
        }
        for (Int i = 0; i < cookies.len; i++) {
            const HttpCookie *ck = &((const HttpCookie *)cookies.p)[i];
            if (!http_request_add_cookie(req, burrow__map_allocator(req->header), ck)) {
                cl_close_body(req);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return NULL;
            }
        }
    }
    HttpResponse *resp = cl_send_request(d, req, deadline, timed_out, err);
    if (resp == NULL)
        return NULL;
    if (c->jar.vt != NULL) {
        Slice rc = http_response_cookies(resp, da);
        if (rc.len > 0)
            http_cookie_jar_set_cookies(c->jar, cookie_url, rc);
    }
    return resp;
}

/* ------------------------------------------------------------ Client.do */

/* Frees what d kept, the responses first, since the transport's on_free looks
 * at their requests, and then the contexts those were made from. */
static void cl_do_free(cl_Do *d) {
    for (Int i = 0; i < d->nolds; i++)
        http_response_free(d->olds[i]);
    for (cl_Send *s = d->sends; s != NULL; s = s->next) {
        cl_send_stop(s);
        context_release(s->ctx);
        http_request_free(s->fork);
    }
    for (Int i = 1; i < d->nreqs; i++)
        http_request_free(d->reqs[i]);
    http_request_free(d->owned);
    if (d->extra != NULL)
        mem_free(d->a, d->extra, d->extra_size, _Alignof(StringsReader));
    Alloc *a = d->a;
    arena_free(&d->arena);
    mem_free(a, d, sizeof *d, _Alignof(cl_Do));
}

/* The response's on_free, once it has a request or a context of d's. */
static void cl_do_on_free(void *env) {
    cl_Do *d = (cl_Do *)env;
    if (!BURROW_FUNC_IS_NIL(d->on_free))
        BURROW_CALLF0(d->on_free);
    cl_do_free(d);
}

/* Hands resp to the caller, with d to be freed when it is. */
static HttpResponse *cl_do_finish(cl_Do *d, HttpResponse *resp) {
    if (d->nreqs <= 1 && d->nolds == 0 && d->sends == NULL && d->owned == NULL) {
        cl_do_free(d);
        return resp;
    }
    d->on_free = resp->on_free;
    resp->on_free = BURROW_FN(Func, cl_do_on_free, d);
    return resp;
}

/* Gives up: frees resp, which may be NULL, and d. */
static HttpResponse *cl_do_fail(cl_Do *d, HttpResponse *resp, Error e, Error *err) {
    http_response_free(resp);
    cl_do_free(d);
    BURROW_OUT(err, e);
    return NULL;
}

/* Room for one more in reqs and olds. False when the arena says no. */
static bool cl_do_grow(cl_Do *d) {
    if (d->nreqs < d->cap)
        return true;
    Alloc *da = arena_allocator(&d->arena);
    Int ncap = d->cap == 0 ? 4 : d->cap * 2;
    HttpRequest **reqs = (HttpRequest **)mem_alloc(da, (size_t)ncap * sizeof *reqs,
                                                   _Alignof(HttpRequest *));
    HttpResponse **olds = (HttpResponse **)mem_alloc(da, (size_t)ncap * sizeof *olds,
                                                     _Alignof(HttpResponse *));
    if (reqs == NULL || olds == NULL)
        return false;
    if (d->nreqs > 0)
        memcpy(reqs, d->reqs, (size_t)d->nreqs * sizeof *reqs);
    if (d->nolds > 0)
        memcpy(olds, d->olds, (size_t)d->nolds * sizeof *olds);
    d->reqs = reqs;
    d->olds = olds;
    d->cap = ncap;
    return true;
}

/* The uerr closure: e as a UrlError for the request that failed, in the
 * caller's error_allocator. url, when it is not "", says which URL. */
static Error cl_uerr(cl_Do *d, HttpRequest *req, const HttpResponse *resp,
                     bool body_closed, Error e, Str url) {
    if (!body_closed)
        cl_close_body(req);
    if (cl_same(e, burrow_err_out_of_memory))
        return e;
    Alloc *ea = error_allocator();
    UrlError ue;
    ue.op = cl_url_error_op(ea, d->reqs[0]->method);
    if (url.len > 0)
        ue.url = url;
    else if (resp != NULL && resp->request != NULL && resp->request->url != NULL)
        ue.url = cl_strip_password(ea, resp->request->url);
    else
        ue.url = cl_strip_password(ea, req->url);
    ue.err = error_retain(ea, e);
    return url_error_as_error(&ue, ea);
}

/* redirectBehavior. */
static void cl_redirect_behavior(Str req_method, const HttpResponse *resp,
                                 const HttpRequest *ireq, Str *redirect_method,
                                 bool *should_redirect, bool *include_body) {
    *should_redirect = false;
    *include_body = false;
    switch (resp->status_code) {
    case 301:
    case 302:
    case 303:
        *redirect_method = req_method;
        *should_redirect = true;
        /* RFC 7231 lets other methods be redirected too, but only GET and
         * HEAD ever were, issue 18570. */
        if (!str_eq(req_method, BURROW_S("GET")) &&
            !str_eq(req_method, BURROW_S("HEAD")))
            *redirect_method = BURROW_S("GET");
        break;
    case 307:
    case 308:
        *redirect_method = req_method;
        *should_redirect = true;
        *include_body = true;
        /* The body has to go again and there is no way to make it, so the
         * caller gets this response rather than an error, as before Go 1.8. */
        if (ireq->get_body.f == NULL && burrow__http_request_outgoing_length(ireq) != 0)
            *should_redirect = false;
        break;
    default:
        break;
    }
}

/* defaultCheckRedirect. */
static Error cl_default_check_redirect(Slice via) {
    if (via.len >= CL_MAX_REDIRECTS)
        return cl_err_too_many_redirects;
    return BURROW_NO_ERROR;
}

/* "name=value" of each cookie left in icookies, sorted and joined with "; ".
 * NULL p when a says no. */
static Str cl_join_icookies(cl_Do *d, Alloc *a, bool *ok) {
    const HttpCookie *cs = (const HttpCookie *)d->icookies.p;
    Int n = 0;
    Str *ss =
        (Str *)mem_alloc(a, (size_t)d->icookies.len * sizeof *ss + 1, _Alignof(Str));
    *ok = ss != NULL;
    if (ss == NULL)
        return BURROW_STR_EMPTY;
    for (Int i = 0; i < d->icookies.len; i++) {
        if (d->idel[i])
            continue;
        Str s = fmt_sprintf_v(a, "%s=%s", cs[i].name, cs[i].value);
        if (s.len == 0) {
            *ok = false;
            return BURROW_STR_EMPTY;
        }
        /* Sorted as they go in, so the header comes out the same each time. */
        Int j = n++;
        while (j > 0 && str_cmp(ss[j - 1], s) > 0) {
            ss[j] = ss[j - 1];
            j--;
        }
        ss[j] = s;
    }
    Int total = 0;
    for (Int i = 0; i < n; i++)
        total += (i > 0 ? 2 : 0) + ss[i].len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)total + 1, 1);
    if (p == NULL) {
        *ok = false;
        return BURROW_STR_EMPTY;
    }
    Int at = 0;
    for (Int i = 0; i < n; i++) {
        if (i > 0) {
            p[at++] = ';';
            p[at++] = ' ';
        }
        memcpy(p + at, ss[i].p, (size_t)ss[i].len);
        at += ss[i].len;
    }
    return str_from_bytes(p, total);
}

/* The closure makeHeadersCopier makes: the first request's header, at least
 * the parts that are safe, into req. False when an allocator says no. */
static bool cl_copy_headers(cl_Do *d, HttpRequest *req, bool strip_sensitive,
                            bool strip_body) {
    Alloc *da = arena_allocator(&d->arena);

    /* Cookies set on the first request by hand, which a redirect's response
     * may have changed. They have no domain or path to go by, so any cookie
     * the response sets of the same name replaces them, issue 17494. */
    if (d->c->jar.vt != NULL && d->has_icookies) {
        bool changed = false;
        Slice rc = http_response_cookies(req->response, da);
        const HttpCookie *cs = (const HttpCookie *)d->icookies.p;
        for (Int i = 0; i < rc.len; i++) {
            Str name = ((const HttpCookie *)rc.p)[i].name;
            for (Int j = 0; j < d->icookies.len; j++) {
                if (!d->idel[j] && str_eq(cs[j].name, name)) {
                    d->idel[j] = true;
                    changed = true;
                }
            }
        }
        if (changed) {
            http_header_del(d->ireqhdr, BURROW_S("Cookie"));
            bool ok = false;
            Str v = cl_join_icookies(d, da, &ok);
            if (!ok || !http_header_set(d->ireqhdr, BURROW_S("Cookie"), v))
                return false;
        }
    }

    const void *k = NULL;
    void *v = NULL;
    for (MapIter it = map_iter(d->ireqhdr); map_next(&it, &k, &v);) {
        Str key = *(const Str *)k;
        Str ck = http_canonical_header_key(da, key);
        bool sensitive = str_eq(ck, BURROW_S("Authorization")) ||
                         str_eq(ck, BURROW_S("Www-Authenticate")) ||
                         str_eq(ck, BURROW_S("Cookie")) ||
                         str_eq(ck, BURROW_S("Cookie2")) ||
                         str_eq(ck, BURROW_S("Proxy-Authorization")) ||
                         str_eq(ck, BURROW_S("Proxy-Authenticate"));
        /* The ones about the body, which a POST redirected to a GET loses. */
        bool body = str_eq(ck, BURROW_S("Content-Encoding")) ||
                    str_eq(ck, BURROW_S("Content-Language")) ||
                    str_eq(ck, BURROW_S("Content-Location")) ||
                    str_eq(ck, BURROW_S("Content-Type"));
        if (!(sensitive && strip_sensitive) && !(body && strip_body)) {
            if (!map_set(req->header, &key, v))
                return false;
        }
    }
    return true;
}

/* The request for the next hop, to loc from req, as Client.do makes it.
 * NULL and the error otherwise, with resp's body closed. */
static HttpRequest *cl_redirect_request(cl_Do *d, HttpRequest *req, HttpResponse *resp,
                                        Str loc, Str method, bool include_body,
                                        bool *strip_sensitive, Error *err) {
    HttpRequest *ireq = d->reqs[0];
    Error e = BURROW_NO_ERROR;
    HttpRequest *nr = (HttpRequest *)mem_alloc(d->a, sizeof *nr, _Alignof(HttpRequest));
    if (nr == NULL) {
        cl_close_response_body(resp);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    nr->a = d->a;
    arena_init(&nr->arena, d->a, 0);
    Alloc *na = arena_allocator(&nr->arena);

    Url *u = url_parse_ref(req->url, na, loc, &e);
    if (u == NULL) {
        cl_close_response_body(resp);
        if (BURROW_OK(e))
            e = burrow_err_out_of_memory;
        else
            e = fmt_errorf_v("failed to parse Location header %q: %v", loc, e);
        http_request_free(nr);
        BURROW_OUT(err, e);
        return NULL;
    }

    /* A Host the caller set stays for a relative Location, issue 22233. */
    Str host = BURROW_STR_EMPTY;
    if (req->host.len > 0 && !str_eq(req->host, req->url->host)) {
        Error ignored = BURROW_NO_ERROR;
        Url *lu = url_parse(na, loc, &ignored);
        if (lu != NULL && !url_is_abs(lu))
            host = req->host;
    }
    nr->method = method;
    nr->response = resp;
    nr->url = u;
    nr->header = http_header_make(na);
    nr->host = host;
    nr->ctx = ireq->ctx;
    if (nr->header == NULL) {
        cl_close_response_body(resp);
        http_request_free(nr);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (include_body && ireq->get_body.f != NULL) {
        nr->body = BURROW_CALLF(ireq->get_body, &e);
        if (BURROW_FAILED(e)) {
            cl_close_response_body(resp);
            http_request_free(nr);
            BURROW_OUT(err, e);
            return NULL;
        }
        nr->get_body = ireq->get_body;
        nr->content_length = ireq->content_length;
    }

    /* The first request's header before Referer, in case it set its own.
     * check_redirect can change it after. */
    if (!*strip_sensitive && !str_eq(ireq->url->host, nr->url->host)) {
        if (!cl_should_copy_header_on_redirect(na, ireq->url, nr->url))
            *strip_sensitive = true;
    }
    bool ok = cl_copy_headers(d, nr, *strip_sensitive, !include_body);
    if (ok) {
        Str ref = cl_referer_for_url(na, d->reqs[d->nreqs - 1]->url, nr->url,
                                     http_header_get(nr->header, BURROW_S("Referer")));
        if (ref.len > 0)
            ok = http_header_set(nr->header, BURROW_S("Referer"), ref);
    }
    if (!ok) {
        cl_close_response_body(resp);
        cl_close_body(nr);
        http_request_free(nr);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return nr;
}

/* Client.do. owned, when it is not NULL, is req, made by the client, and is
 * freed with the response, after extra. */
static HttpResponse *cl_do(HttpClient *c, HttpRequest *req, HttpRequest *owned,
                           void *extra, size_t extra_size, Error *err) {
    Alloc *a = cl_alloc(c);
    if (req->url == NULL) {
        cl_close_body(req);
        UrlError ue;
        ue.op = cl_url_error_op(error_allocator(), req->method);
        ue.url = BURROW_STR_EMPTY;
        ue.err = cl_err_nil_url;
        Error e = url_error_as_error(&ue, error_allocator());
        http_request_free(owned);
        if (extra != NULL)
            mem_free(a, extra, extra_size, _Alignof(StringsReader));
        BURROW_OUT(err, e);
        return NULL;
    }
    cl_Do *d = (cl_Do *)mem_alloc(a, sizeof *d, _Alignof(cl_Do));
    if (d == NULL) {
        cl_close_body(req);
        http_request_free(owned);
        if (extra != NULL)
            mem_free(a, extra, extra_size, _Alignof(StringsReader));
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    d->c = c;
    d->a = a;
    arena_init(&d->arena, a, 0);
    d->owned = owned;
    d->extra = extra;
    d->extra_size = extra_size;
    Alloc *da = arena_allocator(&d->arena);

    int64_t deadline = 0;
    if (c->timeout > 0) {
        int64_t now = burrow_nanotime();
        deadline = c->timeout > INT64_MAX - now ? INT64_MAX : now + c->timeout;
    }

    /* makeHeadersCopier, with the header as it is before the jar adds to
     * it. */
    d->ireqhdr =
        req->header != NULL ? http_header_clone(da, req->header) : http_header_make(da);
    if (d->ireqhdr == NULL) {
        cl_close_body(req);
        return cl_do_fail(d, NULL, burrow_err_out_of_memory, err);
    }
    if (c->jar.vt != NULL && req->header != NULL &&
        http_header_get(req->header, BURROW_S("Cookie")).len > 0) {
        d->has_icookies = true;
        d->icookies = http_request_cookies(req, da);
        d->idel = (bool *)mem_alloc(da, (size_t)d->icookies.len + 1, _Alignof(bool));
        if (d->idel == NULL) {
            cl_close_body(req);
            return cl_do_fail(d, NULL, burrow_err_out_of_memory, err);
        }
    }

    HttpResponse *resp = NULL;
    Str redirect_method = BURROW_STR_EMPTY;
    bool include_body = true;
    bool strip_sensitive = false;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        if (!cl_do_grow(d)) {
            cl_close_body(req);
            return cl_do_fail(d, resp, burrow_err_out_of_memory, err);
        }
        /* Every request after the first is the next hop of a redirect, and
         * there is a response to it from the last time round. */
        if (resp != NULL) {
            Str loc = http_header_get(resp->header, BURROW_S("Location"));
            if (loc.len == 0) {
                /* A 3xx with no Location happens, issues 17773 and 49281. */
                BURROW_OUT(err, BURROW_NO_ERROR);
                return cl_do_finish(d, resp);
            }
            HttpRequest *nr = cl_redirect_request(d, req, resp, loc, redirect_method,
                                                  include_body, &strip_sensitive, &e);
            /* req's body was closed at the end of the last time round. */
            if (nr == NULL)
                return cl_do_fail(
                    d, resp, cl_uerr(d, req, resp, true, e, BURROW_STR_EMPTY), err);

            Slice via = slice_from(d->reqs, d->nreqs, d->nreqs, TYPE_UNSAFE_POINTER);
            Error ce = BURROW_FUNC_IS_NIL(c->check_redirect)
                           ? cl_default_check_redirect(via)
                           : BURROW_CALLF(c->check_redirect, nr, via);

            /* The previous response, with its body not closed, issue 10069. */
            if (cl_same(ce, http_err_use_last_response)) {
                cl_close_body(nr);
                http_request_free(nr);
                BURROW_OUT(err, BURROW_NO_ERROR);
                return cl_do_finish(d, resp);
            }

            /* Read a little of the body, so that a small one leaves its
             * connection to be used again. */
            if (resp->content_length == -1 ||
                resp->content_length <= CL_MAX_BODY_SLURP_SIZE) {
                Error ignored = BURROW_NO_ERROR;
                IoReader body = {&resp->body.vt->reader, resp->body.data};
                (void)io_copy_n(heap_allocator(), io_discard, body,
                                CL_MAX_BODY_SLURP_SIZE, &ignored);
            }
            cl_close_response_body(resp);

            if (BURROW_FAILED(ce)) {
                /* Both the response, its body closed, and the error, as Go 1
                 * did, issue 3795. */
                e = cl_uerr(d, nr, resp, false, ce, loc);
                http_request_free(nr);
                HttpResponse *r = cl_do_finish(d, resp);
                BURROW_OUT(err, e);
                return r;
            }
            d->olds[d->nolds++] = resp;
            resp = NULL;
            req = nr;
        }

        d->reqs[d->nreqs++] = req;
        bool timed_out = false;
        resp = cl_send(d, req, deadline, &timed_out, &e);
        if (resp == NULL) {
            /* send closed the body. */
            if (deadline != 0 && timed_out && !cl_same(e, burrow_err_out_of_memory)) {
                Str text =
                    fmt_sprintf_v(error_allocator(),
                                  "%s (Client.Timeout exceeded while awaiting headers)",
                                  error_text(e));
                e = text.len == 0 ? burrow_err_out_of_memory
                                  : burrow__http_timeout_error(error_allocator(), text);
            }
            return cl_do_fail(d, NULL, cl_uerr(d, req, NULL, true, e, BURROW_STR_EMPTY),
                              err);
        }

        bool should_redirect = false;
        bool include_body_on_hop = false;
        cl_redirect_behavior(req->method, resp, d->reqs[0], &redirect_method,
                             &should_redirect, &include_body_on_hop);
        if (!should_redirect) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return cl_do_finish(d, resp);
        }
        /* A hop that drops the body drops it for good. */
        if (!include_body_on_hop)
            include_body = false;
        cl_close_body(req);
    }
}

/* ------------------------------------------------------------------- Client */

HttpResponse *http_client_do(HttpClient *c, HttpRequest *req, Error *err) {
    return cl_do(c, req, NULL, NULL, 0, err);
}

/* NewRequest and Do, with the request freed with the response. */
static HttpResponse *cl_new_and_do(HttpClient *c, Str method, Str url,
                                   const Str *content_type, IoReader body, void *extra,
                                   size_t extra_size, Error *err) {
    Alloc *a = cl_alloc(c);
    Error e = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(a, method, url, body, &e);
    if (req != NULL && content_type != NULL &&
        !http_header_set(req->header, BURROW_S("Content-Type"), *content_type)) {
        http_request_free(req);
        req = NULL;
        e = burrow_err_out_of_memory;
    }
    if (req == NULL) {
        if (extra != NULL)
            mem_free(a, extra, extra_size, _Alignof(StringsReader));
        BURROW_OUT(err, e);
        return NULL;
    }
    return cl_do(c, req, req, extra, extra_size, err);
}

HttpResponse *http_client_get(HttpClient *c, Str url, Error *err) {
    IoReader none = {NULL, NULL};
    return cl_new_and_do(c, BURROW_S("GET"), url, NULL, none, NULL, 0, err);
}

HttpResponse *http_client_head(HttpClient *c, Str url, Error *err) {
    IoReader none = {NULL, NULL};
    return cl_new_and_do(c, BURROW_S("HEAD"), url, NULL, none, NULL, 0, err);
}

HttpResponse *http_client_post(HttpClient *c, Str url, Str content_type, IoReader body,
                               Error *err) {
    return cl_new_and_do(c, BURROW_S("POST"), url, &content_type, body, NULL, 0, err);
}

HttpResponse *http_client_post_form(HttpClient *c, Str url, UrlValues data,
                                    Error *err) {
    /* The encoded form and the reader of it, in one block that lives as long
     * as the request. */
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Str enc = url_values_encode(data, arena_allocator(&scratch));
    size_t size = sizeof(StringsReader) + (size_t)enc.len;
    StringsReader *r =
        (StringsReader *)mem_alloc(cl_alloc(c), size, _Alignof(StringsReader));
    if (r == NULL) {
        arena_free(&scratch);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Byte *p = (Byte *)(r + 1);
    if (enc.len > 0)
        memcpy(p, enc.p, (size_t)enc.len);
    strings_reader_reset(r, str_from_bytes(p, enc.len));
    arena_free(&scratch);
    Str ct = BURROW_S("application/x-www-form-urlencoded");
    return cl_new_and_do(c, BURROW_S("POST"), url, &ct, strings_reader_as_io_reader(r),
                         r, size, err);
}

void http_client_close_idle_connections(HttpClient *c) {
    HttpTransport *t = burrow__http_as_transport(cl_transport(c));
    if (t != NULL)
        http_transport_close_idle_connections(t);
}

/* ----------------------------------------------------------- DefaultClient */

static HttpClient cl_default_client;

HttpClient *const http_default_client = &cl_default_client;

HttpResponse *http_get(Str url, Error *err) {
    return http_client_get(http_default_client, url, err);
}

HttpResponse *http_head(Str url, Error *err) {
    return http_client_head(http_default_client, url, err);
}

HttpResponse *http_post(Str url, Str content_type, IoReader body, Error *err) {
    return http_client_post(http_default_client, url, content_type, body, err);
}

HttpResponse *http_post_form(Str url, UrlValues data, Error *err) {
    return http_client_post_form(http_default_client, url, data, err);
}
