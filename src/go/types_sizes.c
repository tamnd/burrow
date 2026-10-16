/* go/types: the sizes of types, as package unsafe reports them.
 *
 * StdSizes and Go's unexported gcSizes are the same two numbers and the same
 * Alignof and Offsetsof. They differ in Sizeof, where gc pads an array element
 * and the end of a struct to the alignment, and gives the last field of a
 * struct that is not empty a byte when it has no size of its own. Both are one
 * set of functions here, with a flag for which of the two is asking.
 *
 * Derived from Go's src/go/types/sizes.go, gcsizes.go and gccgosizes.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Go's gcSizes, which is a struct of its own in Go only so that it has methods
 * of its own. */
typedef struct GtGcSizes {
    TypesStdSizes s;
} GtGcSizes;

static int64_t sz_alignof(const TypesStdSizes *s, bool gc, TypesType T);
static int64_t sz_sizeof(const TypesStdSizes *s, bool gc, TypesType T);

/* _IsSyncAtomicAlign64: T is sync/atomic's align64, the empty struct that
 * asks for 64-bit alignment of the struct it is in. */
static bool sz_is_sync_atomic_align64(TypesType T) {
    TypesNamed *named = burrow__types_as_named(T);
    if (named == NULL)
        return false;
    TypesObject obj = (TypesObject)types_named_obj(named);
    TypesPackage *pkg = types_object_pkg(obj);
    if (!str_eq(types_object_name(obj), BURROW_S("align64")) || pkg == NULL)
        return false;
    Str path = types_package_path(pkg);
    return str_eq(path, BURROW_S("sync/atomic")) ||
           str_eq(path, BURROW_S("internal/runtime/atomic"));
}

static int64_t sz_alignof_of(const TypesStdSizes *s, bool gc, TypesType T) {
    /* For arrays and structs, alignment is defined in terms of the alignment
     * of the elements and fields. */
    TypesType u = types_type_underlying(T);
    switch ((TypesTypeKind)u->kind) {
    case TYPES_KIND_ARRAY:
        /* spec: "For a variable x of array type: unsafe.Alignof(x) is the
         * same as unsafe.Alignof(x[0]), but at least 1." */
        return sz_alignof(s, gc, GT_KNOWN(TypesArray, u)->elem);
    case TYPES_KIND_STRUCT: {
        TypesStruct *t = GT_KNOWN(TypesStruct, u);
        if (t->fields.len == 0 && sz_is_sync_atomic_align64(T))
            return 8;
        /* spec: "For a variable x of struct type: unsafe.Alignof(x) is the
         * largest of the values unsafe.Alignof(x.f) for each field f of x,
         * but at least 1." */
        int64_t max = 1;
        for (Int i = 0; i < t->fields.len; i++) {
            int64_t a = sz_alignof(s, gc, GT_OBJ(GT_AT(t->fields, i, TypesVar *))->typ);
            if (a > max)
                max = a;
        }
        return max;
    }
    case TYPES_KIND_SLICE:
    case TYPES_KIND_INTERFACE:
        /* Multiword data structures are effectively structs in which each
         * element has size WordSize. A type parameter has no one size. */
        burrow__types_assert(!burrow__types_is_type_param(T));
        return s->word_size;
    case TYPES_KIND_BASIC:
        /* Strings are like slices and interfaces. */
        if ((GT_KNOWN(TypesBasic, u)->info & TYPES_IS_STRING) != 0)
            return s->word_size;
        break;
    case TYPES_KIND_TYPE_PARAM:
    case TYPES_KIND_UNION:
        panic_str(BURROW_S("unreachable"));
    case TYPES_KIND_INVALID:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_TUPLE:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
    case TYPES_KIND_NAMED:
    case TYPES_KIND_ALIAS:
    default:
        break;
    }
    int64_t a = sz_sizeof(s, gc, T); /* may be 0 or negative */
    /* spec: "For a variable x of any type: unsafe.Alignof(x) is at least 1." */
    if (a < 1)
        return 1;
    /* complex64 and complex128 are aligned like [2]float32 and [2]float64. */
    if (burrow__types_is_basic(T, TYPES_IS_COMPLEX))
        a /= 2;
    if (a > s->max_align)
        return s->max_align;
    return a;
}

/* Alignof, with Go's deferred check that the result is at least 1, which a
 * MaxAlign below 1 fails. */
static int64_t sz_alignof(const TypesStdSizes *s, bool gc, TypesType T) {
    int64_t result = sz_alignof_of(s, gc, T);
    burrow__types_assert(result >= 1);
    return result;
}

/* align: the smallest y >= x that a divides. a is a power of 2 from 1 to 8,
 * and the result may be negative when it overflows. */
