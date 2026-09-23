/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Internal to testing: the parts of go/token, go/scanner, go/ast and
 * go/parser that internal/fuzz reads a corpus file with. Every line of a
 * corpus file after the version is handed to parser.ParseExprFrom, so a line
 * that is not a valid value fails with whatever go/parser says about it, and
 * saying the same thing means scanning and parsing the same way it does, error
 * recovery included. corpus_scan.c is go/scanner and corpus_parse.c is the
 * half of go/parser that ParseExprFrom can reach.
 *
 * This goes when go/parser is ported for real, and corpus.c calls that. */

#ifndef BURROW_TESTING_CORPUS_SYNTAX_H
#define BURROW_TESTING_CORPUS_SYNTAX_H

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/mem.h"

/* go/token's Token, in its order. */
typedef enum CorpusTok {
    CT_ILLEGAL,
    CT_EOF,
    CT_COMMENT,

    CT_IDENT,
    CT_INT,
    CT_FLOAT,
    CT_IMAG,
    CT_CHAR,
    CT_STRING,

    CT_ADD,
    CT_SUB,
    CT_MUL,
    CT_QUO,
    CT_REM,
    CT_AND,
    CT_OR,
    CT_XOR,
    CT_SHL,
    CT_SHR,
    CT_AND_NOT,
    CT_ADD_ASSIGN,
    CT_SUB_ASSIGN,
    CT_MUL_ASSIGN,
    CT_QUO_ASSIGN,
    CT_REM_ASSIGN,
    CT_AND_ASSIGN,
    CT_OR_ASSIGN,
    CT_XOR_ASSIGN,
    CT_SHL_ASSIGN,
    CT_SHR_ASSIGN,
    CT_AND_NOT_ASSIGN,
    CT_LAND,
    CT_LOR,
    CT_ARROW,
    CT_INC,
    CT_DEC,
    CT_EQL,
    CT_LSS,
    CT_GTR,
    CT_ASSIGN,
    CT_NOT,
    CT_NEQ,
    CT_LEQ,
    CT_GEQ,
    CT_DEFINE,
    CT_ELLIPSIS,
    CT_LPAREN,
    CT_LBRACK,
    CT_LBRACE,
    CT_COMMA,
    CT_PERIOD,
    CT_RPAREN,
    CT_RBRACK,
    CT_RBRACE,
    CT_SEMICOLON,
    CT_COLON,
    CT_TILDE,

    CT_BREAK,
    CT_CASE,
    CT_CHAN,
    CT_CONST,
    CT_CONTINUE,
    CT_DEFAULT,
    CT_DEFER,
    CT_ELSE,
    CT_FALLTHROUGH,
    CT_FOR,
    CT_FUNC,
    CT_GO,
    CT_GOTO,
    CT_IF,
    CT_IMPORT,
    CT_INTERFACE,
    CT_MAP,
    CT_PACKAGE,
    CT_RANGE,
    CT_RETURN,
    CT_SELECT,
    CT_STRUCT,
    CT_SWITCH,
    CT_TYPE,
    CT_VAR,

    CT_COUNT
} CorpusTok;

/* Token.String, Token.Precedence and Token.IsLiteral. */
const char *burrow__testing_corpus_tok_string(CorpusTok t);
int burrow__testing_corpus_tok_prec(CorpusTok t);
bool burrow__testing_corpus_tok_is_literal(CorpusTok t);

/* A token.Position. */
typedef struct CorpusPosition {
    Str filename;
    int64_t line;
    int64_t column;
} CorpusPosition;

/* A line directive, token.File's lineInfo. */
typedef struct CorpusLineInfo {
    Int offset;
    Str filename;
    int64_t line;
    int64_t column;
} CorpusLineInfo;

typedef struct CorpusError {
    CorpusPosition pos;
    Str msg;
} CorpusError;

/* The token.File a line is parsed as and the scanner.ErrorList its errors go
 * to, which the scanner and the parser share. Positions are token.Pos values:
 * the file is the only one in its set, so a position is its offset plus one,
 * and zero is NoPos. Everything is allocated from a, which the caller frees as
 * a whole.
 *
 * A line of a corpus file never holds a newline, because the file was split on
 * them, so the file has one line and every position on it is on line 1 unless
 * a line directive says otherwise. */
