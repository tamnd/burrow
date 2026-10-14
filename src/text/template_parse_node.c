/* text/template/parse: the nodes of a parse tree, and writing them back out
 * as template text.
 *
 * Derived from Go's src/text/template/parse/node.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/template/parse.h"

#include "burrow/core.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "template_parse_internal.h"

#include <stddef.h>
#include <string.h>

/* textFormat: %s, or %q for the tests. */
bool burrow__parse_text_format_q = false;

void *burrow__parse_node_new(Alloc *a, size_t size, ParseNodeType typ, ParsePos pos,
                             ParseTree *tr) {
    /* Every node starts with the same three members, and the end node is the
     * smallest struct that has just those. */
    burrow__ParseEndNode *n =
        (burrow__ParseEndNode *)mem_alloc(a, size, _Alignof(max_align_t));
    if (n == NULL)
        return NULL;
    n->node_type = typ;
    n->pos = pos;
    n->tr = tr;
    return n;
}

bool burrow__parse_append(Alloc *a, Slice *s, const Type *elem_type, const void *elem) {
    if (s->elem == NULL)
        *s = slice_nil(elem_type);
    Slice r = slice_append(a, *s, elem, 1);
    if (r.p == NULL)
        return false;
    *s = r;
    return true;
}

void burrow__parse_nil_deref(void) {
    panic_str(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static ParseTree *tpn_tree(ParseTree *t) {
    if (t == NULL)
        burrow__parse_nil_deref();
    return t;
}

static ParseNode tpn_node(void *n) {
    return (ParseNode)n;
}

/* --------------------------------------------------------------- Node */

ParseNodeType parse_node_type(ParseNode n) {
    if (n == NULL)
        burrow__parse_nil_deref();
    return *n;
}

ParsePos parse_node_position(ParseNode n) {
    if (n == NULL)
        burrow__parse_nil_deref();
    return ((burrow__ParseEndNode *)n)->pos;
}

ParseTree *burrow__parse_node_tree(ParseNode n) {
    if (n == NULL)
        burrow__parse_nil_deref();
    return ((burrow__ParseEndNode *)n)->tr;
}

/* ------------------------------------------------------------ writeTo */

/* A strings.Builder that remembers whether a write failed, so that String can
 * give the empty string rather than part of the text. */
typedef struct TpnWriter {
    StringsBuilder sb;
    bool failed;
} TpnWriter;

static void tpn_write_string(TpnWriter *w, Str s) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_string(&w->sb, s, &err);
    if (BURROW_FAILED(err))
        w->failed = true;
}

static void tpn_write_byte(TpnWriter *w, Byte c) {
    if (BURROW_FAILED(strings_builder_write_byte(&w->sb, c)))
        w->failed = true;
}

static void tpn_write_to(TpnWriter *w, ParseNode n);

static void tpn_write_pipe(TpnWriter *w, ParsePipeNode *p) {
    if (p == NULL)
        burrow__parse_nil_deref();
    if (p->decl.len > 0) {
        for (Int i = 0; i < p->decl.len; i++) {
            if (i > 0)
                tpn_write_string(w, BURROW_S(", "));
            tpn_write_to(w, BURROW_AT(ParseNode, p->decl, i));
        }
        if (p->is_assign)
            tpn_write_string(w, BURROW_S(" = "));
        else
            tpn_write_string(w, BURROW_S(" := "));
    }
    for (Int i = 0; i < p->cmds.len; i++) {
        if (i > 0)
            tpn_write_string(w, BURROW_S(" | "));
        tpn_write_to(w, BURROW_AT(ParseNode, p->cmds, i));
    }
}

static void tpn_write_list(TpnWriter *w, ParseListNode *l) {
    if (l == NULL)
        burrow__parse_nil_deref();
    for (Int i = 0; i < l->nodes.len; i++)
        tpn_write_to(w, BURROW_AT(ParseNode, l->nodes, i));
}

/* A delimited keyword, such as {{end}}. */
static void tpn_write_keyword(TpnWriter *w, ParseTree *tr, Str word) {
    tr = tpn_tree(tr);
    tpn_write_string(w, tr->left_delim);
    tpn_write_string(w, word);
    tpn_write_string(w, tr->right_delim);
}

static void tpn_write_idents(TpnWriter *w, Slice ident, bool leading_dot) {
    for (Int i = 0; i < ident.len; i++) {
        if (i > 0 || leading_dot)
            tpn_write_byte(w, '.');
        tpn_write_string(w, BURROW_AT(Str, ident, i));
    }
}

static void tpn_write_branch(TpnWriter *w, ParseBranchNode *b) {
    Str name = BURROW_STR_EMPTY;
    switch (b->node_type) {
    case PARSE_NODE_IF:
        name = BURROW_S("if");
        break;
    case PARSE_NODE_RANGE:
        name = BURROW_S("range");
        break;
    case PARSE_NODE_WITH:
        name = BURROW_S("with");
        break;
    default:
        panic_str(BURROW_S("unknown branch type"));
    }
    ParseTree *tr = tpn_tree(b->tr);
    tpn_write_string(w, tr->left_delim);
    tpn_write_string(w, name);
    tpn_write_byte(w, ' ');
    tpn_write_pipe(w, b->pipe);
    tpn_write_string(w, tr->right_delim);
    tpn_write_list(w, b->list);
    if (b->else_list != NULL) {
        tpn_write_keyword(w, tr, BURROW_S("else"));
        tpn_write_list(w, b->else_list);
    }
    tpn_write_keyword(w, tr, BURROW_S("end"));
}

static void tpn_write_text(TpnWriter *w, ParseTextNode *t) {
    Str text = str_from_bytes((const Byte *)t->text.p, t->text.len);
    if (!burrow__parse_text_format_q) {
        tpn_write_string(w, text);
        return;
    }
    /* The writer's allocator is a scratch arena, so the quoted copy can go
     * there too. */
    Str q = strconv_quote(w->sb.a, text);
    if (q.len == 0)
        w->failed = true;
    tpn_write_string(w, q);
}

static void tpn_write_to(TpnWriter *w, ParseNode n) {
    if (n == NULL)
        burrow__parse_nil_deref();
    switch (*n) {
    case PARSE_NODE_LIST:
        tpn_write_list(w, (ParseListNode *)n);
        break;
    case PARSE_NODE_TEXT:
        tpn_write_text(w, (ParseTextNode *)n);
        break;
    case PARSE_NODE_COMMENT: {
        ParseCommentNode *c = (ParseCommentNode *)n;
        tpn_write_keyword(w, c->tr, c->text);
        break;
    }
    case PARSE_NODE_PIPE:
        tpn_write_pipe(w, (ParsePipeNode *)n);
        break;
    case PARSE_NODE_ACTION: {
        ParseActionNode *a = (ParseActionNode *)n;
        ParseTree *tr = tpn_tree(a->tr);
        tpn_write_string(w, tr->left_delim);
        tpn_write_pipe(w, a->pipe);
        tpn_write_string(w, tr->right_delim);
        break;
    }
    case PARSE_NODE_COMMAND: {
        ParseCommandNode *c = (ParseCommandNode *)n;
        for (Int i = 0; i < c->args.len; i++) {
            if (i > 0)
                tpn_write_byte(w, ' ');
            ParseNode arg = BURROW_AT(ParseNode, c->args, i);
            if (arg != NULL && *arg == PARSE_NODE_PIPE) {
                tpn_write_byte(w, '(');
                tpn_write_pipe(w, (ParsePipeNode *)arg);
                tpn_write_byte(w, ')');
                continue;
            }
            tpn_write_to(w, arg);
        }
        break;
    }
    case PARSE_NODE_IDENTIFIER:
        tpn_write_string(w, ((ParseIdentifierNode *)n)->ident);
        break;
    case PARSE_NODE_VARIABLE:
        tpn_write_idents(w, ((ParseVariableNode *)n)->ident, false);
        break;
    case PARSE_NODE_DOT:
        tpn_write_byte(w, '.');
        break;
    case PARSE_NODE_NIL:
        tpn_write_string(w, BURROW_S("nil"));
        break;
    case PARSE_NODE_FIELD:
        tpn_write_idents(w, ((ParseFieldNode *)n)->ident, true);
        break;
    case PARSE_NODE_CHAIN: {
        ParseChainNode *c = (ParseChainNode *)n;
        if (c->node != NULL && *c->node == PARSE_NODE_PIPE) {
            tpn_write_byte(w, '(');
            tpn_write_pipe(w, (ParsePipeNode *)c->node);
            tpn_write_byte(w, ')');
        } else {
            tpn_write_to(w, c->node);
        }
        tpn_write_idents(w, c->field, true);
        break;
    }
    case PARSE_NODE_BOOL:
        tpn_write_string(w, ((ParseBoolNode *)n)->true_ ? BURROW_S("true")
                                                        : BURROW_S("false"));
        break;
    case PARSE_NODE_NUMBER:
        tpn_write_string(w, ((ParseNumberNode *)n)->text);
        break;
    case PARSE_NODE_STRING:
        tpn_write_string(w, ((ParseStringNode *)n)->quoted);
        break;
    case PARSE_NODE_IF:
    case PARSE_NODE_RANGE:
    case PARSE_NODE_WITH:
        tpn_write_branch(w, (ParseBranchNode *)n);
        break;
    case PARSE_NODE_BREAK:
        tpn_write_keyword(w, ((ParseBreakNode *)n)->tr, BURROW_S("break"));
        break;
    case PARSE_NODE_CONTINUE:
        tpn_write_keyword(w, ((ParseContinueNode *)n)->tr, BURROW_S("continue"));
        break;
    case PARSE_NODE_TEMPLATE: {
        ParseTemplateNode *t = (ParseTemplateNode *)n;
        ParseTree *tr = tpn_tree(t->tr);
        tpn_write_string(w, tr->left_delim);
        tpn_write_string(w, BURROW_S("template "));
        Str q = strconv_quote(w->sb.a, t->name);
        if (q.len == 0)
            w->failed = true;
        tpn_write_string(w, q);
        if (t->pipe != NULL) {
            tpn_write_byte(w, ' ');
            tpn_write_pipe(w, t->pipe);
        }
        tpn_write_string(w, tr->right_delim);
        break;
    }
    case BURROW__PARSE_NODE_ELSE:
        tpn_write_keyword(w, ((burrow__ParseElseNode *)n)->tr, BURROW_S("else"));
        break;
    case BURROW__PARSE_NODE_END:
        tpn_write_keyword(w, ((burrow__ParseEndNode *)n)->tr, BURROW_S("end"));
        break;
    default:
        break;
    }
}

/* String: writeTo into a scratch arena, and the result copied into a. Most
 * nodes print to a few dozen bytes, so the arena asks for small chunks rather
 * than the default 64KB. */
static Str tpn_string(ParseNode n, Alloc *a) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    TpnWriter w = {STRINGS_BUILDER(arena_allocator(&ar)), false};
    tpn_write_to(&w, n);
    Str s = strings_builder_string(&w.sb);
    Str out = BURROW_STR_EMPTY;
    if (!w.failed && s.len > 0)
        out = str_clone(a, s);
    arena_free(&ar);
    return out;
}

