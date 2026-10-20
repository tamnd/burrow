/* crypto/tls: handshake_server.go, the server side of a TLS 1.0–1.2 handshake
 * and the reading of the ClientHello every server handshake starts with.
 *
 * The TLS 1.3 server handshake is in tls_handshake_server_tls13.c. QUIC comes
 * with quic.go and Encrypted Client Hello with ech.go.
 *
 * As on the client, what the handshake only needs while it runs is made in a
 * scratch arena that goes when it ends, and what the Conn keeps afterwards is
 * copied into the Conn's own arena.
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
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(tsv_err_invalid_ech_ext, "tls: client sent invalid "
                                               "encrypted_client_hello extension");
BURROW_SENTINEL_ERROR(tsv_err_ech_not_here, "tls: Encrypted Client Hello is not "
                                            "supported yet");
BURROW_SENTINEL_ERROR(tsv_err_compression,
                      "tls: client does not support uncompressed connections");
BURROW_SENTINEL_ERROR(tsv_err_initial_renegotiation, "tls: initial handshake had "
                                                     "non-empty renegotiation "
                                                     "extension");
BURROW_SENTINEL_ERROR(tsv_err_no_alpn,
                      "tls: client did not request an application protocol");
BURROW_SENTINEL_ERROR(tsv_err_fallback,
                      "tls: client using inappropriate protocol fallback");
BURROW_SENTINEL_ERROR(tsv_err_session_ems, "tls: session supported "
                                           "extended_master_secret but client does "
                                           "not");
BURROW_SENTINEL_ERROR(tsv_err_bad_sig_alg,
                      "tls: client certificate used with invalid signature "
                      "algorithm");
BURROW_SENTINEL_ERROR(tsv_err_no_transcript, "tls: internal error: did not keep "
                                             "handshake transcript for TLS 1.2");
BURROW_SENTINEL_ERROR(tsv_err_finished, "tls: client's Finished message is incorrect");
BURROW_SENTINEL_ERROR(tsv_err_no_client_cert,
                      "tls: client didn't provide a certificate");
BURROW_SENTINEL_ERROR(tsv_err_mldsa_tls12, "tls: client certificate uses ML-DSA, "
                                           "which requires TLS 1.3");

/* Not in Go. The allocator of what c keeps once the handshake is over. */
static Alloc *tsv_keep(TlsConn *c) {
    return arena_allocator(&c->state);
}

/* serverHandshake performs a TLS handshake as a server. */
Error burrow__tls_server_handshake(TlsConn *c, Context ctx) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    TlsHandshakeMsg *client_hello = NULL;
    Error err = burrow__tls_conn_read_client_hello(c, ctx, a, &client_hello);
    if (BURROW_FAILED(err) || client_hello == NULL) {
        arena_free(&ar);
        return err;
    }

    if (c->vers == TLS_VERSION_TLS13) {
        TlsServerHandshakeStateTLS13 hs;
        memset(&hs, 0, sizeof hs);
        hs.c = c;
        hs.ctx = ctx;
        hs.client_hello_msg = client_hello;
        hs.client_hello = &client_hello->client_hello;
        hs.a = a;
        err = burrow__tls_server_handshake_state_tls13_handshake(&hs);
        arena_free(&ar);
        return err;
    }

    TlsServerHandshakeState hs;
    memset(&hs, 0, sizeof hs);
    hs.c = c;
    hs.ctx = ctx;
    hs.client_hello_msg = client_hello;
    hs.client_hello = &client_hello->client_hello;
    hs.master_secret = slice_nil(TYPE_BYTE);
    hs.a = a;
    err = burrow__tls_server_handshake_state_handshake(&hs);
    arena_free(&ar);
    return err;
}

static Error tsv_check_for_resumption(TlsServerHandshakeState *hs);
static Error tsv_do_resume_handshake(TlsServerHandshakeState *hs);
static Error tsv_do_full_handshake(TlsServerHandshakeState *hs);
static Error tsv_establish_keys(TlsServerHandshakeState *hs);
static Error tsv_read_finished(TlsServerHandshakeState *hs, Byte *out);
static Error tsv_send_session_ticket(TlsServerHandshakeState *hs);
static Error tsv_send_finished(TlsServerHandshakeState *hs, Byte *out);

/* The client has included a session ticket and so we do an abbreviated
 * handshake. */
