/* text/template/parse internals shared by the lexer, the nodes and the parser,
 * and the lexer's items, which the tests look at directly as Go's do.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TEXT_TEMPLATE_PARSE_INTERNAL_H
#define BURROW_TEXT_TEMPLATE_PARSE_INTERNAL_H

#include "burrow/text/template/parse.h"

/* itemType, with Go's order. Everything after BURROW__ITEM_KEYWORD is a
 * keyword. */
typedef enum burrow__ItemType {
    BURROW__ITEM_ERROR,         /* error occurred; val is the text of the error */
    BURROW__ITEM_BOOL,          /* boolean constant */
    BURROW__ITEM_CHAR,          /* printable ASCII character; grab bag for comma etc. */
    BURROW__ITEM_CHAR_CONSTANT, /* character constant */
    BURROW__ITEM_COMMENT,       /* comment text */
    BURROW__ITEM_COMPLEX, /* complex constant (1+2i); imaginary is just a number */
    BURROW__ITEM_ASSIGN,  /* equals ('=') introducing an assignment */
    BURROW__ITEM_DECLARE, /* colon-equals (':=') introducing a declaration */
    BURROW__ITEM_EOF,
    BURROW__ITEM_FIELD,       /* alphanumeric identifier starting with '.' */
    BURROW__ITEM_IDENTIFIER,  /* alphanumeric identifier not starting with '.' */
    BURROW__ITEM_LEFT_DELIM,  /* left action delimiter */
    BURROW__ITEM_LEFT_PAREN,  /* '(' inside action */
    BURROW__ITEM_NUMBER,      /* simple number, including imaginary */
    BURROW__ITEM_PIPE,        /* pipe symbol */
    BURROW__ITEM_RAW_STRING,  /* raw quoted string (includes quotes) */
    BURROW__ITEM_RIGHT_DELIM, /* right action delimiter */
    BURROW__ITEM_RIGHT_PAREN, /* ')' inside action */
    BURROW__ITEM_SPACE,       /* run of spaces separating arguments */
    BURROW__ITEM_STRING,      /* quoted string (includes quotes) */
    BURROW__ITEM_TEXT,        /* plain text */
    BURROW__ITEM_VARIABLE, /* variable starting with '$', such as '$' or '$1' or '$hello' */
    BURROW__ITEM_KEYWORD,  /* used only to delimit the keywords */
    BURROW__ITEM_BLOCK,    /* block keyword */
    BURROW__ITEM_BREAK,    /* break keyword */
    BURROW__ITEM_CONTINUE, /* continue keyword */
    BURROW__ITEM_DOT,      /* the cursor, spelled '.' */
    BURROW__ITEM_DEFINE,   /* define keyword */
    BURROW__ITEM_ELSE,     /* else keyword */
    BURROW__ITEM_END,      /* end keyword */
    BURROW__ITEM_IF,       /* if keyword */
    BURROW__ITEM_NIL,      /* the untyped nil constant, easiest to treat as a keyword */
    BURROW__ITEM_RANGE,    /* range keyword */
    BURROW__ITEM_TEMPLATE, /* template keyword */
    BURROW__ITEM_WITH      /* with keyword */
} burrow__ItemType;

/* item, a token and its value. val points into the input, apart from an
 * error's, which is made in the error allocator. */
typedef struct burrow__Item {
    burrow__ItemType typ; /* the type of this item */
    ParsePos pos; /* the starting position, in bytes, of this item in the input */
    Str val;      /* the value of this item */
    Int line;     /* the line number at the start of this item */
} burrow__Item;

/* lexOptions. */
typedef struct burrow__LexOptions {
    bool emit_comment; /* emit comment items */
    bool break_ok;     /* break keyword allowed */
    bool continue_ok;  /* continue keyword allowed */
} burrow__LexOptions;

/* lexer, which hands out one item at a time. */
typedef struct burrow__Lexer {
    Str name;           /* the name of the input; used only for error reports */
    Str input;          /* the string being scanned */
    Str left_delim;     /* start of action marker */
    Str right_delim;    /* end of action marker */
    ParsePos pos;       /* current position in the input */
    ParsePos start;     /* start position of this item */
    bool at_eof;        /* we have hit the end of input and returned eof */
    Int paren_depth;    /* nesting depth of ( ) exprs */
    Int line;           /* 1+number of newlines seen */
    Int start_line;     /* start line of this item */
    burrow__Item item;  /* item to return to parser */
    bool inside_action; /* are we inside an action? */
    burrow__LexOptions options;
} burrow__Lexer;

/* lex: a lexer for input, with {{ and }} for empty delimiters. */
void burrow__parse_lex(burrow__Lexer *l, Str name, Str input, Str left, Str right);

/* nextItem: the next item from the input. */
burrow__Item burrow__parse_lex_next_item(burrow__Lexer *l);

/* item.String, made in a. */
BURROW_OWNS(ret) Str burrow__parse_item_string(burrow__Item i, Alloc *a);

/* The else and end nodes, which the parser makes to find where a list ends. */
enum { BURROW__PARSE_NODE_ELSE = 6, BURROW__PARSE_NODE_END = 7 };

typedef struct burrow__ParseEndNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
} burrow__ParseEndNode;

typedef struct burrow__ParseElseNode {
    ParseNodeType node_type;
    ParsePos pos;
    ParseTree *tr;
    Int line;
} burrow__ParseElseNode;

/* Node.tree. */
ParseTree *burrow__parse_node_tree(ParseNode n);

/* A zeroed node of size bytes made in a, with its type, position and tree set,
 * or NULL when a refuses. */
void *burrow__parse_node_new(Alloc *a, size_t size, ParseNodeType typ, ParsePos pos,
                             ParseTree *tr);

/* append(*s, *elem) for one element, false when a refuses. A zero *s becomes a
 * slice of elem_type first. */
bool burrow__parse_append(Alloc *a, Slice *s, const Type *elem_type, const void *elem);

/* The nil pointer dereference Go would panic with on a missing node. */
BURROW_NORETURN void burrow__parse_nil_deref(void);

/* Tree.newNumber: the number node for text, an item of type typ, made in t's
 * allocator, or NULL with the error in *err. */
ParseNumberNode *burrow__parse_new_number(ParseTree *t, ParsePos pos, Str text,
                                          burrow__ItemType typ, Error *err);

/* textFormat, which the tests change from %s to %q so that a TextNode is
 * written out quoted. */
extern bool burrow__parse_text_format_q;

/* maxStackDepth, which the tests make small. */
extern Int burrow__parse_max_stack_depth;

#endif /* BURROW_TEXT_TEMPLATE_PARSE_INTERNAL_H */