Str parse_node_string(ParseNode n, Alloc *a) {
    return tpn_string(n, a);
}

Str parse_list_node_string(ParseListNode *l, Alloc *a) {
    return tpn_string(tpn_node(l), a);
}

Str parse_text_node_string(ParseTextNode *t, Alloc *a) {
    return tpn_string(tpn_node(t), a);
}

Str parse_comment_node_string(ParseCommentNode *c, Alloc *a) {
    return tpn_string(tpn_node(c), a);
}

Str parse_pipe_node_string(ParsePipeNode *p, Alloc *a) {
    return tpn_string(tpn_node(p), a);
}

Str parse_action_node_string(ParseActionNode *n, Alloc *a) {
    return tpn_string(tpn_node(n), a);
}

Str parse_command_node_string(ParseCommandNode *c, Alloc *a) {
    return tpn_string(tpn_node(c), a);
}

Str parse_identifier_node_string(ParseIdentifierNode *i, Alloc *a) {
    return tpn_string(tpn_node(i), a);
}

Str parse_variable_node_string(ParseVariableNode *v, Alloc *a) {
    return tpn_string(tpn_node(v), a);
}

Str parse_dot_node_string(ParseDotNode *d, Alloc *a) {
    return tpn_string(tpn_node(d), a);
}

