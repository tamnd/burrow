/* html/template's escaper. It walks a template, works out the context each
 * action's output lands in and adds the escapers for that context to the end
 * of the action's pipeline. It also changes the template's text where it has
 * to, and gives a template that is called in a context other than its own a
 * copy for that context.
 *
 * Derived from Go's src/html/template/escape.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/html.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/text/template.h"
#include "burrow/text/template/parse.h"
#include "burrow/type.h"
#include "template_internal.h"

#include <stdint.h>
#include <string.h>

#include "../text/template_internal.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef Slice HteStrs;
typedef Slice HteAnys;
static BURROW_SLICE_TYPE_DEFINE(HteStrs, Str);
static BURROW_SLICE_TYPE_DEFINE(HteAnys, Any);

/* ---------------------------------------------------------------- GODEBUG */

enum {
    HTE_DEBUG_KNOWN = 1 << 0,
    HTE_DEBUG_NO_META_URL = 1 << 1, /* htmlmetacontenturlescape=0 */
};

static uint32_t hte_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice, as Go's
 * internal/godebug reads it. */
static bool hte_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

/* The settings from a GODEBUG value, or the defaults for NULL. */
static uint32_t hte_debug_parse(const char *v) {
    uint32_t f = HTE_DEBUG_KNOWN;
    Str s;
    if (v != NULL && hte_godebug(v, "htmlmetacontenturlescape", &s) && s.len == 1 &&
        s.p[0] == '0')
        f |= HTE_DEBUG_NO_META_URL;
    burrow__atomic_store_relaxed_u32(&hte_debug_flags, f);
    return f;
}

static uint32_t hte_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&hte_debug_flags);
    if ((f & HTE_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return hte_debug_parse(v);
}

void burrow__ht_godebug_set(const char *value) {
    if (value == NULL) {
        burrow__atomic_store_relaxed_u32(&hte_debug_flags, 0);
        return;
    }
    hte_debug_parse(value);
}

/* ---------------------------------------------------------------- funcMap */

/* func(...any) string, the type of every function in funcMap. */
BURROW_FUNC(burrow__HtEscFn, Str, Slice args);
#define HTE_ESC_SIG(IN, OUT) IN(0, HteAnys) OUT(Str)
BURROW_FUNC_TYPE_DECL(burrow__HtEscFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__HtEscFn, Any, HTE_ESC_SIG);

/* The functions' env is the allocator of the execution that calls them, which
 * text/template puts there because their env starts as burrow__tt_exec_env. */
#define HTE_ESCAPER(name, fn)                                                          \
    static Str name(void *env, Slice args) {                                           \
        return fn((Alloc *)env, args);                                                 \
    }

HTE_ESCAPER(hte_attr_escaper, burrow__ht_attr_escaper)
HTE_ESCAPER(hte_comment_escaper, burrow__ht_comment_escaper)
HTE_ESCAPER(hte_css_escaper, burrow__ht_css_escaper)
HTE_ESCAPER(hte_css_value_filter, burrow__ht_css_value_filter)
HTE_ESCAPER(hte_html_name_filter, burrow__ht_html_name_filter)
HTE_ESCAPER(hte_html_escaper, burrow__ht_html_escaper)
HTE_ESCAPER(hte_js_regexp_escaper, burrow__ht_js_regexp_escaper)
HTE_ESCAPER(hte_js_str_escaper, burrow__ht_js_str_escaper)
HTE_ESCAPER(hte_js_tmpl_lit_escaper, burrow__ht_js_tmpl_lit_escaper)
HTE_ESCAPER(hte_js_val_escaper, burrow__ht_js_val_escaper)
HTE_ESCAPER(hte_html_nospace_escaper, burrow__ht_html_nospace_escaper)
HTE_ESCAPER(hte_rcdata_escaper, burrow__ht_rcdata_escaper)
HTE_ESCAPER(hte_srcset_escaper, burrow__ht_srcset_filter_and_escaper)
HTE_ESCAPER(hte_url_escaper, burrow__ht_url_escaper)
HTE_ESCAPER(hte_url_filter, burrow__ht_url_filter)
HTE_ESCAPER(hte_url_normalizer, burrow__ht_url_normalizer)
HTE_ESCAPER(hte_eval_args, burrow__ht_eval_args)

static const struct {
    Str name;
    Str (*f)(void *env, Slice args);
} hte_funcs[] = {
    {S_("_html_template_attrescaper"), hte_attr_escaper},
    {S_("_html_template_commentescaper"), hte_comment_escaper},
    {S_("_html_template_cssescaper"), hte_css_escaper},
    {S_("_html_template_cssvaluefilter"), hte_css_value_filter},
    {S_("_html_template_htmlnamefilter"), hte_html_name_filter},
    {S_("_html_template_htmlescaper"), hte_html_escaper},
    {S_("_html_template_jsregexpescaper"), hte_js_regexp_escaper},
    {S_("_html_template_jsstrescaper"), hte_js_str_escaper},
    {S_("_html_template_jstmpllitescaper"), hte_js_tmpl_lit_escaper},
    {S_("_html_template_jsvalescaper"), hte_js_val_escaper},
    {S_("_html_template_nospaceescaper"), hte_html_nospace_escaper},
    {S_("_html_template_rcdataescaper"), hte_rcdata_escaper},
    {S_("_html_template_srcsetescaper"), hte_srcset_escaper},
    {S_("_html_template_urlescaper"), hte_url_escaper},
    {S_("_html_template_urlfilter"), hte_url_filter},
    {S_("_html_template_urlnormalizer"), hte_url_normalizer},
    {S_("_eval_args_"), hte_eval_args},
};

Str burrow__ht_call_func(Alloc *a, Str name, Slice args) {
    for (Int i = 0; i < NELEM(hte_funcs); i++)
        if (str_eq(name, hte_funcs[i].name))
            return hte_funcs[i].f(a, args);
    return BURROW_STR_EMPTY;
}

/* Adds funcMap to t's functions. text_template_funcs copies the values, so
 * they can live on the stack. */
static void hte_add_funcs(Alloc *a, TextTemplate *t) {
    burrow__HtEscFn fns[NELEM(hte_funcs)];
    Map *m = map_make(a, TYPE_STRING, TYPE_ANY, NELEM(hte_funcs));
    if (m == NULL)
        burrow__ht_oom();
    for (Int i = 0; i < NELEM(hte_funcs); i++) {
        fns[i] =
            (burrow__HtEscFn){hte_funcs[i].f, (void *)(uintptr_t)&burrow__tt_exec_env};
        Any v = BURROW_ANY(&burrow_type_burrow__HtEscFn, &fns[i]);
        if (!map_set(m, &hte_funcs[i].name, &v))
            burrow__ht_oom();
    }
    text_template_funcs(t, m);
}

/* ---------------------------------------------------------------- helpers */

static void hte_set(Map *m, const void *k, const void *v) {
    if (!map_set(m, k, v))
        burrow__ht_oom();
}

static Map *hte_map(Alloc *a, const Type *k, const Type *v) {
    Map *m = map_make(a, k, v, 0);
    if (m == NULL)
        burrow__ht_oom();
    return m;
}

static void hte_copy_map(Map *dst, Map *src) {
    MapIter it = map_iter(src);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v))
        hte_set(dst, k, v);
}

static burrow__HtContext hte_state(burrow__HtState s) {
    burrow__HtContext c = burrow__ht_context();
    c.state = s;
    return c;
}

