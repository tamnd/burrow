/* go/types: isTerminating.
 *
 * Derived from Go's src/go/types/return.go. Go source: go1.27.1.
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
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

static bool trt_has_break(AstStmt s, Str label, bool implicit);
static bool trt_has_break_list(Slice list, Str label, bool implicit);
static bool trt_is_terminating_list(TypesChecker *check, Slice list, Str label);
static bool trt_is_terminating_switch(TypesChecker *check, AstBlockStmt *body,
                                      Str label);

static bool trt_is_panic(TypesChecker *check, AstExpr x) {
    AstExpr u = ast_unparen(x);
    if (u == NULL || u->kind != AST_KIND_CALL_EXPR || check->env.is_panic == NULL)
        return false;
    AstCallExpr *call = (AstCallExpr *)(void *)u;
    const bool *yes = map_get(check->env.is_panic, &call);
    return yes != NULL && *yes;
}

/* isTerminating reports if s is a terminating statement. If s is labeled,
 * label is the label name; otherwise s is "". */
bool burrow__tc_is_terminating(TypesChecker *check, AstStmt s, Str label) {
    Int kind = s->kind;
    switch (kind) {
    default:
        panic_str(BURROW_S("unreachable"));

    case AST_KIND_BAD_STMT:
    case AST_KIND_DECL_STMT:
    case AST_KIND_EMPTY_STMT:
    case AST_KIND_SEND_STMT:
    case AST_KIND_INC_DEC_STMT:
    case AST_KIND_ASSIGN_STMT:
    case AST_KIND_GO_STMT:
    case AST_KIND_DEFER_STMT:
    case AST_KIND_RANGE_STMT:
        /* no chance */
        break;

    case AST_KIND_LABELED_STMT: {
        AstLabeledStmt *ls = (AstLabeledStmt *)(void *)s;
        return burrow__tc_is_terminating(check, ls->stmt, ls->label->name);
    }

    case AST_KIND_EXPR_STMT:
        /* calling the predeclared (possibly parenthesized) panic() function is
         * terminating */
        if (trt_is_panic(check, ((AstExprStmt *)(void *)s)->x))
            return true;
        break;

    case AST_KIND_RETURN_STMT:
        return true;

    case AST_KIND_BRANCH_STMT: {
        Token tok = ((AstBranchStmt *)(void *)s)->tok;
        if (tok == TOKEN_GOTO || tok == TOKEN_FALLTHROUGH)
            return true;
        break;
    }

    case AST_KIND_BLOCK_STMT:
        return trt_is_terminating_list(check, ((AstBlockStmt *)(void *)s)->list,
                                       BURROW_STR_EMPTY);

    case AST_KIND_IF_STMT: {
        AstIfStmt *is = (AstIfStmt *)(void *)s;
        if (is->else_ != NULL &&
            burrow__tc_is_terminating(check, GT_NODE(is->body), BURROW_STR_EMPTY) &&
            burrow__tc_is_terminating(check, is->else_, BURROW_STR_EMPTY))
            return true;
        break;
    }

    case AST_KIND_SWITCH_STMT:
        return trt_is_terminating_switch(check, ((AstSwitchStmt *)(void *)s)->body,
                                         label);

    case AST_KIND_TYPE_SWITCH_STMT:
        return trt_is_terminating_switch(check, ((AstTypeSwitchStmt *)(void *)s)->body,
                                         label);

    case AST_KIND_SELECT_STMT: {
        Slice list = ((AstSelectStmt *)(void *)s)->body->list;
        for (Int i = 0; i < list.len; i++) {
            AstStmt c = GT_AT(list, i, AstStmt);
            burrow__types_assert(c->kind == AST_KIND_COMM_CLAUSE);
            AstCommClause *cc = (AstCommClause *)(void *)c;
            if (!trt_is_terminating_list(check, cc->body, BURROW_STR_EMPTY) ||
                trt_has_break_list(cc->body, label, true))
                return false;
        }
        return true;
    }

    case AST_KIND_FOR_STMT: {
        AstForStmt *fs = (AstForStmt *)(void *)s;
        if (fs->cond == NULL && !trt_has_break(GT_NODE(fs->body), label, true))
            return true;
        break;
    }
    }

    return false;
}

