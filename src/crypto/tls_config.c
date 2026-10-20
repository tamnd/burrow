/* crypto/tls: the Config half of common.go, with its session ticket keys, the
 * LRU client session cache, CertificateVerificationError and the chain checks
 * the client handshake makes, and supportsECDHE from handshake_server.go.
 *
 * Go's ticket key lists are slices the garbage collector frees once the last
 * config or connection lets go of them. Here a list carries a count of
 * references instead, and burrow__tls_config_ticket_keys hands out one of them
 * with each list it returns.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/sha512.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static Error tcfg_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static bool tcfg_contains_u16(Slice s, uint16_t v) {
    const uint16_t *p = (const uint16_t *)s.p;
    for (Int i = 0; i < s.len; i++) {
        if (p[i] == v)
            return true;
    }
    return false;
}

/* --------------------------------------------------------------- ticket keys */

static Alloc *tcfg_alloc(const TlsConfig *c) {
    return c->a != NULL ? c->a : heap_allocator();
}

/* A list with room for n keys and none in it yet, holding one reference. */
static TlsTicketKeys *tcfg_keys_new(const TlsConfig *c, Int n) {
    Alloc *a = tcfg_alloc(c);
    TlsTicketKeys *k =
        mem_alloc(a, sizeof(TlsTicketKeys) + (size_t)n * sizeof(TlsTicketKey),
                  _Alignof(TlsTicketKeys));
    if (k == NULL)
        return NULL;
    k->refs = 1;
    k->cap = n;
    k->a = a;
    return k;
}

static TlsTicketKeys *tcfg_keys_retain(TlsTicketKeys *k) {
    if (k != NULL)
        sync_atomic_add_int32(&k->refs, 1);
    return k;
}

static Int tcfg_keys_len(const TlsTicketKeys *k) {
    return k != NULL ? k->len : 0;
}

void burrow__tls_ticket_keys_release(TlsTicketKeys *k) {
    if (k == NULL || sync_atomic_add_int32(&k->refs, -1) != 0)
        return;
    mem_free(k->a, k, sizeof(TlsTicketKeys) + (size_t)k->cap * sizeof(TlsTicketKey),
             _Alignof(TlsTicketKeys));
}

TlsTicketKey burrow__tls_config_ticket_key_from_bytes(const TlsConfig *c,
                                                      const Byte b[32]) {
    TlsTicketKey key;
    Sha512Sum512Ret hashed =
        sha512_sum512(slice_from((void *)(uintptr_t)b, 32, 32, TYPE_BYTE));
    /* The first 16 bytes of the hash used to be exposed on the wire as a
     * ticket prefix. They MUST NOT be used as a secret. In the future, it
     * would make sense to use a proper KDF here, like HKDF with a fixed salt. */
    enum { legacy_ticket_key_name_len = 16 };
    memcpy(key.aes_key, hashed.a + legacy_ticket_key_name_len, sizeof(key.aes_key));
    memcpy(key.hmac_key, hashed.a + legacy_ticket_key_name_len + sizeof(key.aes_key),
           sizeof(key.hmac_key));
    key.created = burrow__tls_config_time(c);
    return key;
}

/* deprecatedSessionTicketKey is set as the prefix of SessionTicketKey if it was
 * randomized for backwards compatibility but is not in use. */
#define TCFG_DEPRECATED_SESSION_TICKET_KEY "DEPRECATED"
#define TCFG_DEPRECATED_LEN 10

static bool tcfg_session_ticket_key_zero(const TlsConfig *c) {
    for (int i = 0; i < 32; i++) {
        if (c->session_ticket_key[i] != 0)
            return false;
    }
    return true;
}

static bool tcfg_session_ticket_key_deprecated(const TlsConfig *c) {
    return memcmp(c->session_ticket_key, TCFG_DEPRECATED_SESSION_TICKET_KEY,
                  TCFG_DEPRECATED_LEN) == 0;
}

/* Reads 32 random bytes into b for a session ticket key, and panics as Go
 * does when it cannot. The caller holds c's write lock, which is given back
 * first, since it holds nothing else by then. */
static void tcfg_read_ticket_key(TlsConfig *c, Byte b[32], const char *what) {
    Error err = BURROW_NO_ERROR;
    io_read_full(burrow__tls_config_rand(c), slice_from(b, 32, 32, TYPE_BYTE), &err);
    if (!BURROW_FAILED(err))
        return;
    sync_rw_mutex_unlock(&c->mutex);
    panic_str(fmt_sprintf_v(error_allocator(), what, err));
}

/* initLegacySessionTicketKeyRLocked ensures the legacy SessionTicketKey field is
 * randomized if empty, and that sessionTicketKeys is populated from it
 * otherwise.
 *
 * c is read locked, and is again on return. burrow_err_out_of_memory when the
 * list cannot be made. */
static Error tcfg_init_legacy_session_ticket_key_r_locked(TlsConfig *c) {
    /* Don't write if SessionTicketKey is already defined as our deprecated
     * string, or if it is defined by the user but sessionTicketKeys is already
     * set. */
    if (!tcfg_session_ticket_key_zero(c) && (tcfg_session_ticket_key_deprecated(c) ||
                                             tcfg_keys_len(c->session_ticket_keys) > 0))
        return BURROW_NO_ERROR;

    /* We need to write some data, so get an exclusive lock and re-check any
     * conditions. */
    sync_rw_mutex_r_unlock(&c->mutex);
    sync_rw_mutex_lock(&c->mutex);
    Error err = BURROW_NO_ERROR;
    if (tcfg_session_ticket_key_zero(c)) {
        tcfg_read_ticket_key(c, c->session_ticket_key,
                             "tls: unable to generate random session ticket key: %v");
        /* Write the deprecated prefix at the beginning so we know we created
         * it. This key with the DEPRECATED prefix isn't used as an actual
         * session ticket key, and is only randomized in case the application
         * reuses it for some reason. */
        memcpy(c->session_ticket_key, TCFG_DEPRECATED_SESSION_TICKET_KEY,
               TCFG_DEPRECATED_LEN);
    } else if (!tcfg_session_ticket_key_deprecated(c) &&
               tcfg_keys_len(c->session_ticket_keys) == 0) {
        TlsTicketKeys *keys = tcfg_keys_new(c, 1);
        if (keys == NULL) {
            err = burrow_err_out_of_memory;
        } else {
            keys->keys[keys->len++] =
                burrow__tls_config_ticket_key_from_bytes(c, c->session_ticket_key);
            burrow__tls_ticket_keys_release(c->session_ticket_keys);
            c->session_ticket_keys = keys;
        }
    }
    sync_rw_mutex_unlock(&c->mutex);
    sync_rw_mutex_r_lock(&c->mutex);
    return err;
}

