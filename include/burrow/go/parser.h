/* go/parser, a parser for Go source files.
 *
 * Go's go/parser. It reads Go source and builds the syntax tree of
 * burrow/go/ast for it, with positions in a token.FileSet:
 *
 *     TokenFileSet *fset = token_new_file_set(a);
 *     Error err = BURROW_NO_ERROR;
 *     AstFile *f = parser_parse_file(a, fset, BURROW_S("hello.go"),
 *                                    BURROW_ANY(TYPE_STRING, &src), 0, &err);
 *
 * The source can come from a file, when src is the nil Any, or be passed as a
 * Str (TYPE_STRING), a byte Slice (TYPE_BYTES), a BytesBuffer
 * (TYPE_BYTES_BUFFER) or an IoReader (&burrow_type_IoReader). It is copied into
 * the allocator once and the strings in the tree point into that copy.
 *
 * Every node is made in the allocator passed in, and nothing frees one node
 * alone, so parse into an arena. The Error of a syntax error is a
 * GoScannerErrorList, sorted by position, and the tree that comes back with it
 * is as much of the file as could be parsed.
 *
 * Go stops at 100000 levels of nesting with "exceeded max nesting depth". The
 * same error comes here when the C stack is about to run out, since a C stack
 * does not grow the way Go's does. To parse code nested thousands deep, parse
 * it on a goroutine started with go_stack and a bigger stack.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/parser */

#ifndef BURROW_GO_PARSER_H
#define BURROW_GO_PARSER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* parser.Mode, a set of flags that control what the parser does. */
typedef Uint ParserMode;

enum {
    /* Stop after the package clause. */
    PARSER_PACKAGE_CLAUSE_ONLY = 1 << 0,
    /* Stop after the import declarations. */
    PARSER_IMPORTS_ONLY = 1 << 1,
    /* Keep the comments and put them in the tree. */
    PARSER_PARSE_COMMENTS = 1 << 2,
    /* Print a trace of the productions parsed to standard output. */
    PARSER_TRACE = 1 << 3,
    /* Report declaration errors, like a name declared twice. */
    PARSER_DECLARATION_ERRORS = 1 << 4,
    /* The same as PARSER_ALL_ERRORS, kept for compatibility. */
    PARSER_SPURIOUS_ERRORS = 1 << 5,
    /* Leave out the deprecated identifier resolution, which leaves the obj
     * of every AstIdent and the scope and unresolved of the file empty. */
    PARSER_SKIP_OBJECT_RESOLUTION = 1 << 6,
    /* Report every error, not just the first 10 on different lines. */
    PARSER_ALL_ERRORS = PARSER_SPURIOUS_ERRORS
};

/* The filter of parser_parse_dir: whether to parse the file info describes. */
BURROW_FUNC(ParserFileFilter, bool, FsFileInfo info);

/* parser.ParseFile: parses the source of one Go file and gives back its
 * AstFile. filename is the name the positions are recorded under, and the file
 * read when src is the nil Any. The file is added to fset.
 *
 * mode picks how much is parsed and what else is done. With a syntax error the
 * file is still returned, with BadExpr, BadStmt and BadDecl nodes where the
 * errors were, and *err is the GoScannerErrorList of all of them. When the
 * source cannot be read, or the package clause is bad, the result is NULL or a
 * file with just a name of "", and *err says why. */
BURROW_OWNS(ret) AstFile *parser_parse_file(Alloc *a, TokenFileSet *fset, Str filename,
                                            Any src, ParserMode mode, Error *err);

/* parser.ParseDir: parses every file in the directory path whose name ends in
 * ".go" and that filter, when it is not nil, says yes to. The result maps
 * package names (Str) to AstPackage pointers, and each package's files map
 * file names to AstFile pointers. A file with errors is left out, and *first is
 * the first error met. When path cannot be read the result is NULL.
 *
 * Deprecated in Go, since it knows nothing of build tags, but kept here. */
BURROW_OWNS(ret) Map *parser_parse_dir(Alloc *a, TokenFileSet *fset, Str path,
                                       ParserFileFilter filter, ParserMode mode,
                                       Error *first);

/* parser.ParseExprFrom: parses one expression. The arguments are as for
 * parser_parse_file, and the expression may be followed by a newline but
 * nothing else. With a syntax error the partial expression is still returned,
 * or NULL when there is none. */
BURROW_OWNS(ret) AstExpr parser_parse_expr_from(Alloc *a, TokenFileSet *fset,
                                                Str filename, Any src, ParserMode mode,
                                                Error *err);

/* parser.ParseExpr: parses the expression x, with positions in a file set of
 * its own that the caller never sees, so they are only good for ordering. */
BURROW_OWNS(ret) AstExpr parser_parse_expr(Alloc *a, Str x, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_PARSER_H */
