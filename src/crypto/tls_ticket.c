/* crypto/tls: ticket.go. SessionState and its encoding, the tickets a server
 * seals sessions in, and the ClientSessionState a client keeps.
 *
 * A session keeps what it points at in an arena of its own, since a client
 * session outlives the connection it came from. Go shares parsed certificates
 * through globalCertCache. Here a session parses each distinct certificate
 * once and points at that one from peer_certificates and verified_chains
 * both.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/subtle.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "cryptobyte.h"
#include "tls_builder.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* aes.BlockSize and sha256.Size. */
#define TKT_AES_BLOCK_SIZE 16
#define TKT_SHA256_SIZE 32

BURROW_SENTINEL_ERROR(tkt_err_invalid, "tls: invalid session encoding");
BURROW_SENTINEL_ERROR(tkt_err_unknown, "tls: unknown session encoding");
BURROW_SENTINEL_ERROR(tkt_err_no_server_certificates,
                      "tls: no server certificates in client session");
BURROW_SENTINEL_ERROR(tkt_err_empty_chain, "tls: internal error: empty verified chain");
BURROW_SENTINEL_ERROR(tkt_err_keys_unavailable,
                      "tls: internal error: session ticket keys unavailable");

/* ------------------------------------------------------------ SessionState */

/* An empty session with its arena, from a. NULL when a is out of memory. */
static TlsSessionState *tkt_new_session(Alloc *a) {
    TlsSessionState *ss = BURROW_NEW(a, TlsSessionState);
    if (ss == NULL)
        return NULL;
    ss->a = a;
    arena_init(&ss->arena, a, 0);
    ss->extra = slice_nil(TYPE_BYTES);
    ss->secret = slice_nil(TYPE_BYTE);
    ss->peer_certificates = slice_nil(TYPE_X509_CERTIFICATE_PTR);
    ss->ocsp_response = slice_nil(TYPE_BYTE);
    ss->scts = slice_nil(TYPE_BYTES);
    ss->verified_chains = slice_nil(TYPE_X509_CERTIFICATE_CHAIN);
    ss->ticket = slice_nil(TYPE_BYTE);
    return ss;
}

void tls_session_state_free(TlsSessionState *s) {
    if (s == NULL)
        return;
    arena_free(&s->arena);
    mem_free(s->a, s, sizeof(*s), _Alignof(TlsSessionState));
}

Alloc *tls_session_state_allocator(TlsSessionState *s) {
    return arena_allocator(&s->arena);
}

static bool tkt_push(Alloc *a, Slice *s, const void *v) {
    Slice r = slice_append(a, *s, v, 1);
    if (r.p == NULL)
        return false;
    *s = r;
    return true;
}

/* b, copied into the session. A nil b stays nil. */
static bool tkt_clone(TlsSessionState *ss, Slice b, Slice *out) {
    *out = bytes_clone(arena_allocator(&ss->arena), b);
    return b.len == 0 || out->p != NULL;
}

/* globalCertCache.newCert: the certificate in der, parsed into the session.
 * One the session already has is shared rather than parsed again. der has to
 * be in the session already. */
static X509Certificate *tkt_new_cert(TlsSessionState *ss, Slice der, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    X509Certificate *const *certs = ss->peer_certificates.p;
    for (Int i = 0; i < ss->peer_certificates.len; i++)
        if (bytes_equal(certs[i]->raw, der))
            return certs[i];
    for (Int i = 0; i < ss->verified_chains.len; i++) {
        Slice chain = ((const Slice *)ss->verified_chains.p)[i];
        X509Certificate *const *cc = chain.p;
        for (Int k = 0; k < chain.len; k++)
            if (bytes_equal(cc[k]->raw, der))
                return cc[k];
    }
    return x509_parse_certificate(arena_allocator(&ss->arena), der, err);
}