static Error tsv_resume(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;
    Error err = tsv_do_resume_handshake(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_establish_keys(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_send_session_ticket(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_send_finished(hs, c->server_finished);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__tls_conn_flush(c, &err);
    if (BURROW_FAILED(err))
        return err;
    c->client_finished_is_first = false;
    return tsv_read_finished(hs, NULL);
}

/* The client didn't include a session ticket, or it wasn't valid so we do a
 * full handshake. */
static Error tsv_full(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;
    Error err = burrow__tls_server_handshake_state_pick_cipher_suite(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_do_full_handshake(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_establish_keys(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_read_finished(hs, c->client_finished);
    if (BURROW_FAILED(err))
        return err;
    c->client_finished_is_first = true;
    c->buffering = true;
    err = tsv_send_session_ticket(hs);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_send_finished(hs, NULL);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__tls_conn_flush(c, &err);
    return err;
}

Error burrow__tls_server_handshake_state_handshake(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    Error err = burrow__tls_server_handshake_state_process_client_hello(hs);
    if (BURROW_FAILED(err))
        return err;

    /* For an overview of TLS handshaking, see RFC 5246, Section 7.3. */
    c->buffering = true;
    err = tsv_check_for_resumption(hs);
    if (BURROW_FAILED(err))
        return err;
    err = hs->session_state != NULL ? tsv_resume(hs) : tsv_full(hs);
    if (BURROW_FAILED(err))
        return err;

    TlsEkmMaster m = {c->vers, hs->suite, hs->master_secret, hs->client_hello->random,
                      hs->hello->random};
    c->ekm = burrow__tls_ekm_func(tsv_keep(c), &m);
    if (c->ekm.f == NULL)
        return burrow_err_out_of_memory;
    sync_atomic_bool_store(&c->is_handshake_complete, true);

    return BURROW_NO_ERROR;
}

/* Not in Go. The part of processECHClientHello that runs before any keys are
 * needed: the extension is checked the way parseECHExt checks it, with the
 * alerts Go sends. */
static Error tsv_check_ech_ext(TlsConn *c, Slice ext) {
    const Byte *p = ext.p;
    bool malformed = false;
    if (p[0] == 1) {
        /* innerECHExt */
        malformed = ext.len != 1;
    } else if (p[0] == 0) {
        /* outerECHExt: the KDF and AEAD IDs, the config ID, then encap and
         * payload, each with a two byte length. */
        Int i = 6;
        for (int field = 0; field < 2 && !malformed; field++) {
            if (ext.len - i < 2) {
                malformed = true;
                break;
            }
            Int n = (Int)p[i] << 8 | (Int)p[i + 1];
            i += 2;
            if (ext.len - i < n)
                malformed = true;
            i += n;
        }
        if (ext.len < 6)
            malformed = true;
    } else {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tsv_err_invalid_ech_ext;
    }
    if (malformed) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return tsv_err_invalid_ech_ext;
    }
    return BURROW_NO_ERROR;
}

/* readClientHello reads a ClientHello message and selects the protocol
 * version. */
Error burrow__tls_conn_read_client_hello(TlsConn *c, Context ctx, Alloc *a,
                                         TlsHandshakeMsg **out) {
    /* clientHelloMsg is included in the transcript, but we haven't initialized
     * it yet. The respective handshake functions will record it themselves. */
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    IoWriter none = {NULL, NULL};
    Error err = burrow__tls_conn_read_handshake(c, none, a, msg);
    if (BURROW_FAILED(err))
        return err;
    if (msg->kind != TLS_MSG_CLIENT_HELLO) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CLIENT_HELLO, msg->kind);
    }
    TlsClientHelloMsg *client_hello = &msg->client_hello;

    /* ECH processing has to be done before we do any other negotiation based
     * on the contents of the client hello, since we may swap it out
     * completely. */
    if (client_hello->encrypted_client_hello.len != 0) {
        err = tsv_check_ech_ext(c, client_hello->encrypted_client_hello);
        if (BURROW_FAILED(err))
            return err;
        /* Not in Go: an outer ClientHello the server has keys for would be
         * decrypted here, which comes with ech.go. Until then the handshake
         * ends with an alert. */
        const Byte *ech_type = client_hello->encrypted_client_hello.p;
        if (ech_type[0] == 0 &&
            (c->config->encrypted_client_hello_keys.len != 0 ||
             c->config->get_encrypted_client_hello_keys.f != NULL)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return tsv_err_ech_not_here;
        }
    }

    TlsConfig *config_for_client = NULL;
    TlsConfig *original_config = c->config;
    if (c->config->get_config_for_client.f != NULL) {
        TlsClientHelloInfo *chi =
            burrow__tls_client_hello_info(a, ctx, c, client_hello);
        if (chi == NULL)
            return burrow_err_out_of_memory;
        config_for_client = BURROW_CALLF(c->config->get_config_for_client, chi, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return err;
        }
        if (config_for_client != NULL)
            c->config = config_for_client;
    }
    if (c->ticket_keys != NULL)
        burrow__tls_ticket_keys_release(c->ticket_keys);
    c->ticket_keys =
        burrow__tls_config_ticket_keys(original_config, config_for_client, &err);
    if (BURROW_FAILED(err))
        return err;

    uint16_t from_max[4];
    Slice client_versions = client_hello->supported_versions;
    if (client_hello->vers >= TLS_VERSION_TLS13 && client_versions.len == 0) {
        /* RFC 8446 4.2.1 indicates when the supported_versions extension is not
         * sent, compatible servers MUST negotiate TLS 1.2 or earlier if
         * supported, even if the client legacy version is TLS 1.3 or later.
         *
         * Since we reject empty extensionSupportedVersions in the client hello
         * unmarshal finding the supportedVersions empty indicates the extension
         * was not present. */
        Int n = burrow__tls_supported_versions_from_max(TLS_VERSION_TLS12, from_max);
        client_versions = slice_from(from_max, n, n, TYPE_UINT16);
    } else if (client_versions.len == 0) {
        Int n = burrow__tls_supported_versions_from_max(client_hello->vers, from_max);
        client_versions = slice_from(from_max, n, n, TYPE_UINT16);
    }
    if (!burrow__tls_config_mutual_version(c->config, TLS_ROLE_SERVER, false,
                                           client_versions, &c->vers)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_PROTOCOL_VERSION);
        return fmt_errorf_v("tls: client offered only unsupported versions: %x",
                            client_versions);
    }
    c->have_vers = true;
    c->in.version = c->vers;
    c->out.version = c->vers;

    /* Go checks here that a hello with an outer ECH, which it decrypted,
     * negotiated TLS 1.3. That comes with ech.go. */

    *out = msg;
    return BURROW_NO_ERROR;
}

/* Not in Go. supportedPoints = []uint8{pointFormatUncompressed}. */
static const Byte tsv_uncompressed[1] = {TLS_POINT_FORMAT_UNCOMPRESSED};

/* The part of processClientHello that looks at the certificate's key. */
static Error tsv_check_private_key(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;
    Ed25519Signer es;
    CryptoSigner priv;
    if (burrow__tls_private_key_signer(hs->cert->private_key, &es, &priv)) {
        CryptoPublicKey pub = crypto_signer_public(priv);
        if (pub.t == TYPE_ECDSA_PUBLIC_KEY || pub.t == TYPE_ED25519_PUBLIC_KEY) {
            hs->ec_sign_ok = true;
        } else if (pub.t == TYPE_RSA_PUBLIC_KEY) {
            hs->rsa_sign_ok = true;
        } else if (pub.t == TYPE_MLDSA_PUBLIC_KEY) {
            /* ML-DSA can only be used with TLS 1.3. */
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: ML-DSA certificates require TLS 1.3, but client "
                                "negotiated %s",
                                tls_version_name(hs->a, c->vers));
        } else {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: unsupported signing key type (%T)", pub);
        }
    }
    /* Not in Go: of the keys a Certificate can hold, only an RSA key is a
     * crypto.Decrypter, and its public key is an RSA one. */
    if (hs->cert->private_key.t == TYPE_RSA_PRIVATE_KEY &&
        hs->cert->private_key.data != NULL)
        hs->rsa_decrypt_ok = true;
    return BURROW_NO_ERROR;
}

Error burrow__tls_server_handshake_state_process_client_hello(
    TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    hs->hello_msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (hs->hello_msg == NULL)
        return burrow_err_out_of_memory;
    burrow__tls_handshake_msg_init(hs->hello_msg, TLS_MSG_SERVER_HELLO);
    hs->hello = &hs->hello_msg->server_hello;
    hs->hello->vers = c->vers;

    bool found_compression = false;
    /* We only support null compression, so check that the client offered
     * it. */
    const Byte *methods = hs->client_hello->compression_methods.p;
    for (Int i = 0; i < hs->client_hello->compression_methods.len; i++) {
        if (methods[i] == TLS_COMPRESSION_NONE) {
            found_compression = true;
            break;
        }
    }

    if (!found_compression) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return tsv_err_compression;
    }

    hs->hello->random = slice_make(hs->a, TYPE_BYTE, 32, 32);
    if (hs->hello->random.p == NULL)
        return burrow_err_out_of_memory;
    Slice server_random = hs->hello->random;
    /* Downgrade protection canaries. See RFC 8446, Section 4.1.3.
     *
     * Go also sets them when testingOnlyForceDowngradeCanary is, which comes
     * with the tests that set it. */
    uint16_t max_vers =
        burrow__tls_config_max_supported_version(c->config, TLS_ROLE_SERVER, false);
    if (max_vers >= TLS_VERSION_TLS12 && c->vers < max_vers) {
        Byte *canary = (Byte *)server_random.p + 24;
        if (c->vers == TLS_VERSION_TLS12)
            memcpy(canary, TLS_DOWNGRADE_CANARY_TLS12, 8);
        else
            memcpy(canary, TLS_DOWNGRADE_CANARY_TLS11, 8);
        server_random = slice_sub(server_random, 0, 24);
    }
    Error err = BURROW_NO_ERROR;
    io_read_full(burrow__tls_config_rand(c->config), server_random, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }

    if (hs->client_hello->secure_renegotiation.len != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tsv_err_initial_renegotiation;
    }

    hs->hello->extended_master_secret = hs->client_hello->extended_master_secret;
    hs->hello->secure_renegotiation_supported =
        hs->client_hello->secure_renegotiation_supported;
    hs->hello->compression_method = TLS_COMPRESSION_NONE;
    if (hs->client_hello->server_name.len > 0) {
        if (!burrow__tls_clone_str(tsv_keep(c), hs->client_hello->server_name,
                                   &c->server_name))
            return burrow_err_out_of_memory;
    }

    Str selected_proto = BURROW_STR_EMPTY;
    err = burrow__tls_negotiate_alpn(c->config->next_protos,
                                     hs->client_hello->alpn_protocols, false,
                                     &selected_proto);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_NO_APPLICATION_PROTOCOL);
        return err;
    }
    hs->hello->alpn_protocol = selected_proto;
    if (!burrow__tls_clone_str(tsv_keep(c), selected_proto, &c->client_protocol))
        return burrow_err_out_of_memory;

    TlsClientHelloInfo *chi =
        burrow__tls_client_hello_info(hs->a, hs->ctx, c, hs->client_hello);
    if (chi == NULL)
        return burrow_err_out_of_memory;
    hs->cert = burrow__tls_config_get_certificate(c->config, chi, &err);
    if (BURROW_FAILED(err)) {
        if (errors_is(err, burrow__tls_err_no_certificates))
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNRECOGNIZED_NAME);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }

    if (hs->client_hello->scts)
        hs->hello->scts = hs->cert->signed_certificate_timestamps;

    hs->ecdhe_ok = burrow__tls_supports_ecdhe(c->config, c->vers,
                                              hs->client_hello->supported_curves,
                                              hs->client_hello->supported_points, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_MISSING_EXTENSION);
        return err;
    }

    if (hs->ecdhe_ok && hs->client_hello->supported_points.len > 0) {
        /* Although omitting the ec_point_formats extension is permitted, some
         * old OpenSSL version will refuse to handshake if not present.
         *
         * Per RFC 4492, section 5.1.2, implementations MUST support the
         * uncompressed point format. See golang.org/issue/31943. */
        hs->hello->supported_points =
            slice_from((Byte *)(uintptr_t)tsv_uncompressed, 1, 1, TYPE_BYTE);
    }

    return tsv_check_private_key(hs);
}

