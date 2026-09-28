/* regexp/syntax: what a tree can do once it is built, from regexp.go and
 * simplify.go, and the block a tree lives in.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp/syntax.h"

#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/unicode.h"

#include "syntax_internal.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------- scratch */

static const Str syn_scratch_unwind = {(const Byte *)"regexp/syntax: out of memory",
                                       28};

void syn_scratch_init(SynScratch *s, Alloc *parent) {
    arena_init(&s->arena, parent, SYN_ARENA_CHUNK);
    s->a = arena_allocator(&s->arena);
    s->oom = false;
}

void syn_scratch_free(SynScratch *s) {
    arena_free(&s->arena);
}

void *syn_scratch_alloc(SynScratch *s, size_t size, size_t align) {
    void *p = mem_alloc(s->a, size == 0 ? 1 : size, align);
    if (p == NULL) {
        s->oom = true;
        panic_str(syn_scratch_unwind);
    }
    return p;
}

void *syn_stack_push(SynScratch *s, SynStack *k) {
    if (k->len == k->cap) {
        Int ncap = k->cap < 16 ? 16 : k->cap * 2;
        Byte *q =
            (Byte *)syn_scratch_alloc(s, (size_t)ncap * k->elem, BURROW_ALIGN_MAX);
        if (k->len > 0)
            memcpy(q, k->p, (size_t)k->len * k->elem);
        k->p = q;
        k->cap = ncap;
    }
    void *f = k->p + (size_t)k->len * k->elem;
    memset(f, 0, k->elem);
    k->len++;
    return f;
}

bool syn_guard(SynScratch *s, void (*fn)(void *), void *arg) {
    BURROW_TRY {
        fn(arg);
    }
    BURROW_CATCH(r) {
        if (!s->oom) {
            /* Not ours: tidy up and pass it on. */
            syn_scratch_free(s);
            panic(r);
        }
    }
    BURROW_TRY_END;
    return !s->oom;
}

bool syn_in_char_class(Rune r, const Rune *c, Int len) {
    Int lo = 0, hi = len / 2;
    while (lo < hi) {
        Int m = (Int)((uint64_t)(lo + hi) >> 1);
        if (r > c[2 * m + 1])
            lo = m + 1;
        else if (r < c[2 * m])
            hi = m;
        else
            return true;
    }
    return false;
}

/* ------------------------------------------------------------- the block */

/* Grows the list of nodes found so far. */
static bool syn_list_push(Alloc *scratch, const SyntaxRegexp ***list, Int *n, Int *cap,
                          const SyntaxRegexp *re) {
    if (*n == *cap) {
        Int ncap = *cap * 2;
        const SyntaxRegexp **q = (const SyntaxRegexp **)mem_alloc_nozero(
            scratch, (size_t)ncap * sizeof *q, _Alignof(const SyntaxRegexp *));
        if (q == NULL)
            return false;
        memcpy(q, *list, (size_t)*n * sizeof *q);
        *list = q;
        *cap = ncap;
    }
    (*list)[(*n)++] = re;
    return true;
}

SyntaxRegexp *syn_copy_tree(Alloc *a, Alloc *scratch, const SyntaxRegexp *root) {
    if (root == NULL)
        return NULL;

    /* Number the nodes, breadth first so there is no recursion, with a map
     * so that a node reached twice is copied once. */
    SynMap memo = {0};
    memo.a = scratch;
    Int n = 0, cap = 64;
    const SyntaxRegexp **list = (const SyntaxRegexp **)mem_alloc_nozero(
        scratch, (size_t)cap * sizeof *list, _Alignof(const SyntaxRegexp *));
    if (list == NULL || !syn_map_put(&memo, root, 0))
        return NULL;
    list[n++] = root;
    size_t nsub = 0, nrune = 0, nbyte = 0;
    for (Int i = 0; i < n; i++) {
        const SyntaxRegexp *re = list[i];
        if (re->sub.len > 1)
            nsub += (size_t)re->sub.len;
        if (re->rune.len > 2)
            nrune += (size_t)re->rune.len;
        nbyte += (size_t)re->name.len;
        for (Int j = 0; j < re->sub.len; j++) {
            const SyntaxRegexp *sub = SYN_SUBS(re->sub)[j];
            int64_t v;
            if (syn_map_get(&memo, sub, &v))
                continue;
            if (!syn_map_put(&memo, sub, n) ||
                !syn_list_push(scratch, &list, &n, &cap, sub))
                return NULL;
        }
    }

    size_t size = SYN_BLOCK_HDR + (size_t)n * sizeof(SyntaxRegexp) +
                  nsub * sizeof(SyntaxRegexp *) + nrune * sizeof(Rune) + nbyte;
    Byte *block = (Byte *)mem_alloc_nozero(a, size, SYN_BLOCK_ALIGN);
    if (block == NULL)
        return NULL;
    SynBlock *hdr = (SynBlock *)(void *)block;
    hdr->a = a;
    hdr->size = size;
    SyntaxRegexp *nodes = (SyntaxRegexp *)(void *)(block + SYN_BLOCK_HDR);
    SyntaxRegexp **subs = (SyntaxRegexp **)(nodes + n);
    Rune *runes = (Rune *)(subs + nsub);
    Byte *bytes = (Byte *)(runes + nrune);

    for (Int i = 0; i < n; i++) {
        const SyntaxRegexp *src = list[i];
        SyntaxRegexp *dst = &nodes[i];
        *dst = *src;

        dst->sub0[0] = NULL;
        if (src->sub.len == 0) {
            dst->sub = (Slice){0};
        } else {
            SyntaxRegexp **to = dst->sub0;
            Int c = 1;
            if (src->sub.len > 1) {
                to = subs;
                subs += src->sub.len;
                c = src->sub.len;
            }
            for (Int j = 0; j < src->sub.len; j++) {
                int64_t v = 0;
                syn_map_get(&memo, SYN_SUBS(src->sub)[j], &v);
                to[j] = &nodes[v];
            }
            dst->sub = (Slice){to, src->sub.len, c, TYPE_UNSAFE_POINTER};
        }

        if (src->rune.p == NULL) {
            dst->rune = (Slice){0};
        } else if (src->rune.len <= 2) {
            memcpy(dst->rune0, src->rune.p, (size_t)src->rune.len * sizeof(Rune));
            dst->rune = (Slice){dst->rune0, src->rune.len, 2, TYPE_RUNE};
        } else {
            memcpy(runes, src->rune.p, (size_t)src->rune.len * sizeof(Rune));
            dst->rune = (Slice){runes, src->rune.len, src->rune.len, TYPE_RUNE};
            runes += src->rune.len;
        }

        if (src->name.len > 0) {
            memcpy(bytes, src->name.p, (size_t)src->name.len);
            dst->name = str_from_bytes(bytes, src->name.len);
            bytes += src->name.len;
        } else {
            dst->name = BURROW_STR_EMPTY;
        }
    }
    return nodes;
}

