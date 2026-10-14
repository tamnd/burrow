/* html/template, data-driven templates for generating HTML output safe against
 * code injection.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package html/template */

/* Go's html/template. It is text/template with escaping that knows where in
 * the page each value lands: text between tags, an attribute, a URL, a script
 * or a style sheet, and escapes the value for that place.
 *
 * Content. A value of one of the string types below is content the template
 * trusts, of the kind its name says, and is not escaped where that kind is
 * expected. Everything else is escaped. A type here is a Str under another
 * name with a descriptor of its own, so an Any that holds one says what it is:
 *
 *     HtmlTemplateHTML h = BURROW_S("<b>World</b>");
 *     Any v = BURROW_ANY(TYPE_HTML_TEMPLATE_HTML, &h);
 *
 * As in Go, using these types is a promise that the content is safe, and the
 * template takes it at its word. Don't put text from a user in one.
 *
 * Templates. html_template_new, html_template_parse and html_template_execute
 * are used the way text/template's are. The first run escapes the template:
 *
 *     Error err = BURROW_NO_ERROR;
 *     HtmlTemplate *t = html_template_parse(html_template_new(a, BURROW_S("p")),
 *                                           BURROW_S("<p>{{.}}</p>"), &err);
 *     t = html_template_must(t, err);
 *     Str s = BURROW_S("<b>World</b>");
 *     err = html_template_execute(t, out, BURROW_ANY(TYPE_STRING, &s));
 *
 * writes <p>&lt;b&gt;World&lt;/b&gt;</p>, where an HtmlTemplateHTML holding
 * the same bytes would have been written as it is. Once a template or one
 * associated with it has run, none of them can be parsed into or cloned.
 *
 * The escapers. html_template_html_escaper and the others below are what the
 * template's html, js and urlquery functions do, for use outside a template.
 * They are text/template's under the names Go gives them here. */

#ifndef BURROW_HTML_TEMPLATE_H
#define BURROW_HTML_TEMPLATE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/text/template.h"
#include "burrow/text/template/parse.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- content */

/* CSS, content that matches one of a CSS3 stylesheet, a rule, a declaration,
 * a value or a list of declarations, such as `a { color: red }` or `color:
 * red`. */
typedef Str HtmlTemplateCSS;

/* HTML, a known safe HTML document fragment. Don't use it for HTML from a third
 * party, or with unclosed tags or comments: the output of a sanitizer and of a
 * template escaper is what it is for. */
typedef Str HtmlTemplateHTML;

/* HTMLAttr, an HTML attribute from a trusted source, such as ` dir="ltr"`. */
typedef Str HtmlTemplateHTMLAttr;

/* JS, a known safe EcmaScript5 expression, such as `(x + y * z())`. It is put
 * into a script as it is, so a template author who uses it is the one who
 * makes sure it does not break the expression around it. */
typedef Str HtmlTemplateJS;

/* JSStr, a sequence of characters meant to be embedded between quotes in a
 * JavaScript expression, such as `Hello, \"World\"`. */
typedef Str HtmlTemplateJSStr;

/* URL, a known safe URL or URL substring, from a trusted source. A URL such
 * as `javascript:checkThatFormNotEditedBeforeLeavingPage()` is one a template
 * would refuse, which is what the type is for. */
typedef Str HtmlTemplateURL;

/* Srcset, a known safe srcset attribute, such as `image.png 2x`. */
typedef Str HtmlTemplateSrcset;

extern const Type burrow_type_HtmlTemplateCSS;
extern const Type burrow_type_HtmlTemplateHTML;
extern const Type burrow_type_HtmlTemplateHTMLAttr;
extern const Type burrow_type_HtmlTemplateJS;
extern const Type burrow_type_HtmlTemplateJSStr;
extern const Type burrow_type_HtmlTemplateURL;
extern const Type burrow_type_HtmlTemplateSrcset;

#define TYPE_HTML_TEMPLATE_CSS TYPE_OF(HtmlTemplateCSS)
#define TYPE_HTML_TEMPLATE_HTML TYPE_OF(HtmlTemplateHTML)
#define TYPE_HTML_TEMPLATE_HTML_ATTR TYPE_OF(HtmlTemplateHTMLAttr)
#define TYPE_HTML_TEMPLATE_JS TYPE_OF(HtmlTemplateJS)
#define TYPE_HTML_TEMPLATE_JS_STR TYPE_OF(HtmlTemplateJSStr)
#define TYPE_HTML_TEMPLATE_URL TYPE_OF(HtmlTemplateURL)
#define TYPE_HTML_TEMPLATE_SRCSET TYPE_OF(HtmlTemplateSrcset)

/* ------------------------------------------------------------------ errors */

/* ErrorCode, the kind of an escaping error. */
typedef int HtmlTemplateErrorCode;

