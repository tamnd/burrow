/* Derived from Go's src/net/http/pprof/pprof.go, the runtime's profiles served
 * over HTTP. Go source: go1.27.1. See burrow/net/http/pprof.h.
 *
 * The delta profiles go through internal/profile, which burrow has in
 * src/debug/pprof_profile.c for the tests of runtime/pprof.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/pprof.h"

#include "../debug/pprof_profile.h"
#include "http_internal.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/html.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/runtime.h"
#include "burrow/runtime/pprof.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S(lit)

static void hpp_set(HttpResponseWriter w, Str key, Str value) {
    (void)http_header_set(http_response_writer_header(w), key, value);
}

/* Content-Disposition for a profile, with its value made where the header's
 * own are, since a header keeps the bytes it is given and the response can be
 * looked at after the handler is done. */
static void hpp_set_disposition(HttpResponseWriter w, Str name, const char *suffix) {
    HttpHeader h = http_response_writer_header(w);
    Str v = fmt_sprintf_v(burrow__map_allocator(h), "attachment; filename=\"%s%s\"",
                          name, suffix);
    if (v.p == NULL)
        return; /* Not in Go: no memory for it, so no header either. */
    (void)http_header_set(h, S_("Content-Disposition"), v);
}

/* Cmdline responds with the running program's command line, with arguments
 * separated by NUL bytes. */
void http_pprof_cmdline(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    hpp_set(w, S_("Content-Type"), S_("text/plain; charset=utf-8"));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = strings_join(arena_allocator(&ar), os_args(), S_("\x00"));
    (void)fmt_fprint_v(http_response_writer_as_io_writer(w), s);
    arena_free(&ar);
}

static void hpp_sleep(HttpRequest *r, Duration d) {
    Chan *after = time_after_chan(heap_allocator(), d);
    if (after == NULL) {
        /* Not in Go: out of memory for the timer, so no waking up early when
         * the request goes away. */
        time_sleep(d);
        return;
    }
    SelectCase cases[] = {
        BURROW_RECV(after, NULL),
        BURROW_RECV(context_done(http_request_context(r)), NULL),
    };
    (void)chan_select(cases, 2);
    chan_free(after);
}

static void hpp_configure_write_deadline(HttpResponseWriter w, HttpRequest *r,
                                         double seconds) {
    Any v = context_value(http_request_context(r), http_server_context_key);
    HttpServer *srv = v.t == TYPE_HTTP_SERVER ? (HttpServer *)v.data : NULL;
    if (srv != NULL && srv->write_timeout > 0) {
        Duration timeout =
            srv->write_timeout + (Duration)(seconds * (double)TIME_SECOND);

        HttpResponseController rc = http_new_response_controller(w);
        (void)http_response_controller_set_write_deadline(
            &rc, time_add(time_now(), timeout));
    }
}

static void hpp_serve_error(HttpResponseWriter w, Int status, Str txt) {
    HttpHeader h = http_response_writer_header(w);
    http_header_set(h, S_("Content-Type"), S_("text/plain; charset=utf-8"));
    http_header_set(h, S_("X-Go-Pprof"), S_("1"));
    http_header_del(h, S_("Content-Disposition"));
    http_response_writer_write_header(w, status);
    (void)fmt_fprintln_v(http_response_writer_as_io_writer(w), txt);
}

/* Profile responds with the pprof-formatted cpu profile. Profiling lasts for
 * duration specified in seconds GET parameter, or for 30 seconds if not
 * specified. */
void http_pprof_profile(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    Error err = BURROW_NO_ERROR;
    int64_t sec =
        strconv_parse_int(http_request_form_value(r, S_("seconds")), 10, 64, &err);
    if (sec <= 0 || BURROW_FAILED(err))
        sec = 30;

    hpp_configure_write_deadline(w, r, (double)sec);

    /* Set Content Type assuming StartCPUProfile will work, because if it does
     * it starts writing. */
    hpp_set(w, S_("Content-Type"), S_("application/octet-stream"));
    hpp_set(w, S_("Content-Disposition"), S_("attachment; filename=\"profile\""));
    err = pprof_start_cpu_profile(http_response_writer_as_io_writer(w));
    if (BURROW_FAILED(err)) {
        /* StartCPUProfile failed, so no writes yet. */
        Arena ar;
        arena_init(&ar, NULL, 0);
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        fmt_sprintf_v(arena_allocator(&ar),
                                      "Could not enable CPU profiling: %s", err));
        arena_free(&ar);
        return;
    }
    hpp_sleep(r, (Duration)sec * TIME_SECOND);
    pprof_stop_cpu_profile();
}