TlsTicketKeys *
burrow__tls_config_ticket_keys(TlsConfig *c, TlsConfig *config_for_client, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    TlsTicketKeys *ret;
    Error e;

    /* If the ConfigForClient callback returned a Config with explicitly set
     * keys, use those, otherwise just use the original Config. */
    if (config_for_client != NULL) {
        sync_rw_mutex_r_lock(&config_for_client->mutex);
        if (config_for_client->session_tickets_disabled) {
            sync_rw_mutex_r_unlock(&config_for_client->mutex);
            return NULL;
        }
        e = tcfg_init_legacy_session_ticket_key_r_locked(config_for_client);
        if (BURROW_FAILED(e)) {
            sync_rw_mutex_r_unlock(&config_for_client->mutex);
            BURROW_OUT(err, e);
            return NULL;
        }
        if (tcfg_keys_len(config_for_client->session_ticket_keys) != 0) {
            ret = tcfg_keys_retain(config_for_client->session_ticket_keys);
            sync_rw_mutex_r_unlock(&config_for_client->mutex);
            return ret;
        }
        sync_rw_mutex_r_unlock(&config_for_client->mutex);
    }

    sync_rw_mutex_r_lock(&c->mutex);
    if (c->session_tickets_disabled) {
        sync_rw_mutex_r_unlock(&c->mutex);
        return NULL;
    }
    e = tcfg_init_legacy_session_ticket_key_r_locked(c);
    if (BURROW_FAILED(e)) {
        sync_rw_mutex_r_unlock(&c->mutex);
        BURROW_OUT(err, e);
        return NULL;
    }
    if (tcfg_keys_len(c->session_ticket_keys) != 0) {
        ret = tcfg_keys_retain(c->session_ticket_keys);
        sync_rw_mutex_r_unlock(&c->mutex);
        return ret;
    }
    /* Fast path for the common case where the key is fresh enough. */
    TlsTicketKeys *auto_keys = c->auto_session_ticket_keys;
    if (tcfg_keys_len(auto_keys) > 0 &&
        time_sub(burrow__tls_config_time(c), auto_keys->keys[0].created) <
            TLS_TICKET_KEY_ROTATION) {
        ret = tcfg_keys_retain(auto_keys);
        sync_rw_mutex_r_unlock(&c->mutex);
        return ret;
    }

    /* autoSessionTicketKeys are managed by auto-rotation. */
    sync_rw_mutex_r_unlock(&c->mutex);
    sync_rw_mutex_lock(&c->mutex);
    /* Re-check the condition in case it changed since obtaining the new lock. */
    auto_keys = c->auto_session_ticket_keys;
    if (tcfg_keys_len(auto_keys) == 0 ||
        time_sub(burrow__tls_config_time(c), auto_keys->keys[0].created) >=
            TLS_TICKET_KEY_ROTATION) {
        Byte new_key[32];
        tcfg_read_ticket_key(c, new_key,
                             "unable to generate random session ticket key: %v");
        TlsTicketKeys *valid = tcfg_keys_new(c, tcfg_keys_len(auto_keys) + 1);
        if (valid == NULL) {
            sync_rw_mutex_unlock(&c->mutex);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        valid->keys[valid->len++] =
            burrow__tls_config_ticket_key_from_bytes(c, new_key);
        for (Int i = 0; i < tcfg_keys_len(auto_keys); i++) {
            /* While rotating the current key, also remove any expired ones. */
            if (time_sub(burrow__tls_config_time(c), auto_keys->keys[i].created) <
                TLS_TICKET_KEY_LIFETIME)
                valid->keys[valid->len++] = auto_keys->keys[i];
        }
        c->auto_session_ticket_keys = valid;
        burrow__tls_ticket_keys_release(auto_keys);
    }
    ret = tcfg_keys_retain(c->auto_session_ticket_keys);
    sync_rw_mutex_unlock(&c->mutex);
    return ret;
}

/* ------------------------------------------------------------- clone and keys */

TlsConfig *tls_config_clone(TlsConfig *c, Alloc *a) {
    if (c == NULL)
        return NULL;
    TlsConfig *n = BURROW_NEW(a, TlsConfig);
    if (n == NULL)
        return NULL;
    sync_rw_mutex_r_lock(&c->mutex);
    n->rand = c->rand;
    n->time = c->time;
    n->certificates = c->certificates;
    n->name_to_certificate = c->name_to_certificate;
    n->get_certificate = c->get_certificate;
    n->get_client_certificate = c->get_client_certificate;
    n->get_config_for_client = c->get_config_for_client;
    n->get_encrypted_client_hello_keys = c->get_encrypted_client_hello_keys;
    n->verify_peer_certificate = c->verify_peer_certificate;
    n->verify_connection = c->verify_connection;
    n->root_cas = c->root_cas;
    n->next_protos = c->next_protos;
    n->server_name = c->server_name;
    n->client_auth = c->client_auth;
    n->client_cas = c->client_cas;
    n->insecure_skip_verify = c->insecure_skip_verify;
    n->cipher_suites = c->cipher_suites;
    n->prefer_server_cipher_suites = c->prefer_server_cipher_suites;
    n->session_tickets_disabled = c->session_tickets_disabled;
    memcpy(n->session_ticket_key, c->session_ticket_key, sizeof(n->session_ticket_key));
    n->client_session_cache = c->client_session_cache;
    n->unwrap_session = c->unwrap_session;
    n->wrap_session = c->wrap_session;
    n->min_version = c->min_version;
    n->max_version = c->max_version;
    n->curve_preferences = c->curve_preferences;
    n->dynamic_record_sizing_disabled = c->dynamic_record_sizing_disabled;
    n->renegotiation = c->renegotiation;
    n->key_log_writer = c->key_log_writer;
    n->encrypted_client_hello_config_list = c->encrypted_client_hello_config_list;
    n->encrypted_client_hello_rejection_verify =
        c->encrypted_client_hello_rejection_verify;
    n->encrypted_client_hello_keys = c->encrypted_client_hello_keys;
    n->a = c->a;
    n->session_ticket_keys = tcfg_keys_retain(c->session_ticket_keys);
    n->auto_session_ticket_keys = tcfg_keys_retain(c->auto_session_ticket_keys);
    sync_rw_mutex_r_unlock(&c->mutex);
    return n;
}

void tls_config_release(TlsConfig *c) {
    sync_rw_mutex_lock(&c->mutex);
    TlsTicketKeys *keys = c->session_ticket_keys;
    TlsTicketKeys *auto_keys = c->auto_session_ticket_keys;
    c->session_ticket_keys = NULL;
    c->auto_session_ticket_keys = NULL;
    sync_rw_mutex_unlock(&c->mutex);
    burrow__tls_ticket_keys_release(keys);
    burrow__tls_ticket_keys_release(auto_keys);
}

Error tls_config_set_session_ticket_keys(TlsConfig *c, const Byte (*keys)[32], Int n) {
    if (n == 0)
        panic_str(BURROW_S("tls: keys must have at least one key"));

    TlsTicketKeys *new_keys = tcfg_keys_new(c, n);
    if (new_keys == NULL)
        return burrow_err_out_of_memory;
    for (Int i = 0; i < n; i++)
        new_keys->keys[new_keys->len++] =
            burrow__tls_config_ticket_key_from_bytes(c, keys[i]);

    sync_rw_mutex_lock(&c->mutex);
    TlsTicketKeys *old = c->session_ticket_keys;
    c->session_ticket_keys = new_keys;
    sync_rw_mutex_unlock(&c->mutex);
    burrow__tls_ticket_keys_release(old);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ settings */

IoReader burrow__tls_config_rand(const TlsConfig *c) {
    if (c->rand.vt == NULL)
        return crypto_rand_reader;
    return c->rand;
}

Time burrow__tls_config_time(const TlsConfig *c) {
    if (c->time.f == NULL)
        return time_now();
    return c->time.f(c->time.env);
}

Slice burrow__tls_config_cipher_suites(const TlsConfig *c, Alloc *a,
                                       bool aes_gcm_preferred) {
    if (slice_is_nil(c->cipher_suites))
        return burrow__tls_default_cipher_suites(a, aes_gcm_preferred);
    Slice cipher_suites = burrow__tls_supported_cipher_suites(a, aes_gcm_preferred);
    uint16_t *p = (uint16_t *)cipher_suites.p;
    Int n = 0;
    for (Int i = 0; i < cipher_suites.len; i++) {
        if (tcfg_contains_u16(c->cipher_suites, p[i]))
            p[n++] = p[i];
    }
    cipher_suites.len = n;
    return cipher_suites;
}

Slice burrow__tls_config_supported_cipher_suites(const TlsConfig *c, Alloc *a) {
    return burrow__tls_config_cipher_suites(c, a, false);
}

static const uint16_t tcfg_supported_versions[4] = {
    TLS_VERSION_TLS13,
    TLS_VERSION_TLS12,
    TLS_VERSION_TLS11,
    TLS_VERSION_TLS10,
};

Int burrow__tls_config_supported_versions(const TlsConfig *c, bool is_client,
                                          bool is_quic, uint16_t out[4]) {
    Int n = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t v = tcfg_supported_versions[i];
        if ((c == NULL || c->min_version == 0) && v < TLS_VERSION_TLS12)
            continue;
        if (is_client && c != NULL &&
            !slice_is_nil(c->encrypted_client_hello_config_list) &&
            v < TLS_VERSION_TLS13)
            continue;
        if (c != NULL && c->min_version != 0 && v < c->min_version)
            continue;
        if (c != NULL && c->max_version != 0 && v > c->max_version)
            continue;
        if (is_quic && v < TLS_VERSION_TLS13)
            continue;
        out[n++] = v;
    }
    return n;
}

uint16_t burrow__tls_config_max_supported_version(const TlsConfig *c, bool is_client,
                                                  bool is_quic) {
    uint16_t supported_versions[4];
    if (burrow__tls_config_supported_versions(c, is_client, is_quic,
                                              supported_versions) == 0)
        return 0;
    return supported_versions[0];
}

Int burrow__tls_supported_versions_from_max(uint16_t max_version, uint16_t out[4]) {
    Int n = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t v = tcfg_supported_versions[i];
        if (v > max_version)
            continue;
        out[n++] = v;
    }
    return n;
}

