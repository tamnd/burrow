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

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/x509.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"
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

/* ------------------------------------------------------------ certificates */

struct X509Certificate;

/* A Certificate is a chain of one or more certificates, leaf first. */
typedef struct TlsCertificate {
    /* [][]byte, each an ASN.1 DER certificate. */
    Slice certificate;
    /* PrivateKey contains the private key corresponding to the public key in
     * Leaf. This must implement crypto.Signer with an RSA, ECDSA, Ed25519 (TLS
     * 1.2+), or ML-DSA (TLS 1.3) PublicKey.
     *
     * For a server up to TLS 1.2, it can also implement crypto.Decrypter with
     * an RSA PublicKey.
     *
     * If it implements crypto.MessageSigner, SignMessage will be used instead
     * of Sign for TLS 1.2 and later.
     *
     * Here that means an RSA, ECDSA, Ed25519 or ML-DSA private key, as
     * x509_parse_pkcs8_private_key returns one, or a TYPE_CRYPTO_SIGNER
     * pointing at any other CryptoSigner. */
    CryptoPrivateKey private_key;
    /* SupportedSignatureAlgorithms is an optional list restricting what
     * signature algorithms the PrivateKey can be used for. A
     * []SignatureScheme. */
    Slice supported_signature_algorithms;
    /* OCSPStaple contains an optional OCSP response which will be served to
     * clients that request it. */
    Slice ocsp_staple;
    /* SignedCertificateTimestamps contains an optional list of Signed
     * Certificate Timestamps which will be served to clients that request it.
     * A [][]byte. */
    Slice signed_certificate_timestamps;
    /* Leaf is the parsed form of the leaf certificate, which may be
     * initialized using x509_parse_certificate to reduce per-handshake
     * processing. If NULL, the leaf certificate will be parsed as needed. */
    struct X509Certificate *leaf;
} TlsCertificate;

extern const Type burrow_type_TlsCertificate;
#define TYPE_TLS_CERTIFICATE TYPE_OF(TlsCertificate)

BURROW_PTR_TYPE_DECL(TlsCertificatePtr, TlsCertificate);
#define TYPE_TLS_CERTIFICATE_PTR TYPE_OF(TlsCertificatePtr)

/* X509KeyPair parses a public/private key pair from a pair of PEM encoded
 * data. On successful return, Certificate.Leaf will be populated.
 *
 * Everything the certificate holds is made from a, in more pieces than are
 * worth freeing one at a time, so give it an arena. After an error what was
 * made is left in a too. */
BURROW_OWNS(ret) TlsCertificate tls_x509_key_pair(Alloc *a, Slice cert_pem_block,
                                                  Slice key_pem_block, Error *err);

/* LoadX509KeyPair reads and parses a public/private key pair from a pair of
 * files. The files must contain PEM encoded data. The certificate file may
 * contain intermediate certificates following the leaf certificate to form a
 * certificate chain. On successful return, Certificate.Leaf will be populated.
 * The certificate is made from a, as tls_x509_key_pair makes it. */
BURROW_OWNS(ret) TlsCertificate tls_load_x509_key_pair(Alloc *a, Str cert_file,
                                                       Str key_file, Error *err);

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

/* -------------------------------------------------------------------- QUIC */

/* QUICEncryptionLevel represents a QUIC encryption level used to transmit
 * handshake messages. */
typedef Int TlsQuicEncryptionLevel;

#define TLS_QUIC_ENCRYPTION_LEVEL_INITIAL ((TlsQuicEncryptionLevel)0)
#define TLS_QUIC_ENCRYPTION_LEVEL_EARLY ((TlsQuicEncryptionLevel)1)
#define TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE ((TlsQuicEncryptionLevel)2)
#define TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION ((TlsQuicEncryptionLevel)3)

/* QUICEncryptionLevel.String: "Initial", "Early", "Handshake" or
 * "Application", or "QUICEncryptionLevel(7)" from a for any other value. */
BURROW_OWNS(ret) Str tls_quic_encryption_level_string(TlsQuicEncryptionLevel l,
                                                      Alloc *a);

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

/* -------------------------------------------------------- connection state */

/* A SessionState is a resumable session.
 *
 * Not in Go: a session keeps everything it points at in an arena of its own,
 * which tls_session_state_free gives back. Grow extra with the allocator
 * tls_session_state_allocator gives. The fields after early_data are Go's
 * unexported ones, for crypto/tls only. */
typedef struct TlsSessionState {
    /* Extra is ignored by crypto/tls, but is encoded by SessionState.Bytes and
     * parsed by ParseSessionState.
     *
     * This allows Config.UnwrapSession/Config.WrapSession and
     * ClientSessionCache implementations to store and retrieve additional
     * data alongside this session.
     *
     * To allow different layers in a protocol stack to share this field,
     * applications must only append to it, not replace it, and must use
     * entries that can be recognized even if out of order (for example, by
     * starting with an id and version prefix).
     *
     * Not in Go: a slice of Bytes. */
    Slice extra;

    /* EarlyData indicates whether the ticket can be used for 0-RTT in a QUIC
     * connection. The application may set this to false if it is true to
     * decline to offer 0-RTT even if supported. */
    bool early_data;

    uint16_t version;
    bool is_client;
    uint16_t cipher_suite;
    /* createdAt is the generation time of the secret on the server (which for
     * TLS 1.0-1.2 might be earlier than the current session) and the time at
     * which the ticket was received on the client. */
    uint64_t created_at; /* seconds since UNIX epoch */
    Slice secret;        /* master secret for TLS 1.2, or the PSK for TLS 1.3 */
    bool ext_master_secret;
    Slice peer_certificates; /* []*x509.Certificate */
    Slice ocsp_response;
    Slice scts;            /* [][]byte */
    Slice verified_chains; /* [][]*x509.Certificate */
    Str alpn_protocol;     /* only set if EarlyData is true */

    /* Client-side TLS 1.3-only fields. */
    uint64_t use_by; /* seconds since UNIX epoch */
    uint32_t age_add;
    Slice ticket;

    /* TLS 1.0-1.2 only fields. */
    TlsCurveID curve_id;

    /* Not in Go: where the session came from, and the arena it keeps its
     * contents in. */
    Alloc *a;
    Arena arena;
} TlsSessionState;

/* ClientSessionState contains the state needed by a client to resume a
 * previous TLS session.
 *
 * Not in Go: refs counts the references to it in place of the garbage
 * collector. tls_new_resumption_state gives out the first one, and the last
 * tls_client_session_state_release frees the session along with it. */
