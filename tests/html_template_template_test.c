/* Derived from Go's src/html/template/template_test.go. Go source: go1.27.1.
 *
 * The tests of redefining templates after they have run, which html/template
 * refuses, and of a few things the escaper has to leave alone.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/burrow.h"
#include "burrow/encoding/json.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/text/template/parse.h"

#include "check.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* -------------------------------------------------------------- helpers */

/* Go's testCase, with the arena everything in the test is made in, freed when
 * the test ends so that the helpers can stop it with testing_t_fatalf_v. */
typedef struct TestCase {
    Arena ar;
    Alloc *a;
    TestingT *t;
    HtmlTemplate *root;
} TestCase;

static void test_case_free(void *env) {
    arena_free(&((TestCase *)env)->ar);
    mem_free(heap_allocator(), env, sizeof(TestCase), _Alignof(TestCase));
}

static TestCase *new_test_case(TestingT *t) {
    TestCase *c =
        (TestCase *)mem_alloc(heap_allocator(), sizeof *c, _Alignof(TestCase));
    arena_init(&c->ar, NULL, 0);
    c->a = arena_allocator(&c->ar);
    testing_t_cleanup(t, BURROW_FN(Func, test_case_free, c));
    c->t = t;
    c->root = html_template_new(c->a, BURROW_S("root"));
    return c;
}

static HtmlTemplate *lookup(TestCase *c, Str name) {
    return html_template_lookup(c->root, name);
}

static void must_parse(TestCase *c, HtmlTemplate *t, Str text) {
    Error err = BURROW_NO_ERROR;
    (void)html_template_parse(t, text, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(c->t, "parse: %v", err);
}

static void must_not_parse(TestCase *c, HtmlTemplate *t, Str text) {
    Error err = BURROW_NO_ERROR;
    (void)html_template_parse(t, text, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(c->t, "parse: unexpected success");
}

static void must_execute(TestCase *c, HtmlTemplate *t, Any val, Str want) {
    StringsBuilder buf = STRINGS_BUILDER(c->a);
    Error err = html_template_execute(t, strings_builder_as_io_writer(&buf), val);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(c->t, "execute: %v", err);
    if (!str_eq(strings_builder_string(&buf), want))
        testing_t_fatalf_v(c->t, "template output:\n%s\nwant:\n%s",
                           strings_builder_string(&buf), want);
}

static const Any nil_any = {NULL, NULL};

/* ------------------------------------------------------------------ tests */

static void TestTemplateClone(TestingT *t) {
    TestCase *c = new_test_case(t);
    /* https://golang.org/issue/12996 */
    HtmlTemplate *orig = html_template_new(c->a, BURROW_S("name"));
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *clone = html_template_clone(orig, c->a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (html_template_templates(clone, c->a).len !=
        html_template_templates(orig, c->a).len)
        testing_t_fatalf_v(t, "Invalid length of t.Clone().Templates()");

    Str want = BURROW_S("stuff");
    HtmlTemplate *parsed = html_template_parse(clone, want, &err);
    html_template_must(parsed, err);
    StringsBuilder buf = STRINGS_BUILDER(c->a);
    err = html_template_execute(parsed, strings_builder_as_io_writer(&buf), nil_any);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str got = strings_builder_string(&buf);
    if (!str_eq(got, want))
        testing_t_fatalf_v(t, "got %q; want %q", got, want);
}

static void TestRedefineNonEmptyAfterExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_S("foo"));
    must_execute(c, c->root, nil_any, BURROW_S("foo"));
    must_not_parse(c, c->root, BURROW_S("bar"));
}

static void TestRedefineEmptyAfterExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_STR_EMPTY);
    must_execute(c, c->root, nil_any, BURROW_STR_EMPTY);
    must_not_parse(c, c->root, BURROW_S("foo"));
    must_execute(c, c->root, nil_any, BURROW_STR_EMPTY);
}

static Int int_0 = 0;
static Int int_1 = 1;
static Int int_42 = 42;

static void TestRedefineAfterNonExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(
        c, c->root,
        BURROW_S("{{if .}}<{{template \"X\"}}>{{end}}{{define \"X\"}}foo{{end}}"));
    must_execute(c, c->root, BURROW_ANY(TYPE_INT, &int_0), BURROW_STR_EMPTY);
    must_not_parse(c, c->root, BURROW_S("{{define \"X\"}}bar{{end}}"));
    must_execute(c, c->root, BURROW_ANY(TYPE_INT, &int_1), BURROW_S("&lt;foo>"));
}

static void TestRedefineAfterNamedExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root,
               BURROW_S("<{{template \"X\" .}}>{{define \"X\"}}foo{{end}}"));
    must_execute(c, c->root, nil_any, BURROW_S("&lt;foo>"));
    must_not_parse(c, c->root, BURROW_S("{{define \"X\"}}bar{{end}}"));
    must_execute(c, c->root, nil_any, BURROW_S("&lt;foo>"));
}

static void TestRedefineNestedByNameAfterExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_S("{{define \"X\"}}foo{{end}}"));
    must_execute(c, lookup(c, BURROW_S("X")), nil_any, BURROW_S("foo"));
    must_not_parse(c, c->root, BURROW_S("{{define \"X\"}}bar{{end}}"));
    must_execute(c, lookup(c, BURROW_S("X")), nil_any, BURROW_S("foo"));
}

static void TestRedefineNestedByTemplateAfterExecution(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_S("{{define \"X\"}}foo{{end}}"));
    must_execute(c, lookup(c, BURROW_S("X")), nil_any, BURROW_S("foo"));
    must_not_parse(c, lookup(c, BURROW_S("X")), BURROW_S("bar"));
    must_execute(c, lookup(c, BURROW_S("X")), nil_any, BURROW_S("foo"));
}

static void TestRedefineSafety(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(
        c, c->root,
        BURROW_S("<html><a href=\"{{template \"X\"}}\">{{define \"X\"}}{{end}}"));
    must_execute(c, c->root, nil_any, BURROW_S("<html><a href=\"\">"));
    /* Note: Every version of Go prior to Go 1.8 accepted the redefinition of
     * "X" on the next line, but luckily kept it from being used in the outer
     * template. Now we reject it, which makes clearer that we're not going to
     * use it. */
    must_not_parse(c, c->root, BURROW_S("{{define \"X\"}}\" bar=\"baz{{end}}"));
    must_execute(c, c->root, nil_any, BURROW_S("<html><a href=\"\">"));
}

static void TestRedefineTopUse(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_S("{{template \"X\"}}{{.}}{{define \"X\"}}{{end}}"));
    must_execute(c, c->root, BURROW_ANY(TYPE_INT, &int_42), BURROW_S("42"));
    must_not_parse(c, c->root, BURROW_S("{{define \"X\"}}<script>{{end}}"));
    must_execute(c, c->root, BURROW_ANY(TYPE_INT, &int_42), BURROW_S("42"));
}

static void TestRedefineOtherParsers(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_STR_EMPTY);
    must_execute(c, c->root, nil_any, BURROW_STR_EMPTY);
    Str files[] = {BURROW_S("no.template")};
    Error err = BURROW_NO_ERROR;
    (void)html_template_parse_files(c->root, slice_from(files, 1, 1, TYPE_STRING),
                                    &err);
    if (BURROW_OK(err) || !strings_contains(error_text(err), BURROW_S("Execute")))
        testing_t_errorf_v(
            t, "ParseFiles: %v\nwanted error about already having Executed", err);
    err = BURROW_NO_ERROR;
    (void)html_template_parse_glob(c->root, BURROW_S("*.no.template"), &err);
    if (BURROW_OK(err) || !strings_contains(error_text(err), BURROW_S("Execute")))
        testing_t_errorf_v(
            t, "ParseGlob: %v\nwanted error about already having Executed", err);
    err = BURROW_NO_ERROR;
    (void)html_template_add_parse_tree(c->root, BURROW_S("t1"), c->root->tree, &err);
    if (BURROW_OK(err) || !strings_contains(error_text(err), BURROW_S("Execute")))
        testing_t_errorf_v(
            t, "AddParseTree: %v\nwanted error about already having Executed", err);
}

static void TestNumbers(TestingT *t) {
    TestCase *c = new_test_case(t);
    must_parse(c, c->root, BURROW_S("{{print 1_2.3_4}} {{print 0x0_1.e_0p+02}}"));
    must_execute(c, c->root, nil_any, BURROW_S("12.34 7.5"));
}

typedef struct JsonStringTest {
    Str name;
    Str in;
} JsonStringTest;

