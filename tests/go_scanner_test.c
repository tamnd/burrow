/* Derived from Go's src/go/scanner/scanner_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/scanner.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

Str burrow__go_scanner_strip_cr(Alloc *a, Str b, bool comment);

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

enum { CLASS_SPECIAL, CLASS_LITERAL, CLASS_OPERATOR, CLASS_KEYWORD };

static int tokenclass(Token tok) {
    if (token_is_literal(tok))
        return CLASS_LITERAL;
    if (token_is_operator(tok))
        return CLASS_OPERATOR;
    if (token_is_keyword(tok))
        return CLASS_KEYWORD;
    return CLASS_SPECIAL;
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str sub(Str s, Int from, Int to) {
    return str_from_bytes(s.p + from, to - from);
}

static TokenFile *add_file(TokenFileSet *fset, Str name, Int size) {
    return token_file_set_add_file(fset, name, token_file_set_base(fset), size);
}

/* Each test gets an arena for what it allocates and a file set of its own,
 * where Go's share one. */
typedef struct GsEnv {
    Arena ar;
    Alloc *a;
    TokenFileSet *fset;
} GsEnv;

static void env_init(GsEnv *e) {
    arena_init(&e->ar, NULL, 0);
    e->a = arena_allocator(&e->ar);
    e->fset = token_new_file_set(heap_allocator());
}

static void env_free(GsEnv *e) {
    token_file_set_free(e->fset);
    arena_free(&e->ar);
}

typedef struct GsElt {
    Token tok;
    Str lit;
    int cls;
} GsElt;