void syntax_regexp_free(SyntaxRegexp *re) {
    if (re == NULL)
        return;
    SynBlock *hdr = (SynBlock *)(void *)((Byte *)re - SYN_BLOCK_HDR);
    mem_free(hdr->a, hdr, hdr->size, SYN_BLOCK_ALIGN);
}

/* ------------------------------------------------------------------ Op */

static const char *const syn_op_names[256] = {
    "Op(0)",        "NoMatch",      "EmptyMatch",     "Literal",  "CharClass",
    "AnyCharNotNL", "AnyChar",      "BeginLine",      "EndLine",  "BeginText",
    "EndText",      "WordBoundary", "NoWordBoundary", "Capture",  "Star",
    "Plus",         "Quest",        "Repeat",         "Concat",   "Alternate",
    "Op(20)",       "Op(21)",       "Op(22)",         "Op(23)",   "Op(24)",
    "Op(25)",       "Op(26)",       "Op(27)",         "Op(28)",   "Op(29)",
    "Op(30)",       "Op(31)",       "Op(32)",         "Op(33)",   "Op(34)",
    "Op(35)",       "Op(36)",       "Op(37)",         "Op(38)",   "Op(39)",
    "Op(40)",       "Op(41)",       "Op(42)",         "Op(43)",   "Op(44)",
    "Op(45)",       "Op(46)",       "Op(47)",         "Op(48)",   "Op(49)",
    "Op(50)",       "Op(51)",       "Op(52)",         "Op(53)",   "Op(54)",
    "Op(55)",       "Op(56)",       "Op(57)",         "Op(58)",   "Op(59)",
    "Op(60)",       "Op(61)",       "Op(62)",         "Op(63)",   "Op(64)",
    "Op(65)",       "Op(66)",       "Op(67)",         "Op(68)",   "Op(69)",
    "Op(70)",       "Op(71)",       "Op(72)",         "Op(73)",   "Op(74)",
    "Op(75)",       "Op(76)",       "Op(77)",         "Op(78)",   "Op(79)",
    "Op(80)",       "Op(81)",       "Op(82)",         "Op(83)",   "Op(84)",
    "Op(85)",       "Op(86)",       "Op(87)",         "Op(88)",   "Op(89)",
    "Op(90)",       "Op(91)",       "Op(92)",         "Op(93)",   "Op(94)",
    "Op(95)",       "Op(96)",       "Op(97)",         "Op(98)",   "Op(99)",
    "Op(100)",      "Op(101)",      "Op(102)",        "Op(103)",  "Op(104)",
    "Op(105)",      "Op(106)",      "Op(107)",        "Op(108)",  "Op(109)",
    "Op(110)",      "Op(111)",      "Op(112)",        "Op(113)",  "Op(114)",
    "Op(115)",      "Op(116)",      "Op(117)",        "Op(118)",  "Op(119)",
    "Op(120)",      "Op(121)",      "Op(122)",        "Op(123)",  "Op(124)",
    "Op(125)",      "Op(126)",      "Op(127)",        "opPseudo", "Op(129)",
    "Op(130)",      "Op(131)",      "Op(132)",        "Op(133)",  "Op(134)",
    "Op(135)",      "Op(136)",      "Op(137)",        "Op(138)",  "Op(139)",
    "Op(140)",      "Op(141)",      "Op(142)",        "Op(143)",  "Op(144)",
    "Op(145)",      "Op(146)",      "Op(147)",        "Op(148)",  "Op(149)",
    "Op(150)",      "Op(151)",      "Op(152)",        "Op(153)",  "Op(154)",
    "Op(155)",      "Op(156)",      "Op(157)",        "Op(158)",  "Op(159)",
    "Op(160)",      "Op(161)",      "Op(162)",        "Op(163)",  "Op(164)",
    "Op(165)",      "Op(166)",      "Op(167)",        "Op(168)",  "Op(169)",
    "Op(170)",      "Op(171)",      "Op(172)",        "Op(173)",  "Op(174)",
    "Op(175)",      "Op(176)",      "Op(177)",        "Op(178)",  "Op(179)",
    "Op(180)",      "Op(181)",      "Op(182)",        "Op(183)",  "Op(184)",
    "Op(185)",      "Op(186)",      "Op(187)",        "Op(188)",  "Op(189)",
    "Op(190)",      "Op(191)",      "Op(192)",        "Op(193)",  "Op(194)",
    "Op(195)",      "Op(196)",      "Op(197)",        "Op(198)",  "Op(199)",
    "Op(200)",      "Op(201)",      "Op(202)",        "Op(203)",  "Op(204)",
    "Op(205)",      "Op(206)",      "Op(207)",        "Op(208)",  "Op(209)",
    "Op(210)",      "Op(211)",      "Op(212)",        "Op(213)",  "Op(214)",
    "Op(215)",      "Op(216)",      "Op(217)",        "Op(218)",  "Op(219)",
    "Op(220)",      "Op(221)",      "Op(222)",        "Op(223)",  "Op(224)",
    "Op(225)",      "Op(226)",      "Op(227)",        "Op(228)",  "Op(229)",
    "Op(230)",      "Op(231)",      "Op(232)",        "Op(233)",  "Op(234)",
    "Op(235)",      "Op(236)",      "Op(237)",        "Op(238)",  "Op(239)",
    "Op(240)",      "Op(241)",      "Op(242)",        "Op(243)",  "Op(244)",
    "Op(245)",      "Op(246)",      "Op(247)",        "Op(248)",  "Op(249)",
    "Op(250)",      "Op(251)",      "Op(252)",        "Op(253)",  "Op(254)",
    "Op(255)",
};

