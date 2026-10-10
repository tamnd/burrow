/* go/doc: documentation extracted from a Go syntax tree.
 *
 * Derived from Go's src/go/doc/doc.go, reader.go, exports.go, filter.go,
 * synopsis.go, comment.go and example.go.
 * Go source: go1.27.1.
 *
 * Go keeps a lot of this in maps it ranges over, which gives a random order.
 * Where the order can show in the result it is the order things were first
 * seen here, so a run is always the same, and where it cannot a Map is used
 * as in Go. The three regular expressions, for note markers and for the
 * Output: comment of an example, are simple enough to match by hand, which
 * saves compiling them into state that would have to live somewhere.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/doc.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/doc/comment.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- helpers */

BURROW_NORETURN static void gd_oom(void) {
    panic_str(S("go/doc: out of memory"));
}

/* Zeroed memory for one T, which mem_alloc gives. */
static void *gd_alloc(Alloc *a, size_t n) {
    void *p = mem_alloc(a, n, BURROW_ALIGN_MAX);
    if (p == NULL)
        gd_oom();
    return p;
}

#define GD_NEW(a, T) ((T *)gd_alloc((a), sizeof(T)))

/* A copy of the n bytes at p, which is how Go's x := *p is written here. */
static void *gd_copy(Alloc *a, const void *p, size_t n) {
    void *q = gd_alloc(a, n);
    memcpy(q, p, n);
    return q;
}

static void gd_append(Alloc *a, Slice *s, const void *v) {
    Slice next = slice_append(a, *s, v, 1);
    if (next.len != s->len + 1)
        gd_oom();
    *s = next;
}

static void gd_append_slice(Alloc *a, Slice *s, Slice t) {
    if (t.len == 0)
        return;
    Slice next = slice_append_slice(a, *s, t);
    if (next.len != s->len + t.len)
        gd_oom();
    *s = next;
}

static Str gd_sub(Str s, Int lo, Int hi) {
    if (lo == hi)
        return BURROW_STR_EMPTY; /* s.p can be NULL, and NULL + 0 is undefined */
    return str_from_bytes(s.p + lo, hi - lo);
}

static Str gd_concat3(Alloc *a, Str x, Str y, Str z) {
    Int n = x.len + y.len + z.len;
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc(a, (size_t)n, 1);
    if (p == NULL)
        gd_oom();
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    if (z.len > 0)
        memcpy(p + x.len + y.len, z.p, (size_t)z.len);
    return str_from_bytes(p, n);
}

static Map *gd_map(Alloc *a, const Type *key, const Type *val) {
    Map *m = map_make(a, key, val, 0);
    if (m == NULL)
        gd_oom();
    return m;
}

static void gd_map_set(Map *m, const void *key, const void *val) {
    if (!map_set(m, key, val))
        gd_oom();
}

/* The pointer stored under key in a map of pointers, or NULL. */
static void *gd_map_ptr(Map *m, const void *key) {
    if (m == NULL)
        return NULL;
    void **v = (void **)map_get(m, key);
    return v == NULL ? NULL : *v;
}

static bool gd_map_bool(Map *m, Str key) {
    if (m == NULL)
        return false;
    bool *v = (bool *)map_get(m, &key);
    return v != NULL && *v;
}

static void gd_set_true(Map *m, Str key) {
    bool t = true;
    gd_map_set(m, &key, &t);
}

/* A key for a map of pointers that may be NULL, as Go's map[*T] may be. */
static Uintptr gd_key(const void *p) {
    return (Uintptr)p;
}

static AstNode gd_node(Slice s, Int i) {
    return BURROW_AT(AstNode, s, i);
}

static bool gd_is(AstNode n, AstKind kind) {
    return n != NULL && n->kind == (Int)kind;
}

static Str gd_text(Alloc *a, AstCommentGroup *g) {
    return ast_comment_group_text(g, a);
}

static int gd_cmp_str(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp(*(const Str *)x, *(const Str *)y);
}

/* ----------------------------------------------------- predeclared names */

static const Str gd_predeclared_types[] = {
    BURROW_S_INIT("any"),       BURROW_S_INIT("bool"),
    BURROW_S_INIT("byte"),      BURROW_S_INIT("comparable"),
    BURROW_S_INIT("complex64"), BURROW_S_INIT("complex128"),
    BURROW_S_INIT("error"),     BURROW_S_INIT("float32"),
    BURROW_S_INIT("float64"),   BURROW_S_INIT("int"),
    BURROW_S_INIT("int8"),      BURROW_S_INIT("int16"),
    BURROW_S_INIT("int32"),     BURROW_S_INIT("int64"),
    BURROW_S_INIT("rune"),      BURROW_S_INIT("string"),
    BURROW_S_INIT("uint"),      BURROW_S_INIT("uint8"),
    BURROW_S_INIT("uint16"),    BURROW_S_INIT("uint32"),
    BURROW_S_INIT("uint64"),    BURROW_S_INIT("uintptr"),
};

static const Str gd_predeclared_funcs[] = {
    BURROW_S_INIT("append"),  BURROW_S_INIT("cap"),     BURROW_S_INIT("clear"),
    BURROW_S_INIT("close"),   BURROW_S_INIT("complex"), BURROW_S_INIT("copy"),
    BURROW_S_INIT("delete"),  BURROW_S_INIT("imag"),    BURROW_S_INIT("len"),
    BURROW_S_INIT("make"),    BURROW_S_INIT("max"),     BURROW_S_INIT("min"),
    BURROW_S_INIT("new"),     BURROW_S_INIT("panic"),   BURROW_S_INIT("print"),
    BURROW_S_INIT("println"), BURROW_S_INIT("real"),    BURROW_S_INIT("recover"),
};

static const Str gd_predeclared_constants[] = {
    BURROW_S_INIT("false"),
    BURROW_S_INIT("iota"),
    BURROW_S_INIT("nil"),
    BURROW_S_INIT("true"),
};

#define GD_COUNT(arr) ((Int)(sizeof(arr) / sizeof((arr)[0])))

static bool gd_in(const Str *list, Int n, Str s) {
    for (Int i = 0; i < n; i++) {
        if (str_eq(list[i], s))
            return true;
    }
    return false;
}

static bool gd_predeclared_type(Str s) {
    return gd_in(gd_predeclared_types, GD_COUNT(gd_predeclared_types), s);
}

static bool gd_predeclared_func(Str s) {
    return gd_in(gd_predeclared_funcs, GD_COUNT(gd_predeclared_funcs), s);
}

static bool gd_predeclared_constant(Str s) {
    return gd_in(gd_predeclared_constants, GD_COUNT(gd_predeclared_constants), s);
}

bool doc_is_predeclared(Str s) {
    return gd_predeclared_type(s) || gd_predeclared_func(s) ||
           gd_predeclared_constant(s);
}

static bool gd_not_identifier(Rune ch) {
    return !(('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z') ||
             ('0' <= ch && ch <= '9') || ch == '_' ||
             (ch >= UTF8_RUNE_SELF && (unicode_is_letter(ch) || unicode_is_digit(ch))));
}

/* assumedPackageName, the name a package imported as import_path most likely
 * has, a copy of golang.org/x/tools/internal/imports.ImportPathToAssumedName. */
static Str gd_assumed_package_name(Alloc *a, Str import_path) {
    Str base = path_base(import_path);
    if (strings_has_prefix(base, S("v"))) {
        Error err = BURROW_NO_ERROR;
        (void)strconv_atoi(gd_sub(base, 1, base.len), &err);
        if (BURROW_OK(err)) {
            Str dir = path_dir(a, import_path);
            if (!str_eq(dir, S(".")))
                base = path_base(dir);
        }
    }
    base = strings_trim_prefix(base, S("go-"));
    StrIter it = str_runes(base);
    Int i = 0;
    Rune r = 0;
    while (str_next_rune(&it, &i, &r)) {
        if (gd_not_identifier(r))
            return gd_sub(base, 0, i);
    }
    return base;
}

/* ------------------------------------------------- function and method sets */

/* recvParam. */
static Str gd_recv_param(AstExpr p) {
    if (gd_is(p, AST_KIND_IDENT))
        return ((AstIdent *)p)->name;
    return S("BADPARAM");
}

/* recvString: recv as "T", "*T", "T[A, ...]" or "*T[A, ...]", or "BADRECV"
 * when it is not a proper receiver type. */
static Str gd_recv_string(Alloc *a, AstExpr recv) {
    if (recv == NULL)
        return S("BADRECV");
    switch ((int)recv->kind) {
    case AST_KIND_IDENT:
        return ((AstIdent *)recv)->name;
    case AST_KIND_STAR_EXPR:
        return gd_concat3(a, S("*"), gd_recv_string(a, ((AstStarExpr *)recv)->x),
                          BURROW_STR_EMPTY);
    case AST_KIND_INDEX_EXPR: {
        /* generic type with one parameter */
        AstIndexExpr *t = (AstIndexExpr *)recv;
        Str x = gd_concat3(a, gd_recv_string(a, t->x), S("["), gd_recv_param(t->index));
        return gd_concat3(a, x, S("]"), BURROW_STR_EMPTY);
    }
    case AST_KIND_INDEX_LIST_EXPR: {
        /* generic type with multiple parameters */
        AstIndexListExpr *t = (AstIndexListExpr *)recv;
        if (t->indices.len > 0) {
            Str b = gd_concat3(a, gd_recv_string(a, t->x), S("["),
                               gd_recv_param(gd_node(t->indices, 0)));
            for (Int i = 1; i < t->indices.len; i++)
                b = gd_concat3(a, b, S(", "), gd_recv_param(gd_node(t->indices, i)));
            return gd_concat3(a, b, S("]"), BURROW_STR_EMPTY);
        }
        break;
    }
    default:
        break;
    }
    return S("BADRECV");
}

static DocFunc *gd_new_func(Alloc *a) {
    DocFunc *f = GD_NEW(a, DocFunc);
    f->examples = slice_nil(TYPE_DOC_EXAMPLE_PTR);
    return f;
}

/* A methodSet is a map from a name (Str) to a DocFunc pointer. Entries with a
 * NULL decl are conflicts, more than one method with the same name at the same
 * embedding level. */
static Map *gd_new_mset(Alloc *a) {
    return gd_map(a, TYPE_OF(Str), TYPE_DOC_FUNC_PTR);
}

/* methodSet.set: adds the Func for f, keeping the first one with
 * documentation when there are several with the same name. preserve says
 * whether to leave the tree alone. */
static void gd_mset_set(Alloc *a, Map *mset, AstFuncDecl *f, bool preserve) {
    Str name = f->name->name;
    DocFunc *g = (DocFunc *)gd_map_ptr(mset, &name);
    if (g != NULL && g->doc.len > 0) {
        /* A function with the same name has already been registered; since
         * it has documentation, assume f is simply another implementation
         * and ignore it. */
        return;
    }
    /* function doesn't exist or has no documentation; use f */
    Str recv = BURROW_STR_EMPTY;
    if (f->recv != NULL) {
        AstExpr typ = NULL;
        /* be careful in case of incorrect ASTs */
        if (f->recv->list.len == 1)
            typ = BURROW_AT(AstField *, f->recv->list, 0)->type;
        recv = gd_recv_string(a, typ);
    }
    DocFunc *fn = gd_new_func(a);
    fn->doc = gd_text(a, f->doc);
    fn->name = name;
    fn->decl = f;
    fn->recv = recv;
    fn->orig = recv;
    gd_map_set(mset, &name, &fn);
    if (!preserve)
        f->doc = NULL; /* doc consumed - remove from AST */
}

/* methodSet.add: adds m unless the set already has a method with the same
 * name at the same or a higher level. */
static void gd_mset_add(Alloc *a, Map *mset, DocFunc *m) {
    DocFunc *old = (DocFunc *)gd_map_ptr(mset, &m->name);
    if (old == NULL || m->level < old->level) {
        gd_map_set(mset, &m->name, &m);
        return;
    }
    if (m->level == old->level) {
        /* conflict - mark it using a method with nil Decl */
        DocFunc *c = gd_new_func(a);
        c->name = m->name;
        c->level = m->level;
        gd_map_set(mset, &m->name, &c);
    }
}

/* ------------------------------------------------------------ named types */

/* baseTypeName: the name of the base type of x, or "", and whether it is
 * imported. */
static Str gd_base_type_name(AstExpr x, bool *imported) {
    *imported = false;
    if (x == NULL)
        return BURROW_STR_EMPTY;
    switch ((int)x->kind) {
    case AST_KIND_IDENT:
        return ((AstIdent *)x)->name;
    case AST_KIND_INDEX_EXPR:
        return gd_base_type_name(((AstIndexExpr *)x)->x, imported);
    case AST_KIND_INDEX_LIST_EXPR:
        return gd_base_type_name(((AstIndexListExpr *)x)->x, imported);
    case AST_KIND_SELECTOR_EXPR: {
        AstSelectorExpr *t = (AstSelectorExpr *)x;
        if (gd_is(t->x, AST_KIND_IDENT)) {
            /* only possible for qualified type names; assume type is
             * imported */
            *imported = true;
            return t->sel->name;
        }
        break;
    }
    case AST_KIND_PAREN_EXPR:
        return gd_base_type_name(((AstParenExpr *)x)->x, imported);
    case AST_KIND_STAR_EXPR:
        return gd_base_type_name(((AstStarExpr *)x)->x, imported);
    default:
        break;
    }
    return BURROW_STR_EMPTY;
}

typedef struct GdNamed GdNamed;

/* One entry of an embeddedSet: an embedded type and whether it is embedded
 * as a pointer. */
typedef struct GdEmbedded {
    GdNamed *type;
    bool ptr;
} GdEmbedded;

