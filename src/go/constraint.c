/* go/build/constraint: parsing and evaluating build constraint lines.
 *
 * Derived from Go's src/go/build/constraint/expr.go and vers.go.
 * Go source: go1.27.1.
 *
 * Go's parser panics with a *SyntaxError and recovers it at the top. Here the
 * parser keeps the first error in its state instead, and every step returns
 * NULL as soon as there is one, which unwinds the same way.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/build/constraint.h"

#include "burrow/fmt.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <string.h>

/* maxSize in Go, the most ! operands a //go:build expression may have, which
 * keeps the recursion bounded. */
enum { BC_MAX_SIZE = 1000 };

/* maxOldSize, the most operators a // +build line may have. */
enum { BC_MAX_OLD_SIZE = 100 };

static Str bc_sub(Str s, Int i, Int j) {
    if (i == j)
        return BURROW_STR_EMPTY; /* s.p can be NULL, and NULL + 0 is undefined */
    return str_from_bytes(s.p + i, j - i);
}

/* ------------------------------------------------------------------ errors */

static const Str bc_not_constraint_text = BURROW_S_INIT("not a build constraint");
static const Error bc_err_not_constraint = {&burrow_sentinel_error_vt,
                                            &bc_not_constraint_text};

static const Str bc_complex_text =
    BURROW_S_INIT("expression too complex for // +build lines");
static const Error bc_err_complex = {&burrow_sentinel_error_vt, &bc_complex_text};

/* errComplex, so that the tests can check for that error and no other. */
Error burrow__constraint_err_complex(void);
Error burrow__constraint_err_complex(void) {
    return bc_err_complex;
}

