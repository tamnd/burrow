/* go/ast, the syntax tree of Go source.
 *
 * Go's go/ast. Every node is a struct whose first member is an AstBase, and
 * the AstBase says which node it is. AstNode is a pointer to that header, so
 * any node can be passed where an AstNode is wanted by taking the address of
 * its header, and turned back into its own type by looking at the kind:
 *
 *     AstIdent *x = ast_new_ident(a, BURROW_S("x"));
 *     AstBasicLit one = {{AST_KIND_BASIC_LIT}, 0, 0, TOKEN_INT, BURROW_S("1")};
 *     AstBinaryExpr sum = {{AST_KIND_BINARY_EXPR}, &x->node, 0, TOKEN_ADD, &one.node};
 *
 *     AstNode n = &sum.node;
 *     if (n->kind == AST_KIND_BINARY_EXPR) {
 *         AstBinaryExpr *b = (AstBinaryExpr *)n;
 *         // ...
 *     }
 *
 * Go's Expr, Stmt, Decl and Spec interfaces are all AstNode here, under four
 * names so that a field says what Go says it holds. Nothing stops a statement
 * going where an expression should, any more than a type assertion does in Go,
 * and the walker panics on what it does not expect to find.
 *
 * Memory. A tree is a graph of plain pointers into memory nobody owns on their
 * own, the way Go's is, so build it in an arena and free the arena with
 * everything in it. A list of nodes is a Slice of pointers, and the TYPE_AST_*
 * descriptors below are what to make one with. Strings in a node are views,
 * and whatever they point at has to outlive the tree.
 *
 * The functions here only read a tree, apart from the filters, ast_sort_imports
 * and ast_comment_map_update, which change it in place as Go's do. None of
 * them is safe to run on one tree from two threads while anything changes it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/ast */

#ifndef BURROW_GO_AST_H
#define BURROW_GO_AST_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/iter.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- nodes */

/* Which node a struct is, one for each of Go's node types, in the order
 * go/ast declares them. */
typedef enum AstKind {
    AST_KIND_INVALID = 0,
    AST_KIND_COMMENT,
    AST_KIND_COMMENT_GROUP,
    AST_KIND_FIELD,
    AST_KIND_FIELD_LIST,
    /* expressions */
    AST_KIND_BAD_EXPR,
    AST_KIND_IDENT,
    AST_KIND_ELLIPSIS,
    AST_KIND_BASIC_LIT,
    AST_KIND_FUNC_LIT,
    AST_KIND_COMPOSITE_LIT,
    AST_KIND_PAREN_EXPR,
    AST_KIND_SELECTOR_EXPR,
    AST_KIND_INDEX_EXPR,
    AST_KIND_INDEX_LIST_EXPR,
    AST_KIND_SLICE_EXPR,
    AST_KIND_TYPE_ASSERT_EXPR,
    AST_KIND_CALL_EXPR,
    AST_KIND_STAR_EXPR,
    AST_KIND_UNARY_EXPR,
    AST_KIND_BINARY_EXPR,
    AST_KIND_KEY_VALUE_EXPR,
    /* types, which are expressions too */
    AST_KIND_ARRAY_TYPE,
    AST_KIND_STRUCT_TYPE,
    AST_KIND_FUNC_TYPE,
    AST_KIND_INTERFACE_TYPE,
    AST_KIND_MAP_TYPE,
    AST_KIND_CHAN_TYPE,
    /* statements */
    AST_KIND_BAD_STMT,
    AST_KIND_DECL_STMT,
    AST_KIND_EMPTY_STMT,
    AST_KIND_LABELED_STMT,
    AST_KIND_EXPR_STMT,
    AST_KIND_SEND_STMT,
    AST_KIND_INC_DEC_STMT,
    AST_KIND_ASSIGN_STMT,
    AST_KIND_GO_STMT,
    AST_KIND_DEFER_STMT,
    AST_KIND_RETURN_STMT,
    AST_KIND_BRANCH_STMT,
    AST_KIND_BLOCK_STMT,
    AST_KIND_IF_STMT,
    AST_KIND_CASE_CLAUSE,
    AST_KIND_SWITCH_STMT,
    AST_KIND_TYPE_SWITCH_STMT,
    AST_KIND_COMM_CLAUSE,
    AST_KIND_SELECT_STMT,
    AST_KIND_FOR_STMT,
    AST_KIND_RANGE_STMT,
    /* specifications */
    AST_KIND_IMPORT_SPEC,
    AST_KIND_VALUE_SPEC,
    AST_KIND_TYPE_SPEC,
    /* declarations */
    AST_KIND_BAD_DECL,
    AST_KIND_GEN_DECL,
    AST_KIND_FUNC_DECL,
    AST_KIND_FILE,
    AST_KIND_PACKAGE,
    AST_KIND_COUNT /* not a kind, just how many there are */
} AstKind;

/* The header every node starts with. kind holds an AstKind, in an Int so that
 * the header is aligned like any node and a cast from one to the other is not
 * a cast to a stricter alignment. */
typedef struct AstBase {
    Int kind;
} AstBase;

/* ast.Node, and Go's four narrower interfaces, which are the same thing here.
 * A NULL one is Go's nil. */
typedef AstBase *AstNode;
typedef AstNode AstExpr;
typedef AstNode AstStmt;
typedef AstNode AstDecl;
typedef AstNode AstSpec;

/* A zeroed node of the given kind, made in a, with its header filled in, or
 * NULL if a is out of memory. What Go writes as &ast.Ident{} is
 * (AstIdent *)ast_node_new(a, AST_KIND_IDENT), or just a struct literal with
 * the kind in its header when the node does not need to outlive the block. */
BURROW_OWNS(ret) AstNode ast_node_new(Alloc *a, AstKind kind);

/* Node.Pos and Node.End: where n starts, and where the byte just after it is.
 * They work on every kind, and the narrower ones are the same function under
 * the name of the interface Go calls it through. */
TokenPos ast_node_pos(AstNode n);
TokenPos ast_node_end(AstNode n);
TokenPos ast_expr_pos(AstExpr x);
TokenPos ast_expr_end(AstExpr x);
TokenPos ast_stmt_pos(AstStmt s);
TokenPos ast_stmt_end(AstStmt s);
TokenPos ast_decl_pos(AstDecl d);
TokenPos ast_decl_end(AstDecl d);
TokenPos ast_spec_pos(AstSpec s);
TokenPos ast_spec_end(AstSpec s);

/* --------------------------------------------------------------- comments */

