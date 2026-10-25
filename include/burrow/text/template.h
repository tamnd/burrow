/* text/template, data-driven templates for generating text.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package text/template */

/* Go's text/template. A template is text with actions in it, between {{ and
 * }}, that read from the data the template is run with:
 *
 *     Arena ar;
 *     arena_init(&ar, heap_allocator(), 0);
 *     Error err = BURROW_NO_ERROR;
 *     TextTemplate *t = text_template_new(arena_allocator(&ar), BURROW_S("hello"));
 *     text_template_parse(t, BURROW_S("Hello, {{.Name}}!\n"), &err);
 *     Person p = {BURROW_S("Gopher")};
 *     IoWriter out = os_file_as_io_writer(os_stdout);
 *     err = text_template_execute(t, out, BURROW_ANY(TYPE_OF(Person), &p));
 *
 * The language is Go's, and so is what it does with the data. A template reads
 * the data through reflect, so the data's type needs a descriptor: one from
 * BURROW_STRUCT or the other macros in burrow/declare.h, or one of the builtin
 * types. A method a template calls has to be listed with its signature, as
 * BURROW_STRUCT_DEFINE_METHODS lists them, and so does a function in a
 * TextTemplateFuncMap, which is a function value whose type was declared with
 * BURROW_FUNC_TYPE_DEFINE. A method or function that returns a value and an
 * error says so with OUT(ErrorResult), as declare.h explains, and an error
 * stops the template the way it does in Go.
 *
 * Memory. A template, the templates associated with it and their parse trees
 * are made in the allocator text_template_new was given, and so are the
 * copies text_template_clone makes in the allocator it is given. Use an arena
 * and free it, with everything in it, when the templates are done with.
 * Execute makes what it needs while it runs in an arena of its own and frees
 * it before it returns. The errors it returns live in the calling goroutine's
 * error arena, the way the errors of the rest of the library do.
 *
 * Templates can be executed from several threads at once, as in Go. Parsing,
 * Funcs and the other calls that change a template must not run while it is
 * being executed. */

#ifndef BURROW_TEXT_TEMPLATE_H
#define BURROW_TEXT_TEMPLATE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/text/template/parse.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FuncMap, the functions a template can call by name. It is a Map from Str to
 * Any, and each Any holds a function value whose type was declared with
 * BURROW_FUNC_TYPE_DEFINE, so that the template can see its parameters and
 * call it. The function has one result, or a result and an error, which its
 * signature gives as OUT(ErrorResult). The values are copied when the map is
 * handed to text_template_funcs, and the function values they point at have to
 * outlive the template. */
typedef Map *TextTemplateFuncMap;

/* The part of a template that the templates associated with it share. */
typedef struct burrow__TextTemplateCommon burrow__TextTemplateCommon;

/* Template, the representation of a parsed template. Go embeds a *parse.Tree,
 * which is tree here and is NULL until the template is parsed.
 *
 * a is the allocator the template and everything it parses are made in.
 * text_template_new sets it. A zeroed template, Go's var t Template, needs it
 * set before anything else is done with it, which TEXT_TEMPLATE does:
 *
 *     TextTemplate t = TEXT_TEMPLATE(arena_allocator(&ar));
 */
typedef struct TextTemplate {
    Str name;
    ParseTree *tree;
    burrow__TextTemplateCommon *common;
    Str left_delim;
    Str right_delim;
    Alloc *a;
} TextTemplate;

#define TEXT_TEMPLATE(alloc) ((TextTemplate){.a = (alloc)})

/* New: a new, undefined template with the given name, made in a. NULL when a
 * refuses. */
BURROW_OWNS(ret) TextTemplate *text_template_new(Alloc *a, Str name);

/* Template.New: a new, undefined template associated with t, with the same
 * delimiters. Templates that are associated can name each other in a
 * {{template}} action. The template with a name is not there until it is
 * parsed, as in Go. Named for what it does, since text_template_new is the
 * package level New. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_new_associated(TextTemplate *t,
                                                                  Str name);

/* Name: the template's name. */
BURROW_BORROWS(ret, t) Str text_template_name(const TextTemplate *t);

