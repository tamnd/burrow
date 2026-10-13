/* text/template/parse: the parser, which builds trees from the lexer's items.
 *
 * Go's parser reports an error by panicking out of however deep it is and
 * recovering in Tree.Parse. This one does the same with burrow's panic: the
 * value is a private type holding the error, and anything else that is caught
 * is panicked again.
 *
 * Go's goroutine stacks grow, so only parenthesized expressions have a depth
 * limit there. Here the parser also stops with that limit's error when the
 * thread's stack is close to running out, as go/parser does.
 *
 * Derived from Go's src/text/template/parse/parse.go and node.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/panic.h"
#include "burrow/platform.h"
#include "burrow/stack.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/thread.h"
#include "burrow/type.h"
#include "template_parse_internal.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* maxStackDepth: the deepest parenthesized expressions may nest. */
#if defined(BURROW_OS_WASI)
Int burrow__parse_max_stack_depth = 1000;
#else
Int burrow__parse_max_stack_depth = 10000;
#endif

/* The parser's part of a Tree: the fields Go's Tree has for the parse, which
 * live on the C stack for as long as it runs. */
typedef struct TpParser {
    burrow__Lexer *lex;
    burrow__Item token[3]; /* three-token lookahead for parser */
    Int peek_count;
    Slice vars; /* variables defined at the moment */
    Map *tree_set;
    Int action_line; /* line of left delim starting action */
    Int range_depth;
    Int stack_depth; /* depth of nested parenthesized expressions */
    Int action_depth; /* depth of nested actions, for the stack check */
    uintptr_t stack_floor;
} TpParser;

static TpParser *tp_p(ParseTree *t) {
    return (TpParser *)t->burrow__state;
}

/* -------------------------------------------------------------- errors */

typedef struct TpError {
    Error err;
} TpError;

