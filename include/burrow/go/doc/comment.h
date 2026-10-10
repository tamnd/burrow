/* go/doc/comment, Go doc comments.
 *
 * Go's go/doc/comment. A CommentParser turns the text of a doc comment, with
 * the comment markers already taken off, into a CommentDoc of paragraphs,
 * headings, lists and code blocks, and a CommentPrinter writes a CommentDoc out
 * again as a gofmt'ed comment, HTML, Markdown or plain text:
 *
 *     CommentParser p = {0};
 *     CommentDoc *d = comment_parser_parse(&p, a, BURROW_S("Package hello says hello.\n"));
 *     CommentPrinter pr = {0};
 *     Slice html = comment_printer_html(&pr, a, d);
 *
 * The zero Parser and the zero Printer are ready to use, as they are in Go, and
 * a NULL one is taken to be the zero one.
 *
 * Blocks and texts are nodes that start with a CommentBase, the way go/ast's
 * do, and CommentBlock and CommentText are pointers to that header. Look at
 * the kind to see which node one is:
 *
 *     CommentBlock b = BURROW_AT(CommentBlock, d->content, 0);
 *     if (b->kind == COMMENT_KIND_PARAGRAPH) {
 *         CommentParagraph *para = (CommentParagraph *)b;
 *         // ...
 *     }
 *
 * Go's Plain and Italic are strings, and here they are nodes holding one, so
 * that every Text is the same kind of pointer.
 *
 * Memory. comment_parser_parse copies the text into the allocator and builds
 * the whole Doc there, with nothing that frees one node alone, so parse into an
 * arena. A printer gives back one byte Slice made in its allocator, which
 * mem_free(a, out.p, (size_t)out.cap, 1) frees, and frees what else it made on
 * the way.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/doc/comment */

#ifndef BURROW_GO_DOC_COMMENT_H
#define BURROW_GO_DOC_COMMENT_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- nodes */

/* Which node a struct is. The first four are blocks and the rest are texts. */
typedef enum CommentKind {
    COMMENT_KIND_INVALID = 0,
    COMMENT_KIND_CODE,
    COMMENT_KIND_HEADING,
    COMMENT_KIND_LIST,
    COMMENT_KIND_PARAGRAPH,
    COMMENT_KIND_PLAIN,
    COMMENT_KIND_ITALIC,
    COMMENT_KIND_LINK,
    COMMENT_KIND_DOC_LINK
} CommentKind;

/* The header every block and text starts with. kind holds a CommentKind. */
typedef struct CommentBase {
    Int kind;
} CommentBase;

/* comment.Block: a CommentCode, CommentHeading, CommentList or
 * CommentParagraph. */
typedef CommentBase *CommentBlock;

/* comment.Text: a CommentPlain, CommentItalic, CommentLink or
 * CommentDocLink. */
typedef CommentBase *CommentText;

/* comment.Doc, a parsed doc comment. content holds CommentBlock and links
 * holds CommentLinkDef pointers, the link definitions in the comment. */
typedef struct CommentDoc {
    Slice content;
    Slice links;
} CommentDoc;

/* comment.LinkDef, one link definition, "[text]: url". used says whether the
 * comment links to it anywhere. */
typedef struct CommentLinkDef {
    Str text;
    Str url;
    bool used;
} CommentLinkDef;

/* comment.Heading. text holds CommentText. */
typedef struct CommentHeading {
    CommentBase node;
    Slice text;
} CommentHeading;

/* comment.List, a numbered or bullet list. items holds CommentListItem
 * pointers and is never empty.
 *
 * force_blank_before says the list has to have a blank line before it when the
 * comment is printed, and force_blank_between that its items have to have
 * blank lines between them. The parser sets them to keep the blank lines that
 * were in the comment. */
typedef struct CommentList {
    CommentBase node;
    Slice items;
    bool force_blank_before;
    bool force_blank_between;
} CommentList;

/* List.BlankBefore: whether a blank line goes before the list when the comment
 * is printed, which it does when force_blank_before is set or blank lines go
 * between the items. */
bool comment_list_blank_before(CommentList *l);

/* List.BlankBetween: whether blank lines go between the items when the comment
 * is printed, which they do when force_blank_between is set or an item does not
 * have exactly one block. */
bool comment_list_blank_between(CommentList *l);

/* comment.ListItem. number is the decimal number of an item in a numbered list
 * and empty in a bullet list. content holds CommentBlock, and each one of them
 * is a CommentParagraph. */
typedef struct CommentListItem {
    Str number;
    Slice content;
} CommentListItem;

