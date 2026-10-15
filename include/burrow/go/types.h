/* go/types, the types of Go programs.
 *
 * Go's go/types. This part is the model the type checker works on: the types,
 * the objects that name them, the scopes and packages that hold the objects,
 * and the universe they all start from. A type or an object is a pointer to a
 * struct that starts with a header saying which one it is, the way a node in
 * go/ast does:
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *
 *     TypesType str = types_type(types_typ[TYPES_STRING]);
 *     TypesType m = types_type(types_new_map(a, str, types_type(types_new_slice(a, str))));
 *     Str s = types_type_string(a, m, TYPES_NO_QUALIFIER); // "map[string][]string"
 *
 *     if (m->kind == TYPES_KIND_MAP) {
 *         TypesMap *mt = types_as_map(m);
 *         // ...
 *     }
 *
 *     arena_free(&ar);
 *
 * Go's Type and Object interfaces are TypesType and TypesObject here, and a
 * NULL one is Go's nil. types_type and types_object turn a pointer to any
 * concrete type or object into one, and the types_as_ functions turn one back,
 * giving NULL when it is something else, which is Go's t.(*Named) with the ok
 * form. The structs themselves are opaque, as their fields are in Go, and are
 * read through the functions below.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/types */

#ifndef BURROW_GO_TYPES_H
#define BURROW_GO_TYPES_H

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory. The types and objects of a program point at each other in cycles: a
 * named type has methods whose receivers are the named type. Nothing here can
 * be freed on its own, so make them all in one arena and free the arena when
 * you are done with all of them. Some parts are worked out the first time they
 * are asked for, such as the type set of an interface or the underlying type of
 * a named type, and those allocate in the allocator the type was made in.
 * Strings are views, and whatever they point at has to outlive the types.
 *
 * Running out of memory panics, as it does in Go, because most of these
 * functions have no error to report it through.
 *
 * Threads. As in Go, types can be read from any number of threads once they
 * are made. The parts that are worked out lazily take a lock, but the memory
 * for them still comes from the allocator the type was made in, so use one
 * that is safe to share when more than one thread reads the same types. */

/* ------------------------------------------------------------------- types */

/* Which type a TypesType is, one for each of Go's types that implement Type. */
typedef enum TypesTypeKind {
    TYPES_KIND_INVALID = 0, /* not a kind, the zero value */
    TYPES_KIND_BASIC,
    TYPES_KIND_POINTER,
    TYPES_KIND_ARRAY,
    TYPES_KIND_SLICE,
    TYPES_KIND_STRUCT,
    TYPES_KIND_TUPLE,
    TYPES_KIND_SIGNATURE,
    TYPES_KIND_INTERFACE,
    TYPES_KIND_MAP,
    TYPES_KIND_CHAN,
    TYPES_KIND_NAMED,
    TYPES_KIND_TYPE_PARAM,
    TYPES_KIND_UNION,
    TYPES_KIND_ALIAS
} TypesTypeKind;

/* The header every type starts with. kind holds a TypesTypeKind, in an Int so
 * that the header is aligned like any of them. */
typedef struct TypesTypeBase {
    Int kind;
} TypesTypeBase;

/* types.Type. Underlying and String are types_type_underlying and
 * types_type_string. */
typedef TypesTypeBase *TypesType;

typedef struct TypesBasic TypesBasic;
typedef struct TypesPointer TypesPointer;
typedef struct TypesArray TypesArray;
typedef struct TypesSlice TypesSlice;
typedef struct TypesStruct TypesStruct;
typedef struct TypesTuple TypesTuple;
typedef struct TypesSignature TypesSignature;
typedef struct TypesInterface TypesInterface;
typedef struct TypesMap TypesMap;
typedef struct TypesChan TypesChan;
typedef struct TypesNamed TypesNamed;
typedef struct TypesTypeParam TypesTypeParam;
typedef struct TypesUnion TypesUnion;
typedef struct TypesAlias TypesAlias;

/* A pointer to any of the types above as a TypesType, checked at compile
 * time. A TypesType goes through unchanged. */
#define types_type(x)                                                                  \
    _Generic((x),                                                                      \
        TypesType: (TypesType)(x),                                                     \
        TypesBasic *: (TypesType)(void *)(x),                                          \
        TypesPointer *: (TypesType)(void *)(x),                                        \
        TypesArray *: (TypesType)(void *)(x),                                          \
        TypesSlice *: (TypesType)(void *)(x),                                          \
        TypesStruct *: (TypesType)(void *)(x),                                         \
        TypesTuple *: (TypesType)(void *)(x),                                          \
        TypesSignature *: (TypesType)(void *)(x),                                      \
        TypesInterface *: (TypesType)(void *)(x),                                      \
        TypesMap *: (TypesType)(void *)(x),                                            \
        TypesChan *: (TypesType)(void *)(x),                                           \
        TypesNamed *: (TypesType)(void *)(x),                                          \
        TypesTypeParam *: (TypesType)(void *)(x),                                      \
        TypesUnion *: (TypesType)(void *)(x),                                          \
        TypesAlias *: (TypesType)(void *)(x))

