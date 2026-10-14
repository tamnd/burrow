/* Tests for reflect.StructOf.
 *
 * These are Go's tests from all_test.go and type_test.go, under Go's names.
 * Four things about burrow change what they can do.
 *
 * Nothing can make a vtable at run time, so a struct StructOf makes goes into
 * no interface with methods. Where Go asserts v.Interface().(Iface), the test
 * asks Implements instead and calls the method with MethodByName and Call.
 *
 * A receiver is always a pointer, so a type has the methods of its pointer and
 * TestStructOfWithInterface's StructIPtr by value, which Go expects to have no
 * Get, is left out.
 *
 * Whether a field is exported follows from the first byte of its name, so the
 * last two cases of TestStructOfExportRules, Φ and φ, are left out.
 *
 * A descriptor keeps its size in 32 bits, so TestStructOfTooLarge uses the
 * sizes Go's test takes on a 32 bit platform.
 *
 * TestStructOfDirectIface is left out, as InterfaceData has nothing to stand
 * for here, and so are Go's calls to runtime.GC.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"
#include "burrow/time.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

/* A descriptor written out by hand. */
#define DESC(name, pkg, kind, size, align, nfield, fields, nmethod, methods, elem)     \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        nfield,                                                                        \
        nmethod,                                                                       \
        fields,                                                                        \
        methods,                                                                       \
        elem,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* []StructField{...}. */
#define NFIELDS(...)                                                                   \
    ((Int)(sizeof((ReflectStructField[]){__VA_ARGS__}) / sizeof(ReflectStructField)))
#define FIELDS(...)                                                                    \
    slice_from((ReflectStructField[]){__VA_ARGS__}, NFIELDS(__VA_ARGS__),              \
               NFIELDS(__VA_ARGS__), TYPE_REFLECT_STRUCT_FIELD)
#define FIELD_SLICE(arr)                                                               \
    slice_from(arr, (Int)(sizeof(arr) / sizeof(arr)[0]),                               \
               (Int)(sizeof(arr) / sizeof(arr)[0]), TYPE_REFLECT_STRUCT_FIELD)
#define NOVALS ((Slice){NULL, 0, 0, TYPE_REFLECT_VALUE})

/* ----------------------------------------------------------------- helpers */

static ReflectStructField sf(const char *name, const char *pkg, ReflectType t,
                             bool anonymous) {
    ReflectStructField f;
    memset(&f, 0, sizeof f);
    f.name = str_from_cstr(name);
    f.pkg_path = str_from_cstr(pkg);
    f.type = t;
    f.anonymous = anonymous;
    return f;
}

static ReflectStructField tagged(const char *name, ReflectType t, const char *tag) {
    ReflectStructField f = sf(name, "", t, false);
    f.tag = str_from_cstr(tag);
    return f;
}

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

/* New(t).Elem(), made in a. */
static ReflectValue new_elem(Alloc *a, ReflectType t) {
    return reflect_value_elem(reflect_new(a, t));
}

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

static bool str_is(Str s, const char *want) {
    return str_eq(s, str_from_cstr(want));
}

