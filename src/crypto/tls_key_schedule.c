/* crypto/tls: the TLS 1.3 key schedule, which is crypto/internal/fips140/tls13
 * in Go, and the parts of key_schedule.go built on it.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/fips140.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/sha256.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "fips140_module.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A copy of s in a, nil when s is nil or a is out of memory. */
CRYPTO_FIPS_TEXT static Slice tls13_keep(Alloc *a, Slice s) {
    if (s.p == NULL)
        return slice_nil(TYPE_BYTE);
    Slice out = slice_make(a, TYPE_BYTE, s.len, s.len);
    if (out.p == NULL)
        return out;
    memcpy(out.p, s.p, (size_t)s.len);
    return out;
}

/* ExpandLabel into scratch, which the callers here use for what they throw
 * away. */
CRYPTO_FIPS_TEXT static Slice tls13_expand_label(Alloc *scratch, HashNewFunc h,
                                                 Slice secret, Str label, Slice context,
                                                 Int length) {
    static const char prefix[] = "tls13 ";
    Int pl = (Int)sizeof prefix - 1;
    if (pl + label.len > 255 || context.len > 255) {
        /* It should be impossible for this to panic: labels are fixed strings,
         * and context is either a fixed-length computed hash, or parsed from a
         * field which has the same length limitation.
         *
         * Another reasonable approach might be to return a randomized slice if
         * we encounter an error, which would break the connection, but avoid
         * panicking. This would perhaps be safer but significantly more
         * confusing to users. */
        panic_str(BURROW_S("tls13: label or context too long"));
    }
    Hash probe = h(scratch);
    if (probe.vt == NULL)
        return slice_nil(TYPE_BYTE);
    if (length > 255 * hash_size(probe))
        panic_str(BURROW_S("hkdf: counter overflow"));

    Int n = 2 + 1 + pl + label.len + 1 + context.len;
    Byte *p = (Byte *)mem_alloc_nozero(scratch, (size_t)n, 1);
    if (p == NULL)
        return slice_nil(TYPE_BYTE);
    Byte *q = p;
    *q++ = (Byte)((uint16_t)length >> 8);
    *q++ = (Byte)length;
    *q++ = (Byte)(pl + label.len);
    memcpy(q, prefix, (size_t)pl);
    q += pl;
    if (label.len > 0)
        memcpy(q, label.p, (size_t)label.len);
    q += label.len;
    *q++ = (Byte)context.len;
    if (context.len > 0)
        memcpy(q, context.p, (size_t)context.len);

    Error err = BURROW_NO_ERROR;
    Slice out =
        burrow__hkdf_expand(scratch, h, secret, str_from_bytes(p, n), length, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    return out;
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_expand_label(Alloc *a, HashNewFunc h, Slice secret,
                                                  Str label, Slice context,
                                                  Int length) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = tls13_keep(a, tls13_expand_label(arena_allocator(&scratch), h, secret,
                                                 label, context, length));
    arena_free(&scratch);
    return out;
}

/* extract, into scratch. A nil new_secret is zeros as long as the hash. */
CRYPTO_FIPS_TEXT static Slice tls13_extract(Alloc *scratch, HashNewFunc h,
                                            Slice new_secret, Slice current_secret) {
    if (slice_is_nil(new_secret)) {
        Hash probe = h(scratch);
        if (probe.vt == NULL)
            return slice_nil(TYPE_BYTE);
        Int size = hash_size(probe);
        new_secret = slice_make(scratch, TYPE_BYTE, size, size);
        if (new_secret.p == NULL)
            return new_secret;
    }
    Error err = BURROW_NO_ERROR;
    Slice out = burrow__hkdf_extract(scratch, h, new_secret, current_secret, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    return out;
}

/* deriveSecret, into scratch. A nil transcript is a fresh hash. */
CRYPTO_FIPS_TEXT static Slice tls13_derive_secret(Alloc *scratch, HashNewFunc h,
                                                  Slice secret, Str label,
                                                  Hash transcript) {
    if (transcript.vt == NULL) {
        transcript = h(scratch);
        if (transcript.vt == NULL)
            return slice_nil(TYPE_BYTE);
    }
    Slice sum = hash_sum(scratch, transcript, slice_nil(TYPE_BYTE));
    if (sum.p == NULL)
        return sum;
    return tls13_expand_label(scratch, h, secret, label, sum, hash_size(transcript));
}

/* deriveSecret, into a. */
CRYPTO_FIPS_TEXT static Slice tls13_derive(Alloc *a, HashNewFunc h, Slice secret,
                                           Str label, Hash transcript) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = tls13_keep(a, tls13_derive_secret(arena_allocator(&scratch), h, secret,
                                                  label, transcript));
    arena_free(&scratch);
    return out;
}

/* extract(new_secret, deriveSecret(secret, "derived", nil)), into a. */
CRYPTO_FIPS_TEXT static Slice tls13_next_stage(Alloc *a, HashNewFunc h, Slice secret,
                                               Slice new_secret) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Slice out = slice_nil(TYPE_BYTE);
    Slice derived =
        tls13_derive_secret(s, h, secret, BURROW_S("derived"), (Hash){NULL, NULL});
    if (derived.p != NULL)
        out = tls13_keep(a, tls13_extract(s, h, new_secret, derived));
    arena_free(&scratch);
    return out;
}

