/* text/template/parse, the parse trees of text/template and html/template.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package text/template/parse */

/* Go's text/template/parse. It turns the text of a template into a tree of
 * nodes, which text/template runs and html/template rewrites before it does.
 * Programs that want to run a template want text/template, and this package is
 * for the ones that want to look at one:
 *
 *     Arena ar;
 *     arena_init(&ar, heap_allocator(), 0);
 *     Error err = BURROW_NO_ERROR;
 *     Map *trees = parse_parse(arena_allocator(&ar), BURROW_S("page"),
 *                              BURROW_S("Hello, {{.Name}}!"), BURROW_STR_EMPTY,
 *                              BURROW_STR_EMPTY, (Slice){0}, &err);
 *     Str page = BURROW_S("page");
 *     ParseTree **t = map_get(trees, &page);
 *     fmt_println_v(parse_list_node_string((*t)->root, arena_allocator(&ar)));
 *     arena_free(&ar);
 *
 * Nodes. Each of Go's node types is a struct here, and each starts with the
 * same three members: node_type, pos and tr, which are the NodeType and Pos a
 * Go node embeds and the tree it belongs to. ParseNode, Go's Node, is a pointer
 * to the first of them. A node is passed as a ParseNode by taking the address
 * of its node_type, and turned back into its own type by looking at it:
 *
 *     ParseNode n = &text->node_type;
 *     if (*n == PARSE_NODE_TEXT) {
 *         ParseTextNode *t = (ParseTextNode *)n;
 *         // ...
 *     }
 *
 * An if, range or with node starts with the branch node it embeds, so its
 * ParseNode is &n->branch_node.node_type. A NULL ParseNode is Go's nil.
 *
 * Functions. Go's template functions are given as a Slice of Map pointers,
 * Go's ...map[string]any. Each map is keyed by Str and holds Any values, and
 * the parser only asks whether a name is there with a value that is not nil.
 * An empty Slice is no functions.
 *
 * Memory. A tree remembers the allocator it was made with, and parsing puts
 * everything in it: the nodes, the trees of the templates the text defines,
 * and a copy of the text, which the strings in the nodes point into. Use an
 * arena and free it, with everything in it, when the trees are done with. The
 * copy functions and the ones that write a node out as text take an allocator
 * of their own. A copy shares its strings and its tree with the node it was
 * copied from, as in Go, so whatever holds those has to outlive it.
 *
 * Nothing here is safe to use on one tree from two threads while anything
 * changes it. */

#ifndef BURROW_TEXT_TEMPLATE_PARSE_H
#define BURROW_TEXT_TEMPLATE_PARSE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- nodes */

/* parse.Pos, a byte offset into the text of the template the node came from. */
typedef Int ParsePos;

/* Pos.Position: p itself, which is how a node answers Position in Go. */
static inline ParsePos parse_pos_position(ParsePos p) {
    return p;
}

/* parse.NodeType, which node a struct is. It is an Int, as Go's is an int, so
 * that a node and its first member are aligned alike. */
typedef Int ParseNodeType;

/* The node types, with Go's values. 6 and 7 are the else and end nodes, which
 * the parser makes and never leaves in a tree. */
enum {
    PARSE_NODE_TEXT = 0,       /* plain text */
    PARSE_NODE_ACTION = 1,     /* a non-control action such as a field evaluation */
    PARSE_NODE_BOOL = 2,       /* a boolean constant */
    PARSE_NODE_CHAIN = 3,      /* a sequence of field accesses */
    PARSE_NODE_COMMAND = 4,    /* an element of a pipeline */
    PARSE_NODE_DOT = 5,        /* the cursor, dot */
    PARSE_NODE_FIELD = 8,      /* a field or method name */
    PARSE_NODE_IDENTIFIER = 9, /* an identifier, always a function name */
    PARSE_NODE_IF = 10,        /* an if action */
    PARSE_NODE_LIST = 11,      /* a list of nodes */
    PARSE_NODE_NIL = 12,       /* an untyped nil constant */
    PARSE_NODE_NUMBER = 13,    /* a numerical constant */
    PARSE_NODE_PIPE = 14,      /* a pipeline of commands */
    PARSE_NODE_RANGE = 15,     /* a range action */
    PARSE_NODE_STRING = 16,    /* a string constant */
    PARSE_NODE_TEMPLATE = 17,  /* a template invocation action */
    PARSE_NODE_VARIABLE = 18,  /* a $ variable */
    PARSE_NODE_WITH = 19,      /* a with action */
    PARSE_NODE_COMMENT = 20,   /* a comment */
    PARSE_NODE_BREAK = 21,     /* a break action */
    PARSE_NODE_CONTINUE = 22   /* a continue action */
};

