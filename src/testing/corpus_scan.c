/* Derived from Go's src/go/scanner/scanner.go.
 * Go source: go1.27.1.
 *
 * go/scanner, for corpus.c, along with the parts of go/token it needs: the
 * token names and precedences from token.go, the Position arithmetic from
 * position.go and the ErrorList from scanner/errors.go. See corpus_syntax.h
 * for why this exists and how long it is meant to.
 *
 * Two things are simpler than in Go because of what a corpus line is. It never
 * holds a newline, so the line table has one entry and the code that adds
 * lines and synthesises a semicolon for a newline inside a comment has nothing
 * to do. And a //line comment runs to the end of the line, which is the end of
 * the file, so only the form in a block comment can ever take effect, and the check Go
 * makes for a //line comment starting its line is kept but can never matter.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "corpus_syntax.h"

#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/utf8.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ tokens */

static const char *const corpus_tok_names[CT_COUNT] = {
    [CT_ILLEGAL] = "ILLEGAL",
    [CT_EOF] = "EOF",
    [CT_COMMENT] = "COMMENT",
    [CT_IDENT] = "IDENT",
    [CT_INT] = "INT",
    [CT_FLOAT] = "FLOAT",
    [CT_IMAG] = "IMAG",
    [CT_CHAR] = "CHAR",
    [CT_STRING] = "STRING",
    [CT_ADD] = "+",
    [CT_SUB] = "-",
    [CT_MUL] = "*",
    [CT_QUO] = "/",
    [CT_REM] = "%",
    [CT_AND] = "&",
    [CT_OR] = "|",
    [CT_XOR] = "^",
    [CT_SHL] = "<<",
    [CT_SHR] = ">>",
    [CT_AND_NOT] = "&^",
    [CT_ADD_ASSIGN] = "+=",
    [CT_SUB_ASSIGN] = "-=",
    [CT_MUL_ASSIGN] = "*=",
    [CT_QUO_ASSIGN] = "/=",
    [CT_REM_ASSIGN] = "%=",
    [CT_AND_ASSIGN] = "&=",
    [CT_OR_ASSIGN] = "|=",
    [CT_XOR_ASSIGN] = "^=",
    [CT_SHL_ASSIGN] = "<<=",
    [CT_SHR_ASSIGN] = ">>=",
    [CT_AND_NOT_ASSIGN] = "&^=",
    [CT_LAND] = "&&",
    [CT_LOR] = "||",
    [CT_ARROW] = "<-",
    [CT_INC] = "++",
    [CT_DEC] = "--",
    [CT_EQL] = "==",
    [CT_LSS] = "<",
    [CT_GTR] = ">",
    [CT_ASSIGN] = "=",
    [CT_NOT] = "!",
    [CT_NEQ] = "!=",
    [CT_LEQ] = "<=",
    [CT_GEQ] = ">=",
    [CT_DEFINE] = ":=",
    [CT_ELLIPSIS] = "...",
    [CT_LPAREN] = "(",
    [CT_LBRACK] = "[",
    [CT_LBRACE] = "{",
    [CT_COMMA] = ",",
    [CT_PERIOD] = ".",
    [CT_RPAREN] = ")",
    [CT_RBRACK] = "]",
    [CT_RBRACE] = "}",
    [CT_SEMICOLON] = ";",
    [CT_COLON] = ":",
    [CT_TILDE] = "~",
    [CT_BREAK] = "break",
    [CT_CASE] = "case",
    [CT_CHAN] = "chan",
    [CT_CONST] = "const",
    [CT_CONTINUE] = "continue",
    [CT_DEFAULT] = "default",
    [CT_DEFER] = "defer",
    [CT_ELSE] = "else",
    [CT_FALLTHROUGH] = "fallthrough",
    [CT_FOR] = "for",
    [CT_FUNC] = "func",
    [CT_GO] = "go",
    [CT_GOTO] = "goto",
    [CT_IF] = "if",
    [CT_IMPORT] = "import",
    [CT_INTERFACE] = "interface",
    [CT_MAP] = "map",
    [CT_PACKAGE] = "package",
    [CT_RANGE] = "range",
    [CT_RETURN] = "return",
    [CT_SELECT] = "select",
    [CT_STRUCT] = "struct",
    [CT_SWITCH] = "switch",
    [CT_TYPE] = "type",
    [CT_VAR] = "var",
};

const char *burrow__testing_corpus_tok_string(CorpusTok t) {
    return corpus_tok_names[t];
}

int burrow__testing_corpus_tok_prec(CorpusTok t) {
    switch ((int)t) {
    case CT_LOR:
        return 1;
    case CT_LAND:
        return 2;
    case CT_EQL:
    case CT_NEQ:
    case CT_LSS:
    case CT_LEQ:
    case CT_GTR:
    case CT_GEQ:
        return 3;
    case CT_ADD:
    case CT_SUB:
    case CT_OR:
    case CT_XOR:
        return 4;
    case CT_MUL:
    case CT_QUO:
    case CT_REM:
    case CT_SHL:
    case CT_SHR:
    case CT_AND:
    case CT_AND_NOT:
        return 5;
    default:
        break;
    }
    return 0;
}

bool burrow__testing_corpus_tok_is_literal(CorpusTok t) {
    return t >= CT_IDENT && t <= CT_STRING;
}

/* token.Lookup, for a word already known to be longer than one byte. */
static CorpusTok corpus_lookup(Str w) {
    for (int t = CT_BREAK; t <= CT_VAR; t++) {
        const char *k = corpus_tok_names[t];
        size_t n = strlen(k);
        if ((size_t)w.len == n && memcmp(w.p, k, n) == 0)
            return (CorpusTok)t;
    }
    return CT_IDENT;
}

/* -------------------------------------------------------- files and errors */

void *burrow__testing_corpus_alloc(Alloc *a, size_t size) {
    void *p = mem_alloc(a, size, BURROW_ALIGN_MAX);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    memset(p, 0, size);
    return p;
}

static void *corpus_grow(Alloc *a, void *old, Int len, Int *cap, size_t elem) {
    if (len < *cap)
        return old;
    Int ncap = *cap == 0 ? 4 : *cap * 2;
    void *p = burrow__testing_corpus_alloc(a, (size_t)ncap * elem);
    if (len > 0)
        memcpy(p, old, (size_t)len * elem);
    *cap = ncap;
    return p;
}

/* File.Position, which is File.unpack with the line directives applied. The
 * line table has one line starting at offset 0, so the distance Go works out
 * between the directive's line and the position's is always zero. */
CorpusPosition burrow__testing_corpus_position(const CorpusFile *f, Int pos) {
    CorpusPosition r = {0};
    if (pos == 0)
        return r;
    Int offset = pos - 1;
    r.filename = f->name;
    r.line = 1;
    r.column = (int64_t)offset + 1;
    Int i = -1;
    while (i + 1 < f->ninfos && f->infos[i + 1].offset <= offset)
        i++;
    if (i >= 0) {
        const CorpusLineInfo *alt = &f->infos[i];
        r.filename = alt->filename;
        r.line = alt->line;
        if (alt->column == 0)
            r.column = 0;
        else
            r.column = alt->column + (int64_t)(offset - alt->offset);
    }
    return r;
}

void burrow__testing_corpus_error_add(CorpusFile *f, Int pos, Str msg) {
    if (f->bailed)
        return;
    f->errs = corpus_grow(f->a, f->errs, f->nerrs, &f->cap_errs, sizeof *f->errs);
    f->errs[f->nerrs].pos = burrow__testing_corpus_position(f, pos);
    f->errs[f->nerrs].msg = msg;
    f->nerrs++;
}