CRYPTO_FIPS_TEXT Tls13EarlySecret burrow__tls13_new_early_secret(Alloc *a,
                                                                 HashNewFunc h,
                                                                 Slice psk) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Tls13EarlySecret s = {
        tls13_keep(
            a, tls13_extract(arena_allocator(&scratch), h, psk, slice_nil(TYPE_BYTE))),
        h,
    };
    arena_free(&scratch);
    return s;
}

CRYPTO_FIPS_TEXT Slice
burrow__tls13_early_secret_resumption_binder_key(const Tls13EarlySecret *s, Alloc *a) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("res binder"),
                        (Hash){NULL, NULL});
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_early_secret_client_early_traffic_secret(
    const Tls13EarlySecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c e traffic"), transcript);
}

CRYPTO_FIPS_TEXT Tls13HandshakeSecret burrow__tls13_early_secret_handshake_secret(
    const Tls13EarlySecret *s, Alloc *a, Slice shared_secret) {
    Tls13HandshakeSecret hs = {
        tls13_next_stage(a, s->hash, s->secret, shared_secret),
        s->hash,
    };
    return hs;
}

CRYPTO_FIPS_TEXT Tls13ExporterMasterSecret
burrow__tls13_early_secret_early_exporter_master_secret(const Tls13EarlySecret *s,
                                                        Alloc *a, Hash transcript) {
    Tls13ExporterMasterSecret e = {
        tls13_derive(a, s->hash, s->secret, BURROW_S("e exp master"), transcript),
        s->hash,
    };
    return e;
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_handshake_secret_client_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c hs traffic"), transcript);
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_handshake_secret_server_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("s hs traffic"), transcript);
}

CRYPTO_FIPS_TEXT Tls13MasterSecret
burrow__tls13_handshake_secret_master_secret(const Tls13HandshakeSecret *s, Alloc *a) {
    Tls13MasterSecret ms = {
        tls13_next_stage(a, s->hash, s->secret, slice_nil(TYPE_BYTE)),
        s->hash,
    };
    return ms;
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_master_secret_client_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c ap traffic"), transcript);
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_master_secret_server_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("s ap traffic"), transcript);
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_master_secret_resumption_master_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("res master"), transcript);
}

CRYPTO_FIPS_TEXT Tls13ExporterMasterSecret
burrow__tls13_master_secret_exporter_master_secret(const Tls13MasterSecret *s, Alloc *a,
                                                   Hash transcript) {
    Tls13ExporterMasterSecret e = {
        tls13_derive(a, s->hash, s->secret, BURROW_S("exp master"), transcript),
        s->hash,
    };
    return e;
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_exporter_master_secret_exporter(
    const Tls13ExporterMasterSecret *s, Alloc *a, Str label, Slice context,
    Int length) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sc = arena_allocator(&scratch);
    Slice out = slice_nil(TYPE_BYTE);
    Slice secret =
        tls13_derive_secret(sc, s->hash, s->secret, label, (Hash){NULL, NULL});
    Hash h = s->hash(sc);
    if (secret.p != NULL && h.vt != NULL) {
        Error err = BURROW_NO_ERROR;
        hash_write(h, context, &err);
        Slice sum = hash_sum(sc, h, slice_nil(TYPE_BYTE));
        if (sum.p != NULL)
            out = tls13_keep(a, tls13_expand_label(sc, s->hash, secret,
                                                   BURROW_S("exporter"), sum, length));
    }
    arena_free(&scratch);
    return out;
}

