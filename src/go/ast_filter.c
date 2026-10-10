/* go/ast: the filters, MergePackageFiles and SortImports.
 *
 * The filters cut a tree down in place, the way Go's do, so a list that loses
 * elements keeps its backing array and only gets shorter. SortImports works in
 * place too, and the scratch it needs while sorting comes from an arena it
 * throws away before returning.
 *
 * Derived from Go's src/go/ast/filter.go and import.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Every list in a tree is a list of pointers. */
static AstNode af_at(Slice s, Int i) {
    return ((AstNode *)s.p)[i];
}

static void af_set(Slice s, Int i, AstNode n) {
    ((AstNode *)s.p)[i] = n;
}

static Slice af_head(Slice s, Int n) {
    s.len = n;
    return s;
}

BURROW_NORETURN static void af_oom(void) {
    panic_str(BURROW_S("go/ast: out of memory"));
}

/* ----------------------------------------------------------------- filters */

static bool af_export_name(void *env, Str name) {
    (void)env;
    return ast_is_exported(name);
}

static const AstFilter af_export = {af_export_name, NULL};

static bool af_keep(AstFilter f, Str name) {
    return f.f(f.env, name);
}

static Slice af_filter_ident_list(Slice list, AstFilter f) {
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstIdent *x = (AstIdent *)af_at(list, i);
        if (af_keep(f, x->name)) {
            af_set(list, j, &x->node);
            j++;
        }
    }
    return af_head(list, j);
}

/* The name an embedded field goes by, or NULL. */
static AstIdent *af_field_name(AstExpr x) {
    if (x == NULL) {
        return NULL;
    }
    switch (x->kind) {
    case AST_KIND_IDENT:
        return (AstIdent *)x;
    case AST_KIND_SELECTOR_EXPR: {
        AstSelectorExpr *t = (AstSelectorExpr *)x;
        if (t->x != NULL && t->x->kind == AST_KIND_IDENT) {
            return t->sel;
        }
        return NULL;
    }
    case AST_KIND_STAR_EXPR:
        return af_field_name(((AstStarExpr *)x)->x);
    default:
        return NULL;
    }
}

static bool af_filter_type(AstExpr typ, AstFilter f, bool export);

static bool af_filter_field_list(AstFieldList *fields, AstFilter filter, bool export) {
    if (fields == NULL) {
        return false;
    }
    bool removed = false;
    Slice list = fields->list;
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstField *f = (AstField *)af_at(list, i);
        bool keep = false;
        if (f->names.len == 0) {
            /* An embedded field, kept by its type's name. */
            AstIdent *name = af_field_name(f->type);
            keep = name != NULL && af_keep(filter, name->name);
        } else {
            Int n = f->names.len;
            f->names = af_filter_ident_list(f->names, filter);
            if (f->names.len < n) {
                removed = true;
            }
            keep = f->names.len > 0;
        }
        if (keep) {
            if (export) {
                af_filter_type(f->type, filter, export);
            }
            af_set(list, j, &f->node);
            j++;
        }
    }
    if (j < list.len) {
        removed = true;
    }
    fields->list = af_head(list, j);
    return removed;
}

static Slice af_filter_expr_list(Slice list, AstFilter filter, bool export);

static void af_filter_composite_lit(AstCompositeLit *lit, AstFilter filter,
                                    bool export) {
    Int n = lit->elts.len;
    lit->elts = af_filter_expr_list(lit->elts, filter, export);
    if (lit->elts.len < n) {
        lit->incomplete = true;
    }
}

