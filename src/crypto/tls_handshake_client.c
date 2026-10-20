/* crypto/tls: handshake_client.go, the client side of a TLS 1.0–1.2 handshake
 * and the ClientHello every client handshake starts with.
 *
 * The TLS 1.3 handshake is in tls_handshake_client_tls13.c. QUIC comes with
 * quic.go and Encrypted Client Hello with ech.go.
 *
 * Everything the handshake only needs while it runs is made in a scratch
 * arena that goes when it ends. What the Conn keeps afterwards, the server's
 * certificates and what came with them, is copied into the Conn's own arena.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/subtle.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(tcl_err_server_name, "tls: either ServerName or "
                                           "InsecureSkipVerify must be specified in "
                                           "the tls.Config");
BURROW_SENTINEL_ERROR(tcl_err_next_protos, "tls: invalid NextProtos value");
BURROW_SENTINEL_ERROR(tcl_err_next_protos_large, "tls: NextProtos values too large");
BURROW_SENTINEL_ERROR(tcl_err_no_versions, "tls: no supported versions satisfy "
                                           "MinVersion and MaxVersion");
BURROW_SENTINEL_ERROR(tcl_err_no_curves,
                      "tls: no supported key exchange methods (CurveIDs)");
BURROW_SENTINEL_ERROR(tcl_err_curve_accepted, "tls: internal error: supportsCurve "
                                              "accepted unimplemented curve");
BURROW_SENTINEL_ERROR(tcl_err_ech_min_version, "tls: MinVersion must be >= "
                                               "VersionTLS13 if "
                                               "EncryptedClientHelloConfigList is "
                                               "populated");
BURROW_SENTINEL_ERROR(tcl_err_ech_max_version, "tls: MaxVersion must be >= "
                                               "VersionTLS13 if "
                                               "EncryptedClientHelloConfigList is "
                                               "populated");
BURROW_SENTINEL_ERROR(tcl_err_ech_not_here, "tls: Encrypted Client Hello is not "
                                            "supported yet");
BURROW_SENTINEL_ERROR(tcl_err_downgrade, "tls: downgrade attempt detected, possibly "
                                         "due to a MitM attack or a broken middlebox");
BURROW_SENTINEL_ERROR(tcl_err_echoed_session_id, "tls: server echoed TLS 1.3 "
                                                 "compatibility session ID in TLS 1.2");
BURROW_SENTINEL_ERROR(tcl_err_unconfigured_suite,
                      "tls: server chose an unconfigured cipher suite");
BURROW_SENTINEL_ERROR(tcl_err_unexpected_status,
                      "tls: received unexpected CertificateStatus message");
BURROW_SENTINEL_ERROR(tcl_err_identity_changed,
                      "tls: server's identity changed during renegotiation");
BURROW_SENTINEL_ERROR(tcl_err_no_transcript, "tls: internal error: did not keep "
                                             "handshake transcript for TLS 1.2");
BURROW_SENTINEL_ERROR(tcl_err_compression,
                      "tls: server selected unsupported compression format");
BURROW_SENTINEL_ERROR(tcl_err_point_formats,
                      "tls: server offered only incompatible point formats");
BURROW_SENTINEL_ERROR(tcl_err_initial_renegotiation, "tls: initial handshake had "
                                                     "non-empty renegotiation "
                                                     "extension");
BURROW_SENTINEL_ERROR(tcl_err_renegotiation_contents,
                      "tls: incorrect renegotiation extension contents");
BURROW_SENTINEL_ERROR(tcl_err_resumed_version,
                      "tls: server resumed a session with a different version");
BURROW_SENTINEL_ERROR(tcl_err_resumed_suite,
                      "tls: server resumed a session with a different cipher suite");
BURROW_SENTINEL_ERROR(tcl_err_resumed_ems,
                      "tls: server resumed a session with a different EMS extension");
BURROW_SENTINEL_ERROR(tcl_err_no_alpn, "tls: server did not select an ALPN protocol");
BURROW_SENTINEL_ERROR(tcl_err_unrequested_alpn,
                      "tls: server advertised unrequested ALPN extension");
BURROW_SENTINEL_ERROR(tcl_err_unadvertised_alpn,
                      "tls: server selected unadvertised ALPN protocol");
BURROW_SENTINEL_ERROR(tcl_err_finished, "tls: server's Finished message was incorrect");
BURROW_SENTINEL_ERROR(tcl_err_unrequested_ticket,
                      "tls: server sent unrequested session ticket");
BURROW_SENTINEL_ERROR(tcl_err_mldsa_tls12, "tls: server's certificate uses ML-DSA, "
                                           "which requires TLS 1.3");

/* defaultMaxRSAKeySize is the maximum RSA key size in bits that we are willing
 * to verify the signatures of during a TLS handshake. */
#define TCL_DEFAULT_MAX_RSA_KEY_SIZE 8192

typedef struct TclState {
    TlsConn *c;
    Context ctx;
    TlsHandshakeMsg *server_hello_msg;
    TlsServerHelloMsg *server_hello;
    TlsHandshakeMsg *hello_msg;
    TlsClientHelloMsg *hello;
    const TlsSuite *suite;
    TlsFinishedHash finished_hash;
    Slice master_secret;
    TlsSessionState *session; /* the session being resumed */
    Slice ticket;             /* a fresh ticket received during this handshake */

    /* Not in Go: the cache entry session came from, which holds it, and the
     * scratch allocator. */
    TlsClientSessionState *cs;
    Alloc *a;
} TclState;

/* Not in Go. The allocator of what c keeps once the handshake is over. */
static Alloc *tcl_keep(TlsConn *c) {
    return arena_allocator(&c->state);
}

bool burrow__tls_clone_bytes_list(Alloc *a, Slice list, Slice *out) {
    if (slice_is_nil(list)) {
        *out = slice_nil(TYPE_BYTES);
        return true;
    }
    Slice copy = slice_make(a, TYPE_BYTES, list.len, list.len);
    if (copy.p == NULL && list.len > 0)
        return false;
    for (Int i = 0; i < list.len; i++) {
        Slice b = ((const Slice *)list.p)[i];
        Slice d = bytes_clone(a, b);
        if (d.p == NULL && b.len > 0)
            return false;
        ((Slice *)copy.p)[i] = d;
    }
    *out = copy;
    return true;
}

