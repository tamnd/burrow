/* crypto/tls: the handshake messages, from Go's handshake_messages.go.
 *
 * Go builds most messages with a cryptobyte.Builder and a continuation for
 * each length prefix. Here a small builder keeps the open prefixes on a stack
 * instead: tlsb_begin writes a placeholder, tlsb_end fills the length in, and
 * the first error sticks the way a Builder's does, with the same text. The
 * nesting of begin and end follows the nesting of Go's closures line for line.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "cryptobyte.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

const Type burrow__tls_key_share_type = {
    {(const Byte *)"keyShare", 8},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsKeyShare),
    (uint16_t)_Alignof(TlsKeyShare),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow__tls_psk_identity_type = {
    {(const Byte *)"pskIdentity", 11},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsPskIdentity),
    (uint16_t)_Alignof(TlsPskIdentity),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------- the builder */

/* The deepest any message nests its length prefixes. */
#define TLSB_MAX_DEPTH 8

typedef struct TlsBuilder {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    Int start[TLSB_MAX_DEPTH]; /* where each open prefix's contents begin */
    int len_len[TLSB_MAX_DEPTH];
    int depth;
    bool failed;
    Error err;
} TlsBuilder;

static void tlsb_init(TlsBuilder *b, Alloc *a) {
    memset(b, 0, sizeof(*b));
    b->a = a;
    b->err = BURROW_NO_ERROR;
}

static void tlsb_fail(TlsBuilder *b, Error err) {
    if (!b->failed) {
        b->failed = true;
        b->err = err;
    }
}

/* Room for n more bytes. False when the builder has already failed or a is out
 * of memory. */
static bool tlsb_room(TlsBuilder *b, Int n) {
    if (b->failed)
        return false;
    if (b->cap - b->len >= n)
        return true;
    Int ncap = b->cap < 64 ? 64 : b->cap;
    while (ncap - b->len < n)
        ncap *= 2;
    Byte *np = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)ncap, 1);
    if (np == NULL) {
        tlsb_fail(b, burrow_err_out_of_memory);
        return false;
    }
    b->p = np;
    b->cap = ncap;
    return true;
}

static void tlsb_add(TlsBuilder *b, const void *v, Int n) {
    if (n <= 0 || !tlsb_room(b, n))
        return;
    memcpy(b->p + b->len, v, (size_t)n);
    b->len += n;
}

static void tlsb_u8(TlsBuilder *b, uint8_t v) {
    tlsb_add(b, &v, 1);
}

static void tlsb_u16(TlsBuilder *b, uint16_t v) {
    Byte x[2] = {(Byte)(v >> 8), (Byte)v};
    tlsb_add(b, x, 2);
}

static void tlsb_u32(TlsBuilder *b, uint32_t v) {
    Byte x[4] = {(Byte)(v >> 24), (Byte)(v >> 16), (Byte)(v >> 8), (Byte)v};
    tlsb_add(b, x, 4);
}

static void tlsb_bytes(TlsBuilder *b, Slice v) {
    tlsb_add(b, v.p, v.len);
}

static void tlsb_str(TlsBuilder *b, Str v) {
    tlsb_add(b, v.p, v.len);
}

/* Each uint16 in v, a []uint16. */
static void tlsb_u16s(TlsBuilder *b, Slice v) {
    const uint16_t *p = (const uint16_t *)v.p;
    for (Int i = 0; i < v.len; i++)
        tlsb_u16(b, p[i]);
}

/* addBytesWithLength appends a sequence of bytes to the builder. If the length
 * of the sequence is not the value specified, it produces an error. */
static void tlsb_bytes_with_length(TlsBuilder *b, Slice v, Int n) {
    if (v.len != n) {
        tlsb_fail(b,
                  fmt_errorf_v("invalid value length: expected %d, got %d", n, v.len));
        return;
    }
    tlsb_bytes(b, v);
}

/* Opens a length prefix of len_len bytes. */
static void tlsb_begin(TlsBuilder *b, int len_len) {
    if (b->depth == TLSB_MAX_DEPTH)
        panic_str(BURROW_S("tls: internal error: length prefixes nested too deep"));
    static const Byte zero[4] = {0};
    tlsb_add(b, zero, len_len);
    b->start[b->depth] = b->len;
    b->len_len[b->depth] = len_len;
    b->depth++;
}

/* Closes the innermost length prefix and writes its length. */
static void tlsb_end(TlsBuilder *b) {
    b->depth--;
    if (b->failed)
        return;
    Int start = b->start[b->depth];
    int len_len = b->len_len[b->depth];
    Int length = b->len - start;
    uint64_t l = (uint64_t)length;
    for (int i = len_len - 1; i >= 0; i--) {
        b->p[start - len_len + i] = (Byte)l;
        l >>= 8;
    }
    if (l != 0)
        tlsb_fail(b, fmt_errorf_v("cryptobyte: pending child length %d exceeds "
                                  "%d-byte length prefix",
                                  length, (Int)len_len));
}

/* The bytes, from a, or a nil slice and the error. */
static Slice tlsb_finish(TlsBuilder *b, Error *err) {
    if (b->failed) {
        mem_free(b->a, b->p, (size_t)b->cap, 1);
        BURROW_OUT(err, b->err);
        return slice_nil(TYPE_BYTE);
    }
    Slice out = {b->p, b->len, b->cap, TYPE_BYTE};
    return out;
}

