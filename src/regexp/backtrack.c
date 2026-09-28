/* regexp: the backtracker, from backtrack.go.
 *
 * It runs small programs over short inputs, where it beats the Pike VM, and
 * keeps a bit for every instruction at every position so that it never tries
 * the same one twice, which keeps it linear. Go's comment on the idea: see
 * https://swtch.com/~rsc/regexp/regexp2.html#backtrack and RE2's bitstate.cc.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp.h"

#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strings.h"

#include "regexp_internal.h"

#include <string.h>

/* A job on the backtrack stack: pc at pos, and arg for the second visit to an
 * alt or a capture. */
typedef struct RxBtJob {
    uint32_t pc;
    bool arg;
    Int pos;
} RxBtJob;

#define RX_VISITED_BITS 32
#define RX_MAX_BACKTRACK_PROG 500            /* len(prog.Inst) <= max */
#define RX_MAX_BACKTRACK_VECTOR (256 * 1024) /* bit vector size <= max (bits) */
#define RX_VISITED_WORDS (RX_MAX_BACKTRACK_VECTOR / RX_VISITED_BITS)

struct RxBitState {
    RxBitState *next;
    size_t size;
    Int end;
    Int ncap;
    Int *cap;
    Int *matchcap;
    RxBtJob *jobs;
    Int njobs;
    Int jobs_cap;
    uint32_t *visited;
};

Int rx_max_bitstate_len(const SyntaxProg *prog) {
    if (prog->inst.len > RX_MAX_BACKTRACK_PROG)
        return 0;
    return RX_MAX_BACKTRACK_VECTOR / prog->inst.len;
}

static RxBitState *rx_bitstate_get(RxProg *p) {
    sync_mutex_lock(&p->mu);
    RxBitState *b = p->bitstates;
    if (b != NULL)
        p->bitstates = b->next;
    sync_mutex_unlock(&p->mu);
    if (b != NULL)
        return b;
    size_t off = sizeof(RxBitState);
    off = (off + sizeof(Int) - 1) / sizeof(Int) * sizeof(Int);
    size_t o_cap = off;
    off += 2 * (size_t)p->matchcap * sizeof(Int);
    size_t o_visited = off;
    off += RX_VISITED_WORDS * sizeof(uint32_t);
    Byte *m = (Byte *)mem_alloc(heap_allocator(), off, _Alignof(Int));
    if (m == NULL)
        rx_oom();
    b = (RxBitState *)(void *)m;
    b->size = off;
    b->cap = (Int *)(void *)(m + o_cap);
    b->matchcap = b->cap + p->matchcap;
    b->visited = (uint32_t *)(void *)(m + o_visited);
    return b;
}

static void rx_bitstate_put(RxProg *p, RxBitState *b) {
    sync_mutex_lock(&p->mu);
    b->next = p->bitstates;
    p->bitstates = b;
    sync_mutex_unlock(&p->mu);
}

void rx_free_bitstates(RxProg *p) {
    Alloc *h = heap_allocator();
    for (RxBitState *b = p->bitstates; b != NULL;) {
        RxBitState *next = b->next;
        if (b->jobs != NULL)
            mem_free(h, b->jobs, (size_t)b->jobs_cap * sizeof(RxBtJob),
                     _Alignof(RxBtJob));
        mem_free(h, b, b->size, _Alignof(Int));
        b = next;
    }
    p->bitstates = NULL;
}

/* bitState.reset. */
static void rx_bitstate_reset(RxBitState *b, const RxProg *p, Int end, Int ncap) {
    b->end = end;
    b->njobs = 0;
    Int visited = (p->ninst * (end + 1) + RX_VISITED_BITS - 1) / RX_VISITED_BITS;
    memset(b->visited, 0, (size_t)visited * sizeof(uint32_t));
    b->ncap = ncap;
    for (Int i = 0; i < ncap; i++) {
        b->cap[i] = -1;
        b->matchcap[i] = -1;
    }
}

