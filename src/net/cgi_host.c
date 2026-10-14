/* Derived from Go's src/net/http/cgi/host.go, CGI from the side of the web
 * server, which runs the program.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/cgi.h"

#include "cgi_internal.h"
#include "http_internal.h"

#include "burrow/bufio.h"
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
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stddef.h>

/* osDefaultInheritEnv. */
static const Str ch_default_inherit_env[] = {
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    BURROW_S_INIT("DYLD_LIBRARY_PATH"),
#elif defined(BURROW_OS_ANDROID) || defined(BURROW_OS_LINUX) ||                        \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD)
    BURROW_S_INIT("LD_LIBRARY_PATH"),
#elif defined(BURROW_OS_SOLARIS)
    BURROW_S_INIT("LD_LIBRARY_PATH"), BURROW_S_INIT("LD_LIBRARY_PATH_32"),
    BURROW_S_INIT("LD_LIBRARY_PATH_64"),
#elif defined(BURROW_OS_WINDOWS)
    BURROW_S_INIT("SystemRoot"), BURROW_S_INIT("COMSPEC"),
    BURROW_S_INIT("PATHEXT"),    BURROW_S_INIT("WINDIR"),
#endif
    BURROW_S_INIT(""), /* so the array is never empty */
};

/* trailingPort, the regexp `:([0-9]+)$`. The digits at the end of host, when
 * there is at least one and a colon comes before them. */
static bool ch_trailing_port(Str host, Str *port) {
    Int i = host.len;
    while (i > 0 && host.p[i - 1] >= '0' && host.p[i - 1] <= '9')
        i--;
    if (i == host.len || i == 0 || host.p[i - 1] != ':')
        return false;
    *port = str_from_bytes(host.p + i, host.len - i);
    return true;
}

Int burrow__cgi_remove_leading_duplicates(Str *env, Int n) {
    Int out = 0;
    for (Int i = 0; i < n; i++) {
        Str e = env[i];
        bool found = false;
        Int eq = strings_index_byte(e, '=');
        if (eq != -1) {
            Str keq = str_from_bytes(e.p, eq + 1); /* "key=" */
            for (Int j = i + 1; j < n && !found; j++)
                found = strings_has_prefix(env[j], keq);
        }
        if (!found)
            env[out++] = e;
    }
    return out;
}

static Rune ch_upper_case_and_underscore(void *env, Rune r) {
    (void)env;
    if (r >= 'a' && r <= 'z')
        return r - ('a' - 'A');
    if (r == '-')
        return '_';
    if (r == '=') {
        /* Maybe not part of the CGI 'spec' but would mess up
         * the environment in any case, as Go represents the
         * environment as a slice of "key=value" strings. */
        return '_';
    }
    /* TODO: other transformations in spec or practice? */
    return r;
}

/* The environment being built, of Str, in the request's scratch arena. */
typedef struct ChEnv {
    Alloc *a;
    Str *p;
    Int n, cap;
    bool oom;
} ChEnv;

static void ch_push(ChEnv *e, Str s) {
    if (s.p == NULL && s.len > 0)
        e->oom = true;
    if (e->oom)
        return;
    if (e->n == e->cap) {
        Int cap = e->cap * 2 + 16;
        Str *p = (Str *)mem_alloc_array(e->a, (size_t)cap, sizeof(Str), _Alignof(Str));
        if (p == NULL) {
            e->oom = true;
            return;
        }
        for (Int i = 0; i < e->n; i++)
            p[i] = e->p[i];
        e->p = p;
        e->cap = cap;
    }
    e->p[e->n++] = s;
}

static void ch_push_kv(ChEnv *e, const char *key, Str v) {
    ch_push(e, fmt_sprintf_v(e->a, "%s%s", key, v));
}

#define ch_printf(h, ...) log_logger_printf_v((h)->logger, __VA_ARGS__)