Str parse_nil_node_string(ParseNilNode *n, Alloc *a) {
    return tpn_string(tpn_node(n), a);
}

Str parse_field_node_string(ParseFieldNode *f, Alloc *a) {
    return tpn_string(tpn_node(f), a);
}

Str parse_chain_node_string(ParseChainNode *c, Alloc *a) {
    return tpn_string(tpn_node(c), a);
}

Str parse_bool_node_string(ParseBoolNode *b, Alloc *a) {
    return tpn_string(tpn_node(b), a);
}

Str parse_number_node_string(ParseNumberNode *n, Alloc *a) {
    return tpn_string(tpn_node(n), a);
}

Str parse_string_node_string(ParseStringNode *s, Alloc *a) {
    return tpn_string(tpn_node(s), a);
}

Str parse_branch_node_string(ParseBranchNode *b, Alloc *a) {
    return tpn_string(tpn_node(b), a);
}

Str parse_if_node_string(ParseIfNode *i, Alloc *a) {
    return tpn_string(&i->branch_node.node_type, a);
}

Str parse_range_node_string(ParseRangeNode *r, Alloc *a) {
    return tpn_string(&r->branch_node.node_type, a);
}

Str parse_with_node_string(ParseWithNode *w, Alloc *a) {
    return tpn_string(&w->branch_node.node_type, a);
}

