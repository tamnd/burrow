/* crypto/tls: handshake_server_tls13.go, the server side of a TLS 1.3
 * handshake. It starts once readClientHello has picked TLS 1.3.
 *
 * QUIC comes with quic.go and Encrypted Client Hello with ech.go. Until then
 * there is never a QUIC connection or an echContext here, and the parts of
 * Go's code that only run with one are left out, with a note where they would
 * go.
 *
 * As in tls_handshake_server.c, what only lasts the handshake is made in its
 * scratch arena, and what the Conn keeps is copied into the Conn's own.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(ts13_err_legacy_version, "tls: client used the legacy version "
                                               "field to negotiate TLS 1.3");
BURROW_SENTINEL_ERROR(ts13_err_fallback,
                      "tls: client using inappropriate protocol fallback");
BURROW_SENTINEL_ERROR(ts13_err_compression,
                      "tls: TLS 1.3 client supports illegal compression methods");
BURROW_SENTINEL_ERROR(ts13_err_renegotiation, "tls: initial handshake had non-empty "
                                              "renegotiation extension");
BURROW_SENTINEL_ERROR(ts13_err_early_data, "tls: client sent unexpected early data");
BURROW_SENTINEL_ERROR(ts13_err_no_key_exchange, "tls: no key exchanges supported by "
                                                "both client and server");
BURROW_SENTINEL_ERROR(ts13_err_curve_accepted, "tls: internal error: supportsCurve "
                                               "accepted unimplemented curve");
BURROW_SENTINEL_ERROR(ts13_err_invalid_key_share, "tls: invalid client key share");
BURROW_SENTINEL_ERROR(ts13_err_quic_params, "tls: client sent an unexpected "
                                            "quic_transport_parameters extension");
BURROW_SENTINEL_ERROR(ts13_err_psk_binders, "tls: invalid or missing PSK binders");
BURROW_SENTINEL_ERROR(ts13_err_clone_hash, "tls: internal error: failed to clone hash");
BURROW_SENTINEL_ERROR(ts13_err_psk_binder, "tls: invalid PSK binder");
BURROW_SENTINEL_ERROR(ts13_err_hand_not_empty, "tls: handshake buffer not empty "
                                               "before HelloRetryRequest");
BURROW_SENTINEL_ERROR(ts13_err_one_key_share, "tls: client didn't send one key share "
                                              "in second ClientHello");
BURROW_SENTINEL_ERROR(ts13_err_unexpected_key_share, "tls: client sent unexpected key "
                                                     "share in second ClientHello");
BURROW_SENTINEL_ERROR(ts13_err_hrr_early_data, "tls: client indicated early data in "
                                               "second ClientHello");
BURROW_SENTINEL_ERROR(ts13_err_illegal_change,
                      "tls: client illegally modified second ClientHello");
BURROW_SENTINEL_ERROR(ts13_err_unknown_suite,
                      "tls: internal error: unknown cipher suite");
BURROW_SENTINEL_ERROR(ts13_err_signature_algorithm, "tls: client certificate used "
                                                    "with invalid signature "
                                                    "algorithm");
BURROW_SENTINEL_ERROR(ts13_err_client_finished, "tls: invalid client finished hash");

/* Not in Go. The allocator of what c keeps once the handshake is over. */
static Alloc *ts13_keep(TlsConn *c) {
    return arena_allocator(&c->state);
}

/* Not in Go. msg written to the connection and the transcript. */
static Error ts13_write(TlsServerHandshakeStateTLS13 *hs, const TlsHandshakeMsg *msg) {
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_handshake_record(
        hs->c, msg, hash_as_io_writer(hs->transcript), &err);
    return err;
}

/* Not in Go. The next handshake message, written to the transcript when
 * transcribe is set. */
static Error ts13_read(TlsServerHandshakeStateTLS13 *hs, bool transcribe,
                       TlsHandshakeMsg *msg) {
    IoWriter none = {NULL, NULL};
    return burrow__tls_conn_read_handshake(
        hs->c, transcribe ? hash_as_io_writer(hs->transcript) : none, hs->a, msg);
}

/* Not in Go. The message_hash message RFC 8446, Section 4.4.1 puts in place of
 * the first ClientHello, written to transcript. */
static void ts13_write_message_hash(Hash transcript, Slice ch_hash) {
    Byte header[4] = {TLS_TYPE_MESSAGE_HASH, 0, 0, (Byte)ch_hash.len};
    (void)hash_write(transcript, slice_from(header, 4, 4, TYPE_BYTE), NULL);
    (void)hash_write(transcript, ch_hash, NULL);
}

/* Not in Go. Whether want is one of the CurveIDs in list. */
static bool ts13_contains_curve(Slice list, TlsCurveID want) {
    const TlsCurveID *ids = list.p;
    for (Int i = 0; i < list.len; i++) {
        if (ids[i] == want)
            return true;
    }
    return false;
}

/* Not in Go. Whether a key share in key_shares, a []keyShare, is for group. */
static bool ts13_has_key_share(Slice key_shares, TlsCurveID group) {
    const TlsKeyShare *ks = key_shares.p;
    for (Int i = 0; i < key_shares.len; i++) {
        if (ks[i].group == group)
            return true;
    }
    return false;
}

/* Not in Go. sort.SliceStable over groups with a less that puts the groups
 * front marks before the others, keeping the order within each part. */
static void ts13_move_to_front(TlsCurveID *groups, Int n, const bool *front) {
    TlsCurveID sorted[8];
    Int k = 0;
    for (Int i = 0; i < n; i++) {
        if (front[i]) {
            sorted[k++] = groups[i];
        }
    }
    for (Int i = 0; i < n; i++) {
        if (!front[i]) {
            sorted[k++] = groups[i];
        }
    }
    memcpy(groups, sorted, (size_t)n * sizeof *groups);
}

static const TlsKeyShare *ts13_do_hello_retry_request(TlsServerHandshakeStateTLS13 *hs,
                                                      TlsCurveID selected_group,
                                                      Error *err);
static Error ts13_send_server_parameters(TlsServerHandshakeStateTLS13 *hs);
static Error ts13_send_server_certificate(TlsServerHandshakeStateTLS13 *hs);
static Error ts13_send_server_finished(TlsServerHandshakeStateTLS13 *hs);
static Error ts13_send_session_tickets(TlsServerHandshakeStateTLS13 *hs);
static Error ts13_read_client_certificate(TlsServerHandshakeStateTLS13 *hs);
static Error ts13_read_client_finished(TlsServerHandshakeStateTLS13 *hs);

