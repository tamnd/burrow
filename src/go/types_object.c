/* go/types: objects, scopes and packages.
 *
 * Derived from Go's src/go/types/object.go, objset.go, scope.go, scope2.go
 * and package.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static void *gt_obj_at(Slice s, Int i) {
    return *(void **)slice_at(s, i);
}

static BURROW_NORETURN void gt_nil_deref(void) {
    panic_str(BURROW_S("runtime error: invalid memory address or nil pointer "
                       "dereference"));
}

/* -------------------------------------------------------------- downcasts */

#define GT_AS(T, KIND)                                                                 \
    return obj != NULL && obj->kind == (KIND) ? (T *)(void *)obj : NULL

TypesPkgName *types_as_pkg_name(TypesObject obj) {
    GT_AS(TypesPkgName, TYPES_OBJECT_PKG_NAME);
}
TypesConst *types_as_const(TypesObject obj) {
    GT_AS(TypesConst, TYPES_OBJECT_CONST);
}
TypesTypeName *types_as_type_name(TypesObject obj) {
    GT_AS(TypesTypeName, TYPES_OBJECT_TYPE_NAME);
}
TypesVar *types_as_var(TypesObject obj) {
    GT_AS(TypesVar, TYPES_OBJECT_VAR);
}
TypesFunc *types_as_func(TypesObject obj) {
    GT_AS(TypesFunc, TYPES_OBJECT_FUNC);
}
TypesLabel *types_as_label(TypesObject obj) {
    GT_AS(TypesLabel, TYPES_OBJECT_LABEL);
}
TypesBuiltin *types_as_builtin(TypesObject obj) {
    GT_AS(TypesBuiltin, TYPES_OBJECT_BUILTIN);
}
TypesNil *types_as_nil(TypesObject obj) {
    GT_AS(TypesNil, TYPES_OBJECT_NIL);
}

#undef GT_AS

/* ----------------------------------------------------------------- objects */

bool burrow__types_is_exported(Str name) {
    Int size;
    Rune ch = utf8_decode_rune_in_string(name, &size);
    return unicode_is_upper(ch);
}

/* The path part of Id(pkg, name) for an unexported name. */
static Str gt_id_path(TypesPackage *pkg) {
    if (pkg != NULL && pkg->path.len != 0)
        return pkg->path;
    return BURROW_S("_");
}

Str types_id(Alloc *a, TypesPackage *pkg, Str name) {
    if (burrow__types_is_exported(name))
        return name;
    Str path = gt_id_path(pkg);
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_buf_str(&buf, path);
    burrow__types_buf_byte(&buf, '.');
    burrow__types_buf_str(&buf, name);
    return burrow__types_buf_done(&buf, a);
}

/* The parts Id(pkg, name) is made of, with p[0] and p[1] empty for an
 * exported name. */
static void gt_id_parts(TypesPackage *pkg, Str name, Str p[3]) {
    if (burrow__types_is_exported(name)) {
        p[0] = BURROW_STR_EMPTY;
        p[1] = BURROW_STR_EMPTY;
    } else {
        p[0] = gt_id_path(pkg);
        p[1] = BURROW_S(".");
    }
    p[2] = name;
}

bool burrow__types_id_eq(TypesPackage *pkg1, Str name1, TypesPackage *pkg2, Str name2) {
    Str x[3], y[3];
    gt_id_parts(pkg1, name1, x);
    gt_id_parts(pkg2, name2, y);
    if (x[0].len + x[1].len + x[2].len != y[0].len + y[1].len + y[2].len)
        return false;
    /* Walk both concatenations a byte at a time. */
    Int xi = 0, xo = 0, yi = 0, yo = 0;
    for (;;) {
        while (xi < 3 && xo == x[xi].len) {
            xi++;
            xo = 0;
        }
        while (yi < 3 && yo == y[yi].len) {
            yi++;
            yo = 0;
        }
        if (xi == 3 || yi == 3)
            return xi == yi;
        if (x[xi].p[xo] != y[yi].p[yo])
            return false;
        xo++;
        yo++;
    }
}

