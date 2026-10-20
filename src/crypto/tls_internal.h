/* What the crypto/tls files share with each other and with the tests: the
 * protocol's numbers, the cipher suite tables, the TLS 1.0 to 1.2 PRF and the
 * TLS 1.3 key schedule.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_TLS_INTERNAL_H
#define BURROW_SRC_CRYPTO_TLS_INTERNAL_H

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------ the protocol */

#define TLS_MAX_PLAINTEXT 16384           /* maximum plaintext payload length */
#define TLS_MAX_CIPHERTEXT (16384 + 2048) /* maximum ciphertext payload length */
#define TLS_MAX_CIPHERTEXT_TLS13                                                       \
    (16384 + 256)               /* maximum ciphertext length in TLS 1.3 */
#define TLS_RECORD_HEADER_LEN 5 /* record header length */
#define TLS_MAX_HANDSHAKE 65536 /* maximum handshake we support */
#define TLS_MAX_HANDSHAKE_CERTIFICATE_MSG 262144 /* maximum certificate message size */
#define TLS_MAX_USELESS_RECORDS 16 /* maximum consecutive non-advancing records */

/* TLS record types. */
#define TLS_RECORD_TYPE_CHANGE_CIPHER_SPEC 20
#define TLS_RECORD_TYPE_ALERT 21
#define TLS_RECORD_TYPE_HANDSHAKE 22
#define TLS_RECORD_TYPE_APPLICATION_DATA 23

/* TLS handshake message types. */
#define TLS_TYPE_HELLO_REQUEST 0
#define TLS_TYPE_CLIENT_HELLO 1
#define TLS_TYPE_SERVER_HELLO 2
#define TLS_TYPE_NEW_SESSION_TICKET 4
#define TLS_TYPE_END_OF_EARLY_DATA 5
#define TLS_TYPE_ENCRYPTED_EXTENSIONS 8
#define TLS_TYPE_CERTIFICATE 11
#define TLS_TYPE_SERVER_KEY_EXCHANGE 12
#define TLS_TYPE_CERTIFICATE_REQUEST 13
#define TLS_TYPE_SERVER_HELLO_DONE 14
#define TLS_TYPE_CERTIFICATE_VERIFY 15
#define TLS_TYPE_CLIENT_KEY_EXCHANGE 16
#define TLS_TYPE_FINISHED 20
#define TLS_TYPE_CERTIFICATE_STATUS 22
#define TLS_TYPE_KEY_UPDATE 24
#define TLS_TYPE_MESSAGE_HASH 254 /* synthetic message */

/* TLS compression types. */
#define TLS_COMPRESSION_NONE 0

/* TLS extension numbers. */
#define TLS_EXTENSION_SERVER_NAME 0
#define TLS_EXTENSION_STATUS_REQUEST 5
#define TLS_EXTENSION_SUPPORTED_CURVES 10 /* supported_groups in TLS 1.3 */
#define TLS_EXTENSION_SUPPORTED_POINTS 11
#define TLS_EXTENSION_SIGNATURE_ALGORITHMS 13
#define TLS_EXTENSION_ALPN 16
#define TLS_EXTENSION_SCT 18
#define TLS_EXTENSION_EXTENDED_MASTER_SECRET 23
#define TLS_EXTENSION_SESSION_TICKET 35
#define TLS_EXTENSION_PRE_SHARED_KEY 41
#define TLS_EXTENSION_EARLY_DATA 42
#define TLS_EXTENSION_SUPPORTED_VERSIONS 43
#define TLS_EXTENSION_COOKIE 44
#define TLS_EXTENSION_PSK_MODES 45
#define TLS_EXTENSION_CERTIFICATE_AUTHORITIES 47
#define TLS_EXTENSION_SIGNATURE_ALGORITHMS_CERT 50
#define TLS_EXTENSION_KEY_SHARE 51
#define TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS 57
#define TLS_EXTENSION_RENEGOTIATION_INFO 0xff01
#define TLS_EXTENSION_ECH_OUTER_EXTENSIONS 0xfd00
#define TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO 0xfe0d

/* TLS signaling cipher suite values. */
#define TLS_SCSV_RENEGOTIATION 0x00ff

/* TLS 1.3 PSK Key Exchange Modes. See RFC 8446, Section 4.2.9. */
#define TLS_PSK_MODE_PLAIN 0
#define TLS_PSK_MODE_DHE 1

/* TLS Elliptic Curve Point Formats. */
#define TLS_POINT_FORMAT_UNCOMPRESSED 0

/* TLS CertificateStatusType (RFC 3546). */
#define TLS_STATUS_TYPE_OCSP 1

/* Certificate types (for certificateRequestMsg). */
#define TLS_CERT_TYPE_RSA_SIGN 1
#define TLS_CERT_TYPE_ECDSA_SIGN 64 /* ECDSA or EdDSA keys, see RFC 8422 */

/* Signature algorithms (for internal signaling use). Starting at 225 to avoid
 * overlap with TLS 1.2 codepoints (RFC 5246, Appendix A.4.1), with which these
 * have nothing to do. */
#define TLS_SIGNATURE_PKCS1V15 225
#define TLS_SIGNATURE_RSAPSS 226
#define TLS_SIGNATURE_ECDSA 227
#define TLS_SIGNATURE_ED25519 228
#define TLS_SIGNATURE_MLDSA 229

/* helloRetryRequestRandom is set as the Random value of a ServerHello to
 * signal that the message is actually a HelloRetryRequest. 32 bytes. */
extern const Byte burrow__tls_hello_retry_request_random[32];

/* downgradeCanaryTLS12 or downgradeCanaryTLS11 is embedded in the server
 * random as a downgrade protection if the server would be capable of
 * negotiating a higher version. See RFC 8446, Section 4.1.3. */
#define TLS_DOWNGRADE_CANARY_TLS12 "DOWNGRD\x01"
#define TLS_DOWNGRADE_CANARY_TLS11 "DOWNGRD\x00"

bool burrow__tls_is_tls13_only_key_exchange(TlsCurveID curve);
bool burrow__tls_is_pq_key_exchange(TlsCurveID curve);

/* fmt.Sprintf("0x%04X", v), from a, or an empty string when a is out of
 * memory. */
Str burrow__tls_hex4_string(Alloc *a, uint16_t v);

/* ----------------------------------------------------------------- alerts */

