/* runtime/metrics's tests, from Go's description_test.go and example_test.go.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/metrics.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/doc/comment.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static Str bytes_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* s[i:j]. */
static Str sub(Str s, Int i, Int j) {
    return str_from_bytes(s.p + i, j - i);
}

static void TestNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Note that this regexp is promised in the package docs for Description. Do
     * not change. */
    Str expr = BURROW_S("^(?P<name>/[^:]+):(?P<unit>[^:*/]+(?:[*/][^:*/]+)*)$");
    Regexp *r = regexp_must_compile(a, expr);
    Slice all = metrics_all();
    const MetricsDescription *d = (const MetricsDescription *)all.p;
    for (Int i = 0; i < all.len; i++) {
        if (!regexp_match_string(r, d[i].name)) {
            Str name = d[i].name;
            testing_t_errorf_v(t, "name %q does not match regexp %#q", name, expr);
        }
        if (i > 0 && strings_compare(d[i - 1].name, d[i].name) >= 0) {
            Str prev = d[i - 1].name;
            Str name = d[i].name;
            testing_t_fatalf_v(t, "allDesc not sorted: %s ≥ %s", prev, name);
            arena_free(&ar);
            return;
        }
    }

    Slice names = burrow__metrics_read_names(a);
    sort_strings(names);
    Slice samples = slice_make(a, TYPE_METRICS_SAMPLE, names.len, names.len);
    MetricsSample *s = (MetricsSample *)samples.p;
    for (Int i = 0; i < names.len; i++)
        s[i].name = ((const Str *)names.p)[i];
    metrics_read(a, samples);

    Int n = samples.len;
    for (Int i = 0; i < all.len; i++) {
        while (n > 0 && strings_compare(s[0].name, d[i].name) < 0) {
            Str name = s[0].name;
            testing_t_errorf_v(t, "%s: reported by runtime but not listed in All",
                               name);
            s++;
            n--;
        }
        if (n == 0 || strings_compare(d[i].name, s[0].name) < 0) {
            Str name = d[i].name;
            testing_t_errorf_v(t, "%s: listed in All but not reported by runtime",
                               name);
            continue;
        }
        if (metrics_value_kind(s[0].value) != d[i].kind) {
            Str name = d[i].name;
            MetricsValueKind got = metrics_value_kind(s[0].value);
            MetricsValueKind want = d[i].kind;
            testing_t_errorf_v(t, "%s: runtime reports %v but All reports %v", name,
                               got, want);
        }
        s++;
        n--;
    }
    arena_free(&ar);
}

static Str wrap(Alloc *a, Str prefix, Str text, Int width) {
    CommentPlain plain = {{COMMENT_KIND_PLAIN}, text};
    CommentText texts[1] = {&plain.node};
    CommentParagraph para = {{COMMENT_KIND_PARAGRAPH},
                             slice_from(texts, 1, 1, TYPE_COMMENT_TEXT)};
    CommentBlock blocks[1] = {&para.node};
    CommentDoc doc = {slice_from(blocks, 1, 1, TYPE_COMMENT_BLOCK), (Slice){0}};
    CommentPrinter pr = {0};
    pr.text_prefix = prefix;
    pr.text_width = width;
    return bytes_str(comment_printer_text(&pr, a, &doc));
}

/* Not in Go: the list in the header is indented with spaces rather than tabs,
 * four for a name and eight for its description, so the prefix is four spaces
 * where Go's is a tab. */
static Str format_desc(Alloc *a) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Slice all = metrics_all();
    const MetricsDescription *d = (const MetricsDescription *)all.p;
    for (Int i = 0; i < all.len; i++) {
        if (i > 0)
            (void)strings_builder_write_string(&b, BURROW_S("\n"), NULL);
        (void)strings_builder_write_string(&b, d[i].name, NULL);
        (void)strings_builder_write_string(&b, BURROW_S("\n"), NULL);
        (void)strings_builder_write_string(
            &b, wrap(a, BURROW_S("    "), d[i].description, 80 - 2 * 8), NULL);
    }
    Str s = str_clone(a, strings_builder_string(&b));
    strings_builder_reset(&b);
    return s;
}

/* Not in Go: what Go's ast gives back as the package doc of doc.go, done by
 * hand for the header's doc comment, the one that starts with the package's
 * path. The comment markers go, and so does the " * " or " *" at the start of
 * each line after the first. A checkout on Windows can have CRLF line endings,
 * so a carriage return goes with the trailing spaces. */
