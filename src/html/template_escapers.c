/* html/template: the escapers and filters the template puts at the end of a
 * pipeline, for HTML, JS, CSS and URLs.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/core.h"
#include "burrow/encoding/json.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/reflect.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/utf8.h"
#include "template_internal.h"

#include <stdint.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const Str hte_filter_failsafe = S_(BURROW__HT_FILTER_FAILSAFE);

static void hte_write_byte(StringsBuilder *b, Byte c) {
    if (BURROW_FAILED(strings_builder_write_byte(b, c)))
        burrow__ht_oom();
}

static void hte_grow(StringsBuilder *b, Int n) {
    if (!strings_builder_grow(b, n))
        burrow__ht_oom();
}

/* A replacement table: the text for each rune below n, empty for a rune that
 * stays as it is. */
typedef struct {
    const Str *repl;
    Int n;
} HteTable;

#define HTE_TABLE(t) ((HteTable){(t), NELEM(t)})

/* ------------------------------------------------------------------- HTML */

static const Str hte_html_replacement_table[] = {
    [0] = S_("\xef\xbf\xbd"), ['"'] = S_("&#34;"), ['&'] = S_("&amp;"),
    ['\''] = S_("&#39;"),     ['+'] = S_("&#43;"), ['<'] = S_("&lt;"),
    ['>'] = S_("&gt;"),
};

/* htmlNormReplacementTable is like htmlReplacementTable but without '&' to
 * avoid over-encoding existing entities. */
static const Str hte_html_norm_replacement_table[] = {
    [0] = S_("\xef\xbf\xbd"), ['"'] = S_("&#34;"), ['\''] = S_("&#39;"),
    ['+'] = S_("&#43;"),      ['<'] = S_("&lt;"),  ['>'] = S_("&gt;"),
};

/* htmlNospaceReplacementTable contains the runes that need to be escaped
 * inside an unquoted attribute value. The set of runes escaped is the union of
 * the HTML specials and those determined by running the JS below in browsers. */
static const Str hte_html_nospace_replacement_table[] = {
    [0] = S_("&#xfffd;"), ['\t'] = S_("&#9;"),  ['\n'] = S_("&#10;"),
    ['\v'] = S_("&#11;"), ['\f'] = S_("&#12;"), ['\r'] = S_("&#13;"),
    [' '] = S_("&#32;"),  ['"'] = S_("&#34;"),  ['&'] = S_("&amp;"),
    ['\''] = S_("&#39;"), ['+'] = S_("&#43;"),  ['<'] = S_("&lt;"),
    ['='] = S_("&#61;"),  ['>'] = S_("&gt;"),   ['`'] = S_("&#96;"),
};

/* htmlNospaceNormReplacementTable is like htmlNospaceReplacementTable but
 * without '&' to avoid over-encoding existing entities. */
static const Str hte_html_nospace_norm_replacement_table[] = {
    [0] = S_("&#xfffd;"), ['\t'] = S_("&#9;"),  ['\n'] = S_("&#10;"),
    ['\v'] = S_("&#11;"), ['\f'] = S_("&#12;"), ['\r'] = S_("&#13;"),
    [' '] = S_("&#32;"),  ['"'] = S_("&#34;"),  ['\''] = S_("&#39;"),
    ['+'] = S_("&#43;"),  ['<'] = S_("&lt;"),   ['='] = S_("&#61;"),
    ['>'] = S_("&gt;"),   ['`'] = S_("&#96;"),
};

/* htmlReplacer returns s with runes replaced according to replacementTable
 * and when badRunes is true, certain bad runes are allowed through unescaped. */
static Str hte_html_replacer(Alloc *a, Str s, HteTable table, bool bad_runes) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int written = 0;
    Int w = 0;
    for (Int i = 0; i < s.len; i += w) {
        /* Cannot use 'for range s' because we need to preserve the width of
         * the runes in the input. If we see a decoding error, the input width
         * will not be utf8.Runelen(r) and we will overrun the buffer. */
        Rune r = utf8_decode_rune_in_string(burrow__ht_sub(s, i, s.len), &w);
        if (r < table.n) {
            Str repl = table.repl[r];
            if (repl.len != 0) {
                if (written == 0)
                    hte_grow(&b, s.len);
                burrow__ht_write(&b, burrow__ht_sub(s, written, i));
                burrow__ht_write(&b, repl);
                written = i + w;
            }
        } else if (bad_runes) {
            /* No-op. IE does not allow these ranges in unquoted attrs. */
        } else if ((0xfdd0 <= r && r <= 0xfdef) || (0xfff0 <= r && r <= 0xffff)) {
            if (written == 0)
                hte_grow(&b, s.len);
            burrow__ht_write(&b, burrow__ht_sub(s, written, i));
            burrow__ht_write(&b, BURROW_S("&#x"));
            burrow__ht_write(&b, strconv_format_int(a, r, 16));
            burrow__ht_write(&b, BURROW_S(";"));
            written = i + w;
        }
    }
    if (written == 0)
        return s;
    burrow__ht_write(&b, burrow__ht_sub(s, written, s.len));
    return strings_builder_string(&b);
}

/* htmlNospaceEscaper escapes for inclusion in unquoted attribute values. */
Str burrow__ht_html_nospace_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (s.len == 0)
        return hte_filter_failsafe;
    if (t == BURROW__HT_CONTENT_HTML)
        return hte_html_replacer(a, burrow__ht_strip_tags(a, s),
                                 HTE_TABLE(hte_html_nospace_norm_replacement_table),
                                 false);
    return hte_html_replacer(a, s, HTE_TABLE(hte_html_nospace_replacement_table),
                             false);
}

/* attrEscaper escapes for inclusion in quoted attribute values. */
Str burrow__ht_attr_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_HTML)
        return hte_html_replacer(a, burrow__ht_strip_tags(a, s),
                                 HTE_TABLE(hte_html_norm_replacement_table), true);
    return hte_html_replacer(a, s, HTE_TABLE(hte_html_replacement_table), true);
}

/* rcdataEscaper escapes for inclusion in an RCDATA element body. */
Str burrow__ht_rcdata_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_HTML)
        return hte_html_replacer(a, s, HTE_TABLE(hte_html_norm_replacement_table),
                                 true);
    return hte_html_replacer(a, s, HTE_TABLE(hte_html_replacement_table), true);
}

/* htmlEscaper escapes for inclusion in HTML text. */
Str burrow__ht_html_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_HTML)
        return s;
    return hte_html_replacer(a, s, HTE_TABLE(hte_html_replacement_table), true);
}

/* stripTags takes a snippet of HTML and returns only the text content. For
 * example, `<b>&iexcl;Hi!</b> <script>...</script>` -> `&iexcl;Hi! `. */