Str parse_break_node_string(ParseBreakNode *b, Alloc *a) {
    return tpn_string(tpn_node(b), a);
}

Str parse_continue_node_string(ParseContinueNode *c, Alloc *a) {
    return tpn_string(tpn_node(c), a);
}

Str parse_template_node_string(ParseTemplateNode *t, Alloc *a) {
    return tpn_string(tpn_node(t), a);
}

/* --------------------------------------------------------------- Copy */

#define TPN_NEW(a, T, typ, pos, tr)                                                    \
    ((T *)burrow__parse_node_new((a), sizeof(T), (typ), (pos), (tr)))

/* append([]string{}, s...), or false when a refuses. */
static bool tpn_copy_strs(Alloc *a, Slice s, Slice *out) {
    Slice r = slice_make(a, TYPE_STRING, s.len, s.len);
    if (r.p == NULL && s.len > 0)
        return false;
    slice_copy(r, s);
    *out = r;
    return true;
}

ParseListNode *parse_list_node_copy_list(ParseListNode *l, Alloc *a) {
    if (l == NULL)
        return NULL;
    ParseListNode *n = TPN_NEW(a, ParseListNode, PARSE_NODE_LIST, l->pos, l->tr);
    if (n == NULL)
        return NULL;
    for (Int i = 0; i < l->nodes.len; i++) {
        ParseNode elem = parse_node_copy(BURROW_AT(ParseNode, l->nodes, i), a);
        if (elem == NULL ||
            !burrow__parse_append(a, &n->nodes, TYPE_UNSAFE_POINTER, &elem))
            return NULL;
    }
    return n;
}

ParsePipeNode *parse_pipe_node_copy_pipe(ParsePipeNode *p, Alloc *a) {
    if (p == NULL)
        return NULL;
    ParsePipeNode *n = TPN_NEW(a, ParsePipeNode, PARSE_NODE_PIPE, p->pos, p->tr);
    if (n == NULL)
        return NULL;
    n->line = p->line;
    n->decl = slice_make(a, TYPE_UNSAFE_POINTER, p->decl.len, p->decl.len);
    if (n->decl.p == NULL && p->decl.len > 0)
        return NULL;
    for (Int i = 0; i < p->decl.len; i++) {
        ParseNode d = parse_node_copy(BURROW_AT(ParseNode, p->decl, i), a);
        if (d == NULL)
            return NULL;
        BURROW_AT(ParseNode, n->decl, i) = d;
    }
    n->is_assign = p->is_assign;
    for (Int i = 0; i < p->cmds.len; i++) {
        ParseNode c = parse_node_copy(BURROW_AT(ParseNode, p->cmds, i), a);
        if (c == NULL || !burrow__parse_append(a, &n->cmds, TYPE_UNSAFE_POINTER, &c))
            return NULL;
    }
    return n;
}

/* CopyList and CopyPipe for the copy of a branch, where a NULL in is a NULL
 * out and anything else that comes out NULL is a refusal. */
static bool tpn_copy_list(ParseListNode *l, Alloc *a, ParseListNode **out) {
    *out = parse_list_node_copy_list(l, a);
    return l == NULL || *out != NULL;
}

static bool tpn_copy_pipe(ParsePipeNode *p, Alloc *a, ParsePipeNode **out) {
    *out = parse_pipe_node_copy_pipe(p, a);
    return p == NULL || *out != NULL;
}