bool burrow__tls_clone_str(Alloc *a, Str s, Str *out) {
    Slice b = slice_from((Byte *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
    Slice d = bytes_clone(a, b);
    if (d.p == NULL && s.len > 0)
        return false;
    *out = (Str){d.p, d.len};
    return true;
}

/* Not in Go. The n bytes of src as a new []byte from a. */
static Slice tcl_bytes(Alloc *a, const void *src, Int n) {
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    if (b.p != NULL && n > 0)
        memcpy(b.p, src, (size_t)n);
    return b;
}

/* Not in Go. "tls: short read from Rand: " and err, as Go builds it. */
static Error tcl_short_read(Error err) {
    return fmt_errorf_v("tls: short read from Rand: %v", err);
}

/* Not in Go. n random bytes from the config's reader into a new []byte. */
static Slice tcl_random(Alloc *a, const TlsConfig *config, Int n, Error *err) {
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    if (b.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return b;
    }
    Error e = BURROW_NO_ERROR;
    io_read_full(burrow__tls_config_rand(config), b, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, tcl_short_read(e));
        return slice_nil(TYPE_BYTE);
    }
    return b;
}

/* Not in Go. Whether want is one of the CurveIDs in list. */
static bool tcl_contains_curve(Slice list, TlsCurveID want) {
    const TlsCurveID *ids = list.p;
    for (Int i = 0; i < list.len; i++) {
        if (ids[i] == want)
            return true;
    }
    return false;
}

/* Not in Go: what makeClientHello returns besides the hello. Go's
 * echClientContext comes with ech.go. */
typedef struct TclHello {
    TlsHandshakeMsg msg;
    TlsKeySharePrivateKeys key_share_keys;
} TclHello;

static Error tcl_make_client_hello(TlsConn *c, Alloc *a, TclHello *out) {
    TlsConfig *config = c->config;
    if (config->server_name.len == 0 && !config->insecure_skip_verify)
        return tcl_err_server_name;

    Int next_protos_length = 0;
    const Str *protos = config->next_protos.p;
    for (Int i = 0; i < config->next_protos.len; i++) {
        Int l = protos[i].len;
        if (l == 0 || l > 255)
            return tcl_err_next_protos;
        next_protos_length += 1 + l;
    }
    if (next_protos_length > 0xffff)
        return tcl_err_next_protos_large;

    uint16_t versions[4];
    Int nversions =
        burrow__tls_config_supported_versions(config, TLS_ROLE_CLIENT, false, versions);
    if (nversions == 0)
        return tcl_err_no_versions;
    /* Since supportedVersions is sorted in descending order, the first element
     * is the maximum version and the last element is the minimum version. */
    uint16_t max_version = versions[0];
    uint16_t min_version = versions[nversions - 1];

    TlsCurveID curves[8];
    Int ncurves = burrow__tls_config_curve_preferences(config, max_version, curves);

    burrow__tls_handshake_msg_init(&out->msg, TLS_MSG_CLIENT_HELLO);
    TlsClientHelloMsg *hello = &out->msg.client_hello;
    Byte compression_none = TLS_COMPRESSION_NONE;
    Byte point_format_uncompressed = TLS_POINT_FORMAT_UNCOMPRESSED;
    hello->vers = max_version;
    hello->compression_methods = tcl_bytes(a, &compression_none, 1);
    hello->extended_master_secret = true;
    hello->ocsp_stapling = true;
    hello->scts = true;
    hello->server_name = burrow__tls_hostname_in_sni(config->server_name);
    hello->supported_curves = slice_make(a, TYPE_UINT16, ncurves, ncurves);
    hello->supported_points = tcl_bytes(a, &point_format_uncompressed, 1);
    hello->secure_renegotiation_supported = true;
    hello->alpn_protocols = config->next_protos;
    hello->supported_versions = slice_make(a, TYPE_UINT16, nversions, nversions);
    if (hello->compression_methods.p == NULL || hello->supported_points.p == NULL ||
        (hello->supported_curves.p == NULL && ncurves > 0) ||
        hello->supported_versions.p == NULL)
        return burrow_err_out_of_memory;
    if (ncurves > 0)
        memcpy(hello->supported_curves.p, curves, (size_t)ncurves * sizeof curves[0]);
    memcpy(hello->supported_versions.p, versions,
           (size_t)nversions * sizeof versions[0]);
    if (slice_is_nil(hello->alpn_protocols))
        hello->alpn_protocols = slice_nil(TYPE_STRING);

    /* The version at the beginning of the ClientHello was capped at TLS 1.2
     * for compatibility reasons. The supported_versions extension is used to
     * negotiate versions now. See RFC 8446, Section 4.2.1. */
    if (hello->vers > TLS_VERSION_TLS12)
        hello->vers = TLS_VERSION_TLS12;

    if (c->handshakes > 0)
        hello->secure_renegotiation = slice_from(c->client_finished, 12, 12, TYPE_BYTE);

    hello->cipher_suites = burrow__tls_config_cipher_suites(
        config, a, burrow__tls_has_aes_gcm_hardware_support());
    if (hello->cipher_suites.p == NULL && hello->cipher_suites.len > 0)
        return burrow_err_out_of_memory;
    /* Don't advertise TLS 1.2-only cipher suites unless we're attempting TLS
     * 1.2. */
    if (max_version < TLS_VERSION_TLS12) {
        uint16_t *ids = hello->cipher_suites.p;
        Int n = 0;
        for (Int i = 0; i < hello->cipher_suites.len; i++) {
            if ((burrow__tls_cipher_suite_by_id(ids[i])->flags & TLS_SUITE_TLS12) == 0)
                ids[n++] = ids[i];
        }
        hello->cipher_suites.len = n;
    }

    Error err = BURROW_NO_ERROR;
    hello->random = tcl_random(a, config, 32, &err);
    if (BURROW_FAILED(err))
        return err;

    /* A random session ID is used to detect when the server accepted a ticket
     * and is resuming a session (see RFC 5077). In TLS 1.3, it's always set as
     * a compatibility measure (see RFC 8446, Section 4.1.2).
     *
     * The session ID is not set for QUIC connections (see RFC 9001, Section
     * 8.4). Those come with quic.go. */
    hello->session_id = tcl_random(a, config, 32, &err);
    if (BURROW_FAILED(err))
        return err;

    if (max_version >= TLS_VERSION_TLS12) {
        hello->supported_signature_algorithms =
            burrow__tls_supported_signature_algorithms(a, min_version, max_version);
        hello->supported_signature_algorithms_cert =
            burrow__tls_supported_signature_algorithms_cert(a, min_version,
                                                            max_version);
        if (hello->supported_signature_algorithms.p == NULL ||
            hello->supported_signature_algorithms_cert.p == NULL)
            return burrow_err_out_of_memory;
    }

    if (max_version >= TLS_VERSION_TLS13) {
        /* Reset the list of ciphers when the client only supports TLS 1.3. */
        if (min_version >= TLS_VERSION_TLS13)
            hello->cipher_suites = slice_nil(TYPE_UINT16);

        /* There is no FIPS 140-3 mode here, so Go's allowedCipherSuitesTLS13FIPS
         * never comes into it. */
        const uint16_t *tls13 = burrow__tls_has_aes_gcm_hardware_support()
                                    ? burrow__tls_default_cipher_suites_tls13
                                    : burrow__tls_default_cipher_suites_tls13_no_aes;
        hello->cipher_suites = slice_append(a, hello->cipher_suites, tls13, 3);
        if (hello->cipher_suites.p == NULL)
            return burrow_err_out_of_memory;

        if (hello->supported_curves.len == 0)
            return tcl_err_no_curves;
        /* Since the order is fixed, the first one is always the one to send a
         * key share for. All the PQ hybrids sort first, and produce a fallback
         * ECDH share. */
        TlsCurveID curve_id = ((const TlsCurveID *)hello->supported_curves.p)[0];
        Error e = BURROW_NO_ERROR;
        const TlsKeyExchange *ke = burrow__tls_key_exchange_for_curve_id(curve_id, &e);
        if (ke == NULL)
            return tcl_err_curve_accepted;
        if (!burrow__tls_key_exchange_key_shares(ke, a, burrow__tls_config_rand(config),
                                                 &out->key_share_keys,
                                                 &hello->key_shares, &err))
            return err;
        /* Only send the fallback ECDH share if the corresponding CurveID is
         * enabled. */
        if (hello->key_shares.len == 2 &&
            !tcl_contains_curve(hello->supported_curves,
                                ((const TlsKeyShare *)hello->key_shares.p)[1].group))
            hello->key_shares.len = 1;
    }

    /* The QUIC transport parameters come with quic.go. */

    if (!slice_is_nil(config->encrypted_client_hello_config_list)) {
        if (config->min_version != 0 && config->min_version < TLS_VERSION_TLS13)
            return tcl_err_ech_min_version;
        if (config->max_version != 0 && config->max_version <= TLS_VERSION_TLS12)
            return tcl_err_ech_max_version;
        /* Not in Go: the rest of this, from parseECHConfigList on, comes with
         * ech.go, and until then a config list is an error. */
        return tcl_err_ech_not_here;
    }

    return BURROW_NO_ERROR;
}

Error burrow__tls_compute_and_update_psk(TlsClientHelloMsg *m, Alloc *a,
                                         Slice binder_key, Hash transcript,
                                         const TlsSuiteTLS13 *suite) {
    Error err = BURROW_NO_ERROR;
    Slice hello_bytes =
        burrow__tls_client_hello_msg_marshal_without_binders(m, a, &err);
    if (BURROW_FAILED(err))
        return err;
    (void)hash_write(transcript, hello_bytes, NULL);
    Slice binder = burrow__tls13_finished_hash(suite, a, binder_key, transcript);
    Slice psk_binders = slice_make(a, TYPE_BYTES, 1, 1);
    if (binder.p == NULL || psk_binders.p == NULL)
        return burrow_err_out_of_memory;
    ((Slice *)psk_binders.p)[0] = binder;
    if (!burrow__tls_client_hello_msg_update_binders(m, psk_binders, &err))
        return err;
    return BURROW_NO_ERROR;
}

/* Not in Go: what loadSession returns besides the session. The early secret
 * and binder key are for handshake_client_tls13.go. */
typedef struct TclLoaded {
    TlsClientSessionState *cs;
    TlsSessionState *session;
    Tls13EarlySecret early_secret;
    Slice binder_key;
} TclLoaded;

/* Not in Go. Gives back the reference to the entry and returns no session. */
static Error tcl_no_session(TclLoaded *l, TlsClientSessionState *cs) {
    tls_client_session_state_release(cs);
    l->cs = NULL;
    l->session = NULL;
    return BURROW_NO_ERROR;
}

/* loadSession. */
static Error tcl_load_session(TlsConn *c, Alloc *a, TlsClientHelloMsg *hello,
                              TclLoaded *l) {
    TlsConfig *config = c->config;
    memset(l, 0, sizeof *l);
    if (config->session_tickets_disabled || config->client_session_cache.vt == NULL)
        return BURROW_NO_ERROR;

    bool ech_inner = hello->encrypted_client_hello.len == 1 &&
                     ((const Byte *)hello->encrypted_client_hello.p)[0] == 1;

    /* ticketSupported is a TLS 1.2 extension (as TLS 1.3 replaced tickets with
     * PSK identities) and ECH requires and forces TLS 1.3. */
    hello->ticket_supported = !ech_inner;

    if (((const uint16_t *)hello->supported_versions.p)[0] == TLS_VERSION_TLS13) {
        /* Require DHE on resumption as it guarantees forward secrecy against
         * compromise of the session ticket key. See RFC 8446, Section 4.2.9. */
        Byte psk_mode_dhe = TLS_PSK_MODE_DHE;
        hello->psk_modes = tcl_bytes(a, &psk_mode_dhe, 1);
        if (hello->psk_modes.p == NULL)
            return burrow_err_out_of_memory;
    }

    /* Session resumption is not allowed if renegotiating because renegotiation
     * is primarily used to allow a client to send a client certificate, which
     * would be skipped if session resumption occurred. */
    if (c->handshakes != 0)
        return BURROW_NO_ERROR;

    /* Try to resume a previously negotiated TLS session, if available. */
    Str cache_key = burrow__tls_conn_client_session_cache_key(c, a);
    if (cache_key.len == 0)
        return BURROW_NO_ERROR;
    bool ok = false;
    TlsClientSessionState *cs =
        tls_client_session_cache_get(config->client_session_cache, cache_key, &ok);
    if (!ok || cs == NULL)
        return tcl_no_session(l, cs);
    TlsSessionState *session = cs->session;

    /* Check that version used for the previous session is still valid. */
    bool vers_ok = false;
    const uint16_t *versions = hello->supported_versions.p;
    for (Int i = 0; i < hello->supported_versions.len; i++) {
        if (versions[i] == session->version) {
            vers_ok = true;
            break;
        }
    }
    if (!vers_ok)
        return tcl_no_session(l, cs);

    Time now = burrow__tls_config_time(config);
    X509Certificate *leaf =
        *(X509Certificate **)slice_at(session->peer_certificates, 0);
    if (time_after(now, leaf->not_after)) {
        /* Expired certificate, delete the entry. */
        tls_client_session_cache_put(config->client_session_cache, cache_key, NULL);
        return tcl_no_session(l, cs);
    }
    if (!config->insecure_skip_verify) {
        if (session->verified_chains.len == 0) {
            /* The original connection had InsecureSkipVerify, while this
             * doesn't. */
            return tcl_no_session(l, cs);
        }
        if (BURROW_FAILED(
                x509_certificate_verify_hostname(leaf, config->server_name))) {
            /* This should be ensured by the cache key, but protect the
             * application from a faulty ClientSessionCache implementation. */
            return tcl_no_session(l, cs);
        }
        X509ExtKeyUsage server_auth[1] = {X509_EXT_KEY_USAGE_SERVER_AUTH};
        X509VerifyOptions opts;
        memset(&opts, 0, sizeof opts);
        opts.current_time = now;
        opts.roots = config->root_cas;
        opts.key_usages = slice_from(server_auth, 1, 1, TYPE_INT);
        if (!burrow__tls_any_valid_verified_chain(session->verified_chains, opts)) {
            /* No valid chains, delete the entry. */
            tls_client_session_cache_put(config->client_session_cache, cache_key, NULL);
            return tcl_no_session(l, cs);
        }
    }

    if (session->version != TLS_VERSION_TLS13) {
        /* In TLS 1.2 the cipher suite must match the resumed session. Ensure we
         * are still offering it. */
        if (burrow__tls_mutual_cipher_suite(hello->cipher_suites.p,
                                            hello->cipher_suites.len,
                                            session->cipher_suite) == NULL)
            return tcl_no_session(l, cs);

        /* FIPS 140-3 requires the use of Extended Master Secret. There is no
         * FIPS 140-3 mode here. */

        hello->session_ticket = session->ticket;
        l->cs = cs;
        l->session = session;
        return BURROW_NO_ERROR;
    }

    /* Check that the session ticket is not expired. */
    if (time_after(now, time_from_unix((int64_t)session->use_by, 0))) {
        tls_client_session_cache_put(config->client_session_cache, cache_key, NULL);
        return tcl_no_session(l, cs);
    }

    /* In TLS 1.3 the KDF hash must match the resumed session. Ensure we offer
     * at least one cipher suite with that hash. */
    const TlsSuiteTLS13 *cipher_suite =
        burrow__tls_cipher_suite_tls13_by_id(session->cipher_suite);
    if (cipher_suite == NULL)
        return tcl_no_session(l, cs);
    bool cipher_suite_ok = false;
    const uint16_t *offered = hello->cipher_suites.p;
    for (Int i = 0; i < hello->cipher_suites.len; i++) {
        const TlsSuiteTLS13 *offered_suite =
            burrow__tls_cipher_suite_tls13_by_id(offered[i]);
        if (offered_suite != NULL && offered_suite->hash == cipher_suite->hash) {
            cipher_suite_ok = true;
            break;
        }
    }
    if (!cipher_suite_ok)
        return tcl_no_session(l, cs);

    /* The QUIC session events and 0-RTT come with quic.go. */

    /* Set the pre_shared_key extension. See RFC 8446, Section 4.2.11.1. */
    Duration ticket_age =
        time_sub(now, time_from_unix((int64_t)session->created_at, 0));
    TlsPskIdentity identity;
    identity.label = session->ticket;
    identity.obfuscated_ticket_age =
        (uint32_t)(ticket_age / TIME_MILLISECOND) + session->age_add;
    Int hash_size = crypto_hash_size(cipher_suite->hash);
    hello->psk_identities = slice_append(a, hello->psk_identities, &identity, 1);
    hello->psk_binders = slice_make(a, TYPE_BYTES, 1, 1);
    Slice zero_binder = slice_make(a, TYPE_BYTE, hash_size, hash_size);
    if (hello->psk_identities.p == NULL || hello->psk_binders.p == NULL ||
        zero_binder.p == NULL) {
        tls_client_session_state_release(cs);
        return burrow_err_out_of_memory;
    }
    ((Slice *)hello->psk_binders.p)[0] = zero_binder;

    /* Compute the PSK binders. See RFC 8446, Section 4.2.11.2. */
    HashNewFunc hash_new = burrow__tls_suite13_hash_new(cipher_suite);
    l->early_secret = burrow__tls13_new_early_secret(a, hash_new, session->secret);
    l->binder_key =
        burrow__tls13_early_secret_resumption_binder_key(&l->early_secret, a);
    Hash transcript = hash_new(a);
    if (l->early_secret.secret.p == NULL || l->binder_key.p == NULL ||
        transcript.vt == NULL) {
        tls_client_session_state_release(cs);
        return burrow_err_out_of_memory;
    }
    Error err = burrow__tls_compute_and_update_psk(hello, a, l->binder_key, transcript,
                                                   cipher_suite);
    if (BURROW_FAILED(err)) {
        tls_client_session_state_release(cs);
        return err;
    }

    l->cs = cs;
    l->session = session;
    return BURROW_NO_ERROR;
}

/* pickTLSVersion. */
static Error tcl_pick_tls_version(TlsConn *c, const TlsServerHelloMsg *server_hello) {
    uint16_t peer_version = server_hello->vers;
    if (server_hello->supported_version != 0)
        peer_version = server_hello->supported_version;

    uint16_t vers = 0;
    if (!burrow__tls_config_mutual_version(c->config, TLS_ROLE_CLIENT, false,
                                           slice_from(&peer_version, 1, 1, TYPE_UINT16),
                                           &vers)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_PROTOCOL_VERSION);
        return fmt_errorf_v("tls: server selected unsupported protocol version %x",
                            peer_version);
    }

    c->vers = vers;
    c->have_vers = true;
    c->in.version = vers;
    c->out.version = vers;

    return BURROW_NO_ERROR;
}

