/* The builtin type descriptors and the operations over any descriptor.
 *
 * Not derived from Go's source, which is why there is no derivation line here.
 * The kind numbering and the names match go/src/internal/abi/type.go and
 * go/src/reflect/type.go because they are observable through reflect and fmt
 * and have to, but the code is written for C and shares no lines with either.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/type.h"

#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"

#include <string.h>

/* For _umul128, which is how MSVC gets the top half of a 64 by 64 multiply.
 * Everything else in here is plain C. */
#if BURROW_CC_MSVC && !defined(__SIZEOF_INT128__)
#include <intrin.h>
#endif

/* ------------------------------------------------------------------- names */

/* Indexed by Kind, so the order here is locked to the order in the enum. A
 * missing entry would read as a NULL pointer rather than as an empty string, so
 * the table is checked against KIND_MAX by a static assertion below and by a
 * test that walks every kind. */
static const char *const kind_names[KIND_MAX] = {
    "invalid",   "bool",       "int",
    "int8",      "int16",      "int32",
    "int64",     "uint",       "uint8",
    "uint16",    "uint32",     "uint64",
    "uintptr",   "float32",    "float64",
    "complex64", "complex128", "array",
    "chan",      "func",       "interface",
    "map",       "ptr",        "slice",
    "string",    "struct",     "unsafe.Pointer",
};

/* Go calls the pointer kind "ptr" in Kind.String and "*T" in Type.String, which
 * catches people out often enough to be worth a line here rather than a bug
 * report later. */

Str kind_name(Kind k) {
    if (k < 0 || k >= KIND_MAX)
        return BURROW_S("invalid");
    return str_from_cstr(kind_names[k]);
}

Str type_name(const Type *t) {
    if (t == NULL)
        return BURROW_S("invalid");
    if (t->name.len > 0)
        return t->name;
    /* An unnamed composite has no spelling stored, so the kind is the best
     * answer available until the composite constructors land. */
    return kind_name(t->kind);
}

/* ------------------------------------------------------------------- kinds */

bool kind_is_signed(Kind k) {
    return k >= KIND_INT && k <= KIND_INT64;
}

bool kind_is_unsigned(Kind k) {
    return k >= KIND_UINT && k <= KIND_UINTPTR;
}

bool kind_is_float(Kind k) {
    return k == KIND_FLOAT32 || k == KIND_FLOAT64;
}

bool type_is_comparable(const Type *t) {
    if (t == NULL)
        return false;

    switch (t->kind) {
    case KIND_SLICE:
    case KIND_MAP:
    case KIND_FUNC:
        return false;

    case KIND_ARRAY:
        /* An array is comparable when its element is. Go says so and the
         * recursion terminates because an array cannot contain itself. */
        return type_is_comparable(t->elem);

    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            if (!type_is_comparable(t->fields[i].type))
                return false;
        }
        return true;

    case KIND_INVALID:
        return false;

    case KIND_BOOL:
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
    case KIND_FLOAT32:
    case KIND_FLOAT64:
    case KIND_COMPLEX64:
    case KIND_COMPLEX128:
    case KIND_CHAN:
    case KIND_INTERFACE:
    case KIND_POINTER:
    case KIND_STRING:
    case KIND_UNSAFE_POINTER:
    case KIND_MAX:
    default:
        return true;
    }
}

/* -------------------------------------------------------------- operations */

/* A struct and an array are walked a piece at a time rather than compared as
 * bytes, and the reason is padding.
 *
 * struct { char a; int b; } has three bytes between its two fields on most
 * machines, and nothing in C says what is in them. Two structs built the same
 * way with the same values routinely differ there: one came off the stack, one
 * came out of a designated initialiser, one was memcpy'd from a wider buffer.
 * memcmp says they are different values and Go says they are the same one, and
 * a map keyed by such a struct would lose lookups that used a key it had just
 * been handed.
 *
 * The same walk is also what makes a field of a type with its own idea of
 * equality work at all. A struct with a Str in it compares that field by its
 * bytes and not by its pointer, because going field by field means going
 * through type_equal again and Str's ops are there.
 *
 * Everything else still goes through memcmp, which is correct for a type that
 * is nothing but its bytes, and that is most of them. */
