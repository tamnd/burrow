/* Derived from Go's src/crypto/tls/handshake_client_test.go.
 * Go source: go1.27.1.
 *
 * Most of the file runs the client against recorded server flights or a real
 * server, which are not here yet. These are the tests that need neither.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/pem.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_test_gen.h"

#include <string.h>

static TlsClientSessionState *cache_get(TlsClientSessionCache cache, Str key,
                                        bool *ok) {
    return cache.vt->get(cache.data, key, ok);
}

static void cache_put(TlsClientSessionCache cache, Str key, TlsClientSessionState *cs) {
    cache.vt->put(cache.data, key, cs);
}

static void TestLRUClientSessionCache(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Initialize cache of capacity 4. */
    TlsClientSessionCache cache = tls_new_lru_client_session_cache(a, 4);
    if (cache.vt == NULL)
        testing_t_fatal_v(t, "NewLRUClientSessionCache: out of memory");
    TlsClientSessionState cs[6] = {{0}};
    const Str keys[] = {BURROW_S("0"), BURROW_S("1"), BURROW_S("2"), BURROW_S("3"),
                        BURROW_S("4"), BURROW_S("5"), BURROW_S("6")};
    TlsClientSessionState *s;
    bool ok;

    /* Add 4 entries to the cache and look them up. */
    for (int i = 0; i < 4; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 4; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache failed lookup for added key: %s",
                               keys[i]);
    }

    /* Add 2 more entries to the cache. First 2 should be evicted. */
    for (int i = 4; i < 6; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 2; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (ok || s != NULL)
            testing_t_fatalf_v(t, "session cache should have evicted key: %s", keys[i]);
    }

    /* Touch entry 2. LRU should evict 3 next. */
    cache_get(cache, keys[2], &ok);
    cache_put(cache, keys[0], &cs[0]);
    s = cache_get(cache, keys[3], &ok);
    if (ok || s != NULL)
        testing_t_fatalf_v(t, "session cache should have evicted key 3");

    /* Update entry 0 in place. */
    cache_put(cache, keys[0], &cs[3]);
    s = cache_get(cache, keys[0], &ok);
    if (!ok || s != &cs[3])
        testing_t_fatalf_v(t, "session cache failed update for key 0");

    /* Calling Put with a nil entry deletes the key. */
    cache_put(cache, keys[0], NULL);
    cache_get(cache, keys[0], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 0");

    /* Delete entry 2. LRU should keep 4 and 5 */
    cache_put(cache, keys[2], NULL);
    cache_get(cache, keys[2], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 4");
    for (int i = 4; i < 6; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache should not have deleted key: %s",
                               keys[i]);
    }
    arena_free(&ar);
}

/* Not in Go: a Client handshake run on its own goroutine. */
typedef struct ClientJob {
    TlsConn *c;
    Error err;
} ClientJob;

static void client_handshake_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    j->err = tls_conn_handshake(j->c);
}

