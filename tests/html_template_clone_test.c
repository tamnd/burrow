/* Derived from Go's src/html/template/clone_test.go. Go source: go1.27.1.
 *
 * The tests of cloning templates, of adding parse trees to them, and of what
 * stops working once a template has been run.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/text/template.h"
#include "burrow/text/template/parse.h"

#include "check.h"

/* -------------------------------------------------------------- helpers */

/* The arena everything in a test is made in, freed when the test ends, so that
 * a test can stop with testing_t_fatalf_v as Go's do. */
typedef struct CloneTest {
    Arena ar;
    Alloc *a;
} CloneTest;

static void clone_test_free(void *env) {
    arena_free(&((CloneTest *)env)->ar);
    mem_free(heap_allocator(), env, sizeof(CloneTest), _Alignof(CloneTest));
}

static Alloc *test_alloc(TestingT *t) {
    CloneTest *e =
        (CloneTest *)mem_alloc(heap_allocator(), sizeof *e, _Alignof(CloneTest));
    arena_init(&e->ar, NULL, 0);
    e->a = arena_allocator(&e->ar);
    testing_t_cleanup(t, BURROW_FN(Func, clone_test_free, e));
    return e->a;
}

/* Must(t.Parse(text)). */
static HtmlTemplate *must_parse(HtmlTemplate *t, Str text) {
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *r = html_template_parse(t, text, &err);
    return html_template_must(r, err);
}

/* Must(t.Clone()). */
static HtmlTemplate *must_clone(HtmlTemplate *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *r = html_template_clone(t, a, &err);
    return html_template_must(r, err);
}

/* ------------------------------------------------------------------ tests */