static bool panics(void (*f)(void *), void *env, char *buf, size_t n) {
    volatile bool got = false;
    buf[0] = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str text = panic_text(r);
        size_t k = (size_t)text.len < n - 1 ? (size_t)text.len : n - 1;
        memcpy(buf, text.p, k);
        buf[k] = 0;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* Go's shouldPanic: f has to panic, and when expect is not empty the text has
 * to start with "reflect" and contain expect. */
static void should_panic(TestingT *t, const char *expect, void (*f)(void *),
                         void *env) {
    char buf[256];
    if (!panics(f, env, buf, sizeof buf)) {
        testing_t_errorf_v(t, "did not panic, want %q", expect);
        return;
    }
    if (expect[0] == 0)
        return;
    if (strncmp(buf, "reflect", 7) != 0)
        testing_t_errorf_v(t, "panic string does not start with \"reflect\": %s", buf);
    if (strstr(buf, expect) == NULL)
        testing_t_errorf_v(t, "%q does not contain %q", buf, expect);
}

static void struct_of(void *env) {
    (void)reflect_struct_of(*(const Slice *)env);
}

static void should_panic_struct_of(TestingT *t, const char *expect, Slice fields) {
    should_panic(t, expect, struct_of, &fields);
}

/* Go's checkSameType, as far as C can take it: C has no way to find the
 * descriptor the compiler made for a type, so this asks that the two read as
 * the same type and that the made type's zero value carries it. */
static void check_same_type(TestingT *t, ReflectType x, ReflectType y) {
    ReflectType z = reflect_type_of(reflect_value_interface(reflect_zero(x)));
    if (!str_eq(reflect_type_string(x), reflect_type_string(y)) ||
        reflect_type_kind(x) != reflect_type_kind(y) ||
        reflect_type_size(x) != reflect_type_size(y) || z != x)
        testing_t_errorf_v(t, "did not find preexisting type for %s (vs %s)",
                           reflect_type_string(x), reflect_type_string(y));
}

/* ---------------------------------------------------- TestStructOfFieldName */

static void TestStructOfFieldName(TestingT *t) {
    /* invalid field name "1nvalid" */
    should_panic_struct_of(t, "has invalid name",
                           FIELDS(sf("Valid", "", TYPE_STRING, false),
                                  sf("1nvalid", "", TYPE_STRING, false)));

    /* invalid field name "+" */
    should_panic_struct_of(
        t, "has invalid name",
        FIELDS(sf("Val1d", "", TYPE_STRING, false), sf("+", "", TYPE_STRING, false)));

    /* no field name */
    should_panic_struct_of(t, "has no name", FIELDS(sf("", "", TYPE_STRING, false)));

    /* verify creation of a struct with valid struct fields */
    ReflectType valid = reflect_struct_of(FIELDS(
        sf("\xcf\x86", "", TYPE_STRING, false), sf("ValidName", "", TYPE_STRING, false),
        sf("Val1dNam5", "", TYPE_STRING, false)));

    const char *want = "struct { \xcf\x86 string; ValidName string; Val1dNam5 string }";
    if (!str_is(reflect_type_string(valid), want))
        testing_t_errorf_v(t, "StructOf(validFields).String()=%q, want %q",
                           reflect_type_string(valid), want);
}

/* ------------------------------------------------------------- TestStructOf */

/* struct { String string; X byte; Y uint64; Z [3]uint16 }, whose layout the
 * made struct has to match. */
typedef struct StructOfLayout {
    Str String;
    uint8_t X;
    uint64_t Y;
    uint16_t Z[3];
} StructOfLayout;

/* struct{ Y uint64 } */
static const Field y_fields[] = {{S_("Y"), S_(""), TYPE_UINT64, 0}};
static const Type ty_struct_y = DESC(NONAME, NONAME, KIND_STRUCT, sizeof(uint64_t),
                                     _Alignof(uint64_t), 1, y_fields, 0, NULL, NULL);

/* type structFieldType any, and struct{ F structFieldType } */
static const Type ty_structFieldType =
    DESC(S_("structFieldType"), TEST_PKG, KIND_INTERFACE, sizeof(Any), _Alignof(Any), 0,
         NULL, 0, NULL, NULL);
static const Field f_fields[] = {{S_("F"), S_(""), &ty_structFieldType, 0}};
static const Type ty_struct_f = DESC(NONAME, NONAME, KIND_STRUCT, sizeof(Any),
                                     _Alignof(Any), 1, f_fields, 0, NULL, NULL);

static void TestStructOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    /* check construction and use of type not in binary */
    ReflectStructField fields[] = {
        tagged("S", TYPE_STRING, "s"),
        tagged("X", TYPE_UINT8, "x"),
        sf("Y", "", TYPE_UINT64, false),
        sf("Z", "", reflect_array_of(3, TYPE_UINT16), false),
    };

    ReflectType st = reflect_struct_of(FIELD_SLICE(fields));
    ReflectValue v = new_elem(a, st);
    uint8_t two = 2;
    uint8_t one = 1;
    Int index1[] = {1};
    reflect_value_set(reflect_value_field_by_name(v, BURROW_S("X")),
                      val(TYPE_UINT8, &two));
    reflect_value_set(
        reflect_value_field_by_index(v, slice_from(index1, 1, 1, TYPE_INT)),
        val(TYPE_UINT8, &one));

    Any args[1] = {reflect_value_interface(v)};
    Str s = fmt_sprint(a, (Slice){args, 1, 1, TYPE_ANY});
    const char *want = "{ 1 0 [0 0 0]}";
    if (!str_is(s, want))
        testing_t_errorf_v(t, "constructed struct = %s, want %s", s, want);
    const char *st_str =
        "struct { S string \"s\"; X uint8 \"x\"; Y uint64; Z [3]uint16 }";
    if (!str_is(reflect_type_string(st), st_str))
        testing_t_errorf_v(t, "StructOf(fields).String()=%q, want %q",
                           reflect_type_string(st), st_str);

    /* check the size, alignment and field offsets */
    const Uintptr offsets[] = {offsetof(StructOfLayout, String),
                               offsetof(StructOfLayout, X), offsetof(StructOfLayout, Y),
                               offsetof(StructOfLayout, Z)};
    if (reflect_type_size(st) != sizeof(StructOfLayout))
        testing_t_errorf_v(t, "constructed struct size = %d, want %d",
                           reflect_type_size(st), (Uintptr)sizeof(StructOfLayout));
    if (reflect_type_align(st) != (Int) _Alignof(StructOfLayout))
        testing_t_errorf_v(t, "constructed struct align = %d, want %d",
                           reflect_type_align(st), (Int) _Alignof(StructOfLayout));
    if (reflect_type_field_align(st) != (Int) _Alignof(StructOfLayout))
        testing_t_errorf_v(t, "constructed struct field align = %d, want %d",
                           reflect_type_field_align(st),
                           (Int) _Alignof(StructOfLayout));
    for (Int i = 0; i < reflect_type_num_field(st); i++) {
        Uintptr o1 = reflect_type_field(st, i).offset;
        if (o1 != offsets[i])
            testing_t_errorf_v(t, "constructed struct field %d offset = %d, want %d", i,
                               o1, offsets[i]);
    }

    /* Check size and alignment with a trailing zero-sized field. C has no
     * struct { G1 byte; G2 [0]*byte } to compare with, so the numbers are the
     * ones Go gives for it: G2 aligned as a pointer, and a byte after it. */
    st = reflect_struct_of(FIELDS(
        sf("F1", "", TYPE_BYTE, false),
        sf("F2", "", reflect_array_of(0, reflect_pointer_to(TYPE_BYTE)), false)));
    const Uintptr zero_offsets[] = {0, sizeof(void *)};
    if (reflect_type_size(st) != 2 * sizeof(void *))
        testing_t_errorf_v(t, "constructed zero-padded struct size = %d, want %d",
                           reflect_type_size(st), (Uintptr)(2 * sizeof(void *)));
    if (reflect_type_align(st) != (Int) _Alignof(void *))
        testing_t_errorf_v(t, "constructed zero-padded struct align = %d, want %d",
                           reflect_type_align(st), (Int) _Alignof(void *));
    if (reflect_type_field_align(st) != (Int) _Alignof(void *))
        testing_t_errorf_v(t,
                           "constructed zero-padded struct field align = %d, want %d",
                           reflect_type_field_align(st), (Int) _Alignof(void *));
    for (Int i = 0; i < reflect_type_num_field(st); i++) {
        Uintptr o1 = reflect_type_field(st, i).offset;
        if (o1 != zero_offsets[i])
            testing_t_errorf_v(t,
                               "constructed zero-padded struct field %d offset = %d, "
                               "want %d",
                               i, o1, zero_offsets[i]);
    }

    /* check duplicate names */
    should_panic_struct_of(t, "duplicate field",
                           FIELDS(sf("string", "p", TYPE_STRING, false),
                                  sf("string", "p", TYPE_STRING, false)));
    should_panic_struct_of(
        t, "has no name",
        FIELDS(sf("", "", TYPE_STRING, false), sf("string", "p", TYPE_STRING, false)));
    should_panic_struct_of(
        t, "has no name",
        FIELDS(sf("", "", TYPE_STRING, false), sf("", "", TYPE_STRING, false)));
    /* check that type already in binary is found */
    check_same_type(
        t, reflect_struct_of(slice_from(&fields[2], 1, 1, TYPE_REFLECT_STRUCT_FIELD)),
        &ty_struct_y);

    /* gccgo used to fail this test. */
    check_same_type(t,
                    reflect_struct_of(FIELDS(sf("F", "", &ty_structFieldType, false))),
                    &ty_struct_f);

    arena_free(&ar);
}

