/* Derived from golang.org/x/text v0.37.0's unicode/bidi tests, bidi_test.go.
 * That is the version Go 1.27.1 vendors.
 *
 * The rest of the test data comes from tests/xtext_bidi_test_gen.h, which
 * tools/gen-xtext-bidi.sh writes from the Go package itself. It holds hashes of
 * what Go's core says about random runs of classes, of what the API says about
 * a corpus of strings and of what Lookup says about every rune.
 * TestCoreDifferential, TestDifferential and TestSweep work out the same
 * records here and compare. describe_core, describe and sweep_record below have
 * to stay in step with the generator. Set BURROW_XTEXT_SHOW to see the first
 * record that differs.
 *
 * TestBidiCore, TestBidiCharacters and TestTables are left out, as they read
 * BidiTest.txt, BidiCharacterTest.txt and other UCD files from the network.
 * The sweep and the core cases cover the same ground.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xtext/bidi.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BidiCoreCase {
    Int t, nt, p, np, v, nv, b, nb;
    bool nil_values;
    int level;
    uint64_t hash;
} BidiCoreCase;

typedef struct BidiCorpus {
    const char *s;
    Int len;
    uint64_t hash;
} BidiCorpus;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "xtext_bidi_test_gen.h"

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Str lit(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static Slice bytes_of(const void *p, Int len) {
    Slice s = {(void *)(uintptr_t)p, len, len, TYPE_BYTE};
    return s;
}

/* -------------------------------------------------------------- bidi_test.go */

typedef struct RunInformation {
    const char *str;
    BidiDirection dir;
    Int start, end;
} RunInformation;

/* The part of each of Go's paragraph tests that is the same: set the string,
 * order it, and compare the runs. */
static void check_runs(TestingT *t, const char *str, bool ltr,
                       const RunInformation *want, Int nwant) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, a);
    Error err;
    burrow__bidi_paragraph_set_string(&p, lit(str), NULL, 0, &err);
    BidiOrdering order = burrow__bidi_paragraph_order(&p, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Order: %v", err);
        return;
    }
    if (burrow__bidi_paragraph_is_left_to_right(&p) != ltr)
        testing_t_errorf_v(t, "p.IsLeftToRight() == %t; want %t", !ltr, ltr);
    if (burrow__bidi_ordering_num_runs(&order) != nwant)
        testing_t_errorf_v(t, "order.NumRuns() = %d; want %d",
                           burrow__bidi_ordering_num_runs(&order), nwant);
    for (Int i = 0; i < nwant && i < burrow__bidi_ordering_num_runs(&order); i++) {
        BidiRun r = burrow__bidi_ordering_run(&order, i);
        Str got = burrow__bidi_run_string(a, &r);
        if (!str_eq(got, lit(want[i].str)))
            testing_t_errorf_v(t, "Run(%d) = %q; want %q", i, got, lit(want[i].str));
        Int s = 0, e = 0;
        burrow__bidi_run_pos(&r, &s, &e);
        if (s != want[i].start || e != want[i].end)
            testing_t_errorf_v(
                t, "Run(%d).start = %d, .end = %d; want start = %d, end = %d", i, s, e,
                want[i].start, want[i].end);
        if (burrow__bidi_run_direction(&r) != want[i].dir)
            testing_t_errorf_v(t, "Run(%d).Direction = %d; want %d", i,
                               burrow__bidi_run_direction(&r), want[i].dir);
    }
    arena_free(&ar);
}

static void TestSimple(TestingT *t) {
    static const RunInformation want[] = {
        {"Hell\xC3\xB6", BIDI_LEFT_TO_RIGHT, 0, 4},
    };
    check_runs(t, "Hell\xC3\xB6", true, want, LEN(want));
}

