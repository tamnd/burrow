/* crypto/tls, TLS 1.0 to 1.3 as RFC 5246 and RFC 8446 have them.
 *
 *     Slice suites = tls_cipher_suites(a);
 *     for (Int i = 0; i < suites.len; i++) {
 *         TlsCipherSuite *s = ((TlsCipherSuitePtr *)suites.p)[i];
 *         printf("%04x " BURROW_STR_FMT "\n", s->id, BURROW_STR_ARG(s->name));
 *     }
 *
 * This is the part of the package that has no connection in it yet: the
 * protocol versions, the cipher suites and their names, the curves and
 * signature schemes a handshake can agree on, and the alerts. Everything else
 * Go's crypto/tls has is being added on top of it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/tls */

#ifndef BURROW_CRYPTO_TLS_H
#define BURROW_CRYPTO_TLS_H

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- versions */

#define TLS_VERSION_TLS10 0x0301
#define TLS_VERSION_TLS11 0x0302
#define TLS_VERSION_TLS12 0x0303
#define TLS_VERSION_TLS13 0x0304

/* Deprecated: SSLv3 is cryptographically broken, and is no longer supported by
 * this package. See golang.org/issue/32716. */
#define TLS_VERSION_SSL30 0x0300

/* VersionName: the name of a TLS version, such as "TLS 1.3", or a fallback
 * like "0x0305" from a for one it does not know. */
BURROW_OWNS(ret) Str tls_version_name(Alloc *a, uint16_t version);

/* ----------------------------------------------------------- cipher suites */

/* A list of cipher suite IDs that are, or have been, implemented by this
 * package.
 *
 * See https://www.iana.org/assignments/tls-parameters/tls-parameters.xml */

/* TLS 1.0 - 1.2 cipher suites. */
#define TLS_TLS_RSA_WITH_RC4_128_SHA ((uint16_t)0x0005)
#define TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA ((uint16_t)0x000a)
#define TLS_TLS_RSA_WITH_AES_128_CBC_SHA ((uint16_t)0x002f)
#define TLS_TLS_RSA_WITH_AES_256_CBC_SHA ((uint16_t)0x0035)
#define TLS_TLS_RSA_WITH_AES_128_CBC_SHA256 ((uint16_t)0x003c)
#define TLS_TLS_RSA_WITH_AES_128_GCM_SHA256 ((uint16_t)0x009c)
#define TLS_TLS_RSA_WITH_AES_256_GCM_SHA384 ((uint16_t)0x009d)
#define TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA ((uint16_t)0xc007)
#define TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA ((uint16_t)0xc009)
#define TLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA ((uint16_t)0xc00a)
#define TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA ((uint16_t)0xc011)
#define TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA ((uint16_t)0xc012)
#define TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA ((uint16_t)0xc013)
#define TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA ((uint16_t)0xc014)
#define TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 ((uint16_t)0xc023)
#define TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256 ((uint16_t)0xc027)
#define TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 ((uint16_t)0xc02f)
#define TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 ((uint16_t)0xc02b)
#define TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 ((uint16_t)0xc030)
#define TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 ((uint16_t)0xc02c)
#define TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 ((uint16_t)0xcca8)
#define TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 ((uint16_t)0xcca9)

/* TLS 1.3 cipher suites. */
#define TLS_TLS_AES_128_GCM_SHA256 ((uint16_t)0x1301)
#define TLS_TLS_AES_256_GCM_SHA384 ((uint16_t)0x1302)
#define TLS_TLS_CHACHA20_POLY1305_SHA256 ((uint16_t)0x1303)

/* TLS_FALLBACK_SCSV isn't a standard cipher suite but an indicator that the
 * client is doing version fallback. See RFC 7507. */
#define TLS_TLS_FALLBACK_SCSV ((uint16_t)0x5600)

/* Legacy names for the corresponding cipher suites with the correct _SHA256
 * suffix, retained for backward compatibility. */
#define TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305                                       \
    TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
#define TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305                                     \
    TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256

/* CipherSuite is a TLS cipher suite. Note that most functions in this package
 * accept and expose cipher suite IDs instead of this type. */
typedef struct TlsCipherSuite {
    uint16_t id;
    Str name;

    /* Supported versions is the list of TLS protocol versions that can
     * negotiate this cipher suite. A Slice of uint16_t. */
    Slice supported_versions;

    /* Insecure is true if the cipher suite has known security issues due to
     * its primitives, design, or implementation. */
    bool insecure;
} TlsCipherSuite;