int burrow__types_id_cmp(TypesPackage *pkg1, Str name1, TypesPackage *pkg2, Str name2) {
    Str x[3], y[3];
    gt_id_parts(pkg1, name1, x);
    gt_id_parts(pkg2, name2, y);
    /* Compare both concatenations a byte at a time, as strings.Compare would
     * compare the two Ids. */
    Int xi = 0, xo = 0, yi = 0, yo = 0;
    for (;;) {
        while (xi < 3 && xo == x[xi].len) {
            xi++;
            xo = 0;
        }
        while (yi < 3 && yo == y[yi].len) {
            yi++;
            yo = 0;
        }
        if (xi == 3 || yi == 3)
            return (yi == 3) - (xi == 3);
        if (x[xi].p[xo] != y[yi].p[yo])
            return x[xi].p[xo] < y[yi].p[yo] ? -1 : +1;
        xo++;
        yo++;
    }
}

TypesScope *types_object_parent(TypesObject obj) {
    return GT_OBJ(obj)->parent;
}

TokenPos types_object_pos(TypesObject obj) {
    return GT_OBJ(obj)->pos;
}

TypesPackage *types_object_pkg(TypesObject obj) {
    return GT_OBJ(obj)->pkg;
}

Str types_object_name(TypesObject obj) {
    return GT_OBJ(obj)->name;
}

TypesType types_object_type(TypesObject obj) {
    return GT_OBJ(obj)->typ;
}

bool types_object_exported(TypesObject obj) {
    return burrow__types_is_exported(GT_OBJ(obj)->name);
}

Str types_object_id(Alloc *a, TypesObject obj) {
    return types_id(a, GT_OBJ(obj)->pkg, GT_OBJ(obj)->name);
}

bool burrow__types_same_id(TypesObject obj, TypesPackage *pkg, Str name,
                           bool fold_case) {
    GtObject *o = GT_OBJ(obj);
    if (fold_case && strings_equal_fold(o->name, name))
        return true;
    if (!str_eq(o->name, name))
        return false;
    if (burrow__types_is_exported(o->name))
        return true;
    return burrow__types_same_pkg(o->pkg, pkg);
}

int burrow__types_object_cmp(TypesObject x, TypesObject y) {
    if (x == y)
        return 0;
    if (x == NULL)
        return -1;
    if (y == NULL)
        return +1;
    GtObject *a = GT_OBJ(x);
    GtObject *b = GT_OBJ(y);
    bool ea = burrow__types_is_exported(a->name);
    bool eb = burrow__types_is_exported(b->name);
    if (ea != eb)
        return ea ? -1 : +1;
    if (!str_eq(a->name, b->name))
        return (int)strings_compare(a->name, b->name);
    if (!ea) {
        if (a->pkg == NULL || b->pkg == NULL)
            gt_nil_deref();
        return (int)strings_compare(a->pkg->path, b->pkg->path);
    }
    return 0;
}

bool burrow__types_same_pkg(TypesPackage *a, TypesPackage *b) {
    if (a == NULL || b == NULL)
        return a == b;
    return str_eq(a->path, b->path);
}

static void gt_init_object(GtObject *o, TypesObjectKind kind, TokenPos pos,
                           TypesPackage *pkg, Str name, TypesType typ) {
    o->hdr.kind = kind;
    o->pos = pos;
    o->pkg = pkg;
    o->name = name;
    o->typ = typ;
}

TypesPkgName *types_new_pkg_name(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                                 TypesPackage *imported) {
    TypesPkgName *obj = GT_NEW(a, TypesPkgName);
    gt_init_object(&obj->obj, TYPES_OBJECT_PKG_NAME, pos, pkg, name,
                   GT_TYP(TYPES_INVALID));
    obj->imported = imported;
    return obj;
}

TypesPackage *types_pkg_name_imported(TypesPkgName *obj) {
    return obj->imported;
}

TypesConst *types_new_const(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                            TypesType typ, ConstantValue val) {
    TypesConst *obj = GT_NEW(a, TypesConst);
    gt_init_object(&obj->obj, TYPES_OBJECT_CONST, pos, pkg, name, typ);
    obj->val = val;
    return obj;
}

ConstantValue types_const_val(TypesConst *obj) {
    return obj->val;
}

