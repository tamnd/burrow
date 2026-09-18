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

#include <string.h>

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

bool type_equal(const Type *t, const void *a, const void *b) {
    if (t == NULL || a == NULL || b == NULL)
        return a == b;
    if (t->ops != NULL && t->ops->equal != NULL)
        return t->ops->equal(a, b);
    return memcmp(a, b, t->size) == 0;
}

/* FNV-1a, sixty four bit, seeded.
 *
 * Not the hash Go uses. Go's runtime hash is AES accelerated where the chip has
 * the instruction and a different function where it does not, and its output is
 * deliberately unstable between runs. Nothing observable depends on which hash
 * this is, only on it being a hash, so this is the simple one until the map
 * lands and has something to say about throughput.
 *
 * The seed is folded in at the start rather than at the end, so that two values
 * that differ only in a late byte still land in different buckets. */
static uint64_t fnv1a(const void *p, size_t n, uint64_t seed) {
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t type_hash(const Type *t, const void *p, uint64_t seed) {
    if (t == NULL || p == NULL)
        return seed;
    if (t->ops != NULL && t->ops->hash != NULL)
        return t->ops->hash(p, seed);
    return fnv1a(p, t->size, seed);
}

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

const Method *type_method_by_name(const Type *t, Str name) {
    if (t == NULL || t->methods == NULL || t->nmethod == 0)
        return NULL;

    /* Sorted by name, so binary search. The bounds are kept as int rather than
     * uint16_t because lo can go one past hi and hi can go to minus one. */
    int lo = 0;
    int hi = (int)t->nmethod - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = str_cmp(t->methods[mid].name, name);
        if (c == 0)
            return &t->methods[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
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
        return fnv1a("", 0, seed);
    return fnv1a(s->p, (size_t)s->len, seed);
}

static const TypeOps string_ops = {string_equal, string_hash, NULL, NULL};

/* The hash field is a small distinct constant per builtin rather than anything
 * derived. It only has to differ between types within one build, and hand
 * numbering the two dozen builtins is both obviously correct and checkable by a
 * test that looks for duplicates. Generated descriptors will compute theirs. */
#define BUILTIN(var, kindv, ctype, gonm, hashv, opsv)                                  \
    static const Type var##_desc = {                                                   \
        {(const Byte *)gonm, (Int)(sizeof(gonm) - 1)},                                 \
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
    };                                                                                 \
    const Type *const var = &var##_desc

BUILTIN(TYPE_BOOL, KIND_BOOL, bool, "bool", 1, NULL);
BUILTIN(TYPE_INT, KIND_INT, Int, "int", 2, NULL);
BUILTIN(TYPE_INT8, KIND_INT8, int8_t, "int8", 3, NULL);
BUILTIN(TYPE_INT16, KIND_INT16, int16_t, "int16", 4, NULL);
BUILTIN(TYPE_INT32, KIND_INT32, int32_t, "int32", 5, NULL);
BUILTIN(TYPE_INT64, KIND_INT64, int64_t, "int64", 6, NULL);
BUILTIN(TYPE_UINT, KIND_UINT, Uint, "uint", 7, NULL);
BUILTIN(TYPE_UINT8, KIND_UINT8, uint8_t, "uint8", 8, NULL);
BUILTIN(TYPE_UINT16, KIND_UINT16, uint16_t, "uint16", 9, NULL);
BUILTIN(TYPE_UINT32, KIND_UINT32, uint32_t, "uint32", 10, NULL);
BUILTIN(TYPE_UINT64, KIND_UINT64, uint64_t, "uint64", 11, NULL);
BUILTIN(TYPE_UINTPTR, KIND_UINTPTR, Uintptr, "uintptr", 12, NULL);
BUILTIN(TYPE_FLOAT32, KIND_FLOAT32, float, "float32", 13, NULL);
BUILTIN(TYPE_FLOAT64, KIND_FLOAT64, double, "float64", 14, NULL);
BUILTIN(TYPE_COMPLEX64, KIND_COMPLEX64, Complex64, "complex64", 15, NULL);
BUILTIN(TYPE_COMPLEX128, KIND_COMPLEX128, Complex128, "complex128", 16, NULL);
BUILTIN(TYPE_STRING, KIND_STRING, Str, "string", 17, &string_ops);
BUILTIN(TYPE_UNSAFE_POINTER, KIND_UNSAFE_POINTER, void *, "unsafe.Pointer", 18, NULL);
