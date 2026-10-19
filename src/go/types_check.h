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
    GT_CODE_NONE = 0, /* no error; Go leaves the zero Code unnamed */
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
/* assignableTo reports whether x is assignable to a variable of type T. If
 * the result is false and a non-NULL cause is given, *cause may be set to a
 * more detailed explanation of the failure, and *code, if code is not NULL,
 * to the error code. check may be NULL, as in Go, when x is not a constant,
 * and then no cause is made. */
bool burrow__tc_assignable_to(TypesChecker *check, const GtOperand *x, TypesType T,
                              Str *cause, GtCode *code);
/* convertibleTo reports whether T(x) is valid, the same way. */
bool burrow__tc_convertible_to(TypesChecker *check, const GtOperand *x, TypesType T,
                               Str *cause);
/* The errorf of assignableTo and convertibleTo: *cause becomes msg, followed
 * by what *cause held before, if anything, on the next line. */
void burrow__tc_wrap_cause(TypesChecker *check, Str *cause, Str msg);
/* conversion type-checks the conversion T(x). The result is in x. */
void burrow__tc_conversion(TypesChecker *check, GtOperand *x, TypesType T);
/* isPointer: whether typ's underlying type is a pointer. */
bool burrow__tc_is_pointer(TypesType typ);

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

    bool bailing;       /* the panic unwinding is a bailout */
    Slice bail_aliases; /* of TypesAlias *, the alias declarations being checked */

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

/* check.qualifier. A NULL checker has none, so types are written with their
 * package paths, as Go's check.sprintf does with a nil checker. */
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
/* err.posn() and err.msg(); the message is made in check->a. */
GtPosn burrow__tc_error_posn(const GtError *err);
BURROW_OWNS(ret) Str burrow__tc_error_msg(const GtError *err);
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
/* Not in Go: what the deferred function of typeDecl does for each alias
 * declaration a bailout leaves half done. Run after catching a bailout. */
void burrow__tc_bail_aliases(TypesChecker *check);

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

/* The rest of decl.go that other files use. */
void burrow__tc_cycle_error(TypesChecker *check, Slice cycle, Int start);
Int burrow__tc_first_in_src(Slice path);
bool burrow__tc_is_imported_constraint(TypesChecker *check, TypesType typ);
void burrow__tc_collect_type_params(TypesChecker *check, TypesTypeParamList **dst,
                                    AstFieldList *list);
BURROW_BORROWS(ret, check) TypesTypeParam *
burrow__tc_declare_type_param(TypesChecker *check, AstIdent *name, TokenPos scope_pos);

/* Go's newNamed, newAlias and newTypeParam with a checker, which the cleanup
 * phase finishes off. */
BURROW_OWNS(ret) TypesNamed *burrow__tc_new_named(TypesChecker *check,
                                                  TypesTypeName *obj,
                                                  TypesType from_rhs, Slice methods);
BURROW_OWNS(ret) TypesAlias *burrow__tc_new_alias(TypesChecker *check,
                                                  TypesTypeName *obj, TypesType rhs);
BURROW_OWNS(ret) TypesTypeParam *burrow__tc_new_type_param(TypesChecker *check,
                                                           TypesTypeName *obj,
                                                           TypesType constraint);

/* ---------------------------------------------------- types and expressions */

/* Go's target: the signature a function literal is checked against, and what
 * it is for. */
typedef struct GtTarget {
    TypesSignature *sig;
    Str desc;
} GtTarget;

/* NULL when typ is not a function type. */
BURROW_OWNS(ret) GtTarget *burrow__tc_new_target(Alloc *a, TypesType typ, Str desc);

TypesType burrow__tc_typ(TypesChecker *check, AstExpr e);
TypesType burrow__tc_var_type(TypesChecker *check, AstExpr e);
TypesType burrow__tc_declared_type(TypesChecker *check, AstExpr e, TypesTypeName *def);
void burrow__tc_expr(TypesChecker *check, GtTarget *T, GtOperand *x, AstExpr e);
void burrow__tc_init_const(TypesChecker *check, TypesConst *lhs, GtOperand *x);
void burrow__tc_init_var(TypesChecker *check, TypesVar *lhs, GtOperand *x, Str context);
void burrow__tc_init_vars(TypesChecker *check, Slice lhs, Slice orig_rhs,
                          AstStmt return_stmt);
