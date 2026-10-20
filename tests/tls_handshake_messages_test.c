/* Derived from Go's src/crypto/tls/handshake_messages_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests table ends with a SessionState, which comes with ticket.go and is
 * not here yet. Go builds the random messages with testing/quick, which calls
 * each type's Generate method; here a table of functions does the same job,
 * with the Generate bodies ported line for line. reflect.DeepEqual becomes an
 * equal function per message that tells a nil slice from an empty one the way
 * DeepEqual does, and the failure messages print the bytes only, since C has
 * no %#v.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/rand.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

typedef union TestMsg {
    TlsClientHelloMsg client_hello;
    TlsServerHelloMsg server_hello;
    TlsFinishedMsg finished;
    TlsCertificateMsg certificate;
    TlsCertificateRequestMsg certificate_request;
    TlsCertificateVerifyMsg certificate_verify;
    TlsCertificateStatusMsg certificate_status;
    TlsClientKeyExchangeMsg client_key_exchange;
    TlsNewSessionTicketMsg new_session_ticket;
    TlsEncryptedExtensionsMsg encrypted_extensions;
    TlsKeyUpdateMsg key_update;
    TlsNewSessionTicketMsgTLS13 new_session_ticket_tls13;
    TlsCertificateRequestMsgTLS13 certificate_request_tls13;
    TlsCertificateMsgTLS13 certificate_tls13;
} TestMsg;

/* One entry of Go's tests table: the value the entry starts out as, the
 * message's Generate, and the handshakeMessage methods. */
typedef struct MsgKind {
    const char *name;
    void (*init)(TestMsg *m);
    void (*generate)(TestMsg *m, MathRandRand *r, Alloc *a);
    Slice (*marshal)(const TestMsg *m, Alloc *a, Error *err);
    bool (*unmarshal)(TestMsg *m, Alloc *a, Slice data);
    bool (*equal)(const TestMsg *x, const TestMsg *y);
} MsgKind;

/* ------------------------------------------------------------ DeepEqual */

static bool eq_bytes(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y))
        return false;
    return bytes_equal(x, y);
}

static bool eq_u16s(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y) || x.len != y.len)
        return false;
    return x.len == 0 || memcmp(x.p, y.p, (size_t)x.len * sizeof(uint16_t)) == 0;
}

static bool eq_bytes_list(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y) || x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        if (!eq_bytes(BURROW_AT(Slice, x, i), BURROW_AT(Slice, y, i)))
            return false;
    }
    return true;
}

static bool eq_strs(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y) || x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        if (!str_eq(BURROW_AT(Str, x, i), BURROW_AT(Str, y, i)))
            return false;
    }
    return true;
}

static bool eq_key_shares(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y) || x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        TlsKeyShare a = BURROW_AT(TlsKeyShare, x, i);
        TlsKeyShare b = BURROW_AT(TlsKeyShare, y, i);
        if (a.group != b.group || !eq_bytes(a.data, b.data))
            return false;
    }
    return true;
}

static bool eq_psk_identities(Slice x, Slice y) {
    if (slice_is_nil(x) != slice_is_nil(y) || x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        TlsPskIdentity a = BURROW_AT(TlsPskIdentity, x, i);
        TlsPskIdentity b = BURROW_AT(TlsPskIdentity, y, i);
        if (!eq_bytes(a.label, b.label) ||
            a.obfuscated_ticket_age != b.obfuscated_ticket_age)
            return false;
    }
    return true;
}

static bool eq_client_hello(const TestMsg *mx, const TestMsg *my) {
    const TlsClientHelloMsg *x = &mx->client_hello, *y = &my->client_hello;
    return eq_bytes(x->original, y->original) && x->vers == y->vers &&
           eq_bytes(x->random, y->random) && eq_bytes(x->session_id, y->session_id) &&
           eq_u16s(x->cipher_suites, y->cipher_suites) &&
           eq_bytes(x->compression_methods, y->compression_methods) &&
           str_eq(x->server_name, y->server_name) &&
           x->ocsp_stapling == y->ocsp_stapling &&
           eq_u16s(x->supported_curves, y->supported_curves) &&
           eq_bytes(x->supported_points, y->supported_points) &&
           x->ticket_supported == y->ticket_supported &&
           eq_bytes(x->session_ticket, y->session_ticket) &&
           eq_u16s(x->supported_signature_algorithms,
                   y->supported_signature_algorithms) &&
           eq_u16s(x->supported_signature_algorithms_cert,
                   y->supported_signature_algorithms_cert) &&
           x->secure_renegotiation_supported == y->secure_renegotiation_supported &&
           eq_bytes(x->secure_renegotiation, y->secure_renegotiation) &&
           x->extended_master_secret == y->extended_master_secret &&
           eq_strs(x->alpn_protocols, y->alpn_protocols) && x->scts == y->scts &&
           eq_u16s(x->supported_versions, y->supported_versions) &&
           eq_bytes(x->cookie, y->cookie) &&
           eq_key_shares(x->key_shares, y->key_shares) &&
           x->early_data == y->early_data && eq_bytes(x->psk_modes, y->psk_modes) &&
           eq_psk_identities(x->psk_identities, y->psk_identities) &&
           eq_bytes_list(x->psk_binders, y->psk_binders) &&
           eq_bytes(x->quic_transport_parameters, y->quic_transport_parameters) &&
           eq_bytes(x->encrypted_client_hello, y->encrypted_client_hello) &&
           eq_u16s(x->extensions, y->extensions);
}

