/* go/types: what the files of the type checker share.
 *
 * Go's Checker is one struct with its methods spread over twenty files. Here
 * it is TypesChecker, and a method check.f in Go is burrow__tc_f(check, ...).
 * Positions for errors are worked out when the error is made, as a GtPosn,
 * where Go keeps a positioner and asks it later. Messages are formatted with
 * fmt, so a type, object, expression or operand that Go hands to its own
 * sprintf is turned into a string first with burrow__tc_ts and friends.
 *
 * Derived from Go's src/go/types/check.go, errors.go, format.go and
 * src/internal/types/errors/codes.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_TYPES_CHECK_H
#define BURROW_GO_TYPES_CHECK_H

#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------- error codes */

/* Go's internal/types/errors.Code, with the same values. */
typedef enum GtCode {
    GT_CODE_INVALID_SYNTAX_TREE = -1,
    GT_CODE_TEST = 1,
    GT_CODE_BLANK_PKG_NAME = 2,
    GT_CODE_MISMATCHED_PKG_NAME = 3,
    GT_CODE_INVALID_PKG_USE = 4,
    GT_CODE_BAD_IMPORT_PATH = 5,
    GT_CODE_BROKEN_IMPORT = 6,
    GT_CODE_IMPORT_C_RENAMED = 7,
    GT_CODE_UNUSED_IMPORT = 8,
    GT_CODE_INVALID_INIT_CYCLE = 9,
    GT_CODE_DUPLICATE_DECL = 10,
    GT_CODE_INVALID_DECL_CYCLE = 11,
    GT_CODE_INVALID_TYPE_CYCLE = 12,
    GT_CODE_INVALID_CONST_INIT = 13,
    GT_CODE_INVALID_CONST_VAL = 14,
    GT_CODE_INVALID_CONST_TYPE = 15,
    GT_CODE_UNTYPED_NIL_USE = 16,
    GT_CODE_WRONG_ASSIGN_COUNT = 17,
    GT_CODE_UNASSIGNABLE_OPERAND = 18,
    GT_CODE_NO_NEW_VAR = 19,
    GT_CODE_MULTI_VAL_ASSIGN_OP = 20,
    GT_CODE_INVALID_IFACE_ASSIGN = 21,
    GT_CODE_INVALID_CHAN_ASSIGN = 22,
    GT_CODE_INCOMPATIBLE_ASSIGN = 23,
    GT_CODE_UNADDRESSABLE_FIELD_ASSIGN = 24,
    GT_CODE_NOT_A_TYPE = 25,
    GT_CODE_INVALID_ARRAY_LEN = 26,
    GT_CODE_BLANK_IFACE_METHOD = 27,
    GT_CODE_INCOMPARABLE_MAP_KEY = 28,
    GT_CODE_INVALID_PTR_EMBED = 30,
    GT_CODE_BAD_RECV = 31,
    GT_CODE_INVALID_RECV = 32,
    GT_CODE_DUPLICATE_FIELD_AND_METHOD = 33,
    GT_CODE_DUPLICATE_METHOD = 34,
    GT_CODE_INVALID_BLANK = 35,
    GT_CODE_INVALID_IOTA = 36,
    GT_CODE_MISSING_INIT_BODY = 37,
    GT_CODE_INVALID_INIT_SIG = 38,
    GT_CODE_INVALID_INIT_DECL = 39,
    GT_CODE_INVALID_MAIN_DECL = 40,
    GT_CODE_TOO_MANY_VALUES = 41,
    GT_CODE_NOT_AN_EXPR = 42,
    GT_CODE_TRUNCATED_FLOAT = 43,
    GT_CODE_NUMERIC_OVERFLOW = 44,
    GT_CODE_UNDEFINED_OP = 45,
    GT_CODE_MISMATCHED_TYPES = 46,
    GT_CODE_DIV_BY_ZERO = 47,
    GT_CODE_NON_NUMERIC_INC_DEC = 48,
    GT_CODE_UNADDRESSABLE_OPERAND = 49,
    GT_CODE_INVALID_INDIRECTION = 50,
    GT_CODE_NON_INDEXABLE_OPERAND = 51,
    GT_CODE_INVALID_INDEX = 52,
    GT_CODE_SWAPPED_SLICE_INDICES = 53,
    GT_CODE_NON_SLICEABLE_OPERAND = 54,
    GT_CODE_INVALID_SLICE_EXPR = 55,
    GT_CODE_INVALID_SHIFT_COUNT = 56,
    GT_CODE_INVALID_SHIFT_OPERAND = 57,
    GT_CODE_INVALID_RECEIVE = 58,
    GT_CODE_INVALID_SEND = 59,
    GT_CODE_DUPLICATE_LIT_KEY = 60,
    GT_CODE_MISSING_LIT_KEY = 61,
    GT_CODE_INVALID_LIT_INDEX = 62,
    GT_CODE_OVERSIZE_ARRAY_LIT = 63,
    GT_CODE_MIXED_STRUCT_LIT = 64,
    GT_CODE_INVALID_STRUCT_LIT = 65,
    GT_CODE_MISSING_LIT_FIELD = 66,
    GT_CODE_DUPLICATE_LIT_FIELD = 67,
    GT_CODE_UNEXPORTED_LIT_FIELD = 68,
    GT_CODE_INVALID_LIT_FIELD = 69,
    GT_CODE_UNTYPED_LIT = 70,
    GT_CODE_INVALID_LIT = 71,
    GT_CODE_AMBIGUOUS_SELECTOR = 72,
    GT_CODE_UNDECLARED_IMPORTED_NAME = 73,
    GT_CODE_UNEXPORTED_NAME = 74,
    GT_CODE_UNDECLARED_NAME = 75,
    GT_CODE_MISSING_FIELD_OR_METHOD = 76,
    GT_CODE_BAD_DOT_DOT_DOT_SYNTAX = 77,
    GT_CODE_NON_VARIADIC_DOT_DOT_DOT = 78,
    GT_CODE_INVALID_DOT_DOT_DOT = 81,
    GT_CODE_UNCALLED_BUILTIN = 82,
    GT_CODE_INVALID_APPEND = 83,
    GT_CODE_INVALID_CAP = 84,
    GT_CODE_INVALID_CLOSE = 85,
    GT_CODE_INVALID_COPY = 86,
    GT_CODE_INVALID_COMPLEX = 87,
    GT_CODE_INVALID_DELETE = 88,
    GT_CODE_INVALID_IMAG = 89,
    GT_CODE_INVALID_LEN = 90,
    GT_CODE_SWAPPED_MAKE_ARGS = 91,
    GT_CODE_INVALID_MAKE = 92,
    GT_CODE_INVALID_REAL = 93,
    GT_CODE_INVALID_ASSERT = 94,
    GT_CODE_IMPOSSIBLE_ASSERT = 95,
    GT_CODE_INVALID_CONVERSION = 96,
    GT_CODE_INVALID_UNTYPED_CONVERSION = 97,
    GT_CODE_BAD_OFFSETOF_SYNTAX = 98,
    GT_CODE_INVALID_OFFSETOF = 99,
    GT_CODE_UNUSED_EXPR = 100,
    GT_CODE_UNUSED_VAR = 101,
    GT_CODE_MISSING_RETURN = 102,
    GT_CODE_WRONG_RESULT_COUNT = 103,
    GT_CODE_OUT_OF_SCOPE_RESULT = 104,
    GT_CODE_INVALID_COND = 105,
    GT_CODE_INVALID_POST_DECL = 106,
    GT_CODE_INVALID_ITER_VAR = 108,
    GT_CODE_INVALID_RANGE_EXPR = 109,
    GT_CODE_MISPLACED_BREAK = 110,
    GT_CODE_MISPLACED_CONTINUE = 111,
    GT_CODE_MISPLACED_FALLTHROUGH = 112,
    GT_CODE_DUPLICATE_CASE = 113,
    GT_CODE_DUPLICATE_DEFAULT = 114,
    GT_CODE_BAD_TYPE_KEYWORD = 115,
    GT_CODE_INVALID_TYPE_SWITCH = 116,
    GT_CODE_INVALID_EXPR_SWITCH = 117,
    GT_CODE_INVALID_SELECT_CASE = 118,
    GT_CODE_UNDECLARED_LABEL = 119,
    GT_CODE_DUPLICATE_LABEL = 120,
    GT_CODE_MISPLACED_LABEL = 121,
    GT_CODE_UNUSED_LABEL = 122,
    GT_CODE_JUMP_OVER_DECL = 123,
    GT_CODE_JUMP_INTO_BLOCK = 124,
    GT_CODE_INVALID_METHOD_EXPR = 125,
    GT_CODE_WRONG_ARG_COUNT = 126,
    GT_CODE_INVALID_CALL = 127,
    GT_CODE_UNUSED_RESULTS = 128,
    GT_CODE_INVALID_DEFER = 129,
    GT_CODE_INVALID_GO = 130,
    GT_CODE_BAD_DECL = 131,
    GT_CODE_REPEATED_DECL = 132,
    GT_CODE_INVALID_UNSAFE_ADD = 133,
    GT_CODE_INVALID_UNSAFE_SLICE = 134,
    GT_CODE_UNSUPPORTED_FEATURE = 135,
    GT_CODE_NOT_A_GENERIC_TYPE = 136,
    GT_CODE_WRONG_TYPE_ARG_COUNT = 137,
    GT_CODE_CANNOT_INFER_TYPE_ARGS = 138,
    GT_CODE_INVALID_TYPE_ARG = 139,
    GT_CODE_INVALID_INSTANCE_CYCLE = 140,
    GT_CODE_INVALID_UNION = 141,
    GT_CODE_MISPLACED_CONSTRAINT_IFACE = 142,
    GT_CODE_INVALID_METHOD_TYPE_PARAMS = 143,
    GT_CODE_MISPLACED_TYPE_PARAM = 144,
    GT_CODE_INVALID_UNSAFE_SLICE_DATA = 145,
    GT_CODE_INVALID_UNSAFE_STRING = 146,
    GT_CODE_INVALID_CLEAR = 148,
    GT_CODE_TYPE_TOO_LARGE = 149,
    GT_CODE_INVALID_MIN_MAX_OPERAND = 150,
    GT_CODE_TOO_NEW = 151,
} GtCode;

