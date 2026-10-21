/* crypto/tls: parseTestData, replayingConn and runTestAndUpdateIfNeeded of
 * handshake_test.go, which the client and server tests replay Go's recorded
 * handshakes with. Go can also record them again against OpenSSL with -update,
 * and that part, recordingConn and the OpenSSL processes, is not here: the
 * recordings come from Go's testdata as they are, through
 * tools/gen-tls-replay-tests.sh.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TLS_REPLAY_TEST_H
#define BURROW_TESTS_TLS_REPLAY_TEST_H

#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/testing/cryptotest.h"

#include "../src/crypto/tls_internal.h"

#include <stdint.h>
#include <string.h>

#ifndef BURROW_TESTS_TLS_TESTDATA_FILE
#define BURROW_TESTS_TLS_TESTDATA_FILE
/* A file in testdata: its name, and its lines without their newlines, ending
 * with NULL. */
typedef struct TlsTestdataFile {
    const char *name;
    const char *const *lines;
} TlsTestdataFile;
#endif

static inline Slice parse_test_data(Alloc *a, IoReader r, Error *err) {
    Slice flows = slice_nil(TYPE_BYTES);
    Slice current_flow = slice_nil(TYPE_BYTE);

    BufioScanner *scanner = bufio_new_scanner(a, r);
    if (scanner == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTES);
    }
    while (bufio_scanner_scan(scanner)) {
        Str line = bufio_scanner_text(scanner);
        /* If the line starts with ">>> " then it marks the beginning of a new
         * flow. */
        if (strings_has_prefix(line, BURROW_S(">>> "))) {
            if (current_flow.len > 0 || flows.len > 0) {
                flows = slice_append(a, flows, &current_flow, 1);
                current_flow = slice_nil(TYPE_BYTE);
            }
            continue;
        }

        /* Otherwise the line is a line of hex dump that looks like:
         * 00000170  fc f5 06 bf (...)  |.....X{&?......!|
         * (Some bytes have been omitted from the middle section.) */
        Str after;
        bool ok;
        (void)strings_cut(line, BURROW_S(" "), &after, &ok);
        if (!ok) {
            BURROW_OUT(err, errors_new(a, BURROW_S("invalid test data")));
            return slice_nil(TYPE_BYTES);
        }
        line = after;

        Str before = strings_cut(line, BURROW_S("|"), NULL, &ok);
        if (!ok) {
            BURROW_OUT(err, errors_new(a, BURROW_S("invalid test data")));
            return slice_nil(TYPE_BYTES);
        }
        line = before;

        Slice hex_bytes = strings_fields(a, line);
        const Str *hb = hex_bytes.p;
        for (Int i = 0; i < hex_bytes.len; i++) {
            Error perr = BURROW_NO_ERROR;
            uint64_t val = strconv_parse_uint(hb[i], 16, 8, &perr);
            if (BURROW_FAILED(perr)) {
                BURROW_OUT(err, fmt_errorf_v("invalid hex byte in test data: %s",
                                             error_text(perr)));
                return slice_nil(TYPE_BYTES);
            }
            Byte v = (Byte)val;
            current_flow = slice_append(a, current_flow, &v, 1);
        }
    }

    if (current_flow.len > 0)
        flows = slice_append(a, flows, &current_flow, 1);

    BURROW_OUT(err, BURROW_NO_ERROR);
    return flows;
}

/* Not in Go: os.Open of testdata/name and parseTestData of it, from the
 * files tools/gen-tls-replay-tests.sh put in the binary. */
static inline Slice load_testdata(Alloc *a, const TlsTestdataFile *files, size_t n,
                                  Str name, Error *err) {
    for (size_t i = 0; i < n; i++) {
        if (!str_eq(str_from_cstr(files[i].name), name))
            continue;
        StringsBuilder b = STRINGS_BUILDER(a);
        for (const char *const *l = files[i].lines; *l != NULL; l++) {
            (void)strings_builder_write_string(&b, str_from_cstr(*l), NULL);
            (void)strings_builder_write_byte(&b, '\n');
        }
        StringsReader sr;
        strings_reader_reset(&sr, strings_builder_string(&b));
        return parse_test_data(a, strings_reader_as_io_reader(&sr), err);
    }
    BURROW_OUT(err, fmt_errorf_v("open testdata/%s: no such file or directory", name));
    return slice_nil(TYPE_BYTES);
}

/* replayingConn is a net.Conn that replays flows recorded by recordingConn. */
typedef struct ReplayingConn {
    TestingT *t;
    SyncMutex mu;
    Slice flows;
    bool reading;
} ReplayingConn;

/* Not in Go: r.flows[0], with Go's panic when there is none. */
static inline Slice *replaying_conn_flow(ReplayingConn *r) {
    if (r->flows.len == 0)
        panic_str(BURROW_S("runtime error: index out of range [0] with length 0"));
    return (Slice *)r->flows.p;
}

