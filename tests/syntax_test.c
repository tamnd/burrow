/* Derived from Go's src/regexp/syntax/parse_test.go, simplify_test.go and
 * prog_test.go.
 * Go source: go1.27.1.
 *
 * Go's tables, parseTests and the rest, are in tests/syntax_test_gen.h, from
 * tools/gen-syntax-tests.sh, along with a few thousand random patterns and
 * everything Go makes of each one: the error, the tree as dumpRegexp prints
 * it, String, Simplify, the compiled program, Prefix, StartCond, MaxCap and
 * CapNames. TestGoCases checks all of it. The patterns too long for the
 * header, the ones that test the size and depth limits, are built here.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/regexp/syntax.h"

#include "../src/regexp/syntax_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SynText {
    const char *s; /* NULL when only the hash was kept */
    Int len;
    uint64_t hash;
} SynText;

typedef struct SynGenCase {
    const char *pat;
    Int pat_len;
    uint16_t flags;
    SynText err;
    SynText dump;
    SynText str;
    int roundtrip; /* 1 parses back equal, 0 parses back different, 2 fails */
    SynText simple;
    SynText prog;
    SynText prefix;
    int complete;
    int start_cond;
    Int num_cap;
    Int max_cap;
    SynText cap_names;
} SynGenCase;

#include "syntax_test_gen.h"

static uint64_t fnv1a(Str s) {
    uint64_t h = 14695981039346656037ULL;
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool text_eq(Str got, SynText want) {
    if (got.len != want.len)
        return false;
    if (want.s == NULL)
        return fnv1a(got) == want.hash;
    return got.len == 0 || memcmp(got.p, want.s, (size_t)got.len) == 0;
}

/* A C string copy of s, for messages. */
static const char *cs(Alloc *a, Str s) {
    char *p = (char *)mem_alloc(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p;
}

static const char *want_cs(Alloc *a, SynText w) {
    if (w.s != NULL)
        return w.s;
    char *p = (char *)mem_alloc(a, 64, 1);
    snprintf(p, 64, "<%d bytes, hash %016llx>", (int)w.len, (unsigned long long)w.hash);
    return p;
}

/* ------------------------------------------------------------------ dump */

static const char *const op_names[] = {
    NULL,  "no", "emp", "lit", "cc",   "dnl",  "dot", "bol", "eol", "bot",
    "eot", "wb", "nwb", "cap", "star", "plus", "que", "rep", "cat", "alt",
};

typedef struct Dumper {
    Alloc *a;
    StringsBuilder b;
} Dumper;

static void dump_str(Dumper *d, const char *s) {
    strings_builder_write_string(&d->b, str_from_cstr(s), NULL);
}

/* The printf calls are kept out of dump_regexp, which recurses as deep as
 * the tree, so that its frame stays small. */
static BURROW_NOINLINE void dump_hex(Dumper *d, Rune r) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%x", (unsigned)r);
    dump_str(d, buf);
}

static BURROW_NOINLINE void dump_op(Dumper *d, int op) {
    char buf[16];
    snprintf(buf, sizeof buf, "op%d", op);
    dump_str(d, buf);
}

static BURROW_NOINLINE void dump_min_max(Dumper *d, Int min, Int max) {
    char buf[48];
    snprintf(buf, sizeof buf, "%lld,%lld ", (long long)min, (long long)max);
    dump_str(d, buf);
}

/* dumpRegexp from parse_test.go. */
static void dump_regexp(Dumper *d, const SyntaxRegexp *re) {
    const Rune *r = (const Rune *)re->rune.p;
    SyntaxRegexp *const *sub = (SyntaxRegexp *const *)re->sub.p;
    if (re->op >= sizeof op_names / sizeof *op_names || op_names[re->op] == NULL) {
        dump_op(d, re->op);
    } else {
        switch (re->op) {
        default:
            dump_str(d, op_names[re->op]);
            break;
        case SYNTAX_OP_STAR:
        case SYNTAX_OP_PLUS:
        case SYNTAX_OP_QUEST:
        case SYNTAX_OP_REPEAT:
            if (re->flags & SYNTAX_NON_GREEDY)
                dump_str(d, "n");
            dump_str(d, op_names[re->op]);
            break;
        case SYNTAX_OP_LITERAL:
            dump_str(d, re->rune.len > 1 ? "str" : "lit");
            if (re->flags & SYNTAX_FOLD_CASE) {
                for (Int i = 0; i < re->rune.len; i++) {
                    if (unicode_simple_fold(r[i]) != r[i]) {
                        dump_str(d, "fold");
                        break;
                    }
                }
            }
            break;
        }
    }
    dump_str(d, "{");
    switch (re->op) {
    case SYNTAX_OP_END_TEXT:
        if ((re->flags & SYNTAX_WAS_DOLLAR) == 0)
            dump_str(d, "\\z");
        break;
    case SYNTAX_OP_LITERAL:
        for (Int i = 0; i < re->rune.len; i++)
            strings_builder_write_rune(&d->b, r[i], NULL);
        break;
    case SYNTAX_OP_CONCAT:
    case SYNTAX_OP_ALTERNATE:
        for (Int i = 0; i < re->sub.len; i++)
            dump_regexp(d, sub[i]);
        break;
    case SYNTAX_OP_STAR:
    case SYNTAX_OP_PLUS:
    case SYNTAX_OP_QUEST:
        dump_regexp(d, sub[0]);
        break;
    case SYNTAX_OP_REPEAT:
        dump_min_max(d, re->min, re->max);
        dump_regexp(d, sub[0]);
        break;
    case SYNTAX_OP_CAPTURE:
        if (re->name.len > 0) {
            strings_builder_write_string(&d->b, re->name, NULL);
            dump_str(d, ":");
        }
        dump_regexp(d, sub[0]);
        break;
    case SYNTAX_OP_CHAR_CLASS:
        for (Int i = 0; i < re->rune.len; i += 2) {
            if (i > 0)
                dump_str(d, " ");
            dump_hex(d, r[i]);
            if (r[i] != r[i + 1]) {
                dump_str(d, "-");
                dump_hex(d, r[i + 1]);
            }
        }
        break;
    default:
        break;
    }
    dump_str(d, "}");
}

static Str dump(Alloc *a, const SyntaxRegexp *re) {
    Dumper d = {a, STRINGS_BUILDER(a)};
    dump_regexp(&d, re);
    return strings_builder_string(&d.b);
}

/* ---------------------------------------------------------------- the cases */

static Str join_names(Alloc *a, Slice names) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < names.len; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ',');
        strings_builder_write_string(&b, ((Str *)names.p)[i], NULL);
    }
    return strings_builder_string(&b);
}

