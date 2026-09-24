/* Derived from Go's src/go/parser/parser.go.
 * Go source: go1.27.1.
 *
 * go/parser, for corpus.c, from ParseExprFrom down: expressions, types, and the
 * statements and declarations a function literal can hold, with the error
 * recovery that decides which errors a malformed line gets. See
 * corpus_syntax.h for why this exists and how long it is meant to.
 *
 * The functions keep Go's names and order. What differs is what happens around
 * them. Go builds a full tree and the nodes here keep only what is read again,
 * either by corpus.c or by the parser itself while it recovers. Go panics to
 * bail out, and here a flag makes every frame return on its own, as described
 * at CorpusFile. And Go's stacks grow, so it can afford a hundred thousand
 * levels of nesting, while a C stack is fixed: the parse runs on a goroutine
 * with a large one, and the nesting check also stops a parse that is about to
 * run off the end of it, with Go's message for going too deep.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "corpus_syntax.h"

#include "burrow/chan.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

typedef struct CorpusParser {
    CorpusFile *file;
    Alloc *a;
    CorpusScanner scanner;

    /* Next token. */
    Int pos;
    CorpusTok tok;
    Str lit;

    /* Error recovery. */
    Int sync_pos;
    int sync_cnt;

    /* Non-syntactic parser control. */
    int expr_lev;
    bool in_rhs;

    int nest_lev;
    /* Where the stack was when the parse started, and how far below that it
     * may go. */
    uintptr_t stack_base;
    size_t stack_budget;
} CorpusParser;

typedef struct CorpusList {
    CorpusNode **v;
    Int len;
    Int cap;
} CorpusList;

#define CORPUS_MAX_NEST_LEV 100000
#define CORPUS_LOWEST_PREC 0

typedef enum CorpusDir { CORPUS_SEND = 1, CORPUS_RECV = 2 } CorpusDir;

static const char *corpus_tok_str(CorpusTok t) {
    return burrow__testing_corpus_tok_string(t);
}

static void corpus_list_add(CorpusParser *p, CorpusList *l, CorpusNode *n) {
    if (l->len == l->cap) {
        Int ncap = l->cap == 0 ? 4 : l->cap * 2;
        CorpusNode **v =
            (CorpusNode **)burrow__testing_corpus_alloc(p->a, (size_t)ncap * sizeof *v);
        if (l->len > 0)
            memcpy((void *)v, (const void *)l->v, (size_t)l->len * sizeof *v);
        l->v = v;
        l->cap = ncap;
    }
    l->v[l->len++] = n;
}

static CorpusNode *corpus_new(CorpusParser *p, CorpusNodeKind kind, Int pos, Int end) {
    CorpusNode *n = burrow__testing_corpus_alloc(p->a, sizeof *n);
    n->kind = kind;
    n->pos = pos;
    n->end = end;
    return n;
}

static CorpusNode *corpus_bad(CorpusParser *p, Int from, Int to) {
    return corpus_new(p, CN_BAD, from, to);
}

static CorpusNode *corpus_ident(CorpusParser *p, Int pos, Str name) {
    CorpusNode *n = corpus_new(p, CN_IDENT, pos, pos + name.len);
    n->name = name;
    return n;
}

static CorpusNode *corpus_unary(CorpusParser *p, Int pos, CorpusTok op, CorpusNode *x) {
    CorpusNode *n = corpus_new(p, CN_UNARY, pos, x->end);
    n->tok = op;
    n->x = x;
    return n;
}

static CorpusNode *corpus_binary(CorpusParser *p, CorpusNode *x, Int op_pos,
                                 CorpusTok op, CorpusNode *y) {
    CorpusNode *n = corpus_new(p, CN_BINARY, x->pos, y->end);
    n->x = x;
    n->op_pos = op_pos;
    n->tok = op;
    n->y = y;
    return n;
}

static CorpusNode *corpus_star(CorpusParser *p, Int star, CorpusNode *x) {
    CorpusNode *n = corpus_new(p, CN_STAR, star, x->end);
    n->x = x;
    return n;
}

static CorpusNode *corpus_paren(CorpusParser *p, Int lparen, CorpusNode *x,
                                Int rparen) {
    CorpusNode *n = corpus_new(p, CN_PAREN, lparen, rparen + 1);
    n->x = x;
    return n;
}

static CorpusNode *corpus_selector(CorpusParser *p, CorpusNode *x, CorpusNode *sel) {
    CorpusNode *n = corpus_new(p, CN_SELECTOR, x->pos, sel->end);
    n->x = x;
    n->y = sel;
    return n;
}

static CorpusNode *corpus_index(CorpusParser *p, CorpusNode *x, CorpusNode *index,
                                Int rbrack) {
    CorpusNode *n = corpus_new(p, CN_INDEX, x->pos, rbrack + 1);
    n->x = x;
    n->y = index;
    return n;
}

static CorpusNode *corpus_array_type(CorpusParser *p, Int lbrack, CorpusNode *len,
                                     CorpusNode *elt) {
    CorpusNode *n = corpus_new(p, CN_ARRAY_TYPE, lbrack, elt->end);
    n->y = len;
    n->x = elt;
    return n;
}

static CorpusNode *corpus_unparen(CorpusNode *x) {
    while (x->kind == CN_PAREN)
        x = x->x;
    return x;
}

/* ------------------------------------------------------------ parsing support */

static void corpus_bailout(CorpusParser *p) {
    p->file->bailed = true;
    p->tok = CT_EOF;
    p->lit = (Str){0};
}

static void corpus_error(CorpusParser *p, Int pos, Str msg);

/* Where the stack is now. A local's address will not do under
 * AddressSanitizer, which can put locals on a fake stack in the heap to catch
 * use after return, so the frame address is used wherever there is one. */
static uintptr_t corpus_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

static bool corpus_stack_low(const CorpusParser *p) {
    uintptr_t here = corpus_stack_here();
    size_t used = here < p->stack_base ? (size_t)(p->stack_base - here)
                                       : (size_t)(here - p->stack_base);
    return used > p->stack_budget;
}

static void corpus_inc_nest_lev(CorpusParser *p) {
    p->nest_lev++;
    if (p->nest_lev > CORPUS_MAX_NEST_LEV || corpus_stack_low(p)) {
        corpus_error(p, p->pos, BURROW_S("exceeded max nesting depth"));
        corpus_bailout(p);
    }
}

static void corpus_dec_nest_lev(CorpusParser *p) {
    p->nest_lev--;
}

/* next, which is next0 with the comment bookkeeping taken out, since the
 * comments are gone before they get here. */
static void corpus_next(CorpusParser *p) {
    p->tok = burrow__testing_corpus_scan(&p->scanner, &p->pos, &p->lit);
}

static void corpus_error(CorpusParser *p, Int pos, Str msg) {
    if (p->file->bailed)
        return;
    CorpusPosition epos = burrow__testing_corpus_position(p->file, pos);
    /* Errors on the same line as the last one are dropped, and more than ten
     * ends the parse, since AllErrors is not set. */
    Int n = p->file->nerrs;
    if (n > 0 && p->file->errs[n - 1].pos.line == epos.line)
        return;
    if (n > 10) {
        corpus_bailout(p);
        return;
    }
    burrow__testing_corpus_error_add(p->file, pos, msg);
}

static void corpus_errorc(CorpusParser *p, Int pos, const char *msg) {
    corpus_error(p, pos, str_from_cstr(msg));
}

static void corpus_error_expected(CorpusParser *p, Int pos, const char *what) {
    Str msg;
    if (pos == p->pos) {
        if (p->tok == CT_SEMICOLON && str_eq(p->lit, BURROW_S("\n")))
            msg = fmt_sprintf_v(p->a, "expected %s, found newline", what);
        else if (burrow__testing_corpus_tok_is_literal(p->tok))
            msg = fmt_sprintf_v(p->a, "expected %s, found %s", what, p->lit);
        else
            msg = fmt_sprintf_v(p->a, "expected %s, found '%s'", what,
                                corpus_tok_str(p->tok));
    } else {
        msg = fmt_sprintf_v(p->a, "expected %s", what);
    }
    corpus_error(p, pos, msg);
}

static void corpus_error_expected_tok(CorpusParser *p, Int pos, CorpusTok tok) {
    char what[16];
    size_t n = strlen(corpus_tok_str(tok));
    what[0] = '\'';
    memcpy(what + 1, corpus_tok_str(tok), n);
    what[n + 1] = '\'';
    what[n + 2] = '\0';
    corpus_error_expected(p, pos, what);
}

static Int corpus_expect(CorpusParser *p, CorpusTok tok) {
    Int pos = p->pos;
    if (p->tok != tok)
        corpus_error_expected_tok(p, pos, tok);
    corpus_next(p);
    return pos;
}

/* expect2 is like expect, but it returns an invalid position if the expected
 * token is not found. */
static Int corpus_expect2(CorpusParser *p, CorpusTok tok) {
    Int pos = 0;
    if (p->tok == tok)
        pos = p->pos;
    else
        corpus_error_expected_tok(p, p->pos, tok);
    corpus_next(p);
    return pos;
}

static bool corpus_at_newline(const CorpusParser *p) {
    return p->tok == CT_SEMICOLON && str_eq(p->lit, BURROW_S("\n"));
}

/* expectClosing is like expect but provides a better error message for the
 * common case of a missing comma before a newline. */
static Int corpus_expect_closing(CorpusParser *p, CorpusTok tok, const char *context) {
    if (p->tok != tok && corpus_at_newline(p)) {
        corpus_error(p, p->pos,
                     fmt_sprintf_v(p->a, "missing ',' before newline in %s", context));
        corpus_next(p);
    }
    return corpus_expect(p, tok);
}

static bool corpus_stmt_start(CorpusTok t) {
    switch ((int)t) {
    case CT_BREAK:
    case CT_CONST:
    case CT_CONTINUE:
    case CT_DEFER:
    case CT_FALLTHROUGH:
    case CT_FOR:
    case CT_GO:
    case CT_GOTO:
    case CT_IF:
    case CT_RETURN:
    case CT_SELECT:
    case CT_SWITCH:
    case CT_TYPE:
    case CT_VAR:
        return true;
    default:
        break;
    }
    return false;
}