static const Type bc_syntax_error_desc = {
    {(const Byte *)"SyntaxError", 11},
    {(const Byte *)"go/build/constraint", 19},
    KIND_STRUCT,
    (uint32_t)sizeof(ConstraintSyntaxError),
    (uint16_t)_Alignof(ConstraintSyntaxError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x62637365U, /* "bcse" */
    NULL,
};

const Type *const TYPE_CONSTRAINT_SYNTAX_ERROR = &bc_syntax_error_desc;

Str constraint_syntax_error_error(const ConstraintSyntaxError *e) {
    return e->err;
}

static Error bc_syntax_error(Alloc *a, Int offset, Str msg);

static Str bc_syntax_message(const void *self) {
    return ((const ConstraintSyntaxError *)self)->err;
}

static Error bc_syntax_clone(const void *self, Alloc *a) {
    const ConstraintSyntaxError *e = (const ConstraintSyntaxError *)self;
    return bc_syntax_error(a, e->offset, e->err);
}

static const ErrorVT bc_syntax_vt = {
    .self_type = &bc_syntax_error_desc,
    .message = bc_syntax_message,
    .clone = bc_syntax_clone,
};

/* A SyntaxError in a, with its own copy of msg after the struct. */
static Error bc_syntax_error(Alloc *a, Int offset, Str msg) {
    size_t size = sizeof(ConstraintSyntaxError) + (size_t)msg.len;
    ConstraintSyntaxError *e = (ConstraintSyntaxError *)mem_alloc_nozero(
        a, size, _Alignof(ConstraintSyntaxError));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    if (msg.len > 0)
        memcpy(p, msg.p, (size_t)msg.len);
    e->offset = offset;
    e->err = str_from_bytes(p, msg.len);
    return (Error){&bc_syntax_vt, e};
}

/* ------------------------------------------------------------- expressions */

static ConstraintExpr bc_tag(Alloc *a, Str tag) {
    ConstraintTagExpr *x = (ConstraintTagExpr *)mem_alloc(a, sizeof(ConstraintTagExpr),
                                                          _Alignof(ConstraintTagExpr));
    if (x == NULL)
        return NULL;
    x->expr.kind = CONSTRAINT_KIND_TAG;
    x->tag = tag;
    return &x->expr;
}

static ConstraintExpr bc_not(Alloc *a, ConstraintExpr y) {
    ConstraintNotExpr *x = (ConstraintNotExpr *)mem_alloc(a, sizeof(ConstraintNotExpr),
                                                          _Alignof(ConstraintNotExpr));
    if (x == NULL)
        return NULL;
    x->expr.kind = CONSTRAINT_KIND_NOT;
    x->x = y;
    return &x->expr;
}

/* An AndExpr or an OrExpr, which have the same layout. */
static ConstraintExpr bc_binary(Alloc *a, ConstraintKind kind, ConstraintExpr x,
                                ConstraintExpr y) {
    ConstraintAndExpr *e = (ConstraintAndExpr *)mem_alloc(a, sizeof(ConstraintAndExpr),
                                                          _Alignof(ConstraintAndExpr));
    if (e == NULL)
        return NULL;
    e->expr.kind = kind;
    e->x = x;
    e->y = y;
    return &e->expr;
}

bool constraint_tag_expr_eval(ConstraintTagExpr *x, ConstraintTagFunc ok) {
    return BURROW_CALLF(ok, x->tag);
}

bool constraint_not_expr_eval(ConstraintNotExpr *x, ConstraintTagFunc ok) {
    return !constraint_expr_eval(x->x, ok);
}

bool constraint_and_expr_eval(ConstraintAndExpr *x, ConstraintTagFunc ok) {
    /* Both, so that ok sees every tag. */
    bool xok = constraint_expr_eval(x->x, ok);
    bool yok = constraint_expr_eval(x->y, ok);
    return xok && yok;
}

bool constraint_or_expr_eval(ConstraintOrExpr *x, ConstraintTagFunc ok) {
    bool xok = constraint_expr_eval(x->x, ok);
    bool yok = constraint_expr_eval(x->y, ok);
    return xok || yok;
}

bool constraint_expr_eval(ConstraintExpr x, ConstraintTagFunc ok) {
    switch ((int)x->kind) {
    case CONSTRAINT_KIND_TAG:
        return constraint_tag_expr_eval((ConstraintTagExpr *)x, ok);
    case CONSTRAINT_KIND_NOT:
        return constraint_not_expr_eval((ConstraintNotExpr *)x, ok);
    case CONSTRAINT_KIND_AND:
        return constraint_and_expr_eval((ConstraintAndExpr *)x, ok);
    case CONSTRAINT_KIND_OR:
        return constraint_or_expr_eval((ConstraintOrExpr *)x, ok);
    default:
        return false;
    }
}

static void bc_write(StringsBuilder *b, ConstraintExpr x, Error *err);

/* x, in parentheses when it is of kind paren. This is andArg and orArg, and
 * the parentheses NotExpr.String puts around either kind. */
static void bc_write_arg(StringsBuilder *b, ConstraintExpr x, int paren, Error *err) {
    bool p = x != NULL && (int)x->kind == paren;
    if (p)
        strings_builder_write_string(b, BURROW_S("("), err);
    bc_write(b, x, err);
    if (p)
        strings_builder_write_string(b, BURROW_S(")"), err);
}

static void bc_write(StringsBuilder *b, ConstraintExpr x, Error *err) {
    if (x == NULL)
        return;
    switch ((int)x->kind) {
    case CONSTRAINT_KIND_TAG:
        strings_builder_write_string(b, ((ConstraintTagExpr *)x)->tag, err);
        break;
    case CONSTRAINT_KIND_NOT: {
        ConstraintExpr y = ((ConstraintNotExpr *)x)->x;
        strings_builder_write_string(b, BURROW_S("!"), err);
        if (y != NULL && (int)y->kind == CONSTRAINT_KIND_AND)
            bc_write_arg(b, y, CONSTRAINT_KIND_AND, err);
        else
            bc_write_arg(b, y, CONSTRAINT_KIND_OR, err);
        break;
    }
    case CONSTRAINT_KIND_AND:
        bc_write_arg(b, ((ConstraintAndExpr *)x)->x, CONSTRAINT_KIND_OR, err);
        strings_builder_write_string(b, BURROW_S(" && "), err);
        bc_write_arg(b, ((ConstraintAndExpr *)x)->y, CONSTRAINT_KIND_OR, err);
        break;
    case CONSTRAINT_KIND_OR:
        bc_write_arg(b, ((ConstraintOrExpr *)x)->x, CONSTRAINT_KIND_AND, err);
        strings_builder_write_string(b, BURROW_S(" || "), err);
        bc_write_arg(b, ((ConstraintOrExpr *)x)->y, CONSTRAINT_KIND_AND, err);
        break;
    default:
        break;
    }
}

static Str bc_string(ConstraintExpr x, Alloc *a) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    bc_write(&b, x, &err);
    if (!BURROW_OK(err))
        return BURROW_STR_EMPTY;
    return strings_builder_string(&b);
}