/* Trace responds with the execution trace in binary form. Tracing lasts for
 * duration specified in seconds GET parameter, or for 1 second if not
 * specified.
 *
 * Not in Go: burrow has no runtime/trace yet, so this always takes the path Go
 * takes when trace.Start fails. */
void http_pprof_trace(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    Error err = BURROW_NO_ERROR;
    double sec =
        strconv_parse_float(http_request_form_value(r, S_("seconds")), 64, &err);
    if (sec <= 0 || BURROW_FAILED(err))
        sec = 1;

    hpp_configure_write_deadline(w, r, sec);

    /* Set Content Type assuming trace.Start will work, because if it does it
     * starts writing. */
    hpp_set(w, S_("Content-Type"), S_("application/octet-stream"));
    hpp_set(w, S_("Content-Disposition"), S_("attachment; filename=\"trace\""));
    hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                    S_("Could not enable tracing: burrow has no execution tracer yet"));
}

/* Symbol looks up the program counters listed in the request, responding with
 * a table mapping program counters to function names. */
void http_pprof_symbol(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    hpp_set(w, S_("Content-Type"), S_("text/plain; charset=utf-8"));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* We have to read the whole POST body before writing any output. Buffer
     * the output here. */
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter bw = bytes_buffer_as_io_writer(&buf);

    /* We don't know how many symbols we have, but we do have symbol
     * information. Pprof only cares whether this number is 0 (no symbols
     * available) or > 0. */
    (void)fmt_fprintf_v(bw, "num_symbols: 1\n");

    StringsReader query;
    IoReader in;
    if (str_eq(r->method, S_("POST"))) {
        in = io_read_closer_as_io_reader(r->body);
    } else {
        strings_reader_reset(&query, r->url->raw_query);
        in = strings_reader_as_io_reader(&query);
    }
    BufioReader *b = bufio_new_reader(a, in);
    if (b == NULL) {
        /* Not in Go: the arena said no. */
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        S_("net/http/pprof: out of memory"));
        arena_free(&ar);
        return;
    }

    for (;;) {
        Error err = BURROW_NO_ERROR;
        Slice word = bufio_reader_read_slice(b, '+', &err);
        if (BURROW_OK(err))
            word.len--; /* trim + */
        Error perr = BURROW_NO_ERROR;
        uint64_t pc =
            strconv_parse_uint(str_from_bytes(word.p, word.len), 0, 64, &perr);
        if (pc != 0) {
            RuntimeFunc f;
            if (runtime_func_for_pc((Uintptr)pc, &f))
                (void)fmt_fprintf_v(bw, "%#x %s\n", pc, f.name);
        }

        /* Wait until here to check for err; the last symbol will have an err
         * because it doesn't end in +. */
        if (BURROW_FAILED(err)) {
            if (!errors_is(err, io_eof))
                (void)fmt_fprintf_v(bw, "reading request: %v\n", err);
            break;
        }
    }

    (void)http_response_writer_write(w, bytes_buffer_bytes(&buf), NULL);
    arena_free(&ar);
}

static bool hpp_profile_supports_delta(Str name) {
    static const Str names[] = {
        BURROW_S_INIT("allocs"),        BURROW_S_INIT("block"),
        BURROW_S_INIT("goroutineleak"), BURROW_S_INIT("goroutine"),
        BURROW_S_INIT("heap"),          BURROW_S_INIT("mutex"),
        BURROW_S_INIT("threadcreate"),
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (str_eq(name, names[i]))
            return true;
    return false;
}

static PfProfile *hpp_collect_profile(Alloc *a, PprofProfile *p, Error *err) {
    BytesBuffer buf = BYTES_BUFFER(a);
    *err = pprof_profile_write_to(p, bytes_buffer_as_io_writer(&buf), 0);
    if (BURROW_FAILED(*err))
        return NULL;
    int64_t ts = time_unix_nano(time_now());
    PfProfile *p0 = burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), err);
    if (p0 == NULL || BURROW_FAILED(*err))
        return NULL;
    p0->time_nanos = ts;
    return p0;
}