/* ast.Comment, a single // or slash-star comment. text is the whole comment,
 * markers included, without the carriage returns or the newline that ends a
 * line comment. */
typedef struct AstComment {
    AstBase node;
    TokenPos slash; /* where the / that starts it is */
    Str text;
} AstComment;

TokenPos ast_comment_pos(AstComment *c);
TokenPos ast_comment_end(AstComment *c);

/* ast.CommentGroup, comments with nothing but whitespace between them. list
 * holds AstComment pointers and is never empty. */
typedef struct AstCommentGroup {
    AstBase node;
    Slice list;
} AstCommentGroup;

TokenPos ast_comment_group_pos(AstCommentGroup *g);
TokenPos ast_comment_group_end(AstCommentGroup *g);

/* CommentGroup.Text: the text of the comments with their markers, the first
 * space of a line comment, blank lines at either end, trailing spaces and
 * directives such as //go:noinline taken out, and runs of blank lines made
 * one. The result ends in a newline unless it is empty, and a NULL g gives
 * the empty string. */
BURROW_OWNS(ret) Str ast_comment_group_text(AstCommentGroup *g, Alloc *a);

/* ----------------------------------------------------------------- fields */

/* ast.Field: a struct field, a method in an interface, or a parameter or
 * result, with the names in names, which are AstIdent pointers and may be
 * none. */
typedef struct AstField {
    AstBase node;
    AstCommentGroup *doc;     /* or NULL */
    Slice names;              /* of AstIdent *, or empty */
    AstExpr type;             /* or NULL */
    struct AstBasicLit *tag;  /* or NULL */
    AstCommentGroup *comment; /* or NULL */
} AstField;

TokenPos ast_field_pos(AstField *f);
TokenPos ast_field_end(AstField *f);

/* ast.FieldList, the fields between a pair of brackets, braces or parens.
 * list holds AstField pointers. */
typedef struct AstFieldList {
    AstBase node;
    TokenPos opening; /* or 0 if there is no bracket */
    Slice list;
    TokenPos closing; /* or 0 if there is no bracket */
} AstFieldList;

TokenPos ast_field_list_pos(AstFieldList *f);
TokenPos ast_field_list_end(AstFieldList *f);

/* FieldList.NumFields: how many parameters or fields f has, counting each
 * name, and 0 for a NULL f. */
Int ast_field_list_num_fields(AstFieldList *f);

/* ------------------------------------------------------------ expressions */

/* ast.BadExpr, where the parser found an expression it could not read. */
typedef struct AstBadExpr {
    AstBase node;
    TokenPos from, to;
} AstBadExpr;

/* ast.Ident, an identifier. obj is the deprecated resolver's object, or
 * NULL. */
typedef struct AstIdent {
    AstBase node;
    TokenPos name_pos;
    Str name;
    struct AstObject *obj;
} AstIdent;

/* ast.Ellipsis, the ... in a parameter list or an array type. */
typedef struct AstEllipsis {
    AstBase node;
    TokenPos ellipsis;
    AstExpr elt; /* or NULL */
} AstEllipsis;

/* ast.BasicLit, a literal of kind TOKEN_INT, TOKEN_FLOAT, TOKEN_IMAG,
 * TOKEN_CHAR or TOKEN_STRING, with its source text in value, quotes and all. */
typedef struct AstBasicLit {
    AstBase node;
    TokenPos value_pos;
    TokenPos value_end; /* or 0, when the end is value_pos plus the length */
    Token kind;
    Str value;
} AstBasicLit;

/* ast.FuncLit, a function literal. */
typedef struct AstFuncLit {
    AstBase node;
    struct AstFuncType *type;
    struct AstBlockStmt *body;
} AstFuncLit;

/* ast.CompositeLit. elts holds AstExpr. */
typedef struct AstCompositeLit {
    AstBase node;
    AstExpr type; /* or NULL */
    TokenPos lbrace;
    Slice elts;
    TokenPos rbrace;
    bool incomplete; /* whether a filter took elements out of elts */
} AstCompositeLit;

/* ast.ParenExpr. */
typedef struct AstParenExpr {
    AstBase node;
    TokenPos lparen;
    AstExpr x;
    TokenPos rparen;
} AstParenExpr;

/* ast.SelectorExpr, x.sel. */
typedef struct AstSelectorExpr {
    AstBase node;
    AstExpr x;
    AstIdent *sel;
} AstSelectorExpr;

/* ast.IndexExpr, x[index]. */
typedef struct AstIndexExpr {
    AstBase node;
    AstExpr x;
    TokenPos lbrack;
    AstExpr index;
    TokenPos rbrack;
} AstIndexExpr;

/* ast.IndexListExpr, x[a, b], an instantiation with more than one type
 * argument. indices holds AstExpr. */
typedef struct AstIndexListExpr {
    AstBase node;
    AstExpr x;
    TokenPos lbrack;
    Slice indices;
    TokenPos rbrack;
} AstIndexListExpr;

/* ast.SliceExpr, x[low:high] or x[low:high:max]. */
typedef struct AstSliceExpr {
    AstBase node;
    AstExpr x;
    TokenPos lbrack;
    AstExpr low;  /* or NULL */
    AstExpr high; /* or NULL */
    AstExpr max;  /* or NULL */
    bool slice3;  /* whether it is the three index form */
    TokenPos rbrack;
} AstSliceExpr;

/* ast.TypeAssertExpr, x.(type). type is NULL in a type switch's x.(type). */
typedef struct AstTypeAssertExpr {
    AstBase node;
    AstExpr x;
    TokenPos lparen;
    AstExpr type;
    TokenPos rparen;
} AstTypeAssertExpr;

/* ast.CallExpr. args holds AstExpr, and ellipsis is where the ... is in f(x...),
 * or 0. */
typedef struct AstCallExpr {
    AstBase node;
    AstExpr fun;
    TokenPos lparen;
    Slice args;
    TokenPos ellipsis;
    TokenPos rparen;
} AstCallExpr;

/* ast.StarExpr, *x as a dereference or a pointer type. */
typedef struct AstStarExpr {
    AstBase node;
    TokenPos star;
    AstExpr x;
} AstStarExpr;

/* ast.UnaryExpr, op x. */
typedef struct AstUnaryExpr {
    AstBase node;
    TokenPos op_pos;
    Token op;
    AstExpr x;
} AstUnaryExpr;

