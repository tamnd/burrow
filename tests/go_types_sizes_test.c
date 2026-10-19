/* Derived from Go's src/go/types/sizes_test.go. Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Go's tests type check a small package for each case. These cases were
 * written before burrow had a checker, so most of them build the types the
 * checker would make for their source, which is kept next to them.
 * TestIssue16902 uses the checker. */

#include "check.h"
#include "go_types_helpers.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"

#include <stdint.h>

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------------- builders */

typedef struct World {
    Alloc *a;
    TypesPackage *pkg;
} World;

static void world_init(World *w, Alloc *a) {
    w->a = a;
    w->pkg = types_new_package(a, S("p"), S("p"));
}

static TypesVar *field(World *w, Str name, TypesType typ) {
    return types_new_field(w->a, TOKEN_NO_POS, w->pkg, name, typ, false);
}

static TypesStruct *strct(World *w, TypesVar *const *fields, Int n) {
    Slice s = burrow__types_ptrs_make(w->a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(s, i, TypesVar *) = fields[i];
    return types_new_struct(w->a, s, slice_nil(TYPE_STRING));
}

static TypesType named_in(World *w, TypesPackage *pkg, Str name, TypesType u) {
    TypesTypeName *obj = types_new_type_name(w->a, TOKEN_NO_POS, pkg, name, NULL);
    return types_type(types_new_named(w->a, obj, u, slice_nil(TYPE_UNSAFE_POINTER)));
}

static TypesType array(World *w, TypesType elem, int64_t n) {
    return types_type(types_new_array(w->a, elem, n));
}

/* sync/atomic's Int64, as sync/atomic declares it:
 *
 *	type Int64 struct {
 *		_ noCopy
 *		_ align64
 *		v int64
 *	}
 *
 * noCopy and align64 are both struct{}. */
static TypesType atomic_int64(World *w) {
    TypesPackage *atomic = types_new_package(w->a, S("sync/atomic"), S("atomic"));
    TypesType no_copy = named_in(w, atomic, S("noCopy"), types_type(strct(w, NULL, 0)));
    TypesType align64 =
        named_in(w, atomic, S("align64"), types_type(strct(w, NULL, 0)));
    TypesVar *fs[] = {
        types_new_field(w->a, TOKEN_NO_POS, atomic, S("_"), no_copy, false),
        types_new_field(w->a, TOKEN_NO_POS, atomic, S("_"), align64, false),
        types_new_field(w->a, TOKEN_NO_POS, atomic, S("v"), GT_TYP(TYPES_INT64), false),
    };
    return named_in(w, atomic, S("Int64"), types_type(strct(w, fs, NELEM(fs))));
}

/* TYPES_NO_QUALIFIER is a compound literal, which MSVC warns about (C4189) when
 * it ends up inside a _v macro's _Generic. */
static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

static bool offsets_equal(Slice got, const int64_t *want, Int n) {
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++)
        if (((const int64_t *)got.p)[i] != want[i])
            return false;
    return true;
}

/* ------------------------------------------------------------------- tests */

/* go.dev/issue/16316 */
static void TestMultipleSizeUse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    World w;
    world_init(&w, arena_allocator(&ar));
    /*	type S struct {
     *	    i int
     *	    b bool
     *	    s string
     *	    n int
     *	} */
    TypesVar *fs[] = {
        field(&w, S("i"), GT_TYP(TYPES_INT)),
        field(&w, S("b"), GT_TYP(TYPES_BOOL)),
        field(&w, S("s"), GT_TYP(TYPES_STRING)),
        field(&w, S("n"), GT_TYP(TYPES_INT)),
    };
    TypesType ts = types_type(strct(&w, fs, NELEM(fs)));
    TypesStdSizes sizes = {4, 4};
    int64_t got = types_std_sizes_sizeof(&sizes, ts);
    if (got != 20)
        testing_t_errorf_v(t, "Sizeof(%s) with WordSize 4 = %d want 20",
                           type_string(w.a, ts), got);
    sizes = (TypesStdSizes){8, 8};
    got = types_std_sizes_sizeof(&sizes, ts);
    if (got != 40)
        testing_t_errorf_v(t, "Sizeof(%s) with WordSize 8 = %d want 40",
                           type_string(w.a, ts), got);
    arena_free(&ar);
}

