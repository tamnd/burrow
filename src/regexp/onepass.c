/* regexp: one pass programs, from onepass.go.
 *
 * A program is one pass when, at every alternation, the next input rune says
 * which branch to take, so a match never has to back up or run two threads.
 * Such a program only makes sense anchored at the start of the text, and that
 * is the only case compile tries.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp.h"

#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "regexp_internal.h"

#include <string.h>

/* iop: the op, with the single rune ones folded into InstRune. */
static SyntaxInstOp rx_iop(const SyntaxInst *i) {
    switch (i->op) {
    case SYNTAX_INST_RUNE1:
    case SYNTAX_INST_RUNE_ANY:
    case SYNTAX_INST_RUNE_ANY_NOT_NL:
        return SYNTAX_INST_RUNE;
    default:
        return i->op;
    }
}

/* onePassPrefix: the literal text a one pass program has to start with, the
 * pc of the instruction after it, and whether it is the whole match. */
Str rx_onepass_prefix(Alloc *a, const SyntaxProg *p, bool *complete, uint32_t *pc_out,
                      bool *oom) {
    const SyntaxInst *insts = (const SyntaxInst *)p->inst.p;
    const SyntaxInst *i = &insts[p->start];
    *complete = false;
    if (i->op != SYNTAX_INST_EMPTY_WIDTH ||
        ((SyntaxEmptyOp)i->arg & SYNTAX_EMPTY_BEGIN_TEXT) == 0) {
        *complete = i->op == SYNTAX_INST_MATCH;
        *pc_out = (uint32_t)p->start;
        return BURROW_STR_EMPTY;
    }
    uint32_t pc = i->out;
    i = &insts[pc];
    while (i->op == SYNTAX_INST_NOP) {
        pc = i->out;
        i = &insts[pc];
    }
    /* Avoid allocation of buffer if prefix is empty. */
    if (rx_iop(i) != SYNTAX_INST_RUNE || i->rune.len != 1) {
        *complete = i->op == SYNTAX_INST_MATCH;
        *pc_out = (uint32_t)p->start;
        return BURROW_STR_EMPTY;
    }
    /* Have prefix; gather characters. */
    StringsBuilder buf = STRINGS_BUILDER(a);
    while (rx_iop(i) == SYNTAX_INST_RUNE && i->rune.len == 1 &&
           ((SyntaxFlags)i->arg & SYNTAX_FOLD_CASE) == 0 &&
           ((const Rune *)i->rune.p)[0] != UTF8_RUNE_ERROR) {
        Error err = BURROW_NO_ERROR;
        strings_builder_write_rune(&buf, ((const Rune *)i->rune.p)[0], &err);
        if (BURROW_FAILED(err))
            *oom = true;
        pc = i->out;
        i = &insts[i->out];
    }
    if (i->op == SYNTAX_INST_EMPTY_WIDTH &&
        ((SyntaxEmptyOp)i->arg & SYNTAX_EMPTY_END_TEXT) != 0 &&
        insts[i->out].op == SYNTAX_INST_MATCH)
        *complete = true;
    *pc_out = pc;
    return strings_builder_string(&buf);
}

/* ------------------------------------------------------------------ building */

typedef struct RxRunes {
    Rune *p;
    Int len;
} RxRunes;

/* queueOnePass: a sparse set of pcs, in the order they went in. */
typedef struct RxPcQueue {
    uint32_t *sparse;
    uint32_t *dense;
    uint32_t size;
    uint32_t next_index;
    uint32_t cap;
} RxPcQueue;

static bool rx_q_contains(const RxPcQueue *q, uint32_t u) {
    if (u >= q->cap)
        return false;
    return q->sparse[u] < q->size && q->dense[q->sparse[u]] == u;
}

static void rx_q_insert(RxPcQueue *q, uint32_t u) {
    if (rx_q_contains(q, u) || u >= q->cap)
        return;
    q->sparse[u] = q->size;
    q->dense[q->size] = u;
    q->size++;
}

typedef struct RxBuild {
    Alloc *a;
    bool oom;
    RxOnePassInst *inst;
    Int ninst;
    RxRunes *runes; /* onePassRunes */
    bool *m;
    RxPcQueue inst_queue;
    RxPcQueue visit_queue;
} RxBuild;

static void *rx_balloc(RxBuild *b, size_t n, size_t align) {
    void *p = mem_alloc(b->a, n == 0 ? 1 : n, align);
    if (p == NULL)
        b->oom = true;
    return p;
}