/* ---------------------------------------------------------------- positions */

/* Where an error goes: Go's positioner, worked out when the error is made. pos
 * is where it is reported, and start and end are the span of what is at fault,
 * Go's go116start and go116end. */
typedef struct GtPosn {
    TokenPos start, pos, end;
} GtPosn;

typedef struct GtOperand GtOperand;

/* atPos, an ast.Node, inNode, an Object and an *operand as positioners. */
GtPosn burrow__tc_at_pos(TokenPos pos);
GtPosn burrow__tc_at_node(AstNode n);
GtPosn burrow__tc_in_node(AstNode n, TokenPos pos);
GtPosn burrow__tc_at_obj(TypesObject obj);
GtPosn burrow__tc_at_op(const GtOperand *x);

#define GT_NODE(x) (&(x)->node)
#define GT_AT_NODE(x) burrow__tc_at_node(GT_NODE(x))

/* ----------------------------------------------------------------- operands */

/* Go's operandMode. */
typedef enum GtMode {
    GT_MODE_INVALID,  /* operand is invalid */
    GT_MODE_NOVALUE,  /* no value, as from a call of a function with no results */
    GT_MODE_BUILTIN,  /* a built-in function */
    GT_MODE_TYPEXPR,  /* a type */
    GT_MODE_CONSTANT, /* a constant; typ is a Basic type */
    GT_MODE_VARIABLE, /* an addressable variable */
    GT_MODE_MAPINDEX, /* a map index expression */
    GT_MODE_VALUE,    /* a computed value */
    GT_MODE_NILVALUE, /* the nil value, only used by types2 */
    GT_MODE_COMMAOK,  /* like value, but may be used in a comma, ok expression */
    GT_MODE_COMMAERR, /* like commaok, but the second value is an error */
    GT_MODE_CGOFUNC   /* a cgo function */
} GtMode;

