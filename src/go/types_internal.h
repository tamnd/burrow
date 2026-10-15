/* go/types: what the files of the package share.
 *
 * The concrete types and objects behind the opaque ones in the header, and the
 * unexported functions of Go's go/types that more than one file here calls.
 * Each struct starts with the header its kind is read from, so a TypesType or
 * TypesObject points at the struct itself.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_TYPES_INTERNAL_H
#define BURROW_GO_TYPES_INTERNAL_H

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ memory */

BURROW_NORETURN void burrow__types_oom(void);
BURROW_OWNS(ret) void *burrow__types_alloc(Alloc *a, size_t size, size_t align);

/* A zeroed T from a, which panics instead of returning NULL. */
#define GT_NEW(a, T) ((T *)burrow__types_alloc((a), sizeof(T), _Alignof(T)))

/* Pointer slices, which is every slice the package keeps: of TypesType, of
 * TypesVar * and so on, all with TYPE_UNSAFE_POINTER as their element. */
BURROW_OWNS(ret) Slice burrow__types_ptrs_make(Alloc *a, Int n);
void burrow__types_ptrs_append(Alloc *a, Slice *s, void *p);
#define GT_AT(s, i, T) (((T *)(s).p)[i])

/* Go's assert, which panics with "assertion failed" when p is false. It is a
 * macro so that the analyzers see the panic on the false side. */
BURROW_NORETURN void burrow__types_assert_failed(void);
#define burrow__types_assert(p) ((p) ? (void)0 : burrow__types_assert_failed())

/* ------------------------------------------------------------------- types */

struct TypesBasic {
    TypesTypeBase hdr;
    TypesBasicKind kind;
    TypesBasicInfo info;
    Str name;
};

struct TypesPointer {
    TypesTypeBase hdr;
    TypesType base; /* element type */
};

struct TypesArray {
    TypesTypeBase hdr;
    int64_t len; /* negative if unknown */
    TypesType elem;
};

struct TypesSlice {
    TypesTypeBase hdr;
    TypesType elem;
};

struct TypesStruct {
    TypesTypeBase hdr;
    Slice fields;  /* of TypesVar * */
    Slice tags;    /* of Str; empty if there are no tags */
    bool complete; /* Go's fields != nil: the struct is set up */
};

struct TypesTuple {
    TypesTypeBase hdr;
    Slice vars; /* of TypesVar * */
};

struct TypesSignature {
    TypesTypeBase hdr;
    TypesTypeParamList *rparams; /* receiver type parameters, or NULL */
    TypesTypeParamList *tparams; /* type parameters, or NULL */
    TypesScope *scope;           /* function scope for package local signatures */
    TypesVar *recv;              /* NULL if not a method */
    TypesVar *recvold;           /* receiver dropped by method selection, or NULL */
    TypesTuple *params;
    TypesTuple *results;
    bool variadic;
};

/* Go's _TypeSet. terms is a termlist, of TypesTerm *. */
typedef struct GtTypeSet {
    Slice methods; /* of TypesFunc *, sorted by Id */
    Slice terms;
    bool comparable; /* !comparable || terms is all */
} GtTypeSet;

struct TypesInterface {
    TypesTypeBase hdr;
    Alloc *a;            /* for the type set */
    TypesChecker *check; /* for error reporting; NULL once the type set is known */
    Slice methods;       /* of TypesFunc *, explicitly declared */
    Slice embeddeds;     /* of TypesType, explicitly embedded */
    Slice embed_pos;     /* of TokenPos, for errors; empty if not known */
    bool implicit;       /* the wrapper of a type set literal */
    bool complete;       /* methods and embeddeds are set */
    GtTypeSet *tset;     /* worked out lazily */
};

struct TypesMap {
    TypesTypeBase hdr;
    TypesType key;
    TypesType elem;
};

struct TypesChan {
    TypesTypeBase hdr;
    TypesChanDir dir;
    TypesType elem;
};

/* The state bits of a named type, Go's stateMask. */
#define GT_LAZY_LOADED 1u
#define GT_UNPACKED 2u
#define GT_HAS_METHODS 4u
#define GT_HAS_UNDER 8u
#define GT_HAS_VAR_SIZE 16u