static const Type gd_embedded_type = {
    {(const Byte *)"embedded", 8},
    {(const Byte *)"go/doc", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(GdEmbedded),
    (uint16_t)_Alignof(GdEmbedded),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* namedType, a named unqualified type, local to the package or maybe
 * predeclared, always found with gd_lookup_type. */
struct GdNamed {
    Str doc;          /* doc comment for type */
    Str name;         /* type name */
    AstGenDecl *decl; /* NULL if declaration hasn't been seen yet */

    bool is_embedded; /* true if this type is embedded */
    bool is_struct;   /* true if this type is a struct */
    Slice embedded;   /* of GdEmbedded, in the order first embedded */

    /* associated declarations */
    Slice values; /* of DocValue *, consts and vars */
    Map *funcs;
    Map *methods;

    bool deleted;  /* taken out of the reader's types */
    bool visiting; /* in the visited set of gd_collect_embedded_methods */
};

/* A predeclared type an interface embeds, which is taken out of it again if
 * the package declares a type of that name. */
typedef struct GdFix {
    Str predecl;
    AstInterfaceType *ityp;
} GdFix;

static const Type gd_fix_type = {
    {(const Byte *)"fix", 3},
    {(const Byte *)"go/doc", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(GdFix),
    (uint16_t)_Alignof(GdFix),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ----------------------------------------------------------------- reader */

/* reader, which accumulates documentation for a single package. It changes
 * the tree: the comments it collects are taken out of the nodes so that they
 * are not printed twice, once with the documentation and once with the
 * node. */
typedef struct GdReader {
    Alloc *a;
    DocMode mode;

    /* package properties */
    Str doc; /* package documentation, if any */
    Slice filenames;
    Map *notes;

    /* imports */
    Map *imports; /* Str to Int */
    bool has_dot_imp;
    Map *import_by_name;

    /* declarations */
    Slice values; /* of DocValue *, consts and vars */
    Int order;    /* sort order of const and var declarations */
    Slice types;  /* of GdNamed *, in the order first seen */
    Map *type_map;
    Map *funcs;

    /* support for package-local shadowing of predeclared types */
    Map *shadowed_predecl; /* Str to bool, or NULL */
    Slice fixes;           /* of GdFix */

    AstIdent *underscore;
} GdReader;

static bool gd_preserve(const GdReader *r) {
    return (r->mode & DOC_PRESERVE_AST) != 0;
}

static bool gd_is_visible(const GdReader *r, Str name) {
    return (r->mode & DOC_ALL_DECLS) != 0 || token_is_exported(name);
}

/* lookupType: the base type with the given name, added with no declaration
 * when it has not been seen yet, or NULL for no name or a blank one. */
static GdNamed *gd_lookup_type(GdReader *r, Str name) {
    if (name.len == 0 || str_eq(name, S("_")))
        return NULL; /* no type docs for anonymous types */
    GdNamed *typ = (GdNamed *)gd_map_ptr(r->type_map, &name);
    if (typ != NULL)
        return typ;
    /* type not found - add one without declaration */
    typ = GD_NEW(r->a, GdNamed);
    typ->name = name;
    typ->embedded = slice_nil(&gd_embedded_type);
    typ->values = slice_nil(TYPE_DOC_VALUE_PTR);
    typ->funcs = gd_new_mset(r->a);
    typ->methods = gd_new_mset(r->a);
    gd_map_set(r->type_map, &name, &typ);
    gd_append(r->a, &r->types, &typ);
    return typ;
}

static GdNamed *gd_type_at(const GdReader *r, Int i) {
    return BURROW_AT(GdNamed *, r->types, i);
}

/* recordAnonymousField: records field_type as the type of an anonymous field
 * of parent, unless it is imported or parent is NULL, and gives back the
 * field's name. */
static Str gd_record_anonymous_field(GdReader *r, GdNamed *parent, AstExpr field_type) {
    bool imp = false;
    Str fname = gd_base_type_name(field_type, &imp);
    if (parent == NULL || imp)
        return fname;
    GdNamed *ftype = gd_lookup_type(r, fname);
    if (ftype != NULL) {
        ftype->is_embedded = true;
        bool ptr = gd_is(field_type, AST_KIND_STAR_EXPR);
        for (Int i = 0; i < parent->embedded.len; i++) {
            GdEmbedded *e = &BURROW_AT(GdEmbedded, parent->embedded, i);
            if (e->type == ftype) {
                e->ptr = ptr;
                return fname;
            }
        }
        GdEmbedded e = {ftype, ptr};
        gd_append(r->a, &parent->embedded, &e);
    }
    return fname;
}

static void gd_read_doc(GdReader *r, AstCommentGroup *comment) {
    /* By convention there should be only one package comment but collect all
     * of them if there are more than one. */
    Str text = gd_text(r->a, comment);
    if (r->doc.len == 0) {
        r->doc = text;
        return;
    }
    r->doc = gd_concat3(r->a, r->doc, S("\n"), text);
}

static void gd_remember(GdReader *r, Str predecl, AstInterfaceType *typ) {
    GdFix f = {predecl, typ};
    gd_append(r->a, &r->fixes, &f);
}

static Slice gd_spec_names(Alloc *a, Slice specs) {
    Slice names = slice_make(a, TYPE_OF(Str), 0, specs.len); /* reasonable estimate */
    if (specs.len > 0 && slice_is_nil(names))
        gd_oom();
    for (Int i = 0; i < specs.len; i++) {
        AstNode s = gd_node(specs, i);
        if (!gd_is(s, AST_KIND_VALUE_SPEC))
            continue;
        Slice ids = ((AstValueSpec *)s)->names;
        for (Int j = 0; j < ids.len; j++)
            gd_append(a, &names, &BURROW_AT(AstIdent *, ids, j)->name);
    }
    return names;
}

/* readValue: a const or var declaration. */
static void gd_read_value(GdReader *r, AstGenDecl *decl) {
    /* determine if decl should be associated with a type
     * Heuristic: For each typed entry, determine the type name, if any. If
     * there is exactly one type name that is sufficiently frequent, associate
     * the decl with the respective type. */
    Str dom_name = BURROW_STR_EMPTY;
    Int dom_freq = 0;
    Str prev = BURROW_STR_EMPTY;
    Int n = 0;
    for (Int i = 0; i < decl->specs.len; i++) {
        AstNode spec = gd_node(decl->specs, i);
        if (!gd_is(spec, AST_KIND_VALUE_SPEC))
            continue; /* should not happen, but be conservative */
        AstValueSpec *s = (AstValueSpec *)spec;
        Str name = BURROW_STR_EMPTY;
        if (s->type != NULL) {
            /* a type is present; determine its name */
            bool imp = false;
            Str tn = gd_base_type_name(s->type, &imp);
            if (!imp)
                name = tn;
        } else if (decl->tok == TOKEN_CONST && s->values.len == 0) {
            /* no type or value is present but we have a constant
             * declaration; use the previous type name (possibly the empty
             * string) */
            name = prev;
        }
        if (name.len > 0) {
            /* entry has a named type */
            if (dom_name.len > 0 && !str_eq(dom_name, name)) {
                /* more than one type name - do not associate with any type */
                dom_name = BURROW_STR_EMPTY;
                break;
            }
            dom_name = name;
            dom_freq++;
        }
        prev = name;
        n++;
    }

    /* nothing to do w/o a legal declaration */
    if (n == 0)
        return;

    /* determine values list with which to associate the Value for this decl */
    Slice *values = &r->values;
    const double threshold = 0.75;
    if (dom_name.len > 0 && gd_is_visible(r, dom_name) &&
        dom_freq >= (Int)((double)decl->specs.len * threshold)) {
        /* typed entries are sufficiently frequent */
        GdNamed *typ = gd_lookup_type(r, dom_name);
        if (typ != NULL)
            values = &typ->values; /* associate with that type */
    }

    DocValue *v = GD_NEW(r->a, DocValue);
    v->doc = gd_text(r->a, decl->doc);
    v->names = gd_spec_names(r->a, decl->specs);
    v->decl = decl;
    v->order = r->order;
    gd_append(r->a, values, &v);
    if (!gd_preserve(r))
        decl->doc = NULL; /* doc consumed - remove from AST */
    /* The order is global, because gd_cleanup_types may move values
     * associated with types back into the global list. */
    r->order++;
}

/* fields: a struct's fields or an interface's methods. */
static Slice gd_fields(AstExpr typ, bool *is_struct) {
    *is_struct = false;
    AstFieldList *fields = NULL;
    if (gd_is(typ, AST_KIND_STRUCT_TYPE)) {
        fields = ((AstStructType *)typ)->fields;
        *is_struct = true;
    } else if (gd_is(typ, AST_KIND_INTERFACE_TYPE)) {
        fields = ((AstInterfaceType *)typ)->methods;
    }
    if (fields != NULL)
        return fields->list;
    return slice_nil(TYPE_AST_FIELD_PTR);
}

/* readType: a type declaration. */
static void gd_read_type(GdReader *r, AstGenDecl *decl, AstTypeSpec *spec) {
    GdNamed *typ = gd_lookup_type(r, spec->name->name);
    if (typ == NULL)
        return; /* no name or blank name - ignore the type */

    /* A type should be added at most once, so typ->decl should be NULL - if
     * it is not, simply overwrite it. */
    typ->decl = decl;

    /* compute documentation */
    AstCommentGroup *doc = spec->doc;
    if (doc == NULL) {
        /* no doc associated with the spec, use the declaration doc, if any */
        doc = decl->doc;
    }
    if (!gd_preserve(r)) {
        spec->doc = NULL; /* doc consumed - remove from AST */
        decl->doc = NULL; /* doc consumed - remove from AST */
    }
    typ->doc = gd_text(r->a, doc);

    /* record anonymous fields (they may contribute methods) (some fields may
     * have been recorded already when filtering exports, but that's ok) */
    Slice list = gd_fields(spec->type, &typ->is_struct);
    for (Int i = 0; i < list.len; i++) {
        AstField *field = BURROW_AT(AstField *, list, i);
        if (field->names.len == 0)
            (void)gd_record_anonymous_field(r, typ, field->type);
    }
}

/* isPredeclared: whether n is a predeclared type the package does not
 * declare itself. */
static bool gd_is_predeclared(GdReader *r, Str n) {
    return gd_predeclared_type(n) && gd_map_ptr(r->type_map, &n) == NULL;
}

/* lookupTypeParam: the type parameter named name in tparams, or NULL. */
static AstIdent *gd_lookup_type_param(Str name, AstFieldList *tparams) {
    if (tparams == NULL)
        return NULL;
    for (Int i = 0; i < tparams->list.len; i++) {
        AstField *field = BURROW_AT(AstField *, tparams->list, i);
        for (Int j = 0; j < field->names.len; j++) {
            AstIdent *id = BURROW_AT(AstIdent *, field->names, j);
            if (str_eq(id->name, name))
                return id;
        }
    }
    return NULL;
}

/* readFunc: a func or method declaration. */
static void gd_read_func(GdReader *r, AstFuncDecl *fun) {
    /* strip function body if requested. */
    if (!gd_preserve(r))
        fun->body = NULL;

    /* associate methods with the receiver type, if any */
    if (fun->recv != NULL) {
        /* method */
        if (fun->recv->list.len == 0) {
            /* should not happen (incorrect AST); don't show this method */
            return;
        }
        bool imp = false;
        Str recv_type_name =
            gd_base_type_name(BURROW_AT(AstField *, fun->recv->list, 0)->type, &imp);
        if (imp) {
            /* should not happen (incorrect AST); don't show this method */
            return;
        }
        GdNamed *typ = gd_lookup_type(r, recv_type_name);
        if (typ != NULL)
            gd_mset_set(r->a, typ->methods, fun, gd_preserve(r));
        /* otherwise ignore the method */
        return;
    }

    /* Associate factory functions with the first visible result type, as
     * long as others are predeclared types. */
    AstFieldList *results = fun->type->results;
    if (ast_field_list_num_fields(results) >= 1) {
        GdNamed *typ = NULL; /* type to associate the function with */
        Int num_result_types = 0;
        for (Int i = 0; i < results->list.len; i++) {
            AstExpr factory_type = BURROW_AT(AstField *, results->list, i)->type;
            if (gd_is(factory_type, AST_KIND_ARRAY_TYPE)) {
                /* We consider functions that return slices or arrays of type
                 * T (or pointers to T) as factory functions of T. */
                factory_type = ((AstArrayType *)factory_type)->elt;
            }
            bool imp = false;
            Str n = gd_base_type_name(factory_type, &imp);
            if (!imp && gd_is_visible(r, n) && !gd_is_predeclared(r, n)) {
                if (gd_lookup_type_param(n, fun->type->type_params) != NULL) {
                    /* Issue #49477: don't associate fun with its type
                     * parameter result. A type parameter is not a defined
                     * type. */
                    continue;
                }
                GdNamed *t = gd_lookup_type(r, n);
                if (t != NULL) {
                    typ = t;
                    num_result_types++;
                    if (num_result_types > 1)
                        break;
                }
            }
        }
        /* If there is exactly one result type, associate the function with
         * that type. */
        if (num_result_types == 1) {
            gd_mset_set(r->a, typ->funcs, fun, gd_preserve(r));
            return;
        }
    }

    /* just an ordinary function */
    gd_mset_set(r->a, r->funcs, fun, gd_preserve(r));
}

/* --------------------------------------------------------------- notes */

/* noteMarker, ([A-Z][A-Z]+)\(([^)]+)\):?, matched at s[i:]. It gives the end
 * of the match, or -1, and the marker and the uid in m[0:2] and m[2:4]. */
static Int gd_note_marker(Str s, Int i, Int m[4]) {
    Int n = s.len;
    Int ms = i;
    while (i < n && s.p[i] >= 'A' && s.p[i] <= 'Z')
        i++;
    if (i - ms < 2)
        return -1;
    Int me = i;
    if (i >= n || s.p[i] != '(')
        return -1;
    i++;
    Int us = i;
    while (i < n && s.p[i] != ')')
        i++;
    if (i == us || i >= n)
        return -1;
    Int ue = i;
    i++;
    if (i < n && s.p[i] == ':')
        i++;
    m[0] = ms;
    m[1] = me;
    m[2] = us;
    m[3] = ue;
    return i;
}

/* noteMarkerRx, ^[ \t]* followed by a marker. */
static Int gd_note_marker_rx(Str s, Int m[4]) {
    Int i = 0;
    while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t'))
        i++;
    return gd_note_marker(s, i, m);
}

/* noteCommentRx: the start of a comment, any spaces or tabs, then a marker. */
static bool gd_note_comment_rx(Str s) {
    if (s.len < 2 || s.p[0] != '/' || (s.p[1] != '/' && s.p[1] != '*'))
        return false;
    Int i = 2;
    while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t'))
        i++;
    Int m[4];
    return gd_note_marker(s, i, m) >= 0;
}

/* clean: each run of spaces, \r and \t made one space, and the spaces at
 * either end taken off. */
static Str gd_clean(Alloc *a, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *b = (Byte *)mem_alloc(a, (size_t)s.len, 1);
    if (b == NULL)
        gd_oom();
    Int n = 0;
    Byte p = ' ';
    for (Int i = 0; i < s.len; i++) {
        Byte q = s.p[i];
        if (q == '\r' || q == '\t')
            q = ' ';
        if (q != ' ' || p != ' ') {
            b[n++] = q;
            p = q;
        }
    }
    /* remove trailing blank, if any */
    if (n > 0 && p == ' ')
        n--;
    if (n == 0)
        return BURROW_STR_EMPTY;
    return str_from_bytes(b, n);
}

/* readNote: a single note from a run of comments. */
static void gd_read_note(GdReader *r, Slice list) {
    AstCommentGroup g = {{AST_KIND_COMMENT_GROUP}, list};
    Str text = gd_text(r->a, &g);
    Int m[4];
    Int end = gd_note_marker_rx(text, m);
    if (end < 0)
        return;
    /* The note body starts after the marker. We remove any formatting so that
     * we don't get spurious line breaks/indentation when showing the TODO
     * body. */
    Str body = gd_clean(r->a, gd_sub(text, end, text.len));
    if (body.len == 0)
        return;
    Str marker = gd_sub(text, m[0], m[1]);
    DocNote *note = GD_NEW(r->a, DocNote);
    note->pos = ast_comment_pos(BURROW_AT(AstComment *, list, 0));
    note->end = ast_comment_end(BURROW_AT(AstComment *, list, list.len - 1));
    note->uid = gd_sub(text, m[2], m[3]);
    note->body = body;
    Slice *notes = (Slice *)map_get(r->notes, &marker);
    Slice next = notes != NULL ? *notes : slice_nil(TYPE_DOC_NOTE_PTR);
    gd_append(r->a, &next, &note);
    gd_map_set(r->notes, &marker, &next);
}

/* readNotes: the notes in comments. A note starts at the start of a comment
 * with "MARKER(uid):" and runs to the end of the group or the start of the
 * next note in it. */
static void gd_read_notes(GdReader *r, Slice comments) {
    for (Int k = 0; k < comments.len; k++) {
        AstCommentGroup *group = BURROW_AT(AstCommentGroup *, comments, k);
        Int i = -1; /* comment index of most recent note start, valid if >= 0 */
        Slice list = group->list;
        for (Int j = 0; j < list.len; j++) {
            if (gd_note_comment_rx(BURROW_AT(AstComment *, list, j)->text)) {
                if (i >= 0)
                    gd_read_note(r, slice_sub(list, i, j));
                i = j;
            }
        }
        if (i >= 0)
            gd_read_note(r, slice_sub(list, i, list.len));
    }
}

/* ---------------------------------------------------------------- exports */

/* filterIdentList: the exported names of list, in place. */
static Slice gd_filter_ident_list(Slice list) {
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstIdent *x = BURROW_AT(AstIdent *, list, i);
        if (token_is_exported(x->name)) {
            BURROW_AT(AstIdent *, list, j) = x;
            j++;
        }
    }
    return slice_sub(list, 0, j);
}

static Slice gd_filter_expr_list(Slice list);

static void gd_filter_composite_lit(AstCompositeLit *lit) {
    Int n = lit->elts.len;
    lit->elts = gd_filter_expr_list(lit->elts);
    if (lit->elts.len < n)
        lit->incomplete = true;
}

/* filterExprList, whose filter is always token.IsExported in Go. */
static Slice gd_filter_expr_list(Slice list) {
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstExpr exp = gd_node(list, i);
        if (gd_is(exp, AST_KIND_COMPOSITE_LIT)) {
            gd_filter_composite_lit((AstCompositeLit *)exp);
        } else if (gd_is(exp, AST_KIND_KEY_VALUE_EXPR)) {
            AstKeyValueExpr *x = (AstKeyValueExpr *)exp;
            if (gd_is(x->key, AST_KIND_IDENT) &&
                !token_is_exported(((AstIdent *)x->key)->name))
                continue;
            if (gd_is(x->value, AST_KIND_COMPOSITE_LIT))
                gd_filter_composite_lit((AstCompositeLit *)x->value);
        }
        BURROW_AT(AstExpr, list, j) = exp;
        j++;
    }
    return slice_sub(list, 0, j);
}

static AstIdent *gd_underscore(GdReader *r) {
    if (r->underscore == NULL) {
        r->underscore = ast_new_ident(r->a, S("_"));
        if (r->underscore == NULL)
            gd_oom();
    }
    return r->underscore;
}

/* updateIdentList: every unexported name in list replaced with _, and
 * whether any was exported. */
static bool gd_update_ident_list(GdReader *r, Slice list) {
    bool has_exported = false;
    for (Int i = 0; i < list.len; i++) {
        if (token_is_exported(BURROW_AT(AstIdent *, list, i)->name))
            has_exported = true;
        else
            BURROW_AT(AstIdent *, list, i) = gd_underscore(r);
    }
    return has_exported;
}

static bool gd_has_exported_name(Slice list) {
    for (Int i = 0; i < list.len; i++) {
        if (ast_ident_is_exported(BURROW_AT(AstIdent *, list, i)))
            return true;
    }
    return false;
}

/* removeAnonymousField: takes the embedded type name out of ityp. */
static void gd_remove_anonymous_field(Str name, AstInterfaceType *ityp) {
    Slice list = ityp->methods->list; /* we know that ityp->methods != NULL */
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstField *field = BURROW_AT(AstField *, list, i);
        bool keep_field = true;
        if (field->names.len == 0) {
            bool imp = false;
            if (str_eq(gd_base_type_name(field->type, &imp), name))
                keep_field = false;
        }
        if (keep_field) {
            BURROW_AT(AstField *, list, j) = field;
            j++;
        }
    }
    if (j < list.len)
        ityp->incomplete = true;
    ityp->methods->list = slice_sub(list, 0, j);
}

static void gd_filter_type(GdReader *r, GdNamed *parent, AstExpr typ);

/* filterFieldList: the unexported fields of a struct or methods of an
 * interface taken out, and whether any were. ityp is the interface, or NULL
 * for a struct. */
static bool gd_filter_field_list(GdReader *r, GdNamed *parent, AstFieldList *fields,
                                 AstInterfaceType *ityp) {
    bool removed_fields = false;
    if (fields == NULL)
        return removed_fields;
    Slice list = fields->list;
    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstField *field = BURROW_AT(AstField *, list, i);
        bool keep_field = false;
        Int n = field->names.len;
        if (n == 0) {
            /* anonymous field or embedded type or union element */
            Str fname = gd_record_anonymous_field(r, parent, field->type);
            if (fname.len > 0) {
                if (token_is_exported(fname)) {
                    keep_field = true;
                } else if (ityp != NULL && gd_predeclared_type(fname)) {
                    /* possibly an embedded predeclared type; keep it for
                     * now but remember this interface so that it can be
                     * fixed if name is also defined locally */
                    keep_field = true;
                    gd_remember(r, fname, ityp);
                }
            } else {
                /* If we're operating on an interface, assume that this is an
                 * embedded type or union element. */
                keep_field = ityp != NULL;
            }
        } else {
            field->names = gd_filter_ident_list(field->names);
            if (field->names.len < n)
                removed_fields = true;
            if (field->names.len > 0)
                keep_field = true;
        }
        if (keep_field) {
            gd_filter_type(r, NULL, field->type);
            BURROW_AT(AstField *, list, j) = field;
            j++;
        }
    }
    if (j < list.len)
        removed_fields = true;
    fields->list = slice_sub(list, 0, j);
    return removed_fields;
}

