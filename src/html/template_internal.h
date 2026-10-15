/* html/template internals shared by the content types, the contexts and their
 * transitions, the escapers and the tests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_HTML_TEMPLATE_INTERNAL_H
#define BURROW_HTML_TEMPLATE_INTERNAL_H

#include "burrow/html/template.h"

#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>

/* filterFailsafe, what a filter puts in place of a value it refuses. */
#define BURROW__HT_FILTER_FAILSAFE "ZgotmplZ"

/* contentType, with Go's order. */
typedef uint8_t burrow__HtContentType;

enum {
    BURROW__HT_CONTENT_PLAIN,
    BURROW__HT_CONTENT_CSS,
    BURROW__HT_CONTENT_HTML,
    BURROW__HT_CONTENT_HTML_ATTR,
    BURROW__HT_CONTENT_JS,
    BURROW__HT_CONTENT_JS_STR,
    BURROW__HT_CONTENT_URL,
    BURROW__HT_CONTENT_SRCSET,
    /* Content that is not safe to put in the attribute it names at all. */
    BURROW__HT_CONTENT_UNSAFE
};

/* state, the part of an HTML document the escaper is in, with Go's order. */
typedef uint8_t burrow__HtState;

enum {
    BURROW__HT_STATE_TEXT,              /* parsed character data */
    BURROW__HT_STATE_TAG,               /* before an attribute name, or the tag end */
    BURROW__HT_STATE_ATTR_NAME,         /* in an attribute name */
    BURROW__HT_STATE_AFTER_NAME,        /* after an attribute name, before any = */
    BURROW__HT_STATE_BEFORE_VALUE,      /* after the =, before the value */
    BURROW__HT_STATE_HTML_CMT,          /* in an HTML comment */
    BURROW__HT_STATE_RCDATA,            /* in a textarea or title element */
    BURROW__HT_STATE_ATTR,              /* in an attribute of no special kind */
    BURROW__HT_STATE_URL,               /* in a URL attribute */
    BURROW__HT_STATE_SRCSET,            /* in a srcset attribute */
    BURROW__HT_STATE_JS,                /* in script code */
    BURROW__HT_STATE_JS_DQ_STR,         /* in a JS "string" */
    BURROW__HT_STATE_JS_SQ_STR,         /* in a JS 'string' */
    BURROW__HT_STATE_JS_TMPL_LIT,       /* in a JS `template literal` */
    BURROW__HT_STATE_JS_REGEXP,         /* in a JS /regexp/ */
    BURROW__HT_STATE_JS_BLOCK_CMT,      /* in a JS block comment */
    BURROW__HT_STATE_JS_LINE_CMT,       /* in a JS // line comment */
    BURROW__HT_STATE_JS_HTML_OPEN_CMT,  /* in a JS <!-- line comment */
    BURROW__HT_STATE_JS_HTML_CLOSE_CMT, /* in a JS --> line comment */
    BURROW__HT_STATE_CSS,               /* in CSS */
    BURROW__HT_STATE_CSS_DQ_STR,        /* in a CSS "string" */
    BURROW__HT_STATE_CSS_SQ_STR,        /* in a CSS 'string' */
    BURROW__HT_STATE_CSS_DQ_URL,        /* in a CSS url("...") */
    BURROW__HT_STATE_CSS_SQ_URL,        /* in a CSS url('...') */
    BURROW__HT_STATE_CSS_URL,           /* in an unquoted CSS url(...) */
    BURROW__HT_STATE_CSS_BLOCK_CMT,     /* in a CSS block comment */
    BURROW__HT_STATE_CSS_LINE_CMT,      /* in a CSS // line comment */
    BURROW__HT_STATE_ERROR,             /* an unrecoverable error */
    BURROW__HT_STATE_META_CONTENT,      /* in a meta tag's content attribute */
    BURROW__HT_STATE_META_CONTENT_URL,  /* in the url= part of one */
    BURROW__HT_STATE_DEAD,              /* after a {{break}} or {{continue}} */
    BURROW__HT_STATE_COUNT
};

/* delim, what ends the attribute value the escaper is in. */
typedef uint8_t burrow__HtDelim;

enum {
    BURROW__HT_DELIM_NONE,
    BURROW__HT_DELIM_DOUBLE_QUOTE,
    BURROW__HT_DELIM_SINGLE_QUOTE,
    BURROW__HT_DELIM_SPACE_OR_TAG_END
};