static Slice af_filter_expr_list(Slice list, AstFilter filter, bool export) {
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstExpr exp = af_at(list, i);
        if (exp != NULL && exp->kind == AST_KIND_COMPOSITE_LIT) {
            af_filter_composite_lit((AstCompositeLit *)exp, filter, export);
        } else if (exp != NULL && exp->kind == AST_KIND_KEY_VALUE_EXPR) {
            AstKeyValueExpr *x = (AstKeyValueExpr *)exp;
            if (x->key != NULL && x->key->kind == AST_KIND_IDENT &&
                !af_keep(filter, ((AstIdent *)x->key)->name)) {
                continue;
            }
            if (x->value != NULL && x->value->kind == AST_KIND_COMPOSITE_LIT) {
                af_filter_composite_lit((AstCompositeLit *)x->value, filter, export);
            }
        }
        af_set(list, j, exp);
        j++;
    }
    return af_head(list, j);
}

static bool af_filter_param_list(AstFieldList *fields, AstFilter filter, bool export) {
    if (fields == NULL) {
        return false;
    }
    bool b = false;
    for (Int i = 0; i < fields->list.len; i++) {
        AstField *f = (AstField *)af_at(fields->list, i);
        if (af_filter_type(f->type, filter, export)) {
            b = true;
        }
    }
    return b;
}

static bool af_filter_type(AstExpr typ, AstFilter f, bool export) {
    if (typ == NULL) {
        return false;
    }
    switch (typ->kind) {
    case AST_KIND_IDENT:
        return af_keep(f, ((AstIdent *)typ)->name);
    case AST_KIND_PAREN_EXPR:
        return af_filter_type(((AstParenExpr *)typ)->x, f, export);
    case AST_KIND_ARRAY_TYPE:
        return af_filter_type(((AstArrayType *)typ)->elt, f, export);
    case AST_KIND_STRUCT_TYPE: {
        AstStructType *t = (AstStructType *)typ;
        if (af_filter_field_list(t->fields, f, export)) {
            t->incomplete = true;
        }
        return t->fields != NULL && t->fields->list.len > 0;
    }
    case AST_KIND_FUNC_TYPE: {
        AstFuncType *t = (AstFuncType *)typ;
        bool b1 = af_filter_param_list(t->params, f, export);
        bool b2 = af_filter_param_list(t->results, f, export);
        return b1 || b2;
    }
    case AST_KIND_INTERFACE_TYPE: {
        AstInterfaceType *t = (AstInterfaceType *)typ;
        if (af_filter_field_list(t->methods, f, export)) {
            t->incomplete = true;
        }
        return t->methods != NULL && t->methods->list.len > 0;
    }
    case AST_KIND_MAP_TYPE: {
        AstMapType *t = (AstMapType *)typ;
        bool b1 = af_filter_type(t->key, f, export);
        bool b2 = af_filter_type(t->value, f, export);
        return b1 || b2;
    }
    case AST_KIND_CHAN_TYPE:
        return af_filter_type(((AstChanType *)typ)->value, f, export);
    default:
        return false;
    }
}

static bool af_filter_spec(AstSpec spec, AstFilter f, bool export) {
    if (spec == NULL) {
        return false;
    }
    switch (spec->kind) {
    case AST_KIND_VALUE_SPEC: {
        AstValueSpec *s = (AstValueSpec *)spec;
        s->names = af_filter_ident_list(s->names, f);
        s->values = af_filter_expr_list(s->values, f, export);
        if (s->names.len > 0) {
            if (export) {
                af_filter_type(s->type, f, export);
            }
            return true;
        }
        return false;
    }
    case AST_KIND_TYPE_SPEC: {
        AstTypeSpec *s = (AstTypeSpec *)spec;
        if (af_keep(f, s->name->name)) {
            if (export) {
                /* Only the exported fields and methods stay, and an
                 * unexported type keeps none of them. */
                af_filter_type(s->type, f, export);
            }
            return true;
        }
        if (!export) {
            /* For general filtering, a struct or interface type with a field
             * or method f keeps counts too. */
            return af_filter_type(s->type, f, export);
        }
        return false;
    }
    default:
        return false;
    }
}