static void gd_filter_param_list(GdReader *r, AstFieldList *fields) {
    if (fields == NULL)
        return;
    for (Int i = 0; i < fields->list.len; i++)
        gd_filter_type(r, NULL, BURROW_AT(AstField *, fields->list, i)->type);
}

/* filterType: the unexported parts of typ taken out. parent is the named type
 * typ is the type of, if any. */
static void gd_filter_type(GdReader *r, GdNamed *parent, AstExpr typ) {
    if (typ == NULL)
        return;
    switch ((int)typ->kind) {
    case AST_KIND_IDENT:
        break;
    case AST_KIND_PAREN_EXPR:
        gd_filter_type(r, NULL, ((AstParenExpr *)typ)->x);
        break;
    case AST_KIND_STAR_EXPR: /* possibly an embedded type literal */
        gd_filter_type(r, NULL, ((AstStarExpr *)typ)->x);
        break;
    case AST_KIND_UNARY_EXPR: {
        AstUnaryExpr *t = (AstUnaryExpr *)typ;
        if (t->op == TOKEN_TILDE) /* approximation element */
            gd_filter_type(r, NULL, t->x);
        break;
    }
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *t = (AstBinaryExpr *)typ;
        if (t->op == TOKEN_OR) { /* union */
            gd_filter_type(r, NULL, t->x);
            gd_filter_type(r, NULL, t->y);
        }
        break;
    }
    case AST_KIND_ARRAY_TYPE:
        gd_filter_type(r, NULL, ((AstArrayType *)typ)->elt);
        break;
    case AST_KIND_STRUCT_TYPE: {
        AstStructType *t = (AstStructType *)typ;
        if (gd_filter_field_list(r, parent, t->fields, NULL))
            t->incomplete = true;
        break;
    }
    case AST_KIND_FUNC_TYPE: {
        AstFuncType *t = (AstFuncType *)typ;
        gd_filter_param_list(r, t->type_params);
        gd_filter_param_list(r, t->params);
        gd_filter_param_list(r, t->results);
        break;
    }
    case AST_KIND_INTERFACE_TYPE: {
        AstInterfaceType *t = (AstInterfaceType *)typ;
        if (gd_filter_field_list(r, parent, t->methods, t))
            t->incomplete = true;
        break;
    }
    case AST_KIND_MAP_TYPE:
        gd_filter_type(r, NULL, ((AstMapType *)typ)->key);
        gd_filter_type(r, NULL, ((AstMapType *)typ)->value);
        break;
    case AST_KIND_CHAN_TYPE:
        gd_filter_type(r, NULL, ((AstChanType *)typ)->value);
        break;
    default:
        break;
    }
}

static bool gd_filter_spec(GdReader *r, AstSpec spec) {
    switch ((int)spec->kind) {
    case AST_KIND_IMPORT_SPEC:
        /* always keep imports so we can collect them */
        return true;
    case AST_KIND_VALUE_SPEC: {
        AstValueSpec *s = (AstValueSpec *)spec;
        s->values = gd_filter_expr_list(s->values);
        if (s->values.len > 0 || (s->type == NULL && s->values.len == 0)) {
            /* If there are values declared on RHS, just replace the
             * unexported identifiers on the LHS with underscore, so that it
             * matches the sequence of expression on the RHS.
             *
             * Similarly, if there are no type and values, then this
             * expression must be following an iota expression, where order
             * matters. */
            if (gd_update_ident_list(r, s->names)) {
                gd_filter_type(r, NULL, s->type);
                return true;
            }
        } else {
            s->names = gd_filter_ident_list(s->names);
            if (s->names.len > 0) {
                gd_filter_type(r, NULL, s->type);
                return true;
            }
        }
        break;
    }
    case AST_KIND_TYPE_SPEC: {
        AstTypeSpec *s = (AstTypeSpec *)spec;
        /* Don't filter type parameters here, by analogy with function
         * parameters which are not filtered for top-level function
         * declarations. */
        Str name = s->name->name;
        if (token_is_exported(name)) {
            gd_filter_type(r, gd_lookup_type(r, name), s->type);
            return true;
        }
        if (doc_is_predeclared(name)) {
            if (r->shadowed_predecl == NULL)
                r->shadowed_predecl = gd_map(r->a, TYPE_OF(Str), TYPE_BOOL);
            gd_set_true(r->shadowed_predecl, name);
        }
        break;
    }
    default:
        break;
    }
    return false;
}

/* copyConstType: a copy of the type of a const spec at pos, for the specs
 * after it that have neither a type nor values. */
static AstExpr gd_copy_const_type(Alloc *a, AstExpr typ, TokenPos pos) {
    if (gd_is(typ, AST_KIND_IDENT)) {
        AstIdent *id = ast_new_ident(a, ((AstIdent *)typ)->name);
        if (id == NULL)
            gd_oom();
        id->name_pos = pos;
        return &id->node;
    }
    if (gd_is(typ, AST_KIND_SELECTOR_EXPR)) {
        AstSelectorExpr *t = (AstSelectorExpr *)typ;
        if (gd_is(t->x, AST_KIND_IDENT)) {
            AstSelectorExpr *sel =
                (AstSelectorExpr *)ast_node_new(a, AST_KIND_SELECTOR_EXPR);
            AstIdent *x = ast_new_ident(a, ((AstIdent *)t->x)->name);
            AstIdent *name = ast_new_ident(a, t->sel->name);
            if (sel == NULL || x == NULL || name == NULL)
                gd_oom();
            x->name_pos = pos;
            sel->sel = name;
            sel->x = &x->node;
            return &sel->node;
        }
    }
    return NULL; /* shouldn't happen, but be conservative and don't panic */
}

static Slice gd_filter_spec_list(GdReader *r, Slice list, Token tok) {
    if (tok == TOKEN_CONST) {
        /* Remove any unexported const specs, keeping the types of the
         * exported ones that relied on them. */
        AstExpr prev_type = NULL;
        for (Int i = 0; i < list.len; i++) {
            AstNode sp = gd_node(list, i);
            if (!gd_is(sp, AST_KIND_VALUE_SPEC))
                continue;
            AstValueSpec *spec = (AstValueSpec *)sp;
            if (spec->type == NULL && spec->values.len == 0 && prev_type != NULL) {
                /* provide current spec with an explicit type */
                spec->type = gd_copy_const_type(r->a, prev_type, ast_spec_pos(sp));
            }
            if (gd_has_exported_name(spec->names)) {
                /* exported names are preserved so there's no need to
                 * propagate the type */
                prev_type = NULL;
            } else {
                prev_type = spec->type;
            }
        }
    }

    Int j = 0;
    for (Int i = 0; i < list.len; i++) {
        AstSpec s = gd_node(list, i);
        if (gd_filter_spec(r, s)) {
            BURROW_AT(AstSpec, list, j) = s;
            j++;
        }
    }
    return slice_sub(list, 0, j);
}