/* -------------------------------------------------- TestStructOfExportRules */

/* type S1 struct{}, type s2 struct{} */
static const Type ty_S1 =
    DESC(S_("S1"), TEST_PKG, KIND_STRUCT, 0, 1, 0, NULL, 0, NULL, NULL);
static const Type ty_s2 =
    DESC(S_("s2"), TEST_PKG, KIND_STRUCT, 0, 1, 0, NULL, 0, NULL, NULL);

/* A case: the field, with ptr asking for a pointer to typ, and what to expect. */
typedef struct ExportRule {
    const char *name;
    const char *pkg;
    const Type *typ;
    bool ptr;
    bool anonymous;
    bool must_panic;
    bool exported;
} ExportRule;

static const ExportRule export_rules[] = {
    {"S1", "", &ty_S1, false, true, false, true},
    {"S1", "", &ty_S1, true, true, false, true},
    {"s2", "", &ty_s2, false, true, true, false},
    {"s2", "", &ty_s2, true, true, true, false},
    {"Name", "", NULL, false, false, true, false},
    {"", "", &ty_S1, false, false, true, false},
    {"S1", "other/pkg", &ty_S1, false, true, true, false},
    {"S1", "other/pkg", &ty_S1, true, true, true, false},
    {"s2", "other/pkg", &ty_s2, false, true, true, false},
    {"s2", "other/pkg", &ty_s2, true, true, true, false},
    {"s2", "other/pkg", TYPE_INT, false, false, false, false},
    {"s2", "other/pkg", TYPE_INT, false, false, false, false},
    {"S", "", &ty_S1, false, false, false, true},
    {"S", "", &ty_S1, true, false, false, true},
    {"S", "", &ty_s2, false, false, false, true},
    {"S", "", &ty_s2, true, false, false, true},
    {"s", "", &ty_S1, false, false, true, false},
    {"s", "", &ty_S1, true, false, true, false},
    {"s", "", &ty_s2, false, false, true, false},
    {"s", "", &ty_s2, true, false, true, false},
    {"s", "other/pkg", &ty_S1, false, false, false, false},
    {"s", "other/pkg", &ty_S1, true, false, false, false},
    {"s", "other/pkg", &ty_s2, false, false, false, false},
    {"s", "other/pkg", &ty_s2, true, false, false, false},
    {"", "", &ty_S1, false, false, true, false}, /* ΦType{} in Go */
    {"", "", &ty_s2, false, false, true, false}, /* φType{} in Go */
};

typedef struct ExportEnv {
    const ExportRule *rule;
    Str name;
    Str pkg;
} ExportEnv;

static void export_rule(void *env) {
    ExportEnv *e = env;
    const Type *typ = e->rule->typ;
    if (typ != NULL && e->rule->ptr)
        typ = reflect_pointer_to(typ);
    ReflectStructField f = sf(e->rule->name, e->rule->pkg, typ, e->rule->anonymous);
    ReflectType st = reflect_struct_of(slice_from(&f, 1, 1, TYPE_REFLECT_STRUCT_FIELD));
    ReflectStructField field = reflect_type_field(st, 0);
    e->name = field.name;
    e->pkg = field.pkg_path;
}