Str syntax_op_string(SyntaxOp op) {
    const char *s = syn_op_names[op];
    return str_from_bytes((const Byte *)s, (Int)strlen(s));
}

/* ----------------------------------------------------------------- Equal */

static bool syn_runes_equal(Slice x, Slice y) {
    return x.len == y.len &&
           (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len * sizeof(Rune)) == 0);
}

bool syntax_regexp_equal(const SyntaxRegexp *x, const SyntaxRegexp *y) {
    if (x == NULL || y == NULL)
        return x == y;
    if (x->op != y->op)
        return false;
    switch (x->op) {
    case SYNTAX_OP_END_TEXT:
        /* The flags remember whether this was \z or $. */
        if ((x->flags & SYNTAX_WAS_DOLLAR) != (y->flags & SYNTAX_WAS_DOLLAR))
            return false;
        break;
    case SYNTAX_OP_LITERAL:
    case SYNTAX_OP_CHAR_CLASS:
        return (x->flags & SYNTAX_FOLD_CASE) == (y->flags & SYNTAX_FOLD_CASE) &&
               syn_runes_equal(x->rune, y->rune);
    case SYNTAX_OP_ALTERNATE:
    case SYNTAX_OP_CONCAT:
        if (x->sub.len != y->sub.len)
            return false;
        for (Int i = 0; i < x->sub.len; i++)
            if (!syntax_regexp_equal(SYN_SUBS(x->sub)[i], SYN_SUBS(y->sub)[i]))
                return false;
        break;
    case SYNTAX_OP_STAR:
    case SYNTAX_OP_PLUS:
    case SYNTAX_OP_QUEST:
        if ((x->flags & SYNTAX_NON_GREEDY) != (y->flags & SYNTAX_NON_GREEDY) ||
            !syntax_regexp_equal(SYN_SUBS(x->sub)[0], SYN_SUBS(y->sub)[0]))
            return false;
        break;
    case SYNTAX_OP_REPEAT:
        if ((x->flags & SYNTAX_NON_GREEDY) != (y->flags & SYNTAX_NON_GREEDY) ||
            x->min != y->min || x->max != y->max ||
            !syntax_regexp_equal(SYN_SUBS(x->sub)[0], SYN_SUBS(y->sub)[0]))
            return false;
        break;
    case SYNTAX_OP_CAPTURE:
        if (x->cap != y->cap || !str_eq(x->name, y->name) ||
            !syntax_regexp_equal(SYN_SUBS(x->sub)[0], SYN_SUBS(y->sub)[0]))
            return false;
        break;
    default:
        break;
    }
    return true;
}

/* ---------------------------------------------------------------- String */

/* Which flags, and which non-capturing parens, to print around a node. */
enum {
    SYN_PF_I = 1,     /* (?i: */
    SYN_PF_M = 2,     /* (?m: */
    SYN_PF_S = 4,     /* (?s: */
    SYN_PF_OFF = 8,   /* ) */
    SYN_PF_PREC = 16, /* (?: ) */
    SYN_PF_NEG_SHIFT = 5,
};

typedef struct SynPrinter {
    SynScratch s;
    SynMap flags;
    Byte *buf;
    Int len;
    Int cap;
    const SyntaxRegexp *re;
} SynPrinter;

static void syn_pr_bytes(SynPrinter *p, const void *b, Int n) {
    if (p->len + n > p->cap) {
        Int ncap = p->cap * 2;
        if (ncap < p->len + n)
            ncap = p->len + n;
        if (ncap < 64)
            ncap = 64;
        Byte *q = (Byte *)syn_scratch_alloc(&p->s, (size_t)ncap, 1);
        if (p->len > 0)
            memcpy(q, p->buf, (size_t)p->len);
        p->buf = q;
        p->cap = ncap;
    }
    memcpy(p->buf + p->len, b, (size_t)n);
    p->len += n;
}

static void syn_pr(SynPrinter *p, const char *s) {
    syn_pr_bytes(p, s, (Int)strlen(s));
}