/* NodeType.Type: t itself. */
static inline ParseNodeType parse_node_type_type(ParseNodeType t) {
    return t;
}

/* parse.Node: a pointer to the node_type a node starts with. */
typedef ParseNodeType *ParseNode;

typedef struct ParseTree ParseTree;

/* Node.Type, Node.Position and Node.String for any node. The string is the
 * node written back out as template text, made in a, and empty when a
 * refuses. */
ParseNodeType parse_node_type(ParseNode n);
ParsePos parse_node_position(ParseNode n);
BURROW_OWNS(ret) Str parse_node_string(ParseNode n, Alloc *a);

/* Node.Copy: a deep copy of n made in a, or NULL when a refuses or n is NULL.
 * The copy keeps n's tree, and shares its strings, as Go's does. */
BURROW_OWNS(ret) ParseNode parse_node_copy(ParseNode n, Alloc *a);

/* parse.ListNode, a sequence of nodes. nodes holds ParseNode values. */
typedef struct ParseListNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Slice nodes; /* the element nodes in lexical order */
} ParseListNode;

/* parse.TextNode, plain text. */
typedef struct ParseTextNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Bytes text; /* the text, which may span newlines */
} ParseTextNode;

/* parse.CommentNode, a comment, kept only when the tree's mode asks for
 * comments. */
typedef struct ParseCommentNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Str text; /* the comment, with its slash-star markers */
} ParseCommentNode;

/* parse.PipeNode, a pipeline with optional declarations. decl holds
 * ParseVariableNode pointers and cmds holds ParseCommandNode pointers. */
typedef struct ParsePipeNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;       /* the line number in the input; deprecated, as in Go */
    bool is_assign; /* the variables are being assigned, not declared */
    Slice decl;     /* variables in lexical order */
    Slice cmds;     /* the commands in lexical order */
} ParsePipeNode;

/* parse.ActionNode, an action that is not a control structure, such as a
 * field evaluation. */
typedef struct ParseActionNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;            /* the line number in the input; deprecated, as in Go */
    ParsePipeNode *pipe; /* the pipeline in the action */
} ParseActionNode;

/* parse.CommandNode, one element of a pipeline. args holds ParseNode values:
 * identifiers, fields, constants and parenthesised pipelines. */
typedef struct ParseCommandNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Slice args; /* arguments in lexical order */
} ParseCommandNode;

/* parse.IdentifierNode, the name of a function. */
typedef struct ParseIdentifierNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Str ident; /* the identifier's name */
} ParseIdentifierNode;

/* parse.VariableNode, a $ variable followed by any fields. ident holds Str
 * values, the variable's name first. */
typedef struct ParseVariableNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Slice ident; /* variable name and fields in lexical order */
} ParseVariableNode;

/* parse.DotNode, the cursor. */
typedef struct ParseDotNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
} ParseDotNode;

/* parse.NilNode, the untyped nil constant. */
typedef struct ParseNilNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
} ParseNilNode;

/* parse.FieldNode, a field or method name such as .A.B. ident holds Str
 * values, without the dots. */
typedef struct ParseFieldNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Slice ident; /* the identifiers in lexical order */
} ParseFieldNode;

/* parse.ChainNode, fields taken from something other than dot or a variable,
 * as in (pipeline).Field. field holds Str values, without the dots. */
typedef struct ParseChainNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    ParseNode node;
    Slice field; /* the identifiers in lexical order */
} ParseChainNode;

/* parse.BoolNode, true or false. Go's field is True, which C has taken, so it
 * is true_ here. */
typedef struct ParseBoolNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    bool true_; /* the value of the boolean constant */
} ParseBoolNode;

/* parse.NumberNode, a number, with every kind of value the text can be read
 * as. A character constant counts as a number. */