/* make([]byte, n) for the messages Go lays out by hand. */
static Slice tls_make_bytes(Alloc *a, Int n, Error *err) {
    Slice x = slice_make(a, TYPE_BYTE, n, n);
    if (x.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return x;
}

/* ------------------------------------------------------------- the readers */

static bool tlsr_skip(CryptobyteString *s, Int n) {
    return cryptobyte_string_skip(s, n);
}

static bool tlsr_u8(CryptobyteString *s, uint8_t *out) {
    return cryptobyte_string_read_uint8(s, out);
}

static bool tlsr_u16(CryptobyteString *s, uint16_t *out) {
    return cryptobyte_string_read_uint16(s, out);
}

static bool tlsr_u32(CryptobyteString *s, uint32_t *out) {
    return cryptobyte_string_read_uint32(s, out);
}

/* readUint8LengthPrefixed, readUint16LengthPrefixed and
 * readUint24LengthPrefixed, which are the same thing here since a []byte and a
 * cryptobyte.String are both a Slice. */
static bool tlsr_lp8(CryptobyteString *s, Slice *out) {
    return cryptobyte_string_read_uint8_length_prefixed(s, out);
}

static bool tlsr_lp16(CryptobyteString *s, Slice *out) {
    return cryptobyte_string_read_uint16_length_prefixed(s, out);
}

static bool tlsr_lp24(CryptobyteString *s, Slice *out) {
    return cryptobyte_string_read_uint24_length_prefixed(s, out);
}

static bool tlsr_bytes(CryptobyteString *s, Slice *out, Int n) {
    return cryptobyte_string_read_bytes(s, out, n);
}

static bool tlsr_empty(CryptobyteString s) {
    return cryptobyte_string_empty(s);
}

/* append(*s, *v), false when a is out of memory. */
static bool tls_push(Alloc *a, Slice *s, const void *v) {
    Slice r = slice_append(a, *s, v, 1);
    if (r.p == NULL)
        return false;
    *s = r;
    return true;
}

static bool tls_push_u16(Alloc *a, Slice *s, uint16_t v) {
    return tls_push(a, s, &v);
}

/* Reads a non-empty list of uint16s with a uint16 length in front and appends
 * them to *out. */
static bool tlsr_u16_list(CryptobyteString *ext_data, Alloc *a, Slice *out) {
    CryptobyteString list;
    if (!tlsr_lp16(ext_data, &list) || tlsr_empty(list))
        return false;
    while (!tlsr_empty(list)) {
        uint16_t v;
        if (!tlsr_u16(&list, &v) || !tls_push_u16(a, out, v))
            return false;
    }
    return true;
}

/* string(b), a copy from a. */
static bool tls_string(Alloc *a, Slice b, Str *out) {
    if (b.len == 0) {
        *out = BURROW_STR_EMPTY;
        return true;
    }
    Str s = str_clone(a, str_from_bytes((const Byte *)b.p, b.len));
    if (s.p == NULL)
        return false;
    *out = s;
    return true;
}

/* make([]byte, len(*s)) and s.CopyBytes into it. */
static bool tlsr_copy_all(CryptobyteString *s, Alloc *a, Slice *out) {
    Slice x = slice_make(a, TYPE_BYTE, s->len, s->len);
    if (x.p == NULL)
        return false;
    *out = x;
    return cryptobyte_string_copy_bytes(s, x);
}

/* seenExts, which Go keeps in a map. */
typedef struct TlsSeenExts {
    uint64_t bits[65536 / 64];
} TlsSeenExts;

/* Whether ext was seen before, marking it seen. */
static bool tls_seen(TlsSeenExts *seen, uint16_t ext) {
    uint64_t bit = (uint64_t)1 << (ext & 63);
    if ((seen->bits[ext >> 6] & bit) != 0)
        return true;
    seen->bits[ext >> 6] |= bit;
    return false;
}

static bool tls_has_suffix_dot(Str s) {
    return s.len > 0 && s.p[s.len - 1] == '.';
}

/* -------------------------------------------------------------- ClientHello */

TlsClientHelloMsg burrow__tls_client_hello_msg_zero(void) {
    TlsClientHelloMsg m;
    memset(&m, 0, sizeof(m));
    m.original = slice_nil(TYPE_BYTE);
    m.random = slice_nil(TYPE_BYTE);
    m.session_id = slice_nil(TYPE_BYTE);
    m.cipher_suites = slice_nil(TYPE_UINT16);
    m.compression_methods = slice_nil(TYPE_BYTE);
    m.server_name = BURROW_STR_EMPTY;
    m.supported_curves = slice_nil(TYPE_UINT16);
    m.supported_points = slice_nil(TYPE_BYTE);
    m.session_ticket = slice_nil(TYPE_BYTE);
    m.supported_signature_algorithms = slice_nil(TYPE_UINT16);
    m.supported_signature_algorithms_cert = slice_nil(TYPE_UINT16);
    m.secure_renegotiation = slice_nil(TYPE_BYTE);
    m.alpn_protocols = slice_nil(TYPE_STRING);
    m.supported_versions = slice_nil(TYPE_UINT16);
    m.cookie = slice_nil(TYPE_BYTE);
    m.key_shares = slice_nil(&burrow__tls_key_share_type);
    m.psk_modes = slice_nil(TYPE_BYTE);
    m.psk_identities = slice_nil(&burrow__tls_psk_identity_type);
    m.psk_binders = slice_nil(TYPE_BYTES);
    m.quic_transport_parameters = slice_nil(TYPE_BYTE);
    m.encrypted_client_hello = slice_nil(TYPE_BYTE);
    m.extensions = slice_nil(TYPE_UINT16);
    return m;
}

/* An extension holding a list of uint16s with a uint16 length in front. */
static void tlsb_u16_list_ext(TlsBuilder *b, uint16_t ext, Slice list) {
    tlsb_u16(b, ext);
    tlsb_begin(b, 2);
    tlsb_begin(b, 2);
    tlsb_u16s(b, list);
    tlsb_end(b);
    tlsb_end(b);
}

static bool tls_is_inner_ech_ext(Slice ech) {
    return ech.len == 1 && ((const Byte *)ech.p)[0] == TLS_INNER_ECH_EXT;
}

Slice burrow__tls_client_hello_msg_marshal_msg(const TlsClientHelloMsg *m, Alloc *a,
                                               bool ech_inner, Error *err) {
    TlsBuilder exts;
    tlsb_init(&exts, a);
    if (m->server_name.len > 0) {
        /* RFC 6066, Section 3 */
        tlsb_u16(&exts, TLS_EXTENSION_SERVER_NAME);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 2);
        tlsb_u8(&exts, 0); /* name_type = host_name */
        tlsb_begin(&exts, 2);
        tlsb_str(&exts, m->server_name);
        tlsb_end(&exts);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->supported_points.len > 0 && !ech_inner) {
        /* RFC 4492, Section 5.1.2 */
        tlsb_u16(&exts, TLS_EXTENSION_SUPPORTED_POINTS);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 1);
        tlsb_bytes(&exts, m->supported_points);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->ticket_supported && !ech_inner) {
        /* RFC 5077, Section 3.2 */
        tlsb_u16(&exts, TLS_EXTENSION_SESSION_TICKET);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->session_ticket);
        tlsb_end(&exts);
    }
    if (m->secure_renegotiation_supported && !ech_inner) {
        /* RFC 5746, Section 3.2 */
        tlsb_u16(&exts, TLS_EXTENSION_RENEGOTIATION_INFO);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 1);
        tlsb_bytes(&exts, m->secure_renegotiation);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->extended_master_secret && !ech_inner) {
        /* RFC 7627 */
        tlsb_u16(&exts, TLS_EXTENSION_EXTENDED_MASTER_SECRET);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->scts) {
        /* RFC 6962, Section 3.3.1 */
        tlsb_u16(&exts, TLS_EXTENSION_SCT);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->early_data) {
        /* RFC 8446, Section 4.2.10 */
        tlsb_u16(&exts, TLS_EXTENSION_EARLY_DATA);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->quic_transport_parameters.p != NULL) {
        /* marshal zero-length parameters when present */
        /* RFC 9001, Section 8.2 */
        tlsb_u16(&exts, TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->quic_transport_parameters);
        tlsb_end(&exts);
    }
    if (m->encrypted_client_hello.len > 0) {
        tlsb_u16(&exts, TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->encrypted_client_hello);
        tlsb_end(&exts);
    }
    /* Note that any extension that can be compressed during ECH must be
     * contiguous. If any additional extensions are to be compressed they must
     * be added to the following block, so that they can be properly
     * decompressed on the other side. */
    uint16_t ech_outer_exts[9];
    int n_outer = 0;
    if (m->ocsp_stapling) {
        /* RFC 4366, Section 3.6 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_STATUS_REQUEST;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_STATUS_REQUEST);
            tlsb_begin(&exts, 2);
            tlsb_u8(&exts, 1);  /* status_type = ocsp */
            tlsb_u16(&exts, 0); /* empty responder_id_list */
            tlsb_u16(&exts, 0); /* empty request_extensions */
            tlsb_end(&exts);
        }
    }
    if (m->supported_curves.len > 0) {
        /* RFC 4492, sections 5.1.1 and RFC 8446, Section 4.2.7 */
        if (ech_inner)
            ech_outer_exts[n_outer++] = TLS_EXTENSION_SUPPORTED_CURVES;
        else
            tlsb_u16_list_ext(&exts, TLS_EXTENSION_SUPPORTED_CURVES,
                              m->supported_curves);
    }
    if (m->supported_signature_algorithms.len > 0) {
        /* RFC 5246, Section 7.4.1.4.1 */
        if (ech_inner)
            ech_outer_exts[n_outer++] = TLS_EXTENSION_SIGNATURE_ALGORITHMS;
        else
            tlsb_u16_list_ext(&exts, TLS_EXTENSION_SIGNATURE_ALGORITHMS,
                              m->supported_signature_algorithms);
    }
    if (m->supported_signature_algorithms_cert.len > 0) {
        /* RFC 8446, Section 4.2.3 */
        if (ech_inner)
            ech_outer_exts[n_outer++] = TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT;
        else
            tlsb_u16_list_ext(&exts, TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT,
                              m->supported_signature_algorithms_cert);
    }
    if (m->alpn_protocols.len > 0) {
        /* RFC 7301, Section 3.1 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_ALPN;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_ALPN);
            tlsb_begin(&exts, 2);
            tlsb_begin(&exts, 2);
            for (Int i = 0; i < m->alpn_protocols.len; i++) {
                tlsb_begin(&exts, 1);
                tlsb_str(&exts, ((const Str *)m->alpn_protocols.p)[i]);
                tlsb_end(&exts);
            }
            tlsb_end(&exts);
            tlsb_end(&exts);
        }
    }
    if (m->supported_versions.len > 0) {
        /* RFC 8446, Section 4.2.1 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_SUPPORTED_VERSIONS;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_SUPPORTED_VERSIONS);
            tlsb_begin(&exts, 2);
            tlsb_begin(&exts, 1);
            tlsb_u16s(&exts, m->supported_versions);
            tlsb_end(&exts);
            tlsb_end(&exts);
        }
    }
    if (m->cookie.len > 0) {
        /* RFC 8446, Section 4.2.2 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_COOKIE;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_COOKIE);
            tlsb_begin(&exts, 2);
            tlsb_begin(&exts, 2);
            tlsb_bytes(&exts, m->cookie);
            tlsb_end(&exts);
            tlsb_end(&exts);
        }
    }
    if (m->key_shares.len > 0) {
        /* RFC 8446, Section 4.2.8 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_KEY_SHARE;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_KEY_SHARE);
            tlsb_begin(&exts, 2);
            tlsb_begin(&exts, 2);
            for (Int i = 0; i < m->key_shares.len; i++) {
                const TlsKeyShare *ks = &((const TlsKeyShare *)m->key_shares.p)[i];
                tlsb_u16(&exts, ks->group);
                tlsb_begin(&exts, 2);
                tlsb_bytes(&exts, ks->data);
                tlsb_end(&exts);
            }
            tlsb_end(&exts);
            tlsb_end(&exts);
        }
    }
    if (m->psk_modes.len > 0) {
        /* RFC 8446, Section 4.2.9 */
        if (ech_inner) {
            ech_outer_exts[n_outer++] = TLS_EXTENSION_PSK_MODES;
        } else {
            tlsb_u16(&exts, TLS_EXTENSION_PSK_MODES);
            tlsb_begin(&exts, 2);
            tlsb_begin(&exts, 1);
            tlsb_bytes(&exts, m->psk_modes);
            tlsb_end(&exts);
            tlsb_end(&exts);
        }
    }
    if (n_outer > 0 && ech_inner) {
        tlsb_u16(&exts, TLS_EXTENSION_ECH_OUTER_EXTENSIONS);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 1);
        for (int i = 0; i < n_outer; i++)
            tlsb_u16(&exts, ech_outer_exts[i]);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    /* pre_shared_key must be the last extension */
    if (m->psk_identities.len > 0 &&
        (ech_inner || m->encrypted_client_hello.len == 0 ||
         tls_is_inner_ech_ext(m->encrypted_client_hello))) {
        /* RFC 8446, Section 4.2.11 */
        tlsb_u16(&exts, TLS_EXTENSION_PRE_SHARED_KEY);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 2);
        for (Int i = 0; i < m->psk_identities.len; i++) {
            const TlsPskIdentity *psk =
                &((const TlsPskIdentity *)m->psk_identities.p)[i];
            tlsb_begin(&exts, 2);
            tlsb_bytes(&exts, psk->label);
            tlsb_end(&exts);
            tlsb_u32(&exts, psk->obfuscated_ticket_age);
        }
        tlsb_end(&exts);
        tlsb_begin(&exts, 2);
        for (Int i = 0; i < m->psk_binders.len; i++) {
            tlsb_begin(&exts, 1);
            tlsb_bytes(&exts, ((const Slice *)m->psk_binders.p)[i]);
            tlsb_end(&exts);
        }
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    Slice ext_bytes = tlsb_finish(&exts, err);
    if (exts.failed)
        return ext_bytes;

    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_CLIENT_HELLO);
    tlsb_begin(&b, 3);
    tlsb_u16(&b, m->vers);
    tlsb_bytes_with_length(&b, m->random, 32);
    tlsb_begin(&b, 1);
    if (!ech_inner)
        tlsb_bytes(&b, m->session_id);
    tlsb_end(&b);
    tlsb_begin(&b, 2);
    tlsb_u16s(&b, m->cipher_suites);
    tlsb_end(&b);
    tlsb_begin(&b, 1);
    tlsb_bytes(&b, m->compression_methods);
    tlsb_end(&b);

    if (ext_bytes.len > 0) {
        tlsb_begin(&b, 2);
        tlsb_bytes(&b, ext_bytes);
        tlsb_end(&b);
    }
    tlsb_end(&b);
    mem_free(a, ext_bytes.p, (size_t)ext_bytes.cap, 1);

    return tlsb_finish(&b, err);
}

Slice burrow__tls_client_hello_msg_marshal(const TlsClientHelloMsg *m, Alloc *a,
                                           Error *err) {
    return burrow__tls_client_hello_msg_marshal_msg(m, a, false, err);
}

Slice burrow__tls_client_hello_msg_marshal_without_binders(const TlsClientHelloMsg *m,
                                                           Alloc *a, Error *err) {
    Int binders_len = 2; /* uint16 length prefix */
    for (Int i = 0; i < m->psk_binders.len; i++) {
        binders_len += 1; /* uint8 length prefix */
        binders_len += ((const Slice *)m->psk_binders.p)[i].len;
    }

    Slice full_message;
    if (m->original.p != NULL) {
        full_message = m->original;
    } else {
        Error e = BURROW_NO_ERROR;
        full_message = burrow__tls_client_hello_msg_marshal(m, a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
    }
    return slice_sub(full_message, 0, full_message.len - binders_len);
}

bool burrow__tls_client_hello_msg_update_binders(TlsClientHelloMsg *m,
                                                 Slice psk_binders, Error *err) {
    static const char mismatch[] = "tls: internal error: pskBinders length mismatch";
    if (psk_binders.len != m->psk_binders.len) {
        BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(mismatch)));
        return false;
    }
    for (Int i = 0; i < m->psk_binders.len; i++) {
        if (((const Slice *)psk_binders.p)[i].len !=
            ((const Slice *)m->psk_binders.p)[i].len) {
            BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(mismatch)));
            return false;
        }
    }
    m->psk_binders = psk_binders;
    return true;
}