static void syn_pr_rune(SynPrinter *p, Rune r) {
    Byte b[4];
    Int n;
    uint32_t c = (uint32_t)r;
    if (c < 0x80) {
        b[0] = (Byte)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (Byte)(0xC0 | (c >> 6));
        b[1] = (Byte)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        b[0] = 0xEF;
        b[1] = 0xBF;
        b[2] = 0xBD;
        n = 3;
    } else if (c < 0x10000) {
        b[0] = (Byte)(0xE0 | (c >> 12));
        b[1] = (Byte)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (Byte)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        b[0] = (Byte)(0xF0 | (c >> 18));
        b[1] = (Byte)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (Byte)(0x80 | ((c >> 6) & 0x3F));
        b[3] = (Byte)(0x80 | (c & 0x3F));
        n = 4;
    }
    syn_pr_bytes(p, b, n);
}

/* A number in base 10 or 16, lower case, with a minus sign when it has one. */
static void syn_pr_int(SynPrinter *p, int64_t v, int base) {
    char b[24];
    int i = (int)sizeof b;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        b[--i] = "0123456789abcdef"[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u != 0);
    if (v < 0)
        b[--i] = '-';
    syn_pr_bytes(p, b + i, (Int)sizeof b - i);
}

static int64_t syn_pf_get(SynPrinter *p, const SyntaxRegexp *re) {
    int64_t v = 0;
    syn_map_get(&p->flags, re, &v);
    return v;
}

static void syn_pf_put(SynPrinter *p, const SyntaxRegexp *re, int64_t v) {
    if (!syn_map_put(&p->flags, re, v)) {
        p->s.oom = true;
        panic_str(syn_scratch_unwind);
    }
}

/* addSpan: turns f on from start to last. */
static void syn_add_span(SynPrinter *p, const SyntaxRegexp *start,
                         const SyntaxRegexp *last, int f) {
    syn_pf_put(p, start, f);
    syn_pf_put(p, last, syn_pf_get(p, last) | SYN_PF_OFF); /* maybe start == last */
}

/* The flags a node with no subs must have around it, and those it cannot. */
static void syn_leaf_flags(const SyntaxRegexp *re, int *must, int *cant) {
    *must = *cant = 0;
    const Rune *r = SYN_RUNES(re->rune);
    switch (re->op) {
    default:
        return;

    case SYNTAX_OP_LITERAL:
        /* A literal that case folding changes needs (?i) on or off to match
         * how it was parsed. One it does not change needs nothing. */
        for (Int i = 0; i < re->rune.len; i++) {
            if (SYN_MIN_FOLD <= r[i] && r[i] <= SYN_MAX_FOLD &&
                unicode_simple_fold(r[i]) != r[i]) {
                if (re->flags & SYNTAX_FOLD_CASE)
                    *must = SYN_PF_I;
                else
                    *cant = SYN_PF_I;
                return;
            }
        }
        return;

    case SYNTAX_OP_CHAR_CLASS:
        /* The case folding is already in the class, so (?i) must be off if
         * folding would add anything to it. */
        for (Int i = 0; i < re->rune.len; i += 2) {
            Rune lo = r[i] > SYN_MIN_FOLD ? r[i] : SYN_MIN_FOLD;
            Rune hi = r[i + 1] < SYN_MAX_FOLD ? r[i + 1] : SYN_MAX_FOLD;
            for (Rune c = lo; c <= hi; c++) {
                for (Rune f = unicode_simple_fold(c); f != c;
                     f = unicode_simple_fold(f)) {
                    if (!(lo <= f && f <= hi) &&
                        !syn_in_char_class(f, r, re->rune.len)) {
                        *cant = SYN_PF_I;
                        return;
                    }
                }
            }
        }
        return;

    case SYNTAX_OP_ANY_CHAR_NOT_NL: /* (?-s). */
        *cant = SYN_PF_S;
        return;

    case SYNTAX_OP_ANY_CHAR: /* (?s). */
        *must = SYN_PF_S;
        return;

    case SYNTAX_OP_BEGIN_LINE: /* (?m)^ */
    case SYNTAX_OP_END_LINE:   /* (?m)$ */
        *must = SYN_PF_M;
        return;

    case SYNTAX_OP_END_TEXT:
        if (re->flags & SYNTAX_WAS_DOLLAR) /* (?-m)$ */
            *cant = SYN_PF_M;
        return;
    }
}

/* A concatenation or alternation part way through calcFlags. */
typedef struct SynFlagsFrame {
    const SyntaxRegexp *re;
    Int next;
    int m, c, all_cant;
    Int start, last;
    bool did;
} SynFlagsFrame;

/* calcFlags: notes in p->flags which flags to print around each part of root,
 * and gives the flags that must be on around root and those that must not.
 * It keeps its own stack in place of recursion. */