static void TestMixed(TestingT *t) {
    static const RunInformation want[] = {
        {"العاشر ليونيكود (", BIDI_RIGHT_TO_LEFT, 0, 16},
        {"Unicode Conference", BIDI_LEFT_TO_RIGHT, 17, 34},
        {")، الذي سيعقد في ", BIDI_RIGHT_TO_LEFT, 35, 51},
        {"10", BIDI_LEFT_TO_RIGHT, 52, 53},
        {"-", BIDI_RIGHT_TO_LEFT, 54, 54},
        {"12", BIDI_LEFT_TO_RIGHT, 55, 56},
        {" آذار ", BIDI_RIGHT_TO_LEFT, 57, 62},
        {"1997", BIDI_LEFT_TO_RIGHT, 63, 66},
        {" مبدينة", BIDI_RIGHT_TO_LEFT, 67, 73},
    };
    check_runs(
        t, "العاشر ليونيكود (Unicode Conference)، الذي سيعقد في 10-12 آذار 1997 مبدينة",
        false, want, LEN(want));
}

static void TestExplicitIsolate(TestingT *t) {
    /* https://www.w3.org/International/articles/inline-bidi-markup/uba-basics.en#beyond */
    /* The runs cut the isolates in two, which is the point here, so the strings
     * do not close what they open.
     * NOLINTBEGIN(misc-misleading-bidirectional) */
    static const RunInformation want[] = {
        {"The names of these states in Arabic are \xE2\x81\xA7", BIDI_LEFT_TO_RIGHT, 0,
         40},
        {"مصر", BIDI_RIGHT_TO_LEFT, 41, 43},
        {"\xE2\x81\xA9, \xE2\x81\xA7", BIDI_LEFT_TO_RIGHT, 44, 47},
        {"البحرين", BIDI_RIGHT_TO_LEFT, 48, 54},
        {"\xE2\x81\xA9 and \xE2\x81\xA7", BIDI_LEFT_TO_RIGHT, 55, 61},
        {"الكويت", BIDI_RIGHT_TO_LEFT, 62, 67},
        {"\xE2\x81\xA9 respectively.", BIDI_LEFT_TO_RIGHT, 68, 82},
    };
    /* NOLINTEND(misc-misleading-bidirectional) */
    check_runs(t,
               "The names of these states in Arabic are \xE2\x81\xA7مصر\xE2\x81\xA9, "
               "\xE2\x81\xA7البحرين\xE2\x81\xA9 and \xE2\x81\xA7الكويت\xE2\x81\xA9 "
               "respectively.",
               true, want, LEN(want));
}

static void TestWithoutExplicitIsolate(TestingT *t) {
    static const RunInformation want[] = {
        {"The names of these states in Arabic are ", BIDI_LEFT_TO_RIGHT, 0, 39},
        {"مصر, البحرين", BIDI_RIGHT_TO_LEFT, 40, 51},
        {" and ", BIDI_LEFT_TO_RIGHT, 52, 56},
        {"الكويت", BIDI_RIGHT_TO_LEFT, 57, 62},
        {" respectively.", BIDI_LEFT_TO_RIGHT, 63, 76},
    };
    check_runs(
        t,
        "The names of these states in Arabic are مصر, البحرين and الكويت respectively.",
        true, want, LEN(want));
}

static void TestLongUTF8(TestingT *t) {
    static const RunInformation want[] = {
        {"\U00020000", BIDI_LEFT_TO_RIGHT, 0, 0},
    };
    check_runs(t, "\U00020000", true, want, LEN(want));
}

static void TestLLongUTF8(TestingT *t) {
    static const struct {
        const char *str;
        Int l;
    } tests[] = {
        {"ö", 2},
        {"ॡ", 3},
        {"\U00020000", 4},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Int l = 0;
        burrow__bidi_lookup_string(lit(tests[i].str), &l);
        if (l != tests[i].l)
            testing_t_errorf_v(t, "LookupString(%q) length = %d; want %d",
                               lit(tests[i].str), l, tests[i].l);
    }
}