static Error tcl_handshake(TclState *hs);

/* Not in Go. The part of clientHandshake after loadSession, which runs with
 * the session, if there is one, so the caller can drop it from the cache
 * when this fails. */
static Error tcl_client_handshake_with(TlsConn *c, Context ctx, Alloc *a, TclHello *h,
                                       TclLoaded *l) {
    /* Encrypted Client Hello comes with ech.go. */

    if (!burrow__tls_clone_str(tcl_keep(c), h->msg.client_hello.server_name,
                               &c->server_name))
        return burrow_err_out_of_memory;

    IoWriter none = {NULL, NULL};
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_handshake_record(c, &h->msg, none, &err);
    if (BURROW_FAILED(err))
        return err;

    /* hello.earlyData is only set for QUIC, which comes with quic.go. */

    /* serverHelloMsg is not included in the transcript */
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_read_handshake(c, none, a, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_SERVER_HELLO) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_SERVER_HELLO, msg->kind);
    }
    TlsServerHelloMsg *server_hello = &msg->server_hello;

    err = tcl_pick_tls_version(c, server_hello);
    if (BURROW_FAILED(err))
        return err;

    /* If we are negotiating a protocol version that's lower than what we
     * support, check for the server downgrade canaries. See RFC 8446, Section
     * 4.1.3. */
    uint16_t max_vers =
        burrow__tls_config_max_supported_version(c->config, TLS_ROLE_CLIENT, false);
    const Byte *canary = (const Byte *)server_hello->random.p + 24;
    bool tls12_downgrade = memcmp(canary, TLS_DOWNGRADE_CANARY_TLS12, 8) == 0;
    bool tls11_downgrade = memcmp(canary, TLS_DOWNGRADE_CANARY_TLS11, 8) == 0;
    if ((max_vers == TLS_VERSION_TLS13 && c->vers <= TLS_VERSION_TLS12 &&
         (tls12_downgrade || tls11_downgrade)) ||
        (max_vers == TLS_VERSION_TLS12 && c->vers <= TLS_VERSION_TLS11 &&
         tls11_downgrade)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tcl_err_downgrade;
    }

    if (c->vers == TLS_VERSION_TLS13) {
        TlsClientHandshakeStateTLS13 hs;
        memset(&hs, 0, sizeof hs);
        hs.c = c;
        hs.ctx = ctx;
        hs.server_hello = server_hello;
        hs.hello = &h->msg.client_hello;
        hs.key_share_keys = &h->key_share_keys;
        hs.session = l->session;
        hs.early_secret = &l->early_secret;
        hs.binder_key = l->binder_key;
        hs.server_hello_msg = msg;
        hs.hello_msg = &h->msg;
        hs.cs = l->cs;
        hs.a = a;
        err = burrow__tls_client_handshake_state_tls13_handshake(&hs);
        /* Not in Go: a resumption may have moved the session to c. */
        l->cs = hs.cs;
        return err;
    }

    TclState hs;
    memset(&hs, 0, sizeof hs);
    hs.c = c;
    hs.ctx = ctx;
    hs.server_hello_msg = msg;
    hs.server_hello = server_hello;
    hs.hello_msg = &h->msg;
    hs.hello = &h->msg.client_hello;
    hs.session = l->session;
    hs.cs = l->cs;
    hs.ticket = slice_nil(TYPE_BYTE);
    hs.a = a;
    err = tcl_handshake(&hs);
    /* processServerHello may have moved the session to c. */
    l->cs = hs.cs;
    return err;
}

