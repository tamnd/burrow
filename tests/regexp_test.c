/* Derived from Go's src/regexp/all_test.go, exec_test.go, find_test.go and
 * onepass_test.go.
 * Go source: go1.27.1.
 *
 * Go's tables, findTests, replaceTests and the rest, are in
 * tests/regexp_test_gen.h, from tools/gen-regexp-tests.sh, along with a
 * sample of the RE2 search and exhaustive tests, the Fowler POSIX tests and a
 * few thousand random cases. For each one the header has a summary of what
 * Go's regexp makes of it: every Find, FindAll, Split, Replace and Expand
 * form, written out as text. TestGoCases writes the same summary three times,
 * once as the Regexp comes, once with only the Pike VM and once with the
 * backtracker wherever it can run, and each has to come out the same as Go's.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/regexp.h"
#include "burrow/thread.h"

#include "../src/regexp/regexp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RxText {
    const char *s; /* NULL when only the hash was kept */
    Int len;
    uint64_t hash;
} RxText;

typedef struct RxGenCase {
    const char *pat;
    Int pat_len;
    uint16_t flags;
    int longest;
    const char *text;
    Int text_len;
    const char *repl;
    Int repl_len;
    Int n;
    RxText summary;
} RxGenCase;

typedef struct RxOnePassCase {
    const char *re;
    Int len;
    int onepass;
} RxOnePassCase;

typedef struct RxMinLenCase {
    const char *re;
    Int len;
    Int min;
} RxMinLenCase;

#include "regexp_test_gen.h"

static uint64_t fnv1a(Str s) {
    uint64_t h = 14695981039346656037ULL;
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool text_eq(Str got, RxText want) {
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

static const char *want_cs(Alloc *a, RxText w) {
    if (w.s != NULL)
        return w.s;
    char *p = (char *)mem_alloc(a, 64, 1);
    snprintf(p, 64, "<%d bytes, hash %016llx>", (int)w.len, (unsigned long long)w.hash);
    return p;
}

/* Gives back a slice from a, the way the functions under test made it. */
static void sfree(Alloc *a, Slice s) {
    if (s.p != NULL && s.cap > 0)
        mem_free(a, s.p, (size_t)s.cap * s.elem->size, s.elem->align);
}

static Str bstr(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* []byte(s), which is never nil, even for the empty string. */
static Slice sbytes(Str s) {
    static Byte empty[1];
    void *p = s.p != NULL ? (void *)(uintptr_t)s.p : empty;
    return slice_from(p, s.len, s.len, TYPE_BYTE);
}

/* ------------------------------------------------------------- the summary
 *
 * The same text summary() in the generator writes, with Go's fmt and
 * strconv.Quote for the formatting. */

typedef struct Sum {
    Alloc *a;
    StringsBuilder b;
} Sum;

static void put(Sum *s, const char *text) {
    strings_builder_write_string(&s->b, str_from_cstr(text), NULL);
}

static void put_str(Sum *s, Str text) {
    strings_builder_write_string(&s->b, text, NULL);
}

static void put_int(Sum *s, Int v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    put(s, buf);
}

static void put_bool(Sum *s, bool v) {
    put(s, v ? "true" : "false");
}

static void put_q(Sum *s, Str v) {
    put_str(s, strconv_quote(s->a, v));
}

static void put_ints(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_int(s, ((Int *)v.p)[i]);
    }
    put(s, "]");
}

static void put_intss(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_ints(s, ((Slice *)v.p)[i]);
    }
    put(s, "]");
}

static void put_strs(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_q(s, ((Str *)v.p)[i]);
    }
    put(s, "]");
}

static void put_strss(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_strs(s, ((Slice *)v.p)[i]);
    }
    put(s, "]");
}

static void put_bytes(Sum *s, Slice v) {
    if (v.p == NULL)
        put(s, "nil");
    else
        put_q(s, bstr(v));
}

static void put_bytess(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_bytes(s, ((Slice *)v.p)[i]);
    }
    put(s, "]");
}

