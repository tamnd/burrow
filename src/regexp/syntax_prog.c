/* regexp/syntax: the compiler and the program it makes, from compile.go and
 * prog.go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp/syntax.h"

#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "syntax_internal.h"

#include <string.h>

/* --------------------------------------------------------------- InstOp */

static const char *const syn_inst_op_names[] = {
    "InstAlt",   "InstAltMatch", "InstCapture",      "InstEmptyWidth",
    "InstMatch", "InstFail",     "InstNop",          "InstRune",
    "InstRune1", "InstRuneAny",  "InstRuneAnyNotNL",
};

Str syntax_inst_op_string(SyntaxInstOp op) {
    if (op >= sizeof syn_inst_op_names / sizeof *syn_inst_op_names)
        return BURROW_STR_EMPTY;
    const char *s = syn_inst_op_names[op];
    return str_from_bytes((const Byte *)s, (Int)strlen(s));
}

/* -------------------------------------------------------------- EmptyOp */

bool syntax_is_word_char(Rune r) {
    return ('a' <= r && r <= 'z') || ('A' <= r && r <= 'Z') || ('0' <= r && r <= '9') ||
           r == '_';
}

SyntaxEmptyOp syntax_empty_op_context(Rune r1, Rune r2) {
    SyntaxEmptyOp op = SYNTAX_EMPTY_NO_WORD_BOUNDARY;
    int boundary = 0;
    if (syntax_is_word_char(r1))
        boundary = 1;
    else if (r1 == '\n')
        op |= SYNTAX_EMPTY_BEGIN_LINE;
    else if (r1 < 0)
        op |= SYNTAX_EMPTY_BEGIN_TEXT | SYNTAX_EMPTY_BEGIN_LINE;
    if (syntax_is_word_char(r2))
        boundary ^= 1;
    else if (r2 == '\n')
        op |= SYNTAX_EMPTY_END_LINE;
    else if (r2 < 0)
        op |= SYNTAX_EMPTY_END_TEXT | SYNTAX_EMPTY_END_LINE;
    if (boundary != 0) /* one side is a word character and the other is not */
        op ^= SYNTAX_EMPTY_WORD_BOUNDARY | SYNTAX_EMPTY_NO_WORD_BOUNDARY;
    return op;
}

/* ----------------------------------------------------------------- Inst */

