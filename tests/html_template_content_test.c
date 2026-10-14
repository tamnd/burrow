/* Derived from Go's src/html/template/content_test.go. Go source: go1.27.1.
 *
 * The tests of how each kind of typed content is escaped in each context, and
 * of the String and Error methods being used to print a value.
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

#include "check.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* Where the String and Error methods make what they return, set by
 * TestStringer. */
static Alloc *content_a;

/* -------------------------------------------------------- TestTypedContent */

typedef struct TypedContentTest {
    /* A template containing a single {{.}}. */
    Str input;
    Str want[9];
} TypedContentTest;

static const TypedContentTest typed_content_tests[] = {
    {
        S_("<style>{{.}} { color: blue }</style>"),
        {
            S_("ZgotmplZ"),
            S_("a[href =~ \"//example.com\"]#foo"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
        },
    },
    {
        S_("<div style=\"{{.}}\">"),
        {
            S_("ZgotmplZ"),
            S_("a[href =~ &#34;//example.com&#34;]#foo"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
        },
    },
    {
        S_("{{.}}"),
        {
            S_("&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;"),
            S_("a[href =~ &#34;//example.com&#34;]#foo"),
            S_("Hello, <b>World</b> &amp;tc!"),
            S_(" dir=&#34;ltr&#34;"),
            S_("c &amp;&amp; alert(&#34;Hello, World!&#34;);"),
            S_("Hello, World &amp; O&#39;Reilly\\u0021"),
            S_("greeting=H%69,&amp;addressee=(World)"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<a{{.}}>"),
        {
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_(" dir=\"ltr\""),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
            S_("ZgotmplZ"),
        },
    },
    {
        S_("<a title={{.}}>"),
        {
            S_("&lt;b&gt;&#32;&#34;foo%&#34;&#32;O&#39;Reilly&#32;&amp;bar;"),
            S_("a[href&#32;&#61;~&#32;&#34;//example.com&#34;]#foo"),
            S_("Hello,&#32;World&#32;&amp;tc!"),
            S_("&#32;dir&#61;&#34;ltr&#34;"),
            S_("c&#32;&amp;&amp;&#32;alert(&#34;Hello,&#32;World!&#34;);"),
            S_("Hello,&#32;World&#32;&amp;&#32;O&#39;Reilly\\u0021"),
            S_("greeting&#61;H%69,&amp;addressee&#61;(World)"),
            S_("greeting&#61;H%69,&amp;addressee&#61;(World)&#32;2x,&#32;https://"
               "golang."
               "org/favicon.ico&#32;500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<a title='{{.}}'>"),
        {
            S_("&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;"),
            S_("a[href =~ &#34;//example.com&#34;]#foo"),
            S_("Hello, World &amp;tc!"),
            S_(" dir=&#34;ltr&#34;"),
            S_("c &amp;&amp; alert(&#34;Hello, World!&#34;);"),
            S_("Hello, World &amp; O&#39;Reilly\\u0021"),
            S_("greeting=H%69,&amp;addressee=(World)"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<textarea>{{.}}</textarea>"),
        {
            S_("&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;"),
            S_("a[href =~ &#34;//example.com&#34;]#foo"),
            S_("Hello, &lt;b&gt;World&lt;/b&gt; &amp;tc!"),
            S_(" dir=&#34;ltr&#34;"),
            S_("c &amp;&amp; alert(&#34;Hello, World!&#34;);"),
            S_("Hello, World &amp; O&#39;Reilly\\u0021"),
            S_("greeting=H%69,&amp;addressee=(World)"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<script>alert({{.}})</script>"),
        {
            S_("\"\\u003cb\\u003e \\\"foo%\\\" O'Reilly \\u0026bar;\""),
            S_("\"a[href =~ \\\"//example.com\\\"]#foo\""),
            S_("\"Hello, \\u003cb\\u003eWorld\\u003c/b\\u003e \\u0026amp;tc!\""),
            S_("\" dir=\\\"ltr\\\"\""),
            S_("c && alert(\"Hello, World!\");"),
            S_("\"Hello, World & O'Reilly\\u0021\""),
            S_("\"greeting=H%69,\\u0026addressee=(World)\""),
            S_("\"greeting=H%69,\\u0026addressee=(World) 2x, "
               "https://golang.org/favicon."
               "ico 500.5w\""),
            S_("\",foo/,\""),
        },
    },
    {
        S_("<button onclick=\"alert({{.}})\">"),
        {
            S_("&#34;\\u003cb\\u003e \\&#34;foo%\\&#34; O&#39;Reilly \\u0026bar;&#34;"),
            S_("&#34;a[href =~ \\&#34;//example.com\\&#34;]#foo&#34;"),
            S_("&#34;Hello, \\u003cb\\u003eWorld\\u003c/b\\u003e \\u0026amp;tc!&#34;"),
            S_("&#34; dir=\\&#34;ltr\\&#34;&#34;"),
            S_("c &amp;&amp; alert(&#34;Hello, World!&#34;);"),
            S_("&#34;Hello, World &amp; O&#39;Reilly\\u0021&#34;"),
            S_("&#34;greeting=H%69,\\u0026addressee=(World)&#34;"),
            S_("&#34;greeting=H%69,\\u0026addressee=(World) 2x, "
               "https://golang.org/favic"
               "on.ico 500.5w&#34;"),
            S_("&#34;,foo/,&#34;"),
        },
    },
    {
        S_("<script>alert(\"{{.}}\")</script>"),
        {
            S_("\\u003cb\\u003e \\u0022foo%\\u0022 O\\u0027Reilly \\u0026bar;"),
            S_("a[href =~ \\u0022\\/\\/example.com\\u0022]#foo"),
            S_("Hello, \\u003cb\\u003eWorld\\u003c\\/b\\u003e \\u0026amp;tc!"),
            S_(" dir=\\u0022ltr\\u0022"),
            S_("c \\u0026\\u0026 alert(\\u0022Hello, World!\\u0022);"),
            S_("Hello, World \\u0026 O\\u0027Reilly\\u0021"),
            S_("greeting=H%69,\\u0026addressee=(World)"),
            S_("greeting=H%69,\\u0026addressee=(World) 2x, "
               "https:\\/\\/golang.org\\/favi"
               "con.ico 500.5w"),
            S_(",foo\\/,"),
        },
    },
    {
        S_("<script type=\"text/javascript\">alert(\"{{.}}\")</script>"),
        {
            S_("\\u003cb\\u003e \\u0022foo%\\u0022 O\\u0027Reilly \\u0026bar;"),
            S_("a[href =~ \\u0022\\/\\/example.com\\u0022]#foo"),
            S_("Hello, \\u003cb\\u003eWorld\\u003c\\/b\\u003e \\u0026amp;tc!"),
            S_(" dir=\\u0022ltr\\u0022"),
            S_("c \\u0026\\u0026 alert(\\u0022Hello, World!\\u0022);"),
            S_("Hello, World \\u0026 O\\u0027Reilly\\u0021"),
            S_("greeting=H%69,\\u0026addressee=(World)"),
            S_("greeting=H%69,\\u0026addressee=(World) 2x, "
               "https:\\/\\/golang.org\\/favi"
               "con.ico 500.5w"),
            S_(",foo\\/,"),
        },
    },
    {
        S_("<script type=\"text/javascript\">alert({{.}})</script>"),
        {
            S_("\"\\u003cb\\u003e \\\"foo%\\\" O'Reilly \\u0026bar;\""),
            S_("\"a[href =~ \\\"//example.com\\\"]#foo\""),
            S_("\"Hello, \\u003cb\\u003eWorld\\u003c/b\\u003e \\u0026amp;tc!\""),
            S_("\" dir=\\\"ltr\\\"\""),
            S_("c && alert(\"Hello, World!\");"),
            S_("\"Hello, World & O'Reilly\\u0021\""),
            S_("\"greeting=H%69,\\u0026addressee=(World)\""),
            S_("\"greeting=H%69,\\u0026addressee=(World) 2x, "
               "https://golang.org/favicon."
               "ico 500.5w\""),
            S_("\",foo/,\""),
        },
    },
    {
        S_("<script type=\"text/template\">{{.}}</script>"),
        {
            S_("&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;"),
            S_("a[href =~ &#34;//example.com&#34;]#foo"),
            S_("Hello, <b>World</b> &amp;tc!"),
            S_(" dir=&#34;ltr&#34;"),
            S_("c &amp;&amp; alert(&#34;Hello, World!&#34;);"),
            S_("Hello, World &amp; O&#39;Reilly\\u0021"),
            S_("greeting=H%69,&amp;addressee=(World)"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<button onclick='alert(\"{{.}}\")'>"),
        {
            S_("\\u003cb\\u003e \\u0022foo%\\u0022 O\\u0027Reilly \\u0026bar;"),
            S_("a[href =~ \\u0022\\/\\/example.com\\u0022]#foo"),
            S_("Hello, \\u003cb\\u003eWorld\\u003c\\/b\\u003e \\u0026amp;tc!"),
            S_(" dir=\\u0022ltr\\u0022"),
            S_("c \\u0026\\u0026 alert(\\u0022Hello, World!\\u0022);"),
            S_("Hello, World \\u0026 O\\u0027Reilly\\u0021"),
            S_("greeting=H%69,\\u0026addressee=(World)"),
            S_("greeting=H%69,\\u0026addressee=(World) 2x, "
               "https:\\/\\/golang.org\\/favi"
               "con.ico 500.5w"),
            S_(",foo\\/,"),
        },
    },
    {
        S_("<a href=\"?q={{.}}\">"),
        {
            S_("%3cb%3e%20%22foo%25%22%20O%27Reilly%20%26bar%3b"),
            S_("a%5bhref%20%3d~%20%22%2f%2fexample.com%22%5d%23foo"),
            S_("Hello%2c%20%3cb%3eWorld%3c%2fb%3e%20%26amp%3btc%21"),
            S_("%20dir%3d%22ltr%22"),
            S_("c%20%26%26%20alert%28%22Hello%2c%20World%21%22%29%3b"),
            S_("Hello%2c%20World%20%26%20O%27Reilly%5cu0021"),
            S_("greeting=H%69,&amp;addressee=%28World%29"),
            S_("greeting%3dH%2569%2c%26addressee%3d%28World%29%202x%2c%20https%3a%2f%"
               "2fg"
               "olang.org%2ffavicon.ico%20500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<style>body { background: url('?img={{.}}') }</style>"),
        {
            S_("%3cb%3e%20%22foo%25%22%20O%27Reilly%20%26bar%3b"),
            S_("a%5bhref%20%3d~%20%22%2f%2fexample.com%22%5d%23foo"),
            S_("Hello%2c%20%3cb%3eWorld%3c%2fb%3e%20%26amp%3btc%21"),
            S_("%20dir%3d%22ltr%22"),
            S_("c%20%26%26%20alert%28%22Hello%2c%20World%21%22%29%3b"),
            S_("Hello%2c%20World%20%26%20O%27Reilly%5cu0021"),
            S_("greeting=H%69,&addressee=%28World%29"),
            S_("greeting%3dH%2569%2c%26addressee%3d%28World%29%202x%2c%20https%3a%2f%"
               "2fg"
               "olang.org%2ffavicon.ico%20500.5w"),
            S_(",foo/,"),
        },
    },
    {
        S_("<img srcset=\"{{.}}\">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset={{.}}>"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_("&#32;dir&#61;%22ltr%22"),
            S_("#ZgotmplZ,&#32;World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting&#61;H%69%2c&amp;addressee&#61;%28World%29"),
            S_("greeting&#61;H%69,&amp;addressee&#61;(World)&#32;2x,&#32;https://"
               "golang."
               "org/favicon.ico&#32;500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset=\"{{.}} 2x, https://golang.org/ 500.5w\">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset=\"http://godoc.org/ {{.}}, https://golang.org/ 500.5w\">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset=\"http://godoc.org/?q={{.}} 2x, https://golang.org/ 500.5w\""
           ">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset=\"http://godoc.org/ 2x, {{.}} 500.5w\">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
    {
        S_("<img srcset=\"http://godoc.org/ 2x, https://golang.org/ {{.}}\">"),
        {
            S_("#ZgotmplZ"),
            S_("#ZgotmplZ"),
            S_("Hello,#ZgotmplZ"),
            S_(" dir=%22ltr%22"),
            S_("#ZgotmplZ, World!%22%29;"),
            S_("Hello,#ZgotmplZ"),
            S_("greeting=H%69%2c&amp;addressee=%28World%29"),
            S_("greeting=H%69,&amp;addressee=(World) 2x, "
               "https://golang.org/favicon.ico "
               "500.5w"),
            S_("%2cfoo/%2c"),
        },
    },
};

static Str content_plain = S_("<b> \"foo%\" O'Reilly &bar;");
static HtmlTemplateCSS content_css = S_("a[href =~ \"//example.com\"]#foo");
static HtmlTemplateHTML content_html = S_("Hello, <b>World</b> &amp;tc!");
static HtmlTemplateHTMLAttr content_attr = S_(" dir=\"ltr\"");
static HtmlTemplateJS content_js = S_("c && alert(\"Hello, World!\");");
static HtmlTemplateJSStr content_js_str = S_("Hello, World & O'Reilly\\u0021");
static HtmlTemplateURL content_url = S_("greeting=H%69,&addressee=(World)");
static HtmlTemplateSrcset content_srcset =
    S_("greeting=H%69,&addressee=(World) 2x, https://golang.org/favicon.ico 500.5w");
static HtmlTemplateURL content_url_commas = S_(",foo/,");

static void TestTypedContent(TestingT *t) {
    Any data[] = {
        BURROW_ANY(TYPE_STRING, &content_plain),
        BURROW_ANY(TYPE_HTML_TEMPLATE_CSS, &content_css),
        BURROW_ANY(TYPE_HTML_TEMPLATE_HTML, &content_html),
        BURROW_ANY(TYPE_HTML_TEMPLATE_HTML_ATTR, &content_attr),
        BURROW_ANY(TYPE_HTML_TEMPLATE_JS, &content_js),
        BURROW_ANY(TYPE_HTML_TEMPLATE_JS_STR, &content_js_str),
        BURROW_ANY(TYPE_HTML_TEMPLATE_URL, &content_url),
        BURROW_ANY(TYPE_HTML_TEMPLATE_SRCSET, &content_srcset),
        BURROW_ANY(TYPE_HTML_TEMPLATE_URL, &content_url_commas),
    };

    /* For each content sensitive escaper, see how it does on each of the typed
     * strings above. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < NELEM(typed_content_tests); i++) {
        const TypedContentTest *test = &typed_content_tests[i];
        Error err = BURROW_NO_ERROR;
        HtmlTemplate *tmpl =
            html_template_parse(html_template_new(a, BURROW_S("x")), test->input, &err);
        html_template_must(tmpl, err);
        Int pre = strings_index(test->input, BURROW_S("{{.}}"));
        Int post = test->input.len - (pre + 5);
        for (Int j = 0; j < NELEM(data); j++) {
            strings_builder_reset(&b);
            err =
                html_template_execute(tmpl, strings_builder_as_io_writer(&b), data[j]);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%q with %v: %s", test->input, data[j], err);
                continue;
            }
            Str out = strings_builder_string(&b);
            Str got = {out.p + pre, out.len - post - pre};
            Str want = test->want[j];
            if (!str_eq(want, got)) {
                testing_t_errorf_v(t, "%q with %v:\nwant\n\t%q,\ngot\n\t%q\n",
                                   test->input, data[j], want, got);
                continue;
            }
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ TestStringer */

/* Test that we print using the String method. Was issue 3073. */
#define SIG_STR(IN, OUT) OUT(Str)

typedef struct MyStringer MyStringer;
static Str my_stringer_string(MyStringer *s);
#define MY_STRINGER_FIELDS(F, T) F(T, Int, v, "")
#define MY_STRINGER_METHODS(M, T) M(T, String, my_stringer_string, SIG_STR)
BURROW_STRUCT_DECL(MyStringer, MY_STRINGER_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(MyStringer, MY_STRINGER_FIELDS, MY_STRINGER_METHODS);
BURROW_PTR_TYPE(MyStringerPtr, MyStringer);

static Str my_stringer_string(MyStringer *s) {
    return fmt_sprintf_v(content_a, "string=%d", s->v);
}

typedef struct Errorer Errorer;
static Str errorer_error(Errorer *s);
#define ERRORER_FIELDS(F, T) F(T, Int, v, "")
#define ERRORER_METHODS(M, T) M(T, Error, errorer_error, SIG_STR)
BURROW_STRUCT_DECL(Errorer, ERRORER_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(Errorer, ERRORER_FIELDS, ERRORER_METHODS);
BURROW_PTR_TYPE(ErrorerPtr, Errorer);

static Str errorer_error(Errorer *s) {
    return fmt_sprintf_v(content_a, "error=%d", s->v);
}

static void TestStringer(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    content_a = a;
    MyStringer sv = {3};
    MyStringerPtr s = &sv;
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(html_template_new(a, BURROW_S("x")),
                                             BURROW_S("{{.}}"), &err);
    html_template_must(tmpl, err);
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_OF(MyStringerPtr), &s));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    Str expect = BURROW_S("string=3");
    if (!str_eq(strings_builder_string(&b), expect))
        testing_t_errorf_v(t, "expected %q got %q", expect, strings_builder_string(&b));
    Errorer ev = {7};
    ErrorerPtr e = &ev;
    strings_builder_reset(&b);
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_OF(ErrorerPtr), &e));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    expect = BURROW_S("error=7");
    if (!str_eq(strings_builder_string(&b), expect))
        testing_t_errorf_v(t, "expected %q got %q", expect, strings_builder_string(&b));
done:
    content_a = NULL;
    arena_free(&ar);
}

/* ---------------------------------------- TestEscapingNilNonemptyInterfaces */

/* Any non-empty interface here will do; error is just ready at hand. */
#define ERROR_DATA_FIELDS(F, T) F(T, Error, E, "")
BURROW_STRUCT(ErrorData, ERROR_DATA_FIELDS);
#define ANY_DATA_FIELDS(F, T) F(T, Any, E, "")
BURROW_STRUCT(AnyData, ANY_DATA_FIELDS);

/* https://golang.org/issue/5982 */
static void TestEscapingNilNonemptyInterfaces(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(html_template_new(a, BURROW_S("x")),
                                             BURROW_S("{{.E}}"), &err);
    html_template_must(tmpl, err);

    StringsBuilder got = STRINGS_BUILDER(a);
    ErrorData test_data = {BURROW_NO_ERROR};
    (void)html_template_execute(tmpl, strings_builder_as_io_writer(&got),
                                BURROW_ANY(TYPE_OF(ErrorData), &test_data));

    /* A non-empty interface should print like an empty interface. */
    StringsBuilder want = STRINGS_BUILDER(a);
    AnyData data = {{NULL, NULL}};
    (void)html_template_execute(tmpl, strings_builder_as_io_writer(&want),
                                BURROW_ANY(TYPE_OF(AnyData), &data));

    if (!str_eq(strings_builder_string(&want), strings_builder_string(&got)))
        testing_t_errorf_v(t, "expected %q got %q", strings_builder_string(&want),
                           strings_builder_string(&got));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestTypedContent)                                                                \
    X(TestStringer)                                                                    \
    X(TestEscapingNilNonemptyInterfaces)

TESTING_MAIN(TESTS)
