/* go/parser: the identifier resolution the parser does after a file.
 *
 * Derived from Go's src/go/parser/resolver.go.
 * Go source: go1.27.1.
 *
 * The resolver walks the declarations of a file, declares the names it meets
 * in ast.Scopes and points each identifier at the object it names. Go
 * deprecates this in favour of go/types but the parser still does it unless
 * the mode says not to.
 *
 * A "defer r.closeScope()" in Go becomes a count of the scopes a case opened,
 * all closed when the case is done, which is when the defers would run.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"
#include "burrow/go/token.h"

#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/panic.h"

#include <string.h>

/* The parser's side, in parser.c. */
BURROW_NORETURN void burrow__parser_bail(void *p, TokenPos pos, Str msg);
BURROW_NORETURN void burrow__parser_oom(void *p);
void burrow__parser_error(void *p, TokenPos pos, Str msg);
bool burrow__parser_stack_low(void *p, Int depth);
void burrow__parser_resolve_file(void *p, Alloc *a, AstFile *file, TokenFile *handle,
                                 bool decl_errors);

/* maxScopeDepth, how deep scopes may nest. */
enum { GPR_MAX_SCOPE_DEPTH = 1000 };

typedef struct Gpr {
    void *p; /* the parser, for errors and bailouts */
    Alloc *a;
    TokenFile *handle;
    bool decl_errors; /* Go's declErr != nil */

    AstScope *pkg_scope; /* pkg_scope->outer == NULL */
    AstScope *top_scope; /* top-most scope; may be pkg_scope */
    Slice unresolved;    /* of AstIdent *, the unresolved identifiers */
    Int depth;           /* scope depth */
    Int nest;            /* how deep the walk is, for the stack check */

    /* Label scopes */
    AstScope *label_scope; /* label scope for current function */
    /* Go's targetStack, a stack of lists of unresolved labels, kept as one
     * list of AstIdent * and the index in it where each level starts. */
    Slice targets;
    Slice target_marks; /* of Int */

    /* The sentinel Go's unresolved is, the object of an identifier until the
     * end of the file. */
    AstObject *unresolved_obj;
} Gpr;

/* MSVC rejects a const object declared ahead of its definition, so the table
 * is defined here and its function declared ahead of it instead. */
static AstVisitor gpr_visit(void *self, AstNode node);
static const AstVisitorVT gpr_vt = {NULL, gpr_visit};

static void gpr_assert(bool cond, const char *msg) {
    if (!cond) {
        Str m = str_from_bytes(msg, (Int)strlen(msg));
        panic_str(fmt_sprintf_v(error_allocator(), "go/parser internal error: %s", m));
    }
}

static void gpr_append(Gpr *r, Slice *s, const void *v) {
    Slice next = slice_append(r->a, *s, v, 1);
    if (next.len != s->len + 1)
        burrow__parser_oom(r->p);
    *s = next;
}

/* ptr boxed in an Any by its pointer type t, the way an object's decl is. */
static Any gpr_box(Gpr *r, const Type *t, const void *ptr) {
    Any v = any_box(r->a, BURROW_ANY(t, &ptr));
    if (v.t == NULL)
        burrow__parser_oom(r->p);
    return v;
}

static void gpr_walk(Gpr *r, AstNode node) {
    ast_walk((AstVisitor){&gpr_vt, r}, node);
}

static void gpr_open_scope(Gpr *r, TokenPos pos) {
    r->depth++;
    if (r->depth > GPR_MAX_SCOPE_DEPTH)
        burrow__parser_bail(
            r->p, pos, BURROW_S("exceeded max scope depth during object resolution"));
    AstScope *s = ast_new_scope(r->a, r->top_scope);
    if (s == NULL)
        burrow__parser_oom(r->p);
    r->top_scope = s;
}

static void gpr_close_scope(Gpr *r) {
    r->depth--;
    r->top_scope = r->top_scope->outer;
}

static void gpr_close_scopes(Gpr *r, Int n) {
    for (; n > 0; n--)
        gpr_close_scope(r);
}

