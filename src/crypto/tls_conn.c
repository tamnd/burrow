/* crypto/tls: conn.go, the record layer. A halfConn encrypts and decrypts one
 * direction of a connection, and a Conn reads and writes records with two of
 * them.
 *
 * Go keeps a cipher in an any and switches on its type. Here a TlsCipher has
 * one vtable set out of three, and the same switch looks at which. The
 * ciphers live in arenas the halfConn owns, one for the state in use and one
 * for the state a change of cipher spec moves to, and each arena goes when
 * its state is replaced.
 *
 * Go lets the collector keep an error alive for as long as anyone holds it. A
 * halfConn copies the errors it keeps into an arena of its own, which lasts as
 * long as the Conn does, and the Conn keeps its handshake error the same way.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/subtle.h"
#include "burrow/crypto/x509.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* How much an arena of keys asks for at a time. A suite's state is well under
 * this, and anything bigger gets a chunk of its own. */
#define THC_KEYS_CHUNK 4096

static Alloc *thc_parent(const TlsHalfConn *hc) {
    return hc->parent != NULL ? hc->parent : heap_allocator();
}

static Arena *thc_new_arena(const TlsHalfConn *hc) {
    Alloc *parent = thc_parent(hc);
    Arena *ar = BURROW_NEW(parent, Arena);
    if (ar == NULL)
        return NULL;
    arena_init(ar, parent, THC_KEYS_CHUNK);
    return ar;
}

static void thc_free_arena(const TlsHalfConn *hc, Arena *ar) {
    if (ar == NULL)
        return;
    arena_free(ar);
    mem_free(thc_parent(hc), ar, sizeof(Arena), _Alignof(Arena));
}

static Slice thc_seq(TlsHalfConn *hc) {
    return slice_from(hc->seq, 8, 8, TYPE_BYTE);
}

static Error thc_alert(uint8_t e) {
    return burrow__tls_alert(NULL, e);
}

void burrow__tls_half_conn_init(TlsHalfConn *hc, Alloc *a) {
    memset(hc, 0, sizeof *hc);
    hc->parent = a;
}

void burrow__tls_half_conn_free(TlsHalfConn *hc) {
    thc_free_arena(hc, hc->keys);
    thc_free_arena(hc, hc->next_keys);
    thc_free_arena(hc, hc->errs);
    hc->keys = NULL;
    hc->next_keys = NULL;
    hc->errs = NULL;
    hc->err = BURROW_NO_ERROR;
    memset(&hc->cipher, 0, sizeof hc->cipher);
    memset(&hc->next_cipher, 0, sizeof hc->next_cipher);
    memset(&hc->mac, 0, sizeof hc->mac);
    memset(&hc->next_mac, 0, sizeof hc->next_mac);
    hc->traffic_secret = slice_nil(TYPE_BYTE);
}

Alloc *burrow__tls_half_conn_next_alloc(TlsHalfConn *hc) {
    Arena *ar = thc_new_arena(hc);
    if (ar == NULL)
        return NULL;
    thc_free_arena(hc, hc->next_keys);
    hc->next_keys = ar;
    memset(&hc->next_cipher, 0, sizeof hc->next_cipher);
    memset(&hc->next_mac, 0, sizeof hc->next_mac);
    return arena_allocator(ar);
}

void burrow__tls_half_conn_prepare_cipher_spec(TlsHalfConn *hc, uint16_t version,
                                               TlsCipher cipher, Hash mac) {
    hc->version = version;
    hc->next_cipher = cipher;
    hc->next_mac = mac;
}

Error burrow__tls_half_conn_change_cipher_spec(TlsHalfConn *hc) {
    if (burrow__tls_cipher_is_nil(hc->next_cipher) || hc->version == TLS_VERSION_TLS13)
        return thc_alert(TLS_ALERT_INTERNAL_ERROR);
    hc->cipher = hc->next_cipher;
    hc->mac = hc->next_mac;
    memset(&hc->next_cipher, 0, sizeof hc->next_cipher);
    memset(&hc->next_mac, 0, sizeof hc->next_mac);
    thc_free_arena(hc, hc->keys);
    hc->keys = hc->next_keys;
    hc->next_keys = NULL;
    memset(hc->seq, 0, sizeof hc->seq);
    return BURROW_NO_ERROR;
}

Error burrow__tls_half_conn_set_traffic_secret(TlsHalfConn *hc,
                                               const TlsSuiteTLS13 *suite,
                                               TlsQUICEncryptionLevel level,
                                               Slice secret) {
    Arena *ar = thc_new_arena(hc);
    if (ar == NULL) {
        hc->err = burrow_err_out_of_memory;
        return hc->err;
    }
    Alloc *a = arena_allocator(ar);
    Slice s = slice_make(a, TYPE_BYTE, secret.len, secret.len);
    Slice key = slice_nil(TYPE_BYTE), iv = key;
    TlsCipher c = {0};
    if (s.p != NULL || secret.len == 0) {
        slice_copy(s, secret);
        if (burrow__tls13_traffic_key(suite, a, s, &key, &iv))
            c = suite->aead(a, key, iv);
    }
    if (burrow__tls_cipher_is_nil(c)) {
        thc_free_arena(hc, ar);
        hc->err = burrow_err_out_of_memory;
        return hc->err;
    }
    thc_free_arena(hc, hc->keys);
    hc->keys = ar;
    hc->traffic_secret = s;
    hc->level = level;
    hc->cipher = c;
    memset(hc->seq, 0, sizeof hc->seq);
    return BURROW_NO_ERROR;
}

void burrow__tls_half_conn_inc_seq(TlsHalfConn *hc) {
    for (int i = 7; i >= 0; i--) {
        hc->seq[i]++;
        if (hc->seq[i] != 0)
            return;
    }

    /* Not allowed to let sequence number wrap. Instead, must renegotiate before
     * it does. Not likely enough to bother. */
    panic_str(BURROW_S("TLS: sequence number wraparound"));
}

Int burrow__tls_half_conn_explicit_nonce_len(const TlsHalfConn *hc) {
    if (burrow__tls_cipher_is_nil(hc->cipher))
        return 0;

    if (hc->cipher.stream.vt != NULL)
        return 0;
    if (hc->cipher.aead.vt != NULL)
        return hc->cipher.explicit_nonce_len;
    /* TLS 1.1 introduced a per-record explicit IV to fix the BEAST attack. */
    if (hc->version >= TLS_VERSION_TLS11 && hc->cipher.mode.vt != NULL)
        return cipher_block_mode_block_size(hc->cipher.mode);
    return 0;
}

Int burrow__tls_extract_padding(Slice payload, Byte *good) {
    if (payload.len < 1 || payload.p == NULL) {
        *good = 0;
        return 0;
    }

    const Byte *p = (const Byte *)payload.p;
    Byte padding_len = p[payload.len - 1];
    uint64_t t = (uint64_t)(payload.len - 1) - (uint64_t)padding_len;
    /* if len(payload) >= (paddingLen - 1) then the MSB of t is zero */
    Byte g = (Byte)((int32_t)(uint32_t)~t >> 31);

    /* The maximum possible padding length plus the actual length field */
    Int to_check = 256;
    /* The length of the padded data is public, so we can use an if here */
    if (to_check > payload.len)
        to_check = payload.len;

    for (Int i = 0; i < to_check; i++) {
        uint64_t u = (uint64_t)padding_len - (uint64_t)i;
        /* if i <= paddingLen then the MSB of t is zero */
        Byte mask = (Byte)((int32_t)(uint32_t)~u >> 31);
        Byte b = p[payload.len - 1 - i];
        g &= (Byte) ~((mask & padding_len) ^ (mask & b));
    }

    /* We AND together the bits of good and replicate the result across all the
     * bits. */
    g &= (Byte)(g << 4);
    g &= (Byte)(g << 2);
    g &= (Byte)(g << 1);
    g = (Byte)((int8_t)g >> 7);

    /* Zero the padding length on error. This ensures any unchecked bytes are
     * included in the MAC. Otherwise, an attacker that could distinguish MAC
     * failures from padding failures could mount an attack similar to POODLE
     * in SSL 3.0: given a good ciphertext that uses a full block's worth of
     * padding, replace the final block with another block. If the MAC check
     * passed but the padding check failed, the last byte of that block
     * decrypted to the block size.
     *
     * See also macAndPaddingGood logic below. */
    padding_len &= g;

    *good = g;
    return (Int)padding_len + 1;
}

Int burrow__tls_round_up(Int a, Int b) {
    return a + (b - a % b) % b;
}