static bool corpus_expr_end(CorpusTok t) {
    switch ((int)t) {
    case CT_COMMA:
    case CT_COLON:
    case CT_SEMICOLON:
    case CT_RPAREN:
    case CT_RBRACK:
    case CT_RBRACE:
        return true;
    default:
        break;
    }
    return false;
}

/* advance consumes tokens until the current token is in the to set, or EOF.
 * For error recovery. */
static void corpus_advance(CorpusParser *p, bool (*to)(CorpusTok)) {
    for (; p->tok != CT_EOF; corpus_next(p)) {
        if (to(p->tok)) {
            /* Return only if the parser made some progress since the last
             * sync or has not reached 10 advance calls without progress. */
            if (p->pos == p->sync_pos && p->sync_cnt < 10) {
                p->sync_cnt++;
                return;
            }
            if (p->pos > p->sync_pos) {
                p->sync_pos = p->pos;
                p->sync_cnt = 0;
                return;
            }
        }
    }
}

static void corpus_expect_semi(CorpusParser *p) {
    switch ((int)p->tok) {
    case CT_RPAREN:
    case CT_RBRACE:
        return;
    case CT_COMMA:
        corpus_error_expected(p, p->pos, "';'");
        corpus_next(p);
        return;
    case CT_SEMICOLON:
        corpus_next(p);
        return;
    default:
        corpus_error_expected(p, p->pos, "';'");
        corpus_advance(p, corpus_stmt_start);
    }
}

static bool corpus_at_comma(CorpusParser *p, const char *context, CorpusTok follow) {
    if (p->tok == CT_COMMA)
        return true;
    if (p->tok != follow) {
        corpus_error(p, p->pos,
                     fmt_sprintf_v(p->a, "missing ','%s in %s",
                                   corpus_at_newline(p) ? " before newline" : "",
                                   context));
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- identifiers */

static CorpusNode *corpus_parse_ident(CorpusParser *p) {
    Int pos = p->pos;
    Str name = BURROW_S("_");
    if (p->tok == CT_IDENT) {
        name = p->lit;
        corpus_next(p);
    } else {
        corpus_expect(p, CT_IDENT);
    }
    return corpus_ident(p, pos, name);
}

/* ------------------------------------------------------ common productions */

static CorpusNode *corpus_parse_expr(CorpusParser *p);
static CorpusNode *corpus_parse_rhs(CorpusParser *p);
static CorpusNode *corpus_parse_type(CorpusParser *p);
static CorpusNode *corpus_try_ident_or_type(CorpusParser *p);
static CorpusNode *corpus_parse_type_instance(CorpusParser *p, CorpusNode *typ);
static CorpusNode *corpus_embedded_elem(CorpusParser *p, CorpusNode *x);
static CorpusNode *corpus_parse_primary_expr(CorpusParser *p, CorpusNode *x);
static CorpusNode *corpus_parse_binary_expr(CorpusParser *p, CorpusNode *x, int prec1);
static CorpusNode *corpus_parse_stmt(CorpusParser *p);

static CorpusList corpus_parse_expr_list(CorpusParser *p) {
    CorpusList list = {0};
    corpus_list_add(p, &list, corpus_parse_expr(p));
    while (p->tok == CT_COMMA) {
        corpus_next(p);
        corpus_list_add(p, &list, corpus_parse_expr(p));
    }
    return list;
}

static CorpusList corpus_parse_list(CorpusParser *p, bool in_rhs) {
    bool old = p->in_rhs;
    p->in_rhs = in_rhs;
    CorpusList list = corpus_parse_expr_list(p);
    p->in_rhs = old;
    return list;
}

/* ------------------------------------------------------------------- types */

static CorpusNode *corpus_parse_type(CorpusParser *p) {
    CorpusNode *typ = corpus_try_ident_or_type(p);
    if (typ == NULL) {
        Int pos = p->pos;
        corpus_error_expected(p, pos, "type");
        corpus_advance(p, corpus_expr_end);
        return corpus_bad(p, pos, p->pos);
    }
    return typ;
}

static CorpusNode *corpus_parse_type_name(CorpusParser *p, CorpusNode *ident) {
    if (ident == NULL)
        ident = corpus_parse_ident(p);
    if (p->tok == CT_PERIOD) {
        corpus_next(p);
        CorpusNode *sel = corpus_parse_ident(p);
        return corpus_selector(p, ident, sel);
    }
    return ident;
}

static CorpusNode *corpus_parse_qualified_ident(CorpusParser *p, CorpusNode *ident) {
    CorpusNode *typ = corpus_parse_type_name(p, ident);
    if (p->tok == CT_LBRACK)
        typ = corpus_parse_type_instance(p, typ);
    return typ;
}

static CorpusNode *corpus_parse_array_type(CorpusParser *p, Int lbrack,
                                           CorpusNode *len) {
    if (len == NULL) {
        p->expr_lev++;
        if (p->tok == CT_ELLIPSIS) {
            len = corpus_new(p, CN_ELLIPSIS, p->pos, p->pos + 3);
            corpus_next(p);
        } else if (p->tok != CT_RBRACK) {
            len = corpus_parse_rhs(p);
        }
        p->expr_lev--;
    }
    if (p->tok == CT_COMMA) {
        corpus_errorc(p, p->pos, "unexpected comma; expecting ]");
        corpus_next(p);
    }
    corpus_expect(p, CT_RBRACK);
    CorpusNode *elt = corpus_parse_type(p);
    return corpus_array_type(p, lbrack, len, elt);
}

/* packIndexExpr. */
static CorpusNode *corpus_pack_index_expr(CorpusParser *p, CorpusNode *x,
                                          CorpusList exprs, Int rbrack) {
    if (exprs.len == 1)
        return corpus_index(p, x, exprs.v[0], rbrack);
    CorpusNode *n = corpus_new(p, CN_INDEX_LIST, x->pos, rbrack + 1);
    n->x = x;
    n->args = exprs.v;
    n->nargs = exprs.len;
    return n;
}

/* parseArrayFieldOrTypeInstance. The answer is the type, and *x is set to
 * NULL where Go answers a nil name. */
static CorpusNode *corpus_parse_array_field_or_type_instance(CorpusParser *p,
                                                             CorpusNode **x) {
    Int lbrack = corpus_expect(p, CT_LBRACK);
    Int trailing_comma = 0;
    CorpusList args = {0};
    if (p->tok != CT_RBRACK) {
        p->expr_lev++;
        corpus_list_add(p, &args, corpus_parse_rhs(p));
        while (p->tok == CT_COMMA) {
            Int comma = p->pos;
            corpus_next(p);
            if (p->tok == CT_RBRACK) {
                trailing_comma = comma;
                break;
            }
            corpus_list_add(p, &args, corpus_parse_rhs(p));
        }
        p->expr_lev--;
    }
    Int rbrack = corpus_expect(p, CT_RBRACK);

    if (args.len == 0) {
        CorpusNode *elt = corpus_parse_type(p);
        return corpus_array_type(p, lbrack, NULL, elt);
    }

    if (args.len == 1) {
        CorpusNode *elt = corpus_try_ident_or_type(p);
        if (elt != NULL) {
            if (trailing_comma != 0)
                corpus_errorc(p, trailing_comma, "unexpected comma; expecting ]");
            return corpus_array_type(p, lbrack, args.v[0], elt);
        }
    }

    CorpusNode *typ = corpus_pack_index_expr(p, *x, args, rbrack);
    *x = NULL;
    return typ;
}

/* parseFieldDecl. */
static void corpus_parse_field_decl(CorpusParser *p) {
    switch ((int)p->tok) {
    case CT_IDENT: {
        CorpusNode *name = corpus_parse_ident(p);
        if (p->tok == CT_PERIOD || p->tok == CT_STRING || p->tok == CT_SEMICOLON ||
            p->tok == CT_RBRACE) {
            if (p->tok == CT_PERIOD)
                corpus_parse_qualified_ident(p, name);
        } else {
            Int names = 1;
            while (p->tok == CT_COMMA) {
                corpus_next(p);
                corpus_parse_ident(p);
                names++;
            }
            if (names == 1 && p->tok == CT_LBRACK)
                corpus_parse_array_field_or_type_instance(p, &name);
            else
                corpus_parse_type(p);
        }
        break;
    }
    case CT_MUL:
        corpus_next(p);
        if (p->tok == CT_LPAREN) {
            corpus_errorc(p, p->pos, "cannot parenthesize embedded type");
            corpus_next(p);
            corpus_parse_qualified_ident(p, NULL);
            if (p->tok == CT_RPAREN)
                corpus_next(p);
        } else {
            corpus_parse_qualified_ident(p, NULL);
        }
        break;
    case CT_LPAREN:
        corpus_errorc(p, p->pos, "cannot parenthesize embedded type");
        corpus_next(p);
        if (p->tok == CT_MUL)
            corpus_next(p);
        corpus_parse_qualified_ident(p, NULL);
        if (p->tok == CT_RPAREN)
            corpus_next(p);
        break;
    default:
        corpus_error_expected(p, p->pos, "field name or embedded type");
        corpus_advance(p, corpus_expr_end);
    }

    if (p->tok == CT_STRING)
        corpus_next(p);

    corpus_expect_semi(p);
}

static CorpusNode *corpus_parse_struct_type(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_STRUCT);
    corpus_expect(p, CT_LBRACE);
    while (p->tok == CT_IDENT || p->tok == CT_MUL || p->tok == CT_LPAREN)
        corpus_parse_field_decl(p);
    Int rbrace = corpus_expect(p, CT_RBRACE);
    return corpus_new(p, CN_STRUCT_TYPE, pos, rbrace + 1);
}

static CorpusNode *corpus_parse_pointer_type(CorpusParser *p) {
    Int star = corpus_expect(p, CT_MUL);
    CorpusNode *base = corpus_parse_type(p);
    return corpus_star(p, star, base);
}

static CorpusNode *corpus_parse_dots_type(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_ELLIPSIS);
    CorpusNode *elt = corpus_parse_type(p);
    CorpusNode *n = corpus_new(p, CN_ELLIPSIS, pos, elt->end);
    n->x = elt;
    return n;
}

typedef struct CorpusField {
    CorpusNode *name;
    CorpusNode *typ;
} CorpusField;