/* File.AddLineColumnInfo. */
static void corpus_add_line_info(CorpusFile *f, Int offset, Str filename, int64_t line,
                                 int64_t column) {
    if ((f->ninfos == 0 || f->infos[f->ninfos - 1].offset < offset) &&
        offset < f->size) {
        f->infos =
            corpus_grow(f->a, f->infos, f->ninfos, &f->cap_infos, sizeof *f->infos);
        f->infos[f->ninfos] = (CorpusLineInfo){offset, filename, line, column};
        f->ninfos++;
    }
}

/* ----------------------------------------------------------------- unicode
 *
 * unicode.IsLetter and unicode.IsDigit above U+007F, which is all the scanner
 * asks them about, as ranges. They were made by running every rune from U+0080
 * to unicode.MaxRune through the two functions at go1.27.1 and writing down
 * where the answer changes. They go when unicode is ported. */

static const uint32_t corpus_letters[][2] = {
    {0xaa, 0xaa},       {0xb5, 0xb5},       {0xba, 0xba},       {0xc0, 0xd6},
    {0xd8, 0xf6},       {0xf8, 0x2c1},      {0x2c6, 0x2d1},     {0x2e0, 0x2e4},
    {0x2ec, 0x2ec},     {0x2ee, 0x2ee},     {0x370, 0x374},     {0x376, 0x377},
    {0x37a, 0x37d},     {0x37f, 0x37f},     {0x386, 0x386},     {0x388, 0x38a},
    {0x38c, 0x38c},     {0x38e, 0x3a1},     {0x3a3, 0x3f5},     {0x3f7, 0x481},
    {0x48a, 0x52f},     {0x531, 0x556},     {0x559, 0x559},     {0x560, 0x588},
    {0x5d0, 0x5ea},     {0x5ef, 0x5f2},     {0x620, 0x64a},     {0x66e, 0x66f},
    {0x671, 0x6d3},     {0x6d5, 0x6d5},     {0x6e5, 0x6e6},     {0x6ee, 0x6ef},
    {0x6fa, 0x6fc},     {0x6ff, 0x6ff},     {0x710, 0x710},     {0x712, 0x72f},
    {0x74d, 0x7a5},     {0x7b1, 0x7b1},     {0x7ca, 0x7ea},     {0x7f4, 0x7f5},
    {0x7fa, 0x7fa},     {0x800, 0x815},     {0x81a, 0x81a},     {0x824, 0x824},
    {0x828, 0x828},     {0x840, 0x858},     {0x860, 0x86a},     {0x870, 0x887},
    {0x889, 0x88f},     {0x8a0, 0x8c9},     {0x904, 0x939},     {0x93d, 0x93d},
    {0x950, 0x950},     {0x958, 0x961},     {0x971, 0x980},     {0x985, 0x98c},
    {0x98f, 0x990},     {0x993, 0x9a8},     {0x9aa, 0x9b0},     {0x9b2, 0x9b2},
    {0x9b6, 0x9b9},     {0x9bd, 0x9bd},     {0x9ce, 0x9ce},     {0x9dc, 0x9dd},
    {0x9df, 0x9e1},     {0x9f0, 0x9f1},     {0x9fc, 0x9fc},     {0xa05, 0xa0a},
    {0xa0f, 0xa10},     {0xa13, 0xa28},     {0xa2a, 0xa30},     {0xa32, 0xa33},
    {0xa35, 0xa36},     {0xa38, 0xa39},     {0xa59, 0xa5c},     {0xa5e, 0xa5e},
    {0xa72, 0xa74},     {0xa85, 0xa8d},     {0xa8f, 0xa91},     {0xa93, 0xaa8},
    {0xaaa, 0xab0},     {0xab2, 0xab3},     {0xab5, 0xab9},     {0xabd, 0xabd},
    {0xad0, 0xad0},     {0xae0, 0xae1},     {0xaf9, 0xaf9},     {0xb05, 0xb0c},
    {0xb0f, 0xb10},     {0xb13, 0xb28},     {0xb2a, 0xb30},     {0xb32, 0xb33},
    {0xb35, 0xb39},     {0xb3d, 0xb3d},     {0xb5c, 0xb5d},     {0xb5f, 0xb61},
    {0xb71, 0xb71},     {0xb83, 0xb83},     {0xb85, 0xb8a},     {0xb8e, 0xb90},
    {0xb92, 0xb95},     {0xb99, 0xb9a},     {0xb9c, 0xb9c},     {0xb9e, 0xb9f},
    {0xba3, 0xba4},     {0xba8, 0xbaa},     {0xbae, 0xbb9},     {0xbd0, 0xbd0},
    {0xc05, 0xc0c},     {0xc0e, 0xc10},     {0xc12, 0xc28},     {0xc2a, 0xc39},
    {0xc3d, 0xc3d},     {0xc58, 0xc5a},     {0xc5c, 0xc5d},     {0xc60, 0xc61},
    {0xc80, 0xc80},     {0xc85, 0xc8c},     {0xc8e, 0xc90},     {0xc92, 0xca8},
    {0xcaa, 0xcb3},     {0xcb5, 0xcb9},     {0xcbd, 0xcbd},     {0xcdc, 0xcde},
    {0xce0, 0xce1},     {0xcf1, 0xcf2},     {0xd04, 0xd0c},     {0xd0e, 0xd10},
    {0xd12, 0xd3a},     {0xd3d, 0xd3d},     {0xd4e, 0xd4e},     {0xd54, 0xd56},
    {0xd5f, 0xd61},     {0xd7a, 0xd7f},     {0xd85, 0xd96},     {0xd9a, 0xdb1},
    {0xdb3, 0xdbb},     {0xdbd, 0xdbd},     {0xdc0, 0xdc6},     {0xe01, 0xe30},
    {0xe32, 0xe33},     {0xe40, 0xe46},     {0xe81, 0xe82},     {0xe84, 0xe84},
    {0xe86, 0xe8a},     {0xe8c, 0xea3},     {0xea5, 0xea5},     {0xea7, 0xeb0},
    {0xeb2, 0xeb3},     {0xebd, 0xebd},     {0xec0, 0xec4},     {0xec6, 0xec6},
    {0xedc, 0xedf},     {0xf00, 0xf00},     {0xf40, 0xf47},     {0xf49, 0xf6c},
    {0xf88, 0xf8c},     {0x1000, 0x102a},   {0x103f, 0x103f},   {0x1050, 0x1055},
    {0x105a, 0x105d},   {0x1061, 0x1061},   {0x1065, 0x1066},   {0x106e, 0x1070},
    {0x1075, 0x1081},   {0x108e, 0x108e},   {0x10a0, 0x10c5},   {0x10c7, 0x10c7},
    {0x10cd, 0x10cd},   {0x10d0, 0x10fa},   {0x10fc, 0x1248},   {0x124a, 0x124d},
    {0x1250, 0x1256},   {0x1258, 0x1258},   {0x125a, 0x125d},   {0x1260, 0x1288},
    {0x128a, 0x128d},   {0x1290, 0x12b0},   {0x12b2, 0x12b5},   {0x12b8, 0x12be},
    {0x12c0, 0x12c0},   {0x12c2, 0x12c5},   {0x12c8, 0x12d6},   {0x12d8, 0x1310},
    {0x1312, 0x1315},   {0x1318, 0x135a},   {0x1380, 0x138f},   {0x13a0, 0x13f5},
    {0x13f8, 0x13fd},   {0x1401, 0x166c},   {0x166f, 0x167f},   {0x1681, 0x169a},
    {0x16a0, 0x16ea},   {0x16f1, 0x16f8},   {0x1700, 0x1711},   {0x171f, 0x1731},
    {0x1740, 0x1751},   {0x1760, 0x176c},   {0x176e, 0x1770},   {0x1780, 0x17b3},
    {0x17d7, 0x17d7},   {0x17dc, 0x17dc},   {0x1820, 0x1878},   {0x1880, 0x1884},
    {0x1887, 0x18a8},   {0x18aa, 0x18aa},   {0x18b0, 0x18f5},   {0x1900, 0x191e},
    {0x1950, 0x196d},   {0x1970, 0x1974},   {0x1980, 0x19ab},   {0x19b0, 0x19c9},
    {0x1a00, 0x1a16},   {0x1a20, 0x1a54},   {0x1aa7, 0x1aa7},   {0x1b05, 0x1b33},
    {0x1b45, 0x1b4c},   {0x1b83, 0x1ba0},   {0x1bae, 0x1baf},   {0x1bba, 0x1be5},
    {0x1c00, 0x1c23},   {0x1c4d, 0x1c4f},   {0x1c5a, 0x1c7d},   {0x1c80, 0x1c8a},
    {0x1c90, 0x1cba},   {0x1cbd, 0x1cbf},   {0x1ce9, 0x1cec},   {0x1cee, 0x1cf3},
    {0x1cf5, 0x1cf6},   {0x1cfa, 0x1cfa},   {0x1d00, 0x1dbf},   {0x1e00, 0x1f15},
    {0x1f18, 0x1f1d},   {0x1f20, 0x1f45},   {0x1f48, 0x1f4d},   {0x1f50, 0x1f57},
    {0x1f59, 0x1f59},   {0x1f5b, 0x1f5b},   {0x1f5d, 0x1f5d},   {0x1f5f, 0x1f7d},
    {0x1f80, 0x1fb4},   {0x1fb6, 0x1fbc},   {0x1fbe, 0x1fbe},   {0x1fc2, 0x1fc4},
    {0x1fc6, 0x1fcc},   {0x1fd0, 0x1fd3},   {0x1fd6, 0x1fdb},   {0x1fe0, 0x1fec},
    {0x1ff2, 0x1ff4},   {0x1ff6, 0x1ffc},   {0x2071, 0x2071},   {0x207f, 0x207f},
    {0x2090, 0x209c},   {0x2102, 0x2102},   {0x2107, 0x2107},   {0x210a, 0x2113},
    {0x2115, 0x2115},   {0x2119, 0x211d},   {0x2124, 0x2124},   {0x2126, 0x2126},
    {0x2128, 0x2128},   {0x212a, 0x212d},   {0x212f, 0x2139},   {0x213c, 0x213f},
    {0x2145, 0x2149},   {0x214e, 0x214e},   {0x2183, 0x2184},   {0x2c00, 0x2ce4},
    {0x2ceb, 0x2cee},   {0x2cf2, 0x2cf3},   {0x2d00, 0x2d25},   {0x2d27, 0x2d27},
    {0x2d2d, 0x2d2d},   {0x2d30, 0x2d67},   {0x2d6f, 0x2d6f},   {0x2d80, 0x2d96},
    {0x2da0, 0x2da6},   {0x2da8, 0x2dae},   {0x2db0, 0x2db6},   {0x2db8, 0x2dbe},
    {0x2dc0, 0x2dc6},   {0x2dc8, 0x2dce},   {0x2dd0, 0x2dd6},   {0x2dd8, 0x2dde},
    {0x2e2f, 0x2e2f},   {0x3005, 0x3006},   {0x3031, 0x3035},   {0x303b, 0x303c},
    {0x3041, 0x3096},   {0x309d, 0x309f},   {0x30a1, 0x30fa},   {0x30fc, 0x30ff},
    {0x3105, 0x312f},   {0x3131, 0x318e},   {0x31a0, 0x31bf},   {0x31f0, 0x31ff},
    {0x3400, 0x4dbf},   {0x4e00, 0xa48c},   {0xa4d0, 0xa4fd},   {0xa500, 0xa60c},
    {0xa610, 0xa61f},   {0xa62a, 0xa62b},   {0xa640, 0xa66e},   {0xa67f, 0xa69d},
    {0xa6a0, 0xa6e5},   {0xa717, 0xa71f},   {0xa722, 0xa788},   {0xa78b, 0xa7dc},
    {0xa7f1, 0xa801},   {0xa803, 0xa805},   {0xa807, 0xa80a},   {0xa80c, 0xa822},
    {0xa840, 0xa873},   {0xa882, 0xa8b3},   {0xa8f2, 0xa8f7},   {0xa8fb, 0xa8fb},
    {0xa8fd, 0xa8fe},   {0xa90a, 0xa925},   {0xa930, 0xa946},   {0xa960, 0xa97c},
    {0xa984, 0xa9b2},   {0xa9cf, 0xa9cf},   {0xa9e0, 0xa9e4},   {0xa9e6, 0xa9ef},
    {0xa9fa, 0xa9fe},   {0xaa00, 0xaa28},   {0xaa40, 0xaa42},   {0xaa44, 0xaa4b},
    {0xaa60, 0xaa76},   {0xaa7a, 0xaa7a},   {0xaa7e, 0xaaaf},   {0xaab1, 0xaab1},
    {0xaab5, 0xaab6},   {0xaab9, 0xaabd},   {0xaac0, 0xaac0},   {0xaac2, 0xaac2},
    {0xaadb, 0xaadd},   {0xaae0, 0xaaea},   {0xaaf2, 0xaaf4},   {0xab01, 0xab06},
    {0xab09, 0xab0e},   {0xab11, 0xab16},   {0xab20, 0xab26},   {0xab28, 0xab2e},
    {0xab30, 0xab5a},   {0xab5c, 0xab69},   {0xab70, 0xabe2},   {0xac00, 0xd7a3},
    {0xd7b0, 0xd7c6},   {0xd7cb, 0xd7fb},   {0xf900, 0xfa6d},   {0xfa70, 0xfad9},
    {0xfb00, 0xfb06},   {0xfb13, 0xfb17},   {0xfb1d, 0xfb1d},   {0xfb1f, 0xfb28},
    {0xfb2a, 0xfb36},   {0xfb38, 0xfb3c},   {0xfb3e, 0xfb3e},   {0xfb40, 0xfb41},
    {0xfb43, 0xfb44},   {0xfb46, 0xfbb1},   {0xfbd3, 0xfd3d},   {0xfd50, 0xfd8f},
    {0xfd92, 0xfdc7},   {0xfdf0, 0xfdfb},   {0xfe70, 0xfe74},   {0xfe76, 0xfefc},
    {0xff21, 0xff3a},   {0xff41, 0xff5a},   {0xff66, 0xffbe},   {0xffc2, 0xffc7},
    {0xffca, 0xffcf},   {0xffd2, 0xffd7},   {0xffda, 0xffdc},   {0x10000, 0x1000b},
    {0x1000d, 0x10026}, {0x10028, 0x1003a}, {0x1003c, 0x1003d}, {0x1003f, 0x1004d},
    {0x10050, 0x1005d}, {0x10080, 0x100fa}, {0x10280, 0x1029c}, {0x102a0, 0x102d0},
    {0x10300, 0x1031f}, {0x1032d, 0x10340}, {0x10342, 0x10349}, {0x10350, 0x10375},
    {0x10380, 0x1039d}, {0x103a0, 0x103c3}, {0x103c8, 0x103cf}, {0x10400, 0x1049d},
    {0x104b0, 0x104d3}, {0x104d8, 0x104fb}, {0x10500, 0x10527}, {0x10530, 0x10563},
    {0x10570, 0x1057a}, {0x1057c, 0x1058a}, {0x1058c, 0x10592}, {0x10594, 0x10595},
    {0x10597, 0x105a1}, {0x105a3, 0x105b1}, {0x105b3, 0x105b9}, {0x105bb, 0x105bc},
    {0x105c0, 0x105f3}, {0x10600, 0x10736}, {0x10740, 0x10755}, {0x10760, 0x10767},
    {0x10780, 0x10785}, {0x10787, 0x107b0}, {0x107b2, 0x107ba}, {0x10800, 0x10805},
    {0x10808, 0x10808}, {0x1080a, 0x10835}, {0x10837, 0x10838}, {0x1083c, 0x1083c},
    {0x1083f, 0x10855}, {0x10860, 0x10876}, {0x10880, 0x1089e}, {0x108e0, 0x108f2},
    {0x108f4, 0x108f5}, {0x10900, 0x10915}, {0x10920, 0x10939}, {0x10940, 0x10959},
    {0x10980, 0x109b7}, {0x109be, 0x109bf}, {0x10a00, 0x10a00}, {0x10a10, 0x10a13},
    {0x10a15, 0x10a17}, {0x10a19, 0x10a35}, {0x10a60, 0x10a7c}, {0x10a80, 0x10a9c},
    {0x10ac0, 0x10ac7}, {0x10ac9, 0x10ae4}, {0x10b00, 0x10b35}, {0x10b40, 0x10b55},
    {0x10b60, 0x10b72}, {0x10b80, 0x10b91}, {0x10c00, 0x10c48}, {0x10c80, 0x10cb2},
    {0x10cc0, 0x10cf2}, {0x10d00, 0x10d23}, {0x10d4a, 0x10d65}, {0x10d6f, 0x10d85},
    {0x10e80, 0x10ea9}, {0x10eb0, 0x10eb1}, {0x10ec2, 0x10ec7}, {0x10f00, 0x10f1c},
    {0x10f27, 0x10f27}, {0x10f30, 0x10f45}, {0x10f70, 0x10f81}, {0x10fb0, 0x10fc4},
    {0x10fe0, 0x10ff6}, {0x11003, 0x11037}, {0x11071, 0x11072}, {0x11075, 0x11075},
    {0x11083, 0x110af}, {0x110d0, 0x110e8}, {0x11103, 0x11126}, {0x11144, 0x11144},
    {0x11147, 0x11147}, {0x11150, 0x11172}, {0x11176, 0x11176}, {0x11183, 0x111b2},
    {0x111c1, 0x111c4}, {0x111da, 0x111da}, {0x111dc, 0x111dc}, {0x11200, 0x11211},
    {0x11213, 0x1122b}, {0x1123f, 0x11240}, {0x11280, 0x11286}, {0x11288, 0x11288},
    {0x1128a, 0x1128d}, {0x1128f, 0x1129d}, {0x1129f, 0x112a8}, {0x112b0, 0x112de},
    {0x11305, 0x1130c}, {0x1130f, 0x11310}, {0x11313, 0x11328}, {0x1132a, 0x11330},
    {0x11332, 0x11333}, {0x11335, 0x11339}, {0x1133d, 0x1133d}, {0x11350, 0x11350},
    {0x1135d, 0x11361}, {0x11380, 0x11389}, {0x1138b, 0x1138b}, {0x1138e, 0x1138e},
    {0x11390, 0x113b5}, {0x113b7, 0x113b7}, {0x113d1, 0x113d1}, {0x113d3, 0x113d3},
    {0x11400, 0x11434}, {0x11447, 0x1144a}, {0x1145f, 0x11461}, {0x11480, 0x114af},
    {0x114c4, 0x114c5}, {0x114c7, 0x114c7}, {0x11580, 0x115ae}, {0x115d8, 0x115db},
    {0x11600, 0x1162f}, {0x11644, 0x11644}, {0x11680, 0x116aa}, {0x116b8, 0x116b8},
    {0x11700, 0x1171a}, {0x11740, 0x11746}, {0x11800, 0x1182b}, {0x118a0, 0x118df},
    {0x118ff, 0x11906}, {0x11909, 0x11909}, {0x1190c, 0x11913}, {0x11915, 0x11916},
    {0x11918, 0x1192f}, {0x1193f, 0x1193f}, {0x11941, 0x11941}, {0x119a0, 0x119a7},
    {0x119aa, 0x119d0}, {0x119e1, 0x119e1}, {0x119e3, 0x119e3}, {0x11a00, 0x11a00},
    {0x11a0b, 0x11a32}, {0x11a3a, 0x11a3a}, {0x11a50, 0x11a50}, {0x11a5c, 0x11a89},
    {0x11a9d, 0x11a9d}, {0x11ab0, 0x11af8}, {0x11bc0, 0x11be0}, {0x11c00, 0x11c08},
    {0x11c0a, 0x11c2e}, {0x11c40, 0x11c40}, {0x11c72, 0x11c8f}, {0x11d00, 0x11d06},
    {0x11d08, 0x11d09}, {0x11d0b, 0x11d30}, {0x11d46, 0x11d46}, {0x11d60, 0x11d65},
    {0x11d67, 0x11d68}, {0x11d6a, 0x11d89}, {0x11d98, 0x11d98}, {0x11db0, 0x11ddb},
    {0x11ee0, 0x11ef2}, {0x11f02, 0x11f02}, {0x11f04, 0x11f10}, {0x11f12, 0x11f33},
    {0x11fb0, 0x11fb0}, {0x12000, 0x12399}, {0x12480, 0x12543}, {0x12f90, 0x12ff0},
    {0x13000, 0x1342f}, {0x13441, 0x13446}, {0x13460, 0x143fa}, {0x14400, 0x14646},
    {0x16100, 0x1611d}, {0x16800, 0x16a38}, {0x16a40, 0x16a5e}, {0x16a70, 0x16abe},
    {0x16ad0, 0x16aed}, {0x16b00, 0x16b2f}, {0x16b40, 0x16b43}, {0x16b63, 0x16b77},
    {0x16b7d, 0x16b8f}, {0x16d40, 0x16d6c}, {0x16e40, 0x16e7f}, {0x16ea0, 0x16eb8},
    {0x16ebb, 0x16ed3}, {0x16f00, 0x16f4a}, {0x16f50, 0x16f50}, {0x16f93, 0x16f9f},
    {0x16fe0, 0x16fe1}, {0x16fe3, 0x16fe3}, {0x16ff2, 0x16ff3}, {0x17000, 0x18cd5},
    {0x18cff, 0x18d1e}, {0x18d80, 0x18df2}, {0x1aff0, 0x1aff3}, {0x1aff5, 0x1affb},
    {0x1affd, 0x1affe}, {0x1b000, 0x1b122}, {0x1b132, 0x1b132}, {0x1b150, 0x1b152},
    {0x1b155, 0x1b155}, {0x1b164, 0x1b167}, {0x1b170, 0x1b2fb}, {0x1bc00, 0x1bc6a},
    {0x1bc70, 0x1bc7c}, {0x1bc80, 0x1bc88}, {0x1bc90, 0x1bc99}, {0x1d400, 0x1d454},
    {0x1d456, 0x1d49c}, {0x1d49e, 0x1d49f}, {0x1d4a2, 0x1d4a2}, {0x1d4a5, 0x1d4a6},
    {0x1d4a9, 0x1d4ac}, {0x1d4ae, 0x1d4b9}, {0x1d4bb, 0x1d4bb}, {0x1d4bd, 0x1d4c3},
    {0x1d4c5, 0x1d505}, {0x1d507, 0x1d50a}, {0x1d50d, 0x1d514}, {0x1d516, 0x1d51c},
    {0x1d51e, 0x1d539}, {0x1d53b, 0x1d53e}, {0x1d540, 0x1d544}, {0x1d546, 0x1d546},
    {0x1d54a, 0x1d550}, {0x1d552, 0x1d6a5}, {0x1d6a8, 0x1d6c0}, {0x1d6c2, 0x1d6da},
    {0x1d6dc, 0x1d6fa}, {0x1d6fc, 0x1d714}, {0x1d716, 0x1d734}, {0x1d736, 0x1d74e},
    {0x1d750, 0x1d76e}, {0x1d770, 0x1d788}, {0x1d78a, 0x1d7a8}, {0x1d7aa, 0x1d7c2},
    {0x1d7c4, 0x1d7cb}, {0x1df00, 0x1df1e}, {0x1df25, 0x1df2a}, {0x1e030, 0x1e06d},
    {0x1e100, 0x1e12c}, {0x1e137, 0x1e13d}, {0x1e14e, 0x1e14e}, {0x1e290, 0x1e2ad},
    {0x1e2c0, 0x1e2eb}, {0x1e4d0, 0x1e4eb}, {0x1e5d0, 0x1e5ed}, {0x1e5f0, 0x1e5f0},
    {0x1e6c0, 0x1e6de}, {0x1e6e0, 0x1e6e2}, {0x1e6e4, 0x1e6e5}, {0x1e6e7, 0x1e6ed},
    {0x1e6f0, 0x1e6f4}, {0x1e6fe, 0x1e6ff}, {0x1e7e0, 0x1e7e6}, {0x1e7e8, 0x1e7eb},
    {0x1e7ed, 0x1e7ee}, {0x1e7f0, 0x1e7fe}, {0x1e800, 0x1e8c4}, {0x1e900, 0x1e943},
    {0x1e94b, 0x1e94b}, {0x1ee00, 0x1ee03}, {0x1ee05, 0x1ee1f}, {0x1ee21, 0x1ee22},
    {0x1ee24, 0x1ee24}, {0x1ee27, 0x1ee27}, {0x1ee29, 0x1ee32}, {0x1ee34, 0x1ee37},
    {0x1ee39, 0x1ee39}, {0x1ee3b, 0x1ee3b}, {0x1ee42, 0x1ee42}, {0x1ee47, 0x1ee47},
    {0x1ee49, 0x1ee49}, {0x1ee4b, 0x1ee4b}, {0x1ee4d, 0x1ee4f}, {0x1ee51, 0x1ee52},
    {0x1ee54, 0x1ee54}, {0x1ee57, 0x1ee57}, {0x1ee59, 0x1ee59}, {0x1ee5b, 0x1ee5b},
    {0x1ee5d, 0x1ee5d}, {0x1ee5f, 0x1ee5f}, {0x1ee61, 0x1ee62}, {0x1ee64, 0x1ee64},
    {0x1ee67, 0x1ee6a}, {0x1ee6c, 0x1ee72}, {0x1ee74, 0x1ee77}, {0x1ee79, 0x1ee7c},
    {0x1ee7e, 0x1ee7e}, {0x1ee80, 0x1ee89}, {0x1ee8b, 0x1ee9b}, {0x1eea1, 0x1eea3},
    {0x1eea5, 0x1eea9}, {0x1eeab, 0x1eebb}, {0x20000, 0x2a6df}, {0x2a700, 0x2b81d},
    {0x2b820, 0x2cead}, {0x2ceb0, 0x2ebe0}, {0x2ebf0, 0x2ee5d}, {0x2f800, 0x2fa1d},
    {0x30000, 0x3134a}, {0x31350, 0x33479},
};