static burrow__HtContext hte_errorf(Alloc *a, HtmlTemplateErrorCode k, ParseNode node,
                                    Int line, Str description) {
    burrow__HtContext c = hte_state(BURROW__HT_STATE_ERROR);
    c.err = burrow__ht_errorf(a, k, node, line, description);
    return c;
}

static Str hte_concat(Alloc *a, Str x, Str y) {
    return fmt_sprintf_v(a, BURROW_S("%s%s"), x, y);
}

static BURROW_NORETURN void hte_shared(ParseNode n) {
    Alloc *a = error_allocator();
    panic_str(fmt_sprintf_v(a, BURROW_S("node %s shared between templates"),
                            parse_node_string(n, a)));
}

static burrow__HtContext *hte_new_ctx(Alloc *a, burrow__HtContext c) {
    burrow__HtContext *p = BURROW_NEW(a, burrow__HtContext);
    if (p == NULL)
        burrow__ht_oom();
    *p = c;
    return p;
}

static Slice hte_append_ctx(Alloc *a, Slice s, burrow__HtContext c) {
    void *p = hte_new_ctx(a, c);
    if (s.elem == NULL)
        s = slice_nil(TYPE_UNSAFE_POINTER);
    s = slice_append(a, s, &p, 1);
    if (s.p == NULL)
        burrow__ht_oom();
    return s;
}

static Str hte_ident(ParseNode n) {
    return ((const ParseIdentifierNode *)n)->ident;
}

static ParseNode hte_arg0(ParseNode cmd) {
    return BURROW_AT(ParseNode, ((const ParseCommandNode *)cmd)->args, 0);
}

/* ---------------------------------------------------------------- escaper */

/* makeEscaper, with a given apart, so that an escaper can be made without a
 * name space for the tests. */
static void hte_init_escaper(burrow__HtEscaper *e, burrow__HtNameSpace *ns, Alloc *a) {
    *e = (burrow__HtEscaper){
        ns,
        a,
        hte_map(a, TYPE_STRING, TYPE_UNSAFE_POINTER),
        hte_map(a, TYPE_STRING, TYPE_UNSAFE_POINTER),
        hte_map(a, TYPE_STRING, TYPE_BOOL),
        hte_map(a, TYPE_UNSAFE_POINTER, TYPE_OF(HteStrs)),
        hte_map(a, TYPE_UNSAFE_POINTER, TYPE_STRING),
        hte_map(a, TYPE_UNSAFE_POINTER, TYPE_BYTES),
        NULL,
    };
}

void burrow__ht_make_escaper(burrow__HtEscaper *e, burrow__HtNameSpace *ns) {
    hte_init_escaper(e, ns, ns->a);
}

static burrow__HtContext hte_escape_action(burrow__HtEscaper *e, burrow__HtContext c,
                                           ParseActionNode *n);
static burrow__HtContext hte_escape_branch(burrow__HtEscaper *e, burrow__HtContext c,
                                           ParseBranchNode *n, Str node_name);
static burrow__HtContext hte_escape_list(burrow__HtEscaper *e, burrow__HtContext c,
                                         ParseListNode *n);
static burrow__HtContext hte_escape_template(burrow__HtEscaper *e, burrow__HtContext c,
                                             ParseTemplateNode *n);
static burrow__HtContext hte_escape_text(burrow__HtEscaper *e, burrow__HtContext c,
                                         ParseTextNode *n);

/* escape: escapes a template node. */
static burrow__HtContext hte_escape(burrow__HtEscaper *e, burrow__HtContext c,
                                    ParseNode n) {
    switch (parse_node_type(n)) {
    case PARSE_NODE_ACTION:
        return hte_escape_action(e, c, (ParseActionNode *)n);
    case PARSE_NODE_BREAK:
        c.n = n;
        e->range_context->breaks = hte_append_ctx(e->a, e->range_context->breaks, c);
        return hte_state(BURROW__HT_STATE_DEAD);
    case PARSE_NODE_COMMENT:
        return c;
    case PARSE_NODE_CONTINUE:
        c.n = n;
        e->range_context->continues =
            hte_append_ctx(e->a, e->range_context->continues, c);
        return hte_state(BURROW__HT_STATE_DEAD);
    case PARSE_NODE_IF:
        return hte_escape_branch(e, c, &((ParseIfNode *)n)->branch_node,
                                 BURROW_S("if"));
    case PARSE_NODE_LIST:
        return hte_escape_list(e, c, (ParseListNode *)n);
    case PARSE_NODE_RANGE:
        return hte_escape_branch(e, c, &((ParseRangeNode *)n)->branch_node,
                                 BURROW_S("range"));
    case PARSE_NODE_TEMPLATE:
        return hte_escape_template(e, c, (ParseTemplateNode *)n);
    case PARSE_NODE_TEXT:
        return hte_escape_text(e, c, (ParseTextNode *)n);
    case PARSE_NODE_WITH:
        return hte_escape_branch(e, c, &((ParseWithNode *)n)->branch_node,
                                 BURROW_S("with"));
    default:
        break;
    }
    Alloc *a = error_allocator();
    panic_str(fmt_sprintf_v(a, BURROW_S("escaping %s is unimplemented"),
                            parse_node_string(n, a)));
}

/* predefinedEscapers: the escapers text/template predefines that are the
 * same as some of the contextual ones. Keep in step with equivEscapers. */
static bool hte_is_predefined(Str ident) {
    return str_eq(ident, BURROW_S("html")) || str_eq(ident, BURROW_S("urlquery"));
}

/* nudge: the context after the empty string transitions from c. For example,
 * `<a href=` ends in context{stateBeforeValue, attrURL}, but one rune more,
 * `<a href=x`, ends in context{stateURL, delimSpaceOrTagEnd, ...}. Two
 * transitions happen when the x is seen:
 *
 *     (1) from a before-value state to a start-of-value state, without
 *         consuming any character
 *     (2) consuming x and moving past the first character of the value
 *
 * Nudging gives the context after (1). */
static burrow__HtContext hte_nudge(burrow__HtContext c) {
    switch (c.state) {
    case BURROW__HT_STATE_TAG:
        /* In `<foo {{.}}`, the action should emit an attribute. */
        c.state = BURROW__HT_STATE_ATTR_NAME;
        break;
    case BURROW__HT_STATE_BEFORE_VALUE:
        /* In `<foo bar={{.}}`, the action is an undelimited value. */
        c.state = burrow__ht_attr_start_states[c.attr];
        c.delim = BURROW__HT_DELIM_SPACE_OR_TAG_END;
        c.attr = BURROW__HT_ATTR_NONE;
        break;
    case BURROW__HT_STATE_AFTER_NAME:
        /* In `<foo bar {{.}}`, the action is an attribute name. */
        c.state = BURROW__HT_STATE_ATTR_NAME;
        c.attr = BURROW__HT_ATTR_NONE;
        break;
    default:
        break;
    }
    return c;
}

/* editActionNode: records a change to an action's pipeline for commit. */
static void hte_edit_action_node(burrow__HtEscaper *e, ParseActionNode *n, Slice cmds) {
    void *k = n;
    if (map_get(e->action_node_edits, &k) != NULL)
        hte_shared(&n->node_type);
    hte_set(e->action_node_edits, &k, &cmds);
}

/* editTemplateNode: records a change to a {{template}} callee for commit. */
static void hte_edit_template_node(burrow__HtEscaper *e, ParseTemplateNode *n,
                                   Str callee) {
    void *k = n;
    if (map_get(e->template_node_edits, &k) != NULL)
        hte_shared(&n->node_type);
    hte_set(e->template_node_edits, &k, &callee);
}