/* What only an instantiated named type has. */
typedef struct GtInstance {
    TypesNamed *orig;
    TypesTypeList *targs;
    Int expanded_methods;
    TypesContext *ctxt; /* NULL once fully expanded */
} GtInstance;

struct TypesNamed {
    TypesTypeBase hdr;
    Alloc *a;
    TypesChecker *check; /* non-NULL during type checking; NULL otherwise */
    TypesTypeName *obj;
    bool allow_nil_rhs;
    GtInstance *inst;

    SyncMutex mu;           /* guards everything below */
    SyncAtomicUint32 state; /* GT_ bits; read atomically or under mu */
    TypesType from_rhs;
    TypesTypeParamList *tparams;
    TypesType underlying;
    bool var_size;
    Slice methods; /* of TypesFunc *; entries are NULL until expanded for instances */
};

struct TypesTypeParam {
    TypesTypeBase hdr;
    Alloc *a;            /* for the implicit interface of the bound */
    TypesChecker *check; /* for lazy type bound completion */
    uint64_t id;
    TypesTypeName *obj;
    Int index;
    TypesType bound;
};

struct TypesTerm {
    bool tilde;
    TypesType typ;
};

struct TypesUnion {
    TypesTypeBase hdr;
    Slice terms; /* of TypesTerm * */
};

struct TypesAlias {
    TypesTypeBase hdr;
    Alloc *a;
    TypesTypeName *obj;
    TypesAlias *orig;
    TypesTypeParamList *tparams;
    TypesTypeList *targs;
    TypesType from_rhs;
    TypesType actual; /* never an alias; set lazily */
};

struct TypesTypeParamList {
    Slice tparams; /* of TypesTypeParam * */
};

struct TypesTypeList {
    Slice types; /* of TypesType */
};

/* ----------------------------------------------------------------- objects */

/* Go's object, the fields every object shares, right after the header. */
typedef struct GtObject {
    TypesObjectBase hdr;
    TypesScope *parent;
    TokenPos pos;
    TypesPackage *pkg;
    Str name;
    TypesType typ;
    uint32_t order;
    TokenPos scope_pos;
} GtObject;

#define GT_OBJ(x) ((GtObject *)(void *)(x))

/* A downcast where the kind has already been looked at. types_as_* would
 * check it again, and gcc then warns about the NULL it could give. */
#define GT_KNOWN(T, x) ((T *)(void *)(x))

struct TypesPkgName {
    GtObject obj;
    TypesPackage *imported;
};

struct TypesConst {
    GtObject obj;
    ConstantValue val;
};

struct TypesTypeName {
    GtObject obj;
};

struct TypesVar {
    GtObject obj;
    TypesVar *origin;
    TypesVarKind kind;
    bool embedded;
};

struct TypesFunc {
    GtObject obj;
    TypesFunc *origin;
    bool has_ptr_recv; /* only for methods without a type yet */
    bool nointerface;
};

struct TypesLabel {
    GtObject obj;
    bool used;
};

struct TypesBuiltin {
    GtObject obj;
    Int id;
};

struct TypesNil {
    GtObject obj;
};

struct TypesScope {
    Alloc *a;
    TypesScope *parent;
    Slice children; /* of TypesScope * */
    Int number;     /* parent's children[number-1] is this one; 0 if no parent */
    Map *elems;     /* Str to TypesObject, made on the first insert */
    TokenPos pos, end;
    Str comment;
    bool is_func;
    bool universe; /* Go's s == Universe, without making the universe */
};

struct TypesPackage {
    Str path;
    Str name;
    TypesScope *scope;
    Slice imports; /* of TypesPackage * */
    bool complete;
    bool fake;
    bool cgo;
    Str go_version;
};

/* One instance a context knows about, in a list per hash. */
typedef struct GtCtxtEntry {
    TypesType orig;
    Slice targs; /* of TypesType */
    TypesType instance;
    struct GtCtxtEntry *next;
} GtCtxtEntry;

struct TypesContext {
    Alloc *a;
    SyncMutex mu;
    Map *type_map; /* Str hash to GtCtxtEntry * */
    Int next_id;
    Map *origin_ids; /* TypesType to Int */
};

/* --------------------------------------------------------------- builtins */