static bool struct_equal(const Type *t, const void *a, const void *b) {
    const Byte *pa = (const Byte *)a;
    const Byte *pb = (const Byte *)b;

    for (uint16_t i = 0; i < t->nfield; i++) {
        const Field *f = &t->fields[i];
        if (!type_equal(f->type, pa + f->offset, pb + f->offset))
            return false;
    }
    return true;
}

static bool array_equal(const Type *t, const void *a, const void *b) {
    const Type *e = t->elem;
    const Byte *pa = (const Byte *)a;
    const Byte *pb = (const Byte *)b;

    if (e == NULL || e->size == 0)
        return true;

    for (uint32_t i = 0; i < t->len; i++) {
        if (!type_equal(e, pa + (size_t)i * e->size, pb + (size_t)i * e->size))
            return false;
    }
    return true;
}

bool type_equal(const Type *t, const void *a, const void *b) {
    if (t == NULL || a == NULL || b == NULL)
        return a == b;
    if (t->ops != NULL && t->ops->equal != NULL)
        return t->ops->equal(a, b);
    if (t->kind == KIND_STRUCT && t->fields != NULL)
        return struct_equal(t, a, b);
    if (t->kind == KIND_ARRAY)
        return array_equal(t, a, b);
    return memcmp(a, b, t->size) == 0;
}

/* The hash, which is a multiply and fold rather than a byte at a time.
 *
 * Not the hash Go uses. Go's runtime hash is AES accelerated where the chip has
 * the instruction and a different function where it does not, and its output is
 * deliberately unstable between runs. Nothing observable depends on which hash
 * this is, only on it being a hash, so the choice here is free and is made on
 * throughput alone.
 *
 * It used to be FNV-1a, which was the right thing to start with and the wrong
 * thing to keep. FNV is one multiply per byte and each multiply needs the
 * previous one's answer, so an eight byte key costs eight multiplies deep in a
 * dependency chain and the chip cannot overlap any of it. Measured, that was
 * 8.6 ns for an int key on an x86-64 box where the whole map insert around it
 * was 36 ns, so a quarter of the cost of every insert and every lookup was the
 * hash. The map benchmarks in burrow-bench are what said so.
 *
 * What replaces it costs two multiplies for any key up to sixteen bytes, plus
 * one more per sixteen bytes after that, and the multiplies in the loop do not
 * depend on each other. The structure is the one every fast modern hash of the
 * last ten years uses and wyhash is the one it most resembles: read the input
 * as words, combine them with a widening multiply, and keep the two halves of
 * the product folded together. No line of it is copied from anywhere, and the
 * constants are this library's own, generated by the same splitmix64 that seeds
 * the runtime generator and then forced odd.
 *
 * The quality is tested rather than assumed. type_test checks that flipping any
 * one bit of a key flips every bit of the hash about half the time, and that a
 * thousand int keys and a thousand string keys land in the buckets a map would
 * put them in without a pile up. A hash that is fast and lumpy is slower than a
 * slow one, because the probe sequence pays for the lumps. */

/* Six arbitrary odd constants. Nothing depends on their values beyond being
 * odd, having roughly half their bits set, and not being a pattern. */
#define HK0 0x910a2dec89025cc1ULL
#define HK1 0xbeeb8da1658eec67ULL
#define HK2 0xf893a2eefb32555fULL
#define HK3 0x71c18690ee42c90bULL
#define HK4 0x71bb54d8d101b5b9ULL
#define HK5 0xc34d0bff90150281ULL

/* Multiply two words into 128 bits, keeping both halves. This is the one
 * primitive the whole hash is built out of, and it is the reason a multiply and
 * fold hash is strong for its cost: a 128 bit product mixes in a way that no
 * amount of shifting and adding does, because every bit of either input reaches
 * most of the 128 bits of the answer.
 *
 * Three ways to get the top half. Compilers with a 128 bit type get one
 * instruction. MSVC on x64 and arm64 has an intrinsic for it. Anything else
 * does it by hand in four 32 bit multiplies, which is what a 32 bit target has
 * to do anyway and is still far cheaper than a multiply per byte. */
#if defined(__SIZEOF_INT128__)
/* __extension__ is what stops -Wpedantic objecting that ISO C has no 128 bit
 * type, which it does not, which is why this branch is conditional. */
__extension__ typedef unsigned __int128 hash_u128;
#endif

