/* html/template: templates, which are text/template templates that escape
 * themselves the first time they run, and the helpers that parse files.
 *
 * Go's Template embeds a *nameSpace, which the templates associated with each
 * other share. Here it is behind the ns pointer, made in the allocator the first
 * template was made in, along with the templates and what escaping makes.
 *
 * Derived from Go's src/html/template/template.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/text/template.h"
#include "burrow/text/template/parse.h"
#include "burrow/type.h"
#include "template_internal.h"

BURROW_SENTINEL_ERROR(burrow__ht_escape_ok, "template escaped correctly");

static HtmlTemplate *htt_get(burrow__HtNameSpace *ns, Str name) {
    HtmlTemplate *const *p = map_get(ns->set, &name);
    return p == NULL ? NULL : *p;
}

static void htt_put(burrow__HtNameSpace *ns, Str name, HtmlTemplate *t) {
    if (!map_set(ns->set, &name, &t))
        burrow__ht_oom();
}

static HtmlTemplate *htt_make(Alloc *a, TextTemplate *text, ParseTree *tree,
                              burrow__HtNameSpace *ns) {
    HtmlTemplate *t = BURROW_NEW(a, HtmlTemplate);
    if (t == NULL)
        burrow__ht_oom();
    *t = (HtmlTemplate){BURROW_NO_ERROR, text, tree, ns};
    return t;
}

static burrow__HtNameSpace *htt_new_name_space(Alloc *a) {
    burrow__HtNameSpace *ns = BURROW_NEW(a, burrow__HtNameSpace);
    if (ns == NULL)
        return NULL;
    ns->a = a;
    ns->set = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (ns->set == NULL)
        return NULL;
    burrow__ht_make_escaper(&ns->esc, ns);
    return ns;
}

HtmlTemplate *html_template_new(Alloc *a, Str name) {
    burrow__HtNameSpace *ns = htt_new_name_space(a);
    if (ns == NULL)
        return NULL;
    TextTemplate *text = text_template_new(a, name);
    HtmlTemplate *t = BURROW_NEW(a, HtmlTemplate);
    if (text == NULL || t == NULL)
        return NULL;
    *t = (HtmlTemplate){BURROW_NO_ERROR, text, NULL, ns};
    htt_put(ns, text_template_name(text), t);
    return t;
}

/* new: Template.New without the lock. */
static HtmlTemplate *htt_new(HtmlTemplate *t, Str name) {
    burrow__HtNameSpace *ns = t->ns;
    TextTemplate *text = text_template_new_associated(t->text, name);
    HtmlTemplate *tmpl = htt_make(ns->a, text, NULL, ns);
    HtmlTemplate *existing = htt_get(ns, text_template_name(text));
    if (existing != NULL) {
        HtmlTemplate *empty =
            html_template_new(ns->a, text_template_name(existing->text));
        if (empty == NULL)
            burrow__ht_oom();
        *existing = *empty;
    }
    htt_put(ns, text_template_name(text), tmpl);
    return tmpl;
}

HtmlTemplate *html_template_new_associated(HtmlTemplate *t, Str name) {
    /* htt_new resets t when t is the template called name, name space and
     * all, so the lock to let go of is the one taken here. */
    burrow__HtNameSpace *ns = t->ns;
    sync_mutex_lock(&ns->mu);
    HtmlTemplate *r = htt_new(t, name);
    sync_mutex_unlock(&ns->mu);
    return r;
}

Str html_template_name(const HtmlTemplate *t) {
    return text_template_name(t->text);
}

Slice html_template_templates(HtmlTemplate *t, Alloc *a) {
    burrow__HtNameSpace *ns = t->ns;
    sync_mutex_lock(&ns->mu);
    Int n = map_len(ns->set);
    Slice m = slice_make(a, TYPE_UNSAFE_POINTER, 0, n);
    MapIter it = map_iter(ns->set);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v) && m.cap > m.len)
        ((HtmlTemplate **)m.p)[m.len++] = *(HtmlTemplate **)v;
    sync_mutex_unlock(&ns->mu);
    return m;
}

HtmlTemplate *html_template_option(HtmlTemplate *t, Slice opt) {
    text_template_option(t->text, opt);
    return t;
}

/* checkCanParse: an error once the name space has been run. A NULL t can
 * parse. */
static Error htt_check_can_parse(HtmlTemplate *t) {
    if (t == NULL)
        return BURROW_NO_ERROR;
    sync_mutex_lock(&t->ns->mu);
    bool escaped = t->ns->escaped;
    sync_mutex_unlock(&t->ns->mu);
    if (escaped)
        return fmt_errorf_v("html/template: cannot Parse after Execute");
    return BURROW_NO_ERROR;
}

static ParseNode htt_root(const TextTemplate *text) {
    ParseListNode *root = text->tree->root;
    return root == NULL ? NULL : &root->node_type;
}

