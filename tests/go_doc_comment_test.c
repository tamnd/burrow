/* Derived from Go's src/go/doc/comment/testdata_test.go, old_test.go,
 * wrap_test.go, std_test.go and parse_test.go.
 * Go source: go1.27.1.
 *
 * TestTestdata reads its archives from tests/go_doc_comment_test_gen.h, where
 * they are already taken apart, with the dollar signs stripped. TestStd runs
 * go list std in Go; here the list comes from the same header, which worked it
 * out from a Go source tree and checked it against Go's own list.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/doc/comment.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_doc_comment_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* The test hooks of src/go/doc_comment.c and src/go/doc_comment_print.c. */
bool burrow__comment_is_old_heading(Str line, const Str *all, Int nall, Int off);
bool burrow__comment_auto_url(Str s, Str *url);
bool burrow__comment_ident(Str s, Str *id);
Int burrow__comment_std_pkgs(const Str **list);
Slice burrow__comment_wrap(Alloc *a, Slice words, Int max);
int64_t burrow__comment_wrap_penalty(Str s);

static Str piece(GdtPiece p) {
    return str_from_bytes((const Byte *)p.p, (Int)p.n);
}

static Str bytes_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* ------------------------------------------------------------------ dump */

static void dump_str(StringsBuilder *out, Str s) {
    (void)strings_builder_write_string(out, s, NULL);
}

static void dump_nl(StringsBuilder *out, int n) {
    (void)strings_builder_write_byte(out, '\n');
    for (int i = 0; i < n; i++)
        (void)strings_builder_write_byte(out, '\t');
}

static void dump_q(StringsBuilder *out, Str s) {
    dump_str(out, strconv_quote(out->a, s));
}

static Str bool_str(bool b) {
    return b ? S("true") : S("false");
}

/* The string case of Go's dumpTo: each line quoted on a line of its own. */
static void dump_lines(StringsBuilder *out, int indent, Str x) {
    while (x.len > 0) {
        Int i = strings_index(x, S("\n"));
        Int n = i < 0 ? x.len : i + 1;
        dump_nl(out, indent);
        dump_q(out, str_from_bytes(x.p, n));
        x = str_from_bytes(x.p + n, x.len - n);
    }
}

static void dump_texts(StringsBuilder *out, int indent, Slice x);
static void dump_blocks(StringsBuilder *out, int indent, Slice x);

static void dump_string_node(StringsBuilder *out, int indent, Str name, Str s) {
    dump_str(out, name);
    if (!strings_contains(s, S("\n"))) {
        dump_str(out, S(" "));
        dump_q(out, s);
    } else {
        dump_lines(out, indent + 1, s);
    }
}

static void dump_text(StringsBuilder *out, int indent, CommentText x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_PLAIN:
        dump_string_node(out, indent, S("Plain"), ((CommentPlain *)x)->text);
        break;
    case COMMENT_KIND_ITALIC:
        dump_string_node(out, indent, S("Italic"), ((CommentItalic *)x)->text);
        break;
    case COMMENT_KIND_LINK: {
        CommentLink *l = (CommentLink *)x;
        dump_str(out, S("Link "));
        dump_q(out, l->url);
        dump_texts(out, indent + 1, l->text);
        break;
    }
    case COMMENT_KIND_DOC_LINK: {
        CommentDocLink *l = (CommentDocLink *)x;
        dump_str(out, S("DocLink pkg:"));
        dump_q(out, l->import_path);
        dump_str(out, S(", recv:"));
        dump_q(out, l->recv);
        dump_str(out, S(", name:"));
        dump_q(out, l->name);
        dump_texts(out, indent + 1, l->text);
        break;
    }
    default:
        dump_str(out, S("?text"));
        break;
    }
}

static void dump_texts(StringsBuilder *out, int indent, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        dump_nl(out, indent);
        dump_text(out, indent, BURROW_AT(CommentText, x, i));
    }
}

