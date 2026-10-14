/* html/template: the context the escaper is in at each point of a template,
 * and the transition functions that move it over the template's text.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"
#include "template_internal.h"

#include <stdint.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------- the enum names */

static const Str htx_state_names[] = {
    S_("stateText"),           S_("stateTag"),         S_("stateAttrName"),
    S_("stateAfterName"),      S_("stateBeforeValue"), S_("stateHTMLCmt"),
    S_("stateRCDATA"),         S_("stateAttr"),        S_("stateURL"),
    S_("stateSrcset"),         S_("stateJS"),          S_("stateJSDqStr"),
    S_("stateJSSqStr"),        S_("stateJSTmplLit"),   S_("stateJSRegexp"),
    S_("stateJSBlockCmt"),     S_("stateJSLineCmt"),   S_("stateJSHTMLOpenCmt"),
    S_("stateJSHTMLCloseCmt"), S_("stateCSS"),         S_("stateCSSDqStr"),
    S_("stateCSSSqStr"),       S_("stateCSSDqURL"),    S_("stateCSSSqURL"),
    S_("stateCSSURL"),         S_("stateCSSBlockCmt"), S_("stateCSSLineCmt"),
    S_("stateError"),          S_("stateMetaContent"), S_("stateMetaContentURL"),
    S_("stateDead"),
};

static const Str htx_delim_names[] = {
    S_("delimNone"),
    S_("delimDoubleQuote"),
    S_("delimSingleQuote"),
    S_("delimSpaceOrTagEnd"),
};

static const Str htx_url_part_names[] = {
    S_("urlPartNone"),
    S_("urlPartPreQuery"),
    S_("urlPartQueryOrFrag"),
    S_("urlPartUnknown"),
};

static const Str htx_js_ctx_names[] = {
    S_("jsCtxRegexp"),
    S_("jsCtxDivOp"),
    S_("jsCtxUnknown"),
};

static const Str htx_element_names[] = {
    S_("elementNone"),     S_("elementScript"), S_("elementStyle"),
    S_("elementTextarea"), S_("elementTitle"),  S_("elementMeta"),
};

static const Str htx_attr_names[] = {
    S_("attrNone"), S_("attrScript"), S_("attrScriptType"),  S_("attrStyle"),
    S_("attrURL"),  S_("attrSrcset"), S_("attrMetaContent"),
};

/* What stringer writes: the name, or TYPE(N) for a value with none. */
static Str htx_enum_string(Alloc *a, const Str *names, Int n, Str type, uint8_t v) {
    if (v < n)
        return names[v];
    return fmt_sprintf_v(a, BURROW_S("%s(%d)"), type, (Int)v);
}

Str burrow__ht_state_string(Alloc *a, burrow__HtState s) {
    return htx_enum_string(a, htx_state_names, NELEM(htx_state_names),
                           BURROW_S("state"), s);
}

Str burrow__ht_delim_string(Alloc *a, burrow__HtDelim d) {
    return htx_enum_string(a, htx_delim_names, NELEM(htx_delim_names),
                           BURROW_S("delim"), d);
}

Str burrow__ht_url_part_string(Alloc *a, burrow__HtUrlPart u) {
    return htx_enum_string(a, htx_url_part_names, NELEM(htx_url_part_names),
                           BURROW_S("urlPart"), u);
}

Str burrow__ht_js_ctx_string(Alloc *a, burrow__HtJsCtx c) {
    return htx_enum_string(a, htx_js_ctx_names, NELEM(htx_js_ctx_names),
                           BURROW_S("jsCtx"), c);
}

Str burrow__ht_element_string(Alloc *a, burrow__HtElement e) {
    return htx_enum_string(a, htx_element_names, NELEM(htx_element_names),
                           BURROW_S("element"), e);
}

Str burrow__ht_attr_string(Alloc *a, burrow__HtAttr t) {
    return htx_enum_string(a, htx_attr_names, NELEM(htx_attr_names), BURROW_S("attr"),
                           t);
}

/* -------------------------------------------------------------- context */

burrow__HtContext burrow__ht_context(void) {
    burrow__HtContext c = {0};
    c.js_brace_depth = slice_nil(TYPE_INT);
    return c;
}

static burrow__HtContext htx_ctx(burrow__HtState state, burrow__HtElement element) {
    burrow__HtContext c = burrow__ht_context();
    c.state = state;
    c.element = element;
    return c;
}

static burrow__HtContext htx_ctx_err(HtmlTemplateError *err) {
    burrow__HtContext c = htx_ctx(BURROW__HT_STATE_ERROR, BURROW__HT_ELEMENT_NONE);
    c.err = err;
    return c;
}

static Int *htx_depth(burrow__HtContext c) {
    return (Int *)c.js_brace_depth.p;
}

/* fmt's %v of a []int. */
static void htx_write_ints(StringsBuilder *b, Slice s) {
    burrow__ht_write(b, BURROW_S("["));
    for (Int i = 0; i < s.len; i++) {
        if (i > 0)
            burrow__ht_write(b, BURROW_S(" "));
        burrow__ht_write(b, strconv_itoa(b->a, ((const Int *)s.p)[i]));
    }
    burrow__ht_write(b, BURROW_S("]"));
}