/* ast.BinaryExpr, x op y. */
typedef struct AstBinaryExpr {
    AstBase node;
    AstExpr x;
    TokenPos op_pos;
    Token op;
    AstExpr y;
} AstBinaryExpr;

/* ast.KeyValueExpr, key: value in a composite literal. */
typedef struct AstKeyValueExpr {
    AstBase node;
    AstExpr key;
    TokenPos colon;
    AstExpr value;
} AstKeyValueExpr;

/* ast.ChanDir, the direction of a channel type, as a bit set. */
typedef Int AstChanDir;

#define AST_SEND ((AstChanDir)1)
#define AST_RECV ((AstChanDir)2)

/* ------------------------------------------------------------------ types */

/* ast.ArrayType, [len]elt, or []elt for a slice. */
typedef struct AstArrayType {
    AstBase node;
    TokenPos lbrack;
    AstExpr len; /* NULL for a slice, an AstEllipsis for [...]T */
    AstExpr elt;
} AstArrayType;

/* ast.StructType. */
typedef struct AstStructType {
    AstBase node;
    TokenPos struct_; /* Go's Struct, the struct keyword */
    AstFieldList *fields;
    bool incomplete; /* whether a filter took fields out */
} AstStructType;

/* ast.FuncType, a signature. */
typedef struct AstFuncType {
    AstBase node;
    TokenPos func;             /* or 0 in an interface method */
    AstFieldList *type_params; /* or NULL */
    AstFieldList *params;
    AstFieldList *results; /* or NULL */
} AstFuncType;

/* ast.InterfaceType. */
typedef struct AstInterfaceType {
    AstBase node;
    TokenPos interface_;   /* Go's Interface, the interface keyword */
    AstFieldList *methods; /* the methods and embedded elements */
    bool incomplete;       /* whether a filter took methods out */
} AstInterfaceType;

/* ast.MapType. */
typedef struct AstMapType {
    AstBase node;
    TokenPos map;
    AstExpr key;
    AstExpr value;
} AstMapType;

/* ast.ChanType, chan, chan<- or <-chan of value. */
typedef struct AstChanType {
    AstBase node;
    TokenPos begin; /* the chan keyword, or the <- of <-chan */
    TokenPos arrow; /* the <- of chan<-, or 0 */
    AstChanDir dir;
    AstExpr value;
} AstChanType;

/* Pos and End of each expression and type. */
TokenPos ast_bad_expr_pos(AstBadExpr *x);
TokenPos ast_bad_expr_end(AstBadExpr *x);
TokenPos ast_ident_pos(AstIdent *x);
TokenPos ast_ident_end(AstIdent *x);
TokenPos ast_ellipsis_pos(AstEllipsis *x);
TokenPos ast_ellipsis_end(AstEllipsis *x);
TokenPos ast_basic_lit_pos(AstBasicLit *x);
TokenPos ast_basic_lit_end(AstBasicLit *x);
TokenPos ast_func_lit_pos(AstFuncLit *x);
TokenPos ast_func_lit_end(AstFuncLit *x);
TokenPos ast_composite_lit_pos(AstCompositeLit *x);
TokenPos ast_composite_lit_end(AstCompositeLit *x);
TokenPos ast_paren_expr_pos(AstParenExpr *x);
TokenPos ast_paren_expr_end(AstParenExpr *x);
TokenPos ast_selector_expr_pos(AstSelectorExpr *x);
TokenPos ast_selector_expr_end(AstSelectorExpr *x);
TokenPos ast_index_expr_pos(AstIndexExpr *x);
TokenPos ast_index_expr_end(AstIndexExpr *x);
TokenPos ast_index_list_expr_pos(AstIndexListExpr *x);
TokenPos ast_index_list_expr_end(AstIndexListExpr *x);
TokenPos ast_slice_expr_pos(AstSliceExpr *x);
TokenPos ast_slice_expr_end(AstSliceExpr *x);
TokenPos ast_type_assert_expr_pos(AstTypeAssertExpr *x);
TokenPos ast_type_assert_expr_end(AstTypeAssertExpr *x);
TokenPos ast_call_expr_pos(AstCallExpr *x);
TokenPos ast_call_expr_end(AstCallExpr *x);
TokenPos ast_star_expr_pos(AstStarExpr *x);
TokenPos ast_star_expr_end(AstStarExpr *x);
TokenPos ast_unary_expr_pos(AstUnaryExpr *x);
TokenPos ast_unary_expr_end(AstUnaryExpr *x);
TokenPos ast_binary_expr_pos(AstBinaryExpr *x);
TokenPos ast_binary_expr_end(AstBinaryExpr *x);
TokenPos ast_key_value_expr_pos(AstKeyValueExpr *x);
TokenPos ast_key_value_expr_end(AstKeyValueExpr *x);
TokenPos ast_array_type_pos(AstArrayType *x);
TokenPos ast_array_type_end(AstArrayType *x);
TokenPos ast_struct_type_pos(AstStructType *x);
TokenPos ast_struct_type_end(AstStructType *x);
TokenPos ast_func_type_pos(AstFuncType *x);
TokenPos ast_func_type_end(AstFuncType *x);
TokenPos ast_interface_type_pos(AstInterfaceType *x);
TokenPos ast_interface_type_end(AstInterfaceType *x);
TokenPos ast_map_type_pos(AstMapType *x);
TokenPos ast_map_type_end(AstMapType *x);
TokenPos ast_chan_type_pos(AstChanType *x);
TokenPos ast_chan_type_end(AstChanType *x);

/* NewIdent: an AstIdent for name with no position, made in a, or NULL if a is
 * out of memory. name is not copied. */
BURROW_OWNS(ret) AstIdent *ast_new_ident(Alloc *a, Str name);

/* IsExported: whether name starts with an upper case letter. */
bool ast_is_exported(Str name);

/* Ident.IsExported and Ident.String. The string of a NULL id is "<nil>". */
bool ast_ident_is_exported(AstIdent *id);
BURROW_BORROWS(ret, id) Str ast_ident_string(AstIdent *id);

/* Unparen: e with any parentheses around it taken off. */
AstExpr ast_unparen(AstExpr e);

/* ------------------------------------------------------------- statements */

/* ast.BadStmt, where the parser found a statement it could not read. */
typedef struct AstBadStmt {
    AstBase node;
    TokenPos from, to;
} AstBadStmt;

/* ast.DeclStmt, a declaration inside a function. */
typedef struct AstDeclStmt {
    AstBase node;
    AstDecl decl; /* an AstGenDecl of a const, type or var */
} AstDeclStmt;