TypesTypeName *types_new_type_name(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                                   TypesType typ) {
    TypesTypeName *obj = GT_NEW(a, TypesTypeName);
    gt_init_object(&obj->obj, TYPES_OBJECT_TYPE_NAME, pos, pkg, name, typ);
    return obj;
}

bool types_type_name_is_alias(TypesTypeName *obj) {
    GtObject *o = GT_OBJ(obj);
    TypesType typ = o->typ;
    if (typ == NULL)
        return false;
    switch ((TypesTypeKind)typ->kind) {
    case TYPES_KIND_BASIC: {
        TypesBasic *t = types_as_basic(typ);
        if (o->pkg != NULL && o->pkg == types_unsafe())
            return false;
        return o->pkg != NULL || !str_eq(t->name, o->name) ||
               t == burrow__types_universe_byte || t == burrow__types_universe_rune;
    }
    case TYPES_KIND_NAMED:
        return obj != types_as_named(typ)->obj;
    case TYPES_KIND_TYPE_PARAM:
        return obj != types_as_type_param(typ)->obj;
    case TYPES_KIND_INVALID:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_ARRAY:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_STRUCT:
    case TYPES_KIND_TUPLE:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_INTERFACE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
    case TYPES_KIND_UNION:
    case TYPES_KIND_ALIAS:
    default:
        return true;
    }
}

Str types_var_kind_string(Alloc *a, TypesVarKind kind) {
    static const char *const names[] = {
        "VarKind(0)", "PackageVar", "LocalVar", "RecvVar",
        "ParamVar",   "ResultVar",  "FieldVar",
    };
    if (kind >= 0 && kind < (Int)(sizeof names / sizeof names[0]))
        return str_from_cstr(names[kind]);
    return fmt_sprintf_v(a, "VarKind(%d)", (uint8_t)kind);
}

TypesVar *burrow__types_new_var(Alloc *a, TypesVarKind kind, TokenPos pos,
                                TypesPackage *pkg, Str name, TypesType typ) {
    TypesVar *v = GT_NEW(a, TypesVar);
    gt_init_object(&v->obj, TYPES_OBJECT_VAR, pos, pkg, name, typ);
    v->kind = kind;
    return v;
}

TypesVar *types_new_var(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                        TypesType typ) {
    return burrow__types_new_var(a, TYPES_PACKAGE_VAR, pos, pkg, name, typ);
}

TypesVar *types_new_param(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                          TypesType typ) {
    return burrow__types_new_var(a, TYPES_PARAM_VAR, pos, pkg, name, typ);
}

TypesVar *types_new_field(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                          TypesType typ, bool embedded) {
    TypesVar *v = burrow__types_new_var(a, TYPES_FIELD_VAR, pos, pkg, name, typ);
    v->embedded = embedded;
    return v;
}

TypesVarKind types_var_kind(TypesVar *v) {
    return v->kind;
}

void types_var_set_kind(TypesVar *v, TypesVarKind kind) {
    v->kind = kind;
}

bool types_var_anonymous(TypesVar *v) {
    return v->embedded;
}

bool types_var_embedded(TypesVar *v) {
    return v->embedded;
}

bool types_var_is_field(TypesVar *v) {
    return v->kind == TYPES_FIELD_VAR;
}

TypesVar *types_var_origin(TypesVar *v) {
    if (v->origin != NULL)
        return v->origin;
    return v;
}

TypesVar *burrow__types_clone_var(Alloc *a, TypesVar *v, TypesType typ) {
    TypesVar *copy = GT_NEW(a, TypesVar);
    *copy = *v;
    GT_OBJ(copy)->typ = typ;
    copy->origin = types_var_origin(v);
    return copy;
}

TypesFunc *types_new_func(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name,
                          TypesSignature *sig) {
    TypesFunc *f = GT_NEW(a, TypesFunc);
    gt_init_object(&f->obj, TYPES_OBJECT_FUNC, pos, pkg, name,
                   sig != NULL ? types_type(sig) : NULL);
    return f;
}

/* Go's new(Signature), for a function with no type yet. Nothing writes to it. */
static TypesSignature gt_no_signature = {
    {TYPES_KIND_SIGNATURE}, NULL, NULL, NULL, NULL, NULL, NULL, NULL, false};

