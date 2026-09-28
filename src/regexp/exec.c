/* regexp: the inputs, the Pike VM and the one pass matcher, from exec.go and
 * the input half of regexp.go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp.h"

#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "regexp_internal.h"

#include <string.h>

void rx_oom(void) {
    panic_str(BURROW_S("regexp: out of memory"));
}

/* ------------------------------------------------------------------ inputs */

Rune rx_step_reader(RxInput *in, Int pos, Int *width) {
    if (!in->at_eot && pos != in->rpos) {
        *width = 0;
        return RX_EOT;
    }
    Int w = 0;
    Error err = BURROW_NO_ERROR;
    Rune r = in->r->vt->read_rune(in->r->data, &w, &err);
    if (BURROW_FAILED(err)) {
        in->at_eot = true;
        *width = 0;
        return RX_EOT;
    }
    in->rpos += w;
    *width = w;
    return r;
}

RxFlag rx_context(const RxInput *in, Int pos) {
    if (in->r != NULL)
        return 0; /* not used */
    Rune r1 = RX_EOT, r2 = RX_EOT;
    Int w;
    if ((Uint)(pos - 1) < (Uint)in->len)
        r1 = utf8_decode_last_rune_in_string(str_from_bytes(in->p, pos), &w);
    if ((Uint)pos < (Uint)in->len)
        r2 = utf8_decode_rune_in_string(
            str_from_bytes(rx_at(in->p, pos), in->len - pos), &w);
    return rx_flag(r1, r2);
}

bool rx_flag_match(RxFlag f, SyntaxEmptyOp op) {
    if (op == 0)
        return true;
    Rune r1 = (Rune)(int32_t)(uint32_t)(f >> 32);
    if ((op & SYNTAX_EMPTY_BEGIN_LINE) != 0) {
        if (r1 != '\n' && r1 >= 0)
            return false;
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_BEGIN_LINE;
    }
    if ((op & SYNTAX_EMPTY_BEGIN_TEXT) != 0) {
        if (r1 >= 0)
            return false;
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_BEGIN_TEXT;
    }
    if (op == 0)
        return true;
    Rune r2 = (Rune)(int32_t)(uint32_t)f;
    if ((op & SYNTAX_EMPTY_END_LINE) != 0) {
        if (r2 != '\n' && r2 >= 0)
            return false;
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_END_LINE;
    }
    if ((op & SYNTAX_EMPTY_END_TEXT) != 0) {
        if (r2 >= 0)
            return false;
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_END_TEXT;
    }
    if (op == 0)
        return true;
    if (syntax_is_word_char(r1) != syntax_is_word_char(r2))
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_WORD_BOUNDARY;
    else
        op &= (SyntaxEmptyOp)~SYNTAX_EMPTY_NO_WORD_BOUNDARY;
    return op == 0;
}

/* ------------------------------------------------------------------ the VM */

typedef struct RxThread {
    const SyntaxInst *inst;
    Int *cap;
} RxThread;

typedef struct RxEntry {
    uint32_t pc;
    RxThread *t;
} RxEntry;

/* A sparse set of program counters, with a thread for each. */
typedef struct RxQueue {
    uint32_t *sparse;
    RxEntry *dense;
    Int len;
} RxQueue;

/* What add has left to do: go to pc, or when slot is not -1, put val back in
 * that capture slot, which is what Go's recursion does on the way out of a
 * capture instruction. */
typedef struct RxJob {
    uint32_t pc;
    int32_t slot;
    Int val;
} RxJob;

/* Threads come in chunks from the heap, and a machine never has more than two
 * queues' worth plus one alive, so the chunks stop growing early. */
#define RX_CHUNK_THREADS 64

typedef struct RxChunk {
    struct RxChunk *next;
    size_t size;
} RxChunk;

struct RxMachine {
    RxMachine *next;
    size_t size;
    const RxProg *p;
    bool longest;
    RxQueue q0, q1;
    RxThread **pool;
    Int npool;
    RxChunk *chunks;
    bool matched;
    Int *matchcap;
    Int ncap;
    RxJob *stack;
};

