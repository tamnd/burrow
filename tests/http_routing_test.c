/* Derived from Go's src/net/http/pattern_test.go, routing_tree_test.go,
 * routing_index_test.go and mapping_test.go.
 * Go source: go1.27.1.
 *
 * TestRegisterConflict needs a ServeMux, so it is in http_mux_test.c.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_routing.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

typedef burrow__HttpPattern Pattern;
typedef burrow__HttpSegment Segment;
typedef burrow__HttpRelationship Relationship;
typedef burrow__HttpRoutingNode Node;

#define EQUIVALENT BURROW__HTTP_EQUIVALENT
#define MORE_GENERAL BURROW__HTTP_MORE_GENERAL
#define MORE_SPECIFIC BURROW__HTTP_MORE_SPECIFIC
#define DISJOINT BURROW__HTTP_DISJOINT
#define OVERLAPS BURROW__HTTP_OVERLAPS

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

static Str rel_name(Relationship r) {
    return burrow__http_relationship_string(r);
}

static Pattern *must_parse(TestingT *t, Alloc *a, const char *s) {
    Error err;
    Pattern *p = burrow__http_parse_pattern(a, cs(s), &err);
    if (p == NULL)
        testing_t_fatalf_v(t, "parsePattern(%q): %s", cs(s), error_text(err));
    return p;
}

static Slice str_slice(void) {
    return slice_from(NULL, 0, 0, TYPE_STRING);
}

/* --------------------------------------------------------------- parsing */

typedef struct TSeg {
    const char *s; /* NULL ends the list */
    bool wild;
    bool multi;
} TSeg;

#define LIT(x) {x, false, false}
#define WILD(x) {x, true, false}
#define MULTI(x) {x, true, true}

typedef struct ParseTest {
    const char *in;
    const char *method;
    const char *host;
    TSeg segs[7];
} ParseTest;

static const ParseTest parse_tests[] = {
    {"/", "", "", {MULTI("")}},
    {"/a", "", "", {LIT("a")}},
    {"/a/", "", "", {LIT("a"), MULTI("")}},
    {"/path/to/something", "", "", {LIT("path"), LIT("to"), LIT("something")}},
    {"/{w1}/lit/{w2}", "", "", {WILD("w1"), LIT("lit"), WILD("w2")}},
    {"/{w1}/lit/{w2}/", "", "", {WILD("w1"), LIT("lit"), WILD("w2"), MULTI("")}},
    {"example.com/", "", "example.com", {MULTI("")}},
    {"GET /", "GET", "", {MULTI("")}},
    {"POST example.com/foo/{w}", "POST", "example.com", {LIT("foo"), WILD("w")}},
    {"/{$}", "", "", {LIT("/")}},
    {"DELETE example.com/a/{foo12}/{$}",
     "DELETE",
     "example.com",
     {LIT("a"), WILD("foo12"), LIT("/")}},
    {"/foo/{$}", "", "", {LIT("foo"), LIT("/")}},
    {"/{a}/foo/{rest...}", "", "", {WILD("a"), LIT("foo"), MULTI("rest")}},
    {"//", "", "", {LIT(""), MULTI("")}},
    {"/foo///./../bar",
     "",
     "",
     {LIT("foo"), LIT(""), LIT(""), LIT("."), LIT(".."), LIT("bar")}},
    {"a.com/foo//", "", "a.com", {LIT("foo"), LIT(""), MULTI("")}},
    {"/%61%62/%7b/%", "", "", {LIT("ab"), LIT("{"), LIT("%")}},
    /* Any run of spaces and tabs between the method and the path. */
    {"GET\t  /", "GET", "", {MULTI("")}},
    {"POST \t  example.com/foo/{w}", "POST", "example.com", {LIT("foo"), WILD("w")}},
    {"DELETE    \texample.com/a/{foo12}/{$}",
     "DELETE",
     "example.com",
     {LIT("a"), WILD("foo12"), LIT("/")}},
};

static bool pattern_equal(const Pattern *p, const ParseTest *want) {
    if (!str_eq(p->method, cs(want->method)) || !str_eq(p->host, cs(want->host)))
        return false;
    Int n = 0;
    while (want->segs[n].s != NULL)
        n++;
    if (p->nsegments != n)
        return false;
    for (Int i = 0; i < n; i++) {
        const Segment *g = &p->segments[i];
        const TSeg *w = &want->segs[i];
        if (!str_eq(g->s, cs(w->s)) || g->wild != w->wild || g->multi != w->multi)
            return false;
    }
    return true;
}

/* The segments as Go's %v prints them. */
static Str segments_string(Alloc *a, const Pattern *p) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < p->nsegments; i++) {
        if (i > 0)
            (void)strings_builder_write_byte(&b, ' ');
        (void)strings_builder_write_string(
            &b,
            fmt_sprintf_v(a, "{%s %t %t}", p->segments[i].s, p->segments[i].wild,
                          p->segments[i].multi),
            NULL);
    }
    (void)strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static void TestParsePattern(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof parse_tests / sizeof parse_tests[0]; i++) {
        const ParseTest *tt = &parse_tests[i];
        Pattern *got = must_parse(t, a, tt->in);
        if (!pattern_equal(got, tt))
            testing_t_errorf_v(t, "%q:\ngot method %q host %q segments %s", cs(tt->in),
                               got->method, got->host, segments_string(a, got));
    }
    ARENA_END;
}

typedef struct ParseErrorTest {
    const char *in;
    const char *contains;
} ParseErrorTest;

static const ParseErrorTest parse_error_tests[] = {
    {"", "empty pattern"},
    {"A=B /", "at offset 0: invalid method"},
    {" ", "at offset 1: host/path missing /"},
    {"/{w}x", "at offset 1: bad wildcard segment"},
    {"/x{w}", "at offset 1: bad wildcard segment"},
    {"/{wx", "at offset 1: bad wildcard segment"},
    {"/a/{/}/c", "at offset 3: bad wildcard segment"},
    /* Wildcard names are not unescaped. */
    {"/a/{%61}/c", "at offset 3: bad wildcard name"},
    {"/{a$}", "at offset 1: bad wildcard name"},
    {"/{}", "at offset 1: empty wildcard"},
    {"POST a.com/x/{}/y", "at offset 13: empty wildcard"},
    {"/{...}", "at offset 1: empty wildcard"},
    {"/{$...}", "at offset 1: bad wildcard"},
    {"/{$}/", "at offset 1: {$} not at end"},
    {"/{$}/x", "at offset 1: {$} not at end"},
    {"/abc/{$}/x", "at offset 5: {$} not at end"},
    {"/{a...}/", "at offset 1: {...} wildcard not at end"},
    {"/{a...}/x", "at offset 1: {...} wildcard not at end"},
    {"{a}/b", "at offset 0: host contains '{' (missing initial '/'?)"},
    {"/a/{x}/b/{x...}", "at offset 9: duplicate wildcard name"},
    {"GET //", "at offset 4: non-CONNECT pattern with unclean path"},
};