TypesSignature *types_func_signature(TypesFunc *obj) {
    TypesType typ = GT_OBJ(obj)->typ;
    if (typ != NULL) {
        TypesSignature *sig = types_as_signature(typ);
        if (sig == NULL)
            panic_str(BURROW_S("interface conversion: types.Type is not "
                               "*types.Signature"));
        return sig;
    }
    return &gt_no_signature;
}

Str types_func_full_name(Alloc *a, TypesFunc *obj) {
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_write_func_name(&buf, obj, TYPES_NO_QUALIFIER);
    return burrow__types_buf_done(&buf, a);
}

TypesScope *types_func_scope(TypesFunc *obj) {
    TypesSignature *sig = types_as_signature(GT_OBJ(obj)->typ);
    if (sig == NULL)
        gt_nil_deref();
    return sig->scope;
}

TypesFunc *types_func_origin(TypesFunc *obj) {
    if (obj->origin != NULL)
        return obj->origin;
    return obj;
}

TypesFunc *burrow__types_clone_func(Alloc *a, TypesFunc *f, TypesType typ) {
    TypesFunc *copy = GT_NEW(a, TypesFunc);
    *copy = *f;
    GT_OBJ(copy)->typ = typ;
    copy->origin = types_func_origin(f);
    return copy;
}

bool burrow__types_func_has_ptr_recv(TypesFunc *obj) {
    TypesSignature *sig = types_as_signature(GT_OBJ(obj)->typ);
    if (sig != NULL && sig->recv != NULL)
        return types_as_pointer(types_unalias(GT_OBJ(sig->recv)->typ)) != NULL;
    return obj->has_ptr_recv;
}

TypesLabel *types_new_label(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name) {
    TypesLabel *l = GT_NEW(a, TypesLabel);
    gt_init_object(&l->obj, TYPES_OBJECT_LABEL, pos, pkg, name, GT_TYP(TYPES_INVALID));
    return l;
}

/* ----------------------------------------------------------------- objsets */