typedef struct TlsClientSessionState {
    struct TlsSessionState *session;
    int32_t refs;
} TlsClientSessionState;

/* What ConnectionState.ExportKeyingMaterial calls, set by the connection. */
BURROW_FUNC(TlsEkmFunc, Slice, Alloc *a, Str label, Slice context, Int length,
            Error *err);

/* ConnectionState records basic TLS details about the connection.
 *
 * negotiated_protocol and server_name are Strs. peer_certificates is a
 * []*x509.Certificate and verified_chains a [][]*x509.Certificate, which
 * should not be modified. signed_certificate_timestamps and local_certificate
 * are [][]byte, and ocsp_response and tls_unique are []byte. */
typedef struct TlsConnectionState {
    /* NegotiatedProtocol is the application protocol negotiated with ALPN. */
    Str negotiated_protocol;
    /* ServerName is the value of the Server Name Indication extension sent by
     * the client. It's available both on the server and on the client side. */
    Str server_name;
    /* PeerCertificates are the parsed certificates sent by the peer, in the
     * order in which they were sent. The first element is the leaf
     * certificate that the connection is verified against.
     *
     * On the client side, it can't be empty. On the server side, it can be
     * empty if Config.ClientAuth is not RequireAnyClientCert or
     * RequireAndVerifyClientCert. */
    Slice peer_certificates;
    /* VerifiedChains is a list of one or more chains where the first element
     * is PeerCertificates[0] and the last element is from Config.RootCAs (on
     * the client side) or Config.ClientCAs (on the server side).
     *
     * On the client side, it's set if Config.InsecureSkipVerify is false. On
     * the server side, it's set if Config.ClientAuth is
     * VerifyClientCertIfGiven (and the peer provided a certificate) or
     * RequireAndVerifyClientCert. */
    Slice verified_chains;
    /* SignedCertificateTimestamps is a list of SCTs provided by the peer
     * through the TLS handshake for the leaf certificate, if any. */
    Slice signed_certificate_timestamps;
    /* OCSPResponse is a stapled Online Certificate Status Protocol (OCSP)
     * response provided by the peer for the leaf certificate, if any. */
    Slice ocsp_response;
    /* TLSUnique contains the "tls-unique" channel binding value (see RFC
     * 5929, Section 3). This value will be nil for TLS 1.3 connections and for
     * resumed connections that don't support Extended Master Secret (RFC
     * 7627). */
    Slice tls_unique;
    /* LocalCertificate is the certificate chain presented to the peer, if
     * any, during the handshake. This field is only populated for connections
     * which are not resumed (DidResume is false). */
    Slice local_certificate;
    /* The connection's own: the closure exposed via ExportKeyingMaterial. */
    TlsEkmFunc ekm;
    /* Version is the TLS version used by the connection (e.g.
     * TLS_VERSION_TLS12). */
    uint16_t version;
    /* CipherSuite is the cipher suite negotiated for the connection (e.g.
     * TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, TLS_AES_128_GCM_SHA256). */
    uint16_t cipher_suite;
    /* CurveID is the key exchange mechanism used for the connection. The name
     * refers to elliptic curves for legacy reasons, see TlsCurveID. If a
     * legacy RSA key exchange is used, this value is zero. */
    TlsCurveID curve_id;
    /* The connection's own: the signature algorithm used by the peer to sign
     * the handshake. It is not set for resumed connections. */
    TlsSignatureScheme testing_only_peer_signature_algorithm;
    /* HandshakeComplete is true if the handshake has concluded. */
    bool handshake_complete;
    /* DidResume is true if this connection was successfully resumed from a
     * previous session with a session ticket or similar mechanism. */
    bool did_resume;
    /* NegotiatedProtocolIsMutual used to indicate a mutual NPN negotiation.
     *
     * Deprecated: this value is always true. */
    bool negotiated_protocol_is_mutual;
    /* ECHAccepted indicates if Encrypted Client Hello was offered by the
     * client and accepted by the server. Currently, ECH is supported only on
     * the client side. */
    bool ech_accepted;
    /* HelloRetryRequest indicates whether we sent a HelloRetryRequest if we
     * are a server, or if we received a HelloRetryRequest if we are a client. */
    bool hello_retry_request;
} TlsConnectionState;

/* ExportKeyingMaterial returns length bytes of exported key material in a new
 * slice as defined in RFC 5705. If context is nil, it is not used as part of
 * the seed. If the connection was set to allow renegotiation via
 * Config.Renegotiation, or if the connections supports neither TLS 1.3 nor
 * Extended Master Secret, this function will return an error.
 *
 * The bytes are from a. cs has to come from a connection, as calling it on a
 * zero one panics in Go too. */
BURROW_OWNS(ret) Slice tls_connection_state_export_keying_material(
    const TlsConnectionState *cs, Alloc *a, Str label, Slice context, Int length,
    Error *err);

/* ---------------------------------------------------- client session cache */

/* ClientSessionCache is a cache of ClientSessionState objects that can be used
 * by a client to resume a TLS session with a given server. ClientSessionCache
 * implementations should expect to be called concurrently from different
 * goroutines. Up to TLS 1.2, only ticket-based resumption is supported, not
 * SessionID-based resumption. In TLS 1.3 they were merged into PSK modes,
 * which are supported via this interface.
 *
 * get searches for a ClientSessionState associated with the given key. On
 * return, *ok is true if one was found.
 *
 * put adds the ClientSessionState to the cache with the given key. It might
 * get called multiple times in a connection if a TLS 1.3 server provides more
 * than one session ticket. If called with a NULL session, it should remove the
 * cache entry.
 *
 * Not in Go: put takes a reference of its own to what it keeps, with
 * tls_client_session_state_retain, and releases it when the entry goes. get
 * gives the caller a reference too, which the caller releases. */
typedef struct TlsClientSessionCacheVT {
    const Type *self_type;
    struct TlsClientSessionState *(*get)(void *self, Str session_key, bool *ok);
    void (*put)(void *self, Str session_key, struct TlsClientSessionState *cs);
} TlsClientSessionCacheVT;

typedef struct TlsClientSessionCache {
    const TlsClientSessionCacheVT *vt;
    void *data;
} TlsClientSessionCache;

/* ClientSessionCache.Get. */
BURROW_OWNS(ret) static inline struct TlsClientSessionState *
tls_client_session_cache_get(TlsClientSessionCache c, Str session_key, bool *ok) {
    return c.vt->get(c.data, session_key, ok);
}

