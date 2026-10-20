/* crypto/tls: handshake_client_tls13.go, the client side of a TLS 1.3
 * handshake. It starts once the server's first ServerHello has picked TLS 1.3.
 *
 * QUIC comes with quic.go. Until then there is never a QUIC connection here,
 * and the parts of Go's code that only run with one are left out, with a note
 * where they would go.
 *
 * As in tls_handshake_client.c, what only lasts the handshake is made in its
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
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(tc13_err_renegotiation,
                      "tls: server selected TLS 1.3 in a renegotiation");
BURROW_SENTINEL_ERROR(tc13_err_legacy_version_field,
                      "tls: server selected TLS 1.3 using the legacy version field");
BURROW_SENTINEL_ERROR(tc13_err_invalid_version,
                      "tls: server selected an invalid version after a "
                      "HelloRetryRequest");
BURROW_SENTINEL_ERROR(tc13_err_legacy_version,
                      "tls: server sent an incorrect legacy version");
BURROW_SENTINEL_ERROR(tc13_err_forbidden_extension,
                      "tls: server sent a ServerHello extension forbidden in TLS 1.3");
BURROW_SENTINEL_ERROR(tc13_err_session_id,
                      "tls: server did not echo the legacy session ID");
BURROW_SENTINEL_ERROR(tc13_err_compression,
                      "tls: server sent non-zero legacy TLS compression method");
BURROW_SENTINEL_ERROR(tc13_err_changed_suite,
                      "tls: server changed cipher suite after a HelloRetryRequest");
BURROW_SENTINEL_ERROR(tc13_err_unconfigured_suite,
                      "tls: server chose an unconfigured cipher suite");
BURROW_SENTINEL_ERROR(tc13_err_unsolicited_ech, "tls: unexpected encrypted client "
                                                "hello extension in serverHello");
BURROW_SENTINEL_ERROR(tc13_err_clone_hash, "tls: internal error: failed to clone hash");
BURROW_SENTINEL_ERROR(tc13_err_ech_unexpected_accept,
                      "tls: unexpected encrypted client hello extension in server "
                      "hello despite ECH being accepted");
BURROW_SENTINEL_ERROR(tc13_err_ech_server_name,
                      "tls: unexpected server_name extension in server hello");
BURROW_SENTINEL_ERROR(tc13_err_ech_malformed_hrr,
                      "tls: malformed encrypted client hello extension");
BURROW_SENTINEL_ERROR(tc13_err_ech_retry_after_accept,
                      "tls: server sent encrypted client hello retry configs after "
                      "accepting encrypted client hello");
BURROW_SENTINEL_ERROR(tc13_err_unnecessary_hrr,
                      "tls: server sent an unnecessary HelloRetryRequest message");
BURROW_SENTINEL_ERROR(tc13_err_malformed_key_share_hrr,
                      "tls: received malformed key_share extension");
BURROW_SENTINEL_ERROR(tc13_err_unsupported_group,
                      "tls: server selected unsupported group");
BURROW_SENTINEL_ERROR(tc13_err_unnecessary_key_share,
                      "tls: server sent an unnecessary HelloRetryRequest key_share");
BURROW_SENTINEL_ERROR(tc13_err_curve_accepted, "tls: internal error: supportsCurve "
                                               "accepted unimplemented curve");
BURROW_SENTINEL_ERROR(tc13_err_two_hrr,
                      "tls: server sent two HelloRetryRequest messages");
BURROW_SENTINEL_ERROR(tc13_err_cookie,
                      "tls: server sent a cookie in a normal ServerHello");
BURROW_SENTINEL_ERROR(tc13_err_malformed_key_share,
                      "tls: malformed key_share extension");
BURROW_SENTINEL_ERROR(tc13_err_no_key_share, "tls: server did not send a key share");
BURROW_SENTINEL_ERROR(tc13_err_invalid_psk, "tls: server selected an invalid PSK");
BURROW_SENTINEL_ERROR(tc13_err_psk_suite, "tls: server selected an invalid PSK and "
                                          "cipher suite pair");
BURROW_SENTINEL_ERROR(tc13_err_invalid_key_share, "tls: invalid server key share");
BURROW_SENTINEL_ERROR(tc13_err_quic_params, "tls: server sent an unexpected "
                                            "quic_transport_parameters extension");
BURROW_SENTINEL_ERROR(tc13_err_early_data,
                      "tls: server sent an unexpected early_data extension");
BURROW_SENTINEL_ERROR(tc13_err_early_data_suite,
                      "tls: server accepted 0-RTT with the wrong cipher suite");
BURROW_SENTINEL_ERROR(tc13_err_early_data_alpn,
                      "tls: server accepted 0-RTT with the wrong ALPN");
BURROW_SENTINEL_ERROR(tc13_err_empty_certificates,
                      "tls: received empty certificates message");
BURROW_SENTINEL_ERROR(tc13_err_signature_algorithm,
                      "tls: certificate used with invalid signature algorithm");
BURROW_SENTINEL_ERROR(tc13_err_server_finished, "tls: invalid server finished hash");

/* Not in Go. The allocator of what c keeps once the handshake is over. */
static Alloc *tc13_keep(TlsConn *c) {
    return arena_allocator(&c->state);
}