/* editTextNode: records a change to a text node for commit. */
static void hte_edit_text_node(burrow__HtEscaper *e, ParseTextNode *n, Bytes text) {
    void *k = n;
    if (map_get(e->text_node_edits, &k) != NULL)
        hte_shared(&n->node_type);
    hte_set(e->text_node_edits, &k, &text);
}

/* escapeAction: escapes an action template node. */
static burrow__HtContext hte_escape_action(burrow__HtEscaper *e, burrow__HtContext c,
                                           ParseActionNode *n) {
    if (n->pipe->decl.len != 0) {
        /* A local variable assignment, not an interpolation. */
        return c;
    }
    c = hte_nudge(c);
    /* Check for disallowed use of predefined escapers in the pipeline. */
    for (Int pos = 0; pos < n->pipe->cmds.len; pos++) {
        ParseNode node = hte_arg0(BURROW_AT(ParseNode, n->pipe->cmds, pos));
        if (parse_node_type(node) != PARSE_NODE_IDENTIFIER) {
            /* A predefined escaper "esc" will never be found as an identifier
             * in a Chain or Field node, since:
             * - "esc.x ..." is invalid, since predefined escapers return
             *   strings, and strings do not have methods, keys or fields.
             * - "... .esc" is invalid, since predefined escapers are global
             *   functions, not methods or fields of any types.
             * Therefore, it is safe to ignore these two node types. */
            continue;
        }
        Str ident = hte_ident(node);
        if (hte_is_predefined(ident)) {
            if (pos < n->pipe->cmds.len - 1 ||
                (c.state == BURROW__HT_STATE_ATTR &&
                 c.delim == BURROW__HT_DELIM_SPACE_OR_TAG_END &&
                 str_eq(ident, BURROW_S("html"))))
                return hte_errorf(
                    e->a, HTML_TEMPLATE_ERR_PREDEFINED_ESCAPER, &n->node_type, n->line,
                    fmt_sprintf_v(
                        e->a, BURROW_S("predefined escaper %q disallowed in template"),
                        ident));
        }
    }
    Slice s = slice_make(e->a, TYPE_STRING, 0, 3);
    if (s.p == NULL)
        burrow__ht_oom();
#define HTE_ADD(lit) (((Str *)s.p)[s.len++] = BURROW_S(lit))
    switch (c.state) {
    case BURROW__HT_STATE_ERROR:
        return c;
    case BURROW__HT_STATE_URL:
    case BURROW__HT_STATE_CSS_DQ_STR:
    case BURROW__HT_STATE_CSS_SQ_STR:
    case BURROW__HT_STATE_CSS_DQ_URL:
    case BURROW__HT_STATE_CSS_SQ_URL:
    case BURROW__HT_STATE_CSS_URL:
        switch (c.url_part) {
        case BURROW__HT_URL_PART_NONE:
        case BURROW__HT_URL_PART_PRE_QUERY:
            if (c.url_part == BURROW__HT_URL_PART_NONE)
                HTE_ADD("_html_template_urlfilter");
            if (c.state == BURROW__HT_STATE_CSS_DQ_STR ||
                c.state == BURROW__HT_STATE_CSS_SQ_STR)
                HTE_ADD("_html_template_cssescaper");
            else
                HTE_ADD("_html_template_urlnormalizer");
            break;
        case BURROW__HT_URL_PART_QUERY_OR_FRAG:
            HTE_ADD("_html_template_urlescaper");
            break;
        case BURROW__HT_URL_PART_UNKNOWN:
            return hte_errorf(
                e->a, HTML_TEMPLATE_ERR_AMBIG_CONTEXT, &n->node_type, n->line,
                fmt_sprintf_v(
                    e->a, BURROW_S("%s appears in an ambiguous context within a URL"),
                    parse_node_string(&n->node_type, e->a)));
        default:
            panic_str(burrow__ht_url_part_string(error_allocator(), c.url_part));
        }
        break;
    case BURROW__HT_STATE_META_CONTENT:
        /* Handled below in delim check. */
        break;
    case BURROW__HT_STATE_META_CONTENT_URL:
        if ((hte_debug_load() & HTE_DEBUG_NO_META_URL) == 0)
            HTE_ADD("_html_template_urlfilter");
        break;
    case BURROW__HT_STATE_JS:
        HTE_ADD("_html_template_jsvalescaper");
        /* A slash after a value starts a div operator. */
        c.js_ctx = BURROW__HT_JS_CTX_DIV_OP;
        break;
    case BURROW__HT_STATE_JS_DQ_STR:
    case BURROW__HT_STATE_JS_SQ_STR:
        HTE_ADD("_html_template_jsstrescaper");
        break;
    case BURROW__HT_STATE_JS_TMPL_LIT:
        HTE_ADD("_html_template_jstmpllitescaper");
        break;
    case BURROW__HT_STATE_JS_REGEXP:
        HTE_ADD("_html_template_jsregexpescaper");
        break;
    case BURROW__HT_STATE_CSS:
        HTE_ADD("_html_template_cssvaluefilter");
        break;
    case BURROW__HT_STATE_TEXT:
        HTE_ADD("_html_template_htmlescaper");
        break;
    case BURROW__HT_STATE_RCDATA:
        HTE_ADD("_html_template_rcdataescaper");
        break;
    case BURROW__HT_STATE_ATTR:
        /* Handled below in delim check. */
        break;
    case BURROW__HT_STATE_ATTR_NAME:
    case BURROW__HT_STATE_TAG:
        c.state = BURROW__HT_STATE_ATTR_NAME;
        HTE_ADD("_html_template_htmlnamefilter");
        break;
    case BURROW__HT_STATE_SRCSET:
        HTE_ADD("_html_template_srcsetescaper");
        break;
    default:
        if (burrow__ht_is_comment(c.state)) {
            HTE_ADD("_html_template_commentescaper");
        } else {
            Alloc *a = error_allocator();
            panic_str(hte_concat(a, BURROW_S("unexpected state "),
                                 burrow__ht_state_string(a, c.state)));
        }
        break;
    }
    switch (c.delim) {
    case BURROW__HT_DELIM_NONE:
        /* No extra-escaping needed for raw text content. */
        break;
    case BURROW__HT_DELIM_SPACE_OR_TAG_END:
        HTE_ADD("_html_template_nospaceescaper");
        break;
    default:
        HTE_ADD("_html_template_attrescaper");
        break;
    }
#undef HTE_ADD
    hte_edit_action_node(e, n, s);
    return c;
}

/* equivEscapers: the contextual escapers that the predefined ones are
 * equivalent to. */
static const struct {
    Str escaper;
    Str predefined;
} hte_equiv_escapers[] = {
    /* The following pairs of HTML escapers provide equivalent security
     * guarantees, since they all escape '\000', '\'', '"', '&', '<', and '>'. */
    {S_("_html_template_attrescaper"), S_("html")},
    {S_("_html_template_htmlescaper"), S_("html")},
    {S_("_html_template_rcdataescaper"), S_("html")},
    /* These two URL escapers produce URLs safe for embedding in a URL query by
     * percent-encoding all the reserved characters specified in RFC 3986
     * Section 2.2. */
    {S_("_html_template_urlescaper"), S_("urlquery")},
    /* These two functions are not actually equivalent; urlquery is stricter as
     * it escapes reserved characters (e.g. '#'), while
     * _html_template_urlnormalizer does not. It is therefore only safe to
     * replace _html_template_urlnormalizer with urlquery (this happens in
     * ensurePipelineContains), but not the other way around. We keep this
     * entry around to preserve the behavior of templates written before Go
     * 1.9, which might depend on this substitution taking place. */
    {S_("_html_template_urlnormalizer"), S_("urlquery")},
};