/* ClientSessionCache.Put. */
static inline void tls_client_session_cache_put(TlsClientSessionCache c,
                                                Str session_key,
                                                struct TlsClientSessionState *cs) {
    c.vt->put(c.data, session_key, cs);
}

/* NewLRUClientSessionCache returns a ClientSessionCache with the given
 * capacity that uses an LRU strategy. If capacity is < 1, a default capacity
 * is used instead.
 *
 * The cache and the copies of the keys it keeps are made from a, which has to
 * be one any goroutine can use at once. It holds a reference to each session
 * in it. It never holds more than capacity entries, and it gives back the
 * memory of a key it drops, so a heap allocator does not grow with use. A zero
 * cache, with no vtable, when a is out of memory. */
BURROW_OWNS(ret) TlsClientSessionCache tls_new_lru_client_session_cache(Alloc *a,
                                                                        Int capacity);

/* Not in Go: releases the sessions an LRU cache from
 * tls_new_lru_client_session_cache holds and gives the cache back to the
 * allocator it came from. */
void tls_lru_client_session_cache_free(TlsClientSessionCache c);

/* ------------------------------------------------------------- hello infos */

struct TlsConfig;

/* ClientHelloInfo contains information from a ClientHello message in order to
 * guide application logic in the GetCertificate and GetConfigForClient
 * callbacks.
 *
 * cipher_suites, supported_versions and extensions are []uint16,
 * supported_curves a []CurveID, supported_points a []uint8, signature_schemes
 * a []SignatureScheme and supported_protos a []string. */
typedef struct TlsClientHelloInfo {
    /* CipherSuites lists the CipherSuites supported by the client (e.g.
     * TLS_AES_128_GCM_SHA256, TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256). */
    Slice cipher_suites;
    /* ServerName indicates the name of the server requested by the client in
     * order to support virtual hosting. ServerName is only set if the client
     * is using SNI (see RFC 4366, Section 3.1). */
    Str server_name;
    /* SupportedCurves lists the key exchange mechanisms supported by the
     * client. It was renamed to "supported groups" in TLS 1.3, see RFC 8446,
     * Section 4.2.7 and TlsCurveID.
     *
     * SupportedCurves may be nil in TLS 1.2 and lower if the Supported
     * Elliptic Curves Extension is not being used (see RFC 4492, Section
     * 5.1.1). */
    Slice supported_curves;
    /* SupportedPoints lists the point formats supported by the client.
     * SupportedPoints is set only if the Supported Point Formats Extension is
     * being used (see RFC 4492, Section 5.1.2). */
    Slice supported_points;
    /* SignatureSchemes lists the signature and hash schemes that the client is
     * willing to verify. SignatureSchemes is set only if the Signature
     * Algorithms Extension is being used (see RFC 5246, Section 7.4.1.4.1). */
    Slice signature_schemes;
    /* SupportedProtos lists the application protocols supported by the
     * client. SupportedProtos is set only if the Application-Layer Protocol
     * Negotiation Extension is being used (see RFC 7301, Section 3.1).
     *
     * Servers can select a protocol by setting Config.NextProtos in a
     * GetConfigForClient return value. */
    Slice supported_protos;
    /* SupportedVersions lists the TLS versions supported by the client. For
     * TLS versions less than 1.3, this is extrapolated from the max version
     * advertised by the client, so values other than the greatest might be
     * rejected if used. */
    Slice supported_versions;
    /* Extensions lists the IDs of the extensions presented by the client in
     * the ClientHello. */
    Slice extensions;
    /* Conn is the underlying net.Conn for the connection. Do not read from, or
     * write to, this connection; that will cause the TLS connection to fail. */
    NetConn conn;
    /* The package's own: the Config the GetCertificate or GetConfigForClient
     * caller is using, for SupportsCertificate, and the context of the
     * handshake that is in progress. */
    struct TlsConfig *config;
    Context ctx;
    /* HelloRetryRequest indicates whether the ClientHello was sent in response
     * to a HelloRetryRequest message. */
    bool hello_retry_request;
    /* The package's own: whether the connection is a QUIC connection. */
    bool is_quic;
} TlsClientHelloInfo;

/* Context returns the context of the handshake that is in progress. This
 * context is a child of the context passed to HandshakeContext, if any, and is
 * canceled when the handshake concludes. */
BURROW_BORROWS(ret, c) static inline Context
tls_client_hello_info_context(const TlsClientHelloInfo *c) {
    return c->ctx;
}

/* SupportsCertificate returns nil if the provided certificate is supported by
 * the client that sent the ClientHello. Otherwise, it returns an error
 * describing the reason for the incompatibility.
 *
 * If this ClientHelloInfo was passed to a GetConfigForClient or GetCertificate
 * callback, this method will take into account the associated Config. Note
 * that if GetConfigForClient returns a different Config, the change can't be
 * accounted for by this method.
 *
 * This function will call x509_parse_certificate unless c->leaf is set, which
 * can incur a significant performance cost. */
BURROW_STATIC(ret) Error tls_client_hello_info_supports_certificate(
    const TlsClientHelloInfo *chi, const TlsCertificate *c);

/* CertificateRequestInfo contains information from a server's
 * CertificateRequest message, which is used to demand a certificate and proof
 * of control from a client. */
typedef struct TlsCertificateRequestInfo {
    /* AcceptableCAs contains zero or more, DER-encoded, X.501 Distinguished
     * Names. These are the names of root or intermediate CAs that the server
     * wishes the returned certificate to be signed by. An empty slice
     * indicates that the server has no preference. A [][]byte. */
    Slice acceptable_cas;
    /* SignatureSchemes lists the signature schemes that the server is willing
     * to verify. A []SignatureScheme. */
    Slice signature_schemes;
    /* The package's own: the context of the handshake that is in progress. */
    Context ctx;
    /* Version is the TLS version that was negotiated for this connection. */
    uint16_t version;
} TlsCertificateRequestInfo;

/* Context returns the context of the handshake that is in progress. This
 * context is a child of the context passed to HandshakeContext, if any, and is
 * canceled when the handshake concludes. */
BURROW_BORROWS(ret, c) static inline Context
tls_certificate_request_info_context(const TlsCertificateRequestInfo *c) {
    return c->ctx;
}

/* SupportsCertificate returns nil if the provided certificate is supported by
 * the server that sent the CertificateRequest. Otherwise, it returns an error
 * describing the reason for the incompatibility. */