static ParseNode tpn_new_branch(Alloc *a, ParseBranchNode *b, ParsePipeNode *pipe,
                                ParseListNode *list, ParseListNode *else_list) {
    ParseBranchNode *n = TPN_NEW(a, ParseBranchNode, b->node_type, b->pos, b->tr);
    if (n == NULL)
        return NULL;
    n->line = b->line;
    n->pipe = pipe;
    n->list = list;
    n->else_list = else_list;
    return &n->node_type;
}

/* The copy of an if, range or with node, which copies what it holds. */
static ParseNode tpn_copy_branch(ParseBranchNode *b, Alloc *a) {
    ParsePipeNode *pipe = NULL;
    ParseListNode *list = NULL;
    ParseListNode *else_list = NULL;
    if (!tpn_copy_pipe(b->pipe, a, &pipe) || !tpn_copy_list(b->list, a, &list) ||
        !tpn_copy_list(b->else_list, a, &else_list))
        return NULL;
    return tpn_new_branch(a, b, pipe, list, else_list);
}

ParseNode parse_node_copy(ParseNode n, Alloc *a) {
    if (n == NULL)
        return NULL;
    switch (*n) {
    case PARSE_NODE_LIST:
        return tpn_node(parse_list_node_copy_list((ParseListNode *)n, a));
    case PARSE_NODE_TEXT:
        return parse_text_node_copy((ParseTextNode *)n, a);
    case PARSE_NODE_COMMENT:
        return parse_comment_node_copy((ParseCommentNode *)n, a);
    case PARSE_NODE_PIPE:
        return tpn_node(parse_pipe_node_copy_pipe((ParsePipeNode *)n, a));
    case PARSE_NODE_ACTION:
        return parse_action_node_copy((ParseActionNode *)n, a);
    case PARSE_NODE_COMMAND:
        return parse_command_node_copy((ParseCommandNode *)n, a);
    case PARSE_NODE_IDENTIFIER:
        return parse_identifier_node_copy((ParseIdentifierNode *)n, a);
    case PARSE_NODE_VARIABLE:
        return parse_variable_node_copy((ParseVariableNode *)n, a);
    case PARSE_NODE_DOT:
        return parse_dot_node_copy((ParseDotNode *)n, a);
    case PARSE_NODE_NIL:
        return parse_nil_node_copy((ParseNilNode *)n, a);
    case PARSE_NODE_FIELD:
        return parse_field_node_copy((ParseFieldNode *)n, a);
    case PARSE_NODE_CHAIN:
        return parse_chain_node_copy((ParseChainNode *)n, a);
    case PARSE_NODE_BOOL:
        return parse_bool_node_copy((ParseBoolNode *)n, a);
    case PARSE_NODE_NUMBER:
        return parse_number_node_copy((ParseNumberNode *)n, a);
    case PARSE_NODE_STRING:
        return parse_string_node_copy((ParseStringNode *)n, a);
    case PARSE_NODE_IF:
    case PARSE_NODE_RANGE:
    case PARSE_NODE_WITH:
        return tpn_copy_branch((ParseBranchNode *)n, a);
    case PARSE_NODE_BREAK:
        return parse_break_node_copy((ParseBreakNode *)n, a);
    case PARSE_NODE_CONTINUE:
        return parse_continue_node_copy((ParseContinueNode *)n, a);
    case PARSE_NODE_TEMPLATE:
        return parse_template_node_copy((ParseTemplateNode *)n, a);
    case BURROW__PARSE_NODE_ELSE: {
        burrow__ParseElseNode *e = (burrow__ParseElseNode *)n;
        burrow__ParseElseNode *c =
            TPN_NEW(a, burrow__ParseElseNode, BURROW__PARSE_NODE_ELSE, e->pos, e->tr);
        if (c == NULL)
            return NULL;
        c->line = e->line;
        return tpn_node(c);
    }
    case BURROW__PARSE_NODE_END: {
        burrow__ParseEndNode *e = (burrow__ParseEndNode *)n;
        return tpn_node(
            TPN_NEW(a, burrow__ParseEndNode, BURROW__PARSE_NODE_END, e->pos, e->tr));
    }
    default:
        return NULL;
    }
}