static int failures;

#define CASE_TEXT(what, got, want)                                                     \
    do {                                                                               \
        if (!text_eq((got), (want))) {                                                 \
            if (failures++ < 30)                                                       \
                testing_t_errorf_v(t, "case %d %q flags %#x: %s = %q, want %q",        \
                                   (int)k, c->pat, c->flags, what, cs(a, (got)),       \
                                   want_cs(a, (want)));                                \
        }                                                                              \
    } while (0)

#define CASE_INT(what, got, want)                                                      \
    do {                                                                               \
        if ((int64_t)(got) != (int64_t)(want)) {                                       \
            if (failures++ < 30)                                                       \
                testing_t_errorf_v(t, "case %d %q flags %#x: %s = %d, want %d",        \
                                   (int)k, c->pat, c->flags, what, (int64_t)(got),     \
                                   (int64_t)(want));                                   \
        }                                                                              \
    } while (0)

static void TestGoCases(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Alloc *h = heap_allocator();
    size_t n = sizeof syn_gen_cases / sizeof *syn_gen_cases;
    failures = 0;
    for (size_t k = 0; k < n; k++) {
        const SynGenCase *c = &syn_gen_cases[k];
        Str pat = str_from_bytes((const Byte *)c->pat, c->pat_len);
        Error err;
        SyntaxRegexp *re = syntax_parse(h, pat, c->flags, &err);
        if (c->err.len != 0) {
            if (re != NULL) {
                if (failures++ < 30)
                    testing_t_errorf_v(t, "case %d %q flags %#x: parsed, want error %q",
                                       (int)k, c->pat, c->flags, want_cs(a, c->err));
                syntax_regexp_free(re);
            } else {
                CASE_TEXT("error", error_text(err), c->err);
                CHECK(errors_as(err, TYPE_SYNTAX_ERROR) != NULL);
            }
            continue;
        }
        if (re == NULL) {
            if (failures++ < 30)
                testing_t_errorf_v(t, "case %d %q flags %#x: %q", (int)k, c->pat,
                                   c->flags, cs(a, error_text(err)));
            continue;
        }
        CASE_TEXT("dump", dump(a, re), c->dump);
        Str s = syntax_regexp_string(re, a);
        CASE_TEXT("String", s, c->str);

        SyntaxRegexp *re2 = syntax_parse(h, s, c->flags, &err);
        int roundtrip = re2 == NULL ? 2 : syntax_regexp_equal(re, re2) ? 1 : 0;
        CASE_INT("round trip", roundtrip, c->roundtrip);
        syntax_regexp_free(re2);

        SyntaxRegexp *simple = syntax_regexp_simplify(re, h);
        CASE_TEXT("Simplify", syntax_regexp_string(simple, a), c->simple);

        SyntaxProg *prog = syntax_compile(h, simple, &err);
        CASE_TEXT("Prog", syntax_prog_string(prog, a), c->prog);
        bool complete;
        CASE_TEXT("Prefix", syntax_prog_prefix(prog, a, &complete), c->prefix);
        CASE_INT("complete", complete, c->complete);
        CASE_INT("StartCond", syntax_prog_start_cond(prog), c->start_cond);
        CASE_INT("NumCap", prog->num_cap, c->num_cap);

        CASE_INT("MaxCap", syntax_regexp_max_cap(re), c->max_cap);
        CASE_TEXT("CapNames", join_names(a, syntax_regexp_cap_names(re, a)),
                  c->cap_names);

        /* The program and the simplified tree outlive the tree they came
         * from. */
        syntax_regexp_free(re);
        CHECK(syntax_prog_string(prog, a).len > 0);
        syntax_prog_free(prog);
        syntax_regexp_free(simple);
    }
    CHECK(n > 3000);
    arena_free(&ar);
}

