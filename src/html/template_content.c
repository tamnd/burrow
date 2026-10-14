/* html/template: the content types, what kind of content a value and an
 * attribute hold, the Error type, and the escapers that are text/template's.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/reflect.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/text/template.h"
#include "burrow/type.h"
#include "template_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ the types */

/* A string type of the package's own, with no methods. */
#define HTC_STR_TYPE(T, NAME, HASH)                                                    \
    const Type burrow_type_##T = {                                                     \
        BURROW_S_INIT(NAME),                                                           \
        BURROW_S_INIT("html/template"),                                                \
        KIND_STRING,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        HASH,                                                                          \
        NULL,                                                                          \
    }

HTC_STR_TYPE(HtmlTemplateCSS, "CSS", 0x68746373U);           /* "htcs" */
HTC_STR_TYPE(HtmlTemplateHTML, "HTML", 0x6874686dU);         /* "hthm" */
HTC_STR_TYPE(HtmlTemplateHTMLAttr, "HTMLAttr", 0x68746861U); /* "htha" */
HTC_STR_TYPE(HtmlTemplateJS, "JS", 0x68746a73U);             /* "htjs" */
HTC_STR_TYPE(HtmlTemplateJSStr, "JSStr", 0x68746a71U);       /* "htjq" */
HTC_STR_TYPE(HtmlTemplateURL, "URL", 0x68747572U);           /* "htur" */
HTC_STR_TYPE(HtmlTemplateSrcset, "Srcset", 0x68747373U);     /* "htss" */

/* --------------------------------------------------------------- helpers */

BURROW_NORETURN void burrow__ht_oom(void) {
    panic(BURROW_ANY(TYPE_ERROR, (uintptr_t)&burrow_err_out_of_memory));
}

void burrow__ht_write(StringsBuilder *b, Str s) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_string(b, s, &err);
    if (BURROW_FAILED(err))
        burrow__ht_oom();
}

/* --------------------------------------------------------------- content */

/* indirect: a with every pointer followed until one is nil or the value is not
 * a pointer. */
static Any htc_indirect(Any a) {
    if (a.t == NULL || a.t->kind != KIND_POINTER)
        return a;
    ReflectValue v = reflect_value_of(a);
    while (reflect_value_kind(v) == REFLECT_POINTER && !reflect_value_is_nil(v))
        v = reflect_value_elem(v);
    return reflect_value_interface(v);
}

/* Whether fmt prints a t with a method, as Go's Stringer and error are. A
 * pointer has the methods of what it points at. */
static bool htc_has_print_method(ReflectType t) {
    ReflectMethod m;
    return reflect_type_implements(t, TYPE_ERROR) ||
           reflect_type_method_by_name(t, BURROW_S("String"), &m);
}

/* indirectToStringerOrError: a with pointers followed until one is nil or has a
 * String or Error method, so that fmt prints a value rather than its address. */
static Any htc_indirect_to_stringer_or_error(Any a) {
    if (a.t == NULL)
        return a;
    ReflectValue v = reflect_value_of(a);
    while (!htc_has_print_method(reflect_value_type(v)) &&
           reflect_value_kind(v) == REFLECT_POINTER && !reflect_value_is_nil(v))
        v = reflect_value_elem(v);
    return reflect_value_interface(v);
}

static const struct {
    const Type *type;
    burrow__HtContentType content;
} htc_content_types[] = {
    {&burrow_type_HtmlTemplateCSS, BURROW__HT_CONTENT_CSS},
    {&burrow_type_HtmlTemplateHTML, BURROW__HT_CONTENT_HTML},
    {&burrow_type_HtmlTemplateHTMLAttr, BURROW__HT_CONTENT_HTML_ATTR},
    {&burrow_type_HtmlTemplateJS, BURROW__HT_CONTENT_JS},
    {&burrow_type_HtmlTemplateJSStr, BURROW__HT_CONTENT_JS_STR},
    {&burrow_type_HtmlTemplateURL, BURROW__HT_CONTENT_URL},
    {&burrow_type_HtmlTemplateSrcset, BURROW__HT_CONTENT_SRCSET},
};

Str burrow__ht_stringify(Alloc *a, Slice args, burrow__HtContentType *t) {
    *t = BURROW__HT_CONTENT_PLAIN;
    if (args.len == 1) {
        Any s = htc_indirect(BURROW_AT(Any, args, 0));
        if (s.t == TYPE_STRING)
            return *(const Str *)s.data;
        for (size_t i = 0; i < sizeof htc_content_types / sizeof htc_content_types[0];
             i++)
            if (s.t == htc_content_types[i].type) {
                *t = htc_content_types[i].content;
                return *(const Str *)s.data;
            }
    }
    /* Go compacts the caller's arguments in place. A copy leaves them be. */
    Slice c = slice_make(a, TYPE_ANY, 0, args.len);
    if (args.len > 0 && c.p == NULL)
        burrow__ht_oom();
    for (Int i = 0; i < args.len; i++) {
        Any arg = BURROW_AT(Any, args, i);
        if (arg.t == NULL)
            continue;
        ((Any *)c.p)[c.len++] = htc_indirect_to_stringer_or_error(arg);
    }
    return fmt_sprint(a, c);
}

