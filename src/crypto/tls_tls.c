/* crypto/tls: tls.go, Server, Client, the listener, the dials and the loading
 * of a certificate and its key from PEM.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

TlsConn *tls_server(Alloc *a, NetConn conn, TlsConfig *config) {
    return burrow__tls_conn_new(a, conn, config, TLS_ROLE_SERVER,
                                burrow__tls_server_handshake);
}

TlsConn *tls_client(Alloc *a, NetConn conn, TlsConfig *config) {
    return burrow__tls_conn_new(a, conn, config, TLS_ROLE_CLIENT,
                                burrow__tls_client_handshake);
}

/* A listener implements a network listener (net.Listener) for TLS
 * connections. */
typedef struct TlsListener {
    NetListener listener;
    TlsConfig *config;

    /* Not in Go: where the listener and its Conns come from, and whether
     * tls_listener_free gives back the listener under it too. */
    Alloc *a;
    bool owns_inner;
} TlsListener;

/* Accept waits for and returns the next incoming TLS connection. The returned
 * connection is of type *Conn. */
static NetConn tls_listener_accept(void *self, Error *err) {
    TlsListener *l = (TlsListener *)self;
    Error e = BURROW_NO_ERROR;
    NetConn c = l->listener.vt->accept(l->listener.data, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (NetConn){NULL, NULL};
    }
    TlsConn *tc = tls_server(l->a, c, l->config);
    if (tc == NULL) {
        net_conn_free(c);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (NetConn){NULL, NULL};
    }
    tc->owns_conn = true;
    return tls_conn_as_net_conn(tc);
}

static Error tls_listener_close(void *self) {
    TlsListener *l = (TlsListener *)self;
    return l->listener.vt->closer.close(l->listener.data);
}

static NetAddr tls_listener_addr(void *self) {
    TlsListener *l = (TlsListener *)self;
    return l->listener.vt->addr(l->listener.data);
}