static void TestMixedSimple(TestingT *t) {
    static const RunInformation want[] = {
        {"U", BIDI_LEFT_TO_RIGHT, 0, 0},
        {"ا", BIDI_RIGHT_TO_LEFT, 1, 1},
    };
    check_runs(t, "Uا", true, want, LEN(want));
}

static void TestDefaultDirection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, arena_allocator(&ar));
    Error err;
    BidiOption rtl = burrow__bidi_default_direction(BIDI_RIGHT_TO_LEFT);
    burrow__bidi_paragraph_set_string(&p, lit("+"), &rtl, 1, &err);
    burrow__bidi_paragraph_order(&p, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    if (burrow__bidi_paragraph_is_left_to_right(&p))
        testing_t_errorf_v(t, "p.IsLeftToRight() = %t; want %t", true, false);
    BidiOption ltr = burrow__bidi_default_direction(BIDI_LEFT_TO_RIGHT);
    burrow__bidi_paragraph_set_string(&p, lit("+"), &ltr, 1, &err);
    burrow__bidi_paragraph_order(&p, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    if (!burrow__bidi_paragraph_is_left_to_right(&p))
        testing_t_errorf_v(t, "p.IsLeftToRight() = %t; want %t", false, true);
    arena_free(&ar);
}

static void TestEmpty(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, arena_allocator(&ar));
    Error err;
    burrow__bidi_paragraph_set_bytes(&p, bytes_of("", 0), NULL, 0, &err);
    BidiOrdering o = burrow__bidi_paragraph_order(&p, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "p.Order() return err != nil; want err == nil");
    if (burrow__bidi_ordering_num_runs(&o) != 0)
        testing_t_errorf_v(t, "o.NumRuns() = %d; want 0",
                           burrow__bidi_ordering_num_runs(&o));
    arena_free(&ar);
}

static void TestNewline(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, arena_allocator(&ar));
    Error err;
    Int n = burrow__bidi_paragraph_set_string(&p, lit("Hello\nworld"), NULL, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    /* 6 is the length up to and including the \n. */
    if (n != 6)
        testing_t_errorf_v(t, "SetString(%q) = nil, %d; want nil, %d",
                           lit("Hello\nworld"), n, (Int)6);
    arena_free(&ar);
}

static void TestDoubleSetString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, arena_allocator(&ar));
    Str str = lit("العاشر ليونيكود (Unicode Conference)،");
    Error err;
    burrow__bidi_paragraph_set_string(&p, str, NULL, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    burrow__bidi_paragraph_set_string(&p, str, NULL, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    burrow__bidi_paragraph_order(&p, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
}

static void TestReverseString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str got = burrow__bidi_reverse_string(arena_allocator(&ar), lit("(Hello)"));
    if (!str_eq(got, lit("(olleH)")))
        testing_t_errorf_v(t, "ReverseString(%s) = %q; want %q", lit("(Hello)"), got,
                           lit("(olleH)"));
    arena_free(&ar);
}

static void TestAppendReverse(TestingT *t) {
    static const struct {
        const char *in_string, *out_string, *want;
    } tests[] = {
        {"", "Hëllo", "Hëllo"},
        {"nice (wörld)", "", "(dlröw) ecin"},
        {"nice (wörld)", "Hëllo", "Hëllo(dlröw) ecin"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < LEN(tests); i++) {
        Str out = lit(tests[i].out_string), in = lit(tests[i].in_string);
        Slice r = burrow__bidi_append_reverse(
            arena_allocator(&ar), bytes_of(out.p, out.len), bytes_of(in.p, in.len));
        Str got = str_from_bytes(r.p, r.len);
        if (!str_eq(got, lit(tests[i].want)))
            testing_t_errorf_v(t, "AppendReverse([]byte(%q), []byte(%q) = %q; want %q",
                               out, in, got, lit(tests[i].want));
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- differential */

/* A record is a run of tagged values, as the generator writes them, kept only
 * as its FNV-1a hash, with the start of it kept as text for showing. */
typedef struct Rec {
    uint64_t h;
    Byte show[4096];
    Int show_len;
} Rec;

static void rec_init(Rec *r) {
    r->h = 14695981039346656037ULL;
    r->show_len = 0;
}

static void rec_write(Rec *r, const void *p, Int n) {
    const Byte *b = p;
    for (Int i = 0; i < n; i++) {
        r->h ^= b[i];
        r->h *= 1099511628211ULL;
    }
    Int room = (Int)sizeof r->show - r->show_len;
    Int k = n < room ? n : room;
    if (k > 0) {
        memcpy(r->show + r->show_len, b, (size_t)k);
        r->show_len += k;
    }
}

static void rec_cstr(Rec *r, const char *s) {
    rec_write(r, s, (Int)strlen(s));
}

static void rec_int(Rec *r, Int n) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0)
        tmp[--i] = '-';
    rec_write(r, tmp + i, (Int)sizeof tmp - i);
}

static void rec_n(Rec *r, const char *tag, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ";");
}

static void rec_bool(Rec *r, const char *tag, bool b) {
    rec_n(r, tag, b ? 1 : 0);
}

static void rec_s(Rec *r, const char *tag, const void *p, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ":");
    rec_write(r, p, n);
}