/* urlPart, the part of a URL the escaper is in. */
typedef uint8_t burrow__HtUrlPart;

enum {
    BURROW__HT_URL_PART_NONE,
    BURROW__HT_URL_PART_PRE_QUERY,
    BURROW__HT_URL_PART_QUERY_OR_FRAG,
    BURROW__HT_URL_PART_UNKNOWN
};

/* jsCtx, whether a / in script code starts a regexp or is a division. */
typedef uint8_t burrow__HtJsCtx;

enum { BURROW__HT_JS_CTX_REGEXP, BURROW__HT_JS_CTX_DIV_OP, BURROW__HT_JS_CTX_UNKNOWN };

/* element, the element whose content needs special handling. */
typedef uint8_t burrow__HtElement;

enum {
    BURROW__HT_ELEMENT_NONE,
    BURROW__HT_ELEMENT_SCRIPT,
    BURROW__HT_ELEMENT_STYLE,
    BURROW__HT_ELEMENT_TEXTAREA,
    BURROW__HT_ELEMENT_TITLE,
    BURROW__HT_ELEMENT_META
};

/* attr, the kind of attribute value the escaper is in. */
typedef uint8_t burrow__HtAttr;

enum {
    BURROW__HT_ATTR_NONE,
    BURROW__HT_ATTR_SCRIPT,
    BURROW__HT_ATTR_SCRIPT_TYPE,
    BURROW__HT_ATTR_STYLE,
    BURROW__HT_ATTR_URL,
    BURROW__HT_ATTR_SRCSET,
    BURROW__HT_ATTR_META_CONTENT,
    BURROW__HT_ATTR_COUNT
};

/* context, the state of the HTML parser at one point in a template. Go passes
 * it by value and so does this, js_brace_depth included: it is a Slice of Int
 * with Go's []int semantics, so a copy shares the elements, nil and empty are
 * different, and burrow__ht_context_clone is what copies them. err is compared
 * by address, as Go compares the pointers. */
typedef struct burrow__HtContext {
    burrow__HtState state;
    burrow__HtDelim delim;
    burrow__HtUrlPart url_part;
    burrow__HtJsCtx js_ctx;
    Slice js_brace_depth;
    burrow__HtAttr attr;
    burrow__HtElement element;
    ParseNode n; /* for range break and continue */
    HtmlTemplateError *err;
} burrow__HtContext;

/* The zero context, Go's context{}. */
burrow__HtContext burrow__ht_context(void);

/* context.String, context.eq, context.mangle and context.clone. Those that
 * make something make it in a. */
Str burrow__ht_context_string(Alloc *a, burrow__HtContext c);
bool burrow__ht_context_eq(burrow__HtContext c, burrow__HtContext d);
Str burrow__ht_context_mangle(Alloc *a, burrow__HtContext c, Str template_name);
burrow__HtContext burrow__ht_context_clone(Alloc *a, burrow__HtContext c);

/* The String methods stringer writes for the enums. Out of range values print
 * as state(N) and the like, as Go's do. */
Str burrow__ht_state_string(Alloc *a, burrow__HtState s);
Str burrow__ht_delim_string(Alloc *a, burrow__HtDelim d);
Str burrow__ht_url_part_string(Alloc *a, burrow__HtUrlPart u);
Str burrow__ht_js_ctx_string(Alloc *a, burrow__HtJsCtx c);
Str burrow__ht_element_string(Alloc *a, burrow__HtElement e);
Str burrow__ht_attr_string(Alloc *a, burrow__HtAttr t);

/* isComment, isInTag and isInScriptLiteral. */
bool burrow__ht_is_comment(burrow__HtState s);
bool burrow__ht_is_in_tag(burrow__HtState s);
bool burrow__ht_is_in_script_literal(burrow__HtState s);

/* errorf with the description made already: a new Error in a. */
HtmlTemplateError *burrow__ht_errorf(Alloc *a, HtmlTemplateErrorCode k, ParseNode node,
                                     Int line, Str description);

/* s[i:j], for indexes the caller has checked. */
static inline Str burrow__ht_sub(Str s, Int i, Int j) {
    return j > i ? (Str){s.p + i, j - i} : BURROW_STR_EMPTY;
}