/* Go's operand. val is only meaningful when mode is GT_MODE_CONSTANT. */
struct GtOperand {
    GtMode mode;
    AstExpr expr; /* NULL if not known */
    TypesType typ;
    ConstantValue val;
    Int id; /* the builtin, for GT_MODE_BUILTIN */
};

bool burrow__tc_op_valid(const GtOperand *x);
void burrow__tc_op_invalidate(GtOperand *x);
TokenPos burrow__tc_op_pos(const GtOperand *x);
BURROW_OWNS(ret) Str burrow__tc_operand_string(Alloc *a, const GtOperand *x,
                                               TypesQualifier qf);
BURROW_STATIC(ret) Str burrow__tc_composite_kind(TypesType typ);
void burrow__tc_op_set_const(Alloc *a, GtOperand *x, Token k, Str lit);
bool burrow__tc_op_is_nil(const GtOperand *x);

/* ------------------------------------------------------------ declarations */

/* Go's declInfo. */
typedef struct GtDeclInfo {
    TypesScope *file; /* scope of the file with the declaration */
    Str version;      /* Go version of that file */
    Slice lhs;        /* of TypesVar *, for n:1 variable declarations */
    AstExpr vtyp;     /* type, or NULL, for const and var declarations */
    AstExpr init;     /* init or orig expression, or NULL */
    bool inherited;   /* init is inherited from an earlier constant declaration */
    AstTypeSpec *tdecl;
    AstFuncDecl *fdecl;
    Map *deps; /* TypesObject to bool, made on the first addDep */
} GtDeclInfo;