typedef struct ParseNumberNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    bool is_int;           /* the number has an integral value */
    bool is_uint;          /* the number has an unsigned integral value */
    bool is_float;         /* the number has a floating point value */
    bool is_complex;       /* the number is complex */
    int64_t int64;         /* the signed integer value */
    uint64_t uint64;       /* the unsigned integer value */
    double float64;        /* the floating point value */
    Complex128 complex128; /* the complex value */
    Str text;              /* the text the number was written as */
} ParseNumberNode;

/* parse.StringNode, a string constant. */
typedef struct ParseStringNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Str quoted; /* the string as written, with its quotes */
    Str text;   /* the string after quote processing */
} ParseStringNode;

/* parse.BranchNode, what if, range and with have in common. */
typedef struct ParseBranchNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;                 /* the line number in the input; deprecated, as in Go */
    ParsePipeNode *pipe;      /* the pipeline to be evaluated */
    ParseListNode *list;      /* what to run if the value is non-empty */
    ParseListNode *else_list; /* what to run if it is empty, or NULL */
} ParseBranchNode;

/* parse.IfNode, parse.RangeNode and parse.WithNode. */
typedef struct ParseIfNode {
    ParseBranchNode branch_node;
} ParseIfNode;

typedef struct ParseRangeNode {
    ParseBranchNode branch_node;
} ParseRangeNode;

typedef struct ParseWithNode {
    ParseBranchNode branch_node;
} ParseWithNode;

/* parse.BreakNode and parse.ContinueNode, inside a range. */
typedef struct ParseBreakNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;
} ParseBreakNode;

typedef struct ParseContinueNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;
} ParseContinueNode;

/* parse.TemplateNode, a {{template}} action. */
typedef struct ParseTemplateNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;            /* the line number in the input; deprecated, as in Go */
    Str name;            /* the name of the template, unquoted */
    ParsePipeNode *pipe; /* what to run the template with as dot, or NULL */
} ParseTemplateNode;

/* Type and Position for each node, which a Go node has from the NodeType and
 * Pos it embeds, or for dot and nil from methods of their own that give the
 * same answer. */
#define parse_list_node_type(n) ((n)->node_type)
#define parse_list_node_position(n) ((n)->pos)
#define parse_text_node_type(n) ((n)->node_type)
#define parse_text_node_position(n) ((n)->pos)
#define parse_comment_node_type(n) ((n)->node_type)
#define parse_comment_node_position(n) ((n)->pos)
#define parse_pipe_node_type(n) ((n)->node_type)
#define parse_pipe_node_position(n) ((n)->pos)
#define parse_action_node_type(n) ((n)->node_type)
#define parse_action_node_position(n) ((n)->pos)
#define parse_command_node_type(n) ((n)->node_type)
#define parse_command_node_position(n) ((n)->pos)
#define parse_identifier_node_type(n) ((n)->node_type)
#define parse_identifier_node_position(n) ((n)->pos)
#define parse_variable_node_type(n) ((n)->node_type)
#define parse_variable_node_position(n) ((n)->pos)
#define parse_dot_node_type(n) ((n)->node_type)
#define parse_dot_node_position(n) ((n)->pos)
#define parse_nil_node_type(n) ((n)->node_type)
#define parse_nil_node_position(n) ((n)->pos)
#define parse_field_node_type(n) ((n)->node_type)
#define parse_field_node_position(n) ((n)->pos)
#define parse_chain_node_type(n) ((n)->node_type)
#define parse_chain_node_position(n) ((n)->pos)
#define parse_bool_node_type(n) ((n)->node_type)
#define parse_bool_node_position(n) ((n)->pos)
#define parse_number_node_type(n) ((n)->node_type)
#define parse_number_node_position(n) ((n)->pos)
#define parse_string_node_type(n) ((n)->node_type)
#define parse_string_node_position(n) ((n)->pos)
#define parse_branch_node_type(n) ((n)->node_type)
#define parse_branch_node_position(n) ((n)->pos)
#define parse_if_node_type(n) ((n)->branch_node.node_type)
#define parse_if_node_position(n) ((n)->branch_node.pos)
#define parse_range_node_type(n) ((n)->branch_node.node_type)
#define parse_range_node_position(n) ((n)->branch_node.pos)
#define parse_with_node_type(n) ((n)->branch_node.node_type)
#define parse_with_node_position(n) ((n)->branch_node.pos)
#define parse_break_node_type(n) ((n)->node_type)
#define parse_break_node_position(n) ((n)->pos)
#define parse_continue_node_type(n) ((n)->node_type)
#define parse_continue_node_position(n) ((n)->pos)
#define parse_template_node_type(n) ((n)->node_type)
#define parse_template_node_position(n) ((n)->pos)