static void hpp_serve_delta_profile(Str name, HttpResponseWriter w, HttpRequest *r,
                                    PprofProfile *p, Str sec_str) {
    Error err = BURROW_NO_ERROR;
    int64_t sec = strconv_parse_int(sec_str, 10, 64, &err);
    if (BURROW_FAILED(err) || sec <= 0) {
        hpp_serve_error(
            w, HTTP_STATUS_BAD_REQUEST,
            S_("invalid value for \"seconds\" - must be a positive integer"));
        return;
    }
    /* 'name' should be a key in profileSupportsDelta. */
    if (!hpp_profile_supports_delta(name)) {
        hpp_serve_error(
            w, HTTP_STATUS_BAD_REQUEST,
            S_("\"seconds\" parameter is not supported for this profile type"));
        return;
    }

    hpp_configure_write_deadline(w, r, (double)sec);

    Int debug = strconv_atoi(http_request_form_value(r, S_("debug")), NULL);
    if (debug != 0) {
        hpp_serve_error(w, HTTP_STATUS_BAD_REQUEST,
                        S_("seconds and debug params are incompatible"));
        return;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PfProfile *p0 = hpp_collect_profile(a, p, &err);
    if (p0 == NULL) {
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        S_("failed to collect profile"));
        goto out;
    }

    TimeTimer *t = time_new_timer(heap_allocator(), (Duration)sec * TIME_SECOND);
    if (t == NULL) {
        /* Not in Go: no memory for the timer. */
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        S_("net/http/pprof: out of memory"));
        goto out;
    }

    Context ctx = http_request_context(r);
    SelectCase cases[] = {
        BURROW_RECV(context_done(ctx), NULL),
        BURROW_RECV(time_timer_c(t), NULL),
    };
    Int chosen = chan_select(cases, 2);
    time_timer_free(t);
    if (chosen == 0) {
        err = context_err(ctx);
        if (errors_is(err, context_deadline_exceeded))
            hpp_serve_error(w, HTTP_STATUS_REQUEST_TIMEOUT, error_text(err));
        else /* TODO: what's a good status code for canceled requests? 400? */
            hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR, error_text(err));
        goto out;
    }

    PfProfile *p1 = hpp_collect_profile(a, p, &err);
    if (p1 == NULL) {
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        S_("failed to collect profile"));
        goto out;
    }
    int64_t ts = p1->time_nanos;
    int64_t dur = p1->time_nanos - p0->time_nanos;

    burrow__pf_profile_scale(p0, -1);

    PfProfile *ps[] = {p0, p1};
    p1 = burrow__pf_merge(a, slice_from(ps, 2, 2, TYPE_UNSAFE_POINTER), &err);
    if (p1 == NULL || BURROW_FAILED(err)) {
        hpp_serve_error(w, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        S_("failed to compute delta"));
        goto out;
    }

    p1->time_nanos = ts; /* set since we don't know what profile.Merge set for
                          * TimeNanos. */
    p1->duration_nanos = dur;

    hpp_set(w, S_("Content-Type"), S_("application/octet-stream"));
    hpp_set_disposition(w, name, "-delta");
    (void)burrow__pf_profile_write(p1, http_response_writer_as_io_writer(w));
out:
    arena_free(&ar);
}

static void hpp_serve_named(Str name, HttpResponseWriter w, HttpRequest *r) {
    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    PprofProfile *p = pprof_lookup(name);
    if (p == NULL) {
        hpp_serve_error(w, HTTP_STATUS_NOT_FOUND, S_("Unknown profile"));
        return;
    }
    Str sec = http_request_form_value(r, S_("seconds"));
    if (sec.len != 0) {
        hpp_serve_delta_profile(name, w, r, p, sec);
        return;
    }
    /* Not in Go: gc=N is not looked at. Go runs a collection for it on the
     * heap profile, and burrow has no collector, so the profile is already as
     * current as it gets. */
    Int debug = strconv_atoi(http_request_form_value(r, S_("debug")), NULL);
    if (debug != 0) {
        hpp_set(w, S_("Content-Type"), S_("text/plain; charset=utf-8"));
    } else {
        hpp_set(w, S_("Content-Type"), S_("application/octet-stream"));
        hpp_set_disposition(w, name, "");
    }
    (void)pprof_profile_write_to(p, http_response_writer_as_io_writer(w), debug);
}

/* Go's handler, a string, here a Str with the bytes it points at after it. */
static const Type hpp_handler_desc = {
    {(const Byte *)"handler", 7},
    {(const Byte *)"net/http/pprof", 14},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68707066U, /* "hppf" */
    NULL,
};

static void hpp_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    hpp_serve_named(*(const Str *)self, w, r);
}

static const HttpHandlerVT hpp_handler_vt = {&hpp_handler_desc, hpp_handler_serve};