Error burrow__tls_server_handshake_state_tls13_handshake(
    TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* For an overview of the TLS 1.3 handshake, see RFC 8446, Section 2. */
    Error err = burrow__tls_server_handshake_state_tls13_process_client_hello(hs);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__tls_server_handshake_state_tls13_check_for_resumption(hs);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__tls_server_handshake_state_tls13_pick_certificate(hs);
    if (BURROW_FAILED(err))
        return err;
    c->buffering = true;
    err = ts13_send_server_parameters(hs);
    if (BURROW_FAILED(err))
        return err;
    err = ts13_send_server_certificate(hs);
    if (BURROW_FAILED(err))
        return err;
    err = ts13_send_server_finished(hs);
    if (BURROW_FAILED(err))
        return err;
    /* Note that at this point we could start sending application data without
     * waiting for the client's second flight, but the application might not
     * expect the lack of replay protection of the ClientHello parameters. */
    (void)burrow__tls_conn_flush(c, &err);
    if (BURROW_FAILED(err))
        return err;
    err = ts13_read_client_certificate(hs);
    if (BURROW_FAILED(err))
        return err;
    err = ts13_read_client_finished(hs);
    if (BURROW_FAILED(err))
        return err;

    sync_atomic_bool_store(&c->is_handshake_complete, true);

    return BURROW_NO_ERROR;
}

/* Not in Go. The cipher suite processClientHello picks: the first of the
 * preference list the client offers too. */
static const TlsSuiteTLS13 *ts13_pick_cipher_suite(const TlsClientHelloMsg *ch) {
    const uint16_t *offered = ch->cipher_suites.p;
    Int noffered = ch->cipher_suites.len;
    const uint16_t *preference_list = burrow__tls_default_cipher_suites_tls13;
    if (!burrow__tls_has_aes_gcm_hardware_support() ||
        !burrow__tls_is_aes_gcm_preferred(offered, noffered))
        preference_list = burrow__tls_default_cipher_suites_tls13_no_aes;
    /* There is no FIPS 140-3 mode here, so allowedCipherSuitesTLS13FIPS is
     * never the list. */
    for (Int i = 0; i < 3; i++) {
        const TlsSuiteTLS13 *suite = burrow__tls_mutual_cipher_suite_tls13(
            offered, noffered, preference_list[i]);
        if (suite != NULL)
            return suite;
    }
    return NULL;
}

/* Not in Go. The group processClientHello picks, or 0 when there is none both
 * sides support.
 *
 * First, if a post-quantum key exchange is available, use one. See
 * draft-ietf-tls-key-share-prediction-01, Section 4 for why this must be
 * first.
 *
 * Second, if the client sent a key share for a group we support, use that, to
 * avoid a HelloRetryRequest round-trip.
 *
 * Finally, pick in our fixed preference order. */
static TlsCurveID ts13_select_group(TlsConn *c, const TlsClientHelloMsg *ch) {
    TlsCurveID preferred_groups[8];
    Int n = burrow__tls_config_curve_preferences(c->config, c->vers, preferred_groups);
    Int k = 0;
    for (Int i = 0; i < n; i++) {
        if (ts13_contains_curve(ch->supported_curves, preferred_groups[i]))
            preferred_groups[k++] = preferred_groups[i];
    }
    if (k == 0)
        return 0;
    bool front[8];
    for (Int i = 0; i < k; i++)
        front[i] = ts13_has_key_share(ch->key_shares, preferred_groups[i]);
    ts13_move_to_front(preferred_groups, k, front);
    for (Int i = 0; i < k; i++)
        front[i] = burrow__tls_is_pq_key_exchange(preferred_groups[i]);
    ts13_move_to_front(preferred_groups, k, front);
    return preferred_groups[0];
}

Error burrow__tls_server_handshake_state_tls13_process_client_hello(
    TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    hs->hello_msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (hs->hello_msg == NULL)
        return burrow_err_out_of_memory;
    burrow__tls_handshake_msg_init(hs->hello_msg, TLS_MSG_SERVER_HELLO);
    hs->hello = &hs->hello_msg->server_hello;

    /* TLS 1.3 froze the ServerHello.legacy_version field, and uses
     * supported_versions instead. See RFC 8446, sections 4.1.3 and 4.2.1. */
    hs->hello->vers = TLS_VERSION_TLS12;
    hs->hello->supported_version = c->vers;

    if (hs->client_hello->supported_versions.len == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return ts13_err_legacy_version;
    }

    /* Abort if the client is doing a fallback and landing lower than what we
     * support. See RFC 7507, which however does not specify the interaction
     * with supported_versions. The only difference is that with
     * supported_versions a client has a chance to attempt a [TLS 1.2, TLS 1.4]
     * handshake in case TLS 1.3 is broken but 1.2 is not. Alas, in that case,
     * it will have to drop the TLS_FALLBACK_SCSV protection if it falls back
     * to TLS 1.2, because a TLS 1.3 server would abort here. The situation
     * before supported_versions was not better because there was just no way
     * to do a TLS 1.4 handshake without risking the server selecting TLS 1.3. */
    const uint16_t *suites = hs->client_hello->cipher_suites.p;
    for (Int i = 0; i < hs->client_hello->cipher_suites.len; i++) {
        if (suites[i] == TLS_TLS_FALLBACK_SCSV) {
            /* Use c.vers instead of max(supported_versions) because an
             * attacker could defeat this by adding an arbitrary high version
             * otherwise. */
            if (c->vers < burrow__tls_config_max_supported_version(
                              c->config, TLS_ROLE_SERVER, false)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INAPPROPRIATE_FALLBACK);
                return ts13_err_fallback;
            }
            break;
        }
    }

    const Byte *methods = hs->client_hello->compression_methods.p;
    if (hs->client_hello->compression_methods.len != 1 ||
        methods[0] != TLS_COMPRESSION_NONE) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return ts13_err_compression;
    }

    hs->hello->random = slice_make(a, TYPE_BYTE, 32, 32);
    if (hs->hello->random.p == NULL)
        return burrow_err_out_of_memory;
    Error err = BURROW_NO_ERROR;
    io_read_full(burrow__tls_config_rand(c->config), hs->hello->random, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }

    if (hs->client_hello->secure_renegotiation.len != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return ts13_err_renegotiation;
    }

    /* The early data a QUIC client may send comes with quic.go. */
    if (hs->client_hello->early_data) {
        /* See RFC 8446, Section 4.2.10 for the complicated behavior required
         * here. The scenario is that a different server at our address offered
         * to accept early data in the past, which we can't handle. For now,
         * all 0-RTT enabled session tickets need to expire before a Go server
         * can replace a server or join a pool. That's the same requirement
         * that applies to mixing or replacing with any TLS 1.2 server. */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return ts13_err_early_data;
    }

    hs->hello->session_id = hs->client_hello->session_id;
    hs->hello->compression_method = TLS_COMPRESSION_NONE;

    hs->suite = ts13_pick_cipher_suite(hs->client_hello);
    if (hs->suite == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return fmt_errorf_v("tls: no cipher suite supported by both client and server; "
                            "client offered: %x",
                            hs->client_hello->cipher_suites);
    }
    c->cipher_suite = hs->suite->id;
    hs->hello->cipher_suite = hs->suite->id;
    hs->transcript = burrow__tls_suite13_hash_new(hs->suite)(a);
    if (hs->transcript.vt == NULL)
        return burrow_err_out_of_memory;

    TlsCurveID selected_group = ts13_select_group(c, hs->client_hello);
    if (selected_group == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return ts13_err_no_key_exchange;
    }

    const TlsKeyShare *client_key_share = NULL;
    const TlsKeyShare *ks = hs->client_hello->key_shares.p;
    for (Int i = 0; i < hs->client_hello->key_shares.len; i++) {
        if (ks[i].group == selected_group) {
            client_key_share = &ks[i];
            break;
        }
    }
    if (client_key_share == NULL) {
        client_key_share = ts13_do_hello_retry_request(hs, selected_group, &err);
        if (client_key_share == NULL)
            return err;
    }
    c->curve_id = selected_group;

    const TlsKeyExchange *ke =
        burrow__tls_key_exchange_for_curve_id(selected_group, &err);
    if (ke == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return ts13_err_curve_accepted;
    }
    hs->shared_key = burrow__tls_key_exchange_server_shared_secret(
        ke, a, burrow__tls_config_rand(c->config), client_key_share->data,
        &hs->hello->server_share, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return ts13_err_invalid_key_share;
    }

    Str selected_proto = BURROW_STR_EMPTY;
    err = burrow__tls_negotiate_alpn(c->config->next_protos,
                                     hs->client_hello->alpn_protocols, false,
                                     &selected_proto);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_NO_APPLICATION_PROTOCOL);
        return err;
    }
    if (!burrow__tls_clone_str(ts13_keep(c), selected_proto, &c->client_protocol))
        return burrow_err_out_of_memory;

    /* The checks a QUIC connection makes, and the transport parameters it
     * keeps, come with quic.go. */
    if (!slice_is_nil(hs->client_hello->quic_transport_parameters)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return ts13_err_quic_params;
    }

    if (!burrow__tls_clone_str(ts13_keep(c), hs->client_hello->server_name,
                               &c->server_name))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

