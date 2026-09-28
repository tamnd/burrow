/* regexp: what the three matchers and the API on top of them share.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_REGEXP_INTERNAL_H
#define BURROW_REGEXP_INTERNAL_H

#include "burrow/regexp.h"
#include "burrow/regexp/syntax.h"

#include "burrow/mem/arena.h"
#include "burrow/sync.h"
#include "burrow/utf8.h"

#include <stdint.h>

/* endOfText: what stepping past the end of the input gives. */
#define RX_EOT ((Rune) - 1)

/* Where the input comes from: bytes, which is a string too, or a reader. */
typedef struct RxInput {
    const Byte *p;
    Int len;
    const IoRuneReader *r; /* NULL for bytes */
    bool at_eot;
    Int rpos;
} RxInput;

/* p + off, for input that may be the empty string with no pointer at all. */
static inline const Byte *rx_at(const Byte *p, Int off) {
    return p == NULL ? p : p + off;
}

/* The n bytes at p as a Str, without the call str_from_bytes costs. p may
 * be NULL when n is 0. */
static inline Str rx_str(const Byte *p, Int n) {
    Str s = {p, n};
    return s;
}

Rune rx_step_reader(RxInput *in, Int pos, Int *width);

/* input.step: the rune at pos and its width, or RX_EOT and 0. */
static inline Rune rx_step(RxInput *in, Int pos, Int *width) {
    if (in->r != NULL)
        return rx_step_reader(in, pos, width);
    if (pos < in->len) {
        Byte c = in->p[pos];
        if (c < 0x80) {
            *width = 1;
            return c;
        }
        return utf8_decode_rune_in_string(rx_str(in->p + pos, in->len - pos), width);
    }
    *width = 0;
    return RX_EOT;
}

/* lazyFlag: the runes either side of a position, from which the empty width
 * assertions are worked out only when one is asked about. */
typedef uint64_t RxFlag;

static inline RxFlag rx_flag(Rune r1, Rune r2) {
    return (uint64_t)(uint32_t)r1 << 32 | (uint64_t)(uint32_t)r2;
}

bool rx_flag_match(RxFlag f, SyntaxEmptyOp op);

/* input.context. */
RxFlag rx_context(const RxInput *in, Int pos);

/* One instruction of a one pass program: Go's onePassInst. */
typedef struct RxOnePassInst {
    SyntaxInst i;
    uint32_t *next;
    Int nnext;
} RxOnePassInst;

typedef struct RxOnePass {
    RxOnePassInst *inst;
    Int ninst;
    Int start;
    Int num_cap;
} RxOnePass;

typedef struct RxMachine RxMachine;
typedef struct RxBitState RxBitState;

/* Everything compile works out, shared by nothing: regexp_copy compiles
 * again. It lives in its own arena, and the working memory the searches keep
 * comes from the heap, under mu. */
typedef struct RxProg {
    Arena arena;
    Alloc *parent; /* where this struct came from */
    SyntaxFlags mode;
    Str expr;
    const SyntaxProg *prog;
    const SyntaxInst *inst;
    Int ninst;
    RxOnePass *onepass;
    Int num_subexp;
    Int max_bitstate_len;
    Slice subexp_names;
    Str prefix;
    Rune prefix_rune;
    uint32_t prefix_end;
    Int matchcap;
    bool prefix_complete;
    SyntaxEmptyOp cond;
    Int min_input_len;

    SyncMutex mu;
    RxMachine *machines;
    RxBitState *bitstates;
} RxProg;

struct Regexp {
    Alloc *a; /* where this struct came from */
    RxProg *p;
    bool longest;
};

/* Regexp.find: whether re matches the input from pos, and when it does the
 * first ncap capture positions in cap. */
bool rx_find(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap);

/* The one pass program for prog, in a, or NULL when prog is not one pass or
 * a ran out, which *oom tells apart. */
RxOnePass *rx_compile_onepass(Alloc *a, const SyntaxProg *prog, bool *oom);

/* compile, with the syntax flags and the semantics given. */
Regexp *rx_compile(Alloc *a, Str expr, SyntaxFlags mode, bool longest, Error *err);

/* onePassPrefix. *oom is set when a runs out, and is left alone otherwise. */
Str rx_onepass_prefix(Alloc *a, const SyntaxProg *prog, bool *complete, uint32_t *pc,
                      bool *oom);

/* maxBitStateLen. */
Int rx_max_bitstate_len(const SyntaxProg *prog);

/* Regexp.backtrack and Regexp.doOnePass. */
bool rx_backtrack(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap);
bool rx_onepass(const Regexp *re, RxInput *in, Int pos, Int ncap, Int *cap);

/* Frees the working memory the searches kept. */
void rx_free_machines(RxProg *p);
void rx_free_bitstates(RxProg *p);

/* What a search does when the heap refuses. */
BURROW_NORETURN void rx_oom(void);

#endif /* BURROW_REGEXP_INTERNAL_H */