/* ast.EmptyStmt, an explicit ; or one the scanner put in. */
typedef struct AstEmptyStmt {
    AstBase node;
    TokenPos semicolon;
    bool implicit; /* whether the ; was left out of the source */
} AstEmptyStmt;

/* ast.LabeledStmt. */
typedef struct AstLabeledStmt {
    AstBase node;
    AstIdent *label;
    TokenPos colon;
    AstStmt stmt;
} AstLabeledStmt;

/* ast.ExprStmt, an expression on its own as a statement. */
typedef struct AstExprStmt {
    AstBase node;
    AstExpr x;
} AstExprStmt;

/* ast.SendStmt, ch <- value. */
typedef struct AstSendStmt {
    AstBase node;
    AstExpr chan;
    TokenPos arrow;
    AstExpr value;
} AstSendStmt;

/* ast.IncDecStmt, x++ or x--. */
typedef struct AstIncDecStmt {
    AstBase node;
    AstExpr x;
    TokenPos tok_pos;
    Token tok; /* TOKEN_INC or TOKEN_DEC */
} AstIncDecStmt;

/* ast.AssignStmt, an assignment or a short variable declaration. lhs and rhs
 * hold AstExpr. */
typedef struct AstAssignStmt {
    AstBase node;
    Slice lhs;
    TokenPos tok_pos;
    Token tok; /* TOKEN_ASSIGN, TOKEN_DEFINE or an op= token */
    Slice rhs;
} AstAssignStmt;

/* ast.GoStmt. */
typedef struct AstGoStmt {
    AstBase node;
    TokenPos go;
    AstCallExpr *call;
} AstGoStmt;

/* ast.DeferStmt. */
typedef struct AstDeferStmt {
    AstBase node;
    TokenPos defer;
    AstCallExpr *call;
} AstDeferStmt;

/* ast.ReturnStmt. results holds AstExpr. */
typedef struct AstReturnStmt {
    AstBase node;
    TokenPos return_; /* Go's Return, the return keyword */
    Slice results;
} AstReturnStmt;

/* ast.BranchStmt, a break, continue, goto or fallthrough. */
typedef struct AstBranchStmt {
    AstBase node;
    TokenPos tok_pos;
    Token tok;
    AstIdent *label; /* or NULL */
} AstBranchStmt;

/* ast.BlockStmt, a braced list of statements, which list holds as
 * AstStmt. */
typedef struct AstBlockStmt {
    AstBase node;
    TokenPos lbrace;
    Slice list;
    TokenPos rbrace; /* or 0 if the closing brace is missing */
} AstBlockStmt;

/* ast.IfStmt. */
typedef struct AstIfStmt {
    AstBase node;
    TokenPos if_; /* Go's If, the if keyword */
    AstStmt init; /* or NULL */
    AstExpr cond;
    AstBlockStmt *body;
    AstStmt else_; /* Go's Else, or NULL */
} AstIfStmt;

/* ast.CaseClause, a case or default of an expression or type switch. list
 * holds AstExpr and is empty for default, and body holds AstStmt. */
typedef struct AstCaseClause {
    AstBase node;
    TokenPos case_; /* Go's Case, the case or default keyword */
    Slice list;
    TokenPos colon;
    Slice body;
} AstCaseClause;

/* ast.SwitchStmt, an expression switch. */
typedef struct AstSwitchStmt {
    AstBase node;
    TokenPos switch_; /* Go's Switch, the switch keyword */
    AstStmt init;     /* or NULL */
    AstExpr tag;      /* or NULL */
    AstBlockStmt *body;
} AstSwitchStmt;

/* ast.TypeSwitchStmt. */
typedef struct AstTypeSwitchStmt {
    AstBase node;
    TokenPos switch_; /* Go's Switch, the switch keyword */
    AstStmt init;     /* or NULL */
    AstStmt assign;   /* x := y.(type) or y.(type) */
    AstBlockStmt *body;
} AstTypeSwitchStmt;

/* ast.CommClause, a case or default of a select. body holds AstStmt. */
typedef struct AstCommClause {
    AstBase node;
    TokenPos case_; /* Go's Case, the case or default keyword */
    AstStmt comm;   /* the send or receive, or NULL for default */
    TokenPos colon;
    Slice body;
} AstCommClause;

/* ast.SelectStmt. */
typedef struct AstSelectStmt {
    AstBase node;
    TokenPos select;
    AstBlockStmt *body;
} AstSelectStmt;

/* ast.ForStmt. */
typedef struct AstForStmt {
    AstBase node;
    TokenPos for_; /* Go's For, the for keyword */
    AstStmt init;  /* or NULL */
    AstExpr cond;  /* or NULL */
    AstStmt post;  /* or NULL */
    AstBlockStmt *body;
} AstForStmt;

/* ast.RangeStmt, a for with a range clause. */
typedef struct AstRangeStmt {
    AstBase node;
    TokenPos for_;    /* Go's For, the for keyword */
    AstExpr key;      /* or NULL */
    AstExpr value;    /* or NULL */
    TokenPos tok_pos; /* or 0 if there is no key */
    Token tok;        /* TOKEN_ILLEGAL if there is no key */
    TokenPos range;
    AstExpr x;
    AstBlockStmt *body;
} AstRangeStmt;

