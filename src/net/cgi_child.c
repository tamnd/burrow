/* Derived from Go's src/net/http/cgi/child.go, CGI from the side of the
 * program a web server runs.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/cgi.h"

#include "cgi_internal.h"

#include "burrow/bufio.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stddef.h>

/* params[key], or "" when it is not there. */
static Str cc_get(Map *params, const char *key) {
    Str k = str_from_cstr(key);
    Str v = {0};
    (void)map_get2(params, &k, &v);
    return v;
}

/* envMap. The "key=value" strings in env, of Str, as a Map of Str to Str. */
static Map *cc_env_map(Alloc *a, Slice env) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, env.len);
    if (m == NULL)
        return NULL;
    for (Int i = 0; i < env.len; i++) {
        Str kv = ((const Str *)env.p)[i];
        Int eq = strings_index_byte(kv, '=');
        if (eq < 0)
            continue;
        Str k = str_from_bytes(kv.p, eq);
        Str v = str_from_bytes(kv.p + eq + 1, kv.len - eq - 1);
        if (!map_set(m, &k, &v))
            return NULL;
    }
    return m;
}

HttpRequest *cgi_request(Alloc *a, Error *err) {
    /* The environment and its map are in an arena of their own, which goes
     * once the request has copied what it keeps. */
    Arena ar;
    arena_init(&ar, a, 0);
    Error e = BURROW_NO_ERROR;
    Map *params = cc_env_map(arena_allocator(&ar), os_environ(arena_allocator(&ar)));
    HttpRequest *r = NULL;
    if (params == NULL)
        e = burrow_err_out_of_memory;
    else
        r = cgi_request_from_map(a, params, &e);
    arena_free(&ar);
    if (r == NULL) {
        *err = e;
        return NULL;
    }
    if (r->content_length > 0) {
        Alloc *ra = arena_allocator(&r->arena);
        IoLimitedReader *lr =
            (IoLimitedReader *)mem_alloc(ra, sizeof *lr, _Alignof(IoLimitedReader));
        IoNopCloser *nc =
            (IoNopCloser *)mem_alloc(ra, sizeof *nc, _Alignof(IoNopCloser));
        if (lr == NULL || nc == NULL) {
            http_request_free(r);
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        *lr = io_limit_reader(os_file_as_io_reader(os_stdin), r->content_length);
        *nc = io_nop_closer(io_limited_reader_as_io_reader(lr));
        r->body = io_nop_closer_as_io_read_closer(nc);
    }
    *err = BURROW_NO_ERROR;
    return r;
}

static Error cc_error(const char *text, Str s) {
    return fmt_errorf_v("%s%s", text, s);
}

HttpRequest *cgi_request_from_map(Alloc *a, Map *params, Error *err) {
    Str method = cc_get(params, "REQUEST_METHOD");
    if (method.len == 0) {
        *err = errors_new(error_allocator(),
                          BURROW_S("cgi: no REQUEST_METHOD in environment"));
        return NULL;
    }
    Str proto = cc_get(params, "SERVER_PROTOCOL");
    Int major = 0, minor = 0;
    if (str_eq(proto, BURROW_S("INCLUDED"))) {
        /* SSI (Server Side Include) use case
         * CGI Specification RFC 3875 - section 4.1.16 */
        major = 1;
        minor = 0;
    } else if (!http_parse_http_version(proto, &major, &minor)) {
        *err = errors_new(error_allocator(),
                          BURROW_S("cgi: invalid SERVER_PROTOCOL version"));
        return NULL;
    }

    HttpRequest *r = (HttpRequest *)mem_alloc(a, sizeof *r, _Alignof(HttpRequest));
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    r->a = a;
    arena_init(&r->arena, a, 0);
    Alloc *ra = arena_allocator(&r->arena);
    Error e = BURROW_NO_ERROR;

    r->ctx = context_background();
    r->method = str_clone(ra, method);
    r->proto = str_clone(ra, proto);
    r->proto_major = major;
    r->proto_minor = minor;
    r->close = true;
    r->trailer = http_header_make(ra);
    r->header = http_header_make(ra);
    r->transfer_encoding = slice_from(NULL, 0, 0, TYPE_STRING);
    r->host = str_clone(ra, cc_get(params, "HTTP_HOST"));
    if (r->trailer == NULL || r->header == NULL)
        goto oom;

    Str lenstr = cc_get(params, "CONTENT_LENGTH");
    if (lenstr.len > 0) {
        Error pe = BURROW_NO_ERROR;
        int64_t clen = strconv_parse_int(lenstr, 10, 64, &pe);
        if (BURROW_FAILED(pe)) {
            e = cc_error("cgi: bad CONTENT_LENGTH in environment: ", lenstr);
            goto fail;
        }
        r->content_length = clen;
    }

    Str ct = cc_get(params, "CONTENT_TYPE");
    if (ct.len > 0 &&
        !http_header_set(r->header, BURROW_S("Content-Type"), str_clone(ra, ct)))
        goto oom;

    /* Copy "HTTP_FOO_BAR" variables to "Foo-Bar" Headers */
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(params); map_next(&it, &kp, &vp);) {
        Str k = *(const Str *)kp;
        if (str_eq(k, BURROW_S("HTTP_HOST")))
            continue;
        bool found = false;
        Str after = strings_cut_prefix(k, BURROW_S("HTTP_"), &found);
        if (!found)
            continue;
        Str key = strings_replace_all(ra, after, BURROW_S("_"), BURROW_S("-"));
        if (key.p == after.p)
            key = str_clone(ra, key);
        if (!http_header_add(r->header, key, str_clone(ra, *(const Str *)vp)))
            goto oom;
    }

    Str uri_str = cc_get(params, "REQUEST_URI");
    if (uri_str.len == 0) {
        /* Fallback to SCRIPT_NAME, PATH_INFO and QUERY_STRING. */
        Str s = cc_get(params, "QUERY_STRING");
        uri_str = fmt_sprintf_v(ra, "%s%s%s%s", cc_get(params, "SCRIPT_NAME"),
                                cc_get(params, "PATH_INFO"),
                                s.len > 0 ? BURROW_S("?") : BURROW_STR_EMPTY, s);
    }

    /* There's apparently a de-facto standard for this.
     * https://web.archive.org/web/20170105004655/http://docstore.mik.ua/orelly/linux/cgi/ch03_02.htm#ch03-35636
     * Go sets r.TLS here, and there is no TLS field yet. */
    Str https = cc_get(params, "HTTPS");
    bool tls = str_eq(https, BURROW_S("on")) || str_eq(https, BURROW_S("ON")) ||
               str_eq(https, BURROW_S("1"));

    if (r->host.len > 0) {
        /* Hostname is provided, so we can reasonably construct a URL. */
        Str rawurl = fmt_sprintf_v(ra, "%s%s%s",
                                   tls ? BURROW_S("https://") : BURROW_S("http://"),
                                   r->host, uri_str);
        Error ue = BURROW_NO_ERROR;
        r->url = url_parse(ra, rawurl, &ue);
        if (r->url == NULL) {
            e = cc_error("cgi: failed to parse host and REQUEST_URI into a URL: ",
                         rawurl);
            goto fail;
        }
    }
    /* Fallback logic if we don't have a Host header or the URL
     * failed to parse */
    if (r->url == NULL) {
        Error ue = BURROW_NO_ERROR;
        r->url = url_parse(ra, uri_str, &ue);
        if (r->url == NULL) {
            e = cc_error("cgi: failed to parse REQUEST_URI into a URL: ", uri_str);
            goto fail;
        }
    }

    /* Request.RemoteAddr has its port set by Go's standard http
     * server, so we do here too. */
    Error pe = BURROW_NO_ERROR;
    Int remote_port = strconv_atoi(cc_get(params, "REMOTE_PORT"), &pe);
    if (BURROW_FAILED(pe))
        remote_port = 0; /* zero if unset or invalid */
    r->remote_addr = net_join_host_port(ra, cc_get(params, "REMOTE_ADDR"),
                                        strconv_itoa(ra, remote_port));
    *err = BURROW_NO_ERROR;
    return r;

oom:
    e = burrow_err_out_of_memory;
fail:
    http_request_free(r);
    *err = e;
    return NULL;
}