/* ------------------------------------------------------------ big patterns */

static Str repeat(Alloc *a, const char *s, Int n) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < n; i++)
        strings_builder_write_string(&b, str_from_cstr(s), NULL);
    return strings_builder_string(&b);
}

static Str concat3(Alloc *a, Str x, Str y, Str z) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, x, NULL);
    strings_builder_write_string(&b, y, NULL);
    strings_builder_write_string(&b, z, NULL);
    return strings_builder_string(&b);
}

static void check_big(TestingT *t, Alloc *a, Str pat, SyntaxErrorCode want) {
    Error err;
    SyntaxRegexp *re = syntax_parse(heap_allocator(), pat, SYNTAX_PERL, &err);
    if (want.len == 0) {
        if (re == NULL)
            testing_t_errorf_v(t, "%d byte pattern: %q", (int)pat.len,
                               cs(a, error_text(err)));
        /* The whole round: print, simplify and compile it too. */
        Str s = syntax_regexp_string(re, a);
        CHECK(s.len > 0);
        SyntaxRegexp *simple = syntax_regexp_simplify(re, heap_allocator());
        SyntaxProg *prog = syntax_compile(heap_allocator(), simple, &err);
        CHECK(prog != NULL);
        syntax_prog_free(prog);
        syntax_regexp_free(simple);
        syntax_regexp_free(re);
        return;
    }
    CHECK(re == NULL);
    const SyntaxError *se = (const SyntaxError *)errors_as(err, TYPE_SYNTAX_ERROR);
    CHECK(se != NULL);
    if (se != NULL) {
        CHECK(str_eq(se->code, want));
        /* ErrLarge and ErrNestingDepth carry the whole pattern. */
        CHECK(str_eq(se->expr, pat));
    }
}