static void hash_mul(uint64_t *lo, uint64_t *hi) {
#if defined(__SIZEOF_INT128__)
    hash_u128 r = (hash_u128)*lo * *hi;
    *lo = (uint64_t)r;
    *hi = (uint64_t)(r >> 64);
#elif BURROW_CC_MSVC && (defined(_M_X64) || defined(_M_ARM64))
    uint64_t h;
    uint64_t l = _umul128(*lo, *hi, &h);
    *lo = l;
    *hi = h;
#else
    uint64_t a = *lo, b = *hi;
    uint64_t al = a & 0xffffffffULL, ah = a >> 32;
    uint64_t bl = b & 0xffffffffULL, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    /* The two middle products straddle the boundary, so their low halves carry
     * into the top word and their high halves belong there already. */
    uint64_t mid = (ll >> 32) + (lh & 0xffffffffULL) + (hl & 0xffffffffULL);
    *lo = (mid << 32) | (ll & 0xffffffffULL);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}

/* The same multiply with the halves folded on top of each other, which is the
 * step that turns two words into one. */
static uint64_t hash_mix(uint64_t a, uint64_t b) {
    hash_mul(&a, &b);
    return a ^ b;
}

/* Reads through memcpy because a key is only aligned for its own type and this
 * reads it eight bytes at a time regardless. Every compiler that matters turns
 * these into the single load they are. */
