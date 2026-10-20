/* Derived from Go's src/crypto/tls/ticket_test.go.
 * Go source: go1.27.1.
 *
 * ticket_test.go only checks, when it compiles, that Config.EncryptTicket and
 * Config.DecryptTicket fit WrapSession and UnwrapSession. Here they take the
 * config as an argument, so wrap_session and unwrap_session are what fits, and
 * the compiler checks those the same way. The tests are not in Go, which
 * covers tickets with its handshake tests. Those come with the handshakes.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/tls.h"
#include "tls_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice wrap_session(void *env, Alloc *a, const TlsConnectionState *cs,
                          TlsSessionState *ss, Error *err) {
    return tls_config_encrypt_ticket(env, a, cs, ss, err);
}

static TlsSessionState *unwrap_session(void *env, Slice identity,
                                       const TlsConnectionState *cs, Error *err) {
    return tls_config_decrypt_ticket(env, identity, cs, err);
}

/* var _ = &Config{WrapSession: (&Config{}).EncryptTicket} and the same for
 * UnwrapSession, on c. */
static void set_ticket_funcs(TlsConfig *c) {
    c->wrap_session = BURROW_FN(TlsWrapSessionFunc, wrap_session, c);
    c->unwrap_session = BURROW_FN(TlsUnwrapSessionFunc, unwrap_session, c);
}

/* Not in Go: a client session for TLS 1.3 with a chain of two certificates,
 * one verified chain, an extra entry and an ALPN protocol, parsed back from its
 * bytes so that it has an arena of its own, from the heap. */