static void put_bytesss(Sum *s, Slice v) {
    if (v.p == NULL) {
        put(s, "nil");
        return;
    }
    put(s, "[");
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            put(s, " ");
        put_bytess(s, ((Slice *)v.p)[i]);
    }
    put(s, "]");
}

static void key(Sum *s, const char *k) {
    put(s, k);
    put(s, "=");
}

static void sp(Sum *s) {
    put(s, " ");
}

static void nl(Sum *s) {
    put(s, "\n");
}

static IoRuneReader reader(StringsReader *r, Str s) {
    strings_reader_reset(r, s);
    return strings_reader_as_io_rune_reader(r);
}

static Str paren_str(void *env, Str m) {
    Alloc *a = (Alloc *)env;
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '(');
    strings_builder_write_string(&b, m, NULL);
    strings_builder_write_byte(&b, ')');
    return strings_builder_string(&b);
}

static Slice paren_bytes(void *env, Slice m) {
    return sbytes(paren_str(env, bstr(m)));
}

/* prefix and complete are what regexp_literal_prefix said with the Regexp as
 * compiled, which is what the summary reports whichever matcher runs. */
static Str summary(Alloc *a, const Regexp *re, Str t, Str repl, Int n, Str prefix,
                   bool complete) {
    Sum s = {a, STRINGS_BUILDER(a)};
    Slice b = sbytes(t);
    StringsReader rd;

    key(&s, "str");
    put_q(&s, regexp_string(re));
    nl(&s);
    key(&s, "num");
    put_int(&s, regexp_num_subexp(re));
    nl(&s);
    Slice names = regexp_subexp_names(re);
    key(&s, "names");
    put_strs(&s, names);
    nl(&s);
    key(&s, "index");
    put(&s, "[");
    for (Int i = 0; i <= names.len; i++) {
        if (i > 0)
            sp(&s);
        Str name = i < names.len ? ((Str *)names.p)[i] : BURROW_S("missing");
        put_int(&s, regexp_subexp_index(re, name));
    }
    put(&s, "]");
    nl(&s);
    key(&s, "prefix");
    put_q(&s, prefix);
    sp(&s);
    put_bool(&s, complete);
    nl(&s);

    key(&s, "m");
    put_bool(&s, regexp_match_string(re, t));
    sp(&s);
    put_bool(&s, regexp_match(re, b));
    sp(&s);
    put_bool(&s, regexp_match_reader(re, reader(&rd, t)));
    nl(&s);
    key(&s, "f");
    put_q(&s, regexp_find_string(re, t));
    sp(&s);
    put_bytes(&s, regexp_find(re, b));
    nl(&s);
    key(&s, "fi");
    put_ints(&s, regexp_find_string_index(re, a, t));
    sp(&s);
    put_ints(&s, regexp_find_index(re, a, b));
    sp(&s);
    put_ints(&s, regexp_find_reader_index(re, a, reader(&rd, t)));
    nl(&s);
    Slice si = regexp_find_string_submatch_index(re, a, t);
    key(&s, "si");
    put_ints(&s, si);
    sp(&s);
    put_ints(&s, regexp_find_submatch_index(re, a, b));
    sp(&s);
    put_ints(&s, regexp_find_reader_submatch_index(re, a, reader(&rd, t)));
    nl(&s);
    key(&s, "s");
    put_strs(&s, regexp_find_string_submatch(re, a, t));
    sp(&s);
    put_bytess(&s, regexp_find_submatch(re, a, b));
    nl(&s);
    key(&s, "a");
    put_strs(&s, regexp_find_all_string(re, a, t, -1));
    sp(&s);
    put_bytess(&s, regexp_find_all(re, a, b, -1));
    nl(&s);
    key(&s, "a2");
    put_strs(&s, regexp_find_all_string(re, a, t, 2));
    sp(&s);
    put_bytess(&s, regexp_find_all(re, a, b, 2));
    nl(&s);
    key(&s, "ai");
    put_intss(&s, regexp_find_all_string_index(re, a, t, -1));
    sp(&s);
    put_intss(&s, regexp_find_all_index(re, a, b, 2));
    nl(&s);
    key(&s, "as");
    put_strss(&s, regexp_find_all_string_submatch(re, a, t, -1));
    sp(&s);
    put_bytesss(&s, regexp_find_all_submatch(re, a, b, 3));
    nl(&s);
    key(&s, "asi");
    put_intss(&s, regexp_find_all_string_submatch_index(re, a, t, -1));
    sp(&s);
    put_intss(&s, regexp_find_all_submatch_index(re, a, b, 1));
    nl(&s);
    key(&s, "sp");
    put_strs(&s, regexp_split(re, a, t, n));
    sp(&s);
    put_strs(&s, regexp_split(re, a, t, 2));
    nl(&s);
    key(&s, "r");
    put_q(&s, regexp_replace_all_string(re, a, t, repl));
    sp(&s);
    put_bytes(&s, regexp_replace_all(re, a, b, sbytes(repl)));
    nl(&s);
    key(&s, "rl");
    put_q(&s, regexp_replace_all_literal_string(re, a, t, repl));
    sp(&s);
    put_bytes(&s, regexp_replace_all_literal(re, a, b, sbytes(repl)));
    nl(&s);
    key(&s, "rf");
    put_q(&s,
          regexp_replace_all_string_func(re, a, t, BURROW_FN(StrFunc, paren_str, a)));
    sp(&s);
    put_bytes(&s,
              regexp_replace_all_func(re, a, b, BURROW_FN(BytesFunc, paren_bytes, a)));
    nl(&s);
    if (si.p != NULL) {
        key(&s, "e");
        put_bytes(&s, regexp_expand_string(re, a, slice_nil(TYPE_BYTE), repl, t, si));
        sp(&s);
        Byte lt[1] = {'<'};
        Slice dst = slice_from(lt, 1, 1, TYPE_BYTE);
        put_bytes(&s, regexp_expand(re, a, dst, sbytes(repl), b, si));
        nl(&s);
    }
    key(&s, "q");
    put_q(&s, regexp_quote_meta(a, t));
    nl(&s);
    return strings_builder_string(&s.b);
}