/* t as the type its kind says it is, or NULL if t is NULL or another kind. */
BURROW_BORROWS(ret, t) TypesBasic *types_as_basic(TypesType t);
BURROW_BORROWS(ret, t) TypesPointer *types_as_pointer(TypesType t);
BURROW_BORROWS(ret, t) TypesArray *types_as_array(TypesType t);
BURROW_BORROWS(ret, t) TypesSlice *types_as_slice(TypesType t);
BURROW_BORROWS(ret, t) TypesStruct *types_as_struct(TypesType t);
BURROW_BORROWS(ret, t) TypesTuple *types_as_tuple(TypesType t);
BURROW_BORROWS(ret, t) TypesSignature *types_as_signature(TypesType t);
BURROW_BORROWS(ret, t) TypesInterface *types_as_interface(TypesType t);
BURROW_BORROWS(ret, t) TypesMap *types_as_map(TypesType t);
BURROW_BORROWS(ret, t) TypesChan *types_as_chan(TypesType t);
BURROW_BORROWS(ret, t) TypesNamed *types_as_named(TypesType t);
BURROW_BORROWS(ret, t) TypesTypeParam *types_as_type_param(TypesType t);
BURROW_BORROWS(ret, t) TypesUnion *types_as_union(TypesType t);
BURROW_BORROWS(ret, t) TypesAlias *types_as_alias(TypesType t);

/* Type.Underlying: the underlying type of t. For a named type or a type
 * parameter it is worked out the first time it is asked for, and it is NULL
 * for a named type made with types_new_named and no underlying type yet. */
TypesType types_type_underlying(TypesType t);

/* ------------------------------------------------------------- qualifiers */

typedef struct TypesPackage TypesPackage;

/* types.Qualifier: how a package is written in front of the names it
 * declares when a type or object is printed. The string has to live as long
 * as the call to the printing function does. A nil one writes the package
 * path. */
BURROW_FUNC(TypesQualifier, Str, TypesPackage *pkg);

/* The nil qualifier. */
#define TYPES_NO_QUALIFIER ((TypesQualifier){NULL, NULL})

/* types.RelativeTo: a qualifier that writes nothing for pkg and the path for
 * any other package. A NULL pkg gives the nil qualifier. */
TypesQualifier types_relative_to(TypesPackage *pkg);

/* TypeString and Type.String: t as Go writes it, with packages qualified by
 * qf. A NULL t is "<nil>". */
BURROW_OWNS(ret) Str types_type_string(Alloc *a, TypesType t, TypesQualifier qf);

/* WriteType: types_type_string written to buf. */
void types_write_type(BytesBuffer *buf, TypesType t, TypesQualifier qf);

/* ------------------------------------------------------------ basic types */

/* types.BasicKind: which basic type a TypesBasic is. */
typedef Int TypesBasicKind;

#define TYPES_INVALID ((TypesBasicKind)0) /* type is invalid */

/* predeclared types */
#define TYPES_BOOL ((TypesBasicKind)1)
#define TYPES_INT ((TypesBasicKind)2)
#define TYPES_INT8 ((TypesBasicKind)3)
#define TYPES_INT16 ((TypesBasicKind)4)
#define TYPES_INT32 ((TypesBasicKind)5)
#define TYPES_INT64 ((TypesBasicKind)6)
#define TYPES_UINT ((TypesBasicKind)7)
#define TYPES_UINT8 ((TypesBasicKind)8)
#define TYPES_UINT16 ((TypesBasicKind)9)
#define TYPES_UINT32 ((TypesBasicKind)10)
#define TYPES_UINT64 ((TypesBasicKind)11)
#define TYPES_UINTPTR ((TypesBasicKind)12)
#define TYPES_FLOAT32 ((TypesBasicKind)13)
#define TYPES_FLOAT64 ((TypesBasicKind)14)
#define TYPES_COMPLEX64 ((TypesBasicKind)15)
#define TYPES_COMPLEX128 ((TypesBasicKind)16)
#define TYPES_STRING ((TypesBasicKind)17)
#define TYPES_UNSAFE_POINTER ((TypesBasicKind)18)

/* types for untyped values */
#define TYPES_UNTYPED_BOOL ((TypesBasicKind)19)
#define TYPES_UNTYPED_INT ((TypesBasicKind)20)
#define TYPES_UNTYPED_RUNE ((TypesBasicKind)21)
#define TYPES_UNTYPED_FLOAT ((TypesBasicKind)22)
#define TYPES_UNTYPED_COMPLEX ((TypesBasicKind)23)
#define TYPES_UNTYPED_STRING ((TypesBasicKind)24)
#define TYPES_UNTYPED_NIL ((TypesBasicKind)25)

/* aliases */
#define TYPES_BYTE TYPES_UINT8
#define TYPES_RUNE TYPES_INT32

/* types.BasicInfo: the properties of a basic type, as a set of bits. */
typedef Int TypesBasicInfo;

#define TYPES_IS_BOOLEAN ((TypesBasicInfo)1)
#define TYPES_IS_INTEGER ((TypesBasicInfo)2)
#define TYPES_IS_UNSIGNED ((TypesBasicInfo)4)
#define TYPES_IS_FLOAT ((TypesBasicInfo)8)
#define TYPES_IS_COMPLEX ((TypesBasicInfo)16)
#define TYPES_IS_STRING ((TypesBasicInfo)32)
#define TYPES_IS_UNTYPED ((TypesBasicInfo)64)

#define TYPES_IS_ORDERED (TYPES_IS_INTEGER | TYPES_IS_FLOAT | TYPES_IS_STRING)
#define TYPES_IS_NUMERIC (TYPES_IS_INTEGER | TYPES_IS_FLOAT | TYPES_IS_COMPLEX)
#define TYPES_IS_CONST_TYPE (TYPES_IS_BOOLEAN | TYPES_IS_NUMERIC | TYPES_IS_STRING)