Str burrow__ht_strip_tags(Alloc *a, Str html) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Str s = html;
    burrow__HtContext c = burrow__ht_context();
    Int i = 0;
    bool all_text = true;
    while (i != s.len) {
        if (c.delim == BURROW__HT_DELIM_NONE) {
            burrow__HtState st = c.state;
            /* Use RCDATA instead of parsing into JS or CSS styles. */
            if (c.element != BURROW__HT_ELEMENT_NONE && !burrow__ht_is_in_tag(st))
                st = BURROW__HT_STATE_RCDATA;
            Int nread;
            burrow__HtContext d = burrow__ht_transition_func[st](
                a, c, burrow__ht_sub(s, i, s.len), &nread);
            Int i1 = i + nread;
            if (c.state == BURROW__HT_STATE_TEXT ||
                c.state == BURROW__HT_STATE_RCDATA) {
                /* Emit text up to the start of the tag or comment. */
                Int j = i1;
                if (d.state != c.state) {
                    for (Int j1 = j - 1; j1 >= i; j1--) {
                        if (s.p[j1] == '<') {
                            j = j1;
                            break;
                        }
                    }
                }
                burrow__ht_write(&b, burrow__ht_sub(s, i, j));
            } else {
                all_text = false;
            }
            c = d;
            i = i1;
            continue;
        }
        Int k = strings_index_any(burrow__ht_sub(s, i, s.len),
                                  burrow__ht_delim_ends[c.delim]);
        if (k < 0)
            break;
        Int i1 = i + k;
        if (c.delim != BURROW__HT_DELIM_SPACE_OR_TAG_END) {
            /* Consume any quote. */
            i1++;
        }
        burrow__HtElement e = c.element;
        c = burrow__ht_context();
        c.state = BURROW__HT_STATE_TAG;
        c.element = e;
        i = i1;
    }
    if (all_text)
        return html;
    if (c.state == BURROW__HT_STATE_TEXT || c.state == BURROW__HT_STATE_RCDATA)
        burrow__ht_write(&b, burrow__ht_sub(s, i, s.len));
    return strings_builder_string(&b);
}

/* htmlNameFilter accepts valid parts of an HTML attribute or tag name or a
 * known-safe HTML attribute. */
Str burrow__ht_html_name_filter(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_HTML_ATTR)
        return s;
    if (s.len == 0) {
        /* Avoid violation of structure preservation.
         * <input checked {{.K}}={{.V}}>.
         * Without this, if .K is empty then .V is the value of checked, but
         * otherwise .V is the value of the attribute named .K. */
        return hte_filter_failsafe;
    }
    s = strings_to_lower(a, s);
    if (burrow__ht_attr_type(s) != BURROW__HT_CONTENT_PLAIN) {
        /* TODO: Split attr and element name part filters so we can recognize
         * known attributes. */
        return hte_filter_failsafe;
    }
    for (Int i = 0; i < s.len;) {
        Int w;
        Rune r = utf8_decode_rune_in_string(burrow__ht_sub(s, i, s.len), &w);
        if (!('0' <= r && r <= '9') && !('a' <= r && r <= 'z'))
            return hte_filter_failsafe;
        i += w;
    }
    return s;
}

/* commentEscaper returns the empty string regardless of input. Comment content
 * does not correspond to any parsed structure or human-readable content, so
 * the simplest and most secure policy is to drop content interpolated into
 * comments. This approach is equally valid whether or not static comment
 * content is removed from the template. */
Str burrow__ht_comment_escaper(Alloc *a, Slice args) {
    (void)a;
    (void)args;
    return BURROW_STR_EMPTY;
}

/* --------------------------------------------------------------------- JS */

/* jsWhitespace contains all of the JS whitespace characters, as defined by the
 * \s character class.
 * See https://developer.mozilla.org/en-US/docs/Web/JavaScript/Guide/Regular_expressions/Character_classes. */
static const Str hte_js_whitespace =
    S_("\f\n\r\t\v\x20\xc2\xa0\xe1\x9a\x80\xe2\x80\x80\xe2\x80\x81\xe2\x80\x82"
       "\xe2\x80\x83\xe2\x80\x84\xe2\x80\x85\xe2\x80\x86\xe2\x80\x87\xe2\x80\x88"
       "\xe2\x80\x89\xe2\x80\x8a\xe2\x80\xa8\xe2\x80\xa9\xe2\x80\xaf\xe2\x81\x9f"
       "\xe3\x80\x80\xef\xbb\xbf");

/* regexpPrecederKeywords is a set of reserved JS keywords that can precede a
 * regular expression in JS source. */
static const Str hte_regexp_preceder_keywords[] = {
    S_("break"), S_("case"),    S_("continue"), S_("delete"),     S_("do"),
    S_("else"),  S_("finally"), S_("in"),       S_("instanceof"), S_("return"),
    S_("throw"), S_("try"),     S_("typeof"),   S_("void"),
};

/* nextJSCtx returns the context that determines whether a slash after the
 * given run of tokens starts a regular expression instead of a division
 * operator: / or /=.
 *
 * This assumes that the token run does not include any string tokens, comment
 * tokens, regular expression literal tokens, or division operators.
 *
 * This fails on some valid but nonsensical JavaScript programs like
 * "x = ++/foo/i" which is quite different than "x++/foo/i", but is not known
 * to fail on any known useful programs. It is based on the draft JavaScript
 * 2.0 lexical grammar and requires one token of lookbehind:
 * https://www.mozilla.org/js/language/js20-2000-07/rationale/syntax.html */