static Str header_doc(Alloc *a, Str src) {
    Int start = strings_index(src, BURROW_S("\n/* runtime/metrics: "));
    if (start < 0)
        return BURROW_STR_EMPTY;
    src = sub(src, start + 1, src.len);
    Int end = strings_index(src, BURROW_S("*/"));
    if (end < 0)
        return BURROW_STR_EMPTY;
    StringsBuilder b = STRINGS_BUILDER(a);
    Str rest = sub(src, 3, end);
    for (Int line = 0; rest.len > 0; line++) {
        Int nl = strings_index_byte(rest, '\n');
        Str l = nl < 0 ? rest : sub(rest, 0, nl);
        rest = nl < 0 ? BURROW_STR_EMPTY : sub(rest, nl + 1, rest.len);
        if (line > 0) {
            if (strings_has_prefix(l, BURROW_S(" * ")))
                l = sub(l, 3, l.len);
            else if (strings_has_prefix(l, BURROW_S(" *")))
                l = sub(l, 2, l.len);
        }
        (void)strings_builder_write_string(&b, strings_trim_right(l, BURROW_S(" \r")),
                                           NULL);
        (void)strings_builder_write_string(&b, BURROW_S("\n"), NULL);
    }
    Str s = str_clone(a, strings_builder_string(&b));
    strings_builder_reset(&b);
    return s;
}

/* The first line where have and want differ, and the line number from 1. Not in
 * Go: there is no internal/diff here, so the failure shows that line instead
 * of a diff. */
static Int first_diff(Str have, Str want, Str *hl, Str *wl) {
    for (Int line = 1;; line++) {
        Int hn = strings_index_byte(have, '\n');
        Int wn = strings_index_byte(want, '\n');
        *hl = hn < 0 ? have : sub(have, 0, hn);
        *wl = wn < 0 ? want : sub(want, 0, wn);
        if (!str_eq(*hl, *wl) || hn < 0 || wn < 0)
            return line;
        have = sub(have, hn + 1, have.len);
        want = sub(want, wn + 1, want.len);
    }
}

/* Go's test can rewrite doc.go with -generate. Not in Go: burrow's testing
 * reads no flags of its own, so that half is left out. */