static const uint32_t corpus_digits[][2] = {
    {0x660, 0x669},     {0x6f0, 0x6f9},     {0x7c0, 0x7c9},     {0x966, 0x96f},
    {0x9e6, 0x9ef},     {0xa66, 0xa6f},     {0xae6, 0xaef},     {0xb66, 0xb6f},
    {0xbe6, 0xbef},     {0xc66, 0xc6f},     {0xce6, 0xcef},     {0xd66, 0xd6f},
    {0xde6, 0xdef},     {0xe50, 0xe59},     {0xed0, 0xed9},     {0xf20, 0xf29},
    {0x1040, 0x1049},   {0x1090, 0x1099},   {0x17e0, 0x17e9},   {0x1810, 0x1819},
    {0x1946, 0x194f},   {0x19d0, 0x19d9},   {0x1a80, 0x1a89},   {0x1a90, 0x1a99},
    {0x1b50, 0x1b59},   {0x1bb0, 0x1bb9},   {0x1c40, 0x1c49},   {0x1c50, 0x1c59},
    {0xa620, 0xa629},   {0xa8d0, 0xa8d9},   {0xa900, 0xa909},   {0xa9d0, 0xa9d9},
    {0xa9f0, 0xa9f9},   {0xaa50, 0xaa59},   {0xabf0, 0xabf9},   {0xff10, 0xff19},
    {0x104a0, 0x104a9}, {0x10d30, 0x10d39}, {0x10d40, 0x10d49}, {0x11066, 0x1106f},
    {0x110f0, 0x110f9}, {0x11136, 0x1113f}, {0x111d0, 0x111d9}, {0x112f0, 0x112f9},
    {0x11450, 0x11459}, {0x114d0, 0x114d9}, {0x11650, 0x11659}, {0x116c0, 0x116c9},
    {0x116d0, 0x116e3}, {0x11730, 0x11739}, {0x118e0, 0x118e9}, {0x11950, 0x11959},
    {0x11bf0, 0x11bf9}, {0x11c50, 0x11c59}, {0x11d50, 0x11d59}, {0x11da0, 0x11da9},
    {0x11de0, 0x11de9}, {0x11f50, 0x11f59}, {0x16130, 0x16139}, {0x16a60, 0x16a69},
    {0x16ac0, 0x16ac9}, {0x16b50, 0x16b59}, {0x16d70, 0x16d79}, {0x1ccf0, 0x1ccf9},
    {0x1d7ce, 0x1d7ff}, {0x1e140, 0x1e149}, {0x1e2f0, 0x1e2f9}, {0x1e4f0, 0x1e4f9},
    {0x1e5f1, 0x1e5fa}, {0x1e950, 0x1e959}, {0x1fbf0, 0x1fbf9},
};