/* evalArgs: what text/template's print makes of args, or the one string when
 * there is one. */
Str burrow__ht_eval_args(Alloc *a, Slice args) {
    if (args.len == 1 && BURROW_AT(Any, args, 0).t == TYPE_STRING)
        return *(const Str *)BURROW_AT(Any, args, 0).data;
    /* Go changes the caller's arguments in place. A copy leaves them be. */
    Slice c = slice_make(a, TYPE_ANY, args.len, args.len);
    if (args.len > 0 && c.p == NULL)
        burrow__ht_oom();
    for (Int i = 0; i < args.len; i++)
        BURROW_AT(Any, c, i) =
            htc_indirect_to_stringer_or_error(BURROW_AT(Any, args, i));
    return fmt_sprint(a, c);
}

/* attrTypeMap, in byte order so that it can be searched. */
static const struct {
    Str name;
    burrow__HtContentType type;
} htc_attr_types[] = {
    {BURROW_S_INIT("accept"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("accept-charset"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("action"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("alt"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("archive"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("async"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("autocomplete"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("autofocus"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("autoplay"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("background"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("border"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("challenge"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("charset"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("checked"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("cite"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("class"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("classid"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("codebase"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("cols"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("colspan"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("content"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("contenteditable"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("contextmenu"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("controls"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("coords"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("crossorigin"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("data"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("datetime"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("default"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("defer"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("dir"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("dirname"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("disabled"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("draggable"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("dropzone"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("enctype"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("for"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("form"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("formaction"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("formenctype"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("formmethod"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("formnovalidate"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("formtarget"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("headers"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("height"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("hidden"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("high"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("href"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("hreflang"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("http-equiv"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("icon"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("id"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("ismap"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("keytype"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("kind"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("label"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("lang"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("language"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("list"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("longdesc"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("loop"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("low"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("manifest"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("max"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("maxlength"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("media"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("mediagroup"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("method"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("min"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("multiple"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("name"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("novalidate"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("open"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("optimum"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("pattern"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("placeholder"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("poster"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("preload"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("profile"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("pubdate"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("radiogroup"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("readonly"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("rel"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("required"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("reversed"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("rows"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("rowspan"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("sandbox"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("scope"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("scoped"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("seamless"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("selected"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("shape"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("size"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("sizes"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("span"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("spellcheck"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("src"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("srcdoc"), BURROW__HT_CONTENT_HTML},
    {BURROW_S_INIT("srclang"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("srcset"), BURROW__HT_CONTENT_SRCSET},
    {BURROW_S_INIT("start"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("step"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("style"), BURROW__HT_CONTENT_CSS},
    {BURROW_S_INIT("tabindex"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("target"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("title"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("type"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("usemap"), BURROW__HT_CONTENT_URL},
    {BURROW_S_INIT("value"), BURROW__HT_CONTENT_UNSAFE},
    {BURROW_S_INIT("width"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("wrap"), BURROW__HT_CONTENT_PLAIN},
    {BURROW_S_INIT("xmlns"), BURROW__HT_CONTENT_URL},
};

static int htc_str_compare(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    int c = n > 0 ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c != 0)
        return c;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

static bool htc_attr_lookup(Str name, burrow__HtContentType *t) {
    size_t lo = 0, hi = sizeof htc_attr_types / sizeof htc_attr_types[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = htc_str_compare(htc_attr_types[mid].name, name);
        if (c == 0) {
            *t = htc_attr_types[mid].type;
            return true;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

burrow__HtContentType burrow__ht_attr_type(Str name) {
    Str short_name;
    bool ok = false;
    if (strings_has_prefix(name, BURROW_S("data-"))) {
        /* Strip data- so that custom attribute heuristics below are widely
         * applied. Treat data-action as URL. */
        name = burrow__ht_sub(name, 5, name.len);
    } else {
        Str prefix = strings_cut(name, BURROW_S(":"), &short_name, &ok);
        if (ok) {
            if (str_eq(prefix, BURROW_S("xmlns")))
                return BURROW__HT_CONTENT_URL;
            /* Treat svg:href and xlink:href as href below. */
            name = short_name;
        }
    }
    burrow__HtContentType t;
    if (htc_attr_lookup(name, &t))
        return t;
    /* Treat partial event handler names as script. */
    if (strings_has_prefix(name, BURROW_S("on")))
        return BURROW__HT_CONTENT_JS;

    /* Heuristics to prevent "javascript:..." injection in custom data
     * attributes and custom attributes like g:tweetUrl.
     * https://www.w3.org/TR/html5/dom.html#embedding-custom-non-visible-data-with-the-data-*-attributes
     * "Custom data attributes are intended to store custom data private to the
     * page or application, for which there are no more appropriate attributes
     * or elements." Developers seem to store URL content in data URLs that
     * start or end with "URI" or "URL". */
    if (strings_contains(name, BURROW_S("src")) ||
        strings_contains(name, BURROW_S("uri")) ||
        strings_contains(name, BURROW_S("url")))
        return BURROW__HT_CONTENT_URL;
    return BURROW__HT_CONTENT_PLAIN;
}

/* ----------------------------------------------------------------- Error */

/* An Error's box: a copy of the struct, with the message made from it, which
 * the vtable hands out. errors_as gives back the struct at the front. */
typedef struct HtcErrorBox {
    HtmlTemplateError e;
    Str message;
} HtcErrorBox;

static const Type htc_error_desc = {
    BURROW_S_INIT("Error"),
    BURROW_S_INIT("html/template"),
    KIND_STRUCT,
    (uint32_t)sizeof(HtmlTemplateError),
    (uint16_t)_Alignof(HtmlTemplateError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68746572U, /* "hter" */
    NULL,
};

const Type *const TYPE_HTML_TEMPLATE_ERROR = &htc_error_desc;

static Str htc_error_message(const void *self) {
    return ((const HtcErrorBox *)self)->message;
}

static Error htc_error_clone(const void *self, Alloc *a) {
    return html_template_error_as_error(a, &((const HtcErrorBox *)self)->e);
}

static const ErrorVT htc_error_vt = {
    &htc_error_desc, htc_error_message, NULL, NULL, NULL, NULL, htc_error_clone,
};

Str html_template_error_error(const HtmlTemplateError *e, Alloc *a) {
    if (e->node != NULL) {
        Str context;
        Str loc = parse_tree_error_context(NULL, a, e->node, &context);
        return fmt_sprintf_v(a, "html/template:%s: %s", loc, e->description);
    }
    if (e->line != 0)
        return fmt_sprintf_v(a, "html/template:%s:%d: %s", e->name, e->line,
                             e->description);
    if (e->name.len != 0)
        return fmt_sprintf_v(a, "html/template:%s: %s", e->name, e->description);
    return fmt_sprintf_v(a, "html/template: %s", e->description);
}

static Str htc_copy_str(Alloc *a, Str s, bool *ok) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL) {
        *ok = false;
        return BURROW_STR_EMPTY;
    }
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

Error html_template_error_as_error(Alloc *a, const HtmlTemplateError *e) {
    HtcErrorBox *b = BURROW_NEW(a, HtcErrorBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    bool ok = true;
    b->e = *e;
    b->e.name = htc_copy_str(a, e->name, &ok);
    b->e.description = htc_copy_str(a, e->description, &ok);
    b->message = html_template_error_error(e, a);
    if (!ok || (b->message.len == 0))
        return burrow_err_out_of_memory;
    return (Error){&htc_error_vt, b};
}

HtmlTemplateError *burrow__ht_errorf(Alloc *a, HtmlTemplateErrorCode k, ParseNode node,
                                     Int line, Str description) {
    HtmlTemplateError *e = BURROW_NEW(a, HtmlTemplateError);
    if (e == NULL)
        burrow__ht_oom();
    *e = (HtmlTemplateError){k, node, BURROW_STR_EMPTY, line, description};
    return e;
}

/* ------------------------------------------------------ text/template's */

void html_template_html_escape(IoWriter w, Slice b) {
    text_template_html_escape(w, b);
}

Str html_template_html_escape_string(Alloc *a, Str s) {
    return text_template_html_escape_string(a, s);
}

Str html_template_html_escaper(Alloc *a, Slice args) {
    return text_template_html_escaper(a, args);
}

void html_template_js_escape(IoWriter w, Slice b) {
    text_template_js_escape(w, b);
}

Str html_template_js_escape_string(Alloc *a, Str s) {
    return text_template_js_escape_string(a, s);
}

Str html_template_js_escaper(Alloc *a, Slice args) {
    return text_template_js_escaper(a, args);
}

Str html_template_url_query_escaper(Alloc *a, Slice args) {
    return text_template_url_query_escaper(a, args);
}

bool html_template_is_true(Any val, bool *ok) {
    return text_template_is_true(val, ok);
}