/* ------------------------------------------------------- key_schedule.go */

CRYPTO_FIPS_TEXT Slice burrow__tls13_next_traffic_secret(const TlsSuiteTLS13 *c,
                                                         Alloc *a,
                                                         Slice traffic_secret) {
    HashNewFunc h = burrow__tls_suite13_hash_new(c);
    return burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("traffic upd"),
                                      slice_nil(TYPE_BYTE), crypto_hash_size(c->hash));
}

CRYPTO_FIPS_TEXT bool burrow__tls13_traffic_key(const TlsSuiteTLS13 *c, Alloc *a,
                                                Slice traffic_secret, Slice *key,
                                                Slice *iv) {
    HashNewFunc h = burrow__tls_suite13_hash_new(c);
    *key = burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("key"),
                                      slice_nil(TYPE_BYTE), c->key_len);
    *iv = burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("iv"),
                                     slice_nil(TYPE_BYTE), TLS_AEAD_NONCE_LENGTH);
    return key->p != NULL && iv->p != NULL;
}

CRYPTO_FIPS_TEXT Slice burrow__tls13_finished_hash(const TlsSuiteTLS13 *c, Alloc *a,
                                                   Slice base_key, Hash transcript) {
    HashNewFunc h = burrow__tls_suite13_hash_new(c);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Slice out = slice_nil(TYPE_BYTE);
    Slice finished_key =
        tls13_expand_label(s, h, base_key, BURROW_S("finished"), slice_nil(TYPE_BYTE),
                           crypto_hash_size(c->hash));
    if (finished_key.p != NULL) {
        Hash verify_data = hmac_new(s, h, finished_key);
        Slice sum = hash_sum(s, transcript, slice_nil(TYPE_BYTE));
        if (verify_data.vt != NULL && sum.p != NULL) {
            Error err = BURROW_NO_ERROR;
            hash_write(verify_data, sum, &err);
            out = hash_sum(a, verify_data, slice_nil(TYPE_BYTE));
        }
    }
    arena_free(&scratch);
    return out;
}

CRYPTO_FIPS_TEXT static Slice tls13_ekm_call(void *env, Alloc *a, Str label,
                                             Slice context, Int length, Error *err) {
    Slice out = burrow__tls13_exporter_master_secret_exporter(
        (const Tls13ExporterMasterSecret *)env, a, label, context, length);
    if (out.p == NULL && length > 0)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}

CRYPTO_FIPS_TEXT TlsEkmFunc burrow__tls13_export_keying_material(
    const TlsSuiteTLS13 *c, Alloc *a, const Tls13MasterSecret *s, Hash transcript) {
    (void)c;
    TlsEkmFunc f = {0};
    Tls13ExporterMasterSecret *exp_master_secret =
        BURROW_NEW(a, Tls13ExporterMasterSecret);
    if (exp_master_secret == NULL)
        return f;
    *exp_master_secret =
        burrow__tls13_master_secret_exporter_master_secret(s, a, transcript);
    if (exp_master_secret->secret.p == NULL)
        return f;
    return BURROW_FN(TlsEkmFunc, tls13_ekm_call, exp_master_secret);
}

/* ------------------------------------------------------- key exchanges */

BURROW_SENTINEL_ERROR(tke_err_unsupported, "tls: unsupported key exchange");
BURROW_SENTINEL_ERROR(tke_err_client_share_length,
                      "tls: invalid client key share length for hybrid key exchange");
BURROW_SENTINEL_ERROR(tke_err_server_share_length,
                      "tls: invalid server key share length for hybrid key exchange");

enum { TKE_ECDH, TKE_HYBRID, TKE_MLKEM1024 };

/* ecdhKeyExchange. */
typedef struct TkeEcdh {
    TlsCurveID id;
    const EcdhCurve *(*curve)(void);
} TkeEcdh;