static bool gd_filter_decl(GdReader *r, AstDecl decl) {
    if (gd_is(decl, AST_KIND_GEN_DECL)) {
        AstGenDecl *d = (AstGenDecl *)decl;
        d->specs = gd_filter_spec_list(r, d->specs, d->tok);
        return d->specs.len > 0;
    }
    if (gd_is(decl, AST_KIND_FUNC_DECL)) {
        /* ok to filter these methods early because any conflicting method
         * will be filtered here, too - thus, removing these methods early
         * will not lead to the false removal of possible conflicts */
        return token_is_exported(((AstFuncDecl *)decl)->name->name);
    }
    return false;
}

/* fileExports: the unexported declarations of src taken out. */
static void gd_file_exports(GdReader *r, AstFile *src) {
    Int j = 0;
    for (Int i = 0; i < src->decls.len; i++) {
        AstDecl d = gd_node(src->decls, i);
        if (gd_filter_decl(r, d)) {
            BURROW_AT(AstDecl, src->decls, j) = d;
            j++;
        }
    }
    src->decls = slice_sub(src->decls, 0, j);
}

/* ------------------------------------------------------------ reading files */

static void gd_read_import(GdReader *r, AstImportSpec *s) {
    Error err = BURROW_NO_ERROR;
    Str import_ = strconv_unquote(r->a, s->path->value, &err);
    if (BURROW_FAILED(err))
        return;
    Int one = 1;
    gd_map_set(r->imports, &import_, &one);
    Str name = BURROW_STR_EMPTY;
    if (s->name != NULL) {
        name = s->name->name;
        if (str_eq(name, S(".")))
            r->has_dot_imp = true;
    }
    if (str_eq(name, S(".")))
        return;
    if (name.len == 0)
        name = gd_assumed_package_name(r->a, import_);
    Str *old = (Str *)map_get(r->import_by_name, &name);
    if (old == NULL) {
        gd_map_set(r->import_by_name, &name, &import_);
    } else if (!str_eq(*old, import_) && old->len > 0) {
        Str ambiguous = BURROW_STR_EMPTY;
        gd_map_set(r->import_by_name, &name, &ambiguous);
    }
}

/* readFile: adds the tree of a source file to the reader. */
static void gd_read_file(GdReader *r, AstFile *src) {
    /* add package documentation */
    if (src->doc != NULL) {
        gd_read_doc(r, src->doc);
        if (!gd_preserve(r))
            src->doc = NULL; /* doc consumed - remove from AST */
    }

    /* add all declarations but for functions which are processed in a
     * separate pass */
    for (Int i = 0; i < src->decls.len; i++) {
        AstDecl decl = gd_node(src->decls, i);
        if (!gd_is(decl, AST_KIND_GEN_DECL))
            continue;
        AstGenDecl *d = (AstGenDecl *)decl;
        switch ((int)d->tok) {
        case TOKEN_IMPORT:
            /* imports are handled individually */
            for (Int j = 0; j < d->specs.len; j++) {
                AstSpec spec = gd_node(d->specs, j);
                if (gd_is(spec, AST_KIND_IMPORT_SPEC))
                    gd_read_import(r, (AstImportSpec *)spec);
            }
            break;
        case TOKEN_CONST:
        case TOKEN_VAR:
            /* constants and variables are always handled as a group */
            gd_read_value(r, d);
            break;
        case TOKEN_TYPE_:
            /* types are handled individually */
            if (d->specs.len == 1 && !token_pos_is_valid(d->lparen)) {
                /* common case: single declaration w/o parentheses (if a
                 * single declaration is parenthesized, create a new fake
                 * declaration below, so that go/doc type declarations always
                 * appear w/o parentheses) */
                AstSpec s = gd_node(d->specs, 0);
                if (gd_is(s, AST_KIND_TYPE_SPEC))
                    gd_read_type(r, d, (AstTypeSpec *)s);
                break;
            }
            for (Int j = 0; j < d->specs.len; j++) {
                AstSpec s = gd_node(d->specs, j);
                if (!gd_is(s, AST_KIND_TYPE_SPEC))
                    continue;
                /* use an individual (possibly fake) declaration for each
                 * type; this also ensures that each type gets to (re-)use the
                 * declaration documentation if there's none associated with
                 * the spec itself. The fake declaration is not given the
                 * existing tok_pos, which would give it the wrong selection
                 * range when there are more types in the group. */
                AstGenDecl *fake = (AstGenDecl *)ast_node_new(r->a, AST_KIND_GEN_DECL);
                if (fake == NULL)
                    gd_oom();
                fake->doc = d->doc;
                fake->tok_pos = ast_spec_pos(s);
                fake->tok = TOKEN_TYPE_;
                fake->specs = slice_make(r->a, TYPE_AST_SPEC, 1, 1);
                if (slice_is_nil(fake->specs))
                    gd_oom();
                BURROW_AT(AstSpec, fake->specs, 0) = s;
                gd_read_type(r, fake, (AstTypeSpec *)s);
            }
            break;
        default:
            break;
        }
    }

    /* collect MARKER(...): annotations */
    gd_read_notes(r, src->comments);
    if (!gd_preserve(r)) {
        /* consumed unassociated comments - remove from AST */
        src->comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    }
}

static AstFile *gd_package_file(AstPackage *pkg, Str filename) {
    return (AstFile *)gd_map_ptr(pkg->files, &filename);
}

static void gd_read_package(GdReader *r, AstPackage *pkg, DocMode mode) {
    Alloc *a = r->a;
    /* initialize reader */
    Int nfiles = pkg->files == NULL ? 0 : map_len(pkg->files);
    r->filenames = slice_make(a, TYPE_OF(Str), 0, nfiles);
    if (nfiles > 0 && slice_is_nil(r->filenames))
        gd_oom();
    r->imports = gd_map(a, TYPE_OF(Str), TYPE_INT);
    r->mode = mode;
    r->types = slice_nil(TYPE_UNSAFE_POINTER);
    r->type_map = gd_map(a, TYPE_OF(Str), TYPE_UNSAFE_POINTER);
    r->funcs = gd_new_mset(a);
    r->notes = gd_map(a, TYPE_OF(Str), TYPE_DOC_NOTE_SLICE);
    r->import_by_name = gd_map(a, TYPE_OF(Str), TYPE_OF(Str));
    r->values = slice_nil(TYPE_DOC_VALUE_PTR);
    r->fixes = slice_nil(&gd_fix_type);

    /* sort package files before reading them so that the result does not
     * depend on map iteration order */
    if (nfiles > 0) {
        MapIter it = map_iter(pkg->files);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v))
            gd_append(a, &r->filenames, k);
    }
    slices_sort_func(r->filenames, (SlicesCmpFunc){gd_cmp_str, NULL});

    /* process files in sorted order */
    for (Int i = 0; i < r->filenames.len; i++) {
        AstFile *f = gd_package_file(pkg, BURROW_AT(Str, r->filenames, i));
        if ((mode & DOC_ALL_DECLS) == 0)
            gd_file_exports(r, f);
        gd_read_file(r, f);
    }

    /* drop the names more than one import used */
    Slice ambiguous = slice_nil(TYPE_OF(Str));
    MapIter it = map_iter(r->import_by_name);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        if (((const Str *)v)->len == 0)
            gd_append(a, &ambiguous, k);
    }
    for (Int i = 0; i < ambiguous.len; i++)
        map_del(r->import_by_name, &BURROW_AT(Str, ambiguous, i));

    /* process functions now that we have better type information */
    for (Int i = 0; i < r->filenames.len; i++) {
        AstFile *f = gd_package_file(pkg, BURROW_AT(Str, r->filenames, i));
        for (Int j = 0; j < f->decls.len; j++) {
            AstDecl d = gd_node(f->decls, j);
            if (gd_is(d, AST_KIND_FUNC_DECL))
                gd_read_func(r, (AstFuncDecl *)d);
        }
    }
}

/* ------------------------------------------------------------------ types */

/* customizeRecv: a copy of f, with a copy of its declaration whose receiver
 * is recv_type_name, a pointer unless the embedded type was one or the
 * original receiver was not. */
static DocFunc *gd_customize_recv(Alloc *a, DocFunc *f, Str recv_type_name,
                                  bool embedded_is_ptr, Int level) {
    if (f == NULL || f->decl == NULL || f->decl->recv == NULL ||
        f->decl->recv->list.len != 1)
        return f; /* shouldn't happen, but be safe */

    /* copy existing receiver field and set new type */
    AstField *new_field = (AstField *)gd_copy(
        a, BURROW_AT(AstField *, f->decl->recv->list, 0), sizeof(AstField));
    TokenPos orig_pos = ast_expr_pos(new_field->type);
    bool orig_recv_is_ptr = gd_is(new_field->type, AST_KIND_STAR_EXPR);
    AstIdent *new_ident = ast_new_ident(a, recv_type_name);
    if (new_ident == NULL)
        gd_oom();
    new_ident->name_pos = orig_pos;
    AstExpr typ = &new_ident->node;
    if (!embedded_is_ptr && orig_recv_is_ptr) {
        new_ident->name_pos++; /* '*' is one character */
        AstStarExpr *star = (AstStarExpr *)ast_node_new(a, AST_KIND_STAR_EXPR);
        if (star == NULL)
            gd_oom();
        star->star = orig_pos;
        star->x = typ;
        typ = &star->node;
    }
    new_field->type = typ;

    /* copy existing receiver field list and set new receiver field */
    AstFieldList *new_field_list =
        (AstFieldList *)gd_copy(a, f->decl->recv, sizeof(AstFieldList));
    new_field_list->list = slice_make(a, TYPE_AST_FIELD_PTR, 1, 1);
    if (slice_is_nil(new_field_list->list))
        gd_oom();
    BURROW_AT(AstField *, new_field_list->list, 0) = new_field;

    /* copy existing function declaration and set new receiver field list */
    AstFuncDecl *new_func_decl =
        (AstFuncDecl *)gd_copy(a, f->decl, sizeof(AstFuncDecl));
    new_func_decl->recv = new_field_list;

    /* copy existing function documentation and set new declaration */
    DocFunc *new_f = (DocFunc *)gd_copy(a, f, sizeof(DocFunc));
    new_f->decl = new_func_decl;
    new_f->recv = gd_recv_string(a, typ);
    /* the orig field never changes */
    new_f->level = level;
    return new_f;
}

/* The DocFunc pointers of a method set, in no particular order. */
static Slice gd_mset_list(Alloc *a, Map *mset) {
    Slice list = slice_make(a, TYPE_DOC_FUNC_PTR, 0, map_len(mset));
    if (map_len(mset) > 0 && slice_is_nil(list))
        gd_oom();
    MapIter it = map_iter(mset);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v))
        gd_append(a, &list, v);
    return list;
}

/* collectEmbeddedMethods: the embedded methods of typ, added to mset. */
static void gd_collect_embedded_methods(GdReader *r, Map *mset, GdNamed *typ,
                                        Str recv_type_name, bool embedded_is_ptr,
                                        Int level) {
    typ->visiting = true;
    for (Int i = 0; i < typ->embedded.len; i++) {
        GdEmbedded e = BURROW_AT(GdEmbedded, typ->embedded, i);
        /* Once an embedded type is embedded as a pointer type all embedded
         * types in those types are treated like pointer types for the purpose
         * of the receiver type computation; i.e., embedded_is_ptr is sticky
         * for this embedding hierarchy. */
        bool this_embedded_is_ptr = embedded_is_ptr || e.ptr;
        /* A list first, since mset may be the very set being read. */
        Slice methods = gd_mset_list(r->a, e.type->methods);
        for (Int j = 0; j < methods.len; j++) {
            DocFunc *m = BURROW_AT(DocFunc *, methods, j);
            /* only top-level methods are embedded */
            if (m->level == 0)
                gd_mset_add(r->a, mset,
                            gd_customize_recv(r->a, m, recv_type_name,
                                              this_embedded_is_ptr, level));
        }
        if (!e.type->visiting)
            gd_collect_embedded_methods(r, mset, e.type, recv_type_name,
                                        this_embedded_is_ptr, level + 1);
    }
    typ->visiting = false;
}

/* computeMethodSets: the actual method sets of each type. */
static void gd_compute_method_sets(GdReader *r) {
    for (Int i = 0; i < r->types.len; i++) {
        GdNamed *t = gd_type_at(r, i);
        /* collect embedded methods for t */
        if (t->is_struct)
            gd_collect_embedded_methods(r, t->methods, t, t->name, false, 1);
        /* interfaces: TODO(gri) fix this, as Go says */
    }

    /* For any predeclared names that are declared locally, don't treat them
     * as exported fields anymore. */
    for (Int i = 0; i < r->fixes.len; i++) {
        GdFix f = BURROW_AT(GdFix, r->fixes, i);
        if (gd_map_bool(r->shadowed_predecl, f.predecl))
            gd_remove_anonymous_field(f.predecl, f.ityp);
    }
}

/* cleanupTypes: functions and methods of types with no declaration moved to
 * the package level, and the types with no declaration, or that are not
 * visible, taken out. */