/* The long ones from parseTests and invalidRegexps. */
static void TestBigPatterns(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str none = BURROW_STR_EMPTY;
    check_big(t, a, concat3(a, repeat(a, "(", 999), repeat(a, ")", 999), none), none);
    check_big(t, a, concat3(a, repeat(a, "(?:", 999), repeat(a, ")*", 999), none),
              none);
    check_big(t, a, concat3(a, BURROW_S("("), repeat(a, "|", 12345), BURROW_S(")")),
              none);

    check_big(t, a, concat3(a, repeat(a, "(", 1000), repeat(a, ")", 1000), none),
              SYNTAX_ERR_NESTING_DEPTH);
    check_big(t, a, concat3(a, repeat(a, "(?:", 1000), repeat(a, ")*", 1000), none),
              SYNTAX_ERR_NESTING_DEPTH);
    check_big(t, a,
              concat3(a, BURROW_S("("), repeat(a, "(xx?)", 1000), BURROW_S("){1000}")),
              SYNTAX_ERR_LARGE);
    check_big(t, a, repeat(a, "(xx?){1000}", 1000), SYNTAX_ERR_LARGE);
    check_big(t, a, repeat(a, "\\pL", 27000), SYNTAX_ERR_LARGE);
    arena_free(&ar);
}

/* ------------------------------------------------------------- the rest */

/* TestFoldConstants. */
static void TestFoldConstants(TestingT *t) {
    Rune last = -1;
    for (Rune i = 0; i <= UNICODE_MAX_RUNE; i++) {
        if (unicode_simple_fold(i) == i)
            continue;
        if (last == -1)
            CHECK_INT_EQ(SYN_MIN_FOLD, i);
        last = i;
    }
    CHECK_INT_EQ(SYN_MAX_FOLD, last);
}

static void TestOpString(TestingT *t) {
    CHECK(str_eq(syntax_op_string(SYNTAX_OP_NO_MATCH), BURROW_S("NoMatch")));
    CHECK(str_eq(syntax_op_string(SYNTAX_OP_ALTERNATE), BURROW_S("Alternate")));
    CHECK(str_eq(syntax_op_string(0), BURROW_S("Op(0)")));
    CHECK(str_eq(syntax_op_string(20), BURROW_S("Op(20)")));
    CHECK(str_eq(syntax_op_string(128), BURROW_S("opPseudo")));
    CHECK(str_eq(syntax_op_string(255), BURROW_S("Op(255)")));
    CHECK(str_eq(syntax_inst_op_string(SYNTAX_INST_ALT), BURROW_S("InstAlt")));
    CHECK(str_eq(syntax_inst_op_string(SYNTAX_INST_RUNE_ANY_NOT_NL),
                 BURROW_S("InstRuneAnyNotNL")));
    CHECK(syntax_inst_op_string(11).len == 0);
    CHECK(str_eq(syntax_error_code_string(SYNTAX_ERR_MISSING_PAREN),
                 BURROW_S("missing closing )")));
}

static void TestEmptyOpContext(TestingT *t) {
    CHECK_INT_EQ(syntax_empty_op_context(-1, -1),
                 SYNTAX_EMPTY_BEGIN_LINE | SYNTAX_EMPTY_END_LINE |
                     SYNTAX_EMPTY_BEGIN_TEXT | SYNTAX_EMPTY_END_TEXT |
                     SYNTAX_EMPTY_NO_WORD_BOUNDARY);
    CHECK_INT_EQ(syntax_empty_op_context(-1, 'a'), SYNTAX_EMPTY_BEGIN_LINE |
                                                       SYNTAX_EMPTY_BEGIN_TEXT |
                                                       SYNTAX_EMPTY_WORD_BOUNDARY);
    CHECK_INT_EQ(syntax_empty_op_context('\n', ' '),
                 SYNTAX_EMPTY_BEGIN_LINE | SYNTAX_EMPTY_NO_WORD_BOUNDARY);
    CHECK_INT_EQ(syntax_empty_op_context('a', '\n'),
                 SYNTAX_EMPTY_END_LINE | SYNTAX_EMPTY_WORD_BOUNDARY);
    CHECK_INT_EQ(syntax_empty_op_context('a', 'b'), SYNTAX_EMPTY_NO_WORD_BOUNDARY);
    CHECK(syntax_is_word_char('_') && syntax_is_word_char('Z') &&
          !syntax_is_word_char('-'));
}