static void TestStructOfExportRules(TestingT *t) {
    Int n = (Int)(sizeof export_rules / sizeof export_rules[0]);
    for (Int i = 0; i < n; i++) {
        const ExportRule *test = &export_rules[i];
        ExportEnv e = {test, BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        char buf[256];
        bool got = panics(export_rule, &e, buf, sizeof buf);
        if (!got && test->must_panic)
            testing_t_errorf_v(t, "test-%d did not panic", i);
        if (got && !test->must_panic)
            testing_t_errorf_v(t, "test-%d panicked: %s\n", i, buf);
        if (got)
            continue;
        if (e.name.len == 0) {
            testing_t_errorf_v(t, "test-%d: field.Name must not be empty", i);
            continue;
        }
        /* token.IsExported, for the ASCII names left here */
        bool exported = e.name.p[0] >= 'A' && e.name.p[0] <= 'Z';
        if (exported != test->exported)
            testing_t_errorf_v(t, "test-%d: got exported=%v want exported=%v", i,
                               exported, test->exported);
        if (!str_is(e.pkg, test->pkg))
            testing_t_errorf_v(t, "test-%d: got PkgPath=%q want pkgPath=%q", i, e.pkg,
                               test->pkg);
    }
}

/* ---------------------------------------------------------- TestStructOfGC */

/* type T *uintptr */
static const Type ty_T = DESC(S_("T"), TEST_PKG, KIND_POINTER, sizeof(Uintptr *),
                              _Alignof(Uintptr *), 0, NULL, 0, NULL, TYPE_UINTPTR);

static void TestStructOfGC(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    ReflectType tt = &ty_T;
    ReflectType st =
        reflect_struct_of(FIELDS(sf("X", "", tt, false), sf("Y", "", tt, false)));
    ReflectType pt = reflect_pointer_to(TYPE_UINTPTR);

    enum { N = 10000 };
    Any *x = mem_alloc(a, N * sizeof(Any), _Alignof(Any));
    if (x == NULL)
        abort();
    for (Int i = 0; i < N; i++) {
        ReflectValue v = new_elem(a, st);
        for (Int j = 0; j < reflect_value_num_field(v); j++) {
            Uintptr *p = BURROW_NEW(a, Uintptr);
            if (p == NULL)
                abort();
            *p = (Uintptr)(i * N + j);
            reflect_value_set(reflect_value_field(v, j),
                              reflect_value_convert(val(pt, &p), a, tt));
        }
        x[i] = reflect_value_interface(v);
    }

    for (Int i = 0; i < N; i++) {
        ReflectValue v = reflect_value_of(x[i]);
        for (Int j = 0; j < reflect_value_num_field(v); j++) {
            uint64_t k =
                reflect_value_uint(reflect_value_elem(reflect_value_field(v, j)));
            if (k != (uint64_t)(i * N + j))
                testing_t_errorf_v(t, "lost x[%d].%s = %d, want %d", i,
                                   j == 0 ? "X" : "Y", k, i * N + j);
        }
    }

    arena_free(&ar);
}

/* ---------------------------------------------------------- TestStructOfAlg */

static void compare_self(void *env) {
    const Any *i = env;
    (void)any_equal(*i, *i);
}

static void TestStructOfAlg(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    ReflectType st = reflect_struct_of(FIELDS(tagged("X", TYPE_INT, "x")));
    ReflectValue v1 = new_elem(a, st);
    ReflectValue v2 = new_elem(a, st);
    if (!reflect_deep_equal(reflect_value_interface(v1), reflect_value_interface(v1)))
        testing_t_errorf_v(t, "constructed struct %v not equal to itself",
                           reflect_value_interface(v1));
    Int one = 1;
    reflect_value_set(reflect_value_field_by_name(v1, BURROW_S("X")),
                      val(TYPE_INT, &one));
    Any i1 = reflect_value_interface(v1);
    Any i2 = reflect_value_interface(v2);
    if (reflect_deep_equal(i1, i2))
        testing_t_errorf_v(t, "constructed structs %v and %v should not be equal", i1,
                           i2);

    st = reflect_struct_of(FIELDS(tagged("X", reflect_slice_of(TYPE_INT), "x")));
    v1 = new_elem(a, st);
    i1 = reflect_value_interface(v1);
    should_panic(t, "", compare_self, &i1);

    arena_free(&ar);
}

/* --------------------------------------------------- TestStructOfGenericAlg */

typedef struct GenericAlgCase {
    ReflectType rt;
    Int idx[2];
    Int nidx;
} GenericAlgCase;

static void set_string(ReflectValue v, const GenericAlgCase *c, Str s) {
    ReflectValue f = reflect_value_field_by_index(
        v, slice_from((Int *)(uintptr_t)c->idx, c->nidx, c->nidx, TYPE_INT));
    reflect_value_set(f, val(TYPE_STRING, &s));
}

static void TestStructOfGenericAlg(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    ReflectType st1 = reflect_struct_of(
        FIELDS(tagged("X", TYPE_INT64, "x"), sf("Y", "", TYPE_STRING, false)));
    ReflectType st =
        reflect_struct_of(FIELDS(sf("S0", "", st1, false), sf("S1", "", st1, false)));
    ReflectType a0 = reflect_array_of(0, TYPE_INT);
    ReflectType a1 = reflect_array_of(1, TYPE_INT);
    ReflectType a2 = reflect_array_of(2, TYPE_INT);

    GenericAlgCase tests[] = {
        {st, {0, 1}, 2},
        {st1, {1, 0}, 1},
        {reflect_struct_of(
             FIELDS(sf("XX", "", a0, false), sf("YY", "", TYPE_STRING, false))),
         {1, 0},
         1},
        {reflect_struct_of(FIELDS(sf("XX", "", a0, false),
                                  sf("YY", "", TYPE_STRING, false),
                                  sf("ZZ", "", a2, false))),
         {1, 0},
         1},
        {reflect_struct_of(
             FIELDS(sf("XX", "", a1, false), sf("YY", "", TYPE_STRING, false))),
         {1, 0},
         1},
        {reflect_struct_of(FIELDS(sf("XX", "", a1, false),
                                  sf("YY", "", TYPE_STRING, false),
                                  sf("ZZ", "", a1, false))),
         {1, 0},
         1},
        {reflect_struct_of(FIELDS(sf("XX", "", a2, false),
                                  sf("YY", "", TYPE_STRING, false),
                                  sf("ZZ", "", a2, false))),
         {1, 0},
         1},
        {reflect_struct_of(FIELDS(sf("XX", "", TYPE_INT64, false),
                                  sf("YY", "", TYPE_BYTE, false),
                                  sf("ZZ", "", TYPE_STRING, false))),
         {2, 0},
         1},
        {reflect_struct_of(
             FIELDS(sf("XX", "", TYPE_INT64, false), sf("YY", "", TYPE_INT64, false),
                    sf("ZZ", "", TYPE_STRING, false),
                    sf("AA", "", reflect_array_of(1, TYPE_INT64), false))),
         {2, 0},
         1},
    };

    Int one = 1;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const GenericAlgCase *table = &tests[i];
        ReflectValue v1 = new_elem(a, table->rt);
        ReflectValue v2 = new_elem(a, table->rt);

        if (!reflect_deep_equal(reflect_value_interface(v1),
                                reflect_value_interface(v1)))
            testing_t_errorf_v(t, "constructed struct %v not equal to itself",
                               reflect_value_interface(v1));

        set_string(v1, table, BURROW_S("abc"));
        set_string(v2, table, BURROW_S("def"));
        Any i1 = reflect_value_interface(v1);
        Any i2 = reflect_value_interface(v2);
        if (reflect_deep_equal(i1, i2))
            testing_t_errorf_v(t, "constructed structs %v and %v should not be equal",
                               i1, i2);

        Str abc = BURROW_S("abc");
        set_string(v1, table, abc);
        Str plus = BURROW_S("+abc-");
        set_string(v2, table, (Str){plus.p + 1, 3});
        if (!reflect_deep_equal(i1, i2))
            testing_t_errorf_v(t, "constructed structs %v and %v should be equal", i1,
                               i2);

        /* Test hash */
        ReflectValue m = reflect_make_map(a, reflect_map_of(table->rt, TYPE_INT));
        reflect_value_set_map_index(m, v1, val(TYPE_INT, &one));
        if (!reflect_value_is_valid(reflect_value_map_index(m, v2)))
            testing_t_errorf_v(
                t, "constructed structs %#v and %#v have different hashes", i1, i2);

        set_string(v2, table, BURROW_S("abc"));
        if (!reflect_deep_equal(i1, i2))
            testing_t_errorf_v(t, "constructed structs %v and %v should be equal", i1,
                               i2);

        if (!reflect_value_is_valid(reflect_value_map_index(m, v2)))
            testing_t_errorf_v(t, "constructed structs %v and %v have different hashes",
                               i1, i2);
    }

    arena_free(&ar);
}