Int burrow__tls_config_curve_preferences(const TlsConfig *c, uint16_t version,
                                         TlsCurveID out[8]) {
    Int n = 0;
    for (int i = 0; i < 8; i++) {
        TlsCurveID x = burrow__tls_curve_preference_order[i];
        if (burrow__tls_config_supports_curve(c, version, x))
            out[n++] = x;
    }
    return n;
}

static bool tcfg_in_preference_order(TlsCurveID x) {
    for (int i = 0; i < 8; i++) {
        if (burrow__tls_curve_preference_order[i] == x)
            return true;
    }
    return false;
}

bool burrow__tls_config_supports_curve(const TlsConfig *c, uint16_t version,
                                       TlsCurveID x) {
    if (c != NULL && c->curve_preferences.len != 0) {
        if (!tcfg_contains_u16(c->curve_preferences, x))
            return false;
        /* Ignore unimplemented entries in c.CurvePreferences. */
        if (!tcfg_in_preference_order(x))
            return false;
    } else {
        if (!burrow__tls_default_curve_enabled(x))
            return false;
    }
    if (version < TLS_VERSION_TLS13 && burrow__tls_is_tls13_only_key_exchange(x))
        return false;
    return true;
}

bool burrow__tls_config_mutual_version(const TlsConfig *c, bool is_client, bool is_quic,
                                       Slice peer_versions, uint16_t *out) {
    uint16_t supported_versions[4];
    Int n = burrow__tls_config_supported_versions(c, is_client, is_quic,
                                                  supported_versions);
    for (Int i = 0; i < n; i++) {
        if (tcfg_contains_u16(peer_versions, supported_versions[i])) {
            *out = supported_versions[i];
            return true;
        }
    }
    *out = 0;
    return false;
}

