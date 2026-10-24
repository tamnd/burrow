/* Tests for net/http/pprof.
 *
 * Go's pprof_test.go, test for test and in the same order. The functions the
 * tests look for in a profile are static here, and a static function has no
 * name in one, so seen tells them apart by address as well. Go's
 * TestDeltaProfileEmptyBase runs testdata/delta_mutex.go with go run, and here
 * it runs this program again with BURROW_PPROF_DELTA_MUTEX set, which does
 * what that file does.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/debug/pprof_profile.h"
#include "../src/net/http_internal.h"
#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mprof.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/pprof.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/runtime.h"
#include "burrow/runtime/pprof.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

/* TestDescriptions checks that the profile names under runtime/pprof package
 * have a key in the description map. */
static void TestDescriptions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice ps = pprof_profiles(arena_allocator(&ar));
    for (Int i = 0; i < ps.len; i++) {
        PprofProfile *p = BURROW_AT(PprofProfile *, ps, i);
        Str desc;
        bool ok = burrow__http_pprof_description(pprof_profile_name(p), &desc);
        if (ok != true)
            testing_t_errorf_v(t, "%s does not exist in profileDescriptions map\n",
                               pprof_profile_name(p));
    }
    arena_free(&ar);
}

typedef struct HandlerCase {
    Str path;
    void (*handler)(void *env, HttpResponseWriter w, HttpRequest *r);
    Int status_code;
    Str content_type;
    Str content_disposition;
    Str resp;
} HandlerCase;

static const HandlerCase handler_cases[] = {
    {BURROW_S_INIT("/debug/pprof/<script>scripty<script>"), http_pprof_index,
     HTTP_STATUS_NOT_FOUND, BURROW_S_INIT("text/plain; charset=utf-8"),
     BURROW_S_INIT(""), BURROW_S_INIT("Unknown profile\n")},
    {BURROW_S_INIT("/debug/pprof/heap"), http_pprof_index, HTTP_STATUS_OK,
     BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"heap\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/heap?debug=1"), http_pprof_index, HTTP_STATUS_OK,
     BURROW_S_INIT("text/plain; charset=utf-8"), BURROW_S_INIT(""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/cmdline"), http_pprof_cmdline, HTTP_STATUS_OK,
     BURROW_S_INIT("text/plain; charset=utf-8"), BURROW_S_INIT(""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/profile?seconds=1"), http_pprof_profile,
     HTTP_STATUS_OK, BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"profile\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/symbol"), http_pprof_symbol, HTTP_STATUS_OK,
     BURROW_S_INIT("text/plain; charset=utf-8"), BURROW_S_INIT(""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/trace"), http_pprof_trace, HTTP_STATUS_OK,
     BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"trace\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/mutex"), http_pprof_index, HTTP_STATUS_OK,
     BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"mutex\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/block?seconds=1"), http_pprof_index, HTTP_STATUS_OK,
     BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"block-delta\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/goroutine?seconds=1"), http_pprof_index,
     HTTP_STATUS_OK, BURROW_S_INIT("application/octet-stream"),
     BURROW_S_INIT("attachment; filename=\"goroutine-delta\""), BURROW_S_INIT("")},
    {BURROW_S_INIT("/debug/pprof/"), http_pprof_index, HTTP_STATUS_OK,
     BURROW_S_INIT("text/html; charset=utf-8"), BURROW_S_INIT(""),
     BURROW_S_INIT("Types of profiles available:")},
};