/* One ClientHello extension. False when it does not parse. */
static bool tls_client_hello_extension(TlsClientHelloMsg *m, Alloc *a,
                                       CryptobyteString extensions, uint16_t extension,
                                       CryptobyteString *ext_data, bool *known) {
    *known = true;
    switch (extension) {
    case TLS_EXTENSION_SERVER_NAME: {
        /* RFC 6066, Section 3 */
        CryptobyteString name_list;
        if (!tlsr_lp16(ext_data, &name_list) || tlsr_empty(name_list))
            return false;
        while (!tlsr_empty(name_list)) {
            uint8_t name_type;
            CryptobyteString server_name;
            if (!tlsr_u8(&name_list, &name_type) ||
                !tlsr_lp16(&name_list, &server_name) || tlsr_empty(server_name))
                return false;
            if (name_type != 0)
                continue;
            if (m->server_name.len != 0) {
                /* Multiple names of the same name_type are prohibited. */
                return false;
            }
            if (!tls_string(a, server_name, &m->server_name))
                return false;
            /* An SNI value may not include a trailing dot. */
            if (tls_has_suffix_dot(m->server_name))
                return false;
        }
        return true;
    }
    case TLS_EXTENSION_STATUS_REQUEST: {
        /* RFC 4366, Section 3.6 */
        uint8_t status_type;
        CryptobyteString ignored;
        if (!tlsr_u8(ext_data, &status_type) || !tlsr_lp16(ext_data, &ignored) ||
            !tlsr_lp16(ext_data, &ignored))
            return false;
        m->ocsp_stapling = status_type == TLS_STATUS_TYPE_OCSP;
        return true;
    }
    case TLS_EXTENSION_SUPPORTED_CURVES:
        /* RFC 4492, sections 5.1.1 and RFC 8446, Section 4.2.7 */
        return tlsr_u16_list(ext_data, a, &m->supported_curves);
    case TLS_EXTENSION_SUPPORTED_POINTS:
        /* RFC 4492, Section 5.1.2 */
        return tlsr_lp8(ext_data, &m->supported_points) && m->supported_points.len != 0;
    case TLS_EXTENSION_SESSION_TICKET:
        /* RFC 5077, Section 3.2 */
        m->ticket_supported = true;
        (void)tlsr_bytes(ext_data, &m->session_ticket, ext_data->len);
        return true;
    case TLS_EXTENSION_SIGNATURE_ALGORITHMS:
        /* RFC 5246, Section 7.4.1.4.1 */
        return tlsr_u16_list(ext_data, a, &m->supported_signature_algorithms);
    case TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT:
        /* RFC 8446, Section 4.2.3 */
        return tlsr_u16_list(ext_data, a, &m->supported_signature_algorithms_cert);
    case TLS_EXTENSION_RENEGOTIATION_INFO:
        /* RFC 5746, Section 3.2 */
        if (!tlsr_lp8(ext_data, &m->secure_renegotiation))
            return false;
        m->secure_renegotiation_supported = true;
        return true;
    case TLS_EXTENSION_EXTENDED_MASTER_SECRET:
        /* RFC 7627 */
        m->extended_master_secret = true;
        return true;
    case TLS_EXTENSION_ALPN: {
        /* RFC 7301, Section 3.1 */
        CryptobyteString proto_list;
        if (!tlsr_lp16(ext_data, &proto_list) || tlsr_empty(proto_list))
            return false;
        while (!tlsr_empty(proto_list)) {
            CryptobyteString proto;
            Str s;
            if (!tlsr_lp8(&proto_list, &proto) || tlsr_empty(proto) ||
                !tls_string(a, proto, &s) || !tls_push(a, &m->alpn_protocols, &s))
                return false;
        }
        return true;
    }
    case TLS_EXTENSION_SCT:
        /* RFC 6962, Section 3.3.1 */
        m->scts = true;
        return true;
    case TLS_EXTENSION_SUPPORTED_VERSIONS: {
        /* RFC 8446, Section 4.2.1 */
        CryptobyteString vers_list;
        if (!tlsr_lp8(ext_data, &vers_list) || tlsr_empty(vers_list))
            return false;
        while (!tlsr_empty(vers_list)) {
            uint16_t vers;
            if (!tlsr_u16(&vers_list, &vers) ||
                !tls_push_u16(a, &m->supported_versions, vers))
                return false;
        }
        return true;
    }
    case TLS_EXTENSION_COOKIE:
        /* RFC 8446, Section 4.2.2 */
        return tlsr_lp16(ext_data, &m->cookie) && m->cookie.len != 0;
    case TLS_EXTENSION_KEY_SHARE: {
        /* RFC 8446, Section 4.2.8 */
        CryptobyteString client_shares;
        if (!tlsr_lp16(ext_data, &client_shares))
            return false;
        while (!tlsr_empty(client_shares)) {
            TlsKeyShare ks;
            if (!tlsr_u16(&client_shares, &ks.group) ||
                !tlsr_lp16(&client_shares, &ks.data) || ks.data.len == 0 ||
                !tls_push(a, &m->key_shares, &ks))
                return false;
        }
        return true;
    }
    case TLS_EXTENSION_EARLY_DATA:
        /* RFC 8446, Section 4.2.10 */
        m->early_data = true;
        return true;
    case TLS_EXTENSION_PSK_MODES:
        /* RFC 8446, Section 4.2.9 */
        return tlsr_lp8(ext_data, &m->psk_modes);
    case TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS:
        return tlsr_copy_all(ext_data, a, &m->quic_transport_parameters);
    case TLS_EXTENSION_PRE_SHARED_KEY: {
        /* RFC 8446, Section 4.2.11 */
        if (!tlsr_empty(extensions))
            return false; /* pre_shared_key must be the last extension */
        CryptobyteString identities;
        if (!tlsr_lp16(ext_data, &identities) || tlsr_empty(identities))
            return false;
        while (!tlsr_empty(identities)) {
            TlsPskIdentity psk;
            if (!tlsr_lp16(&identities, &psk.label) ||
                !tlsr_u32(&identities, &psk.obfuscated_ticket_age) ||
                psk.label.len == 0 || !tls_push(a, &m->psk_identities, &psk))
                return false;
        }
        CryptobyteString binders;
        if (!tlsr_lp16(ext_data, &binders) || tlsr_empty(binders))
            return false;
        while (!tlsr_empty(binders)) {
            Slice binder;
            if (!tlsr_lp8(&binders, &binder) || binder.len == 0 ||
                !tls_push(a, &m->psk_binders, &binder))
                return false;
        }
        return true;
    }
    case TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO:
        return tlsr_bytes(ext_data, &m->encrypted_client_hello, ext_data->len);
    default:
        /* Ignore unknown extensions. */
        *known = false;
        return true;
    }
}

bool burrow__tls_client_hello_msg_unmarshal(TlsClientHelloMsg *m, Alloc *a,
                                            Slice data) {
    *m = burrow__tls_client_hello_msg_zero();
    m->original = data;
    CryptobyteString s = data;

    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_u16(&s, &m->vers) || !tlsr_bytes(&s, &m->random, 32) ||
        !tlsr_lp8(&s, &m->session_id))
        return false;

    CryptobyteString cipher_suites;
    if (!tlsr_lp16(&s, &cipher_suites))
        return false;
    m->cipher_suites = slice_make(a, TYPE_UINT16, 0, 0);
    m->secure_renegotiation_supported = false;
    while (!tlsr_empty(cipher_suites)) {
        uint16_t suite;
        if (!tlsr_u16(&cipher_suites, &suite))
            return false;
        if (suite == TLS_SCSV_RENEGOTIATION)
            m->secure_renegotiation_supported = true;
        if (!tls_push_u16(a, &m->cipher_suites, suite))
            return false;
    }

    if (!tlsr_lp8(&s, &m->compression_methods))
        return false;

    if (tlsr_empty(s)) {
        /* ClientHello is optionally followed by extension data */
        return true;
    }

    CryptobyteString extensions;
    if (!tlsr_lp16(&s, &extensions) || !tlsr_empty(s))
        return false;

    TlsSeenExts seen_exts;
    memset(&seen_exts, 0, sizeof(seen_exts));
    while (!tlsr_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!tlsr_u16(&extensions, &extension) || !tlsr_lp16(&extensions, &ext_data))
            return false;

        if (tls_seen(&seen_exts, extension))
            return false;
        if (!tls_push_u16(a, &m->extensions, extension))
            return false;

        bool known;
        if (!tls_client_hello_extension(m, a, extensions, extension, &ext_data, &known))
            return false;
        if (known && !tlsr_empty(ext_data))
            return false;
    }

    return true;
}

/* slices.Clone(s) into *out, from a. */
static bool tls_clone(Alloc *a, Slice s, Slice *out) {
    if (s.p == NULL) {
        *out = s;
        return true;
    }
    Slice c = slice_make(a, s.elem, s.len, s.len);
    if (c.p == NULL)
        return false;
    slice_copy(c, s);
    *out = c;
    return true;
}

bool burrow__tls_client_hello_msg_clone(const TlsClientHelloMsg *m, Alloc *a,
                                        TlsClientHelloMsg *out) {
    TlsClientHelloMsg c = burrow__tls_client_hello_msg_zero();
    c.vers = m->vers;
    c.server_name = m->server_name;
    c.ocsp_stapling = m->ocsp_stapling;
    c.ticket_supported = m->ticket_supported;
    c.secure_renegotiation_supported = m->secure_renegotiation_supported;
    c.extended_master_secret = m->extended_master_secret;
    c.scts = m->scts;
    c.early_data = m->early_data;
    if (!tls_clone(a, m->original, &c.original) ||
        !tls_clone(a, m->random, &c.random) ||
        !tls_clone(a, m->session_id, &c.session_id) ||
        !tls_clone(a, m->cipher_suites, &c.cipher_suites) ||
        !tls_clone(a, m->compression_methods, &c.compression_methods) ||
        !tls_clone(a, m->supported_curves, &c.supported_curves) ||
        !tls_clone(a, m->supported_points, &c.supported_points) ||
        !tls_clone(a, m->session_ticket, &c.session_ticket) ||
        !tls_clone(a, m->supported_signature_algorithms,
                   &c.supported_signature_algorithms) ||
        !tls_clone(a, m->supported_signature_algorithms_cert,
                   &c.supported_signature_algorithms_cert) ||
        !tls_clone(a, m->secure_renegotiation, &c.secure_renegotiation) ||
        !tls_clone(a, m->alpn_protocols, &c.alpn_protocols) ||
        !tls_clone(a, m->supported_versions, &c.supported_versions) ||
        !tls_clone(a, m->cookie, &c.cookie) ||
        !tls_clone(a, m->key_shares, &c.key_shares) ||
        !tls_clone(a, m->psk_modes, &c.psk_modes) ||
        !tls_clone(a, m->psk_identities, &c.psk_identities) ||
        !tls_clone(a, m->psk_binders, &c.psk_binders) ||
        !tls_clone(a, m->quic_transport_parameters, &c.quic_transport_parameters) ||
        !tls_clone(a, m->encrypted_client_hello, &c.encrypted_client_hello))
        return false;
    *out = c;
    return true;
}