static void rec_e(Rec *r, const char *tag, Error err) {
    rec_cstr(r, tag);
    if (!BURROW_FAILED(err)) {
        rec_cstr(r, "-;");
        return;
    }
    Str s = error_text(err);
    rec_write(r, s.p, s.len);
    rec_cstr(r, ";");
}

typedef struct Desc {
    Rec *r;
    Alloc *a;
    /* The core case. */
    const BidiCoreCase *c;
    BidiCore *core;
    const Int *breaks;
    /* The string and the paragraph. */
    Str s;
    BidiParagraph *p;
    const BidiOption *opts;
    Int nopts;
    Int pos;
    Int line[2];
    Int i;
    BidiOrdering *o;
} Desc;

/* catch runs op and records the panic, if it panics. */
static void catch (Desc *d, void (*op)(Desc *d)) {
    BURROW_TRY {
        op(d);
    }
    BURROW_CATCH(p) {
        rec_cstr(d->r, "PANIC");
        Str s = panic_text(p);
        rec_write(d->r, s.p, s.len);
        rec_cstr(d->r, ";");
    }
    BURROW_TRY_END;
}

static void op_core_levels(Desc *d) {
    int8_t *levels = burrow__bidi_core_levels(d->core, d->breaks, d->c->nb);
    for (Int i = 0; i < burrow__bidi_core_len(d->core); i++)
        rec_n(d->r, "", levels[i]);
}

static void op_core_reordering(Desc *d) {
    Int *order = burrow__bidi_core_reordering(d->core, d->breaks, d->c->nb);
    for (Int i = 0; i < burrow__bidi_core_len(d->core); i++)
        rec_n(d->r, "O", order[i]);
}

static void op_core(Desc *d) {
    const BidiCoreCase *c = d->c;
    BidiClass *types = mem_alloc_array(d->a, (size_t)(c->nt > 0 ? c->nt : 1),
                                       sizeof(BidiClass), _Alignof(BidiClass));
    for (Int i = 0; i < c->nt; i++)
        types[i] = bidi_core_types[c->t + i];
    Rune *values = NULL;
    if (!c->nil_values) {
        values = mem_alloc_array(d->a, (size_t)(c->nv > 0 ? c->nv : 1), sizeof(Rune),
                                 _Alignof(Rune));
        for (Int i = 0; i < c->nv; i++)
            values[i] = bidi_core_pair_values[c->v + i];
    }
    Error err;
    d->core = burrow__bidi_new_core(d->a, types, c->nt, bidi_core_pair_types + c->p,
                                    c->np, values, c->nil_values ? 0 : c->nv,
                                    (int8_t)c->level, &err);
    rec_e(d->r, "E", err);
    if (BURROW_FAILED(err))
        return;
    d->breaks = c->nb > 0 ? bidi_core_breaks + c->b : NULL;
    rec_n(d->r, "L", burrow__bidi_core_embedding_level(d->core));
    rec_n(d->r, "N", burrow__bidi_core_len(d->core));
    catch (d, op_core_levels);
    catch (d, op_core_reordering);
}