static void handlers_one(void *env, TestingT *t) {
    const HandlerCase *tc = (const HandlerCase *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HttpRequest *req = httptest_new_request(
        a, S("GET"), fmt_sprintf_v(a, "http://example.com%s", tc->path), (IoReader){0});
    HttptestResponseRecorder *w = httptest_new_recorder(a);
    if (req == NULL || w == NULL) {
        testing_t_fatal_v(t, "out of memory");
    }
    tc->handler(NULL, httptest_response_recorder_as_response_writer(w), req);

    HttpResponse *resp = httptest_response_recorder_result(w);
    if (resp->status_code != tc->status_code)
        testing_t_errorf_v(t, "status code: got %d; want %d", resp->status_code,
                           tc->status_code);

    Error err = BURROW_NO_ERROR;
    Slice body = io_read_all(a, io_read_closer_as_io_reader(resp->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "when reading response body, expected non-nil err; got %v", err);
    Str got = http_header_get(resp->header, S("X-Content-Type-Options"));
    if (!str_eq(got, S("nosniff")))
        testing_t_errorf_v(t, "X-Content-Type-Options: got %q; want %q", got,
                           S("nosniff"));
    got = http_header_get(resp->header, S("Content-Type"));
    if (!str_eq(got, tc->content_type))
        testing_t_errorf_v(t, "Content-Type: got %q; want %q", got, tc->content_type);
    got = http_header_get(resp->header, S("Content-Disposition"));
    if (!str_eq(got, tc->content_disposition))
        testing_t_errorf_v(t, "Content-Disposition: got %q; want %q", got,
                           tc->content_disposition);

    if (resp->status_code == HTTP_STATUS_OK)
        goto out;
    got = http_header_get(resp->header, S("X-Go-Pprof"));
    if (!str_eq(got, S("1")))
        testing_t_errorf_v(t, "X-Go-Pprof: got %q; want %q", got, S("1"));
    if (!str_eq(str_from_bytes(body.p, body.len), tc->resp))
        testing_t_errorf_v(t, "response: got %q; want %q", body, tc->resp);
out:
    httptest_response_recorder_free(w);
    http_request_free(req);
    arena_free(&ar);
}

static void TestHandlers(TestingT *t) {
    for (size_t i = 0; i < sizeof handler_cases / sizeof handler_cases[0]; i++)
        (void)testing_t_run(t, handler_cases[i].path,
                            BURROW_FN(TestingTFunc, handlers_one,
                                      (void *)(uintptr_t)&handler_cases[i]));
}

static uint32_t sink;

static BURROW_NOINLINE void mutex_hog1(SyncMutex *mu1, SyncMutex *mu2, Time start,
                                       Duration dt) {
    (void)sync_atomic_add_uint32(&sink, 1);
    while (time_since(start) < dt) {
        /* When using gccgo the loop of mutex operations is not preemptible.
         * This can cause the loop to block a GC, causing the time limits in
         * TestDeltaContentionz to fail. Since this loop is not very realistic,
         * when using gccgo add preemption points 100 times a second. */
        Time t1 = time_now();
        while (time_since(start) < dt && time_since(t1) < 10 * TIME_MILLISECOND) {
            sync_mutex_lock(mu1);
            sync_mutex_lock(mu2);
            sync_mutex_unlock(mu1);
            sync_mutex_unlock(mu2);
        }
        /* Not in Go: burrow's goroutines do not get preempted either, so this
         * is gccgo's branch every time. */
        runtime_gosched();
    }
}

/* mutexHog2 is almost identical to mutexHog but we keep them separate in order
 * to distinguish them with function names in the stack trace. We make them
 * slightly different, using Sink, because otherwise gccgo -c opt will merge
 * them. */
static BURROW_NOINLINE void mutex_hog2(SyncMutex *mu1, SyncMutex *mu2, Time start,
                                       Duration dt) {
    (void)sync_atomic_add_uint32(&sink, 2);
    while (time_since(start) < dt) {
        /* See comment in mutexHog. */
        Time t1 = time_now();
        while (time_since(start) < dt && time_since(t1) < 10 * TIME_MILLISECOND) {
            sync_mutex_lock(mu1);
            sync_mutex_lock(mu2);
            sync_mutex_unlock(mu1);
            sync_mutex_unlock(mu2);
        }
        runtime_gosched();
    }
}

typedef void (*Hogger)(SyncMutex *mu1, SyncMutex *mu2, Time start, Duration dt);

typedef struct HogRun {
    Hogger hogger;
    SyncMutex mu1, mu2;
    Time start;
    Duration duration;
    SyncWaitGroup wg;
} HogRun;

static void mutex_hog_worker(void *env) {
    HogRun *h = (HogRun *)env;
    h->hogger(&h->mu1, &h->mu2, h->start, h->duration);
    sync_wait_group_done(&h->wg);
}

/* mutexHog starts multiple goroutines that runs the given hogger function for
 * the specified duration. The hogger function will be given two mutexes to
 * lock & unlock. */
static void mutex_hog(Duration duration, Hogger hogger) {
    HogRun h;
    memset(&h, 0, sizeof h);
    h.hogger = hogger;
    h.start = time_now();
    h.duration = duration;
    sync_wait_group_add(&h.wg, 10);
    for (int i = 0; i < 10; i++)
        if (!go(BURROW_FN(Func, mutex_hog_worker, &h)))
            sync_wait_group_done(&h.wg);
    sync_wait_group_wait(&h.wg);
}

/* Go's srv is a package variable that is never closed. Not in Go: here it is
 * started by the first query and closed when TestDeltaProfile, the test that
 * queries it, is done, along with the default client's connections to it, so
 * that the leak checker finds nothing left open at exit. */
static HttptestServer *srv;

static void srv_close(void *env) {
    (void)env;
    http_transport_close_idle_connections(http_default_transport);
    httptest_server_free(srv);
    srv = NULL;
}

static PfProfile *query(Alloc *a, Str endpoint, Error *err) {
    if (srv == NULL) {
        /* Not in Go: importing net/http/pprof is what puts the handlers on the
         * default mux there. */
        http_pprof_register();
        srv = httptest_new_server(NULL, (HttpHandler){0});
        if (srv == NULL) {
            *err = fmt_errorf_v("cannot start the test server");
            return NULL;
        }
    }
    Str url = fmt_sprintf_v(a, "%s%s", srv->url, endpoint);
    HttpResponse *r = http_get(url, err);
    if (BURROW_FAILED(*err)) {
        *err = fmt_errorf_v("failed to fetch %q: %v", url, *err);
        return NULL;
    }
    if (r->status_code != HTTP_STATUS_OK) {
        *err = fmt_errorf_v("failed to fetch %q: %v", url, r->status);
        http_response_free(r);
        return NULL;
    }

    Slice b = io_read_all(a, io_read_closer_as_io_reader(r->body), err);
    http_response_free(r);
    if (BURROW_FAILED(*err)) {
        *err =
            fmt_errorf_v("failed to read and parse the result from %q: %v", url, *err);
        return NULL;
    }
    StringsReader rd;
    strings_reader_reset(&rd, str_from_bytes(b.p, b.len));
    return burrow__pf_parse(a, strings_reader_as_io_reader(&rd), err);
}

/* Not in Go: which of the two hoggers addr is in, 1 or 2, or 0 for neither.
 * They have no names to look for, so it is the one of them and
 * mutex_hog_worker, which is in every one of their stacks, that starts nearest
 * below addr. */
static int hog_of(Uintptr addr) {
    Uintptr starts[] = {(Uintptr)mutex_hog1, (Uintptr)mutex_hog2,
                        (Uintptr)mutex_hog_worker};
    int which = 0;
    Uintptr best = 0;
    for (int i = 0; i < 3; i++) {
        if (addr >= starts[i] && starts[i] >= best && addr - starts[i] < 0x10000) {
            best = starts[i];
            which = i + 1;
        }
    }
    return which == 3 ? 0 : which;
}

/* seen returns true if the profile includes samples whose stacks include the
 * specified function name (fname). Not in Go: a location with no names in it
 * counts when hog_of puts it in hogger number which. */
static bool seen(PfProfile *p, const char *fname, int which) {
    Str name = str_from_cstr(fname);
    PfSample **samples = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        PfLocation **sl = (PfLocation **)samples[i]->location.p;
        for (Int j = 0; j < samples[i]->location.len; j++) {
            const PfLocation *loc = sl[j];
            bool named = false;
            const PfLine *lines = (const PfLine *)loc->line.p;
            for (Int k = 0; k < loc->line.len; k++) {
                if (lines[k].function == NULL || lines[k].function->name.len == 0)
                    continue;
                named = true;
                if (strings_contains(lines[k].function->name, name))
                    return true;
            }
            if (!named && hog_of((Uintptr)loc->address) == which)
                return true;
        }
    }
    return false;
}

static Chan *hog_done;

static void hog2_loop(void *env) {
    (void)env;
    for (;;) {
        mutex_hog(20 * TIME_MILLISECOND, mutex_hog2);
        bool v = false;
        SelectCase cases[] = {BURROW_RECV(hog_done, &v), BURROW_DEFAULT};
        if (chan_select(cases, 2) == 0) {
            v = true;
            chan_send(hog_done, &v);
            return;
        }
        time_sleep(10 * TIME_MILLISECOND);
    }
}

/* Not in Go: an arena that is freed when the test ends, however it ends, which
 * the garbage collector does for Go. A skip or a fatal ends the test where it
 * is, so freeing it at the end of the test function would not be enough. */
static void free_test_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

static Alloc *test_arena(TestingT *t) {
    Arena *ar = mem_alloc(heap_allocator(), sizeof *ar, _Alignof(Arena));
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, free_test_arena, ar));
    return arena_allocator(ar);
}