/* ---------------------------------------------------------- the response */

struct burrow__CgiResponse {
    HttpRequest *req;
    HttpHeader header;
    Int code;
    bool wrote_header;
    bool wrote_cgi_header;
    BufioWriter *bufw;
};

static void cc_write_header(void *self, Int code) {
    burrow__CgiResponse *r = (burrow__CgiResponse *)self;
    if (r->wrote_header) {
        /* Note: explicitly using Stderr, as Stdout is our HTTP output. */
        Alloc *ra = arena_allocator(&r->req->arena);
        (void)fmt_fprintf_v(os_file_as_io_writer(os_stderr),
                            "CGI attempted to write header twice on request for %s",
                            url_string(r->req->url, ra));
        return;
    }
    r->wrote_header = true;
    r->code = code;
}

/* writeCGIHeader finalizes the header sent to the client and writes it to the
 * output. p is not written by writeHeader, but is the first chunk of the body
 * that will be written. It is sniffed for a Content-Type if none is set
 * explicitly. */
void burrow__cgi_write_cgi_header(burrow__CgiResponse *r, Slice p) {
    if (r->wrote_cgi_header)
        return;
    r->wrote_cgi_header = true;
    IoWriter w = bufio_writer_as_io_writer(r->bufw);
    (void)fmt_fprintf_v(w, "Status: %d %s\r\n", r->code, http_status_text(r->code));
    Str ct = BURROW_S("Content-Type");
    if (map_get(r->header, &ct) == NULL)
        (void)http_header_set(r->header, ct, http_detect_content_type(p));
    (void)http_header_write(r->header, w);
    Error err;
    (void)bufio_writer_write_string(r->bufw, BURROW_S("\r\n"), &err);
    (void)bufio_writer_flush(r->bufw);
}