static CorpusField corpus_parse_param_decl(CorpusParser *p, CorpusNode *name,
                                           bool type_sets_ok) {
    CorpusField f = {0};
    CorpusTok ptok = p->tok;
    if (name != NULL) {
        p->tok = CT_IDENT; /* force the IDENT case in the switch below */
    } else if (type_sets_ok && p->tok == CT_TILDE) {
        f.typ = corpus_embedded_elem(p, NULL);
        return f;
    }

    switch ((int)p->tok) {
    case CT_IDENT:
        if (name != NULL) {
            f.name = name;
            p->tok = ptok;
        } else {
            f.name = corpus_parse_ident(p);
        }
        switch ((int)p->tok) {
        case CT_IDENT:
        case CT_MUL:
        case CT_ARROW:
        case CT_FUNC:
        case CT_CHAN:
        case CT_MAP:
        case CT_STRUCT:
        case CT_INTERFACE:
        case CT_LPAREN:
            f.typ = corpus_parse_type(p);
            break;
        case CT_LBRACK:
            f.typ = corpus_parse_array_field_or_type_instance(p, &f.name);
            break;
        case CT_ELLIPSIS:
            f.typ = corpus_parse_dots_type(p);
            return f;
        case CT_PERIOD:
            f.typ = corpus_parse_qualified_ident(p, f.name);
            f.name = NULL;
            break;
        case CT_TILDE:
            if (type_sets_ok) {
                f.typ = corpus_embedded_elem(p, NULL);
                return f;
            }
            break;
        case CT_OR:
            if (type_sets_ok) {
                f.typ = corpus_embedded_elem(p, f.name);
                f.name = NULL;
                return f;
            }
            break;
        default:
            break;
        }
        break;
    case CT_MUL:
    case CT_ARROW:
    case CT_FUNC:
    case CT_LBRACK:
    case CT_CHAN:
    case CT_MAP:
    case CT_STRUCT:
    case CT_INTERFACE:
    case CT_LPAREN:
        f.typ = corpus_parse_type(p);
        break;
    case CT_ELLIPSIS:
        f.typ = corpus_parse_dots_type(p);
        return f;
    default:
        corpus_error_expected(p, p->pos, "')'");
        corpus_advance(p, corpus_expr_end);
    }

    if (type_sets_ok && p->tok == CT_OR && f.typ != NULL)
        f.typ = corpus_embedded_elem(p, f.typ);
    return f;
}

/* parseParameterList. The fields themselves are never looked at again, so
 * the answer is only how many there were. */
static Int corpus_parse_parameter_list(CorpusParser *p, CorpusNode *name0,
                                       CorpusNode *typ0, CorpusTok closing,
                                       bool dddok) {
    bool tparams = closing == CT_RBRACK;

    Int pos0 = p->pos;
    if (name0 != NULL)
        pos0 = name0->pos;
    else if (typ0 != NULL)
        pos0 = typ0->pos;

    CorpusField *list = NULL;
    Int len = 0, cap = 0;
    Int named = 0; /* parameters that have an explicit name and type */
    Int typed = 0; /* parameters that have an explicit type */

    while (name0 != NULL || (p->tok != closing && p->tok != CT_EOF)) {
        CorpusField par;
        if (typ0 != NULL) {
            if (tparams)
                typ0 = corpus_embedded_elem(p, typ0);
            par = (CorpusField){name0, typ0};
        } else {
            par = corpus_parse_param_decl(p, name0, tparams);
        }
        name0 = NULL;
        typ0 = NULL;
        if (par.name != NULL || par.typ != NULL) {
            if (len == cap) {
                Int ncap = cap == 0 ? 4 : cap * 2;
                CorpusField *nl =
                    burrow__testing_corpus_alloc(p->a, (size_t)ncap * sizeof *nl);
                if (len > 0)
                    memcpy(nl, list, (size_t)len * sizeof *nl);
                list = nl;
                cap = ncap;
            }
            list[len++] = par;
            if (par.name != NULL && par.typ != NULL)
                named++;
            if (par.typ != NULL)
                typed++;
        }
        if (!corpus_at_comma(p, "parameter list", closing))
            break;
        corpus_next(p);
    }

    if (len == 0)
        return 0;

    if (named == 0) {
        /* All unnamed: the names were types. */
        for (Int i = 0; i < len; i++) {
            if (list[i].name != NULL) {
                list[i].typ = list[i].name;
                list[i].name = NULL;
            }
        }
        if (tparams) {
            Int err_pos;
            Str msg;
            if (named == typed) {
                err_pos = p->pos;
                msg = BURROW_S("missing type constraint");
            } else {
                err_pos = pos0;
                msg = len == 1
                          ? BURROW_S(
                                "missing type parameter name or invalid array length")
                          : BURROW_S("missing type parameter name");
            }
            corpus_error(p, err_pos, msg);
        }
    } else if (named != len) {
        /* Some named or we are in a type parameter list. */
        Int err_pos = 0;
        CorpusNode *typ = NULL;
        for (Int i = 0; i < len; i++) {
            CorpusField *par = &list[len - i - 1];
            if (par->typ != NULL) {
                typ = par->typ;
                if (par->name == NULL) {
                    err_pos = typ->pos;
                    par->name = corpus_ident(p, err_pos, BURROW_S("_"));
                }
            } else if (typ != NULL) {
                par->typ = typ;
            } else {
                err_pos = par->name->pos;
                par->typ = corpus_bad(p, err_pos, p->pos);
            }
        }
        if (err_pos != 0) {
            Str msg;
            if (named == typed) {
                err_pos = p->pos;
                msg = tparams ? BURROW_S("missing type constraint")
                              : BURROW_S("missing parameter type");
            } else if (tparams) {
                msg = len == 1
                          ? BURROW_S(
                                "missing type parameter name or invalid array length")
                          : BURROW_S("missing type parameter name");
            } else {
                msg = BURROW_S("missing parameter name");
            }
            corpus_error(p, err_pos, msg);
        }
    }

    /* The type of a ... parameter is only allowed last, and not at all in a
     * type parameter list or a result list. */
    bool first = true;
    for (Int i = 0; i < len; i++) {
        CorpusNode *t = list[i].typ;
        if (t->kind == CN_ELLIPSIS && (!dddok || i + 1 < len)) {
            if (first) {
                first = false;
                corpus_errorc(p, t->pos,
                              dddok ? "can only use ... with final parameter"
                                    : "invalid use of ...");
            }
            list[i].typ = corpus_bad(p, t->pos, t->end);
        }
    }
    return len;
}

/* A FieldList, as far as anything reads one: whether there is one, and where
 * it starts and ends. */
typedef struct CorpusFieldList {
    bool present;
    Int pos;
    Int end;
} CorpusFieldList;

static bool corpus_parse_type_parameters(CorpusParser *p, Int *lbrack_out) {
    Int lbrack = corpus_expect(p, CT_LBRACK);
    Int n = 0;
    if (p->tok != CT_RBRACK)
        n = corpus_parse_parameter_list(p, NULL, NULL, CT_RBRACK, false);
    Int rbrack = corpus_expect(p, CT_RBRACK);
    if (n == 0) {
        corpus_errorc(p, rbrack, "empty type parameter list");
        return false;
    }
    *lbrack_out = lbrack;
    return true;
}

static CorpusFieldList corpus_parse_parameters(CorpusParser *p, bool result) {
    CorpusFieldList fl = {0};
    if (!result || p->tok == CT_LPAREN) {
        Int lparen = corpus_expect(p, CT_LPAREN);
        if (p->tok != CT_RPAREN)
            corpus_parse_parameter_list(p, NULL, NULL, CT_RPAREN, !result);
        Int rparen = corpus_expect(p, CT_RPAREN);
        fl.present = true;
        fl.pos = lparen;
        fl.end = rparen + 1;
        return fl;
    }
    CorpusNode *typ = corpus_try_ident_or_type(p);
    if (typ != NULL) {
        fl.present = true;
        fl.pos = typ->pos;
        fl.end = typ->end;
    }
    return fl;
}

static CorpusNode *corpus_func_type(CorpusParser *p, Int func, CorpusFieldList params,
                                    CorpusFieldList results) {
    Int pos = func != 0 ? func : params.pos;
    return corpus_new(p, CN_FUNC_TYPE, pos, results.present ? results.end : params.end);
}

static CorpusNode *corpus_parse_func_type(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_FUNC);
    if (p->tok == CT_LBRACK) {
        Int lbrack;
        if (corpus_parse_type_parameters(p, &lbrack))
            corpus_errorc(p, lbrack, "function type must have no type parameters");
    }
    CorpusFieldList params = corpus_parse_parameters(p, false);
    CorpusFieldList results = corpus_parse_parameters(p, true);
    return corpus_func_type(p, pos, params, results);
}

/* parseMethodSpec. The answer is the field's type, and *named says whether
 * the field has names. */
static CorpusNode *corpus_parse_method_spec(CorpusParser *p, bool *named) {
    *named = false;
    CorpusNode *x = corpus_parse_type_name(p, NULL);
    if (x->kind != CN_IDENT) {
        if (p->tok == CT_LBRACK)
            x = corpus_parse_type_instance(p, x);
        return x;
    }
    CorpusNode *ident = x;
    if (p->tok == CT_LBRACK) {
        /* Generic method or embedded instantiated type. */
        Int lbrack = p->pos;
        corpus_next(p);
        p->expr_lev++;
        CorpusNode *e = corpus_parse_expr(p);
        p->expr_lev--;
        if (e->kind == CN_IDENT && p->tok != CT_COMMA && p->tok != CT_RBRACK) {
            /* Generic method m[T any]. */
            corpus_parse_parameter_list(p, e, NULL, CT_RBRACK, false);
            corpus_expect(p, CT_RBRACK);
            corpus_errorc(p, lbrack, "interface method must have no type parameters");
            CorpusFieldList params = corpus_parse_parameters(p, false);
            CorpusFieldList results = corpus_parse_parameters(p, true);
            *named = true;
            return corpus_func_type(p, 0, params, results);
        }
        /* Embedded instantiated type. */
        CorpusList list = {0};
        corpus_list_add(p, &list, e);
        if (corpus_at_comma(p, "type argument list", CT_RBRACK)) {
            p->expr_lev++;
            corpus_next(p);
            while (p->tok != CT_RBRACK && p->tok != CT_EOF) {
                corpus_list_add(p, &list, corpus_parse_type(p));
                if (!corpus_at_comma(p, "type argument list", CT_RBRACK))
                    break;
                corpus_next(p);
            }
            p->expr_lev--;
        }
        Int rbrack = corpus_expect_closing(p, CT_RBRACK, "type argument list");
        return corpus_pack_index_expr(p, ident, list, rbrack);
    }
    if (p->tok == CT_LPAREN) {
        /* Ordinary method. */
        CorpusFieldList params = corpus_parse_parameters(p, false);
        CorpusFieldList results = corpus_parse_parameters(p, true);
        *named = true;
        return corpus_func_type(p, 0, params, results);
    }
    /* Embedded type. */
    return x;
}

