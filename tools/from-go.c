/* from-go: C declarations and type descriptors from Go declarations, for
 * burrow-gen from-go.
 *
 *     from-go [-o file] [-header file] [-prefix name] file.go ...
 *
 * The files are one Go package. Its types become C types with descriptors,
 * declared with the macros in burrow/declare.h, its exported constants become
 * macros, and its exported functions and methods become prototypes for you to
 * write in C. Nothing is run and no Go toolchain is needed: the files are read
 * with burrow's own go/parser, and go/types works out constant values and
 * array lengths.
 *
 * Everything goes to -o, or to standard output, as one file that both declares
 * and defines. With -header the declarations go to that file instead and the
 * -o file includes it and keeps only the definitions, which is the split a
 * package used from more than one C file needs.
 *
 * Names follow burrow's own: a type is the prefix and its Go name, so Item in
 * package inventory is InventoryItem, a function is inventory_new_shelf and a
 * constant is INVENTORY_MAX_ITEMS. The prefix is the package name, none for
 * package main, or whatever -prefix says, and -prefix "" turns it off.
 *
 * A declaration with a type C cannot hold the way burrow holds it, which is a
 * channel, a function, an interface with methods or anything generic, is an
 * error that names it, and then nothing is written.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

/* What a GenType is. */
enum {
    GT_BUILTIN, /* one burrow already has a descriptor for */
    GT_STRUCT,
    GT_BASIC, /* a named type over a basic one, type Kind int */
    GT_SLICE,
    GT_ARRAY,
    GT_PTR,
    GT_MAP,
    GT_ALIAS, /* type A = B */
};

typedef struct GenType GenType;

typedef struct GenField {
    Str cname;  /* the C member */
    Str goname; /* the name in the descriptor */
    Str tag;    /* unquoted */
    GenType *type;
} GenField;

struct GenType {
    int what;
    Str cname;        /* the C spelling, the name TYPE_OF pastes */
    Str comp;         /* what stands for it in a composite's name: Int, Item */
    Str goname;       /* a declared type's Go name, empty for a composite */
    const char *kind; /* KIND_ of a builtin or of a GT_BASIC, or NULL */
    bool declared;    /* from a type declaration rather than made up */
    GenType *elem;    /* element, pointee, map value, alias target, basic type */
    GenType *key;
    int64_t len;
    AstTypeSpec *spec;
    AstCommentGroup *doc;
    Slice fields; /* of GenField * */
    int state;    /* 0 not yet worked out, 1 being worked out, 2 done */
    int mark;     /* the same for the order of the declarations */
};

typedef struct Gen {
    Alloc *a;
    TokenFileSet *fset;
    TypesInfo info;
    Str pkg;
    Str tprefix;    /* Inventory */
    Str fprefix;    /* inventory_ */
    Str cprefix;    /* INVENTORY_ */
    Slice builtins; /* of GenType * */
    Slice types;    /* of GenType *, declared and made up */
    Slice order;    /* of GenType *, as the declarations have to come */
    Slice roots;    /* of GenType *, the ones that have to be declared */
    int errors;
    IoWriter err;
} Gen;

/* ------------------------------------------------------------------ helpers */

static Slice push(Gen *g, Slice s, void *p) {
    return slice_append(g->a, s, &p, 1);
}

static void *at(Slice s, Int i) {
    return ((void **)s.p)[i];
}

static Str cat(Gen *g, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc(g->a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str cat3(Gen *g, Str x, Str y, Str z) {
    return cat(g, cat(g, x, y), z);
}

static Str cstr(const char *s) {
    return str_from_cstr(s);
}

static Str itoa(Gen *g, int64_t n) {
    return strconv_format_int(g->a, n, 10);
}

static void put(BytesBuffer *b, Str s) {
    (void)bytes_buffer_write_string(b, s, NULL);
}

static void puts_(BytesBuffer *b, const char *s) {
    put(b, str_from_cstr(s));
}

static void put_buf(BytesBuffer *b, BytesBuffer *from) {
    Slice p = bytes_buffer_bytes(from);
    put(b, str_from_bytes(p.p, p.len));
}

static void gen_error(Gen *g, TokenPos pos, Str msg) {
    TokenPosition p = token_file_set_position(g->fset, pos);
    (void)fmt_fprintf_v(g->err, "%s: %s\n", token_position_string(p, g->a), msg);
    g->errors++;
}

static bool is_upper(Byte c) {
    return c >= 'A' && c <= 'Z';
}
static bool is_lower(Byte c) {
    return c >= 'a' && c <= 'z';
}
static bool is_digit(Byte c) {
    return c >= '0' && c <= '9';
}

/* NewShelf to new_shelf, UserID to user_id, HTTPServer to http_server. An
 * underscore goes in before a capital that follows a small letter or a digit,
 * and before the last capital of a run when a small letter follows it. Names
 * outside ASCII come back empty, for the caller to complain about. */
static Str snake(Gen *g, Str s) {
    Byte *p = (Byte *)mem_alloc(g->a, (size_t)s.len * 2 + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= 0x80)
            return BURROW_STR_EMPTY;
        if (is_upper(c)) {
            if (i > 0 && (is_lower(s.p[i - 1]) || is_digit(s.p[i - 1])))
                p[n++] = '_';
            else if (i > 0 && is_upper(s.p[i - 1]) && i + 1 < s.len &&
                     is_lower(s.p[i + 1]))
                p[n++] = '_';
            c = (Byte)(c - 'A' + 'a');
        }
        p[n++] = c;
    }
    return str_from_bytes(p, n);
}

static Str upper(Gen *g, Str s) {
    return strings_to_upper(g->a, s);
}

/* Words C or its standard headers already use, which a Go name can turn into.
 * A trailing underscore keeps them apart. */
static const char *const c_reserved[] = {
    "alignas",  "alignof",  "asm",           "assert",   "auto",
    "bool",     "break",    "case",          "char",     "complex",
    "const",    "continue", "default",       "do",       "double",
    "else",     "enum",     "errno",         "extern",   "false",
    "float",    "for",      "goto",          "if",       "imaginary",
    "inline",   "int",      "long",          "noreturn", "offsetof",
    "register", "restrict", "return",        "short",    "signed",
    "sizeof",   "static",   "static_assert", "stderr",   "stdin",
    "stdout",   "struct",   "switch",        "true",     "typedef",
    "union",    "unsigned", "void",          "volatile", "while",
};

static Str c_ident(Gen *g, Str s) {
    for (size_t i = 0; i < sizeof c_reserved / sizeof c_reserved[0]; i++) {
        if (str_eq(s, cstr(c_reserved[i])))
            return cat(g, s, S("_"));
    }
    return s;
}

/* A C string literal holding s, with octal escapes for anything that is not
 * printable ASCII. They are always three digits, so a digit after one cannot
 * be read as part of it. */
static Str c_quote(Gen *g, Str s) {
    BytesBuffer b = BYTES_BUFFER(g->a);
    puts_(&b, "\"");
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == '"' || c == '\\') {
            Byte e[2] = {'\\', c};
            put(&b, str_from_bytes(e, 2));
        } else if (c == '?' && i > 0 && s.p[i - 1] == '?') {
            puts_(&b, "\\?"); /* not a trigraph */
        } else if (c < 0x20 || c >= 0x7f) {
            Byte e[4] = {'\\', (Byte)('0' + (c >> 6)), (Byte)('0' + ((c >> 3) & 7)),
                         (Byte)('0' + (c & 7))};
            put(&b, str_from_bytes(e, 4));
        } else {
            put(&b, str_from_bytes(&s.p[i], 1));
        }
    }
    puts_(&b, "\"");
    return bytes_buffer_string(&b, g->a);
}