static void ch_internal_error(CgiHandler *h, HttpResponseWriter rw, Error err) {
    http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    ch_printf(h, "CGI error: %v", err);
}

static void ch_handle_internal_redirect(CgiHandler *h, HttpResponseWriter rw,
                                        HttpRequest *req, Str path) {
    Alloc *a = heap_allocator();
    HttpRequest *nr = (HttpRequest *)mem_alloc(a, sizeof *nr, _Alignof(HttpRequest));
    if (nr == NULL) {
        ch_internal_error(h, rw, burrow_err_out_of_memory);
        return;
    }
    nr->a = a;
    arena_init(&nr->arena, a, 0);
    Alloc *ra = arena_allocator(&nr->arena);
    Error err = BURROW_NO_ERROR;
    Url *url = url_parse_ref(req->url, ra, path, &err);
    if (url == NULL) {
        http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        ch_printf(h, "cgi: error resolving local URI path %q: %v", path, err);
        http_request_free(nr);
        return;
    }
    /* TODO: RFC 3875 isn't clear if only GET is supported, but it
     * suggests so: "Note that any message-body attached to the
     * request (such as for a POST request) may not be available
     * to the resource that is the target of the redirect."  We
     * should do some tests against Apache to see how it handles
     * POST, HEAD, etc. Does the internal redirect get the same
     * method or just GET? What about incoming headers?
     * (e.g. Cookies) Which headers, if any, are copied into the
     * second request? */
    nr->ctx = context_background();
    nr->method = BURROW_S("GET");
    nr->url = url;
    nr->proto = BURROW_S("HTTP/1.1");
    nr->proto_major = 1;
    nr->proto_minor = 1;
    nr->header = http_header_make(ra);
    nr->host = url->host;
    nr->remote_addr = str_clone(ra, req->remote_addr);
    nr->transfer_encoding = slice_from(NULL, 0, 0, TYPE_STRING);
    nr->body = http_no_body;
    if (nr->header == NULL) {
        ch_internal_error(h, rw, burrow_err_out_of_memory);
        http_request_free(nr);
        return;
    }
    http_handler_serve_http(h->path_location_handler, rw, nr);
    http_request_free(nr);
}

/* What ServeHTTP holds, given back by ch_serve_end. */
typedef struct ChServe {
    Arena arena;
    ExecCmd cmd;
    IoReadCloser stdout_read;
    bool started;
} ChServe;

/* Go's deferred stdoutRead.Close and cmd.Wait. */
static void ch_serve_end(ChServe *s) {
    if (s->started) {
        (void)s->stdout_read.vt->closer.close(s->stdout_read.data);
        (void)exec_cmd_wait(&s->cmd);
    }
    exec_cmd_free(&s->cmd);
    arena_free(&s->arena);
}

/* Reads the program's header, and answers false when ServeHTTP is done
 * because of something it found there. */
