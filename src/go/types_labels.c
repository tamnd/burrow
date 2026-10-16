/* go/types: checking of label use.
 *
 * Derived from Go's src/go/types/labels.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* A block tracks label declarations in a block and its enclosing blocks. */
typedef struct TlbBlock {
    struct TlbBlock *parent; /* enclosing block */
    AstLabeledStmt *lstmt;   /* labeled statement to which this block belongs, or nil */
    Map *labels;             /* Str to AstLabeledStmt *, allocated lazily */
} TlbBlock;

static Slice tlb_block_branches(TypesChecker *check, TypesScope *all, TlbBlock *parent,
                                AstLabeledStmt *lstmt, Slice list);

/* labels checks correct label use in body. */
void burrow__tc_labels(TypesChecker *check, AstBlockStmt *body) {
    /* set of all labels in this body */
    TypesScope *all = types_new_scope(check->a, NULL, ast_node_pos(GT_NODE(body)),
                                      ast_node_end(GT_NODE(body)), BURROW_S("label"));

    Slice fwd_jumps = tlb_block_branches(check, all, NULL, NULL, body->list);

    /* If there are any forward jumps left, no label was found for the
     * corresponding goto statements. Either those labels were never defined, or
     * they are inside blocks and not reachable for the respective gotos. */
    for (Int i = 0; i < fwd_jumps.len; i++) {
        AstBranchStmt *jmp = GT_AT(fwd_jumps, i, AstBranchStmt *);
        Str name = jmp->label->name;
        TypesObject alt = types_scope_lookup(all, name);
        if (alt != NULL) {
            GT_ERRORF(check, GT_AT_NODE(jmp->label), GT_CODE_JUMP_INTO_BLOCK,
                      "goto %s jumps into block", name);
            GT_KNOWN(TypesLabel, alt)->used = true; /* avoid another error */
        } else {
            GT_ERRORF(check, GT_AT_NODE(jmp->label), GT_CODE_UNDECLARED_LABEL,
                      "label %s not declared", name);
        }
    }

    /* spec: "It is illegal to define a label that is never used." */
    Slice names = types_scope_names(check->a, all);
    for (Int i = 0; i < names.len; i++) {
        TypesLabel *lbl = types_as_label(types_scope_lookup(all, GT_AT(names, i, Str)));
        if (lbl != NULL && !lbl->used) {
            Str name = GT_OBJ(lbl)->name;
            GT_SOFT_ERRORF(check, burrow__tc_at_obj(types_object(lbl)),
                           GT_CODE_UNUSED_LABEL, "label %s declared and not used",
                           name);
        }
    }
}

/* insert records a new label declaration for the current block. The label
 * must not have been declared before in any block. */
static void tlb_insert(Alloc *a, TlbBlock *b, AstLabeledStmt *s) {
    Str name = s->label->name;
    if (b->labels == NULL) {
        b->labels = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (b->labels == NULL)
            burrow__types_oom();
    }
    if (!map_set(b->labels, &name, &s))
        burrow__types_oom();
}

/* gotoTarget returns the labeled statement in the current or an enclosing
 * block with the given label name, or nil. */
static AstLabeledStmt *tlb_goto_target(TlbBlock *b, Str name) {
    for (TlbBlock *s = b; s != NULL; s = s->parent) {
        if (s->labels == NULL)
            continue;
        AstLabeledStmt *const *t = map_get(s->labels, &name);
        if (t != NULL && *t != NULL)
            return *t;
    }
    return NULL;
}

/* enclosingTarget returns the innermost enclosing labeled statement with the
 * given label name, or nil. */
static AstLabeledStmt *tlb_enclosing_target(TlbBlock *b, Str name) {
    for (TlbBlock *s = b; s != NULL; s = s->parent) {
        AstLabeledStmt *t = s->lstmt;
        if (t != NULL && str_eq(t->label->name, name))
            return t;
    }
    return NULL;
}

/* What blockBranches' closures share. */
typedef struct TlbBranches {
    TypesChecker *check;
    TypesScope *all;
    TlbBlock *b;
    TokenPos var_decl_pos;
    Slice fwd_jumps; /* of AstBranchStmt * */
    Slice bad_jumps; /* of AstBranchStmt * */
} TlbBranches;

/* All forward jumps jumping over a variable declaration are possibly invalid
 * (they may still jump out of the block and be ok). recordVarDecl records them
 * for the given position. */
static void tlb_record_var_decl(TlbBranches *bb, TokenPos pos) {
    bb->var_decl_pos = pos;
    /* copy fwdJumps to badJumps */
    bb->bad_jumps.len = 0;
    for (Int i = 0; i < bb->fwd_jumps.len; i++)
        burrow__types_ptrs_append(bb->check->a, &bb->bad_jumps,
                                  GT_AT(bb->fwd_jumps, i, AstBranchStmt *));
}