static bool eq_server_hello(const TestMsg *mx, const TestMsg *my) {
    const TlsServerHelloMsg *x = &mx->server_hello, *y = &my->server_hello;
    return eq_bytes(x->original, y->original) && x->vers == y->vers &&
           eq_bytes(x->random, y->random) && eq_bytes(x->session_id, y->session_id) &&
           x->cipher_suite == y->cipher_suite &&
           x->compression_method == y->compression_method &&
           x->ocsp_stapling == y->ocsp_stapling &&
           x->ticket_supported == y->ticket_supported &&
           x->secure_renegotiation_supported == y->secure_renegotiation_supported &&
           eq_bytes(x->secure_renegotiation, y->secure_renegotiation) &&
           x->extended_master_secret == y->extended_master_secret &&
           str_eq(x->alpn_protocol, y->alpn_protocol) &&
           eq_bytes_list(x->scts, y->scts) &&
           x->supported_version == y->supported_version &&
           x->server_share.group == y->server_share.group &&
           eq_bytes(x->server_share.data, y->server_share.data) &&
           x->selected_identity_present == y->selected_identity_present &&
           x->selected_identity == y->selected_identity &&
           eq_bytes(x->supported_points, y->supported_points) &&
           eq_bytes(x->encrypted_client_hello, y->encrypted_client_hello) &&
           x->server_name_ack == y->server_name_ack && eq_bytes(x->cookie, y->cookie) &&
           x->selected_group == y->selected_group;
}

static bool eq_finished(const TestMsg *x, const TestMsg *y) {
    return eq_bytes(x->finished.verify_data, y->finished.verify_data);
}

static bool eq_certificate(const TestMsg *x, const TestMsg *y) {
    return eq_bytes_list(x->certificate.certificates, y->certificate.certificates);
}

static bool eq_certificate_request(const TestMsg *mx, const TestMsg *my) {
    const TlsCertificateRequestMsg *x = &mx->certificate_request;
    const TlsCertificateRequestMsg *y = &my->certificate_request;
    return x->has_signature_algorithm == y->has_signature_algorithm &&
           eq_bytes(x->certificate_types, y->certificate_types) &&
           eq_u16s(x->supported_signature_algorithms,
                   y->supported_signature_algorithms) &&
           eq_bytes_list(x->certificate_authorities, y->certificate_authorities);
}

static bool eq_certificate_verify(const TestMsg *mx, const TestMsg *my) {
    const TlsCertificateVerifyMsg *x = &mx->certificate_verify;
    const TlsCertificateVerifyMsg *y = &my->certificate_verify;
    return x->has_signature_algorithm == y->has_signature_algorithm &&
           x->signature_algorithm == y->signature_algorithm &&
           eq_bytes(x->signature, y->signature);
}

static bool eq_certificate_status(const TestMsg *x, const TestMsg *y) {
    return eq_bytes(x->certificate_status.response, y->certificate_status.response);
}

static bool eq_client_key_exchange(const TestMsg *x, const TestMsg *y) {
    return eq_bytes(x->client_key_exchange.ciphertext,
                    y->client_key_exchange.ciphertext);
}

static bool eq_new_session_ticket(const TestMsg *x, const TestMsg *y) {
    return eq_bytes(x->new_session_ticket.ticket, y->new_session_ticket.ticket);
}

static bool eq_encrypted_extensions(const TestMsg *mx, const TestMsg *my) {
    const TlsEncryptedExtensionsMsg *x = &mx->encrypted_extensions;
    const TlsEncryptedExtensionsMsg *y = &my->encrypted_extensions;
    return str_eq(x->alpn_protocol, y->alpn_protocol) &&
           eq_bytes(x->quic_transport_parameters, y->quic_transport_parameters) &&
           x->early_data == y->early_data &&
           eq_bytes(x->ech_retry_configs, y->ech_retry_configs) &&
           x->server_name_ack == y->server_name_ack;
}

static bool eq_end_of_early_data(const TestMsg *x, const TestMsg *y) {
    (void)x;
    (void)y;
    return true;
}

static bool eq_key_update(const TestMsg *x, const TestMsg *y) {
    return x->key_update.update_requested == y->key_update.update_requested;
}

static bool eq_new_session_ticket_tls13(const TestMsg *mx, const TestMsg *my) {
    const TlsNewSessionTicketMsgTLS13 *x = &mx->new_session_ticket_tls13;
    const TlsNewSessionTicketMsgTLS13 *y = &my->new_session_ticket_tls13;
    return x->lifetime == y->lifetime && x->age_add == y->age_add &&
           eq_bytes(x->nonce, y->nonce) && eq_bytes(x->label, y->label) &&
           x->max_early_data == y->max_early_data;
}

static bool eq_certificate_request_tls13(const TestMsg *mx, const TestMsg *my) {
    const TlsCertificateRequestMsgTLS13 *x = &mx->certificate_request_tls13;
    const TlsCertificateRequestMsgTLS13 *y = &my->certificate_request_tls13;
    return x->ocsp_stapling == y->ocsp_stapling && x->scts == y->scts &&
           eq_u16s(x->supported_signature_algorithms,
                   y->supported_signature_algorithms) &&
           eq_u16s(x->supported_signature_algorithms_cert,
                   y->supported_signature_algorithms_cert) &&
           eq_bytes_list(x->certificate_authorities, y->certificate_authorities);
}

static bool eq_certificate_tls13(const TestMsg *mx, const TestMsg *my) {
    const TlsCertificateMsgTLS13 *x = &mx->certificate_tls13;
    const TlsCertificateMsgTLS13 *y = &my->certificate_tls13;
    const TlsCertificate *cx = &x->certificate, *cy = &y->certificate;
    return eq_bytes_list(cx->certificate, cy->certificate) &&
           eq_u16s(cx->supported_signature_algorithms,
                   cy->supported_signature_algorithms) &&
           eq_bytes(cx->ocsp_staple, cy->ocsp_staple) &&
           eq_bytes_list(cx->signed_certificate_timestamps,
                         cy->signed_certificate_timestamps) &&
           cx->leaf == cy->leaf && x->ocsp_stapling == y->ocsp_stapling &&
           x->scts == y->scts;
}

