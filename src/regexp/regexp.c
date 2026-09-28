/* regexp: compiling, and the Find, Replace and Split families on top of the
 * matchers in exec.c, backtrack.c and onepass.c. From regexp.go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/regexp.h"

#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "regexp_internal.h"
#include "syntax_internal.h"

#include <string.h>

/* ---------------------------------------------------------------- the types */

/* []int */
static const Type rx_ints_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_INT,
    NULL,
    0,
    0x72786931U, /* "rxi1" */
    NULL,
};

/* []string */
static const Type rx_strs_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_STRING,
    NULL,
    0,
    0x72787331U, /* "rxs1" */
    NULL,
};

/* [][]byte */
static const Type rx_bytess_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTES,
    NULL,
    0,
    0x72786232U, /* "rxb2" */
    NULL,
};

/* [][]int */
static const Type rx_intss_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &rx_ints_desc,
    NULL,
    0,
    0x72786932U, /* "rxi2" */
    NULL,
};

/* [][]string */
static const Type rx_strss_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &rx_strs_desc,
    NULL,
    0,
    0x72787332U, /* "rxs2" */
    NULL,
};

/* [][][]byte */
static const Type rx_bytesss_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &rx_bytess_desc,
    NULL,
    0,
    0x72786233U, /* "rxb3" */
    NULL,
};

/* ---------------------------------------------------------------- compiling */

typedef struct RxLenFrame {
    const SyntaxRegexp *re;
    Int i;
    Int acc;
} RxLenFrame;

/* minInputLen: the fewest bytes a match can take. Go's walks the tree by
 * recursion, and this with its own stack from a. -1 when a runs out. */
static Int rx_min_input_len(Alloc *a, const SyntaxRegexp *root) {
    Int cap = 16, n = 0, r = 0;
    RxLenFrame *stk = (RxLenFrame *)mem_alloc(a, (size_t)cap * sizeof(RxLenFrame),
                                              _Alignof(RxLenFrame));
    if (stk == NULL)
        return -1;
    stk[n].re = root;
    stk[n].i = 0;
    stk[n].acc = 0;
    n++;
    while (n > 0) {
        RxLenFrame *f = &stk[n - 1];
        const SyntaxRegexp *re = f->re;
        const SyntaxRegexp *const *sub = (const SyntaxRegexp *const *)re->sub.p;
        const SyntaxRegexp *child = NULL;
        switch (re->op) {
        default:
            r = 0;
            break;
        case SYNTAX_OP_ANY_CHAR:
        case SYNTAX_OP_ANY_CHAR_NOT_NL:
        case SYNTAX_OP_CHAR_CLASS:
            r = 1;
            break;
        case SYNTAX_OP_LITERAL: {
            const Rune *rs = (const Rune *)re->rune.p;
            r = 0;
            for (Int k = 0; k < re->rune.len; k++)
                r += rs[k] == UTF8_RUNE_ERROR ? 1 : utf8_rune_len(rs[k]);
            break;
        }
        case SYNTAX_OP_CAPTURE:
        case SYNTAX_OP_PLUS:
        case SYNTAX_OP_REPEAT:
            if (f->i == 0) {
                f->i = 1;
                child = sub[0];
                break;
            }
            if (re->op == SYNTAX_OP_REPEAT)
                r = re->min * r;
            break;
        case SYNTAX_OP_CONCAT:
            if (f->i > 0)
                f->acc += r;
            if (f->i < re->sub.len) {
                child = sub[f->i++];
                break;
            }
            r = f->acc;
            break;
        case SYNTAX_OP_ALTERNATE:
            if (f->i == 1 || (f->i > 1 && r < f->acc))
                f->acc = r;
            if (f->i < re->sub.len) {
                child = sub[f->i++];
                break;
            }
            r = f->acc;
            break;
        }
        if (child == NULL) {
            n--;
            continue;
        }
        if (n == cap) {
            RxLenFrame *g = (RxLenFrame *)mem_realloc(
                a, stk, (size_t)cap * sizeof(RxLenFrame),
                (size_t)(2 * cap) * sizeof(RxLenFrame), _Alignof(RxLenFrame));
            if (g == NULL) {
                mem_free(a, stk, (size_t)cap * sizeof(RxLenFrame),
                         _Alignof(RxLenFrame));
                return -1;
            }
            stk = g;
            cap *= 2;
        }
        stk[n].re = child;
        stk[n].i = 0;
        stk[n].acc = 0;
        n++;
    }
    mem_free(a, stk, (size_t)cap * sizeof(RxLenFrame), _Alignof(RxLenFrame));
    return r;
}

static void rx_prog_free(RxProg *p) {
    if (p == NULL)
        return;
    rx_free_machines(p);
    rx_free_bitstates(p);
    arena_free(&p->arena);
    mem_free(p->parent, p, sizeof(RxProg), _Alignof(RxProg));
}

static RxProg *rx_prog_oom(RxProg *p, SyntaxRegexp *re, Error *err) {
    syntax_regexp_free(re);
    rx_prog_free(p);
    BURROW_OUT(err, burrow_err_out_of_memory);
    return NULL;
}

/* compile, less the Regexp around it. */
/* Whether Prog.Prefix has something to give, to tell an empty one from the
 * allocator running out. */
