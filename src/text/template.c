/* text/template: templates, the set of templates associated with one, their
 * functions and options, and the helpers that parse files.
 *
 * Go's Template embeds a *parse.Tree and a *common. Here the tree is the tree
 * field and the common part is behind the common pointer, made in the
 * template's allocator the first time it is needed, as Go's init does.
 *
 * Derived from Go's src/text/template/template.go, helper.go, option.go and
 * exec.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/template.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/reflect.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "template_internal.h"

#include <string.h>

/* What parse_funcs holds for a builtin. The parser only asks whether a name is
 * there. */
static const bool tt_builtin_mark = true;

static const char *const tt_builtin_names[] = {
    "and", "call", "html",  "index",  "slice",   "js",       "len",
    "not", "or",   "print", "printf", "println", "urlquery", "eq",
    "ge",  "gt",   "le",    "lt",     "ne",
};

static bool tt_init_common(TextTemplate *t) {
    if (t->common != NULL)
        return true;
    burrow__TextTemplateCommon *c = BURROW_NEW(t->a, burrow__TextTemplateCommon);
    if (c == NULL)
        return false;
    c->tmpl = map_make(t->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    c->parse_funcs = map_make(t->a, TYPE_STRING, TYPE_ANY, 0);
    c->exec_funcs = map_make(t->a, TYPE_STRING, TYPE_REFLECT_VALUE, 0);
    if (c->tmpl == NULL || c->parse_funcs == NULL || c->exec_funcs == NULL)
        return false;
    Any mark = BURROW_ANY(TYPE_BOOL, (uintptr_t)&tt_builtin_mark);
    for (size_t i = 0; i < sizeof tt_builtin_names / sizeof tt_builtin_names[0]; i++) {
        Str name = str_from_cstr(tt_builtin_names[i]);
        if (!map_set(c->parse_funcs, &name, &mark))
            return false;
    }
    t->common = c;
    return true;
}

/* init: makes the common part when t does not have one yet. */
static void tt_init(TextTemplate *t) {
    if (!tt_init_common(t))
        burrow__tt_oom();
}

static Str tt_clone_str(Alloc *a, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Str c = str_clone(a, s);
    if (c.p == NULL)
        burrow__tt_oom();
    return c;
}

TextTemplate *text_template_new(Alloc *a, Str name) {
    TextTemplate *t = BURROW_NEW(a, TextTemplate);
    if (t == NULL)
        return NULL;
    t->a = a;
    if (name.len > 0) {
        t->name = str_clone(a, name);
        if (t->name.p == NULL)
            return NULL;
    }
    if (!tt_init_common(t))
        return NULL;
    return t;
}

Str text_template_name(const TextTemplate *t) {
    return t->name;
}

TextTemplate *text_template_new_associated(TextTemplate *t, Str name) {
    tt_init(t);
    TextTemplate *nt = BURROW_NEW(t->a, TextTemplate);
    if (nt == NULL)
        burrow__tt_oom();
    nt->name = tt_clone_str(t->a, name);
    nt->common = t->common;
    nt->left_delim = t->left_delim;
    nt->right_delim = t->right_delim;
    nt->a = t->a;
    return nt;
}

/* copy: a shallow copy of t in a, with c as its common part. */
static TextTemplate *tt_copy(const TextTemplate *t, Alloc *a,
                             burrow__TextTemplateCommon *c) {
    TextTemplate *nt = BURROW_NEW(a, TextTemplate);
    if (nt == NULL)
        burrow__tt_oom();
    *nt = *t;
    nt->common = c;
    nt->a = a;
    return nt;
}

static TextTemplate *tt_get(Map *m, Str name) {
    TextTemplate *const *p = map_get(m, &name);
    return p == NULL ? NULL : *p;
}

static void tt_put(Map *m, Str name, TextTemplate *t) {
    if (!map_set(m, &name, &t))
        burrow__tt_oom();
}

TextTemplate *text_template_clone(TextTemplate *t, Alloc *a, Error *err) {
    (void)err;
    TextTemplate *nt = tt_copy(t, a, NULL);
    tt_init(nt);
    if (t->common == NULL)
        return nt;
    burrow__TextTemplateCommon *c = t->common;
    nt->common->missing_key = c->missing_key;
    sync_rw_mutex_r_lock(&c->mu_tmpl);
    MapIter it = map_iter(c->tmpl);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str name = *(const Str *)k;
        if (str_eq(name, t->name)) {
            tt_put(nt->common->tmpl, t->name, nt);
            continue;
        }
        tt_put(nt->common->tmpl, name, tt_copy(*(TextTemplate **)v, a, nt->common));
    }
    sync_rw_mutex_r_unlock(&c->mu_tmpl);
    sync_rw_mutex_r_lock(&c->mu_funcs);
    Map *pf = map_clone(a, c->parse_funcs);
    Map *ef = map_clone(a, c->exec_funcs);
    sync_rw_mutex_r_unlock(&c->mu_funcs);
    if (pf == NULL || ef == NULL)
        burrow__tt_oom();
    nt->common->parse_funcs = pf;
    nt->common->exec_funcs = ef;
    return nt;
}