static void syn_calc_flags(SynPrinter *p, const SyntaxRegexp *root, int *must,
                           int *cant) {
    SynStack k = {NULL, 0, 0, sizeof(SynFlagsFrame)};
    SynFlagsFrame *t;
    const SyntaxRegexp *re = root;
    int rm = 0, rc = 0; /* what the node just finished came to */
    for (;;) {
        /* Down to the first node that is not a capture or a repeat, which
         * pass up whatever their sub needs. */
        while (re->op == SYNTAX_OP_CAPTURE || re->op == SYNTAX_OP_STAR ||
               re->op == SYNTAX_OP_PLUS || re->op == SYNTAX_OP_QUEST ||
               re->op == SYNTAX_OP_REPEAT)
            re = SYN_SUBS(re->sub)[0];
        if ((re->op == SYNTAX_OP_CONCAT || re->op == SYNTAX_OP_ALTERNATE) &&
            re->sub.len > 0) {
            t = (SynFlagsFrame *)syn_stack_push(&p->s, &k);
            t->re = re;
            t->next = 1;
            re = SYN_SUBS(re->sub)[0];
            continue;
        }
        if (re->op == SYNTAX_OP_CONCAT || re->op == SYNTAX_OP_ALTERNATE)
            rm = rc = 0;
        else
            syn_leaf_flags(re, &rm, &rc);

        /* Hand rm and rc up to the concatenations and alternations waiting
         * for them. */
        while ((t = (SynFlagsFrame *)syn_stack_top(&k)) != NULL) {
            /* Gather what each part must and cannot have. At a part that
             * clashes with what came before, put the flags around the span so
             * far and start again. */
            SyntaxRegexp *const *sub = SYN_SUBS(t->re->sub);
            Int i = t->next - 1;
            if ((t->m & rc) != 0 || (rm & t->c) != 0) {
                if (t->m != 0)
                    syn_add_span(p, sub[t->start], sub[t->last], t->m);
                t->m = 0;
                t->c = 0;
                t->start = i;
                t->did = true;
            }
            t->m |= rm;
            t->c |= rc;
            t->all_cant |= rc;
            if (rm != 0)
                t->last = i;
            if (t->m == 0 && t->start == i)
                t->start++;
            if (t->next < t->re->sub.len)
                break;
            if (!t->did) {
                /* No clashes: hand the lot up. */
                rm = t->m;
                rc = t->c;
            } else {
                if (t->m != 0) {
                    /* Finish the last span. */
                    syn_add_span(p, sub[t->start], sub[t->last], t->m);
                }
                rm = 0;
                rc = t->all_cant;
            }
            k.len--;
        }
        if (t == NULL)
            break;
        re = SYN_SUBS(t->re->sub)[t->next++];
    }
    *must = rm;
    *cant = rc;
}

/* escape: one rune, with a backslash when it is special, or as an escape
 * when it does not print. force escapes it anyway. */
static void syn_escape(SynPrinter *p, Rune r, bool force) {
    if (unicode_is_print(r)) {
        if ((r < 0x80 && strchr("\\.+*?()|[]{}^$", (int)r) != NULL && r != 0) || force)
            syn_pr(p, "\\");
        syn_pr_rune(p, r);
        return;
    }
    switch (r) {
    case '\a':
        syn_pr(p, "\\a");
        break;
    case '\f':
        syn_pr(p, "\\f");
        break;
    case '\n':
        syn_pr(p, "\\n");
        break;
    case '\r':
        syn_pr(p, "\\r");
        break;
    case '\t':
        syn_pr(p, "\\t");
        break;
    case '\v':
        syn_pr(p, "\\v");
        break;
    default:
        if (r < 0x100) {
            syn_pr(p, "\\x");
            if (r >= 0 && r < 0x10)
                syn_pr(p, "0");
            syn_pr_int(p, r, 16);
            break;
        }
        syn_pr(p, "\\x{");
        syn_pr_int(p, r, 16);
        syn_pr(p, "}");
        break;
    }
}

static void syn_write_class_range(SynPrinter *p, Rune lo, Rune hi) {
    syn_escape(p, lo, lo == '-');
    if (lo != hi) {
        if (hi != lo + 1)
            syn_pr(p, "-");
        syn_escape(p, hi, hi == '-');
    }
}

/* The flags and parens that open re, given the flags f asked for around it,
 * and the flags to close it with. */
static int syn_write_open(SynPrinter *p, const SyntaxRegexp *re, int f) {
    f |= (int)syn_pf_get(p, re);
    if ((f & SYN_PF_PREC) != 0 && (f & ~(SYN_PF_OFF | SYN_PF_PREC)) != 0 &&
        (f & SYN_PF_OFF) != 0) {
        /* The flags going on and off already group it. */
        f &= ~SYN_PF_PREC;
    }
    if ((f & ~(SYN_PF_OFF | SYN_PF_PREC)) != 0) {
        syn_pr(p, "(?");
        if (f & SYN_PF_I)
            syn_pr(p, "i");
        if (f & SYN_PF_M)
            syn_pr(p, "m");
        if (f & SYN_PF_S)
            syn_pr(p, "s");
        if (f & ((SYN_PF_M | SYN_PF_S) << SYN_PF_NEG_SHIFT)) {
            syn_pr(p, "-");
            if (f & (SYN_PF_M << SYN_PF_NEG_SHIFT))
                syn_pr(p, "m");
            if (f & (SYN_PF_S << SYN_PF_NEG_SHIFT))
                syn_pr(p, "s");
        }
        syn_pr(p, ":");
    }
    if (f & SYN_PF_PREC)
        syn_pr(p, "(?:");
    return f;
}

static void syn_write_close(SynPrinter *p, int f) {
    if (f & SYN_PF_PREC)
        syn_pr(p, ")");
    if (f & SYN_PF_OFF)
        syn_pr(p, ")");
}

