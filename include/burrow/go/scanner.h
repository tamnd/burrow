/* go/scanner, the scanner that turns Go source into tokens.
 *
 * Go's go/scanner. A GoScanner reads the source of one TokenFile and hands
 * out its tokens one at a time, with their positions and literal text, and
 * adds the file's lines to it as it goes:
 *
 *     Str src = BURROW_S("cos(x) + 1i*sin(x) // Euler");
 *     TokenFileSet *fset = token_new_file_set(a);
 *     TokenFile *f = token_file_set_add_file(fset, BURROW_S(""), -1, src.len);
 *     GoScanner s;
 *     go_scanner_init(&s, a, f, slice_from_str(a, src), (GoScannerErrorHandler){0},
 *                     GO_SCANNER_SCAN_COMMENTS);
 *     for (;;) {
 *         Str lit;
 *         Token tok;
 *         TokenPos pos = go_scanner_scan(&s, &tok, &lit);
 *         if (tok == TOKEN_EOF)
 *             break;
 *         // ...
 *     }
 *
 * A literal is a view of the source, which has to stay put for as long as the
 * literals are in use. The exceptions are a comment or a raw string with a
 * carriage return in it, which Go hands back with the carriage returns taken
 * out, and that copy is made in the allocator given to go_scanner_init.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/scanner */

#ifndef BURROW_GO_SCANNER_H
#define BURROW_GO_SCANNER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ errors */

/* scanner.Error. pos, if valid, is where the offending token starts, and msg
 * says what is wrong with it. */
typedef struct GoScannerError {
    TokenPosition pos;
    Str msg;
} GoScannerError;

/* Error.Error: "file:line:column: msg", or msg alone when pos has neither a
 * file name nor a line. Built in a. */
BURROW_OWNS(ret) Str go_scanner_error_error(GoScannerError e, Alloc *a);

/* scanner.ErrorList, a slice of GoScannerError pointers. A zeroed one is an
 * empty list ready to use. Every error in it, and the text in each, is in the
 * allocator given to go_scanner_error_list_add. */
typedef Slice GoScannerErrorList;

extern const Type *const TYPE_GO_SCANNER_ERROR_LIST;

/* ErrorList.Add. Copies pos's file name and msg into a. On an allocation
 * failure the list is left as it was. */
void go_scanner_error_list_add(GoScannerErrorList *p, Alloc *a, TokenPosition pos,
                               Str msg);

/* The i'th error. */
BURROW_BORROWS(ret, p) GoScannerError *go_scanner_error_list_at(GoScannerErrorList p,
                                                                Int i);

/* ErrorList.Reset. Keeps the memory, as Go's does. */
void go_scanner_error_list_reset(GoScannerErrorList *p);

/* ErrorList.Len, Swap and Less, its sort.Interface. Less orders by file name,
 * then line, then column, then message. */
Int go_scanner_error_list_len(GoScannerErrorList p);
void go_scanner_error_list_swap(GoScannerErrorList p, Int i, Int j);
bool go_scanner_error_list_less(GoScannerErrorList p, Int i, Int j);

/* ErrorList.Sort, with sort_sort, so equal errors end up as Go's do. */
void go_scanner_error_list_sort(GoScannerErrorList p);

/* ErrorList.RemoveMultiples. Sorts the list and keeps only the first error on
 * each line. */
void go_scanner_error_list_remove_multiples(GoScannerErrorList *p);

/* ErrorList.Error: "no errors", the one error's text, or the first one's
 * followed by " (and N more errors)". Built in a. */
BURROW_OWNS(ret) Str go_scanner_error_list_error(GoScannerErrorList p, Alloc *a);

/* ErrorList.Err. The list as an Error, or no error when it is empty. Like Go's
 * it shares the list's memory, so it is good for as long as the list is, and
 * errors_as with TYPE_GO_SCANNER_ERROR_LIST gets the list back. The box around
 * it is made in the calling goroutine's error arena. */
BURROW_BORROWS(ret, p) Error go_scanner_error_list_err(GoScannerErrorList p);

/* scanner.PrintError. Writes each error in an ErrorList to w on a line of its
 * own, or for any other error its text and a newline. No error writes
 * nothing. */
void go_scanner_print_error(IoWriter w, Error err);

/* ----------------------------------------------------------------- scanner */

/* scanner.ErrorHandler. Called for each syntax error with where it is and
 * what it is. msg is good for the length of the call. */
BURROW_FUNC(GoScannerErrorHandler, void, TokenPosition pos, Str msg);

/* scanner.Mode, a set of GO_SCANNER_ flags. */
typedef Uint GoScannerMode;

enum {
    /* ScanComments: hand out comments as TOKEN_COMMENT. */
    GO_SCANNER_SCAN_COMMENTS = 1 << 0,
    /* Go's dontInsertSemis, for tests: no semicolons for newlines. */
    BURROW__GO_SCANNER_DONT_INSERT_SEMIS = 1 << 1
};

/* scanner.Scanner. error_count is Go's ErrorCount, the number of errors so
 * far, and is yours to read and change. The rest is the scanner's own. A
 * GoScanner holds no memory of its own, so there is nothing to free. */
typedef struct GoScanner {
    Alloc *a;
    TokenFile *file; /* source file handle */
    Str dir;         /* directory portion of the file name */
    Slice src;       /* source */
    GoScannerErrorHandler err;
    GoScannerMode mode;

    Rune ch;            /* current character */
    Int offset;         /* character offset */
    Int rd_offset;      /* reading offset (position after current character) */
    Int line_offset;    /* current line offset */
    bool insert_semi;   /* insert a semicolon before next newline */
    TokenPos nl_pos;    /* position of newline in preceding comment */
    bool end_pos_valid; /* end_pos overrides the offset as the end position */
    TokenPos end_pos;

    Int error_count;
} GoScanner;

/* Scanner.Init. Sets s up to scan src, which file's lines are added to as the
 * scanner meets them. Lines file already has are left alone, so scanning the
 * same file twice is fine. Panics when file's size is not src.len. err may be
 * zero, and is called for each error otherwise, possibly already here for the
 * first character. Stripped copies of literals come from a. */
void go_scanner_init(GoScanner *s, Alloc *a, TokenFile *file, Slice src,
                     GoScannerErrorHandler err, GoScannerMode mode);

/* Scanner.Scan. The next token, its position, and its literal in *lit:
 *
 *   - the text, for TOKEN_IDENT, TOKEN_INT, TOKEN_FLOAT, TOKEN_IMAG,
 *     TOKEN_CHAR, TOKEN_STRING and TOKEN_COMMENT, and for a keyword;
 *   - ";" for a semicolon in the source and "\n" for one the scanner put in
 *     at a newline or at the end of the file;
 *   - the offending character for TOKEN_ILLEGAL;
 *   - empty for everything else.
 *
 * The end of the source is TOKEN_EOF. On a syntax error Scan still gives the
 * best token it can, so check error_count or count the handler's calls rather
 * than looking for TOKEN_ILLEGAL. tok and lit may be NULL. */
TokenPos go_scanner_scan(GoScanner *s, Token *tok, Str *lit);

/* Scanner.End. The position just after the last token scanned, or
 * TOKEN_NO_POS before the first go_scanner_scan. */
TokenPos go_scanner_end(const GoScanner *s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_SCANNER_H */