/* Not in Go: the rate that TestDeltaProfile's deferred function puts back. */
static Int saved_rate;

static void restore_rate(void *env) {
    (void)env;
    (void)runtime_set_mutex_profile_fraction(saved_rate);
}

static void TestDeltaProfile(TestingT *t) {
#if defined(BURROW_ARCH_ARM) || defined(BURROW_ARCH_ARM64)
    /* testenv.SkipFlaky(t, 50218). Not in Go: there is no -flaky flag to run
     * it anyway. */
    testing_t_skip_v(t, "skipping known flaky test without the -flaky flag; see "
                        "golang.org/issue/50218");
#endif
#if !defined(HAVE_TCP)
    /* Not in Go: the test server needs TCP, which here needs the readiness poll
     * FD. */
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
    return;
#endif
    testing_t_cleanup(t, BURROW_FN(Func, srv_close, NULL));
    /* Not in Go: with no stack walk every sample has an empty stack, which
     * Go's test finds out below and skips for. */
    saved_rate = runtime_set_mutex_profile_fraction(1);
    testing_t_cleanup(t, BURROW_FN(Func, restore_rate, NULL));

    /* mutexHog1 will appear in non-delta mutex profile if the mutex profile
     * works. */
    mutex_hog(20 * TIME_MILLISECOND, mutex_hog1);

    /* If mutexHog1 does not appear in the mutex profile, skip this test. Mutex
     * profile is likely not working, so is the delta profile. */
    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = query(a, S("/debug/pprof/mutex"), &err);
    if (BURROW_FAILED(err))
        testing_t_skipf_v(t, "mutex profile is unsupported: %v", err);

    if (!seen(p, "mutex_hog1", 1))
        testing_t_skipf_v(t, "mutex profile is not working: %v",
                          burrow__pf_profile_string(p, a));

    /* causes mutexHog2 call stacks to appear in the mutex profile. */
    hog_done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (hog_done == NULL || !go(BURROW_FN(Func, hog2_loop, NULL))) {
        testing_t_fatal_v(t, "out of memory");
    }

    static const int ds[] = {1, 4, 16, 32};
    for (size_t i = 0; i < sizeof ds / sizeof ds[0]; i++) {
        int d = ds[i];
        Str endpoint = fmt_sprintf_v(a, "/debug/pprof/mutex?seconds=%d", d);
        p = query(a, endpoint, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed to query %q: %v", endpoint, err);
            goto cleanup;
        }
        if (!seen(p, "mutex_hog1", 1) && seen(p, "mutex_hog2", 2) &&
            p->duration_nanos > 0)
            break; /* pass */
        if (d == 32)
            testing_t_errorf_v(t,
                               "want mutexHog2 but no mutexHog1 in the profile, and "
                               "non-zero p.DurationNanos, got %v",
                               burrow__pf_profile_string(p, a));
    }
    p = query(a, S("/debug/pprof/mutex"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to query mutex profile: %v", err);
        goto cleanup;
    }
    if (!seen(p, "mutex_hog1", 1) || !seen(p, "mutex_hog2", 2))
        testing_t_errorf_v(t,
                           "want both mutexHog1 and mutexHog2 in the profile, got %v",
                           burrow__pf_profile_string(p, a));
cleanup:; /* cleanup the above goroutine. */
    bool v = true;
    chan_send(hog_done, &v);
    (void)chan_recv(hog_done, &v); /* wait for the goroutine to exit. */
    chan_free(hog_done);
    hog_done = NULL;
}

/* What Go's testdata/delta_mutex.go does: collects a 1s delta mutex profile
 * and dumps it to stdout, in a process of its own so that the base mutex
 * profile is empty. */
static void delta_mutex_main(void) {
    /* Disable the mutex profiler. This is the default, but that default is
     * load-bearing for this test, which needs the base profile to be empty. */
    (void)runtime_set_mutex_profile_fraction(0);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HttpHandler h = http_pprof_handler(a, S("mutex"));

    HttpRequest *req = httptest_new_request(
        a, S("GET"), S("/debug/pprof/mutex?seconds=1"), (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    if (h.data == NULL || req == NULL || rec == NULL)
        log_fatalf_v("out of memory");

    http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec), req);
    HttpResponse *resp = httptest_response_recorder_result(rec);
    if (resp->status_code != HTTP_STATUS_OK)
        log_fatalf_v("Request failed: %s\n%s", resp->status,
                     bytes_buffer_string(rec->body, a));

    (void)os_file_write(os_stdout, bytes_buffer_bytes(rec->body), NULL);
    os_exit(0);
}

/* TestDeltaProfileEmptyBase validates that we still receive a valid delta
 * profile even if the base contains no samples.
 *
 * Regression test for https://go.dev/issue/64566. */
static void TestDeltaProfileEmptyBase(TestingT *t) {
    Alloc *a = test_arena(t);
    if (os_getenv(a, S("BURROW_PPROF_DELTA_MUTEX")).len > 0)
        delta_mutex_main();

    if (testing_short()) {
        /* Delta profile collection has a 1s minimum. */
        testing_t_skip_v(t, "skipping in -short mode");
    }

    SKIP_WITHOUT_EXEC(t);

    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "error finding go tool: %v", err);
    }
    ExecCmd *cmd =
        exec_command_v(a, exe, 1, S("-test.run=^TestDeltaProfileEmptyBase$"));
    if (cmd == NULL) {
        testing_t_fatal_v(t, "out of memory");
    }
    cmd->env = BURROW_APPEND(Str, a, os_environ(a), S("BURROW_PPROF_DELTA_MUTEX=1"));
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "error running profile collection: %v\noutput: %s", err,
                           str_from_bytes(out.p, out.len));
    }

    /* Log the binary output for debugging failures. */
    testing_t_logf_v(t, "Output in base64.StdEncoding: %s",
                     base64_encoding_encode_to_string(base64_std_encoding, a, out));

    StringsReader rd;
    strings_reader_reset(&rd, str_from_bytes(out.p, out.len));
    PfProfile *p = burrow__pf_parse(a, strings_reader_as_io_reader(&rd), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Parse got err %v want nil", err);
    }

    testing_t_logf_v(t, "Output as parsed Profile: %s",
                     burrow__pf_profile_string(p, a));

    PfValueType **st = (PfValueType **)p->sample_type.p;
    if (p->sample_type.len != 2) {
        testing_t_errorf_v(t, "len(p.SampleType) got %d want 2", p->sample_type.len);
        /* Not in Go: Go goes on to index it and panics if it is short. */
        return;
    }
    if (!str_eq(st[0]->type, S("contentions")))
        testing_t_errorf_v(t, "p.SampleType[0].Type got %q want \"contentions\"",
                           st[0]->type);
    if (!str_eq(st[0]->unit, S("count")))
        testing_t_errorf_v(t, "p.SampleType[0].Unit got %q want \"count\"",
                           st[0]->unit);
    if (!str_eq(st[1]->type, S("delay")))
        testing_t_errorf_v(t, "p.SampleType[1].Type got %q want \"delay\"",
                           st[1]->type);
    if (!str_eq(st[1]->unit, S("nanoseconds")))
        testing_t_errorf_v(t, "p.SampleType[1].Unit got %q want \"nanoseconds\"",
                           st[1]->unit);

    if (p->period_type == NULL) {
        testing_t_fatal_v(t, "p.PeriodType got nil want not nil");
    }
    if (!str_eq(p->period_type->type, S("contentions")))
        testing_t_errorf_v(t, "p.PeriodType.Type got %q want \"contentions\"",
                           p->period_type->type);
    if (!str_eq(p->period_type->unit, S("count")))
        testing_t_errorf_v(t, "p.PeriodType.Unit got %q want \"count\"",
                           p->period_type->unit);
}

#define TESTS(X)                                                                       \
    X(TestDescriptions)                                                                \
    X(TestHandlers)                                                                    \
    X(TestDeltaProfile)                                                                \
    X(TestDeltaProfileEmptyBase)

TESTING_MAIN(TESTS)