#define TLS_ALERT_LEVEL_WARNING 1
#define TLS_ALERT_LEVEL_ERROR 2

#define TLS_ALERT_CLOSE_NOTIFY 0
#define TLS_ALERT_UNEXPECTED_MESSAGE 10
#define TLS_ALERT_BAD_RECORD_MAC 20
#define TLS_ALERT_DECRYPTION_FAILED 21
#define TLS_ALERT_RECORD_OVERFLOW 22
#define TLS_ALERT_DECOMPRESSION_FAILURE 30
#define TLS_ALERT_HANDSHAKE_FAILURE 40
#define TLS_ALERT_BAD_CERTIFICATE 42
#define TLS_ALERT_UNSUPPORTED_CERTIFICATE 43
#define TLS_ALERT_CERTIFICATE_REVOKED 44
#define TLS_ALERT_CERTIFICATE_EXPIRED 45
#define TLS_ALERT_CERTIFICATE_UNKNOWN 46
#define TLS_ALERT_ILLEGAL_PARAMETER 47
#define TLS_ALERT_UNKNOWN_CA 48
#define TLS_ALERT_ACCESS_DENIED 49
#define TLS_ALERT_DECODE_ERROR 50
#define TLS_ALERT_DECRYPT_ERROR 51
#define TLS_ALERT_EXPORT_RESTRICTION 60
#define TLS_ALERT_PROTOCOL_VERSION 70
#define TLS_ALERT_INSUFFICIENT_SECURITY 71
#define TLS_ALERT_INTERNAL_ERROR 80
#define TLS_ALERT_INAPPROPRIATE_FALLBACK 86
#define TLS_ALERT_USER_CANCELED 90
#define TLS_ALERT_NO_RENEGOTIATION 100
#define TLS_ALERT_MISSING_EXTENSION 109
#define TLS_ALERT_UNSUPPORTED_EXTENSION 110
#define TLS_ALERT_CERTIFICATE_UNOBTAINABLE 111
#define TLS_ALERT_UNRECOGNIZED_NAME 112
#define TLS_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE 113
#define TLS_ALERT_BAD_CERTIFICATE_HASH_VALUE 114
#define TLS_ALERT_UNKNOWN_PSK_IDENTITY 115
#define TLS_ALERT_CERTIFICATE_REQUIRED 116
#define TLS_ALERT_NO_APPLICATION_PROTOCOL 120
#define TLS_ALERT_ECH_REQUIRED 121

/* alert.String without the "tls: " in front, or an empty string for an alert
 * Go has no text for. Static. */
Str burrow__tls_alert_text(uint8_t e);

/* ---------------------------------------------------------------- GODEBUG */

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * forgets them when value is NULL so that the next use reads the environment
 * again. For tests. */
void burrow__tls_godebug_set(const char *value);

/* Whether GODEBUG has tlssha1=1. */
bool burrow__tls_godebug_sha1(void);

/* --------------------------------------------------------- cipher suites */

#define TLS_SUITE_ECDHE (1 << 0)
#define TLS_SUITE_EC_SIGN (1 << 1)
#define TLS_SUITE_TLS12 (1 << 2)
#define TLS_SUITE_SHA384 (1 << 3)

#define TLS_AEAD_NONCE_LENGTH 12
#define TLS_NONCE_PREFIX_LENGTH 4

/* The key agreement a suite uses, where Go has a function that makes one. */
typedef enum TlsKeyAgreementKind {
    TLS_KA_RSA,
    TLS_KA_ECDHE_RSA,
    TLS_KA_ECDHE_ECDSA,
} TlsKeyAgreementKind;

/* What a cipher function makes, which Go returns as an any: a cipher.Stream
 * for RC4, a cipher.BlockMode for CBC, or an AEAD with its explicit nonce
 * length. Exactly one has a vtable, or none when the memory ran out. */
typedef struct TlsCipher {
    CipherStream stream;
    CipherBlockMode mode;
    CipherAEAD aead;
    Int explicit_nonce_len;
} TlsCipher;

/* Whether c has nothing in it, which is how a constructor says it was out of
 * memory. */
bool burrow__tls_cipher_is_nil(TlsCipher c);

/* A cipherSuite is a TLS 1.0–1.2 cipher suite, and defines the key exchange
 * mechanism, as well as the cipher+MAC pair or the AEAD. */
typedef struct TlsSuite {
    uint16_t id;
    /* the lengths, in bytes, of the key material needed for each component. */
    Int key_len;
    Int mac_len;
    Int iv_len;
    TlsKeyAgreementKind ka;
    /* flags is a bitmask of the TLS_SUITE_ values, above. */
    int flags;
    TlsCipher (*cipher)(Alloc *a, Slice key, Slice iv, bool is_read);
    Hash (*mac)(Alloc *a, Slice key);
    TlsCipher (*aead)(Alloc *a, Slice key, Slice fixed_nonce);
} TlsSuite;

/* A cipherSuiteTLS13 defines only the pair of the AEAD algorithm and hash
 * algorithm to be used with HKDF. See RFC 8446, Appendix B.4. */
typedef struct TlsSuiteTLS13 {
    uint16_t id;
    Int key_len;
    TlsCipher (*aead)(Alloc *a, Slice key, Slice fixed_nonce);
    CryptoHash hash;
} TlsSuiteTLS13;

/* cipherSuites and cipherSuitesTLS13. */
extern const TlsSuite burrow__tls_cipher_suites[];
extern const Int burrow__tls_ncipher_suites;
extern const TlsSuiteTLS13 burrow__tls_cipher_suites_tls13[];
extern const Int burrow__tls_ncipher_suites_tls13;

/* cipherSuitesPreferenceOrder and cipherSuitesPreferenceOrderNoAES, and
 * their lengths, which are the same. */
extern const uint16_t burrow__tls_cipher_suites_preference_order[];
extern const uint16_t burrow__tls_cipher_suites_preference_order_no_aes[];
extern const Int burrow__tls_npreference_order;
extern const Int burrow__tls_npreference_order_no_aes;

/* defaultCipherSuitesTLS13 and defaultCipherSuitesTLS13NoAES, three each. */
extern const uint16_t burrow__tls_default_cipher_suites_tls13[3];
extern const uint16_t burrow__tls_default_cipher_suites_tls13_no_aes[3];

/* disabledCipherSuites: not used unless explicitly listed in
 * Config.CipherSuites. */