static bool corpus_in_table(const uint32_t (*t)[2], size_t n, int32_t r) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if ((uint32_t)r < t[m][0])
            hi = m;
        else if ((uint32_t)r > t[m][1])
            lo = m + 1;
        else
            return true;
    }
    return false;
}

/* ----------------------------------------------------------------- scanner */

#define CORPUS_EOF (-1)
#define CORPUS_BOM 0xFEFF

static void corpus_scan_error(CorpusScanner *s, Int offs, Str msg) {
    burrow__testing_corpus_error_add(s->file, offs + 1, msg);
}

static void corpus_scan_errorc(CorpusScanner *s, Int offs, const char *msg) {
    corpus_scan_error(s, offs, str_from_cstr(msg));
}

/* Read the next Unicode char into s.ch. */
static void corpus_scan_next(CorpusScanner *s) {
    if (s->rd_offset < s->src.len) {
        s->offset = s->rd_offset;
        int32_t r = s->src.p[s->rd_offset];
        Int w = 1;
        if (r == 0) {
            corpus_scan_errorc(s, s->offset, "illegal character NUL");
        } else if (r >= 0x80) {
            Str in = str_from_bytes(s->src.p + s->rd_offset, s->src.len - s->rd_offset);
            r = utf8_decode_rune_in_string(in, &w);
            if (r == UTF8_RUNE_ERROR && w == 1) {
                if (s->offset == 0 && in.len >= 2 &&
                    ((in.p[0] == 0xFF && in.p[1] == 0xFE) ||
                     (in.p[0] == 0xFE && in.p[1] == 0xFF))) {
                    /* A byte order mark at the start of the file in UTF-16. */
                    corpus_scan_errorc(s, s->offset,
                                       "illegal UTF-8 encoding (got UTF-16)");
                    s->rd_offset += in.len;
                } else {
                    corpus_scan_errorc(s, s->offset, "illegal UTF-8 encoding");
                }
            } else if (r == CORPUS_BOM && s->offset > 0) {
                corpus_scan_errorc(s, s->offset, "illegal byte order mark");
            }
        }
        s->rd_offset += w;
        s->ch = r;
    } else {
        s->offset = s->src.len;
        s->ch = CORPUS_EOF;
    }
}

