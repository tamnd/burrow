/* go/types: how the checker reports errors and formats their messages.
 *
 * Derived from Go's src/go/types/errors.go and format.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* -------------------------------------------------------------- positions */

GtPosn burrow__tc_at_pos(TokenPos pos) {
    GtPosn p = {pos, pos, pos};
    return p;
}

GtPosn burrow__tc_at_node(AstNode n) {
    TokenPos pos = ast_node_pos(n);
    GtPosn p = {pos, pos, ast_node_end(n)};
    return p;
}

GtPosn burrow__tc_in_node(AstNode n, TokenPos pos) {
    GtPosn p = {ast_node_pos(n), pos, ast_node_end(n)};
    return p;
}

GtPosn burrow__tc_at_obj(TypesObject obj) {
    return burrow__tc_at_pos(types_object_pos(obj));
}

GtPosn burrow__tc_at_op(const GtOperand *x) {
    if (x->expr != NULL) {
        TokenPos pos = ast_expr_pos(x->expr);
        GtPosn p = {pos, pos, ast_expr_end(x->expr)};
        return p;
    }
    return burrow__tc_at_pos(TOKEN_NO_POS);
}

/* ------------------------------------------------------------- formatting */

Str burrow__tc_ts(TypesChecker *check, TypesType t) {
    return types_type_string(check->a, t, burrow__tc_qualifier(check));
}

Str burrow__tc_os(TypesChecker *check, TypesObject obj) {
    if (obj == NULL)
        return BURROW_S("<nil>");
    return types_object_string(check->a, obj, burrow__tc_qualifier(check));
}

Str burrow__tc_es(TypesChecker *check, AstExpr x) {
    if (x == NULL)
        return BURROW_S("<nil>");
    return types_expr_string(check->a, x);
}

Str burrow__tc_xs(TypesChecker *check, const GtOperand *x) {
    return burrow__tc_operand_string(check->a, x, burrow__tc_qualifier(check));
}

Str burrow__tc_ps(TypesChecker *check, TokenPos pos) {
    if (check->fset == NULL)
        return fmt_sprintf_v(check->a, "%d", (Int)pos);
    return token_position_string(token_file_set_position(check->fset, pos), check->a);
}

Str burrow__tc_tls(TypesChecker *check, Slice list) {
    BytesBuffer buf = BYTES_BUFFER(check->a);
    GtTypeWriter w = burrow__types_new_type_writer(&buf, burrow__tc_qualifier(check));
    burrow__types_buf_byte(&buf, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        burrow__types_tw_typ(&w, GT_AT(list, i, TypesType));
    }
    burrow__types_buf_byte(&buf, ']');
    return burrow__types_buf_done(&buf, check->a);
}

Str burrow__tc_els(TypesChecker *check, Slice list) {
    BytesBuffer buf = BYTES_BUFFER(check->a);
    burrow__types_buf_byte(&buf, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        types_write_expr(&buf, GT_AT(list, i, AstExpr));
    }
    burrow__types_buf_byte(&buf, ']');
    return burrow__types_buf_done(&buf, check->a);
}

Str burrow__tc_xls(TypesChecker *check, Slice list) {
    BytesBuffer buf = BYTES_BUFFER(check->a);
    burrow__types_buf_byte(&buf, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        burrow__types_buf_str(&buf, burrow__tc_xs(check, GT_AT(list, i, GtOperand *)));
    }
    burrow__types_buf_byte(&buf, ']');
    return burrow__types_buf_done(&buf, check->a);
}

Str burrow__tc_sprintf(TypesChecker *check, Str format, Slice args) {
    Str s = fmt_sprintf(check->a, format, args);
    if (s.len == 0 && format.len > 0)
        burrow__types_oom();
    return s;
}

/* markImports: every package name in the import graph of pkg, with the paths
 * it is used for. */