static void show(TestingT *t, const Rec *r) {
    if (getenv("BURROW_XTEXT_SHOW") != NULL)
        testing_t_logf_v(t, "record starts %q", str_from_bytes(r->show, r->show_len));
}

static void TestCoreDifferential(TestingT *t) {
    int shown = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Rec *r = mem_alloc(arena_allocator(&ar), sizeof *r, _Alignof(Rec));
    Arena car;
    arena_init(&car, NULL, 0);
    for (Int i = 0; i < LEN(bidi_core_cases); i++) {
        Desc d = {0};
        d.r = r;
        d.a = arena_allocator(&car);
        d.c = &bidi_core_cases[i];
        rec_init(r);
        catch (&d, op_core);
        arena_reset(&car);
        if (r->h != bidi_core_cases[i].hash) {
            testing_t_errorf_v(t, "core case %d: differs from Go", i);
            if (shown++ == 0)
                show(t, r);
        }
    }
    arena_free(&car);
    arena_free(&ar);
}

static void describe_run(Desc *d, const BidiRun *run) {
    Str s = burrow__bidi_run_string(d->a, run);
    rec_s(d->r, "", s.p, s.len);
    Slice b = burrow__bidi_run_bytes(d->a, run);
    rec_s(d->r, "", b.p, b.len);
    Int start = 0, end = 0;
    burrow__bidi_run_pos(run, &start, &end);
    rec_n(d->r, "", start);
    rec_n(d->r, "", end);
    rec_n(d->r, "", burrow__bidi_run_direction(run));
}

static void op_ordering_direction(Desc *d) {
    rec_n(d->r, "OD", burrow__bidi_ordering_direction(d->o));
}

static void describe_ordering(Desc *d, BidiOrdering *o) {
    rec_n(d->r, "R", burrow__bidi_ordering_num_runs(o));
    d->o = o;
    catch (d, op_ordering_direction);
    for (Int i = 0; i < burrow__bidi_ordering_num_runs(o); i++) {
        BidiRun run = burrow__bidi_ordering_run(o, i);
        describe_run(d, &run);
    }
}

static void op_order(Desc *d) {
    Error err;
    BidiOrdering o = burrow__bidi_paragraph_order(d->p, &err);
    rec_e(d->r, "O", err);
    describe_ordering(d, &o);
}

static void op_direction(Desc *d) {
    rec_n(d->r, "D", burrow__bidi_paragraph_direction(d->p));
}

static void op_is_ltr(Desc *d) {
    rec_bool(d->r, "LTR", burrow__bidi_paragraph_is_left_to_right(d->p));
}

static void op_run_at(Desc *d) {
    BidiRun run = burrow__bidi_paragraph_run_at(d->p, d->pos);
    describe_run(d, &run);
}

static void describe_order(Desc *d) {
    catch (d, op_order);
    catch (d, op_direction);
    catch (d, op_is_ltr);
    Int nr = d->p->nrunes;
    const Int pos[] = {-1, 0, 1, 2, nr - 1, nr, nr + 1};
    for (Int i = 0; i < LEN(pos); i++) {
        d->pos = pos[i];
        catch (d, op_run_at);
    }
}

static void op_set_string(Desc *d) {
    Error err;
    Int n = burrow__bidi_paragraph_set_string(d->p, d->s, d->opts, d->nopts, &err);
    rec_n(d->r, "N", n);
    rec_e(d->r, "", err);
}

static void op_line(Desc *d) {
    Error err;
    BidiOrdering o = burrow__bidi_paragraph_line(d->p, d->line[0], d->line[1], &err);
    rec_e(d->r, "L", err);
    describe_ordering(d, &o);
}