static inline Int replaying_conn_read(void *self, Slice b, Error *err) {
    ReplayingConn *r = self;
    sync_mutex_lock(&r->mu);

    if (!r->reading) {
        testing_t_errorf_v(r->t, "expected write, got read");
        sync_mutex_unlock(&r->mu);
        BURROW_OUT(err, fmt_errorf_v("recording expected write, got read"));
        return 0;
    }

    Slice *flow = replaying_conn_flow(r);
    Int n = slice_copy(b, *flow);
    *flow = slice_sub(*flow, n, flow->len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (flow->len == 0) {
        r->flows = slice_sub(r->flows, 1, r->flows.len);
        if (r->flows.len == 0)
            BURROW_OUT(err, io_eof);
        else
            r->reading = false;
    }
    sync_mutex_unlock(&r->mu);
    return n;
}

static inline Int replaying_conn_write(void *self, Slice b, Error *err) {
    ReplayingConn *r = self;
    sync_mutex_lock(&r->mu);

    if (r->reading) {
        testing_t_errorf_v(r->t, "expected read, got write");
        sync_mutex_unlock(&r->mu);
        BURROW_OUT(err, fmt_errorf_v("recording expected read, got write"));
        return 0;
    }

    Slice *flow = replaying_conn_flow(r);
    if (!bytes_has_prefix(*flow, b)) {
        testing_t_errorf_v(r->t, "write mismatch: expected %x, got %x", *flow, b);
        sync_mutex_unlock(&r->mu);
        BURROW_OUT(err, fmt_errorf_v("write mismatch"));
        return 0;
    }
    *flow = slice_sub(*flow, b.len, flow->len);
    if (flow->len == 0) {
        r->flows = slice_sub(r->flows, 1, r->flows.len);
        r->reading = true;
    }
    sync_mutex_unlock(&r->mu);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b.len;
}

static inline Error replaying_conn_close(void *self) {
    ReplayingConn *r = self;
    sync_mutex_lock(&r->mu);

    Error err = BURROW_NO_ERROR;
    if (r->flows.len > 0) {
        testing_t_errorf_v(r->t, "closed with unfinished flows");
        err = fmt_errorf_v("unexpected close");
    }
    sync_mutex_unlock(&r->mu);
    return err;
}

static inline NetAddr replaying_conn_addr(void *self) {
    (void)self;
    NetAddr none = {NULL, NULL};
    return none;
}

static inline Error replaying_conn_set_deadline(void *self, Time t) {
    (void)self;
    (void)t;
    return BURROW_NO_ERROR;
}

static const Type replaying_conn_type = {
    {(const Byte *)"replayingConn", 13},
    {(const Byte *)"tls_test", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(ReplayingConn),
    (uint16_t)_Alignof(ReplayingConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72706c63U, /* "rplc" */
    NULL,
};

static const NetConnVT replaying_conn_vt = {
    {&replaying_conn_type, replaying_conn_read},
    {&replaying_conn_type, replaying_conn_write},
    {&replaying_conn_type, replaying_conn_close},
    replaying_conn_addr,
    replaying_conn_addr,
    replaying_conn_set_deadline,
    replaying_conn_set_deadline,
    replaying_conn_set_deadline,
};

static inline NetConn replaying_conn_as_net_conn(ReplayingConn *r) {
    return (NetConn){&replaying_conn_vt, r};
}

/* Not in Go: the func(ConnectionState) error that validates a connection in a
 * clientTest or serverTest. */
BURROW_FUNC(ConnectionStateCheck, Error, const TlsConnectionState *cs);

static inline Error check_cipher_suite_fn(void *env, const TlsConnectionState *state) {
    uint16_t want = *(const uint16_t *)env;
    if (state->cipher_suite != want)
        return fmt_errorf_v("got cipher suite %x, want %x", state->cipher_suite, want);
    return BURROW_NO_ERROR;
}

/* checkCipherSuite of handshake_server_test.go. Not in Go: want is kept in a. */
static inline ConnectionStateCheck check_cipher_suite(Alloc *a, uint16_t want) {
    uint16_t *w = mem_alloc(a, sizeof *w, _Alignof(uint16_t));
    if (w == NULL)
        panic_str(BURROW_S("out of memory"));
    *w = want;
    return BURROW_FN(ConnectionStateCheck, check_cipher_suite_fn, w);
}

static inline Error check_curve_id_fn(void *env, const TlsConnectionState *state) {
    TlsCurveID want = *(const TlsCurveID *)env;
    if (state->curve_id != want)
        return fmt_errorf_v("got curve %d, want %d", state->curve_id, want);
    return BURROW_NO_ERROR;
}

/* checkCurveID of handshake_server_test.go. Not in Go: want is kept in a. */
static inline ConnectionStateCheck check_curve_id(Alloc *a, TlsCurveID want) {
    TlsCurveID *w = mem_alloc(a, sizeof *w, _Alignof(TlsCurveID));
    if (w == NULL)
        panic_str(BURROW_S("out of memory"));
    *w = want;
    return BURROW_FN(ConnectionStateCheck, check_curve_id_fn, w);
}

/* Not in Go: what a test runs, the run method of a clientTest or serverTest
 * with update false, since there is no -update. */
BURROW_FUNC(ReplayRunFunc, void, TestingT *t);

/* Not in Go: what runTestAndUpdateIfNeeded's subtest closes over. */
typedef struct ReplayRun {
    ReplayRunFunc run;
} ReplayRun;

static inline void replay_run(void *env, TestingT *t) {
    ReplayRun *rr = env;
    cryptotest_set_global_random(t, 0);
    /* Not in Go: Go's TestMain turns off hasAESGCMHardwareSupport for the
     * whole binary, so that the cipher suite order is the same everywhere. A
     * burrow test binary has no TestMain, so each replay does it. */
    burrow__tls_aes_gcm_hardware_override = 0;
    rr->run.f(rr->run.env, t);
    burrow__tls_aes_gcm_hardware_override = -1;
}

static inline void run_test_and_update_if_needed(TestingT *t, Str name,
                                                 ReplayRunFunc run) {
    ReplayRun rr = {run};
    (void)testing_t_run(t, name, BURROW_FN(TestingTFunc, replay_run, &rr));
}

#endif /* BURROW_TESTS_TLS_REPLAY_TEST_H */
