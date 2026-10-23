/* crypto/tls: ech.go, Encrypted Client Hello: the ECHConfig and extension
 * parsing, the inner hello's encoding and its reconstruction on the server,
 * and ECHRejectionError.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/hpke.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "cryptobyte.h"
#include "tls_builder.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

const Type burrow__tls_ech_cipher_type = {
    {(const Byte *)"echCipher", 9},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsECHCipher),
    (uint16_t)_Alignof(TlsECHCipher),
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

const Type burrow__tls_ech_extension_type = {
    {(const Byte *)"echExtension", 12},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsECHExtension),
    (uint16_t)_Alignof(TlsECHExtension),
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

const Type burrow__tls_ech_config_type = {
    {(const Byte *)"echConfig", 9},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsECHConfig),
    (uint16_t)_Alignof(TlsECHConfig),
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

/* ------------------------------------------------------------------ errors */

BURROW_SENTINEL_ERROR(tech_err_malformed_ech_config_list,
                      "tls: malformed ECHConfigList");
BURROW_SENTINEL_ERROR(tech_err_malformed_ech_config, "tls: malformed ECHConfig");
BURROW_SENTINEL_ERROR(tech_err_malformed_outer, "tls: malformed outer client hello");
BURROW_SENTINEL_ERROR(tech_err_invalid_inner, "tls: invalid inner client hello");
BURROW_SENTINEL_ERROR(tech_err_invalid_outer_extensions,
                      "tls: invalid outer extensions");
BURROW_SENTINEL_ERROR(tech_err_invalid_reconstructed,
                      "tls: invalid reconstructed inner client hello");
BURROW_SENTINEL_ERROR(tech_err_unsupported_versions,
                      "tls: client sent encrypted_client_hello extension with "
                      "unsupported versions");
BURROW_SENTINEL_ERROR(tech_err_no_tls13, "tls: client sent encrypted_client_hello "
                                         "extension but did not offer TLS 1.3");
BURROW_SENTINEL_ERROR(tech_err_malformed_ech_ext,
                      "tls: malformed encrypted_client_hello extension");
BURROW_SENTINEL_ERROR(burrow__tls_err_invalid_ech_ext,
                      "tls: client sent invalid encrypted_client_hello extension");

/* echConfigErr, which is only ever made with a field. */
static Error tech_config_err(Str field) {
    return fmt_errorf_v("tls: malformed ECHConfig, invalid %s field", field);
}

/* ------------------------------------------------------------- echConfig */

/* echConfig{}, with the slices nil and typed. */
static TlsECHConfig tech_config_zero(void) {
    TlsECHConfig ec;
    memset(&ec, 0, sizeof ec);
    ec.raw = slice_nil(TYPE_BYTE);
    ec.public_key = slice_nil(TYPE_BYTE);
    ec.symmetric_cipher_suite = slice_nil(&burrow__tls_ech_cipher_type);
    ec.public_name = slice_nil(TYPE_BYTE);
    ec.extensions = slice_nil(&burrow__tls_ech_extension_type);
    return ec;
}

/* append(*s, *v), false when a is out of memory. */
static bool tech_push(Alloc *a, Slice *s, const void *v) {
    Slice r = slice_append(a, *s, v, 1);
    if (r.p == NULL)
        return false;
    *s = r;
    return true;
}

