/* go/parser: a parser for Go source files.
 *
 * Derived from Go's src/go/parser/parser.go and interface.go.
 * Go source: go1.27.1.
 *
 * The parser is a recursive descent one, a function per production, as in Go.
 * Go gives up on a file with panic(bailout{}) and recovers it in ParseFile.
 * Here the parser notes that it is bailing out in its state and panics, and
 * the guarded function at the top catches it, the way regexp/syntax does. A
 * failed allocation bails out the same way.
 *
 * Go's "defer un(trace(p, name))" becomes a wrapper per traced production,
 * made by GP_TRACED, that prints the trace around the body. When the parser
 * bails out the catch prints the closing lines the defers would have.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/parser.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/go/build/constraint.h"
#include "burrow/go/scanner.h"
#include "burrow/io.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/stack.h"
#include "burrow/strings.h"
#include "burrow/thread.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* maxNestLev, how deep expressions and statements may nest. */
enum { GP_MAX_NEST_LEV = 100000 };

typedef struct GpParser {
    Alloc *a;
    TokenFile *file;
    GoScannerErrorList errors; /* in error_allocator(), so the Error can keep it */
    GoScanner scanner;

    /* Tracing and issue tracking */
    ParserMode mode;
    bool trace; /* mode & PARSER_TRACE */
    Int indent; /* indentation used for tracing output */

    /* Comments */
    Slice comments;                /* of AstCommentGroup * */
    AstCommentGroup *lead_comment; /* last lead comment */
    AstCommentGroup *line_comment; /* last line comment */
    bool top;                      /* in top of file (before package clause) */
    Str go_version;                /* minimum Go version found in //go:build comment */

    /* Next token */
    TokenPos pos; /* token position */
    Token tok;    /* one token look-ahead */
    Str lit;      /* token literal */

    /* Error recovery */
    TokenPos sync_pos; /* last synchronization position */
    Int sync_cnt;      /* number of gp_advance calls without progress */

    /* Non-syntactic parser control */
    Int expr_lev; /* < 0: in control clause, >= 0: in expression */
    bool in_rhs;  /* if set, the parser is parsing a rhs expression */

    Slice imports; /* of AstImportSpec * */

    /* nest_lev is used to track and limit the recursion depth during
     * parsing. */
    Int nest_lev;
    /* Where the stack runs short, or 0, 1 or 2 as gp_stack_low says. */
    uintptr_t stack_floor;

    /* Go's bailout: set before the panic that unwinds to the top. */
    bool bailing;
    bool oom;
    TokenPos bail_pos;
    Str bail_msg;
} GpParser;

static const Str gp_unwind = BURROW_S_INIT("go/parser: bailout");

BURROW_NORETURN static void gp_bail(GpParser *p) {
    p->bailing = true;
    panic_str(gp_unwind);
}

BURROW_NORETURN static void gp_oom(GpParser *p) {
    p->oom = true;
    gp_bail(p);
}

/* The parser's hooks for the resolver in parser_resolver.c. */
BURROW_NORETURN void burrow__parser_bail(void *p, TokenPos pos, Str msg);
BURROW_NORETURN void burrow__parser_oom(void *p);
void burrow__parser_error(void *p, TokenPos pos, Str msg);
void burrow__parser_resolve_file(void *p, Alloc *a, AstFile *file, TokenFile *handle,
                                 bool decl_errors);

BURROW_NORETURN void burrow__parser_bail(void *p, TokenPos pos, Str msg) {
    GpParser *gp = (GpParser *)p;
    gp->bail_pos = pos;
    gp->bail_msg = msg;
    gp_bail(gp);
}

BURROW_NORETURN void burrow__parser_oom(void *p) {
    gp_oom((GpParser *)p);
}

/* A node of the given kind, zeroed, or a bailout. */
static void *gp_node(GpParser *p, AstKind kind) {
    AstNode n = ast_node_new(p->a, kind);
    if (n == NULL)
        gp_oom(p);
    return n;
}

#define GP_NEW(p, T, kind) ((T *)gp_node((p), (kind)))

/* append(*s, *v) for one element, or a bailout. */
static void gp_append(GpParser *p, Slice *s, const void *v) {
    Slice next = slice_append(p->a, *s, v, 1);
    if (next.len != s->len + 1)
        gp_oom(p);
    *s = next;
}

static Str gp_tok_string(GpParser *p, Token tok) {
    return token_string(tok, p->a);
}

static TokenPos gp_expr_pos(AstExpr x) {
    return ast_expr_pos(x);
}

static bool gp_is(AstNode n, AstKind kind) {
    return n != NULL && n->kind == (Int)kind;
}

/* ----------------------------------------------------------------- tracing */

static void gp_print_trace(GpParser *p, Str msg) {
    static const char dots[] =
        ". . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . ";
    const Int n = (Int)sizeof dots - 1;
    TokenPosition pos = token_file_position(p->file, p->pos);
    Int i = 2 * p->indent;
    Str all = str_from_bytes(dots, n);
    fmt_printf_v("%5d:%3d: ", pos.line, pos.column);
    for (; i > n; i -= n)
        fmt_print_v(all);
    fmt_print_v(str_from_bytes(dots, i));
    fmt_println_v(msg);
}

static void gp_trace_in(GpParser *p, Str msg) {
    gp_print_trace(p, fmt_sprintf_v(p->a, "%s (", msg));
    p->indent++;
}

static void gp_trace_out(GpParser *p) {
    p->indent--;
    gp_print_trace(p, BURROW_S(")"));
}

/* A traced production: fn prints the trace around fn_body, which is the
 * production itself. */
#define GP_TRACED(T, fn, label, params, args)                                          \
    static T fn##_body params;                                                         \
    static T fn params {                                                               \
        if (p->trace)                                                                  \
            gp_trace_in(p, BURROW_S(label));                                           \
        T r_ = fn##_body args;                                                         \
        if (p->trace)                                                                  \
            gp_trace_out(p);                                                           \
        return r_;                                                                     \
    }

static void gp_error(GpParser *p, TokenPos pos, Str msg);

/* ------------------------------------------------------------ stack room
 *
 * Go's stack grows to fit any nesting, and maxNestLev is what stops the parser.
 * A C stack does not grow, and a goroutine's is 256 KB unless it asked for
 * more, so each level also asks whether there is room for the next one, the
 * check encoding/gob makes. Running short ends the parse with Go's "exceeded
 * max nesting depth" and not with a crash. On a goroutine the stack's bounds
 * are one read away. On a thread of the system's own they cost a system call
 * on some systems, so they are only looked up once the nesting is deep. */

enum { GP_THREAD_CHECK_AFTER = 32 };

/* The cap on nesting on wasip1, where calls run on the engine's own stack,
 * which nothing inside the module can measure. */
enum { GP_WASI_MAX_NEST_LEV = 1000 };

#define GP_STACK_MARGIN ((uintptr_t)64 << 10)

static uintptr_t gp_stack_here(void) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(BURROW_OS_WASI)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

/* Whether the stack is too low to go depth levels deep. p->stack_floor is 0
 * before the first look, 1 when the bounds cannot be had and the count is all
 * there is, and 2 on a thread whose bounds are left until they matter. */