static void gpr_open_label_scope(Gpr *r) {
    AstScope *s = ast_new_scope(r->a, r->label_scope);
    if (s == NULL)
        burrow__parser_oom(r->p);
    r->label_scope = s;
    Int mark = r->targets.len;
    gpr_append(r, &r->target_marks, &mark);
}

static void gpr_close_label_scope(Gpr *r) {
    Int n = r->target_marks.len - 1;
    Int start = BURROW_AT(Int, r->target_marks, n);
    AstScope *scope = r->label_scope;
    for (Int i = start; i < r->targets.len; i++) {
        AstIdent *ident = BURROW_AT(AstIdent *, r->targets, i);
        ident->obj = ast_scope_lookup(scope, ident->name);
        if (ident->obj == NULL && r->decl_errors)
            burrow__parser_error(
                r->p, ast_ident_pos(ident),
                fmt_sprintf_v(r->a, "label %s undefined", ident->name));
    }
    r->targets.len = start;
    r->target_marks.len = n;
    r->label_scope = r->label_scope->outer;
}

static void gpr_declare(Gpr *r, Any decl, Any data, AstScope *scope, AstObjKind kind,
                        AstIdent *ident) {
    if (ident->obj != NULL)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "%d: identifier %s already declared or resolved",
                                ast_ident_pos(ident), ident->name));
    AstObject *obj = ast_new_obj(r->a, kind, ident->name);
    if (obj == NULL)
        burrow__parser_oom(r->p);
    obj->decl = decl;
    obj->data = data;
    if (decl.t != TYPE_AST_IDENT_PTR)
        ident->obj = obj;
    if (!str_eq(ident->name, BURROW_S("_"))) {
        AstObject *alt = ast_scope_insert(scope, obj);
        if (alt != NULL && r->decl_errors) {
            Str prev_decl = BURROW_STR_EMPTY;
            TokenPos pos = ast_object_pos(alt);
            if (token_pos_is_valid(pos)) {
                Str where =
                    token_position_string(token_file_position(r->handle, pos), r->a);
                prev_decl =
                    fmt_sprintf_v(r->a, "\n\tprevious declaration at %s", where);
            }
            burrow__parser_error(r->p, ast_ident_pos(ident),
                                 fmt_sprintf_v(r->a, "%s redeclared in this block%s",
                                               ident->name, prev_decl));
        }
    }
}

static void gpr_declare_all(Gpr *r, Any decl, Any data, AstScope *scope,
                            AstObjKind kind, Slice idents) {
    for (Int i = 0; i < idents.len; i++)
        gpr_declare(r, decl, data, scope, kind, BURROW_AT(AstIdent *, idents, i));
}

static void gpr_short_var_decl(Gpr *r, AstAssignStmt *decl) {
    /* Go spec: A short variable declaration may redeclare variables provided
     * they were originally declared in the same block with the same type,
     * and at least one of the non-blank variables is new. */
    Any boxed = gpr_box(r, TYPE_OF(AstAssignStmtPtr), decl);
    Int n = 0; /* number of new variables */
    for (Int i = 0; i < decl->lhs.len; i++) {
        AstExpr x = BURROW_AT(AstExpr, decl->lhs, i);
        if (x == NULL || x->kind != (Int)AST_KIND_IDENT)
            continue;
        AstIdent *ident = (AstIdent *)x;
        gpr_assert(ident->obj == NULL, "identifier already declared or resolved");
        AstObject *obj = ast_new_obj(r->a, AST_VAR, ident->name);
        if (obj == NULL)
            burrow__parser_oom(r->p);
        /* remember corresponding assignment for other tools */
        obj->decl = boxed;
        ident->obj = obj;
        if (!str_eq(ident->name, BURROW_S("_"))) {
            AstObject *alt = ast_scope_insert(r->top_scope, obj);
            if (alt != NULL)
                ident->obj = alt; /* redeclaration */
            else
                n++; /* new declaration */
        }
    }
    if (n == 0 && r->decl_errors)
        burrow__parser_error(r->p, ast_expr_pos(BURROW_AT(AstExpr, decl->lhs, 0)),
                             BURROW_S("no new variables on left side of :="));
}

/* If collect_unresolved is set, an identifier that cannot be resolved is
 * collected in the list of unresolved identifiers. */