typedef struct CorpusFile {
    Alloc *a;
    Str name;
    Int size;
    CorpusLineInfo *infos;
    Int ninfos;
    Int cap_infos;
    CorpusError *errs;
    Int nerrs;
    Int cap_errs;
    /* The parser has bailed out. go/parser panics for that, which unwinds
     * every frame at once, and here the scanner reports EOF from then on and
     * nothing more is recorded, so that every frame returns on its own
     * without adding to what the panic would have left. */
    bool bailed;
} CorpusFile;

void *burrow__testing_corpus_alloc(Alloc *a, size_t size);
CorpusPosition burrow__testing_corpus_position(const CorpusFile *f, Int pos);
/* ErrorList.Add, at the Position of pos as it is now. */
void burrow__testing_corpus_error_add(CorpusFile *f, Int pos, Str msg);

/* go/scanner's Scanner, in ScanComments mode, which is the one go/parser
 * uses. */
typedef struct CorpusScanner {
    CorpusFile *file;
    Str src;
    int32_t ch;
    Int offset;
    Int rd_offset;
    bool insert_semi;
} CorpusScanner;

void burrow__testing_corpus_scan_init(CorpusScanner *s, CorpusFile *file, Str src);
CorpusTok burrow__testing_corpus_scan(CorpusScanner *s, Int *pos, Str *lit);
/* Scanner.End. */
Int burrow__testing_corpus_scan_end(const CorpusScanner *s);

/* The go/ast nodes ParseExprFrom can build, as one struct. Expressions keep
 * what parseCorpusValue reads and what go/parser looks back at while it
 * recovers from an error. Statements are only ever inside a function literal,
 * which is never a value, so they keep just enough for the parser. */
typedef enum CorpusNodeKind {
    CN_BAD,
    CN_IDENT,
    CN_BASIC_LIT,
    CN_COMPOSITE_LIT,
    CN_PAREN,
    CN_SELECTOR,
    CN_INDEX,
    CN_INDEX_LIST,
    CN_SLICE,
    CN_TYPE_ASSERT,
    CN_CALL,
    CN_STAR,
    CN_UNARY,
    CN_BINARY,
    CN_KEY_VALUE,
    CN_ARRAY_TYPE,
    CN_STRUCT_TYPE,
    CN_FUNC_TYPE,
    CN_INTERFACE_TYPE,
    CN_MAP_TYPE,
    CN_CHAN_TYPE,
    CN_ELLIPSIS,
    CN_FUNC_LIT,

    CN_STMT_EXPR,
    CN_STMT_ASSIGN,
    CN_STMT_LABELED,
    CN_STMT_OTHER
} CorpusNodeKind;

typedef struct CorpusNode CorpusNode;

struct CorpusNode {
    CorpusNodeKind kind;
    /* Pos() and End(). */
    Int pos;
    Int end;
    /* BasicLit's Kind, UnaryExpr's and BinaryExpr's Op, AssignStmt's Tok. */
    CorpusTok tok;
    /* Ident's Name, BasicLit's Value. */
    Str name;
    /* X, Fun, Elt, Value or Type, whichever the node has. ExprStmt's X. */
    CorpusNode *x;
    /* BinaryExpr's Y, ArrayType's Len, SelectorExpr's Sel. */
    CorpusNode *y;
    /* CallExpr's Args, IndexListExpr's Indices, AssignStmt's Lhs. */
    CorpusNode **args;
    Int nargs;
    /* AssignStmt's Rhs. */
    CorpusNode **rhs;
    Int nrhs;
    /* BinaryExpr's OpPos, AssignStmt's TokPos, ChanType's Arrow. */
    Int op_pos;
    /* CallExpr's Ellipsis, Lparen and Rparen. */
    Int ellipsis;
    Int lparen;
    Int rparen;
    /* ChanType's Dir. */
    int dir;
};

/* parser.ParseExprFrom(fset, "(test)", line, 0). On failure the answer is
 * NULL and *err is the ErrorList's Error. The tree and the text are allocated
 * from a. */
CorpusNode *burrow__testing_corpus_parse_expr(Alloc *a, Str line, Str *err);

#endif /* BURROW_TESTING_CORPUS_SYNTAX_H */