/* types.Typ: the predeclared basic types, indexed by kind. There is one of
 * each and they live as long as the program does. */
extern TypesBasic *const types_typ[26];

TypesBasicKind types_basic_kind(TypesBasic *b);
TypesBasicInfo types_basic_info(TypesBasic *b);
BURROW_BORROWS(ret, b) Str types_basic_name(TypesBasic *b);

/* --------------------------------------------------- composite type literals */

typedef struct TypesVar TypesVar;
typedef struct TypesFunc TypesFunc;
typedef struct TypesTypeName TypesTypeName;
typedef struct TypesScope TypesScope;
typedef struct TypesTypeParamList TypesTypeParamList;
typedef struct TypesTypeList TypesTypeList;

/* NewPointer: *elem. */
BURROW_OWNS(ret) TypesPointer *types_new_pointer(Alloc *a, TypesType elem);
TypesType types_pointer_elem(TypesPointer *p);

/* NewArray: [len]elem. A negative len is an array whose length is not known,
 * which is what [...]T is until the elements have been counted. */
BURROW_OWNS(ret) TypesArray *types_new_array(Alloc *a, TypesType elem, int64_t len);
int64_t types_array_len(TypesArray *t);
TypesType types_array_elem(TypesArray *t);

/* NewSlice: []elem. */
BURROW_OWNS(ret) TypesSlice *types_new_slice(Alloc *a, TypesType elem);
TypesType types_slice_elem(TypesSlice *s);

/* NewMap: map[key]elem. */
BURROW_OWNS(ret) TypesMap *types_new_map(Alloc *a, TypesType key, TypesType elem);
TypesType types_map_key(TypesMap *m);
TypesType types_map_elem(TypesMap *m);

/* types.ChanDir: which ways a channel can carry values. */
typedef Int TypesChanDir;

#define TYPES_SEND_RECV ((TypesChanDir)0)
#define TYPES_SEND_ONLY ((TypesChanDir)1)
#define TYPES_RECV_ONLY ((TypesChanDir)2)

/* NewChan: a channel of elem that goes the way dir says. */
BURROW_OWNS(ret) TypesChan *types_new_chan(Alloc *a, TypesChanDir dir, TypesType elem);
TypesChanDir types_chan_dir(TypesChan *c);
TypesType types_chan_elem(TypesChan *c);

/* NewStruct: a struct with the given fields, of TypesVar *, and tags, of Str,
 * which may be shorter than the fields and is empty when there are none. Two
 * fields with the same name panic, and so do more tags than fields. The
 * slices are kept, not copied. */
BURROW_OWNS(ret) TypesStruct *types_new_struct(Alloc *a, Slice fields, Slice tags);
Int types_struct_num_fields(TypesStruct *s);
BURROW_BORROWS(ret, s) TypesVar *types_struct_field(TypesStruct *s, Int i);
/* Struct.Tag: the tag of field i, or the empty string if it has none. */
BURROW_BORROWS(ret, s) Str types_struct_tag(TypesStruct *s, Int i);

/* NewTuple: an ordered list of variables, of TypesVar *, as the parameters or
 * results of a function have. An empty list gives NULL, which is the empty
 * tuple, and every function here takes NULL as one. The slice is kept. */
BURROW_OWNS(ret) TypesTuple *types_new_tuple(Alloc *a, Slice vars);
Int types_tuple_len(TypesTuple *t);
BURROW_BORROWS(ret, t) TypesVar *types_tuple_at(TypesTuple *t, Int i);

/* NewSignatureType: a function type. recv is the receiver of a method or NULL
 * for a function, and recv_type_params and type_params are of
 * TypesTypeParam *, which may each be empty. A variadic signature has to have
 * at least one parameter, and the last one has to be a slice or a string, or
 * this panics. So does a list of receiver type parameters without a receiver,
 * or a type parameter that is already bound to another list. */
BURROW_OWNS(ret) TypesSignature *
types_new_signature_type(Alloc *a, TypesVar *recv, Slice recv_type_params,
                         Slice type_params, TypesTuple *params, TypesTuple *results,
                         bool variadic);

/* NewSignature: a signature without type parameters. Deprecated in Go in
 * favour of NewSignatureType. */
BURROW_OWNS(ret) TypesSignature *types_new_signature(Alloc *a, TypesVar *recv,
                                                     TypesTuple *params,
                                                     TypesTuple *results,
                                                     bool variadic);

BURROW_BORROWS(ret, s) TypesVar *types_signature_recv(TypesSignature *s);
BURROW_BORROWS(ret, s) TypesTypeParamList *
types_signature_type_params(TypesSignature *s);
BURROW_BORROWS(ret, s) TypesTypeParamList *
types_signature_recv_type_params(TypesSignature *s);
BURROW_BORROWS(ret, s) TypesTuple *types_signature_params(TypesSignature *s);
BURROW_BORROWS(ret, s) TypesTuple *types_signature_results(TypesSignature *s);
bool types_signature_variadic(TypesSignature *s);

/* WriteSignature: sig as Go writes it, without the leading "func". */
void types_write_signature(BytesBuffer *buf, TypesSignature *sig, TypesQualifier qf);

/* ------------------------------------------------------------- interfaces */