static CorpusNode *corpus_embedded_term(CorpusParser *p) {
    if (p->tok == CT_TILDE) {
        Int pos = p->pos;
        corpus_next(p);
        CorpusNode *x = corpus_parse_type(p);
        return corpus_unary(p, pos, CT_TILDE, x);
    }
    CorpusNode *t = corpus_try_ident_or_type(p);
    if (t == NULL) {
        Int pos = p->pos;
        corpus_error_expected(p, pos, "~ term or type");
        corpus_advance(p, corpus_expr_end);
        return corpus_bad(p, pos, p->pos);
    }
    return t;
}

static CorpusNode *corpus_embedded_elem(CorpusParser *p, CorpusNode *x) {
    if (x == NULL)
        x = corpus_embedded_term(p);
    while (p->tok == CT_OR) {
        Int op_pos = p->pos;
        corpus_next(p);
        CorpusNode *y = corpus_embedded_term(p);
        x = corpus_binary(p, x, op_pos, CT_OR, y);
    }
    return x;
}

static CorpusNode *corpus_parse_interface_type(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_INTERFACE);
    corpus_expect(p, CT_LBRACE);
    for (;;) {
        if (p->tok == CT_IDENT) {
            bool named;
            CorpusNode *typ = corpus_parse_method_spec(p, &named);
            if (!named)
                corpus_embedded_elem(p, typ);
            corpus_expect_semi(p);
        } else if (p->tok == CT_TILDE) {
            corpus_embedded_elem(p, NULL);
            corpus_expect_semi(p);
        } else {
            CorpusNode *t = corpus_try_ident_or_type(p);
            if (t == NULL)
                break;
            corpus_embedded_elem(p, t);
            corpus_expect_semi(p);
        }
    }
    Int rbrace = corpus_expect(p, CT_RBRACE);
    return corpus_new(p, CN_INTERFACE_TYPE, pos, rbrace + 1);
}

static CorpusNode *corpus_parse_map_type(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_MAP);
    corpus_expect(p, CT_LBRACK);
    CorpusNode *key = corpus_parse_type(p);
    corpus_expect(p, CT_RBRACK);
    CorpusNode *value = corpus_parse_type(p);
    CorpusNode *n = corpus_new(p, CN_MAP_TYPE, pos, value->end);
    n->y = key;
    n->x = value;
    return n;
}

static CorpusNode *corpus_parse_chan_type(CorpusParser *p) {
    Int pos = p->pos;
    int dir = CORPUS_SEND | CORPUS_RECV;
    Int arrow = 0;
    if (p->tok == CT_CHAN) {
        corpus_next(p);
        if (p->tok == CT_ARROW) {
            arrow = p->pos;
            corpus_next(p);
            dir = CORPUS_SEND;
        }
    } else {
        arrow = corpus_expect(p, CT_ARROW);
        corpus_expect(p, CT_CHAN);
        dir = CORPUS_RECV;
    }
    CorpusNode *value = corpus_parse_type(p);
    CorpusNode *n = corpus_new(p, CN_CHAN_TYPE, pos, value->end);
    n->op_pos = arrow;
    n->dir = dir;
    n->x = value;
    return n;
}

static CorpusNode *corpus_parse_type_instance(CorpusParser *p, CorpusNode *typ) {
    Int opening = corpus_expect(p, CT_LBRACK);
    p->expr_lev++;
    CorpusList list = {0};
    while (p->tok != CT_RBRACK && p->tok != CT_EOF) {
        corpus_list_add(p, &list, corpus_parse_type(p));
        if (!corpus_at_comma(p, "type argument list", CT_RBRACK))
            break;
        corpus_next(p);
    }
    p->expr_lev--;

    Int closing = corpus_expect_closing(p, CT_RBRACK, "type argument list");

    if (list.len == 0) {
        corpus_error_expected(p, closing, "type argument list");
        return corpus_index(p, typ, corpus_bad(p, opening + 1, closing), closing);
    }
    return corpus_pack_index_expr(p, typ, list, closing);
}

static CorpusNode *corpus_try_ident_or_type_nested(CorpusParser *p) {
    switch ((int)p->tok) {
    case CT_IDENT: {
        CorpusNode *typ = corpus_parse_type_name(p, NULL);
        if (p->tok == CT_LBRACK)
            typ = corpus_parse_type_instance(p, typ);
        return typ;
    }
    case CT_LBRACK: {
        Int lbrack = corpus_expect(p, CT_LBRACK);
        return corpus_parse_array_type(p, lbrack, NULL);
    }
    case CT_STRUCT:
        return corpus_parse_struct_type(p);
    case CT_MUL:
        return corpus_parse_pointer_type(p);
    case CT_FUNC:
        return corpus_parse_func_type(p);
    case CT_INTERFACE:
        return corpus_parse_interface_type(p);
    case CT_MAP:
        return corpus_parse_map_type(p);
    case CT_CHAN:
    case CT_ARROW:
        return corpus_parse_chan_type(p);
    case CT_LPAREN: {
        Int lparen = p->pos;
        corpus_next(p);
        CorpusNode *typ = corpus_parse_type(p);
        Int rparen = corpus_expect(p, CT_RPAREN);
        return corpus_paren(p, lparen, typ, rparen);
    }
    default:
        break;
    }
    return NULL;
}

static CorpusNode *corpus_try_ident_or_type(CorpusParser *p) {
    corpus_inc_nest_lev(p);
    CorpusNode *typ = corpus_try_ident_or_type_nested(p);
    corpus_dec_nest_lev(p);
    return typ;
}

/* ------------------------------------------------------------------ blocks */

static CorpusList corpus_parse_stmt_list(CorpusParser *p) {
    CorpusList list = {0};
    while (p->tok != CT_CASE && p->tok != CT_DEFAULT && p->tok != CT_RBRACE &&
           p->tok != CT_EOF)
        corpus_list_add(p, &list, corpus_parse_stmt(p));
    return list;
}

/* BlockStmt.End: the closing brace if there was one, or else the end of the
 * last statement, or else just past the opening brace. */
static CorpusNode *corpus_block(CorpusParser *p, Int lbrace, CorpusList list,
                                Int rbrace) {
    Int end;
    if (rbrace != 0)
        end = rbrace + 1;
    else if (list.len > 0)
        end = list.v[list.len - 1]->end;
    else
        end = lbrace + 1;
    return corpus_new(p, CN_STMT_OTHER, lbrace, end);
}

/* parseBody and parseBlockStmt, which are the same. */
static CorpusNode *corpus_parse_block_stmt(CorpusParser *p) {
    Int lbrace = corpus_expect(p, CT_LBRACE);
    CorpusList list = corpus_parse_stmt_list(p);
    Int rbrace = corpus_expect2(p, CT_RBRACE);
    return corpus_block(p, lbrace, list, rbrace);
}

/* ------------------------------------------------------------- expressions */

static CorpusNode *corpus_parse_func_type_or_lit(CorpusParser *p) {
    CorpusNode *typ = corpus_parse_func_type(p);
    if (p->tok != CT_LBRACE)
        return typ;

    p->expr_lev++;
    CorpusNode *body = corpus_parse_block_stmt(p);
    p->expr_lev--;

    CorpusNode *n = corpus_new(p, CN_FUNC_LIT, typ->pos, body->end);
    n->x = typ;
    n->y = body;
    return n;
}

static CorpusNode *corpus_parse_operand(CorpusParser *p) {
    switch ((int)p->tok) {
    case CT_IDENT:
        return corpus_parse_ident(p);
    case CT_INT:
    case CT_FLOAT:
    case CT_IMAG:
    case CT_CHAR:
    case CT_STRING: {
        CorpusNode *x = corpus_new(p, CN_BASIC_LIT, p->pos,
                                   burrow__testing_corpus_scan_end(&p->scanner));
        x->tok = p->tok;
        x->name = p->lit;
        corpus_next(p);
        return x;
    }
    case CT_LPAREN: {
        Int lparen = p->pos;
        corpus_next(p);
        p->expr_lev++;
        CorpusNode *x = corpus_parse_rhs(p);
        p->expr_lev--;
        Int rparen = corpus_expect(p, CT_RPAREN);
        return corpus_paren(p, lparen, x, rparen);
    }
    case CT_FUNC:
        return corpus_parse_func_type_or_lit(p);
    default:
        break;
    }

    CorpusNode *typ = corpus_try_ident_or_type(p);
    if (typ != NULL)
        return typ;

    Int pos = p->pos;
    corpus_error_expected(p, pos, "operand");
    corpus_advance(p, corpus_stmt_start);
    return corpus_bad(p, pos, p->pos);
}

static CorpusNode *corpus_parse_type_assertion(CorpusParser *p, CorpusNode *x) {
    Int lparen = corpus_expect(p, CT_LPAREN);
    CorpusNode *typ = NULL;
    if (p->tok == CT_TYPE)
        corpus_next(p); /* type switch: typ is nil */
    else
        typ = corpus_parse_type(p);
    Int rparen = corpus_expect(p, CT_RPAREN);
    CorpusNode *n = corpus_new(p, CN_TYPE_ASSERT, x->pos, rparen + 1);
    n->x = x;
    n->y = typ;
    n->lparen = lparen;
    n->rparen = rparen;
    return n;
}