/* Panics with burrow_err_out_of_memory. */
BURROW_NORETURN void burrow__ht_oom(void);

/* Writes s to b, and panics when b cannot grow. */
void burrow__ht_write(StringsBuilder *b, Str s);

/* ------------------------------------------------------------ transitions */

/* A transition function: the context after the text s, which starts in c, and
 * in *nread how much of s it read to get there. What it makes, an error, a
 * longer js_brace_depth, goes in a. */
typedef burrow__HtContext (*burrow__HtTransition)(Alloc *a, burrow__HtContext c, Str s,
                                                  Int *nread);

/* transitionFunc, indexed by state. */
extern const burrow__HtTransition burrow__ht_transition_func[BURROW__HT_STATE_COUNT];

/* tSpecialTagEnd, the transition for raw text and RCDATA, which the escaper
 * runs before the state's own. */
burrow__HtContext burrow__ht_t_special_tag_end(Alloc *a, burrow__HtContext c, Str s,
                                               Int *nread);

/* attrStartStates: the state an attribute value of each kind starts in. */
extern const burrow__HtState burrow__ht_attr_start_states[BURROW__HT_ATTR_COUNT];

/* delimEnds: the bytes that end each delim. */
extern const Str burrow__ht_delim_ends[BURROW__HT_DELIM_SPACE_OR_TAG_END + 1];

/* indexTagEnd: where the end tag </tag in s starts, matching tag in any case,
 * or -1. */
Int burrow__ht_index_tag_end(Str s, Str tag);

/* eatWhiteSpace: the index of the first byte at or after i that is not HTML
 * space. */
Int burrow__ht_eat_white_space(Str s, Int i);

/* ---------------------------------------------------------------- content */

/* stringify: the one string, or what fmt_sprint makes of args, and the kind
 * of content it is. */
Str burrow__ht_stringify(Alloc *a, Slice args, burrow__HtContentType *t);

/* evalArgs: the one string, or what fmt_sprint makes of args with every
 * pointer followed, as stringify does but keeping nil arguments. */
Str burrow__ht_eval_args(Alloc *a, Slice args);

/* attrType: the kind of content the attribute name, in lower case, holds. */
burrow__HtContentType burrow__ht_attr_type(Str name);

/* --------------------------------------------------------------- escapers
 *
 * The escapers and filters the escaper adds to pipelines. Each takes the
 * pipeline's values as a Slice of Any and returns the text, made in a. */
Str burrow__ht_html_nospace_escaper(Alloc *a, Slice args);
Str burrow__ht_attr_escaper(Alloc *a, Slice args);
Str burrow__ht_rcdata_escaper(Alloc *a, Slice args);
Str burrow__ht_html_escaper(Alloc *a, Slice args);
Str burrow__ht_html_name_filter(Alloc *a, Slice args);
Str burrow__ht_comment_escaper(Alloc *a, Slice args);
Str burrow__ht_js_val_escaper(Alloc *a, Slice args);
Str burrow__ht_js_str_escaper(Alloc *a, Slice args);
Str burrow__ht_js_tmpl_lit_escaper(Alloc *a, Slice args);
Str burrow__ht_js_regexp_escaper(Alloc *a, Slice args);
Str burrow__ht_css_escaper(Alloc *a, Slice args);
Str burrow__ht_css_value_filter(Alloc *a, Slice args);
Str burrow__ht_url_filter(Alloc *a, Slice args);
Str burrow__ht_url_escaper(Alloc *a, Slice args);
Str burrow__ht_url_normalizer(Alloc *a, Slice args);
Str burrow__ht_srcset_filter_and_escaper(Alloc *a, Slice args);

/* stripTags: the text of an HTML fragment with the tags taken out. */
Str burrow__ht_strip_tags(Alloc *a, Str html);

/* nextJSCtx: the context after the script code s, which comes after code that
 * left preceding. */
burrow__HtJsCtx burrow__ht_next_js_ctx(Str s, burrow__HtJsCtx preceding);

/* isJSIdentPart and isJSType. */
bool burrow__ht_is_js_ident_part(Rune r);
bool burrow__ht_is_js_type(Str mime_type);

/* The CSS helpers. decodeCSS returns s itself when it has no escapes, and a
 * decoded copy made in a when it has. */