burrow__HtJsCtx burrow__ht_next_js_ctx(Str s, burrow__HtJsCtx preceding) {
    /* Trim all JS whitespace characters. */
    s = strings_trim_right(s, hte_js_whitespace);
    if (s.len == 0)
        return preceding;

    /* All cases below are in the single-byte UTF-8 group. */
    Byte c = s.p[s.len - 1];
    Int n = s.len;
    switch (c) {
    case '+':
    case '-': {
        /* ++ and -- are not regexp preceders, but + and - are whether they
         * are used as infix or prefix operators. */
        Int start = n - 1;
        /* Count the number of adjacent dashes or pluses. */
        while (start > 0 && s.p[start - 1] == c)
            start--;
        if (((n - start) & 1) == 1) {
            /* Reached for trailing minus signs since "---" is the same as
             * "-- -". */
            return BURROW__HT_JS_CTX_REGEXP;
        }
        return BURROW__HT_JS_CTX_DIV_OP;
    }
    case '.':
        /* Handle "42." */
        if (n != 1 && '0' <= s.p[n - 2] && s.p[n - 2] <= '9')
            return BURROW__HT_JS_CTX_DIV_OP;
        return BURROW__HT_JS_CTX_REGEXP;
    /* Suffixes for all punctuators from section 7.7 of the language spec that
     * only end binary operators not handled above. */
    case ',':
    case '<':
    case '>':
    case '=':
    case '*':
    case '%':
    case '&':
    case '|':
    case '^':
    case '?':
    /* Suffixes for all punctuators from section 7.7 of the language spec that
     * are prefix operators not handled above. */
    case '!':
    case '~':
    /* Matches all the punctuators from section 7.7 of the language spec that
     * are open brackets not handled above. */
    case '(':
    case '[':
    /* Matches all the punctuators from section 7.7 of the language spec that
     * precede expression starts. */
    case ':':
    case ';':
    case '{':
    /* CAVEAT: the close punctuators ('}', ']', ')') precede div ops and are
     * handled in the default except for '}' which can precede a division op as
     * in
     *    ({ valueOf: function () { return 42 } } / 2
     * which is valid, but, in practice, developers don't divide object
     * literals, so our heuristic works well for code like
     *    function () { ... }  /foo/.test(x) && sideEffect();
     * The ')' punctuator can precede a regular expression as in
     *     if (b) /foo/.test(x) && ...
     * but this is much less likely than
     *     (a + b) / c */
    case '}':
        return BURROW__HT_JS_CTX_REGEXP;
    default: {
        /* Look for an IdentifierName and see if it is a keyword that can
         * precede a regular expression. */
        Int j = n;
        while (j > 0 && burrow__ht_is_js_ident_part(s.p[j - 1]))
            j--;
        Str word = burrow__ht_sub(s, j, s.len);
        for (Int k = 0; k < NELEM(hte_regexp_preceder_keywords); k++)
            if (str_eq(word, hte_regexp_preceder_keywords[k]))
                return BURROW__HT_JS_CTX_REGEXP;
        break;
    }
    }
    /* Otherwise is a punctuator not listed above, or a string which precedes a
     * div op, or an identifier which precedes a div op. */
    return BURROW__HT_JS_CTX_DIV_OP;
}

/* The type a value's methods are on and what to call them on, as for fmt: a
 * pointer's are those of what it points at. False when there are none. */
static bool hte_method_holder(Any v, const Type **t, void **recv) {
    if (v.t == NULL)
        return false;
    if (v.t == TYPE_ERROR) {
        const Error *e = v.data;
        if (e->vt == NULL || e->vt->self_type == NULL)
            return false;
        *t = e->vt->self_type;
        *recv = (void *)(uintptr_t)e->data;
        return true;
    }
    if (v.t->nmethod > 0) {
        *t = v.t;
        *recv = (void *)(uintptr_t)v.data;
        return true;
    }
    if (v.t->kind == KIND_POINTER && v.t->elem != NULL && v.t->elem->nmethod > 0) {
        *t = v.t->elem;
        *recv = *(void *const *)v.data;
        return true;
    }
    return false;
}

static bool hte_is_json_marshaler(ReflectType t) {
    ReflectMethod m;
    return reflect_type_method_by_name(t, BURROW_S("MarshalJSON"), &m);
}

/* indirectToJSONMarshaler returns the value, after dereferencing as many
 * times as necessary to reach the base type (or nil) or an implementation of
 * json.Marshal. */
static Any hte_indirect_to_json_marshaler(Any a) {
    /* text/template now supports passing untyped nil as a func call argument,
     * so we must support it. Otherwise we'd panic below, as one cannot call
     * the Type or Interface methods on an invalid reflect.Value. See
     * golang.org/issue/18716. */
    if (a.t == NULL)
        return a;
    ReflectValue v = reflect_value_of(a);
    while (!hte_is_json_marshaler(reflect_value_type(v)) &&
           reflect_value_kind(v) == REFLECT_POINTER && !reflect_value_is_nil(v))
        v = reflect_value_elem(v);
    return reflect_value_interface(v);
}

/* The String method of a, when it has one with Go's signature, called. */
static bool hte_call_string(Any a, Str *out) {
    const Type *t;
    void *recv;
    if (!hte_method_holder(a, &t, &recv))
        return false;
    const Method *m = type_method_by_name(t, BURROW_S("String"));
    if (m == NULL || m->ftype == NULL || type_num_in(m->ftype) != 0 ||
        type_num_out(m->ftype) != 1 || type_out(m->ftype, 0) != TYPE_STRING)
        return false;
    if (recv == NULL) {
        /* What Go's wrapper for a value method says when it is called through
         * a nil pointer. */
        Str pkg = t->pkg_path;
        Int slash = strings_last_index_byte(pkg, '/');
        if (slash >= 0)
            pkg = burrow__ht_sub(pkg, slash + 1, pkg.len);
        panic_str(fmt_sprintf_v(heap_allocator(),
                                "value method %s.%s.String called using nil *%s "
                                "pointer",
                                pkg, t->name, t->name));
    }
    void *rets[1] = {out};
    method_call(m, recv, NULL, rets);
    return true;
}

/* scriptTagRe, (?i)<(/?)script, matched by hand: the length of the match at
 * s[i], or 0 for none. EqualFold folds the way (?i) does, so the s of script
 * matches U+017F too. */
static Int hte_script_tag_at(Str s, Int i) {
    if (s.p[i] != '<')
        return 0;
    Int j = i + 1;
    if (j < s.len && s.p[j] == '/')
        j++;
    Int k = j;
    for (Int n = 0; n < 6 && k < s.len; n++) {
        Int w;
        utf8_decode_rune_in_string(burrow__ht_sub(s, k, s.len), &w);
        k += w;
    }
    if (!strings_equal_fold(burrow__ht_sub(s, j, k), BURROW_S("script")))
        return 0;
    return k - i;
}

static Str hte_js_val_error(Alloc *a, Str err_str) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int written = 0;
    for (Int i = 0; i < err_str.len; i++) {
        Int n = hte_script_tag_at(err_str, i);
        if (n == 0)
            continue;
        burrow__ht_write(&b, burrow__ht_sub(err_str, written, i));
        burrow__ht_write(&b, BURROW_S("\\x3C"));
        if (err_str.p[i + 1] == '/')
            burrow__ht_write(&b, BURROW_S("/"));
        burrow__ht_write(&b, BURROW_S("script"));
        written = i + n;
        i += n - 1;
    }
    burrow__ht_write(&b, burrow__ht_sub(err_str, written, err_str.len));
    err_str = strings_builder_string(&b);
    err_str = strings_replace_all(a, err_str, BURROW_S("*/"), BURROW_S("* /"));
    err_str = strings_replace_all(a, err_str, BURROW_S("<!--"), BURROW_S("\\x3C!--"));
    return fmt_sprintf_v(a, " /* %s */null ", err_str);
}

/* jsValEscaper escapes its inputs to a JS Expression (section 11.14) that has
 * neither side-effects nor free variables outside (NaN, Infinity). */