static Slice af_filter_spec_list(Slice list, AstFilter f, bool export) {
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstSpec s = af_at(list, i);
        if (af_filter_spec(s, f, export)) {
            af_set(list, j, s);
            j++;
        }
    }
    return af_head(list, j);
}

static bool af_filter_decl(AstDecl decl, AstFilter f, bool export) {
    if (decl == NULL) {
        return false;
    }
    switch (decl->kind) {
    case AST_KIND_GEN_DECL: {
        AstGenDecl *d = (AstGenDecl *)decl;
        d->specs = af_filter_spec_list(d->specs, f, export);
        return d->specs.len > 0;
    }
    case AST_KIND_FUNC_DECL:
        return af_keep(f, ((AstFuncDecl *)decl)->name->name);
    default:
        return false;
    }
}

static bool af_filter_file(AstFile *src, AstFilter f, bool export) {
    Int j = 0;
    for (Int i = 0; i < src->decls.len; i++) {
        AstDecl d = af_at(src->decls, i);
        if (af_filter_decl(d, f, export)) {
            af_set(src->decls, j, d);
            j++;
        }
    }
    src->decls = af_head(src->decls, j);
    return j > 0;
}

static bool af_filter_package(AstPackage *pkg, AstFilter f, bool export) {
    bool has_decls = false;
    MapIter it = map_iter(pkg->files);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        if (af_filter_file(*(AstFile **)v, f, export)) {
            has_decls = true;
        }
    }
    return has_decls;
}

bool ast_file_exports(AstFile *src) {
    return af_filter_file(src, af_export, true);
}

bool ast_package_exports(AstPackage *pkg) {
    return af_filter_package(pkg, af_export, true);
}

bool ast_filter_decl(AstDecl decl, AstFilter f) {
    return af_filter_decl(decl, f, false);
}

bool ast_filter_file(AstFile *src, AstFilter f) {
    return af_filter_file(src, f, false);
}

bool ast_filter_package(AstPackage *pkg, AstFilter f) {
    return af_filter_package(pkg, f, false);
}

/* ------------------------------------------------------ merging a package */

/* The name a function goes by for FilterFuncDuplicates, T.m for a method.
 * The result lives in ar. */
static Str af_name_of(Alloc *ar, AstFuncDecl *f) {
    AstFieldList *r = f->recv;
    if (r != NULL && r->list.len == 1) {
        AstExpr t = ((AstField *)af_at(r->list, 0))->type;
        if (t != NULL && t->kind == AST_KIND_STAR_EXPR) {
            t = ((AstStarExpr *)t)->x;
        }
        if (t != NULL && t->kind == AST_KIND_IDENT) {
            Str tn = ((AstIdent *)t)->name;
            Str fn = f->name->name;
            Int n = tn.len + 1 + fn.len;
            Byte *p = (Byte *)mem_alloc(ar, (size_t)n, 1);
            if (p == NULL) {
                af_oom();
            }
            memcpy(p, tn.p, (size_t)tn.len);
            p[tn.len] = '.';
            memcpy(p + tn.len + 1, fn.p, (size_t)fn.len);
            return str_from_bytes(p, n);
        }
    }
    return f->name->name;
}

static int af_cmp_str(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp(*(const Str *)x, *(const Str *)y);
}

static AstFile *af_file(AstPackage *pkg, Str name) {
    return *(AstFile **)map_get(pkg->files, &name);
}

static void af_free_list(Alloc *a, Slice s) {
    if (s.cap > 0) {
        mem_free(a, s.p, (size_t)s.cap * sizeof(AstNode), _Alignof(AstNode));
    }
}

typedef struct AfMerged {
    AstFile *file;
    AstCommentGroup *doc;
    AstComment *separator;
    AstIdent *name;
} AfMerged;