static Byte corpus_scan_peek(const CorpusScanner *s) {
    if (s->rd_offset < s->src.len)
        return s->src.p[s->rd_offset];
    return 0;
}

void burrow__testing_corpus_scan_init(CorpusScanner *s, CorpusFile *file, Str src) {
    memset(s, 0, sizeof *s);
    s->file = file;
    s->src = src;
    s->ch = ' ';
    corpus_scan_next(s);
    if (s->ch == CORPUS_BOM)
        corpus_scan_next(s);
}

Int burrow__testing_corpus_scan_end(const CorpusScanner *s) {
    return s->offset + 1;
}

static int32_t corpus_lower(int32_t ch) {
    return ('a' - 'A') | ch;
}

static bool corpus_is_decimal(int32_t ch) {
    return '0' <= ch && ch <= '9';
}

static bool corpus_is_hex(int32_t ch) {
    return ('0' <= ch && ch <= '9') ||
           ('a' <= corpus_lower(ch) && corpus_lower(ch) <= 'f');
}

static bool corpus_is_letter(int32_t ch) {
    return ('a' <= corpus_lower(ch) && corpus_lower(ch) <= 'z') || ch == '_' ||
           (ch >= 0x80 &&
            corpus_in_table(corpus_letters,
                            sizeof corpus_letters / sizeof corpus_letters[0], ch));
}