Error burrow__tls_parse_ech_config(Alloc *a, Slice enc, bool *skip, TlsECHConfig *out) {
    *skip = false;
    *out = tech_config_zero();
    CryptobyteString s = enc;
    TlsECHConfig ec = tech_config_zero();
    ec.raw = enc;
    if (!cryptobyte_string_read_uint16(&s, &ec.version))
        return tech_config_err(BURROW_S("version"));
    if (!cryptobyte_string_read_uint16(&s, &ec.length))
        return tech_config_err(BURROW_S("length"));
    if (ec.raw.len < (Int)ec.length + 4)
        return tech_config_err(BURROW_S("length"));
    /* Go's ec.Length+4 is a uint16 sum, and wraps the same way. */
    ec.raw = slice_sub(ec.raw, 0, (uint16_t)(ec.length + 4));
    if (ec.version != TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO) {
        *skip = true;
        return BURROW_NO_ERROR;
    }
    if (!cryptobyte_string_read_uint8(&s, &ec.config_id))
        return tech_config_err(BURROW_S("config_id"));
    if (!cryptobyte_string_read_uint16(&s, &ec.kem_id))
        return tech_config_err(BURROW_S("kem_id"));
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &ec.public_key))
        return tech_config_err(BURROW_S("public_key"));
    CryptobyteString cipher_suites;
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &cipher_suites))
        return tech_config_err(BURROW_S("cipher_suites"));
    while (!cryptobyte_string_empty(cipher_suites)) {
        TlsECHCipher c;
        if (!cryptobyte_string_read_uint16(&cipher_suites, &c.kdf_id))
            return tech_config_err(BURROW_S("cipher_suites kdf_id"));
        if (!cryptobyte_string_read_uint16(&cipher_suites, &c.aead_id))
            return tech_config_err(BURROW_S("cipher_suites aead_id"));
        if (!tech_push(a, &ec.symmetric_cipher_suite, &c))
            return burrow_err_out_of_memory;
    }
    if (!cryptobyte_string_read_uint8(&s, &ec.max_name_length))
        return tech_config_err(BURROW_S("maximum_name_length"));
    CryptobyteString public_name;
    if (!cryptobyte_string_read_uint8_length_prefixed(&s, &public_name))
        return tech_config_err(BURROW_S("public_name"));
    ec.public_name = public_name;
    CryptobyteString extensions;
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &extensions))
        return tech_config_err(BURROW_S("extensions"));
    while (!cryptobyte_string_empty(extensions)) {
        TlsECHExtension e;
        if (!cryptobyte_string_read_uint16(&extensions, &e.type))
            return tech_config_err(BURROW_S("extensions type"));
        if (!cryptobyte_string_read_uint16_length_prefixed(&extensions, &e.data))
            return tech_config_err(BURROW_S("extensions data"));
        if (!tech_push(a, &ec.extensions, &e))
            return burrow_err_out_of_memory;
    }

    *out = ec;
    return BURROW_NO_ERROR;
}

Slice burrow__tls_parse_ech_config_list(Alloc *a, Slice data, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice none = slice_nil(&burrow__tls_ech_config_type);
    CryptobyteString s = data;
    uint16_t length;
    if (!cryptobyte_string_read_uint16(&s, &length)) {
        BURROW_OUT(err, tech_err_malformed_ech_config_list);
        return none;
    }
    if (length != (uint16_t)(data.len - 2)) {
        BURROW_OUT(err, tech_err_malformed_ech_config_list);
        return none;
    }
    Slice configs = none;
    while (s.len > 0) {
        if (s.len < 4) {
            BURROW_OUT(err, tech_err_malformed_ech_config);
            return none;
        }
        CryptobyteString head = slice_sub(s, 2, 4);
        uint16_t config_len = 0;
        (void)cryptobyte_string_read_uint16(&head, &config_len);
        bool skip = false;
        TlsECHConfig ec;
        Error e = burrow__tls_parse_ech_config(a, s, &skip, &ec);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return none;
        }
        s = slice_sub(s, (uint16_t)(config_len + 4), s.len);
        if (!skip && !tech_push(a, &configs, &ec)) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
    }
    return configs;
}

const TlsECHConfig *burrow__tls_pick_ech_config(Alloc *a, Slice list,
                                                HpkePublicKey **pub,
                                                const HpkeKDF **kdf,
                                                const HpkeAEAD **aead) {
    *pub = NULL;
    *kdf = NULL;
    *aead = NULL;
    const TlsECHConfig *configs = list.p;
    for (Int i = 0; i < list.len; i++) {
        const TlsECHConfig *ec = &configs[i];
        if (!burrow__tls_valid_dns_name(
                str_from_bytes(ec->public_name.p, ec->public_name.len)))
            continue;
        bool unsupported_ext = false;
        const TlsECHExtension *exts = ec->extensions.p;
        for (Int j = 0; j < ec->extensions.len; j++) {
            /* If high order bit is set to 1 the extension is mandatory. Since
             * we don't support any extensions, if we see a mandatory bit, we
             * skip the config. */
            if ((exts[j].type & (1U << 15)) != 0)
                unsupported_ext = true;
        }
        if (unsupported_ext)
            continue;
        Error err = BURROW_NO_ERROR;
        const HpkeKEM *kem = hpke_new_kem(ec->kem_id, &err);
        if (BURROW_FAILED(err))
            continue;
        HpkePublicKey *pk = hpke_kem_new_public_key(kem, a, ec->public_key, &err);
        if (BURROW_FAILED(err)) {
            /* This is an error in the config, but killing the connection feels
             * excessive. */
            continue;
        }
        const TlsECHCipher *suites = ec->symmetric_cipher_suite.p;
        for (Int j = 0; j < ec->symmetric_cipher_suite.len; j++) {
            /* All of the supported AEADs and KDFs are fine, rather than
             * imposing some sort of preference here, we just pick the first
             * valid suite. */
            Error e = BURROW_NO_ERROR;
            const HpkeKDF *k = hpke_new_kdf(suites[j].kdf_id, &e);
            if (BURROW_FAILED(e))
                continue;
            /* 0xFFFF is an export-only AEAD that cannot seal/open, making it an
             * invalid choice for encrypting ClientHelloInner. */
            if (suites[j].aead_id == 0xFFFF)
                continue;
            const HpkeAEAD *ad = hpke_new_aead(suites[j].aead_id, &e);
            if (BURROW_FAILED(e))
                continue;
            *pub = pk;
            *kdf = k;
            *aead = ad;
            return ec;
        }
        hpke_public_key_free(pk);
    }
    return NULL;
}