Str burrow__ht_js_val_escaper(Alloc *a, Slice args) {
    Any v;
    Str str;
    if (args.len == 1) {
        v = hte_indirect_to_json_marshaler(BURROW_AT(Any, args, 0));
        if (v.t == TYPE_HTML_TEMPLATE_JS)
            return *(const Str *)v.data;
        if (v.t == TYPE_HTML_TEMPLATE_JS_STR) {
            /* TODO: normalize quotes. */
            StringsBuilder b = STRINGS_BUILDER(a);
            burrow__ht_write(&b, BURROW_S("\""));
            burrow__ht_write(&b, *(const Str *)v.data);
            burrow__ht_write(&b, BURROW_S("\""));
            return strings_builder_string(&b);
        }
        if (v.t != NULL && hte_is_json_marshaler(v.t)) {
            /* Do not treat as a Stringer. */
        } else if (hte_call_string(v, &str)) {
            v = BURROW_ANY(TYPE_STRING, &str);
        }
    } else {
        Slice c = slice_make(a, TYPE_ANY, args.len, args.len);
        if (c.p == NULL && args.len > 0)
            burrow__ht_oom();
        for (Int i = 0; i < args.len; i++)
            BURROW_AT(Any, c, i) =
                hte_indirect_to_json_marshaler(BURROW_AT(Any, args, i));
        str = fmt_sprint(a, c);
        v = BURROW_ANY(TYPE_STRING, &str);
    }
    /* TODO: detect cycles before calling Marshal which loops infinitely on
     * cyclic data. This may be an unacceptable DoS risk. */
    Error err = BURROW_NO_ERROR;
    Slice out = json_marshal(a, v, &err);
    if (BURROW_FAILED(err)) {
        /* While the standard JSON marshaler does not include user controlled
         * information in the error message, if a type has a MarshalJSON
         * method, the content of the error message is not guaranteed. Since we
         * insert the error into the template, as part of a comment, we attempt
         * to prevent the error from either terminating the comment, or the
         * script block itself.
         *
         * In particular we:
         *   * replace the comment end token, a star then a slash, with "* /",
         *     which does not terminate the comment
         *   * replace "<script" and "</script" with "\x3Cscript" and
         *     "\x3C/script" (case insensitively), and "<!--" with "\x3C!--",
         *     which prevents confusing script block termination semantics
         *
         * We also put a space before the comment so that if it is flush
         * against a division operator it is not turned into a line comment. */
        return hte_js_val_error(a, error_text(err));
    }

    /* TODO: maybe post-process output to prevent it from containing "<!--",
     * "-->", "<![CDATA[", "]]>", or "</script" in case custom marshalers
     * produce output containing those.
     * Note: Do not use \x escaping to save bytes because it is not JSON
     * compatible and this escaper supports ld+json content-type. */
    Str b = str_from_bytes(out.p, out.len);
    if (b.len == 0) {
        /* In `x=y/{{.}}*z`, a json.Marshaler that produces "" should not
         * cause output with a slash and a star together, which opens a
         * comment. */
        return BURROW_S(" null ");
    }
    Int size;
    Rune first = utf8_decode_rune_in_string(b, &size);
    Rune last = utf8_decode_last_rune_in_string(b, &size);
    StringsBuilder buf = STRINGS_BUILDER(a);
    /* Prevent IdentifierNames and NumericLiterals from running into keywords:
     * in, instanceof, typeof, void */
    bool pad = burrow__ht_is_js_ident_part(first) || burrow__ht_is_js_ident_part(last);
    if (pad)
        hte_write_byte(&buf, ' ');
    Int written = 0;
    /* Make sure that json.Marshal escapes codepoints U+2028 & U+2029 so it
     * falls within the subset of JSON which is valid JS. */
    for (Int i = 0; i < b.len;) {
        Int n;
        Rune r = utf8_decode_rune_in_string(burrow__ht_sub(b, i, b.len), &n);
        Str repl = BURROW_STR_EMPTY;
        if (r == 0x2028)
            repl = BURROW_S("\\u2028");
        else if (r == 0x2029)
            repl = BURROW_S("\\u2029");
        if (repl.len != 0) {
            burrow__ht_write(&buf, burrow__ht_sub(b, written, i));
            burrow__ht_write(&buf, repl);
            written = i + n;
        }
        i += n;
    }
    if (strings_builder_len(&buf) != 0) {
        burrow__ht_write(&buf, burrow__ht_sub(b, written, b.len));
        if (pad)
            hte_write_byte(&buf, ' ');
        return strings_builder_string(&buf);
    }
    return b;
}

static const Str hte_low_unicode_replacement_table[] = {
    [0] = S_("\\u0000"),    [1] = S_("\\u0001"),
    [2] = S_("\\u0002"),    [3] = S_("\\u0003"),
    [4] = S_("\\u0004"),    [5] = S_("\\u0005"),
    [6] = S_("\\u0006"),    ['\a'] = S_("\\u0007"),
    ['\b'] = S_("\\u0008"), ['\t'] = S_("\\t"),
    ['\n'] = S_("\\n"),     ['\v'] = S_("\\u000b"), /* "\v" == "v" on IE 6. */
    ['\f'] = S_("\\f"),     ['\r'] = S_("\\r"),
    [0xe] = S_("\\u000e"),  [0xf] = S_("\\u000f"),
    [0x10] = S_("\\u0010"), [0x11] = S_("\\u0011"),
    [0x12] = S_("\\u0012"), [0x13] = S_("\\u0013"),
    [0x14] = S_("\\u0014"), [0x15] = S_("\\u0015"),
    [0x16] = S_("\\u0016"), [0x17] = S_("\\u0017"),
    [0x18] = S_("\\u0018"), [0x19] = S_("\\u0019"),
    [0x1a] = S_("\\u001a"), [0x1b] = S_("\\u001b"),
    [0x1c] = S_("\\u001c"), [0x1d] = S_("\\u001d"),
    [0x1e] = S_("\\u001e"), [0x1f] = S_("\\u001f"),
};

static const Str hte_js_str_replacement_table[] = {
    [0] = S_("\\u0000"),
    ['\t'] = S_("\\t"),
    ['\n'] = S_("\\n"),
    ['\v'] = S_("\\u000b"), /* "\v" == "v" on IE 6. */
    ['\f'] = S_("\\f"),
    ['\r'] = S_("\\r"),
    /* Encode HTML specials as hex so the output can be embedded in HTML
     * attributes without further encoding. */
    ['"'] = S_("\\u0022"),
    ['`'] = S_("\\u0060"),
    ['&'] = S_("\\u0026"),
    ['\''] = S_("\\u0027"),
    ['+'] = S_("\\u002b"),
    ['/'] = S_("\\/"),
    ['<'] = S_("\\u003c"),
    ['>'] = S_("\\u003e"),
    ['\\'] = S_("\\\\"),
};

/* jsBqStrReplacementTable is like jsStrReplacementTable except it also
 * contains the special characters for JS template literals: $, {, and }. */