static bool ch_read_header(CgiHandler *h, HttpResponseWriter rw, BufioReader *linebody,
                           HttpHeader headers, Int *status_code) {
    Int header_lines = 0;
    bool saw_blank_line = false;
    for (;;) {
        bool is_prefix = false;
        Error err = BURROW_NO_ERROR;
        Slice lineb = bufio_reader_read_line(linebody, &is_prefix, &err);
        if (is_prefix) {
            http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
            ch_printf(h, "cgi: long header line from subprocess.");
            return false;
        }
        if (errors_is(err, io_eof))
            break;
        if (BURROW_FAILED(err)) {
            http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
            ch_printf(h, "cgi: error reading headers: %v", err);
            return false;
        }
        /* The line is in linebody's buffer, which the next read reuses, and
         * headers keeps what it is given, so the line is copied first. */
        Str line = str_clone(burrow__map_allocator(headers),
                             str_from_bytes(lineb.p, lineb.len));
        if (line.len > 0 && line.p == NULL) {
            ch_internal_error(h, rw, burrow_err_out_of_memory);
            return false;
        }
        if (line.len == 0) {
            saw_blank_line = true;
            break;
        }
        header_lines++;
        Str val = {0};
        bool ok = false;
        Str header = strings_cut(line, BURROW_S(":"), &val, &ok);
        if (!ok) {
            ch_printf(h, "cgi: bogus header line: %s", line);
            continue;
        }
        if (!burrow__http_is_token(header)) {
            ch_printf(h, "cgi: invalid header name: %q", header);
            continue;
        }
        val = textproto_trim_string(val);
        if (str_eq(header, BURROW_S("Status"))) {
            if (val.len < 3) {
                ch_printf(h, "cgi: bogus status (short): %q", val);
                return false;
            }
            Error ae = BURROW_NO_ERROR;
            Int code = strconv_atoi(str_from_bytes(val.p, 3), &ae);
            if (BURROW_FAILED(ae)) {
                ch_printf(h, "cgi: bogus status: %q", val);
                ch_printf(h, "cgi: line was %q", line);
                return false;
            }
            *status_code = code;
        } else if (!http_header_add(headers, header, val)) {
            ch_internal_error(h, rw, burrow_err_out_of_memory);
            return false;
        }
    }
    if (header_lines == 0 || !saw_blank_line) {
        http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        ch_printf(h, "cgi: no headers");
        return false;
    }
    return true;
}

/* Builds the program's environment. False when memory ran out. */
static bool ch_build_env(CgiHandler *h, HttpRequest *req, Str root, ChEnv *env) {
    Alloc *a = env->a;
    Str path_info = strings_trim_prefix(req->url->path, root);
    Str port = BURROW_S("80"); /* "443" with TLS, which there isn't yet */
    Str m = {0};
    if (ch_trailing_port(req->host, &m))
        port = m;

    ch_push(env, BURROW_S("SERVER_SOFTWARE=go"));
    ch_push(env, BURROW_S("SERVER_PROTOCOL=HTTP/1.1"));
    ch_push_kv(env, "HTTP_HOST=", req->host);
    ch_push(env, BURROW_S("GATEWAY_INTERFACE=CGI/1.1"));
    ch_push_kv(env, "REQUEST_METHOD=", req->method);
    ch_push_kv(env, "QUERY_STRING=", req->url->raw_query);
    ch_push_kv(env, "REQUEST_URI=", url_request_uri(req->url, a));
    ch_push_kv(env, "PATH_INFO=", path_info);
    ch_push_kv(env, "SCRIPT_NAME=", root);
    ch_push_kv(env, "SCRIPT_FILENAME=", h->path);
    ch_push_kv(env, "SERVER_PORT=", port);

    Error err = BURROW_NO_ERROR;
    Str remote_port = {0};
    Str remote_ip = net_split_host_port(req->remote_addr, &remote_port, &err);
    if (!BURROW_FAILED(err)) {
        ch_push_kv(env, "REMOTE_ADDR=", remote_ip);
        ch_push_kv(env, "REMOTE_HOST=", remote_ip);
        ch_push_kv(env, "REMOTE_PORT=", remote_port);
    } else {
        /* could not parse ip:port, let's use whole RemoteAddr and leave
         * REMOTE_PORT undefined */
        ch_push_kv(env, "REMOTE_ADDR=", req->remote_addr);
        ch_push_kv(env, "REMOTE_HOST=", req->remote_addr);
    }

    err = BURROW_NO_ERROR;
    Str host_domain = net_split_host_port(req->host, NULL, &err);
    ch_push_kv(env, "SERVER_NAME=", BURROW_FAILED(err) ? req->host : host_domain);

    const void *kp;
    void *vp;
    for (MapIter it = map_iter(req->header); map_next(&it, &kp, &vp);) {
        Str k =
            strings_map(a, BURROW_FN(RuneMapFunc, ch_upper_case_and_underscore, NULL),
                        *(const Str *)kp);
        if (str_eq(k, BURROW_S("PROXY"))) {
            /* See Issue 16405 */
            continue;
        }
        Str join_str = str_eq(k, BURROW_S("COOKIE")) ? BURROW_S("; ") : BURROW_S(", ");
        ch_push(env, fmt_sprintf_v(a, "HTTP_%s=%s", k,
                                   strings_join(a, *(const Slice *)vp, join_str)));
    }

    if (req->content_length > 0)
        ch_push(env, fmt_sprintf_v(a, "CONTENT_LENGTH=%d", req->content_length));
    Str ctype = http_header_get(req->header, BURROW_S("Content-Type"));
    if (ctype.len > 0)
        ch_push_kv(env, "CONTENT_TYPE=", ctype);

    Str env_path = os_getenv(a, BURROW_S("PATH"));
    if (env_path.len == 0)
        env_path = BURROW_S("/bin:/usr/bin:/usr/ucb:/usr/bsd:/usr/local/bin");
    ch_push_kv(env, "PATH=", env_path);

    for (Int i = 0; i < h->inherit_env.len; i++) {
        Str e = ((const Str *)h->inherit_env.p)[i];
        Str v = os_getenv(a, e);
        if (v.len > 0)
            ch_push(env, fmt_sprintf_v(a, "%s=%s", e, v));
    }
    for (size_t i = 0;
         i < sizeof ch_default_inherit_env / sizeof ch_default_inherit_env[0]; i++) {
        Str e = ch_default_inherit_env[i];
        if (e.len == 0)
            continue;
        Str v = os_getenv(a, e);
        if (v.len > 0)
            ch_push(env, fmt_sprintf_v(a, "%s=%s", e, v));
    }
    for (Int i = 0; i < h->env.len; i++)
        ch_push(env, ((const Str *)h->env.p)[i]);
    if (env->oom)
        return false;
    env->n = burrow__cgi_remove_leading_duplicates(env->p, env->n);
    return true;
}

