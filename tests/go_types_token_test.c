/* Derived from Go's src/go/types/token_test.go. Go source: go1.27.1.
 *
 * This file checks invariants of token.Token ordering that we rely on since
 * package go/token doesn't provide any guarantees at the moment.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/types_check.h"
#include "burrow/burrow.h"
#include "burrow/go/token.h"
#include "burrow/mem/arena.h"

#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const struct {
    Token op, want;
} assign_ops[] = {
    {TOKEN_ADD_ASSIGN, TOKEN_ADD},         {TOKEN_SUB_ASSIGN, TOKEN_SUB},
    {TOKEN_MUL_ASSIGN, TOKEN_MUL},         {TOKEN_QUO_ASSIGN, TOKEN_QUO},
    {TOKEN_REM_ASSIGN, TOKEN_REM},         {TOKEN_AND_ASSIGN, TOKEN_AND},
    {TOKEN_OR_ASSIGN, TOKEN_OR},           {TOKEN_XOR_ASSIGN, TOKEN_XOR},
    {TOKEN_SHL_ASSIGN, TOKEN_SHL},         {TOKEN_SHR_ASSIGN, TOKEN_SHR},
    {TOKEN_AND_NOT_ASSIGN, TOKEN_AND_NOT},
};

static Token assign_ops_get(Token tok) {
    for (Int i = 0; i < NELEM(assign_ops); i++)
        if (assign_ops[i].op == tok)
            return assign_ops[i].want;
    return TOKEN_ILLEGAL;
}

static void TestZeroTok(TestingT *t) {
    /* zero value for token.Token must be token.ILLEGAL */
    Token zero = 0;
    if (TOKEN_ILLEGAL != zero) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        testing_t_errorf_v(t, "%s == %d; want 0",
                           token_string(TOKEN_ILLEGAL, arena_allocator(&ar)),
                           (Int)zero);
        arena_free(&ar);
    }
}

static void TestAssignOp(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* there are fewer than 256 tokens */
    for (int i = 0; i < 256; i++) {
        Token tok = (Token)i;
        Token got = burrow__tc_assign_op(tok);
        Token want = assign_ops_get(tok);
        if (got != want)
            testing_t_errorf_v(t, "for assignOp(%s): got %s; want %s",
                               token_string(tok, a), token_string(got, a),
                               token_string(want, a));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestZeroTok)                                                                     \
    X(TestAssignOp)

TESTING_MAIN(TESTS)