static const Type tp_error_desc = {
    BURROW_S_INIT("parseError"),
    BURROW_S_INIT("text/template/parse"),
    KIND_STRUCT,
    (uint32_t)sizeof(TpError),
    (uint16_t)_Alignof(TpError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

BURROW_NORETURN static void tp_raise(Error err) {
    TpError e = {err};
    panic((Any){&tp_error_desc, &e});
}

BURROW_NORETURN static void tp_oom(void) {
    tp_raise(burrow_err_out_of_memory);
}

/* errorf: formats the error with the template's name and the line of the
 * current token, and stops the parse. msg is the formatted text. */
BURROW_NORETURN static void tp_errorf(ParseTree *t, Str msg) {
    t->root = NULL;
    tp_raise(fmt_errorf_v("template: %s:%d: %s", t->parse_name, tp_p(t)->token[0].line,
                          msg));
}

#define tp_errorf_v(t, ...)                                                            \
    tp_errorf((t), fmt_sprintf_v(error_allocator(), __VA_ARGS__))

/* error: errorf with err's text. */
BURROW_NORETURN static void tp_error(ParseTree *t, Error err) {
    if (errors_is(err, burrow_err_out_of_memory))
        tp_oom();
    tp_errorf(t, error_text(err));
}

static Str tp_item_string(burrow__Item i) {
    return burrow__parse_item_string(i, error_allocator());
}

/* unexpected: complains about token and stops the parse. */
BURROW_NORETURN static void tp_unexpected(ParseTree *t, burrow__Item token,
                                          const char *context) {
    TpParser *p = tp_p(t);
    if (token.typ == BURROW__ITEM_ERROR) {
        Str extra = BURROW_STR_EMPTY;
        if (p->action_line != 0 && p->action_line != token.line) {
            extra = fmt_sprintf_v(error_allocator(), " in action started at %s:%d",
                                  t->parse_name, p->action_line);
            if (strings_has_suffix(token.val, BURROW_S(" action"))) {
                /* avoid "action in action" */
                Int skip = BURROW_S(" in action").len;
                extra = str_from_bytes(extra.p + skip, extra.len - skip);
            }
        }
        tp_errorf_v(t, "%s%s", tp_item_string(token), extra);
    }
    tp_errorf_v(t, "unexpected %s in %s", tp_item_string(token), context);
}

/* ------------------------------------------------------------- tokens */

/* next: the next token. */
static burrow__Item tp_next(ParseTree *t) {
    TpParser *p = tp_p(t);
    if (p->peek_count > 0)
        p->peek_count--;
    else
        p->token[0] = burrow__parse_lex_next_item(p->lex);
    return p->token[p->peek_count];
}

/* backup: backs the input stream up one token. */
static void tp_backup(ParseTree *t) {
    tp_p(t)->peek_count++;
}

/* backup2: backs the input stream up two tokens. The zeroth token is already
 * there. */
static void tp_backup2(ParseTree *t, burrow__Item t1) {
    TpParser *p = tp_p(t);
    p->token[1] = t1;
    p->peek_count = 2;
}

/* backup3: backs the input stream up three tokens. The zeroth token is
 * already there. Reverse order: we're pushing back. */
static void tp_backup3(ParseTree *t, burrow__Item t2, burrow__Item t1) {
    TpParser *p = tp_p(t);
    p->token[1] = t1;
    p->token[2] = t2;
    p->peek_count = 3;
}

/* peek: the next token, without taking it. */
static burrow__Item tp_peek(ParseTree *t) {
    TpParser *p = tp_p(t);
    if (p->peek_count > 0)
        return p->token[p->peek_count - 1];
    p->peek_count = 1;
    p->token[0] = burrow__parse_lex_next_item(p->lex);
    return p->token[0];
}

/* nextNonSpace: the next token that is not a space. */
static burrow__Item tp_next_non_space(ParseTree *t) {
    burrow__Item token;
    for (;;) {
        token = tp_next(t);
        if (token.typ != BURROW__ITEM_SPACE)
            break;
    }
    return token;
}

/* peekNonSpace: the next token that is not a space, without taking it. */
static burrow__Item tp_peek_non_space(ParseTree *t) {
    burrow__Item token = tp_next_non_space(t);
    tp_backup(t);
    return token;
}

/* expect: the next token, which has to be of the expected type. */
static burrow__Item tp_expect(ParseTree *t, burrow__ItemType expected,
                              const char *context) {
    burrow__Item token = tp_next_non_space(t);
    if (token.typ != expected)
        tp_unexpected(t, token, context);
    return token;
}

/* expectOneOf: the next token, which has to be one of two types. */
static burrow__Item tp_expect_one_of(ParseTree *t, burrow__ItemType expected1,
                                     burrow__ItemType expected2, const char *context) {
    burrow__Item token = tp_next_non_space(t);
    if (token.typ != expected1 && token.typ != expected2)
        tp_unexpected(t, token, context);
    return token;
}

/* ---------------------------------------------------------- stack room */

/* How deep the parse goes before it asks the thread for its stack bounds,
 * which is a system call on some systems. */
enum { TP_THREAD_CHECK_AFTER = 32 };

#define TP_STACK_MARGIN ((uintptr_t)64 << 10)

static uintptr_t tp_stack_here(void) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(BURROW_OS_WASI)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    uintptr_t here = (uintptr_t)&probe;
    return here;
#endif
}

/* Whether the stack is too low to go deeper. stack_floor is 0 before the first
 * look, 1 when the bounds cannot be had, and 2 on a thread whose bounds are
 * left until the nesting is deep. */
static bool tp_stack_low(TpParser *p, Int depth) {
#if defined(BURROW_OS_WASI)
    (void)p;
    (void)depth;
    return false;
#else
    if (p->stack_floor == 0) {
        burrow__Stack *s = burrow__stack_current();
        if (s != NULL && s->lo != NULL)
            p->stack_floor = (uintptr_t)s->lo + TP_STACK_MARGIN;
        else
            p->stack_floor = 2;
    }
    if (p->stack_floor == 2) {
        if (depth < TP_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        if (!burrow__thread_stack_limit(&lo))
            lo = NULL;
        p->stack_floor = lo == NULL ? 1 : (uintptr_t)lo + TP_STACK_MARGIN;
    }
    if (p->stack_floor == 1)
        return false;
    return tp_stack_here() < p->stack_floor;
#endif
}

static void tp_check_stack(ParseTree *t, Int depth) {
    if (tp_stack_low(tp_p(t), depth))
        tp_errorf(t, BURROW_S("max expression depth exceeded"));
}

/* -------------------------------------------------------------- nodes */

static void *tp_new_node(ParseTree *t, size_t size, ParseNodeType typ, ParsePos pos) {
    void *n = burrow__parse_node_new(t->a, size, typ, pos, t);
    if (n == NULL)
        tp_oom();
    return n;
}

#define TP_NEW(t, T, typ, pos) ((T *)tp_new_node((t), sizeof(T), (typ), (pos)))

static void tp_append(ParseTree *t, Slice *s, const Type *elem_type, const void *elem) {
    if (!burrow__parse_append(t->a, s, elem_type, elem))
        tp_oom();
}

static void tp_append_node(ParseTree *t, Slice *s, ParseNode n) {
    tp_append(t, s, TYPE_UNSAFE_POINTER, &n);
}

static ParseNode tp_node(void *n) {
    return (ParseNode)n;
}

static ParseListNode *tp_new_list(ParseTree *t, ParsePos pos) {
    return TP_NEW(t, ParseListNode, PARSE_NODE_LIST, pos);
}

static ParseNode tp_new_text(ParseTree *t, ParsePos pos, Str text) {
    ParseTextNode *n = TP_NEW(t, ParseTextNode, PARSE_NODE_TEXT, pos);
    n->text = slice_from_str(t->a, text);
    if (n->text.p == NULL && text.len > 0)
        tp_oom();
    return tp_node(n);
}

static ParseNode tp_new_comment(ParseTree *t, ParsePos pos, Str text) {
    ParseCommentNode *n = TP_NEW(t, ParseCommentNode, PARSE_NODE_COMMENT, pos);
    n->text = text;
    return tp_node(n);
}

static ParsePipeNode *tp_new_pipeline(ParseTree *t, ParsePos pos, Int line) {
    ParsePipeNode *n = TP_NEW(t, ParsePipeNode, PARSE_NODE_PIPE, pos);
    n->line = line;
    return n;
}

static ParseNode tp_new_action(ParseTree *t, ParsePos pos, Int line,
                               ParsePipeNode *pipe) {
    ParseActionNode *n = TP_NEW(t, ParseActionNode, PARSE_NODE_ACTION, pos);
    n->line = line;
    n->pipe = pipe;
    return tp_node(n);
}

static ParseCommandNode *tp_new_command(ParseTree *t, ParsePos pos) {
    return TP_NEW(t, ParseCommandNode, PARSE_NODE_COMMAND, pos);
}

static Slice tp_split(ParseTree *t, Str s) {
    Slice parts = strings_split(t->a, s, BURROW_S("."));
    if (parts.p == NULL)
        tp_oom();
    return parts;
}

static ParseVariableNode *tp_new_variable(ParseTree *t, ParsePos pos, Str ident) {
    ParseVariableNode *n = TP_NEW(t, ParseVariableNode, PARSE_NODE_VARIABLE, pos);
    n->ident = tp_split(t, ident);
    return n;
}

static ParseNode tp_new_dot(ParseTree *t, ParsePos pos) {
    return tp_node(TP_NEW(t, ParseDotNode, PARSE_NODE_DOT, pos));
}

static ParseNode tp_new_nil(ParseTree *t, ParsePos pos) {
    return tp_node(TP_NEW(t, ParseNilNode, PARSE_NODE_NIL, pos));
}

static ParseNode tp_new_field(ParseTree *t, ParsePos pos, Str ident) {
    ParseFieldNode *n = TP_NEW(t, ParseFieldNode, PARSE_NODE_FIELD, pos);
    /* [1:] to drop leading period */
    n->ident = tp_split(t, str_from_bytes(ident.p + 1, ident.len - 1));
    return tp_node(n);
}

static ParseChainNode *tp_new_chain(ParseTree *t, ParsePos pos, ParseNode node) {
    ParseChainNode *n = TP_NEW(t, ParseChainNode, PARSE_NODE_CHAIN, pos);
    n->node = node;
    return n;
}

static ParseNode tp_new_bool(ParseTree *t, ParsePos pos, bool true_) {
    ParseBoolNode *n = TP_NEW(t, ParseBoolNode, PARSE_NODE_BOOL, pos);
    n->true_ = true_;
    return tp_node(n);
}

static ParseNode tp_new_string(ParseTree *t, ParsePos pos, Str orig, Str text) {
    ParseStringNode *n = TP_NEW(t, ParseStringNode, PARSE_NODE_STRING, pos);
    n->quoted = orig;
    n->text = text;
    return tp_node(n);
}

static ParseNode tp_new_end(ParseTree *t, ParsePos pos) {
    return tp_node(TP_NEW(t, burrow__ParseEndNode, BURROW__PARSE_NODE_END, pos));
}

static ParseNode tp_new_else(ParseTree *t, ParsePos pos, Int line) {
    burrow__ParseElseNode *n =
        TP_NEW(t, burrow__ParseElseNode, BURROW__PARSE_NODE_ELSE, pos);
    n->line = line;
    return tp_node(n);
}

/* What parseControl gives newIf, newRange and newWith. */
typedef struct TpControl {
    ParsePos pos;
    Int line;
    ParsePipeNode *pipe;
    ParseListNode *list;
    ParseListNode *else_list;
} TpControl;

static ParseNode tp_new_branch(ParseTree *t, ParseNodeType typ, TpControl c) {
    ParseBranchNode *n = TP_NEW(t, ParseBranchNode, typ, c.pos);
    n->line = c.line;
    n->pipe = c.pipe;
    n->list = c.list;
    n->else_list = c.else_list;
    return &n->node_type;
}

static ParseNode tp_new_break(ParseTree *t, ParsePos pos, Int line) {
    ParseBreakNode *n = TP_NEW(t, ParseBreakNode, PARSE_NODE_BREAK, pos);
    n->line = line;
    return tp_node(n);
}

static ParseNode tp_new_continue(ParseTree *t, ParsePos pos, Int line) {
    ParseContinueNode *n = TP_NEW(t, ParseContinueNode, PARSE_NODE_CONTINUE, pos);
    n->line = line;
    return tp_node(n);
}

static ParseNode tp_new_template(ParseTree *t, ParsePos pos, Int line, Str name,
                                 ParsePipeNode *pipe) {
    ParseTemplateNode *n = TP_NEW(t, ParseTemplateNode, PARSE_NODE_TEMPLATE, pos);
    n->line = line;
    n->name = name;
    n->pipe = pipe;
    return tp_node(n);
}

/* ------------------------------------------------------------- numbers */

/* float64(int64(f)) == f, without C's undefined conversion when f is out of
 * range, where Go's conversion gives a value that does not compare equal. */
static bool tp_float_is_int64(double f, int64_t *out) {
    if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0))
        return false;
    int64_t i = (int64_t)f;
    if ((double)i != f)
        return false;
    *out = i;
    return true;
}

