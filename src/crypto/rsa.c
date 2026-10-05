/* Derived from Go's src/crypto/rsa/rsa.go, fips.go and pkcs1v15.go, and
 * src/crypto/internal/fips140/rsa/rsa.go, pkcs1v15.go, pkcs1v22.go and
 * keygen.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/rsa.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "bigmod.h"
#include "rand_internal.h"
#include "rsa_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ types */

const Type burrow_type_RsaPublicKey = {
    BURROW_S_INIT("PublicKey"),
    BURROW_S_INIT("crypto/rsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(RsaPublicKey),
    (uint16_t)_Alignof(RsaPublicKey),
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

const Type burrow_type_RsaPrivateKey = {
    BURROW_S_INIT("PrivateKey"),
    BURROW_S_INIT("crypto/rsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(RsaPrivateKey),
    (uint16_t)_Alignof(RsaPrivateKey),
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

const Type burrow_type_RsaPSSOptions = {
    BURROW_S_INIT("PSSOptions"),
    BURROW_S_INIT("crypto/rsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(RsaPSSOptions),
    (uint16_t)_Alignof(RsaPSSOptions),
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

const Type burrow_type_RsaOAEPOptions = {
    BURROW_S_INIT("OAEPOptions"),
    BURROW_S_INIT("crypto/rsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(RsaOAEPOptions),
    (uint16_t)_Alignof(RsaOAEPOptions),
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

const Type burrow_type_RsaPKCS1v15DecryptOptions = {
    BURROW_S_INIT("PKCS1v15DecryptOptions"),
    BURROW_S_INIT("crypto/rsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(RsaPKCS1v15DecryptOptions),
    (uint16_t)_Alignof(RsaPKCS1v15DecryptOptions),
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

BURROW_SENTINEL_ERROR(rsa_err_message_too_long,
                      "crypto/rsa: message too long for RSA key size");
BURROW_SENTINEL_ERROR(rsa_err_decryption, "crypto/rsa: decryption error");
BURROW_SENTINEL_ERROR(rsa_err_verification, "crypto/rsa: verification error");
BURROW_SENTINEL_ERROR(burrow__rsa_err_divisor_too_large, "divisor too large");

/* ------------------------------------------------------------------ GODEBUG */

enum {
    RSA_DEBUG_KNOWN = 1 << 0,
    RSA_DEBUG_1024MIN_OFF = 1 << 1, /* rsa1024min=0 */
};

static uint32_t rsa_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool rsa_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

static uint32_t rsa_debug_parse(const char *v) {
    uint32_t f = RSA_DEBUG_KNOWN;
    Str s;
    if (v != NULL && rsa_godebug(v, "rsa1024min", &s) && s.len == 1 && s.p[0] == '0')
        f |= RSA_DEBUG_1024MIN_OFF;
    burrow__atomic_store_relaxed_u32(&rsa_debug_flags, f);
    return f;
}

static uint32_t rsa_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&rsa_debug_flags);
    if ((f & RSA_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return rsa_debug_parse(v);
}

void burrow__rsa_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&rsa_debug_flags, 0);
    else
        (void)rsa_debug_parse(value);
}

/* ---------------------------------------------------------------- helpers */

static Alloc *rsa_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

BURROW_NORETURN static void rsa_oom(void) {
    panic_str(BURROW_S("crypto/rsa: out of memory"));
}

/* What Go does with a nil *big.Int, a nil options pointer or a nil reader. */
BURROW_NORETURN static void rsa_nil(void) {
    panic_str(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static Error rsa_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static void rsa_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, rsa_error(msg));
}

/* prefix and then the name of h, as an error. */
static void rsa_unavailable(Error *err, const char *prefix, CryptoHash h) {
    if (err == NULL)
        return;
    Alloc *ea = error_allocator();
    Str name = crypto_hash_string(h, ea);
    Int plen = (Int)strlen(prefix);
    Byte *p = mem_alloc_nozero(ea, (size_t)(plen + name.len), 1);
    if (p == NULL) {
        *err = burrow_err_out_of_memory;
        return;
    }
    memcpy(p, prefix, (size_t)plen);
    if (name.len > 0)
        memcpy(p + plen, name.p, (size_t)name.len);
    *err = errors_new(ea, str_from_bytes(p, plen + name.len));
}

static uint8_t *rsa_p(Slice s) {
    return (uint8_t *)s.p;
}

static Slice rsa_sub(Slice s, Int lo, Int hi) {
    return slice_from(rsa_p(s) + lo, hi - lo, hi - lo, TYPE_BYTE);
}

/* make([]byte, n) from a, which panics when a has no memory. */
static Slice rsa_make(Alloc *a, Int n) {
    Slice s = slice_make(rsa_alloc(a), TYPE_BYTE, n, n);
    if (s.p == NULL && n > 0)
        rsa_oom();
    return s;
}

/* A copy of s from a, for what a function gives back. */
static Slice rsa_clone(Alloc *a, Slice s, Error *err) {
    Slice c = slice_make(rsa_alloc(a), TYPE_BYTE, s.len, s.len);
    if (c.p == NULL && s.len > 0) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    if (s.len > 0)
        memcpy(c.p, s.p, (size_t)s.len);
    return c;
}

static void rsa_slice_free(Alloc *a, Slice s) {
    if (s.cap > 0) {
        memset(s.p, 0, (size_t)s.cap);
        mem_free(rsa_alloc(a), s.p, (size_t)s.cap, 1);
    }
}

/* drbg.ReadWithReader: the system generator for the default reader, which is
 * what drbg.Read is outside FIPS mode, and io.ReadFull for anything else. */
static bool rsa_read(IoReader r, Slice b, Error *err) {
    if (burrow__crypto_rand_is_default_reader(r)) {
        burrow__crypto_rand_system(b);
        return true;
    }
    if (b.len == 0)
        return true;
    if (r.vt == NULL)
        rsa_nil();
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, b, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

/* What the functions that take a random do with a nil one, before
 * rand.CustomReader. */
static IoReader rsa_reader(IoReader r) {
    if (r.vt == NULL)
        r = burrow__crypto_rand_nil_reader();
    return burrow__crypto_rand_custom_reader(r);
}

/* Scratch memory for one operation: an arena whose chunks are cleared before
 * they go back to the heap, since most of what goes in it is secret. */
static void *rsa_wipe_alloc(void *self, size_t size, size_t align) {
    (void)self;
    return mem_alloc_nozero(heap_allocator(), size, align);
}

static void rsa_wipe_free(void *self, void *p, size_t size, size_t align) {
    (void)self;
    if (p == NULL)
        return;
    memset(p, 0, size);
    mem_free(heap_allocator(), p, size, align);
}

static void *rsa_wipe_realloc(void *self, void *p, size_t old, size_t nsz,
                              size_t align) {
    void *q = rsa_wipe_alloc(self, nsz, align);
    if (q == NULL)
        return NULL;
    if (p != NULL) {
        memcpy(q, p, old < nsz ? old : nsz);
        rsa_wipe_free(self, p, old, align);
    }
    return q;
}

static const AllocVT rsa_wipe_vt = {
    rsa_wipe_alloc, NULL, rsa_wipe_realloc, rsa_wipe_free, NULL, NULL,
};

typedef struct RsaScratch {
    Alloc wipe;
    Arena ar;
    Alloc *a;
} RsaScratch;

static void rsa_scratch_init(RsaScratch *s) {
    s->wipe = (Alloc){&rsa_wipe_vt, NULL, NULL, NULL};
    arena_init(&s->ar, &s->wipe, 0);
    s->a = arena_allocator(&s->ar);
}

static void rsa_scratch_free(RsaScratch *s) {
    arena_free(&s->ar);
}

/* ------------------------------------------------------------ big.Int use */

static BigInt *rsa_big_new(Alloc *a) {
    return big_new_int(a, 0);
}

/* new(big.Int).SetBytes(b), from a. */
static BigInt *rsa_big_from_bytes(Alloc *a, Slice b) {
    return big_int_set_bytes(rsa_big_new(a), b);
}

/* An integer from rsa_big_new back to a, cleared first. */
static void rsa_big_free(BigInt *x, Alloc *a) {
    if (x == NULL)
        return;
    if (x->abs.p != NULL && x->abs.cap > 0)
        memset(x->abs.p, 0, (size_t)x->abs.cap * sizeof *x->abs.p);
    big_int_free(x);
    mem_free(rsa_alloc(a), x, sizeof(BigInt), _Alignof(BigInt));
}

/* x.Bytes(), from a. A nil x panics, as it does in Go. */
static Slice rsa_big_bytes(const BigInt *x, Alloc *a) {
    if (x == NULL)
        rsa_nil();
    return big_int_bytes(x, a);
}

/* bigIntEqual: a.Bytes() and b.Bytes() compared in constant time. */
static bool rsa_big_equal(const BigInt *x, const BigInt *y) {
    RsaScratch s;
    rsa_scratch_init(&s);
    Slice xb = rsa_big_bytes(x, s.a);
    Slice yb = rsa_big_bytes(y, s.a);
    bool eq = subtle_constant_time_compare(xb, yb) == 1;
    rsa_scratch_free(&s);
    return eq;
}

/* bigIntEqualToBytes. */
static bool rsa_big_equal_to_bytes(const BigInt *x, Slice b, Alloc *tmp) {
    if (x == NULL || big_int_bit_len(x) > b.len * 8)
        return false;
    Slice buf = big_int_fill_bytes(x, rsa_make(tmp, b.len));
    return subtle_constant_time_compare(buf, b) == 1;
}

/* ------------------------------------------------------- hash prefixes */

typedef struct RsaHashPrefix {
    const char *name;
    const char *prefix;
    Int len;
} RsaHashPrefix;

static const RsaHashPrefix rsa_hash_prefixes[] = {
    {"MD5", "\x30\x20\x30\x0c\x06\x08\x2a\x86\x48\x86\xf7\x0d\x02\x05\x05\x00\x04\x10",
     18},
    {"SHA-1", "\x30\x21\x30\x09\x06\x05\x2b\x0e\x03\x02\x1a\x05\x00\x04\x14", 15},
    {"SHA-224",
     "\x30\x2d\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x04\x05\x00\x04\x1c",
     19},
    {"SHA-256",
     "\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20",
     19},
    {"SHA-384",
     "\x30\x41\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x02\x05\x00\x04\x30",
     19},
    {"SHA-512",
     "\x30\x51\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x03\x05\x00\x04\x40",
     19},
    {"SHA-512/224",
     "\x30\x2d\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x05\x05\x00\x04\x1c",
     19},
    {"SHA-512/256",
     "\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x06\x05\x00\x04\x20",
     19},
    {"SHA3-224",
     "\x30\x2d\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x07\x05\x00\x04\x1c",
     19},
    {"SHA3-256",
     "\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x08\x05\x00\x04\x20",
     19},
    {"SHA3-384",
     "\x30\x41\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x09\x05\x00\x04\x30",
     19},
    {"SHA3-512",
     "\x30\x51\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x0a\x05\x00\x04\x40",
     19},
    {"MD5+SHA1", "", 0},
    {"RIPEMD-160", "\x30\x20\x30\x08\x06\x06\x28\xcf\x06\x03\x00\x31\x04\x14", 14},
};
static const uint16_t rsa_primes[255] = {
    3,    5,    7,    11,   13,   17,   19,   23,   29,   31,   37,   41,   43,   47,
    53,   59,   61,   67,   71,   73,   79,   83,   89,   97,   101,  103,  107,  109,
    113,  127,  131,  137,  139,  149,  151,  157,  163,  167,  173,  179,  181,  191,
    193,  197,  199,  211,  223,  227,  229,  233,  239,  241,  251,  257,  263,  269,
    271,  277,  281,  283,  293,  307,  311,  313,  317,  331,  337,  347,  349,  353,
    359,  367,  373,  379,  383,  389,  397,  401,  409,  419,  421,  431,  433,  439,
    443,  449,  457,  461,  463,  467,  479,  487,  491,  499,  503,  509,  521,  523,
    541,  547,  557,  563,  569,  571,  577,  587,  593,  599,  601,  607,  613,  617,
    619,  631,  641,  643,  647,  653,  659,  661,  673,  677,  683,  691,  701,  709,
    719,  727,  733,  739,  743,  751,  757,  761,  769,  773,  787,  797,  809,  811,
    821,  823,  827,  829,  839,  853,  857,  859,  863,  877,  881,  883,  887,  907,
    911,  919,  929,  937,  941,  947,  953,  967,  971,  977,  983,  991,  997,  1009,
    1013, 1019, 1021, 1031, 1033, 1039, 1049, 1051, 1061, 1063, 1069, 1087, 1091, 1093,
    1097, 1103, 1109, 1117, 1123, 1129, 1151, 1153, 1163, 1171, 1181, 1187, 1193, 1201,
    1213, 1217, 1223, 1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283, 1289, 1291, 1297,
    1301, 1303, 1307, 1319, 1321, 1327, 1361, 1367, 1373, 1381, 1399, 1409, 1423, 1427,
    1429, 1433, 1439, 1447, 1451, 1453, 1459, 1471, 1481, 1483, 1487, 1489, 1493, 1499,
    1511, 1523, 1531, 1543, 1549, 1553, 1559, 1567, 1571, 1579, 1583, 1597, 1601, 1607,
    1609, 1613, 1619,
};

static const RsaHashPrefix *rsa_find_prefix(Str name) {
    for (size_t i = 0; i < sizeof rsa_hash_prefixes / sizeof rsa_hash_prefixes[0];
         i++) {
        const RsaHashPrefix *hp = &rsa_hash_prefixes[i];
        if (str_eq(name, str_from_cstr(hp->name)))
            return hp;
    }
    return NULL;
}

bool burrow__rsa_hash_prefix(Str name, Slice *prefix) {
    const RsaHashPrefix *hp = rsa_find_prefix(name);
    if (hp == NULL)
        return false;
    *prefix = slice_from((void *)(uintptr_t)hp->prefix, hp->len, hp->len, TYPE_BYTE);
    return true;
}

/* hashSize. */
static Int rsa_hash_size(Str name) {
    static const struct {
        const char *name;
        Int size;
    } sizes[] = {
        {"MD5", 16},         {"SHA-1", 20},    {"RIPEMD-160", 20}, {"SHA-224", 28},
        {"SHA-512/224", 28}, {"SHA3-224", 28}, {"SHA-256", 32},    {"SHA-512/256", 32},
        {"SHA3-256", 32},    {"SHA-384", 48},  {"SHA3-384", 48},   {"SHA-512", 64},
        {"SHA3-512", 64},    {"MD5+SHA1", 36},
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (str_eq(name, str_from_cstr(sizes[i].name)))
            return sizes[i].size;
    return -1;
}

/* ------------------------------------------------------------- FIPS keys */

/* crypto/internal/fips140/rsa.PrivateKey. The public half is n and e. Without
 * the CRT values, crt is false and p, q, dp, dq and qinv are not set. */
struct RsaFipsKey {
    BigmodModulus *n;
    Int e;
    BigmodNat d;
    BigmodModulus *p, *q;
    Slice dp, dq;
    BigmodNat qinv;
    bool crt;
    Alloc *a;
};

/* What PrivateKey.Export gives, from the Alloc it is asked to use. */
typedef struct RsaFipsExport {
    Slice n;
    Int e;
    Slice d, p, q, dp, dq, qinv;
    bool crt;
} RsaFipsExport;

static RsaFipsKey *rsa_fips_key_new(Alloc *a) {
    a = rsa_alloc(a);
    RsaFipsKey *k = mem_alloc(a, sizeof *k, _Alignof(RsaFipsKey));
    if (k == NULL)
        rsa_oom();
    k->a = a;
    bigmod_nat_init(&k->d, a);
    bigmod_nat_init(&k->qinv, a);
    k->dp = slice_nil(TYPE_BYTE);
    k->dq = slice_nil(TYPE_BYTE);
    return k;
}

static void rsa_nat_wipe(BigmodNat *x) {
    if (x->limbs != NULL && x->cap > 0 && !x->borrowed)
        memset(x->limbs, 0, (size_t)x->cap * sizeof(Uint));
}

static void rsa_modulus_free(BigmodModulus *m) {
    if (m == NULL)
        return;
    rsa_nat_wipe(&m->nat);
    rsa_nat_wipe(&m->rr);
    bigmod_modulus_free(m);
}

static void rsa_fips_key_free(RsaFipsKey *k) {
    if (k == NULL)
        return;
    Alloc *a = k->a;
    rsa_modulus_free(k->n);
    rsa_modulus_free(k->p);
    rsa_modulus_free(k->q);
    rsa_nat_wipe(&k->d);
    bigmod_nat_free(&k->d);
    rsa_nat_wipe(&k->qinv);
    bigmod_nat_free(&k->qinv);
    rsa_slice_free(a, k->dp);
    rsa_slice_free(a, k->dq);
    memset(k, 0, sizeof *k);
    mem_free(a, k, sizeof *k, _Alignof(RsaFipsKey));
}

/* m.Nat().Bytes(m), from tmp. */
static Slice rsa_modulus_bytes(const BigmodModulus *m, Alloc *tmp) {
    return bigmod_nat_bytes(&m->nat, tmp, m);
}

/* NewModulus(m.Nat().SubOne(m).Bytes(m)), from tmp. */
static BigmodModulus *rsa_minus_one(const BigmodModulus *m, Alloc *tmp, Error *err) {
    BigmodNat *x = bigmod_nat_sub_one(bigmod_modulus_nat(m, tmp), m);
    return bigmod_new_modulus(tmp, bigmod_nat_bytes(x, tmp, m), err);
}

/* checkPublicKey. */
static Error rsa_check_public(const BigmodModulus *n, Int e) {
    if (n == NULL)
        return rsa_error("crypto/rsa: missing public modulus");
    if (bigmod_nat_is_odd(&n->nat) == 0)
        return rsa_error("crypto/rsa: public modulus is even");
    if (e < 2)
        return rsa_error("crypto/rsa: public exponent too small or negative");
    if ((e & 1) == 0)
        return rsa_error("crypto/rsa: public exponent is even");
    if ((uint64_t)e > UINT64_C(0x7fffffff))
        return rsa_error("crypto/rsa: public exponent too large");
    return BURROW_NO_ERROR;
}

/* checkPrivateKey. */
static Error rsa_check_private(const RsaFipsKey *k, Alloc *tmp) {
    Error e = rsa_check_public(k->n, k->e);
    if (BURROW_FAILED(e))
        return e;
    if (!k->crt)
        return BURROW_NO_ERROR;

    const BigmodModulus *N = k->n, *p = k->p, *q = k->q;
    Slice pb = rsa_modulus_bytes(p, tmp);
    Slice qb = rsa_modulus_bytes(q, tmp);

    /* Check that pq ≡ 1 mod N (and that p < N and q < N). */
    BigmodNat *pN = bigmod_nat_expand_for(bigmod_new_nat(tmp), N);
    if (bigmod_nat_set_bytes(pN, pb, N, NULL) == NULL)
        return rsa_error("crypto/rsa: invalid prime");
    BigmodNat *qN = bigmod_nat_expand_for(bigmod_new_nat(tmp), N);
    if (bigmod_nat_set_bytes(qN, qb, N, NULL) == NULL)
        return rsa_error("crypto/rsa: invalid prime");
    if (bigmod_nat_is_zero(bigmod_nat_mul(pN, qN, N)) != 1)
        return rsa_error("crypto/rsa: p * q != n");

    /* Check that de ≡ 1 mod p-1, and de ≡ 1 mod q-1. */
    BigmodModulus *pm1 = rsa_minus_one(p, tmp, NULL);
    if (pm1 == NULL)
        return rsa_error("crypto/rsa: invalid prime");
    BigmodNat *dP = bigmod_nat_set_bytes(bigmod_new_nat(tmp), k->dp, pm1, NULL);
    if (dP == NULL)
        return rsa_error("crypto/rsa: invalid CRT exponent");
    BigmodNat *de = bigmod_new_nat(tmp);
    bigmod_nat_expand_for(bigmod_nat_set_uint(de, (Uint)k->e), pm1);
    bigmod_nat_mul(de, dP, pm1);
    if (bigmod_nat_is_one(de) != 1)
        return rsa_error("crypto/rsa: invalid CRT exponent");

    BigmodModulus *qm1 = rsa_minus_one(q, tmp, NULL);
    if (qm1 == NULL)
        return rsa_error("crypto/rsa: invalid prime");
    BigmodNat *dQ = bigmod_nat_set_bytes(bigmod_new_nat(tmp), k->dq, qm1, NULL);
    if (dQ == NULL)
        return rsa_error("crypto/rsa: invalid CRT exponent");
    bigmod_nat_expand_for(bigmod_nat_set_uint(de, (Uint)k->e), qm1);
    bigmod_nat_mul(de, dQ, qm1);
    if (bigmod_nat_is_one(de) != 1)
        return rsa_error("crypto/rsa: invalid CRT exponent");

    /* Check that qInv * q ≡ 1 mod p. */
    BigmodNat *qP = bigmod_nat_set_overflowing_bytes(bigmod_new_nat(tmp), qb, p, NULL);
    if (qP == NULL)
        qP = bigmod_nat_mod(bigmod_new_nat(tmp), &q->nat, p);
    if (bigmod_nat_is_one(bigmod_nat_mul(qP, &k->qinv, p)) != 1)
        return rsa_error("crypto/rsa: invalid CRT coefficient");

    /* Check that |p - q| > 2^(nlen/2 - 100). */
    BigmodNat *dP1 = bigmod_nat_mod(bigmod_new_nat(tmp), &k->d, pm1);
    if (bigmod_nat_equal(dP1, dP) != 1)
        return rsa_error("crypto/rsa: d does not match dP");
    BigmodNat *dQ1 = bigmod_nat_mod(bigmod_new_nat(tmp), &k->d, qm1);
    if (bigmod_nat_equal(dQ1, dQ) != 1)
        return rsa_error("crypto/rsa: d does not match dQ");

    BigmodNat *diff = bigmod_new_nat(tmp);
    BigmodNat *qP2 = bigmod_nat_set_bytes(bigmod_new_nat(tmp), qb, p, NULL);
    if (qP2 == NULL) {
        BigmodNat *pQ = bigmod_nat_set_bytes(bigmod_new_nat(tmp), pb, q, NULL);
        if (pQ == NULL)
            return rsa_error("crypto/rsa: p == q");
        bigmod_nat_sub(bigmod_nat_expand_for(diff, q), pQ, q);
    } else {
        bigmod_nat_sub(bigmod_nat_expand_for(diff, p), qP2, p);
    }
    if (bigmod_nat_bit_len_var_time(diff) <= bigmod_modulus_bit_len(N) / 2 - 100)
        return rsa_error("crypto/rsa: |p - q| too small");

    /* Check that d > 2^(nlen/2). */
    if (bigmod_nat_bit_len_var_time(&k->d) <= bigmod_modulus_bit_len(N) / 2)
        return rsa_error("crypto/rsa: d too small");
    return BURROW_NO_ERROR;
}

/* newPrivateKey, for k with n, e, d, p and q set: works out dP, dQ and qInv
 * and checks the key. */
static Error rsa_fips_complete(RsaFipsKey *k, Alloc *tmp) {
    const BigmodModulus *p = k->p, *q = k->q;
    Error e = BURROW_NO_ERROR;

    BigmodModulus *pm1 = rsa_minus_one(p, tmp, &e);
    if (pm1 == NULL)
        return e;
    k->dp =
        bigmod_nat_bytes(bigmod_nat_mod(bigmod_new_nat(tmp), &k->d, pm1), k->a, pm1);

    BigmodModulus *qm1 = rsa_minus_one(q, tmp, &e);
    if (qm1 == NULL)
        return e;
    k->dq =
        bigmod_nat_bytes(bigmod_nat_mod(bigmod_new_nat(tmp), &k->d, qm1), k->a, qm1);

    /* Constant-time modular inversion with prime modulus by Fermat's Little
     * Theorem: qInv = q⁻¹ mod p = q^(p-2) mod p. */
    if (bigmod_nat_is_odd(&p->nat) == 0)
        return rsa_error("crypto/rsa: p is even");
    BigmodNat *pm2 =
        bigmod_nat_sub_one(bigmod_nat_sub_one(bigmod_modulus_nat(p, tmp), p), p);
    Slice pm2b = bigmod_nat_bytes(pm2, tmp, p);
    BigmodNat *qinv = bigmod_nat_mod(bigmod_new_nat(tmp), &q->nat, p);
    bigmod_nat_exp(&k->qinv, qinv, pm2b, p);

    k->crt = true;
    return rsa_check_private(k, tmp);
}

/* NewPrivateKey. */
static RsaFipsKey *rsa_fips_new_private_key(Alloc *a, Alloc *tmp, Slice N, Int e,
                                            Slice d, Slice P, Slice Q, Error *err) {
    RsaFipsKey *k = rsa_fips_key_new(a);
    k->e = e;
    if ((k->n = bigmod_new_modulus(k->a, N, err)) == NULL)
        goto fail;
    if ((k->p = bigmod_new_modulus(k->a, P, err)) == NULL)
        goto fail;
    if ((k->q = bigmod_new_modulus(k->a, Q, err)) == NULL)
        goto fail;
    if (bigmod_nat_set_bytes(&k->d, d, k->n, err) == NULL)
        goto fail;
    Error ce = rsa_fips_complete(k, tmp);
    if (BURROW_FAILED(ce)) {
        BURROW_OUT(err, ce);
        goto fail;
    }
    return k;

fail:
    rsa_fips_key_free(k);
    return NULL;
}

/* NewPrivateKeyWithPrecomputation. */
static RsaFipsKey *
rsa_fips_new_private_key_with_precomputation(Alloc *a, Alloc *tmp, Slice N, Int e,
                                             Slice d, Slice P, Slice Q, Slice dP,
                                             Slice dQ, Slice qInv, Error *err) {
    RsaFipsKey *k = rsa_fips_key_new(a);
    k->e = e;
    if ((k->n = bigmod_new_modulus(k->a, N, err)) == NULL)
        goto fail;
    if ((k->p = bigmod_new_modulus(k->a, P, err)) == NULL)
        goto fail;
    if ((k->q = bigmod_new_modulus(k->a, Q, err)) == NULL)
        goto fail;
    if (bigmod_nat_set_bytes(&k->d, d, k->n, err) == NULL)
        goto fail;
    if (bigmod_nat_set_bytes(&k->qinv, qInv, k->p, err) == NULL)
        goto fail;
    k->dp = rsa_clone(k->a, dP, NULL);
    k->dq = rsa_clone(k->a, dQ, NULL);
    if ((k->dp.p == NULL && dP.len > 0) || (k->dq.p == NULL && dQ.len > 0))
        rsa_oom();
    k->crt = true;
    Error ce = rsa_check_private(k, tmp);
    if (BURROW_FAILED(ce)) {
        BURROW_OUT(err, ce);
        goto fail;
    }
    return k;

fail:
    rsa_fips_key_free(k);
    return NULL;
}

/* NewPrivateKeyWithoutCRT. */
static RsaFipsKey *rsa_fips_new_private_key_without_crt(Alloc *a, Alloc *tmp, Slice N,
                                                        Int e, Slice d, Error *err) {
    RsaFipsKey *k = rsa_fips_key_new(a);
    k->e = e;
    if ((k->n = bigmod_new_modulus(k->a, N, err)) == NULL)
        goto fail;
    if (bigmod_nat_set_bytes(&k->d, d, k->n, err) == NULL)
        goto fail;
    Error ce = rsa_check_private(k, tmp);
    if (BURROW_FAILED(ce)) {
        BURROW_OUT(err, ce);
        goto fail;
    }
    return k;

fail:
    rsa_fips_key_free(k);
    return NULL;
}

/* PrivateKey.Export, from tmp. */
static RsaFipsExport rsa_fips_export(const RsaFipsKey *k, Alloc *tmp) {
    RsaFipsExport x;
    memset(&x, 0, sizeof x);
    x.n = rsa_modulus_bytes(k->n, tmp);
    x.e = k->e;
    x.d = bigmod_nat_bytes(&k->d, tmp, k->n);
    x.p = x.q = x.dp = x.dq = x.qinv = slice_nil(TYPE_BYTE);
    if (!k->crt)
        return x;
    x.crt = true;
    x.p = rsa_modulus_bytes(k->p, tmp);
    x.q = rsa_modulus_bytes(k->q, tmp);
    x.dp = rsa_clone(tmp, k->dp, NULL);
    x.dq = rsa_clone(tmp, k->dq, NULL);
    x.qinv = bigmod_nat_bytes(&k->qinv, tmp, k->p);
    return x;
}

/* --------------------------------------------------- encrypt and decrypt */

/* encrypt: plaintext^e mod N, from tmp. */
static bool rsa_fips_encrypt(const BigmodModulus *N, Int e, Slice plaintext, Alloc *tmp,
                             Slice *out, Error *err) {
    BigmodNat *m = bigmod_nat_set_bytes(bigmod_new_nat(tmp), plaintext, N, err);
    if (m == NULL)
        return false;
    BigmodNat *c = bigmod_nat_exp_short_var_time(bigmod_new_nat(tmp), m, (Uint)e, N);
    *out = bigmod_nat_bytes(c, tmp, N);
    return true;
}

/* decrypt: ciphertext^d mod N, from tmp, and false for ErrDecryption. With
 * check, the result is encrypted again and compared, which stops a fault in
 * the CRT from leaking the key. */
static bool rsa_fips_decrypt(const RsaFipsKey *k, Slice ciphertext, bool check,
                             Alloc *tmp, Slice *out) {
    const BigmodModulus *N = k->n;
    BigmodNat *c = bigmod_nat_set_bytes(bigmod_new_nat(tmp), ciphertext, N, NULL);
    if (c == NULL)
        return false;

    BigmodNat *m = bigmod_new_nat(tmp);
    if (!k->crt) {
        bigmod_nat_exp(m, c, bigmod_nat_bytes(&k->d, tmp, N), N);
    } else {
        const BigmodModulus *P = k->p, *Q = k->q;
        BigmodNat *t0 = bigmod_new_nat(tmp);
        /* m = c ^ Dp mod p */
        bigmod_nat_exp(m, bigmod_nat_mod(t0, c, P), k->dp, P);
        /* m2 = c ^ Dq mod q */
        BigmodNat *m2 =
            bigmod_nat_exp(bigmod_new_nat(tmp), bigmod_nat_mod(t0, c, Q), k->dq, Q);
        /* m = m - m2 mod p */
        bigmod_nat_sub(m, bigmod_nat_mod(t0, m2, P), P);
        /* m = m * Qinv mod p */
        bigmod_nat_mul(m, &k->qinv, P);
        /* m = m * q mod N */
        bigmod_nat_mul(bigmod_nat_expand_for(m, N), bigmod_nat_mod(t0, &Q->nat, N), N);
        /* m = m + m2 mod N */
        bigmod_nat_add(m, bigmod_nat_expand_for(m2, N), N);
    }

    if (check) {
        BigmodNat *c1 =
            bigmod_nat_exp_short_var_time(bigmod_new_nat(tmp), m, (Uint)k->e, N);
        if (bigmod_nat_equal(c1, c) != 1)
            return false;
    }
    *out = bigmod_nat_bytes(m, tmp, N);
    return true;
}

/* ------------------------------------------------------ PKCS #1 v1.5 (FIPS) */

/* pkcs1v15ConstructEM, from tmp. An empty hash means hashed is signed as it
 * is. */
static bool rsa_pkcs1_v15_construct_em(Int k, Str hash, Slice hashed, Alloc *tmp,
                                       Slice *out, Error *err) {
    /* Special case: crypto.Hash(0) is used to indicate that the data is
     * signed directly. */
    Slice prefix = slice_nil(TYPE_BYTE);
    if (hash.len > 0) {
        if (!burrow__rsa_hash_prefix(hash, &prefix)) {
            rsa_set_error(err, "crypto/rsa: unsupported hash function");
            return false;
        }
        if (hashed.len != rsa_hash_size(hash)) {
            rsa_set_error(
                err, "crypto/rsa: hashed message length does not match hash function");
            return false;
        }
    }

    /* EM = 0x00 || 0x01 || PS || 0x00 || T */
    if (k < prefix.len + hashed.len + 2 + 8 + 1) {
        BURROW_OUT(err, rsa_err_message_too_long);
        return false;
    }
    Slice em = rsa_make(tmp, k);
    uint8_t *p = rsa_p(em);
    p[1] = 1;
    for (Int i = 2; i < k - prefix.len - hashed.len - 1; i++)
        p[i] = 0xff;
    if (prefix.len > 0)
        memcpy(p + k - prefix.len - hashed.len, prefix.p, (size_t)prefix.len);
    if (hashed.len > 0)
        memcpy(p + k - hashed.len, hashed.p, (size_t)hashed.len);
    *out = em;
    return true;
}

/* signPKCS1v15. */
static bool rsa_fips_sign_pkcs1_v15(const RsaFipsKey *k, Str hash, Slice hashed,
                                    Alloc *tmp, Slice *out, Error *err) {
    Slice em;
    if (!rsa_pkcs1_v15_construct_em(bigmod_modulus_size(k->n), hash, hashed, tmp, &em,
                                    err))
        return false;
    if (!rsa_fips_decrypt(k, em, true, tmp, out)) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }
    return true;
}

/* verifyPKCS1v15. */
static Error rsa_fips_verify_pkcs1_v15(const BigmodModulus *N, Int e, Str hash,
                                       Slice hashed, Slice sig, Alloc *tmp) {
    Error ce = rsa_check_public(N, e);
    if (BURROW_FAILED(ce))
        return ce;

    /* RFC 8017 Section 8.2.2: If the length of the signature S is not k
     * octets (where k is the length in octets of the RSA modulus n), output
     * "invalid signature" and stop. */
    if (bigmod_modulus_size(N) != sig.len)
        return rsa_err_verification;

    Slice em, expected;
    if (!rsa_fips_encrypt(N, e, sig, tmp, &em, NULL))
        return rsa_err_verification;
    if (!rsa_pkcs1_v15_construct_em(bigmod_modulus_size(N), hash, hashed, tmp,
                                    &expected, NULL))
        return rsa_err_verification;
    if (em.len != expected.len || memcmp(em.p, expected.p, (size_t)em.len) != 0)
        return rsa_err_verification;
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------- PSS and OAEP (FIPS) */

static void rsa_inc_counter(uint8_t c[4]) {
    if (++c[3] != 0)
        return;
    if (++c[2] != 0)
        return;
    if (++c[1] != 0)
        return;
    c[0]++;
}

/* mgf1XOR XORs the bytes in out with a mask generated using the MGF1 function
 * specified in PKCS #1 v2.1. */
static void rsa_mgf1_xor(Slice out, Hash hash, Slice seed, Alloc *tmp) {
    uint8_t counter[4] = {0, 0, 0, 0};
    uint8_t *o = rsa_p(out);
    Int done = 0;
    while (done < out.len) {
        hash_reset(hash);
        (void)hash_write(hash, seed, NULL);
        (void)hash_write(hash, slice_from(counter, 4, 4, TYPE_BYTE), NULL);
        Slice digest = hash_sum(tmp, hash, slice_nil(TYPE_BYTE));
        const uint8_t *dg = rsa_p(digest);
        for (Int i = 0; i < digest.len && done < out.len; i++)
            o[done++] ^= dg[i];
        rsa_inc_counter(counter);
    }
}

/* H = Hash(0x00 * 8 || mHash || salt), from tmp. */
static Slice rsa_pss_hash(Hash hash, Slice m_hash, Slice salt, Alloc *tmp) {
    uint8_t prefix[8] = {0};
    hash_reset(hash);
    (void)hash_write(hash, slice_from(prefix, 8, 8, TYPE_BYTE), NULL);
    (void)hash_write(hash, m_hash, NULL);
    (void)hash_write(hash, salt, NULL);
    return hash_sum(tmp, hash, slice_nil(TYPE_BYTE));
}

/* emsaPSSEncode, from tmp. See RFC 8017, Section 9.1.1. */
static bool rsa_emsa_pss_encode(Slice m_hash, Int em_bits, Slice salt, Hash hash,
                                Alloc *tmp, Slice *out, Error *err) {
    Int h_len = hash_size(hash);
    Int s_len = salt.len;
    Int em_len = (em_bits + 7) / 8;

    /* 2. Let mHash = Hash(M), an octet string of length hLen. */
    if (m_hash.len != h_len) {
        rsa_set_error(err, "crypto/rsa: input must be hashed with given hash");
        return false;
    }

    /* 3. If emLen < hLen + sLen + 2, output "encoding error" and stop. */
    if (em_len < h_len + s_len + 2) {
        BURROW_OUT(err, rsa_err_message_too_long);
        return false;
    }

    Slice em = rsa_make(tmp, em_len);
    uint8_t *p = rsa_p(em);
    Int ps_len = em_len - s_len - h_len - 2;
    Slice db = rsa_sub(em, 0, ps_len + 1 + s_len);

    /* 5. Let M' = (0x)00 00 00 00 00 00 00 00 || mHash || salt;
     * 6. Let H = Hash(M'), an octet string of length hLen. */
    Slice h = rsa_pss_hash(hash, m_hash, salt, tmp);
    memcpy(p + ps_len + 1 + s_len, h.p, (size_t)h_len);

    /* 7. Generate an octet string PS consisting of emLen - sLen - hLen - 2
     * zero octets. The length of PS may be 0.
     * 8. Let DB = PS || 0x01 || salt; DB is an octet string of length
     * emLen - hLen - 1. */
    p[ps_len] = 0x01;
    if (s_len > 0)
        memcpy(p + ps_len + 1, salt.p, (size_t)s_len);

    /* 9. Let dbMask = MGF(H, emLen - hLen - 1).
     * 10. Let maskedDB = DB \xor dbMask. */
    rsa_mgf1_xor(db, hash, h, tmp);

    /* 11. Set the leftmost 8 * emLen - emBits bits of the leftmost octet in
     * maskedDB to zero. */
    p[0] &= (uint8_t)(0xff >> (8 * em_len - em_bits));

    /* 12. Let EM = maskedDB || H || 0xbc. */
    p[em_len - 1] = 0xbc;
    *out = em;
    return true;
}

Slice burrow__rsa_emsa_pss_encode(Alloc *a, Slice m_hash, Int em_bits, Slice salt,
                                  Hash hash, Error *err) {
    RsaScratch s;
    rsa_scratch_init(&s);
    Slice em, out = slice_nil(TYPE_BYTE);
    if (rsa_emsa_pss_encode(m_hash, em_bits, salt, hash, s.a, &em, err))
        out = rsa_clone(a, em, err);
    rsa_scratch_free(&s);
    return out;
}

#define RSA_PSS_SALT_LENGTH_AUTODETECT (-1)

/* emsaPSSVerify. See RFC 8017, Section 9.1.2. em is changed. */
static Error rsa_emsa_pss_verify(Slice m_hash, Slice em, Int em_bits, Int s_len,
                                 Hash hash, Alloc *tmp) {
    /* 1. If the length of M is greater than the input limitation for the
     * hash function, output "inconsistent" and stop.
     * 2. Let mHash = Hash(M), an octet string of length hLen. */
    Int h_len = hash_size(hash);
    Int em_len = (em_bits + 7) / 8;
    if (em_len != em.len)
        return rsa_error("rsa: internal error: inconsistent length");
    if (h_len != m_hash.len)
        return rsa_err_verification;

    /* 3. If emLen < hLen + sLen + 2, output "inconsistent" and stop. */
    if (em_len < h_len + s_len + 2)
        return rsa_err_verification;

    /* 4. If the rightmost octet of EM does not have hexadecimal value 0xbc,
     * output "inconsistent" and stop. */
    uint8_t *p = rsa_p(em);
    if (p[em_len - 1] != 0xbc)
        return rsa_err_verification;

    /* 5. Let maskedDB be the leftmost emLen - hLen - 1 octets of EM, and let
     * H be the next hLen octets. */
    Slice db = rsa_sub(em, 0, em_len - h_len - 1);
    Slice h = rsa_sub(em, em_len - h_len - 1, em_len - 1);
    uint8_t *dbp = rsa_p(db);

    /* 6. If the leftmost 8 * emLen - emBits bits of the leftmost octet in
     * maskedDB are not all equal to zero, output "inconsistent" and stop. */
    uint8_t bit_mask = (uint8_t)(0xff >> (8 * em_len - em_bits));
    if ((p[0] & (uint8_t)~bit_mask) != 0)
        return rsa_err_verification;

    /* 7. Let dbMask = MGF(H, emLen - hLen - 1).
     * 8. Let DB = maskedDB \xor dbMask. */
    rsa_mgf1_xor(db, hash, h, tmp);

    /* 9. Set the leftmost 8 * emLen - emBits bits of the leftmost octet in DB
     * to zero. */
    dbp[0] &= bit_mask;

    /* If we don't know the salt length, look for the 0x01 delimiter. */
    if (s_len == RSA_PSS_SALT_LENGTH_AUTODETECT) {
        const uint8_t *one = db.len > 0 ? memchr(dbp, 0x01, (size_t)db.len) : NULL;
        if (one == NULL)
            return rsa_err_verification;
        s_len = db.len - (Int)(one - dbp) - 1;
    }

    /* 10. If the emLen - hLen - sLen - 2 leftmost octets of DB are not zero or
     * if the octet at position emLen - hLen - sLen - 1 (the leftmost position
     * is "position 1") does not have hexadecimal value 0x01, output
     * "inconsistent" and stop. */
    Int ps_len = em_len - h_len - s_len - 2;
    for (Int i = 0; i < ps_len; i++)
        if (dbp[i] != 0x00)
            return rsa_err_verification;
    if (dbp[ps_len] != 0x01)
        return rsa_err_verification;

    /* 11. Let salt be the last sLen octets of DB. */
    Slice salt = rsa_sub(db, db.len - s_len, db.len);

    /* 12. Let M' = (0x)00 00 00 00 00 00 00 00 || mHash || salt;
     * 13. Let H' = Hash(M'), an octet string of length hLen. */
    Slice h0 = rsa_pss_hash(hash, m_hash, salt, tmp);

    /* 14. If H = H', output "consistent." Otherwise, output "inconsistent." */
    if (h0.len != h.len || memcmp(h0.p, h.p, (size_t)h.len) != 0)
        return rsa_err_verification;
    return BURROW_NO_ERROR;
}

Error burrow__rsa_emsa_pss_verify(Slice m_hash, Slice em, Int em_bits, Int s_len,
                                  Hash hash) {
    RsaScratch s;
    rsa_scratch_init(&s);
    Error err = BURROW_NO_ERROR;
    Slice c = rsa_clone(s.a, em, &err);
    if (!BURROW_FAILED(err))
        err = rsa_emsa_pss_verify(m_hash, c, em_bits, s_len, hash, s.a);
    rsa_scratch_free(&s);
    return err;
}

/* PSSMaxSaltLength. */
static bool rsa_pss_max_salt_length(const BigmodModulus *N, Hash hash, Int *out) {
    Int salt_length = (bigmod_modulus_bit_len(N) - 1 + 7) / 8 - 2 - hash_size(hash);
    if (salt_length < 0)
        return false;
    *out = salt_length;
    return true;
}

/* SignPSS calculates the signature of hashed using RSASSA-PSS. */
static bool rsa_fips_sign_pss(IoReader rand, const RsaFipsKey *k, Hash hash,
                              Slice hashed, Int salt_length, Alloc *tmp, Slice *out,
                              Error *err) {
    if (salt_length < 0) {
        rsa_set_error(err, "crypto/rsa: salt length cannot be negative");
        return false;
    }
    Slice salt = rsa_make(tmp, salt_length);
    if (!rsa_read(rand, salt, err))
        return false;

    Int em_bits = bigmod_modulus_bit_len(k->n) - 1;
    Slice em;
    if (!rsa_emsa_pss_encode(hashed, em_bits, salt, hash, tmp, &em, err))
        return false;

    /* RFC 8017: "Note that the octet length of EM will be one less than k if
     * modBits - 1 is divisible by 8 and equal to k otherwise, where k is the
     * length in octets of the RSA modulus n." 🙄
     *
     * This is extremely annoying, as all other encrypt and decrypt inputs are
     * always the exact same size as the modulus. Since it only happens for
     * weird modulus sizes, fix it by padding inefficiently. */
    Int k_len = bigmod_modulus_size(k->n);
    if (em.len < k_len) {
        Slice em_new = rsa_make(tmp, k_len);
        memcpy(rsa_p(em_new) + k_len - em.len, em.p, (size_t)em.len);
        em = em_new;
    }

    if (!rsa_fips_decrypt(k, em, true, tmp, out)) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }
    return true;
}

/* verifyPSS. */
static Error rsa_fips_verify_pss(const BigmodModulus *N, Int e, Hash hash, Slice digest,
                                 Slice sig, Int salt_length, Alloc *tmp) {
    Error ce = rsa_check_public(N, e);
    if (BURROW_FAILED(ce))
        return ce;
    if (sig.len != bigmod_modulus_size(N))
        return rsa_err_verification;

    Int em_bits = bigmod_modulus_bit_len(N) - 1;
    Int em_len = (em_bits + 7) / 8;
    Slice em;
    if (!rsa_fips_encrypt(N, e, sig, tmp, &em, NULL))
        return rsa_err_verification;

    /* Like in signPSSWithSalt, deal with mismatches between emLen and the
     * size of the modulus. The spec would have us wire emLen into the
     * encoding function, but we'd rather always encode to the size of the
     * modulus and then strip leading zeroes if necessary. This only happens
     * for weird modulus sizes anyway. */
    while (em.len > em_len && em.len > 0) {
        if (rsa_p(em)[0] != 0)
            return rsa_err_verification;
        em = rsa_sub(em, 1, em.len);
    }

    return rsa_emsa_pss_verify(digest, em, em_bits, salt_length, hash, tmp);
}

/* VerifyPSSWithSaltLength. */
static Error rsa_fips_verify_pss_with_salt_length(const BigmodModulus *N, Int e,
                                                  Hash hash, Slice digest, Slice sig,
                                                  Int salt_length, Alloc *tmp) {
    if (salt_length < 0)
        return rsa_error("crypto/rsa: salt length cannot be negative");
    return rsa_fips_verify_pss(N, e, hash, digest, sig, salt_length, tmp);
}

/* EncryptOAEP encrypts the given message with RSAES-OAEP. */
static bool rsa_fips_encrypt_oaep(Hash hash, Hash mgf_hash, IoReader random,
                                  const BigmodModulus *N, Int e, Slice msg, Slice label,
                                  Alloc *tmp, Slice *out, Error *err) {
    Error ce = rsa_check_public(N, e);
    if (BURROW_FAILED(ce)) {
        BURROW_OUT(err, ce);
        return false;
    }
    Int k = bigmod_modulus_size(N);
    Int h_len = hash_size(hash);
    if (msg.len > k - 2 * h_len - 2) {
        BURROW_OUT(err, rsa_err_message_too_long);
        return false;
    }

    hash_reset(hash);
    (void)hash_write(hash, label, NULL);
    Slice l_hash = hash_sum(tmp, hash, slice_nil(TYPE_BYTE));

    Slice em = rsa_make(tmp, k);
    Slice seed = rsa_sub(em, 1, 1 + h_len);
    Slice db = rsa_sub(em, 1 + h_len, k);
    uint8_t *dbp = rsa_p(db);

    memcpy(dbp, l_hash.p, (size_t)h_len);
    dbp[db.len - msg.len - 1] = 1;
    if (msg.len > 0)
        memcpy(dbp + db.len - msg.len, msg.p, (size_t)msg.len);

    if (!rsa_read(random, seed, err))
        return false;

    rsa_mgf1_xor(db, mgf_hash, seed, tmp);
    rsa_mgf1_xor(seed, mgf_hash, db, tmp);

    return rsa_fips_encrypt(N, e, em, tmp, out, err);
}

/* DecryptOAEP decrypts ciphertext using RSAES-OAEP, giving the message in
 * *out, which points into memory from tmp. */
static bool rsa_fips_decrypt_oaep(Hash hash, Hash mgf_hash, const RsaFipsKey *priv,
                                  Slice ciphertext, Slice label, Alloc *tmp, Slice *out,
                                  Error *err) {
    Int k = bigmod_modulus_size(priv->n);
    Int h_len = hash_size(hash);
    if (ciphertext.len > k || k < h_len * 2 + 2) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }

    Slice em;
    if (!rsa_fips_decrypt(priv, ciphertext, false, tmp, &em)) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }

    hash_reset(hash);
    (void)hash_write(hash, label, NULL);
    Slice l_hash = hash_sum(tmp, hash, slice_nil(TYPE_BYTE));

    Int first_byte_is_zero = subtle_constant_time_byte_eq(rsa_p(em)[0], 0);

    Slice seed = rsa_sub(em, 1, h_len + 1);
    Slice db = rsa_sub(em, h_len + 1, em.len);

    rsa_mgf1_xor(seed, mgf_hash, db, tmp);
    rsa_mgf1_xor(db, mgf_hash, seed, tmp);

    Slice l_hash2 = rsa_sub(db, 0, h_len);

    /* We have to validate the plaintext in constant time in order to avoid
     * attacks like: J. Manger. A Chosen Ciphertext Attack on RSA Optimal
     * Asymmetric Encryption Padding (OAEP) as Standardized in PKCS #1 v2.0.
     * In J. Kilian, editor, Advances in Cryptology. */
    Int l_hash2_good = subtle_constant_time_compare(l_hash, l_hash2);

    /* The remainder of the plaintext must be zero or more 0x00, followed by
     * 0x01, followed by the message.
     *   lookingForIndex: 1 iff we are still looking for the 0x01
     *   index: the offset of the first 0x01 byte
     *   invalid: 1 iff we saw a non-zero byte before the 0x01. */
    Int looking_for_index = 1, index = 0, invalid = 0;
    Slice rest = rsa_sub(db, h_len, db.len);
    const uint8_t *r = rsa_p(rest);

    for (Int i = 0; i < rest.len; i++) {
        Int equals0 = subtle_constant_time_byte_eq(r[i], 0);
        Int equals1 = subtle_constant_time_byte_eq(r[i], 1);
        index = subtle_constant_time_select(looking_for_index & equals1, i, index);
        looking_for_index = subtle_constant_time_select(equals1, 0, looking_for_index);
        invalid = subtle_constant_time_select(looking_for_index & ~equals0, 1, invalid);
    }

    if ((first_byte_is_zero & l_hash2_good & ~invalid & ~looking_for_index) != 1) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }
    *out = rsa_sub(rest, index + 1, rest.len);
    return true;
}

/* ------------------------------------------------------- key generation */

struct RsaMillerRabin {
    BigmodModulus *w;
    Slice m;
};

/* millerRabinSetup prepares state that's reused across multiple iterations of
 * the Miller-Rabin test. */
static RsaMillerRabin *rsa_miller_rabin_setup(Alloc *a, Slice w, Error *err) {
    /* Check that w is odd, and precompute Montgomery parameters. */
    const uint8_t *wp = rsa_p(w);
    if (w.len == 0 || (wp[w.len - 1] & 3) != 3) {
        rsa_set_error(err, "candidate is not 3 mod 4");
        return NULL;
    }
    a = rsa_alloc(a);
    RsaMillerRabin *mr = mem_alloc(a, sizeof *mr, _Alignof(RsaMillerRabin));
    if (mr == NULL)
        rsa_oom();
    mr->w = bigmod_new_modulus(a, w, err);
    if (mr->w == NULL) {
        mem_free(a, mr, sizeof *mr, _Alignof(RsaMillerRabin));
        return NULL;
    }

    /* Compute m = (w-1)/2^a, where m is odd. Since w is 3 mod 4, a = 1. */
    BigmodNat *m = bigmod_nat_shift_right_by_one(
        bigmod_nat_sub_one(bigmod_modulus_nat(mr->w, a), mr->w));
    mr->m = bigmod_nat_bytes(m, a, mr->w);
    bigmod_nat_free(m);
    mem_free(a, m, sizeof *m, _Alignof(BigmodNat));
    while (rsa_p(mr->m)[0] == 0)
        mr->m = slice_from(rsa_p(mr->m) + 1, mr->m.len - 1, mr->m.cap - 1, TYPE_BYTE);
    return mr;
}

RsaMillerRabin *burrow__rsa_miller_rabin_setup(Alloc *a, Slice w, Error *err) {
    return rsa_miller_rabin_setup(a, w, err);
}

/* millerRabinIteration: 1 for possibly prime, 0 for composite and -1 for an
 * error, whose message goes to *msg when the error is ours and to *err when
 * it is bigmod's. */
static int rsa_miller_rabin_iteration(const RsaMillerRabin *mr, Slice bb, Alloc *tmp,
                                      const char **msg, Error *err) {
    /* Reject b ≤ 1 or b ≥ w − 1. */
    if (bb.len != (bigmod_modulus_bit_len(mr->w) + 7) / 8) {
        *msg = "incorrect length";
        return -1;
    }
    BigmodNat *b = bigmod_nat_set_bytes(bigmod_new_nat(tmp), bb, mr->w, err);
    if (b == NULL)
        return -1;
    if (bigmod_nat_is_zero(b) == 1 || bigmod_nat_is_one(b) == 1 ||
        bigmod_nat_is_minus_one(b, mr->w) == 1) {
        *msg = "out-of-range candidate";
        return -1;
    }

    /* Compute b^(m*2^i) mod w for successive i. If b^m mod w = 1, b is a
     * possible prime. If b^(m*2^i) mod w = -1 for some 0 <= i < a, b is a
     * possible prime. Otherwise, b is composite. Since a = 1, this is just
     * b^m mod w = ±1. */
    BigmodNat *z = bigmod_nat_exp(bigmod_new_nat(tmp), b, mr->m, mr->w);
    if (bigmod_nat_is_one(z) == 1 || bigmod_nat_is_minus_one(z, mr->w) == 1)
        return 1;
    return 0;
}

Error burrow__rsa_miller_rabin_iteration(const RsaMillerRabin *mr, Slice b,
                                         bool *possibly_prime) {
    RsaScratch s;
    rsa_scratch_init(&s);
    const char *msg = NULL;
    Error err = BURROW_NO_ERROR;
    int r = rsa_miller_rabin_iteration(mr, b, s.a, &msg, &err);
    rsa_scratch_free(&s);
    *possibly_prime = r == 1;
    if (r < 0 && msg != NULL)
        return rsa_error(msg);
    return err;
}

/* isPrime runs the Miller-Rabin Probabilistic Primality Test from FIPS 186-5,
 * Appendix B.3.1, after trial division by the first odd primes. */
static bool rsa_is_prime(RsaScratch *s, Slice w) {
    ArenaMark mark = arena_mark(&s->ar);
    Alloc *tmp = s->a;
    bool prime = false;

    RsaMillerRabin *mr = rsa_miller_rabin_setup(tmp, w, NULL);
    if (mr == NULL)
        goto done;

    BigmodNat *x = bigmod_new_nat(tmp);
    for (int i = 0; i < 255; i += 3) {
        Uint p1 = rsa_primes[i], p2 = rsa_primes[i + 1], p3 = rsa_primes[i + 2];
        bigmod_nat_set(x, &mr->w->nat);
        Uint r = bigmod_nat_div_short_var_time(x, p1 * p2 * p3);
        if (r % p1 == 0 || r % p2 == 0 || r % p3 == 0)
            goto done;
    }

    /* iterations is the number of Miller-Rabin rounds, each with a
     * randomly-selected base. The worst case error rate of the Miller-Rabin
     * test is 1/4 per round, and 1/4^50 = 2^-100 is the target, but FIPS
     * 186-5, Appendix C.1, Table C.1 allows fewer rounds for large candidates
     * that came from a random generator. */
    Int bits = bigmod_modulus_bit_len(mr->w);
    int iterations;
    if (bits >= 3747)
        iterations = 3;
    else if (bits >= 1345)
        iterations = 4;
    else if (bits >= 476)
        iterations = 5;
    else if (bits >= 400)
        iterations = 6;
    else if (bits >= 347)
        iterations = 7;
    else if (bits >= 308)
        iterations = 8;
    else if (bits >= 55)
        iterations = 27;
    else
        iterations = 34;

    Slice b = rsa_make(tmp, (bits + 7) / 8);
    uint8_t *bp = rsa_p(b);
    for (;;) {
        burrow__crypto_rand_system(b);
        Int excess = b.len * 8 - bits;
        bp[0] &= (uint8_t)(0xff >> excess);
        ArenaMark inner = arena_mark(&s->ar);
        const char *msg = NULL;
        int r = rsa_miller_rabin_iteration(mr, b, tmp, &msg, NULL);
        arena_release(&s->ar, inner);
        if (r < 0)
            continue;
        if (r == 0)
            goto done;
        if (--iterations == 0) {
            prime = true;
            goto done;
        }
    }

done:
    arena_release(&s->ar, mark);
    return prime;
}

/* randomPrime returns a random prime number of the given bit size following
 * the process in FIPS 186-5, Appendix A.1.3, in memory from s. */
static bool rsa_random_prime(RsaScratch *s, IoReader rand, Int bits, Slice *out,
                             Error *err) {
    if (bits < 16) {
        rsa_set_error(err, "rsa: prime size must be at least 16 bits");
        return false;
    }
    Slice b = rsa_make(s->a, (bits + 7) / 8);
    uint8_t *p = rsa_p(b);
    for (;;) {
        if (!rsa_read(rand, b, err))
            return false;
        /* Clear the most significant bits to reach the desired size. We use a
         * mask rather than right-shifting b[0] to make it easier to inject
         * test candidates, which can be represented as simple big-endian
         * integers. */
        Int excess = b.len * 8 - bits;
        p[0] &= (uint8_t)(0xff >> excess);

        /* Don't let the value be too small: set the most significant two
         * bits. Setting the top two bits, rather than just the top bit, means
         * that when two of these values are multiplied together, the result
         * isn't ever one bit short. */
        if (excess < 7) {
            p[0] |= (uint8_t)(0xc0 >> excess);
        } else {
            p[0] |= 0x01;
            p[1] |= 0x80;
        }

        /* Make the value odd since an even number certainly isn't prime, and
         * 3 mod 4 for the Miller-Rabin test and 7 mod 8 for totient. */
        p[b.len - 1] |= 0x07;

        if (rsa_is_prime(s, b)) {
            *out = b;
            return true;
        }
    }
}

/* totient computes the Carmichael totient function λ(N) = lcm(p-1, q-1), from
 * tmp. errDivisorTooLarge is the sentinel. */
static BigmodModulus *rsa_totient_mod(const BigmodModulus *p, const BigmodModulus *q,
                                      Alloc *tmp, Error *err) {
    BigmodNat *a = bigmod_nat_sub_one(bigmod_modulus_nat(p, tmp), p);
    BigmodNat *b = bigmod_nat_sub_one(bigmod_modulus_nat(q, tmp), q);

    /* lcm(a, b) = a×b / gcd(a, b) = a × (b / gcd(a, b)) */

    /* Our GCD requires at least one of the numbers to be odd. For LCM we only
     * need to preserve the larger prime power of each prime factor, so we can
     * right-shift the number with the fewest trailing zeros until it's odd.
     * For odd a, b and m >= n, lcm(a×2ᵐ, b×2ⁿ) = lcm(a×2ᵐ, b). */
    if (bigmod_nat_is_odd(a) == 1 || bigmod_nat_is_odd(b) == 1) {
        rsa_set_error(err, "rsa: internal error: p and q must be 7 mod 8");
        return NULL;
    }
    b = bigmod_nat_shift_right_by_one(b);
    if (bigmod_nat_is_odd(b) == 0) {
        rsa_set_error(err, "rsa: internal error: p and q must be 7 mod 8");
        return NULL;
    }

    BigmodNat *gcd = bigmod_nat_gcd_var_time(bigmod_new_nat(tmp), a, b, err);
    if (gcd == NULL)
        return NULL;
    if (bigmod_nat_is_odd(gcd) == 0) {
        rsa_set_error(err, "rsa: internal error: gcd(a, b) is even");
        return NULL;
    }

    /* To avoid implementing multiple-precision division, we just try again if
     * the divisor doesn't fit in a single word. This would have a chance of
     * 2⁻⁶⁴ on 64-bit platforms, and 2⁻³² on 32-bit platforms, but testing
     * 2⁻⁶⁴ edge cases is impractical, and we'd rather not behave differently
     * on 32-bit and 64-bit platforms, so we reject divisors above 2³²-1. */
    if (bigmod_nat_bit_len_var_time(gcd) + 1 > 32) {
        BURROW_OUT(err, burrow__rsa_err_divisor_too_large);
        return NULL;
    }

    if (bigmod_nat_is_zero(gcd) == 1 || gcd->limbs[0] == 0) {
        rsa_set_error(err, "rsa: internal error: gcd(a, b) is zero");
        return NULL;
    }
    if (bigmod_nat_div_short_var_time(b, gcd->limbs[0]) != 0) {
        rsa_set_error(err, "rsa: internal error: b is not divisible by gcd(a, b)");
        return NULL;
    }

    return bigmod_new_modulus_product(tmp, bigmod_nat_bytes(a, tmp, p),
                                      bigmod_nat_bytes(b, tmp, q), err);
}

Slice burrow__rsa_totient(Alloc *a, Slice p, Slice q, Error *err) {
    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);
    BigmodModulus *P = bigmod_new_modulus(s.a, p, err);
    BigmodModulus *Q = P != NULL ? bigmod_new_modulus(s.a, q, err) : NULL;
    BigmodModulus *l = Q != NULL ? rsa_totient_mod(P, Q, s.a, err) : NULL;
    if (l != NULL)
        out = rsa_clone(a, rsa_modulus_bytes(l, s.a), err);
    rsa_scratch_free(&s);
    return out;
}

/* GenerateKey generates a new RSA key pair of the given bit size. bits must
 * be at least 32. */
static RsaFipsKey *rsa_fips_generate_key(Alloc *a, IoReader rand, Int bits,
                                         Error *err) {
    if (bits < 32) {
        rsa_set_error(err, "rsa: key too small");
        return NULL;
    }

    RsaScratch s;
    rsa_scratch_init(&s);
    RsaFipsKey *k = NULL;
    for (;;) {
        ArenaMark mark = arena_mark(&s.ar);
        Slice p, q;
        if (!rsa_random_prime(&s, rand, (bits + 1) / 2, &p, err))
            break;
        if (!rsa_random_prime(&s, rand, bits / 2, &q, err))
            break;

        k = rsa_fips_key_new(a);
        if ((k->p = bigmod_new_modulus(k->a, p, err)) == NULL)
            goto fail;
        if ((k->q = bigmod_new_modulus(k->a, q, err)) == NULL)
            goto fail;

        BigmodNat *qp = bigmod_nat_expand_for(bigmod_modulus_nat(k->q, s.a), k->p);
        if (bigmod_nat_equal(qp, &k->p->nat) == 1) {
            rsa_set_error(err, "rsa: generated p == q, random source is broken");
            goto fail;
        }

        if ((k->n = bigmod_new_modulus_product(k->a, p, q, err)) == NULL)
            goto fail;
        if (bigmod_modulus_bit_len(k->n) != bits) {
            rsa_set_error(err, "rsa: internal error: modulus size incorrect");
            goto fail;
        }

        /* d can be safely computed as e⁻¹ mod φ(N) where φ(N) = (p-1)(q-1),
         * and indeed that's what both the original RSA paper and the
         * pre-FIPS crypto/rsa implementation did.
         *
         * However, FIPS 186-5, A.1.1(3) requires computing it as e⁻¹ mod λ(N)
         * where λ(N) = lcm(p-1, q-1). */
        Error terr = BURROW_NO_ERROR;
        BigmodModulus *lambda = rsa_totient_mod(k->p, k->q, s.a, &terr);
        if (lambda == NULL) {
            if (errors_is(terr, burrow__rsa_err_divisor_too_large)) {
                rsa_fips_key_free(k);
                k = NULL;
                arena_release(&s.ar, mark);
                continue;
            }
            BURROW_OUT(err, terr);
            goto fail;
        }

        BigmodNat *e = bigmod_nat_set_uint(bigmod_new_nat(s.a), 65537);
        if (!bigmod_nat_inverse_var_time(&k->d, e, lambda)) {
            /* This checks that GCD(e, lcm(p-1, q-1)) = 1, which is equivalent
             * to checking GCD(e, p-1) = 1 and GCD(e, q-1) = 1 separately in
             * FIPS 186-5, Appendix A.1.3, steps 4.5 and 5.6. */
            rsa_fips_key_free(k);
            k = NULL;
            arena_release(&s.ar, mark);
            continue;
        }

        if (bigmod_nat_is_one(
                bigmod_nat_mul(bigmod_nat_expand_for(e, lambda), &k->d, lambda)) == 0) {
            rsa_set_error(err, "rsa: internal error: e*d != 1 mod λ(N)");
            goto fail;
        }

        /* FIPS 186-5, A.1.1(3) requires checking that d > 2^(nlen / 2).
         *
         * The probability of this check failing when d is derived from
         * (e, p, q) is roughly
         *
         *   2^(nlen/2) / 2^nlen = 2^(-nlen/2)
         *
         * so less than 2⁻¹²⁸ for keys larger than 256 bits.
         *
         * We still need to check to comply with FIPS 186-5, but knowing it
         * has negligible chance of failure we can defer the check to the end
         * of key generation and return an error if it fails. See
         * checkPrivateKey. */
        k->e = 65537;
        Error ce = rsa_fips_complete(k, s.a);
        if (BURROW_FAILED(ce)) {
            BURROW_OUT(err, ce);
            goto fail;
        }
        rsa_scratch_free(&s);
        return k;
    }
    rsa_scratch_free(&s);
    return NULL;

fail:
    rsa_fips_key_free(k);
    rsa_scratch_free(&s);
    return NULL;
}

/* ----------------------------------------------------- nonZeroRandomBytes */

Error burrow__rsa_non_zero_random_bytes(Slice s, IoReader random) {
    Error err = BURROW_NO_ERROR;
    if (s.len > 0 && random.vt == NULL)
        rsa_nil();
    (void)io_read_full(random, s, &err);
    if (BURROW_FAILED(err))
        return err;
    uint8_t *p = rsa_p(s);
    for (Int i = 0; i < s.len; i++) {
        while (p[i] == 0) {
            (void)io_read_full(random, slice_from(p + i, 1, 1, TYPE_BYTE), &err);
            if (BURROW_FAILED(err))
                return err;
            /* In tests, the PRNG may return all zeros so we do this to break
             * the pattern. */
            p[i] ^= 0x42;
        }
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------ key checks */

/* Writes "crypto/rsa: %d-bit keys are insecure (see ...)" as an error. */
static Error rsa_insecure_size(Int size) {
    static const char pre[] = "crypto/rsa: ";
    static const char post[] = "-bit keys are insecure (see "
                               "https://go.dev/pkg/crypto/rsa#hdr-Minimum_key_size)";
    char num[24];
    Int n = 0;
    uint64_t u = size < 0 ? (uint64_t)0 - (uint64_t)size : (uint64_t)size;
    do {
        num[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (size < 0)
        num[n++] = '-';
    char buf[sizeof pre + sizeof num + sizeof post];
    Int len = 0;
    memcpy(buf, pre, sizeof pre - 1);
    len += (Int)sizeof pre - 1;
    while (n > 0)
        buf[len++] = num[--n];
    memcpy(buf + len, post, sizeof post - 1);
    len += (Int)sizeof post - 1;
    return errors_new(error_allocator(), str_from_bytes(buf, len));
}

/* checkKeySize. */
static Error rsa_check_key_size(Int size) {
    if (size >= 1024)
        return BURROW_NO_ERROR;
    if ((rsa_debug_load() & RSA_DEBUG_1024MIN_OFF) != 0)
        return BURROW_NO_ERROR;
    return rsa_insecure_size(size);
}

/* checkPublicKeySize. */
static Error rsa_check_public_key_size(const RsaPublicKey *k) {
    if (k->n == NULL)
        return rsa_error("crypto/rsa: missing public modulus");
    return rsa_check_key_size(big_int_bit_len(k->n));
}

static bool rsa_fail(Error *err, Error e) {
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return true;
    }
    return false;
}

/* ------------------------------------------------------- precomputation */

static void rsa_precomputed_free(RsaPrecomputedValues *pv, Alloc *a) {
    rsa_big_free(pv->dp, a);
    rsa_big_free(pv->dq, a);
    rsa_big_free(pv->qinv, a);
    if (pv->crt_values != NULL) {
        for (Int i = 0; i < pv->crt_values_len; i++) {
            rsa_big_free(pv->crt_values[i].exp, a);
            rsa_big_free(pv->crt_values[i].coeff, a);
            rsa_big_free(pv->crt_values[i].r, a);
        }
        mem_free(rsa_alloc(a), pv->crt_values,
                 (size_t)pv->crt_values_len * sizeof(RsaCRTValue),
                 _Alignof(RsaCRTValue));
    }
    rsa_fips_key_free(pv->fips);
    memset(pv, 0, sizeof *pv);
}

/* precomputeLegacy. */
static bool rsa_precompute_legacy(const RsaPrivateKey *priv, Alloc *a, Alloc *tmp,
                                  RsaPrecomputedValues *out, Error *err) {
    RsaFipsKey *k = rsa_fips_new_private_key_without_crt(
        a, tmp, big_int_bytes(priv->public_key.n, tmp), priv->public_key.e,
        big_int_bytes(priv->d, tmp), err);
    if (k == NULL)
        return false;
    out->fips = k;

    /* Not enough primes to compute the CRT values. */
    if (priv->primes_len < 2)
        return true;

    for (Int i = 0; i < priv->primes_len; i++) {
        const BigInt *prime = priv->primes[i];
        if (prime == NULL) {
            rsa_set_error(err, "crypto/rsa: prime factor is nil");
            goto fail;
        }
        if (big_int_cmp(prime, big_new_int(tmp, 1)) <= 0) {
            rsa_set_error(err, "crypto/rsa: prime factor is <= 1");
            goto fail;
        }
    }

    BigInt one = BIG_INT(tmp);
    big_int_set_int64(&one, 1);
    out->dp = big_int_sub(rsa_big_new(a), priv->primes[0], &one);
    big_int_mod(out->dp, priv->d, out->dp);
    out->dq = big_int_sub(rsa_big_new(a), priv->primes[1], &one);
    big_int_mod(out->dq, priv->d, out->dq);
    out->qinv = rsa_big_new(a);
    if (big_int_mod_inverse(out->qinv, priv->primes[1], priv->primes[0]) == NULL) {
        rsa_set_error(err, "crypto/rsa: prime factors are not relatively prime");
        goto fail;
    }

    BigInt r = BIG_INT(tmp);
    big_int_mul(&r, priv->primes[0], priv->primes[1]);
    Int n = priv->primes_len - 2;
    if (n > 0) {
        out->crt_values = mem_alloc(rsa_alloc(a), (size_t)n * sizeof(RsaCRTValue),
                                    _Alignof(RsaCRTValue));
        if (out->crt_values == NULL)
            rsa_oom();
        out->crt_values_len = n;
    }
    for (Int i = 2; i < priv->primes_len; i++) {
        const BigInt *prime = priv->primes[i];
        RsaCRTValue *values = &out->crt_values[i - 2];
        values->exp = big_int_sub(rsa_big_new(a), prime, &one);
        big_int_mod(values->exp, priv->d, values->exp);
        values->r = big_int_set(rsa_big_new(a), &r);
        values->coeff = rsa_big_new(a);
        if (big_int_mod_inverse(values->coeff, &r, prime) == NULL) {
            rsa_set_error(err, "crypto/rsa: prime factors are not relatively prime");
            goto fail;
        }
        big_int_mul(&r, &r, prime);
    }
    return true;

fail:
    rsa_precomputed_free(out, a);
    return false;
}

/* precompute: the values for priv, from a, in *out. */
static bool rsa_precompute(const RsaPrivateKey *priv, Alloc *a, Alloc *tmp,
                           RsaPrecomputedValues *out, Error *err) {
    memset(out, 0, sizeof *out);

    /* Check that the struct is not missing the values needed for the FIPS
     * key, which we can't afford to panic on. */
    if (priv->public_key.n == NULL) {
        rsa_set_error(err, "crypto/rsa: missing public modulus");
        return false;
    }
    if (priv->d == NULL) {
        rsa_set_error(err, "crypto/rsa: missing private exponent");
        return false;
    }
    if (priv->primes_len != 2)
        return rsa_precompute_legacy(priv, a, tmp, out, err);
    if (priv->primes[0] == NULL) {
        rsa_set_error(err, "crypto/rsa: prime P is nil");
        return false;
    }
    if (priv->primes[1] == NULL) {
        rsa_set_error(err, "crypto/rsa: prime Q is nil");
        return false;
    }

    Slice N = big_int_bytes(priv->public_key.n, tmp);
    Slice d = big_int_bytes(priv->d, tmp);
    Slice P = big_int_bytes(priv->primes[0], tmp);
    Slice Q = big_int_bytes(priv->primes[1], tmp);

    /* If the CRT values are already set, use them. */
    const RsaPrecomputedValues *pv = &priv->precomputed;
    if (pv->dp != NULL && pv->dq != NULL && pv->qinv != NULL) {
        RsaFipsKey *k = rsa_fips_new_private_key_with_precomputation(
            a, tmp, N, priv->public_key.e, d, P, Q, big_int_bytes(pv->dp, tmp),
            big_int_bytes(pv->dq, tmp), big_int_bytes(pv->qinv, tmp), err);
        if (k == NULL)
            return false;
        *out = *pv;
        out->fips = k;
        out->crt_values = NULL;
        out->crt_values_len = 0;
        return true;
    }

    RsaFipsKey *k =
        rsa_fips_new_private_key(a, tmp, N, priv->public_key.e, d, P, Q, err);
    if (k == NULL)
        return false;
    out->fips = k;
    RsaFipsExport x = rsa_fips_export(k, tmp);
    out->dp = rsa_big_from_bytes(a, x.dp);
    out->dq = rsa_big_from_bytes(a, x.dq);
    out->qinv = rsa_big_from_bytes(a, x.qinv);
    return true;
}

/* precomputedIsConsistent. */
static bool rsa_precomputed_is_consistent(const RsaPrivateKey *priv, Alloc *tmp) {
    if (priv->precomputed.fips == NULL)
        return false;
    RsaFipsExport x = rsa_fips_export(priv->precomputed.fips, tmp);
    if (!rsa_big_equal_to_bytes(priv->public_key.n, x.n, tmp) ||
        priv->public_key.e != x.e || !rsa_big_equal_to_bytes(priv->d, x.d, tmp))
        return false;
    if (priv->primes_len != 2)
        return !x.crt;
    return rsa_big_equal_to_bytes(priv->primes[0], x.p, tmp) &&
           rsa_big_equal_to_bytes(priv->primes[1], x.q, tmp) &&
           rsa_big_equal_to_bytes(priv->precomputed.dp, x.dp, tmp) &&
           rsa_big_equal_to_bytes(priv->precomputed.dq, x.dq, tmp) &&
           rsa_big_equal_to_bytes(priv->precomputed.qinv, x.qinv, tmp);
}

/* fipsPrivateKey: the cached key, or one worked out into tmp. */
static const RsaFipsKey *rsa_fips_private_key(const RsaPrivateKey *priv, Alloc *tmp,
                                              Error *err) {
    if (priv->precomputed.fips != NULL)
        return priv->precomputed.fips;
    RsaPrecomputedValues pv;
    if (!rsa_precompute(priv, tmp, tmp, &pv, err))
        return NULL;
    return pv.fips;
}

/* fipsPublicKey: the modulus of pub, from tmp. */
static BigmodModulus *rsa_fips_public_key(const RsaPublicKey *pub, Alloc *tmp,
                                          Error *err) {
    return bigmod_new_modulus(tmp, big_int_bytes(pub->n, tmp), err);
}

void rsa_private_key_precompute(RsaPrivateKey *priv, Alloc *a) {
    RsaScratch s;
    rsa_scratch_init(&s);
    if (!rsa_precomputed_is_consistent(priv, s.a)) {
        RsaPrecomputedValues pv;
        if (!rsa_precompute(priv, a, s.a, &pv, NULL))
            priv->precomputed.fips = NULL;
        else
            priv->precomputed = pv;
    }
    rsa_scratch_free(&s);
}

Error rsa_private_key_validate(const RsaPrivateKey *priv) {
    /* We can operate on keys based on d alone, but they can't be encoded with
     * crypto/x509.MarshalPKCS1PrivateKey, which unfortunately doesn't return
     * an error. */
    if (priv->primes_len < 2)
        return rsa_error("crypto/rsa: missing primes");

    RsaScratch s;
    rsa_scratch_init(&s);
    Error err = BURROW_NO_ERROR;
    if (rsa_precomputed_is_consistent(priv, s.a)) {
        /* nothing to do */
    } else if (priv->precomputed.fips != NULL) {
        /* Normally, the precomputed values are always consistent with the
         * key, but the user can modify the key after calling Precompute. */
        err = rsa_error("crypto/rsa: precomputed values are inconsistent with the key");
    } else {
        RsaPrecomputedValues pv;
        (void)rsa_precompute(priv, s.a, s.a, &pv, &err);
    }
    rsa_scratch_free(&s);
    return err;
}

/* ------------------------------------------------------------ public keys */

Int rsa_public_key_size(const RsaPublicKey *pub) {
    if (pub->n == NULL)
        rsa_nil();
    return (big_int_bit_len(pub->n) + 7) / 8;
}

static bool rsa_public_equal(const RsaPublicKey *pub, const RsaPublicKey *xx) {
    return rsa_big_equal(pub->n, xx->n) && pub->e == xx->e;
}

bool rsa_public_key_equal(const RsaPublicKey *pub, CryptoPublicKey x) {
    if (x.t != TYPE_RSA_PUBLIC_KEY || x.data == NULL)
        return false;
    return rsa_public_equal(pub, x.data);
}

/* ----------------------------------------------------------- private keys */

Int rsa_private_key_size(const RsaPrivateKey *priv) {
    return rsa_public_key_size(&priv->public_key);
}

CryptoPublicKey rsa_private_key_public(const RsaPrivateKey *priv) {
    return BURROW_ANY(TYPE_RSA_PUBLIC_KEY, (uintptr_t)&priv->public_key);
}

bool rsa_private_key_equal(const RsaPrivateKey *priv, CryptoPrivateKey x) {
    if (x.t != TYPE_RSA_PRIVATE_KEY || x.data == NULL)
        return false;
    const RsaPrivateKey *xx = x.data;
    if (!rsa_public_equal(&priv->public_key, &xx->public_key) ||
        !rsa_big_equal(priv->d, xx->d))
        return false;
    if (priv->primes_len != xx->primes_len)
        return false;
    for (Int i = 0; i < priv->primes_len; i++)
        if (!rsa_big_equal(priv->primes[i], xx->primes[i]))
            return false;
    return true;
}

void rsa_private_key_free(RsaPrivateKey *priv, Alloc *a) {
    if (priv == NULL)
        return;
    rsa_big_free(priv->public_key.n, a);
    rsa_big_free(priv->d, a);
    if (priv->primes != NULL) {
        for (Int i = 0; i < priv->primes_len; i++)
            rsa_big_free(priv->primes[i], a);
        mem_free(rsa_alloc(a), priv->primes,
                 (size_t)priv->primes_len * sizeof(BigInt *), _Alignof(BigInt *));
    }
    rsa_precomputed_free(&priv->precomputed, a);
    memset(priv, 0, sizeof *priv);
    mem_free(rsa_alloc(a), priv, sizeof *priv, _Alignof(RsaPrivateKey));
}

static RsaPrivateKey *rsa_private_key_new(Alloc *a, Int nprimes) {
    RsaPrivateKey *priv =
        mem_alloc(rsa_alloc(a), sizeof *priv, _Alignof(RsaPrivateKey));
    if (priv == NULL)
        rsa_oom();
    priv->primes =
        mem_alloc(rsa_alloc(a), (size_t)nprimes * sizeof(BigInt *), _Alignof(BigInt *));
    if (priv->primes == NULL)
        rsa_oom();
    priv->primes_len = nprimes;
    return priv;
}

RsaPrivateKey *rsa_generate_key(Alloc *a, IoReader random, Int bits, Error *err) {
    if (rsa_fail(err, rsa_check_key_size(bits)))
        return NULL;

    random = rsa_reader(random);
    Error e = BURROW_NO_ERROR;
    RsaFipsKey *k = rsa_fips_generate_key(a, random, bits, &e);
    if (bits < 256 && k == NULL) {
        /* Toy-sized keys have a non-negligible chance of hitting two hard
         * failure cases: p == q and d <= 2^(nlen / 2).
         *
         * Since these are impossible to hit for real keys, we don't want to
         * make the production code path more complex and harder to think
         * about to handle them.
         *
         * Instead, just rerun the whole process a total of 8 times, which
         * brings the chance of failure for 32-bit keys down to the same as
         * for 256-bit keys. */
        for (int i = 1; i < 8 && k == NULL; i++) {
            e = BURROW_NO_ERROR;
            k = rsa_fips_generate_key(a, random, bits, &e);
        }
    }
    if (k == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }

    RsaScratch s;
    rsa_scratch_init(&s);
    RsaFipsExport x = rsa_fips_export(k, s.a);
    RsaPrivateKey *priv = rsa_private_key_new(a, 2);
    priv->public_key.n = rsa_big_from_bytes(a, x.n);
    priv->public_key.e = x.e;
    priv->d = rsa_big_from_bytes(a, x.d);
    priv->primes[0] = rsa_big_from_bytes(a, x.p);
    priv->primes[1] = rsa_big_from_bytes(a, x.q);
    priv->precomputed.fips = k;
    priv->precomputed.dp = rsa_big_from_bytes(a, x.dp);
    priv->precomputed.dq = rsa_big_from_bytes(a, x.dq);
    priv->precomputed.qinv = rsa_big_from_bytes(a, x.qinv);
    rsa_scratch_free(&s);
    return priv;
}

RsaPrivateKey *rsa_generate_multi_prime_key(Alloc *a, IoReader random, Int nprimes,
                                            Int bits, Error *err) {
    if (nprimes == 2)
        return rsa_generate_key(a, random, bits, err);

    random = rsa_reader(random);

    if (nprimes < 2) {
        rsa_set_error(err, "crypto/rsa: GenerateMultiPrimeKey: nprimes must be >= 2");
        return NULL;
    }

    if (bits < 64) {
        uint64_t shift = (uint64_t)(bits / nprimes);
        double prime_limit = shift >= 64 ? 0.0 : (double)(UINT64_C(1) << shift);
        /* pi approximates the number of primes less than primeLimit */
        double pi = prime_limit / (math_log(prime_limit) - 1);
        /* Generated primes start with 11 (in binary) so we can only use a
         * quarter of them. */
        pi /= 4;
        /* Use a factor of two to ensure that key generation terminates in a
         * reasonable amount of time. */
        pi /= 2;
        if (pi <= (double)nprimes) {
            rsa_set_error(
                err,
                "crypto/rsa: too few primes of given length to generate an RSA key");
            return NULL;
        }
    }

    RsaPrivateKey *priv = rsa_private_key_new(a, nprimes);
    priv->public_key.e = 65537;
    BigInt **primes = priv->primes;

    RsaScratch s;
    rsa_scratch_init(&s);
    for (;;) {
        ArenaMark mark = arena_mark(&s.ar);
        Int todo = bits;
        /* crypto/rand should set the top two bits in each prime. Thus each
         * prime has the form p_i = 2^bitlen(p_i) × 0.11... (in base 2). And
         * the product is:
         *
         *   P = 2^todo × α
         *
         * where α is the product of nprimes numbers of the form 0.11...
         *
         * If α < 1/2 (which can happen for nprimes > 2), we need to shift
         * todo to compensate for lost bits: the mean value of 0.11... is 7/8,
         * so todo + shift - nprimes * log2(7/8) ~= bits - 1/2 will give good
         * results. */
        if (nprimes >= 7)
            todo += (nprimes - 2) / 5;
        for (Int i = 0; i < nprimes; i++) {
            rsa_big_free(primes[i], a);
            primes[i] = crypto_rand_prime(a, random, todo / (nprimes - i), err);
            if (primes[i] == NULL) {
                rsa_scratch_free(&s);
                rsa_private_key_free(priv, a);
                return NULL;
            }
            todo -= big_int_bit_len(primes[i]);
        }

        /* Make sure that primes is pairwise unequal. */
        bool dup = false;
        for (Int i = 0; i < nprimes && !dup; i++)
            for (Int j = 0; j < i && !dup; j++)
                dup = big_int_cmp(primes[i], primes[j]) == 0;
        if (dup) {
            arena_release(&s.ar, mark);
            continue;
        }

        BigInt n = BIG_INT(s.a), totient = BIG_INT(s.a), pminus1 = BIG_INT(s.a);
        BigInt one = BIG_INT(s.a);
        big_int_set_int64(&one, 1);
        big_int_set_int64(&n, 1);
        big_int_set_int64(&totient, 1);
        for (Int i = 0; i < nprimes; i++) {
            big_int_mul(&n, &n, primes[i]);
            big_int_sub(&pminus1, primes[i], &one);
            big_int_mul(&totient, &totient, &pminus1);
        }
        if (big_int_bit_len(&n) != bits) {
            /* This should never happen for nprimes == 2 because crypto/rand
             * should set the top two bits in each prime. For nprimes > 2 we
             * hope it does not happen often. */
            arena_release(&s.ar, mark);
            continue;
        }

        rsa_big_free(priv->d, a);
        priv->d = rsa_big_new(a);
        BigInt e = BIG_INT(s.a);
        big_int_set_int64(&e, priv->public_key.e);
        if (big_int_mod_inverse(priv->d, &e, &totient) != NULL) {
            priv->public_key.n = big_int_set(rsa_big_new(a), &n);
            break;
        }
        arena_release(&s.ar, mark);
    }
    rsa_scratch_free(&s);

    rsa_private_key_precompute(priv, a);
    Error ve = rsa_private_key_validate(priv);
    if (BURROW_FAILED(ve)) {
        BURROW_OUT(err, ve);
        rsa_private_key_free(priv, a);
        return NULL;
    }
    return priv;
}

/* ---------------------------------------------------------- PSS (public) */

CryptoHash rsa_pss_options_hash_func(const RsaPSSOptions *opts) {
    return opts->hash;
}

static CryptoHash rsa_pss_opts_hash_func(void *self) {
    return rsa_pss_options_hash_func(self);
}

static const CryptoSignerOptsVT rsa_pss_opts_vt = {TYPE_RSA_PSS_OPTIONS,
                                                   rsa_pss_opts_hash_func};

CryptoSignerOpts rsa_pss_options_as_signer_opts(const RsaPSSOptions *opts) {
    return (CryptoSignerOpts){&rsa_pss_opts_vt, (void *)(uintptr_t)opts};
}

CryptoDecrypterOpts rsa_oaep_options_as_decrypter_opts(const RsaOAEPOptions *opts) {
    return BURROW_ANY(TYPE_RSA_OAEP_OPTIONS, (uintptr_t)opts);
}

CryptoDecrypterOpts
rsa_pkcs1_v15_decrypt_options_as_decrypter_opts(const RsaPKCS1v15DecryptOptions *opts) {
    return BURROW_ANY(TYPE_RSA_PKCS1V15_DECRYPT_OPTIONS, (uintptr_t)opts);
}

static Int rsa_pss_salt_length(const RsaPSSOptions *opts) {
    return opts == NULL ? RSA_PSS_SALT_LENGTH_AUTO : opts->salt_length;
}

Slice rsa_sign_pss(Alloc *a, IoReader rand, const RsaPrivateKey *priv, CryptoHash hash,
                   Slice digest, const RsaPSSOptions *opts, Error *err) {
    if (rsa_fail(err, rsa_check_public_key_size(&priv->public_key)))
        return slice_nil(TYPE_BYTE);
    if (opts != NULL && opts->hash != 0)
        hash = opts->hash;
    if (!crypto_hash_available(hash)) {
        rsa_unavailable(err, "crypto/rsa: requested hash function unavailable: ", hash);
        return slice_nil(TYPE_BYTE);
    }

    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);
    Hash h = crypto_hash_new(hash, s.a);
    const RsaFipsKey *k = rsa_fips_private_key(priv, s.a, err);
    if (k == NULL)
        goto done;

    Int salt_length = rsa_pss_salt_length(opts);
    switch (salt_length) {
    case RSA_PSS_SALT_LENGTH_AUTO:
        if (!rsa_pss_max_salt_length(k->n, h, &salt_length)) {
            BURROW_OUT(err, rsa_err_message_too_long);
            goto done;
        }
        break;
    case RSA_PSS_SALT_LENGTH_EQUALS_HASH:
        salt_length = hash_size(h);
        break;
    default:
        /* If we get here saltLength is either > 0 or < -1, in the latter case
         * we fail out. */
        if (salt_length <= 0) {
            rsa_set_error(err, "crypto/rsa: invalid PSS salt length");
            goto done;
        }
    }

    Slice sig;
    if (rsa_fips_sign_pss(rand, k, h, digest, salt_length, s.a, &sig, err))
        out = rsa_clone(a, sig, err);

done:
    rsa_scratch_free(&s);
    return out;
}

Error rsa_verify_pss(const RsaPublicKey *pub, CryptoHash hash, Slice digest, Slice sig,
                     const RsaPSSOptions *opts) {
    Error err = rsa_check_public_key_size(pub);
    if (BURROW_FAILED(err))
        return err;
    if (!crypto_hash_available(hash)) {
        rsa_unavailable(&err,
                        "crypto/rsa: requested hash function unavailable: ", hash);
        return err;
    }

    RsaScratch s;
    rsa_scratch_init(&s);
    Hash h = crypto_hash_new(hash, s.a);
    BigmodModulus *N = rsa_fips_public_key(pub, s.a, &err);
    if (N == NULL)
        goto done;

    Int salt_length = rsa_pss_salt_length(opts);
    switch (salt_length) {
    case RSA_PSS_SALT_LENGTH_AUTO:
        err = rsa_fips_verify_pss(N, pub->e, h, digest, sig,
                                  RSA_PSS_SALT_LENGTH_AUTODETECT, s.a);
        break;
    case RSA_PSS_SALT_LENGTH_EQUALS_HASH:
        err = rsa_fips_verify_pss_with_salt_length(N, pub->e, h, digest, sig,
                                                   hash_size(h), s.a);
        break;
    default:
        err = rsa_fips_verify_pss_with_salt_length(N, pub->e, h, digest, sig,
                                                   salt_length, s.a);
    }

done:
    rsa_scratch_free(&s);
    return err;
}

/* --------------------------------------------------------- OAEP (public) */

/* encryptOAEP. */
static Slice rsa_encrypt_oaep_hashes(Alloc *a, Hash hash, Hash mgf_hash,
                                     IoReader random, const RsaPublicKey *pub,
                                     Slice msg, Slice label, Error *err) {
    Slice out = slice_nil(TYPE_BYTE);
    if (rsa_fail(err, rsa_check_public_key_size(pub)))
        goto reset;

    RsaScratch s;
    rsa_scratch_init(&s);
    BigmodModulus *N = rsa_fips_public_key(pub, s.a, err);
    Slice ct;
    if (N != NULL && rsa_fips_encrypt_oaep(hash, mgf_hash, random, N, pub->e, msg,
                                           label, s.a, &ct, err))
        out = rsa_clone(a, ct, err);
    rsa_scratch_free(&s);

reset:
    hash_reset(hash);
    hash_reset(mgf_hash);
    return out;
}

Slice rsa_encrypt_oaep(Alloc *a, Hash hash, IoReader random, const RsaPublicKey *pub,
                       Slice msg, Slice label, Error *err) {
    return rsa_encrypt_oaep_hashes(a, hash, hash, random, pub, msg, label, err);
}

Slice rsa_encrypt_oaep_with_options(Alloc *a, IoReader random, const RsaPublicKey *pub,
                                    Slice msg, const RsaOAEPOptions *opts, Error *err) {
    if (!crypto_hash_available(opts->hash)) {
        rsa_unavailable(
            err, "crypto/rsa: requested hash function unavailable: ", opts->hash);
        return slice_nil(TYPE_BYTE);
    }
    if (opts->mgf_hash != 0 && !crypto_hash_available(opts->mgf_hash)) {
        rsa_unavailable(
            err, "crypto/rsa: requested hash function unavailable: ", opts->mgf_hash);
        return slice_nil(TYPE_BYTE);
    }
    RsaScratch s;
    rsa_scratch_init(&s);
    Hash h = crypto_hash_new(opts->hash, s.a);
    Hash mgf = crypto_hash_new(opts->mgf_hash == 0 ? opts->hash : opts->mgf_hash, s.a);
    Slice out = rsa_encrypt_oaep_hashes(a, h, mgf, random, pub, msg, opts->label, err);
    rsa_scratch_free(&s);
    return out;
}

/* decryptOAEP. */
static Slice rsa_decrypt_oaep_hashes(Alloc *a, Hash hash, Hash mgf_hash,
                                     const RsaPrivateKey *priv, Slice ciphertext,
                                     Slice label, Error *err) {
    if (rsa_fail(err, rsa_check_public_key_size(&priv->public_key)))
        return slice_nil(TYPE_BYTE);

    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);
    const RsaFipsKey *k = rsa_fips_private_key(priv, s.a, err);
    Slice msg;
    if (k != NULL &&
        rsa_fips_decrypt_oaep(hash, mgf_hash, k, ciphertext, label, s.a, &msg, err))
        out = rsa_clone(a, msg, err);
    rsa_scratch_free(&s);
    return out;
}

Slice rsa_decrypt_oaep(Alloc *a, Hash hash, IoReader random, const RsaPrivateKey *priv,
                       Slice ciphertext, Slice label, Error *err) {
    (void)random;
    Slice out = rsa_decrypt_oaep_hashes(a, hash, hash, priv, ciphertext, label, err);
    hash_reset(hash);
    return out;
}

/* ------------------------------------------------- PKCS #1 v1.5 (public) */

Slice rsa_sign_pkcs1_v15(Alloc *a, IoReader random, const RsaPrivateKey *priv,
                         CryptoHash hash, Slice hashed, Error *err) {
    (void)random;
    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);
    Str name = str_from_cstr("");
    if (hash != 0) {
        if (hashed.len != crypto_hash_size(hash)) {
            rsa_set_error(err, "crypto/rsa: input must be hashed message");
            goto done;
        }
        name = crypto_hash_string(hash, s.a);
    }
    if (rsa_fail(err, rsa_check_public_key_size(&priv->public_key)))
        goto done;

    const RsaFipsKey *k = rsa_fips_private_key(priv, s.a, err);
    Slice sig;
    if (k != NULL && rsa_fips_sign_pkcs1_v15(k, name, hashed, s.a, &sig, err))
        out = rsa_clone(a, sig, err);

done:
    rsa_scratch_free(&s);
    return out;
}

Error rsa_verify_pkcs1_v15(const RsaPublicKey *pub, CryptoHash hash, Slice hashed,
                           Slice sig) {
    RsaScratch s;
    rsa_scratch_init(&s);
    Error err = BURROW_NO_ERROR;
    Str name = str_from_cstr("");
    if (hash != 0) {
        if (hashed.len != crypto_hash_size(hash)) {
            err = rsa_error("crypto/rsa: input must be hashed message");
            goto done;
        }
        name = crypto_hash_string(hash, s.a);
    }
    err = rsa_check_public_key_size(pub);
    if (BURROW_FAILED(err))
        goto done;

    BigmodModulus *N = rsa_fips_public_key(pub, s.a, &err);
    if (N != NULL)
        err = rsa_fips_verify_pkcs1_v15(N, pub->e, name, hashed, sig, s.a);

done:
    rsa_scratch_free(&s);
    return err;
}

Slice rsa_encrypt_pkcs1_v15(Alloc *a, IoReader random, const RsaPublicKey *pub,
                            Slice msg, Error *err) {
    if (rsa_fail(err, rsa_check_public_key_size(pub)))
        return slice_nil(TYPE_BYTE);
    Int k = rsa_public_key_size(pub);
    if (msg.len > k - 11) {
        BURROW_OUT(err, rsa_err_message_too_long);
        return slice_nil(TYPE_BYTE);
    }

    random = rsa_reader(random);

    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);

    /* EM = 0x00 || 0x02 || PS || 0x00 || M */
    Slice em = rsa_make(s.a, k);
    uint8_t *p = rsa_p(em);
    p[1] = 2;
    Slice ps = rsa_sub(em, 2, k - msg.len - 1);
    if (rsa_fail(err, burrow__rsa_non_zero_random_bytes(ps, random)))
        goto done;
    p[k - msg.len - 1] = 0;
    if (msg.len > 0)
        memcpy(p + k - msg.len, msg.p, (size_t)msg.len);

    BigmodModulus *N = rsa_fips_public_key(pub, s.a, err);
    if (N == NULL)
        goto done;
    if (rsa_fail(err, rsa_check_public(N, pub->e)))
        goto done;
    Slice ct;
    if (rsa_fips_encrypt(N, pub->e, em, s.a, &ct, err))
        out = rsa_clone(a, ct, err);

done:
    rsa_scratch_free(&s);
    return out;
}

/* decryptPKCS1v15 decrypts ciphertext using priv. It returns one or zero in
 * *valid that indicates whether the plaintext was correctly structured. In
 * either case, the plaintext is returned in *em so that it may be read
 * independently of whether it was valid in order to maintain constant memory
 * access patterns. If the plaintext was valid then *index contains the index
 * of the original message in em, to allow constant time padding removal. */
static bool rsa_decrypt_pkcs1_v15_em(const RsaPrivateKey *priv, Slice ciphertext,
                                     Alloc *tmp, Int *valid, Slice *em, Int *index,
                                     Error *err) {
    Int k = rsa_public_key_size(&priv->public_key);
    if (k < 11) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }

    const RsaFipsKey *fk = rsa_fips_private_key(priv, tmp, err);
    if (fk == NULL)
        return false;
    if (!rsa_fips_decrypt(fk, ciphertext, false, tmp, em)) {
        BURROW_OUT(err, rsa_err_decryption);
        return false;
    }

    const uint8_t *p = rsa_p(*em);
    Int first_byte_is_zero = subtle_constant_time_byte_eq(p[0], 0);
    Int second_byte_is_two = subtle_constant_time_byte_eq(p[1], 2);

    /* The remainder of the plaintext must be a string of non-zero random
     * octets, followed by a 0, followed by the message.
     *   lookingForIndex: 1 iff we are still looking for the zero.
     *   index: the offset of the first zero byte. */
    Int looking_for_index = 1;
    Int idx = 0;
    for (Int i = 2; i < em->len; i++) {
        Int equals0 = subtle_constant_time_byte_eq(p[i], 0);
        idx = subtle_constant_time_select(looking_for_index & equals0, i, idx);
        looking_for_index = subtle_constant_time_select(equals0, 0, looking_for_index);
    }

    /* The PS padding must be at least 8 bytes long, and it starts two bytes
     * into em. */
    Int valid_ps = subtle_constant_time_less_or_eq(2 + 8, idx);

    *valid =
        first_byte_is_zero & second_byte_is_two & (~looking_for_index & 1) & valid_ps;
    *index = subtle_constant_time_select(*valid, idx + 1, 0);
    return true;
}

Slice rsa_decrypt_pkcs1_v15(Alloc *a, IoReader random, const RsaPrivateKey *priv,
                            Slice ciphertext, Error *err) {
    (void)random;
    if (rsa_fail(err, rsa_check_public_key_size(&priv->public_key)))
        return slice_nil(TYPE_BYTE);

    RsaScratch s;
    rsa_scratch_init(&s);
    Slice out = slice_nil(TYPE_BYTE);
    Int valid = 0, index = 0;
    Slice em;
    if (rsa_decrypt_pkcs1_v15_em(priv, ciphertext, s.a, &valid, &em, &index, err)) {
        if (valid == 0)
            BURROW_OUT(err, rsa_err_decryption);
        else
            out = rsa_clone(a, rsa_sub(em, index, em.len), err);
    }
    rsa_scratch_free(&s);
    return out;
}

Error rsa_decrypt_pkcs1_v15_session_key(IoReader random, const RsaPrivateKey *priv,
                                        Slice ciphertext, Slice key) {
    (void)random;
    Error err = rsa_check_public_key_size(&priv->public_key);
    if (BURROW_FAILED(err))
        return err;
    Int k = rsa_public_key_size(&priv->public_key);
    if (k - (key.len + 3 + 8) < 0)
        return rsa_err_decryption;

    RsaScratch s;
    rsa_scratch_init(&s);
    Int valid = 0, index = 0;
    Slice em;
    if (rsa_decrypt_pkcs1_v15_em(priv, ciphertext, s.a, &valid, &em, &index, &err)) {
        if (em.len != k) {
            /* This should be impossible because decryptPKCS1v15 always
             * returns the full slice. */
            err = rsa_err_decryption;
        } else {
            valid &=
                subtle_constant_time_eq((int32_t)(em.len - index), (int32_t)key.len);
            subtle_constant_time_copy(valid, key,
                                      rsa_sub(em, em.len - key.len, em.len));
        }
    }
    rsa_scratch_free(&s);
    return err;
}

/* ------------------------------------------------- Sign and Decrypt */

Slice rsa_private_key_sign(const RsaPrivateKey *priv, Alloc *a, IoReader rand,
                           Slice digest, CryptoSignerOpts opts, Error *err) {
    if (opts.vt == NULL)
        rsa_nil();
    if (opts.vt->self_type == TYPE_RSA_PSS_OPTIONS) {
        const RsaPSSOptions *pss = opts.data;
        if (pss == NULL)
            rsa_nil();
        return rsa_sign_pss(a, rand, priv, pss->hash, digest, pss, err);
    }
    return rsa_sign_pkcs1_v15(a, rand, priv, opts.vt->hash_func(opts.data), digest,
                              err);
}

Slice rsa_private_key_decrypt(const RsaPrivateKey *priv, Alloc *a, IoReader rand,
                              Slice ciphertext, CryptoDecrypterOpts opts, Error *err) {
    if (opts.t == NULL)
        return rsa_decrypt_pkcs1_v15(a, rand, priv, ciphertext, err);

    if (opts.t == TYPE_RSA_OAEP_OPTIONS) {
        const RsaOAEPOptions *o = opts.data;
        if (o == NULL)
            rsa_nil();
        if (!crypto_hash_available(o->hash)) {
            rsa_unavailable(err, "rsa: requested hash function unavailable: ", o->hash);
            return slice_nil(TYPE_BYTE);
        }
        if (o->mgf_hash != 0 && !crypto_hash_available(o->mgf_hash)) {
            rsa_unavailable(err,
                            "rsa: requested hash function unavailable: ", o->mgf_hash);
            return slice_nil(TYPE_BYTE);
        }
        RsaScratch s;
        rsa_scratch_init(&s);
        Hash h = crypto_hash_new(o->hash, s.a);
        Hash mgf = crypto_hash_new(o->mgf_hash == 0 ? o->hash : o->mgf_hash, s.a);
        Slice out = rsa_decrypt_oaep_hashes(a, h, mgf, priv, ciphertext, o->label, err);
        rsa_scratch_free(&s);
        return out;
    }

    if (opts.t == TYPE_RSA_PKCS1V15_DECRYPT_OPTIONS) {
        const RsaPKCS1v15DecryptOptions *o = opts.data;
        if (o == NULL)
            rsa_nil();
        Int l = o->session_key_len;
        if (l <= 0)
            return rsa_decrypt_pkcs1_v15(a, rand, priv, ciphertext, err);
        Slice plaintext = rsa_make(a, l);
        if (rand.vt == NULL)
            rsa_nil();
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, plaintext, &e);
        if (BURROW_FAILED(e) || BURROW_FAILED(e = rsa_decrypt_pkcs1_v15_session_key(
                                                  rand, priv, ciphertext, plaintext))) {
            rsa_slice_free(a, plaintext);
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
        return plaintext;
    }

    rsa_set_error(err, "crypto/rsa: invalid options for Decrypt");
    return slice_nil(TYPE_BYTE);
}

/* ---------------------------------------------------- Signer and Decrypter */

static CryptoPublicKey rsa_signer_public(void *self) {
    return rsa_private_key_public(self);
}

static Slice rsa_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                             CryptoSignerOpts opts, Error *err) {
    return rsa_private_key_sign(self, a, rand, digest, opts, err);
}

static const CryptoSignerVT rsa_signer_vt = {TYPE_RSA_PRIVATE_KEY, rsa_signer_public,
                                             rsa_signer_sign};

CryptoSigner rsa_private_key_signer(const RsaPrivateKey *priv) {
    return (CryptoSigner){&rsa_signer_vt, (void *)(uintptr_t)priv};
}

static Slice rsa_decrypter_decrypt(void *self, Alloc *a, IoReader rand, Slice msg,
                                   CryptoDecrypterOpts opts, Error *err) {
    return rsa_private_key_decrypt(self, a, rand, msg, opts, err);
}

static const CryptoDecrypterVT rsa_decrypter_vt = {
    TYPE_RSA_PRIVATE_KEY, rsa_signer_public, rsa_decrypter_decrypt};

CryptoDecrypter rsa_private_key_decrypter(const RsaPrivateKey *priv) {
    return (CryptoDecrypter){&rsa_decrypter_vt, (void *)(uintptr_t)priv};
}