bool burrow__tls_disabled_cipher_suite(uint16_t id);

/* hasAESGCMHardwareSupport */
bool burrow__tls_has_aes_gcm_hardware_support(void);

/* isAESGCMPreferred returns whether we have hardware support for AES-GCM, and
 * the first known cipher in the peer's preference list, n IDs at ciphers, is
 * an AES-GCM cipher, implying the peer also has hardware support for it. */
bool burrow__tls_is_aes_gcm_preferred(const uint16_t *ciphers, Int n);

/* supportedCipherSuites and defaultCipherSuites, as Slices of uint16_t from a,
 * nil when a is out of memory. */
Slice burrow__tls_supported_cipher_suites(Alloc *a, bool aes_gcm_preferred);
Slice burrow__tls_default_cipher_suites(Alloc *a, bool aes_gcm_preferred);

/* defaultCurveEnabled returns whether the key exchange c is enabled by
 * default. */
bool burrow__tls_default_curve_enabled(TlsCurveID c);

/* curvePreferenceOrder is the fixed preference order of key exchanges. It
 * includes every supported key exchange. Eight of them. */
extern const TlsCurveID burrow__tls_curve_preference_order[8];

/* defaultSupportedSignatureAlgorithms returns the signature and hash
 * algorithms that the code advertises and supports in a TLS 1.2+ ClientHello
 * and in a TLS 1.2+ CertificateRequest. Fifteen of them. */
extern const TlsSignatureScheme burrow__tls_default_supported_signature_algorithms[15];

/* directSigning is a standard Hash value that signals that no pre-hashing
 * should be performed, and that the input should be signed directly. It is the
 * hash function associated with the Ed25519 signature scheme. */
#define TLS_DIRECT_SIGNING ((CryptoHash)0)

/* typeAndHashFromSignatureScheme returns the corresponding signature type and
 * crypto.Hash for a given TLS SignatureScheme. False, with zeroes and Go's
 * error in err when err is not NULL, for a scheme it does not know. */
bool burrow__tls_type_and_hash_from_signature_scheme(TlsSignatureScheme s,
                                                     uint8_t *sig_type,
                                                     CryptoHash *hash, Error *err);

/* isDisabledSignatureAlgorithm, which reads tlssha1 from GODEBUG. There is no
 * FIPS 140-3 mode here, so the checks Go makes for one never fire. */
bool burrow__tls_is_disabled_signature_algorithm(uint16_t version, TlsSignatureScheme s,
                                                 bool is_cert);

/* supportedSignatureAlgorithms returns the supported signature algorithms for
 * the given range of TLS versions, to advertise in ClientHello and
 * CertificateRequest messages. An algorithm is included if it is enabled at any
 * version in the range. A []SignatureScheme from a, nil when a is out of
 * memory. */
Slice burrow__tls_supported_signature_algorithms(Alloc *a, uint16_t min_vers,
                                                 uint16_t max_vers);

/* supportedSignatureAlgorithmsCert returns the supported algorithms for
 * signatures in certificates. From a, as above. */
Slice burrow__tls_supported_signature_algorithms_cert(Alloc *a, uint16_t min_vers,
                                                      uint16_t max_vers);

/* isSupportedSignatureAlgorithm: whether sig_alg is in supported, a
 * []SignatureScheme. */
bool burrow__tls_is_supported_signature_algorithm(TlsSignatureScheme sig_alg,
                                                  Slice supported);

/* ------------------------------------------------------------------ auth.go */

/* verifyHandshakeSignature verifies a signature against unhashed handshake
 * contents. */
BURROW_STATIC(ret) Error burrow__tls_verify_handshake_signature(uint8_t sig_type,
                                                                CryptoPublicKey pubkey,
                                                                CryptoHash hash_func,
                                                                Slice signed_,
                                                                Slice sig);

/* verifyLegacyHandshakeSignature verifies a TLS 1.0 and 1.1 signature against
 * pre-hashed handshake contents. */
BURROW_STATIC(ret) Error burrow__tls_verify_legacy_handshake_signature(
    uint8_t sig_type, CryptoPublicKey pubkey, CryptoHash hash_func, Slice hashed,
    Slice sig);

/* serverSignatureContext and clientSignatureContext, each with the zero byte
 * at the end that is part of it. */
#define TLS_SERVER_SIGNATURE_CONTEXT "TLS 1.3, server CertificateVerify\0"
#define TLS_CLIENT_SIGNATURE_CONTEXT "TLS 1.3, client CertificateVerify\0"

/* signedMessage returns the (unhashed) message to be signed by certificate
 * keys in TLS 1.3. See RFC 8446, Section 4.4.3. context is one of the two
 * above, and the message is from a. */
BURROW_OWNS(ret) Slice burrow__tls_signed_message(Alloc *a, Str context,
                                                  Hash transcript);

/* legacyTypeAndHashFromPublicKey returns the fixed signature type and
 * crypto.Hash for a given public key used with TLS 1.0 and 1.1, before the
 * introduction of signature algorithm negotiation. False, with zeroes and Go's
 * error in err when err is not NULL, for a key it cannot be used with. */
bool burrow__tls_legacy_type_and_hash_from_public_key(CryptoPublicKey pub,
                                                      uint8_t *sig_type,
                                                      CryptoHash *hash, Error *err);

/* signatureSchemesForPublicKey: the schemes pub can sign with at version, a
 * []SignatureScheme from a, nil for a key it does not know. */
BURROW_OWNS(ret) Slice burrow__tls_signature_schemes_for_public_key(
    Alloc *a, uint16_t version, CryptoPublicKey pub);

/* The private key of a certificate as a crypto.Signer: the RSA, ECDSA, Ed25519
 * and ML-DSA keys, and a TYPE_CRYPTO_SIGNER, which is copied out. es is where
 * the signer of an Ed25519 key is kept, so it has to outlive *out. False for
 * any other key, which Go's c.PrivateKey.(crypto.Signer) would not take. */
bool burrow__tls_private_key_signer(CryptoPrivateKey key, Ed25519Signer *es,
                                    CryptoSigner *out);

/* selectSignatureScheme picks a SignatureScheme from the peer's preference
 * list that works with the selected certificate. It's only called for protocol
 * versions that support signature algorithms, so TLS 1.2 and 1.3. peer_algs is
 * a []SignatureScheme. Zero, with the reason in err, when there is none. */