/* normalizeEscFn: the same name for any two escapers that are equivalent. */
static Str hte_normalize_esc_fn(Str e) {
    for (Int i = 0; i < NELEM(hte_equiv_escapers); i++)
        if (str_eq(e, hte_equiv_escapers[i].escaper))
            return hte_equiv_escapers[i].predefined;
    return e;
}

/* escFnsEq: whether the two escaping functions are equivalent. */
static bool hte_esc_fns_eq(Str a, Str b) {
    return str_eq(hte_normalize_esc_fn(a), hte_normalize_esc_fn(b));
}

/* redundantFuncs: {a, b} means that funcMap[b](funcMap[a](x)) == funcMap[a](x)
 * for all x. */
static const struct {
    Str a;
    Str b;
} hte_redundant_funcs[] = {
    {S_("_html_template_commentescaper"), S_("_html_template_attrescaper")},
    {S_("_html_template_commentescaper"), S_("_html_template_htmlescaper")},
    {S_("_html_template_cssescaper"), S_("_html_template_attrescaper")},
    {S_("_html_template_jsregexpescaper"), S_("_html_template_attrescaper")},
    {S_("_html_template_jsstrescaper"), S_("_html_template_attrescaper")},
    {S_("_html_template_jstmpllitescaper"), S_("_html_template_attrescaper")},
    {S_("_html_template_urlescaper"), S_("_html_template_urlnormalizer")},
};

bool burrow__ht_redundant_pair(Int i, Str *a, Str *b) {
    if (i < 0 || i >= NELEM(hte_redundant_funcs))
        return false;
    *a = hte_redundant_funcs[i].a;
    *b = hte_redundant_funcs[i].b;
    return true;
}

static bool hte_is_redundant(Str a, Str b) {
    for (Int i = 0; i < NELEM(hte_redundant_funcs); i++)
        if (str_eq(a, hte_redundant_funcs[i].a) && str_eq(b, hte_redundant_funcs[i].b))
            return true;
    return false;
}

/* appendCmd: cmds with cmd at the end, unless cmd is redundant with the last
 * command. */
static Slice hte_append_cmd(Alloc *a, Slice cmds, ParseCommandNode *cmd) {
    if (cmds.len != 0) {
        ParseNode last = hte_arg0(BURROW_AT(ParseNode, cmds, cmds.len - 1));
        ParseNode next = hte_arg0(&cmd->node_type);
        if (parse_node_type(last) == PARSE_NODE_IDENTIFIER &&
            parse_node_type(next) == PARSE_NODE_IDENTIFIER &&
            hte_is_redundant(hte_ident(last), hte_ident(next)))
            return cmds;
    }
    ParseNode c = &cmd->node_type;
    if (cmds.elem == NULL)
        cmds = slice_nil(TYPE_UNSAFE_POINTER);
    cmds = slice_append(a, cmds, &c, 1);
    if (cmds.p == NULL)
        burrow__ht_oom();
    return cmds;
}

/* An identifier node for ident at pos, with no tree, as Go's
 * NewIdentifier(ident).SetTree(nil).SetPos(pos). */
static ParseIdentifierNode *hte_new_identifier(Alloc *a, Str ident, ParsePos pos) {
    ParseIdentifierNode *id = parse_new_identifier(a, ident);
    if (id == NULL)
        burrow__ht_oom();
    return parse_identifier_node_set_pos(parse_identifier_node_set_tree(id, NULL), pos);
}

/* newIdentCmd: a command holding one identifier node. */
static ParseCommandNode *hte_new_ident_cmd(Alloc *a, Str identifier, ParsePos pos) {
    ParseCommandNode *cmd = BURROW_NEW(a, ParseCommandNode);
    if (cmd == NULL)
        burrow__ht_oom();
    ParseNode id = &hte_new_identifier(a, identifier, pos)->node_type;
    cmd->node_type = PARSE_NODE_COMMAND;
    cmd->args = slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &id, 1);
    if (cmd->args.p == NULL)
        burrow__ht_oom();
    return cmd;
}

void burrow__ht_ensure_pipeline_contains(Alloc *a, ParsePipeNode *p, Slice s) {
    if (s.len == 0) {
        /* Do not rewrite pipeline if we have no escapers to insert. */
        return;
    }
    /* Precondition: p->cmds contains at most one predefined escaper and the
     * escaper will be present at the end of p->cmds. This precondition is
     * always true because of the checks in escapeAction. */
    Int pipeline_len = p->cmds.len;
    if (pipeline_len > 0) {
        ParseCommandNode *last_cmd =
            BURROW_AT(ParseCommandNode *, p->cmds, pipeline_len - 1);
        ParseNode id_node = hte_arg0(&last_cmd->node_type);
        if (parse_node_type(id_node) == PARSE_NODE_IDENTIFIER) {
            Str esc = hte_ident(id_node);
            if (hte_is_predefined(esc)) {
                /* Pipeline ends with a predefined escaper. */
                if (p->cmds.len == 1 && last_cmd->args.len > 1) {
                    /* Special case: pipeline is of the form {{ esc arg1 arg2
                     * ... argN }}, where esc is the predefined escaper, and
                     * arg1...argN are its arguments. Convert this into the
                     * equivalent form {{ _eval_args_ arg1 arg2 ... argN | esc
                     * }}, so that esc can be easily merged with the escapers
                     * in s. */
                    BURROW_AT(ParseNode, last_cmd->args, 0) =
                        &hte_new_identifier(a, BURROW_S("_eval_args_"),
                                            parse_node_position(id_node))
                             ->node_type;
                    p->cmds =
                        hte_append_cmd(a, p->cmds, hte_new_ident_cmd(a, esc, p->pos));
                    pipeline_len++;
                }
                /* If any of the commands in s that we are about to insert is
                 * equivalent to the predefined escaper, use the predefined
                 * escaper instead. */
                bool dup = false;
                for (Int i = 0; i < s.len; i++) {
                    if (hte_esc_fns_eq(esc, BURROW_AT(Str, s, i))) {
                        BURROW_AT(Str, s, i) = esc;
                        dup = true;
                    }
                }
                if (dup) {
                    /* The predefined escaper will already be inserted along
                     * with the escapers in s, so do not copy it to the
                     * rewritten pipeline. */
                    pipeline_len--;
                }
            }
        }
    }
    /* Rewrite the pipeline, creating the escapers in s at the end of the
     * pipeline. */
    Slice new_cmds =
        slice_make(a, TYPE_UNSAFE_POINTER, pipeline_len, pipeline_len + s.len);
    Slice inserted = slice_make(a, TYPE_STRING, 0, pipeline_len);
    if ((new_cmds.p == NULL && pipeline_len + s.len > 0) ||
        (inserted.p == NULL && pipeline_len > 0))
        burrow__ht_oom();
    for (Int i = 0; i < pipeline_len; i++) {
        ParseNode cmd = BURROW_AT(ParseNode, p->cmds, i);
        BURROW_AT(ParseNode, new_cmds, i) = cmd;
        ParseNode id_node = hte_arg0(cmd);
        if (parse_node_type(id_node) == PARSE_NODE_IDENTIFIER)
            ((Str *)inserted.p)[inserted.len++] =
                hte_normalize_esc_fn(hte_ident(id_node));
    }
    for (Int i = 0; i < s.len; i++) {
        Str name = BURROW_AT(Str, s, i);
        Str norm = hte_normalize_esc_fn(name);
        bool found = false;
        for (Int j = 0; j < inserted.len && !found; j++)
            found = str_eq(BURROW_AT(Str, inserted, j), norm);
        if (!found) {
            /* When two templates share an underlying parse tree via the use
             * of AddParseTree and one template is executed after the other,
             * this check ensures that escapers that were already inserted into
             * the pipeline on the first escaping pass do not get inserted
             * again. */
            new_cmds = hte_append_cmd(a, new_cmds, hte_new_ident_cmd(a, name, p->pos));
        }
    }
    p->cmds = new_cmds;
}

