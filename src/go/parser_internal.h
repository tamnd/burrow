/* go/parser internals shared by parser.c and parser_resolver.c: the hooks the
 * resolver calls back into the parser with, and the resolver itself. Each
 * takes the parser as a void pointer.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_PARSER_INTERNAL_H
#define BURROW_GO_PARSER_INTERNAL_H

#include "burrow/go/ast.h"
#include "burrow/go/token.h"

/* Gives up on the file with msg at pos, as the parser's bailout does. */
BURROW_NORETURN void burrow__parser_bail(void *p, TokenPos pos, Str msg);
/* Gives up on the file for a failed allocation. */
BURROW_NORETURN void burrow__parser_oom(void *p);
/* Reports an error at pos and goes on. */
void burrow__parser_error(void *p, TokenPos pos, Str msg);
/* Whether the stack is too low to go depth levels deeper. */
bool burrow__parser_stack_low(void *p, Int depth);
/* resolveFile: resolves the identifiers of file. */
void burrow__parser_resolve_file(void *p, Alloc *a, AstFile *file, TokenFile *handle,
                                 bool decl_errors);

#endif /* BURROW_GO_PARSER_INTERNAL_H */