/* associate: installs new into the set of templates. A tree that is empty, only
 * spaces and comments, does not replace a template that has one. Reports
 * whether new went in. */
static bool tt_associate(TextTemplate *t, TextTemplate *new, ParseTree *tree) {
    if (new->common != t->common)
        panic_str(BURROW_S("internal error: associate not common"));
    TextTemplate *old = tt_get(t->common->tmpl, new->name);
    ParseNode root = tree->root == NULL ? NULL : &tree->root->node_type;
    if (old != NULL && parse_is_empty_tree(root) && old->tree != NULL)
        return false;
    tt_put(t->common->tmpl, new->name, new);
    return true;
}

TextTemplate *text_template_add_parse_tree(TextTemplate *t, Str name, ParseTree *tree,
                                           Error *err) {
    (void)err;
    tt_init(t);
    sync_rw_mutex_lock(&t->common->mu_tmpl);
    TextTemplate *nt = t;
    if (!str_eq(name, t->name))
        nt = text_template_new_associated(t, name);
    /* Even if nt == t, we need to install it in the common.tmpl map. */
    if (tt_associate(t, nt, tree) || nt->tree == NULL)
        nt->tree = tree;
    sync_rw_mutex_unlock(&t->common->mu_tmpl);
    return nt;
}

Slice text_template_templates(TextTemplate *t, Alloc *a) {
    if (t->common == NULL)
        return slice_nil(TYPE_UNSAFE_POINTER);
    sync_rw_mutex_r_lock(&t->common->mu_tmpl);
    Int n = map_len(t->common->tmpl);
    Slice m = slice_make(a, TYPE_UNSAFE_POINTER, 0, n);
    MapIter it = map_iter(t->common->tmpl);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v) && m.cap > m.len)
        BURROW_AT(TextTemplate *, m, m.len++) = *(TextTemplate **)v;
    sync_rw_mutex_r_unlock(&t->common->mu_tmpl);
    return m;
}

TextTemplate *text_template_delims(TextTemplate *t, Str left, Str right) {
    tt_init(t);
    t->left_delim = left;
    t->right_delim = right;
    return t;
}

/* addValueFuncs' checks, all made before anything is added. */
static void tt_check_funcs(TextTemplateFuncMap funcs) {
    MapIter it = map_iter(funcs);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str name = *(const Str *)k;
        if (!burrow__tt_good_name(name)) {
            Error e = fmt_errorf_v("function name %q is not a valid identifier", name);
            panic(BURROW_ANY(TYPE_ERROR, &e));
        }
        ReflectValue fv = reflect_value_of(*(const Any *)v);
        if (reflect_value_kind(fv) != REFLECT_FUNC)
            panic_str(
                fmt_sprintf_v(error_allocator(), "value for %s not a function", name));
        Error e = burrow__tt_good_func(name, reflect_value_type(fv));
        if (BURROW_FAILED(e))
            panic(BURROW_ANY(TYPE_ERROR, &e));
    }
}