Slice burrow__tls_half_conn_decrypt(TlsHalfConn *hc, Slice record, uint8_t *typ,
                                    Error *err) {
    Slice plaintext = slice_nil(TYPE_BYTE);
    Byte *r = (Byte *)record.p;
    *typ = r[0];
    Slice payload = slice_sub(record, TLS_RECORD_HEADER_LEN, record.len);

    /* In TLS 1.3, change_cipher_spec messages are to be ignored without being
     * decrypted. See RFC 8446, Appendix D.4. */
    if (hc->version == TLS_VERSION_TLS13 && *typ == TLS_RECORD_TYPE_CHANGE_CIPHER_SPEC)
        return payload;

    Byte padding_good = 255;
    Int padding_len = 0;

    Int explicit_nonce_len = burrow__tls_half_conn_explicit_nonce_len(hc);

    if (!burrow__tls_cipher_is_nil(hc->cipher)) {
        if (hc->cipher.stream.vt != NULL) {
            cipher_stream_xor_key_stream(hc->cipher.stream, payload, payload);
        } else if (hc->cipher.aead.vt != NULL) {
            CipherAEAD c = hc->cipher.aead;
            if (payload.len < explicit_nonce_len) {
                *err = thc_alert(TLS_ALERT_BAD_RECORD_MAC);
                return slice_nil(TYPE_BYTE);
            }
            Slice nonce = slice_sub(payload, 0, explicit_nonce_len);
            if (nonce.len == 0)
                nonce = thc_seq(hc);
            payload = slice_sub(payload, explicit_nonce_len, payload.len);

            Byte scratch[13];
            Slice additional_data;
            if (hc->version == TLS_VERSION_TLS13) {
                additional_data = slice_sub(record, 0, TLS_RECORD_HEADER_LEN);
            } else {
                memcpy(scratch, hc->seq, 8);
                memcpy(scratch + 8, r, 3);
                Int n = payload.len - cipher_aead_overhead(c);
                scratch[11] = (Byte)((uint64_t)n >> 8);
                scratch[12] = (Byte)n;
                additional_data = slice_from(scratch, 13, 13, TYPE_BYTE);
            }

            Error open_err = BURROW_NO_ERROR;
            plaintext = cipher_aead_open(c, heap_allocator(), slice_sub(payload, 0, 0),
                                         nonce, payload, additional_data, &open_err);
            if (BURROW_FAILED(open_err)) {
                *err = thc_alert(TLS_ALERT_BAD_RECORD_MAC);
                return slice_nil(TYPE_BYTE);
            }
        } else if (hc->cipher.mode.vt != NULL) {
            CipherBlockMode c = hc->cipher.mode;
            Int block_size = cipher_block_mode_block_size(c);
            Int min_payload = explicit_nonce_len +
                              burrow__tls_round_up(hash_size(hc->mac) + 1, block_size);
            if (payload.len % block_size != 0 || payload.len < min_payload) {
                *err = thc_alert(TLS_ALERT_BAD_RECORD_MAC);
                return slice_nil(TYPE_BYTE);
            }

            if (explicit_nonce_len > 0) {
                cipher_cbc_set_iv(c, slice_sub(payload, 0, explicit_nonce_len));
                payload = slice_sub(payload, explicit_nonce_len, payload.len);
            }
            cipher_block_mode_crypt_blocks(c, payload, payload);

            /* In a limited attempt to protect against CBC padding oracles like
             * Lucky13, the data past paddingLen (which is secret) is passed to
             * the MAC function as extra data, to be fed into the HMAC after
             * computing the digest. This makes the MAC roughly constant time as
             * long as the digest computation is constant time and does not
             * affect the subsequent write, modulo cache effects. */
            padding_len = burrow__tls_extract_padding(payload, &padding_good);
        }

        if (hc->version == TLS_VERSION_TLS13) {
            if (*typ != TLS_RECORD_TYPE_APPLICATION_DATA) {
                *err = thc_alert(TLS_ALERT_UNEXPECTED_MESSAGE);
                return slice_nil(TYPE_BYTE);
            }
            if (plaintext.len > TLS_MAX_PLAINTEXT + 1) {
                *err = thc_alert(TLS_ALERT_RECORD_OVERFLOW);
                return slice_nil(TYPE_BYTE);
            }
            /* Remove padding and find the ContentType scanning from the end. */
            const Byte *p = (const Byte *)plaintext.p;
            for (Int i = plaintext.len - 1; i >= 0; i--) {
                if (p[i] != 0) {
                    *typ = p[i];
                    plaintext = slice_sub(plaintext, 0, i);
                    break;
                }
                if (i == 0) {
                    *err = thc_alert(TLS_ALERT_UNEXPECTED_MESSAGE);
                    return slice_nil(TYPE_BYTE);
                }
            }
        }
    } else {
        plaintext = payload;
    }

    if (hc->mac.vt != NULL) {
        Int mac_size = hash_size(hc->mac);
        if (payload.len < mac_size) {
            *err = thc_alert(TLS_ALERT_BAD_RECORD_MAC);
            return slice_nil(TYPE_BYTE);
        }

        Int n = payload.len - mac_size - padding_len;
        /* if n < 0 { n = 0 } */
        n = subtle_constant_time_select((Int)((uint32_t)n >> 31), 0, n);
        r[3] = (Byte)(n >> 8);
        r[4] = (Byte)n;
        Slice remote_mac = slice_sub(payload, n, n + mac_size);
        Byte local[64];
        Slice local_mac = burrow__tls10_mac(
            heap_allocator(), hc->mac, slice_from(local, 0, sizeof local, TYPE_BYTE),
            thc_seq(hc), slice_sub(record, 0, TLS_RECORD_HEADER_LEN),
            slice_sub(payload, 0, n), slice_sub(payload, n + mac_size, payload.len));

        /* This is equivalent to checking the MACs and paddingGood separately,
         * but in constant-time to prevent distinguishing padding failures from
         * MAC failures. Depending on what value of paddingLen was returned on
         * bad padding, distinguishing bad MAC from bad padding can lead to an
         * attack.
         *
         * See also the logic at the end of extractPadding. */
        Int mac_and_padding_good =
            subtle_constant_time_compare(local_mac, remote_mac) & (Int)padding_good;
        if (mac_and_padding_good != 1) {
            *err = thc_alert(TLS_ALERT_BAD_RECORD_MAC);
            return slice_nil(TYPE_BYTE);
        }

        plaintext = slice_sub(payload, 0, n);
    }

    burrow__tls_half_conn_inc_seq(hc);
    return plaintext;
}

/* sliceForAppend extends the input slice by n bytes. head is the full extended
 * slice, while tail is the appended part. If the original slice has sufficient
 * capacity no allocation is performed. head is nil when a is out of memory. */
static Slice thc_slice_for_append(Alloc *a, Slice in, Int n, Slice *tail) {
    Int total = in.len + n;
    Slice head;
    if (in.cap >= total) {
        head = slice_sub(in, 0, total);
    } else {
        head = slice_make(a, TYPE_BYTE, total, total);
        if (head.p == NULL) {
            *tail = head;
            return head;
        }
        slice_copy(head, in);
    }
    *tail = slice_sub(head, in.len, head.len);
    return head;
}

static Slice thc_oom(Error *err) {
    *err = burrow_err_out_of_memory;
    return slice_nil(TYPE_BYTE);
}

Slice burrow__tls_half_conn_encrypt(TlsHalfConn *hc, Alloc *a, Slice record,
                                    Slice payload, IoReader rand, Error *err) {
    if (burrow__tls_cipher_is_nil(hc->cipher)) {
        Slice out = slice_append_slice(a, record, payload);
        if (out.p == NULL && record.len + payload.len > 0)
            return thc_oom(err);
        return out;
    }

    Slice explicit_nonce = slice_nil(TYPE_BYTE);
    Int explicit_nonce_len = burrow__tls_half_conn_explicit_nonce_len(hc);
    if (explicit_nonce_len > 0) {
        record = thc_slice_for_append(a, record, explicit_nonce_len, &explicit_nonce);
        if (record.p == NULL)
            return thc_oom(err);
        if (hc->cipher.mode.vt == NULL && explicit_nonce_len < 16) {
            /* The AES-GCM construction in TLS has an explicit nonce so that the
             * nonce can be random. However, the nonce is only 8 bytes which is
             * too small for a secure, random nonce. Therefore we use the
             * sequence number as the nonce. The 3DES-CBC construction also has
             * an 8 bytes nonce but its nonces must be unpredictable (see RFC
             * 5246, Appendix F.3), forcing us to use randomness. That's not
             * 3DES' biggest problem anyway because the birthday bound on block
             * collision is reached first due to its similarly small block size
             * (see the Sweet32 attack). */
            slice_copy(explicit_nonce, thc_seq(hc));
        } else {
            Error read_err = BURROW_NO_ERROR;
            io_read_full(rand, explicit_nonce, &read_err);
            if (BURROW_FAILED(read_err)) {
                *err = read_err;
                return slice_nil(TYPE_BYTE);
            }
        }
    }

    Byte scratch[64];
    Slice dst = slice_nil(TYPE_BYTE);
    if (hc->cipher.stream.vt != NULL) {
        CipherStream c = hc->cipher.stream;
        Slice mac = burrow__tls10_mac(
            heap_allocator(), hc->mac,
            slice_from(scratch, 0, sizeof scratch, TYPE_BYTE), thc_seq(hc),
            slice_sub(record, 0, TLS_RECORD_HEADER_LEN), payload, slice_nil(TYPE_BYTE));
        record = thc_slice_for_append(a, record, payload.len + mac.len, &dst);
        if (record.p == NULL)
            return thc_oom(err);
        cipher_stream_xor_key_stream(c, slice_sub(dst, 0, payload.len), payload);
        cipher_stream_xor_key_stream(c, slice_sub(dst, payload.len, dst.len), mac);
    } else if (hc->cipher.aead.vt != NULL) {
        CipherAEAD c = hc->cipher.aead;
        Slice nonce = explicit_nonce;
        if (nonce.len == 0)
            nonce = thc_seq(hc);

        if (hc->version == TLS_VERSION_TLS13) {
            record = slice_append_slice(a, record, payload);
            if (record.p == NULL)
                return thc_oom(err);

            /* Encrypt the actual ContentType and replace the plaintext one. */
            Byte content_type = ((Byte *)record.p)[0];
            record = slice_append(a, record, &content_type, 1);
            if (record.p == NULL)
                return thc_oom(err);
            ((Byte *)record.p)[0] = TLS_RECORD_TYPE_APPLICATION_DATA;

            Int n = payload.len + 1 + cipher_aead_overhead(c);
            ((Byte *)record.p)[3] = (Byte)(n >> 8);
            ((Byte *)record.p)[4] = (Byte)n;

            record = cipher_aead_seal(
                c, a, slice_sub(record, 0, TLS_RECORD_HEADER_LEN), nonce,
                slice_sub(record, TLS_RECORD_HEADER_LEN, record.len),
                slice_sub(record, 0, TLS_RECORD_HEADER_LEN));
        } else {
            memcpy(scratch, hc->seq, 8);
            memcpy(scratch + 8, record.p, TLS_RECORD_HEADER_LEN);
            Slice additional_data = slice_from(scratch, 13, 13, TYPE_BYTE);
            record = cipher_aead_seal(c, a, record, nonce, payload, additional_data);
        }
        if (record.p == NULL)
            return thc_oom(err);
    } else if (hc->cipher.mode.vt != NULL) {
        CipherBlockMode c = hc->cipher.mode;
        Slice mac = burrow__tls10_mac(
            heap_allocator(), hc->mac,
            slice_from(scratch, 0, sizeof scratch, TYPE_BYTE), thc_seq(hc),
            slice_sub(record, 0, TLS_RECORD_HEADER_LEN), payload, slice_nil(TYPE_BYTE));
        Int block_size = cipher_block_mode_block_size(c);
        Int plaintext_len = payload.len + mac.len;
        Int padding_len = block_size - plaintext_len % block_size;
        record = thc_slice_for_append(a, record, plaintext_len + padding_len, &dst);
        if (record.p == NULL || dst.p == NULL)
            return thc_oom(err);
        slice_copy(dst, payload);
        slice_copy(slice_sub(dst, payload.len, dst.len), mac);
        memset((Byte *)dst.p + plaintext_len, (int)(padding_len - 1),
               (size_t)padding_len);
        if (explicit_nonce.len > 0)
            cipher_cbc_set_iv(c, explicit_nonce);
        cipher_block_mode_crypt_blocks(c, dst, dst);
    }

    /* Update length to include nonce, MAC and any block padding needed. */
    Int n = record.len - TLS_RECORD_HEADER_LEN;
    ((Byte *)record.p)[3] = (Byte)(n >> 8);
    ((Byte *)record.p)[4] = (Byte)n;
    burrow__tls_half_conn_inc_seq(hc);

    return record;
}

/* ------------------------------------------------------------------ errors */

static bool thc_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* permanentError, a net.Error that says it is not temporary, whatever the one
 * inside says. Unwrap is the vtable's rather than a method. */
typedef struct thc_PermanentError {
    Error err;
} thc_PermanentError;

static Str thc_perm_m_error(thc_PermanentError *self) {
    return error_text(self->err);
}

static bool thc_perm_m_temporary(thc_PermanentError *self) {
    (void)self;
    return false;
}

static bool thc_perm_m_timeout(thc_PermanentError *self) {
    return net_error_timeout(self->err);
}

#define THC_SIG_STRING(IN, OUT) OUT(Str)
#define THC_SIG_BOOL(IN, OUT) OUT(bool)

#define THC_PERM_METHODS(M, T)                                                         \
    M(T, Error, thc_perm_m_error, THC_SIG_STRING)                                      \
    M(T, Temporary, thc_perm_m_temporary, THC_SIG_BOOL)                                \
    M(T, Timeout, thc_perm_m_timeout, THC_SIG_BOOL)

BURROW_METHODS_DEFINE(thc_PermanentError, THC_PERM_METHODS);

#define THC_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