Error burrow__tls_client_handshake(TlsConn *c, Context ctx) {
    if (c->config == NULL)
        c->config = burrow__tls_default_config();

    /* This may be a renegotiation handshake, in which case some fields need to
     * be reset. */
    c->did_resume = false;
    c->curve_id = 0;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    TclHello h;
    memset(&h, 0, sizeof h);
    Error err = tcl_make_client_hello(c, a, &h);
    if (BURROW_FAILED(err)) {
        burrow__tls_key_share_private_keys_free(&h.key_share_keys);
        arena_free(&ar);
        return err;
    }

    TclLoaded l;
    err = tcl_load_session(c, a, &h.msg.client_hello, &l);
    if (BURROW_FAILED(err)) {
        burrow__tls_key_share_private_keys_free(&h.key_share_keys);
        arena_free(&ar);
        return err;
    }
    bool had_session = l.session != NULL;

    err = tcl_client_handshake_with(c, ctx, a, &h, &l);
    if (had_session && BURROW_FAILED(err)) {
        /* If we got a handshake failure when resuming a session, throw away
         * the session ticket. See RFC 5077, Section 3.2.
         *
         * RFC 8446 makes no mention of dropping tickets on failure, but it does
         * require servers to abort on invalid binders, so we need to delete
         * tickets to recover from a corrupted PSK. */
        Str cache_key = burrow__tls_conn_client_session_cache_key(c, a);
        if (cache_key.len > 0)
            tls_client_session_cache_put(c->config->client_session_cache, cache_key,
                                         NULL);
    }

    if (l.cs != NULL)
        tls_client_session_state_release(l.cs);
    burrow__tls_key_share_private_keys_free(&h.key_share_keys);
    arena_free(&ar);
    return err;
}

static Error tcl_process_server_hello(TclState *hs, bool *is_resume);
static Error tcl_do_full_handshake(TclState *hs);
static Error tcl_establish_keys(TclState *hs);
static Error tcl_read_session_ticket(TclState *hs);
static Error tcl_read_finished(TclState *hs, Byte out[12]);
static Error tcl_save_session_ticket(TclState *hs);
static Error tcl_send_finished(TclState *hs, Byte out[12]);

/* The resumption half of handshake. */
static Error tcl_resume(TclState *hs) {
    TlsConn *c = hs->c;
    Error err = tcl_establish_keys(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tcl_read_session_ticket(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tcl_read_finished(hs, c->server_finished);
    if (BURROW_FAILED(err))
        return err;
    c->client_finished_is_first = false;
    /* Make sure the connection is still being verified whether or not this is
     * a resumption. Resumptions currently don't reverify certificates so they
     * don't call verifyServerCertificate. See Issue 31641. */
    if (c->config->verify_connection.f != NULL) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        err = BURROW_CALLF(c->config->verify_connection, &cs);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
            return err;
        }
    }
    err = tcl_send_finished(hs, c->client_finished);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__tls_conn_flush(c, &err);
    return err;
}

