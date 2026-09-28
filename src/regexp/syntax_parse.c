/* regexp/syntax: the parser, from parse.go and perl_groups.go.
 *
 * This follows Go's parser step for step: the same stack of nodes, the same
 * free list, the same factoring of alternations and the same size and height
 * limits, down to which node a cached size belongs to. The parse runs in a
 * scratch arena, where a slice that outgrows its array gets a new one and the
 * old one is left for the arena to drop, the way Go leaves it for the
 * collector. The finished tree is copied out into one block.
 *
 * Go returns errors up through every call. Here the first error, like Go's
 * ErrLarge and ErrNestingDepth panics, is noted in the parser and unwinds with
 * a panic to syntax_parse, which turns it into the SyntaxError.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp/syntax.h"

#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "syntax_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ errors */

typedef struct SyntaxErrorBox {
    SyntaxError e;
    Str message;
} SyntaxErrorBox;

static const Type syntax_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"regexp/syntax", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(SyntaxError),
    (uint16_t)_Alignof(SyntaxError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72657365U, /* "rese" */
    NULL,
};

const Type *const TYPE_SYNTAX_ERROR = &syntax_error_desc;

#define SYN_PREFIX "error parsing regexp: "

static Str syntax_error_message(const void *self) {
    return ((const SyntaxErrorBox *)self)->message;
}

static Error syntax_error_clone(const void *self, Alloc *a) {
    return syntax_error_as_error(&((const SyntaxErrorBox *)self)->e, a);
}

static const ErrorVT syntax_error_vt = {
    .self_type = &syntax_error_desc,
    .message = syntax_error_message,
    .clone = syntax_error_clone,
};

Str syntax_error_code_string(SyntaxErrorCode e) {
    return e;
}

/* The message, written into p, which has room for syn_error_len(e). */
static Int syn_error_len(const SyntaxError *e) {
    return (Int)sizeof SYN_PREFIX - 1 + e->code.len + 3 + e->expr.len + 1;
}

static void syn_error_write(Byte *p, const SyntaxError *e) {
    Int n = (Int)sizeof SYN_PREFIX - 1;
    memcpy(p, SYN_PREFIX, (size_t)n);
    if (e->code.len > 0)
        memcpy(p + n, e->code.p, (size_t)e->code.len);
    n += e->code.len;
    memcpy(p + n, ": `", 3);
    n += 3;
    if (e->expr.len > 0)
        memcpy(p + n, e->expr.p, (size_t)e->expr.len);
    n += e->expr.len;
    p[n] = '`';
}

