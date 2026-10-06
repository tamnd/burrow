/* Derived from Go's src/net/http/routing_tree.go, routing_index.go and
 * mapping.go: the tree ServeMux matches requests with, the index it finds
 * conflicting patterns with, and the small map the tree's nodes keep their
 * children in.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_routing.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- mapping */

bool burrow__http_mapping_add(Alloc *a, burrow__HttpMapping *h, Str k, void *v) {
    if (h->m == NULL && h->len < BURROW__HTTP_MAX_SLICE) {
        if (h->s == NULL) {
            h->s = (burrow__HttpEntry *)mem_alloc(
                a, BURROW__HTTP_MAX_SLICE * sizeof *h->s, _Alignof(burrow__HttpEntry));
            if (h->s == NULL)
                return false;
        }
        h->s[h->len].key = k;
        h->s[h->len].value = v;
        h->len++;
        return true;
    }
    if (h->m == NULL) {
        Map *m = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (m == NULL)
            return false;
        for (Int i = 0; i < h->len; i++)
            if (!map_set(m, &h->s[i].key, &h->s[i].value))
                return false;
        mem_free(a, h->s, BURROW__HTTP_MAX_SLICE * sizeof *h->s,
                 _Alignof(burrow__HttpEntry));
        h->s = NULL;
        h->len = 0;
        h->m = m;
    }
    return map_set(h->m, &k, &v);
}

void *burrow__http_mapping_find(const burrow__HttpMapping *h, Str k, bool *found) {
    if (found != NULL)
        *found = false;
    if (h == NULL)
        return NULL;
    if (h->m != NULL) {
        void **vp = (void **)map_get(h->m, &k);
        if (vp == NULL)
            return NULL;
        if (found != NULL)
            *found = true;
        return *vp;
    }
    for (Int i = 0; i < h->len; i++) {
        if (str_eq(h->s[i].key, k)) {
            if (found != NULL)
                *found = true;
            return h->s[i].value;
        }
    }
    return NULL;
}

void burrow__http_mapping_each_pair(const burrow__HttpMapping *h,
                                    burrow__HttpMappingFunc f, void *env) {
    if (h == NULL)
        return;
    if (h->m != NULL) {
        const void *k;
        void *v;
        for (MapIter it = map_iter(h->m); map_next(&it, &k, &v);)
            if (!f(env, *(const Str *)k, *(void **)v))
                return;
        return;
    }
    for (Int i = 0; i < h->len; i++)
        if (!f(env, h->s[i].key, h->s[i].value))
            return;
}

/* ------------------------------------------------------------ routing tree */

static burrow__HttpRoutingNode *rt_new_node(Alloc *a) {
    burrow__HttpRoutingNode *n = (burrow__HttpRoutingNode *)mem_alloc(
        a, sizeof *n, _Alignof(burrow__HttpRoutingNode));
    if (n != NULL)
        memset(n, 0, sizeof *n);
    return n;
}

burrow__HttpRoutingNode *
burrow__http_routing_find_child(const burrow__HttpRoutingNode *n, Str key) {
    if (key.len == 0)
        return n->empty_child;
    return (burrow__HttpRoutingNode *)burrow__http_mapping_find(&n->children, key,
                                                                NULL);
}

/* addChild. The child of n for key, made if it is not there yet. NULL when a
 * says no. */
static burrow__HttpRoutingNode *rt_add_child(Alloc *a, burrow__HttpRoutingNode *n,
                                             Str key) {
    if (key.len == 0) {
        if (n->empty_child == NULL)
            n->empty_child = rt_new_node(a);
        return n->empty_child;
    }
    burrow__HttpRoutingNode *c = burrow__http_routing_find_child(n, key);
    if (c != NULL)
        return c;
    c = rt_new_node(a);
    if (c == NULL || !burrow__http_mapping_add(a, &n->children, key, c))
        return NULL;
    return c;
}

/* set. Makes n the leaf for p. */
static void rt_set(burrow__HttpRoutingNode *n, burrow__HttpPattern *p, void *h) {
    if (n->pattern != NULL || n->handler != NULL)
        panic_str(BURROW_S("non-nil leaf fields"));
    n->pattern = p;
    n->handler = h;
}