/* A Go doc comment as a C one, or nothing for no comment. */
static void put_doc(Gen *g, BytesBuffer *b, AstCommentGroup *doc, const char *indent) {
    if (doc == NULL)
        return;
    Str text = strings_trim_right(ast_comment_group_text(doc, g->a), S("\n"));
    if (text.len == 0)
        return;
    text = strings_replace_all(g->a, text, S("*/"), S("* /"));
    Slice lines = strings_split(g->a, text, S("\n"));
    for (Int i = 0; i < lines.len; i++) {
        Str line = ((Str *)lines.p)[i];
        puts_(b, indent);
        puts_(b, i == 0 ? "/* " : line.len > 0 ? " * " : " *");
        put(b, line);
        puts_(b, i == lines.len - 1 ? " */\n" : "\n");
    }
}

/* ----------------------------------------------------------------- the types */

static GenType *new_type(Gen *g, int what, Str cname, Str comp) {
    GenType *t = BURROW_NEW(g->a, GenType);
    if (t == NULL)
        return NULL;
    t->what = what;
    t->cname = cname;
    t->comp = comp;
    t->fields = slice_make(g->a, TYPE_UNSAFE_POINTER, 0, 0);
    return t;
}

typedef struct GenBuiltin {
    const char *go;
    const char *c;
    const char *comp;
    const char *kind;
} GenBuiltin;

/* The Go types burrow has descriptors for, by the name Go code writes. The
 * three with a package are matched on the package name the file uses. */
static const GenBuiltin gen_builtins[] = {
    {"bool", "bool", "Bool", "KIND_BOOL"},
    {"int", "Int", "Int", "KIND_INT"},
    {"int8", "int8_t", "Int8", "KIND_INT8"},
    {"int16", "int16_t", "Int16", "KIND_INT16"},
    {"int32", "int32_t", "Int32", "KIND_INT32"},
    {"int64", "int64_t", "Int64", "KIND_INT64"},
    {"uint", "Uint", "Uint", "KIND_UINT"},
    {"uint8", "uint8_t", "Uint8", "KIND_UINT8"},
    {"uint16", "uint16_t", "Uint16", "KIND_UINT16"},
    {"uint32", "uint32_t", "Uint32", "KIND_UINT32"},
    {"uint64", "uint64_t", "Uint64", "KIND_UINT64"},
    {"uintptr", "Uintptr", "Uintptr", "KIND_UINTPTR"},
    {"float32", "float", "Float32", "KIND_FLOAT32"},
    {"float64", "double", "Float64", "KIND_FLOAT64"},
    {"complex64", "Complex64", "Complex64", "KIND_COMPLEX64"},
    {"complex128", "Complex128", "Complex128", "KIND_COMPLEX128"},
    {"string", "Str", "Str", "KIND_STRING"},
    {"byte", "Byte", "Byte", "KIND_UINT8"},
    {"rune", "Rune", "Rune", "KIND_INT32"},
    {"error", "Error", "Error", NULL},
    {"any", "Any", "Any", NULL},
    {"time.Time", "Time", "Time", NULL},
    {"time.Duration", "Duration", "Duration", "KIND_INT64"},
    {"unsafe.Pointer", "UnsafePointer", "UnsafePointer", "KIND_UNSAFE_POINTER"},
    {"[]byte", "Bytes", "Bytes", NULL},
};

static GenType *builtin(Gen *g, Str go) {
    for (Int i = 0; i < g->builtins.len; i++) {
        if (str_eq(go, cstr(gen_builtins[i].go)))
            return (GenType *)at(g->builtins, i);
    }
    return NULL;
}

static GenType *find_declared(Gen *g, Str goname) {
    for (Int i = 0; i < g->types.len; i++) {
        GenType *t = (GenType *)at(g->types, i);
        if (t->declared && str_eq(t->goname, goname))
            return t;
    }
    return NULL;
}