/* ---------------------------------------------------------------- the cases */

static int failures;

/* The three ways a Regexp can be made to search: as compiled, with the Pike
 * VM for everything, and with the backtracker for everything it can take. */
enum { ENGINE_AS_IS, ENGINE_PIKE, ENGINE_BACKTRACK, ENGINE_COUNT };

static const char *const engine_names[] = {"as compiled", "pike", "backtrack"};

static void TestGoCases(TestingT *t) {
    Alloc *h = heap_allocator();
    size_t n = sizeof rx_gen_cases / sizeof *rx_gen_cases;
    failures = 0;
    size_t onepass = 0, backtrack = 0;
    for (size_t k = 0; k < n; k++) {
        const RxGenCase *c = &rx_gen_cases[k];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str pat = str_from_bytes((const Byte *)c->pat, c->pat_len);
        Str text = str_from_bytes((const Byte *)c->text, c->text_len);
        Str repl = str_from_bytes((const Byte *)c->repl, c->repl_len);
        Error err;
        Regexp *re = rx_compile(h, pat, c->flags, c->longest != 0, &err);
        if (re == NULL) {
            StringsBuilder b = STRINGS_BUILDER(a);
            strings_builder_write_string(&b, BURROW_S("err="), NULL);
            strings_builder_write_string(&b, error_text(err), NULL);
            strings_builder_write_byte(&b, '\n');
            Str got = strings_builder_string(&b);
            if (!text_eq(got, c->summary) && failures++ < 20)
                testing_t_errorf_v(t, "case %d %q flags %#x: %q, want %q", (int)k,
                                   c->pat, c->flags, cs(a, got),
                                   want_cs(a, c->summary));
            arena_free(&ar);
            continue;
        }
        RxProg *p = re->p;
        RxProg saved = *p;
        bool complete;
        Str prefix = regexp_literal_prefix(re, &complete);
        if (saved.onepass != NULL)
            onepass++;
        else if (saved.max_bitstate_len > text.len)
            backtrack++;
        for (int e = 0; e < ENGINE_COUNT; e++) {
            if (e != ENGINE_AS_IS && saved.onepass != NULL) {
                /* What compile would have set up with no one pass program:
                 * the prefix is the program's, which is empty when it is
                 * anchored, and not the one pass one. */
                p->onepass = NULL;
                p->prefix = syntax_prog_prefix(p->prog, a, &p->prefix_complete);
                p->prefix_rune = 0;
                if (p->prefix.len > 0) {
                    Int w;
                    p->prefix_rune = utf8_decode_rune_in_string(p->prefix, &w);
                }
            }
            if (e == ENGINE_PIKE)
                p->max_bitstate_len = 0;
            else if (e == ENGINE_BACKTRACK)
                p->max_bitstate_len = rx_max_bitstate_len(p->prog);
            Str got = summary(a, re, text, repl, c->n, prefix, complete);
            if (!text_eq(got, c->summary) && failures++ < 20)
                testing_t_errorf_v(
                    t, "case %d %q flags %#x longest %d text %q, %s:\n%s\nwant %s",
                    (int)k, c->pat, c->flags, c->longest, c->text, engine_names[e],
                    cs(a, got), want_cs(a, c->summary));
        }
        p->onepass = saved.onepass;
        p->max_bitstate_len = saved.max_bitstate_len;
        p->prefix = saved.prefix;
        p->prefix_complete = saved.prefix_complete;
        p->prefix_rune = saved.prefix_rune;
        regexp_free(re);
        arena_free(&ar);
    }
    CHECK(n > 10000);
    /* Enough of the cases go down each road to test it. */
    CHECK(onepass > 500);
    CHECK(backtrack > 3000);
}