static bool corpus_is_digit(int32_t ch) {
    return corpus_is_decimal(ch) ||
           (ch >= 0x80 &&
            corpus_in_table(corpus_digits,
                            sizeof corpus_digits / sizeof corpus_digits[0], ch));
}

static int corpus_digit_val(int32_t ch) {
    if ('0' <= ch && ch <= '9')
        return ch - '0';
    if ('a' <= corpus_lower(ch) && corpus_lower(ch) <= 'f')
        return corpus_lower(ch) - 'a' + 10;
    return 16;
}

static Str corpus_src(const CorpusScanner *s, Int from, Int to) {
    return str_from_bytes(s->src.p + from, to - from);
}

/* Path.Clean, which is what filepath.Clean is outside Windows. On Windows Go
 * also turns slashes round and treats a volume name as a root, and a line
 * directive naming a file is the one place that shows. */
static Str corpus_clean(Alloc *a, Str path) {
    Int n = path.len;
    bool rooted = path.p[0] == '/';
    Byte *out = burrow__testing_corpus_alloc(a, (size_t)n + 1);
    Int w = 0, r = 0, dotdot = 0;
    if (rooted) {
        out[w++] = '/';
        r = 1;
        dotdot = 1;
    }
    while (r < n) {
        if (path.p[r] == '/' ||
            (path.p[r] == '.' && (r + 1 == n || path.p[r + 1] == '/'))) {
            /* an empty element, or a . element */
            r++;
        } else if (path.p[r] == '.' && path.p[r + 1] == '.' &&
                   (r + 2 == n || path.p[r + 2] == '/')) {
            r += 2;
            if (w > dotdot) {
                w--;
                while (w > dotdot && out[w] != '/')
                    w--;
            } else if (!rooted) {
                if (w > 0)
                    out[w++] = '/';
                out[w++] = '.';
                out[w++] = '.';
                dotdot = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0))
                out[w++] = '/';
            for (; r < n && path.p[r] != '/'; r++)
                out[w++] = path.p[r];
        }
    }
    if (w == 0)
        out[w++] = '.';
    return str_from_bytes(out, w);
}

/* trailingDigits. ParseUint with base 10 and the int size, read as an int the
 * way Go's conversion does, so that a number past the top of an int64 comes
 * out negative in both. */
static Int corpus_trailing_digits(Str text, int64_t *n, bool *ok) {
    Int i = text.len - 1;
    while (i >= 0 && text.p[i] != ':')
        i--;
    *n = 0;
    *ok = false;
    if (i < 0)
        return 0;
    Error err = {0};
    uint64_t u = strconv_parse_uint(str_from_bytes(text.p + i + 1, text.len - i - 1),
                                    10, 64, &err);
    *n = (int64_t)u;
    *ok = BURROW_OK(err);
    return i + 1;
}

/* updateLineInfo. */
static void corpus_update_line_info(CorpusScanner *s, Int next, Int offs, Str text) {
    const int64_t max_line_col = (int64_t)1 << 30;
    Alloc *a = s->file->a;
    if (text.p[1] == '*')
        text.len -= 2;
    text = str_from_bytes(text.p + 7, text.len - 7);
    offs += 7;

    int64_t n;
    bool ok;
    Int i = corpus_trailing_digits(text, &n, &ok);
    if (i == 0)
        return;
    if (!ok) {
        corpus_scan_error(s, offs + i,
                          fmt_sprintf_v(a, "invalid line number: %s",
                                        str_from_bytes(text.p + i, text.len - i)));
        return;
    }
    int64_t line, col = 0;
    int64_t n2;
    bool ok2;
    Int i2 = corpus_trailing_digits(str_from_bytes(text.p, i - 1), &n2, &ok2);
    if (ok2) {
        Int t = i;
        i = i2;
        i2 = t;
        line = n2;
        col = n;
        if (col == 0 || col > max_line_col) {
            corpus_scan_error(
                s, offs + i2,
                fmt_sprintf_v(a, "invalid column number: %s",
                              str_from_bytes(text.p + i2, text.len - i2)));
            return;
        }
        text.len = i2 - 1;
    } else {
        line = n;
    }
    if (line == 0 || line > max_line_col) {
        corpus_scan_error(s, offs + i,
                          fmt_sprintf_v(a, "invalid line number: %s",
                                        str_from_bytes(text.p + i, text.len - i)));
        return;
    }
    Str filename = str_from_bytes(text.p, i - 1);
    if (filename.len == 0 && ok2)
        filename = burrow__testing_corpus_position(s->file, offs + 1).filename;
    else if (filename.len > 0)
        filename = corpus_clean(a, filename);
    corpus_add_line_info(s->file, next, filename, line, col);
}

/* scanComment, without the text, which the parser drops. */
static void corpus_scan_comment(CorpusScanner *s) {
    Int offs = s->offset - 1;
    Int next = -1;
    Int num_cr = 0;

    if (s->ch == '/') {
        corpus_scan_next(s);
        while (s->ch != '\n' && s->ch >= 0) {
            if (s->ch == '\r')
                num_cr++;
            corpus_scan_next(s);
        }
        next = s->offset;
        if (s->ch == '\n')
            next++;
        goto exit;
    }

    corpus_scan_next(s);
    while (s->ch >= 0) {
        int32_t ch = s->ch;
        if (ch == '\r')
            num_cr++;
        corpus_scan_next(s);
        if (ch == '*' && s->ch == '/') {
            corpus_scan_next(s);
            next = s->offset;
            goto exit;
        }
    }

    corpus_scan_errorc(s, offs, "comment not terminated");

exit:;
    Str lit = corpus_src(s, offs, s->offset);
    if (num_cr > 0 && lit.len >= 2 && lit.p[1] == '/' && lit.p[lit.len - 1] == '\r')
        lit.len--;
    /* A //line directive has to start its line, and the line starts at 0. */
    if (next >= 0 && (lit.p[1] == '*' || offs == 0) && lit.len >= 7 &&
        memcmp(lit.p + 2, "line ", 5) == 0)
        corpus_update_line_info(s, next, offs, lit);
}

static Str corpus_scan_identifier(CorpusScanner *s) {
    Int offs = s->offset;
    while (corpus_is_letter(s->ch) || corpus_is_digit(s->ch))
        corpus_scan_next(s);
    return corpus_src(s, offs, s->offset);
}

/* digits. */
static int corpus_scan_digits(CorpusScanner *s, int base, Int *invalid) {
    int digsep = 0;
    if (base <= 10) {
        int32_t max = '0' + base;
        while (corpus_is_decimal(s->ch) || s->ch == '_') {
            int ds = 1;
            if (s->ch == '_')
                ds = 2;
            else if (s->ch >= max && invalid != NULL && *invalid < 0)
                *invalid = s->offset;
            digsep |= ds;
            corpus_scan_next(s);
        }
    } else {
        while (corpus_is_hex(s->ch) || s->ch == '_') {
            digsep |= s->ch == '_' ? 2 : 1;
            corpus_scan_next(s);
        }
    }
    return digsep;
}