/* cloneHash.
 *
 * Not in Go: the hashes here can't be cloned or marshalled, so this makes a
 * new one for hs.suite and writes to it again what went into hs.transcript.
 * It is only called before the ClientHello goes in, when that is nothing or,
 * after a HelloRetryRequest, the message_hash and the HelloRetryRequest. A
 * zero Hash when it fails. */
static Hash ts13_clone_hash(TlsServerHandshakeStateTLS13 *hs) {
    Hash out = burrow__tls_suite13_hash_new(hs->suite)(hs->a);
    if (out.vt == NULL || hs->hello_retry_request_msg == NULL)
        return out;
    ts13_write_message_hash(out, hs->ch_hash);
    Error err = burrow__tls_transcript_msg(hs->hello_retry_request_msg,
                                           hash_as_io_writer(out), hs->a);
    if (BURROW_FAILED(err)) {
        Hash none = {NULL, NULL};
        return none;
    }
    return out;
}

/* Not in Go. The checks checkForResumption makes of a session before it looks
 * at the binder: whether it can be resumed on this connection. */
static bool ts13_can_resume(TlsServerHandshakeStateTLS13 *hs,
                            const TlsSessionState *session_state) {
    TlsConn *c = hs->c;
    Time now = burrow__tls_config_time(c->config);

    if (session_state->version != TLS_VERSION_TLS13)
        return false;

    Time created_at = time_from_unix((int64_t)session_state->created_at, 0);
    if (time_sub(now, created_at) > TLS_MAX_SESSION_TICKET_LIFETIME)
        return false;

    const TlsSuiteTLS13 *psk_suite =
        burrow__tls_cipher_suite_tls13_by_id(session_state->cipher_suite);
    if (psk_suite == NULL || psk_suite->hash != hs->suite->hash)
        return false;

    /* PSK connections don't re-establish client certificates, but carry them
     * over in the session ticket. Ensure the presence of client certs in the
     * ticket is consistent with the configured requirements. */
    bool session_has_client_certs = session_state->peer_certificates.len != 0;
    bool need_client_certs = burrow__tls_requires_client_cert(c->config->client_auth);
    if (need_client_certs && !session_has_client_certs)
        return false;
    if (session_has_client_certs && c->config->client_auth == TLS_NO_CLIENT_CERT)
        return false;
    if (session_has_client_certs &&
        time_after(now,
                   ((X509Certificate *const *)session_state->peer_certificates.p)[0]
                       ->not_after))
        return false;
    X509ExtKeyUsage client_auth[1] = {X509_EXT_KEY_USAGE_CLIENT_AUTH};
    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.current_time = now;
    opts.roots = c->config->client_cas;
    opts.key_usages = slice_from(client_auth, 1, 1, TYPE_INT);
    if (session_has_client_certs &&
        c->config->client_auth >= TLS_VERIFY_CLIENT_CERT_IF_GIVEN &&
        !burrow__tls_any_valid_verified_chain(session_state->verified_chains, opts))
        return false;

    /* The session events of a QUIC connection come with quic.go. */
    return true;
}

/* Not in Go. The part of checkForResumption that checks the binder the client
 * sent for the i-th identity, whose session is session_state. */