HttpHandler http_pprof_handler(Alloc *a, Str name) {
    Str *h = (Str *)mem_alloc(a, sizeof(Str) + (size_t)name.len, _Alignof(Str));
    if (h == NULL)
        return (HttpHandler){&hpp_handler_vt, NULL};
    Byte *bytes = (Byte *)(h + 1);
    if (name.len > 0)
        memcpy(bytes, name.p, (size_t)name.len);
    *h = (Str){bytes, name.len};
    return (HttpHandler){&hpp_handler_vt, h};
}

static const struct {
    Str name;
    Str desc;
} hpp_profile_descriptions[] = {
    {BURROW_S_INIT("allocs"),
     BURROW_S_INIT("A sampling of all past memory allocations")},
    {BURROW_S_INIT("block"),
     BURROW_S_INIT("Stack traces that led to blocking on synchronization primitives")},
    {BURROW_S_INIT("cmdline"),
     BURROW_S_INIT("The command line invocation of the current program")},
    {BURROW_S_INIT("goroutine"),
     BURROW_S_INIT("Stack traces of all current goroutines. Use debug=2 as a query "
                   "parameter to export in the same format as an unrecovered panic.")},
    {BURROW_S_INIT("heap"),
     BURROW_S_INIT("A sampling of memory allocations of live objects. You can specify "
                   "the gc GET parameter to run GC before taking the heap sample.")},
    {BURROW_S_INIT("mutex"),
     BURROW_S_INIT("Stack traces of holders of contended mutexes")},
    {BURROW_S_INIT("profile"),
     BURROW_S_INIT("CPU profile. You can specify the duration in the seconds GET "
                   "parameter. After you get the profile file, use the go tool pprof "
                   "command to investigate the profile.")},
    {BURROW_S_INIT("symbol"),
     BURROW_S_INIT("Maps given program counters to function names. Counters can be "
                   "specified in a GET raw query or POST body, multiple counters are "
                   "separated by '+'.")},
    {BURROW_S_INIT("threadcreate"),
     BURROW_S_INIT("Stack traces that led to the creation of new OS threads")},
    {BURROW_S_INIT("trace"),
     BURROW_S_INIT("A trace of execution of the current program. You can specify the "
                   "duration in the seconds GET parameter. After you get the trace "
                   "file, use the go tool trace command to investigate the trace.")},
    {BURROW_S_INIT("goroutineleak"),
     BURROW_S_INIT("Stack traces of all leaked goroutines. Use debug=2 as a query "
                   "parameter to export in the same format as an unrecovered panic.")},
};

bool burrow__http_pprof_description(Str name, Str *desc) {
    for (size_t i = 0;
         i < sizeof hpp_profile_descriptions / sizeof hpp_profile_descriptions[0];
         i++) {
        if (str_eq(name, hpp_profile_descriptions[i].name)) {
            *desc = hpp_profile_descriptions[i].desc;
            return true;
        }
    }
    *desc = (Str){0};
    return false;
}

typedef struct HppProfileEntry {
    Str name;
    Str href;
    Str desc;
    Int count;
} HppProfileEntry;

static int hpp_entry_compare(const void *x, const void *y) {
    const HppProfileEntry *a = (const HppProfileEntry *)x;
    const HppProfileEntry *b = (const HppProfileEntry *)y;
    return (int)strings_compare(a->name, b->name);
}

static Error hpp_index_tmpl_execute(Alloc *a, IoWriter w,
                                    const HppProfileEntry *profiles, Int n) {
    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter bw = bytes_buffer_as_io_writer(&b);
    (void)bytes_buffer_write_string(
        &b,
        S_("<html>\n"
           "<head>\n"
           "<title>/debug/pprof/</title>\n"
           "<style>\n"
           ".profile-name{\n"
           "\tdisplay:inline-block;\n"
           "\twidth:6rem;\n"
           "}\n"
           "</style>\n"
           "</head>\n"
           "<body>\n"
           "/debug/pprof/\n"
           "<br>\n"
           "<p>Set debug=1 as a query parameter to export "
           "in legacy text format</p>\n"
           "<br>\n"
           "Types of profiles available:\n"
           "<table>\n"
           "<thead><td>Count</td><td>Profile</td></thead>\n"),
        NULL);

    for (Int i = 0; i < n; i++) {
        const HppProfileEntry *profile = &profiles[i];
        Url link = {.path = profile->href, .raw_query = S_("debug=1")};
        (void)fmt_fprintf_v(bw, "<tr><td>%d</td><td><a href='%s'>%s</a></td></tr>\n",
                            profile->count, url_string(&link, a),
                            html_escape_string(a, profile->name));
    }

    (void)bytes_buffer_write_string(&b,
                                    S_("</table>\n"
                                       "<a href=\"goroutine?debug=2\">full goroutine "
                                       "stack dump</a>\n"
                                       "<br>\n"
                                       "<p>\n"
                                       "Profile Descriptions:\n"
                                       "<ul>\n"),
                                    NULL);
    for (Int i = 0; i < n; i++) {
        const HppProfileEntry *profile = &profiles[i];
        (void)fmt_fprintf_v(bw, "<li><div class=profile-name>%s: </div> %s</li>\n",
                            html_escape_string(a, profile->name),
                            html_escape_string(a, profile->desc));
    }
    (void)bytes_buffer_write_string(&b,
                                    S_("</ul>\n"
                                       "</p>\n"
                                       "</body>\n"
                                       "</html>"),
                                    NULL);

    Error err = BURROW_NO_ERROR;
    (void)w.vt->write(w.data, bytes_buffer_bytes(&b), &err);
    return err;
}