/* Not in Go. The record the client sends first, read whole from s into a. */
static Slice read_record(TestingT *t, Alloc *a, NetConn s) {
    Byte header[5];
    Error err = BURROW_NO_ERROR;
    io_read_full((IoReader){&s.vt->reader, s.data}, slice_from(header, 5, 5, TYPE_BYTE),
                 &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    Int record_len = (Int)header[3] << 8 | (Int)header[4];

    Slice record = slice_make(a, TYPE_BYTE, record_len, record_len);
    io_read_full((IoReader){&s.vt->reader, s.data}, record, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    return record;
}

typedef struct HostnameInSNITest {
    Str in, out;
} HostnameInSNITest;

static const HostnameInSNITest hostname_in_sni_tests[] = {
    /* Opaque string */
    {BURROW_S_INIT(""), BURROW_S_INIT("")},
    {BURROW_S_INIT("localhost"), BURROW_S_INIT("localhost")},
    {BURROW_S_INIT("foo, bar, baz and qux"), BURROW_S_INIT("foo, bar, baz and qux")},

    /* DNS hostname */
    {BURROW_S_INIT("golang.org"), BURROW_S_INIT("golang.org")},
    {BURROW_S_INIT("golang.org."), BURROW_S_INIT("golang.org")},

    /* Literal IPv4 address */
    {BURROW_S_INIT("1.2.3.4"), BURROW_S_INIT("")},

    /* Literal IPv6 address */
    {BURROW_S_INIT("::1"), BURROW_S_INIT("")},
    /* with zone identifier */
    {BURROW_S_INIT("::1%lo0"), BURROW_S_INIT("")},
    /* as per RFC 5952 we allow the [] style as IPv6 literal */
    {BURROW_S_INIT("[::1]"), BURROW_S_INIT("")},
    {BURROW_S_INIT("[::1%lo0]"), BURROW_S_INIT("")},
};

/* localPipe is a net.Pipe. Go leaves the client goroutine running, and here it
 * is waited for before its Conn is freed. */
static void TestHostnameInSNI(TestingT *t) {
    Alloc *heap = heap_allocator();
    for (size_t i = 0;
         i < sizeof hostname_in_sni_tests / sizeof hostname_in_sni_tests[0]; i++) {
        const HostnameInSNITest *tt = &hostname_in_sni_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        net_pipe(a, &c, &s);

        TlsConfig config;
        memset(&config, 0, sizeof config);
        config.server_name = tt->in;
        config.insecure_skip_verify = true;
        ClientJob job = {tls_client(heap, c, &config), BURROW_NO_ERROR};
        if (job.c == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

        Slice record = read_record(t, a, s);

        (void)c.vt->closer.close(c.data);
        (void)s.vt->closer.close(s.data);
        sync_wait_group_wait(&wg);
        tls_conn_free(job.c);

        TlsClientHelloMsg m = burrow__tls_client_hello_msg_zero();
        if (!burrow__tls_client_hello_msg_unmarshal(&m, a, record)) {
            testing_t_errorf_v(t, "unmarshaling ClientHello for %q failed", tt->in);
        } else {
            if (!str_eq(tt->in, tt->out) && str_eq(m.server_name, tt->in))
                testing_t_errorf_v(t, "prohibited %q found in ClientHello: %x", tt->in,
                                   record);
            if (!str_eq(m.server_name, tt->out))
                testing_t_errorf_v(t, "expected %q not found in ClientHello: %x",
                                   tt->out, record);
        }
        net_pipe_free(c);
        arena_free(&ar);
    }
}

static void TestServerSelectingUnconfiguredCipherSuite(TestingT *t) {
    /* This checks that the server can't select a cipher suite that the client
     * didn't offer. See #13174. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);

    uint16_t suites[1] = {TLS_TLS_RSA_WITH_AES_128_GCM_SHA256};
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("foo");
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientJob job = {tls_client(heap_allocator(), c, &config), BURROW_NO_ERROR};
    if (job.c == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

    (void)read_record(t, a, s);

    /* Create a ServerHello that selects a different cipher suite than the sole
     * one that the client offered. */
    TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
    server_hello.vers = TLS_VERSION_TLS12;
    server_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
    server_hello.cipher_suite = TLS_TLS_RSA_WITH_AES_256_GCM_SHA384;
    Error err = BURROW_NO_ERROR;
    Slice server_hello_bytes =
        burrow__tls_server_hello_msg_marshal(&server_hello, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);

    Byte header[5] = {
        TLS_RECORD_TYPE_HANDSHAKE,        (Byte)(TLS_VERSION_TLS12 >> 8),
        (Byte)(TLS_VERSION_TLS12 & 0xff), (Byte)(server_hello_bytes.len >> 8),
        (Byte)server_hello_bytes.len,
    };
    (void)s.vt->writer.write(s.data, slice_from(header, 5, 5, TYPE_BYTE), NULL);
    (void)s.vt->writer.write(s.data, server_hello_bytes, NULL);
    (void)s.vt->closer.close(s.data);

    sync_wait_group_wait(&wg);
    if (!strings_contains(error_text(job.err), BURROW_S("unconfigured cipher")))
        testing_t_errorf_v(t,
                           "Expected error about unconfigured cipher suite but got %q",
                           error_text(job.err));

    tls_conn_free(job.c);
    (void)c.vt->closer.close(c.data);
    net_pipe_free(c);
    arena_free(&ar);
}

/* discardConn is a net.Conn whose writes go nowhere. Not in Go: nothing reads
 * from it, and a read gets io.EOF. */
static Int discard_read(void *self, Slice b, Error *err) {
    (void)self;
    (void)b;
    BURROW_OUT(err, io_eof);
    return 0;
}

static Int discard_write(void *self, Slice b, Error *err) {
    (void)self;
    (void)err;
    return b.len;
}

static Error discard_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const NetConnVT discard_vt = {
    {NULL, discard_read},
    {NULL, discard_write},
    {NULL, discard_close},
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* testConfigClient is not here yet, so the config only has its ServerName. The
 * key size check comes before anything else in it is looked at. Only the
 * client half is here; the second half, processCertsFromClient, is the server
 * handshake's. */
static void TestHandshakeRSATooBig(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* largeRSAKeyCertPEM contains a 8193 bit RSA key */
    Slice rest;
    PemBlock *test_cert = pem_decode(a, tls_test_pem(a, large_rsa_key_cert_pem), &rest);
    if (test_cert == NULL)
        testing_t_fatal_v(t, "failed to decode largeRSAKeyCertPEM");

    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("test.golang.example");
    TlsConn *c = tls_client(heap_allocator(), (NetConn){&discard_vt, NULL}, &config);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    Str expected_err = BURROW_S(
        "tls: server sent certificate containing RSA key larger than 8192 bits");
    Error err = burrow__tls_conn_verify_server_certificate(
        c, slice_from(&test_cert->bytes, 1, 1, TYPE_BYTES));
    if (BURROW_OK(err) || !str_eq(error_text(err), expected_err))
        testing_t_errorf_v(
            t, "Conn.verifyServerCertificate unexpected error: want %q, got %q",
            expected_err, error_text(err));
    tls_conn_free(c);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLRUClientSessionCache)                                                       \
    X(TestHostnameInSNI)                                                               \
    X(TestServerSelectingUnconfiguredCipherSuite)                                      \
    X(TestHandshakeRSATooBig)

TESTING_MAIN(TESTS)