Str constraint_expr_string(ConstraintExpr x, Alloc *a) {
    return bc_string(x, a);
}

Str constraint_tag_expr_string(ConstraintTagExpr *x) {
    return x->tag;
}

Str constraint_not_expr_string(ConstraintNotExpr *x, Alloc *a) {
    return bc_string(&x->expr, a);
}

Str constraint_and_expr_string(ConstraintAndExpr *x, Alloc *a) {
    return bc_string(&x->expr, a);
}

Str constraint_or_expr_string(ConstraintOrExpr *x, Alloc *a) {
    return bc_string(&x->expr, a);
}

/* ------------------------------------------------------------------ lexing */

/* exprParser. failed is set by the first error, after which nothing more is
 * read, and oom says that error was running out of memory rather than a
 * SyntaxError at err_offset. */
typedef struct BcParser {
    Alloc *a;
    Str s;
    Int i;
    Str tok;
    bool is_tag;
    Int pos;
    Int size;
    bool failed;
    bool oom;
    Int err_offset;
    Str err_msg;
} BcParser;

static void bc_fail(BcParser *p, Int offset, Str msg) {
    if (p->failed)
        return;
    p->failed = true;
    p->err_offset = offset;
    p->err_msg = msg;
}

static void bc_fail_oom(BcParser *p) {
    if (p->failed)
        return;
    p->failed = true;
    p->oom = true;
}

static bool bc_is_tag_rune(Rune c) {
    return unicode_is_letter(c) || unicode_is_digit(c) || c == '_' || c == '.';
}

static void bc_fail_at(BcParser *p, Int offset, Rune c) {
    Str msg = fmt_sprintf_v(p->a, "invalid syntax at %c", c);
    if (msg.len == 0)
        bc_fail_oom(p);
    else
        bc_fail(p, offset, msg);
}

/* lex: the next token in p->tok, empty at the end of the input. */
static void bc_lex(BcParser *p) {
    p->is_tag = false;
    while (p->i < p->s.len && (p->s.p[p->i] == ' ' || p->s.p[p->i] == '\t'))
        p->i++;
    if (p->i >= p->s.len) {
        p->tok = BURROW_STR_EMPTY;
        p->pos = p->i;
        return;
    }
    Byte c = p->s.p[p->i];
    switch (c) {
    case '(':
    case ')':
    case '!':
        p->pos = p->i;
        p->i++;
        p->tok = bc_sub(p->s, p->pos, p->i);
        return;
    case '&':
    case '|':
        if (p->i + 1 >= p->s.len || p->s.p[p->i + 1] != c) {
            bc_fail_at(p, p->i, (Rune)c);
            return;
        }
        p->pos = p->i;
        p->i += 2;
        p->tok = bc_sub(p->s, p->pos, p->i);
        return;
    default:
        break;
    }

    Str rest = bc_sub(p->s, p->i, p->s.len);
    Int n = 0;
    while (n < rest.len) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(bc_sub(rest, n, rest.len), &size);
        if (!bc_is_tag_rune(r))
            break;
        n += size;
    }
    if (n == 0) {
        Int size = 0;
        bc_fail_at(p, p->i, utf8_decode_rune_in_string(rest, &size));
        return;
    }
    p->pos = p->i;
    p->i += n;
    p->tok = bc_sub(p->s, p->pos, p->i);
    p->is_tag = true;
}