/* Pos and End of each statement. */
TokenPos ast_bad_stmt_pos(AstBadStmt *s);
TokenPos ast_bad_stmt_end(AstBadStmt *s);
TokenPos ast_decl_stmt_pos(AstDeclStmt *s);
TokenPos ast_decl_stmt_end(AstDeclStmt *s);
TokenPos ast_empty_stmt_pos(AstEmptyStmt *s);
TokenPos ast_empty_stmt_end(AstEmptyStmt *s);
TokenPos ast_labeled_stmt_pos(AstLabeledStmt *s);
TokenPos ast_labeled_stmt_end(AstLabeledStmt *s);
TokenPos ast_expr_stmt_pos(AstExprStmt *s);
TokenPos ast_expr_stmt_end(AstExprStmt *s);
TokenPos ast_send_stmt_pos(AstSendStmt *s);
TokenPos ast_send_stmt_end(AstSendStmt *s);
TokenPos ast_inc_dec_stmt_pos(AstIncDecStmt *s);
TokenPos ast_inc_dec_stmt_end(AstIncDecStmt *s);
TokenPos ast_assign_stmt_pos(AstAssignStmt *s);
TokenPos ast_assign_stmt_end(AstAssignStmt *s);
TokenPos ast_go_stmt_pos(AstGoStmt *s);
TokenPos ast_go_stmt_end(AstGoStmt *s);
TokenPos ast_defer_stmt_pos(AstDeferStmt *s);
TokenPos ast_defer_stmt_end(AstDeferStmt *s);
TokenPos ast_return_stmt_pos(AstReturnStmt *s);
TokenPos ast_return_stmt_end(AstReturnStmt *s);
TokenPos ast_branch_stmt_pos(AstBranchStmt *s);
TokenPos ast_branch_stmt_end(AstBranchStmt *s);
TokenPos ast_block_stmt_pos(AstBlockStmt *s);
TokenPos ast_block_stmt_end(AstBlockStmt *s);
TokenPos ast_if_stmt_pos(AstIfStmt *s);
TokenPos ast_if_stmt_end(AstIfStmt *s);
TokenPos ast_case_clause_pos(AstCaseClause *s);
TokenPos ast_case_clause_end(AstCaseClause *s);
TokenPos ast_switch_stmt_pos(AstSwitchStmt *s);
TokenPos ast_switch_stmt_end(AstSwitchStmt *s);
TokenPos ast_type_switch_stmt_pos(AstTypeSwitchStmt *s);
TokenPos ast_type_switch_stmt_end(AstTypeSwitchStmt *s);
TokenPos ast_comm_clause_pos(AstCommClause *s);
TokenPos ast_comm_clause_end(AstCommClause *s);
TokenPos ast_select_stmt_pos(AstSelectStmt *s);
TokenPos ast_select_stmt_end(AstSelectStmt *s);
TokenPos ast_for_stmt_pos(AstForStmt *s);
TokenPos ast_for_stmt_end(AstForStmt *s);
TokenPos ast_range_stmt_pos(AstRangeStmt *s);
TokenPos ast_range_stmt_end(AstRangeStmt *s);

/* ---------------------------------------------------------- declarations */

/* ast.ImportSpec, one import. */
typedef struct AstImportSpec {
    AstBase node;
    AstCommentGroup *doc;     /* or NULL */
    AstIdent *name;           /* the local name, . included, or NULL */
    AstBasicLit *path;        /* the quoted import path */
    AstCommentGroup *comment; /* or NULL */
    TokenPos end_pos;         /* where it ends, if not where path does, or 0 */
} AstImportSpec;

/* ast.ValueSpec, one line of a const or var declaration. names holds
 * AstIdent pointers and values AstExpr. */
typedef struct AstValueSpec {
    AstBase node;
    AstCommentGroup *doc; /* or NULL */
    Slice names;
    AstExpr type; /* or NULL */
    Slice values;
    AstCommentGroup *comment; /* or NULL */
} AstValueSpec;

/* ast.TypeSpec, one type declaration. */
typedef struct AstTypeSpec {
    AstBase node;
    AstCommentGroup *doc; /* or NULL */
    AstIdent *name;
    AstFieldList *type_params; /* or NULL */
    TokenPos assign;           /* the = of an alias, or 0 */
    AstExpr type;
    AstCommentGroup *comment; /* or NULL */
} AstTypeSpec;

TokenPos ast_import_spec_pos(AstImportSpec *s);
TokenPos ast_import_spec_end(AstImportSpec *s);
TokenPos ast_value_spec_pos(AstValueSpec *s);
TokenPos ast_value_spec_end(AstValueSpec *s);
TokenPos ast_type_spec_pos(AstTypeSpec *s);
TokenPos ast_type_spec_end(AstTypeSpec *s);

/* ast.BadDecl, where the parser found a declaration it could not read. */
typedef struct AstBadDecl {
    AstBase node;
    TokenPos from, to;
} AstBadDecl;

/* ast.GenDecl, an import, const, type or var declaration, with its specs in
 * specs as AstSpec. lparen and rparen are 0 when there are no parens. */
typedef struct AstGenDecl {
    AstBase node;
    AstCommentGroup *doc; /* or NULL */
    TokenPos tok_pos;
    Token tok;
    TokenPos lparen;
    Slice specs;
    TokenPos rparen;
} AstGenDecl;

/* ast.FuncDecl, a function or method. */
typedef struct AstFuncDecl {
    AstBase node;
    AstCommentGroup *doc; /* or NULL */
    AstFieldList *recv;   /* or NULL for a function */
    AstIdent *name;
    AstFuncType *type;
    AstBlockStmt *body; /* or NULL for one written in assembly */
} AstFuncDecl;

TokenPos ast_bad_decl_pos(AstBadDecl *d);
TokenPos ast_bad_decl_end(AstBadDecl *d);
TokenPos ast_gen_decl_pos(AstGenDecl *d);
TokenPos ast_gen_decl_end(AstGenDecl *d);
TokenPos ast_func_decl_pos(AstFuncDecl *d);
TokenPos ast_func_decl_end(AstFuncDecl *d);

/* ---------------------------------------------------- files and packages */

/* ast.File, one source file. decls holds AstDecl, imports AstImportSpec
 * pointers, unresolved AstIdent pointers and comments AstCommentGroup
 * pointers, every comment in the file in order. */
typedef struct AstFile {
    AstBase node;
    AstCommentGroup *doc; /* or NULL */
    TokenPos package;     /* the package keyword */
    AstIdent *name;
    Slice decls;
    TokenPos file_start, file_end;
    struct AstScope *scope; /* deprecated, the resolver's file scope */
    Slice imports;
    Slice unresolved; /* deprecated, what the resolver could not resolve */
    Slice comments;
    Str go_version; /* the minimum version from a //go:build line, or "" */
} AstFile;

TokenPos ast_file_pos(AstFile *f);
TokenPos ast_file_end(AstFile *f);

/* IsGenerated: whether f has a "// Code generated ... DO NOT EDIT." line
 * before its package clause. */
bool ast_is_generated(AstFile *f);

/* ast.Package, deprecated in Go along with the resolver, with the files of
 * one package by filename. imports maps a Str to an AstObject pointer and
 * files a Str to an AstFile pointer. */
typedef struct AstPackage {
    AstBase node;
    Str name;
    struct AstScope *scope;
    Map *imports;
    Map *files;
} AstPackage;

TokenPos ast_package_pos(AstPackage *p);
TokenPos ast_package_end(AstPackage *p);

/* ------------------------------------------------------------ descriptors */