static void TestParsePatternError(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof parse_error_tests / sizeof parse_error_tests[0];
         i++) {
        const ParseErrorTest *tt = &parse_error_tests[i];
        Error err;
        Pattern *p = burrow__http_parse_pattern(a, cs(tt->in), &err);
        if (p != NULL || !strings_contains(error_text(err), cs(tt->contains)))
            testing_t_errorf_v(t, "%q:\ngot %s, want error containing %q", cs(tt->in),
                               p != NULL ? S("<nil>") : error_text(err),
                               cs(tt->contains));
    }
    ARENA_END;
}

/* ------------------------------------------------------------- comparing */

typedef struct RelTest {
    const char *p1;
    const char *p2;
    Relationship want;
} RelTest;

static const RelTest compare_methods_tests[] = {
    {"/", "/", EQUIVALENT},           {"GET /", "GET /", EQUIVALENT},
    {"HEAD /", "HEAD /", EQUIVALENT}, {"POST /", "POST /", EQUIVALENT},
    {"GET /", "POST /", DISJOINT},    {"GET /", "/", MORE_SPECIFIC},
    {"HEAD /", "/", MORE_SPECIFIC},   {"GET /", "HEAD /", MORE_GENERAL},
};

static void TestCompareMethods(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0;
         i < sizeof compare_methods_tests / sizeof compare_methods_tests[0]; i++) {
        const RelTest *tt = &compare_methods_tests[i];
        Pattern *pat1 = must_parse(t, a, tt->p1);
        Pattern *pat2 = must_parse(t, a, tt->p2);
        Relationship got = burrow__http_pattern_compare_methods(pat1, pat2);
        if (got != tt->want)
            testing_t_errorf_v(t, "%s vs %s: got %s, want %s", cs(tt->p1), cs(tt->p2),
                               rel_name(got), rel_name(tt->want));
        Relationship got2 = burrow__http_pattern_compare_methods(pat2, pat1);
        Relationship want2 = burrow__http_inverse_relationship(tt->want);
        if (got2 != want2)
            testing_t_errorf_v(t, "%s vs %s: got %s, want %s", cs(tt->p2), cs(tt->p1),
                               rel_name(got2), rel_name(want2));
    }
    ARENA_END;
}