static bool rx_has_prefix(const SyntaxProg *prog) {
    const SyntaxInst *insts = (const SyntaxInst *)prog->inst.p;
    const SyntaxInst *i = &insts[prog->start];
    while (i->op == SYNTAX_INST_NOP)
        i = &insts[i->out];
    SyntaxInstOp op = i->op;
    if (op == SYNTAX_INST_RUNE1 || op == SYNTAX_INST_RUNE_ANY ||
        op == SYNTAX_INST_RUNE_ANY_NOT_NL)
        op = SYNTAX_INST_RUNE;
    return op == SYNTAX_INST_RUNE && i->rune.len == 1 &&
           ((SyntaxFlags)i->arg & SYNTAX_FOLD_CASE) == 0 &&
           ((const Rune *)i->rune.p)[0] != UTF8_RUNE_ERROR;
}

static RxProg *rx_prog_new(Alloc *a, Str expr, SyntaxFlags mode, Error *err) {
    SyntaxRegexp *re = syntax_parse(a, expr, mode, err);
    if (re == NULL)
        return NULL;
    RxProg *p = (RxProg *)mem_alloc(a, sizeof(RxProg), _Alignof(RxProg));
    if (p == NULL)
        return rx_prog_oom(NULL, re, err);
    p->parent = a;
    arena_init(&p->arena, a, SYN_ARENA_CHUNK);
    Alloc *pa = arena_allocator(&p->arena);
    p->mode = mode;
    p->expr = str_clone(pa, expr);
    if (p->expr.len != expr.len)
        return rx_prog_oom(p, re, err);

    p->num_subexp = syntax_regexp_max_cap(re);
    p->subexp_names = syntax_regexp_cap_names(re, pa);
    if (p->subexp_names.p == NULL)
        return rx_prog_oom(p, re, err);
    Str *names = (Str *)p->subexp_names.p;
    for (Int k = 0; k < p->subexp_names.len; k++) {
        Str name = str_clone(pa, names[k]);
        if (name.len != names[k].len)
            return rx_prog_oom(p, re, err);
        names[k] = name;
    }

    SyntaxRegexp *s = syntax_regexp_simplify(re, a);
    syntax_regexp_free(re);
    if (s == NULL)
        return rx_prog_oom(p, NULL, err);
    SyntaxProg *prog = syntax_compile(pa, s, NULL);
    p->min_input_len = rx_min_input_len(a, s);
    syntax_regexp_free(s);
    if (prog == NULL || p->min_input_len < 0)
        return rx_prog_oom(p, NULL, err);
    p->prog = prog;
    p->inst = (const SyntaxInst *)prog->inst.p;
    p->ninst = prog->inst.len;
    p->matchcap = prog->num_cap < 2 ? 2 : prog->num_cap;
    p->cond = syntax_prog_start_cond(prog);

    bool oom = false;
    p->onepass = rx_compile_onepass(pa, prog, &oom);
    if (oom)
        return rx_prog_oom(p, NULL, err);
    if (p->onepass == NULL) {
        p->prefix = syntax_prog_prefix(prog, pa, &p->prefix_complete);
        if (p->prefix.len == 0 && rx_has_prefix(prog))
            return rx_prog_oom(p, NULL, err);
        p->max_bitstate_len = rx_max_bitstate_len(prog);
    } else {
        p->prefix =
            rx_onepass_prefix(pa, prog, &p->prefix_complete, &p->prefix_end, &oom);
        if (oom)
            return rx_prog_oom(p, NULL, err);
    }
    if (p->prefix.len > 0) {
        Int w;
        p->prefix_rune = utf8_decode_rune_in_string(p->prefix, &w);
    }
    return p;
}