/* -------------------------------------------------------------- ServerHello */

TlsServerHelloMsg burrow__tls_server_hello_msg_zero(void) {
    TlsServerHelloMsg m;
    memset(&m, 0, sizeof(m));
    m.original = slice_nil(TYPE_BYTE);
    m.random = slice_nil(TYPE_BYTE);
    m.session_id = slice_nil(TYPE_BYTE);
    m.secure_renegotiation = slice_nil(TYPE_BYTE);
    m.alpn_protocol = BURROW_STR_EMPTY;
    m.scts = slice_nil(TYPE_BYTES);
    m.server_share.data = slice_nil(TYPE_BYTE);
    m.supported_points = slice_nil(TYPE_BYTE);
    m.encrypted_client_hello = slice_nil(TYPE_BYTE);
    m.cookie = slice_nil(TYPE_BYTE);
    return m;
}

/* An ALPN extension holding the one protocol p. */
static void tlsb_alpn_ext(TlsBuilder *b, Str p) {
    tlsb_u16(b, TLS_EXTENSION_ALPN);
    tlsb_begin(b, 2);
    tlsb_begin(b, 2);
    tlsb_begin(b, 1);
    tlsb_str(b, p);
    tlsb_end(b);
    tlsb_end(b);
    tlsb_end(b);
}

Slice burrow__tls_server_hello_msg_marshal(const TlsServerHelloMsg *m, Alloc *a,
                                           Error *err) {
    TlsBuilder exts;
    tlsb_init(&exts, a);
    if (m->ocsp_stapling) {
        tlsb_u16(&exts, TLS_EXTENSION_STATUS_REQUEST);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->ticket_supported) {
        tlsb_u16(&exts, TLS_EXTENSION_SESSION_TICKET);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->secure_renegotiation_supported) {
        tlsb_u16(&exts, TLS_EXTENSION_RENEGOTIATION_INFO);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 1);
        tlsb_bytes(&exts, m->secure_renegotiation);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->extended_master_secret) {
        tlsb_u16(&exts, TLS_EXTENSION_EXTENDED_MASTER_SECRET);
        tlsb_u16(&exts, 0); /* empty extension_data */
    }
    if (m->alpn_protocol.len > 0)
        tlsb_alpn_ext(&exts, m->alpn_protocol);
    if (m->scts.len > 0) {
        tlsb_u16(&exts, TLS_EXTENSION_SCT);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 2);
        for (Int i = 0; i < m->scts.len; i++) {
            tlsb_begin(&exts, 2);
            tlsb_bytes(&exts, ((const Slice *)m->scts.p)[i]);
            tlsb_end(&exts);
        }
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->supported_version != 0) {
        tlsb_u16(&exts, TLS_EXTENSION_SUPPORTED_VERSIONS);
        tlsb_begin(&exts, 2);
        tlsb_u16(&exts, m->supported_version);
        tlsb_end(&exts);
    }
    if (m->server_share.group != 0) {
        tlsb_u16(&exts, TLS_EXTENSION_KEY_SHARE);
        tlsb_begin(&exts, 2);
        tlsb_u16(&exts, m->server_share.group);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->server_share.data);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->selected_identity_present) {
        tlsb_u16(&exts, TLS_EXTENSION_PRE_SHARED_KEY);
        tlsb_begin(&exts, 2);
        tlsb_u16(&exts, m->selected_identity);
        tlsb_end(&exts);
    }

    if (m->cookie.len > 0) {
        tlsb_u16(&exts, TLS_EXTENSION_COOKIE);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->cookie);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->selected_group != 0) {
        tlsb_u16(&exts, TLS_EXTENSION_KEY_SHARE);
        tlsb_begin(&exts, 2);
        tlsb_u16(&exts, m->selected_group);
        tlsb_end(&exts);
    }
    if (m->supported_points.len > 0) {
        tlsb_u16(&exts, TLS_EXTENSION_SUPPORTED_POINTS);
        tlsb_begin(&exts, 2);
        tlsb_begin(&exts, 1);
        tlsb_bytes(&exts, m->supported_points);
        tlsb_end(&exts);
        tlsb_end(&exts);
    }
    if (m->encrypted_client_hello.len > 0) {
        tlsb_u16(&exts, TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO);
        tlsb_begin(&exts, 2);
        tlsb_bytes(&exts, m->encrypted_client_hello);
        tlsb_end(&exts);
    }
    if (m->server_name_ack) {
        tlsb_u16(&exts, TLS_EXTENSION_SERVER_NAME);
        tlsb_u16(&exts, 0);
    }

    Slice ext_bytes = tlsb_finish(&exts, err);
    if (exts.failed)
        return ext_bytes;

    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_SERVER_HELLO);
    tlsb_begin(&b, 3);
    tlsb_u16(&b, m->vers);
    tlsb_bytes_with_length(&b, m->random, 32);
    tlsb_begin(&b, 1);
    tlsb_bytes(&b, m->session_id);
    tlsb_end(&b);
    tlsb_u16(&b, m->cipher_suite);
    tlsb_u8(&b, m->compression_method);

    if (ext_bytes.len > 0) {
        tlsb_begin(&b, 2);
        tlsb_bytes(&b, ext_bytes);
        tlsb_end(&b);
    }
    tlsb_end(&b);
    mem_free(a, ext_bytes.p, (size_t)ext_bytes.cap, 1);

    return tlsb_finish(&b, err);
}

/* An ALPN extension that names exactly one protocol, for ServerHello and
 * EncryptedExtensions. */
static bool tlsr_alpn_one(CryptobyteString *ext_data, Alloc *a, Str *out) {
    CryptobyteString proto_list;
    if (!tlsr_lp16(ext_data, &proto_list) || tlsr_empty(proto_list))
        return false;
    CryptobyteString proto;
    if (!tlsr_lp8(&proto_list, &proto) || tlsr_empty(proto) || !tlsr_empty(proto_list))
        return false;
    return tls_string(a, proto, out);
}

/* A list of SCTs, each with a uint16 length in front, appended to *out. */
static bool tlsr_scts(CryptobyteString *ext_data, Alloc *a, Slice *out) {
    CryptobyteString sct_list;
    if (!tlsr_lp16(ext_data, &sct_list) || tlsr_empty(sct_list))
        return false;
    while (!tlsr_empty(sct_list)) {
        Slice sct;
        if (!tlsr_lp16(&sct_list, &sct) || sct.len == 0 || !tls_push(a, out, &sct))
            return false;
    }
    return true;
}

static bool tls_server_hello_extension(TlsServerHelloMsg *m, Alloc *a,
                                       uint16_t extension, CryptobyteString *ext_data,
                                       bool *known) {
    *known = true;
    switch (extension) {
    case TLS_EXTENSION_STATUS_REQUEST:
        m->ocsp_stapling = true;
        return true;
    case TLS_EXTENSION_SESSION_TICKET:
        m->ticket_supported = true;
        return true;
    case TLS_EXTENSION_RENEGOTIATION_INFO:
        if (!tlsr_lp8(ext_data, &m->secure_renegotiation))
            return false;
        m->secure_renegotiation_supported = true;
        return true;
    case TLS_EXTENSION_EXTENDED_MASTER_SECRET:
        m->extended_master_secret = true;
        return true;
    case TLS_EXTENSION_ALPN:
        return tlsr_alpn_one(ext_data, a, &m->alpn_protocol);
    case TLS_EXTENSION_SCT:
        return tlsr_scts(ext_data, a, &m->scts);
    case TLS_EXTENSION_SUPPORTED_VERSIONS:
        return tlsr_u16(ext_data, &m->supported_version);
    case TLS_EXTENSION_COOKIE:
        return tlsr_lp16(ext_data, &m->cookie) && m->cookie.len != 0;
    case TLS_EXTENSION_KEY_SHARE:
        /* This extension has different formats in SH and HRR, accept either
         * and let the handshake logic decide. See RFC 8446, Section 4.2.8. */
        if (ext_data->len == 2)
            return tlsr_u16(ext_data, &m->selected_group);
        return tlsr_u16(ext_data, &m->server_share.group) &&
               tlsr_lp16(ext_data, &m->server_share.data);
    case TLS_EXTENSION_PRE_SHARED_KEY:
        m->selected_identity_present = true;
        return tlsr_u16(ext_data, &m->selected_identity);
    case TLS_EXTENSION_SUPPORTED_POINTS:
        /* RFC 4492, Section 5.1.2 */
        return tlsr_lp8(ext_data, &m->supported_points) && m->supported_points.len != 0;
    case TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO: /* encrypted_client_hello */
        return tlsr_copy_all(ext_data, a, &m->encrypted_client_hello);
    case TLS_EXTENSION_SERVER_NAME:
        if (ext_data->len != 0)
            return false;
        m->server_name_ack = true;
        return true;
    default:
        /* Ignore unknown extensions. */
        *known = false;
        return true;
    }
}

bool burrow__tls_server_hello_msg_unmarshal(TlsServerHelloMsg *m, Alloc *a,
                                            Slice data) {
    *m = burrow__tls_server_hello_msg_zero();
    m->original = data;
    CryptobyteString s = data;

    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_u16(&s, &m->vers) || !tlsr_bytes(&s, &m->random, 32) ||
        !tlsr_lp8(&s, &m->session_id) || !tlsr_u16(&s, &m->cipher_suite) ||
        !tlsr_u8(&s, &m->compression_method))
        return false;

    if (tlsr_empty(s)) {
        /* ServerHello is optionally followed by extension data */
        return true;
    }

    CryptobyteString extensions;
    if (!tlsr_lp16(&s, &extensions) || !tlsr_empty(s))
        return false;

    TlsSeenExts seen_exts;
    memset(&seen_exts, 0, sizeof(seen_exts));
    while (!tlsr_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!tlsr_u16(&extensions, &extension) || !tlsr_lp16(&extensions, &ext_data))
            return false;

        if (tls_seen(&seen_exts, extension))
            return false;

        bool known;
        if (!tls_server_hello_extension(m, a, extension, &ext_data, &known))
            return false;
        if (known && !tlsr_empty(ext_data))
            return false;
    }

    return true;
}