/* -------------------------------------------------------------- Generate */

static Slice random_bytes(Int n, MathRandRand *rand, Alloc *a) {
    Slice r = slice_make(a, TYPE_BYTE, n, n);
    if (r.p == NULL)
        panic_str(BURROW_S("random_bytes: out of memory"));
    Error err = BURROW_NO_ERROR;
    math_rand_rand_read(rand, r, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("rand.Read failed"));
    return r;
}

static Str random_string(Int n, MathRandRand *rand, Alloc *a) {
    Slice b = random_bytes(n, rand, a);
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* *s = append(*s, *v), for one element of size bytes. */
static void append_one(Alloc *a, Slice *s, const void *v, size_t size) {
    Slice r = slice_append_fast(a, *s, v, size);
    if (r.p == NULL)
        panic_str(BURROW_S("append_one: out of memory"));
    *s = r;
}

/* []byte{v} */
static Slice one_byte(Alloc *a, Byte v) {
    Slice b = slice_make(a, TYPE_BYTE, 1, 1);
    if (b.p == NULL)
        panic_str(BURROW_S("one_byte: out of memory"));
    ((Byte *)b.p)[0] = v;
    return b;
}

/* make(T, n), panicking where Go would on running out of memory. */
static Slice make_n(Alloc *a, const Type *elem, Int n) {
    Slice s = slice_make(a, elem, n, n);
    if (s.p == NULL)
        panic_str(BURROW_S("make_n: out of memory"));
    return s;
}

static Int intn(MathRandRand *rand, Int n) {
    return math_rand_rand_intn(rand, n);
}

static void generate_client_hello(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsClientHelloMsg *m = &mm->client_hello;
    *m = burrow__tls_client_hello_msg_zero();
    m->vers = (uint16_t)intn(rand, 65536);
    m->random = random_bytes(32, rand, a);
    m->session_id = random_bytes(intn(rand, 32), rand, a);
    m->cipher_suites = make_n(a, TYPE_UINT16, intn(rand, 63) + 1);
    for (Int i = 0; i < m->cipher_suites.len; i++) {
        uint16_t cs = (uint16_t)math_rand_rand_int31(rand);
        if (cs == TLS_SCSV_RENEGOTIATION)
            cs += 1;
        BURROW_AT(uint16_t, m->cipher_suites, i) = cs;
    }
    m->compression_methods = random_bytes(intn(rand, 63) + 1, rand, a);
    if (intn(rand, 10) > 5) {
        m->server_name = random_string(intn(rand, 255), rand, a);
        while (strings_has_suffix(m->server_name, BURROW_S(".")))
            m->server_name.len--;
    }
    m->ocsp_stapling = intn(rand, 10) > 5;
    m->supported_points = random_bytes(intn(rand, 5) + 1, rand, a);
    m->supported_curves = make_n(a, TYPE_UINT16, intn(rand, 5) + 1);
    for (Int i = 0; i < m->supported_curves.len; i++)
        BURROW_AT(TlsCurveID, m->supported_curves, i) =
            (TlsCurveID)(intn(rand, 30000) + 1);
    if (intn(rand, 10) > 5) {
        m->ticket_supported = true;
        if (intn(rand, 10) > 5)
            m->session_ticket = random_bytes(intn(rand, 300), rand, a);
        else
            m->session_ticket = make_n(a, TYPE_BYTE, 0);
    }
    if (intn(rand, 10) > 5)
        m->supported_signature_algorithms = burrow__tls_supported_signature_algorithms(
            a, TLS_VERSION_TLS12, TLS_VERSION_TLS13);
    if (intn(rand, 10) > 5)
        m->supported_signature_algorithms_cert =
            burrow__tls_supported_signature_algorithms(a, TLS_VERSION_TLS12,
                                                       TLS_VERSION_TLS13);
    for (Int i = 0; i < intn(rand, 5); i++) {
        Str p = random_string(intn(rand, 20) + 1, rand, a);
        append_one(a, &m->alpn_protocols, &p, sizeof(p));
    }
    if (intn(rand, 10) > 5)
        m->scts = true;
    if (intn(rand, 10) > 5) {
        m->secure_renegotiation_supported = true;
        m->secure_renegotiation = random_bytes(intn(rand, 50) + 1, rand, a);
    }
    if (intn(rand, 10) > 5)
        m->extended_master_secret = true;
    for (Int i = 0; i < intn(rand, 5); i++) {
        uint16_t v = (uint16_t)(intn(rand, 0xffff) + 1);
        append_one(a, &m->supported_versions, &v, sizeof(v));
    }
    if (intn(rand, 10) > 5)
        m->cookie = random_bytes(intn(rand, 500) + 1, rand, a);
    for (Int i = 0; i < intn(rand, 5); i++) {
        TlsKeyShare ks;
        ks.group = (TlsCurveID)(intn(rand, 30000) + 1);
        ks.data = random_bytes(intn(rand, 200) + 1, rand, a);
        append_one(a, &m->key_shares, &ks, sizeof(ks));
    }
    switch (intn(rand, 3)) {
    case 1:
        m->psk_modes = one_byte(a, TLS_PSK_MODE_DHE);
        break;
    case 2: {
        Byte plain = TLS_PSK_MODE_PLAIN;
        m->psk_modes = one_byte(a, TLS_PSK_MODE_DHE);
        append_one(a, &m->psk_modes, &plain, 1);
        break;
    }
    default:
        break;
    }
    /* clientHelloMsg.marshal uses echInner == false. If m.encryptedClientHello
     * > 0 and does not equal []byte{innerECHExt}, then the psk extension will
     * be omitted, so either only emit empty encryptedClientHello and psk,
     * encryptedClientHello with []byte{innerECHExt} and psk, or random
     * encryptedClientHello and no psk. */
    if (intn(rand, 10) > 5) {
        m->encrypted_client_hello = random_bytes(intn(rand, 50) + 1, rand, a);
    } else {
        if (intn(rand, 10) > 5)
            m->encrypted_client_hello = one_byte(a, TLS_INNER_ECH_EXT);
        TlsPskIdentity psk;
        psk.obfuscated_ticket_age = (uint32_t)intn(rand, 500000);
        psk.label = random_bytes(intn(rand, 500) + 1, rand, a);
        append_one(a, &m->psk_identities, &psk, sizeof(psk));
        Slice binder = random_bytes(intn(rand, 50) + 32, rand, a);
        append_one(a, &m->psk_binders, &binder, sizeof(binder));
    }
    if (intn(rand, 10) > 5)
        m->quic_transport_parameters = random_bytes(intn(rand, 500), rand, a);
    if (intn(rand, 10) > 5)
        m->early_data = true;
}

static void generate_server_hello(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsServerHelloMsg *m = &mm->server_hello;
    *m = burrow__tls_server_hello_msg_zero();
    m->vers = (uint16_t)intn(rand, 65536);
    m->random = random_bytes(32, rand, a);
    m->session_id = random_bytes(intn(rand, 32), rand, a);
    m->cipher_suite = (uint16_t)math_rand_rand_int31(rand);
    m->compression_method = (uint8_t)intn(rand, 256);
    m->supported_points = random_bytes(intn(rand, 5) + 1, rand, a);

    if (intn(rand, 10) > 5)
        m->ocsp_stapling = true;
    if (intn(rand, 10) > 5)
        m->ticket_supported = true;
    if (intn(rand, 10) > 5)
        m->alpn_protocol = random_string(intn(rand, 32) + 1, rand, a);

    for (Int i = 0; i < intn(rand, 4); i++) {
        Slice sct = random_bytes(intn(rand, 500) + 1, rand, a);
        append_one(a, &m->scts, &sct, sizeof(sct));
    }

    if (intn(rand, 10) > 5) {
        m->secure_renegotiation_supported = true;
        m->secure_renegotiation = random_bytes(intn(rand, 50) + 1, rand, a);
    }
    if (intn(rand, 10) > 5)
        m->extended_master_secret = true;
    if (intn(rand, 10) > 5)
        m->supported_version = (uint16_t)(intn(rand, 0xffff) + 1);
    if (intn(rand, 10) > 5)
        m->cookie = random_bytes(intn(rand, 500) + 1, rand, a);
    if (intn(rand, 10) > 5) {
        for (Int i = 0; i < intn(rand, 5); i++) {
            m->server_share.group = (TlsCurveID)(intn(rand, 30000) + 1);
            m->server_share.data = random_bytes(intn(rand, 200) + 1, rand, a);
        }
    } else if (intn(rand, 10) > 5) {
        m->selected_group = (TlsCurveID)(intn(rand, 30000) + 1);
    }
    if (intn(rand, 10) > 5) {
        m->selected_identity_present = true;
        m->selected_identity = (uint16_t)intn(rand, 0xffff);
    }
    if (intn(rand, 10) > 5)
        m->encrypted_client_hello = random_bytes(intn(rand, 50) + 1, rand, a);
    if (intn(rand, 10) > 5)
        m->server_name_ack = intn(rand, 2) == 1;
}

static void generate_encrypted_extensions(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsEncryptedExtensionsMsg *m = &mm->encrypted_extensions;
    *m = burrow__tls_encrypted_extensions_msg_zero();

    if (intn(rand, 10) > 5)
        m->alpn_protocol = random_string(intn(rand, 32) + 1, rand, a);
    if (intn(rand, 10) > 5)
        m->early_data = true;
}

static void generate_certificate(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsCertificateMsg *m = &mm->certificate;
    Int num_certs = intn(rand, 20);
    m->certificates = make_n(a, TYPE_BYTES, num_certs);
    for (Int i = 0; i < num_certs; i++)
        BURROW_AT(Slice, m->certificates, i) =
            random_bytes(intn(rand, 10) + 1, rand, a);
}

static void generate_certificate_request(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsCertificateRequestMsg *m = &mm->certificate_request;
    *m = burrow__tls_certificate_request_msg_zero();
    m->certificate_types = random_bytes(intn(rand, 5) + 1, rand, a);
    for (Int i = 0; i < intn(rand, 100); i++) {
        Slice ca = random_bytes(intn(rand, 15) + 1, rand, a);
        append_one(a, &m->certificate_authorities, &ca, sizeof(ca));
    }
}

static void generate_certificate_verify(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsCertificateVerifyMsg *m = &mm->certificate_verify;
    m->has_signature_algorithm = true;
    m->signature_algorithm = (TlsSignatureScheme)intn(rand, 30000);
    m->signature = random_bytes(intn(rand, 15) + 1, rand, a);
}

static void generate_certificate_status(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    mm->certificate_status.response = random_bytes(intn(rand, 10) + 1, rand, a);
}

static void generate_client_key_exchange(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    mm->client_key_exchange.ciphertext = random_bytes(intn(rand, 1000) + 1, rand, a);
}

static void generate_finished(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    mm->finished.verify_data = random_bytes(12, rand, a);
}

static void generate_new_session_ticket(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    mm->new_session_ticket.ticket = random_bytes(intn(rand, 4), rand, a);
}

static void generate_end_of_early_data(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    (void)mm;
    (void)rand;
    (void)a;
}

static void generate_key_update(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    (void)a;
    mm->key_update.update_requested = intn(rand, 10) > 5;
}

static void generate_new_session_ticket_tls13(TestMsg *mm, MathRandRand *rand,
                                              Alloc *a) {
    TlsNewSessionTicketMsgTLS13 *m = &mm->new_session_ticket_tls13;
    *m = burrow__tls_new_session_ticket_msg_tls13_zero();
    m->lifetime = (uint32_t)intn(rand, 500000);
    m->age_add = (uint32_t)intn(rand, 500000);
    m->nonce = random_bytes(intn(rand, 100), rand, a);
    m->label = random_bytes(intn(rand, 1000), rand, a);
    if (intn(rand, 10) > 5)
        m->max_early_data = (uint32_t)intn(rand, 500000);
}

static void generate_certificate_request_tls13(TestMsg *mm, MathRandRand *rand,
                                               Alloc *a) {
    TlsCertificateRequestMsgTLS13 *m = &mm->certificate_request_tls13;
    *m = burrow__tls_certificate_request_msg_tls13_zero();
    if (intn(rand, 10) > 5)
        m->ocsp_stapling = true;
    if (intn(rand, 10) > 5)
        m->scts = true;
    if (intn(rand, 10) > 5)
        m->supported_signature_algorithms = burrow__tls_supported_signature_algorithms(
            a, TLS_VERSION_TLS12, TLS_VERSION_TLS13);
    if (intn(rand, 10) > 5)
        m->supported_signature_algorithms_cert =
            burrow__tls_supported_signature_algorithms(a, TLS_VERSION_TLS12,
                                                       TLS_VERSION_TLS13);
    if (intn(rand, 10) > 5) {
        m->certificate_authorities = make_n(a, TYPE_BYTES, 3);
        for (Int i = 0; i < 3; i++)
            BURROW_AT(Slice, m->certificate_authorities, i) =
                random_bytes(intn(rand, 10) + 1, rand, a);
    }
}

static void generate_certificate_tls13(TestMsg *mm, MathRandRand *rand, Alloc *a) {
    TlsCertificateMsgTLS13 *m = &mm->certificate_tls13;
    *m = burrow__tls_certificate_msg_tls13_zero();
    for (Int i = 0; i < intn(rand, 2) + 1; i++) {
        Slice cert = random_bytes(intn(rand, 500) + 1, rand, a);
        append_one(a, &m->certificate.certificate, &cert, sizeof(cert));
    }
    if (intn(rand, 10) > 5) {
        m->ocsp_stapling = true;
        m->certificate.ocsp_staple = random_bytes(intn(rand, 100) + 1, rand, a);
    }
    if (intn(rand, 10) > 5) {
        m->scts = true;
        for (Int i = 0; i < intn(rand, 2) + 1; i++) {
            Slice sct = random_bytes(intn(rand, 500) + 1, rand, a);
            append_one(a, &m->certificate.signed_certificate_timestamps, &sct,
                       sizeof(sct));
        }
    }
}

/* ------------------------------------------------- the handshakeMessages */

static void init_client_hello(TestMsg *m) {
    m->client_hello = burrow__tls_client_hello_msg_zero();
}

static void init_server_hello(TestMsg *m) {
    m->server_hello = burrow__tls_server_hello_msg_zero();
}

static void init_finished(TestMsg *m) {
    m->finished.verify_data = slice_nil(TYPE_BYTE);
}

static void init_certificate(TestMsg *m) {
    m->certificate.certificates = slice_nil(TYPE_BYTES);
}

static void init_certificate_request(TestMsg *m) {
    m->certificate_request = burrow__tls_certificate_request_msg_zero();
}

static void init_certificate_verify(TestMsg *m) {
    m->certificate_verify.has_signature_algorithm = true;
    m->certificate_verify.signature_algorithm = 0;
    m->certificate_verify.signature = slice_nil(TYPE_BYTE);
}

static void init_certificate_status(TestMsg *m) {
    m->certificate_status.response = slice_nil(TYPE_BYTE);
}

static void init_client_key_exchange(TestMsg *m) {
    m->client_key_exchange.ciphertext = slice_nil(TYPE_BYTE);
}

static void init_new_session_ticket(TestMsg *m) {
    m->new_session_ticket.ticket = slice_nil(TYPE_BYTE);
}

static void init_encrypted_extensions(TestMsg *m) {
    m->encrypted_extensions = burrow__tls_encrypted_extensions_msg_zero();
}

static void init_end_of_early_data(TestMsg *m) {
    (void)m;
}

static void init_key_update(TestMsg *m) {
    m->key_update.update_requested = false;
}

static void init_new_session_ticket_tls13(TestMsg *m) {
    m->new_session_ticket_tls13 = burrow__tls_new_session_ticket_msg_tls13_zero();
}

static void init_certificate_request_tls13(TestMsg *m) {
    m->certificate_request_tls13 = burrow__tls_certificate_request_msg_tls13_zero();
}

static void init_certificate_tls13(TestMsg *m) {
    m->certificate_tls13 = burrow__tls_certificate_msg_tls13_zero();
}

static Slice marshal_client_hello(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_client_hello_msg_marshal(&m->client_hello, a, err);
}

static bool unmarshal_client_hello(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_client_hello_msg_unmarshal(&m->client_hello, a, data);
}

static Slice marshal_server_hello(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_server_hello_msg_marshal(&m->server_hello, a, err);
}

static bool unmarshal_server_hello(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_server_hello_msg_unmarshal(&m->server_hello, a, data);
}

static Slice marshal_finished(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_finished_msg_marshal(&m->finished, a, err);
}

static bool unmarshal_finished(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_finished_msg_unmarshal(&m->finished, data);
}

static Slice marshal_certificate(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_msg_marshal(&m->certificate, a, err);
}

static bool unmarshal_certificate(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_certificate_msg_unmarshal(&m->certificate, a, data);
}

static Slice marshal_certificate_request(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_request_msg_marshal(&m->certificate_request, a, err);
}

static bool unmarshal_certificate_request(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_certificate_request_msg_unmarshal(&m->certificate_request, a,
                                                         data);
}

static Slice marshal_certificate_verify(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_verify_msg_marshal(&m->certificate_verify, a, err);
}

static bool unmarshal_certificate_verify(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_certificate_verify_msg_unmarshal(&m->certificate_verify, data);
}

static Slice marshal_certificate_status(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_status_msg_marshal(&m->certificate_status, a, err);
}

static bool unmarshal_certificate_status(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_certificate_status_msg_unmarshal(&m->certificate_status, data);
}

static Slice marshal_client_key_exchange(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_client_key_exchange_msg_marshal(&m->client_key_exchange, a, err);
}

static bool unmarshal_client_key_exchange(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_client_key_exchange_msg_unmarshal(&m->client_key_exchange, data);
}

static Slice marshal_new_session_ticket(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_new_session_ticket_msg_marshal(&m->new_session_ticket, a, err);
}

static bool unmarshal_new_session_ticket(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_new_session_ticket_msg_unmarshal(&m->new_session_ticket, data);
}

static Slice marshal_encrypted_extensions(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_encrypted_extensions_msg_marshal(&m->encrypted_extensions, a,
                                                        err);
}

static bool unmarshal_encrypted_extensions(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_encrypted_extensions_msg_unmarshal(&m->encrypted_extensions, a,
                                                          data);
}

static Slice marshal_end_of_early_data(const TestMsg *m, Alloc *a, Error *err) {
    (void)m;
    return burrow__tls_end_of_early_data_msg_marshal(a, err);
}

static bool unmarshal_end_of_early_data(TestMsg *m, Alloc *a, Slice data) {
    (void)m;
    (void)a;
    return burrow__tls_end_of_early_data_msg_unmarshal(data);
}

static Slice marshal_key_update(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_key_update_msg_marshal(&m->key_update, a, err);
}

static bool unmarshal_key_update(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_key_update_msg_unmarshal(&m->key_update, data);
}

static Slice marshal_new_session_ticket_tls13(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_new_session_ticket_msg_tls13_marshal(
        &m->new_session_ticket_tls13, a, err);
}

static bool unmarshal_new_session_ticket_tls13(TestMsg *m, Alloc *a, Slice data) {
    (void)a;
    return burrow__tls_new_session_ticket_msg_tls13_unmarshal(
        &m->new_session_ticket_tls13, data);
}

static Slice marshal_certificate_request_tls13(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_request_msg_tls13_marshal(
        &m->certificate_request_tls13, a, err);
}

static bool unmarshal_certificate_request_tls13(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_certificate_request_msg_tls13_unmarshal(
        &m->certificate_request_tls13, a, data);
}

static Slice marshal_certificate_tls13(const TestMsg *m, Alloc *a, Error *err) {
    return burrow__tls_certificate_msg_tls13_marshal(&m->certificate_tls13, a, err);
}

static bool unmarshal_certificate_tls13(TestMsg *m, Alloc *a, Slice data) {
    return burrow__tls_certificate_msg_tls13_unmarshal(&m->certificate_tls13, a, data);
}

#define MSG_KIND(name, f)                                                              \
    {name, init_##f, generate_##f, marshal_##f, unmarshal_##f, eq_##f}

static const MsgKind tests[] = {
    MSG_KIND("*tls.clientHelloMsg", client_hello),
    MSG_KIND("*tls.serverHelloMsg", server_hello),
    MSG_KIND("*tls.finishedMsg", finished),

    MSG_KIND("*tls.certificateMsg", certificate),
    MSG_KIND("*tls.certificateRequestMsg", certificate_request),
    MSG_KIND("*tls.certificateVerifyMsg", certificate_verify),
    MSG_KIND("*tls.certificateStatusMsg", certificate_status),
    MSG_KIND("*tls.clientKeyExchangeMsg", client_key_exchange),
    MSG_KIND("*tls.newSessionTicketMsg", new_session_ticket),
    MSG_KIND("*tls.encryptedExtensionsMsg", encrypted_extensions),
    MSG_KIND("*tls.endOfEarlyDataMsg", end_of_early_data),
    MSG_KIND("*tls.keyUpdateMsg", key_update),
    MSG_KIND("*tls.newSessionTicketMsgTLS13", new_session_ticket_tls13),
    MSG_KIND("*tls.certificateRequestMsgTLS13", certificate_request_tls13),
    MSG_KIND("*tls.certificateMsgTLS13", certificate_tls13),
};

static Slice must_marshal(TestingT *t, const MsgKind *k, const TestMsg *msg, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice b = k->marshal(msg, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    return b;
}

static Str as_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* --------------------------------------------------------------- the tests */

/* What a TestMarshalUnmarshal subtest closes over: Go's i, m and rand. */
typedef struct MarshalUnmarshalEnv {
    Int i;
    TestMsg *m;
    MathRandRand *rand;
    Alloc *a;
} MarshalUnmarshalEnv;

static void marshal_unmarshal(void *envp, TestingT *t) {
    MarshalUnmarshalEnv *env = (MarshalUnmarshalEnv *)envp;
    Int i = env->i;
    const MsgKind *k = &tests[i];
    TestMsg *m = env->m;
    Alloc *a = env->a;

    int n = 100;
    if (testing_short())
        n = 5;
    for (int j = 0; j < n; j++) {
        TestMsg m1;
        memset(&m1, 0, sizeof(m1));
        k->generate(&m1, env->rand, a);

        Slice marshaled = must_marshal(t, k, &m1, a);
        if (!k->unmarshal(m, a, marshaled)) {
            testing_t_errorf_v(t, "#%d failed to unmarshal %x", i, as_str(marshaled));
            break;
        }

        if (i == 0) {
            /* extensions is special cased, as it is only populated by the
             * server-side of a handshake and is not expected to roundtrip
             * through marshal + unmarshal.  m ends up with the list of
             * extensions necessary to serialize the other fields of
             * clientHelloMsg, so check that it is non-empty, then clear it. */
            if (m->client_hello.extensions.len == 0)
                testing_t_errorf_v(
                    t, "expected ch.extensions to be populated on unmarshal");
            m->client_hello.extensions = slice_nil(TYPE_UINT16);
        }

        /* clientHelloMsg and serverHelloMsg, when unmarshalled, store their
         * original representation, for later use in the handshake transcript.
         * In order to prevent DeepEqual from failing since we didn't create the
         * original message via unmarshalling, nil the field. */
        if (i == 0)
            m->client_hello.original = slice_nil(TYPE_BYTE);
        else if (i == 1)
            m->server_hello.original = slice_nil(TYPE_BYTE);

        if (!k->equal(&m1, m)) {
            testing_t_errorf_v(t, "#%d got a different message back from %x", i,
                               as_str(marshaled));
            break;
        }

        if (i >= 3) {
            /* The first three message types (ClientHello, ServerHello and
             * Finished) are allowed to have parsable prefixes because the
             * extension data is optional and the length of the Finished varies
             * across versions. */
            for (Int jj = 0; jj < marshaled.len; jj++) {
                if (k->unmarshal(m, a, slice_sub(marshaled, 0, jj))) {
                    testing_t_errorf_v(t, "#%d unmarshaled a prefix of length %d", i,
                                       jj);
                    break;
                }
            }
        }
    }
}

/* Go's tests table holds one message of each type, which TestMarshalUnmarshal
 * and TestFuzz both unmarshal into. */
static void init_messages(TestMsg *msgs) {
    memset(msgs, 0, NELEM(tests) * sizeof(TestMsg));
    for (size_t k = 0; k < NELEM(tests); k++)
        tests[k].init(&msgs[k]);
}

static void TestMarshalUnmarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *rand =
        math_rand_new(a, math_rand_new_source(a, time_unix_nano(time_now())));
    TestMsg msgs[NELEM(tests)];
    init_messages(msgs);

    for (Int i = 0; i < (Int)NELEM(tests); i++) {
        MarshalUnmarshalEnv env = {i, &msgs[i], rand, a};
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, marshal_unmarshal, &env));
    }
    arena_free(&ar);
}