/* ------------------------------------------------ TestStructOfWithInterface */

#define SIG_Get(IN, OUT) OUT(Int)
#define SIG_Set(IN, OUT) IN(0, Int)

/* type StructI int, with Get. */
typedef Int StructI;

static Int structi_get(StructI *i) {
    return *i;
}

#define STRUCTI_METHODS(X, T) X(T, Get, structi_get, SIG_Get)
BURROW_METHODS_DEFINE(StructI, STRUCTI_METHODS);
static const Type ty_StructI =
    DESC(S_("StructI"), TEST_PKG, KIND_INT, sizeof(StructI), _Alignof(StructI), 0, NULL,
         1, burrow__methods_StructI, NULL);

/* type StructIPtr int, with Get and Set on the pointer. */
typedef Int StructIPtr;

static Int structiptr_get(StructIPtr *i) {
    return *i;
}

static void structiptr_set(StructIPtr *i, Int v) {
    *i = v;
}

#define STRUCTIPTR_METHODS(X, T)                                                       \
    X(T, Get, structiptr_get, SIG_Get)                                                 \
    X(T, Set, structiptr_set, SIG_Set)
BURROW_METHODS_DEFINE(StructIPtr, STRUCTIPTR_METHODS);
static const Type ty_StructIPtr =
    DESC(S_("StructIPtr"), TEST_PKG, KIND_INT, sizeof(StructIPtr), _Alignof(StructIPtr),
         0, NULL, 2, burrow__methods_StructIPtr, NULL);

/* type SettableStruct struct{ SettableField int }, with Set. */
typedef struct SettableStruct {
    Int SettableField;
} SettableStruct;