static void gpr_resolve(Gpr *r, AstIdent *ident, bool collect_unresolved) {
    if (ident->obj != NULL) {
        Str where = token_position_string(
            token_file_position(r->handle, ast_ident_pos(ident)), error_allocator());
        panic_str(fmt_sprintf_v(error_allocator(),
                                "%s: identifier %s already declared or resolved", where,
                                ident->name));
    }
    /* '_' should never refer to existing declarations, because it has
     * special handling in the spec. */
    if (str_eq(ident->name, BURROW_S("_")))
        return;
    for (AstScope *s = r->top_scope; s != NULL; s = s->outer) {
        AstObject *obj = ast_scope_lookup(s, ident->name);
        if (obj != NULL) {
            gpr_assert(obj->name.len != 0, "obj with no name");
            /* Identifiers (for receiver type parameters) are written to the
             * scope, but never set as the resolved object. See
             * go.dev/issue/50956. */
            if (obj->decl.t != TYPE_AST_IDENT_PTR)
                ident->obj = obj;
            return;
        }
    }
    /* all local scopes are known, so any unresolved identifier must be
     * found either in the file scope, package scope (perhaps in another
     * file), or universe scope --- collect them so that they can be
     * resolved later */
    if (collect_unresolved) {
        ident->obj = r->unresolved_obj;
        gpr_append(r, &r->unresolved, &ident);
    }
}

static void gpr_walk_exprs(Gpr *r, Slice list) {
    for (Int i = 0; i < list.len; i++)
        gpr_walk(r, BURROW_AT(AstExpr, list, i));
}

static void gpr_walk_lhs(Gpr *r, Slice list) {
    for (Int i = 0; i < list.len; i++) {
        AstExpr expr = ast_unparen(BURROW_AT(AstExpr, list, i));
        if (expr != NULL && expr->kind != (Int)AST_KIND_IDENT)
            gpr_walk(r, expr);
    }
}

static void gpr_walk_stmts(Gpr *r, Slice list) {
    for (Int i = 0; i < list.len; i++)
        gpr_walk(r, BURROW_AT(AstStmt, list, i));
}

static void gpr_resolve_list(Gpr *r, AstFieldList *list) {
    if (list == NULL)
        return;
    for (Int i = 0; i < list->list.len; i++) {
        AstField *f = BURROW_AT(AstField *, list->list, i);
        if (f->type != NULL)
            gpr_walk(r, f->type);
    }
}

static void gpr_declare_list(Gpr *r, AstFieldList *list, AstObjKind kind) {
    if (list == NULL)
        return;
    for (Int i = 0; i < list->list.len; i++) {
        AstField *f = BURROW_AT(AstField *, list->list, i);
        if (f->names.len == 0)
            continue;
        Any decl = gpr_box(r, TYPE_OF(AstFieldPtr), f);
        gpr_declare_all(r, decl, (Any){NULL, NULL}, r->top_scope, kind, f->names);
    }
}

static void gpr_walk_func_type(Gpr *r, AstFuncType *typ) {
    /* typ.TypeParams must be walked separately for FuncDecls. */
    gpr_resolve_list(r, typ->params);
    gpr_resolve_list(r, typ->results);
    gpr_declare_list(r, typ->params, AST_VAR);
    gpr_declare_list(r, typ->results, AST_VAR);
}