/* TestCompileOnePass. */
static void TestCompileOnePass(TestingT *t) {
    Alloc *h = heap_allocator();
    for (size_t k = 0; k < sizeof rx_onepass_cases / sizeof *rx_onepass_cases; k++) {
        const RxOnePassCase *c = &rx_onepass_cases[k];
        Error err;
        Regexp *re =
            regexp_compile(h, str_from_bytes((const Byte *)c->re, c->len), &err);
        if (re == NULL) {
            testing_t_errorf_v(t, "%q: does not compile", c->re);
            continue;
        }
        if ((re->p->onepass != NULL) != (c->onepass != 0))
            testing_t_errorf_v(t, "%q: one pass %d, want %d", c->re,
                               re->p->onepass != NULL, c->onepass);
        regexp_free(re);
    }
    /* The long one, ^lo...ong$, is one pass too. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    strings_builder_write_string(&b, BURROW_S("^l"), NULL);
    for (int i = 0; i < 512; i++)
        strings_builder_write_byte(&b, 'o');
    strings_builder_write_string(&b, BURROW_S("ng$"), NULL);
    Str s = strings_builder_string(&b);
    Regexp *re = regexp_must_compile(h, s);
    CHECK(re->p->onepass != NULL);
    CHECK(regexp_match_string(re, str_from_bytes(s.p + 1, s.len - 2)));
    regexp_free(re);
    arena_free(&ar);
}

/* TestRunOnePass. */
static void TestRunOnePass(TestingT *t) {
    Regexp *re = regexp_must_compile(heap_allocator(), BURROW_S("^a(/b+(#c+)*)*$"));
    CHECK(re->p->onepass != NULL);
    CHECK(regexp_match_string(re, BURROW_S("a/b#c")));
    regexp_free(re);
}

/* TestOnePassCutoff: too many instructions to be worth it. */
static void TestOnePassCutoff(TestingT *t) {
    Regexp *re =
        regexp_must_compile(heap_allocator(), BURROW_S("^x{1,1000}y{1,1000}$"));
    CHECK(re->p->onepass == NULL);
    regexp_free(re);
}

/* TestMinInputLen. */
static void TestMinInputLen(TestingT *t) {
    for (size_t k = 0; k < sizeof rx_min_len_cases / sizeof *rx_min_len_cases; k++) {
        const RxMinLenCase *c = &rx_min_len_cases[k];
        Regexp *re = regexp_must_compile(heap_allocator(),
                                         str_from_bytes((const Byte *)c->re, c->len));
        if (re->p->min_input_len != c->min)
            testing_t_errorf_v(t, "%q: min input len %d, want %d", c->re,
                               re->p->min_input_len, c->min);
        regexp_free(re);
    }
}

/* TestSwitchBacktrack: the same Regexp, and so the same cached machines, used
 * by the Pike VM and then the backtracker, with no captures asked for. */
static void TestSwitchBacktrack(TestingT *t) {
    Alloc *h = heap_allocator();
    Regexp *re = regexp_must_compile(h, BURROW_S("a|b"));
    Int n = 256 * 1024 + 1;
    Byte *p = (Byte *)mem_alloc(h, (size_t)n, 1);
    CHECK(!regexp_match(re, slice_from(p, n, n, TYPE_BYTE)));
    p[0] = 'a';
    CHECK(regexp_match(re, slice_from(p, 1, 1, TYPE_BYTE)));
    CHECK(regexp_match(re, slice_from(p, n, n, TYPE_BYTE)));
    mem_free(h, p, (size_t)n, 1);
    regexp_free(re);
}

/* TestProgramTooLongForBacktrack. */
static void TestProgramTooLongForBacktrack(TestingT *t) {
    static const char *const words[] = {"one", "two",   "three", "four", "five",
                                        "six", "seven", "eight", "nine", "ten"};
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    strings_builder_write_byte(&b, '(');
    for (int i = 0; i < 100; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, '|');
        strings_builder_write_string(&b, str_from_cstr(words[i % 10]), NULL);
        char num[16];
        snprintf(num, sizeof num, "%d", i);
        strings_builder_write_string(&b, str_from_cstr(num), NULL);
    }
    strings_builder_write_string(&b, BURROW_S("|two)"), NULL);
    Regexp *re = regexp_must_compile(heap_allocator(), strings_builder_string(&b));
    CHECK(re->p->max_bitstate_len == 0);
    CHECK(regexp_match_string(re, BURROW_S("two")));
    CHECK(regexp_match_string(re, BURROW_S("xxsix55xx")));
    CHECK(!regexp_match_string(re, BURROW_S("xxx")));
    regexp_free(re);
    arena_free(&ar);
}