/* negotiateALPN picks a shared ALPN protocol that both sides support in server
 * preference order. If ALPN is not configured or the peer doesn't support it,
 * it returns "" and no error. */
Error burrow__tls_negotiate_alpn(Slice server_protos, Slice client_protos, bool quic,
                                 Str *out) {
    *out = BURROW_STR_EMPTY;
    if (server_protos.len == 0 || client_protos.len == 0) {
        if (quic && server_protos.len != 0) {
            /* RFC 9001, Section 8.1 */
            return tsv_err_no_alpn;
        }
        return BURROW_NO_ERROR;
    }
    bool http11fallback = false;
    const Str *ss = server_protos.p;
    const Str *cs = client_protos.p;
    for (Int i = 0; i < server_protos.len; i++) {
        for (Int j = 0; j < client_protos.len; j++) {
            if (str_eq(ss[i], cs[j])) {
                *out = ss[i];
                return BURROW_NO_ERROR;
            }
            if (str_eq(ss[i], BURROW_S("h2")) && str_eq(cs[j], BURROW_S("http/1.1")))
                http11fallback = true;
        }
    }
    /* As a special case, let http/1.1 clients connect to h2 servers as if they
     * didn't support ALPN. We used not to enforce protocol overlap, so over
     * time a number of HTTP servers were configured with only "h2", but
     * expected to accept connections from "http/1.1" clients. See Issue
     * 46310. */
    if (http11fallback)
        return BURROW_NO_ERROR;
    return fmt_errorf_v("tls: client requested unsupported application protocols (%q)",
                        client_protos);
}

/* cipherSuiteOk, in the shape selectCipherSuite takes. */
static bool tsv_cipher_suite_ok(const TlsSuite *c, void *env) {
    const TlsServerHandshakeState *hs = env;
    if ((c->flags & TLS_SUITE_ECDHE) != 0) {
        if (!hs->ecdhe_ok)
            return false;
        if ((c->flags & TLS_SUITE_EC_SIGN) != 0) {
            if (!hs->ec_sign_ok)
                return false;
        } else if (!hs->rsa_sign_ok) {
            return false;
        }
    } else if (!hs->rsa_decrypt_ok) {
        return false;
    }
    if (hs->c->vers < TLS_VERSION_TLS12 && (c->flags & TLS_SUITE_TLS12) != 0)
        return false;
    return true;
}

Error burrow__tls_server_handshake_state_pick_cipher_suite(
    TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    const uint16_t *offered = hs->client_hello->cipher_suites.p;
    Int noffered = hs->client_hello->cipher_suites.len;
    Slice preference_list = burrow__tls_config_cipher_suites(
        c->config, hs->a, burrow__tls_is_aes_gcm_preferred(offered, noffered));
    if (preference_list.p == NULL)
        return burrow_err_out_of_memory;

    hs->suite =
        burrow__tls_select_cipher_suite(preference_list.p, preference_list.len, offered,
                                        noffered, tsv_cipher_suite_ok, hs);
    if (hs->suite == NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return fmt_errorf_v("tls: no cipher suite supported by both client and server; "
                            "client offered: %x",
                            hs->client_hello->cipher_suites);
    }
    c->cipher_suite = hs->suite->id;

    for (Int i = 0; i < noffered; i++) {
        if (offered[i] == TLS_TLS_FALLBACK_SCSV) {
            /* The client is doing a fallback connection. See RFC 7507. */
            if (hs->client_hello->vers < burrow__tls_config_max_supported_version(
                                             c->config, TLS_ROLE_SERVER, false)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INAPPROPRIATE_FALLBACK);
                return tsv_err_fallback;
            }
            break;
        }
    }

    return BURROW_NO_ERROR;
}