/* Not in Go. msg written to the connection and the transcript. */
static Error tc13_write(TlsClientHandshakeStateTLS13 *hs, const TlsHandshakeMsg *msg) {
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_handshake_record(
        hs->c, msg, hash_as_io_writer(hs->transcript), &err);
    return err;
}

/* Not in Go. The next handshake message, written to the transcript when
 * transcribe is set. */
static Error tc13_read(TlsClientHandshakeStateTLS13 *hs, bool transcribe,
                       TlsHandshakeMsg *msg) {
    IoWriter none = {NULL, NULL};
    return burrow__tls_conn_read_handshake(
        hs->c, transcribe ? hash_as_io_writer(hs->transcript) : none, hs->a, msg);
}

/* Not in Go. The message_hash message RFC 8446, Section 4.4.1 puts in place of
 * the first ClientHello, written to transcript. */
static void tc13_write_message_hash(Hash transcript, Slice ch_hash) {
    Byte header[4] = {TLS_TYPE_MESSAGE_HASH, 0, 0, (Byte)ch_hash.len};
    (void)hash_write(transcript, slice_from(header, 4, 4, TYPE_BYTE), NULL);
    (void)hash_write(transcript, ch_hash, NULL);
}

/* Not in Go. Whether want is one of the CurveIDs in list. */
static bool tc13_contains_curve(Slice list, TlsCurveID want) {
    const TlsCurveID *ids = list.p;
    for (Int i = 0; i < list.len; i++) {
        if (ids[i] == want)
            return true;
    }
    return false;
}

/* Not in Go. Whether a key share in key_shares, a []keyShare, is for group. */
static bool tc13_has_key_share(Slice key_shares, TlsCurveID group) {
    const TlsKeyShare *ks = key_shares.p;
    for (Int i = 0; i < key_shares.len; i++) {
        if (ks[i].group == group)
            return true;
    }
    return false;
}