static void gd_cleanup_types(GdReader *r) {
    for (Int i = 0; i < r->types.len; i++) {
        GdNamed *t = gd_type_at(r, i);
        bool visible = gd_is_visible(r, t->name);
        bool predeclared = gd_predeclared_type(t->name);

        if (t->decl == NULL &&
            (predeclared || (visible && (t->is_embedded || r->has_dot_imp)))) {
            /* t->name is a predeclared type (and was not redeclared in this
             * package), or it was embedded somewhere but its declaration is
             * missing (because the AST is incomplete), or we have a dot-import
             * (and all bets are off): move any associated values, funcs, and
             * methods back to the top-level so that they are not lost. */
            /* 1) move values */
            gd_append_slice(r->a, &r->values, t->values);
            /* 2) move factory functions */
            MapIter it = map_iter(t->funcs);
            const void *k = NULL;
            void *v = NULL;
            while (map_next(&it, &k, &v)) {
                /* in a correct AST, package-level function names are all
                 * different - no need to check for conflicts */
                gd_map_set(r->funcs, k, v);
            }
            /* 3) move methods */
            if (!predeclared) {
                it = map_iter(t->methods);
                while (map_next(&it, &k, &v)) {
                    /* don't overwrite functions with the same name - drop
                     * them */
                    if (map_get(r->funcs, k) == NULL)
                        gd_map_set(r->funcs, k, v);
                }
            }
        }
        /* remove types w/o declaration or which are not visible */
        if (t->decl == NULL || !visible) {
            t->deleted = true;
            map_del(r->type_map, &t->name);
        }
    }
}

/* ---------------------------------------------------------------- sorting */

/* sortingName: the name to sort d by, or "". */
static Str gd_sorting_name(AstGenDecl *d) {
    if (d->specs.len == 1) {
        AstSpec s = gd_node(d->specs, 0);
        if (gd_is(s, AST_KIND_VALUE_SPEC))
            return BURROW_AT(AstIdent *, ((AstValueSpec *)s)->names, 0)->name;
    }
    return BURROW_STR_EMPTY;
}

static int gd_cmp_value(void *env, const void *x, const void *y) {
    (void)env;
    const DocValue *a = *(DocValue *const *)x;
    const DocValue *b = *(DocValue *const *)y;
    int r = str_cmp(gd_sorting_name(a->decl), gd_sorting_name(b->decl));
    if (r != 0)
        return r;
    return a->order < b->order ? -1 : a->order > b->order;
}

static Slice gd_sorted_values(Alloc *a, Slice m, Token tok) {
    Slice list =
        slice_make(a, TYPE_DOC_VALUE_PTR, 0, m.len); /* big enough in any case */
    if (m.len > 0 && slice_is_nil(list))
        gd_oom();
    for (Int i = 0; i < m.len; i++) {
        DocValue *val = BURROW_AT(DocValue *, m, i);
        if (val->decl->tok == tok)
            gd_append(a, &list, &val);
    }
    slices_sort_func(list, (SlicesCmpFunc){gd_cmp_value, NULL});
    return list;
}

static int gd_cmp_func(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp((*(DocFunc *const *)x)->name, (*(DocFunc *const *)y)->name);
}

static Str gd_remove_star(Str s) {
    if (s.len > 0 && s.p[0] == '*')
        return gd_sub(s, 1, s.len);
    return s;
}

static Slice gd_sorted_funcs(Alloc *a, Map *m, bool all_methods) {
    Slice list = slice_make(a, TYPE_DOC_FUNC_PTR, 0, map_len(m));
    if (map_len(m) > 0 && slice_is_nil(list))
        gd_oom();
    MapIter it = map_iter(m);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        DocFunc *f = *(DocFunc **)v;
        /* determine which methods to include */
        if (f->decl == NULL)
            continue; /* exclude conflict entry */
        if (all_methods || f->level == 0 ||
            !token_is_exported(gd_remove_star(f->orig))) {
            /* forced inclusion, method not embedded, or method embedded but
             * original receiver type not exported */
            gd_append(a, &list, &f);
        }
    }
    slices_sort_func(list, (SlicesCmpFunc){gd_cmp_func, NULL});
    return list;
}

static int gd_cmp_type(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp((*(DocType *const *)x)->name, (*(DocType *const *)y)->name);
}

static Slice gd_sorted_types(GdReader *r, bool all_methods) {
    Alloc *a = r->a;
    Slice list = slice_make(a, TYPE_DOC_TYPE_PTR, 0, r->types.len);
    if (r->types.len > 0 && slice_is_nil(list))
        gd_oom();
    for (Int i = 0; i < r->types.len; i++) {
        GdNamed *t = gd_type_at(r, i);
        if (t->deleted)
            continue;
        DocType *dt = GD_NEW(a, DocType);
        dt->doc = t->doc;
        dt->name = t->name;
        dt->decl = t->decl;
        dt->consts = gd_sorted_values(a, t->values, TOKEN_CONST);
        dt->vars = gd_sorted_values(a, t->values, TOKEN_VAR);
        dt->funcs = gd_sorted_funcs(a, t->funcs, true);
        dt->methods = gd_sorted_funcs(a, t->methods, all_methods);
        dt->examples = slice_nil(TYPE_DOC_EXAMPLE_PTR);
        gd_append(a, &list, &dt);
    }
    slices_sort_func(list, (SlicesCmpFunc){gd_cmp_type, NULL});
    return list;
}

/* noteBodies: the bodies of notes, for the deprecated Package.Bugs. */
static Slice gd_note_bodies(Alloc *a, Map *notes) {
    Slice list = slice_nil(TYPE_OF(Str));
    Str bug = S("BUG");
    Slice *bugs = (Slice *)map_get(notes, &bug);
    if (bugs == NULL)
        return list;
    for (Int i = 0; i < bugs->len; i++)
        gd_append(a, &list, &BURROW_AT(DocNote *, *bugs, i)->body);
    return list;
}

/* sortedKeys: the keys of a map from Str, sorted. */
static Slice gd_map_keys_sorted(Alloc *a, Map *m) {
    Slice list = slice_make(a, TYPE_OF(Str), 0, map_len(m));
    if (map_len(m) > 0 && slice_is_nil(list))
        gd_oom();
    MapIter it = map_iter(m);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v))
        gd_append(a, &list, k);
    slices_sort_func(list, (SlicesCmpFunc){gd_cmp_str, NULL});
    return list;
}

/* --------------------------------------------------------------- package */

static void gd_collect_values(DocPackage *p, Slice values) {
    for (Int i = 0; i < values.len; i++) {
        DocValue *v = BURROW_AT(DocValue *, values, i);
        for (Int j = 0; j < v->names.len; j++)
            gd_set_true(p->syms, BURROW_AT(Str, v->names, j));
    }
}

static void gd_collect_funcs(Alloc *a, DocPackage *p, Slice funcs) {
    for (Int i = 0; i < funcs.len; i++) {
        DocFunc *f = BURROW_AT(DocFunc *, funcs, i);
        if (f->recv.len > 0) {
            Str r = strings_trim_prefix(f->recv, S("*"));
            Int j = strings_index_byte(r, '[');
            if (j >= 0)
                r = gd_sub(r, 0, j); /* remove type parameters */
            gd_set_true(p->syms, gd_concat3(a, r, S("."), f->name));
        } else {
            gd_set_true(p->syms, f->name);
        }
    }
}

/* collectInterfaceMethods and collectStructFields, which are the same but for
 * which of the two kinds of type they look at. Interface methods are added to
 * syms so that links like [io.Reader.Read] work, without adding them to
 * Type.Methods and so to the index. */
static void gd_collect_fields(Alloc *a, DocPackage *p, DocType *t, bool structs) {
    for (Int i = 0; i < t->decl->specs.len; i++) {
        AstSpec s = gd_node(t->decl->specs, i);
        if (!gd_is(s, AST_KIND_TYPE_SPEC))
            continue;
        bool is_struct = false;
        Slice list = gd_fields(((AstTypeSpec *)s)->type, &is_struct);
        if (is_struct != structs)
            continue;
        for (Int j = 0; j < list.len; j++) {
            AstField *field = BURROW_AT(AstField *, list, j);
            for (Int k = 0; k < field->names.len; k++) {
                Str name = BURROW_AT(AstIdent *, field->names, k)->name;
                gd_set_true(p->syms, gd_concat3(a, t->name, S("."), name));
            }
        }
    }
}

static void gd_collect_types(Alloc *a, DocPackage *p, Slice types) {
    for (Int i = 0; i < types.len; i++) {
        DocType *t = BURROW_AT(DocType *, types, i);
        if (gd_map_bool(p->syms, t->name)) {
            /* Shouldn't be any cycles but stop just in case. */
            continue;
        }
        gd_set_true(p->syms, t->name);
        gd_collect_values(p, t->consts);
        gd_collect_values(p, t->vars);
        gd_collect_funcs(a, p, t->funcs);
        gd_collect_funcs(a, p, t->methods);
        gd_collect_fields(a, p, t, false);
        gd_collect_fields(a, p, t, true);
    }
}

DocPackage *doc_new(Alloc *a, AstPackage *pkg, Str import_path, DocMode mode) {
    GdReader r;
    memset(&r, 0, sizeof r);
    r.a = a;
    gd_read_package(&r, pkg, mode);
    gd_compute_method_sets(&r);
    gd_cleanup_types(&r);

    DocPackage *p = GD_NEW(a, DocPackage);
    p->doc = r.doc;
    p->name = pkg->name;
    p->import_path = import_path;

    p->imports = gd_map_keys_sorted(a, r.imports);
    p->filenames = r.filenames;
    p->notes = r.notes;
    p->bugs = gd_note_bodies(a, r.notes);
    p->consts = gd_sorted_values(a, r.values, TOKEN_CONST);
    p->types = gd_sorted_types(&r, (mode & DOC_ALL_METHODS) != 0);
    p->vars = gd_sorted_values(a, r.values, TOKEN_VAR);
    p->funcs = gd_sorted_funcs(a, r.funcs, true);
    p->examples = slice_nil(TYPE_DOC_EXAMPLE_PTR);

    p->import_by_name = r.import_by_name;
    p->syms = gd_map(a, TYPE_OF(Str), TYPE_BOOL);

    gd_collect_values(p, p->consts);
    gd_collect_values(p, p->vars);
    gd_collect_types(a, p, p->types);
    gd_collect_funcs(a, p, p->funcs);
    return p;
}

/* lookupSym: whether the package has a top level name, or with recv a type
 * recv with a field or method name. */
static bool gd_lookup_sym(void *env, Str recv, Str name) {
    DocPackage *p = (DocPackage *)env;
    if (recv.len == 0)
        return gd_map_bool(p->syms, name);
    Byte buf[256];
    if (recv.len + 1 + name.len <= (Int)sizeof buf) {
        memcpy(buf, recv.p, (size_t)recv.len);
        buf[recv.len] = '.';
        if (name.len > 0)
            memcpy(buf + recv.len + 1, name.p, (size_t)name.len);
        return gd_map_bool(p->syms, str_from_bytes(buf, recv.len + 1 + name.len));
    }
    /* Too long for the buffer, and there is no allocator here, so look at
     * each key instead. */
    if (p->syms == NULL)
        return false;
    MapIter it = map_iter(p->syms);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        Str key = *(const Str *)k;
        if (key.len == recv.len + 1 + name.len && strings_has_prefix(key, recv) &&
            key.p[recv.len] == '.' && strings_has_suffix(key, name))
            return *(bool *)v;
    }
    return false;
}

/* lookupPackage: the import path that name is for. ok is true with "" for
 * the package itself, and false for an unknown name or one that more than
 * one import uses. */
static Str gd_lookup_package(void *env, Str name, bool *ok) {
    DocPackage *p = (DocPackage *)env;
    Str *path =
        p->import_by_name == NULL ? NULL : (Str *)map_get(p->import_by_name, &name);
    if (path != NULL) {
        *ok = path->len > 0; /* "" means multiple imports used the name */
        return *ok ? *path : BURROW_STR_EMPTY;
    }
    *ok = str_eq(p->name, name); /* allow reference to this package */
    return BURROW_STR_EMPTY;
}

static CommentParser gd_parser(DocPackage *p) {
    CommentParser parser;
    memset(&parser, 0, sizeof parser);
    parser.lookup_package = BURROW_FN(CommentLookupPackageFunc, gd_lookup_package, p);
    parser.lookup_sym = BURROW_FN(CommentLookupSymFunc, gd_lookup_sym, p);
    return parser;
}

static CommentPrinter gd_printer(DocPackage *p) {
    /* No customization today, as in Go. */
    (void)p;
    CommentPrinter pr;
    memset(&pr, 0, sizeof pr);
    return pr;
}

CommentParser *doc_package_parser(DocPackage *p, Alloc *a) {
    CommentParser *parser = GD_NEW(a, CommentParser);
    *parser = gd_parser(p);
    return parser;
}

CommentPrinter *doc_package_printer(DocPackage *p, Alloc *a) {
    CommentPrinter *pr = GD_NEW(a, CommentPrinter);
    *pr = gd_printer(p);
    return pr;
}

Slice doc_package_html(DocPackage *p, Alloc *a, Str text) {
    CommentParser parser = gd_parser(p);
    CommentPrinter pr = gd_printer(p);
    return comment_printer_html(&pr, a, comment_parser_parse(&parser, a, text));
}

Slice doc_package_markdown(DocPackage *p, Alloc *a, Str text) {
    CommentParser parser = gd_parser(p);
    CommentPrinter pr = gd_printer(p);
    return comment_printer_markdown(&pr, a, comment_parser_parse(&parser, a, text));
}

Slice doc_package_text(DocPackage *p, Alloc *a, Str text) {
    CommentParser parser = gd_parser(p);
    CommentPrinter pr = gd_printer(p);
    return comment_printer_text(&pr, a, comment_parser_parse(&parser, a, text));
}

static void gd_write(IoWriter w, Slice b) {
    Error err = BURROW_NO_ERROR;
    (void)w.vt->write(w.data, b, &err); /* Go ignores the error too */
}

void doc_to_html(Alloc *a, IoWriter w, Str text, Map *words) {
    DocPackage p;
    memset(&p, 0, sizeof p);
    CommentParser parser = gd_parser(&p);
    parser.words = words;
    CommentDoc *d = comment_parser_parse(&parser, a, text);
    CommentPrinter pr;
    memset(&pr, 0, sizeof pr);
    gd_write(w, comment_printer_html(&pr, a, d));
}

