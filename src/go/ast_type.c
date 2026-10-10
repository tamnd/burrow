/* go/ast: the type descriptors.
 *
 * One struct descriptor and one pointer descriptor for each node, with Go's
 * names for the types and the fields, so that fmt and ast_fprint print a tree
 * the way Go does. A field Go declares as an interface, such as an Expr, is
 * a pointer here, and its descriptor is a named pointer to the header that
 * ast_fprint knows to look through. The struct tables come from ast.go
 * mechanically, field for field.
 *
 * Derived from Go's src/go/ast/ast.go and scope.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>

#define AT_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define AT_FIELD(T, go, c, ft) {AT_STR(go), {NULL, 0}, &(ft), (uint32_t)offsetof(T, c)}
#define AT_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

#define AT_STRUCT(T, go, fields)                                                       \
    const Type burrow_type_##T = {                                                     \
        AT_STR(go),                                                                    \
        AT_STR("go/ast"),                                                              \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        AT_COUNT(fields),                                                              \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define AT_PTR(T)                                                                      \
    const Type burrow_type_##T##Ptr = {                                                \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(void *),                                                      \
        (uint16_t)_Alignof(void *),                                                    \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* A slice or map type with no name, which is all of the ones in go/ast. */
#define AT_UNNAMED(name, kind, size, align, elem, key)                                 \
    static const Type name = {                                                         \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define AT_SLICE(name, elem)                                                           \
    AT_UNNAMED(name, KIND_SLICE, sizeof(Slice), _Alignof(Slice), elem, NULL)
#define AT_MAP(name, elem)                                                             \
    AT_UNNAMED(name, KIND_MAP, sizeof(Map *), _Alignof(Map *), elem, &burrow_type_Str)

/* The header every node starts with, which is what the interface pointers
 * below point at until ast_fprint looks at the kind. */
static const Field at_fields_base[] = {
    {AT_STR("Kind"), {NULL, 0}, &burrow_type_Int, (uint32_t)offsetof(AstBase, kind)},
};

static const Type at_base_type = {
    AT_STR("node"),
    AT_STR("go/ast"),
    KIND_STRUCT,
    (uint32_t)sizeof(AstBase),
    (uint16_t)_Alignof(AstBase),
    AT_COUNT(at_fields_base),
    0,
    at_fields_base,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

#define AT_IFACE(T, go)                                                                \
    const Type burrow_type_##T = {                                                     \
        AT_STR(go),                                                                    \
        AT_STR("go/ast"),                                                              \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(AstNode),                                                     \
        (uint16_t)_Alignof(AstNode),                                                   \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        &at_base_type,                                                                 \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

AT_IFACE(AstNode, "Node");
AT_IFACE(AstExpr, "Expr");
AT_IFACE(AstStmt, "Stmt");
AT_IFACE(AstDecl, "Decl");
AT_IFACE(AstSpec, "Spec");

const Type burrow_type_AstChanDir = {
    AT_STR("ChanDir"),
    AT_STR("go/ast"),
    KIND_INT,
    (uint32_t)sizeof(AstChanDir),
    (uint16_t)_Alignof(AstChanDir),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str at_m_obj_kind_string(AstObjKind *self) {
    return ast_obj_kind_string(*self);
}

#define AT_SIG_STRING(IN, OUT) OUT(Str)
#define AT_OBJ_KIND_METHODS(M, T) M(T, String, at_m_obj_kind_string, AT_SIG_STRING)

BURROW_METHODS_DEFINE(AstObjKind, AT_OBJ_KIND_METHODS);

const Type burrow_type_AstObjKind = {
    AT_STR("ObjKind"),
    AT_STR("go/ast"),
    KIND_INT,
    (uint32_t)sizeof(AstObjKind),
    (uint16_t)_Alignof(AstObjKind),
    0,
    AT_COUNT(burrow__methods_AstObjKind),
    NULL,
    burrow__methods_AstObjKind,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* The lists and maps the nodes hold. */
AT_SLICE(at_slice_expr, &burrow_type_AstExpr);
AT_SLICE(at_slice_stmt, &burrow_type_AstStmt);
AT_SLICE(at_slice_decl, &burrow_type_AstDecl);
AT_SLICE(at_slice_spec, &burrow_type_AstSpec);
AT_SLICE(at_slice_ident, &burrow_type_AstIdentPtr);
AT_SLICE(at_slice_comment, &burrow_type_AstCommentPtr);
AT_SLICE(at_slice_comment_group, &burrow_type_AstCommentGroupPtr);
AT_SLICE(at_slice_field, &burrow_type_AstFieldPtr);
AT_SLICE(at_slice_import_spec, &burrow_type_AstImportSpecPtr);
AT_MAP(at_map_object, &burrow_type_AstObjectPtr);
AT_MAP(at_map_file, &burrow_type_AstFilePtr);

/* ------------------------------------------------------------------- nodes */

static const Field at_fields_comment[] = {
    AT_FIELD(AstComment, "Slash", slash, burrow_type_TokenPos),
    AT_FIELD(AstComment, "Text", text, burrow_type_Str),
};
AT_STRUCT(AstComment, "Comment", at_fields_comment);
AT_PTR(AstComment);

static const Field at_fields_comment_group[] = {
    AT_FIELD(AstCommentGroup, "List", list, at_slice_comment),
};
AT_STRUCT(AstCommentGroup, "CommentGroup", at_fields_comment_group);
AT_PTR(AstCommentGroup);

static const Field at_fields_field[] = {
    AT_FIELD(AstField, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstField, "Names", names, at_slice_ident),
    AT_FIELD(AstField, "Type", type, burrow_type_AstExpr),
    AT_FIELD(AstField, "Tag", tag, burrow_type_AstBasicLitPtr),
    AT_FIELD(AstField, "Comment", comment, burrow_type_AstCommentGroupPtr),
};
AT_STRUCT(AstField, "Field", at_fields_field);
AT_PTR(AstField);

static const Field at_fields_field_list[] = {
    AT_FIELD(AstFieldList, "Opening", opening, burrow_type_TokenPos),
    AT_FIELD(AstFieldList, "List", list, at_slice_field),
    AT_FIELD(AstFieldList, "Closing", closing, burrow_type_TokenPos),
};
AT_STRUCT(AstFieldList, "FieldList", at_fields_field_list);
AT_PTR(AstFieldList);

static const Field at_fields_bad_expr[] = {
    AT_FIELD(AstBadExpr, "From", from, burrow_type_TokenPos),
    AT_FIELD(AstBadExpr, "To", to, burrow_type_TokenPos),
};
AT_STRUCT(AstBadExpr, "BadExpr", at_fields_bad_expr);
AT_PTR(AstBadExpr);

static const Field at_fields_ident[] = {
    AT_FIELD(AstIdent, "NamePos", name_pos, burrow_type_TokenPos),
    AT_FIELD(AstIdent, "Name", name, burrow_type_Str),
    AT_FIELD(AstIdent, "Obj", obj, burrow_type_AstObjectPtr),
};
AT_STRUCT(AstIdent, "Ident", at_fields_ident);
AT_PTR(AstIdent);

static const Field at_fields_ellipsis[] = {
    AT_FIELD(AstEllipsis, "Ellipsis", ellipsis, burrow_type_TokenPos),
    AT_FIELD(AstEllipsis, "Elt", elt, burrow_type_AstExpr),
};
AT_STRUCT(AstEllipsis, "Ellipsis", at_fields_ellipsis);
AT_PTR(AstEllipsis);

static const Field at_fields_basic_lit[] = {
    AT_FIELD(AstBasicLit, "ValuePos", value_pos, burrow_type_TokenPos),
    AT_FIELD(AstBasicLit, "ValueEnd", value_end, burrow_type_TokenPos),
    AT_FIELD(AstBasicLit, "Kind", kind, burrow_type_Token),
    AT_FIELD(AstBasicLit, "Value", value, burrow_type_Str),
};
AT_STRUCT(AstBasicLit, "BasicLit", at_fields_basic_lit);
AT_PTR(AstBasicLit);

static const Field at_fields_func_lit[] = {
    AT_FIELD(AstFuncLit, "Type", type, burrow_type_AstFuncTypePtr),
    AT_FIELD(AstFuncLit, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstFuncLit, "FuncLit", at_fields_func_lit);
AT_PTR(AstFuncLit);

static const Field at_fields_composite_lit[] = {
    AT_FIELD(AstCompositeLit, "Type", type, burrow_type_AstExpr),
    AT_FIELD(AstCompositeLit, "Lbrace", lbrace, burrow_type_TokenPos),
    AT_FIELD(AstCompositeLit, "Elts", elts, at_slice_expr),
    AT_FIELD(AstCompositeLit, "Rbrace", rbrace, burrow_type_TokenPos),
    AT_FIELD(AstCompositeLit, "Incomplete", incomplete, burrow_type_bool),
};
AT_STRUCT(AstCompositeLit, "CompositeLit", at_fields_composite_lit);
AT_PTR(AstCompositeLit);

static const Field at_fields_paren_expr[] = {
    AT_FIELD(AstParenExpr, "Lparen", lparen, burrow_type_TokenPos),
    AT_FIELD(AstParenExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstParenExpr, "Rparen", rparen, burrow_type_TokenPos),
};
AT_STRUCT(AstParenExpr, "ParenExpr", at_fields_paren_expr);
AT_PTR(AstParenExpr);

static const Field at_fields_selector_expr[] = {
    AT_FIELD(AstSelectorExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstSelectorExpr, "Sel", sel, burrow_type_AstIdentPtr),
};
AT_STRUCT(AstSelectorExpr, "SelectorExpr", at_fields_selector_expr);
AT_PTR(AstSelectorExpr);

static const Field at_fields_index_expr[] = {
    AT_FIELD(AstIndexExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstIndexExpr, "Lbrack", lbrack, burrow_type_TokenPos),
    AT_FIELD(AstIndexExpr, "Index", index, burrow_type_AstExpr),
    AT_FIELD(AstIndexExpr, "Rbrack", rbrack, burrow_type_TokenPos),
};
AT_STRUCT(AstIndexExpr, "IndexExpr", at_fields_index_expr);
AT_PTR(AstIndexExpr);

static const Field at_fields_index_list_expr[] = {
    AT_FIELD(AstIndexListExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstIndexListExpr, "Lbrack", lbrack, burrow_type_TokenPos),
    AT_FIELD(AstIndexListExpr, "Indices", indices, at_slice_expr),
    AT_FIELD(AstIndexListExpr, "Rbrack", rbrack, burrow_type_TokenPos),
};
AT_STRUCT(AstIndexListExpr, "IndexListExpr", at_fields_index_list_expr);
AT_PTR(AstIndexListExpr);

static const Field at_fields_slice_expr[] = {
    AT_FIELD(AstSliceExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstSliceExpr, "Lbrack", lbrack, burrow_type_TokenPos),
    AT_FIELD(AstSliceExpr, "Low", low, burrow_type_AstExpr),
    AT_FIELD(AstSliceExpr, "High", high, burrow_type_AstExpr),
    AT_FIELD(AstSliceExpr, "Max", max, burrow_type_AstExpr),
    AT_FIELD(AstSliceExpr, "Slice3", slice3, burrow_type_bool),
    AT_FIELD(AstSliceExpr, "Rbrack", rbrack, burrow_type_TokenPos),
};
AT_STRUCT(AstSliceExpr, "SliceExpr", at_fields_slice_expr);
AT_PTR(AstSliceExpr);

static const Field at_fields_type_assert_expr[] = {
    AT_FIELD(AstTypeAssertExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstTypeAssertExpr, "Lparen", lparen, burrow_type_TokenPos),
    AT_FIELD(AstTypeAssertExpr, "Type", type, burrow_type_AstExpr),
    AT_FIELD(AstTypeAssertExpr, "Rparen", rparen, burrow_type_TokenPos),
};
AT_STRUCT(AstTypeAssertExpr, "TypeAssertExpr", at_fields_type_assert_expr);
AT_PTR(AstTypeAssertExpr);

static const Field at_fields_call_expr[] = {
    AT_FIELD(AstCallExpr, "Fun", fun, burrow_type_AstExpr),
    AT_FIELD(AstCallExpr, "Lparen", lparen, burrow_type_TokenPos),
    AT_FIELD(AstCallExpr, "Args", args, at_slice_expr),
    AT_FIELD(AstCallExpr, "Ellipsis", ellipsis, burrow_type_TokenPos),
    AT_FIELD(AstCallExpr, "Rparen", rparen, burrow_type_TokenPos),
};
AT_STRUCT(AstCallExpr, "CallExpr", at_fields_call_expr);
AT_PTR(AstCallExpr);

static const Field at_fields_star_expr[] = {
    AT_FIELD(AstStarExpr, "Star", star, burrow_type_TokenPos),
    AT_FIELD(AstStarExpr, "X", x, burrow_type_AstExpr),
};
AT_STRUCT(AstStarExpr, "StarExpr", at_fields_star_expr);
AT_PTR(AstStarExpr);

static const Field at_fields_unary_expr[] = {
    AT_FIELD(AstUnaryExpr, "OpPos", op_pos, burrow_type_TokenPos),
    AT_FIELD(AstUnaryExpr, "Op", op, burrow_type_Token),
    AT_FIELD(AstUnaryExpr, "X", x, burrow_type_AstExpr),
};
AT_STRUCT(AstUnaryExpr, "UnaryExpr", at_fields_unary_expr);
AT_PTR(AstUnaryExpr);

static const Field at_fields_binary_expr[] = {
    AT_FIELD(AstBinaryExpr, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstBinaryExpr, "OpPos", op_pos, burrow_type_TokenPos),
    AT_FIELD(AstBinaryExpr, "Op", op, burrow_type_Token),
    AT_FIELD(AstBinaryExpr, "Y", y, burrow_type_AstExpr),
};
AT_STRUCT(AstBinaryExpr, "BinaryExpr", at_fields_binary_expr);
AT_PTR(AstBinaryExpr);

static const Field at_fields_key_value_expr[] = {
    AT_FIELD(AstKeyValueExpr, "Key", key, burrow_type_AstExpr),
    AT_FIELD(AstKeyValueExpr, "Colon", colon, burrow_type_TokenPos),
    AT_FIELD(AstKeyValueExpr, "Value", value, burrow_type_AstExpr),
};
AT_STRUCT(AstKeyValueExpr, "KeyValueExpr", at_fields_key_value_expr);
AT_PTR(AstKeyValueExpr);

static const Field at_fields_array_type[] = {
    AT_FIELD(AstArrayType, "Lbrack", lbrack, burrow_type_TokenPos),
    AT_FIELD(AstArrayType, "Len", len, burrow_type_AstExpr),
    AT_FIELD(AstArrayType, "Elt", elt, burrow_type_AstExpr),
};
AT_STRUCT(AstArrayType, "ArrayType", at_fields_array_type);
AT_PTR(AstArrayType);

static const Field at_fields_struct_type[] = {
    AT_FIELD(AstStructType, "Struct", struct_, burrow_type_TokenPos),
    AT_FIELD(AstStructType, "Fields", fields, burrow_type_AstFieldListPtr),
    AT_FIELD(AstStructType, "Incomplete", incomplete, burrow_type_bool),
};
AT_STRUCT(AstStructType, "StructType", at_fields_struct_type);
AT_PTR(AstStructType);

static const Field at_fields_func_type[] = {
    AT_FIELD(AstFuncType, "Func", func, burrow_type_TokenPos),
    AT_FIELD(AstFuncType, "TypeParams", type_params, burrow_type_AstFieldListPtr),
    AT_FIELD(AstFuncType, "Params", params, burrow_type_AstFieldListPtr),
    AT_FIELD(AstFuncType, "Results", results, burrow_type_AstFieldListPtr),
};
AT_STRUCT(AstFuncType, "FuncType", at_fields_func_type);
AT_PTR(AstFuncType);

static const Field at_fields_interface_type[] = {
    AT_FIELD(AstInterfaceType, "Interface", interface_, burrow_type_TokenPos),
    AT_FIELD(AstInterfaceType, "Methods", methods, burrow_type_AstFieldListPtr),
    AT_FIELD(AstInterfaceType, "Incomplete", incomplete, burrow_type_bool),
};
AT_STRUCT(AstInterfaceType, "InterfaceType", at_fields_interface_type);
AT_PTR(AstInterfaceType);

static const Field at_fields_map_type[] = {
    AT_FIELD(AstMapType, "Map", map, burrow_type_TokenPos),
    AT_FIELD(AstMapType, "Key", key, burrow_type_AstExpr),
    AT_FIELD(AstMapType, "Value", value, burrow_type_AstExpr),
};
AT_STRUCT(AstMapType, "MapType", at_fields_map_type);
AT_PTR(AstMapType);

static const Field at_fields_chan_type[] = {
    AT_FIELD(AstChanType, "Begin", begin, burrow_type_TokenPos),
    AT_FIELD(AstChanType, "Arrow", arrow, burrow_type_TokenPos),
    AT_FIELD(AstChanType, "Dir", dir, burrow_type_AstChanDir),
    AT_FIELD(AstChanType, "Value", value, burrow_type_AstExpr),
};
AT_STRUCT(AstChanType, "ChanType", at_fields_chan_type);
AT_PTR(AstChanType);

static const Field at_fields_bad_stmt[] = {
    AT_FIELD(AstBadStmt, "From", from, burrow_type_TokenPos),
    AT_FIELD(AstBadStmt, "To", to, burrow_type_TokenPos),
};
AT_STRUCT(AstBadStmt, "BadStmt", at_fields_bad_stmt);
AT_PTR(AstBadStmt);

static const Field at_fields_decl_stmt[] = {
    AT_FIELD(AstDeclStmt, "Decl", decl, burrow_type_AstDecl),
};
AT_STRUCT(AstDeclStmt, "DeclStmt", at_fields_decl_stmt);
AT_PTR(AstDeclStmt);

static const Field at_fields_empty_stmt[] = {
    AT_FIELD(AstEmptyStmt, "Semicolon", semicolon, burrow_type_TokenPos),
    AT_FIELD(AstEmptyStmt, "Implicit", implicit, burrow_type_bool),
};
AT_STRUCT(AstEmptyStmt, "EmptyStmt", at_fields_empty_stmt);
AT_PTR(AstEmptyStmt);

static const Field at_fields_labeled_stmt[] = {
    AT_FIELD(AstLabeledStmt, "Label", label, burrow_type_AstIdentPtr),
    AT_FIELD(AstLabeledStmt, "Colon", colon, burrow_type_TokenPos),
    AT_FIELD(AstLabeledStmt, "Stmt", stmt, burrow_type_AstStmt),
};
AT_STRUCT(AstLabeledStmt, "LabeledStmt", at_fields_labeled_stmt);
AT_PTR(AstLabeledStmt);

static const Field at_fields_expr_stmt[] = {
    AT_FIELD(AstExprStmt, "X", x, burrow_type_AstExpr),
};
AT_STRUCT(AstExprStmt, "ExprStmt", at_fields_expr_stmt);
AT_PTR(AstExprStmt);

static const Field at_fields_send_stmt[] = {
    AT_FIELD(AstSendStmt, "Chan", chan, burrow_type_AstExpr),
    AT_FIELD(AstSendStmt, "Arrow", arrow, burrow_type_TokenPos),
    AT_FIELD(AstSendStmt, "Value", value, burrow_type_AstExpr),
};
AT_STRUCT(AstSendStmt, "SendStmt", at_fields_send_stmt);
AT_PTR(AstSendStmt);

static const Field at_fields_inc_dec_stmt[] = {
    AT_FIELD(AstIncDecStmt, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstIncDecStmt, "TokPos", tok_pos, burrow_type_TokenPos),
    AT_FIELD(AstIncDecStmt, "Tok", tok, burrow_type_Token),
};
AT_STRUCT(AstIncDecStmt, "IncDecStmt", at_fields_inc_dec_stmt);
AT_PTR(AstIncDecStmt);

static const Field at_fields_assign_stmt[] = {
    AT_FIELD(AstAssignStmt, "Lhs", lhs, at_slice_expr),
    AT_FIELD(AstAssignStmt, "TokPos", tok_pos, burrow_type_TokenPos),
    AT_FIELD(AstAssignStmt, "Tok", tok, burrow_type_Token),
    AT_FIELD(AstAssignStmt, "Rhs", rhs, at_slice_expr),
};
AT_STRUCT(AstAssignStmt, "AssignStmt", at_fields_assign_stmt);
AT_PTR(AstAssignStmt);

static const Field at_fields_go_stmt[] = {
    AT_FIELD(AstGoStmt, "Go", go, burrow_type_TokenPos),
    AT_FIELD(AstGoStmt, "Call", call, burrow_type_AstCallExprPtr),
};
AT_STRUCT(AstGoStmt, "GoStmt", at_fields_go_stmt);
AT_PTR(AstGoStmt);

static const Field at_fields_defer_stmt[] = {
    AT_FIELD(AstDeferStmt, "Defer", defer, burrow_type_TokenPos),
    AT_FIELD(AstDeferStmt, "Call", call, burrow_type_AstCallExprPtr),
};
AT_STRUCT(AstDeferStmt, "DeferStmt", at_fields_defer_stmt);
AT_PTR(AstDeferStmt);

static const Field at_fields_return_stmt[] = {
    AT_FIELD(AstReturnStmt, "Return", return_, burrow_type_TokenPos),
    AT_FIELD(AstReturnStmt, "Results", results, at_slice_expr),
};
AT_STRUCT(AstReturnStmt, "ReturnStmt", at_fields_return_stmt);
AT_PTR(AstReturnStmt);

static const Field at_fields_branch_stmt[] = {
    AT_FIELD(AstBranchStmt, "TokPos", tok_pos, burrow_type_TokenPos),
    AT_FIELD(AstBranchStmt, "Tok", tok, burrow_type_Token),
    AT_FIELD(AstBranchStmt, "Label", label, burrow_type_AstIdentPtr),
};
AT_STRUCT(AstBranchStmt, "BranchStmt", at_fields_branch_stmt);
AT_PTR(AstBranchStmt);

static const Field at_fields_block_stmt[] = {
    AT_FIELD(AstBlockStmt, "Lbrace", lbrace, burrow_type_TokenPos),
    AT_FIELD(AstBlockStmt, "List", list, at_slice_stmt),
    AT_FIELD(AstBlockStmt, "Rbrace", rbrace, burrow_type_TokenPos),
};
AT_STRUCT(AstBlockStmt, "BlockStmt", at_fields_block_stmt);
AT_PTR(AstBlockStmt);

static const Field at_fields_if_stmt[] = {
    AT_FIELD(AstIfStmt, "If", if_, burrow_type_TokenPos),
    AT_FIELD(AstIfStmt, "Init", init, burrow_type_AstStmt),
    AT_FIELD(AstIfStmt, "Cond", cond, burrow_type_AstExpr),
    AT_FIELD(AstIfStmt, "Body", body, burrow_type_AstBlockStmtPtr),
    AT_FIELD(AstIfStmt, "Else", else_, burrow_type_AstStmt),
};
AT_STRUCT(AstIfStmt, "IfStmt", at_fields_if_stmt);
AT_PTR(AstIfStmt);

static const Field at_fields_case_clause[] = {
    AT_FIELD(AstCaseClause, "Case", case_, burrow_type_TokenPos),
    AT_FIELD(AstCaseClause, "List", list, at_slice_expr),
    AT_FIELD(AstCaseClause, "Colon", colon, burrow_type_TokenPos),
    AT_FIELD(AstCaseClause, "Body", body, at_slice_stmt),
};
AT_STRUCT(AstCaseClause, "CaseClause", at_fields_case_clause);
AT_PTR(AstCaseClause);

static const Field at_fields_switch_stmt[] = {
    AT_FIELD(AstSwitchStmt, "Switch", switch_, burrow_type_TokenPos),
    AT_FIELD(AstSwitchStmt, "Init", init, burrow_type_AstStmt),
    AT_FIELD(AstSwitchStmt, "Tag", tag, burrow_type_AstExpr),
    AT_FIELD(AstSwitchStmt, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstSwitchStmt, "SwitchStmt", at_fields_switch_stmt);
AT_PTR(AstSwitchStmt);

static const Field at_fields_type_switch_stmt[] = {
    AT_FIELD(AstTypeSwitchStmt, "Switch", switch_, burrow_type_TokenPos),
    AT_FIELD(AstTypeSwitchStmt, "Init", init, burrow_type_AstStmt),
    AT_FIELD(AstTypeSwitchStmt, "Assign", assign, burrow_type_AstStmt),
    AT_FIELD(AstTypeSwitchStmt, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstTypeSwitchStmt, "TypeSwitchStmt", at_fields_type_switch_stmt);
AT_PTR(AstTypeSwitchStmt);

static const Field at_fields_comm_clause[] = {
    AT_FIELD(AstCommClause, "Case", case_, burrow_type_TokenPos),
    AT_FIELD(AstCommClause, "Comm", comm, burrow_type_AstStmt),
    AT_FIELD(AstCommClause, "Colon", colon, burrow_type_TokenPos),
    AT_FIELD(AstCommClause, "Body", body, at_slice_stmt),
};
AT_STRUCT(AstCommClause, "CommClause", at_fields_comm_clause);
AT_PTR(AstCommClause);

static const Field at_fields_select_stmt[] = {
    AT_FIELD(AstSelectStmt, "Select", select, burrow_type_TokenPos),
    AT_FIELD(AstSelectStmt, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstSelectStmt, "SelectStmt", at_fields_select_stmt);
AT_PTR(AstSelectStmt);

static const Field at_fields_for_stmt[] = {
    AT_FIELD(AstForStmt, "For", for_, burrow_type_TokenPos),
    AT_FIELD(AstForStmt, "Init", init, burrow_type_AstStmt),
    AT_FIELD(AstForStmt, "Cond", cond, burrow_type_AstExpr),
    AT_FIELD(AstForStmt, "Post", post, burrow_type_AstStmt),
    AT_FIELD(AstForStmt, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstForStmt, "ForStmt", at_fields_for_stmt);
AT_PTR(AstForStmt);

static const Field at_fields_range_stmt[] = {
    AT_FIELD(AstRangeStmt, "For", for_, burrow_type_TokenPos),
    AT_FIELD(AstRangeStmt, "Key", key, burrow_type_AstExpr),
    AT_FIELD(AstRangeStmt, "Value", value, burrow_type_AstExpr),
    AT_FIELD(AstRangeStmt, "TokPos", tok_pos, burrow_type_TokenPos),
    AT_FIELD(AstRangeStmt, "Tok", tok, burrow_type_Token),
    AT_FIELD(AstRangeStmt, "Range", range, burrow_type_TokenPos),
    AT_FIELD(AstRangeStmt, "X", x, burrow_type_AstExpr),
    AT_FIELD(AstRangeStmt, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstRangeStmt, "RangeStmt", at_fields_range_stmt);
AT_PTR(AstRangeStmt);

static const Field at_fields_import_spec[] = {
    AT_FIELD(AstImportSpec, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstImportSpec, "Name", name, burrow_type_AstIdentPtr),
    AT_FIELD(AstImportSpec, "Path", path, burrow_type_AstBasicLitPtr),
    AT_FIELD(AstImportSpec, "Comment", comment, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstImportSpec, "EndPos", end_pos, burrow_type_TokenPos),
};
AT_STRUCT(AstImportSpec, "ImportSpec", at_fields_import_spec);
AT_PTR(AstImportSpec);

static const Field at_fields_value_spec[] = {
    AT_FIELD(AstValueSpec, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstValueSpec, "Names", names, at_slice_ident),
    AT_FIELD(AstValueSpec, "Type", type, burrow_type_AstExpr),
    AT_FIELD(AstValueSpec, "Values", values, at_slice_expr),
    AT_FIELD(AstValueSpec, "Comment", comment, burrow_type_AstCommentGroupPtr),
};
AT_STRUCT(AstValueSpec, "ValueSpec", at_fields_value_spec);
AT_PTR(AstValueSpec);

static const Field at_fields_type_spec[] = {
    AT_FIELD(AstTypeSpec, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstTypeSpec, "Name", name, burrow_type_AstIdentPtr),
    AT_FIELD(AstTypeSpec, "TypeParams", type_params, burrow_type_AstFieldListPtr),
    AT_FIELD(AstTypeSpec, "Assign", assign, burrow_type_TokenPos),
    AT_FIELD(AstTypeSpec, "Type", type, burrow_type_AstExpr),
    AT_FIELD(AstTypeSpec, "Comment", comment, burrow_type_AstCommentGroupPtr),
};
AT_STRUCT(AstTypeSpec, "TypeSpec", at_fields_type_spec);
AT_PTR(AstTypeSpec);

static const Field at_fields_bad_decl[] = {
    AT_FIELD(AstBadDecl, "From", from, burrow_type_TokenPos),
    AT_FIELD(AstBadDecl, "To", to, burrow_type_TokenPos),
};
AT_STRUCT(AstBadDecl, "BadDecl", at_fields_bad_decl);
AT_PTR(AstBadDecl);

static const Field at_fields_gen_decl[] = {
    AT_FIELD(AstGenDecl, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstGenDecl, "TokPos", tok_pos, burrow_type_TokenPos),
    AT_FIELD(AstGenDecl, "Tok", tok, burrow_type_Token),
    AT_FIELD(AstGenDecl, "Lparen", lparen, burrow_type_TokenPos),
    AT_FIELD(AstGenDecl, "Specs", specs, at_slice_spec),
    AT_FIELD(AstGenDecl, "Rparen", rparen, burrow_type_TokenPos),
};
AT_STRUCT(AstGenDecl, "GenDecl", at_fields_gen_decl);
AT_PTR(AstGenDecl);

static const Field at_fields_func_decl[] = {
    AT_FIELD(AstFuncDecl, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstFuncDecl, "Recv", recv, burrow_type_AstFieldListPtr),
    AT_FIELD(AstFuncDecl, "Name", name, burrow_type_AstIdentPtr),
    AT_FIELD(AstFuncDecl, "Type", type, burrow_type_AstFuncTypePtr),
    AT_FIELD(AstFuncDecl, "Body", body, burrow_type_AstBlockStmtPtr),
};
AT_STRUCT(AstFuncDecl, "FuncDecl", at_fields_func_decl);
AT_PTR(AstFuncDecl);

static const Field at_fields_file[] = {
    AT_FIELD(AstFile, "Doc", doc, burrow_type_AstCommentGroupPtr),
    AT_FIELD(AstFile, "Package", package, burrow_type_TokenPos),
    AT_FIELD(AstFile, "Name", name, burrow_type_AstIdentPtr),
    AT_FIELD(AstFile, "Decls", decls, at_slice_decl),
    AT_FIELD(AstFile, "FileStart", file_start, burrow_type_TokenPos),
    AT_FIELD(AstFile, "FileEnd", file_end, burrow_type_TokenPos),
    AT_FIELD(AstFile, "Scope", scope, burrow_type_AstScopePtr),
    AT_FIELD(AstFile, "Imports", imports, at_slice_import_spec),
    AT_FIELD(AstFile, "Unresolved", unresolved, at_slice_ident),
    AT_FIELD(AstFile, "Comments", comments, at_slice_comment_group),
    AT_FIELD(AstFile, "GoVersion", go_version, burrow_type_Str),
};
AT_STRUCT(AstFile, "File", at_fields_file);
AT_PTR(AstFile);

static const Field at_fields_package[] = {
    AT_FIELD(AstPackage, "Name", name, burrow_type_Str),
    AT_FIELD(AstPackage, "Scope", scope, burrow_type_AstScopePtr),
    AT_FIELD(AstPackage, "Imports", imports, at_map_object),
    AT_FIELD(AstPackage, "Files", files, at_map_file),
};
AT_STRUCT(AstPackage, "Package", at_fields_package);
AT_PTR(AstPackage);

/* ---------------------------------------------------- objects and scopes */

static const Field at_fields_object[] = {
    AT_FIELD(AstObject, "Kind", kind, burrow_type_AstObjKind),
    AT_FIELD(AstObject, "Name", name, burrow_type_Str),
    AT_FIELD(AstObject, "Decl", decl, burrow_type_Any),
    AT_FIELD(AstObject, "Data", data, burrow_type_Any),
    AT_FIELD(AstObject, "Type", type, burrow_type_Any),
};
AT_STRUCT(AstObject, "Object", at_fields_object);
AT_PTR(AstObject);

static const Field at_fields_scope[] = {
    AT_FIELD(AstScope, "Outer", outer, burrow_type_AstScopePtr),
    AT_FIELD(AstScope, "Objects", objects, at_map_object),
};
AT_STRUCT(AstScope, "Scope", at_fields_scope);
AT_PTR(AstScope);

/* ------------------------------------------------------------- directives */

static const Field at_fields_directive[] = {
    AT_FIELD(AstDirective, "Tool", tool, burrow_type_Str),
    AT_FIELD(AstDirective, "Name", name, burrow_type_Str),
    AT_FIELD(AstDirective, "Args", args, burrow_type_Str),
    AT_FIELD(AstDirective, "Slash", slash, burrow_type_TokenPos),
    AT_FIELD(AstDirective, "ArgsPos", args_pos, burrow_type_TokenPos),
};
AT_STRUCT(AstDirective, "Directive", at_fields_directive);

static const Field at_fields_directive_arg[] = {
    AT_FIELD(AstDirectiveArg, "Arg", arg, burrow_type_Str),
    AT_FIELD(AstDirectiveArg, "Pos", pos, burrow_type_TokenPos),
};
AT_STRUCT(AstDirectiveArg, "DirectiveArg", at_fields_directive_arg);

/* ------------------------------------------------------------ by the kind */

static const Type *const at_node_types[AST_KIND_COUNT] = {
    NULL,
    &burrow_type_AstComment,
    &burrow_type_AstCommentGroup,
    &burrow_type_AstField,
    &burrow_type_AstFieldList,
    &burrow_type_AstBadExpr,
    &burrow_type_AstIdent,
    &burrow_type_AstEllipsis,
    &burrow_type_AstBasicLit,
    &burrow_type_AstFuncLit,
    &burrow_type_AstCompositeLit,
    &burrow_type_AstParenExpr,
    &burrow_type_AstSelectorExpr,
    &burrow_type_AstIndexExpr,
    &burrow_type_AstIndexListExpr,
    &burrow_type_AstSliceExpr,
    &burrow_type_AstTypeAssertExpr,
    &burrow_type_AstCallExpr,
    &burrow_type_AstStarExpr,
    &burrow_type_AstUnaryExpr,
    &burrow_type_AstBinaryExpr,
    &burrow_type_AstKeyValueExpr,
    &burrow_type_AstArrayType,
    &burrow_type_AstStructType,
    &burrow_type_AstFuncType,
    &burrow_type_AstInterfaceType,
    &burrow_type_AstMapType,
    &burrow_type_AstChanType,
    &burrow_type_AstBadStmt,
    &burrow_type_AstDeclStmt,
    &burrow_type_AstEmptyStmt,
    &burrow_type_AstLabeledStmt,
    &burrow_type_AstExprStmt,
    &burrow_type_AstSendStmt,
    &burrow_type_AstIncDecStmt,
    &burrow_type_AstAssignStmt,
    &burrow_type_AstGoStmt,
    &burrow_type_AstDeferStmt,
    &burrow_type_AstReturnStmt,
    &burrow_type_AstBranchStmt,
    &burrow_type_AstBlockStmt,
    &burrow_type_AstIfStmt,
    &burrow_type_AstCaseClause,
    &burrow_type_AstSwitchStmt,
    &burrow_type_AstTypeSwitchStmt,
    &burrow_type_AstCommClause,
    &burrow_type_AstSelectStmt,
    &burrow_type_AstForStmt,
    &burrow_type_AstRangeStmt,
    &burrow_type_AstImportSpec,
    &burrow_type_AstValueSpec,
    &burrow_type_AstTypeSpec,
    &burrow_type_AstBadDecl,
    &burrow_type_AstGenDecl,
    &burrow_type_AstFuncDecl,
    &burrow_type_AstFile,
    &burrow_type_AstPackage,
};

const Type *ast_node_type(AstNode n) {
    if (n == NULL || n->kind <= AST_KIND_INVALID || n->kind >= AST_KIND_COUNT) {
        return NULL;
    }
    return at_node_types[n->kind];
}

AstNode ast_node_new(Alloc *a, AstKind kind) {
    const Type *t =
        (kind > AST_KIND_INVALID && kind < AST_KIND_COUNT) ? at_node_types[kind] : NULL;
    if (t == NULL) {
        return NULL;
    }
    AstNode n = (AstNode)mem_alloc(a, t->size, t->align);
    if (n == NULL) {
        return NULL;
    }
    n->kind = kind;
    return n;
}