static const RelTest compare_paths_tests[] = {
    /* A non-final pattern segment can be a literal or a single wildcard. A
     * final one can be empty (a trailing slash), a literal, a dollar, a single
     * wildcard or a multi wildcard, and the trailing slash and the multi are
     * the same. */

    /* A literal is more specific than anything it overlaps but itself. */
    {"/a", "/a", EQUIVALENT},
    {"/a", "/b", DISJOINT},
    {"/a", "/", MORE_SPECIFIC},
    {"/a", "/{$}", DISJOINT},
    {"/a", "/{x}", MORE_SPECIFIC},
    {"/a", "/{x...}", MORE_SPECIFIC},

    /* And another segment in front does not change that. */
    {"/b/a", "/b/a", EQUIVALENT},
    {"/b/a", "/b/b", DISJOINT},
    {"/b/a", "/b/", MORE_SPECIFIC},
    {"/b/a", "/b/{$}", DISJOINT},
    {"/b/a", "/b/{x}", MORE_SPECIFIC},
    {"/b/a", "/b/{x...}", MORE_SPECIFIC},
    {"/{z}/a", "/{z}/a", EQUIVALENT},
    {"/{z}/a", "/{z}/b", DISJOINT},
    {"/{z}/a", "/{z}/", MORE_SPECIFIC},
    {"/{z}/a", "/{z}/{$}", DISJOINT},
    {"/{z}/a", "/{z}/{x}", MORE_SPECIFIC},
    {"/{z}/a", "/{z}/{x...}", MORE_SPECIFIC},

    /* A single wildcard on the left. */
    {"/{z}", "/a", MORE_GENERAL},
    {"/{z}", "/a/b", DISJOINT},
    {"/{z}", "/{$}", DISJOINT},
    {"/{z}", "/{x}", EQUIVALENT},
    {"/{z}", "/", MORE_SPECIFIC},
    {"/{z}", "/{x...}", MORE_SPECIFIC},
    {"/b/{z}", "/b/a", MORE_GENERAL},
    {"/b/{z}", "/b/a/b", DISJOINT},
    {"/b/{z}", "/b/{$}", DISJOINT},
    {"/b/{z}", "/b/{x}", EQUIVALENT},
    {"/b/{z}", "/b/", MORE_SPECIFIC},
    {"/b/{z}", "/b/{x...}", MORE_SPECIFIC},

    /* A trailing slash on the left. */
    {"/", "/a", MORE_GENERAL},
    {"/", "/a/b", MORE_GENERAL},
    {"/", "/{$}", MORE_GENERAL},
    {"/", "/{x}", MORE_GENERAL},
    {"/", "/", EQUIVALENT},
    {"/", "/{x...}", EQUIVALENT},

    {"/b/", "/b/a", MORE_GENERAL},
    {"/b/", "/b/a/b", MORE_GENERAL},
    {"/b/", "/b/{$}", MORE_GENERAL},
    {"/b/", "/b/{x}", MORE_GENERAL},
    {"/b/", "/b/", EQUIVALENT},
    {"/b/", "/b/{x...}", EQUIVALENT},

    {"/{z}/", "/{z}/a", MORE_GENERAL},
    {"/{z}/", "/{z}/a/b", MORE_GENERAL},
    {"/{z}/", "/{z}/{$}", MORE_GENERAL},
    {"/{z}/", "/{z}/{x}", MORE_GENERAL},
    {"/{z}/", "/{z}/", EQUIVALENT},
    {"/{z}/", "/a/", MORE_GENERAL},
    {"/{z}/", "/{z}/{x...}", EQUIVALENT},
    {"/{z}/", "/a/{x...}", MORE_GENERAL},
    {"/a/{z}/", "/{z}/a/", OVERLAPS},
    {"/a/{z}/b/", "/{x}/c/{y...}", OVERLAPS},

    /* A multi wildcard on the left. */
    {"/{m...}", "/a", MORE_GENERAL},
    {"/{m...}", "/a/b", MORE_GENERAL},
    {"/{m...}", "/{$}", MORE_GENERAL},
    {"/{m...}", "/{x}", MORE_GENERAL},
    {"/{m...}", "/", EQUIVALENT},
    {"/{m...}", "/{x...}", EQUIVALENT},

    {"/b/{m...}", "/b/a", MORE_GENERAL},
    {"/b/{m...}", "/b/a/b", MORE_GENERAL},
    {"/b/{m...}", "/b/{$}", MORE_GENERAL},
    {"/b/{m...}", "/b/{x}", MORE_GENERAL},
    {"/b/{m...}", "/b/", EQUIVALENT},
    {"/b/{m...}", "/b/{x...}", EQUIVALENT},
    {"/b/{m...}", "/a/{x...}", DISJOINT},

    {"/{z}/{m...}", "/{z}/a", MORE_GENERAL},
    {"/{z}/{m...}", "/{z}/a/b", MORE_GENERAL},
    {"/{z}/{m...}", "/{z}/{$}", MORE_GENERAL},
    {"/{z}/{m...}", "/{z}/{x}", MORE_GENERAL},
    {"/{z}/{m...}", "/{w}/", EQUIVALENT},
    {"/{z}/{m...}", "/a/", MORE_GENERAL},
    {"/{z}/{m...}", "/{z}/{x...}", EQUIVALENT},
    {"/{z}/{m...}", "/a/{x...}", MORE_GENERAL},
    {"/a/{m...}", "/a/b/{y...}", MORE_GENERAL},
    {"/a/{m...}", "/a/{x}/{y...}", MORE_GENERAL},
    {"/a/{z}/{m...}", "/a/b/{y...}", MORE_GENERAL},
    {"/a/{z}/{m...}", "/{z}/a/", OVERLAPS},
    {"/a/{z}/{m...}", "/{z}/b/{y...}", OVERLAPS},
    {"/a/{z}/b/{m...}", "/{x}/c/{y...}", OVERLAPS},
    {"/a/{z}/a/{m...}", "/{x}/b", DISJOINT},

    /* A dollar on the left. */
    {"/{$}", "/a", DISJOINT},
    {"/{$}", "/a/b", DISJOINT},
    {"/{$}", "/{$}", EQUIVALENT},
    {"/{$}", "/{x}", DISJOINT},
    {"/{$}", "/", MORE_SPECIFIC},
    {"/{$}", "/{x...}", MORE_SPECIFIC},

    {"/b/{$}", "/b", DISJOINT},
    {"/b/{$}", "/b/a", DISJOINT},
    {"/b/{$}", "/b/a/b", DISJOINT},
    {"/b/{$}", "/b/{$}", EQUIVALENT},
    {"/b/{$}", "/b/{x}", DISJOINT},
    {"/b/{$}", "/b/", MORE_SPECIFIC},
    {"/b/{$}", "/b/{x...}", MORE_SPECIFIC},
    {"/b/{$}", "/b/c/{x...}", DISJOINT},
    {"/b/{x}/a/{$}", "/{x}/c/{y...}", OVERLAPS},
    {"/{x}/b/{$}", "/a/{x}/{y}", DISJOINT},
    {"/{x}/b/{$}", "/a/{x}/c", DISJOINT},

    {"/{z}/{$}", "/{z}/a", DISJOINT},
    {"/{z}/{$}", "/{z}/a/b", DISJOINT},
    {"/{z}/{$}", "/{z}/{$}", EQUIVALENT},
    {"/{z}/{$}", "/{z}/{x}", DISJOINT},
    {"/{z}/{$}", "/{z}/", MORE_SPECIFIC},
    {"/{z}/{$}", "/a/", OVERLAPS},
    {"/{z}/{$}", "/a/{x...}", OVERLAPS},
    {"/{z}/{$}", "/{z}/{x...}", MORE_SPECIFIC},
    {"/a/{z}/{$}", "/{z}/a/", OVERLAPS},
};

static void TestComparePaths(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof compare_paths_tests / sizeof compare_paths_tests[0];
         i++) {
        const RelTest *tt = &compare_paths_tests[i];
        Pattern *pat1 = must_parse(t, a, tt->p1);
        Pattern *pat2 = must_parse(t, a, tt->p2);
        Relationship g = burrow__http_pattern_compare_paths(pat1, pat1);
        if (g != EQUIVALENT)
            testing_t_errorf_v(t, "%s does not match itself; got %s", pat1->str,
                               rel_name(g));
        g = burrow__http_pattern_compare_paths(pat2, pat2);
        if (g != EQUIVALENT)
            testing_t_errorf_v(t, "%s does not match itself; got %s", pat2->str,
                               rel_name(g));
        Relationship got = burrow__http_pattern_compare_paths(pat1, pat2);
        if (got != tt->want) {
            testing_t_errorf_v(t, "%s vs %s: got %s, want %s", cs(tt->p1), cs(tt->p2),
                               rel_name(got), rel_name(tt->want));
            testing_t_logf_v(t, "pat1: %s\n", segments_string(a, pat1));
            testing_t_logf_v(t, "pat2: %s\n", segments_string(a, pat2));
        }
        Relationship want2 = burrow__http_inverse_relationship(tt->want);
        Relationship got2 = burrow__http_pattern_compare_paths(pat2, pat1);
        if (got2 != want2)
            testing_t_errorf_v(t, "%s vs %s: got %s, want %s", cs(tt->p2), cs(tt->p1),
                               rel_name(got2), rel_name(want2));
    }
    ARENA_END;
}

typedef struct ConflictTest {
    const char *p1;
    const char *p2;
    bool want;
} ConflictTest;