static CorpusNode *corpus_parse_index_or_slice_or_instance(CorpusParser *p,
                                                           CorpusNode *x) {
    corpus_expect(p, CT_LBRACK);
    if (p->tok == CT_RBRACK) {
        /* Empty index, slice or index expressions are not permitted. Accept
         * them for parsing tolerance, but complain. */
        corpus_error_expected(p, p->pos, "operand");
        Int rbrack = p->pos;
        corpus_next(p);
        return corpus_index(p, x, corpus_bad(p, rbrack, rbrack), rbrack);
    }
    p->expr_lev++;

    CorpusList args = {0};
    CorpusNode *index[3] = {NULL, NULL, NULL};
    Int colons[2] = {0, 0};
    if (p->tok != CT_COLON)
        index[0] = corpus_parse_rhs(p);
    int ncolons = 0;
    switch ((int)p->tok) {
    case CT_COLON:
        while (p->tok == CT_COLON && ncolons < 2) {
            colons[ncolons] = p->pos;
            ncolons++;
            corpus_next(p);
            if (p->tok != CT_COLON && p->tok != CT_RBRACK && p->tok != CT_EOF)
                index[ncolons] = corpus_parse_rhs(p);
        }
        break;
    case CT_COMMA:
        corpus_list_add(p, &args, index[0]);
        while (p->tok == CT_COMMA) {
            corpus_next(p);
            if (p->tok != CT_RBRACK && p->tok != CT_EOF)
                corpus_list_add(p, &args, corpus_parse_type(p));
        }
        break;
    default:
        break;
    }

    p->expr_lev--;
    Int rbrack = corpus_expect(p, CT_RBRACK);

    if (ncolons > 0) {
        if (ncolons == 2) {
            if (index[1] == NULL)
                corpus_errorc(p, colons[0], "middle index required in 3-index slice");
            if (index[2] == NULL)
                corpus_errorc(p, colons[1], "final index required in 3-index slice");
        }
        CorpusNode *n = corpus_new(p, CN_SLICE, x->pos, rbrack + 1);
        n->x = x;
        return n;
    }

    if (args.len == 0)
        return corpus_index(p, x, index[0], rbrack);

    return corpus_pack_index_expr(p, x, args, rbrack);
}

static CorpusNode *corpus_parse_call_or_conversion(CorpusParser *p, CorpusNode *fun) {
    Int lparen = corpus_expect(p, CT_LPAREN);
    p->expr_lev++;
    CorpusList list = {0};
    Int ellipsis = 0;
    while (p->tok != CT_RPAREN && p->tok != CT_EOF && ellipsis == 0) {
        corpus_list_add(p, &list, corpus_parse_rhs(p));
        if (p->tok == CT_ELLIPSIS) {
            ellipsis = p->pos;
            corpus_next(p);
        }
        if (!corpus_at_comma(p, "argument list", CT_RPAREN))
            break;
        corpus_next(p);
    }
    p->expr_lev--;
    Int rparen = corpus_expect_closing(p, CT_RPAREN, "argument list");

    CorpusNode *n = corpus_new(p, CN_CALL, fun->pos, rparen + 1);
    n->x = fun;
    n->args = list.v;
    n->nargs = list.len;
    n->ellipsis = ellipsis;
    n->lparen = lparen;
    n->rparen = rparen;
    return n;
}

static CorpusNode *corpus_parse_literal_value(CorpusParser *p, CorpusNode *typ);

static CorpusNode *corpus_parse_value(CorpusParser *p) {
    if (p->tok == CT_LBRACE)
        return corpus_parse_literal_value(p, NULL);
    return corpus_parse_expr(p);
}

static CorpusNode *corpus_parse_element(CorpusParser *p) {
    CorpusNode *x = corpus_parse_value(p);
    if (p->tok == CT_COLON) {
        Int colon = p->pos;
        corpus_next(p);
        CorpusNode *value = corpus_parse_value(p);
        CorpusNode *kv = corpus_new(p, CN_KEY_VALUE, x->pos, value->end);
        kv->x = x;
        kv->y = value;
        kv->op_pos = colon;
        x = kv;
    }
    return x;
}

static CorpusList corpus_parse_element_list(CorpusParser *p) {
    CorpusList list = {0};
    while (p->tok != CT_RBRACE && p->tok != CT_EOF) {
        corpus_list_add(p, &list, corpus_parse_element(p));
        if (!corpus_at_comma(p, "composite literal", CT_RBRACE))
            break;
        corpus_next(p);
    }
    return list;
}

static CorpusNode *corpus_parse_literal_value_nested(CorpusParser *p, CorpusNode *typ) {
    Int lbrace = corpus_expect(p, CT_LBRACE);
    CorpusList elts = {0};
    p->expr_lev++;
    if (p->tok != CT_RBRACE)
        elts = corpus_parse_element_list(p);
    p->expr_lev--;
    Int rbrace = corpus_expect_closing(p, CT_RBRACE, "composite literal");
    CorpusNode *n =
        corpus_new(p, CN_COMPOSITE_LIT, typ != NULL ? typ->pos : lbrace, rbrace + 1);
    n->x = typ;
    n->args = elts.v;
    n->nargs = elts.len;
    return n;
}

static CorpusNode *corpus_parse_literal_value(CorpusParser *p, CorpusNode *typ) {
    corpus_inc_nest_lev(p);
    CorpusNode *n = corpus_parse_literal_value_nested(p, typ);
    corpus_dec_nest_lev(p);
    return n;
}

/* parsePrimaryExpr. Go counts the nesting once per trip round the loop and
 * takes it all back on the way out, which is what n is for. */
static CorpusNode *corpus_parse_primary_expr(CorpusParser *p, CorpusNode *x) {
    if (x == NULL)
        x = corpus_parse_operand(p);
    Int n;
    for (n = 1;; n++) {
        corpus_inc_nest_lev(p);
        switch ((int)p->tok) {
        case CT_PERIOD:
            corpus_next(p);
            switch ((int)p->tok) {
            case CT_IDENT:
                x = corpus_selector(p, x, corpus_parse_ident(p));
                break;
            case CT_LPAREN:
                x = corpus_parse_type_assertion(p, x);
                break;
            default: {
                Int pos = p->pos;
                corpus_error_expected(p, pos, "selector or type assertion");
                if (p->tok != CT_RBRACE)
                    corpus_next(p); /* make progress */
                x = corpus_selector(p, x, corpus_ident(p, pos, BURROW_S("_")));
            }
            }
            break;
        case CT_LBRACK:
            x = corpus_parse_index_or_slice_or_instance(p, x);
            break;
        case CT_LPAREN:
            x = corpus_parse_call_or_conversion(p, x);
            break;
        case CT_LBRACE: {
            /* The operand may have been a parenthesized composite literal
             * type. Accept it but complain if it has a literal. */
            CorpusNode *t = corpus_unparen(x);
            switch ((int)t->kind) {
            case CN_BAD:
            case CN_IDENT:
            case CN_SELECTOR:
            case CN_INDEX:
            case CN_INDEX_LIST:
                if (p->expr_lev < 0)
                    goto done;
                break;
            case CN_ARRAY_TYPE:
            case CN_STRUCT_TYPE:
            case CN_MAP_TYPE:
                break;
            default:
                goto done;
            }
            if (t != x)
                corpus_errorc(p, t->pos,
                              "cannot parenthesize type in composite literal");
            x = corpus_parse_literal_value(p, x);
            break;
        }
        default:
            goto done;
        }
    }
done:
    p->nest_lev -= (int)n;
    return x;
}

static CorpusNode *corpus_parse_unary_expr(CorpusParser *p);

static CorpusNode *corpus_parse_unary_expr_nested(CorpusParser *p) {
    switch ((int)p->tok) {
    case CT_ADD:
    case CT_SUB:
    case CT_NOT:
    case CT_XOR:
    case CT_AND:
    case CT_TILDE: {
        Int pos = p->pos;
        CorpusTok op = p->tok;
        corpus_next(p);
        CorpusNode *x = corpus_parse_unary_expr(p);
        return corpus_unary(p, pos, op, x);
    }
    case CT_ARROW: {
        /* A channel type or a receive expression. */
        Int arrow = p->pos;
        corpus_next(p);

        CorpusNode *x = corpus_parse_unary_expr(p);

        if (x->kind == CN_CHAN_TYPE) {
            /* (<-type): re-associate position info and <-. */
            CorpusNode *typ = x;
            int dir = CORPUS_SEND;
            while (typ != NULL && dir == CORPUS_SEND) {
                if (typ->dir == CORPUS_RECV) /* (<-type) is (<-(<-chan T)) */
                    corpus_error_expected(p, typ->op_pos, "'chan'");
                Int old = typ->op_pos;
                typ->pos = arrow;
                typ->op_pos = arrow;
                arrow = old;
                dir = typ->dir;
                typ->dir = CORPUS_RECV;
                typ = typ->x->kind == CN_CHAN_TYPE ? typ->x : NULL;
            }
            if (dir == CORPUS_SEND)
                corpus_error_expected(p, arrow, "channel type");
            return x;
        }

        /* <-(expr) */
        return corpus_unary(p, arrow, CT_ARROW, x);
    }
    case CT_MUL: {
        /* A pointer type or a unary * expression. */
        Int pos = p->pos;
        corpus_next(p);
        CorpusNode *x = corpus_parse_unary_expr(p);
        return corpus_star(p, pos, x);
    }
    default:
        break;
    }
    return corpus_parse_primary_expr(p, NULL);
}

static CorpusNode *corpus_parse_unary_expr(CorpusParser *p) {
    corpus_inc_nest_lev(p);
    CorpusNode *x = corpus_parse_unary_expr_nested(p);
    corpus_dec_nest_lev(p);
    return x;
}

static CorpusTok corpus_tok_prec(const CorpusParser *p, int *prec) {
    CorpusTok tok = p->tok;
    if (p->in_rhs && tok == CT_ASSIGN)
        tok = CT_EQL;
    *prec = burrow__testing_corpus_tok_prec(tok);
    return tok;
}

static CorpusNode *corpus_parse_binary_expr(CorpusParser *p, CorpusNode *x, int prec1) {
    if (x == NULL)
        x = corpus_parse_unary_expr(p);
    Int n;
    for (n = 1;; n++) {
        corpus_inc_nest_lev(p);
        int oprec;
        CorpusTok op = corpus_tok_prec(p, &oprec);
        if (oprec < prec1)
            break;
        Int pos = corpus_expect(p, op);
        CorpusNode *y = corpus_parse_binary_expr(p, NULL, oprec + 1);
        x = corpus_binary(p, x, pos, op, y);
    }
    p->nest_lev -= (int)n;
    return x;
}

static CorpusNode *corpus_parse_expr(CorpusParser *p) {
    return corpus_parse_binary_expr(p, NULL, CORPUS_LOWEST_PREC + 1);
}