void doc_to_text(Alloc *a, IoWriter w, Str text, Str prefix, Str code_prefix,
                 Int width) {
    DocPackage p;
    memset(&p, 0, sizeof p);
    CommentParser parser = gd_parser(&p);
    CommentDoc *d = comment_parser_parse(&parser, a, text);
    CommentPrinter pr;
    memset(&pr, 0, sizeof pr);
    pr.text_prefix = prefix;
    pr.text_code_prefix = code_prefix;
    pr.text_width = width;
    gd_write(w, comment_printer_text(&pr, a, d));
}

/* --------------------------------------------------------------- synopsis */

/* firstSentence: the first sentence of s, which ends after the first period
 * followed by space and not preceded by exactly one upper case letter, or
 * after a full stop of the CJK kind. */
Str burrow__doc_first_sentence(Str s);
Str burrow__doc_first_sentence(Str s) {
    Rune ppp = 0;
    Rune pp = 0;
    Rune p = 0;
    StrIter it = str_runes(s);
    Int i = 0;
    Rune q = 0;
    while (str_next_rune(&it, &i, &q)) {
        if (q == '\n' || q == '\r' || q == '\t')
            q = ' ';
        if (q == ' ' && p == '.' && (!unicode_is_upper(pp) || unicode_is_upper(ppp)))
            return gd_sub(s, 0, i);
        if (p == 0x3002 || p == 0xFF0E) /* 。 and ． */
            return gd_sub(s, 0, i);
        ppp = pp;
        pp = p;
        p = q;
    }
    return s;
}

static const Str gd_illegal_prefixes[] = {
    BURROW_S_INIT("copyright"),
    BURROW_S_INIT("all rights"),
    BURROW_S_INIT("author"),
};

Slice doc_illegal_prefixes(void) {
    return slice_from((void *)(uintptr_t)gd_illegal_prefixes,
                      GD_COUNT(gd_illegal_prefixes), GD_COUNT(gd_illegal_prefixes),
                      TYPE_OF(Str));
}

Str doc_package_synopsis(DocPackage *p, Alloc *a, Str text) {
    text = burrow__doc_first_sentence(text);
    Str lower = strings_to_lower(a, text);
    for (Int i = 0; i < GD_COUNT(gd_illegal_prefixes); i++) {
        if (strings_has_prefix(lower, gd_illegal_prefixes[i]))
            return BURROW_STR_EMPTY;
    }
    CommentPrinter pr = gd_printer(p);
    pr.text_width = -1;
    CommentParser parser = gd_parser(p);
    CommentDoc *d = comment_parser_parse(&parser, a, text);
    if (d->content.len == 0)
        return BURROW_STR_EMPTY;
    if (BURROW_AT(CommentBlock, d->content, 0)->kind != COMMENT_KIND_PARAGRAPH)
        return BURROW_STR_EMPTY;
    /* might be blank lines, code blocks, etc in "first sentence" */
    d->content = slice_sub(d->content, 0, 1);
    Slice out = comment_printer_text(&pr, a, d);
    if (out.len == 0)
        return BURROW_STR_EMPTY;
    return strings_trim_space(str_from_bytes((const Byte *)out.p, out.len));
}

Str doc_synopsis(Alloc *a, Str text) {
    DocPackage p;
    memset(&p, 0, sizeof p);
    return doc_package_synopsis(&p, a, text);
}

/* ----------------------------------------------------------------- filter */

static bool gd_match_fields(AstFieldList *fields, DocFilter f) {
    if (fields == NULL)
        return false;
    for (Int i = 0; i < fields->list.len; i++) {
        Slice names = BURROW_AT(AstField *, fields->list, i)->names;
        for (Int j = 0; j < names.len; j++) {
            if (BURROW_CALLF(f, BURROW_AT(AstIdent *, names, j)->name))
                return true;
        }
    }
    return false;
}

static bool gd_match_decl(AstGenDecl *d, DocFilter f) {
    for (Int i = 0; i < d->specs.len; i++) {
        AstSpec spec = gd_node(d->specs, i);
        if (gd_is(spec, AST_KIND_VALUE_SPEC)) {
            Slice names = ((AstValueSpec *)spec)->names;
            for (Int j = 0; j < names.len; j++) {
                if (BURROW_CALLF(f, BURROW_AT(AstIdent *, names, j)->name))
                    return true;
            }
        } else if (gd_is(spec, AST_KIND_TYPE_SPEC)) {
            AstTypeSpec *v = (AstTypeSpec *)spec;
            if (BURROW_CALLF(f, v->name->name))
                return true;
            /* We don't match ordinary parameters in filterFuncs, so by
             * analogy don't match type parameters here. */
            if (gd_is(v->type, AST_KIND_STRUCT_TYPE) &&
                gd_match_fields(((AstStructType *)v->type)->fields, f))
                return true;
            if (gd_is(v->type, AST_KIND_INTERFACE_TYPE) &&
                gd_match_fields(((AstInterfaceType *)v->type)->methods, f))
                return true;
        }
    }
    return false;
}

static Slice gd_filter_values(Slice a, DocFilter f) {
    Int w = 0;
    for (Int i = 0; i < a.len; i++) {
        DocValue *vd = BURROW_AT(DocValue *, a, i);
        if (gd_match_decl(vd->decl, f)) {
            BURROW_AT(DocValue *, a, w) = vd;
            w++;
        }
    }
    return slice_sub(a, 0, w);
}

static Slice gd_filter_funcs(Slice a, DocFilter f) {
    Int w = 0;
    for (Int i = 0; i < a.len; i++) {
        DocFunc *fd = BURROW_AT(DocFunc *, a, i);
        if (BURROW_CALLF(f, fd->name)) {
            BURROW_AT(DocFunc *, a, w) = fd;
            w++;
        }
    }
    return slice_sub(a, 0, w);
}

static Slice gd_filter_types(Slice a, DocFilter f) {
    Int w = 0;
    for (Int i = 0; i < a.len; i++) {
        DocType *td = BURROW_AT(DocType *, a, i);
        Int n = 0; /* number of matches */
        if (gd_match_decl(td->decl, f)) {
            n = 1;
        } else {
            /* type name doesn't match, but we may have matching consts,
             * vars, factories or methods */
            td->consts = gd_filter_values(td->consts, f);
            td->vars = gd_filter_values(td->vars, f);
            td->funcs = gd_filter_funcs(td->funcs, f);
            td->methods = gd_filter_funcs(td->methods, f);
            n += td->consts.len + td->vars.len + td->funcs.len + td->methods.len;
        }
        if (n > 0) {
            BURROW_AT(DocType *, a, w) = td;
            w++;
        }
    }
    return slice_sub(a, 0, w);
}

void doc_package_filter(DocPackage *p, DocFilter f) {
    p->consts = gd_filter_values(p->consts, f);
    p->vars = gd_filter_values(p->vars, f);
    p->types = gd_filter_types(p->types, f);
    p->funcs = gd_filter_funcs(p->funcs, f);
    p->doc = BURROW_STR_EMPTY; /* don't show top-level package doc */
}

/* --------------------------------------------------------------- examples */

