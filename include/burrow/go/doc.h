/* go/doc, documentation extracted from a Go syntax tree.
 *
 * Go's go/doc. doc_new_from_files reads the files of one package, parsed with
 * their comments, and gives back a DocPackage holding the package comment and
 * every constant, variable, function and type worth documenting, sorted and
 * grouped the way go doc shows them. Functions that return a type are filed
 * under that type, and so are the methods, embedded ones included. The
 * examples in the package's _test.go files are attached to what they show:
 *
 *     Slice files = ...; // AstFile pointers from parser_parse_file
 *     Error err = BURROW_NO_ERROR;
 *     DocPackage *p = doc_new_from_files(a, fset, files, BURROW_S("example.com/p"), 0,
 *                                        &err);
 *     for (Int i = 0; i < p->funcs.len; i++) {
 *         DocFunc *f = BURROW_AT(DocFunc *, p->funcs, i);
 *         // f->name, f->doc, f->decl ...
 *     }
 *
 * As in Go the trees are taken over and changed: the comments that end up in
 * the documentation are taken out of the declarations, function bodies are
 * dropped, and without DOC_ALL_DECLS everything unexported is filtered away.
 * DOC_PRESERVE_AST leaves the trees as they were.
 *
 * Memory. Everything is made in the allocator passed in, next to the trees,
 * and points into them, with nothing that frees one part alone. Use the arena
 * the files were parsed into and free it when done.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/doc */

#ifndef BURROW_GO_DOC_H
#define BURROW_GO_DOC_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/doc/comment.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* doc.Mode, the flags of doc_new and doc_new_from_files. */
typedef Int DocMode;

enum {
    /* Document every package level declaration, not just the exported ones. */
    DOC_ALL_DECLS = 1 << 0,
    /* Show every embedded method, not just the ones of unexported anonymous
     * fields. */
    DOC_ALL_METHODS = 1 << 1,
    /* Leave the trees as they were, function bodies and comments included. */
    DOC_PRESERVE_AST = 1 << 2
};

/* doc.Example, an example function found in a _test.go file. */
typedef struct DocExample {
    Str name;       /* what it is an example of, with any suffix */
    Str suffix;     /* the suffix without its '_', set by doc_new_from_files */
    Str doc;        /* the function's doc comment */
    AstNode code;   /* the body, or the whole file when that is the example */
    AstFile *play;  /* a whole program version of it, or NULL */
    Slice comments; /* of AstCommentGroup * */
    Str output;     /* the expected output */
    bool unordered;
    bool empty_output; /* whether the output is expected to be empty */
    Int order;         /* where it was in the source */
} DocExample;

/* doc.Value, the documentation of a const or var declaration, which may be a
 * group of them. */
typedef struct DocValue {
    Str doc;
    Slice names; /* of Str, in the order they are declared */
    AstGenDecl *decl;
    Int order; /* what they sort by when there is no name, unexported in Go */
} DocValue;

/* doc.Func, the documentation of a function or a method. recv, orig and level
 * are only set for a method. */
typedef struct DocFunc {
    Str doc;
    Str name;
    AstFuncDecl *decl;
    Str recv;       /* the receiver, "T" or "*T", maybe with [P1, ..., Pn] */
    Str orig;       /* the receiver the method was declared with */
    Int level;      /* how deep it is embedded, 0 when it is not */
    Slice examples; /* of DocExample *, sorted */
} DocFunc;

/* doc.Type, the documentation of a type declaration and what goes with it. */
typedef struct DocType {
    Str doc;
    Str name;
    AstGenDecl *decl;
    Slice consts;   /* of DocValue *, the constants mostly of this type */
    Slice vars;     /* of DocValue *, the variables mostly of this type */
    Slice funcs;    /* of DocFunc *, the functions that return it */
    Slice methods;  /* of DocFunc *, embedded ones included */
    Slice examples; /* of DocExample *, sorted */
} DocType;

/* doc.Note, a comment that starts with MARKER(uid): and a body. A marker is
 * two or more of A to Z, the uid at least one character, and the colon may be
 * left out. */
typedef struct DocNote {
    TokenPos pos, end; /* where the comment holding the marker is */
    Str uid;
    Str body;
} DocNote;

/* doc.Package, the documentation of a whole package. */
typedef struct DocPackage {
    Str doc;
    Str name;
    Str import_path;
    Slice imports;   /* of Str, sorted */
    Slice filenames; /* of Str, sorted */
    Map *notes;      /* marker (Str) to a Slice of DocNote *, in source order */
    Slice bugs;      /* of Str, the bodies of the BUG notes. Deprecated, use notes */
    Slice consts;    /* of DocValue *, sorted */
    Slice types;     /* of DocType *, sorted */
    Slice vars;      /* of DocValue *, sorted */
    Slice funcs;     /* of DocFunc *, sorted */
    Slice examples;  /* of DocExample *, the package's own, sorted */
    /* What doc links in comments are resolved with. Unexported in Go. */
    Map *import_by_name; /* name (Str) to import path (Str) */
    Map *syms;           /* "name" or "Type.name" (Str) to bool */
} DocPackage;