void burrow__tc_func_type(TypesChecker *check, TypesSignature *sig,
                          AstFieldList *recv_par, AstFuncType *ftyp);
void burrow__tc_func_body(TypesChecker *check, GtDeclInfo *decl, Str name,
                          TypesSignature *sig, AstBlockStmt *body,
                          const ConstantValue *iota);
void burrow__tc_open_scope(TypesChecker *check, AstNode node, Str comment);
void burrow__tc_close_scope(TypesChecker *check);
/* assignOp: the operator of an op= assignment, or TOKEN_ILLEGAL. */
Token burrow__tc_assign_op(Token op);
/* trie.insert: inserts val for key, a non-empty Slice of Int, into keys, a
 * Slice of pointers that starts out nil. Two keys collide if one is a prefix
 * of the other. If an existing key is a prefix of (or equal to) key, it
 * returns that key's value and sets *n to its length. If key is a proper
 * prefix of existing keys, it returns the value of the smallest of them and
 * sets *n to the length of key. Otherwise it inserts val, returns it and sets
 * *n to 0. */
void *burrow__tc_trie_insert(Alloc *a, Slice *keys, Slice key, void *val, Int *n);
void burrow__tc_valid_type(TypesChecker *check, TypesNamed *typ);
void burrow__tc_ident(TypesChecker *check, GtOperand *x, AstIdent *e, bool want_type);
void burrow__tc_valid_var_type(TypesChecker *check, AstExpr e, TypesType typ);
/* cause may be NULL. */
TypesType burrow__tc_generic_type(TypesChecker *check, AstExpr e, Str *cause);
/* false where Go returns a nil list; *res is nil then. */
bool burrow__tc_type_list(TypesChecker *check, Slice list, Slice *res);
void burrow__tc_selector(TypesChecker *check, GtOperand *x, AstSelectorExpr *e,
                         bool want_type);
bool burrow__tc_use(TypesChecker *check, Slice args);
bool burrow__tc_use_lhs(TypesChecker *check, Slice args);
bool burrow__tc_use1(TypesChecker *check, AstExpr e, bool lhs);
TypesType burrow__tc_parse_union(TypesChecker *check, AstExpr uexpr);
void burrow__tc_struct_type(TypesChecker *check, TypesStruct *styp, AstStructType *e);
BURROW_OWNS(ret) TypesInterface *burrow__tc_new_interface(TypesChecker *check);
void burrow__tc_interface_type(TypesChecker *check, TypesInterface *ityp,
                               AstInterfaceType *iface, TypesTypeName *def);
/* rounded may be NULL. */
bool burrow__tc_representable_const(Alloc *a, ConstantValue x, TypesChecker *check,
                                    TypesBasic *typ, ConstantValue *rounded);

/* const.go */
void burrow__tc_overflow(TypesChecker *check, GtOperand *x, TokenPos op_pos);
GtCode burrow__tc_representation(TypesChecker *check, const GtOperand *x,
                                 TypesBasic *typ, ConstantValue *v);
void burrow__tc_representable(TypesChecker *check, GtOperand *x, TypesBasic *typ);
void burrow__tc_invalid_conversion(TypesChecker *check, GtCode code, const GtOperand *x,
                                   TypesType target);
void burrow__tc_convert_untyped(TypesChecker *check, GtOperand *x, TypesType target);

/* under.go: Go's func(t, u Type) *typeError. Returning false is a non-nil
 * error, with *cause set to its text, which is empty for the empty error. */
BURROW_FUNC(GtUnderCond, bool, TypesType t, TypesType u, Str *cause);
/* NULL, with *cause set, where Go returns a nil Type. cond.f may be NULL, and
 * cause may be NULL when the caller ignores the error. */
TypesType burrow__tc_common_under(TypesChecker *check, TypesType t, GtUnderCond cond,
                                  Str *cause);