static bool rx_q_init(RxBuild *b, RxPcQueue *q, Int size) {
    q->sparse =
        (uint32_t *)rx_balloc(b, (size_t)size * sizeof(uint32_t), _Alignof(uint32_t));
    q->dense =
        (uint32_t *)rx_balloc(b, (size_t)size * sizeof(uint32_t), _Alignof(uint32_t));
    q->size = 0;
    q->next_index = 0;
    q->cap = (uint32_t)size;
    return !b->oom;
}

static bool rx_runes_copy(RxBuild *b, RxRunes *dst, const Rune *src, Int n) {
    dst->p = (Rune *)rx_balloc(b, (size_t)n * sizeof(Rune), _Alignof(Rune));
    if (dst->p == NULL)
        return false;
    if (n > 0)
        memcpy(dst->p, src, (size_t)n * sizeof(Rune));
    dst->len = n;
    return true;
}

/* A next list of n entries, all to. */
static bool rx_next_fill(RxBuild *b, RxOnePassInst *inst, Int n, uint32_t to) {
    inst->next =
        (uint32_t *)rx_balloc(b, (size_t)n * sizeof(uint32_t), _Alignof(uint32_t));
    if (inst->next == NULL)
        return false;
    for (Int k = 0; k < n; k++)
        inst->next[k] = to;
    inst->nnext = n;
    return true;
}

/* mergeRuneSets: merges two sorted, non overlapping lists of rune ranges,
 * and says for each range of the result which pc it came from. False when
 * the two overlap, which means the program is not one pass. */
static bool rx_merge_rune_sets(RxBuild *b, const RxRunes *left, const RxRunes *right,
                               uint32_t left_pc, uint32_t right_pc, RxRunes *merged,
                               uint32_t **next, Int *nnext) {
    Int left_len = left->len, right_len = right->len;
    Int lx = 0, rx = 0;
    merged->p = (Rune *)rx_balloc(b, (size_t)(left_len + right_len) * sizeof(Rune),
                                  _Alignof(Rune));
    *next = (uint32_t *)rx_balloc(
        b, (size_t)((left_len + right_len) / 2 + 1) * sizeof(uint32_t),
        _Alignof(uint32_t));
    if (b->oom)
        return false;
    merged->len = 0;
    *nnext = 0;
    Int ix = -1;
    while (lx < left_len || rx < right_len) {
        const RxRunes *arr;
        Int *low;
        uint32_t pc;
        if (rx >= right_len) {
            arr = left, low = &lx, pc = left_pc;
        } else if (lx >= left_len) {
            arr = right, low = &rx, pc = right_pc;
        } else if (right->p[rx] < left->p[lx]) {
            arr = right, low = &rx, pc = right_pc;
        } else {
            arr = left, low = &lx, pc = left_pc;
        }
        /* extend */
        if (ix > 0 && arr->p[*low] <= merged->p[ix])
            return false;
        merged->p[merged->len++] = arr->p[*low];
        merged->p[merged->len++] = arr->p[*low + 1];
        *low += 2;
        ix += 2;
        (*next)[(*nnext)++] = pc;
    }
    return true;
}

static const Rune rx_any_rune_not_nl[] = {0, '\n' - 1, '\n' + 1, UNICODE_MAX_RUNE};
static const Rune rx_any_rune[] = {0, UNICODE_MAX_RUNE};

/* The fold orbit of r0 as ranges of one rune each, sorted. */
static bool rx_fold_runes(RxBuild *b, RxRunes *out, Rune r0) {
    Int n = 1;
    for (Rune r1 = unicode_simple_fold(r0); r1 != r0; r1 = unicode_simple_fold(r1))
        n++;
    out->p = (Rune *)rx_balloc(b, (size_t)(2 * n) * sizeof(Rune), _Alignof(Rune));
    if (out->p == NULL)
        return false;
    Int k = 0;
    out->p[k++] = r0;
    out->p[k++] = r0;
    for (Rune r1 = unicode_simple_fold(r0); r1 != r0; r1 = unicode_simple_fold(r1)) {
        out->p[k++] = r1;
        out->p[k++] = r1;
    }
    /* slices.Sort: insertion sort, the orbits are a handful long. */
    for (Int i = 1; i < k; i++) {
        Rune v = out->p[i];
        Int j = i;
        while (j > 0 && out->p[j - 1] > v) {
            out->p[j] = out->p[j - 1];
            j--;
        }
        out->p[j] = v;
    }
    out->len = k;
    return true;
}

/* The work check does for pc before or without looking at what follows it,
 * for the instructions that consume a rune or end the program. */
