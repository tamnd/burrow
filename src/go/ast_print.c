/* go/ast: Fprint, Print and NotNilFilter.
 *
 * Go's printer walks the tree with reflect. This one walks it with the type
 * descriptors, which carry the same names, so the output is the same. The one
 * thing a descriptor cannot say by itself is what an Expr or a Stmt holds, so
 * a field of one of the five node interface types is looked through with
 * ast_node_type, the way reflect looks through an interface to its dynamic
 * type.
 *
 * Derived from Go's src/go/ast/print.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ast_not_nil_filter(Str name, Any v) {
    (void)name;
    if (v.t == NULL || v.data == NULL) {
        return true;
    }
    switch ((int)v.t->kind) {
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_INTERFACE:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_SLICE:
        /* Each of these starts with the word that is NULL when it is nil: the
         * pointer, the function, the descriptor or vtable, the slice's array. */
        return *(void *const *)v.data != NULL;
    default:
        return true;
    }
}

typedef struct ApPrinter {
    IoWriter output;
    TokenFileSet *fset;
    AstFieldFilter filter;
    Map *ptrmap; /* uintptr_t of a pointer -> the line it was first printed on */
    Int indent;  /* the current indentation level */
    Byte last;   /* the last byte written */
    Int line;    /* the current line number */
    Error err;   /* the first error, after which nothing more is written */
    Arena scratch;
} ApPrinter;

/* printer.Write: data with a line number and the indentation put in front of
 * every line. */
static void ap_write(ApPrinter *p, Str data) {
    Int n = 0;
    for (Int i = 0; i < data.len && BURROW_OK(p->err); i++) {
        Byte b = data.p[i];
        if (b == '\n') {
            io_write_string(p->output, str_from_bytes(data.p + n, i + 1 - n), &p->err);
            n = i + 1;
            p->line++;
        } else if (p->last == '\n') {
            Alloc *sa = arena_allocator(&p->scratch);
            ArenaMark mark = arena_mark(&p->scratch);
            Str num = fmt_sprintf_v(sa, "%6d  ", p->line);
            io_write_string(p->output, num, &p->err);
            arena_release(&p->scratch, mark);
            for (Int j = p->indent; j > 0 && BURROW_OK(p->err); j--) {
                io_write_string(p->output, BURROW_S(".  "), &p->err);
            }
        }
        p->last = b;
    }
    if (BURROW_OK(p->err) && data.len > n) {
        io_write_string(p->output, str_from_bytes(data.p + n, data.len - n), &p->err);
    }
}

static void ap_puts(ApPrinter *p, Str s) {
    ap_write(p, s);
}

/* One value formatted by fmt with verb, which is a whole format string such
 * as "%q". */
static void ap_format(ApPrinter *p, Str format, Any v) {
    if (!BURROW_OK(p->err)) {
        return;
    }
    Alloc *sa = arena_allocator(&p->scratch);
    ArenaMark mark = arena_mark(&p->scratch);
    Any args[1] = {v};
    ap_write(p, fmt_sprintf(sa, format, slice_from(args, 1, 1, TYPE_OF(Any))));
    arena_release(&p->scratch, mark);
}

static void ap_int(ApPrinter *p, Str format, Int n) {
    ap_format(p, format, BURROW_ANY(TYPE_OF(Int), &n));
}

/* x.Type(), as fmt's %T spells it. */
static void ap_type(ApPrinter *p, Any x) {
    ap_format(p, BURROW_S("%T"), x);
}

static void ap_print(ApPrinter *p, Any x);

static bool ap_is_node_iface(const Type *t) {
    return t == TYPE_AST_NODE || t == TYPE_AST_EXPR || t == TYPE_AST_STMT ||
           t == TYPE_AST_DECL || t == TYPE_AST_SPEC;
}

/* A pointer to elem at addr, printed once and referred to by line number after
 * that. */
static void ap_pointer(ApPrinter *p, void *addr, const Type *elem) {
    ap_puts(p, BURROW_S("*"));
    uintptr_t key = (uintptr_t)addr;
    Int *line = (Int *)map_get(p->ptrmap, &key);
    if (line != NULL) {
        ap_int(p, BURROW_S("(obj @ %d)"), *line);
        return;
    }
    if (!map_set(p->ptrmap, &key, &p->line)) {
        p->err = burrow_err_out_of_memory;
        return;
    }
    ap_print(p, BURROW_ANY(elem, addr));
}

/* The elements of an array or slice, numbered, between braces that follow
 * whatever came first. */
static void ap_elements(ApPrinter *p, const Type *elem, Byte *base, Int n) {
    if (n > 0) {
        p->indent++;
        ap_puts(p, BURROW_S("\n"));
        for (Int i = 0; i < n; i++) {
            ap_int(p, BURROW_S("%d: "), i);
            ap_print(p, BURROW_ANY(elem, base + (size_t)i * elem->size));
            ap_puts(p, BURROW_S("\n"));
        }
        p->indent--;
    }
    ap_puts(p, BURROW_S("}"));
}