struct TlsKeyExchange {
    /* The rest of a hybridKeyExchange, whose mlkemGenerateKey and
     * mlkemNewPublicKey are those for ML-KEM-1024 when mlkem1024 is set and
     * for ML-KEM-768 when it is not. */
    Int ecdh_element_size;
    Int mlkem_public_key_size;
    Int mlkem_ciphertext_size;

    /* The whole of an ecdhKeyExchange, or the ECDH part of a hybrid. */
    TkeEcdh ecdh;

    int kind;
    TlsCurveID id;
    bool mlkem1024;
};

static const TlsKeyExchange tke_exchanges[] = {
    {0, 0, 0, {TLS_X25519, ecdh_x25519}, TKE_ECDH, TLS_X25519, false},
    {0, 0, 0, {TLS_CURVE_P256, ecdh_p256}, TKE_ECDH, TLS_CURVE_P256, false},
    {0, 0, 0, {TLS_CURVE_P384, ecdh_p384}, TKE_ECDH, TLS_CURVE_P384, false},
    {0, 0, 0, {TLS_CURVE_P521, ecdh_p521}, TKE_ECDH, TLS_CURVE_P521, false},
    {32,
     MLKEM_ENCAPSULATION_KEY_SIZE768,
     MLKEM_CIPHERTEXT_SIZE768,
     {TLS_X25519, ecdh_x25519},
     TKE_HYBRID,
     TLS_X25519MLKEM768,
     false},
    {65,
     MLKEM_ENCAPSULATION_KEY_SIZE768,
     MLKEM_CIPHERTEXT_SIZE768,
     {TLS_CURVE_P256, ecdh_p256},
     TKE_HYBRID,
     TLS_SEC_P256_R1_MLKEM768,
     false},
    {97,
     MLKEM_ENCAPSULATION_KEY_SIZE1024,
     MLKEM_CIPHERTEXT_SIZE1024,
     {TLS_CURVE_P384, ecdh_p384},
     TKE_HYBRID,
     TLS_SEC_P384_R1_MLKEM1024,
     true},
    {0, 0, 0, {0, NULL}, TKE_MLKEM1024, TLS_MLKEM1024, true},
};

CRYPTO_FIPS_TEXT const TlsKeyExchange *
burrow__tls_key_exchange_for_curve_id(TlsCurveID id, Error *err) {
    for (size_t i = 0; i < sizeof(tke_exchanges) / sizeof(tke_exchanges[0]); i++) {
        if (tke_exchanges[i].id == id)
            return &tke_exchanges[i];
    }
    BURROW_OUT(err, tke_err_unsupported);
    return NULL;
}

CRYPTO_FIPS_TEXT void
burrow__tls_key_share_private_keys_free(TlsKeySharePrivateKeys *k) {
    ecdh_private_key_free(k->ecdhe);
    mlkem_decapsulation_key768_free(k->mlkem768);
    mlkem_decapsulation_key1024_free(k->mlkem1024);
    memset(k, 0, sizeof *k);
}