static const char *corpus_litname(int32_t prefix) {
    switch (prefix) {
    case 'x':
        return "hexadecimal literal";
    case 'o':
    case '0':
        return "octal literal";
    case 'b':
        return "binary literal";
    default:
        break;
    }
    return "decimal literal";
}

/* invalidSep. */
static Int corpus_invalid_sep(Str x) {
    int32_t x1 = ' ';
    int32_t d = '.';
    Int i = 0;
    if (x.len >= 2 && x.p[0] == '0') {
        x1 = corpus_lower(x.p[1]);
        if (x1 == 'x' || x1 == 'o' || x1 == 'b') {
            d = '0';
            i = 2;
        }
    }
    for (; i < x.len; i++) {
        int32_t p = d;
        d = x.p[i];
        if (d == '_') {
            if (p != '0')
                return i;
        } else if (corpus_is_decimal(d) || (x1 == 'x' && corpus_is_hex(d))) {
            d = '0';
        } else {
            if (p == '_')
                return i - 1;
            d = '.';
        }
    }
    if (d == '_')
        return x.len - 1;
    return -1;
}

static CorpusTok corpus_scan_number(CorpusScanner *s, Str *lit) {
    Alloc *a = s->file->a;
    Int offs = s->offset;
    CorpusTok tok = CT_ILLEGAL;
    int base = 10;
    int32_t prefix = 0;
    int digsep = 0;
    Int invalid = -1;

    if (s->ch != '.') {
        tok = CT_INT;
        if (s->ch == '0') {
            corpus_scan_next(s);
            switch (corpus_lower(s->ch)) {
            case 'x':
                corpus_scan_next(s);
                base = 16;
                prefix = 'x';
                break;
            case 'o':
                corpus_scan_next(s);
                base = 8;
                prefix = 'o';
                break;
            case 'b':
                corpus_scan_next(s);
                base = 2;
                prefix = 'b';
                break;
            default:
                base = 8;
                prefix = '0';
                digsep = 1;
            }
        }
        digsep |= corpus_scan_digits(s, base, &invalid);
    }

    if (s->ch == '.') {
        tok = CT_FLOAT;
        if (prefix == 'o' || prefix == 'b')
            corpus_scan_error(
                s, s->offset,
                fmt_sprintf_v(a, "invalid radix point in %s", corpus_litname(prefix)));
        corpus_scan_next(s);
        digsep |= corpus_scan_digits(s, base, &invalid);
    }

    if ((digsep & 1) == 0)
        corpus_scan_error(s, s->offset,
                          fmt_sprintf_v(a, "%s has no digits", corpus_litname(prefix)));

    int32_t e = corpus_lower(s->ch);
    if (e == 'e' || e == 'p') {
        if (e == 'e' && prefix != 0 && prefix != '0')
            corpus_scan_error(s, s->offset,
                              fmt_sprintf_v(a,
                                            "'%c' exponent requires decimal mantissa",
                                            (int)s->ch));
        else if (e == 'p' && prefix != 'x')
            corpus_scan_error(
                s, s->offset,
                fmt_sprintf_v(a, "'%c' exponent requires hexadecimal mantissa",
                              (int)s->ch));
        corpus_scan_next(s);
        tok = CT_FLOAT;
        if (s->ch == '+' || s->ch == '-')
            corpus_scan_next(s);
        int ds = corpus_scan_digits(s, 10, NULL);
        digsep |= ds;
        if ((ds & 1) == 0)
            corpus_scan_errorc(s, s->offset, "exponent has no digits");
    } else if (prefix == 'x' && tok == CT_FLOAT) {
        corpus_scan_errorc(s, s->offset,
                           "hexadecimal mantissa requires a 'p' exponent");
    }

    if (s->ch == 'i') {
        tok = CT_IMAG;
        corpus_scan_next(s);
    }

    *lit = corpus_src(s, offs, s->offset);
    if (tok == CT_INT && invalid >= 0)
        corpus_scan_error(s, invalid,
                          fmt_sprintf_v(a, "invalid digit '%c' in %s",
                                        (int)lit->p[invalid - offs],
                                        corpus_litname(prefix)));
    if (digsep & 2) {
        Int i = corpus_invalid_sep(*lit);
        if (i >= 0)
            corpus_scan_errorc(s, offs + i, "'_' must separate successive digits");
    }
    return tok;
}

/* scanEscape. */
static bool corpus_scan_escape(CorpusScanner *s, int32_t quote) {
    Alloc *a = s->file->a;
    Int offs = s->offset;
    int n;
    uint32_t base, max;
    switch (s->ch) {
    case 'a':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
    case 'v':
    case '\\':
        corpus_scan_next(s);
        return true;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        n = 3;
        base = 8;
        max = 255;
        break;
    case 'x':
        corpus_scan_next(s);
        n = 2;
        base = 16;
        max = 255;
        break;
    case 'u':
        corpus_scan_next(s);
        n = 4;
        base = 16;
        max = UTF8_MAX_RUNE;
        break;
    case 'U':
        corpus_scan_next(s);
        n = 8;
        base = 16;
        max = UTF8_MAX_RUNE;
        break;
    default:
        if (s->ch == quote) {
            corpus_scan_next(s);
            return true;
        }
        corpus_scan_errorc(s, offs,
                           s->ch < 0 ? "escape sequence not terminated"
                                     : "unknown escape sequence");
        return false;
    }

    uint32_t x = 0;
    for (; n > 0; n--) {
        uint32_t d = (uint32_t)corpus_digit_val(s->ch);
        if (d >= base) {
            if (s->ch < 0)
                corpus_scan_errorc(s, s->offset, "escape sequence not terminated");
            else
                corpus_scan_error(
                    s, s->offset,
                    fmt_sprintf_v(a, "illegal character %#U in escape sequence",
                                  s->ch));
            return false;
        }
        x = x * base + d;
        corpus_scan_next(s);
    }

    if (x > max || (0xD800 <= x && x < 0xE000)) {
        corpus_scan_errorc(s, offs, "escape sequence is invalid Unicode code point");
        return false;
    }
    return true;
}

static Str corpus_scan_rune(CorpusScanner *s) {
    Int offs = s->offset - 1;
    bool valid = true;
    Int n = 0;
    for (;;) {
        int32_t ch = s->ch;
        if (ch == '\n' || ch < 0) {
            if (valid) {
                corpus_scan_errorc(s, offs, "rune literal not terminated");
                valid = false;
            }
            break;
        }
        corpus_scan_next(s);
        if (ch == '\'')
            break;
        n++;
        if (ch == '\\') {
            if (!corpus_scan_escape(s, '\''))
                valid = false;
        }
    }
    if (valid && n != 1)
        corpus_scan_errorc(s, offs, "illegal rune literal");
    return corpus_src(s, offs, s->offset);
}

static Str corpus_scan_string(CorpusScanner *s) {
    Int offs = s->offset - 1;
    for (;;) {
        int32_t ch = s->ch;
        if (ch == '\n' || ch < 0) {
            corpus_scan_errorc(s, offs, "string literal not terminated");
            break;
        }
        corpus_scan_next(s);
        if (ch == '"')
            break;
        if (ch == '\\')
            corpus_scan_escape(s, '"');
    }
    return corpus_src(s, offs, s->offset);
}

/* scanRawString. Go strips any carriage return from the text, which is a copy. */
static Str corpus_scan_raw_string(CorpusScanner *s) {
    Int offs = s->offset - 1;
    bool has_cr = false;
    for (;;) {
        int32_t ch = s->ch;
        if (ch < 0) {
            corpus_scan_errorc(s, offs, "raw string literal not terminated");
            break;
        }
        corpus_scan_next(s);
        if (ch == '`')
            break;
        if (ch == '\r')
            has_cr = true;
    }
    Str lit = corpus_src(s, offs, s->offset);
    if (has_cr) {
        Byte *c = burrow__testing_corpus_alloc(s->file->a, (size_t)lit.len + 1);
        Int w = 0;
        for (Int j = 0; j < lit.len; j++)
            if (lit.p[j] != '\r')
                c[w++] = lit.p[j];
        lit = str_from_bytes(c, w);
    }
    return lit;
}