BURROW_STATIC(ret) Error tls_certificate_request_info_supports_certificate(
    const TlsCertificateRequestInfo *cri, const TlsCertificate *c);

/* ------------------------------------------------------------------ config */

/* EncryptedClientHelloKey holds a private key that is associated with a
 * specific ECH config known to a client. */
typedef struct TlsEncryptedClientHelloKey {
    /* Config should be a marshalled ECHConfig associated with PrivateKey. This
     * must match the config provided to clients byte-for-byte. The config must
     * use as KEM one of
     *
     *   - DHKEM(P-256, HKDF-SHA256) (0x0010)
     *   - DHKEM(P-384, HKDF-SHA384) (0x0011)
     *   - DHKEM(P-521, HKDF-SHA512) (0x0012)
     *   - DHKEM(X25519, HKDF-SHA256) (0x0020)
     *   - ML-KEM-768 (0x0041)
     *   - ML-KEM-1024 (0x0042)
     *   - MLKEM768-P256 (0x0050)
     *   - MLKEM1024-P384 (0x0051)
     *   - MLKEM768-X25519 (0x647a)
     *
     * and as KDF one of
     *
     *   - HKDF-SHA256 (0x0001)
     *   - HKDF-SHA384 (0x0002)
     *   - HKDF-SHA512 (0x0003)
     *
     * and as AEAD one of
     *
     *   - AES-128-GCM (0x0001)
     *   - AES-256-GCM (0x0002)
     *   - ChaCha20Poly1305 (0x0003) */
    Slice config;
    /* PrivateKey should be a marshalled private key, in the format expected by
     * HPKE's DeserializePrivateKey (see RFC 9180), for the KEM used in Config. */
    Slice private_key;
    /* SendAsRetry indicates if Config should be sent as part of the list of
     * retry configs when ECH is requested by the client but rejected by the
     * server. */
    bool send_as_retry;
} TlsEncryptedClientHelloKey;

extern const Type burrow_type_TlsEncryptedClientHelloKey;
#define TYPE_TLS_ENCRYPTED_CLIENT_HELLO_KEY TYPE_OF(TlsEncryptedClientHelloKey)

/* The callbacks a Config can have. Each says what it returns in Go's way: a
 * certificate, config or session, or NULL with nothing in *err, or an error.
 * What one returns has to stay valid for the handshake it was called for. */
BURROW_FUNC0(TlsTimeFunc, Time);
BURROW_FUNC(TlsGetCertificateFunc, TlsCertificate *, TlsClientHelloInfo *chi,
            Error *err);
BURROW_FUNC(TlsGetClientCertificateFunc, TlsCertificate *,
            TlsCertificateRequestInfo *cri, Error *err);
BURROW_FUNC(TlsGetConfigForClientFunc, struct TlsConfig *, TlsClientHelloInfo *chi,
            Error *err);
BURROW_FUNC(TlsVerifyPeerCertificateFunc, Error, Slice raw_certs,
            Slice verified_chains);
BURROW_FUNC(TlsVerifyConnectionFunc, Error, const TlsConnectionState *cs);
BURROW_FUNC(TlsUnwrapSessionFunc, struct TlsSessionState *, Slice identity,
            const TlsConnectionState *cs, Error *err);
BURROW_FUNC(TlsWrapSessionFunc, Slice, Alloc *a, const TlsConnectionState *cs,
            struct TlsSessionState *ss, Error *err);
BURROW_FUNC(TlsGetEncryptedClientHelloKeysFunc, Slice, TlsClientHelloInfo *chi,
            Error *err);

/* A Config structure is used to configure a TLS client or server. After one
 * has been passed to a TLS function it must not be modified. A Config may be
 * reused; the tls package will also not modify it.
 *
 * A zero Config, {0}, is ready to use. The slices in it are the Go types the
 * field comments name, and a nil one is a zero Slice. */