bool burrow__tc_decl_has_initializer(const GtDeclInfo *d);
void burrow__tc_decl_add_dep(TypesChecker *check, GtDeclInfo *d, TypesObject obj);
/* check.objMap[obj], or NULL. */
BURROW_BORROWS(ret, check) GtDeclInfo *burrow__tc_obj_info(TypesChecker *check,
                                                           TypesObject obj);

/* Go's decl: one of the five things walkDecl finds. */
typedef enum GtDeclKind {
    GT_DECL_IMPORT,
    GT_DECL_CONST,
    GT_DECL_VAR,
    GT_DECL_TYPE,
    GT_DECL_FUNC
} GtDeclKind;

typedef struct GtDecl {
    GtDeclKind kind;
    AstImportSpec *import_spec; /* GT_DECL_IMPORT */
    AstValueSpec *value_spec;   /* GT_DECL_CONST and GT_DECL_VAR */
    AstTypeSpec *type_spec;     /* GT_DECL_TYPE */
    AstFuncDecl *func_decl;     /* GT_DECL_FUNC */
    /* GT_DECL_CONST only */
    Int iota;
    AstExpr typ;
    Slice init; /* of AstExpr */
    bool inherited;
} GtDecl;

typedef void (*GtDeclFn)(TypesChecker *check, void *env, const GtDecl *d);

/* decl.go */
void burrow__tc_declare(TypesChecker *check, TypesScope *scope, AstIdent *id,
                        TypesObject obj, TokenPos pos);
void burrow__tc_obj_decl(TypesChecker *check, TypesObject obj);
void burrow__tc_walk_decls(TypesChecker *check, Slice decls, GtDeclFn f, void *env);
void burrow__tc_walk_decl(TypesChecker *check, AstDecl d, GtDeclFn f, void *env);
void burrow__tc_collect_methods(TypesChecker *check, TypesTypeName *obj);
void burrow__tc_decl_stmt(TypesChecker *check, AstDecl d);

/* resolver.go */
void burrow__tc_arity_match(TypesChecker *check, AstValueSpec *s, AstValueSpec *init);
bool burrow__tc_unpack_recv(TypesChecker *check, AstExpr rtyp, bool unpack_params,
                            AstExpr *base, Slice *tparams);

/* index.go's indexedExpr. */
typedef struct GtIndexedExpr {
    AstExpr orig; /* the wrapped expression */
    AstExpr x;
    TokenPos lbrack;
    Slice indices; /* of AstExpr */
    TokenPos rbrack;
} GtIndexedExpr;

/* unpackIndexedExpr: false for anything but an index or index list. */
bool burrow__tc_unpack_indexed_expr(Alloc *a, AstNode n, GtIndexedExpr *ix);