TlsSignatureScheme burrow__tls_select_signature_scheme(uint16_t vers,
                                                       const TlsCertificate *c,
                                                       Slice peer_algs, Error *err);

/* unsupportedCertificateError returns a helpful error for certificates with
 * an unsupported private key. */
BURROW_STATIC(ret) Error
burrow__tls_unsupported_certificate_error(const TlsCertificate *cert);

/* cipherSuiteByID and cipherSuiteTLS13ByID, NULL when there is none. */
const TlsSuite *burrow__tls_cipher_suite_by_id(uint16_t id);
const TlsSuiteTLS13 *burrow__tls_cipher_suite_tls13_by_id(uint16_t id);

/* mutualCipherSuite and mutualCipherSuiteTLS13: the suite for want if it is
 * one of the n IDs at have. */
const TlsSuite *burrow__tls_mutual_cipher_suite(const uint16_t *have, Int n,
                                                uint16_t want);
const TlsSuiteTLS13 *burrow__tls_mutual_cipher_suite_tls13(const uint16_t *have, Int n,
                                                           uint16_t want);

/* selectCipherSuite returns the first TLS 1.0–1.2 cipher suite from ids which
 * is also in supported_ids and passes the ok filter. */
const TlsSuite *burrow__tls_select_cipher_suite(const uint16_t *ids, Int nids,
                                                const uint16_t *supported_ids,
                                                Int nsupported,
                                                bool (*ok)(const TlsSuite *, void *),
                                                void *env);

/* The hash a TLS 1.3 suite uses, as a function that makes one. */
HashNewFunc burrow__tls_suite13_hash_new(const TlsSuiteTLS13 *c);

/* tls10MAC implements the TLS 1.0 MAC function. RFC 2246, Section 6.2.3.
 * extra is written after the sum is taken unless it is nil. */
Slice burrow__tls10_mac(Alloc *a, Hash h, Slice out, Slice seq, Slice header,
                        Slice data, Slice extra);

/* ------------------------------------------------------------------- PRF */

#define TLS_MASTER_SECRET_LENGTH 48   /* Length of a master secret in TLS 1.1. */
#define TLS_FINISHED_VERIFY_LENGTH 12 /* Length of verify_data in a Finished message. */

/* The PRF a version and suite use, where Go has a prfFunc: prf10 for TLS 1.0
 * and 1.1, and prf12 over SHA-256 or SHA-384 for 1.2. */
typedef enum TlsPrfKind {
    TLS_PRF_10,
    TLS_PRF_12_SHA256,
    TLS_PRF_12_SHA384,
} TlsPrfKind;

/* splitPreMasterSecret splits a premaster secret in two as specified in RFC
 * 4346, Section 5. The halves are views of secret. */
void burrow__tls_split_pre_master_secret(Slice secret, Slice *s1, Slice *s2);

/* prf10 and prf12: key_len bytes from a, or a nil Slice when a is out of
 * memory. */
Slice burrow__tls_prf10(Alloc *a, Slice secret, Str label, Slice seed, Int key_len);
Slice burrow__tls_prf12(Alloc *a, HashNewFunc h, Slice secret, Str label, Slice seed,
                        Int key_len);

/* The PRF of prf_kind, which is how the callers of a prfFunc call it. */
Slice burrow__tls_prf(Alloc *a, TlsPrfKind prf_kind, Slice secret, Str label,
                      Slice seed, Int key_len);

/* prfAndHashForVersion. The hash is 0 for TLS 1.0 and 1.1. Any other version
 * panics with "unknown version". */
TlsPrfKind burrow__tls_prf_and_hash_for_version(uint16_t version, const TlsSuite *suite,
                                                CryptoHash *hash);

/* masterFromPreMasterSecret generates the master secret from the pre-master
 * secret. See RFC 5246, Section 8.1. */
Slice burrow__tls_master_from_pre_master_secret(Alloc *a, uint16_t version,
                                                const TlsSuite *suite,
                                                Slice pre_master_secret,
                                                Slice client_random,
                                                Slice server_random);

/* extMasterFromPreMasterSecret generates the extended master secret from the
 * pre-master secret. See RFC 7627. */
Slice burrow__tls_ext_master_from_pre_master_secret(Alloc *a, uint16_t version,
                                                    const TlsSuite *suite,
                                                    Slice pre_master_secret,
                                                    Slice transcript);

/* The six slices keysFromMasterSecret returns, which are views of one buffer
 * from a. */
typedef struct TlsKeys {
    Slice client_mac, server_mac;
    Slice client_key, server_key;
    Slice client_iv, server_iv;
} TlsKeys;

/* keysFromMasterSecret generates the connection keys from the master secret,
 * given the lengths of the MAC key, cipher key and IV, as defined in RFC 2246,
 * Section 6.3. False when a is out of memory. */
bool burrow__tls_keys_from_master_secret(Alloc *a, uint16_t version,
                                         const TlsSuite *suite, Slice master_secret,
                                         Slice client_random, Slice server_random,
                                         Int mac_len, Int key_len, Int iv_len,
                                         TlsKeys *keys);

/* A finishedHash calculates the hash of a set of handshake messages suitable
 * for including in a Finished message. */
typedef struct TlsFinishedHash {
    Hash client;
    Hash server;

    /* Prior to TLS 1.2, an additional MD5 hash is required. */
    Hash client_md5;
    Hash server_md5;

    /* In TLS 1.2, a full buffer is sadly required. Nil when there is none. */
    Slice buffer;

    uint16_t version;
    TlsPrfKind prf;

    /* Where the buffer grows from. */
    Alloc *a;
} TlsFinishedHash;

/* newFinishedHash. False when a is out of memory. */
bool burrow__tls_new_finished_hash(Alloc *a, uint16_t version, const TlsSuite *suite,
                                   TlsFinishedHash *h);

/* finishedHash.Write. False when the buffer could not grow. */
bool burrow__tls_finished_hash_write(TlsFinishedHash *h, Slice msg);

/* Sum, clientSum, serverSum and hashForClientCertificate, from a. */
Slice burrow__tls_finished_hash_sum(const TlsFinishedHash *h, Alloc *a);
Slice burrow__tls_finished_hash_client_sum(const TlsFinishedHash *h, Alloc *a,
                                           Slice master_secret);
Slice burrow__tls_finished_hash_server_sum(const TlsFinishedHash *h, Alloc *a,
                                           Slice master_secret);