/* Clone: a copy of t and of every template associated with it, made in a. A
 * clone can be given more templates and functions without changing t, which
 * is what it is for: the common templates are parsed once, and each clone
 * adds its own. The parse trees are shared rather than copied, as in Go, so t's
 * allocator has to outlive the clone. The error is always nil and is there
 * because Go's Clone has one. */
BURROW_OWNS(ret) TextTemplate *text_template_clone(TextTemplate *t, Alloc *a,
                                                   Error *err);

/* AddParseTree: associates tree with t under name, making a new template for it
 * when name is not t's own. A tree that is empty does not replace one that is
 * already there. */
BURROW_BORROWS(ret, t) TextTemplate *
text_template_add_parse_tree(TextTemplate *t, Str name, ParseTree *tree, Error *err);

/* Templates: the templates associated with t, t included, as a Slice of
 * TextTemplate pointers made in a, in no particular order. */
BURROW_OWNS(ret) Slice text_template_templates(TextTemplate *t, Alloc *a);

/* Delims: the delimiters for the parses that follow. Empty ones are {{ and }}.
 * The strings are kept, not copied. Returns t. */
BURROW_RETAINS(left, right) BURROW_BORROWS(ret, t) TextTemplate *
text_template_delims(TextTemplate *t, Str left, Str right);

/* Funcs: adds the functions in funcs to t's, replacing any with the same name.
 * Panics when a name is not an identifier, when a value is not a function, and
 * when a function does not return one value or a value and an error. Has to be
 * called before the templates that use the functions are parsed. Returns t. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_funcs(TextTemplate *t,
                                                         TextTemplateFuncMap funcs);

/* Lookup: the template called name associated with t, or NULL. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_lookup(TextTemplate *t, Str name);

/* Parse: parses text as the body of t. The templates it defines with {{define}}
 * and {{block}} are associated with t. Parsing again can redefine them, and a
 * body that is only spaces and comments leaves one that is already there
 * alone. Returns t, or NULL with the error in *err. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_parse(TextTemplate *t, Str text,
                                                         Error *err);

/* Execute: runs t with data and writes what it makes to wr. data is what dot
 * starts as. An Any holding a ReflectValue stands for the value the
 * ReflectValue holds. On an error the output stops where it was, and what had
 * been written stays written. An error in the template is a
 * TextTemplateExecError, and an error from wr is returned as it is. */
BURROW_STATIC(ret) Error text_template_execute(TextTemplate *t, IoWriter wr, Any data);

/* ExecuteTemplate: Execute for the template called name associated with t. */
BURROW_STATIC(ret) Error text_template_execute_template(TextTemplate *t, IoWriter wr,
                                                        Str name, Any data);

/* DefinedTemplates: "; defined templates are: " and the quoted names of the
 * templates associated with t that have a body, made in a, or the empty string
 * when there are none. For error messages. */
BURROW_OWNS(ret) Str text_template_defined_templates(TextTemplate *t, Alloc *a);

/* Option: sets options, each a Str of the form key=value. The one there is,
 * missingkey, says what a map index with a missing key gives:
 *
 *     missingkey=default or missingkey=invalid, "<no value>" when printed
 *     missingkey=zero, the zero value of the map's element type
 *     missingkey=error, an error that stops the template
 *
 * Panics on an option it does not know. Returns t. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_option(TextTemplate *t, Slice opt);

/* IsTrue: whether val is true in the sense of if and the other actions, which
 * is not the zero value of its type, and in *ok whether val has a truth value
 * at all. Structs are always true. */
bool text_template_is_true(Any val, bool *ok);

/* Must: t, or a panic with err when it failed. Go wraps the call in it, but
 * in C the order a call's arguments are evaluated in is unspecified, so err
 * could be read before the call inside sets it. Call it on its own line:
 *
 *     TextTemplate *t =
 *         text_template_parse(text_template_new(a, BURROW_S("x")), text, &err);
 *     t = text_template_must(t, err);
 */