static const ConflictTest conflict_tests[] = {
    {"/a", "/a", true},
    {"/a", "/ab", false},
    {"/a/b/cd", "/a/b/cd", true},
    {"/a/b/cd", "/a/b/c", false},
    {"/a/b/c", "/a/c/c", false},
    {"/{x}", "/{y}", true},
    {"/{x}", "/a", false}, /* more specific */
    {"/{x}/{y}", "/{x}/a", false},
    {"/{x}/{y}", "/{x}/a/b", false},
    {"/{x}", "/a/{y}", false},
    {"/{x}/{y}", "/{x}/a/", false},
    {"/{x}", "/a/{y...}", false},           /* more specific */
    {"/{x}/a/{y}", "/{x}/a/{y...}", false}, /* more specific */
    {"/{x}/{y}", "/{x}/a/{$}", false},      /* more specific */
    {"/{x}/{y}/{$}", "/{x}/a/{$}", false},
    {"/a/{x}", "/{x}/b", true},
    {"/", "GET /", false},
    {"/", "GET /foo", false},
    {"GET /", "GET /foo", false},
    {"GET /", "/foo", true},
    {"GET /foo", "HEAD /", true},
};

static void TestConflictsWith(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof conflict_tests / sizeof conflict_tests[0]; i++) {
        const ConflictTest *tt = &conflict_tests[i];
        Pattern *pat1 = must_parse(t, a, tt->p1);
        Pattern *pat2 = must_parse(t, a, tt->p2);
        bool got = burrow__http_pattern_conflicts_with(pat1, pat2);
        if (got != tt->want)
            testing_t_errorf_v(t, "%q.ConflictsWith(%q) = %t, want %t", cs(tt->p1),
                               cs(tt->p2), got, tt->want);
        /* It goes both ways. */
        got = burrow__http_pattern_conflicts_with(pat2, pat1);
        if (got != tt->want)
            testing_t_errorf_v(t, "%q.ConflictsWith(%q) = %t, want %t", cs(tt->p2),
                               cs(tt->p1), got, tt->want);
    }
    ARENA_END;
}

typedef struct PathTest {
    const char *p1;
    const char *p2;
    const char *want;
} PathTest;

static const PathTest describe_tests[] = {
    {"/a/{x}", "/a/{y}", "the same requests"},
    {"/", "/{m...}", "the same requests"},
    {"/a/{x}", "/{y}/b", "both match some paths"},
    {"/a", "GET /{x}",
     "matches more methods than GET /{x}, but has a more specific path pattern"},
    {"GET /a", "HEAD /",
     "matches more methods than HEAD /, but has a more specific path pattern"},
    {"POST /", "/a",
     "matches fewer methods than /a, but has a more general path pattern"},
};

static void TestDescribeConflict(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof describe_tests / sizeof describe_tests[0]; i++) {
        const PathTest *tt = &describe_tests[i];
        Str got = burrow__http_describe_conflict(a, must_parse(t, a, tt->p1),
                                                 must_parse(t, a, tt->p2));
        if (!strings_contains(got, cs(tt->want)))
            testing_t_errorf_v(t, "%s vs. %s:\ngot:\n%s\nwhich does not contain %q",
                               cs(tt->p1), cs(tt->p2), got, cs(tt->want));
    }
    ARENA_END;
}

static const PathTest common_path_tests[] = {
    {"/a/{x}", "/{x}/a", "/a/a"},
    {"/a/{z}/", "/{z}/a/", "/a/a/"},
    {"/a/{z}/{m...}", "/{z}/a/", "/a/a/"},
    {"/{z}/{$}", "/a/", "/a/"},
    {"/{z}/{$}", "/a/{x...}", "/a/"},
    {"/a/{z}/{$}", "/{z}/a/", "/a/a/"},
    {"/a/{x}/b/{y...}", "/{x}/c/{y...}", "/a/c/b/"},
    {"/a/{x}/b/", "/{x}/c/{y...}", "/a/c/b/"},
    {"/a/{x}/b/{$}", "/{x}/c/{y...}", "/a/c/b/"},
    {"/a/{z}/{x...}", "/{z}/b/{y...}", "/a/b/"},
};

static void TestCommonPath(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof common_path_tests / sizeof common_path_tests[0];
         i++) {
        const PathTest *tt = &common_path_tests[i];
        Pattern *pat1 = must_parse(t, a, tt->p1);
        Pattern *pat2 = must_parse(t, a, tt->p2);
        if (burrow__http_pattern_compare_paths(pat1, pat2) != OVERLAPS)
            testing_t_fatalf_v(t, "%s does not overlap %s", cs(tt->p1), cs(tt->p2));
        Str got = burrow__http_common_path(a, pat1, pat2);
        if (!str_eq(got, cs(tt->want)))
            testing_t_errorf_v(t, "%s vs. %s: got %q, want %q", cs(tt->p1), cs(tt->p2),
                               got, cs(tt->want));
    }
    ARENA_END;
}

static const PathTest difference_path_tests[] = {
    {"/a/{x}", "/{x}/a", "/a/x"},
    {"/{x}/a", "/a/{x}", "/x/a"},
    {"/a/{z}/", "/{z}/a/", "/a/z/"},
    {"/{z}/a/", "/a/{z}/", "/z/a/"},
    {"/{a}/a/", "/a/{z}/", "/ax/a/"},
    {"/a/{z}/{x...}", "/{z}/b/{y...}", "/a/z/"},
    {"/{z}/b/{y...}", "/a/{z}/{x...}", "/z/b/"},
    {"/a/b/", "/a/b/c", "/a/b/"},
    {"/a/b/{x...}", "/a/b/c", "/a/b/"},
    {"/a/b/{x...}", "/a/b/c/d", "/a/b/"},
    {"/a/b/{x...}", "/a/b/c/d/", "/a/b/"},
    {"/a/{z}/{m...}", "/{z}/a/", "/a/z/"},
    {"/{z}/a/", "/a/{z}/{m...}", "/z/a/"},
    {"/{z}/{$}", "/a/", "/z/"},
    {"/a/", "/{z}/{$}", "/a/x"},
    {"/{z}/{$}", "/a/{x...}", "/z/"},
    {"/a/{foo...}", "/{z}/{$}", "/a/foo"},
    {"/a/{z}/{$}", "/{z}/a/", "/a/z/"},
    {"/{z}/a/", "/a/{z}/{$}", "/z/a/x"},
    {"/a/{x}/b/{y...}", "/{x}/c/{y...}", "/a/x/b/"},
    {"/{x}/c/{y...}", "/a/{x}/b/{y...}", "/x/c/"},
    {"/a/{c}/b/", "/{x}/c/{y...}", "/a/cx/b/"},
    {"/{x}/c/{y...}", "/a/{c}/b/", "/x/c/"},
    {"/a/{x}/b/{$}", "/{x}/c/{y...}", "/a/x/b/"},
    {"/{x}/c/{y...}", "/a/{x}/b/{$}", "/x/c/"},
};