/* escape: escapes t, the first time. */
static Error htt_escape(HtmlTemplate *t) {
    burrow__HtNameSpace *ns = t->ns;
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&ns->mu);
    ns->escaped = true;
    if (BURROW_OK(t->escape_err)) {
        if (t->tree == NULL)
            err = fmt_errorf_v("template: %q is an incomplete or empty template",
                               html_template_name(t));
        else
            err =
                burrow__ht_escape_template(t, htt_root(t->text), html_template_name(t));
    } else if (!burrow__ht_is_escape_ok(t->escape_err)) {
        err = t->escape_err;
    }
    sync_mutex_unlock(&ns->mu);
    return err;
}

Error html_template_execute(HtmlTemplate *t, IoWriter wr, Any data) {
    Error err = htt_escape(t);
    if (BURROW_FAILED(err))
        return err;
    return text_template_execute(t->text, wr, data);
}

/* lookupAndEscapeTemplate: the template called name, escaped. */
static HtmlTemplate *htt_lookup_and_escape(HtmlTemplate *t, Str name, Error *err) {
    burrow__HtNameSpace *ns = t->ns;
    sync_mutex_lock(&ns->mu);
    ns->escaped = true;
    HtmlTemplate *tmpl = htt_get(ns, name);
    if (tmpl == NULL) {
        *err = fmt_errorf_v("html/template: %q is undefined", name);
    } else if (BURROW_FAILED(tmpl->escape_err) &&
               !burrow__ht_is_escape_ok(tmpl->escape_err)) {
        *err = tmpl->escape_err;
        tmpl = NULL;
    } else if (tmpl->text->tree == NULL || tmpl->text->tree->root == NULL) {
        *err = fmt_errorf_v("html/template: %q is an incomplete template", name);
        tmpl = NULL;
    } else {
        if (text_template_lookup(t->text, name) == NULL) {
            sync_mutex_unlock(&ns->mu);
            panic_str(BURROW_S("html/template internal error: template escaping out of "
                               "sync"));
        }
        if (BURROW_OK(tmpl->escape_err))
            *err = burrow__ht_escape_template(tmpl, htt_root(tmpl->text), name);
    }
    sync_mutex_unlock(&ns->mu);
    return tmpl;
}

Error html_template_execute_template(HtmlTemplate *t, IoWriter wr, Str name, Any data) {
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = htt_lookup_and_escape(t, name, &err);
    if (BURROW_FAILED(err))
        return err;
    return text_template_execute(tmpl->text, wr, data);
}

Str html_template_defined_templates(HtmlTemplate *t, Alloc *a) {
    return text_template_defined_templates(t->text, a);
}

HtmlTemplate *html_template_parse(HtmlTemplate *t, Str text, Error *err) {
    Error e = htt_check_can_parse(t);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    TextTemplate *ret = text_template_parse(t->text, text, err);
    if (ret == NULL)
        return NULL;

    burrow__HtNameSpace *ns = t->ns;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    sync_mutex_lock(&ns->mu);
    Slice all = text_template_templates(ret, arena_allocator(&ar));
    for (Int i = 0; i < all.len; i++) {
        /* In Parse, we get the template from text. We need the HTML template
         * for each of them. */
        TextTemplate *v = BURROW_AT(TextTemplate *, all, i);
        Str name = text_template_name(v);
        HtmlTemplate *tmpl = htt_get(ns, name);
        if (tmpl == NULL)
            tmpl = htt_new(t, name);
        tmpl->text = v;
        tmpl->tree = v->tree;
    }
    sync_mutex_unlock(&ns->mu);
    arena_free(&ar);
    return t;
}

HtmlTemplate *html_template_add_parse_tree(HtmlTemplate *t, Str name, ParseTree *tree,
                                           Error *err) {
    Error e = htt_check_can_parse(t);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    burrow__HtNameSpace *ns = t->ns;
    sync_mutex_lock(&ns->mu);
    HtmlTemplate *ret = NULL;
    TextTemplate *text = text_template_add_parse_tree(t->text, name, tree, err);
    if (text != NULL) {
        ret = htt_make(ns->a, text, text->tree, ns);
        htt_put(ns, text_template_name(text), ret);
    }
    sync_mutex_unlock(&ns->mu);
    return ret;
}

HtmlTemplate *html_template_clone(HtmlTemplate *t, Alloc *a, Error *err) {
    burrow__HtNameSpace *src_ns = t->ns;
    HtmlTemplate *r = NULL;
    sync_mutex_lock(&src_ns->mu);
    if (BURROW_FAILED(t->escape_err)) {
        *err = fmt_errorf_v("html/template: cannot Clone %q after it has executed",
                            html_template_name(t));
        goto out;
    }
    TextTemplate *text_clone = text_template_clone(t->text, a, err);
    if (text_clone == NULL)
        goto out;
    burrow__HtNameSpace *ns = htt_new_name_space(a);
    if (ns == NULL)
        burrow__ht_oom();
    HtmlTemplate *ret = htt_make(a, text_clone, text_clone->tree, ns);
    htt_put(ns, html_template_name(ret), ret);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice all = text_template_templates(text_clone, arena_allocator(&ar));
    Int i = 0;
    for (; i < all.len; i++) {
        TextTemplate *x = BURROW_AT(TextTemplate *, all, i);
        Str name = text_template_name(x);
        HtmlTemplate *src = htt_get(src_ns, name);
        if (src == NULL || BURROW_FAILED(src->escape_err)) {
            *err = fmt_errorf_v("html/template: cannot Clone %q after it has executed",
                                html_template_name(t));
            break;
        }
        if (x->tree != NULL) {
            x->tree = parse_tree_copy(x->tree, a);
            if (x->tree == NULL)
                burrow__ht_oom();
        }
        htt_put(ns, name, htt_make(a, x, x->tree, ns));
    }
    arena_free(&ar);
    if (i == all.len)
        r = htt_get(ns, html_template_name(ret));
out:
    sync_mutex_unlock(&src_ns->mu);
    return r;
}