static void dump_block(StringsBuilder *out, int indent, CommentBlock x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_HEADING:
        dump_str(out, S("Heading"));
        dump_texts(out, indent + 1, ((CommentHeading *)x)->text);
        break;
    case COMMENT_KIND_LIST: {
        CommentList *l = (CommentList *)x;
        dump_str(out, S("List ForceBlankBefore="));
        dump_str(out, bool_str(l->force_blank_before));
        dump_str(out, S(" ForceBlankBetween="));
        dump_str(out, bool_str(l->force_blank_between));
        for (Int i = 0; i < l->items.len; i++) {
            CommentListItem *item = BURROW_AT(CommentListItem *, l->items, i);
            dump_nl(out, indent + 1);
            dump_str(out, S("Item Number="));
            dump_q(out, item->number);
            dump_blocks(out, indent + 2, item->content);
        }
        break;
    }
    case COMMENT_KIND_PARAGRAPH:
        dump_str(out, S("Paragraph"));
        dump_texts(out, indent + 1, ((CommentParagraph *)x)->text);
        break;
    case COMMENT_KIND_CODE:
        dump_str(out, S("Code"));
        dump_lines(out, indent + 1, ((CommentCode *)x)->text);
        break;
    default:
        dump_str(out, S("?block"));
        break;
    }
}

static void dump_blocks(StringsBuilder *out, int indent, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        dump_nl(out, indent);
        dump_block(out, indent, BURROW_AT(CommentBlock, x, i));
    }
}

/* dump returns a dump of the doc, the way testdata_test.go's dump does. */
static Str dump(Alloc *a, CommentDoc *d) {
    StringsBuilder out = STRINGS_BUILDER(a);
    dump_str(&out, S("Doc"));
    dump_blocks(&out, 1, d->content);
    if (d->links.len > 0) {
        dump_nl(&out, 1);
        dump_str(&out, S("Links"));
        for (Int i = 0; i < d->links.len; i++) {
            CommentLinkDef *def = BURROW_AT(CommentLinkDef *, d->links, i);
            dump_nl(&out, 2);
            dump_str(&out, S("LinkDef Used:"));
            dump_str(&out, bool_str(def->used));
            dump_str(&out, S(" Text:"));
            dump_q(&out, def->text);
            dump_str(&out, S(" URL:"));
            dump_str(&out, def->url);
        }
    }
    dump_str(&out, S("\n"));
    return strings_builder_string(&out);
}

/* -------------------------------------------------------------- testdata */

static Str lookup_package(void *env, Str name, bool *ok) {
    (void)env;
    if (str_eq(name, S("comment"))) {
        *ok = true;
        return S("go/doc/comment");
    }
    return comment_default_lookup_package(name, ok);
}

static bool lookup_sym(void *env, Str recv, Str name) {
    (void)env;
    return (str_eq(recv, S("Parser")) && str_eq(name, S("Parse"))) ||
           (recv.len == 0 && str_eq(name, S("Doc"))) ||
           (recv.len == 0 && str_eq(name, S("NoURL")));
}