static void gpr_walk_recv(Gpr *r, AstFieldList *recv) {
    /* If our receiver has receiver type parameters, we must declare them
     * before trying to resolve the rest of the receiver, and avoid
     * re-resolving the type parameter identifiers. */
    if (recv == NULL || recv->list.len == 0)
        return; /* nothing to do */
    AstExpr typ = BURROW_AT(AstField *, recv->list, 0)->type;
    if (typ != NULL && typ->kind == (Int)AST_KIND_STAR_EXPR)
        typ = ((AstStarExpr *)typ)->x;

    Slice declare_exprs = slice_nil(TYPE_AST_EXPR); /* exprs to declare */
    Slice resolve_exprs = slice_nil(TYPE_AST_EXPR); /* exprs to resolve */
    if (typ != NULL && typ->kind == (Int)AST_KIND_INDEX_EXPR) {
        AstIndexExpr *ix = (AstIndexExpr *)typ;
        gpr_append(r, &declare_exprs, &ix->index);
        gpr_append(r, &resolve_exprs, &ix->x);
    } else if (typ != NULL && typ->kind == (Int)AST_KIND_INDEX_LIST_EXPR) {
        AstIndexListExpr *ix = (AstIndexListExpr *)typ;
        declare_exprs = ix->indices;
        gpr_append(r, &resolve_exprs, &ix->x);
    } else {
        gpr_append(r, &resolve_exprs, &typ);
    }
    for (Int i = 0; i < declare_exprs.len; i++) {
        AstExpr expr = BURROW_AT(AstExpr, declare_exprs, i);
        if (expr != NULL && expr->kind == (Int)AST_KIND_IDENT) {
            AstIdent *id = (AstIdent *)expr;
            gpr_declare(r, gpr_box(r, TYPE_AST_IDENT_PTR, id), (Any){NULL, NULL},
                        r->top_scope, AST_TYP, id);
        } else {
            /* The receiver type parameter expression is invalid, but try to
             * resolve it anyway for consistency. */
            gpr_append(r, &resolve_exprs, &expr);
        }
    }
    for (Int i = 0; i < resolve_exprs.len; i++) {
        AstExpr expr = BURROW_AT(AstExpr, resolve_exprs, i);
        if (expr != NULL)
            gpr_walk(r, expr);
    }
    /* The receiver is invalid, but try to resolve it anyway for
     * consistency. */
    for (Int i = 1; i < recv->list.len; i++) {
        AstField *f = BURROW_AT(AstField *, recv->list, i);
        if (f->type != NULL)
            gpr_walk(r, f->type);
    }
}

static void gpr_walk_field_list(Gpr *r, AstFieldList *list, AstObjKind kind) {
    if (list == NULL)
        return;
    gpr_resolve_list(r, list);
    gpr_declare_list(r, list, kind);
}

static void gpr_walk_tparams(Gpr *r, AstFieldList *list) {
    /* walkTParams is like walkFieldList, but declares type parameters
     * eagerly so that they may be resolved in the constraint expressions
     * held in the field Type. */
    gpr_declare_list(r, list, AST_TYP);
    gpr_resolve_list(r, list);
}

static void gpr_walk_body(Gpr *r, AstBlockStmt *body) {
    if (body == NULL)
        return;
    gpr_open_label_scope(r);
    gpr_walk_stmts(r, body->list);
    gpr_close_label_scope(r);
}

/* The cases of Visit that do something, which give back true. On false the
 * walk goes on into the node's children. */