/* ------------------------------------------------------ EncryptedExtensions */

TlsEncryptedExtensionsMsg burrow__tls_encrypted_extensions_msg_zero(void) {
    TlsEncryptedExtensionsMsg m;
    memset(&m, 0, sizeof(m));
    m.alpn_protocol = BURROW_STR_EMPTY;
    m.quic_transport_parameters = slice_nil(TYPE_BYTE);
    m.ech_retry_configs = slice_nil(TYPE_BYTE);
    return m;
}

Slice burrow__tls_encrypted_extensions_msg_marshal(const TlsEncryptedExtensionsMsg *m,
                                                   Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_ENCRYPTED_EXTENSIONS);
    tlsb_begin(&b, 3);
    tlsb_begin(&b, 2);
    if (m->alpn_protocol.len > 0)
        tlsb_alpn_ext(&b, m->alpn_protocol);
    if (m->quic_transport_parameters.p != NULL) {
        /* marshal zero-length parameters when present */
        /* draft-ietf-quic-tls-32, Section 8.2 */
        tlsb_u16(&b, TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS);
        tlsb_begin(&b, 2);
        tlsb_bytes(&b, m->quic_transport_parameters);
        tlsb_end(&b);
    }
    if (m->early_data) {
        /* RFC 8446, Section 4.2.10 */
        tlsb_u16(&b, TLS_EXTENSION_EARLY_DATA);
        tlsb_u16(&b, 0); /* empty extension_data */
    }
    if (m->ech_retry_configs.len > 0) {
        tlsb_u16(&b, TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO);
        tlsb_begin(&b, 2);
        tlsb_bytes(&b, m->ech_retry_configs);
        tlsb_end(&b);
    }
    if (m->server_name_ack) {
        tlsb_u16(&b, TLS_EXTENSION_SERVER_NAME);
        tlsb_u16(&b, 0); /* empty extension_data */
    }
    tlsb_end(&b);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_encrypted_extensions_msg_unmarshal(TlsEncryptedExtensionsMsg *m,
                                                    Alloc *a, Slice data) {
    *m = burrow__tls_encrypted_extensions_msg_zero();
    CryptobyteString s = data;

    CryptobyteString extensions;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_lp16(&s, &extensions) || !tlsr_empty(s))
        return false;

    TlsSeenExts seen_exts;
    memset(&seen_exts, 0, sizeof(seen_exts));
    while (!tlsr_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!tlsr_u16(&extensions, &extension) || !tlsr_lp16(&extensions, &ext_data))
            return false;

        if (tls_seen(&seen_exts, extension))
            return false;

        switch (extension) {
        case TLS_EXTENSION_ALPN:
            if (!tlsr_alpn_one(&ext_data, a, &m->alpn_protocol))
                return false;
            break;
        case TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS:
            if (!tlsr_copy_all(&ext_data, a, &m->quic_transport_parameters))
                return false;
            break;
        case TLS_EXTENSION_EARLY_DATA:
            /* RFC 8446, Section 4.2.10 */
            m->early_data = true;
            break;
        case TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO:
            if (!tlsr_copy_all(&ext_data, a, &m->ech_retry_configs))
                return false;
            break;
        case TLS_EXTENSION_SERVER_NAME:
            if (ext_data.len != 0)
                return false;
            m->server_name_ack = true;
            break;
        default:
            /* Ignore unknown extensions. */
            continue;
        }

        if (!tlsr_empty(ext_data))
            return false;
    }

    return true;
}

/* --------------------------------------------------------- EndOfEarlyData */

Slice burrow__tls_end_of_early_data_msg_marshal(Alloc *a, Error *err) {
    Slice x = tls_make_bytes(a, 4, err);
    if (x.p != NULL)
        ((Byte *)x.p)[0] = TLS_TYPE_END_OF_EARLY_DATA;
    return x;
}

bool burrow__tls_end_of_early_data_msg_unmarshal(Slice data) {
    return data.len == 4;
}

/* ---------------------------------------------------------------- KeyUpdate */

Slice burrow__tls_key_update_msg_marshal(const TlsKeyUpdateMsg *m, Alloc *a,
                                         Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_KEY_UPDATE);
    tlsb_begin(&b, 3);
    if (m->update_requested)
        tlsb_u8(&b, 1);
    else
        tlsb_u8(&b, 0);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_key_update_msg_unmarshal(TlsKeyUpdateMsg *m, Slice data) {
    CryptobyteString s = data;

    uint8_t update_requested;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_u8(&s, &update_requested) || !tlsr_empty(s))
        return false;
    switch (update_requested) {
    case 0:
        m->update_requested = false;
        break;
    case 1:
        m->update_requested = true;
        break;
    default:
        return false;
    }
    return true;
}

/* ------------------------------------------------- NewSessionTicket, TLS 1.3 */

TlsNewSessionTicketMsgTLS13 burrow__tls_new_session_ticket_msg_tls13_zero(void) {
    TlsNewSessionTicketMsgTLS13 m;
    memset(&m, 0, sizeof(m));
    m.nonce = slice_nil(TYPE_BYTE);
    m.label = slice_nil(TYPE_BYTE);
    return m;
}

Slice burrow__tls_new_session_ticket_msg_tls13_marshal(
    const TlsNewSessionTicketMsgTLS13 *m, Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_NEW_SESSION_TICKET);
    tlsb_begin(&b, 3);
    tlsb_u32(&b, m->lifetime);
    tlsb_u32(&b, m->age_add);
    tlsb_begin(&b, 1);
    tlsb_bytes(&b, m->nonce);
    tlsb_end(&b);
    tlsb_begin(&b, 2);
    tlsb_bytes(&b, m->label);
    tlsb_end(&b);

    tlsb_begin(&b, 2);
    if (m->max_early_data > 0) {
        tlsb_u16(&b, TLS_EXTENSION_EARLY_DATA);
        tlsb_begin(&b, 2);
        tlsb_u32(&b, m->max_early_data);
        tlsb_end(&b);
    }
    tlsb_end(&b);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_new_session_ticket_msg_tls13_unmarshal(TlsNewSessionTicketMsgTLS13 *m,
                                                        Slice data) {
    *m = burrow__tls_new_session_ticket_msg_tls13_zero();
    CryptobyteString s = data;

    CryptobyteString extensions;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_u32(&s, &m->lifetime) || !tlsr_u32(&s, &m->age_add) ||
        !tlsr_lp8(&s, &m->nonce) || !tlsr_lp16(&s, &m->label) ||
        !tlsr_lp16(&s, &extensions) || !tlsr_empty(s))
        return false;

    while (!tlsr_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!tlsr_u16(&extensions, &extension) || !tlsr_lp16(&extensions, &ext_data))
            return false;

        switch (extension) {
        case TLS_EXTENSION_EARLY_DATA:
            if (!tlsr_u32(&ext_data, &m->max_early_data))
                return false;
            break;
        default:
            /* Ignore unknown extensions. */
            continue;
        }

        if (!tlsr_empty(ext_data))
            return false;
    }

    return true;
}

/* --------------------------------------------- CertificateRequest, TLS 1.3 */

TlsCertificateRequestMsgTLS13 burrow__tls_certificate_request_msg_tls13_zero(void) {
    TlsCertificateRequestMsgTLS13 m;
    memset(&m, 0, sizeof(m));
    m.supported_signature_algorithms = slice_nil(TYPE_UINT16);
    m.supported_signature_algorithms_cert = slice_nil(TYPE_UINT16);
    m.certificate_authorities = slice_nil(TYPE_BYTES);
    return m;
}

Slice burrow__tls_certificate_request_msg_tls13_marshal(
    const TlsCertificateRequestMsgTLS13 *m, Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_CERTIFICATE_REQUEST);
    tlsb_begin(&b, 3);
    /* certificate_request_context (SHALL be zero length unless used for
     * post-handshake authentication) */
    tlsb_u8(&b, 0);

    tlsb_begin(&b, 2);
    if (m->ocsp_stapling) {
        tlsb_u16(&b, TLS_EXTENSION_STATUS_REQUEST);
        tlsb_u16(&b, 0); /* empty extension_data */
    }
    if (m->scts) {
        /* RFC 8446, Section 4.4.2.1 makes no mention of
         * signed_certificate_timestamp in CertificateRequest, but "Extensions
         * in the Certificate message from the client MUST correspond to
         * extensions in the CertificateRequest message from the server." and it
         * appears in the table in Section 4.2. */
        tlsb_u16(&b, TLS_EXTENSION_SCT);
        tlsb_u16(&b, 0); /* empty extension_data */
    }
    if (m->supported_signature_algorithms.len > 0)
        tlsb_u16_list_ext(&b, TLS_EXTENSION_SIGNATURE_ALGORITHMS,
                          m->supported_signature_algorithms);
    if (m->supported_signature_algorithms_cert.len > 0)
        tlsb_u16_list_ext(&b, TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT,
                          m->supported_signature_algorithms_cert);
    if (m->certificate_authorities.len > 0) {
        tlsb_u16(&b, TLS_EXTENSION_CERTIFICATE_AUTHORITIES);
        tlsb_begin(&b, 2);
        tlsb_begin(&b, 2);
        for (Int i = 0; i < m->certificate_authorities.len; i++) {
            tlsb_begin(&b, 2);
            tlsb_bytes(&b, ((const Slice *)m->certificate_authorities.p)[i]);
            tlsb_end(&b);
        }
        tlsb_end(&b);
        tlsb_end(&b);
    }
    tlsb_end(&b);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_certificate_request_msg_tls13_unmarshal(
    TlsCertificateRequestMsgTLS13 *m, Alloc *a, Slice data) {
    *m = burrow__tls_certificate_request_msg_tls13_zero();
    CryptobyteString s = data;

    CryptobyteString context, extensions;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_lp8(&s, &context) || !tlsr_empty(context) ||
        !tlsr_lp16(&s, &extensions) || !tlsr_empty(s))
        return false;

    while (!tlsr_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!tlsr_u16(&extensions, &extension) || !tlsr_lp16(&extensions, &ext_data))
            return false;

        switch (extension) {
        case TLS_EXTENSION_STATUS_REQUEST:
            m->ocsp_stapling = true;
            break;
        case TLS_EXTENSION_SCT:
            m->scts = true;
            break;
        case TLS_EXTENSION_SIGNATURE_ALGORITHMS:
            if (!tlsr_u16_list(&ext_data, a, &m->supported_signature_algorithms))
                return false;
            break;
        case TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT:
            if (!tlsr_u16_list(&ext_data, a, &m->supported_signature_algorithms_cert))
                return false;
            break;
        case TLS_EXTENSION_CERTIFICATE_AUTHORITIES: {
            CryptobyteString auths;
            if (!tlsr_lp16(&ext_data, &auths) || tlsr_empty(auths))
                return false;
            while (!tlsr_empty(auths)) {
                Slice ca;
                if (!tlsr_lp16(&auths, &ca) || ca.len == 0 ||
                    !tls_push(a, &m->certificate_authorities, &ca))
                    return false;
            }
            break;
        }
        default:
            /* Ignore unknown extensions. */
            continue;
        }

        if (!tlsr_empty(ext_data))
            return false;
    }

    return true;
}