static void op_set_bytes(Desc *d) {
    Error err;
    Int n = burrow__bidi_paragraph_set_bytes(d->p, bytes_of(d->s.p, d->s.len), NULL, 0,
                                             &err);
    rec_n(d->r, "NB", n);
    rec_e(d->r, "", err);
}

static void op_set_string_quiet(Desc *d) {
    Error err;
    burrow__bidi_paragraph_set_string(d->p, d->s, d->opts, d->nopts, &err);
}

static void op_append_reverse(Desc *d) {
    Slice r = burrow__bidi_append_reverse(d->a, slice_nil(TYPE_BYTE),
                                          bytes_of(d->s.p, d->s.len));
    rec_s(d->r, "AR", r.p, r.len);
}

static void op_append_reverse_hello(Desc *d) {
    Byte *out = mem_alloc_nozero(d->a, 6, 1);
    memcpy(out, "H\xC3\xABllo", 6);
    Slice r =
        burrow__bidi_append_reverse(d->a, bytes_of(out, 6), bytes_of(d->s.p, d->s.len));
    rec_s(d->r, "AR", r.p, r.len);
}

static void op_reverse_string(Desc *d) {
    Str r = burrow__bidi_reverse_string(d->a, d->s);
    rec_s(d->r, "RS", r.p, r.len);
}

/* s.p + i, except for the empty string, whose p can be NULL. */
static const Byte *tail_of(Str s, Int i) {
    return s.p == NULL ? NULL : s.p + i;
}

static void op_lookup(Desc *d) {
    Int sz = 0;
    BidiProperties p =
        burrow__bidi_lookup(bytes_of(tail_of(d->s, d->i), d->s.len - d->i), &sz);
    rec_n(d->r, "K", (Int)burrow__bidi_class(p));
    rec_n(d->r, "", sz);
    rec_bool(d->r, "", burrow__bidi_is_bracket(p));
    rec_bool(d->r, "", burrow__bidi_is_opening_bracket(p));
}

static void op_lookup_string(Desc *d) {
    Int sz = 0;
    BidiProperties p = burrow__bidi_lookup_string(
        str_from_bytes(tail_of(d->s, d->i), d->s.len - d->i), &sz);
    rec_n(d->r, "KS", (Int)burrow__bidi_class(p));
    rec_n(d->r, "", sz);
}

static void describe(Rec *r, Alloc *a, Str s) {
    BidiOption opt_sets[5][2];
    Int nopt_sets[5] = {0, 1, 1, 2, 2};
    opt_sets[1][0] = burrow__bidi_default_direction(BIDI_LEFT_TO_RIGHT);
    opt_sets[2][0] = burrow__bidi_default_direction(BIDI_RIGHT_TO_LEFT);
    opt_sets[3][0] = burrow__bidi_default_direction(BIDI_RIGHT_TO_LEFT);
    opt_sets[3][1] = burrow__bidi_default_direction(BIDI_LEFT_TO_RIGHT);
    opt_sets[4][0] = burrow__bidi_default_direction(BIDI_LEFT_TO_RIGHT);
    opt_sets[4][1] = burrow__bidi_default_direction(BIDI_RIGHT_TO_LEFT);

    Desc d = {0};
    d.r = r;
    d.a = a;
    d.s = s;
    for (Int k = 0; k < 5; k++) {
        BidiParagraph p;
        burrow__bidi_paragraph_init(&p, a);
        d.p = &p;
        d.opts = opt_sets[k];
        d.nopts = nopt_sets[k];
        catch (&d, op_set_string);
        describe_order(&d);
        Int nt = p.ntypes, cap = p.types_cap, nr = p.nrunes;
        const Int lines[][2] = {
            {0, nt},  {0, 1}, {1, nt},  {nt / 2, nt}, {0, nt + 1}, {0, nt + 5},
            {nt, nt}, {2, 1}, {0, cap}, {0, cap + 1}, {1, nr + 1}, {0, nr},
        };
        for (Int i = 0; i < LEN(lines); i++) {
            d.line[0] = lines[i][0];
            d.line[1] = lines[i][1];
            catch (&d, op_line);
        }
    }

    /* SetBytes, and the options carrying over from one Order to the next. */
    BidiParagraph p;
    burrow__bidi_paragraph_init(&p, a);
    d.p = &p;
    catch (&d, op_set_bytes);
    describe_order(&d);
    BidiOption rtl = burrow__bidi_default_direction(BIDI_RIGHT_TO_LEFT);
    d.opts = &rtl;
    d.nopts = 1;
    catch (&d, op_set_string_quiet);
    describe_order(&d);
    d.opts = NULL;
    d.nopts = 0;
    catch (&d, op_set_string_quiet);
    describe_order(&d);
    Str saved = d.s;
    d.s = str_from_bytes("", 0);
    catch (&d, op_set_string_quiet);
    describe_order(&d);
    d.s = saved;

    catch (&d, op_append_reverse);
    catch (&d, op_append_reverse_hello);
    catch (&d, op_reverse_string);

    for (d.i = 0; d.i <= s.len; d.i++) {
        catch (&d, op_lookup);
        catch (&d, op_lookup_string);
    }
}