Regexp *rx_compile(Alloc *a, Str expr, SyntaxFlags mode, bool longest, Error *err) {
    Regexp *re = (Regexp *)mem_alloc(a, sizeof(Regexp), _Alignof(Regexp));
    if (re == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    re->p = rx_prog_new(a, expr, mode, err);
    if (re->p == NULL) {
        mem_free(a, re, sizeof(Regexp), _Alignof(Regexp));
        return NULL;
    }
    re->a = a;
    re->longest = longest;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return re;
}

Regexp *regexp_compile(Alloc *a, Str expr, Error *err) {
    return rx_compile(a, expr, SYNTAX_PERL, false, err);
}

Regexp *regexp_compile_posix(Alloc *a, Str expr, Error *err) {
    return rx_compile(a, expr, SYNTAX_POSIX, true, err);
}

/* MustCompile's panic, `regexp: Compile(` + quote(str) + `): ` + err. */
BURROW_NORETURN static void rx_must_panic(Str fn, Str str, Error err) {
    Alloc *h = heap_allocator();
    StringsBuilder b = STRINGS_BUILDER(h);
    strings_builder_write_string(&b, BURROW_S("regexp: "), NULL);
    strings_builder_write_string(&b, fn, NULL);
    strings_builder_write_byte(&b, '(');
    if (strconv_can_backquote(str)) {
        strings_builder_write_byte(&b, '`');
        strings_builder_write_string(&b, str, NULL);
        strings_builder_write_byte(&b, '`');
    } else {
        strings_builder_write_string(&b, strconv_quote(h, str), NULL);
    }
    strings_builder_write_string(&b, BURROW_S("): "), NULL);
    strings_builder_write_string(&b, error_text(err), NULL);
    panic_str(strings_builder_string(&b));
}

Regexp *regexp_must_compile(Alloc *a, Str str) {
    Error err;
    Regexp *re = regexp_compile(a, str, &err);
    if (re == NULL && !errors_is(err, burrow_err_out_of_memory))
        rx_must_panic(BURROW_S("Compile"), str, err);
    return re;
}

Regexp *regexp_must_compile_posix(Alloc *a, Str str) {
    Error err;
    Regexp *re = regexp_compile_posix(a, str, &err);
    if (re == NULL && !errors_is(err, burrow_err_out_of_memory))
        rx_must_panic(BURROW_S("CompilePOSIX"), str, err);
    return re;
}

void regexp_free(Regexp *re) {
    if (re == NULL)
        return;
    rx_prog_free(re->p);
    mem_free(re->a, re, sizeof(Regexp), _Alignof(Regexp));
}

Regexp *regexp_copy(const Regexp *re, Alloc *a) {
    Regexp *c = rx_compile(a, re->p->expr, re->p->mode, re->longest, NULL);
    return c;
}

void regexp_longest(Regexp *re) {
    re->longest = true;
}

/* --------------------------------------------------------------- inspecting */

Str regexp_string(const Regexp *re) {
    return re->p->expr;
}

Int regexp_num_subexp(const Regexp *re) {
    return re->p->num_subexp;
}

Slice regexp_subexp_names(const Regexp *re) {
    return re->p->subexp_names;
}

Int regexp_subexp_index(const Regexp *re, Str name) {
    if (name.len != 0) {
        const Str *names = (const Str *)re->p->subexp_names.p;
        for (Int i = 0; i < re->p->subexp_names.len; i++)
            if (str_eq(name, names[i]))
                return i;
    }
    return -1;
}

Str regexp_literal_prefix(const Regexp *re, bool *complete) {
    if (complete != NULL)
        *complete = re->p->prefix_complete;
    return re->p->prefix;
}

/* special, as a table: a load a byte instead of a chain of compares. */
static const bool rx_special[256] = {
    ['\\'] = true, ['.'] = true, ['+'] = true, ['*'] = true, ['?'] = true,
    ['('] = true,  [')'] = true, ['|'] = true, ['['] = true, [']'] = true,
    ['{'] = true,  ['}'] = true, ['^'] = true, ['$'] = true,
};

Str regexp_quote_meta(Alloc *a, Str s) {
    /* A byte loop is correct because all metacharacters are ASCII. */
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += rx_special[s.p[i]];
    /* No meta characters found, so return original string. */
    if (n == 0)
        return s;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)(s.len + n), 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        if (rx_special[s.p[i]])
            b[j++] = '\\';
        b[j++] = s.p[i];
    }
    return str_from_bytes(b, j);
}

/* ----------------------------------------------------------------- matching */

static RxInput rx_bytes(const Byte *p, Int len) {
    RxInput in = {p, len, NULL, false, 0};
    return in;
}

static RxInput rx_reader(const IoRuneReader *r) {
    RxInput in = {NULL, 0, r, false, 0};
    return in;
}

static const Byte *rx_bp(Slice b) {
    return (const Byte *)b.p;
}

/* The length check is rx_find's too, done here first because most of what
 * a short input that cannot match costs is getting to it. */
bool regexp_match(const Regexp *re, Slice b) {
    if (b.len < re->p->min_input_len)
        return false;
    RxInput in = rx_bytes(rx_bp(b), b.len);
    return rx_find(re, &in, 0, 0, NULL);
}

bool regexp_match_string(const Regexp *re, Str s) {
    if (s.len < re->p->min_input_len)
        return false;
    RxInput in = rx_bytes(s.p, s.len);
    return rx_find(re, &in, 0, 0, NULL);
}

bool regexp_match_reader(const Regexp *re, IoRuneReader r) {
    RxInput in = rx_reader(&r);
    return rx_find(re, &in, 0, 0, NULL);
}

bool regexp_match_pattern(Alloc *a, Str pattern, Slice b, Error *err) {
    Regexp *re = regexp_compile(a, pattern, err);
    if (re == NULL)
        return false;
    bool ok = regexp_match(re, b);
    regexp_free(re);
    return ok;
}

bool regexp_match_string_pattern(Alloc *a, Str pattern, Str s, Error *err) {
    Regexp *re = regexp_compile(a, pattern, err);
    if (re == NULL)
        return false;
    bool ok = regexp_match_string(re, s);
    regexp_free(re);
    return ok;
}

bool regexp_match_reader_pattern(Alloc *a, Str pattern, IoRuneReader r, Error *err) {
    Regexp *re = regexp_compile(a, pattern, err);
    if (re == NULL)
        return false;
    bool ok = regexp_match_reader(re, r);
    regexp_free(re);
    return ok;
}

/* ------------------------------------------------------------------ finding */

/* Room for the capture positions of one search: on the stack when there are
 * few groups, which is nearly always, and from the heap when not. */
enum { RX_CAPS_INLINE = 32 };

typedef struct RxCaps {
    Int *m;
    Int n; /* (1 + num_subexp) * 2, the padded length */
    Int buf[RX_CAPS_INLINE];
} RxCaps;

static bool rx_caps_init(RxCaps *c, const Regexp *re) {
    c->n = (1 + re->p->num_subexp) * 2;
    if (c->n <= RX_CAPS_INLINE) {
        c->m = c->buf;
        return true;
    }
    c->m =
        (Int *)mem_alloc(heap_allocator(), (size_t)c->n * sizeof(Int), _Alignof(Int));
    return c->m != NULL;
}