static Error ts13_check_psk_binder(TlsServerHandshakeStateTLS13 *hs,
                                   const TlsSessionState *session_state, Int i) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    hs->early_secret = burrow__tls13_new_early_secret(
        a, burrow__tls_suite13_hash_new(hs->suite), session_state->secret);
    if (hs->early_secret.secret.p == NULL)
        return burrow_err_out_of_memory;
    Slice binder_key =
        burrow__tls13_early_secret_resumption_binder_key(&hs->early_secret, a);
    if (binder_key.p == NULL)
        return burrow_err_out_of_memory;
    /* Clone the transcript in case a HelloRetryRequest was recorded. */
    Hash transcript = ts13_clone_hash(hs);
    if (transcript.vt == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return ts13_err_clone_hash;
    }
    Error err = BURROW_NO_ERROR;
    Slice client_hello_bytes =
        burrow__tls_client_hello_msg_marshal_without_binders(hs->client_hello, a, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    (void)hash_write(transcript, client_hello_bytes, NULL);
    Slice psk_binder =
        burrow__tls13_finished_hash(hs->suite, a, binder_key, transcript);
    if (psk_binder.p == NULL)
        return burrow_err_out_of_memory;
    const Slice *binders = hs->client_hello->psk_binders.p;
    if (!hmac_equal(binders[i], psk_binder)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
        return ts13_err_psk_binder;
    }

    /* Accepting early data is only for QUIC, and comes with quic.go. */
    return BURROW_NO_ERROR;
}

/* Not in Go. The session for identity, from Config.UnwrapSession or the
 * ticket keys: NULL with no error when there is none to try. */
static TlsSessionState *ts13_session_for(TlsServerHandshakeStateTLS13 *hs,
                                         Slice identity, Error *err) {
    TlsConn *c = hs->c;
    if (c->config->unwrap_session.f != NULL) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        Error e = BURROW_NO_ERROR;
        TlsSessionState *ss =
            BURROW_CALLF(c->config->unwrap_session, identity, &cs, &e);
        if (BURROW_FAILED(e)) {
            if (ss != NULL)
                tls_session_state_free(ss);
            BURROW_OUT(err, e);
            return NULL;
        }
        return ss;
    }
    Slice plaintext =
        burrow__tls_config_decrypt_ticket(c->config, hs->a, identity, c->ticket_keys);
    if (slice_is_nil(plaintext))
        return NULL;
    Error e = BURROW_NO_ERROR;
    return tls_parse_session_state(c->a, plaintext, &e);
}

/* Not in Go: a resumed session is the Conn's from here on, and every other one
 * is freed at once. */
Error burrow__tls_server_handshake_state_tls13_check_for_resumption(
    TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    if (c->config->session_tickets_disabled)
        return BURROW_NO_ERROR;

    bool mode_ok = false;
    const Byte *modes = hs->client_hello->psk_modes.p;
    for (Int i = 0; i < hs->client_hello->psk_modes.len; i++) {
        if (modes[i] == TLS_PSK_MODE_DHE) {
            mode_ok = true;
            break;
        }
    }
    if (!mode_ok)
        return BURROW_NO_ERROR;

    if (hs->client_hello->psk_identities.len != hs->client_hello->psk_binders.len) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return ts13_err_psk_binders;
    }
    if (hs->client_hello->psk_identities.len == 0)
        return BURROW_NO_ERROR;

    const TlsPskIdentity *identities = hs->client_hello->psk_identities.p;
    for (Int i = 0; i < hs->client_hello->psk_identities.len; i++) {
        if (i >= TLS_MAX_CLIENT_PSK_IDENTITIES)
            break;

        Error err = BURROW_NO_ERROR;
        TlsSessionState *session_state =
            ts13_session_for(hs, identities[i].label, &err);
        if (BURROW_FAILED(err))
            return err;
        if (session_state == NULL)
            continue;

        if (!ts13_can_resume(hs, session_state)) {
            tls_session_state_free(session_state);
            continue;
        }

        err = ts13_check_psk_binder(hs, session_state, i);
        if (BURROW_FAILED(err)) {
            tls_session_state_free(session_state);
            return err;
        }

        c->did_resume = true;
        c->peer_certificates = session_state->peer_certificates;
        c->ocsp_response = session_state->ocsp_response;
        c->scts = session_state->scts;
        c->verified_chains = session_state->verified_chains;
        /* Not in Go: c now points into the session, so c holds on to it. */
        if (c->server_session != NULL)
            tls_session_state_free(c->server_session);
        c->server_session = session_state;

        hs->hello->selected_identity_present = true;
        hs->hello->selected_identity = (uint16_t)i;
        hs->using_psk = true;
        return BURROW_NO_ERROR;
    }

    return BURROW_NO_ERROR;
}

Error burrow__tls_server_handshake_state_tls13_pick_certificate(
    TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* Only one of PSK and certificates are used at a time. */
    if (hs->using_psk)
        return BURROW_NO_ERROR;

    /* signature_algorithms is required in TLS 1.3. See RFC 8446, Section
     * 4.2.3. */
    if (hs->client_hello->supported_signature_algorithms.len == 0)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_MISSING_EXTENSION);

    TlsClientHelloInfo *chi =
        burrow__tls_client_hello_info(hs->a, hs->ctx, c, hs->client_hello);
    if (chi == NULL)
        return burrow_err_out_of_memory;
    Error err = BURROW_NO_ERROR;
    TlsCertificate *certificate =
        burrow__tls_config_get_certificate(c->config, chi, &err);
    if (BURROW_FAILED(err)) {
        if (errors_is(err, burrow__tls_err_no_certificates))
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNRECOGNIZED_NAME);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    if (certificate != NULL) {
        /* Not in Go: the certificate only has to last for the handshake, so c
         * keeps a copy. */
        if (!burrow__tls_clone_bytes_list(ts13_keep(c), certificate->certificate,
                                          &c->local_certificate))
            return burrow_err_out_of_memory;
    }
    hs->sig_alg = burrow__tls_select_signature_scheme(
        c->vers, certificate, hs->client_hello->supported_signature_algorithms, &err);
    if (BURROW_FAILED(err)) {
        /* getCertificate returned a certificate that is unsupported or
         * incompatible with the client's signature algorithms. */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return err;
    }
    hs->cert = certificate;

    return BURROW_NO_ERROR;
}

/* sendDummyChangeCipherSpec sends a ChangeCipherSpec record for compatibility
 * with middleboxes that didn't implement TLS correctly. See RFC 8446, Appendix
 * D.4. */
static Error ts13_send_dummy_change_cipher_spec(TlsServerHandshakeStateTLS13 *hs) {
    /* QUIC comes with quic.go. */
    if (hs->sent_dummy_ccs)
        return BURROW_NO_ERROR;
    hs->sent_dummy_ccs = true;

    return burrow__tls_conn_write_change_cipher_record(hs->c);
}

/* Not in Go. Whether the first n elements of x and y, both []uint16 or a
 * type with uint16 underneath, are the same. */
static bool ts13_same_u16s(Slice x, Slice y) {
    const uint16_t *xs = x.p;
    const uint16_t *ys = y.p;
    for (Int i = 0; i < x.len; i++) {
        if (xs[i] != ys[i])
            return false;
    }
    return true;
}

