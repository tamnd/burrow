/* go/build/constraint, build constraint lines.
 *
 * Go's go/build/constraint. It parses the "//go:build" lines that say which
 * builds a file belongs to, and the older "// +build" lines, into a tree that
 * can be evaluated against a set of tags:
 *
 *     Error err = BURROW_NO_ERROR;
 *     ConstraintExpr x = constraint_parse(a, BURROW_S("//go:build linux && !cgo"), &err);
 *     bool ok = constraint_expr_eval(x, BURROW_FN(ConstraintTagFunc, has_tag, NULL));
 *
 * Every Expr is made in the allocator passed to the call that made it, and
 * nothing frees one Expr alone, so parse into an arena.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/build/constraint */

#ifndef BURROW_GO_BUILD_CONSTRAINT_H
#define BURROW_GO_BUILD_CONSTRAINT_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- expressions */

/* What kind of Expr a node is. */
typedef enum ConstraintKind {
    CONSTRAINT_KIND_TAG = 1,
    CONSTRAINT_KIND_NOT,
    CONSTRAINT_KIND_AND,
    CONSTRAINT_KIND_OR
} ConstraintKind;

/* The header every Expr starts with, which holds its ConstraintKind. */
typedef struct ConstraintBase {
    Int kind;
} ConstraintBase;

/* constraint.Expr: a pointer to the header of a TagExpr, NotExpr, AndExpr or
 * OrExpr. Switch on x->kind to tell them apart, where Go uses a type switch. */
typedef ConstraintBase *ConstraintExpr;

/* constraint.TagExpr, the single tag tag, like "linux" or "cgo". */
typedef struct ConstraintTagExpr {
    ConstraintBase expr;
    Str tag;
} ConstraintTagExpr;

/* constraint.NotExpr, !x. */
typedef struct ConstraintNotExpr {
    ConstraintBase expr;
    ConstraintExpr x;
} ConstraintNotExpr;

/* constraint.AndExpr, x && y. */
typedef struct ConstraintAndExpr {
    ConstraintBase expr;
    ConstraintExpr x, y;
} ConstraintAndExpr;

/* constraint.OrExpr, x || y. */
typedef struct ConstraintOrExpr {
    ConstraintBase expr;
    ConstraintExpr x, y;
} ConstraintOrExpr;

/* Go's func(tag string) bool for Eval: whether the build has tag. */
BURROW_FUNC(ConstraintTagFunc, bool, Str tag);

/* Expr.Eval: whether x is true when ok says which tags are set. Both sides of
 * an && or || are evaluated, so ok sees every tag in x. */
bool constraint_expr_eval(ConstraintExpr x, ConstraintTagFunc ok);

/* Expr.String: x in the //go:build syntax, with parentheses only where they
 * are needed, built in a. */
BURROW_OWNS(ret) Str constraint_expr_string(ConstraintExpr x, Alloc *a);

/* The Eval and String methods of each kind, which are what the two above
 * call. */
bool constraint_tag_expr_eval(ConstraintTagExpr *x, ConstraintTagFunc ok);
BURROW_BORROWS(ret, x) Str constraint_tag_expr_string(ConstraintTagExpr *x);
bool constraint_not_expr_eval(ConstraintNotExpr *x, ConstraintTagFunc ok);
BURROW_OWNS(ret) Str constraint_not_expr_string(ConstraintNotExpr *x, Alloc *a);
bool constraint_and_expr_eval(ConstraintAndExpr *x, ConstraintTagFunc ok);
BURROW_OWNS(ret) Str constraint_and_expr_string(ConstraintAndExpr *x, Alloc *a);
bool constraint_or_expr_eval(ConstraintOrExpr *x, ConstraintTagFunc ok);
BURROW_OWNS(ret) Str constraint_or_expr_string(ConstraintOrExpr *x, Alloc *a);

/* ------------------------------------------------------------------ errors */

/* constraint.SyntaxError: a //go:build expression that did not parse. offset
 * is the byte in the expression where the problem was found, and err says
 * what it was. errors_as with TYPE_CONSTRAINT_SYNTAX_ERROR gives a pointer to
 * the one inside an error from constraint_parse. */
typedef struct ConstraintSyntaxError {
    Int offset;
    Str err;
} ConstraintSyntaxError;

extern const Type *const TYPE_CONSTRAINT_SYNTAX_ERROR;

/* SyntaxError.Error, which is err. */
BURROW_BORROWS(ret, e) Str
constraint_syntax_error_error(const ConstraintSyntaxError *e);

/* ----------------------------------------------------------------- parsing */

/* Parse: the expression of a single "//go:build ..." or "// +build ..." line,
 * made in a. A line that is neither gives the error "not a build constraint",
 * a //go:build expression that does not parse gives a ConstraintSyntaxError,
 * and a // +build line with more than 100 operators gives "expression too
 * complex for // +build lines". The result is NULL whenever *err is set. */
BURROW_OWNS(ret) ConstraintExpr constraint_parse(Alloc *a, Str line, Error *err);

/* IsGoBuild: whether line is a //go:build line. Only the prefix is looked at,
 * not whether the expression parses. One trailing newline is allowed and any
 * other newline is not. */
bool constraint_is_go_build(Str line);

/* IsPlusBuild: whether line is a // +build line, the same way. */
bool constraint_is_plus_build(Str line);

/* PlusBuildLines: the // +build lines that say the same as x, made in a.
 * Negations are pushed down to the tags first, and an expression that still
 * does not fit the old syntax gives the error "expression too complex for //
 * +build lines" and a nil slice. */
BURROW_OWNS(ret) Slice constraint_plus_build_lines(Alloc *a, ConstraintExpr x,
                                                   Error *err);

/* GoVersion: the oldest Go version x allows, like "go1.22", or empty if x can
 * be true without any go1.N tag. Only the shape of x is looked at, so
 * (linux && !linux && go1.20) || go1.21 is "go1.20". A version is made in a. */
BURROW_OWNS(ret) Str constraint_go_version(Alloc *a, ConstraintExpr x);

#ifdef __cplusplus
}
#endif

#endif