enum {
    /* No error. */
    HTML_TEMPLATE_OK = 0,

    /* A value lands in an ambiguous context within a URL, as in
     * <a href="{{if .C}}/path/{{else}}/search?q={{end}}{{.X}}">, where {{.X}}
     * is in the path or in the query depending on .C. */
    HTML_TEMPLATE_ERR_AMBIG_CONTEXT,

    /* Bad HTML, such as an unquoted attribute value with " ' < = or ` in it,
     * or a tag that is never closed. */
    HTML_TEMPLATE_ERR_BAD_HTML,

    /* The branches of an {{if}}, {{range}} or {{with}} end in different
     * contexts. */
    HTML_TEMPLATE_ERR_BRANCH_END,

    /* A template ends in a context other than text. */
    HTML_TEMPLATE_ERR_END_CONTEXT,

    /* A {{template}} action names a template that is not there. */
    HTML_TEMPLATE_ERR_NO_SUCH_TEMPLATE,

    /* The output context of a template cannot be worked out. */
    HTML_TEMPLATE_ERR_OUTPUT_CONTEXT,

    /* A JS regular expression ends inside a character set. */
    HTML_TEMPLATE_ERR_PARTIAL_CHARSET,

    /* A string, regular expression or CSS string ends in the middle of an
     * escape sequence. */
    HTML_TEMPLATE_ERR_PARTIAL_ESCAPE,

    /* A {{range}} body ends in a context other than the one it starts in, so
     * a second time round would be escaped wrongly. */
    HTML_TEMPLATE_ERR_RANGE_LOOP_REENTRY,

    /* A / in a script could start a division or a regular expression. */
    HTML_TEMPLATE_ERR_SLASH_AMBIG,

    /* A pipeline uses html or urlquery, the escapers text/template
     * predefines, somewhere other than as its last command, or uses html in
     * an unquoted attribute value. The template escapes every pipeline
     * already, so they are not needed. */
    HTML_TEMPLATE_ERR_PREDEFINED_ESCAPER,

    /* Not returned any more. Actions in JS template literals are escaped
     * like the rest, as in Go 1.21 and later. */
    HTML_TEMPLATE_ERR_JS_TEMPLATE
};

/* Error, a problem escaping a template. node is where it went wrong, or NULL
 * when that is not known. name is the name of the template, line its line
 * number when it is known and 0 when not, and description says what is wrong
 * in a form a person can read. */
typedef struct HtmlTemplateError {
    HtmlTemplateErrorCode error_code;
    ParseNode node;
    Str name;
    Int line;
    Str description;
} HtmlTemplateError;

extern const Type *const TYPE_HTML_TEMPLATE_ERROR;

/* Error.Error: the message, made in a, as html/template:LOCATION: DESCRIPTION,
 * where the location is the node's when there is one and the name and line
 * when not. */
BURROW_OWNS(ret) Str html_template_error_error(const HtmlTemplateError *e, Alloc *a);

/* An Error for e, with its message made and e copied into a. errors_as with
 * TYPE_HTML_TEMPLATE_ERROR gives the copy back. On an allocation failure you
 * get burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error html_template_error_as_error(Alloc *a,
                                                    const HtmlTemplateError *e);

/* --------------------------------------------------------------- templates */

/* FuncMap, text/template's. */
typedef TextTemplateFuncMap HtmlTemplateFuncMap;

/* The part of a template that the templates associated with it share. */
typedef struct burrow__HtNameSpace burrow__HtNameSpace;

/* Template, a text/template template that makes output safe to put in an HTML
 * page. text is the text/template template underneath it, and tree is that
 * template's parse tree, NULL until the template is parsed. The first time a
 * template runs, it is escaped: its tree is rewritten so that every action's
 * output goes through the escaper for where it lands. escape_err is what
 * escaping gave, nil before the template has been escaped, and is kept, so
 * a template that failed to escape fails each time it is run. */
typedef struct HtmlTemplate {
    Error escape_err;
    TextTemplate *text;
    ParseTree *tree;
    burrow__HtNameSpace *ns;
} HtmlTemplate;

/* New: a new HTML template with the given name, made in a, as are the
 * templates associated with it, their parse trees and what escaping them
 * makes. NULL when a refuses. */
BURROW_OWNS(ret) HtmlTemplate *html_template_new(Alloc *a, Str name);

/* Template.New: a new HTML template associated with t, with the same
 * delimiters. A template already there with the name is reset and is not
 * associated with t any more. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_new_associated(HtmlTemplate *t,
                                                                  Str name);

/* Name: the template's name. */
BURROW_BORROWS(ret, t) Str html_template_name(const HtmlTemplate *t);