static bool gpr_visit_node(Gpr *r, AstNode node) {
    Int scopes = 0; /* scopes to close when done, Go's deferred closeScopes */
    switch (node->kind) {
    case AST_KIND_IDENT:
        gpr_resolve(r, (AstIdent *)node, true);
        break;
    case AST_KIND_FUNC_LIT: {
        AstFuncLit *n = (AstFuncLit *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_func_type(r, n->type);
        gpr_walk_body(r, n->body);
        break;
    }
    case AST_KIND_SELECTOR_EXPR:
        gpr_walk(r, ((AstSelectorExpr *)node)->x);
        /* Note: don't try to resolve n.Sel, as we don't support qualified
         * resolution. */
        break;
    case AST_KIND_STRUCT_TYPE:
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_field_list(r, ((AstStructType *)node)->fields, AST_VAR);
        break;
    case AST_KIND_FUNC_TYPE:
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_func_type(r, (AstFuncType *)node);
        break;
    case AST_KIND_COMPOSITE_LIT: {
        AstCompositeLit *n = (AstCompositeLit *)node;
        if (n->type != NULL)
            gpr_walk(r, n->type);
        for (Int i = 0; i < n->elts.len; i++) {
            AstExpr e = BURROW_AT(AstExpr, n->elts, i);
            if (e != NULL && e->kind == (Int)AST_KIND_KEY_VALUE_EXPR) {
                AstKeyValueExpr *kv = (AstKeyValueExpr *)e;
                /* See go.dev/issue/45160: try to resolve composite lit keys,
                 * but don't collect them as unresolved if resolution failed.
                 * This replicates existing behavior when resolving during
                 * parsing. */
                if (kv->key != NULL && kv->key->kind == (Int)AST_KIND_IDENT)
                    gpr_resolve(r, (AstIdent *)kv->key, false);
                else
                    gpr_walk(r, kv->key);
                gpr_walk(r, kv->value);
            } else {
                gpr_walk(r, e);
            }
        }
        break;
    }
    case AST_KIND_INTERFACE_TYPE:
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_field_list(r, ((AstInterfaceType *)node)->methods, AST_FUN);
        break;

    /* Statements */
    case AST_KIND_LABELED_STMT: {
        AstLabeledStmt *n = (AstLabeledStmt *)node;
        gpr_declare(r, gpr_box(r, TYPE_OF(AstLabeledStmtPtr), n), (Any){NULL, NULL},
                    r->label_scope, AST_LBL, n->label);
        gpr_walk(r, n->stmt);
        break;
    }
    case AST_KIND_ASSIGN_STMT: {
        AstAssignStmt *n = (AstAssignStmt *)node;
        gpr_walk_exprs(r, n->rhs);
        if (n->tok == TOKEN_DEFINE)
            gpr_short_var_decl(r, n);
        else
            gpr_walk_exprs(r, n->lhs);
        break;
    }
    case AST_KIND_BRANCH_STMT: {
        /* add to list of unresolved targets */
        AstBranchStmt *n = (AstBranchStmt *)node;
        if (n->tok != TOKEN_FALLTHROUGH && n->label != NULL) {
            gpr_assert(r->target_marks.len > 0, "branch statement outside a body");
            gpr_append(r, &r->targets, &n->label);
        }
        break;
    }
    case AST_KIND_BLOCK_STMT:
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_stmts(r, ((AstBlockStmt *)node)->list);
        break;
    case AST_KIND_IF_STMT: {
        AstIfStmt *n = (AstIfStmt *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        if (n->init != NULL)
            gpr_walk(r, n->init);
        gpr_walk(r, n->cond);
        gpr_walk(r, &n->body->node);
        if (n->else_ != NULL)
            gpr_walk(r, n->else_);
        break;
    }
    case AST_KIND_CASE_CLAUSE: {
        AstCaseClause *n = (AstCaseClause *)node;
        gpr_walk_exprs(r, n->list);
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk_stmts(r, n->body);
        break;
    }
    case AST_KIND_SWITCH_STMT: {
        AstSwitchStmt *n = (AstSwitchStmt *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        if (n->init != NULL)
            gpr_walk(r, n->init);
        if (n->tag != NULL) {
            /* The scope below reproduces some unnecessary behavior of the
             * parser, opening an extra scope in case this is a type switch.
             * It's not needed for expression switches.
             * TODO: remove this once we've matched the parser resolution. */
            if (n->init != NULL) {
                gpr_open_scope(r, ast_expr_pos(n->tag));
                scopes++;
            }
            gpr_walk(r, n->tag);
        }
        if (n->body != NULL)
            gpr_walk_stmts(r, n->body->list);
        break;
    }
    case AST_KIND_TYPE_SWITCH_STMT: {
        AstTypeSwitchStmt *n = (AstTypeSwitchStmt *)node;
        if (n->init != NULL) {
            gpr_open_scope(r, ast_node_pos(node));
            scopes++;
            gpr_walk(r, n->init);
        }
        gpr_open_scope(r, ast_stmt_pos(n->assign));
        scopes++;
        gpr_walk(r, n->assign);
        /* s.Body consists only of case clauses, so does not get its own
         * scope. */
        if (n->body != NULL)
            gpr_walk_stmts(r, n->body->list);
        break;
    }
    case AST_KIND_COMM_CLAUSE: {
        AstCommClause *n = (AstCommClause *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        if (n->comm != NULL)
            gpr_walk(r, n->comm);
        gpr_walk_stmts(r, n->body);
        break;
    }
    case AST_KIND_SELECT_STMT: {
        /* as for switch statements, select statement bodies don't get their
         * own scope. */
        AstSelectStmt *n = (AstSelectStmt *)node;
        if (n->body != NULL)
            gpr_walk_stmts(r, n->body->list);
        break;
    }
    case AST_KIND_FOR_STMT: {
        AstForStmt *n = (AstForStmt *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        if (n->init != NULL)
            gpr_walk(r, n->init);
        if (n->cond != NULL)
            gpr_walk(r, n->cond);
        if (n->post != NULL)
            gpr_walk(r, n->post);
        gpr_walk(r, &n->body->node);
        break;
    }
    case AST_KIND_RANGE_STMT: {
        AstRangeStmt *n = (AstRangeStmt *)node;
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;
        gpr_walk(r, n->x);
        Slice lhs = slice_nil(TYPE_AST_EXPR);
        if (n->key != NULL)
            gpr_append(r, &lhs, &n->key);
        if (n->value != NULL)
            gpr_append(r, &lhs, &n->value);
        if (lhs.len > 0) {
            if (n->tok == TOKEN_DEFINE) {
                /* Note: we can't exactly match the behavior of object
                 * resolution here, as it treats the rhs of the range as
                 * declared in the scope of the assignment. */
                AstUnaryExpr *rng =
                    (AstUnaryExpr *)ast_node_new(r->a, AST_KIND_UNARY_EXPR);
                AstAssignStmt *as =
                    (AstAssignStmt *)ast_node_new(r->a, AST_KIND_ASSIGN_STMT);
                Slice rhs = slice_nil(TYPE_AST_EXPR);
                if (rng == NULL || as == NULL)
                    burrow__parser_oom(r->p);
                rng->op = TOKEN_RANGE;
                rng->x = n->x;
                AstExpr rx = &rng->node;
                gpr_append(r, &rhs, &rx);
                as->lhs = lhs;
                as->tok = TOKEN_DEFINE;
                as->tok_pos = n->tok_pos;
                as->rhs = rhs;
                /* TODO(rFindley): walkLHS reproduced the parser resolution,
                 * but is it necessary? By comparison, for a normal
                 * AssignStmt we don't walk the LHS in case there is an
                 * invalid identifier list. */
                gpr_walk_lhs(r, lhs);
                gpr_short_var_decl(r, as);
            } else {
                gpr_walk_exprs(r, lhs);
            }
        }
        gpr_walk(r, &n->body->node);
        break;
    }

    /* Declarations */
    case AST_KIND_GEN_DECL: {
        AstGenDecl *n = (AstGenDecl *)node;
        switch (n->tok) {
        case TOKEN_CONST:
        case TOKEN_VAR:
            for (Int i = 0; i < n->specs.len; i++) {
                AstValueSpec *spec = (AstValueSpec *)BURROW_AT(AstSpec, n->specs, i);
                AstObjKind kind = n->tok == TOKEN_VAR ? AST_VAR : AST_CON;
                gpr_walk_exprs(r, spec->values);
                if (spec->type != NULL)
                    gpr_walk(r, spec->type);
                if (spec->names.len == 0)
                    continue;
                Any decl = gpr_box(r, TYPE_OF(AstValueSpecPtr), spec);
                Any data = any_box(r->a, BURROW_ANY(TYPE_INT, &i));
                if (data.t == NULL)
                    burrow__parser_oom(r->p);
                gpr_declare_all(r, decl, data, r->top_scope, kind, spec->names);
            }
            break;
        case TOKEN_TYPE_:
            for (Int i = 0; i < n->specs.len; i++) {
                AstTypeSpec *spec = (AstTypeSpec *)BURROW_AT(AstSpec, n->specs, i);
                /* Go spec: The scope of a type identifier declared inside a
                 * function begins at the identifier in the TypeSpec and ends
                 * at the end of the innermost containing block. */
                gpr_declare(r, gpr_box(r, TYPE_OF(AstTypeSpecPtr), spec),
                            (Any){NULL, NULL}, r->top_scope, AST_TYP, spec->name);
                if (spec->type_params != NULL) {
                    gpr_open_scope(r, ast_node_pos(&spec->node));
                    scopes++;
                    gpr_walk_tparams(r, spec->type_params);
                }
                gpr_walk(r, spec->type);
            }
            break;
        default:
            break;
        }
        break;
    }
    case AST_KIND_FUNC_DECL: {
        AstFuncDecl *n = (AstFuncDecl *)node;
        /* Open the function scope. */
        gpr_open_scope(r, ast_node_pos(node));
        scopes++;

        gpr_walk_recv(r, n->recv);

        /* Type parameters are walked normally: they can reference each other,
         * and can be referenced by normal parameters. */
        if (n->type->type_params != NULL)
            gpr_walk_tparams(r, n->type->type_params);

        gpr_resolve_list(r, n->type->params);
        gpr_resolve_list(r, n->type->results);
        gpr_declare_list(r, n->recv, AST_VAR);
        gpr_declare_list(r, n->type->params, AST_VAR);
        gpr_declare_list(r, n->type->results, AST_VAR);

        gpr_walk_body(r, n->body);
        if (n->recv == NULL && !str_eq(n->name->name, BURROW_S("init")))
            gpr_declare(r, gpr_box(r, TYPE_OF(AstFuncDeclPtr), n), (Any){NULL, NULL},
                        r->pkg_scope, AST_FUN, n->name);
        break;
    }
    default:
        return false;
    }
    gpr_close_scopes(r, scopes);
    return true;
}

/* Visit. Go's stack grows to fit the tree and a C one does not, so a walk
 * that would run the stack short stops with the parser's "exceeded max nesting
 * depth". nest counts the nodes being walked, and a node whose children the
 * walk goes into is done when the walk calls back with NULL. */
static AstVisitor gpr_visit(void *self, AstNode node) {
    Gpr *r = (Gpr *)self;
    if (node == NULL) {
        r->nest--;
        return (AstVisitor){NULL, NULL};
    }
    r->nest++;
    if (burrow__parser_stack_low(r->p, r->nest))
        burrow__parser_bail(r->p, ast_node_pos(node),
                            BURROW_S("exceeded max nesting depth"));
    if (gpr_visit_node(r, node)) {
        r->nest--;
        return (AstVisitor){NULL, NULL};
    }
    return (AstVisitor){&gpr_vt, r};
}

/* resolveFile walks the given file to resolve identifiers within the file
 * scope, updating ast.Ident.Obj fields with declaration information.
 *
 * If declErr is non-nil, it is used to report declaration errors during
 * resolution. tok is used to format position in error messages. */
void burrow__parser_resolve_file(void *p, Alloc *a, AstFile *file, TokenFile *handle,
                                 bool decl_errors) {
    Gpr r;
    memset(&r, 0, sizeof r);
    r.p = p;
    r.a = a;
    r.handle = handle;
    r.decl_errors = decl_errors;
    r.pkg_scope = ast_new_scope(a, NULL);
    r.unresolved_obj = BURROW_NEW(a, AstObject);
    if (r.pkg_scope == NULL || r.unresolved_obj == NULL)
        burrow__parser_oom(p);
    r.top_scope = r.pkg_scope;
    r.depth = 1;
    r.unresolved = slice_nil(TYPE_AST_IDENT_PTR);
    r.targets = slice_nil(TYPE_AST_IDENT_PTR);
    r.target_marks = slice_nil(TYPE_INT);

    for (Int i = 0; i < file->decls.len; i++)
        gpr_walk(&r, BURROW_AT(AstDecl, file->decls, i));

    gpr_close_scope(&r);
    gpr_assert(r.top_scope == NULL, "unbalanced scopes");
    gpr_assert(r.label_scope == NULL, "unbalanced label scopes");

    /* resolve global identifiers within the same file */
    Int i = 0;
    for (Int j = 0; j < r.unresolved.len; j++) {
        AstIdent *ident = BURROW_AT(AstIdent *, r.unresolved, j);
        /* i <= index for current ident */
        gpr_assert(ident->obj == r.unresolved_obj, "object already resolved");
        ident->obj = ast_scope_lookup(r.pkg_scope, ident->name); /* also removes
                                                                    unresolved
                                                                    sentinel */
        if (ident->obj == NULL) {
            BURROW_AT(AstIdent *, r.unresolved, i) = ident;
            i++;
        }
    }
    file->scope = r.pkg_scope;
    r.unresolved.len = i;
    file->unresolved = r.unresolved;
}