static int64_t sz_align(int64_t x, int64_t a) {
    burrow__types_assert(x >= 0 && 1 <= a && a <= 8 && (a & (a - 1)) == 0);
    /* Go's int64 arithmetic wraps, so this is done unsigned. */
    return (int64_t)(((uint64_t)x + (uint64_t)a - 1) & ~((uint64_t)a - 1));
}

/* Offsetsof into out, which may be NULL, and the offset of the last field.
 * Sizeof only needs the last one, so it asks for no slice. */
static int64_t sz_offsets(const TypesStdSizes *s, bool gc, Slice fields, int64_t *out) {
    int64_t offs = 0;
    int64_t last = 0;
    for (Int i = 0; i < fields.len; i++) {
        if (offs < 0) {
            /* all remaining offsets are too large */
            last = -1;
            if (out != NULL)
                out[i] = -1;
            continue;
        }
        TypesType typ = GT_OBJ(GT_AT(fields, i, TypesVar *))->typ;
        offs = sz_align(offs, sz_alignof(s, gc, typ)); /* < 0 if it overflows */
        last = offs;
        if (out != NULL)
            out[i] = offs;
        int64_t d = sz_sizeof(s, gc, typ);
        if (d >= 0 && offs >= 0)
            offs = (int64_t)((uint64_t)offs + (uint64_t)d); /* ok to overflow */
        else
            offs = -1; /* the field's type or offs is too large */
    }
    return last;
}

static Slice sz_offsetsof(const TypesStdSizes *s, bool gc, Alloc *a, Slice fields) {
    Slice offsets = slice_make(a, TYPE_INT64, fields.len, fields.len);
    if (offsets.p == NULL && fields.len > 0)
        burrow__types_oom();
    (void)sz_offsets(s, gc, fields, (int64_t *)offsets.p);
    return offsets;
}

/* basicSizes, by kind. The kinds with no entry are a word or two. */
static const uint8_t sz_basic_sizes[] = {
    [TYPES_BOOL] = 1,        [TYPES_INT8] = 1,    [TYPES_INT16] = 2,
    [TYPES_INT32] = 4,       [TYPES_INT64] = 8,   [TYPES_UINT8] = 1,
    [TYPES_UINT16] = 2,      [TYPES_UINT32] = 4,  [TYPES_UINT64] = 8,
    [TYPES_FLOAT32] = 4,     [TYPES_FLOAT64] = 8, [TYPES_COMPLEX64] = 8,
    [TYPES_COMPLEX128] = 16,
};

#define SZ_MAX_INT64 INT64_MAX

static int64_t sz_array_size(const TypesStdSizes *s, bool gc, TypesArray *t) {
    int64_t n = t->len;
    if (n <= 0)
        return 0;
    int64_t esize = sz_sizeof(s, gc, t->elem);
    if (esize < 0)
        return -1; /* element too large */
    if (esize == 0)
        return 0; /* 0-size element */
    if (gc) {
        /* The size is esize*n, which has to fit in an int64. */
        if (esize > SZ_MAX_INT64 / n)
            return -1;
        return esize * n;
    }
    int64_t ea = sz_align(esize, sz_alignof(s, gc, t->elem)); /* < 0 on overflow */
    if (ea < 0)
        return -1;
    /* The size is ea*n1 + esize, and has to fit in an int64. */
    int64_t n1 = n - 1;
    if (n1 > 0 && ea > SZ_MAX_INT64 / n1)
        return -1;
    return (int64_t)((uint64_t)(ea * n1) + (uint64_t)esize); /* may overflow */
}

static int64_t sz_struct_size(const TypesStdSizes *s, bool gc, TypesType T,
                              TypesStruct *t) {
    Int n = t->fields.len;
    if (n == 0)
        return 0;
    int64_t offs = sz_offsets(s, gc, t->fields, NULL);
    int64_t size = sz_sizeof(s, gc, GT_OBJ(GT_AT(t->fields, n - 1, TypesVar *))->typ);
    if (offs < 0 || size < 0)
        return -1; /* type too large */
    if (!gc)
        return (int64_t)((uint64_t)offs + (uint64_t)size); /* may overflow */
    /* gc: The last field of a non-zero-sized struct is not allowed to have
     * size 0. */
    if (offs > 0 && size == 0)
        size = 1;
    /* gc: Size includes alignment padding. */
    return sz_align((int64_t)((uint64_t)offs + (uint64_t)size), sz_alignof(s, gc, T));
}