static const Str ch_text_chunked =
    BURROW_S_INIT("Chunked request bodies are not supported by CGI.");

void cgi_handler_serve_http(CgiHandler *h, HttpResponseWriter rw, HttpRequest *req) {
    if (req->transfer_encoding.len > 0 &&
        str_eq(((const Str *)req->transfer_encoding.p)[0], BURROW_S("chunked"))) {
        http_response_writer_write_header(rw, HTTP_STATUS_BAD_REQUEST);
        (void)io_write_string(http_response_writer_as_io_writer(rw), ch_text_chunked,
                              NULL);
        return;
    }

    ChServe s = {0};
    arena_init(&s.arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&s.arena);

    Str root = strings_trim_right(h->root, BURROW_S("/"));
    ChEnv env = {a, NULL, 0, 0, false};
    if (!ch_build_env(h, req, root, &env)) {
        ch_internal_error(h, rw, burrow_err_out_of_memory);
        ch_serve_end(&s);
        return;
    }

    Str cwd, path;
    if (h->dir.len > 0) {
        path = h->path;
        cwd = h->dir;
    } else {
        cwd = filepath_split(h->path, &path);
    }
    if (cwd.len == 0)
        cwd = BURROW_S(".");

    Str *args = (Str *)mem_alloc_array(a, (size_t)(h->args.len + 1), sizeof(Str),
                                       _Alignof(Str));
    if (args == NULL) {
        ch_internal_error(h, rw, burrow_err_out_of_memory);
        ch_serve_end(&s);
        return;
    }
    args[0] = h->path;
    for (Int i = 0; i < h->args.len; i++)
        args[i + 1] = ((const Str *)h->args.p)[i];

    s.cmd.path = path;
    s.cmd.args = slice_from(args, h->args.len + 1, h->args.len + 1, TYPE_STRING);
    s.cmd.dir = cwd;
    s.cmd.env = slice_from(env.p, env.n, env.cap, TYPE_STRING);
    s.cmd.stderr_ =
        h->stderr_.vt != NULL ? h->stderr_ : os_file_as_io_writer(os_stderr);
    if (req->content_length != 0 && req->body.vt != NULL)
        s.cmd.stdin_ = io_read_closer_as_io_reader(req->body);

    Error err = BURROW_NO_ERROR;
    s.stdout_read = exec_cmd_stdout_pipe(&s.cmd, &err);
    if (BURROW_FAILED(err)) {
        ch_internal_error(h, rw, err);
        ch_serve_end(&s);
        return;
    }
    err = exec_cmd_start(&s.cmd);
    if (BURROW_FAILED(err)) {
        ch_internal_error(h, rw, err);
        ch_serve_end(&s);
        return;
    }
    s.started = true;

    BufioReader *linebody =
        bufio_new_reader_size(a, io_read_closer_as_io_reader(s.stdout_read), 1024);
    HttpHeader headers = http_header_make(a);
    if (linebody == NULL || headers == NULL) {
        ch_internal_error(h, rw, burrow_err_out_of_memory);
        ch_serve_end(&s);
        return;
    }
    Int status_code = 0;
    if (!ch_read_header(h, rw, linebody, headers, &status_code)) {
        ch_serve_end(&s);
        return;
    }

    Str loc = http_header_get(headers, BURROW_S("Location"));
    if (loc.len > 0) {
        if (strings_has_prefix(loc, BURROW_S("/")) &&
            h->path_location_handler.vt != NULL) {
            ch_handle_internal_redirect(h, rw, req, loc);
            ch_serve_end(&s);
            return;
        }
        if (status_code == 0)
            status_code = HTTP_STATUS_FOUND;
    }

    if (status_code == 0 &&
        http_header_get(headers, BURROW_S("Content-Type")).len == 0) {
        http_response_writer_write_header(rw, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        ch_printf(h, "cgi: missing required Content-Type in headers");
        ch_serve_end(&s);
        return;
    }

    if (status_code == 0)
        status_code = HTTP_STATUS_OK;

    /* Copy headers to rw's headers, after we've decided not to
     * go into handleInternalRedirect, which won't want its rw
     * headers to have been touched. */
    HttpHeader out = http_response_writer_header(rw);
    /* The child's headers are in s.arena, which goes when this returns, and
     * out keeps what it is given, so they are copied into out's allocator. */
    Alloc *oa = burrow__map_allocator(out);
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(headers); map_next(&it, &kp, &vp);) {
        const Slice *vv = (const Slice *)vp;
        Str k = str_clone(oa, *(const Str *)kp);
        for (Int i = 0; i < vv->len; i++)
            (void)http_header_add(out, k, str_clone(oa, ((const Str *)vv->p)[i]));
    }

    http_response_writer_write_header(rw, status_code);

    err = BURROW_NO_ERROR;
    (void)io_copy(a, http_response_writer_as_io_writer(rw),
                  bufio_reader_as_io_reader(linebody), &err);
    if (BURROW_FAILED(err)) {
        ch_printf(h, "cgi: copy error: %v", err);
        /* And kill the child CGI process so we don't hang on
         * the deferred cmd.Wait above if the error was just
         * the client (rw) going away. If it was a read error
         * (because the child died itself), then the extra
         * kill of an already-dead process is harmless (the PID
         * won't be reused until the Wait above). */
        (void)os_process_kill(s.cmd.process);
    }
    ch_serve_end(&s);
}

static void ch_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    cgi_handler_serve_http((CgiHandler *)self, w, r);
}

static const HttpHandlerVT ch_handler_vt = {NULL, ch_handler_serve};

HttpHandler cgi_handler_as_handler(CgiHandler *h) {
    return (HttpHandler){&ch_handler_vt, h};
}