static bool gp_stack_low(GpParser *p, Int depth) {
#if defined(BURROW_OS_WASI)
    if (depth >= GP_WASI_MAX_NEST_LEV)
        return true;
#endif
    if (p->stack_floor == 0) {
        burrow__Stack *s = burrow__stack_current();
        if (s != NULL && s->lo != NULL)
            p->stack_floor = (uintptr_t)s->lo + GP_STACK_MARGIN;
        else
            p->stack_floor = 2;
    }
    if (p->stack_floor == 2) {
        if (depth < GP_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        if (!burrow__thread_stack_limit(&lo))
            lo = NULL;
        p->stack_floor = lo == NULL ? 1 : (uintptr_t)lo + GP_STACK_MARGIN;
    }
    if (p->stack_floor == 1)
        return false;
    return gp_stack_here() < p->stack_floor;
}

/* The same for the resolver, which walks the tree the parser made and stops
 * the same way when the stack runs short. */
bool burrow__parser_stack_low(void *p, Int depth);
bool burrow__parser_stack_low(void *p, Int depth) {
    return gp_stack_low((GpParser *)p, depth);
}

static void gp_inc_nest_lev(GpParser *p) {
    p->nest_lev++;
    if (p->nest_lev > GP_MAX_NEST_LEV || gp_stack_low(p, p->nest_lev)) {
        gp_error(p, p->pos, BURROW_S("exceeded max nesting depth"));
        gp_bail(p);
    }
}

/* GP_TRACED for the productions that also count the nesting depth, Go's
 * "defer decNestLev(incNestLev(p))". */
#define GP_TRACED_NEST(T, fn, label, params, args)                                     \
    static T fn##_body params;                                                         \
    static T fn params {                                                               \
        gp_inc_nest_lev(p);                                                            \
        if (p->trace)                                                                  \
            gp_trace_in(p, BURROW_S(label));                                           \
        T r_ = fn##_body args;                                                         \
        if (p->trace)                                                                  \
            gp_trace_out(p);                                                           \
        p->nest_lev--;                                                                 \
        return r_;                                                                     \
    }

/* ------------------------------------------------------------ the scanner */

static void gp_scan_error(void *env, TokenPosition pos, Str msg) {
    GpParser *p = (GpParser *)env;
    go_scanner_error_list_add(&p->errors, error_allocator(), pos, msg);
}

static void gp_next(GpParser *p);

static void gp_init(GpParser *p, TokenFile *file, Slice src, ParserMode mode) {
    p->file = file;
    go_scanner_init(&p->scanner, p->a, file, src,
                    BURROW_FN(GoScannerErrorHandler, gp_scan_error, p),
                    GO_SCANNER_SCAN_COMMENTS);
    p->top = true;
    p->mode = mode;
    p->trace =
        (mode & PARSER_TRACE) != 0; /* for convenience (p->trace is used frequently) */
    gp_next(p);
}

static TokenPos gp_end(GpParser *p) {
    return go_scanner_end(&p->scanner);
}

/* Advance to the next token. */
static void gp_next0(GpParser *p) {
    /* Because of one-token look-ahead, print the previous token when tracing
     * as it provides a more readable output. The very first token
     * (!p->pos.IsValid()) is not initialized (it is TOKEN_ILLEGAL), so don't
     * print it. */
    if (p->trace && token_pos_is_valid(p->pos)) {
        Str s = gp_tok_string(p, p->tok);
        if (token_is_literal(p->tok))
            gp_print_trace(p, fmt_sprintf_v(p->a, "%s %s", s, p->lit));
        else if (token_is_operator(p->tok) || token_is_keyword(p->tok))
            gp_print_trace(p, fmt_sprintf_v(p->a, "\"%s\"", s));
        else
            gp_print_trace(p, s);
    }

    for (;;) {
        p->pos = go_scanner_scan(&p->scanner, &p->tok, &p->lit);
        if (p->tok == TOKEN_COMMENT) {
            if (p->top && strings_has_prefix(p->lit, BURROW_S("//go:build"))) {
                Error err = BURROW_NO_ERROR;
                ConstraintExpr x = constraint_parse(p->a, p->lit, &err);
                if (BURROW_OK(err))
                    p->go_version = constraint_go_version(p->a, x);
            }
            if ((p->mode & PARSER_PARSE_COMMENTS) == 0)
                continue;
        } else {
            /* Found a non-comment; top of file is over. */
            p->top = false;
        }
        break;
    }
}

/* lineFor returns the line of pos, ignoring line directive adjustments. */
static Int gp_line_for(GpParser *p, TokenPos pos) {
    return token_file_position_for(p->file, pos, false).line;
}

/* Consume a comment and return it and the line on which it ends. */
static AstComment *gp_consume_comment(GpParser *p, Int *endline) {
    /* /-style comments may end on a different line than where they start.
     * Scan the comment for '\n' chars and adjust endline accordingly. */
    *endline = gp_line_for(p, p->pos);
    if (p->lit.len > 1 && p->lit.p[1] == '*') {
        /* don't use range here - no need to decode Unicode code points */
        for (Int i = 0; i < p->lit.len; i++) {
            if (p->lit.p[i] == '\n')
                (*endline)++;
        }
    }

    AstComment *comment = GP_NEW(p, AstComment, AST_KIND_COMMENT);
    comment->slash = p->pos;
    comment->text = p->lit;
    gp_next0(p);
    return comment;
}

/* Consume a group of adjacent comments, add it to the parser's comments list,
 * and return it together with the line at which the last comment in the
 * group ends. A non-comment token or n empty lines terminate a comment
 * group. */
static AstCommentGroup *gp_consume_comment_group(GpParser *p, Int n, Int *endline) {
    Slice list = slice_nil(TYPE_AST_COMMENT_PTR);
    *endline = gp_line_for(p, p->pos);
    while (p->tok == TOKEN_COMMENT && gp_line_for(p, p->pos) <= *endline + n) {
        AstComment *comment = gp_consume_comment(p, endline);
        gp_append(p, &list, &comment);
    }

    /* add comment group to the comments list */
    AstCommentGroup *comments = GP_NEW(p, AstCommentGroup, AST_KIND_COMMENT_GROUP);
    comments->list = list;
    gp_append(p, &p->comments, &comments);
    return comments;
}

/* Advance to the next non-comment token. In the process, collect any comment
 * groups encountered, and remember the last lead and line comments.
 *
 * A lead comment is a comment group that starts and ends in a line without
 * any other tokens and that is followed by a non-comment token on the line
 * immediately after the comment group.
 *
 * A line comment is a comment group that follows a non-comment token on the
 * same line, and that has no tokens after it on the line where it ends.
 *
 * Lead and line comments may be considered documentation that is stored in
 * the AST. */
static void gp_next(GpParser *p) {
    p->lead_comment = NULL;
    p->line_comment = NULL;
    TokenPos prev = p->pos;
    gp_next0(p);

    if (p->tok == TOKEN_COMMENT) {
        AstCommentGroup *comment = NULL;
        Int endline = 0;

        if (gp_line_for(p, p->pos) == gp_line_for(p, prev)) {
            /* The comment is on same line as the previous token; it cannot be
             * a lead comment but may be a line comment. */
            comment = gp_consume_comment_group(p, 0, &endline);
            if (gp_line_for(p, p->pos) != endline || p->tok == TOKEN_SEMICOLON ||
                p->tok == TOKEN_EOF) {
                /* The next token is on a different line, thus the last comment
                 * group is a line comment. */
                p->line_comment = comment;
            }
        }

        /* consume successor comments, if any */
        endline = -1;
        while (p->tok == TOKEN_COMMENT)
            comment = gp_consume_comment_group(p, 1, &endline);

        if (endline + 1 == gp_line_for(p, p->pos)) {
            /* The next token is following on the line immediately after the
             * comment group, thus the last comment group is a lead comment. */
            p->lead_comment = comment;
        }
    }
}

/* ------------------------------------------------------------------ errors */

static void gp_error(GpParser *p, TokenPos pos, Str msg) {
    if (p->trace)
        gp_trace_in(p, fmt_sprintf_v(p->a, "error: %s", msg));

    TokenPosition epos = token_file_position(p->file, pos);

    /* If AllErrors is not set, discard errors reported on the same line as
     * the last recorded error and stop parsing if there are more than 10
     * errors. */
    if ((p->mode & PARSER_ALL_ERRORS) == 0) {
        Int n = go_scanner_error_list_len(p->errors);
        if (n > 0 &&
            go_scanner_error_list_at(p->errors, n - 1)->pos.line == epos.line) {
            if (p->trace)
                gp_trace_out(p);
            return; /* discard - likely a spurious error */
        }
        if (n > 10)
            gp_bail(p);
    }

    go_scanner_error_list_add(&p->errors, error_allocator(), epos, msg);
    if (p->trace)
        gp_trace_out(p);
}

void burrow__parser_error(void *p, TokenPos pos, Str msg) {
    gp_error((GpParser *)p, pos, msg);
}

static void gp_error_expected(GpParser *p, TokenPos pos, Str what) {
    Str msg = fmt_sprintf_v(p->a, "expected %s", what);
    if (pos == p->pos) {
        /* the error happened at the current position; make the error message
         * more specific */
        if (p->tok == TOKEN_SEMICOLON && str_eq(p->lit, BURROW_S("\n")))
            msg = fmt_sprintf_v(p->a, "%s, found newline", msg);
        else if (token_is_literal(p->tok))
            /* print 123 rather than 'INT', etc. */
            msg = fmt_sprintf_v(p->a, "%s, found %s", msg, p->lit);
        else
            msg = fmt_sprintf_v(p->a, "%s, found '%s'", msg, gp_tok_string(p, p->tok));
    }
    gp_error(p, pos, msg);
}

static Str gp_quote_tok(GpParser *p, Token tok) {
    return fmt_sprintf_v(p->a, "'%s'", gp_tok_string(p, tok));
}

static TokenPos gp_expect(GpParser *p, Token tok) {
    TokenPos pos = p->pos;
    if (p->tok != tok)
        gp_error_expected(p, pos, gp_quote_tok(p, tok));
    gp_next(p); /* make progress */
    return pos;
}

/* expect2 is like expect, but it returns an invalid position if the expected
 * token is not found. */
static TokenPos gp_expect2(GpParser *p, Token tok) {
    TokenPos pos = TOKEN_NO_POS;
    if (p->tok == tok)
        pos = p->pos;
    else
        gp_error_expected(p, p->pos, gp_quote_tok(p, tok));
    gp_next(p); /* make progress */
    return pos;
}

/* expectClosing is like expect but provides a better error message for the
 * common case of a missing comma before a newline. */
static TokenPos gp_expect_closing(GpParser *p, Token tok, Str context) {
    if (p->tok != tok && p->tok == TOKEN_SEMICOLON && str_eq(p->lit, BURROW_S("\n"))) {
        gp_error(p, p->pos,
                 fmt_sprintf_v(p->a, "missing ',' before newline in %s", context));
        gp_next(p);
    }
    return gp_expect(p, tok);
}

static void gp_advance(GpParser *p, bool (*to)(Token));
static bool gp_stmt_start(Token tok);

/* expectSemi consumes a semicolon and returns the applicable line comment. */
static AstCommentGroup *gp_expect_semi(GpParser *p) {
    AstCommentGroup *comment = NULL;
    switch (p->tok) {
    case TOKEN_RPAREN:
    case TOKEN_RBRACE:
        return NULL; /* semicolon is optional before a closing ')' or '}' */
    case TOKEN_COMMA:
        /* permit a ',' instead of a ';' but complain */
        gp_error_expected(p, p->pos, BURROW_S("';'"));
        /* fallthrough */
    case TOKEN_SEMICOLON:
        if (str_eq(p->lit, BURROW_S(";"))) {
            /* explicit semicolon */
            gp_next(p);
            comment = p->line_comment; /* use following comments */
        } else {
            /* artificial semicolon */
            comment = p->line_comment; /* use preceding comments */
            gp_next(p);
        }
        return comment;
    default:
        gp_error_expected(p, p->pos, BURROW_S("';'"));
        gp_advance(p, gp_stmt_start);
        return NULL;
    }
}

static bool gp_at_comma(GpParser *p, Str context, Token follow) {
    if (p->tok == TOKEN_COMMA)
        return true;
    if (p->tok != follow) {
        Str msg = BURROW_S("missing ','");
        if (p->tok == TOKEN_SEMICOLON && str_eq(p->lit, BURROW_S("\n")))
            msg = BURROW_S("missing ',' before newline");
        gp_error(p, p->pos, fmt_sprintf_v(p->a, "%s in %s", msg, context));
        return true; /* "insert" comma and continue */
    }
    return false;
}

static void gp_assert(bool cond, const char *msg) {
    if (!cond) {
        Str m = str_from_bytes(msg, (Int)strlen(msg));
        panic_str(fmt_sprintf_v(error_allocator(), "go/parser internal error: %s", m));
    }
}

/* advance consumes tokens until the current token p->tok is in the 'to' set,
 * or TOKEN_EOF. For error recovery. */
static void gp_advance(GpParser *p, bool (*to)(Token)) {
    for (; p->tok != TOKEN_EOF; gp_next(p)) {
        if (to(p->tok)) {
            /* Return only if parser made some progress since last sync or if
             * it has not reached 10 advance calls without progress. Otherwise
             * consume at least one token to avoid an endless parser loop (it
             * is possible that both parseOperand and parseStmt call advance
             * and correctly do not advance, thus the need for the
             * invocation limit p->sync_cnt). */
            if (p->pos == p->sync_pos && p->sync_cnt < 10) {
                p->sync_cnt++;
                return;
            }
            if (p->pos > p->sync_pos) {
                p->sync_pos = p->pos;
                p->sync_cnt = 0;
                return;
            }
            /* Reaching here indicates a parser bug, likely an incorrect token
             * list in this function, but it only leads to skipping of
             * possibly correct code if a previous error is present, and thus
             * is preferred over a non-terminating parse. */
        }
    }
}

static bool gp_stmt_start(Token tok) {
    switch (tok) {
    case TOKEN_BREAK:
    case TOKEN_CONST:
    case TOKEN_CONTINUE:
    case TOKEN_DEFER:
    case TOKEN_FALLTHROUGH:
    case TOKEN_FOR:
    case TOKEN_GO:
    case TOKEN_GOTO:
    case TOKEN_IF:
    case TOKEN_RETURN:
    case TOKEN_SELECT:
    case TOKEN_SWITCH:
    case TOKEN_TYPE_:
    case TOKEN_VAR:
        return true;
    default:
        return false;
    }
}

static bool gp_decl_start(Token tok) {
    switch (tok) {
    case TOKEN_IMPORT:
    case TOKEN_CONST:
    case TOKEN_TYPE_:
    case TOKEN_VAR:
        return true;
    default:
        return false;
    }
}

static bool gp_expr_end(Token tok) {
    switch (tok) {
    case TOKEN_COMMA:
    case TOKEN_COLON:
    case TOKEN_SEMICOLON:
    case TOKEN_RPAREN:
    case TOKEN_RBRACK:
    case TOKEN_RBRACE:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------- identifiers */

static AstIdent *gp_new_ident(GpParser *p, TokenPos pos, Str name) {
    AstIdent *id = GP_NEW(p, AstIdent, AST_KIND_IDENT);
    id->name_pos = pos;
    id->name = name;
    return id;
}

static AstIdent *gp_parse_ident(GpParser *p) {
    TokenPos pos = p->pos;
    Str name = BURROW_S("_");
    if (p->tok == TOKEN_IDENT) {
        name = p->lit;
        gp_next(p);
    } else {
        gp_expect(p, TOKEN_IDENT); /* use expect() error handling */
    }
    return gp_new_ident(p, pos, name);
}

GP_TRACED(Slice, gp_parse_ident_list, "IdentList", (GpParser * p), (p))
static Slice gp_parse_ident_list_body(GpParser *p) {
    Slice list = slice_nil(TYPE_AST_IDENT_PTR);
    AstIdent *id = gp_parse_ident(p);
    gp_append(p, &list, &id);
    while (p->tok == TOKEN_COMMA) {
        gp_next(p);
        id = gp_parse_ident(p);
        gp_append(p, &list, &id);
    }
    return list;
}

/* ------------------------------------------------------------- common */

static AstExpr gp_parse_expr(GpParser *p);
static AstExpr gp_parse_rhs(GpParser *p);

GP_TRACED(Slice, gp_parse_expr_list, "ExpressionList", (GpParser * p), (p))
static Slice gp_parse_expr_list_body(GpParser *p) {
    Slice list = slice_nil(TYPE_AST_EXPR);
    AstExpr x = gp_parse_expr(p);
    gp_append(p, &list, &x);
    while (p->tok == TOKEN_COMMA) {
        gp_next(p);
        x = gp_parse_expr(p);
        gp_append(p, &list, &x);
    }
    return list;
}

static Slice gp_parse_list(GpParser *p, bool in_rhs) {
    bool old = p->in_rhs;
    p->in_rhs = in_rhs;
    Slice list = gp_parse_expr_list(p);
    p->in_rhs = old;
    return list;
}

/* ------------------------------------------------------------------ types */

static AstExpr gp_try_ident_or_type(GpParser *p);
static AstExpr gp_parse_type_instance(GpParser *p, AstExpr typ);
static AstExpr gp_embedded_elem(GpParser *p, AstExpr x);
static AstFuncType *gp_parse_func_type(GpParser *p);

static AstExpr gp_bad_expr(GpParser *p, TokenPos from, TokenPos to) {
    AstBadExpr *x = GP_NEW(p, AstBadExpr, AST_KIND_BAD_EXPR);
    x->from = from;
    x->to = to;
    return &x->node;
}

GP_TRACED(AstExpr, gp_parse_type, "Type", (GpParser * p), (p))
static AstExpr gp_parse_type_body(GpParser *p) {
    AstExpr typ = gp_try_ident_or_type(p);
    if (typ == NULL) {
        TokenPos pos = p->pos;
        gp_error_expected(p, pos, BURROW_S("type"));
        gp_advance(p, gp_expr_end);
        return gp_bad_expr(p, pos, p->pos);
    }
    return typ;
}

static AstExpr gp_parse_type_name(GpParser *p, AstIdent *ident);

GP_TRACED(AstExpr, gp_parse_qualified_ident, "QualifiedIdent",
          (GpParser * p, AstIdent *ident), (p, ident))
static AstExpr gp_parse_qualified_ident_body(GpParser *p, AstIdent *ident) {
    AstExpr typ = gp_parse_type_name(p, ident);
    if (p->tok == TOKEN_LBRACK)
        typ = gp_parse_type_instance(p, typ);
    return typ;
}

/* If the result is an identifier, it is not resolved. */
GP_TRACED(AstExpr, gp_parse_type_name, "TypeName", (GpParser * p, AstIdent *ident),
          (p, ident))
static AstExpr gp_parse_type_name_body(GpParser *p, AstIdent *ident) {
    if (ident == NULL)
        ident = gp_parse_ident(p);

    if (p->tok == TOKEN_PERIOD) {
        gp_next(p);
        AstIdent *sel = gp_parse_ident(p);
        AstSelectorExpr *x = GP_NEW(p, AstSelectorExpr, AST_KIND_SELECTOR_EXPR);
        x->x = &ident->node;
        x->sel = sel;
        return &x->node;
    }
    return &ident->node;
}

/* "[" has already been consumed, and lbrack is its position. If len != NULL
 * it is the already consumed array length. */
GP_TRACED(AstArrayType *, gp_parse_array_type, "ArrayType",
          (GpParser * p, TokenPos lbrack, AstExpr len), (p, lbrack, len))
static AstArrayType *gp_parse_array_type_body(GpParser *p, TokenPos lbrack,
                                              AstExpr len) {
    if (len == NULL) {
        p->expr_lev++;
        /* always permit ellipsis for more fault-tolerant parsing */
        if (p->tok == TOKEN_ELLIPSIS) {
            AstEllipsis *e = GP_NEW(p, AstEllipsis, AST_KIND_ELLIPSIS);
            e->ellipsis = p->pos;
            len = &e->node;
            gp_next(p);
        } else if (p->tok != TOKEN_RBRACK) {
            len = gp_parse_rhs(p);
        }
        p->expr_lev--;
    }
    if (p->tok == TOKEN_COMMA) {
        /* Trailing commas are accepted in type parameter lists but not in
         * array type declarations. Accept for better error handling but
         * complain. */
        gp_error(p, p->pos, BURROW_S("unexpected comma; expecting ]"));
        gp_next(p);
    }
    gp_expect(p, TOKEN_RBRACK);
    AstExpr elt = gp_parse_type(p);
    AstArrayType *t = GP_NEW(p, AstArrayType, AST_KIND_ARRAY_TYPE);
    t->lbrack = lbrack;
    t->len = len;
    t->elt = elt;
    return t;
}

/* packIndexExpr returns an IndexExpr x[expr0] or IndexListExpr
 * x[expr0, ...]. */
static AstExpr gp_pack_index_expr(GpParser *p, AstExpr x, TokenPos lbrack, Slice exprs,
                                  TokenPos rbrack) {
    gp_assert(exprs.len > 0, "packIndexExpr with empty expr slice");
    if (exprs.len == 1) {
        AstIndexExpr *ix = GP_NEW(p, AstIndexExpr, AST_KIND_INDEX_EXPR);
        ix->x = x;
        ix->lbrack = lbrack;
        ix->index = BURROW_AT(AstExpr, exprs, 0);
        ix->rbrack = rbrack;
        return &ix->node;
    }
    AstIndexListExpr *ix = GP_NEW(p, AstIndexListExpr, AST_KIND_INDEX_LIST_EXPR);
    ix->x = x;
    ix->lbrack = lbrack;
    ix->indices = exprs;
    ix->rbrack = rbrack;
    return &ix->node;
}

GP_TRACED(AstExpr, gp_parse_array_field_or_type_instance, "ArrayFieldOrTypeInstance",
          (GpParser * p, AstIdent *x, AstIdent **name), (p, x, name))
static AstExpr gp_parse_array_field_or_type_instance_body(GpParser *p, AstIdent *x,
                                                          AstIdent **name) {
    TokenPos lbrack = gp_expect(p, TOKEN_LBRACK);
    /* if valid, the position of a trailing comma preceding the ']' */
    TokenPos trailing_comma = TOKEN_NO_POS;
    Slice args = slice_nil(TYPE_AST_EXPR);
    if (p->tok != TOKEN_RBRACK) {
        p->expr_lev++;
        AstExpr arg = gp_parse_rhs(p);
        gp_append(p, &args, &arg);
        while (p->tok == TOKEN_COMMA) {
            TokenPos comma = p->pos;
            gp_next(p);
            if (p->tok == TOKEN_RBRACK) {
                trailing_comma = comma;
                break;
            }
            arg = gp_parse_rhs(p);
            gp_append(p, &args, &arg);
        }
        p->expr_lev--;
    }
    TokenPos rbrack = gp_expect(p, TOKEN_RBRACK);

    if (args.len == 0) {
        /* x []E */
        AstExpr elt = gp_parse_type(p);
        AstArrayType *t = GP_NEW(p, AstArrayType, AST_KIND_ARRAY_TYPE);
        t->lbrack = lbrack;
        t->elt = elt;
        *name = x;
        return &t->node;
    }

    /* x [P]E or x[P] */
    if (args.len == 1) {
        AstExpr elt = gp_try_ident_or_type(p);
        if (elt != NULL) {
            /* x [P]E */
            if (token_pos_is_valid(trailing_comma)) {
                /* Trailing commas are invalid in array type fields. */
                gp_error(p, trailing_comma, BURROW_S("unexpected comma; expecting ]"));
            }
            AstArrayType *t = GP_NEW(p, AstArrayType, AST_KIND_ARRAY_TYPE);
            t->lbrack = lbrack;
            t->len = BURROW_AT(AstExpr, args, 0);
            t->elt = elt;
            *name = x;
            return &t->node;
        }
    }

    /* x[P], x[P1, P2], ... */
    *name = NULL;
    return gp_pack_index_expr(p, &x->node, lbrack, args, rbrack);
}

static AstStarExpr *gp_new_star(GpParser *p, TokenPos star, AstExpr x) {
    AstStarExpr *s = GP_NEW(p, AstStarExpr, AST_KIND_STAR_EXPR);
    s->star = star;
    s->x = x;
    return s;
}

GP_TRACED(AstField *, gp_parse_field_decl, "FieldDecl", (GpParser * p), (p))
static AstField *gp_parse_field_decl_body(GpParser *p) {
    AstCommentGroup *doc = p->lead_comment;

    Slice names = slice_nil(TYPE_AST_IDENT_PTR);
    AstExpr typ = NULL;
    switch (p->tok) {
    case TOKEN_IDENT: {
        AstIdent *name = gp_parse_ident(p);
        if (p->tok == TOKEN_PERIOD || p->tok == TOKEN_STRING ||
            p->tok == TOKEN_SEMICOLON || p->tok == TOKEN_RBRACE) {
            /* embedded type */
            typ = &name->node;
            if (p->tok == TOKEN_PERIOD)
                typ = gp_parse_qualified_ident(p, name);
        } else {
            /* name1, name2, ... T */
            gp_append(p, &names, &name);
            while (p->tok == TOKEN_COMMA) {
                gp_next(p);
                AstIdent *id = gp_parse_ident(p);
                gp_append(p, &names, &id);
            }
            /* Careful dance: We don't know if we have an embedded instantiated
             * type T[P1, P2, ...] or a field T of array type []E or [P]E. */
            if (names.len == 1 && p->tok == TOKEN_LBRACK) {
                typ = gp_parse_array_field_or_type_instance(p, name, &name);
                if (name == NULL)
                    names = slice_nil(TYPE_AST_IDENT_PTR);
            } else {
                /* T P */
                typ = gp_parse_type(p);
            }
        }
        break;
    }
    case TOKEN_MUL: {
        TokenPos star = p->pos;
        gp_next(p);
        if (p->tok == TOKEN_LPAREN) {
            /* *(T) */
            gp_error(p, p->pos, BURROW_S("cannot parenthesize embedded type"));
            gp_next(p);
            typ = gp_parse_qualified_ident(p, NULL);
            /* expect closing ')' but no need to complain if missing */
            if (p->tok == TOKEN_RPAREN)
                gp_next(p);
        } else {
            /* *T */
            typ = gp_parse_qualified_ident(p, NULL);
        }
        typ = &gp_new_star(p, star, typ)->node;
        break;
    }
    case TOKEN_LPAREN:
        gp_error(p, p->pos, BURROW_S("cannot parenthesize embedded type"));
        gp_next(p);
        if (p->tok == TOKEN_MUL) {
            /* (*T) */
            TokenPos star = p->pos;
            gp_next(p);
            typ = &gp_new_star(p, star, gp_parse_qualified_ident(p, NULL))->node;
        } else {
            /* (T) */
            typ = gp_parse_qualified_ident(p, NULL);
        }
        /* expect closing ')' but no need to complain if missing */
        if (p->tok == TOKEN_RPAREN)
            gp_next(p);
        break;
    default: {
        TokenPos pos = p->pos;
        gp_error_expected(p, pos, BURROW_S("field name or embedded type"));
        gp_advance(p, gp_expr_end);
        typ = gp_bad_expr(p, pos, p->pos);
        break;
    }
    }

    AstBasicLit *tag = NULL;
    if (p->tok == TOKEN_STRING) {
        tag = GP_NEW(p, AstBasicLit, AST_KIND_BASIC_LIT);
        tag->value_pos = p->pos;
        tag->value_end = gp_end(p);
        tag->kind = p->tok;
        tag->value = p->lit;
        gp_next(p);
    }

    AstCommentGroup *comment = gp_expect_semi(p);

    AstField *field = GP_NEW(p, AstField, AST_KIND_FIELD);
    field->doc = doc;
    field->names = names;
    field->type = typ;
    field->tag = tag;
    field->comment = comment;
    return field;
}

static AstFieldList *gp_new_field_list(GpParser *p, TokenPos opening, Slice list,
                                       TokenPos closing) {
    AstFieldList *l = GP_NEW(p, AstFieldList, AST_KIND_FIELD_LIST);
    l->opening = opening;
    l->list = list;
    l->closing = closing;
    return l;
}

GP_TRACED(AstStructType *, gp_parse_struct_type, "StructType", (GpParser * p), (p))
static AstStructType *gp_parse_struct_type_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_STRUCT);
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice list = slice_nil(TYPE_AST_FIELD_PTR);
    while (p->tok == TOKEN_IDENT || p->tok == TOKEN_MUL || p->tok == TOKEN_LPAREN) {
        /* a field declaration cannot start with a '(' but we accept it here
         * for more robust parsing and better error messages (parseFieldDecl
         * will check and complain if necessary) */
        AstField *f = gp_parse_field_decl(p);
        gp_append(p, &list, &f);
    }
    TokenPos rbrace = gp_expect(p, TOKEN_RBRACE);

    AstStructType *t = GP_NEW(p, AstStructType, AST_KIND_STRUCT_TYPE);
    t->struct_ = pos;
    t->fields = gp_new_field_list(p, lbrace, list, rbrace);
    return t;
}

GP_TRACED(AstStarExpr *, gp_parse_pointer_type, "PointerType", (GpParser * p), (p))
static AstStarExpr *gp_parse_pointer_type_body(GpParser *p) {
    TokenPos star = gp_expect(p, TOKEN_MUL);
    AstExpr base = gp_parse_type(p);
    return gp_new_star(p, star, base);
}

GP_TRACED(AstEllipsis *, gp_parse_dots_type, "DotsType", (GpParser * p), (p))
static AstEllipsis *gp_parse_dots_type_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_ELLIPSIS);
    AstExpr elt = gp_parse_type(p);
    AstEllipsis *e = GP_NEW(p, AstEllipsis, AST_KIND_ELLIPSIS);
    e->ellipsis = pos;
    e->elt = elt;
    return e;
}