/* go.dev/issue/16464 */
static void TestAlignofNaclSlice(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    World w;
    world_init(&w, arena_allocator(&ar));
    /*	var s struct {
     *		x *int
     *		y []byte
     *	} */
    TypesVar *fs[] = {
        field(&w, S("x"), types_type(types_new_pointer(w.a, GT_TYP(TYPES_INT)))),
        field(&w, S("y"), types_type(types_new_slice(w.a, GT_TYP(TYPES_BYTE)))),
    };
    TypesStruct *ts = strct(&w, fs, NELEM(fs));
    TypesStdSizes sizes = {4, 8};
    Slice fields = burrow__types_ptrs_make(w.a, types_struct_num_fields(ts));
    for (Int i = 0; i < types_struct_num_fields(ts); i++)
        GT_AT(fields, i, TypesVar *) = types_struct_field(ts, i);
    Slice offsets = types_std_sizes_offsetsof(&sizes, w.a, fields);
    const int64_t want[] = {0, 4};
    if (!offsets_equal(offsets, want, NELEM(want)))
        testing_t_errorf_v(t, "OffsetsOf(%s) = %v want [0 4]",
                           type_string(w.a, types_type(ts)), offsets);
    arena_free(&ar);
}

/* go.dev/issue/53884. */
static void TestIssue16902(TestingT *t) {
    Str src = S("\n"
                "package a\n"
                "\n"
                "import \"unsafe\"\n"
                "\n"
                "const _ = unsafe.Offsetof(struct{ x int64 }{}.x)\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesInfo info = {
        .types = map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0)};
    TypesStdSizes std = {8, 8};
    TypesConfig conf = {
        /* TODO(adonovan): use same FileSet as mustTypecheck. */
        .importer = default_importer(token_new_file_set(a)),
        .sizes = types_std_sizes_as_sizes(&std),
    };
    must_typecheck(a, src, &conf, &info);
    MapIter it = map_iter(info.types);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        TypesTypeAndValue *tv = v;
        (void)types_sizes_sizeof(conf.sizes, tv->type);
        (void)types_sizes_alignof(conf.sizes, tv->type);
    }
    (void)t;
    arena_free(&ar);
}

static void TestAtomicAlign(TestingT *t) {
    static const char *const arches[] = {"386", "amd64"};
    const int64_t want[] = {0, 8, 16};
    for (Int i = 0; i < NELEM(arches); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        World w;
        world_init(&w, arena_allocator(&ar));
        /*	var s struct {
         *		x int32
         *		y atomic.Int64
         *		z int64
         *	} */
        TypesVar *fs[] = {
            field(&w, S("x"), GT_TYP(TYPES_INT32)),
            field(&w, S("y"), atomic_int64(&w)),
            field(&w, S("z"), GT_TYP(TYPES_INT64)),
        };
        TypesStruct *ts = strct(&w, fs, NELEM(fs));
        TypesSizes sizes = types_sizes_for(S("gc"), str_from_cstr(arches[i]));
        Slice fields = burrow__types_ptrs_make(w.a, types_struct_num_fields(ts));
        for (Int j = 0; j < types_struct_num_fields(ts); j++)
            GT_AT(fields, j, TypesVar *) = types_struct_field(ts, j);
        Slice offsets = types_sizes_offsetsof(sizes, w.a, fields);
        if (!offsets_equal(offsets, want, NELEM(want)))
            testing_t_errorf_v(t, "%s: OffsetsOf(%s) = %v want [0 8 16]", arches[i],
                               type_string(w.a, types_type(ts)), offsets);
        arena_free(&ar);
    }
}