static int64_t sz_sizeof(const TypesStdSizes *s, bool gc, TypesType T) {
    TypesType u = types_type_underlying(T);
    switch ((TypesTypeKind)u->kind) {
    case TYPES_KIND_BASIC: {
        burrow__types_assert(burrow__types_is_typed(T));
        TypesBasicKind k = GT_KNOWN(TypesBasic, u)->kind;
        if (k >= 0 &&
            k < (TypesBasicKind)(sizeof sz_basic_sizes / sizeof sz_basic_sizes[0]) &&
            sz_basic_sizes[k] > 0)
            return sz_basic_sizes[k];
        if (k == TYPES_STRING)
            return s->word_size * 2;
        break;
    }
    case TYPES_KIND_ARRAY:
        return sz_array_size(s, gc, GT_KNOWN(TypesArray, u));
    case TYPES_KIND_SLICE:
        return s->word_size * 3;
    case TYPES_KIND_STRUCT:
        return sz_struct_size(s, gc, T, GT_KNOWN(TypesStruct, u));
    case TYPES_KIND_INTERFACE:
        /* A type parameter has no one size. */
        burrow__types_assert(!burrow__types_is_type_param(T));
        return s->word_size * 2;
    case TYPES_KIND_TYPE_PARAM:
    case TYPES_KIND_UNION:
        panic_str(BURROW_S("unreachable"));
    case TYPES_KIND_INVALID:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_TUPLE:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
    case TYPES_KIND_NAMED:
    case TYPES_KIND_ALIAS:
    default:
        break;
    }
    return s->word_size; /* catch-all */
}

/* ---------------------------------------------------------------- StdSizes */

int64_t types_std_sizes_alignof(TypesStdSizes *s, TypesType T) {
    return sz_alignof(s, false, T);
}

Slice types_std_sizes_offsetsof(TypesStdSizes *s, Alloc *a, Slice fields) {
    return sz_offsetsof(s, false, a, fields);
}

int64_t types_std_sizes_sizeof(TypesStdSizes *s, TypesType T) {
    return sz_sizeof(s, false, T);
}

static int64_t sz_std_alignof(void *self, TypesType T) {
    return sz_alignof((const TypesStdSizes *)self, false, T);
}

static Slice sz_std_offsetsof(void *self, Alloc *a, Slice fields) {
    return sz_offsetsof((const TypesStdSizes *)self, false, a, fields);
}

static int64_t sz_std_sizeof(void *self, TypesType T) {
    return sz_sizeof((const TypesStdSizes *)self, false, T);
}

static int64_t sz_gc_alignof(void *self, TypesType T) {
    return sz_alignof(&((const GtGcSizes *)self)->s, true, T);
}

static Slice sz_gc_offsetsof(void *self, Alloc *a, Slice fields) {
    return sz_offsetsof(&((const GtGcSizes *)self)->s, true, a, fields);
}

static int64_t sz_gc_sizeof(void *self, TypesType T) {
    return sz_sizeof(&((const GtGcSizes *)self)->s, true, T);
}

#define SZ_TYPE(var, gonm, ctype, hash)                                                \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"go/types", 8},                                                 \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (hash),                                                                        \
        NULL,                                                                          \
    }

SZ_TYPE(sz_std_desc, "StdSizes", TypesStdSizes, 0x67747373U /* "gtss" */);
SZ_TYPE(sz_gc_desc, "gcSizes", GtGcSizes, 0x67746773U /* "gtgs" */);

static const TypesSizesVT sz_std_vt = {
    &sz_std_desc,
    sz_std_alignof,
    sz_std_offsetsof,
    sz_std_sizeof,
};

static const TypesSizesVT sz_gc_vt = {
    &sz_gc_desc,
    sz_gc_alignof,
    sz_gc_offsetsof,
    sz_gc_sizeof,
};

TypesSizes types_std_sizes_as_sizes(TypesStdSizes *s) {
    TypesSizes sz = {&sz_std_vt, s};
    return sz;
}

/* ------------------------------------------------------------------ Sizes */

int64_t types_sizes_alignof(TypesSizes s, TypesType T) {
    return s.vt->alignof_(s.data, T);
}

Slice types_sizes_offsetsof(TypesSizes s, Alloc *a, Slice fields) {
    return s.vt->offsetsof(s.data, a, fields);
}

int64_t types_sizes_sizeof(TypesSizes s, TypesType T) {
    return s.vt->sizeof_(s.data, T);
}

/* gcArchSizes: the word sizes and alignments of gc's architectures. */
static const struct {
    const char *arch;
    GtGcSizes sizes;
} sz_gc_arch[] = {
    {"386", {{4, 4}}},      {"amd64", {{8, 8}}},  {"amd64p32", {{4, 8}}},
    {"arm", {{4, 4}}},      {"arm64", {{8, 8}}},  {"loong64", {{8, 8}}},
    {"mips", {{4, 4}}},     {"mipsle", {{4, 4}}}, {"mips64", {{8, 8}}},
    {"mips64le", {{8, 8}}}, {"ppc64", {{8, 8}}},  {"ppc64le", {{8, 8}}},
    {"riscv64", {{8, 8}}},  {"s390x", {{8, 8}}},  {"sparc64", {{8, 8}}},
    {"wasm", {{8, 8}}},
};