static const Type thc_perm_desc = {
    BURROW_S_INIT("permanentError"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(thc_PermanentError),
    (uint16_t)_Alignof(thc_PermanentError),
    0,
    THC_COUNT(burrow__methods_thc_PermanentError),
    NULL,
    burrow__methods_thc_PermanentError,
    NULL,
    NULL,
    0,
    0x746c7065U, /* "tlpe" */
    NULL,
};

static Str thc_perm_message(const void *self) {
    return error_text(((const thc_PermanentError *)self)->err);
}

static Error thc_perm_unwrap(const void *self) {
    return ((const thc_PermanentError *)self)->err;
}

static Error thc_perm_clone(const void *self, Alloc *a);

static const ErrorVT thc_perm_vt = {
    .self_type = &thc_perm_desc,
    .message = thc_perm_message,
    .unwrap = thc_perm_unwrap,
    .clone = thc_perm_clone,
};

static Error thc_perm_clone(const void *self, Alloc *a) {
    thc_PermanentError *e = BURROW_NEW(a, thc_PermanentError);
    if (e == NULL)
        return burrow_err_out_of_memory;
    e->err = error_retain(a, ((const thc_PermanentError *)self)->err);
    return (Error){&thc_perm_vt, e};
}

/* Not in Go. err copied into the arena of errors hc keeps, which lasts until
 * hc is freed. */
static Error thc_keep(TlsHalfConn *hc, Error err) {
    if (BURROW_OK(err))
        return err;
    if (hc->errs == NULL) {
        hc->errs = thc_new_arena(hc);
        if (hc->errs == NULL)
            return burrow_err_out_of_memory;
    }
    return error_retain(arena_allocator(hc->errs), err);
}

Error burrow__tls_half_conn_set_error_locked(TlsHalfConn *hc, Error err) {
    Error kept = thc_keep(hc, err);
    if (BURROW_FAILED(err) && net_is_error(err) &&
        !thc_same(kept, burrow_err_out_of_memory)) {
        thc_PermanentError *e =
            BURROW_NEW(arena_allocator(hc->errs), thc_PermanentError);
        if (e == NULL) {
            kept = burrow_err_out_of_memory;
        } else {
            e->err = kept;
            kept = (Error){&thc_perm_vt, e};
        }
    }
    hc->err = kept;
    return hc->err;
}

/* RecordHeaderError as an Error. The error first, so the data pointer is a
 * pointer to it and errors_as hands it straight back. */
typedef struct thc_RecordHeaderBox {
    TlsRecordHeaderError e;
    Str text;
} thc_RecordHeaderBox;

static const Type thc_record_header_error_desc = {
    BURROW_S_INIT("RecordHeaderError"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(TlsRecordHeaderError),
    (uint16_t)_Alignof(TlsRecordHeaderError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c7268U, /* "tlrh" */
    NULL,
};

const Type *const TYPE_TLS_RECORD_HEADER_ERROR = &thc_record_header_error_desc;

static Str thc_rhe_message(const void *self) {
    return ((const thc_RecordHeaderBox *)self)->text;
}

static Error thc_rhe_clone(const void *self, Alloc *a) {
    return tls_record_header_error_as_error(&((const thc_RecordHeaderBox *)self)->e, a);
}

static const ErrorVT thc_rhe_vt = {
    .self_type = &thc_record_header_error_desc,
    .message = thc_rhe_message,
    .clone = thc_rhe_clone,
};

Str tls_record_header_error_error(const TlsRecordHeaderError *e, Alloc *a) {
    return fmt_sprintf_v(a, "tls: %s", e->msg);
}

Error tls_record_header_error_as_error(const TlsRecordHeaderError *e, Alloc *a) {
    /* One block: the box, then the text, which ends with msg. */
    thc_RecordHeaderBox *b = (thc_RecordHeaderBox *)mem_alloc_nozero(
        a, sizeof(thc_RecordHeaderBox) + 5 + (size_t)e->msg.len,
        _Alignof(thc_RecordHeaderBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    memcpy(p, "tls: ", 5);
    if (e->msg.len > 0)
        memcpy(p + 5, e->msg.p, (size_t)e->msg.len);
    b->e = *e;
    b->e.msg = str_from_bytes(p + 5, e->msg.len);
    b->text = str_from_bytes(p, 5 + e->msg.len);
    return (Error){&thc_rhe_vt, b};
}

static Error thc_new_record_header_error(TlsConn *c, NetConn conn, Str msg) {
    TlsRecordHeaderError e;
    memset(&e, 0, sizeof e);
    e.msg = msg;
    e.conn = conn;
    Slice raw = bytes_buffer_bytes(&c->raw_input);
    Int n = raw.len < 5 ? raw.len : 5;
    if (n > 0)
        memcpy(e.record_header, raw.p, (size_t)n);
    return tls_record_header_error_as_error(&e, error_allocator());
}

/* &net.OpError{Op: op, Err: alert(e)}, which reads as "remote error: tls: bad
 * certificate". */
static Error thc_op_alert(Str op, uint8_t e) {
    NetOpError oe;
    memset(&oe, 0, sizeof oe);
    oe.op = op;
    oe.err = burrow__tls_alert(error_allocator(), e);
    return net_op_error_as_error(&oe, error_allocator());
}

/* err.(alert), which is what decrypt and changeCipherSpec fail with. */
static uint8_t thc_alert_of(Error err) {
    uint8_t e = TLS_ALERT_INTERNAL_ERROR;
    (void)burrow__tls_is_alert(err, &e);
    return e;
}

BURROW_SENTINEL_ERROR(thc_err_pending_data,
                      "tls: internal error: attempted to read record with pending "
                      "application data");
BURROW_SENTINEL_ERROR(thc_err_too_many_ignored, "tls: too many ignored records");
BURROW_SENTINEL_ERROR(thc_err_quic_read_record,
                      "tls: internal error: attempted to read record with QUIC "
                      "transport");
BURROW_SENTINEL_ERROR(thc_err_quic_non_handshake,
                      "tls: internal error: sending non-handshake message to QUIC "
                      "transport");
BURROW_SENTINEL_ERROR(thc_err_quic_key_update,
                      "tls: received unexpected key update message");
BURROW_SENTINEL_ERROR(thc_err_quic_early_data,
                      "tls: invalid early data for QUIC connection");
BURROW_SENTINEL_ERROR(thc_err_too_many_non_advancing,
                      "tls: too many non-advancing records");
BURROW_SENTINEL_ERROR(thc_err_unexpected_renegotiation,
                      "tls: internal error: unexpected renegotiation");
BURROW_SENTINEL_ERROR(thc_err_unknown_renegotiation,
                      "tls: unknown Renegotiation value");
BURROW_SENTINEL_ERROR(thc_err_hand_not_empty,
                      "tls: handshake buffer not empty before setting read traffic "
                      "secret");
BURROW_SENTINEL_ERROR(burrow__tls_err_shutdown, "tls: protocol is shutdown");
BURROW_SENTINEL_ERROR(burrow__tls_err_early_close_write,
                      "tls: CloseWrite called before handshake complete");
BURROW_SENTINEL_ERROR(thc_err_no_result,
                      "tls: internal error: handshake should have had a result");
BURROW_SENTINEL_ERROR(thc_err_verify_on_server,
                      "tls: VerifyHostname called on TLS server connection");
BURROW_SENTINEL_ERROR(thc_err_no_handshake,
                      "tls: handshake has not yet been performed");
BURROW_SENTINEL_ERROR(thc_err_no_verified_chain,
                      "tls: handshake did not verify certificate chain");
BURROW_SENTINEL_ERROR(thc_err_ticket_from_client,
                      "tls: received new session ticket from a client");
BURROW_SENTINEL_ERROR(thc_err_ticket_lifetime,
                      "tls: received a session ticket with invalid lifetime");
BURROW_SENTINEL_ERROR(thc_err_ticket_empty_label,
                      "tls: received a session ticket with empty opaque ticket label");

/* -------------------------------------------------------------------- Conn */

static Int thc_conn_read(NetConn c, Slice p, Error *err) {
    return c.vt->reader.read(c.data, p, err);
}

static Int thc_conn_write(NetConn c, Slice p, Error *err) {
    return c.vt->writer.write(c.data, p, err);
}

static Error thc_conn_close(NetConn c) {
    return c.vt->closer.close(c.data);
}

TlsConn *burrow__tls_conn_new(Alloc *a, NetConn conn, TlsConfig *config, bool is_client,
                              TlsHandshakeFn fn) {
    TlsConn *c = BURROW_NEW(a, TlsConn);
    if (c == NULL)
        return NULL;
    c->a = a;
    c->conn = conn;
    c->config = config;
    c->is_client = is_client;
    c->handshake_fn = fn;
    burrow__tls_half_conn_init(&c->in, a);
    burrow__tls_half_conn_init(&c->out, a);
    c->raw_input = BYTES_BUFFER(a);
    c->hand = BYTES_BUFFER(a);
    c->send_buf = BYTES_BUFFER(a);
    c->input = slice_nil(TYPE_BYTE);
    c->ocsp_response = slice_nil(TYPE_BYTE);
    c->resumption_secret = slice_nil(TYPE_BYTE);
    arena_init(&c->state, a, 0);
    arena_init(&c->out_bufs, a, 0);
    return c;
}

void tls_conn_free(TlsConn *c) {
    if (c == NULL)
        return;
    burrow__tls_half_conn_free(&c->in);
    burrow__tls_half_conn_free(&c->out);
    bytes_buffer_free(&c->raw_input);
    bytes_buffer_free(&c->hand);
    bytes_buffer_free(&c->send_buf);
    if (c->ticket_keys != NULL)
        burrow__tls_ticket_keys_release(c->ticket_keys);
    if (c->resumed_session != NULL)
        tls_client_session_state_release(c->resumed_session);
    if (c->server_session != NULL)
        tls_session_state_free(c->server_session);
    arena_free(&c->state);
    arena_free(&c->out_bufs);
    if (c->owns_conn)
        net_conn_free(c->conn);
    if (c->owned_config != NULL) {
        tls_config_release(c->owned_config);
        mem_free(c->a, c->owned_config, sizeof *c->owned_config, _Alignof(TlsConfig));
    }
    if (c->owned_server_name.len > 0)
        mem_free(c->a, (void *)(uintptr_t)c->owned_server_name.p,
                 (size_t)c->owned_server_name.len, 1);
    mem_free(c->a, c, sizeof *c, _Alignof(TlsConn));
}

NetAddr tls_conn_local_addr(TlsConn *c) {
    return c->conn.vt->local_addr(c->conn.data);
}

NetAddr tls_conn_remote_addr(TlsConn *c) {
    return c->conn.vt->remote_addr(c->conn.data);
}

Error tls_conn_set_deadline(TlsConn *c, Time t) {
    return c->conn.vt->set_deadline(c->conn.data, t);
}

Error tls_conn_set_read_deadline(TlsConn *c, Time t) {
    return c->conn.vt->set_read_deadline(c->conn.data, t);
}

Error tls_conn_set_write_deadline(TlsConn *c, Time t) {
    return c->conn.vt->set_write_deadline(c->conn.data, t);
}

NetConn tls_conn_net_conn(TlsConn *c) {
    return c->conn;
}

static Error thc_read_record_or_ccs(TlsConn *c, bool expect_change_cipher_spec);

Error burrow__tls_conn_read_record(TlsConn *c) {
    return thc_read_record_or_ccs(c, false);
}

Error burrow__tls_conn_read_change_cipher_spec(TlsConn *c) {
    return thc_read_record_or_ccs(c, true);
}

/* c.in.setErrorLocked(c.sendAlert(e)). */
static Error thc_in_alert(TlsConn *c, uint8_t e) {
    return burrow__tls_half_conn_set_error_locked(&c->in,
                                                  burrow__tls_conn_send_alert(c, e));
}

/* retryReadRecord recurs into readRecordOrCCS to drop a non-advancing record,
 * like a warning alert, empty application_data, or a change_cipher_spec in TLS
 * 1.3. */
static Error thc_retry_read_record(TlsConn *c, bool expect_change_cipher_spec) {
    c->retry_count++;
    if (c->retry_count > TLS_MAX_USELESS_RECORDS) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_half_conn_set_error_locked(&c->in, thc_err_too_many_ignored);
    }
    return thc_read_record_or_ccs(c, expect_change_cipher_spec);
}

/* readFromUntil reads from r into c.rawInput until c.rawInput contains at least
 * n bytes or else returns an error. */
static Error thc_read_from_until(TlsConn *c, NetConn r, Int n) {
    if (bytes_buffer_len(&c->raw_input) >= n)
        return BURROW_NO_ERROR;
    Int needs = n - bytes_buffer_len(&c->raw_input);
    /* There might be extra input waiting on the wire. Make a best effort
     * attempt to fetch it so that it can be used in (*Conn).Read to "predict"
     * closeNotify alerts. */
    if (!bytes_buffer_grow(&c->raw_input, needs + BYTES_MIN_READ))
        return burrow_err_out_of_memory;
    for (;;) {
        Slice buf = bytes_buffer_available_buffer(&c->raw_input);
        buf = slice_sub(buf, 0, bytes_buffer_available(&c->raw_input));
        Error err = BURROW_NO_ERROR;
        Int m = thc_conn_read(r, buf, &err);
        /* This write is just to update the internal state of the rawInput
         * bytes.Buffer. It cannot fail. */
        (void)bytes_buffer_write(&c->raw_input, slice_sub(buf, 0, m), NULL);
        needs -= m;
        if (needs <= 0) {
            if (thc_same(err, io_eof))
                err = BURROW_NO_ERROR;
            return err;
        }
        if (thc_same(err, io_eof))
            return io_err_unexpected_eof;
        if (BURROW_FAILED(err))
            return err;
    }
}

/* readRecordOrCCS reads one or more TLS records from the connection and
 * updates the record layer state. Some invariants:
 *   - c.in must be locked
 *   - c.input must be empty
 *
 * During the handshake one and only one of the following will happen:
 *   - c.hand grows
 *   - c.in.changeCipherSpec is called
 *   - an error is returned
 *
 * After the handshake one and only one of the following will happen:
 *   - c.hand grows
 *   - c.input is set
 *   - an error is returned */
static Error thc_read_record_or_ccs(TlsConn *c, bool expect_change_cipher_spec) {
    if (BURROW_FAILED(c->in.err))
        return c->in.err;
    bool handshake_complete = sync_atomic_bool_load(&c->is_handshake_complete);

    /* This function modifies c.rawInput, which owns the c.input memory. */
    if (c->input.len != 0)
        return burrow__tls_half_conn_set_error_locked(&c->in, thc_err_pending_data);
    c->input = slice_nil(TYPE_BYTE);

    if (c->quic != NULL)
        return burrow__tls_half_conn_set_error_locked(&c->in, thc_err_quic_read_record);

    NetConn none;
    memset(&none, 0, sizeof none);

    /* Read header, payload. */
    Error err = thc_read_from_until(c, c->conn, TLS_RECORD_HEADER_LEN);
    if (BURROW_FAILED(err)) {
        /* RFC 8446, Section 6.1 suggests that EOF without an alertCloseNotify
         * is an error, but popular web sites seem to do this, so we accept it
         * if and only if at the record boundary. */
        if (thc_same(err, io_err_unexpected_eof) &&
            bytes_buffer_len(&c->raw_input) == 0)
            err = io_eof;
        if (!net_is_error(err) || !net_error_temporary(err))
            (void)burrow__tls_half_conn_set_error_locked(&c->in, err);
        return err;
    }
    const Byte *hdr = (const Byte *)bytes_buffer_bytes(&c->raw_input).p;
    uint8_t typ = hdr[0];

    /* No valid TLS record has a type of 0x80, however SSLv2 handshakes start
     * with a uint16 length where the MSB is set and the first record is always
     * < 256 bytes long. Therefore typ == 0x80 strongly suggests an SSLv2
     * client. */
    if (!handshake_complete && typ == 0x80) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_PROTOCOL_VERSION);
        return burrow__tls_half_conn_set_error_locked(
            &c->in, thc_new_record_header_error(
                        c, none, BURROW_S("unsupported SSLv2 handshake received")));
    }

    uint16_t vers = (uint16_t)((unsigned)hdr[1] << 8 | hdr[2]);
    uint16_t expected_vers = c->vers;
    if (expected_vers == TLS_VERSION_TLS13) {
        /* All TLS 1.3 records are expected to have 0x0303 (1.2) after the
         * initial hello (RFC 8446 Section 5.1). */
        expected_vers = TLS_VERSION_TLS12;
    }
    Int n = (Int)hdr[3] << 8 | (Int)hdr[4];
    if (c->have_vers && vers != expected_vers) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_PROTOCOL_VERSION);
        Str msg = fmt_sprintf_v(error_allocator(),
                                "received record with version %x when expecting "
                                "version %x",
                                (Int)vers, (Int)expected_vers);
        return burrow__tls_half_conn_set_error_locked(
            &c->in, thc_new_record_header_error(c, none, msg));
    }
    if (!c->have_vers) {
        /* First message, be extra suspicious: this might not be a TLS client.
         * Bail out before reading a full 'body', if possible. The current max
         * version is 3.3 so if the version is >= 16.0, it's probably not
         * real. */
        if ((typ != TLS_RECORD_TYPE_ALERT && typ != TLS_RECORD_TYPE_HANDSHAKE) ||
            vers >= 0x1000)
            return burrow__tls_half_conn_set_error_locked(
                &c->in,
                thc_new_record_header_error(
                    c, c->conn,
                    BURROW_S("first record does not look like a TLS handshake")));
    }
    if ((c->vers == TLS_VERSION_TLS13 && n > TLS_MAX_CIPHERTEXT_TLS13) ||
        n > TLS_MAX_CIPHERTEXT) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_RECORD_OVERFLOW);
        Str msg = fmt_sprintf_v(error_allocator(),
                                "oversized record received with length %d", n);
        return burrow__tls_half_conn_set_error_locked(
            &c->in, thc_new_record_header_error(c, none, msg));
    }
    err = thc_read_from_until(c, c->conn, TLS_RECORD_HEADER_LEN + n);
    if (BURROW_FAILED(err)) {
        if (!net_is_error(err) || !net_error_temporary(err))
            (void)burrow__tls_half_conn_set_error_locked(&c->in, err);
        return err;
    }

    /* Process message. */
    Slice record = bytes_buffer_next(&c->raw_input, TLS_RECORD_HEADER_LEN + n);
    Slice data = burrow__tls_half_conn_decrypt(&c->in, record, &typ, &err);
    if (BURROW_FAILED(err))
        return thc_in_alert(c, thc_alert_of(err));
    /* Not in Go: decrypt never gives bytes without memory, and this says so
     * to the analyzer. */
    if (data.len > 0 && data.p == NULL)
        return thc_in_alert(c, TLS_ALERT_INTERNAL_ERROR);
    if (data.len > TLS_MAX_PLAINTEXT)
        return thc_in_alert(c, TLS_ALERT_RECORD_OVERFLOW);

    /* Application Data messages are always protected. */
    if (burrow__tls_cipher_is_nil(c->in.cipher) &&
        typ == TLS_RECORD_TYPE_APPLICATION_DATA)
        return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);

    if ((typ == TLS_RECORD_TYPE_APPLICATION_DATA ||
         (typ == TLS_RECORD_TYPE_HANDSHAKE && !handshake_complete)) &&
        data.len > 0) {
        /* This is a state-advancing message: reset the retry count. */
        c->retry_count = 0;
    }

    /* Handshake messages MUST NOT be interleaved with other record types in TLS
     * 1.3. */
    if (c->vers == TLS_VERSION_TLS13 && typ != TLS_RECORD_TYPE_HANDSHAKE &&
        bytes_buffer_len(&c->hand) > 0)
        return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);

    const Byte *d = (const Byte *)data.p;
    switch (typ) {
    case TLS_RECORD_TYPE_ALERT:
        if (c->quic != NULL)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        if (data.len != 2)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        if (d[1] == TLS_ALERT_CLOSE_NOTIFY)
            return burrow__tls_half_conn_set_error_locked(&c->in, io_eof);
        if (c->vers == TLS_VERSION_TLS13) {
            /* TLS 1.3 removed warning-level alerts except for
             * alertUserCanceled (RFC 8446, § 6.1). Since at least one major
             * implementation (https://bugs.openjdk.org/browse/JDK-8323517)
             * misuses this alert, many TLS stacks now ignore it outright when
             * seen in a TLS 1.3 handshake (e.g. BoringSSL, NSS, Rustls). */
            if (d[1] == TLS_ALERT_USER_CANCELED) {
                /* Like TLS 1.2 alertLevelWarning alerts, we drop the record and
                 * retry. */
                return thc_retry_read_record(c, expect_change_cipher_spec);
            }
            return burrow__tls_half_conn_set_error_locked(
                &c->in, thc_op_alert(BURROW_S("remote error"), d[1]));
        }
        switch (d[0]) {
        case TLS_ALERT_LEVEL_WARNING:
            /* Drop the record on the floor and retry. */
            return thc_retry_read_record(c, expect_change_cipher_spec);
        case TLS_ALERT_LEVEL_ERROR:
            return burrow__tls_half_conn_set_error_locked(
                &c->in, thc_op_alert(BURROW_S("remote error"), d[1]));
        default:
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        }

    case TLS_RECORD_TYPE_CHANGE_CIPHER_SPEC:
        if (data.len != 1 || d[0] != 1)
            return thc_in_alert(c, TLS_ALERT_DECODE_ERROR);
        /* Handshake messages are not allowed to fragment across the CCS. */
        if (bytes_buffer_len(&c->hand) > 0)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        /* In TLS 1.3, change_cipher_spec records are ignored until the
         * Finished. See RFC 8446, Appendix D.4. Note that according to Section
         * 5, a server can send a ChangeCipherSpec before its ServerHello, when
         * c.vers is still unset. That's not useful though and suspicious if the
         * server then selects a lower protocol version, so don't allow that. */
        if (c->vers == TLS_VERSION_TLS13)
            return thc_retry_read_record(c, expect_change_cipher_spec);
        if (!expect_change_cipher_spec)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        err = burrow__tls_half_conn_change_cipher_spec(&c->in);
        if (BURROW_FAILED(err))
            return thc_in_alert(c, thc_alert_of(err));
        break;

    case TLS_RECORD_TYPE_APPLICATION_DATA:
        if (!handshake_complete || expect_change_cipher_spec)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        /* Some OpenSSL servers send empty records in order to randomize the
         * CBC IV. Ignore a limited number of empty records. */
        if (data.len == 0)
            return thc_retry_read_record(c, expect_change_cipher_spec);
        /* Note that data is owned by c.rawInput, following the Next call
         * above, to avoid copying the plaintext. This is safe because
         * c.rawInput is not read from or written to until c.input is
         * drained. */
        c->input = data;
        break;

    case TLS_RECORD_TYPE_HANDSHAKE:
        if (data.len == 0 || expect_change_cipher_spec)
            return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        (void)bytes_buffer_write(&c->hand, data, &err);
        /* Not in Go: the buffer could not grow. */
        if (BURROW_FAILED(err))
            return burrow__tls_half_conn_set_error_locked(&c->in, err);
        break;

    default:
        return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
    }

    return BURROW_NO_ERROR;
}