/* -------------------------------------------------------------- environment */

/* Go's environment: where the object being checked is. */
typedef struct GtEnv {
    GtDeclInfo *decl;      /* the package level declaration being checked */
    TypesScope *scope;     /* top-most scope for lookups */
    Str version;           /* current accepted language version */
    ConstantValue *iota;   /* iota in a constant declaration, or NULL */
    bool has_errpos;       /* errpos is set */
    GtPosn errpos;         /* identifier of a constant with an inherited init */
    bool in_tparam_list;   /* inside a type parameter list */
    TypesSignature *sig;   /* signature of the function being checked, or NULL */
    Map *is_panic;         /* AstCallExpr * to bool: the calls of panic */
    bool has_label;        /* the function has labels */
    bool has_call_or_recv; /* the expression has a call or a receive */
    TokenPos expr_pos;     /* if valid, look identifiers up as if here */
} GtEnv;

BURROW_BORROWS(ret, env) TypesScope *burrow__tc_env_lookup_scope(GtEnv *env, Str name,
                                                                 TypesObject *obj);
BURROW_BORROWS(ret, env) TypesObject burrow__tc_env_lookup(GtEnv *env, Str name);

/* An action, and what it needs, for check.later. */
typedef void (*GtActionFn)(TypesChecker *check, void *env);

typedef struct GtAction {
    Str version; /* the language version it runs with */
    GtActionFn f;
    void *env;
} GtAction;

/* Go's exprInfo: an expression whose type is still untyped. */
typedef struct GtExprInfo {
    bool is_lhs; /* lhs operand of a shift with a delayed type check */
    GtMode mode;
    TypesBasic *typ;
    ConstantValue val; /* when mode is constant */
} GtExprInfo;

/* ---------------------------------------------------------- monomorphization */

/* Go's monoGraph, monoVertex and monoEdge. */
typedef struct GtMonoVertex {
    Int weight;         /* weight of the heaviest known path to this vertex */
    Int pre;            /* previous edge, if any, in the above path */
    Int len;            /* length of the above path */
    TypesTypeName *obj; /* the type parameter's object */
} GtMonoVertex;

typedef struct GtMonoEdge {
    Int dst, src;
    Int weight;
    TokenPos pos;
    TypesType typ;
} GtMonoEdge;

typedef struct GtMonoGraph {
    Slice vertices; /* of GtMonoVertex * */
    Slice edges;    /* of GtMonoEdge * */
    Map *canon;     /* TypesTypeParam * to TypesTypeParam * */
    Map *name_idx;  /* TypesTypeName * to Int */
} GtMonoGraph;

/* ------------------------------------------------------------------ checker */

struct TypesChecker {
    Alloc *a; /* everything comes from here */

    /* package information, for the life of the checker */
    TypesConfig *conf;
    TypesContext *ctxt;
    TokenFileSet *fset;
    TypesPackage *pkg;
    TypesInfo *info;
    uint64_t next_id;  /* for type parameters; the first valid one is 1 */
    Map *obj_map;      /* TypesObject to GtDeclInfo * */
    Slice obj_list;    /* of TypesObject, the keys of obj_map in source order */
    Map *imp_map;      /* Str of path, NUL and dir to TypesPackage * */
    Map *pkg_path_map; /* Str package name to Map * of Str path to bool */
    Map *seen_pkg_map; /* TypesPackage * to bool */

    /* for the duration of types_checker_files */
    Slice files;          /* of AstFile * */
    Map *versions;        /* AstFile * to Str */
    Slice imports;        /* of TypesPkgName * */
    Map *dot_import_map;  /* TypesScope * to Map * of Str name to TypesPkgName * */
    Map *broken_aliases;  /* TypesTypeName * to bool */
    Map *union_type_sets; /* TypesUnion * to GtTypeSet * */
    Map *used_vars;       /* TypesVar * to bool */
    Map *used_pkg_names;  /* TypesPkgName * to bool */
    GtMonoGraph mono;