/* Go's field, a parameter as it is parsed: a name, a type or both. */
typedef struct GpParam {
    AstIdent *name;
    AstExpr typ;
} GpParam;

GP_TRACED(GpParam, gp_parse_param_decl, "ParamDecl",
          (GpParser * p, AstIdent *name, bool type_sets_ok), (p, name, type_sets_ok))
static GpParam gp_parse_param_decl_body(GpParser *p, AstIdent *name,
                                        bool type_sets_ok) {
    GpParam f = {NULL, NULL};

    /* TODO(rFindley) refactor to be more similar to paramDeclOrNil in the
     * syntax package */
    Token ptok = p->tok;
    if (name != NULL) {
        p->tok = TOKEN_IDENT; /* force TOKEN_IDENT case in switch below */
    } else if (type_sets_ok && p->tok == TOKEN_TILDE) {
        /* "~" ... */
        f.typ = gp_embedded_elem(p, NULL);
        return f;
    }

    switch (p->tok) {
    case TOKEN_IDENT:
        /* name */
        if (name != NULL) {
            f.name = name;
            p->tok = ptok;
        } else {
            f.name = gp_parse_ident(p);
        }
        switch (p->tok) {
        case TOKEN_IDENT:
        case TOKEN_MUL:
        case TOKEN_ARROW:
        case TOKEN_FUNC:
        case TOKEN_CHAN:
        case TOKEN_MAP:
        case TOKEN_STRUCT:
        case TOKEN_INTERFACE:
        case TOKEN_LPAREN:
            /* name type */
            f.typ = gp_parse_type(p);
            break;
        case TOKEN_LBRACK:
            /* name "[" type1, ..., typeN "]" or name "[" n "]" type */
            f.typ = gp_parse_array_field_or_type_instance(p, f.name, &f.name);
            break;
        case TOKEN_ELLIPSIS:
            /* name "..." type */
            f.typ = &gp_parse_dots_type(p)->node;
            return f; /* don't allow ...type "|" ... */
        case TOKEN_PERIOD:
            /* name "." ... */
            f.typ = gp_parse_qualified_ident(p, f.name);
            f.name = NULL;
            break;
        case TOKEN_TILDE:
            if (type_sets_ok) {
                f.typ = gp_embedded_elem(p, NULL);
                return f;
            }
            break;
        case TOKEN_OR:
            if (type_sets_ok) {
                f.typ = gp_embedded_elem(p, &f.name->node);
                f.name = NULL;
                return f;
            }
            break;
        default:
            break;
        }
        break;
    case TOKEN_MUL:
    case TOKEN_ARROW:
    case TOKEN_FUNC:
    case TOKEN_LBRACK:
    case TOKEN_CHAN:
    case TOKEN_MAP:
    case TOKEN_STRUCT:
    case TOKEN_INTERFACE:
    case TOKEN_LPAREN:
        /* type */
        f.typ = gp_parse_type(p);
        break;
    case TOKEN_ELLIPSIS:
        /* "..." type (always accepted) */
        f.typ = &gp_parse_dots_type(p)->node;
        return f; /* don't allow ...type "|" ... */
    default:
        /* TODO(rfindley): this is incorrect in the case of type parameter
         * lists (should be "']'" in that case) */
        gp_error_expected(p, p->pos, BURROW_S("')'"));
        gp_advance(p, gp_expr_end);
        break;
    }

    /* [name] type "|" */
    if (type_sets_ok && p->tok == TOKEN_OR && f.typ != NULL)
        f.typ = gp_embedded_elem(p, f.typ);

    return f;
}

static AstField *gp_new_field(GpParser *p, Slice names, AstExpr typ) {
    AstField *f = GP_NEW(p, AstField, AST_KIND_FIELD);
    f->names = names;
    f->type = typ;
    return f;
}

GP_TRACED(Slice, gp_parse_parameter_list, "ParameterList",
          (GpParser * p, AstIdent *name0, AstExpr typ0, Token closing, bool dddok),
          (p, name0, typ0, closing, dddok))
static Slice gp_parse_parameter_list_body(GpParser *p, AstIdent *name0, AstExpr typ0,
                                          Token closing, bool dddok) {
    Slice params = slice_nil(TYPE_AST_FIELD_PTR);

    /* Type parameters are the only parameter list closed by ']'. */
    bool tparams = closing == TOKEN_RBRACK;

    TokenPos pos0 = p->pos;
    if (name0 != NULL)
        pos0 = ast_ident_pos(name0);
    else if (typ0 != NULL)
        pos0 = gp_expr_pos(typ0);

    /* Note: The code below matches the corresponding code in the syntax
     * parser closely. Changes must be reflected in either parser. For the
     * code to match, we use the local function gp_parse_param_decl in place
     * of the parser's paramDeclOrNil. Go's list of fields is kept here as
     * two parallel slices, the names and the types. */
    Slice names = slice_nil(TYPE_AST_IDENT_PTR);
    Slice types = slice_nil(TYPE_AST_EXPR);
    Int named = 0; /* number of parameters that have an explicit name and type */
    Int typed = 0; /* number of parameters that have an explicit type */

    while (name0 != NULL || (p->tok != closing && p->tok != TOKEN_EOF)) {
        GpParam par;
        if (typ0 != NULL) {
            if (tparams)
                typ0 = gp_embedded_elem(p, typ0);
            par.name = name0;
            par.typ = typ0;
        } else {
            par = gp_parse_param_decl(p, name0, tparams);
        }
        name0 = NULL; /* 1st name was consumed if present */
        typ0 = NULL;  /* 1st typ was consumed if present */
        if (par.name != NULL || par.typ != NULL) {
            gp_append(p, &names, &par.name);
            gp_append(p, &types, &par.typ);
            if (par.name != NULL && par.typ != NULL)
                named++;
            if (par.typ != NULL)
                typed++;
        }
        if (!gp_at_comma(p, BURROW_S("parameter list"), closing))
            break;
        gp_next(p);
    }

    Int n = names.len;
    if (n == 0)
        return params; /* not uncommon */

    /* distribute parameter types (len(list) > 0) */
    if (named == 0) {
        /* all unnamed => found names are type names */
        for (Int i = 0; i < n; i++) {
            AstIdent *typ = BURROW_AT(AstIdent *, names, i);
            if (typ != NULL) {
                BURROW_AT(AstExpr, types, i) = &typ->node;
                BURROW_AT(AstIdent *, names, i) = NULL;
            }
        }
        if (tparams) {
            /* This is the same error handling as below, adjusted for type
             * parameters only. See comment below for details. (go.dev/issue/64534) */
            TokenPos err_pos;
            Str msg;
            if (named == typed /* same as typed == 0 */) {
                err_pos = p->pos; /* position error at closing ] */
                msg = BURROW_S("missing type constraint");
            } else {
                err_pos = pos0; /* position at opening [ or first name */
                msg = BURROW_S("missing type parameter name");
                if (n == 1)
                    msg =
                        BURROW_S("missing type parameter name or invalid array length");
            }
            gp_error(p, err_pos, msg);
        }
    } else if (named != n) {
        /* some named or we're in a type parameter list => all must be
         * named */
        TokenPos err_pos = TOKEN_NO_POS; /* left-most error position (or invalid) */
        AstExpr typ = NULL;              /* current type (from right to left) */
        for (Int i = 0; i < n; i++) {
            Int j = n - i - 1;
            AstExpr par_typ = BURROW_AT(AstExpr, types, j);
            if (par_typ != NULL) {
                typ = par_typ;
                if (BURROW_AT(AstIdent *, names, j) == NULL) {
                    err_pos = gp_expr_pos(typ);
                    AstIdent *nm = gp_new_ident(p, err_pos, BURROW_S("_"));
                    BURROW_AT(AstIdent *, names, j) = nm; /* correct position */
                }
            } else if (typ != NULL) {
                BURROW_AT(AstExpr, types, j) = typ;
            } else {
                /* par.typ == nil && typ == nil => we only have a par.name */
                err_pos = ast_ident_pos(BURROW_AT(AstIdent *, names, j));
                BURROW_AT(AstExpr, types, j) = gp_bad_expr(p, err_pos, p->pos);
            }
        }
        if (token_pos_is_valid(err_pos)) {
            /* Not all parameters are named because named != len(list). If
             * named == typed, there must be parameters that have no types.
             * They must be at the end of the parameter list, otherwise types
             * would have been filled in by the right-to-left sweep above and
             * there would be no error. If tparams is set, the parameter list
             * is a type parameter list. */
            Str msg;
            if (named == typed) {
                err_pos = p->pos; /* position error at closing token ) or ] */
                if (tparams)
                    msg = BURROW_S("missing type constraint");
                else
                    msg = BURROW_S("missing parameter type");
            } else {
                if (tparams) {
                    msg = BURROW_S("missing type parameter name");
                    /* go.dev/issue/60812 */
                    if (n == 1)
                        msg = BURROW_S("missing type parameter name or invalid array "
                                       "length");
                } else {
                    msg = BURROW_S("missing parameter name");
                }
            }
            gp_error(p, err_pos, msg);
        }
    }

    /* check use of ... */
    bool first = true; /* only report first occurrence */
    for (Int i = 0; i < n; i++) {
        AstExpr ft = BURROW_AT(AstExpr, types, i);
        if (gp_is(ft, AST_KIND_ELLIPSIS) && (!dddok || i + 1 < n)) {
            if (first) {
                first = false;
                TokenPos at = ((AstEllipsis *)ft)->ellipsis;
                if (dddok)
                    gp_error(p, at, BURROW_S("can only use ... with final parameter"));
                else
                    gp_error(p, at, BURROW_S("invalid use of ..."));
            }
            /* use T instead of invalid ...T
             * TODO(gri) would like to use `f.typ = t.Elt` but that causes
             * problems with the resolver in cases of reuse of the same
             * identifier */
            BURROW_AT(AstExpr, types, i) =
                gp_bad_expr(p, gp_expr_pos(ft), ast_expr_end(ft));
        }
    }

    /* Convert list to []*ast.Field. If list contains types only, each type
     * gets its own ast.Field. */
    if (named == 0) {
        /* parameter list consists of types only */
        for (Int i = 0; i < n; i++) {
            AstExpr typ = BURROW_AT(AstExpr, types, i);
            gp_assert(typ != NULL, "nil type in unnamed parameter list");
            AstField *f = gp_new_field(p, slice_nil(TYPE_AST_IDENT_PTR), typ);
            gp_append(p, &params, &f);
        }
        return params;
    }

    /* If the parameter list consists of named parameters with types, collect
     * all names with the same types into a single ast.Field. */
    Slice group = slice_nil(TYPE_AST_IDENT_PTR);
    AstExpr typ = NULL;
    for (Int i = 0; i < n; i++) {
        AstExpr par_typ = BURROW_AT(AstExpr, types, i);
        if (par_typ != typ) {
            if (group.len > 0) {
                gp_assert(typ != NULL, "nil type in named parameter list");
                AstField *f = gp_new_field(p, group, typ);
                gp_append(p, &params, &f);
                group = slice_nil(TYPE_AST_IDENT_PTR);
            }
            typ = par_typ;
        }
        AstIdent *nm = BURROW_AT(AstIdent *, names, i);
        gp_append(p, &group, &nm);
    }
    if (group.len > 0) {
        gp_assert(typ != NULL, "nil type in named parameter list");
        AstField *f = gp_new_field(p, group, typ);
        gp_append(p, &params, &f);
    }
    return params;
}

GP_TRACED(AstFieldList *, gp_parse_type_parameters, "TypeParameters", (GpParser * p),
          (p))
static AstFieldList *gp_parse_type_parameters_body(GpParser *p) {
    TokenPos lbrack = gp_expect(p, TOKEN_LBRACK);
    Slice list = slice_nil(TYPE_AST_FIELD_PTR);
    if (p->tok != TOKEN_RBRACK)
        list = gp_parse_parameter_list(p, NULL, NULL, TOKEN_RBRACK, false);
    TokenPos rbrack = gp_expect(p, TOKEN_RBRACK);

    if (list.len == 0) {
        gp_error(p, rbrack, BURROW_S("empty type parameter list"));
        return NULL; /* avoid follow-on errors */
    }

    return gp_new_field_list(p, lbrack, list, rbrack);
}

GP_TRACED(AstFieldList *, gp_parse_parameters, "Parameters",
          (GpParser * p, bool result), (p, result))
static AstFieldList *gp_parse_parameters_body(GpParser *p, bool result) {
    if (!result || p->tok == TOKEN_LPAREN) {
        TokenPos lparen = gp_expect(p, TOKEN_LPAREN);
        Slice list = slice_nil(TYPE_AST_FIELD_PTR);
        if (p->tok != TOKEN_RPAREN)
            list = gp_parse_parameter_list(p, NULL, NULL, TOKEN_RPAREN, !result);
        TokenPos rparen = gp_expect(p, TOKEN_RPAREN);
        return gp_new_field_list(p, lparen, list, rparen);
    }

    AstExpr typ = gp_try_ident_or_type(p);
    if (typ != NULL) {
        Slice list = slice_nil(TYPE_AST_FIELD_PTR);
        AstField *f = gp_new_field(p, slice_nil(TYPE_AST_IDENT_PTR), typ);
        gp_append(p, &list, &f);
        return gp_new_field_list(p, TOKEN_NO_POS, list, TOKEN_NO_POS);
    }

    return NULL;
}

static AstFuncType *gp_new_func_type(GpParser *p, TokenPos func, AstFieldList *params,
                                     AstFieldList *results) {
    AstFuncType *t = GP_NEW(p, AstFuncType, AST_KIND_FUNC_TYPE);
    t->func = func;
    t->params = params;
    t->results = results;
    return t;
}

GP_TRACED(AstFuncType *, gp_parse_func_type, "FuncType", (GpParser * p), (p))
static AstFuncType *gp_parse_func_type_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_FUNC);
    /* accept type parameters for more tolerant parsing but complain */
    if (p->tok == TOKEN_LBRACK) {
        AstFieldList *tparams = gp_parse_type_parameters(p);
        if (tparams != NULL)
            gp_error(p, tparams->opening,
                     BURROW_S("function type must have no type parameters"));
    }
    AstFieldList *params = gp_parse_parameters(p, false);
    AstFieldList *results = gp_parse_parameters(p, true);

    return gp_new_func_type(p, pos, params, results);
}