/* join: the context after a branch whose two ends are in x and y. It is an
 * error context when either is one, or when they differ. */
static burrow__HtContext hte_join(Alloc *a, burrow__HtContext x, burrow__HtContext y,
                                  ParseNode node, Str node_name) {
    if (x.state == BURROW__HT_STATE_ERROR)
        return x;
    if (y.state == BURROW__HT_STATE_ERROR)
        return y;
    if (x.state == BURROW__HT_STATE_DEAD)
        return y;
    if (y.state == BURROW__HT_STATE_DEAD)
        return x;
    if (burrow__ht_context_eq(x, y))
        return x;

    burrow__HtContext c = x;
    c.url_part = y.url_part;
    if (burrow__ht_context_eq(c, y)) {
        /* The contexts differ only by urlPart. */
        c.url_part = BURROW__HT_URL_PART_UNKNOWN;
        return c;
    }

    c = x;
    c.js_ctx = y.js_ctx;
    if (burrow__ht_context_eq(c, y)) {
        /* The contexts differ only by jsCtx. */
        c.js_ctx = BURROW__HT_JS_CTX_UNKNOWN;
        return c;
    }

    /* Allow a nudged context to join with an unnudged one. This means that
     *   <p title={{if .C}}{{.}}{{end}}
     * ends in an unquoted value state even though the else branch ends in
     * stateBeforeValue. */
    burrow__HtContext cn = hte_nudge(x);
    burrow__HtContext d = hte_nudge(y);
    if (!(burrow__ht_context_eq(cn, x) && burrow__ht_context_eq(d, y))) {
        burrow__HtContext j = hte_join(a, cn, d, node, node_name);
        if (j.state != BURROW__HT_STATE_ERROR)
            return j;
    }

    return hte_errorf(
        a, HTML_TEMPLATE_ERR_BRANCH_END, node, 0,
        fmt_sprintf_v(a, BURROW_S("{{%s}} branches end in different contexts: %s, %s"),
                      node_name, burrow__ht_context_string(a, x),
                      burrow__ht_context_string(a, y)));
}

static burrow__HtRangeContext *hte_new_range_context(Alloc *a,
                                                     burrow__HtRangeContext *outer) {
    burrow__HtRangeContext *rc = BURROW_NEW(a, burrow__HtRangeContext);
    if (rc == NULL)
        burrow__ht_oom();
    *rc = (burrow__HtRangeContext){outer, slice_nil(TYPE_UNSAFE_POINTER),
                                   slice_nil(TYPE_UNSAFE_POINTER)};
    return rc;
}

/* joinRange: merges the contexts at the break and continue statements into
 * the context of the body. In theory breaks could be treated differently from
 * continues, but for now it is enough to treat them both as going back to the
 * start of the loop, which may then stop. */
static burrow__HtContext hte_join_range(Alloc *a, burrow__HtContext c0,
                                        const burrow__HtRangeContext *rc) {
    for (Int i = 0; i < rc->breaks.len; i++) {
        const burrow__HtContext *c = BURROW_AT(burrow__HtContext *, rc->breaks, i);
        c0 = hte_join(a, c0, *c, c->n, BURROW_S("range"));
        if (c0.state == BURROW__HT_STATE_ERROR) {
            c0.err->line = ((const ParseBreakNode *)c->n)->line;
            c0.err->description =
                hte_concat(a, BURROW_S("at range loop break: "), c0.err->description);
            return c0;
        }
    }
    for (Int i = 0; i < rc->continues.len; i++) {
        const burrow__HtContext *c = BURROW_AT(burrow__HtContext *, rc->continues, i);
        c0 = hte_join(a, c0, *c, c->n, BURROW_S("range"));
        if (c0.state == BURROW__HT_STATE_ERROR) {
            c0.err->line = ((const ParseContinueNode *)c->n)->line;
            c0.err->description = hte_concat(a, BURROW_S("at range loop continue: "),
                                             c0.err->description);
            return c0;
        }
    }
    return c0;
}

/* The filter escapeListConditionally takes, with what it closes over. */
typedef bool (*HteFilter)(burrow__HtEscaper *e1, burrow__HtContext c, void *data);

/* escapeListConditionally: escapes a list node but keeps the edits and
 * inferences in e only when the inferences and the output context satisfy
 * filter. It returns the best guess at an output context, and in *ok whether
 * e was updated. */
static burrow__HtContext hte_escape_list_conditionally(burrow__HtEscaper *e,
                                                       burrow__HtContext c,
                                                       ParseListNode *n,
                                                       HteFilter filter, void *data,
                                                       bool *ok) {
    burrow__HtEscaper e1;
    hte_init_escaper(&e1, e->ns, e->a);
    e1.range_context = e->range_context;
    /* Make type inferences available to f. */
    hte_copy_map(e1.output, e->output);
    c = hte_escape_list(&e1, c, n);
    *ok = filter != NULL && filter(&e1, c, data);
    if (*ok) {
        /* Copy inferences and edits from e1 back into e. */
        hte_copy_map(e->output, e1.output);
        hte_copy_map(e->derived, e1.derived);
        hte_copy_map(e->called, e1.called);
        MapIter it = map_iter(e1.action_node_edits);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v))
            hte_edit_action_node(e, *(ParseActionNode *const *)k, *(const Slice *)v);
        it = map_iter(e1.template_node_edits);
        while (map_next(&it, &k, &v))
            hte_edit_template_node(e, *(ParseTemplateNode *const *)k, *(const Str *)v);
        it = map_iter(e1.text_node_edits);
        while (map_next(&it, &k, &v))
            hte_edit_text_node(e, *(ParseTextNode *const *)k, *(const Bytes *)v);
    }
    return c;
}

/* escapeBranch: escapes a branch template node: "if", "range" and "with". */
static burrow__HtContext hte_escape_branch(burrow__HtEscaper *e, burrow__HtContext c,
                                           ParseBranchNode *n, Str node_name) {
    bool is_range = str_eq(node_name, BURROW_S("range"));
    if (is_range)
        e->range_context = hte_new_range_context(e->a, e->range_context);
    burrow__HtContext c0 =
        hte_escape_list(e, burrow__ht_context_clone(e->a, c), n->list);
    if (is_range) {
        if (c0.state != BURROW__HT_STATE_ERROR)
            c0 = hte_join_range(e->a, c0, e->range_context);
        e->range_context = e->range_context->outer;
        if (c0.state == BURROW__HT_STATE_ERROR)
            return c0;

        /* The "true" branch of a "range" node can execute multiple times. We
         * check that executing n->list once results in the same context as
         * executing n->list twice. */
        e->range_context = hte_new_range_context(e->a, e->range_context);
        bool ok;
        burrow__HtContext c1 =
            hte_escape_list_conditionally(e, c0, n->list, NULL, NULL, &ok);
        c0 = hte_join(e->a, c0, c1, &n->node_type, node_name);
        if (c0.state == BURROW__HT_STATE_ERROR) {
            e->range_context = e->range_context->outer;
            /* Make clear that this is a problem on loop re-entry since
             * developers tend to overlook that branch when debugging
             * templates. */
            c0.err->line = n->line;
            c0.err->description = hte_concat(e->a, BURROW_S("on range loop re-entry: "),
                                             c0.err->description);
            return c0;
        }
        c0 = hte_join_range(e->a, c0, e->range_context);
        e->range_context = e->range_context->outer;
        if (c0.state == BURROW__HT_STATE_ERROR)
            return c0;
    }
    burrow__HtContext c1 =
        hte_escape_list(e, burrow__ht_context_clone(e->a, c), n->else_list);
    return hte_join(e->a, c0, c1, &n->node_type, node_name);
}