/* float64(uint64(f)) == f, the same way. */
static bool tp_float_is_uint64(double f, uint64_t *out) {
    if (!(f > -1.0 && f < 18446744073709551616.0))
        return false;
    uint64_t u = (uint64_t)f;
    if ((double)u != f)
        return false;
    *out = u;
    return true;
}

/* simplifyComplex: pulls out any other types that are represented by the
 * complex number. These all require that the imaginary part be zero. */
static void tp_simplify_complex(ParseNumberNode *n) {
    n->is_float = n->complex128.im == 0;
    if (n->is_float) {
        n->float64 = n->complex128.re;
        n->is_int = tp_float_is_int64(n->float64, &n->int64);
        n->is_uint = tp_float_is_uint64(n->float64, &n->uint64);
    }
}

ParseNumberNode *burrow__parse_new_number(ParseTree *t, ParsePos pos, Str text,
                                          burrow__ItemType typ, Error *err) {
    *err = BURROW_NO_ERROR;
    ParseNumberNode *n = (ParseNumberNode *)burrow__parse_node_new(
        t->a, sizeof(ParseNumberNode), PARSE_NODE_NUMBER, pos, t);
    if (n == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    n->text = text;
    if (typ == BURROW__ITEM_CHAR_CONSTANT) {
        /* fmt.Sscan can't handle this, and it's not all that simple to do
         * from the text. */
        bool multibyte = false;
        Str tail = BURROW_STR_EMPTY;
        Error e = BURROW_NO_ERROR;
        Rune r = strconv_unquote_char(str_from_bytes(text.p + 1, text.len - 1),
                                      text.p[0], &multibyte, &tail, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }
        if (!str_eq(tail, BURROW_S("'"))) {
            *err = fmt_errorf_v("malformed character constant: %s", text);
            return NULL;
        }
        n->int64 = (int64_t)r;
        n->is_int = true;
        n->uint64 = (uint64_t)r;
        n->is_uint = true;
        n->float64 = (double)r; /* odd but those are the rules. */
        n->is_float = true;
        return n;
    }
    if (typ == BURROW__ITEM_COMPLEX) {
        /* fmt.Sscan can handle this. */
        Error e = BURROW_NO_ERROR;
        fmt_sscan_v(t->a, &e, text, &n->complex128);
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }
        n->is_complex = true;
        tp_simplify_complex(n);
        return n;
    }
    /* Imaginary constants can only be complex unless they are zero. */
    if (text.len > 0 && text.p[text.len - 1] == 'i') {
        Error e = BURROW_NO_ERROR;
        double f = strconv_parse_float(str_from_bytes(text.p, text.len - 1), 64, &e);
        if (BURROW_OK(e)) {
            n->is_complex = true;
            n->complex128 = (Complex128){0, f};
            tp_simplify_complex(n);
            return n;
        }
    }
    /* Do integer test first so we get 0x123 etc. */
    Error e = BURROW_NO_ERROR;
    uint64_t u =
        strconv_parse_uint(text, 0, 64, &e); /* will fail for -0; fixed below. */
    if (BURROW_OK(e)) {
        n->is_uint = true;
        n->uint64 = u;
    }
    e = BURROW_NO_ERROR;
    int64_t i = strconv_parse_int(text, 0, 64, &e);
    if (BURROW_OK(e)) {
        n->is_int = true;
        n->int64 = i;
        if (i == 0) {
            n->is_uint = true; /* in case of -0. */
            n->uint64 = u;
        }
    }
    /* If an integer extraction succeeded, promote the float. */
    if (n->is_int) {
        n->is_float = true;
        n->float64 = (double)n->int64;
    } else if (n->is_uint) {
        n->is_float = true;
        n->float64 = (double)n->uint64;
    } else {
        e = BURROW_NO_ERROR;
        double f = strconv_parse_float(text, 64, &e);
        if (BURROW_OK(e)) {
            /* If we parsed it as a float but it looks like an integer, it's a
             * huge number too large to fit in an int. Reject it. */
            if (!strings_contains_any(text, BURROW_S(".eEpP"))) {
                *err = fmt_errorf_v("integer overflow: %q", text);
                return NULL;
            }
            n->is_float = true;
            n->float64 = f;
            /* If a floating-point extraction succeeded, extract the int if
             * needed. */
            if (!n->is_int)
                n->is_int = tp_float_is_int64(f, &n->int64);
            if (!n->is_uint)
                n->is_uint = tp_float_is_uint64(f, &n->uint64);
        }
    }
    if (!n->is_int && !n->is_uint && !n->is_float) {
        *err = fmt_errorf_v("illegal number syntax: %q", text);
        return NULL;
    }
    return n;
}