/* TestLongest. */
static void TestLongest(TestingT *t) {
    Regexp *re = regexp_must_compile(heap_allocator(), BURROW_S("a(|b)"));
    CHECK(str_eq(regexp_find_string(re, BURROW_S("ab")), BURROW_S("a")));
    regexp_longest(re);
    CHECK(str_eq(regexp_find_string(re, BURROW_S("ab")), BURROW_S("ab")));
    /* A copy keeps it. */
    Regexp *c = regexp_copy(re, heap_allocator());
    CHECK(str_eq(regexp_find_string(c, BURROW_S("ab")), BURROW_S("ab")));
    regexp_free(c);
    regexp_free(re);
}

/* TestCopyMatch and TestMatchFunction, over every compiling case. */
static void TestCopyAndMatchFunction(TestingT *t) {
    Alloc *h = heap_allocator();
    for (size_t k = 0; k < 2000; k++) {
        const RxGenCase *c = &rx_gen_cases[k];
        if (c->flags != SYNTAX_PERL || c->longest)
            continue;
        Str pat = str_from_bytes((const Byte *)c->pat, c->pat_len);
        Str text = str_from_bytes((const Byte *)c->text, c->text_len);
        Error err;
        Regexp *re = regexp_compile(h, pat, &err);
        if (re == NULL) {
            Error err2;
            CHECK(!regexp_match_string_pattern(h, pat, text, &err2));
            CHECK(str_eq(error_text(err), error_text(err2)));
            continue;
        }
        bool m = regexp_match_string(re, text);
        Regexp *cp = regexp_copy(re, h);
        CHECK(regexp_match_string(cp, text) == m);
        regexp_free(cp);
        CHECK(regexp_match_string_pattern(h, pat, text, &err) == m);
        CHECK(regexp_match_pattern(h, pat, sbytes(text), &err) == m);
        StringsReader rd;
        CHECK(regexp_match_reader_pattern(h, pat, reader(&rd, text), &err) == m);
        regexp_free(re);
    }
}