/* The full half of handshake. */
static Error tcl_full(TclState *hs) {
    TlsConn *c = hs->c;
    Error err = tcl_do_full_handshake(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tcl_establish_keys(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tcl_send_finished(hs, c->client_finished);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__tls_conn_flush(c, &err);
    if (BURROW_FAILED(err))
        return err;
    c->client_finished_is_first = true;
    err = tcl_read_session_ticket(hs);
    if (BURROW_FAILED(err))
        return err;
    return tcl_read_finished(hs, c->server_finished);
}

/* Does the handshake, either a full one or resumes old session. Requires hs.c,
 * hs.hello, hs.serverHello, and, optionally, hs.session to be set. */
static Error tcl_handshake(TclState *hs) {
    TlsConn *c = hs->c;

    /* If we did not load a session (hs.session == nil), but we did set a
     * session ID in the transmitted client hello (hs.hello.sessionId != nil),
     * it means we tried to negotiate TLS 1.3 and sent a random session ID as a
     * compatibility measure (see RFC 8446, Section 4.1.2).
     *
     * Since we're now handshaking for TLS 1.2, if the server echoed the
     * transmitted ID back to us, we know mischief is afoot: the session ID was
     * random and can't possibly be recognized by the server. */
    if (hs->session == NULL && !slice_is_nil(hs->hello->session_id) &&
        bytes_equal(hs->hello->session_id, hs->server_hello->session_id)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tcl_err_echoed_session_id;
    }

    bool is_resume = false;
    Error err = tcl_process_server_hello(hs, &is_resume);
    if (BURROW_FAILED(err))
        return err;

    if (!burrow__tls_new_finished_hash(hs->a, c->vers, hs->suite, &hs->finished_hash))
        return burrow_err_out_of_memory;

    /* No signatures of the handshake are needed in a resumption. Otherwise, in
     * a full handshake, if we don't have any certificates configured then we
     * will never send a CertificateVerify message and thus no signatures are
     * needed in that case either. */
    if (is_resume || (c->config->certificates.len == 0 &&
                      c->config->get_client_certificate.f == NULL))
        burrow__tls_finished_hash_discard_handshake_buffer(&hs->finished_hash);

    IoWriter transcript = burrow__tls_finished_hash_writer(&hs->finished_hash);
    err = burrow__tls_transcript_msg(hs->hello_msg, transcript, hs->a);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__tls_transcript_msg(hs->server_hello_msg, transcript, hs->a);
    if (BURROW_FAILED(err))
        return err;

    c->buffering = true;
    c->did_resume = is_resume;
    err = is_resume ? tcl_resume(hs) : tcl_full(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tcl_save_session_ticket(hs);
    if (BURROW_FAILED(err))
        return err;

    TlsEkmMaster m = {c->vers, hs->suite, hs->master_secret, hs->hello->random,
                      hs->server_hello->random};
    c->ekm = burrow__tls_ekm_func(tcl_keep(c), &m);
    if (c->ekm.f == NULL)
        return burrow_err_out_of_memory;
    sync_atomic_bool_store(&c->is_handshake_complete, true);

    return BURROW_NO_ERROR;
}

/* pickCipherSuite. */
static Error tcl_pick_cipher_suite(TclState *hs) {
    hs->suite = burrow__tls_mutual_cipher_suite(hs->hello->cipher_suites.p,
                                                hs->hello->cipher_suites.len,
                                                hs->server_hello->cipher_suite);
    if (hs->suite == NULL) {
        (void)burrow__tls_conn_send_alert(hs->c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tcl_err_unconfigured_suite;
    }

    hs->c->cipher_suite = hs->suite->id;
    return BURROW_NO_ERROR;
}

/* Not in Go. The next handshake message, written to the finished hash. */
static Error tcl_read(TclState *hs, TlsHandshakeMsg *msg) {
    return burrow__tls_conn_read_handshake(
        hs->c, burrow__tls_finished_hash_writer(&hs->finished_hash), hs->a, msg);
}

/* Not in Go. msg written to the connection and the finished hash. */
static Error tcl_write(TclState *hs, const TlsHandshakeMsg *msg) {
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_handshake_record(
        hs->c, msg, burrow__tls_finished_hash_writer(&hs->finished_hash), &err);
    return err;
}

/* Not in Go. The CertificateVerify doFullHandshake sends, signed with key. */
static Error tcl_certificate_verify(TclState *hs, const TlsCertificate *chain_to_send,
                                    const TlsCertificateRequestMsg *cert_req) {
    TlsConn *c = hs->c;
    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_VERIFY);
    TlsCertificateVerifyMsg *cert_verify = &msg.certificate_verify;

    Ed25519Signer es;
    CryptoSigner key;
    if (!burrow__tls_private_key_signer(chain_to_send->private_key, &es, &key)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return fmt_errorf_v("tls: client certificate private key of type %T does not "
                            "implement crypto.Signer",
                            chain_to_send->private_key);
    }

    Error err = BURROW_NO_ERROR;
    IoReader rand = burrow__tls_config_rand(c->config);
    if (c->vers >= TLS_VERSION_TLS12) {
        TlsSignatureScheme signature_algorithm = burrow__tls_select_signature_scheme(
            c->vers, chain_to_send, cert_req->supported_signature_algorithms, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
            return err;
        }
        uint8_t sig_type;
        CryptoHash sig_hash;
        if (!burrow__tls_type_and_hash_from_signature_scheme(
                signature_algorithm, &sig_type, &sig_hash, NULL))
            return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        cert_verify->has_signature_algorithm = true;
        cert_verify->signature_algorithm = signature_algorithm;
        /* Go counts the tlssha1 GODEBUG setting here when sigHash is SHA-1. */
        if (slice_is_nil(hs->finished_hash.buffer)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return tcl_err_no_transcript;
        }
        /* Not in Go: a transcript that could not all be kept. */
        if (hs->finished_hash.oom) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return burrow_err_out_of_memory;
        }
        CryptoSignerOpts sign_opts = crypto_hash_as_signer_opts(&sig_hash);
        RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, sig_hash};
        if (sig_type == TLS_SIGNATURE_RSAPSS)
            sign_opts = rsa_pss_options_as_signer_opts(&pss);
        cert_verify->signature = crypto_sign_message(
            hs->a, key, rand, hs->finished_hash.buffer, sign_opts, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }
    } else {
        uint8_t sig_type;
        CryptoHash sig_hash;
        if (!burrow__tls_legacy_type_and_hash_from_public_key(
                crypto_signer_public(key), &sig_type, &sig_hash, &err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return err;
        }
        Slice signed_ = burrow__tls_finished_hash_for_client_certificate(
            &hs->finished_hash, hs->a, sig_type);
        if (signed_.p == NULL)
            return burrow_err_out_of_memory;
        cert_verify->signature = crypto_signer_sign(
            key, hs->a, rand, signed_, crypto_hash_as_signer_opts(&sig_hash), &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }
    }

    return tcl_write(hs, &msg);
}

static TlsCertificateRequestInfo *
tcl_certificate_request_info_from_msg(Alloc *a, Context ctx, uint16_t vers,
                                      const TlsCertificateRequestMsg *cert_req);

/* What doFullHandshake does once it has the server's messages up to
 * ServerHelloDone, with the key agreement they set up. */
static Error tcl_finish_full_handshake(TclState *hs, TlsKeyAgreement *key_agreement,
                                       const TlsCertificate *chain_to_send,
                                       const TlsCertificateRequestMsg *cert_req) {
    TlsConn *c = hs->c;
    Error err = BURROW_NO_ERROR;

    /* If the server requested a certificate then we have to send a Certificate
     * message, even if it's empty because we don't have a certificate to
     * send.
     *
     * Not in Go: chain_to_send is only set when it did, so it stands for
     * certRequested. */
    if (chain_to_send != NULL) {
        TlsHandshakeMsg cert_msg;
        burrow__tls_handshake_msg_init(&cert_msg, TLS_MSG_CERTIFICATE);
        cert_msg.certificate.certificates = chain_to_send->certificate;
        err = tcl_write(hs, &cert_msg);
        if (BURROW_FAILED(err))
            return err;
    }

    X509Certificate *peer = ((X509Certificate *const *)c->peer_certificates.p)[0];
    TlsClientKeyExchangeMsg *ckx = NULL;
    Slice pre_master_secret = burrow__tls_key_agreement_generate_client_key_exchange(
        key_agreement, hs->a, c->config, hs->hello, peer, &ckx, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    if (ckx != NULL) {
        TlsHandshakeMsg ckx_msg;
        burrow__tls_handshake_msg_init(&ckx_msg, TLS_MSG_CLIENT_KEY_EXCHANGE);
        ckx_msg.client_key_exchange = *ckx;
        err = tcl_write(hs, &ckx_msg);
        if (BURROW_FAILED(err))
            return err;
    }

    if (hs->server_hello->extended_master_secret) {
        c->ext_master_secret = true;
        hs->master_secret = burrow__tls_ext_master_from_pre_master_secret(
            hs->a, c->vers, hs->suite, pre_master_secret,
            burrow__tls_finished_hash_sum(&hs->finished_hash, hs->a));
    } else {
        /* FIPS 140-3 requires the use of Extended Master Secret. There is no
         * FIPS 140-3 mode here. */
        hs->master_secret = burrow__tls_master_from_pre_master_secret(
            hs->a, c->vers, hs->suite, pre_master_secret, hs->hello->random,
            hs->server_hello->random);
    }
    if (hs->master_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_config_write_key_log(c->config, BURROW_S(TLS_KEY_LOG_LABEL_TLS12),
                                           hs->hello->random, hs->master_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return fmt_errorf_v("tls: failed to write to key log: %v", err);
    }

    if (chain_to_send != NULL && chain_to_send->certificate.len > 0) {
        err = tcl_certificate_verify(hs, chain_to_send, cert_req);
        if (BURROW_FAILED(err))
            return err;
    }

    burrow__tls_finished_hash_discard_handshake_buffer(&hs->finished_hash);

    return BURROW_NO_ERROR;
}

/* The part of doFullHandshake after the Certificate and CertificateStatus
 * messages, with msg the message after them. */
static Error tcl_full_handshake_key_exchange(TclState *hs, TlsHandshakeMsg *msg,
                                             TlsKeyAgreement *key_agreement) {
    TlsConn *c = hs->c;
    Error err = BURROW_NO_ERROR;

    if (msg->kind == TLS_MSG_SERVER_KEY_EXCHANGE) {
        X509Certificate *peer = ((X509Certificate *const *)c->peer_certificates.p)[0];
        err = burrow__tls_key_agreement_process_server_key_exchange(
            key_agreement, hs->a, c->config, hs->hello, hs->server_hello, peer,
            &msg->server_key_exchange);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return err;
        }
        if (key_agreement->is_ecdhe) {
            c->curve_id = key_agreement->curve_id;
            c->peer_sig_alg = key_agreement->signature_algorithm;
        }

        err = tcl_read(hs, msg);
        if (BURROW_FAILED(err))
            return err;
    }

    TlsCertificate *chain_to_send = NULL;
    TlsCertificateRequestMsg *cert_req = NULL;
    if (msg->kind == TLS_MSG_CERTIFICATE_REQUEST) {
        cert_req = BURROW_NEW(hs->a, TlsCertificateRequestMsg);
        if (cert_req == NULL)
            return burrow_err_out_of_memory;
        *cert_req = msg->certificate_request;

        TlsCertificateRequestInfo *cri =
            tcl_certificate_request_info_from_msg(hs->a, hs->ctx, c->vers, cert_req);
        if (cri == NULL)
            return burrow_err_out_of_memory;
        chain_to_send = burrow__tls_conn_get_client_certificate(c, hs->a, cri, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }

        err = tcl_read(hs, msg);
        if (BURROW_FAILED(err))
            return err;
    }

    if (chain_to_send != NULL) {
        /* Not in Go: the certificate only has to last for the handshake, so c
         * keeps a copy. */
        if (!burrow__tls_clone_bytes_list(tcl_keep(c), chain_to_send->certificate,
                                          &c->local_certificate))
            return burrow_err_out_of_memory;
    }

    if (msg->kind != TLS_MSG_SERVER_HELLO_DONE) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_SERVER_HELLO_DONE,
                                                    msg->kind);
    }

    return tcl_finish_full_handshake(hs, key_agreement, chain_to_send, cert_req);
}

/* doFullHandshake. */
static Error tcl_do_full_handshake(TclState *hs) {
    TlsConn *c = hs->c;

    TlsHandshakeMsg *cert_msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (cert_msg == NULL || msg == NULL)
        return burrow_err_out_of_memory;
    Error err = tcl_read(hs, cert_msg);
    if (BURROW_FAILED(err))
        return err;
    if (cert_msg->kind != TLS_MSG_CERTIFICATE ||
        cert_msg->certificate.certificates.len == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE,
                                                    cert_msg->kind);
    }
    Slice certificates = cert_msg->certificate.certificates;

    err = tcl_read(hs, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind == TLS_MSG_CERTIFICATE_STATUS) {
        /* RFC4366 on Certificate Status Request: The server MAY return a
         * "certificate_status" message. */

        if (!hs->server_hello->ocsp_stapling) {
            /* If a server returns a "CertificateStatus" message, then the
             * server MUST have included an extension of type "status_request"
             * with empty "extension_data" in the extended server hello. */

            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
            return tcl_err_unexpected_status;
        }

        c->ocsp_response = bytes_clone(tcl_keep(c), msg->certificate_status.response);
        if (c->ocsp_response.p == NULL && msg->certificate_status.response.len > 0)
            return burrow_err_out_of_memory;

        err = tcl_read(hs, msg);
        if (BURROW_FAILED(err))
            return err;
    }

    if (c->handshakes == 0) {
        /* If this is the first handshake on a connection, process and
         * (optionally) verify the server's certificates. */
        err = burrow__tls_conn_verify_server_certificate(c, certificates);
        if (BURROW_FAILED(err))
            return err;
    } else {
        /* This is a renegotiation handshake. We require that the server's
         * identity (i.e. leaf certificate) is unchanged and thus any previous
         * trust decision is still valid.
         *
         * See https://mitls.org/pages/attacks/3SHAKE for the motivation behind
         * this requirement. */
        const X509Certificate *peer =
            ((X509Certificate *const *)c->peer_certificates.p)[0];
        if (!bytes_equal(peer->raw, ((const Slice *)certificates.p)[0])) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
            return tcl_err_identity_changed;
        }
    }

    TlsKeyAgreement key_agreement;
    burrow__tls_key_agreement_init(&key_agreement, hs->suite, c->vers);
    err = tcl_full_handshake_key_exchange(hs, msg, &key_agreement);
    burrow__tls_key_agreement_free(&key_agreement);
    return err;
}

