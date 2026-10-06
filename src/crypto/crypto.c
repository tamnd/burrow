/* crypto: Hash and its registry, and SignMessage.
 *
 * Derived from Go's src/crypto/crypto.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* One past the last hash, Go's maxHash. */
#define CRYPTO_MAX_HASH 21

/* ------------------------------------------------------------------ names */

static const char *const crypto_hash_names[CRYPTO_MAX_HASH] = {
    [1] = "MD4",          [2] = "MD5",
    [3] = "SHA-1",        [4] = "SHA-224",
    [5] = "SHA-256",      [6] = "SHA-384",
    [7] = "SHA-512",      [8] = "MD5+SHA1",
    [9] = "RIPEMD-160",   [10] = "SHA3-224",
    [11] = "SHA3-256",    [12] = "SHA3-384",
    [13] = "SHA3-512",    [14] = "SHA-512/224",
    [15] = "SHA-512/256", [16] = "BLAKE2s-256",
    [17] = "BLAKE2b-256", [18] = "BLAKE2b-384",
    [19] = "BLAKE2b-512", [20] = "ML-DSA \xce\xbc message representative",
};

static const uint8_t crypto_digest_sizes[CRYPTO_MAX_HASH] = {
    [1] = 16,  [2] = 16,  [3] = 20,  [4] = 28,  [5] = 32,  [6] = 48,  [7] = 64,
    [8] = 36,  [9] = 20,  [10] = 28, [11] = 32, [12] = 48, [13] = 64, [14] = 28,
    [15] = 32, [16] = 32, [17] = 32, [18] = 48, [19] = 64, [20] = 64,
};

CryptoHash crypto_hash_hash_func(CryptoHash h) {
    return h;
}

static const char crypto_unknown_prefix[] = "unknown hash value ";

#define CRYPTO_UNKNOWN_LEN ((Int)sizeof(crypto_unknown_prefix) - 1)

Str crypto_hash_string(CryptoHash h, Alloc *a) {
    if (h > 0 && h < CRYPTO_MAX_HASH)
        return str_from_cstr(crypto_hash_names[h]);
    /* Go converts to int for strconv.Itoa, so a number past the top of int
     * comes out negative, and the same conversion here does the same. */
    Int v = (Int)h;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    Byte digits[20];
    Int n = 0;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = CRYPTO_UNKNOWN_LEN + (v < 0) + n;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = p;
    memcpy(q, crypto_unknown_prefix, (size_t)CRYPTO_UNKNOWN_LEN);
    q += CRYPTO_UNKNOWN_LEN;
    if (v < 0)
        *q++ = '-';
    while (n > 0)
        *q++ = digits[--n];
    return str_from_bytes(p, len);
}

Int crypto_hash_size(CryptoHash h) {
    if (h > 0 && h < CRYPTO_MAX_HASH)
        return (Int)crypto_digest_sizes[h];
    panic_str(BURROW_S("crypto: Size of unknown hash function"));
}

/* --------------------------------------------------------------- registry */

/* What RegisterHash has been given, as the function's address so that a
 * plain atomic word can hold it. Zero means nothing was registered and the
 * built in one, if there is one, is used. */
static uint64_t crypto_hashes[CRYPTO_MAX_HASH];

/* What a registered NULL is stored as, so that it hides a built in hash the
 * way Go's nil in the table does, rather than looking like nothing was
 * registered. Never called. */
static Hash crypto_registered_nil(Alloc *a) {
    (void)a;
    Hash nil = {0};
    return nil;
}

static Hash crypto_new_sha3(Sha3 *d) {
    if (d == NULL) {
        Hash nil = {0};
        return nil;
    }
    return sha3_as_hash(d);
}

static Hash crypto_new_sha3_224(Alloc *a) {
    return crypto_new_sha3(sha3_new224(a));
}

static Hash crypto_new_sha3_256(Alloc *a) {
    return crypto_new_sha3(sha3_new256(a));
}

static Hash crypto_new_sha3_384(Alloc *a) {
    return crypto_new_sha3(sha3_new384(a));
}

static Hash crypto_new_sha3_512(Alloc *a) {
    return crypto_new_sha3(sha3_new512(a));
}

/* The hashes Go's own packages register from init, all of which are always
 * linked in here. */
static HashNewFunc crypto_builtin(CryptoHash h) {
    switch (h) {
    case CRYPTO_MD5:
        return md5_new;
    case CRYPTO_SHA1:
        return sha1_new;
    case CRYPTO_SHA224:
        return sha256_new224;
    case CRYPTO_SHA256:
        return sha256_new;
    case CRYPTO_SHA384:
        return sha512_new384;
    case CRYPTO_SHA512:
        return sha512_new;
    case CRYPTO_SHA512_224:
        return sha512_new512224;
    case CRYPTO_SHA512_256:
        return sha512_new512256;
    case CRYPTO_SHA3_224:
        return crypto_new_sha3_224;
    case CRYPTO_SHA3_256:
        return crypto_new_sha3_256;
    case CRYPTO_SHA3_384:
        return crypto_new_sha3_384;
    case CRYPTO_SHA3_512:
        return crypto_new_sha3_512;
    default:
        return NULL;
    }
}

static HashNewFunc crypto_lookup(CryptoHash h) {
    if (h == 0 || h >= CRYPTO_MAX_HASH)
        return NULL;
    uint64_t f = burrow__atomic_load_acquire_u64(&crypto_hashes[h]);
    if (f == 0)
        return crypto_builtin(h);
    HashNewFunc fn = (HashNewFunc)(uintptr_t)f;
    return fn == crypto_registered_nil ? NULL : fn;
}

/* "crypto: requested hash function unavailable: " and h's name, built in a,
 * which is both New's panic and SignMessage's error. Empty when a refuses. */