GP_TRACED(AstField *, gp_parse_method_spec, "MethodSpec", (GpParser * p), (p))
static AstField *gp_parse_method_spec_body(GpParser *p) {
    AstCommentGroup *doc = p->lead_comment;
    Slice idents = slice_nil(TYPE_AST_IDENT_PTR);
    AstExpr typ = NULL;
    AstExpr x = gp_parse_type_name(p, NULL);
    if (gp_is(x, AST_KIND_IDENT)) {
        AstIdent *ident = (AstIdent *)x;
        if (p->tok == TOKEN_LBRACK) {
            /* generic method or embedded instantiated type */
            TokenPos lbrack = p->pos;
            gp_next(p);
            p->expr_lev++;
            AstExpr x2 = gp_parse_expr(p);
            p->expr_lev--;
            if (gp_is(x2, AST_KIND_IDENT) && p->tok != TOKEN_COMMA &&
                p->tok != TOKEN_RBRACK) {
                /* generic method m[T any]
                 *
                 * Interface methods do not have type parameters. We parse them
                 * for a better error message and improved error recovery. */
                (void)gp_parse_parameter_list(p, (AstIdent *)x2, NULL, TOKEN_RBRACK,
                                              false);
                (void)gp_expect(p, TOKEN_RBRACK);
                gp_error(p, lbrack,
                         BURROW_S("interface method must have no type parameters"));

                /* TODO(rfindley) refactor to share code with
                 * parseFuncType. */
                AstFieldList *params = gp_parse_parameters(p, false);
                AstFieldList *results = gp_parse_parameters(p, true);
                gp_append(p, &idents, &ident);
                typ = &gp_new_func_type(p, TOKEN_NO_POS, params, results)->node;
            } else {
                /* embedded instantiated type
                 * TODO(rfindley) should resolve all identifiers in x. */
                Slice list = slice_nil(TYPE_AST_EXPR);
                gp_append(p, &list, &x2);
                if (gp_at_comma(p, BURROW_S("type argument list"), TOKEN_RBRACK)) {
                    p->expr_lev++;
                    gp_next(p);
                    while (p->tok != TOKEN_RBRACK && p->tok != TOKEN_EOF) {
                        AstExpr t = gp_parse_type(p);
                        gp_append(p, &list, &t);
                        if (!gp_at_comma(p, BURROW_S("type argument list"),
                                         TOKEN_RBRACK))
                            break;
                        gp_next(p);
                    }
                    p->expr_lev--;
                }
                TokenPos rbrack =
                    gp_expect_closing(p, TOKEN_RBRACK, BURROW_S("type argument list"));
                typ = gp_pack_index_expr(p, &ident->node, lbrack, list, rbrack);
            }
        } else if (p->tok == TOKEN_LPAREN) {
            /* ordinary method
             * TODO(rfindley) refactor to share code with parseFuncType. */
            AstFieldList *params = gp_parse_parameters(p, false);
            AstFieldList *results = gp_parse_parameters(p, true);
            gp_append(p, &idents, &ident);
            typ = &gp_new_func_type(p, TOKEN_NO_POS, params, results)->node;
        } else {
            /* embedded type */
            typ = x;
        }
    } else {
        /* embedded, possibly instantiated type */
        typ = x;
        if (p->tok == TOKEN_LBRACK) {
            /* embedded instantiated interface */
            typ = gp_parse_type_instance(p, typ);
        }
    }

    /* Comment is added at the callsite: the field below may joined with
     * additional type specs using '|'.
     * TODO(rfindley) this should be refactored.
     * TODO(rfindley) add more tests for comment handling. */
    AstField *f = gp_new_field(p, idents, typ);
    f->doc = doc;
    return f;
}

static AstExpr gp_embedded_term(GpParser *p);

static AstBinaryExpr *gp_new_binary(GpParser *p, AstExpr x, TokenPos op_pos, Token op,
                                    AstExpr y) {
    AstBinaryExpr *b = GP_NEW(p, AstBinaryExpr, AST_KIND_BINARY_EXPR);
    b->x = x;
    b->op_pos = op_pos;
    b->op = op;
    b->y = y;
    return b;
}

static AstUnaryExpr *gp_new_unary(GpParser *p, TokenPos op_pos, Token op, AstExpr x) {
    AstUnaryExpr *u = GP_NEW(p, AstUnaryExpr, AST_KIND_UNARY_EXPR);
    u->op_pos = op_pos;
    u->op = op;
    u->x = x;
    return u;
}

GP_TRACED(AstExpr, gp_embedded_elem, "EmbeddedElem", (GpParser * p, AstExpr x), (p, x))
static AstExpr gp_embedded_elem_body(GpParser *p, AstExpr x) {
    if (x == NULL)
        x = gp_embedded_term(p);
    while (p->tok == TOKEN_OR) {
        TokenPos op_pos = p->pos;
        gp_next(p);
        AstBinaryExpr *t = gp_new_binary(p, x, op_pos, TOKEN_OR, NULL);
        t->y = gp_embedded_term(p);
        x = &t->node;
    }
    return x;
}

GP_TRACED(AstExpr, gp_embedded_term, "EmbeddedTerm", (GpParser * p), (p))
static AstExpr gp_embedded_term_body(GpParser *p) {
    if (p->tok == TOKEN_TILDE) {
        TokenPos op_pos = p->pos;
        gp_next(p);
        AstUnaryExpr *t = gp_new_unary(p, op_pos, TOKEN_TILDE, NULL);
        t->x = gp_parse_type(p);
        return &t->node;
    }

    AstExpr t = gp_try_ident_or_type(p);
    if (t == NULL) {
        TokenPos pos = p->pos;
        gp_error_expected(p, pos, BURROW_S("~ term or type"));
        gp_advance(p, gp_expr_end);
        return gp_bad_expr(p, pos, p->pos);
    }

    return t;
}

GP_TRACED(AstInterfaceType *, gp_parse_interface_type, "InterfaceType", (GpParser * p),
          (p))
static AstInterfaceType *gp_parse_interface_type_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_INTERFACE);
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);

    Slice list = slice_nil(TYPE_AST_FIELD_PTR);

    for (;;) {
        AstField *f;
        if (p->tok == TOKEN_IDENT) {
            f = gp_parse_method_spec(p);
            if (f->names.len == 0)
                f->type = gp_embedded_elem(p, f->type);
            f->comment = gp_expect_semi(p);
        } else if (p->tok == TOKEN_TILDE) {
            AstExpr typ = gp_embedded_elem(p, NULL);
            AstCommentGroup *comment = gp_expect_semi(p);
            f = gp_new_field(p, slice_nil(TYPE_AST_IDENT_PTR), typ);
            f->comment = comment;
        } else {
            AstExpr t = gp_try_ident_or_type(p);
            if (t == NULL)
                break;
            AstExpr typ = gp_embedded_elem(p, t);
            AstCommentGroup *comment = gp_expect_semi(p);
            f = gp_new_field(p, slice_nil(TYPE_AST_IDENT_PTR), typ);
            f->comment = comment;
        }
        gp_append(p, &list, &f);
    }

    /* TODO(rfindley): the error produced here could be improved, since we
     * could accept an identifier, 'type', or a '}' at this point. */
    TokenPos rbrace = gp_expect(p, TOKEN_RBRACE);

    AstInterfaceType *t = GP_NEW(p, AstInterfaceType, AST_KIND_INTERFACE_TYPE);
    t->interface_ = pos;
    t->methods = gp_new_field_list(p, lbrace, list, rbrace);
    return t;
}

GP_TRACED(AstMapType *, gp_parse_map_type, "MapType", (GpParser * p), (p))
static AstMapType *gp_parse_map_type_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_MAP);
    gp_expect(p, TOKEN_LBRACK);
    AstExpr key = gp_parse_type(p);
    gp_expect(p, TOKEN_RBRACK);
    AstExpr value = gp_parse_type(p);

    AstMapType *t = GP_NEW(p, AstMapType, AST_KIND_MAP_TYPE);
    t->map = pos;
    t->key = key;
    t->value = value;
    return t;
}

GP_TRACED(AstChanType *, gp_parse_chan_type, "ChanType", (GpParser * p), (p))
static AstChanType *gp_parse_chan_type_body(GpParser *p) {
    TokenPos pos = p->pos;
    AstChanDir dir = AST_SEND | AST_RECV;
    TokenPos arrow = TOKEN_NO_POS;
    if (p->tok == TOKEN_CHAN) {
        gp_next(p);
        if (p->tok == TOKEN_ARROW) {
            arrow = p->pos;
            gp_next(p);
            dir = AST_SEND;
        }
    } else {
        arrow = gp_expect(p, TOKEN_ARROW);
        gp_expect(p, TOKEN_CHAN);
        dir = AST_RECV;
    }
    AstExpr value = gp_parse_type(p);

    AstChanType *t = GP_NEW(p, AstChanType, AST_KIND_CHAN_TYPE);
    t->begin = pos;
    t->arrow = arrow;
    t->dir = dir;
    t->value = value;
    return t;
}

GP_TRACED(AstExpr, gp_parse_type_instance, "TypeInstance", (GpParser * p, AstExpr typ),
          (p, typ))
static AstExpr gp_parse_type_instance_body(GpParser *p, AstExpr typ) {
    TokenPos opening = gp_expect(p, TOKEN_LBRACK);
    p->expr_lev++;
    Slice list = slice_nil(TYPE_AST_EXPR);
    while (p->tok != TOKEN_RBRACK && p->tok != TOKEN_EOF) {
        AstExpr t = gp_parse_type(p);
        gp_append(p, &list, &t);
        if (!gp_at_comma(p, BURROW_S("type argument list"), TOKEN_RBRACK))
            break;
        gp_next(p);
    }
    p->expr_lev--;

    TokenPos closing =
        gp_expect_closing(p, TOKEN_RBRACK, BURROW_S("type argument list"));

    if (list.len == 0) {
        gp_error_expected(p, closing, BURROW_S("type argument list"));
        AstIndexExpr *ix = GP_NEW(p, AstIndexExpr, AST_KIND_INDEX_EXPR);
        ix->x = typ;
        ix->lbrack = opening;
        ix->index = gp_bad_expr(p, opening + 1, closing);
        ix->rbrack = closing;
        return &ix->node;
    }

    return gp_pack_index_expr(p, typ, opening, list, closing);
}

static AstExpr gp_paren(GpParser *p, TokenPos lparen, AstExpr x, TokenPos rparen) {
    AstParenExpr *e = GP_NEW(p, AstParenExpr, AST_KIND_PAREN_EXPR);
    e->lparen = lparen;
    e->x = x;
    e->rparen = rparen;
    return &e->node;
}

static AstExpr gp_try_ident_or_type_body(GpParser *p);

static AstExpr gp_try_ident_or_type(GpParser *p) {
    gp_inc_nest_lev(p);
    AstExpr x = gp_try_ident_or_type_body(p);
    p->nest_lev--;
    return x;
}

static AstExpr gp_try_ident_or_type_body(GpParser *p) {
    switch (p->tok) {
    case TOKEN_IDENT: {
        AstExpr typ = gp_parse_type_name(p, NULL);
        if (p->tok == TOKEN_LBRACK)
            typ = gp_parse_type_instance(p, typ);
        return typ;
    }
    case TOKEN_LBRACK: {
        TokenPos lbrack = gp_expect(p, TOKEN_LBRACK);
        return &gp_parse_array_type(p, lbrack, NULL)->node;
    }
    case TOKEN_STRUCT:
        return &gp_parse_struct_type(p)->node;
    case TOKEN_MUL:
        return &gp_parse_pointer_type(p)->node;
    case TOKEN_FUNC:
        return &gp_parse_func_type(p)->node;
    case TOKEN_INTERFACE:
        return &gp_parse_interface_type(p)->node;
    case TOKEN_MAP:
        return &gp_parse_map_type(p)->node;
    case TOKEN_CHAN:
    case TOKEN_ARROW:
        return &gp_parse_chan_type(p)->node;
    case TOKEN_LPAREN: {
        TokenPos lparen = p->pos;
        gp_next(p);
        AstExpr typ = gp_parse_type(p);
        TokenPos rparen = gp_expect(p, TOKEN_RPAREN);
        return gp_paren(p, lparen, typ, rparen);
    }
    default:
        break;
    }

    /* no type found */
    return NULL;
}

/* ------------------------------------------------------------------ blocks */

static AstStmt gp_parse_stmt(GpParser *p);

GP_TRACED(Slice, gp_parse_stmt_list, "StatementList", (GpParser * p), (p))
static Slice gp_parse_stmt_list_body(GpParser *p) {
    Slice list = slice_nil(TYPE_AST_STMT);
    while (p->tok != TOKEN_CASE && p->tok != TOKEN_DEFAULT && p->tok != TOKEN_RBRACE &&
           p->tok != TOKEN_EOF) {
        AstStmt s = gp_parse_stmt(p);
        gp_append(p, &list, &s);
    }
    return list;
}

static AstBlockStmt *gp_new_block(GpParser *p, TokenPos lbrace, Slice list,
                                  TokenPos rbrace) {
    AstBlockStmt *b = GP_NEW(p, AstBlockStmt, AST_KIND_BLOCK_STMT);
    b->lbrace = lbrace;
    b->list = list;
    b->rbrace = rbrace;
    return b;
}

GP_TRACED(AstBlockStmt *, gp_parse_body, "Body", (GpParser * p), (p))
static AstBlockStmt *gp_parse_body_body(GpParser *p) {
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice list = gp_parse_stmt_list(p);
    TokenPos rbrace = gp_expect2(p, TOKEN_RBRACE);
    return gp_new_block(p, lbrace, list, rbrace);
}

GP_TRACED(AstBlockStmt *, gp_parse_block_stmt, "BlockStmt", (GpParser * p), (p))
static AstBlockStmt *gp_parse_block_stmt_body(GpParser *p) {
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice list = gp_parse_stmt_list(p);
    TokenPos rbrace = gp_expect2(p, TOKEN_RBRACE);
    return gp_new_block(p, lbrace, list, rbrace);
}

/* ------------------------------------------------------------- expressions */

GP_TRACED(AstExpr, gp_parse_func_type_or_lit, "FuncTypeOrLit", (GpParser * p), (p))
static AstExpr gp_parse_func_type_or_lit_body(GpParser *p) {
    AstFuncType *typ = gp_parse_func_type(p);
    if (p->tok != TOKEN_LBRACE) {
        /* function type only */
        return &typ->node;
    }

    p->expr_lev++;
    AstBlockStmt *body = gp_parse_body(p);
    p->expr_lev--;

    AstFuncLit *f = GP_NEW(p, AstFuncLit, AST_KIND_FUNC_LIT);
    f->type = typ;
    f->body = body;
    return &f->node;
}

/* parseOperand may return an expression or a raw type (incl. array types of
 * the form [...]T). Callers must verify the result. */
GP_TRACED(AstExpr, gp_parse_operand, "Operand", (GpParser * p), (p))
static AstExpr gp_parse_operand_body(GpParser *p) {
    switch (p->tok) {
    case TOKEN_IDENT:
        return &gp_parse_ident(p)->node;
    case TOKEN_INT:
    case TOKEN_FLOAT:
    case TOKEN_IMAG:
    case TOKEN_CHAR:
    case TOKEN_STRING: {
        AstBasicLit *x = GP_NEW(p, AstBasicLit, AST_KIND_BASIC_LIT);
        x->value_pos = p->pos;
        x->value_end = gp_end(p);
        x->kind = p->tok;
        x->value = p->lit;
        gp_next(p);
        return &x->node;
    }
    case TOKEN_LPAREN: {
        TokenPos lparen = p->pos;
        gp_next(p);
        p->expr_lev++;
        AstExpr x = gp_parse_rhs(p); /* types may be parenthesized: (some type) */
        p->expr_lev--;
        TokenPos rparen = gp_expect(p, TOKEN_RPAREN);
        return gp_paren(p, lparen, x, rparen);
    }
    case TOKEN_FUNC:
        return gp_parse_func_type_or_lit(p);
    default:
        break;
    }

    AstExpr typ = gp_try_ident_or_type(p);
    if (typ != NULL) { /* do not consume trailing type parameters */
        /* could be type for composite literal or conversion */
        gp_assert(!gp_is(typ, AST_KIND_IDENT), "type cannot be identifier");
        return typ;
    }

    /* we have an error */
    TokenPos pos = p->pos;
    gp_error_expected(p, pos, BURROW_S("operand"));
    gp_advance(p, gp_stmt_start);
    return gp_bad_expr(p, pos, p->pos);
}

static AstSelectorExpr *gp_new_selector(GpParser *p, AstExpr x, AstIdent *sel) {
    AstSelectorExpr *s = GP_NEW(p, AstSelectorExpr, AST_KIND_SELECTOR_EXPR);
    s->x = x;
    s->sel = sel;
    return s;
}

GP_TRACED(AstExpr, gp_parse_selector, "Selector", (GpParser * p, AstExpr x), (p, x))
static AstExpr gp_parse_selector_body(GpParser *p, AstExpr x) {
    AstIdent *sel = gp_parse_ident(p);
    return &gp_new_selector(p, x, sel)->node;
}

GP_TRACED(AstExpr, gp_parse_type_assertion, "TypeAssertion", (GpParser * p, AstExpr x),
          (p, x))
static AstExpr gp_parse_type_assertion_body(GpParser *p, AstExpr x) {
    TokenPos lparen = gp_expect(p, TOKEN_LPAREN);
    AstExpr typ = NULL;
    if (p->tok == TOKEN_TYPE_) {
        /* type switch: typ == nil */
        gp_next(p);
    } else {
        typ = gp_parse_type(p);
    }
    TokenPos rparen = gp_expect(p, TOKEN_RPAREN);

    AstTypeAssertExpr *t = GP_NEW(p, AstTypeAssertExpr, AST_KIND_TYPE_ASSERT_EXPR);
    t->x = x;
    t->type = typ;
    t->lparen = lparen;
    t->rparen = rparen;
    return &t->node;
}

GP_TRACED(AstExpr, gp_parse_index_or_slice_or_instance, "parseIndexOrSliceOrInstance",
          (GpParser * p, AstExpr x), (p, x))
