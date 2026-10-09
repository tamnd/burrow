/* go/token, the tokens of Go source and positions in it.
 *
 * Go's go/token. A Token is one of the lexical tokens of the Go language, the
 * kind go/scanner hands out: TOKEN_IDENT, TOKEN_ADD, TOKEN_FUNC and the rest. A
 * TokenPos is a place in a set of source files packed into one integer, and a
 * TokenFileSet turns it back into a file name, line and column:
 *
 *     TokenFileSet *fset = token_new_file_set(a);
 *     Str src = BURROW_S("package p\n\nvar x int\n");
 *     TokenFile *f = token_file_set_add_file(fset, BURROW_S("p.go"), -1, src.len);
 *     token_file_set_lines_for_content(f, slice_from_str(a, src));
 *     TokenPosition pos = token_file_set_position(fset, token_file_pos(f, 15));
 *     // token_position_string(pos, a) is "p.go:3:5"
 *     token_file_set_free(fset);
 *
 * A file set and the files in it come from the allocator given to
 * token_new_file_set, and so do the line tables. token_file_set_free gives all
 * of it back, the files that were removed from the set included, so a file is
 * good until its set is freed. From an arena, freeing the arena does the same.
 *
 * The methods are safe to call from more than one thread, as Go's are, as long
 * as the allocator is. Go's are on a garbage collected heap, where an old line
 * table stays alive for whoever is still reading it. Here a table that is
 * replaced is freed, so what token_file_lines returns is only good until the
 * next change to that file's lines.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/token */

#ifndef BURROW_GO_TOKEN_H
#define BURROW_GO_TOKEN_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ tokens */

/* token.Token. The values are Go's, gaps included, so a Token means the same
 * number in both. The BURROW__TOKEN_ ones mark where each group starts and
 * ends, as Go's unexported constants do, and are not tokens. */
typedef Int Token;

enum {
    /* Special tokens */
    TOKEN_ILLEGAL = 0,
    TOKEN_EOF = 1,
    TOKEN_COMMENT = 2,

    BURROW__TOKEN_LITERAL_BEG = 3,
    /* Identifiers and basic type literals */
    TOKEN_IDENT = 4,  /* main */
    TOKEN_INT = 5,    /* 12345 */
    TOKEN_FLOAT = 6,  /* 123.45 */
    TOKEN_IMAG = 7,   /* 123.45i */
    TOKEN_CHAR = 8,   /* 'a' */
    TOKEN_STRING = 9, /* "abc" */
    BURROW__TOKEN_LITERAL_END = 10,

    BURROW__TOKEN_OPERATOR_BEG = 11,
    /* Operators and delimiters */
    TOKEN_ADD = 12, /* + */
    TOKEN_SUB = 13, /* - */
    TOKEN_MUL = 14, /* * */
    TOKEN_QUO = 15, /* / */
    TOKEN_REM = 16, /* % */

    TOKEN_AND = 17,     /* & */
    TOKEN_OR = 18,      /* | */
    TOKEN_XOR = 19,     /* ^ */
    TOKEN_SHL = 20,     /* << */
    TOKEN_SHR = 21,     /* >> */
    TOKEN_AND_NOT = 22, /* &^ */

    TOKEN_ADD_ASSIGN = 23, /* += */
    TOKEN_SUB_ASSIGN = 24, /* -= */
    TOKEN_MUL_ASSIGN = 25, /* *= */
    TOKEN_QUO_ASSIGN = 26, /* /= */
    TOKEN_REM_ASSIGN = 27, /* %= */

    TOKEN_AND_ASSIGN = 28,     /* &= */
    TOKEN_OR_ASSIGN = 29,      /* |= */
    TOKEN_XOR_ASSIGN = 30,     /* ^= */
    TOKEN_SHL_ASSIGN = 31,     /* <<= */
    TOKEN_SHR_ASSIGN = 32,     /* >>= */
    TOKEN_AND_NOT_ASSIGN = 33, /* &^= */

    TOKEN_LAND = 34,  /* && */
    TOKEN_LOR = 35,   /* || */
    TOKEN_ARROW = 36, /* <- */
    TOKEN_INC = 37,   /* ++ */
    TOKEN_DEC = 38,   /* -- */