/* Go type checks each source with SizesFor("gc", "amd64") and lets its assert
 * check the size. Here the size is asked for directly. */
static void TestGCSizes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    World w;
    world_init(&w, arena_allocator(&ar));
    TypesSizes sizes = types_sizes_for(S("gc"), S("amd64"));

    /* issue60431: the foo struct size is expected to be rounded up to 16 bytes.
     *
     *	type foo struct {
     *		a int64
     *		b bool
     *	} */
    TypesVar *foo[] = {
        field(&w, S("a"), GT_TYP(TYPES_INT64)),
        field(&w, S("b"), GT_TYP(TYPES_BOOL)),
    };
    TypesType foo_t =
        named_in(&w, w.pkg, S("foo"), types_type(strct(&w, foo, NELEM(foo))));
    int64_t got = types_sizes_sizeof(sizes, foo_t);
    if (got != 16)
        testing_t_errorf_v(t, "issue60431: unsafe.Sizeof(foo{}) = %d, want 16", got);

    /* issue60734: the Data struct size is expected to be rounded up to 16
     * bytes.
     *
     *	type Data struct {
     *		Value  uint32   // 4 bytes
     *		Label  [10]byte // 10 bytes
     *		Active bool     // 1 byte
     *		// padded with 1 byte to make it align
     *	} */
    TypesVar *data[] = {
        field(&w, S("Value"), GT_TYP(TYPES_UINT32)),
        field(&w, S("Label"), array(&w, GT_TYP(TYPES_BYTE), 10)),
        field(&w, S("Active"), GT_TYP(TYPES_BOOL)),
    };
    TypesType data_t =
        named_in(&w, w.pkg, S("Data"), types_type(strct(&w, data, NELEM(data))));
    got = types_sizes_sizeof(sizes, data_t);
    if (got != 16)
        testing_t_errorf_v(t, "issue60734: unsafe.Sizeof(Data{}) = %d, want 16", got);
    arena_free(&ar);
}

/* ------------------------------------------------------------ not in Go */

/* The types TestSizes measures, in the order of size_types. */
static void build_types(World *w, TypesType *out) {
    TypesType empty = types_type(strct(w, NULL, 0));
    TypesVar *pair[] = {
        field(w, S("a"), GT_TYP(TYPES_INT64)),
        field(w, S("b"), GT_TYP(TYPES_BOOL)),
    };
    TypesVar *zero_end[] = {
        field(w, S("a"), GT_TYP(TYPES_INT32)),
        field(w, S("b"), empty),
    };
    TypesVar *mixed[] = {
        field(w, S("a"), GT_TYP(TYPES_INT8)),
        field(w, S("b"), GT_TYP(TYPES_COMPLEX128)),
        field(w, S("c"), GT_TYP(TYPES_INT16)),
    };
    TypesVar *atomic[] = {
        field(w, S("a"), GT_TYP(TYPES_BOOL)),
        field(w, S("b"), atomic_int64(w)),
    };
    TypesVar *huge[] = {
        field(w, S("a"), array(w, GT_TYP(TYPES_INT64), (int64_t)1 << 62)),
        field(w, S("b"), GT_TYP(TYPES_INT8)),
    };
    Int i = 0;
    out[i++] = GT_TYP(TYPES_BOOL);
    out[i++] = GT_TYP(TYPES_INT);
    out[i++] = GT_TYP(TYPES_UINTPTR);
    out[i++] = GT_TYP(TYPES_COMPLEX64);
    out[i++] = GT_TYP(TYPES_COMPLEX128);
    out[i++] = GT_TYP(TYPES_STRING);
    out[i++] = GT_TYP(TYPES_UNSAFE_POINTER);
    out[i++] = types_type(types_new_pointer(w->a, GT_TYP(TYPES_INT)));
    out[i++] = types_type(types_new_slice(w->a, GT_TYP(TYPES_BYTE)));
    out[i++] = types_type(types_new_map(w->a, GT_TYP(TYPES_STRING), GT_TYP(TYPES_INT)));
    out[i++] = types_type(types_new_chan(w->a, TYPES_SEND_RECV, GT_TYP(TYPES_INT)));
    out[i++] = types_type(types_new_signature(w->a, NULL, NULL, NULL, false));
    out[i++] = types_type(types_new_interface_type(w->a, slice_nil(TYPE_UNSAFE_POINTER),
                                                   slice_nil(TYPE_UNSAFE_POINTER)));
    out[i++] = empty;
    out[i++] = array(w, GT_TYP(TYPES_INT64), 0);
    out[i++] = array(w, types_type(strct(w, pair, NELEM(pair))), 3);
    out[i++] = types_type(strct(w, zero_end, NELEM(zero_end)));
    out[i++] = types_type(strct(w, mixed, NELEM(mixed)));
    out[i++] = types_type(strct(w, atomic, NELEM(atomic)));
    out[i++] = array(w, GT_TYP(TYPES_INT64), (int64_t)1 << 59);
    out[i++] = array(w, GT_TYP(TYPES_INT64), (int64_t)1 << 60);
    out[i++] = types_type(strct(w, huge, NELEM(huge)));
}