static void af_merged_free(Alloc *a, AfMerged *m) {
    if (m->file != NULL) {
        af_free_list(a, m->file->decls);
        af_free_list(a, m->file->imports);
        af_free_list(a, m->file->comments);
        mem_free(a, m->file, sizeof(AstFile), _Alignof(AstFile));
    }
    if (m->doc != NULL) {
        af_free_list(a, m->doc->list);
        mem_free(a, m->doc, sizeof(AstCommentGroup), _Alignof(AstCommentGroup));
    }
    if (m->separator != NULL) {
        mem_free(a, m->separator, sizeof(AstComment), _Alignof(AstComment));
    }
    if (m->name != NULL) {
        mem_free(a, m->name, sizeof(AstIdent), _Alignof(AstIdent));
    }
}

/* Every list is made at its final size, so a failure part way leaves nothing
 * that af_merged_free cannot find. */
static bool af_make_list(Alloc *a, Slice *out, const Type *elem, Int n) {
    *out = slice_make(a, elem, n, n);
    return n == 0 || !slice_is_nil(*out);
}

AstFile *ast_merge_package_files(Alloc *a, AstPackage *pkg, AstMergeMode mode) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    AfMerged m = {NULL, NULL, NULL, NULL};

    /* Count everything, for the lists made below. */
    Int ndocs = 0;
    Int ncomments = 0;
    Int ndecls = 0;
    Int nimports = 0;
    Int nfiles = map_len(pkg->files);
    Slice filenames = slice_make(sa, TYPE_OF(Str), nfiles, nfiles);
    if (nfiles > 0 && slice_is_nil(filenames)) {
        goto fail;
    }
    TokenPos min_pos = TOKEN_NO_POS;
    TokenPos max_pos = TOKEN_NO_POS;
    Int i = 0;
    MapIter it = map_iter(pkg->files);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        AstFile *f = *(AstFile **)v;
        BURROW_AT(Str, filenames, i) = *(const Str *)k;
        if (f->doc != NULL) {
            ndocs += f->doc->list.len + 1; /* +1 for the separator */
        }
        ncomments += f->comments.len;
        ndecls += f->decls.len;
        nimports += f->imports.len;
        if (i == 0 || f->file_start < min_pos) {
            min_pos = f->file_start;
        }
        if (i == 0 || f->file_end > max_pos) {
            max_pos = f->file_end;
        }
        i++;
    }
    slices_sort_func(filenames, (SlicesCmpFunc){af_cmp_str, NULL});

    m.file = (AstFile *)ast_node_new(a, AST_KIND_FILE);
    if (m.file == NULL) {
        goto fail;
    }
    AstFile *out = m.file;

    /* The package comments, one after the other with a // line between
     * them. */
    if (ndocs > 0) {
        m.doc = (AstCommentGroup *)ast_node_new(a, AST_KIND_COMMENT_GROUP);
        m.separator = (AstComment *)ast_node_new(a, AST_KIND_COMMENT);
        if (m.doc == NULL || m.separator == NULL ||
            !af_make_list(a, &m.doc->list, TYPE_AST_COMMENT_PTR, ndocs - 1)) {
            goto fail;
        }
        m.separator->slash = TOKEN_NO_POS;
        m.separator->text = BURROW_S("//");
        Int j = 0;
        for (Int n = 0; n < filenames.len; n++) {
            AstFile *f = af_file(pkg, BURROW_AT(Str, filenames, n));
            if (f->doc == NULL) {
                continue;
            }
            if (j > 0) {
                af_set(m.doc->list, j, &m.separator->node);
                j++;
            }
            for (Int c = 0; c < f->doc->list.len; c++) {
                af_set(m.doc->list, j, af_at(f->doc->list, c));
                j++;
            }
            if (f->package > out->package) {
                out->package = f->package;
            }
        }
        out->doc = m.doc;
    }

    /* The declarations, in filename order. */
    if (ndecls > 0) {
        if (!af_make_list(a, &out->decls, TYPE_AST_DECL, ndecls)) {
            goto fail;
        }
        Map *funcs = NULL;
        if ((mode & AST_FILTER_FUNC_DUPLICATES) != 0) {
            funcs = map_make(sa, TYPE_OF(Str), TYPE_OF(Int), 0);
            if (funcs == NULL) {
                goto fail;
            }
        }
        Int j = 0; /* the current index */
        Int n = 0; /* how many were filtered out */
        for (Int fi = 0; fi < filenames.len; fi++) {
            AstFile *f = af_file(pkg, BURROW_AT(Str, filenames, fi));
            for (Int di = 0; di < f->decls.len; di++) {
                AstDecl d = af_at(f->decls, di);
                if (funcs != NULL && d != NULL && d->kind == AST_KIND_FUNC_DECL) {
                    /* A function or method declared more than once: keep the
                     * one with a doc comment, or the first if neither or both
                     * have one. */
                    Str name = af_name_of(sa, (AstFuncDecl *)d);
                    Int *prev = (Int *)map_get(funcs, &name);
                    if (prev != NULL) {
                        AstDecl had = af_at(out->decls, *prev);
                        if (had != NULL && ((AstFuncDecl *)had)->doc == NULL) {
                            af_set(out->decls, *prev, NULL);
                        } else {
                            d = NULL;
                        }
                        n++;
                    } else if (!map_set(funcs, &name, &j)) {
                        goto fail;
                    }
                }
                af_set(out->decls, j, d);
                j++;
            }
        }
        if (n > 0) {
            j = 0;
            for (Int di = 0; di < out->decls.len; di++) {
                AstDecl d = af_at(out->decls, di);
                if (d != NULL) {
                    af_set(out->decls, j, d);
                    j++;
                }
            }
            out->decls = af_head(out->decls, j);
        }
    }

    /* The imports. Go appends to a nil slice, so no imports stays nil. */
    if (nimports > 0) {
        if (!af_make_list(a, &out->imports, TYPE_AST_IMPORT_SPEC_PTR, nimports)) {
            goto fail;
        }
        Map *seen = NULL;
        if ((mode & AST_FILTER_IMPORT_DUPLICATES) != 0) {
            seen = map_make(sa, TYPE_OF(Str), TYPE_OF(bool), 0);
            if (seen == NULL) {
                goto fail;
            }
        }
        Int j = 0;
        for (Int fi = 0; fi < filenames.len; fi++) {
            AstFile *f = af_file(pkg, BURROW_AT(Str, filenames, fi));
            for (Int ii = 0; ii < f->imports.len; ii++) {
                AstImportSpec *imp = (AstImportSpec *)af_at(f->imports, ii);
                if (seen != NULL) {
                    Str path = imp->path->value;
                    if (map_get(seen, &path) != NULL) {
                        continue;
                    }
                    bool yes = true;
                    if (!map_set(seen, &path, &yes)) {
                        goto fail;
                    }
                }
                af_set(out->imports, j, &imp->node);
                j++;
            }
        }
        out->imports = af_head(out->imports, j);
    }

    /* The comments, unless only the doc comments are wanted. */
    if ((mode & AST_FILTER_UNASSOCIATED_COMMENTS) == 0) {
        if (!af_make_list(a, &out->comments, TYPE_AST_COMMENT_GROUP_PTR, ncomments)) {
            goto fail;
        }
        Int j = 0;
        for (Int fi = 0; fi < filenames.len; fi++) {
            AstFile *f = af_file(pkg, BURROW_AT(Str, filenames, fi));
            if (f->comments.len > 0) {
                memcpy((AstNode *)out->comments.p + j, f->comments.p,
                       (size_t)f->comments.len * sizeof(AstNode));
                j += f->comments.len;
            }
        }
    }

    m.name = ast_new_ident(a, pkg->name);
    if (m.name == NULL) {
        goto fail;
    }
    out->name = m.name;
    out->file_start = min_pos;
    out->file_end = max_pos;
    out->scope = pkg->scope;
    arena_free(&scratch);
    return out;