static const Str hte_js_bq_str_replacement_table[] = {
    [0] = S_("\\u0000"),
    ['\t'] = S_("\\t"),
    ['\n'] = S_("\\n"),
    ['\v'] = S_("\\u000b"), /* "\v" == "v" on IE 6. */
    ['\f'] = S_("\\f"),
    ['\r'] = S_("\\r"),
    /* Encode HTML specials as hex so the output can be embedded in HTML
     * attributes without further encoding. */
    ['"'] = S_("\\u0022"),
    ['`'] = S_("\\u0060"),
    ['&'] = S_("\\u0026"),
    ['\''] = S_("\\u0027"),
    ['+'] = S_("\\u002b"),
    ['/'] = S_("\\/"),
    ['<'] = S_("\\u003c"),
    ['>'] = S_("\\u003e"),
    ['\\'] = S_("\\\\"),
    ['$'] = S_("\\u0024"),
    ['{'] = S_("\\u007b"),
    ['}'] = S_("\\u007d"),
};

/* jsStrNormReplacementTable is like jsStrReplacementTable but does not
 * overencode existing escapes since this table has no entry for `\`. */
static const Str hte_js_str_norm_replacement_table[] = {
    [0] = S_("\\u0000"),
    ['\t'] = S_("\\t"),
    ['\n'] = S_("\\n"),
    ['\v'] = S_("\\u000b"), /* "\v" == "v" on IE 6. */
    ['\f'] = S_("\\f"),
    ['\r'] = S_("\\r"),
    /* Encode HTML specials as hex so the output can be embedded in HTML
     * attributes without further encoding. */
    ['"'] = S_("\\u0022"),
    ['&'] = S_("\\u0026"),
    ['\''] = S_("\\u0027"),
    ['`'] = S_("\\u0060"),
    ['+'] = S_("\\u002b"),
    ['/'] = S_("\\/"),
    ['<'] = S_("\\u003c"),
    ['>'] = S_("\\u003e"),
};

static const Str hte_js_regexp_replacement_table[] = {
    [0] = S_("\\u0000"),
    ['\t'] = S_("\\t"),
    ['\n'] = S_("\\n"),
    ['\v'] = S_("\\u000b"), /* "\v" == "v" on IE 6. */
    ['\f'] = S_("\\f"),
    ['\r'] = S_("\\r"),
    /* Encode HTML specials as hex so the output can be embedded in HTML
     * attributes without further encoding. */
    ['"'] = S_("\\u0022"),
    ['$'] = S_("\\$"),
    ['&'] = S_("\\u0026"),
    ['\''] = S_("\\u0027"),
    ['('] = S_("\\("),
    [')'] = S_("\\)"),
    ['*'] = S_("\\*"),
    ['+'] = S_("\\u002b"),
    ['-'] = S_("\\-"),
    ['.'] = S_("\\."),
    ['/'] = S_("\\/"),
    ['<'] = S_("\\u003c"),
    ['>'] = S_("\\u003e"),
    ['?'] = S_("\\?"),
    ['['] = S_("\\["),
    ['\\'] = S_("\\\\"),
    [']'] = S_("\\]"),
    ['^'] = S_("\\^"),
    ['{'] = S_("\\{"),
    ['|'] = S_("\\|"),
    ['}'] = S_("\\}"),
};

/* replace replaces each rune r of s with replacementTable[r], provided that
 * r < len(replacementTable). If replacementTable[r] is the empty string then
 * no replacement is made. It also replaces runes U+2028 and U+2029 with the
 * raw strings `\u2028` and `\u2029`. */
static Str hte_replace(Alloc *a, Str s, HteTable table) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int written = 0;
    Int w = 0;
    for (Int i = 0; i < s.len; i += w) {
        /* See comment in htmlEscaper. */
        Rune r = utf8_decode_rune_in_string(burrow__ht_sub(s, i, s.len), &w);
        Str repl;
        if (r < NELEM(hte_low_unicode_replacement_table))
            repl = hte_low_unicode_replacement_table[r];
        else if (r < table.n && table.repl[r].len != 0)
            repl = table.repl[r];
        else if (r == 0x2028)
            repl = BURROW_S("\\u2028");
        else if (r == 0x2029)
            repl = BURROW_S("\\u2029");
        else
            continue;
        if (written == 0)
            hte_grow(&b, s.len);
        burrow__ht_write(&b, burrow__ht_sub(s, written, i));
        burrow__ht_write(&b, repl);
        written = i + w;
    }
    if (written == 0)
        return s;
    burrow__ht_write(&b, burrow__ht_sub(s, written, s.len));
    return strings_builder_string(&b);
}

/* jsStrEscaper produces a string that can be included between quotes in
 * JavaScript source, in JavaScript embedded in an HTML5 <script> element, or
 * in an HTML5 event handler attribute such as onclick. */
Str burrow__ht_js_str_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_JS_STR)
        return hte_replace(a, s, HTE_TABLE(hte_js_str_norm_replacement_table));
    return hte_replace(a, s, HTE_TABLE(hte_js_str_replacement_table));
}

Str burrow__ht_js_tmpl_lit_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    return hte_replace(a, s, HTE_TABLE(hte_js_bq_str_replacement_table));
}

/* jsRegexpEscaper behaves like jsStrEscaper but escapes regular expression
 * specials so the result is treated literally when included in a regular
 * expression literal. /foo{{.X}}bar/ matches the string "foo" followed by the
 * literal text of {{.X}} followed by the string "bar". */
Str burrow__ht_js_regexp_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    s = hte_replace(a, s, HTE_TABLE(hte_js_regexp_replacement_table));
    if (s.len == 0) {
        /* /{{.X}}/ should not produce a line comment when .X == "". */
        return BURROW_S("(?:)");
    }
    return s;
}

/* isJSIdentPart reports whether the given rune is a JS identifier part. It
 * does not handle all the non-Latin letters, joiners, and combining marks,
 * but it does handle every codepoint that can occur in a numeric literal or a
 * keyword. */
bool burrow__ht_is_js_ident_part(Rune r) {
    return r == '$' || ('0' <= r && r <= '9') || ('A' <= r && r <= 'Z') || r == '_' ||
           ('a' <= r && r <= 'z');
}

static const Str hte_js_mime_types[] = {
    S_(""),
    S_("application/ecmascript"),
    S_("application/javascript"),
    S_("application/json"),
    S_("application/ld+json"),
    S_("application/x-ecmascript"),
    S_("application/x-javascript"),
    S_("module"),
    S_("text/ecmascript"),
    S_("text/javascript"),
    S_("text/javascript1.0"),
    S_("text/javascript1.1"),
    S_("text/javascript1.2"),
    S_("text/javascript1.3"),
    S_("text/javascript1.4"),
    S_("text/javascript1.5"),
    S_("text/jscript"),
    S_("text/livescript"),
    S_("text/x-ecmascript"),
    S_("text/x-javascript"),
};

/* isJSType reports whether the given MIME type should be considered
 * JavaScript.
 *
 * It is used to determine whether a script tag with a type attribute is a
 * javascript container. */