    TOKEN_EQL = 39,    /* == */
    TOKEN_LSS = 40,    /* < */
    TOKEN_GTR = 41,    /* > */
    TOKEN_ASSIGN = 42, /* = */
    TOKEN_NOT = 43,    /* ! */

    TOKEN_NEQ = 44,      /* != */
    TOKEN_LEQ = 45,      /* <= */
    TOKEN_GEQ = 46,      /* >= */
    TOKEN_DEFINE = 47,   /* := */
    TOKEN_ELLIPSIS = 48, /* ... */

    TOKEN_LPAREN = 49, /* ( */
    TOKEN_LBRACK = 50, /* [ */
    TOKEN_LBRACE = 51, /* { */
    TOKEN_COMMA = 52,  /* , */
    TOKEN_PERIOD = 53, /* . */

    TOKEN_RPAREN = 54,    /* ) */
    TOKEN_RBRACK = 55,    /* ] */
    TOKEN_RBRACE = 56,    /* } */
    TOKEN_SEMICOLON = 57, /* ; */
    TOKEN_COLON = 58,     /* : */
    BURROW__TOKEN_OPERATOR_END = 59,

    BURROW__TOKEN_KEYWORD_BEG = 60,
    /* Keywords */
    TOKEN_BREAK = 61,
    TOKEN_CASE = 62,
    TOKEN_CHAN = 63,
    TOKEN_CONST = 64,
    TOKEN_CONTINUE = 65,

    TOKEN_DEFAULT = 66,
    TOKEN_DEFER = 67,
    TOKEN_ELSE = 68,
    TOKEN_FALLTHROUGH = 69,
    TOKEN_FOR = 70,

    TOKEN_FUNC = 71,
    TOKEN_GO = 72,
    TOKEN_GOTO = 73,
    TOKEN_IF = 74,
    TOKEN_IMPORT = 75,

    TOKEN_INTERFACE = 76,
    TOKEN_MAP = 77,
    TOKEN_PACKAGE = 78,
    TOKEN_RANGE = 79,
    TOKEN_RETURN = 80,

    TOKEN_SELECT = 81,
    TOKEN_STRUCT = 82,
    TOKEN_SWITCH = 83,
    TOKEN_TYPE = 84,
    TOKEN_VAR = 85,
    BURROW__TOKEN_KEYWORD_END = 86,

    BURROW__TOKEN_ADDITIONAL_BEG = 87,
    /* Additional tokens, handled in an ad-hoc manner */
    TOKEN_TILDE = 88,
    BURROW__TOKEN_ADDITIONAL_END = 89,
};

/* Token.String: the token's text for operators, delimiters and keywords, its
 * name for the rest ("IDENT", "EOF"), and "token(" and the number for a value
 * that is not a token. The string is built in a. A failed allocation gives the
 * empty string. */
BURROW_OWNS(ret) Str token_string(Token tok, Alloc *a);

/* The precedences token_precedence gives. A binary operator has a precedence
 * from 1 to 5, and anything else has TOKEN_LOWEST_PREC. */
enum {
    TOKEN_LOWEST_PREC = 0, /* non-operators */
    TOKEN_UNARY_PREC = 6,
    TOKEN_HIGHEST_PREC = 7,
};

/* Token.Precedence: the precedence of the binary operator op, or
 * TOKEN_LOWEST_PREC when op is not one. */
Int token_precedence(Token op);

/* token.Lookup: the keyword token for ident, or TOKEN_IDENT when ident is not a
 * keyword. */
Token token_lookup(Str ident);

/* Token.IsLiteral: tok is an identifier or a basic literal. */
bool token_is_literal(Token tok);

/* Token.IsOperator: tok is an operator or delimiter, TOKEN_TILDE included. */
bool token_is_operator(Token tok);

/* Token.IsKeyword: tok is a keyword. */
bool token_is_keyword(Token tok);

/* token.IsExported: name starts with an upper case letter. */
bool token_is_exported(Str name);

/* token.IsKeyword: name is a Go keyword, such as "func" or "return". The name
 * token_is_keyword was taken by the method. */
bool token_is_keyword_str(Str name);

/* token.IsIdentifier: name is a Go identifier, which is letters, digits and
 * underscores, not empty and not starting with a digit. Keywords are not
 * identifiers. */
bool token_is_identifier(Str name);

/* --------------------------------------------------------------- positions */

/* token.Position, a position in a file in full. It is valid when line is above
 * zero. filename is borrowed from the file it came from. */