Error burrow__tls_conn_send_alert_locked(TlsConn *c, uint8_t err) {
    if (c->quic != NULL)
        return burrow__tls_half_conn_set_error_locked(
            &c->out, thc_op_alert(BURROW_S("local error"), err));

    switch (err) {
    case TLS_ALERT_NO_RENEGOTIATION:
    case TLS_ALERT_CLOSE_NOTIFY:
        c->tmp[0] = TLS_ALERT_LEVEL_WARNING;
        break;
    default:
        c->tmp[0] = TLS_ALERT_LEVEL_ERROR;
        break;
    }
    c->tmp[1] = err;

    Error write_err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_record_locked(
        c, TLS_RECORD_TYPE_ALERT, slice_from(c->tmp, 2, 2, TYPE_BYTE), &write_err);
    if (err == TLS_ALERT_CLOSE_NOTIFY) {
        /* closeNotify is a special case in that it isn't an error. */
        return write_err;
    }

    return burrow__tls_half_conn_set_error_locked(
        &c->out, thc_op_alert(BURROW_S("local error"), err));
}

Error burrow__tls_conn_send_alert(TlsConn *c, uint8_t err) {
    sync_mutex_lock(&c->out.mu);
    Error ret = burrow__tls_conn_send_alert_locked(c, err);
    sync_mutex_unlock(&c->out.mu);
    return ret;
}