static void TestMatchRune(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    /* One of each shape of class: a folded rune, one range, a few ranges and
     * enough for the binary search. */
    SyntaxRegexp *re = syntax_parse(a, BURROW_S("(?i)k"), SYNTAX_PERL, &err);
    SyntaxProg *p = syntax_compile(a, re, &err);
    const SyntaxInst *i = &((const SyntaxInst *)p->inst.p)[p->start + 0];
    while (i->op != SYNTAX_INST_RUNE)
        i = &((const SyntaxInst *)p->inst.p)[i->out];
    CHECK(syntax_inst_match_rune(i, 'k') && syntax_inst_match_rune(i, 'K') &&
          syntax_inst_match_rune(i, 0x212A) && !syntax_inst_match_rune(i, 'j'));
    CHECK(str_eq(syntax_inst_string(i, a), BURROW_S("rune \"K\"/i -> 2")));

    static const Rune two[] = {'a', 'c'};
    static const Rune six[] = {'a', 'c', 'e', 'g', 'x', 'z'};
    static const Rune ten[] = {'0', '1', '3', '4', '6', '7', 'a', 'c', 'x', 'z'};
    SyntaxInst in = {SYNTAX_INST_RUNE, 0, 0, {(void *)(uintptr_t)two, 2, 2, NULL}};
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, 'b'), 0);
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, 'd'), -1);
    in.rune = (Slice){(void *)(uintptr_t)six, 6, 6, NULL};
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, 'f'), 1);
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, 'y'), 2);
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, 'd'), -1);
    CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, '~'), -1);
    in.rune = (Slice){(void *)(uintptr_t)ten, 10, 10, NULL};
    for (Rune r = 0; r < 128; r++) {
        Int want = -1;
        for (Int j = 0; j < 10; j += 2)
            if (ten[j] <= r && r <= ten[j + 1])
                want = j / 2;
        CHECK_INT_EQ(syntax_inst_match_rune_pos(&in, r), want);
    }
    in.rune = (Slice){0};
    CHECK(!syntax_inst_match_rune(&in, 'a'));
    CHECK(str_eq(syntax_inst_string(&in, a), BURROW_S("rune <nil>rune \"\" -> 0")));
    arena_free(&ar);
}

static void bad_empty_width(void *arg) {
    SyntaxInst *i = (SyntaxInst *)arg;
    syntax_inst_match_empty_width(i, 'a', 'b');
}

static void TestMatchEmptyWidth(TestingT *t) {
    SyntaxInst i = {SYNTAX_INST_EMPTY_WIDTH, 0, SYNTAX_EMPTY_BEGIN_LINE, {0}};
    CHECK(syntax_inst_match_empty_width(&i, '\n', 'a'));
    CHECK(syntax_inst_match_empty_width(&i, -1, 'a'));
    CHECK(!syntax_inst_match_empty_width(&i, 'a', 'a'));
    i.arg = SYNTAX_EMPTY_END_TEXT;
    CHECK(syntax_inst_match_empty_width(&i, 'a', -1));
    CHECK(!syntax_inst_match_empty_width(&i, 'a', '\n'));
    i.arg = SYNTAX_EMPTY_WORD_BOUNDARY;
    CHECK(syntax_inst_match_empty_width(&i, 'a', ' '));
    i.arg = SYNTAX_EMPTY_NO_WORD_BOUNDARY;
    CHECK(syntax_inst_match_empty_width(&i, 'a', 'b'));
    i.arg = 3;
    volatile bool panicked = false;
    BURROW_TRY {
        bad_empty_width(&i);
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    CHECK(panicked);
}

static void TestSyntaxError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    CHECK(syntax_parse(a, BURROW_S("a(b"), SYNTAX_PERL, &err) == NULL);
    CHECK(str_eq(error_text(err),
                 BURROW_S("error parsing regexp: missing closing ): `a(b`")));
    const SyntaxError *se = (const SyntaxError *)errors_as(err, TYPE_SYNTAX_ERROR);
    CHECK(se != NULL && str_eq(se->code, SYNTAX_ERR_MISSING_PAREN) &&
          str_eq(se->expr, BURROW_S("a(b")));
    SyntaxError e = {SYNTAX_ERR_INVALID_ESCAPE, BURROW_S("\\8")};
    CHECK(str_eq(syntax_error_error(&e, a),
                 BURROW_S("error parsing regexp: invalid escape sequence: `\\8`")));
    Error c = error_retain(a, err);
    CHECK(errors_as(c, TYPE_SYNTAX_ERROR) != NULL);
    CHECK(str_eq(error_text(c), error_text(err)));
    arena_free(&ar);
}