typedef struct TokenPosition {
    Str filename; /* filename, if any */
    Int offset;   /* offset, starting at 0 */
    Int line;     /* line number, starting at 1 */
    Int column;   /* column number, starting at 1 (byte count) */
} TokenPosition;

/* Position.IsValid. */
bool token_position_is_valid(const TokenPosition *pos);

/* Position.String, built in a, in one of these forms:
 *
 *     file:line:column    valid position with file name
 *     file:line           valid position with file name but no column (column == 0)
 *     line:column         valid position without file name
 *     line                valid position without file name and no column (column == 0)
 *     file                invalid position with file name
 *     -                   invalid position without file name
 *
 * A failed allocation gives the empty string. */
BURROW_OWNS(ret) Str token_position_string(TokenPosition pos, Alloc *a);

/* token.Pos, a position in a file set as one integer. The positions of a file
 * run from its base to its base plus its size, so the difference between a
 * Pos and the base is the byte offset in the file. Pos values compare with the
 * usual operators: in one file that is comparing offsets, and across files the
 * one added to the set first is the smaller. */
typedef Int TokenPos;

/* token.NoPos, the zero Pos, which has no file and is smaller than any other
 * Pos. */
enum { TOKEN_NO_POS = 0 };

/* Pos.IsValid: p is not TOKEN_NO_POS. */
bool token_pos_is_valid(TokenPos p);

/* -------------------------------------------------------------------- files */

/* token.File, a file in a file set: its name, its size and the offsets its
 * lines start at. Made by token_file_set_add_file and not by hand. A file may
 * be in more than one set, see token_file_set_add_existing_files. */
typedef struct TokenFile TokenFile;

/* File.String: "name(base-end)", built in a. */
BURROW_OWNS(ret) Str token_file_string(TokenFile *f, Alloc *a);

/* File.Name, File.Base, File.Size and File.End. The name is the file's own
 * copy, good as long as the file. */
BURROW_BORROWS(ret, f) Str token_file_name(TokenFile *f);
Int token_file_base(TokenFile *f);
Int token_file_size(TokenFile *f);
TokenPos token_file_end(TokenFile *f);

/* File.LineCount. */
Int token_file_line_count(TokenFile *f);

/* File.AddLine: offset is where a new line starts. It has to be past the
 * start of the last line and inside the file, and is ignored otherwise. */
void token_file_add_line(TokenFile *f, Int offset);

/* File.MergeLine: line and the one after it become one line, as if the newline
 * at the end of line were a space. Panics when line is not a line of f, or is
 * its last. */
void token_file_merge_line(TokenFile *f, Int line);

/* File.Lines: the offsets the lines start at, a Slice of Int. Borrowed, and
 * good until the next change to the file's lines. Do not change it. */
BURROW_BORROWS(ret, f) Slice token_file_lines(TokenFile *f);

/* File.SetLines: the offsets of the first character of each line, each larger
 * than the last and smaller than the file's size, so "ab\nc\n" is {0, 3} and an
 * empty file has none. Reports whether lines was in order and the table was
 * set. lines is a Slice of Int, and the file keeps a copy of it. */
bool token_file_set_lines(TokenFile *f, Slice lines);

/* File.SetLinesForContent: the line table read from the file's content. Line
 * directives in it are not looked at. */
void token_file_set_lines_for_content(TokenFile *f, Slice content);

/* File.LineStart: the Pos of the start of line, which counts from 1. Line
 * information added with token_file_add_line_column_info is not looked at.
 * Panics when line is not a line of f. */
TokenPos token_file_line_start(TokenFile *f, Int line);

/* File.AddLineInfo: token_file_add_line_column_info with a column of 1. */
void token_file_add_line_info(TokenFile *f, Int offset, Str filename, Int line);

/* File.AddLineColumnInfo: from offset on, positions in f are reported as being
 * in filename at line and column, the way a //line directive asks. offset has
 * to be past the one given last time and inside the file, and the call is
 * ignored otherwise. The file keeps a copy of filename. */
void token_file_add_line_column_info(TokenFile *f, Int offset, Str filename, Int line,
                                     Int column);

/* File.Pos: the Pos of offset in f. An offset below zero gives the start of the
 * file and one past its end gives the end, and token_file_pos(f,
 * token_file_offset(f, p)) == p for any p this returns. */