/* expr.go */

/* exprKind describes the kind of an expression; the kind determines if an
 * expression is valid in 'statement context'. */
typedef enum GtExprKind {
    GT_EXPR_CONVERSION,
    GT_EXPR_EXPRESSION,
    GT_EXPR_STATEMENT
} GtExprKind;

/* opPos: where the operator of x is, if x is an operation. */
TokenPos burrow__tc_expr_op_pos(AstExpr x);
BURROW_STATIC(ret) Str burrow__tc_op_name(AstExpr e);
/* NULL, after reporting an error, if the operation is not valid. */
TypesType burrow__tc_chan_elem(TypesChecker *check, GtPosn pos, const GtOperand *x,
                               bool recv);
bool burrow__tc_is_comparison(Token op);
void burrow__tc_comparison(TypesChecker *check, GtOperand *x, GtOperand *y, Token op,
                           bool switch_case);
void burrow__tc_match_types(TypesChecker *check, GtOperand *x, GtOperand *y);
/* e may be NULL for an assignment operation. */
void burrow__tc_binary(TypesChecker *check, GtOperand *x, AstExpr e, AstExpr lhs,
                       AstExpr rhs, Token op, TokenPos op_pos);
GtExprKind burrow__tc_raw_expr(TypesChecker *check, GtTarget *T, GtOperand *x,
                               AstExpr e, TypesType hint, bool allow_generic);
void burrow__tc_non_generic(TypesChecker *check, GtTarget *T, GtOperand *x);
GtExprKind burrow__tc_expr_internal(TypesChecker *check, GtTarget *T, GtOperand *x,
                                    AstExpr e, TypesType hint);
/* keyVal, as a Str to key a map with, made in a. */
BURROW_OWNS(ret) Str burrow__tc_key_val(Alloc *a, ConstantValue x);
void burrow__tc_type_assertion(TypesChecker *check, AstExpr e, GtOperand *x,
                               TypesType T, bool type_switch);
void burrow__tc_generic_expr(TypesChecker *check, GtOperand *x, AstExpr e,
                             TypesType hint);
/* *list is of GtOperand *; the result is Go's commaOk. */
bool burrow__tc_multi_expr(TypesChecker *check, AstExpr e, bool allow_comma_ok,
                           Slice *list);
BURROW_OWNS(ret) Str burrow__tc_nth(TypesChecker *check, Int n, Str what);
void burrow__tc_expr_or_type(TypesChecker *check, GtOperand *x, AstExpr e,
                             bool allow_generic);
/* modeset is of 1 << GtMode bits. */
void burrow__tc_exclude(TypesChecker *check, GtOperand *x, unsigned modeset);
void burrow__tc_single_value(TypesChecker *check, GtOperand *x);

/* literals.go */
void burrow__tc_basic_lit(TypesChecker *check, GtOperand *x, AstBasicLit *e);
void burrow__tc_func_lit(TypesChecker *check, GtOperand *x, AstFuncLit *e);
void burrow__tc_composite_lit(TypesChecker *check, GtOperand *x, AstCompositeLit *e,
                              TypesType hint);

/* index.go */
/* Go's isFuncInst. */
bool burrow__tc_index_expr(TypesChecker *check, GtOperand *x, GtIndexedExpr *e);
void burrow__tc_slice_expr(TypesChecker *check, GtOperand *x, AstSliceExpr *e);
/* index checks an index expression for validity. If max >= 0, it is the upper
 * bound for index. The result is the index type, invalid if the index is not
 * valid, and *val is the index value if it is a constant, else -1. */
TypesType burrow__tc_index(TypesChecker *check, AstExpr index, int64_t max,
                           int64_t *val);
bool burrow__tc_is_valid_index(TypesChecker *check, GtOperand *x, GtCode code, Str what,
                               bool allow_negative);

/* call.go */
/* The type arguments, of TypesType; ix may be NULL. */
Slice burrow__tc_func_inst(TypesChecker *check, GtTarget *T, TokenPos pos, GtOperand *x,
                           GtIndexedExpr *ix, bool infer);