static CorpusNode *corpus_parse_rhs(CorpusParser *p) {
    bool old = p->in_rhs;
    p->in_rhs = true;
    CorpusNode *x = corpus_parse_expr(p);
    p->in_rhs = old;
    return x;
}

/* -------------------------------------------------------------- statements */

enum { CORPUS_BASIC, CORPUS_LABEL_OK, CORPUS_RANGE_OK };

static CorpusNode *corpus_stmt(CorpusParser *p, CorpusNodeKind kind, Int pos, Int end) {
    return corpus_new(p, kind, pos, end);
}

/* parseSimpleStmt. *is_range is its second result. */
static CorpusNode *corpus_parse_simple_stmt(CorpusParser *p, int mode, bool *is_range) {
    *is_range = false;
    CorpusList x = corpus_parse_list(p, false);

    switch ((int)p->tok) {
    case CT_DEFINE:
    case CT_ASSIGN:
    case CT_ADD_ASSIGN:
    case CT_SUB_ASSIGN:
    case CT_MUL_ASSIGN:
    case CT_QUO_ASSIGN:
    case CT_REM_ASSIGN:
    case CT_AND_ASSIGN:
    case CT_OR_ASSIGN:
    case CT_XOR_ASSIGN:
    case CT_SHL_ASSIGN:
    case CT_SHR_ASSIGN:
    case CT_AND_NOT_ASSIGN: {
        /* An assignment statement, possibly part of a range clause. */
        Int pos = p->pos;
        CorpusTok tok = p->tok;
        corpus_next(p);
        CorpusList y = {0};
        if (mode == CORPUS_RANGE_OK && p->tok == CT_RANGE &&
            (tok == CT_DEFINE || tok == CT_ASSIGN)) {
            Int rpos = p->pos;
            corpus_next(p);
            corpus_list_add(p, &y,
                            corpus_unary(p, rpos, CT_RANGE, corpus_parse_rhs(p)));
            *is_range = true;
        } else {
            y = corpus_parse_list(p, true);
        }
        CorpusNode *s =
            corpus_stmt(p, CN_STMT_ASSIGN, x.v[0]->pos, y.v[y.len - 1]->end);
        s->args = x.v;
        s->nargs = x.len;
        s->rhs = y.v;
        s->nrhs = y.len;
        s->op_pos = pos;
        s->tok = tok;
        return s;
    }
    default:
        break;
    }

    if (x.len > 1)
        corpus_error_expected(p, x.v[0]->pos, "1 expression");

    switch ((int)p->tok) {
    case CT_COLON: {
        /* A labeled statement. */
        Int colon = p->pos;
        corpus_next(p);
        if (mode == CORPUS_LABEL_OK && x.v[0]->kind == CN_IDENT) {
            CorpusNode *stmt = corpus_parse_stmt(p);
            CorpusNode *s = corpus_stmt(p, CN_STMT_LABELED, x.v[0]->pos, stmt->end);
            s->x = stmt;
            return s;
        }
        /* The label may be wrong because of a token between it and the colon,
         * so the error goes at the colon. */
        corpus_errorc(p, colon, "illegal label declaration");
        return corpus_stmt(p, CN_STMT_OTHER, x.v[0]->pos, colon + 1);
    }
    case CT_ARROW: {
        /* A send statement. */
        corpus_next(p);
        CorpusNode *y = corpus_parse_rhs(p);
        return corpus_stmt(p, CN_STMT_OTHER, x.v[0]->pos, y->end);
    }
    case CT_INC:
    case CT_DEC: {
        CorpusNode *s = corpus_stmt(p, CN_STMT_OTHER, x.v[0]->pos, p->pos + 2);
        corpus_next(p);
        return s;
    }
    default:
        break;
    }

    CorpusNode *s = corpus_stmt(p, CN_STMT_EXPR, x.v[0]->pos, x.v[0]->end);
    s->x = x.v[0];
    return s;
}

static CorpusNode *corpus_parse_call_expr(CorpusParser *p, const char *call_type) {
    CorpusNode *x = corpus_parse_rhs(p); /* could be a conversion: (some type)(x) */
    CorpusNode *t = corpus_unparen(x);
    if (t != x) {
        corpus_error(p, x->pos,
                     fmt_sprintf_v(p->a, "expression in %s must not be parenthesized",
                                   call_type));
        x = t;
    }
    if (x->kind == CN_CALL)
        return x;
    if (x->kind != CN_BAD) /* only report the error if it is a new one */
        corpus_error(
            p, x->end,
            fmt_sprintf_v(p->a, "expression in %s must be function call", call_type));
    return NULL;
}

/* parseGoStmt and parseDeferStmt. */
static CorpusNode *corpus_parse_go_or_defer_stmt(CorpusParser *p, CorpusTok keyword) {
    Int pos = corpus_expect(p, keyword);
    CorpusNode *call = corpus_parse_call_expr(p, corpus_tok_str(keyword));
    corpus_expect_semi(p);
    if (call == NULL)
        return corpus_stmt(p, CN_STMT_OTHER, pos,
                           pos + (Int)strlen(corpus_tok_str(keyword)));
    return corpus_stmt(p, CN_STMT_OTHER, pos, call->end);
}

static CorpusNode *corpus_parse_return_stmt(CorpusParser *p) {
    Int pos = p->pos;
    corpus_expect(p, CT_RETURN);
    CorpusList x = {0};
    if (p->tok != CT_SEMICOLON && p->tok != CT_RBRACE)
        x = corpus_parse_list(p, true);
    corpus_expect_semi(p);
    return corpus_stmt(p, CN_STMT_OTHER, pos,
                       x.len > 0 ? x.v[x.len - 1]->end : pos + 6);
}

static CorpusNode *corpus_parse_branch_stmt(CorpusParser *p, CorpusTok tok) {
    Int pos = corpus_expect(p, tok);
    CorpusNode *label = NULL;
    if (tok == CT_GOTO ||
        ((tok == CT_CONTINUE || tok == CT_BREAK) && p->tok == CT_IDENT))
        label = corpus_parse_ident(p);
    corpus_expect_semi(p);
    Int end = label != NULL ? label->end : pos + (Int)strlen(corpus_tok_str(tok));
    return corpus_stmt(p, CN_STMT_OTHER, pos, end);
}

static CorpusNode *corpus_make_expr(CorpusParser *p, CorpusNode *s, const char *want) {
    if (s == NULL)
        return NULL;
    if (s->kind == CN_STMT_EXPR)
        return s->x;
    const char *found = s->kind == CN_STMT_ASSIGN ? "assignment" : "simple statement";
    corpus_error(
        p, s->pos,
        fmt_sprintf_v(p->a,
                      "expected %s, found %s (missing parentheses around composite "
                      "literal?)",
                      want, found));
    return corpus_bad(p, s->pos, s->end);
}

static void corpus_parse_if_header(CorpusParser *p) {
    if (p->tok == CT_LBRACE) {
        corpus_errorc(p, p->pos, "missing condition in if statement");
        return;
    }

    int prev_lev = p->expr_lev;
    p->expr_lev = -1;

    CorpusNode *init = NULL;
    bool is_range;
    if (p->tok != CT_SEMICOLON) {
        /* Accept a variable declaration but complain. */
        if (p->tok == CT_VAR) {
            corpus_next(p);
            corpus_errorc(p, p->pos, "var declaration not allowed in if initializer");
        }
        init = corpus_parse_simple_stmt(p, CORPUS_BASIC, &is_range);
    }

    CorpusNode *cond_stmt = NULL;
    Int semi_pos = 0;
    Str semi_lit = {0};
    if (p->tok != CT_LBRACE) {
        if (p->tok == CT_SEMICOLON) {
            semi_pos = p->pos;
            semi_lit = p->lit;
            corpus_next(p);
        } else {
            corpus_expect(p, CT_SEMICOLON);
        }
        if (p->tok != CT_LBRACE)
            cond_stmt = corpus_parse_simple_stmt(p, CORPUS_BASIC, &is_range);
    } else {
        cond_stmt = init;
    }

    if (cond_stmt != NULL) {
        corpus_make_expr(p, cond_stmt, "boolean expression");
    } else if (semi_pos != 0) {
        if (str_eq(semi_lit, BURROW_S("\n")))
            corpus_errorc(p, semi_pos,
                          "unexpected newline, expecting { after if clause");
        else
            corpus_errorc(p, semi_pos, "missing condition in if statement");
    }

    p->expr_lev = prev_lev;
}

static CorpusNode *corpus_parse_if_stmt(CorpusParser *p);

static CorpusNode *corpus_parse_if_stmt_nested(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_IF);

    corpus_parse_if_header(p);
    CorpusNode *body = corpus_parse_block_stmt(p);

    CorpusNode *else_ = NULL;
    if (p->tok == CT_ELSE) {
        corpus_next(p);
        switch ((int)p->tok) {
        case CT_IF:
            else_ = corpus_parse_if_stmt(p);
            break;
        case CT_LBRACE:
            else_ = corpus_parse_block_stmt(p);
            corpus_expect_semi(p);
            break;
        default:
            corpus_error_expected(p, p->pos, "if statement or block");
            else_ = corpus_stmt(p, CN_STMT_OTHER, p->pos, p->pos);
        }
    } else {
        corpus_expect_semi(p);
    }

    return corpus_stmt(p, CN_STMT_OTHER, pos, else_ != NULL ? else_->end : body->end);
}

static CorpusNode *corpus_parse_if_stmt(CorpusParser *p) {
    corpus_inc_nest_lev(p);
    CorpusNode *s = corpus_parse_if_stmt_nested(p);
    corpus_dec_nest_lev(p);
    return s;
}

/* CaseClause.End and CommClause.End. */
static CorpusNode *corpus_clause(CorpusParser *p, Int pos, Int colon, CorpusList body) {
    return corpus_stmt(p, CN_STMT_OTHER, pos,
                       body.len > 0 ? body.v[body.len - 1]->end : colon + 1);
}

static CorpusNode *corpus_parse_case_clause(CorpusParser *p) {
    Int pos = p->pos;
    if (p->tok == CT_CASE) {
        corpus_next(p);
        corpus_parse_list(p, true);
    } else {
        corpus_expect(p, CT_DEFAULT);
    }

    Int colon = corpus_expect(p, CT_COLON);
    CorpusList body = corpus_parse_stmt_list(p);
    return corpus_clause(p, pos, colon, body);
}