static bool gd_is_space(Byte c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Whether s[i:] starts with lit, ignoring ASCII case. Every letter of the
 * words matched here folds only to its other ASCII case, so this is all the
 * (?i) of Go's expression does for them. */
static bool gd_has_fold(Str s, Int i, const char *lit) {
    for (Int j = 0; lit[j] != 0; j++, i++) {
        if (i >= s.len)
            return false;
        Byte c = s.p[i];
        if (c >= 'A' && c <= 'Z')
            c = (Byte)(c + ('a' - 'A'));
        if (c != (Byte)lit[j])
            return false;
    }
    return true;
}

/* outputPrefix, (?i)^[[:space:]]*(unordered )?output:, giving the end of the
 * match or -1, and whether the group matched. */
static Int gd_output_prefix(Str s, bool *unordered) {
    Int i = 0;
    while (i < s.len && gd_is_space(s.p[i]))
        i++;
    *unordered = false;
    if (gd_has_fold(s, i, "unordered ") && gd_has_fold(s, i + 10, "output:")) {
        *unordered = true;
        return i + 17;
    }
    if (gd_has_fold(s, i, "output:"))
        return i + 7;
    return -1;
}

/* isTest: whether name looks like a test, example, fuzz test or benchmark,
 * prefix followed by nothing or by something that is not lower case. */
static bool gd_is_test(Str name, Str prefix) {
    if (!strings_has_prefix(name, prefix))
        return false;
    if (name.len == prefix.len) /* "Test" is ok */
        return true;
    Int size = 0;
    Rune r = utf8_decode_rune_in_string(gd_sub(name, prefix.len, name.len), &size);
    return !unicode_is_lower(r);
}

/* lastComment: the last comment group inside b, and its index. */
static AstCommentGroup *gd_last_comment(AstBlockStmt *b, Slice c, Int *index) {
    *index = 0;
    if (b == NULL)
        return NULL;
    AstCommentGroup *last = NULL;
    TokenPos pos = ast_node_pos(&b->node);
    TokenPos end = ast_node_end(&b->node);
    for (Int j = 0; j < c.len; j++) {
        AstCommentGroup *cg = BURROW_AT(AstCommentGroup *, c, j);
        if (ast_comment_group_pos(cg) < pos)
            continue;
        if (ast_comment_group_end(cg) > end)
            break;
        *index = j;
        last = cg;
    }
    return last;
}

/* exampleOutput: the output an example's comment says it prints, whether it
 * is unordered, and whether there is such a comment. */
static Str gd_example_output(Alloc *a, AstBlockStmt *b, Slice comments, bool *unordered,
                             bool *ok) {
    *unordered = false;
    *ok = false;
    Int i = 0;
    AstCommentGroup *last = gd_last_comment(b, comments, &i);
    if (last == NULL)
        return BURROW_STR_EMPTY; /* no suitable comment found */
    Str text = gd_text(a, last);
    Int end = gd_output_prefix(text, unordered);
    if (end < 0)
        return BURROW_STR_EMPTY;
    text = gd_sub(text, end, text.len);
    Int j = 0;
    while (j < text.len && text.p[j] == ' ')
        j++;
    if (j < text.len && text.p[j] == '\n')
        j++;
    *ok = true;
    return gd_sub(text, j, text.len);
}

static int gd_cmp_decl_pos(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos a = ast_decl_pos(*(AstDecl const *)x);
    TokenPos b = ast_decl_pos(*(AstDecl const *)y);
    return a < b ? -1 : a > b;
}

static int gd_cmp_comment_pos(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos a = ast_comment_group_pos(*(AstCommentGroup *const *)x);
    TokenPos b = ast_comment_group_pos(*(AstCommentGroup *const *)y);
    return a < b ? -1 : a > b;
}

static int gd_cmp_spec_pos(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos a = ast_spec_pos(*(AstSpec const *)x);
    TokenPos b = ast_spec_pos(*(AstSpec const *)y);
    return a < b ? -1 : a > b;
}

/* stripOutputComment: the Output: comment taken out of a copy of comments,
 * and a copy of body that ends where it started. */
static AstBlockStmt *gd_strip_output_comment(Alloc *a, AstBlockStmt *body,
                                             Slice *comments) {
    Int i = 0;
    AstCommentGroup *last = gd_last_comment(body, *comments, &i);
    bool unordered = false;
    if (last == NULL || gd_output_prefix(gd_text(a, last), &unordered) < 0)
        return body;
    /* Copy body and comments, as the originals may be used elsewhere. */
    AstBlockStmt *nb = (AstBlockStmt *)ast_node_new(a, AST_KIND_BLOCK_STMT);
    if (nb == NULL)
        gd_oom();
    nb->lbrace = body->lbrace;
    nb->list = body->list;
    nb->rbrace = ast_comment_group_pos(last);
    Slice old = *comments;
    Slice nc = slice_make(a, TYPE_AST_COMMENT_GROUP_PTR, old.len - 1, old.len - 1);
    if (old.len > 1 && slice_is_nil(nc))
        gd_oom();
    Int w = 0;
    for (Int j = 0; j < old.len; j++) {
        if (j != i)
            BURROW_AT(AstCommentGroup *, nc, w++) =
                BURROW_AT(AstCommentGroup *, old, j);
    }
    *comments = nc;
    return nb;
}

/* The state of findDeclsAndUnresolved, which Go keeps in a closure. */
typedef struct GdFind {
    Alloc *a;
    Map *top_decls;  /* object (Uintptr) to its top level declaration */
    Map *unresolved; /* Str to bool */
    Map *used_decls; /* declaration (Uintptr) to bool */
    Map *used_objs;  /* object (Uintptr) to bool */
    Slice dep_decls; /* of AstDecl */
} GdFind;

static bool gd_find_inspect(void *env, AstNode n);

static void gd_find_walk(GdFind *st, AstNode n) {
    if (n != NULL)
        ast_inspect(n, BURROW_FN(AstInspectFunc, gd_find_inspect, st));
}

static bool gd_find_inspect(void *env, AstNode n) {
    GdFind *st = (GdFind *)env;
    if (n == NULL)
        return true;
    switch ((int)n->kind) {
    case AST_KIND_IDENT: {
        AstIdent *e = (AstIdent *)n;
        if (e->obj == NULL && !str_eq(e->name, S("_"))) {
            gd_set_true(st->unresolved, e->name);
            return true;
        }
        Uintptr obj = gd_key(e->obj);
        AstDecl d = (AstDecl)gd_map_ptr(st->top_decls, &obj);
        if (d != NULL) {
            bool t = true;
            gd_map_set(st->used_objs, &obj, &t);
            Uintptr dk = gd_key(d);
            if (map_get(st->used_decls, &dk) == NULL) {
                gd_map_set(st->used_decls, &dk, &t);
                gd_append(st->a, &st->dep_decls, &d);
            }
        }
        return true;
    }
    case AST_KIND_SELECTOR_EXPR:
        /* For selector expressions, only inspect the left hand side. (For an
         * expression like fmt.Println, only add "fmt" to the set of
         * unresolved names, not "Println".) */
        gd_find_walk(st, ((AstSelectorExpr *)n)->x);
        return false;
    case AST_KIND_KEY_VALUE_EXPR:
        /* For key value expressions, only inspect the value as the key
         * should be resolved by the type of the composite literal. */
        gd_find_walk(st, ((AstKeyValueExpr *)n)->value);
        return false;
    default:
        return true;
    }
}

static void gd_find_field_list(GdFind *st, AstFieldList *fl) {
    if (fl == NULL)
        return;
    for (Int i = 0; i < fl->list.len; i++)
        gd_find_walk(st, BURROW_AT(AstField *, fl->list, i)->type);
}

static bool gd_has_iota_inspect(void *env, AstNode n) {
    bool *found = (bool *)env;
    /* Check that this is the special built-in "iota" identifier, not a
     * user-defined shadow. */
    if (gd_is(n, AST_KIND_IDENT) && str_eq(((AstIdent *)n)->name, S("iota")) &&
        ((AstIdent *)n)->obj == NULL)
        *found = true;
    return !*found;
}

static bool gd_has_iota(AstSpec s) {
    bool found = false;
    ast_inspect(s, BURROW_FN(AstInspectFunc, gd_has_iota_inspect, &found));
    return found;
}

static bool gd_used(Map *used_objs, AstIdent *id) {
    Uintptr k = gd_key(id->obj);
    bool *v = (bool *)map_get(used_objs, &k);
    return v != NULL && *v;
}

/* trimDecl, the end of findDeclsAndUnresolved for one GenDecl: only the specs
 * the example uses, unless it is a constant group with iota, which has to stay
 * whole. Gives NULL for none. */
static AstDecl gd_trim_gen_decl(GdFind *st, AstGenDecl *d) {
    Alloc *a = st->a;
    bool contains_iota = false; /* does any spec have iota? */
    /* Collect all Specs that were mentioned in the example. */
    Slice specs = slice_nil(TYPE_AST_SPEC);
    for (Int i = 0; i < d->specs.len; i++) {
        AstSpec s = gd_node(d->specs, i);
        if (gd_is(s, AST_KIND_TYPE_SPEC)) {
            if (gd_used(st->used_objs, ((AstTypeSpec *)s)->name))
                gd_append(a, &specs, &s);
            continue;
        }
        if (!gd_is(s, AST_KIND_VALUE_SPEC))
            continue;
        AstValueSpec *vs = (AstValueSpec *)s;
        if (!contains_iota)
            contains_iota = gd_has_iota(s);
        /* A ValueSpec may have multiple names (e.g. "var a, b int"). Keep only
         * the names that were mentioned in the example. Exception: the
         * multiple names have a single initializer (which would be a function
         * call with multiple return values). In that case, keep everything. */
        if (vs->names.len > 1 && vs->values.len == 1) {
            gd_append(a, &specs, &s);
            continue;
        }
        AstValueSpec *ns = (AstValueSpec *)gd_copy(a, vs, sizeof(AstValueSpec));
        ns->names = slice_nil(TYPE_AST_IDENT_PTR);
        ns->values = slice_nil(TYPE_AST_EXPR);
        for (Int j = 0; j < vs->names.len; j++) {
            AstIdent *n = BURROW_AT(AstIdent *, vs->names, j);
            if (gd_used(st->used_objs, n)) {
                gd_append(a, &ns->names, &n);
                if (vs->values.len > 0)
                    gd_append(a, &ns->values, &BURROW_AT(AstExpr, vs->values, j));
            }
        }
        if (ns->names.len > 0) {
            AstSpec nsp = &ns->node;
            gd_append(a, &specs, &nsp);
        }
    }
    if (specs.len == 0)
        return NULL;
    /* Constant with iota? Keep it all. */
    if (d->tok == TOKEN_CONST && contains_iota)
        return &d->node;
    /* Synthesize a GenDecl with just the Specs we need. */
    AstGenDecl *nd = (AstGenDecl *)gd_copy(a, d, sizeof(AstGenDecl));
    nd->specs = specs;
    if (specs.len == 1) {
        /* Remove grouping parens if there is only one spec. */
        nd->lparen = 0;
    }
    return &nd->node;
}

/* findDeclsAndUnresolved: the top level declarations body uses, directly or
 * not, and the names it uses that are declared nowhere. */
static Slice gd_find_decls_and_unresolved(GdFind *st, AstNode body, Map *typ_methods) {
    Alloc *a = st->a;
    st->unresolved = gd_map(a, TYPE_OF(Str), TYPE_BOOL);
    st->used_decls = gd_map(a, TYPE_UINTPTR, TYPE_BOOL);
    st->used_objs = gd_map(a, TYPE_UINTPTR, TYPE_BOOL);
    st->dep_decls = slice_nil(TYPE_AST_DECL);

    /* Find the decls immediately referenced by body. */
    gd_find_walk(st, body);

    /* Now loop over them, adding to the list when we find a new decl that the
     * body depends on. Keep going until we don't find anything new. */
    for (Int i = 0; i < st->dep_decls.len; i++) {
        AstDecl dd = gd_node(st->dep_decls, i);
        if (gd_is(dd, AST_KIND_FUNC_DECL)) {
            AstFuncDecl *d = (AstFuncDecl *)dd;
            /* Inspect type parameters. */
            gd_find_field_list(st, d->type->type_params);
            /* Inspect types of parameters and results. See #28492. */
            gd_find_field_list(st, d->type->params);
            gd_find_field_list(st, d->type->results);
            /* Functions might not have a body. See #42706. */
            if (d->body != NULL)
                gd_find_walk(st, &d->body->node);
        } else if (gd_is(dd, AST_KIND_GEN_DECL)) {
            AstGenDecl *d = (AstGenDecl *)dd;
            for (Int j = 0; j < d->specs.len; j++) {
                AstSpec spec = gd_node(d->specs, j);
                if (gd_is(spec, AST_KIND_TYPE_SPEC)) {
                    AstTypeSpec *s = (AstTypeSpec *)spec;
                    gd_find_field_list(st, s->type_params);
                    gd_find_walk(st, s->type);
                    Slice *methods = (Slice *)gd_map_ptr(typ_methods, &s->name->name);
                    if (methods != NULL)
                        gd_append_slice(a, &st->dep_decls, *methods);
                } else if (gd_is(spec, AST_KIND_VALUE_SPEC)) {
                    AstValueSpec *s = (AstValueSpec *)spec;
                    if (s->type != NULL)
                        gd_find_walk(st, s->type);
                    for (Int k = 0; k < s->values.len; k++)
                        gd_find_walk(st, gd_node(s->values, k));
                }
            }
        }
    }

    /* Some decls include multiple specs, such as a variable declaration with
     * multiple variables on the same line, or a parenthesized declaration.
     * Trim the declarations to include only the specs that are actually
     * mentioned. However, if there is a constant group with iota, leave it
     * all: later constant declarations in the group may have no value and so
     * cannot stand on their own, and removing any constant from the group
     * could change the values of subsequent ones. See
     * testdata/examples/iota.go for a minimal example. */
    Slice ds = slice_nil(TYPE_AST_DECL);
    for (Int i = 0; i < st->dep_decls.len; i++) {
        AstDecl d = gd_node(st->dep_decls, i);
        if (gd_is(d, AST_KIND_FUNC_DECL)) {
            gd_append(a, &ds, &d);
        } else if (gd_is(d, AST_KIND_GEN_DECL)) {
            AstDecl nd = gd_trim_gen_decl(st, (AstGenDecl *)d);
            if (nd != NULL)
                gd_append(a, &ds, &nd);
        }
    }
    return ds;
}

/* findImportGroupStarts1: the first import of each run of them with no blank
 * line in between, assuming the file is gofmt'ed. */
Slice burrow__doc_find_import_group_starts1(Alloc *a, Slice orig_imps);
Slice burrow__doc_find_import_group_starts1(Alloc *a, Slice orig_imps) {
    /* Copy to avoid mutation. */
    Slice imps = slice_make(a, TYPE_AST_SPEC, orig_imps.len, orig_imps.len);
    if (orig_imps.len > 0 && slice_is_nil(imps))
        gd_oom();
    for (Int i = 0; i < orig_imps.len; i++)
        BURROW_AT(AstSpec, imps, i) = gd_node(orig_imps, i);
    /* Assume the imports are sorted by position. */
    slices_sort_func(imps, (SlicesCmpFunc){gd_cmp_spec_pos, NULL});
    /* Assume gofmt has been applied, so there is a blank line between
     * adjacent imps if and only if they are more than 2 positions apart
     * (newline, tab). */
    Slice group_starts = slice_nil(TYPE_AST_IMPORT_SPEC_PTR);
    TokenPos prev_end = -2;
    for (Int i = 0; i < imps.len; i++) {
        AstImportSpec *imp = (AstImportSpec *)gd_node(imps, i);
        if (ast_spec_pos(&imp->node) - prev_end > 2)
            gd_append(a, &group_starts, &imp);
        prev_end = ast_node_end(&imp->node);
        /* Account for end-of-line comments. */
        if (imp->comment != NULL)
            prev_end = ast_comment_group_end(imp->comment);
    }
    return group_starts;
}

/* updateBasicLitPos: lit moved to pos, its end with it. */
static void gd_update_basic_lit_pos(AstBasicLit *lit, TokenPos pos) {
    TokenPos len = ast_node_end(&lit->node) - ast_node_pos(&lit->node);
    lit->value_pos = pos;
    if (token_pos_is_valid(lit->value_end))
        lit->value_end = pos + len;
}

static void gd_top_decl(Alloc *a, Map *top_decls, AstIdent *name, AstDecl d) {
    (void)a;
    Uintptr k = gd_key(name->obj);
    gd_map_set(top_decls, &k, &d);
}

/* playExample: a whole program version of the example f, or NULL when one
 * cannot be made. */
static AstFile *gd_play_example(Alloc *a, AstFile *file, AstFuncDecl *f) {
    AstBlockStmt *body = f->body;

    if (!strings_has_suffix(file->name->name, S("_test"))) {
        /* We don't support examples that are part of the greater package
         * (yet). */
        return NULL;
    }

    /* Collect top-level declarations in the file. */
    Map *top_decls = gd_map(a, TYPE_UINTPTR, TYPE_UNSAFE_POINTER);
    Map *typ_methods = gd_map(a, TYPE_OF(Str), TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl decl = gd_node(file->decls, i);
        if (gd_is(decl, AST_KIND_FUNC_DECL)) {
            AstFuncDecl *d = (AstFuncDecl *)decl;
            if (d->recv == NULL) {
                gd_top_decl(a, top_decls, d->name, decl);
            } else if (d->recv->list.len == 1) {
                bool imp = false;
                Str tname = gd_base_type_name(
                    BURROW_AT(AstField *, d->recv->list, 0)->type, &imp);
                Slice *list = (Slice *)gd_map_ptr(typ_methods, &tname);
                if (list == NULL) {
                    list = GD_NEW(a, Slice);
                    *list = slice_nil(TYPE_AST_DECL);
                    gd_map_set(typ_methods, &tname, &list);
                }
                gd_append(a, list, &decl);
            }
        } else if (gd_is(decl, AST_KIND_GEN_DECL)) {
            AstGenDecl *d = (AstGenDecl *)decl;
            for (Int j = 0; j < d->specs.len; j++) {
                AstSpec spec = gd_node(d->specs, j);
                if (gd_is(spec, AST_KIND_TYPE_SPEC)) {
                    gd_top_decl(a, top_decls, ((AstTypeSpec *)spec)->name, decl);
                } else if (gd_is(spec, AST_KIND_VALUE_SPEC)) {
                    Slice names = ((AstValueSpec *)spec)->names;
                    for (Int k = 0; k < names.len; k++)
                        gd_top_decl(a, top_decls, BURROW_AT(AstIdent *, names, k),
                                    decl);
                }
            }
        }
    }

    /* Find unresolved identifiers and uses of top-level declarations. */
    GdFind st;
    memset(&st, 0, sizeof st);
    st.a = a;
    st.top_decls = top_decls;
    Slice dep_decls = gd_find_decls_and_unresolved(&st, &body->node, typ_methods);
    Map *unresolved = st.unresolved;

    /* Use unresolved identifiers to determine the imports used by this
     * example. The heuristic assumes package names match base import paths
     * for imports w/o renames (should be good enough most of the time). */
    Slice named_imports = slice_nil(TYPE_AST_SPEC);
    Slice blank_imports = slice_nil(TYPE_AST_SPEC); /* _ imports */

    /* To preserve the blank lines between groups of imports, find the start
     * position of each group, and assign that position to all imports from
     * that group. */
    Slice starts = burrow__doc_find_import_group_starts1(a, file->imports);

    for (Int i = 0; i < file->imports.len; i++) {
        AstImportSpec *s = BURROW_AT(AstImportSpec *, file->imports, i);
        Error err = BURROW_NO_ERROR;
        Str p = strconv_unquote(a, s->path->value, &err);
        if (BURROW_FAILED(err))
            continue;
        if (str_eq(p, S("syscall/js"))) {
            /* We don't support examples that import syscall/js, because the
             * package syscall/js is not available in the playground. */
            return NULL;
        }
        Str n = gd_assumed_package_name(a, p);
        if (s->name != NULL) {
            n = s->name->name;
            if (str_eq(n, S("_"))) {
                AstSpec sp = &s->node;
                gd_append(a, &blank_imports, &sp);
                continue;
            }
            if (str_eq(n, S("."))) {
                /* We can't resolve dot imports (yet). */
                return NULL;
            }
        }
        if (gd_map_bool(unresolved, n)) {
            /* Copy the spec and its path to avoid modifying the original. */
            AstImportSpec *spec = (AstImportSpec *)gd_copy(a, s, sizeof(AstImportSpec));
            spec->path = (AstBasicLit *)gd_copy(a, s->path, sizeof(AstBasicLit));
            /* groupStart: the start of the group spec is in */
            TokenPos start = 0;
            for (Int j = 0; j < starts.len; j++) {
                TokenPos sj = ast_spec_pos(gd_node(starts, j));
                if (spec->path->value_pos < sj)
                    break;
                start = sj;
            }
            gd_update_basic_lit_pos(spec->path, start);
            AstSpec sp = &spec->node;
            gd_append(a, &named_imports, &sp);
            map_del(unresolved, &n);
        }
    }

    /* Remove predeclared identifiers from unresolved list, and if there are
     * other unresolved identifiers, give up because this synthesized file is
     * not going to build. */
    MapIter it = map_iter(unresolved);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        Str n = *(const Str *)k;
        if (!gd_predeclared_type(n) && !gd_predeclared_constant(n) &&
            !gd_predeclared_func(n))
            return NULL;
    }

    /* Include documentation belonging to blank imports. */
    Slice comments = slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    for (Int i = 0; i < blank_imports.len; i++) {
        AstImportSpec *s = (AstImportSpec *)gd_node(blank_imports, i);
        if (s->doc != NULL)
            gd_append(a, &comments, &s->doc);
    }

    /* Include comments that are inside the function body. */
    TokenPos bpos = ast_node_pos(&body->node);
    TokenPos bend = ast_node_end(&body->node);
    for (Int i = 0; i < file->comments.len; i++) {
        AstCommentGroup *c = BURROW_AT(AstCommentGroup *, file->comments, i);
        if (bpos <= ast_comment_group_pos(c) && ast_comment_group_end(c) <= bend)
            gd_append(a, &comments, &c);
    }

    /* Strip the "Output:" or "Unordered output:" comment and adjust body end
     * position. */
    body = gd_strip_output_comment(a, body, &comments);

    /* Include documentation belonging to dependent declarations. */
    for (Int i = 0; i < dep_decls.len; i++) {
        AstDecl d = gd_node(dep_decls, i);
        AstCommentGroup *doc = NULL;
        if (gd_is(d, AST_KIND_GEN_DECL))
            doc = ((AstGenDecl *)d)->doc;
        else if (gd_is(d, AST_KIND_FUNC_DECL))
            doc = ((AstFuncDecl *)d)->doc;
        if (doc != NULL)
            gd_append(a, &comments, &doc);
    }

    /* Synthesize import declaration. */
    AstGenDecl *import_decl = (AstGenDecl *)ast_node_new(a, AST_KIND_GEN_DECL);
    if (import_decl == NULL)
        gd_oom();
    import_decl->tok = TOKEN_IMPORT;
    /* Need non-zero lparen and rparen so that printer treats this as a
     * factored import. */
    import_decl->lparen = 1;
    import_decl->rparen = 1;
    import_decl->specs = named_imports;
    gd_append_slice(a, &import_decl->specs, blank_imports);

    /* Synthesize main function. */
    AstFuncDecl *func_decl = (AstFuncDecl *)ast_node_new(a, AST_KIND_FUNC_DECL);
    AstIdent *main_name = ast_new_ident(a, S("main"));
    if (func_decl == NULL || main_name == NULL)
        gd_oom();
    func_decl->name = main_name;
    func_decl->type = f->type;
    func_decl->body = body;

    Slice decls = slice_make(a, TYPE_AST_DECL, 0, 2 + dep_decls.len);
    if (slice_is_nil(decls))
        gd_oom();
    AstDecl id = &import_decl->node;
    gd_append(a, &decls, &id);
    gd_append_slice(a, &decls, dep_decls);
    AstDecl fd = &func_decl->node;
    gd_append(a, &decls, &fd);

    slices_sort_func(decls, (SlicesCmpFunc){gd_cmp_decl_pos, NULL});
    slices_sort_func(comments, (SlicesCmpFunc){gd_cmp_comment_pos, NULL});

    /* Synthesize file. */
    AstFile *out = (AstFile *)ast_node_new(a, AST_KIND_FILE);
    AstIdent *pkg_name = ast_new_ident(a, S("main"));
    if (out == NULL || pkg_name == NULL)
        gd_oom();
    out->name = pkg_name;
    out->decls = decls;
    out->comments = comments;
    return out;
}