/* A made up composite, the same one each time it is asked for. */
static GenType *composite(Gen *g, TokenPos pos, int what, Str comp, GenType *elem,
                          GenType *key, int64_t len) {
    Str cname = cat(g, g->tprefix, comp);
    for (Int i = 0; i < g->types.len; i++) {
        GenType *t = (GenType *)at(g->types, i);
        if (!str_eq(t->cname, cname))
            continue;
        if (!t->declared)
            return t;
        gen_error(g, pos,
                  fmt_sprintf_v(g->a,
                                "the type made up for this would be called %s, "
                                "which is already the name of a declared type",
                                cname));
        return NULL;
    }
    GenType *t = new_type(g, what, cname, comp);
    if (t == NULL)
        return NULL;
    t->elem = elem;
    t->key = key;
    t->len = len;
    t->state = 2;
    g->types = push(g, g->types, t);
    return t;
}

static int64_t array_len(Gen *g, AstExpr len, bool *ok) {
    TypesTypeAndValue tv;
    *ok = false;
    if (!map_get2(g->info.types, &len, &tv) ||
        constant_value_kind(tv.value) != CONSTANT_INT)
        return 0;
    bool exact = false;
    int64_t n = constant_int64_val(tv.value, &exact);
    *ok = exact && n >= 0 && n <= UINT32_MAX;
    return n;
}

static Str expr_string(Gen *g, AstExpr e) {
    BytesBuffer b = BYTES_BUFFER(g->a);
    (void)printer_fprint(g->a, bytes_buffer_as_io_writer(&b), g->fset,
                         BURROW_ANY(TYPE_AST_EXPR, &e));
    return bytes_buffer_string(&b, g->a);
}

static void unsupported(Gen *g, AstExpr e, const char *why) {
    gen_error(g, ast_node_pos((AstNode)e),
              fmt_sprintf_v(g->a, "%s: %s", expr_string(g, e), cstr(why)));
}

/* The GenType a type expression stands for, or NULL after an error. */
static GenType *resolve(Gen *g, AstExpr e) {
    switch (e->kind) {
    case AST_KIND_PAREN_EXPR:
        return resolve(g, ((AstParenExpr *)e)->x);
    case AST_KIND_IDENT: {
        Str name = ((AstIdent *)e)->name;
        GenType *t = find_declared(g, name);
        if (t == NULL)
            t = builtin(g, name);
        if (t == NULL)
            unsupported(g, e, "not a type from-go knows");
        return t;
    }
    case AST_KIND_SELECTOR_EXPR: {
        GenType *t = builtin(g, expr_string(g, e));
        if (t == NULL)
            unsupported(g, e,
                        "only time.Time, time.Duration and unsafe.Pointer can "
                        "come from another package");
        return t;
    }
    case AST_KIND_STAR_EXPR: {
        GenType *elem = resolve(g, ((AstStarExpr *)e)->x);
        if (elem == NULL)
            return NULL;
        return composite(g, ast_node_pos((AstNode)e), GT_PTR,
                         cat(g, elem->comp, S("Ptr")), elem, NULL, 0);
    }
    case AST_KIND_ARRAY_TYPE: {
        AstArrayType *at_ = (AstArrayType *)e;
        GenType *elem = resolve(g, at_->elt);
        if (elem == NULL)
            return NULL;
        if (at_->len == NULL) {
            if (str_eq(elem->cname, S("Byte")) || str_eq(elem->cname, S("uint8_t")))
                return builtin(g, S("[]byte"));
            return composite(g, ast_node_pos((AstNode)e), GT_SLICE,
                             cat(g, elem->comp, S("Slice")), elem, NULL, 0);
        }
        bool ok;
        int64_t n = array_len(g, at_->len, &ok);
        if (!ok) {
            unsupported(g, e, "the length is not a constant from-go could work out");
            return NULL;
        }
        return composite(g, ast_node_pos((AstNode)e), GT_ARRAY,
                         cat3(g, elem->comp, S("Array"), itoa(g, n)), elem, NULL, n);
    }
    case AST_KIND_MAP_TYPE: {
        AstMapType *mt = (AstMapType *)e;
        GenType *key = resolve(g, mt->key);
        GenType *val = resolve(g, mt->value);
        if (key == NULL || val == NULL)
            return NULL;
        return composite(g, ast_node_pos((AstNode)e), GT_MAP,
                         cat3(g, key->comp, val->comp, S("Map")), val, key, 0);
    }
    case AST_KIND_INTERFACE_TYPE: {
        AstInterfaceType *it = (AstInterfaceType *)e;
        if (it->methods == NULL || it->methods->list.len == 0)
            return builtin(g, S("any"));
        unsupported(g, e, "an interface with methods has no descriptor to give it");
        return NULL;
    }
    case AST_KIND_STRUCT_TYPE:
        unsupported(g, e, "a struct needs a name of its own");
        return NULL;
    case AST_KIND_FUNC_TYPE:
        unsupported(g, e, "a function type has no descriptor to give it");
        return NULL;
    case AST_KIND_CHAN_TYPE:
        unsupported(g, e, "a channel has no descriptor to give it");
        return NULL;
    case AST_KIND_INDEX_EXPR:
    case AST_KIND_INDEX_LIST_EXPR:
        unsupported(g, e, "generic types are not supported");
        return NULL;
    default:
        unsupported(g, e, "not a type from-go knows");
        return NULL;
    }
}

static void define(Gen *g, GenType *t);

static void add_field(Gen *g, GenType *t, Str cname, Str goname, Str tag, GenType *ft) {
    GenField *f = BURROW_NEW(g->a, GenField);
    if (f == NULL)
        return;
    *f = (GenField){cname, goname, tag, ft};
    t->fields = push(g, t->fields, f);
}

static GenField *field(GenType *t, Int i) {
    return (GenField *)at(t->fields, i);
}