static Error tc13_check_server_hello_or_hrr(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_check_ech_accept(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_send_dummy_change_cipher_spec(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_process_hello_retry_request(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_process_server_hello(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_establish_handshake_keys(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_read_server_parameters(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_read_server_certificate(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_read_server_finished(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_send_client_certificate(TlsClientHandshakeStateTLS13 *hs);
static Error tc13_send_client_finished(TlsClientHandshakeStateTLS13 *hs);

Error burrow__tls_client_handshake_state_tls13_handshake(
    TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* The server must not select TLS 1.3 in a renegotiation. See RFC 8446,
     * sections 4.1.2 and 4.1.3. */
    if (c->handshakes > 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_PROTOCOL_VERSION);
        return tc13_err_renegotiation;
    }

    /* Consistency check on the presence of a keyShare and its parameters. */
    if (hs->key_share_keys == NULL ||
        (hs->key_share_keys->ecdhe == NULL && hs->key_share_keys->mlkem.vt == NULL) ||
        hs->hello->key_shares.len == 0)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);

    Error err = tc13_check_server_hello_or_hrr(hs);
    if (BURROW_FAILED(err))
        return err;

    hs->transcript = burrow__tls_suite13_hash_new(hs->suite)(hs->a);
    if (hs->transcript.vt == NULL)
        return burrow_err_out_of_memory;

    err = burrow__tls_transcript_msg(hs->hello_msg, hash_as_io_writer(hs->transcript),
                                     hs->a);
    if (BURROW_FAILED(err))
        return err;

    if (hs->ech_context != NULL) {
        hs->ech_context->inner_transcript =
            burrow__tls_suite13_hash_new(hs->suite)(hs->a);
        if (hs->ech_context->inner_transcript.vt == NULL)
            return burrow_err_out_of_memory;
        err = burrow__tls_transcript_msg(
            hs->ech_context->inner_hello_msg,
            hash_as_io_writer(hs->ech_context->inner_transcript), hs->a);
        if (BURROW_FAILED(err))
            return err;
    }

    if (bytes_equal(
            hs->server_hello->random,
            slice_from((Byte *)(uintptr_t)burrow__tls_hello_retry_request_random, 32,
                       32, TYPE_BYTE))) {
        err = tc13_send_dummy_change_cipher_spec(hs);
        if (BURROW_FAILED(err))
            return err;
        err = tc13_process_hello_retry_request(hs);
        if (BURROW_FAILED(err))
            return err;
    }

    if (hs->ech_context != NULL) {
        err = tc13_check_ech_accept(hs);
        if (BURROW_FAILED(err))
            return err;
    }

    err = burrow__tls_transcript_msg(hs->server_hello_msg,
                                     hash_as_io_writer(hs->transcript), hs->a);
    if (BURROW_FAILED(err))
        return err;

    c->buffering = true;
    err = tc13_process_server_hello(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_send_dummy_change_cipher_spec(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_establish_handshake_keys(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_read_server_parameters(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_read_server_certificate(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_read_server_finished(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_send_client_certificate(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tc13_send_client_finished(hs);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__tls_conn_flush(c, &err);
    if (BURROW_FAILED(err))
        return err;

    if (hs->ech_context != NULL && hs->ech_context->ech_rejected) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ECH_REQUIRED);
        TlsECHRejectionError rejection = {hs->ech_context->retry_configs};
        return tls_ech_rejection_error_as_error(&rejection, hs->a);
    }

    sync_atomic_bool_store(&c->is_handshake_complete, true);

    return BURROW_NO_ERROR;
}

/* Not in Go. The part of handshake that checks whether the server accepted
 * ECH, from the last eight bytes of its random, and moves to the inner hello
 * and transcript when it did. */
static Error tc13_check_ech_accept(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;
    TlsECHClientContext *ech = hs->ech_context;
    const TlsServerHelloMsg *sh = hs->server_hello;
    Hash conf_transcript =
        burrow__tls_clone_hash(a, ech->inner_transcript, hs->suite->hash);
    if (conf_transcript.vt == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return tc13_err_clone_hash;
    }
    Byte zeros[8] = {0};
    (void)hash_write(conf_transcript, slice_sub(sh->original, 0, 30), NULL);
    (void)hash_write(conf_transcript, slice_from(zeros, 8, 8, TYPE_BYTE), NULL);
    (void)hash_write(conf_transcript, slice_sub(sh->original, 38, sh->original.len),
                     NULL);
    Error err = BURROW_NO_ERROR;
    Slice accept_confirmation = burrow__tls_ech_accept_confirmation(
        a, hs->suite, ech->inner_hello->random, BURROW_S("ech accept confirmation"),
        conf_transcript, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    if (subtle_constant_time_compare(
            accept_confirmation,
            slice_sub(sh->random, sh->random.len - 8, sh->random.len)) == 1) {
        hs->hello = ech->inner_hello;
        hs->hello_msg = ech->inner_hello_msg;
        if (!burrow__tls_clone_str(tc13_keep(c), c->config->server_name,
                                   &c->server_name))
            return burrow_err_out_of_memory;
        hs->transcript = ech->inner_transcript;
        c->ech_accepted = true;

        if (!slice_is_nil(sh->encrypted_client_hello)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
            return tc13_err_ech_unexpected_accept;
        }

        if (hs->hello->server_name.len == 0 && sh->server_name_ack) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
            return tc13_err_ech_server_name;
        }
    } else {
        ech->ech_rejected = true;
    }
    return BURROW_NO_ERROR;
}

/* checkServerHelloOrHRR does validity checks that apply to both ServerHello and
 * HelloRetryRequest messages. It sets hs.suite. */
static Error tc13_check_server_hello_or_hrr(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    const TlsServerHelloMsg *sh = hs->server_hello;

    if (sh->supported_version == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_MISSING_EXTENSION);
        return tc13_err_legacy_version_field;
    }

    if (sh->supported_version != TLS_VERSION_TLS13) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_invalid_version;
    }

    if (sh->vers != TLS_VERSION_TLS12) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_legacy_version;
    }

    if (sh->ocsp_stapling || sh->ticket_supported || sh->extended_master_secret ||
        sh->secure_renegotiation_supported || sh->secure_renegotiation.len != 0 ||
        sh->alpn_protocol.len != 0 || sh->scts.len != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return tc13_err_forbidden_extension;
    }

    if (!bytes_equal(hs->hello->session_id, sh->session_id)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_session_id;
    }

    if (sh->compression_method != TLS_COMPRESSION_NONE) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return tc13_err_compression;
    }

    const TlsSuiteTLS13 *selected_suite = burrow__tls_mutual_cipher_suite_tls13(
        hs->hello->cipher_suites.p, hs->hello->cipher_suites.len, sh->cipher_suite);
    if (hs->suite != NULL && selected_suite != hs->suite) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_changed_suite;
    }
    if (selected_suite == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_unconfigured_suite;
    }
    hs->suite = selected_suite;
    c->cipher_suite = hs->suite->id;

    return BURROW_NO_ERROR;
}

/* sendDummyChangeCipherSpec sends a ChangeCipherSpec record for compatibility
 * with middleboxes that didn't implement TLS correctly. See RFC 8446, Appendix
 * D.4. */
static Error tc13_send_dummy_change_cipher_spec(TlsClientHandshakeStateTLS13 *hs) {
    /* QUIC comes with quic.go. */
    if (hs->sent_dummy_ccs)
        return BURROW_NO_ERROR;
    hs->sent_dummy_ccs = true;

    return burrow__tls_conn_write_change_cipher_record(hs->c);
}

/* Not in Go. The new key share processHelloRetryRequest sends for the group
 * the server selected. */
static Error tc13_hrr_key_share(TlsClientHandshakeStateTLS13 *hs,
                                TlsClientHelloMsg *hello, TlsCurveID curve_id) {
    TlsConn *c = hs->c;
    if (!tc13_contains_curve(hello->supported_curves, curve_id)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_unsupported_group;
    }
    if (tc13_has_key_share(hs->hello->key_shares, curve_id)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_unnecessary_key_share;
    }
    Error err = BURROW_NO_ERROR;
    const TlsKeyExchange *ke = burrow__tls_key_exchange_for_curve_id(curve_id, &err);
    if (ke == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return tc13_err_curve_accepted;
    }
    TlsKeySharePrivateKeys keys;
    memset(&keys, 0, sizeof keys);
    Slice key_shares = slice_nil(&burrow__tls_key_share_type);
    if (!burrow__tls_key_exchange_key_shares(
            ke, hs->a, burrow__tls_config_rand(c->config), &keys, &key_shares, &err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    burrow__tls_key_share_private_keys_free(hs->key_share_keys);
    *hs->key_share_keys = keys;
    hello->key_shares = key_shares;
    /* Do not send the fallback ECDH key share in a HRR response. */
    hello->key_shares.len = 1;
    return BURROW_NO_ERROR;
}

/* Not in Go. The part of processHelloRetryRequest that keeps the inner
 * transcript, and checks whether the server accepted ECH from the HRR's
 * encrypted_client_hello extension. */
static Error tc13_hrr_ech(TlsClientHandshakeStateTLS13 *hs, bool *is_inner_hello) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;
    TlsECHClientContext *ech = hs->ech_context;
    const TlsServerHelloMsg *sh = hs->server_hello;
    Slice ch_hash = hash_sum(a, ech->inner_transcript, slice_nil(TYPE_BYTE));
    if (ch_hash.p == NULL)
        return burrow_err_out_of_memory;
    hash_reset(ech->inner_transcript);
    tc13_write_message_hash(ech->inner_transcript, ch_hash);

    if (!slice_is_nil(sh->encrypted_client_hello)) {
        if (sh->encrypted_client_hello.len != 8) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
            return tc13_err_ech_malformed_hrr;
        }

        Hash conf_transcript =
            burrow__tls_clone_hash(a, ech->inner_transcript, hs->suite->hash);
        if (conf_transcript.vt == NULL) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return tc13_err_clone_hash;
        }
        Byte zeros[8] = {0};
        Slice hrr_hello = bytes_replace(a, sh->original, sh->encrypted_client_hello,
                                        slice_from(zeros, 8, 8, TYPE_BYTE), 1);
        if (hrr_hello.p == NULL && sh->original.len > 0)
            return burrow_err_out_of_memory;
        (void)hash_write(conf_transcript, hrr_hello, NULL);
        Error err = BURROW_NO_ERROR;
        Slice accept_confirmation = burrow__tls_ech_accept_confirmation(
            a, hs->suite, ech->inner_hello->random,
            BURROW_S("hrr ech accept confirmation"), conf_transcript, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }
        if (subtle_constant_time_compare(accept_confirmation,
                                         sh->encrypted_client_hello) == 1) {
            if (!burrow__tls_clone_str(tc13_keep(c), c->config->server_name,
                                       &c->server_name))
                return burrow_err_out_of_memory;
            *is_inner_hello = true;
            c->ech_accepted = true;
        }
    }

    return burrow__tls_transcript_msg(hs->server_hello_msg,
                                      hash_as_io_writer(ech->inner_transcript), a);
}

/* processHelloRetryRequest handles the HRR in hs.serverHello, modifies and
 * resends hs.hello, and reads the new ServerHello into hs.serverHello. */
static Error tc13_process_hello_retry_request(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* The first ClientHello gets double-hashed into the transcript upon a
     * HelloRetryRequest. (The idea is that the server might offload transcript
     * storage to the client in the cookie.) See RFC 8446, Section 4.4.1. */
    Slice ch_hash = hash_sum(hs->a, hs->transcript, slice_nil(TYPE_BYTE));
    if (ch_hash.p == NULL)
        return burrow_err_out_of_memory;
    hash_reset(hs->transcript);
    tc13_write_message_hash(hs->transcript, ch_hash);
    Error err = burrow__tls_transcript_msg(hs->server_hello_msg,
                                           hash_as_io_writer(hs->transcript), hs->a);
    if (BURROW_FAILED(err))
        return err;

    bool is_inner_hello = false;
    TlsClientHelloMsg *hello = hs->hello;
    if (hs->ech_context != NULL) {
        err = tc13_hrr_ech(hs, &is_inner_hello);
        if (BURROW_FAILED(err))
            return err;
        if (is_inner_hello)
            hello = hs->ech_context->inner_hello;
    } else if (!slice_is_nil(hs->server_hello->encrypted_client_hello)) {
        /* Unsolicited ECH extension should be rejected */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return tc13_err_unsolicited_ech;
    }

    /* The only HelloRetryRequest extensions we support are key_share and
     * cookie, and clients must abort the handshake if the HRR would not result
     * in any change in the ClientHello. */
    if (hs->server_hello->selected_group == 0 &&
        slice_is_nil(hs->server_hello->cookie)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_unnecessary_hrr;
    }

    if (!slice_is_nil(hs->server_hello->cookie))
        hello->cookie = hs->server_hello->cookie;

    if (hs->server_hello->server_share.group != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return tc13_err_malformed_key_share_hrr;
    }

    /* If the server sent a key_share extension selecting a group, ensure it's
     * a group we advertised but did not send a key share for, and send a key
     * share for it this time. */
    TlsCurveID curve_id = hs->server_hello->selected_group;
    if (curve_id != 0) {
        err = tc13_hrr_key_share(hs, hello, curve_id);
        if (BURROW_FAILED(err))
            return err;
    }

    if (hello->psk_identities.len > 0) {
        const TlsSuiteTLS13 *psk_suite =
            burrow__tls_cipher_suite_tls13_by_id(hs->session->cipher_suite);
        if (psk_suite == NULL)
            return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        if (psk_suite->hash == hs->suite->hash) {
            /* Update binders and obfuscated_ticket_age. */
            Duration ticket_age =
                time_sub(burrow__tls_config_time(c->config),
                         time_from_unix((int64_t)hs->session->created_at, 0));
            ((TlsPskIdentity *)hello->psk_identities.p)[0].obfuscated_ticket_age =
                (uint32_t)(ticket_age / TIME_MILLISECOND) + hs->session->age_add;

            Hash transcript = burrow__tls_suite13_hash_new(hs->suite)(hs->a);
            if (transcript.vt == NULL)
                return burrow_err_out_of_memory;
            tc13_write_message_hash(transcript, ch_hash);
            err = burrow__tls_transcript_msg(hs->server_hello_msg,
                                             hash_as_io_writer(transcript), hs->a);
            if (BURROW_FAILED(err))
                return err;

            err = burrow__tls_compute_and_update_psk(hello, hs->a, hs->binder_key,
                                                     transcript, hs->suite);
            if (BURROW_FAILED(err))
                return err;
        } else {
            /* Server selected a cipher suite incompatible with the PSK. */
            hello->psk_identities = slice_nil(&burrow__tls_psk_identity_type);
            hello->psk_binders = slice_nil(TYPE_BYTES);
        }
    }

    if (hello->early_data) {
        hello->early_data = false;
        /* c.quicRejectedEarlyData comes with quic.go. */
    }

    if (is_inner_hello) {
        /* Any extensions which have changed in hello, but are mirrored in the
         * outer hello and compressed, need to be copied to the outer hello, so
         * they can be properly decompressed by the server. For now, the only
         * extension which may have changed is keyShares. */
        hs->hello->key_shares = hello->key_shares;
        hs->ech_context->inner_hello = hello;
        err = burrow__tls_transcript_msg(
            hs->ech_context->inner_hello_msg,
            hash_as_io_writer(hs->ech_context->inner_transcript), hs->a);
        if (BURROW_FAILED(err))
            return err;

        err = burrow__tls_compute_and_update_outer_ech_extension(
            hs->hello, hs->ech_context->inner_hello, hs->ech_context, false, hs->a);
        if (BURROW_FAILED(err))
            return err;
    } else {
        hs->hello = hello;
    }

    err = tc13_write(hs, hs->hello_msg);
    if (BURROW_FAILED(err))
        return err;

    /* serverHelloMsg is not included in the transcript */
    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    err = tc13_read(hs, false, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_SERVER_HELLO) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_SERVER_HELLO, msg->kind);
    }
    hs->server_hello_msg = msg;
    hs->server_hello = &msg->server_hello;

    err = tc13_check_server_hello_or_hrr(hs);
    if (BURROW_FAILED(err))
        return err;

    c->did_hrr = true;
    return BURROW_NO_ERROR;
}

static Error tc13_process_server_hello(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    const TlsServerHelloMsg *sh = hs->server_hello;

    if (bytes_equal(
            sh->random,
            slice_from((Byte *)(uintptr_t)burrow__tls_hello_retry_request_random, 32,
                       32, TYPE_BYTE))) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return tc13_err_two_hrr;
    }

    if (sh->cookie.len != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return tc13_err_cookie;
    }

    if (sh->selected_group != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return tc13_err_malformed_key_share;
    }

    if (sh->server_share.group == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_no_key_share;
    }
    if (!tc13_has_key_share(hs->hello->key_shares, sh->server_share.group)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_unsupported_group;
    }

    if (!sh->selected_identity_present)
        return BURROW_NO_ERROR;

    if ((Int)sh->selected_identity >= hs->hello->psk_identities.len) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_invalid_psk;
    }

    if (hs->hello->psk_identities.len != 1 || hs->session == NULL)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    const TlsSuiteTLS13 *psk_suite =
        burrow__tls_cipher_suite_tls13_by_id(hs->session->cipher_suite);
    if (psk_suite == NULL)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    if (psk_suite->hash != hs->suite->hash) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_psk_suite;
    }

    hs->using_psk = true;
    c->did_resume = true;
    c->peer_certificates = hs->session->peer_certificates;
    c->verified_chains = hs->session->verified_chains;
    c->ocsp_response = hs->session->ocsp_response;
    c->scts = hs->session->scts;
    /* Not in Go: c now points into the session, so c holds on to it. Resuming
     * is only done on the first handshake, so there is none there already. */
    if (c->resumed_session != NULL)
        tls_client_session_state_release(c->resumed_session);
    c->resumed_session = hs->cs;
    hs->cs = NULL;
    return BURROW_NO_ERROR;
}

/* Not in Go. writeKeyLog for both secrets of a stage, with an internal_error
 * alert when it fails. */
static Error tc13_write_key_logs(TlsClientHandshakeStateTLS13 *hs, Str client_label,
                                 Slice client_secret, Str server_label,
                                 Slice server_secret) {
    TlsConn *c = hs->c;
    Error err = burrow__tls_config_write_key_log(c->config, client_label,
                                                 hs->hello->random, client_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    err = burrow__tls_config_write_key_log(c->config, server_label, hs->hello->random,
                                           server_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    return BURROW_NO_ERROR;
}

static Error tc13_establish_handshake_keys(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    Error err = BURROW_NO_ERROR;
    const TlsKeyExchange *ke = burrow__tls_key_exchange_for_curve_id(
        hs->server_hello->server_share.group, &err);
    if (ke == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }
    Slice shared_key = burrow__tls_key_exchange_client_shared_secret(
        ke, a, hs->key_share_keys, hs->server_hello->server_share.data, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tc13_err_invalid_key_share;
    }
    c->curve_id = hs->server_hello->server_share.group;

    Tls13EarlySecret fresh;
    const Tls13EarlySecret *early_secret = hs->early_secret;
    if (!hs->using_psk) {
        fresh = burrow__tls13_new_early_secret(
            a, burrow__tls_suite13_hash_new(hs->suite), slice_nil(TYPE_BYTE));
        if (fresh.secret.p == NULL)
            return burrow_err_out_of_memory;
        early_secret = &fresh;
    }

    Tls13HandshakeSecret handshake_secret =
        burrow__tls13_early_secret_handshake_secret(early_secret, a, shared_key);
    if (handshake_secret.secret.p == NULL)
        return burrow_err_out_of_memory;

    Slice client_secret =
        burrow__tls13_handshake_secret_client_handshake_traffic_secret(
            &handshake_secret, a, hs->transcript);
    if (client_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_set_write_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE, client_secret);
    if (BURROW_FAILED(err))
        return err;
    Slice server_secret =
        burrow__tls13_handshake_secret_server_handshake_traffic_secret(
            &handshake_secret, a, hs->transcript);
    if (server_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_set_read_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE, server_secret, false);
    if (BURROW_FAILED(err))
        return err;

    /* Handing the secrets to QUIC comes with quic.go. */

    err = tc13_write_key_logs(
        hs, BURROW_S(TLS_KEY_LOG_LABEL_CLIENT_HANDSHAKE), client_secret,
        BURROW_S(TLS_KEY_LOG_LABEL_SERVER_HANDSHAKE), server_secret);
    if (BURROW_FAILED(err))
        return err;

    hs->master_secret =
        burrow__tls13_handshake_secret_master_secret(&handshake_secret, a);
    if (hs->master_secret.secret.p == NULL)
        return burrow_err_out_of_memory;

    return BURROW_NO_ERROR;
}

static Error tc13_read_server_parameters(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = tc13_read(hs, true, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_ENCRYPTED_EXTENSIONS) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_ENCRYPTED_EXTENSIONS,
                                                    msg->kind);
    }
    const TlsEncryptedExtensionsMsg *encrypted_extensions = &msg->encrypted_extensions;

    err = burrow__tls_check_alpn(hs->hello->alpn_protocols,
                                 encrypted_extensions->alpn_protocol, false);
    if (BURROW_FAILED(err)) {
        /* RFC 8446 specifies that no_application_protocol is sent by servers,
         * but does not specify how clients handle the selection of an
         * incompatible protocol. RFC 9001 Section 8.1 specifies that QUIC
         * clients send no_application_protocol in this case. Always sending
         * no_application_protocol seems reasonable. */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_NO_APPLICATION_PROTOCOL);
        return err;
    }
    if (!burrow__tls_clone_str(tc13_keep(c), encrypted_extensions->alpn_protocol,
                               &c->client_protocol))
        return burrow_err_out_of_memory;

    /* The QUIC transport parameters come with quic.go, and until then there
     * is never a QUIC connection here. */
    if (!slice_is_nil(encrypted_extensions->quic_transport_parameters)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return tc13_err_quic_params;
    }

    if (!hs->hello->early_data && encrypted_extensions->early_data) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
        return tc13_err_early_data;
    }
    /* c.quicRejectedEarlyData comes with quic.go. */
    if (encrypted_extensions->early_data) {
        if (hs->session->cipher_suite != c->cipher_suite) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
            return tc13_err_early_data_suite;
        }
        if (!str_eq(hs->session->alpn_protocol, c->client_protocol)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
            return tc13_err_early_data_alpn;
        }
    }
    if (hs->ech_context != NULL) {
        if (hs->ech_context->ech_rejected) {
            hs->ech_context->retry_configs = encrypted_extensions->ech_retry_configs;
        } else if (!slice_is_nil(encrypted_extensions->ech_retry_configs)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_EXTENSION);
            return tc13_err_ech_retry_after_accept;
        }
    }

    return BURROW_NO_ERROR;
}