/* The type descriptors, so that a node can go to fmt or ast_fprint and a list
 * of them can be made. TYPE_OF(AstIdent) is ast.Ident, and so on for every
 * node, and TYPE_OF(AstIdentPtr) is *ast.Ident. TYPE_AST_EXPR and the other
 * interface ones are what a list of AstExpr holds:
 *
 *     Slice args = slice_make(a, TYPE_AST_EXPR, 0, 2);
 *     args = BURROW_APPEND(AstExpr, a, args, &x->node); */
extern const Type burrow_type_AstComment;
extern const Type burrow_type_AstCommentPtr;
extern const Type burrow_type_AstCommentGroup;
extern const Type burrow_type_AstCommentGroupPtr;
extern const Type burrow_type_AstField;
extern const Type burrow_type_AstFieldPtr;
extern const Type burrow_type_AstFieldList;
extern const Type burrow_type_AstFieldListPtr;
extern const Type burrow_type_AstBadExpr;
extern const Type burrow_type_AstBadExprPtr;
extern const Type burrow_type_AstIdent;
extern const Type burrow_type_AstIdentPtr;
extern const Type burrow_type_AstEllipsis;
extern const Type burrow_type_AstEllipsisPtr;
extern const Type burrow_type_AstBasicLit;
extern const Type burrow_type_AstBasicLitPtr;
extern const Type burrow_type_AstFuncLit;
extern const Type burrow_type_AstFuncLitPtr;
extern const Type burrow_type_AstCompositeLit;
extern const Type burrow_type_AstCompositeLitPtr;
extern const Type burrow_type_AstParenExpr;
extern const Type burrow_type_AstParenExprPtr;
extern const Type burrow_type_AstSelectorExpr;
extern const Type burrow_type_AstSelectorExprPtr;
extern const Type burrow_type_AstIndexExpr;
extern const Type burrow_type_AstIndexExprPtr;
extern const Type burrow_type_AstIndexListExpr;
extern const Type burrow_type_AstIndexListExprPtr;
extern const Type burrow_type_AstSliceExpr;
extern const Type burrow_type_AstSliceExprPtr;
extern const Type burrow_type_AstTypeAssertExpr;
extern const Type burrow_type_AstTypeAssertExprPtr;
extern const Type burrow_type_AstCallExpr;
extern const Type burrow_type_AstCallExprPtr;
extern const Type burrow_type_AstStarExpr;
extern const Type burrow_type_AstStarExprPtr;
extern const Type burrow_type_AstUnaryExpr;
extern const Type burrow_type_AstUnaryExprPtr;
extern const Type burrow_type_AstBinaryExpr;
extern const Type burrow_type_AstBinaryExprPtr;
extern const Type burrow_type_AstKeyValueExpr;
extern const Type burrow_type_AstKeyValueExprPtr;
extern const Type burrow_type_AstArrayType;
extern const Type burrow_type_AstArrayTypePtr;
extern const Type burrow_type_AstStructType;
extern const Type burrow_type_AstStructTypePtr;
extern const Type burrow_type_AstFuncType;
extern const Type burrow_type_AstFuncTypePtr;
extern const Type burrow_type_AstInterfaceType;
extern const Type burrow_type_AstInterfaceTypePtr;
extern const Type burrow_type_AstMapType;
extern const Type burrow_type_AstMapTypePtr;
extern const Type burrow_type_AstChanType;
extern const Type burrow_type_AstChanTypePtr;
extern const Type burrow_type_AstBadStmt;
extern const Type burrow_type_AstBadStmtPtr;
extern const Type burrow_type_AstDeclStmt;
extern const Type burrow_type_AstDeclStmtPtr;
extern const Type burrow_type_AstEmptyStmt;
extern const Type burrow_type_AstEmptyStmtPtr;
extern const Type burrow_type_AstLabeledStmt;
extern const Type burrow_type_AstLabeledStmtPtr;
extern const Type burrow_type_AstExprStmt;
extern const Type burrow_type_AstExprStmtPtr;
extern const Type burrow_type_AstSendStmt;
extern const Type burrow_type_AstSendStmtPtr;
extern const Type burrow_type_AstIncDecStmt;
extern const Type burrow_type_AstIncDecStmtPtr;
extern const Type burrow_type_AstAssignStmt;
extern const Type burrow_type_AstAssignStmtPtr;
extern const Type burrow_type_AstGoStmt;
extern const Type burrow_type_AstGoStmtPtr;
extern const Type burrow_type_AstDeferStmt;
extern const Type burrow_type_AstDeferStmtPtr;
extern const Type burrow_type_AstReturnStmt;
extern const Type burrow_type_AstReturnStmtPtr;
extern const Type burrow_type_AstBranchStmt;
extern const Type burrow_type_AstBranchStmtPtr;
extern const Type burrow_type_AstBlockStmt;
extern const Type burrow_type_AstBlockStmtPtr;
extern const Type burrow_type_AstIfStmt;
extern const Type burrow_type_AstIfStmtPtr;
extern const Type burrow_type_AstCaseClause;
extern const Type burrow_type_AstCaseClausePtr;
extern const Type burrow_type_AstSwitchStmt;
extern const Type burrow_type_AstSwitchStmtPtr;
extern const Type burrow_type_AstTypeSwitchStmt;
extern const Type burrow_type_AstTypeSwitchStmtPtr;
extern const Type burrow_type_AstCommClause;
extern const Type burrow_type_AstCommClausePtr;
extern const Type burrow_type_AstSelectStmt;
extern const Type burrow_type_AstSelectStmtPtr;
extern const Type burrow_type_AstForStmt;
extern const Type burrow_type_AstForStmtPtr;
extern const Type burrow_type_AstRangeStmt;
extern const Type burrow_type_AstRangeStmtPtr;
extern const Type burrow_type_AstImportSpec;
extern const Type burrow_type_AstImportSpecPtr;
extern const Type burrow_type_AstValueSpec;
extern const Type burrow_type_AstValueSpecPtr;
extern const Type burrow_type_AstTypeSpec;
extern const Type burrow_type_AstTypeSpecPtr;
extern const Type burrow_type_AstBadDecl;
extern const Type burrow_type_AstBadDeclPtr;
extern const Type burrow_type_AstGenDecl;
extern const Type burrow_type_AstGenDeclPtr;
extern const Type burrow_type_AstFuncDecl;
extern const Type burrow_type_AstFuncDeclPtr;
extern const Type burrow_type_AstFile;
extern const Type burrow_type_AstFilePtr;
extern const Type burrow_type_AstPackage;
extern const Type burrow_type_AstPackagePtr;
extern const Type burrow_type_AstObject;
extern const Type burrow_type_AstObjectPtr;
extern const Type burrow_type_AstScope;
extern const Type burrow_type_AstScopePtr;
extern const Type burrow_type_AstNode;
extern const Type burrow_type_AstExpr;
extern const Type burrow_type_AstStmt;
extern const Type burrow_type_AstDecl;
extern const Type burrow_type_AstSpec;
extern const Type burrow_type_AstChanDir;
extern const Type burrow_type_AstObjKind;
extern const Type burrow_type_AstDirective;
extern const Type burrow_type_AstDirectiveArg;

