/* Not in Go: Go doesn't run h2spec in its tests. This runs h2spec,
 * github.com/summerwind/h2spec, the HTTP/2 conformance tests for a server,
 * against net/http's HTTP/2 server.
 *
 * TestH2spec fetches the module, as TestACVP and TestBogoSuite fetch theirs,
 * builds its h2spec command, and points it at an httptest server, once with
 * HTTP/2 over TCP with prior knowledge and once over TLS, where the client
 * asks for "h2" by ALPN. The handler answers GET / and POST / with a 200 and
 * a body, which is what h2spec's README asks of a server. h2spec exits with
 * status 1 when a test case fails, and its output ends with a count of the
 * test cases. It needs the go command on $PATH.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fetchmodule.h"

#include "burrow/burrow.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"

static const Str h2spec_module = BURROW_S_INIT("github.com/summerwind/h2spec");
/* The last commit on master, which is newer than the last tag. The tags
 * after v2.2.1 have a go.mod without the /v2 the module path would need, so
 * the module proxy has none of them. */
static const Str h2spec_version = BURROW_S_INIT("v1.5.1-0.20220625142712-af83a65f0b62");

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define H2SPEC_HAVE_TCP 1
#endif

static void h2spec_free_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

static void h2spec_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    static const char hello[] = "Hello, world!\n";
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(r->body),
                  &err);
    (void)http_response_writer_write(w,
                                     slice_from((void *)(uintptr_t)hello,
                                                (Int)sizeof hello - 1,
                                                (Int)sizeof hello - 1, TYPE_BYTE),
                                     NULL);
}

typedef struct H2specRun {
    Str h2spec; /* the h2spec command */
    bool tls;
} H2specRun;

/* Not in Go: one run of h2spec against a new server. */
static void run_h2spec(void *env, TestingT *t) {
    H2specRun *r = env;
#if !defined(H2SPEC_HAVE_TCP)
    (void)r;
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, h2spec_handler, NULL);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&hf));
    if (ts == NULL) {
        testing_t_errorf_v(t, "no memory for the server");
        arena_free(&ar);
        return;
    }
    HttpProtocols protos = {0};
    if (r->tls) {
        ts->enable_http2 = true;
        httptest_server_start_tls(ts);
    } else {
        http_protocols_set_http1(&protos, true);
        http_protocols_set_unencrypted_http2(&protos, true);
        ts->config.protocols = &protos;
        httptest_server_start(ts);
    }

    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, ts->url, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to parse the server's URL %q: %s", ts->url, err);
        httptest_server_free(ts);
        arena_free(&ar);
        return;
    }
    Slice args = slice_nil(TYPE_STRING);
    args = BURROW_APPEND(Str, a, args, BURROW_S("-h"));
    args = BURROW_APPEND(Str, a, args, url_hostname(u));
    args = BURROW_APPEND(Str, a, args, BURROW_S("-p"));
    args = BURROW_APPEND(Str, a, args, url_port(u));
    /* Not in Go: 5 seconds for each test case rather than h2spec's 2, for a
     * slow machine or a sanitizer build. */
    args = BURROW_APPEND(Str, a, args, BURROW_S("-o"));
    args = BURROW_APPEND(Str, a, args, BURROW_S("5"));
    if (r->tls) {
        args = BURROW_APPEND(Str, a, args, BURROW_S("-t"));
        args = BURROW_APPEND(Str, a, args, BURROW_S("-k"));
    }
    ExecCmd *cmd = exec_command(a, r->h2spec, args);
    cmd->env = clean_cmd_env(a);
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    httptest_server_close(ts);
    httptest_server_free(ts);

    Str outs = fetchmodule_str(out);
    Str summary = BURROW_S("");
    /* The count, the line reporter.Summary prints last with the format
     * "%d tests, %d passed, %d skipped, %d failed". */
    Int i = strings_last_index(outs, BURROW_S(" tests, "));
    if (i >= 0) {
        Int start = strings_last_index(str_from_bytes(outs.p, i), BURROW_S("\n")) + 1;
        Str rest = str_from_bytes(outs.p + start, outs.len - start);
        summary = strings_cut(rest, BURROW_S("\n"), NULL, NULL);
    }
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "h2spec failed: %s\n%s", err, outs);
    } else if (summary.len == 0) {
        testing_t_errorf_v(t, "h2spec printed no count of its test cases:\n%s", outs);
    } else {
        testing_t_logf_v(t, "%s", summary);
    }
    arena_free(&ar);
#endif
}

static void TestH2spec(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
    }

    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, h2spec_free_arena, ar));
    Alloc *a = arena_allocator(ar);

    Str dir = fetch_module(t, a, h2spec_module, h2spec_version);
    Str h2spec = filepath_join_v(a, 2, fetchmodule_temp_dir(t, a), BURROW_S("h2spec"));
    ExecCmd *cmd = exec_command_v(a, go_tool_path(t, a), 4, BURROW_S("build"),
                                  BURROW_S("-o"), h2spec, BURROW_S("./cmd/h2spec"));
    cmd->dir = dir;
    cmd->env = clean_cmd_env(a);
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to build h2spec: %s\n%s", err,
                           fetchmodule_str(out));
    }

    H2specRun *h2c = BURROW_NEW(a, H2specRun);
    h2c->h2spec = h2spec;
    testing_t_run(t, BURROW_S("h2c"), BURROW_FN(TestingTFunc, run_h2spec, h2c));
    H2specRun *tls = BURROW_NEW(a, H2specRun);
    tls->h2spec = h2spec;
    tls->tls = true;
    testing_t_run(t, BURROW_S("tls"), BURROW_FN(TestingTFunc, run_h2spec, tls));
}

#define TESTS(X) X(TestH2spec)
TESTING_MAIN(TESTS)