static Int cc_write(void *self, Slice p, Error *err) {
    burrow__CgiResponse *r = (burrow__CgiResponse *)self;
    if (!r->wrote_header)
        cc_write_header(r, 200);
    if (!r->wrote_cgi_header)
        burrow__cgi_write_cgi_header(r, p);
    return bufio_writer_write(r->bufw, p, err);
}

static HttpHeader cc_header(void *self) {
    return ((burrow__CgiResponse *)self)->header;
}

static Error cc_flush(void *self) {
    (void)bufio_writer_flush(((burrow__CgiResponse *)self)->bufw);
    return BURROW_NO_ERROR;
}

static const HttpResponseWriterVT cc_response_vt = {
    {NULL, cc_write},
    cc_header,
    cc_write_header,
    cc_flush,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

burrow__CgiResponse *burrow__cgi_new_response(Alloc *a, HttpRequest *req,
                                              IoWriter out) {
    burrow__CgiResponse *r =
        (burrow__CgiResponse *)mem_alloc(a, sizeof *r, _Alignof(burrow__CgiResponse));
    if (r == NULL)
        return NULL;
    r->req = req;
    r->header = http_header_make(a);
    r->bufw = bufio_new_writer(a, out);
    if (r->header == NULL || r->bufw == NULL)
        return NULL;
    return r;
}

HttpResponseWriter burrow__cgi_response_writer(burrow__CgiResponse *r) {
    return (HttpResponseWriter){&cc_response_vt, r};
}

Error cgi_serve(HttpHandler handler) {
    Alloc *a = heap_allocator();
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = cgi_request(a, &err);
    if (req == NULL)
        return err;
    if (req->body.vt == NULL)
        req->body = http_no_body;
    if (handler.vt == NULL)
        handler = http_serve_mux_as_handler(http_default_serve_mux);
    burrow__CgiResponse *rw = burrow__cgi_new_response(
        arena_allocator(&req->arena), req, os_file_as_io_writer(os_stdout));
    if (rw == NULL) {
        http_request_free(req);
        return burrow_err_out_of_memory;
    }
    http_handler_serve_http(handler, burrow__cgi_response_writer(rw), req);
    (void)cc_write(rw, (Slice){0}, &err); /* make sure a response is sent */
    err = bufio_writer_flush(rw->bufw);
    http_request_free(req);
    return err;
}