bool burrow__ht_is_js_type(Str mime_type) {
    /* per
     *   https://www.w3.org/TR/html5/scripting-1.html#attr-script-type
     *   https://tools.ietf.org/html/rfc7231#section-3.1.1
     *   https://tools.ietf.org/html/rfc4329#section-3
     *   https://www.ietf.org/rfc/rfc4627.txt
     * discard parameters */
    Str after;
    bool found;
    mime_type = strings_cut(mime_type, BURROW_S(";"), &after, &found);
    /* Go lowers with strings.ToLower, which makes nothing that is not already
     * one of the names below into one of them unless it is ASCII, so this
     * compares folding ASCII case only. Unicode folding would let U+212A, the
     * Kelvin sign, stand for the k of livescript. */
    mime_type = strings_trim_space(mime_type);
    for (Int i = 0; i < NELEM(hte_js_mime_types); i++) {
        Str m = hte_js_mime_types[i];
        if (m.len != mime_type.len)
            continue;
        Int j = 0;
        while (j < m.len) {
            Byte c = mime_type.p[j];
            if ('A' <= c && c <= 'Z')
                c = (Byte)(c + ('a' - 'A'));
            if (c != m.p[j])
                break;
            j++;
        }
        if (j == m.len)
            return true;
    }
    return false;
}

/* -------------------------------------------------------------------- CSS */

/* endsWithCSSKeyword reports whether b ends with an ident that
 * case-insensitively matches the lower-case kw. */
bool burrow__ht_ends_with_css_keyword(Str b, Str kw) {
    Int i = b.len - kw.len;
    if (i < 0) {
        /* Too short. */
        return false;
    }
    if (i != 0) {
        Int size;
        Rune r = utf8_decode_last_rune_in_string(burrow__ht_sub(b, 0, i), &size);
        if (burrow__ht_is_css_nmchar(r)) {
            /* Too long. */
            return false;
        }
    }
    /* Many CSS keywords, such as "!important" can have characters encoded,
     * but the URI production does not allow that according to
     * https://www.w3.org/TR/css3-syntax/#TOK-URI
     * This does not attempt to recognize encoded keywords. For example, given
     * "\75\72\6c" and "url" this return false. Go lowers with bytes.ToLower;
     * kw is ASCII, and no other rune lowers to an ASCII letter of the same
     * length in bytes, so lowering ASCII is the same thing. */
    for (Int j = 0; j < kw.len; j++) {
        Byte c = b.p[i + j];
        if ('A' <= c && c <= 'Z')
            c = (Byte)(c + ('a' - 'A'));
        if (c != kw.p[j])
            return false;
    }
    return true;
}

/* isCSSNmchar reports whether rune is allowed anywhere in a CSS identifier. */
bool burrow__ht_is_css_nmchar(Rune r) {
    /* Based on the CSS3 nmchar production but ignores multi-rune escape
     * sequences.
     * https://www.w3.org/TR/css3-syntax/#SUBTOK-nmchar */
    return ('a' <= r && r <= 'z') || ('A' <= r && r <= 'Z') || ('0' <= r && r <= '9') ||
           r == '-' || r == '_' ||
           /* Non-ASCII cases below. */
           (0x80 <= r && r <= 0xd7ff) || (0xe000 <= r && r <= 0xfffd) ||
           (0x10000 <= r && r <= 0x10ffff);
}

/* decodeCSS decodes CSS3 escapes given a sequence of stringchars. If there is
 * no change, it returns the input, otherwise it returns a slice backed by a
 * new array.
 * https://www.w3.org/TR/css3-syntax/#SUBTOK-stringchar defines stringchar. */
Str burrow__ht_decode_css(Alloc *a, Str s) {
    Int i = strings_index_byte(s, '\\');
    if (i == -1)
        return s;
    /* The UTF-8 sequence for a codepoint is never longer than 1 + the number
     * hex digits need to represent that codepoint, so len(s) is an upper
     * bound on the output length. */
    Int cap = s.len;
    Byte *b = mem_alloc_nozero(a, (size_t)cap, 1);
    if (b == NULL)
        burrow__ht_oom();
    Int n = 0;
    while (s.len != 0) {
        i = strings_index_byte(s, '\\');
        if (i == -1)
            i = s.len;
        for (Int k = 0; k < i; k++)
            b[n + k] = s.p[k];
        n += i;
        s = burrow__ht_sub(s, i, s.len);
        if (s.len < 2)
            break;
        /* https://www.w3.org/TR/css3-syntax/#SUBTOK-escape
         * escape ::= unicode | '\' [#x20-#x7E#x80-#xD7FF#xE000-#xFFFD#x10000-#x10FFFF] */
        if (burrow__ht_is_hex(s.p[1])) {
            /* https://www.w3.org/TR/css3-syntax/#SUBTOK-unicode
             *   unicode ::= '\' [0-9a-fA-F]{1,6} wc? */
            Int j = 2;
            while (j < s.len && j < 7 && burrow__ht_is_hex(s.p[j]))
                j++;
            Rune r = burrow__ht_hex_decode(burrow__ht_sub(s, 1, j));
            if (r > UTF8_MAX_RUNE) {
                r /= 16;
                j--;
            }
            n += utf8_encode_rune(slice_from(b + n, cap - n, cap - n, TYPE_BYTE), r);
            s = burrow__ht_skip_css_space(burrow__ht_sub(s, j, s.len));
        } else {
            /* `\\` decodes to `\` and `\"` to `"`. */
            Int w;
            utf8_decode_rune_in_string(burrow__ht_sub(s, 1, s.len), &w);
            for (Int k = 0; k < w; k++)
                b[n + k] = s.p[1 + k];
            n += w;
            s = burrow__ht_sub(s, 1 + w, s.len);
        }
    }
    return (Str){b, n};
}

/* isHex reports whether the given character is a hex digit. */
bool burrow__ht_is_hex(Byte c) {
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

/* hexDecode decodes a short hex digit sequence: "10" -> 16. */
Rune burrow__ht_hex_decode(Str s) {
    Rune n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        n <<= 4;
        if ('0' <= c && c <= '9')
            n |= (Rune)(c - '0');
        else if ('a' <= c && c <= 'f')
            n |= (Rune)(c - 'a') + 10;
        else if ('A' <= c && c <= 'F')
            n |= (Rune)(c - 'A') + 10;
        else
            panic_str(fmt_sprintf_v(heap_allocator(), "Bad hex digit in %q", s));
    }
    return n;
}