typedef struct TlsConfig {
    /* Rand provides the source of entropy for the connection. If Rand is
     * zero, TLS uses the cryptographic random reader in package crypto/rand.
     * The Reader must be safe for use by multiple goroutines.
     *
     * Deprecated: this should be left zero in production. Not all TLS
     * configurations are guaranteed to use Rand. */
    IoReader rand;

    /* Time returns the current time as the number of seconds since the epoch.
     * If Time is nil, TLS uses time_now. */
    TlsTimeFunc time;

    /* Certificates contains one or more certificate chains to present to the
     * other side of the connection. The first certificate compatible with the
     * peer's requirements is selected automatically. A []Certificate.
     *
     * Server configurations must set one of Certificates, GetCertificate or
     * GetConfigForClient. Clients doing client-authentication may set either
     * Certificates or GetClientCertificate.
     *
     * Note: if there are multiple Certificates, and they don't have the
     * optional field Leaf set, certificate selection will incur a significant
     * per-handshake performance cost. */
    Slice certificates;

    /* NameToCertificate maps from a certificate name to an element of
     * Certificates. Note that a certificate name can be of the form
     * '*.example.com' and so doesn't have to be a domain name as such. A
     * map[string]*Certificate.
     *
     * Deprecated: NameToCertificate only allows associating a single
     * certificate with a given name. Leave this field NULL to let the library
     * select the first compatible chain from Certificates. */
    Map *name_to_certificate;

    /* GetCertificate returns a Certificate based on the given
     * ClientHelloInfo. It will only be called if the client supplies SNI
     * information or if Certificates is empty.
     *
     * If GetCertificate is nil or returns nil, then the certificate is
     * retrieved from NameToCertificate. If NameToCertificate is nil, the best
     * element of Certificates will be used.
     *
     * Once a Certificate is returned it should not be modified. */
    TlsGetCertificateFunc get_certificate;

    /* GetClientCertificate, if not nil, is called when a server requests a
     * certificate from a client. If set, the contents of Certificates will be
     * ignored.
     *
     * If GetClientCertificate returns an error, the handshake will be aborted
     * and that error will be returned. Otherwise GetClientCertificate must
     * return a non-nil Certificate. If Certificate.Certificate is empty then
     * no certificate will be sent to the server. If this is unacceptable to
     * the server then it may abort the handshake.
     *
     * GetClientCertificate may be called multiple times for the same
     * connection if renegotiation occurs or if TLS 1.3 is in use.
     *
     * Once a Certificate is returned it should not be modified. */
    TlsGetClientCertificateFunc get_client_certificate;

    /* GetConfigForClient, if not nil, is called after a ClientHello is
     * received from a client. It may return a non-nil Config in order to
     * change the Config that will be used to handle this connection. If the
     * returned Config is nil, the original Config will be used. The Config
     * returned by this callback may not be subsequently modified.
     *
     * If GetConfigForClient is nil, the Config passed to Server() will be
     * used for all connections.
     *
     * If SessionTicketKey is explicitly set on the returned Config, or if
     * SetSessionTicketKeys is called on the returned Config, those keys will
     * be used. Otherwise, the original Config keys will be used (and possibly
     * rotated if they are automatically managed). WARNING: this allows session
     * resumption of connections originally established with the parent (or a
     * sibling) Config, which may bypass the VerifyPeerCertificate value of the
     * returned Config. */
    TlsGetConfigForClientFunc get_config_for_client;

    /* VerifyPeerCertificate, if not nil, is called after normal certificate
     * verification by either a TLS client or server. It receives the raw
     * ASN.1 certificates provided by the peer and also any verified chains
     * that normal processing found. If it returns a non-nil error, the
     * handshake is aborted and that error results.
     *
     * If normal verification fails then the handshake will abort before
     * considering this callback. If normal verification is disabled (on the
     * client when InsecureSkipVerify is set, or on a server when ClientAuth is
     * RequestClientCert or RequireAnyClientCert), then this callback will be
     * considered but the verifiedChains argument will always be nil. When
     * ClientAuth is NoClientCert, this callback is not called on the server.
     * rawCerts may be empty on the server if ClientAuth is RequestClientCert
     * or VerifyClientCertIfGiven.
     *
     * This callback is not invoked on resumed connections. WARNING: this
     * includes connections resumed across Configs returned by Config.Clone or
     * Config.GetConfigForClient and their parents. If that is not intended,
     * use Config.VerifyConnection instead, or set
     * Config.SessionTicketsDisabled.
     *
     * verifiedChains and its contents should not be modified. */
    TlsVerifyPeerCertificateFunc verify_peer_certificate;

    /* VerifyConnection, if not nil, is called after normal certificate
     * verification and after VerifyPeerCertificate by either a TLS client or
     * server. If it returns a non-nil error, the handshake is aborted and that
     * error results.
     *
     * If normal verification fails then the handshake will abort before
     * considering this callback. This callback will run for all connections,
     * including resumptions, regardless of InsecureSkipVerify or ClientAuth
     * settings. */
    TlsVerifyConnectionFunc verify_connection;

    /* RootCAs defines the set of root certificate authorities that clients
     * use when verifying server certificates. If RootCAs is nil, TLS uses the
     * host's root CA set. */
    X509CertPool *root_cas;

    /* NextProtos is a list of supported application level protocols, in order
     * of preference. If both peers support ALPN, the selected protocol will be
     * one from this list, and the connection will fail if there is no mutually
     * supported protocol. If NextProtos is empty or the peer doesn't support
     * ALPN, the connection will succeed and
     * ConnectionState.NegotiatedProtocol will be empty. A []string. */
    Slice next_protos;

    /* ServerName is used to verify the hostname on the returned certificates
     * unless InsecureSkipVerify is given. It is also included in the client's
     * handshake to support virtual hosting unless it is an IP address. */
    Str server_name;

    /* ClientAuth determines the server's policy for TLS Client
     * Authentication. The default is NoClientCert. */
    TlsClientAuthType client_auth;

    /* ClientCAs defines the set of root certificate authorities that servers
     * use if required to verify a client certificate by the policy in
     * ClientAuth. */
    X509CertPool *client_cas;

    /* CipherSuites is a list of enabled TLS 1.0–1.2 cipher suites. The order
     * of the list is ignored. Note that TLS 1.3 ciphersuites are not
     * configurable. A []uint16.
     *
     * If CipherSuites is nil, a safe default list is used. The default cipher
     * suites might change over time. */
    Slice cipher_suites;

    /* ClientSessionCache is a cache of ClientSessionState entries for TLS
     * session resumption. It is only used by clients. */
    TlsClientSessionCache client_session_cache;

    /* UnwrapSession is called on the server to turn a ticket/identity
     * previously produced by WrapSession into a usable session.
     *
     * UnwrapSession will usually either decrypt a session state in the ticket
     * (for example with Config.EncryptTicket), or use the ticket as a handle
     * to recover a previously stored state. It must use ParseSessionState to
     * deserialize the session state.
     *
     * If UnwrapSession returns an error, the connection is terminated. If it
     * returns (nil, nil), the session is ignored. crypto/tls may still choose
     * not to resume the returned session. */
    TlsUnwrapSessionFunc unwrap_session;

    /* WrapSession is called on the server to produce a session
     * ticket/identity.
     *
     * WrapSession must serialize the session state with SessionState.Bytes.
     * It may then encrypt the serialized state (for example with
     * Config.DecryptTicket) and use it as the ticket, or store the state and
     * return a handle for it.
     *
     * If WrapSession returns an error, the connection is terminated.
     *
     * Warning: the return value will be exposed on the wire and to clients in
     * plaintext. The application is in charge of encrypting and authenticating
     * it (and rotating keys) or returning high-entropy identifiers. Failing to
     * do so correctly can compromise current, previous, and future connections
     * depending on the protocol version. */
    TlsWrapSessionFunc wrap_session;

    /* CurvePreferences contains a set of supported key exchange mechanisms.
     * The name refers to elliptic curves for legacy reasons, see TlsCurveID.
     * The order of the list is ignored, and key exchange mechanisms are chosen
     * from this list using an internal preference order. If empty, the
     * default will be used. A []CurveID.
     *
     * From Go 1.24, the default includes the X25519MLKEM768 hybrid
     * post-quantum key exchange. To disable it, set CurvePreferences
     * explicitly or use the GODEBUG=tlsmlkem=0 environment variable.
     *
     * From Go 1.26, the default includes the SecP256r1MLKEM768 and
     * SecP384r1MLKEM1024 hybrid post-quantum key exchanges, too. To disable
     * them, set CurvePreferences explicitly or use either the
     * GODEBUG=tlsmlkem=0 or the GODEBUG=tlssecpmlkem=0 environment variable. */
    Slice curve_preferences;

    /* Renegotiation controls what types of renegotiation are supported. The
     * default, none, is correct for the vast majority of applications. */
    TlsRenegotiationSupport renegotiation;

    /* KeyLogWriter optionally specifies a destination for TLS master secrets
     * in NSS key log format that can be used to allow external programs such
     * as Wireshark to decrypt TLS connections. See
     * https://datatracker.ietf.org/doc/draft-ietf-tls-keylogfile/. Use of
     * KeyLogWriter compromises security and should only be used for
     * debugging. */
    IoWriter key_log_writer;

    /* EncryptedClientHelloConfigList is a serialized ECHConfigList. If
     * provided, clients will attempt to connect to servers using Encrypted
     * Client Hello (ECH) using one of the provided ECHConfigs.
     *
     * Servers do not use this field. In order to configure ECH for servers,
     * see the EncryptedClientHelloKeys field.
     *
     * If the list contains no valid ECH configs, the handshake will fail and
     * return an error.
     *
     * If EncryptedClientHelloConfigList is set, MinVersion, if set, must be
     * VersionTLS13.
     *
     * When EncryptedClientHelloConfigList is set, the handshake will only
     * succeed if ECH is successfully negotiated. If the server rejects ECH, an
     * ECHRejectionError error will be returned, which may contain a new
     * ECHConfigList that the server suggests using.
     *
     * How this field is parsed may change in future Go versions, if the
     * encoding described in the final Encrypted Client Hello RFC changes. */
    Slice encrypted_client_hello_config_list;

    /* EncryptedClientHelloRejectionVerify, if not nil, is called when ECH is
     * rejected by the remote server, in order to verify the ECH provider
     * certificate in the outer ClientHello. If it returns a non-nil error, the
     * handshake is aborted and that error results.
     *
     * On the server side this field is not used.
     *
     * Unlike VerifyPeerCertificate and VerifyConnection, normal certificate
     * verification will not be performed before calling
     * EncryptedClientHelloRejectionVerify.
     *
     * If EncryptedClientHelloRejectionVerify is nil and ECH is rejected, the
     * roots in RootCAs will be used to verify the ECH providers public
     * certificate. VerifyPeerCertificate and VerifyConnection are not called
     * when ECH is rejected, even if set, and InsecureSkipVerify is ignored. */
    TlsVerifyConnectionFunc encrypted_client_hello_rejection_verify;

    /* GetEncryptedClientHelloKeys, if not nil, is called when by a server
     * when a client attempts ECH.
     *
     * If GetEncryptedClientHelloKeys is not nil, EncryptedClientHelloKeys is
     * ignored.
     *
     * If GetEncryptedClientHelloKeys returns an error, the handshake will be
     * aborted and the error will be returned. Otherwise,
     * GetEncryptedClientHelloKeys must return a non-nil slice of
     * EncryptedClientHelloKey that represents the acceptable ECH keys.
     *
     * For further details, see EncryptedClientHelloKeys. */
    TlsGetEncryptedClientHelloKeysFunc get_encrypted_client_hello_keys;

    /* EncryptedClientHelloKeys are the ECH keys to use when a client attempts
     * ECH. A []EncryptedClientHelloKey.
     *
     * If EncryptedClientHelloKeys is set, MinVersion, if set, must be
     * VersionTLS13.
     *
     * If a client attempts ECH, but it is rejected by the server, the server
     * will send a list of configs to retry based on the set of
     * EncryptedClientHelloKeys which have the SendAsRetry field set.
     *
     * If GetEncryptedClientHelloKeys is non-nil, EncryptedClientHelloKeys is
     * ignored.
     *
     * On the client side, this field is ignored. In order to configure ECH for
     * clients, see the EncryptedClientHelloConfigList field. */
    Slice encrypted_client_hello_keys;

    /* Not in Go: where the config makes its session ticket keys, and the
     * sessions a client keeps and DecryptTicket makes, the heap when it is
     * NULL. It has to be one any goroutine can use at once. */
    Alloc *a;

    /* SessionTicketKey is used by TLS servers to provide session resumption.
     * See RFC 5077 and the PSK mode of RFC 8446. If zero, it will be filled
     * with random data before the first server handshake.
     *
     * Deprecated: if this field is left at zero, session ticket keys will be
     * automatically rotated every day and dropped after seven days. For
     * customizing the rotation schedule or synchronizing servers that are
     * terminating connections for the same host, use SetSessionTicketKeys. */
    Byte session_ticket_key[32];

    /* MinVersion contains the minimum TLS version that is acceptable.
     *
     * By default, TLS 1.2 is currently used as the minimum. TLS 1.0 is the
     * minimum supported by this package. */
    uint16_t min_version;

    /* MaxVersion contains the maximum TLS version that is acceptable.
     *
     * By default, the maximum version supported by this package is used,
     * which is currently TLS 1.3. */
    uint16_t max_version;

    /* InsecureSkipVerify controls whether a client verifies the server's
     * certificate chain and host name. If InsecureSkipVerify is true,
     * crypto/tls accepts any certificate presented by the server and any host
     * name in that certificate. In this mode, TLS is susceptible to
     * machine-in-the-middle attacks unless custom verification is used. This
     * should be used only for testing or in combination with VerifyConnection
     * or VerifyPeerCertificate. */
    bool insecure_skip_verify;

    /* PreferServerCipherSuites is a legacy field and has no effect.
     *
     * It used to control whether the server would follow the client's or the
     * server's preference. Servers now select the best mutually supported
     * cipher suite based on logic that takes into account inferred client
     * hardware, server hardware, and security.
     *
     * Deprecated: PreferServerCipherSuites is ignored. */
    bool prefer_server_cipher_suites;

    /* SessionTicketsDisabled may be set to true to disable session ticket and
     * PSK (resumption) support. Note that on clients, session ticket support
     * is also disabled if ClientSessionCache is nil. */
    bool session_tickets_disabled;

    /* DynamicRecordSizingDisabled disables adaptive sizing of TLS records.
     * When true, the largest possible TLS record size is always used. When
     * false, the size of TLS records may be adjusted in an attempt to improve
     * latency. */
    bool dynamic_record_sizing_disabled;

    /* The config's own. mutex protects session_ticket_keys and
     * auto_session_ticket_keys. session_ticket_keys contains zero or more
     * ticket keys. If set, it means the keys were set with SessionTicketKey or
     * SetSessionTicketKeys. The first key is used for new tickets and any
     * subsequent keys can be used to decrypt old tickets. The contents are not
     * protected by the mutex and are immutable. auto_session_ticket_keys is
     * like session_ticket_keys but is owned by the auto-rotation logic. */
    SyncRWMutex mutex;
    struct burrow__TlsTicketKeys *session_ticket_keys;
    struct burrow__TlsTicketKeys *auto_session_ticket_keys;
} TlsConfig;