bool burrow__ht_ends_with_css_keyword(Str b, Str kw);
bool burrow__ht_is_css_nmchar(Rune r);
Str burrow__ht_decode_css(Alloc *a, Str s);
bool burrow__ht_is_hex(Byte c);
Rune burrow__ht_hex_decode(Str s);
Str burrow__ht_skip_css_space(Str c);

/* isSafeURL and processURLOnto. */
bool burrow__ht_is_safe_url(Str s);
bool burrow__ht_process_url_onto(Str s, bool norm, StringsBuilder *b);

/* ---------------------------------------------------------------- escaper */

/* rangeContext, what the escaper knows about the range loop it is in. breaks
 * and continues are Slices of burrow__HtContext *, the contexts at each break
 * and continue. */
typedef struct burrow__HtRangeContext burrow__HtRangeContext;

struct burrow__HtRangeContext {
    burrow__HtRangeContext *outer;
    Slice breaks;
    Slice continues;
};

/* escaper, what escaping has worked out about the templates in a name space
 * and the edits it is going to make to them. Its maps are made in a:
 *
 *     output, Str to burrow__HtContext *, the output context of each template
 *         name mangled with the context it starts in
 *     derived, Str to TextTemplate *, the templates made for a start context
 *     called, Str to bool, the mangled names that have been called
 *     action_node_edits, ParseActionNode * to a Slice of Str, the escapers to
 *         add to each action
 *     template_node_edits, ParseTemplateNode * to Str, the template to call
 *     text_node_edits, ParseTextNode * to Bytes, the text to put in
 *
 * The edits are kept until commit, rather than made as they are found, in case
 * a template is run in different contexts. */
typedef struct burrow__HtEscaper {
    burrow__HtNameSpace *ns;
    Alloc *a;
    Map *output;
    Map *derived;
    Map *called;
    Map *action_node_edits;
    Map *template_node_edits;
    Map *text_node_edits;
    burrow__HtRangeContext *range_context;
} burrow__HtEscaper;

/* nameSpace, what the templates associated with each other share. set maps
 * a Str to an HtmlTemplate *. a is where the templates, the escaper and what
 * it makes are. */
struct burrow__HtNameSpace {
    SyncMutex mu;
    Map *set;
    bool escaped;
    burrow__HtEscaper esc;
    Alloc *a;
};

/* escapeOK, the escape_err of a template that escaped. */
extern const Error burrow__ht_escape_ok;

/* Whether err is burrow__ht_escape_ok. */
static inline bool burrow__ht_is_escape_ok(Error err) {
    return err.vt == burrow__ht_escape_ok.vt && err.data == burrow__ht_escape_ok.data;
}

/* redundantFuncs, for the tests: the i'th pair {a, b} in the table of
 * escapers where b does nothing to what a gives back, so that a pipeline
 * ending in a needs no b after it. False past the end. */
bool burrow__ht_redundant_pair(Int i, Str *a, Str *b);

/* funcMap, for the tests: calls the escaping function called name with args,
 * a Slice of Any, making the result in a. The empty string for a name that is
 * not there. */
BURROW_OWNS(ret) Str burrow__ht_call_func(Alloc *a, Str name, Slice args);

/* escapeText, for the tests: what escaping the text node n in context c gives,
 * with an escaper made in a that has no templates, as makeEscaper(nil). */
burrow__HtContext burrow__ht_escape_text_node(Alloc *a, burrow__HtContext c,
                                              ParseTextNode *n);

/* ensurePipelineContains: puts the escapers named in s, a Slice of Str, at the
 * end of p, leaving out the ones p already ends with and using a predefined
 * escaper p ends with where it does the same job. */
void burrow__ht_ensure_pipeline_contains(Alloc *a, ParsePipeNode *p, Slice s);

/* makeEscaper, in *e. */
void burrow__ht_make_escaper(burrow__HtEscaper *e, burrow__HtNameSpace *ns);

/* escapeTemplate: rewrites the template called name in tmpl's name space, whose
 * root is node, so that its output is escaped. On an error the template is left
 * so that it cannot run. */
Error burrow__ht_escape_template(HtmlTemplate *tmpl, ParseNode node, Str name);

/* For the tests: reads the GODEBUG settings from value as if it were the
 * environment's, or from the environment again for NULL. */
void burrow__ht_godebug_set(const char *value);

#endif /* BURROW_HTML_TEMPLATE_INTERNAL_H */