/* skipCSSSpace returns a suffix of c, skipping over a single space. */
Str burrow__ht_skip_css_space(Str c) {
    if (c.len == 0)
        return c;
    /* wc ::= #x9 | #xA | #xC | #xD | #x20 */
    switch (c.p[0]) {
    case '\t':
    case '\n':
    case '\f':
    case ' ':
        return burrow__ht_sub(c, 1, c.len);
    case '\r':
        /* This differs from CSS3's wc production because it contains a
         * probable spec error whereby wc contains all the single byte
         * sequences in nl (newline) but not CRLF. */
        if (c.len >= 2 && c.p[1] == '\n')
            return burrow__ht_sub(c, 2, c.len);
        return burrow__ht_sub(c, 1, c.len);
    default:
        break;
    }
    return c;
}

/* isCSSSpace reports whether b is a CSS space char as defined in wc. */
static bool hte_is_css_space(Byte b) {
    switch (b) {
    case '\t':
    case '\n':
    case '\f':
    case '\r':
    case ' ':
        return true;
    default:
        return false;
    }
}

static const Str hte_css_replacement_table[] = {
    [0] = S_("\\0"),
    ['\t'] = S_("\\9"),
    ['\n'] = S_("\\a"),
    ['\f'] = S_("\\c"),
    ['\r'] = S_("\\d"),
    /* Encode HTML specials as hex so the output can be embedded in HTML
     * attributes without further encoding. */
    ['"'] = S_("\\22"),
    ['&'] = S_("\\26"),
    ['\''] = S_("\\27"),
    ['('] = S_("\\28"),
    [')'] = S_("\\29"),
    ['+'] = S_("\\2b"),
    ['/'] = S_("\\2f"),
    [':'] = S_("\\3a"),
    [';'] = S_("\\3b"),
    ['<'] = S_("\\3c"),
    ['>'] = S_("\\3e"),
    ['\\'] = S_("\\\\"),
    ['{'] = S_("\\7b"),
    ['}'] = S_("\\7d"),
};

/* cssEscaper escapes HTML and CSS special characters using \<hex>+ escapes. */
Str burrow__ht_css_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    StringsBuilder b = STRINGS_BUILDER(a);
    Int written = 0;
    Int w = 0;
    for (Int i = 0; i < s.len; i += w) {
        /* See comment in htmlEscaper. */
        Rune r = utf8_decode_rune_in_string(burrow__ht_sub(s, i, s.len), &w);
        if (r >= NELEM(hte_css_replacement_table) ||
            hte_css_replacement_table[r].len == 0)
            continue;
        Str repl = hte_css_replacement_table[r];
        if (written == 0)
            hte_grow(&b, s.len);
        burrow__ht_write(&b, burrow__ht_sub(s, written, i));
        burrow__ht_write(&b, repl);
        written = i + w;
        if (!str_eq(repl, BURROW_S("\\\\")) &&
            (written == s.len || burrow__ht_is_hex(s.p[written]) ||
             hte_is_css_space(s.p[written])))
            hte_write_byte(&b, ' ');
    }
    if (written == 0)
        return s;
    burrow__ht_write(&b, burrow__ht_sub(s, written, s.len));
    return strings_builder_string(&b);
}

/* cssValueFilter allows innocuous CSS values in the output including CSS
 * quantities (10px or 25%), ID or class literals (#foo, .bar), keyword values
 * (inherit, blue), and colors (#888). It filters out unsafe values, such as
 * those that affect token boundaries, and anything that might execute
 * scripts. */
Str burrow__ht_css_value_filter(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_CSS)
        return s;
    Str b = burrow__ht_decode_css(a, s);
    Byte *id = b.len > 0 ? mem_alloc_nozero(a, (size_t)b.len, 1) : NULL;
    if (b.len > 0 && id == NULL)
        burrow__ht_oom();
    Int nid = 0;

    /* CSS3 error handling is specified as honoring string boundaries per
     * https://www.w3.org/TR/css3-syntax/#error-handling :
     *     Malformed declarations. User agents must handle unexpected tokens
     *     encountered while parsing a declaration by reading until the end of
     *     the declaration, while observing the rules for matching pairs of (),
     *     [], {}, "", and '', and correctly handling escapes. For example, a
     *     malformed declaration may be missing a property, colon (:) or value.
     * So we need to make sure that values do not have mismatched bracket or
     * quote characters to prevent the browser from restarting parsing inside a
     * string that might embed JavaScript source. */
    for (Int i = 0; i < b.len; i++) {
        Byte c = b.p[i];
        switch (c) {
        case 0:
        case '"':
        case '\'':
        case '(':
        case ')':
        case '/':
        case ';':
        case '@':
        case '[':
        case '\\':
        case ']':
        case '`':
        case '{':
        case '}':
        case '<':
        case '>':
            return hte_filter_failsafe;
        case '-':
            /* Disallow <!-- or -->.
             * -- should not appear in valid identifiers. */
            if (i != 0 && b.p[i - 1] == '-')
                return hte_filter_failsafe;
            break;
        default:
            if (c < UTF8_RUNE_SELF && burrow__ht_is_css_nmchar(c)) {
                id[nid++] = ('A' <= c && c <= 'Z') ? (Byte)(c + ('a' - 'A')) : c;
            }
            break;
        }
    }
    Str ids = {id, nid};
    if (strings_contains(ids, BURROW_S("expression")) ||
        strings_contains(ids, BURROW_S("mozbinding")))
        return hte_filter_failsafe;
    return b;
}

/* -------------------------------------------------------------------- URL */

/* urlFilter returns its input unless it contains an unsafe scheme in which
 * case it defangs the entire URL.
 *
 * Schemes that cause unintended side effects that are irreversible without
 * user interaction are considered unsafe. For example, clicking on a
 * "javascript:" link can immediately trigger JavaScript code execution.
 *
 * This filter conservatively assumes that all schemes other than the
 * following are unsafe:
 *    * http:   Navigates to a new website, and may open a new window or tab.
 *              These side effects can be reversed by navigating back to the
 *              previous website, or closing the window or tab. No irreversible
 *              changes will take place without further user interaction with
 *              the new website.
 *    * https:  Same as http.
 *    * mailto: Opens an email program and starts a new draft. This side
 *              effect is not irreversible until the user explicitly clicks
 *              send; it can be undone by closing the email program.
 *
 * To allow URLs containing other schemes to bypass this filter, developers
 * must explicitly indicate that such a URL is expected and safe by
 * encapsulating it in a template.URL value. */
Str burrow__ht_url_filter(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_URL)
        return s;
    if (!burrow__ht_is_safe_url(s))
        return BURROW_S("#" BURROW__HT_FILTER_FAILSAFE);
    return s;
}

/* isSafeURL is true if s is a relative URL or if URL has a protocol in
 * (http, https, mailto). */
bool burrow__ht_is_safe_url(Str s) {
    Str after;
    bool ok;
    Str protocol = strings_cut(s, BURROW_S(":"), &after, &ok);
    if (ok && !strings_contains(protocol, BURROW_S("/"))) {
        if (!strings_equal_fold(protocol, BURROW_S("http")) &&
            !strings_equal_fold(protocol, BURROW_S("https")) &&
            !strings_equal_fold(protocol, BURROW_S("mailto")))
            return false;
    }
    return true;
}