fail:
    af_merged_free(a, &m);
    arena_free(&scratch);
    return NULL;
}

/* ------------------------------------------------------- sorting imports */

typedef struct AfSort {
    TokenFileSet *fset;
    Arena scratch;
} AfSort;

static Int af_line_at(TokenFileSet *fset, TokenPos pos) {
    return token_file_set_position_for(fset, pos, false).line;
}

/* importPath: the unquoted path, or "" if it does not unquote. */
static Str af_import_path(Alloc *a, AstSpec s) {
    Error err = BURROW_NO_ERROR;
    Str t = strconv_unquote(a, ((AstImportSpec *)s)->path->value, &err);
    if (BURROW_OK(err)) {
        return t;
    }
    return BURROW_STR_EMPTY;
}

static Str af_import_name(AstSpec s) {
    AstIdent *n = ((AstImportSpec *)s)->name;
    if (n == NULL) {
        return BURROW_STR_EMPTY;
    }
    return n->name;
}

static Str af_import_comment(Alloc *a, AstSpec s) {
    AstCommentGroup *c = ((AstImportSpec *)s)->comment;
    if (c == NULL) {
        return BURROW_STR_EMPTY;
    }
    return ast_comment_group_text(c, a);
}

/* Whether next is a duplicate of prev that can go. */
static bool af_collapse(AfSort *st, AstSpec prev, AstSpec next) {
    Alloc *sa = arena_allocator(&st->scratch);
    ArenaMark mark = arena_mark(&st->scratch);
    bool same = str_eq(af_import_path(sa, next), af_import_path(sa, prev)) &&
                str_eq(af_import_name(next), af_import_name(prev));
    arena_release(&st->scratch, mark);
    if (!same) {
        return false;
    }
    return ((AstImportSpec *)prev)->comment == NULL;
}