static void run_testdata(void *env, TestingT *t) {
    const GdtCase *tc = (const GdtCase *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    CommentParser p = {0};
    p.words = map_make(a, TYPE_STRING, TYPE_STRING, 2);
    Str italic = S("italicword"), linked = S("linkedword");
    Str none = BURROW_STR_EMPTY, url = S("https://example.com/linkedword");
    if (p.words == NULL || !map_set(p.words, &italic, &none) ||
        !map_set(p.words, &linked, &url)) {
        testing_t_fatalf_v(t, "out of memory");
        arena_free(&ar);
        return;
    }
    p.lookup_package = BURROW_FN(CommentLookupPackageFunc, lookup_package, NULL);
    p.lookup_sym = BURROW_FN(CommentLookupSymFunc, lookup_sym, NULL);

    CommentPrinter pr = {0};
    pr.heading_level = (Int)tc->heading_level;
    pr.doc_link_base_url = piece(tc->doc_link_base_url);
    pr.text_prefix = piece(tc->text_prefix);
    pr.text_code_prefix = piece(tc->text_code_prefix);
    pr.text_width = (Int)tc->text_width;

    CommentDoc *d = comment_parser_parse(&p, a, piece(tc->input));
    for (int i = 0; i < tc->nouts; i++) {
        const GdtOut *f = &tc->outs[i];
        Str name = str_from_cstr(f->name);
        Str want = piece(f->want);
        Str out = BURROW_STR_EMPTY;
        if (str_eq(name, S("dump")))
            out = dump(a, d);
        else if (str_eq(name, S("gofmt")))
            out = bytes_str(comment_printer_comment(&pr, a, d));
        else if (str_eq(name, S("html")))
            out = bytes_str(comment_printer_html(&pr, a, d));
        else if (str_eq(name, S("markdown")))
            out = bytes_str(comment_printer_markdown(&pr, a, d));
        else if (str_eq(name, S("text")))
            out = bytes_str(comment_printer_text(&pr, a, d));
        else {
            testing_t_fatalf_v(t, "unknown output file %q", name);
            break;
        }
        if (!str_eq(out, want))
            testing_t_errorf_v(t, "testdata/%s: %s:\n%s\nhave:\n%s",
                               str_from_cstr(tc->name), name, want, out);
    }
    arena_free(&ar);
}

static void TestTestdata(TestingT *t) {
    if (NELEM(gdt_cases) == 0) {
        testing_t_fatalf_v(t, "no testdata");
        return;
    }
    for (Int i = 0; i < NELEM(gdt_cases); i++) {
        const GdtCase *tc = &gdt_cases[i];
        testing_t_run(t, str_from_cstr(tc->name),
                      BURROW_FN(TestingTFunc, run_testdata, (void *)(uintptr_t)tc));
    }
}

/* -------------------------------------------------------------------- old */

static const struct {
    const char *line;
    bool ok;
} old_heading_tests[] = {
    {"Section", true},
    {"A typical usage", true},
    {"\316\224\316\233\316\236 is Greek", true},
    {"Foo 42", true},
    {"", false},
    {"section", false},
    {"A typical usage:", false},
    {"This code:", false},
    {"\316\264 is Greek", false},
    {"Foo \302\247", false},
    {"Fermat's Last Sentence", true},
    {"Fermat's", true},
    {"'sX", false},
    {"Ted 'Too' Bar", false},
    {"Use n+m", false},
    {"Scanning:", false},
    {"N:M", false},
};

static void TestIsOldHeading(TestingT *t) {
    for (Int i = 0; i < NELEM(old_heading_tests); i++) {
        Str line = str_from_cstr(old_heading_tests[i].line);
        bool ok = old_heading_tests[i].ok;
        Str all[] = {S("Text."), BURROW_STR_EMPTY, line, BURROW_STR_EMPTY, S("Text.")};
        if (burrow__comment_is_old_heading(line, all, NELEM(all), 2) != ok)
            testing_t_errorf_v(t, "isOldHeading(%q) = %v, want %v", line, !ok, ok);
    }
}

static const struct {
    const char *in;
    const char *out;
} auto_url_tests[] = {
    {"", ""},
    {"http://[::1]:8080/foo.txt", "http://[::1]:8080/foo.txt"},
    {"https://www.google.com) after", "https://www.google.com"},
    {"https://www.google.com:30/x/y/z:b::c. After",
     "https://www.google.com:30/x/y/z:b::c"},
    {"http://www.google.com/path/:;!-/?query=%34b#093124",
     "http://www.google.com/path/:;!-/?query=%34b#093124"},
    {"http://www.google.com/path/:;!-/?query=%34bar#093124",
     "http://www.google.com/path/:;!-/?query=%34bar#093124"},
    {"http://www.google.com/index.html! After", "http://www.google.com/index.html"},
    {"http://www.google.com/", "http://www.google.com/"},
    {"https://www.google.com/", "https://www.google.com/"},
    {"http://www.google.com/path.", "http://www.google.com/path"},
    {"http://en.wikipedia.org/wiki/Camellia_(cipher)",
     "http://en.wikipedia.org/wiki/Camellia_(cipher)"},
    {"http://www.google.com/)", "http://www.google.com/"},
    {"http://gmail.com)", "http://gmail.com"},
    {"http://gmail.com))", "http://gmail.com"},
    {"http://gmail.com ((http://gmail.com)) ()", "http://gmail.com"},
    {"http://example.com/ quux!", "http://example.com/"},
    {"http://example.com/%2f/ /world.", "http://example.com/%2f/"},
    {"http: ipsum //host/path", ""},
    {"javascript://is/not/linked", ""},
    {"http://foo", "http://foo"},
    {"https://www.example.com/person/][Person Name]]",
     "https://www.example.com/person/"},
    {"http://golang.org/)", "http://golang.org/"},
    {"http://golang.org/hello())", "http://golang.org/hello()"},
    {"http://git.qemu.org/?p=qemu.git;a=blob;f=qapi-schema.json;hb=HEAD",
     "http://git.qemu.org/?p=qemu.git;a=blob;f=qapi-schema.json;hb=HEAD"},
    /* inner ] causes (]) to be cut off from URL */
    {"https://foo.bar/bal/x(])", "https://foo.bar/bal/x"},
    {"http://bar(])", "http://bar"}, /* same */
};

static void TestAutoURL(TestingT *t) {
    for (Int i = 0; i < NELEM(auto_url_tests); i++) {
        Str in = str_from_cstr(auto_url_tests[i].in);
        Str want = str_from_cstr(auto_url_tests[i].out);
        Str url = BURROW_STR_EMPTY;
        bool ok = burrow__comment_auto_url(in, &url);
        if (!str_eq(url, want) || ok != (want.len != 0))
            testing_t_errorf_v(t, "autoURL(%q) = %q, %v, want %q, %v", in, url, ok,
                               want, want.len != 0);
    }
}

/* ------------------------------------------------------------------- wrap */

typedef struct WrapCase {
    const Str *words;
    Int n;
    Int max;
} WrapCase;

/* wrapSlow is an O(n^2) reference implementation for wrap. It returns a
 * minimal-score sequence along with the score. It is OK if wrap returns a
 * different sequence as long as that sequence has the same score. */
static int64_t wrap_slow(Alloc *a, const Str *words, Int n, Int max) {
    /* Quadratic dynamic programming algorithm for line wrapping problem.
     * best[i] tracks the best score possible for words[:i], assuming that for
     * i < len(words) the line breaks after those words. bestleft[i] tracks the
     * previous line break for best[i]. */
    int64_t *best =
        (int64_t *)mem_alloc(a, (size_t)(n + 1) * sizeof(int64_t), _Alignof(int64_t));
    if (best == NULL)
        return -1;
    best[0] = 0;
    for (Int i = 0; i < n; i++) {
        Str w = words[i];
        if (utf8_rune_count_in_string(w) >= max) {
            /* Overlong word must appear on line by itself. No choice. */
            best[i + 1] = best[i];
            continue;
        }
        best[i + 1] = (int64_t)1e18;
        int64_t p = burrow__comment_wrap_penalty(w);
        Int k = -1;
        for (Int j = i; j >= 0; j--) {
            k += 1 + utf8_rune_count_in_string(words[j]);
            if (k > max)
                break;
            int64_t line = (int64_t)(k - max) * (int64_t)(k - max) + p;
            if (i == n - 1)
                line = 0; /* no score for final line being too short */
            int64_t s = best[j] + line;
            if (best[i + 1] > s)
                best[i + 1] = s;
        }
    }
    return best[n];
}

static void run_wrap_max(void *env, TestingT *t) {
    const WrapCase *wc = (const WrapCase *)env;
    const Str *words = wc->words;
    Int n = wc->n, max = wc->max;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice seq = burrow__comment_wrap(
        a, slice_from((void *)(uintptr_t)words, n, n, TYPE_STRING), max);
    Int start = 0;
    int64_t score = 0;
    if (seq.len == 0) {
        testing_t_fatalf_v(t, "wrap seq is empty");
        arena_free(&ar);
        return;
    }
    if (BURROW_AT(Int, seq, 0) != 0) {
        testing_t_fatalf_v(t, "wrap seq does not start with 0");
        arena_free(&ar);
        return;
    }
    for (Int i = 1; i < seq.len; i++) {
        Int m = BURROW_AT(Int, seq, i);
        if (m <= start) {
            testing_t_fatalf_v(t, "wrap seq is non-increasing: %v", seq);
            arena_free(&ar);
            return;
        }
        if (m > n) {
            testing_t_fatalf_v(t, "wrap seq contains %d > %d: %v", m, n, seq);
            arena_free(&ar);
            return;
        }
        Int size = -1;
        for (Int j = start; j < m; j++)
            size += 1 + utf8_rune_count_in_string(words[j]);
        if (m - start == 1 && size >= max) {
            /* no score */
        } else if (size > max) {
            testing_t_fatalf_v(t, "wrap used overlong line %d:%d: %v", start, m,
                               slice_from((void *)(uintptr_t)(words + start), m - start,
                                          m - start, TYPE_STRING));
            arena_free(&ar);
            return;
        } else if (m != n) {
            score += (int64_t)(max - size) * (int64_t)(max - size) +
                     burrow__comment_wrap_penalty(words[m - 1]);
        }
        start = m;
    }
    if (start != n) {
        testing_t_fatalf_v(t, "wrap seq does not use all words (%d < %d): %v", start, n,
                           seq);
        arena_free(&ar);
        return;
    }
    int64_t slow = wrap_slow(a, words, n, max);
    if (score != slow)
        testing_t_fatalf_v(t, "wrap score = %d != wrapSlow score %d\nwrap: %v", score,
                           slow, seq);
    arena_free(&ar);
}

static void run_wrap_n(void *env, TestingT *t) {
    const WrapCase *wc = (const WrapCase *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_t_logf_v(
        t, "words: %v",
        slice_from((void *)(uintptr_t)wc->words, wc->n, wc->n, TYPE_STRING));
    for (Int max = 1; max < 100 && !testing_t_failed(t); max++) {
        WrapCase c = {wc->words, wc->n, max};
        testing_t_run(t, fmt_sprintf_v(a, "max=%d", max),
                      BURROW_FN(TestingTFunc, run_wrap_max, &c));
    }
    arena_free(&ar);
}

static void TestWrap(TestingT *t) {
    int64_t seed = time_unix_nano(time_now());
    testing_t_logf_v(t, "-wrapseed=%#x\n", seed);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, seed));
    if (r == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        arena_free(&ar);
        return;
    }
    /* Generate words of random length. */
    Str s = S("1234567890\316\261\316\262cdefghijklmnopqrstuvwxyz");
    Int sn = utf8_rune_count_in_string(s);
    Str words[100];
    for (Int i = 0; i < NELEM(words); i++) {
        Int n = 1 + math_rand_rand_intn(r, sn - 1);
        if (n >= 12)
            n++; /* extra byte for beta */
        if (n >= 11)
            n++; /* extra byte for alpha */
        words[i] = str_from_bytes(s.p, n);
    }

    /* Check that we compute correct optimal solutions to the problem. */
    for (Int n = 1; n <= NELEM(words) && !testing_t_failed(t); n++) {
        WrapCase c = {words, n, 0};
        testing_t_run(t, fmt_sprintf_v(a, "n=%d", n),
                      BURROW_FN(TestingTFunc, run_wrap_n, &c));
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------------- std */

static void TestStd(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Str *list = NULL;
    Int n = burrow__comment_std_pkgs(&list);
    StringsBuilder have = STRINGS_BUILDER(a);
    for (Int i = 0; i < n; i++) {
        (void)strings_builder_write_string(&have, list[i], NULL);
        (void)strings_builder_write_byte(&have, '\n');
    }
    StringsBuilder want = STRINGS_BUILDER(a);
    for (Int i = 0; i < NELEM(gdt_std); i++) {
        (void)strings_builder_write_string(&want, str_from_cstr(gdt_std[i]), NULL);
        (void)strings_builder_write_byte(&want, '\n');
    }
    Str h = strings_builder_string(&have), w = strings_builder_string(&want);
    if (!str_eq(h, w))
        testing_t_errorf_v(t,
                           "stdPkgs is out of date: regenerate with 'go generate'\n"
                           "stdPkgs:\n%swant:\n%s",
                           h, w);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ parse */

static void Test52353(TestingT *t) {
    (void)t;
    Str id = BURROW_STR_EMPTY;
    (void)burrow__comment_ident(S("\360\253\225\220\357\257\257"), &id);
}

#define TESTS(X)                                                                       \
    X(TestTestdata)                                                                    \
    X(TestIsOldHeading)                                                                \
    X(TestAutoURL)                                                                     \
    X(TestWrap)                                                                        \
    X(TestStd)                                                                         \
    X(Test52353)

TESTING_MAIN(TESTS)