/* -------------------------------------------------------------- Certificate */

/* b[i], which panics out of range the way Go's index does. */
static uint32_t tls_byte_at(Slice b, Int i) {
    return BURROW_AT(Byte, b, i);
}

Slice burrow__tls_certificate_msg_marshal(const TlsCertificateMsg *m, Alloc *a,
                                          Error *err) {
    const Slice *certs = (const Slice *)m->certificates.p;
    Int i = 0;
    for (Int k = 0; k < m->certificates.len; k++)
        i += certs[k].len;

    Int length = 3 + 3 * m->certificates.len + i;
    Slice xs = tls_make_bytes(a, 4 + length, err);
    if (xs.p == NULL)
        return xs;
    Byte *x = (Byte *)xs.p;
    x[0] = TLS_TYPE_CERTIFICATE;
    x[1] = (Byte)(length >> 16);
    x[2] = (Byte)(length >> 8);
    x[3] = (Byte)length;

    Int certificate_octets = length - 3;
    x[4] = (Byte)(certificate_octets >> 16);
    x[5] = (Byte)(certificate_octets >> 8);
    x[6] = (Byte)certificate_octets;

    Byte *y = x + 7;
    for (Int k = 0; k < m->certificates.len; k++) {
        Slice slice = certs[k];
        y[0] = (Byte)(slice.len >> 16);
        y[1] = (Byte)(slice.len >> 8);
        y[2] = (Byte)slice.len;
        if (slice.len > 0)
            memcpy(y + 3, slice.p, (size_t)slice.len);
        y += 3 + slice.len;
    }

    return xs;
}

bool burrow__tls_certificate_msg_unmarshal(TlsCertificateMsg *m, Alloc *a, Slice data) {
    if (data.len < 7)
        return false;

    uint32_t certs_len =
        tls_byte_at(data, 4) << 16 | tls_byte_at(data, 5) << 8 | tls_byte_at(data, 6);
    if ((uint32_t)data.len != certs_len + 7)
        return false;

    Int num_certs = 0;
    Slice d = slice_sub(data, 7, data.len);
    while (certs_len > 0) {
        if (d.len < 4)
            return false;
        uint32_t cert_len =
            tls_byte_at(d, 0) << 16 | tls_byte_at(d, 1) << 8 | tls_byte_at(d, 2);
        if ((uint32_t)d.len < 3 + cert_len)
            return false;
        d = slice_sub(d, 3 + (Int)cert_len, d.len);
        certs_len -= 3 + cert_len;
        num_certs++;
    }

    Slice certificates = slice_make(a, TYPE_BYTES, num_certs, num_certs);
    if (certificates.p == NULL)
        return false;
    m->certificates = certificates;
    d = slice_sub(data, 7, data.len);
    for (Int i = 0; i < num_certs; i++) {
        uint32_t cert_len =
            tls_byte_at(d, 0) << 16 | tls_byte_at(d, 1) << 8 | tls_byte_at(d, 2);
        ((Slice *)certificates.p)[i] = slice_sub(d, 3, 3 + (Int)cert_len);
        d = slice_sub(d, 3 + (Int)cert_len, d.len);
    }

    return true;
}

/* ---------------------------------------------------- Certificate, TLS 1.3 */

TlsCertificate burrow__tls_certificate_zero(void) {
    TlsCertificate c;
    memset(&c, 0, sizeof(c));
    c.certificate = slice_nil(TYPE_BYTES);
    c.supported_signature_algorithms = slice_nil(TYPE_UINT16);
    c.ocsp_staple = slice_nil(TYPE_BYTE);
    c.signed_certificate_timestamps = slice_nil(TYPE_BYTES);
    return c;
}

TlsCertificateMsgTLS13 burrow__tls_certificate_msg_tls13_zero(void) {
    TlsCertificateMsgTLS13 m;
    memset(&m, 0, sizeof(m));
    m.certificate = burrow__tls_certificate_zero();
    return m;
}

static void tlsb_certificate(TlsBuilder *b, const TlsCertificate *certificate) {
    tlsb_begin(b, 3);
    for (Int i = 0; i < certificate->certificate.len; i++) {
        tlsb_begin(b, 3);
        tlsb_bytes(b, ((const Slice *)certificate->certificate.p)[i]);
        tlsb_end(b);
        tlsb_begin(b, 2);
        /* This library only supports OCSP and SCT for leaf certificates. */
        if (i == 0) {
            if (certificate->ocsp_staple.p != NULL) {
                tlsb_u16(b, TLS_EXTENSION_STATUS_REQUEST);
                tlsb_begin(b, 2);
                tlsb_u8(b, TLS_STATUS_TYPE_OCSP);
                tlsb_begin(b, 3);
                tlsb_bytes(b, certificate->ocsp_staple);
                tlsb_end(b);
                tlsb_end(b);
            }
            if (certificate->signed_certificate_timestamps.p != NULL) {
                const Slice *scts =
                    (const Slice *)certificate->signed_certificate_timestamps.p;
                tlsb_u16(b, TLS_EXTENSION_SCT);
                tlsb_begin(b, 2);
                tlsb_begin(b, 2);
                for (Int k = 0; k < certificate->signed_certificate_timestamps.len;
                     k++) {
                    tlsb_begin(b, 2);
                    tlsb_bytes(b, scts[k]);
                    tlsb_end(b);
                }
                tlsb_end(b);
                tlsb_end(b);
            }
        }
        tlsb_end(b);
    }
    tlsb_end(b);
}

Slice burrow__tls_certificate_msg_tls13_marshal(const TlsCertificateMsgTLS13 *m,
                                                Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_CERTIFICATE);
    tlsb_begin(&b, 3);
    tlsb_u8(&b, 0); /* certificate_request_context */

    TlsCertificate certificate = m->certificate;
    if (!m->ocsp_stapling)
        certificate.ocsp_staple = slice_nil(TYPE_BYTE);
    if (!m->scts)
        certificate.signed_certificate_timestamps = slice_nil(TYPE_BYTES);
    tlsb_certificate(&b, &certificate);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

static bool tlsr_certificate(CryptobyteString *s, Alloc *a,
                             TlsCertificate *certificate) {
    CryptobyteString cert_list;
    if (!tlsr_lp24(s, &cert_list))
        return false;
    while (!tlsr_empty(cert_list)) {
        Slice cert;
        CryptobyteString extensions;
        if (!tlsr_lp24(&cert_list, &cert) || !tlsr_lp16(&cert_list, &extensions))
            return false;
        if (!tls_push(a, &certificate->certificate, &cert))
            return false;
        while (!tlsr_empty(extensions)) {
            uint16_t extension;
            CryptobyteString ext_data;
            if (!tlsr_u16(&extensions, &extension) ||
                !tlsr_lp16(&extensions, &ext_data))
                return false;
            if (certificate->certificate.len > 1) {
                /* This library only supports OCSP and SCT for leaf
                 * certificates. */
                continue;
            }

            switch (extension) {
            case TLS_EXTENSION_STATUS_REQUEST: {
                uint8_t status_type;
                if (!tlsr_u8(&ext_data, &status_type) ||
                    status_type != TLS_STATUS_TYPE_OCSP ||
                    !tlsr_lp24(&ext_data, &certificate->ocsp_staple) ||
                    certificate->ocsp_staple.len == 0)
                    return false;
                break;
            }
            case TLS_EXTENSION_SCT:
                if (!tlsr_scts(&ext_data, a,
                               &certificate->signed_certificate_timestamps))
                    return false;
                break;
            default:
                /* Ignore unknown extensions. */
                continue;
            }

            if (!tlsr_empty(ext_data))
                return false;
        }
    }
    return true;
}

bool burrow__tls_certificate_msg_tls13_unmarshal(TlsCertificateMsgTLS13 *m, Alloc *a,
                                                 Slice data) {
    *m = burrow__tls_certificate_msg_tls13_zero();
    CryptobyteString s = data;

    CryptobyteString context;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_lp8(&s, &context) || !tlsr_empty(context) ||
        !tlsr_certificate(&s, a, &m->certificate) || !tlsr_empty(s))
        return false;

    m->scts = m->certificate.signed_certificate_timestamps.p != NULL;
    m->ocsp_stapling = m->certificate.ocsp_staple.p != NULL;

    return true;
}

/* -------------------------------------------------------- ServerKeyExchange */

/* A message that is a type, a uint24 length and then body, laid out by hand
 * the way Go does for serverKeyExchangeMsg and clientKeyExchangeMsg. */
static Slice tls_marshal_body(Alloc *a, uint8_t typ, Slice body, Error *err) {
    Int length = body.len;
    Slice xs = tls_make_bytes(a, length + 4, err);
    if (xs.p == NULL)
        return xs;
    Byte *x = (Byte *)xs.p;
    x[0] = typ;
    x[1] = (Byte)(length >> 16);
    x[2] = (Byte)(length >> 8);
    x[3] = (Byte)length;
    if (length > 0)
        memcpy(x + 4, body.p, (size_t)length);

    return xs;
}

Slice burrow__tls_server_key_exchange_msg_marshal(const TlsServerKeyExchangeMsg *m,
                                                  Alloc *a, Error *err) {
    return tls_marshal_body(a, TLS_TYPE_SERVER_KEY_EXCHANGE, m->key, err);
}

bool burrow__tls_server_key_exchange_msg_unmarshal(TlsServerKeyExchangeMsg *m,
                                                   Slice data) {
    if (data.len < 4)
        return false;
    m->key = slice_sub(data, 4, data.len);
    return true;
}

/* -------------------------------------------------------- CertificateStatus */

Slice burrow__tls_certificate_status_msg_marshal(const TlsCertificateStatusMsg *m,
                                                 Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_CERTIFICATE_STATUS);
    tlsb_begin(&b, 3);
    tlsb_u8(&b, TLS_STATUS_TYPE_OCSP);
    tlsb_begin(&b, 3);
    tlsb_bytes(&b, m->response);
    tlsb_end(&b);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_certificate_status_msg_unmarshal(TlsCertificateStatusMsg *m,
                                                  Slice data) {
    CryptobyteString s = data;

    uint8_t status_type;
    if (!tlsr_skip(&s, 4) || /* message type and uint24 length field */
        !tlsr_u8(&s, &status_type) || status_type != TLS_STATUS_TYPE_OCSP ||
        !tlsr_lp24(&s, &m->response) || m->response.len == 0 || !tlsr_empty(s))
        return false;
    return true;
}

