/* crypto/tls: the record layer's halfConn from conn.go, which encrypts and
 * decrypts one direction of a connection.
 *
 * Go keeps a cipher in an any and switches on its type. Here a TlsCipher has
 * one vtable set out of three, and the same switch looks at which. The
 * ciphers live in arenas the halfConn owns, one for the state in use and one
 * for the state a change of cipher spec moves to, and each arena goes when
 * its state is replaced.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

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
    hc->keys = NULL;
    hc->next_keys = NULL;
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
                                               TlsQuicEncryptionLevel level,
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