static AstExpr gp_parse_index_or_slice_or_instance_body(GpParser *p, AstExpr x) {
    TokenPos lbrack = gp_expect(p, TOKEN_LBRACK);
    if (p->tok == TOKEN_RBRACK) {
        /* empty index, slice or index expressions are not permitted;
         * accept them for parsing tolerance, but complain */
        gp_error_expected(p, p->pos, BURROW_S("operand"));
        TokenPos rbrack = p->pos;
        gp_next(p);
        AstIndexExpr *ix = GP_NEW(p, AstIndexExpr, AST_KIND_INDEX_EXPR);
        ix->x = x;
        ix->lbrack = lbrack;
        ix->index = gp_bad_expr(p, rbrack, rbrack);
        ix->rbrack = rbrack;
        return &ix->node;
    }
    p->expr_lev++;

    enum { N = 3 }; /* change the 3 to 2 to disable 3-index slices */
    Slice args = slice_nil(TYPE_AST_EXPR);
    AstExpr index[N] = {NULL, NULL, NULL};
    TokenPos colons[N - 1] = {TOKEN_NO_POS, TOKEN_NO_POS};
    if (p->tok != TOKEN_COLON) {
        /* We can't know if we have an index expression or a type instance;
         * accept any expression or type. */
        index[0] = gp_parse_rhs(p);
    }
    Int ncolons = 0;
    switch (p->tok) {
    case TOKEN_COLON:
        /* slice expression */
        while (p->tok == TOKEN_COLON && ncolons < N - 1) {
            colons[ncolons] = p->pos;
            ncolons++;
            gp_next(p);
            if (p->tok != TOKEN_COLON && p->tok != TOKEN_RBRACK && p->tok != TOKEN_EOF)
                index[ncolons] = gp_parse_rhs(p);
        }
        break;
    case TOKEN_COMMA:
        /* instance expression */
        gp_append(p, &args, &index[0]);
        while (p->tok == TOKEN_COMMA) {
            gp_next(p);
            if (p->tok != TOKEN_RBRACK && p->tok != TOKEN_EOF) {
                AstExpr t = gp_parse_type(p);
                gp_append(p, &args, &t);
            }
        }
        break;
    default:
        break;
    }

    p->expr_lev--;
    TokenPos rbrack = gp_expect(p, TOKEN_RBRACK);

    if (ncolons > 0) {
        /* slice expression */
        bool slice3 = false;
        if (ncolons == 2) {
            slice3 = true;
            /* Check presence of middle and final index here rather than
             * during type-checking to prevent erroneous programs from passing
             * through gofmt (was go.dev/issue/7305). */
            if (index[1] == NULL) {
                gp_error(p, colons[0],
                         BURROW_S("middle index required in 3-index slice"));
                index[1] = gp_bad_expr(p, colons[0] + 1, colons[1]);
            }
            if (index[2] == NULL) {
                gp_error(p, colons[1],
                         BURROW_S("final index required in 3-index slice"));
                index[2] = gp_bad_expr(p, colons[1] + 1, rbrack);
            }
        }
        AstSliceExpr *s = GP_NEW(p, AstSliceExpr, AST_KIND_SLICE_EXPR);
        s->x = x;
        s->lbrack = lbrack;
        s->low = index[0];
        s->high = index[1];
        s->max = index[2];
        s->slice3 = slice3;
        s->rbrack = rbrack;
        return &s->node;
    }

    if (args.len == 0) {
        /* index expression */
        AstIndexExpr *ix = GP_NEW(p, AstIndexExpr, AST_KIND_INDEX_EXPR);
        ix->x = x;
        ix->lbrack = lbrack;
        ix->index = index[0];
        ix->rbrack = rbrack;
        return &ix->node;
    }

    /* instance expression */
    return gp_pack_index_expr(p, x, lbrack, args, rbrack);
}

GP_TRACED(AstCallExpr *, gp_parse_call_or_conversion, "CallOrConversion",
          (GpParser * p, AstExpr fun), (p, fun))
static AstCallExpr *gp_parse_call_or_conversion_body(GpParser *p, AstExpr fun) {
    TokenPos lparen = gp_expect(p, TOKEN_LPAREN);
    p->expr_lev++;
    Slice list = slice_nil(TYPE_AST_EXPR);
    TokenPos ellipsis = TOKEN_NO_POS;
    while (p->tok != TOKEN_RPAREN && p->tok != TOKEN_EOF &&
           !token_pos_is_valid(ellipsis)) {
        /* builtins may expect a type: make(some type, ...) */
        AstExpr arg = gp_parse_rhs(p);
        gp_append(p, &list, &arg);
        if (p->tok == TOKEN_ELLIPSIS) {
            ellipsis = p->pos;
            gp_next(p);
        }
        if (!gp_at_comma(p, BURROW_S("argument list"), TOKEN_RPAREN))
            break;
        gp_next(p);
    }
    p->expr_lev--;
    TokenPos rparen = gp_expect_closing(p, TOKEN_RPAREN, BURROW_S("argument list"));

    AstCallExpr *c = GP_NEW(p, AstCallExpr, AST_KIND_CALL_EXPR);
    c->fun = fun;
    c->lparen = lparen;
    c->args = list;
    c->ellipsis = ellipsis;
    c->rparen = rparen;
    return c;
}

static AstExpr gp_parse_literal_value(GpParser *p, AstExpr typ);

GP_TRACED(AstExpr, gp_parse_value, "Element", (GpParser * p), (p))
static AstExpr gp_parse_value_body(GpParser *p) {
    if (p->tok == TOKEN_LBRACE)
        return gp_parse_literal_value(p, NULL);

    return gp_parse_expr(p);
}

GP_TRACED(AstExpr, gp_parse_element, "Element", (GpParser * p), (p))
static AstExpr gp_parse_element_body(GpParser *p) {
    AstExpr x = gp_parse_value(p);
    if (p->tok == TOKEN_COLON) {
        TokenPos colon = p->pos;
        gp_next(p);
        AstKeyValueExpr *kv = GP_NEW(p, AstKeyValueExpr, AST_KIND_KEY_VALUE_EXPR);
        kv->key = x;
        kv->colon = colon;
        kv->value = gp_parse_value(p);
        x = &kv->node;
    }
    return x;
}

GP_TRACED(Slice, gp_parse_element_list, "ElementList", (GpParser * p), (p))
static Slice gp_parse_element_list_body(GpParser *p) {
    Slice list = slice_nil(TYPE_AST_EXPR);
    while (p->tok != TOKEN_RBRACE && p->tok != TOKEN_EOF) {
        AstExpr e = gp_parse_element(p);
        gp_append(p, &list, &e);
        if (!gp_at_comma(p, BURROW_S("composite literal"), TOKEN_RBRACE))
            break;
        gp_next(p);
    }
    return list;
}

GP_TRACED_NEST(AstExpr, gp_parse_literal_value, "LiteralValue",
               (GpParser * p, AstExpr typ), (p, typ))
static AstExpr gp_parse_literal_value_body(GpParser *p, AstExpr typ) {
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice elts = slice_nil(TYPE_AST_EXPR);
    p->expr_lev++;
    if (p->tok != TOKEN_RBRACE)
        elts = gp_parse_element_list(p);
    p->expr_lev--;
    TokenPos rbrace = gp_expect_closing(p, TOKEN_RBRACE, BURROW_S("composite literal"));

    AstCompositeLit *c = GP_NEW(p, AstCompositeLit, AST_KIND_COMPOSITE_LIT);
    c->type = typ;
    c->lbrace = lbrace;
    c->elts = elts;
    c->rbrace = rbrace;
    return &c->node;
}

GP_TRACED(AstExpr, gp_parse_primary_expr, "PrimaryExpr", (GpParser * p, AstExpr x),
          (p, x))
static AstExpr gp_parse_primary_expr_body(GpParser *p, AstExpr x) {
    if (x == NULL)
        x = gp_parse_operand(p);
    /* We track the nesting here rather than at the entry of the function,
     * since it can iteratively produce a nested output, and we want to limit
     * how deep a structure we generate. */
    Int n;
    for (n = 1;; n++) {
        gp_inc_nest_lev(p);
        switch (p->tok) {
        case TOKEN_PERIOD:
            gp_next(p);
            switch (p->tok) {
            case TOKEN_IDENT:
                x = gp_parse_selector(p, x);
                break;
            case TOKEN_LPAREN:
                x = gp_parse_type_assertion(p, x);
                break;
            default: {
                TokenPos pos = p->pos;
                gp_error_expected(p, pos, BURROW_S("selector or type assertion"));
                /* TODO(rFindley) The check for token.RBRACE below is a
                 * targeted fix to error recovery sufficient to make the
                 * x/tools tests to pass with the new parsing logic introduced
                 * for type parameters. Remove this once error recovery has
                 * been more generally reconsidered. */
                if (p->tok != TOKEN_RBRACE)
                    gp_next(p); /* make progress */
                AstIdent *sel = gp_new_ident(p, pos, BURROW_S("_"));
                x = &gp_new_selector(p, x, sel)->node;
                break;
            }
            }
            break;
        case TOKEN_LBRACK:
            x = gp_parse_index_or_slice_or_instance(p, x);
            break;
        case TOKEN_LPAREN:
            x = &gp_parse_call_or_conversion(p, x)->node;
            break;
        case TOKEN_LBRACE: {
            /* operand may have returned a parenthesized complit type; accept
             * it but complain if we have a complit */
            AstExpr t = ast_unparen(x);
            /* determine if '{' belongs to a composite literal or a block
             * statement */
            switch (t->kind) {
            case AST_KIND_BAD_EXPR:
            case AST_KIND_IDENT:
            case AST_KIND_SELECTOR_EXPR:
            case AST_KIND_INDEX_EXPR:
            case AST_KIND_INDEX_LIST_EXPR:
                if (p->expr_lev < 0) {
                    p->nest_lev -= n;
                    return x;
                }
                /* x is possibly a composite literal type */
                break;
            case AST_KIND_ARRAY_TYPE:
            case AST_KIND_STRUCT_TYPE:
            case AST_KIND_MAP_TYPE:
                /* x is a composite literal type */
                break;
            default:
                p->nest_lev -= n;
                return x;
            }
            if (t != x)
                gp_error(p, gp_expr_pos(t),
                         BURROW_S("cannot parenthesize type in composite literal"));
            /* already progressed, no need to advance */
            x = gp_parse_literal_value(p, x);
            break;
        }
        default:
            p->nest_lev -= n;
            return x;
        }
    }
}

GP_TRACED_NEST(AstExpr, gp_parse_unary_expr, "UnaryExpr", (GpParser * p), (p))
static AstExpr gp_parse_unary_expr_body(GpParser *p) {
    switch (p->tok) {
    case TOKEN_ADD:
    case TOKEN_SUB:
    case TOKEN_NOT:
    case TOKEN_XOR:
    case TOKEN_AND:
    case TOKEN_TILDE: {
        TokenPos pos = p->pos;
        Token op = p->tok;
        gp_next(p);
        AstExpr x = gp_parse_unary_expr(p);
        return &gp_new_unary(p, pos, op, x)->node;
    }
    case TOKEN_ARROW: {
        /* channel type or receive expression */
        TokenPos arrow = p->pos;
        gp_next(p);

        /* If the next token is token.CHAN we still don't know if it is a
         * channel type or a receive operation - we only know once we have
         * found the end of the unary expression. There are two cases:
         *
         *   <- type  => (<-type) must be channel type
         *   <- expr  => <-(expr) is a receive from an expression
         *
         * In the first case, the arrow must be re-associated with the
         * channel type parsed already:
         *
         *   <- (chan type)    =>  (<-chan type)
         *   <- (chan<- type)  =>  (<-chan (<-type)) */

        AstExpr x = gp_parse_unary_expr(p);

        /* determine which case we have */
        if (gp_is(x, AST_KIND_CHAN_TYPE)) {
            AstChanType *typ = (AstChanType *)x;
            /* (<-type) */

            /* re-associate position info and <- */
            AstChanDir dir = AST_SEND;
            while (typ != NULL && dir == AST_SEND) {
                if (typ->dir == AST_RECV) {
                    /* error: (<-type) is (<-(<-chan T)) */
                    gp_error_expected(p, typ->arrow, BURROW_S("'chan'"));
                }
                TokenPos old_arrow = typ->arrow;
                typ->begin = arrow;
                typ->arrow = arrow;
                arrow = old_arrow;
                dir = typ->dir;
                typ->dir = AST_RECV;
                typ = gp_is(typ->value, AST_KIND_CHAN_TYPE) ? (AstChanType *)typ->value
                                                            : NULL;
            }
            if (dir == AST_SEND)
                gp_error_expected(p, arrow, BURROW_S("channel type"));

            return x;
        }

        /* <-(expr) */
        return &gp_new_unary(p, arrow, TOKEN_ARROW, x)->node;
    }
    case TOKEN_MUL: {
        /* pointer type or unary "*" expression */
        TokenPos pos = p->pos;
        gp_next(p);
        AstExpr x = gp_parse_unary_expr(p);
        return &gp_new_star(p, pos, x)->node;
    }
    default:
        break;
    }

    return gp_parse_primary_expr(p, NULL);
}

static Token gp_tok_prec(GpParser *p, Int *prec) {
    Token tok = p->tok;
    if (p->in_rhs && tok == TOKEN_ASSIGN)
        tok = TOKEN_EQL;
    *prec = token_precedence(tok);
    return tok;
}

/* parseBinaryExpr parses a (possibly) binary expression. If x is non-nil, it
 * is used as the left operand. */
GP_TRACED(AstExpr, gp_parse_binary_expr, "BinaryExpr",
          (GpParser * p, AstExpr x, Int prec1), (p, x, prec1))
static AstExpr gp_parse_binary_expr_body(GpParser *p, AstExpr x, Int prec1) {
    if (x == NULL)
        x = gp_parse_unary_expr(p);
    /* We track the nesting here rather than at the entry of the function,
     * since it can iteratively produce a nested output, and we want to limit
     * how deep a structure we generate. */
    for (Int n = 1;; n++) {
        gp_inc_nest_lev(p);
        Int oprec;
        Token op = gp_tok_prec(p, &oprec);
        if (oprec < prec1) {
            p->nest_lev -= n;
            return x;
        }
        TokenPos pos = gp_expect(p, op);
        AstExpr y = gp_parse_binary_expr(p, NULL, oprec + 1);
        x = &gp_new_binary(p, x, pos, op, y)->node;
    }
}

/* The result may be a type or even a raw type ([...]int). */
GP_TRACED(AstExpr, gp_parse_expr, "Expression", (GpParser * p), (p))
static AstExpr gp_parse_expr_body(GpParser *p) {
    return gp_parse_binary_expr(p, NULL, TOKEN_LOWEST_PREC + 1);
}

static AstExpr gp_parse_rhs(GpParser *p) {
    bool old = p->in_rhs;
    p->in_rhs = true;
    AstExpr x = gp_parse_expr(p);
    p->in_rhs = old;
    return x;
}

/* -------------------------------------------------------------- statements */

/* Parsing modes for parseSimpleStmt. */
enum { GP_BASIC, GP_LABEL_OK, GP_RANGE_OK };

static AstAssignStmt *gp_new_assign(GpParser *p, Slice lhs, TokenPos tok_pos, Token tok,
                                    Slice rhs) {
    AstAssignStmt *s = GP_NEW(p, AstAssignStmt, AST_KIND_ASSIGN_STMT);
    s->lhs = lhs;
    s->tok_pos = tok_pos;
    s->tok = tok;
    s->rhs = rhs;
    return s;
}

static AstStmt gp_bad_stmt(GpParser *p, TokenPos from, TokenPos to) {
    AstBadStmt *s = GP_NEW(p, AstBadStmt, AST_KIND_BAD_STMT);
    s->from = from;
    s->to = to;
    return &s->node;
}

/* parseSimpleStmt returns true as 2nd result if it parsed the assignment of
 * a range clause (with mode == rangeOk). The returned statement is an
 * assignment with a right-hand side that is a single unary expression of the
 * form "range x". No guarantees are given for the left-hand side. */
GP_TRACED(AstStmt, gp_parse_simple_stmt, "SimpleStmt",
          (GpParser * p, int mode, bool *is_range), (p, mode, is_range))
static AstStmt gp_parse_simple_stmt_body(GpParser *p, int mode, bool *is_range) {
    *is_range = false;
    Slice x = gp_parse_list(p, false);

    switch (p->tok) {
    case TOKEN_DEFINE:
    case TOKEN_ASSIGN:
    case TOKEN_ADD_ASSIGN:
    case TOKEN_SUB_ASSIGN:
    case TOKEN_MUL_ASSIGN:
    case TOKEN_QUO_ASSIGN:
    case TOKEN_REM_ASSIGN:
    case TOKEN_AND_ASSIGN:
    case TOKEN_OR_ASSIGN:
    case TOKEN_XOR_ASSIGN:
    case TOKEN_SHL_ASSIGN:
    case TOKEN_SHR_ASSIGN:
    case TOKEN_AND_NOT_ASSIGN: {
        /* assignment statement, possibly part of a range clause */
        TokenPos pos = p->pos;
        Token tok = p->tok;
        gp_next(p);
        Slice y;
        if (mode == GP_RANGE_OK && p->tok == TOKEN_RANGE &&
            (tok == TOKEN_DEFINE || tok == TOKEN_ASSIGN)) {
            TokenPos rpos = p->pos;
            gp_next(p);
            y = slice_nil(TYPE_AST_EXPR);
            AstExpr r = &gp_new_unary(p, rpos, TOKEN_RANGE, gp_parse_rhs(p))->node;
            gp_append(p, &y, &r);
            *is_range = true;
        } else {
            y = gp_parse_list(p, true);
        }
        return &gp_new_assign(p, x, pos, tok, y)->node;
    }
    default:
        break;
    }

    AstExpr x0 = BURROW_AT(AstExpr, x, 0);
    if (x.len > 1) {
        gp_error_expected(p, gp_expr_pos(x0), BURROW_S("1 expression"));
        /* continue with first expression */
    }

    switch (p->tok) {
    case TOKEN_COLON: {
        /* labeled statement */
        TokenPos colon = p->pos;
        gp_next(p);
        if (mode == GP_LABEL_OK && gp_is(x0, AST_KIND_IDENT)) {
            /* Go spec: The scope of a label is the body of the function in
             * which it is declared and excludes the body of any nested
             * function. */
            AstLabeledStmt *s = GP_NEW(p, AstLabeledStmt, AST_KIND_LABELED_STMT);
            s->label = (AstIdent *)x0;
            s->colon = colon;
            s->stmt = gp_parse_stmt(p);
            return &s->node;
        }
        /* The label declaration typically starts at x[0].Pos(), but the
         * label declaration may be erroneous due to a token after that
         * position (and before the ':'). If SpuriousErrors is not set, the
         * (only) error reported for the line is the illegal label error
         * instead of the token before the ':' that caused the problem. Thus,
         * use the (latest) colon position for error reporting. */
        gp_error(p, colon, BURROW_S("illegal label declaration"));
        return gp_bad_stmt(p, gp_expr_pos(x0), colon + 1);
    }
    case TOKEN_ARROW: {
        /* send statement */
        TokenPos arrow = p->pos;
        gp_next(p);
        AstExpr y = gp_parse_rhs(p);
        AstSendStmt *s = GP_NEW(p, AstSendStmt, AST_KIND_SEND_STMT);
        s->chan = x0;
        s->arrow = arrow;
        s->value = y;
        return &s->node;
    }
    case TOKEN_INC:
    case TOKEN_DEC: {
        /* increment or decrement */
        AstIncDecStmt *s = GP_NEW(p, AstIncDecStmt, AST_KIND_INC_DEC_STMT);
        s->x = x0;
        s->tok_pos = p->pos;
        s->tok = p->tok;
        gp_next(p);
        return &s->node;
    }
    default:
        break;
    }

    /* expression */
    AstExprStmt *s = GP_NEW(p, AstExprStmt, AST_KIND_EXPR_STMT);
    s->x = x0;
    return &s->node;
}