static void TestAddParseTreeHTML(TestingT *t) {
    Alloc *a = test_alloc(t);
    HtmlTemplate *root = must_parse(
        html_template_new(a, BURROW_S("root")),
        BURROW_S("{{define \"a\"}} {{.}} {{template \"b\"}} {{.}} \"></a>{{end}}"));
    Error err = BURROW_NO_ERROR;
    Map *tree = parse_parse(
        a, BURROW_S("t"), BURROW_S("{{define \"b\"}}<a href=\"{{end}}"),
        BURROW_STR_EMPTY, BURROW_STR_EMPTY, (Slice){NULL, 0, 0, NULL}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str b_name = BURROW_S("b");
    ParseTree **b_tree = map_get(tree, &b_name);
    HtmlTemplate *added = html_template_add_parse_tree(root, b_name, *b_tree, &err);
    html_template_must(added, err);
    StringsBuilder b = STRINGS_BUILDER(a);
    Str data = BURROW_S("1>0");
    err = html_template_execute_template(added, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str got = strings_builder_string(&b);
    Str want = BURROW_S(" 1&gt;0 <a href=\" 1%3e0 \"></a>");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q want %q", got, want);
}

static void TestClone(TestingT *t) {
    Alloc *a = test_alloc(t);
    /* The {{.}} will be executed with data "<i>*" "/" in different contexts.
     * In the t0 template, it will be in a text context.
     * In the t1 template, it will be in a URL context.
     * In the t2 template, it will be in a JavaScript context.
     * In the t3 template, it will be in a CSS context. */
    Str tmpl = BURROW_S(
        "{{define \"a\"}}{{template \"lhs\"}}{{.}}{{template \"rhs\"}}{{end}}");
    StringsBuilder b = STRINGS_BUILDER(a);
    Str data = BURROW_S("<i>*/");
    Error err = BURROW_NO_ERROR;

    /* Create an incomplete template t0. */
    HtmlTemplate *t0 = must_parse(html_template_new(a, BURROW_S("t0")), tmpl);

    /* Clone t0 as t1. */
    HtmlTemplate *t1 = must_clone(t0, a);
    must_parse(t1, BURROW_S("{{define \"lhs\"}} <a href=\" {{end}}"));
    must_parse(t1, BURROW_S("{{define \"rhs\"}} \"></a> {{end}}"));

    /* Execute t1. */
    strings_builder_reset(&b);
    err = html_template_execute_template(t1, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str got = strings_builder_string(&b);
    Str want = BURROW_S(" <a href=\" %3ci%3e*/ \"></a> ");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "t1: got %q want %q", got, want);

    /* Clone t0 as t2. */
    HtmlTemplate *t2 = must_clone(t0, a);
    must_parse(t2, BURROW_S("{{define \"lhs\"}} <p onclick=\"javascript: {{end}}"));
    must_parse(t2, BURROW_S("{{define \"rhs\"}} \"></p> {{end}}"));

    /* Execute t2. */
    strings_builder_reset(&b);
    err = html_template_execute_template(t2, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = strings_builder_string(&b);
    want = BURROW_S(" <p onclick=\"javascript: &#34;\\u003ci\\u003e*/&#34; \"></p> ");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "t2: got %q want %q", got, want);

    /* Clone t0 as t3, but do not execute t3 yet. */
    HtmlTemplate *t3 = must_clone(t0, a);
    must_parse(t3, BURROW_S("{{define \"lhs\"}} <style> {{end}}"));
    must_parse(t3, BURROW_S("{{define \"rhs\"}} </style> {{end}}"));

    /* Complete t0. */
    must_parse(t0, BURROW_S("{{define \"lhs\"}} ( {{end}}"));
    must_parse(t0, BURROW_S("{{define \"rhs\"}} ) {{end}}"));

    /* Clone t0 as t4. Redefining the "lhs" template should not fail. */
    HtmlTemplate *t4 = must_clone(t0, a);
    err = BURROW_NO_ERROR;
    (void)html_template_parse(t4, BURROW_S("{{define \"lhs\"}} OK {{end}}"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "redefine \"lhs\": got err %v want nil", err);
    /* Cloning t1 should fail as it has been executed. */
    err = BURROW_NO_ERROR;
    (void)html_template_clone(t1, a, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "cloning t1: got nil err want non-nil");
    /* Redefining the "lhs" template in t1 should fail as it has been
     * executed. */
    err = BURROW_NO_ERROR;
    (void)html_template_parse(t1, BURROW_S("{{define \"lhs\"}} OK {{end}}"), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "redefine \"lhs\": got nil err want non-nil");

    /* Execute t0. */
    strings_builder_reset(&b);
    err = html_template_execute_template(t0, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = strings_builder_string(&b);
    want = BURROW_S(" ( &lt;i&gt;*/ ) ");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "t0: got %q want %q", got, want);

    /* Clone t0. This should fail, as t0 has already executed. */
    err = BURROW_NO_ERROR;
    (void)html_template_clone(t0, a, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "t0.Clone(): got nil err want non-nil");

    /* Similarly, cloning sub-templates should fail. */
    err = BURROW_NO_ERROR;
    (void)html_template_clone(html_template_lookup(t0, BURROW_S("a")), a, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "t0.Lookup(\"a\").Clone(): got nil err want non-nil");
    err = BURROW_NO_ERROR;
    (void)html_template_clone(html_template_lookup(t0, BURROW_S("lhs")), a, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "t0.Lookup(\"lhs\").Clone(): got nil err want non-nil");

    /* Execute t3. */
    strings_builder_reset(&b);
    err = html_template_execute_template(t3, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    got = strings_builder_string(&b);
    want = BURROW_S(" <style> ZgotmplZ </style> ");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "t3: got %q want %q", got, want);
}

static void TestTemplates(TestingT *t) {
    Alloc *a = test_alloc(t);
    Str names[] = {BURROW_S("t0"), BURROW_S("a"), BURROW_S("lhs"), BURROW_S("rhs")};
    /* Some template definitions borrowed from TestClone. */
    Str tmpl = BURROW_S("\n"
                        "\t\t{{define \"a\"}}{{template \"lhs\"}}{{.}}{{template "
                        "\"rhs\"}}{{end}}\n"
                        "\t\t{{define \"lhs\"}} <a href=\" {{end}}\n"
                        "\t\t{{define \"rhs\"}} \"></a> {{end}}");
    HtmlTemplate *t0 = must_parse(html_template_new(a, BURROW_S("t0")), tmpl);
    Slice templates = html_template_templates(t0, a);
    Int nnames = (Int)(sizeof names / sizeof names[0]);
    if (templates.len != nnames)
        testing_t_errorf_v(t, "expected %d templates; got %d", nnames, templates.len);
    for (Int i = 0; i < nnames; i++) {
        bool found = false;
        for (Int j = 0; j < templates.len; j++) {
            HtmlTemplate *tt = BURROW_AT(HtmlTemplate *, templates, j);
            if (str_eq(names[i], text_template_name(tt->text))) {
                found = true;
                break;
            }
        }
        if (!found)
            testing_t_error_v(t, "could not find template", names[i]);
    }
}

/* This used to crash; https://golang.org/issue/3281 */
static void TestCloneCrash(TestingT *t) {
    Alloc *a = test_alloc(t);
    HtmlTemplate *t1 = html_template_new(a, BURROW_S("all"));
    must_parse(html_template_new_associated(t1, BURROW_S("t1")),
               BURROW_S("{{define \"foo\"}}foo{{end}}"));
    Error err = BURROW_NO_ERROR;
    (void)html_template_clone(t1, a, &err);
}

/* Ensure that this guarantee from the docs is upheld: "Further calls to Parse
 * in the copy will add templates to the copy but not to the original." */
static void TestCloneThenParse(TestingT *t) {
    Alloc *a = test_alloc(t);
    HtmlTemplate *t0 =
        must_parse(html_template_new(a, BURROW_S("t0")),
                   BURROW_S("{{define \"a\"}}{{template \"embedded\"}}{{end}}"));
    HtmlTemplate *t1 = must_clone(t0, a);
    must_parse(t1, BURROW_S("{{define \"embedded\"}}t1{{end}}"));
    if (html_template_templates(t0, a).len + 1 != html_template_templates(t1, a).len)
        testing_t_error_v(t, "adding a template to a clone added it to the original");
    /* double check that the embedded template isn't available in the
     * original */
    Error err = html_template_execute_template(t0, io_discard, BURROW_S("a"),
                                               (Any){NULL, NULL});
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected 'no such template' error");
}

/* func() (string, error), the type of customFunc. */
#define SIG_CUSTOM(IN, OUT) OUT(Str) OUT(ErrorResult)
BURROW_FUNC(CustomFn, Str, Error *err);
BURROW_FUNC_TYPE_DEFINE(CustomFn, SIG_CUSTOM);

static Str custom_func(void *env, Error *err) {
    BURROW_OUT(err, errors_new((Alloc *)env, BURROW_S("issue5980")));
    return BURROW_STR_EMPTY;
}

/* https://golang.org/issue/5980 */
static void TestFuncMapWorksAfterClone(TestingT *t) {
    Alloc *a = test_alloc(t);
    CustomFn custom = {custom_func, a};
    Map *funcs = map_make(a, TYPE_STRING, TYPE_ANY, 1);
    Str name = BURROW_S("customFunc");
    Any fn = BURROW_ANY(TYPE_OF(CustomFn), &custom);
    map_set(funcs, &name, &fn);

    /* get the expected error output (no clone) */
    HtmlTemplate *uncloned =
        must_parse(html_template_funcs(html_template_new(a, BURROW_STR_EMPTY), funcs),
                   BURROW_S("{{customFunc}}"));
    Error want_err = html_template_execute(uncloned, io_discard, (Any){NULL, NULL});

    /* toClone must be the same as uncloned. It has to be recreated from
     * scratch, since cloning cannot occur after execution. */
    HtmlTemplate *to_clone =
        must_parse(html_template_funcs(html_template_new(a, BURROW_STR_EMPTY), funcs),
                   BURROW_S("{{customFunc}}"));
    HtmlTemplate *cloned = must_clone(to_clone, a);
    Error got_err = html_template_execute(cloned, io_discard, (Any){NULL, NULL});

    if (!str_eq(error_text(want_err), error_text(got_err)))
        testing_t_errorf_v(t, "clone error message mismatch want %q got %q", want_err,
                           got_err);
}

/* One of TestTemplateCloneExecuteRace's goroutines. */
static void clone_execute_loop(void *env) {
    HtmlTemplate *tmpl = env;
    Str data = BURROW_S("data");
    for (int i = 0; i < 100; i++) {
        Error err =
            html_template_execute(tmpl, io_discard, BURROW_ANY(TYPE_STRING, &data));
        if (BURROW_FAILED(err))
            panic(BURROW_ANY(TYPE_OF(Error), &err));
    }
}

/* https://golang.org/issue/16101 */
static void TestTemplateCloneExecuteRace(TestingT *t) {
    Alloc *a = test_alloc(t);
    Str input = BURROW_S("<title>{{block \"a\" .}}a{{end}}</title><body>{{block \"b\" "
                         ".}}b{{end}}<body>");
    Str overlay = BURROW_S("{{define \"b\"}}A{{end}}");
    HtmlTemplate *outer = must_parse(html_template_new(a, BURROW_S("outer")), input);
    HtmlTemplate *tmpl = must_parse(must_clone(outer, a), overlay);

    SyncWaitGroup wg = {0};
    for (int i = 0; i < 10; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, clone_execute_loop, tmpl));
    sync_wait_group_wait(&wg);
}

static void TestTemplateCloneLookup(TestingT *t) {
    Alloc *a = test_alloc(t);
    /* Template.escape makes an assumption that the template associated with
     * t.Name() is t. Check that this holds. */
    HtmlTemplate *tmpl = must_parse(html_template_new(a, BURROW_S("x")), BURROW_S("a"));
    tmpl = must_clone(tmpl, a);
    if (html_template_lookup(tmpl, html_template_name(tmpl)) != tmpl)
        testing_t_error_v(t, "after Clone, tmpl.Lookup(tmpl.Name()) != tmpl");
}

static void TestCloneGrowth(TestingT *t) {
    Alloc *a = test_alloc(t);
    HtmlTemplate *tmpl =
        must_parse(html_template_new(a, BURROW_S("root")),
                   BURROW_S("<title>{{block \"B\". }}Arg{{end}}</title>"));
    tmpl = must_clone(tmpl, a);
    must_parse(tmpl, BURROW_S("{{define \"B\"}}Text{{end}}"));
    for (int i = 0; i < 10; i++)
        (void)html_template_execute(tmpl, io_discard, (Any){NULL, NULL});
    Str defined = html_template_defined_templates(tmpl, a);
    if (defined.len > 200)
        testing_t_fatalf_v(t, "too many templates: %v", defined.len);
}

/* https://golang.org/issue/17735 */
static void TestCloneRedefinedName(TestingT *t) {
    Alloc *a = test_alloc(t);
    Str base =
        BURROW_S("\n"
                 "{{ define \"a\" -}}<title>{{ template \"b\" . -}}</title>{{ end "
                 "-}}\n"
                 "{{ define \"b\" }}{{ end -}}\n");
    Str page = BURROW_S("{{ template \"a\" . }}");

    HtmlTemplate *t1 = must_parse(html_template_new(a, BURROW_S("a")), base);

    for (int i = 0; i < 2; i++) {
        HtmlTemplate *t2 = must_clone(t1, a);
        t2 = must_parse(html_template_new_associated(t2, fmt_sprintf_v(a, "%d", i)),
                        page);
        Error err = html_template_execute(t2, io_discard, (Any){NULL, NULL});
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
    }
}

BURROW_SLICE_TYPE(StrSlice, Str);
#define PIPE_DATA_FIELDS(F, T) F(T, StrSlice, A, "")
BURROW_STRUCT(PipeData, PIPE_DATA_FIELDS);
BURROW_PTR_TYPE(PipeDataPtr, PipeData);

/* Issue 24791. */
static void TestClonePipe(TestingT *t) {
    Alloc *a = test_alloc(t);
    HtmlTemplate *ta =
        must_parse(html_template_new(a, BURROW_S("a")),
                   BURROW_S("{{define \"a\"}}{{range $v := .A}}{{$v}}{{end}}{{end}}"));
    Str hi[] = {BURROW_S("hi")};
    PipeData data = {slice_from(hi, 1, 1, TYPE_STRING)};
    PipeDataPtr pdata = &data;
    HtmlTemplate *tb = must_clone(ta, a);
    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = html_template_execute(tb, strings_builder_as_io_writer(&buf),
                                      BURROW_ANY(TYPE_OF(PipeDataPtr), &pdata));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str got = strings_builder_string(&buf);
    Str want = BURROW_S("hi");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q want %q", got, want);
}

#define TESTS(X)                                                                       \
    X(TestAddParseTreeHTML)                                                            \
    X(TestClone)                                                                       \
    X(TestTemplates)                                                                   \
    X(TestCloneCrash)                                                                  \
    X(TestCloneThenParse)                                                              \
    X(TestFuncMapWorksAfterClone)                                                      \
    X(TestTemplateCloneExecuteRace)                                                    \
    X(TestTemplateCloneLookup)                                                         \
    X(TestCloneGrowth)                                                                 \
    X(TestCloneRedefinedName)                                                          \
    X(TestClonePipe)

TESTING_MAIN(TESTS)