static void ap_print(ApPrinter *p, Any x) {
    if (!BURROW_OK(p->err)) {
        return;
    }
    if (!ast_not_nil_filter(BURROW_STR_EMPTY, x)) {
        ap_puts(p, BURROW_S("nil"));
        return;
    }
    const Type *t = x.t;
    if (ap_is_node_iface(t)) {
        /* An Expr and the like holds a pointer to a node, whose kind says
         * what it is. */
        AstNode n = *(AstNode *)x.data;
        const Type *st = ast_node_type(n);
        if (st == NULL) {
            ap_int(p, BURROW_S("<bad node kind %d>"), n->kind);
            return;
        }
        ap_pointer(p, n, st);
        return;
    }
    switch ((int)t->kind) {
    case KIND_INTERFACE:
        if (t == TYPE_OF(Any)) {
            ap_print(p, *(Any *)x.data);
        } else {
            ap_format(p, BURROW_S("%v"), x);
        }
        return;
    case KIND_MAP: {
        Map *m = *(Map **)x.data;
        ap_type(p, x);
        ap_int(p, BURROW_S(" (len = %d) {"), map_len(m));
        if (map_len(m) > 0) {
            p->indent++;
            ap_puts(p, BURROW_S("\n"));
            MapIter it = map_iter(m);
            const void *k = NULL;
            void *v = NULL;
            while (map_next(&it, &k, &v)) {
                ap_print(p, BURROW_ANY(t->key, (void *)(uintptr_t)k));
                ap_puts(p, BURROW_S(": "));
                ap_print(p, BURROW_ANY(t->elem, v));
                ap_puts(p, BURROW_S("\n"));
            }
            p->indent--;
        }
        ap_puts(p, BURROW_S("}"));
        return;
    }
    case KIND_POINTER:
        if (t->elem == NULL) {
            ap_format(p, BURROW_S("%v"), x);
            return;
        }
        ap_pointer(p, *(void **)x.data, t->elem);
        return;
    case KIND_ARRAY:
        ap_type(p, x);
        ap_puts(p, BURROW_S(" {"));
        ap_elements(p, t->elem, (Byte *)x.data, (Int)t->len);
        return;
    case KIND_SLICE: {
        Slice s = *(Slice *)x.data;
        if (t->elem == TYPE_BYTE) {
            ap_format(p, BURROW_S("%#q"), x);
            return;
        }
        ap_type(p, x);
        ap_int(p, BURROW_S(" (len = %d) {"), s.len);
        ap_elements(p, t->elem, (Byte *)s.p, s.len);
        return;
    }
    case KIND_STRUCT: {
        ap_type(p, x);
        ap_puts(p, BURROW_S(" {"));
        p->indent++;
        bool first = true;
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (!ast_is_exported(f->name)) {
                continue;
            }
            Any value = BURROW_ANY(f->type, (Byte *)x.data + f->offset);
            if (p->filter.f == NULL || p->filter.f(p->filter.env, f->name, value)) {
                if (first) {
                    ap_puts(p, BURROW_S("\n"));
                    first = false;
                }
                ap_puts(p, f->name);
                ap_puts(p, BURROW_S(": "));
                ap_print(p, value);
                ap_puts(p, BURROW_S("\n"));
            }
        }
        p->indent--;
        ap_puts(p, BURROW_S("}"));
        return;
    }
    default:
        if (t->kind == KIND_STRING) {
            ap_format(p, BURROW_S("%q"), x);
            return;
        }
        if (t == TYPE_TOKEN_POS && p->fset != NULL) {
            TokenPosition pos = token_file_set_position(p->fset, *(TokenPos *)x.data);
            Alloc *sa = arena_allocator(&p->scratch);
            ArenaMark mark = arena_mark(&p->scratch);
            ap_puts(p, token_position_string(pos, sa));
            arena_release(&p->scratch, mark);
            return;
        }
        ap_format(p, BURROW_S("%v"), x);
        return;
    }
}

Error ast_fprint(Alloc *a, IoWriter w, TokenFileSet *fset, Any x, AstFieldFilter f) {
    ApPrinter p;
    p.output = w;
    p.fset = fset;
    p.filter = f;
    p.indent = 0;
    p.last = '\n'; /* so that the first line gets its number */
    p.line = 0;
    p.err = BURROW_NO_ERROR;
    p.ptrmap = map_make(a, TYPE_UINTPTR, TYPE_OF(Int), 0);
    if (p.ptrmap == NULL) {
        return burrow_err_out_of_memory;
    }
    arena_init(&p.scratch, a, 0);
    if (x.t == NULL) {
        ap_puts(&p, BURROW_S("nil\n"));
    } else {
        ap_print(&p, x);
        ap_puts(&p, BURROW_S("\n"));
    }
    arena_free(&p.scratch);
    map_free(p.ptrmap);
    return p.err;
}

static bool ap_not_nil(void *env, Str name, Any v) {
    (void)env;
    return ast_not_nil_filter(name, v);
}

Error ast_print(Alloc *a, TokenFileSet *fset, Any x) {
    return ast_fprint(a, os_file_as_io_writer(os_stdout), fset, x,
                      (AstFieldFilter){ap_not_nil, NULL});
}