/* --------------------------------------------------------------- trees */

ParseTree *parse_new(Alloc *a, Str name, Slice funcs) {
    ParseTree *t = BURROW_NEW(a, ParseTree);
    if (t == NULL)
        return NULL;
    t->a = a;
    if (name.len > 0) {
        t->name = str_clone(a, name);
        if (t->name.len == 0)
            return NULL;
    }
    t->funcs = funcs;
    return t;
}

ParseTree *parse_tree_copy(ParseTree *t, Alloc *a) {
    if (t == NULL)
        return NULL;
    ParseTree *c = BURROW_NEW(a, ParseTree);
    if (c == NULL)
        return NULL;
    c->name = t->name;
    c->parse_name = t->parse_name;
    c->root = parse_list_node_copy_list(t->root, a);
    if (c->root == NULL && t->root != NULL)
        return NULL;
    c->a = a;
    c->text = t->text;
    c->left_delim = t->left_delim;
    c->right_delim = t->right_delim;
    return c;
}

Str parse_tree_error_context(ParseTree *t, Alloc *a, ParseNode n, Str *context) {
    Int pos = parse_node_position(n);
    ParseTree *tree = burrow__parse_node_tree(n);
    if (tree == NULL)
        tree = t;
    if (tree == NULL)
        burrow__parse_nil_deref();
    if (pos < 0 || pos > tree->text.len)
        runtime_panic(BURROW_S("runtime error: slice bounds out of range"));
    Str text = str_from_bytes(tree->text.p, pos);
    Int byte_num = strings_last_index(text, BURROW_S("\n"));
    if (byte_num == -1) {
        byte_num = pos; /* On first line. */
    } else {
        byte_num++; /* After the newline. */
        byte_num = pos - byte_num;
    }
    Int line_num = 1 + strings_count(text, BURROW_S("\n"));
    *context = parse_node_string(n, a);
    return fmt_sprintf_v(a, "%s:%d:%d", tree->parse_name, line_num, byte_num);
}

/* hasFunction: whether a function is defined with this name. */
static bool tp_has_function(ParseTree *t, Str name) {
    for (Int i = 0; i < t->funcs.len; i++) {
        Map *func_map = BURROW_AT(Map *, t->funcs, i);
        if (func_map == NULL)
            continue;
        Any *v = (Any *)map_get(func_map, &name);
        if (v != NULL && v->t != NULL)
            return true;
    }
    return false;
}

/* startParse: initializes the parser, using the lexer. */
static void tp_start_parse(ParseTree *t, TpParser *p, Slice funcs, burrow__Lexer *lex,
                           Map *tree_set) {
    memset(p, 0, sizeof *p);
    t->root = NULL;
    t->burrow__state = p;
    p->lex = lex;
    Str dollar = BURROW_S("$");
    tp_append(t, &p->vars, TYPE_STRING, &dollar);
    t->funcs = funcs;
    p->tree_set = tree_set;
    lex->options = (burrow__LexOptions){
        .emit_comment = (t->mode & PARSE_PARSE_COMMENTS) != 0,
        .break_ok = !tp_has_function(t, BURROW_S("break")),
        .continue_ok = !tp_has_function(t, BURROW_S("continue")),
    };
}

/* stopParse: ends parsing. */
static void tp_stop_parse(ParseTree *t) {
    t->burrow__state = NULL;
    t->funcs = (Slice){0};
}

bool parse_is_empty_tree(ParseNode n) {
    if (n == NULL)
        return true;
    switch (*n) {
    case PARSE_NODE_ACTION:
    case PARSE_NODE_IF:
    case PARSE_NODE_RANGE:
    case PARSE_NODE_TEMPLATE:
    case PARSE_NODE_WITH:
        return false;
    case PARSE_NODE_COMMENT:
        return true;
    case PARSE_NODE_LIST: {
        ParseListNode *l = (ParseListNode *)n;
        for (Int i = 0; i < l->nodes.len; i++)
            if (!parse_is_empty_tree(BURROW_AT(ParseNode, l->nodes, i)))
                return false;
        return true;
    }
    case PARSE_NODE_TEXT:
        return bytes_trim_space(((ParseTextNode *)n)->text).len == 0;
    default: {
        Str s = parse_node_string(n, error_allocator());
        panic_str(fmt_sprintf_v(error_allocator(), "unknown node: %s", s));
    }
    }
}

/* add: adds tree to t's tree set. */
static void tp_add(ParseTree *t) {
    Map *tree_set = tp_p(t)->tree_set;
    ParseTree **tree = (ParseTree **)map_get(tree_set, &t->name);
    if (tree == NULL || *tree == NULL || parse_is_empty_tree(tp_node((*tree)->root))) {
        if (!map_set(tree_set, &t->name, &t))
            tp_oom();
        return;
    }
    if (!parse_is_empty_tree(tp_node(t->root)))
        tp_errorf_v(t, "template: multiple definition of template %q", t->name);
}

static ParseNode tp_text_or_action(ParseTree *t);
static ParseNode tp_if_control(ParseTree *t);
static ParseNode tp_with_control(ParseTree *t);
static ParsePipeNode *tp_pipeline(ParseTree *t, const char *context,
                                  burrow__ItemType end);

/* A tree for a define or block, which shares the parse with t. */
static ParseTree *tp_new_sub_tree(ParseTree *t, TpParser *p, Str name) {
    ParseTree *n = parse_new(t->a, name, (Slice){0});
    if (n == NULL)
        tp_oom();
    n->text = t->text;
    n->mode = t->mode;
    n->left_delim = t->left_delim;
    n->right_delim = t->right_delim;
    n->parse_name = t->parse_name;
    tp_start_parse(n, p, t->funcs, tp_p(t)->lex, tp_p(t)->tree_set);
    return n;
}

/* itemList:
 *
 *     textOrAction*
 *
 * Terminates at {{end}} or {{else}}, returned separately. */