bool burrow__http_routing_add_pattern(Alloc *a, burrow__HttpRoutingNode *root,
                                      burrow__HttpPattern *p, void *h) {
    burrow__HttpRoutingNode *n = rt_add_child(a, root, p->host);
    if (n == NULL)
        return false;
    n = rt_add_child(a, n, p->method);
    if (n == NULL)
        return false;
    /* addSegments. */
    for (Int i = 0; i < p->nsegments; i++) {
        burrow__HttpSegment seg = p->segments[i];
        if (seg.multi) {
            if (i != p->nsegments - 1)
                panic_str(BURROW_S("multi wildcard not last"));
            burrow__HttpRoutingNode *c = rt_new_node(a);
            if (c == NULL)
                return false;
            n->multi_child = c;
            rt_set(c, p, h);
            return true;
        }
        /* A single wildcard is the child with the empty key. */
        n = rt_add_child(a, n, seg.wild ? BURROW_S("") : seg.s);
        if (n == NULL)
            return false;
    }
    rt_set(n, p, h);
    return true;
}

Str burrow__http_first_segment(Alloc *a, Str path, Str *rest) {
    if (str_eq(path, BURROW_S("/"))) {
        *rest = BURROW_S("");
        return BURROW_S("/");
    }
    /* Past the slash it starts with. */
    path = str_from_bytes(path.p + 1, path.len - 1);
    Int i = strings_index_byte(path, '/');
    if (i < 0)
        i = path.len;
    *rest = str_from_bytes(path.p + i, path.len - i);
    return burrow__http_path_unescape(a, str_from_bytes(path.p, i));
}

/* matchPath. The leaf under n that matches path, with *out set to matches and
 * the values of the wildcards met on the way. Like the Go, which appends to
 * matches as it goes, a branch that fails leaves nothing behind that a later
 * one can see, because each has its own length. */
static burrow__HttpRoutingNode *rt_match_path(burrow__HttpRoutingNode *n, Alloc *a,
                                              Str path, Slice matches, Slice *out) {
    if (n == NULL)
        return NULL;
    /* The end of the path, so a match only if a pattern ends here. */
    if (path.len == 0) {
        if (n->pattern == NULL)
            return NULL;
        *out = matches;
        return n;
    }
    /* The literal first, as the more specific. */
    Str rest;
    Str seg = burrow__http_first_segment(a, path, &rest);
    burrow__HttpRoutingNode *l =
        rt_match_path(burrow__http_routing_find_child(n, seg), a, rest, matches, out);
    if (l != NULL)
        return l;
    /* Then a single wildcard, which a trailing slash does not match. */
    if (!str_eq(seg, BURROW_S("/")) && n->empty_child != NULL) {
        Slice m = slice_append(a, matches, &seg, 1);
        if (m.len == matches.len + 1) {
            l = rt_match_path(n->empty_child, a, rest, m, out);
            if (l != NULL)
                return l;
        }
    }
    /* Then a multi, which matches all that is left, and gives it as the value
     * unless it is the anonymous one of a trailing slash. */
    burrow__HttpRoutingNode *c = n->multi_child;
    if (c != NULL) {
        if (burrow__http_pattern_last_segment(c->pattern).s.len != 0) {
            Str v =
                burrow__http_path_unescape(a, str_from_bytes(path.p + 1, path.len - 1));
            matches = slice_append(a, matches, &v, 1);
        }
        *out = matches;
        return c;
    }
    return NULL;
}