/* Go's builtinId. */
enum {
    GT_APPEND,
    GT_CAP,
    GT_CLEAR,
    GT_CLOSE,
    GT_COMPLEX,
    GT_COPY,
    GT_DELETE,
    GT_IMAG,
    GT_LEN,
    GT_MAKE,
    GT_MAX,
    GT_MIN,
    GT_NEW_,
    GT_PANIC,
    GT_PRINT,
    GT_PRINTLN,
    GT_REAL,
    GT_RECOVER,
    GT_ADD,
    GT_ALIGNOF,
    GT_OFFSETOF,
    GT_SIZEOF,
    GT_SLICE,
    GT_SLICE_DATA,
    GT_STRING,
    GT_STRING_DATA,
    GT_ASSERT,
    GT_TRACE,
    GT_NUM_BUILTINS
};

/* Go's predeclaredFuncs. kind is 0 for an expression and 1 for a statement,
 * which is all of Go's exprKind the table uses. */
typedef struct GtBuiltinInfo {
    Str name;
    Int nargs;
    bool variadic;
    Int kind;
} GtBuiltinInfo;

extern const GtBuiltinInfo burrow__types_predeclared_funcs[GT_NUM_BUILTINS];

/* --------------------------------------------------------------- universe */

/* Typ[i] as a TypesType. */
#define GT_TYP(i) types_type(types_typ[i])

/* byte and rune, the two basic types that are aliases. */
extern TypesBasic *const burrow__types_universe_byte;
extern TypesBasic *const burrow__types_universe_rune;

/* interface{}, the one shared empty interface, and the type set of every
 * type. */
extern TypesInterface *const burrow__types_empty_interface;
extern GtTypeSet *const burrow__types_top_type_set;

/* The termlist with the one term 𝓤. */
Slice burrow__types_all_termlist(void);

/* The predeclared objects, which make the universe the first time they are
 * asked for. */
TypesType burrow__types_universe_error(void);
TypesObject burrow__types_universe_any(void);
TypesObject burrow__types_universe_comparable(void);
TypesObject burrow__types_universe_iota(void);

/* ----------------------------------------------------------------- objects */

bool burrow__types_is_exported(Str name);
/* Id(pkg1, name1) == Id(pkg2, name2) and strings.Compare of the two, without
 * making either. */
bool burrow__types_id_eq(TypesPackage *pkg1, Str name1, TypesPackage *pkg2, Str name2);
int burrow__types_id_cmp(TypesPackage *pkg1, Str name1, TypesPackage *pkg2, Str name2);
bool burrow__types_same_id(TypesObject obj, TypesPackage *pkg, Str name,
                           bool fold_case);
int burrow__types_object_cmp(TypesObject x, TypesObject y);
bool burrow__types_same_pkg(TypesPackage *a, TypesPackage *b);

BURROW_OWNS(ret) TypesVar *burrow__types_new_var(Alloc *a, TypesVarKind kind,
                                                 TokenPos pos, TypesPackage *pkg,
                                                 Str name, TypesType typ);
BURROW_OWNS(ret) TypesVar *burrow__types_clone_var(Alloc *a, TypesVar *v,
                                                   TypesType typ);
BURROW_OWNS(ret) TypesFunc *burrow__types_clone_func(Alloc *a, TypesFunc *f,
                                                     TypesType typ);
bool burrow__types_func_has_ptr_recv(TypesFunc *f);
void burrow__types_scope_insert_raw(TypesScope *s, Str name, TypesObject obj);

void burrow__types_write_object(BytesBuffer *buf, TypesObject obj, TypesQualifier qf);
void burrow__types_write_func_name(BytesBuffer *buf, TypesFunc *f, TypesQualifier qf);
/* The package prefix, with its dot, written to buf. */
void burrow__types_write_package_prefix(BytesBuffer *buf, TypesPackage *pkg,
                                        TypesQualifier qf);
/* Go's objset: objects by Id, in a map made from a the first time it is
 * needed. */
typedef struct GtObjset {
    Alloc *a;
    Map *m;
} GtObjset;

/* Inserts obj, or leaves s as it is and returns the object with the same Id
 * that is already there. */
BURROW_BORROWS(ret, s) TypesObject burrow__types_objset_insert(GtObjset *s,
                                                               TypesObject obj);

/* Go's deref: the base of typ if it is a *Pointer, but not a named type whose
 * underlying type is one. */
TypesType burrow__types_deref(TypesType typ, bool *is_ptr);