static ParseListNode *tp_item_list(ParseTree *t, ParseNode *next) {
    ParseListNode *list = tp_new_list(t, tp_peek_non_space(t).pos);
    while (tp_peek_non_space(t).typ != BURROW__ITEM_EOF) {
        ParseNode n = tp_text_or_action(t);
        switch (*n) {
        case BURROW__PARSE_NODE_END:
        case BURROW__PARSE_NODE_ELSE:
            *next = n;
            return list;
        default:
            break;
        }
        tp_append_node(t, &list->nodes, n);
    }
    tp_errorf(t, BURROW_S("unexpected EOF"));
}

/* parseDefinition parses a {{define}} ...  {{end}} template definition and
 * installs the definition in t's tree set. The "define" keyword has already
 * been scanned. */
static void tp_parse_definition(ParseTree *t) {
    static const char context[] = "define clause";
    burrow__Item name =
        tp_expect_one_of(t, BURROW__ITEM_STRING, BURROW__ITEM_RAW_STRING, context);
    Error err = BURROW_NO_ERROR;
    t->name = strconv_unquote(t->a, name.val, &err);
    if (BURROW_FAILED(err))
        tp_error(t, err);
    tp_expect(t, BURROW__ITEM_RIGHT_DELIM, context);
    ParseNode end = NULL;
    t->root = tp_item_list(t, &end);
    if (*end != BURROW__PARSE_NODE_END)
        tp_errorf_v(t, "unexpected %s in %s", parse_node_string(end, error_allocator()),
                    context);
    tp_add(t);
    tp_stop_parse(t);
}

/* parse is the top-level parser for a template, essentially the same as
 * itemList except it also parses {{define}} actions. It runs to EOF. */
static void tp_parse(ParseTree *t) {
    t->root = tp_new_list(t, tp_peek(t).pos);
    while (tp_peek(t).typ != BURROW__ITEM_EOF) {
        if (tp_peek(t).typ == BURROW__ITEM_LEFT_DELIM) {
            burrow__Item delim = tp_next(t);
            if (tp_next_non_space(t).typ == BURROW__ITEM_DEFINE) {
                TpParser p;
                /* name will be updated once we know it. */
                ParseTree *new_t = tp_new_sub_tree(t, &p, BURROW_S("definition"));
                tp_parse_definition(new_t);
                continue;
            }
            tp_backup2(t, delim);
        }
        ParseNode n = tp_text_or_action(t);
        switch (*n) {
        case BURROW__PARSE_NODE_END:
        case BURROW__PARSE_NODE_ELSE:
            tp_errorf_v(t, "unexpected %s", parse_node_string(n, error_allocator()));
        default:
            tp_append_node(t, &t->root->nodes, n);
        }
    }
}

/* Break:
 *
 *     {{break}}
 *
 * Break keyword is past. */
static ParseNode tp_break_control(ParseTree *t, ParsePos pos, Int line) {
    burrow__Item token = tp_next_non_space(t);
    if (token.typ != BURROW__ITEM_RIGHT_DELIM)
        tp_unexpected(t, token, "{{break}}");
    if (tp_p(t)->range_depth == 0)
        tp_errorf(t, BURROW_S("{{break}} outside {{range}}"));
    return tp_new_break(t, pos, line);
}

/* Continue:
 *
 *     {{continue}}
 *
 * Continue keyword is past. */
static ParseNode tp_continue_control(ParseTree *t, ParsePos pos, Int line) {
    burrow__Item token = tp_next_non_space(t);
    if (token.typ != BURROW__ITEM_RIGHT_DELIM)
        tp_unexpected(t, token, "{{continue}}");
    if (tp_p(t)->range_depth == 0)
        tp_errorf(t, BURROW_S("{{continue}} outside {{range}}"));
    return tp_new_continue(t, pos, line);
}

static void tp_check_pipeline(ParseTree *t, ParsePipeNode *pipe, const char *context) {
    /* Reject empty pipelines */
    if (pipe->cmds.len == 0)
        tp_errorf_v(t, "missing value for %s", context);
    /* Only the first command of a pipeline can start with a non executable
     * operand */
    for (Int i = 1; i < pipe->cmds.len; i++) {
        ParseCommandNode *c = (ParseCommandNode *)BURROW_AT(ParseNode, pipe->cmds, i);
        switch (*BURROW_AT(ParseNode, c->args, 0)) {
        case PARSE_NODE_BOOL:
        case PARSE_NODE_DOT:
        case PARSE_NODE_NIL:
        case PARSE_NODE_NUMBER:
        case PARSE_NODE_STRING:
            /* With A|B|C, pipeline stage 2 is B */
            tp_errorf_v(t, "non executable command in pipeline stage %d", i + 1);
        default:
            break;
        }
    }
}

/* parseControl: the pipeline and lists of an if, range or with. */
static TpControl tp_parse_control(ParseTree *t, const char *context) {
    TpParser *p = tp_p(t);
    Int nvars = p->vars.len;
    TpControl c = {0, 0, NULL, NULL, NULL};
    bool is_range = strcmp(context, "range") == 0;
    c.pipe = tp_pipeline(t, context, BURROW__ITEM_RIGHT_DELIM);
    if (is_range)
        p->range_depth++;
    ParseNode next = NULL;
    c.list = tp_item_list(t, &next);
    if (is_range)
        p->range_depth--;
    switch (*next) {
    case BURROW__PARSE_NODE_END: /* done */
        break;
    case BURROW__PARSE_NODE_ELSE:
        /* Special case for "else if" and "else with". If the "else" is
         * followed immediately by an "if" or "with", the elseControl will have
         * left the "if" or "with" token pending. Treat
         *     {{if a}}_{{else if b}}_{{end}}
         *     {{with a}}_{{else with b}}_{{end}}
         * as
         *     {{if a}}_{{else}}{{if b}}_{{end}}{{end}}
         *     {{with a}}_{{else}}{{with b}}_{{end}}{{end}}.
         * To do this, parse the "if" or "with" as usual and stop at it
         * {{end}}; the subsequent{{end}} is assumed. This technique works
         * even for long if-else-if chains. */
        if (strcmp(context, "if") == 0 && tp_peek(t).typ == BURROW__ITEM_IF) {
            tp_next(t); /* Consume the "if" token. */
            c.else_list = tp_new_list(t, parse_node_position(next));
            tp_append_node(t, &c.else_list->nodes, tp_if_control(t));
        } else if (strcmp(context, "with") == 0 &&
                   tp_peek(t).typ == BURROW__ITEM_WITH) {
            tp_next(t);
            c.else_list = tp_new_list(t, parse_node_position(next));
            tp_append_node(t, &c.else_list->nodes, tp_with_control(t));
        } else {
            c.else_list = tp_item_list(t, &next);
            if (*next != BURROW__PARSE_NODE_END)
                tp_errorf_v(t, "expected end; found %s",
                            parse_node_string(next, error_allocator()));
        }
        break;
    default:
        break;
    }
    c.pos = c.pipe->pos;
    c.line = c.pipe->line;
    p->vars.len = nvars; /* popVars */
    return c;
}