static bool rx_check_leaf(RxBuild *b, uint32_t pc) {
    RxOnePassInst *inst = &b->inst[pc];
    switch (inst->i.op) {
    case SYNTAX_INST_MATCH:
    case SYNTAX_INST_FAIL:
        b->m[pc] = inst->i.op == SYNTAX_INST_MATCH;
        return true;
    case SYNTAX_INST_RUNE: {
        b->m[pc] = false;
        if (inst->nnext > 0)
            return true;
        rx_q_insert(&b->inst_queue, inst->i.out);
        if (inst->i.rune.len == 0) {
            b->runes[pc].p = (Rune *)rx_balloc(b, 0, _Alignof(Rune));
            b->runes[pc].len = 0;
            return !b->oom && rx_next_fill(b, inst, 1, inst->i.out);
        }
        const Rune *ir = (const Rune *)inst->i.rune.p;
        bool ok;
        if (inst->i.rune.len == 1 && ((SyntaxFlags)inst->i.arg & SYNTAX_FOLD_CASE) != 0)
            ok = rx_fold_runes(b, &b->runes[pc], ir[0]);
        else
            ok = rx_runes_copy(b, &b->runes[pc], ir, inst->i.rune.len);
        if (!ok || !rx_next_fill(b, inst, b->runes[pc].len / 2 + 1, inst->i.out))
            return false;
        inst->i.op = SYNTAX_INST_RUNE;
        return true;
    }
    case SYNTAX_INST_RUNE1: {
        b->m[pc] = false;
        if (inst->nnext > 0)
            return true;
        rx_q_insert(&b->inst_queue, inst->i.out);
        Rune r0 = ((const Rune *)inst->i.rune.p)[0];
        bool ok;
        if (((SyntaxFlags)inst->i.arg & SYNTAX_FOLD_CASE) != 0) {
            ok = rx_fold_runes(b, &b->runes[pc], r0);
        } else {
            Rune two[2] = {r0, r0};
            ok = rx_runes_copy(b, &b->runes[pc], two, 2);
        }
        if (!ok || !rx_next_fill(b, inst, b->runes[pc].len / 2 + 1, inst->i.out))
            return false;
        inst->i.op = SYNTAX_INST_RUNE;
        return true;
    }
    case SYNTAX_INST_RUNE_ANY:
        b->m[pc] = false;
        if (inst->nnext > 0)
            return true;
        rx_q_insert(&b->inst_queue, inst->i.out);
        return rx_runes_copy(b, &b->runes[pc], rx_any_rune, 2) &&
               rx_next_fill(b, inst, 1, inst->i.out);
    case SYNTAX_INST_RUNE_ANY_NOT_NL:
        b->m[pc] = false;
        if (inst->nnext > 0)
            return true;
        rx_q_insert(&b->inst_queue, inst->i.out);
        return rx_runes_copy(b, &b->runes[pc], rx_any_rune_not_nl, 4) &&
               rx_next_fill(b, inst, 4 / 2 + 1, inst->i.out);
    default:
        return true;
    }
}

typedef struct RxCheckFrame {
    uint32_t pc;
    int stage;
} RxCheckFrame;

/* check, from makeOnePass: whether the program from pc0 is one pass, filling
 * in the runes and next lists on the way. Go's is a recursion as deep as the
 * program is long, and this keeps its own stack. A failure anywhere fails the
 * whole program, so it stops at the first. */
static bool rx_check(RxBuild *b, RxCheckFrame *stk, uint32_t pc0) {
    Int n = 0;
    stk[n].pc = pc0;
    stk[n].stage = 0;
    n++;
    while (n > 0) {
        RxCheckFrame *f = &stk[n - 1];
        uint32_t pc = f->pc;
        RxOnePassInst *inst = &b->inst[pc];
        if (f->stage == 0) {
            if (rx_q_contains(&b->visit_queue, pc)) {
                n--;
                continue;
            }
            rx_q_insert(&b->visit_queue, pc);
        }
        switch (inst->i.op) {
        case SYNTAX_INST_ALT:
        case SYNTAX_INST_ALT_MATCH: {
            if (f->stage < 2) {
                uint32_t child = f->stage == 0 ? inst->i.out : inst->i.arg;
                f->stage++;
                stk[n].pc = child;
                stk[n].stage = 0;
                n++;
                continue;
            }
            bool match_out = b->m[inst->i.out];
            bool match_arg = b->m[inst->i.arg];
            if (match_out && match_arg)
                return false;
            /* Match on empty goes in inst.Out */
            if (match_arg) {
                uint32_t t = inst->i.out;
                inst->i.out = inst->i.arg;
                inst->i.arg = t;
                match_out = match_arg;
            }
            if (match_out) {
                b->m[pc] = true;
                inst->i.op = SYNTAX_INST_ALT_MATCH;
            }
            /* build a dispatch operator from the two legs */
            if (!rx_merge_rune_sets(b, &b->runes[inst->i.out], &b->runes[inst->i.arg],
                                    inst->i.out, inst->i.arg, &b->runes[pc],
                                    &inst->next, &inst->nnext))
                return false;
            n--;
            continue;
        }
        case SYNTAX_INST_CAPTURE:
        case SYNTAX_INST_NOP:
        case SYNTAX_INST_EMPTY_WIDTH: {
            if (f->stage == 0) {
                f->stage = 1;
                stk[n].pc = inst->i.out;
                stk[n].stage = 0;
                n++;
                continue;
            }
            b->m[pc] = b->m[inst->i.out];
            /* pass matching runes through these no-ops */
            const RxRunes *from = &b->runes[inst->i.out];
            if (!rx_runes_copy(b, &b->runes[pc], from->p, from->len) ||
                !rx_next_fill(b, inst, b->runes[pc].len / 2 + 1, inst->i.out))
                return false;
            n--;
            continue;
        }
        default:
            if (!rx_check_leaf(b, pc))
                return false;
            n--;
            continue;
        }
    }
    return true;
}