/* -------------------------------------------------------------- certificates */

BURROW_SENTINEL_ERROR(burrow__tls_err_no_certificates,
                      "tls: no certificates configured");

X509Certificate *burrow__tls_certificate_leaf(const TlsCertificate *c, Alloc *a,
                                              Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (c->leaf != NULL)
        return c->leaf;
    return x509_parse_certificate(a, BURROW_AT(Slice, c->certificate, 0), err);
}

/* c.NameToCertificate[name], NULL when it has none. */
static TlsCertificate *tcfg_name_to_certificate(Map *m, Str name) {
    TlsCertificate **cert = map_get(m, &name);
    return cert != NULL ? *cert : NULL;
}

TlsCertificate *burrow__tls_config_get_certificate(TlsConfig *c,
                                                   TlsClientHelloInfo *client_hello,
                                                   Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (c->get_certificate.f != NULL &&
        (c->certificates.len == 0 || client_hello->server_name.len > 0)) {
        Error e = BURROW_NO_ERROR;
        TlsCertificate *cert =
            c->get_certificate.f(c->get_certificate.env, client_hello, &e);
        if (cert != NULL || BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return cert;
        }
    }

    if (c->certificates.len == 0) {
        BURROW_OUT(err, burrow__tls_err_no_certificates);
        return NULL;
    }

    TlsCertificate *certs = (TlsCertificate *)c->certificates.p;
    if (c->certificates.len == 1) {
        /* There's only one choice, so no point doing any work. */
        return &certs[0];
    }

    if (c->name_to_certificate != NULL) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str name = strings_to_lower(a, client_hello->server_name);
        TlsCertificate *cert = tcfg_name_to_certificate(c->name_to_certificate, name);
        if (cert == NULL && name.len > 0) {
            /* The name with its first label made "*". */
            Int dot = strings_index_byte(name, '.');
            Str rest = dot < 0 ? BURROW_STR_EMPTY
                               : str_from_bytes(name.p + dot, name.len - dot);
            Byte *p = mem_alloc(a, (size_t)rest.len + 1, 1);
            if (p != NULL) {
                p[0] = '*';
                if (rest.len > 0)
                    memcpy(p + 1, rest.p, (size_t)rest.len);
                Str wildcard_name = {p, rest.len + 1};
                cert = tcfg_name_to_certificate(c->name_to_certificate, wildcard_name);
            }
        }
        arena_free(&ar);
        if (cert != NULL)
            return cert;
    }

    for (Int i = 0; i < c->certificates.len; i++) {
        if (!BURROW_FAILED(
                tls_client_hello_info_supports_certificate(client_hello, &certs[i])))
            return &certs[i];
    }

    /* If nothing matches, return the first certificate. */
    return &certs[0];
}

/* emptyConfig, for a ClientHelloInfo with no Config and for a Conn with none. */
static TlsConfig tcfg_empty_config = {0};

TlsConfig *burrow__tls_default_config(void) {
    return &tcfg_empty_config;
}

static bool tcfg_curve_is(EllipticCurve c, EllipticCurve want) {
    return c.vt == want.vt && c.data == want.data;
}

typedef struct TcfgSupports {
    const TlsClientHelloInfo *chi;
    const TlsCertificate *c;
    const TlsConfig *config;
    Alloc *a;
    uint16_t vers;
    bool ecdsa_cipher_suite;
} TcfgSupports;

static bool tcfg_rsa_suite_ok(const TlsSuite *suite, void *env) {
    const TcfgSupports *s = env;
    if ((suite->flags & TLS_SUITE_ECDHE) != 0)
        return false;
    if (s->vers < TLS_VERSION_TLS12 && (suite->flags & TLS_SUITE_TLS12) != 0)
        return false;
    return true;
}

/* supportsRSAFallback returns nil if the certificate and connection support the
 * static RSA key exchange, and unsupported otherwise. The logic for supporting
 * static RSA is completely disjoint from the logic for supporting signed key
 * exchanges, so we just check it as a fallback. */
static Error tcfg_supports_rsa_fallback(TcfgSupports *s, Error unsupported) {
    /* TLS 1.3 dropped support for the static RSA key exchange. */
    if (s->vers == TLS_VERSION_TLS13)
        return unsupported;
    /* The static RSA key exchange works by decrypting a challenge with the RSA
     * private key, not by signing, so check the PrivateKey implements
     * crypto.Decrypter, like *rsa.PrivateKey does. Here only an RSA key does. */
    if (s->c->private_key.t != TYPE_RSA_PRIVATE_KEY || s->c->private_key.data == NULL)
        return unsupported;
    /* Finally, there needs to be a mutual cipher suite that uses the static RSA
     * key exchange instead of ECDHE. */
    Slice supported = burrow__tls_config_supported_cipher_suites(s->config, s->a);
    if (slice_is_nil(supported))
        return burrow_err_out_of_memory;
    const TlsSuite *rsa_cipher_suite = burrow__tls_select_cipher_suite(
        (const uint16_t *)s->chi->cipher_suites.p, s->chi->cipher_suites.len,
        (const uint16_t *)supported.p, supported.len, tcfg_rsa_suite_ok, s);
    if (rsa_cipher_suite == NULL)
        return unsupported;
    return BURROW_NO_ERROR;
}

