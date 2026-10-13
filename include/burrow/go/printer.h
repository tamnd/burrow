/* go/printer, printing of Go syntax trees.
 *
 * Go's go/printer. printer_fprint writes a tree from go/parser, or one built by
 * hand, back out as Go source, in the layout gofmt uses for it:
 *
 *     TokenFileSet *fset = token_new_file_set(a);
 *     AstFile *f = parser_parse_file(a, fset, BURROW_S("hello.go"), src,
 *                                    PARSER_PARSE_COMMENTS, &err);
 *     StringsBuilder b = STRINGS_BUILDER(a);
 *     err = printer_fprint(a, strings_builder_as_io_writer(&b), fset,
 *                          BURROW_ANY(TYPE_AST_FILE_PTR, &f));
 *
 * The node goes in as an Any, as Go's goes in as an any. It can be an AstFile
 * pointer, a PrinterCommentedNode pointer, a Slice of AstDecl or of AstStmt
 * (TYPE_AST_DECL_SLICE and TYPE_AST_STMT_SLICE), or any expression, statement,
 * declaration or spec, given either by its own pointer type, such as
 * TYPE_OF(AstIdentPtr), or by one of the interface ones, such as
 * TYPE_AST_EXPR. The Any points at the pointer, not at the node.
 *
 * Positions are looked up in fset, and a node whose positions are not in it is
 * still printed, only without the line breaks the source had.
 *
 * Memory. The printer works in an arena of its own that it takes from a, and
 * gives all of it back before it returns. Nothing it returns lives in a.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/printer */

#ifndef BURROW_GO_PRINTER_H
#define BURROW_GO_PRINTER_H

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* printer.Mode, a set of flags that control the printing. */
typedef Uint PrinterMode;

enum {
    /* Do not use a tabwriter. PRINTER_USE_SPACES is ignored when this is set. */
    PRINTER_RAW_FORMAT = 1 << 0,
    /* Use tabs for indentation, whatever PRINTER_USE_SPACES says. */
    PRINTER_TAB_INDENT = 1 << 1,
    /* Use spaces instead of tabs for alignment. */
    PRINTER_USE_SPACES = 1 << 2,
    /* Write //line directives so the output keeps the source's positions. */
    PRINTER_SOURCE_POS = 1 << 3
};

/* Not API. Go's unexported normalizeNumbers mode, which go/format and gofmt
 * set to print number literals with lower case prefixes and exponents, 0X1P4
 * as 0x1p4, and integer imaginary literals without leading zeros. */
#define BURROW__PRINTER_NORMALIZE_NUMBERS ((PrinterMode)1 << 30)

/* printer.Config. The zero Config prints with a tab width of 0, and
 * printer_fprint uses one of 8. indent is how many tabs every line starts with
 * on top of its own indentation. */
typedef struct PrinterConfig {
    PrinterMode mode;
    Int tabwidth;
    Int indent;
} PrinterConfig;

/* printer.CommentedNode, a node and the comments to print with it. node is an
 * AstFile pointer or an expression, statement, declaration or spec, as for
 * printer_fprint, and comments holds AstCommentGroup pointers in source order,
 * usually the whole file's. Only the ones inside the node, and its doc
 * comment, are printed. */
typedef struct PrinterCommentedNode {
    Any node;
    Slice comments;
} PrinterCommentedNode;

/* Config.Fprint: node printed to output as cfg says, with positions from fset.
 * The error is the first one output gave back, one for a node of a type the
 * printer does not take, or, with PRINTER_SOURCE_POS, one for a file name with
 * a newline in it, which no //line directive can hold. */
BURROW_BORROWS(ret) Error printer_config_fprint(const PrinterConfig *cfg, Alloc *a,
                                                IoWriter output, TokenFileSet *fset,
                                                Any node);

/* printer.Fprint: printer_config_fprint with a tab width of 8 and nothing else
 * set. gofmt uses tabs to indent but spaces to align, so use go/format for
 * output that matches it. */
BURROW_BORROWS(ret) Error printer_fprint(Alloc *a, IoWriter output, TokenFileSet *fset,
                                         Any node);

extern const Type burrow_type_PrinterMode;
extern const Type burrow_type_PrinterConfig;
extern const Type burrow_type_PrinterConfigPtr;
extern const Type burrow_type_PrinterCommentedNode;
extern const Type burrow_type_PrinterCommentedNodePtr;

#define TYPE_PRINTER_CONFIG_PTR TYPE_OF(PrinterConfigPtr)
#define TYPE_PRINTER_COMMENTED_NODE_PTR TYPE_OF(PrinterCommentedNodePtr)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_PRINTER_H */