/* comment.Paragraph. text holds CommentText. */
typedef struct CommentParagraph {
    CommentBase node;
    Slice text;
} CommentParagraph;

/* comment.Code, a preformatted block. text is never empty, ends in a newline,
 * and does not start or end with a blank line. */
typedef struct CommentCode {
    CommentBase node;
    Str text;
} CommentCode;

/* comment.Plain, text printed as it is. */
typedef struct CommentPlain {
    CommentBase node;
    Str text;
} CommentPlain;

/* comment.Italic, text printed in italics. */
typedef struct CommentItalic {
    CommentBase node;
    Str text;
} CommentItalic;

/* comment.Link, a link to a URL. auto_ (Go's Auto) says whether it is a URL
 * written out in the text, rather than one linked with [text]. text holds
 * CommentText. */
typedef struct CommentLink {
    CommentBase node;
    bool auto_;
    Slice text;
    Str url;
} CommentLink;

/* comment.DocLink, a link to the documentation of a Go package or symbol. text
 * holds CommentText. The fields that are set say what is linked to:
 *
 *   - import_path: another package;
 *   - import_path and name: a const, func, type or var in another package;
 *   - import_path, recv and name: a method in another package;
 *   - name: a const, func, type or var in this package;
 *   - recv and name: a method in this package.
 *
 * recv is the receiver type without any star. */
typedef struct CommentDocLink {
    CommentBase node;
    Slice text;
    Str import_path;
    Str recv;
    Str name;
} CommentDocLink;

/* DocLink.DefaultURL: the URL of the documentation l links to, with base_url
 * in front of a link to another package:
 *
 *   - base_url/ImportPath for a package;
 *   - base_url/ImportPath#Name for a symbol in another package;
 *   - base_url/ImportPath#Recv.Name for a method in another package;
 *   - #Name and #Recv.Name for this package.
 *
 * When base_url ends in a slash, one goes between ImportPath and the #, so
 * "/pkg/" gives "/pkg/math/#Sqrt" and "/pkg" gives "/pkg/math#Sqrt". */
BURROW_OWNS(ret) Str comment_doc_link_default_url(CommentDocLink *l, Alloc *a,
                                                  Str base_url);

/* Heading.DefaultID: the anchor of h, "hdr-" and then its text with every rune
 * that is not an ASCII letter, digit or underscore made an underscore, or the
 * empty string for an empty heading. "Go Doc Comments" gives
 * "hdr-Go_Doc_Comments". */
BURROW_OWNS(ret) Str comment_heading_default_id(CommentHeading *h, Alloc *a);

/* ------------------------------------------------------------------ parser */

/* Parser.LookupPackage: resolves the package name in [name], [name.Sym] or
 * [name.Sym.Method] to an import path, setting *ok to say whether it could.
 * An empty path with *ok set says the name is the current package. */
BURROW_FUNC(CommentLookupPackageFunc, Str, Str name, bool *ok);

/* Parser.LookupSym: whether recv.name, or name when recv is empty, is a symbol
 * of the current package. */
BURROW_FUNC(CommentLookupSymFunc, bool, Str recv, Str name);

/* comment.Parser, a doc comment parser. All of its fields may be left zero.
 *
 * words maps Str to Str: identifiers to put in italics, and to link to the URL
 * they map to when that is not empty. It is what go/doc.ToHTML's words are.
 *
 * lookup_package resolves a package name for a doc link, and a doc link is
 * only made for a package it says yes to, or one with a slash in its path, or
 * one of the standard library's packages with a one element path, such as math.
 * Those last ones are asked about too, in case the file imports something else
 * under the same name. A nil func says no to everything.
 *
 * lookup_sym says which symbols of the current package [Name] and [Recv.Name]
 * link to, and a nil func says none. */
typedef struct CommentParser {
    Map *words;
    CommentLookupPackageFunc lookup_package;
    CommentLookupSymFunc lookup_sym;
} CommentParser;

/* DefaultLookupPackage: what a nil lookup_package falls back to. It knows the
 * standard library's packages with a one element import path, such as math,
 * and gives back the name itself as the path. */
BURROW_BORROWS(ret, name) Str comment_default_lookup_package(Str name, bool *ok);

/* Parser.Parse: parses the text of a doc comment, without its comment markers,
 * into a Doc made in a. Parsing never fails, and every text is something. */
BURROW_OWNS(ret) CommentDoc *comment_parser_parse(CommentParser *p, Alloc *a, Str text);

/* ----------------------------------------------------------------- printer */

/* Printer.HeadingID: the anchor to give a heading in HTML and Markdown, or the
 * empty string for none. What it gives back is copied, so it can live
 * anywhere. */