static void settable_struct_set(SettableStruct *p, Int v) {
    p->SettableField = v;
}

#define SETTABLE_STRUCT_METHODS(X, T) X(T, Set, settable_struct_set, SIG_Set)
BURROW_METHODS_DEFINE(SettableStruct, SETTABLE_STRUCT_METHODS);
static const Field settable_struct_fields[] = {
    {S_("SettableField"), S_(""), TYPE_INT, offsetof(SettableStruct, SettableField)},
};
static const Type ty_SettableStruct =
    DESC(S_("SettableStruct"), TEST_PKG, KIND_STRUCT, sizeof(SettableStruct),
         _Alignof(SettableStruct), 1, settable_struct_fields, 1,
         burrow__methods_SettableStruct, NULL);

/* type SettablePointer struct{ SettableField *int }, with Set. */
typedef struct SettablePointer {
    Int *SettableField;
} SettablePointer;

static void settable_pointer_set(SettablePointer *p, Int v) {
    *p->SettableField = v;
}

#define SETTABLE_POINTER_METHODS(X, T) X(T, Set, settable_pointer_set, SIG_Set)
BURROW_METHODS_DEFINE(SettablePointer, SETTABLE_POINTER_METHODS);
static const Type ty_ptr_int = DESC(NONAME, NONAME, KIND_POINTER, sizeof(Int *),
                                    _Alignof(Int *), 0, NULL, 0, NULL, TYPE_INT);
static const Field settable_pointer_fields[] = {
    {S_("SettableField"), S_(""), &ty_ptr_int,
     offsetof(SettablePointer, SettableField)},
};
static const Type ty_SettablePointer =
    DESC(S_("SettablePointer"), TEST_PKG, KIND_STRUCT, sizeof(SettablePointer),
         _Alignof(SettablePointer), 1, settable_pointer_fields, 1,
         burrow__methods_SettablePointer, NULL);

/* type Iface interface{ Get() int } and type IfaceSet interface{ Set(int) } */
typedef struct GetterVT {
    const Type *self_type;
    Int (*get)(void *self);
} GetterVT;

typedef struct Getter {
    const GetterVT *vt;
    void *data;
} Getter;

#define IFACE_METHODS(X, I) X(I, Get, get, SIG_Get)
IFACE_METHODS(BURROW__IFACE_METHOD_DEFS, Getter)
static const Method iface_methods[] = {IFACE_METHODS(BURROW__METHOD_ENTRY, Getter)};
static const Type ty_Iface = DESC(S_("Iface"), TEST_PKG, KIND_INTERFACE, sizeof(Getter),
                                  _Alignof(Getter), 0, NULL, 1, iface_methods, NULL);

typedef struct SetterVT {
    const Type *self_type;
    void (*set)(void *self, Int v);
} SetterVT;

typedef struct Setter {
    const SetterVT *vt;
    void *data;
} Setter;

#define IFACE_SET_METHODS(X, I) X(I, Set, set, SIG_Set)
IFACE_SET_METHODS(BURROW__IFACE_METHOD_DEFS, Setter)
static const Method iface_set_methods[] = {
    IFACE_SET_METHODS(BURROW__METHOD_ENTRY, Setter)};
static const Type ty_IfaceSet =
    DESC(S_("IfaceSet"), TEST_PKG, KIND_INTERFACE, sizeof(Setter), _Alignof(Setter), 0,
         NULL, 1, iface_set_methods, NULL);

typedef struct CallEnv {
    ReflectValue v;
    Alloc *a;
    const char *name;
    Slice in;
} CallEnv;

static void call_method(void *env) {
    const CallEnv *e = env;
    ReflectValue m = reflect_value_method_by_name(e->v, e->a, str_from_cstr(e->name));
    (void)reflect_value_call(m, e->a, e->in);
}