/* escapeList: escapes a list template node. */
static burrow__HtContext hte_escape_list(burrow__HtEscaper *e, burrow__HtContext c,
                                         ParseListNode *n) {
    if (n == NULL)
        return c;
    for (Int i = 0; i < n->nodes.len; i++) {
        c = hte_escape(e, c, BURROW_AT(ParseNode, n->nodes, i));
        if (c.state == BURROW__HT_STATE_DEAD)
            break;
    }
    return c;
}

/* arbitraryTemplate: a template from e's name space, any one. Panics when
 * there are none. */
static HtmlTemplate *hte_arbitrary_template(burrow__HtEscaper *e) {
    MapIter it = map_iter(e->ns->set);
    const void *k;
    void *v;
    if (map_next(&it, &k, &v))
        return *(HtmlTemplate **)v;
    panic_str(BURROW_S("no templates in name space"));
}

/* template: the template called name, a mangled name. */
static TextTemplate *hte_template(burrow__HtEscaper *e, Str name) {
    /* Any template from the name space associated with this escaper can be
     * used to look up templates in the underlying text/template name space. */
    TextTemplate *t = text_template_lookup(hte_arbitrary_template(e)->text, name);
    if (t == NULL) {
        TextTemplate **d = map_get(e->derived, &name);
        if (d != NULL)
            t = *d;
    }
    return t;
}

static burrow__HtContext hte_compute_out_ctx(burrow__HtEscaper *e, burrow__HtContext c,
                                             TextTemplate *t);

/* escapeTree: escapes the template called name starting in c as needed, and
 * returns its output context, with the name of the template that is to be
 * called in *dname. */
static burrow__HtContext hte_escape_tree(burrow__HtEscaper *e, burrow__HtContext c,
                                         ParseNode node, Str name, Int line,
                                         Str *dname) {
    /* Mangle the template name with the input context to produce a reliable
     * identifier. */
    *dname = burrow__ht_context_mangle(e->a, c, name);
    bool yes = true;
    hte_set(e->called, dname, &yes);
    burrow__HtContext **out = map_get(e->output, dname);
    if (out != NULL) {
        /* Already escaped. */
        return **out;
    }
    TextTemplate *t = hte_template(e, name);
    if (t == NULL) {
        /* Two cases: the template exists but is empty, or has never been
         * mentioned at all. Distinguish the cases in the error messages. */
        HtmlTemplate **h = map_get(e->ns->set, &name);
        if (h != NULL && *h != NULL)
            return hte_errorf(
                e->a, HTML_TEMPLATE_ERR_NO_SUCH_TEMPLATE, node, line,
                fmt_sprintf_v(e->a, BURROW_S("%q is an incomplete or empty template"),
                              name));
        return hte_errorf(e->a, HTML_TEMPLATE_ERR_NO_SUCH_TEMPLATE, node, line,
                          fmt_sprintf_v(e->a, BURROW_S("no such template %q"), name));
    }
    if (!str_eq(*dname, name)) {
        /* Use any template derived during an earlier call to escapeTemplate
         * with different top level templates, or clone if necessary. */
        TextTemplate *dt = hte_template(e, *dname);
        if (dt == NULL) {
            dt = text_template_new(e->a, *dname);
            ParseTree *tree = BURROW_NEW(e->a, ParseTree);
            if (dt == NULL || tree == NULL)
                burrow__ht_oom();
            tree->name = *dname;
            tree->a = e->a;
            tree->root = parse_list_node_copy_list(t->tree->root, e->a);
            if (tree->root == NULL)
                burrow__ht_oom();
            dt->tree = tree;
            hte_set(e->derived, dname, &dt);
        }
        t = dt;
    }
    return hte_compute_out_ctx(e, c, t);
}

/* escapeTemplate: escapes a {{template}} call node. */
static burrow__HtContext hte_escape_template(burrow__HtEscaper *e, burrow__HtContext c,
                                             ParseTemplateNode *n) {
    Str name;
    c = hte_escape_tree(e, c, &n->node_type, n->name, n->line, &name);
    if (!str_eq(name, n->name))
        hte_edit_template_node(e, n, name);
    return c;
}

/* What escapeTemplateBody's filter closes over. */
typedef struct HteBody {
    TextTemplate *t;
    burrow__HtContext c;
} HteBody;

static bool hte_body_filter(burrow__HtEscaper *e1, burrow__HtContext c1, void *data) {
    const HteBody *b = data;
    if (c1.state == BURROW__HT_STATE_ERROR) {
        /* Do not update the input escaper, e. */
        return false;
    }
    const bool *called = map_get(e1->called, &b->t->name);
    if (called == NULL || !*called) {
        /* If t is not recursively called, then c1 is an accurate output
         * context. */
        return true;
    }
    /* c1 is accurate if it matches our assumed output context. */
    return burrow__ht_context_eq(b->c, c1);
}

/* escapeTemplateBody: escapes t assuming it ends in c, and returns the best
 * guess at the output context, with whether the guess was right in *ok. */
static burrow__HtContext hte_escape_template_body(burrow__HtEscaper *e,
                                                  burrow__HtContext c, TextTemplate *t,
                                                  bool *ok) {
    HteBody b = {t, c};
    /* We need to assume an output context so that recursive template calls
     * take the fast path out of escapeTree instead of infinitely recurring.
     * Naively assuming that the input context is the same as the output works
     * >90% of the time. */
    burrow__HtContext *p = hte_new_ctx(e->a, c);
    hte_set(e->output, &t->name, &p);
    return hte_escape_list_conditionally(e, c, t->tree->root, hte_body_filter, &b, ok);
}

/* computeOutCtx: the output context of t when it starts in c, with what was
 * worked out on the way kept in e. */
static burrow__HtContext hte_compute_out_ctx(burrow__HtEscaper *e, burrow__HtContext c,
                                             TextTemplate *t) {
    /* Propagate context over the body. */
    bool ok;
    burrow__HtContext c1 = hte_escape_template_body(e, c, t, &ok);
    if (!ok) {
        /* Look for a fixed point by assuming c1 as the output context. */
        bool ok2;
        burrow__HtContext c2 = hte_escape_template_body(e, c1, t, &ok2);
        if (ok2) {
            c1 = c2;
            ok = true;
        }
        /* Use c1 as the error context if neither assumption worked. */
    }
    if (!ok && c1.state != BURROW__HT_STATE_ERROR)
        return hte_errorf(
            e->a, HTML_TEMPLATE_ERR_OUTPUT_CONTEXT, &t->tree->root->node_type, 0,
            fmt_sprintf_v(e->a,
                          BURROW_S("cannot compute output context for template %s"),
                          text_template_name(t)));
    return c1;
}