static TlsSessionState *client_session(TestingT *t, Alloc *a) {
    Slice certs = slice_make(a, TYPE_X509_CERTIFICATE_PTR, 2, 2);
    if (certs.p == NULL)
        testing_t_fatal_v(t, "out of memory");
    BURROW_AT(X509Certificate *, certs, 0) =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem).leaf;
    BURROW_AT(X509Certificate *, certs, 1) =
        parse_test_cert(a, test_root_cert_pem, test_root_key_pem).leaf;
    Slice chains = slice_make(a, TYPE_X509_CERTIFICATE_CHAIN, 1, 1);
    Slice extra = slice_make(a, TYPE_BYTES, 1, 1);
    if (chains.p == NULL || extra.p == NULL)
        testing_t_fatal_v(t, "out of memory");
    BURROW_AT(Slice, chains, 0) = certs;
    static Byte extra_bytes[] = {'x', 1, 2, 3};
    BURROW_AT(Slice, extra, 0) = slice_from(extra_bytes, 4, 4, TYPE_BYTE);
    static Byte secret[32] = {1, 2, 3, 4, 5, 6, 7, 8};

    TlsSessionState s;
    memset(&s, 0, sizeof s);
    s.extra = extra;
    s.early_data = true;
    s.version = TLS_VERSION_TLS13;
    s.is_client = true;
    s.cipher_suite = TLS_TLS_AES_128_GCM_SHA256;
    s.created_at = 1700000000;
    s.secret = slice_from(secret, 32, 32, TYPE_BYTE);
    s.peer_certificates = certs;
    s.verified_chains = chains;
    s.alpn_protocol = BURROW_S("h2");
    s.use_by = 1700086400;
    s.age_add = 0x01020304;

    Error err = BURROW_NO_ERROR;
    Slice b = tls_session_state_bytes(&s, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bytes: %v", err);
    TlsSessionState *ss = tls_parse_session_state(heap_allocator(), b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseSessionState: %v", err);
    return ss;
}

/* Not in Go: what client_session put in, checked field by field. */
static void check_client_session(TestingT *t, const TlsSessionState *ss) {
    if (ss->version != TLS_VERSION_TLS13 || !ss->is_client ||
        ss->cipher_suite != TLS_TLS_AES_128_GCM_SHA256 ||
        ss->created_at != 1700000000 || ss->use_by != 1700086400 ||
        ss->age_add != 0x01020304 || !ss->early_data || ss->ext_master_secret)
        testing_t_errorf_v(t, "the fixed fields did not round trip");
    if (ss->secret.len != 32 || ((const Byte *)ss->secret.p)[7] != 8)
        testing_t_errorf_v(t, "secret did not round trip");
    if (ss->extra.len != 1 || BURROW_AT(Slice, ss->extra, 0).len != 4)
        testing_t_errorf_v(t, "Extra did not round trip");
    if (!str_eq(ss->alpn_protocol, BURROW_S("h2")))
        testing_t_errorf_v(t, "alpnProtocol = %q, want %q", ss->alpn_protocol,
                           BURROW_S("h2"));
    if (ss->peer_certificates.len != 2 || ss->verified_chains.len != 1) {
        testing_t_errorf_v(t, "got %d certificates and %d chains, want 2 and 1",
                           ss->peer_certificates.len, ss->verified_chains.len);
        return;
    }
    Slice chain = BURROW_AT(Slice, ss->verified_chains, 0);
    if (chain.len != 2 ||
        BURROW_AT(X509Certificate *, chain, 0) !=
            BURROW_AT(X509Certificate *, ss->peer_certificates, 0) ||
        BURROW_AT(X509Certificate *, chain, 1) !=
            BURROW_AT(X509Certificate *, ss->peer_certificates, 1))
        testing_t_errorf_v(t, "the verified chain does not share the certificates");
}

/* Not in Go. */
static void TestEncryptDecryptTicket(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig config = {0};
    set_ticket_funcs(&config);

    TlsSessionState *ss = client_session(t, a);
    check_client_session(t, ss);
    Error err = BURROW_NO_ERROR;
    TlsConnectionState cs;
    memset(&cs, 0, sizeof cs);
    Slice ticket = BURROW_CALLF(config.wrap_session, a, &cs, ss, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "EncryptTicket: %v", err);
    tls_session_state_free(ss);

    TlsSessionState *got = BURROW_CALLF(config.unwrap_session, ticket, &cs, &err);
    if (BURROW_FAILED(err) || got == NULL)
        testing_t_fatalf_v(t, "DecryptTicket: %v", err);
    check_client_session(t, got);
    tls_session_state_free(got);

    /* A ticket with a byte changed, or too short to hold an IV and a MAC, is
     * dropped without an error. */
    Slice bad = bytes_clone(a, ticket);
    ((Byte *)bad.p)[20] ^= 1;
    got = BURROW_CALLF(config.unwrap_session, bad, &cs, &err);
    if (got != NULL || BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptTicket accepted a changed ticket");
    got = BURROW_CALLF(config.unwrap_session, slice_sub(ticket, 0, 47), &cs, &err);
    if (got != NULL || BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptTicket accepted a short ticket");

    /* New keys turn the ticket away, and the old key behind a new one opens
     * it again. */
    static const Byte keys[2][32] = {{1}, {2}};
    err = tls_config_set_session_ticket_keys(&config, keys, 1);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = BURROW_CALLF(config.unwrap_session, ticket, &cs, &err);
    if (got != NULL || BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptTicket opened a ticket with a key it lost");

    ss = client_session(t, a);
    Slice ticket1 = BURROW_CALLF(config.wrap_session, a, &cs, ss, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "EncryptTicket: %v", err);
    tls_session_state_free(ss);
    err = tls_config_set_session_ticket_keys(&config, keys + 1, 1);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = BURROW_CALLF(config.unwrap_session, ticket1, &cs, &err);
    if (got != NULL || BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptTicket opened a ticket with a key it lost");
    err = tls_config_set_session_ticket_keys(&config, keys, 2);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = BURROW_CALLF(config.unwrap_session, ticket1, &cs, &err);
    if (got == NULL || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "DecryptTicket did not try every key: %v", err);
    check_client_session(t, got);
    tls_session_state_free(got);

    tls_config_release(&config);
    arena_free(&ar);
}

/* Not in Go. */
static void TestParseSessionStateErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TlsSessionState *ss = client_session(t, a);
    Error err = BURROW_NO_ERROR;
    Slice b = tls_session_state_bytes(ss, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    tls_session_state_free(ss);

    /* The type is the third byte, and ext_master_secret follows the extra
     * entries, 2+1+2+8+1+32 bytes in and then 3+3+4 more. */
    struct {
        Int at;
        Byte value;
        const char *want;
    } tests[] = {
        {2, 3, "tls: unknown session encoding"},
        {2 + 1 + 2 + 8 + 1 + 32 + 3 + 3 + 4, 2, "tls: invalid session encoding"},
        {2 + 1 + 2 + 8 + 1 + 32 + 3 + 3 + 4 + 1, 2, "tls: invalid session encoding"},
        {2 + 1 + 2 + 8, 0, "tls: invalid session encoding"},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Slice c = bytes_clone(a, b);
        ((Byte *)c.p)[tests[i].at] = tests[i].value;
        if (tests[i].value == 0) {
            /* An empty secret, with the rest moved up by the 32 bytes it
             * had. */
            memmove((Byte *)c.p + tests[i].at + 1, (Byte *)c.p + tests[i].at + 1 + 32,
                    (size_t)(c.len - tests[i].at - 1 - 32));
            c.len -= 32;
        }
        err = BURROW_NO_ERROR;
        TlsSessionState *got = tls_parse_session_state(heap_allocator(), c, &err);
        if (got != NULL || !BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: ParseSessionState did not fail", (Int)i);
            tls_session_state_free(got);
            continue;
        }
        Str msg = error_text(err);
        if (!str_eq(msg, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "#%d: got %q, want %q", (Int)i, msg,
                               str_from_cstr(tests[i].want));
    }

    /* A client session with no certificates. */
    TlsSessionState s;
    memset(&s, 0, sizeof s);
    static Byte secret[1] = {1};
    s.version = TLS_VERSION_TLS12;
    s.is_client = true;
    s.secret = slice_from(secret, 1, 1, TYPE_BYTE);
    err = BURROW_NO_ERROR;
    b = tls_session_state_bytes(&s, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    TlsSessionState *got = tls_parse_session_state(heap_allocator(), b, &err);
    if (got != NULL ||
        !str_eq(error_text(err),
                BURROW_S("tls: no server certificates in client session")))
        testing_t_errorf_v(t, "got %v, want no server certificates", err);

    /* A verified chain with nothing in it. */
    Slice certs = slice_make(a, TYPE_X509_CERTIFICATE_PTR, 1, 1);
    BURROW_AT(X509Certificate *, certs, 0) =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem).leaf;
    Slice chains = slice_make(a, TYPE_X509_CERTIFICATE_CHAIN, 1, 1);
    BURROW_AT(Slice, chains, 0) = slice_nil(TYPE_X509_CERTIFICATE_PTR);
    s.peer_certificates = certs;
    s.verified_chains = chains;
    err = BURROW_NO_ERROR;
    b = tls_session_state_bytes(&s, a, &err);
    if (b.p != NULL ||
        !str_eq(error_text(err), BURROW_S("tls: internal error: empty verified chain")))
        testing_t_errorf_v(t, "got %v, want empty verified chain", err);
    arena_free(&ar);
}

/* Not in Go: the LRU cache holds a reference to what is in it, Get hands out
 * one more, and the last release frees the session. ASan and the heap tell
 * whether anything is freed twice or not at all. */
static void TestClientSessionStateRefs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TlsSessionState *ss = client_session(t, a);
    static Byte ticket_bytes[] = {9, 8, 7, 6};
    Slice ticket = slice_from(ticket_bytes, 4, 4, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    TlsClientSessionState *cs = tls_new_resumption_state(ticket, ss, &err);
    if (cs == NULL)
        testing_t_fatal_v(t, err);
    ticket_bytes[0] = 0;

    TlsSessionState *state = NULL;
    Slice got_ticket = tls_client_session_state_resumption_state(cs, &state, &err);
    if (state != ss || got_ticket.len != 4 || ((const Byte *)got_ticket.p)[0] != 9)
        testing_t_errorf_v(t, "ResumptionState did not give back the ticket and state");
    got_ticket = tls_client_session_state_resumption_state(NULL, &state, &err);
    if (state != NULL || got_ticket.p != NULL)
        testing_t_errorf_v(t, "ResumptionState of nil gave something back");

    TlsClientSessionCache cache = tls_new_lru_client_session_cache(heap_allocator(), 2);
    if (cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    tls_client_session_cache_put(cache, BURROW_S("a"), cs);
    tls_client_session_cache_put(cache, BURROW_S("b"), cs);
    tls_client_session_state_release(cs);

    bool ok;
    TlsClientSessionState *got =
        tls_client_session_cache_get(cache, BURROW_S("a"), &ok);
    if (!ok || got != cs)
        testing_t_fatalf_v(t, "Get did not find the session");
    tls_client_session_state_release(got);

    /* Replacing and deleting both let go of the old reference. */
    ss = client_session(t, a);
    TlsClientSessionState *cs2 = tls_new_resumption_state(ticket, ss, &err);
    if (cs2 == NULL)
        testing_t_fatal_v(t, err);
    tls_client_session_cache_put(cache, BURROW_S("a"), cs2);
    tls_client_session_cache_put(cache, BURROW_S("b"), NULL);
    tls_client_session_state_release(cs2);
    got = tls_client_session_cache_get(cache, BURROW_S("b"), &ok);
    if (ok || got != NULL)
        testing_t_errorf_v(t, "Get found a deleted session");

    /* An entry pushed out frees its session, and freeing the cache frees
     * what is left. */
    ss = client_session(t, a);
    TlsClientSessionState *cs3 = tls_new_resumption_state(ticket, ss, &err);
    if (cs3 == NULL)
        testing_t_fatal_v(t, err);
    tls_client_session_cache_put(cache, BURROW_S("c"), cs3);
    tls_client_session_cache_put(cache, BURROW_S("d"), cs3);
    tls_client_session_state_release(cs3);
    got = tls_client_session_cache_get(cache, BURROW_S("a"), &ok);
    if (ok || got != NULL)
        testing_t_errorf_v(t, "Get found a session that should be gone");
    tls_lru_client_session_cache_free(cache);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncryptDecryptTicket)                                                        \
    X(TestParseSessionStateErrors)                                                     \
    X(TestClientSessionStateRefs)

TESTING_MAIN(TESTS)