/* Clone returns a shallow clone of c or NULL if c is NULL. It is safe to clone
 * a Config that is being used concurrently by a TLS client or server.
 *
 * The returned Config can share session ticket keys with the original Config,
 * which means connections could be resumed across the two Configs. WARNING:
 * Config.VerifyPeerCertificate does not get called on resumed connections,
 * including connections that were originally established on the parent
 * Config. If that is not intended, use Config.VerifyConnection instead, or set
 * Config.SessionTicketsDisabled.
 *
 * The clone is made from a, and NULL when a is out of memory too. Call
 * tls_config_release on it before giving it back to a heap. */
BURROW_OWNS(ret) TlsConfig *tls_config_clone(TlsConfig *c, Alloc *a);

/* Not in Go: gives back the session ticket keys c holds a share of. c can be
 * used after, and makes or takes keys again when it needs them. Nothing for a
 * config that never had any. */
void tls_config_release(TlsConfig *c);

/* SetSessionTicketKeys updates the session ticket keys for a server.
 *
 * The first key will be used when creating new tickets, while all keys can be
 * used for decrypting tickets. It is safe to call this function while the
 * server is running in order to rotate the session ticket keys. The function
 * will panic if keys is empty.
 *
 * Calling this function will turn off automatic session ticket key rotation.
 *
 * If multiple servers are terminating connections for the same host they
 * should all have the same session ticket keys. If the session ticket keys
 * leaks, previously recorded and future TLS connections using those keys might
 * be compromised.
 *
 * keys is n keys of 32 bytes each. burrow_err_out_of_memory when c->a is out
 * of memory, and the keys are left as they were; otherwise no error. */