/* bitState.shouldVisit: whether (pc, pos) is new, marking it seen. */
static inline bool rx_should_visit(RxBitState *b, uint32_t pc, Int pos) {
    Uint n = (Uint)((Int)pc * (b->end + 1) + pos);
    uint32_t bit = (uint32_t)1 << (n & (RX_VISITED_BITS - 1));
    if ((b->visited[n / RX_VISITED_BITS] & bit) != 0)
        return false;
    b->visited[n / RX_VISITED_BITS] |= bit;
    return true;
}

/* bitState.push. */
static void rx_bt_push(RxBitState *b, const RxProg *p, uint32_t pc, Int pos, bool arg) {
    /* Only check shouldVisit when arg is false. When arg is true, we are
     * continuing a previous visit. */
    if (p->inst[pc].op == SYNTAX_INST_FAIL || (!arg && !rx_should_visit(b, pc, pos)))
        return;
    if (b->njobs == b->jobs_cap) {
        Int ncap = b->jobs_cap == 0 ? 256 : b->jobs_cap * 2;
        RxBtJob *j = (RxBtJob *)mem_realloc(
            heap_allocator(), b->jobs, (size_t)b->jobs_cap * sizeof(RxBtJob),
            (size_t)ncap * sizeof(RxBtJob), _Alignof(RxBtJob));
        if (j == NULL)
            rx_oom();
        b->jobs = j;
        b->jobs_cap = ncap;
    }
    b->jobs[b->njobs].pc = pc;
    b->jobs[b->njobs].arg = arg;
    b->jobs[b->njobs].pos = pos;
    b->njobs++;
}

/* Regexp.tryBacktrack: runs a backtracking search starting at pos. */
static bool rx_try_backtrack(const Regexp *re, RxBitState *b, RxInput *in, uint32_t pc0,
                             Int pos0) {
    const RxProg *p = re->p;
    bool longest = re->longest;
    rx_bt_push(b, p, pc0, pos0, false);
    while (b->njobs > 0) {
        b->njobs--;
        uint32_t pc = b->jobs[b->njobs].pc;
        Int pos = b->jobs[b->njobs].pos;
        bool arg = b->jobs[b->njobs].arg;
        goto skip;
    check_and_loop:
        if (!rx_should_visit(b, pc, pos))
            continue;
    skip:;
        const SyntaxInst *inst = &p->inst[pc];
        Rune r;
        Int width;
        switch (inst->op) {
        case SYNTAX_INST_FAIL:
            panic_str(BURROW_S("unexpected InstFail"));
        case SYNTAX_INST_ALT:
            /* Cannot just
             *   b->push(inst->out, pos, false);
             *   b->push(inst->arg, pos, false);
             * If during the processing of inst->out, we encounter inst->arg
             * via another path, we want to process it then. Pushing it here
             * will inhibit that. Instead, re-push inst with arg==true as a
             * reminder to push inst->arg out later. */
            if (arg) {
                /* Finished inst->out; try inst->arg. */
                arg = false;
                pc = inst->arg;
                goto check_and_loop;
            }
            rx_bt_push(b, p, pc, pos, true);
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_ALT_MATCH:
            /* One opcode consumes runes; the other leads to match. */
            switch (p->inst[inst->out].op) {
            case SYNTAX_INST_RUNE:
            case SYNTAX_INST_RUNE1:
            case SYNTAX_INST_RUNE_ANY:
            case SYNTAX_INST_RUNE_ANY_NOT_NL:
                /* inst->arg is the match. */
                rx_bt_push(b, p, inst->arg, pos, false);
                pc = inst->arg;
                pos = b->end;
                goto check_and_loop;
            default:
                break;
            }
            /* inst->out is the match - non-greedy */
            rx_bt_push(b, p, inst->out, b->end, false);
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_RUNE:
            r = rx_step(in, pos, &width);
            if (!syntax_inst_match_rune(inst, r))
                continue;
            pos += width;
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_RUNE1:
            r = rx_step(in, pos, &width);
            if (r != ((const Rune *)inst->rune.p)[0])
                continue;
            pos += width;
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_RUNE_ANY_NOT_NL:
            r = rx_step(in, pos, &width);
            if (r == '\n' || r == RX_EOT)
                continue;
            pos += width;
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_RUNE_ANY:
            r = rx_step(in, pos, &width);
            if (r == RX_EOT)
                continue;
            pos += width;
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_CAPTURE:
            if (arg) {
                /* Finished inst->out; restore the old value. */
                b->cap[inst->arg] = pos;
                continue;
            }
            if ((Int)inst->arg < b->ncap) {
                /* Capture pos to register, but save old value. */
                rx_bt_push(b, p, pc, b->cap[inst->arg],
                           true); /* come back when we're done. */
                b->cap[inst->arg] = pos;
            }
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_EMPTY_WIDTH:
            if (!rx_flag_match(rx_context(in, pos), (SyntaxEmptyOp)inst->arg))
                continue;
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_NOP:
            pc = inst->out;
            goto check_and_loop;
        case SYNTAX_INST_MATCH: {
            /* We found a match. If the caller doesn't care where the match
             * is, no point going further. */
            if (b->ncap == 0)
                return true;
            /* Record best match so far. Only need to check end point, because
             * this entire call is only considering one start position. */
            if (b->ncap > 1)
                b->cap[1] = pos;
            Int old = b->matchcap[1];
            if (old == -1 || (longest && pos > 0 && pos > old))
                memcpy(b->matchcap, b->cap, (size_t)b->ncap * sizeof(Int));
            /* If going for first match, we're done. */
            if (!longest)
                return true;
            /* If we used the entire text, no longer match is possible. */
            if (pos == b->end)
                return true;
            /* Otherwise, continue on in hope of a longer match. */
            continue;
        }
        default:
            panic_str(BURROW_S("bad inst"));
        }
    }
    return longest && b->ncap > 1 && b->matchcap[1] >= 0;
}