Int burrow__tls_conn_max_payload_size_for_write(TlsConn *c, uint8_t typ) {
    if (c->config->dynamic_record_sizing_disabled ||
        typ != TLS_RECORD_TYPE_APPLICATION_DATA)
        return TLS_MAX_PLAINTEXT;

    if (c->bytes_sent >= TLS_RECORD_SIZE_BOOST_THRESHOLD)
        return TLS_MAX_PLAINTEXT;

    /* Subtract TLS overheads to get the maximum payload size. */
    Int payload_bytes = TLS_TCP_MSS_ESTIMATE - TLS_RECORD_HEADER_LEN -
                        burrow__tls_half_conn_explicit_nonce_len(&c->out);
    if (!burrow__tls_cipher_is_nil(c->out.cipher)) {
        if (c->out.cipher.stream.vt != NULL) {
            payload_bytes -= hash_size(c->out.mac);
        } else if (c->out.cipher.aead.vt != NULL) {
            payload_bytes -= cipher_aead_overhead(c->out.cipher.aead);
        } else if (c->out.cipher.mode.vt != NULL) {
            Int block_size = cipher_block_mode_block_size(c->out.cipher.mode);
            /* The payload must fit in a multiple of blockSize, with room for
             * at least one padding byte. */
            payload_bytes = (payload_bytes & ~(block_size - 1)) - 1;
            /* The MAC is appended before padding so affects the payload size
             * directly. */
            payload_bytes -= hash_size(c->out.mac);
        }
    }
    if (c->vers == TLS_VERSION_TLS13)
        payload_bytes--; /* encrypted ContentType */

    /* Allow packet growth in arithmetic progression up to max. */
    int64_t pkt = c->packets_sent;
    c->packets_sent++;
    if (pkt > 1000)
        return TLS_MAX_PLAINTEXT; /* avoid overflow in multiply below */

    Int n = payload_bytes * (Int)(pkt + 1);
    if (n > TLS_MAX_PLAINTEXT)
        n = TLS_MAX_PLAINTEXT;
    return n;
}

static Int thc_write(TlsConn *c, Slice data, Error *err) {
    if (c->buffering) {
        /* Not in Go: the buffer can fail to grow. */
        Int n = bytes_buffer_write(&c->send_buf, data, err);
        return n;
    }

    Int n = thc_conn_write(c->conn, data, err);
    c->bytes_sent += n;
    return n;
}

Int burrow__tls_conn_flush(TlsConn *c, Error *err) {
    if (bytes_buffer_len(&c->send_buf) == 0)
        return 0;

    Int n = thc_conn_write(c->conn, bytes_buffer_bytes(&c->send_buf), err);
    c->bytes_sent += n;
    bytes_buffer_reset(&c->send_buf);
    c->buffering = false;
    return n;
}

Int burrow__tls_conn_write_record_locked(TlsConn *c, uint8_t typ, Slice data,
                                         Error *err) {
    if (c->quic != NULL) {
        if (typ != TLS_RECORD_TYPE_HANDSHAKE) {
            BURROW_OUT(err, thc_err_quic_non_handshake);
            return 0;
        }
        /* Not in Go: the copy can run out of memory. */
        Error e = burrow__tls_conn_quic_write_crypto_data(c, c->out.level, data);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        if (!c->buffering) {
            Error ferr = BURROW_NO_ERROR;
            (void)burrow__tls_conn_flush(c, &ferr);
            if (BURROW_FAILED(ferr)) {
                BURROW_OUT(err, ferr);
                return 0;
            }
        }
        return data.len;
    }

    Int n = 0;
    while (data.len > 0) {
        Int m = data.len;
        Int max_payload = burrow__tls_conn_max_payload_size_for_write(c, typ);
        if (m > max_payload)
            m = max_payload;

        /* Not in Go: in place of outBufPool, out_bufs gives the record room
         * for whatever encrypt adds, and takes it back once it is written. */
        ArenaMark mark = arena_mark(&c->out_bufs);
        Alloc *a = arena_allocator(&c->out_bufs);
        Slice out_buf = slice_make(a, TYPE_BYTE, TLS_RECORD_HEADER_LEN,
                                   TLS_RECORD_HEADER_LEN + m + TLS_MAX_CIPHERTEXT -
                                       TLS_MAX_PLAINTEXT);
        if (out_buf.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return n;
        }
        Byte *hdr = (Byte *)out_buf.p;
        hdr[0] = typ;
        uint16_t vers = c->vers;
        if (vers == 0) {
            /* Some TLS servers fail if the record version is greater than TLS
             * 1.0 for the initial ClientHello. */
            vers = TLS_VERSION_TLS10;
        } else if (vers == TLS_VERSION_TLS13) {
            /* TLS 1.3 froze the record layer version to 1.2. See RFC 8446,
             * Section 5.1. */
            vers = TLS_VERSION_TLS12;
        }
        hdr[1] = (Byte)(vers >> 8);
        hdr[2] = (Byte)vers;
        hdr[3] = (Byte)(m >> 8);
        hdr[4] = (Byte)m;

        Error e = BURROW_NO_ERROR;
        out_buf =
            burrow__tls_half_conn_encrypt(&c->out, a, out_buf, slice_sub(data, 0, m),
                                          burrow__tls_config_rand(c->config), &e);
        if (BURROW_OK(e))
            (void)thc_write(c, out_buf, &e);
        arena_release(&c->out_bufs, mark);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        n += m;
        data = slice_sub(data, m, data.len);
    }

    if (typ == TLS_RECORD_TYPE_CHANGE_CIPHER_SPEC && c->vers != TLS_VERSION_TLS13) {
        Error e = burrow__tls_half_conn_change_cipher_spec(&c->out);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, burrow__tls_conn_send_alert_locked(c, thc_alert_of(e)));
            return n;
        }
    }

    return n;
}

static void thc_transcript_write(IoWriter transcript, Slice data) {
    if (transcript.vt == NULL)
        return;
    Error err = BURROW_NO_ERROR;
    (void)transcript.vt->write(transcript.data, data, &err);
}

Int burrow__tls_conn_write_handshake_record(TlsConn *c, const TlsHandshakeMsg *msg,
                                            IoWriter transcript, Error *err) {
    sync_mutex_lock(&c->out.mu);

    ArenaMark mark = arena_mark(&c->out_bufs);
    Error e = BURROW_NO_ERROR;
    Slice data =
        burrow__tls_handshake_msg_marshal(msg, arena_allocator(&c->out_bufs), &e);
    Int n = 0;
    if (BURROW_OK(e)) {
        thc_transcript_write(transcript, data);
        n = burrow__tls_conn_write_record_locked(c, TLS_RECORD_TYPE_HANDSHAKE, data,
                                                 &e);
    }
    arena_release(&c->out_bufs, mark);

    sync_mutex_unlock(&c->out.mu);
    if (BURROW_FAILED(e))
        BURROW_OUT(err, e);
    return n;
}

Error burrow__tls_conn_write_change_cipher_record(TlsConn *c) {
    sync_mutex_lock(&c->out.mu);
    Byte one = 1;
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_write_record_locked(c, TLS_RECORD_TYPE_CHANGE_CIPHER_SPEC,
                                               slice_from(&one, 1, 1, TYPE_BYTE), &err);
    sync_mutex_unlock(&c->out.mu);
    return err;
}

/* readHandshakeBytes reads handshake data until c.hand contains at least n
 * bytes. */