extern const Type burrow_type_TlsCipherSuite;
#define TYPE_TLS_CIPHER_SUITE TYPE_OF(TlsCipherSuite)

BURROW_PTR_TYPE_DECL(TlsCipherSuitePtr, TlsCipherSuite);
#define TYPE_TLS_CIPHER_SUITE_PTR TYPE_OF(TlsCipherSuitePtr)

/* CipherSuites returns a list of cipher suites currently implemented by this
 * package, excluding those with security issues, which are returned by
 * tls_insecure_cipher_suites.
 *
 * The list is sorted by ID. Note that the default cipher suites selected by
 * this package might depend on logic that can't be captured by a static list,
 * and might not match those returned by this function.
 *
 * A Slice of TlsCipherSuitePtr, with the suites and the slice from a and a
 * fresh copy each call, as in Go. Their supported_versions are shared. A nil
 * slice when a is out of memory. */
BURROW_OWNS(ret) Slice tls_cipher_suites(Alloc *a);

/* InsecureCipherSuites returns a list of cipher suites currently implemented
 * by this package and which have security issues.
 *
 * Most applications should not use the cipher suites in this list, and should
 * only use those returned by tls_cipher_suites. */
BURROW_OWNS(ret) Slice tls_insecure_cipher_suites(Alloc *a);

/* CipherSuiteName returns the standard name for the passed cipher suite ID
 * (e.g. "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"), or a fallback
 * representation of the ID value if the cipher suite is not implemented by
 * this package. Only the fallback is allocated, from a. */
BURROW_OWNS(ret) Str tls_cipher_suite_name(Alloc *a, uint16_t id);

/* ------------------------------------------------------------------ curves */

/* CurveID is the type of a TLS identifier for a key exchange mechanism. See
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xml#tls-parameters-8.
 *
 * In TLS 1.2, this registry used to support only elliptic curves. In TLS 1.3,
 * it was extended to other groups and renamed NamedGroup. See RFC 8446,
 * Section 4.2.7. It was then also extended to other mechanisms, such as hybrid
 * post-quantum KEMs. */
typedef uint16_t TlsCurveID;

#define TLS_CURVE_P256 ((TlsCurveID)23)
#define TLS_CURVE_P384 ((TlsCurveID)24)
#define TLS_CURVE_P521 ((TlsCurveID)25)
#define TLS_X25519 ((TlsCurveID)29)
#define TLS_X25519MLKEM768 ((TlsCurveID)4588)
#define TLS_SEC_P256_R1_MLKEM768 ((TlsCurveID)4587)
#define TLS_SEC_P384_R1_MLKEM1024 ((TlsCurveID)4589)
#define TLS_MLKEM1024 ((TlsCurveID)514)

/* CurveID.String: Go's name for the constant, such as "X25519MLKEM768", or
 * "CurveID(7)" from a. */
BURROW_OWNS(ret) Str tls_curve_id_string(TlsCurveID i, Alloc *a);

/* ------------------------------------------------------- signature schemes */

/* SignatureScheme identifies a signature algorithm supported by TLS. See RFC
 * 8446, Section 4.2.3. */
typedef uint16_t TlsSignatureScheme;

/* RSASSA-PKCS1-v1_5 algorithms. */
#define TLS_PKCS1_WITH_SHA256 ((TlsSignatureScheme)0x0401)
#define TLS_PKCS1_WITH_SHA384 ((TlsSignatureScheme)0x0501)
#define TLS_PKCS1_WITH_SHA512 ((TlsSignatureScheme)0x0601)

/* RSASSA-PSS algorithms with public key OID rsaEncryption. */
#define TLS_PSS_WITH_SHA256 ((TlsSignatureScheme)0x0804)
#define TLS_PSS_WITH_SHA384 ((TlsSignatureScheme)0x0805)
#define TLS_PSS_WITH_SHA512 ((TlsSignatureScheme)0x0806)

/* ECDSA algorithms. Only constrained to a specific curve in TLS 1.3. */
#define TLS_ECDSA_WITH_P256_AND_SHA256 ((TlsSignatureScheme)0x0403)
#define TLS_ECDSA_WITH_P384_AND_SHA384 ((TlsSignatureScheme)0x0503)
#define TLS_ECDSA_WITH_P521_AND_SHA512 ((TlsSignatureScheme)0x0603)

/* EdDSA algorithms. */
#define TLS_ED25519 ((TlsSignatureScheme)0x0807)