/* Not in Go. Whether the client still offers the session's cipher suite, and
 * we support it too: the suite, or NULL. */
static const TlsSuite *tsv_session_suite(TlsServerHandshakeState *hs,
                                         const TlsSessionState *ss, Error *err) {
    bool cipher_suite_ok = false;
    /* Check that the client is still offering the ciphersuite in the session. */
    const uint16_t *offered = hs->client_hello->cipher_suites.p;
    for (Int i = 0; i < hs->client_hello->cipher_suites.len; i++) {
        if (offered[i] == ss->cipher_suite) {
            cipher_suite_ok = true;
            break;
        }
    }
    if (!cipher_suite_ok)
        return NULL;

    /* Check that we also support the ciphersuite from the session. */
    Slice supported = burrow__tls_config_supported_cipher_suites(hs->c->config, hs->a);
    if (supported.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    return burrow__tls_select_cipher_suite(&ss->cipher_suite, 1, supported.p,
                                           supported.len, tsv_cipher_suite_ok, hs);
}

/* Not in Go. The checks checkForResumption makes of a session it has: whether
 * it can be resumed, or the error that ends the handshake. */
static bool tsv_can_resume(TlsServerHandshakeState *hs, const TlsSessionState *ss,
                           const TlsSuite **suite, Error *err) {
    TlsConn *c = hs->c;
    Time now = burrow__tls_config_time(c->config);

    /* TLS 1.2 tickets don't natively have a lifetime, but we want to avoid
     * re-wrapping the same master secret in different tickets over and over
     * for too long, weakening forward secrecy. */
    Time created_at = time_from_unix((int64_t)ss->created_at, 0);
    if (time_sub(now, created_at) > TLS_MAX_SESSION_TICKET_LIFETIME)
        return false;

    /* Never resume a session for a different TLS version. */
    if (c->vers != ss->version)
        return false;

    *suite = tsv_session_suite(hs, ss, err);
    if (*suite == NULL)
        return false;

    bool session_has_client_certs = ss->peer_certificates.len != 0;
    bool need_client_certs = burrow__tls_requires_client_cert(c->config->client_auth);
    if (need_client_certs && !session_has_client_certs)
        return false;
    if (session_has_client_certs && c->config->client_auth == TLS_NO_CLIENT_CERT)
        return false;
    if (session_has_client_certs &&
        time_after(now,
                   ((X509Certificate *const *)ss->peer_certificates.p)[0]->not_after))
        return false;
    X509ExtKeyUsage client_auth[1] = {X509_EXT_KEY_USAGE_CLIENT_AUTH};
    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.current_time = now;
    opts.roots = c->config->client_cas;
    opts.key_usages = slice_from(client_auth, 1, 1, TYPE_INT);
    if (session_has_client_certs &&
        c->config->client_auth >= TLS_VERIFY_CLIENT_CERT_IF_GIVEN &&
        !burrow__tls_any_valid_verified_chain(ss->verified_chains, opts))
        return false;

    /* RFC 7627, Section 5.3 */
    if (!ss->ext_master_secret && hs->client_hello->extended_master_secret)
        return false;
    if (ss->ext_master_secret && !hs->client_hello->extended_master_secret) {
        /* Aborting is somewhat harsh, but it's a MUST and it would indicate a
         * weird downgrade in client capabilities. */
        BURROW_OUT(err, tsv_err_session_ems);
        return false;
    }
    /* FIPS 140-3 requires the use of Extended Master Secret. There is no FIPS
     * 140-3 mode here. */
    return true;
}

/* checkForResumption reports whether we should perform resumption on this
 * connection.
 *
 * Not in Go: the session is the Conn's from here on when it is resumed, and is
 * freed at once when it is not. */
static Error tsv_check_for_resumption(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    if (c->config->session_tickets_disabled)
        return BURROW_NO_ERROR;

    TlsSessionState *session_state = NULL;
    Error err = BURROW_NO_ERROR;
    if (c->config->unwrap_session.f != NULL) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        TlsSessionState *ss = BURROW_CALLF(c->config->unwrap_session,
                                           hs->client_hello->session_ticket, &cs, &err);
        if (BURROW_FAILED(err)) {
            if (ss != NULL)
                tls_session_state_free(ss);
            return err;
        }
        if (ss == NULL)
            return BURROW_NO_ERROR;
        session_state = ss;
    } else {
        Slice plaintext = burrow__tls_config_decrypt_ticket(
            c->config, hs->a, hs->client_hello->session_ticket, c->ticket_keys);
        if (slice_is_nil(plaintext))
            return BURROW_NO_ERROR;
        TlsSessionState *ss = tls_parse_session_state(c->a, plaintext, &err);
        if (ss == NULL)
            return BURROW_NO_ERROR;
        session_state = ss;
    }

    const TlsSuite *suite = NULL;
    if (!tsv_can_resume(hs, session_state, &suite, &err)) {
        tls_session_state_free(session_state);
        return err;
    }

    c->peer_certificates = session_state->peer_certificates;
    c->ocsp_response = session_state->ocsp_response;
    c->scts = session_state->scts;
    c->verified_chains = session_state->verified_chains;
    c->ext_master_secret = session_state->ext_master_secret;
    hs->session_state = session_state;
    hs->suite = suite;
    c->curve_id = session_state->curve_id;
    c->did_resume = true;
    /* Not in Go: c now points into the session, so c holds on to it. */
    if (c->server_session != NULL)
        tls_session_state_free(c->server_session);
    c->server_session = session_state;
    return BURROW_NO_ERROR;
}

/* Not in Go. The next handshake message, written to the finished hash. */
static Error tsv_read(TlsServerHandshakeState *hs, TlsHandshakeMsg *msg) {
    return burrow__tls_conn_read_handshake(
        hs->c, burrow__tls_finished_hash_writer(&hs->finished_hash), hs->a, msg);
}