static void rx_caps_free(RxCaps *c) {
    if (c->m != c->buf && c->m != NULL)
        mem_free(heap_allocator(), c->m, (size_t)c->n * sizeof(Int), _Alignof(Int));
}

/* pad: the positions past ncap are groups the program has no slots for. */
static void rx_pad(RxCaps *c, Int ncap) {
    for (Int k = ncap; k < c->n; k++)
        c->m[k] = -1;
}

static Slice rx_ints(Alloc *a, const Int *m, Int n) {
    Slice s = slice_make(a, &rx_ints_desc, n, n);
    if (s.p != NULL && n > 0)
        memcpy(s.p, m, (size_t)n * sizeof(Int));
    return s;
}

/* b[lo:hi:hi], as a []byte. */
static Slice rx_sub(Slice b, Int lo, Int hi) {
    if (b.elem == NULL)
        b.elem = TYPE_BYTE;
    return slice_sub3(b, lo, hi, hi);
}

Slice regexp_find(const Regexp *re, Slice b) {
    RxInput in = rx_bytes(rx_bp(b), b.len);
    Int m[2];
    if (!rx_find(re, &in, 0, 2, m))
        return slice_nil(TYPE_BYTE);
    return rx_sub(b, m[0], m[1]);
}

Str regexp_find_string(const Regexp *re, Str s) {
    RxInput in = rx_bytes(s.p, s.len);
    Int m[2];
    if (!rx_find(re, &in, 0, 2, m))
        return BURROW_STR_EMPTY;
    return str_from_bytes(rx_at(s.p, m[0]), m[1] - m[0]);
}

static Slice rx_find_index(const Regexp *re, Alloc *a, RxInput *in) {
    Int m[2];
    if (!rx_find(re, in, 0, 2, m))
        return slice_nil(&rx_ints_desc);
    return rx_ints(a, m, 2);
}

Slice regexp_find_index(const Regexp *re, Alloc *a, Slice b) {
    RxInput in = rx_bytes(rx_bp(b), b.len);
    return rx_find_index(re, a, &in);
}

Slice regexp_find_string_index(const Regexp *re, Alloc *a, Str s) {
    RxInput in = rx_bytes(s.p, s.len);
    return rx_find_index(re, a, &in);
}

Slice regexp_find_reader_index(const Regexp *re, Alloc *a, IoRuneReader r) {
    RxInput in = rx_reader(&r);
    return rx_find_index(re, a, &in);
}

/* find with prog.NumCap slots, padded: FindSubmatchIndex without the copy.
 * False for no match, or when the heap will not give room for the slots. */
static bool rx_find_submatch(const Regexp *re, RxInput *in, RxCaps *c) {
    if (!rx_caps_init(c, re))
        return false;
    Int ncap = re->p->prog->num_cap;
    if (!rx_find(re, in, 0, ncap, c->m)) {
        rx_caps_free(c);
        return false;
    }
    rx_pad(c, ncap);
    return true;
}

/* The [][]byte FindSubmatch gives for the positions in m. */
static Slice rx_bytes_submatch(Alloc *a, Slice b, const Int *m, Int n) {
    Slice sub = slice_make(a, &rx_bytess_desc, n / 2, n / 2);
    if (sub.p == NULL)
        return sub;
    Slice *out = (Slice *)sub.p;
    for (Int i = 0; i < n / 2; i++)
        out[i] =
            m[2 * i] >= 0 ? rx_sub(b, m[2 * i], m[2 * i + 1]) : slice_nil(TYPE_BYTE);
    return sub;
}

/* The []string FindStringSubmatch gives. */
static Slice rx_str_submatch(Alloc *a, Str s, const Int *m, Int n) {
    Slice sub = slice_make(a, &rx_strs_desc, n / 2, n / 2);
    if (sub.p == NULL)
        return sub;
    Str *out = (Str *)sub.p;
    for (Int i = 0; i < n / 2; i++)
        out[i] = m[2 * i] >= 0
                     ? str_from_bytes(rx_at(s.p, m[2 * i]), m[2 * i + 1] - m[2 * i])
                     : BURROW_STR_EMPTY;
    return sub;
}

Slice regexp_find_submatch(const Regexp *re, Alloc *a, Slice b) {
    RxInput in = rx_bytes(rx_bp(b), b.len);
    RxCaps c;
    if (!rx_find_submatch(re, &in, &c))
        return slice_nil(&rx_bytess_desc);
    Slice sub = rx_bytes_submatch(a, b, c.m, c.n);
    rx_caps_free(&c);
    return sub;
}

Slice regexp_find_string_submatch(const Regexp *re, Alloc *a, Str s) {
    RxInput in = rx_bytes(s.p, s.len);
    RxCaps c;
    if (!rx_find_submatch(re, &in, &c))
        return slice_nil(&rx_strs_desc);
    Slice sub = rx_str_submatch(a, s, c.m, c.n);
    rx_caps_free(&c);
    return sub;
}

static Slice rx_find_submatch_index(const Regexp *re, Alloc *a, RxInput *in) {
    RxCaps c;
    if (!rx_find_submatch(re, in, &c))
        return slice_nil(&rx_ints_desc);
    Slice sub = rx_ints(a, c.m, c.n);
    rx_caps_free(&c);
    return sub;
}

Slice regexp_find_submatch_index(const Regexp *re, Alloc *a, Slice b) {
    RxInput in = rx_bytes(rx_bp(b), b.len);
    return rx_find_submatch_index(re, a, &in);
}