static Error thc_read_handshake_bytes(TlsConn *c, Int n) {
    if (c->quic != NULL)
        return burrow__tls_conn_quic_read_handshake_bytes(c, n);
    while (bytes_buffer_len(&c->hand) < n) {
        Error err = burrow__tls_conn_read_record(c);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

static Error thc_unmarshal_handshake_message(TlsConn *c, Slice data,
                                             IoWriter transcript, Alloc *a,
                                             TlsHandshakeMsg *m);

Error burrow__tls_conn_read_handshake(TlsConn *c, IoWriter transcript, Alloc *a,
                                      TlsHandshakeMsg *msg) {
    Error err = thc_read_handshake_bytes(c, 4);
    if (BURROW_FAILED(err))
        return err;
    const Byte *data = (const Byte *)bytes_buffer_bytes(&c->hand).p;

    Int max_handshake_size = TLS_MAX_HANDSHAKE;
    /* hasVers indicates we're past the first message, forcing someone trying
     * to make us just allocate a large buffer to at least do the initial part
     * of the handshake first. */
    if (c->have_vers && data[0] == TLS_TYPE_CERTIFICATE) {
        /* Since certificate messages are likely to be the only messages that
         * can be larger than maxHandshake, we use a special limit for just
         * those messages. */
        max_handshake_size = TLS_MAX_HANDSHAKE_CERTIFICATE_MSG;
    }

    Int n = (Int)data[1] << 16 | (Int)data[2] << 8 | (Int)data[3];
    if (n > max_handshake_size) {
        /* Not in Go: Go calls sendAlertLocked here without holding c.out, and
         * this takes it. */
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return burrow__tls_half_conn_set_error_locked(
            &c->in, fmt_errorf_v("tls: handshake message of length %d bytes exceeds "
                                 "maximum of %d bytes",
                                 n, max_handshake_size));
    }
    err = thc_read_handshake_bytes(c, 4 + n);
    if (BURROW_FAILED(err))
        return err;
    Slice next = bytes_buffer_next(&c->hand, 4 + n);
    return thc_unmarshal_handshake_message(c, next, transcript, a, msg);
}

static Error thc_unmarshal_handshake_message(TlsConn *c, Slice data,
                                             IoWriter transcript, Alloc *a,
                                             TlsHandshakeMsg *m) {
    bool tls13 = c->vers == TLS_VERSION_TLS13;
    TlsMsgKind kind;
    switch (((const Byte *)data.p)[0]) {
    case TLS_TYPE_HELLO_REQUEST:
        kind = TLS_MSG_HELLO_REQUEST;
        break;
    case TLS_TYPE_CLIENT_HELLO:
        kind = TLS_MSG_CLIENT_HELLO;
        break;
    case TLS_TYPE_SERVER_HELLO:
        kind = TLS_MSG_SERVER_HELLO;
        break;
    case TLS_TYPE_NEW_SESSION_TICKET:
        kind = tls13 ? TLS_MSG_NEW_SESSION_TICKET_TLS13 : TLS_MSG_NEW_SESSION_TICKET;
        break;
    case TLS_TYPE_CERTIFICATE:
        kind = tls13 ? TLS_MSG_CERTIFICATE_TLS13 : TLS_MSG_CERTIFICATE;
        break;
    case TLS_TYPE_CERTIFICATE_REQUEST:
        kind = tls13 ? TLS_MSG_CERTIFICATE_REQUEST_TLS13 : TLS_MSG_CERTIFICATE_REQUEST;
        break;
    case TLS_TYPE_CERTIFICATE_STATUS:
        kind = TLS_MSG_CERTIFICATE_STATUS;
        break;
    case TLS_TYPE_SERVER_KEY_EXCHANGE:
        kind = TLS_MSG_SERVER_KEY_EXCHANGE;
        break;
    case TLS_TYPE_SERVER_HELLO_DONE:
        kind = TLS_MSG_SERVER_HELLO_DONE;
        break;
    case TLS_TYPE_CLIENT_KEY_EXCHANGE:
        kind = TLS_MSG_CLIENT_KEY_EXCHANGE;
        break;
    case TLS_TYPE_CERTIFICATE_VERIFY:
        kind = TLS_MSG_CERTIFICATE_VERIFY;
        break;
    case TLS_TYPE_FINISHED:
        kind = TLS_MSG_FINISHED;
        break;
    case TLS_TYPE_ENCRYPTED_EXTENSIONS:
        kind = TLS_MSG_ENCRYPTED_EXTENSIONS;
        break;
    case TLS_TYPE_END_OF_EARLY_DATA:
        kind = TLS_MSG_END_OF_EARLY_DATA;
        break;
    case TLS_TYPE_KEY_UPDATE:
        kind = TLS_MSG_KEY_UPDATE;
        break;
    default:
        return thc_in_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
    }
    burrow__tls_handshake_msg_init(m, kind);
    if (kind == TLS_MSG_CERTIFICATE_REQUEST)
        m->certificate_request.has_signature_algorithm = c->vers >= TLS_VERSION_TLS12;
    if (kind == TLS_MSG_CERTIFICATE_VERIFY)
        m->certificate_verify.has_signature_algorithm = c->vers >= TLS_VERSION_TLS12;

    /* The handshake message unmarshalers expect to be able to keep references
     * to data, so pass in a fresh copy that won't be overwritten. */
    Slice copy = slice_make(a, TYPE_BYTE, data.len, data.len);
    if (copy.p == NULL) {
        /* Not in Go: there was no room for the copy. */
        return burrow__tls_half_conn_set_error_locked(&c->in, burrow_err_out_of_memory);
    }
    slice_copy(copy, data);

    if (!burrow__tls_handshake_msg_unmarshal(m, a, copy))
        return thc_in_alert(c, TLS_ALERT_DECODE_ERROR);

    thc_transcript_write(transcript, copy);

    return BURROW_NO_ERROR;
}

static Int thc_write_locked_app(TlsConn *c, Slice b, Error *err) {
    if (BURROW_FAILED(c->out.err)) {
        BURROW_OUT(err, c->out.err);
        return 0;
    }

    if (!sync_atomic_bool_load(&c->is_handshake_complete)) {
        BURROW_OUT(err, thc_alert(TLS_ALERT_INTERNAL_ERROR));
        return 0;
    }

    if (c->close_notify_sent) {
        BURROW_OUT(err, burrow__tls_err_shutdown);
        return 0;
    }

    /* TLS 1.0 is susceptible to a chosen-plaintext attack when using block
     * mode ciphers due to predictable IVs. This can be prevented by splitting
     * each Application Data record into two records, effectively randomizing
     * the IV.
     *
     * https://www.openssl.org/~bodo/tls-cbc.txt
     * https://bugzilla.mozilla.org/show_bug.cgi?id=665814
     * https://www.imperialviolet.org/2012/01/15/beastfollowup.html */

    Int m = 0;
    if (b.len > 1 && c->vers == TLS_VERSION_TLS10) {
        if (c->out.cipher.mode.vt != NULL) {
            Error e = BURROW_NO_ERROR;
            Int n = burrow__tls_conn_write_record_locked(
                c, TLS_RECORD_TYPE_APPLICATION_DATA, slice_sub(b, 0, 1), &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, burrow__tls_half_conn_set_error_locked(&c->out, e));
                return n;
            }
            m = 1;
            b = slice_sub(b, 1, b.len);
        }
    }

    Error e = BURROW_NO_ERROR;
    Int n = burrow__tls_conn_write_record_locked(c, TLS_RECORD_TYPE_APPLICATION_DATA, b,
                                                 &e);
    e = burrow__tls_half_conn_set_error_locked(&c->out, e);
    if (BURROW_FAILED(e))
        BURROW_OUT(err, e);
    return n + m;
}

Int tls_conn_write(TlsConn *c, Slice b, Error *err) {
    /* interlock with Close below */
    for (;;) {
        int32_t x = sync_atomic_int32_load(&c->active_call);
        if ((x & 1) != 0) {
            BURROW_OUT(err, net_err_closed);
            return 0;
        }
        if (sync_atomic_int32_compare_and_swap(&c->active_call, x, x + 2))
            break;
    }

    Int n = 0;
    Error e = tls_conn_handshake(c);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
    } else {
        sync_mutex_lock(&c->out.mu);
        n = thc_write_locked_app(c, b, err);
        sync_mutex_unlock(&c->out.mu);
    }

    (void)sync_atomic_int32_add(&c->active_call, -2);
    return n;
}

/* c.handshakeErr = err, kept where the Conn keeps what lasts as long as it
 * does. handshake_mutex has to be held. */
void burrow__tls_conn_set_handshake_err(TlsConn *c, Error err) {
    c->handshake_err =
        BURROW_OK(err) ? err : error_retain(arena_allocator(&c->state), err);
}

/* handleRenegotiation processes a HelloRequest handshake message. */
static Error thc_handle_renegotiation(TlsConn *c) {
    if (c->vers == TLS_VERSION_TLS13)
        return thc_err_unexpected_renegotiation;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    TlsHandshakeMsg msg;
    memset(&msg, 0, sizeof msg);
    IoWriter none = {NULL, NULL};
    Error err = burrow__tls_conn_read_handshake(c, none, arena_allocator(&ar), &msg);
    arena_free(&ar);
    if (BURROW_FAILED(err))
        return err;
    TlsMsgKind kind = msg.kind;

    if (kind != TLS_MSG_HELLO_REQUEST) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_unexpected_message_error(TLS_MSG_HELLO_REQUEST, kind);
    }

    if (!c->is_client)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_NO_RENEGOTIATION);

    switch (c->config->renegotiation) {
    case TLS_RENEGOTIATE_NEVER:
        return burrow__tls_conn_send_alert(c, TLS_ALERT_NO_RENEGOTIATION);
    case TLS_RENEGOTIATE_ONCE_AS_CLIENT:
        if (c->handshakes > 1)
            return burrow__tls_conn_send_alert(c, TLS_ALERT_NO_RENEGOTIATION);
        break;
    case TLS_RENEGOTIATE_FREELY_AS_CLIENT:
        /* Ok. */
        break;
    default:
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);
        return thc_err_unknown_renegotiation;
    }

    sync_mutex_lock(&c->handshake_mutex);

    sync_atomic_bool_store(&c->is_handshake_complete, false);
    burrow__tls_conn_set_handshake_err(c, c->handshake_fn(c, context_background()));
    if (BURROW_OK(c->handshake_err))
        c->handshakes++;
    Error ret = c->handshake_err;

    sync_mutex_unlock(&c->handshake_mutex);
    return ret;
}

Str burrow__tls_conn_client_session_cache_key(TlsConn *c, Alloc *a) {
    if (c->config->server_name.len > 0)
        return c->config->server_name;
    if (c->conn.vt != NULL) {
        NetAddr addr = c->conn.vt->remote_addr(c->conn.data);
        return addr.vt->string(addr.data, a);
    }
    return BURROW_STR_EMPTY;
}

/* handleNewSessionTicket, from handshake_client_tls13.go.
 *
 * Not in Go: with no memory for the session, the ticket is dropped, which a
 * client is free to do, and the connection goes on. */
static Error thc_handle_new_session_ticket(TlsConn *c,
                                           const TlsNewSessionTicketMsgTLS13 *msg) {
    if (!c->is_client) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return thc_err_ticket_from_client;
    }

    if (c->config->session_tickets_disabled ||
        c->config->client_session_cache.vt == NULL)
        return BURROW_NO_ERROR;

    /* See RFC 8446, Section 4.6.1. */
    if (msg->lifetime == 0)
        return BURROW_NO_ERROR;
    Duration lifetime = (Duration)msg->lifetime * TIME_SECOND;
    if (lifetime > TLS_MAX_SESSION_TICKET_LIFETIME) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return thc_err_ticket_lifetime;
    }

    if (msg->label.len == 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_DECODE_ERROR);
        return thc_err_ticket_empty_label;
    }

    /* RFC 9001, Section 4.6.1 */
    if (c->quic != NULL && msg->max_early_data != 0 &&
        msg->max_early_data != 0xffffffff) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_ILLEGAL_PARAMETER);
        return thc_err_quic_early_data;
    }

    const TlsSuiteTLS13 *cipher_suite =
        burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
    if (cipher_suite == NULL || c->resumption_secret.p == NULL)
        return burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR);

    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&tmp);
    Slice psk = burrow__tls13_expand_label(
        ta, burrow__tls_suite13_hash_new(cipher_suite), c->resumption_secret,
        BURROW_S("resumption"), msg->nonce, crypto_hash_size(cipher_suite->hash));
    if (psk.p == NULL) {
        arena_free(&tmp);
        return BURROW_NO_ERROR;
    }

    /* Not in Go: the session outlives c in the cache, so it comes from the
     * config's allocator, which any goroutine can use. */
    Alloc *sa = c->config->a != NULL ? c->config->a : heap_allocator();
    Error err = BURROW_NO_ERROR;
    TlsSessionState *session = burrow__tls_conn_session_state(c, sa, &err);
    if (session == NULL) {
        arena_free(&tmp);
        return BURROW_NO_ERROR;
    }
    session->secret = bytes_clone(tls_session_state_allocator(session), psk);
    if (session->secret.p == NULL) {
        tls_session_state_free(session);
        arena_free(&tmp);
        return BURROW_NO_ERROR;
    }
    session->use_by =
        (uint64_t)time_unix(time_add(burrow__tls_config_time(c->config), lifetime));
    session->age_add = msg->age_add;
    session->early_data =
        c->quic != NULL &&
        msg->max_early_data == 0xffffffff; /* RFC 9001, Section 4.6.1 */
    TlsClientSessionState *cs = tls_new_resumption_state(msg->label, session, &err);
    if (cs == NULL) {
        arena_free(&tmp);
        return BURROW_NO_ERROR;
    }
    if (c->quic != NULL && c->quic->enable_session_events) {
        arena_free(&tmp);
        return burrow__tls_conn_quic_store_session(c, cs);
    }
    Str cache_key = burrow__tls_conn_client_session_cache_key(c, ta);
    if (cache_key.len > 0)
        tls_client_session_cache_put(c->config->client_session_cache, cache_key, cs);
    tls_client_session_state_release(cs);
    arena_free(&tmp);

    return BURROW_NO_ERROR;
}