/* Go's objectKind. */
BURROW_STATIC(ret) Str burrow__types_object_kind(TypesObject obj);

/* -------------------------------------------------------------- type lists */

BURROW_BORROWS(ret, l) Slice burrow__types_tparam_list(TypesTypeParamList *l);
BURROW_BORROWS(ret, l) Slice burrow__types_type_list(TypesTypeList *l);
BURROW_OWNS(ret) TypesTypeList *burrow__types_new_type_list(Alloc *a, Slice list);
BURROW_OWNS(ret) TypesTypeParamList *burrow__types_bind_tparams(Alloc *a, Slice list);

/* ---------------------------------------------------------- named, aliases */

BURROW_BORROWS(ret, n) TypesNamed *burrow__types_named_unpack(TypesNamed *n);
bool burrow__types_named_state_has(TypesNamed *n, uint32_t m);
BURROW_BORROWS(ret, n) TypesType burrow__types_named_rhs(TypesNamed *n);
/* Go's Named.Underlying. */
BURROW_BORROWS(ret, n) TypesType burrow__types_named_underlying(TypesNamed *n);
void burrow__types_named_cleanup(TypesNamed *n);
Int burrow__types_named_method_index(TypesNamed *t, Str name, bool fold_case);
BURROW_BORROWS(ret, n) TypesFunc *
burrow__types_named_lookup_method(TypesNamed *n, TypesPackage *pkg, Str name,
                                  bool fold_case, Int *index);
BURROW_OWNS(ret) TypesNamed *burrow__types_new_named_instance(Alloc *a, TokenPos pos,
                                                              TypesNamed *orig,
                                                              Slice targs,
                                                              TypesNamed *expanding);
BURROW_BORROWS(ret, t) TypesType burrow__types_safe_underlying(TypesType t);
/* Go's asNamed: the named type t is once aliases are followed, or NULL. */
BURROW_BORROWS(ret, t) TypesNamed *burrow__types_as_named(TypesType t);
BURROW_BORROWS(ret, a0) TypesType burrow__types_unalias(TypesAlias *a0);
BURROW_OWNS(ret) TypesAlias *burrow__types_new_alias(Alloc *a, TypesTypeName *obj,
                                                     TypesType rhs);
BURROW_OWNS(ret) TypesAlias *
burrow__types_new_alias_instance(Alloc *a, TokenPos pos, TypesAlias *orig, Slice targs,
                                 TypesNamed *expanding, TypesContext *ctxt);

/* ---------------------------------------------------------- interfaces */

BURROW_OWNS(ret) TypesInterface *burrow__types_new_interface(Alloc *a);
BURROW_BORROWS(ret, t) GtTypeSet *burrow__types_iface_type_set(TypesInterface *t);
BURROW_BORROWS(ret, ityp) GtTypeSet *
burrow__types_compute_interface_type_set(TokenPos pos, TypesInterface *ityp);
BURROW_BORROWS(ret, t) TypesInterface *
burrow__types_type_param_iface(TypesTypeParam *t);
void burrow__types_sort_methods(Slice list);
void burrow__types_interface_cleanup(TypesInterface *t);
void burrow__types_type_param_cleanup(TypesTypeParam *t);

/* --------------------------------------------------------- terms and sets */

bool burrow__types_term_equal(TypesTerm *x, TypesTerm *y);
/* x ∪ y, which is one term in *r1 with *r2 NULL, or the two when they are
 * disjoint. */
void burrow__types_term_union(TypesTerm *x, TypesTerm *y, TypesTerm **r1,
                              TypesTerm **r2);
BURROW_BORROWS(ret, x, y) TypesTerm *burrow__types_term_intersect(TypesTerm *x,
                                                                  TypesTerm *y);
bool burrow__types_term_includes(TypesTerm *x, TypesType t);
bool burrow__types_term_subset_of(TypesTerm *x, TypesTerm *y);
bool burrow__types_term_disjoint(TypesTerm *x, TypesTerm *y);
void burrow__types_write_term(BytesBuffer *buf, TypesTerm *x);