Slice tls_session_state_bytes(const TlsSessionState *s, Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u16(&b, s->version);
    if (s->is_client)
        tlsb_u8(&b, 2); /* client */
    else
        tlsb_u8(&b, 1); /* server */
    tlsb_u16(&b, s->cipher_suite);
    tlsb_u64(&b, s->created_at);
    tlsb_begin(&b, 1);
    tlsb_bytes(&b, s->secret);
    tlsb_end(&b);
    tlsb_begin(&b, 3);
    for (Int i = 0; i < s->extra.len; i++) {
        tlsb_begin(&b, 3);
        tlsb_bytes(&b, ((const Slice *)s->extra.p)[i]);
        tlsb_end(&b);
    }
    tlsb_end(&b);
    if (s->ext_master_secret)
        tlsb_u8(&b, 1);
    else
        tlsb_u8(&b, 0);
    if (s->early_data)
        tlsb_u8(&b, 1);
    else
        tlsb_u8(&b, 0);

    /* certificatesToBytesSlice, with the raw bytes pointed at rather than
     * copied, in an arena of its own. */
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    TlsCertificate cert;
    memset(&cert, 0, sizeof cert);
    cert.certificate =
        slice_make(arena_allocator(&scratch), TYPE_BYTES, 0, s->peer_certificates.len);
    X509Certificate *const *peers = s->peer_certificates.p;
    for (Int i = 0; i < s->peer_certificates.len; i++)
        if (!tkt_push(arena_allocator(&scratch), &cert.certificate, &peers[i]->raw))
            tlsb_fail(&b, burrow_err_out_of_memory);
    cert.ocsp_staple = s->ocsp_response;
    cert.signed_certificate_timestamps = s->scts;
    burrow__tls_marshal_certificate(&b, &cert);
    arena_free(&scratch);

    tlsb_begin(&b, 3);
    for (Int i = 0; i < s->verified_chains.len; i++) {
        Slice chain = ((const Slice *)s->verified_chains.p)[i];
        tlsb_begin(&b, 3);
        /* We elide the first certificate because it's always the leaf. */
        if (chain.len == 0) {
            tlsb_fail(&b, tkt_err_empty_chain);
        } else {
            X509Certificate *const *cc = chain.p;
            for (Int k = 1; k < chain.len; k++) {
                tlsb_begin(&b, 3);
                tlsb_bytes(&b, cc[k]->raw);
                tlsb_end(&b);
            }
        }
        tlsb_end(&b);
    }
    tlsb_end(&b);
    if (s->early_data) {
        tlsb_begin(&b, 1);
        tlsb_str(&b, s->alpn_protocol);
        tlsb_end(&b);
    }
    if (s->version >= TLS_VERSION_TLS13) {
        if (s->is_client) {
            tlsb_u64(&b, s->use_by);
            tlsb_u32(&b, s->age_add);
        }
    } else {
        tlsb_u16(&b, s->curve_id);
    }
    return tlsb_finish(&b, err);
}

/* readUint24LengthPrefixed, which is the same as reading a
 * cryptobyte.String here. */
static bool tkt_lp24(CryptobyteString *s, Slice *out) {
    return cryptobyte_string_read_uint24_length_prefixed(s, out);
}