static AstCallExpr *gp_parse_call_expr(GpParser *p, Str call_type) {
    AstExpr x = gp_parse_rhs(p); /* could be a conversion: (some type)(x) */
    AstExpr t = ast_unparen(x);
    if (t != x) {
        gp_error(p, gp_expr_pos(x),
                 fmt_sprintf_v(p->a, "expression in %s must not be parenthesized",
                               call_type));
        x = t;
    }
    if (gp_is(x, AST_KIND_CALL_EXPR))
        return (AstCallExpr *)x;
    if (!gp_is(x, AST_KIND_BAD_EXPR)) {
        /* only report error if it's a new one */
        gp_error(
            p, ast_expr_end(x),
            fmt_sprintf_v(p->a, "expression in %s must be function call", call_type));
    }
    return NULL;
}

GP_TRACED(AstStmt, gp_parse_go_stmt, "GoStmt", (GpParser * p), (p))
static AstStmt gp_parse_go_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_GO);
    AstCallExpr *call = gp_parse_call_expr(p, BURROW_S("go"));
    gp_expect_semi(p);
    if (call == NULL)
        return gp_bad_stmt(p, pos, pos + 2); /* len("go") */

    AstGoStmt *s = GP_NEW(p, AstGoStmt, AST_KIND_GO_STMT);
    s->go = pos;
    s->call = call;
    return &s->node;
}

GP_TRACED(AstStmt, gp_parse_defer_stmt, "DeferStmt", (GpParser * p), (p))
static AstStmt gp_parse_defer_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_DEFER);
    AstCallExpr *call = gp_parse_call_expr(p, BURROW_S("defer"));
    gp_expect_semi(p);
    if (call == NULL)
        return gp_bad_stmt(p, pos, pos + 5); /* len("defer") */

    AstDeferStmt *s = GP_NEW(p, AstDeferStmt, AST_KIND_DEFER_STMT);
    s->defer = pos;
    s->call = call;
    return &s->node;
}

GP_TRACED(AstReturnStmt *, gp_parse_return_stmt, "ReturnStmt", (GpParser * p), (p))
static AstReturnStmt *gp_parse_return_stmt_body(GpParser *p) {
    TokenPos pos = p->pos;
    gp_expect(p, TOKEN_RETURN);
    Slice x = slice_nil(TYPE_AST_EXPR);
    if (p->tok != TOKEN_SEMICOLON && p->tok != TOKEN_RBRACE)
        x = gp_parse_list(p, true);
    gp_expect_semi(p);

    AstReturnStmt *s = GP_NEW(p, AstReturnStmt, AST_KIND_RETURN_STMT);
    s->return_ = pos;
    s->results = x;
    return s;
}

GP_TRACED(AstBranchStmt *, gp_parse_branch_stmt, "BranchStmt",
          (GpParser * p, Token tok), (p, tok))
static AstBranchStmt *gp_parse_branch_stmt_body(GpParser *p, Token tok) {
    TokenPos pos = gp_expect(p, tok);
    AstIdent *label = NULL;
    if (tok == TOKEN_GOTO ||
        ((tok == TOKEN_CONTINUE || tok == TOKEN_BREAK) && p->tok == TOKEN_IDENT))
        label = gp_parse_ident(p);
    gp_expect_semi(p);

    AstBranchStmt *s = GP_NEW(p, AstBranchStmt, AST_KIND_BRANCH_STMT);
    s->tok_pos = pos;
    s->tok = tok;
    s->label = label;
    return s;
}

static AstExpr gp_make_expr(GpParser *p, AstStmt s, Str want) {
    if (s == NULL)
        return NULL;
    if (gp_is(s, AST_KIND_EXPR_STMT))
        return ((AstExprStmt *)s)->x;
    Str found = BURROW_S("simple statement");
    if (gp_is(s, AST_KIND_ASSIGN_STMT))
        found = BURROW_S("assignment");
    gp_error(
        p, ast_stmt_pos(s),
        fmt_sprintf_v(p->a,
                      "expected %s, found %s (missing parentheses around composite "
                      "literal?)",
                      want, found));
    return gp_bad_expr(p, ast_stmt_pos(s), ast_stmt_end(s));
}

/* parseIfHeader is an adjusted version of parser.header in
 * cmd/compile/internal/syntax/parser.go, which has been tuned for better
 * error handling. */
static void gp_parse_if_header(GpParser *p, AstStmt *init_out, AstExpr *cond_out) {
    AstStmt init = NULL;
    AstExpr cond = NULL;
    bool is_range;
    if (p->tok == TOKEN_LBRACE) {
        gp_error(p, p->pos, BURROW_S("missing condition in if statement"));
        *init_out = NULL;
        *cond_out = gp_bad_expr(p, p->pos, p->pos);
        return;
    }
    /* p->tok != TOKEN_LBRACE */

    Int prev_lev = p->expr_lev;
    p->expr_lev = -1;

    if (p->tok != TOKEN_SEMICOLON) {
        /* accept potential variable declaration but complain */
        if (p->tok == TOKEN_VAR) {
            gp_next(p);
            gp_error(p, p->pos,
                     BURROW_S("var declaration not allowed in if initializer"));
        }
        init = gp_parse_simple_stmt(p, GP_BASIC, &is_range);
    }

    AstStmt cond_stmt = NULL;
    TokenPos semi_pos = TOKEN_NO_POS;
    Str semi_lit = BURROW_STR_EMPTY; /* ";" or "\n"; valid if semi_pos is valid */
    if (p->tok != TOKEN_LBRACE) {
        if (p->tok == TOKEN_SEMICOLON) {
            semi_pos = p->pos;
            semi_lit = p->lit;
            gp_next(p);
        } else {
            gp_expect(p, TOKEN_SEMICOLON);
        }
        if (p->tok != TOKEN_LBRACE)
            cond_stmt = gp_parse_simple_stmt(p, GP_BASIC, &is_range);
    } else {
        cond_stmt = init;
        init = NULL;
    }

    if (cond_stmt != NULL) {
        cond = gp_make_expr(p, cond_stmt, BURROW_S("boolean expression"));
    } else if (token_pos_is_valid(semi_pos)) {
        if (str_eq(semi_lit, BURROW_S("\n")))
            gp_error(p, semi_pos,
                     BURROW_S("unexpected newline, expecting { after if clause"));
        else
            gp_error(p, semi_pos, BURROW_S("missing condition in if statement"));
    }

    /* make sure we have a valid AST */
    if (cond == NULL)
        cond = gp_bad_expr(p, p->pos, p->pos);

    p->expr_lev = prev_lev;
    *init_out = init;
    *cond_out = cond;
}

GP_TRACED_NEST(AstIfStmt *, gp_parse_if_stmt, "IfStmt", (GpParser * p), (p))
static AstIfStmt *gp_parse_if_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_IF);

    AstStmt init;
    AstExpr cond;
    gp_parse_if_header(p, &init, &cond);
    AstBlockStmt *body = gp_parse_block_stmt(p);

    AstStmt else_ = NULL;
    if (p->tok == TOKEN_ELSE) {
        gp_next(p);
        switch (p->tok) {
        case TOKEN_IF:
            else_ = &gp_parse_if_stmt(p)->node;
            break;
        case TOKEN_LBRACE:
            else_ = &gp_parse_block_stmt(p)->node;
            gp_expect_semi(p);
            break;
        default:
            gp_error_expected(p, p->pos, BURROW_S("if statement or block"));
            else_ = gp_bad_stmt(p, p->pos, p->pos);
            break;
        }
    } else {
        gp_expect_semi(p);
    }

    AstIfStmt *s = GP_NEW(p, AstIfStmt, AST_KIND_IF_STMT);
    s->if_ = pos;
    s->init = init;
    s->cond = cond;
    s->body = body;
    s->else_ = else_;
    return s;
}

GP_TRACED(AstCaseClause *, gp_parse_case_clause, "CaseClause", (GpParser * p), (p))
static AstCaseClause *gp_parse_case_clause_body(GpParser *p) {
    TokenPos pos = p->pos;
    Slice list = slice_nil(TYPE_AST_EXPR);
    if (p->tok == TOKEN_CASE) {
        gp_next(p);
        list = gp_parse_list(p, true);
    } else {
        gp_expect(p, TOKEN_DEFAULT);
    }

    TokenPos colon = gp_expect(p, TOKEN_COLON);
    Slice body = gp_parse_stmt_list(p);

    AstCaseClause *c = GP_NEW(p, AstCaseClause, AST_KIND_CASE_CLAUSE);
    c->case_ = pos;
    c->list = list;
    c->colon = colon;
    c->body = body;
    return c;
}

static bool gp_is_type_switch_assert(AstExpr x) {
    return gp_is(x, AST_KIND_TYPE_ASSERT_EXPR) &&
           ((AstTypeAssertExpr *)x)->type == NULL;
}

static bool gp_is_type_switch_guard(GpParser *p, AstStmt s) {
    if (s == NULL)
        return false;
    if (gp_is(s, AST_KIND_EXPR_STMT)) {
        /* x.(type) */
        return gp_is_type_switch_assert(((AstExprStmt *)s)->x);
    }
    if (gp_is(s, AST_KIND_ASSIGN_STMT)) {
        /* v := x.(type) */
        AstAssignStmt *t = (AstAssignStmt *)s;
        if (t->lhs.len == 1 && t->rhs.len == 1 &&
            gp_is_type_switch_assert(BURROW_AT(AstExpr, t->rhs, 0))) {
            switch (t->tok) {
            case TOKEN_ASSIGN:
                /* permit v = x.(type) but complain */
                gp_error(p, t->tok_pos, BURROW_S("expected ':=', found '='"));
                return true;
            case TOKEN_DEFINE:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

GP_TRACED(AstStmt, gp_parse_switch_stmt, "SwitchStmt", (GpParser * p), (p))
static AstStmt gp_parse_switch_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_SWITCH);

    AstStmt s1 = NULL;
    AstStmt s2 = NULL;
    bool is_range;
    if (p->tok != TOKEN_LBRACE) {
        Int prev_lev = p->expr_lev;
        p->expr_lev = -1;
        if (p->tok != TOKEN_SEMICOLON)
            s2 = gp_parse_simple_stmt(p, GP_BASIC, &is_range);
        if (p->tok == TOKEN_SEMICOLON) {
            gp_next(p);
            s1 = s2;
            s2 = NULL;
            if (p->tok != TOKEN_LBRACE) {
                /* A TypeSwitchGuard may declare a variable in addition to the
                 * variable declared in the initial SimpleStmt. Introduce an
                 * extra scope to avoid redeclaration errors:
                 *
                 *	switch t := 0; t := x.(T) { ... }
                 *
                 * (this code is not valid Go because the first t cannot be
                 * accessed and thus is never used, the extra scope is needed
                 * for the correct error message).
                 *
                 * If we don't have a type switch, s2 must be an expression.
                 * Having the extra nested but empty scope won't affect
                 * matters. */
                s2 = gp_parse_simple_stmt(p, GP_BASIC, &is_range);
            }
        }
        p->expr_lev = prev_lev;
    }

    bool type_switch = gp_is_type_switch_guard(p, s2);
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice list = slice_nil(TYPE_AST_STMT);
    while (p->tok == TOKEN_CASE || p->tok == TOKEN_DEFAULT) {
        AstStmt c = &gp_parse_case_clause(p)->node;
        gp_append(p, &list, &c);
    }
    TokenPos rbrace = gp_expect(p, TOKEN_RBRACE);
    gp_expect_semi(p);
    AstBlockStmt *body = gp_new_block(p, lbrace, list, rbrace);

    if (type_switch) {
        AstTypeSwitchStmt *s = GP_NEW(p, AstTypeSwitchStmt, AST_KIND_TYPE_SWITCH_STMT);
        s->switch_ = pos;
        s->init = s1;
        s->assign = s2;
        s->body = body;
        return &s->node;
    }

    AstSwitchStmt *s = GP_NEW(p, AstSwitchStmt, AST_KIND_SWITCH_STMT);
    s->switch_ = pos;
    s->init = s1;
    s->tag = gp_make_expr(p, s2, BURROW_S("switch expression"));
    s->body = body;
    return &s->node;
}

GP_TRACED(AstCommClause *, gp_parse_comm_clause, "CommClause", (GpParser * p), (p))
static AstCommClause *gp_parse_comm_clause_body(GpParser *p) {
    TokenPos pos = p->pos;
    AstStmt comm = NULL;
    if (p->tok == TOKEN_CASE) {
        gp_next(p);
        Slice lhs = gp_parse_list(p, false);
        AstExpr lhs0 = BURROW_AT(AstExpr, lhs, 0);
        if (p->tok == TOKEN_ARROW) {
            /* SendStmt */
            if (lhs.len > 1) {
                gp_error_expected(p, gp_expr_pos(lhs0), BURROW_S("1 expression"));
                /* continue with first expression */
            }
            TokenPos arrow = p->pos;
            gp_next(p);
            AstExpr rhs = gp_parse_rhs(p);
            AstSendStmt *s = GP_NEW(p, AstSendStmt, AST_KIND_SEND_STMT);
            s->chan = lhs0;
            s->arrow = arrow;
            s->value = rhs;
            comm = &s->node;
        } else {
            /* RecvStmt */
            Token tok = p->tok;
            if (tok == TOKEN_ASSIGN || tok == TOKEN_DEFINE) {
                /* RecvStmt with assignment */
                if (lhs.len > 2) {
                    gp_error_expected(p, gp_expr_pos(lhs0),
                                      BURROW_S("1 or 2 expressions"));
                    /* continue with first two expressions */
                    lhs.len = 2;
                }
                TokenPos tpos = p->pos;
                gp_next(p);
                AstExpr rhs = gp_parse_rhs(p);
                Slice rl = slice_nil(TYPE_AST_EXPR);
                gp_append(p, &rl, &rhs);
                comm = &gp_new_assign(p, lhs, tpos, tok, rl)->node;
            } else {
                /* lhs must be single receive operation */
                if (lhs.len > 1) {
                    gp_error_expected(p, gp_expr_pos(lhs0), BURROW_S("1 expression"));
                    /* continue with first expression */
                }
                AstExprStmt *s = GP_NEW(p, AstExprStmt, AST_KIND_EXPR_STMT);
                s->x = lhs0;
                comm = &s->node;
            }
        }
    } else {
        gp_expect(p, TOKEN_DEFAULT);
    }

    TokenPos colon = gp_expect(p, TOKEN_COLON);
    Slice body = gp_parse_stmt_list(p);

    AstCommClause *c = GP_NEW(p, AstCommClause, AST_KIND_COMM_CLAUSE);
    c->case_ = pos;
    c->comm = comm;
    c->colon = colon;
    c->body = body;
    return c;
}

GP_TRACED(AstSelectStmt *, gp_parse_select_stmt, "SelectStmt", (GpParser * p), (p))
static AstSelectStmt *gp_parse_select_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_SELECT);
    TokenPos lbrace = gp_expect(p, TOKEN_LBRACE);
    Slice list = slice_nil(TYPE_AST_STMT);
    while (p->tok == TOKEN_CASE || p->tok == TOKEN_DEFAULT) {
        AstStmt c = &gp_parse_comm_clause(p)->node;
        gp_append(p, &list, &c);
    }
    TokenPos rbrace = gp_expect(p, TOKEN_RBRACE);
    gp_expect_semi(p);
    AstBlockStmt *body = gp_new_block(p, lbrace, list, rbrace);

    AstSelectStmt *s = GP_NEW(p, AstSelectStmt, AST_KIND_SELECT_STMT);
    s->select = pos;
    s->body = body;
    return s;
}