Str syntax_error_error(const SyntaxError *e, Alloc *a) {
    Int n = syn_error_len(e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    syn_error_write(p, e);
    return str_from_bytes(p, n);
}

Error syntax_error_as_error(const SyntaxError *e, Alloc *a) {
    Int n = syn_error_len(e);
    size_t size = sizeof(SyntaxErrorBox) + (size_t)(e->code.len + e->expr.len + n);
    SyntaxErrorBox *b =
        (SyntaxErrorBox *)mem_alloc_nozero(a, size, _Alignof(SyntaxErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (e->code.len > 0)
        memcpy(p, e->code.p, (size_t)e->code.len);
    b->e.code = str_from_bytes(p, e->code.len);
    p += e->code.len;
    if (e->expr.len > 0)
        memcpy(p, e->expr.p, (size_t)e->expr.len);
    b->e.expr = str_from_bytes(p, e->expr.len);
    p += e->expr.len;
    syn_error_write(p, e);
    b->message = str_from_bytes(p, n);
    return (Error){&syntax_error_vt, b};
}

/* ------------------------------------------------------------ the node map */

#define SYN_TOMB ((const void *)(uintptr_t)1)

static size_t syn_hash(const void *k) {
    uint64_t x = (uint64_t)(uintptr_t)k;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

bool syn_map_get(const SynMap *m, const void *k, int64_t *v) {
    if (m->cap == 0)
        return false;
    size_t mask = (size_t)m->cap - 1;
    for (size_t i = syn_hash(k) & mask;; i = (i + 1) & mask) {
        const void *q = m->keys[i];
        if (q == NULL)
            return false;
        if (q == k) {
            *v = m->vals[i];
            return true;
        }
    }
}

static bool syn_map_grow(SynMap *m) {
    Int live = 0;
    for (Int i = 0; i < m->cap; i++)
        if (m->keys[i] != NULL && m->keys[i] != SYN_TOMB)
            live++;
    Int ncap = 16;
    while (ncap < 4 * (live + 1))
        ncap *= 2;
    const void **keys =
        (const void **)mem_alloc(m->a, (size_t)ncap * sizeof(void *), _Alignof(void *));
    int64_t *vals = (int64_t *)mem_alloc_nozero(m->a, (size_t)ncap * sizeof(int64_t),
                                                _Alignof(int64_t));
    if (keys == NULL || vals == NULL)
        return false;
    size_t mask = (size_t)ncap - 1;
    for (Int i = 0; i < m->cap; i++) {
        const void *k = m->keys[i];
        if (k == NULL || k == SYN_TOMB)
            continue;
        size_t j = syn_hash(k) & mask;
        while (keys[j] != NULL)
            j = (j + 1) & mask;
        keys[j] = k;
        vals[j] = m->vals[i];
    }
    if (m->cap > 0) {
        mem_free(m->a, (void *)m->keys, (size_t)m->cap * sizeof(void *),
                 _Alignof(void *));
        mem_free(m->a, m->vals, (size_t)m->cap * sizeof(int64_t), _Alignof(int64_t));
    }
    m->keys = keys;
    m->vals = vals;
    m->cap = ncap;
    m->used = live;
    return true;
}

bool syn_map_put(SynMap *m, const void *k, int64_t v) {
    if (m->cap > 0) {
        size_t mask = (size_t)m->cap - 1;
        size_t free_slot = SIZE_MAX;
        for (size_t i = syn_hash(k) & mask;; i = (i + 1) & mask) {
            const void *q = m->keys[i];
            if (q == k) {
                m->vals[i] = v;
                return true;
            }
            if (q == SYN_TOMB && free_slot == SIZE_MAX)
                free_slot = i;
            if (q == NULL) {
                if (free_slot != SIZE_MAX) {
                    m->keys[free_slot] = k;
                    m->vals[free_slot] = v;
                    return true;
                }
                break;
            }
        }
    }
    if ((m->used + 1) * 4 > m->cap * 3 && !syn_map_grow(m))
        return false;
    size_t mask = (size_t)m->cap - 1;
    size_t i = syn_hash(k) & mask;
    while (m->keys[i] != NULL && m->keys[i] != SYN_TOMB)
        i = (i + 1) & mask;
    if (m->keys[i] == NULL)
        m->used++;
    m->keys[i] = k;
    m->vals[i] = v;
    return true;
}

void syn_map_delete(SynMap *m, const void *k) {
    if (m->cap == 0)
        return;
    size_t mask = (size_t)m->cap - 1;
    for (size_t i = syn_hash(k) & mask;; i = (i + 1) & mask) {
        const void *q = m->keys[i];
        if (q == NULL)
            return;
        if (q == k) {
            m->keys[i] = SYN_TOMB;
            return;
        }
    }
}

/* ---------------------------------------------------------------- the parser */

/* Go's limits: nesting 1000 deep, a program of 128 MB at 40 bytes an
 * instruction, and 128 MB of runes in classes. */
#define SYN_MAX_HEIGHT 1000
#define SYN_MAX_SIZE ((int64_t)(128 << 20) / 40)
#define SYN_MAX_RUNES ((Int)(128 << 20) / 4)

typedef struct SynParser {
    SyntaxFlags flags;
    Slice stack; /* of SyntaxRegexp * */
    SyntaxRegexp *free;
    Int num_cap;
    Str whole;
    Slice tmp_class; /* of Rune */
    Int num_regexp;
    Int num_runes;
    int64_t repeats;
    bool have_height;
    SynMap height;
    bool have_size;
    SynMap size;
    Alloc *sa; /* the scratch arena */

    /* How it failed. */
    bool failed;
    bool oom;
    SyntaxErrorCode code;
    Str expr;
} SynParser;

static const Str syn_unwind = {(const Byte *)"regexp/syntax: parse error", 26};

BURROW_NORETURN static void syn_fail(SynParser *p, SyntaxErrorCode code, Str expr) {
    p->failed = true;
    p->code = code;
    p->expr = expr;
    panic_str(syn_unwind);
}

BURROW_NORETURN static void syn_oom(SynParser *p) {
    p->failed = true;
    p->oom = true;
    panic_str(syn_unwind);
}

static inline Str syn_from(Str s, Int i) {
    return (Str){s.p + i, s.len - i};
}

static inline Str syn_to(Str s, Int j) {
    return (Str){s.p, j};
}

/* s[:len(s)-len(t)], for t a tail of s. */
static inline Str syn_upto(Str s, Str t) {
    return (Str){s.p, s.len - t.len};
}

/* Go's append for s, with n elements of esz bytes from src: in place when
 * the capacity is there, into a new array from the arena when it is not. */
static Slice syn_append(SynParser *p, Slice s, const void *src, Int n, size_t esz,
                        size_t align) {
    if (n == 0)
        return s;
    if (s.len + n > s.cap) {
        Int ncap = s.cap * 2;
        if (ncap < s.len + n)
            ncap = s.len + n;
        if (ncap < 4)
            ncap = 4;
        void *q = mem_alloc_nozero(p->sa, (size_t)ncap * esz, align);
        if (q == NULL)
            syn_oom(p);
        if (s.len > 0)
            memcpy(q, s.p, (size_t)s.len * esz);
        s.p = q;
        s.cap = ncap;
    }
    memmove((Byte *)s.p + (size_t)s.len * esz, src, (size_t)n * esz);
    s.len += n;
    return s;
}

static inline Slice syn_append_runes(SynParser *p, Slice s, const Rune *src, Int n) {
    s = syn_append(p, s, src, n, sizeof(Rune), _Alignof(Rune));
    s.elem = TYPE_RUNE;
    return s;
}

static inline Slice syn_append2(SynParser *p, Slice s, Rune lo, Rune hi) {
    Rune two[2] = {lo, hi};
    return syn_append_runes(p, s, two, 2);
}

static inline Slice syn_append_subs(SynParser *p, Slice s, SyntaxRegexp *const *src,
                                    Int n) {
    s = syn_append(p, s, src, n, sizeof(SyntaxRegexp *), _Alignof(SyntaxRegexp *));
    s.elem = TYPE_UNSAFE_POINTER;
    return s;
}

static inline Slice syn_append_sub(SynParser *p, Slice s, SyntaxRegexp *re) {
    return syn_append_subs(p, s, &re, 1);
}

static inline Slice syn_sub0(SyntaxRegexp *re, Int len) {
    return (Slice){re->sub0, len, 1, TYPE_UNSAFE_POINTER};
}

static inline Slice syn_rune0(SyntaxRegexp *re, Int len) {
    return (Slice){re->rune0, len, 2, TYPE_RUNE};
}

static inline SyntaxRegexp *syn_top(SynParser *p, Int back) {
    return SYN_SUBS(p->stack)[p->stack.len - back];
}

static SyntaxRegexp *syn_new(SynParser *p, SyntaxOp op) {
    SyntaxRegexp *re = p->free;
    if (re != NULL) {
        p->free = re->sub0[0];
    } else {
        re =
            (SyntaxRegexp *)mem_alloc_nozero(p->sa, sizeof *re, _Alignof(SyntaxRegexp));
        if (re == NULL)
            syn_oom(p);
        p->num_regexp++;
    }
    memset(re, 0, sizeof *re);
    re->op = op;
    return re;
}

static void syn_reuse(SynParser *p, SyntaxRegexp *re) {
    if (p->have_height)
        syn_map_delete(&p->height, re);
    re->sub0[0] = p->free;
    p->free = re;
}

static void syn_put(SynParser *p, SynMap *m, const void *k, int64_t v) {
    if (!syn_map_put(m, k, v))
        syn_oom(p);
}

/* ------------------------------------------------------------------ limits */

static int64_t syn_calc_size(SynParser *p, const SyntaxRegexp *re, bool force) {
    int64_t size = 0;
    if (!force && syn_map_get(&p->size, re, &size))
        return size;
    size = 0;
    SyntaxRegexp **sub = SYN_SUBS(re->sub);
    switch (re->op) {
    case SYNTAX_OP_LITERAL:
        size = re->rune.len;
        break;
    case SYNTAX_OP_CAPTURE:
    case SYNTAX_OP_STAR:
        /* x* becomes (x)*, one alt and the x. */
        size = 2 + syn_calc_size(p, sub[0], false);
        break;
    case SYNTAX_OP_PLUS:
    case SYNTAX_OP_QUEST:
        size = 1 + syn_calc_size(p, sub[0], false);
        break;
    case SYNTAX_OP_CONCAT:
        for (Int i = 0; i < re->sub.len; i++)
            size += syn_calc_size(p, sub[i], false);
        break;
    case SYNTAX_OP_ALTERNATE:
        for (Int i = 0; i < re->sub.len; i++)
            size += syn_calc_size(p, sub[i], false);
        if (re->sub.len > 1)
            size += re->sub.len - 1;
        break;
    case SYNTAX_OP_REPEAT: {
        int64_t s = syn_calc_size(p, sub[0], false);
        if (re->max == -1) {
            if (re->min == 0)
                size = 2 + s; /* x* */
            else
                size = 1 + (int64_t)re->min * s; /* xxx+ */
            break;
        }
        /* x{2,5} becomes xx(x(x(x)?)?)? */
        size = (int64_t)re->max * s + (int64_t)(re->max - re->min);
        break;
    }
    default:
        break;
    }
    if (size < 1)
        size = 1;
    syn_put(p, &p->size, re, size);
    return size;
}

static void syn_check_size(SynParser *p, const SyntaxRegexp *re) {
    if (!p->have_size) {
        /* Until the pattern has enough nodes to possibly be too big, there is
         * no need to keep the map. Repeats multiply what one node can cost,
         * so they count against the threshold. */
        if (p->repeats == 0)
            p->repeats = 1;
        if (re->op == SYNTAX_OP_REPEAT) {
            Int n = re->max;
            if (n == -1)
                n = re->min;
            if (n <= 0)
                n = 1;
            if ((int64_t)n > SYN_MAX_SIZE / p->repeats)
                p->repeats = SYN_MAX_SIZE;
            else
                p->repeats *= (int64_t)n;
        }
        if ((int64_t)p->num_regexp < SYN_MAX_SIZE / p->repeats)
            return;

        /* The pattern might be too big now, so measure what is on the stack
         * so far and keep measuring from here on. */
        p->have_size = true;
        for (Int i = 0; i < p->stack.len; i++)
            syn_check_size(p, SYN_SUBS(p->stack)[i]);
    }
    if (syn_calc_size(p, re, true) > SYN_MAX_SIZE)
        syn_fail(p, SYNTAX_ERR_LARGE, p->whole);
}

static Int syn_calc_height(SynParser *p, const SyntaxRegexp *re, bool force) {
    int64_t h = 0;
    if (!force && syn_map_get(&p->height, re, &h))
        return (Int)h;
    h = 1;
    for (Int i = 0; i < re->sub.len; i++) {
        Int hsub = syn_calc_height(p, SYN_SUBS(re->sub)[i], false);
        if (h < 1 + hsub)
            h = 1 + hsub;
    }
    syn_put(p, &p->height, re, h);
    return (Int)h;
}

static void syn_check_height(SynParser *p, const SyntaxRegexp *re) {
    if (p->num_regexp < SYN_MAX_HEIGHT)
        return;
    if (!p->have_height) {
        p->have_height = true;
        for (Int i = 0; i < p->stack.len; i++)
            syn_check_height(p, SYN_SUBS(p->stack)[i]);
    }
    if (syn_calc_height(p, re, true) > SYN_MAX_HEIGHT)
        syn_fail(p, SYNTAX_ERR_NESTING_DEPTH, p->whole);
}

static void syn_check_limits(SynParser *p, const SyntaxRegexp *re) {
    if (p->num_runes > SYN_MAX_RUNES)
        syn_fail(p, SYNTAX_ERR_LARGE, p->whole);
    syn_check_size(p, re);
    syn_check_height(p, re);
}

/* ------------------------------------------------------------ char classes */

/* Sorts the lo, hi pairs of a class by lo, and by hi from high to low where
 * two share a lo. A heap sort: the only ties are pairs that are the same, so
 * any sort gives the same order. */
static bool syn_pair_less(const Rune *r, Int i, Int j) {
    return r[2 * i] < r[2 * j] || (r[2 * i] == r[2 * j] && r[2 * i + 1] > r[2 * j + 1]);
}

static void syn_pair_swap(Rune *r, Int i, Int j) {
    Rune lo = r[2 * i], hi = r[2 * i + 1];
    r[2 * i] = r[2 * j];
    r[2 * i + 1] = r[2 * j + 1];
    r[2 * j] = lo;
    r[2 * j + 1] = hi;
}

static void syn_sift_down(Rune *r, Int root, Int n) {
    for (;;) {
        Int child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && syn_pair_less(r, child, child + 1))
            child++;
        if (!syn_pair_less(r, root, child))
            return;
        syn_pair_swap(r, root, child);
        root = child;
    }
}

static void syn_sort_pairs(Rune *r, Int n) {
    if (n < 12) {
        for (Int i = 1; i < n; i++)
            for (Int j = i; j > 0 && syn_pair_less(r, j, j - 1); j--)
                syn_pair_swap(r, j, j - 1);
        return;
    }
    for (Int i = n / 2 - 1; i >= 0; i--)
        syn_sift_down(r, i, n);
    for (Int i = n - 1; i > 0; i--) {
        syn_pair_swap(r, 0, i);
        syn_sift_down(r, 0, i);
    }
}

/* cleanClass: sorts the ranges and merges the ones that touch or overlap. */
static Slice syn_clean_class(Slice *rp) {
    Rune *r = SYN_RUNES(*rp);
    syn_sort_pairs(r, rp->len / 2);
    if (rp->len < 2)
        return *rp;
    Int w = 2;
    for (Int i = 2; i < rp->len; i += 2) {
        Rune lo = r[i], hi = r[i + 1];
        if (lo <= r[w - 1] + 1) {
            /* It overlaps or touches the last one. */
            if (hi > r[w - 1])
                r[w - 1] = hi;
            continue;
        }
        r[w] = lo;
        r[w + 1] = hi;
        w += 2;
    }
    Slice out = *rp;
    out.len = w;
    return out;
}

/* appendRange: adds lo-hi to r, folding it into one of the last two ranges
 * when it touches it. */
static Slice syn_append_range(SynParser *p, Slice r, Rune lo, Rune hi) {
    Int n = r.len;
    Rune *q = SYN_RUNES(r);
    for (Int i = 2; i <= 4; i += 2) {
        if (n >= i) {
            Rune rlo = q[n - i], rhi = q[n - i + 1];
            if (lo <= rhi + 1 && rlo <= hi + 1) {
                if (lo < rlo)
                    q[n - i] = lo;
                if (hi > rhi)
                    q[n - i + 1] = hi;
                return r;
            }
        }
    }
    return syn_append2(p, r, lo, hi);
}

/* appendFoldedRange: lo-hi and every rune any of them case folds to. */
static Slice syn_append_folded_range(SynParser *p, Slice r, Rune lo, Rune hi) {
    if (lo <= SYN_MIN_FOLD && hi >= SYN_MAX_FOLD)
        return syn_append_range(p, r, lo, hi);
    if (hi < SYN_MIN_FOLD || lo > SYN_MAX_FOLD)
        return syn_append_range(p, r, lo, hi);
    if (lo < SYN_MIN_FOLD) {
        r = syn_append_range(p, r, lo, SYN_MIN_FOLD - 1);
        lo = SYN_MIN_FOLD;
    }
    if (hi > SYN_MAX_FOLD) {
        r = syn_append_range(p, r, SYN_MAX_FOLD + 1, hi);
        hi = SYN_MAX_FOLD;
    }
    for (Rune c = lo; c <= hi; c++) {
        r = syn_append_range(p, r, c, c);
        for (Rune f = unicode_simple_fold(c); f != c; f = unicode_simple_fold(f))
            r = syn_append_range(p, r, f, f);
    }
    return r;
}

static Slice syn_append_literal(SynParser *p, Slice r, Rune x, SyntaxFlags flags) {
    if (flags & SYNTAX_FOLD_CASE)
        return syn_append_folded_range(p, r, x, x);
    return syn_append_range(p, r, x, x);
}

static Slice syn_append_class(SynParser *p, Slice r, const Rune *x, Int len) {
    for (Int i = 0; i < len; i += 2)
        r = syn_append_range(p, r, x[i], x[i + 1]);
    return r;
}

static Slice syn_append_folded_class(SynParser *p, Slice r, const Rune *x, Int len) {
    for (Int i = 0; i < len; i += 2)
        r = syn_append_folded_range(p, r, x[i], x[i + 1]);
    return r;
}

static Slice syn_append_negated_class(SynParser *p, Slice r, const Rune *x, Int len) {
    Rune next_lo = 0;
    for (Int i = 0; i < len; i += 2) {
        Rune lo = x[i], hi = x[i + 1];
        if (next_lo <= lo - 1)
            r = syn_append_range(p, r, next_lo, lo - 1);
        next_lo = hi + 1;
    }
    if (next_lo <= UNICODE_MAX_RUNE)
        r = syn_append_range(p, r, next_lo, UNICODE_MAX_RUNE);
    return r;
}

static Slice syn_append_table(SynParser *p, Slice r, const UnicodeRangeTable *x) {
    for (Int k = 0; k < x->r16_len; k++) {
        Rune lo = (Rune)x->r16[k].lo, hi = (Rune)x->r16[k].hi,
             stride = (Rune)x->r16[k].stride;
        if (stride == 1) {
            r = syn_append_range(p, r, lo, hi);
            continue;
        }
        for (Rune c = lo; c <= hi; c += stride)
            r = syn_append_range(p, r, c, c);
    }
    for (Int k = 0; k < x->r32_len; k++) {
        Rune lo = (Rune)x->r32[k].lo, hi = (Rune)x->r32[k].hi,
             stride = (Rune)x->r32[k].stride;
        if (stride == 1) {
            r = syn_append_range(p, r, lo, hi);
            continue;
        }
        for (Rune c = lo; c <= hi; c += stride)
            r = syn_append_range(p, r, c, c);
    }
    return r;
}

static Slice syn_append_negated_table(SynParser *p, Slice r,
                                      const UnicodeRangeTable *x) {
    Rune next_lo = 0;
    for (Int k = 0; k < x->r16_len; k++) {
        Rune lo = (Rune)x->r16[k].lo, hi = (Rune)x->r16[k].hi,
             stride = (Rune)x->r16[k].stride;
        if (stride == 1) {
            if (next_lo <= lo - 1)
                r = syn_append_range(p, r, next_lo, lo - 1);
            next_lo = hi + 1;
            continue;
        }
        for (Rune c = lo; c <= hi; c += stride) {
            if (next_lo <= c - 1)
                r = syn_append_range(p, r, next_lo, c - 1);
            next_lo = c + 1;
        }
    }
    for (Int k = 0; k < x->r32_len; k++) {
        Rune lo = (Rune)x->r32[k].lo, hi = (Rune)x->r32[k].hi,
             stride = (Rune)x->r32[k].stride;
        if (stride == 1) {
            if (next_lo <= lo - 1)
                r = syn_append_range(p, r, next_lo, lo - 1);
            next_lo = hi + 1;
            continue;
        }
        for (Rune c = lo; c <= hi; c += stride) {
            if (next_lo <= c - 1)
                r = syn_append_range(p, r, next_lo, c - 1);
            next_lo = c + 1;
        }
    }
    if (next_lo <= UNICODE_MAX_RUNE)
        r = syn_append_range(p, r, next_lo, UNICODE_MAX_RUNE);
    return r;
}

/* negateClass: the complement of a sorted, clean class, in place. */
static Slice syn_negate_class(SynParser *p, Slice r) {
    Rune *q = SYN_RUNES(r);
    Rune next_lo = 0;
    Int w = 0;
    for (Int i = 0; i < r.len; i += 2) {
        Rune lo = q[i], hi = q[i + 1];
        if (next_lo <= lo - 1) {
            q[w] = next_lo;
            q[w + 1] = lo - 1;
            w += 2;
        }
        next_lo = hi + 1;
    }
    r.len = w;
    if (next_lo <= UNICODE_MAX_RUNE)
        r = syn_append2(p, r, next_lo, UNICODE_MAX_RUNE);
    return r;
}

/* ------------------------------------------------------------ perl groups */

typedef struct SynGroup {
    const char *name;
    int sign;
    const Rune *class;
    Int len;
} SynGroup;

static const Rune syn_code1[] = {0x30, 0x39};                        /* \d */
static const Rune syn_code2[] = {0x9, 0xa, 0xc, 0xd, 0x20, 0x20};    /* \s */
static const Rune syn_code3[] = {0x30, 0x39, 0x41, 0x5a, 0x5f, 0x5f, /* \w */
                                 0x61, 0x7a};
static const Rune syn_code4[] = {0x30, 0x39, 0x41, 0x5a, 0x61, 0x7a}; /* alnum */
static const Rune syn_code5[] = {0x41, 0x5a, 0x61, 0x7a};             /* alpha */
static const Rune syn_code6[] = {0x0, 0x7f};                          /* ascii */
static const Rune syn_code7[] = {0x9, 0x9, 0x20, 0x20};               /* blank */
static const Rune syn_code8[] = {0x0, 0x1f, 0x7f, 0x7f};              /* cntrl */
static const Rune syn_code9[] = {0x30, 0x39};                         /* digit */
static const Rune syn_code10[] = {0x21, 0x7e};                        /* graph */
static const Rune syn_code11[] = {0x61, 0x7a};                        /* lower */
static const Rune syn_code12[] = {0x20, 0x7e};                        /* print */
static const Rune syn_code13[] = {0x21, 0x2f, 0x3a, 0x40, 0x5b, 0x60, /* punct */
                                  0x7b, 0x7e};
static const Rune syn_code14[] = {0x9, 0xd, 0x20, 0x20};              /* space */
static const Rune syn_code15[] = {0x41, 0x5a};                        /* upper */
static const Rune syn_code16[] = {0x30, 0x39, 0x41, 0x5a, 0x5f, 0x5f, /* word */
                                  0x61, 0x7a};
static const Rune syn_code17[] = {0x30, 0x39, 0x41, 0x46, 0x61, 0x66}; /* xdigit */

#define SYN_GROUP(n, s, c) {n, s, c, (Int)(sizeof c / sizeof(Rune))}

static const SynGroup syn_perl_groups[] = {
    SYN_GROUP("\\d", +1, syn_code1), SYN_GROUP("\\D", -1, syn_code1),
    SYN_GROUP("\\s", +1, syn_code2), SYN_GROUP("\\S", -1, syn_code2),
    SYN_GROUP("\\w", +1, syn_code3), SYN_GROUP("\\W", -1, syn_code3),
};

static const SynGroup syn_posix_groups[] = {
    SYN_GROUP("[:alnum:]", +1, syn_code4),   SYN_GROUP("[:^alnum:]", -1, syn_code4),
    SYN_GROUP("[:alpha:]", +1, syn_code5),   SYN_GROUP("[:^alpha:]", -1, syn_code5),
    SYN_GROUP("[:ascii:]", +1, syn_code6),   SYN_GROUP("[:^ascii:]", -1, syn_code6),
    SYN_GROUP("[:blank:]", +1, syn_code7),   SYN_GROUP("[:^blank:]", -1, syn_code7),
    SYN_GROUP("[:cntrl:]", +1, syn_code8),   SYN_GROUP("[:^cntrl:]", -1, syn_code8),
    SYN_GROUP("[:digit:]", +1, syn_code9),   SYN_GROUP("[:^digit:]", -1, syn_code9),
    SYN_GROUP("[:graph:]", +1, syn_code10),  SYN_GROUP("[:^graph:]", -1, syn_code10),
    SYN_GROUP("[:lower:]", +1, syn_code11),  SYN_GROUP("[:^lower:]", -1, syn_code11),
    SYN_GROUP("[:print:]", +1, syn_code12),  SYN_GROUP("[:^print:]", -1, syn_code12),
    SYN_GROUP("[:punct:]", +1, syn_code13),  SYN_GROUP("[:^punct:]", -1, syn_code13),
    SYN_GROUP("[:space:]", +1, syn_code14),  SYN_GROUP("[:^space:]", -1, syn_code14),
    SYN_GROUP("[:upper:]", +1, syn_code15),  SYN_GROUP("[:^upper:]", -1, syn_code15),
    SYN_GROUP("[:word:]", +1, syn_code16),   SYN_GROUP("[:^word:]", -1, syn_code16),
    SYN_GROUP("[:xdigit:]", +1, syn_code17), SYN_GROUP("[:^xdigit:]", -1, syn_code17),
};

static const SynGroup *syn_find_group(const SynGroup *g, size_t n, Str name) {
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(g[i].name);
        if ((Int)len == name.len && memcmp(g[i].name, name.p, len) == 0)
            return &g[i];
    }
    return NULL;
}

/* appendGroup: adds a Perl or POSIX group to r, case folded when the flags
 * say so. */
static Slice syn_append_group(SynParser *p, Slice r, const SynGroup *g) {
    if ((p->flags & SYNTAX_FOLD_CASE) == 0) {
        if (g->sign < 0)
            return syn_append_negated_class(p, r, g->class, g->len);
        return syn_append_class(p, r, g->class, g->len);
    }
    Slice tmp = p->tmp_class;
    tmp.len = 0;
    tmp = syn_append_folded_class(p, tmp, g->class, g->len);
    p->tmp_class = tmp;
    tmp = syn_clean_class(&p->tmp_class);
    if (g->sign < 0)
        return syn_append_negated_class(p, r, SYN_RUNES(tmp), tmp.len);
    return syn_append_class(p, r, SYN_RUNES(tmp), tmp.len);
}

/* ------------------------------------------------------------ unicode groups */

static const UnicodeRange16 syn_any16[] = {{0, 0xFFFF, 1}};
static const UnicodeRange32 syn_any32[] = {{0x10000, 0x10FFFF, 1}};
static const UnicodeRangeTable syn_any_table = {syn_any16, 1, syn_any32, 1, 0};

static const UnicodeRange16 syn_ascii16[] = {{0, 0x7F, 1}};
static const UnicodeRangeTable syn_ascii_table = {syn_ascii16, 1, NULL, 0, 0};

/* ASCII and the two runes outside it that fold into it: the long s, which
 * folds to S and s, and the Kelvin sign, which folds to K and k. */
static const UnicodeRange16 syn_ascii_fold16[] = {
    {0, 0x7F, 1}, {0x017F, 0x017F, 1}, {0x212A, 0x212A, 1}};
static const UnicodeRangeTable syn_ascii_fold_table = {syn_ascii_fold16, 3, NULL, 0, 0};

/* canonicalName: the name with its first letter upper case, the rest lower
 * case, and every _, - and space gone, written into buf, which has room for
 * name.len bytes. */
static Str syn_canonical_name(Str name, Byte *buf) {
    Int n = 0;
    bool first = true;
    for (Int i = 0; i < name.len; i++) {
        Byte c = name.p[i];
        if (c == '_' || c == '-' || c == ' ')
            continue;
        if (first) {
            if ('a' <= c && c <= 'z')
                c -= 'a' - 'A';
            first = false;
        } else if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
        buf[n++] = c;
    }
    return str_from_bytes(buf, n);
}

static bool syn_str_is(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (Int)n == s.len && memcmp(s.p, lit, n) == 0;
}

/* unicodeTable: the table for a \p name, the table of what folds into it, and
 * whether the match is inverted. */
static int syn_unicode_table(SynParser *p, Str name0, const UnicodeRangeTable **tab,
                             const UnicodeRangeTable **fold) {
    Byte *buf = (Byte *)mem_alloc_nozero(p->sa, (size_t)name0.len + 1, 1);
    if (buf == NULL)
        syn_oom(p);
    Str name = syn_canonical_name(name0, buf);

    if (syn_str_is(name, "Any")) {
        *tab = *fold = &syn_any_table;
        return +1;
    }
    if (syn_str_is(name, "Assigned")) {
        *tab = *fold = unicode_cn; /* inverting Cn, the unassigned */
        return -1;
    }
    if (syn_str_is(name, "Ascii")) {
        *tab = &syn_ascii_table;
        *fold = &syn_ascii_fold_table;
        return +1;
    }
    if (syn_str_is(name, "Lc")) {
        *tab = unicode_table_map_get(unicode_categories, BURROW_S("LC"));
        *fold = unicode_table_map_get(unicode_fold_category, BURROW_S("LC"));
        return +1;
    }
    const UnicodeRangeTable *t = unicode_table_map_get(unicode_categories, name);
    if (t != NULL) {
        *tab = t;
        *fold = unicode_table_map_get(unicode_fold_category, name);
        return +1;
    }
    t = unicode_table_map_get(unicode_scripts, name);
    if (t != NULL) {
        *tab = t;
        *fold = unicode_table_map_get(unicode_fold_script, name);
        return +1;
    }

    /* The long names, compared in their canonical forms. Go builds maps of
     * these on first use; the lists are short enough to walk. */
    Byte cbuf[128];
    for (Int i = 0; i < unicode_category_aliases.len; i++) {
        Str alias = unicode_category_aliases.p[i].name;
        if (alias.len > (Int)sizeof cbuf)
            continue;
        if (str_eq(syn_canonical_name(alias, cbuf), name)) {
            Str actual = unicode_category_aliases.p[i].target;
            *tab = unicode_table_map_get(unicode_categories, actual);
            *fold = unicode_table_map_get(unicode_fold_category, actual);
            return +1;
        }
    }
    for (Int i = 0; i < unicode_scripts.len; i++) {
        Str script = unicode_scripts.p[i].name;
        if (script.len > (Int)sizeof cbuf)
            continue;
        if (str_eq(syn_canonical_name(script, cbuf), name)) {
            *tab = unicode_scripts.p[i].table;
            *fold = unicode_table_map_get(unicode_fold_script, script);
            return +1;
        }
    }
    *tab = *fold = NULL;
    return 0;
}

/* ------------------------------------------------------------------ runes */

static void syn_check_utf8(SynParser *p, Str s) {
    while (s.len > 0) {
        Int size;
        Rune r = utf8_decode_rune_in_string(s, &size);
        if (r == UTF8_RUNE_ERROR && size == 1)
            syn_fail(p, SYNTAX_ERR_INVALID_UTF8, s);
        s = syn_from(s, size);
    }
}

static Rune syn_next_rune(SynParser *p, Str s, Str *t) {
    Int size;
    Rune c = utf8_decode_rune_in_string(s, &size);
    if (c == UTF8_RUNE_ERROR && size == 1)
        syn_fail(p, SYNTAX_ERR_INVALID_UTF8, s);
    *t = syn_from(s, size);
    return c;
}

static bool syn_isalnum(Rune c) {
    return ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z');
}

static Rune syn_unhex(Rune c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* --------------------------------------------------------------- the stack */

static bool syn_is_char_class(const SyntaxRegexp *re) {
    return (re->op == SYNTAX_OP_LITERAL && re->rune.len == 1) ||
           re->op == SYNTAX_OP_CHAR_CLASS || re->op == SYNTAX_OP_ANY_CHAR_NOT_NL ||
           re->op == SYNTAX_OP_ANY_CHAR;
}

static bool syn_match_rune(const SyntaxRegexp *re, Rune r) {
    const Rune *q = SYN_RUNES(re->rune);
    switch (re->op) {
    case SYNTAX_OP_LITERAL:
        return re->rune.len == 1 && q[0] == r;
    case SYNTAX_OP_CHAR_CLASS:
        for (Int i = 0; i < re->rune.len; i += 2)
            if (q[i] <= r && r <= q[i + 1])
                return true;
        return false;
    case SYNTAX_OP_ANY_CHAR_NOT_NL:
        return r != '\n';
    case SYNTAX_OP_ANY_CHAR:
        return true;
    default:
        return false;
    }
}

/* maybeConcat: when the top two of the stack are literals with the same case
 * folding, moves the top one's runes onto the other. With r >= 0 the top one is
 * then reused for r with flags, which saves a pop and a push. */
static bool syn_maybe_concat(SynParser *p, Rune r, SyntaxFlags flags) {
    Int n = p->stack.len;
    if (n < 2)
        return false;
    SyntaxRegexp *re1 = syn_top(p, 1);
    SyntaxRegexp *re2 = syn_top(p, 2);
    if (re1->op != SYNTAX_OP_LITERAL || re2->op != SYNTAX_OP_LITERAL ||
        (re1->flags & SYNTAX_FOLD_CASE) != (re2->flags & SYNTAX_FOLD_CASE))
        return false;

    re2->rune = syn_append_runes(p, re2->rune, SYN_RUNES(re1->rune), re1->rune.len);

    if (r >= 0) {
        re1->rune = syn_rune0(re1, 1);
        re1->rune0[0] = r;
        re1->flags = flags;
        return true;
    }

    p->stack.len = n - 1;
    syn_reuse(p, re1);
    return false;
}

/* push: puts re on the stack, first turning a one rune class into a literal
 * and gluing literals together. Gives re, or NULL when re went into the
 * literal below it. */
static SyntaxRegexp *syn_push(SynParser *p, SyntaxRegexp *re) {
    p->num_runes += re->rune.len;
    Rune *q = SYN_RUNES(re->rune);
    if (re->op == SYNTAX_OP_CHAR_CLASS && re->rune.len == 2 && q[0] == q[1]) {
        /* A single rune. */
        if (syn_maybe_concat(p, q[0], (SyntaxFlags)(p->flags & ~SYNTAX_FOLD_CASE)))
            return NULL;
        re->op = SYNTAX_OP_LITERAL;
        re->rune.len = 1;
        re->flags = (SyntaxFlags)(p->flags & ~SYNTAX_FOLD_CASE);
    } else if ((re->op == SYNTAX_OP_CHAR_CLASS && re->rune.len == 4 && q[0] == q[1] &&
                q[2] == q[3] && unicode_simple_fold(q[0]) == q[2] &&
                unicode_simple_fold(q[2]) == q[0]) ||
               (re->op == SYNTAX_OP_CHAR_CLASS && re->rune.len == 2 &&
                q[0] + 1 == q[1] && unicode_simple_fold(q[0]) == q[1] &&
                unicode_simple_fold(q[1]) == q[0])) {
        /* A case folded rune, like [Aa] or [Kk]. */
        if (syn_maybe_concat(p, q[0], (SyntaxFlags)(p->flags | SYNTAX_FOLD_CASE)))
            return NULL;
        re->op = SYNTAX_OP_LITERAL;
        re->rune.len = 1;
        re->flags = (SyntaxFlags)(p->flags | SYNTAX_FOLD_CASE);
    } else {
        /* Anything else: glue the literals below it together first. */
        syn_maybe_concat(p, -1, 0);
    }

    p->stack = syn_append_sub(p, p->stack, re);
    syn_check_limits(p, re);
    return re;
}

/* minFoldRune: the smallest rune r case folds to. */
static Rune syn_min_fold_rune(Rune r) {
    if (r < SYN_MIN_FOLD || r > SYN_MAX_FOLD)
        return r;
    Rune m = r, r0 = r;
    for (r = unicode_simple_fold(r); r != r0; r = unicode_simple_fold(r))
        if (r < m)
            m = r;
    return m;
}

static void syn_literal(SynParser *p, Rune r) {
    SyntaxRegexp *re = syn_new(p, SYNTAX_OP_LITERAL);
    re->flags = p->flags;
    if (p->flags & SYNTAX_FOLD_CASE)
        r = syn_min_fold_rune(r);
    re->rune0[0] = r;
    re->rune = syn_rune0(re, 1);
    syn_push(p, re);
}

static SyntaxRegexp *syn_op(SynParser *p, SyntaxOp op) {
    SyntaxRegexp *re = syn_new(p, op);
    re->flags = p->flags;
    return syn_push(p, re);
}

/* repeatIsValid: whether the nested counted repetitions in re multiply out to
 * no more than n. */
static bool syn_repeat_is_valid(const SyntaxRegexp *re, Int n) {
    if (re->op == SYNTAX_OP_REPEAT) {
        Int m = re->max;
        if (m == 0)
            return true;
        if (m < 0)
            m = re->min;
        if (m > n)
            return false;
        if (m > 0)
            n /= m;
    }
    for (Int i = 0; i < re->sub.len; i++)
        if (!syn_repeat_is_valid(SYN_SUBS(re->sub)[i], n))
            return false;
    return true;
}

/* repeat: applies a repetition to the top of the stack. before is the text
 * from the operator on, after the text after it, and last_repeat the text of
 * the repetition just before this one, if this follows one. Gives the text to
 * go on with. */
static Str syn_repeat(SynParser *p, SyntaxOp op, Int min, Int max, Str before,
                      Str after, Str last_repeat) {
    SyntaxFlags flags = p->flags;
    if (p->flags & SYNTAX_PERL_X) {
        if (after.len > 0 && after.p[0] == '?') {
            after = syn_from(after, 1);
            flags ^= SYNTAX_NON_GREEDY;
        }
        if (last_repeat.len != 0) {
            /* Perl allows x** but it means (x*)*, which is surely not what
             * anyone meant, so it is an error. */
            syn_fail(p, SYNTAX_ERR_INVALID_REPEAT_OP,
                     syn_to(last_repeat, last_repeat.len - after.len));
        }
    }
    Int n = p->stack.len;
    if (n == 0)
        syn_fail(p, SYNTAX_ERR_MISSING_REPEAT_ARGUMENT, syn_upto(before, after));
    SyntaxRegexp *sub = syn_top(p, 1);
    if (sub->op >= SYN_OP_PSEUDO)
        syn_fail(p, SYNTAX_ERR_MISSING_REPEAT_ARGUMENT, syn_upto(before, after));

    SyntaxRegexp *re = syn_new(p, op);
    re->min = min;
    re->max = max;
    re->flags = flags;
    re->sub = syn_sub0(re, 1);
    re->sub0[0] = sub;
    SYN_SUBS(p->stack)[n - 1] = re;
    syn_check_limits(p, re);

    if (op == SYNTAX_OP_REPEAT && (min >= 2 || max >= 2) &&
        !syn_repeat_is_valid(re, 1000))
        syn_fail(p, SYNTAX_ERR_INVALID_REPEAT_SIZE, syn_upto(before, after));
    return after;
}

/* ------------------------------------------------------------- alternation */

static Slice syn_factor(SynParser *p, Slice sub);

/* cleanAlt: tidies a class that is about to become one branch of an
 * alternation, turning the classes of everything into the dot operators. */
static void syn_clean_alt(SynParser *p, SyntaxRegexp *re) {
    if (re->op != SYNTAX_OP_CHAR_CLASS)
        return;
    re->rune = syn_clean_class(&re->rune);
    const Rune *q = SYN_RUNES(re->rune);
    if (re->rune.len == 2 && q[0] == 0 && q[1] == UNICODE_MAX_RUNE) {
        re->rune = (Slice){0};
        re->op = SYNTAX_OP_ANY_CHAR;
        return;
    }
    if (re->rune.len == 4 && q[0] == 0 && q[1] == '\n' - 1 && q[2] == '\n' + 1 &&
        q[3] == UNICODE_MAX_RUNE) {
        re->rune = (Slice){0};
        re->op = SYNTAX_OP_ANY_CHAR_NOT_NL;
        return;
    }
    if (re->rune.cap - re->rune.len > 100) {
        /* A lot of room left over from building the class. Copy it to
         * something smaller. */
        re->rune = syn_append_runes(p, syn_rune0(re, 0), q, re->rune.len);
    }
}

/* collapse: the subs joined with op, where a sub that is already op has its
 * own subs spliced in. */
static SyntaxRegexp *syn_collapse(SynParser *p, Slice subs, SyntaxOp op) {
    if (subs.len == 1)
        return SYN_SUBS(subs)[0];
    SyntaxRegexp *re = syn_new(p, op);
    re->sub = syn_sub0(re, 0);
    for (Int i = 0; i < subs.len; i++) {
        SyntaxRegexp *sub = SYN_SUBS(subs)[i];
        if (sub->op == op) {
            re->sub = syn_append_subs(p, re->sub, SYN_SUBS(sub->sub), sub->sub.len);
            syn_reuse(p, sub);
        } else {
            re->sub = syn_append_sub(p, re->sub, sub);
        }
    }
    if (op == SYNTAX_OP_ALTERNATE) {
        re->sub = syn_factor(p, re->sub);
        if (re->sub.len == 1) {
            SyntaxRegexp *old = re;
            re = SYN_SUBS(re->sub)[0];
            syn_reuse(p, old);
        }
    }
    return re;
}

/* leadingString: the literal runes re starts with, and their case folding. */
static Slice syn_leading_string(const SyntaxRegexp *re, SyntaxFlags *flags) {
    if (re->op == SYNTAX_OP_CONCAT && re->sub.len > 0)
        re = SYN_SUBS(re->sub)[0];
    if (re->op != SYNTAX_OP_LITERAL) {
        *flags = 0;
        return (Slice){0};
    }
    *flags = re->flags & SYNTAX_FOLD_CASE;
    return re->rune;
}

/* removeLeadingString: re without its first n literal runes. */
static SyntaxRegexp *syn_remove_leading_string(SynParser *p, SyntaxRegexp *re, Int n) {
    if (re->op == SYNTAX_OP_CONCAT && re->sub.len > 0) {
        /* Take them off the first sub. That can leave the first sub empty,
         * and then it goes. */
        SyntaxRegexp **s = SYN_SUBS(re->sub);
        SyntaxRegexp *sub = syn_remove_leading_string(p, s[0], n);
        s[0] = sub;
        if (sub->op == SYNTAX_OP_EMPTY_MATCH) {
            syn_reuse(p, sub);
            switch (re->sub.len) {
            case 0:
            case 1:
                /* Cannot happen, but be safe. */
                re->op = SYNTAX_OP_EMPTY_MATCH;
                re->sub = (Slice){0};
                break;
            case 2: {
                SyntaxRegexp *old = re;
                re = s[1];
                syn_reuse(p, old);
                break;
            }
            default:
                memmove(s, s + 1, (size_t)(re->sub.len - 1) * sizeof *s);
                re->sub.len--;
                break;
            }
        }
        return re;
    }

    if (re->op == SYNTAX_OP_LITERAL) {
        Rune *q = SYN_RUNES(re->rune);
        Int keep = re->rune.len - n;
        if (keep > 0)
            memmove(q, q + n, (size_t)keep * sizeof(Rune));
        re->rune.len = keep;
        if (keep == 0)
            re->op = SYNTAX_OP_EMPTY_MATCH;
    }
    return re;
}

/* leadingRegexp: the node re starts with, or NULL when that is an empty
 * match. */
static SyntaxRegexp *syn_leading_regexp(SyntaxRegexp *re) {
    if (re->op == SYNTAX_OP_EMPTY_MATCH)
        return NULL;
    if (re->op == SYNTAX_OP_CONCAT && re->sub.len > 0) {
        SyntaxRegexp *sub = SYN_SUBS(re->sub)[0];
        if (sub->op == SYNTAX_OP_EMPTY_MATCH)
            return NULL;
        return sub;
    }
    return re;
}

/* removeLeadingRegexp: re without the node it starts with, which goes on the
 * free list when reuse says so. */
static SyntaxRegexp *syn_remove_leading_regexp(SynParser *p, SyntaxRegexp *re,
                                               bool reuse) {
    if (re->op == SYNTAX_OP_CONCAT && re->sub.len > 0) {
        SyntaxRegexp **s = SYN_SUBS(re->sub);
        if (reuse)
            syn_reuse(p, s[0]);
        memmove(s, s + 1, (size_t)(re->sub.len - 1) * sizeof *s);
        re->sub.len--;
        switch (re->sub.len) {
        case 0:
            re->op = SYNTAX_OP_EMPTY_MATCH;
            re->sub = (Slice){0};
            break;
        case 1: {
            SyntaxRegexp *old = re;
            re = s[0];
            syn_reuse(p, old);
            break;
        }
        default:
            break;
        }
        return re;
    }
    if (reuse)
        syn_reuse(p, re);
    return syn_new(p, SYNTAX_OP_EMPTY_MATCH);
}

static void syn_merge_char_class(SynParser *p, SyntaxRegexp *dst, SyntaxRegexp *src);

/* factor: shortens an alternation by pulling out common prefixes, in place,
 * as Go does it:
 *
 *     ABC|ABD|AEF|BCX|BCY  becomes  A(B(C|D)|EF)|BC(X|Y)
 *
 * then merging runs of single characters into one class. */
static Slice syn_factor(SynParser *p, Slice sub) {
    if (sub.len < 2)
        return sub;
    SyntaxRegexp **s = SYN_SUBS(sub);

    /* Round 1: common literal prefixes. */
    Slice str = {0};
    SyntaxFlags strflags = 0;
    Int start = 0;
    Slice out = sub;
    out.len = 0;
    for (Int i = 0; i <= sub.len; i++) {
        /* Invariant: s[start:i] all begin with str, which has strflags. */
        Slice istr = {0};
        SyntaxFlags iflags = 0;
        if (i < sub.len) {
            istr = syn_leading_string(s[i], &iflags);
            if (iflags == strflags) {
                Int same = 0;
                const Rune *a = SYN_RUNES(str), *b = SYN_RUNES(istr);
                while (same < str.len && same < istr.len && a[same] == b[same])
                    same++;
                if (same > 0) {
                    /* Matches at least one rune of the current prefix. Keep
                     * going around. */
                    str.len = same;
                    continue;
                }
            }
        }

        /* s[start:i] share str, and s[i] does not. */
        if (i == start) {
            /* Nothing to do. */
        } else if (i == start + 1) {
            out = syn_append_sub(p, out, s[start]);
        } else {
            SyntaxRegexp *prefix = syn_new(p, SYNTAX_OP_LITERAL);
            prefix->flags = strflags;
            Slice pr = prefix->rune;
            pr.len = 0;
            prefix->rune = syn_append_runes(p, pr, SYN_RUNES(str), str.len);

            for (Int j = start; j < i; j++) {
                s[j] = syn_remove_leading_string(p, s[j], str.len);
                syn_check_limits(p, s[j]);
            }
            Slice part = {s + start, i - start, sub.cap - start, TYPE_UNSAFE_POINTER};
            SyntaxRegexp *suffix = syn_collapse(p, part, SYNTAX_OP_ALTERNATE);

            SyntaxRegexp *re = syn_new(p, SYNTAX_OP_CONCAT);
            SyntaxRegexp *two[2] = {prefix, suffix};
            Slice rs = re->sub;
            rs.len = 0;
            re->sub = syn_append_subs(p, rs, two, 2);
            out = syn_append_sub(p, out, re);
        }

        start = i;
        str = istr;
        strflags = iflags;
    }
    sub = out;
    s = SYN_SUBS(sub);

    /* Round 2: common simple prefixes, just the first piece of each concat,
     * and only when it is a class or a fixed repeat of one. */
    start = 0;
    out = sub;
    out.len = 0;
    SyntaxRegexp *first = NULL;
    for (Int i = 0; i <= sub.len; i++) {
        SyntaxRegexp *ifirst = NULL;
        if (i < sub.len) {
            ifirst = syn_leading_regexp(s[i]);
            if (first != NULL && syntax_regexp_equal(first, ifirst) &&
                (syn_is_char_class(first) ||
                 (first->op == SYNTAX_OP_REPEAT && first->min == first->max &&
                  syn_is_char_class(SYN_SUBS(first->sub)[0]))))
                continue;
        }

        if (i == start) {
            /* Nothing to do. */
        } else if (i == start + 1) {
            out = syn_append_sub(p, out, s[start]);
        } else {
            SyntaxRegexp *prefix = first;
            for (Int j = start; j < i; j++) {
                bool reuse = j != start; /* prefix came from s[start] */
                s[j] = syn_remove_leading_regexp(p, s[j], reuse);
                syn_check_limits(p, s[j]);
            }
            Slice part = {s + start, i - start, sub.cap - start, TYPE_UNSAFE_POINTER};
            SyntaxRegexp *suffix = syn_collapse(p, part, SYNTAX_OP_ALTERNATE);

            SyntaxRegexp *re = syn_new(p, SYNTAX_OP_CONCAT);
            SyntaxRegexp *two[2] = {prefix, suffix};
            Slice rs = re->sub;
            rs.len = 0;
            re->sub = syn_append_subs(p, rs, two, 2);
            out = syn_append_sub(p, out, re);
        }

        start = i;
        first = ifirst;
    }
    sub = out;
    s = SYN_SUBS(sub);

    /* Round 3: runs of single characters become one class. */
    start = 0;
    out = sub;
    out.len = 0;
    for (Int i = 0; i <= sub.len; i++) {
        if (i < sub.len && syn_is_char_class(s[i]))
            continue;

        if (i == start) {
            /* Nothing to do. */
        } else if (i == start + 1) {
            out = syn_append_sub(p, out, s[start]);
        } else {
            /* Make the widest one the one the others merge into. */
            Int max = start;
            for (Int j = start + 1; j < i; j++)
                if (s[max]->op < s[j]->op ||
                    (s[max]->op == s[j]->op && s[max]->rune.len < s[j]->rune.len))
                    max = j;
            SyntaxRegexp *t = s[start];
            s[start] = s[max];
            s[max] = t;

            for (Int j = start + 1; j < i; j++) {
                syn_merge_char_class(p, s[start], s[j]);
                syn_reuse(p, s[j]);
            }
            syn_clean_alt(p, s[start]);
            out = syn_append_sub(p, out, s[start]);
        }

        if (i < sub.len)
            out = syn_append_sub(p, out, s[i]);
        start = i + 1;
    }
    sub = out;
    s = SYN_SUBS(sub);

    /* Round 4: no two empty matches in a row. */
    out = sub;
    out.len = 0;
    for (Int i = 0; i < sub.len; i++) {
        if (i + 1 < sub.len && s[i]->op == SYNTAX_OP_EMPTY_MATCH &&
            s[i + 1]->op == SYNTAX_OP_EMPTY_MATCH)
            continue;
        out = syn_append_sub(p, out, s[i]);
    }
    return out;
}

/* mergeCharClass: makes dst match what src matches too. */
static void syn_merge_char_class(SynParser *p, SyntaxRegexp *dst, SyntaxRegexp *src) {
    switch (dst->op) {
    case SYNTAX_OP_ANY_CHAR:
        break;
    case SYNTAX_OP_ANY_CHAR_NOT_NL:
        if (syn_match_rune(src, '\n'))
            dst->op = SYNTAX_OP_ANY_CHAR;
        break;
    case SYNTAX_OP_CHAR_CLASS:
        if (src->op == SYNTAX_OP_LITERAL)
            dst->rune =
                syn_append_literal(p, dst->rune, SYN_RUNES(src->rune)[0], src->flags);
        else
            dst->rune =
                syn_append_class(p, dst->rune, SYN_RUNES(src->rune), src->rune.len);
        break;
    case SYNTAX_OP_LITERAL: {
        Rune d = SYN_RUNES(dst->rune)[0];
        if (SYN_RUNES(src->rune)[0] == d && src->flags == dst->flags)
            break;
        dst->op = SYNTAX_OP_CHAR_CLASS;
        Slice r = dst->rune;
        r.len = 0;
        dst->rune = syn_append_literal(p, r, d, dst->flags);
        dst->rune =
            syn_append_literal(p, dst->rune, SYN_RUNES(src->rune)[0], src->flags);
        break;
    }
    default:
        break;
    }
}

/* concat: replaces the stack above the last marker with its concatenation. */
static SyntaxRegexp *syn_concat(SynParser *p) {
    syn_maybe_concat(p, -1, 0);

    Int i = p->stack.len;
    SyntaxRegexp **s = SYN_SUBS(p->stack);
    while (i > 0 && s[i - 1]->op < SYN_OP_PSEUDO)
        i--;
    Slice subs = {s == NULL ? NULL : s + i, p->stack.len - i, p->stack.cap - i,
                  TYPE_UNSAFE_POINTER};
    p->stack.len = i;

    if (subs.len == 0)
        return syn_push(p, syn_new(p, SYNTAX_OP_EMPTY_MATCH));
    return syn_push(p, syn_collapse(p, subs, SYNTAX_OP_CONCAT));
}

/* alternate: replaces the stack above the last marker with its
 * alternation. */
static SyntaxRegexp *syn_alternate(SynParser *p) {
    Int i = p->stack.len;
    SyntaxRegexp **s = SYN_SUBS(p->stack);
    while (i > 0 && s[i - 1]->op < SYN_OP_PSEUDO)
        i--;
    Slice subs = {s == NULL ? NULL : s + i, p->stack.len - i, p->stack.cap - i,
                  TYPE_UNSAFE_POINTER};
    p->stack.len = i;

    /* The last one might be a class that swapVerticalBar left untidy. */
    if (subs.len > 0)
        syn_clean_alt(p, SYN_SUBS(subs)[subs.len - 1]);

    if (subs.len == 0)
        return syn_push(p, syn_new(p, SYNTAX_OP_NO_MATCH));
    return syn_push(p, syn_collapse(p, subs, SYNTAX_OP_ALTERNATE));
}

/* swapVerticalBar: when the stack ends class, bar, class, merges the two
 * classes. When it ends x, bar, swaps them so the bar stays on top. Gives
 * whether it did either. */
static bool syn_swap_vertical_bar(SynParser *p) {
    Int n = p->stack.len;
    SyntaxRegexp **s = SYN_SUBS(p->stack);
    if (n >= 3 && s[n - 2]->op == SYN_OP_VERTICAL_BAR && syn_is_char_class(s[n - 1]) &&
        syn_is_char_class(s[n - 3])) {
        SyntaxRegexp *re1 = s[n - 1];
        SyntaxRegexp *re3 = s[n - 3];
        /* Make re3 the more complex of the two. */
        if (re1->op > re3->op) {
            SyntaxRegexp *t = re1;
            re1 = re3;
            re3 = t;
            s[n - 3] = re3;
        }
        syn_merge_char_class(p, re3, re1);
        syn_reuse(p, re1);
        p->stack.len = n - 1;
        return true;
    }

    if (n >= 2) {
        SyntaxRegexp *re1 = s[n - 1];
        SyntaxRegexp *re2 = s[n - 2];
        if (re2->op == SYN_OP_VERTICAL_BAR) {
            if (n >= 3) {
                /* Now out of reach. Clean it up. */
                syn_clean_alt(p, s[n - 3]);
            }
            s[n - 2] = re1;
            s[n - 1] = re2;
            return true;
        }
    }
    return false;
}

static void syn_parse_vertical_bar(SynParser *p) {
    syn_concat(p);
    if (!syn_swap_vertical_bar(p))
        syn_op(p, SYN_OP_VERTICAL_BAR);
}

static void syn_parse_right_paren(SynParser *p) {
    syn_concat(p);
    if (syn_swap_vertical_bar(p))
        p->stack.len--;
    syn_alternate(p);

    Int n = p->stack.len;
    if (n < 2)
        syn_fail(p, SYNTAX_ERR_UNEXPECTED_PAREN, p->whole);
    SyntaxRegexp *re1 = syn_top(p, 1);
    SyntaxRegexp *re2 = syn_top(p, 2);
    p->stack.len = n - 2;
    if (re2->op != SYN_OP_LEFT_PAREN)
        syn_fail(p, SYNTAX_ERR_UNEXPECTED_PAREN, p->whole);
    /* Back to the flags from before the group. */
    p->flags = re2->flags;
    if (re2->cap == 0) {
        /* Just for grouping. */
        syn_push(p, re1);
    } else {
        re2->op = SYNTAX_OP_CAPTURE;
        re2->sub = syn_sub0(re2, 1);
        re2->sub0[0] = re1;
        syn_push(p, re2);
    }
}

/* ------------------------------------------------------------- the syntax */

/* parseInt: a decimal number with no leading zero, -1 when it is 1e8 or
 * more. */
static bool syn_parse_int(Str s, Int *n, Str *rest) {
    *n = 0;
    if (s.len == 0 || s.p[0] < '0' || '9' < s.p[0])
        return false;
    /* No leading zeros. */
    if (s.len >= 2 && s.p[0] == '0' && '0' <= s.p[1] && s.p[1] <= '9')
        return false;
    Str t = s;
    while (s.len > 0 && '0' <= s.p[0] && s.p[0] <= '9')
        s = syn_from(s, 1);
    *rest = s;
    t = syn_upto(t, s);
    for (Int i = 0; i < t.len; i++) {
        if (*n >= 100000000) {
            *n = -1;
            break;
        }
        *n = *n * 10 + (Int)t.p[i] - '0';
    }
    return true;
}

/* parseRepeat: {min}, {min,} or {min,max} at the start of s. */
static bool syn_parse_repeat(Str s, Int *min, Int *max, Str *rest) {
    *min = *max = 0;
    if (s.len == 0 || s.p[0] != '{')
        return false;
    s = syn_from(s, 1);
    if (!syn_parse_int(s, min, &s))
        return false;
    if (s.len == 0)
        return false;
    if (s.p[0] != ',') {
        *max = *min;
    } else {
        s = syn_from(s, 1);
        if (s.len == 0)
            return false;
        if (s.p[0] == '}') {
            *max = -1;
        } else if (!syn_parse_int(s, max, &s)) {
            return false;
        } else if (*max < 0) {
            /* parseInt found too big a number. */
            *min = -1;
        }
    }
    if (s.len == 0 || s.p[0] != '}')
        return false;
    *rest = syn_from(s, 1);
    return true;
}

static bool syn_is_valid_capture_name(Str name) {
    if (name.len == 0)
        return false;
    while (name.len > 0) {
        Int size;
        Rune c = utf8_decode_rune_in_string(name, &size);
        if (c != '_' && !syn_isalnum(c))
            return false;
        name = syn_from(name, size);
    }
    return true;
}

static Int syn_index_byte(Str s, Byte c) {
    const void *q = s.len > 0 ? memchr(s.p, c, (size_t)s.len) : NULL;
    return q == NULL ? -1 : (Int)((const Byte *)q - s.p);
}

/* parsePerlFlags: a (? group at the start of s, which is either a named
 * capture or a flag setting like (?i) or (?i:. */
static Str syn_parse_perl_flags(SynParser *p, Str s) {
    Str t = s;

    /* Named captures, (?P<name>expr) and (?<name>expr). */
    bool starts_with_p = t.len > 4 && t.p[2] == 'P' && t.p[3] == '<';
    bool starts_with_name = t.len > 3 && t.p[2] == '<';
    if (starts_with_p || starts_with_name) {
        Int expr_start = starts_with_name ? 3 : 4;
        Int end = syn_index_byte(t, '>');
        if (end < 0) {
            syn_check_utf8(p, t);
            syn_fail(p, SYNTAX_ERR_INVALID_NAMED_CAPTURE, s);
        }
        Str capture = syn_to(t, end + 1);                     /* "(?P<name>" */
        Str name = (Str){t.p + expr_start, end - expr_start}; /* "name" */
        syn_check_utf8(p, name);
        if (!syn_is_valid_capture_name(name))
            syn_fail(p, SYNTAX_ERR_INVALID_NAMED_CAPTURE, capture);

        p->num_cap++;
        SyntaxRegexp *re = syn_op(p, SYN_OP_LEFT_PAREN);
        re->cap = p->num_cap;
        re->name = name;
        return syn_from(t, end + 1);
    }

    /* Flags, (?i) or (?i:. */
    t = syn_from(t, 2);
    SyntaxFlags flags = p->flags;
    int sign = +1;
    bool saw_flag = false;
    while (t.len > 0) {
        Rune c = syn_next_rune(p, t, &t);
        switch (c) {
        case 'i':
            flags |= SYNTAX_FOLD_CASE;
            saw_flag = true;
            continue;
        case 'm':
            flags &= (SyntaxFlags)~SYNTAX_ONE_LINE;
            saw_flag = true;
            continue;
        case 's':
            flags |= SYNTAX_DOT_NL;
            saw_flag = true;
            continue;
        case 'U':
            flags |= SYNTAX_NON_GREEDY;
            saw_flag = true;
            continue;
        case '-':
            if (sign < 0)
                break;
            sign = -1;
            /* Invert the flags, so the ones that follow are set in the
             * complement and inverted back at the end. */
            flags = (SyntaxFlags)~flags;
            saw_flag = false;
            continue;
        case ':':
        case ')':
            if (sign < 0) {
                if (!saw_flag)
                    break;
                flags = (SyntaxFlags)~flags;
            }
            if (c == ':') {
                /* Open a group. */
                syn_op(p, SYN_OP_LEFT_PAREN);
            }
            p->flags = flags;
            return t;
        default:
            break;
        }
        break;
    }
    syn_fail(p, SYNTAX_ERR_INVALID_PERL_OP, syn_upto(s, t));
}

/* parseEscape: the escape at the start of s, which begins with a
 * backslash. */
static Rune syn_parse_escape(SynParser *p, Str s, Str *rest) {
    Str t = syn_from(s, 1);
    if (t.len == 0)
        syn_fail(p, SYNTAX_ERR_TRAILING_BACKSLASH, BURROW_STR_EMPTY);
    Rune c = syn_next_rune(p, t, &t);
    Rune r;

    switch (c) {
    default:
        if (c < UTF8_RUNE_SELF && !syn_isalnum(c)) {
            /* Escaped punctuation stands for itself. Perl lets any character
             * be escaped, but that way lies trouble. */
            *rest = t;
            return c;
        }
        break;

    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        /* A single non-zero digit is a backreference, which is not
         * supported. */
        if (t.len == 0 || t.p[0] < '0' || t.p[0] > '7')
            break;
        /* Fall through. */
    case '0':
        /* Up to three octal digits in all. */
        r = c - '0';
        for (int i = 1; i < 3; i++) {
            if (t.len == 0 || t.p[0] < '0' || t.p[0] > '7')
                break;
            r = r * 8 + (Rune)t.p[0] - '0';
            t = syn_from(t, 1);
        }
        *rest = t;
        return r;

    case 'x': {
        /* Hex, \x{10FFFF} or \xFF. */
        if (t.len == 0)
            break;
        c = syn_next_rune(p, t, &t);
        if (c == '{') {
            /* Any number of digits in braces, up to the largest rune. */
            Int nhex = 0;
            r = 0;
            for (;;) {
                if (t.len == 0)
                    goto bad;
                c = syn_next_rune(p, t, &t);
                if (c == '}')
                    break;
                Rune v = syn_unhex(c);
                if (v < 0)
                    goto bad;
                r = r * 16 + v;
                if (r > UNICODE_MAX_RUNE)
                    goto bad;
                nhex++;
            }
            if (nhex == 0)
                goto bad;
            *rest = t;
            return r;
        }

        /* Exactly two digits. */
        Rune x = syn_unhex(c);
        c = syn_next_rune(p, t, &t);
        Rune y = syn_unhex(c);
        if (x < 0 || y < 0)
            break;
        *rest = t;
        return x * 16 + y;
    }

    /* The C escapes. There is no \b, because it is the word boundary, and
     * no \e, which Perl has and C does not. */
    case 'a':
        *rest = t;
        return '\a';
    case 'f':
        *rest = t;
        return '\f';
    case 'n':
        *rest = t;
        return '\n';
    case 'r':
        *rest = t;
        return '\r';
    case 't':
        *rest = t;
        return '\t';
    case 'v':
        *rest = t;
        return '\v';
    }
bad:
    syn_fail(p, SYNTAX_ERR_INVALID_ESCAPE, syn_upto(s, t));
}

/* parseClassChar: one character of a class, escaped or not. */
static Rune syn_parse_class_char(SynParser *p, Str s, Str whole_class, Str *rest) {
    if (s.len == 0)
        syn_fail(p, SYNTAX_ERR_MISSING_BRACKET, whole_class);
    if (s.p[0] == '\\')
        return syn_parse_escape(p, s, rest);
    return syn_next_rune(p, s, rest);
}

/* parsePerlClassEscape: \d and the rest at the start of s, added to r. */
static bool syn_parse_perl_class_escape(SynParser *p, Str s, Slice r, Slice *out,
                                        Str *rest) {
    if ((p->flags & SYNTAX_PERL_X) == 0 || s.len < 2 || s.p[0] != '\\')
        return false;
    const SynGroup *g =
        syn_find_group(syn_perl_groups,
                       sizeof syn_perl_groups / sizeof *syn_perl_groups, syn_to(s, 2));
    if (g == NULL)
        return false;
    *out = syn_append_group(p, r, g);
    *rest = syn_from(s, 2);
    return true;
}

/* parseNamedClass: [:alnum:] and the rest at the start of s, added to r. */
static bool syn_parse_named_class(SynParser *p, Str s, Slice r, Slice *out, Str *rest) {
    if (s.len < 2 || s.p[0] != '[' || s.p[1] != ':')
        return false;
    Int i = -1;
    for (Int k = 2; k + 1 < s.len; k++) {
        if (s.p[k] == ':' && s.p[k + 1] == ']') {
            i = k;
            break;
        }
    }
    if (i < 0)
        return false;
    Str name = syn_to(s, i + 2);
    s = syn_from(s, i + 2);
    const SynGroup *g = syn_find_group(
        syn_posix_groups, sizeof syn_posix_groups / sizeof *syn_posix_groups, name);
    if (g == NULL)
        syn_fail(p, SYNTAX_ERR_INVALID_CHAR_RANGE, name);
    *out = syn_append_group(p, r, g);
    *rest = s;
    return true;
}

/* parseUnicodeClass: \pN, \p{Han} or \P{^Greek} at the start of s, added to
 * r. */
static bool syn_parse_unicode_class(SynParser *p, Str s, Slice r, Slice *out,
                                    Str *rest) {
    if ((p->flags & SYNTAX_UNICODE_GROUPS) == 0 || s.len < 2 || s.p[0] != '\\' ||
        (s.p[1] != 'p' && s.p[1] != 'P'))
        return false;

    int sign = s.p[1] == 'P' ? -1 : +1;
    Str t = syn_from(s, 2);
    Rune c = syn_next_rune(p, t, &t);
    Str seq, name;
    if (c != '{') {
        /* A single letter name. */
        seq = syn_upto(s, t);
        name = syn_from(seq, 2);
    } else {
        /* A name in braces. */
        Int end = syn_index_byte(s, '}');
        if (end < 0) {
            syn_check_utf8(p, s);
            syn_fail(p, SYNTAX_ERR_INVALID_CHAR_RANGE, s);
        }
        seq = syn_to(s, end + 1);
        t = syn_from(s, end + 1);
        name = (Str){s.p + 3, end - 3};
        syn_check_utf8(p, name);
    }

    /* \p{^Greek} is \P{Greek}. */
    if (name.len > 0 && name.p[0] == '^') {
        sign = -sign;
        name = syn_from(name, 1);
    }

    const UnicodeRangeTable *tab, *fold;
    int tsign = syn_unicode_table(p, name, &tab, &fold);
    if (tab == NULL)
        syn_fail(p, SYNTAX_ERR_INVALID_CHAR_RANGE, seq);
    if (tsign < 0)
        sign = -sign;

    if ((p->flags & SYNTAX_FOLD_CASE) == 0 || fold == NULL) {
        if (sign > 0)
            r = syn_append_table(p, r, tab);
        else
            r = syn_append_negated_table(p, r, tab);
    } else {
        /* Merge and clean tab and fold in a scratch class. */
        Slice tmp = p->tmp_class;
        tmp.len = 0;
        tmp = syn_append_table(p, tmp, tab);
        tmp = syn_append_table(p, tmp, fold);
        p->tmp_class = tmp;
        tmp = syn_clean_class(&p->tmp_class);
        if (sign > 0)
            r = syn_append_class(p, r, SYN_RUNES(tmp), tmp.len);
        else
            r = syn_append_negated_class(p, r, SYN_RUNES(tmp), tmp.len);
    }
    *out = r;
    *rest = t;
    return true;
}

/* parseClass: a bracketed class at the start of s. */
static Str syn_parse_class(SynParser *p, Str s) {
    Str t = syn_from(s, 1); /* chop [ */
    SyntaxRegexp *re = syn_new(p, SYNTAX_OP_CHAR_CLASS);
    re->flags = p->flags;
    re->rune = syn_rune0(re, 0);

    int sign = +1;
    if (t.len > 0 && t.p[0] == '^') {
        sign = -1;
        t = syn_from(t, 1);
        /* When the class is negated, it cannot match a newline unless the
         * flags allow it: put the newline in, to be negated out. */
        if ((p->flags & SYNTAX_CLASS_NL) == 0)
            re->rune = syn_append2(p, re->rune, '\n', '\n');
    }

    Slice class = re->rune;
    bool first = true; /* ] and - are fine as the first character */
    while (t.len == 0 || t.p[0] != ']' || first) {
        /* POSIX says - is only fine unescaped at the start or end of a
         * class. Perl lets it be anywhere, and so does PerlX. */
        if (t.len > 0 && t.p[0] == '-' && (p->flags & SYNTAX_PERL_X) == 0 && !first &&
            (t.len == 1 || t.p[1] != ']')) {
            Int size;
            utf8_decode_rune_in_string(syn_from(t, 1), &size);
            syn_fail(p, SYNTAX_ERR_INVALID_CHAR_RANGE, syn_to(t, 1 + size));
        }
        first = false;

        Slice nclass;
        Str nt;

        /* [:alnum:] */
        if (t.len > 2 && t.p[0] == '[' && t.p[1] == ':') {
            if (syn_parse_named_class(p, t, class, &nclass, &nt)) {
                class = nclass;
                t = nt;
                continue;
            }
        }

        /* \p{Han} */
        if (syn_parse_unicode_class(p, t, class, &nclass, &nt)) {
            class = nclass;
            t = nt;
            continue;
        }

        /* \d */
        if (syn_parse_perl_class_escape(p, t, class, &nclass, &nt)) {
            class = nclass;
            t = nt;
            continue;
        }

        /* A single character or a range. */
        Str rng = t;
        Rune lo = syn_parse_class_char(p, t, s, &t);
        Rune hi = lo;
        if (t.len >= 2 && t.p[0] == '-' && t.p[1] != ']') {
            t = syn_from(t, 1);
            hi = syn_parse_class_char(p, t, s, &t);
            if (hi < lo)
                syn_fail(p, SYNTAX_ERR_INVALID_CHAR_RANGE, syn_upto(rng, t));
        }
        if ((p->flags & SYNTAX_FOLD_CASE) == 0)
            class = syn_append_range(p, class, lo, hi);
        else
            class = syn_append_folded_range(p, class, lo, hi);
    }
    t = syn_from(t, 1); /* chop ] */

    /* Use &re->rune rather than &class so the sort sees the final slice. */
    re->rune = class;
    class = syn_clean_class(&re->rune);
    if (sign < 0)
        class = syn_negate_class(p, class);
    re->rune = class;
    syn_push(p, re);
    return t;
}

/* ---------------------------------------------------------------- parsing */

static SyntaxRegexp *syn_literal_regexp(SynParser *p, Str s, SyntaxFlags flags) {
    SyntaxRegexp *re = syn_new(p, SYNTAX_OP_LITERAL);
    re->flags = flags;
    re->rune = syn_rune0(re, 0); /* room for a short string */
    Str t = s;
    while (t.len > 0) {
        Int size;
        Rune c = utf8_decode_rune_in_string(t, &size);
        if (re->rune.len >= re->rune.cap) {
            /* Too long for the node: the whole string's runes. */
            Slice all = {0};
            Str u = s;
            while (u.len > 0) {
                Rune d = utf8_decode_rune_in_string(u, &size);
                all = syn_append_runes(p, all, &d, 1);
                u = syn_from(u, size);
            }
            re->rune = all;
            break;
        }
        re->rune = syn_append_runes(p, re->rune, &c, 1);
        t = syn_from(t, size);
    }
    return re;
}

/* Not inlined, so that its locals stay out of the frame that syn_parse_guarded
 * sets its jump buffer in. */
static BURROW_NOINLINE SyntaxRegexp *syn_parse_body(SynParser *p, Str s) {
    if (p->flags & SYNTAX_LITERAL) {
        /* Trivial parser for a literal string. */
        syn_check_utf8(p, s);
        return syn_literal_regexp(p, s, p->flags);
    }

    Str last_repeat = BURROW_STR_EMPTY;
    Str t = s;
    while (t.len > 0) {
        Str repeat = BURROW_STR_EMPTY;
        switch (t.p[0]) {
        default: {
            Rune c = syn_next_rune(p, t, &t);
            syn_literal(p, c);
            break;
        }

        case '(':
            if ((p->flags & SYNTAX_PERL_X) && t.len >= 2 && t.p[1] == '?') {
                /* Flag changes and non-capturing groups. */
                t = syn_parse_perl_flags(p, t);
                break;
            }
            p->num_cap++;
            syn_op(p, SYN_OP_LEFT_PAREN)->cap = p->num_cap;
            t = syn_from(t, 1);
            break;

        case '|':
            syn_parse_vertical_bar(p);
            t = syn_from(t, 1);
            break;

        case ')':
            syn_parse_right_paren(p);
            t = syn_from(t, 1);
            break;

        case '^':
            if (p->flags & SYNTAX_ONE_LINE)
                syn_op(p, SYNTAX_OP_BEGIN_TEXT);
            else
                syn_op(p, SYNTAX_OP_BEGIN_LINE);
            t = syn_from(t, 1);
            break;

        case '$':
            if (p->flags & SYNTAX_ONE_LINE)
                syn_op(p, SYNTAX_OP_END_TEXT)->flags |= SYNTAX_WAS_DOLLAR;
            else
                syn_op(p, SYNTAX_OP_END_LINE);
            t = syn_from(t, 1);
            break;

        case '.':
            if (p->flags & SYNTAX_DOT_NL)
                syn_op(p, SYNTAX_OP_ANY_CHAR);
            else
                syn_op(p, SYNTAX_OP_ANY_CHAR_NOT_NL);
            t = syn_from(t, 1);
            break;

        case '[':
            t = syn_parse_class(p, t);
            break;

        case '*':
        case '+':
        case '?': {
            Str before = t;
            SyntaxOp op = t.p[0] == '*'   ? SYNTAX_OP_STAR
                          : t.p[0] == '+' ? SYNTAX_OP_PLUS
                                          : SYNTAX_OP_QUEST;
            Str after = syn_from(t, 1);
            after = syn_repeat(p, op, 0, 0, before, after, last_repeat);
            repeat = before;
            t = after;
            break;
        }

        case '{': {
            Str before = t;
            Int min, max;
            Str after;
            if (!syn_parse_repeat(t, &min, &max, &after)) {
                /* If the repeat cannot be parsed, { is a literal. */
                syn_literal(p, '{');
                t = syn_from(t, 1);
                break;
            }
            if (min < 0 || min > 1000 || max > 1000 || (max >= 0 && min > max)) {
                /* Numbers were too big, or max is present and min > max. */
                syn_fail(p, SYNTAX_ERR_INVALID_REPEAT_SIZE, syn_upto(before, after));
            }
            after =
                syn_repeat(p, SYNTAX_OP_REPEAT, min, max, before, after, last_repeat);
            repeat = before;
            t = after;
            break;
        }

        case '\\': {
            if ((p->flags & SYNTAX_PERL_X) && t.len >= 2) {
                switch (t.p[1]) {
                case 'A':
                    syn_op(p, SYNTAX_OP_BEGIN_TEXT);
                    t = syn_from(t, 2);
                    goto next;
                case 'b':
                    syn_op(p, SYNTAX_OP_WORD_BOUNDARY);
                    t = syn_from(t, 2);
                    goto next;
                case 'B':
                    syn_op(p, SYNTAX_OP_NO_WORD_BOUNDARY);
                    t = syn_from(t, 2);
                    goto next;
                case 'C':
                    /* Any byte, which is not supported. */
                    syn_fail(p, SYNTAX_ERR_INVALID_ESCAPE, syn_to(t, 2));
                case 'Q': {
                    /* \Q ... \E: the text between is literal. */
                    Str lit = syn_from(t, 2);
                    t = BURROW_STR_EMPTY;
                    for (Int i = 0; i + 1 < lit.len; i++) {
                        if (lit.p[i] == '\\' && lit.p[i + 1] == 'E') {
                            t = syn_from(lit, i + 2);
                            lit = syn_to(lit, i);
                            break;
                        }
                    }
                    while (lit.len > 0) {
                        Str after;
                        Rune c = syn_next_rune(p, lit, &after);
                        syn_literal(p, c);
                        lit = after;
                    }
                    goto next;
                }
                case 'z':
                    syn_op(p, SYNTAX_OP_END_TEXT);
                    t = syn_from(t, 2);
                    goto next;
                default:
                    break;
                }
            }

            SyntaxRegexp *re = syn_new(p, SYNTAX_OP_CHAR_CLASS);
            re->flags = p->flags;

            /* \p{Han} and \pL. */
            Slice r;
            Str rest;
            if (t.len >= 2 && (t.p[1] == 'p' || t.p[1] == 'P')) {
                if (syn_parse_unicode_class(p, t, syn_rune0(re, 0), &r, &rest)) {
                    re->rune = r;
                    t = rest;
                    syn_push(p, re);
                    goto next;
                }
            }

            /* \d and the rest. */
            if (syn_parse_perl_class_escape(p, t, syn_rune0(re, 0), &r, &rest)) {
                re->rune = r;
                t = rest;
                syn_push(p, re);
                goto next;
            }
            syn_reuse(p, re);

            /* Anything else is an escaped literal. */
            Rune c = syn_parse_escape(p, t, &t);
            syn_literal(p, c);
            break;
        }
        }
    next:
        last_repeat = repeat;
    }

    syn_concat(p);
    if (syn_swap_vertical_bar(p)) {
        /* Pop the vertical bar. */
        p->stack.len--;
    }
    syn_alternate(p);

    if (p->stack.len != 1)
        syn_fail(p, SYNTAX_ERR_MISSING_PAREN, s);
    return SYN_SUBS(p->stack)[0];
}

/* The part that can unwind, in a function of its own so that nothing the
 * catch block reads is a local of the frame the jump lands in. */
static SyntaxRegexp *syn_parse_guarded(SynParser *p, Str s) {
    SyntaxRegexp *volatile re = NULL;
    BURROW_TRY {
        re = syn_parse_body(p, s);
    }
    BURROW_CATCH(r) {
        if (!p->failed)
            panic(r);
        re = NULL;
    }
    BURROW_TRY_END;
    return re;
}

SyntaxRegexp *syntax_parse(Alloc *a, Str s, SyntaxFlags flags, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, a, SYN_ARENA_CHUNK);
    SynParser p;
    memset(&p, 0, sizeof p);
    p.flags = flags;
    p.whole = s;
    p.sa = arena_allocator(&scratch);
    p.height.a = p.sa;
    p.size.a = p.sa;

    SyntaxRegexp *re = syn_parse_guarded(&p, s);
    SyntaxRegexp *out = NULL;
    Error e = BURROW_NO_ERROR;
    if (re == NULL) {
        if (p.oom) {
            e = burrow_err_out_of_memory;
        } else {
            SyntaxError se = {p.code, p.expr};
            e = syntax_error_as_error(&se, error_allocator());
        }
    } else {
        out = syn_copy_tree(a, p.sa, re);
        if (out == NULL)
            e = burrow_err_out_of_memory;
    }
    arena_free(&scratch);
    if (err != NULL)
        *err = e;
    return out;
}