/* establishKeys. */
static Error tcl_establish_keys(TclState *hs) {
    TlsConn *c = hs->c;
    const TlsSuite *suite = hs->suite;

    TlsKeys k;
    if (!burrow__tls_keys_from_master_secret(hs->a, c->vers, suite, hs->master_secret,
                                             hs->hello->random,
                                             hs->server_hello->random, suite->mac_len,
                                             suite->key_len, suite->iv_len, &k))
        return burrow_err_out_of_memory;
    /* Not in Go: each half conn makes its cipher and MAC in memory of its own,
     * which can run out. */
    Alloc *in = burrow__tls_half_conn_next_alloc(&c->in);
    Alloc *out = burrow__tls_half_conn_next_alloc(&c->out);
    if (in == NULL || out == NULL)
        return burrow_err_out_of_memory;
    TlsCipher client_cipher, server_cipher;
    Hash client_hash = {NULL, NULL}, server_hash = {NULL, NULL};
    if (suite->cipher != NULL) {
        client_cipher =
            suite->cipher(out, k.client_key, k.client_iv, false /* not for reading */);
        client_hash = suite->mac(out, k.client_mac);
        server_cipher =
            suite->cipher(in, k.server_key, k.server_iv, true /* for reading */);
        server_hash = suite->mac(in, k.server_mac);
        if (client_hash.vt == NULL || server_hash.vt == NULL)
            return burrow_err_out_of_memory;
    } else {
        client_cipher = suite->aead(out, k.client_key, k.client_iv);
        server_cipher = suite->aead(in, k.server_key, k.server_iv);
    }
    if (burrow__tls_cipher_is_nil(client_cipher) ||
        burrow__tls_cipher_is_nil(server_cipher))
        return burrow_err_out_of_memory;

    burrow__tls_half_conn_prepare_cipher_spec(&c->in, c->vers, server_cipher,
                                              server_hash);
    burrow__tls_half_conn_prepare_cipher_spec(&c->out, c->vers, client_cipher,
                                              client_hash);
    return BURROW_NO_ERROR;
}

/* serverResumedSession. */
static bool tcl_server_resumed_session(const TclState *hs) {
    /* If the server responded with the same sessionId then it means the
     * sessionTicket is being used to resume a TLS session. */
    return hs->session != NULL && !slice_is_nil(hs->hello->session_id) &&
           bytes_equal(hs->server_hello->session_id, hs->hello->session_id);
}

/* processServerHello. */
static Error tcl_process_server_hello(TclState *hs, bool *is_resume) {
    TlsConn *c = hs->c;
    *is_resume = false;

    Error err = tcl_pick_cipher_suite(hs);
    if (BURROW_FAILED(err))
        return err;

    if (hs->server_hello->compression_method != TLS_COMPRESSION_NONE) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tcl_err_compression;
    }

    bool supports_point_format = false;
    bool offered_non_compressed_format = false;
    const Byte *formats = hs->server_hello->supported_points.p;
    for (Int i = 0; i < hs->server_hello->supported_points.len; i++) {
        if (formats[i] == TLS_POINT_FORMAT_UNCOMPRESSED)
            supports_point_format = true;
        else
            offered_non_compressed_format = true;
    }
    if (!supports_point_format && offered_non_compressed_format)
        return tcl_err_point_formats;

    if (c->handshakes == 0 && hs->server_hello->secure_renegotiation_supported) {
        c->secure_renegotiation = true;
        if (hs->server_hello->secure_renegotiation.len != 0) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
            return tcl_err_initial_renegotiation;
        }
    }

    if (c->handshakes > 0 && c->secure_renegotiation) {
        Byte expected_secure_renegotiation[24];
        memcpy(expected_secure_renegotiation, c->client_finished, 12);
        memcpy(expected_secure_renegotiation + 12, c->server_finished, 12);
        if (!bytes_equal(
                hs->server_hello->secure_renegotiation,
                slice_from(expected_secure_renegotiation, 24, 24, TYPE_BYTE))) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
            return tcl_err_renegotiation_contents;
        }
    }

    err = burrow__tls_check_alpn(hs->hello->alpn_protocols,
                                 hs->server_hello->alpn_protocol, false);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return err;
    }
    if (!burrow__tls_clone_str(tcl_keep(c), hs->server_hello->alpn_protocol,
                               &c->client_protocol))
        return burrow_err_out_of_memory;

    if (!burrow__tls_clone_bytes_list(tcl_keep(c), hs->server_hello->scts, &c->scts))
        return burrow_err_out_of_memory;

    if (!tcl_server_resumed_session(hs))
        return BURROW_NO_ERROR;

    if (hs->session->version != c->vers) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tcl_err_resumed_version;
    }

    if (hs->session->cipher_suite != hs->suite->id) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tcl_err_resumed_suite;
    }

    /* RFC 7627, Section 5.3 */
    if (hs->session->ext_master_secret != hs->server_hello->extended_master_secret) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tcl_err_resumed_ems;
    }

    /* Restore master secret and certificates from previous state */
    hs->master_secret = hs->session->secret;
    c->ext_master_secret = hs->session->ext_master_secret;
    c->peer_certificates = hs->session->peer_certificates;
    c->verified_chains = hs->session->verified_chains;
    c->ocsp_response = hs->session->ocsp_response;
    /* Let the ServerHello SCTs override the session SCTs from the original
     * connection, if any are provided. */
    if (c->scts.len == 0 && hs->session->scts.len != 0)
        c->scts = hs->session->scts;
    c->curve_id = hs->session->curve_id;
    /* Not in Go: c now points into the session, so c holds on to it. Resuming
     * is only done on the first handshake, so there is none there already. */
    if (c->resumed_session != NULL)
        tls_client_session_state_release(c->resumed_session);
    c->resumed_session = hs->cs;
    hs->cs = NULL;

    *is_resume = true;
    return BURROW_NO_ERROR;
}