/* If:
 *
 *     {{if pipeline}} itemList {{end}}
 *     {{if pipeline}} itemList {{else}} itemList {{end}}
 *
 * If keyword is past. */
static ParseNode tp_if_control(ParseTree *t) {
    return tp_new_branch(t, PARSE_NODE_IF, tp_parse_control(t, "if"));
}

/* Range:
 *
 *     {{range pipeline}} itemList {{end}}
 *     {{range pipeline}} itemList {{else}} itemList {{end}}
 *
 * Range keyword is past. */
static ParseNode tp_range_control(ParseTree *t) {
    return tp_new_branch(t, PARSE_NODE_RANGE, tp_parse_control(t, "range"));
}

/* With:
 *
 *     {{with pipeline}} itemList {{end}}
 *     {{with pipeline}} itemList {{else}} itemList {{end}}
 *
 * If keyword is past. */
static ParseNode tp_with_control(ParseTree *t) {
    return tp_new_branch(t, PARSE_NODE_WITH, tp_parse_control(t, "with"));
}

/* End:
 *
 *     {{end}}
 *
 * End keyword is past. */
static ParseNode tp_end_control(ParseTree *t) {
    return tp_new_end(t, tp_expect(t, BURROW__ITEM_RIGHT_DELIM, "end").pos);
}

/* Else:
 *
 *     {{else}}
 *
 * Else keyword is past. */
static ParseNode tp_else_control(ParseTree *t) {
    burrow__Item peek = tp_peek_non_space(t);
    /* The "{{else if ... " and "{{else with ..." will be treated as "{{else}}{{if
     * ..." and "{{else}}{{with ...". So return the else node here. */
    if (peek.typ == BURROW__ITEM_IF || peek.typ == BURROW__ITEM_WITH)
        return tp_new_else(t, peek.pos, peek.line);
    burrow__Item token = tp_expect(t, BURROW__ITEM_RIGHT_DELIM, "else");
    return tp_new_else(t, token.pos, token.line);
}

static Str tp_parse_template_name(ParseTree *t, burrow__Item token,
                                  const char *context) {
    Str name = BURROW_STR_EMPTY;
    switch ((int)token.typ) {
    case BURROW__ITEM_STRING:
    case BURROW__ITEM_RAW_STRING: {
        Error err = BURROW_NO_ERROR;
        name = strconv_unquote(t->a, token.val, &err);
        if (BURROW_FAILED(err))
            tp_error(t, err);
        break;
    }
    default:
        tp_unexpected(t, token, context);
    }
    return name;
}

/* Block:
 *
 *     {{block stringValue pipeline}}
 *
 * Block keyword is past. The name must be something that can evaluate to a
 * string. The pipeline is mandatory. */
static ParseNode tp_block_control(ParseTree *t) {
    static const char context[] = "block clause";

    burrow__Item token = tp_next_non_space(t);
    Str name = tp_parse_template_name(t, token, context);
    ParsePipeNode *pipe = tp_pipeline(t, context, BURROW__ITEM_RIGHT_DELIM);

    TpParser p;
    ParseTree *block =
        tp_new_sub_tree(t, &p, name); /* name will be updated once we know it. */
    ParseNode end = NULL;
    block->root = tp_item_list(block, &end);
    if (*end != BURROW__PARSE_NODE_END)
        tp_errorf_v(t, "unexpected %s in %s", parse_node_string(end, error_allocator()),
                    context);
    tp_add(block);
    tp_stop_parse(block);

    return tp_new_template(t, token.pos, token.line, name, pipe);
}

/* Template:
 *
 *     {{template stringValue pipeline}}
 *
 * Template keyword is past. The name must be something that can evaluate to a
 * string. */
static ParseNode tp_template_control(ParseTree *t) {
    static const char context[] = "template clause";
    burrow__Item token = tp_next_non_space(t);
    Str name = tp_parse_template_name(t, token, context);
    ParsePipeNode *pipe = NULL;
    if (tp_next_non_space(t).typ != BURROW__ITEM_RIGHT_DELIM) {
        tp_backup(t);
        /* Do not pop variables; they persist until "end". */
        pipe = tp_pipeline(t, context, BURROW__ITEM_RIGHT_DELIM);
    }
    return tp_new_template(t, token.pos, token.line, name, pipe);
}

/* Action:
 *
 *     control
 *     command ("|" command)*
 *
 * Left delim is past. Now get actions. First word could be a keyword such as
 * range. */
static ParseNode tp_action(ParseTree *t) {
    burrow__Item token = tp_next_non_space(t);
    switch ((int)token.typ) {
    case BURROW__ITEM_BLOCK:
        return tp_block_control(t);
    case BURROW__ITEM_BREAK:
        return tp_break_control(t, token.pos, token.line);
    case BURROW__ITEM_CONTINUE:
        return tp_continue_control(t, token.pos, token.line);
    case BURROW__ITEM_ELSE:
        return tp_else_control(t);
    case BURROW__ITEM_END:
        return tp_end_control(t);
    case BURROW__ITEM_IF:
        return tp_if_control(t);
    case BURROW__ITEM_RANGE:
        return tp_range_control(t);
    case BURROW__ITEM_TEMPLATE:
        return tp_template_control(t);
    case BURROW__ITEM_WITH:
        return tp_with_control(t);
    default:
        break;
    }
    tp_backup(t);
    token = tp_peek(t);
    /* Do not pop variables; they persist until "end". */
    return tp_new_action(t, token.pos, token.line,
                         tp_pipeline(t, "command", BURROW__ITEM_RIGHT_DELIM));
}

/* textOrAction:
 *
 *     text | comment | action */