HtmlTemplate *html_template_funcs(HtmlTemplate *t, HtmlTemplateFuncMap funcs) {
    text_template_funcs(t->text, funcs);
    return t;
}

HtmlTemplate *html_template_delims(HtmlTemplate *t, Str left, Str right) {
    text_template_delims(t->text, left, right);
    return t;
}

HtmlTemplate *html_template_lookup(HtmlTemplate *t, Str name) {
    sync_mutex_lock(&t->ns->mu);
    HtmlTemplate *r = htt_get(t->ns, name);
    sync_mutex_unlock(&t->ns->mu);
    return r;
}

HtmlTemplate *html_template_must(HtmlTemplate *t, Error err) {
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return t;
}

/* ------------------------------------------------------------------ files */

/* readFileOS and readFileFS: the template name for a file and its text, made
 * in a. fsys is NULL for the OS. */
static Slice htt_read_file(Alloc *a, const Fs *fsys, Str file, Str *name, Error *err) {
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
static HtmlTemplate *htt_parse_files(HtmlTemplate *t, Alloc *a, const Fs *fsys,
                                     Slice filenames, Error *err) {
    Error e = htt_check_can_parse(t);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    if (filenames.len == 0) {
        *err = fmt_errorf_v("html/template: no files named in call to ParseFiles");
        return NULL;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    HtmlTemplate *ret = t;
    for (Int i = 0; i < filenames.len; i++) {
        Str name = BURROW_STR_EMPTY;
        Slice b = htt_read_file(ta, fsys, BURROW_AT(Str, filenames, i), &name, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            ret = NULL;
            break;
        }
        if (ret == NULL) {
            ret = html_template_new(a, name);
            if (ret == NULL) {
                *err = burrow_err_out_of_memory;
                break;
            }
        }
        HtmlTemplate *tmpl = str_eq(name, html_template_name(ret))
                                 ? ret
                                 : html_template_new_associated(ret, name);
        if (html_template_parse(tmpl, str_from_bytes(b.p, b.len), err) == NULL) {
            ret = NULL;
            break;
        }
    }
    arena_free(&ar);
    return ret;
}

HtmlTemplate *html_template_parse_files(HtmlTemplate *t, Slice filenames, Error *err) {
    return htt_parse_files(t, t->ns->a, NULL, filenames, err);
}

HtmlTemplate *html_template_from_files(Alloc *a, Slice filenames, Error *err) {
    return htt_parse_files(NULL, a, NULL, filenames, err);
}

static HtmlTemplate *htt_parse_glob(HtmlTemplate *t, Alloc *a, Str pattern,
                                    Error *err) {
    Error e = htt_check_can_parse(t);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HtmlTemplate *ret = NULL;
    Slice filenames = filepath_glob(arena_allocator(&ar), pattern, &e);
    if (BURROW_FAILED(e))
        *err = e;
    else if (filenames.len == 0)
        *err = fmt_errorf_v("html/template: pattern matches no files: %#q", pattern);
    else
        ret = htt_parse_files(t, a, NULL, filenames, err);
    arena_free(&ar);
    return ret;
}

HtmlTemplate *html_template_parse_glob(HtmlTemplate *t, Str pattern, Error *err) {
    return htt_parse_glob(t, t->ns->a, pattern, err);
}

HtmlTemplate *html_template_from_glob(Alloc *a, Str pattern, Error *err) {
    return htt_parse_glob(NULL, a, pattern, err);
}

static HtmlTemplate *htt_parse_fs(HtmlTemplate *t, Alloc *a, Fs fsys, Slice patterns,
                                  Error *err) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    HtmlTemplate *ret = NULL;
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
        ret = htt_parse_files(t, a, &fsys, filenames, err);
    arena_free(&ar);
    return ret;
}

HtmlTemplate *html_template_parse_fs(HtmlTemplate *t, Fs fsys, Slice patterns,
                                     Error *err) {
    return htt_parse_fs(t, t->ns->a, fsys, patterns, err);
}

HtmlTemplate *html_template_from_fs(Alloc *a, Fs fsys, Slice patterns, Error *err) {
    return htt_parse_fs(NULL, a, fsys, patterns, err);
}