Int syntax_inst_match_rune_pos(const SyntaxInst *i, Rune r) {
    const Rune *rune = (const Rune *)i->rune.p;
    Int n = i->rune.len;
    switch (n) {
    case 0:
        return -1;

    case 1: {
        /* A single rune, maybe case folded. */
        Rune r0 = rune[0];
        if (r == r0)
            return 0;
        if (i->arg & SYNTAX_FOLD_CASE) {
            for (Rune r1 = unicode_simple_fold(r0); r1 != r0;
                 r1 = unicode_simple_fold(r1))
                if (r == r1)
                    return 0;
        }
        return -1;
    }

    case 2:
        return r >= rune[0] && r <= rune[1] ? 0 : -1;

    case 4:
    case 6:
    case 8:
        /* A few ranges: a straight search is fastest. */
        for (Int j = 0; j < n; j += 2) {
            if (r < rune[j])
                return -1;
            if (r <= rune[j + 1])
                return j / 2;
        }
        return -1;

    default:
        break;
    }

    /* Otherwise a binary search. */
    Int lo = 0, hi = n / 2;
    while (lo < hi) {
        Int m = (Int)((uint64_t)(lo + hi) >> 1);
        Rune c = rune[2 * m];
        if (c <= r) {
            if (r <= rune[2 * m + 1])
                return m;
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    return -1;
}

bool syntax_inst_match_rune(const SyntaxInst *i, Rune r) {
    return syntax_inst_match_rune_pos(i, r) != -1;
}

bool syntax_inst_match_empty_width(const SyntaxInst *i, Rune before, Rune after) {
    switch (i->arg) {
    case SYNTAX_EMPTY_BEGIN_LINE:
        return before == '\n' || before == -1;
    case SYNTAX_EMPTY_END_LINE:
        return after == '\n' || after == -1;
    case SYNTAX_EMPTY_BEGIN_TEXT:
        return before == -1;
    case SYNTAX_EMPTY_END_TEXT:
        return after == -1;
    case SYNTAX_EMPTY_WORD_BOUNDARY:
        return syntax_is_word_char(before) != syntax_is_word_char(after);
    case SYNTAX_EMPTY_NO_WORD_BOUNDARY:
        return syntax_is_word_char(before) == syntax_is_word_char(after);
    default:
        break;
    }
    panic_str(BURROW_S("unknown empty width arg"));
}

/* ------------------------------------------------------------- printing */

typedef struct SynOut {
    SynScratch s;
    Byte *buf;
    Int len;
    Int cap;
    const SyntaxProg *prog;
    const SyntaxInst *inst;
} SynOut;

static void syn_out_bytes(SynOut *o, const void *b, Int n) {
    if (o->len + n > o->cap) {
        Int ncap = o->cap * 2;
        if (ncap < o->len + n)
            ncap = o->len + n;
        if (ncap < 64)
            ncap = 64;
        Byte *q = (Byte *)syn_scratch_alloc(&o->s, (size_t)ncap, 1);
        if (o->len > 0)
            memcpy(q, o->buf, (size_t)o->len);
        o->buf = q;
        o->cap = ncap;
    }
    if (n > 0)
        memcpy(o->buf + o->len, b, (size_t)n);
    o->len += n;
}

static void syn_out(SynOut *o, const char *s) {
    syn_out_bytes(o, s, (Int)strlen(s));
}

static void syn_out_uint(SynOut *o, uint64_t u) {
    char b[24];
    int i = (int)sizeof b;
    do {
        b[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    syn_out_bytes(o, b + i, (Int)sizeof b - i);
}

static void syn_out_rune(SynOut *o, Rune r) {
    Byte b[4];
    Int n = utf8_encode_rune((Slice){b, 4, 4, NULL}, r);
    syn_out_bytes(o, b, n);
}

/* strconv.QuoteToASCII(string(runes)). */
static void syn_out_quoted_runes(SynOut *o, Slice rune) {
    Int start = o->len;
    for (Int j = 0; j < rune.len; j++)
        syn_out_rune(o, ((const Rune *)rune.p)[j]);
    Str q =
        strconv_quote_to_ascii(o->s.a, str_from_bytes(o->buf + start, o->len - start));
    if (q.len == 0) {
        o->s.oom = true;
        panic_str(BURROW_S("regexp/syntax: out of memory"));
    }
    o->len = start;
    syn_out_bytes(o, q.p, q.len);
}

/* dumpInst. */
static void syn_dump_inst(SynOut *o, const SyntaxInst *i) {
    switch (i->op) {
    case SYNTAX_INST_ALT:
        syn_out(o, "alt -> ");
        syn_out_uint(o, i->out);
        syn_out(o, ", ");
        syn_out_uint(o, i->arg);
        break;
    case SYNTAX_INST_ALT_MATCH:
        syn_out(o, "altmatch -> ");
        syn_out_uint(o, i->out);
        syn_out(o, ", ");
        syn_out_uint(o, i->arg);
        break;
    case SYNTAX_INST_CAPTURE:
        syn_out(o, "cap ");
        syn_out_uint(o, i->arg);
        syn_out(o, " -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_EMPTY_WIDTH:
        syn_out(o, "empty ");
        syn_out_uint(o, i->arg);
        syn_out(o, " -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_MATCH:
        syn_out(o, "match");
        break;
    case SYNTAX_INST_FAIL:
        syn_out(o, "fail");
        break;
    case SYNTAX_INST_NOP:
        syn_out(o, "nop -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_RUNE:
        /* Go writes both of these for a nil slice. */
        if (i->rune.p == NULL)
            syn_out(o, "rune <nil>");
        syn_out(o, "rune ");
        syn_out_quoted_runes(o, i->rune);
        if (i->arg & SYNTAX_FOLD_CASE)
            syn_out(o, "/i");
        syn_out(o, " -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_RUNE1:
        syn_out(o, "rune1 ");
        syn_out_quoted_runes(o, i->rune);
        syn_out(o, " -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_RUNE_ANY:
        syn_out(o, "any -> ");
        syn_out_uint(o, i->out);
        break;
    case SYNTAX_INST_RUNE_ANY_NOT_NL:
        syn_out(o, "anynotnl -> ");
        syn_out_uint(o, i->out);
        break;
    default:
        break;
    }
}

/* dumpProg. */
static void syn_dump_prog(void *arg) {
    SynOut *o = (SynOut *)arg;
    const SyntaxProg *p = o->prog;
    const SyntaxInst *inst = (const SyntaxInst *)p->inst.p;
    for (Int j = 0; j < p->inst.len; j++) {
        Int w = o->len;
        syn_out_uint(o, (uint64_t)j);
        Int digits = o->len - w;
        if (digits < 3) {
            /* Pad on the left to three places. */
            Byte num[3];
            memcpy(num, o->buf + w, (size_t)digits);
            o->len = w;
            syn_out_bytes(o, "   ", 3 - digits);
            syn_out_bytes(o, num, digits);
        }
        if (j == p->start)
            syn_out(o, "*");
        syn_out(o, "\t");
        syn_dump_inst(o, &inst[j]);
        syn_out(o, "\n");
    }
}

static void syn_dump_one(void *arg) {
    SynOut *o = (SynOut *)arg;
    syn_dump_inst(o, o->inst);
}

static Str syn_out_run(SynOut *o, Alloc *a, void (*fn)(void *)) {
    syn_scratch_init(&o->s, a);
    Str out = BURROW_STR_EMPTY;
    if (syn_guard(&o->s, fn, o) && o->len > 0) {
        Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)o->len, 1);
        if (b != NULL) {
            memcpy(b, o->buf, (size_t)o->len);
            out = str_from_bytes(b, o->len);
        }
    }
    syn_scratch_free(&o->s);
    return out;
}

Str syntax_inst_string(const SyntaxInst *i, Alloc *a) {
    SynOut o;
    memset(&o, 0, sizeof o);
    o.inst = i;
    return syn_out_run(&o, a, syn_dump_one);
}

Str syntax_prog_string(const SyntaxProg *p, Alloc *a) {
    SynOut o;
    memset(&o, 0, sizeof o);
    o.prog = p;
    return syn_out_run(&o, a, syn_dump_prog);
}

/* ----------------------------------------------------------------- Prog */

/* skipNop: the first instruction from pc on that is not a nop or a
 * capture. */
static const SyntaxInst *syn_skip_nop(const SyntaxProg *p, uint32_t pc) {
    const SyntaxInst *inst = (const SyntaxInst *)p->inst.p;
    const SyntaxInst *i = &inst[pc];
    while (i->op == SYNTAX_INST_NOP || i->op == SYNTAX_INST_CAPTURE)
        i = &inst[i->out];
    return i;
}

/* op: the op, with all the rune instructions as SYNTAX_INST_RUNE. */
static SyntaxInstOp syn_inst_op(const SyntaxInst *i) {
    switch (i->op) {
    case SYNTAX_INST_RUNE1:
    case SYNTAX_INST_RUNE_ANY:
    case SYNTAX_INST_RUNE_ANY_NOT_NL:
        return SYNTAX_INST_RUNE;
    default:
        return i->op;
    }
}

Str syntax_prog_prefix(const SyntaxProg *p, Alloc *a, bool *complete) {
    const SyntaxInst *i = syn_skip_nop(p, (uint32_t)p->start);

    /* Not starting with a single rune means no prefix. */
    if (syn_inst_op(i) != SYNTAX_INST_RUNE || i->rune.len != 1) {
        if (complete != NULL)
            *complete = i->op == SYNTAX_INST_MATCH;
        return BURROW_STR_EMPTY;
    }

    /* Measure first, then write. */
    Int n = 0;
    const SyntaxInst *j = i;
    while (syn_inst_op(j) == SYNTAX_INST_RUNE && j->rune.len == 1 &&
           (j->arg & SYNTAX_FOLD_CASE) == 0 &&
           ((const Rune *)j->rune.p)[0] != UTF8_RUNE_ERROR) {
        n += utf8_rune_len(((const Rune *)j->rune.p)[0]) < 0
                 ? 3
                 : utf8_rune_len(((const Rune *)j->rune.p)[0]);
        j = syn_skip_nop(p, j->out);
    }
    if (complete != NULL)
        *complete = j->op == SYNTAX_INST_MATCH;
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    Int w = 0;
    for (j = i; w < n; j = syn_skip_nop(p, j->out))
        w += utf8_encode_rune((Slice){b + w, n - w, n - w, NULL},
                              ((const Rune *)j->rune.p)[0]);
    return str_from_bytes(b, n);
}

SyntaxEmptyOp syntax_prog_start_cond(const SyntaxProg *p) {
    SyntaxEmptyOp flag = 0;
    const SyntaxInst *inst = (const SyntaxInst *)p->inst.p;
    const SyntaxInst *i = &inst[p->start];
    for (;;) {
        switch (i->op) {
        case SYNTAX_INST_EMPTY_WIDTH:
            flag |= (SyntaxEmptyOp)i->arg;
            break;
        case SYNTAX_INST_FAIL:
            return (SyntaxEmptyOp)~0;
        case SYNTAX_INST_CAPTURE:
        case SYNTAX_INST_NOP:
            /* Skip it. */
            break;
        default:
            return flag;
        }
        i = &inst[i->out];
    }
}

/* ------------------------------------------------------------- compiling */

/* A list of the out and arg fields still to be filled in, threaded through
 * the fields themselves. An entry n is inst n>>1's out when n&1 is 0 and its
 * arg when it is 1. Instruction 0 is always the fail instruction, so 0 can
 * mean the end of the list. */
typedef struct SynPatchList {
    uint32_t head, tail;
} SynPatchList;

typedef struct SynFrag {
    uint32_t i;       /* the first instruction */
    SynPatchList out; /* where to point at what comes next */
    bool nullable;    /* whether it can match the empty string */
} SynFrag;

typedef struct SynCompiler {
    SynScratch s;
    SyntaxInst *inst;
    Int n;
    Int cap;
    Int num_cap;
    const SyntaxRegexp *re;
    SynFrag f;
} SynCompiler;

static const Rune syn_any_rune_not_nl[] = {0, '\n' - 1, '\n' + 1, UNICODE_MAX_RUNE};
static const Rune syn_any_rune[] = {0, UNICODE_MAX_RUNE};

static void syn_patch(SynCompiler *c, SynPatchList l, uint32_t val) {
    uint32_t head = l.head;
    while (head != 0) {
        SyntaxInst *i = &c->inst[head >> 1];
        if ((head & 1) == 0) {
            head = i->out;
            i->out = val;
        } else {
            head = i->arg;
            i->arg = val;
        }
    }
}

static SynPatchList syn_patch_append(SynCompiler *c, SynPatchList l1, SynPatchList l2) {
    if (l1.head == 0)
        return l2;
    if (l2.head == 0)
        return l1;
    SyntaxInst *i = &c->inst[l1.tail >> 1];
    if ((l1.tail & 1) == 0)
        i->out = l2.head;
    else
        i->arg = l2.head;
    return (SynPatchList){l1.head, l2.tail};
}

static SynPatchList syn_make_patch(uint32_t n) {
    return (SynPatchList){n, n};
}

static SynFrag syn_inst(SynCompiler *c, SyntaxInstOp op) {
    if (c->n == c->cap) {
        Int ncap = c->cap < 16 ? 16 : c->cap * 2;
        SyntaxInst *q = (SyntaxInst *)syn_scratch_alloc(&c->s, (size_t)ncap * sizeof *q,
                                                        _Alignof(SyntaxInst));
        if (c->n > 0)
            memcpy(q, c->inst, (size_t)c->n * sizeof *q);
        c->inst = q;
        c->cap = ncap;
    }
    SynFrag f = {(uint32_t)c->n, {0, 0}, true};
    memset(&c->inst[c->n], 0, sizeof c->inst[c->n]);
    c->inst[c->n].op = op;
    c->n++;
    return f;
}

static SynFrag syn_nop(SynCompiler *c) {
    SynFrag f = syn_inst(c, SYNTAX_INST_NOP);
    f.out = syn_make_patch(f.i << 1);
    return f;
}

static SynFrag syn_fail_frag(void) {
    return (SynFrag){0, {0, 0}, false};
}

static SynFrag syn_cap(SynCompiler *c, uint32_t arg) {
    SynFrag f = syn_inst(c, SYNTAX_INST_CAPTURE);
    f.out = syn_make_patch(f.i << 1);
    c->inst[f.i].arg = arg;
    if (c->num_cap < (Int)arg + 1)
        c->num_cap = (Int)arg + 1;
    return f;
}

static SynFrag syn_cat(SynCompiler *c, SynFrag f1, SynFrag f2) {
    /* Anything followed by fail is fail. */
    if (f1.i == 0 || f2.i == 0)
        return syn_fail_frag();
    syn_patch(c, f1.out, f2.i);
    return (SynFrag){f1.i, f2.out, f1.nullable && f2.nullable};
}

static SynFrag syn_alt(SynCompiler *c, SynFrag f1, SynFrag f2) {
    /* One side failing leaves the other. */
    if (f1.i == 0)
        return f2;
    if (f2.i == 0)
        return f1;
    SynFrag f = syn_inst(c, SYNTAX_INST_ALT);
    c->inst[f.i].out = f1.i;
    c->inst[f.i].arg = f2.i;
    f.out = syn_patch_append(c, f1.out, f2.out);
    f.nullable = f1.nullable || f2.nullable;
    return f;
}

static SynFrag syn_quest(SynCompiler *c, SynFrag f1, bool nongreedy) {
    SynFrag f = syn_inst(c, SYNTAX_INST_ALT);
    if (nongreedy) {
        c->inst[f.i].arg = f1.i;
        f.out = syn_make_patch(f.i << 1);
    } else {
        c->inst[f.i].out = f1.i;
        f.out = syn_make_patch(f.i << 1 | 1);
    }
    f.out = syn_patch_append(c, f.out, f1.out);
    return f;
}

/* loop: f1 any number of times, at least once, entered at f1 and left by
 * the alt that loops back. */
static SynFrag syn_loop(SynCompiler *c, SynFrag f1, bool nongreedy) {
    SynFrag f = syn_inst(c, SYNTAX_INST_ALT);
    if (nongreedy) {
        c->inst[f.i].arg = f1.i;
        f.out = syn_make_patch(f.i << 1);
    } else {
        c->inst[f.i].out = f1.i;
        f.out = syn_make_patch(f.i << 1 | 1);
    }
    syn_patch(c, f1.out, f.i);
    return f;
}

static SynFrag syn_plus(SynCompiler *c, SynFrag f1, bool nongreedy) {
    return (SynFrag){f1.i, syn_loop(c, f1, nongreedy).out, f1.nullable};
}

static SynFrag syn_star(SynCompiler *c, SynFrag f1, bool nongreedy) {
    /* When f1 can match the empty string, (f1)* as a plain loop would loop
     * forever without moving, so it becomes (f1+)?. */
    if (f1.nullable)
        return syn_quest(c, syn_plus(c, f1, nongreedy), nongreedy);
    return syn_loop(c, f1, nongreedy);
}

static SynFrag syn_empty(SynCompiler *c, SyntaxEmptyOp op) {
    SynFrag f = syn_inst(c, SYNTAX_INST_EMPTY_WIDTH);
    c->inst[f.i].arg = op;
    f.out = syn_make_patch(f.i << 1);
    return f;
}

static SynFrag syn_rune(SynCompiler *c, Slice r, SyntaxFlags flags) {
    SynFrag f = syn_inst(c, SYNTAX_INST_RUNE);
    f.nullable = false;
    SyntaxInst *i = &c->inst[f.i];
    i->rune = r;
    const Rune *q = (const Rune *)r.p;
    flags &= SYNTAX_FOLD_CASE; /* the only flag that matters here */
    if (r.len != 1 || unicode_simple_fold(q[0]) == q[0])
        flags &= (SyntaxFlags)~SYNTAX_FOLD_CASE;
    i->arg = flags;
    f.out = syn_make_patch(f.i << 1);

    /* The special cases make the matchers faster. */
    if ((flags & SYNTAX_FOLD_CASE) == 0 && (r.len == 1 || (r.len == 2 && q[0] == q[1])))
        i->op = SYNTAX_INST_RUNE1;
    else if (r.len == 2 && q[0] == 0 && q[1] == UNICODE_MAX_RUNE)
        i->op = SYNTAX_INST_RUNE_ANY;
    else if (r.len == 4 && q[0] == 0 && q[1] == '\n' - 1 && q[2] == '\n' + 1 &&
             q[3] == UNICODE_MAX_RUNE)
        i->op = SYNTAX_INST_RUNE_ANY_NOT_NL;
    return f;
}

static Slice syn_static_runes(const Rune *r, Int n) {
    return (Slice){(void *)(uintptr_t)r, n, n, TYPE_RUNE};
}

/* The fragment for a node with no subs, or false in *leaf when it has some. */
static SynFrag syn_compile_leaf(SynCompiler *c, const SyntaxRegexp *re, bool *leaf) {
    *leaf = true;
    switch (re->op) {
    case SYNTAX_OP_NO_MATCH:
        return syn_fail_frag();
    case SYNTAX_OP_EMPTY_MATCH:
        return syn_nop(c);
    case SYNTAX_OP_LITERAL: {
        if (re->rune.len == 0)
            return syn_nop(c);
        SynFrag f = syn_fail_frag();
        for (Int j = 0; j < re->rune.len; j++) {
            Slice one = {SYN_RUNES(re->rune) + j, 1, re->rune.cap - j, TYPE_RUNE};
            SynFrag f1 = syn_rune(c, one, re->flags);
            f = j == 0 ? f1 : syn_cat(c, f, f1);
        }
        return f;
    }
    case SYNTAX_OP_CHAR_CLASS:
        return syn_rune(c, re->rune, re->flags);
    case SYNTAX_OP_ANY_CHAR_NOT_NL:
        return syn_rune(c, syn_static_runes(syn_any_rune_not_nl, 4), 0);
    case SYNTAX_OP_ANY_CHAR:
        return syn_rune(c, syn_static_runes(syn_any_rune, 2), 0);
    case SYNTAX_OP_BEGIN_LINE:
        return syn_empty(c, SYNTAX_EMPTY_BEGIN_LINE);
    case SYNTAX_OP_END_LINE:
        return syn_empty(c, SYNTAX_EMPTY_END_LINE);
    case SYNTAX_OP_BEGIN_TEXT:
        return syn_empty(c, SYNTAX_EMPTY_BEGIN_TEXT);
    case SYNTAX_OP_END_TEXT:
        return syn_empty(c, SYNTAX_EMPTY_END_TEXT);
    case SYNTAX_OP_WORD_BOUNDARY:
        return syn_empty(c, SYNTAX_EMPTY_WORD_BOUNDARY);
    case SYNTAX_OP_NO_WORD_BOUNDARY:
        return syn_empty(c, SYNTAX_EMPTY_NO_WORD_BOUNDARY);
    case SYNTAX_OP_CONCAT:
        if (re->sub.len == 0)
            return syn_nop(c);
        break;
    case SYNTAX_OP_ALTERNATE:
        if (re->sub.len == 0)
            return syn_fail_frag();
        break;
    case SYNTAX_OP_CAPTURE:
    case SYNTAX_OP_STAR:
    case SYNTAX_OP_PLUS:
    case SYNTAX_OP_QUEST:
        break;
    default:
        panic_str(BURROW_S("regexp: unhandled case in compile"));
    }
    *leaf = false;
    return syn_fail_frag();
}

/* A node being compiled: the next sub to do, and what the ones before it
 * came to. */
typedef struct SynCompileFrame {
    const SyntaxRegexp *re;
    Int next;
    SynFrag acc;
} SynCompileFrame;

/* compile, with its own stack in place of recursion. The instructions come
 * out in the same order as Go's, which the program listings depend on. */
static SynFrag syn_compile_re(SynCompiler *c, const SyntaxRegexp *root) {
    SynStack k = {NULL, 0, 0, sizeof(SynCompileFrame)};
    SynCompileFrame *t = (SynCompileFrame *)syn_stack_push(&c->s, &k);
    t->re = root;
    SynFrag ret = syn_fail_frag();
    bool back = false; /* whether ret holds the fragment for t's last sub */
    while ((t = (SynCompileFrame *)syn_stack_top(&k)) != NULL) {
        const SyntaxRegexp *re = t->re;
        SyntaxRegexp *const *sub = SYN_SUBS(re->sub);
        bool ng = (re->flags & SYNTAX_NON_GREEDY) != 0;
        if (!back) {
            bool leaf;
            ret = syn_compile_leaf(c, re, &leaf);
            if (leaf) {
                k.len--;
                back = true;
                continue;
            }
            if (re->op == SYNTAX_OP_CAPTURE)
                t->acc = syn_cap(c, (uint32_t)(re->cap << 1));
            t->next = 1;
            SynCompileFrame *u = (SynCompileFrame *)syn_stack_push(&c->s, &k);
            u->re = sub[0];
            continue;
        }
        switch (re->op) {
        case SYNTAX_OP_CAPTURE: {
            SynFrag ket = syn_cap(c, (uint32_t)(re->cap << 1 | 1));
            ret = syn_cat(c, syn_cat(c, t->acc, ret), ket);
            break;
        }
        case SYNTAX_OP_STAR:
            ret = syn_star(c, ret, ng);
            break;
        case SYNTAX_OP_PLUS:
            ret = syn_plus(c, ret, ng);
            break;
        case SYNTAX_OP_QUEST:
            ret = syn_quest(c, ret, ng);
            break;
        default:
            if (re->op == SYNTAX_OP_CONCAT)
                t->acc = t->next == 1 ? ret : syn_cat(c, t->acc, ret);
            else
                t->acc = syn_alt(c, t->next == 1 ? syn_fail_frag() : t->acc, ret);
            if (t->next < re->sub.len) {
                const SyntaxRegexp *s = sub[t->next++];
                SynCompileFrame *u = (SynCompileFrame *)syn_stack_push(&c->s, &k);
                u->re = s;
                back = false;
                continue;
            }
            ret = t->acc;
            break;
        }
        k.len--;
    }
    return ret;
}

static void syn_compile_run(void *arg) {
    SynCompiler *c = (SynCompiler *)arg;
    c->num_cap = 2; /* the ( and ) around the whole match, $0 */
    syn_inst(c, SYNTAX_INST_FAIL);
    SynFrag f = syn_compile_re(c, c->re);
    syn_patch(c, f.out, syn_inst(c, SYNTAX_INST_MATCH).i);
    c->f = f;
}

SyntaxProg *syntax_compile(Alloc *a, const SyntaxRegexp *re, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    SynCompiler c;
    memset(&c, 0, sizeof c);
    syn_scratch_init(&c.s, a);
    c.re = re;
    SyntaxProg *prog = NULL;
    if (syn_guard(&c.s, syn_compile_run, &c)) {
        /* One block: the program, the instructions, and the runes. */
        size_t nrune = 0;
        for (Int j = 0; j < c.n; j++)
            nrune += (size_t)c.inst[j].rune.len;
        size_t off_inst = SYN_BLOCK_HDR + sizeof(SyntaxProg);
        off_inst = (off_inst + _Alignof(SyntaxInst) - 1) / _Alignof(SyntaxInst) *
                   _Alignof(SyntaxInst);
        size_t off_rune = off_inst + (size_t)c.n * sizeof(SyntaxInst);
        size_t size = off_rune + nrune * sizeof(Rune);
        Byte *block = (Byte *)mem_alloc_nozero(a, size, SYN_BLOCK_ALIGN);
        if (block != NULL) {
            SynBlock *hdr = (SynBlock *)(void *)block;
            hdr->a = a;
            hdr->size = size;
            prog = (SyntaxProg *)(void *)(block + SYN_BLOCK_HDR);
            SyntaxInst *inst = (SyntaxInst *)(void *)(block + off_inst);
            Rune *runes = (Rune *)(void *)(block + off_rune);
            for (Int j = 0; j < c.n; j++) {
                inst[j] = c.inst[j];
                Slice r = c.inst[j].rune;
                if (r.p == NULL) {
                    inst[j].rune = (Slice){0};
                    continue;
                }
                if (r.len > 0)
                    memcpy(runes, r.p, (size_t)r.len * sizeof(Rune));
                inst[j].rune = (Slice){runes, r.len, r.len, TYPE_RUNE};
                runes += r.len;
            }
            prog->inst = (Slice){inst, c.n, c.n, NULL};
            prog->start = (Int)c.f.i;
            prog->num_cap = c.num_cap;
        }
    }
    syn_scratch_free(&c.s);
    if (prog == NULL && err != NULL)
        *err = burrow_err_out_of_memory;
    return prog;
}

void syntax_prog_free(SyntaxProg *p) {
    if (p == NULL)
        return;
    SynBlock *hdr = (SynBlock *)(void *)((Byte *)p - SYN_BLOCK_HDR);
    mem_free(hdr->a, hdr, hdr->size, SYN_BLOCK_ALIGN);
}