ParseNode parse_list_node_copy(ParseListNode *l, Alloc *a) {
    return tpn_node(parse_list_node_copy_list(l, a));
}

ParseNode parse_text_node_copy(ParseTextNode *t, Alloc *a) {
    ParseTextNode *n = TPN_NEW(a, ParseTextNode, PARSE_NODE_TEXT, t->pos, t->tr);
    if (n == NULL)
        return NULL;
    n->text = slice_make(a, TYPE_BYTE, 0, 0);
    if (t->text.len > 0) {
        n->text =
            slice_from_str(a, str_from_bytes((const Byte *)t->text.p, t->text.len));
        if (n->text.p == NULL)
            return NULL;
    }
    return tpn_node(n);
}

ParseNode parse_comment_node_copy(ParseCommentNode *c, Alloc *a) {
    ParseCommentNode *n =
        TPN_NEW(a, ParseCommentNode, PARSE_NODE_COMMENT, c->pos, c->tr);
    if (n == NULL)
        return NULL;
    n->text = c->text;
    return tpn_node(n);
}

ParseNode parse_pipe_node_copy(ParsePipeNode *p, Alloc *a) {
    return tpn_node(parse_pipe_node_copy_pipe(p, a));
}

ParseNode parse_action_node_copy(ParseActionNode *n, Alloc *a) {
    ParsePipeNode *pipe = NULL;
    if (!tpn_copy_pipe(n->pipe, a, &pipe))
        return NULL;
    ParseActionNode *c = TPN_NEW(a, ParseActionNode, PARSE_NODE_ACTION, n->pos, n->tr);
    if (c == NULL)
        return NULL;
    c->line = n->line;
    c->pipe = pipe;
    return tpn_node(c);
}

ParseNode parse_command_node_copy(ParseCommandNode *c, Alloc *a) {
    if (c == NULL)
        return NULL;
    ParseCommandNode *n =
        TPN_NEW(a, ParseCommandNode, PARSE_NODE_COMMAND, c->pos, c->tr);
    if (n == NULL)
        return NULL;
    for (Int i = 0; i < c->args.len; i++) {
        ParseNode arg = parse_node_copy(BURROW_AT(ParseNode, c->args, i), a);
        if (arg == NULL ||
            !burrow__parse_append(a, &n->args, TYPE_UNSAFE_POINTER, &arg))
            return NULL;
    }
    return tpn_node(n);
}

ParseIdentifierNode *parse_new_identifier(Alloc *a, Str ident) {
    ParseIdentifierNode *n =
        TPN_NEW(a, ParseIdentifierNode, PARSE_NODE_IDENTIFIER, 0, NULL);
    if (n == NULL)
        return NULL;
    n->ident = ident;
    return n;
}

ParseIdentifierNode *parse_identifier_node_set_pos(ParseIdentifierNode *i,
                                                   ParsePos pos) {
    i->pos = pos;
    return i;
}

ParseIdentifierNode *parse_identifier_node_set_tree(ParseIdentifierNode *i,
                                                    ParseTree *t) {
    i->tr = t;
    return i;
}

ParseNode parse_identifier_node_copy(ParseIdentifierNode *i, Alloc *a) {
    ParseIdentifierNode *n = parse_new_identifier(a, i->ident);
    if (n == NULL)
        return NULL;
    return tpn_node(parse_identifier_node_set_pos(
        parse_identifier_node_set_tree(n, i->tr), i->pos));
}

ParseNode parse_variable_node_copy(ParseVariableNode *v, Alloc *a) {
    ParseVariableNode *n =
        TPN_NEW(a, ParseVariableNode, PARSE_NODE_VARIABLE, v->pos, v->tr);
    if (n == NULL || !tpn_copy_strs(a, v->ident, &n->ident))
        return NULL;
    return tpn_node(n);
}

ParseNode parse_dot_node_copy(ParseDotNode *d, Alloc *a) {
    return tpn_node(TPN_NEW(a, ParseDotNode, PARSE_NODE_DOT, d->pos, d->tr));
}

ParseNode parse_nil_node_copy(ParseNilNode *n, Alloc *a) {
    return tpn_node(TPN_NEW(a, ParseNilNode, PARSE_NODE_NIL, n->pos, n->tr));
}

