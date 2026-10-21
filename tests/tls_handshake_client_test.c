/* Derived from Go's src/crypto/tls/handshake_client_test.go.
 * Go source: go1.27.1.
 *
 * Most of the file runs the client against recorded server flights or a real
 * server, which are not here yet. These are the tests that need neither, and
 * the ECH ones, which run against this package's own server.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_handshake_test.h"
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

/* Not in Go. Reads s until the client closes its end, so that what the
 * client writes after the ServerHello does not block it. */
static void drain_job(void *env) {
    NetConn *s = (NetConn *)env;
    Byte buf[512];
    Error err = BURROW_NO_ERROR;
    while (BURROW_OK(err))
        (void)s->vt->reader.read(s->data, slice_from(buf, 512, 512, TYPE_BYTE), &err);
}

/* Not in Go. Each mutation turns a good TLS 1.3 ServerHello for hello into one
 * the client has to reject. */
static void sh_legacy_version_field(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->supported_version = 0;
    m->vers = TLS_VERSION_TLS13;
}

static void sh_legacy_version(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->vers = TLS_VERSION_TLS11;
}

static void sh_ticket(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->ticket_supported = true;
}

static void sh_session_id(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->session_id = slice_nil(TYPE_BYTE);
}

static void sh_compression(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->compression_method = 1;
}

static void sh_tls12_suite(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->cipher_suite = TLS_TLS_RSA_WITH_AES_128_GCM_SHA256;
}

/* Any non-empty bytes do for the cookie and the ECH extension, so these
 * borrow the client's legacy session ID. */
static void sh_cookie(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    m->cookie = h->session_id;
}

static void sh_no_key_share(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->server_share.group = 0;
}

static void sh_other_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->server_share.group = TLS_CURVE_P521;
}

static void sh_psk(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->selected_identity_present = true;
}

static void sh_hrr(TlsServerHelloMsg *m) {
    memcpy(m->random.p, burrow__tls_hello_retry_request_random, 32);
    m->server_share.group = 0;
    m->server_share.data = slice_nil(TYPE_BYTE);
}

static void hrr_unnecessary(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    sh_hrr(m);
}

static void hrr_same_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    sh_hrr(m);
    m->selected_group = ((const TlsKeyShare *)h->key_shares.p)[0].group;
}

static void hrr_other_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    sh_hrr(m);
    m->selected_group = 0x0a0a;
}

static void hrr_server_share(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    TlsKeyShare share = m->server_share;
    sh_hrr(m);
    m->server_share = share;
    sh_cookie(m, h);
}

static void hrr_ech(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    sh_hrr(m);
    m->encrypted_client_hello = slice_sub(h->session_id, 0, 8);
}

typedef struct ServerHelloTLS13Test {
    const char *name;
    void (*mutate)(TlsServerHelloMsg *m, const TlsClientHelloMsg *h);
    Str want;
} ServerHelloTLS13Test;

static const ServerHelloTLS13Test server_hello_tls13_tests[] = {
    {"legacy version field", sh_legacy_version_field,
     BURROW_S_INIT("tls: server selected TLS 1.3 using the legacy version field")},
    {"legacy version", sh_legacy_version,
     BURROW_S_INIT("tls: server sent an incorrect legacy version")},
    {"ticket extension", sh_ticket,
     BURROW_S_INIT("tls: server sent a ServerHello extension forbidden in TLS 1.3")},
    {"session ID", sh_session_id,
     BURROW_S_INIT("tls: server did not echo the legacy session ID")},
    {"compression", sh_compression,
     BURROW_S_INIT("tls: server sent non-zero legacy TLS compression method")},
    {"TLS 1.2 suite", sh_tls12_suite,
     BURROW_S_INIT("tls: server chose an unconfigured cipher suite")},
    {"cookie", sh_cookie,
     BURROW_S_INIT("tls: server sent a cookie in a normal ServerHello")},
    {"no key share", sh_no_key_share,
     BURROW_S_INIT("tls: server did not send a key share")},
    {"other group", sh_other_group,
     BURROW_S_INIT("tls: server selected unsupported group")},
    {"PSK", sh_psk, BURROW_S_INIT("tls: server selected an invalid PSK")},
    {"unnecessary HRR", hrr_unnecessary,
     BURROW_S_INIT("tls: server sent an unnecessary HelloRetryRequest message")},
    {"HRR for a sent group", hrr_same_group,
     BURROW_S_INIT("tls: server sent an unnecessary HelloRetryRequest key_share")},
    {"HRR for another group", hrr_other_group,
     BURROW_S_INIT("tls: server selected unsupported group")},
    {"HRR with a key share", hrr_server_share,
     BURROW_S_INIT("tls: received malformed key_share extension")},
    {"HRR with ECH", hrr_ech,
     BURROW_S_INIT("tls: unexpected encrypted client hello extension in serverHello")},
};

