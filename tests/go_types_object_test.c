/* Derived from Go's src/go/types/object_test.go. Go source: go1.27.1.
 *
 * TestIsAlias is in tests/go_types_test.c.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* TestEmbeddedMethod checks that an embedded method is represented by
 * the same Func Object as the original method. See also go.dev/issue/34421. */
static void TestEmbeddedMethod(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; type I interface { error }");
    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);

    /* get original error.Error method */
    TypesObject eface = types_scope_lookup(types_universe(), S("error"));
    Slice index;
    bool indirect;
    TypesObject orig = types_lookup_field_or_method(
        a, types_object_type(eface), false, NULL, S("Error"), &index, &indirect);
    if (orig == NULL) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "original error.Error not found");
    }

    /* get embedded error.Error method */
    TypesObject iface = types_scope_lookup(types_package_scope(pkg), S("I"));
    TypesObject embed = types_lookup_field_or_method(
        a, types_object_type(iface), false, NULL, S("Error"), &index, &indirect);
    if (embed == NULL) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "embedded error.Error not found");
    }

    /* original and embedded Error object should be identical */
    if (orig != embed) {
        Str msg = fmt_sprintf_v(
            a, "%s (%p) != %s (%p)", types_object_string(a, orig, TYPES_NO_QUALIFIER),
            (void *)orig, types_object_string(a, embed, TYPES_NO_QUALIFIER),
            (void *)embed);
        testing_t_error_v(t, msg);
    }
    arena_free(&ar);
}

static const struct {
    Str src;
    Str obj;
    Str want;
} test_objects[] = {
    {S_("import \"io\"; var r io.Reader"), S_("r"), S_("var p.r io.Reader")},

    {S_("const c = 1.2"), S_("c"), S_("const p.c untyped float")},
    {S_("const c float64 = 3.14"), S_("c"), S_("const p.c float64")},

    {S_("type t struct{f int}"), S_("t"), S_("type p.t struct{f int}")},
    {S_("type t func(int)"), S_("t"), S_("type p.t func(int)")},
    {S_("type t[P any] struct{f P}"), S_("t"), S_("type p.t[P any] struct{f P}")},
    {S_("type t[P any] struct{f P}"), S_("t.P"), S_("type parameter P any")},
    {S_("type C interface{m()}; type t[P C] struct{}"), S_("t.P"),
     S_("type parameter P p.C")},

    {S_("type t = struct{f int}"), S_("t"), S_("type p.t = struct{f int}")},
    {S_("type t = func(int)"), S_("t"), S_("type p.t = func(int)")},
    {S_("type A = B; type B = int"), S_("A"), S_("type p.A = p.B")},
    {S_("type A[P ~int] = struct{}"), S_("A"), S_("type p.A[P ~int] = struct{}")},
    {S_("var v int"), S_("v"), S_("var p.v int")},

    {S_("func f(int) string"), S_("f"), S_("func p.f(int) string")},
    {S_("func g[P any](x P){}"), S_("g"), S_("func p.g[P any](x P)")},
    {S_("func g[P interface{~int}](x P){}"), S_("g.P"),
     S_("type parameter P interface{~int}")},
    {S_(""), S_("any"), S_("type any = interface{}")},
};

static TypesObject lookup_type_param_obj(TypesTypeParamList *list, Str name) {
    for (Int i = 0; i < types_type_param_list_len(list); i++) {
        TypesTypeParam *tpar = types_type_param_list_at(list, i);
        TypesObject obj = types_object(types_type_param_obj(tpar));
        if (str_eq(types_object_name(obj), name))
            return obj;
    }
    return NULL;
}

/* Not in Go: the TypeParams of the types that have the method, which Go finds
 * with an interface assertion. ok is false for the others. */
static TypesTypeParamList *type_params_of(TypesType typ, bool *ok) {
    *ok = true;
    TypesNamed *n = types_as_named(typ);
    if (n != NULL)
        return types_named_type_params(n);
    TypesAlias *al = types_as_alias(typ);
    if (al != NULL)
        return types_alias_type_params(al);
    TypesSignature *sig = types_as_signature(typ);
    if (sig != NULL)
        return types_signature_type_params(sig);
    *ok = false;
    return NULL;
}

typedef struct {
    Int i;
} ObjectStringEnv;

static void object_string_run(void *env, TestingT *t) {
    Int i = ((ObjectStringEnv *)env)->i;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str test_src = test_objects[i].src;
    Str test_obj = test_objects[i].obj;
    Str src = fmt_sprintf_v(a, "package p; %s", test_src);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, NULL, NULL, &err);
    if (BURROW_FAILED(err)) {
        Str msg = fmt_sprintf_v(a, "%s: %s", src, error_text(err));
        testing_t_fatal_v(t, msg);
    }

    Slice names = strings_split(a, test_obj, S("."));
    if (names.len != 1 && names.len != 2)
        testing_t_fatal_v(
            t, fmt_sprintf_v(a, "%s: invalid object path %s", test_src, test_obj));
    Str name0 = ((Str *)names.p)[0];

    TypesObject obj = NULL;
    for (TypesScope *s = types_package_scope(pkg); s != NULL && obj == NULL;
         s = types_scope_parent(s))
        obj = types_scope_lookup(s, name0);
    if (obj == NULL)
        testing_t_fatal_v(t, fmt_sprintf_v(a, "%s: %s not found", test_src, name0));

    if (names.len == 2) {
        bool ok;
        TypesTypeParamList *tparams = type_params_of(types_object_type(obj), &ok);
        if (ok) {
            obj = lookup_type_param_obj(tparams, ((Str *)names.p)[1]);
            if (obj == NULL)
                testing_t_fatal_v(
                    t, fmt_sprintf_v(a, "%s: %s not found", test_src, test_obj));
        } else {
            testing_t_fatal_v(
                t, fmt_sprintf_v(a, "%s: %s has no type parameters", test_src, name0));
        }
    }

    Str got = types_object_string(a, obj, TYPES_NO_QUALIFIER);
    if (!str_eq(got, test_objects[i].want))
        testing_t_errorf_v(t, "%s: got %s, want %s", test_src, got,
                           test_objects[i].want);
    arena_free(&ar);
}

static void TestObjectString(TestingT *t) {
    /* Not in Go: Go checks testenv.MustHaveGoBuild, for the gc importer. burrow
     * imports io from source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import io from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(test_objects); i++) {
        ObjectStringEnv env = {i};
        testing_t_run(t, strconv_itoa(a, i),
                      BURROW_FN(TestingTFunc, object_string_run, &env));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEmbeddedMethod)                                                              \
    X(TestObjectString)

TESTING_MAIN(TESTS)