Slice burrow__tls_finished_hash_for_client_certificate(const TlsFinishedHash *h,
                                                       Alloc *a, uint8_t sig_type);

/* discardHandshakeBuffer is called when there is no more need to buffer the
 * entirety of the handshake messages. */
void burrow__tls_finished_hash_discard_handshake_buffer(TlsFinishedHash *h);

/* noEKMBecauseRenegotiation and noEKMBecauseNoEMS. */
extern const Error burrow__tls_err_no_ekm_renegotiation;
extern const Error burrow__tls_err_no_ekm_no_ems;

/* What ekmFromMasterSecret closes over. */
typedef struct TlsEkmMaster {
    uint16_t version;
    const TlsSuite *suite;
    Slice master_secret;
    Slice client_random;
    Slice server_random;
} TlsEkmMaster;

/* The function ekmFromMasterSecret returns, which generates exported keying
 * material as defined in RFC 5705. context is left out of the seed when it is
 * nil. */
Slice burrow__tls_ekm_from_master_secret(const TlsEkmMaster *m, Alloc *a, Str label,
                                         Slice context, Int length, Error *err);

/* ---------------------------------------------------- TLS 1.3 key schedule */

/* From crypto/internal/fips140/tls13. Every function gives its result from a,
 * or a nil Slice when a is out of memory. */

/* ExpandLabel implements HKDF-Expand-Label from RFC 8446, Section 7.1. A
 * label or context too long panics with "tls13: label or context too long",
 * and a length past 255 times the hash size with "hkdf: counter overflow". */
Slice burrow__tls13_expand_label(Alloc *a, HashNewFunc h, Slice secret, Str label,
                                 Slice context, Int length);

typedef struct Tls13EarlySecret {
    Slice secret;
    HashNewFunc hash;
} Tls13EarlySecret;

typedef struct Tls13HandshakeSecret {
    Slice secret;
    HashNewFunc hash;
} Tls13HandshakeSecret;

typedef struct Tls13MasterSecret {
    Slice secret;
    HashNewFunc hash;
} Tls13MasterSecret;

typedef struct Tls13ExporterMasterSecret {
    Slice secret;
    HashNewFunc hash;
} Tls13ExporterMasterSecret;

/* NewEarlySecret. A nil psk is all zeros. */
Tls13EarlySecret burrow__tls13_new_early_secret(Alloc *a, HashNewFunc h, Slice psk);
Slice burrow__tls13_early_secret_resumption_binder_key(const Tls13EarlySecret *s,
                                                       Alloc *a);

/* ClientEarlyTrafficSecret derives the client_early_traffic_secret from the
 * early secret and the transcript up to the ClientHello. Every transcript
 * here can be a nil Hash, which is the hash of nothing. */
Slice burrow__tls13_early_secret_client_early_traffic_secret(const Tls13EarlySecret *s,
                                                             Alloc *a, Hash transcript);
Tls13HandshakeSecret
burrow__tls13_early_secret_handshake_secret(const Tls13EarlySecret *s, Alloc *a,
                                            Slice shared_secret);

/* EarlyExporterMasterSecret derives the exporter_master_secret from the early
 * secret and the transcript up to the ClientHello. */
Tls13ExporterMasterSecret
burrow__tls13_early_secret_early_exporter_master_secret(const Tls13EarlySecret *s,
                                                        Alloc *a, Hash transcript);

/* ClientHandshakeTrafficSecret and ServerHandshakeTrafficSecret derive the
 * client_handshake_traffic_secret and server_handshake_traffic_secret from the
 * handshake secret and the transcript up to the ServerHello. */
Slice burrow__tls13_handshake_secret_client_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript);
Slice burrow__tls13_handshake_secret_server_handshake_traffic_secret(
    const Tls13HandshakeSecret *s, Alloc *a, Hash transcript);
Tls13MasterSecret
burrow__tls13_handshake_secret_master_secret(const Tls13HandshakeSecret *s, Alloc *a);

/* ClientApplicationTrafficSecret and ServerApplicationTrafficSecret derive
 * the client_application_traffic_secret_0 and
 * server_application_traffic_secret_0 from the master secret and the
 * transcript up to the server Finished. */
Slice burrow__tls13_master_secret_client_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript);
Slice burrow__tls13_master_secret_server_application_traffic_secret(
    const Tls13MasterSecret *s, Alloc *a, Hash transcript);

/* ResumptionMasterSecret derives the resumption_master_secret from the master
 * secret and the transcript up to the client Finished. */
Slice burrow__tls13_master_secret_resumption_master_secret(const Tls13MasterSecret *s,
                                                           Alloc *a, Hash transcript);

/* ExporterMasterSecret derives the exporter_master_secret from the master
 * secret and the transcript up to the server Finished. */
Tls13ExporterMasterSecret
burrow__tls13_master_secret_exporter_master_secret(const Tls13MasterSecret *s, Alloc *a,
                                                   Hash transcript);

/* Exporter, for a context that may be nil, which hashes the same as empty. */
Slice burrow__tls13_exporter_master_secret_exporter(const Tls13ExporterMasterSecret *s,
                                                    Alloc *a, Str label, Slice context,
                                                    Int length);

/* nextTrafficSecret generates the next traffic secret, given the current one,
 * according to RFC 8446, Section 7.2. */
Slice burrow__tls13_next_traffic_secret(const TlsSuiteTLS13 *c, Alloc *a,
                                        Slice traffic_secret);

/* trafficKey generates traffic keys according to RFC 8446, Section 7.3. False
 * when a is out of memory. */
bool burrow__tls13_traffic_key(const TlsSuiteTLS13 *c, Alloc *a, Slice traffic_secret,
                               Slice *key, Slice *iv);

/* finishedHash generates the Finished verify_data or PskBinderEntry according
 * to RFC 8446, Section 4.4.4. See sections 4.4 and 4.2.11.2 for the base_key
 * selection. */
Slice burrow__tls13_finished_hash(const TlsSuiteTLS13 *c, Alloc *a, Slice base_key,
                                  Hash transcript);