Str burrow__ht_context_string(Alloc *a, burrow__HtContext c) {
    StringsBuilder b = STRINGS_BUILDER(a);
    burrow__ht_write(&b, BURROW_S("{"));
    burrow__ht_write(&b, burrow__ht_state_string(a, c.state));
    burrow__ht_write(&b, BURROW_S(" "));
    burrow__ht_write(&b, burrow__ht_delim_string(a, c.delim));
    burrow__ht_write(&b, BURROW_S(" "));
    burrow__ht_write(&b, burrow__ht_url_part_string(a, c.url_part));
    burrow__ht_write(&b, BURROW_S(" "));
    burrow__ht_write(&b, burrow__ht_js_ctx_string(a, c.js_ctx));
    burrow__ht_write(&b, BURROW_S(" "));
    htx_write_ints(&b, c.js_brace_depth);
    burrow__ht_write(&b, BURROW_S(" "));
    burrow__ht_write(&b, burrow__ht_attr_string(a, c.attr));
    burrow__ht_write(&b, BURROW_S(" "));
    burrow__ht_write(&b, burrow__ht_element_string(a, c.element));
    burrow__ht_write(&b, BURROW_S(" "));
    if (c.err != NULL)
        burrow__ht_write(&b, html_template_error_error(c.err, a));
    else
        burrow__ht_write(&b, BURROW_S("<nil>"));
    burrow__ht_write(&b, BURROW_S("}"));
    return strings_builder_string(&b);
}

bool burrow__ht_context_eq(burrow__HtContext c, burrow__HtContext d) {
    if (c.state != d.state || c.delim != d.delim || c.url_part != d.url_part ||
        c.js_ctx != d.js_ctx || c.js_brace_depth.len != d.js_brace_depth.len ||
        c.attr != d.attr || c.element != d.element || c.err != d.err)
        return false;
    for (Int i = 0; i < c.js_brace_depth.len; i++)
        if (htx_depth(c)[i] != htx_depth(d)[i])
            return false;
    return true;
}

/* mangle: a name for the template that is template_name when the context is
 * the start state, and different for every other context. */
Str burrow__ht_context_mangle(Alloc *a, burrow__HtContext c, Str template_name) {
    /* The mangled name for the default context is the input template_name. */
    if (c.state == BURROW__HT_STATE_TEXT)
        return template_name;
    StringsBuilder b = STRINGS_BUILDER(a);
    burrow__ht_write(&b, template_name);
    burrow__ht_write(&b, BURROW_S("$htmltemplate_"));
    burrow__ht_write(&b, burrow__ht_state_string(a, c.state));
    if (c.delim != BURROW__HT_DELIM_NONE) {
        burrow__ht_write(&b, BURROW_S("_"));
        burrow__ht_write(&b, burrow__ht_delim_string(a, c.delim));
    }
    if (c.url_part != BURROW__HT_URL_PART_NONE) {
        burrow__ht_write(&b, BURROW_S("_"));
        burrow__ht_write(&b, burrow__ht_url_part_string(a, c.url_part));
    }
    if (c.js_ctx != BURROW__HT_JS_CTX_REGEXP) {
        burrow__ht_write(&b, BURROW_S("_"));
        burrow__ht_write(&b, burrow__ht_js_ctx_string(a, c.js_ctx));
    }
    if (!slice_is_nil(c.js_brace_depth)) {
        burrow__ht_write(&b, BURROW_S("_jsBraceDepth("));
        htx_write_ints(&b, c.js_brace_depth);
        burrow__ht_write(&b, BURROW_S(")"));
    }
    if (c.attr != BURROW__HT_ATTR_NONE) {
        burrow__ht_write(&b, BURROW_S("_"));
        burrow__ht_write(&b, burrow__ht_attr_string(a, c.attr));
    }
    if (c.element != BURROW__HT_ELEMENT_NONE) {
        burrow__ht_write(&b, BURROW_S("_"));
        burrow__ht_write(&b, burrow__ht_element_string(a, c.element));
    }
    return strings_builder_string(&b);
}

/* clone: c with js_brace_depth copied, as slices.Clone does it, so that nil
 * stays nil and empty stays empty. */
burrow__HtContext burrow__ht_context_clone(Alloc *a, burrow__HtContext c) {
    Slice d = c.js_brace_depth;
    if (d.len == 0) {
        c.js_brace_depth.cap = 0;
        return c;
    }
    Slice n = slice_make(a, TYPE_INT, d.len, d.len);
    if (n.p == NULL)
        burrow__ht_oom();
    slice_copy(n, d);
    c.js_brace_depth = n;
    return c;
}

bool burrow__ht_is_comment(burrow__HtState s) {
    switch (s) {
    case BURROW__HT_STATE_HTML_CMT:
    case BURROW__HT_STATE_JS_BLOCK_CMT:
    case BURROW__HT_STATE_JS_LINE_CMT:
    case BURROW__HT_STATE_JS_HTML_OPEN_CMT:
    case BURROW__HT_STATE_JS_HTML_CLOSE_CMT:
    case BURROW__HT_STATE_CSS_BLOCK_CMT:
    case BURROW__HT_STATE_CSS_LINE_CMT:
        return true;
    default:
        return false;
    }
}

bool burrow__ht_is_in_tag(burrow__HtState s) {
    switch (s) {
    case BURROW__HT_STATE_TAG:
    case BURROW__HT_STATE_ATTR_NAME:
    case BURROW__HT_STATE_AFTER_NAME:
    case BURROW__HT_STATE_BEFORE_VALUE:
    case BURROW__HT_STATE_ATTR:
        return true;
    default:
        return false;
    }
}