/* ------------------------------------------------------------ inner hello */

Slice burrow__tls_encode_inner_client_hello(const TlsClientHelloMsg *inner, Alloc *a,
                                            Int max_name_length, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    Slice h = burrow__tls_client_hello_msg_marshal_msg(inner, a, true, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    h = slice_sub(h, 4, h.len); /* strip four byte prefix */

    Int padding_len;
    if (inner->server_name.len != 0) {
        padding_len = max_name_length - inner->server_name.len;
        if (padding_len < 0)
            padding_len = 0;
    } else {
        padding_len = max_name_length + 9;
    }
    padding_len += 31 - ((h.len + padding_len - 1) % 32);

    Slice out = slice_append(a, h, NULL, padding_len);
    if (out.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return out;
}

static bool tech_skip_uint8_length_prefixed(CryptobyteString *s) {
    uint8_t skip;
    if (!cryptobyte_string_read_uint8(s, &skip))
        return false;
    return cryptobyte_string_skip(s, skip);
}

static bool tech_skip_uint16_length_prefixed(CryptobyteString *s) {
    uint16_t skip;
    if (!cryptobyte_string_read_uint16(s, &skip))
        return false;
    return cryptobyte_string_skip(s, skip);
}

typedef struct tech_RawExtension {
    uint16_t ext_type;
    Slice data;
} tech_RawExtension;

static const Type tech_raw_extension_type = {
    {(const Byte *)"rawExtension", 12},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(tech_RawExtension),
    (uint16_t)_Alignof(tech_RawExtension),
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

/* extractRawExtensions. Not in Go: the list comes back in *out, made in a. */
static Error tech_extract_raw_extensions(const TlsClientHelloMsg *hello, Alloc *a,
                                         Slice *out) {
    *out = slice_nil(&tech_raw_extension_type);
    CryptobyteString s = hello->original;
    if (!cryptobyte_string_skip(&s, 4 + 2 + 32) || /* header, version, random */
        !tech_skip_uint8_length_prefixed(&s) ||    /* session ID */
        !tech_skip_uint16_length_prefixed(&s) ||   /* cipher suites */
        !tech_skip_uint8_length_prefixed(&s))      /* compression methods */
        return tech_err_malformed_outer;
    Slice raw_extensions = slice_nil(&tech_raw_extension_type);
    CryptobyteString extensions;
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &extensions))
        return tech_err_malformed_outer;

    while (!cryptobyte_string_empty(extensions)) {
        tech_RawExtension ext;
        if (!cryptobyte_string_read_uint16(&extensions, &ext.ext_type) ||
            !cryptobyte_string_read_uint16_length_prefixed(&extensions, &ext.data))
            return tech_err_invalid_inner;
        if (!tech_push(a, &raw_extensions, &ext))
            return burrow_err_out_of_memory;
    }
    *out = raw_extensions;
    return BURROW_NO_ERROR;
}

/* The extensions of the reconstructed inner hello, what Go's innermost
 * AddUint16LengthPrefixed closure in decodeInnerClientHello writes. */
static void tech_add_inner_extensions(TlsBuilder *recon, CryptobyteString extensions,
                                      Slice raw_outer_exts) {
    const tech_RawExtension *raw = raw_outer_exts.p;
    bool outer_extensions_seen = false;
    while (!cryptobyte_string_empty(extensions)) {
        uint16_t extension;
        CryptobyteString ext_data;
        if (!cryptobyte_string_read_uint16(&extensions, &extension) ||
            !cryptobyte_string_read_uint16_length_prefixed(&extensions, &ext_data)) {
            tlsb_fail(recon, tech_err_invalid_inner);
            return;
        }
        if (extension == TLS_EXTENSION_ECH_OUTER_EXTENSIONS) {
            if (outer_extensions_seen) {
                tlsb_fail(recon, tech_err_invalid_outer_extensions);
                return;
            }
            outer_extensions_seen = true;
            CryptobyteString outer_extensions;
            if (!cryptobyte_string_read_uint8_length_prefixed(&ext_data,
                                                              &outer_extensions) ||
                !cryptobyte_string_empty(ext_data) ||
                cryptobyte_string_empty(outer_extensions)) {
                tlsb_fail(recon, tech_err_invalid_inner);
                return;
            }
            /* OuterExtensions reconstruction per RFC 9849, Appendix A.
             * i scans the outer extensions in order and never rewinds,
             * so a referenced type that is out of order or duplicated
             * cannot be found again and is rejected. */
            Int i = 0;
            while (!cryptobyte_string_empty(outer_extensions)) {
                uint16_t ext_type;
                if (!cryptobyte_string_read_uint16(&outer_extensions, &ext_type)) {
                    tlsb_fail(recon, tech_err_invalid_inner);
                    return;
                }
                if (ext_type == TLS_EXTENSION_ENCRYPTED_CLIENT_HELLO ||
                    ext_type == TLS_EXTENSION_ECH_OUTER_EXTENSIONS) {
                    tlsb_fail(recon, tech_err_invalid_outer_extensions);
                    return;
                }
                for (; i <= raw_outer_exts.len; i++) {
                    if (i == raw_outer_exts.len) {
                        tlsb_fail(recon, tech_err_invalid_outer_extensions);
                        return;
                    }
                    if (raw[i].ext_type == ext_type)
                        break;
                }
                tlsb_u16(recon, raw[i].ext_type);
                tlsb_begin(recon, 2);
                tlsb_bytes(recon, raw[i].data);
                tlsb_end(recon);
                i++;
            }
        } else {
            tlsb_u16(recon, extension);
            tlsb_begin(recon, 2);
            tlsb_bytes(recon, ext_data);
            tlsb_end(recon);
        }
    }
}

TlsHandshakeMsg *burrow__tls_decode_inner_client_hello(const TlsClientHelloMsg *outer,
                                                       Alloc *a, Slice encoded,
                                                       Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    /* Reconstructing the inner client hello from its encoded form is somewhat
     * complicated. It is missing its header (message type and length), session
     * ID, and the extensions may be compressed. Since we need to put the
     * extensions back in the same order as they were in the raw outer hello,
     * and since we don't store the raw extensions, or the order we parsed them
     * in, we need to reparse the raw extensions from the outer hello in order
     * to properly insert them into the inner hello. This _should_ result in raw
     * bytes which match the hello as it was generated by the client. */
    CryptobyteString inner_reader = encoded;
    Slice version_and_random, session_id, cipher_suites, compression_methods;
    CryptobyteString extensions;
    if (!cryptobyte_string_read_bytes(&inner_reader, &version_and_random, 2 + 32) ||
        !cryptobyte_string_read_uint8_length_prefixed(&inner_reader, &session_id) ||
        session_id.len != 0 ||
        !cryptobyte_string_read_uint16_length_prefixed(&inner_reader, &cipher_suites) ||
        !cryptobyte_string_read_uint8_length_prefixed(&inner_reader,
                                                      &compression_methods) ||
        !cryptobyte_string_read_uint16_length_prefixed(&inner_reader, &extensions)) {
        BURROW_OUT(err, tech_err_invalid_inner);
        return NULL;
    }

    /* The specification says we must verify that the trailing padding is all
     * zeros. This is kind of weird for TLS messages, where we generally just
     * throw away any trailing garbage. */
    const Byte *pad = inner_reader.p;
    for (Int i = 0; i < inner_reader.len; i++) {
        if (pad[i] != 0) {
            BURROW_OUT(err, tech_err_invalid_inner);
            return NULL;
        }
    }

    Slice raw_outer_exts;
    Error e = tech_extract_raw_extensions(outer, a, &raw_outer_exts);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }

    TlsBuilder recon;
    tlsb_init(&recon, a);
    tlsb_u8(&recon, TLS_TYPE_CLIENT_HELLO);
    tlsb_begin(&recon, 3);
    tlsb_bytes(&recon, version_and_random);
    tlsb_begin(&recon, 1);
    tlsb_bytes(&recon, outer->session_id);
    tlsb_end(&recon);
    tlsb_begin(&recon, 2);
    tlsb_bytes(&recon, cipher_suites);
    tlsb_end(&recon);
    tlsb_begin(&recon, 1);
    tlsb_bytes(&recon, compression_methods);
    tlsb_end(&recon);
    tlsb_begin(&recon, 2);
    tech_add_inner_extensions(&recon, extensions, raw_outer_exts);
    tlsb_end(&recon);
    tlsb_end(&recon);

    Slice recon_bytes = tlsb_finish(&recon, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    TlsHandshakeMsg *msg = BURROW_NEW(a, TlsHandshakeMsg);
    if (msg == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__tls_handshake_msg_init(msg, TLS_MSG_CLIENT_HELLO);
    TlsClientHelloMsg *inner = &msg->client_hello;
    if (!burrow__tls_client_hello_msg_unmarshal(inner, a, recon_bytes)) {
        BURROW_OUT(err, tech_err_invalid_reconstructed);
        return NULL;
    }

    Byte inner_ech_ext = TLS_INNER_ECH_EXT;
    if (!bytes_equal(inner->encrypted_client_hello,
                     slice_from(&inner_ech_ext, 1, 1, TYPE_BYTE))) {
        BURROW_OUT(err, burrow__tls_err_invalid_ech_ext);
        return NULL;
    }

    bool has_tls13 = false;
    const uint16_t *versions = inner->supported_versions.p;
    for (Int i = 0; i < inner->supported_versions.len; i++) {
        uint16_t v = versions[i];
        /* Skip GREASE values (values of the form 0x?A0A). GREASE (Generate
         * Random Extensions And Sustain Extensibility) is a mechanism used by
         * browsers like Chrome to ensure TLS implementations correctly ignore
         * unknown values. GREASE values follow a specific pattern: 0x?A0A,
         * where ? can be any hex digit. These values should be ignored when
         * processing supported TLS versions. */
        if ((v & 0x0F0F) == 0x0A0A && (v & 0xff) == v >> 8)
            continue;

        /* Ensure at least TLS 1.3 is offered. */
        if (v == TLS_VERSION_TLS13) {
            has_tls13 = true;
        } else if (v < TLS_VERSION_TLS13) {
            /* Reject if any non-GREASE value is below TLS 1.3, as ECH requires
             * TLS 1.3+. */
            BURROW_OUT(err, tech_err_unsupported_versions);
            return NULL;
        }
    }

    if (!has_tls13) {
        BURROW_OUT(err, tech_err_no_tls13);
        return NULL;
    }

    return msg;
}

Slice burrow__tls_decrypt_ech_payload(HpkeRecipient *context, Alloc *a, Slice hello,
                                      Slice payload, Error *err) {
    Slice zeros = slice_make(a, TYPE_BYTE, payload.len, payload.len);
    Slice outer_aad = slice_nil(TYPE_BYTE);
    if (zeros.p != NULL || payload.len == 0)
        outer_aad = bytes_replace(a, slice_sub(hello, 4, hello.len), payload, zeros, 1);
    if (outer_aad.p == NULL && hello.len > 4) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return hpke_recipient_open(context, a, outer_aad, payload, err);
}

/* generateOuterECHExt. */
static Slice tech_generate_outer_ech_ext(Alloc *a, uint8_t id, uint16_t kdf_id,
                                         uint16_t aead_id, Slice encoded_key,
                                         Slice payload, Error *err) {
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_u8(&b, 0); /* outer */
    tlsb_u16(&b, kdf_id);
    tlsb_u16(&b, aead_id);
    tlsb_u8(&b, id);
    tlsb_begin(&b, 2);
    tlsb_bytes(&b, encoded_key);
    tlsb_end(&b);
    tlsb_begin(&b, 2);
    tlsb_bytes(&b, payload);
    tlsb_end(&b);
    return tlsb_finish(&b, err);
}

Error burrow__tls_compute_and_update_outer_ech_extension(TlsClientHelloMsg *outer,
                                                         const TlsClientHelloMsg *inner,
                                                         TlsECHClientContext *ech,
                                                         bool use_key, Alloc *a) {
    Slice encap_key = slice_nil(TYPE_BYTE);
    if (use_key)
        encap_key = ech->encapsulated_key;
    Error err = BURROW_NO_ERROR;
    Slice encoded_inner = burrow__tls_encode_inner_client_hello(
        inner, a, ech->config->max_name_length, &err);
    if (BURROW_FAILED(err))
        return err;
    /* NOTE: the tag lengths for all of the supported AEADs are the same (16
     * bytes), so we have hardcoded it here. If we add support for another AEAD
     * with a different tag length, we will need to change this. */
    Int encrypted_len = encoded_inner.len + 16; /* AEAD tag length */
    Slice zeros = slice_make(a, TYPE_BYTE, encrypted_len, encrypted_len);
    if (zeros.p == NULL)
        return burrow_err_out_of_memory;
    outer->encrypted_client_hello = tech_generate_outer_ech_ext(
        a, ech->config->config_id, ech->kdf_id, ech->aead_id, encap_key, zeros, &err);
    if (BURROW_FAILED(err))
        return err;
    Slice serialized_outer = burrow__tls_client_hello_msg_marshal(outer, a, &err);
    if (BURROW_FAILED(err))
        return err;
    /* strip the four byte prefix */
    serialized_outer = slice_sub(serialized_outer, 4, serialized_outer.len);
    Slice encrypted_inner =
        hpke_sender_seal(ech->hpke_context, a, serialized_outer, encoded_inner, &err);
    if (BURROW_FAILED(err))
        return err;
    outer->encrypted_client_hello =
        tech_generate_outer_ech_ext(a, ech->config->config_id, ech->kdf_id,
                                    ech->aead_id, encap_key, encrypted_inner, &err);
    if (BURROW_FAILED(err))
        return err;
    return BURROW_NO_ERROR;
}

bool burrow__tls_valid_dns_name(Str name) {
    if (name.len > 253)
        return false;
    const Byte *p = name.p;
    /* strings.Split gives one label more than there are dots. */
    Int labels = 1;
    for (Int i = 0; i < name.len; i++) {
        if (p[i] == '.')
            labels++;
    }
    if (labels <= 1)
        return false;
    Int start = 0;
    for (Int end = 0; end <= name.len; end++) {
        if (end < name.len && p[end] != '.')
            continue;
        Int label_len = end - start;
        if (label_len == 0)
            return false;
        /* Go ranges over the runes of the label, and a rune past ASCII fails
         * the same test its first byte does here. */
        for (Int i = 0; i < label_len; i++) {
            Byte r = p[start + i];
            if (r == '-' && (i == 0 || i == label_len - 1))
                return false;
            if ((r < '0' || r > '9') && (r < 'a' || r > 'z') && (r < 'A' || r > 'Z') &&
                r != '-')
                return false;
        }
        start = end + 1;
    }
    return true;
}

/* ------------------------------------------------------- ECHRejectionError */

/* The error first, so the data pointer is a pointer to it and errors_as hands
 * it straight back. */
typedef struct tech_RejectionBox {
    TlsECHRejectionError e;
} tech_RejectionBox;

static const Type tech_rejection_error_desc = {
    BURROW_S_INIT("ECHRejectionError"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(TlsECHRejectionError),
    (uint16_t)_Alignof(TlsECHRejectionError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c6563U, /* "tlec" */
    NULL,
};

const Type *const TYPE_TLS_ECH_REJECTION_ERROR = &tech_rejection_error_desc;

static Str tech_rejection_message(const void *self) {
    return tls_ech_rejection_error_error(&((const tech_RejectionBox *)self)->e);
}

static Error tech_rejection_clone(const void *self, Alloc *a) {
    return tls_ech_rejection_error_as_error(&((const tech_RejectionBox *)self)->e, a);
}

static const ErrorVT tech_rejection_vt = {
    .self_type = &tech_rejection_error_desc,
    .message = tech_rejection_message,
    .clone = tech_rejection_clone,
};

Str tls_ech_rejection_error_error(const TlsECHRejectionError *e) {
    (void)e;
    return BURROW_S("tls: server rejected ECH");
}

Error tls_ech_rejection_error_as_error(const TlsECHRejectionError *e, Alloc *a) {
    /* One block: the box, then the retry configs. */
    Int n = e->retry_config_list.len;
    tech_RejectionBox *b = (tech_RejectionBox *)mem_alloc_nozero(
        a, sizeof(tech_RejectionBox) + (size_t)n, _Alignof(tech_RejectionBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = *e;
    if (!slice_is_nil(e->retry_config_list)) {
        Byte *p = (Byte *)(b + 1);
        if (n > 0)
            memcpy(p, e->retry_config_list.p, (size_t)n);
        b->e.retry_config_list = slice_from(p, n, n, TYPE_BYTE);
    }
    return (Error){&tech_rejection_vt, b};
}

/* ------------------------------------------------------- server extension */

Error burrow__tls_parse_ech_ext(Alloc *a, Slice ext, uint8_t *ech_type,
                                TlsECHCipher *cs, uint8_t *config_id, Slice *encap,
                                Slice *payload) {
    *ech_type = 0;
    memset(cs, 0, sizeof *cs);
    *config_id = 0;
    *encap = slice_nil(TYPE_BYTE);
    *payload = slice_nil(TYPE_BYTE);
    /* Not in Go: ext is read in place, and encap and payload are copied out of
     * it below, so the copy Go makes of all of it first is left out. */
    CryptobyteString s = ext;
    uint8_t ech_int;
    if (!cryptobyte_string_read_uint8(&s, &ech_int))
        return tech_err_malformed_ech_ext;
    *ech_type = ech_int;
    if (ech_int == TLS_INNER_ECH_EXT) {
        if (!cryptobyte_string_empty(s))
            return tech_err_malformed_ech_ext;
        return BURROW_NO_ERROR;
    }
    if (ech_int != TLS_OUTER_ECH_EXT)
        return burrow__tls_err_invalid_ech_ext;
    if (!cryptobyte_string_read_uint16(&s, &cs->kdf_id))
        return tech_err_malformed_ech_ext;
    if (!cryptobyte_string_read_uint16(&s, &cs->aead_id))
        return tech_err_malformed_ech_ext;
    if (!cryptobyte_string_read_uint8(&s, config_id))
        return tech_err_malformed_ech_ext;
    Slice e, p;
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &e))
        return tech_err_malformed_ech_ext;
    if (!cryptobyte_string_read_uint16_length_prefixed(&s, &p))
        return tech_err_malformed_ech_ext;

    /* NOTE: clone encap and payload so that mutating them does not mutate the
     * raw extension bytes. */
    *encap = bytes_clone(a, e);
    *payload = bytes_clone(a, p);
    if ((encap->p == NULL && e.len > 0) || (payload->p == NULL && p.len > 0))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

/* append([]byte("tls ech\x00"), config...), the HPKE info for a config. */
static Slice tech_info(Alloc *a, Slice config) {
    Slice info = slice_make(a, TYPE_BYTE, 8 + config.len, 8 + config.len);
    if (info.p == NULL)
        return info;
    memcpy(info.p, "tls ech\0", 8);
    if (config.len > 0)
        memcpy((Byte *)info.p + 8, config.p, (size_t)config.len);
    return info;
}

Error burrow__tls_conn_process_ech_client_hello(TlsConn *c, Alloc *a,
                                                TlsHandshakeMsg *outer_msg,
                                                Slice ech_keys, TlsHandshakeMsg **out,
                                                TlsECHServerContext **ech) {
    *out = NULL;
    *ech = NULL;
    const TlsClientHelloMsg *outer = &outer_msg->client_hello;
    uint8_t ech_type;
    TlsECHCipher ech_ciphersuite;
    uint8_t config_id;
    Slice encap, payload;
    Error err =
        burrow__tls_parse_ech_ext(a, outer->encrypted_client_hello, &ech_type,
                                  &ech_ciphersuite, &config_id, &encap, &payload);
    if (BURROW_FAILED(err)) {
        /* Not in Go: running out of memory is not the client's doing. */
        if (errors_is(err, burrow_err_out_of_memory))
            return err;
        if (errors_is(err, burrow__tls_err_invalid_ech_ext))
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);

        return burrow__tls_err_invalid_ech_ext;
    }

    if (ech_type == TLS_INNER_ECH_EXT) {
        TlsECHServerContext *ctx = BURROW_NEW(a, TlsECHServerContext);
        if (ctx == NULL)
            return burrow_err_out_of_memory;
        ctx->inner = true;
        *out = outer_msg;
        *ech = ctx;
        return BURROW_NO_ERROR;
    }

    if (ech_keys.len == 0) {
        *out = outer_msg;
        return BURROW_NO_ERROR;
    }

    const TlsEncryptedClientHelloKey *keys = ech_keys.p;
    for (Int i = 0; i < ech_keys.len; i++) {
        const TlsEncryptedClientHelloKey *ech_key = &keys[i];
        bool skip = false;
        TlsECHConfig config;
        err = burrow__tls_parse_ech_config(a, ech_key->config, &skip, &config);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: invalid EncryptedClientHelloKey Config: %s",
                                error_text(err));
        }
        if (skip)
            continue;
        const HpkeKEM *kem = hpke_new_kem(config.kem_id, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: invalid EncryptedClientHelloKey Config KEM: %s",
                                error_text(err));
        }
        HpkePrivateKey *ech_priv =
            hpke_kem_new_private_key(kem, a, ech_key->private_key, &err);
        if (BURROW_FAILED(err)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: invalid EncryptedClientHelloKey PrivateKey: %s",
                                error_text(err));
        }
        const HpkeKDF *kdf = hpke_new_kdf(ech_ciphersuite.kdf_id, &err);
        if (BURROW_FAILED(err)) {
            hpke_private_key_free(ech_priv);
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: invalid EncryptedClientHelloKey Config KDF: %s",
                                error_text(err));
        }
        const HpkeAEAD *aead = hpke_new_aead(ech_ciphersuite.aead_id, &err);
        if (BURROW_FAILED(err)) {
            hpke_private_key_free(ech_priv);
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
            return fmt_errorf_v("tls: invalid EncryptedClientHelloKey Config AEAD: %s",
                                error_text(err));
        }
        Slice info = tech_info(a, ech_key->config);
        if (info.p == NULL) {
            hpke_private_key_free(ech_priv);
            return burrow_err_out_of_memory;
        }
        Error e = BURROW_NO_ERROR;
        HpkeRecipient *hpke_context =
            hpke_new_recipient(a, encap, ech_priv, kdf, aead, info, &e);
        hpke_private_key_free(ech_priv);
        if (BURROW_FAILED(e)) {
            /* attempt next trial decryption */
            continue;
        }

        Slice encoded_inner = burrow__tls_decrypt_ech_payload(
            hpke_context, a, outer->original, payload, &e);
        if (BURROW_FAILED(e)) {
            /* attempt next trial decryption */
            continue;
        }

        /* NOTE: we do not enforce that the sent server_name matches the ECH
         * configs PublicName, since this is not particularly important, and
         * the client already had to know what it was in order to properly
         * encrypt the payload. This is only a MAY in the spec, so we're not
         * doing anything revolutionary. */

        TlsHandshakeMsg *ech_inner =
            burrow__tls_decode_inner_client_hello(outer, a, encoded_inner, &e);
        if (BURROW_FAILED(e)) {
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
            return burrow__tls_err_invalid_ech_ext;
        }

        c->ech_accepted = true;

        TlsECHServerContext *ctx = BURROW_NEW(a, TlsECHServerContext);
        if (ctx == NULL)
            return burrow_err_out_of_memory;
        ctx->hpke_context = hpke_context;
        ctx->config_id = config_id;
        ctx->ciphersuite = ech_ciphersuite;
        *out = ech_inner;
        *ech = ctx;
        return BURROW_NO_ERROR;
    }

    *out = outer_msg;
    return BURROW_NO_ERROR;
}