/* Regexp.backtrack: runs a backtracking search of prog on the input starting
 * at pos. */
bool rx_backtrack(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap) {
    RxProg *p = re->p;
    SyntaxEmptyOp start_cond = p->cond;
    if (start_cond == (SyntaxEmptyOp)0xFF) /* impossible */
        return false;
    if ((start_cond & SYNTAX_EMPTY_BEGIN_TEXT) != 0 && pos != 0)
        /* Anchored match, past beginning of text. */
        return false;
    RxBitState *b = rx_bitstate_get(p);
    Int end = in->len;
    rx_bitstate_reset(b, p, end, ncap);
    uint32_t start = (uint32_t)p->prog->start;
    /* Anchored search must start at the beginning of the input. */
    if ((start_cond & SYNTAX_EMPTY_BEGIN_TEXT) != 0) {
        if (ncap > 0)
            b->cap[0] = pos;
        if (!rx_try_backtrack(re, b, in, start, pos)) {
            rx_bitstate_put(p, b);
            return false;
        }
    } else {
        /* Unanchored search, starting from each possible text position.
         * Notice that we have to try the empty string at the end of the
         * text, so the loop condition is pos <= end, not pos < end. This
         * looks like it's quadratic in the size of the text, but we are not
         * clearing visited between calls to tryBacktrack, so no work is
         * duplicated and it ends up still being linear. */
        Int width = -1;
        for (; pos <= end && width != 0; pos += width) {
            if (p->prefix.len > 0) {
                /* Match requires literal prefix; fast search for it. */
                Int advance = strings_index(
                    str_from_bytes(rx_at(in->p, pos), in->len - pos), p->prefix);
                if (advance < 0) {
                    rx_bitstate_put(p, b);
                    return false;
                }
                pos += advance;
            }
            if (ncap > 0)
                b->cap[0] = pos;
            if (rx_try_backtrack(re, b, in, start, pos))
                goto match; /* Match must be leftmost; done. */
            rx_step(in, pos, &width);
        }
        rx_bitstate_put(p, b);
        return false;
    }
match:
    if (ncap > 0)
        memcpy(cap, b->matchcap, (size_t)ncap * sizeof(Int));
    rx_bitstate_put(p, b);
    return true;
}