Slice regexp_find_string_submatch_index(const Regexp *re, Alloc *a, Str s) {
    RxInput in = rx_bytes(s.p, s.len);
    return rx_find_submatch_index(re, a, &in);
}

Slice regexp_find_reader_submatch_index(const Regexp *re, Alloc *a, IoRuneReader r) {
    RxInput in = rx_reader(&r);
    return rx_find_submatch_index(re, a, &in);
}

/* --------------------------------------------------------------- find all */

/* matches: the successive matches, as an iterator you pull from. */
typedef struct RxMatches {
    const Regexp *re;
    RxInput in;
    Int end;
    Int pos;
    Int prev_match_end;
    Int max;
    Int ncap;
    RxCaps c;
} RxMatches;

static bool rx_matches_init(RxMatches *it, const Regexp *re, const Byte *p, Int len,
                            Int max, Int ncap) {
    it->re = re;
    it->in = rx_bytes(p, len);
    it->end = len;
    it->pos = 0;
    it->prev_match_end = -1;
    it->max = max;
    it->ncap = ncap;
    return rx_caps_init(&it->c, re);
}

/* The next match, padded, in it->c.m. */
static bool rx_matches_next(RxMatches *it) {
    if (it->max == 0)
        return false;
    while (it->pos <= it->end) {
        Int *m = it->c.m;
        if (!rx_find(it->re, &it->in, it->pos, it->ncap, m)) {
            it->pos = it->end + 1;
            return false;
        }
        rx_pad(&it->c, it->ncap);
        bool accept = true;
        if (m[1] == it->pos) {
            /* We've found an empty match. */
            if (m[0] == it->prev_match_end) {
                /* We don't allow an empty match right after a previous
                 * match, so ignore it. */
                accept = false;
            }
            Int width;
            rx_step(&it->in, it->pos, &width);
            if (width > 0)
                it->pos += width;
            else
                it->pos = it->end + 1;
        } else {
            it->pos = m[1];
        }
        it->prev_match_end = m[1];
        if (accept) {
            if (it->max > 0)
                it->max--;
            return true;
        }
    }
    return false;
}

typedef enum RxAllKind {
    RX_ALL,
    RX_ALL_INDEX,
    RX_ALL_SUBMATCH,
    RX_ALL_SUBMATCH_INDEX,
} RxAllKind;

/* Frees what one element of a FindAll result holds, when a later one could
 * not be built. */
static void rx_free_elem(Alloc *a, Slice s) {
    if (s.p != NULL && s.elem != NULL && s.cap > 0)
        mem_free(a, s.p, (size_t)s.cap * s.elem->size, s.elem->align);
}

/* The FindAll family. The elements go into a vector on the heap first, and
 * the result is one slice of exactly the right length from a. */
static Slice rx_find_all(const Regexp *re, Alloc *a, const Byte *p, Int len, Slice b,
                         bool is_str, Int n, RxAllKind kind) {
    const Type *desc;
    switch (kind) {
    case RX_ALL:
        desc = is_str ? &rx_strs_desc : &rx_bytess_desc;
        break;
    case RX_ALL_SUBMATCH:
        desc = is_str ? &rx_strss_desc : &rx_bytesss_desc;
        break;
    case RX_ALL_INDEX:
    case RX_ALL_SUBMATCH_INDEX:
    default:
        desc = &rx_intss_desc;
        break;
    }
    Slice nil = slice_nil(desc);
    Int ncap = kind == RX_ALL || kind == RX_ALL_INDEX ? 2 : re->p->prog->num_cap;
    RxMatches it;
    if (!rx_matches_init(&it, re, p, len, n, ncap))
        return nil;
    Alloc *h = heap_allocator();
    size_t esz = desc->elem->size;
    Byte *vec = NULL;
    Int count = 0, vcap = 0;
    bool oom = false;
    Str s = str_from_bytes(p, len);
    while (rx_matches_next(&it)) {
        const Int *m = it.c.m;
        union {
            Str s;
            Slice sl;
        } e;
        switch (kind) {
        case RX_ALL:
            if (is_str)
                e.s = str_from_bytes(rx_at(p, m[0]), m[1] - m[0]);
            else
                e.sl = rx_sub(b, m[0], m[1]);
            break;
        case RX_ALL_INDEX:
            e.sl = rx_ints(a, m, 2);
            oom = e.sl.p == NULL;
            break;
        case RX_ALL_SUBMATCH:
            e.sl = is_str ? rx_str_submatch(a, s, m, it.c.n)
                          : rx_bytes_submatch(a, b, m, it.c.n);
            oom = e.sl.p == NULL;
            break;
        case RX_ALL_SUBMATCH_INDEX:
        default:
            e.sl = rx_ints(a, m, it.c.n);
            oom = e.sl.p == NULL;
            break;
        }
        if (oom)
            break;
        if (count == vcap) {
            Int ncap2 = vcap == 0 ? 8 : 2 * vcap;
            Byte *v = (Byte *)mem_realloc(h, vec, (size_t)vcap * esz,
                                          (size_t)ncap2 * esz, _Alignof(Slice));
            if (v == NULL) {
                if (kind != RX_ALL)
                    rx_free_elem(a, e.sl);
                oom = true;
                break;
            }
            vec = v;
            vcap = ncap2;
        }
        memcpy(vec + (size_t)count * esz, &e, esz);
        count++;
    }
    rx_caps_free(&it.c);
    Slice out = nil;
    if (!oom && count > 0) {
        out = slice_make(a, desc, count, count);
        if (out.p != NULL)
            memcpy(out.p, vec, (size_t)count * esz);
        else
            oom = true;
    }
    if (oom && kind != RX_ALL) {
        for (Int k = 0; k < count; k++) {
            Slice el;
            memcpy(&el, vec + (size_t)k * esz, sizeof el);
            rx_free_elem(a, el);
        }
    }
    if (vec != NULL)
        mem_free(h, vec, (size_t)vcap * esz, _Alignof(Slice));
    return oom ? nil : out;
}

