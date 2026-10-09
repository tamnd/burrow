/* go/ast: CommentMap.
 *
 * A Map from an AstNode to a Slice of the AstCommentGroup pointers that
 * belong to it. The slices are made in the allocator the map was, and Filter
 * shares them with the map it filters, as Go's does.
 *
 * Derived from Go's src/go/ast/commentmap.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

/* []*ast.CommentGroup, the type of the map's values. */
static const Type acm_slice_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_AstCommentGroupPtr,
    NULL,
    0,
    0,
    NULL,
};

BURROW_NORETURN static void acm_oom(void) {
    panic_str(BURROW_S("go/ast: out of memory"));
}

static AstNode acm_at(Slice s, Int i) {
    return ((AstNode *)s.p)[i];
}

static Slice acm_append(Alloc *a, Slice s, AstNode n) {
    Slice grown = slice_append(a, s, &n, 1);
    if (slice_is_nil(grown)) {
        acm_oom();
    }
    return grown;
}

static int acm_cmp_group(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos a = ast_comment_group_pos(*(AstCommentGroup *const *)x);
    TokenPos b = ast_comment_group_pos(*(AstCommentGroup *const *)y);
    return (a > b) - (a < b);
}

static void acm_sort_comments(Slice list) {
    slices_sort_func(list, (SlicesCmpFunc){acm_cmp_group, NULL});
}

static Map *acm_make(Alloc *a) {
    return map_make(a, TYPE_AST_NODE, &acm_slice_type, 0);
}

static void acm_set(Map *cmap, AstNode n, Slice list) {
    if (!map_set(cmap, &n, &list)) {
        acm_oom();
    }
}

static Slice acm_get(Map *cmap, AstNode n) {
    Slice *list = (Slice *)map_get(cmap, &n);
    if (list == NULL) {
        return slice_nil(TYPE_AST_COMMENT_GROUP_PTR);
    }
    return *list;
}

static void acm_add_comment(Map *cmap, Alloc *a, AstNode n, AstCommentGroup *c) {
    Slice list = acm_get(cmap, n);
    if (list.len == 0) {
        list = slice_make(a, TYPE_AST_COMMENT_GROUP_PTR, 1, 1);
        if (slice_is_nil(list)) {
            acm_oom();
        }
        ((AstNode *)list.p)[0] = &c->node;
    } else {
        list = acm_append(a, list, &c->node);
    }
    acm_set(cmap, n, list);
}

/* nodeList: the nodes under n in depth first order, comments left out. */
typedef struct AcmList {
    Alloc *a;
    Slice list;
} AcmList;

static bool acm_collect(void *env, AstNode n) {
    AcmList *l = (AcmList *)env;
    if (n == NULL || n->kind == AST_KIND_COMMENT_GROUP || n->kind == AST_KIND_COMMENT) {
        return false;
    }
    l->list = acm_append(l->a, l->list, n);
    return true;
}

/* nodeStack: the enclosing nodes that comments can go to. */
static AstNode acm_pop(Slice *s, TokenPos pos) {
    AstNode top = NULL;
    Int i = s->len;
    while (i > 0 && ast_node_end(acm_at(*s, i - 1)) <= pos) {
        top = acm_at(*s, i - 1);
        i--;
    }
    s->len = i;
    return top;
}

static void acm_push(Alloc *a, Slice *s, AstNode n) {
    acm_pop(s, ast_node_pos(n));
    *s = acm_append(a, *s, n);
}

/* Whether a comment group can belong to n as one of the nodes that enclose
 * it: a file, a field, a declaration, a spec or a statement. */
static bool acm_is_group(AstNode n) {
    return n->kind == AST_KIND_FIELD ||
           (n->kind >= AST_KIND_BAD_STMT && n->kind <= AST_KIND_FILE);
}

/* commentListReader */
typedef struct AcmReader {
    TokenFileSet *fset;
    Slice list;
    Int index;
    AstCommentGroup *comment;
    TokenPosition pos, end;
} AcmReader;

static bool acm_eol(AcmReader *r) {
    return r->index >= r->list.len;
}