/* matchMethodAndPath. n is a host's node. */
static burrow__HttpRoutingNode *rt_match_method_and_path(burrow__HttpRoutingNode *n,
                                                         Alloc *a, Str method, Str path,
                                                         Slice *out) {
    if (n == NULL)
        return NULL;
    Slice none = slice_from(NULL, 0, 0, TYPE_STRING);
    burrow__HttpRoutingNode *l =
        rt_match_path(burrow__http_routing_find_child(n, method), a, path, none, out);
    if (l != NULL)
        return l;
    /* A pattern for GET serves HEAD too. */
    if (str_eq(method, BURROW_S("HEAD"))) {
        l = rt_match_path(burrow__http_routing_find_child(n, BURROW_S("GET")), a, path,
                          none, out);
        if (l != NULL)
            return l;
    }
    /* Last the patterns with no method. */
    return rt_match_path(n->empty_child, a, path, none, out);
}

burrow__HttpRoutingNode *burrow__http_routing_match(const burrow__HttpRoutingNode *root,
                                                    Alloc *a, Str host, Str method,
                                                    Str path, Slice *matches) {
    *matches = slice_from(NULL, 0, 0, TYPE_STRING);
    /* A pattern with the host wins over one without. */
    if (host.len > 0) {
        burrow__HttpRoutingNode *l = rt_match_method_and_path(
            burrow__http_routing_find_child(root, host), a, method, path, matches);
        if (l != NULL)
            return l;
    }
    return rt_match_method_and_path(root->empty_child, a, method, path, matches);
}

typedef struct rt_MethodsEnv {
    Alloc *a;
    Str path;
    Map *set;
    bool ok;
} rt_MethodsEnv;

static bool rt_add_method(void *env, Str method, void *child) {
    rt_MethodsEnv *e = (rt_MethodsEnv *)env;
    Slice m;
    if (rt_match_path((burrow__HttpRoutingNode *)child, e->a, e->path,
                      slice_from(NULL, 0, 0, TYPE_STRING), &m) != NULL) {
        bool yes = true;
        if (!map_set(e->set, &method, &yes)) {
            e->ok = false;
            return false;
        }
    }
    return true;
}

/* matchingMethodsPath. */
static bool rt_matching_methods_path(const burrow__HttpRoutingNode *n, Alloc *a,
                                     Str path, Map *set) {
    if (n == NULL)
        return true;
    rt_MethodsEnv e = {a, path, set, true};
    burrow__http_mapping_each_pair(&n->children, rt_add_method, &e);
    return e.ok;
}

bool burrow__http_routing_matching_methods(const burrow__HttpRoutingNode *root,
                                           Alloc *a, Str host, Str path,
                                           Map *method_set) {
    if (host.len > 0 &&
        !rt_matching_methods_path(burrow__http_routing_find_child(root, host), a, path,
                                  method_set))
        return false;
    if (!rt_matching_methods_path(root->empty_child, a, path, method_set))
        return false;
    /* What serves GET serves HEAD. */
    Str get = BURROW_S("GET");
    bool *has_get = (bool *)map_get(method_set, &get);
    if (has_get != NULL && *has_get) {
        Str head = BURROW_S("HEAD");
        bool yes = true;
        return map_set(method_set, &head, &yes);
    }
    return true;
}

/* ----------------------------------------------------------- routing index */

/* The patterns at position pos with the literal s there, or with a wildcard
 * there when s is "". Empty when there are none. */
static Slice ri_lookup(const burrow__HttpRoutingIndex *idx, Int pos, Str s) {
    Slice none = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    if (pos >= idx->nsegments || idx->segments[pos] == NULL)
        return none;
    Slice **sp = (Slice **)map_get(idx->segments[pos], &s);
    return sp != NULL ? **sp : none;
}

/* The map for position pos, made if need be. NULL when a says no. */
static Map *ri_level(Alloc *a, burrow__HttpRoutingIndex *idx, Int pos) {
    if (pos >= idx->nsegments) {
        Int n = idx->nsegments * 2;
        if (n <= pos)
            n = pos + 1;
        Map **grown = (Map **)mem_alloc(a, (size_t)n * sizeof *grown, _Alignof(Map *));
        if (grown == NULL)
            return NULL;
        memset(grown, 0, (size_t)n * sizeof *grown);
        if (idx->nsegments > 0)
            memcpy(grown, idx->segments, (size_t)idx->nsegments * sizeof *grown);
        mem_free(a, idx->segments, (size_t)idx->nsegments * sizeof *grown,
                 _Alignof(Map *));
        idx->segments = grown;
        idx->nsegments = n;
    }
    if (idx->segments[pos] == NULL)
        idx->segments[pos] = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    return idx->segments[pos];
}