/* ML-DSA algorithms. */
#define TLS_MLDSA44 ((TlsSignatureScheme)0x0904)
#define TLS_MLDSA65 ((TlsSignatureScheme)0x0905)
#define TLS_MLDSA87 ((TlsSignatureScheme)0x0906)

/* Legacy signature and hash algorithms for TLS 1.2. */
#define TLS_PKCS1_WITH_SHA1 ((TlsSignatureScheme)0x0201)
#define TLS_ECDSA_WITH_SHA1 ((TlsSignatureScheme)0x0203)

/* SignatureScheme.String: Go's name for the constant, such as
 * "PSSWithSHA256", or "SignatureScheme(7)" from a. */
BURROW_OWNS(ret) Str tls_signature_scheme_string(TlsSignatureScheme i, Alloc *a);

/* ---------------------------------------------------- client auth policies */

/* ClientAuthType declares the policy the server will follow for TLS Client
 * Authentication. */
typedef Int TlsClientAuthType;

/* NoClientCert indicates that no client certificate should be requested
 * during the handshake, and if any certificates are sent they will not be
 * verified. */
#define TLS_NO_CLIENT_CERT ((TlsClientAuthType)0)
/* RequestClientCert indicates that a client certificate should be requested
 * during the handshake, but does not require that the client send any
 * certificates. */
#define TLS_REQUEST_CLIENT_CERT ((TlsClientAuthType)1)
/* RequireAnyClientCert indicates that a client certificate should be requested
 * during the handshake, and that at least one certificate is required to be
 * sent by the client, but that certificate is not required to be valid. */
#define TLS_REQUIRE_ANY_CLIENT_CERT ((TlsClientAuthType)2)
/* VerifyClientCertIfGiven indicates that a client certificate should be
 * requested during the handshake, but does not require that the client sends
 * a certificate. If the client does send a certificate it is required to be
 * valid. */
#define TLS_VERIFY_CLIENT_CERT_IF_GIVEN ((TlsClientAuthType)3)
/* RequireAndVerifyClientCert indicates that a client certificate should be
 * requested during the handshake, and that at least one valid certificate is
 * required to be sent by the client. */
#define TLS_REQUIRE_AND_VERIFY_CLIENT_CERT ((TlsClientAuthType)4)

/* ClientAuthType.String: Go's name for the constant, such as
 * "RequireAnyClientCert", or "ClientAuthType(7)" from a. */
BURROW_OWNS(ret) Str tls_client_auth_type_string(TlsClientAuthType i, Alloc *a);

/* RenegotiationSupport enumerates the different levels of support for TLS
 * renegotiation. TLS renegotiation is the act of performing subsequent
 * handshakes on a connection after the first. This significantly complicates
 * the state machine and has been the source of numerous, subtle security
 * issues. Initiating a renegotiation is not supported, but support for
 * accepting renegotiation requests may be enabled.
 *
 * Even when enabled, the server may not change its identity between
 * handshakes (i.e. the leaf certificate must be the same). Additionally,
 * concurrent handshake and application data flow is not permitted so
 * renegotiation can only be used with protocols that synchronise with the
 * renegotiation, such as HTTPS.
 *
 * Renegotiation is not defined in TLS 1.3. */
typedef Int TlsRenegotiationSupport;

/* RenegotiateNever disables renegotiation. */
#define TLS_RENEGOTIATE_NEVER ((TlsRenegotiationSupport)0)
/* RenegotiateOnceAsClient allows a remote server to request renegotiation once
 * per connection. */
#define TLS_RENEGOTIATE_ONCE_AS_CLIENT ((TlsRenegotiationSupport)1)
/* RenegotiateFreelyAsClient allows a remote server to repeatedly request
 * renegotiation. */
#define TLS_RENEGOTIATE_FREELY_AS_CLIENT ((TlsRenegotiationSupport)2)

/* ------------------------------------------------------------------ alerts */

/* An AlertError is a TLS alert.
 *
 * When using a QUIC transport, QUICConn methods will return an error which
 * wraps AlertError rather than sending a TLS alert. */
typedef uint8_t TlsAlertError;

extern const Type *const TYPE_TLS_ALERT_ERROR;

/* AlertError.Error: "tls: " and the alert's description, such as "tls: bad
 * record MAC", or "tls: alert(7)" from a for one Go has no text for. */
BURROW_OWNS(ret) Str tls_alert_error_error(TlsAlertError e, Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error tls_alert_error_as_error(TlsAlertError e, Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_TLS_H */