/* The tokens of s one at a time, for the lexer tests: the token at *i, with
 * *i moved past it, or the SyntaxError there in *err. */
Str burrow__constraint_lex(Alloc *a, Str s, Int *i, Error *err);
Str burrow__constraint_lex(Alloc *a, Str s, Int *i, Error *err) {
    BcParser p = {0};
    p.a = a;
    p.s = s;
    p.i = *i;
    bc_lex(&p);
    *i = p.i;
    *err = BURROW_NO_ERROR;
    if (p.oom)
        *err = burrow_err_out_of_memory;
    else if (p.failed)
        *err = bc_syntax_error(error_allocator(), p.err_offset, p.err_msg);
    return p.tok;
}

/* ----------------------------------------------------------------- parsing */

static ConstraintExpr bc_parse_or(BcParser *p);

static bool bc_tok_is(BcParser *p, const char *lit) {
    return str_eq(p->tok, str_from_cstr(lit));
}

static ConstraintExpr bc_check(BcParser *p, ConstraintExpr x) {
    if (x == NULL)
        bc_fail_oom(p);
    return x;
}

static void bc_fail_token(BcParser *p) {
    Str msg = fmt_sprintf_v(p->a, "unexpected token %s", p->tok);
    if (msg.len == 0)
        bc_fail_oom(p);
    else
        bc_fail(p, p->pos, msg);
}

/* atom: a tag or a parenthesized expression, with its first token already in
 * p->tok. */
static ConstraintExpr bc_parse_atom(BcParser *p) {
    if (bc_tok_is(p, "(")) {
        Int pos = p->pos;
        ConstraintExpr x = bc_parse_or(p);
        if (p->failed) {
            if (!p->oom && str_eq(p->err_msg, BURROW_S("unexpected end of expression")))
                p->err_msg = BURROW_S("missing close paren");
            return NULL;
        }
        if (!bc_tok_is(p, ")")) {
            bc_fail(p, pos, BURROW_S("missing close paren"));
            return NULL;
        }
        bc_lex(p);
        return p->failed ? NULL : x;
    }

    if (!p->is_tag) {
        if (p->tok.len == 0)
            bc_fail(p, p->pos, BURROW_S("unexpected end of expression"));
        else
            bc_fail_token(p);
        return NULL;
    }
    Str tok = str_clone(p->a, p->tok);
    if (tok.len == 0) {
        bc_fail_oom(p);
        return NULL;
    }
    bc_lex(p);
    if (p->failed)
        return NULL;
    return bc_check(p, bc_tag(p->a, tok));
}

/* not: an atom, or ! and an atom. */
static ConstraintExpr bc_parse_not(BcParser *p) {
    p->size++;
    if (p->size > BC_MAX_SIZE) {
        bc_fail(p, p->pos, BURROW_S("build expression too large"));
        return NULL;
    }
    bc_lex(p);
    if (p->failed)
        return NULL;
    if (bc_tok_is(p, "!")) {
        bc_lex(p);
        if (p->failed)
            return NULL;
        if (bc_tok_is(p, "!")) {
            bc_fail(p, p->pos, BURROW_S("double negation not allowed"));
            return NULL;
        }
        ConstraintExpr x = bc_parse_atom(p);
        if (x == NULL)
            return NULL;
        return bc_check(p, bc_not(p->a, x));
    }
    return bc_parse_atom(p);
}