static void TestStructOfWithInterface(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    const Int want = 42;
    StructI si = want;
    StructI *psi = &si;
    StructIPtr sip = want;
    StructIPtr *psip = &sip;

    /* Go's fourth case, StructIPtr by value, is left out: its value has Get
     * here, where Go's has no methods at all. */
    struct {
        const char *name;
        ReflectType typ;
        ReflectValue val;
    } tests[] = {
        {"StructI", &ty_StructI, val(&ty_StructI, &si)},
        {"StructI", reflect_pointer_to(&ty_StructI),
         val(reflect_pointer_to(&ty_StructI), &psi)},
        {"StructIPtr", reflect_pointer_to(&ty_StructIPtr),
         val(reflect_pointer_to(&ty_StructIPtr), &psip)},
    };

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        for (Int j = 0; j < 2; j++) {
            ReflectStructField fields[2];
            Int n = 0;
            if (j == 1)
                fields[n++] = sf("Dummy", "", TYPE_INT, false);
            fields[n++] = sf(tests[i].name, "", tests[i].typ, true);
            Slice fs = slice_from(fields, n, n, TYPE_REFLECT_STRUCT_FIELD);

            /* We currently do not correctly implement methods for embedded
             * fields other than the first. Therefore, for now, we expect
             * those methods to not exist. See issues 15924 and 20824. */
            if (j == 1) {
                char buf[256];
                if (!panics(struct_of, &fs, buf, sizeof buf))
                    testing_t_errorf_v(t, "test-%d-%d did not panic", i, j);
                continue;
            }

            ReflectType rt = reflect_struct_of(fs);
            ReflectValue rv = new_elem(a, rt);
            reflect_value_set(reflect_value_field(rv, j), tests[i].val);

            if (!reflect_type_implements(rt, &ty_Iface)) {
                testing_t_errorf_v(t, "test-%d-%d: type=%s fails to implement Iface.\n",
                                   i, j, reflect_type_string(tests[i].typ));
                continue;
            }

            ReflectValue fct = reflect_value_method_by_name(rv, a, BURROW_S("Get"));
            Slice out = reflect_value_call(fct, a, NOVALS);
            Int got = reflect_value_int(((const ReflectValue *)out.p)[0]);
            if (got != want)
                testing_t_errorf_v(t, "test-%d-%d: x.Get()=%d. want=%d\n", i, j, got,
                                   want);
        }
    }

    ReflectValue in[1] = {val(TYPE_INT, &want)};
    Slice set_in = slice_from(in, 1, 1, TYPE_REFLECT_VALUE);

    /* Test an embedded nil pointer with pointer methods. */
    ReflectType rt = reflect_struct_of(
        FIELDS(sf("StructIPtr", "", reflect_pointer_to(&ty_StructIPtr), true)));
    CallEnv e = {new_elem(a, rt), a, "Set", set_in};
    if (!reflect_type_implements(rt, &ty_IfaceSet))
        testing_t_errorf_v(t, "%s does not implement IfaceSet",
                           reflect_type_string(rt));
    /* This should panic since the pointer is nil. */
    should_panic(t, "", call_method, &e);

    /* Test an embedded nil pointer to a struct with pointer methods. */
    rt = reflect_struct_of(
        FIELDS(sf("SettableStruct", "", reflect_pointer_to(&ty_SettableStruct), true)));
    e.v = new_elem(a, rt);
    if (!reflect_type_implements(rt, &ty_IfaceSet))
        testing_t_errorf_v(t, "%s does not implement IfaceSet",
                           reflect_type_string(rt));
    /* This should panic since the pointer is nil. */
    should_panic(t, "", call_method, &e);

    /* The behavior is different if there is a second field, since now an
     * interface value holds a pointer to the struct rather than just holding a
     * copy of the struct. With the current implementation this is expected to
     * panic. */
    ReflectType empty =
        reflect_struct_of((Slice){NULL, 0, 0, TYPE_REFLECT_STRUCT_FIELD});
    should_panic_struct_of(
        t, "",
        FIELDS(sf("SettableStruct", "", reflect_pointer_to(&ty_SettableStruct), true),
               sf("EmptyStruct", "", empty, true)));

    /* Embed a field that can be stored directly in an interface, with a second
     * field. With the current implementation this is expected to panic. */
    should_panic_struct_of(t, "",
                           FIELDS(sf("SettablePointer", "", &ty_SettablePointer, true),
                                  sf("EmptyStruct", "", empty, true)));

    arena_free(&ar);
}

/* ------------------------------------------------ TestStructOfTooManyFields */

static void TestStructOfTooManyFields(TestingT *t) {
    /* Bug Fix: #25402 - this should not panic */
    ReflectType tt = reflect_struct_of(FIELDS(sf("Time", "", TYPE_TIME, true)));

    /* Go looks for After. burrow's time.Time lists the methods its encoders
     * use, which After is not one of, so this looks for one that is. */
    ReflectMethod m;
    if (!reflect_type_method_by_name(tt, BURROW_S("IsZero"), &m))
        testing_t_errorf_v(t, "Expected method `IsZero` to be found");
}

/* --------------------------------------------- TestStructOfDifferentPkgPath */

static void TestStructOfDifferentPkgPath(TestingT *t) {
    should_panic_struct_of(
        t, "different PkgPath",
        FIELDS(sf("f1", "p1", TYPE_INT, false), sf("f2", "p2", TYPE_INT, false)));
}

/* ------------------------------------------------------ TestStructOfTooLarge */

static void TestStructOfTooLarge(TestingT *t) {
    ReflectType t1 = TYPE_BYTE;
    ReflectType t2 = TYPE_INT16;
    ReflectType t4 = TYPE_INT32;
    ReflectType t0 = reflect_array_of(0, t1);

    /* 2^32-3 sized type, as on Go's 32-bit archs */
    ReflectType big_type = reflect_struct_of(
        FIELDS(sf("F1", "", reflect_array_of(INT32_MAX, t1), false),
               sf("F2", "", reflect_array_of(INT32_MAX - 1, t1), false)));

    /* 2^32-1, ok */
    ReflectStructField c0[] = {sf("F1", "", big_type, false),
                               sf("F2", "", reflect_array_of(2, t1), false)};
    /* overflow in total size */
    ReflectStructField c1[] = {sf("F1", "", big_type, false),
                               sf("F2", "", reflect_array_of(3, t1), false)};
    /* overflow while aligning F2 */
    ReflectStructField c2[] = {sf("F1", "", big_type, false), sf("F2", "", t4, false)};
    /* overflow while adding trailing byte for zero-sized fields */
    ReflectStructField c3[] = {sf("F1", "", big_type, false),
                               sf("F2", "", reflect_array_of(2, t1), false),
                               sf("F3", "", t0, false)};
    /* overflow while aligning total size */
    ReflectStructField c4[] = {sf("F1", "", t2, false), sf("F2", "", big_type, false)};

    struct {
        bool should_panic;
        Slice fields;
    } tests[] = {
        {false, FIELD_SLICE(c0)}, {true, FIELD_SLICE(c1)}, {true, FIELD_SLICE(c2)},
        {true, FIELD_SLICE(c3)},  {true, FIELD_SLICE(c4)},
    };

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        char buf[256];
        bool got = panics(struct_of, &tests[i].fields, buf, sizeof buf);
        if (!tests[i].should_panic) {
            if (got)
                testing_t_errorf_v(t, "test %d should not panic, got %s", i, buf);
            continue;
        }
        if (!got) {
            testing_t_errorf_v(t, "test %d expected to panic", i);
            continue;
        }
        if (strcmp(buf, "reflect.StructOf: struct size would exceed virtual address "
                        "space") != 0)
            testing_t_errorf_v(t, "test %d wrong panic message: %s", i, buf);
    }
}