static void acm_next(AcmReader *r) {
    if (!acm_eol(r)) {
        r->comment = (AstCommentGroup *)acm_at(r->list, r->index);
        r->pos = token_file_set_position(r->fset, ast_comment_group_pos(r->comment));
        r->end = token_file_set_position(r->fset, ast_comment_group_end(r->comment));
        r->index++;
    }
}

AstCommentMap ast_new_comment_map(Alloc *a, TokenFileSet *fset, AstNode node,
                                  Slice comments) {
    if (comments.len == 0) {
        return NULL;
    }
    Map *cmap = acm_make(a);
    if (cmap == NULL) {
        return NULL;
    }
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);

    /* A sorted copy, so that comments is left as it was. */
    Slice tmp = slice_make(sa, TYPE_AST_COMMENT_GROUP_PTR, comments.len, comments.len);
    if (slice_is_nil(tmp)) {
        acm_oom();
    }
    slice_copy(tmp, comments);
    acm_sort_comments(tmp);
    AcmReader r = {fset, tmp, 0, NULL, {{NULL, 0}, 0, 0, 0}, {{NULL, 0}, 0, 0, 0}};
    acm_next(&r);

    AcmList nodes = {sa, slice_nil(TYPE_AST_NODE)};
    ast_inspect(node, (AstInspectFunc){acm_collect, &nodes});
    nodes.list = acm_append(sa, nodes.list, NULL); /* the sentinel */

    AstNode p = NULL;                          /* the previous node */
    TokenPosition pend = {{NULL, 0}, 0, 0, 0}; /* where p ends */
    AstNode pg = NULL; /* the previous group of nodes that matter */
    TokenPosition pgend = {{NULL, 0}, 0, 0, 0};
    Slice stack = slice_nil(TYPE_AST_NODE);

    for (Int i = 0; i < nodes.list.len; i++) {
        AstNode q = acm_at(nodes.list, i);
        TokenPosition qpos = {{NULL, 0}, 0, 0, 0};
        if (q != NULL) {
            qpos = token_file_set_position(fset, ast_node_pos(q));
        } else {
            /* The sentinel is past the end of everything. */
            qpos.offset = (Int)1 << 30;
            qpos.line = (Int)1 << 30;
        }

        /* The comments before q. */
        while (r.end.offset <= qpos.offset) {
            AstNode top = acm_pop(&stack, ast_comment_group_pos(r.comment));
            if (top != NULL) {
                pg = top;
                pgend = token_file_set_position(fset, ast_node_end(pg));
            }
            AstNode assoc = NULL;
            if (pg != NULL &&
                (pgend.line == r.pos.line ||
                 (pgend.line + 1 == r.pos.line && r.end.line + 1 < qpos.line))) {
                /* On the line pg ends on, or on the next line with a blank line
                 * after it: pg's. */
                assoc = pg;
            } else if (p != NULL &&
                       (pend.line == r.pos.line ||
                        (pend.line + 1 == r.pos.line && r.end.line + 1 < qpos.line) ||
                        q == NULL)) {
                /* The same for the previous node, which also gets everything
                 * after the last one. */
                assoc = p;
            } else {
                if (q == NULL) {
                    panic_str(BURROW_S("internal error: no comments should be "
                                       "associated with sentinel"));
                }
                assoc = q;
            }
            acm_add_comment(cmap, a, assoc, r.comment);
            if (acm_eol(&r)) {
                arena_free(&scratch);
                return cmap;
            }
            acm_next(&r);
        }

        if (q == NULL) {
            break; /* every comment comes before the sentinel */
        }
        p = q;
        pend = token_file_set_position(fset, ast_node_end(p));
        if (acm_is_group(q)) {
            acm_push(sa, &stack, q);
        }
    }
    arena_free(&scratch);
    return cmap;
}

AstNode ast_comment_map_update(AstCommentMap cmap, Alloc *a, AstNode old,
                               AstNode new_node) {
    Slice list = acm_get(cmap, old);
    if (list.len > 0) {
        map_del(cmap, &old);
        Slice to = acm_get(cmap, new_node);
        Slice grown = slice_append_slice(a, to, list);
        if (slice_is_nil(grown)) {
            acm_oom();
        }
        acm_set(cmap, new_node, grown);
    }
    return new_node;
}

typedef struct AcmFilter {
    Map *cmap;
    Map *umap;
} AcmFilter;