static Error thc_handle_key_update(TlsConn *c, const TlsKeyUpdateMsg *key_update) {
    if (c->quic != NULL) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_half_conn_set_error_locked(&c->in, thc_err_quic_key_update);
    }

    const TlsSuiteTLS13 *cipher_suite =
        burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
    if (cipher_suite == NULL) {
        return burrow__tls_half_conn_set_error_locked(
            &c->in, burrow__tls_conn_send_alert(c, TLS_ALERT_INTERNAL_ERROR));
    }

    /* Not in Go: the secrets are made in tmp, as the half conns copy them. */
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Alloc *a = arena_allocator(&tmp);

    if (key_update->update_requested) {
        sync_mutex_lock(&c->out.mu);

        TlsKeyUpdateMsg msg = {false};
        Error err = BURROW_NO_ERROR;
        Slice msg_bytes = burrow__tls_key_update_msg_marshal(&msg, a, &err);
        if (BURROW_FAILED(err)) {
            sync_mutex_unlock(&c->out.mu);
            arena_free(&tmp);
            return err;
        }
        (void)burrow__tls_conn_write_record_locked(c, TLS_RECORD_TYPE_HANDSHAKE,
                                                   msg_bytes, &err);
        if (BURROW_FAILED(err)) {
            /* Surface the error at the next write. */
            (void)burrow__tls_half_conn_set_error_locked(&c->out, err);
            sync_mutex_unlock(&c->out.mu);
            arena_free(&tmp);
            return BURROW_NO_ERROR;
        }

        Slice new_secret =
            burrow__tls13_next_traffic_secret(cipher_suite, a, c->out.traffic_secret);
        if (new_secret.p == NULL) {
            /* Not in Go: no room for the secret, which goes to the next
             * write. */
            (void)burrow__tls_half_conn_set_error_locked(&c->out,
                                                         burrow_err_out_of_memory);
        } else {
            (void)burrow__tls_conn_set_write_traffic_secret(
                c, cipher_suite, TLS_QUIC_ENCRYPTION_LEVEL_INITIAL, new_secret);
        }
    }

    Error err = burrow_err_out_of_memory;
    Slice new_secret =
        burrow__tls13_next_traffic_secret(cipher_suite, a, c->in.traffic_secret);
    if (new_secret.p != NULL)
        err = burrow__tls_conn_set_read_traffic_secret(
            c, cipher_suite, TLS_QUIC_ENCRYPTION_LEVEL_INITIAL, new_secret,
            key_update->update_requested);

    if (key_update->update_requested)
        sync_mutex_unlock(&c->out.mu);
    arena_free(&tmp);
    return err;
}

/* handlePostHandshakeMessage processes a handshake message arrived after the
 * handshake is complete. Up to TLS 1.2, it indicates the start of a
 * renegotiation. */
Error burrow__tls_conn_handle_post_handshake_message(TlsConn *c) {
    if (c->vers != TLS_VERSION_TLS13)
        return thc_handle_renegotiation(c);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    TlsHandshakeMsg msg;
    memset(&msg, 0, sizeof msg);
    IoWriter none = {NULL, NULL};
    Error err = burrow__tls_conn_read_handshake(c, none, arena_allocator(&ar), &msg);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        return err;
    }
    c->retry_count++;
    if (c->retry_count > TLS_MAX_USELESS_RECORDS) {
        arena_free(&ar);
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return burrow__tls_half_conn_set_error_locked(&c->in,
                                                      thc_err_too_many_non_advancing);
    }

    switch (msg.kind) {
    case TLS_MSG_NEW_SESSION_TICKET_TLS13:
        err = thc_handle_new_session_ticket(c, &msg.new_session_ticket_tls13);
        arena_free(&ar);
        return err;
    case TLS_MSG_KEY_UPDATE:
        err = thc_handle_key_update(c, &msg.key_update);
        arena_free(&ar);
        return err;
    default:
        break;
    }
    arena_free(&ar);
    /* The QUIC layer is supposed to treat an unexpected post-handshake
     * CertificateRequest as a QUIC-level PROTOCOL_VIOLATION error (RFC 9001,
     * Section 4.4). Returning an unexpected_message alert here doesn't provide
     * it with enough information to distinguish this condition from other
     * unexpected messages. This is probably fine. */
    (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
    return fmt_errorf_v("tls: received unexpected handshake message of type %s",
                        burrow__tls_handshake_msg_type_name(msg.kind));
}

Int tls_conn_read(TlsConn *c, Slice b, Error *err) {
    Error e = tls_conn_handshake(c);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    if (b.len == 0) {
        /* Put this after Handshake, in case people were calling Read(nil) for
         * the side effect of the Handshake. */
        return 0;
    }

    sync_mutex_lock(&c->in.mu);

    while (c->input.len == 0) {
        e = burrow__tls_conn_read_record(c);
        while (BURROW_OK(e) && bytes_buffer_len(&c->hand) > 0)
            e = burrow__tls_conn_handle_post_handshake_message(c);
        if (BURROW_FAILED(e)) {
            sync_mutex_unlock(&c->in.mu);
            BURROW_OUT(err, e);
            return 0;
        }
    }

    Int n = slice_copy(b, c->input);
    c->input = slice_sub(c->input, n, c->input.len);

    /* If a close-notify alert is waiting, read it so that we can return (n,
     * EOF) instead of (n, nil), to signal to the HTTP response reading
     * goroutine that the connection is now closed. This eliminates a race
     * where the HTTP response reading goroutine would otherwise not observe
     * the EOF until its next read, by which time a client goroutine might have
     * already tried to reuse the HTTP connection for a new request. See
     * https://golang.org/cl/76400046 and https://golang.org/issue/3514 */
    if (n != 0 && c->input.len == 0 && bytes_buffer_len(&c->raw_input) > 0 &&
        ((const Byte *)bytes_buffer_bytes(&c->raw_input).p)[0] ==
            TLS_RECORD_TYPE_ALERT) {
        e = burrow__tls_conn_read_record(c);
        if (BURROW_FAILED(e))
            BURROW_OUT(err, e); /* will be io.EOF on closeNotify */
    }

    sync_mutex_unlock(&c->in.mu);
    return n;
}

static Error thc_close_notify(TlsConn *c) {
    sync_mutex_lock(&c->out.mu);

    if (!c->close_notify_sent) {
        /* Set a Write Deadline to prevent possibly blocking forever. */
        (void)tls_conn_set_write_deadline(c, time_add(time_now(), 5 * TIME_SECOND));
        c->close_notify_err = thc_keep(
            &c->out, burrow__tls_conn_send_alert_locked(c, TLS_ALERT_CLOSE_NOTIFY));
        c->close_notify_sent = true;
        /* Any subsequent writes will fail. */
        (void)tls_conn_set_write_deadline(c, time_now());
    }
    Error err = c->close_notify_err;

    sync_mutex_unlock(&c->out.mu);
    return err;
}

Error tls_conn_close(TlsConn *c) {
    /* Interlock with Conn.Write above. */
    int32_t x;
    for (;;) {
        x = sync_atomic_int32_load(&c->active_call);
        if ((x & 1) != 0)
            return net_err_closed;
        if (sync_atomic_int32_compare_and_swap(&c->active_call, x, x | 1))
            break;
    }
    if (x != 0) {
        /* io.Writer and io.Closer should not be used concurrently. If Close is
         * called while a Write is currently in-flight, interpret that as a sign
         * that this Close is really just being used to break the Write and/or
         * clean up resources and avoid sending the alertCloseNotify, which may
         * block waiting on handshakeMutex or the c.out mutex. */
        return thc_conn_close(c->conn);
    }

    Error alert_err = BURROW_NO_ERROR;
    if (sync_atomic_bool_load(&c->is_handshake_complete)) {
        Error err = thc_close_notify(c);
        if (BURROW_FAILED(err))
            alert_err = fmt_errorf_v("tls: failed to send closeNotify alert (but "
                                     "connection was closed anyway): %w",
                                     err);
    }

    Error err = thc_conn_close(c->conn);
    if (BURROW_FAILED(err))
        return err;
    return alert_err;
}

Error tls_conn_close_write(TlsConn *c) {
    if (!sync_atomic_bool_load(&c->is_handshake_complete))
        return burrow__tls_err_early_close_write;

    return thc_close_notify(c);
}

Error tls_conn_handshake(TlsConn *c) {
    return tls_conn_handshake_context(c, context_background());
}

/* The part of handshakeContext under handshakeMutex. */
static Error thc_handshake_locked(TlsConn *c, Context handshake_ctx) {
    sync_mutex_lock(&c->handshake_mutex);

    if (BURROW_FAILED(c->handshake_err) ||
        sync_atomic_bool_load(&c->is_handshake_complete)) {
        Error err = c->handshake_err;
        sync_mutex_unlock(&c->handshake_mutex);
        return err;
    }

    sync_mutex_lock(&c->in.mu);

    Error err = c->handshake_fn(c, handshake_ctx);
    if (BURROW_OK(err)) {
        c->handshakes++;
    } else {
        /* If an error occurred during the handshake try to flush the alert
         * that might be left in the buffer. */
        (void)burrow__tls_conn_flush(c, NULL);
    }

    bool complete = sync_atomic_bool_load(&c->is_handshake_complete);
    if (BURROW_OK(err) && !complete)
        err = thc_err_no_result;
    if (BURROW_FAILED(err) && complete) {
        sync_mutex_unlock(&c->in.mu);
        sync_mutex_unlock(&c->handshake_mutex);
        panic_str(
            BURROW_S("tls: internal error: handshake returned an error but is marked "
                     "successful"));
    }
    burrow__tls_conn_set_handshake_err(c, err);
    err = c->handshake_err;

    if (c->quic != NULL) {
        if (BURROW_OK(c->handshake_err)) {
            /* Not in Go: the event can run out of memory. */
            err = burrow__tls_conn_quic_handshake_complete(c);
            /* Provide the 1-RTT read secret now that the handshake is complete.
             * The QUIC layer MUST NOT decrypt 1-RTT packets prior to completing
             * the handshake (RFC 9001, Section 5.7). */
            if (BURROW_OK(err))
                err = burrow__tls_conn_quic_set_read_secret(
                    c, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, c->cipher_suite,
                    c->in.traffic_secret);
            if (BURROW_FAILED(err)) {
                sync_mutex_unlock(&c->in.mu);
                sync_mutex_unlock(&c->handshake_mutex);
                return err;
            }
        } else {
            sync_mutex_lock(&c->out.mu);
            uint8_t a = TLS_ALERT_INTERNAL_ERROR;
            (void)burrow__tls_as_alert(c->out.err, &a);
            sync_mutex_unlock(&c->out.mu);
            /* Return an error which wraps both the handshake error and any
             * alert error we may have sent, or alertInternalError if we didn't
             * send an alert. Truncate the text of the alert to 0 characters. */
            Error ae = tls_alert_error_as_error(a, error_allocator());
            burrow__tls_conn_set_handshake_err(
                c, fmt_errorf_v("%w%.0w", c->handshake_err, ae));
            err = c->handshake_err;
        }
        chan_close(c->quic->blockedc);
        chan_close(c->quic->signalc);
    }

    sync_mutex_unlock(&c->in.mu);
    sync_mutex_unlock(&c->handshake_mutex);
    return err;
}