/* checkALPN ensure that the server's choice of ALPN protocol is compatible with
 * the protocols that we advertised in the ClientHello. */
Error burrow__tls_check_alpn(Slice client_protos, Str server_proto, bool quic) {
    if (server_proto.len == 0) {
        if (quic && client_protos.len > 0) {
            /* RFC 9001, Section 8.1 */
            return tcl_err_no_alpn;
        }
        return BURROW_NO_ERROR;
    }
    if (client_protos.len == 0)
        return tcl_err_unrequested_alpn;
    const Str *protos = client_protos.p;
    for (Int i = 0; i < client_protos.len; i++) {
        if (str_eq(protos[i], server_proto))
            return BURROW_NO_ERROR;
    }
    return tcl_err_unadvertised_alpn;
}

/* readFinished. */
static Error tcl_read_finished(TclState *hs, Byte out[12]) {
    TlsConn *c = hs->c;

    Error err = burrow__tls_conn_read_change_cipher_spec(c);
    if (BURROW_FAILED(err))
        return err;

    /* finishedMsg is included in the transcript, but not until after we check
     * the client version, since the state before this message was sent is used
     * during verification. */
    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    IoWriter none = {NULL, NULL};
    err = burrow__tls_conn_read_handshake(c, none, hs->a, msg);
    if (BURROW_FAILED(err))
        return err;
    if (msg->kind != TLS_MSG_FINISHED) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_FINISHED, msg->kind);
    }
    const TlsFinishedMsg *server_finished = &msg->finished;

    Slice verify = burrow__tls_finished_hash_server_sum(&hs->finished_hash, hs->a,
                                                        hs->master_secret);
    if (verify.p == NULL)
        return burrow_err_out_of_memory;
    if (verify.len != server_finished->verify_data.len ||
        subtle_constant_time_compare(verify, server_finished->verify_data) != 1) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tcl_err_finished;
    }

    err = burrow__tls_transcript_msg(
        msg, burrow__tls_finished_hash_writer(&hs->finished_hash), hs->a);
    if (BURROW_FAILED(err))
        return err;

    memcpy(out, verify.p, (size_t)(verify.len < 12 ? verify.len : 12));
    return BURROW_NO_ERROR;
}

/* readSessionTicket. */
static Error tcl_read_session_ticket(TclState *hs) {
    if (!hs->server_hello->ticket_supported)
        return BURROW_NO_ERROR;
    TlsConn *c = hs->c;

    if (!hs->hello->ticket_supported) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tcl_err_unrequested_ticket;
    }

    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = tcl_read(hs, msg);
    if (BURROW_FAILED(err))
        return err;
    if (msg->kind != TLS_MSG_NEW_SESSION_TICKET) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_NEW_SESSION_TICKET,
                                                    msg->kind);
    }

    hs->ticket = msg->new_session_ticket.ticket;
    return BURROW_NO_ERROR;
}

/* saveSessionTicket.
 *
 * Not in Go: with no memory for the session, the ticket is dropped, which a
 * client is free to do, and the connection goes on. */
static Error tcl_save_session_ticket(TclState *hs) {
    if (slice_is_nil(hs->ticket))
        return BURROW_NO_ERROR;
    TlsConn *c = hs->c;

    Str cache_key = burrow__tls_conn_client_session_cache_key(c, hs->a);
    if (cache_key.len == 0)
        return BURROW_NO_ERROR;

    /* Not in Go: the session outlives c in the cache, so it comes from the
     * config's allocator, which any goroutine can use. */
    Alloc *sa = c->config->a != NULL ? c->config->a : heap_allocator();
    Error err = BURROW_NO_ERROR;
    TlsSessionState *session = burrow__tls_conn_session_state(c, sa, &err);
    if (session == NULL)
        return BURROW_NO_ERROR;
    session->secret =
        bytes_clone(tls_session_state_allocator(session), hs->master_secret);
    if (session->secret.p == NULL) {
        tls_session_state_free(session);
        return BURROW_NO_ERROR;
    }

    TlsClientSessionState *cs = tls_new_resumption_state(hs->ticket, session, &err);
    if (cs == NULL)
        return BURROW_NO_ERROR;
    tls_client_session_cache_put(c->config->client_session_cache, cache_key, cs);
    tls_client_session_state_release(cs);
    return BURROW_NO_ERROR;
}

/* sendFinished. */
static Error tcl_send_finished(TclState *hs, Byte out[12]) {
    TlsConn *c = hs->c;

    Error err = burrow__tls_conn_write_change_cipher_record(c);
    if (BURROW_FAILED(err))
        return err;

    TlsHandshakeMsg finished;
    burrow__tls_handshake_msg_init(&finished, TLS_MSG_FINISHED);
    finished.finished.verify_data = burrow__tls_finished_hash_client_sum(
        &hs->finished_hash, hs->a, hs->master_secret);
    if (finished.finished.verify_data.p == NULL)
        return burrow_err_out_of_memory;
    err = tcl_write(hs, &finished);
    if (BURROW_FAILED(err))
        return err;
    Slice v = finished.finished.verify_data;
    memcpy(out, v.p, (size_t)(v.len < 12 ? v.len : 12));
    return BURROW_NO_ERROR;
}

/* checkKeySize. */
static bool tcl_check_key_size(Int n, Int *max) {
    Int m;
    if (burrow__tls_godebug_max_rsa_size(&m)) {
        /* Go counts the tlsmaxrsasize GODEBUG setting here when it changes
         * the outcome. */
        *max = m;
        return n <= m;
    }
    *max = TCL_DEFAULT_MAX_RSA_KEY_SIZE;
    return n <= TCL_DEFAULT_MAX_RSA_KEY_SIZE;
}

/* The CertificateVerificationError verifyServerCertificate returns. */
static Error tcl_verification_error(Slice certs, Error err) {
    TlsCertificateVerificationError e = {certs, err};
    return tls_certificate_verification_error_as_error(&e, error_allocator());
}

/* Not in Go. The x509.Verify verifyServerCertificate does, with what is
 * given, and the chains it makes go to c. */
static Error tcl_verify_chains(TlsConn *c, Slice certs, Str dns_name) {
    X509Certificate *const *list = certs.p;
    X509CertPool *intermediates = x509_new_cert_pool(heap_allocator());
    if (intermediates == NULL)
        return burrow_err_out_of_memory;
    for (Int i = 1; i < certs.len; i++)
        x509_cert_pool_add_cert(intermediates, list[i]);
    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.roots = c->config->root_cas;
    opts.current_time = burrow__tls_config_time(c->config);
    opts.dns_name = dns_name;
    opts.intermediates = intermediates;

    Error err = BURROW_NO_ERROR;
    Slice chains = x509_certificate_verify(list[0], tcl_keep(c), opts, &err);
    x509_cert_pool_free(intermediates);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
        return tcl_verification_error(certs, err);
    }

    c->verified_chains = burrow__tls_fips_allowed_chains(chains, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
        return tcl_verification_error(certs, err);
    }
    return BURROW_NO_ERROR;
}

/* verifyServerCertificate parses and verifies the provided chain, setting
 * c.verifiedChains and c.peerCertificates or sending the appropriate alert.
 *
 * Not in Go: there is no certificate cache, so each one is parsed again, into
 * c's own memory along with a copy of its bytes. */