static bool acm_filter_visit(void *env, AstNode n) {
    AcmFilter *f = (AcmFilter *)env;
    if (n == NULL) {
        return true;
    }
    Slice g = acm_get(f->cmap, n);
    if (g.len > 0) {
        acm_set(f->umap, n, g);
    }
    return true;
}

AstCommentMap ast_comment_map_filter(AstCommentMap cmap, Alloc *a, AstNode node) {
    AcmFilter f = {cmap, acm_make(a)};
    if (f.umap == NULL) {
        acm_oom();
    }
    if (map_len(cmap) > 0) {
        ast_inspect(node, (AstInspectFunc){acm_filter_visit, &f});
    }
    return f.umap;
}

Slice ast_comment_map_comments(AstCommentMap cmap, Alloc *a) {
    Slice list = slice_make(a, TYPE_AST_COMMENT_GROUP_PTR, 0, map_len(cmap));
    if (slice_is_nil(list)) {
        acm_oom();
    }
    MapIter it = map_iter(cmap);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        Slice grown = slice_append_slice(a, list, *(Slice *)v);
        if (slice_is_nil(grown)) {
            acm_oom();
        }
        list = grown;
    }
    acm_sort_comments(list);
    return list;
}

/* summary: the start of the text of the comments, on one line. */
static Str acm_summary(Alloc *a, Slice list) {
    enum { max_len = 40 };
    Byte buf[max_len + 1];
    Int n = 0;
    bool over = false;
    for (Int i = 0; i < list.len && n < max_len; i++) {
        AstCommentGroup *group = (AstCommentGroup *)acm_at(list, i);
        for (Int j = 0; j < group->list.len && n < max_len; j++) {
            Str text = ((AstComment *)acm_at(group->list, j))->text;
            /* Only whether it went past max_len matters, not by how much. */
            for (Int k = 0; k < text.len; k++) {
                if (n == max_len) {
                    over = true;
                    break;
                }
                buf[n++] = text.p[k];
            }
        }
    }
    if (over) {
        n = max_len - 3;
        buf[n++] = '.';
        buf[n++] = '.';
        buf[n++] = '.';
    }
    for (Int i = 0; i < n; i++) {
        if (buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = ' ';
        }
    }
    Byte *out = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    if (out == NULL) {
        acm_oom();
    }
    for (Int i = 0; i < n; i++) {
        out[i] = buf[i];
    }
    return str_from_bytes(out, n);
}

static int acm_cmp_node(void *env, const void *x, const void *y) {
    (void)env;
    AstNode a = *(const AstNode *)x;
    AstNode b = *(const AstNode *)y;
    TokenPos ap = ast_node_pos(a);
    TokenPos bp = ast_node_pos(b);
    if (ap != bp) {
        return (ap > bp) - (ap < bp);
    }
    TokenPos ae = ast_node_end(a);
    TokenPos be = ast_node_end(b);
    return (ae > be) - (ae < be);
}

Str ast_comment_map_string(AstCommentMap cmap, Alloc *a) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);

    Slice nodes = slice_nil(TYPE_AST_NODE);
    MapIter it = map_iter(cmap);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        nodes = acm_append(sa, nodes, *(const AstNode *)k);
    }
    slices_sort_func(nodes, (SlicesCmpFunc){acm_cmp_node, NULL});

    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    strings_builder_write_string(&buf, BURROW_S("CommentMap {\n"), &err);
    for (Int i = 0; i < nodes.len && BURROW_OK(err); i++) {
        AstNode node = acm_at(nodes, i);
        Str s;
        if (node->kind == AST_KIND_IDENT) {
            s = ((AstIdent *)node)->name;
        } else {
            s = fmt_sprintf_v(sa, "*%T", BURROW_ANY(ast_node_type(node), node));
        }
        Str line =
            fmt_sprintf_v(sa, "\t%p  %20s:  %s\n", BURROW_ANY(TYPE_AST_NODE, &node), s,
                          acm_summary(sa, acm_get(cmap, node)));
        strings_builder_write_string(&buf, line, &err);
    }
    strings_builder_write_string(&buf, BURROW_S("}\n"), &err);
    arena_free(&scratch);
    if (!BURROW_OK(err)) {
        acm_oom();
    }
    return strings_builder_string(&buf);
}
