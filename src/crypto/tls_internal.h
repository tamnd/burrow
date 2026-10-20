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

#endif /* BURROW_SRC_CRYPTO_TLS_INTERNAL_H */