static bool trt_is_terminating_list(TypesChecker *check, Slice list, Str label) {
    /* trailing empty statements are permitted - skip them */
    for (Int i = list.len - 1; i >= 0; i--) {
        AstStmt s = GT_AT(list, i, AstStmt);
        if (s->kind != AST_KIND_EMPTY_STMT)
            return burrow__tc_is_terminating(check, s, label);
    }
    return false; /* all statements are empty */
}

static bool trt_is_terminating_switch(TypesChecker *check, AstBlockStmt *body,
                                      Str label) {
    bool has_default = false;
    for (Int i = 0; i < body->list.len; i++) {
        AstStmt s = GT_AT(body->list, i, AstStmt);
        burrow__types_assert(s->kind == AST_KIND_CASE_CLAUSE);
        AstCaseClause *cc = (AstCaseClause *)(void *)s;
        if (cc->list.p == NULL)
            has_default = true;
        if (!trt_is_terminating_list(check, cc->body, BURROW_STR_EMPTY) ||
            trt_has_break_list(cc->body, label, true))
            return false;
    }
    return has_default;
}

/* TODO(gri) For nested breakable statements, the current implementation of
 * hasBreak will traverse the same subtree repeatedly, once for each label.
 * Replace with a single-pass label/break matching phase. */

/* hasBreak reports if s is or contains a break statement referring to the
 * label-ed statement or implicit-ly the closest outer breakable statement. */
static bool trt_has_break(AstStmt s, Str label, bool implicit) {
    Int kind = s->kind;
    switch (kind) {
    default:
        panic_str(BURROW_S("unreachable"));

    case AST_KIND_BAD_STMT:
    case AST_KIND_DECL_STMT:
    case AST_KIND_EMPTY_STMT:
    case AST_KIND_EXPR_STMT:
    case AST_KIND_SEND_STMT:
    case AST_KIND_INC_DEC_STMT:
    case AST_KIND_ASSIGN_STMT:
    case AST_KIND_GO_STMT:
    case AST_KIND_DEFER_STMT:
    case AST_KIND_RETURN_STMT:
        /* no chance */
        break;

    case AST_KIND_LABELED_STMT:
        return trt_has_break(((AstLabeledStmt *)(void *)s)->stmt, label, implicit);

    case AST_KIND_BRANCH_STMT: {
        AstBranchStmt *bs = (AstBranchStmt *)(void *)s;
        if (bs->tok == TOKEN_BREAK) {
            if (bs->label == NULL)
                return implicit;
            if (str_eq(bs->label->name, label))
                return true;
        }
        break;
    }

    case AST_KIND_BLOCK_STMT:
        return trt_has_break_list(((AstBlockStmt *)(void *)s)->list, label, implicit);

    case AST_KIND_IF_STMT: {
        AstIfStmt *is = (AstIfStmt *)(void *)s;
        if (trt_has_break(GT_NODE(is->body), label, implicit) ||
            (is->else_ != NULL && trt_has_break(is->else_, label, implicit)))
            return true;
        break;
    }

    case AST_KIND_CASE_CLAUSE:
        return trt_has_break_list(((AstCaseClause *)(void *)s)->body, label, implicit);

    case AST_KIND_SWITCH_STMT:
        if (label.len != 0 &&
            trt_has_break(GT_NODE(((AstSwitchStmt *)(void *)s)->body), label, false))
            return true;
        break;

    case AST_KIND_TYPE_SWITCH_STMT:
        if (label.len != 0 &&
            trt_has_break(GT_NODE(((AstTypeSwitchStmt *)(void *)s)->body), label,
                          false))
            return true;
        break;

    case AST_KIND_COMM_CLAUSE:
        return trt_has_break_list(((AstCommClause *)(void *)s)->body, label, implicit);

    case AST_KIND_SELECT_STMT:
        if (label.len != 0 &&
            trt_has_break(GT_NODE(((AstSelectStmt *)(void *)s)->body), label, false))
            return true;
        break;

    case AST_KIND_FOR_STMT:
        if (label.len != 0 &&
            trt_has_break(GT_NODE(((AstForStmt *)(void *)s)->body), label, false))
            return true;
        break;

    case AST_KIND_RANGE_STMT:
        if (label.len != 0 &&
            trt_has_break(GT_NODE(((AstRangeStmt *)(void *)s)->body), label, false))
            return true;
        break;
    }

    return false;
}

static bool trt_has_break_list(Slice list, Str label, bool implicit) {
    for (Int i = 0; i < list.len; i++) {
        if (trt_has_break(GT_AT(list, i, AstStmt), label, implicit))
            return true;
    }
    return false;
}