static void struct_fields(Gen *g, GenType *t, AstStructType *st) {
    Int blanks = 0;
    for (Int i = 0; i < st->fields->list.len; i++) {
        AstField *f = (AstField *)at(st->fields->list, i);
        GenType *ft = resolve(g, f->type);
        if (ft == NULL)
            continue;
        Str tag = BURROW_STR_EMPTY;
        if (f->tag != NULL) {
            Error err = BURROW_NO_ERROR;
            tag = strconv_unquote(g->a, f->tag->value, &err);
            if (BURROW_FAILED(err))
                gen_error(g, f->tag->value_pos, S("a tag from-go cannot unquote"));
        }
        if (f->names.len == 0) {
            /* Embedded. The field takes the name of its type, the C one, since
             * that is the name field_is_embedded compares with. */
            GenType *named = ft->what == GT_PTR ? ft->elem : ft;
            add_field(g, t, c_ident(g, snake(g, named->comp)), named->cname, tag, ft);
            continue;
        }
        for (Int j = 0; j < f->names.len; j++) {
            Str name = ((AstIdent *)at(f->names, j))->name;
            if (str_eq(name, S("_"))) {
                /* Go allows any number of these and C allows one of each name,
                 * so they are numbered, which field_is_blank allows for. */
                Str blank = cat(g, S("_"), itoa(g, ++blanks));
                add_field(g, t, blank, blank, tag, ft);
                continue;
            }
            Str cname = c_ident(g, snake(g, name));
            if (cname.len == 0) {
                gen_error(g, ast_node_pos((AstNode)f),
                          fmt_sprintf_v(g->a, "%s: only ASCII names", name));
                continue;
            }
            add_field(g, t, cname, name, tag, ft);
        }
    }
    if (t->fields.len == 0 && g->errors == 0)
        gen_error(g, t->spec->name->name_pos,
                  fmt_sprintf_v(g->a, "%s: C has no empty structs", t->goname));
    for (Int i = 0; i < t->fields.len; i++) {
        for (Int j = 0; j < i; j++) {
            if (str_eq(field(t, i)->cname, field(t, j)->cname))
                gen_error(g, t->spec->name->name_pos,
                          fmt_sprintf_v(g->a, "%s: fields %s and %s are both %s in C",
                                        t->goname, field(t, j)->goname,
                                        field(t, i)->goname, field(t, i)->cname));
        }
    }
}

/* Works out what declared type t is from the type expression e on the right of
 * its declaration. A declared type on the right is followed to its own right
 * hand side, since type A B gives A the structure of B and not B's name. */
static void classify(Gen *g, GenType *t, AstExpr e, int depth) {
    if (depth > 64) {
        gen_error(g, t->spec->name->name_pos,
                  fmt_sprintf_v(g->a, "%s: invalid recursive type", t->goname));
        return;
    }
    switch (e->kind) {
    case AST_KIND_PAREN_EXPR:
        classify(g, t, ((AstParenExpr *)e)->x, depth + 1);
        return;
    case AST_KIND_STRUCT_TYPE:
        t->what = GT_STRUCT;
        struct_fields(g, t, (AstStructType *)e);
        return;
    case AST_KIND_IDENT:
    case AST_KIND_SELECTOR_EXPR: {
        GenType *u =
            e->kind == AST_KIND_IDENT ? find_declared(g, ((AstIdent *)e)->name) : NULL;
        if (u != NULL) {
            classify(g, t, u->spec->type, depth + 1);
            return;
        }
        GenType *b = resolve(g, e);
        if (b == NULL)
            return;
        if (b->kind == NULL || str_eq(b->cname, S("UnsafePointer"))) {
            unsupported(g, e,
                        "a named type has to be over a basic type, a struct, a "
                        "slice, an array, a pointer or a map");
            return;
        }
        t->what = GT_BASIC;
        t->elem = b;
        t->kind = b->kind;
        return;
    }
    case AST_KIND_ARRAY_TYPE: {
        AstArrayType *at_ = (AstArrayType *)e;
        t->elem = resolve(g, at_->elt);
        if (at_->len == NULL) {
            t->what = GT_SLICE;
            return;
        }
        bool ok;
        t->what = GT_ARRAY;
        t->len = array_len(g, at_->len, &ok);
        if (!ok)
            unsupported(g, e, "the length is not a constant from-go could work out");
        return;
    }
    case AST_KIND_STAR_EXPR:
        t->what = GT_PTR;
        t->elem = resolve(g, ((AstStarExpr *)e)->x);
        return;
    case AST_KIND_MAP_TYPE:
        t->what = GT_MAP;
        t->key = resolve(g, ((AstMapType *)e)->key);
        t->elem = resolve(g, ((AstMapType *)e)->value);
        return;
    default:
        (void)resolve(g, e); /* says why not */
        return;
    }
}

static void define(Gen *g, GenType *t) {
    if (t->state != 0)
        return;
    t->state = 1;
    AstTypeSpec *ts = t->spec;
    if (ts->type_params != NULL && ts->type_params->list.len > 0) {
        gen_error(
            g, ts->name->name_pos,
            fmt_sprintf_v(g->a, "%s: generic types are not supported", t->goname));
    } else if (ts->assign != 0) {
        t->what = GT_ALIAS;
        t->elem = resolve(g, ts->type);
    } else {
        classify(g, t, ts->type, 0);
    }
    t->state = 2;
}

/* -------------------------------------------------------------- the order */

static void visit(Gen *g, GenType *t);

/* A type that only has to be somewhere. One still being visited will be, so
 * this is not a cycle: type Item struct { Next *Item } is fine. */
static void visit_after(Gen *g, GenType *t) {
    if (t != NULL && t->mark == 0)
        visit(g, t);
}

/* Each type after the ones its declaration needs complete. A struct needs its
 * fields' types, an array its element, an alias its target, and a pointer the
 * type it points at unless that is a struct or an array, which have forward
 * declarations. Slices and maps are a Slice and a Map * whatever is in them,
 * so they need nothing, and the types in them only have to be somewhere. */