#define TYPE_AST_NODE TYPE_OF(AstNode)
#define TYPE_AST_EXPR TYPE_OF(AstExpr)
#define TYPE_AST_STMT TYPE_OF(AstStmt)
#define TYPE_AST_DECL TYPE_OF(AstDecl)
#define TYPE_AST_SPEC TYPE_OF(AstSpec)
#define TYPE_AST_COMMENT_PTR TYPE_OF(AstCommentPtr)
#define TYPE_AST_COMMENT_GROUP_PTR TYPE_OF(AstCommentGroupPtr)
#define TYPE_AST_FIELD_PTR TYPE_OF(AstFieldPtr)
#define TYPE_AST_IDENT_PTR TYPE_OF(AstIdentPtr)
#define TYPE_AST_IMPORT_SPEC_PTR TYPE_OF(AstImportSpecPtr)
#define TYPE_AST_FILE_PTR TYPE_OF(AstFilePtr)
#define TYPE_AST_OBJECT_PTR TYPE_OF(AstObjectPtr)

/* The descriptor of the struct n is, such as ast.Ident for an AstIdent, or
 * NULL for a NULL n or one whose kind is out of range. */
BURROW_STATIC(ret) const Type *ast_node_type(AstNode n);

/* --------------------------------------------------------------- walking */

typedef struct AstVisitor AstVisitor;

/* ast.Visitor. visit is called with each node on the way down, and with NULL
 * once a node's children are done. It returns the visitor for the children,
 * which may be itself, or a nil visitor to skip them. */
typedef struct AstVisitorVT {
    const Type *self_type;
    AstVisitor (*visit)(void *self, AstNode node);
} AstVisitorVT;

/* A visitor whose vt is NULL is nil. */
struct AstVisitor {
    const AstVisitorVT *vt;
    void *data;
};

/* Visitor.Visit. */
AstVisitor ast_visitor_visit(AstVisitor v, AstNode node);

/* Walk: the tree under node in depth first order, starting with
 * ast_visitor_visit(v, node). If that gives a visitor w that is not nil, Walk
 * goes on with w into each child that is not NULL, and ends with
 * ast_visitor_visit(w, NULL). A node of a kind it does not know panics. */
void ast_walk(AstVisitor v, AstNode node);

BURROW_FUNC(AstInspectFunc, bool, AstNode node);

/* Inspect: Walk with f as the visitor, going into a node's children as long
 * as f says true, and calling f with NULL after the children. */
void ast_inspect(AstNode node, AstInspectFunc f);

/* Preorder: the nodes under root, root first, in depth first order, as an
 * iterator. The yield gets a pointer to an AstNode. Nothing is allocated, and
 * root has to outlive the iterator. */
IterSeq ast_preorder(AstNode root);

BURROW_FUNC(AstPreorderStackFunc, bool, AstNode node, Slice stack);

/* PreorderStack: Inspect without the NULL calls, with stack holding the
 * nodes that enclose the one f is called with, outermost first, after
 * whatever stack held to begin with. f saying false skips that node's
 * children. Elements are appended in a, and stack must hold AstNode. */
void ast_preorder_stack(Alloc *a, AstNode root, Slice stack, AstPreorderStackFunc f);

/* ------------------------------------------------------------- directives */

/* ast.Directive, a //tool:name args comment. args has no space at either
 * end. */
typedef struct AstDirective {
    Str tool;
    Str name;
    Str args;
    TokenPos slash;    /* where the comment starts */
    TokenPos args_pos; /* where args starts */
} AstDirective;

/* ParseDirective: the directive in comment c, which starts at pos, with *ok
 * set to whether it is one. A directive is a line comment with no space after
 * the //, a tool name, a colon and a name, the first two in lower case
 * letters and digits. */
AstDirective ast_parse_directive(TokenPos pos, Str c, bool *ok);

TokenPos ast_directive_pos(AstDirective *d);
TokenPos ast_directive_end(AstDirective *d);

/* ast.DirectiveArg, one argument and where it starts. */
typedef struct AstDirectiveArg {
    Str arg;
    TokenPos pos;
} AstDirectiveArg;

/* Directive.ParseArgs: d's arguments as a Slice of AstDirectiveArg, split at
 * spaces, where an argument in double quotes or backquotes is unquoted. A bad
 * quoted argument gives an error and an empty slice. Arguments that need no
 * unquoting are views of d->args. */
BURROW_OWNS(ret) Slice ast_directive_parse_args(AstDirective *d, Alloc *a, Error *err);

/* -------------------------------------------------- scopes and objects
 *
 * The parser's old resolver, which Go deprecates in favour of go/types and
 * still fills in. */

/* ast.ObjKind. */
typedef Int AstObjKind;

#define AST_BAD ((AstObjKind)0)
#define AST_PKG ((AstObjKind)1)
#define AST_CON ((AstObjKind)2)
#define AST_TYP ((AstObjKind)3)
#define AST_VAR ((AstObjKind)4)
#define AST_FUN ((AstObjKind)5)
#define AST_LBL ((AstObjKind)6)

/* ObjKind.String: "bad", "package", "const" and so on. A kind out of range
 * panics, as it does in Go. */
BURROW_STATIC(ret) Str ast_obj_kind_string(AstObjKind kind);

/* ast.Object, a named language entity. decl is the AstField, AstImportSpec,
 * AstValueSpec, AstTypeSpec, AstFuncDecl, AstLabeledStmt, AstAssignStmt or
 * AstScope pointer that declares it, boxed in an Any by its pointer type. */
typedef struct AstObject {
    AstObjKind kind;
    Str name;
    Any decl; /* or nil */
    Any data; /* or nil */
    Any type; /* or nil */
} AstObject;

/* NewObj: an object made in a, or NULL if a is out of memory. */
BURROW_OWNS(ret) AstObject *ast_new_obj(Alloc *a, AstObjKind kind, Str name);