/* A node with no subs. */
static void syn_write_leaf(SynPrinter *p, const SyntaxRegexp *re) {
    const Rune *r = SYN_RUNES(re->rune);
    switch (re->op) {
    default:
        syn_pr(p, "<invalid op");
        syn_pr_int(p, re->op, 10);
        syn_pr(p, ">");
        break;
    case SYNTAX_OP_NO_MATCH:
        syn_pr(p, "[^\\x00-\\x{10FFFF}]");
        break;
    case SYNTAX_OP_EMPTY_MATCH:
        syn_pr(p, "(?:)");
        break;
    case SYNTAX_OP_LITERAL:
        for (Int i = 0; i < re->rune.len; i++)
            syn_escape(p, r[i], false);
        break;
    case SYNTAX_OP_CHAR_CLASS: {
        Int n = re->rune.len;
        if (n % 2 != 0) {
            syn_pr(p, "[invalid char class]");
            break;
        }
        syn_pr(p, "[");
        if (n == 0) {
            syn_pr(p, "^\\x00-\\x{10FFFF}");
        } else if (r[0] == 0 && r[n - 1] == UNICODE_MAX_RUNE && n > 2) {
            /* Has 0 and the largest rune, so probably negated. Print the
             * gaps. */
            syn_pr(p, "^");
            for (Int i = 1; i < n - 1; i += 2)
                syn_write_class_range(p, r[i] + 1, r[i + 1] - 1);
        } else {
            for (Int i = 0; i < n; i += 2)
                syn_write_class_range(p, r[i], r[i + 1]);
        }
        syn_pr(p, "]");
        break;
    }
    case SYNTAX_OP_ANY_CHAR_NOT_NL:
    case SYNTAX_OP_ANY_CHAR:
        syn_pr(p, ".");
        break;
    case SYNTAX_OP_BEGIN_LINE:
        syn_pr(p, "^");
        break;
    case SYNTAX_OP_END_LINE:
        syn_pr(p, "$");
        break;
    case SYNTAX_OP_BEGIN_TEXT:
        syn_pr(p, "\\A");
        break;
    case SYNTAX_OP_END_TEXT:
        syn_pr(p, (re->flags & SYNTAX_WAS_DOLLAR) ? "$" : "\\z");
        break;
    case SYNTAX_OP_WORD_BOUNDARY:
        syn_pr(p, "\\b");
        break;
    case SYNTAX_OP_NO_WORD_BOUNDARY:
        syn_pr(p, "\\B");
        break;
    }
}

/* What goes after the sub of a repeat. */
static void syn_write_repeat_op(SynPrinter *p, const SyntaxRegexp *re) {
    if (re->op == SYNTAX_OP_STAR) {
        syn_pr(p, "*");
    } else if (re->op == SYNTAX_OP_PLUS) {
        syn_pr(p, "+");
    } else if (re->op == SYNTAX_OP_QUEST) {
        syn_pr(p, "?");
    } else {
        syn_pr(p, "{");
        syn_pr_int(p, re->min, 10);
        if (re->max != re->min) {
            syn_pr(p, ",");
            if (re->max >= 0)
                syn_pr_int(p, re->max, 10);
        }
        syn_pr(p, "}");
    }
    if (re->flags & SYNTAX_NON_GREEDY)
        syn_pr(p, "?");
}

/* A node part way through being written: its flags, and the next sub. */
typedef struct SynWriteFrame {
    const SyntaxRegexp *re;
    int f;
    Int next;
} SynWriteFrame;

/* The flags writeRegexp passes to sub i of re. */
static int syn_sub_flags(SynPrinter *p, const SyntaxRegexp *re, Int i) {
    const SyntaxRegexp *sub = SYN_SUBS(re->sub)[i];
    switch (re->op) {
    case SYNTAX_OP_CAPTURE:
        return (int)syn_pf_get(p, sub);
    case SYNTAX_OP_CONCAT:
        return sub->op == SYNTAX_OP_ALTERNATE ? SYN_PF_PREC : 0;
    case SYNTAX_OP_ALTERNATE:
        return 0;
    default: /* the repeats */
        return sub->op > SYNTAX_OP_CAPTURE ||
                       (sub->op == SYNTAX_OP_LITERAL && sub->rune.len > 1)
                   ? SYN_PF_PREC
                   : 0;
    }
}

/* writeRegexp: root in Perl syntax, with the flags f0 around it. It keeps its
 * own stack in place of recursion. */
static void syn_write_regexp(SynPrinter *p, const SyntaxRegexp *root, int f0) {
    SynStack k = {NULL, 0, 0, sizeof(SynWriteFrame)};
    const SyntaxRegexp *re = root;
    int f = f0;
    SynWriteFrame *t;
    for (;;) {
        /* Open re, and write it whole if it has no subs. */
        f = syn_write_open(p, re, f);
        bool leaf = false;
        switch (re->op) {
        case SYNTAX_OP_CAPTURE:
            if (re->name.len > 0) {
                syn_pr(p, "(?P<");
                syn_pr_bytes(p, re->name.p, re->name.len);
                syn_pr(p, ">");
            } else {
                syn_pr(p, "(");
            }
            if (SYN_SUBS(re->sub)[0]->op == SYNTAX_OP_EMPTY_MATCH) {
                syn_pr(p, ")");
                leaf = true;
            }
            break;
        case SYNTAX_OP_STAR:
        case SYNTAX_OP_PLUS:
        case SYNTAX_OP_QUEST:
        case SYNTAX_OP_REPEAT:
            break;
        case SYNTAX_OP_CONCAT:
        case SYNTAX_OP_ALTERNATE:
            leaf = re->sub.len == 0;
            break;
        default:
            syn_write_leaf(p, re);
            leaf = true;
            break;
        }
        if (!leaf) {
            t = (SynWriteFrame *)syn_stack_push(&p->s, &k);
            t->re = re;
            t->f = f;
            t->next = 1;
            f = syn_sub_flags(p, re, 0);
            re = SYN_SUBS(re->sub)[0];
            continue;
        }
        syn_write_close(p, f);

        /* Close what is finished, and move on to the next sub. */
        while ((t = (SynWriteFrame *)syn_stack_top(&k)) != NULL) {
            if ((t->re->op == SYNTAX_OP_CONCAT || t->re->op == SYNTAX_OP_ALTERNATE) &&
                t->next < t->re->sub.len)
                break;
            if (t->re->op == SYNTAX_OP_CAPTURE)
                syn_pr(p, ")");
            else if (t->re->op != SYNTAX_OP_CONCAT && t->re->op != SYNTAX_OP_ALTERNATE)
                syn_write_repeat_op(p, t->re);
            syn_write_close(p, t->f);
            k.len--;
        }
        if (t == NULL)
            return;
        if (t->re->op == SYNTAX_OP_ALTERNATE)
            syn_pr(p, "|");
        f = syn_sub_flags(p, t->re, t->next);
        re = SYN_SUBS(t->re->sub)[t->next++];
    }
}