static bool ri_append(Alloc *a, Slice *s, burrow__HttpPattern *pat) {
    void *p = pat;
    Slice grown = slice_append(a, *s, &p, 1);
    if (grown.len != s->len + 1)
        return false;
    *s = grown;
    return true;
}

bool burrow__http_routing_index_add_pattern(Alloc *a, burrow__HttpRoutingIndex *idx,
                                            burrow__HttpPattern *pat) {
    if (burrow__http_pattern_last_segment(pat).multi) {
        if (idx->multis.elem == NULL)
            idx->multis = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
        return ri_append(a, &idx->multis, pat);
    }
    for (Int pos = 0; pos < pat->nsegments; pos++) {
        burrow__HttpSegment seg = pat->segments[pos];
        Str key = seg.wild ? BURROW_S("") : seg.s;
        Map *m = ri_level(a, idx, pos);
        if (m == NULL)
            return false;
        Slice **sp = (Slice **)map_get(m, &key);
        Slice *pats;
        if (sp != NULL) {
            pats = *sp;
        } else {
            pats = (Slice *)mem_alloc(a, sizeof *pats, _Alignof(Slice));
            if (pats == NULL)
                return false;
            *pats = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
            if (!map_set(m, &key, &pats))
                return false;
        }
        if (!ri_append(a, pats, pat))
            return false;
    }
    return true;
}

/* apply. Calls f with each of pats unless an earlier call failed, and stops at
 * the first that fails. */
static void ri_apply(Slice pats, burrow__HttpPatternFunc f, void *env, Error *err) {
    if (BURROW_FAILED(*err))
        return;
    for (Int i = 0; i < pats.len; i++) {
        *err = f(env, (burrow__HttpPattern *)((void **)pats.p)[i]);
        if (BURROW_FAILED(*err))
            return;
    }
}

Error burrow__http_routing_index_possibly_conflicting(
    const burrow__HttpRoutingIndex *idx, const burrow__HttpPattern *pat,
    burrow__HttpPatternFunc f, void *env) {
    Error err = BURROW_NO_ERROR;
    /* A pattern ending in a multi might conflict with anything. */
    ri_apply(idx->multis, f, env, &err);
    if (BURROW_FAILED(err))
        return err;
    /* One ending in "{$}" only with those that have "/" at the same place. */
    if (str_eq(burrow__http_pattern_last_segment(pat).s, BURROW_S("/"))) {
        ri_apply(ri_lookup(idx, pat->nsegments - 1, BURROW_S("/")), f, env, &err);
        return err;
    }
    /* Otherwise, those with the same literal or a wildcard at some position
     * where pat has a literal, at the position with the fewest of them. */
    Slice lmin = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    Slice wmin = lmin;
    Int min = INT64_MAX;
    bool has_lit = false;
    for (Int i = 0; i < pat->nsegments; i++) {
        burrow__HttpSegment seg = pat->segments[i];
        if (seg.multi)
            break;
        if (!seg.wild) {
            has_lit = true;
            Slice lpats = ri_lookup(idx, i, seg.s);
            Slice wpats = ri_lookup(idx, i, BURROW_S(""));
            Int sum = lpats.len + wpats.len;
            if (sum < min) {
                lmin = lpats;
                wmin = wpats;
                min = sum;
            }
        }
    }
    if (has_lit) {
        ri_apply(lmin, f, env, &err);
        ri_apply(wmin, f, env, &err);
        return err;
    }
    /* All wildcards, so any pattern at all might conflict. */
    for (Int pos = 0; pos < idx->nsegments; pos++) {
        if (idx->segments[pos] == NULL)
            continue;
        void *v;
        for (MapIter it = map_iter(idx->segments[pos]); map_next(&it, NULL, &v);)
            ri_apply(**(Slice **)v, f, env, &err);
    }
    return err;
}