/* Not in Go. The CertificateVerify from the server, checked against the
 * transcript up to it, which is the part of readServerCertificate after the
 * certificate is verified. */
static Error tc13_read_server_certificate_verify(TlsClientHandshakeStateTLS13 *hs,
                                                 TlsHandshakeMsg *msg) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    /* certificateVerifyMsg is included in the transcript, but not until after
     * we verify the handshake signature, since the state before this message
     * was sent is used. */
    Error err = tc13_read(hs, false, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_CERTIFICATE_VERIFY) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE_VERIFY,
                                                    msg->kind);
    }
    const TlsCertificateVerifyMsg *cert_verify = &msg->certificate_verify;

    /* See RFC 8446, Section 4.4.3. We don't use
     * hs.hello.supportedSignatureAlgorithms because it might include PKCS#1
     * v1.5 and SHA-1 if the ClientHello also supported TLS 1.2. */
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
        return tc13_err_signature_algorithm;
    }
    uint8_t sig_type;
    CryptoHash sig_hash;
    if (!burrow__tls_type_and_hash_from_signature_scheme(
            cert_verify->signature_algorithm, &sig_type, &sig_hash, NULL))
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    if (sig_type == TLS_SIGNATURE_PKCS1V15 || sig_hash == CRYPTO_SHA1)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
    Slice signed_ = burrow__tls_signed_message(
        a, BURROW_S(TLS_SERVER_SIGNATURE_CONTEXT), hs->transcript);
    if (signed_.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_verify_handshake_signature(sig_type, pub, sig_hash, signed_,
                                                 cert_verify->signature);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
        return fmt_errorf_v("tls: invalid signature by the server certificate: %v",
                            err);
    }
    c->peer_sig_alg = cert_verify->signature_algorithm;

    return burrow__tls_transcript_msg(msg, hash_as_io_writer(hs->transcript), a);
}