/* delimEnds: the bytes that end each delim. */
static const Str hte_delim_ends[] = {
    [BURROW__HT_DELIM_NONE] = S_(""),
    [BURROW__HT_DELIM_DOUBLE_QUOTE] = S_("\""),
    [BURROW__HT_DELIM_SINGLE_QUOTE] = S_("'"),
    /* Determined empirically by running the below in various browsers.
     * var div = document.createElement("DIV");
     * for (var i = 0; i < 0x10000; ++i) {
     *   div.innerHTML = "<span title=x" + String.fromCharCode(i) + "-bar>";
     *   if (div.getElementsByTagName("SPAN")[0].title.indexOf("bar") < 0)
     *     document.write("<p>U+" + i.toString(16));
     * } */
    [BURROW__HT_DELIM_SPACE_OR_TAG_END] = S_(" \t\n\f\r>"),
};

/* Per WHATWG HTML specification, section 4.12.1.3, there are extremely
 * complicated rules for how to handle the set of opening tags <!--, <script,
 * and </script when they appear in JS literals (i.e. strings, regexs, and
 * comments). The specification suggests a simple solution, rather than
 * implementing the arcane ABNF, which involves simply escaping the opening
 * bracket with \x3C. Go uses the regexp (?i)<(script|/script|!--) for this and
 * so does this, written out: under (?i) an s also matches U+017F, the long s,
 * and nothing else here folds to more than its two cases.
 *
 * The length of the match of the group at s[i], after a '<', or 0. */
static Int hte_special_script_tag(Str s, Int i) {
    if (i + 3 <= s.len && memcmp(s.p + i, "!--", 3) == 0)
        return 3;
    Int j = i;
    if (j < s.len && s.p[j] == '/')
        j++;
    if (j < s.len && (s.p[j] == 's' || s.p[j] == 'S'))
        j++;
    else if (j + 2 <= s.len && s.p[j] == 0xC5 && s.p[j + 1] == 0xBF)
        j += 2;
    else
        return 0;
    static const char rest[] = "cript";
    for (Int k = 0; k < 5; k++, j++)
        if (j >= s.len || (s.p[j] | 0x20) != (Byte)rest[k])
            return 0;
    return j - i;
}

/* containsSpecialScriptTag. */
static bool hte_contains_special_script_tag(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == '<' && hte_special_script_tag(s, i + 1) > 0)
            return true;
    return false;
}

/* escapeSpecialScriptTags, written to b. */
static void hte_escape_special_script_tags(StringsBuilder *b, Str s) {
    Int written = 0;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] != '<')
            continue;
        Int n = hte_special_script_tag(s, i + 1);
        if (n == 0)
            continue;
        burrow__ht_write(b, burrow__ht_sub(s, written, i));
        burrow__ht_write(b, BURROW_S("\\x3C"));
        burrow__ht_write(b, burrow__ht_sub(s, i + 1, i + 1 + n));
        written = i + 1 + n;
        i = written - 1;
    }
    burrow__ht_write(b, burrow__ht_sub(s, written, s.len));
}

/* Whether s starts with <!DOCTYPE in any case. Go upper cases all of s first,
 * but no rune upper cases to one of DOCTYPE's letters apart from that letter's
 * lower case, so ASCII folding gives the same answer. */
static bool hte_has_doctype(Str s) {
    static const char doctype[] = "<!DOCTYPE";
    if (s.len < 9)
        return false;
    for (Int i = 0; i < 9; i++) {
        Byte c = s.p[i];
        if (c >= 'a' && c <= 'z')
            c = (Byte)(c - 'a' + 'A');
        if (c != (Byte)doctype[i])
            return false;
    }
    return true;
}

/* contextAfterText: starts in c, consumes some tokens from the front of s, and
 * returns the context after them, with how much of s it read in *nread. */
static burrow__HtContext hte_context_after_text(Alloc *a, burrow__HtContext c, Str s,
                                                Int *nread) {
    if (c.delim == BURROW__HT_DELIM_NONE) {
        Int i;
        burrow__HtContext c1 = burrow__ht_t_special_tag_end(a, c, s, &i);
        if (i == 0) {
            /* A special end tag (`</script>`) has been seen and all content
             * preceding it has been consumed. */
            *nread = 0;
            return c1;
        }
        /* Consider all content up to any end tag. */
        return burrow__ht_transition_func[c.state](a, c, burrow__ht_sub(s, 0, i),
                                                   nread);
    }

    /* We are at the beginning of an attribute value. */

    Int i = strings_index_any(s, hte_delim_ends[c.delim]);
    if (i == -1)
        i = s.len;
    if (c.delim == BURROW__HT_DELIM_SPACE_OR_TAG_END) {
        /* https://www.w3.org/TR/html5/syntax.html#attribute-value-(unquoted)-state
         * lists the runes below as error characters. Error out because HTML
         * parsers may differ on whether
         *   "<a id= onclick=f("     ends inside id's or onclick's value,
         *   "<a class=`foo "        ends inside a value,
         *   "<a style=font:'Arial'" needs open-quote fixup.
         * IE treats '`' as a quotation character. */
        Int j = strings_index_any(burrow__ht_sub(s, 0, i), BURROW_S("\"'<=`"));
        if (j >= 0) {
            *nread = s.len;
            return hte_errorf(a, HTML_TEMPLATE_ERR_BAD_HTML, NULL, 0,
                              fmt_sprintf_v(a, BURROW_S("%q in unquoted attr: %q"),
                                            burrow__ht_sub(s, j, j + 1),
                                            burrow__ht_sub(s, 0, i)));
        }
    }
    if (i == s.len) {
        /* Remain inside the attribute. Decode the value so non-HTML rules can
         * easily handle
         *     <button onclick="alert(&quot;Hi!&quot;)">
         * without having to entity decode token boundaries. */
        for (Str u = html_unescape_string(a, s); u.len != 0;) {
            Int i1;
            c = burrow__ht_transition_func[c.state](a, c, u, &i1);
            u = burrow__ht_sub(u, i1, u.len);
        }
        *nread = s.len;
        return c;
    }

    burrow__HtElement element = c.element;

    /* If this is a non-JS "type" attribute inside "script" tag, do not treat
     * the contents as JS. */
    if (c.state == BURROW__HT_STATE_ATTR && c.element == BURROW__HT_ELEMENT_SCRIPT &&
        c.attr == BURROW__HT_ATTR_SCRIPT_TYPE &&
        !burrow__ht_is_js_type(burrow__ht_sub(s, 0, i)))
        element = BURROW__HT_ELEMENT_NONE;

    if (c.delim != BURROW__HT_DELIM_SPACE_OR_TAG_END) {
        /* Consume any quote. */
        i++;
    }
    /* On exiting an attribute, we discard all state information except the
     * state and element. */
    burrow__HtContext r = hte_state(BURROW__HT_STATE_TAG);
    r.element = element;
    *nread = i;
    return r;
}