static const Type tls_listener_desc = {
    BURROW_S_INIT("listener"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(TlsListener),
    (uint16_t)_Alignof(TlsListener),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c6c6eU, /* "tlln" */
    NULL,
};

static const NetListenerVT tls_listener_vt = {
    {&tls_listener_desc, tls_listener_close},
    tls_listener_accept,
    tls_listener_addr,
};

NetListener tls_new_listener(Alloc *a, NetListener inner, TlsConfig *config) {
    TlsListener *l = BURROW_NEW(a, TlsListener);
    if (l == NULL)
        return (NetListener){NULL, NULL};
    l->listener = inner;
    l->config = config;
    l->a = a;
    return (NetListener){&tls_listener_vt, l};
}

BURROW_SENTINEL_ERROR(tls_err_listen_config,
                      "tls: neither Certificates, GetCertificate, "
                      "nor GetConfigForClient set in Config");

NetListener tls_listen(Alloc *a, Str network, Str laddr, TlsConfig *config,
                       Error *err) {
    /* If this condition changes, consider updating http.Server.ServeTLS too. */
    if (config == NULL ||
        (config->certificates.len == 0 && config->get_certificate.f == NULL &&
         config->get_config_for_client.f == NULL)) {
        BURROW_OUT(err, tls_err_listen_config);
        return (NetListener){NULL, NULL};
    }
    Error e = BURROW_NO_ERROR;
    NetListener l = net_listen(a, network, laddr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (NetListener){NULL, NULL};
    }
    NetListener tl = tls_new_listener(a, l, config);
    if (tl.vt == NULL) {
        net_listener_free(l);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return tl;
    }
    ((TlsListener *)tl.data)->owns_inner = true;
    return tl;
}

void tls_listener_free(NetListener l) {
    if (l.vt == NULL)
        return;
    TlsListener *tl = (TlsListener *)l.data;
    if (tl->owns_inner)
        net_listener_free(tl->listener);
    else
        (void)tl->listener.vt->closer.close(tl->listener.data);
    mem_free(tl->a, tl, sizeof *tl, _Alignof(TlsListener));
}

/* Not in Go: now + d on the monotonic clock, without going past either end. */
static int64_t tls_mono_add(int64_t now, int64_t d) {
    if (d > 0 && now > INT64_MAX - d)
        return INT64_MAX;
    if (d < 0 && now < INT64_MIN - d)
        return INT64_MIN;
    return now + d;
}

/* Not in Go: the contexts dial puts over the caller's, and their cancels,
 * which Go defers. */
typedef struct TlsDialCtx {
    Context timeout_ctx;
    Context deadline_ctx;
    ContextCancelFunc cancel1;
    ContextCancelFunc cancel2;
} TlsDialCtx;

static void tls_dial_ctx_end(TlsDialCtx *dc) {
    if (!BURROW_CONTEXT_IS_NIL(dc->deadline_ctx)) {
        BURROW_CALLF0(dc->cancel2);
        context_release(dc->deadline_ctx);
    }
    if (!BURROW_CONTEXT_IS_NIL(dc->timeout_ctx)) {
        BURROW_CALLF0(dc->cancel1);
        context_release(dc->timeout_ctx);
    }
}

/* Not in Go: gives back the copy of the config dial made and the name in it,
 * when it doesn't get as far as a Conn to hand them to. */
static void tls_dial_drop(Alloc *a, TlsConfig *owned, Str owned_name) {
    if (owned != NULL) {
        tls_config_release(owned);
        mem_free(a, owned, sizeof *owned, _Alignof(TlsConfig));
    }
    if (owned_name.len > 0)
        mem_free(a, (void *)(uintptr_t)owned_name.p, (size_t)owned_name.len, 1);
}

static TlsConn *tls_dial_ctx(Alloc *a, Context ctx, const NetDialer *net_dialer,
                             Str network, Str addr, TlsConfig *config, Error *err) {
    TlsDialCtx dc;
    memset(&dc, 0, sizeof dc);
    if (net_dialer->timeout != 0) {
        dc.timeout_ctx = context_with_timeout(heap_allocator(), ctx,
                                              net_dialer->timeout, &dc.cancel1);
        if (BURROW_CONTEXT_IS_NIL(dc.timeout_ctx)) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        ctx = dc.timeout_ctx;
    }

    if (!time_is_zero(net_dialer->deadline)) {
        int64_t when =
            tls_mono_add(burrow_nanotime(), time_sub(net_dialer->deadline, time_now()));
        dc.deadline_ctx =
            context_with_deadline(heap_allocator(), ctx, when, &dc.cancel2);
        if (BURROW_CONTEXT_IS_NIL(dc.deadline_ctx)) {
            tls_dial_ctx_end(&dc);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        ctx = dc.deadline_ctx;
    }

    Error e = BURROW_NO_ERROR;
    NetConn raw_conn = net_dialer_dial_context(net_dialer, a, ctx, network, addr, &e);
    if (BURROW_FAILED(e)) {
        tls_dial_ctx_end(&dc);
        BURROW_OUT(err, e);
        return NULL;
    }

    Int colon_pos = strings_last_index(addr, BURROW_S(":"));
    if (colon_pos == -1)
        colon_pos = addr.len;
    Str hostname = {addr.p, colon_pos};

    if (config == NULL)
        config = burrow__tls_default_config();
    /* If no ServerName is set, infer the ServerName from the hostname we're
     * connecting to. */
    TlsConfig *owned = NULL;
    Str owned_name = BURROW_STR_EMPTY;
    if (config->server_name.len == 0) {
        /* Make a copy to avoid polluting argument or default. */
        owned = tls_config_clone(config, a);
        owned_name = str_clone(a, hostname);
        if (owned == NULL || owned_name.len != hostname.len) {
            tls_dial_drop(a, owned, owned_name);
            net_conn_free(raw_conn);
            tls_dial_ctx_end(&dc);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        owned->server_name = owned_name;
        config = owned;
    }

    TlsConn *conn = tls_client(a, raw_conn, config);
    if (conn == NULL) {
        tls_dial_drop(a, owned, owned_name);
        net_conn_free(raw_conn);
        tls_dial_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    conn->owns_conn = true;
    conn->owned_config = owned;
    conn->owned_server_name = owned_name;
    e = tls_conn_handshake_context(conn, ctx);
    if (BURROW_FAILED(e)) {
        (void)raw_conn.vt->closer.close(raw_conn.data);
        /* Not in Go: the error belongs to conn, so it is copied out first. */
        e = error_retain(error_allocator(), e);
        tls_conn_free(conn);
        tls_dial_ctx_end(&dc);
        BURROW_OUT(err, e);
        return NULL;
    }
    tls_dial_ctx_end(&dc);
    return conn;
}

TlsConn *tls_dial_with_dialer(Alloc *a, const NetDialer *dialer, Str network, Str addr,
                              TlsConfig *config, Error *err) {
    NetDialer zero;
    if (dialer == NULL) {
        memset(&zero, 0, sizeof zero);
        dialer = &zero;
    }
    return tls_dial_ctx(a, context_background(), dialer, network, addr, config, err);
}

TlsConn *tls_dial(Alloc *a, Str network, Str addr, TlsConfig *config, Error *err) {
    return tls_dial_with_dialer(a, NULL, network, addr, config, err);
}

NetConn tls_dialer_dial(const TlsDialer *d, Alloc *a, Str network, Str addr,
                        Error *err) {
    return tls_dialer_dial_context(d, a, context_background(), network, addr, err);
}

NetConn tls_dialer_dial_context(const TlsDialer *d, Alloc *a, Context ctx, Str network,
                                Str addr, Error *err) {
    NetDialer zero;
    memset(&zero, 0, sizeof zero);
    const NetDialer *net_dialer = &zero;
    TlsConfig *config = NULL;
    if (d != NULL) {
        if (d->net_dialer != NULL)
            net_dialer = d->net_dialer;
        config = d->config;
    }
    TlsConn *c = tls_dial_ctx(a, ctx, net_dialer, network, addr, config, err);
    if (c == NULL)
        /* Don't return c (a typed nil) in an interface. */
        return (NetConn){NULL, NULL};
    return tls_conn_as_net_conn(c);
}

static Error tls_key_pair_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static TlsCertificate tls_key_pair_fail(Error *err, Error e) {
    BURROW_OUT(err, e);
    return (TlsCertificate){0};
}

TlsCertificate tls_load_x509_key_pair(Alloc *a, Str cert_file, Str key_file,
                                      Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice cert_pem_block = os_read_file(a, cert_file, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    Slice key_pem_block = os_read_file(a, key_file, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    return tls_x509_key_pair(a, cert_pem_block, key_pem_block, err);
}

/* Attempt to parse the given private key DER block. OpenSSL 0.9.8 generates
 * PKCS #1 private keys by default, while OpenSSL 1.0.0 generates PKCS #8 keys.
 * OpenSSL ecparam generates SEC1 EC private keys for ECDSA. We try all three. */
static CryptoPrivateKey tls_parse_private_key(Alloc *a, Slice der, Error *err) {
    Error pkcs8_err = BURROW_NO_ERROR; /* Return the PKCS#8 error if all parsing
                                        * attempts fail. */
    Any key = x509_parse_pkcs8_private_key(a, der, &pkcs8_err);
    if (BURROW_FAILED(pkcs8_err)) {
        Error e = BURROW_NO_ERROR;
        RsaPrivateKey *rsa = x509_parse_pkcs1_private_key(a, der, &e);
        if (!BURROW_FAILED(e)) {
            key = BURROW_ANY(TYPE_RSA_PRIVATE_KEY, rsa);
        } else {
            e = BURROW_NO_ERROR;
            EcdsaPrivateKey *ec = x509_parse_ec_private_key(a, der, &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, fmt_errorf_v("tls: failed to parse private key: %w",
                                             pkcs8_err));
                return (Any){NULL, NULL};
            }
            key = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, ec);
        }
    }
    if (key.t == TYPE_RSA_PRIVATE_KEY || key.t == TYPE_ECDSA_PRIVATE_KEY ||
        key.t == TYPE_ED25519_PRIVATE_KEY || key.t == TYPE_MLDSA_PRIVATE_KEY) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return key;
    }
    BURROW_OUT(err, tls_key_pair_error("tls: found unknown private key type in "
                                       "PKCS#8 wrapping"));
    return (Any){NULL, NULL};
}

/* Whether priv, which the switch in X509KeyPair has found to be the key type
 * that goes with pub, is the private half of pub. */
static bool tls_key_pair_matches(Alloc *a, CryptoPrivateKey priv, CryptoPublicKey pub) {
    if (pub.t == TYPE_RSA_PUBLIC_KEY)
        return rsa_public_key_equal(&((const RsaPrivateKey *)priv.data)->public_key,
                                    pub);
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY)
        return ecdsa_public_key_equal(&((const EcdsaPrivateKey *)priv.data)->public_key,
                                      pub);
    if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        Ed25519PublicKey p =
            ed25519_private_key_public(*(const Ed25519PrivateKey *)priv.data, a);
        return ed25519_public_key_equal(p, pub);
    }
    return mldsa_public_key_equal(mldsa_private_key_public_key(priv.data), pub);
}

TlsCertificate tls_x509_key_pair(Alloc *a, Slice cert_pem_block, Slice key_pem_block,
                                 Error *err) {
    TlsCertificate cert = {0};
    cert.certificate = slice_nil(TYPE_BYTES);
    Slice skipped_block_types = slice_nil(TYPE_STRING);
    for (;;) {
        PemBlock *cert_der_block = pem_decode(a, cert_pem_block, &cert_pem_block);
        if (cert_der_block == NULL)
            break;
        if (str_eq(cert_der_block->type, BURROW_S("CERTIFICATE")))
            cert.certificate =
                slice_append(a, cert.certificate, &cert_der_block->bytes, 1);
        else
            skipped_block_types =
                slice_append(a, skipped_block_types, &cert_der_block->type, 1);
    }

    if (cert.certificate.len == 0) {
        if (skipped_block_types.len == 0)
            return tls_key_pair_fail(err, tls_key_pair_error("tls: failed to find any "
                                                             "PEM data in certificate "
                                                             "input"));
        if (skipped_block_types.len == 1 &&
            strings_has_suffix(BURROW_AT(Str, skipped_block_types, 0),
                               BURROW_S("PRIVATE KEY")))
            return tls_key_pair_fail(
                err, tls_key_pair_error("tls: failed to find certificate PEM data in "
                                        "certificate input, but did find a private "
                                        "key; PEM inputs may have been switched"));
        return tls_key_pair_fail(
            err, fmt_errorf_v("tls: failed to find \"CERTIFICATE\" PEM block in "
                              "certificate input after skipping PEM blocks of the "
                              "following types: %v",
                              skipped_block_types));
    }

    skipped_block_types.len = 0;
    PemBlock *key_der_block;
    for (;;) {
        key_der_block = pem_decode(a, key_pem_block, &key_pem_block);
        if (key_der_block == NULL) {
            if (skipped_block_types.len == 0)
                return tls_key_pair_fail(err, tls_key_pair_error("tls: failed to find "
                                                                 "any PEM data in key "
                                                                 "input"));
            if (skipped_block_types.len == 1 &&
                str_eq(BURROW_AT(Str, skipped_block_types, 0), BURROW_S("CERTIFICATE")))
                return tls_key_pair_fail(
                    err,
                    tls_key_pair_error("tls: found a certificate rather than a key "
                                       "in the PEM for the private key"));
            return tls_key_pair_fail(
                err, fmt_errorf_v("tls: failed to find PEM block with type ending in "
                                  "\"PRIVATE KEY\" in key input after skipping PEM "
                                  "blocks of the following types: %v",
                                  skipped_block_types));
        }
        if (str_eq(key_der_block->type, BURROW_S("PRIVATE KEY")) ||
            strings_has_suffix(key_der_block->type, BURROW_S(" PRIVATE KEY")))
            break;
        skipped_block_types =
            slice_append(a, skipped_block_types, &key_der_block->type, 1);
    }

    /* We don't need to parse the public key for TLS, but we so do anyway
     * to check that it looks sane and matches the private key. */
    Error e = BURROW_NO_ERROR;
    X509Certificate *x509_cert =
        x509_parse_certificate(a, BURROW_AT(Slice, cert.certificate, 0), &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    cert.leaf = x509_cert;

    cert.private_key = tls_parse_private_key(a, key_der_block->bytes, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);

    CryptoPublicKey pub = x509_cert->public_key;
    const Type *want;
    if (pub.t == TYPE_RSA_PUBLIC_KEY)
        want = TYPE_RSA_PRIVATE_KEY;
    else if (pub.t == TYPE_ECDSA_PUBLIC_KEY)
        want = TYPE_ECDSA_PRIVATE_KEY;
    else if (pub.t == TYPE_ED25519_PUBLIC_KEY)
        want = TYPE_ED25519_PRIVATE_KEY;
    else if (pub.t == TYPE_MLDSA_PUBLIC_KEY)
        want = TYPE_MLDSA_PRIVATE_KEY;
    else
        return tls_key_pair_fail(
            err, tls_key_pair_error("tls: unknown public key algorithm"));
    if (cert.private_key.t != want)
        return tls_key_pair_fail(err, tls_key_pair_error("tls: private key type does "
                                                         "not match public key type"));
    if (!tls_key_pair_matches(a, cert.private_key, pub))
        return tls_key_pair_fail(err, tls_key_pair_error("tls: private key does not "
                                                         "match public key"));

    BURROW_OUT(err, BURROW_NO_ERROR);
    return cert;
}