static Error tc13_read_server_certificate(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    /* Either a PSK or a certificate is always used, but not both. See RFC
     * 8446, Section 4.1.1. */
    if (hs->using_psk) {
        /* Make sure the connection is still being verified whether or not this
         * is a resumption. Resumptions currently don't reverify certificates
         * so they don't call verifyServerCertificate. See Issue 31641. */
        if (c->config->verify_connection.f != NULL) {
            TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
            Error err = BURROW_CALLF(c->config->verify_connection, &cs);
            if (BURROW_FAILED(err)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
                return err;
            }
        }
        return BURROW_NO_ERROR;
    }

    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = tc13_read(hs, true, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind == TLS_MSG_CERTIFICATE_REQUEST_TLS13) {
        hs->cert_req = &msg->certificate_request_tls13;

        msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
        if (msg == NULL)
            return burrow_err_out_of_memory;
        err = tc13_read(hs, true, msg);
        if (BURROW_FAILED(err))
            return err;
    }

    if (msg->kind != TLS_MSG_CERTIFICATE_TLS13) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE_TLS13,
                                                    msg->kind);
    }
    const TlsCertificateMsgTLS13 *cert_msg = &msg->certificate_tls13;
    if (cert_msg->certificate.certificate.len == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return tc13_err_empty_certificates;
    }

    /* Not in Go: c keeps copies, as the message goes with the handshake. */
    Alloc *keep = tc13_keep(c);
    if (!burrow__tls_clone_bytes_list(
            keep, cert_msg->certificate.signed_certificate_timestamps, &c->scts))
        return burrow_err_out_of_memory;
    Slice ocsp = cert_msg->certificate.ocsp_staple;
    c->ocsp_response = bytes_clone(keep, ocsp);
    if (c->ocsp_response.p == NULL && ocsp.len > 0)
        return burrow_err_out_of_memory;

    err = burrow__tls_conn_verify_server_certificate(c,
                                                     cert_msg->certificate.certificate);
    if (BURROW_FAILED(err))
        return err;

    return tc13_read_server_certificate_verify(hs, msg);
}