TextTemplate *text_template_funcs(TextTemplate *t, TextTemplateFuncMap funcs) {
    tt_init(t);
    tt_check_funcs(funcs);
    sync_rw_mutex_lock(&t->common->mu_funcs);
    MapIter it = map_iter(funcs);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str name = tt_clone_str(t->a, *(const Str *)k);
        ReflectValue fv = reflect_value_of(*(const Any *)v);
        fv = burrow__tt_value_of(t->a, fv.typ, fv.ptr);
        Any fn = BURROW_ANY(fv.typ, fv.ptr);
        if (!map_set(t->common->exec_funcs, &name, &fv) ||
            !map_set(t->common->parse_funcs, &name, &fn))
            burrow__tt_oom();
    }
    sync_rw_mutex_unlock(&t->common->mu_funcs);
    return t;
}

TextTemplate *text_template_lookup(TextTemplate *t, Str name) {
    if (t->common == NULL)
        return NULL;
    sync_rw_mutex_r_lock(&t->common->mu_tmpl);
    TextTemplate *r = tt_get(t->common->tmpl, name);
    sync_rw_mutex_r_unlock(&t->common->mu_tmpl);
    return r;
}

TextTemplate *text_template_parse(TextTemplate *t, Str text, Error *err) {
    tt_init(t);
    Error e = BURROW_NO_ERROR;
    sync_rw_mutex_r_lock(&t->common->mu_funcs);
    Map *funcs[1] = {t->common->parse_funcs};
    Map *trees = parse_parse(t->a, t->name, text, t->left_delim, t->right_delim,
                             slice_from(funcs, 1, 1, TYPE_UNSAFE_POINTER), &e);
    sync_rw_mutex_r_unlock(&t->common->mu_funcs);
    if (BURROW_FAILED(e) || trees == NULL) {
        *err = BURROW_FAILED(e) ? e : burrow_err_out_of_memory;
        return NULL;
    }
    MapIter it = map_iter(trees);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v))
        text_template_add_parse_tree(t, *(const Str *)k, *(ParseTree **)v, err);
    return t;
}

Str text_template_defined_templates(TextTemplate *t, Alloc *a) {
    if (t->common == NULL)
        return BURROW_STR_EMPTY;
    StringsBuilder b = STRINGS_BUILDER(a);
    sync_rw_mutex_r_lock(&t->common->mu_tmpl);
    MapIter it = map_iter(t->common->tmpl);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        const TextTemplate *tmpl = *(TextTemplate **)v;
        if (tmpl->tree == NULL || tmpl->tree->root == NULL)
            continue;
        if (strings_builder_len(&b) == 0)
            strings_builder_write_string(&b, BURROW_S("; defined templates are: "),
                                         NULL);
        else
            strings_builder_write_string(&b, BURROW_S(", "), NULL);
        fmt_fprintf_v(strings_builder_as_io_writer(&b), "%q", *(const Str *)k);
    }
    sync_rw_mutex_r_unlock(&t->common->mu_tmpl);
    return strings_builder_string(&b);
}

/* ---------------------------------------------------------------- options */

static void tt_set_option(TextTemplate *t, Str opt) {
    if (opt.len == 0)
        panic_str(BURROW_S("empty option string"));
    Str value = BURROW_STR_EMPTY;
    bool found = false;
    Str key = strings_cut(opt, BURROW_S("="), &value, &found);
    if (found && str_eq(key, BURROW_S("missingkey"))) {
        if (str_eq(value, BURROW_S("invalid")) || str_eq(value, BURROW_S("default"))) {
            t->common->missing_key = BURROW__TT_MAP_INVALID;
            return;
        }
        if (str_eq(value, BURROW_S("zero"))) {
            t->common->missing_key = BURROW__TT_MAP_ZERO;
            return;
        }
        if (str_eq(value, BURROW_S("error"))) {
            t->common->missing_key = BURROW__TT_MAP_ERROR;
            return;
        }
    }
    panic_str(fmt_sprintf_v(error_allocator(), "unrecognized option: %s", opt));
}

