/* regexp/syntax, the parser and compiler under package regexp.
 *
 * Parse turns a pattern into a tree of SyntaxRegexp nodes, Simplify rewrites
 * counted repetitions into the plain operators, and Compile turns a tree into a
 * SyntaxProg, the instruction list the matchers in regexp run.
 *
 *     Error err;
 *     SyntaxRegexp *re = syntax_parse(a, BURROW_S("a(b|c)*d"), SYNTAX_PERL, &err);
 *     SyntaxRegexp *s = syntax_regexp_simplify(re, a);
 *     SyntaxProg *prog = syntax_compile(a, s, &err);
 *     ...
 *     syntax_prog_free(prog);
 *     syntax_regexp_free(s);
 *     syntax_regexp_free(re);
 *
 * Most programs want package regexp and never touch this one. It is here for
 * the ones that inspect or rewrite patterns.
 *
 * The syntax is Go's, which is RE2's, described in full in docs/guides/regexp.md.
 *
 * Memory. A tree from syntax_parse or syntax_regexp_simplify lives in one
 * block from the allocator it was given, nodes, runes and names together, and
 * syntax_regexp_free gives the whole block back. Nodes inside it can be shared
 * by more than one parent, as in Go, so a tree is really a DAG and must not be
 * freed node by node. Simplify always builds a new block: where Go can hand
 * back the same tree or reuse parts of it, this copies, so the two trees can be
 * freed in either order. A program from syntax_compile is one block too, and
 * owns copies of the runes it matches, so it outlives the tree it came from.
 *
 * Trees built by hand work with every function here except the two free
 * functions, which only take what this package made.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package regexp/syntax */

#ifndef BURROW_REGEXP_SYNTAX_H
#define BURROW_REGEXP_SYNTAX_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- errors */

/* syntax.ErrorCode: what went wrong, as its text. The codes are the Str
 * macros below, and syntax_error_code_string gives the text back. */
typedef Str SyntaxErrorCode;

#define SYNTAX_ERR_INTERNAL_ERROR BURROW_S("regexp/syntax: internal error")
#define SYNTAX_ERR_INVALID_CHAR_CLASS BURROW_S("invalid character class")
#define SYNTAX_ERR_INVALID_CHAR_RANGE BURROW_S("invalid character class range")
#define SYNTAX_ERR_INVALID_ESCAPE BURROW_S("invalid escape sequence")
#define SYNTAX_ERR_INVALID_NAMED_CAPTURE BURROW_S("invalid named capture")
#define SYNTAX_ERR_INVALID_PERL_OP BURROW_S("invalid or unsupported Perl syntax")
#define SYNTAX_ERR_INVALID_REPEAT_OP BURROW_S("invalid nested repetition operator")
#define SYNTAX_ERR_INVALID_REPEAT_SIZE BURROW_S("invalid repeat count")
#define SYNTAX_ERR_INVALID_UTF8 BURROW_S("invalid UTF-8")
#define SYNTAX_ERR_MISSING_BRACKET BURROW_S("missing closing ]")
#define SYNTAX_ERR_MISSING_PAREN BURROW_S("missing closing )")
#define SYNTAX_ERR_MISSING_REPEAT_ARGUMENT                                             \
    BURROW_S("missing argument to repetition operator")
#define SYNTAX_ERR_TRAILING_BACKSLASH                                                  \
    BURROW_S("trailing backslash at end of expression")
#define SYNTAX_ERR_UNEXPECTED_PAREN BURROW_S("unexpected )")
#define SYNTAX_ERR_NESTING_DEPTH BURROW_S("expression nests too deeply")
#define SYNTAX_ERR_LARGE BURROW_S("expression too large")

/* ErrorCode.String: the code itself. */
BURROW_BORROWS(ret, e) Str syntax_error_code_string(SyntaxErrorCode e);