static void visit(Gen *g, GenType *t) {
    if (t == NULL || t->what == GT_BUILTIN || t->mark == 2)
        return;
    if (t->mark == 1) {
        gen_error(g, t->spec != NULL ? t->spec->name->name_pos : 0,
                  fmt_sprintf_v(g->a, "%s: invalid recursive type", t->cname));
        return;
    }
    t->mark = 1;
    switch (t->what) {
    case GT_STRUCT:
        for (Int i = 0; i < t->fields.len; i++)
            visit(g, field(t, i)->type);
        break;
    case GT_ARRAY:
    case GT_ALIAS:
        visit(g, t->elem);
        break;
    case GT_PTR:
        if (t->elem != NULL && t->elem->what != GT_STRUCT && t->elem->what != GT_ARRAY)
            visit(g, t->elem);
        break;
    default:
        break;
    }
    t->mark = 2;
    g->order = push(g, g->order, t);
    if (t->what == GT_SLICE || t->what == GT_PTR || t->what == GT_MAP) {
        visit_after(g, t->elem);
        visit_after(g, t->key);
    }
}

/* ---------------------------------------------------------------- the output */

/* The type as a parameter of a prototype. The made up slices, maps and
 * pointers are spelled as what they are rather than by a typedef that would
 * need a descriptor nothing uses. */
static Str c_type(Gen *g, GenType *t) {
    if (t->declared || t->what == GT_BUILTIN)
        return t->cname;
    switch (t->what) {
    case GT_SLICE:
        return S("Slice");
    case GT_MAP:
        return S("Map *");
    case GT_PTR: {
        Str e = c_type(g, t->elem);
        return cat(g, e, e.p[e.len - 1] == '*' ? S("*") : S(" *"));
    }
    default:
        g->roots = push(g, g->roots, t); /* an array has to be declared */
        return t->cname;
    }
}

static Str c_decl(Gen *g, Str type, Str name) {
    return cat3(g, type, type.p[type.len - 1] == '*' ? S("") : S(" "), name);
}

static void put_fields_macro(Gen *g, BytesBuffer *b, GenType *t) {
    Str m = upper(g, cat(g, snake(g, t->cname), S("_FIELDS")));
    puts_(b, "#define ");
    put(b, m);
    puts_(b, "(F, T)");
    for (Int i = 0; i < t->fields.len; i++) {
        GenField *f = field(t, i);
        puts_(b, " \\\n    F(T, ");
        put(b, f->type->cname);
        puts_(b, ", ");
        put(b, f->cname);
        puts_(b, ", ");
        put(b, f->goname);
        puts_(b, ", ");
        put(b, c_quote(g, f->tag));
        puts_(b, ")");
    }
    puts_(b, "\n");
}

/* One type's declaration, or with define its definition. */
static void put_type(Gen *g, BytesBuffer *b, GenType *t, bool def) {
    Str suffix = def ? S("_DEFINE(") : S("_DECL(");
    switch (t->what) {
    case GT_STRUCT: {
        Str m = upper(g, cat(g, snake(g, t->cname), S("_FIELDS")));
        if (!def)
            put_fields_macro(g, b, t);
        put(b, cat3(g, S("BURROW_STRUCT_AS"), suffix, t->cname));
        put(b, cat3(g, S(", "), m, S(");\n")));
        return;
    }
    case GT_BASIC:
        put(b, cat3(g, S("BURROW_NAMED_TYPE"), suffix, t->cname));
        put(b, cat3(g, S(", "), t->elem->cname, S(", ")));
        puts_(b, t->kind);
        puts_(b, ");\n");
        return;
    case GT_SLICE:
        put(b,
            cat3(g, t->declared ? S("BURROW_NAMED_SLICE_TYPE") : S("BURROW_SLICE_TYPE"),
                 suffix, t->cname));
        put(b, cat3(g, S(", "), t->elem->cname, S(");\n")));
        return;
    case GT_ARRAY:
        put(b, cat3(g, S("BURROW_ARRAY_TYPE"), suffix, t->cname));
        put(b, cat3(g, S(", "), t->elem->cname, S(", ")));
        put(b, cat(g, itoa(g, t->len), S(");\n")));
        return;
    case GT_PTR:
        put(b, cat3(g, S("BURROW_PTR_TYPE"), suffix, t->cname));
        put(b, cat3(g, S(", "), t->elem->cname, S(");\n")));
        return;
    case GT_MAP:
        put(b, cat3(g, S("BURROW_MAP_TYPE"), suffix, t->cname));
        put(b, cat3(g, S(", "), t->key->cname, S(", ")));
        put(b, cat(g, t->elem->cname, S(");\n")));
        return;
    case GT_ALIAS:
        if (def)
            return;
        put(b, cat3(g, S("typedef "), t->elem->cname, S(" ")));
        put(b, cat(g, t->cname, S(";\n")));
        put(b, cat3(g, S("#define burrow_type_"), t->cname, S(" burrow_type_")));
        put(b, cat(g, t->elem->cname, S("\n")));
        return;
    default:
        return;
    }
}