TextTemplate *text_template_option(TextTemplate *t, Slice opt) {
    tt_init(t);
    for (Int i = 0; i < opt.len; i++)
        tt_set_option(t, BURROW_AT(Str, opt, i));
    return t;
}

bool text_template_is_true(Any val, bool *ok) {
    bool good = false;
    bool truth = burrow__tt_is_true(reflect_value_of(val), &good);
    if (ok != NULL)
        *ok = good;
    return truth;
}

TextTemplate *text_template_must(TextTemplate *t, Error err) {
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return t;
}

/* ------------------------------------------------------------------ files */

/* readFileOS and readFileFS: the template name for a file and its text, made
 * in a. fsys is NULL for the OS. */
static Slice tt_read_file(Alloc *a, const Fs *fsys, Str file, Str *name, Error *err) {
    if (fsys == NULL) {
        *name = filepath_base(file);
        return os_read_file(a, file, err);
    }
    *name = path_base(file);
    return fs_read_file(a, *fsys, file, err);
}

/* parseFiles. t is NULL for the package level form, which makes the templates
 * in a. The files are read into an arena of their own, since the parser keeps a
 * copy of the text. */
static TextTemplate *tt_parse_files(TextTemplate *t, Alloc *a, const Fs *fsys,
                                    Slice filenames, Error *err) {
    if (filenames.len == 0) {
        *err = fmt_errorf_v("template: no files named in call to ParseFiles");
        return NULL;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    TextTemplate *ret = t;
    for (Int i = 0; i < filenames.len; i++) {
        Str name = BURROW_STR_EMPTY;
        Error e = BURROW_NO_ERROR;
        Slice b = tt_read_file(ta, fsys, BURROW_AT(Str, filenames, i), &name, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            ret = NULL;
            break;
        }
        if (ret == NULL) {
            ret = text_template_new(a, name);
            if (ret == NULL) {
                *err = burrow_err_out_of_memory;
                break;
            }
        }
        TextTemplate *tmpl =
            str_eq(name, ret->name) ? ret : text_template_new_associated(ret, name);
        if (text_template_parse(tmpl, str_from_bytes(b.p, b.len), err) == NULL) {
            ret = NULL;
            break;
        }
    }
    arena_free(&ar);
    return ret;
}

TextTemplate *text_template_parse_files(TextTemplate *t, Slice filenames, Error *err) {
    tt_init(t);
    return tt_parse_files(t, t->a, NULL, filenames, err);
}

TextTemplate *text_template_from_files(Alloc *a, Slice filenames, Error *err) {
    return tt_parse_files(NULL, a, NULL, filenames, err);
}

static TextTemplate *tt_parse_glob(TextTemplate *t, Alloc *a, Str pattern, Error *err) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    TextTemplate *ret = NULL;
    Error e = BURROW_NO_ERROR;
    Slice filenames = filepath_glob(arena_allocator(&ar), pattern, &e);
    if (BURROW_FAILED(e))
        *err = e;
    else if (filenames.len == 0)
        *err = fmt_errorf_v("template: pattern matches no files: %#q", pattern);
    else
        ret = tt_parse_files(t, a, NULL, filenames, err);
    arena_free(&ar);
    return ret;
}

TextTemplate *text_template_parse_glob(TextTemplate *t, Str pattern, Error *err) {
    tt_init(t);
    return tt_parse_glob(t, t->a, pattern, err);
}