static void TestDifferential(TestingT *t) {
    int shown = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Rec *r = mem_alloc(arena_allocator(&ar), sizeof *r, _Alignof(Rec));
    Arena sar;
    arena_init(&sar, NULL, 0);
    for (Int i = 0; i < LEN(bidi_corpus); i++) {
        Str s = str_from_bytes(bidi_corpus[i].s, bidi_corpus[i].len);
        rec_init(r);
        describe(r, arena_allocator(&sar), s);
        arena_reset(&sar);
        if (r->h != bidi_corpus[i].hash) {
            testing_t_errorf_v(t, "corpus %d %+q: differs from Go", i, s);
            if (shown++ == 0)
                show(t, r);
        }
    }
    arena_free(&sar);
    arena_free(&ar);
}

/* -------------------------------------------------------------------- sweep */

static void sweep_record(Rec *rec, Rune r) {
    Int sz = 0;
    BidiProperties p = burrow__bidi_lookup_rune(r, &sz);
    rec_n(rec, "P", (Int)burrow__bidi_class(p));
    rec_n(rec, "", sz);
    rec_bool(rec, "", burrow__bidi_is_bracket(p));
    rec_bool(rec, "", burrow__bidi_is_opening_bracket(p));
    if (burrow__bidi_is_bracket(p))
        rec_n(rec, "", burrow__bidi_reverse_bracket(p, r));
}

static void TestSweep(TestingT *t) {
    Rec rec;
    for (Rune blk = 0; blk < 0x110; blk++) {
        rec_init(&rec);
        for (Rune r = blk << 12; r < (blk + 1) << 12; r++) {
            sweep_record(&rec, r);
            rec.show_len = 0;
        }
        if (rec.h != bidi_sweep_hash[blk])
            testing_t_errorf_v(t, "block %X: differs from Go", blk);
    }
}

#define TESTS(X)                                                                       \
    X(TestSimple)                                                                      \
    X(TestMixed)                                                                       \
    X(TestExplicitIsolate)                                                             \
    X(TestWithoutExplicitIsolate)                                                      \
    X(TestLongUTF8)                                                                    \
    X(TestLLongUTF8)                                                                   \
    X(TestMixedSimple)                                                                 \
    X(TestDefaultDirection)                                                            \
    X(TestEmpty)                                                                       \
    X(TestNewline)                                                                     \
    X(TestDoubleSetString)                                                             \
    X(TestReverseString)                                                               \
    X(TestAppendReverse)                                                               \
    X(TestCoreDifferential)                                                            \
    X(TestDifferential)                                                                \
    X(TestSweep)

TESTING_MAIN(TESTS)