void burrow__tc_mark_imports(TypesChecker *check, TypesPackage *pkg) {
    if (map_get(check->seen_pkg_map, &pkg) != NULL)
        return;
    bool yes = true;
    if (!map_set(check->seen_pkg_map, &pkg, &yes))
        burrow__types_oom();

    Map **for_name = map_get(check->pkg_path_map, &pkg->name);
    Map *m;
    if (for_name == NULL) {
        m = map_make(check->a, TYPE_STRING, TYPE_BOOL, 0);
        if (m == NULL || !map_set(check->pkg_path_map, &pkg->name, &m))
            burrow__types_oom();
    } else {
        m = *for_name;
    }
    if (!map_set(m, &pkg->path, &yes))
        burrow__types_oom();

    for (Int i = 0; i < pkg->imports.len; i++)
        burrow__tc_mark_imports(check, GT_AT(pkg->imports, i, TypesPackage *));
}

static Str tce_qualifier(void *env, TypesPackage *pkg) {
    TypesChecker *check = env;
    if (pkg != check->pkg) {
        if (check->pkg_path_map == NULL) {
            check->pkg_path_map =
                map_make(check->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
            check->seen_pkg_map = map_make(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
            if (check->pkg_path_map == NULL || check->seen_pkg_map == NULL)
                burrow__types_oom();
            burrow__tc_mark_imports(check, check->pkg);
        }
        Map **paths = map_get(check->pkg_path_map, &pkg->name);
        if (paths != NULL && map_len(*paths) > 1)
            return strconv_quote(check->a, pkg->path);
        return pkg->name;
    }
    return BURROW_STR_EMPTY;
}

TypesQualifier burrow__tc_qualifier(TypesChecker *check) {
    if (check == NULL)
        return TYPES_NO_QUALIFIER;
    return BURROW_FN(TypesQualifier, tce_qualifier, check);
}

/* The subscripts ₀ to ₉ that tell type parameters with the same name apart. */
Str burrow__tc_strip_annotations(Alloc *a, Str s) {
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 0; i < s.len;) {
        Int size;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        if (r < 0x2080 || 0x2080 + 10 <= r)
            burrow__types_buf_str(&buf, str_from_bytes(s.p + i, size));
        i += size;
    }
    if (bytes_buffer_len(&buf) < s.len)
        return burrow__types_buf_done(&buf, a);
    return s;
}

/* ----------------------------------------------------------------- errors */

GtError *burrow__tc_new_error(TypesChecker *check, GtCode code) {
    if (code == 0)
        panic_str(BURROW_S("error code must not be 0"));
    GtError *err = GT_NEW(check->a, GtError);
    err->check = check;
    err->code = code;
    return err;
}

void burrow__tc_error_add(GtError *err, GtPosn at, Str msg) {
    GtErrorDesc *d = GT_NEW(err->check->a, GtErrorDesc);
    d->posn = at;
    d->msg = msg;
    burrow__types_ptrs_append(err->check->a, &err->desc, d);
}

void burrow__tc_error_add_alt_decl(GtError *err, TypesObject obj) {
    TokenPos pos = types_object_pos(obj);
    if (pos != TOKEN_NO_POS) {
        Str name = types_object_name(obj);
        GT_ADDF(err, burrow__tc_at_obj(obj), "other declaration of %s", name);
    }
}

bool burrow__tc_error_empty(const GtError *err) {
    return err->desc.len == 0;
}

GtPosn burrow__tc_error_posn(const GtError *err) {
    if (burrow__tc_error_empty(err))
        return burrow__tc_at_pos(TOKEN_NO_POS);
    return GT_AT(err->desc, 0, GtErrorDesc *)->posn;
}

Str burrow__tc_error_msg(const GtError *err) {
    if (burrow__tc_error_empty(err))
        return BURROW_S("no error");
    TypesChecker *check = err->check;
    BytesBuffer buf = BYTES_BUFFER(check->a);
    for (Int i = 0; i < err->desc.len; i++) {
        GtErrorDesc *p = GT_AT(err->desc, i, GtErrorDesc *);
        if (i > 0) {
            burrow__types_buf_str(&buf, BURROW_S("\n\t"));
            if (p->posn.pos != TOKEN_NO_POS) {
                burrow__types_buf_str(&buf, burrow__tc_ps(check, p->posn.pos));
                burrow__types_buf_str(&buf, BURROW_S(": "));
            }
        }
        burrow__types_buf_str(&buf, p->msg);
    }
    return burrow__types_buf_done(&buf, check->a);
}

/* The names of the error codes, as Go's stringer-made Code.String has them. */
static const char *const tce_code_names[] = {
    [GT_CODE_INVALID_SYNTAX_TREE + 1] = "InvalidSyntaxTree",
    [GT_CODE_TEST + 1] = "Test",
    [GT_CODE_BLANK_PKG_NAME + 1] = "BlankPkgName",
    [GT_CODE_MISMATCHED_PKG_NAME + 1] = "MismatchedPkgName",
    [GT_CODE_INVALID_PKG_USE + 1] = "InvalidPkgUse",
    [GT_CODE_BAD_IMPORT_PATH + 1] = "BadImportPath",
    [GT_CODE_BROKEN_IMPORT + 1] = "BrokenImport",
    [GT_CODE_IMPORT_C_RENAMED + 1] = "ImportCRenamed",
    [GT_CODE_UNUSED_IMPORT + 1] = "UnusedImport",
    [GT_CODE_INVALID_INIT_CYCLE + 1] = "InvalidInitCycle",
    [GT_CODE_DUPLICATE_DECL + 1] = "DuplicateDecl",
    [GT_CODE_INVALID_DECL_CYCLE + 1] = "InvalidDeclCycle",
    [GT_CODE_INVALID_TYPE_CYCLE + 1] = "InvalidTypeCycle",
    [GT_CODE_INVALID_CONST_INIT + 1] = "InvalidConstInit",
    [GT_CODE_INVALID_CONST_VAL + 1] = "InvalidConstVal",
    [GT_CODE_INVALID_CONST_TYPE + 1] = "InvalidConstType",
    [GT_CODE_UNTYPED_NIL_USE + 1] = "UntypedNilUse",
    [GT_CODE_WRONG_ASSIGN_COUNT + 1] = "WrongAssignCount",
    [GT_CODE_UNASSIGNABLE_OPERAND + 1] = "UnassignableOperand",
    [GT_CODE_NO_NEW_VAR + 1] = "NoNewVar",
    [GT_CODE_MULTI_VAL_ASSIGN_OP + 1] = "MultiValAssignOp",
    [GT_CODE_INVALID_IFACE_ASSIGN + 1] = "InvalidIfaceAssign",
    [GT_CODE_INVALID_CHAN_ASSIGN + 1] = "InvalidChanAssign",
    [GT_CODE_INCOMPATIBLE_ASSIGN + 1] = "IncompatibleAssign",
    [GT_CODE_UNADDRESSABLE_FIELD_ASSIGN + 1] = "UnaddressableFieldAssign",
    [GT_CODE_NOT_A_TYPE + 1] = "NotAType",
    [GT_CODE_INVALID_ARRAY_LEN + 1] = "InvalidArrayLen",
    [GT_CODE_BLANK_IFACE_METHOD + 1] = "BlankIfaceMethod",
    [GT_CODE_INCOMPARABLE_MAP_KEY + 1] = "IncomparableMapKey",
    [GT_CODE_INVALID_PTR_EMBED + 1] = "InvalidPtrEmbed",
    [GT_CODE_BAD_RECV + 1] = "BadRecv",
    [GT_CODE_INVALID_RECV + 1] = "InvalidRecv",
    [GT_CODE_DUPLICATE_FIELD_AND_METHOD + 1] = "DuplicateFieldAndMethod",
    [GT_CODE_DUPLICATE_METHOD + 1] = "DuplicateMethod",
    [GT_CODE_INVALID_BLANK + 1] = "InvalidBlank",
    [GT_CODE_INVALID_IOTA + 1] = "InvalidIota",
    [GT_CODE_MISSING_INIT_BODY + 1] = "MissingInitBody",
    [GT_CODE_INVALID_INIT_SIG + 1] = "InvalidInitSig",
    [GT_CODE_INVALID_INIT_DECL + 1] = "InvalidInitDecl",
    [GT_CODE_INVALID_MAIN_DECL + 1] = "InvalidMainDecl",
    [GT_CODE_TOO_MANY_VALUES + 1] = "TooManyValues",
    [GT_CODE_NOT_AN_EXPR + 1] = "NotAnExpr",
    [GT_CODE_TRUNCATED_FLOAT + 1] = "TruncatedFloat",
    [GT_CODE_NUMERIC_OVERFLOW + 1] = "NumericOverflow",
    [GT_CODE_UNDEFINED_OP + 1] = "UndefinedOp",
    [GT_CODE_MISMATCHED_TYPES + 1] = "MismatchedTypes",
    [GT_CODE_DIV_BY_ZERO + 1] = "DivByZero",
    [GT_CODE_NON_NUMERIC_INC_DEC + 1] = "NonNumericIncDec",
    [GT_CODE_UNADDRESSABLE_OPERAND + 1] = "UnaddressableOperand",
    [GT_CODE_INVALID_INDIRECTION + 1] = "InvalidIndirection",
    [GT_CODE_NON_INDEXABLE_OPERAND + 1] = "NonIndexableOperand",
    [GT_CODE_INVALID_INDEX + 1] = "InvalidIndex",
    [GT_CODE_SWAPPED_SLICE_INDICES + 1] = "SwappedSliceIndices",
    [GT_CODE_NON_SLICEABLE_OPERAND + 1] = "NonSliceableOperand",
    [GT_CODE_INVALID_SLICE_EXPR + 1] = "InvalidSliceExpr",
    [GT_CODE_INVALID_SHIFT_COUNT + 1] = "InvalidShiftCount",
    [GT_CODE_INVALID_SHIFT_OPERAND + 1] = "InvalidShiftOperand",
    [GT_CODE_INVALID_RECEIVE + 1] = "InvalidReceive",
    [GT_CODE_INVALID_SEND + 1] = "InvalidSend",
    [GT_CODE_DUPLICATE_LIT_KEY + 1] = "DuplicateLitKey",
    [GT_CODE_MISSING_LIT_KEY + 1] = "MissingLitKey",
    [GT_CODE_INVALID_LIT_INDEX + 1] = "InvalidLitIndex",
    [GT_CODE_OVERSIZE_ARRAY_LIT + 1] = "OversizeArrayLit",
    [GT_CODE_MIXED_STRUCT_LIT + 1] = "MixedStructLit",
    [GT_CODE_INVALID_STRUCT_LIT + 1] = "InvalidStructLit",
    [GT_CODE_MISSING_LIT_FIELD + 1] = "MissingLitField",
    [GT_CODE_DUPLICATE_LIT_FIELD + 1] = "DuplicateLitField",
    [GT_CODE_UNEXPORTED_LIT_FIELD + 1] = "UnexportedLitField",
    [GT_CODE_INVALID_LIT_FIELD + 1] = "InvalidLitField",
    [GT_CODE_UNTYPED_LIT + 1] = "UntypedLit",
    [GT_CODE_INVALID_LIT + 1] = "InvalidLit",
    [GT_CODE_AMBIGUOUS_SELECTOR + 1] = "AmbiguousSelector",
    [GT_CODE_UNDECLARED_IMPORTED_NAME + 1] = "UndeclaredImportedName",
    [GT_CODE_UNEXPORTED_NAME + 1] = "UnexportedName",
    [GT_CODE_UNDECLARED_NAME + 1] = "UndeclaredName",
    [GT_CODE_MISSING_FIELD_OR_METHOD + 1] = "MissingFieldOrMethod",
    [GT_CODE_BAD_DOT_DOT_DOT_SYNTAX + 1] = "BadDotDotDotSyntax",
    [GT_CODE_NON_VARIADIC_DOT_DOT_DOT + 1] = "NonVariadicDotDotDot",
    [GT_CODE_INVALID_DOT_DOT_DOT + 1] = "InvalidDotDotDot",
    [GT_CODE_UNCALLED_BUILTIN + 1] = "UncalledBuiltin",
    [GT_CODE_INVALID_APPEND + 1] = "InvalidAppend",
    [GT_CODE_INVALID_CAP + 1] = "InvalidCap",
    [GT_CODE_INVALID_CLOSE + 1] = "InvalidClose",
    [GT_CODE_INVALID_COPY + 1] = "InvalidCopy",
    [GT_CODE_INVALID_COMPLEX + 1] = "InvalidComplex",
    [GT_CODE_INVALID_DELETE + 1] = "InvalidDelete",
    [GT_CODE_INVALID_IMAG + 1] = "InvalidImag",
    [GT_CODE_INVALID_LEN + 1] = "InvalidLen",
    [GT_CODE_SWAPPED_MAKE_ARGS + 1] = "SwappedMakeArgs",
    [GT_CODE_INVALID_MAKE + 1] = "InvalidMake",
    [GT_CODE_INVALID_REAL + 1] = "InvalidReal",
    [GT_CODE_INVALID_ASSERT + 1] = "InvalidAssert",
    [GT_CODE_IMPOSSIBLE_ASSERT + 1] = "ImpossibleAssert",
    [GT_CODE_INVALID_CONVERSION + 1] = "InvalidConversion",
    [GT_CODE_INVALID_UNTYPED_CONVERSION + 1] = "InvalidUntypedConversion",
    [GT_CODE_BAD_OFFSETOF_SYNTAX + 1] = "BadOffsetofSyntax",
    [GT_CODE_INVALID_OFFSETOF + 1] = "InvalidOffsetof",
    [GT_CODE_UNUSED_EXPR + 1] = "UnusedExpr",
    [GT_CODE_UNUSED_VAR + 1] = "UnusedVar",
    [GT_CODE_MISSING_RETURN + 1] = "MissingReturn",
    [GT_CODE_WRONG_RESULT_COUNT + 1] = "WrongResultCount",
    [GT_CODE_OUT_OF_SCOPE_RESULT + 1] = "OutOfScopeResult",
    [GT_CODE_INVALID_COND + 1] = "InvalidCond",
    [GT_CODE_INVALID_POST_DECL + 1] = "InvalidPostDecl",
    [GT_CODE_INVALID_ITER_VAR + 1] = "InvalidIterVar",
    [GT_CODE_INVALID_RANGE_EXPR + 1] = "InvalidRangeExpr",
    [GT_CODE_MISPLACED_BREAK + 1] = "MisplacedBreak",
    [GT_CODE_MISPLACED_CONTINUE + 1] = "MisplacedContinue",
    [GT_CODE_MISPLACED_FALLTHROUGH + 1] = "MisplacedFallthrough",
    [GT_CODE_DUPLICATE_CASE + 1] = "DuplicateCase",
    [GT_CODE_DUPLICATE_DEFAULT + 1] = "DuplicateDefault",
    [GT_CODE_BAD_TYPE_KEYWORD + 1] = "BadTypeKeyword",
    [GT_CODE_INVALID_TYPE_SWITCH + 1] = "InvalidTypeSwitch",
    [GT_CODE_INVALID_EXPR_SWITCH + 1] = "InvalidExprSwitch",
    [GT_CODE_INVALID_SELECT_CASE + 1] = "InvalidSelectCase",
    [GT_CODE_UNDECLARED_LABEL + 1] = "UndeclaredLabel",
    [GT_CODE_DUPLICATE_LABEL + 1] = "DuplicateLabel",
    [GT_CODE_MISPLACED_LABEL + 1] = "MisplacedLabel",
    [GT_CODE_UNUSED_LABEL + 1] = "UnusedLabel",
    [GT_CODE_JUMP_OVER_DECL + 1] = "JumpOverDecl",
    [GT_CODE_JUMP_INTO_BLOCK + 1] = "JumpIntoBlock",
    [GT_CODE_INVALID_METHOD_EXPR + 1] = "InvalidMethodExpr",
    [GT_CODE_WRONG_ARG_COUNT + 1] = "WrongArgCount",
    [GT_CODE_INVALID_CALL + 1] = "InvalidCall",
    [GT_CODE_UNUSED_RESULTS + 1] = "UnusedResults",
    [GT_CODE_INVALID_DEFER + 1] = "InvalidDefer",
    [GT_CODE_INVALID_GO + 1] = "InvalidGo",
    [GT_CODE_BAD_DECL + 1] = "BadDecl",
    [GT_CODE_REPEATED_DECL + 1] = "RepeatedDecl",
    [GT_CODE_INVALID_UNSAFE_ADD + 1] = "InvalidUnsafeAdd",
    [GT_CODE_INVALID_UNSAFE_SLICE + 1] = "InvalidUnsafeSlice",
    [GT_CODE_UNSUPPORTED_FEATURE + 1] = "UnsupportedFeature",
    [GT_CODE_NOT_A_GENERIC_TYPE + 1] = "NotAGenericType",
    [GT_CODE_WRONG_TYPE_ARG_COUNT + 1] = "WrongTypeArgCount",
    [GT_CODE_CANNOT_INFER_TYPE_ARGS + 1] = "CannotInferTypeArgs",
    [GT_CODE_INVALID_TYPE_ARG + 1] = "InvalidTypeArg",
    [GT_CODE_INVALID_INSTANCE_CYCLE + 1] = "InvalidInstanceCycle",
    [GT_CODE_INVALID_UNION + 1] = "InvalidUnion",
    [GT_CODE_MISPLACED_CONSTRAINT_IFACE + 1] = "MisplacedConstraintIface",
    [GT_CODE_INVALID_METHOD_TYPE_PARAMS + 1] = "InvalidMethodTypeParams",
    [GT_CODE_MISPLACED_TYPE_PARAM + 1] = "MisplacedTypeParam",
    [GT_CODE_INVALID_UNSAFE_SLICE_DATA + 1] = "InvalidUnsafeSliceData",
    [GT_CODE_INVALID_UNSAFE_STRING + 1] = "InvalidUnsafeString",
    [GT_CODE_INVALID_CLEAR + 1] = "InvalidClear",
    [GT_CODE_TYPE_TOO_LARGE + 1] = "TypeTooLarge",
    [GT_CODE_INVALID_MIN_MAX_OPERAND + 1] = "InvalidMinMaxOperand",
    [GT_CODE_TOO_NEW + 1] = "TooNew",
};

/* Code.String. */
static Str tce_code_string(Alloc *a, GtCode code) {
    Int i = (Int)code + 1;
    if (i >= 0 && i < (Int)(sizeof(tce_code_names) / sizeof(tce_code_names[0])) &&
        tce_code_names[i] != NULL)
        return str_from_cstr(tce_code_names[i]);
    return fmt_sprintf_v(a, "Code(%d)", (Int)code);
}

/* handleError: hands one error to Config.Error, or bails out when there is
 * none. */
static void tce_handle_error(TypesChecker *check, Int index, GtPosn posn, GtCode code,
                             Str msg, bool soft) {
    burrow__types_assert(code != 0);

    if (index == 0) {
        /* If we are encountering an error while evaluating inherited constant
         * initialization expressions, posn is the position of the original
         * expression, and not of the currently declared constant identifier.
         * Use the provided errpos instead. */
        if (check->env.has_errpos && check->env.errpos.pos != TOKEN_NO_POS) {
            burrow__types_assert(check->env.iota != NULL);
            posn = check->env.errpos;
        }

        /* Report invalid syntax trees explicitly. */
        if (code == GT_CODE_INVALID_SYNTAX_TREE)
            msg = fmt_sprintf_v(check->a, "invalid syntax tree: %s", msg);

        /* If we have a URL for error codes, add a link to the first line. */
        if (check->conf->error_url.len > 0) {
            Str url = fmt_sprintf_v(check->a, check->conf->error_url,
                                    tce_code_string(check->a, code));
            Int i = strings_index(msg, BURROW_S("\n"));
            if (i >= 0)
                msg = fmt_sprintf_v(check->a, "%s%s%s", str_from_bytes(msg.p, i), url,
                                    str_from_bytes(msg.p + i, msg.len - i));
            else
                msg = fmt_sprintf_v(check->a, "%s%s", msg, url);
        }
    } else {
        /* Indent sub-error. Position information is already
         * encoded in the error message. */
        msg = fmt_sprintf_v(check->a, "\t%s", msg);
    }

    TypesError e = {
        .fset = check->fset,
        .pos = posn.pos,
        .msg = burrow__tc_strip_annotations(check->a, msg),
        .soft = soft,
        .code = code,
        .start = posn.start,
        .end = posn.end,
    };

    /* If we have an errpos, use it for the span as well. */
    if (check->env.has_errpos) {
        e.pos = check->env.errpos.pos;
        e.start = check->env.errpos.start;
        e.end = check->env.errpos.end;
    }

    if (!check->has_first_err) {
        check->has_first_err = true;
        check->first_err = e;
    }

    TypesErrorFunc f = check->conf->error;
    if (f.f == NULL)
        burrow__tc_bailout(check); /* record first error and exit */
    f.f(f.env, &e);
}

/* report: reports the error, and resets it so that it can be reused. */
void burrow__tc_error_report(GtError *err) {
    if (burrow__tc_error_empty(err))
        panic_str(BURROW_S("no error"));

    /* Cheap trick: Don't report errors with messages containing
     * "invalid operand" or "invalid type" as those tend to be
     * follow-on errors which don't add useful information. Only
     * exclude them if these strings are not at the beginning,
     * and only if we have at least one error already reported. */
    TypesChecker *check = err->check;
    if (check->has_first_err) {
        Str msg = GT_AT(err->desc, 0, GtErrorDesc *)->msg;
        if (strings_index(msg, BURROW_S("invalid operand")) > 0 ||
            strings_index(msg, BURROW_S("invalid type")) > 0)
            return;
    }

    /* In go/types, if there is a sub-error with a valid position,
     * call the typechecker error handler for each sub-error.
     * Otherwise, call it once, with a single combined message. */
    bool multi_error = false;
    for (Int i = 1; i < err->desc.len; i++) {
        if (GT_AT(err->desc, i, GtErrorDesc *)->posn.pos != TOKEN_NO_POS) {
            multi_error = true;
            break;
        }
    }

    if (multi_error) {
        for (Int i = 0; i < err->desc.len; i++) {
            GtErrorDesc *p = GT_AT(err->desc, i, GtErrorDesc *);
            tce_handle_error(check, i, p->posn, err->code, p->msg, err->soft);
        }
    } else {
        tce_handle_error(check, 0, burrow__tc_error_posn(err), err->code,
                         burrow__tc_error_msg(err), err->soft);
    }

    /* make sure the error is not reported twice */
    err->desc = slice_nil(err->desc.elem);
}

void burrow__tc_error(TypesChecker *check, GtPosn at, GtCode code, Str msg) {
    GtError *err = burrow__tc_new_error(check, code);
    burrow__tc_error_add(err, at, msg);
    burrow__tc_error_report(err);
}

void burrow__tc_soft_error(TypesChecker *check, GtPosn at, GtCode code, Str msg) {
    GtError *err = burrow__tc_new_error(check, code);
    burrow__tc_error_add(err, at, msg);
    err->soft = true;
    burrow__tc_error_report(err);
}

void burrow__tc_version_error(TypesChecker *check, GtPosn at, Str v, Str msg) {
    GtError *err = burrow__tc_new_error(check, GT_CODE_UNSUPPORTED_FEATURE);
    GT_ADDF(err, at, "%s requires %s or later", msg, v);
    burrow__tc_error_report(err);
}

/* The bailout: a panic the checker catches in types_checker_files, told
 * apart from any other by the flag it sets first. */
void burrow__tc_bailout(TypesChecker *check) {
    check->bailing = true;
    panic_str(BURROW_S("go/types: bailout"));
}