static ParseNode tp_text_or_action(ParseTree *t) {
    burrow__Item token = tp_next_non_space(t);
    switch ((int)token.typ) {
    case BURROW__ITEM_TEXT:
        return tp_new_text(t, token.pos, token.val);
    case BURROW__ITEM_LEFT_DELIM: {
        TpParser *p = tp_p(t);
        tp_check_stack(t, p->action_depth + p->stack_depth);
        p->action_depth++;
        p->action_line = token.line;
        ParseNode n = tp_action(t);
        p->action_line = 0; /* clearActionLine */
        p->action_depth--;
        return n;
    }
    case BURROW__ITEM_COMMENT:
        return tp_new_comment(t, token.pos, token.val);
    default:
        tp_unexpected(t, token, "input");
    }
}

static ParseCommandNode *tp_command(ParseTree *t);

/* Pipeline:
 *
 *     declarations? command ('|' command)*
 */
static ParsePipeNode *tp_pipeline(ParseTree *t, const char *context,
                                  burrow__ItemType end) {
    TpParser *p = tp_p(t);
    burrow__Item token = tp_peek_non_space(t);
    ParsePipeNode *pipe = tp_new_pipeline(t, token.pos, token.line);
    /* Are there declarations or assignments? */
    for (;;) {
        burrow__Item v = tp_peek_non_space(t);
        if (v.typ != BURROW__ITEM_VARIABLE)
            break;
        tp_next(t);
        /* Since space is a token, we need 3-token look-ahead here in the
         * worst case: in "$x foo" we need to read "foo" (as opposed to ":=")
         * to know that $x is an argument variable rather than a declaration.
         * So remember the token adjacent to the variable so we can push it
         * back if necessary. */
        burrow__Item token_after_variable = tp_peek(t);
        burrow__Item next = tp_peek_non_space(t);
        if (next.typ == BURROW__ITEM_ASSIGN || next.typ == BURROW__ITEM_DECLARE) {
            pipe->is_assign = next.typ == BURROW__ITEM_ASSIGN;
            tp_next_non_space(t);
            tp_append_node(t, &pipe->decl, tp_node(tp_new_variable(t, v.pos, v.val)));
            tp_append(t, &p->vars, TYPE_STRING, &v.val);
        } else if (next.typ == BURROW__ITEM_CHAR && str_eq(next.val, BURROW_S(","))) {
            tp_next_non_space(t);
            tp_append_node(t, &pipe->decl, tp_node(tp_new_variable(t, v.pos, v.val)));
            tp_append(t, &p->vars, TYPE_STRING, &v.val);
            if (strcmp(context, "range") == 0 && pipe->decl.len < 2) {
                switch ((int)tp_peek_non_space(t).typ) {
                case BURROW__ITEM_VARIABLE:
                case BURROW__ITEM_RIGHT_DELIM:
                case BURROW__ITEM_RIGHT_PAREN:
                    /* second initialized variable in a range pipeline */
                    continue;
                default:
                    tp_errorf(t, BURROW_S("range can only initialize variables"));
                }
            }
            tp_errorf_v(t, "too many declarations in %s", context);
        } else if (token_after_variable.typ == BURROW__ITEM_SPACE) {
            tp_backup3(t, v, token_after_variable);
        } else {
            tp_backup2(t, v);
        }
        break;
    }
    for (;;) {
        burrow__Item tok = tp_next_non_space(t);
        if (tok.typ == end) {
            /* At this point, the pipeline is complete */
            tp_check_pipeline(t, pipe, context);
            return pipe;
        }
        switch ((int)tok.typ) {
        case BURROW__ITEM_BOOL:
        case BURROW__ITEM_CHAR_CONSTANT:
        case BURROW__ITEM_COMPLEX:
        case BURROW__ITEM_DOT:
        case BURROW__ITEM_FIELD:
        case BURROW__ITEM_IDENTIFIER:
        case BURROW__ITEM_NUMBER:
        case BURROW__ITEM_NIL:
        case BURROW__ITEM_RAW_STRING:
        case BURROW__ITEM_STRING:
        case BURROW__ITEM_VARIABLE:
        case BURROW__ITEM_LEFT_PAREN:
            tp_backup(t);
            tp_append_node(t, &pipe->cmds, tp_node(tp_command(t)));
            break;
        default:
            tp_unexpected(t, tok, context);
        }
    }
}

/* useVar: a node for a variable reference. It errors if the variable is not
 * defined. */
static ParseNode tp_use_var(ParseTree *t, ParsePos pos, Str name) {
    TpParser *p = tp_p(t);
    ParseVariableNode *v = tp_new_variable(t, pos, name);
    Str first = BURROW_AT(Str, v->ident, 0);
    for (Int i = 0; i < p->vars.len; i++)
        if (str_eq(BURROW_AT(Str, p->vars, i), first))
            return tp_node(v);
    tp_errorf_v(t, "undefined variable %q", first);
}

/* term:
 *
 *     literal (number, string, nil, boolean)
 *     function (identifier)
 *     .
 *     .Field
 *     $
 *     '(' pipeline ')'
 *
 * A term is a simple "expression". A nil return means the next item is not a
 * term. */
static ParseNode tp_term(ParseTree *t) {
    burrow__Item token = tp_next_non_space(t);
    switch ((int)token.typ) {
    case BURROW__ITEM_IDENTIFIER: {
        bool check_func = (t->mode & PARSE_SKIP_FUNC_CHECK) == 0;
        if (check_func && !tp_has_function(t, token.val))
            tp_errorf_v(t, "function %q not defined", token.val);
        ParseIdentifierNode *n = parse_new_identifier(t->a, token.val);
        if (n == NULL)
            tp_oom();
        return tp_node(parse_identifier_node_set_pos(
            parse_identifier_node_set_tree(n, t), token.pos));
    }
    case BURROW__ITEM_DOT:
        return tp_new_dot(t, token.pos);
    case BURROW__ITEM_NIL:
        return tp_new_nil(t, token.pos);
    case BURROW__ITEM_VARIABLE:
        return tp_use_var(t, token.pos, token.val);
    case BURROW__ITEM_FIELD:
        return tp_new_field(t, token.pos, token.val);
    case BURROW__ITEM_BOOL:
        return tp_new_bool(t, token.pos, str_eq(token.val, BURROW_S("true")));
    case BURROW__ITEM_CHAR_CONSTANT:
    case BURROW__ITEM_COMPLEX:
    case BURROW__ITEM_NUMBER: {
        Error err = BURROW_NO_ERROR;
        ParseNumberNode *number =
            burrow__parse_new_number(t, token.pos, token.val, token.typ, &err);
        if (BURROW_FAILED(err))
            tp_error(t, err);
        return tp_node(number);
    }
    case BURROW__ITEM_LEFT_PAREN: {
        TpParser *p = tp_p(t);
        if (p->stack_depth >= burrow__parse_max_stack_depth)
            tp_errorf(t, BURROW_S("max expression depth exceeded"));
        tp_check_stack(t, p->action_depth + p->stack_depth);
        p->stack_depth++;
        ParsePipeNode *pipe =
            tp_pipeline(t, "parenthesized pipeline", BURROW__ITEM_RIGHT_PAREN);
        p->stack_depth--;
        return tp_node(pipe);
    }
    case BURROW__ITEM_STRING:
    case BURROW__ITEM_RAW_STRING: {
        Error err = BURROW_NO_ERROR;
        Str s = strconv_unquote(t->a, token.val, &err);
        if (BURROW_FAILED(err))
            tp_error(t, err);
        return tp_new_string(t, token.pos, token.val, s);
    }
    default:
        break;
    }
    tp_backup(t);
    return NULL;
}