/* Not in Go. msg written to the connection and the finished hash. */
static Error tsv_write(TlsServerHandshakeState *hs, const TlsHandshakeMsg *msg) {
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_handshake_record(
        hs->c, msg, burrow__tls_finished_hash_writer(&hs->finished_hash), &err);
    return err;
}

/* Not in Go. Config.VerifyConnection, when there is one, with the alert Go
 * sends when it fails. */
static Error tsv_verify_connection(TlsConn *c) {
    if (c->config->verify_connection.f == NULL)
        return BURROW_NO_ERROR;
    TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
    Error err = BURROW_CALLF(c->config->verify_connection, &cs);
    if (BURROW_FAILED(err))
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
    return err;
}

static Error tsv_do_resume_handshake(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    hs->hello->cipher_suite = hs->suite->id;
    c->cipher_suite = hs->suite->id;
    /* We echo the client's session ID in the ServerHello to let it know that
     * we're doing a resumption. */
    hs->hello->session_id = hs->client_hello->session_id;
    /* We always send a new session ticket, even if it wraps the same master
     * secret and it's potentially encrypted with the same key, to help the
     * client avoid cross-connection tracking from a network observer. */
    hs->hello->ticket_supported = true;
    if (!burrow__tls_new_finished_hash(hs->a, c->vers, hs->suite, &hs->finished_hash))
        return burrow_err_out_of_memory;
    burrow__tls_finished_hash_discard_handshake_buffer(&hs->finished_hash);
    Error err = burrow__tls_transcript_msg(
        hs->client_hello_msg, burrow__tls_finished_hash_writer(&hs->finished_hash),
        hs->a);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_write(hs, hs->hello_msg);
    if (BURROW_FAILED(err))
        return err;

    err = tsv_verify_connection(c);
    if (BURROW_FAILED(err))
        return err;

    hs->master_secret = hs->session_state->secret;

    return BURROW_NO_ERROR;
}

/* The ServerHello, Certificate and CertificateStatus messages doFullHandshake
 * starts with. */
/* The NOLINTNEXTLINE lines below are clang-analyzer paths where an earlier
 * step failed but its error looked like no error, so hs->cert or hs->suite was
 * never set. Every step that leaves them NULL returns an error. */
static Error tsv_send_hello_and_certificate(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;

    /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
    if (hs->client_hello->ocsp_stapling && hs->cert->ocsp_staple.len > 0)
        hs->hello->ocsp_stapling = true;

    if (hs->client_hello->server_name.len != 0)
        hs->hello->server_name_ack = true;

    hs->hello->ticket_supported =
        hs->client_hello->ticket_supported && !c->config->session_tickets_disabled;
    /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
    hs->hello->cipher_suite = hs->suite->id;

    if (!burrow__tls_new_finished_hash(hs->a, c->vers, hs->suite, &hs->finished_hash))
        return burrow_err_out_of_memory;
    if (c->config->client_auth == TLS_NO_CLIENT_CERT) {
        /* No need to keep a full record of the handshake if client
         * certificates won't be used. */
        burrow__tls_finished_hash_discard_handshake_buffer(&hs->finished_hash);
    }
    Error err = burrow__tls_transcript_msg(
        hs->client_hello_msg, burrow__tls_finished_hash_writer(&hs->finished_hash),
        hs->a);
    if (BURROW_FAILED(err))
        return err;
    err = tsv_write(hs, hs->hello_msg);
    if (BURROW_FAILED(err))
        return err;

    TlsHandshakeMsg cert_msg;
    burrow__tls_handshake_msg_init(&cert_msg, TLS_MSG_CERTIFICATE);
    cert_msg.certificate.certificates = hs->cert->certificate;
    /* Set localCertificate here, rather than at certificate selection time, so
     * that it is only populated when a certificate is actually presented to
     * the peer, and not on resumed connections.
     *
     * Not in Go: the certificate only has to last for the handshake, so c
     * keeps a copy. */
    if (!burrow__tls_clone_bytes_list(tsv_keep(c), hs->cert->certificate,
                                      &c->local_certificate))
        return burrow_err_out_of_memory;
    err = tsv_write(hs, &cert_msg);
    if (BURROW_FAILED(err))
        return err;

    if (hs->hello->ocsp_stapling) {
        TlsHandshakeMsg cert_status;
        burrow__tls_handshake_msg_init(&cert_status, TLS_MSG_CERTIFICATE_STATUS);
        cert_status.certificate_status.response = hs->cert->ocsp_staple;
        err = tsv_write(hs, &cert_status);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

/* The CertificateRequest doFullHandshake sends, into msg. */
static Error tsv_send_certificate_request(TlsServerHandshakeState *hs,
                                          TlsHandshakeMsg *msg) {
    TlsConn *c = hs->c;
    TlsCertificateRequestMsg *cert_req = &msg->certificate_request;
    static const Byte certificate_types[2] = {TLS_CERT_TYPE_RSA_SIGN,
                                              TLS_CERT_TYPE_ECDSA_SIGN};
    cert_req->certificate_types =
        slice_from((Byte *)(uintptr_t)certificate_types, 2, 2, TYPE_BYTE);
    if (c->vers >= TLS_VERSION_TLS12) {
        cert_req->has_signature_algorithm = true;
        cert_req->supported_signature_algorithms =
            burrow__tls_supported_signature_algorithms(hs->a, c->vers, c->vers);
        if (cert_req->supported_signature_algorithms.p == NULL)
            return burrow_err_out_of_memory;
    }

    /* An empty list of certificateAuthorities signals to the client that it
     * may send any certificate in response to our request. When we know the
     * CAs we trust, then we can send them down, so that the client can choose
     * an appropriate certificate to give to us. */
    if (c->config->client_cas != NULL) {
        cert_req->certificate_authorities =
            x509_cert_pool_subjects(c->config->client_cas, hs->a);
        if (cert_req->certificate_authorities.p == NULL &&
            cert_req->certificate_authorities.len > 0)
            return burrow_err_out_of_memory;
    }
    return tsv_write(hs, msg);
}

/* The CertificateVerify a client with a certificate sends, checked against
 * pub. */
static Error tsv_read_certificate_verify(TlsServerHandshakeState *hs,
                                         CryptoPublicKey pub,
                                         const TlsCertificateRequestMsg *cert_req) {
    TlsConn *c = hs->c;

    /* certificateVerifyMsg is included in the transcript, but not until after
     * we verify the handshake signature, since the state before this message
     * was sent is used. */
    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    IoWriter none = {NULL, NULL};
    Error err = burrow__tls_conn_read_handshake(c, none, hs->a, msg);
    if (BURROW_FAILED(err))
        return err;
    if (msg->kind != TLS_MSG_CERTIFICATE_VERIFY) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE_VERIFY,
                                                    msg->kind);
    }
    const TlsCertificateVerifyMsg *cert_verify = &msg->certificate_verify;

    uint8_t sig_type;
    CryptoHash sig_hash;
    if (c->vers >= TLS_VERSION_TLS12) {
        if (!burrow__tls_is_supported_signature_algorithm(
                cert_verify->signature_algorithm,
                cert_req->supported_signature_algorithms)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return tsv_err_bad_sig_alg;
        }
        if (!burrow__tls_type_and_hash_from_signature_scheme(
                cert_verify->signature_algorithm, &sig_type, &sig_hash, NULL))
            return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        /* Go counts the tlssha1 GODEBUG setting here when sigHash is SHA-1. */
        if (slice_is_nil(hs->finished_hash.buffer)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return tsv_err_no_transcript;
        }
        /* Not in Go: a transcript that could not all be kept. */
        if (hs->finished_hash.oom) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return burrow_err_out_of_memory;
        }
        err = burrow__tls_verify_handshake_signature(
            sig_type, pub, sig_hash, hs->finished_hash.buffer, cert_verify->signature);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
            return fmt_errorf_v("tls: invalid signature by the client certificate: %s",
                                error_text(err));
        }
    } else {
        if (!burrow__tls_legacy_type_and_hash_from_public_key(pub, &sig_type, &sig_hash,
                                                              &err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return err;
        }
        Slice signed_ = burrow__tls_finished_hash_for_client_certificate(
            &hs->finished_hash, hs->a, sig_type);
        if (signed_.p == NULL)
            return burrow_err_out_of_memory;
        err = burrow__tls_verify_legacy_handshake_signature(
            sig_type, pub, sig_hash, signed_, cert_verify->signature);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECRYPT_ERROR);
            return fmt_errorf_v("tls: invalid signature by the client certificate: %s",
                                error_text(err));
        }
    }

    c->peer_sig_alg = cert_verify->signature_algorithm;

    return burrow__tls_transcript_msg(
        msg, burrow__tls_finished_hash_writer(&hs->finished_hash), hs->a);
}