BURROW_STATIC(ret) Error tls_config_set_session_ticket_keys(TlsConfig *c,
                                                            const Byte (*keys)[32],
                                                            Int n);

/* BuildNameToCertificate parses c.Certificates and builds c.NameToCertificate
 * from the CommonName and SubjectAlternateName fields of each of the leaf
 * certificates.
 *
 * Deprecated: NameToCertificate only allows associating a single certificate
 * with a given name. Leave that field NULL to let the library select the first
 * compatible chain from Certificates.
 *
 * The map, its keys and the certificates parsed to find them are made from a,
 * so give it an arena. A certificate that does not parse is left out, as in
 * Go. burrow_err_out_of_memory when a runs out, with name_to_certificate left
 * as it was; otherwise no error. */
BURROW_STATIC(ret) Error tls_config_build_name_to_certificate(TlsConfig *c, Alloc *a);

/* ---------------------------------------------------------------- sessions */

/* Bytes encodes the session, including any private fields, so that it can be
 * parsed by ParseSessionState. The encoding contains secret values critical
 * to the security of future and possibly past sessions.
 *
 * The specific encoding should be considered opaque and may change
 * incompatibly between Go versions.
 *
 * Not in Go: the bytes are made from a. */
BURROW_OWNS(ret) Slice tls_session_state_bytes(const TlsSessionState *s, Alloc *a,
                                               Error *err);

/* ParseSessionState parses a SessionState encoded by SessionState.Bytes.
 *
 * Not in Go: the session copies what it needs out of data, so data can go
 * after. It comes from a, and tls_session_state_free gives it back. */
BURROW_OWNS(ret) TlsSessionState *tls_parse_session_state(Alloc *a, Slice data,
                                                          Error *err);

/* Not in Go: gives back s and everything in it. Nothing for NULL. */
void tls_session_state_free(TlsSessionState *s);

/* Not in Go: the allocator of the arena s keeps its contents in, for growing
 * s->extra. */
BURROW_BORROWS(ret, s) Alloc *tls_session_state_allocator(TlsSessionState *s);

/* EncryptTicket encrypts a ticket with the Config's configured (or default)
 * session ticket keys. It can be used as a Config.WrapSession implementation.
 *
 * Not in Go: the ticket is made from a. */
BURROW_OWNS(ret) Slice tls_config_encrypt_ticket(TlsConfig *c, Alloc *a,
                                                 const TlsConnectionState *cs,
                                                 const TlsSessionState *ss, Error *err);

/* DecryptTicket decrypts a ticket encrypted by Config.EncryptTicket. It can be
 * used as a Config.UnwrapSession implementation.
 *
 * If the ticket can't be decrypted or parsed, DecryptTicket returns (nil,
 * nil).
 *
 * Not in Go: the session comes from c->a, or the heap when that is NULL. */
BURROW_OWNS(ret) TlsSessionState *
tls_config_decrypt_ticket(TlsConfig *c, Slice identity, const TlsConnectionState *cs,
                          Error *err);

/* ResumptionState returns the session ticket sent by the server (also known as
 * the session's identity) and the state necessary to resume this session.
 *
 * It can be called by ClientSessionCache.Put to serialize (with
 * SessionState.Bytes) and store the session.
 *
 * Not in Go: the ticket comes back, the state goes in *state, and both belong
 * to cs. */
BURROW_BORROWS(ret, cs) Slice tls_client_session_state_resumption_state(
    TlsClientSessionState *cs, TlsSessionState **state, Error *err);

/* NewResumptionState returns a state value that can be returned by
 * ClientSessionCache.Get to resume a previous session.
 *
 * state needs to be returned by ParseSessionState, and the ticket and session
 * state must have been returned by ClientSessionState.ResumptionState.
 *
 * Not in Go: the new value owns state from here on and keeps a copy of
 * ticket. It holds one reference, for the caller. NULL with
 * burrow_err_out_of_memory, and state freed, when there is no memory. */
BURROW_OWNS(ret) TlsClientSessionState *
tls_new_resumption_state(Slice ticket, TlsSessionState *state, Error *err);

/* Not in Go: one more reference to cs, which comes back. NULL stays NULL. */
BURROW_BORROWS(ret, cs) TlsClientSessionState *
tls_client_session_state_retain(TlsClientSessionState *cs);

/* Not in Go: gives back a reference to cs. The last one frees its session,
 * and cs with it. Nothing for NULL. */
void tls_client_session_state_release(TlsClientSessionState *cs);

/* -------------------------------------------------------------------- Conn */