static void TestFuzz(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *rand = math_rand_new(a, math_rand_new_source(a, 0));
    TestMsg msgs[NELEM(tests)];
    init_messages(msgs);
    for (size_t k = 0; k < NELEM(tests); k++) {
        for (int j = 0; j < 1000; j++) {
            Int len = intn(rand, 1000);
            Slice bytes = random_bytes(len, rand, a);
            /* This just looks for crashes due to bounds errors etc. */
            tests[k].unmarshal(&msgs[k], a, bytes);
        }
    }
    arena_free(&ar);
}

static void TestRejectEmptySCTList(TestingT *t) {
    /* RFC 6962, Section 3.3.1 specifies that empty SCT lists are invalid. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Byte random[32] = {0};
    Byte sct_bytes[] = {0x42, 0x42, 0x42, 0x42};
    Slice sct = slice_from(sct_bytes, 4, 4, TYPE_BYTE);
    TestMsg server_hello;
    server_hello.server_hello = burrow__tls_server_hello_msg_zero();
    server_hello.server_hello.vers = TLS_VERSION_TLS12;
    server_hello.server_hello.random = slice_from(random, 32, 32, TYPE_BYTE);
    server_hello.server_hello.scts = make_n(a, TYPE_BYTES, 1);
    BURROW_AT(Slice, server_hello.server_hello.scts, 0) = sct;
    Slice server_hello_bytes = must_marshal(t, &tests[1], &server_hello, a);

    TlsServerHelloMsg server_hello_copy = burrow__tls_server_hello_msg_zero();
    if (!burrow__tls_server_hello_msg_unmarshal(&server_hello_copy, a,
                                                server_hello_bytes))
        testing_t_fatalf_v(t, "Failed to unmarshal initial message");

    /* Change serverHelloBytes so that the SCT list is empty */
    Int i = bytes_index(server_hello_bytes, sct);
    if (i < 0)
        testing_t_fatalf_v(t, "Cannot find SCT in ServerHello");

    Slice server_hello_empty_sct = slice_nil(TYPE_BYTE);
    server_hello_empty_sct =
        slice_append(a, server_hello_empty_sct, server_hello_bytes.p, i - 6);
    /* Append the extension length and SCT list length for an empty list. */
    static const Byte empty_list[] = {0, 2, 0, 0};
    server_hello_empty_sct = slice_append(a, server_hello_empty_sct, empty_list, 4);
    server_hello_empty_sct = slice_append(a, server_hello_empty_sct,
                                          (const Byte *)server_hello_bytes.p + i + 4,
                                          server_hello_bytes.len - i - 4);
    CHECK(server_hello_empty_sct.p != NULL);
    Byte *e = (Byte *)server_hello_empty_sct.p;
    Int n = server_hello_empty_sct.len;

    /* Update the handshake message length. */
    e[1] = (Byte)((n - 4) >> 16);
    e[2] = (Byte)((n - 4) >> 8);
    e[3] = (Byte)(n - 4);

    /* Update the extensions length */
    e[42] = (Byte)((n - 44) >> 8);
    e[43] = (Byte)(n - 44);

    if (burrow__tls_server_hello_msg_unmarshal(&server_hello_copy, a,
                                               server_hello_empty_sct))
        testing_t_fatalf_v(t, "Unmarshaled ServerHello with empty SCT list");
    arena_free(&ar);
}