/* playExampleFile: a whole file example made into package main, with the
 * ExampleX function renamed main. */
static AstFile *gd_play_example_file(Alloc *a, AstFile *file) {
    /* Strip copyright comment if present. */
    Slice comments = file->comments;
    if (comments.len > 0 &&
        strings_has_prefix(gd_text(a, BURROW_AT(AstCommentGroup *, comments, 0)),
                           S("Copyright")))
        comments = slice_sub(comments, 1, comments.len);

    /* Copy declaration slice, rewriting the ExampleX function to main. */
    Slice decls = slice_nil(TYPE_AST_DECL);
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl d = gd_node(file->decls, i);
        if (gd_is(d, AST_KIND_FUNC_DECL) &&
            gd_is_test(((AstFuncDecl *)d)->name->name, S("Example"))) {
            AstFuncDecl *f = (AstFuncDecl *)d;
            /* Copy the FuncDecl, as it may be used elsewhere. */
            AstFuncDecl *nf = (AstFuncDecl *)gd_copy(a, f, sizeof(AstFuncDecl));
            nf->name = ast_new_ident(a, S("main"));
            if (nf->name == NULL)
                gd_oom();
            nf->body = gd_strip_output_comment(a, f->body, &comments);
            d = &nf->node;
        }
        gd_append(a, &decls, &d);
    }

    /* Copy the File, as it may be used elsewhere. */
    AstFile *f = (AstFile *)gd_copy(a, file, sizeof(AstFile));
    f->name = ast_new_ident(a, S("main"));
    if (f->name == NULL)
        gd_oom();
    f->decls = decls;
    f->comments = comments;
    return f;
}

static int gd_cmp_example_name(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp((*(DocExample *const *)x)->name, (*(DocExample *const *)y)->name);
}

static int gd_cmp_example_suffix(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp((*(DocExample *const *)x)->suffix,
                   (*(DocExample *const *)y)->suffix);
}

Slice doc_examples(Alloc *a, Slice test_files) {
    Slice list = slice_nil(TYPE_DOC_EXAMPLE_PTR);
    for (Int fi = 0; fi < test_files.len; fi++) {
        AstFile *file = BURROW_AT(AstFile *, test_files, fi);
        bool has_tests = false; /* file contains tests, fuzz test, or benchmarks */
        Int num_decl = 0;       /* number of non-import declarations in the file */
        Slice flist = slice_nil(TYPE_DOC_EXAMPLE_PTR);
        for (Int i = 0; i < file->decls.len; i++) {
            AstDecl decl = gd_node(file->decls, i);
            if (gd_is(decl, AST_KIND_GEN_DECL) &&
                ((AstGenDecl *)decl)->tok != TOKEN_IMPORT) {
                num_decl++;
                continue;
            }
            if (!gd_is(decl, AST_KIND_FUNC_DECL))
                continue;
            AstFuncDecl *f = (AstFuncDecl *)decl;
            if (f->recv != NULL)
                continue;
            num_decl++;
            Str name = f->name->name;
            if (gd_is_test(name, S("Test")) || gd_is_test(name, S("Benchmark")) ||
                gd_is_test(name, S("Fuzz"))) {
                has_tests = true;
                continue;
            }
            if (!gd_is_test(name, S("Example")))
                continue;
            if (f->type->params != NULL && f->type->params->list.len != 0)
                continue; /* function has params; not a valid example */
            if (f->type->results != NULL && f->type->results->list.len != 0)
                continue;        /* function has results; not a valid example */
            if (f->body == NULL) /* ast.File.Body nil dereference (see issue 28044) */
                continue;
            DocExample *ex = GD_NEW(a, DocExample);
            ex->name = gd_sub(name, 7, name.len);
            if (f->doc != NULL)
                ex->doc = gd_text(a, f->doc);
            bool has_output = false;
            ex->output = gd_example_output(a, f->body, file->comments, &ex->unordered,
                                           &has_output);
            ex->code = &f->body->node;
            ex->play = gd_play_example(a, file, f);
            ex->comments = file->comments;
            ex->empty_output = ex->output.len == 0 && has_output;
            ex->order = flist.len;
            gd_append(a, &flist, &ex);
        }
        if (!has_tests && num_decl > 1 && flist.len == 1) {
            DocExample *ex = BURROW_AT(DocExample *, flist, 0);
            ex->code = &file->node;
            ex->play = gd_play_example_file(a, file);
        }
        gd_append_slice(a, &list, flist);
    }
    slices_sort_func(list, (SlicesCmpFunc){gd_cmp_example_name, NULL});
    return list;
}

/* nameWithoutInst: name with everything from the first '[' to the last ']'
 * taken out. */
static Str gd_name_without_inst(Alloc *a, Str name) {
    Int start = strings_index_byte(name, '[');
    if (start < 0)
        return name;
    Int end = strings_last_index_byte(name, ']');
    if (end < 0) {
        /* Malformed name, should contain closing bracket too. */
        return name;
    }
    return gd_concat3(a, gd_sub(name, 0, start), gd_sub(name, end + 1, name.len),
                      BURROW_STR_EMPTY);
}

/* isExampleSuffix: whether s starts with a lower case letter. */
static bool gd_is_example_suffix(Str s) {
    Int size = 0;
    Rune r = utf8_decode_rune_in_string(s, &size);
    return size > 0 && unicode_is_lower(r);
}

/* splitExampleName: s split at i into a prefix and a suffix, which must start
 * with a lower case letter after the '_' at i, or be absent when i is the
 * end. */
static bool gd_split_example_name(Str s, Int i, Str *prefix, Str *suffix) {
    *prefix = BURROW_STR_EMPTY;
    *suffix = BURROW_STR_EMPTY;
    if (i == s.len) {
        *prefix = s;
        return true;
    }
    if (i == s.len - 1)
        return false;
    *prefix = gd_sub(s, 0, i);
    *suffix = gd_sub(s, i + 1, s.len);
    return gd_is_example_suffix(*suffix);
}

static void gd_example_id(Map *ids, Str name, Slice *exs) {
    gd_map_set(ids, &name, &exs);
}

/* classifyExamples: each example added to the Examples of the function, type,
 * method or package it is named after. */
static void gd_classify_examples(Alloc *a, DocPackage *p, Slice examples) {
    if (examples.len == 0)
        return;

    /* Mapping of names for funcs, types, and methods to the example
     * listing. */
    Map *ids = gd_map(a, TYPE_OF(Str), TYPE_UNSAFE_POINTER);
    gd_example_id(ids, BURROW_STR_EMPTY, &p->examples); /* package-level examples */
    for (Int i = 0; i < p->funcs.len; i++) {
        DocFunc *f = BURROW_AT(DocFunc *, p->funcs, i);
        if (token_is_exported(f->name))
            gd_example_id(ids, f->name, &f->examples);
    }
    for (Int i = 0; i < p->types.len; i++) {
        DocType *t = BURROW_AT(DocType *, p->types, i);
        if (!token_is_exported(t->name))
            continue;
        gd_example_id(ids, t->name, &t->examples);
        for (Int j = 0; j < t->funcs.len; j++) {
            DocFunc *f = BURROW_AT(DocFunc *, t->funcs, j);
            if (token_is_exported(f->name))
                gd_example_id(ids, f->name, &f->examples);
        }
        for (Int j = 0; j < t->methods.len; j++) {
            DocFunc *m = BURROW_AT(DocFunc *, t->methods, j);
            if (!token_is_exported(m->name))
                continue;
            Str recv = strings_trim_prefix(gd_name_without_inst(a, m->recv), S("*"));
            gd_example_id(ids, gd_concat3(a, recv, S("_"), m->name), &m->examples);
        }
    }

    /* Group each example with the associated func, type, or method. */
    for (Int e = 0; e < examples.len; e++) {
        DocExample *ex = BURROW_AT(DocExample *, examples, e);
        /* Consider all possible split points for the suffix by starting at
         * the end of string (no suffix case), then trying all positions that
         * contain a '_' character.
         *
         * An association is made on the first successful match. Examples
         * with malformed names that match nothing are skipped. */
        for (Int i = ex->name.len; i >= 0;
             i = strings_last_index_byte(gd_sub(ex->name, 0, i), '_')) {
            Str prefix = BURROW_STR_EMPTY;
            Str suffix = BURROW_STR_EMPTY;
            if (!gd_split_example_name(ex->name, i, &prefix, &suffix))
                continue;
            Slice *exs = (Slice *)gd_map_ptr(ids, &prefix);
            if (exs == NULL)
                continue;
            ex->suffix = suffix;
            gd_append(a, exs, &ex);
            break;
        }
    }

    /* Sort list of example according to the user-specified suffix name. */
    MapIter it = map_iter(ids);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v))
        slices_sort_func(**(Slice **)v, (SlicesCmpFunc){gd_cmp_example_suffix, NULL});
}

DocPackage *doc_new_from_files(Alloc *a, TokenFileSet *fset, Slice files,
                               Str import_path, DocMode mode, Error *err) {
    *err = BURROW_NO_ERROR;
    /* Check for invalid API usage. */
    if (fset == NULL)
        panic_str(S("doc.NewFromFiles: no token.FileSet provided (fset == nil)"));

    /* Collect .go and _test.go files. */
    Str pkg_name = BURROW_STR_EMPTY;
    Map *go_files = gd_map(a, TYPE_OF(Str), TYPE_AST_FILE_PTR);
    Slice test_go_files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < files.len; i++) {
        AstFile *file = BURROW_AT(AstFile *, files, i);
        TokenFile *f = token_file_set_file(fset, ast_file_pos(file));
        if (f == NULL) {
            *err =
                fmt_errorf_v("file files[%d] is not found in the provided file set", i);
            return NULL;
        }
        Str filename = token_file_name(f);
        if (strings_has_suffix(filename, S("_test.go"))) {
            gd_append(a, &test_go_files, &file);
        } else if (strings_has_suffix(filename, S(".go"))) {
            pkg_name = file->name->name;
            gd_map_set(go_files, &filename, &file);
        } else {
            *err =
                fmt_errorf_v("file files[%d] filename %q does not have a .go extension",
                             i, filename);
            return NULL;
        }
    }

    /* Compute package documentation.
     *
     * Since this package doesn't need Package.{Scope,Imports}, or handle
     * errors, and AstFile's scope field is unset in files parsed with
     * PARSER_SKIP_OBJECT_RESOLUTION, we construct the Package directly
     * instead of calling ast_new_package. */
    AstPackage *pkg = (AstPackage *)ast_node_new(a, AST_KIND_PACKAGE);
    if (pkg == NULL)
        gd_oom();
    pkg->name = pkg_name;
    pkg->files = go_files;
    DocPackage *p = doc_new(a, pkg, import_path, mode);
    gd_classify_examples(a, p, doc_examples(a, test_go_files));
    return p;
}