#define RX_ALIGN(n, a) (((n) + (a) - 1) / (a) * (a))

static RxMachine *rx_machine_new(const RxProg *p) {
    size_t n = (size_t)p->ninst;
    size_t off = RX_ALIGN(sizeof(RxMachine), sizeof(Int));
    size_t o_mc = off;
    off += (size_t)p->matchcap * sizeof(Int);
    size_t o_stack = off;
    off += (n + 1) * sizeof(RxJob);
    size_t o_pool = RX_ALIGN(off, sizeof(void *));
    off = o_pool + (2 * n + 2 + RX_CHUNK_THREADS) * sizeof(RxThread *);
    size_t o_dense0 = RX_ALIGN(off, sizeof(void *));
    off = o_dense0 + n * sizeof(RxEntry);
    size_t o_dense1 = off;
    off += n * sizeof(RxEntry);
    size_t o_sparse0 = off;
    off += n * sizeof(uint32_t);
    size_t o_sparse1 = off;
    off += n * sizeof(uint32_t);
    Byte *b = (Byte *)mem_alloc(heap_allocator(), off, _Alignof(RxJob));
    if (b == NULL)
        rx_oom();
    RxMachine *m = (RxMachine *)(void *)b;
    m->size = off;
    m->p = p;
    m->matchcap = (Int *)(void *)(b + o_mc);
    m->stack = (RxJob *)(void *)(b + o_stack);
    m->pool = (RxThread **)(void *)(b + o_pool);
    m->q0.dense = (RxEntry *)(void *)(b + o_dense0);
    m->q1.dense = (RxEntry *)(void *)(b + o_dense1);
    m->q0.sparse = (uint32_t *)(void *)(b + o_sparse0);
    m->q1.sparse = (uint32_t *)(void *)(b + o_sparse1);
    return m;
}

static void rx_machine_free(RxMachine *m) {
    Alloc *h = heap_allocator();
    for (RxChunk *c = m->chunks; c != NULL;) {
        RxChunk *next = c->next;
        mem_free(h, c, c->size, _Alignof(RxThread));
        c = next;
    }
    mem_free(h, m, m->size, _Alignof(RxJob));
}

/* A chunk of threads, all into the pool. */
static void rx_machine_grow(RxMachine *m) {
    size_t hdr = RX_ALIGN(sizeof(RxChunk), _Alignof(RxThread));
    size_t caps = (size_t)m->p->matchcap * sizeof(Int);
    size_t size = hdr + RX_CHUNK_THREADS * (sizeof(RxThread) + caps);
    Byte *b = (Byte *)mem_alloc(heap_allocator(), size, _Alignof(RxThread));
    if (b == NULL)
        rx_oom();
    RxChunk *c = (RxChunk *)(void *)b;
    c->size = size;
    c->next = m->chunks;
    m->chunks = c;
    RxThread *t = (RxThread *)(void *)(b + hdr);
    Int *cap = (Int *)(void *)(t + RX_CHUNK_THREADS);
    for (int i = 0; i < RX_CHUNK_THREADS; i++) {
        t[i].cap = cap + (size_t)i * (size_t)m->p->matchcap;
        m->pool[m->npool++] = &t[i];
    }
}

static inline RxThread *rx_alloc(RxMachine *m, const SyntaxInst *i) {
    if (m->npool == 0)
        rx_machine_grow(m);
    RxThread *t = m->pool[--m->npool];
    t->inst = i;
    return t;
}

static inline void rx_free_thread(RxMachine *m, RxThread *t) {
    m->pool[m->npool++] = t;
}

static RxMachine *rx_get(const Regexp *re) {
    RxProg *p = re->p;
    sync_mutex_lock(&p->mu);
    RxMachine *m = p->machines;
    if (m != NULL)
        p->machines = m->next;
    sync_mutex_unlock(&p->mu);
    if (m == NULL)
        m = rx_machine_new(p);
    m->longest = re->longest;
    return m;
}