static Error tc13_read_server_finished(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    /* finishedMsg is included in the transcript, but not until after we check
     * the client version, since the state before this message was sent is
     * used during verification. */
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    Error err = tc13_read(hs, false, msg);
    if (BURROW_FAILED(err))
        return err;

    if (msg->kind != TLS_MSG_FINISHED) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_FINISHED, msg->kind);
    }

    Slice expected_mac =
        burrow__tls13_finished_hash(hs->suite, a, c->in.traffic_secret, hs->transcript);
    if (expected_mac.p == NULL)
        return burrow_err_out_of_memory;
    if (!hmac_equal(expected_mac, msg->finished.verify_data)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
        return tc13_err_server_finished;
    }

    err = burrow__tls_transcript_msg(msg, hash_as_io_writer(hs->transcript), a);
    if (BURROW_FAILED(err))
        return err;

    /* Derive secrets that take context through the server Finished. */

    hs->traffic_secret = burrow__tls13_master_secret_client_application_traffic_secret(
        &hs->master_secret, a, hs->transcript);
    Slice server_secret = burrow__tls13_master_secret_server_application_traffic_secret(
        &hs->master_secret, a, hs->transcript);
    if (hs->traffic_secret.p == NULL || server_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_conn_set_read_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, server_secret, false);
    if (BURROW_FAILED(err))
        return err;

    err = tc13_write_key_logs(
        hs, BURROW_S(TLS_KEY_LOG_LABEL_CLIENT_TRAFFIC), hs->traffic_secret,
        BURROW_S(TLS_KEY_LOG_LABEL_SERVER_TRAFFIC), server_secret);
    if (BURROW_FAILED(err))
        return err;

    c->ekm = burrow__tls13_export_keying_material(hs->suite, tc13_keep(c),
                                                  &hs->master_secret, hs->transcript);
    if (c->ekm.f == NULL)
        return burrow_err_out_of_memory;

    return BURROW_NO_ERROR;
}