/* syntax.Error: a pattern that did not parse, with the code and the part of
 * the pattern it is about. The message is "error parsing regexp: " and the
 * code, then ": `" and the expression and "`", as in "error parsing regexp:
 * missing closing ): `(a`". errors_as with TYPE_SYNTAX_ERROR gives a pointer
 * to the SyntaxError inside an error from syntax_parse. As in Go, where the
 * error is a pointer, two of them are only errors_is each other when they are
 * the same one. */
typedef struct SyntaxError {
    SyntaxErrorCode code;
    Str expr;
} SyntaxError;

extern const Type *const TYPE_SYNTAX_ERROR;

/* Error.Error, built in a. */
BURROW_OWNS(ret) Str syntax_error_error(const SyntaxError *e, Alloc *a);

/* An Error for e, built in a with its own copy of the text. Out of memory
 * gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error syntax_error_as_error(const SyntaxError *e, Alloc *a);

/* ------------------------------------------------------------------ flags */

/* syntax.Flags: how to parse, and on a parsed node, how it was parsed. */
typedef uint16_t SyntaxFlags;

enum {
    SYNTAX_FOLD_CASE = 1,            /* case insensitive match */
    SYNTAX_LITERAL = 2,              /* the pattern is a literal string */
    SYNTAX_CLASS_NL = 4,             /* [^a-z] and [[:space:]] can match newline */
    SYNTAX_DOT_NL = 8,               /* . matches newline */
    SYNTAX_ONE_LINE = 16,            /* ^ and $ only match at the ends of the text */
    SYNTAX_NON_GREEDY = 32,          /* repetitions are non-greedy by default */
    SYNTAX_PERL_X = 64,              /* the Perl extensions */
    SYNTAX_UNICODE_GROUPS = 128,     /* \p{Han} and \P{Han} */
    SYNTAX_WAS_DOLLAR = 256,         /* an OpEndText that was $, not \z */
    SYNTAX_SIMPLE = 512,             /* no counted repetition in the pattern */
    SYNTAX_MATCH_NL = 4 | 8,         /* SYNTAX_CLASS_NL | SYNTAX_DOT_NL */
    SYNTAX_PERL = 4 | 16 | 64 | 128, /* as close to Perl as it gets */
    SYNTAX_POSIX = 0,                /* POSIX syntax */
};

/* ------------------------------------------------------------------- tree */

/* syntax.Op: what a node is. */
typedef uint8_t SyntaxOp;

enum {
    SYNTAX_OP_NO_MATCH = 1,          /* matches no strings */
    SYNTAX_OP_EMPTY_MATCH = 2,       /* matches the empty string */
    SYNTAX_OP_LITERAL = 3,           /* matches the runes in rune */
    SYNTAX_OP_CHAR_CLASS = 4,        /* rune is lo, hi pairs of a class */
    SYNTAX_OP_ANY_CHAR_NOT_NL = 5,   /* any character but newline */
    SYNTAX_OP_ANY_CHAR = 6,          /* any character */
    SYNTAX_OP_BEGIN_LINE = 7,        /* empty, at the start of a line */
    SYNTAX_OP_END_LINE = 8,          /* empty, at the end of a line */
    SYNTAX_OP_BEGIN_TEXT = 9,        /* empty, at the start of the text */
    SYNTAX_OP_END_TEXT = 10,         /* empty, at the end of the text */
    SYNTAX_OP_WORD_BOUNDARY = 11,    /* \b */
    SYNTAX_OP_NO_WORD_BOUNDARY = 12, /* \B */
    SYNTAX_OP_CAPTURE = 13,          /* group number cap, maybe named */
    SYNTAX_OP_STAR = 14,             /* sub[0] zero or more times */
    SYNTAX_OP_PLUS = 15,             /* sub[0] one or more times */
    SYNTAX_OP_QUEST = 16,            /* sub[0] zero or one times */
    SYNTAX_OP_REPEAT = 17,           /* sub[0] min to max times, max -1 for no limit */
    SYNTAX_OP_CONCAT = 18,           /* the subs one after another */
    SYNTAX_OP_ALTERNATE = 19,        /* any one of the subs */
};