static Str hte_url_processor(Alloc *a, bool norm, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    if (t == BURROW__HT_CONTENT_URL)
        norm = true;
    StringsBuilder b = STRINGS_BUILDER(a);
    if (burrow__ht_process_url_onto(s, norm, &b))
        return strings_builder_string(&b);
    return s;
}

/* urlEscaper produces an output that can be embedded in a URL query. The
 * output can be embedded in an HTML attribute without further escaping. */
Str burrow__ht_url_escaper(Alloc *a, Slice args) {
    return hte_url_processor(a, false, args);
}

/* urlNormalizer normalizes URL content so it can be embedded in a quote-delimited
 * string or parenthesis delimited url(...). The normalizer does not encode all
 * HTML specials. Specifically, it does not encode '&' so correct embedding in
 * an HTML attribute requires escaping of '&' to '&amp;'. */
Str burrow__ht_url_normalizer(Alloc *a, Slice args) {
    return hte_url_processor(a, true, args);
}

/* processURLOnto appends a normalized URL corresponding to its input to b and
 * reports whether the appended content differs from s. */
bool burrow__ht_process_url_onto(Str s, bool norm, StringsBuilder *b) {
    static const char hex[] = "0123456789abcdef";
    hte_grow(b, s.len + 16);
    Int written = 0;
    /* The byte loop below assumes that all URLs use UTF-8 as the content-encoding.
     * This is similar to the URI to IRI encoding scheme defined in section 3.1
     * of RFC 3987, and behaves the same as the EcmaScript builtin
     * encodeURIComponent. It should not cause any misencoding of URLs in pages
     * with Content-type: text/html;charset=UTF-8. */
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        switch (c) {
        /* Single quote and parens are sub-delims in RFC 3986, but we escape
         * them so the output can be embedded in single quoted attributes and
         * unquoted CSS url(...) constructs. Single quotes are reserved in
         * URLs, but are only used in the obsolete "mark" rule in an appendix
         * in RFC 3986 so can be safely encoded. */
        case '!':
        case '#':
        case '$':
        case '&':
        case '*':
        case '+':
        case ',':
        case '/':
        case ':':
        case ';':
        case '=':
        case '?':
        case '@':
        case '[':
        case ']':
            if (norm)
                continue;
            break;
        /* Unreserved according to RFC 3986 sec 2.3
         * "For consistency, percent-encoded octets in the ranges of ALPHA
         * (%41-%5A and %61-%7A), DIGIT (%30-%39), hyphen (%2D), period (%2E),
         * underscore (%5F), or tilde (%7E) should not be created by URI
         * producers */
        case '-':
        case '.':
        case '_':
        case '~':
            continue;
        case '%':
            /* When normalizing do not re-encode valid escapes. */
            if (norm && i + 2 < s.len && burrow__ht_is_hex(s.p[i + 1]) &&
                burrow__ht_is_hex(s.p[i + 2]))
                continue;
            break;
        default:
            /* Unreserved according to RFC 3986 sec 2.3 */
            if ('a' <= c && c <= 'z')
                continue;
            if ('A' <= c && c <= 'Z')
                continue;
            if ('0' <= c && c <= '9')
                continue;
            break;
        }
        burrow__ht_write(b, burrow__ht_sub(s, written, i));
        Byte esc[3] = {'%', (Byte)hex[c >> 4], (Byte)hex[c & 0xf]};
        burrow__ht_write(b, (Str){esc, 3});
        written = i + 1;
    }
    burrow__ht_write(b, burrow__ht_sub(s, written, s.len));
    return written != 0;
}

/* htmlSpaceAndASCIIAlnumBytes is a bitmask of the bytes that are HTML space
 * or ASCII alphanumeric. Derived from the Go code it was written to match. */
static const Byte hte_html_space_and_ascii_alnum_bytes[16] = {
    0x00, 0x36, 0x00, 0x00, 0x01, 0x00, 0xff, 0x03,
    0xfe, 0xff, 0xff, 0x07, 0xfe, 0xff, 0xff, 0x07,
};

/* isHTMLSpace is true iff c is a whitespace character per
 * https://infra.spec.whatwg.org/#ascii-whitespace */
static bool hte_is_html_space(Byte c) {
    return c <= 0x20 &&
           (hte_html_space_and_ascii_alnum_bytes[c >> 3] & (1U << (c & 0x7))) != 0;
}

static bool hte_is_html_space_or_ascii_alnum(Byte c) {
    return c < 0x80 &&
           (hte_html_space_and_ascii_alnum_bytes[c >> 3] & (1U << (c & 0x7))) != 0;
}

static void hte_filter_srcset_element(Str s, Int left, Int right, StringsBuilder *b) {
    Int start = left;
    while (start < right && hte_is_html_space(s.p[start]))
        start++;
    Int end = right;
    for (Int i = start; i < right; i++) {
        if (hte_is_html_space(s.p[i])) {
            end = i;
            break;
        }
    }
    Str url = burrow__ht_sub(s, start, end);
    if (burrow__ht_is_safe_url(url)) {
        /* If image metadata is only spaces or alnums then we don't need to
         * URL normalize it. */
        bool metadata_ok = true;
        for (Int i = end; i < right; i++) {
            if (!hte_is_html_space_or_ascii_alnum(s.p[i])) {
                metadata_ok = false;
                break;
            }
        }
        if (metadata_ok) {
            burrow__ht_write(b, burrow__ht_sub(s, left, start));
            burrow__ht_process_url_onto(url, true, b);
            burrow__ht_write(b, burrow__ht_sub(s, end, right));
            return;
        }
    }
    burrow__ht_write(b, BURROW_S("#"));
    burrow__ht_write(b, hte_filter_failsafe);
}

Str burrow__ht_srcset_filter_and_escaper(Alloc *a, Slice args) {
    burrow__HtContentType t;
    Str s = burrow__ht_stringify(a, args, &t);
    switch (t) {
    case BURROW__HT_CONTENT_SRCSET:
        return s;
    case BURROW__HT_CONTENT_URL: {
        /* Normalizing gets rid of all HTML whitespace which separate the image
         * URL from its metadata. */
        StringsBuilder b = STRINGS_BUILDER(a);
        if (burrow__ht_process_url_onto(s, true, &b))
            s = strings_builder_string(&b);
        /* Additionally, commas separate one source from another. */
        return strings_replace_all(a, s, BURROW_S(","), BURROW_S("%2c"));
    }
    default:
        break;
    }

    StringsBuilder b = STRINGS_BUILDER(a);
    Int written = 0;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == ',') {
            hte_filter_srcset_element(s, written, i, &b);
            burrow__ht_write(&b, BURROW_S(","));
            written = i + 1;
        }
    }
    hte_filter_srcset_element(s, written, s.len, &b);
    return strings_builder_string(&b);
}