/* operand:
 *
 *     term .Field*
 *
 * An operand is a space-separated component of a command, a term possibly
 * followed by field accesses. A nil return means the next item is not an
 * operand. */
static ParseNode tp_operand(ParseTree *t) {
    ParseNode node = tp_term(t);
    if (node == NULL)
        return NULL;
    if (tp_peek(t).typ == BURROW__ITEM_FIELD) {
        ParseChainNode *chain = tp_new_chain(t, tp_peek(t).pos, node);
        while (tp_peek(t).typ == BURROW__ITEM_FIELD)
            if (!parse_chain_node_add(chain, t->a, tp_next(t).val))
                tp_oom();
        /* Compatibility with original API: If the term is of type NodeField
         * or NodeVariable, just put more fields on the original. Otherwise,
         * keep the Chain node. Obvious parsing errors involving literal values
         * are detected here. More complex error cases will have to be handled
         * at execution time. */
        switch (*node) {
        case PARSE_NODE_FIELD:
        case PARSE_NODE_VARIABLE: {
            Str s = parse_chain_node_string(chain, t->a);
            if (s.len == 0)
                tp_oom();
            if (*node == PARSE_NODE_FIELD)
                node = tp_new_field(t, chain->pos, s);
            else
                node = tp_node(tp_new_variable(t, chain->pos, s));
            break;
        }
        case PARSE_NODE_BOOL:
        case PARSE_NODE_STRING:
        case PARSE_NODE_NUMBER:
        case PARSE_NODE_NIL:
        case PARSE_NODE_DOT:
            tp_errorf_v(t, "unexpected . after term %q",
                        parse_node_string(node, error_allocator()));
        default:
            node = tp_node(chain);
        }
    }
    return node;
}

/* command:
 *
 *     operand (space operand)*
 *
 * space-separated arguments up to a pipeline character or right delimiter. We
 * consume the pipe character but leave the right delim to terminate the
 * action. */
static ParseCommandNode *tp_command(ParseTree *t) {
    ParseCommandNode *cmd = tp_new_command(t, tp_peek_non_space(t).pos);
    for (;;) {
        tp_peek_non_space(t); /* skip leading spaces. */
        ParseNode operand = tp_operand(t);
        if (operand != NULL)
            tp_append_node(t, &cmd->args, operand);
        burrow__Item token = tp_next(t);
        if (token.typ == BURROW__ITEM_SPACE)
            continue;
        if (token.typ == BURROW__ITEM_RIGHT_DELIM ||
            token.typ == BURROW__ITEM_RIGHT_PAREN)
            tp_backup(t);
        else if (token.typ != BURROW__ITEM_PIPE)
            /* nothing here; break loop below */
            tp_unexpected(t, token, "operand");
        break;
    }
    if (cmd->args.len == 0)
        tp_errorf(t, BURROW_S("empty command"));
    return cmd;
}

/* recover: the error a parse stopped with, from the panic p. */
static void tp_recover(ParseTree *t, Any p, Error *err) {
    if (p.t != &tp_error_desc)
        panic(p);
    if (t != NULL)
        tp_stop_parse(t);
    *err = ((const TpError *)p.data)->err;
}

/* The body of Tree.Parse, which panics on an error. */
static void tp_tree_parse(ParseTree *t, TpParser *p, burrow__Lexer *lexer, Str text,
                          Str left_delim, Str right_delim, Map *tree_set, Slice funcs) {
    t->parse_name = t->name;
    t->left_delim = left_delim.len == 0 ? BURROW_S("{{") : left_delim;
    t->right_delim = right_delim.len == 0 ? BURROW_S("}}") : right_delim;
    /* The nodes point into the text, so the tree keeps a copy of its own. */
    if (text.len > 0) {
        t->text = str_clone(t->a, text);
        if (t->text.len == 0)
            tp_oom();
    } else {
        t->text = BURROW_STR_EMPTY;
    }
    burrow__parse_lex(lexer, t->name, t->text, t->left_delim, t->right_delim);
    tp_start_parse(t, p, funcs, lexer, tree_set);
    tp_parse(t);
    tp_add(t);
    tp_stop_parse(t);
}

ParseTree *parse_tree_parse(ParseTree *t, Str text, Str left_delim, Str right_delim,
                            Map *tree_set, Slice funcs, Error *err) {
    Error e = BURROW_NO_ERROR;
    volatile bool failed = false;
    TpParser p;
    burrow__Lexer lexer;
    BURROW_TRY {
        tp_tree_parse(t, &p, &lexer, text, left_delim, right_delim, tree_set, funcs);
    }
    BURROW_CATCH(v) {
        tp_recover(t, v, &e);
        failed = true;
    }
    BURROW_TRY_END;
    if (err != NULL)
        *err = e;
    return failed ? NULL : t;
}

Map *parse_parse(Alloc *a, Str name, Str text, Str left_delim, Str right_delim,
                 Slice funcs, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    Map *tree_set = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    ParseTree *t = parse_new(a, name, (Slice){0});
    if (tree_set == NULL || t == NULL) {
        if (err != NULL)
            *err = burrow_err_out_of_memory;
        return tree_set;
    }
    parse_tree_parse(t, text, left_delim, right_delim, tree_set, funcs, err);
    return tree_set;
}