static void syn_print(void *arg) {
    SynPrinter *p = (SynPrinter *)arg;
    int must, cant;
    syn_calc_flags(p, p->re, &must, &cant);
    must |= (cant & ~SYN_PF_I) << SYN_PF_NEG_SHIFT;
    if (must != 0)
        must |= SYN_PF_OFF;
    syn_write_regexp(p, p->re, must);
}

Str syntax_regexp_string(const SyntaxRegexp *re, Alloc *a) {
    SynPrinter p;
    memset(&p, 0, sizeof p);
    syn_scratch_init(&p.s, a);
    p.flags.a = p.s.a;
    p.re = re;
    Str out = BURROW_STR_EMPTY;
    if (syn_guard(&p.s, syn_print, &p) && p.len > 0) {
        Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)p.len, 1);
        if (b != NULL) {
            memcpy(b, p.buf, (size_t)p.len);
            out = str_from_bytes(b, p.len);
        }
    }
    syn_scratch_free(&p.s);
    return out;
}

/* ------------------------------------------------------ MaxCap, CapNames */

Int syntax_regexp_max_cap(const SyntaxRegexp *re) {
    Int m = 0;
    if (re->op == SYNTAX_OP_CAPTURE)
        m = re->cap;
    for (Int i = 0; i < re->sub.len; i++) {
        Int n = syntax_regexp_max_cap(SYN_SUBS(re->sub)[i]);
        if (m < n)
            m = n;
    }
    return m;
}

static void syn_cap_names(const SyntaxRegexp *re, Str *names) {
    if (re->op == SYNTAX_OP_CAPTURE)
        names[re->cap] = re->name;
    for (Int i = 0; i < re->sub.len; i++)
        syn_cap_names(SYN_SUBS(re->sub)[i], names);
}

Slice syntax_regexp_cap_names(const SyntaxRegexp *re, Alloc *a) {
    Int n = syntax_regexp_max_cap(re) + 1;
    Str *names = (Str *)mem_alloc(a, (size_t)n * sizeof(Str), _Alignof(Str));
    if (names == NULL)
        return (Slice){0};
    syn_cap_names(re, names);
    return (Slice){names, n, n, TYPE_STRING};
}

/* -------------------------------------------------------------- Simplify */

typedef struct SynSimplifier {
    SynScratch s;
    const SyntaxRegexp *in;
    SyntaxRegexp *out;
} SynSimplifier;

/* A new node with room for n subs. */
static SyntaxRegexp *syn_simp_node(SynSimplifier *z, SyntaxOp op, SyntaxFlags flags,
                                   Int n) {
    SyntaxRegexp *re =
        (SyntaxRegexp *)syn_scratch_alloc(&z->s, sizeof *re, _Alignof(SyntaxRegexp));
    re->op = op;
    re->flags = flags;
    if (n > 0) {
        SyntaxRegexp **sub = (SyntaxRegexp **)syn_scratch_alloc(
            &z->s, (size_t)n * sizeof *sub, _Alignof(SyntaxRegexp *));
        re->sub = (Slice){sub, 0, n, TYPE_UNSAFE_POINTER};
    }
    return re;
}

static void syn_simp_add(SyntaxRegexp *re, SyntaxRegexp *sub) {
    SYN_SUBS(re->sub)[re->sub.len++] = sub;
}

/* simplify1: op applied to sub, which is already simple, with re given back
 * when it is exactly that. */
static SyntaxRegexp *syn_simplify1(SynSimplifier *z, SyntaxOp op, SyntaxFlags flags,
                                   SyntaxRegexp *sub, SyntaxRegexp *re) {
    /* The empty string repeated is still the empty string. */
    if (sub->op == SYNTAX_OP_EMPTY_MATCH)
        return sub;
    /* The operators are idempotent when the flags match. */
    if (op == sub->op &&
        (flags & SYNTAX_NON_GREEDY) == (sub->flags & SYNTAX_NON_GREEDY))
        return sub;
    if (re != NULL && re->op == op &&
        (re->flags & SYNTAX_NON_GREEDY) == (flags & SYNTAX_NON_GREEDY) &&
        sub == SYN_SUBS(re->sub)[0])
        return re;
    re = syn_simp_node(z, op, flags, 1);
    syn_simp_add(re, sub);
    return re;
}