/* ------------------------------------------------------ handshake messages
 *
 * Go's handshake_messages.go. Each message has a marshal that gives its bytes
 * from a, or a nil slice and the error in err, and an unmarshal that reports
 * whether data parsed. What unmarshal reads is not copied: the byte slices in
 * the message point into data, as they do in Go, except where Go copies too.
 * Lists that grow, and strings, come from a, and running out of memory makes
 * unmarshal report false. The _zero functions give a message whose slices are
 * nil but typed, which the lists unmarshal appends to need, and a nil slice
 * stays nil the way Go's does, because the difference between nil and empty is
 * part of what the messages mean. */

/* innerECHExt, the ECH extension type that says a ClientHello is the inner
 * one. */
#define TLS_INNER_ECH_EXT 1

/* TLS 1.3 Key Share. See RFC 8446, Section 4.2.8. */
typedef struct TlsKeyShare {
    TlsCurveID group;
    Slice data;
} TlsKeyShare;

/* TLS 1.3 PSK Identity. Can be a Session Ticket, or a reference to a saved
 * session. See RFC 8446, Section 4.2.11. */
typedef struct TlsPskIdentity {
    Slice label;
    uint32_t obfuscated_ticket_age;
} TlsPskIdentity;

/* The element types of a []keyShare and a []pskIdentity. */
extern const Type burrow__tls_key_share_type;
extern const Type burrow__tls_psk_identity_type;

typedef struct TlsClientHelloMsg {
    Slice original;
    uint16_t vers;
    Slice random;
    Slice session_id;
    Slice cipher_suites; /* []uint16 */
    Slice compression_methods;
    Str server_name;
    bool ocsp_stapling;
    Slice supported_curves; /* []CurveID */
    Slice supported_points;
    bool ticket_supported;
    Slice session_ticket;
    Slice supported_signature_algorithms;      /* []SignatureScheme */
    Slice supported_signature_algorithms_cert; /* []SignatureScheme */
    bool secure_renegotiation_supported;
    Slice secure_renegotiation;
    bool extended_master_secret;
    Slice alpn_protocols; /* []string */
    bool scts;
    Slice supported_versions; /* []uint16 */
    Slice cookie;
    Slice key_shares; /* []keyShare */
    bool early_data;
    Slice psk_modes;
    Slice psk_identities; /* []pskIdentity */
    Slice psk_binders;    /* [][]byte */
    Slice quic_transport_parameters;
    Slice encrypted_client_hello;
    /* extensions are only populated on the server-side of a handshake. A
     * []uint16. */
    Slice extensions;
} TlsClientHelloMsg;

/* A clientHelloMsg{} with every slice nil and typed. */
TlsClientHelloMsg burrow__tls_client_hello_msg_zero(void);
Slice burrow__tls_client_hello_msg_marshal_msg(const TlsClientHelloMsg *m, Alloc *a,
                                               bool ech_inner, Error *err);
Slice burrow__tls_client_hello_msg_marshal(const TlsClientHelloMsg *m, Alloc *a,
                                           Error *err);

/* marshalWithoutBinders returns the ClientHello through the
 * PreSharedKeyExtension.identities field, according to RFC 8446, Section
 * 4.2.11.2. Note that m.pskBinders must be set to slices of the correct
 * length. */
Slice burrow__tls_client_hello_msg_marshal_without_binders(const TlsClientHelloMsg *m,
                                                           Alloc *a, Error *err);

/* updateBinders updates the m.pskBinders field. The supplied binders must have
 * the same length as the current m.pskBinders. */
bool burrow__tls_client_hello_msg_update_binders(TlsClientHelloMsg *m,
                                                 Slice psk_binders, Error *err);
bool burrow__tls_client_hello_msg_unmarshal(TlsClientHelloMsg *m, Alloc *a, Slice data);

/* clone, a copy of every slice in m from a. False when a is out of memory. */
bool burrow__tls_client_hello_msg_clone(const TlsClientHelloMsg *m, Alloc *a,
                                        TlsClientHelloMsg *out);

typedef struct TlsServerHelloMsg {
    Slice original;
    uint16_t vers;
    Slice random;
    Slice session_id;
    uint16_t cipher_suite;
    uint8_t compression_method;
    bool ocsp_stapling;
    bool ticket_supported;
    bool secure_renegotiation_supported;
    Slice secure_renegotiation;
    bool extended_master_secret;
    Str alpn_protocol;
    Slice scts; /* [][]byte */
    uint16_t supported_version;
    TlsKeyShare server_share;
    bool selected_identity_present;
    uint16_t selected_identity;
    Slice supported_points;
    Slice encrypted_client_hello;
    bool server_name_ack;

    /* HelloRetryRequest extensions */
    Slice cookie;
    TlsCurveID selected_group;
} TlsServerHelloMsg;

TlsServerHelloMsg burrow__tls_server_hello_msg_zero(void);
Slice burrow__tls_server_hello_msg_marshal(const TlsServerHelloMsg *m, Alloc *a,
                                           Error *err);
bool burrow__tls_server_hello_msg_unmarshal(TlsServerHelloMsg *m, Alloc *a, Slice data);

typedef struct TlsEncryptedExtensionsMsg {
    Str alpn_protocol;
    Slice quic_transport_parameters;
    bool early_data;
    Slice ech_retry_configs;
    bool server_name_ack;
} TlsEncryptedExtensionsMsg;

TlsEncryptedExtensionsMsg burrow__tls_encrypted_extensions_msg_zero(void);
Slice burrow__tls_encrypted_extensions_msg_marshal(const TlsEncryptedExtensionsMsg *m,
                                                   Alloc *a, Error *err);
bool burrow__tls_encrypted_extensions_msg_unmarshal(TlsEncryptedExtensionsMsg *m,
                                                    Alloc *a, Slice data);

/* endOfEarlyDataMsg and serverHelloDoneMsg have nothing in them, and neither
 * does helloRequestMsg. */
Slice burrow__tls_end_of_early_data_msg_marshal(Alloc *a, Error *err);
bool burrow__tls_end_of_early_data_msg_unmarshal(Slice data);

typedef struct TlsKeyUpdateMsg {
    bool update_requested;
} TlsKeyUpdateMsg;

Slice burrow__tls_key_update_msg_marshal(const TlsKeyUpdateMsg *m, Alloc *a,
                                         Error *err);
bool burrow__tls_key_update_msg_unmarshal(TlsKeyUpdateMsg *m, Slice data);

typedef struct TlsNewSessionTicketMsgTLS13 {
    uint32_t lifetime;
    uint32_t age_add;
    Slice nonce;
    Slice label;
    uint32_t max_early_data;
} TlsNewSessionTicketMsgTLS13;