static void rx_put(const Regexp *re, RxMachine *m) {
    RxProg *p = re->p;
    sync_mutex_lock(&p->mu);
    m->next = p->machines;
    p->machines = m;
    sync_mutex_unlock(&p->mu);
}

void rx_free_machines(RxProg *p) {
    for (RxMachine *m = p->machines; m != NULL;) {
        RxMachine *next = m->next;
        rx_machine_free(m);
        m = next;
    }
    p->machines = NULL;
}

/* machine.add: follows pc to the instructions that consume a rune or match,
 * putting a thread on q for each, with the captures cap has at that point.
 * Go writes it as a recursion as deep as the program is long, and this keeps
 * the pending branches on m->stack instead, in the same order. t is a thread
 * the caller has finished with, used for the first thread this makes that is
 * not under a capture, since those take cap by value. */
static RxThread *rx_add(RxMachine *m, RxQueue *q, uint32_t pc0, Int pos, Int *cap,
                        RxFlag cond, RxThread *t) {
    const SyntaxInst *prog = m->p->inst;
    Int ncap = m->ncap;
    RxJob *stk = m->stack;
    Int n = 0;
    Int restores = 0;
    stk[n].pc = pc0;
    stk[n].slot = -1;
    n++;
    while (n > 0) {
        RxJob j = stk[--n];
        if (j.slot >= 0) {
            cap[j.slot] = j.val;
            restores--;
            continue;
        }
        uint32_t pc = j.pc;
        for (;;) {
            if (pc == 0)
                break;
            uint32_t k = q->sparse[pc];
            if (k < (uint32_t)q->len && q->dense[k].pc == pc)
                break;
            RxEntry *d = &q->dense[q->len];
            q->sparse[pc] = (uint32_t)q->len;
            q->len++;
            d->t = NULL;
            d->pc = pc;
            const SyntaxInst *i = &prog[pc];
            switch (i->op) {
            case SYNTAX_INST_FAIL:
                goto next;
            case SYNTAX_INST_ALT:
            case SYNTAX_INST_ALT_MATCH:
                stk[n].pc = i->arg;
                stk[n].slot = -1;
                n++;
                pc = i->out;
                continue;
            case SYNTAX_INST_EMPTY_WIDTH:
                if (rx_flag_match(cond, (SyntaxEmptyOp)i->arg)) {
                    pc = i->out;
                    continue;
                }
                goto next;
            case SYNTAX_INST_NOP:
                pc = i->out;
                continue;
            case SYNTAX_INST_CAPTURE:
                if ((Int)i->arg < ncap) {
                    stk[n].slot = (int32_t)i->arg;
                    stk[n].val = cap[i->arg];
                    n++;
                    restores++;
                    cap[i->arg] = pos;
                }
                pc = i->out;
                continue;
            case SYNTAX_INST_MATCH:
            case SYNTAX_INST_RUNE:
            case SYNTAX_INST_RUNE1:
            case SYNTAX_INST_RUNE_ANY:
            case SYNTAX_INST_RUNE_ANY_NOT_NL: {
                RxThread *u;
                if (restores == 0 && t != NULL) {
                    u = t;
                    t = NULL;
                    u->inst = i;
                } else {
                    u = rx_alloc(m, i);
                }
                if (ncap > 0 && u->cap != cap)
                    memcpy(u->cap, cap, (size_t)ncap * sizeof(Int));
                d->t = u;
                goto next;
            }
            default:
                panic_str(BURROW_S("unhandled"));
            }
        }
    next:;
    }
    return t;
}

static void rx_clear(RxMachine *m, RxQueue *q) {
    for (Int j = 0; j < q->len; j++)
        if (q->dense[j].t != NULL)
            rx_free_thread(m, q->dense[j].t);
    q->len = 0;
}

/* machine.step: runs the threads on runq over c, which is at pos, putting
 * what comes next on nextq. */
