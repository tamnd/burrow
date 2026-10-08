/* Derived from Go's src/net/smtp/auth.go, the PLAIN and CRAM-MD5
 * mechanisms.
 *
 * Each SmtpAuth is one allocation, the struct with its strings after it, so
 * smtp_auth_free has one thing to give back.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/smtp.h"

#include "burrow/core.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/md5.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <string.h>

BURROW_SENTINEL_ERROR(sma_err_unencrypted, "unencrypted connection");
BURROW_SENTINEL_ERROR(sma_err_wrong_host, "wrong host name");
BURROW_SENTINEL_ERROR(sma_err_challenge, "unexpected server challenge");

/* The size of the allocation goes first, for smtp_auth_free. */
typedef struct SmaPlain {
    size_t size;
    Str identity, username, password;
    Str host;
} SmaPlain;

typedef struct SmaCramMD5 {
    size_t size;
    Str username, secret;
} SmaCramMD5;

/* Copies s to *p and moves *p past it. */
static Str sma_put(Byte **p, Str s) {
    Str out = str_from_bytes(*p, s.len);
    if (s.len > 0) {
        memcpy(*p, s.p, (size_t)s.len);
        *p += s.len;
    }
    return out;
}

static bool sma_is_localhost(Str name) {
    return str_eq(name, BURROW_S("localhost")) || str_eq(name, BURROW_S("127.0.0.1")) ||
           str_eq(name, BURROW_S("::1"));
}

static Str sma_plain_start(void *self, const SmtpServerInfo *server, Alloc *a,
                           Slice *to_server, Error *err) {
    const SmaPlain *pa = (const SmaPlain *)self;
    BURROW_OUT(to_server, slice_nil(TYPE_BYTE));
    /* Must have TLS, or else localhost server.
     * Note: If TLS is not true, then we can't trust ANYTHING in ServerInfo.
     * In particular, it doesn't matter if the server advertises PLAIN auth.
     * That might just be the attacker saying
     * "it's ok, you can trust me with your password." */
    if (!server->tls && !sma_is_localhost(server->name)) {
        BURROW_OUT(err, sma_err_unencrypted);
        return BURROW_S("");
    }
    if (!str_eq(server->name, pa->host)) {
        BURROW_OUT(err, sma_err_wrong_host);
        return BURROW_S("");
    }
    Int n = pa->identity.len + 1 + pa->username.len + 1 + pa->password.len;
    Slice resp = slice_make(a, TYPE_BYTE, n, n);
    if (resp.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_S("");
    }
    Byte *p = (Byte *)resp.p;
    (void)sma_put(&p, pa->identity);
    *p++ = 0;
    (void)sma_put(&p, pa->username);
    *p++ = 0;
    (void)sma_put(&p, pa->password);
    BURROW_OUT(to_server, resp);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return BURROW_S("PLAIN");
}

static Slice sma_plain_next(void *self, Slice from_server, bool more, Alloc *a,
                            Error *err) {
    (void)self;
    (void)from_server;
    (void)a;
    /* We've already sent everything. */
    BURROW_OUT(err, more ? sma_err_challenge : BURROW_NO_ERROR);
    return slice_nil(TYPE_BYTE);
}

static const SmtpAuthVT sma_plain_vt = {NULL, sma_plain_start, sma_plain_next};

SmtpAuth smtp_plain_auth(Alloc *a, Str identity, Str username, Str password, Str host) {
    size_t size = sizeof(SmaPlain) + (size_t)identity.len + (size_t)username.len +
                  (size_t)password.len + (size_t)host.len;
    SmaPlain *pa = (SmaPlain *)mem_alloc_nozero(a, size, _Alignof(SmaPlain));
    if (pa == NULL)
        return (SmtpAuth){NULL, NULL};
    Byte *p = (Byte *)(pa + 1);
    pa->size = size;
    pa->identity = sma_put(&p, identity);
    pa->username = sma_put(&p, username);
    pa->password = sma_put(&p, password);
    pa->host = sma_put(&p, host);
    return (SmtpAuth){&sma_plain_vt, pa};
}

static Str sma_cram_md5_start(void *self, const SmtpServerInfo *server, Alloc *a,
                              Slice *to_server, Error *err) {
    (void)self;
    (void)server;
    (void)a;
    BURROW_OUT(to_server, slice_nil(TYPE_BYTE));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return BURROW_S("CRAM-MD5");
}

static Slice sma_cram_md5_next(void *self, Slice from_server, bool more, Alloc *a,
                               Error *err) {
    const SmaCramMD5 *ca = (const SmaCramMD5 *)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!more)
        return slice_nil(TYPE_BYTE);
    /* The HMAC keeps nothing after the sum, so it lives in an arena of its
     * own that goes before this returns. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Slice key = slice_from((Byte *)(uintptr_t)ca->secret.p, ca->secret.len,
                           ca->secret.len, TYPE_BYTE);
    Hash d = hmac_new(t, md5_new, key);
    Slice out = slice_nil(TYPE_BYTE);
    if (d.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
    } else {
        (void)hash_write(d, from_server, NULL);
        Slice s = slice_make(t, TYPE_BYTE, 0, hash_size(d));
        Slice sum = hash_sum(t, d, s);
        out = fmt_appendf_v(a, slice_nil(TYPE_BYTE), "%s %x", ca->username, sum);
        if (out.p == NULL)
            BURROW_OUT(err, burrow_err_out_of_memory);
    }
    arena_free(&ar);
    return out;
}

static const SmtpAuthVT sma_cram_md5_vt = {NULL, sma_cram_md5_start, sma_cram_md5_next};

SmtpAuth smtp_crammd5_auth(Alloc *a, Str username, Str secret) {
    size_t size = sizeof(SmaCramMD5) + (size_t)username.len + (size_t)secret.len;
    SmaCramMD5 *ca = (SmaCramMD5 *)mem_alloc_nozero(a, size, _Alignof(SmaCramMD5));
    if (ca == NULL)
        return (SmtpAuth){NULL, NULL};
    Byte *p = (Byte *)(ca + 1);
    ca->size = size;
    ca->username = sma_put(&p, username);
    ca->secret = sma_put(&p, secret);
    return (SmtpAuth){&sma_cram_md5_vt, ca};
}

void smtp_auth_free(Alloc *a, SmtpAuth auth) {
    if (auth.vt == &sma_plain_vt)
        mem_free(a, auth.data, ((SmaPlain *)auth.data)->size, _Alignof(SmaPlain));
    else if (auth.vt == &sma_cram_md5_vt)
        mem_free(a, auth.data, ((SmaCramMD5 *)auth.data)->size, _Alignof(SmaCramMD5));
}