static bool tcfg_cipher_suite_ok(const TlsSuite *suite, void *env) {
    const TcfgSupports *s = env;
    if ((suite->flags & TLS_SUITE_ECDHE) == 0)
        return false;
    if ((suite->flags & TLS_SUITE_EC_SIGN) != 0) {
        if (!s->ecdsa_cipher_suite)
            return false;
    } else {
        if (s->ecdsa_cipher_suite)
            return false;
    }
    if (s->vers < TLS_VERSION_TLS12 && (suite->flags & TLS_SUITE_TLS12) != 0)
        return false;
    return true;
}

/* SupportsCertificate once the version is known, with a for what is only
 * needed while it runs. */
static Error tcfg_supports_certificate(TcfgSupports *s) {
    const TlsClientHelloInfo *chi = s->chi;
    const TlsCertificate *c = s->c;

    /* If the client specified the name they are trying to connect to, the
     * certificate needs to be valid for it. The leaf is parsed into the
     * error allocator, as the error can point at it. */
    if (chi->server_name.len > 0) {
        Error err = BURROW_NO_ERROR;
        X509Certificate *x509_cert =
            burrow__tls_certificate_leaf(c, error_allocator(), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("failed to parse certificate: %w", err);
        err = x509_certificate_verify_hostname(x509_cert, chi->server_name);
        if (BURROW_FAILED(err))
            return fmt_errorf_v(
                "certificate is not valid for requested server name: %w", err);
    }

    /* If the client sent the signature_algorithms extension, ensure it
     * supports schemes we can use with this certificate and TLS version. */
    if (chi->signature_schemes.len > 0) {
        Error err = BURROW_NO_ERROR;
        burrow__tls_select_signature_scheme(s->vers, c, chi->signature_schemes, &err);
        if (BURROW_FAILED(err))
            return tcfg_supports_rsa_fallback(s, err);
    }

    /* In TLS 1.3 we are done because supported_groups is only relevant to the
     * ECDHE computation, point format negotiation is removed, cipher suites
     * are only relevant to the AEAD choice, and static RSA does not exist. */
    if (s->vers == TLS_VERSION_TLS13)
        return BURROW_NO_ERROR;

    /* The only signed key exchange we support is ECDHE. */
    Error err = BURROW_NO_ERROR;
    bool ecdhe_supported = burrow__tls_supports_ecdhe(
        s->config, s->vers, chi->supported_curves, chi->supported_points, &err);
    if (BURROW_FAILED(err))
        return err;
    if (!ecdhe_supported)
        return tcfg_supports_rsa_fallback(
            s,
            tcfg_error(
                "client doesn't support ECDHE, can only use legacy RSA key exchange"));

    Ed25519Signer es;
    CryptoSigner priv;
    if (!burrow__tls_private_key_signer(c->private_key, &es, &priv))
        return tcfg_supports_rsa_fallback(s,
                                          burrow__tls_unsupported_certificate_error(c));
    CryptoPublicKey pub = crypto_signer_public(priv);
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY && pub.data != NULL) {
        const EcdsaPublicKey *k = pub.data;
        TlsCurveID curve;
        if (tcfg_curve_is(k->curve, elliptic_p256()))
            curve = TLS_CURVE_P256;
        else if (tcfg_curve_is(k->curve, elliptic_p384()))
            curve = TLS_CURVE_P384;
        else if (tcfg_curve_is(k->curve, elliptic_p521()))
            curve = TLS_CURVE_P521;
        else
            return tcfg_supports_rsa_fallback(
                s, burrow__tls_unsupported_certificate_error(c));
        bool curve_ok = false;
        const TlsCurveID *curves = (const TlsCurveID *)chi->supported_curves.p;
        for (Int i = 0; i < chi->supported_curves.len; i++) {
            if (curves[i] == curve &&
                burrow__tls_config_supports_curve(s->config, s->vers, curves[i])) {
                curve_ok = true;
                break;
            }
        }
        if (!curve_ok)
            return tcfg_error("client doesn't support certificate curve");
        s->ecdsa_cipher_suite = true;
    } else if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        if (s->vers < TLS_VERSION_TLS12 || chi->signature_schemes.len == 0)
            return tcfg_error("connection doesn't support Ed25519");
        s->ecdsa_cipher_suite = true;
    } else if (pub.t == TYPE_MLDSA_PUBLIC_KEY) {
        /* ML-DSA requires TLS 1.3, which we already excluded above. */
        return tcfg_error("connection doesn't support ML-DSA");
    } else if (pub.t != TYPE_RSA_PUBLIC_KEY) {
        return tcfg_supports_rsa_fallback(s,
                                          burrow__tls_unsupported_certificate_error(c));
    }

    /* Make sure that there is a mutually supported cipher suite that works
     * with this certificate. Cipher suite selection will then apply the logic
     * in reverse to pick it. See also serverHandshakeState.cipherSuiteOk. */
    Slice supported = burrow__tls_config_supported_cipher_suites(s->config, s->a);
    if (slice_is_nil(supported))
        return burrow_err_out_of_memory;
    const TlsSuite *cipher_suite = burrow__tls_select_cipher_suite(
        (const uint16_t *)chi->cipher_suites.p, chi->cipher_suites.len,
        (const uint16_t *)supported.p, supported.len, tcfg_cipher_suite_ok, s);
    if (cipher_suite == NULL)
        return tcfg_supports_rsa_fallback(
            s,
            tcfg_error("client doesn't support any cipher suites compatible with the "
                       "certificate"));

    return BURROW_NO_ERROR;
}

Error tls_client_hello_info_supports_certificate(const TlsClientHelloInfo *chi,
                                                 const TlsCertificate *c) {
    /* Note we don't currently support certificate_authorities nor
     * signature_algorithms_cert, and don't check the algorithms of the
     * signatures on the chain (which anyway are a SHOULD, see RFC 8446, Section
     * 4.4.2.2). */

    const TlsConfig *config = chi->config;
    if (config == NULL)
        config = &tcfg_empty_config;
    uint16_t vers;
    if (!burrow__tls_config_mutual_version(config, TLS_ROLE_SERVER, chi->is_quic,
                                           chi->supported_versions, &vers))
        return tcfg_error("no mutually supported protocol versions");

    /* What an answer of yes made in the error allocator is given back. */
    ArenaMark mark = error_mark();
    Arena ar;
    arena_init(&ar, NULL, 0);
    TcfgSupports s = {chi, c, config, arena_allocator(&ar), vers, false};
    Error err = tcfg_supports_certificate(&s);
    arena_free(&ar);
    if (!BURROW_FAILED(err))
        error_release(mark);
    return err;
}