/* NewInterfaceType: an interface with the given methods, of TypesFunc *, and
 * embedded types, of TypesType. A method whose signature has no receiver gets
 * one, of the new interface type. The methods are sorted in place, and the
 * slices are kept. With neither, this is the empty interface, which is shared
 * and must not be changed. */
BURROW_OWNS(ret) TypesInterface *types_new_interface_type(Alloc *a, Slice methods,
                                                          Slice embeddeds);

/* NewInterface: the same with embeddeds of TypesNamed *. Deprecated in Go in
 * favour of NewInterfaceType. */
BURROW_OWNS(ret) TypesInterface *types_new_interface(Alloc *a, Slice methods,
                                                     Slice embeddeds);

/* Interface.MarkImplicit: marks t as the wrapper of a type set literal, the
 * interface around ~int or A|B in a constraint. */
void types_interface_mark_implicit(TypesInterface *t);

Int types_interface_num_explicit_methods(TypesInterface *t);
BURROW_BORROWS(ret, t) TypesFunc *types_interface_explicit_method(TypesInterface *t,
                                                                  Int i);
Int types_interface_num_embeddeds(TypesInterface *t);
/* Interface.Embedded: embedded type i if it is a named type, and NULL if it
 * is not. Deprecated in Go in favour of EmbeddedType. */
BURROW_BORROWS(ret, t) TypesNamed *types_interface_embedded(TypesInterface *t, Int i);
TypesType types_interface_embedded_type(TypesInterface *t, Int i);

/* The methods of t's type set, the embedded ones included, sorted by their
 * Id. */
Int types_interface_num_methods(TypesInterface *t);
BURROW_BORROWS(ret, t) TypesFunc *types_interface_method(TypesInterface *t, Int i);

/* Interface.Empty: t's type set is every type, which is interface{} and the
 * interfaces that come to the same thing. */
bool types_interface_empty(TypesInterface *t);
/* Interface.IsComparable: every type in t's type set is comparable. */
bool types_interface_is_comparable(TypesInterface *t);
/* Interface.IsMethodSet: t is described entirely by its methods. */
bool types_interface_is_method_set(TypesInterface *t);
bool types_interface_is_implicit(TypesInterface *t);

/* Interface.Complete: works out t's type set, which has to be done before t is
 * used from more than one thread if t was made by hand. Returns t. */
BURROW_BORROWS(ret, t) TypesInterface *types_interface_complete(TypesInterface *t);

/* -------------------------------------------------------- unions and terms */

typedef struct TypesTerm TypesTerm;

/* NewTerm: ~typ if tilde is set, and typ if not. */
BURROW_OWNS(ret) TypesTerm *types_new_term(Alloc *a, bool tilde, TypesType typ);
bool types_term_tilde(TypesTerm *t);
TypesType types_term_type(TypesTerm *t);
/* Term.String: "~int", "int", and for the special terms of a type set, "∅"
 * for a NULL term and "𝓤" for one with no type. */
BURROW_OWNS(ret) Str types_term_string(Alloc *a, TypesTerm *t);

/* NewUnion: the union of terms, of TypesTerm *, which must not be empty. The
 * slice is kept. */
BURROW_OWNS(ret) TypesUnion *types_new_union(Alloc *a, Slice terms);
Int types_union_len(TypesUnion *u);
BURROW_BORROWS(ret, u) TypesTerm *types_union_term(TypesUnion *u, Int i);

/* -------------------------------------------------------- type parameters */

/* NewTypeParam: a type parameter named by obj with the given constraint. If
 * obj has no type yet it is given this one. The constraint may be NULL for
 * now, and then has to be set with types_type_param_set_constraint before
 * the type parameter is used. */
BURROW_OWNS(ret) TypesTypeParam *types_new_type_param(Alloc *a, TypesTypeName *obj,
                                                      TypesType constraint);
BURROW_BORROWS(ret, t) TypesTypeName *types_type_param_obj(TypesTypeParam *t);
/* TypeParam.Index: where t is in the list it is bound to, or -1 if it is not
 * bound to one yet. */
Int types_type_param_index(TypesTypeParam *t);
TypesType types_type_param_constraint(TypesTypeParam *t);
/* TypeParam.SetConstraint: panics on a NULL bound. */
void types_type_param_set_constraint(TypesTypeParam *t, TypesType bound);

/* TypeParamList: the type parameters of a generic type or function. A NULL
 * list is an empty one. */
Int types_type_param_list_len(TypesTypeParamList *l);
BURROW_BORROWS(ret, l) TypesTypeParam *types_type_param_list_at(TypesTypeParamList *l,
                                                                Int i);
BURROW_OWNS(ret) Str types_type_param_list_string(Alloc *a, TypesTypeParamList *l);

/* TypeList: the type arguments of an instance. A NULL list is an empty one. */
Int types_type_list_len(TypesTypeList *l);
TypesType types_type_list_at(TypesTypeList *l, Int i);
BURROW_OWNS(ret) Str types_type_list_string(Alloc *a, TypesTypeList *l);

/* ------------------------------------------------------------- named types */

/* NewNamed: the type obj declares, with the given underlying type and
 * methods, of TypesFunc *, which the type keeps. If obj has no type yet it is
 * given this one. underlying may be NULL and set later with
 * types_named_set_underlying, and must not itself be a named type. */
BURROW_OWNS(ret) TypesNamed *types_new_named(Alloc *a, TypesTypeName *obj,
                                             TypesType underlying, Slice methods);