/* Not in Go: what the AfterFunc closes, and the group it is done in. Go's
 * collector keeps the connection alive while the close runs; here the
 * handshake waits for it, so that whoever frees the connection after the
 * handshake returns does not free it under the close. */
typedef struct ThcCanceler {
    NetConn conn;
    SyncWaitGroup wg;
} ThcCanceler;

static void thc_close_on_cancel(void *env) {
    ThcCanceler *k = (ThcCanceler *)env;
    (void)thc_conn_close(k->conn);
    sync_wait_group_done(&k->wg);
}

/* Not in Go: the end of the handshake for a QUICConn when there is no memory
 * to start it with. */
static void thc_quic_out_of_memory(TlsConn *c) {
    sync_mutex_lock(&c->handshake_mutex);
    burrow__tls_conn_set_handshake_err(c, burrow_err_out_of_memory);
    chan_close(c->quic->blockedc);
    chan_close(c->quic->signalc);
    sync_mutex_unlock(&c->handshake_mutex);
}

Error tls_conn_handshake_context(TlsConn *c, Context ctx) {
    /* Fast sync/atomic-based exit if there is no handshake in flight and the
     * last one succeeded without an error. Avoids the expensive context setup
     * and mutex for most Read and Write calls. */
    if (sync_atomic_bool_load(&c->is_handshake_complete))
        return BURROW_NO_ERROR;

    ContextCancelFunc cancel;
    Context handshake_ctx = context_with_cancel(heap_allocator(), ctx, &cancel);
    if (BURROW_CONTEXT_IS_NIL(handshake_ctx)) {
        /* Not in Go: a QUICConn waits for the handshake to say it is over. */
        if (c->quic != NULL)
            thc_quic_out_of_memory(c);
        return burrow_err_out_of_memory;
    }

    Context reg;
    memset(&reg, 0, sizeof reg);
    StopFunc stop;
    memset(&stop, 0, sizeof stop);
    ThcCanceler *k = NULL;
    if (c->quic != NULL) {
        /* Not in Go: the QUICConn releases handshake_ctx, which it can cancel
         * after this returns. */
        c->quic->ctx = handshake_ctx;
        c->quic->cancel = cancel;
    } else if (context_done(ctx) != NULL) {
        /* Close the connection if ctx is canceled before the function
         * returns. */
        k = BURROW_NEW(heap_allocator(), ThcCanceler);
        if (k != NULL) {
            k->conn = c->conn;
            sync_wait_group_add(&k->wg, 1);
            reg = context_after_func(heap_allocator(), ctx,
                                     BURROW_FN(Func, thc_close_on_cancel, k), &stop);
        }
        if (BURROW_CONTEXT_IS_NIL(reg)) {
            if (k != NULL)
                mem_free(heap_allocator(), k, sizeof *k, _Alignof(ThcCanceler));
            BURROW_CALLF0(cancel);
            context_release(handshake_ctx);
            return burrow_err_out_of_memory;
        }
    }

    Error ret = thc_handshake_locked(c, handshake_ctx);

    if (k != NULL) {
        if (!BURROW_CALLF0(stop)) {
            ret = context_err(ctx); /* Return context error to user. */
            sync_wait_group_wait(&k->wg);
        }
        mem_free(heap_allocator(), k, sizeof *k, _Alignof(ThcCanceler));
        context_release(reg);
    }
    BURROW_CALLF0(cancel);
    if (c->quic == NULL)
        context_release(handshake_ctx);
    return ret;
}

static Slice thc_no_ekm_renegotiation(void *env, Alloc *a, Str label, Slice context,
                                      Int length, Error *err) {
    (void)env;
    (void)a;
    (void)label;
    (void)context;
    (void)length;
    BURROW_OUT(err, burrow__tls_err_no_ekm_renegotiation);
    return slice_nil(TYPE_BYTE);
}

static Slice thc_no_ekm_no_ems(void *env, Alloc *a, Str label, Slice context,
                               Int length, Error *err) {
    (void)env;
    (void)a;
    (void)label;
    (void)context;
    (void)length;
    BURROW_OUT(err, burrow__tls_err_no_ekm_no_ems);
    return slice_nil(TYPE_BYTE);
}

TlsConnectionState burrow__tls_conn_connection_state_locked(TlsConn *c) {
    TlsConnectionState state;
    memset(&state, 0, sizeof state);
    state.handshake_complete = sync_atomic_bool_load(&c->is_handshake_complete);
    state.version = c->vers;
    state.negotiated_protocol = c->client_protocol;
    state.did_resume = c->did_resume;
    state.hello_retry_request = c->did_hrr;
    state.testing_only_peer_signature_algorithm = c->peer_sig_alg;
    state.curve_id = c->curve_id;
    state.negotiated_protocol_is_mutual = true;
    state.server_name = c->server_name;
    state.cipher_suite = c->cipher_suite;
    state.peer_certificates = c->peer_certificates;
    state.local_certificate = c->local_certificate;
    state.verified_chains = c->verified_chains;
    state.signed_certificate_timestamps = c->scts;
    state.ocsp_response = c->ocsp_response;
    state.tls_unique = slice_nil(TYPE_BYTE);
    if ((!c->did_resume || c->ext_master_secret) && c->vers != TLS_VERSION_TLS13) {
        if (c->client_finished_is_first)
            state.tls_unique = slice_from(c->client_finished, 12, 12, TYPE_BYTE);
        else
            state.tls_unique = slice_from(c->server_finished, 12, 12, TYPE_BYTE);
    }
    if (c->config->renegotiation != TLS_RENEGOTIATE_NEVER)
        state.ekm = BURROW_FN(TlsEkmFunc, thc_no_ekm_renegotiation, NULL);
    else if (c->vers != TLS_VERSION_TLS13 && !c->ext_master_secret)
        state.ekm = BURROW_FN(TlsEkmFunc, thc_no_ekm_no_ems, NULL);
    else
        state.ekm = c->ekm;
    state.ech_accepted = c->ech_accepted;
    return state;
}

TlsConnectionState tls_conn_connection_state(TlsConn *c) {
    sync_mutex_lock(&c->handshake_mutex);
    TlsConnectionState state = burrow__tls_conn_connection_state_locked(c);
    sync_mutex_unlock(&c->handshake_mutex);
    return state;
}

Slice tls_conn_ocsp_response(TlsConn *c) {
    sync_mutex_lock(&c->handshake_mutex);
    Slice r = c->ocsp_response;
    sync_mutex_unlock(&c->handshake_mutex);
    return r;
}

Error tls_conn_verify_hostname(TlsConn *c, Str host) {
    sync_mutex_lock(&c->handshake_mutex);
    Error err;
    if (!c->is_client)
        err = thc_err_verify_on_server;
    else if (!sync_atomic_bool_load(&c->is_handshake_complete))
        err = thc_err_no_handshake;
    else if (c->verified_chains.len == 0)
        err = thc_err_no_verified_chain;
    else
        err = x509_certificate_verify_hostname(
            ((X509Certificate *const *)c->peer_certificates.p)[0], host);
    sync_mutex_unlock(&c->handshake_mutex);
    return err;
}

Error burrow__tls_conn_set_read_traffic_secret(TlsConn *c, const TlsSuiteTLS13 *suite,
                                               TlsQUICEncryptionLevel level,
                                               Slice secret, bool locked) {
    /* Ensure that there are no buffered handshake messages before changing the
     * read keys, since that can cause messages to be parsed that were
     * encrypted using old keys which are no longer appropriate. */
    if (bytes_buffer_len(&c->hand) != 0) {
        if (locked)
            (void)burrow__tls_conn_send_alert_locked(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        else
            (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return thc_err_hand_not_empty;
    }
    return burrow__tls_half_conn_set_traffic_secret(&c->in, suite, level, secret);
}

Error burrow__tls_conn_set_write_traffic_secret(TlsConn *c, const TlsSuiteTLS13 *suite,
                                                TlsQUICEncryptionLevel level,
                                                Slice secret) {
    return burrow__tls_half_conn_set_traffic_secret(&c->out, suite, level, secret);
}

/* ---------------------------------------------------------------- net.Conn */

static const Type thc_conn_desc = {
    BURROW_S_INIT("Conn"),
    BURROW_S_INIT("crypto/tls"),
    KIND_STRUCT,
    (uint32_t)sizeof(TlsConn),
    (uint16_t)_Alignof(TlsConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c636eU, /* "tlcn" */
    NULL,
};

static Int thc_nc_read(void *self, Slice p, Error *err) {
    return tls_conn_read((TlsConn *)self, p, err);
}

static Int thc_nc_write(void *self, Slice p, Error *err) {
    return tls_conn_write((TlsConn *)self, p, err);
}

static Error thc_nc_close(void *self) {
    return tls_conn_close((TlsConn *)self);
}

static NetAddr thc_nc_local_addr(void *self) {
    return tls_conn_local_addr((TlsConn *)self);
}

static NetAddr thc_nc_remote_addr(void *self) {
    return tls_conn_remote_addr((TlsConn *)self);
}

static Error thc_nc_set_deadline(void *self, Time t) {
    return tls_conn_set_deadline((TlsConn *)self, t);
}

static Error thc_nc_set_read_deadline(void *self, Time t) {
    return tls_conn_set_read_deadline((TlsConn *)self, t);
}

static Error thc_nc_set_write_deadline(void *self, Time t) {
    return tls_conn_set_write_deadline((TlsConn *)self, t);
}

static const NetConnVT thc_net_conn_vt = {
    {&thc_conn_desc, thc_nc_read},
    {&thc_conn_desc, thc_nc_write},
    {&thc_conn_desc, thc_nc_close},
    thc_nc_local_addr,
    thc_nc_remote_addr,
    thc_nc_set_deadline,
    thc_nc_set_read_deadline,
    thc_nc_set_write_deadline,
};

NetConn tls_conn_as_net_conn(TlsConn *c) {
    return (NetConn){&thc_net_conn_vt, c};
}

TlsConn *tls_conn_from_net_conn(NetConn c) {
    return c.vt == &thc_net_conn_vt ? (TlsConn *)c.data : NULL;
}