/* illegalClientHelloChange reports whether the two ClientHello messages are
 * different, with the exception of the changes allowed before and after a
 * HelloRetryRequest. See RFC 8446, Section 4.1.2. */
static bool ts13_illegal_client_hello_change(const TlsClientHelloMsg *ch,
                                             const TlsClientHelloMsg *ch1) {
    if (ch->supported_versions.len != ch1->supported_versions.len ||
        ch->cipher_suites.len != ch1->cipher_suites.len ||
        ch->supported_curves.len != ch1->supported_curves.len ||
        ch->supported_signature_algorithms.len !=
            ch1->supported_signature_algorithms.len ||
        ch->supported_signature_algorithms_cert.len !=
            ch1->supported_signature_algorithms_cert.len ||
        ch->alpn_protocols.len != ch1->alpn_protocols.len)
        return true;
    if (!ts13_same_u16s(ch->supported_versions, ch1->supported_versions) ||
        !ts13_same_u16s(ch->cipher_suites, ch1->cipher_suites) ||
        !ts13_same_u16s(ch->supported_curves, ch1->supported_curves) ||
        !ts13_same_u16s(ch->supported_signature_algorithms,
                        ch1->supported_signature_algorithms) ||
        !ts13_same_u16s(ch->supported_signature_algorithms_cert,
                        ch1->supported_signature_algorithms_cert))
        return true;
    const Str *protos = ch->alpn_protocols.p;
    const Str *protos1 = ch1->alpn_protocols.p;
    for (Int i = 0; i < ch->alpn_protocols.len; i++) {
        if (!str_eq(protos[i], protos1[i]))
            return true;
    }
    return ch->vers != ch1->vers || !bytes_equal(ch->random, ch1->random) ||
           !bytes_equal(ch->session_id, ch1->session_id) ||
           !bytes_equal(ch->compression_methods, ch1->compression_methods) ||
           !str_eq(ch->server_name, ch1->server_name) ||
           ch->ocsp_stapling != ch1->ocsp_stapling ||
           !bytes_equal(ch->supported_points, ch1->supported_points) ||
           ch->ticket_supported != ch1->ticket_supported ||
           !bytes_equal(ch->session_ticket, ch1->session_ticket) ||
           ch->secure_renegotiation_supported != ch1->secure_renegotiation_supported ||
           !bytes_equal(ch->secure_renegotiation, ch1->secure_renegotiation) ||
           ch->scts != ch1->scts || !bytes_equal(ch->cookie, ch1->cookie) ||
           !bytes_equal(ch->psk_modes, ch1->psk_modes);
}

/* Not in Go. The checks doHelloRetryRequest makes of the second ClientHello:
 * the key share it has for selected_group, or NULL with the error. */
static const TlsKeyShare *ts13_check_second_hello(TlsServerHandshakeStateTLS13 *hs,
                                                  const TlsClientHelloMsg *client_hello,
                                                  TlsCurveID selected_group,
                                                  Error *err) {
    TlsConn *c = hs->c;

    if (client_hello->key_shares.len != 1) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        BURROW_OUT(err, ts13_err_one_key_share);
        return NULL;
    }
    const TlsKeyShare *ks = client_hello->key_shares.p;

    if (ks->group != selected_group) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        BURROW_OUT(err, ts13_err_unexpected_key_share);
        return NULL;
    }

    if (client_hello->early_data) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        BURROW_OUT(err, ts13_err_hrr_early_data);
        return NULL;
    }

    if (ts13_illegal_client_hello_change(client_hello, hs->client_hello)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        BURROW_OUT(err, ts13_err_illegal_change);
        return NULL;
    }
    return ks;
}

/* Not in Go: the key share is NULL when there is an error. */
static const TlsKeyShare *ts13_do_hello_retry_request(TlsServerHandshakeStateTLS13 *hs,
                                                      TlsCurveID selected_group,
                                                      Error *err) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    /* Make sure the client didn't send extra handshake messages alongside
     * their initial client_hello. If they sent two client_hello messages, we
     * will consume the second before they respond to the server_hello. */
    if (bytes_buffer_len(&c->hand) != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        BURROW_OUT(err, ts13_err_hand_not_empty);
        return NULL;
    }

    /* The first ClientHello gets double-hashed into the transcript upon a
     * HelloRetryRequest. See RFC 8446, Section 4.4.1. */
    Error e = burrow__tls_transcript_msg(hs->client_hello_msg,
                                         hash_as_io_writer(hs->transcript), a);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    Slice ch_hash = hash_sum(a, hs->transcript, slice_nil(TYPE_BYTE));
    if (ch_hash.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    hash_reset(hs->transcript);
    ts13_write_message_hash(hs->transcript, ch_hash);

    TlsHandshakeMsg *hrr_msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (hrr_msg == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__tls_handshake_msg_init(hrr_msg, TLS_MSG_SERVER_HELLO);
    TlsServerHelloMsg *hello_retry_request = &hrr_msg->server_hello;
    hello_retry_request->vers = hs->hello->vers;
    hello_retry_request->random = slice_from(
        (Byte *)(uintptr_t)burrow__tls_hello_retry_request_random, 32, 32, TYPE_BYTE);
    hello_retry_request->session_id = hs->hello->session_id;
    hello_retry_request->cipher_suite = hs->hello->cipher_suite;
    hello_retry_request->compression_method = hs->hello->compression_method;
    hello_retry_request->supported_version = hs->hello->supported_version;
    hello_retry_request->selected_group = selected_group;

    /* The ECH acceptance message comes with ech.go. */

    e = ts13_write(hs, hrr_msg);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }

    e = ts13_send_dummy_change_cipher_spec(hs);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }

    /* clientHelloMsg is not included in the transcript. */
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    e = ts13_read(hs, false, msg);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }

    if (msg->kind != TLS_MSG_CLIENT_HELLO) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        BURROW_OUT(
            err, burrow__tls_unexpected_message_error(TLS_MSG_CLIENT_HELLO, msg->kind));
        return NULL;
    }
    TlsClientHelloMsg *client_hello = &msg->client_hello;

    /* The second hello of Encrypted Client Hello comes with ech.go. */

    const TlsKeyShare *ks =
        ts13_check_second_hello(hs, client_hello, selected_group, err);
    if (ks == NULL)
        return NULL;

    c->did_hrr = true;
    hs->client_hello = client_hello;
    hs->client_hello_msg = msg;
    hs->ch_hash = ch_hash;
    hs->hello_retry_request_msg = hrr_msg;
    return ks;
}