TypesObject burrow__types_objset_insert(GtObjset *s, TypesObject obj) {
    Str id = types_object_id(s->a, obj);
    TypesObject *alt = map_get(s->m, &id);
    if (alt != NULL && *alt != NULL)
        return *alt;
    if (s->m == NULL) {
        s->m = map_make(s->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (s->m == NULL)
            burrow__types_oom();
    }
    if (!map_set(s->m, &id, &obj))
        burrow__types_oom();
    return NULL;
}

/* ------------------------------------------------------------ printing */

void burrow__types_write_package_prefix(BytesBuffer *buf, TypesPackage *pkg,
                                        TypesQualifier qf) {
    if (pkg == NULL)
        return;
    Str s;
    if (!BURROW_FUNC_IS_NIL(qf))
        s = BURROW_CALLF(qf, pkg);
    else
        s = pkg->path;
    if (s.len != 0) {
        burrow__types_buf_str(buf, s);
        burrow__types_buf_byte(buf, '.');
    }
}

void burrow__types_write_object(BytesBuffer *buf, TypesObject obj, TypesQualifier qf) {
    GtObject *o = GT_OBJ(obj);
    TypesTypeName *tname = NULL;
    TypesType typ = o->typ;
    switch ((TypesObjectKind)obj->kind) {
    case TYPES_OBJECT_PKG_NAME: {
        TypesPkgName *p = GT_KNOWN(TypesPkgName, obj);
        burrow__types_buf_str(buf, BURROW_S("package "));
        burrow__types_buf_str(buf, o->name);
        if (p->imported == NULL)
            gt_nil_deref();
        Str path = p->imported->path;
        if (path.len != 0 && !str_eq(path, o->name)) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            burrow__types_buf_str(buf, BURROW_S(" ("));
            burrow__types_buf_str(buf, strconv_quote(arena_allocator(&ar), path));
            burrow__types_buf_byte(buf, ')');
            arena_free(&ar);
        }
        return;
    }
    case TYPES_OBJECT_CONST:
        burrow__types_buf_str(buf, BURROW_S("const"));
        break;
    case TYPES_OBJECT_TYPE_NAME:
        tname = GT_KNOWN(TypesTypeName, obj);
        burrow__types_buf_str(buf, BURROW_S("type"));
        if (burrow__types_is_type_param(typ))
            burrow__types_buf_str(buf, BURROW_S(" parameter"));
        break;
    case TYPES_OBJECT_VAR:
        if (types_var_is_field(GT_KNOWN(TypesVar, obj)))
            burrow__types_buf_str(buf, BURROW_S("field"));
        else
            burrow__types_buf_str(buf, BURROW_S("var"));
        break;
    case TYPES_OBJECT_FUNC:
        burrow__types_buf_str(buf, BURROW_S("func "));
        burrow__types_write_func_name(buf, GT_KNOWN(TypesFunc, obj), qf);
        if (typ != NULL)
            types_write_signature(buf, types_as_signature(typ), qf);
        return;
    case TYPES_OBJECT_LABEL:
        burrow__types_buf_str(buf, BURROW_S("label"));
        typ = NULL;
        break;
    case TYPES_OBJECT_BUILTIN:
        burrow__types_buf_str(buf, BURROW_S("builtin"));
        typ = NULL;
        break;
    case TYPES_OBJECT_NIL:
        burrow__types_buf_str(buf, BURROW_S("nil"));
        return;
    case TYPES_OBJECT_INVALID:
    default:
        panic_str(BURROW_S("writeObject(invalid object)"));
    }

    burrow__types_buf_byte(buf, ' ');
    if (o->pkg != NULL && types_scope_lookup(o->pkg->scope, o->name) == obj)
        burrow__types_write_package_prefix(buf, o->pkg, qf);
    burrow__types_buf_str(buf, o->name);

    if (typ == NULL)
        return;

    if (tname != NULL) {
        if (typ->kind == TYPES_KIND_BASIC)
            return;
        TypesTypeParamList *tparams = NULL;
        if (typ->kind == TYPES_KIND_NAMED)
            tparams = types_named_type_params(types_as_named(typ));
        else if (typ->kind == TYPES_KIND_ALIAS)
            tparams = types_alias_type_params(types_as_alias(typ));
        if (types_type_param_list_len(tparams) > 0) {
            GtTypeWriter w = burrow__types_new_type_writer(buf, qf);
            burrow__types_tw_tparam_list(&w, burrow__types_tparam_list(tparams));
        }
        if (types_type_name_is_alias(tname)) {
            burrow__types_buf_str(buf, BURROW_S(" ="));
            TypesAlias *alias = types_as_alias(typ);
            if (alias != NULL)
                typ = alias->from_rhs;
        } else if (typ->kind == TYPES_KIND_TYPE_PARAM) {
            typ = types_as_type_param(typ)->bound;
        } else {
            typ = types_type_underlying(typ);
        }
    }

    if (str_eq(o->name, BURROW_S("any")) && o->parent != NULL && o->parent->universe) {
        burrow__types_assert(
            types_identical(typ, types_type(burrow__types_empty_interface)));
        typ = types_type(burrow__types_empty_interface);
    }

    burrow__types_buf_byte(buf, ' ');
    types_write_type(buf, typ, qf);
}

Str types_object_string(Alloc *a, TypesObject obj, TypesQualifier qf) {
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_write_object(&buf, obj, qf);
    return burrow__types_buf_done(&buf, a);
}

void burrow__types_write_func_name(BytesBuffer *buf, TypesFunc *f, TypesQualifier qf) {
    TypesType typ = GT_OBJ(f)->typ;
    if (typ != NULL) {
        TypesSignature *sig = types_as_signature(typ);
        TypesVar *recv = sig->recv;
        if (recv != NULL) {
            burrow__types_buf_byte(buf, '(');
            if (types_as_interface(GT_OBJ(recv)->typ) != NULL)
                burrow__types_buf_str(buf, BURROW_S("interface"));
            else
                types_write_type(buf, GT_OBJ(recv)->typ, qf);
            burrow__types_buf_byte(buf, ')');
            burrow__types_buf_byte(buf, '.');
        } else if (GT_OBJ(f)->pkg != NULL) {
            burrow__types_write_package_prefix(buf, GT_OBJ(f)->pkg, qf);
        }
    }
    burrow__types_buf_str(buf, GT_OBJ(f)->name);
}