static int af_cmp_spec(void *env, const void *x, const void *y) {
    AfSort *st = (AfSort *)env;
    AstSpec a = *(const AstSpec *)x;
    AstSpec b = *(const AstSpec *)y;
    Alloc *sa = arena_allocator(&st->scratch);
    ArenaMark mark = arena_mark(&st->scratch);
    int r = str_cmp(af_import_path(sa, a), af_import_path(sa, b));
    if (r == 0) {
        r = str_cmp(af_import_name(a), af_import_name(b));
    }
    if (r == 0) {
        r = str_cmp(af_import_comment(sa, a), af_import_comment(sa, b));
    }
    arena_release(&st->scratch, mark);
    return r;
}

static int af_cmp_group_pos(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos a = ast_comment_group_pos(*(AstCommentGroup *const *)x);
    TokenPos b = ast_comment_group_pos(*(AstCommentGroup *const *)y);
    return (a > b) - (a < b);
}

typedef struct AfSpan {
    TokenPos start, end;
} AfSpan;

/* A comment group and the import it belongs to, with whether it sits to the
 * left of the import or after it. */
typedef struct AfCgPos {
    AstImportSpec *spec;
    bool left;
    AstCommentGroup *cg;
} AfCgPos;

static void af_update_basic_lit_pos(AstBasicLit *lit, TokenPos pos) {
    TokenPos len = ast_basic_lit_end(lit) - lit->value_pos;
    lit->value_pos = pos;
    if (token_pos_is_valid(lit->value_end)) {
        lit->value_end = pos + len;
    }
}

/* specs sorted in place, with the duplicates taken out of the front of the
 * list. Returns how many are left. */