/* Not in Go. writeKeyLog for both secrets of a stage, with an internal_error
 * alert when it fails. */
static Error ts13_write_key_logs(TlsServerHandshakeStateTLS13 *hs, Str client_label,
                                 Slice client_secret, Str server_label,
                                 Slice server_secret) {
    TlsConn *c = hs->c;
    Error err = burrow__tls_config_write_key_log(
        c->config, client_label, hs->client_hello->random, client_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    err = burrow__tls_config_write_key_log(c->config, server_label,
                                           hs->client_hello->random, server_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    return BURROW_NO_ERROR;
}

/* Not in Go. The handshake traffic secrets sendServerParameters derives once
 * the ServerHello is in the transcript, put to use and logged. */
static Error ts13_establish_handshake_keys(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    Tls13EarlySecret early_secret = hs->early_secret;
    if (early_secret.secret.p == NULL) {
        early_secret = burrow__tls13_new_early_secret(
            a, burrow__tls_suite13_hash_new(hs->suite), slice_nil(TYPE_BYTE));
        if (early_secret.secret.p == NULL)
            return burrow_err_out_of_memory;
    }
    hs->handshake_secret =
        burrow__tls13_early_secret_handshake_secret(&early_secret, a, hs->shared_key);
    if (hs->handshake_secret.secret.p == NULL)
        return burrow_err_out_of_memory;

    Slice server_secret =
        burrow__tls13_handshake_secret_server_handshake_traffic_secret(
            &hs->handshake_secret, a, hs->transcript);
    if (server_secret.p == NULL)
        return burrow_err_out_of_memory;
    Error err = burrow__tls_conn_set_write_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE, server_secret);
    if (BURROW_FAILED(err))
        return err;
    Slice client_secret =
        burrow__tls13_handshake_secret_client_handshake_traffic_secret(
            &hs->handshake_secret, a, hs->transcript);
    if (client_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_set_read_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE, client_secret, false);
    if (BURROW_FAILED(err))
        return err;

    /* Handing the secrets to QUIC comes with quic.go. */

    return ts13_write_key_logs(
        hs, BURROW_S(TLS_KEY_LOG_LABEL_CLIENT_HANDSHAKE), client_secret,
        BURROW_S(TLS_KEY_LOG_LABEL_SERVER_HANDSHAKE), server_secret);
}

static Error ts13_send_server_parameters(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    /* The ECH acceptance message comes with ech.go. */

    Error err = burrow__tls_transcript_msg(hs->client_hello_msg,
                                           hash_as_io_writer(hs->transcript), a);
    if (BURROW_FAILED(err))
        return err;

    err = ts13_write(hs, hs->hello_msg);
    if (BURROW_FAILED(err))
        return err;

    err = ts13_send_dummy_change_cipher_spec(hs);
    if (BURROW_FAILED(err))
        return err;

    err = ts13_establish_handshake_keys(hs);
    if (BURROW_FAILED(err))
        return err;

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_ENCRYPTED_EXTENSIONS);
    TlsEncryptedExtensionsMsg *encrypted_extensions = &msg.encrypted_extensions;
    encrypted_extensions->alpn_protocol = c->client_protocol;

    /* The QUIC transport parameters and early data come with quic.go. */

    if (!c->did_resume && hs->client_hello->server_name.len != 0)
        encrypted_extensions->server_name_ack = true;

    /* If client sent ECH extension, but we didn't accept it, send retry
     * configs, if available.
     *
     * Not in Go: until ech.go, readClientHello ends the handshake for an outer
     * ECH extension when there are keys, so there are never retry configs to
     * send. GetEncryptedClientHelloKeys is still called, as Go calls it. */
    if (c->config->get_encrypted_client_hello_keys.f != NULL) {
        TlsClientHelloInfo *chi =
            burrow__tls_client_hello_info(a, hs->ctx, c, hs->client_hello);
        if (chi == NULL)
            return burrow_err_out_of_memory;
        (void)BURROW_CALLF(c->config->get_encrypted_client_hello_keys, chi, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }
    }

    return ts13_write(hs, &msg);
}

static bool ts13_request_client_cert(const TlsServerHandshakeStateTLS13 *hs) {
    return hs->c->config->client_auth >= TLS_REQUEST_CLIENT_CERT && !hs->using_psk;
}

/* Not in Go. The CertificateRequest sendServerCertificate sends. */
static Error ts13_send_certificate_request(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_REQUEST_TLS13);
    TlsCertificateRequestMsgTLS13 *cert_req = &msg.certificate_request_tls13;
    cert_req->ocsp_stapling = true;
    cert_req->scts = true;
    cert_req->supported_signature_algorithms =
        burrow__tls_supported_signature_algorithms(a, c->vers, c->vers);
    cert_req->supported_signature_algorithms_cert =
        burrow__tls_supported_signature_algorithms_cert(a, c->vers, c->vers);
    if (cert_req->supported_signature_algorithms.p == NULL ||
        cert_req->supported_signature_algorithms_cert.p == NULL)
        return burrow_err_out_of_memory;
    if (c->config->client_cas != NULL) {
        cert_req->certificate_authorities =
            x509_cert_pool_subjects(c->config->client_cas, a);
        if (cert_req->certificate_authorities.p == NULL &&
            cert_req->certificate_authorities.len > 0)
            return burrow_err_out_of_memory;
    }

    return ts13_write(hs, &msg);
}

/* Not in Go. The CertificateVerify sendServerCertificate sends. */
static Error ts13_send_certificate_verify(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_VERIFY);
    TlsCertificateVerifyMsg *cert_verify_msg = &msg.certificate_verify;
    cert_verify_msg->has_signature_algorithm = true;
    cert_verify_msg->signature_algorithm = hs->sig_alg;

    uint8_t sig_type;
    CryptoHash sig_hash;
    if (!burrow__tls_type_and_hash_from_signature_scheme(hs->sig_alg, &sig_type,
                                                         &sig_hash, NULL))
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);

    Slice signed_ = burrow__tls_signed_message(
        a, BURROW_S(TLS_SERVER_SIGNATURE_CONTEXT), hs->transcript);
    if (signed_.p == NULL)
        return burrow_err_out_of_memory;
    CryptoSignerOpts sign_opts = crypto_hash_as_signer_opts(&sig_hash);
    RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, sig_hash};
    if (sig_type == TLS_SIGNATURE_RSAPSS)
        sign_opts = rsa_pss_options_as_signer_opts(&pss);
    /* Not in Go: selectSignatureScheme has already turned down a key that is
     * not a crypto.Signer. */
    Ed25519Signer es;
    CryptoSigner key;
    if (!burrow__tls_private_key_signer(hs->cert->private_key, &es, &key))
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    Error err = BURROW_NO_ERROR;
    Slice sig = crypto_sign_message(a, key, burrow__tls_config_rand(c->config), signed_,
                                    sign_opts, &err);
    if (BURROW_FAILED(err)) {
        CryptoPublicKey pub = crypto_signer_public(key);
        const RsaPublicKey *rsa_key =
            pub.t == TYPE_RSA_PUBLIC_KEY ? (const RsaPublicKey *)pub.data : NULL;
        if (rsa_key != NULL && sig_type == TLS_SIGNATURE_RSAPSS &&
            big_int_bit_len(rsa_key->n) / 8 <
                crypto_hash_size(sig_hash) * 2 + 2) /* key too small for RSA-PSS */
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return fmt_errorf_v("tls: failed to sign handshake: %v", err);
    }
    cert_verify_msg->signature = sig;

    return ts13_write(hs, &msg);
}

