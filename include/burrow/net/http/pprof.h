/* net/http/pprof, the runtime's profiles served over HTTP in the form
 * go tool pprof reads.
 *
 * Go registers its handlers on http.DefaultServeMux when the package is
 * imported. C has nothing that runs on import, so http_pprof_register does it,
 * and a program that already serves the default mux has them under
 * /debug/pprof/ from then on:
 *
 *     http_pprof_register();
 *     http_listen_and_serve(BURROW_S("localhost:6060"), (HttpHandler){0});
 *
 * Then, from another terminal:
 *
 *     go tool pprof http://localhost:6060/debug/pprof/heap
 *     go tool pprof http://localhost:6060/debug/pprof/profile?seconds=30
 *
 * The block and mutex profiles are empty until the program turns them on with
 * runtime_set_block_profile_rate and runtime_set_mutex_profile_fraction.
 * /debug/pprof/ itself is an HTML page that lists every profile.
 *
 * The handlers take the query parameters Go's do: debug=N for the text form of
 * a profile, seconds=N for how long the CPU profile runs, and seconds=N on
 * allocs, block, goroutine, heap, mutex and threadcreate for a delta profile,
 * what changed over that many seconds. gc=N on the heap profile is accepted
 * and does nothing, as burrow has no collector to run.
 *
 * A program that does not serve the default mux can put the handlers on its
 * own. Each one is a function an HttpHandlerFunc can point at, and ignores its
 * env:
 *
 *     http_serve_mux_handle_func(mux, BURROW_S("GET /debug/pprof/"),
 *                                BURROW_FN(HttpHandlerFunc, http_pprof_index, NULL));
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/pprof */

#ifndef BURROW_NET_HTTP_PPROF_H
#define BURROW_NET_HTTP_PPROF_H

#include "burrow/core.h"
#include "burrow/net/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cmdline responds with the running program's command line, with arguments
 * separated by NUL bytes. http_pprof_register registers it as
 * /debug/pprof/cmdline. */
void http_pprof_cmdline(void *env, HttpResponseWriter w, HttpRequest *r);

/* Profile responds with the pprof-formatted cpu profile. Profiling lasts for
 * duration specified in seconds GET parameter, or for 30 seconds if not
 * specified. http_pprof_register registers it as /debug/pprof/profile. */
void http_pprof_profile(void *env, HttpResponseWriter w, HttpRequest *r);

/* Trace responds with the execution trace in binary form, for a duration given
 * by the seconds GET parameter, 1 second if there is none. burrow has no
 * runtime/trace yet, so for now this answers 500 with "Could not enable
 * tracing", as Go does when trace.Start fails. http_pprof_register registers it
 * as /debug/pprof/trace. */
void http_pprof_trace(void *env, HttpResponseWriter w, HttpRequest *r);

/* Symbol looks up the program counters listed in the request, responding
 * with a table mapping program counters to function names.
 * http_pprof_register registers it as /debug/pprof/symbol. A counter in a
 * static function or outside burrow gets no line, since runtime_func_for_pc
 * cannot name it. */
void http_pprof_symbol(void *env, HttpResponseWriter w, HttpRequest *r);

/* Index responds with the pprof-formatted profile named by the request. For
 * example, "/debug/pprof/heap" serves the "heap" profile. Index responds to a
 * request for "/debug/pprof/" with an HTML page listing the available
 * profiles. */
void http_pprof_index(void *env, HttpResponseWriter w, HttpRequest *r);

/* Handler returns an HTTP handler that serves the named profile. Available
 * profiles can be found in pprof_profiles. Made in a, with a copy of name,
 * and NULL data when a says no. */
BURROW_OWNS(ret) HttpHandler http_pprof_handler(Alloc *a, Str name);

/* What importing the package does in Go: puts http_pprof_index,
 * http_pprof_cmdline, http_pprof_profile, http_pprof_symbol and
 * http_pprof_trace on http_default_serve_mux under /debug/pprof/, each with
 * GET in front of its pattern unless GODEBUG has httpmuxgo121=1. Only the
 * first call does anything. */
void http_pprof_register(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_PPROF_H */