/* Termlists are Slices of TypesTerm *, where a NULL entry is ∅. */
void burrow__types_write_termlist(BytesBuffer *buf, Slice xl);
BURROW_OWNS(ret) Str burrow__types_termlist_string(Alloc *a, Slice xl);
bool burrow__types_termlist_is_empty(Slice xl);
bool burrow__types_termlist_is_all(Slice xl);
BURROW_OWNS(ret) Slice burrow__types_termlist_norm(Alloc *a, Slice xl);
BURROW_OWNS(ret) Slice burrow__types_termlist_union(Alloc *a, Slice xl, Slice yl);
BURROW_OWNS(ret) Slice burrow__types_termlist_intersect(Alloc *a, Slice xl, Slice yl);
bool burrow__types_termlist_equal(Slice xl, Slice yl);
bool burrow__types_termlist_includes(Slice xl, TypesType t);
bool burrow__types_termlist_superset_of(Slice xl, TypesTerm *y);
bool burrow__types_termlist_subset_of(Slice xl, Slice yl);

bool burrow__types_type_set_is_empty(GtTypeSet *s);
bool burrow__types_type_set_is_all(GtTypeSet *s);
bool burrow__types_type_set_is_method_set(GtTypeSet *s);
bool burrow__types_type_set_is_comparable(GtTypeSet *s, Map *seen);
bool burrow__types_type_set_has_terms(GtTypeSet *s);
bool burrow__types_type_set_subset_of(GtTypeSet *s1, GtTypeSet *s2);
BURROW_OWNS(ret) Str burrow__types_type_set_string(Alloc *a, GtTypeSet *s);
/* invalidTypeSet: the type set of a union with too many terms, which is
 * empty. */
BURROW_STATIC(ret) GtTypeSet *burrow__types_invalid_type_set(void);

/* Go's func(t, u Type) bool and func(*term) bool, for the walks over a type
 * set. */
BURROW_FUNC(GtTypePairFunc, bool, TypesType t, TypesType u);
BURROW_FUNC(GtTermFunc, bool, TypesTerm *t);
bool burrow__types_type_set_all(GtTypeSet *s, GtTypePairFunc f);
bool burrow__types_type_set_is(GtTypeSet *s, GtTermFunc f);

/* Go's all and underIs from under.go. */
bool burrow__types_all(TypesType t, GtTypePairFunc f);
/* Go's (*TypeParam).is: f called for each term of t's type set, or with NULL
 * if there are no specific terms. */
bool burrow__types_type_param_is(TypesTypeParam *t, GtTermFunc f);

/* ----------------------------------------------------------- predicates */

bool burrow__types_is_valid(TypesType t);
bool burrow__types_is_basic(TypesType t, TypesBasicInfo info);
bool burrow__types_all_basic(TypesType t, TypesBasicInfo info);
bool burrow__types_has_name(TypesType t);
bool burrow__types_is_type_lit(TypesType t);
bool burrow__types_is_typed(TypesType t);
bool burrow__types_is_untyped_numeric(TypesType t);
bool burrow__types_is_non_type_param_interface(TypesType t);
bool burrow__types_is_type_param(TypesType t);
bool burrow__types_has_empty_typeset(TypesType t);
bool burrow__types_is_generic(TypesType t);
/* Go's comparableType, reporting whether it found no reason T is not
 * comparable. seen may be NULL. */
bool burrow__types_comparable_type(TypesType t, bool dynamic, Map *seen);
bool burrow__types_has_nil(TypesType t);
bool burrow__types_identical_origin(TypesNamed *x, TypesNamed *y);
bool burrow__types_identical_instance(TypesType xorig, Slice xargs, TypesType yorig,
                                      Slice yargs);
BURROW_BORROWS(ret, x, y) TypesType burrow__types_max_type(TypesType x, TypesType y);

/* ------------------------------------------------------------ typestring */

/* Go's typeWriter. */
typedef struct GtTypeWriter {
    BytesBuffer *buf;
    struct GtSeen *seen; /* the types being written, innermost first */
    TypesQualifier qf;
    TypesContext *ctxt;          /* non-NULL when hashing */
    TypesTypeParamList *tparams; /* local type parameters */
    bool param_names;
    bool tp_subscripts;
    bool pkg_info;
} GtTypeWriter;