static Int af_sort_specs(AfSort *st, AstFile *f, AstGenDecl *d, Slice specs) {
    if (specs.len <= 1) {
        return specs.len;
    }
    TokenFileSet *fset = st->fset;
    Alloc *sa = arena_allocator(&st->scratch);
    Int n = specs.len;

    /* Where each import was, since the sorted ones take over those places. */
    AfSpan *pos = (AfSpan *)mem_alloc(sa, (size_t)n * sizeof(AfSpan), _Alignof(AfSpan));
    if (pos == NULL) {
        af_oom();
    }
    for (Int i = 0; i < n; i++) {
        AstSpec s = af_at(specs, i);
        pos[i].start = ast_spec_pos(s);
        pos[i].end = ast_spec_end(s);
    }

    /* The comments from the start of the first import's line to the end of
     * the last one's go with the imports. */
    TokenPos beg_specs = pos[0].start;
    TokenPos end_specs = pos[n - 1].end;
    TokenPos beg = token_file_line_start(token_file_set_file(fset, beg_specs),
                                         af_line_at(fset, beg_specs));
    Int end_line = af_line_at(fset, end_specs);
    TokenFile *end_file = token_file_set_file(fset, end_specs);
    TokenPos end = 0;
    if (end_line == token_file_line_count(end_file)) {
        end = end_specs;
    } else {
        end = token_file_line_start(end_file, end_line + 1); /* the next line */
    }
    Int first = f->comments.len;
    Int last = -1;
    for (Int i = 0; i < f->comments.len; i++) {
        AstCommentGroup *g = (AstCommentGroup *)af_at(f->comments, i);
        if (ast_comment_group_end(g) >= end) {
            break;
        }
        if (beg <= ast_comment_group_pos(g)) {
            if (i < first) {
                first = i;
            }
            if (i > last) {
                last = i;
            }
        }
    }
    Slice comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    if (last >= 0) {
        comments = slice_from((AstNode *)f->comments.p + first, last + 1 - first,
                              last + 1 - first, TYPE_AST_COMMENT_GROUP_PTR);
    }

    /* Which import each comment belongs to. */
    AfCgPos *cgs = NULL;
    if (comments.len > 0) {
        cgs = (AfCgPos *)mem_alloc(sa, (size_t)comments.len * sizeof(AfCgPos),
                                   _Alignof(AfCgPos));
        if (cgs == NULL) {
            af_oom();
        }
    }
    Int spec_index = 0;
    for (Int i = 0; i < comments.len; i++) {
        AstCommentGroup *g = (AstCommentGroup *)af_at(comments, i);
        TokenPos gpos = ast_comment_group_pos(g);
        while (spec_index + 1 < n && pos[spec_index + 1].start <= gpos) {
            spec_index++;
        }
        bool left = false;
        if (spec_index == 0 && pos[spec_index].start > gpos) {
            /* A comment before the first import. */
            left = true;
        } else if (spec_index + 1 < n && af_line_at(fset, pos[spec_index].start) + 1 ==
                                             af_line_at(fset, gpos)) {
            /* A comment on the line before the next import. */
            spec_index++;
            left = true;
        }
        cgs[i].spec = (AstImportSpec *)af_at(specs, spec_index);
        cgs[i].left = left;
        cgs[i].cg = g;
    }

    /* Sort, comparing paths, then names, then comments. */
    Slice view = slice_from(specs.p, n, n, TYPE_AST_SPEC);
    slices_sort_func(view, (SlicesCmpFunc){af_cmp_spec, st});

    /* Take out the duplicates, keeping the last of each run, and the line
     * each one was on. */
    Int kept = 0;
    for (Int i = 0; i < n; i++) {
        AstSpec s = af_at(specs, i);
        if (i == n - 1 || !af_collapse(st, s, af_at(specs, i + 1))) {
            af_set(specs, kept, s);
            kept++;
        } else {
            TokenPos p = ast_spec_pos(s);
            Int l = af_line_at(fset, p);
            if (l != af_line_at(fset, d->rparen)) {
                token_file_merge_line(token_file_set_file(fset, p), l);
            }
        }
    }

    /* Move the imports, and their comments, into the places they took. */
    for (Int i = 0; i < kept; i++) {
        AstImportSpec *s = (AstImportSpec *)af_at(specs, i);
        if (s->name != NULL) {
            s->name->name_pos = pos[i].start;
        }
        af_update_basic_lit_pos(s->path, pos[i].start);
        s->end_pos = pos[i].end;
        for (Int c = 0; c < comments.len; c++) {
            if (cgs[c].spec != s) {
                continue;
            }
            Slice list = cgs[c].cg->list;
            for (Int ci = 0; ci < list.len; ci++) {
                AstComment *cm = (AstComment *)af_at(list, ci);
                cm->slash = cgs[c].left ? pos[i].start - 1 : pos[i].end;
            }
        }
    }

    slices_sort_func(comments, (SlicesCmpFunc){af_cmp_group_pos, NULL});
    return kept;
}