BURROW_BORROWS(ret, t) TextTemplate *text_template_must(TextTemplate *t, Error err);

/* ParseFiles: parses the files, each as the body of a template named for the
 * file's base name, and associates them. The method form parses them into t,
 * where a file named like t is t's body, and returns t. The package level form
 * makes the templates in a and returns the one for the first file. Both fail
 * when the Slice of Str names no files. */
BURROW_BORROWS(ret, t) TextTemplate *
text_template_parse_files(TextTemplate *t, Slice filenames, Error *err);
BURROW_OWNS(ret) TextTemplate *text_template_from_files(Alloc *a, Slice filenames,
                                                        Error *err);

/* ParseGlob: ParseFiles for the files that match pattern, which is
 * filepath_glob's. Fails when nothing matches. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_parse_glob(TextTemplate *t,
                                                              Str pattern, Error *err);
BURROW_OWNS(ret) TextTemplate *text_template_from_glob(Alloc *a, Str pattern,
                                                       Error *err);

/* ParseFS: ParseFiles for the files in fsys that match the patterns, a Slice
 * of Str in fs_glob's syntax. Fails when a pattern matches nothing. */
BURROW_BORROWS(ret, t) TextTemplate *text_template_parse_fs(TextTemplate *t, Fs fsys,
                                                            Slice patterns, Error *err);
BURROW_OWNS(ret) TextTemplate *text_template_from_fs(Alloc *a, Fs fsys, Slice patterns,
                                                     Error *err);

/* ParseTree's Copy and ErrorContext, which Go's Template has through the tree
 * it embeds. Copy copies t's tree into a. ErrorContext gives where n is, as
 * name:line:column, and n written out in *context, both made in a. */
BURROW_OWNS(ret) ParseTree *text_template_copy(TextTemplate t, Alloc *a);
BURROW_OWNS(ret) Str text_template_error_context(TextTemplate t, Alloc *a, ParseNode n,
                                                 Str *context);

/* ExecError, the error Execute returns when the template goes wrong, as
 * opposed to the writer. name is the template's and err is the error, whose
 * text is the whole message. errors_as with TYPE_TEXT_TEMPLATE_EXEC_ERROR gets
 * you the struct, and errors_is sees through to err. */
typedef struct TextTemplateExecError {
    Str name;
    Error err;
} TextTemplateExecError;

extern const Type *const TYPE_TEXT_TEMPLATE_EXEC_ERROR;

/* ExecError's Error and Unwrap: err's text, and err. */
BURROW_BORROWS(ret, e) Str text_template_exec_error_error(TextTemplateExecError e);
BURROW_BORROWS(ret, e) Error text_template_exec_error_unwrap(TextTemplateExecError e);

/* An Error for an ExecError you filled in yourself. name is copied into a, and
 * err is kept as it is. On an allocation failure you get
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error
text_template_exec_error_as_error(Alloc *a, const TextTemplateExecError *e);

/* ------------------------------------------------------------- the escapers
 *
 * What the html, js and urlquery functions do, for use outside a template. The
 * Writer forms write b escaped to w. The String forms return s itself when
 * there is nothing to escape and an escaped copy made in a when there is. The
 * escapers take a Slice of Any, the arguments the template function was given,
 * and escape what fmt_sprint makes of them, or the one string when there is
 * one. */
void text_template_html_escape(IoWriter w, Slice b);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str text_template_html_escape_string(Alloc *a,
                                                                             Str s);
BURROW_OWNS(ret) Str text_template_html_escaper(Alloc *a, Slice args);

void text_template_js_escape(IoWriter w, Slice b);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str text_template_js_escape_string(Alloc *a,
                                                                           Str s);
BURROW_OWNS(ret) Str text_template_js_escaper(Alloc *a, Slice args);

BURROW_OWNS(ret) Str text_template_url_query_escaper(Alloc *a, Slice args);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TEXT_TEMPLATE_H */