static const char *const size_types[] = {
    "bool",
    "int",
    "uintptr",
    "complex64",
    "complex128",
    "string",
    "unsafe.Pointer",
    "*int",
    "[]byte",
    "map[string]int",
    "chan int",
    "func()",
    "interface{}",
    "struct{}",
    "[0]int64",
    "[3]struct{a int64; b bool}",
    "struct{a int32; b struct{}}",
    "struct{a int8; b complex128; c int16}",
    "struct{a bool; b atomic.Int64}",
    "[1<<59]int64",
    "[1<<60]int64",
    "struct{a [1<<62]int64; b int8}",
};

/* Sizeof, Alignof and, for a struct, Offsetsof, as Go 1.27.1 gives them for
 * the same types with StdSizes{8, 8}, SizesFor("gc", "386"), SizesFor("gc",
 * "amd64") and SizesFor("gccgo", "m68k"). The arrays too large for an int64
 * show the overflow Go allows: StdSizes adds the last element after it checks,
 * and gc checks the whole product. */
static const struct {
    Int sizes;
    Int typ;
    int64_t size;
    int64_t align;
    Int noffs;
    int64_t offs[3];
} size_tests[] = {
    {0, 0, 1, 1, 0, {0}},          {0, 1, 8, 8, 0, {0}},
    {0, 2, 8, 8, 0, {0}},          {0, 3, 8, 4, 0, {0}},
    {0, 4, 16, 8, 0, {0}},         {0, 5, 16, 8, 0, {0}},
    {0, 6, 8, 8, 0, {0}},          {0, 7, 8, 8, 0, {0}},
    {0, 8, 24, 8, 0, {0}},         {0, 9, 8, 8, 0, {0}},
    {0, 10, 8, 8, 0, {0}},         {0, 11, 8, 8, 0, {0}},
    {0, 12, 16, 8, 0, {0}},        {0, 13, 0, 1, 0, {0}},
    {0, 14, 0, 8, 0, {0}},         {0, 15, 41, 8, 0, {0}},
    {0, 16, 4, 4, 2, {0, 4}},      {0, 17, 26, 8, 3, {0, 8, 24}},
    {0, 18, 16, 8, 2, {0, 8}},     {0, 19, 4611686018427387904, 8, 0, {0}},
    {0, 20, INT64_MIN, 8, 0, {0}}, {0, 21, -1, 8, 2, {0, -1}},
    {1, 0, 1, 1, 0, {0}},          {1, 1, 4, 4, 0, {0}},
    {1, 2, 4, 4, 0, {0}},          {1, 3, 8, 4, 0, {0}},
    {1, 4, 16, 4, 0, {0}},         {1, 5, 8, 4, 0, {0}},
    {1, 6, 4, 4, 0, {0}},          {1, 7, 4, 4, 0, {0}},
    {1, 8, 12, 4, 0, {0}},         {1, 9, 4, 4, 0, {0}},
    {1, 10, 4, 4, 0, {0}},         {1, 11, 4, 4, 0, {0}},
    {1, 12, 8, 4, 0, {0}},         {1, 13, 0, 1, 0, {0}},
    {1, 14, 0, 4, 0, {0}},         {1, 15, 36, 4, 0, {0}},
    {1, 16, 8, 4, 2, {0, 4}},      {1, 17, 24, 4, 3, {0, 4, 20}},
    {1, 18, 16, 8, 2, {0, 8}},     {1, 19, 4611686018427387904, 4, 0, {0}},
    {1, 20, -1, 4, 0, {0}},        {1, 21, -1, 4, 2, {0, -1}},
    {2, 0, 1, 1, 0, {0}},          {2, 1, 8, 8, 0, {0}},
    {2, 2, 8, 8, 0, {0}},          {2, 3, 8, 4, 0, {0}},
    {2, 4, 16, 8, 0, {0}},         {2, 5, 16, 8, 0, {0}},
    {2, 6, 8, 8, 0, {0}},          {2, 7, 8, 8, 0, {0}},
    {2, 8, 24, 8, 0, {0}},         {2, 9, 8, 8, 0, {0}},
    {2, 10, 8, 8, 0, {0}},         {2, 11, 8, 8, 0, {0}},
    {2, 12, 16, 8, 0, {0}},        {2, 13, 0, 1, 0, {0}},
    {2, 14, 0, 8, 0, {0}},         {2, 15, 48, 8, 0, {0}},
    {2, 16, 8, 4, 2, {0, 4}},      {2, 17, 32, 8, 3, {0, 8, 24}},
    {2, 18, 16, 8, 2, {0, 8}},     {2, 19, 4611686018427387904, 8, 0, {0}},
    {2, 20, -1, 8, 0, {0}},        {2, 21, -1, 8, 2, {0, -1}},
    {3, 0, 1, 1, 0, {0}},          {3, 1, 4, 2, 0, {0}},
    {3, 2, 4, 2, 0, {0}},          {3, 3, 8, 2, 0, {0}},
    {3, 4, 16, 2, 0, {0}},         {3, 5, 8, 4, 0, {0}},
    {3, 6, 4, 2, 0, {0}},          {3, 7, 4, 2, 0, {0}},
    {3, 8, 12, 4, 0, {0}},         {3, 9, 4, 2, 0, {0}},
    {3, 10, 4, 2, 0, {0}},         {3, 11, 4, 2, 0, {0}},
    {3, 12, 8, 4, 0, {0}},         {3, 13, 0, 1, 0, {0}},
    {3, 14, 0, 2, 0, {0}},         {3, 15, 29, 2, 0, {0}},
    {3, 16, 4, 2, 2, {0, 4}},      {3, 17, 20, 2, 3, {0, 2, 18}},
    {3, 18, 16, 8, 2, {0, 8}},     {3, 19, 4611686018427387904, 2, 0, {0}},
    {3, 20, INT64_MIN, 2, 0, {0}}, {3, 21, -1, 2, 2, {0, -1}},
};