Slice burrow__tls_build_retry_config_list(Alloc *a, Slice keys, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    const TlsEncryptedClientHelloKey *k = keys.p;
    bool at_least_one_retry_config = false;
    for (Int i = 0; i < keys.len; i++) {
        if (k[i].send_as_retry)
            at_least_one_retry_config = true;
    }
    /* Not in Go: with nothing to send the list is not built at all, where Go
     * builds it and then drops it. */
    if (!at_least_one_retry_config)
        return slice_nil(TYPE_BYTE);
    TlsBuilder b;
    tlsb_init(&b, a);
    tlsb_begin(&b, 2);
    for (Int i = 0; i < keys.len; i++) {
        if (!k[i].send_as_retry)
            continue;
        tlsb_bytes(&b, k[i].config);
    }
    tlsb_end(&b);
    return tlsb_finish(&b, err);
}

/* ------------------------------------------------------- accept confirmation */

Slice burrow__tls_ech_accept_confirmation(Alloc *a, const TlsSuiteTLS13 *suite,
                                          Slice random, Str label, Hash transcript,
                                          Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    HashNewFunc h = burrow__tls_suite13_hash_new(suite);
    Error e = BURROW_NO_ERROR;
    Slice prk = hkdf_extract(a, h, random, slice_nil(TYPE_BYTE), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Slice sum = hash_sum(a, transcript, slice_nil(TYPE_BYTE));
    Slice out = slice_nil(TYPE_BYTE);
    if (sum.p != NULL)
        out = burrow__tls13_expand_label(a, h, prk, label, sum, 8);
    if (out.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return out;
}