/* Not in Go. The CertificateVerify sendClientCertificate sends for cert. */
static Error tc13_send_client_certificate_verify(TlsClientHandshakeStateTLS13 *hs,
                                                 const TlsCertificate *cert) {
    TlsConn *c = hs->c;
    Alloc *a = hs->a;

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_VERIFY);
    TlsCertificateVerifyMsg *cert_verify_msg = &msg.certificate_verify;
    cert_verify_msg->has_signature_algorithm = true;

    Error err = BURROW_NO_ERROR;
    cert_verify_msg->signature_algorithm = burrow__tls_select_signature_scheme(
        c->vers, cert, hs->cert_req->supported_signature_algorithms, &err);
    if (BURROW_FAILED(err)) {
        /* getClientCertificate returned a certificate incompatible with the
         * CertificateRequestInfo supported signature algorithms. */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return err;
    }

    uint8_t sig_type;
    CryptoHash sig_hash;
    if (!burrow__tls_type_and_hash_from_signature_scheme(
            cert_verify_msg->signature_algorithm, &sig_type, &sig_hash, NULL))
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);

    Slice signed_ = burrow__tls_signed_message(
        a, BURROW_S(TLS_CLIENT_SIGNATURE_CONTEXT), hs->transcript);
    if (signed_.p == NULL)
        return burrow_err_out_of_memory;
    CryptoSignerOpts sign_opts = crypto_hash_as_signer_opts(&sig_hash);
    RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, sig_hash};
    if (sig_type == TLS_SIGNATURE_RSAPSS)
        sign_opts = rsa_pss_options_as_signer_opts(&pss);
    /* Not in Go: Go's cert.PrivateKey.(crypto.Signer) panics on a key that is
     * not a signer. This gives the error the TLS 1.2 handshake gives. */
    Ed25519Signer es;
    CryptoSigner key;
    if (!burrow__tls_private_key_signer(cert->private_key, &es, &key)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return fmt_errorf_v("tls: client certificate private key of type %T does not "
                            "implement crypto.Signer",
                            cert->private_key);
    }
    Slice sig = crypto_sign_message(a, key, burrow__tls_config_rand(c->config), signed_,
                                    sign_opts, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return fmt_errorf_v("tls: failed to sign handshake: %v", err);
    }
    cert_verify_msg->signature = sig;

    return tc13_write(hs, &msg);
}