/* Index responds with the pprof-formatted profile named by the request. For
 * example, "/debug/pprof/heap" serves the "heap" profile. Index responds to a
 * request for "/debug/pprof/" with an HTML page listing the available
 * profiles. */
void http_pprof_index(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    bool found = false;
    Str name = strings_cut_prefix(r->url->path, S_("/debug/pprof/"), &found);
    if (found) {
        if (name.len != 0) {
            hpp_serve_named(name, w, r);
            return;
        }
    }

    hpp_set(w, S_("X-Content-Type-Options"), S_("nosniff"));
    hpp_set(w, S_("Content-Type"), S_("text/html; charset=utf-8"));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice all = pprof_profiles(a);
    static const Str others[] = {
        BURROW_S_INIT("cmdline"),
        BURROW_S_INIT("profile"),
        BURROW_S_INIT("symbol"),
        BURROW_S_INIT("trace"),
    };
    Int n = all.len + (Int)(sizeof others / sizeof others[0]);
    HppProfileEntry *profiles = (HppProfileEntry *)mem_alloc(
        a, (size_t)n * sizeof *profiles, _Alignof(HppProfileEntry));
    if (profiles == NULL) {
        /* Not in Go: the arena said no. */
        log_print_v(S_("net/http/pprof: out of memory"));
        arena_free(&ar);
        return;
    }
    Int k = 0;
    for (Int i = 0; i < all.len; i++) {
        PprofProfile *p = BURROW_AT(PprofProfile *, all, i);
        Str pn = pprof_profile_name(p);
        Str desc;
        (void)burrow__http_pprof_description(pn, &desc);
        profiles[k++] = (HppProfileEntry){pn, pn, desc, pprof_profile_count(p)};
    }

    /* Adding other profiles exposed from within this package */
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        Str desc;
        (void)burrow__http_pprof_description(others[i], &desc);
        profiles[k++] = (HppProfileEntry){others[i], others[i], desc, 0};
    }

    qsort(profiles, (size_t)n, sizeof *profiles, hpp_entry_compare);

    Error err =
        hpp_index_tmpl_execute(a, http_response_writer_as_io_writer(w), profiles, n);
    if (BURROW_FAILED(err))
        log_print_v(err);
    arena_free(&ar);
}

static void hpp_register(void *env) {
    (void)env;
    static const struct {
        Str get;
        void (*fn)(void *env, HttpResponseWriter w, HttpRequest *r);
    } routes[] = {
        {BURROW_S_INIT("GET /debug/pprof/"), http_pprof_index},
        {BURROW_S_INIT("GET /debug/pprof/cmdline"), http_pprof_cmdline},
        {BURROW_S_INIT("GET /debug/pprof/profile"), http_pprof_profile},
        {BURROW_S_INIT("GET /debug/pprof/symbol"), http_pprof_symbol},
        {BURROW_S_INIT("GET /debug/pprof/trace"), http_pprof_trace},
    };
    /* Go puts "GET " in front unless GODEBUG has httpmuxgo121=1. Here the
     * patterns have it, and it comes off for the old mux. */
    bool old = burrow__http_godebug_mux121();
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        Str pattern = routes[i].get;
        if (old)
            pattern = str_from_bytes(pattern.p + 4, pattern.len - 4);
        http_handle_func(pattern, BURROW_FN(HttpHandlerFunc, routes[i].fn, NULL));
    }
}

static SyncOnce hpp_register_once;

void http_pprof_register(void) {
    sync_once_do(&hpp_register_once, BURROW_FN(Func, hpp_register, NULL));
}