/* onePassCopy: a copy of prog with alternations of alternations rewritten so
 * that the loop at the heart of x* does not need a second Alt to get out. */
static RxOnePassInst *rx_onepass_copy(RxBuild *b, const SyntaxProg *prog) {
    Int n = prog->inst.len;
    RxOnePassInst *insts = (RxOnePassInst *)rx_balloc(
        b, (size_t)n * sizeof(RxOnePassInst), _Alignof(RxOnePassInst));
    if (insts == NULL)
        return NULL;
    const SyntaxInst *src = (const SyntaxInst *)prog->inst.p;
    for (Int i = 0; i < n; i++)
        insts[i].i = src[i];

    /* rewrites one or more common Prog constructs that enable some otherwise
     * non-onepass Progs to be onepass. A:BD (for example) means an InstAlt at
     * ip A, that points to ips B & C.
     * A:BC + B:DA => A:BC + B:CD
     * A:BC + B:DC => A:DC + B:DC */
    for (Int pc = 0; pc < n; pc++) {
        switch (insts[pc].i.op) {
        case SYNTAX_INST_ALT:
        case SYNTAX_INST_ALT_MATCH:
            break;
        default:
            continue;
        }
        /* A:Bx + B:Ay */
        uint32_t *p_a_other = &insts[pc].i.out;
        uint32_t *p_a_alt = &insts[pc].i.arg;
        /* make sure a target is another Alt */
        SyntaxInst inst_alt = insts[*p_a_alt].i;
        if (!(inst_alt.op == SYNTAX_INST_ALT || inst_alt.op == SYNTAX_INST_ALT_MATCH)) {
            uint32_t *t = p_a_alt;
            p_a_alt = p_a_other;
            p_a_other = t;
            inst_alt = insts[*p_a_alt].i;
            if (!(inst_alt.op == SYNTAX_INST_ALT ||
                  inst_alt.op == SYNTAX_INST_ALT_MATCH))
                continue;
        }
        SyntaxInst inst_other = insts[*p_a_other].i;
        /* Analyzing both legs pointing to Alts is for another day */
        if (inst_other.op == SYNTAX_INST_ALT || inst_other.op == SYNTAX_INST_ALT_MATCH)
            /* too complicated */
            continue;
        /* simple empty transition loop
         * A:BC + B:DA => A:BC + B:DC */
        uint32_t *p_b_alt = &insts[*p_a_alt].i.out;
        uint32_t *p_b_other = &insts[*p_a_alt].i.arg;
        bool patch = false;
        if (inst_alt.out == (uint32_t)pc) {
            patch = true;
        } else if (inst_alt.arg == (uint32_t)pc) {
            patch = true;
            uint32_t *t = p_b_alt;
            p_b_alt = p_b_other;
            p_b_other = t;
        }
        if (patch)
            *p_b_alt = *p_a_other;
        /* empty transition to common target
         * A:BC + B:DC => A:DC + B:DC */
        if (*p_a_other == *p_b_alt)
            *p_a_alt = *p_b_other;
    }
    return insts;
}

