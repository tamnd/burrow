/* go/format, standard formatting of Go source.
 *
 * Go's go/format. format_source takes Go source, a whole file or a run of
 * declarations or statements, and gives it back laid out the way gofmt lays it
 * out:
 *
 *     Error err = BURROW_NO_ERROR;
 *     Slice out = format_source(a, src, &err);
 *     if (BURROW_FAILED(err))
 *         return err; // a syntax error, with positions in src
 *
 * format_node does the same for a tree that is already parsed, or built by
 * hand, and writes it to an IoWriter. The node goes in as an Any, as for
 * printer_fprint.
 *
 * Formatting changes over time, as it does in Go, so output from one version
 * of the library is not promised to match another's byte for byte.
 *
 * Memory. format_source parses into a and leaves the tree there along with the
 * result, so an arena is the easy thing to give it. format_node only allocates
 * in a when the file it is given has imports to sort, since it sorts a copy.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/format */

#ifndef BURROW_GO_FORMAT_H
#define BURROW_GO_FORMAT_H

#include "burrow/core.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* format.Node: node in gofmt style, written to dst. node is an AstFile
 * pointer, a PrinterCommentedNode pointer, a Slice of AstDecl or of AstStmt,
 * or any expression, statement, declaration or spec, as for printer_fprint.
 * Positions are looked up in fset, which has to be the set node was parsed
 * with.
 *
 * The tree is not changed. A file with grouped imports is printed, parsed
 * again into fset and a, and its imports sorted in the copy, which is what
 * gets written. Output can stop part way when an error comes back. */
BURROW_BORROWS(ret) Error format_node(Alloc *a, IoWriter dst, TokenFileSet *fset,
                                      Any node);

/* format.Source: src in gofmt style. src is a whole Go file, or a list of
 * declarations or statements, in which case the leading and trailing space
 * and the indentation of the first line are kept as they were, and src that
 * is only space comes back as it was.
 *
 * With a syntax error the result is a nil slice and *err is the
 * GoScannerErrorList go/parser gave back. The result is a new slice in a in
 * every case where it is not nil. */
BURROW_OWNS(ret) Slice format_source(Alloc *a, Slice src, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_FORMAT_H */