GtExprKind burrow__tc_call_expr(TypesChecker *check, GtOperand *x, AstCallExpr *call);
BURROW_OWNS(ret) TypesSignature *
burrow__tc_instantiate_signature(TypesChecker *check, TokenPos pos, AstExpr expr,
                                 TypesSignature *typ, Slice targs, Slice xlist);
/* Of GtOperand *. */
Slice burrow__tc_expr_list(TypesChecker *check, Slice elist);
/* Of GtOperand *. *targs_list is of Slice *, one for each operand, and a NULL
 * entry, or a short or nil list, is Go's nil []Type. */
Slice burrow__tc_generic_expr_list(TypesChecker *check, Slice elist, Slice *targs_list);
/* args is of GtOperand * and atargs of Slice *, as from generic_expr_list. */
BURROW_BORROWS(ret, sig) TypesSignature *
burrow__tc_arguments(TypesChecker *check, AstCallExpr *call, TypesSignature *sig,
                     Slice targs, Slice xlist, Slice args, Slice atargs);
/* util.go's hasDots. */
bool burrow__tc_has_dots(const AstCallExpr *call);

/* builtins.go: id is the index into burrow__types_predeclared_funcs. */
bool burrow__tc_builtin(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id);
/* arrayPtrDeref returns A if typ is of the form *A and A is an array;
 * otherwise it returns typ. */
TypesType burrow__tc_array_ptr_deref(TypesType typ);

/* unify.go. A unifier maintains a list of type parameters and the types
 * inferred for them. targs may be shorter than tparams, and may hold NULLs. */
typedef struct GtUnifier GtUnifier;
/* unifyMode: with assign, the types may match inexactly at the top but
 * element types must match exactly; with exact, the types must be identical
 * once type parameters are set. Neither is inexact unification. */
#define GT_UNIFY_ASSIGN 1u
#define GT_UNIFY_EXACT 2u
BURROW_OWNS(ret) GtUnifier *burrow__tc_new_unifier(TypesChecker *check, Slice tparams,
                                                   Slice targs,
                                                   bool enable_interface_inference);
bool burrow__tc_unify(GtUnifier *u, TypesType x, TypesType y, unsigned mode);
TypesType burrow__tc_unifier_at(const GtUnifier *u, const TypesTypeParam *x);
void burrow__tc_unifier_set(GtUnifier *u, const TypesTypeParam *x, TypesType t);
Int burrow__tc_unifier_unknowns(const GtUnifier *u);
/* The types inferred for tparams, NULL where there is none yet. */
BURROW_OWNS(ret) Slice burrow__tc_unifier_inferred(const GtUnifier *u, Slice tparams);

/* infer.go. Go's nil []Type result is a Slice with a NULL p. tparams is of
 * TypesTypeParam *, targs of TypesType with NULL for the ones to infer, and
 * args of GtOperand *. */
Slice burrow__tc_infer(TypesChecker *check, GtPosn posn, Slice tparams, Slice targs,
                       TypesTuple *params, Slice args, bool reverse, GtError *err);
/* The renamed type parameters, with *out set to typ with them in. */
Slice burrow__tc_rename_tparams(TypesChecker *check, TokenPos pos, Slice tparams,
                                TypesType typ, TypesType *out);
bool burrow__tc_is_parameterized(Alloc *a, Slice tparams, TypesType typ);

/* stmt.go. stmtContext is a bitset describing which control-flow statements
 * are permissible, and provides additional context information for better
 * error messages. */
typedef unsigned GtStmtContext;
/* permissible control-flow statements */
#define GT_BREAK_OK 1u
#define GT_CONTINUE_OK 2u
#define GT_FALLTHROUGH_OK 4u
/* additional context information */
#define GT_FINAL_SWITCH_CASE 8u
#define GT_IN_TYPE_SWITCH 16u
void burrow__tc_stmt(TypesChecker *check, GtStmtContext ctxt, AstStmt s);
void burrow__tc_stmt_list(TypesChecker *check, GtStmtContext ctxt, Slice list);