/* Templates: the templates associated with t, t included, as a Slice of
 * HtmlTemplate pointers made in a, in no particular order. */
BURROW_OWNS(ret) Slice html_template_templates(HtmlTemplate *t, Alloc *a);

/* Option: text_template_option for t. Returns t. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_option(HtmlTemplate *t, Slice opt);

/* Execute: escapes t, the first time, and runs it with data, writing what it
 * makes to wr. An escaping error is an HtmlTemplateError and comes back every
 * time; the other errors are text_template_execute's. Templates can be run
 * from several threads at once. */
BURROW_STATIC(ret) Error html_template_execute(HtmlTemplate *t, IoWriter wr, Any data);

/* ExecuteTemplate: Execute for the template called name associated with t. */
BURROW_STATIC(ret) Error html_template_execute_template(HtmlTemplate *t, IoWriter wr,
                                                        Str name, Any data);

/* DefinedTemplates: text_template_defined_templates for t. */
BURROW_OWNS(ret) Str html_template_defined_templates(HtmlTemplate *t, Alloc *a);

/* Parse: parses text as the body of t, as text_template_parse does, and adds
 * the templates it defines to t's. Templates can be redefined until t or one
 * associated with it has been run, and after that Parse fails. Returns t, or
 * NULL with the error in *err. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_parse(HtmlTemplate *t, Str text,
                                                         Error *err);

/* AddParseTree: a new template with the name and parse tree, associated with
 * t. Fails once t or one associated with it has been run. */
BURROW_BORROWS(ret, t) HtmlTemplate *
html_template_add_parse_tree(HtmlTemplate *t, Str name, ParseTree *tree, Error *err);

/* Clone: a copy of t and the templates associated with it, made in a, with the
 * parse trees copied, so that templates parsed into the clone are not in t.
 * Fails once t has been run. */
BURROW_OWNS(ret) HtmlTemplate *html_template_clone(HtmlTemplate *t, Alloc *a,
                                                   Error *err);

/* Funcs: text_template_funcs for t. Returns t. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_funcs(HtmlTemplate *t,
                                                         HtmlTemplateFuncMap funcs);

/* Delims: text_template_delims for t. Returns t. */
BURROW_RETAINS(left, right) BURROW_BORROWS(ret, t) HtmlTemplate *
html_template_delims(HtmlTemplate *t, Str left, Str right);

/* Lookup: the template called name associated with t, or NULL. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_lookup(HtmlTemplate *t, Str name);

/* Must: t, or a panic with err when it failed. */
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_must(HtmlTemplate *t, Error err);

/* ParseFiles, ParseGlob and ParseFS, as text/template's. The method forms
 * parse into t and return it, and the package level forms make the templates
 * in a and return the one for the first file. */
BURROW_BORROWS(ret, t) HtmlTemplate *
html_template_parse_files(HtmlTemplate *t, Slice filenames, Error *err);
BURROW_OWNS(ret) HtmlTemplate *html_template_from_files(Alloc *a, Slice filenames,
                                                        Error *err);
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_parse_glob(HtmlTemplate *t,
                                                              Str pattern, Error *err);
BURROW_OWNS(ret) HtmlTemplate *html_template_from_glob(Alloc *a, Str pattern,
                                                       Error *err);
BURROW_BORROWS(ret, t) HtmlTemplate *html_template_parse_fs(HtmlTemplate *t, Fs fsys,
                                                            Slice patterns, Error *err);
BURROW_OWNS(ret) HtmlTemplate *html_template_from_fs(Alloc *a, Fs fsys, Slice patterns,
                                                     Error *err);

/* ---------------------------------------------------------------- escapers
 *
 * What text/template's html, js and urlquery functions do. The Writer forms
 * write b escaped to w. The String forms return s itself when there is nothing
 * to escape and an escaped copy made in a when there is. The escapers take a
 * Slice of Any, the arguments a template function is given, and escape what
 * fmt_sprint makes of them, or the one string when there is one. */
void html_template_html_escape(IoWriter w, Slice b);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str html_template_html_escape_string(Alloc *a,
                                                                             Str s);
BURROW_OWNS(ret) Str html_template_html_escaper(Alloc *a, Slice args);

void html_template_js_escape(IoWriter w, Slice b);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str html_template_js_escape_string(Alloc *a,
                                                                           Str s);
BURROW_OWNS(ret) Str html_template_js_escaper(Alloc *a, Slice args);

BURROW_OWNS(ret) Str html_template_url_query_escaper(Alloc *a, Slice args);

/* IsTrue: whether val is true in the sense of if and the other actions, which
 * is not the zero value of its type, and in *ok whether val has a truth value
 * at all. It is text_template_is_true. */
bool html_template_is_true(Any val, bool *ok);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HTML_TEMPLATE_H */