/* Object.Pos: where obj's name is declared, or 0 if that is not known. */
TokenPos ast_object_pos(AstObject *obj);

/* ast.Scope. objects maps a Str to an AstObject pointer. */
typedef struct AstScope {
    struct AstScope *outer;
    Map *objects;
} AstScope;

/* NewScope: an empty scope inside outer, made in a, or NULL if a is out of
 * memory. */
BURROW_OWNS(ret) AstScope *ast_new_scope(Alloc *a, AstScope *outer);

/* Scope.Lookup: the object called name in s itself, or NULL. */
BURROW_BORROWS(ret, s) AstObject *ast_scope_lookup(AstScope *s, Str name);

/* Scope.Insert: obj added to s and NULL, unless s already has an object of
 * that name, which comes back instead, with s left alone. */
BURROW_BORROWS(ret, s) AstObject *ast_scope_insert(AstScope *s, AstObject *obj);

/* Scope.String: the scope and its objects, for debugging. */
BURROW_OWNS(ret) Str ast_scope_string(AstScope *s, Alloc *a);

/* ast.Importer: the package object for path, made and added to imports if it
 * is not there already. */
BURROW_FUNC(AstImporter, AstObject *, Map *imports, Str path, Error *err);

/* NewPackage: the package of files, which maps a filename Str to an AstFile
 * pointer, with the files' identifiers resolved against each other, the
 * imports importer gives and universe. An importer with a NULL function makes
 * every import fail. Errors are reported together, as a go/scanner error
 * list, and the package is made even when there are some. */
BURROW_OWNS(ret) AstPackage *ast_new_package(Alloc *a, TokenFileSet *fset, Map *files,
                                             AstImporter importer, AstScope *universe,
                                             Error *err);

/* ---------------------------------------------------------------- filters */

/* ast.Filter, which says whether to keep a name. */
BURROW_FUNC(AstFilter, bool, Str name);

/* FileExports: src cut down to its exported declarations, with unexported
 * fields and methods taken out of the types. Whether anything is left. */
bool ast_file_exports(AstFile *src);

/* PackageExports: ast_file_exports on each of pkg's files. Whether anything
 * is left. */
bool ast_package_exports(AstPackage *pkg);

/* FilterDecl: decl cut down to the names f keeps. Whether anything is
 * left. */
bool ast_filter_decl(AstDecl decl, AstFilter f);

/* FilterFile: ast_filter_decl on each of src's declarations, dropping those
 * with nothing left. Whether anything is left. */
bool ast_filter_file(AstFile *src, AstFilter f);

/* FilterPackage: ast_filter_file on each of pkg's files. Whether anything is
 * left. */
bool ast_filter_package(AstPackage *pkg, AstFilter f);

/* ast.MergeMode, for ast_merge_package_files. */
typedef Uint AstMergeMode;

/* Drop a function or method declared in more than one file, keeping one that
 * has a doc comment. */
#define AST_FILTER_FUNC_DUPLICATES ((AstMergeMode)1)
/* Leave out comments that are not doc comments. */
#define AST_FILTER_UNASSOCIATED_COMMENTS ((AstMergeMode)2)
/* Drop an import path seen in an earlier file. */
#define AST_FILTER_IMPORT_DUPLICATES ((AstMergeMode)4)

/* MergePackageFiles: one file made in a from all of pkg's, in filename
 * order, or NULL if a is out of memory. */
BURROW_OWNS(ret) AstFile *ast_merge_package_files(Alloc *a, AstPackage *pkg,
                                                  AstMergeMode mode);

/* SortImports: the runs of imports in f without a blank line between them
 * sorted, with duplicates taken out, the way gofmt does it. Positions move
 * with the specs, and fset loses the lines a removed import was on. */
void ast_sort_imports(Alloc *a, TokenFileSet *fset, AstFile *f);

/* --------------------------------------------------------------- printing */

/* ast.FieldFilter: whether ast_fprint prints a struct field, given its Go
 * name and its value, which points at the field. */
BURROW_FUNC(AstFieldFilter, bool, Str name, Any value);

/* NotNilFilter: false for a field that is a nil pointer, slice, map, channel,
 * function or interface, and true for anything else. Go hands it a
 * reflect.Value, and here it is an Any pointing at the field. */
bool ast_not_nil_filter(Str name, Any v);

/* Fprint: x written to w as a tree, one value per line with line numbers,
 * the way Go prints it, with positions as file:line:column when fset is not
 * NULL. A pointer seen before is printed as the line it was first printed on.
 * Struct fields are left out when f says to, and every field is printed when
 * f has no function. a holds the bookkeeping until it returns. The error is the
 * first one w gave back, or burrow_err_out_of_memory. */
BURROW_BORROWS(ret, w) Error ast_fprint(Alloc *a, IoWriter w, TokenFileSet *fset, Any x,
                                        AstFieldFilter f);

/* Print: ast_fprint to standard output, leaving out nil fields. */
BURROW_BORROWS(ret) Error ast_print(Alloc *a, TokenFileSet *fset, Any x);

/* ------------------------------------------------------------ comment maps */

/* ast.CommentMap, which maps an AstNode to a Slice of the AstCommentGroup
 * pointers that belong to it. A NULL map is an empty one. */
typedef Map *AstCommentMap;

/* NewCommentMap: comments, a Slice of AstCommentGroup pointers, each given to
 * the node in the tree under node it belongs to, as go/printer would place
 * them. The map is made in a, and is NULL if comments is empty. */
BURROW_OWNS(ret) AstCommentMap ast_new_comment_map(Alloc *a, TokenFileSet *fset,
                                                   AstNode node, Slice comments);

/* CommentMap.Update: the comments of old moved to new_node, which comes
 * back. */
AstNode ast_comment_map_update(AstCommentMap cmap, Alloc *a, AstNode old,
                               AstNode new_node);

/* CommentMap.Filter: a map made in a with only the entries of cmap for nodes
 * in the tree under node. */
BURROW_OWNS(ret) AstCommentMap ast_comment_map_filter(AstCommentMap cmap, Alloc *a,
                                                      AstNode node);

/* CommentMap.Comments: every comment group in cmap, in source order. */
BURROW_OWNS(ret) Slice ast_comment_map_comments(AstCommentMap cmap, Alloc *a);

/* CommentMap.String: the map, one node per line, for debugging. */
BURROW_OWNS(ret) Str ast_comment_map_string(AstCommentMap cmap, Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_AST_H */