/* A constant's value as C, or empty after an error. */
static Str const_value(Gen *g, AstIdent *id, GenType *typ) {
    TypesObject obj = NULL;
    TypesConst *c = NULL;
    if (map_get2(g->info.defs, &id, &obj) && obj != NULL)
        c = types_as_const(obj);
    ConstantValue v = {0};
    if (c != NULL)
        v = types_const_val(c);
    ConstantKind k = constant_value_kind(v);
    const char *kind = typ != NULL ? typ->kind : NULL;
    bool is_float = kind != NULL && (strcmp(kind, "KIND_FLOAT32") == 0 ||
                                     strcmp(kind, "KIND_FLOAT64") == 0);
    Str s = BURROW_STR_EMPTY;
    bool exact = false;
    if (k == CONSTANT_BOOL) {
        s = constant_bool_val(v) ? S("true") : S("false");
    } else if (k == CONSTANT_STRING) {
        return cat3(g, S("BURROW_S("), c_quote(g, constant_string_val(v)), S(")"));
    } else if (k == CONSTANT_INT && !is_float) {
        int64_t n = constant_int64_val(v, &exact);
        if (exact && n >= INT32_MIN && n <= INT32_MAX) {
            s = itoa(g, n);
        } else if (exact) {
            s = cat3(g, S("INT64_C("), itoa(g, n), S(")"));
        } else {
            uint64_t u = constant_uint64_val(v, &exact);
            if (exact)
                s = cat3(g, S("UINT64_C("), strconv_format_uint(g->a, u, 10), S(")"));
        }
        if (!exact) {
            gen_error(g, id->name_pos,
                      fmt_sprintf_v(g->a, "%s: does not fit in 64 bits", id->name));
            return BURROW_STR_EMPTY;
        }
    } else if (k == CONSTANT_INT || k == CONSTANT_FLOAT) {
        double f = constant_float64_val(v, &exact);
        if (!isfinite(f)) {
            gen_error(g, id->name_pos,
                      fmt_sprintf_v(g->a, "%s: does not fit in a float64", id->name));
            return BURROW_STR_EMPTY;
        }
        s = strconv_format_float(g->a, f, 'g', -1, 64);
        if (strings_index_any(s, S(".e")) < 0)
            s = cat(g, s, S(".0"));
    } else {
        gen_error(g, id->name_pos,
                  fmt_sprintf_v(g->a,
                                "%s: from-go cannot work out this constant's value, "
                                "or it is complex",
                                id->name));
        return BURROW_STR_EMPTY;
    }
    if (typ == NULL || str_eq(typ->cname, S("bool")))
        return s;
    return cat3(g, cat3(g, S("(("), typ->cname, S(")")), s, S(")"));
}

static void put_consts(Gen *g, BytesBuffer *b, AstGenDecl *d) {
    AstExpr last_type = NULL;
    bool first = true;
    for (Int i = 0; i < d->specs.len; i++) {
        AstValueSpec *vs = (AstValueSpec *)at(d->specs, i);
        /* A spec with neither type nor values repeats the one before it. */
        if (vs->type != NULL || vs->values.len > 0)
            last_type = vs->type;
        GenType *typ = last_type != NULL ? resolve(g, last_type) : NULL;
        while (typ != NULL && typ->what == GT_ALIAS)
            typ = typ->elem;
        if (typ != NULL && typ->kind == NULL) {
            unsupported(g, last_type, "a constant has to have a basic type");
            continue;
        }
        for (Int j = 0; j < vs->names.len; j++) {
            AstIdent *id = (AstIdent *)at(vs->names, j);
            if (!token_is_exported(id->name))
                continue;
            Str v = const_value(g, id, typ);
            if (v.len == 0)
                continue;
            if (first) {
                puts_(b, "\n");
                if (d->lparen != 0)
                    put_doc(g, b, d->doc, "");
                first = false;
            }
            put_doc(g, b, d->lparen != 0 ? vs->doc : d->doc, "");
            put(b, cat3(g, S("#define "), g->cprefix, upper(g, snake(g, id->name))));
            put(b, cat3(g, S(" "), v, S("\n")));
        }
    }
}

typedef struct GenParam {
    Str type;
    Str name;
} GenParam;

static Slice add_param(Gen *g, Slice ps, Str type, Str name) {
    GenParam *p = BURROW_NEW(g->a, GenParam);
    if (p == NULL)
        return ps;
    *p = (GenParam){type, name};
    return push(g, ps, p);
}

static bool is_error_type(AstExpr e) {
    return e->kind == AST_KIND_IDENT && str_eq(((AstIdent *)e)->name, S("error"));
}

static void put_func(Gen *g, BytesBuffer *b, AstFuncDecl *fd) {
    if (!token_is_exported(fd->name->name))
        return;
    if (fd->type->type_params != NULL && fd->type->type_params->list.len > 0) {
        gen_error(g, fd->name->name_pos,
                  fmt_sprintf_v(g->a, "%s: generic functions are not supported",
                                fd->name->name));
        return;
    }
    Str cname = cat(g, g->fprefix, snake(g, fd->name->name));
    Slice ps = slice_make(g->a, TYPE_UNSAFE_POINTER, 0, 0);
    int errors = g->errors;

    if (fd->recv != NULL && fd->recv->list.len == 1) {
        AstField *r = (AstField *)at(fd->recv->list, 0);
        AstExpr rt = r->type;
        bool ptr = rt->kind == AST_KIND_STAR_EXPR;
        if (ptr)
            rt = ((AstStarExpr *)rt)->x;
        GenType *t = rt->kind == AST_KIND_IDENT
                         ? find_declared(g, ((AstIdent *)rt)->name)
                         : NULL;
        if (t == NULL) {
            unsupported(g, r->type, "a method has to be on a type declared here");
            return;
        }
        if (!token_is_exported(t->goname))
            return;
        cname = cat3(g, g->fprefix, snake(g, t->goname),
                     cat(g, S("_"), snake(g, fd->name->name)));
        Str name = S("recv");
        if (r->names.len > 0 && !str_eq(((AstIdent *)at(r->names, 0))->name, S("_")))
            name = c_ident(g, snake(g, ((AstIdent *)at(r->names, 0))->name));
        ps = add_param(g, ps, ptr ? cat(g, t->cname, S(" *")) : t->cname, name);
    }

    Int n = 0;
    for (Int i = 0; fd->type->params != NULL && i < fd->type->params->list.len; i++) {
        AstField *f = (AstField *)at(fd->type->params->list, i);
        Str type;
        if (f->type->kind == AST_KIND_ELLIPSIS) {
            type = S("Slice"); /* f(xs ...T) takes a []T */
            (void)resolve(g, ((AstEllipsis *)f->type)->elt);
        } else {
            GenType *t = resolve(g, f->type);
            type = t != NULL ? c_type(g, t) : S("");
        }
        Int names = f->names.len > 0 ? f->names.len : 1;
        for (Int j = 0; j < names; j++) {
            n++;
            Str name = f->names.len > 0 ? ((AstIdent *)at(f->names, j))->name : S("_");
            name = str_eq(name, S("_")) ? cat(g, S("arg"), itoa(g, n))
                                        : c_ident(g, snake(g, name));
            ps = add_param(g, ps, type, name);
        }
    }

    /* The results: the first is returned, the rest are out parameters in Go's
     * order, and an error last is Error *err, or the return value when it is
     * the only result. */
    Slice rs = slice_make(g->a, TYPE_UNSAFE_POINTER, 0, 0);
    n = 0;
    for (Int i = 0; fd->type->results != NULL && i < fd->type->results->list.len; i++) {
        AstField *f = (AstField *)at(fd->type->results->list, i);
        GenType *t = resolve(g, f->type);
        Str type = t != NULL ? c_type(g, t) : S("");
        Int names = f->names.len > 0 ? f->names.len : 1;
        for (Int j = 0; j < names; j++) {
            n++;
            Str name = f->names.len > 0 ? ((AstIdent *)at(f->names, j))->name : S("_");
            if (str_eq(name, S("_")))
                name = is_error_type(f->type) ? S("err") : cat(g, S("r"), itoa(g, n));
            rs = add_param(g, rs, type, c_ident(g, snake(g, name)));
        }
    }
    if (g->errors != errors)
        return;
    Str ret = S("void");
    for (Int i = 0; i < rs.len; i++) {
        GenParam *r = (GenParam *)at(rs, i);
        if (i == 0)
            ret = r->type;
        else
            ps = add_param(
                g, ps,
                cat(g, r->type, r->type.p[r->type.len - 1] == '*' ? S("*") : S(" *")),
                r->name);
    }

    puts_(b, "\n");
    put_doc(g, b, fd->doc, "");
    put(b, c_decl(g, ret, cname));
    puts_(b, "(");
    for (Int i = 0; i < ps.len; i++) {
        GenParam *p = (GenParam *)at(ps, i);
        if (i > 0)
            puts_(b, ", ");
        put(b, c_decl(g, p->type, p->name));
    }
    puts_(b, ps.len == 0 ? "void);\n" : ");\n");
}