static void TestDifferencePath(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0;
         i < sizeof difference_path_tests / sizeof difference_path_tests[0]; i++) {
        const PathTest *tt = &difference_path_tests[i];
        Pattern *pat1 = must_parse(t, a, tt->p1);
        Pattern *pat2 = must_parse(t, a, tt->p2);
        Relationship rel = burrow__http_pattern_compare_paths(pat1, pat2);
        if (rel != OVERLAPS && rel != MORE_GENERAL)
            testing_t_fatalf_v(t, "%s vs. %s are %s, need overlaps or moreGeneral",
                               pat1->str, pat2->str, rel_name(rel));
        Str got = burrow__http_difference_path(a, pat1, pat2);
        if (!str_eq(got, cs(tt->want)))
            testing_t_errorf_v(t, "%s vs. %s: got %q, want %q", cs(tt->p1), cs(tt->p2),
                               got, cs(tt->want));
    }
    ARENA_END;
}

/* ---------------------------------------------------------- routing tree */

/* A list of strings as Go's %v prints a []string. */
static Str strs_string(Alloc *a, Slice v) {
    return fmt_sprintf_v(a, "[%s]", strings_join(a, v, S(" ")));
}

static Slice strs_of(Alloc *a, const char *const *v) {
    Slice s = str_slice();
    for (Int i = 0; v[i] != NULL; i++) {
        Str x = cs(v[i]);
        s = slice_append(a, s, &x, 1);
    }
    return s;
}

typedef struct FirstSegmentTest {
    const char *in;
    const char *want[4];
} FirstSegmentTest;

static const FirstSegmentTest first_segment_tests[] = {
    {"/a/b/c", {"a", "b", "c", NULL}},
    {"/a/b/", {"a", "b", "/", NULL}},
    {"/", {"/", NULL}},
    {"/a/%62/c", {"a", "b", "c", NULL}},
    {"/a%2Fb%2fc", {"a/b/c", NULL}},
};

static void TestRoutingFirstSegment(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof first_segment_tests / sizeof first_segment_tests[0];
         i++) {
        const FirstSegmentTest *tt = &first_segment_tests[i];
        Slice got = str_slice();
        Str rest = cs(tt->in);
        while (rest.len > 0) {
            Str seg = burrow__http_first_segment(a, rest, &rest);
            got = slice_append(a, got, &seg, 1);
        }
        Slice want = strs_of(a, tt->want);
        if (!slices_equal(got, want))
            testing_t_errorf_v(t, "%q: got %s, want %s", cs(tt->in),
                               strs_string(a, got), strs_string(a, want));
    }
    ARENA_END;
}

static Node *build_tree(Alloc *a, const char *const *pats) {
    Node *root = (Node *)mem_alloc(a, sizeof *root, _Alignof(Node));
    if (root == NULL)
        panic_str(S("out of memory"));
    memset(root, 0, sizeof *root);
    for (Int i = 0; pats[i] != NULL; i++) {
        Error err;
        Pattern *pat = burrow__http_parse_pattern(a, cs(pats[i]), &err);
        if (pat == NULL)
            panic_str(error_text(err));
        if (!burrow__http_routing_add_pattern(a, root, pat, NULL))
            panic_str(S("out of memory"));
    }
    return root;
}

static const char *const test_tree_patterns[] = {"/a",       "/a/b",     "/a/{x}",
                                                 "/g/h/i",   "/g/{x}/j", "/a/b/{x...}",
                                                 "/a/b/{y}", "/a/b/{$}", NULL};

typedef struct KeysEnv {
    Alloc *a;
    Slice keys;
} KeysEnv;

static bool collect_key(void *env, Str k, void *v) {
    (void)v;
    KeysEnv *e = (KeysEnv *)env;
    e->keys = slice_append(e->a, e->keys, &k, 1);
    return true;
}

/* print, from routing_tree_test.go. */
static void print_node(StringsBuilder *b, Alloc *a, const Node *n, Int level) {
    Str indent = strings_repeat(a, S("    "), level);
    if (n->pattern != NULL)
        (void)strings_builder_write_string(
            b, fmt_sprintf_v(a, "%s%q\n", indent, n->pattern->str), NULL);
    if (n->empty_child != NULL) {
        (void)strings_builder_write_string(
            b, fmt_sprintf_v(a, "%s%q:\n", indent, S("")), NULL);
        print_node(b, a, n->empty_child, level + 1);
    }

    KeysEnv e = {a, str_slice()};
    burrow__http_mapping_each_pair(&n->children, collect_key, &e);
    slices_sort(e.keys);

    for (Int i = 0; i < e.keys.len; i++) {
        Str k = ((const Str *)e.keys.p)[i];
        (void)strings_builder_write_string(b, fmt_sprintf_v(a, "%s%q:\n", indent, k),
                                           NULL);
        print_node(b, a, burrow__http_routing_find_child(n, k), level + 1);
    }

    if (n->multi_child != NULL) {
        (void)strings_builder_write_string(b, fmt_sprintf_v(a, "%sMULTI:\n", indent),
                                           NULL);
        print_node(b, a, n->multi_child, level + 1);
    }
}