void ast_sort_imports(Alloc *a, TokenFileSet *fset, AstFile *f) {
    AfSort st;
    st.fset = fset;
    arena_init(&st.scratch, a, 0);

    for (Int di = 0; di < f->decls.len; di++) {
        AstDecl decl = af_at(f->decls, di);
        if (decl == NULL || decl->kind != AST_KIND_GEN_DECL ||
            ((AstGenDecl *)decl)->tok != TOKEN_IMPORT) {
            /* Not an import, so the imports are over. */
            break;
        }
        AstGenDecl *d = (AstGenDecl *)decl;
        if (!token_pos_is_valid(d->lparen) || d->specs.len == 0) {
            /* Not a block, so nothing to sort. */
            continue;
        }

        /* Sort each run of lines with no blank line in it. The sorted runs
         * move down over the duplicates taken out of earlier ones. */
        Int i = 0;
        Int out = 0;
        for (Int j = 0; j <= d->specs.len; j++) {
            bool run_ends = j == d->specs.len;
            if (!run_ends && j > i) {
                Int line = af_line_at(fset, ast_spec_pos(af_at(d->specs, j)));
                Int prev = af_line_at(fset, ast_spec_end(af_at(d->specs, j - 1)));
                run_ends = line > 1 + prev;
            }
            if (!run_ends) {
                continue;
            }
            Slice run =
                slice_from((AstNode *)d->specs.p + i, j - i, j - i, TYPE_AST_SPEC);
            Int kept = af_sort_specs(&st, f, d, run);
            if (kept > 0) {
                memmove((AstNode *)d->specs.p + out, run.p,
                        (size_t)kept * sizeof(AstNode));
            }
            out += kept;
            i = j;
            arena_reset(&st.scratch);
        }
        d->specs = af_head(d->specs, out);

        /* Take out the blank lines a removed import left before the ). */
        if (d->specs.len > 0) {
            AstSpec last_spec = af_at(d->specs, d->specs.len - 1);
            Int last_line = af_line_at(fset, ast_spec_pos(last_spec));
            Int rparen_line = af_line_at(fset, d->rparen);
            while (rparen_line > last_line + 1) {
                rparen_line--;
                token_file_merge_line(token_file_set_file(fset, d->rparen),
                                      rparen_line);
            }
        }
    }
    arena_free(&st.scratch);

    /* f->imports again, from the declarations. */
    Slice imports = af_head(f->imports, 0);
    if (imports.elem == NULL) {
        imports.elem = TYPE_AST_IMPORT_SPEC_PTR;
    }
    for (Int di = 0; di < f->decls.len; di++) {
        AstDecl decl = af_at(f->decls, di);
        if (decl == NULL || decl->kind != AST_KIND_GEN_DECL ||
            ((AstGenDecl *)decl)->tok != TOKEN_IMPORT) {
            continue;
        }
        AstGenDecl *d = (AstGenDecl *)decl;
        for (Int si = 0; si < d->specs.len; si++) {
            AstNode spec = af_at(d->specs, si);
            Slice grown = slice_append(a, imports, &spec, 1);
            if (slice_is_nil(grown)) {
                af_oom();
            }
            imports = grown;
        }
    }
    f->imports = imports;
}