GtTypeWriter burrow__types_new_type_writer(BytesBuffer *buf, TypesQualifier qf);
GtTypeWriter burrow__types_new_type_hasher(BytesBuffer *buf, TypesContext *ctxt);
void burrow__types_tw_typ(GtTypeWriter *w, TypesType typ);
void burrow__types_tw_signature(GtTypeWriter *w, TypesSignature *sig);
void burrow__types_tw_tparam_list(GtTypeWriter *w, Slice list);
void burrow__types_tw_type_list(GtTypeWriter *w, Slice list);
void burrow__types_tw_string(GtTypeWriter *w, Str s);

/* Writes to buf, and panics if buf cannot grow. */
void burrow__types_buf_str(BytesBuffer *buf, Str s);
void burrow__types_buf_byte(BytesBuffer *buf, Byte c);
BURROW_OWNS(ret) Str burrow__types_buf_done(BytesBuffer *buf, Alloc *a);

/* ------------------------------------------------------------------ lookup */

BURROW_OWNS(ret) TypesSelection *
burrow__types_new_selection(Alloc *a, TypesSelectionKind kind, TypesType recv,
                            TypesObject obj, Slice index, bool indirect);
/* missingMethod with the identity check passed in. check may be NULL, as in
 * Go, for the exported API; then a cause is made in a and writes types with
 * their package paths, and a may be NULL when cause is. With a checker, the
 * methods found are type-checked first and the cause is made in check->a. */
TypesFunc *burrow__types_missing_method(TypesChecker *check, Alloc *a, TypesType V,
                                        TypesType T, bool is_static,
                                        bool (*equivalent)(TypesType x, TypesType y),
                                        bool *wrong_type, Str *cause);
/* hasAllMethods, the same way. */
bool burrow__types_has_all_methods(TypesChecker *check, Alloc *a, TypesType V,
                                   TypesType T, bool is_static,
                                   bool (*equivalent)(TypesType x, TypesType y),
                                   Str *cause);
bool burrow__types_is_interface_ptr(TypesType T);
/* interfacePtrError, made in a. T must be a pointer to an interface. */
BURROW_OWNS(ret) Str burrow__types_interface_ptr_error(Alloc *a, TypesQualifier qf,
                                                       TypesType T);
/* funcString: f's name and signature, without parameter names, made in a. */
BURROW_OWNS(ret) Str burrow__types_func_string(Alloc *a, TypesQualifier qf,
                                               TypesFunc *f, bool pkg_info);

/* Go's Checker.implements: whether V implements T, or satisfies it when
 * constraint is set. When cause is not NULL and the answer is no, *cause is
 * set to why. check may be NULL, the same way as for missingMethod. */
bool burrow__types_implements(TypesChecker *check, Alloc *a, TypesType V, TypesType T,
                              bool constraint, Str *cause);

/* ----------------------------------------------------- subst and contexts */

/* A substitution map is a Map from TypesTypeParam * to TypesType. */
BURROW_OWNS(ret) Map *burrow__types_make_subst_map(Alloc *a, Slice tpars, Slice targs);
BURROW_BORROWS(ret, typ) TypesType burrow__types_subst(Alloc *a, TokenPos pos,
                                                       TypesType typ, Map *smap,
                                                       TypesNamed *expanding,
                                                       TypesContext *ctxt);
/* Go's replaceRecvType. Sets *copied when out is a new slice. */
BURROW_OWNS(ret) Slice burrow__types_replace_recv_type(Alloc *a, Slice in,
                                                       TypesType old, TypesType new_,
                                                       bool *copied);

BURROW_OWNS(ret) Str burrow__types_context_instance_hash(TypesContext *ctxt,
                                                         TypesType orig, Slice targs);
BURROW_BORROWS(ret, ctxt) TypesType burrow__types_context_lookup(TypesContext *ctxt,
                                                                 Str h, TypesType orig,
                                                                 Slice targs);
BURROW_BORROWS(ret, ctxt) TypesType burrow__types_context_update(TypesContext *ctxt,
                                                                 Str h, TypesType orig,
                                                                 Slice targs,
                                                                 TypesType inst);
Int burrow__types_context_get_id(TypesContext *ctxt, TypesType t);

/* Go's Checker.instance with a nil checker. */
BURROW_OWNS(ret) TypesType burrow__types_instance(Alloc *a, TokenPos pos,
                                                  TypesType orig, Slice targs,
                                                  TypesNamed *expanding,
                                                  TypesContext *ctxt);

#endif