static void TestRoutingAddPattern(TestingT *t) {
    ARENA_BEGIN;
    const char *want = "\"\":\n"
                       "    \"\":\n"
                       "        \"a\":\n"
                       "            \"/a\"\n"
                       "            \"\":\n"
                       "                \"/a/{x}\"\n"
                       "            \"b\":\n"
                       "                \"/a/b\"\n"
                       "                \"\":\n"
                       "                    \"/a/b/{y}\"\n"
                       "                \"/\":\n"
                       "                    \"/a/b/{$}\"\n"
                       "                MULTI:\n"
                       "                    \"/a/b/{x...}\"\n"
                       "        \"g\":\n"
                       "            \"\":\n"
                       "                \"j\":\n"
                       "                    \"/g/{x}/j\"\n"
                       "            \"h\":\n"
                       "                \"i\":\n"
                       "                    \"/g/h/i\"\n";
    StringsBuilder b = STRINGS_BUILDER(a);
    print_node(&b, a, build_tree(a, test_tree_patterns), 0);
    Str got = strings_builder_string(&b);
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "got\n%s\nwant\n%s", got, cs(want));
    ARENA_END;
}

typedef struct MatchCase {
    const char *method;
    const char *host;
    const char *path;
    const char *want_pat; /* "" for no match */
    const char *want_matches[3];
} MatchCase;

static void test_match(TestingT *t, Alloc *a, const Node *tree, const MatchCase *tests,
                       size_t n) {
    for (size_t i = 0; i < n; i++) {
        const MatchCase *tt = &tests[i];
        Slice got_matches;
        const Node *got_node = burrow__http_routing_match(
            tree, a, cs(tt->host), cs(tt->method), cs(tt->path), &got_matches);
        Str got = S("");
        if (got_node != NULL)
            got = got_node->pattern->str;
        if (!str_eq(got, cs(tt->want_pat)))
            testing_t_errorf_v(t, "%s, %s, %s: got %q, want %q", cs(tt->host),
                               cs(tt->method), cs(tt->path), got, cs(tt->want_pat));
        Slice want_matches = strs_of(a, tt->want_matches);
        if (!slices_equal(got_matches, want_matches))
            testing_t_errorf_v(t, "%s, %s, %s: got matches %s, want %s", cs(tt->host),
                               cs(tt->method), cs(tt->path),
                               strs_string(a, got_matches),
                               strs_string(a, want_matches));
    }
}

#define NO_MATCHES {NULL}

static const MatchCase match_test_tree[] = {
    {"GET", "", "/a", "/a", NO_MATCHES},
    {"Get", "", "/b", "", NO_MATCHES},
    {"Get", "", "/a/b", "/a/b", NO_MATCHES},
    {"Get", "", "/a/c", "/a/{x}", {"c", NULL}},
    {"Get", "", "/a/b/", "/a/b/{$}", NO_MATCHES},
    {"Get", "", "/a/b/c", "/a/b/{y}", {"c", NULL}},
    {"Get", "", "/a/b/c/d", "/a/b/{x...}", {"c/d", NULL}},
    {"Get", "", "/g/h/i", "/g/h/i", NO_MATCHES},
    {"Get", "", "/g/h/j", "/g/{x}/j", {"h", NULL}},
};

static const char *const item_patterns[] = {"/item/",
                                            "POST /item/{user}",
                                            "GET /item/{user}",
                                            "/item/{user}",
                                            "/item/{user}/{id}",
                                            "/item/{user}/new",
                                            "/item/{$}",
                                            "POST alt.com/item/{user}",
                                            "GET /headwins",
                                            "HEAD /headwins",
                                            "/path/{p...}",
                                            NULL};

static const MatchCase match_item[] = {
    {"GET", "", "/item/jba", "GET /item/{user}", {"jba", NULL}},
    {"POST", "", "/item/jba", "POST /item/{user}", {"jba", NULL}},
    {"HEAD", "", "/item/jba", "GET /item/{user}", {"jba", NULL}},
    /* Methods match case-sensitively. */
    {"get", "", "/item/jba", "/item/{user}", {"jba", NULL}},
    {"POST", "", "/item/jba/17", "/item/{user}/{id}", {"jba", "17", NULL}},
    {"GET", "", "/item/jba/new", "/item/{user}/new", {"jba", NULL}},
    {"GET", "", "/item/", "/item/{$}", NO_MATCHES},
    {"GET", "", "/item/jba/17/line2", "/item/", NO_MATCHES},
    {"POST", "alt.com", "/item/jba", "POST alt.com/item/{user}", {"jba", NULL}},
    {"GET", "alt.com", "/item/jba", "GET /item/{user}", {"jba", NULL}},
    /* No match. */
    {"GET", "", "/item", "", NO_MATCHES},
    {"GET", "", "/headwins", "GET /headwins", NO_MATCHES},
    /* HEAD is more specific than GET. */
    {"HEAD", "", "/headwins", "HEAD /headwins", NO_MATCHES},
    {"GET", "", "/path/to/file", "/path/{p...}", {"to/file", NULL}},
    {"GET", "", "/path/*", "/path/{p...}", {"*", NULL}},
};

/* A pattern ending in {$} only matches a URL with a trailing slash. */
static const char *const pat1_patterns[] = {"/a/b/{$}", NULL};
static const MatchCase match_pat1[] = {
    {"GET", "", "/a/b", "", NO_MATCHES},
    {"GET", "", "/a/b/", "/a/b/{$}", NO_MATCHES},
    {"GET", "", "/a/b/c", "", NO_MATCHES},
    {"GET", "", "/a/b/c/d", "", NO_MATCHES},
};

/* One ending in a single wildcard does not match a trailing slash. */
static const char *const pat2_patterns[] = {"/a/b/{w}", NULL};
static const MatchCase match_pat2[] = {
    {"GET", "", "/a/b", "", NO_MATCHES},
    {"GET", "", "/a/b/", "", NO_MATCHES},
    {"GET", "", "/a/b/c", "/a/b/{w}", {"c", NULL}},
    {"GET", "", "/a/b/c/d", "", NO_MATCHES},
};

/* One ending in a multi wildcard matches both. */
static const char *const pat3_patterns[] = {"/a/b/{w...}", NULL};
static const MatchCase match_pat3[] = {
    {"GET", "", "/a/b", "", NO_MATCHES},
    {"GET", "", "/a/b/", "/a/b/{w...}", {"", NULL}},
    {"GET", "", "/a/b/c", "/a/b/{w...}", {"c", NULL}},
    {"GET", "", "/a/b/c/d", "/a/b/{w...}", {"c/d", NULL}},
};