Slice regexp_find_all(const Regexp *re, Alloc *a, Slice b, Int n) {
    return rx_find_all(re, a, rx_bp(b), b.len, b, false, n, RX_ALL);
}

Slice regexp_find_all_string(const Regexp *re, Alloc *a, Str s, Int n) {
    return rx_find_all(re, a, s.p, s.len, slice_nil(TYPE_BYTE), true, n, RX_ALL);
}

Slice regexp_find_all_index(const Regexp *re, Alloc *a, Slice b, Int n) {
    return rx_find_all(re, a, rx_bp(b), b.len, b, false, n, RX_ALL_INDEX);
}

Slice regexp_find_all_string_index(const Regexp *re, Alloc *a, Str s, Int n) {
    return rx_find_all(re, a, s.p, s.len, slice_nil(TYPE_BYTE), true, n, RX_ALL_INDEX);
}

Slice regexp_find_all_submatch(const Regexp *re, Alloc *a, Slice b, Int n) {
    return rx_find_all(re, a, rx_bp(b), b.len, b, false, n, RX_ALL_SUBMATCH);
}

Slice regexp_find_all_string_submatch(const Regexp *re, Alloc *a, Str s, Int n) {
    return rx_find_all(re, a, s.p, s.len, slice_nil(TYPE_BYTE), true, n,
                       RX_ALL_SUBMATCH);
}

Slice regexp_find_all_submatch_index(const Regexp *re, Alloc *a, Slice b, Int n) {
    return rx_find_all(re, a, rx_bp(b), b.len, b, false, n, RX_ALL_SUBMATCH_INDEX);
}

Slice regexp_find_all_string_submatch_index(const Regexp *re, Alloc *a, Str s, Int n) {
    return rx_find_all(re, a, s.p, s.len, slice_nil(TYPE_BYTE), true, n,
                       RX_ALL_SUBMATCH_INDEX);
}

Slice regexp_split(const Regexp *re, Alloc *a, Str s, Int n) {
    Slice nil = slice_nil(&rx_strs_desc);
    if (n == 0)
        return nil;
    if (re->p->expr.len > 0 && s.len == 0) {
        Slice one = slice_make(a, &rx_strs_desc, 1, 1);
        if (one.p != NULL)
            *(Str *)one.p = BURROW_STR_EMPTY;
        return one;
    }
    /* Split takes at most n pieces, so at most n matches matter, and the
     * pieces are never more than the matches plus one. Count them first. */
    RxMatches it;
    if (!rx_matches_init(&it, re, s.p, s.len, n, 2))
        return nil;
    Int nmatch = 0;
    while (rx_matches_next(&it))
        nmatch++;
    rx_caps_free(&it.c);

    Slice out = slice_make(a, &rx_strs_desc, 0, nmatch + 1);
    if (out.p == NULL)
        return nil;
    Str *strs = (Str *)out.p;
    if (!rx_matches_init(&it, re, s.p, s.len, n, 2)) {
        rx_free_elem(a, out);
        return nil;
    }
    Int beg = 0, end = 0;
    while (rx_matches_next(&it)) {
        const Int *m = it.c.m;
        if (n > 0 && out.len >= n - 1)
            break;
        end = m[0];
        if (m[1] != 0)
            strs[out.len++] = str_from_bytes(rx_at(s.p, beg), end - beg);
        beg = m[1];
    }
    rx_caps_free(&it.c);
    if (end != s.len)
        strs[out.len++] = str_from_bytes(rx_at(s.p, beg), s.len - beg);
    return out;
}

/* -------------------------------------------------------------- replacing */

/* A growing byte buffer in a, which remembers running out. */
typedef struct RxBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    bool oom;
} RxBuf;