static ConstraintExpr bc_parse_and(BcParser *p) {
    ConstraintExpr x = bc_parse_not(p);
    while (x != NULL && bc_tok_is(p, "&&")) {
        ConstraintExpr y = bc_parse_not(p);
        if (y == NULL)
            return NULL;
        x = bc_check(p, bc_binary(p->a, CONSTRAINT_KIND_AND, x, y));
    }
    return x;
}

static ConstraintExpr bc_parse_or(BcParser *p) {
    ConstraintExpr x = bc_parse_and(p);
    while (x != NULL && bc_tok_is(p, "||")) {
        ConstraintExpr y = bc_parse_and(p);
        if (y == NULL)
            return NULL;
        x = bc_check(p, bc_binary(p->a, CONSTRAINT_KIND_OR, x, y));
    }
    return x;
}

/* parseExpr: a //go:build expression without the prefix. */
static ConstraintExpr bc_parse_expr(Alloc *a, Str text, Error *err) {
    BcParser p = {0};
    p.a = a;
    p.s = text;
    ConstraintExpr x = bc_parse_or(&p);
    if (!p.failed && p.tok.len > 0)
        bc_fail_token(&p);
    if (p.oom) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    if (p.failed) {
        *err = bc_syntax_error(error_allocator(), p.err_offset, p.err_msg);
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return x;
}

ConstraintExpr burrow__constraint_parse_expr(Alloc *a, Str text, Error *err);
ConstraintExpr burrow__constraint_parse_expr(Alloc *a, Str text, Error *err) {
    return bc_parse_expr(a, text, err);
}

/* What follows prefix on a line of its own, trimmed, when prefix is followed by
 * a space or by nothing. A single trailing newline is allowed. */
static bool bc_split_after(Str line, Str prefix, Str *expr) {
    if (line.len > 0 && line.p[line.len - 1] == '\n')
        line.len--;
    if (strings_index_byte(line, '\n') >= 0)
        return false;
    if (!strings_has_prefix(line, prefix))
        return false;
    line = strings_trim_space(line);
    line = bc_sub(line, prefix.len, line.len);

    /* If there is more to trim, the prefix was followed by a space, which
     * makes this a constraint and not, say, //go:buildsomethingelse. A line
     * that is only the prefix counts too. */
    Str trim = strings_trim_space(line);
    if (line.len == trim.len && line.len != 0)
        return false;
    *expr = trim;
    return true;
}

/* splitGoBuild. */
static bool bc_split_go_build(Str line, Str *expr) {
    return bc_split_after(line, BURROW_S("//go:build"), expr);
}

/* splitPlusBuild. The space after // is optional, so "//+build" counts. */
static bool bc_split_plus_build(Str line, Str *expr) {
    if (line.len > 0 && line.p[line.len - 1] == '\n')
        line.len--;
    if (strings_index_byte(line, '\n') >= 0)
        return false;
    if (!strings_has_prefix(line, BURROW_S("//")))
        return false;
    line = strings_trim_space(bc_sub(line, 2, line.len));
    return bc_split_after(line, BURROW_S("+build"), expr);
}

bool constraint_is_go_build(Str line) {
    Str expr;
    return bc_split_go_build(line, &expr);
}

bool constraint_is_plus_build(Str line) {
    Str expr;
    return bc_split_plus_build(line, &expr);
}

/* isValidTag: letters, digits, underscores and dots, and not empty. */
static bool bc_is_valid_tag(Str word) {
    if (word.len == 0)
        return false;
    for (Int i = 0; i < word.len;) {
        Int size = 0;
        Rune c = utf8_decode_rune_in_string(bc_sub(word, i, word.len), &size);
        if (!bc_is_tag_rune(c))
            return false;
        i += size;
    }
    return true;
}

/* One comma separated literal of a // +build clause. */
static ConstraintExpr bc_plus_build_lit(Alloc *a, Str lit) {
    if (strings_has_prefix(lit, BURROW_S("!!")) || str_eq(lit, BURROW_S("!")))
        return bc_tag(a, BURROW_S("ignore"));
    bool neg = false;
    if (strings_has_prefix(lit, BURROW_S("!"))) {
        neg = true;
        lit = bc_sub(lit, 1, lit.len);
    }
    ConstraintExpr z = NULL;
    if (bc_is_valid_tag(lit)) {
        Str tag = str_clone(a, lit);
        if (tag.len == 0)
            return NULL;
        z = bc_tag(a, tag);
    } else {
        z = bc_tag(a, BURROW_S("ignore"));
    }
    if (z != NULL && neg)
        z = bc_not(a, z);
    return z;
}

/* parsePlusBuildExpr: space separated clauses ORed together, each one comma
 * separated literals ANDed together. */
static ConstraintExpr bc_parse_plus_build_expr(Alloc *a, Str text, Error *err) {
    Int size = 0;
    ConstraintExpr x = NULL;
    *err = BURROW_NO_ERROR;
    for (Int i = 0; i < text.len;) {
        Int n = 0;
        Rune r = utf8_decode_rune_in_string(bc_sub(text, i, text.len), &n);
        if (unicode_is_space(r)) {
            i += n;
            continue;
        }
        Int start = i;
        while (i < text.len) {
            r = utf8_decode_rune_in_string(bc_sub(text, i, text.len), &n);
            if (unicode_is_space(r))
                break;
            i += n;
        }
        Str clause = bc_sub(text, start, i);

        ConstraintExpr y = NULL;
        for (Int j = 0;;) {
            Str rest = bc_sub(clause, j, clause.len);
            Int k = strings_index_byte(rest, ',');
            Str lit = k < 0 ? rest : bc_sub(rest, 0, k);
            ConstraintExpr z = bc_plus_build_lit(a, lit);
            if (z == NULL)
                goto oom;
            if (y == NULL) {
                y = z;
            } else {
                if (++size > BC_MAX_OLD_SIZE) {
                    *err = bc_err_complex;
                    return NULL;
                }
                y = bc_binary(a, CONSTRAINT_KIND_AND, y, z);
                if (y == NULL)
                    goto oom;
            }
            if (k < 0)
                break;
            j += k + 1;
        }
        if (x == NULL) {
            x = y;
        } else {
            if (++size > BC_MAX_OLD_SIZE) {
                *err = bc_err_complex;
                return NULL;
            }
            x = bc_binary(a, CONSTRAINT_KIND_OR, x, y);
            if (x == NULL)
                goto oom;
        }
    }
    if (x == NULL)
        x = bc_tag(a, BURROW_S("ignore"));
    if (x == NULL)
        goto oom;
    return x;

oom:
    *err = burrow_err_out_of_memory;
    return NULL;
}

ConstraintExpr burrow__constraint_parse_plus_build_expr(Alloc *a, Str text, Error *err);
ConstraintExpr burrow__constraint_parse_plus_build_expr(Alloc *a, Str text,
                                                        Error *err) {
    return bc_parse_plus_build_expr(a, text, err);
}

ConstraintExpr constraint_parse(Alloc *a, Str line, Error *err) {
    Str text;
    if (bc_split_go_build(line, &text))
        return bc_parse_expr(a, text, err);
    if (bc_split_plus_build(line, &text))
        return bc_parse_plus_build_expr(a, text, err);
    *err = bc_err_not_constraint;
    return NULL;
}

/* -------------------------------------------------------------- +build lines */

/* pushNot: x with De Morgan's laws applied until only tags are negated, and
 * negated as a whole when not is set. Unchanged parts are shared with x. NULL
 * when a runs out. */
static ConstraintExpr bc_push_not(Alloc *a, ConstraintExpr x, bool not) {
    switch ((int)x->kind) {
    case CONSTRAINT_KIND_NOT: {
        ConstraintExpr y = ((ConstraintNotExpr *)x)->x;
        if ((int)y->kind == CONSTRAINT_KIND_TAG && !not)
            return x;
        return bc_push_not(a, y, !not);
    }
    case CONSTRAINT_KIND_TAG:
        return not? bc_not(a, x) : x;
    case CONSTRAINT_KIND_AND:
    case CONSTRAINT_KIND_OR: {
        ConstraintAndExpr *e = (ConstraintAndExpr *)x;
        ConstraintExpr x1 = bc_push_not(a, e->x, not);
        ConstraintExpr y1 = bc_push_not(a, e->y, not);
        if (x1 == NULL || y1 == NULL)
            return NULL;
        ConstraintKind kind = (ConstraintKind)x->kind;
        if (not)
            kind =
                kind == CONSTRAINT_KIND_AND ? CONSTRAINT_KIND_OR : CONSTRAINT_KIND_AND;
        else if (x1 == e->x && y1 == e->y)
            return x;
        return bc_binary(a, kind, x1, y1);
    }
    default:
        return x;
    }
}

/* appendSplitAnd and appendSplitOr: the operands of the top level run of
 * kind in x, appended to list. list has room for every leaf of x. */
static void bc_split(ConstraintExpr *list, Int *n, ConstraintExpr x, int kind) {
    if ((int)x->kind == kind) {
        bc_split(list, n, ((ConstraintAndExpr *)x)->x, kind);
        bc_split(list, n, ((ConstraintAndExpr *)x)->y, kind);
        return;
    }
    list[(*n)++] = x;
}

static Int bc_leaves(ConstraintExpr x) {
    switch ((int)x->kind) {
    case CONSTRAINT_KIND_AND:
    case CONSTRAINT_KIND_OR:
        return bc_leaves(((ConstraintAndExpr *)x)->x) +
               bc_leaves(((ConstraintAndExpr *)x)->y);
    default:
        return 1;
    }
}

/* A literal of a // +build line, a tag or a negated one. */
static bool bc_is_lit(ConstraintExpr x) {
    return (int)x->kind == CONSTRAINT_KIND_TAG || (int)x->kind == CONSTRAINT_KIND_NOT;
}

/* The literals of and, with commas between them. */
static void bc_write_and(StringsBuilder *b, ConstraintExpr and, Error *err) {
    if ((int)and->kind == CONSTRAINT_KIND_AND) {
        bc_write_and(b, ((ConstraintAndExpr *) and)->x, err);
        strings_builder_write_string(b, BURROW_S(","), err);
        bc_write_and(b, ((ConstraintAndExpr *) and)->y, err);
        return;
    }
    bc_write(b, and, err);
}

Slice constraint_plus_build_lines(Alloc *a, ConstraintExpr x, Error *err) {
    Slice lines = slice_nil(TYPE_STRING);
    *err = BURROW_NO_ERROR;
    if (x == NULL) {
        *err = bc_err_complex;
        return lines;
    }

    /* Push every ! down to the tags, so that !(x && y) can be !x || !y. */
    x = bc_push_not(a, x, false);
    if (x == NULL)
        goto oom;

    /* Split into an AND of ORs of ANDs of literals. Only the ORs and the ANDs
     * under them are kept, since bc_write_and walks the literals of an AND as
     * they are written. */
    Int nleaves = bc_leaves(x);
    ConstraintExpr *ors = (ConstraintExpr *)mem_alloc(
        a, 3 * (size_t)nleaves * sizeof(ConstraintExpr), _Alignof(ConstraintExpr));
    if (ors == NULL)
        goto oom;
    ConstraintExpr *ands = ors + nleaves;
    ConstraintExpr *lits = ands + nleaves;
    Int nors = 0;
    bc_split(ors, &nors, x, CONSTRAINT_KIND_AND);
    Int max_or = 0;
    for (Int i = 0; i < nors; i++) {
        Int nands = 0;
        bc_split(ands, &nands, ors[i], CONSTRAINT_KIND_OR);
        for (Int j = 0; j < nands; j++) {
            Int nlits = 0;
            bc_split(lits, &nlits, ands[j], CONSTRAINT_KIND_AND);
            for (Int k = 0; k < nlits; k++) {
                if (!bc_is_lit(lits[k])) {
                    *err = bc_err_complex;
                    return lines;
                }
            }
        }
        if (max_or < nands)
            max_or = nands;
    }

    /* When no OR has more than one operand there is nothing to OR, and the
     * whole thing is one line: every literal, ANDed. */
    if (max_or == 1) {
        StringsBuilder b = STRINGS_BUILDER(a);
        strings_builder_write_string(&b, BURROW_S("// +build "), err);
        bc_write_and(&b, x, err);
        if (!BURROW_OK(*err))
            goto oom;
        Str line = strings_builder_string(&b);
        lines = slice_append(a, lines, &line, 1);
        if (lines.len != 1)
            goto oom;
        return lines;
    }

    for (Int i = 0; i < nors; i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        strings_builder_write_string(&b, BURROW_S("// +build"), err);
        Int nands = 0;
        bc_split(ands, &nands, ors[i], CONSTRAINT_KIND_OR);
        for (Int j = 0; j < nands; j++) {
            strings_builder_write_string(&b, BURROW_S(" "), err);
            bc_write_and(&b, ands[j], err);
        }
        if (!BURROW_OK(*err))
            goto oom;
        Str line = strings_builder_string(&b);
        Int want = lines.len + 1;
        lines = slice_append(a, lines, &line, 1);
        if (lines.len != want)
            goto oom;
    }
    return lines;

oom:
    *err = burrow_err_out_of_memory;
    return slice_nil(TYPE_STRING);
}

/* --------------------------------------------------------------- versions */

/* andVersion: the AND of two minimum versions needs the larger. */
static Int bc_and_version(Int x, Int y) {
    return x > y ? x : y;
}

/* orVersion: the OR of two needs only the smaller. */
static Int bc_or_version(Int x, Int y) {
    return x < y ? x : y;
}

/* minVersion: the minor version of the oldest Go z allows, 9 for go1.9, or of
 * !z when sign is negative, and -1 for none. */
static Int bc_min_version(ConstraintExpr z, int sign) {
    switch ((int)z->kind) {
    case CONSTRAINT_KIND_AND: {
        Int x = bc_min_version(((ConstraintAndExpr *)z)->x, sign);
        Int y = bc_min_version(((ConstraintAndExpr *)z)->y, sign);
        return sign < 0 ? bc_or_version(x, y) : bc_and_version(x, y);
    }
    case CONSTRAINT_KIND_OR: {
        Int x = bc_min_version(((ConstraintOrExpr *)z)->x, sign);
        Int y = bc_min_version(((ConstraintOrExpr *)z)->y, sign);
        return sign < 0 ? bc_and_version(x, y) : bc_or_version(x, y);
    }
    case CONSTRAINT_KIND_NOT:
        return bc_min_version(((ConstraintNotExpr *)z)->x, -sign);
    case CONSTRAINT_KIND_TAG: {
        if (sign < 0)
            return -1; /* !foo says nothing */
        Str tag = ((ConstraintTagExpr *)z)->tag;
        if (str_eq(tag, BURROW_S("go1")))
            return 0;
        Str v = BURROW_STR_EMPTY;
        bool ok = false;
        strings_cut(tag, BURROW_S("go1."), &v, &ok);
        if (!ok)
            return -1;
        Error err = BURROW_NO_ERROR;
        Int n = strconv_atoi(v, &err);
        if (!BURROW_OK(err))
            return -1; /* not a go1.N tag */
        return n;
    }
    default:
        return -1;
    }
}

Str constraint_go_version(Alloc *a, ConstraintExpr x) {
    Int v = bc_min_version(x, +1);
    if (v < 0)
        return BURROW_STR_EMPTY;
    if (v == 0)
        return str_clone(a, BURROW_S("go1"));
    return fmt_sprintf_v(a, "go1.%d", v);
}