/* TestUnmarshalText. */
static void TestUnmarshalText(TestingT *t) {
    static const char *const good[] = {
        "", "a", "abc", "a.c", "a|b|c", "(a)*", "(?P<name>x)(y)", "[^\\n]", "\\pL+"};
    Alloc *h = heap_allocator();
    Regexp *un = regexp_must_compile(h, BURROW_S("unused"));
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        Str s = str_from_cstr(good[i]);
        Regexp *re = regexp_must_compile(h, s);
        regexp_longest(un);
        Error err;
        Slice m = regexp_marshal_text(re, h, &err);
        CHECK(str_eq(bstr(m), s));
        CHECK(BURROW_OK(regexp_unmarshal_text(un, h, m)));
        CHECK(str_eq(regexp_string(un), s));
        CHECK(!un->longest);
        sfree(h, m);

        Slice buf = slice_make(h, TYPE_BYTE, 4, 32);
        Slice ap = regexp_append_text(re, h, buf, &err);
        CHECK(ap.p == buf.p);
        CHECK(str_eq(bstr(slice_sub(ap, 4, ap.len)), s));
        CHECK(BURROW_OK(regexp_unmarshal_text(un, h, slice_sub(ap, 4, ap.len))));
        CHECK(str_eq(regexp_string(un), s));
        sfree(h, ap);
        regexp_free(re);
    }
    /* A bad pattern leaves it as it was. */
    Error err = regexp_unmarshal_text(un, h, sbytes(BURROW_S("\\")));
    CHECK(BURROW_FAILED(err));
    CHECK(str_eq(regexp_string(un), BURROW_S("\\pL+")));
    regexp_free(un);
}

/* TestBadCompile and must_compile's panic. */
static void TestBadCompile(TestingT *t) {
    Error err;
    CHECK(regexp_compile(heap_allocator(), BURROW_S("a**"), &err) == NULL);
    CHECK(str_eq(
        error_text(err),
        BURROW_S("error parsing regexp: invalid nested repetition operator: `**`")));
    CHECK(errors_as(err, TYPE_SYNTAX_ERROR) != NULL);
    CHECK(regexp_compile_posix(heap_allocator(), BURROW_S("\\d"), &err) == NULL);
    CHECK(str_eq(error_text(err),
                 BURROW_S("error parsing regexp: invalid escape sequence: `\\d`")));
}

/* Many threads on one Regexp, which shares its cached machines under a lock. */
typedef struct Worker {
    const Regexp *re;
    int bad;
} Worker;

static void worker(void *arg) {
    Worker *w = (Worker *)arg;
    Alloc *h = heap_allocator();
    for (int i = 0; i < 500; i++) {
        Str in = (i & 1) ? BURROW_S("xx foo123 bar45 yy")
                         : BURROW_S("foo1bar2baz3xxxxxxxxxxx");
        Slice all = regexp_find_all_string_submatch_index(w->re, h, in, -1);
        if ((i & 1) ? all.len != 2 : all.len != 3)
            w->bad++;
        for (Int j = 0; j < all.len; j++)
            sfree(h, ((Slice *)all.p)[j]);
        sfree(h, all);
        StringsReader rd;
        if (!regexp_match_reader(w->re, reader(&rd, in)))
            w->bad++;
    }
}

static void TestConcurrent(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);

    Regexp *re = regexp_must_compile(heap_allocator(), BURROW_S("([a-z]+)([0-9]+)"));
    enum { N = 8 };
    Worker w[N];
    burrow__Thread th[N];
    for (int i = 0; i < N; i++) {
        w[i].re = re;
        w[i].bad = 0;
        CHECK(burrow__thread_start(&th[i], worker, &w[i], 0));
    }
    for (int i = 0; i < N; i++) {
        CHECK(burrow__thread_join(&th[i]));
        CHECK_INT_EQ(w[i].bad, 0);
    }
    regexp_free(re);
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