static void TestSizes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    World w;
    world_init(&w, arena_allocator(&ar));
    TypesType typs[NELEM(size_types)];
    build_types(&w, typs);
    TypesStdSizes std = {8, 8};
    const char *names[] = {"StdSizes{8, 8}", "gc 386", "gc amd64", "gccgo m68k"};
    TypesSizes sizes[] = {
        types_std_sizes_as_sizes(&std),
        types_sizes_for(S("gc"), S("386")),
        types_sizes_for(S("gc"), S("amd64")),
        types_sizes_for(S("gccgo"), S("m68k")),
    };
    for (Int i = 0; i < NELEM(size_tests); i++) {
        TypesSizes s = sizes[size_tests[i].sizes];
        const char *name = names[size_tests[i].sizes];
        TypesType typ = typs[size_tests[i].typ];
        const char *tname = size_types[size_tests[i].typ];
        int64_t got = types_sizes_sizeof(s, typ);
        if (got != size_tests[i].size)
            testing_t_errorf_v(t, "%s: Sizeof(%s) = %d, want %d", name, tname, got,
                               size_tests[i].size);
        got = types_sizes_alignof(s, typ);
        if (got != size_tests[i].align)
            testing_t_errorf_v(t, "%s: Alignof(%s) = %d, want %d", name, tname, got,
                               size_tests[i].align);
        if (size_tests[i].noffs == 0)
            continue;
        TypesStruct *st = types_as_struct(types_type_underlying(typ));
        Slice fields = burrow__types_ptrs_make(w.a, types_struct_num_fields(st));
        for (Int j = 0; j < types_struct_num_fields(st); j++)
            GT_AT(fields, j, TypesVar *) = types_struct_field(st, j);
        Slice offs = types_sizes_offsetsof(s, w.a, fields);
        if (!offsets_equal(offs, size_tests[i].offs, size_tests[i].noffs))
            testing_t_errorf_v(t, "%s: Offsetsof(%s) = %v", name, tname, offs);
    }
    arena_free(&ar);
}