    bool has_first_err;
    TypesError first_err;
    Map *methods;      /* TypesTypeName * to Slice * of TypesFunc * */
    Map *untyped;      /* AstExpr to GtExprInfo * */
    Slice delayed;     /* of GtAction *, processed in FIFO order */
    Slice obj_path;    /* of TypesObject, for cycle reporting */
    Map *obj_path_idx; /* TypesObject to Int */
    Slice cleaners;    /* of TypesType: named, alias, interface and type parameter */

    /* where the object being checked is */
    GtEnv env;

    bool bailing; /* the panic unwinding is a bailout */

    /* what conf and info point at when the caller gave none */
    TypesConfig conf0;
    TypesInfo info0;
};

/* --------------------------------------------------------------- formatting */

/* The arguments Go's sprintf turns into text, turned into text here first.
 * Each is made in check->a. */
BURROW_OWNS(ret) Str burrow__tc_ts(TypesChecker *check, TypesType t);
BURROW_OWNS(ret) Str burrow__tc_os(TypesChecker *check, TypesObject obj);
BURROW_OWNS(ret) Str burrow__tc_es(TypesChecker *check, AstExpr x);
BURROW_OWNS(ret) Str burrow__tc_xs(TypesChecker *check, const GtOperand *x);
BURROW_OWNS(ret) Str burrow__tc_ps(TypesChecker *check, TokenPos pos);
/* A []Type, a []*TypeParam, a []ast.Expr and a []*operand, in brackets. */
BURROW_OWNS(ret) Str burrow__tc_tls(TypesChecker *check, Slice list);
BURROW_OWNS(ret) Str burrow__tc_els(TypesChecker *check, Slice list);
BURROW_OWNS(ret) Str burrow__tc_xls(TypesChecker *check, Slice list);