/* The part of doFullHandshake from the ServerKeyExchange on, with the key
 * agreement of the suite. */
static Error tsv_full_handshake_key_exchange(TlsServerHandshakeState *hs,
                                             TlsKeyAgreement *key_agreement) {
    TlsConn *c = hs->c;
    Error err = BURROW_NO_ERROR;

    TlsServerKeyExchangeMsg *skx =
        burrow__tls_key_agreement_generate_server_key_exchange(
            key_agreement, hs->a, c->config, hs->cert, hs->client_hello, hs->hello,
            &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return err;
    }
    if (skx != NULL) {
        if (key_agreement->is_ecdhe) {
            c->curve_id = key_agreement->curve_id;
            c->peer_sig_alg = key_agreement->signature_algorithm;
        }
        TlsHandshakeMsg skx_msg;
        burrow__tls_handshake_msg_init(&skx_msg, TLS_MSG_SERVER_KEY_EXCHANGE);
        skx_msg.server_key_exchange = *skx;
        err = tsv_write(hs, &skx_msg);
        if (BURROW_FAILED(err))
            return err;
    }

    /* Not in Go: cert_req stays empty when no certificate is requested. */
    TlsHandshakeMsg cert_req_msg;
    burrow__tls_handshake_msg_init(&cert_req_msg, TLS_MSG_CERTIFICATE_REQUEST);
    if (c->config->client_auth >= TLS_REQUEST_CLIENT_CERT) {
        /* Request a client certificate */
        err = tsv_send_certificate_request(hs, &cert_req_msg);
        if (BURROW_FAILED(err))
            return err;
    }

    TlsHandshakeMsg hello_done;
    burrow__tls_handshake_msg_init(&hello_done, TLS_MSG_SERVER_HELLO_DONE);
    err = tsv_write(hs, &hello_done);
    if (BURROW_FAILED(err))
        return err;

    (void)burrow__tls_conn_flush(c, &err);
    if (BURROW_FAILED(err))
        return err;

    CryptoPublicKey pub = {0}; /* public key for client auth, if any */

    TlsHandshakeMsg *msg = BURROW_NEW(hs->a, TlsHandshakeMsg);
    if (msg == NULL)
        return burrow_err_out_of_memory;
    err = tsv_read(hs, msg);
    if (BURROW_FAILED(err))
        return err;

    /* If we requested a client certificate, then the client must send a
     * certificate message, even if it's empty. */
    if (c->config->client_auth >= TLS_REQUEST_CLIENT_CERT) {
        if (msg->kind != TLS_MSG_CERTIFICATE) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
            return burrow__tls_unexpected_message_error(TLS_MSG_CERTIFICATE, msg->kind);
        }

        TlsCertificate certificate;
        memset(&certificate, 0, sizeof certificate);
        certificate.certificate = msg->certificate.certificates;
        certificate.ocsp_staple = slice_nil(TYPE_BYTE);
        certificate.signed_certificate_timestamps = slice_nil(TYPE_BYTES);
        err = burrow__tls_conn_process_certs_from_client(c, &certificate);
        if (BURROW_FAILED(err))
            return err;
        if (msg->certificate.certificates.len != 0)
            pub = ((X509Certificate *const *)c->peer_certificates.p)[0]->public_key;

        err = tsv_read(hs, msg);
        if (BURROW_FAILED(err))
            return err;
    }
    err = tsv_verify_connection(c);
    if (BURROW_FAILED(err))
        return err;

    /* Get client key exchange */
    if (msg->kind != TLS_MSG_CLIENT_KEY_EXCHANGE) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_CLIENT_KEY_EXCHANGE,
                                                    msg->kind);
    }

    Slice pre_master_secret = burrow__tls_key_agreement_process_client_key_exchange(
        key_agreement, hs->a, c->config, hs->cert, &msg->client_key_exchange, c->vers,
        &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return err;
    }
    if (hs->hello->extended_master_secret) {
        c->ext_master_secret = true;
        hs->master_secret = burrow__tls_ext_master_from_pre_master_secret(
            hs->a, c->vers, hs->suite, pre_master_secret,
            burrow__tls_finished_hash_sum(&hs->finished_hash, hs->a));
    } else {
        /* FIPS 140-3 requires the use of Extended Master Secret. There is no
         * FIPS 140-3 mode here. */
        hs->master_secret = burrow__tls_master_from_pre_master_secret(
            hs->a, c->vers, hs->suite, pre_master_secret, hs->client_hello->random,
            hs->hello->random);
    }
    if (hs->master_secret.p == NULL)
        return burrow_err_out_of_memory;
    err = burrow__tls_config_write_key_log(c->config, BURROW_S(TLS_KEY_LOG_LABEL_TLS12),
                                           hs->client_hello->random, hs->master_secret);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return err;
    }

    /* If we received a client cert in response to our certificate request
     * message, the client will send us a certificateVerifyMsg immediately after
     * the clientKeyExchangeMsg. This message is a digest of all preceding
     * handshake-layer messages that is signed using the private key
     * corresponding to the client's certificate. This allows us to verify that
     * the client is in possession of the private key of the certificate. */
    if (c->peer_certificates.len > 0) {
        err = tsv_read_certificate_verify(hs, pub, &cert_req_msg.certificate_request);
        if (BURROW_FAILED(err))
            return err;
    }

    burrow__tls_finished_hash_discard_handshake_buffer(&hs->finished_hash);

    return BURROW_NO_ERROR;
}