/* Op.String: "Literal", "Concat" and so on, "Op(42)" for a number with no
 * name. Always static. */
BURROW_STATIC(ret) Str syntax_op_string(SyntaxOp op);

/* syntax.Regexp: one node of a parsed pattern.
 *
 * sub is a Slice of SyntaxRegexp pointers and rune a Slice of Rune. sub0 and
 * rune0 are room for short ones inside the node, the way Go has them, so a
 * slice can point into its own node. */
typedef struct SyntaxRegexp SyntaxRegexp;

struct SyntaxRegexp {
    SyntaxOp op;
    SyntaxFlags flags;
    Slice sub;             /* subexpressions, if any */
    SyntaxRegexp *sub0[1]; /* storage for a short sub */
    Slice rune;            /* matched runes, for literals and classes */
    Rune rune0[2];         /* storage for a short rune */
    Int min;               /* for SYNTAX_OP_REPEAT */
    Int max;               /* for SYNTAX_OP_REPEAT, -1 for no upper bound */
    Int cap;               /* the group number, for SYNTAX_OP_CAPTURE */
    Str name;              /* the group name, for SYNTAX_OP_CAPTURE */
};

/* syntax.Parse. The tree for s, parsed as flags say, in one block from a, or
 * NULL and *err. A pattern that does not parse gives a SyntaxError. One that
 * nests deeper than 1000 or would compile to too big a program gives
 * SYNTAX_ERR_NESTING_DEPTH or SYNTAX_ERR_LARGE with the whole pattern, as
 * Go's does, and running out of memory gives burrow_err_out_of_memory. The
 * parser's own working memory comes from a too and is given back before this
 * returns. err may be NULL. */
BURROW_OWNS(ret) SyntaxRegexp *syntax_parse(Alloc *a, Str s, SyntaxFlags flags,
                                            Error *err);

/* Gives a tree from syntax_parse or syntax_regexp_simplify back to its
 * allocator, all of it. NULL is fine. */
void syntax_regexp_free(SyntaxRegexp *re);

/* Regexp.Equal: whether x and y are the same tree, node for node. Two NULLs
 * are equal. */
bool syntax_regexp_equal(const SyntaxRegexp *x, const SyntaxRegexp *y);

/* Regexp.String: the pattern written back out, in a form that parses to the
 * same tree, built in a. Empty when a refuses. */
BURROW_OWNS(ret) Str syntax_regexp_string(const SyntaxRegexp *re, Alloc *a);

/* Regexp.MaxCap: the highest group number in the tree. */
Int syntax_regexp_max_cap(const SyntaxRegexp *re);

/* Regexp.CapNames: a Slice of Str from a, one per group from 0 to MaxCap, with
 * the name of each group and "" for the unnamed ones. The names borrow from
 * re. A nil Slice when a refuses. */
BURROW_OWNS(ret) Slice syntax_regexp_cap_names(const SyntaxRegexp *re, Alloc *a);

/* Regexp.Simplify: an equivalent tree with each counted repetition written out
 * with the plain operators, x{1,3} as xx?x?? and so on, in a new block from a.
 * The result shares nothing with re. NULL when re is NULL or a refuses. */
BURROW_OWNS(ret) SyntaxRegexp *syntax_regexp_simplify(const SyntaxRegexp *re, Alloc *a);

/* --------------------------------------------------------------- programs */

/* syntax.InstOp: what an instruction does. */
typedef uint8_t SyntaxInstOp;

enum {
    SYNTAX_INST_ALT = 0,
    SYNTAX_INST_ALT_MATCH = 1,
    SYNTAX_INST_CAPTURE = 2,
    SYNTAX_INST_EMPTY_WIDTH = 3,
    SYNTAX_INST_MATCH = 4,
    SYNTAX_INST_FAIL = 5,
    SYNTAX_INST_NOP = 6,
    SYNTAX_INST_RUNE = 7,
    SYNTAX_INST_RUNE1 = 8,
    SYNTAX_INST_RUNE_ANY = 9,
    SYNTAX_INST_RUNE_ANY_NOT_NL = 10,
};