/* Named.Obj: the type name of t, or of the type it is an instance of. */
BURROW_BORROWS(ret, t) TypesTypeName *types_named_obj(TypesNamed *t);
/* Named.Origin: the generic type t is an instance of, or t if it is not one. */
BURROW_BORROWS(ret, t) TypesNamed *types_named_origin(TypesNamed *t);
BURROW_BORROWS(ret, t) TypesTypeParamList *types_named_type_params(TypesNamed *t);
/* Named.SetTypeParams: binds tparams, of TypesTypeParam *, to t. */
void types_named_set_type_params(TypesNamed *t, Slice tparams);
/* Named.TypeArgs: the type arguments of an instance, or NULL. */
BURROW_BORROWS(ret, t) TypesTypeList *types_named_type_args(TypesNamed *t);
Int types_named_num_methods(TypesNamed *t);
BURROW_BORROWS(ret, t) TypesFunc *types_named_method(TypesNamed *t, Int i);
/* Named.SetUnderlying: u must not be NULL or a named type. */
void types_named_set_underlying(TypesNamed *t, TypesType u);
/* Named.AddMethod: adds m unless t already has a method with its name. */
void types_named_add_method(TypesNamed *t, TypesFunc *m);

/* ----------------------------------------------------------------- aliases */

/* NewAlias: the alias obj declares for rhs. If obj has no type yet it is
 * given this one. */
BURROW_OWNS(ret) TypesAlias *types_new_alias(Alloc *a, TypesTypeName *obj,
                                             TypesType rhs);
BURROW_BORROWS(ret, a) TypesTypeName *types_alias_obj(TypesAlias *a);
BURROW_BORROWS(ret, a) TypesAlias *types_alias_origin(TypesAlias *a);
BURROW_BORROWS(ret, a) TypesTypeParamList *types_alias_type_params(TypesAlias *a);
void types_alias_set_type_params(TypesAlias *a, Slice tparams);
BURROW_BORROWS(ret, a) TypesTypeList *types_alias_type_args(TypesAlias *a);
/* Alias.Rhs: the type on the right of the declaration, which may be another
 * alias. */
TypesType types_alias_rhs(TypesAlias *a);

/* Unalias: t with every alias followed to the type at the end, or t if it is
 * not an alias. */
TypesType types_unalias(TypesType t);

/* ----------------------------------------------------------------- objects */

/* Which object a TypesObject is. */
typedef enum TypesObjectKind {
    TYPES_OBJECT_INVALID = 0, /* not a kind, the zero value */
    TYPES_OBJECT_PKG_NAME,
    TYPES_OBJECT_CONST,
    TYPES_OBJECT_TYPE_NAME,
    TYPES_OBJECT_VAR,
    TYPES_OBJECT_FUNC,
    TYPES_OBJECT_LABEL,
    TYPES_OBJECT_BUILTIN,
    TYPES_OBJECT_NIL
} TypesObjectKind;

/* The header every object starts with. */
typedef struct TypesObjectBase {
    Int kind;
} TypesObjectBase;

/* types.Object: a named entity in a program, such as a constant, a type, a
 * variable or a function. */
typedef TypesObjectBase *TypesObject;

typedef struct TypesPkgName TypesPkgName;
typedef struct TypesConst TypesConst;
typedef struct TypesLabel TypesLabel;
typedef struct TypesBuiltin TypesBuiltin;
typedef struct TypesNil TypesNil;

/* A pointer to any of the objects as a TypesObject, checked at compile time. */
#define types_object(x)                                                                \
    _Generic((x),                                                                      \
        TypesObject: (TypesObject)(x),                                                 \
        TypesPkgName *: (TypesObject)(void *)(x),                                      \
        TypesConst *: (TypesObject)(void *)(x),                                        \
        TypesTypeName *: (TypesObject)(void *)(x),                                     \
        TypesVar *: (TypesObject)(void *)(x),                                          \
        TypesFunc *: (TypesObject)(void *)(x),                                         \
        TypesLabel *: (TypesObject)(void *)(x),                                        \
        TypesBuiltin *: (TypesObject)(void *)(x),                                      \
        TypesNil *: (TypesObject)(void *)(x))