TokenPos token_file_pos(TokenFile *f, Int offset);

/* File.Offset: the offset of p in f. A p before the file, TOKEN_NO_POS
 * included, gives 0 and one past its end gives its size. */
Int token_file_offset(TokenFile *f, TokenPos p);

/* File.Line: the line number of p, which has to be in f or TOKEN_NO_POS. */
Int token_file_line(TokenFile *f, TokenPos p);

/* File.PositionFor: the Position of p in f, with p brought inside the file as
 * token_file_offset does. When adjusted is set, line information from //line
 * directives applies. TOKEN_NO_POS gives the zero Position. */
TokenPosition token_file_position_for(TokenFile *f, TokenPos p, bool adjusted);

/* File.Position: token_file_position_for with adjusted set. */
TokenPosition token_file_position(TokenFile *f, TokenPos p);

/* ---------------------------------------------------------------- file sets */

/* token.FileSet, a set of files, each with its own range of Pos values. */
typedef struct TokenFileSet TokenFileSet;

/* token.NewFileSet. A set in a, or NULL when a says no. */
BURROW_OWNS(ret) TokenFileSet *token_new_file_set(Alloc *a);

/* Gives back s, the files it made and their line tables. Files it was given by
 * token_file_set_add_existing_files belong to the set that made them. NULL is
 * fine. */
void token_file_set_free(TokenFileSet *s);

/* FileSet.Base: the smallest base the next token_file_set_add_file may have. */
Int token_file_set_base(TokenFileSet *s);

/* FileSet.AddFile: a new file in s called filename, with Pos values from base to
 * base plus size. base may not be less than token_file_set_base, and a negative
 * base means that. The set's base moves to base + size + 1, past the file and
 * the position of its end. The file keeps a copy of filename. Panics when base
 * or size is out of range, and returns NULL when the allocator says no. */
BURROW_BORROWS(ret, s) TokenFile *token_file_set_add_file(TokenFileSet *s, Str filename,
                                                          Int base, Int size);

/* FileSet.AddExistingFiles: adds each of the n files that is not in s already.
 * No two of the files in s afterwards may overlap. The files stay where they
 * were made, and s does not free them. */
void token_file_set_add_existing_files(TokenFileSet *s, TokenFile *const *files, Int n);

/* FileSet.RemoveFile: takes file out of s, so its positions no longer find
 * anything. A file that is not in s is ignored. The file itself stays until the
 * set that made it is freed. */
void token_file_set_remove_file(TokenFileSet *s, TokenFile *file);

/* The func(*File) bool that FileSet.Iterate takes. */
BURROW_FUNC(TokenFileFunc, bool, TokenFile *f);

/* FileSet.Iterate: calls yield for each file in s in ascending base order,
 * until it returns false. The set is not locked while yield runs, and yield may
 * change it. */
void token_file_set_iterate(TokenFileSet *s, TokenFileFunc yield);

/* FileSet.File: the file p is in, or NULL when there is none, as for
 * TOKEN_NO_POS. */
BURROW_BORROWS(ret, s) TokenFile *token_file_set_file(TokenFileSet *s, TokenPos p);

/* FileSet.PositionFor: the Position of p, which has to be in s or be
 * TOKEN_NO_POS. When adjusted is set, //line directives apply. s may be NULL
 * when p is TOKEN_NO_POS. */
TokenPosition token_file_set_position_for(TokenFileSet *s, TokenPos p, bool adjusted);

/* FileSet.Position: token_file_set_position_for with adjusted set. */
TokenPosition token_file_set_position(TokenFileSet *s, TokenPos p);

/* The func(any) error that FileSet.Read and FileSet.Write take. v is the set in
 * the form Go's serializedFileSet has, a struct with descriptors, so that
 * encoding/gob and encoding/json can take it as they are. */
BURROW_FUNC(TokenCodecFunc, Error, Any v);

/* FileSet.Read: calls decode to fill in s. The set copies what was decoded, so
 * what decode allocated is still its own to give back. */
BURROW_BORROWS(ret) Error token_file_set_read(TokenFileSet *s, TokenCodecFunc decode);

/* FileSet.Write: calls encode with a copy of s. */
BURROW_BORROWS(ret) Error token_file_set_write(TokenFileSet *s, TokenCodecFunc encode);

#ifdef __cplusplus
}
#endif

#endif