/* Not in Go. Go checks these through its recorded handshakes and its own
 * server. This sends the client a TLS 1.3 ServerHello, or HelloRetryRequest,
 * with one thing wrong, and checks the error the handshake gives. */
static void TestClientTLS13ServerHelloErrors(TestingT *t) {
    for (size_t i = 0;
         i < sizeof server_hello_tls13_tests / sizeof server_hello_tls13_tests[0];
         i++) {
        const ServerHelloTLS13Test *tt = &server_hello_tls13_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        net_pipe(a, &c, &s);

        TlsConfig config;
        memset(&config, 0, sizeof config);
        config.server_name = BURROW_S("foo");
        ClientJob job = {tls_client(heap_allocator(), c, &config), BURROW_NO_ERROR};
        if (job.c == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

        Slice record = read_record(t, a, s);
        TlsClientHelloMsg hello = burrow__tls_client_hello_msg_zero();
        if (!burrow__tls_client_hello_msg_unmarshal(&hello, a, record) ||
            hello.key_shares.len == 0)
            testing_t_fatalf_v(t, "%s: unmarshaling ClientHello failed", tt->name);

        TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
        server_hello.vers = TLS_VERSION_TLS12;
        server_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
        server_hello.session_id = hello.session_id;
        server_hello.cipher_suite = TLS_TLS_AES_128_GCM_SHA256;
        server_hello.supported_version = TLS_VERSION_TLS13;
        server_hello.server_share = ((const TlsKeyShare *)hello.key_shares.p)[0];
        tt->mutate(&server_hello, &hello);
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
        SyncWaitGroup drain = {0};
        sync_wait_group_go(&drain, BURROW_FN(Func, drain_job, &s));

        sync_wait_group_wait(&wg);
        if (!str_eq(error_text(job.err), tt->want))
            testing_t_errorf_v(t, "%s: got error %q, want %q", tt->name,
                               error_text(job.err), tt->want);

        (void)c.vt->closer.close(c.data);
        sync_wait_group_wait(&drain);
        (void)s.vt->closer.close(s.data);
        tls_conn_free(job.c);
        net_pipe_free(c);
        arena_free(&ar);
    }
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

static const char ech_test_config_list[] =
    "0041fe0d003d0100200020204bed0a11fc0dde595a9b78d966b0011128eb83f65d3c91c1cc5ac786c"
    "d246f000400010001ff0e6578616d706c652e676f6c616e670000";

static Slice ech_config_list(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(ech_test_config_list), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return b;
}

BURROW_SENTINEL_ERROR(ech_callback_err, "callback err");

static Error ech_verify_connection_ok(void *env, const TlsConnectionState *cs) {
    (void)env;
    (void)cs;
    return BURROW_NO_ERROR;
}

static Error ech_verify_connection_err(void *env, const TlsConnectionState *cs) {
    (void)env;
    (void)cs;
    return ech_callback_err;
}

static Error ech_verify_peer_certificate_err(void *env, Slice raw_certs,
                                             Slice verified_chains) {
    (void)env;
    (void)raw_certs;
    (void)verified_chains;
    return ech_callback_err;
}

typedef struct ECHRejectionCallbacksCase {
    const char *name;
    const char *expected_err;

    TlsVerifyConnectionFunc verify_connection;
    TlsVerifyPeerCertificateFunc verify_peer_certificate;
    TlsVerifyConnectionFunc encrypted_client_hello_rejection_verify;
} ECHRejectionCallbacksCase;

typedef struct ECHRejectionCallbacks {
    const ECHRejectionCallbacksCase *tc;
    TlsConfig *client_config, *server_config;
} ECHRejectionCallbacks;

/* Not in Go: a Server handshake run on its own goroutine, which closes its end
 * of the pipe after. */
typedef struct ServerJob {
    TlsConn *s;
    NetConn conn;
    Error err;
} ServerJob;

static void server_handshake_job(void *env) {
    ServerJob *j = (ServerJob *)env;
    j->err = tls_conn_handshake(j->s);
    (void)j->conn.vt->closer.close(j->conn.data);
}

static void ech_rejection_callbacks(void *env, TestingT *t) {
    const ECHRejectionCallbacks *e = env;
    const ECHRejectionCallbacksCase *tc = e->tc;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    ServerJob job = {tls_server(heap_allocator(), s, e->server_config), s,
                     BURROW_NO_ERROR};
    if (job.s == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, server_handshake_job, &job));

    TlsConfig c_config = *e->client_config;
    c_config.verify_connection = tc->verify_connection;
    c_config.verify_peer_certificate = tc->verify_peer_certificate;
    c_config.encrypted_client_hello_rejection_verify =
        tc->encrypted_client_hello_rejection_verify;

    TlsConn *cli = tls_client(heap_allocator(), c, &c_config);
    if (cli == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error client_err = tls_conn_handshake(cli);
    (void)c.vt->closer.close(c.data);
    sync_wait_group_wait(&wg);

    Str expected_err = str_from_cstr(tc->expected_err);
    if (expected_err.len == 0 && BURROW_FAILED(client_err))
        testing_t_errorf_v(t, "unexpected err: %s", error_text(client_err));
    else if (BURROW_FAILED(client_err) && !str_eq(expected_err, error_text(client_err)))
        testing_t_errorf_v(t, "unexpected err: got %q, want %q", error_text(client_err),
                           expected_err);
    tls_conn_free(cli);
    tls_conn_free(job.s);
    local_pipe_free(c);
    arena_free(&ar);
}

static void TestTLS13ECHRejectionCallbacks(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Str dns_names[1] = {BURROW_S("example.golang")};
    X509Certificate tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("test");
    tmpl.dns_names = slice_from(dns_names, 1, 1, TYPE_STRING);
    tmpl.not_before = time_add(test_time(NULL), -TIME_HOUR);
    tmpl.not_after = time_add(test_time(NULL), TIME_HOUR);
    Slice cert_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                             ecdsa_private_key_public(k),
                                             ecdsa_private_key_signer(k), &err);
    X509Certificate *cert = NULL;
    if (!BURROW_FAILED(err))
        cert = x509_parse_certificate(a, cert_der, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }

    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);
    TlsCertificate certificate;
    memset(&certificate, 0, sizeof certificate);
    certificate.certificate = slice_from(&cert_der, 1, 1, TYPE_BYTES);
    certificate.private_key = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k);
    server_config.certificates = slice_from(&certificate, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.min_version = TLS_VERSION_TLS13;
    client_config.root_cas = x509_new_cert_pool(a);
    if (client_config.root_cas == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    x509_cert_pool_add_cert(client_config.root_cas, cert);
    client_config.min_version = TLS_VERSION_TLS13;
    client_config.encrypted_client_hello_config_list = ech_config_list(t, a);
    client_config.server_name = BURROW_S("example.golang");

    static const ECHRejectionCallbacksCase cases[] = {
        {
            .name = "no callbacks",
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "EncryptedClientHelloRejectionVerify, no err",
            .encrypted_client_hello_rejection_verify = {ech_verify_connection_ok, NULL},
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "EncryptedClientHelloRejectionVerify, err",
            .encrypted_client_hello_rejection_verify = {ech_verify_connection_err,
                                                        NULL},
            /* testHandshake returns the server side error, so we just need to
             * check alertBadCertificate was sent */
            .expected_err = "callback err",
        },
        {
            .name = "VerifyConnection, err",
            .verify_connection = {ech_verify_connection_err, NULL},
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "VerifyPeerCertificate, err",
            .verify_peer_certificate = {ech_verify_peer_certificate_err, NULL},
            .expected_err = "tls: server rejected ECH",
        },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ECHRejectionCallbacks env = {&cases[i], &client_config, &server_config};
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, ech_rejection_callbacks, &env));
    }

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void TestECHTLS12Server(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);

    server_config.max_version = TLS_VERSION_TLS12;
    client_config.min_version = 0;

    client_config.encrypted_client_hello_config_list = ech_config_list(t, a);

    Str expected_err = BURROW_S(
        "server: tls: client offered only unsupported versions: [304]\nclient: "
        "remote error: tls: protocol version not supported");
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_OK(h.err) || !str_eq(error_text(h.err), expected_err)) {
        testing_t_errorf_v(t, "unexpected handshake error: got %q, want %q",
                           BURROW_OK(h.err) ? BURROW_S("<nil>") : error_text(h.err),
                           expected_err);
        handshake_free(&h);
        tls_config_release(&server_config);
        tls_config_release(&client_config);
        arena_free(&ar);
        testing_t_fail_now(t);
    }
    handshake_free(&h);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLRUClientSessionCache)                                                       \
    X(TestHostnameInSNI)                                                               \
    X(TestServerSelectingUnconfiguredCipherSuite)                                      \
    X(TestClientTLS13ServerHelloErrors)                                                \
    X(TestHandshakeRSATooBig)                                                          \
    X(TestTLS13ECHRejectionCallbacks)                                                  \
    X(TestECHTLS12Server)

TESTING_MAIN(TESTS)