bool burrow__ht_is_in_script_literal(burrow__HtState s) {
    /* Ignore the comment states (stateJSBlockCmt, stateJSLineCmt,
     * stateJSHTMLOpenCmt, stateJSHTMLCloseCmt) because their content is already
     * omitted from the output. */
    switch (s) {
    case BURROW__HT_STATE_JS_DQ_STR:
    case BURROW__HT_STATE_JS_SQ_STR:
    case BURROW__HT_STATE_JS_TMPL_LIT:
    case BURROW__HT_STATE_JS_REGEXP:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- helpers */

static bool htx_has_prefix_at(Str s, Int i, Str p) {
    return i + p.len <= s.len && str_eq(burrow__ht_sub(s, i, i + p.len), p);
}

static Int htx_index_from(Str s, Int k, Byte c) {
    Int i = strings_index_byte(burrow__ht_sub(s, k, s.len), c);
    return i < 0 ? -1 : k + i;
}

static Int htx_index_any_from(Str s, Int k, Str chars) {
    Int i = strings_index_any(burrow__ht_sub(s, k, s.len), chars);
    return i < 0 ? -1 : k + i;
}

static bool htx_ascii_alpha(Byte c) {
    return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z');
}

static bool htx_ascii_alpha_num(Byte c) {
    return htx_ascii_alpha(c) || ('0' <= c && c <= '9');
}

static const Str htx_comment_start = S_("<!--");
static const Str htx_comment_end = S_("-->");

/* eatAttrName: the index just past the attribute name that starts at s[i], or
 * -1 and an error in *err when the name has a byte no browser allows. */
static Int htx_eat_attr_name(Alloc *a, Str s, Int i, HtmlTemplateError **err) {
    *err = NULL;
    for (Int j = i; j < s.len; j++) {
        switch (s.p[j]) {
        case ' ':
        case '\t':
        case '\n':
        case '\f':
        case '\r':
        case '=':
        case '>':
            return j;
        case '\'':
        case '"':
        case '<':
            /* These result in a parse warning in HTML5 and are indicative of
             * serious problems if seen in an attr name in a template. */
            *err = burrow__ht_errorf(
                a, HTML_TEMPLATE_ERR_BAD_HTML, NULL, 0,
                fmt_sprintf_v(a, BURROW_S("%q in attribute name: %.32q"),
                              burrow__ht_sub(s, j, j + 1), s));
            return -1;
        default:
            /* No-op. */
            break;
        }
    }
    return s.len;
}

/* elementNameMap, which only the ASCII names below can match. */
static burrow__HtElement htx_element_named(Str name) {
    static const struct {
        Str name;
        burrow__HtElement e;
    } m[] = {
        {S_("script"), BURROW__HT_ELEMENT_SCRIPT},
        {S_("style"), BURROW__HT_ELEMENT_STYLE},
        {S_("textarea"), BURROW__HT_ELEMENT_TEXTAREA},
        {S_("title"), BURROW__HT_ELEMENT_TITLE},
        {S_("meta"), BURROW__HT_ELEMENT_META},
    };
    for (Int i = 0; i < NELEM(m); i++)
        if (strings_equal_fold(name, m[i].name))
            return m[i].e;
    return BURROW__HT_ELEMENT_NONE;
}

/* eatTagName: the index just past the tag name that starts at s[i], and the
 * element it names. The name is ASCII, so comparing it to the element names
 * folding case is what Go's strings.ToLower and map lookup do. */
static Int htx_eat_tag_name(Str s, Int i, burrow__HtElement *e) {
    *e = BURROW__HT_ELEMENT_NONE;
    if (i == s.len || !htx_ascii_alpha(s.p[i]))
        return i;
    Int j = i + 1;
    while (j < s.len) {
        Byte x = s.p[j];
        if (htx_ascii_alpha_num(x)) {
            j++;
            continue;
        }
        /* Allow "x-y" or "x:y" but not "x-", "-y", or "x--y". */
        if ((x == ':' || x == '-') && j + 1 < s.len &&
            htx_ascii_alpha_num(s.p[j + 1])) {
            j += 2;
            continue;
        }
        break;
    }
    *e = htx_element_named(burrow__ht_sub(s, i, j));
    return j;
}

Int burrow__ht_eat_white_space(Str s, Int i) {
    for (Int j = i; j < s.len; j++) {
        switch (s.p[j]) {
        case ' ':
        case '\t':
        case '\n':
        case '\f':
        case '\r':
            /* No-op. */
            break;
        default:
            return j;
        }
    }
    return s.len;
}

/* -------------------------------------------------------------- transitions */

/* tText is the context transition function for the text state. */
static burrow__HtContext htx_t_text(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    (void)a;
    Int k = 0;
    for (;;) {
        Int i = htx_index_from(s, k, '<');
        if (i < k || i + 1 == s.len) {
            *nread = s.len;
            return c;
        }
        if (htx_has_prefix_at(s, i, htx_comment_start)) {
            *nread = i + 4;
            return htx_ctx(BURROW__HT_STATE_HTML_CMT, BURROW__HT_ELEMENT_NONE);
        }
        i++;
        bool end = false;
        if (s.p[i] == '/') {
            if (i + 1 == s.len) {
                *nread = s.len;
                return c;
            }
            end = true;
            i++;
        }
        burrow__HtElement e;
        Int j = htx_eat_tag_name(s, i, &e);
        if (j != i) {
            if (end)
                e = BURROW__HT_ELEMENT_NONE;
            /* We've found an HTML tag. */
            *nread = j;
            return htx_ctx(BURROW__HT_STATE_TAG, e);
        }
        k = j;
    }
}

static const burrow__HtState htx_element_content_type[] = {
    BURROW__HT_STATE_TEXT,   /* elementNone */
    BURROW__HT_STATE_JS,     /* elementScript */
    BURROW__HT_STATE_CSS,    /* elementStyle */
    BURROW__HT_STATE_RCDATA, /* elementTextarea */
    BURROW__HT_STATE_RCDATA, /* elementTitle */
    BURROW__HT_STATE_TEXT,   /* elementMeta */
};

/* tTag is the context transition function for the tag state. */
static burrow__HtContext htx_t_tag(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    /* Find the attribute name. */
    Int i = burrow__ht_eat_white_space(s, 0);
    if (i == s.len) {
        *nread = s.len;
        return c;
    }
    if (s.p[i] == '>') {
        *nread = i + 1;
        /* Treat <meta> specially, because it doesn't have an end tag, and we
         * want to transition into the correct state/element for it. */
        if (c.element == BURROW__HT_ELEMENT_META)
            return htx_ctx(BURROW__HT_STATE_TEXT, BURROW__HT_ELEMENT_NONE);
        return htx_ctx(htx_element_content_type[c.element], c.element);
    }
    HtmlTemplateError *err;
    Int j = htx_eat_attr_name(a, s, i, &err);
    if (err != NULL) {
        *nread = s.len;
        return htx_ctx_err(err);
    }
    burrow__HtState state = BURROW__HT_STATE_TAG;
    burrow__HtAttr attr = BURROW__HT_ATTR_NONE;
    if (i == j) {
        *nread = s.len;
        return htx_ctx_err(burrow__ht_errorf(
            a, HTML_TEMPLATE_ERR_BAD_HTML, NULL, 0,
            fmt_sprintf_v(
                a, BURROW_S("expected space, attr name, or end of tag, but got %q"),
                burrow__ht_sub(s, i, s.len))));
    }

    Str attr_name = strings_to_lower(a, burrow__ht_sub(s, i, j));
    if (c.element == BURROW__HT_ELEMENT_SCRIPT && str_eq(attr_name, BURROW_S("type"))) {
        attr = BURROW__HT_ATTR_SCRIPT_TYPE;
    } else if (c.element == BURROW__HT_ELEMENT_META &&
               str_eq(attr_name, BURROW_S("content"))) {
        attr = BURROW__HT_ATTR_META_CONTENT;
    } else {
        switch (burrow__ht_attr_type(attr_name)) {
        case BURROW__HT_CONTENT_URL:
            attr = BURROW__HT_ATTR_URL;
            break;
        case BURROW__HT_CONTENT_CSS:
            attr = BURROW__HT_ATTR_STYLE;
            break;
        case BURROW__HT_CONTENT_JS:
            attr = BURROW__HT_ATTR_SCRIPT;
            break;
        case BURROW__HT_CONTENT_SRCSET:
            attr = BURROW__HT_ATTR_SRCSET;
            break;
        default:
            break;
        }
    }

    if (j == s.len)
        state = BURROW__HT_STATE_ATTR_NAME;
    else
        state = BURROW__HT_STATE_AFTER_NAME;
    *nread = j;
    burrow__HtContext d = htx_ctx(state, c.element);
    d.attr = attr;
    return d;
}

/* tAttrName is the context transition function for stateAttrName. */
static burrow__HtContext htx_t_attr_name(Alloc *a, burrow__HtContext c, Str s,
                                         Int *nread) {
    HtmlTemplateError *err;
    Int i = htx_eat_attr_name(a, s, 0, &err);
    if (err != NULL) {
        *nread = s.len;
        return htx_ctx_err(err);
    }
    if (i != s.len)
        c.state = BURROW__HT_STATE_AFTER_NAME;
    *nread = i;
    return c;
}

/* tAfterName is the context transition function for stateAfterName. */
static burrow__HtContext htx_t_after_name(Alloc *a, burrow__HtContext c, Str s,
                                          Int *nread) {
    (void)a;
    /* Look for the start of the value. */
    Int i = burrow__ht_eat_white_space(s, 0);
    if (i == s.len) {
        *nread = s.len;
        return c;
    }
    if (s.p[i] != '=') {
        /* Occurs due to tag ending '>', and valueless attribute. */
        c.state = BURROW__HT_STATE_TAG;
        *nread = i;
        return c;
    }
    c.state = BURROW__HT_STATE_BEFORE_VALUE;
    /* Consume the "=". */
    *nread = i + 1;
    return c;
}

static const burrow__HtState htx_attr_start_states[] = {
    BURROW__HT_STATE_ATTR,         /* attrNone */
    BURROW__HT_STATE_JS,           /* attrScript */
    BURROW__HT_STATE_ATTR,         /* attrScriptType */
    BURROW__HT_STATE_CSS,          /* attrStyle */
    BURROW__HT_STATE_URL,          /* attrURL */
    BURROW__HT_STATE_SRCSET,       /* attrSrcset */
    BURROW__HT_STATE_META_CONTENT, /* attrMetaContent */
};

/* tBeforeValue is the context transition function for stateBeforeValue. */
static burrow__HtContext htx_t_before_value(Alloc *a, burrow__HtContext c, Str s,
                                            Int *nread) {
    (void)a;
    Int i = burrow__ht_eat_white_space(s, 0);
    if (i == s.len) {
        *nread = s.len;
        return c;
    }
    burrow__HtDelim delim = BURROW__HT_DELIM_SPACE_OR_TAG_END;
    switch (s.p[i]) {
    case '\'':
        delim = BURROW__HT_DELIM_SINGLE_QUOTE;
        i++;
        break;
    case '"':
        delim = BURROW__HT_DELIM_DOUBLE_QUOTE;
        i++;
        break;
    default:
        break;
    }
    c.state = htx_attr_start_states[c.attr];
    c.delim = delim;
    *nread = i;
    return c;
}

/* tHTMLCmt is the context transition function for stateHTMLCmt. */
static burrow__HtContext htx_t_html_cmt(Alloc *a, burrow__HtContext c, Str s,
                                        Int *nread) {
    (void)a;
    Int i = strings_index(s, htx_comment_end);
    if (i != -1) {
        *nread = i + 3;
        return burrow__ht_context();
    }
    *nread = s.len;
    return c;
}

/* specialTagEndMarkers maps element types to the character sequence that case
 * insensitively signals the end of the special tag body. */
static const Str htx_special_tag_end_markers[] = {
    S_(""),         /* elementNone, which is never looked up */
    S_("script"),   /* elementScript */
    S_("style"),    /* elementStyle */
    S_("textarea"), /* elementTextarea */
    S_("title"),    /* elementTitle */
    S_(""),         /* elementMeta */
};

static const Str htx_special_tag_end_prefix = S_("</");
static const Str htx_tag_end_separators = S_("> \t\n\f/");

/* tSpecialTagEnd is the context transition function for raw text and RCDATA
 * element states. */
static burrow__HtContext htx_t_special_tag_end(Alloc *a, burrow__HtContext c, Str s,
                                               Int *nread) {
    (void)a;
    if (c.element != BURROW__HT_ELEMENT_NONE) {
        /* script end tags ("</script") within script literals are ignored, so
         * that we can properly escape them. */
        if (c.element == BURROW__HT_ELEMENT_SCRIPT &&
            (burrow__ht_is_in_script_literal(c.state) ||
             burrow__ht_is_comment(c.state))) {
            *nread = s.len;
            return c;
        }
        Int i = burrow__ht_index_tag_end(s, htx_special_tag_end_markers[c.element]);
        if (i != -1) {
            *nread = i;
            return burrow__ht_context();
        }
    }
    *nread = s.len;
    return c;
}

Int burrow__ht_index_tag_end(Str s, Str tag) {
    Int res = 0;
    Int plen = htx_special_tag_end_prefix.len;
    while (s.len > 0) {
        /* Try to find the tag end prefix first. */
        Int i = strings_index(s, htx_special_tag_end_prefix);
        if (i == -1)
            return i;
        s = burrow__ht_sub(s, i + plen, s.len);
        /* Try to match the actual tag if there is still space for it. */
        if (tag.len <= s.len &&
            strings_equal_fold(tag, burrow__ht_sub(s, 0, tag.len))) {
            s = burrow__ht_sub(s, tag.len, s.len);
            /* Check the tag is followed by a proper separator. */
            /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
            if (s.len > 0 && strings_index_byte(htx_tag_end_separators, s.p[0]) != -1)
                return res + i;
            res += tag.len;
        }
        res += i + plen;
    }
    return -1;
}

/* tAttr is the context transition function for the attribute state. */
static burrow__HtContext htx_t_attr(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    (void)a;
    *nread = s.len;
    return c;
}

/* tURL is the context transition function for the URL state. */
static burrow__HtContext htx_t_url(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    (void)a;
    if (strings_contains_any(s, BURROW_S("#?")))
        c.url_part = BURROW__HT_URL_PART_QUERY_OR_FRAG;
    else if (s.len != burrow__ht_eat_white_space(s, 0) &&
             c.url_part == BURROW__HT_URL_PART_NONE)
        /* HTML5 uses "Valid URL potentially surrounded by spaces" for attrs:
         * https://www.w3.org/TR/html5/index.html#attributes-1 */
        c.url_part = BURROW__HT_URL_PART_PRE_QUERY;
    *nread = s.len;
    return c;
}

/* The one byte at s[i], as nextJSCtx is given it. */
static Str htx_byte_at(Str s, Int i) {
    return burrow__ht_sub(s, i, i + 1);
}

/* tJS is the context transition function for the JS state. */
static burrow__HtContext htx_t_js(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    Int i = strings_index_any(s, BURROW_S("\"`'/{}<-#"));
    if (i == -1) {
        /* Entire input is non string, comment, regexp tokens. */
        c.js_ctx = burrow__ht_next_js_ctx(s, c.js_ctx);
        *nread = s.len;
        return c;
    }
    c.js_ctx = burrow__ht_next_js_ctx(burrow__ht_sub(s, 0, i), c.js_ctx);
    switch (s.p[i]) {
    case '"':
        c.state = BURROW__HT_STATE_JS_DQ_STR;
        c.js_ctx = BURROW__HT_JS_CTX_REGEXP;
        break;
    case '\'':
        c.state = BURROW__HT_STATE_JS_SQ_STR;
        c.js_ctx = BURROW__HT_JS_CTX_REGEXP;
        break;
    case '`':
        c.state = BURROW__HT_STATE_JS_TMPL_LIT;
        c.js_ctx = BURROW__HT_JS_CTX_REGEXP;
        break;
    case '/':
        if (i + 1 < s.len && s.p[i + 1] == '/') {
            c.state = BURROW__HT_STATE_JS_LINE_CMT;
            i++;
        } else if (i + 1 < s.len && s.p[i + 1] == '*') {
            c.state = BURROW__HT_STATE_JS_BLOCK_CMT;
            i++;
        } else if (c.js_ctx == BURROW__HT_JS_CTX_REGEXP) {
            c.state = BURROW__HT_STATE_JS_REGEXP;
        } else if (c.js_ctx == BURROW__HT_JS_CTX_DIV_OP) {
            c.js_ctx = BURROW__HT_JS_CTX_REGEXP;
        } else {
            *nread = s.len;
            return htx_ctx_err(burrow__ht_errorf(
                a, HTML_TEMPLATE_ERR_SLASH_AMBIG, NULL, 0,
                fmt_sprintf_v(a,
                              BURROW_S("'/' could start a division or regexp: %.32q"),
                              burrow__ht_sub(s, i, s.len))));
        }
        break;
    case '<':
        if (i + 3 < s.len && htx_has_prefix_at(s, i, htx_comment_start)) {
            c.state = BURROW__HT_STATE_JS_HTML_OPEN_CMT;
            i += 3;
        }
        break;
    case '-':
        if (i + 2 < s.len && htx_has_prefix_at(s, i, htx_comment_end)) {
            c.state = BURROW__HT_STATE_JS_HTML_CLOSE_CMT;
            i += 2;
        }
        break;
    /* ECMAScript supports HTML style comments for legacy reasons, see Appendix
     * B.1.1 "HTML-like Comments". The handling of these comments is somewhat
     * confusing. Multi-line comments are not supported, i.e. anything on lines
     * between the opening and closing tokens is not considered a comment, but
     * anything following the opening or closing token, on the same line, is
     * ignored. As such we simply treat any line prefixed with "<!--" or "-->"
     * as if it were actually prefixed with "//" and move on. */
    case '#':
        if (i + 1 < s.len && s.p[i + 1] == '!') {
            c.state = BURROW__HT_STATE_JS_LINE_CMT;
            i++;
        }
        break;
    case '{':
        /* We only care about tracking brace depth if we are inside of a
         * template literal. */
        if (c.js_brace_depth.len == 0) {
            c.js_ctx = burrow__ht_next_js_ctx(htx_byte_at(s, i), c.js_ctx);
            *nread = i + 1;
            return c;
        }
        htx_depth(c)[c.js_brace_depth.len - 1]++;
        c.js_ctx = burrow__ht_next_js_ctx(htx_byte_at(s, i), c.js_ctx);
        break;
    case '}':
        if (c.js_brace_depth.len == 0) {
            c.js_ctx = burrow__ht_next_js_ctx(htx_byte_at(s, i), c.js_ctx);
            *nread = i + 1;
            return c;
        }
        /* There are no cases where a brace can be escaped in the JS context
         * that are not syntax errors, it seems. Because of this we can just
         * count "\}" as "}" and move on, the script is already broken as fully
         * fledged parsers will just fail anyway. */
        htx_depth(c)[c.js_brace_depth.len - 1]--;
        if (htx_depth(c)[c.js_brace_depth.len - 1] >= 0) {
            c.js_ctx = burrow__ht_next_js_ctx(htx_byte_at(s, i), c.js_ctx);
            *nread = i + 1;
            return c;
        }
        c.js_brace_depth = slice_sub(c.js_brace_depth, 0, c.js_brace_depth.len - 1);
        c.state = BURROW__HT_STATE_JS_TMPL_LIT;
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    *nread = i + 1;
    return c;
}

static Str htx_err_partial_escape(Alloc *a, Str what, Str s) {
    return fmt_sprintf_v(a, BURROW_S("unfinished escape sequence in %s: %q"), what, s);
}

static burrow__HtContext htx_t_js_tmpl(Alloc *a, burrow__HtContext c, Str s,
                                       Int *nread) {
    Int k = 0;
    for (;;) {
        Int i = htx_index_any_from(s, k, BURROW_S("`\\$"));
        if (i < k)
            break;
        switch (s.p[i]) {
        case '\\':
            i++;
            if (i == s.len) {
                *nread = s.len;
                return htx_ctx_err(burrow__ht_errorf(
                    a, HTML_TEMPLATE_ERR_PARTIAL_ESCAPE, NULL, 0,
                    htx_err_partial_escape(a, BURROW_S("JS string"), s)));
            }
            break;
        case '$':
            if (s.len >= i + 2 && s.p[i + 1] == '{') {
                Int zero = 0;
                c.js_brace_depth = slice_append(a, c.js_brace_depth, &zero, 1);
                if (c.js_brace_depth.p == NULL)
                    burrow__ht_oom();
                c.state = BURROW__HT_STATE_JS;
                *nread = i + 2;
                return c;
            }
            break;
        case '`':
            /* End of template literal. */
            c.state = BURROW__HT_STATE_JS;
            *nread = i + 1;
            return c;
        default:
            break;
        }
        k = i + 1;
    }
    *nread = s.len;
    return c;
}

/* tJSDelimited is the context transition function for the JS string and
 * regexp states. */
static burrow__HtContext htx_t_js_delimited(Alloc *a, burrow__HtContext c, Str s,
                                            Int *nread) {
    Str specials = BURROW_S("\\\"");
    switch (c.state) {
    case BURROW__HT_STATE_JS_SQ_STR:
        specials = BURROW_S("\\'");
        break;
    case BURROW__HT_STATE_JS_REGEXP:
        specials = BURROW_S("\\/[]");
        break;
    default:
        break;
    }

    Int k = 0;
    bool in_charset = false;
    for (;;) {
        Int i = htx_index_any_from(s, k, specials);
        if (i < k)
            break;
        switch (s.p[i]) {
        case '\\':
            i++;
            if (i == s.len) {
                *nread = s.len;
                return htx_ctx_err(burrow__ht_errorf(
                    a, HTML_TEMPLATE_ERR_PARTIAL_ESCAPE, NULL, 0,
                    htx_err_partial_escape(a, BURROW_S("JS string"), s)));
            }
            break;
        case '[':
            in_charset = true;
            break;
        case ']':
            in_charset = false;
            break;
        case '/':
            /* If "</script" appears in a regex literal, the '/' should not
             * close the regex literal, and it will later be escaped to
             * "\x3C/script" in escapeText. */
            if (i > 0 && i + 7 <= s.len &&
                strings_equal_fold(burrow__ht_sub(s, i - 1, i + 7),
                                   BURROW_S("</script"))) {
                i++;
            } else if (!in_charset) {
                c.state = BURROW__HT_STATE_JS;
                c.js_ctx = BURROW__HT_JS_CTX_DIV_OP;
                *nread = i + 1;
                return c;
            }
            break;
        default:
            /* end delimiter */
            if (!in_charset) {
                c.state = BURROW__HT_STATE_JS;
                c.js_ctx = BURROW__HT_JS_CTX_DIV_OP;
                *nread = i + 1;
                return c;
            }
            break;
        }
        k = i + 1;
    }

    if (in_charset) {
        /* This can be fixed by making context richer if interpolation into
         * charsets is desired. */
        *nread = s.len;
        return htx_ctx_err(burrow__ht_errorf(
            a, HTML_TEMPLATE_ERR_PARTIAL_CHARSET, NULL, 0,
            fmt_sprintf_v(a, BURROW_S("unfinished JS regexp charset: %q"), s)));
    }

    *nread = s.len;
    return c;
}

static const Str htx_block_comment_end = S_("*/");

/* tBlockCmt is the context transition function for /\*comment*\/ states. */
static burrow__HtContext htx_t_block_cmt(Alloc *a, burrow__HtContext c, Str s,
                                         Int *nread) {
    Int i = strings_index(s, htx_block_comment_end);
    if (i == -1) {
        *nread = s.len;
        return c;
    }
    switch (c.state) {
    case BURROW__HT_STATE_JS_BLOCK_CMT:
        c.state = BURROW__HT_STATE_JS;
        break;
    case BURROW__HT_STATE_CSS_BLOCK_CMT:
        c.state = BURROW__HT_STATE_CSS;
        break;
    default:
        panic_str(burrow__ht_state_string(a, c.state));
    }
    *nread = i + 2;
    return c;
}

/* tLineCmt is the context transition function for //comment states, and the
 * JS HTML-like comment state. */
static burrow__HtContext htx_t_line_cmt(Alloc *a, burrow__HtContext c, Str s,
                                        Int *nread) {
    Str line_terminators;
    burrow__HtState end_state;
    switch (c.state) {
    case BURROW__HT_STATE_JS_LINE_CMT:
    case BURROW__HT_STATE_JS_HTML_OPEN_CMT:
    case BURROW__HT_STATE_JS_HTML_CLOSE_CMT:
        line_terminators = BURROW_S("\n\r\xe2\x80\xa8\xe2\x80\xa9");
        end_state = BURROW__HT_STATE_JS;
        break;
    case BURROW__HT_STATE_CSS_LINE_CMT:
        line_terminators = BURROW_S("\n\f\r");
        end_state = BURROW__HT_STATE_CSS;
        /* Line comments are not part of any published CSS standard but are
         * supported by the 4 major browsers. This defines line comments as
         * LINECOMMENT ::= "//" [^\n\f\d]* since
         * https://www.w3.org/TR/css3-syntax/#SUBTOK-nl defines newlines:
         * nl ::= #xA | #xD #xA | #xD | #xC */
        break;
    default:
        panic_str(burrow__ht_state_string(a, c.state));
    }

    Int i = strings_index_any(s, line_terminators);
    if (i == -1) {
        *nread = s.len;
        return c;
    }
    c.state = end_state;
    /* Per section 7.4 of EcmaScript 5 :
     * https://es5.github.io/#x7.4 the line terminator is not part of the
     * comment, so we do not consume it. */
    *nread = i;
    return c;
}

static const Str htx_css_space = S_("\t\n\f\r ");

/* tCSS is the context transition function for the CSS state. */
static burrow__HtContext htx_t_css(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    (void)a;
    /* CSS quoted strings are almost never used except for:
     * (1) URLs as in background: "/foo.png"
     * (2) Multiword font-names as in font-family: "Times New Roman"
     * (3) List separators in content values as in inline-lists:
     *    <style>
     *    ul.inlineList { list-style: none; padding:0 }
     *    ul.inlineList > li { display: inline }
     *    ul.inlineList > li:before { content: ", " }
     *    ul.inlineList > li:first-child:before { content: "" }
     *    </style>
     * (4) Attribute value selectors as in a[href="http://example.com/"]
     *
     * We conservatively treat all strings as URLs, but make some allowances to
     * avoid confusion.
     *
     * In (1), our conservative assumption is justified.
     * In (2), valid font names do not contain ':', '?', or '#', so our
     * conservative assumption is fine since we will never transition past
     * urlPartPreQuery.
     * In (3), our protocol heuristic should not be tripped, and there should
     * not be non-space content after a '?' or '#', so as long as we only %-encode
     * RFC 3986 reserved characters we are ok.
     * In (4), we should URL escape for URL attributes, and for others we have
     * the attribute name available if more context is needed. */
    Int k = 0;
    for (;;) {
        Int i = htx_index_any_from(s, k, BURROW_S("(\"'/"));
        if (i < k) {
            *nread = s.len;
            return c;
        }
        switch (s.p[i]) {
        case '(': {
            /* Look for url to the left. */
            Str p = strings_trim_right(burrow__ht_sub(s, 0, i), htx_css_space);
            if (burrow__ht_ends_with_css_keyword(p, BURROW_S("url"))) {
                Int j = s.len - strings_trim_left(burrow__ht_sub(s, i + 1, s.len),
                                                  htx_css_space)
                                    .len;
                if (j != s.len && s.p[j] == '"') {
                    c.state = BURROW__HT_STATE_CSS_DQ_URL;
                    j++;
                } else if (j != s.len && s.p[j] == '\'') {
                    c.state = BURROW__HT_STATE_CSS_SQ_URL;
                    j++;
                } else {
                    c.state = BURROW__HT_STATE_CSS_URL;
                }
                *nread = j;
                return c;
            }
            break;
        }
        case '/':
            if (i + 1 < s.len) {
                switch (s.p[i + 1]) {
                case '/':
                    c.state = BURROW__HT_STATE_CSS_LINE_CMT;
                    *nread = i + 2;
                    return c;
                case '*':
                    c.state = BURROW__HT_STATE_CSS_BLOCK_CMT;
                    *nread = i + 2;
                    return c;
                default:
                    break;
                }
            }
            break;
        case '"':
            c.state = BURROW__HT_STATE_CSS_DQ_STR;
            *nread = i + 1;
            return c;
        case '\'':
            c.state = BURROW__HT_STATE_CSS_SQ_STR;
            *nread = i + 1;
            return c;
        default:
            break;
        }
        k = i + 1;
    }
}

/* tCSSStr is the context transition function for the CSS string and URL
 * states. */
static burrow__HtContext htx_t_css_str(Alloc *a, burrow__HtContext c, Str s,
                                       Int *nread) {
    Str end_and_esc;
    switch (c.state) {
    case BURROW__HT_STATE_CSS_DQ_STR:
    case BURROW__HT_STATE_CSS_DQ_URL:
        end_and_esc = BURROW_S("\\\"");
        break;
    case BURROW__HT_STATE_CSS_SQ_STR:
    case BURROW__HT_STATE_CSS_SQ_URL:
        end_and_esc = BURROW_S("\\'");
        break;
    case BURROW__HT_STATE_CSS_URL:
        /* Unquoted URLs end with a newline or close parenthesis. The below
         * includes the wc (whitespace character) and nl. */
        end_and_esc = BURROW_S("\\\t\n\f\r )");
        break;
    default:
        panic_str(burrow__ht_state_string(a, c.state));
    }

    Int k = 0;
    for (;;) {
        Int i = htx_index_any_from(s, k, end_and_esc);
        if (i < k) {
            Int n;
            c = htx_t_url(a, c, burrow__ht_decode_css(a, burrow__ht_sub(s, k, s.len)),
                          &n);
            *nread = k + n;
            return c;
        }
        if (s.p[i] == '\\') {
            i++;
            if (i == s.len) {
                *nread = s.len;
                return htx_ctx_err(burrow__ht_errorf(
                    a, HTML_TEMPLATE_ERR_PARTIAL_ESCAPE, NULL, 0,
                    htx_err_partial_escape(a, BURROW_S("CSS string"), s)));
            }
        } else {
            c.state = BURROW__HT_STATE_CSS;
            *nread = i + 1;
            return c;
        }
        Int n;
        c = htx_t_url(a, c, burrow__ht_decode_css(a, burrow__ht_sub(s, 0, i + 1)), &n);
        k = i + 1;
    }
}

/* tError is the context transition function for the error state. */
static burrow__HtContext htx_t_error(Alloc *a, burrow__HtContext c, Str s, Int *nread) {
    (void)a;
    *nread = s.len;
    return c;
}

/* tMetaContent is the context transition function for the meta content
 * attribute state. */
static burrow__HtContext htx_t_meta_content(Alloc *a, burrow__HtContext c, Str s,
                                            Int *nread) {
    (void)a;
    for (Int i = 0; i < s.len; i++) {
        if (i + 3 <= s.len - 1 &&
            strings_equal_fold(burrow__ht_sub(s, i, i + 3), BURROW_S("url"))) {
            Int j = burrow__ht_eat_white_space(s, i + 3);
            if (j < s.len && s.p[j] == '=') {
                c.state = BURROW__HT_STATE_META_CONTENT_URL;
                *nread = j + 1;
                return c;
            }
        }
    }
    *nread = s.len;
    return c;
}

/* tMetaContentURL is the context transition function for the "url=" part of
 * a meta content attribute state. */
static burrow__HtContext htx_t_meta_content_url(Alloc *a, burrow__HtContext c, Str s,
                                                Int *nread) {
    (void)a;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == ';') {
            c.state = BURROW__HT_STATE_META_CONTENT;
            *nread = i + 1;
            return c;
        }
    }
    *nread = s.len;
    return c;
}

const burrow__HtTransition burrow__ht_transition_func[BURROW__HT_STATE_COUNT] = {
    [BURROW__HT_STATE_TEXT] = htx_t_text,
    [BURROW__HT_STATE_TAG] = htx_t_tag,
    [BURROW__HT_STATE_ATTR_NAME] = htx_t_attr_name,
    [BURROW__HT_STATE_AFTER_NAME] = htx_t_after_name,
    [BURROW__HT_STATE_BEFORE_VALUE] = htx_t_before_value,
    [BURROW__HT_STATE_HTML_CMT] = htx_t_html_cmt,
    [BURROW__HT_STATE_RCDATA] = htx_t_special_tag_end,
    [BURROW__HT_STATE_ATTR] = htx_t_attr,
    [BURROW__HT_STATE_URL] = htx_t_url,
    [BURROW__HT_STATE_META_CONTENT] = htx_t_meta_content,
    [BURROW__HT_STATE_META_CONTENT_URL] = htx_t_meta_content_url,
    [BURROW__HT_STATE_SRCSET] = htx_t_url,
    [BURROW__HT_STATE_JS] = htx_t_js,
    [BURROW__HT_STATE_JS_DQ_STR] = htx_t_js_delimited,
    [BURROW__HT_STATE_JS_SQ_STR] = htx_t_js_delimited,
    [BURROW__HT_STATE_JS_REGEXP] = htx_t_js_delimited,
    [BURROW__HT_STATE_JS_TMPL_LIT] = htx_t_js_tmpl,
    [BURROW__HT_STATE_JS_BLOCK_CMT] = htx_t_block_cmt,
    [BURROW__HT_STATE_JS_LINE_CMT] = htx_t_line_cmt,
    [BURROW__HT_STATE_JS_HTML_OPEN_CMT] = htx_t_line_cmt,
    [BURROW__HT_STATE_JS_HTML_CLOSE_CMT] = htx_t_line_cmt,
    [BURROW__HT_STATE_CSS] = htx_t_css,
    [BURROW__HT_STATE_CSS_DQ_STR] = htx_t_css_str,
    [BURROW__HT_STATE_CSS_SQ_STR] = htx_t_css_str,
    [BURROW__HT_STATE_CSS_DQ_URL] = htx_t_css_str,
    [BURROW__HT_STATE_CSS_SQ_URL] = htx_t_css_str,
    [BURROW__HT_STATE_CSS_URL] = htx_t_css_str,
    [BURROW__HT_STATE_CSS_BLOCK_CMT] = htx_t_block_cmt,
    [BURROW__HT_STATE_CSS_LINE_CMT] = htx_t_line_cmt,
    [BURROW__HT_STATE_ERROR] = htx_t_error,
};