/* doc.Filter, which names doc_package_filter keeps. */
BURROW_FUNC(DocFilter, bool, Str name);

/* doc.New: the documentation of pkg, whose files are read in order of their
 * names. The trees are changed unless mode has DOC_PRESERVE_AST. Examples are
 * not looked for, which needs doc_new_from_files. */
BURROW_OWNS(ret) DocPackage *doc_new(Alloc *a, AstPackage *pkg, Str import_path,
                                     DocMode mode);

/* doc.NewFromFiles: the documentation of the package made of files, which
 * are AstFile pointers that must all be in fset. Those whose names end in
 * "_test.go" are searched for examples, which are attached to the function,
 * type, method or package they are named after. The others must end in ".go"
 * and are the package. Go takes the mode as an optional argument, and 0 here
 * is leaving it out. A NULL fset panics, as it does in Go. */
BURROW_OWNS(ret) DocPackage *doc_new_from_files(Alloc *a, TokenFileSet *fset,
                                                Slice files, Str import_path,
                                                DocMode mode, Error *err);

/* Package.Filter: keeps only the declarations f says yes to, along with the
 * types that have a field, method or function it says yes to, and clears the
 * package comment. */
void doc_package_filter(DocPackage *p, DocFilter f);

/* doc.Examples: the examples in test_files, AstFile pointers, sorted by name.
 * The suffix is left empty, since only doc_new_from_files sets it. */
BURROW_OWNS(ret) Slice doc_examples(Alloc *a, Slice test_files);

/* doc.IsPredeclared: whether s is a predeclared type, function or constant. */
bool doc_is_predeclared(Str s);

/* doc.IllegalPrefixes: the lower case prefixes that keep a sentence from
 * being a synopsis. */
BURROW_STATIC(ret) Slice doc_illegal_prefixes(void);

/* Package.Synopsis: the first sentence of text, as plain text without doc
 * links or other markup, or "" when it starts with one of the illegal
 * prefixes or is not a paragraph. */
BURROW_OWNS(ret) Str doc_package_synopsis(DocPackage *p, Alloc *a, Str text);

/* doc.Synopsis: doc_package_synopsis for a package with no symbols, so that
 * doc links are left as they are. Deprecated in Go. */
BURROW_OWNS(ret) Str doc_synopsis(Alloc *a, Str text);

/* Package.Parser: a new go/doc/comment parser, made in a, that resolves doc
 * links against p. p has to outlive it. */
BURROW_OWNS(ret) CommentParser *doc_package_parser(DocPackage *p, Alloc *a);

/* Package.Printer: a new go/doc/comment printer, made in a, for p's
 * comments. */
BURROW_OWNS(ret) CommentPrinter *doc_package_printer(DocPackage *p, Alloc *a);

/* Package.HTML, Package.Markdown and Package.Text: the doc comment text
 * parsed with doc_package_parser and printed with doc_package_printer. */
BURROW_OWNS(ret) Slice doc_package_html(DocPackage *p, Alloc *a, Str text);
BURROW_OWNS(ret) Slice doc_package_markdown(DocPackage *p, Alloc *a, Str text);
BURROW_OWNS(ret) Slice doc_package_text(DocPackage *p, Alloc *a, Str text);

/* doc.ToHTML: text as HTML, with the words in words (Str to Str) set in
 * italics. Deprecated in Go. */
void doc_to_html(Alloc *a, IoWriter w, Str text, Map *words);

/* doc.ToText: text as plain text wrapped at width, with prefix before each
 * line and code_prefix before each line of code. Deprecated in Go. */
void doc_to_text(Alloc *a, IoWriter w, Str text, Str prefix, Str code_prefix,
                 Int width);

extern const Type burrow_type_DocMode;
extern const Type burrow_type_DocExample;
extern const Type burrow_type_DocExamplePtr;
extern const Type burrow_type_DocValue;
extern const Type burrow_type_DocValuePtr;
extern const Type burrow_type_DocFunc;
extern const Type burrow_type_DocFuncPtr;
extern const Type burrow_type_DocType;
extern const Type burrow_type_DocTypePtr;
extern const Type burrow_type_DocNote;
extern const Type burrow_type_DocNotePtr;
extern const Type burrow_type_DocNoteSlice;
extern const Type burrow_type_DocPackage;
extern const Type burrow_type_DocPackagePtr;

#define TYPE_DOC_EXAMPLE_PTR TYPE_OF(DocExamplePtr)
#define TYPE_DOC_VALUE_PTR TYPE_OF(DocValuePtr)
#define TYPE_DOC_FUNC_PTR TYPE_OF(DocFuncPtr)
#define TYPE_DOC_TYPE_PTR TYPE_OF(DocTypePtr)
#define TYPE_DOC_NOTE_PTR TYPE_OF(DocNotePtr)
#define TYPE_DOC_NOTE_SLICE TYPE_OF(DocNoteSlice) /* []*doc.Note */
#define TYPE_DOC_PACKAGE_PTR TYPE_OF(DocPackagePtr)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_DOC_H */