static void TestRejectEmptySCT(TestingT *t) {
    /* Not only must the SCT list be non-empty, but the SCT elements must not be
     * zero length. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Byte random[32] = {0};
    TestMsg server_hello;
    server_hello.server_hello = burrow__tls_server_hello_msg_zero();
    server_hello.server_hello.vers = TLS_VERSION_TLS12;
    server_hello.server_hello.random = slice_from(random, 32, 32, TYPE_BYTE);
    server_hello.server_hello.scts = make_n(a, TYPE_BYTES, 1);
    BURROW_AT(Slice, server_hello.server_hello.scts, 0) = slice_nil(TYPE_BYTE);
    Slice server_hello_bytes = must_marshal(t, &tests[1], &server_hello, a);

    TlsServerHelloMsg server_hello_copy = burrow__tls_server_hello_msg_zero();
    if (burrow__tls_server_hello_msg_unmarshal(&server_hello_copy, a,
                                               server_hello_bytes))
        testing_t_fatalf_v(t, "Unmarshaled ServerHello with zero-length SCT");
    arena_free(&ar);
}

static void TestRejectDuplicateExtensions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice client_hello_bytes = hex_decode_string(
        a,
        BURROW_S("0100004403030000000000000000000000000000000000000000000000000000"
                 "00000000000000000000001c0000000a000800000568656c6c6f0000000a0008"
                 "00000568656c6c6f"),
        &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to decode test ClientHello: %s", err);
    TlsClientHelloMsg client_hello_copy = burrow__tls_client_hello_msg_zero();
    if (burrow__tls_client_hello_msg_unmarshal(&client_hello_copy, a,
                                               client_hello_bytes))
        testing_t_errorf_v(t, "Unmarshaled ClientHello with duplicate extensions");

    Slice server_hello_bytes = hex_decode_string(
        a,
        BURROW_S("0200003003030000000000000000000000000000000000000000000000000000"
                 "0000000000000000000000080005000000050000"),
        &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to decode test ServerHello: %s", err);
    TlsServerHelloMsg server_hello_copy = burrow__tls_server_hello_msg_zero();
    if (burrow__tls_server_hello_msg_unmarshal(&server_hello_copy, a,
                                               server_hello_bytes))
        testing_t_fatalf_v(t, "Unmarshaled ServerHello with duplicate extensions");
    arena_free(&ar);
}

typedef struct ECHRemoveOuterPSKCase {
    const char *name;
    int ech_ext; /* the one byte of echExt, or -1 for a nil echExt */
    bool ech_inner;
    bool expect_removed;
} ECHRemoveOuterPSKCase;