static void rx_buf_write(RxBuf *b, const Byte *p, Int n) {
    if (n <= 0 || b->oom)
        return;
    if (b->len + n > b->cap) {
        Int c = b->cap == 0 ? 64 : b->cap;
        while (c < b->len + n)
            c *= 2;
        Byte *q = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)c, 1);
        if (q == NULL) {
            b->oom = true;
            return;
        }
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void rx_buf_free(RxBuf *b) {
    if (b->p != NULL)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* The buffer as a []byte, nil when nothing went in, as Go's append leaves it. */
static Slice rx_buf_bytes(RxBuf *b) {
    if (b->oom || b->len == 0) {
        rx_buf_free(b);
        return slice_nil(TYPE_BYTE);
    }
    Slice s = {b->p, b->len, b->cap, TYPE_BYTE};
    return s;
}

/* The buffer as a Str, trimmed to its length. */
static Str rx_buf_str(RxBuf *b) {
    if (b->oom || b->len == 0) {
        rx_buf_free(b);
        return BURROW_STR_EMPTY;
    }
    /* The caller frees len bytes, so a buffer that will not shrink to that is
     * as good as one that could not grow. */
    Byte *p = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)b->len, 1);
    if (p == NULL) {
        rx_buf_free(b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(p, b->len);
}

/* extract: the name at the start of str, after a $, with or without braces. */
static bool rx_extract(Str str, Str *name, Int *num, Str *rest) {
    if (str.len == 0)
        return false;
    bool brace = false;
    if (str.p[0] == '{') {
        brace = true;
        str = str_from_bytes(str.p + 1, str.len - 1);
    }
    Int i = 0;
    while (i < str.len) {
        Int size;
        Rune r = utf8_decode_rune_in_string(
            str_from_bytes(rx_at(str.p, i), str.len - i), &size);
        if (!unicode_is_letter(r) && !unicode_is_digit(r) && r != '_')
            break;
        i += size;
    }
    if (i == 0) /* empty name is not okay */
        return false;
    *name = str_from_bytes(str.p, i);
    if (brace) {
        if (i >= str.len || str.p[i] != '}') /* missing closing brace */
            return false;
        i++;
    }
    /* Parse number. */
    Int n = 0;
    for (Int k = 0; k < name->len; k++) {
        if (name->p[k] < '0' || '9' < name->p[k] || n >= 100000000) {
            n = -1;
            break;
        }
        n = n * 10 + (Int)(name->p[k] - '0');
    }
    /* Disallow leading zeros. */
    if (name->p[0] == '0' && name->len > 1)
        n = -1;
    *num = n;
    *rest = str_from_bytes(rx_at(str.p, i), str.len - i);
    return true;
}

/* expand: template with its $ references filled in from src and match. */
static void rx_expand(const Regexp *re, RxBuf *dst, Str tmpl, Str src, const Int *match,
                      Int nmatch) {
    while (tmpl.len > 0) {
        Int d = strings_index_byte(tmpl, '$');
        if (d < 0)
            break;
        rx_buf_write(dst, tmpl.p, d);
        tmpl = str_from_bytes(tmpl.p + d + 1, tmpl.len - d - 1);
        if (tmpl.len > 0 && tmpl.p[0] == '$') {
            /* Treat $$ as $. */
            rx_buf_write(dst, (const Byte *)"$", 1);
            tmpl = str_from_bytes(tmpl.p + 1, tmpl.len - 1);
            continue;
        }
        Str name, rest;
        Int num;
        if (!rx_extract(tmpl, &name, &num, &rest)) {
            /* Malformed; treat $ as raw text. */
            rx_buf_write(dst, (const Byte *)"$", 1);
            continue;
        }
        tmpl = rest;
        if (num >= 0) {
            if (2 * num + 1 < nmatch && match[2 * num] >= 0)
                rx_buf_write(dst, rx_at(src.p, match[2 * num]),
                             match[2 * num + 1] - match[2 * num]);
        } else {
            const Str *names = (const Str *)re->p->subexp_names.p;
            for (Int i = 0; i < re->p->subexp_names.len; i++) {
                if (str_eq(name, names[i]) && 2 * i + 1 < nmatch && match[2 * i] >= 0) {
                    rx_buf_write(dst, rx_at(src.p, match[2 * i]),
                                 match[2 * i + 1] - match[2 * i]);
                    break;
                }
            }
        }
    }
    rx_buf_write(dst, tmpl.p, tmpl.len);
}

typedef enum RxReplKind {
    RX_REPL_EXPAND,
    RX_REPL_LITERAL,
    RX_REPL_STR_FUNC,
    RX_REPL_BYTES_FUNC,
} RxReplKind;

typedef struct RxRepl {
    RxReplKind kind;
    Str repl;
    StrFunc sf;
    BytesFunc bf;
    Slice bsrc;
} RxRepl;

/* replaceAll. */
static void rx_replace_all(const Regexp *re, RxBuf *buf, Str src, Int nmatch,
                           const RxRepl *r) {
    Int last_match_end = 0; /* end position of the most recent match */
    Int search_pos = 0;     /* position where we next look for a match */
    Int end_pos = src.len;
    if (nmatch > re->p->prog->num_cap)
        nmatch = re->p->prog->num_cap;
    RxCaps c;
    if (!rx_caps_init(&c, re)) {
        buf->oom = true;
        return;
    }
    Int *a = c.m;
    RxInput in = rx_bytes(src.p, src.len);
    while (search_pos <= end_pos) {
        if (!rx_find(re, &in, search_pos, nmatch, a))
            break; /* no more matches */

        /* Copy the unmatched characters before this match. */
        rx_buf_write(buf, rx_at(src.p, last_match_end), a[0] - last_match_end);

        /* Now insert a copy of the replacement string, but not for a match of
         * the empty string immediately after another match. (Otherwise, we
         * get double replacement for patterns that match both empty and
         * nonempty strings.) */
        if (a[1] > last_match_end || a[0] == 0) {
            switch (r->kind) {
            case RX_REPL_EXPAND:
                rx_expand(re, buf, r->repl, src, a, nmatch);
                break;
            case RX_REPL_LITERAL:
                rx_buf_write(buf, r->repl.p, r->repl.len);
                break;
            case RX_REPL_STR_FUNC: {
                Str got = BURROW_CALLF(r->sf,
                                       str_from_bytes(rx_at(src.p, a[0]), a[1] - a[0]));
                rx_buf_write(buf, got.p, got.len);
                break;
            }
            case RX_REPL_BYTES_FUNC:
            default: {
                Slice in_b = r->bsrc;
                if (in_b.elem == NULL)
                    in_b.elem = TYPE_BYTE;
                Slice got = BURROW_CALLF(r->bf, slice_sub(in_b, a[0], a[1]));
                rx_buf_write(buf, (const Byte *)got.p, got.len);
                break;
            }
            }
        }
        last_match_end = a[1];

        /* Advance past this match; always advance at least one character. */
        Int width;
        utf8_decode_rune_in_string(
            str_from_bytes(rx_at(src.p, search_pos), src.len - search_pos), &width);
        if (search_pos + width > a[1])
            search_pos += width;
        else if (search_pos + 1 > a[1])
            /* This clause is only needed at the end of the input string. In
             * that case, DecodeRuneInString returns width=0. */
            search_pos++;
        else
            search_pos = a[1];
    }
    rx_caps_free(&c);

    /* Copy the unmatched characters after the last match. */
    rx_buf_write(buf, rx_at(src.p, last_match_end), src.len - last_match_end);
}

static Int rx_expand_nmatch(const Regexp *re, Str repl) {
    return strings_index_byte(repl, '$') >= 0 ? 2 * (re->p->num_subexp + 1) : 2;
}

static Str rx_bstr(Slice b) {
    return str_from_bytes(rx_bp(b), b.len);
}

Slice regexp_replace_all(const Regexp *re, Alloc *a, Slice src, Slice repl) {
    RxRepl r = {RX_REPL_EXPAND, rx_bstr(repl), {0}, {0}, src};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, rx_bstr(src), rx_expand_nmatch(re, r.repl), &r);
    return rx_buf_bytes(&buf);
}