Error tls_certificate_request_info_supports_certificate(
    const TlsCertificateRequestInfo *cri, const TlsCertificate *c) {
    Error err = BURROW_NO_ERROR;
    burrow__tls_select_signature_scheme(cri->version, c, cri->signature_schemes, &err);
    if (BURROW_FAILED(err))
        return err;

    if (cri->acceptable_cas.len == 0)
        return BURROW_NO_ERROR;

    /* The certificates are parsed into the error allocator, as an error can
     * point at what was parsed, and given back unless one is returned. */
    ArenaMark mark = error_mark();
    const Slice *chain = (const Slice *)c->certificate.p;
    const Slice *cas = (const Slice *)cri->acceptable_cas.p;
    for (Int j = 0; j < c->certificate.len; j++) {
        const X509Certificate *x509_cert = c->leaf;
        /* Parse the certificate if this isn't the leaf node, or if chain.Leaf
         * was nil. */
        if (j != 0 || x509_cert == NULL) {
            x509_cert = x509_parse_certificate(error_allocator(), chain[j], &err);
            if (BURROW_FAILED(err))
                return fmt_errorf_v("failed to parse certificate #%d in the chain: %w",
                                    j, err);
        }

        for (Int i = 0; i < cri->acceptable_cas.len; i++) {
            if (bytes_equal(x509_cert->raw_issuer, cas[i])) {
                error_release(mark);
                return BURROW_NO_ERROR;
            }
        }
    }
    error_release(mark);
    return tcfg_error("chain is not signed by an acceptable CA");
}

Error tls_config_build_name_to_certificate(TlsConfig *c, Alloc *a) {
    Map *name_to_certificate = map_make(a, TYPE_STRING, TYPE_TLS_CERTIFICATE_PTR, 0);
    if (name_to_certificate == NULL)
        return burrow_err_out_of_memory;
    TlsCertificate *certs = (TlsCertificate *)c->certificates.p;
    for (Int i = 0; i < c->certificates.len; i++) {
        TlsCertificate *cert = &certs[i];
        ArenaMark mark = error_mark();
        Error err = BURROW_NO_ERROR;
        X509Certificate *x509_cert = burrow__tls_certificate_leaf(cert, a, &err);
        error_release(mark);
        if (BURROW_FAILED(err))
            continue;
        /* If SANs are *not* present, some clients will consider the
         * certificate valid for the name in the Common Name. */
        if (x509_cert->subject.common_name.len > 0 && x509_cert->dns_names.len == 0) {
            if (!map_set(name_to_certificate, &x509_cert->subject.common_name, &cert))
                return burrow_err_out_of_memory;
        }
        const Str *sans = (const Str *)x509_cert->dns_names.p;
        for (Int j = 0; j < x509_cert->dns_names.len; j++) {
            if (!map_set(name_to_certificate, &sans[j], &cert))
                return burrow_err_out_of_memory;
        }
    }
    c->name_to_certificate = name_to_certificate;
    return BURROW_NO_ERROR;
}

/* writerMutex protects all KeyLogWriters globally. It is rarely enabled, and
 * is only for debugging, so a global mutex saves space. */
static SyncMutex tcfg_writer_mutex;

Error burrow__tls_config_write_key_log(const TlsConfig *c, Str label,
                                       Slice client_random, Slice secret) {
    if (c->key_log_writer.vt == NULL)
        return BURROW_NO_ERROR;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Str log_line =
        fmt_sprintf_v(arena_allocator(&ar), "%s %x %x\n", label, client_random, secret);

    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&tcfg_writer_mutex);
    c->key_log_writer.vt->write(c->key_log_writer.data,
                                slice_from((void *)(uintptr_t)log_line.p, log_line.len,
                                           log_line.len, TYPE_BYTE),
                                &err);
    sync_mutex_unlock(&tcfg_writer_mutex);
    arena_free(&ar);

    if (BURROW_FAILED(err))
        return fmt_errorf_v("tls: KeyLogWriter: %w", err);
    return BURROW_NO_ERROR;
}

bool burrow__tls_supports_ecdhe(const TlsConfig *c, uint16_t version,
                                Slice supported_curves, Slice supported_points,
                                Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    bool supports_curve = false;
    const TlsCurveID *curves = (const TlsCurveID *)supported_curves.p;
    for (Int i = 0; i < supported_curves.len; i++) {
        if (burrow__tls_config_supports_curve(c, version, curves[i])) {
            supports_curve = true;
            break;
        }
    }

    bool supports_point_format = false;
    bool offered_non_compressed_format = false;
    const uint8_t *points = (const uint8_t *)supported_points.p;
    for (Int i = 0; i < supported_points.len; i++) {
        if (points[i] == TLS_POINT_FORMAT_UNCOMPRESSED)
            supports_point_format = true;
        else
            offered_non_compressed_format = true;
    }
    /* Per RFC 8422, Section 5.1.2, if the Supported Point Formats extension is
     * missing, uncompressed points are supported. If supportedPoints is empty,
     * the extension must be missing, as an empty extension body is rejected by
     * the parser. See https://go.dev/issue/49126. */
    if (supported_points.len == 0) {
        supports_point_format = true;
    } else if (offered_non_compressed_format && !supports_point_format) {
        BURROW_OUT(err,
                   tcfg_error("tls: client offered only incompatible point formats"));
        return false;
    }

    return supports_curve && supports_point_format;
}