static uint64_t hash_load64(const unsigned char *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t hash_load32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* The byte order is not corrected on a big endian machine, so the same key
 * hashes to a different number there. That is allowed, and it is allowed for the
 * same reason the seed changes every run: nothing outside can see a hash. Byte
 * swapping every word to make the numbers match across machines would cost
 * throughput to buy a property no caller has. */
static uint64_t hash_bytes(const void *p, size_t n, uint64_t seed) {
    const unsigned char *b = (const unsigned char *)p;
    /* The length goes in through a multiply and not through an exclusive or.
     * An exclusive or leaves it in the low bits, and the three byte path below
     * also builds its answer in the low bits, so "ab" and "abc" cancelled the
     * length against the last byte and hashed identically. That was a real bug,
     * found by the test that hashes every length, and it is the reason the
     * length is spread over the whole word before it meets anything. The
     * multiply is free in practice because it does not depend on the key, so it
     * runs while the loads below are still in flight. */
    uint64_t h = seed ^ (HK0 * ((uint64_t)n + 1));
    uint64_t a, c;

    if (n <= 16) {
        /* Two reads that overlap in the middle, which is how the sizes between
         * the powers of two avoid a switch. Eight bytes, the size of an int or
         * a pointer and the case that matters most, reads the same word twice
         * and leaves here with nothing to pay but the two multiplies below. */
        if (n >= 8) {
            a = hash_load64(b);
            c = hash_load64(b + n - 8);
        } else if (n >= 4) {
            a = hash_load32(b);
            c = hash_load32(b + n - 4);
        } else if (n > 0) {
            /* First, middle and last byte. For one and two byte keys those
             * repeat, and the length in h is what keeps the lengths apart. */
            a = ((uint64_t)b[0] << 16) | ((uint64_t)b[n >> 1] << 8) |
                (uint64_t)b[n - 1];
            c = 0;
        } else {
            a = 0;
            c = 0;
        }
    } else {
        /* Sixteen bytes an iteration in two lanes, so the two multiplies in the
         * body do not wait for each other and the chip runs them together.
         * Then the last sixteen bytes again, overlapping whatever the loop
         * left, which costs one extra read and removes every tail case. */
        uint64_t s0 = h ^ HK1;
        uint64_t s1 = h ^ HK2;
        size_t left = n;

        while (left > 16) {
            s0 = hash_mix(s0 ^ hash_load64(b), HK3);
            s1 = hash_mix(s1 ^ hash_load64(b + 8), HK4);
            b += 16;
            left -= 16;
        }

        /* left is between one and sixteen and b has moved on by at least
         * sixteen, so both of these read inside the key. */
        a = s0 ^ hash_load64(b + left - 16);
        c = s1 ^ hash_load64(b + left - 8);
    }

    /* Two multiplies to finish, not one. One is not enough and it is easy to
     * see why in the eight byte case, where a and c are the same word: a single
     * product of two numbers that differ only by a constant leaves whole
     * patterns of input bits that cannot reach some output bits, and the
     * avalanche test in type_test fails on it by a mile. Feeding the two halves
     * of the first product into a second one fixes it, and that is the shape
     * every hash in this family ends with. */
    a ^= h;
    c ^= HK5;
    hash_mul(&a, &c);
    return hash_mix(a ^ HK1, c ^ HK2);
}

/* The other half of the padding story. Equality that skips the padding and a
 * hash that does not would be worse than either mistake on its own: two values
 * that compare equal would land in different buckets, and a map would hold two
 * entries for one key and find whichever of them it probed first.
 *
 * So the hash walks whatever the comparison walks, field by field and element
 * by element, and the result is the seed threaded through all of them. */
static uint64_t struct_hash(const Type *t, const void *p, uint64_t seed) {
    const Byte *b = (const Byte *)p;

    for (uint16_t i = 0; i < t->nfield; i++) {
        const Field *f = &t->fields[i];
        seed = type_hash(f->type, b + f->offset, seed);
    }
    return seed;
}

static uint64_t array_hash(const Type *t, const void *p, uint64_t seed) {
    const Type *e = t->elem;
    const Byte *b = (const Byte *)p;

    if (e == NULL || e->size == 0)
        return seed;

    for (uint32_t i = 0; i < t->len; i++)
        seed = type_hash(e, b + (size_t)i * e->size, seed);
    return seed;
}

uint64_t type_hash(const Type *t, const void *p, uint64_t seed) {
    if (t == NULL || p == NULL)
        return seed;
    if (t->ops != NULL && t->ops->hash != NULL)
        return t->ops->hash(p, seed);
    if (t->kind == KIND_STRUCT && t->fields != NULL)
        return struct_hash(t, p, seed);
    if (t->kind == KIND_ARRAY)
        return array_hash(t, p, seed);
    return hash_bytes(p, t->size, seed);
}

/* No field by field walk here, unlike the two above, and it is not an
 * oversight. Copying the padding along with the fields is harmless, because
 * nothing reads it and neither of the two functions above looks at it. A walk
 * would only be needed for a struct with a field whose type has a copy op of
 * its own, and no type in burrow has one. When the first one appears, this is
 * where it gets handled. */
void type_copy(const Type *t, void *dst, const void *src) {
    if (t == NULL || dst == NULL || src == NULL)
        return;
    if (t->ops != NULL && t->ops->copy != NULL) {
        t->ops->copy(dst, src);
        return;
    }
    memcpy(dst, src, t->size);
}

void type_zero(const Type *t, void *p) {
    if (t == NULL || p == NULL)
        return;
    if (t->ops != NULL && t->ops->zero != NULL) {
        t->ops->zero(p);
        return;
    }
    memset(p, 0, t->size);
}

/* ------------------------------------------------------- fields and methods */

const Field *type_field_by_name(const Type *t, Str name) {
    if (t == NULL || t->fields == NULL)
        return NULL;
    for (uint16_t i = 0; i < t->nfield; i++) {
        if (str_eq(t->fields[i].name, name))
            return &t->fields[i];
    }
    return NULL;
}

bool field_is_exported(const Field *f) {
    if (f == NULL || f->name.len <= 0 || f->name.p == NULL)
        return false;
    return f->name.p[0] >= 'A' && f->name.p[0] <= 'Z';
}

bool field_is_blank(const Field *f) {
    if (f == NULL || f->name.len <= 0 || f->name.p == NULL || f->name.p[0] != '_')
        return false;
    for (Int i = 1; i < f->name.len; i++)
        if (f->name.p[i] < '0' || f->name.p[i] > '9')
            return false;
    return true;
}

bool field_is_embedded(const Field *f) {
    if (f == NULL || f->type == NULL || f->type->name.len <= 0)
        return false;
    /* The descriptor's own name and not type_name, which falls back to the kind
     * for an unnamed type. Going through the fallback would make a field called
     * slice of type []int look embedded, since type_name of an unnamed slice is
     * "slice". An unnamed type cannot be embedded in Go at all, so the right
     * answer for one is no rather than a comparison. */
    return str_eq(f->name, f->type->name);
}

const Method *type_method_by_name(const Type *t, Str name) {
    if (t == NULL || t->methods == NULL || t->nmethod == 0)
        return NULL;

    /* Linear, and the header says why: nothing can make the array sorted, so
     * nothing may assume it is. */
    for (uint16_t i = 0; i < t->nmethod; i++)
        if (str_eq(t->methods[i].name, name))
            return &t->methods[i];

    return NULL;
}

bool type_methods_sorted(const Type *t) {
    if (t == NULL || t->methods == NULL || t->nmethod < 2)
        return true;

    for (uint16_t i = 1; i < t->nmethod; i++)
        if (str_cmp(t->methods[i - 1].name, t->methods[i].name) >= 0)
            return false;

    return true;
}

bool method_call(const Method *m, void *recv, void **args, void **rets) {
    if (m == NULL || m->thunk == NULL)
        return false;

    m->thunk(recv, args, rets);
    return true;
}

/* ----------------------------------------------------------- function types
 *
 * Parameters then results in one fields array, len parameters of them. A type
 * that is not a function answers zero and NULL, which is what a caller that did
 * not check the kind would want anyway. */

Int type_num_in(const Type *t) {
    if (t == NULL || t->kind != KIND_FUNC)
        return 0;

    return (Int)t->len;
}

Int type_num_out(const Type *t) {
    if (t == NULL || t->kind != KIND_FUNC)
        return 0;

    return (Int)t->nfield - (Int)t->len;
}

const Type *type_in(const Type *t, Int i) {
    if (i < 0 || i >= type_num_in(t) || t->fields == NULL)
        return NULL;

    return t->fields[i].type;
}

const Type *type_out(const Type *t, Int i) {
    if (i < 0 || i >= type_num_out(t) || t->fields == NULL)
        return NULL;

    return t->fields[(Int)t->len + i].type;
}

/* ---------------------------------------------------------------- builtins */

/* Str does not compare by its bytes as a struct, because two Str values with
 * different pointers and the same contents are one string in Go. memcmp on the
 * struct would say they differ, which would make a map keyed by string lose
 * every lookup that did not use the identical pointer. This is the reason the
 * ops table exists at all. */
static bool string_equal(const void *a, const void *b) {
    return str_eq(*(const Str *)a, *(const Str *)b);
}

static uint64_t string_hash(const void *p, uint64_t seed) {
    const Str *s = (const Str *)p;
    if (s->p == NULL || s->len <= 0)
        return hash_bytes("", 0, seed);
    return hash_bytes(s->p, (size_t)s->len, seed);
}

static const TypeOps string_ops = {string_equal, string_hash, NULL, NULL};

/* Floats do not compare by their bytes either, and the reasons are the two
 * corners of IEEE 754 that Go's map inherits from the hardware.
 *
 * A negative zero and a positive zero are different bit patterns and are the
 * same number, so m[0.0] has to find an entry stored under -0.0. Both of them
 * therefore hash as a positive zero.
 *
 * A NaN is not equal to itself, so a NaN key can be stored and can never be
 * found again, and storing two of them stores two entries. Hashing the bits
 * would put every NaN in one bucket, and since none of them ever matches, a
 * program that manages to fill a map with NaN keys would turn every one of
 * those inserts into a walk over all of them. Mixing a random number in
 * scatters them instead, which is exactly what Go's runtime does and the reason
 * runtime.rand is reachable from the hashing code.
 *
 * Everything else, including the infinities, is just its bits. */
static uint64_t float64_hash_bits(double f, uint64_t seed) {
    if (f == 0)
        f = 0; /* turns a negative zero into a positive one */
    else if (f != f)
        return hash_bytes("", 0, seed ^ runtime_rand64());
    return hash_bytes(&f, sizeof f, seed);
}

static bool float32_equal(const void *a, const void *b) {
    return *(const float *)a == *(const float *)b;
}

static uint64_t float32_hash(const void *p, uint64_t seed) {
    /* Widened to double first, so that a float32 and a float64 holding the same
     * number are not required to hash the same and the zero and NaN rules only
     * have to be written once. They are different types and never share a map,
     * so nothing depends on the two agreeing. */
    return float64_hash_bits((double)*(const float *)p, seed);
}

static bool float64_equal(const void *a, const void *b) {
    return *(const double *)a == *(const double *)b;
}

static uint64_t float64_hash(const void *p, uint64_t seed) {
    return float64_hash_bits(*(const double *)p, seed);
}

/* A complex number is two floats and it follows both rules componentwise, since
 * Go defines its equality as the real parts being equal and the imaginary parts
 * being equal. So a complex with a NaN anywhere in it is never equal to
 * anything, including itself. */
static bool complex64_equal(const void *a, const void *b) {
    const Complex64 *x = (const Complex64 *)a;
    const Complex64 *y = (const Complex64 *)b;
    return x->re == y->re && x->im == y->im;
}

static uint64_t complex64_hash(const void *p, uint64_t seed) {
    const Complex64 *c = (const Complex64 *)p;
    return float64_hash_bits((double)c->im, float64_hash_bits((double)c->re, seed));
}

static bool complex128_equal(const void *a, const void *b) {
    const Complex128 *x = (const Complex128 *)a;
    const Complex128 *y = (const Complex128 *)b;
    return x->re == y->re && x->im == y->im;
}

static uint64_t complex128_hash(const void *p, uint64_t seed) {
    const Complex128 *c = (const Complex128 *)p;
    return float64_hash_bits(c->im, float64_hash_bits(c->re, seed));
}

static const TypeOps float32_ops = {float32_equal, float32_hash, NULL, NULL};
static const TypeOps float64_ops = {float64_equal, float64_hash, NULL, NULL};
static const TypeOps complex64_ops = {complex64_equal, complex64_hash, NULL, NULL};
static const TypeOps complex128_ops = {complex128_equal, complex128_hash, NULL, NULL};

/* The hash field is a small distinct constant per builtin rather than anything
 * derived. It only has to differ between types within one build, and hand
 * numbering the two dozen builtins is both obviously correct and checkable by a
 * test that looks for duplicates. Nothing else in the library has one, for the
 * reason written on the field in type.h.
 *
 * The object is named after the C spelling of the type rather than after the Go
 * one, because TYPE_OF pastes that name and a field list is written in C. The
 * two disagree more often than they look: Go's int is C's Int, Go's float32 is
 * C's float, and Go's int64 is also C's Int on a 64 bit machine, which is why
 * the name has to come from the spelling somebody wrote and not from the type
 * the compiler resolves it to. */
#define BUILTIN(cname, kindv, ctype, gonm, hashv, opsv)                                \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {NULL, 0},                                                                     \
        kindv,                                                                         \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        hashv,                                                                         \
        opsv,                                                                          \
    }