/* String for each node: the node written back out as template text, made in
 * a, and empty when a refuses. */
BURROW_OWNS(ret) Str parse_list_node_string(ParseListNode *l, Alloc *a);
BURROW_OWNS(ret) Str parse_text_node_string(ParseTextNode *t, Alloc *a);
BURROW_OWNS(ret) Str parse_comment_node_string(ParseCommentNode *c, Alloc *a);
BURROW_OWNS(ret) Str parse_pipe_node_string(ParsePipeNode *p, Alloc *a);
BURROW_OWNS(ret) Str parse_action_node_string(ParseActionNode *n, Alloc *a);
BURROW_OWNS(ret) Str parse_command_node_string(ParseCommandNode *c, Alloc *a);
BURROW_OWNS(ret) Str parse_identifier_node_string(ParseIdentifierNode *i, Alloc *a);
BURROW_OWNS(ret) Str parse_variable_node_string(ParseVariableNode *v, Alloc *a);
BURROW_OWNS(ret) Str parse_dot_node_string(ParseDotNode *d, Alloc *a);
BURROW_OWNS(ret) Str parse_nil_node_string(ParseNilNode *n, Alloc *a);
BURROW_OWNS(ret) Str parse_field_node_string(ParseFieldNode *f, Alloc *a);
BURROW_OWNS(ret) Str parse_chain_node_string(ParseChainNode *c, Alloc *a);
BURROW_OWNS(ret) Str parse_bool_node_string(ParseBoolNode *b, Alloc *a);
BURROW_OWNS(ret) Str parse_number_node_string(ParseNumberNode *n, Alloc *a);
BURROW_OWNS(ret) Str parse_string_node_string(ParseStringNode *s, Alloc *a);
BURROW_OWNS(ret) Str parse_branch_node_string(ParseBranchNode *b, Alloc *a);
BURROW_OWNS(ret) Str parse_if_node_string(ParseIfNode *i, Alloc *a);
BURROW_OWNS(ret) Str parse_range_node_string(ParseRangeNode *r, Alloc *a);
BURROW_OWNS(ret) Str parse_with_node_string(ParseWithNode *w, Alloc *a);
BURROW_OWNS(ret) Str parse_break_node_string(ParseBreakNode *b, Alloc *a);
BURROW_OWNS(ret) Str parse_continue_node_string(ParseContinueNode *c, Alloc *a);
BURROW_OWNS(ret) Str parse_template_node_string(ParseTemplateNode *t, Alloc *a);

/* Copy for each node: a deep copy made in a, or NULL when a refuses. A copy of
 * a branch node is an if, range or with node, by its node_type. */