static Str crypto_unavailable_text(CryptoHash h, Alloc *a) {
    static const char prefix[] = "crypto: requested hash function unavailable: ";
    Int plen = (Int)sizeof(prefix) - 1;
    Str name = crypto_hash_string(h, a);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + name.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix, (size_t)plen);
    if (name.len > 0)
        memcpy(p + plen, name.p, (size_t)name.len);
    return str_from_bytes(p, plen + name.len);
}

Hash crypto_hash_new(CryptoHash h, Alloc *a) {
    HashNewFunc f = crypto_lookup(h);
    if (f != NULL)
        return f(a);
    Str text = crypto_unavailable_text(h, error_allocator());
    if (text.len == 0)
        text = BURROW_S("crypto: requested hash function unavailable");
    panic_str(text);
}

bool crypto_hash_available(CryptoHash h) {
    return crypto_lookup(h) != NULL;
}

void crypto_register_hash(CryptoHash h, HashNewFunc f) {
    if (h == 0 || h >= CRYPTO_MAX_HASH)
        panic_str(BURROW_S("crypto: RegisterHash of unknown hash function"));
    if (h == CRYPTO_MLDSA_MU)
        panic_str(BURROW_S("crypto: cannot RegisterHash for MLDSAMu"));
    if (f == NULL)
        f = crypto_registered_nil;
    burrow__atomic_store_release_u64(&crypto_hashes[h], (uint64_t)(uintptr_t)f);
}

/* -------------------------------------------------------------- SignerOpts */

const Type burrow_type_CryptoHash = {
    {(const Byte *)"Hash", 4},
    {(const Byte *)"crypto", 6},
    KIND_UINT,
    (uint32_t)sizeof(CryptoHash),
    (uint16_t)_Alignof(CryptoHash),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63726873U, /* "crhs" */
    NULL,
};

static CryptoHash crypto_hash_opts_hash_func(void *self) {
    return *(const CryptoHash *)self;
}

static const CryptoSignerOptsVT crypto_hash_opts_vt = {&burrow_type_CryptoHash,
                                                       crypto_hash_opts_hash_func};

CryptoSignerOpts crypto_hash_as_signer_opts(const CryptoHash *h) {
    CryptoSignerOpts o = {&crypto_hash_opts_vt, (void *)(uintptr_t)h};
    return o;
}

/* ------------------------------------------------------------ SignMessage */

/* Only here to be named in a signature, so as far as reflection can tell they
 * are unsafe pointers, with names that say what they point at. */
const Type burrow_type_CryptoAllocArg = {
    {(const Byte *)"*Alloc", 6},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(CryptoAllocArg),
    (uint16_t)_Alignof(CryptoAllocArg),
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

const Type burrow_type_CryptoErrorArg = {
    {(const Byte *)"*error", 6},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(CryptoErrorArg),
    (uint16_t)_Alignof(CryptoErrorArg),
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

const Type burrow_type_CryptoSignerOpts = {
    {(const Byte *)"SignerOpts", 10},
    {(const Byte *)"crypto", 6},
    KIND_INTERFACE,
    (uint32_t)sizeof(CryptoSignerOpts),
    (uint16_t)_Alignof(CryptoSignerOpts),
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

static const Str crypto_name_sign_message = {(const Byte *)"SignMessage", 11};

/* The SignMessage method on t, if it has one with CRYPTO_SIG_SIGN_MESSAGE's
 * shape. One with the right name and another shape is not MessageSigner's,
 * as in Go, where it would not satisfy the interface. */
static const Method *crypto_sign_message_method(const Type *t) {
    if (t == NULL)
        return NULL;
    const Method *m = type_method_by_name(t, crypto_name_sign_message);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 5 || type_num_out(f) != 1)
        return NULL;
    if (type_in(f, 0) != &burrow_type_CryptoAllocArg ||
        type_in(f, 1) != TYPE_OF(IoReader) || type_in(f, 2) != TYPE_OF(Bytes) ||
        type_in(f, 3) != &burrow_type_CryptoSignerOpts ||
        type_in(f, 4) != &burrow_type_CryptoErrorArg ||
        type_out(f, 0) != TYPE_OF(Bytes))
        return NULL;
    return m;
}

static Error crypto_hash_unavailable(Alloc *a, CryptoHash h) {
    Str text = crypto_unavailable_text(h, a);
    if (text.len == 0)
        return burrow_err_out_of_memory;
    return errors_new(a, text);
}

Slice crypto_sign_message(Alloc *a, CryptoSigner signer, IoReader rand, Slice msg,
                          CryptoSignerOpts opts, Error *err) {
    const Method *m = crypto_sign_message_method(signer.vt->self_type);
    if (m != NULL) {
        Error e = BURROW_NO_ERROR;
        CryptoAllocArg aa = a;
        CryptoErrorArg ea = &e;
        Slice out = slice_nil(TYPE_BYTE);
        void *args[5] = {(void *)&aa, &rand, &msg, &opts, (void *)&ea};
        void *rets[1] = {&out};
        method_call(m, signer.data, args, rets);
        BURROW_OUT(err, e);
        return out;
    }
    CryptoHash h = opts.vt->hash_func(opts.data);
    if (h != 0) {
        if (!crypto_hash_available(h)) {
            BURROW_OUT(err, crypto_hash_unavailable(a, h));
            return slice_nil(TYPE_BYTE);
        }
        Hash d = crypto_hash_new(h, a);
        if (d.vt == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_BYTE);
        }
        hash_write(d, msg, NULL);
        msg = hash_sum(a, d, slice_nil(TYPE_BYTE));
    }
    return signer.vt->sign(signer.data, a, rand, msg, opts, err);
}