GP_TRACED(AstStmt, gp_parse_for_stmt, "ForStmt", (GpParser * p), (p))
static AstStmt gp_parse_for_stmt_body(GpParser *p) {
    TokenPos pos = gp_expect(p, TOKEN_FOR);

    AstStmt s1 = NULL;
    AstStmt s2 = NULL;
    AstStmt s3 = NULL;
    bool is_range = false;
    bool ignored;
    if (p->tok != TOKEN_LBRACE) {
        Int prev_lev = p->expr_lev;
        p->expr_lev = -1;
        if (p->tok != TOKEN_SEMICOLON) {
            if (p->tok == TOKEN_RANGE) {
                /* "for range x" (nil lhs in assignment) */
                TokenPos rpos = p->pos;
                gp_next(p);
                Slice y = slice_nil(TYPE_AST_EXPR);
                AstExpr r = &gp_new_unary(p, rpos, TOKEN_RANGE, gp_parse_rhs(p))->node;
                gp_append(p, &y, &r);
                s2 = &gp_new_assign(p, slice_nil(TYPE_AST_EXPR), TOKEN_NO_POS,
                                    TOKEN_ILLEGAL, y)
                          ->node;
                is_range = true;
            } else {
                s2 = gp_parse_simple_stmt(p, GP_RANGE_OK, &is_range);
            }
        }
        if (!is_range && p->tok == TOKEN_SEMICOLON) {
            gp_next(p);
            s1 = s2;
            s2 = NULL;
            if (p->tok != TOKEN_SEMICOLON)
                s2 = gp_parse_simple_stmt(p, GP_BASIC, &ignored);
            gp_expect_semi(p);
            if (p->tok != TOKEN_LBRACE)
                s3 = gp_parse_simple_stmt(p, GP_BASIC, &ignored);
        }
        p->expr_lev = prev_lev;
    }

    AstBlockStmt *body = gp_parse_block_stmt(p);
    gp_expect_semi(p);

    if (is_range) {
        AstAssignStmt *as = (AstAssignStmt *)s2;
        /* check lhs */
        AstExpr key = NULL;
        AstExpr value = NULL;
        switch (as->lhs.len) {
        case 0:
            /* nothing to do */
            break;
        case 1:
            key = BURROW_AT(AstExpr, as->lhs, 0);
            break;
        case 2:
            key = BURROW_AT(AstExpr, as->lhs, 0);
            value = BURROW_AT(AstExpr, as->lhs, 1);
            break;
        default:
            gp_error_expected(p,
                              gp_expr_pos(BURROW_AT(AstExpr, as->lhs, as->lhs.len - 1)),
                              BURROW_S("at most 2 expressions"));
            return gp_bad_stmt(p, pos, ast_block_stmt_end(body));
        }
        /* parseSimpleStmt returned a right-hand side that is a single unary
         * expression of the form "range x" */
        AstExpr rhs0 = BURROW_AT(AstExpr, as->rhs, 0);
        AstRangeStmt *s = GP_NEW(p, AstRangeStmt, AST_KIND_RANGE_STMT);
        s->for_ = pos;
        s->key = key;
        s->value = value;
        s->tok_pos = as->tok_pos;
        s->tok = as->tok;
        s->range = gp_expr_pos(rhs0);
        s->x = ((AstUnaryExpr *)rhs0)->x;
        s->body = body;
        return &s->node;
    }

    /* regular for statement */
    AstForStmt *s = GP_NEW(p, AstForStmt, AST_KIND_FOR_STMT);
    s->for_ = pos;
    s->init = s1;
    s->cond = gp_make_expr(p, s2, BURROW_S("boolean or range expression"));
    s->post = s3;
    s->body = body;
    return &s->node;
}

static AstDecl gp_parse_decl(GpParser *p, bool (*sync)(Token));

GP_TRACED_NEST(AstStmt, gp_parse_stmt, "Statement", (GpParser * p), (p))
static AstStmt gp_parse_stmt_body(GpParser *p) {
    AstStmt s = NULL;
    bool is_range;
    switch (p->tok) {
    case TOKEN_CONST:
    case TOKEN_TYPE_:
    case TOKEN_VAR: {
        AstDeclStmt *d = GP_NEW(p, AstDeclStmt, AST_KIND_DECL_STMT);
        d->decl = gp_parse_decl(p, gp_stmt_start);
        s = &d->node;
        break;
    }
    case TOKEN_IDENT:
    case TOKEN_INT:
    case TOKEN_FLOAT:
    case TOKEN_IMAG:
    case TOKEN_CHAR:
    case TOKEN_STRING:
    case TOKEN_FUNC:
    case TOKEN_LPAREN: /* operands */
    case TOKEN_LBRACK:
    case TOKEN_STRUCT:
    case TOKEN_MAP:
    case TOKEN_CHAN:
    case TOKEN_INTERFACE: /* composite types */
    case TOKEN_ADD:
    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_AND:
    case TOKEN_XOR:
    case TOKEN_ARROW:
    case TOKEN_NOT: /* unary operators */
        s = gp_parse_simple_stmt(p, GP_LABEL_OK, &is_range);
        /* because of the required look-ahead, labeled statements are parsed
         * by parseSimpleStmt - don't expect a semicolon after them */
        if (!gp_is(s, AST_KIND_LABELED_STMT))
            gp_expect_semi(p);
        break;
    case TOKEN_GO:
        s = gp_parse_go_stmt(p);
        break;
    case TOKEN_DEFER:
        s = gp_parse_defer_stmt(p);
        break;
    case TOKEN_RETURN:
        s = &gp_parse_return_stmt(p)->node;
        break;
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
    case TOKEN_GOTO:
    case TOKEN_FALLTHROUGH:
        s = &gp_parse_branch_stmt(p, p->tok)->node;
        break;
    case TOKEN_LBRACE:
        s = &gp_parse_block_stmt(p)->node;
        gp_expect_semi(p);
        break;
    case TOKEN_IF:
        s = &gp_parse_if_stmt(p)->node;
        break;
    case TOKEN_SWITCH:
        s = gp_parse_switch_stmt(p);
        break;
    case TOKEN_SELECT:
        s = &gp_parse_select_stmt(p)->node;
        break;
    case TOKEN_FOR:
        s = gp_parse_for_stmt(p);
        break;
    case TOKEN_SEMICOLON: {
        /* Is it ever possible to have an implicit semicolon producing an
         * empty statement in a valid program? (handle correctly anyway) */
        AstEmptyStmt *e = GP_NEW(p, AstEmptyStmt, AST_KIND_EMPTY_STMT);
        e->semicolon = p->pos;
        e->implicit = str_eq(p->lit, BURROW_S("\n"));
        gp_next(p);
        s = &e->node;
        break;
    }
    case TOKEN_RBRACE: {
        /* a semicolon may be omitted before a closing "}" */
        AstEmptyStmt *e = GP_NEW(p, AstEmptyStmt, AST_KIND_EMPTY_STMT);
        e->semicolon = p->pos;
        e->implicit = true;
        s = &e->node;
        break;
    }
    default: {
        /* no statement found */
        TokenPos pos = p->pos;
        gp_error_expected(p, pos, BURROW_S("statement"));
        gp_advance(p, gp_stmt_start);
        s = gp_bad_stmt(p, pos, p->pos);
        break;
    }
    }

    return s;
}

/* ------------------------------------------------------------ declarations */

typedef AstSpec (*GpSpecFunc)(GpParser *p, AstCommentGroup *doc, Token keyword,
                              Int iota);

GP_TRACED(AstSpec, gp_parse_import_spec, "ImportSpec",
          (GpParser * p, AstCommentGroup *doc, Token keyword, Int iota),
          (p, doc, keyword, iota))
static AstSpec gp_parse_import_spec_body(GpParser *p, AstCommentGroup *doc,
                                         Token keyword, Int iota) {
    (void)keyword;
    (void)iota;
    AstIdent *ident = NULL;
    switch (p->tok) {
    case TOKEN_IDENT:
        ident = gp_parse_ident(p);
        break;
    case TOKEN_PERIOD:
        ident = gp_new_ident(p, p->pos, BURROW_S("."));
        gp_next(p);
        break;
    default:
        break;
    }

    TokenPos pos = p->pos;
    TokenPos end = p->pos;
    Str path = BURROW_STR_EMPTY;
    if (p->tok == TOKEN_STRING) {
        path = p->lit;
        end = gp_end(p);
        gp_next(p);
    } else if (token_is_literal(p->tok)) {
        gp_error(p, pos, BURROW_S("import path must be a string"));
        gp_next(p);
    } else {
        gp_error(p, pos, BURROW_S("missing import path"));
        gp_advance(p, gp_expr_end);
    }
    AstCommentGroup *comment = gp_expect_semi(p);

    /* collect imports */
    AstBasicLit *lit = GP_NEW(p, AstBasicLit, AST_KIND_BASIC_LIT);
    lit->value_pos = pos;
    lit->value_end = end;
    lit->kind = TOKEN_STRING;
    lit->value = path;
    AstImportSpec *spec = GP_NEW(p, AstImportSpec, AST_KIND_IMPORT_SPEC);
    spec->doc = doc;
    spec->name = ident;
    spec->path = lit;
    spec->comment = comment;
    gp_append(p, &p->imports, &spec);

    return &spec->node;
}

static AstSpec gp_parse_value_spec_body(GpParser *p, AstCommentGroup *doc,
                                        Token keyword, Int iota);

static AstSpec gp_parse_value_spec(GpParser *p, AstCommentGroup *doc, Token keyword,
                                   Int iota) {
    if (p->trace)
        gp_trace_in(p, fmt_sprintf_v(p->a, "%sSpec", gp_tok_string(p, keyword)));
    AstSpec s = gp_parse_value_spec_body(p, doc, keyword, iota);
    if (p->trace)
        gp_trace_out(p);
    return s;
}

static AstSpec gp_parse_value_spec_body(GpParser *p, AstCommentGroup *doc,
                                        Token keyword, Int iota) {
    (void)iota;
    Slice idents = gp_parse_ident_list(p);
    AstExpr typ = NULL;
    Slice values = slice_nil(TYPE_AST_EXPR);
    switch (keyword) {
    case TOKEN_CONST:
        /* always permit optional type and initialization for more tolerant
         * parsing */
        if (p->tok != TOKEN_EOF && p->tok != TOKEN_SEMICOLON &&
            p->tok != TOKEN_RPAREN) {
            typ = gp_try_ident_or_type(p);
            if (p->tok == TOKEN_ASSIGN) {
                gp_next(p);
                values = gp_parse_list(p, true);
            }
        }
        break;
    case TOKEN_VAR:
        if (p->tok != TOKEN_ASSIGN)
            typ = gp_parse_type(p);
        if (p->tok == TOKEN_ASSIGN) {
            gp_next(p);
            values = gp_parse_list(p, true);
        }
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    AstCommentGroup *comment = gp_expect_semi(p);

    AstValueSpec *spec = GP_NEW(p, AstValueSpec, AST_KIND_VALUE_SPEC);
    spec->doc = doc;
    spec->names = idents;
    spec->type = typ;
    spec->values = values;
    spec->comment = comment;
    return &spec->node;
}

static void gp_parse_generic_type_body(GpParser *p, AstTypeSpec *spec,
                                       TokenPos open_pos, AstIdent *name0,
                                       AstExpr typ0) {
    Slice list = gp_parse_parameter_list(p, name0, typ0, TOKEN_RBRACK, false);
    TokenPos close_pos = gp_expect(p, TOKEN_RBRACK);
    spec->type_params = gp_new_field_list(p, open_pos, list, close_pos);
    if (p->tok == TOKEN_ASSIGN) {
        /* type alias */
        spec->assign = p->pos;
        gp_next(p);
    }
    spec->type = gp_parse_type(p);
}

static void gp_parse_generic_type(GpParser *p, AstTypeSpec *spec, TokenPos open_pos,
                                  AstIdent *name0, AstExpr typ0) {
    if (p->trace)
        gp_trace_in(p, BURROW_S("parseGenericType"));
    gp_parse_generic_type_body(p, spec, open_pos, name0, typ0);
    if (p->trace)
        gp_trace_out(p);
}

static bool gp_is_type_elem(AstExpr x);

/* extractName splits the expression x into (name, expr) if syntactically x
 * can be written as name expr. The split only happens if expr is a type
 * element (per the isTypeElem predicate) or if force is set. If x is just a
 * name, the result is (name, nil). If the split succeeds, the result is
 * (name, expr). Otherwise the result is (nil, x). Examples:
 *
 *	x           force    name    expr
 *	------------------------------------
 *	P*[]int     T/F      P       *[]int
 *	P*E         T        P       *E
 *	P*E         F        nil     P*E
 *	P([]int)    T/F      P       ([]int)
 *	P(E)        T        P       (E)
 *	P(E)        F        nil     P(E)
 *	P*E|F|~G    T/F      P       *E|F|~G
 *	P*E|F|G     T        P       *E|F|G
 *	P*E|F|G     F        nil     P*E|F|G */
static AstIdent *gp_extract_name(GpParser *p, AstExpr x, bool force, AstExpr *expr) {
    switch (x->kind) {
    case AST_KIND_IDENT:
        *expr = NULL;
        return (AstIdent *)x;
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *b = (AstBinaryExpr *)x;
        switch (b->op) {
        case TOKEN_MUL:
            if (gp_is(b->x, AST_KIND_IDENT) && (force || gp_is_type_elem(b->y))) {
                /* x = name *b.Y */
                *expr = &gp_new_star(p, b->op_pos, b->y)->node;
                return (AstIdent *)b->x;
            }
            break;
        case TOKEN_OR: {
            AstExpr lhs = NULL;
            AstIdent *name =
                gp_extract_name(p, b->x, force || gp_is_type_elem(b->y), &lhs);
            if (name != NULL && lhs != NULL) {
                /* x = name lhs|b.Y */
                AstBinaryExpr *op = gp_new_binary(p, lhs, b->op_pos, b->op, b->y);
                *expr = &op->node;
                return name;
            }
            break;
        }
        default:
            break;
        }
        break;
    }
    case AST_KIND_CALL_EXPR: {
        AstCallExpr *c = (AstCallExpr *)x;
        if (gp_is(c->fun, AST_KIND_IDENT)) {
            if (c->args.len == 1 && c->ellipsis == TOKEN_NO_POS &&
                (force || gp_is_type_elem(BURROW_AT(AstExpr, c->args, 0)))) {
                /* x = name (x.Args[0])
                 * (Note that the cmd/compile/internal/syntax parser does not
                 * care about syntax tree fidelity and does not preserve
                 * parentheses here.) */
                *expr =
                    gp_paren(p, c->lparen, BURROW_AT(AstExpr, c->args, 0), c->rparen);
                return (AstIdent *)c->fun;
            }
        }
        break;
    }
    default:
        break;
    }
    *expr = x;
    return NULL;
}

/* isTypeElem reports whether x is a (possibly parenthesized) type element
 * expression. The result is false if x could be a type element OR an
 * ordinary (value) expression. */
static bool gp_is_type_elem(AstExpr x) {
    switch (x->kind) {
    case AST_KIND_ARRAY_TYPE:
    case AST_KIND_STRUCT_TYPE:
    case AST_KIND_FUNC_TYPE:
    case AST_KIND_INTERFACE_TYPE:
    case AST_KIND_MAP_TYPE:
    case AST_KIND_CHAN_TYPE:
        return true;
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *b = (AstBinaryExpr *)x;
        return gp_is_type_elem(b->x) || gp_is_type_elem(b->y);
    }
    case AST_KIND_UNARY_EXPR:
        return ((AstUnaryExpr *)x)->op == TOKEN_TILDE;
    case AST_KIND_PAREN_EXPR:
        return gp_is_type_elem(((AstParenExpr *)x)->x);
    default:
        return false;
    }
}

GP_TRACED(AstSpec, gp_parse_type_spec, "TypeSpec",
          (GpParser * p, AstCommentGroup *doc, Token keyword, Int iota),
          (p, doc, keyword, iota))
static AstSpec gp_parse_type_spec_body(GpParser *p, AstCommentGroup *doc, Token keyword,
                                       Int iota) {
    (void)keyword;
    (void)iota;
    AstIdent *name = gp_parse_ident(p);
    AstTypeSpec *spec = GP_NEW(p, AstTypeSpec, AST_KIND_TYPE_SPEC);
    spec->doc = doc;
    spec->name = name;

    if (p->tok == TOKEN_LBRACK) {
        /* spec.Name "[" ... must be an array type or a generic type
         * declaration */
        TokenPos lbrack = p->pos;
        gp_next(p);
        if (p->tok == TOKEN_IDENT) {
            /* We may have an array type or a type parameter list. In either
             * case we expect an expression x (which may just be a name, or a
             * more complex expression) which we can analyze further.
             *
             * A type parameter list may have a type bound starting with a
             * "[" as in
             *
             *	P []E
             *
             * In that case, x would be parsed as an (invalid) index
             * expression P[]E. Tell the parser that we are parsing a type
             * parameter list. */
            AstExpr x = &gp_parse_ident(p)->node;
            if (p->tok != TOKEN_LBRACK) {
                /* To parse the expression starting with name, expand the call
                 * sequence we would get by passing in name to
                 * parser.expr, and pass in name to parsePrimaryExpr. */
                p->expr_lev++;
                AstExpr lhs = gp_parse_primary_expr(p, x);
                x = gp_parse_binary_expr(p, lhs, TOKEN_LOWEST_PREC + 1);
                p->expr_lev--;
            }
            /* Analyze expression x. If we can split x into a type parameter
             * name, possibly followed by a type parameter type, we consider
             * this the start of a type parameter list, with some caveats: a
             * single name followed by "]" tilts the decision towards an
             * array declaration; a type parameter type that could also be an
             * ordinary expression but which is followed by a comma tilts the
             * decision towards a type parameter list. */
            AstExpr ptype = NULL;
            AstIdent *pname = gp_extract_name(p, x, p->tok == TOKEN_COMMA, &ptype);
            if (pname != NULL && (ptype != NULL || p->tok != TOKEN_RBRACK)) {
                /* spec.Name "[" pname ...
                 * spec.Name "[" pname ptype ...
                 * spec.Name "[" pname ptype "," ... */
                gp_parse_generic_type(p, spec, lbrack, pname,
                                      ptype); /* ptype may be nil */
            } else {
                /* spec.Name "[" pname "]" ...
                 * spec.Name "[" x ... */
                spec->type = &gp_parse_array_type(p, lbrack, x)->node;
            }
        } else {
            /* array type */
            spec->type = &gp_parse_array_type(p, lbrack, NULL)->node;
        }
    } else {
        /* no type parameters */
        if (p->tok == TOKEN_ASSIGN) {
            /* type alias */
            spec->assign = p->pos;
            gp_next(p);
        }
        spec->type = gp_parse_type(p);
    }

    spec->comment = gp_expect_semi(p);

    return &spec->node;
}