BURROW_FUNC(CommentHeadingIDFunc, Str, CommentHeading *h);

/* Printer.DocLinkURL: the URL a doc link goes to. What it gives back is
 * copied, so it can live anywhere. */
BURROW_FUNC(CommentDocLinkURLFunc, Str, CommentDocLink *link);

/* comment.Printer, a doc comment printer. All of its fields may be left zero.
 *
 * heading_level is the level of the HTML and Markdown headings, 3 when it is
 * zero, which is <h3> and ###. heading_id gives the anchor of a heading, and
 * comment_heading_default_id is used when it is nil. doc_link_url gives the URL
 * of a doc link, and when it is nil the URL is comment_doc_link_default_url
 * with doc_link_base_url.
 *
 * The rest is for comment_printer_text. text_prefix goes at the start of every
 * line, and text_code_prefix instead of it at the start of each line of a code
 * block, text_prefix and a tab when it is empty. text_width is the longest a
 * line can be in runes, leaving out the prefix and the newline: 80 less the
 * runes of text_prefix when it is zero, and no limit at all when it is less
 * than zero. */
typedef struct CommentPrinter {
    Int heading_level;
    CommentHeadingIDFunc heading_id;
    CommentDocLinkURLFunc doc_link_url;
    Str doc_link_base_url;
    Str text_prefix;
    Str text_code_prefix;
    Int text_width;
} CommentPrinter;

/* Printer.Comment: d as gofmt formats a doc comment, without the comment
 * markers. The link definitions go at the end, the ones used first and then a
 * block of the rest. */
BURROW_OWNS(ret) Slice comment_printer_comment(CommentPrinter *p, Alloc *a,
                                               CommentDoc *d);

/* Printer.HTML: d as HTML. */
BURROW_OWNS(ret) Slice comment_printer_html(CommentPrinter *p, Alloc *a, CommentDoc *d);

/* Printer.Markdown: d as Markdown. */
BURROW_OWNS(ret) Slice comment_printer_markdown(CommentPrinter *p, Alloc *a,
                                                CommentDoc *d);

/* Printer.Text: d as plain text, with the paragraphs wrapped to text_width. */
BURROW_OWNS(ret) Slice comment_printer_text(CommentPrinter *p, Alloc *a, CommentDoc *d);

/* ------------------------------------------------------------ descriptors */

/* The type descriptors, so that a node can go to fmt and a list of them can be
 * made. TYPE_OF(CommentHeading) is comment.Heading, TYPE_OF(CommentHeadingPtr)
 * is *comment.Heading, and so on. TYPE_COMMENT_BLOCK and TYPE_COMMENT_TEXT are
 * what a list of blocks and a list of texts hold:
 *
 *     Slice text = slice_make(a, TYPE_COMMENT_TEXT, 0, 1);
 *     text = BURROW_APPEND(CommentText, a, text, &plain->node); */
extern const Type burrow_type_CommentBlock;
extern const Type burrow_type_CommentText;
extern const Type burrow_type_CommentDoc;
extern const Type burrow_type_CommentDocPtr;
extern const Type burrow_type_CommentLinkDef;
extern const Type burrow_type_CommentLinkDefPtr;
extern const Type burrow_type_CommentHeading;
extern const Type burrow_type_CommentHeadingPtr;
extern const Type burrow_type_CommentList;
extern const Type burrow_type_CommentListPtr;
extern const Type burrow_type_CommentListItem;
extern const Type burrow_type_CommentListItemPtr;
extern const Type burrow_type_CommentParagraph;
extern const Type burrow_type_CommentParagraphPtr;
extern const Type burrow_type_CommentCode;
extern const Type burrow_type_CommentCodePtr;
extern const Type burrow_type_CommentPlain;
extern const Type burrow_type_CommentPlainPtr;
extern const Type burrow_type_CommentItalic;
extern const Type burrow_type_CommentItalicPtr;
extern const Type burrow_type_CommentLink;
extern const Type burrow_type_CommentLinkPtr;
extern const Type burrow_type_CommentDocLink;
extern const Type burrow_type_CommentDocLinkPtr;

#define TYPE_COMMENT_BLOCK TYPE_OF(CommentBlock)
#define TYPE_COMMENT_TEXT TYPE_OF(CommentText)
#define TYPE_COMMENT_LINK_DEF_PTR TYPE_OF(CommentLinkDefPtr)
#define TYPE_COMMENT_LIST_ITEM_PTR TYPE_OF(CommentListItemPtr)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_DOC_COMMENT_H */