/* And the three work together. */
static const char *const all_patterns[] = {"/a/b/{$}", "/a/b/{w}", "/a/b/{w...}", NULL};
static const MatchCase match_all[] = {
    {"GET", "", "/a/b", "", NO_MATCHES},
    {"GET", "", "/a/b/", "/a/b/{$}", NO_MATCHES},
    {"GET", "", "/a/b/c", "/a/b/{w}", {"c", NULL}},
    {"GET", "", "/a/b/c/d", "/a/b/{w...}", {"c/d", NULL}},
};

#define MATCH(tree, cases)                                                             \
    test_match(t, a, (tree), (cases), sizeof(cases) / sizeof(cases)[0])

static void TestRoutingNodeMatch(TestingT *t) {
    ARENA_BEGIN;
    MATCH(build_tree(a, test_tree_patterns), match_test_tree);
    MATCH(build_tree(a, item_patterns), match_item);
    MATCH(build_tree(a, pat1_patterns), match_pat1);
    MATCH(build_tree(a, pat2_patterns), match_pat2);
    MATCH(build_tree(a, pat3_patterns), match_pat3);
    MATCH(build_tree(a, all_patterns), match_all);
    ARENA_END;
}

typedef struct MethodsCase {
    const char *name;
    const char *const *tree;
    const char *host;
    const char *path;
    const char *want;
} MethodsCase;

static const char *const host_tree[] = {"GET a.com/", "PUT b.com/", "POST /foo/{x}",
                                        NULL};
static const char *const post_root[] = {"POST /", NULL};
static const char *const get_root[] = {"GET /", NULL};
static const char *const any_root[] = {"/", NULL};

static const MethodsCase methods_cases[] = {
    {"post", post_root, "", "/foo", "POST"},
    {"get", get_root, "", "/foo", "GET,HEAD"},
    {"host", host_tree, "", "/foo", ""},
    {"host", host_tree, "", "/foo/bar", "POST"},
    {"host2", host_tree, "a.com", "/foo/bar", "GET,HEAD,POST"},
    {"host3", host_tree, "b.com", "/bar", "PUT"},
    /* This does not come up, because matchingMethods is only called when
     * nothing matched, but it is here for completeness. */
    {"empty", any_root, "", "/", ""},
};

static void matching_methods_case(void *env, TestingT *t) {
    const MethodsCase *tt = (const MethodsCase *)env;
    ARENA_BEGIN;
    Map *ms = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    if (ms == NULL || !burrow__http_routing_matching_methods(
                          build_tree(a, tt->tree), a, cs(tt->host), cs(tt->path), ms))
        testing_t_fatalf_v(t, "out of memory");
    Slice keys = str_slice();
    const void *k;
    for (MapIter it = map_iter(ms); map_next(&it, &k, NULL);)
        keys = slice_append(a, keys, k, 1);
    slices_sort(keys);
    Str got = strings_join(a, keys, S(","));
    if (!str_eq(got, cs(tt->want)))
        testing_t_errorf_v(t, "got %s, want %s", got, cs(tt->want));
    ARENA_END;
}

static void TestMatchingMethods(TestingT *t) {
    for (size_t i = 0; i < sizeof methods_cases / sizeof methods_cases[0]; i++)
        testing_t_run(t, cs(methods_cases[i].name),
                      BURROW_FN(TestingTFunc, matching_methods_case,
                                (void *)(uintptr_t)&methods_cases[i]));
}

/* --------------------------------------------------------- routing index */

typedef struct ConflictsEnv {
    Alloc *a;
    const Pattern *pat;
    Slice found;
} ConflictsEnv;

static Error note_conflict(void *env, Pattern *p) {
    ConflictsEnv *e = (ConflictsEnv *)env;
    if (burrow__http_pattern_conflicts_with(e->pat, p))
        e->found = slice_append(e->a, e->found, &p->str, 1);
    return BURROW_NO_ERROR;
}

/* trueConflicts. */
static Slice true_conflicts(Alloc *a, const Pattern *pat, Pattern *const *pats, Int n) {
    Slice s = str_slice();
    for (Int i = 0; i < n; i++)
        if (burrow__http_pattern_conflicts_with(pat, pats[i]))
            s = slice_append(a, s, &pats[i]->str, 1);
    slices_sort(s);
    return s;
}

/* indexConflicts. */
static Slice index_conflicts(Alloc *a, const Pattern *pat,
                             const burrow__HttpRoutingIndex *idx) {
    ConflictsEnv e = {a, pat, str_slice()};
    (void)burrow__http_routing_index_possibly_conflicting(idx, pat, note_conflict, &e);
    slices_sort(e.found);
    return slices_compact(e.found);
}

/* Replaces each "{x}" in s with "{x0}", "{x1}" and so on, so that no pattern
 * has the same wildcard twice. */
static Str unique_wildcards(Alloc *a, Str s) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int wc = 0;
    for (;;) {
        Int i = strings_index(s, S("{x}"));
        if (i < 0) {
            (void)strings_builder_write_string(&b, s, NULL);
            break;
        }
        (void)strings_builder_write_string(&b, str_from_bytes(s.p, i), NULL);
        (void)strings_builder_write_string(&b, fmt_sprintf_v(a, "{x%d}", wc), NULL);
        wc++;
        s = str_from_bytes(s.p + i + 3, s.len - i - 3);
    }
    return strings_builder_string(&b);
}

/* generatePatterns. Every pattern made of a method, a host, up to three of the
 * segments and one of the final segments, in the order Go's generators make
 * them. */
static Slice generate_patterns(Alloc *a) {
    static const char *const methods[] = {"", "GET ", "HEAD ", "POST "};
    static const char *const hosts[] = {"", "h1", "h2"};
    static const char *const segs[] = {"/a", "/b", "/{x}"};
    static const char *const final_segs[] = {"/a", "/b", "/{f}", "/{m...}", "/{$}"};
    Slice pats = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    for (int m = 0; m < 4; m++) {
        for (int h = 0; h < 3; h++) {
            for (int count = 0; count <= 3; count++) {
                int combos = 1;
                for (int k = 0; k < count; k++)
                    combos *= 3;
                for (int c = 0; c < combos; c++) {
                    for (int f = 0; f < 5; f++) {
                        StringsBuilder b = STRINGS_BUILDER(a);
                        (void)strings_builder_write_string(&b, cs(methods[m]), NULL);
                        (void)strings_builder_write_string(&b, cs(hosts[h]), NULL);
                        /* The first segment varies slowest. */
                        int div = combos;
                        for (int k = 0; k < count; k++) {
                            div /= 3;
                            (void)strings_builder_write_string(
                                &b, cs(segs[(c / div) % 3]), NULL);
                        }
                        (void)strings_builder_write_string(&b, cs(final_segs[f]), NULL);
                        Str s = unique_wildcards(a, strings_builder_string(&b));
                        Error err;
                        Pattern *pat = burrow__http_parse_pattern(a, s, &err);
                        if (pat == NULL)
                            panic_str(error_text(err));
                        void *p = pat;
                        pats = slice_append(a, pats, &p, 1);
                    }
                }
            }
        }
    }
    return pats;
}