/* gccgoArchSizes, the copy of the table gccgo's build makes. */
static const struct {
    const char *arch;
    TypesStdSizes sizes;
} sz_gccgo_arch[] = {
    {"386", {4, 4}},       {"alpha", {8, 8}},       {"amd64", {8, 8}},
    {"amd64p32", {4, 8}},  {"arm", {4, 8}},         {"armbe", {4, 8}},
    {"arm64", {8, 8}},     {"arm64be", {8, 8}},     {"ia64", {8, 8}},
    {"loong64", {8, 8}},   {"m68k", {4, 2}},        {"mips", {4, 8}},
    {"mipsle", {4, 8}},    {"mips64", {8, 8}},      {"mips64le", {8, 8}},
    {"mips64p32", {4, 8}}, {"mips64p32le", {4, 8}}, {"nios2", {4, 8}},
    {"ppc", {4, 8}},       {"ppc64", {8, 8}},       {"ppc64le", {8, 8}},
    {"riscv", {4, 8}},     {"riscv64", {8, 8}},     {"s390", {4, 8}},
    {"s390x", {8, 8}},     {"sh", {4, 8}},          {"shbe", {4, 8}},
    {"sparc", {4, 8}},     {"sparc64", {8, 8}},     {"wasm", {8, 8}},
};

TypesSizes types_sizes_for(Str compiler, Str arch) {
    TypesSizes none = {NULL, NULL};
    if (str_eq(compiler, BURROW_S("gc"))) {
        for (size_t i = 0; i < sizeof sz_gc_arch / sizeof sz_gc_arch[0]; i++) {
            if (str_eq(arch, str_from_cstr(sz_gc_arch[i].arch))) {
                /* The methods only read it. */
                TypesSizes sz = {&sz_gc_vt, (void *)(uintptr_t)&sz_gc_arch[i].sizes};
                return sz;
            }
        }
    } else if (str_eq(compiler, BURROW_S("gccgo"))) {
        for (size_t i = 0; i < sizeof sz_gccgo_arch / sizeof sz_gccgo_arch[0]; i++) {
            if (str_eq(arch, str_from_cstr(sz_gccgo_arch[i].arch))) {
                TypesSizes sz = {&sz_std_vt,
                                 (void *)(uintptr_t)&sz_gccgo_arch[i].sizes};
                return sz;
            }
        }
    }
    return none;
}

/* ----------------------------------------------------------- Config sizes */

/* The Sizes a Config uses: its own, or stdSizes, which is SizesFor("gc",
 * "amd64"), when it has none. Not in Go: conf may be NULL, which also gives
 * stdSizes. */
static TypesSizes sz_conf_sizes(const TypesConfig *conf) {
    TypesSizes s = {NULL, NULL};
    if (conf != NULL)
        s = conf->sizes;
    if (s.vt == NULL)
        s = types_sizes_for(BURROW_S("gc"), BURROW_S("amd64"));
    burrow__types_assert(s.vt != NULL);
    return s;
}

int64_t burrow__types_conf_alignof(const TypesConfig *conf, TypesType T) {
    int64_t a = types_sizes_alignof(sz_conf_sizes(conf), T);
    if (a >= 1)
        return a;
    panic_str(BURROW_S("implementation of alignof returned an alignment < 1"));
}

Slice burrow__types_conf_offsetsof(const TypesConfig *conf, Alloc *a, TypesStruct *T) {
    Slice offsets = slice_nil(TYPE_INT64);
    if (T->fields.len > 0) {
        /* compute offsets on demand */
        offsets = types_sizes_offsetsof(sz_conf_sizes(conf), a, T->fields);
        /* sanity checks */
        if (offsets.len != T->fields.len)
            panic_str(BURROW_S(
                "implementation of offsetsof returned the wrong number of offsets"));
    }
    return offsets;
}

int64_t burrow__types_conf_offsetof(const TypesConfig *conf, Alloc *a, TypesType T,
                                    Slice index) {
    int64_t offs = 0;
    for (Int k = 0; k < index.len; k++) {
        Int i = GT_AT(index, k, Int);
        TypesStruct *s = types_as_struct(types_type_underlying(T));
        Slice offsets = burrow__types_conf_offsetsof(conf, a, s);
        int64_t d = GT_AT(offsets, i, int64_t);
        if (d < 0)
            return -1;
        offs += d;
        if (offs < 0)
            return -1;
        T = GT_OBJ(GT_AT(s->fields, i, TypesVar *))->typ;
    }
    return offs;
}

int64_t burrow__types_conf_sizeof(const TypesConfig *conf, TypesType T) {
    return types_sizes_sizeof(sz_conf_sizes(conf), T);
}
