/* crypto/tls: the TLS 1.3 key schedule, which is crypto/internal/fips140/tls13
 * in Go, and the parts of key_schedule.go built on it.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/hmac.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A copy of s in a, nil when s is nil or a is out of memory. */
static Slice tls13_keep(Alloc *a, Slice s) {
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
static Slice tls13_expand_label(Alloc *scratch, HashNewFunc h, Slice secret, Str label,
                                Slice context, Int length) {
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
    Slice out = hkdf_expand(scratch, h, secret, str_from_bytes(p, n), length, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    return out;
}

Slice burrow__tls13_expand_label(Alloc *a, HashNewFunc h, Slice secret, Str label,
                                 Slice context, Int length) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = tls13_keep(a, tls13_expand_label(arena_allocator(&scratch), h, secret,
                                                 label, context, length));
    arena_free(&scratch);
    return out;
}

/* extract, into scratch. A nil new_secret is zeros as long as the hash. */
static Slice tls13_extract(Alloc *scratch, HashNewFunc h, Slice new_secret,
                           Slice current_secret) {
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
    Slice out = hkdf_extract(scratch, h, new_secret, current_secret, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    return out;
}

/* deriveSecret, into scratch. A nil transcript is a fresh hash. */
static Slice tls13_derive_secret(Alloc *scratch, HashNewFunc h, Slice secret, Str label,
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
static Slice tls13_derive(Alloc *a, HashNewFunc h, Slice secret, Str label,
                          Hash transcript) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = tls13_keep(a, tls13_derive_secret(arena_allocator(&scratch), h, secret,
                                                  label, transcript));
    arena_free(&scratch);
    return out;
}

/* extract(new_secret, deriveSecret(secret, "derived", nil)), into a. */
static Slice tls13_next_stage(Alloc *a, HashNewFunc h, Slice secret, Slice new_secret) {
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

Tls13EarlySecret burrow__tls13_new_early_secret(Alloc *a, HashNewFunc h, Slice psk) {
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

Slice burrow__tls13_early_secret_resumption_binder_key(const Tls13EarlySecret *s,
                                                       Alloc *a) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("res binder"),
                        (Hash){NULL, NULL});
}

Slice burrow__tls13_early_secret_client_early_traffic_secret(const Tls13EarlySecret *s,
                                                             Alloc *a,
                                                             Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c e traffic"), transcript);
}

Tls13HandshakeSecret
burrow__tls13_early_secret_handshake_secret(const Tls13EarlySecret *s, Alloc *a,
                                            Slice shared_secret) {
    Tls13HandshakeSecret hs = {
        tls13_next_stage(a, s->hash, s->secret, shared_secret),
        s->hash,
    };
    return hs;
}

Tls13ExporterMasterSecret
burrow__tls13_early_secret_early_exporter_master_secret(const Tls13EarlySecret *s,
                                                        Alloc *a, Hash transcript) {
    Tls13ExporterMasterSecret e = {
        tls13_derive(a, s->hash, s->secret, BURROW_S("e exp master"), transcript),
        s->hash,
    };
    return e;
}

Slice burrow__tls13_handshake_secret_client_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c hs traffic"), transcript);
}

Slice burrow__tls13_handshake_secret_server_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("s hs traffic"), transcript);
}

Tls13MasterSecret
burrow__tls13_handshake_secret_master_secret(const Tls13HandshakeSecret *s, Alloc *a) {
    Tls13MasterSecret ms = {
        tls13_next_stage(a, s->hash, s->secret, slice_nil(TYPE_BYTE)),
        s->hash,
    };
    return ms;
}

Slice burrow__tls13_master_secret_client_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("c ap traffic"), transcript);
}

Slice burrow__tls13_master_secret_server_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("s ap traffic"), transcript);
}

Slice burrow__tls13_master_secret_resumption_master_secret(const Tls13MasterSecret *s,
                                                           Alloc *a, Hash transcript) {
    return tls13_derive(a, s->hash, s->secret, BURROW_S("res master"), transcript);
}

Tls13ExporterMasterSecret
burrow__tls13_master_secret_exporter_master_secret(const Tls13MasterSecret *s, Alloc *a,
                                                   Hash transcript) {
    Tls13ExporterMasterSecret e = {
        tls13_derive(a, s->hash, s->secret, BURROW_S("exp master"), transcript),
        s->hash,
    };
    return e;
}

Slice burrow__tls13_exporter_master_secret_exporter(const Tls13ExporterMasterSecret *s,
                                                    Alloc *a, Str label, Slice context,
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

Slice burrow__tls13_next_traffic_secret(const TlsSuiteTLS13 *c, Alloc *a,
                                        Slice traffic_secret) {
    HashNewFunc h = burrow__tls_suite13_hash_new(c);
    return burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("traffic upd"),
                                      slice_nil(TYPE_BYTE), crypto_hash_size(c->hash));
}

bool burrow__tls13_traffic_key(const TlsSuiteTLS13 *c, Alloc *a, Slice traffic_secret,
                               Slice *key, Slice *iv) {
    HashNewFunc h = burrow__tls_suite13_hash_new(c);
    *key = burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("key"),
                                      slice_nil(TYPE_BYTE), c->key_len);
    *iv = burrow__tls13_expand_label(a, h, traffic_secret, BURROW_S("iv"),
                                     slice_nil(TYPE_BYTE), TLS_AEAD_NONCE_LENGTH);
    return key->p != NULL && iv->p != NULL;
}

Slice burrow__tls13_finished_hash(const TlsSuiteTLS13 *c, Alloc *a, Slice base_key,
                                  Hash transcript) {
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