TextTemplate *text_template_from_glob(Alloc *a, Str pattern, Error *err) {
    return tt_parse_glob(NULL, a, pattern, err);
}

static TextTemplate *tt_parse_fs(TextTemplate *t, Alloc *a, Fs fsys, Slice patterns,
                                 Error *err) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    TextTemplate *ret = NULL;
    Slice filenames = slice_nil(TYPE_STRING);
    Int i = 0;
    for (; i < patterns.len; i++) {
        Str pattern = BURROW_AT(Str, patterns, i);
        Error e = BURROW_NO_ERROR;
        Slice list = fs_glob(ta, fsys, pattern, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            break;
        }
        if (list.len == 0) {
            *err = fmt_errorf_v("template: pattern matches no files: %#q", pattern);
            break;
        }
        filenames = slice_append_slice(ta, filenames, list);
    }
    if (i == patterns.len)
        ret = tt_parse_files(t, a, &fsys, filenames, err);
    arena_free(&ar);
    return ret;
}

TextTemplate *text_template_parse_fs(TextTemplate *t, Fs fsys, Slice patterns,
                                     Error *err) {
    tt_init(t);
    return tt_parse_fs(t, t->a, fsys, patterns, err);
}

TextTemplate *text_template_from_fs(Alloc *a, Fs fsys, Slice patterns, Error *err) {
    return tt_parse_fs(NULL, a, fsys, patterns, err);
}

/* ------------------------------------------------------------- the tree */

ParseTree *text_template_copy(TextTemplate t, Alloc *a) {
    return parse_tree_copy(t.tree, a);
}

Str text_template_error_context(TextTemplate t, Alloc *a, ParseNode n, Str *context) {
    return parse_tree_error_context(t.tree, a, n, context);
}

/* -------------------------------------------------------------- ExecError */

/* The public struct is the whole box: the message is err's text, which needs
 * nothing made for it. */
static Str tt_exec_error_message(const void *self) {
    return error_text(((const TextTemplateExecError *)self)->err);
}

static Error tt_exec_error_unwrap(const void *self) {
    return ((const TextTemplateExecError *)self)->err;
}

static const Type tt_exec_error_desc = {
    BURROW_S_INIT("ExecError"),
    BURROW_S_INIT("text/template"),
    KIND_STRUCT,
    (uint32_t)sizeof(TextTemplateExecError),
    (uint16_t)_Alignof(TextTemplateExecError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74746578U, /* "ttex" */
    NULL,
};

const Type *const TYPE_TEXT_TEMPLATE_EXEC_ERROR = &tt_exec_error_desc;

static Error tt_exec_error_clone(const void *self, Alloc *a);

static const ErrorVT tt_exec_error_vt = {
    &tt_exec_error_desc, tt_exec_error_message, tt_exec_error_unwrap, NULL, NULL, NULL,
    tt_exec_error_clone,
};

Error text_template_exec_error_as_error(Alloc *a, const TextTemplateExecError *e) {
    size_t size = sizeof(TextTemplateExecError) + (size_t)e->name.len;
    TextTemplateExecError *b = (TextTemplateExecError *)mem_alloc_nozero(
        a, size, _Alignof(TextTemplateExecError));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (e->name.len > 0)
        memcpy(p, e->name.p, (size_t)e->name.len);
    b->name = str_from_bytes(p, e->name.len);
    b->err = e->err;
    return (Error){&tt_exec_error_vt, b};
}

/* What error_retain calls. The wrapped error is retained first, since it lives
 * in the error arena too. */
static Error tt_exec_error_clone(const void *self, Alloc *a) {
    TextTemplateExecError copy = *(const TextTemplateExecError *)self;
    copy.err = error_retain(a, copy.err);
    return text_template_exec_error_as_error(a, &copy);
}

Str text_template_exec_error_error(TextTemplateExecError e) {
    return error_text(e.err);
}

Error text_template_exec_error_unwrap(TextTemplateExecError e) {
    return e.err;
}