static const ECHRemoveOuterPSKCase ech_remove_outer_psk_cases[] = {
    {"echInner true", -1, true, false},
    {"echInner true, no ech ext", -1, true, false},
    {"echInner true, ech ext present", 254, true, false},
    {"echInner true, ech ext present, inner ech sentinel", TLS_INNER_ECH_EXT, true,
     false},
    {"echInner false, no ech ext", -1, false, false},
    {"echInner false, ech ext present", 254, false, true},
    {"echInner false, ech ext present, inner ech sentinel", TLS_INNER_ECH_EXT, false,
     false},
};

typedef struct ECHRemoveOuterPSKEnv {
    const ECHRemoveOuterPSKCase *tc;
    MathRandRand *r;
    Alloc *a;
} ECHRemoveOuterPSKEnv;

static void ech_remove_outer_psk(void *envp, TestingT *t) {
    ECHRemoveOuterPSKEnv *env = (ECHRemoveOuterPSKEnv *)envp;
    const ECHRemoveOuterPSKCase *tc = env->tc;
    Alloc *a = env->a;
    TestMsg msg;
    generate_client_hello(&msg, env->r, a);
    TlsClientHelloMsg *ch = &msg.client_hello;

    static const Byte test[] = {'t', 'e', 's', 't'};
    Slice binder = make_n(a, TYPE_BYTE, 4);
    memcpy(binder.p, test, 4);
    Slice label = make_n(a, TYPE_BYTE, 4);
    memcpy(label.p, test, 4);
    ch->psk_binders = make_n(a, TYPE_BYTES, 1);
    BURROW_AT(Slice, ch->psk_binders, 0) = binder;
    ch->psk_identities = make_n(a, &burrow__tls_psk_identity_type, 1);
    BURROW_AT(TlsPskIdentity, ch->psk_identities, 0) = (TlsPskIdentity){label, 0};
    ch->encrypted_client_hello =
        tc->ech_ext < 0 ? slice_nil(TYPE_BYTE) : one_byte(a, (Byte)tc->ech_ext);

    Error err = BURROW_NO_ERROR;
    Slice b = burrow__tls_client_hello_msg_marshal_msg(ch, a, tc->ech_inner, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    TlsClientHelloMsg rch = burrow__tls_client_hello_msg_zero();
    if (!burrow__tls_client_hello_msg_unmarshal(&rch, a, b))
        testing_t_fatalf_v(t, "Failed to unmarshal ClientHello");

    if (tc->expect_removed) {
        if (!slice_is_nil(rch.psk_identities))
            testing_t_errorf_v(t, "expected PSK identities to be removed");
        if (!slice_is_nil(rch.psk_binders))
            testing_t_errorf_v(t, "expected PSK binders to be removed");
    } else {
        if (slice_is_nil(rch.psk_identities))
            testing_t_errorf_v(t, "expected PSK identities to be present");
        if (slice_is_nil(rch.psk_binders))
            testing_t_errorf_v(t, "expected PSK binders to be present");
    }
}

static void TestECHRemoveOuterPSK(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 0));

    for (size_t i = 0; i < NELEM(ech_remove_outer_psk_cases); i++) {
        ECHRemoveOuterPSKEnv env = {&ech_remove_outer_psk_cases[i], r, a};
        testing_t_run(t, str_from_cstr(ech_remove_outer_psk_cases[i].name),
                      BURROW_FN(TestingTFunc, ech_remove_outer_psk, &env));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshalUnmarshal)                                                            \
    X(TestFuzz)                                                                        \
    X(TestRejectEmptySCTList)                                                          \
    X(TestRejectEmptySCT)                                                              \
    X(TestRejectDuplicateExtensions)                                                   \
    X(TestECHRemoveOuterPSK)

TESTING_MAIN(TESTS)