TlsNewSessionTicketMsgTLS13 burrow__tls_new_session_ticket_msg_tls13_zero(void);
Slice burrow__tls_new_session_ticket_msg_tls13_marshal(
    const TlsNewSessionTicketMsgTLS13 *m, Alloc *a, Error *err);
bool burrow__tls_new_session_ticket_msg_tls13_unmarshal(TlsNewSessionTicketMsgTLS13 *m,
                                                        Slice data);

typedef struct TlsCertificateRequestMsgTLS13 {
    bool ocsp_stapling;
    bool scts;
    Slice supported_signature_algorithms;      /* []SignatureScheme */
    Slice supported_signature_algorithms_cert; /* []SignatureScheme */
    Slice certificate_authorities;             /* [][]byte */
} TlsCertificateRequestMsgTLS13;

TlsCertificateRequestMsgTLS13 burrow__tls_certificate_request_msg_tls13_zero(void);
Slice burrow__tls_certificate_request_msg_tls13_marshal(
    const TlsCertificateRequestMsgTLS13 *m, Alloc *a, Error *err);
bool burrow__tls_certificate_request_msg_tls13_unmarshal(
    TlsCertificateRequestMsgTLS13 *m, Alloc *a, Slice data);

typedef struct TlsCertificateMsg {
    Slice certificates; /* [][]byte */
} TlsCertificateMsg;

Slice burrow__tls_certificate_msg_marshal(const TlsCertificateMsg *m, Alloc *a,
                                          Error *err);
bool burrow__tls_certificate_msg_unmarshal(TlsCertificateMsg *m, Alloc *a, Slice data);

typedef struct TlsCertificateMsgTLS13 {
    TlsCertificate certificate;
    bool ocsp_stapling;
    bool scts;
} TlsCertificateMsgTLS13;

/* A Certificate{} with its slices nil and typed. */
TlsCertificate burrow__tls_certificate_zero(void);
TlsCertificateMsgTLS13 burrow__tls_certificate_msg_tls13_zero(void);
Slice burrow__tls_certificate_msg_tls13_marshal(const TlsCertificateMsgTLS13 *m,
                                                Alloc *a, Error *err);
bool burrow__tls_certificate_msg_tls13_unmarshal(TlsCertificateMsgTLS13 *m, Alloc *a,
                                                 Slice data);

typedef struct TlsServerKeyExchangeMsg {
    Slice key;
} TlsServerKeyExchangeMsg;

Slice burrow__tls_server_key_exchange_msg_marshal(const TlsServerKeyExchangeMsg *m,
                                                  Alloc *a, Error *err);
bool burrow__tls_server_key_exchange_msg_unmarshal(TlsServerKeyExchangeMsg *m,
                                                   Slice data);

typedef struct TlsCertificateStatusMsg {
    Slice response;
} TlsCertificateStatusMsg;

Slice burrow__tls_certificate_status_msg_marshal(const TlsCertificateStatusMsg *m,
                                                 Alloc *a, Error *err);
bool burrow__tls_certificate_status_msg_unmarshal(TlsCertificateStatusMsg *m,
                                                  Slice data);

Slice burrow__tls_server_hello_done_msg_marshal(Alloc *a, Error *err);
bool burrow__tls_server_hello_done_msg_unmarshal(Slice data);

typedef struct TlsClientKeyExchangeMsg {
    Slice ciphertext;
} TlsClientKeyExchangeMsg;

Slice burrow__tls_client_key_exchange_msg_marshal(const TlsClientKeyExchangeMsg *m,
                                                  Alloc *a, Error *err);
bool burrow__tls_client_key_exchange_msg_unmarshal(TlsClientKeyExchangeMsg *m,
                                                   Slice data);

typedef struct TlsFinishedMsg {
    Slice verify_data;
} TlsFinishedMsg;

Slice burrow__tls_finished_msg_marshal(const TlsFinishedMsg *m, Alloc *a, Error *err);
bool burrow__tls_finished_msg_unmarshal(TlsFinishedMsg *m, Slice data);

typedef struct TlsCertificateRequestMsg {
    /* hasSignatureAlgorithm indicates whether this message includes a list of
     * supported signature algorithms. This change was introduced with TLS
     * 1.2. */
    bool has_signature_algorithm;

    Slice certificate_types;
    Slice supported_signature_algorithms; /* []SignatureScheme */
    Slice certificate_authorities;        /* [][]byte */
} TlsCertificateRequestMsg;

TlsCertificateRequestMsg burrow__tls_certificate_request_msg_zero(void);
Slice burrow__tls_certificate_request_msg_marshal(const TlsCertificateRequestMsg *m,
                                                  Alloc *a, Error *err);
bool burrow__tls_certificate_request_msg_unmarshal(TlsCertificateRequestMsg *m,
                                                   Alloc *a, Slice data);

typedef struct TlsCertificateVerifyMsg {
    bool has_signature_algorithm; /* format change introduced in TLS 1.2 */
    TlsSignatureScheme signature_algorithm;
    Slice signature;
} TlsCertificateVerifyMsg;

Slice burrow__tls_certificate_verify_msg_marshal(const TlsCertificateVerifyMsg *m,
                                                 Alloc *a, Error *err);
bool burrow__tls_certificate_verify_msg_unmarshal(TlsCertificateVerifyMsg *m,
                                                  Slice data);

typedef struct TlsNewSessionTicketMsg {
    Slice ticket;
} TlsNewSessionTicketMsg;

Slice burrow__tls_new_session_ticket_msg_marshal(const TlsNewSessionTicketMsg *m,
                                                 Alloc *a, Error *err);
bool burrow__tls_new_session_ticket_msg_unmarshal(TlsNewSessionTicketMsg *m,
                                                  Slice data);

Slice burrow__tls_hello_request_msg_marshal(Alloc *a, Error *err);
bool burrow__tls_hello_request_msg_unmarshal(Slice data);

/* ---------------------------------------------------------------- config */

/* roleClient and roleServer are meant to call supportedVersions and parents
 * with more readability at the callsite. */
#define TLS_ROLE_CLIENT true
#define TLS_ROLE_SERVER false

/* ticketKeyLifetime is how long a ticket key remains valid and can be used to
 * resume a client connection. ticketKeyRotation is how often the server
 * should rotate the session ticket key that is used for new tickets. */