static bool corpus_is_type_switch_assert(const CorpusNode *x) {
    return x->kind == CN_TYPE_ASSERT && x->y == NULL;
}

static bool corpus_is_type_switch_guard(CorpusParser *p, const CorpusNode *s) {
    if (s == NULL)
        return false;
    if (s->kind == CN_STMT_EXPR)
        return corpus_is_type_switch_assert(s->x); /* x.(type) */
    if (s->kind == CN_STMT_ASSIGN && s->nargs == 1 && s->nrhs == 1 &&
        corpus_is_type_switch_assert(s->rhs[0])) {
        /* v := x.(type), and v = x.(type) with a complaint. */
        if (s->tok == CT_ASSIGN) {
            corpus_errorc(p, s->op_pos, "expected ':=', found '='");
            return true;
        }
        return s->tok == CT_DEFINE;
    }
    return false;
}

static CorpusNode *corpus_parse_switch_stmt(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_SWITCH);

    CorpusNode *s2 = NULL;
    bool is_range;
    if (p->tok != CT_LBRACE) {
        int prev_lev = p->expr_lev;
        p->expr_lev = -1;
        if (p->tok != CT_SEMICOLON)
            s2 = corpus_parse_simple_stmt(p, CORPUS_BASIC, &is_range);
        if (p->tok == CT_SEMICOLON) {
            corpus_next(p);
            s2 = NULL;
            if (p->tok != CT_LBRACE)
                s2 = corpus_parse_simple_stmt(p, CORPUS_BASIC, &is_range);
        }
        p->expr_lev = prev_lev;
    }

    bool type_switch = corpus_is_type_switch_guard(p, s2);
    Int lbrace = corpus_expect(p, CT_LBRACE);
    CorpusList list = {0};
    while (p->tok == CT_CASE || p->tok == CT_DEFAULT)
        corpus_list_add(p, &list, corpus_parse_case_clause(p));
    Int rbrace = corpus_expect(p, CT_RBRACE);
    corpus_expect_semi(p);
    CorpusNode *body = corpus_block(p, lbrace, list, rbrace);

    if (!type_switch)
        corpus_make_expr(p, s2, "switch expression");
    return corpus_stmt(p, CN_STMT_OTHER, pos, body->end);
}

static CorpusNode *corpus_parse_comm_clause(CorpusParser *p) {
    Int pos = p->pos;
    if (p->tok == CT_CASE) {
        corpus_next(p);
        CorpusList lhs = corpus_parse_list(p, false);
        if (p->tok == CT_ARROW) {
            /* A send statement. */
            if (lhs.len > 1)
                corpus_error_expected(p, lhs.v[0]->pos, "1 expression");
            corpus_next(p);
            corpus_parse_rhs(p);
        } else if (p->tok == CT_ASSIGN || p->tok == CT_DEFINE) {
            /* A receive statement with an assignment. */
            if (lhs.len > 2)
                corpus_error_expected(p, lhs.v[0]->pos, "1 or 2 expressions");
            corpus_next(p);
            corpus_parse_rhs(p);
        } else {
            /* The left hand side must be a single receive operation. */
            if (lhs.len > 1)
                corpus_error_expected(p, lhs.v[0]->pos, "1 expression");
        }
    } else {
        corpus_expect(p, CT_DEFAULT);
    }

    Int colon = corpus_expect(p, CT_COLON);
    CorpusList body = corpus_parse_stmt_list(p);
    return corpus_clause(p, pos, colon, body);
}

static CorpusNode *corpus_parse_select_stmt(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_SELECT);
    Int lbrace = corpus_expect(p, CT_LBRACE);
    CorpusList list = {0};
    while (p->tok == CT_CASE || p->tok == CT_DEFAULT)
        corpus_list_add(p, &list, corpus_parse_comm_clause(p));
    Int rbrace = corpus_expect(p, CT_RBRACE);
    corpus_expect_semi(p);
    CorpusNode *body = corpus_block(p, lbrace, list, rbrace);
    return corpus_stmt(p, CN_STMT_OTHER, pos, body->end);
}

static CorpusNode *corpus_parse_for_stmt(CorpusParser *p) {
    Int pos = corpus_expect(p, CT_FOR);

    CorpusNode *s2 = NULL;
    bool is_range = false;
    if (p->tok != CT_LBRACE) {
        int prev_lev = p->expr_lev;
        p->expr_lev = -1;
        if (p->tok != CT_SEMICOLON) {
            if (p->tok == CT_RANGE) {
                /* "for range x", with no left hand side. */
                Int rpos = p->pos;
                corpus_next(p);
                CorpusNode *y = corpus_unary(p, rpos, CT_RANGE, corpus_parse_rhs(p));
                s2 = corpus_stmt(p, CN_STMT_ASSIGN, 0, y->end);
                s2->rhs =
                    (CorpusNode **)burrow__testing_corpus_alloc(p->a, sizeof *s2->rhs);
                s2->rhs[0] = y;
                s2->nrhs = 1;
                is_range = true;
            } else {
                s2 = corpus_parse_simple_stmt(p, CORPUS_RANGE_OK, &is_range);
            }
        }
        if (!is_range && p->tok == CT_SEMICOLON) {
            bool unused;
            corpus_next(p);
            s2 = NULL;
            if (p->tok != CT_SEMICOLON)
                s2 = corpus_parse_simple_stmt(p, CORPUS_BASIC, &unused);
            corpus_expect_semi(p);
            if (p->tok != CT_LBRACE)
                corpus_parse_simple_stmt(p, CORPUS_BASIC, &unused);
        }
        p->expr_lev = prev_lev;
    }

    CorpusNode *body = corpus_parse_block_stmt(p);
    corpus_expect_semi(p);

    if (is_range) {
        if (s2->nargs > 2)
            corpus_error_expected(p, s2->args[s2->nargs - 1]->pos,
                                  "at most 2 expressions");
        return corpus_stmt(p, CN_STMT_OTHER, pos, body->end);
    }

    corpus_make_expr(p, s2, "boolean or range expression");
    return corpus_stmt(p, CN_STMT_OTHER, pos, body->end);
}

static CorpusNode *corpus_parse_decl(CorpusParser *p);

static CorpusNode *corpus_parse_stmt_nested(CorpusParser *p) {
    CorpusNode *s;
    switch ((int)p->tok) {
    case CT_CONST:
    case CT_TYPE:
    case CT_VAR:
        return corpus_parse_decl(p);
    /* Tokens that may start an expression: operands, composite types and
     * unary operators. */
    case CT_IDENT:
    case CT_INT:
    case CT_FLOAT:
    case CT_IMAG:
    case CT_CHAR:
    case CT_STRING:
    case CT_FUNC:
    case CT_LPAREN:
    case CT_LBRACK:
    case CT_STRUCT:
    case CT_MAP:
    case CT_CHAN:
    case CT_INTERFACE:
    case CT_ADD:
    case CT_SUB:
    case CT_MUL:
    case CT_AND:
    case CT_XOR:
    case CT_ARROW:
    case CT_NOT: {
        bool is_range;
        s = corpus_parse_simple_stmt(p, CORPUS_LABEL_OK, &is_range);
        /* Labeled statements are parsed by parseSimpleStmt because of the
         * lookahead, and take no semicolon after them. */
        if (s->kind != CN_STMT_LABELED)
            corpus_expect_semi(p);
        return s;
    }
    case CT_GO:
    case CT_DEFER:
        return corpus_parse_go_or_defer_stmt(p, p->tok);
    case CT_RETURN:
        return corpus_parse_return_stmt(p);
    case CT_BREAK:
    case CT_CONTINUE:
    case CT_GOTO:
    case CT_FALLTHROUGH:
        return corpus_parse_branch_stmt(p, p->tok);
    case CT_LBRACE:
        s = corpus_parse_block_stmt(p);
        corpus_expect_semi(p);
        return s;
    case CT_IF:
        return corpus_parse_if_stmt(p);
    case CT_SWITCH:
        return corpus_parse_switch_stmt(p);
    case CT_SELECT:
        return corpus_parse_select_stmt(p);
    case CT_FOR:
        return corpus_parse_for_stmt(p);
    case CT_SEMICOLON: {
        /* An EmptyStmt, which ends at the semicolon when it was implicit. */
        bool implicit = str_eq(p->lit, BURROW_S("\n"));
        s = corpus_stmt(p, CN_STMT_OTHER, p->pos, implicit ? p->pos : p->pos + 1);
        corpus_next(p);
        return s;
    }
    case CT_RBRACE:
        /* A semicolon may be left out before a closing brace. */
        return corpus_stmt(p, CN_STMT_OTHER, p->pos, p->pos);
    default:
        break;
    }

    Int pos = p->pos;
    corpus_error_expected(p, pos, "statement");
    corpus_advance(p, corpus_stmt_start);
    return corpus_stmt(p, CN_STMT_OTHER, pos, p->pos);
}

static CorpusNode *corpus_parse_stmt(CorpusParser *p) {
    corpus_inc_nest_lev(p);
    CorpusNode *s = corpus_parse_stmt_nested(p);
    corpus_dec_nest_lev(p);
    return s;
}

/* ------------------------------------------------------------ declarations */

/* parseValueSpec, which answers the spec's End. */
static Int corpus_parse_value_spec(CorpusParser *p, CorpusTok keyword) {
    CorpusNode *last_name = corpus_parse_ident(p);
    while (p->tok == CT_COMMA) {
        corpus_next(p);
        last_name = corpus_parse_ident(p);
    }
    CorpusNode *typ = NULL;
    CorpusList values = {0};
    if (keyword == CT_CONST) {
        /* Always permit an optional type and initialization, for more
         * tolerant parsing. */
        if (p->tok != CT_EOF && p->tok != CT_SEMICOLON && p->tok != CT_RPAREN) {
            typ = corpus_try_ident_or_type(p);
            if (p->tok == CT_ASSIGN) {
                corpus_next(p);
                values = corpus_parse_list(p, true);
            }
        }
    } else {
        if (p->tok != CT_ASSIGN)
            typ = corpus_parse_type(p);
        if (p->tok == CT_ASSIGN) {
            corpus_next(p);
            values = corpus_parse_list(p, true);
        }
    }
    corpus_expect_semi(p);

    if (values.len > 0)
        return values.v[values.len - 1]->end;
    if (typ != NULL)
        return typ->end;
    return last_name->end;
}