/* SizesFor knows gc's own architectures and gccgo's, and nothing else. */
static void TestSizesFor(TestingT *t) {
    CHECK(types_sizes_for(S("gc"), S("m68k")).vt == NULL);
    CHECK(types_sizes_for(S("gccgo"), S("m68k")).vt != NULL);
    CHECK(types_sizes_for(S("tinygo"), S("amd64")).vt == NULL);
    CHECK(types_sizes_for(S("gc"), S("")).vt == NULL);
    /* Go's gcArchSizes. */
    static const struct {
        const char *arch;
        int64_t word_size;
        int64_t max_align;
    } gc[] = {
        {"386", 4, 4},     {"amd64", 8, 8},    {"amd64p32", 4, 8}, {"arm", 4, 4},
        {"arm64", 8, 8},   {"loong64", 8, 8},  {"mips", 4, 4},     {"mipsle", 4, 4},
        {"mips64", 8, 8},  {"mips64le", 8, 8}, {"ppc64", 8, 8},    {"ppc64le", 8, 8},
        {"riscv64", 8, 8}, {"s390x", 8, 8},    {"sparc64", 8, 8},  {"wasm", 8, 8},
    };
    for (Int i = 0; i < NELEM(gc); i++) {
        TypesSizes s = types_sizes_for(S("gc"), str_from_cstr(gc[i].arch));
        if (s.vt == NULL) {
            testing_t_errorf_v(t, "SizesFor(\"gc\", %q) = nil", gc[i].arch);
            continue;
        }
        /* uintptr is a word, and int64 is aligned to the largest alignment. */
        int64_t word = types_sizes_sizeof(s, GT_TYP(TYPES_UINTPTR));
        int64_t max = types_sizes_alignof(s, GT_TYP(TYPES_INT64));
        if (word != gc[i].word_size || max != gc[i].max_align)
            testing_t_errorf_v(
                t, "SizesFor(\"gc\", %q): word %d, max align %d, want %d, %d",
                gc[i].arch, word, max, gc[i].word_size, gc[i].max_align);
    }
}

#define TESTS(X)                                                                       \
    X(TestMultipleSizeUse)                                                             \
    X(TestAlignofNaclSlice)                                                            \
    X(TestIssue16902)                                                                  \
    X(TestAtomicAlign)                                                                 \
    X(TestGCSizes)                                                                     \
    X(TestSizes)                                                                       \
    X(TestSizesFor)

TESTING_MAIN(TESTS)