/* What ParseSessionState does once data is in ss. */
static Error tkt_parse(TlsSessionState *ss, Slice data) {
    Alloc *a = arena_allocator(&ss->arena);
    CryptobyteString s = data;
    uint8_t typ, ext_master_secret, early_data;
    TlsCertificate cert = burrow__tls_certificate_zero();
    CryptobyteString extra;
    if (!cryptobyte_string_read_uint16(&s, &ss->version) ||
        !cryptobyte_string_read_uint8(&s, &typ) ||
        !cryptobyte_string_read_uint16(&s, &ss->cipher_suite) ||
        !cryptobyte_string_read_uint64(&s, &ss->created_at) ||
        !cryptobyte_string_read_uint8_length_prefixed(&s, &ss->secret) ||
        !tkt_lp24(&s, &extra) ||
        !cryptobyte_string_read_uint8(&s, &ext_master_secret) ||
        !cryptobyte_string_read_uint8(&s, &early_data) || ss->secret.len == 0 ||
        !burrow__tls_unmarshal_certificate(&s, a, &cert))
        return tkt_err_invalid;
    while (!cryptobyte_string_empty(extra)) {
        Slice e;
        if (!tkt_lp24(&extra, &e))
            return tkt_err_invalid;
        if (!tkt_push(a, &ss->extra, &e))
            return burrow_err_out_of_memory;
    }
    switch (typ) {
    case 1:
        ss->is_client = false;
        break;
    case 2:
        ss->is_client = true;
        break;
    default:
        return tkt_err_unknown;
    }
    switch (ext_master_secret) {
    case 0:
        ss->ext_master_secret = false;
        break;
    case 1:
        ss->ext_master_secret = true;
        break;
    default:
        return tkt_err_invalid;
    }
    switch (early_data) {
    case 0:
        ss->early_data = false;
        break;
    case 1:
        ss->early_data = true;
        break;
    default:
        return tkt_err_invalid;
    }
    const Slice *certs = cert.certificate.p;
    for (Int i = 0; i < cert.certificate.len; i++) {
        Error err = BURROW_NO_ERROR;
        X509Certificate *c = tkt_new_cert(ss, certs[i], &err);
        if (c == NULL)
            return err;
        if (!tkt_push(a, &ss->peer_certificates, &c))
            return burrow_err_out_of_memory;
    }
    if (ss->is_client && ss->peer_certificates.len == 0)
        return tkt_err_no_server_certificates;
    ss->ocsp_response = cert.ocsp_staple;
    ss->scts = cert.signed_certificate_timestamps;
    CryptobyteString chain_list;
    if (!tkt_lp24(&s, &chain_list))
        return tkt_err_invalid;
    while (!cryptobyte_string_empty(chain_list)) {
        CryptobyteString cert_list;
        if (!tkt_lp24(&chain_list, &cert_list))
            return tkt_err_invalid;
        Slice chain = slice_nil(TYPE_X509_CERTIFICATE_PTR);
        if (ss->peer_certificates.len == 0)
            return tkt_err_invalid;
        if (!tkt_push(a, &chain, ss->peer_certificates.p))
            return burrow_err_out_of_memory;
        while (!cryptobyte_string_empty(cert_list)) {
            Slice der;
            if (!tkt_lp24(&cert_list, &der))
                return tkt_err_invalid;
            Error err = BURROW_NO_ERROR;
            X509Certificate *c = tkt_new_cert(ss, der, &err);
            if (c == NULL)
                return err;
            if (!tkt_push(a, &chain, &c))
                return burrow_err_out_of_memory;
        }
        if (!tkt_push(a, &ss->verified_chains, &chain))
            return burrow_err_out_of_memory;
    }
    if (ss->early_data) {
        Slice alpn;
        if (!cryptobyte_string_read_uint8_length_prefixed(&s, &alpn))
            return tkt_err_invalid;
        ss->alpn_protocol = (Str){alpn.p, alpn.len};
    }
    if (ss->version >= TLS_VERSION_TLS13) {
        if (ss->is_client) {
            if (!cryptobyte_string_read_uint64(&s, &ss->use_by) ||
                !cryptobyte_string_read_uint32(&s, &ss->age_add))
                return tkt_err_invalid;
        }
    } else {
        if (!cryptobyte_string_read_uint16(&s, &ss->curve_id))
            return tkt_err_invalid;
    }
    return BURROW_NO_ERROR;
}