/* string(rune(-1)) is the replacement character. */
static const JsonStringTest json_string_tests[] = {
    {S_("empty"), S_("")},
    {S_("invalid"), S_("\xef\xbf\xbd")},
    {S_("null"), S_("\x00")},
    {S_("unit separator"), S_("\x1f")},
    {S_("tab"), S_("\t")},
    {S_("gt and lt"), S_("<>")},
    {S_("quotes"), S_("'\"")},
    {S_("ASCII letters"), S_("ASCII letters")},
    {S_("Unicode"), S_("\xca\x95\xe2\x8a\x99\xcf\x96\xe2\x8a\x99\xca\x94")},
    {S_("Pizza"), S_("\xf0\x9f\x8d\x95")},
};

#define JSON_PREFIX "<script type=\"application/ld+json\">"
#define JSON_SUFFIX "</script>"

typedef struct JsonStringCase {
    Alloc *a;
    HtmlTemplate *tpl;
    const JsonStringTest *tt;
} JsonStringCase;

static void json_string_case(void *env, TestingT *t) {
    JsonStringCase *jc = env;
    StringsBuilder buf = STRINGS_BUILDER(jc->a);
    Str in = jc->tt->in;
    Error err = html_template_execute(jc->tpl, strings_builder_as_io_writer(&buf),
                                      BURROW_ANY(TYPE_STRING, &in));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Cannot render template: %v", err);
    Str trimmed = strings_trim_suffix(
        strings_trim_prefix(strings_builder_string(&buf), BURROW_S(JSON_PREFIX)),
        BURROW_S(JSON_SUFFIX));
    Str got = BURROW_STR_EMPTY;
    err = json_unmarshal(jc->a, slice_from_str(jc->a, trimmed),
                         BURROW_ANY(TYPE_STRING, &got));
    if (BURROW_FAILED(err)) {
        Str inner = {trimmed.p + 1, trimmed.len - 2};
        testing_t_fatalf_v(t, "Cannot parse JS string %q as JSON: %v", inner, err);
    }
    if (!str_eq(got, in))
        testing_t_errorf_v(t, "Serialization changed the string value: got %q want %q",
                           got, in);
}

static void TestStringsInScriptsWithJsonContentTypeAreCorrectlyEscaped(TestingT *t) {
    TestCase *c = new_test_case(t);
    /* See #33671 and #37634 for more context on this. */
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tpl = html_template_parse(
        html_template_new(c->a, BURROW_S("JS string is JSON string")),
        BURROW_S(JSON_PREFIX "\"{{.}}\"" JSON_SUFFIX), &err);
    html_template_must(tpl, err);
    for (Int i = 0; i < NELEM(json_string_tests); i++) {
        JsonStringCase jc = {c->a, tpl, &json_string_tests[i]};
        testing_t_run(t, json_string_tests[i].name,
                      BURROW_FN(TestingTFunc, json_string_case, &jc));
    }
}

static void TestSkipEscapeComments(TestingT *t) {
    TestCase *c = new_test_case(t);
    ParseTree *tr = parse_new(c->a, BURROW_S("root"), (Slice){NULL, 0, 0, NULL});
    tr->mode = PARSE_PARSE_COMMENTS;
    Error err = BURROW_NO_ERROR;
    ParseTree *new_t = parse_tree_parse(
        tr, BURROW_S("{{/* A comment */}}{{ 1 }}{{/* Another comment */}}"),
        BURROW_STR_EMPTY, BURROW_STR_EMPTY,
        map_make(c->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0), (Slice){NULL, 0, 0, NULL},
        &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Cannot parse template text: %v", err);
    c->root = html_template_add_parse_tree(c->root, BURROW_S("root"), new_t, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Cannot add parse tree to template: %v", err);
    must_execute(c, c->root, nil_any, BURROW_S("1"));
}

#define TESTS(X)                                                                       \
    X(TestTemplateClone)                                                               \
    X(TestRedefineNonEmptyAfterExecution)                                              \
    X(TestRedefineEmptyAfterExecution)                                                 \
    X(TestRedefineAfterNonExecution)                                                   \
    X(TestRedefineAfterNamedExecution)                                                 \
    X(TestRedefineNestedByNameAfterExecution)                                          \
    X(TestRedefineNestedByTemplateAfterExecution)                                      \
    X(TestRedefineSafety)                                                              \
    X(TestRedefineTopUse)                                                              \
    X(TestRedefineOtherParsers)                                                        \
    X(TestNumbers)                                                                     \
    X(TestStringsInScriptsWithJsonContentTypeAreCorrectlyEscaped)                      \
    X(TestSkipEscapeComments)

TESTING_MAIN(TESTS)