static void TestIndex(TestingT *t) {
    ARENA_BEGIN;
    /* Every kind of pattern up to some number of segments, with the conflicts
     * the index finds compared against those an exhaustive comparison finds. */
    Slice patterns = generate_patterns(a);
    Pattern *const *pats = (Pattern *const *)patterns.p;
    burrow__HttpRoutingIndex idx;
    memset(&idx, 0, sizeof idx);
    for (Int i = 0; i < patterns.len; i++) {
        Pattern *pat = pats[i];
        Slice got = index_conflicts(a, pat, &idx);
        Slice want = true_conflicts(a, pat, pats, i);
        if (!slices_equal(got, want))
            testing_t_fatalf_v(t, "%q:\ngot  %s\nwant %s", pat->str,
                               strs_string(a, got), strs_string(a, want));
        if (!burrow__http_routing_index_add_pattern(a, &idx, pat))
            testing_t_fatalf_v(t, "out of memory");
    }
    ARENA_END;
}

/* --------------------------------------------------------------- mapping */

static Str itoa(Alloc *a, Int i) {
    return strconv_itoa(a, i);
}

/* The value stored for i, which is the Str itoa(i) somewhere in a. */
static void *value_of(Alloc *a, Int i) {
    Str *v = (Str *)mem_alloc(a, sizeof *v, _Alignof(Str));
    if (v == NULL)
        panic_str(S("out of memory"));
    *v = itoa(a, i);
    return v;
}

static void TestMapping(TestingT *t) {
    ARENA_BEGIN;
    burrow__HttpMapping m;
    memset(&m, 0, sizeof m);
    for (Int i = 0; i < BURROW__HTTP_MAX_SLICE; i++)
        CHECK(burrow__http_mapping_add(a, &m, itoa(a, i), value_of(a, i)));
    if (m.m != NULL)
        testing_t_fatalf_v(t, "m.m != nil");
    for (Int i = 0; i < BURROW__HTTP_MAX_SLICE; i++) {
        const Str *g = (const Str *)burrow__http_mapping_find(&m, itoa(a, i), NULL);
        Str w = itoa(a, i);
        if (g == NULL || !str_eq(*g, w))
            testing_t_fatalf_v(t, "%d: got %s, want %s", i, g != NULL ? *g : S(""), w);
    }
    CHECK(burrow__http_mapping_add(a, &m, S("4"), value_of(a, 4)));
    if (m.s != NULL)
        testing_t_fatalf_v(t, "m.s != nil");
    if (m.m == NULL)
        testing_t_fatalf_v(t, "m.m == nil");
    const Str *g = (const Str *)burrow__http_mapping_find(&m, S("4"), NULL);
    Str w = S("4");
    if (g == NULL || !str_eq(*g, w))
        testing_t_fatalf_v(t, "got %s, want %s", g != NULL ? *g : S(""), w);
    ARENA_END;
}

#define NPAIRS ((Int)2 * BURROW__HTTP_MAX_SLICE)

typedef struct PairsEnv {
    Alloc *a;
    Int n;
    Str keys[NPAIRS];
    Str values[NPAIRS];
} PairsEnv;

static bool collect_pair(void *env, Str k, void *v) {
    PairsEnv *e = (PairsEnv *)env;
    if (e->n < NPAIRS) {
        e->keys[e->n] = k;
        e->values[e->n] = *(const Str *)v;
    }
    e->n++;
    return true;
}

static Int key_number(Str k) {
    Error err;
    return strconv_atoi(k, &err);
}

static void TestMappingEachPair(TestingT *t) {
    ARENA_BEGIN;
    burrow__HttpMapping m;
    memset(&m, 0, sizeof m);
    for (Int i = 0; i < NPAIRS; i++)
        CHECK(burrow__http_mapping_add(a, &m, itoa(a, i), value_of(a, i)));

    PairsEnv e;
    memset(&e, 0, sizeof e);
    e.a = a;
    burrow__http_mapping_each_pair(&m, collect_pair, &e);
    if (e.n != NPAIRS)
        testing_t_fatalf_v(t, "got %d pairs, want %d", e.n, NPAIRS);
    /* Sorted by key as a number, then each must be {i, itoa(i)}. */
    for (Int i = 1; i < e.n; i++) {
        for (Int j = i; j > 0 && key_number(e.keys[j - 1]) > key_number(e.keys[j]);
             j--) {
            Str tk = e.keys[j];
            Str tv = e.values[j];
            e.keys[j] = e.keys[j - 1];
            e.values[j] = e.values[j - 1];
            e.keys[j - 1] = tk;
            e.values[j - 1] = tv;
        }
    }
    for (Int i = 0; i < e.n; i++) {
        Str w = itoa(a, i);
        if (!str_eq(e.keys[i], w) || !str_eq(e.values[i], w))
            testing_t_errorf_v(t, "pair %d: got {%s %s}, want {%s %s}", i, e.keys[i],
                               e.values[i], w, w);
    }
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestParsePattern)                                                                \
    X(TestParsePatternError)                                                           \
    X(TestCompareMethods)                                                              \
    X(TestComparePaths)                                                                \
    X(TestConflictsWith)                                                               \
    X(TestDescribeConflict)                                                            \
    X(TestCommonPath)                                                                  \
    X(TestDifferencePath)                                                              \
    X(TestRoutingFirstSegment)                                                         \
    X(TestRoutingAddPattern)                                                           \
    X(TestRoutingNodeMatch)                                                            \
    X(TestMatchingMethods)                                                             \
    X(TestIndex)                                                                       \
    X(TestMapping)                                                                     \
    X(TestMappingEachPair)

TESTING_MAIN(TESTS)