TlsSessionState *tls_parse_session_state(Alloc *a, Slice data, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    TlsSessionState *ss = tkt_new_session(a);
    if (ss == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Slice owned;
    if (!tkt_clone(ss, data, &owned)) {
        tls_session_state_free(ss);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Error e = tkt_parse(ss, owned);
    if (BURROW_FAILED(e)) {
        /* What x509 says about a certificate is in the session's arena. */
        e = error_retain(error_allocator(), e);
        tls_session_state_free(ss);
        BURROW_OUT(err, e);
        return NULL;
    }
    return ss;
}

TlsSessionState *burrow__tls_conn_session_state(TlsConn *c, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    TlsSessionState *ss = tkt_new_session(a);
    if (ss == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Alloc *sa = arena_allocator(&ss->arena);
    ss->version = c->vers;
    ss->cipher_suite = c->cipher_suite;
    ss->created_at = (uint64_t)time_unix(burrow__tls_config_time(c->config));
    ss->is_client = c->is_client;
    ss->ext_master_secret = c->ext_master_secret;
    ss->curve_id = c->curve_id;

    Error e = BURROW_NO_ERROR;
    Slice alpn;
    Slice protocol =
        slice_from((Byte *)(uintptr_t)c->client_protocol.p, c->client_protocol.len,
                   c->client_protocol.len, TYPE_BYTE);
    if (!tkt_clone(ss, protocol, &alpn) ||
        !tkt_clone(ss, c->ocsp_response, &ss->ocsp_response)) {
        e = burrow_err_out_of_memory;
        goto fail;
    }
    ss->alpn_protocol = (Str){alpn.p, alpn.len};
    if (!slice_is_nil(c->scts)) {
        ss->scts = slice_make(sa, TYPE_BYTES, 0, c->scts.len);
        if (ss->scts.p == NULL && c->scts.len > 0) {
            e = burrow_err_out_of_memory;
            goto fail;
        }
        for (Int i = 0; i < c->scts.len; i++) {
            Slice sct;
            if (!tkt_clone(ss, ((const Slice *)c->scts.p)[i], &sct) ||
                !tkt_push(sa, &ss->scts, &sct)) {
                e = burrow_err_out_of_memory;
                goto fail;
            }
        }
    }

    /* Go points at the connection's certificates. They go with the
     * connection here, so the session parses its own. */
    X509Certificate *const *peers = c->peer_certificates.p;
    for (Int i = 0; i < c->peer_certificates.len; i++) {
        Slice der;
        X509Certificate *cert = NULL;
        if (tkt_clone(ss, peers[i]->raw, &der))
            cert = tkt_new_cert(ss, der, &e);
        else
            e = burrow_err_out_of_memory;
        if (cert == NULL || !tkt_push(sa, &ss->peer_certificates, &cert)) {
            if (!BURROW_FAILED(e))
                e = burrow_err_out_of_memory;
            goto fail;
        }
    }
    for (Int i = 0; i < c->verified_chains.len; i++) {
        Slice from = ((const Slice *)c->verified_chains.p)[i];
        Slice chain = slice_make(sa, TYPE_X509_CERTIFICATE_PTR, 0, from.len);
        if (chain.p == NULL && from.len > 0) {
            e = burrow_err_out_of_memory;
            goto fail;
        }
        X509Certificate *const *cc = from.p;
        for (Int k = 0; k < from.len; k++) {
            Slice der;
            X509Certificate *cert = NULL;
            if (tkt_clone(ss, cc[k]->raw, &der))
                cert = tkt_new_cert(ss, der, &e);
            else
                e = burrow_err_out_of_memory;
            if (cert == NULL || !tkt_push(sa, &chain, &cert)) {
                if (!BURROW_FAILED(e))
                    e = burrow_err_out_of_memory;
                goto fail;
            }
        }
        if (!tkt_push(sa, &ss->verified_chains, &chain)) {
            e = burrow_err_out_of_memory;
            goto fail;
        }
    }
    return ss;

fail:
    e = error_retain(error_allocator(), e);
    tls_session_state_free(ss);
    BURROW_OUT(err, e);
    return NULL;
}

/* ---------------------------------------------------------------- tickets */

Slice tls_config_encrypt_ticket(TlsConfig *c, Alloc *a, const TlsConnectionState *cs,
                                const TlsSessionState *ss, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)cs;
    Error e = BURROW_NO_ERROR;
    TlsTicketKeys *ticket_keys = burrow__tls_config_ticket_keys(c, NULL, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Slice state_bytes = tls_session_state_bytes(ss, heap_allocator(), &e);
    if (BURROW_FAILED(e)) {
        burrow__tls_ticket_keys_release(ticket_keys);
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Slice out = burrow__tls_config_encrypt_ticket(c, a, state_bytes, ticket_keys, err);
    mem_free(heap_allocator(), state_bytes.p, (size_t)state_bytes.cap, 1);
    burrow__tls_ticket_keys_release(ticket_keys);
    return out;
}

Slice burrow__tls_config_encrypt_ticket(TlsConfig *c, Alloc *a, Slice state,
                                        const TlsTicketKeys *ticket_keys, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (ticket_keys == NULL || ticket_keys->len == 0) {
        BURROW_OUT(err, tkt_err_keys_unavailable);
        return slice_nil(TYPE_BYTE);
    }

    Int n = TKT_AES_BLOCK_SIZE + state.len + TKT_SHA256_SIZE;
    Slice encrypted = slice_make(a, TYPE_BYTE, n, n);
    if (encrypted.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Slice iv = slice_sub(encrypted, 0, TKT_AES_BLOCK_SIZE);
    Slice ciphertext = slice_sub(encrypted, TKT_AES_BLOCK_SIZE, n - TKT_SHA256_SIZE);
    Slice authenticated = slice_sub(encrypted, 0, n - TKT_SHA256_SIZE);
    Slice mac_bytes = slice_sub(encrypted, n - TKT_SHA256_SIZE, n);

    Error e = BURROW_NO_ERROR;
    io_read_full(burrow__tls_config_rand(c), iv, &e);
    if (BURROW_FAILED(e)) {
        mem_free(a, encrypted.p, (size_t)encrypted.cap, 1);
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    const TlsTicketKey *key = &ticket_keys->keys[0];
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    CipherBlock block = aes_new_cipher(
        sa, slice_from((Byte *)(uintptr_t)key->aes_key, 16, 16, TYPE_BYTE), &e);
    if (BURROW_FAILED(e)) {
        Error out =
            fmt_errorf_v("tls: failed to create cipher while encrypting ticket: %v", e);
        arena_free(&scratch);
        mem_free(a, encrypted.p, (size_t)encrypted.cap, 1);
        BURROW_OUT(err, out);
        return slice_nil(TYPE_BYTE);
    }
    CipherStream ctr = cipher_new_ctr(sa, block, iv);
    Hash mac =
        hmac_new(sa, sha256_new,
                 slice_from((Byte *)(uintptr_t)key->hmac_key, 16, 16, TYPE_BYTE));
    if (ctr.vt == NULL || mac.vt == NULL) {
        arena_free(&scratch);
        mem_free(a, encrypted.p, (size_t)encrypted.cap, 1);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    cipher_stream_xor_key_stream(ctr, ciphertext, state);

    hash_write(mac, authenticated, &e);
    hash_sum(sa, mac, slice_sub(mac_bytes, 0, 0));
    arena_free(&scratch);

    return encrypted;
}

TlsSessionState *tls_config_decrypt_ticket(TlsConfig *c, Slice identity,
                                           const TlsConnectionState *cs, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)cs;
    Error e = BURROW_NO_ERROR;
    TlsTicketKeys *ticket_keys = burrow__tls_config_ticket_keys(c, NULL, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    Alloc *a = c->a != NULL ? c->a : heap_allocator();
    Slice state_bytes = burrow__tls_config_decrypt_ticket(c, a, identity, ticket_keys);
    burrow__tls_ticket_keys_release(ticket_keys);
    if (state_bytes.p == NULL)
        return NULL;
    TlsSessionState *s = tls_parse_session_state(a, state_bytes, &e);
    mem_free(a, state_bytes.p, (size_t)state_bytes.cap, 1);
    if (BURROW_FAILED(e))
        return NULL; /* drop unparsable tickets on the floor */
    return s;
}

Slice burrow__tls_config_decrypt_ticket(TlsConfig *c, Alloc *a, Slice encrypted,
                                        const TlsTicketKeys *ticket_keys) {
    (void)c;
    if (encrypted.len < TKT_AES_BLOCK_SIZE + TKT_SHA256_SIZE || ticket_keys == NULL)
        return slice_nil(TYPE_BYTE);

    Int n = encrypted.len;
    Slice iv = slice_sub(encrypted, 0, TKT_AES_BLOCK_SIZE);
    Slice ciphertext = slice_sub(encrypted, TKT_AES_BLOCK_SIZE, n - TKT_SHA256_SIZE);
    Slice authenticated = slice_sub(encrypted, 0, n - TKT_SHA256_SIZE);
    Slice mac_bytes = slice_sub(encrypted, n - TKT_SHA256_SIZE, n);

    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    Slice out = slice_nil(TYPE_BYTE);
    for (Int i = 0; i < ticket_keys->len; i++) {
        const TlsTicketKey *key = &ticket_keys->keys[i];
        ArenaMark m = arena_mark(&scratch);
        Hash mac =
            hmac_new(sa, sha256_new,
                     slice_from((Byte *)(uintptr_t)key->hmac_key, 16, 16, TYPE_BYTE));
        if (mac.vt == NULL)
            break;
        Error e = BURROW_NO_ERROR;
        hash_write(mac, authenticated, &e);
        Slice expected = hash_sum(sa, mac, slice_nil(TYPE_BYTE));

        if (subtle_constant_time_compare(mac_bytes, expected) != 1) {
            arena_release(&scratch, m);
            continue;
        }

        CipherBlock block = aes_new_cipher(
            sa, slice_from((Byte *)(uintptr_t)key->aes_key, 16, 16, TYPE_BYTE), &e);
        if (BURROW_FAILED(e))
            break;
        CipherStream ctr = cipher_new_ctr(sa, block, iv);
        if (ctr.vt == NULL)
            break;
        Slice plaintext = slice_make(a, TYPE_BYTE, ciphertext.len, ciphertext.len);
        if (plaintext.p == NULL)
            break;
        cipher_stream_xor_key_stream(ctr, plaintext, ciphertext);
        out = plaintext;
        break;
    }
    arena_free(&scratch);
    return out;
}

/* ------------------------------------------------------ ClientSessionState */

Slice tls_client_session_state_resumption_state(TlsClientSessionState *cs,
                                                TlsSessionState **state, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)err;
    if (cs == NULL || cs->session == NULL) {
        *state = NULL;
        return slice_nil(TYPE_BYTE);
    }
    *state = cs->session;
    return cs->session->ticket;
}

TlsClientSessionState *tls_new_resumption_state(Slice ticket, TlsSessionState *state,
                                                Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *sa = arena_allocator(&state->arena);
    TlsClientSessionState *cs = BURROW_NEW(sa, TlsClientSessionState);
    if (cs == NULL || !tkt_clone(state, ticket, &state->ticket)) {
        tls_session_state_free(state);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    cs->session = state;
    cs->refs = 1;
    return cs;
}

TlsClientSessionState *tls_client_session_state_retain(TlsClientSessionState *cs) {
    if (cs != NULL)
        sync_atomic_add_int32(&cs->refs, 1);
    return cs;
}

void tls_client_session_state_release(TlsClientSessionState *cs) {
    if (cs == NULL || sync_atomic_add_int32(&cs->refs, -1) != 0)
        return;
    /* cs is in the session's arena, so it goes with it. */
    tls_session_state_free(cs->session);
}