static void TestDocs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = format_desc(a);

    /* Not in Go: the list is in the header, read from the root of the tree. */
    Error err = BURROW_NO_ERROR;
    Slice src = os_read_file(a, BURROW_S("include/burrow/runtime/metrics.h"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    Str text = header_doc(a, bytes_str(src));
    if (text.len == 0) {
        testing_t_fatalf_v(t, "no doc comment in doc.go");
        arena_free(&ar);
        return;
    }
    CommentParser p = {0};
    CommentDoc *doc = comment_parser_parse(&p, a, text);
    bool expect_code = false;
    bool found_code = false;
    CommentBlock *blocks = (CommentBlock *)doc->content.p;
    for (Int i = 0; i < doc->content.len; i++) {
        CommentBlock block = blocks[i];
        if (block->kind == COMMENT_KIND_HEADING) {
            CommentHeading *b = (CommentHeading *)block;
            expect_code = false;
            CommentText first = b->text.len > 0 ? ((CommentText *)b->text.p)[0] : NULL;
            if (first != NULL && first->kind == COMMENT_KIND_PLAIN &&
                str_eq(((CommentPlain *)first)->text, BURROW_S("Supported metrics")))
                expect_code = true;
        } else if (block->kind == COMMENT_KIND_CODE) {
            CommentCode *b = (CommentCode *)block;
            if (expect_code) {
                found_code = true;
                if (!str_eq(b->text, want)) {
                    Str hl, wl;
                    Int line = first_diff(b->text, want, &hl, &wl);
                    testing_t_fatalf_v(t,
                                       "doc comment out of date; use go generate to "
                                       "rebuild\nline %d\nold:  %q\nwant: %q",
                                       line, hl, wl);
                    arena_free(&ar);
                    return;
                }
            }
        }
    }

    if (!found_code)
        testing_t_fatalf_v(t, "did not find Supported metrics list in doc.go");
    arena_free(&ar);
}

static void TestReadEmptySlice(TestingT *t) {
    (void)t;
    /* Test that Read does not panic when given an empty slice. This should be
     * a no-op. */
    Slice nil = {0};
    metrics_read(heap_allocator(), nil);
    MetricsSample none[1];
    metrics_read(heap_allocator(), slice_from(none, 0, 0, TYPE_METRICS_SAMPLE));
}

static void ExampleRead_readingOneMetric(void) {
    /* Name of the metric we want to read. */
    Str my_metric = BURROW_S("/memory/classes/heap/free:bytes");

    /* Create a sample for the metric. */
    MetricsSample sample[1];
    memset(sample, 0, sizeof sample);
    sample[0].name = my_metric;

    /* Sample the metric. */
    metrics_read(heap_allocator(), slice_from(sample, 1, 1, TYPE_METRICS_SAMPLE));

    /* Check if the metric is actually supported. If it's not, the resulting
     * value will always have kind KindBad. */
    if (metrics_value_kind(sample[0].value) == METRICS_KIND_BAD) {
        Str msg =
            fmt_sprintf_v(heap_allocator(), "metric %q no longer supported", my_metric);
        panic_str(msg);
    }

    /* Handle the result.
     *
     * It's OK to assume a particular Kind for a metric; they're guaranteed not
     * to change. */
    uint64_t free_bytes = metrics_value_uint64(sample[0].value);

    fmt_printf_v("free but not released memory: %d\n", free_bytes);
}

static double median_bucket(MetricsFloat64Histogram *h) {
    const uint64_t *counts = (const uint64_t *)h->counts.p;
    const double *buckets = (const double *)h->buckets.p;
    uint64_t total = 0;
    for (Int i = 0; i < h->counts.len; i++)
        total += counts[i];
    uint64_t thresh = total / 2;
    total = 0;
    for (Int i = 0; i < h->counts.len; i++) {
        total += counts[i];
        if (total >= thresh)
            return buckets[i];
    }
    panic_str(BURROW_S("should not happen"));
}

static void ExampleRead_readingAllMetrics(void) {
    /* Not in Go: the histograms come from an arena, given back at the end. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Get descriptions for all supported metrics. */
    Slice descs = metrics_all();
    const MetricsDescription *d = (const MetricsDescription *)descs.p;

    /* Create a sample for each metric. */
    Slice samples = slice_make(a, TYPE_METRICS_SAMPLE, descs.len, descs.len);
    MetricsSample *s = (MetricsSample *)samples.p;
    for (Int i = 0; i < samples.len; i++)
        s[i].name = d[i].name;

    /* Sample the metrics. Re-use the samples slice if you can! */
    metrics_read(a, samples);

    /* Iterate over all results. */
    for (Int i = 0; i < samples.len; i++) {
        /* Pull out the name and value. */
        Str name = s[i].name;
        MetricsValue value = s[i].value;

        /* Handle each sample. */
        MetricsValueKind kind = metrics_value_kind(value);
        if (kind == METRICS_KIND_UINT64) {
            uint64_t v = metrics_value_uint64(value);
            fmt_printf_v("%s: %d\n", name, v);
        } else if (kind == METRICS_KIND_FLOAT64) {
            double v = metrics_value_float64(value);
            fmt_printf_v("%s: %f\n", name, v);
        } else if (kind == METRICS_KIND_FLOAT64_HISTOGRAM) {
            /* The histogram may be quite large, so let's just pull out a crude
             * estimate for the median for the sake of this example. */
            double v = median_bucket(metrics_value_float64_histogram(value));
            fmt_printf_v("%s: %f\n", name, v);
        } else if (kind == METRICS_KIND_BAD) {
            /* This should never happen because all metrics are supported by
             * construction. */
            panic_str(BURROW_S("bug in runtime/metrics package!"));
        } else {
            /* This may happen as new metrics get added.
             *
             * The safest thing to do here is to simply log it somewhere as
             * something to look into, but ignore it for now. In the worst case,
             * you might temporarily miss out on a new metric. */
            fmt_printf_v("%s: unexpected metric Kind: %v\n", name, kind);
        }
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestNames)                                                                       \
    X(TestDocs)                                                                        \
    X(TestReadEmptySlice)                                                              \
    X(ExampleRead_readingOneMetric)                                                    \
    X(ExampleRead_readingAllMetrics)

TESTING_MAIN(TESTS)