/* escapeText: escapes a text template node. */
static burrow__HtContext hte_escape_text(burrow__HtEscaper *e, burrow__HtContext c,
                                         ParseTextNode *n) {
    Str s = {(const Byte *)n->text.p, n->text.len};
    Int written = 0;
    Int i = 0;
    StringsBuilder b = STRINGS_BUILDER(e->a);
    while (i != s.len) {
        Int nread;
        burrow__HtContext c1 =
            hte_context_after_text(e->a, c, burrow__ht_sub(s, i, s.len), &nread);
        Int i1 = i + nread;
        if (c.state == BURROW__HT_STATE_TEXT || c.state == BURROW__HT_STATE_RCDATA) {
            Int end = i1;
            if (c1.state != c.state) {
                for (Int j = end - 1; j >= i; j--) {
                    if (s.p[j] == '<') {
                        end = j;
                        break;
                    }
                }
            }
            for (Int j = i; j < end; j++) {
                if (s.p[j] == '<' && !hte_has_doctype(burrow__ht_sub(s, j, s.len))) {
                    burrow__ht_write(&b, burrow__ht_sub(s, written, j));
                    burrow__ht_write(&b, BURROW_S("&lt;"));
                    written = j + 1;
                }
            }
        } else if (burrow__ht_is_comment(c.state) && c.delim == BURROW__HT_DELIM_NONE) {
            switch (c.state) {
            case BURROW__HT_STATE_JS_BLOCK_CMT:
                /* https://es5.github.io/#x7.4: "Comments behave like white
                 * space and are discarded except that, if a MultiLineComment
                 * contains a line terminator character, then the entire
                 * comment is considered to be a LineTerminator for purposes of
                 * parsing by the syntactic grammar." */
                if (strings_contains_any(burrow__ht_sub(s, written, i1),
                                         BURROW_S("\n\r\xE2\x80\xA8\xE2\x80\xA9")))
                    burrow__ht_write(&b, BURROW_S("\n"));
                else
                    burrow__ht_write(&b, BURROW_S(" "));
                break;
            case BURROW__HT_STATE_CSS_BLOCK_CMT:
                burrow__ht_write(&b, BURROW_S(" "));
                break;
            default:
                break;
            }
            written = i1;
        }
        if (c.state != c1.state && burrow__ht_is_comment(c1.state) &&
            c1.delim == BURROW__HT_DELIM_NONE) {
            /* Preserve the portion between written and the comment start. */
            Int cs = i1 - 2;
            if (c1.state == BURROW__HT_STATE_HTML_CMT ||
                c1.state == BURROW__HT_STATE_JS_HTML_OPEN_CMT) {
                /* "<!--" instead of "/" "*" or "//" */
                cs -= 2;
            } else if (c1.state == BURROW__HT_STATE_JS_HTML_CLOSE_CMT) {
                /* "-->" instead of "/" "*" or "//" */
                cs -= 1;
            }
            burrow__ht_write(&b, burrow__ht_sub(s, written, cs));
            written = i1;
        }
        if (burrow__ht_is_in_script_literal(c.state) &&
            hte_contains_special_script_tag(burrow__ht_sub(s, i, i1))) {
            burrow__ht_write(&b, burrow__ht_sub(s, written, i));
            hte_escape_special_script_tags(&b, burrow__ht_sub(s, i, i1));
            written = i1;
        }
        if (i == i1 && c.state == c1.state) {
            Alloc *a = error_allocator();
            panic_str(fmt_sprintf_v(
                a, BURROW_S("infinite loop from %s to %s on %q..%q"),
                burrow__ht_context_string(a, c), burrow__ht_context_string(a, c1),
                burrow__ht_sub(s, 0, i), burrow__ht_sub(s, i, s.len)));
        }
        c = c1;
        i = i1;
    }

    if (written != 0 && c.state != BURROW__HT_STATE_ERROR) {
        if (!burrow__ht_is_comment(c.state) || c.delim != BURROW__HT_DELIM_NONE)
            burrow__ht_write(&b, burrow__ht_sub(s, written, s.len));
        Str out = strings_builder_string(&b);
        hte_edit_text_node(
            e, n, (Bytes){(void *)(uintptr_t)out.p, out.len, out.len, TYPE_BYTE});
    }
    return c;
}

burrow__HtContext burrow__ht_escape_text_node(Alloc *a, burrow__HtContext c,
                                              ParseTextNode *n) {
    burrow__HtEscaper e;
    hte_init_escaper(&e, NULL, a);
    return hte_escape_text(&e, c, n);
}

/* commit: makes the changes to actions and template calls that escaping
 * worked out, and adds the derived templates to the set. */
static void hte_commit(burrow__HtEscaper *e) {
    MapIter it = map_iter(e->output);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        TextTemplate *t = hte_template(e, *(const Str *)k);
        hte_add_funcs(e->a, t);
    }
    /* Any template from the name space associated with this escaper can be
     * used to add derived templates to the underlying text/template name
     * space. */
    HtmlTemplate *tmpl = hte_arbitrary_template(e);
    it = map_iter(e->derived);
    while (map_next(&it, &k, &v)) {
        TextTemplate *t = *(TextTemplate **)v;
        Error err = BURROW_NO_ERROR;
        if (text_template_add_parse_tree(tmpl->text, text_template_name(t), t->tree,
                                         &err) == NULL ||
            BURROW_FAILED(err))
            panic_str(BURROW_S("error adding derived template"));
    }
    it = map_iter(e->action_node_edits);
    while (map_next(&it, &k, &v))
        burrow__ht_ensure_pipeline_contains(e->a, (*(ParseActionNode *const *)k)->pipe,
                                            *(const Slice *)v);
    it = map_iter(e->template_node_edits);
    while (map_next(&it, &k, &v))
        (*(ParseTemplateNode *const *)k)->name = *(const Str *)v;
    it = map_iter(e->text_node_edits);
    while (map_next(&it, &k, &v))
        (*(ParseTextNode *const *)k)->text = *(const Bytes *)v;
    /* Reset state that is specific to this commit so that the same changes
     * are not re-applied to the template on subsequent calls to commit. */
    e->called = hte_map(e->a, TYPE_STRING, TYPE_BOOL);
    e->action_node_edits = hte_map(e->a, TYPE_UNSAFE_POINTER, TYPE_OF(HteStrs));
    e->template_node_edits = hte_map(e->a, TYPE_UNSAFE_POINTER, TYPE_STRING);
    e->text_node_edits = hte_map(e->a, TYPE_UNSAFE_POINTER, TYPE_BYTES);
}

Error burrow__ht_escape_template(HtmlTemplate *tmpl, ParseNode node, Str name) {
    burrow__HtNameSpace *ns = tmpl->ns;
    Str dname;
    burrow__HtContext c =
        hte_escape_tree(&ns->esc, burrow__ht_context(), node, name, 0, &dname);
    HtmlTemplateError *herr = NULL;
    if (c.err != NULL) {
        herr = c.err;
        herr->name = name;
    } else if (c.state != BURROW__HT_STATE_TEXT) {
        herr = burrow__ht_errorf(
            ns->a, HTML_TEMPLATE_ERR_END_CONTEXT, NULL, 0,
            fmt_sprintf_v(ns->a, BURROW_S("ends in a non-text context: %s"),
                          burrow__ht_context_string(ns->a, c)));
        herr->name = name;
    }
    HtmlTemplate **t = map_get(ns->set, &name);
    if (herr != NULL) {
        Error err = html_template_error_as_error(ns->a, herr);
        /* Prevent execution of unsafe templates. */
        if (t != NULL && *t != NULL) {
            (*t)->escape_err = err;
            (*t)->text->tree = NULL;
            (*t)->tree = NULL;
        }
        return err;
    }
    hte_commit(&ns->esc);
    if (t != NULL && *t != NULL) {
        (*t)->escape_err = burrow__ht_escape_ok;
        (*t)->tree = (*t)->text->tree;
    }
    return BURROW_NO_ERROR;
}