/* ---------------------------------------------------------- ServerHelloDone */

Slice burrow__tls_server_hello_done_msg_marshal(Alloc *a, Error *err) {
    Slice x = tls_make_bytes(a, 4, err);
    if (x.p != NULL)
        ((Byte *)x.p)[0] = TLS_TYPE_SERVER_HELLO_DONE;
    return x;
}

bool burrow__tls_server_hello_done_msg_unmarshal(Slice data) {
    return data.len == 4;
}

/* -------------------------------------------------------- ClientKeyExchange */

Slice burrow__tls_client_key_exchange_msg_marshal(const TlsClientKeyExchangeMsg *m,
                                                  Alloc *a, Error *err) {
    return tls_marshal_body(a, TLS_TYPE_CLIENT_KEY_EXCHANGE, m->ciphertext, err);
}

bool burrow__tls_client_key_exchange_msg_unmarshal(TlsClientKeyExchangeMsg *m,
                                                   Slice data) {
    if (data.len < 4)
        return false;
    Int l = (Int)(tls_byte_at(data, 1) << 16 | tls_byte_at(data, 2) << 8 |
                  tls_byte_at(data, 3));
    if (l != data.len - 4)
        return false;
    m->ciphertext = slice_sub(data, 4, data.len);
    return true;
}

/* ----------------------------------------------------------------- Finished */

Slice burrow__tls_finished_msg_marshal(const TlsFinishedMsg *m, Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_FINISHED);
    tlsb_begin(&b, 3);
    tlsb_bytes(&b, m->verify_data);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_finished_msg_unmarshal(TlsFinishedMsg *m, Slice data) {
    CryptobyteString s = data;
    return tlsr_skip(&s, 1) && tlsr_lp24(&s, &m->verify_data) && tlsr_empty(s);
}

/* ------------------------------------------------------- CertificateRequest */

TlsCertificateRequestMsg burrow__tls_certificate_request_msg_zero(void) {
    TlsCertificateRequestMsg m;
    memset(&m, 0, sizeof(m));
    m.certificate_types = slice_nil(TYPE_BYTE);
    m.supported_signature_algorithms = slice_nil(TYPE_UINT16);
    m.certificate_authorities = slice_nil(TYPE_BYTES);
    return m;
}

Slice burrow__tls_certificate_request_msg_marshal(const TlsCertificateRequestMsg *m,
                                                  Alloc *a, Error *err) {
    /* See RFC 4346, Section 7.4.4. */
    const Slice *cas = (const Slice *)m->certificate_authorities.p;
    Int length = 1 + m->certificate_types.len + 2;
    Int cas_length = 0;
    for (Int i = 0; i < m->certificate_authorities.len; i++)
        cas_length += 2 + cas[i].len;
    length += cas_length;

    if (m->has_signature_algorithm)
        length += 2 + 2 * m->supported_signature_algorithms.len;

    Slice xs = tls_make_bytes(a, 4 + length, err);
    if (xs.p == NULL)
        return xs;
    Byte *x = (Byte *)xs.p;
    x[0] = TLS_TYPE_CERTIFICATE_REQUEST;
    x[1] = (Byte)(length >> 16);
    x[2] = (Byte)(length >> 8);
    x[3] = (Byte)length;

    x[4] = (Byte)m->certificate_types.len;

    if (m->certificate_types.len > 0)
        memcpy(x + 5, m->certificate_types.p, (size_t)m->certificate_types.len);
    Byte *y = x + 5 + m->certificate_types.len;

    if (m->has_signature_algorithm) {
        const uint16_t *algs = (const uint16_t *)m->supported_signature_algorithms.p;
        Int n = m->supported_signature_algorithms.len * 2;
        y[0] = (Byte)(n >> 8);
        y[1] = (Byte)n;
        y += 2;
        for (Int i = 0; i < m->supported_signature_algorithms.len; i++) {
            y[0] = (Byte)(algs[i] >> 8);
            y[1] = (Byte)algs[i];
            y += 2;
        }
    }

    y[0] = (Byte)(cas_length >> 8);
    y[1] = (Byte)cas_length;
    y += 2;
    for (Int i = 0; i < m->certificate_authorities.len; i++) {
        Slice ca = cas[i];
        y[0] = (Byte)(ca.len >> 8);
        y[1] = (Byte)ca.len;
        y += 2;
        if (ca.len > 0)
            memcpy(y, ca.p, (size_t)ca.len);
        y += ca.len;
    }

    return xs;
}

bool burrow__tls_certificate_request_msg_unmarshal(TlsCertificateRequestMsg *m,
                                                   Alloc *a, Slice data) {
    if (data.len < 5)
        return false;

    uint32_t length =
        tls_byte_at(data, 1) << 16 | tls_byte_at(data, 2) << 8 | tls_byte_at(data, 3);
    if ((uint32_t)data.len - 4 != length)
        return false;

    Int num_cert_types = (Int)tls_byte_at(data, 4);
    data = slice_sub(data, 5, data.len);
    if (num_cert_types == 0 || data.len <= num_cert_types)
        return false;

    m->certificate_types = slice_make(a, TYPE_BYTE, num_cert_types, num_cert_types);
    if (m->certificate_types.p == NULL ||
        slice_copy(m->certificate_types, data) != num_cert_types)
        return false;

    data = slice_sub(data, num_cert_types, data.len);

    if (m->has_signature_algorithm) {
        if (data.len < 2)
            return false;
        uint16_t sig_and_hash_len =
            (uint16_t)(tls_byte_at(data, 0) << 8 | tls_byte_at(data, 1));
        data = slice_sub(data, 2, data.len);
        if ((sig_and_hash_len & 1) != 0 || sig_and_hash_len == 0)
            return false;
        if (data.len < (Int)sig_and_hash_len)
            return false;
        Int num_sig_algos = sig_and_hash_len / 2;
        Slice algs = slice_make(a, TYPE_UINT16, num_sig_algos, num_sig_algos);
        if (algs.p == NULL)
            return false;
        m->supported_signature_algorithms = algs;
        for (Int i = 0; i < num_sig_algos; i++) {
            ((uint16_t *)algs.p)[i] =
                (uint16_t)(tls_byte_at(data, 0) << 8 | tls_byte_at(data, 1));
            data = slice_sub(data, 2, data.len);
        }
    }

    if (data.len < 2)
        return false;
    uint16_t cas_length = (uint16_t)(tls_byte_at(data, 0) << 8 | tls_byte_at(data, 1));
    data = slice_sub(data, 2, data.len);
    if (data.len < (Int)cas_length)
        return false;
    Slice cas = slice_make(a, TYPE_BYTE, cas_length, cas_length);
    if (cas.p == NULL)
        return false;
    slice_copy(cas, data);
    data = slice_sub(data, cas_length, data.len);

    m->certificate_authorities = slice_nil(TYPE_BYTES);
    while (cas.len > 0) {
        if (cas.len < 2)
            return false;
        uint16_t ca_len = (uint16_t)(tls_byte_at(cas, 0) << 8 | tls_byte_at(cas, 1));
        cas = slice_sub(cas, 2, cas.len);

        if (cas.len < (Int)ca_len)
            return false;

        Slice ca = slice_sub(cas, 0, ca_len);
        if (!tls_push(a, &m->certificate_authorities, &ca))
            return false;
        cas = slice_sub(cas, ca_len, cas.len);
    }

    return data.len == 0;
}

/* -------------------------------------------------------- CertificateVerify */

Slice burrow__tls_certificate_verify_msg_marshal(const TlsCertificateVerifyMsg *m,
                                                 Alloc *a, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, TLS_TYPE_CERTIFICATE_VERIFY);
    tlsb_begin(&b, 3);
    if (m->has_signature_algorithm)
        tlsb_u16(&b, m->signature_algorithm);
    tlsb_begin(&b, 2);
    tlsb_bytes(&b, m->signature);
    tlsb_end(&b);
    tlsb_end(&b);

    return tlsb_finish(&b, err);
}

bool burrow__tls_certificate_verify_msg_unmarshal(TlsCertificateVerifyMsg *m,
                                                  Slice data) {
    CryptobyteString s = data;

    if (!tlsr_skip(&s, 4)) /* message type and uint24 length field */
        return false;
    if (m->has_signature_algorithm) {
        if (!tlsr_u16(&s, &m->signature_algorithm))
            return false;
    }
    return tlsr_lp16(&s, &m->signature) && tlsr_empty(s);
}

/* --------------------------------------------------------- NewSessionTicket */

Slice burrow__tls_new_session_ticket_msg_marshal(const TlsNewSessionTicketMsg *m,
                                                 Alloc *a, Error *err) {
    /* See RFC 5077, Section 3.3. */
    Int ticket_len = m->ticket.len;
    Int length = 2 + 4 + ticket_len;
    Slice xs = tls_make_bytes(a, 4 + length, err);
    if (xs.p == NULL)
        return xs;
    Byte *x = (Byte *)xs.p;
    x[0] = TLS_TYPE_NEW_SESSION_TICKET;
    x[1] = (Byte)(length >> 16);
    x[2] = (Byte)(length >> 8);
    x[3] = (Byte)length;
    x[8] = (Byte)(ticket_len >> 8);
    x[9] = (Byte)ticket_len;
    if (ticket_len > 0)
        memcpy(x + 10, m->ticket.p, (size_t)ticket_len);

    return xs;
}

bool burrow__tls_new_session_ticket_msg_unmarshal(TlsNewSessionTicketMsg *m,
                                                  Slice data) {
    if (data.len < 10)
        return false;

    uint32_t length =
        tls_byte_at(data, 1) << 16 | tls_byte_at(data, 2) << 8 | tls_byte_at(data, 3);
    if ((uint32_t)data.len - 4 != length)
        return false;

    Int ticket_len = (Int)(tls_byte_at(data, 8) << 8) + (Int)tls_byte_at(data, 9);
    if (data.len - 10 != ticket_len)
        return false;

    m->ticket = slice_sub(data, 10, data.len);

    return true;
}

/* ------------------------------------------------------------- HelloRequest */

Slice burrow__tls_hello_request_msg_marshal(Alloc *a, Error *err) {
    Slice x = tls_make_bytes(a, 4, err);
    if (x.p != NULL)
        ((Byte *)x.p)[0] = TLS_TYPE_HELLO_REQUEST;
    return x;
}

bool burrow__tls_hello_request_msg_unmarshal(Slice data) {
    return data.len == 4;
}

/* ------------------------------------------------------- handshakeMessage */