/* obj as the object its kind says it is, or NULL. */
BURROW_BORROWS(ret, obj) TypesPkgName *types_as_pkg_name(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesConst *types_as_const(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesTypeName *types_as_type_name(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesVar *types_as_var(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesFunc *types_as_func(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesLabel *types_as_label(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesBuiltin *types_as_builtin(TypesObject obj);
BURROW_BORROWS(ret, obj) TypesNil *types_as_nil(TypesObject obj);

/* Object.Parent: the scope obj is declared in, NULL for methods and fields. */
BURROW_BORROWS(ret, obj) TypesScope *types_object_parent(TypesObject obj);
/* Object.Pos: where the identifier that declares obj is. */
TokenPos types_object_pos(TypesObject obj);
/* Object.Pkg: the package obj belongs to, NULL for labels and for the objects
 * of the universe. */
BURROW_BORROWS(ret, obj) TypesPackage *types_object_pkg(TypesObject obj);
BURROW_BORROWS(ret, obj) Str types_object_name(TypesObject obj);
TypesType types_object_type(TypesObject obj);
/* Object.Exported: the name starts with a capital letter. */
bool types_object_exported(TypesObject obj);
/* Object.Id: types_id of obj's package and name. */
BURROW_OWNS(ret) Str types_object_id(Alloc *a, TypesObject obj);

/* Id: name if it is exported, and the package path, a dot and the name if it
 * is not, with "_" for the path when pkg is NULL or has none. The result is
 * name itself when that is all it is. */
BURROW_OWNS(ret) Str types_id(Alloc *a, TypesPackage *pkg, Str name);

/* ObjectString and Object.String: obj as Go describes it, "var x int" or
 * "func (T).m()", with packages qualified by qf. */
BURROW_OWNS(ret) Str types_object_string(Alloc *a, TypesObject obj, TypesQualifier qf);

/* NewPkgName: the name an import declares for imported. */
BURROW_OWNS(ret) TypesPkgName *types_new_pkg_name(Alloc *a, TokenPos pos,
                                                  TypesPackage *pkg, Str name,
                                                  TypesPackage *imported);
BURROW_BORROWS(ret, obj) TypesPackage *types_pkg_name_imported(TypesPkgName *obj);

/* NewConst: a constant of type typ with the value val. */
BURROW_OWNS(ret) TypesConst *types_new_const(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                             Str name, TypesType typ,
                                             ConstantValue val);
ConstantValue types_const_val(TypesConst *obj);

/* NewTypeName: a type name for typ, which may be NULL until a named type,
 * alias or type parameter made for the name fills it in. */
BURROW_OWNS(ret) TypesTypeName *
types_new_type_name(Alloc *a, TokenPos pos, TypesPackage *pkg, Str name, TypesType typ);
/* TypeName.IsAlias: obj names a type it did not declare. */
bool types_type_name_is_alias(TypesTypeName *obj);

/* types.VarKind: what sort of variable a TypesVar is. */
typedef Int TypesVarKind;

#define TYPES_PACKAGE_VAR ((TypesVarKind)1) /* a package-level variable */
#define TYPES_LOCAL_VAR ((TypesVarKind)2)   /* a local variable */
#define TYPES_RECV_VAR ((TypesVarKind)3)    /* a method receiver variable */
#define TYPES_PARAM_VAR ((TypesVarKind)4)   /* a function parameter variable */
#define TYPES_RESULT_VAR ((TypesVarKind)5)  /* a function result variable */
#define TYPES_FIELD_VAR ((TypesVarKind)6)   /* a struct field */

/* VarKind.String: "PackageVar" and so on, and "VarKind(n)" for any other n. */
BURROW_OWNS(ret) Str types_var_kind_string(Alloc *a, TypesVarKind kind);

/* NewVar, NewParam and NewField: a package-level variable, a parameter, and a
 * struct field, which is embedded if embedded is set. */
BURROW_OWNS(ret) TypesVar *types_new_var(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                         Str name, TypesType typ);
BURROW_OWNS(ret) TypesVar *types_new_param(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                           Str name, TypesType typ);
BURROW_OWNS(ret) TypesVar *types_new_field(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                           Str name, TypesType typ, bool embedded);
TypesVarKind types_var_kind(TypesVar *v);
void types_var_set_kind(TypesVar *v, TypesVarKind kind);
/* Var.Anonymous and Var.Embedded: v is an embedded field. */
bool types_var_anonymous(TypesVar *v);
bool types_var_embedded(TypesVar *v);
bool types_var_is_field(TypesVar *v);
/* Var.Origin: the variable v was instantiated from, or v. */
BURROW_BORROWS(ret, v) TypesVar *types_var_origin(TypesVar *v);

/* NewFunc: a function, or a method if sig has a receiver. sig may be NULL for
 * a method whose signature is not known yet. */
BURROW_OWNS(ret) TypesFunc *types_new_func(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                           Str name, TypesSignature *sig);
/* Func.Signature: the signature of obj, which is an empty one if obj has no
 * type yet. */
BURROW_BORROWS(ret, obj) TypesSignature *types_func_signature(TypesFunc *obj);
/* Func.FullName: the package path or receiver type and the name, as in
 * "fmt.Println" or "(*bytes.Buffer).Write". */
BURROW_OWNS(ret) Str types_func_full_name(Alloc *a, TypesFunc *obj);
/* Func.Scope: the scope of the function's body, NULL for one that was not
 * type checked from source. */
BURROW_BORROWS(ret, obj) TypesScope *types_func_scope(TypesFunc *obj);
/* Func.Origin: the function obj was instantiated from, or obj. */
BURROW_BORROWS(ret, obj) TypesFunc *types_func_origin(TypesFunc *obj);

/* NewLabel: a label of a statement. */
BURROW_OWNS(ret) TypesLabel *types_new_label(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                             Str name);

/* ------------------------------------------------------------------ scopes */

/* NewScope: a scope inside parent, which may be NULL, covering pos to end.
 * comment is only for printing. A scope inside anything but the universe is
 * added to its parent's children. */
BURROW_OWNS(ret) TypesScope *types_new_scope(Alloc *a, TypesScope *parent, TokenPos pos,
                                             TokenPos end, Str comment);
BURROW_BORROWS(ret, s) TypesScope *types_scope_parent(TypesScope *s);
Int types_scope_len(TypesScope *s);
/* Scope.Names: the names declared in s, of Str, sorted. */
BURROW_OWNS(ret) Slice types_scope_names(Alloc *a, TypesScope *s);
Int types_scope_num_children(TypesScope *s);
BURROW_BORROWS(ret, s) TypesScope *types_scope_child(TypesScope *s, Int i);
/* Scope.Lookup: the object named name in s itself, or NULL. */
TypesObject types_scope_lookup(TypesScope *s, Str name);
/* Scope.LookupParent: the first object named name in s or the scopes around
 * it, and the scope it is in, if it was declared before pos or pos is
 * TOKEN_NO_POS. NULL and NULL when there is none. */
TypesObject types_scope_lookup_parent(TypesScope *s, Str name, TokenPos pos,
                                      TypesScope **scope);
/* Scope.Insert: adds obj to s and returns NULL, or returns the object already
 * there with that name and leaves s alone. obj's parent becomes s if it has
 * none. */
TypesObject types_scope_insert(TypesScope *s, TypesObject obj);
TokenPos types_scope_pos(TypesScope *s);
TokenPos types_scope_end(TypesScope *s);
/* Scope.Contains: pos is in the extent of s. */
bool types_scope_contains(TypesScope *s, TokenPos pos);
/* Scope.Innermost: the innermost scope in s that contains pos, or NULL. */
BURROW_BORROWS(ret, s) TypesScope *types_scope_innermost(TypesScope *s, TokenPos pos);
/* Scope.WriteTo: s as text, indented n levels, with the children too if
 * recurse is set. */
void types_scope_write_to(TypesScope *s, IoWriter w, Int n, bool recurse);
BURROW_OWNS(ret) Str types_scope_string(Alloc *a, TypesScope *s);

/* ---------------------------------------------------------------- packages */

/* NewPackage: a package with the given import path and name, and a scope of
 * its own inside the universe. */
BURROW_OWNS(ret) TypesPackage *types_new_package(Alloc *a, Str path, Str name);
BURROW_BORROWS(ret, pkg) Str types_package_path(TypesPackage *pkg);
BURROW_BORROWS(ret, pkg) Str types_package_name(TypesPackage *pkg);
void types_package_set_name(TypesPackage *pkg, Str name);
/* Package.GoVersion: the Go version the package needs, such as "go1.21", or
 * the empty string if that is not known. */
BURROW_BORROWS(ret, pkg) Str types_package_go_version(TypesPackage *pkg);
/* Package.Scope: the package's scope, and the universe for a NULL pkg. */
BURROW_BORROWS(ret, pkg) TypesScope *types_package_scope(TypesPackage *pkg);
bool types_package_complete(TypesPackage *pkg);
void types_package_mark_complete(TypesPackage *pkg);
/* Package.Imports: the packages pkg imports, of TypesPackage *. */
BURROW_BORROWS(ret, pkg) Slice types_package_imports(TypesPackage *pkg);
/* Package.SetImports: the slice is kept. */
void types_package_set_imports(TypesPackage *pkg, Slice list);
/* Package.String: package name ("path"). */
BURROW_OWNS(ret) Str types_package_string(Alloc *a, TypesPackage *pkg);

/* ---------------------------------------------------------------- universe */

/* types.Universe: the scope of the predeclared objects, the types, constants
 * and functions every Go program starts with. */
BURROW_STATIC(ret) TypesScope *types_universe(void);

/* types.Unsafe: the package unsafe. */
BURROW_STATIC(ret) TypesPackage *types_unsafe(void);

/* DefPredeclaredTestFuncs: adds assert and trace to the universe, which only
 * the type checker's own tests use. */
void types_def_predeclared_test_funcs(void);

/* -------------------------------------------------------------- predicates */

/* Identical: x and y are the same type. */
bool types_identical(TypesType x, TypesType y);
/* IdenticalIgnoreTags: the same, with struct tags left out of it. */
bool types_identical_ignore_tags(TypesType x, TypesType y);
/* Comparable: values of t can be compared with ==. */
bool types_comparable(TypesType t);
/* IsInterface: t is an interface, or a type parameter. */
bool types_is_interface(TypesType t);
/* Default: the type an untyped constant of type t gets by default, and t
 * itself for any other type. */
TypesType types_default(TypesType t);

/* ------------------------------------------------------ lookup and methods */

/* SelectionKind: what x.f denotes. */
typedef enum TypesSelectionKind {
    TYPES_FIELD_VAL,   /* x.f is a struct field selector */
    TYPES_METHOD_VAL,  /* x.f is a method selector */
    TYPES_METHOD_EXPR, /* x.f is a method expression */
} TypesSelectionKind;

/* Selection: a selector expression x.f, which is the field or method f of the
 * value or type x and the path of embedded fields to it. */
typedef struct TypesSelection TypesSelection;

/* LookupSelection: x.f where x has type T, or NULL if there is no field or
 * method f, or more than one at the same depth, or f is a method with a
 * pointer receiver and x is neither a pointer nor addressable. The selection
 * is allocated in a. */
BURROW_OWNS(ret) TypesSelection *types_lookup_selection(Alloc *a, TypesType T,
                                                        bool addressable,
                                                        TypesPackage *pkg, Str name);
/* LookupFieldOrMethod: the field or method f of T, with pkg needed for an
 * unexported name. index gets the path to it through embedded fields, made
 * in a, and indirect is set if a pointer was followed on the way. When
 * nothing is found, a non-nil index means the name was ambiguous, and
 * indirect means a method has a pointer receiver that x can't supply. Either
 * out pointer can be NULL. Panics if T is NULL. */
BURROW_BORROWS(ret, T) TypesObject types_lookup_field_or_method(Alloc *a, TypesType T,
                                                                bool addressable,
                                                                TypesPackage *pkg,
                                                                Str name, Slice *index,
                                                                bool *indirect);
/* MissingMethod: a method of T that V lacks, or NULL if V implements T. With
 * is_static false, an interface V only has to agree on the methods it has.
 * wrong_type, which can be NULL, is set when V has the method with the wrong
 * signature or on a pointer receiver. */
BURROW_BORROWS(ret, T) TypesFunc *
types_missing_method(TypesType V, TypesInterface *T, bool is_static, bool *wrong_type);

TypesSelectionKind types_selection_kind(TypesSelection *s);
/* Recv: the type of x. */
BURROW_BORROWS(ret, s) TypesType types_selection_recv(TypesSelection *s);
/* Obj: the field or method f. */
BURROW_BORROWS(ret, s) TypesObject types_selection_obj(TypesSelection *s);
/* Type: the type of x.f. For a method value that is f's signature with x as
 * the receiver, and for a method expression it is a function with x as the
 * first parameter. Those signatures are new, and made in a. */
TypesType types_selection_type(Alloc *a, TypesSelection *s);
/* Index: the path from x to f, as indices of embedded fields and then of f
 * itself. Don't change it. */
BURROW_BORROWS(ret, s) Slice types_selection_index(TypesSelection *s);
/* Indirect: a pointer was followed on the path from x to f. */
bool types_selection_indirect(TypesSelection *s);
/* WriteSelection and SelectionString: s as text, such as "field (T) f int". */
void types_write_selection(BytesBuffer *buf, TypesSelection *s, TypesQualifier qf);
BURROW_OWNS(ret) Str types_selection_string(Alloc *a, TypesSelection *s,
                                            TypesQualifier qf);

/* MethodSet: the methods of a type, sorted by Id. */
typedef struct TypesMethodSet TypesMethodSet;

/* NewMethodSet: the method set of T, made in a. It is never NULL. */
BURROW_OWNS(ret) TypesMethodSet *types_new_method_set(Alloc *a, TypesType T);
Int types_method_set_len(TypesMethodSet *s);
/* At: the i'th method, which panics if i is out of range. */
BURROW_BORROWS(ret, s) TypesSelection *types_method_set_at(TypesMethodSet *s, Int i);
/* Lookup: the method with the given package and name, or NULL. */
BURROW_BORROWS(ret, s) TypesSelection *
types_method_set_lookup(TypesMethodSet *s, TypesPackage *pkg, Str name);
BURROW_OWNS(ret) Str types_method_set_string(Alloc *a, TypesMethodSet *s);

/* ------------------------------------------------------------------- sizes */

/* Sizes: the sizing functions for package unsafe. alignof_ is at least 1.
 * offsetsof gives the offsets of a []*Var's fields, as int64_t in a slice it
 * makes in a, and a negative one means the struct is too large from there on.
 * A negative sizeof_ means the type is too large. */
typedef struct TypesSizesVT {
    const Type *self_type;
    int64_t (*alignof_)(void *self, TypesType T);
    Slice (*offsetsof)(void *self, Alloc *a, Slice fields);
    int64_t (*sizeof_)(void *self, TypesType T);
} TypesSizesVT;

typedef struct TypesSizes {
    const TypesSizesVT *vt;
    void *data;
} TypesSizes;

int64_t types_sizes_alignof(TypesSizes s, TypesType T);
BURROW_OWNS(ret) Slice types_sizes_offsetsof(TypesSizes s, Alloc *a, Slice fields);
int64_t types_sizes_sizeof(TypesSizes s, TypesType T);

/* StdSizes: the Sizes most targets can use. A sized basic type such as int16
 * is its size, a string or interface is two words, a slice is three, an array
 * is laid out like a struct of its elements, a struct ends after its last
 * field, and anything else is a word. Arrays and structs are aligned as the
 * spec says, and everything else on its own size, up to max_align. */
typedef struct TypesStdSizes {
    int64_t word_size; /* in bytes, at least 4 */
    int64_t max_align; /* in bytes, at least 1 */
} TypesStdSizes;

int64_t types_std_sizes_alignof(TypesStdSizes *s, TypesType T);
BURROW_OWNS(ret) Slice types_std_sizes_offsetsof(TypesStdSizes *s, Alloc *a,
                                                 Slice fields);
int64_t types_std_sizes_sizeof(TypesStdSizes *s, TypesType T);
TypesSizes types_std_sizes_as_sizes(TypesStdSizes *s);

/* SizesFor: the Sizes compiler uses for arch, or a nil Sizes (vt NULL) if the
 * pair is not one it knows. compiler is "gc" or "gccgo". For "gc" the
 * architectures are 386, amd64, amd64p32, arm, arm64, loong64, mips, mipsle,
 * mips64, mips64le, ppc64, ppc64le, riscv64, s390x, sparc64 and wasm. The
 * result lives as long as the program and must not be changed. */
TypesSizes types_sizes_for(Str compiler, Str arch);

/* ---------------------------------------------------------------- contexts */

typedef struct TypesContext TypesContext;

/* NewContext: somewhere for instances of generic types to be shared, so that
 * instantiating the same type with the same arguments twice gives the same
 * type both times. */
BURROW_OWNS(ret) TypesContext *types_new_context(Alloc *a);

#ifdef __cplusplus
}
#endif

#endif