static bool tlb_jumps_over_var_decl(const TlbBranches *bb, AstBranchStmt *jmp) {
    if (!token_pos_is_valid(bb->var_decl_pos))
        return false;
    for (Int i = 0; i < bb->bad_jumps.len; i++) {
        if (GT_AT(bb->bad_jumps, i, AstBranchStmt *) == jmp)
            return true;
    }
    return false;
}

static void tlb_nested_branches(TlbBranches *bb, AstLabeledStmt *lstmt, Slice list) {
    /* Unresolved forward jumps inside the nested block become forward jumps in
     * the current block. */
    Slice fwd = tlb_block_branches(bb->check, bb->all, bb->b, lstmt, list);
    for (Int i = 0; i < fwd.len; i++)
        burrow__types_ptrs_append(bb->check->a, &bb->fwd_jumps,
                                  GT_AT(fwd, i, AstBranchStmt *));
}

static void tlb_labeled_stmt(TlbBranches *bb, AstLabeledStmt *s,
                             AstLabeledStmt **lstmt) {
    TypesChecker *check = bb->check;
    /* declare non-blank label */
    Str name = s->label->name;
    if (str_eq(name, BURROW_S("_")))
        return;
    TypesLabel *lbl =
        types_new_label(check->a, ast_node_pos(GT_NODE(s->label)), check->pkg, name);
    TypesObject alt = types_scope_insert(bb->all, types_object(lbl));
    if (alt != NULL) {
        GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_LABEL);
        err->soft = true;
        GT_ADDF(err, burrow__tc_at_obj(types_object(lbl)), "label %s already declared",
                name);
        burrow__tc_error_add_alt_decl(err, alt);
        burrow__tc_error_report(err);
        /* ok to continue */
    } else {
        tlb_insert(check->a, bb->b, s);
        burrow__tc_record_def(check, s->label, types_object(lbl));
    }
    /* resolve matching forward jumps and remove them from fwdJumps */
    Int i = 0;
    for (Int j = 0; j < bb->fwd_jumps.len; j++) {
        AstBranchStmt *jmp = GT_AT(bb->fwd_jumps, j, AstBranchStmt *);
        if (str_eq(jmp->label->name, name)) {
            /* match */
            lbl->used = true;
            burrow__tc_record_use(check, jmp->label, types_object(lbl));
            if (tlb_jumps_over_var_decl(bb, jmp)) {
                Int line = 0;
                if (check->fset != NULL)
                    line = token_file_set_position(check->fset, bb->var_decl_pos).line;
                GT_SOFT_ERRORF(check, GT_AT_NODE(jmp->label), GT_CODE_JUMP_OVER_DECL,
                               "goto %s jumps over variable declaration at line %d",
                               name, line);
                /* ok to continue */
            }
        } else {
            /* no match - record new forward jump */
            GT_AT(bb->fwd_jumps, i, AstBranchStmt *) = jmp;
            i++;
        }
    }
    bb->fwd_jumps.len = i;
    *lstmt = s;
}

static void tlb_branch_stmt(TlbBranches *bb, AstBranchStmt *s) {
    TypesChecker *check = bb->check;
    if (s->label == NULL)
        return; /* checked in 1st pass (check.stmt) */

    /* determine and validate target */
    Str name = s->label->name;
    Token tok = s->tok;
    switch (tok) {
    case TOKEN_BREAK: {
        /* spec: "If there is a label, it must be that of an enclosing "for",
         * "switch", or "select" statement, and that is the one whose execution
         * terminates." */
        bool valid = false;
        AstLabeledStmt *t = tlb_enclosing_target(bb->b, name);
        if (t != NULL) {
            Int kind = t->stmt->kind;
            valid = kind == AST_KIND_SWITCH_STMT || kind == AST_KIND_TYPE_SWITCH_STMT ||
                    kind == AST_KIND_SELECT_STMT || kind == AST_KIND_FOR_STMT ||
                    kind == AST_KIND_RANGE_STMT;
        }
        if (!valid) {
            GT_ERRORF(check, GT_AT_NODE(s->label), GT_CODE_MISPLACED_LABEL,
                      "invalid break label %s", name);
            return;
        }
        break;
    }

    case TOKEN_CONTINUE: {
        /* spec: "If there is a label, it must be that of an enclosing "for"
         * statement, and that is the one whose execution advances." */
        bool valid = false;
        AstLabeledStmt *t = tlb_enclosing_target(bb->b, name);
        if (t != NULL) {
            Int kind = t->stmt->kind;
            valid = kind == AST_KIND_FOR_STMT || kind == AST_KIND_RANGE_STMT;
        }
        if (!valid) {
            GT_ERRORF(check, GT_AT_NODE(s->label), GT_CODE_MISPLACED_LABEL,
                      "invalid continue label %s", name);
            return;
        }
        break;
    }

    case TOKEN_GOTO:
        if (tlb_goto_target(bb->b, name) == NULL) {
            /* label may be declared later - add branch to forward jumps */
            burrow__types_ptrs_append(check->a, &bb->fwd_jumps, s);
            return;
        }
        break;

    default: {
        Str ts = token_string(tok, check->a);
        GT_ERRORF(check, GT_AT_NODE(s), GT_CODE_INVALID_SYNTAX_TREE,
                  "branch statement: %s %s", ts, name);
        return;
    }
    }

    /* record label use */
    TypesObject obj = types_scope_lookup(bb->all, name);
    GT_KNOWN(TypesLabel, obj)->used = true;
    burrow__tc_record_use(check, s->label, obj);
}