/* InstOp.String: "InstAlt" and so on, "" for a number with no name. */
BURROW_STATIC(ret) Str syntax_inst_op_string(SyntaxInstOp op);

/* syntax.EmptyOp: the zero width assertions, as bits. */
typedef uint8_t SyntaxEmptyOp;

enum {
    SYNTAX_EMPTY_BEGIN_LINE = 1,
    SYNTAX_EMPTY_END_LINE = 2,
    SYNTAX_EMPTY_BEGIN_TEXT = 4,
    SYNTAX_EMPTY_END_TEXT = 8,
    SYNTAX_EMPTY_WORD_BOUNDARY = 16,
    SYNTAX_EMPTY_NO_WORD_BOUNDARY = 32,
};

/* syntax.EmptyOpContext: which assertions hold between r1 and r2, where -1
 * stands for the start or end of the text. */
SyntaxEmptyOp syntax_empty_op_context(Rune r1, Rune r2);

/* syntax.IsWordChar: whether r is an ASCII letter, digit or underscore, which
 * is what \b means by a word character. */
bool syntax_is_word_char(Rune r);

/* syntax.Inst: one instruction. rune is a Slice of Rune. */
typedef struct SyntaxInst {
    SyntaxInstOp op;
    uint32_t out; /* all but SYNTAX_INST_MATCH and SYNTAX_INST_FAIL */
    uint32_t arg; /* alt, alt match, capture and empty width */
    Slice rune;
} SyntaxInst;

/* Inst.MatchRune: whether i, a rune instruction, matches r. */
bool syntax_inst_match_rune(const SyntaxInst *i, Rune r);

/* Inst.MatchRunePos: which of the lo, hi pairs in i's runes has r in it, or
 * -1 for none. A single rune case folds when the instruction says to. */
Int syntax_inst_match_rune_pos(const SyntaxInst *i, Rune r);

/* Inst.MatchEmptyWidth: whether i, an empty width instruction, holds between
 * before and after. Panics when i's arg is not one of the assertions. */
bool syntax_inst_match_empty_width(const SyntaxInst *i, Rune before, Rune after);

/* Inst.String: the instruction as the program listing shows it, in a. */
BURROW_OWNS(ret) Str syntax_inst_string(const SyntaxInst *i, Alloc *a);

/* syntax.Prog: a compiled program. inst is a Slice of SyntaxInst. */
typedef struct SyntaxProg {
    Slice inst;
    Int start;   /* where to start */
    Int num_cap; /* how many capture slots the program uses */
} SyntaxProg;

/* syntax.Compile: the program for re, in one block from a, or NULL and
 * burrow_err_out_of_memory in *err, which is the only way it fails. err may
 * be NULL. */
BURROW_OWNS(ret) SyntaxProg *syntax_compile(Alloc *a, const SyntaxRegexp *re,
                                            Error *err);

/* Gives a program from syntax_compile back to its allocator. NULL is fine. */
void syntax_prog_free(SyntaxProg *p);

/* Prog.String: the listing, one instruction a line, in a. */
BURROW_OWNS(ret) Str syntax_prog_string(const SyntaxProg *p, Alloc *a);

/* Prog.Prefix: the literal text every match has to start with, built in a,
 * and in *complete whether it is the whole match. complete may be NULL. */
BURROW_OWNS(ret) Str syntax_prog_prefix(const SyntaxProg *p, Alloc *a, bool *complete);

/* Prog.StartCond: the assertions that have to hold at the start of any match,
 * or 0xFF when nothing can match. */
SyntaxEmptyOp syntax_prog_start_cond(const SyntaxProg *p);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_REGEXP_SYNTAX_H */