/* labels.go */
void burrow__tc_labels(TypesChecker *check, AstBlockStmt *body);

/* return.go. label is empty when s is not labeled. */
bool burrow__tc_is_terminating(TypesChecker *check, AstStmt s, Str label);

/* range.go. s_key, s_value and s_extra may be NULL. */
void burrow__tc_range_stmt(TypesChecker *check, GtStmtContext inner,
                           AstRangeStmt *range_stmt, GtPosn no_new_var_pos,
                           AstExpr s_key, AstExpr s_value, AstExpr s_extra,
                           AstExpr range_var, bool is_def);

/* assignments.go */
void burrow__tc_assignment(TypesChecker *check, GtOperand *x, TypesType T, Str context);
/* x may be NULL, and then rhs is evaluated; else rhs is ignored. */
void burrow__tc_assign_var(TypesChecker *check, AstExpr lhs, AstExpr rhs, GtOperand *x,
                           Str context);
/* lhs and orig_rhs are of AstExpr. */
void burrow__tc_assign_vars(TypesChecker *check, Slice lhs, Slice orig_rhs);
void burrow__tc_short_var_decl(TypesChecker *check, GtPosn pos, Slice lhs,
                               Slice orig_rhs);
/* list is of TypesType, and a NULL entry is an invalid type. */
BURROW_OWNS(ret) Str burrow__tc_types_summary(TypesChecker *check, Slice list,
                                              bool variadic, bool has_dots);
/* Of TypesType, from a Slice of GtOperand * and a Slice of TypesVar *. */
BURROW_OWNS(ret) Slice burrow__tc_operand_types(Alloc *a, Slice list);
BURROW_OWNS(ret) Slice burrow__tc_var_types(Alloc *a, Slice list);

/* errsupport.go: obj may be NULL. */
BURROW_OWNS(ret) Str burrow__tc_lookup_error(TypesChecker *check, TypesType typ,
                                             Str sel, TypesObject obj, bool struct_lit);

/* cycles.go */
bool burrow__tc_is_complete(TypesChecker *check, TypesType t);

/* lookup.go: assertableTo. check may be NULL, as for the API, and then the
 * cause is made in a. */
bool burrow__tc_assertable_to(TypesChecker *check, Alloc *a, TypesType V, TypesType T,
                              Str *cause);
/* Sets *has_val where Go returns a non-nil value. check may be NULL, as in
 * Go, when x is not a constant. */
GtCode burrow__tc_implicit_type_and_value(TypesChecker *check, const GtOperand *x,
                                          TypesType target, TypesType *typ,
                                          ConstantValue *val, bool *has_val);
void burrow__tc_update_expr_val(TypesChecker *check, AstExpr x, ConstantValue val);
void burrow__tc_update_expr_type(TypesChecker *check, AstExpr x, TypesType typ,
                                 bool final);

/* ------------------------------------------------------------ instantiation */

TypesType burrow__tc_instance(TypesChecker *check, TokenPos pos, TypesType orig,
                              Slice targs, TypesNamed *expanding, TypesContext *ctxt);
bool burrow__tc_validate_targ_len(TypesChecker *check, TokenPos pos, Str name, Int want,
                                  Int got);
/* The index of the failing type argument, with *err set, or -1. */
Int burrow__tc_verify(TypesChecker *check, TokenPos pos, Slice tparams, Slice targs,
                      TypesContext *ctxt, Error *err);
void burrow__tc_mono_record_canon(TypesChecker *check, TypesTypeParam *mpar,
                                  TypesTypeParam *tpar);
TypesType burrow__tc_subst(TypesChecker *check, TokenPos pos, TypesType typ, Map *smap,
                           TypesNamed *expanding, TypesContext *ctxt);
/* "%d unit", with an s on unit unless x is 1. */
BURROW_OWNS(ret) Str burrow__tc_measure(TypesChecker *check, Int x, Str unit);
void burrow__tc_mono_record_instance(TypesChecker *check, TypesPackage *pkg,
                                     TokenPos pos, Slice tparams, Slice targs,
                                     Slice xlist);

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