static void rx_step_threads(RxMachine *m, RxQueue *runq, RxQueue *nextq, Int pos,
                            Int next_pos, Rune c, RxFlag next_cond) {
    bool longest = m->longest;
    Int ncap = m->ncap;
    for (Int j = 0; j < runq->len; j++) {
        RxThread *t = runq->dense[j].t;
        if (t == NULL)
            continue;
        if (longest && m->matched && ncap > 0 && m->matchcap[0] < t->cap[0]) {
            rx_free_thread(m, t);
            continue;
        }
        const SyntaxInst *i = t->inst;
        bool add = false;
        switch (i->op) {
        case SYNTAX_INST_MATCH:
            if (ncap > 0 && (!longest || !m->matched || m->matchcap[1] < pos)) {
                t->cap[1] = pos;
                memcpy(m->matchcap, t->cap, (size_t)ncap * sizeof(Int));
            }
            if (!longest) {
                /* First match wins, so the lower priority threads go. */
                for (Int k = j + 1; k < runq->len; k++)
                    if (runq->dense[k].t != NULL)
                        rx_free_thread(m, runq->dense[k].t);
                runq->len = 0;
            }
            m->matched = true;
            break;
        case SYNTAX_INST_RUNE:
            add = syntax_inst_match_rune(i, c);
            break;
        case SYNTAX_INST_RUNE1:
            add = c == ((const Rune *)i->rune.p)[0];
            break;
        case SYNTAX_INST_RUNE_ANY:
            add = true;
            break;
        case SYNTAX_INST_RUNE_ANY_NOT_NL:
            add = c != '\n';
            break;
        default:
            panic_str(BURROW_S("bad inst"));
        }
        if (add)
            t = rx_add(m, nextq, i->out, next_pos, t->cap, next_cond, t);
        if (t != NULL)
            rx_free_thread(m, t);
    }
    runq->len = 0;
}

/* machine.match. */
static bool rx_machine_match(RxMachine *m, RxInput *in, Int pos) {
    const RxProg *p = m->p;
    SyntaxEmptyOp start_cond = p->cond;
    if (start_cond == (SyntaxEmptyOp)0xFF)
        return false;
    m->matched = false;
    for (Int k = 0; k < m->ncap; k++)
        m->matchcap[k] = -1;
    RxQueue *runq = &m->q0, *nextq = &m->q1;
    Rune r1 = RX_EOT;
    Int width = 0, width1 = 0;
    Rune r = rx_step(in, pos, &width);
    if (r != RX_EOT)
        r1 = rx_step(in, pos + width, &width1);
    RxFlag flag = pos == 0 ? rx_flag(-1, r) : rx_context(in, pos);
    for (;;) {
        if (runq->len == 0) {
            if ((start_cond & SYNTAX_EMPTY_BEGIN_TEXT) != 0 && pos != 0)
                break; /* anchored match, past beginning of text */
            if (m->matched)
                break; /* have match; finished exploring alternatives */
            if (p->prefix.len > 0 && r1 != p->prefix_rune && in->r == NULL) {
                /* Match requires literal prefix; fast search for it. */
                Int advance = strings_index(
                    str_from_bytes(rx_at(in->p, pos), in->len - pos), p->prefix);
                if (advance < 0)
                    break;
                pos += advance;
                r = rx_step(in, pos, &width);
                r1 = rx_step(in, pos + width, &width1);
            }
        }
        if (!m->matched) {
            if (m->ncap > 0)
                m->matchcap[0] = pos;
            rx_add(m, runq, (uint32_t)p->prog->start, pos, m->matchcap, flag, NULL);
        }
        flag = rx_flag(r, r1);
        rx_step_threads(m, runq, nextq, pos, pos + width, r, flag);
        if (width == 0)
            break;
        if (m->ncap == 0 && m->matched)
            break; /* found a match and not paying attention to where it is */
        pos += width;
        r = r1;
        width = width1;
        if (r != RX_EOT)
            r1 = rx_step(in, pos + width, &width1);
        RxQueue *tmp = runq;
        runq = nextq;
        nextq = tmp;
    }
    rx_clear(m, nextq);
    return m->matched;
}

/* ---------------------------------------------------------------- one pass */