RxOnePass *rx_compile_onepass(Alloc *a, const SyntaxProg *prog, bool *oom) {
    *oom = false;
    const SyntaxInst *insts = (const SyntaxInst *)prog->inst.p;
    Int n = prog->inst.len;
    if (prog->start == 0)
        return NULL;
    /* onepass regexp is anchored */
    if (insts[prog->start].op != SYNTAX_INST_EMPTY_WIDTH ||
        ((SyntaxEmptyOp)insts[prog->start].arg & SYNTAX_EMPTY_BEGIN_TEXT) !=
            SYNTAX_EMPTY_BEGIN_TEXT)
        return NULL;
    bool has_alt = false;
    for (Int k = 0; k < n; k++) {
        if (insts[k].op == SYNTAX_INST_ALT || insts[k].op == SYNTAX_INST_ALT_MATCH) {
            has_alt = true;
            break;
        }
    }
    /* every instruction leading to InstMatch must be EmptyEndText */
    for (Int k = 0; k < n; k++) {
        const SyntaxInst *inst = &insts[k];
        SyntaxInstOp op_out = insts[inst->out].op;
        switch (inst->op) {
        case SYNTAX_INST_ALT:
        case SYNTAX_INST_ALT_MATCH:
            if (op_out == SYNTAX_INST_MATCH || insts[inst->arg].op == SYNTAX_INST_MATCH)
                return NULL;
            break;
        case SYNTAX_INST_EMPTY_WIDTH:
            if (op_out == SYNTAX_INST_MATCH) {
                if (((SyntaxEmptyOp)inst->arg & SYNTAX_EMPTY_END_TEXT) ==
                    SYNTAX_EMPTY_END_TEXT)
                    continue;
                return NULL;
            }
            break;
        default:
            if (op_out == SYNTAX_INST_MATCH && has_alt)
                return NULL;
            break;
        }
    }

    /* Creates a slightly optimized copy of the original Prog that cleans up
     * some Prog idioms that block valid onepass programs. */
    /* makeOnePass gives up on long programs; checking first saves the copy. */
    if (n >= 1000) /* not worth it */
        return NULL;
    RxBuild b;
    memset(&b, 0, sizeof b);
    b.a = a;
    b.ninst = n;
    b.inst = rx_onepass_copy(&b, prog);
    if (b.inst == NULL) {
        *oom = true;
        return NULL;
    }

    /* makeOnePass: checkAmbiguity on InstAlts, build onepass Prog if
     * possible. */
    b.runes = (RxRunes *)rx_balloc(&b, (size_t)n * sizeof(RxRunes), _Alignof(RxRunes));
    b.m = (bool *)rx_balloc(&b, (size_t)n * sizeof(bool), _Alignof(bool));
    RxCheckFrame *stk = (RxCheckFrame *)rx_balloc(
        &b, (size_t)(n + 1) * sizeof(RxCheckFrame), _Alignof(RxCheckFrame));
    if (b.oom || !rx_q_init(&b, &b.inst_queue, n) ||
        !rx_q_init(&b, &b.visit_queue, n)) {
        *oom = true;
        return NULL;
    }
    rx_q_insert(&b.inst_queue, (uint32_t)prog->start);
    while (b.inst_queue.next_index < b.inst_queue.size) {
        b.visit_queue.size = 0;
        b.visit_queue.next_index = 0;
        uint32_t pc = b.inst_queue.dense[b.inst_queue.next_index++];
        if (!rx_check(&b, stk, pc)) {
            *oom = b.oom;
            return NULL;
        }
    }
    for (Int k = 0; k < n; k++) {
        b.inst[k].i.rune.p = b.runes[k].p;
        b.inst[k].i.rune.len = b.runes[k].len;
        b.inst[k].i.rune.cap = b.runes[k].len;
    }

    /* cleanupOnePass drops working memory, and restores certain shortcut
     * instructions. */
    for (Int k = 0; k < n; k++) {
        switch (insts[k].op) {
        case SYNTAX_INST_ALT:
        case SYNTAX_INST_ALT_MATCH:
        case SYNTAX_INST_RUNE:
            break;
        case SYNTAX_INST_RUNE1:
        case SYNTAX_INST_RUNE_ANY:
        case SYNTAX_INST_RUNE_ANY_NOT_NL:
            b.inst[k].i = insts[k];
            b.inst[k].next = NULL;
            b.inst[k].nnext = 0;
            break;
        default:
            b.inst[k].next = NULL;
            b.inst[k].nnext = 0;
            break;
        }
    }

    RxOnePass *op = (RxOnePass *)rx_balloc(&b, sizeof(RxOnePass), _Alignof(RxOnePass));
    if (op == NULL) {
        *oom = true;
        return NULL;
    }
    op->inst = b.inst;
    op->ninst = n;
    op->start = prog->start;
    op->num_cap = prog->num_cap;
    return op;
}