static void corpus_skip_whitespace(CorpusScanner *s) {
    while (s->ch == ' ' || s->ch == '\t' || (s->ch == '\n' && !s->insert_semi) ||
           s->ch == '\r')
        corpus_scan_next(s);
}

static CorpusTok corpus_switch2(CorpusScanner *s, CorpusTok tok0, CorpusTok tok1) {
    if (s->ch == '=') {
        corpus_scan_next(s);
        return tok1;
    }
    return tok0;
}

static CorpusTok corpus_switch3(CorpusScanner *s, CorpusTok tok0, CorpusTok tok1,
                                int32_t ch2, CorpusTok tok2) {
    if (s->ch == '=') {
        corpus_scan_next(s);
        return tok1;
    }
    if (s->ch == ch2) {
        corpus_scan_next(s);
        return tok2;
    }
    return tok0;
}

static CorpusTok corpus_switch4(CorpusScanner *s, CorpusTok tok0, CorpusTok tok1,
                                int32_t ch2, CorpusTok tok2, CorpusTok tok3) {
    if (s->ch == '=') {
        corpus_scan_next(s);
        return tok1;
    }
    if (s->ch == ch2) {
        corpus_scan_next(s);
        if (s->ch == '=') {
            corpus_scan_next(s);
            return tok3;
        }
        return tok2;
    }
    return tok0;
}

/* Scan. Comments are skipped here rather than handed to the parser to skip,
 * which comes to the same thing: go/parser drops them when it is not asked to
 * keep them, and keeps nothing else from them. */
CorpusTok burrow__testing_corpus_scan(CorpusScanner *s, Int *pos, Str *lit) {
    *lit = (Str){0};
    if (s->file->bailed) {
        *pos = s->src.len + 1;
        return CT_EOF;
    }
scan_again:
    corpus_skip_whitespace(s);

    *pos = s->offset + 1;

    bool insert_semi = false;
    CorpusTok tok;
    int32_t ch = s->ch;
    if (corpus_is_letter(ch)) {
        *lit = corpus_scan_identifier(s);
        if (lit->len > 1) {
            tok = corpus_lookup(*lit);
            switch ((int)tok) {
            case CT_IDENT:
            case CT_BREAK:
            case CT_CONTINUE:
            case CT_FALLTHROUGH:
            case CT_RETURN:
                insert_semi = true;
            default:
                break;
            }
        } else {
            insert_semi = true;
            tok = CT_IDENT;
        }
    } else if (corpus_is_decimal(ch) ||
               (ch == '.' && corpus_is_decimal(corpus_scan_peek(s)))) {
        insert_semi = true;
        tok = corpus_scan_number(s, lit);
    } else {
        corpus_scan_next(s);
        switch (ch) {
        case CORPUS_EOF:
            if (s->insert_semi) {
                s->insert_semi = false;
                *lit = BURROW_S("\n");
                return CT_SEMICOLON;
            }
            tok = CT_EOF;
            break;
        case '\n':
            s->insert_semi = false;
            *lit = BURROW_S("\n");
            return CT_SEMICOLON;
        case '"':
            insert_semi = true;
            tok = CT_STRING;
            *lit = corpus_scan_string(s);
            break;
        case '\'':
            insert_semi = true;
            tok = CT_CHAR;
            *lit = corpus_scan_rune(s);
            break;
        case '`':
            insert_semi = true;
            tok = CT_STRING;
            *lit = corpus_scan_raw_string(s);
            break;
        case ':':
            tok = corpus_switch2(s, CT_COLON, CT_DEFINE);
            break;
        case '.':
            tok = CT_PERIOD;
            if (s->ch == '.' && corpus_scan_peek(s) == '.') {
                corpus_scan_next(s);
                corpus_scan_next(s);
                tok = CT_ELLIPSIS;
            }
            break;
        case ',':
            tok = CT_COMMA;
            break;
        case ';':
            tok = CT_SEMICOLON;
            *lit = BURROW_S(";");
            break;
        case '(':
            tok = CT_LPAREN;
            break;
        case ')':
            insert_semi = true;
            tok = CT_RPAREN;
            break;
        case '[':
            tok = CT_LBRACK;
            break;
        case ']':
            insert_semi = true;
            tok = CT_RBRACK;
            break;
        case '{':
            tok = CT_LBRACE;
            break;
        case '}':
            insert_semi = true;
            tok = CT_RBRACE;
            break;
        case '+':
            tok = corpus_switch3(s, CT_ADD, CT_ADD_ASSIGN, '+', CT_INC);
            if (tok == CT_INC)
                insert_semi = true;
            break;
        case '-':
            tok = corpus_switch3(s, CT_SUB, CT_SUB_ASSIGN, '-', CT_DEC);
            if (tok == CT_DEC)
                insert_semi = true;
            break;
        case '*':
            tok = corpus_switch2(s, CT_MUL, CT_MUL_ASSIGN);
            break;
        case '/':
            if (s->ch == '/' || s->ch == '*') {
                /* A comment. The newline inside a block comment that Go turns
                 * into a semicolon cannot happen on one line, so all that is
                 * left is to carry insertSemi over it. */
                corpus_scan_comment(s);
                goto scan_again;
            }
            tok = corpus_switch2(s, CT_QUO, CT_QUO_ASSIGN);
            break;
        case '%':
            tok = corpus_switch2(s, CT_REM, CT_REM_ASSIGN);
            break;
        case '^':
            tok = corpus_switch2(s, CT_XOR, CT_XOR_ASSIGN);
            break;
        case '<':
            if (s->ch == '-') {
                corpus_scan_next(s);
                tok = CT_ARROW;
            } else {
                tok = corpus_switch4(s, CT_LSS, CT_LEQ, '<', CT_SHL, CT_SHL_ASSIGN);
            }
            break;
        case '>':
            tok = corpus_switch4(s, CT_GTR, CT_GEQ, '>', CT_SHR, CT_SHR_ASSIGN);
            break;
        case '=':
            tok = corpus_switch2(s, CT_ASSIGN, CT_EQL);
            break;
        case '!':
            tok = corpus_switch2(s, CT_NOT, CT_NEQ);
            break;
        case '&':
            if (s->ch == '^') {
                corpus_scan_next(s);
                tok = corpus_switch2(s, CT_AND_NOT, CT_AND_NOT_ASSIGN);
            } else {
                tok = corpus_switch3(s, CT_AND, CT_AND_ASSIGN, '&', CT_LAND);
            }
            break;
        case '|':
            tok = corpus_switch3(s, CT_OR, CT_OR_ASSIGN, '|', CT_LOR);
            break;
        case '~':
            tok = CT_TILDE;
            break;
        default:
            /* next reports unexpected byte order marks, so they are not
             * reported twice. */
            if (ch != CORPUS_BOM) {
                Alloc *a = s->file->a;
                if (ch == 0x201C)
                    corpus_scan_errorc(
                        s, *pos - 1,
                        "curly quotation mark '\xe2\x80\x9c' (use neutral '\"')");
                else if (ch == 0x201D)
                    corpus_scan_errorc(
                        s, *pos - 1,
                        "curly quotation mark '\xe2\x80\x9d' (use neutral '\"')");
                else
                    corpus_scan_error(s, *pos - 1,
                                      fmt_sprintf_v(a, "illegal character %#U", ch));
            }
            insert_semi = s->insert_semi;
            /* Go puts the character in the literal, where nothing reads it. */
            tok = CT_ILLEGAL;
        }
    }
    s->insert_semi = insert_semi;
    return tok;
}