static bool corpus_is_type_elem(const CorpusNode *x) {
    switch ((int)x->kind) {
    case CN_ARRAY_TYPE:
    case CN_STRUCT_TYPE:
    case CN_FUNC_TYPE:
    case CN_INTERFACE_TYPE:
    case CN_MAP_TYPE:
    case CN_CHAN_TYPE:
        return true;
    case CN_BINARY:
        return corpus_is_type_elem(x->x) || corpus_is_type_elem(x->y);
    case CN_UNARY:
        return x->tok == CT_TILDE;
    case CN_PAREN:
        return corpus_is_type_elem(x->x);
    default:
        break;
    }
    return false;
}

/* extractName. The name is the answer and *expr the rest, and a NULL answer
 * means x does not split. */
static CorpusNode *corpus_extract_name(CorpusParser *p, CorpusNode *x, bool force,
                                       CorpusNode **expr) {
    *expr = x;
    switch ((int)x->kind) {
    case CN_IDENT:
        *expr = NULL;
        return x;
    case CN_BINARY:
        if (x->tok == CT_MUL) {
            if (x->x->kind == CN_IDENT && (force || corpus_is_type_elem(x->y))) {
                /* x = name *x.Y */
                *expr = corpus_star(p, x->op_pos, x->y);
                return x->x;
            }
        } else if (x->tok == CT_OR) {
            CorpusNode *lhs;
            CorpusNode *name =
                corpus_extract_name(p, x->x, force || corpus_is_type_elem(x->y), &lhs);
            if (name != NULL && lhs != NULL) {
                /* x = name lhs|x.Y */
                *expr = corpus_binary(p, lhs, x->op_pos, x->tok, x->y);
                return name;
            }
            *expr = x;
        }
        break;
    case CN_CALL:
        if (x->x->kind == CN_IDENT && x->nargs == 1 && x->ellipsis == 0 &&
            (force || corpus_is_type_elem(x->args[0]))) {
            /* x = name (x.Args[0]) */
            *expr = corpus_paren(p, x->lparen, x->args[0], x->rparen);
            return x->x;
        }
        break;
    default:
        break;
    }
    return NULL;
}

/* parseGenericType, which answers the spec's type. */
static CorpusNode *corpus_parse_generic_type(CorpusParser *p, CorpusNode *name0,
                                             CorpusNode *typ0) {
    corpus_parse_parameter_list(p, name0, typ0, CT_RBRACK, false);
    corpus_expect(p, CT_RBRACK);
    if (p->tok == CT_ASSIGN)
        corpus_next(p); /* a type alias */
    return corpus_parse_type(p);
}

/* parseTypeSpec, which answers the spec's End. */
static Int corpus_parse_type_spec(CorpusParser *p) {
    corpus_parse_ident(p);
    CorpusNode *typ;

    if (p->tok == CT_LBRACK) {
        /* An array or slice type, or a type parameter list. */
        Int lbrack = p->pos;
        corpus_next(p);
        if (p->tok == CT_IDENT) {
            /* Either way there is an expression x, which may be just a name.
             * If the name is followed by "[" it is the start of an array or
             * slice constraint, and only otherwise does a full expression
             * need parsing. */
            CorpusNode *x = corpus_parse_ident(p);
            if (p->tok != CT_LBRACK) {
                p->expr_lev++;
                CorpusNode *lhs = corpus_parse_primary_expr(p, x);
                x = corpus_parse_binary_expr(p, lhs, CORPUS_LOWEST_PREC + 1);
                p->expr_lev--;
            }
            /* A name, possibly followed by a type, starts a type parameter
             * list, except that a single name followed by "]" is an array
             * length, and a type that could be an ordinary expression counts
             * as one only when a comma follows it. */
            CorpusNode *ptype;
            CorpusNode *pname = corpus_extract_name(p, x, p->tok == CT_COMMA, &ptype);
            if (pname != NULL && (ptype != NULL || p->tok != CT_RBRACK))
                typ = corpus_parse_generic_type(p, pname, ptype);
            else
                typ = corpus_parse_array_type(p, lbrack, x);
        } else {
            typ = corpus_parse_array_type(p, lbrack, NULL);
        }
    } else {
        if (p->tok == CT_ASSIGN)
            corpus_next(p); /* a type alias */
        typ = corpus_parse_type(p);
    }

    corpus_expect_semi(p);
    return typ->end;
}

/* parseDecl and parseGenDecl, as a DeclStmt. A statement only gets here on
 * const, type or var, so imports and functions never do. */
static CorpusNode *corpus_parse_decl(CorpusParser *p) {
    CorpusTok keyword = p->tok;
    Int pos = corpus_expect(p, keyword);
    Int end;
    if (p->tok == CT_LPAREN) {
        corpus_next(p);
        while (p->tok != CT_RPAREN && p->tok != CT_EOF) {
            if (keyword == CT_TYPE)
                corpus_parse_type_spec(p);
            else
                corpus_parse_value_spec(p, keyword);
        }
        end = corpus_expect(p, CT_RPAREN) + 1;
        corpus_expect_semi(p);
    } else if (keyword == CT_TYPE) {
        end = corpus_parse_type_spec(p);
    } else {
        end = corpus_parse_value_spec(p, keyword);
    }
    return corpus_stmt(p, CN_STMT_OTHER, pos, end);
}

/* ------------------------------------------------------------ ParseExprFrom */

/* The stack the parse runs on. A hundred thousand levels of nesting is
 * several frames each, and on a 64 bit machine the address space for all of
 * it costs nothing until it is touched. */
#define CORPUS_STACK (SIZE_MAX > UINT32_MAX ? (size_t)64 << 20 : (size_t)4 << 20)
/* What is left over for the frames below the check and for the allocator. */
#define CORPUS_STACK_MARGIN ((size_t)64 << 10)

typedef struct CorpusParseJob {
    CorpusFile *file;
    Str line;
    size_t budget;
    CorpusNode *expr;
    Chan *done;
} CorpusParseJob;

static void corpus_parse_run(CorpusParseJob *job) {
    CorpusParser p;
    memset(&p, 0, sizeof p);
    p.file = job->file;
    p.a = job->file->a;
    p.stack_base = corpus_stack_here();
    p.stack_budget = job->budget;

    burrow__testing_corpus_scan_init(&p.scanner, p.file, job->line);
    corpus_next(&p);

    job->expr = corpus_parse_rhs(&p);

    /* If a semicolon was inserted, consume it, and report an error if there
     * are more tokens. */
    if (p.tok == CT_SEMICOLON && str_eq(p.lit, BURROW_S("\n")))
        corpus_next(&p);
    corpus_expect(&p, CT_EOF);
}

static void corpus_parse_go(void *env) {
    CorpusParseJob *job = env;
    corpus_parse_run(job);
    bool done = true;
    chan_send(job->done, &done);
}

/* ErrorList.Less. */
static bool corpus_error_less(const CorpusError *e, const CorpusError *f) {
    int c = str_cmp(e->pos.filename, f->pos.filename);
    if (c != 0)
        return c < 0;
    if (e->pos.line != f->pos.line)
        return e->pos.line < f->pos.line;
    if (e->pos.column != f->pos.column)
        return e->pos.column < f->pos.column;
    return str_cmp(e->msg, f->msg) < 0;
}

/* ErrorList.Sort, as a merge sort, since a line of illegal characters can
 * have a great many errors. Errors that compare equal are the same text, so
 * which order they end up in makes no difference. */
static void corpus_sort_errors(Alloc *a, CorpusError *v, Int n) {
    CorpusError *tmp = burrow__testing_corpus_alloc(a, (size_t)n * sizeof *tmp);
    for (Int width = 1; width < n; width *= 2) {
        for (Int lo = 0; lo < n; lo += 2 * width) {
            Int mid = lo + width < n ? lo + width : n;
            Int hi = lo + 2 * width < n ? lo + 2 * width : n;
            Int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = corpus_error_less(&v[j], &v[i]) ? v[j++] : v[i++];
            while (i < mid)
                tmp[k++] = v[i++];
            while (j < hi)
                tmp[k++] = v[j++];
        }
        memcpy(v, tmp, (size_t)n * sizeof *v);
    }
}

/* Error.Error, with Position.String in it. */
static Str corpus_error_string(Alloc *a, const CorpusError *e) {
    const CorpusPosition *pos = &e->pos;
    if (pos->line <= 0)
        return pos->filename.len == 0
                   ? e->msg
                   : fmt_sprintf_v(a, "%s: %s", pos->filename, e->msg);
    const char *sep = pos->filename.len > 0 ? ":" : "";
    if (pos->column != 0)
        return fmt_sprintf_v(a, "%s%s%d:%d: %s", pos->filename, sep, pos->line,
                             pos->column, e->msg);
    return fmt_sprintf_v(a, "%s%s%d: %s", pos->filename, sep, pos->line, e->msg);
}

CorpusNode *burrow__testing_corpus_parse_expr(Alloc *a, Str line, Str *err) {
    CorpusFile *file = burrow__testing_corpus_alloc(a, sizeof *file);
    file->a = a;
    file->name = BURROW_S("(test)");
    file->size = line.len;

    CorpusParseJob job = {
        .file = file, .line = line, .budget = CORPUS_STACK - CORPUS_STACK_MARGIN};
    job.done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (job.done != NULL &&
        go_stack(BURROW_FN(Func, corpus_parse_go, &job), CORPUS_STACK)) {
        bool done;
        chan_recv(job.done, &done);
    } else {
        /* No goroutine, so the parse gets the caller's stack, and whatever
         * the caller is using it for leaves it less room. */
        job.budget = CORPUS_STACK_MARGIN;
        corpus_parse_run(&job);
    }
    if (job.done != NULL)
        chan_free(job.done);

    if (file->nerrs == 0) {
        *err = (Str){0};
        return job.expr;
    }
    corpus_sort_errors(a, file->errs, file->nerrs);
    Str first = corpus_error_string(a, &file->errs[0]);
    *err = file->nerrs == 1
               ? first
               : fmt_sprintf_v(a, "%s (and %d more errors)", first, file->nerrs - 1);
    return NULL;
}