/* --------------------------------------------------- TestStructOfAnonymous */

/* type D1 struct{ d int }, and struct{ D1 } */
typedef struct D1 {
    Int d;
} D1;

static const Field d1_fields[] = {{S_("d"), S_(""), TYPE_INT, 0}};
static const Type ty_D1 = DESC(S_("D1"), TEST_PKG, KIND_STRUCT, sizeof(D1),
                               _Alignof(D1), 1, d1_fields, 0, NULL, NULL);
static const Field embeds_d1_fields[] = {{S_("D1"), S_(""), &ty_D1, 0}};
static const Type ty_embeds_D1 = DESC(NONAME, NONAME, KIND_STRUCT, sizeof(D1),
                                      _Alignof(D1), 1, embeds_d1_fields, 0, NULL, NULL);

static void TestStructOfAnonymous(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    ReflectStructField f = reflect_type_field(&ty_embeds_D1, 0);
    ReflectType ds = reflect_struct_of(slice_from(&f, 1, 1, TYPE_REFLECT_STRUCT_FIELD));
    ReflectValue dt = new_elem(a, ds);
    check_same_type(t, reflect_value_type(dt), &ty_embeds_D1);

    D1 out;
    if (!reflect_type_assert(dt, &ty_embeds_D1, &out))
        testing_t_errorf_v(t, "%s does not assert to struct{ D1 }",
                           reflect_type_string(ds));

    arena_free(&ar);
}

/* ------------------------------------- TestStructOfEmbeddedIfaceMethodCall */

/* type Named interface{ Name() string } */
typedef struct NamedVT {
    const Type *self_type;
    Str (*name)(void *self);
} NamedVT;

typedef struct Named {
    const NamedVT *vt;
    void *data;
} Named;

#define SIG_Name(IN, OUT) OUT(Str)
#define NAMED_METHODS(X, I) X(I, Name, name, SIG_Name)
NAMED_METHODS(BURROW__IFACE_METHOD_DEFS, Named)
static const Method named_methods[] = {NAMED_METHODS(BURROW__METHOD_ENTRY, Named)};
static const Type ty_Named = DESC(S_("Named"), TEST_PKG, KIND_INTERFACE, sizeof(Named),
                                  _Alignof(Named), 0, NULL, 1, named_methods, NULL);

/* What goes into the Named field. Go's test puts reflect.TypeFor[string]()
 * there, whose Name is "string", and this stands in for it. */
typedef struct NameString {
    Str s;
} NameString;

static Str name_string_name(void *self) {
    return ((const NameString *)self)->s;
}

#define NAME_STRING_METHODS(X, T) X(T, Name, name_string_name, SIG_Name)
BURROW_METHODS_DEFINE(NameString, NAME_STRING_METHODS);
static const Field name_string_fields[] = {{S_("s"), S_(""), TYPE_STRING, 0}};
static const Type ty_NameString = DESC(
    S_("nameString"), TEST_PKG, KIND_STRUCT, sizeof(NameString), _Alignof(NameString),
    1, name_string_fields, 1, burrow__methods_NameString, NULL);
static const NamedVT name_string_named = {&ty_NameString, name_string_name};
BURROW_IMPLEMENTS_DESC(name_string_as_named, &ty_NameString, &ty_Named,
                       name_string_named);

static void TestStructOfEmbeddedIfaceMethodCall(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    ReflectType typ = reflect_struct_of(FIELDS(sf("Named", "", &ty_Named, true)));

    ReflectValue v = new_elem(a, typ);
    NameString s = {BURROW_S("string")};
    reflect_value_set(reflect_value_field(v, 0), val(&ty_NameString, &s));

    CallEnv e = {v, a, "Name", NOVALS};
    should_panic(t, "StructOf does not support methods of embedded interfaces",
                 call_method, &e);

    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestStructOfFieldName)                                                           \
    X(TestStructOf)                                                                    \
    X(TestStructOfExportRules)                                                         \
    X(TestStructOfGC)                                                                  \
    X(TestStructOfAlg)                                                                 \
    X(TestStructOfGenericAlg)                                                          \
    X(TestStructOfWithInterface)                                                       \
    X(TestStructOfTooManyFields)                                                       \
    X(TestStructOfDifferentPkgPath)                                                    \
    X(TestStructOfTooLarge)                                                            \
    X(TestStructOfAnonymous)                                                           \
    X(TestStructOfEmbeddedIfaceMethodCall)

TESTING_MAIN(TESTS)