static uint32_t rx_onepass_next(const RxOnePassInst *i, Rune r) {
    Int next = syntax_inst_match_rune_pos(&i->i, r);
    if (next >= 0)
        return i->next[next];
    if (i->i.op == SYNTAX_INST_ALT_MATCH)
        return i->i.out;
    return 0;
}

bool rx_onepass(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap) {
    const RxProg *p = re->p;
    const RxOnePass *op = p->onepass;
    if (p->cond == (SyntaxEmptyOp)0xFF)
        return false;
    bool matched = false;
    for (Int k = 0; k < ncap; k++)
        cap[k] = -1;
    Rune r1 = RX_EOT;
    Int width = 0, width1 = 0;
    Rune r = rx_step(in, pos, &width);
    if (r != RX_EOT)
        r1 = rx_step(in, pos + width, &width1);
    RxFlag flag = pos == 0 ? rx_flag(-1, r) : rx_context(in, pos);
    Int pc = op->start;
    const RxOnePassInst *inst = &op->inst[pc];
    /* If there is a simple literal prefix, skip over it. */
    if (pos == 0 && rx_flag_match(flag, (SyntaxEmptyOp)inst->i.arg) &&
        p->prefix.len > 0 && in->r == NULL) {
        /* Match requires literal prefix; fast search for it. */
        if (!strings_has_prefix(str_from_bytes(in->p, in->len), p->prefix))
            return false;
        pos += p->prefix.len;
        r = rx_step(in, pos, &width);
        r1 = rx_step(in, pos + width, &width1);
        flag = rx_context(in, pos);
        pc = (Int)p->prefix_end;
    }
    for (;;) {
        inst = &op->inst[pc];
        pc = (Int)inst->i.out;
        switch (inst->i.op) {
        case SYNTAX_INST_MATCH:
            matched = true;
            if (ncap > 0) {
                cap[0] = 0;
                cap[1] = pos;
            }
            return matched;
        case SYNTAX_INST_RUNE:
            if (!syntax_inst_match_rune(&inst->i, r))
                return matched;
            break;
        case SYNTAX_INST_RUNE1:
            if (r != ((const Rune *)inst->i.rune.p)[0])
                return matched;
            break;
        case SYNTAX_INST_RUNE_ANY:
            /* Matches any rune. */
            break;
        case SYNTAX_INST_RUNE_ANY_NOT_NL:
            if (r == '\n')
                return matched;
            break;
        /* peek at the input rune to see which branch of the Alt to take */
        case SYNTAX_INST_ALT:
        case SYNTAX_INST_ALT_MATCH:
            pc = (Int)rx_onepass_next(inst, r);
            continue;
        case SYNTAX_INST_FAIL:
            return matched;
        case SYNTAX_INST_NOP:
            continue;
        case SYNTAX_INST_EMPTY_WIDTH:
            if (!rx_flag_match(flag, (SyntaxEmptyOp)inst->i.arg))
                return matched;
            continue;
        case SYNTAX_INST_CAPTURE:
            if ((Int)inst->i.arg < ncap)
                cap[inst->i.arg] = pos;
            continue;
        default:
            panic_str(BURROW_S("bad inst"));
        }
        if (width == 0)
            break;
        flag = rx_flag(r, r1);
        pos += width;
        r = r1;
        width = width1;
        if (r != RX_EOT)
            r1 = rx_step(in, pos + width, &width1);
    }
    return matched;
}

/* -------------------------------------------------------------------- find */

bool rx_find(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap) {
    const RxProg *p = re->p;
    if (in->r == NULL && in->len < p->min_input_len)
        return false;
    if (p->onepass != NULL)
        return rx_onepass(re, in, pos, ncap, cap);
    if (in->r == NULL && in->len < p->max_bitstate_len)
        return rx_backtrack(re, in, pos, ncap, cap);
    RxMachine *m = rx_get(re);
    m->ncap = ncap;
    bool ok = rx_machine_match(m, in, pos);
    if (ok && ncap > 0)
        memcpy(cap, m->matchcap, (size_t)ncap * sizeof(Int));
    rx_put(re, m);
    return ok;
}