static AstGenDecl *gp_parse_gen_decl_body(GpParser *p, Token keyword, GpSpecFunc f);

static AstGenDecl *gp_parse_gen_decl(GpParser *p, Token keyword, GpSpecFunc f) {
    if (p->trace)
        gp_trace_in(p, fmt_sprintf_v(p->a, "GenDecl(%s)", gp_tok_string(p, keyword)));
    AstGenDecl *d = gp_parse_gen_decl_body(p, keyword, f);
    if (p->trace)
        gp_trace_out(p);
    return d;
}

static AstGenDecl *gp_parse_gen_decl_body(GpParser *p, Token keyword, GpSpecFunc f) {
    AstCommentGroup *doc = p->lead_comment;
    TokenPos pos = gp_expect(p, keyword);
    TokenPos lparen = TOKEN_NO_POS;
    TokenPos rparen = TOKEN_NO_POS;
    Slice list = slice_nil(TYPE_AST_SPEC);
    if (p->tok == TOKEN_LPAREN) {
        lparen = p->pos;
        gp_next(p);
        for (Int iota = 0; p->tok != TOKEN_RPAREN && p->tok != TOKEN_EOF; iota++) {
            AstSpec s = f(p, p->lead_comment, keyword, iota);
            gp_append(p, &list, &s);
        }
        rparen = gp_expect(p, TOKEN_RPAREN);
        gp_expect_semi(p);
    } else {
        AstSpec s = f(p, NULL, keyword, 0);
        gp_append(p, &list, &s);
    }

    AstGenDecl *d = GP_NEW(p, AstGenDecl, AST_KIND_GEN_DECL);
    d->doc = doc;
    d->tok_pos = pos;
    d->tok = keyword;
    d->lparen = lparen;
    d->specs = list;
    d->rparen = rparen;
    return d;
}

GP_TRACED(AstFuncDecl *, gp_parse_func_decl, "FunctionDecl", (GpParser * p), (p))
static AstFuncDecl *gp_parse_func_decl_body(GpParser *p) {
    AstCommentGroup *doc = p->lead_comment;
    TokenPos pos = gp_expect(p, TOKEN_FUNC);

    AstFieldList *recv = NULL;
    if (p->tok == TOKEN_LPAREN)
        recv = gp_parse_parameters(p, false);

    AstIdent *ident = gp_parse_ident(p);

    AstFieldList *tparams = NULL;
    if (p->tok == TOKEN_LBRACK) {
        tparams = gp_parse_type_parameters(p);
        if (recv != NULL && tparams != NULL) {
            /* Method declarations do not have type parameters. We parse them
             * for a better error message and improved error recovery. */
            gp_error(p, tparams->opening,
                     BURROW_S("method must have no type parameters"));
        }
    }
    AstFieldList *params = gp_parse_parameters(p, false);
    AstFieldList *results = gp_parse_parameters(p, true);

    AstBlockStmt *body = NULL;
    switch (p->tok) {
    case TOKEN_LBRACE:
        body = gp_parse_body(p);
        gp_expect_semi(p);
        break;
    case TOKEN_SEMICOLON:
        gp_next(p);
        if (p->tok == TOKEN_LBRACE) {
            /* opening { of function declaration on next line */
            gp_error(p, p->pos, BURROW_S("unexpected semicolon or newline before {"));
            body = gp_parse_body(p);
            gp_expect_semi(p);
        }
        break;
    default:
        gp_expect_semi(p);
        break;
    }

    AstFuncType *typ = gp_new_func_type(p, pos, params, results);
    typ->type_params = tparams;
    AstFuncDecl *decl = GP_NEW(p, AstFuncDecl, AST_KIND_FUNC_DECL);
    decl->doc = doc;
    decl->recv = recv;
    decl->name = ident;
    decl->type = typ;
    decl->body = body;
    return decl;
}

GP_TRACED(AstDecl, gp_parse_decl, "Declaration", (GpParser * p, bool (*sync)(Token)),
          (p, sync))
static AstDecl gp_parse_decl_body(GpParser *p, bool (*sync)(Token)) {
    GpSpecFunc f;
    switch (p->tok) {
    case TOKEN_IMPORT:
        f = gp_parse_import_spec;
        break;
    case TOKEN_CONST:
    case TOKEN_VAR:
        f = gp_parse_value_spec;
        break;
    case TOKEN_TYPE_:
        f = gp_parse_type_spec;
        break;
    case TOKEN_FUNC:
        return &gp_parse_func_decl(p)->node;
    default: {
        TokenPos pos = p->pos;
        gp_error_expected(p, pos, BURROW_S("declaration"));
        gp_advance(p, sync);
        AstBadDecl *d = GP_NEW(p, AstBadDecl, AST_KIND_BAD_DECL);
        d->from = pos;
        d->to = p->pos;
        return &d->node;
    }
    }

    return &gp_parse_gen_decl(p, p->tok, f)->node;
}

/* ------------------------------------------------------------ source files */

GP_TRACED(AstFile *, gp_parse_file, "File", (GpParser * p), (p))
static AstFile *gp_parse_file_body(GpParser *p) {
    /* Don't bother parsing the rest if we had errors scanning the first
     * token. Likely not a Go source file at all. */
    if (go_scanner_error_list_len(p->errors) != 0)
        return NULL;

    /* package clause */
    AstCommentGroup *doc = p->lead_comment;
    TokenPos pos = gp_expect(p, TOKEN_PACKAGE);
    /* Go spec: The package clause is not a declaration; the package name
     * does not appear in any scope. */
    AstIdent *ident = gp_parse_ident(p);
    if (str_eq(ident->name, BURROW_S("_")) &&
        (p->mode & PARSER_DECLARATION_ERRORS) != 0)
        gp_error(p, p->pos, BURROW_S("invalid package name _"));
    gp_expect_semi(p);

    /* Don't bother parsing the rest if we had errors parsing the package
     * clause. Likely not a Go source file at all. */
    if (go_scanner_error_list_len(p->errors) != 0)
        return NULL;

    Slice decls = slice_nil(TYPE_AST_DECL);
    if ((p->mode & PARSER_PACKAGE_CLAUSE_ONLY) == 0) {
        /* import decls */
        while (p->tok == TOKEN_IMPORT) {
            AstDecl d = &gp_parse_gen_decl(p, TOKEN_IMPORT, gp_parse_import_spec)->node;
            gp_append(p, &decls, &d);
        }

        if ((p->mode & PARSER_IMPORTS_ONLY) == 0) {
            /* rest of package body */
            Token prev = TOKEN_IMPORT;
            while (p->tok != TOKEN_EOF) {
                /* Continue to accept import declarations for error tolerance,
                 * but complain. */
                if (p->tok == TOKEN_IMPORT && prev != TOKEN_IMPORT)
                    gp_error(p, p->pos,
                             BURROW_S("imports must appear before other declarations"));
                prev = p->tok;

                AstDecl d = gp_parse_decl(p, gp_decl_start);
                gp_append(p, &decls, &d);
            }
        }
    }

    AstFile *f = GP_NEW(p, AstFile, AST_KIND_FILE);
    f->doc = doc;
    f->package = pos;
    f->name = ident;
    f->decls = decls;
    /* file_start and file_end are set by the caller. */
    f->imports = p->imports;
    f->comments = p->comments;
    f->go_version = p->go_version;
    f->unresolved = slice_nil(TYPE_AST_IDENT_PTR);
    if ((p->mode & PARSER_SKIP_OBJECT_RESOLUTION) == 0)
        burrow__parser_resolve_file(p, p->a, f, p->file,
                                    (p->mode & PARSER_DECLARATION_ERRORS) != 0);

    return f;
}

/* --------------------------------------------------------------- interface */

static const Str gp_invalid_source_text = BURROW_S_INIT("invalid source");

/* The source as bytes in a, from src or, when src is nil, the file. */
static Slice gp_read_source(Alloc *a, Str filename, Any src, Error *err) {
    if (src.t == NULL)
        return os_read_file(a, filename, err);
    Slice from = slice_nil(TYPE_BYTES);
    bool ok = false;
    if (src.t == TYPE_STRING && src.data != NULL) {
        Str s = *(const Str *)src.data;
        from.p = (void *)(uintptr_t)s.p;
        from.len = s.len;
        from.cap = s.len;
        ok = true;
    } else if (src.t == TYPE_BYTES && src.data != NULL) {
        from = *(const Slice *)src.data;
        ok = true;
    } else if (src.t == TYPE_BYTES_BUFFER && src.data != NULL) {
        from = bytes_buffer_bytes((BytesBuffer *)src.data);
        ok = true;
    } else if (src.t == &burrow_type_IoReader && src.data != NULL) {
        return io_read_all(a, *(const IoReader *)src.data, err);
    }
    if (!ok) {
        *err = errors_new(error_allocator(), gp_invalid_source_text);
        return slice_nil(TYPE_BYTES);
    }
    Slice out = slice_nil(TYPE_BYTES);
    if (from.len > 0) {
        out = slice_append(a, out, from.p, from.len);
        if (out.len != from.len) {
            *err = burrow_err_out_of_memory;
            return slice_nil(TYPE_BYTES);
        }
    }
    return out;
}

/* The trace lines the defers in Go print while a bailout unwinds. */
static void gp_unwind_trace(GpParser *p) {
    if (p->trace) {
        while (p->indent > 0)
            gp_trace_out(p);
    }
}

/* The part of ParseFile that can bail out, in a function of its own so that
 * nothing the catch block reads is a local of the frame the jump lands in. */
static AstFile *gp_parse_file_guarded(GpParser *p, TokenFile *file, Slice src,
                                      ParserMode mode) {
    AstFile *volatile f = NULL;
    BURROW_TRY {
        gp_init(p, file, src, mode);
        f = gp_parse_file(p);
    }
    BURROW_CATCH(r) {
        if (!p->bailing)
            panic(r);
        gp_unwind_trace(p);
        f = NULL;
    }
    BURROW_TRY_END;
    return f;
}

static Error gp_finish(GpParser *p) {
    if (p->oom)
        return burrow_err_out_of_memory;
    if (p->bailing && p->bail_msg.len > 0)
        go_scanner_error_list_add(&p->errors, error_allocator(),
                                  token_file_position(p->file, p->bail_pos),
                                  p->bail_msg);
    go_scanner_error_list_sort(p->errors);
    return go_scanner_error_list_err(p->errors);
}

AstFile *parser_parse_file(Alloc *a, TokenFileSet *fset, Str filename, Any src,
                           ParserMode mode, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    if (fset == NULL)
        panic_str(
            BURROW_S("parser.ParseFile: no token.FileSet provided (fset == nil)"));

    Error e = BURROW_NO_ERROR;
    Slice text = gp_read_source(a, filename, src, &e);
    if (!BURROW_OK(e)) {
        if (err != NULL)
            *err = e;
        return NULL;
    }

    TokenFile *file = token_file_set_add_file(fset, filename, -1, text.len);
    if (file == NULL) {
        if (err != NULL)
            *err = burrow_err_out_of_memory;
        return NULL;
    }

    GpParser p;
    memset(&p, 0, sizeof p);
    p.a = a;
    p.errors = slice_nil(TYPE_GO_SCANNER_ERROR_LIST->elem);
    p.comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    p.imports = slice_nil(TYPE_AST_IMPORT_SPEC_PTR);
    AstFile *f = gp_parse_file_guarded(&p, file, text, mode);

    if (!p.oom && f == NULL) {
        /* source is not a valid Go source file - satisfy ParseFile API and
         * return a valid (but) empty *ast.File */
        f = (AstFile *)ast_node_new(a, AST_KIND_FILE);
        if (f != NULL) {
            f->name = (AstIdent *)ast_node_new(a, AST_KIND_IDENT);
            f->scope = ast_new_scope(a, NULL);
            f->decls = slice_nil(TYPE_AST_DECL);
            f->imports = slice_nil(TYPE_AST_IMPORT_SPEC_PTR);
            f->unresolved = slice_nil(TYPE_AST_IDENT_PTR);
            f->comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
        }
        if (f == NULL || f->name == NULL || f->scope == NULL)
            p.oom = true;
    }

    if (f != NULL && !p.oom) {
        f->file_start = (TokenPos)token_file_base(file);
        f->file_end = token_file_end(file);
    }

    Error out = gp_finish(&p);
    if (err != NULL)
        *err = out;
    return p.oom ? NULL : f;
}

/* The part of ParseExprFrom that can bail out. */
static AstExpr gp_parse_expr_guarded(GpParser *p, TokenFile *file, Slice src,
                                     ParserMode mode) {
    AstExpr volatile expr = NULL;
    BURROW_TRY {
        gp_init(p, file, src, mode);
        AstExpr x = gp_parse_rhs(p);

        /* If a semicolon was inserted, consume it; report an error if there's
         * more tokens. */
        if (p->tok == TOKEN_SEMICOLON && str_eq(p->lit, BURROW_S("\n")))
            gp_next(p);
        expr = x;
        gp_expect(p, TOKEN_EOF);
    }
    BURROW_CATCH(r) {
        if (!p->bailing)
            panic(r);
        gp_unwind_trace(p);
    }
    BURROW_TRY_END;
    return expr;
}

AstExpr parser_parse_expr_from(Alloc *a, TokenFileSet *fset, Str filename, Any src,
                               ParserMode mode, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    if (fset == NULL)
        panic_str(
            BURROW_S("parser.ParseExprFrom: no token.FileSet provided (fset == nil)"));

    Error e = BURROW_NO_ERROR;
    Slice text = gp_read_source(a, filename, src, &e);
    if (!BURROW_OK(e)) {
        if (err != NULL)
            *err = e;
        return NULL;
    }

    TokenFile *file = token_file_set_add_file(fset, filename, -1, text.len);
    if (file == NULL) {
        if (err != NULL)
            *err = burrow_err_out_of_memory;
        return NULL;
    }

    GpParser p;
    memset(&p, 0, sizeof p);
    p.a = a;
    p.errors = slice_nil(TYPE_GO_SCANNER_ERROR_LIST->elem);
    p.comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    p.imports = slice_nil(TYPE_AST_IMPORT_SPEC_PTR);
    AstExpr expr = gp_parse_expr_guarded(&p, file, text, mode);

    Error out = gp_finish(&p);
    if (err != NULL)
        *err = out;
    return p.oom ? NULL : expr;
}

AstExpr parser_parse_expr(Alloc *a, Str x, Error *err) {
    TokenFileSet *fset = token_new_file_set(a);
    if (fset == NULL) {
        if (err != NULL)
            *err = burrow_err_out_of_memory;
        return NULL;
    }
    return parser_parse_expr_from(a, fset, BURROW_STR_EMPTY,
                                  BURROW_ANY(TYPE_STRING, &x), 0, err);
}

Map *parser_parse_dir(Alloc *a, TokenFileSet *fset, Str path, ParserFileFilter filter,
                      ParserMode mode, Error *first) {
    if (first != NULL)
        *first = BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    Slice list = os_read_dir(a, path, &err);
    if (!BURROW_OK(err)) {
        if (first != NULL)
            *first = err;
        return NULL;
    }

    Map *pkgs = map_make(a, TYPE_STRING, TYPE_OF(AstPackagePtr), 0);
    if (pkgs == NULL) {
        if (first != NULL)
            *first = burrow_err_out_of_memory;
        return NULL;
    }
    Error first_err = BURROW_NO_ERROR;
    for (Int i = 0; i < list.len; i++) {
        FsDirEntry d = BURROW_AT(FsDirEntry, list, i);
        Str name = d.vt->name(d.data);
        if (d.vt->is_dir(d.data) || !strings_has_suffix(name, BURROW_S(".go")))
            continue;
        if (filter.f != NULL) {
            FsFileInfo info = d.vt->info(d.data, a, &err);
            if (!BURROW_OK(err)) {
                if (first != NULL)
                    *first = err;
                return NULL;
            }
            if (!filter.f(filter.env, info))
                continue;
        }
        Str filename = filepath_join_v(a, 2, path, name);
        Error ferr = BURROW_NO_ERROR;
        AstFile *src =
            parser_parse_file(a, fset, filename, (Any){NULL, NULL}, mode, &ferr);
        if (BURROW_OK(ferr) && src != NULL) {
            Str pname = src->name->name;
            AstPackage **found = (AstPackage **)map_get(pkgs, &pname);
            AstPackage *pkg = found != NULL ? *found : NULL;
            if (pkg == NULL) {
                pkg = (AstPackage *)ast_node_new(a, AST_KIND_PACKAGE);
                if (pkg == NULL) {
                    if (first != NULL)
                        *first = burrow_err_out_of_memory;
                    return NULL;
                }
                pkg->name = pname;
                pkg->files = map_make(a, TYPE_STRING, TYPE_AST_FILE_PTR, 0);
                if (pkg->files == NULL || !map_set(pkgs, &pname, &pkg)) {
                    if (first != NULL)
                        *first = burrow_err_out_of_memory;
                    return NULL;
                }
            }
            if (!map_set(pkg->files, &filename, &src)) {
                if (first != NULL)
                    *first = burrow_err_out_of_memory;
                return NULL;
            }
        } else if (BURROW_OK(first_err)) {
            first_err = BURROW_OK(ferr) ? burrow_err_out_of_memory : ferr;
        }
    }

    if (first != NULL)
        *first = first_err;
    return pkgs;
}