/* Go's check.sprintf, once the arguments are text. */
BURROW_OWNS(ret) Str burrow__tc_sprintf(TypesChecker *check, Str format, Slice args);
#define GT_SPRINTF(check, ...)                                                         \
    burrow__tc_sprintf((check), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* check.qualifier. */
TypesQualifier burrow__tc_qualifier(TypesChecker *check);
/* markImports: adds pkg and what it imports to check->pkg_path_map. */
void burrow__tc_mark_imports(TypesChecker *check, TypesPackage *pkg);
BURROW_OWNS(ret) Str burrow__tc_strip_annotations(Alloc *a, Str s);

/* ------------------------------------------------------------------- errors */

typedef struct GtErrorDesc {
    GtPosn posn;
    Str msg;
} GtErrorDesc;

/* Go's error_. */
typedef struct GtError {
    TypesChecker *check;
    Slice desc; /* of GtErrorDesc * */
    GtCode code;
    bool soft;
} GtError;

BURROW_OWNS(ret) GtError *burrow__tc_new_error(TypesChecker *check, GtCode code);
void burrow__tc_error_add(GtError *err, GtPosn at, Str msg);
void burrow__tc_error_add_alt_decl(GtError *err, TypesObject obj);
bool burrow__tc_error_empty(const GtError *err);
void burrow__tc_error_report(GtError *err);
#define GT_ADDF(err, at, ...)                                                          \
    burrow__tc_error_add((err), (at), GT_SPRINTF((err)->check, __VA_ARGS__))

/* check.error, check.errorf, check.softErrorf and check.versionErrorf. */
void burrow__tc_error(TypesChecker *check, GtPosn at, GtCode code, Str msg);
void burrow__tc_soft_error(TypesChecker *check, GtPosn at, GtCode code, Str msg);
void burrow__tc_version_error(TypesChecker *check, GtPosn at, Str v, Str msg);
#define GT_ERRORF(check, at, code, ...)                                                \
    burrow__tc_error((check), (at), (code), GT_SPRINTF((check), __VA_ARGS__))
#define GT_SOFT_ERRORF(check, at, code, ...)                                           \
    burrow__tc_soft_error((check), (at), (code), GT_SPRINTF((check), __VA_ARGS__))
#define GT_VERSION_ERRORF(check, at, v, ...)                                           \
    burrow__tc_version_error((check), (at), (v), GT_SPRINTF((check), __VA_ARGS__))

/* The bailout panic, for when there is no Config.Error. */
BURROW_NORETURN void burrow__tc_bailout(TypesChecker *check);

#define GT_INVALID_ARG "invalid argument: "
#define GT_INVALID_OP "invalid operation: "

/* ------------------------------------------------------------------ versions */

/* Go's goVersion: asGoVersion, and the language versions the checker knows. */
BURROW_BORROWS(ret, v) Str burrow__tc_as_go_version(Alloc *a, Str v);
extern const Str burrow__tc_go1_9, burrow__tc_go1_13, burrow__tc_go1_14,
    burrow__tc_go1_17, burrow__tc_go1_18, burrow__tc_go1_20, burrow__tc_go1_21,
    burrow__tc_go1_22, burrow__tc_go1_23, burrow__tc_go1_26, burrow__tc_go1_27,
    burrow__tc_go_current;
bool burrow__tc_allow_version(TypesChecker *check, Str want);
bool burrow__tc_verify_version(TypesChecker *check, GtPosn at, Str v, Str msg);
#define GT_VERIFY_VERSIONF(check, at, v, ...)                                          \
    burrow__tc_verify_version((check), (at), (v), GT_SPRINTF((check), __VA_ARGS__))

/* ------------------------------------------------------------------ checker */

void burrow__tc_add_decl_dep(TypesChecker *check, TypesObject to);
void burrow__tc_remember_untyped(TypesChecker *check, AstExpr e, bool lhs, GtMode mode,
                                 TypesBasic *typ, ConstantValue val);
BURROW_BORROWS(ret, check) GtAction *burrow__tc_later(TypesChecker *check, GtActionFn f,
                                                      void *env);
void burrow__tc_push(TypesChecker *check, TypesObject obj);
void burrow__tc_pop(TypesChecker *check);
void burrow__tc_needs_cleanup(TypesChecker *check, TypesType c);
void burrow__tc_process_delayed(TypesChecker *check, Int top);
BURROW_BORROWS(ret, check) TypesContext *burrow__tc_context(TypesChecker *check);
BURROW_BORROWS(ret, expr) AstIdent *burrow__tc_instantiated_ident(TypesChecker *check,
                                                                  AstExpr expr);
void burrow__tc_cleanup(TypesChecker *check);

/* What a method expression leaves in recvold: the receiver is params[0]. */
extern TypesVar *const burrow__tc_method_expr_sentinel;

/* The phases of checkFiles, from resolver.go, decl.go, initorder.go,
 * mono.go and the rest, in the order they run. */
void burrow__tc_collect_objects(TypesChecker *check);
void burrow__tc_sort_objects(TypesChecker *check);
void burrow__tc_direct_cycles(TypesChecker *check);
void burrow__tc_package_objects(TypesChecker *check);
void burrow__tc_init_order(TypesChecker *check);
void burrow__tc_unused_imports(TypesChecker *check);
void burrow__tc_monomorph(TypesChecker *check);

/* ---------------------------------------------------------------- recording */

void burrow__tc_record(TypesChecker *check, const GtOperand *x);
void burrow__tc_record_untyped(TypesChecker *check);
void burrow__tc_record_type_and_value(TypesChecker *check, AstExpr x, GtMode mode,
                                      TypesType typ, ConstantValue val);
void burrow__tc_record_builtin_type(TypesChecker *check, AstExpr f,
                                    TypesSignature *sig);
void burrow__tc_record_comma_ok_types(TypesChecker *check, AstExpr x, GtOperand *a0,
                                      GtOperand *a1);
void burrow__tc_record_instance(TypesChecker *check, AstExpr expr, Slice targs,
                                TypesType typ);
void burrow__tc_record_def(TypesChecker *check, AstIdent *id, TypesObject obj);
void burrow__tc_record_use(TypesChecker *check, AstIdent *id, TypesObject obj);
void burrow__tc_record_implicit(TypesChecker *check, AstNode node, TypesObject obj);
void burrow__tc_record_selection(TypesChecker *check, AstSelectorExpr *x,
                                 TypesSelectionKind kind, TypesType recv,
                                 TypesObject obj, Slice index, bool indirect);
void burrow__tc_record_scope(TypesChecker *check, AstNode node, TypesScope *scope);

#endif