Error burrow__tls_conn_verify_server_certificate(TlsConn *c, Slice certificates) {
    Alloc *keep = tcl_keep(c);
    Slice certs =
        slice_make(keep, TYPE_X509_CERTIFICATE_PTR, certificates.len, certificates.len);
    if (certs.p == NULL && certificates.len > 0)
        return burrow_err_out_of_memory;
    for (Int i = 0; i < certificates.len; i++) {
        Slice asn1_data = bytes_clone(keep, ((const Slice *)certificates.p)[i]);
        if (asn1_data.p == NULL && ((const Slice *)certificates.p)[i].len > 0)
            return burrow_err_out_of_memory;
        Error err = BURROW_NO_ERROR;
        X509Certificate *cert = x509_parse_certificate(keep, asn1_data, &err);
        if (cert == NULL) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
            return fmt_errorf_v("tls: failed to parse certificate from server: %v",
                                err);
        }
        if (cert->public_key_algorithm == X509_RSA) {
            const RsaPublicKey *pub = cert->public_key.data;
            Int max;
            if (!tcl_check_key_size(big_int_bit_len(pub->n), &max)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
                return fmt_errorf_v("tls: server sent certificate containing RSA key "
                                    "larger than %d bits",
                                    max);
            }
        }
        ((X509Certificate **)certs.p)[i] = cert;
    }
    X509Certificate *leaf = *(X509Certificate **)slice_at(certs, 0);

    bool ech_rejected = !slice_is_nil(c->config->encrypted_client_hello_config_list) &&
                        !c->ech_accepted;
    Error err = BURROW_NO_ERROR;
    if (ech_rejected) {
        if (c->config->encrypted_client_hello_rejection_verify.f != NULL) {
            TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
            err = BURROW_CALLF(c->config->encrypted_client_hello_rejection_verify, &cs);
            if (BURROW_FAILED(err)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
                return err;
            }
        } else {
            err = tcl_verify_chains(c, certs, c->server_name);
            if (BURROW_FAILED(err))
                return err;
        }
    } else if (!c->config->insecure_skip_verify) {
        err = tcl_verify_chains(c, certs, c->config->server_name);
        if (BURROW_FAILED(err))
            return err;
    }

    const Type *t = leaf->public_key.t;
    if (t == TYPE_RSA_PUBLIC_KEY || t == TYPE_ECDSA_PUBLIC_KEY ||
        t == TYPE_ED25519_PUBLIC_KEY) {
    } else if (t == TYPE_MLDSA_PUBLIC_KEY) {
        if (c->vers < TLS_VERSION_TLS13) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return tcl_err_mldsa_tls12;
        }
    } else {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_CERTIFICATE);
        return fmt_errorf_v("tls: server's certificate contains an unsupported type of "
                            "public key: %T",
                            leaf->public_key);
    }

    c->peer_certificates = certs;

    if (c->config->verify_peer_certificate.f != NULL && !ech_rejected) {
        err = BURROW_CALLF(c->config->verify_peer_certificate, certificates,
                           c->verified_chains);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
            return err;
        }
    }

    if (c->config->verify_connection.f != NULL && !ech_rejected) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        err = BURROW_CALLF(c->config->verify_connection, &cs);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
            return err;
        }
    }

    return BURROW_NO_ERROR;
}

/* Not in Go. schemes as a []SignatureScheme from a. */
static Slice tcl_schemes(Alloc *a, const TlsSignatureScheme *schemes, Int n) {
    Slice s = slice_make(a, TYPE_UINT16, n, n);
    if (s.p != NULL)
        memcpy(s.p, schemes, (size_t)n * sizeof schemes[0]);
    return s;
}

/* certificateRequestInfoFromMsg generates a CertificateRequestInfo from a TLS
 * <= 1.2 CertificateRequest, making an effort to fill in missing information.
 *
 * Not in Go: NULL when a is out of memory. */
static TlsCertificateRequestInfo *
tcl_certificate_request_info_from_msg(Alloc *a, Context ctx, uint16_t vers,
                                      const TlsCertificateRequestMsg *cert_req) {
    TlsCertificateRequestInfo *cri = BURROW_NEW(a, TlsCertificateRequestInfo);
    if (cri == NULL)
        return NULL;
    cri->acceptable_cas = cert_req->certificate_authorities;
    cri->version = vers;
    cri->ctx = ctx;
    cri->signature_schemes = slice_nil(TYPE_UINT16);

    bool rsa_avail = false, ec_avail = false;
    const Byte *types = cert_req->certificate_types.p;
    for (Int i = 0; i < cert_req->certificate_types.len; i++) {
        switch (types[i]) {
        case TLS_CERT_TYPE_RSA_SIGN:
            rsa_avail = true;
            break;
        case TLS_CERT_TYPE_ECDSA_SIGN:
            ec_avail = true;
            break;
        default:
            break;
        }
    }

    if (!cert_req->has_signature_algorithm) {
        /* Prior to TLS 1.2, signature schemes did not exist. In this case we
         * make up a list based on the acceptable certificate types, to help
         * GetClientCertificate and SupportsCertificate select the right
         * certificate. The hash part of the SignatureScheme is a lie here,
         * because TLS 1.0 and 1.1 always use MD5+SHA1 for RSA and SHA1 for
         * ECDSA. */
        static const TlsSignatureScheme both[] = {
            TLS_ECDSA_WITH_P256_AND_SHA256,
            TLS_ECDSA_WITH_P384_AND_SHA384,
            TLS_ECDSA_WITH_P521_AND_SHA512,
            TLS_PKCS1_WITH_SHA256,
            TLS_PKCS1_WITH_SHA384,
            TLS_PKCS1_WITH_SHA512,
            TLS_PKCS1_WITH_SHA1,
        };
        if (rsa_avail && ec_avail)
            cri->signature_schemes = tcl_schemes(a, both, 7);
        else if (rsa_avail)
            cri->signature_schemes = tcl_schemes(a, both + 3, 4);
        else if (ec_avail)
            cri->signature_schemes = tcl_schemes(a, both, 3);
        if ((rsa_avail || ec_avail) && cri->signature_schemes.p == NULL)
            return NULL;
        return cri;
    }

    /* Filter the signature schemes based on the certificate types. See RFC
     * 5246, Section 7.4.4 (where it calls this "somewhat complicated"). */
    Int n = cert_req->supported_signature_algorithms.len;
    cri->signature_schemes = slice_make(a, TYPE_UINT16, 0, n);
    if (cri->signature_schemes.p == NULL && n > 0)
        return NULL;
    const TlsSignatureScheme *schemes = cert_req->supported_signature_algorithms.p;
    TlsSignatureScheme *out = cri->signature_schemes.p;
    for (Int i = 0; i < n; i++) {
        TlsSignatureScheme sig_scheme = schemes[i];
        uint8_t sig_type;
        CryptoHash hash;
        if (!burrow__tls_type_and_hash_from_signature_scheme(sig_scheme, &sig_type,
                                                             &hash, NULL))
            continue;
        switch (sig_type) {
        case TLS_SIGNATURE_ECDSA:
        case TLS_SIGNATURE_ED25519:
            if (ec_avail)
                out[cri->signature_schemes.len++] = sig_scheme;
            break;
        case TLS_SIGNATURE_RSAPSS:
        case TLS_SIGNATURE_PKCS1V15:
            if (rsa_avail)
                out[cri->signature_schemes.len++] = sig_scheme;
            break;
        default:
            break;
        }
    }

    return cri;
}

/* getClientCertificate.
 *
 * Not in Go: a NULL certificate with no error from GetClientCertificate,
 * which its doc rules out and Go would crash on, is taken as none. */
TlsCertificate *burrow__tls_conn_get_client_certificate(TlsConn *c, Alloc *a,
                                                        TlsCertificateRequestInfo *cri,
                                                        Error *err) {
    if (c->config->get_client_certificate.f != NULL) {
        Error e = BURROW_NO_ERROR;
        TlsCertificate *cert = BURROW_CALLF(c->config->get_client_certificate, cri, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        if (cert != NULL)
            return cert;
    } else {
        TlsCertificate *chains = c->config->certificates.p;
        for (Int i = 0; i < c->config->certificates.len; i++) {
            if (BURROW_FAILED(
                    tls_certificate_request_info_supports_certificate(cri, &chains[i])))
                continue;
            return &chains[i];
        }
    }

    /* No acceptable certificate found. Don't send a certificate. */
    TlsCertificate *none = BURROW_NEW(a, TlsCertificate);
    if (none == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    none->certificate = slice_nil(TYPE_BYTES);
    return none;
}

/* hostnameInSNI converts name into an appropriate hostname for SNI. Literal IP
 * addresses and absolute FQDNs are not permitted as SNI values. See RFC 6066,
 * Section 3. */
Str burrow__tls_hostname_in_sni(Str name) {
    Str host = name;
    const char *p = (const char *)host.p;
    if (host.len > 0 && p[0] == '[' && p[host.len - 1] == ']')
        host = str_from_bytes(p + 1, host.len - 2);
    Int i = strings_last_index(host, BURROW_S("%"));
    if (i > 0)
        host.len = i;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    NetIP ip = net_parse_ip(arena_allocator(&ar), host);
    bool is_ip = !slice_is_nil(ip);
    arena_free(&ar);
    if (is_ip)
        return BURROW_STR_EMPTY;
    while (name.len > 0 && ((const char *)name.p)[name.len - 1] == '.')
        name.len--;
    return name;
}