ParseNode parse_field_node_copy(ParseFieldNode *f, Alloc *a) {
    ParseFieldNode *n = TPN_NEW(a, ParseFieldNode, PARSE_NODE_FIELD, f->pos, f->tr);
    if (n == NULL || !tpn_copy_strs(a, f->ident, &n->ident))
        return NULL;
    return tpn_node(n);
}

bool parse_chain_node_add(ParseChainNode *c, Alloc *a, Str field) {
    if (field.len == 0 || field.p[0] != '.')
        panic_str(BURROW_S("no dot in field"));
    field = str_from_bytes(field.p + 1, field.len - 1); /* Remove leading dot. */
    if (field.len == 0)
        panic_str(BURROW_S("empty field"));
    return burrow__parse_append(a, &c->field, TYPE_STRING, &field);
}

ParseNode parse_chain_node_copy(ParseChainNode *c, Alloc *a) {
    ParseChainNode *n = TPN_NEW(a, ParseChainNode, PARSE_NODE_CHAIN, c->pos, c->tr);
    if (n == NULL || !tpn_copy_strs(a, c->field, &n->field))
        return NULL;
    n->node = c->node;
    return tpn_node(n);
}

ParseNode parse_bool_node_copy(ParseBoolNode *b, Alloc *a) {
    ParseBoolNode *n = TPN_NEW(a, ParseBoolNode, PARSE_NODE_BOOL, b->pos, b->tr);
    if (n == NULL)
        return NULL;
    n->true_ = b->true_;
    return tpn_node(n);
}

ParseNode parse_number_node_copy(ParseNumberNode *n, Alloc *a) {
    ParseNumberNode *nn = TPN_NEW(a, ParseNumberNode, PARSE_NODE_NUMBER, n->pos, n->tr);
    if (nn == NULL)
        return NULL;
    *nn = *n; /* Easy, fast, correct. */
    return tpn_node(nn);
}

ParseNode parse_string_node_copy(ParseStringNode *s, Alloc *a) {
    ParseStringNode *n = TPN_NEW(a, ParseStringNode, PARSE_NODE_STRING, s->pos, s->tr);
    if (n == NULL)
        return NULL;
    n->quoted = s->quoted;
    n->text = s->text;
    return tpn_node(n);
}

ParseNode parse_branch_node_copy(ParseBranchNode *b, Alloc *a) {
    switch (b->node_type) {
    case PARSE_NODE_IF:
    case PARSE_NODE_RANGE:
    case PARSE_NODE_WITH:
        /* Go's BranchNode.Copy makes a new node around the same pipeline and
         * lists, unlike the Copy of an if, range or with node. */
        return tpn_new_branch(a, b, b->pipe, b->list, b->else_list);
    default:
        panic_str(BURROW_S("unknown branch type"));
    }
}

ParseNode parse_if_node_copy(ParseIfNode *i, Alloc *a) {
    return tpn_copy_branch(&i->branch_node, a);
}

ParseNode parse_range_node_copy(ParseRangeNode *r, Alloc *a) {
    return tpn_copy_branch(&r->branch_node, a);
}

ParseNode parse_with_node_copy(ParseWithNode *w, Alloc *a) {
    return tpn_copy_branch(&w->branch_node, a);
}

ParseNode parse_break_node_copy(ParseBreakNode *b, Alloc *a) {
    ParseBreakNode *n = TPN_NEW(a, ParseBreakNode, PARSE_NODE_BREAK, b->pos, b->tr);
    if (n == NULL)
        return NULL;
    n->line = b->line;
    return tpn_node(n);
}

ParseNode parse_continue_node_copy(ParseContinueNode *c, Alloc *a) {
    ParseContinueNode *n =
        TPN_NEW(a, ParseContinueNode, PARSE_NODE_CONTINUE, c->pos, c->tr);
    if (n == NULL)
        return NULL;
    n->line = c->line;
    return tpn_node(n);
}

ParseNode parse_template_node_copy(ParseTemplateNode *t, Alloc *a) {
    ParsePipeNode *pipe = NULL;
    if (!tpn_copy_pipe(t->pipe, a, &pipe))
        return NULL;
    ParseTemplateNode *n =
        TPN_NEW(a, ParseTemplateNode, PARSE_NODE_TEMPLATE, t->pos, t->tr);
    if (n == NULL)
        return NULL;
    n->line = t->line;
    n->name = t->name;
    n->pipe = pipe;
    return tpn_node(n);
}