static Error tsv_do_full_handshake(TlsServerHandshakeState *hs) {
    Error err = tsv_send_hello_and_certificate(hs);
    if (BURROW_FAILED(err))
        return err;

    TlsKeyAgreement key_agreement;
    burrow__tls_key_agreement_init(&key_agreement, hs->suite, hs->c->vers);
    err = tsv_full_handshake_key_exchange(hs, &key_agreement);
    burrow__tls_key_agreement_free(&key_agreement);
    return err;
}

static Error tsv_establish_keys(TlsServerHandshakeState *hs) {
    TlsConn *c = hs->c;
    const TlsSuite *suite = hs->suite;

    TlsKeys k;
    if (!burrow__tls_keys_from_master_secret(
            hs->a, c->vers, suite, hs->master_secret, hs->client_hello->random,
            hs->hello->random, suite->mac_len, suite->key_len, suite->iv_len, &k))
        return burrow_err_out_of_memory;
    /* Not in Go: each half conn makes its cipher and MAC in memory of its own,
     * which can run out. */
    Alloc *in = burrow__tls_half_conn_next_alloc(&c->in);
    Alloc *out = burrow__tls_half_conn_next_alloc(&c->out);
    if (in == NULL || out == NULL)
        return burrow_err_out_of_memory;
    TlsCipher client_cipher, server_cipher;
    Hash client_hash = {NULL, NULL}, server_hash = {NULL, NULL};
    if (suite->aead == NULL) {
        client_cipher =
            suite->cipher(in, k.client_key, k.client_iv, true /* for reading */);
        client_hash = suite->mac(in, k.client_mac);
        server_cipher =
            suite->cipher(out, k.server_key, k.server_iv, false /* not for reading */);
        server_hash = suite->mac(out, k.server_mac);
        if (client_hash.vt == NULL || server_hash.vt == NULL)
            return burrow_err_out_of_memory;
    } else {
        client_cipher = suite->aead(in, k.client_key, k.client_iv);
        server_cipher = suite->aead(out, k.server_key, k.server_iv);
    }
    if (burrow__tls_cipher_is_nil(client_cipher) ||
        burrow__tls_cipher_is_nil(server_cipher))
        return burrow_err_out_of_memory;

    burrow__tls_half_conn_prepare_cipher_spec(&c->in, c->vers, client_cipher,
                                              client_hash);
    burrow__tls_half_conn_prepare_cipher_spec(&c->out, c->vers, server_cipher,
                                              server_hash);

    return BURROW_NO_ERROR;
}

static Error tsv_read_finished(TlsServerHandshakeState *hs, Byte *out) {
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
    const TlsFinishedMsg *client_finished = &msg->finished;

    Slice verify = burrow__tls_finished_hash_client_sum(&hs->finished_hash, hs->a,
                                                        hs->master_secret);
    if (verify.p == NULL)
        return burrow_err_out_of_memory;
    if (verify.len != client_finished->verify_data.len ||
        subtle_constant_time_compare(verify, client_finished->verify_data) != 1) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tsv_err_finished;
    }

    err = burrow__tls_transcript_msg(
        msg, burrow__tls_finished_hash_writer(&hs->finished_hash), hs->a);
    if (BURROW_FAILED(err))
        return err;

    if (out != NULL)
        memcpy(out, verify.p, (size_t)(verify.len < 12 ? verify.len : 12));
    return BURROW_NO_ERROR;
}

/* Not in Go. The ticket sendSessionTicket sends for state, from hs->a. */
static Slice tsv_wrap_session(TlsServerHandshakeState *hs, TlsSessionState *state,
                              Error *err) {
    TlsConn *c = hs->c;
    if (c->config->wrap_session.f != NULL) {
        TlsConnectionState cs = burrow__tls_conn_connection_state_locked(c);
        return BURROW_CALLF(c->config->wrap_session, hs->a, &cs, state, err);
    }
    Error e = BURROW_NO_ERROR;
    Slice state_bytes = tls_session_state_bytes(state, hs->a, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    return burrow__tls_config_encrypt_ticket(c->config, hs->a, state_bytes,
                                             c->ticket_keys, err);
}

static Error tsv_send_session_ticket(TlsServerHandshakeState *hs) {
    if (!hs->hello->ticket_supported)
        return BURROW_NO_ERROR;

    TlsConn *c = hs->c;
    TlsHandshakeMsg m;
    burrow__tls_handshake_msg_init(&m, TLS_MSG_NEW_SESSION_TICKET);

    Error err = BURROW_NO_ERROR;
    TlsSessionState *state = burrow__tls_conn_session_state(c, hs->a, &err);
    if (state == NULL)
        return err;
    state->secret = hs->master_secret;
    if (hs->session_state != NULL) {
        /* If this is re-wrapping an old key, then keep the original time it
         * was created. */
        state->created_at = hs->session_state->created_at;
    }
    m.new_session_ticket.ticket = tsv_wrap_session(hs, state, &err);
    tls_session_state_free(state);
    if (BURROW_FAILED(err))
        return err;

    return tsv_write(hs, &m);
}

static Error tsv_send_finished(TlsServerHandshakeState *hs, Byte *out) {
    TlsConn *c = hs->c;

    Error err = burrow__tls_conn_write_change_cipher_record(c);
    if (BURROW_FAILED(err))
        return err;

    TlsHandshakeMsg finished;
    burrow__tls_handshake_msg_init(&finished, TLS_MSG_FINISHED);
    finished.finished.verify_data = burrow__tls_finished_hash_server_sum(
        &hs->finished_hash, hs->a, hs->master_secret);
    if (finished.finished.verify_data.p == NULL)
        return burrow_err_out_of_memory;
    err = tsv_write(hs, &finished);
    if (BURROW_FAILED(err))
        return err;

    Slice v = finished.finished.verify_data;
    if (out != NULL)
        memcpy(out, v.p, (size_t)(v.len < 12 ? v.len : 12));

    return BURROW_NO_ERROR;
}

/* The CertificateVerificationError processCertsFromClient returns. */
static Error tsv_verification_error(Slice certs, Error err) {
    TlsCertificateVerificationError e = {certs, err};
    return tls_certificate_verification_error_as_error(&e, error_allocator());
}

/* Not in Go. The x509.Verify processCertsFromClient does, with the chains it
 * makes going to c. */
static Error tsv_verify_chains(TlsConn *c, Slice certs) {
    X509Certificate *const *list = certs.p;
    X509CertPool *intermediates = x509_new_cert_pool(heap_allocator());
    if (intermediates == NULL)
        return burrow_err_out_of_memory;
    for (Int i = 1; i < certs.len; i++)
        x509_cert_pool_add_cert(intermediates, list[i]);
    X509ExtKeyUsage client_auth[1] = {X509_EXT_KEY_USAGE_CLIENT_AUTH};
    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.roots = c->config->client_cas;
    opts.current_time = burrow__tls_config_time(c->config);
    opts.intermediates = intermediates;
    opts.key_usages = slice_from(client_auth, 1, 1, TYPE_INT);

    Error err = BURROW_NO_ERROR;
    Slice chains = x509_certificate_verify(list[0], tsv_keep(c), opts, &err);
    x509_cert_pool_free(intermediates);
    if (BURROW_FAILED(err)) {
        const X509CertificateInvalidError *invalid =
            errors_as(err, TYPE_X509_CERTIFICATE_INVALID_ERROR);
        if (errors_as(err, TYPE_X509_UNKNOWN_AUTHORITY_ERROR) != NULL)
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNKNOWN_CA);
        else if (invalid != NULL && invalid->reason == X509_EXPIRED)
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_CERTIFICATE_EXPIRED);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
        return tsv_verification_error(certs, err);
    }

    c->verified_chains = burrow__tls_fips_allowed_chains(chains, &err);
    if (BURROW_FAILED(err)) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
        return tsv_verification_error(certs, err);
    }
    return BURROW_NO_ERROR;
}