Str burrow__types_object_kind(TypesObject obj) {
    switch ((TypesObjectKind)(obj != NULL ? obj->kind : TYPES_OBJECT_INVALID)) {
    case TYPES_OBJECT_PKG_NAME:
        return BURROW_S("package name");
    case TYPES_OBJECT_CONST:
        return BURROW_S("constant");
    case TYPES_OBJECT_TYPE_NAME:
        if (types_type_name_is_alias(types_as_type_name(obj)))
            return BURROW_S("type alias");
        if (types_as_type_param(GT_OBJ(obj)->typ) != NULL)
            return BURROW_S("type parameter");
        return BURROW_S("defined type");
    case TYPES_OBJECT_VAR:
        switch (GT_KNOWN(TypesVar, obj)->kind) {
        case TYPES_PACKAGE_VAR:
            return BURROW_S("package-level variable");
        case TYPES_LOCAL_VAR:
            return BURROW_S("local variable");
        case TYPES_RECV_VAR:
            return BURROW_S("receiver");
        case TYPES_PARAM_VAR:
            return BURROW_S("parameter");
        case TYPES_RESULT_VAR:
            return BURROW_S("result variable");
        case TYPES_FIELD_VAR:
            return BURROW_S("struct field");
        default:
            break;
        }
        break;
    case TYPES_OBJECT_FUNC:
        if (types_func_signature(GT_KNOWN(TypesFunc, obj))->recv != NULL)
            return BURROW_S("method");
        return BURROW_S("function");
    case TYPES_OBJECT_LABEL:
        return BURROW_S("label");
    case TYPES_OBJECT_BUILTIN:
        return BURROW_S("built-in function");
    case TYPES_OBJECT_NIL:
        return BURROW_S("untyped nil");
    case TYPES_OBJECT_INVALID:
    default:
        break;
    }
    return BURROW_S("unknown symbol");
}

/* ------------------------------------------------------------------ scopes */

TypesScope *types_new_scope(Alloc *a, TypesScope *parent, TokenPos pos, TokenPos end,
                            Str comment) {
    TypesScope *s = GT_NEW(a, TypesScope);
    s->a = a;
    s->parent = parent;
    s->pos = pos;
    s->end = end;
    s->comment = comment;
    if (parent != NULL && !parent->universe) {
        burrow__types_ptrs_append(parent->a, &parent->children, s);
        s->number = parent->children.len;
    }
    return s;
}

TypesScope *types_scope_parent(TypesScope *s) {
    return s->parent;
}

Int types_scope_len(TypesScope *s) {
    return map_len(s->elems);
}

Slice types_scope_names(Alloc *a, TypesScope *s) {
    Int n = map_len(s->elems);
    Slice names = slice_make(a, TYPE_STRING, n, n);
    if (n > 0 && names.p == NULL)
        burrow__types_oom();
    MapIter it = map_iter(s->elems);
    const void *k;
    void *v;
    Int i = 0;
    while (map_next(&it, &k, &v))
        ((Str *)names.p)[i++] = *(const Str *)k;
    sort_strings(names);
    return names;
}

Int types_scope_num_children(TypesScope *s) {
    return s->children.len;
}

TypesScope *types_scope_child(TypesScope *s, Int i) {
    return gt_obj_at(s->children, i);
}

TypesObject types_scope_lookup(TypesScope *s, Str name) {
    TypesObject *obj = map_get(s->elems, &name);
    return obj != NULL ? *obj : NULL;
}