#define TLS_TICKET_KEY_LIFETIME (7 * (24 * TIME_HOUR))
#define TLS_TICKET_KEY_ROTATION (24 * TIME_HOUR)

/* maxSessionTicketLifetime is the maximum allowed lifetime of a TLS 1.3
 * session ticket, and the lifetime we set for all tickets we send. */
#define TLS_MAX_SESSION_TICKET_LIFETIME (7 * (24 * TIME_HOUR))

/* ticketKey is the internal representation of a session ticket key. */
typedef struct TlsTicketKey {
    Byte aes_key[16];
    Byte hmac_key[16];
    /* created is the time at which this ticket key was created. See
     * Config.ticketKeys. */
    Time created;
} TlsTicketKey;

/* A []ticketKey that a Config and its clones share. Go's slices of them are
 * never written to after they are made, and these are not either. A count of
 * references stands in for the garbage collector: a config holds one for each
 * of its two lists, and so does whoever burrow__tls_config_ticket_keys gave
 * one to. */
typedef struct burrow__TlsTicketKeys {
    int32_t refs;
    Int len;
    Int cap;
    Alloc *a;
    TlsTicketKey keys[];
} TlsTicketKeys;

/* Gives back one reference to k. Nothing for NULL. */
void burrow__tls_ticket_keys_release(TlsTicketKeys *k);

/* ticketKeyFromBytes converts from the external representation of a session
 * ticket key to a ticketKey. Externally, session ticket keys are 32 random
 * bytes and this function expands that into sufficient name and key material. */
TlsTicketKey burrow__tls_config_ticket_key_from_bytes(const TlsConfig *c,
                                                      const Byte b[32]);

/* ticketKeys returns the ticketKeys for this connection. If configForClient
 * has explicitly set keys, those will be returned. Otherwise, the keys on c
 * will be used and may be rotated if auto-managed. During rotation, any
 * expired session ticket keys are deleted from c.sessionTicketKeys. If the
 * session ticket key that is currently encrypting tickets (ie. the first
 * ticketKey in c.sessionTicketKeys) is not fresh, then a new session ticket key
 * will be created and prepended to c.sessionTicketKeys.
 *
 * NULL when session tickets are disabled. Otherwise a reference the caller
 * gives back with burrow__tls_ticket_keys_release, or NULL with
 * burrow_err_out_of_memory in *err. */
TlsTicketKeys *burrow__tls_config_ticket_keys(TlsConfig *c,
                                              TlsConfig *config_for_client, Error *err);

/* c.rand() and c.time(): the reader and clock c has, or crypto/rand and
 * time_now. */
IoReader burrow__tls_config_rand(const TlsConfig *c);
Time burrow__tls_config_time(const TlsConfig *c);

/* cipherSuites and supportedCipherSuites, a []uint16 from a. */
Slice burrow__tls_config_cipher_suites(const TlsConfig *c, Alloc *a,
                                       bool aes_gcm_preferred);
Slice burrow__tls_config_supported_cipher_suites(const TlsConfig *c, Alloc *a);

/* supportedVersions returns the list of supported TLS versions, sorted from
 * highest to lowest (and hence also in preference order). Into out, with the
 * count. c can be NULL. */
Int burrow__tls_config_supported_versions(const TlsConfig *c, bool is_client,
                                          bool is_quic, uint16_t out[4]);

/* maxSupportedVersion, zero when there is none. */
uint16_t burrow__tls_config_max_supported_version(const TlsConfig *c, bool is_client,
                                                  bool is_quic);

/* supportedVersionsFromMax returns a list of supported versions derived from a
 * legacy maximum version value. Note that only versions supported by this
 * library are returned. Any newer peer will use supportedVersions anyway. Into
 * out, with the count. */
Int burrow__tls_supported_versions_from_max(uint16_t max_version, uint16_t out[4]);

/* curvePreferences, into out, with the count. */
Int burrow__tls_config_curve_preferences(const TlsConfig *c, uint16_t version,
                                         TlsCurveID out[8]);

/* supportsCurve. c can be NULL. */
bool burrow__tls_config_supports_curve(const TlsConfig *c, uint16_t version,
                                       TlsCurveID x);

/* mutualVersion returns the protocol version to use given the advertised
 * versions of the peer, a []uint16. The highest supported version is
 * preferred. False when there is none. */
bool burrow__tls_config_mutual_version(const TlsConfig *c, bool is_client, bool is_quic,
                                       Slice peer_versions, uint16_t *out);

extern const Error burrow__tls_err_no_certificates;

/* getCertificate returns the best certificate for the given ClientHelloInfo,
 * defaulting to the first element of c.Certificates. */
TlsCertificate *burrow__tls_config_get_certificate(TlsConfig *c,
                                                   TlsClientHelloInfo *client_hello,
                                                   Error *err);

/* leaf returns the parsed leaf certificate, either from c.Leaf or by parsing
 * the corresponding c.Certificate[0], which is made from a. */
X509Certificate *burrow__tls_certificate_leaf(const TlsCertificate *c, Alloc *a,
                                              Error *err);

#define TLS_KEY_LOG_LABEL_TLS12 "CLIENT_RANDOM"
#define TLS_KEY_LOG_LABEL_CLIENT_HANDSHAKE "CLIENT_HANDSHAKE_TRAFFIC_SECRET"
#define TLS_KEY_LOG_LABEL_SERVER_HANDSHAKE "SERVER_HANDSHAKE_TRAFFIC_SECRET"
#define TLS_KEY_LOG_LABEL_CLIENT_TRAFFIC "CLIENT_TRAFFIC_SECRET_0"
#define TLS_KEY_LOG_LABEL_SERVER_TRAFFIC "SERVER_TRAFFIC_SECRET_0"

/* writeKeyLog: label, client_random and secret as one line of the NSS key log
 * format, to c->key_log_writer when there is one. */
Error burrow__tls_config_write_key_log(const TlsConfig *c, Str label,
                                       Slice client_random, Slice secret);

/* supportsECDHE returns whether ECDHE key exchanges can be used with this
 * pre-TLS 1.3 client. supported_curves is a []CurveID and supported_points a
 * []uint8. */
bool burrow__tls_supports_ecdhe(const TlsConfig *c, uint16_t version,
                                Slice supported_curves, Slice supported_points,
                                Error *err);

#endif /* BURROW_SRC_CRYPTO_TLS_INTERNAL_H */