static Error tc13_send_client_certificate(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    if (hs->cert_req == NULL)
        return BURROW_NO_ERROR;

    if (hs->ech_context != NULL && hs->ech_context->ech_rejected) {
        TlsHandshakeMsg empty;
        burrow__tls_handshake_msg_init(&empty, TLS_MSG_CERTIFICATE_TLS13);
        return tc13_write(hs, &empty);
    }

    TlsCertificateRequestInfo *cri = BURROW_NEW(hs->a, TlsCertificateRequestInfo);
    if (cri == NULL)
        return burrow_err_out_of_memory;
    cri->acceptable_cas = hs->cert_req->certificate_authorities;
    cri->signature_schemes = hs->cert_req->supported_signature_algorithms;
    cri->version = c->vers;
    cri->ctx = hs->ctx;
    Error err = BURROW_NO_ERROR;
    TlsCertificate *cert = burrow__tls_conn_get_client_certificate(c, hs->a, cri, &err);
    if (BURROW_FAILED(err))
        return err;

    /* Not in Go: the certificate only has to last for the handshake, so c
     * keeps a copy. */
    if (!burrow__tls_clone_bytes_list(tc13_keep(c), cert->certificate,
                                      &c->local_certificate))
        return burrow_err_out_of_memory;

    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_CERTIFICATE_TLS13);
    TlsCertificateMsgTLS13 *cert_msg = &msg.certificate_tls13;

    cert_msg->certificate = *cert;
    cert_msg->scts = hs->cert_req->scts && cert->signed_certificate_timestamps.len > 0;
    cert_msg->ocsp_stapling = hs->cert_req->ocsp_stapling && cert->ocsp_staple.len > 0;

    err = tc13_write(hs, &msg);
    if (BURROW_FAILED(err))
        return err;

    /* If we sent an empty certificate message, skip the CertificateVerify. */
    if (cert->certificate.len == 0)
        return BURROW_NO_ERROR;

    return tc13_send_client_certificate_verify(hs, cert);
}

static Error tc13_send_client_finished(TlsClientHandshakeStateTLS13 *hs) {
    TlsConn *c = hs->c;

    TlsHandshakeMsg finished;
    burrow__tls_handshake_msg_init(&finished, TLS_MSG_FINISHED);
    finished.finished.verify_data = burrow__tls13_finished_hash(
        hs->suite, hs->a, c->out.traffic_secret, hs->transcript);
    if (finished.finished.verify_data.p == NULL)
        return burrow_err_out_of_memory;

    Error err = tc13_write(hs, &finished);
    if (BURROW_FAILED(err))
        return err;

    err = burrow__tls_conn_set_write_traffic_secret(
        c, hs->suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, hs->traffic_secret);
    if (BURROW_FAILED(err))
        return err;

    if (!c->config->session_tickets_disabled &&
        c->config->client_session_cache.vt != NULL) {
        c->resumption_secret = burrow__tls13_master_secret_resumption_master_secret(
            &hs->master_secret, tc13_keep(c), hs->transcript);
        if (c->resumption_secret.p == NULL)
            return burrow_err_out_of_memory;
    }

    /* Handing the secret to QUIC comes with quic.go. */

    return BURROW_NO_ERROR;
}