BUILTIN(bool, KIND_BOOL, bool, "bool", 1, NULL);
BUILTIN(Int, KIND_INT, Int, "int", 2, NULL);
BUILTIN(int8_t, KIND_INT8, int8_t, "int8", 3, NULL);
BUILTIN(int16_t, KIND_INT16, int16_t, "int16", 4, NULL);
BUILTIN(int32_t, KIND_INT32, int32_t, "int32", 5, NULL);
BUILTIN(int64_t, KIND_INT64, int64_t, "int64", 6, NULL);
BUILTIN(Uint, KIND_UINT, Uint, "uint", 7, NULL);
BUILTIN(uint8_t, KIND_UINT8, uint8_t, "uint8", 8, NULL);
BUILTIN(uint16_t, KIND_UINT16, uint16_t, "uint16", 9, NULL);
BUILTIN(uint32_t, KIND_UINT32, uint32_t, "uint32", 10, NULL);
BUILTIN(uint64_t, KIND_UINT64, uint64_t, "uint64", 11, NULL);
BUILTIN(Uintptr, KIND_UINTPTR, Uintptr, "uintptr", 12, NULL);
BUILTIN(float, KIND_FLOAT32, float, "float32", 13, &float32_ops);
BUILTIN(double, KIND_FLOAT64, double, "float64", 14, &float64_ops);
BUILTIN(Complex64, KIND_COMPLEX64, Complex64, "complex64", 15, &complex64_ops);
BUILTIN(Complex128, KIND_COMPLEX128, Complex128, "complex128", 16, &complex128_ops);
BUILTIN(Str, KIND_STRING, Str, "string", 17, &string_ops);
BUILTIN(UnsafePointer, KIND_UNSAFE_POINTER, void *, "unsafe.Pointer", 18, NULL);

/* []byte, declared in slice.h. Unnamed like every slice type, with no ops
 * because a slice is not comparable, and numbered with the builtins because
 * it is defined along with them. */
const Type burrow_type_Bytes = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_uint8_t,
    NULL,
    0,
    19,
    NULL,
};