/* Compiling and every function that allocates, with the allocator giving out
 * fewer and fewer blocks: each gives an error or the nil slice, never a
 * crash, and frees whatever it had got. */
static void TestNoMemory(TestingT *t) {
    Str pat = BURROW_S("(?P<word>[a-z]+)(\\d*)|x+y");
    Str in = BURROW_S("abc12 xxy de 9 fgh");
    int compiled = 0, found = 0;
    Arena ar; /* for what the replace function returns */
    arena_init(&ar, NULL, 0);
    for (int budget = 0; budget < 400; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Error err;
        Regexp *re = regexp_compile(&al, pat, &err);
        if (re == NULL) {
            CHECK(errors_is(err, burrow_err_out_of_memory));
            if (b.live != 0)
                testing_t_errorf_v(t, "compile, budget %d: %d bytes leaked", budget,
                                   (int)b.live);
            continue;
        }
        compiled++;
        long long base = b.live;
        int left = b.left;
        for (int f = 0; f < 8; f++) {
            b.left = left > 12 ? 12 : left;
            left = b.left;
            Slice s = slice_nil(TYPE_INT);
            Str str = BURROW_STR_EMPTY;
            switch (f) {
            case 0:
                s = regexp_find_all_string_submatch(re, &al, in, -1);
                if (s.p != NULL) {
                    for (Int i = 0; i < s.len; i++)
                        sfree(&al, ((Slice *)s.p)[i]);
                    sfree(&al, s);
                    found++;
                }
                break;
            case 1:
                s = regexp_find_all_string_index(re, &al, in, -1);
                if (s.p != NULL) {
                    for (Int i = 0; i < s.len; i++)
                        sfree(&al, ((Slice *)s.p)[i]);
                    sfree(&al, s);
                }
                break;
            case 2:
                s = regexp_split(re, &al, in, -1);
                sfree(&al, s);
                break;
            case 3:
                str = regexp_replace_all_string(re, &al, in, BURROW_S("<$word>"));
                mem_free(&al, (void *)(uintptr_t)str.p, (size_t)str.len, 1);
                break;
            case 4:
                s = regexp_find_submatch_index(re, &al, sbytes(in));
                sfree(&al, s);
                break;
            case 5:
                /* With nothing to quote it is in itself. */
                str = regexp_quote_meta(&al, BURROW_S("a.b"));
                if (str.len > 0)
                    mem_free(&al, (void *)(uintptr_t)str.p, (size_t)str.len, 1);
                break;
            case 6:
                s = regexp_marshal_text(re, &al, &err);
                sfree(&al, s);
                break;
            default:
                s = regexp_replace_all_func(
                    re, &al, sbytes(in),
                    BURROW_FN(BytesFunc, paren_bytes, arena_allocator(&ar)));
                sfree(&al, s);
                break;
            }
            /* The machines the searches cache come from the heap. */
            if (b.live != base)
                testing_t_errorf_v(t, "function %d, budget %d: %d bytes leaked", f,
                                   budget, (int)(b.live - base));
        }
        regexp_free(re);
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked", budget, (int)b.live);
    }
    arena_free(&ar);
    CHECK(compiled > 0 && found > 0);
}

#define TESTS(X)                                                                       \
    X(TestGoCases)                                                                     \
    X(TestCompileOnePass)                                                              \
    X(TestRunOnePass)                                                                  \
    X(TestOnePassCutoff)                                                               \
    X(TestMinInputLen)                                                                 \
    X(TestSwitchBacktrack)                                                             \
    X(TestProgramTooLongForBacktrack)                                                  \
    X(TestLongest)                                                                     \
    X(TestCopyAndMatchFunction)                                                        \
    X(TestUnmarshalText)                                                               \
    X(TestBadCompile)                                                                  \
    X(TestConcurrent)                                                                  \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