static Error ts13_send_server_certificate(TlsServerHandshakeStateTLS13 *hs) {
    /* Only one of PSK and certificates are used at a time. */
    if (hs->using_psk)
        return BURROW_NO_ERROR;

    Error err = BURROW_NO_ERROR;
    if (ts13_request_client_cert(hs)) {
        /* Request a client certificate */
        err = ts13_send_certificate_request(hs);
        if (BURROW_FAILED(err))
            return err;
    }

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_TLS13);
    TlsCertificateMsgTLS13 *cert_msg = &msg.certificate_tls13;

    cert_msg->certificate = *hs->cert;
    cert_msg->scts =
        hs->client_hello->scts && hs->cert->signed_certificate_timestamps.len > 0;
    cert_msg->ocsp_stapling =
        hs->client_hello->ocsp_stapling && hs->cert->ocsp_staple.len > 0;

    err = ts13_write(hs, &msg);
    if (BURROW_FAILED(err))
        return err;

    return ts13_send_certificate_verify(hs);
}

static Error ts13_send_server_finished(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    TlsHandshakeMsg finished;
    burrow__tls_handshake_msg_init(&finished, TLS_MSG_FINISHED);
    finished.finished.verify_data = burrow__tls13_finished_hash(
        hs->suite, a, c->out.traffic_secret, hs->transcript);
    if (finished.finished.verify_data.p == NULL)
        return burrow_err_out_of_memory;

    Error err = ts13_write(hs, &finished);
    if (BURROW_FAILED(err))
        return err;

    /* Derive secrets that take context through the server Finished. */

    hs->master_secret =
        burrow__tls13_handshake_secret_master_secret(&hs->handshake_secret, a);
    if (hs->master_secret.secret.p == NULL)
        return burrow_err_out_of_memory;

    hs->traffic_secret = burrow__tls13_master_secret_client_application_traffic_secret(
        &hs->master_secret, a, hs->transcript);
    Slice server_secret = burrow__tls13_master_secret_server_application_traffic_secret(
        &hs->master_secret, a, hs->transcript);
    if (hs->traffic_secret.p == NULL || server_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_set_write_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, server_secret);
    if (BURROW_FAILED(err))
        return err;

    /* Handing the secret to QUIC comes with quic.go. */

    err = ts13_write_key_logs(
        hs, BURROW_S(TLS_KEY_LOG_LABEL_CLIENT_TRAFFIC), hs->traffic_secret,
        BURROW_S(TLS_KEY_LOG_LABEL_SERVER_TRAFFIC), server_secret);
    if (BURROW_FAILED(err))
        return err;

    c->ekm = burrow__tls13_export_keying_material(hs->suite, ts13_keep(c),
                                                  &hs->master_secret, hs->transcript);
    if (c->ekm.f == NULL)
        return burrow_err_out_of_memory;

    /* If we did not request client certificates, at this point we can
     * precompute the client finished and roll the transcript forward to send
     * session tickets in our first flight. */
    if (!ts13_request_client_cert(hs))
        return ts13_send_session_tickets(hs);

    return BURROW_NO_ERROR;
}

static bool ts13_should_send_session_tickets(const TlsServerHandshakeStateTLS13 *hs) {
    if (hs->c->config->session_tickets_disabled)
        return false;

    /* QUIC tickets are sent by QUICConn.SendSessionTicket, not automatically.
     * QUIC comes with quic.go. */

    /* Don't send tickets the client wouldn't use. See RFC 8446, Section 4.2.9. */
    const Byte *modes = hs->client_hello->psk_modes.p;
    for (Int i = 0; i < hs->client_hello->psk_modes.len; i++) {
        if (modes[i] == TLS_PSK_MODE_DHE)
            return true;
    }
    return false;
}

static Error ts13_send_session_tickets(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    hs->client_finished =
        burrow__tls13_finished_hash(hs->suite, a, c->in.traffic_secret, hs->transcript);
    if (hs->client_finished.p == NULL)
        return burrow_err_out_of_memory;
    TlsHandshakeMsg finished_msg;
    burrow__tls_handshake_msg_init(&finished_msg, TLS_MSG_FINISHED);
    finished_msg.finished.verify_data = hs->client_finished;
    Error err =
        burrow__tls_transcript_msg(&finished_msg, hash_as_io_writer(hs->transcript), a);
    if (BURROW_FAILED(err))
        return err;

    c->resumption_secret = burrow__tls13_master_secret_resumption_master_secret(
        &hs->master_secret, ts13_keep(c), hs->transcript);
    if (c->resumption_secret.p == NULL)
        return burrow_err_out_of_memory;

    if (!ts13_should_send_session_tickets(hs))
        return BURROW_NO_ERROR;
    return burrow__tls_conn_send_session_ticket(c, false, slice_nil(TYPE_BYTES));
}

/* Not in Go. The ticket sendSessionTicket sends for state, from a. */
static Slice ts13_wrap_session(TlsConn *c, Alloc *a, TlsSessionState *state,
                               Error *err) {
    if (c->config->wrap_session.f != NULL) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        return BURROW_CALLF(c->config->wrap_session, a, &cs, state, err);
    }
    Error e = BURROW_NO_ERROR;
    Slice state_bytes = tls_session_state_bytes(state, a, &e);
    if (BURROW_FAILED(e)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    return burrow__tls_config_encrypt_ticket(c->config, a, state_bytes, c->ticket_keys,
                                             err);
}

