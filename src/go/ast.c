/* go/ast: positions, comment text, walking and directives.
 *
 * Go gives each node type its own Pos and End and walks a tree with a type
 * switch. Here both are a switch on the kind in the header, with one case per
 * node in the order ast.go declares them, so the two read side by side.
 *
 * Derived from Go's src/go/ast/ast.go, walk.go and directive.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>

/* Every list in a tree is a list of pointers, so one accessor does for all. */
static AstNode an_at(Slice s, Int i) {
    return ((AstNode *)s.p)[i];
}

static AstNode an_last(Slice s) {
    return ((AstNode *)s.p)[s.len - 1];
}

/* s[i:j], which Go has as syntax. */
static Str an_sub(Str s, Int i, Int j) {
    return str_from_bytes(s.p + i, j - i);
}

/* ---------------------------------------------------------------- comments */

TokenPos ast_comment_pos(AstComment *c) {
    return c->slash;
}

TokenPos ast_comment_end(AstComment *c) {
    return c->slash + c->text.len;
}

TokenPos ast_comment_group_pos(AstCommentGroup *g) {
    if (g->list.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_comment_pos((AstComment *)an_at(g->list, 0));
}

TokenPos ast_comment_group_end(AstCommentGroup *g) {
    if (g->list.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_comment_end((AstComment *)an_last(g->list));
}

static bool an_is_whitespace(Byte ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static Str an_strip_trailing_whitespace(Str s) {
    Int i = s.len;
    while (i > 0 && an_is_whitespace(s.p[i - 1])) {
        i--;
    }
    return an_sub(s, 0, i);
}

static bool an_is_lower_alnum(Byte b) {
    return ('a' <= b && b <= 'z') || ('0' <= b && b <= '9');
}

/* isDirective, with the // already taken off c. go/printer has its own copy,
 * as Go's does. */
static bool an_is_directive(Str c) {
    /* "//line " is a line directive, "//extern " is for gccgo and "//export "
     * is for cgo. */
    if (strings_has_prefix(c, BURROW_S("line ")) || strings_has_prefix(c, BURROW_S("extern ")) ||
        strings_has_prefix(c, BURROW_S("export "))) {
        return true;
    }
    /* "//[a-z0-9]+:[a-z0-9]" */
    Int colon = strings_index_byte(c, ':');
    if (colon <= 0 || colon + 1 >= c.len) {
        return false;
    }
    for (Int i = 0; i <= colon + 1; i++) {
        if (i == colon) {
            continue;
        }
        if (!an_is_lower_alnum(c.p[i])) {
            return false;
        }
    }
    return true;
}

/* For the tests, which check the table Go's do. */
bool burrow__ast_is_directive(Str c);
bool burrow__ast_is_directive(Str c) {
    return an_is_directive(c);
}

static void an_must(Error err) {
    if (!BURROW_OK(err)) {
        panic_str(BURROW_S("go/ast: out of memory"));
    }
}

/* Go collects the lines and then drops blank ones from the front and runs of
 * them from the middle. Writing each line as it comes, with a blank line held
 * back until something follows it, gives the same text without the list. */
typedef struct AnText {
    StringsBuilder b;
    bool started;
    bool blank;
} AnText;

static void an_text_line(AnText *t, Str line) {
    if (line.len == 0) {
        t->blank = t->started;
        return;
    }
    Error err = BURROW_NO_ERROR;
    if (t->blank) {
        strings_builder_write_string(&t->b, BURROW_S("\n"), &err);
        an_must(err);
        t->blank = false;
    }
    strings_builder_write_string(&t->b, line, &err);
    an_must(err);
    strings_builder_write_string(&t->b, BURROW_S("\n"), &err);
    an_must(err);
    t->started = true;
}

Str ast_comment_group_text(AstCommentGroup *g, Alloc *a) {
    if (g == NULL) {
        return BURROW_STR_EMPTY;
    }
    AnText t = {STRINGS_BUILDER(a), false, false};
    for (Int i = 0; i < g->list.len; i++) {
        Str c = ((AstComment *)an_at(g->list, i))->text;
        /* Remove the comment markers. The parser has given us exactly the
         * comment text. */
        if (c.len >= 2 && c.p[1] == '/') {
            c = an_sub(c, 2, c.len);
            if (c.len > 0) {
                if (c.p[0] == ' ') {
                    /* strip the first space, which Example tests need */
                    c = an_sub(c, 1, c.len);
                } else if (an_is_directive(c)) {
                    /* leave out //go:noinline, //line and so on */
                    continue;
                }
            }
        } else if (c.len >= 2 && c.p[1] == '*') {
            c = an_sub(c, 2, c.len - 2);
        }
        /* split on newlines, with the trailing white space of each taken off */
        for (;;) {
            Int nl = strings_index_byte(c, '\n');
            if (nl < 0) {
                an_text_line(&t, an_strip_trailing_whitespace(c));
                break;
            }
            an_text_line(&t, an_strip_trailing_whitespace(an_sub(c, 0, nl)));
            c = an_sub(c, nl + 1, c.len);
        }
    }
    return strings_builder_string(&t.b);
}

/* ------------------------------------------------------------------ fields */

TokenPos ast_field_pos(AstField *f) {
    if (f->names.len > 0) {
        return ast_ident_pos((AstIdent *)an_at(f->names, 0));
    }
    if (f->type != NULL) {
        return ast_node_pos(f->type);
    }
    return TOKEN_NO_POS;
}

TokenPos ast_field_end(AstField *f) {
    if (f->tag != NULL) {
        return ast_basic_lit_end(f->tag);
    }
    if (f->type != NULL) {
        return ast_node_end(f->type);
    }
    if (f->names.len > 0) {
        return ast_ident_end((AstIdent *)an_last(f->names));
    }
    return TOKEN_NO_POS;
}

TokenPos ast_field_list_pos(AstFieldList *f) {
    if (token_pos_is_valid(f->opening)) {
        return f->opening;
    }
    /* The list should not be empty in this case, but guard against bad
     * trees. */
    if (f->list.len > 0) {
        return ast_field_pos((AstField *)an_at(f->list, 0));
    }
    return TOKEN_NO_POS;
}

TokenPos ast_field_list_end(AstFieldList *f) {
    if (token_pos_is_valid(f->closing)) {
        return f->closing + 1;
    }
    if (f->list.len > 0) {
        return ast_field_end((AstField *)an_last(f->list));
    }
    return TOKEN_NO_POS;
}

Int ast_field_list_num_fields(AstFieldList *f) {
    Int n = 0;
    if (f != NULL) {
        for (Int i = 0; i < f->list.len; i++) {
            Int m = ((AstField *)an_at(f->list, i))->names.len;
            if (m == 0) {
                m = 1;
            }
            n += m;
        }
    }
    return n;
}

/* ------------------------------------------------------------- expressions */

TokenPos ast_bad_expr_pos(AstBadExpr *x) {
    return x->from;
}

TokenPos ast_ident_pos(AstIdent *x) {
    return x->name_pos;
}

TokenPos ast_ellipsis_pos(AstEllipsis *x) {
    return x->ellipsis;
}

TokenPos ast_basic_lit_pos(AstBasicLit *x) {
    return x->value_pos;
}

TokenPos ast_func_lit_pos(AstFuncLit *x) {
    return ast_func_type_pos(x->type);
}

TokenPos ast_composite_lit_pos(AstCompositeLit *x) {
    if (x->type != NULL) {
        return ast_node_pos(x->type);
    }
    return x->lbrace;
}

TokenPos ast_paren_expr_pos(AstParenExpr *x) {
    return x->lparen;
}

TokenPos ast_selector_expr_pos(AstSelectorExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_index_expr_pos(AstIndexExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_index_list_expr_pos(AstIndexListExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_slice_expr_pos(AstSliceExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_type_assert_expr_pos(AstTypeAssertExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_call_expr_pos(AstCallExpr *x) {
    return ast_node_pos(x->fun);
}

TokenPos ast_star_expr_pos(AstStarExpr *x) {
    return x->star;
}

TokenPos ast_unary_expr_pos(AstUnaryExpr *x) {
    return x->op_pos;
}

TokenPos ast_binary_expr_pos(AstBinaryExpr *x) {
    return ast_node_pos(x->x);
}

TokenPos ast_key_value_expr_pos(AstKeyValueExpr *x) {
    return ast_node_pos(x->key);
}

TokenPos ast_array_type_pos(AstArrayType *x) {
    return x->lbrack;
}

TokenPos ast_struct_type_pos(AstStructType *x) {
    return x->struct_;
}

TokenPos ast_func_type_pos(AstFuncType *x) {
    if (token_pos_is_valid(x->func) || x->params == NULL) { /* see Go issue 3870 */
        return x->func;
    }
    return ast_field_list_pos(x->params); /* an interface method has no func */
}

TokenPos ast_interface_type_pos(AstInterfaceType *x) {
    return x->interface_;
}

TokenPos ast_map_type_pos(AstMapType *x) {
    return x->map;
}

TokenPos ast_chan_type_pos(AstChanType *x) {
    return x->begin;
}

TokenPos ast_bad_expr_end(AstBadExpr *x) {
    return x->to;
}

TokenPos ast_ident_end(AstIdent *x) {
    return x->name_pos + x->name.len;
}

TokenPos ast_ellipsis_end(AstEllipsis *x) {
    if (x->elt != NULL) {
        return ast_node_end(x->elt);
    }
    return x->ellipsis + 3; /* len("...") */
}

TokenPos ast_basic_lit_end(AstBasicLit *x) {
    if (!token_pos_is_valid(x->value_end)) {
        /* Not from the parser, so guess, wrongly for a raw string with \r\n
         * in it, see Go issue 76031. */
        return x->value_pos + x->value.len;
    }
    return x->value_end;
}

TokenPos ast_func_lit_end(AstFuncLit *x) {
    return ast_block_stmt_end(x->body);
}

TokenPos ast_composite_lit_end(AstCompositeLit *x) {
    return x->rbrace + 1;
}

TokenPos ast_paren_expr_end(AstParenExpr *x) {
    return x->rparen + 1;
}

TokenPos ast_selector_expr_end(AstSelectorExpr *x) {
    return ast_ident_end(x->sel);
}

TokenPos ast_index_expr_end(AstIndexExpr *x) {
    return x->rbrack + 1;
}

TokenPos ast_index_list_expr_end(AstIndexListExpr *x) {
    return x->rbrack + 1;
}

TokenPos ast_slice_expr_end(AstSliceExpr *x) {
    return x->rbrack + 1;
}

TokenPos ast_type_assert_expr_end(AstTypeAssertExpr *x) {
    return x->rparen + 1;
}

TokenPos ast_call_expr_end(AstCallExpr *x) {
    return x->rparen + 1;
}

TokenPos ast_star_expr_end(AstStarExpr *x) {
    return ast_node_end(x->x);
}

TokenPos ast_unary_expr_end(AstUnaryExpr *x) {
    return ast_node_end(x->x);
}

TokenPos ast_binary_expr_end(AstBinaryExpr *x) {
    return ast_node_end(x->y);
}

TokenPos ast_key_value_expr_end(AstKeyValueExpr *x) {
    return ast_node_end(x->value);
}

TokenPos ast_array_type_end(AstArrayType *x) {
    return ast_node_end(x->elt);
}

TokenPos ast_struct_type_end(AstStructType *x) {
    return ast_field_list_end(x->fields);
}

TokenPos ast_func_type_end(AstFuncType *x) {
    if (x->results != NULL) {
        return ast_field_list_end(x->results);
    }
    return ast_field_list_end(x->params);
}

TokenPos ast_interface_type_end(AstInterfaceType *x) {
    return ast_field_list_end(x->methods);
}

TokenPos ast_map_type_end(AstMapType *x) {
    return ast_node_end(x->value);
}

TokenPos ast_chan_type_end(AstChanType *x) {
    return ast_node_end(x->value);
}

AstIdent *ast_new_ident(Alloc *a, Str name) {
    AstIdent *id = (AstIdent *)ast_node_new(a, AST_KIND_IDENT);
    if (id == NULL) {
        return NULL;
    }
    id->name = name;
    return id;
}

bool ast_is_exported(Str name) {
    return token_is_exported(name);
}

bool ast_ident_is_exported(AstIdent *id) {
    return token_is_exported(id->name);
}

Str ast_ident_string(AstIdent *id) {
    if (id != NULL) {
        return id->name;
    }
    return BURROW_S("<nil>");
}

AstExpr ast_unparen(AstExpr e) {
    while (e != NULL && e->kind == AST_KIND_PAREN_EXPR) {
        e = ((AstParenExpr *)e)->x;
    }
    return e;
}

/* -------------------------------------------------------------- statements */

TokenPos ast_bad_stmt_pos(AstBadStmt *s) {
    return s->from;
}

TokenPos ast_decl_stmt_pos(AstDeclStmt *s) {
    return ast_node_pos(s->decl);
}

TokenPos ast_empty_stmt_pos(AstEmptyStmt *s) {
    return s->semicolon;
}

TokenPos ast_labeled_stmt_pos(AstLabeledStmt *s) {
    return ast_ident_pos(s->label);
}

TokenPos ast_expr_stmt_pos(AstExprStmt *s) {
    return ast_node_pos(s->x);
}

TokenPos ast_send_stmt_pos(AstSendStmt *s) {
    return ast_node_pos(s->chan);
}

TokenPos ast_inc_dec_stmt_pos(AstIncDecStmt *s) {
    return ast_node_pos(s->x);
}

TokenPos ast_assign_stmt_pos(AstAssignStmt *s) {
    if (s->lhs.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_node_pos(an_at(s->lhs, 0));
}

TokenPos ast_go_stmt_pos(AstGoStmt *s) {
    return s->go;
}

TokenPos ast_defer_stmt_pos(AstDeferStmt *s) {
    return s->defer;
}

TokenPos ast_return_stmt_pos(AstReturnStmt *s) {
    return s->return_;
}

TokenPos ast_branch_stmt_pos(AstBranchStmt *s) {
    return s->tok_pos;
}

TokenPos ast_block_stmt_pos(AstBlockStmt *s) {
    return s->lbrace;
}

TokenPos ast_if_stmt_pos(AstIfStmt *s) {
    return s->if_;
}

TokenPos ast_case_clause_pos(AstCaseClause *s) {
    return s->case_;
}

TokenPos ast_switch_stmt_pos(AstSwitchStmt *s) {
    return s->switch_;
}

TokenPos ast_type_switch_stmt_pos(AstTypeSwitchStmt *s) {
    return s->switch_;
}

TokenPos ast_comm_clause_pos(AstCommClause *s) {
    return s->case_;
}

TokenPos ast_select_stmt_pos(AstSelectStmt *s) {
    return s->select;
}

TokenPos ast_for_stmt_pos(AstForStmt *s) {
    return s->for_;
}

TokenPos ast_range_stmt_pos(AstRangeStmt *s) {
    return s->for_;
}

TokenPos ast_bad_stmt_end(AstBadStmt *s) {
    return s->to;
}

TokenPos ast_decl_stmt_end(AstDeclStmt *s) {
    return ast_node_end(s->decl);
}

TokenPos ast_empty_stmt_end(AstEmptyStmt *s) {
    if (s->implicit) {
        return s->semicolon;
    }
    return s->semicolon + 1; /* len(";") */
}

TokenPos ast_labeled_stmt_end(AstLabeledStmt *s) {
    return ast_node_end(s->stmt);
}

TokenPos ast_expr_stmt_end(AstExprStmt *s) {
    return ast_node_end(s->x);
}

TokenPos ast_send_stmt_end(AstSendStmt *s) {
    return ast_node_end(s->value);
}

TokenPos ast_inc_dec_stmt_end(AstIncDecStmt *s) {
    return s->tok_pos + 2; /* len("++") */
}

TokenPos ast_assign_stmt_end(AstAssignStmt *s) {
    if (s->rhs.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_node_end(an_last(s->rhs));
}

TokenPos ast_go_stmt_end(AstGoStmt *s) {
    return ast_call_expr_end(s->call);
}

TokenPos ast_defer_stmt_end(AstDeferStmt *s) {
    return ast_call_expr_end(s->call);
}

TokenPos ast_return_stmt_end(AstReturnStmt *s) {
    if (s->results.len > 0) {
        return ast_node_end(an_last(s->results));
    }
    return s->return_ + 6; /* len("return") */
}

TokenPos ast_branch_stmt_end(AstBranchStmt *s) {
    if (s->label != NULL) {
        return ast_ident_end(s->label);
    }
    return s->tok_pos + token_string(s->tok, error_allocator()).len;
}

TokenPos ast_block_stmt_end(AstBlockStmt *s) {
    if (token_pos_is_valid(s->rbrace)) {
        return s->rbrace + 1;
    }
    if (s->list.len > 0) {
        return ast_node_end(an_last(s->list));
    }
    return s->lbrace + 1;
}

TokenPos ast_if_stmt_end(AstIfStmt *s) {
    if (s->else_ != NULL) {
        return ast_node_end(s->else_);
    }
    return ast_block_stmt_end(s->body);
}

TokenPos ast_case_clause_end(AstCaseClause *s) {
    if (s->body.len > 0) {
        return ast_node_end(an_last(s->body));
    }
    return s->colon + 1;
}

TokenPos ast_switch_stmt_end(AstSwitchStmt *s) {
    return ast_block_stmt_end(s->body);
}

TokenPos ast_type_switch_stmt_end(AstTypeSwitchStmt *s) {
    return ast_block_stmt_end(s->body);
}

TokenPos ast_comm_clause_end(AstCommClause *s) {
    if (s->body.len > 0) {
        return ast_node_end(an_last(s->body));
    }
    return s->colon + 1;
}

TokenPos ast_select_stmt_end(AstSelectStmt *s) {
    return ast_block_stmt_end(s->body);
}

TokenPos ast_for_stmt_end(AstForStmt *s) {
    return ast_block_stmt_end(s->body);
}

TokenPos ast_range_stmt_end(AstRangeStmt *s) {
    return ast_block_stmt_end(s->body);
}

/* ------------------------------------------------------------ declarations */

TokenPos ast_import_spec_pos(AstImportSpec *s) {
    if (s->name != NULL) {
        return ast_ident_pos(s->name);
    }
    return ast_basic_lit_pos(s->path);
}

TokenPos ast_value_spec_pos(AstValueSpec *s) {
    if (s->names.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_ident_pos((AstIdent *)an_at(s->names, 0));
}

TokenPos ast_type_spec_pos(AstTypeSpec *s) {
    return ast_ident_pos(s->name);
}

TokenPos ast_import_spec_end(AstImportSpec *s) {
    if (s->end_pos != 0) {
        return s->end_pos;
    }
    return ast_basic_lit_end(s->path);
}

TokenPos ast_value_spec_end(AstValueSpec *s) {
    if (s->values.len > 0) {
        return ast_node_end(an_last(s->values));
    }
    if (s->type != NULL) {
        return ast_node_end(s->type);
    }
    if (s->names.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_ident_end((AstIdent *)an_last(s->names));
}

TokenPos ast_type_spec_end(AstTypeSpec *s) {
    return ast_node_end(s->type);
}

TokenPos ast_bad_decl_pos(AstBadDecl *d) {
    return d->from;
}

TokenPos ast_gen_decl_pos(AstGenDecl *d) {
    return d->tok_pos;
}

TokenPos ast_func_decl_pos(AstFuncDecl *d) {
    return ast_func_type_pos(d->type);
}

TokenPos ast_bad_decl_end(AstBadDecl *d) {
    return d->to;
}

TokenPos ast_gen_decl_end(AstGenDecl *d) {
    if (token_pos_is_valid(d->rparen)) {
        return d->rparen + 1;
    }
    if (d->specs.len == 0) {
        return TOKEN_NO_POS;
    }
    return ast_node_end(an_at(d->specs, 0));
}

TokenPos ast_func_decl_end(AstFuncDecl *d) {
    if (d->body != NULL) {
        return ast_block_stmt_end(d->body);
    }
    return ast_func_type_end(d->type);
}

/* ------------------------------------------------------ files and packages */

TokenPos ast_file_pos(AstFile *f) {
    return f->package;
}

TokenPos ast_file_end(AstFile *f) {
    if (f->decls.len > 0) {
        return ast_node_end(an_last(f->decls));
    }
    return ast_ident_end(f->name);
}

TokenPos ast_package_pos(AstPackage *p) {
    (void)p;
    return TOKEN_NO_POS;
}

TokenPos ast_package_end(AstPackage *p) {
    (void)p;
    return TOKEN_NO_POS;
}

/* generator in Go, without the name it finds, which nothing exported needs. */
bool ast_is_generated(AstFile *f) {
    static const Str prefix = BURROW_S_INIT("// Code generated ");
    static const Str suffix = BURROW_S_INIT(" DO NOT EDIT.");
    for (Int i = 0; i < f->comments.len; i++) {
        AstCommentGroup *group = (AstCommentGroup *)an_at(f->comments, i);
        for (Int j = 0; j < group->list.len; j++) {
            AstComment *comment = (AstComment *)an_at(group->list, j);
            if (ast_comment_pos(comment) > f->package) {
                break; /* after the package clause */
            }
            Str text = comment->text;
            if (!strings_contains(text, prefix)) {
                continue;
            }
            for (;;) {
                Int nl = strings_index_byte(text, '\n');
                Str line = nl < 0 ? text : an_sub(text, 0, nl);
                bool found = false;
                Str rest = strings_cut_prefix(line, prefix, &found);
                if (found) {
                    (void)strings_cut_suffix(rest, suffix, &found);
                    if (found) {
                        return true;
                    }
                }
                if (nl < 0) {
                    break;
                }
                text = an_sub(text, nl + 1, text.len);
            }
        }
    }
    return false;
}

/* ------------------------------------------------------- Pos and End by kind */

TokenPos ast_node_pos(AstNode n) {
    if (n == NULL) {
        return TOKEN_NO_POS;
    }
    switch (n->kind) {
    case AST_KIND_COMMENT:
        return ast_comment_pos((AstComment *)n);
    case AST_KIND_COMMENT_GROUP:
        return ast_comment_group_pos((AstCommentGroup *)n);
    case AST_KIND_FIELD:
        return ast_field_pos((AstField *)n);
    case AST_KIND_FIELD_LIST:
        return ast_field_list_pos((AstFieldList *)n);
    case AST_KIND_BAD_EXPR:
        return ast_bad_expr_pos((AstBadExpr *)n);
    case AST_KIND_IDENT:
        return ast_ident_pos((AstIdent *)n);
    case AST_KIND_ELLIPSIS:
        return ast_ellipsis_pos((AstEllipsis *)n);
    case AST_KIND_BASIC_LIT:
        return ast_basic_lit_pos((AstBasicLit *)n);
    case AST_KIND_FUNC_LIT:
        return ast_func_lit_pos((AstFuncLit *)n);
    case AST_KIND_COMPOSITE_LIT:
        return ast_composite_lit_pos((AstCompositeLit *)n);
    case AST_KIND_PAREN_EXPR:
        return ast_paren_expr_pos((AstParenExpr *)n);
    case AST_KIND_SELECTOR_EXPR:
        return ast_selector_expr_pos((AstSelectorExpr *)n);
    case AST_KIND_INDEX_EXPR:
        return ast_index_expr_pos((AstIndexExpr *)n);
    case AST_KIND_INDEX_LIST_EXPR:
        return ast_index_list_expr_pos((AstIndexListExpr *)n);
    case AST_KIND_SLICE_EXPR:
        return ast_slice_expr_pos((AstSliceExpr *)n);
    case AST_KIND_TYPE_ASSERT_EXPR:
        return ast_type_assert_expr_pos((AstTypeAssertExpr *)n);
    case AST_KIND_CALL_EXPR:
        return ast_call_expr_pos((AstCallExpr *)n);
    case AST_KIND_STAR_EXPR:
        return ast_star_expr_pos((AstStarExpr *)n);
    case AST_KIND_UNARY_EXPR:
        return ast_unary_expr_pos((AstUnaryExpr *)n);
    case AST_KIND_BINARY_EXPR:
        return ast_binary_expr_pos((AstBinaryExpr *)n);
    case AST_KIND_KEY_VALUE_EXPR:
        return ast_key_value_expr_pos((AstKeyValueExpr *)n);
    case AST_KIND_ARRAY_TYPE:
        return ast_array_type_pos((AstArrayType *)n);
    case AST_KIND_STRUCT_TYPE:
        return ast_struct_type_pos((AstStructType *)n);
    case AST_KIND_FUNC_TYPE:
        return ast_func_type_pos((AstFuncType *)n);
    case AST_KIND_INTERFACE_TYPE:
        return ast_interface_type_pos((AstInterfaceType *)n);
    case AST_KIND_MAP_TYPE:
        return ast_map_type_pos((AstMapType *)n);
    case AST_KIND_CHAN_TYPE:
        return ast_chan_type_pos((AstChanType *)n);
    case AST_KIND_BAD_STMT:
        return ast_bad_stmt_pos((AstBadStmt *)n);
    case AST_KIND_DECL_STMT:
        return ast_decl_stmt_pos((AstDeclStmt *)n);
    case AST_KIND_EMPTY_STMT:
        return ast_empty_stmt_pos((AstEmptyStmt *)n);
    case AST_KIND_LABELED_STMT:
        return ast_labeled_stmt_pos((AstLabeledStmt *)n);
    case AST_KIND_EXPR_STMT:
        return ast_expr_stmt_pos((AstExprStmt *)n);
    case AST_KIND_SEND_STMT:
        return ast_send_stmt_pos((AstSendStmt *)n);
    case AST_KIND_INC_DEC_STMT:
        return ast_inc_dec_stmt_pos((AstIncDecStmt *)n);
    case AST_KIND_ASSIGN_STMT:
        return ast_assign_stmt_pos((AstAssignStmt *)n);
    case AST_KIND_GO_STMT:
        return ast_go_stmt_pos((AstGoStmt *)n);
    case AST_KIND_DEFER_STMT:
        return ast_defer_stmt_pos((AstDeferStmt *)n);
    case AST_KIND_RETURN_STMT:
        return ast_return_stmt_pos((AstReturnStmt *)n);
    case AST_KIND_BRANCH_STMT:
        return ast_branch_stmt_pos((AstBranchStmt *)n);
    case AST_KIND_BLOCK_STMT:
        return ast_block_stmt_pos((AstBlockStmt *)n);
    case AST_KIND_IF_STMT:
        return ast_if_stmt_pos((AstIfStmt *)n);
    case AST_KIND_CASE_CLAUSE:
        return ast_case_clause_pos((AstCaseClause *)n);
    case AST_KIND_SWITCH_STMT:
        return ast_switch_stmt_pos((AstSwitchStmt *)n);
    case AST_KIND_TYPE_SWITCH_STMT:
        return ast_type_switch_stmt_pos((AstTypeSwitchStmt *)n);
    case AST_KIND_COMM_CLAUSE:
        return ast_comm_clause_pos((AstCommClause *)n);
    case AST_KIND_SELECT_STMT:
        return ast_select_stmt_pos((AstSelectStmt *)n);
    case AST_KIND_FOR_STMT:
        return ast_for_stmt_pos((AstForStmt *)n);
    case AST_KIND_RANGE_STMT:
        return ast_range_stmt_pos((AstRangeStmt *)n);
    case AST_KIND_IMPORT_SPEC:
        return ast_import_spec_pos((AstImportSpec *)n);
    case AST_KIND_VALUE_SPEC:
        return ast_value_spec_pos((AstValueSpec *)n);
    case AST_KIND_TYPE_SPEC:
        return ast_type_spec_pos((AstTypeSpec *)n);
    case AST_KIND_BAD_DECL:
        return ast_bad_decl_pos((AstBadDecl *)n);
    case AST_KIND_GEN_DECL:
        return ast_gen_decl_pos((AstGenDecl *)n);
    case AST_KIND_FUNC_DECL:
        return ast_func_decl_pos((AstFuncDecl *)n);
    case AST_KIND_FILE:
        return ast_file_pos((AstFile *)n);
    case AST_KIND_PACKAGE:
        return ast_package_pos((AstPackage *)n);
    default:
        return TOKEN_NO_POS;
    }
}

TokenPos ast_node_end(AstNode n) {
    if (n == NULL) {
        return TOKEN_NO_POS;
    }
    switch (n->kind) {
    case AST_KIND_COMMENT:
        return ast_comment_end((AstComment *)n);
    case AST_KIND_COMMENT_GROUP:
        return ast_comment_group_end((AstCommentGroup *)n);
    case AST_KIND_FIELD:
        return ast_field_end((AstField *)n);
    case AST_KIND_FIELD_LIST:
        return ast_field_list_end((AstFieldList *)n);
    case AST_KIND_BAD_EXPR:
        return ast_bad_expr_end((AstBadExpr *)n);
    case AST_KIND_IDENT:
        return ast_ident_end((AstIdent *)n);
    case AST_KIND_ELLIPSIS:
        return ast_ellipsis_end((AstEllipsis *)n);
    case AST_KIND_BASIC_LIT:
        return ast_basic_lit_end((AstBasicLit *)n);
    case AST_KIND_FUNC_LIT:
        return ast_func_lit_end((AstFuncLit *)n);
    case AST_KIND_COMPOSITE_LIT:
        return ast_composite_lit_end((AstCompositeLit *)n);
    case AST_KIND_PAREN_EXPR:
        return ast_paren_expr_end((AstParenExpr *)n);
    case AST_KIND_SELECTOR_EXPR:
        return ast_selector_expr_end((AstSelectorExpr *)n);
    case AST_KIND_INDEX_EXPR:
        return ast_index_expr_end((AstIndexExpr *)n);
    case AST_KIND_INDEX_LIST_EXPR:
        return ast_index_list_expr_end((AstIndexListExpr *)n);
    case AST_KIND_SLICE_EXPR:
        return ast_slice_expr_end((AstSliceExpr *)n);
    case AST_KIND_TYPE_ASSERT_EXPR:
        return ast_type_assert_expr_end((AstTypeAssertExpr *)n);
    case AST_KIND_CALL_EXPR:
        return ast_call_expr_end((AstCallExpr *)n);
    case AST_KIND_STAR_EXPR:
        return ast_star_expr_end((AstStarExpr *)n);
    case AST_KIND_UNARY_EXPR:
        return ast_unary_expr_end((AstUnaryExpr *)n);
    case AST_KIND_BINARY_EXPR:
        return ast_binary_expr_end((AstBinaryExpr *)n);
    case AST_KIND_KEY_VALUE_EXPR:
        return ast_key_value_expr_end((AstKeyValueExpr *)n);
    case AST_KIND_ARRAY_TYPE:
        return ast_array_type_end((AstArrayType *)n);
    case AST_KIND_STRUCT_TYPE:
        return ast_struct_type_end((AstStructType *)n);
    case AST_KIND_FUNC_TYPE:
        return ast_func_type_end((AstFuncType *)n);
    case AST_KIND_INTERFACE_TYPE:
        return ast_interface_type_end((AstInterfaceType *)n);
    case AST_KIND_MAP_TYPE:
        return ast_map_type_end((AstMapType *)n);
    case AST_KIND_CHAN_TYPE:
        return ast_chan_type_end((AstChanType *)n);
    case AST_KIND_BAD_STMT:
        return ast_bad_stmt_end((AstBadStmt *)n);
    case AST_KIND_DECL_STMT:
        return ast_decl_stmt_end((AstDeclStmt *)n);
    case AST_KIND_EMPTY_STMT:
        return ast_empty_stmt_end((AstEmptyStmt *)n);
    case AST_KIND_LABELED_STMT:
        return ast_labeled_stmt_end((AstLabeledStmt *)n);
    case AST_KIND_EXPR_STMT:
        return ast_expr_stmt_end((AstExprStmt *)n);
    case AST_KIND_SEND_STMT:
        return ast_send_stmt_end((AstSendStmt *)n);
    case AST_KIND_INC_DEC_STMT:
        return ast_inc_dec_stmt_end((AstIncDecStmt *)n);
    case AST_KIND_ASSIGN_STMT:
        return ast_assign_stmt_end((AstAssignStmt *)n);
    case AST_KIND_GO_STMT:
        return ast_go_stmt_end((AstGoStmt *)n);
    case AST_KIND_DEFER_STMT:
        return ast_defer_stmt_end((AstDeferStmt *)n);
    case AST_KIND_RETURN_STMT:
        return ast_return_stmt_end((AstReturnStmt *)n);
    case AST_KIND_BRANCH_STMT:
        return ast_branch_stmt_end((AstBranchStmt *)n);
    case AST_KIND_BLOCK_STMT:
        return ast_block_stmt_end((AstBlockStmt *)n);
    case AST_KIND_IF_STMT:
        return ast_if_stmt_end((AstIfStmt *)n);
    case AST_KIND_CASE_CLAUSE:
        return ast_case_clause_end((AstCaseClause *)n);
    case AST_KIND_SWITCH_STMT:
        return ast_switch_stmt_end((AstSwitchStmt *)n);
    case AST_KIND_TYPE_SWITCH_STMT:
        return ast_type_switch_stmt_end((AstTypeSwitchStmt *)n);
    case AST_KIND_COMM_CLAUSE:
        return ast_comm_clause_end((AstCommClause *)n);
    case AST_KIND_SELECT_STMT:
        return ast_select_stmt_end((AstSelectStmt *)n);
    case AST_KIND_FOR_STMT:
        return ast_for_stmt_end((AstForStmt *)n);
    case AST_KIND_RANGE_STMT:
        return ast_range_stmt_end((AstRangeStmt *)n);
    case AST_KIND_IMPORT_SPEC:
        return ast_import_spec_end((AstImportSpec *)n);
    case AST_KIND_VALUE_SPEC:
        return ast_value_spec_end((AstValueSpec *)n);
    case AST_KIND_TYPE_SPEC:
        return ast_type_spec_end((AstTypeSpec *)n);
    case AST_KIND_BAD_DECL:
        return ast_bad_decl_end((AstBadDecl *)n);
    case AST_KIND_GEN_DECL:
        return ast_gen_decl_end((AstGenDecl *)n);
    case AST_KIND_FUNC_DECL:
        return ast_func_decl_end((AstFuncDecl *)n);
    case AST_KIND_FILE:
        return ast_file_end((AstFile *)n);
    case AST_KIND_PACKAGE:
        return ast_package_end((AstPackage *)n);
    default:
        return TOKEN_NO_POS;
    }
}

TokenPos ast_expr_pos(AstExpr x) {
    return ast_node_pos(x);
}

TokenPos ast_expr_end(AstExpr x) {
    return ast_node_end(x);
}

TokenPos ast_stmt_pos(AstStmt s) {
    return ast_node_pos(s);
}

TokenPos ast_stmt_end(AstStmt s) {
    return ast_node_end(s);
}

TokenPos ast_decl_pos(AstDecl d) {
    return ast_node_pos(d);
}

TokenPos ast_decl_end(AstDecl d) {
    return ast_node_end(d);
}

TokenPos ast_spec_pos(AstSpec s) {
    return ast_node_pos(s);
}

TokenPos ast_spec_end(AstSpec s) {
    return ast_node_end(s);
}

/* ----------------------------------------------------------------- walking */

AstVisitor ast_visitor_visit(AstVisitor v, AstNode node) {
    return v.vt->visit(v.data, node);
}

static void an_walk_list(AstVisitor v, Slice list) {
    for (Int i = 0; i < list.len; i++) {
        ast_walk(v, an_at(list, i));
    }
}

static void an_walk_opt(AstVisitor v, AstNode n) {
    if (n != NULL) {
        ast_walk(v, n);
    }
}

BURROW_NORETURN static void an_walk_unexpected(AstNode node) {
    if (node == NULL) {
        panic_str(BURROW_S("ast.Walk: unexpected node type <nil>"));
    }
    panic_str(fmt_sprintf_v(error_allocator(), "ast.Walk: unexpected node kind %d",
                            (Int)node->kind));
}

static void an_walk_package(AstVisitor v, AstPackage *p) {
    MapIter it = map_iter(p->files);
    const void *k = NULL;
    void *f = NULL;
    while (map_next(&it, &k, &f)) {
        ast_walk(v, *(AstNode *)f);
    }
}

/* The children of node in walk.go's order. Split from ast_walk to keep each
 * function a size a reader can hold. */
static void an_walk_children(AstVisitor v, AstNode node) {
    switch (node->kind) {
    /* comments and fields */
    case AST_KIND_COMMENT:
        break;
    case AST_KIND_COMMENT_GROUP:
        an_walk_list(v, ((AstCommentGroup *)node)->list);
        break;
    case AST_KIND_FIELD: {
        AstField *n = (AstField *)node;
        an_walk_opt(v, (AstNode)n->doc);
        an_walk_list(v, n->names);
        an_walk_opt(v, n->type);
        an_walk_opt(v, (AstNode)n->tag);
        an_walk_opt(v, (AstNode)n->comment);
        break;
    }
    case AST_KIND_FIELD_LIST:
        an_walk_list(v, ((AstFieldList *)node)->list);
        break;

    /* expressions */
    case AST_KIND_BAD_EXPR:
    case AST_KIND_IDENT:
    case AST_KIND_BASIC_LIT:
        break;
    case AST_KIND_ELLIPSIS:
        an_walk_opt(v, ((AstEllipsis *)node)->elt);
        break;
    case AST_KIND_FUNC_LIT:
        ast_walk(v, (AstNode)((AstFuncLit *)node)->type);
        ast_walk(v, (AstNode)((AstFuncLit *)node)->body);
        break;
    case AST_KIND_COMPOSITE_LIT:
        an_walk_opt(v, ((AstCompositeLit *)node)->type);
        an_walk_list(v, ((AstCompositeLit *)node)->elts);
        break;
    case AST_KIND_PAREN_EXPR:
        ast_walk(v, ((AstParenExpr *)node)->x);
        break;
    case AST_KIND_SELECTOR_EXPR:
        ast_walk(v, ((AstSelectorExpr *)node)->x);
        ast_walk(v, (AstNode)((AstSelectorExpr *)node)->sel);
        break;
    case AST_KIND_INDEX_EXPR:
        ast_walk(v, ((AstIndexExpr *)node)->x);
        ast_walk(v, ((AstIndexExpr *)node)->index);
        break;
    case AST_KIND_INDEX_LIST_EXPR:
        ast_walk(v, ((AstIndexListExpr *)node)->x);
        an_walk_list(v, ((AstIndexListExpr *)node)->indices);
        break;
    case AST_KIND_SLICE_EXPR: {
        AstSliceExpr *n = (AstSliceExpr *)node;
        ast_walk(v, n->x);
        an_walk_opt(v, n->low);
        an_walk_opt(v, n->high);
        an_walk_opt(v, n->max);
        break;
    }
    case AST_KIND_TYPE_ASSERT_EXPR:
        ast_walk(v, ((AstTypeAssertExpr *)node)->x);
        an_walk_opt(v, ((AstTypeAssertExpr *)node)->type);
        break;
    case AST_KIND_CALL_EXPR:
        ast_walk(v, ((AstCallExpr *)node)->fun);
        an_walk_list(v, ((AstCallExpr *)node)->args);
        break;
    case AST_KIND_STAR_EXPR:
        ast_walk(v, ((AstStarExpr *)node)->x);
        break;
    case AST_KIND_UNARY_EXPR:
        ast_walk(v, ((AstUnaryExpr *)node)->x);
        break;
    case AST_KIND_BINARY_EXPR:
        ast_walk(v, ((AstBinaryExpr *)node)->x);
        ast_walk(v, ((AstBinaryExpr *)node)->y);
        break;
    case AST_KIND_KEY_VALUE_EXPR:
        ast_walk(v, ((AstKeyValueExpr *)node)->key);
        ast_walk(v, ((AstKeyValueExpr *)node)->value);
        break;

    /* types */
    case AST_KIND_ARRAY_TYPE:
        an_walk_opt(v, ((AstArrayType *)node)->len);
        ast_walk(v, ((AstArrayType *)node)->elt);
        break;
    case AST_KIND_STRUCT_TYPE:
        ast_walk(v, (AstNode)((AstStructType *)node)->fields);
        break;
    case AST_KIND_FUNC_TYPE: {
        AstFuncType *n = (AstFuncType *)node;
        an_walk_opt(v, (AstNode)n->type_params);
        an_walk_opt(v, (AstNode)n->params);
        an_walk_opt(v, (AstNode)n->results);
        break;
    }
    case AST_KIND_INTERFACE_TYPE:
        ast_walk(v, (AstNode)((AstInterfaceType *)node)->methods);
        break;
    case AST_KIND_MAP_TYPE:
        ast_walk(v, ((AstMapType *)node)->key);
        ast_walk(v, ((AstMapType *)node)->value);
        break;
    case AST_KIND_CHAN_TYPE:
        ast_walk(v, ((AstChanType *)node)->value);
        break;

    /* statements */
    case AST_KIND_BAD_STMT:
        break;
    case AST_KIND_DECL_STMT:
        ast_walk(v, ((AstDeclStmt *)node)->decl);
        break;
    case AST_KIND_EMPTY_STMT:
        break;
    case AST_KIND_LABELED_STMT:
        ast_walk(v, (AstNode)((AstLabeledStmt *)node)->label);
        ast_walk(v, ((AstLabeledStmt *)node)->stmt);
        break;
    case AST_KIND_EXPR_STMT:
        ast_walk(v, ((AstExprStmt *)node)->x);
        break;
    case AST_KIND_SEND_STMT:
        ast_walk(v, ((AstSendStmt *)node)->chan);
        ast_walk(v, ((AstSendStmt *)node)->value);
        break;
    case AST_KIND_INC_DEC_STMT:
        ast_walk(v, ((AstIncDecStmt *)node)->x);
        break;
    case AST_KIND_ASSIGN_STMT:
        an_walk_list(v, ((AstAssignStmt *)node)->lhs);
        an_walk_list(v, ((AstAssignStmt *)node)->rhs);
        break;
    case AST_KIND_GO_STMT:
        ast_walk(v, (AstNode)((AstGoStmt *)node)->call);
        break;
    case AST_KIND_DEFER_STMT:
        ast_walk(v, (AstNode)((AstDeferStmt *)node)->call);
        break;
    case AST_KIND_RETURN_STMT:
        an_walk_list(v, ((AstReturnStmt *)node)->results);
        break;
    case AST_KIND_BRANCH_STMT:
        an_walk_opt(v, (AstNode)((AstBranchStmt *)node)->label);
        break;
    case AST_KIND_BLOCK_STMT:
        an_walk_list(v, ((AstBlockStmt *)node)->list);
        break;
    case AST_KIND_IF_STMT: {
        AstIfStmt *n = (AstIfStmt *)node;
        an_walk_opt(v, n->init);
        ast_walk(v, n->cond);
        ast_walk(v, (AstNode)n->body);
        an_walk_opt(v, n->else_);
        break;
    }
    case AST_KIND_CASE_CLAUSE:
        an_walk_list(v, ((AstCaseClause *)node)->list);
        an_walk_list(v, ((AstCaseClause *)node)->body);
        break;
    case AST_KIND_SWITCH_STMT: {
        AstSwitchStmt *n = (AstSwitchStmt *)node;
        an_walk_opt(v, n->init);
        an_walk_opt(v, n->tag);
        ast_walk(v, (AstNode)n->body);
        break;
    }
    case AST_KIND_TYPE_SWITCH_STMT: {
        AstTypeSwitchStmt *n = (AstTypeSwitchStmt *)node;
        an_walk_opt(v, n->init);
        ast_walk(v, n->assign);
        ast_walk(v, (AstNode)n->body);
        break;
    }
    case AST_KIND_COMM_CLAUSE:
        an_walk_opt(v, ((AstCommClause *)node)->comm);
        an_walk_list(v, ((AstCommClause *)node)->body);
        break;
    case AST_KIND_SELECT_STMT:
        ast_walk(v, (AstNode)((AstSelectStmt *)node)->body);
        break;
    case AST_KIND_FOR_STMT: {
        AstForStmt *n = (AstForStmt *)node;
        an_walk_opt(v, n->init);
        an_walk_opt(v, n->cond);
        an_walk_opt(v, n->post);
        ast_walk(v, (AstNode)n->body);
        break;
    }
    case AST_KIND_RANGE_STMT: {
        AstRangeStmt *n = (AstRangeStmt *)node;
        an_walk_opt(v, n->key);
        an_walk_opt(v, n->value);
        ast_walk(v, n->x);
        ast_walk(v, (AstNode)n->body);
        break;
    }

    /* declarations */
    case AST_KIND_IMPORT_SPEC: {
        AstImportSpec *n = (AstImportSpec *)node;
        an_walk_opt(v, (AstNode)n->doc);
        an_walk_opt(v, (AstNode)n->name);
        ast_walk(v, (AstNode)n->path);
        an_walk_opt(v, (AstNode)n->comment);
        break;
    }
    case AST_KIND_VALUE_SPEC: {
        AstValueSpec *n = (AstValueSpec *)node;
        an_walk_opt(v, (AstNode)n->doc);
        an_walk_list(v, n->names);
        an_walk_opt(v, n->type);
        an_walk_list(v, n->values);
        an_walk_opt(v, (AstNode)n->comment);
        break;
    }
    case AST_KIND_TYPE_SPEC: {
        AstTypeSpec *n = (AstTypeSpec *)node;
        an_walk_opt(v, (AstNode)n->doc);
        ast_walk(v, (AstNode)n->name);
        an_walk_opt(v, (AstNode)n->type_params);
        ast_walk(v, n->type);
        an_walk_opt(v, (AstNode)n->comment);
        break;
    }
    case AST_KIND_BAD_DECL:
        break;
    case AST_KIND_GEN_DECL:
        an_walk_opt(v, (AstNode)((AstGenDecl *)node)->doc);
        an_walk_list(v, ((AstGenDecl *)node)->specs);
        break;
    case AST_KIND_FUNC_DECL: {
        AstFuncDecl *n = (AstFuncDecl *)node;
        an_walk_opt(v, (AstNode)n->doc);
        an_walk_opt(v, (AstNode)n->recv);
        ast_walk(v, (AstNode)n->name);
        ast_walk(v, (AstNode)n->type);
        an_walk_opt(v, (AstNode)n->body);
        break;
    }

    /* files and packages */
    case AST_KIND_FILE: {
        AstFile *n = (AstFile *)node;
        an_walk_opt(v, (AstNode)n->doc);
        ast_walk(v, (AstNode)n->name);
        an_walk_list(v, n->decls);
        /* not n->comments, which the nodes they belong to have had already */
        break;
    }
    case AST_KIND_PACKAGE:
        an_walk_package(v, (AstPackage *)node);
        break;

    default:
        an_walk_unexpected(node);
    }
}

void ast_walk(AstVisitor v, AstNode node) {
    v = ast_visitor_visit(v, node);
    if (v.vt == NULL) {
        return;
    }
    if (node == NULL) {
        an_walk_unexpected(node);
    }
    an_walk_children(v, node);
    (void)ast_visitor_visit(v, NULL);
}

/* inspector in Go: a func(Node) bool used as a Visitor. */
static AstVisitor an_inspector_visit(void *self, AstNode node);

static const AstVisitorVT an_inspector_vt = {NULL, an_inspector_visit};

static AstVisitor an_inspector_visit(void *self, AstNode node) {
    AstInspectFunc *f = (AstInspectFunc *)self;
    if (f->f(f->env, node)) {
        return (AstVisitor){&an_inspector_vt, self};
    }
    return (AstVisitor){NULL, NULL};
}

void ast_inspect(AstNode node, AstInspectFunc f) {
    ast_walk((AstVisitor){&an_inspector_vt, &f}, node);
}

typedef struct AnPreorder {
    IterYield yield;
    bool ok;
} AnPreorder;

static bool an_preorder_visit(void *env, AstNode n) {
    AnPreorder *p = (AnPreorder *)env;
    if (n != NULL) {
        /* yield must not be called once ok is false */
        p->ok = p->ok && p->yield.f(p->yield.env, &n);
    }
    return p->ok;
}

static void an_preorder_seq(void *env, IterYield yield) {
    AnPreorder p = {yield, true};
    ast_inspect((AstNode)env, (AstInspectFunc){an_preorder_visit, &p});
}

IterSeq ast_preorder(AstNode root) {
    return (IterSeq){an_preorder_seq, root};
}

typedef struct AnStack {
    Alloc *a;
    Slice stack;
    AstPreorderStackFunc f;
} AnStack;

static bool an_stack_visit(void *env, AstNode n) {
    AnStack *s = (AnStack *)env;
    if (n == NULL) {
        s->stack.len--; /* pop */
        return true;
    }
    if (!s->f.f(s->f.env, n, s->stack)) {
        /* do not push, as there will be no pop to match */
        return false;
    }
    Slice grown = slice_append(s->a, s->stack, &n, 1); /* push */
    if (grown.p == NULL) {
        panic_str(BURROW_S("go/ast: out of memory"));
    }
    s->stack = grown;
    return true;
}

void ast_preorder_stack(Alloc *a, AstNode root, Slice stack, AstPreorderStackFunc f) {
    if (stack.elem == NULL) {
        stack.elem = TYPE_AST_NODE;
    }
    Int before = stack.len;
    AnStack s = {a, stack, f};
    ast_inspect(root, (AstInspectFunc){an_stack_visit, &s});
    if (s.stack.len != before) {
        panic_str(BURROW_S("push/pop mismatch"));
    }
}

/* -------------------------------------------------------------- directives */

/* directiveScanner in Go: the rest of a comment and where it is. */
typedef struct AnScan {
    Str str;
    TokenPos pos;
} AnScan;

static void an_scan_skip(AnScan *s, Int n) {
    s->pos += n;
    s->str = an_sub(s->str, n, s->str.len);
}

static Str an_scan_take(AnScan *s, Int n) {
    Str res = an_sub(s->str, 0, n);
    an_scan_skip(s, n);
    return res;
}

static bool an_is_space(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

static Str an_scan_take_non_space(AnScan *s) {
    Int i = strings_index_func(s->str, (RuneFunc){an_is_space, NULL});
    if (i == -1) {
        i = s->str.len;
    }
    return an_scan_take(s, i);
}

static void an_scan_skip_space(AnScan *s) {
    Str trim = strings_trim_left_func(s->str, (RuneFunc){an_is_space, NULL});
    an_scan_skip(s, s->str.len - trim.len);
}

AstDirective ast_parse_directive(TokenPos pos, Str c, bool *ok) {
    AstDirective d = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_STR_EMPTY, 0, 0};
    *ok = false;
    /* The fast way out for most comments: a directive is a line comment that
     * starts with [a-z0-9]. */
    if (!(c.len >= 3 && c.p[0] == '/' && c.p[1] == '/' && an_is_lower_alnum(c.p[2]))) {
        return d;
    }
    AnScan buf = {c, pos};
    an_scan_skip(&buf, 2); /* len("//") */

    /* Check for a valid directive and parse the tool part, the way
     * an_is_directive does. */
    Int colon = strings_index_byte(buf.str, ':');
    if (colon <= 0 || colon + 1 >= buf.str.len) {
        return d;
    }
    for (Int i = 0; i <= colon + 1; i++) {
        if (i == colon) {
            continue;
        }
        if (!an_is_lower_alnum(buf.str.p[i])) {
            return d;
        }
    }
    d.tool = an_scan_take(&buf, colon);
    an_scan_skip(&buf, 1); /* len(":") */

    /* parse the name and the arguments */
    d.name = an_scan_take_non_space(&buf);
    an_scan_skip_space(&buf);
    d.args_pos = buf.pos;
    d.args = strings_trim_right_func(buf.str, (RuneFunc){an_is_space, NULL});
    d.slash = pos;
    *ok = true;
    return d;
}

TokenPos ast_directive_pos(AstDirective *d) {
    return d->slash;
}

TokenPos ast_directive_end(AstDirective *d) {
    return d->args_pos + d->args.len;
}

static Error an_bad_quote(AstDirective *d, Str rest) {
    return fmt_errorf_v("invalid quoted string in //%s:%s: %s", d->tool, d->name, rest);
}

Slice ast_directive_parse_args(AstDirective *d, Alloc *a, Error *err) {
    *err = BURROW_NO_ERROR;
    AnScan args = {d->args, d->args_pos};
    Slice list = slice_make(a, TYPE_OF(AstDirectiveArg), 0, 0);
    for (an_scan_skip_space(&args); args.str.len > 0; an_scan_skip_space(&args)) {
        AstDirectiveArg arg = {BURROW_STR_EMPTY, args.pos};
        if (args.str.p[0] == '`' || args.str.p[0] == '"') {
            Error qerr = BURROW_NO_ERROR;
            Str q = strconv_quoted_prefix(args.str, &qerr);
            if (!BURROW_OK(qerr)) { /* always strconv.ErrSyntax */
                *err = an_bad_quote(d, args.str);
                return (Slice){NULL, 0, 0, TYPE_OF(AstDirectiveArg)};
            }
            /* any errors were found by strconv_quoted_prefix */
            arg.arg = strconv_unquote(a, an_scan_take(&args, q.len), &qerr);
            if (!BURROW_OK(qerr)) {
                *err = qerr;
                return (Slice){NULL, 0, 0, TYPE_OF(AstDirectiveArg)};
            }
            /* the quoted string must be followed by a space or nothing */
            if (args.str.len > 0) {
                Int size = 0;
                Rune r = utf8_decode_rune_in_string(args.str, &size);
                if (!unicode_is_space(r)) {
                    *err = an_bad_quote(d, args.str);
                    return (Slice){NULL, 0, 0, TYPE_OF(AstDirectiveArg)};
                }
            }
        } else {
            arg.arg = an_scan_take_non_space(&args);
        }
        Slice grown = slice_append(a, list, &arg, 1);
        if (grown.p == NULL) {
            *err = burrow_err_out_of_memory;
            return (Slice){NULL, 0, 0, TYPE_OF(AstDirectiveArg)};
        }
        list = grown;
    }
    return list;
}