static void tlb_stmt_branches(TlbBranches *bb, AstLabeledStmt *lstmt, AstStmt s) {
    Int kind = s->kind;
    switch (kind) {
    case AST_KIND_DECL_STMT: {
        AstDecl d = ((AstDeclStmt *)(void *)s)->decl;
        if (d != NULL && d->kind == AST_KIND_GEN_DECL &&
            ((AstGenDecl *)(void *)d)->tok == TOKEN_VAR)
            tlb_record_var_decl(bb, ast_node_pos(d));
        break;
    }

    case AST_KIND_LABELED_STMT: {
        AstLabeledStmt *ls = (AstLabeledStmt *)(void *)s;
        tlb_labeled_stmt(bb, ls, &lstmt);
        tlb_stmt_branches(bb, lstmt, ls->stmt);
        break;
    }

    case AST_KIND_BRANCH_STMT:
        tlb_branch_stmt(bb, (AstBranchStmt *)(void *)s);
        break;

    case AST_KIND_ASSIGN_STMT:
        if (((AstAssignStmt *)(void *)s)->tok == TOKEN_DEFINE)
            tlb_record_var_decl(bb, ast_node_pos(s));
        break;

    case AST_KIND_BLOCK_STMT:
        tlb_nested_branches(bb, lstmt, ((AstBlockStmt *)(void *)s)->list);
        break;

    case AST_KIND_IF_STMT: {
        AstIfStmt *is = (AstIfStmt *)(void *)s;
        tlb_stmt_branches(bb, lstmt, GT_NODE(is->body));
        if (is->else_ != NULL)
            tlb_stmt_branches(bb, lstmt, is->else_);
        break;
    }

    case AST_KIND_CASE_CLAUSE:
        tlb_nested_branches(bb, NULL, ((AstCaseClause *)(void *)s)->body);
        break;

    case AST_KIND_SWITCH_STMT:
        tlb_stmt_branches(bb, lstmt, GT_NODE(((AstSwitchStmt *)(void *)s)->body));
        break;

    case AST_KIND_TYPE_SWITCH_STMT:
        tlb_stmt_branches(bb, lstmt, GT_NODE(((AstTypeSwitchStmt *)(void *)s)->body));
        break;

    case AST_KIND_COMM_CLAUSE:
        tlb_nested_branches(bb, NULL, ((AstCommClause *)(void *)s)->body);
        break;

    case AST_KIND_SELECT_STMT:
        tlb_stmt_branches(bb, lstmt, GT_NODE(((AstSelectStmt *)(void *)s)->body));
        break;

    case AST_KIND_FOR_STMT:
        tlb_stmt_branches(bb, lstmt, GT_NODE(((AstForStmt *)(void *)s)->body));
        break;

    case AST_KIND_RANGE_STMT:
        tlb_stmt_branches(bb, lstmt, GT_NODE(((AstRangeStmt *)(void *)s)->body));
        break;

    default:
        break;
    }
}

/* blockBranches processes a block's statement list and returns the set of
 * outgoing forward jumps. all is the scope of all declared labels, parent the
 * set of labels declared in the immediately enclosing block, and lstmt is the
 * labeled statement this block is associated with (or nil). */
static Slice tlb_block_branches(TypesChecker *check, TypesScope *all, TlbBlock *parent,
                                AstLabeledStmt *lstmt, Slice list) {
    TlbBlock b = {parent, lstmt, NULL};
    TlbBranches bb = {check,
                      all,
                      &b,
                      TOKEN_NO_POS,
                      slice_nil(TYPE_UNSAFE_POINTER),
                      slice_nil(TYPE_UNSAFE_POINTER)};

    for (Int i = 0; i < list.len; i++)
        tlb_stmt_branches(&bb, NULL, GT_AT(list, i, AstStmt));

    return bb.fwd_jumps;
}