/* The simple form of the repeat re, given its sub already simplified. */
static SyntaxRegexp *syn_simplify_repeat(SynSimplifier *z, SyntaxRegexp *re,
                                         SyntaxRegexp *sub) {
    /* x{n,} is at least n of x. */
    if (re->max == -1) {
        /* x{0,} is x*. */
        if (re->min == 0)
            return syn_simplify1(z, SYNTAX_OP_STAR, re->flags, sub, NULL);
        /* x{1,} is x+. */
        if (re->min == 1)
            return syn_simplify1(z, SYNTAX_OP_PLUS, re->flags, sub, NULL);
        /* x{4,} is xxxx+. */
        SyntaxRegexp *nre = syn_simp_node(z, SYNTAX_OP_CONCAT, 0, re->min);
        for (Int i = 0; i < re->min - 1; i++)
            syn_simp_add(nre, sub);
        syn_simp_add(nre, syn_simplify1(z, SYNTAX_OP_PLUS, re->flags, sub, NULL));
        return nre;
    }

    /* x{1} is x. */
    if (re->min == 1 && re->max == 1)
        return sub;

    /* x{n,m} is n of x and then m-n of x?, nested so the matcher does less
     * work: x{2,5} is xx(x(x(x)?)?)?. The prefix first, xx. */
    SyntaxRegexp *prefix = NULL;
    if (re->min > 0) {
        prefix = syn_simp_node(z, SYNTAX_OP_CONCAT, 0, re->min + 1);
        for (Int i = 0; i < re->min; i++)
            syn_simp_add(prefix, sub);
    }

    /* Then the suffix, (x(x(x)?)?)?. */
    if (re->max > re->min) {
        SyntaxRegexp *suffix = syn_simplify1(z, SYNTAX_OP_QUEST, re->flags, sub, NULL);
        for (Int i = re->min + 1; i < re->max; i++) {
            SyntaxRegexp *nre2 = syn_simp_node(z, SYNTAX_OP_CONCAT, 0, 2);
            syn_simp_add(nre2, sub);
            syn_simp_add(nre2, suffix);
            suffix = syn_simplify1(z, SYNTAX_OP_QUEST, re->flags, nre2, NULL);
        }
        if (prefix == NULL)
            return suffix;
        syn_simp_add(prefix, suffix);
    }
    if (prefix != NULL)
        return prefix;

    /* Something that cannot happen, like min > max or min < max < 0, is no
     * match. */
    return syn_simp_node(z, SYNTAX_OP_NO_MATCH, 0, 0);
}

/* A node being simplified: the next sub to do, and the copy of the node once
 * one of its subs has changed. */
typedef struct SynSimplifyFrame {
    SyntaxRegexp *re;
    SyntaxRegexp *nre;
    Int next;
} SynSimplifyFrame;

/* Simplify, with its own stack in place of recursion. */
static SyntaxRegexp *syn_simplify(SynSimplifier *z, SyntaxRegexp *root) {
    if (root == NULL)
        return NULL;
    SynStack k = {NULL, 0, 0, sizeof(SynSimplifyFrame)};
    SynSimplifyFrame *t = (SynSimplifyFrame *)syn_stack_push(&z->s, &k);
    t->re = root;
    SyntaxRegexp *ret = NULL;
    bool back = false; /* whether ret is the simple form of t's last sub */
    while ((t = (SynSimplifyFrame *)syn_stack_top(&k)) != NULL) {
        SyntaxRegexp *re = t->re;
        SyntaxRegexp *const *subs = SYN_SUBS(re->sub);
        if (!back) {
            bool has_subs;
            switch (re->op) {
            case SYNTAX_OP_CAPTURE:
            case SYNTAX_OP_CONCAT:
            case SYNTAX_OP_ALTERNATE:
                has_subs = re->sub.len > 0;
                ret = re;
                break;
            case SYNTAX_OP_STAR:
            case SYNTAX_OP_PLUS:
            case SYNTAX_OP_QUEST:
                has_subs = true;
                break;
            case SYNTAX_OP_REPEAT:
                /* x{0} matches the empty string and never looks at x. */
                has_subs = !(re->min == 0 && re->max == 0);
                if (!has_subs)
                    ret = syn_simp_node(z, SYNTAX_OP_EMPTY_MATCH, 0, 0);
                break;
            default:
                has_subs = false;
                ret = re;
                break;
            }
            if (!has_subs) {
                k.len--;
                back = true;
                continue;
            }
            t->nre = re;
            t->next = 1;
            SynSimplifyFrame *u = (SynSimplifyFrame *)syn_stack_push(&z->s, &k);
            u->re = subs[0];
            continue;
        }
        switch (re->op) {
        case SYNTAX_OP_CAPTURE:
        case SYNTAX_OP_CONCAT:
        case SYNTAX_OP_ALTERNATE: {
            /* Copy the node once one of the subs changes. */
            Int i = t->next - 1;
            SyntaxRegexp *nsub = ret;
            if (t->nre == re && nsub != subs[i]) {
                SyntaxRegexp *nre = syn_simp_node(z, re->op, re->flags, re->sub.len);
                nre->min = re->min;
                nre->max = re->max;
                nre->cap = re->cap;
                nre->name = re->name;
                for (Int j = 0; j < i; j++)
                    syn_simp_add(nre, subs[j]);
                t->nre = nre;
            }
            if (t->nre != re)
                syn_simp_add(t->nre, nsub);
            if (t->next < re->sub.len) {
                SyntaxRegexp *s = subs[t->next++];
                SynSimplifyFrame *u = (SynSimplifyFrame *)syn_stack_push(&z->s, &k);
                u->re = s;
                back = false;
                continue;
            }
            ret = t->nre;
            break;
        }
        case SYNTAX_OP_REPEAT:
            ret = syn_simplify_repeat(z, re, ret);
            break;
        default: /* star, plus and quest */
            ret = syn_simplify1(z, re->op, re->flags, ret, re);
            break;
        }
        k.len--;
    }
    return ret;
}

static void syn_simplify_run(void *arg) {
    SynSimplifier *z = (SynSimplifier *)arg;
    /* Nothing here writes to a node it did not make, so the cast is safe. */
    z->out = syn_simplify(z, (SyntaxRegexp *)(uintptr_t)z->in);
}

SyntaxRegexp *syntax_regexp_simplify(const SyntaxRegexp *re, Alloc *a) {
    if (re == NULL)
        return NULL;
    SynSimplifier z;
    memset(&z, 0, sizeof z);
    syn_scratch_init(&z.s, a);
    z.in = re;
    SyntaxRegexp *out = NULL;
    if (syn_guard(&z.s, syn_simplify_run, &z))
        out = syn_copy_tree(a, z.s.a, z.out);
    syn_scratch_free(&z.s);
    return out;
}