BURROW_OWNS(ret) ParseNode parse_list_node_copy(ParseListNode *l, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_text_node_copy(ParseTextNode *t, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_comment_node_copy(ParseCommentNode *c, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_pipe_node_copy(ParsePipeNode *p, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_action_node_copy(ParseActionNode *n, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_command_node_copy(ParseCommandNode *c, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_identifier_node_copy(ParseIdentifierNode *i, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_variable_node_copy(ParseVariableNode *v, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_dot_node_copy(ParseDotNode *d, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_nil_node_copy(ParseNilNode *n, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_field_node_copy(ParseFieldNode *f, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_chain_node_copy(ParseChainNode *c, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_bool_node_copy(ParseBoolNode *b, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_number_node_copy(ParseNumberNode *n, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_string_node_copy(ParseStringNode *s, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_branch_node_copy(ParseBranchNode *b, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_if_node_copy(ParseIfNode *i, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_range_node_copy(ParseRangeNode *r, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_with_node_copy(ParseWithNode *w, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_break_node_copy(ParseBreakNode *b, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_continue_node_copy(ParseContinueNode *c, Alloc *a);
BURROW_OWNS(ret) ParseNode parse_template_node_copy(ParseTemplateNode *t, Alloc *a);

/* ListNode.CopyList and PipeNode.CopyPipe: the same copies with their own
 * types. A NULL l or p gives NULL. */
BURROW_OWNS(ret) ParseListNode *parse_list_node_copy_list(ParseListNode *l, Alloc *a);
BURROW_OWNS(ret) ParsePipeNode *parse_pipe_node_copy_pipe(ParsePipeNode *p, Alloc *a);

/* NewIdentifier: an identifier node made in a, with no position and no tree
 * until SetPos and SetTree give it them. NULL when a refuses. */
BURROW_OWNS(ret) ParseIdentifierNode *parse_new_identifier(Alloc *a, Str ident);

/* IdentifierNode.SetPos and SetTree. Each returns i, so that they chain the way
 * Go's do. */
ParseIdentifierNode *parse_identifier_node_set_pos(ParseIdentifierNode *i,
                                                   ParsePos pos);
ParseIdentifierNode *parse_identifier_node_set_tree(ParseIdentifierNode *i,
                                                    ParseTree *t);

/* ChainNode.Add: appends a field, which has to start with a dot, as in Go,
 * which panics on one that does not. The field's bytes are not copied. False
 * when a refuses. */
bool parse_chain_node_add(ParseChainNode *c, Alloc *a, Str field);

/* IsEmptyTree: whether n holds nothing but space and comments. Panics on a node
 * that cannot be in a tree, as Go's does. */
bool parse_is_empty_tree(ParseNode n);

/* -------------------------------------------------------------------- trees */

/* parse.Mode, what to do differently while parsing. */
typedef Uint ParseMode;

enum {
    PARSE_PARSE_COMMENTS = 1, /* keep comments in the tree */
    PARSE_SKIP_FUNC_CHECK = 2 /* do not check that functions are defined */
};

/* parse.Tree, one parsed template. name, parse_name, root and mode are Go's
 * fields, and the rest belong to the parser. */
typedef struct ParseTree {
    Str name;            /* the name of the template the tree is for */
    Str parse_name;      /* the top-level template's name, for error messages */
    ParseListNode *root; /* the top-level list of the tree */
    ParseMode mode;      /* the parsing mode */
    Alloc *a;            /* where the tree and its nodes are made */
    Str text;            /* the text the tree was parsed from */
    Str left_delim;      /* the delimiters, for writing nodes back out */
    Str right_delim;
    Slice funcs;         /* the function maps, while parsing */
    void *burrow__state; /* the parser's state, while parsing */
} ParseTree;

/* New: a tree with the given name and functions, made in a, or NULL when a
 * refuses. funcs holds Map pointers and is kept, so it has to outlive the
 * tree's parse. */
BURROW_OWNS(ret) ParseTree *parse_new(Alloc *a, Str name, Slice funcs);

/* Tree.Parse: parses text into t, with the given delimiters, or {{ and }} for
 * empty ones. Each template the text defines becomes a tree of its own, and t
 * and those trees go into tree_set, a Map from Str to ParseTree pointers, under
 * their names. Returns t, or NULL with the error in *err. funcs, when not
 * empty, are the function maps for this parse. */
ParseTree *parse_tree_parse(ParseTree *t, Str text, Str left_delim, Str right_delim,
                            Map *tree_set, Slice funcs, Error *err);

/* Parse: parses text as a template called name, in a, and returns the map of
 * the trees it holds, keyed by name, as parse_tree_parse fills one in. On an
 * error the map holds the trees parsed before it. NULL when a refuses. */
BURROW_OWNS(ret) Map *parse_parse(Alloc *a, Str name, Str text, Str left_delim,
                                  Str right_delim, Slice funcs, Error *err);

/* Tree.Copy: a copy of t and its nodes, made in a. The copy keeps t's text,
 * and its nodes keep t as their tree, as Go's do. NULL for a NULL t, or when a
 * refuses. */
BURROW_OWNS(ret) ParseTree *parse_tree_copy(ParseTree *t, Alloc *a);

/* Tree.ErrorContext: where n is, as name:line:column, with n written out in
 * *context, both made in a. It is for error messages, and uses the text of n's
 * own tree, or of t's if n has none. */
BURROW_OWNS(ret) Str parse_tree_error_context(ParseTree *t, Alloc *a, ParseNode n,
                                              Str *context);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TEXT_TEMPLATE_PARSE_H */