/* processCertsFromClient takes a chain of client certificates either from a
 * certificateMsg message or a certificateMsgTLS13 message and verifies them.
 *
 * Not in Go: each certificate is parsed into c's own memory along with a copy
 * of its bytes, and so are the OCSP response and SCTs. */
Error burrow__tls_conn_process_certs_from_client(TlsConn *c,
                                                 const TlsCertificate *certificate) {
    Alloc *keep = tsv_keep(c);
    Slice certificates = certificate->certificate;
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
            return fmt_errorf_v("tls: failed to parse client certificate: %s",
                                error_text(err));
        }
        if (cert->public_key_algorithm == X509_RSA) {
            const RsaPublicKey *pub = cert->public_key.data;
            Int max;
            if (!burrow__tls_check_key_size(big_int_bit_len(pub->n), &max)) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
                return fmt_errorf_v("tls: client sent certificate containing RSA key "
                                    "larger than %d bits",
                                    max);
            }
        }
        ((X509Certificate **)certs.p)[i] = cert;
    }

    if (certs.len == 0 && burrow__tls_requires_client_cert(c->config->client_auth)) {
        if (c->vers == TLS_VERSION_TLS13)
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_CERTIFICATE_REQUIRED);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_HANDSHAKE_FAILURE);
        return tsv_err_no_client_cert;
    }

    if (c->config->client_auth >= TLS_VERIFY_CLIENT_CERT_IF_GIVEN && certs.len > 0) {
        Error err = tsv_verify_chains(c, certs);
        if (BURROW_FAILED(err))
            return err;
    }

    c->peer_certificates = certs;
    c->ocsp_response = bytes_clone(keep, certificate->ocsp_staple);
    if (c->ocsp_response.p == NULL && certificate->ocsp_staple.len > 0)
        return burrow_err_out_of_memory;
    if (!burrow__tls_clone_bytes_list(keep, certificate->signed_certificate_timestamps,
                                      &c->scts))
        return burrow_err_out_of_memory;

    if (certificates.len > 0) {
        const X509Certificate *leaf = ((X509Certificate *const *)certs.p)[0];
        const Type *t = leaf->public_key.t;
        if (t == TYPE_MLDSA_PUBLIC_KEY) {
            if (c->vers < TLS_VERSION_TLS13) {
                (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
                return tsv_err_mldsa_tls12;
            }
        } else if (t != TYPE_ECDSA_PUBLIC_KEY && t != TYPE_RSA_PUBLIC_KEY &&
                   t != TYPE_ED25519_PUBLIC_KEY) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNSUPPORTED_CERTIFICATE);
            return fmt_errorf_v(
                "tls: client certificate contains an unsupported public "
                "key of type %T",
                leaf->public_key);
        }
    }

    if (c->config->verify_peer_certificate.f != NULL) {
        Error err = BURROW_CALLF(c->config->verify_peer_certificate, certificates,
                                 c->verified_chains);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_BAD_CERTIFICATE);
            return err;
        }
    }

    return BURROW_NO_ERROR;
}

TlsClientHelloInfo *
burrow__tls_client_hello_info(Alloc *a, Context ctx, TlsConn *c,
                              const TlsClientHelloMsg *client_hello) {
    TlsClientHelloInfo *chi = BURROW_NEW(a, TlsClientHelloInfo);
    if (chi == NULL)
        return NULL;
    Slice supported_versions = client_hello->supported_versions;
    if (client_hello->supported_versions.len == 0) {
        uint16_t from_max[4];
        Int n = burrow__tls_supported_versions_from_max(client_hello->vers, from_max);
        supported_versions = slice_make(a, TYPE_UINT16, n, n);
        if (supported_versions.p == NULL && n > 0)
            return NULL;
        if (n > 0)
            memcpy(supported_versions.p, from_max, (size_t)n * sizeof from_max[0]);
    }

    /* QUIC's clientHelloInfoConn comes with quic.go. */
    chi->cipher_suites = client_hello->cipher_suites;
    chi->server_name = client_hello->server_name;
    chi->supported_curves = client_hello->supported_curves;
    chi->supported_points = client_hello->supported_points;
    chi->signature_schemes = client_hello->supported_signature_algorithms;
    chi->supported_protos = client_hello->alpn_protocols;
    chi->supported_versions = supported_versions;
    chi->extensions = client_hello->extensions;
    chi->conn = c->conn;
    chi->hello_retry_request = c->did_hrr;
    chi->config = c->config;
    chi->is_quic = false;
    chi->ctx = ctx;
    return chi;
}