/* Hand-built trees work with everything but the free functions. */
static void TestHandBuilt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SyntaxRegexp lit = {0};
    lit.op = SYNTAX_OP_LITERAL;
    lit.rune0[0] = 'x';
    lit.rune = (Slice){lit.rune0, 1, 2, TYPE_RUNE};
    SyntaxRegexp rep = {0};
    rep.op = SYNTAX_OP_REPEAT;
    rep.min = 2;
    rep.max = 3;
    rep.sub0[0] = &lit;
    rep.sub = (Slice){rep.sub0, 1, 1, TYPE_UNSAFE_POINTER};
    CHECK(str_eq(syntax_regexp_string(&rep, a), BURROW_S("x{2,3}")));
    SyntaxRegexp *s = syntax_regexp_simplify(&rep, a);
    CHECK(str_eq(syntax_regexp_string(s, a), BURROW_S("xxx?")));
    Error err;
    SyntaxProg *p = syntax_compile(a, s, &err);
    bool complete;
    CHECK(str_eq(syntax_prog_prefix(p, a, &complete), BURROW_S("xx")) && !complete);
    CHECK(syntax_regexp_simplify(NULL, a) == NULL);
    CHECK(syntax_regexp_equal(NULL, NULL) && !syntax_regexp_equal(&lit, NULL));
    syntax_regexp_free(NULL);
    syntax_prog_free(NULL);
    arena_free(&ar);
}

/* An allocator that gives out a set number of blocks and then refuses, and
 * keeps count of what is live, so that nothing leaks when it runs out. */
typedef struct Budget {
    int left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(heap_allocator(), p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

static void TestNoMemory(TestingT *t) {
    /* Big enough that the parser, the printer, Simplify and the compiler all
     * need more than one block of scratch memory. */
    Str pat = BURROW_S("(?i)(?P<first>[a-z\\pN]+|\\p{Greek}{2,5})(x|y|z)*?\\b"
                       "(?:abc|abd|aef|bcx|bcy){3}$");
    int parsed = 0, printed = 0, simplified = 0, compiled = 0;
    for (int budget = 0; budget < 400; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Error err;
        SyntaxRegexp *re = syntax_parse(&al, pat, SYNTAX_PERL, &err);
        if (re == NULL) {
            CHECK(errors_is(err, burrow_err_out_of_memory));
        } else {
            parsed++;
            Str s = syntax_regexp_string(re, &al);
            if (s.len > 0) {
                printed++;
                mem_free(&al, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            }
            SyntaxRegexp *simple = syntax_regexp_simplify(re, &al);
            if (simple != NULL) {
                simplified++;
                SyntaxProg *p = syntax_compile(&al, simple, &err);
                if (p != NULL) {
                    compiled++;
                    syntax_prog_free(p);
                } else {
                    CHECK(errors_is(err, burrow_err_out_of_memory));
                }
                syntax_regexp_free(simple);
            }
            syntax_regexp_free(re);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked", budget, (int)b.live);
    }
    CHECK(parsed > 0 && printed > 0 && simplified > 0 && compiled > 0);
    CHECK(parsed < 400 && compiled < parsed);
}

#define TESTS(X)                                                                       \
    X(TestGoCases)                                                                     \
    X(TestBigPatterns)                                                                 \
    X(TestFoldConstants)                                                               \
    X(TestOpString)                                                                    \
    X(TestEmptyOpContext)                                                              \
    X(TestMatchRune)                                                                   \
    X(TestMatchEmptyWidth)                                                             \
    X(TestSyntaxError)                                                                 \
    X(TestHandBuilt)                                                                   \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