/* RecordHeaderError is returned when a TLS record header is invalid. */
typedef struct TlsRecordHeaderError {
    /* Msg contains a human readable string that describes the error. */
    Str msg;
    /* RecordHeader contains the five bytes of TLS record header that
     * triggered the error. */
    Byte record_header[5];
    /* Conn provides the underlying net.Conn in the case that a client sent an
     * initial handshake that didn't look like TLS. It is nil if there's
     * already been a handshake or a TLS alert has been written to the
     * connection. */
    NetConn conn;
} TlsRecordHeaderError;

extern const Type *const TYPE_TLS_RECORD_HEADER_ERROR;

/* RecordHeaderError.Error: "tls: " and e->msg, built in a. */
BURROW_OWNS(ret) Str tls_record_header_error_error(const TlsRecordHeaderError *e,
                                                   Alloc *a);

/* An Error for e, which errors_as with TYPE_TLS_RECORD_HEADER_ERROR gives
 * back. msg is copied into a, and conn is not, so whatever conn is has to
 * outlive the error if anything is going to use it. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error tls_record_header_error_as_error(const TlsRecordHeaderError *e,
                                                        Alloc *a);

/* A Conn represents a secured connection. It implements the net.Conn
 * interface.
 *
 * Client and Server, which make one, come with the handshakes. A Conn comes
 * from the allocator it was made with, which has to be one that two
 * goroutines can use at once, as the heap can, since reads and writes run in
 * parallel. tls_conn_free gives it back.
 *
 * Once a read or a write has failed for good, the Conn keeps the error and
 * gives it back from every later call. Those errors belong to the Conn and
 * last until tls_conn_free. Any other error comes from the calling
 * goroutine's error_allocator. */
typedef struct TlsConn TlsConn;

/* LocalAddr returns the local network address. */
NetAddr tls_conn_local_addr(TlsConn *c);

/* RemoteAddr returns the remote network address. */
NetAddr tls_conn_remote_addr(TlsConn *c);

/* SetDeadline sets the read and write deadlines associated with the
 * connection. A zero value for t means Conn.Read and Conn.Write will not time
 * out. After a Write has timed out, the TLS state is corrupt and all future
 * writes will return the same error. */
BURROW_BORROWS(ret, c) Error tls_conn_set_deadline(TlsConn *c, Time t);

/* SetReadDeadline sets the read deadline on the underlying connection. A zero
 * value for t means Conn.Read will not time out. */
BURROW_BORROWS(ret, c) Error tls_conn_set_read_deadline(TlsConn *c, Time t);

/* SetWriteDeadline sets the write deadline on the underlying connection. A
 * zero value for t means Conn.Write will not time out. After a Conn.Write has
 * timed out, the TLS state is corrupt and all future writes will return the
 * same error. */
BURROW_BORROWS(ret, c) Error tls_conn_set_write_deadline(TlsConn *c, Time t);

/* NetConn returns the underlying connection that is wrapped by c. Note that
 * writing to or reading from this connection directly will corrupt the TLS
 * session. */
NetConn tls_conn_net_conn(TlsConn *c);

/* Write writes data to the connection.
 *
 * As Write calls Conn.Handshake, in order to prevent indefinite blocking a
 * deadline must be set for both Conn.Read and Write before Write is called
 * when the handshake has not yet completed. See Conn.SetDeadline,
 * Conn.SetReadDeadline, and Conn.SetWriteDeadline. */
Int tls_conn_write(TlsConn *c, Slice b, Error *err);

/* Read reads data from the connection.
 *
 * As Read calls Conn.Handshake, in order to prevent indefinite blocking a
 * deadline must be set for both Read and Conn.Write before Read is called when
 * the handshake has not yet completed. See Conn.SetDeadline,
 * Conn.SetReadDeadline, and Conn.SetWriteDeadline. */
Int tls_conn_read(TlsConn *c, Slice b, Error *err);

/* Close closes the connection. */
BURROW_BORROWS(ret, c) Error tls_conn_close(TlsConn *c);

/* CloseWrite shuts down the writing side of the connection. It should only be
 * called once the handshake has completed and does not call CloseWrite on the
 * underlying connection. Most callers should just use Conn.Close. */
BURROW_BORROWS(ret, c) Error tls_conn_close_write(TlsConn *c);

/* Handshake runs the client or server handshake protocol if it has not yet
 * been run.
 *
 * Most uses of this package need not call Handshake explicitly: the first
 * Conn.Read or Conn.Write will call it automatically.
 *
 * For control over canceling or setting a timeout on a handshake, use
 * Conn.HandshakeContext or the Dialer's DialContext method instead. */
BURROW_BORROWS(ret, c) Error tls_conn_handshake(TlsConn *c);

/* HandshakeContext runs the client or server handshake protocol if it has not
 * yet been run.
 *
 * The provided Context must be non-nil. If the context is canceled before the
 * handshake is complete, the handshake is interrupted and an error is
 * returned. Once the handshake has completed, cancellation of the context will
 * not affect the connection.
 *
 * Most uses of this package need not call HandshakeContext explicitly: the
 * first Conn.Read or Conn.Write will call it automatically. */
BURROW_BORROWS(ret, c) Error tls_conn_handshake_context(TlsConn *c, Context ctx);

/* ConnectionState returns basic TLS details about the connection.
 *
 * The returned ConnectionState is only meaningful after the handshake has
 * completed, as reported by ConnectionState.HandshakeComplete; before then its
 * fields are not populated. The handshake is run automatically by the first
 * Conn.Read or Conn.Write, or it can be triggered explicitly with
 * Conn.Handshake.
 *
 * Not in Go: what the state points at belongs to c. */
TlsConnectionState tls_conn_connection_state(TlsConn *c);

/* OCSPResponse returns the stapled OCSP response from the TLS server, if any.
 * (Only valid for client connections.)
 *
 * Not in Go: the response belongs to c. */
BURROW_BORROWS(ret, c) Slice tls_conn_ocsp_response(TlsConn *c);

/* VerifyHostname checks that the peer certificate chain is valid for
 * connecting to host. If so, it returns nil; if not, it returns an error
 * describing the problem. */
BURROW_BORROWS(ret, c) Error tls_conn_verify_hostname(TlsConn *c, Str host);

/* Not in Go: c as a net.Conn, which is what Go gets by using the *Conn as
 * one. */
NetConn tls_conn_as_net_conn(TlsConn *c);

/* Not in Go: gives c back to the allocator it came from, with everything it
 * kept. It does not close or free the connection underneath, which is the
 * caller's. Close c first, and free it once no goroutine is in a call on it
 * or will make one. NULL does nothing. */
void tls_conn_free(TlsConn *c);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_TLS_H */