bool burrow__tls_requires_client_cert(TlsClientAuthType c) {
    switch (c) {
    case TLS_REQUIRE_ANY_CLIENT_CERT:
    case TLS_REQUIRE_AND_VERIFY_CLIENT_CERT:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------- connection state */

Slice tls_connection_state_export_keying_material(const TlsConnectionState *cs,
                                                  Alloc *a, Str label, Slice context,
                                                  Int length, Error *err) {
    return cs->ekm.f(cs->ekm.env, a, label, context, length, err);
}

/* ------------------------------------------------------ LRU session cache */

/* An entry of the list, linked by index. key is the cache's own copy. */
typedef struct TcfgLruEntry {
    Str key;
    TlsClientSessionState *state;
    Int prev;
    Int next;
} TcfgLruEntry;

/* lruSessionCache is a ClientSessionCache implementation that uses an LRU
 * caching strategy.
 *
 * Go's list.List is an array of capacity entries here, linked by index, with
 * the unused ones on a list of their own. m maps a key to its index. */
typedef struct TcfgLruSessionCache {
    SyncMutex mu;
    Alloc *a;
    Map *m;
    TcfgLruEntry *entries;
    Int capacity;
    Int len;
    Int front;
    Int back;
    Int free;
} TcfgLruSessionCache;

static void tcfg_lru_unlink(TcfgLruSessionCache *c, Int i) {
    TcfgLruEntry *e = &c->entries[i];
    if (e->prev >= 0)
        c->entries[e->prev].next = e->next;
    else
        c->front = e->next;
    if (e->next >= 0)
        c->entries[e->next].prev = e->prev;
    else
        c->back = e->prev;
}

static void tcfg_lru_push_front(TcfgLruSessionCache *c, Int i) {
    TcfgLruEntry *e = &c->entries[i];
    e->prev = -1;
    e->next = c->front;
    if (c->front >= 0)
        c->entries[c->front].prev = i;
    else
        c->back = i;
    c->front = i;
}

static void tcfg_lru_free_key(TcfgLruSessionCache *c, TcfgLruEntry *e) {
    if (e->key.len > 0)
        mem_free(c->a, (void *)(uintptr_t)e->key.p, (size_t)e->key.len, 1);
    e->key = BURROW_STR_EMPTY;
}

/* Takes entry i out of the list and the map and puts it on the free list. The
 * session it held is what the caller releases, after unlocking. */
static TlsClientSessionState *tcfg_lru_remove(TcfgLruSessionCache *c, Int i) {
    TcfgLruEntry *e = &c->entries[i];
    TlsClientSessionState *old = e->state;
    tcfg_lru_unlink(c, i);
    map_del(c->m, &e->key);
    tcfg_lru_free_key(c, e);
    e->state = NULL;
    e->next = c->free;
    c->free = i;
    c->len--;
    return old;
}

/* Put adds the provided (sessionKey, cs) pair to the cache. If cs is nil, the
 * entry corresponding to sessionKey is removed from the cache instead.
 *
 * The cache takes a reference to cs and releases the one to whatever cs puts
 * out, once the mutex is unlocked, since that can free a session. */
static void tcfg_lru_put(void *self, Str session_key, TlsClientSessionState *cs) {
    TcfgLruSessionCache *c = self;
    TlsClientSessionState *old = NULL;
    sync_mutex_lock(&c->mu);

    Int *elem = map_get(c->m, &session_key);
    if (elem != NULL) {
        Int i = *elem;
        if (cs == NULL) {
            old = tcfg_lru_remove(c, i);
        } else {
            old = c->entries[i].state;
            c->entries[i].state = tls_client_session_state_retain(cs);
            tcfg_lru_unlink(c, i);
            tcfg_lru_push_front(c, i);
        }
        sync_mutex_unlock(&c->mu);
        tls_client_session_state_release(old);
        return;
    }
    if (cs == NULL) {
        sync_mutex_unlock(&c->mu);
        return;
    }

    /* Here the oldest entry gives up its place when the cache is full, as Go
     * reuses it. */
    if (c->len == c->capacity)
        old = tcfg_lru_remove(c, c->back);

    Str key = str_clone(c->a, session_key);
    if (key.len != session_key.len) {
        /* Out of memory: the session is not cached, which a cache may do. */
        sync_mutex_unlock(&c->mu);
        tls_client_session_state_release(old);
        return;
    }
    Int i = c->free;
    TcfgLruEntry *e = &c->entries[i];
    c->free = e->next;
    e->key = key;
    if (!map_set(c->m, &e->key, &i)) {
        tcfg_lru_free_key(c, e);
        e->next = c->free;
        c->free = i;
        sync_mutex_unlock(&c->mu);
        tls_client_session_state_release(old);
        return;
    }
    e->state = tls_client_session_state_retain(cs);
    tcfg_lru_push_front(c, i);
    c->len++;
    sync_mutex_unlock(&c->mu);
    tls_client_session_state_release(old);
}

/* Get returns the ClientSessionState value associated with a given key. It
 * returns (nil, false) if no value is found. The caller gets a reference of
 * its own. */
static TlsClientSessionState *tcfg_lru_get(void *self, Str session_key, bool *ok) {
    TcfgLruSessionCache *c = self;
    sync_mutex_lock(&c->mu);
    Int *elem = map_get(c->m, &session_key);
    if (elem != NULL) {
        Int i = *elem;
        tcfg_lru_unlink(c, i);
        tcfg_lru_push_front(c, i);
        TlsClientSessionState *state =
            tls_client_session_state_retain(c->entries[i].state);
        sync_mutex_unlock(&c->mu);
        *ok = true;
        return state;
    }
    sync_mutex_unlock(&c->mu);
    *ok = false;
    return NULL;
}

static const TlsClientSessionCacheVT tcfg_lru_session_cache_vt = {NULL, tcfg_lru_get,
                                                                  tcfg_lru_put};

TlsClientSessionCache tls_new_lru_client_session_cache(Alloc *a, Int capacity) {
    enum { default_session_cache_capacity = 64 };

    TlsClientSessionCache out = {NULL, NULL};
    if (capacity < 1)
        capacity = default_session_cache_capacity;
    TcfgLruSessionCache *c = BURROW_NEW(a, TcfgLruSessionCache);
    if (c == NULL)
        return out;
    c->entries =
        mem_alloc(a, (size_t)capacity * sizeof(TcfgLruEntry), _Alignof(TcfgLruEntry));
    if (c->entries == NULL) {
        mem_free(a, c, sizeof(*c), _Alignof(TcfgLruSessionCache));
        return out;
    }
    c->m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    if (c->m == NULL) {
        mem_free(a, c->entries, (size_t)capacity * sizeof(TcfgLruEntry),
                 _Alignof(TcfgLruEntry));
        mem_free(a, c, sizeof(*c), _Alignof(TcfgLruSessionCache));
        return out;
    }
    c->a = a;
    c->capacity = capacity;
    c->front = -1;
    c->back = -1;
    for (Int i = 0; i < capacity; i++)
        c->entries[i].next = i + 1 < capacity ? i + 1 : -1;
    c->free = 0;
    out.vt = &tcfg_lru_session_cache_vt;
    out.data = c;
    return out;
}

void tls_lru_client_session_cache_free(TlsClientSessionCache cache) {
    if (cache.vt != &tcfg_lru_session_cache_vt)
        return;
    TcfgLruSessionCache *c = cache.data;
    while (c->front >= 0)
        tls_client_session_state_release(tcfg_lru_remove(c, c->front));
    map_free(c->m);
    mem_free(c->a, c->entries, (size_t)c->capacity * sizeof(TcfgLruEntry),
             _Alignof(TcfgLruEntry));
    mem_free(c->a, c, sizeof(*c), _Alignof(TcfgLruSessionCache));
}

/* CertificateVerificationError as an Error. The error first, so the data
 * pointer is a pointer to it and errors_as hands it straight back. */
typedef struct TcfgVerificationBox {
    TlsCertificateVerificationError e;
    Str text;
} TcfgVerificationBox;

static const Type tcfg_verification_error_desc = {
    BURROW_S_INIT("CertificateVerificationError"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(TlsCertificateVerificationError),
    (uint16_t)_Alignof(TlsCertificateVerificationError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c6376U, /* "tlcv" */
    NULL,
};

const Type *const TYPE_TLS_CERTIFICATE_VERIFICATION_ERROR =
    &tcfg_verification_error_desc;

static Str tcfg_cve_message(const void *self) {
    return ((const TcfgVerificationBox *)self)->text;
}

static Error tcfg_cve_unwrap(const void *self) {
    return ((const TcfgVerificationBox *)self)->e.err;
}

static Error tcfg_cve_clone(const void *self, Alloc *a) {
    return tls_certificate_verification_error_as_error(
        &((const TcfgVerificationBox *)self)->e, a);
}

static const ErrorVT tcfg_cve_vt = {
    .self_type = &tcfg_verification_error_desc,
    .message = tcfg_cve_message,
    .unwrap = tcfg_cve_unwrap,
    .clone = tcfg_cve_clone,
};

Str tls_certificate_verification_error_error(const TlsCertificateVerificationError *e,
                                             Alloc *a) {
    return fmt_sprintf_v(a, "tls: failed to verify certificate: %v", e->err);
}

Error tls_certificate_verification_error_as_error(
    const TlsCertificateVerificationError *e, Alloc *a) {
    TcfgVerificationBox *b = BURROW_NEW(a, TcfgVerificationBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = *e;
    b->e.err = error_retain(a, e->err);
    b->text = tls_certificate_verification_error_error(e, a);
    if (b->text.p == NULL)
        return burrow_err_out_of_memory;
    return (Error){&tcfg_cve_vt, b};
}

Slice burrow__tls_fips_allowed_chains(Slice chains, Error *err) {
    (void)err;
    return chains;
}

/* Whether some certificate in chain is outside its validity period at now. */
static bool tcfg_chain_expired(const X509CertificateChain *chain, Time now) {
    X509Certificate *const *certs = chain->p;
    for (Int i = 0; i < chain->len; i++) {
        if (time_before(now, certs[i]->not_before) ||
            time_after(now, certs[i]->not_after))
            return true;
    }
    return false;
}

/* Whether cert verifies with opts. The chains are made in a scratch arena and
 * thrown away. */
static bool tcfg_verifies(const X509Certificate *cert, X509VerifyOptions opts) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    (void)x509_certificate_verify(cert, arena_allocator(&ar), opts, &err);
    arena_free(&ar);
    return BURROW_OK(err);
}

bool burrow__tls_any_valid_verified_chain(Slice verified_chains,
                                          X509VerifyOptions opts) {
    const X509CertificateChain *chains = verified_chains.p;
    for (Int i = 0; i < verified_chains.len; i++) {
        const X509CertificateChain *chain = &chains[i];
        if (chain->len == 0)
            continue;
        if (tcfg_chain_expired(chain, opts.current_time))
            continue;
        X509Certificate *const *certs = chain->p;
        /* Since we already validated the chain, we only care that it is rooted
         * in a CA in opts.Roots. On platforms where we control chain validation
         * (e.g. not Windows or macOS) this is a simple lookup in the CertPool
         * internal hash map, which we can simulate by running Verify on the
         * root. On other platforms, we have to do full verification again,
         * because EKU handling might differ. We will want to replace this with
         * CertPool.Contains if/once that is available. See go.dev/issue/77376. */
#if defined(_WIN32) || defined(__APPLE__)
        X509CertPool *intermediates = x509_new_cert_pool(heap_allocator());
        if (intermediates == NULL)
            continue;
        Int end = chain->len - 1 > 1 ? chain->len - 1 : 1;
        for (Int j = 1; j < end; j++)
            x509_cert_pool_add_cert(intermediates, certs[j]);
        opts.intermediates = intermediates;
        bool ok = tcfg_verifies(certs[0], opts);
        x509_cert_pool_free(intermediates);
        if (ok)
            return true;
#else
        if (tcfg_verifies(certs[chain->len - 1], opts))
            return true;
#endif
    }
    return false;
}