Str regexp_replace_all_string(const Regexp *re, Alloc *a, Str src, Str repl) {
    RxRepl r = {RX_REPL_EXPAND, repl, {0}, {0}, {0}};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, src, rx_expand_nmatch(re, repl), &r);
    return rx_buf_str(&buf);
}

Slice regexp_replace_all_literal(const Regexp *re, Alloc *a, Slice src, Slice repl) {
    RxRepl r = {RX_REPL_LITERAL, rx_bstr(repl), {0}, {0}, src};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, rx_bstr(src), 2, &r);
    return rx_buf_bytes(&buf);
}

Str regexp_replace_all_literal_string(const Regexp *re, Alloc *a, Str src, Str repl) {
    RxRepl r = {RX_REPL_LITERAL, repl, {0}, {0}, {0}};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, src, 2, &r);
    return rx_buf_str(&buf);
}

Slice regexp_replace_all_func(const Regexp *re, Alloc *a, Slice src, BytesFunc repl) {
    RxRepl r = {RX_REPL_BYTES_FUNC, BURROW_STR_EMPTY, {0}, repl, src};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, rx_bstr(src), 2, &r);
    return rx_buf_bytes(&buf);
}

Str regexp_replace_all_string_func(const Regexp *re, Alloc *a, Str src, StrFunc repl) {
    RxRepl r = {RX_REPL_STR_FUNC, BURROW_STR_EMPTY, repl, {0}, {0}};
    RxBuf buf = {a, NULL, 0, 0, false};
    rx_replace_all(re, &buf, src, 2, &r);
    return rx_buf_str(&buf);
}

/* Expand and ExpandString: the expansion goes into a buffer on the heap, then
 * onto dst with append's rules. */
static Slice rx_expand_append(const Regexp *re, Alloc *a, Slice dst, Str tmpl, Str src,
                              Slice match) {
    RxBuf buf = {heap_allocator(), NULL, 0, 0, false};
    rx_expand(re, &buf, tmpl, src, (const Int *)match.p, match.len);
    if (dst.elem == NULL)
        dst.elem = TYPE_BYTE;
    Slice out = dst;
    if (buf.oom)
        out = slice_nil(TYPE_BYTE);
    else if (buf.len > 0)
        out = slice_append(a, dst, buf.p, buf.len);
    rx_buf_free(&buf);
    return out;
}

Slice regexp_expand(const Regexp *re, Alloc *a, Slice dst, Slice template_, Slice src,
                    Slice match) {
    return rx_expand_append(re, a, dst, rx_bstr(template_), rx_bstr(src), match);
}

Slice regexp_expand_string(const Regexp *re, Alloc *a, Slice dst, Str template_,
                           Str src, Slice match) {
    return rx_expand_append(re, a, dst, template_, src, match);
}

/* ---------------------------------------------------------------- text form */

Slice regexp_append_text(const Regexp *re, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (b.elem == NULL)
        b.elem = TYPE_BYTE;
    Str s = re->p->expr;
    if (s.len == 0)
        return b;
    Slice out = slice_append(a, b, s.p, s.len);
    if (out.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}

Slice regexp_marshal_text(const Regexp *re, Alloc *a, Error *err) {
    return regexp_append_text(re, a, slice_nil(TYPE_BYTE), err);
}

Error regexp_unmarshal_text(Regexp *re, Alloc *a, Slice text) {
    Error err;
    RxProg *p = rx_prog_new(a, rx_bstr(text), SYNTAX_PERL, &err);
    if (p == NULL)
        return err;
    rx_prog_free(re->p);
    re->p = p;
    re->longest = false;
    return BURROW_NO_ERROR;
}