static const GsElt tokens[] = {
    {TOKEN_COMMENT, S_("/* a comment */"), CLASS_SPECIAL},
    {TOKEN_COMMENT, S_("// a comment \n"), CLASS_SPECIAL},
    {TOKEN_COMMENT, S_("/*\r*/"), CLASS_SPECIAL},
    {TOKEN_COMMENT, S_("/**\r/*/"), CLASS_SPECIAL},
    {TOKEN_COMMENT, S_("/**\r\r/*/"), CLASS_SPECIAL},
    {TOKEN_COMMENT, S_("//\r\n"), CLASS_SPECIAL},
    {TOKEN_IDENT, S_("foobar"), CLASS_LITERAL},
    {TOKEN_IDENT, S_("a۰۱۸"), CLASS_LITERAL},
    {TOKEN_IDENT, S_("foo६४"), CLASS_LITERAL},
    {TOKEN_IDENT, S_("bar９８７６"), CLASS_LITERAL},
    {TOKEN_IDENT, S_("ŝ"), CLASS_LITERAL},
    {TOKEN_IDENT, S_("ŝfoo"), CLASS_LITERAL},
    {TOKEN_INT, S_("0"), CLASS_LITERAL},
    {TOKEN_INT, S_("1"), CLASS_LITERAL},
    {TOKEN_INT, S_("123456789012345678890"), CLASS_LITERAL},
    {TOKEN_INT, S_("01234567"), CLASS_LITERAL},
    {TOKEN_INT, S_("0xcafebabe"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("0."), CLASS_LITERAL},
    {TOKEN_FLOAT, S_(".0"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("3.14159265"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("1e0"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("1e+100"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("1e-100"), CLASS_LITERAL},
    {TOKEN_FLOAT, S_("2.71828e-1000"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("0i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("1i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("012345678901234567889i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("123456789012345678890i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("0.i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_(".0i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("3.14159265i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("1e0i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("1e+100i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("1e-100i"), CLASS_LITERAL},
    {TOKEN_IMAG, S_("2.71828e-1000i"), CLASS_LITERAL},
    {TOKEN_CHAR, S_("'a'"), CLASS_LITERAL},
    {TOKEN_CHAR, S_("'\\000'"), CLASS_LITERAL},
    {TOKEN_CHAR, S_("'\\xFF'"), CLASS_LITERAL},
    {TOKEN_CHAR, S_("'\\uff16'"), CLASS_LITERAL},
    {TOKEN_CHAR, S_("'\\U0000ff16'"), CLASS_LITERAL},
    {TOKEN_STRING, S_("`foobar`"), CLASS_LITERAL},
    {TOKEN_STRING, S_("`foo\n\t                        bar`"), CLASS_LITERAL},
    {TOKEN_STRING, S_("`\r`"), CLASS_LITERAL},
    {TOKEN_STRING, S_("`foo\r\nbar`"), CLASS_LITERAL},
    {TOKEN_ADD, S_("+"), CLASS_OPERATOR},
    {TOKEN_SUB, S_("-"), CLASS_OPERATOR},
    {TOKEN_MUL, S_("*"), CLASS_OPERATOR},
    {TOKEN_QUO, S_("/"), CLASS_OPERATOR},
    {TOKEN_REM, S_("%"), CLASS_OPERATOR},
    {TOKEN_AND, S_("&"), CLASS_OPERATOR},
    {TOKEN_OR, S_("|"), CLASS_OPERATOR},
    {TOKEN_XOR, S_("^"), CLASS_OPERATOR},
    {TOKEN_SHL, S_("<<"), CLASS_OPERATOR},
    {TOKEN_SHR, S_(">>"), CLASS_OPERATOR},
    {TOKEN_AND_NOT, S_("&^"), CLASS_OPERATOR},
    {TOKEN_ADD_ASSIGN, S_("+="), CLASS_OPERATOR},
    {TOKEN_SUB_ASSIGN, S_("-="), CLASS_OPERATOR},
    {TOKEN_MUL_ASSIGN, S_("*="), CLASS_OPERATOR},
    {TOKEN_QUO_ASSIGN, S_("/="), CLASS_OPERATOR},
    {TOKEN_REM_ASSIGN, S_("%="), CLASS_OPERATOR},
    {TOKEN_AND_ASSIGN, S_("&="), CLASS_OPERATOR},
    {TOKEN_OR_ASSIGN, S_("|="), CLASS_OPERATOR},
    {TOKEN_XOR_ASSIGN, S_("^="), CLASS_OPERATOR},
    {TOKEN_SHL_ASSIGN, S_("<<="), CLASS_OPERATOR},
    {TOKEN_SHR_ASSIGN, S_(">>="), CLASS_OPERATOR},
    {TOKEN_AND_NOT_ASSIGN, S_("&^="), CLASS_OPERATOR},
    {TOKEN_LAND, S_("&&"), CLASS_OPERATOR},
    {TOKEN_LOR, S_("||"), CLASS_OPERATOR},
    {TOKEN_ARROW, S_("<-"), CLASS_OPERATOR},
    {TOKEN_INC, S_("++"), CLASS_OPERATOR},
    {TOKEN_DEC, S_("--"), CLASS_OPERATOR},
    {TOKEN_EQL, S_("=="), CLASS_OPERATOR},
    {TOKEN_LSS, S_("<"), CLASS_OPERATOR},
    {TOKEN_GTR, S_(">"), CLASS_OPERATOR},
    {TOKEN_ASSIGN, S_("="), CLASS_OPERATOR},
    {TOKEN_NOT, S_("!"), CLASS_OPERATOR},
    {TOKEN_NEQ, S_("!="), CLASS_OPERATOR},
    {TOKEN_LEQ, S_("<="), CLASS_OPERATOR},
    {TOKEN_GEQ, S_(">="), CLASS_OPERATOR},
    {TOKEN_DEFINE, S_(":="), CLASS_OPERATOR},
    {TOKEN_ELLIPSIS, S_("..."), CLASS_OPERATOR},
    {TOKEN_LPAREN, S_("("), CLASS_OPERATOR},
    {TOKEN_LBRACK, S_("["), CLASS_OPERATOR},
    {TOKEN_LBRACE, S_("{"), CLASS_OPERATOR},
    {TOKEN_COMMA, S_(","), CLASS_OPERATOR},
    {TOKEN_PERIOD, S_("."), CLASS_OPERATOR},
    {TOKEN_RPAREN, S_(")"), CLASS_OPERATOR},
    {TOKEN_RBRACK, S_("]"), CLASS_OPERATOR},
    {TOKEN_RBRACE, S_("}"), CLASS_OPERATOR},
    {TOKEN_SEMICOLON, S_(";"), CLASS_OPERATOR},
    {TOKEN_COLON, S_(":"), CLASS_OPERATOR},
    {TOKEN_TILDE, S_("~"), CLASS_OPERATOR},
    {TOKEN_BREAK, S_("break"), CLASS_KEYWORD},
    {TOKEN_CASE, S_("case"), CLASS_KEYWORD},
    {TOKEN_CHAN, S_("chan"), CLASS_KEYWORD},
    {TOKEN_CONST, S_("const"), CLASS_KEYWORD},
    {TOKEN_CONTINUE, S_("continue"), CLASS_KEYWORD},
    {TOKEN_DEFAULT, S_("default"), CLASS_KEYWORD},
    {TOKEN_DEFER, S_("defer"), CLASS_KEYWORD},
    {TOKEN_ELSE, S_("else"), CLASS_KEYWORD},
    {TOKEN_FALLTHROUGH, S_("fallthrough"), CLASS_KEYWORD},
    {TOKEN_FOR, S_("for"), CLASS_KEYWORD},
    {TOKEN_FUNC, S_("func"), CLASS_KEYWORD},
    {TOKEN_GO, S_("go"), CLASS_KEYWORD},
    {TOKEN_GOTO, S_("goto"), CLASS_KEYWORD},
    {TOKEN_IF, S_("if"), CLASS_KEYWORD},
    {TOKEN_IMPORT, S_("import"), CLASS_KEYWORD},
    {TOKEN_INTERFACE, S_("interface"), CLASS_KEYWORD},
    {TOKEN_MAP, S_("map"), CLASS_KEYWORD},
    {TOKEN_PACKAGE, S_("package"), CLASS_KEYWORD},
    {TOKEN_RANGE, S_("range"), CLASS_KEYWORD},
    {TOKEN_RETURN, S_("return"), CLASS_KEYWORD},
    {TOKEN_SELECT, S_("select"), CLASS_KEYWORD},
    {TOKEN_STRUCT, S_("struct"), CLASS_KEYWORD},
    {TOKEN_SWITCH, S_("switch"), CLASS_KEYWORD},
    {TOKEN_TYPE_, S_("type"), CLASS_KEYWORD},
    {TOKEN_VAR, S_("var"), CLASS_KEYWORD},
};

static const Str whitespace = S_("  \t  \n\n\n"); /* to separate tokens */

static Str make_source(Alloc *a) {
    Int n = 0;
    for (Int i = 0; i < NELEM(tokens); i++)
        n += tokens[i].lit.len + whitespace.len;
    Byte *p = (Byte *)mem_alloc(a, (size_t)n, 1);
    Int at = 0;
    for (Int i = 0; i < NELEM(tokens); i++) {
        memcpy(p + at, tokens[i].lit.p, (size_t)tokens[i].lit.len);
        at += tokens[i].lit.len;
        memcpy(p + at, whitespace.p, (size_t)whitespace.len);
        at += whitespace.len;
    }
    return str_from_bytes(p, n);
}

static Int newline_count(Str s) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == '\n')
            n++;
    return n;
}

static void check_pos(TestingT *t, GsEnv *e, Str lit, TokenPos p,
                      TokenPosition expected) {
    TokenPosition pos = token_file_set_position(e->fset, p);
    /* Check cleaned filenames so that we don't have to worry about different
     * os.PathSeparator values. */
    if (!str_eq(pos.filename, expected.filename) &&
        !str_eq(filepath_clean(e->a, pos.filename),
                filepath_clean(e->a, expected.filename)))
        testing_t_errorf_v(t, "bad filename for %q: got %s, expected %s", lit,
                           pos.filename, expected.filename);
    if (pos.offset != expected.offset)
        testing_t_errorf_v(t, "bad position for %q: got %d, expected %d", lit,
                           pos.offset, expected.offset);
    if (pos.line != expected.line)
        testing_t_errorf_v(t, "bad line for %q: got %d, expected %d", lit, pos.line,
                           expected.line);
    if (pos.column != expected.column)
        testing_t_errorf_v(t, "bad column for %q: got %d, expected %d", lit, pos.column,
                           expected.column);
}

static void error_called(void *env, TokenPosition pos, Str msg) {
    (void)pos;
    testing_t_errorf_v((TestingT *)env, "error handler called (msg = %s)", msg);
}

/* Verify that calling Scan() provides the correct results. */
static void TestScan(TestingT *t) {
    GsEnv e;
    env_init(&e);
    Str source = make_source(e.a);
    Int whitespace_linecount = newline_count(whitespace);

    /* verify scan */
    GoScanner s;
    go_scanner_init(&s, e.a, add_file(e.fset, BURROW_S(""), source.len),
                    bytes_of(source), BURROW_FN(GoScannerErrorHandler, error_called, t),
                    GO_SCANNER_SCAN_COMMENTS | BURROW__GO_SCANNER_DONT_INSERT_SEMIS);

    /* set up expected position */
    TokenPosition epos = {BURROW_STR_EMPTY, 0, 1, 1};

    Int index = 0;
    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);

        /* check position */
        if (tok == TOKEN_EOF) {
            /* correction for EOF */
            epos.line = newline_count(source);
            epos.column = 2;
        }
        check_pos(t, &e, lit, pos, epos);

        /* check token */
        GsElt el = {TOKEN_EOF, BURROW_S_INIT(""), CLASS_SPECIAL};
        if (index < NELEM(tokens)) {
            el = tokens[index];
            index++;
        }
        if (tok != el.tok)
            testing_t_errorf_v(t, "bad token for %q: got %s, expected %s", lit,
                               token_string(tok, e.a), token_string(el.tok, e.a));

        /* check token class */
        if (tokenclass(tok) != el.cls)
            testing_t_errorf_v(t, "bad class for %q: got %d, expected %d", lit,
                               tokenclass(tok), el.cls);

        /* check literal */
        Str elit = BURROW_STR_EMPTY;
        switch (el.tok) {
        case TOKEN_COMMENT:
            /* no CRs in comments */
            elit = burrow__go_scanner_strip_cr(e.a, el.lit, el.lit.p[1] == '*');
            /* //-style comment literal doesn't contain newline */
            if (elit.p[1] == '/')
                elit.len--;
            break;
        case TOKEN_IDENT:
            elit = el.lit;
            break;
        case TOKEN_SEMICOLON:
            elit = BURROW_S(";");
            break;
        default:
            if (token_is_literal(el.tok)) {
                /* no CRs in raw string literals */
                elit = el.lit;
                if (elit.p[0] == '`')
                    elit = burrow__go_scanner_strip_cr(e.a, elit, false);
            } else if (token_is_keyword(el.tok)) {
                elit = el.lit;
            }
            break;
        }
        if (!str_eq(lit, elit))
            testing_t_errorf_v(t, "bad literal for %q: got %q, expected %q", lit, lit,
                               elit);

        if (tok == TOKEN_EOF)
            break;

        /* update position */
        epos.offset += el.lit.len + whitespace.len;
        epos.line += newline_count(el.lit) + whitespace_linecount;
    }

    if (s.error_count != 0)
        testing_t_errorf_v(t, "found %d errors", s.error_count);
    env_free(&e);
}

typedef struct GsStripTest {
    Str have;
    Str want;
} GsStripTest;

static const GsStripTest strip_cr_tests[] = {
    {S_("//\n"), S_("//\n")},
    {S_("//\r\n"), S_("//\n")},
    {S_("//\r\r\r\n"), S_("//\n")},
    {S_("//\r*\r/\r\n"), S_("//*/\n")},
    {S_("/**/"), S_("/**/")},
    {S_("/*\r/*/"), S_("/*/*/")},
    {S_("/*\r*/"), S_("/**/")},
    {S_("/**\r/*/"), S_("/**\r/*/")},
    {S_("/*\r/\r*\r/*/"), S_("/*/*\r/*/")},
    {S_("/*\r\r\r\r*/"), S_("/**/")},
};

static void TestStripCR(TestingT *t) {
    GsEnv e;
    env_init(&e);
    for (Int i = 0; i < NELEM(strip_cr_tests); i++) {
        const GsStripTest *test = &strip_cr_tests[i];
        Str got = burrow__go_scanner_strip_cr(
            e.a, test->have, test->have.len >= 2 && test->have.p[1] == '*');
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "stripCR(%q) = %q; want %q", test->have, got,
                               test->want);
    }
    env_free(&e);
}

static void check_semi(TestingT *t, GsEnv *e, Str input, Str want, GoScannerMode mode) {
    if ((mode & GO_SCANNER_SCAN_COMMENTS) == 0) {
        want = strings_replace_all(e->a, want, BURROW_S("COMMENT "), BURROW_S(""));
        want = strings_replace_all(e->a, want, BURROW_S(" COMMENT"),
                                   BURROW_S("")); /* if at end */
        want = strings_replace_all(e->a, want, BURROW_S("COMMENT"),
                                   BURROW_S("")); /* if sole token */
    }

    TokenFile *file = add_file(e->fset, BURROW_S("TestSemis"), input.len);
    GoScanner scan;
    go_scanner_init(&scan, e->a, file, bytes_of(input), (GoScannerErrorHandler){0},
                    mode);
    Str got = BURROW_STR_EMPTY;
    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&scan, &tok, &lit);
        if (tok == TOKEN_EOF)
            break;
        if (tok == TOKEN_SEMICOLON && !str_eq(lit, BURROW_S(";"))) {
            /* Artificial semicolon: assert that position is EOF or that of a
             * newline. */
            Int off = token_file_offset(file, pos);
            if (off != input.len && input.p[off] != '\n')
                testing_t_errorf_v(t,
                                   "scanning <<%s>>, got SEMICOLON at offset %d, want "
                                   "newline or EOF",
                                   input, off);
        }
        lit = token_string(tok, e->a); /* "\n" => ";" */
        got = got.len == 0 ? lit : fmt_sprintf_v(e->a, "%s %s", got, lit);
    }
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "scanning <<%s>>, got [%s], want [%s]", input, got, want);
}

typedef struct GsSemiTest {
    Str input;
    Str want;
} GsSemiTest;

static const GsSemiTest semicolon_tests[] = {
    {S_(""), S_("")},
    {S_("\xEF\xBB\xBF;"), S_(";")},
    {S_(";"), S_(";")},
    {S_("foo\n"), S_("IDENT ;")},
    {S_("123\n"), S_("INT ;")},
    {S_("1.2\n"), S_("FLOAT ;")},
    {S_("'x'\n"), S_("CHAR ;")},
    {S_("\"x\"\n"), S_("STRING ;")},
    {S_("`x`\n"), S_("STRING ;")},
    {S_("+\n"), S_("+")},
    {S_("-\n"), S_("-")},
    {S_("*\n"), S_("*")},
    {S_("/\n"), S_("/")},
    {S_("%\n"), S_("%")},
    {S_("&\n"), S_("&")},
    {S_("|\n"), S_("|")},
    {S_("^\n"), S_("^")},
    {S_("<<\n"), S_("<<")},
    {S_(">>\n"), S_(">>")},
    {S_("&^\n"), S_("&^")},
    {S_("+=\n"), S_("+=")},
    {S_("-=\n"), S_("-=")},
    {S_("*=\n"), S_("*=")},
    {S_("/=\n"), S_("/=")},
    {S_("%=\n"), S_("%=")},
    {S_("&=\n"), S_("&=")},
    {S_("|=\n"), S_("|=")},
    {S_("^=\n"), S_("^=")},
    {S_("<<=\n"), S_("<<=")},
    {S_(">>=\n"), S_(">>=")},
    {S_("&^=\n"), S_("&^=")},
    {S_("&&\n"), S_("&&")},
    {S_("||\n"), S_("||")},
    {S_("<-\n"), S_("<-")},
    {S_("++\n"), S_("++ ;")},
    {S_("--\n"), S_("-- ;")},
    {S_("==\n"), S_("==")},
    {S_("<\n"), S_("<")},
    {S_(">\n"), S_(">")},
    {S_("=\n"), S_("=")},
    {S_("!\n"), S_("!")},
    {S_("!=\n"), S_("!=")},
    {S_("<=\n"), S_("<=")},
    {S_(">=\n"), S_(">=")},
    {S_(":=\n"), S_(":=")},
    {S_("...\n"), S_("...")},
    {S_("(\n"), S_("(")},
    {S_("[\n"), S_("[")},
    {S_("{\n"), S_("{")},
    {S_(",\n"), S_(",")},
    {S_(".\n"), S_(".")},
    {S_(")\n"), S_(") ;")},
    {S_("]\n"), S_("] ;")},
    {S_("}\n"), S_("} ;")},
    {S_(";\n"), S_(";")},
    {S_(":\n"), S_(":")},
    {S_("break\n"), S_("break ;")},
    {S_("case\n"), S_("case")},
    {S_("chan\n"), S_("chan")},
    {S_("const\n"), S_("const")},
    {S_("continue\n"), S_("continue ;")},
    {S_("default\n"), S_("default")},
    {S_("defer\n"), S_("defer")},
    {S_("else\n"), S_("else")},
    {S_("fallthrough\n"), S_("fallthrough ;")},
    {S_("for\n"), S_("for")},
    {S_("func\n"), S_("func")},
    {S_("go\n"), S_("go")},
    {S_("goto\n"), S_("goto")},
    {S_("if\n"), S_("if")},
    {S_("import\n"), S_("import")},
    {S_("interface\n"), S_("interface")},
    {S_("map\n"), S_("map")},
    {S_("package\n"), S_("package")},
    {S_("range\n"), S_("range")},
    {S_("return\n"), S_("return ;")},
    {S_("select\n"), S_("select")},
    {S_("struct\n"), S_("struct")},
    {S_("switch\n"), S_("switch")},
    {S_("type\n"), S_("type")},
    {S_("var\n"), S_("var")},
    {S_("foo//comment\n"), S_("IDENT COMMENT ;")},
    {S_("foo//comment"), S_("IDENT COMMENT ;")},
    {S_("foo/*comment*/\n"), S_("IDENT COMMENT ;")},
    {S_("foo/*\n*/"), S_("IDENT COMMENT ;")},
    {S_("foo/*comment*/    \n"), S_("IDENT COMMENT ;")},
    {S_("foo/*\n*/    "), S_("IDENT COMMENT ;")},
    {S_("foo    // comment\n"), S_("IDENT COMMENT ;")},
    {S_("foo    // comment"), S_("IDENT COMMENT ;")},
    {S_("foo    /*comment*/\n"), S_("IDENT COMMENT ;")},
    {S_("foo    /*\n*/"), S_("IDENT COMMENT ;")},
    {S_("foo    /*  */ /* \n */ bar/**/\n"),
     S_("IDENT COMMENT COMMENT ; IDENT COMMENT ;")},
    {S_("foo    /*0*/ /*1*/ /*2*/\n"), S_("IDENT COMMENT COMMENT COMMENT ;")},
    {S_("foo    /*comment*/    \n"), S_("IDENT COMMENT ;")},
    {S_("foo    /*0*/ /*1*/ /*2*/    \n"), S_("IDENT COMMENT COMMENT COMMENT ;")},
    {S_("foo\t/**/ /*-------------*/       /*----\n*/bar       /*  \n*/baa\n"),
     S_("IDENT COMMENT COMMENT COMMENT ; IDENT COMMENT ; IDENT ;")},
    {S_("foo    /* an EOF terminates a line */"), S_("IDENT COMMENT ;")},
    {S_("foo    /* an EOF terminates a line */ /*"), S_("IDENT COMMENT COMMENT ;")},
    {S_("foo    /* an EOF terminates a line */ //"), S_("IDENT COMMENT COMMENT ;")},
    {S_("package main\n\nfunc main() {\n\tif {\n\t\treturn /* */ }\n}\n"),
     S_("package IDENT ; func IDENT ( ) { if { return COMMENT } ; } ;")},
    {S_("package main"), S_("package IDENT ;")},
};

static void TestSemicolons(TestingT *t) {
    GsEnv e;
    env_init(&e);
    for (Int k = 0; k < NELEM(semicolon_tests); k++) {
        Str input = semicolon_tests[k].input;
        Str want = semicolon_tests[k].want;
        check_semi(t, &e, input, want, 0);
        check_semi(t, &e, input, want, GO_SCANNER_SCAN_COMMENTS);

        /* if the input ended in newlines, the input must tokenize the same
         * with or without those newlines */
        for (Int i = input.len - 1; i >= 0 && input.p[i] == '\n'; i--) {
            check_semi(t, &e, sub(input, 0, i), want, 0);
            check_semi(t, &e, sub(input, 0, i), want, GO_SCANNER_SCAN_COMMENTS);
        }
    }
    env_free(&e);
}

typedef struct GsSegment {
    Str srcline;  /* a line of source text */
    Str filename; /* filename for current token; error message for invalid line
                     directives */
    Int line;     /* line for current token; error position for invalid line
                     directives */
    Int column;   /* column, likewise */
} GsSegment;

static const GsSegment segments[] = {
    /* exactly one token per line since the test consumes one token per segment */
    {S_("  line1"), S_("TestLineDirectives"), 1, 3},
    {S_("\nline2"), S_("TestLineDirectives"), 2, 1},
    {S_("\nline3  //line File1.go:100"), S_("TestLineDirectives"), 3, 1},
    {S_("\nline4"), S_("TestLineDirectives"), 4, 1},
    {S_("\n//line File1.go:100\n  line100"), S_("File1.go"), 100, 0},
    {S_("\n//line  \t :42\n  line1"), S_(" \t "), 42, 0},
    {S_("\n//line File2.go:200\n  line200"), S_("File2.go"), 200, 0},
    {S_("\n//line foo\t:42\n  line42"), S_("foo\t"), 42, 0},
    {S_("\n //line foo:42\n  line43"), S_("foo\t"), 44, 0},
    {S_("\n//line foo 42\n  line44"), S_("foo\t"), 46, 0},
    {S_("\n//line /bar:42\n  line45"), S_("/bar"), 42, 0},
    {S_("\n//line ./foo:42\n  line46"), S_("foo"), 42, 0},
    {S_("\n//line a/b/c/File1.go:100\n  line100"), S_("a/b/c/File1.go"), 100, 0},
    {S_("\n//line c:\\bar:42\n  line200"), S_("c:\\bar"), 42, 0},
    {S_("\n//line c:\\dir\\File1.go:100\n  line201"), S_("c:\\dir\\File1.go"), 100, 0},
    {S_("\n//line :100\na1"), S_(""), 100, 0},
    {S_("\n//line bar:100\nb1"), S_("bar"), 100, 0},
    {S_("\n//line :100:10\nc1"), S_("bar"), 100, 10},
    {S_("\n//line foo:100:10\nd1"), S_("foo"), 100, 10},
    {S_("\n/*line :100*/a2"), S_(""), 100, 0},
    {S_("\n/*line bar:100*/b2"), S_("bar"), 100, 0},
    {S_("\n/*line :100:10*/c2"), S_("bar"), 100, 10},
    {S_("\n/*line foo:100:10*/d2"), S_("foo"), 100, 10},
    {S_("\n/*line foo:100:10*/    e2"), S_("foo"), 100, 14},
    {S_("\n/*line foo:100:10*/\n\nf2"), S_("foo"), 102, 1},
};

static const GsSegment dirsegments[] = {
    /* exactly one token per line since the test consumes one token per segment */
    {S_("  line1"), S_("TestLineDir/TestLineDirectives"), 1, 3},
    {S_("\n//line File1.go:100\n  line100"), S_("TestLineDir/File1.go"), 100, 0},
};

#ifdef _WIN32
static const GsSegment dir_windows_segments[] = {
    {S_("\n//line c:\\bar:42\n  line42"), S_("c:\\bar"), 42, 0},
};
#else
static const GsSegment dir_unix_segments[] = {
    {S_("\n//line /bar:42\n  line42"), S_("/bar"), 42, 0},
};
#endif

typedef struct GsSegEnv {
    TestingT *t;
    Alloc *a;
} GsSegEnv;

static void segment_error(void *env, TokenPosition pos, Str msg) {
    GsSegEnv *se = (GsSegEnv *)env;
    GoScannerError err = {pos, msg};
    testing_t_error_v(se->t, go_scanner_error_error(err, se->a));
}

static Str join_srclines(Alloc *a, const GsSegment *segs, Int n) {
    Str src = BURROW_STR_EMPTY;
    for (Int i = 0; i < n; i++)
        src = fmt_sprintf_v(a, "%s%s", src, segs[i].srcline);
    return src;
}

static void test_segments(TestingT *t, const GsSegment *segs, Int n, Str filename) {
    GsEnv e;
    env_init(&e);
    Str src = join_srclines(e.a, segs, n);

    /* verify scan */
    GoScanner S;
    GsSegEnv se = {t, e.a};
    TokenFile *file = add_file(e.fset, filename, src.len);
    go_scanner_init(&S, e.a, file, bytes_of(src),
                    BURROW_FN(GoScannerErrorHandler, segment_error, &se),
                    BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    for (Int i = 0; i < n; i++) {
        Str lit = BURROW_STR_EMPTY;
        TokenPos p = go_scanner_scan(&S, NULL, &lit);
        TokenPosition pos = token_file_position(file, p);
        TokenPosition want = {segs[i].filename, pos.offset, segs[i].line,
                              segs[i].column};
        check_pos(t, &e, lit, p, want);
    }

    if (S.error_count != 0)
        testing_t_errorf_v(t, "got %d errors", S.error_count);
    env_free(&e);
}

/* Verify that line directives are interpreted correctly. */
static void TestLineDirectives(TestingT *t) {
    test_segments(t, segments, NELEM(segments), BURROW_S("TestLineDirectives"));
    test_segments(t, dirsegments, NELEM(dirsegments),
                  BURROW_S("TestLineDir/TestLineDirectives"));
#ifdef _WIN32
    test_segments(t, dir_windows_segments, NELEM(dir_windows_segments),
                  BURROW_S("TestLineDir/TestLineDirectives"));
#else
    test_segments(t, dir_unix_segments, NELEM(dir_unix_segments),
                  BURROW_S("TestLineDir/TestLineDirectives"));
#endif
}

/* The filename is used for the error message in these test cases. The first
 * line directive is valid and used to control the expected error line. */
static const GsSegment invalid_segments[] = {
    {S_("\n//line :1:1\n//line foo:42 extra text\ndummy"),
     S_("invalid line number: 42 extra text"), 1, 12},
    {S_("\n//line :2:1\n//line foobar:\ndummy"), S_("invalid line number: "), 2, 15},
    {S_("\n//line :5:1\n//line :0\ndummy"), S_("invalid line number: 0"), 5, 9},
    {S_("\n//line :10:1\n//line :1:0\ndummy"), S_("invalid column number: 0"), 10, 11},
    {S_("\n//line :1:1\n//line :foo:0\ndummy"), S_("invalid line number: 0"), 1, 13},
};

typedef struct GsInvalidEnv {
    TestingT *t;
    const GsSegment *s; /* current segment */
} GsInvalidEnv;

static void invalid_error(void *env, TokenPosition pos, Str msg) {
    GsInvalidEnv *ie = (GsInvalidEnv *)env;
    if (!str_eq(msg, ie->s->filename))
        testing_t_errorf_v(ie->t, "got error %q; want %q", msg, ie->s->filename);
    if (pos.line != ie->s->line || pos.column != ie->s->column)
        testing_t_errorf_v(ie->t, "got position %d:%d; want %d:%d", pos.line,
                           pos.column, ie->s->line, ie->s->column);
}

/* Verify that invalid line directives get the correct error message. */
static void TestInvalidLineDirectives(TestingT *t) {
    GsEnv e;
    env_init(&e);
    /* make source */
    Str src = join_srclines(e.a, invalid_segments, NELEM(invalid_segments));

    /* verify scan */
    GoScanner S;
    GsInvalidEnv ie = {t, &invalid_segments[0]};
    TokenFile *file = add_file(
        e.fset,
        filepath_join_v(e.a, 2, BURROW_S("dir"), BURROW_S("TestInvalidLineDirectives")),
        src.len);
    go_scanner_init(&S, e.a, file, bytes_of(src),
                    BURROW_FN(GoScannerErrorHandler, invalid_error, &ie),
                    BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    for (Int i = 0; i < NELEM(invalid_segments); i++) {
        ie.s = &invalid_segments[i];
        go_scanner_scan(&S, NULL, NULL);
    }

    if (S.error_count != NELEM(invalid_segments))
        testing_t_errorf_v(t, "got %d errors; want %d", S.error_count,
                           NELEM(invalid_segments));
    env_free(&e);
}

/* Verify that initializing the same scanner more than once works correctly. */
static void TestInit(TestingT *t) {
    GsEnv e;
    env_init(&e);
    GoScanner s;
    Token tok = TOKEN_ILLEGAL;

    /* 1st init */
    Str src1 = BURROW_S("if true { }");
    TokenFile *f1 = add_file(e.fset, BURROW_S("src1"), src1.len);
    go_scanner_init(&s, e.a, f1, bytes_of(src1), (GoScannerErrorHandler){0},
                    BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    if (token_file_size(f1) != src1.len)
        testing_t_errorf_v(t, "bad file size: got %d, expected %d", token_file_size(f1),
                           src1.len);
    go_scanner_scan(&s, NULL, NULL); /* if */
    go_scanner_scan(&s, NULL, NULL); /* true */
    go_scanner_scan(&s, &tok, NULL); /* { */
    if (tok != TOKEN_LBRACE)
        testing_t_errorf_v(t, "bad token: got %s, expected %s", token_string(tok, e.a),
                           token_string(TOKEN_LBRACE, e.a));

    /* 2nd init */
    Str src2 = BURROW_S("go true { ]");
    TokenFile *f2 = add_file(e.fset, BURROW_S("src2"), src2.len);
    go_scanner_init(&s, e.a, f2, bytes_of(src2), (GoScannerErrorHandler){0},
                    BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    if (token_file_size(f2) != src2.len)
        testing_t_errorf_v(t, "bad file size: got %d, expected %d", token_file_size(f2),
                           src2.len);
    go_scanner_scan(&s, &tok, NULL); /* go */
    if (tok != TOKEN_GO)
        testing_t_errorf_v(t, "bad token: got %s, expected %s", token_string(tok, e.a),
                           token_string(TOKEN_GO, e.a));

    if (s.error_count != 0)
        testing_t_errorf_v(t, "found %d errors", s.error_count);
    env_free(&e);
}

typedef struct GsListEnv {
    GoScannerErrorList *list;
    Alloc *a;
} GsListEnv;

static void list_add(void *env, TokenPosition pos, Str msg) {
    GsListEnv *le = (GsListEnv *)env;
    go_scanner_error_list_add(le->list, le->a, pos, msg);
}

static void TestStdErrorHandler(TestingT *t) {
    GsEnv e;
    env_init(&e);
    Str src = BURROW_S("@\n"               /* illegal character, cause an error */
                       "@ @\n"             /* two errors on the same line */
                       "//line File2:20\n" /* */
                       "@\n"               /* different file, but same line */
                       "//line File2:1\n"  /* */
                       "@ @\n"             /* same file, decreasing line number */
                       "//line File1:1\n"  /* */
                       "@ @ @");           /* original file, line 1 again */

    GoScannerErrorList list = {NULL, 0, 0, NULL};
    GsListEnv le = {&list, e.a};

    GoScanner s;
    go_scanner_init(&s, e.a, add_file(e.fset, BURROW_S("File1"), src.len),
                    bytes_of(src), BURROW_FN(GoScannerErrorHandler, list_add, &le),
                    BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        go_scanner_scan(&s, &tok, NULL);
        if (tok == TOKEN_EOF)
            break;
    }

    IoWriter stderr_w = os_file_as_io_writer(os_stderr);
    Error err = go_scanner_error_list_err(list);

    if (list.len != s.error_count)
        testing_t_errorf_v(t, "found %d errors, expected %d", list.len, s.error_count);

    if (list.len != 9) {
        testing_t_errorf_v(t, "found %d raw errors, expected 9", list.len);
        go_scanner_print_error(stderr_w, err);
    }

    go_scanner_error_list_sort(list);
    if (list.len != 9) {
        testing_t_errorf_v(t, "found %d sorted errors, expected 9", list.len);
        go_scanner_print_error(stderr_w, err);
    }

    go_scanner_error_list_remove_multiples(&list);
    if (list.len != 4) {
        testing_t_errorf_v(t, "found %d one-per-line errors, expected 4", list.len);
        go_scanner_print_error(stderr_w, go_scanner_error_list_err(list));
    }
    env_free(&e);
}

typedef struct GsErrorCollector {
    Int cnt;           /* number of errors encountered */
    Str msg;           /* last error message encountered */
    TokenPosition pos; /* last error position encountered */
    Alloc *a;
} GsErrorCollector;

static void collect_error(void *env, TokenPosition pos, Str msg) {
    GsErrorCollector *h = (GsErrorCollector *)env;
    h->cnt++;
    h->msg = str_clone(h->a, msg);
    h->pos = pos;
}

static void check_error(TestingT *t, GsEnv *e, Str src, Token tok, Int pos, Str lit,
                        Str err) {
    GoScanner s;
    GsErrorCollector h = {0, BURROW_STR_EMPTY, {BURROW_STR_EMPTY, 0, 0, 0}, e->a};
    go_scanner_init(&s, e->a, add_file(e->fset, BURROW_S(""), src.len), bytes_of(src),
                    BURROW_FN(GoScannerErrorHandler, collect_error, &h),
                    GO_SCANNER_SCAN_COMMENTS | BURROW__GO_SCANNER_DONT_INSERT_SEMIS);
    Token tok0 = TOKEN_ILLEGAL;
    Str lit0 = BURROW_STR_EMPTY;
    go_scanner_scan(&s, &tok0, &lit0);
    if (tok0 != tok)
        testing_t_errorf_v(t, "%q: got %s, expected %s", src, token_string(tok0, e->a),
                           token_string(tok, e->a));
    if (tok0 != TOKEN_ILLEGAL && !str_eq(lit0, lit))
        testing_t_errorf_v(t, "%q: got literal %q, expected %q", src, lit0, lit);
    Int cnt = 0;
    if (err.len > 0)
        cnt = 1;
    if (h.cnt != cnt)
        testing_t_errorf_v(t, "%q: got cnt %d, expected %d", src, h.cnt, cnt);
    if (!str_eq(h.msg, err))
        testing_t_errorf_v(t, "%q: got msg %q, expected %q", src, h.msg, err);
    if (h.pos.offset != pos)
        testing_t_errorf_v(t, "%q: got offset %d, expected %d", src, h.pos.offset, pos);
}

typedef struct GsErrTest {
    Str src;
    Token tok;
    Int pos;
    Str lit;
    Str err;
} GsErrTest;

static const GsErrTest error_tests[] = {
    {S_("\a"), TOKEN_ILLEGAL, 0, S_(""), S_("illegal character U+0007")},
    {S_("#"), TOKEN_ILLEGAL, 0, S_(""), S_("illegal character U+0023 '#'")},
    {S_("…"), TOKEN_ILLEGAL, 0, S_(""), S_("illegal character U+2026 '…'")},
    {S_(".."), TOKEN_PERIOD, 0, S_(""), S_("")},
    {S_("' '"), TOKEN_CHAR, 0, S_("' '"), S_("")},
    {S_("''"), TOKEN_CHAR, 0, S_("''"), S_("illegal rune literal")},
    {S_("'12'"), TOKEN_CHAR, 0, S_("'12'"), S_("illegal rune literal")},
    {S_("'123'"), TOKEN_CHAR, 0, S_("'123'"), S_("illegal rune literal")},
    {S_("'\\0'"), TOKEN_CHAR, 3, S_("'\\0'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\07'"), TOKEN_CHAR, 4, S_("'\\07'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\8'"), TOKEN_CHAR, 2, S_("'\\8'"), S_("unknown escape sequence")},
    {S_("'\\08'"), TOKEN_CHAR, 3, S_("'\\08'"),
     S_("illegal character U+0038 '8' in escape sequence")},
    {S_("'\\x'"), TOKEN_CHAR, 3, S_("'\\x'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\x0'"), TOKEN_CHAR, 4, S_("'\\x0'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\x0g'"), TOKEN_CHAR, 4, S_("'\\x0g'"),
     S_("illegal character U+0067 'g' in escape sequence")},
    {S_("'\\u'"), TOKEN_CHAR, 3, S_("'\\u'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\u0'"), TOKEN_CHAR, 4, S_("'\\u0'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\u00'"), TOKEN_CHAR, 5, S_("'\\u00'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\u000'"), TOKEN_CHAR, 6, S_("'\\u000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\u000"), TOKEN_CHAR, 6, S_("'\\u000"), S_("escape sequence not terminated")},
    {S_("'\\u0000'"), TOKEN_CHAR, 0, S_("'\\u0000'"), S_("")},
    {S_("'\\U'"), TOKEN_CHAR, 3, S_("'\\U'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U0'"), TOKEN_CHAR, 4, S_("'\\U0'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U00'"), TOKEN_CHAR, 5, S_("'\\U00'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U000'"), TOKEN_CHAR, 6, S_("'\\U000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U0000'"), TOKEN_CHAR, 7, S_("'\\U0000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U00000'"), TOKEN_CHAR, 8, S_("'\\U00000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U000000'"), TOKEN_CHAR, 9, S_("'\\U000000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U0000000'"), TOKEN_CHAR, 10, S_("'\\U0000000'"),
     S_("illegal character U+0027 ''' in escape sequence")},
    {S_("'\\U0000000"), TOKEN_CHAR, 10, S_("'\\U0000000"),
     S_("escape sequence not terminated")},
    {S_("'\\U00000000'"), TOKEN_CHAR, 0, S_("'\\U00000000'"), S_("")},
    {S_("'\\Uffffffff'"), TOKEN_CHAR, 2, S_("'\\Uffffffff'"),
     S_("escape sequence is invalid Unicode code point")},
    {S_("'"), TOKEN_CHAR, 0, S_("'"), S_("rune literal not terminated")},
    {S_("'\\"), TOKEN_CHAR, 2, S_("'\\"), S_("escape sequence not terminated")},
    {S_("'\n"), TOKEN_CHAR, 0, S_("'"), S_("rune literal not terminated")},
    {S_("'\n   "), TOKEN_CHAR, 0, S_("'"), S_("rune literal not terminated")},
    {S_("\"\""), TOKEN_STRING, 0, S_("\"\""), S_("")},
    {S_("\"abc"), TOKEN_STRING, 0, S_("\"abc"), S_("string literal not terminated")},
    {S_("\"abc\n"), TOKEN_STRING, 0, S_("\"abc"), S_("string literal not terminated")},
    {S_("\"abc\n   "), TOKEN_STRING, 0, S_("\"abc"),
     S_("string literal not terminated")},
    {S_("``"), TOKEN_STRING, 0, S_("``"), S_("")},
    {S_("`"), TOKEN_STRING, 0, S_("`"), S_("raw string literal not terminated")},
    {S_("/**/"), TOKEN_COMMENT, 0, S_("/**/"), S_("")},
    {S_("/*"), TOKEN_COMMENT, 0, S_("/*"), S_("comment not terminated")},
    {S_("077"), TOKEN_INT, 0, S_("077"), S_("")},
    {S_("078."), TOKEN_FLOAT, 0, S_("078."), S_("")},
    {S_("07801234567."), TOKEN_FLOAT, 0, S_("07801234567."), S_("")},
    {S_("078e0"), TOKEN_FLOAT, 0, S_("078e0"), S_("")},
    {S_("0E"), TOKEN_FLOAT, 2, S_("0E"), S_("exponent has no digits")},
    {S_("078"), TOKEN_INT, 2, S_("078"), S_("invalid digit '8' in octal literal")},
    {S_("07090000008"), TOKEN_INT, 3, S_("07090000008"),
     S_("invalid digit '9' in octal literal")},
    {S_("0x"), TOKEN_INT, 2, S_("0x"), S_("hexadecimal literal has no digits")},
    {S_("\"abc\x00"
        "def\""),
     TOKEN_STRING, 4,
     S_("\"abc\x00"
        "def\""),
     S_("illegal character NUL")},
    {S_("\"abc\x80"
        "def\""),
     TOKEN_STRING, 4,
     S_("\"abc\x80"
        "def\""),
     S_("illegal UTF-8 encoding")},
    {S_("\xEF\xBB\xBF\xEF\xBB\xBF"), TOKEN_ILLEGAL, 3, S_("\xEF\xBB\xBF\xEF\xBB\xBF"),
     S_("illegal byte order mark")},
    {S_("//\xEF\xBB\xBF"), TOKEN_COMMENT, 2, S_("//\xEF\xBB\xBF"),
     S_("illegal byte order mark")},
    {S_("'\xEF\xBB\xBF'"), TOKEN_CHAR, 1, S_("'\xEF\xBB\xBF'"),
     S_("illegal byte order mark")},
    {S_("\"abc\xEF\xBB\xBF"
        "def\""),
     TOKEN_STRING, 4,
     S_("\"abc\xEF\xBB\xBF"
        "def\""),
     S_("illegal byte order mark")},
    {S_("abc\x00"
        "def"),
     TOKEN_IDENT, 3, S_("abc"), S_("illegal character NUL")},
    {S_("abc\x00"), TOKEN_IDENT, 3, S_("abc"), S_("illegal character NUL")},
    {S_("“abc”"), TOKEN_ILLEGAL, 0, S_("abc"),
     S_("curly quotation mark '“' (use neutral '\"')")},
};

static void TestScanErrors(TestingT *t) {
    GsEnv e;
    env_init(&e);
    for (Int i = 0; i < NELEM(error_tests); i++) {
        const GsErrTest *et = &error_tests[i];
        check_error(t, &e, et->src, et->tok, et->pos, et->lit, et->err);
    }
    env_free(&e);
}

typedef struct GsUtf16Env {
    Alloc *a;
    Str got[4];
    Int n;
} GsUtf16Env;

static void utf16_error(void *env, TokenPosition posn, Str msg) {
    GsUtf16Env *u = (GsUtf16Env *)env;
    if (u->n < 4)
        u->got[u->n] = fmt_sprintf_v(u->a, "#%d: %s", posn.offset, msg);
    u->n++;
}

static void TestUTF16(TestingT *t) {
    /* This test doesn't fit within TestScanErrors because the latter assumes
     * that there was only one error. */
    static const Str srcs[] = {
        /* BOM + "package p" encoded as UTF-16 BE */
        S_("\xfe\xff\x00p\x00"
           "a\x00"
           "c\x00k\x00"
           "a\x00g\x00"
           "e\x00 \x00p"),
        /* BOM + "package p" encoded as UTF-16 LE */
        S_("\xff\xfep\x00"
           "a\x00"
           "c\x00k\x00"
           "a\x00g\x00"
           "e\x00 \x00p\x00"),
    };
    /* We expect two errors: one from the decoder, one from the scanner. */
    static const Str want[] = {
        S_("#0: illegal UTF-8 encoding (got UTF-16)"),
        S_("#0: illegal character U+FFFD '\xEF\xBF\xBD'"),
    };
    GsEnv e;
    env_init(&e);
    for (Int i = 0; i < NELEM(srcs); i++) {
        Str src = srcs[i];
        GsUtf16Env u = {e.a, {BURROW_S_INIT("")}, 0};
        GoScanner sc;
        go_scanner_init(&sc, e.a, add_file(e.fset, BURROW_S(""), src.len),
                        bytes_of(src),
                        BURROW_FN(GoScannerErrorHandler, utf16_error, &u), 0);
        go_scanner_scan(&sc, NULL, NULL);

        bool equal = u.n == NELEM(want);
        for (Int k = 0; equal && k < u.n; k++)
            equal = str_eq(u.got[k], want[k]);
        if (!equal) {
            Str got = BURROW_STR_EMPTY;
            for (Int k = 0; k < u.n && k < 4; k++)
                got = fmt_sprintf_v(e.a, k == 0 ? "%s%q" : "%s %q", got, u.got[k]);
            testing_t_errorf_v(t, "Scan(%q) returned errors [%s], want [%q %q]", src,
                               got, want[0], want[1]);
        }
    }
    env_free(&e);
}

/* Verify that no comments show up as literal values when skipping comments. */
static void TestIssue10213(TestingT *t) {
    Str src = BURROW_S("\n"
                       "\t\tvar (\n"
                       "\t\t\tA = 1 // foo\n"
                       "\t\t)\n"
                       "\n"
                       "\t\tvar (\n"
                       "\t\t\tB = 2\n"
                       "\t\t\t// foo\n"
                       "\t\t)\n"
                       "\n"
                       "\t\tvar C = 3 // foo\n"
                       "\n"
                       "\t\tvar D = 4\n"
                       "\t\t// foo\n"
                       "\n"
                       "\t\tfunc anycode() {\n"
                       "\t\t// foo\n"
                       "\t\t}\n"
                       "\t");
    GsEnv e;
    env_init(&e);
    GoScanner s;
    go_scanner_init(&s, e.a, add_file(e.fset, BURROW_S(""), src.len), bytes_of(src),
                    (GoScannerErrorHandler){0}, 0);
    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);
        int cls = tokenclass(tok);
        if (lit.len > 0 && cls != CLASS_KEYWORD && cls != CLASS_LITERAL &&
            tok != TOKEN_SEMICOLON)
            testing_t_errorf_v(
                t, "%s: tok = %s, lit = %q",
                token_position_string(token_file_set_position(e.fset, pos), e.a),
                token_string(tok, e.a), lit);
        if (tok <= TOKEN_EOF)
            break;
    }
    env_free(&e);
}

static void TestIssue28112(TestingT *t) {
    /* make sure to have stand-alone ".." immediately before EOF to test EOF
     * behavior */
    Str src = BURROW_S("... .. 0.. ..");
    static const Token want_tokens[] = {TOKEN_ELLIPSIS, TOKEN_PERIOD, TOKEN_PERIOD,
                                        TOKEN_FLOAT,    TOKEN_PERIOD, TOKEN_PERIOD,
                                        TOKEN_PERIOD,   TOKEN_EOF};
    GsEnv e;
    env_init(&e);
    GoScanner s;
    go_scanner_init(&s, e.a, add_file(e.fset, BURROW_S(""), src.len), bytes_of(src),
                    (GoScannerErrorHandler){0}, 0);
    for (Int i = 0; i < NELEM(want_tokens); i++) {
        Token want = want_tokens[i];
        Token got = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&s, &got, &lit);
        Str where = token_position_string(token_file_set_position(e.fset, pos), e.a);
        if (got != want)
            testing_t_errorf_v(t, "%s: got %s, want %s", where, token_string(got, e.a),
                               token_string(want, e.a));
        /* literals expect to have a (non-empty) literal string and we don't
         * care about other tokens for this test */
        if (tokenclass(got) == CLASS_LITERAL && lit.len == 0)
            testing_t_errorf_v(t, "%s: for %s got empty literal string", where,
                               token_string(got, e.a));
    }
    env_free(&e);
}

typedef struct GsNumTest {
    Token tok;
    Str src;
    Str tokens;
    Str err;
} GsNumTest;

static const GsNumTest number_tests[] = {
    {TOKEN_INT, S_("0b0"), S_("0b0"), S_("")},
    {TOKEN_INT, S_("0b1010"), S_("0b1010"), S_("")},
    {TOKEN_INT, S_("0B1110"), S_("0B1110"), S_("")},
    {TOKEN_INT, S_("0b"), S_("0b"), S_("binary literal has no digits")},
    {TOKEN_INT, S_("0b0190"), S_("0b0190"), S_("invalid digit '9' in binary literal")},
    {TOKEN_INT, S_("0b01a0"), S_("0b01 a0"), S_("")},
    {TOKEN_FLOAT, S_("0b."), S_("0b."), S_("invalid radix point in binary literal")},
    {TOKEN_FLOAT, S_("0b.1"), S_("0b.1"), S_("invalid radix point in binary literal")},
    {TOKEN_FLOAT, S_("0b1.0"), S_("0b1.0"),
     S_("invalid radix point in binary literal")},
    {TOKEN_FLOAT, S_("0b1e10"), S_("0b1e10"),
     S_("'e' exponent requires decimal mantissa")},
    {TOKEN_FLOAT, S_("0b1P-1"), S_("0b1P-1"),
     S_("'P' exponent requires hexadecimal mantissa")},
    {TOKEN_IMAG, S_("0b10i"), S_("0b10i"), S_("")},
    {TOKEN_IMAG, S_("0b10.0i"), S_("0b10.0i"),
     S_("invalid radix point in binary literal")},
    {TOKEN_INT, S_("0o0"), S_("0o0"), S_("")},
    {TOKEN_INT, S_("0o1234"), S_("0o1234"), S_("")},
    {TOKEN_INT, S_("0O1234"), S_("0O1234"), S_("")},
    {TOKEN_INT, S_("0o"), S_("0o"), S_("octal literal has no digits")},
    {TOKEN_INT, S_("0o8123"), S_("0o8123"), S_("invalid digit '8' in octal literal")},
    {TOKEN_INT, S_("0o1293"), S_("0o1293"), S_("invalid digit '9' in octal literal")},
    {TOKEN_INT, S_("0o12a3"), S_("0o12 a3"), S_("")},
    {TOKEN_FLOAT, S_("0o."), S_("0o."), S_("invalid radix point in octal literal")},
    {TOKEN_FLOAT, S_("0o.2"), S_("0o.2"), S_("invalid radix point in octal literal")},
    {TOKEN_FLOAT, S_("0o1.2"), S_("0o1.2"), S_("invalid radix point in octal literal")},
    {TOKEN_FLOAT, S_("0o1E+2"), S_("0o1E+2"),
     S_("'E' exponent requires decimal mantissa")},
    {TOKEN_FLOAT, S_("0o1p10"), S_("0o1p10"),
     S_("'p' exponent requires hexadecimal mantissa")},
    {TOKEN_IMAG, S_("0o10i"), S_("0o10i"), S_("")},
    {TOKEN_IMAG, S_("0o10e0i"), S_("0o10e0i"),
     S_("'e' exponent requires decimal mantissa")},
    {TOKEN_INT, S_("0"), S_("0"), S_("")},
    {TOKEN_INT, S_("0123"), S_("0123"), S_("")},
    {TOKEN_INT, S_("08123"), S_("08123"), S_("invalid digit '8' in octal literal")},
    {TOKEN_INT, S_("01293"), S_("01293"), S_("invalid digit '9' in octal literal")},
    {TOKEN_INT, S_("0F."), S_("0 F ."), S_("")},
    {TOKEN_INT, S_("0123F."), S_("0123 F ."), S_("")},
    {TOKEN_INT, S_("0123456x"), S_("0123456 x"), S_("")},
    {TOKEN_INT, S_("1"), S_("1"), S_("")},
    {TOKEN_INT, S_("1234"), S_("1234"), S_("")},
    {TOKEN_INT, S_("1f"), S_("1 f"), S_("")},
    {TOKEN_IMAG, S_("0i"), S_("0i"), S_("")},
    {TOKEN_IMAG, S_("0678i"), S_("0678i"), S_("")},
    {TOKEN_FLOAT, S_("0."), S_("0."), S_("")},
    {TOKEN_FLOAT, S_("123."), S_("123."), S_("")},
    {TOKEN_FLOAT, S_("0123."), S_("0123."), S_("")},
    {TOKEN_FLOAT, S_(".0"), S_(".0"), S_("")},
    {TOKEN_FLOAT, S_(".123"), S_(".123"), S_("")},
    {TOKEN_FLOAT, S_(".0123"), S_(".0123"), S_("")},
    {TOKEN_FLOAT, S_("0.0"), S_("0.0"), S_("")},
    {TOKEN_FLOAT, S_("123.123"), S_("123.123"), S_("")},
    {TOKEN_FLOAT, S_("0123.0123"), S_("0123.0123"), S_("")},
    {TOKEN_FLOAT, S_("0e0"), S_("0e0"), S_("")},
    {TOKEN_FLOAT, S_("123e+0"), S_("123e+0"), S_("")},
    {TOKEN_FLOAT, S_("0123E-1"), S_("0123E-1"), S_("")},
    {TOKEN_FLOAT, S_("0.e+1"), S_("0.e+1"), S_("")},
    {TOKEN_FLOAT, S_("123.E-10"), S_("123.E-10"), S_("")},
    {TOKEN_FLOAT, S_("0123.e123"), S_("0123.e123"), S_("")},
    {TOKEN_FLOAT, S_(".0e-1"), S_(".0e-1"), S_("")},
    {TOKEN_FLOAT, S_(".123E+10"), S_(".123E+10"), S_("")},
    {TOKEN_FLOAT, S_(".0123E123"), S_(".0123E123"), S_("")},
    {TOKEN_FLOAT, S_("0.0e1"), S_("0.0e1"), S_("")},
    {TOKEN_FLOAT, S_("123.123E-10"), S_("123.123E-10"), S_("")},
    {TOKEN_FLOAT, S_("0123.0123e+456"), S_("0123.0123e+456"), S_("")},
    {TOKEN_FLOAT, S_("0e"), S_("0e"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("0E+"), S_("0E+"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("1e+f"), S_("1e+ f"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("0p0"), S_("0p0"),
     S_("'p' exponent requires hexadecimal mantissa")},
    {TOKEN_FLOAT, S_("1.0P-1"), S_("1.0P-1"),
     S_("'P' exponent requires hexadecimal mantissa")},
    {TOKEN_IMAG, S_("0.i"), S_("0.i"), S_("")},
    {TOKEN_IMAG, S_(".123i"), S_(".123i"), S_("")},
    {TOKEN_IMAG, S_("123.123i"), S_("123.123i"), S_("")},
    {TOKEN_IMAG, S_("123e+0i"), S_("123e+0i"), S_("")},
    {TOKEN_IMAG, S_("123.E-10i"), S_("123.E-10i"), S_("")},
    {TOKEN_IMAG, S_(".123E+10i"), S_(".123E+10i"), S_("")},
    {TOKEN_INT, S_("0x0"), S_("0x0"), S_("")},
    {TOKEN_INT, S_("0x1234"), S_("0x1234"), S_("")},
    {TOKEN_INT, S_("0xcafef00d"), S_("0xcafef00d"), S_("")},
    {TOKEN_INT, S_("0XCAFEF00D"), S_("0XCAFEF00D"), S_("")},
    {TOKEN_INT, S_("0x"), S_("0x"), S_("hexadecimal literal has no digits")},
    {TOKEN_INT, S_("0x1g"), S_("0x1 g"), S_("")},
    {TOKEN_IMAG, S_("0xf00i"), S_("0xf00i"), S_("")},
    {TOKEN_FLOAT, S_("0x0p0"), S_("0x0p0"), S_("")},
    {TOKEN_FLOAT, S_("0x12efp-123"), S_("0x12efp-123"), S_("")},
    {TOKEN_FLOAT, S_("0xABCD.p+0"), S_("0xABCD.p+0"), S_("")},
    {TOKEN_FLOAT, S_("0x.0189P-0"), S_("0x.0189P-0"), S_("")},
    {TOKEN_FLOAT, S_("0x1.ffffp+1023"), S_("0x1.ffffp+1023"), S_("")},
    {TOKEN_FLOAT, S_("0x."), S_("0x."), S_("hexadecimal literal has no digits")},
    {TOKEN_FLOAT, S_("0x0."), S_("0x0."),
     S_("hexadecimal mantissa requires a 'p' exponent")},
    {TOKEN_FLOAT, S_("0x.0"), S_("0x.0"),
     S_("hexadecimal mantissa requires a 'p' exponent")},
    {TOKEN_FLOAT, S_("0x1.1"), S_("0x1.1"),
     S_("hexadecimal mantissa requires a 'p' exponent")},
    {TOKEN_FLOAT, S_("0x1.1e0"), S_("0x1.1e0"),
     S_("hexadecimal mantissa requires a 'p' exponent")},
    {TOKEN_FLOAT, S_("0x1.2gp1a"), S_("0x1.2 gp1a"),
     S_("hexadecimal mantissa requires a 'p' exponent")},
    {TOKEN_FLOAT, S_("0x0p"), S_("0x0p"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("0xeP-"), S_("0xeP-"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("0x1234PAB"), S_("0x1234P AB"), S_("exponent has no digits")},
    {TOKEN_FLOAT, S_("0x1.2p1a"), S_("0x1.2p1 a"), S_("")},
    {TOKEN_IMAG, S_("0xf00.bap+12i"), S_("0xf00.bap+12i"), S_("")},
    {TOKEN_INT, S_("0b_1000_0001"), S_("0b_1000_0001"), S_("")},
    {TOKEN_INT, S_("0o_600"), S_("0o_600"), S_("")},
    {TOKEN_INT, S_("0_466"), S_("0_466"), S_("")},
    {TOKEN_INT, S_("1_000"), S_("1_000"), S_("")},
    {TOKEN_FLOAT, S_("1_000.000_1"), S_("1_000.000_1"), S_("")},
    {TOKEN_IMAG, S_("10e+1_2_3i"), S_("10e+1_2_3i"), S_("")},
    {TOKEN_INT, S_("0x_f00d"), S_("0x_f00d"), S_("")},
    {TOKEN_FLOAT, S_("0x_f00d.0p1_2"), S_("0x_f00d.0p1_2"), S_("")},
    {TOKEN_INT, S_("0b__1000"), S_("0b__1000"),
     S_("'_' must separate successive digits")},
    {TOKEN_INT, S_("0o60___0"), S_("0o60___0"),
     S_("'_' must separate successive digits")},
    {TOKEN_INT, S_("0466_"), S_("0466_"), S_("'_' must separate successive digits")},
    {TOKEN_FLOAT, S_("1_."), S_("1_."), S_("'_' must separate successive digits")},
    {TOKEN_FLOAT, S_("0._1"), S_("0._1"), S_("'_' must separate successive digits")},
    {TOKEN_FLOAT, S_("2.7_e0"), S_("2.7_e0"),
     S_("'_' must separate successive digits")},
    {TOKEN_IMAG, S_("10e+12_i"), S_("10e+12_i"),
     S_("'_' must separate successive digits")},
    {TOKEN_INT, S_("0x___0"), S_("0x___0"), S_("'_' must separate successive digits")},
    {TOKEN_FLOAT, S_("0x1.0_p0"), S_("0x1.0_p0"),
     S_("'_' must separate successive digits")},
};

typedef struct GsFirstError {
    Str err;
    Alloc *a;
} GsFirstError;

static void first_error(void *env, TokenPosition pos, Str msg) {
    (void)pos;
    GsFirstError *fe = (GsFirstError *)env;
    if (fe->err.len == 0)
        fe->err = str_clone(fe->a, msg);
}

static void TestNumbers(TestingT *t) {
    GsEnv e;
    env_init(&e);
    for (Int k = 0; k < NELEM(number_tests); k++) {
        const GsNumTest *test = &number_tests[k];
        GoScanner s;
        GsFirstError fe = {BURROW_STR_EMPTY, e.a};
        go_scanner_init(&s, e.a, add_file(e.fset, BURROW_S(""), test->src.len),
                        bytes_of(test->src),
                        BURROW_FN(GoScannerErrorHandler, first_error, &fe), 0);
        Slice wants = strings_split(e.a, test->tokens, BURROW_S(" "));
        for (Int i = 0; i < wants.len; i++) {
            Str want = BURROW_AT(Str, wants, i);
            fe.err = BURROW_STR_EMPTY;
            Token tok = TOKEN_ILLEGAL;
            Str lit = BURROW_STR_EMPTY;
            go_scanner_scan(&s, &tok, &lit);

            /* compute lit where for tokens where lit is not defined */
            switch (tok) {
            case TOKEN_PERIOD:
                lit = BURROW_S(".");
                break;
            case TOKEN_ADD:
                lit = BURROW_S("+");
                break;
            case TOKEN_SUB:
                lit = BURROW_S("-");
                break;
            default:
                break;
            }

            if (i == 0) {
                if (tok != test->tok)
                    testing_t_errorf_v(t, "%q: got token %s; want %s", test->src,
                                       token_string(tok, e.a),
                                       token_string(test->tok, e.a));
                if (!str_eq(fe.err, test->err))
                    testing_t_errorf_v(t, "%q: got error %q; want %q", test->src,
                                       fe.err, test->err);
            }

            if (!str_eq(lit, want))
                testing_t_errorf_v(t, "%q: got literal %q (%s); want %s", test->src,
                                   lit, token_string(tok, e.a), want);
        }

        /* make sure we read all */
        Token tok = TOKEN_ILLEGAL;
        go_scanner_scan(&s, &tok, NULL);
        if (tok == TOKEN_SEMICOLON)
            go_scanner_scan(&s, &tok, NULL);
        if (tok != TOKEN_EOF)
            testing_t_errorf_v(t, "%q: got %s; want EOF", test->src,
                               token_string(tok, e.a));
    }
    env_free(&e);
}

static void fatal_error(void *env, TokenPosition pos, Str msg) {
    (void)pos;
    testing_t_fatalf_v((TestingT *)env, "%s", msg);
}

static void TestScanReuseSemiInNewlineComment(TestingT *t) {
    GsEnv e;
    env_init(&e);

    Str src = BURROW_S("identifier /*a\nb*/ + other");
    GoScanner s;
    go_scanner_init(&s, e.a,
                    token_file_set_add_file(e.fset, BURROW_S("test.go"), -1, src.len),
                    bytes_of(src), BURROW_FN(GoScannerErrorHandler, fatal_error, t),
                    GO_SCANNER_SCAN_COMMENTS);

    go_scanner_scan(&s, NULL, NULL); /* IDENT(identifier) */

    Token tok = TOKEN_ILLEGAL;
    go_scanner_scan(&s, &tok, NULL); /* COMMENT */
    if (tok != TOKEN_COMMENT) {
        testing_t_fatalf_v(t, "tok = %v; want = token.SEMICOLON",
                           token_string(tok, e.a));
        return;
    }

    go_scanner_init(&s, e.a,
                    token_file_set_add_file(e.fset, BURROW_S("test.go"), -1, src.len),
                    bytes_of(src), BURROW_FN(GoScannerErrorHandler, fatal_error, t),
                    GO_SCANNER_SCAN_COMMENTS);

    go_scanner_scan(&s, &tok, NULL);
    if (tok != TOKEN_IDENT) {
        testing_t_fatalf_v(t, "tok = %v; want = token.IDENT", token_string(tok, e.a));
        return;
    }
    env_free(&e);
}

typedef struct GsTok {
    Token tok;
    TokenPos start;
    TokenPos end;
} GsTok;

typedef struct GsEndCase {
    Str name;
    Str src;
    GsTok end[8];
    Int nend;
} GsEndCase;

static const GsEndCase end_cases[] = {
    {S_("operators"),
     S_("+ - / >> == ="),
     {
         {TOKEN_ADD, 1, 2},
         {TOKEN_SUB, 3, 4},
         {TOKEN_QUO, 5, 6},
         {TOKEN_SHR, 7, 9},
         {TOKEN_EQL, 10, 12},
         {TOKEN_ASSIGN, 13, 14},
         {TOKEN_EOF, 14, 14},
     },
     7},
    {S_("braces"),
     S_("{([])}"),
     {
         {TOKEN_LBRACE, 1, 2},
         {TOKEN_LPAREN, 2, 3},
         {TOKEN_LBRACK, 3, 4},
         {TOKEN_RBRACK, 4, 5},
         {TOKEN_RPAREN, 5, 6},
         {TOKEN_RBRACE, 6, 7},
         {TOKEN_SEMICOLON, 7, 7},
         {TOKEN_EOF, 7, 7},
     },
     8},
    {S_("literals"),
     S_("\"foo\" 123 1.23 0b11"),
     {
         {TOKEN_STRING, 1, 6},
         {TOKEN_INT, 7, 10},
         {TOKEN_FLOAT, 11, 15},
         {TOKEN_INT, 16, 20},
         {TOKEN_SEMICOLON, 20, 20},
         {TOKEN_EOF, 20, 20},
     },
     6},
    {S_("missing newline at the end of file"),
     S_("foo"),
     {
         {TOKEN_IDENT, 1, 4},
         {TOKEN_SEMICOLON, 4, 4},
         {TOKEN_EOF, 4, 4},
     },
     3},
    {S_("newline at the end of file"),
     S_("foo\n"),
     {
         {TOKEN_IDENT, 1, 4},
         {TOKEN_SEMICOLON, 4, 5},
         {TOKEN_EOF, 5, 5},
     },
     3},
    {S_("semicolon at the end of file"),
     S_("foo;"),
     {
         {TOKEN_IDENT, 1, 4},
         {TOKEN_SEMICOLON, 4, 5},
         {TOKEN_EOF, 5, 5},
     },
     3},
    {S_("semicolon and newline at the end of file"),
     S_("foo;\n"),
     {
         {TOKEN_IDENT, 1, 4},
         {TOKEN_SEMICOLON, 4, 5},
         {TOKEN_EOF, 6, 6},
     },
     3},
    {S_("newline in comment acting as semicolon"),
     S_("foo /*\n*/ bar"),
     {
         {TOKEN_IDENT, 1, 4},
         {TOKEN_COMMENT, 5, 10},
         {TOKEN_SEMICOLON, 7, 8},
         {TOKEN_IDENT, 11, 14},
         {TOKEN_SEMICOLON, 14, 14},
         {TOKEN_EOF, 14, 14},
     },
     6},
    {S_("BOM"),
     S_("\xEF\xBB\xBF"
        "foo"),
     {
         {TOKEN_IDENT, 4, 7},
         {TOKEN_SEMICOLON, 7, 7},
         {TOKEN_EOF, 7, 7},
     },
     3},
};

/* Go's %v of a []tok. */
static Str format_toks(Alloc *a, const GsTok *toks, Int n) {
    Str s = BURROW_S("[");
    for (Int i = 0; i < n; i++)
        s = fmt_sprintf_v(a, i == 0 ? "%s{%s %d %d}" : "%s {%s %d %d}", s,
                          token_string(toks[i].tok, a), toks[i].start, toks[i].end);
    return fmt_sprintf_v(a, "%s]", s);
}

static void run_end_case(void *env, TestingT *t) {
    const GsEndCase *tt = (const GsEndCase *)env;
    GsEnv e;
    env_init(&e);

    GoScanner s;
    go_scanner_init(
        &s, e.a, token_file_set_add_file(e.fset, BURROW_S("test.go"), -1, tt->src.len),
        bytes_of(tt->src), BURROW_FN(GoScannerErrorHandler, fatal_error, t),
        GO_SCANNER_SCAN_COMMENTS);

    TokenPos end = go_scanner_end(&s);
    if (end != TOKEN_NO_POS)
        testing_t_errorf_v(t, "after init: s.End() = %d; want token.NoPos", end);

    GsTok got[16];
    Int n = 0;
    for (;;) {
        Token tok_typ = TOKEN_ILLEGAL;
        TokenPos pos = go_scanner_scan(&s, &tok_typ, NULL);
        if (n < NELEM(got)) {
            got[n] = (GsTok){tok_typ, pos, go_scanner_end(&s)};
            n++;
        }
        if (tok_typ == TOKEN_EOF)
            break;
    }

    bool equal = n == tt->nend;
    for (Int i = 0; equal && i < n; i++)
        equal = got[i].tok == tt->end[i].tok && got[i].start == tt->end[i].start &&
                got[i].end == tt->end[i].end;
    if (!equal) {
        testing_t_fatalf_v(t, "input %q: got = %s; want = %s", tt->src,
                           format_toks(e.a, got, n),
                           format_toks(e.a, tt->end, tt->nend));
        return;
    }
    env_free(&e);
}

static void TestScannerEnd(TestingT *t) {
    for (Int i = 0; i < NELEM(end_cases); i++)
        testing_t_run(
            t, end_cases[i].name,
            BURROW_FN(TestingTFunc, run_end_case, (void *)(uintptr_t)&end_cases[i]));
}

static void TestScannerEndReuse(TestingT *t) {
    GsEnv e;
    env_init(&e);

    Str src = BURROW_S("identifier /*a\nb*/ + other");
    GoScanner s;
    go_scanner_init(&s, e.a,
                    token_file_set_add_file(e.fset, BURROW_S("test.go"), -1, src.len),
                    bytes_of(src), BURROW_FN(GoScannerErrorHandler, fatal_error, t),
                    GO_SCANNER_SCAN_COMMENTS);

    go_scanner_scan(&s, NULL, NULL); /* IDENT(identifier) */
    go_scanner_scan(&s, NULL, NULL); /* COMMENT */

    Token tok = TOKEN_ILLEGAL;
    go_scanner_scan(&s, &tok, NULL); /* SEMICOLON */
    if (tok != TOKEN_SEMICOLON) {
        testing_t_fatalf_v(t, "tok = %v; want = token.SEMICOLON",
                           token_string(tok, e.a));
        return;
    }

    go_scanner_init(&s, e.a,
                    token_file_set_add_file(e.fset, BURROW_S("test.go"), -1, src.len),
                    bytes_of(src), BURROW_FN(GoScannerErrorHandler, fatal_error, t),
                    GO_SCANNER_SCAN_COMMENTS);

    TokenPos end = go_scanner_end(&s);
    if (end != TOKEN_NO_POS)
        testing_t_errorf_v(t, "s.End() = %d; want token.NoPos", end);
    env_free(&e);
}

/* Not in Go: the ErrorList methods and Err, Error and PrintError, which Go
 * only reaches through the tests above when they fail. */
static void TestErrorList(TestingT *t) {
    GsEnv e;
    env_init(&e);
    GoScannerErrorList list = {NULL, 0, 0, NULL};

    Str got = go_scanner_error_list_error(list, e.a);
    if (!str_eq(got, BURROW_S("no errors")))
        testing_t_errorf_v(t, "empty list: Error() = %q", got);
    if (BURROW_FAILED(go_scanner_error_list_err(list)))
        testing_t_errorf_v(t, "empty list: Err() is not nil");

    TokenPosition p1 = {BURROW_S_INIT("b.go"), 10, 2, 3};
    TokenPosition p2 = {BURROW_S_INIT("a.go"), 40, 7, 1};
    TokenPosition p3 = {BURROW_S_INIT("a.go"), 4, 1, 5};
    go_scanner_error_list_add(&list, e.a, p1, BURROW_S("third"));
    go_scanner_error_list_add(&list, e.a, p2, BURROW_S("second"));
    got = go_scanner_error_list_error(list, e.a);
    if (!str_eq(got, BURROW_S("b.go:2:3: third (and 1 more errors)")))
        testing_t_errorf_v(t, "two errors: Error() = %q", got);
    go_scanner_error_list_add(&list, e.a, p3, BURROW_S("first"));

    go_scanner_error_list_sort(list);
    static const Str want_sorted[] = {S_("a.go:1:5: first"), S_("a.go:7:1: second"),
                                      S_("b.go:2:3: third")};
    for (Int i = 0; i < list.len; i++) {
        got = go_scanner_error_error(*go_scanner_error_list_at(list, i), e.a);
        if (!str_eq(got, want_sorted[i]))
            testing_t_errorf_v(t, "sorted [%d] = %q, want %q", i, got, want_sorted[i]);
    }

    Error err = go_scanner_error_list_err(list);
    if (!str_eq(error_text(err), BURROW_S("a.go:1:5: first (and 2 more errors)")))
        testing_t_errorf_v(t, "Err().Error() = %q", error_text(err));
    const GoScannerErrorList *as =
        (const GoScannerErrorList *)errors_as(err, TYPE_GO_SCANNER_ERROR_LIST);
    if (as == NULL || as->len != 3)
        testing_t_errorf_v(t, "errors.As did not give the list back");

    BytesBuffer buf = BYTES_BUFFER(e.a);
    go_scanner_print_error(bytes_buffer_as_io_writer(&buf), err);
    got = bytes_buffer_string(&buf, e.a);
    if (!str_eq(got, BURROW_S("a.go:1:5: first\na.go:7:1: second\nb.go:2:3: third\n")))
        testing_t_errorf_v(t, "PrintError(list) wrote %q", got);

    Error kept = error_retain(e.a, err);
    go_scanner_error_list_reset(&list);
    if (go_scanner_error_list_len(list) != 0)
        testing_t_errorf_v(t, "len after Reset = %d", go_scanner_error_list_len(list));
    if (!str_eq(error_text(kept), BURROW_S("a.go:1:5: first (and 2 more errors)")))
        testing_t_errorf_v(t, "retained Error() = %q", error_text(kept));

    bytes_buffer_reset(&buf);
    go_scanner_print_error(bytes_buffer_as_io_writer(&buf),
                           errors_new(e.a, BURROW_S("plain")));
    go_scanner_print_error(bytes_buffer_as_io_writer(&buf), BURROW_NO_ERROR);
    got = bytes_buffer_string(&buf, e.a);
    if (!str_eq(got, BURROW_S("plain\n")))
        testing_t_errorf_v(t, "PrintError(other) wrote %q", got);

    GoScannerError bare = {{BURROW_S_INIT(""), 0, 0, 0}, BURROW_S_INIT("bare")};
    got = go_scanner_error_error(bare, e.a);
    if (!str_eq(got, BURROW_S("bare")))
        testing_t_errorf_v(t, "Error() with no position = %q", got);
    GoScannerError named = {{BURROW_S_INIT("f.go"), 0, 0, 0}, BURROW_S_INIT("named")};
    got = go_scanner_error_error(named, e.a);
    if (!str_eq(got, BURROW_S("f.go: named")))
        testing_t_errorf_v(t, "Error() with a file name only = %q", got);

    bytes_buffer_free(&buf);
    env_free(&e);
}

/* Not in Go: Init's panic when the file and the source disagree. */
static void TestInitSizeMismatch(TestingT *t) {
    GsEnv e;
    env_init(&e);
    TokenFile *f = add_file(e.fset, BURROW_S("x.go"), 3);
    Str src = BURROW_S("abcd");
    /* The message is checked inside the catch so that nothing but a volatile flag
     * changes after the setjmp, which keeps mingw's -Wclobbered quiet. */
    volatile bool panicked = false;
    BURROW_TRY {
        GoScanner s;
        go_scanner_init(&s, e.a, f, bytes_of(src), (GoScannerErrorHandler){0}, 0);
    }
    BURROW_CATCH(r) {
        panicked = true;
        if (!str_eq(panic_text(r),
                    BURROW_S("file size (3) does not match src len (4)")))
            testing_t_errorf_v(t, "wrong panic message: %q", panic_text(r));
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "Init with a short file did not panic");
    env_free(&e);
}

#define TESTS(X)                                                                       \
    X(TestScan)                                                                        \
    X(TestStripCR)                                                                     \
    X(TestSemicolons)                                                                  \
    X(TestLineDirectives)                                                              \
    X(TestInvalidLineDirectives)                                                       \
    X(TestInit)                                                                        \
    X(TestStdErrorHandler)                                                             \
    X(TestScanErrors)                                                                  \
    X(TestUTF16)                                                                       \
    X(TestIssue10213)                                                                  \
    X(TestIssue28112)                                                                  \
    X(TestNumbers)                                                                     \
    X(TestScanReuseSemiInNewlineComment)                                               \
    X(TestScannerEnd)                                                                  \
    X(TestScannerEndReuse)                                                             \
    X(TestErrorList)                                                                   \
    X(TestInitSizeMismatch)

TESTING_MAIN(TESTS)