/* x then y, from a, or nil when a is out of memory. */
CRYPTO_FIPS_TEXT static Slice tke_concat(Alloc *a, Slice x, Slice y) {
    Slice out = slice_make(a, TYPE_BYTE, x.len + y.len, x.len + y.len);
    if (out.p == NULL)
        return out;
    if (x.len > 0)
        memcpy(out.p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy((Byte *)out.p + x.len, y.p, (size_t)y.len);
    return out;
}

/* mlkemGenerateKey, into priv. */
CRYPTO_FIPS_TEXT static bool
tke_mlkem_generate_key(bool k1024, Alloc *a, TlsKeySharePrivateKeys *priv, Error *err) {
    if (k1024) {
        priv->mlkem1024 = mlkem_generate_key1024(a, err);
        if (priv->mlkem1024 == NULL)
            return false;
        priv->mlkem = mlkem_decapsulation_key1024_as_decapsulator(priv->mlkem1024);
    } else {
        priv->mlkem768 = mlkem_generate_key768(a, err);
        if (priv->mlkem768 == NULL)
            return false;
        priv->mlkem = mlkem_decapsulation_key768_as_decapsulator(priv->mlkem768);
    }
    return true;
}

/* mlkemNewPublicKey and then Encapsulate: the shared key, with the ciphertext
 * in *ciphertext, both from a. */
CRYPTO_FIPS_TEXT static Slice tke_mlkem_encapsulate(bool k1024, Alloc *a,
                                                    Slice public_key, Slice *ciphertext,
                                                    Error *err) {
    Slice shared_key = slice_nil(TYPE_BYTE);
    if (k1024) {
        MlkemEncapsulationKey1024 *ek =
            mlkem_new_encapsulation_key1024(a, public_key, err);
        if (ek == NULL)
            return shared_key;
        shared_key = mlkem_encapsulation_key1024_encapsulate(ek, a, ciphertext);
        mlkem_encapsulation_key1024_free(ek);
    } else {
        MlkemEncapsulationKey768 *ek =
            mlkem_new_encapsulation_key768(a, public_key, err);
        if (ek == NULL)
            return shared_key;
        shared_key = mlkem_encapsulation_key768_encapsulate(ek, a, ciphertext);
        mlkem_encapsulation_key768_free(ek);
    }
    if (shared_key.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return shared_key;
}

CRYPTO_FIPS_TEXT static bool tke_ecdh_key_shares(const TkeEcdh *ke, Alloc *a,
                                                 IoReader rand,
                                                 TlsKeySharePrivateKeys *priv,
                                                 TlsKeyShare *share, Error *err) {
    EcdhPrivateKey *key = ecdh_curve_generate_key(ke->curve(), a, rand, err);
    if (key == NULL)
        return false;
    Slice data = ecdh_public_key_bytes(ecdh_private_key_public_key(key), a);
    if (data.p == NULL) {
        ecdh_private_key_free(key);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return false;
    }
    priv->ecdhe = key;
    *share = (TlsKeyShare){ke->id, data};
    return true;
}

CRYPTO_FIPS_TEXT static Slice
tke_ecdh_server_shared_secret(const TkeEcdh *ke, Alloc *a, IoReader rand,
                              Slice client_key_share, TlsKeyShare *ks, Error *err) {
    Slice shared_key = slice_nil(TYPE_BYTE);
    EcdhPrivateKey *key = ecdh_curve_generate_key(ke->curve(), a, rand, err);
    if (key == NULL)
        return shared_key;
    EcdhPublicKey *peer_key =
        ecdh_curve_new_public_key(ke->curve(), a, client_key_share, err);
    if (peer_key != NULL) {
        Slice secret = ecdh_private_key_ecdh(key, a, peer_key, err);
        if (secret.p != NULL) {
            Slice data = ecdh_public_key_bytes(ecdh_private_key_public_key(key), a);
            if (data.p == NULL) {
                BURROW_OUT(err, burrow_err_out_of_memory);
            } else {
                shared_key = secret;
                *ks = (TlsKeyShare){ke->id, data};
            }
        }
        ecdh_public_key_free(peer_key);
    }
    ecdh_private_key_free(key);
    return shared_key;
}

CRYPTO_FIPS_TEXT static Slice
tke_ecdh_client_shared_secret(const TkeEcdh *ke, Alloc *a,
                              const TlsKeySharePrivateKeys *priv,
                              Slice server_key_share, Error *err) {
    EcdhPublicKey *peer_key =
        ecdh_curve_new_public_key(ke->curve(), a, server_key_share, err);
    if (peer_key == NULL)
        return slice_nil(TYPE_BYTE);
    Slice shared_key = ecdh_private_key_ecdh(priv->ecdhe, a, peer_key, err);
    ecdh_public_key_free(peer_key);
    return shared_key;
}

/* The ECDH half of a hybrid, which Go runs under fips140.WithoutEnforcement,
 * being a hybrid of ML-KEM, which is Approved. Not in Go: the arguments and
 * results of the three calls, for the Func that fips140_without_enforcement
 * runs. */
typedef struct TkeEcdhCall {
    enum { TKE_ECDH_KEY_SHARES, TKE_ECDH_SERVER, TKE_ECDH_CLIENT } op;
    const TkeEcdh *ke;
    Alloc *a;
    IoReader rand;
    TlsKeySharePrivateKeys *priv;
    const TlsKeySharePrivateKeys *cpriv;
    Slice key_share;
    TlsKeyShare *ks;
    Error *err;
    bool ok;
    Slice shared_key;
} TkeEcdhCall;

CRYPTO_FIPS_TEXT static void tke_ecdh_call_run(void *env) {
    TkeEcdhCall *c = env;
    switch (c->op) {
    case TKE_ECDH_KEY_SHARES:
        c->ok = tke_ecdh_key_shares(c->ke, c->a, c->rand, c->priv, c->ks, c->err);
        break;
    case TKE_ECDH_SERVER:
        c->shared_key = tke_ecdh_server_shared_secret(c->ke, c->a, c->rand,
                                                      c->key_share, c->ks, c->err);
        break;
    case TKE_ECDH_CLIENT:
        c->shared_key =
            tke_ecdh_client_shared_secret(c->ke, c->a, c->cpriv, c->key_share, c->err);
        break;
    default:
        break;
    }
}

CRYPTO_FIPS_TEXT static void tke_ecdh_call(TkeEcdhCall *c) {
    c->shared_key = slice_nil(TYPE_BYTE);
    fips140_without_enforcement(BURROW_FN(Func, tke_ecdh_call_run, c));
}

/* The rest of hybridKeyExchange.serverSharedSecret once the share is split, with
 * the halves in s and what it gives in a. */
CRYPTO_FIPS_TEXT static Slice
tke_hybrid_server_shared_secret(const TlsKeyExchange *ke, Alloc *a, Alloc *s,
                                IoReader rand, Slice ecdh_share_data,
                                Slice mlkem_share_data, TlsKeyShare *ks, Error *err) {
    TlsKeyShare ecdh_ks = {0};
    TkeEcdhCall c = {.op = TKE_ECDH_SERVER,
                     .ke = &ke->ecdh,
                     .a = s,
                     .rand = rand,
                     .key_share = ecdh_share_data,
                     .ks = &ecdh_ks,
                     .err = err};
    tke_ecdh_call(&c);
    Slice ecdh_shared_secret = c.shared_key;
    if (ecdh_shared_secret.p == NULL)
        return slice_nil(TYPE_BYTE);
    Slice mlkem_key_share = slice_nil(TYPE_BYTE);
    Slice mlkem_shared_secret = tke_mlkem_encapsulate(
        ke->mlkem1024, s, mlkem_share_data, &mlkem_key_share, err);
    if (mlkem_shared_secret.p == NULL)
        return slice_nil(TYPE_BYTE);
    Slice shared_key, data;
    if (ke->id == TLS_X25519MLKEM768) {
        shared_key = tke_concat(a, mlkem_shared_secret, ecdh_shared_secret);
        data = tke_concat(a, mlkem_key_share, ecdh_ks.data);
    } else {
        shared_key = tke_concat(a, ecdh_shared_secret, mlkem_shared_secret);
        data = tke_concat(a, ecdh_ks.data, mlkem_key_share);
    }
    if (shared_key.p == NULL || data.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    *ks = (TlsKeyShare){ke->id, data};
    return shared_key;
}

/* The same for hybridKeyExchange.clientSharedSecret. */
CRYPTO_FIPS_TEXT static Slice tke_hybrid_client_shared_secret(
    const TlsKeyExchange *ke, Alloc *a, Alloc *s, const TlsKeySharePrivateKeys *priv,
    Slice ecdh_share_data, Slice mlkem_share_data, Error *err) {
    TkeEcdhCall c = {.op = TKE_ECDH_CLIENT,
                     .ke = &ke->ecdh,
                     .a = s,
                     .cpriv = priv,
                     .key_share = ecdh_share_data,
                     .err = err};
    tke_ecdh_call(&c);
    Slice ecdh_shared_secret = c.shared_key;
    if (ecdh_shared_secret.p == NULL)
        return slice_nil(TYPE_BYTE);
    Slice mlkem_shared_secret =
        crypto_decapsulator_decapsulate(priv->mlkem, s, mlkem_share_data, err);
    if (mlkem_shared_secret.p == NULL)
        return slice_nil(TYPE_BYTE);
    Slice shared_key;
    if (ke->id == TLS_X25519MLKEM768)
        shared_key = tke_concat(a, mlkem_shared_secret, ecdh_shared_secret);
    else
        shared_key = tke_concat(a, ecdh_shared_secret, mlkem_shared_secret);
    if (shared_key.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return shared_key;
}

CRYPTO_FIPS_TEXT bool burrow__tls_key_exchange_key_shares(const TlsKeyExchange *ke,
                                                          Alloc *a, IoReader rand,
                                                          TlsKeySharePrivateKeys *priv,
                                                          Slice *shares, Error *err) {
    memset(priv, 0, sizeof *priv);
    Int n = ke->kind == TKE_HYBRID ? 2 : 1;
    *shares = slice_make(a, &burrow__tls_key_share_type, n, n);
    if (shares->p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return false;
    }
    TlsKeyShare *out = shares->p;

    if (ke->kind == TKE_ECDH) {
        if (tke_ecdh_key_shares(&ke->ecdh, a, rand, priv, &out[0], err))
            return true;
        *shares = slice_nil(&burrow__tls_key_share_type);
        return false;
    }

    if (ke->kind == TKE_MLKEM1024) {
        if (tke_mlkem_generate_key(true, a, priv, err)) {
            Slice data = crypto_encapsulator_bytes(
                crypto_decapsulator_encapsulator(priv->mlkem), a);
            if (data.p != NULL) {
                out[0] = (TlsKeyShare){TLS_MLKEM1024, data};
                return true;
            }
            BURROW_OUT(err, burrow_err_out_of_memory);
        }
        burrow__tls_key_share_private_keys_free(priv);
        *shares = slice_nil(&burrow__tls_key_share_type);
        return false;
    }

    TkeEcdhCall c = {.op = TKE_ECDH_KEY_SHARES,
                     .ke = &ke->ecdh,
                     .a = a,
                     .rand = rand,
                     .priv = priv,
                     .ks = &out[1],
                     .err = err};
    tke_ecdh_call(&c);
    if (c.ok && tke_mlkem_generate_key(ke->mlkem1024, a, priv, err)) {
        Slice ek =
            crypto_encapsulator_bytes(crypto_decapsulator_encapsulator(priv->mlkem), a);
        Slice share_data = slice_nil(TYPE_BYTE);
        /* For X25519MLKEM768, the ML-KEM-768 encapsulation key comes first.
         * For SecP256r1MLKEM768 and SecP384r1MLKEM1024, the ECDH share comes
         * first. See draft-ietf-tls-ecdhe-mlkem-02, Section 4.1. */
        if (ek.p != NULL)
            share_data = ke->id == TLS_X25519MLKEM768 ? tke_concat(a, ek, out[1].data)
                                                      : tke_concat(a, out[1].data, ek);
        if (share_data.p != NULL) {
            out[0] = (TlsKeyShare){ke->id, share_data};
            return true;
        }
        BURROW_OUT(err, burrow_err_out_of_memory);
    }
    burrow__tls_key_share_private_keys_free(priv);
    *shares = slice_nil(&burrow__tls_key_share_type);
    return false;
}

CRYPTO_FIPS_TEXT Slice burrow__tls_key_exchange_server_shared_secret(
    const TlsKeyExchange *ke, Alloc *a, IoReader rand, Slice client_key_share,
    TlsKeyShare *ks, Error *err) {
    if (ke->kind == TKE_ECDH)
        return tke_ecdh_server_shared_secret(&ke->ecdh, a, rand, client_key_share, ks,
                                             err);

    if (ke->kind == TKE_MLKEM1024) {
        Slice ciphertext = slice_nil(TYPE_BYTE);
        Slice shared_key =
            tke_mlkem_encapsulate(true, a, client_key_share, &ciphertext, err);
        if (shared_key.p != NULL)
            *ks = (TlsKeyShare){TLS_MLKEM1024, ciphertext};
        return shared_key;
    }

    if (client_key_share.len != ke->ecdh_element_size + ke->mlkem_public_key_size) {
        BURROW_OUT(err, tke_err_client_share_length);
        return slice_nil(TYPE_BYTE);
    }
    Slice ecdh_share_data, mlkem_share_data;
    if (ke->id == TLS_X25519MLKEM768) {
        mlkem_share_data = slice_sub(client_key_share, 0, ke->mlkem_public_key_size);
        ecdh_share_data = slice_sub(client_key_share, ke->mlkem_public_key_size,
                                    client_key_share.len);
    } else {
        ecdh_share_data = slice_sub(client_key_share, 0, ke->ecdh_element_size);
        mlkem_share_data =
            slice_sub(client_key_share, ke->ecdh_element_size, client_key_share.len);
    }

    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice shared_key =
        tke_hybrid_server_shared_secret(ke, a, arena_allocator(&scratch), rand,
                                        ecdh_share_data, mlkem_share_data, ks, err);
    arena_free(&scratch);
    return shared_key;
}

CRYPTO_FIPS_TEXT Slice burrow__tls_key_exchange_client_shared_secret(
    const TlsKeyExchange *ke, Alloc *a, const TlsKeySharePrivateKeys *priv,
    Slice server_key_share, Error *err) {
    if (ke->kind == TKE_ECDH)
        return tke_ecdh_client_shared_secret(&ke->ecdh, a, priv, server_key_share, err);

    if (ke->kind == TKE_MLKEM1024)
        return crypto_decapsulator_decapsulate(priv->mlkem, a, server_key_share, err);

    if (server_key_share.len != ke->ecdh_element_size + ke->mlkem_ciphertext_size) {
        BURROW_OUT(err, tke_err_server_share_length);
        return slice_nil(TYPE_BYTE);
    }
    Slice ecdh_share_data, mlkem_share_data;
    if (ke->id == TLS_X25519MLKEM768) {
        mlkem_share_data = slice_sub(server_key_share, 0, ke->mlkem_ciphertext_size);
        ecdh_share_data = slice_sub(server_key_share, ke->mlkem_ciphertext_size,
                                    server_key_share.len);
    } else {
        ecdh_share_data = slice_sub(server_key_share, 0, ke->ecdh_element_size);
        mlkem_share_data =
            slice_sub(server_key_share, ke->ecdh_element_size, server_key_share.len);
    }

    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice shared_key = tke_hybrid_client_shared_secret(
        ke, a, arena_allocator(&scratch), priv, ecdh_share_data, mlkem_share_data, err);
    arena_free(&scratch);
    return shared_key;
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/tls13/cast.go. */

CRYPTO_FIPS_RODATA static const Byte tls13_cast_input[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

CRYPTO_FIPS_RODATA static const Byte tls13_cast_want[32] = {
    0x78, 0x20, 0x71, 0x75, 0x52, 0xfd, 0x47, 0x67, 0xe1, 0x07, 0x5c,
    0x83, 0x74, 0x2e, 0x49, 0x43, 0xf7, 0xe3, 0x08, 0x6a, 0x2a, 0xcb,
    0x96, 0xc7, 0xa3, 0x1f, 0xe3, 0x23, 0x56, 0x6e, 0x14, 0x5b,
};

CRYPTO_FIPS_TEXT Error burrow__tls13_cast(void *arg) {
    (void)arg;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Tls13EarlySecret es =
        burrow__tls13_new_early_secret(a, sha256_new, slice_nil(TYPE_BYTE));
    Tls13HandshakeSecret hs =
        burrow__tls13_early_secret_handshake_secret(&es, a, slice_nil(TYPE_BYTE));
    Tls13MasterSecret ms = burrow__tls13_handshake_secret_master_secret(&hs, a);
    Hash transcript = sha256_new(a);
    if (es.secret.p == NULL || hs.secret.p == NULL || ms.secret.p == NULL ||
        transcript.vt == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    Slice input = slice_from((void *)(uintptr_t)tls13_cast_input, 16, 16, TYPE_BYTE);
    hash_write(transcript, input, NULL);
    Slice got =
        burrow__tls13_master_secret_resumption_master_secret(&ms, a, transcript);
    if (got.p == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    bool ok = got.len == (Int)sizeof tls13_cast_want &&
              memcmp(got.p, tls13_cast_want, sizeof tls13_cast_want) == 0;
    arena_free(&ar);
    return ok ? BURROW_NO_ERROR
              : errors_new(error_allocator(), BURROW_S("unexpected result"));
}