static void ignore_type_error(void *env, const TypesError *e) {
    (void)env;
    (void)e;
}

static Str capitalize(Gen *g, Str s) {
    if (s.len == 0 || !is_lower(s.p[0]))
        return s;
    Byte c = (Byte)(s.p[0] - 'a' + 'A');
    return cat(g, str_from_bytes(&c, 1), str_from_bytes(s.p + 1, s.len - 1));
}

static int usage(Gen *g) {
    (void)fmt_fprintf_v(g->err,
                        "usage: from-go [-o file] [-header file] [-prefix name] "
                        "file.go ...\n");
    return 2;
}

static int write_out(Gen *g, const char *name, BytesBuffer *b) {
    Slice data = bytes_buffer_bytes(b);
    Error err = BURROW_NO_ERROR;
    if (name == NULL)
        (void)io_write_string(os_file_as_io_writer(os_stdout),
                              str_from_bytes(data.p, data.len), &err);
    else
        err = os_write_file(cstr(name), data, 0644);
    if (BURROW_FAILED(err)) {
        (void)fmt_fprintf_v(g->err, "from-go: %v\n", err);
        return 1;
    }
    return 0;
}

static int run(Gen *g, int argc, char **argv) {
    const char *out = NULL;
    const char *header = NULL;
    const char *prefix = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (i + 1 >= argc)
            return usage(g);
        if (strcmp(argv[i], "-o") == 0)
            out = argv[++i];
        else if (strcmp(argv[i], "-header") == 0)
            header = argv[++i];
        else if (strcmp(argv[i], "-prefix") == 0)
            prefix = argv[++i];
        else
            return usage(g);
    }
    if (i == argc)
        return usage(g);

    g->fset = token_new_file_set(g->a);
    Slice files = slice_make(g->a, TYPE_AST_FILE_PTR, 0, 0);
    Slice names = slice_make(g->a, TYPE_OF(Str), 0, 0);
    for (; i < argc; i++) {
        Str name = cstr(argv[i]);
        Error err = BURROW_NO_ERROR;
        Slice src = os_read_file(g->a, name, &err);
        AstFile *f = NULL;
        if (!BURROW_FAILED(err))
            f = parser_parse_file(g->a, g->fset, name, BURROW_ANY(TYPE_BYTES, &src),
                                  PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION,
                                  &err);
        if (BURROW_FAILED(err)) {
            go_scanner_print_error(g->err, err);
            return 1;
        }
        if (files.len > 0 && !str_eq(f->name->name, g->pkg)) {
            gen_error(g, f->name->name_pos,
                      fmt_sprintf_v(g->a, "package %s, after files of package %s",
                                    f->name->name, g->pkg));
            return 1;
        }
        g->pkg = f->name->name;
        files = slice_append(g->a, files, &f, 1);
        Str base = filepath_base(name);
        names = slice_append(g->a, names, &base, 1);
    }

    Str p = prefix != NULL ? cstr(prefix) : str_eq(g->pkg, S("main")) ? S("") : g->pkg;
    g->tprefix = capitalize(g, p);
    g->fprefix = p.len > 0 ? cat(g, snake(g, p), S("_")) : S("");
    g->cprefix = upper(g, g->fprefix);

    /* Only the constants and the array lengths come from the checker, so its
     * errors, which include every import it was given no importer for, are no
     * concern of this. Whatever matters shows up as a value it could not work
     * out. */
    TypesConfig conf = {0};
    conf.ignore_func_bodies = true;
    conf.error = BURROW_FN(TypesErrorFunc, ignore_type_error, NULL);
    g->info = types_info_all(g->a);
    (void)types_config_check(&conf, g->a, g->pkg, g->fset, files, &g->info, NULL);

    for (size_t k = 0; k < sizeof gen_builtins / sizeof gen_builtins[0]; k++) {
        const GenBuiltin *gb = &gen_builtins[k];
        GenType *t = new_type(g, GT_BUILTIN, cstr(gb->c), cstr(gb->comp));
        if (t == NULL)
            return 1;
        t->kind = gb->kind;
        t->state = 2;
        g->builtins = push(g, g->builtins, t);
    }

    /* Every declared type first, so that they can name each other in any
     * order, then what each one is. */
    for (Int fi = 0; fi < files.len; fi++) {
        AstFile *f = ((AstFile **)files.p)[fi];
        for (Int di = 0; di < f->decls.len; di++) {
            AstGenDecl *d = (AstGenDecl *)at(f->decls, di);
            if (d->node.kind != AST_KIND_GEN_DECL || d->tok != TOKEN_TYPE_)
                continue;
            for (Int si = 0; si < d->specs.len; si++) {
                AstTypeSpec *ts = (AstTypeSpec *)at(d->specs, si);
                if (str_eq(ts->name->name, S("_")))
                    continue;
                GenType *t = new_type(g, GT_BUILTIN, cat(g, g->tprefix, ts->name->name),
                                      ts->name->name);
                if (t == NULL)
                    return 1;
                t->goname = ts->name->name;
                t->declared = true;
                t->spec = ts;
                t->doc = d->lparen != 0 ? ts->doc : d->doc;
                g->types = push(g, g->types, t);
                g->roots = push(g, g->roots, t);
            }
        }
    }
    Int declared = g->types.len;
    for (Int k = 0; k < declared; k++)
        define(g, (GenType *)at(g->types, k));

    BytesBuffer consts = BYTES_BUFFER(g->a);
    BytesBuffer funcs = BYTES_BUFFER(g->a);
    for (Int fi = 0; fi < files.len; fi++) {
        AstFile *f = ((AstFile **)files.p)[fi];
        for (Int di = 0; di < f->decls.len; di++) {
            AstDecl d = (AstDecl)at(f->decls, di);
            if (d->kind == AST_KIND_FUNC_DECL)
                put_func(g, &funcs, (AstFuncDecl *)d);
            else if (((AstGenDecl *)d)->tok == TOKEN_CONST)
                put_consts(g, &consts, (AstGenDecl *)d);
        }
    }
    if (g->errors > 0)
        return 1;

    for (Int k = 0; k < g->roots.len; k++)
        visit(g, (GenType *)at(g->roots, k));
    if (g->errors > 0)
        return 1;

    /* The declarations. */
    BytesBuffer banner = BYTES_BUFFER(g->a);
    puts_(&banner, "/* Code generated by burrow-gen from-go from ");
    for (Int k = 0; k < names.len; k++) {
        if (k > 0)
            puts_(&banner, k == names.len - 1 ? " and " : ", ");
        put(&banner, ((Str *)names.p)[k]);
    }
    puts_(&banner, ". DO NOT EDIT. */\n\n");
    BytesBuffer h = BYTES_BUFFER(g->a);
    put_buf(&h, &banner);
    Str guard = BURROW_STR_EMPTY;
    if (header != NULL) {
        Str base = filepath_base(cstr(header));
        Byte *gp = (Byte *)mem_alloc(g->a, (size_t)base.len + 1, 1);
        if (gp == NULL)
            return 1;
        for (Int k = 0; k < base.len; k++) {
            Byte c = base.p[k];
            gp[k] = is_lower(c)                  ? (Byte)(c - 'a' + 'A')
                    : is_upper(c) || is_digit(c) ? c
                                                 : '_';
        }
        guard = str_from_bytes(gp, base.len);
        put(&h, cat3(g, S("#ifndef "), guard, S("\n")));
        put(&h, cat3(g, S("#define "), guard, S("\n\n")));
    }
    puts_(&h, "#include \"burrow/burrow.h\"\n");

    bool any = false;
    for (Int k = 0; k < g->order.len; k++) {
        GenType *t = (GenType *)at(g->order, k);
        if (t->what != GT_STRUCT && t->what != GT_ARRAY)
            continue;
        if (!any)
            puts_(&h, "\n");
        any = true;
        put(&h, cat3(g, S("typedef struct "), t->cname, S(" ")));
        put(&h, cat(g, t->cname, S(";\n")));
    }
    for (Int k = 0; k < g->order.len; k++) {
        GenType *t = (GenType *)at(g->order, k);
        puts_(&h, "\n");
        put_doc(g, &h, t->doc, "");
        put_type(g, &h, t, false);
    }
    put_buf(&h, &consts);
    put_buf(&h, &funcs);

    /* The definitions. */
    BytesBuffer cb = BYTES_BUFFER(g->a);
    BytesBuffer *defs = &h;
    if (header != NULL) {
        put(&h, cat3(g, S("\n#endif /* "), guard, S(" */\n")));
        put_buf(&cb, &banner);
        put(&cb, cat3(g, S("#include \""), filepath_base(cstr(header)), S("\"\n")));
        defs = &cb;
    }
    any = false;
    for (Int k = 0; k < g->order.len; k++) {
        GenType *t = (GenType *)at(g->order, k);
        if (t->what == GT_ALIAS)
            continue;
        if (!any)
            puts_(defs, "\n");
        any = true;
        put_type(g, defs, t, true);
    }

    if (header != NULL && write_out(g, header, &h) != 0)
        return 1;
    return write_out(g, out, defs);
}

int main(int argc, char **argv) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Gen g = {0};
    g.a = arena_allocator(&ar);
    g.err = os_file_as_io_writer(os_stderr);
    g.builtins = slice_make(g.a, TYPE_UNSAFE_POINTER, 0, 0);
    g.types = slice_make(g.a, TYPE_UNSAFE_POINTER, 0, 0);
    g.order = slice_make(g.a, TYPE_UNSAFE_POINTER, 0, 0);
    g.roots = slice_make(g.a, TYPE_UNSAFE_POINTER, 0, 0);
    int rc = run(&g, argc, argv);
    arena_free(&ar);
    return rc;
}