/* Not in Go: what it makes only lasts the call, in an arena of its own. */
Error burrow__tls_conn_send_session_ticket(TlsConn *c, bool early_data, Slice extra) {
    const TlsSuiteTLS13 *suite = burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
    if (suite == NULL)
        return ts13_err_unknown_suite;

    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Alloc *a = arena_allocator(&tmp);

    /* ticket_nonce, which must be unique per connection, is always left at
     * zero because we only ever send one ticket per connection. */
    Slice psk = burrow__tls13_expand_label(
        a, burrow__tls_suite13_hash_new(suite), c->resumption_secret,
        BURROW_S("resumption"), slice_nil(TYPE_BYTE), crypto_hash_size(suite->hash));
    if (psk.p == NULL) {
        arena_free(&tmp);
        return burrow_err_out_of_memory;
    }

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_NEW_SESSION_TICKET_TLS13);
    TlsNewSessionTicketMsgTLS13 *m = &msg.new_session_ticket_tls13;

    Error err = BURROW_NO_ERROR;
    TlsSessionState *state = burrow__tls_conn_session_state(c, a, &err);
    if (state == NULL) {
        arena_free(&tmp);
        return err;
    }
    state->secret = psk;
    state->early_data = early_data;
    state->extra = extra;
    m->label = ts13_wrap_session(c, a, state, &err);
    tls_session_state_free(state);
    if (BURROW_FAILED(err)) {
        arena_free(&tmp);
        return err;
    }
    m->lifetime = (uint32_t)(TLS_MAX_SESSION_TICKET_LIFETIME / TIME_SECOND);

    /* ticket_age_add is a random 32-bit value. See RFC 8446, section 4.6.1 The
     * value is not stored anywhere; we never need to check the ticket age
     * because 0-RTT is not supported. */
    Byte age_add[4] = {0};
    IoReader rand = burrow__tls_config_rand(c->config);
    (void)rand.vt->read(rand.data, slice_from(age_add, 4, 4, TYPE_BYTE), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&tmp);
        return err;
    }
    m->age_add = binary_little_endian_uint32(slice_from(age_add, 4, 4, TYPE_BYTE));

    if (early_data) {
        /* RFC 9001, Section 4.6.1 */
        m->max_early_data = 0xffffffff;
    }

    IoWriter none = {NULL, NULL};
    (void)burrow__tls_conn_write_handshake_record(c, &msg, none, &err);
    arena_free(&tmp);
    return err;
}

/* Not in Go. Config.VerifyConnection, when there is one, with the alert Go
 * sends when it fails. */
static Error ts13_verify_connection(TlsConn *c) {
    if (c->config->verify_connection.f == NULL)
        return BURROW_NO_ERROR;
    TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
    Error err = BURROW_CALLF(c->config->verify_connection, &cs);
    if (BURROW_FAILED(err))
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
    return err;
}

/* Not in Go. The CertificateVerify from the client, checked against the
 * transcript up to it, which is the part of readClientCertificate after the
 * certificate is verified. */
static Error ts13_read_certificate_verify(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    /* certificateVerifyMsg is included in the transcript, but not until after
     * we verify the handshake signature, since the state before this message
     * was sent is used. */
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = ts13_read(hs, false, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_CERTIFICATE_VERIFY) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE_VERIFY,
                                                    msg->kind);
    }
    const TlsCertificateVerifyMsg *cert_verify = &msg->certificate_verify;

    /* See RFC 8446, Section 4.4.3. We don't use
     * certReq.supportedSignatureAlgorithms because it would require keeping
     * the certificateRequestMsgTLS13 around in the hs. */
    CryptoPublicKey pub =
        (*(X509Certificate **)slice_at(c->peer_certificates, 0))->public_key;
    Slice supported = burrow__tls_supported_signature_algorithms(a, c->vers, c->vers);
    Slice for_key = burrow__tls_signature_schemes_for_public_key(a, c->vers, pub);
    if (supported.p == NULL)
        return burrow_err_out_of_memory;
    if (!burrow__tls_is_supported_signature_algorithm(cert_verify->signature_algorithm,
                                                      supported) ||
        !burrow__tls_is_supported_signature_algorithm(cert_verify->signature_algorithm,
                                                      for_key)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return ts13_err_signature_algorithm;
    }
    uint8_t sig_type;
    CryptoHash sig_hash;
    if (!burrow__tls_type_and_hash_from_signature_scheme(
            cert_verify->signature_algorithm, &sig_type, &sig_hash, NULL))
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    if (sig_type == TLS_SIGNATURE_PKCS1V15 || sig_hash == CRYPTO_SHA1)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    Slice signed_ = burrow__tls_signed_message(
        a, BURROW_S(TLS_CLIENT_SIGNATURE_CONTEXT), hs->transcript);
    if (signed_.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_verify_handshake_signature(sig_type, pub, sig_hash, signed_,
                                                 cert_verify->signature);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
        return fmt_errorf_v("tls: invalid signature by the client certificate: %v",
                            err);
    }
    c->peer_sig_alg = cert_verify->signature_algorithm;

    return burrow__tls_transcript_msg(msg, hash_as_io_writer(hs->transcript), a);
}

static Error ts13_read_client_certificate(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    if (!ts13_request_client_cert(hs)) {
        /* Make sure the connection is still being verified whether or not the
         * server requested a client certificate. */
        return ts13_verify_connection(c);
    }

    /* If we requested a client certificate, then the client must send a
     * certificate message. If it's empty, no CertificateVerify is sent. */

    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = ts13_read(hs, true, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_CERTIFICATE_TLS13) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE_TLS13,
                                                    msg->kind);
    }
    const TlsCertificateMsgTLS13 *cert_msg = &msg->certificate_tls13;

    err = burrow__tls_conn_process_certs_from_client(c, &cert_msg->certificate);
    if (BURROW_FAILED(err))
        return err;

    err = ts13_verify_connection(c);
    if (BURROW_FAILED(err))
        return err;

    if (cert_msg->certificate.certificate.len != 0) {
        err = ts13_read_certificate_verify(hs);
        if (BURROW_FAILED(err))
            return err;
    }

    /* If we waited until the client certificates to send session tickets, we
     * are ready to do it now. */
    return ts13_send_session_tickets(hs);
}

static Error ts13_read_client_finished(TlsServerHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* finishedMsg is not included in the transcript. */
    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = ts13_read(hs, false, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_FINISHED) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_FINISHED, msg->kind);
    }

    if (!hmac_equal(hs->client_finished, msg->finished.verify_data)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
        return ts13_err_client_finished;
    }

    return burrow__tls_conn_set_read_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, hs->traffic_secret, false);
}