TypesObject types_scope_insert(TypesScope *s, TypesObject obj) {
    Str name = GT_OBJ(obj)->name;
    TypesObject alt = types_scope_lookup(s, name);
    if (alt != NULL)
        return alt;
    if (s->elems == NULL) {
        s->elems = map_make(s->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (s->elems == NULL)
            burrow__types_oom();
    }
    if (!map_set(s->elems, &name, &obj))
        burrow__types_oom();
    if (GT_OBJ(obj)->parent == NULL)
        GT_OBJ(obj)->parent = s;
    return NULL;
}

TypesObject types_scope_lookup_parent(TypesScope *s, Str name, TokenPos pos,
                                      TypesScope **scope) {
    for (; s != NULL; s = s->parent) {
        TypesObject obj = types_scope_lookup(s, name);
        if (obj != NULL && (pos == TOKEN_NO_POS || GT_OBJ(obj)->scope_pos <= pos)) {
            if (scope != NULL)
                *scope = s;
            return obj;
        }
    }
    if (scope != NULL)
        *scope = NULL;
    return NULL;
}

TokenPos types_scope_pos(TypesScope *s) {
    return s->pos;
}

TokenPos types_scope_end(TypesScope *s) {
    return s->end;
}

bool types_scope_contains(TypesScope *s, TokenPos pos) {
    return s->pos <= pos && pos < s->end;
}

TypesScope *types_scope_innermost(TypesScope *s, TokenPos pos) {
    if (s->parent != NULL && s->parent->universe) {
        for (Int i = 0; i < s->children.len; i++) {
            TypesScope *inner = types_scope_innermost(gt_obj_at(s->children, i), pos);
            if (inner != NULL)
                return inner;
        }
    }
    if (types_scope_contains(s, pos)) {
        for (Int i = 0; i < s->children.len; i++) {
            TypesScope *c = gt_obj_at(s->children, i);
            if (types_scope_contains(c, pos))
                return types_scope_innermost(c, pos);
        }
        return s;
    }
    return NULL;
}

void types_scope_write_to(TypesScope *s, IoWriter w, Int n, bool recurse) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str ind = BURROW_S(".  ");
    Str indn = strings_repeat(a, ind, n);
    (void)fmt_fprintf_v(w, "%s%s scope %p {\n", indn, s->comment, (void *)s);
    Str indn1 = strings_repeat(a, ind, n + 1);
    Slice names = types_scope_names(a, s);
    for (Int i = 0; i < names.len; i++) {
        Str name = ((Str *)names.p)[i];
        Str os =
            types_object_string(a, types_scope_lookup(s, name), TYPES_NO_QUALIFIER);
        (void)fmt_fprintf_v(w, "%s%s\n", indn1, os);
    }
    if (recurse) {
        for (Int i = 0; i < s->children.len; i++)
            types_scope_write_to(gt_obj_at(s->children, i), w, n + 1, recurse);
    }
    (void)fmt_fprintf_v(w, "%s}\n", indn);
    arena_free(&ar);
}

Str types_scope_string(Alloc *a, TypesScope *s) {
    BytesBuffer buf = BYTES_BUFFER(a);
    types_scope_write_to(s, bytes_buffer_as_io_writer(&buf), 0, false);
    return burrow__types_buf_done(&buf, a);
}

/* ---------------------------------------------------------------- packages */

TypesPackage *types_new_package(Alloc *a, Str path, Str name) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str q = strconv_quote(arena_allocator(&ar), path);
    Str comment = fmt_sprintf_v(a, "package %s", q);
    arena_free(&ar);
    TypesPackage *pkg = GT_NEW(a, TypesPackage);
    pkg->path = path;
    pkg->name = name;
    pkg->scope =
        types_new_scope(a, types_universe(), TOKEN_NO_POS, TOKEN_NO_POS, comment);
    return pkg;
}

Str types_package_path(TypesPackage *pkg) {
    return pkg->path;
}

Str types_package_name(TypesPackage *pkg) {
    return pkg->name;
}

void types_package_set_name(TypesPackage *pkg, Str name) {
    pkg->name = name;
}

Str types_package_go_version(TypesPackage *pkg) {
    return pkg->go_version;
}

TypesScope *types_package_scope(TypesPackage *pkg) {
    if (pkg != NULL)
        return pkg->scope;
    return types_universe();
}

bool types_package_complete(TypesPackage *pkg) {
    return pkg->complete;
}

void types_package_mark_complete(TypesPackage *pkg) {
    pkg->complete = true;
}

Slice types_package_imports(TypesPackage *pkg) {
    return pkg->imports;
}

void types_package_set_imports(TypesPackage *pkg, Slice list) {
    pkg->imports = list;
}

Str types_package_string(Alloc *a, TypesPackage *pkg) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str q = strconv_quote(arena_allocator(&ar), pkg->path);
    Str s = fmt_sprintf_v(a, "package %s (%s)", pkg->name, q);
    arena_free(&ar);
    return s;
}