void burrow__tls_handshake_msg_init(TlsHandshakeMsg *m, TlsMsgKind kind) {
    memset(m, 0, sizeof *m);
    m->kind = kind;
    switch (kind) {
    case TLS_MSG_CLIENT_HELLO:
        m->client_hello = burrow__tls_client_hello_msg_zero();
        break;
    case TLS_MSG_SERVER_HELLO:
        m->server_hello = burrow__tls_server_hello_msg_zero();
        break;
    case TLS_MSG_NEW_SESSION_TICKET:
        m->new_session_ticket.ticket = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_NEW_SESSION_TICKET_TLS13:
        m->new_session_ticket_tls13 = burrow__tls_new_session_ticket_msg_tls13_zero();
        break;
    case TLS_MSG_CERTIFICATE:
        m->certificate.certificates = slice_nil(TYPE_BYTES);
        break;
    case TLS_MSG_CERTIFICATE_TLS13:
        m->certificate_tls13 = burrow__tls_certificate_msg_tls13_zero();
        break;
    case TLS_MSG_CERTIFICATE_REQUEST:
        m->certificate_request = burrow__tls_certificate_request_msg_zero();
        break;
    case TLS_MSG_CERTIFICATE_REQUEST_TLS13:
        m->certificate_request_tls13 = burrow__tls_certificate_request_msg_tls13_zero();
        break;
    case TLS_MSG_CERTIFICATE_STATUS:
        m->certificate_status.response = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_SERVER_KEY_EXCHANGE:
        m->server_key_exchange.key = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_CLIENT_KEY_EXCHANGE:
        m->client_key_exchange.ciphertext = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_CERTIFICATE_VERIFY:
        m->certificate_verify.signature = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_FINISHED:
        m->finished.verify_data = slice_nil(TYPE_BYTE);
        break;
    case TLS_MSG_ENCRYPTED_EXTENSIONS:
        m->encrypted_extensions = burrow__tls_encrypted_extensions_msg_zero();
        break;
    default:
        break;
    }
}

Slice burrow__tls_handshake_msg_marshal(const TlsHandshakeMsg *m, Alloc *a,
                                        Error *err) {
    switch (m->kind) {
    case TLS_MSG_HELLO_REQUEST:
        return burrow__tls_hello_request_msg_marshal(a, err);
    case TLS_MSG_CLIENT_HELLO:
        return burrow__tls_client_hello_msg_marshal(&m->client_hello, a, err);
    case TLS_MSG_SERVER_HELLO:
        return burrow__tls_server_hello_msg_marshal(&m->server_hello, a, err);
    case TLS_MSG_NEW_SESSION_TICKET:
        return burrow__tls_new_session_ticket_msg_marshal(&m->new_session_ticket, a,
                                                          err);
    case TLS_MSG_NEW_SESSION_TICKET_TLS13:
        return burrow__tls_new_session_ticket_msg_tls13_marshal(
            &m->new_session_ticket_tls13, a, err);
    case TLS_MSG_CERTIFICATE:
        return burrow__tls_certificate_msg_marshal(&m->certificate, a, err);
    case TLS_MSG_CERTIFICATE_TLS13:
        return burrow__tls_certificate_msg_tls13_marshal(&m->certificate_tls13, a, err);
    case TLS_MSG_CERTIFICATE_REQUEST:
        return burrow__tls_certificate_request_msg_marshal(&m->certificate_request, a,
                                                           err);
    case TLS_MSG_CERTIFICATE_REQUEST_TLS13:
        return burrow__tls_certificate_request_msg_tls13_marshal(
            &m->certificate_request_tls13, a, err);
    case TLS_MSG_CERTIFICATE_STATUS:
        return burrow__tls_certificate_status_msg_marshal(&m->certificate_status, a,
                                                          err);
    case TLS_MSG_SERVER_KEY_EXCHANGE:
        return burrow__tls_server_key_exchange_msg_marshal(&m->server_key_exchange, a,
                                                           err);
    case TLS_MSG_SERVER_HELLO_DONE:
        return burrow__tls_server_hello_done_msg_marshal(a, err);
    case TLS_MSG_CLIENT_KEY_EXCHANGE:
        return burrow__tls_client_key_exchange_msg_marshal(&m->client_key_exchange, a,
                                                           err);
    case TLS_MSG_CERTIFICATE_VERIFY:
        return burrow__tls_certificate_verify_msg_marshal(&m->certificate_verify, a,
                                                          err);
    case TLS_MSG_FINISHED:
        return burrow__tls_finished_msg_marshal(&m->finished, a, err);
    case TLS_MSG_ENCRYPTED_EXTENSIONS:
        return burrow__tls_encrypted_extensions_msg_marshal(&m->encrypted_extensions, a,
                                                            err);
    case TLS_MSG_END_OF_EARLY_DATA:
        return burrow__tls_end_of_early_data_msg_marshal(a, err);
    case TLS_MSG_KEY_UPDATE:
        return burrow__tls_key_update_msg_marshal(&m->key_update, a, err);
    default:
        panic_str(BURROW_S("tls: marshal of a handshake message of no kind"));
    }
}

bool burrow__tls_handshake_msg_unmarshal(TlsHandshakeMsg *m, Alloc *a, Slice data) {
    switch (m->kind) {
    case TLS_MSG_HELLO_REQUEST:
        return burrow__tls_hello_request_msg_unmarshal(data);
    case TLS_MSG_CLIENT_HELLO:
        return burrow__tls_client_hello_msg_unmarshal(&m->client_hello, a, data);
    case TLS_MSG_SERVER_HELLO:
        return burrow__tls_server_hello_msg_unmarshal(&m->server_hello, a, data);
    case TLS_MSG_NEW_SESSION_TICKET:
        return burrow__tls_new_session_ticket_msg_unmarshal(&m->new_session_ticket,
                                                            data);
    case TLS_MSG_NEW_SESSION_TICKET_TLS13:
        return burrow__tls_new_session_ticket_msg_tls13_unmarshal(
            &m->new_session_ticket_tls13, data);
    case TLS_MSG_CERTIFICATE:
        return burrow__tls_certificate_msg_unmarshal(&m->certificate, a, data);
    case TLS_MSG_CERTIFICATE_TLS13:
        return burrow__tls_certificate_msg_tls13_unmarshal(&m->certificate_tls13, a,
                                                           data);
    case TLS_MSG_CERTIFICATE_REQUEST:
        return burrow__tls_certificate_request_msg_unmarshal(&m->certificate_request, a,
                                                             data);
    case TLS_MSG_CERTIFICATE_REQUEST_TLS13:
        return burrow__tls_certificate_request_msg_tls13_unmarshal(
            &m->certificate_request_tls13, a, data);
    case TLS_MSG_CERTIFICATE_STATUS:
        return burrow__tls_certificate_status_msg_unmarshal(&m->certificate_status,
                                                            data);
    case TLS_MSG_SERVER_KEY_EXCHANGE:
        return burrow__tls_server_key_exchange_msg_unmarshal(&m->server_key_exchange,
                                                             data);
    case TLS_MSG_SERVER_HELLO_DONE:
        return burrow__tls_server_hello_done_msg_unmarshal(data);
    case TLS_MSG_CLIENT_KEY_EXCHANGE:
        return burrow__tls_client_key_exchange_msg_unmarshal(&m->client_key_exchange,
                                                             data);
    case TLS_MSG_CERTIFICATE_VERIFY:
        return burrow__tls_certificate_verify_msg_unmarshal(&m->certificate_verify,
                                                            data);
    case TLS_MSG_FINISHED:
        return burrow__tls_finished_msg_unmarshal(&m->finished, data);
    case TLS_MSG_ENCRYPTED_EXTENSIONS:
        return burrow__tls_encrypted_extensions_msg_unmarshal(&m->encrypted_extensions,
                                                              a, data);
    case TLS_MSG_END_OF_EARLY_DATA:
        return burrow__tls_end_of_early_data_msg_unmarshal(data);
    case TLS_MSG_KEY_UPDATE:
        return burrow__tls_key_update_msg_unmarshal(&m->key_update, data);
    default:
        return false;
    }
}

static const Str tls_msg_type_names[] = {
    BURROW_S_INIT("<nil>"),
    BURROW_S_INIT("*tls.helloRequestMsg"),
    BURROW_S_INIT("*tls.clientHelloMsg"),
    BURROW_S_INIT("*tls.serverHelloMsg"),
    BURROW_S_INIT("*tls.newSessionTicketMsg"),
    BURROW_S_INIT("*tls.newSessionTicketMsgTLS13"),
    BURROW_S_INIT("*tls.certificateMsg"),
    BURROW_S_INIT("*tls.certificateMsgTLS13"),
    BURROW_S_INIT("*tls.certificateRequestMsg"),
    BURROW_S_INIT("*tls.certificateRequestMsgTLS13"),
    BURROW_S_INIT("*tls.certificateStatusMsg"),
    BURROW_S_INIT("*tls.serverKeyExchangeMsg"),
    BURROW_S_INIT("*tls.serverHelloDoneMsg"),
    BURROW_S_INIT("*tls.clientKeyExchangeMsg"),
    BURROW_S_INIT("*tls.certificateVerifyMsg"),
    BURROW_S_INIT("*tls.finishedMsg"),
    BURROW_S_INIT("*tls.encryptedExtensionsMsg"),
    BURROW_S_INIT("*tls.endOfEarlyDataMsg"),
    BURROW_S_INIT("*tls.keyUpdateMsg"),
};

Str burrow__tls_handshake_msg_type_name(TlsMsgKind kind) {
    if (kind < 0 ||
        kind >= (TlsMsgKind)(sizeof tls_msg_type_names / sizeof tls_msg_type_names[0]))
        return tls_msg_type_names[0];
    return tls_msg_type_names[kind];
}

Error burrow__tls_unexpected_message_error(TlsMsgKind wanted, TlsMsgKind got) {
    return fmt_errorf_v("tls: received unexpected handshake message of type %s when "
                        "waiting for %s",
                        burrow__tls_handshake_msg_type_name(got),
                        burrow__tls_handshake_msg_type_name(wanted));
}

/* h.Write, whose error a hash never has. */
static void tls_transcript_write(IoWriter h, Slice p) {
    Error err = BURROW_NO_ERROR;
    (void)h.vt->write(h.data, p, &err);
}

Error burrow__tls_transcript_msg(const TlsHandshakeMsg *m, IoWriter h, Alloc *a) {
    Slice orig = slice_nil(TYPE_BYTE);
    if (m->kind == TLS_MSG_CLIENT_HELLO)
        orig = m->client_hello.original;
    else if (m->kind == TLS_MSG_SERVER_HELLO)
        orig = m->server_hello.original;
    if (orig.p != NULL) {
        tls_transcript_write(h, orig);
        return BURROW_NO_ERROR;
    }

    Error err = BURROW_NO_ERROR;
    Slice data = burrow__tls_handshake_msg_marshal(m, a, &err);
    if (BURROW_FAILED(err))
        return err;
    tls_transcript_write(h, data);
    return BURROW_NO_ERROR;
}
