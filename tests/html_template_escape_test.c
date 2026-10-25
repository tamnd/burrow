/* Derived from Go's src/html/template/escape_test.go, and the benchmark of
 * special tags in transition_test.go. Go source: go1.27.1.
 *
 * The tests of escaping whole templates. TestEscapeText, TestRedundantFuncs and
 * TestEnsurePipelineContains look at the escaper's insides, as Go's do, through
 * the internal header. C has no empty struct, so BadMarshaler and GoodMarshaler
 * have one unused field, and the message for the bad one names it as
 * *BadMarshaler where Go says *template.badMarshaler, since burrow types have
 * no package qualifier. TestMetaContentEscapeGODEBUG sets the setting through
 * burrow__ht_godebug_set where Go uses testenv.SetGODEBUG, and
 * TestEmptyTemplateHTML writes to io_discard where Go writes to os.Stdout,
 * which it never reaches. TestIssue81821, TestIssue81823 and the last two
 * TestEscapeText cases came with go1.27.2 (GO-2026-6599, CL 839866, and
 * GO-2026-6600, CL 840925).
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "../src/html/template_internal.h"
#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/strings.h"
#include "burrow/text/template.h"
#include "burrow/text/template/parse.h"

#include "check.h"

#include <stdint.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* Where the methods and functions the templates call make what they return,
 * set by each test that needs it. */
static Alloc *esc_a;

/* --------------------------------------------------------------- types */

BURROW_SLICE_TYPE(Anys, Any);
BURROW_SLICE_TYPE(StrSlice, Str);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_PTR_TYPE(StrPtr, Str);
BURROW_PTR_TYPE(StrPtrPtr, StrPtr);
BURROW_MAP_TYPE(StrStrMap, Str, Str);

#define MARSHALER_FIELDS(F, T) F(T, Int, Unused, "json:\"-\"")
BURROW_STRUCT_DECL(BadMarshaler, MARSHALER_FIELDS);
BURROW_STRUCT_DECL(GoodMarshaler, MARSHALER_FIELDS);

static Slice bad_marshaler_marshal_json(BadMarshaler *x, Alloc *a, Error *err) {
    (void)x;
    BURROW_OUT(err, BURROW_NO_ERROR);
    /* Keys in valid JSON must be double quoted as must all strings. */
    return slice_from_str(a, BURROW_S("{ foo: 'not quite valid JSON' }"));
}

static Slice good_marshaler_marshal_json(GoodMarshaler *x, Alloc *a, Error *err) {
    (void)x;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slice_from_str(a, BURROW_S("{ \"<foo>\": \"O'Reilly\" }"));
}

#define BAD_MARSHALER_METHODS(M, T)                                                    \
    M(T, MarshalJSON, bad_marshaler_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(BadMarshaler, MARSHALER_FIELDS, BAD_MARSHALER_METHODS);
#define GOOD_MARSHALER_METHODS(M, T)                                                   \
    M(T, MarshalJSON, good_marshaler_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(GoodMarshaler, MARSHALER_FIELDS, GOOD_MARSHALER_METHODS);
BURROW_PTR_TYPE(BadMarshalerPtr, BadMarshaler);
BURROW_PTR_TYPE(GoodMarshalerPtr, GoodMarshaler);

/* TestEscape's data. B and M are Go's json.Marshaler, which hold pointers. */
#define ESC_DATA_FIELDS(FIELD, TYPE)                                                   \
    FIELD(TYPE, bool, F, "")                                                           \
    FIELD(TYPE, bool, T, "")                                                           \
    FIELD(TYPE, Str, C, "")                                                            \
    FIELD(TYPE, Str, G, "")                                                            \
    FIELD(TYPE, Str, H, "")                                                            \
    FIELD(TYPE, Str, I, "")                                                            \
    FIELD(TYPE, StrSlice, A, "")                                                       \
    FIELD(TYPE, StrSlice, E, "")                                                       \
    FIELD(TYPE, Any, B, "")                                                            \
    FIELD(TYPE, Any, M, "")                                                            \
    FIELD(TYPE, Int, N, "")                                                            \
    FIELD(TYPE, Any, U, "")                                                            \
    FIELD(TYPE, IntPtr, Z, "")                                                         \
    FIELD(TYPE, HtmlTemplateHTML, W, "")
BURROW_STRUCT(EscData, ESC_DATA_FIELDS);
BURROW_PTR_TYPE(EscDataPtr, EscData);

/* TestEscapeSet's dataItem, which holds pointers to more of itself. */
BURROW_SLICE_TYPE_DECL(DataItemPtrs, DataItemPtr);
#define DATA_ITEM_FIELDS(F, T) F(T, DataItemPtrs, Children, "") F(T, Str, X, "")
BURROW_STRUCT(DataItem, DATA_ITEM_FIELDS);
BURROW_PTR_TYPE(DataItemPtr, DataItem);
BURROW_SLICE_TYPE_DEFINE(DataItemPtrs, DataItemPtr);

/* type Issue7379 int, with a method that takes an argument. */
typedef Int Issue7379;

static Str issue7379_some_method(Issue7379 *x, Int y) {
    (void)x;
    return fmt_sprintf_v(esc_a, "<%d>", y);
}

#define ISSUE7379_SIG(IN, OUT) IN(0, Int) OUT(Str)
#define ISSUE7379_METHODS(M, T) M(T, SomeMethod, issue7379_some_method, ISSUE7379_SIG)
BURROW_METHODS_DEFINE(Issue7379, ISSUE7379_METHODS);

static const Type burrow_type_Issue7379 = {
    BURROW_S_INIT("Issue7379"),
    BURROW_S_INIT("html/template"),
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    1,
    NULL,
    burrow__methods_Issue7379,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* func(a ...any) (any, error), the type of TestEscapeSet's pred. */
#define SIG_PRED(IN, OUT) IN(0, Anys) OUT(Any) OUT(ErrorResult)
BURROW_FUNC(PredFn, Any, Slice a, Error *err);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(PredFn, Any, SIG_PRED);

/* -------------------------------------------------------------- helpers */

/* The template called name parsed from text, made in a. A parse error fails the
 * test and gives NULL. */
static HtmlTemplate *must_parse(TestingT *t, Alloc *a, Str name, Str text) {
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(html_template_new(a, name), text, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%q: parse error: %s", text, err);
        return NULL;
    }
    return tmpl;
}

/* ----------------------------------------------------------- TestEscape */

typedef struct EscapeTest {
    Str name;
    Str input;
    Str output;
} EscapeTest;

typedef struct EscapeSetTest {
    Int n;
    Str names[3];
    Str bodies[3];
    Str want;
} EscapeSetTest;

typedef struct ErrorTest {
    Str input;
    Str err;
} ErrorTest;

/* The contexts TestEscapeText wants, with Go's names. */
#define ST(x) BURROW__HT_STATE_##x
#define DL(x) BURROW__HT_DELIM_##x
#define UP(x) BURROW__HT_URL_PART_##x
#define JC(x) BURROW__HT_JS_CTX_##x
#define AT(x) BURROW__HT_ATTR_##x
#define EL(x) BURROW__HT_ELEMENT_##x
#define DEPTH(d) .js_brace_depth = {d, NELEM(d), NELEM(d), TYPE_INT}

static Int depth_0[] = {0};
static Int depth_1[] = {1};
static Int depth_2_0[] = {2, 0};
static Int depth_0_0[] = {0, 0};

typedef struct EscapeTextTest {
    Str input;
    burrow__HtContext output;
} EscapeTextTest;

typedef struct PipelineTest {
    Str input;
    Str output;
    Int nids;
    Str ids[2];
} PipelineTest;

typedef struct CveTest {
    Str name;
    Str tmpl;
    Str input;
    Str want;
} CveTest;

static const EscapeTest escape_tests[] = {
    {
        S_("if"),
        S_("{{if .T}}Hello{{end}}, {{.C}}!"),
        S_("Hello, &lt;Cincinnati&gt;!"),
    },
    {
        S_("else"),
        S_("{{if .F}}{{.H}}{{else}}{{.G}}{{end}}!"),
        S_("&lt;Goodbye&gt;!"),
    },
    {
        S_("overescaping1"),
        S_("Hello, {{.C | html}}!"),
        S_("Hello, &lt;Cincinnati&gt;!"),
    },
    {
        S_("overescaping2"),
        S_("Hello, {{html .C}}!"),
        S_("Hello, &lt;Cincinnati&gt;!"),
    },
    {
        S_("overescaping3"),
        S_("{{with .C}}{{$msg := .}}Hello, {{$msg}}!{{end}}"),
        S_("Hello, &lt;Cincinnati&gt;!"),
    },
    {
        S_("assignment"),
        S_("{{if $x := .H}}{{$x}}{{end}}"),
        S_("&lt;Hello&gt;"),
    },
    {
        S_("withBody"),
        S_("{{with .H}}{{.}}{{end}}"),
        S_("&lt;Hello&gt;"),
    },
    {
        S_("withElse"),
        S_("{{with .E}}{{.}}{{else}}{{.H}}{{end}}"),
        S_("&lt;Hello&gt;"),
    },
    {
        S_("rangeBody"),
        S_("{{range .A}}{{.}}{{end}}"),
        S_("&lt;a&gt;&lt;b&gt;"),
    },
    {
        S_("rangeElse"),
        S_("{{range .E}}{{.}}{{else}}{{.H}}{{end}}"),
        S_("&lt;Hello&gt;"),
    },
    {
        S_("nonStringValue"),
        S_("{{.T}}"),
        S_("true"),
    },
    {
        S_("untypedNilValue"),
        S_("{{.U}}"),
        S_(""),
    },
    {
        S_("typedNilValue"),
        S_("{{.Z}}"),
        S_("&lt;nil&gt;"),
    },
    {
        S_("constant"),
        S_("<a href=\"/search?q={{\"'a<b'\"}}\">"),
        S_("<a href=\"/search?q=%27a%3cb%27\">"),
    },
    {
        S_("multipleAttrs"),
        S_("<a b=1 c={{.H}}>"),
        S_("<a b=1 c=&lt;Hello&gt;>"),
    },
    {
        S_("urlStartRel"),
        S_("<a href='{{\"/foo/bar?a=b&c=d\"}}'>"),
        S_("<a href='/foo/bar?a=b&amp;c=d'>"),
    },
    {
        S_("urlStartAbsOk"),
        S_("<a href='{{\"http://example.com/foo/bar?a=b&c=d\"}}'>"),
        S_("<a href='http://example.com/foo/bar?a=b&amp;c=d'>"),
    },
    {
        S_("protocolRelativeURLStart"),
        S_("<a href='{{\"//example.com:8000/foo/bar?a=b&c=d\"}}'>"),
        S_("<a href='//example.com:8000/foo/bar?a=b&amp;c=d'>"),
    },
    {
        S_("pathRelativeURLStart"),
        S_("<a href=\"{{\"/javascript:80/foo/bar\"}}\">"),
        S_("<a href=\"/javascript:80/foo/bar\">"),
    },
    {
        S_("dangerousURLStart"),
        S_("<a href='{{\"javascript:alert(%22pwned%22)\"}}'>"),
        S_("<a href='#ZgotmplZ'>"),
    },
    {
        S_("dangerousURLStart2"),
        S_("<a href='  {{\"javascript:alert(%22pwned%22)\"}}'>"),
        S_("<a href='  #ZgotmplZ'>"),
    },
    {
        S_("nonHierURL"),
        S_("<a href={{\"mailto:Muhammed \\\"The Greatest\\\" Ali <m.ali@example.com>"
           "\"}}>"),
        S_("<a href=mailto:Muhammed%20%22The%20Greatest%22%20Ali%20%3cm.ali@example."
           "com%3e>"),
    },
    {
        S_("urlPath"),
        S_("<a href='http://{{\"javascript:80\"}}/foo'>"),
        S_("<a href='http://javascript:80/foo'>"),
    },
    {
        S_("urlQuery"),
        S_("<a href='/search?q={{.H}}'>"),
        S_("<a href='/search?q=%3cHello%3e'>"),
    },
    {
        S_("urlFragment"),
        S_("<a href='/faq#{{.H}}'>"),
        S_("<a href='/faq#%3cHello%3e'>"),
    },
    {
        S_("urlBranch"),
        S_("<a href=\"{{if .F}}/foo?a=b{{else}}/bar{{end}}\">"),
        S_("<a href=\"/bar\">"),
    },
    {
        S_("urlBranchConflictMoot"),
        S_("<a href=\"{{if .T}}/foo?a={{else}}/bar#{{end}}{{.C}}\">"),
        S_("<a href=\"/foo?a=%3cCincinnati%3e\">"),
    },
    {
        S_("jsStrValue"),
        S_("<button onclick='alert({{.H}})'>"),
        S_("<button onclick='alert(&#34;\\u003cHello\\u003e&#34;)'>"),
    },
    {
        S_("jsNumericValue"),
        S_("<button onclick='alert({{.N}})'>"),
        S_("<button onclick='alert( 42 )'>"),
    },
    {
        S_("jsBoolValue"),
        S_("<button onclick='alert({{.T}})'>"),
        S_("<button onclick='alert( true )'>"),
    },
    {
        S_("jsNilValueTyped"),
        S_("<button onclick='alert(typeof{{.Z}})'>"),
        S_("<button onclick='alert(typeof null )'>"),
    },
    {
        S_("jsNilValueUntyped"),
        S_("<button onclick='alert(typeof{{.U}})'>"),
        S_("<button onclick='alert(typeof null )'>"),
    },
    {
        S_("jsObjValue"),
        S_("<button onclick='alert({{.A}})'>"),
        S_("<button onclick='alert([&#34;\\u003ca\\u003e&#34;,&#34;\\u003cb\\u003e&#"
           "34;])'>"),
    },
    {
        S_("jsObjValueScript"),
        S_("<script>alert({{.A}})</script>"),
        S_("<script>alert([\"\\u003ca\\u003e\",\"\\u003cb\\u003e\"])</script>"),
    },
    {
        S_("scriptTypeSpace"),
        S_("<script type=\" \">{{.H}}</script>"),
        S_("<script type=\" \">\"\\u003cHello\\u003e\"</script>"),
    },
    {
        S_("scriptTypeTab"),
        S_("<script type=\"\t\">{{.H}}</script>"),
        S_("<script type=\"\t\">\"\\u003cHello\\u003e\"</script>"),
    },
    {
        S_("scriptTypeEmpty"),
        S_("<script type=\"\">{{.H}}</script>"),
        S_("<script type=\"\">\"\\u003cHello\\u003e\"</script>"),
    },
    {
        S_("jsObjValueNotOverEscaped"),
        S_("<button onclick='alert({{.A | html}})'>"),
        S_("<button onclick='alert([&#34;\\u003ca\\u003e&#34;,&#34;\\u003cb\\u003e&#"
           "34;])'>"),
    },
    {
        S_("jsStr"),
        S_("<button onclick='alert(&quot;{{.H}}&quot;)'>"),
        S_("<button onclick='alert(&quot;\\u003cHello\\u003e&quot;)'>"),
    },
    {
        S_("badMarshaler"),
        S_("<button onclick='alert(1/{{.B}}in numbers)'>"),
        S_("<button onclick='alert(1/ /* json: error calling MarshalJSON for type *B"
           "adMarshaler: invalid character &#39;f&#39; looking for beginning of obje"
           "ct key string */null in numbers)'>"),
    },
    {
        S_("jsMarshaler"),
        S_("<button onclick='alert({{.M}})'>"),
        S_("<button onclick='alert({&#34;\\u003cfoo\\u003e&#34;:&#34;O&#39;Reilly&#3"
           "4;})'>"),
    },
    {
        S_("jsStrNotUnderEscaped"),
        S_("<button onclick='alert({{.C | urlquery}})'>"),
        S_("<button onclick='alert(&#34;%3CCincinnati%3E&#34;)'>"),
    },
    {
        S_("jsRe"),
        S_("<button onclick='alert(/{{\"foo+bar\"}}/.test(\"\"))'>"),
        S_("<button onclick='alert(/foo\\u002bbar/.test(\"\"))'>"),
    },
    {
        S_("jsReBlank"),
        S_("<script>alert(/{{\"\"}}/.test(\"\"));</script>"),
        S_("<script>alert(/(?:)/.test(\"\"));</script>"),
    },
    {
        S_("jsReAmbigOk"),
        S_("<script>{{if true}}var x = 1{{end}}</script>"),
        S_("<script>var x = 1</script>"),
    },
    {
        S_("styleBidiKeywordPassed"),
        S_("<p style=\"dir: {{\"ltr\"}}\">"),
        S_("<p style=\"dir: ltr\">"),
    },
    {
        S_("styleBidiPropNamePassed"),
        S_("<p style=\"border-{{\"left\"}}: 0; border-{{\"right\"}}: 1in\">"),
        S_("<p style=\"border-left: 0; border-right: 1in\">"),
    },
    {
        S_("styleExpressionBlocked"),
        S_("<p style=\"width: {{\"expression(alert(1337))\"}}\">"),
        S_("<p style=\"width: ZgotmplZ\">"),
    },
    {
        S_("styleTagSelectorPassed"),
        S_("<style>{{\"p\"}} { color: pink }</style>"),
        S_("<style>p { color: pink }</style>"),
    },
    {
        S_("styleIDPassed"),
        S_("<style>p{{\"#my-ID\"}} { font: Arial }</style>"),
        S_("<style>p#my-ID { font: Arial }</style>"),
    },
    {
        S_("styleClassPassed"),
        S_("<style>p{{\".my_class\"}} { font: Arial }</style>"),
        S_("<style>p.my_class { font: Arial }</style>"),
    },
    {
        S_("styleQuantityPassed"),
        S_("<a style=\"left: {{\"2em\"}}; top: {{0}}\">"),
        S_("<a style=\"left: 2em; top: 0\">"),
    },
    {
        S_("stylePctPassed"),
        S_("<table style=width:{{\"100%\"}}>"),
        S_("<table style=width:100%>"),
    },
    {
        S_("styleColorPassed"),
        S_("<p style=\"color: {{\"#8ff\"}}; background: {{\"#000\"}}\">"),
        S_("<p style=\"color: #8ff; background: #000\">"),
    },
    {
        S_("styleObfuscatedExpressionBlocked"),
        S_("<p style=\"width: {{\"  e\\\\78preS\\x00Sio/**/n(alert(1337))\"}}\">"),
        S_("<p style=\"width: ZgotmplZ\">"),
    },
    {
        S_("styleMozBindingBlocked"),
        S_("<p style=\"{{\"-moz-binding(alert(1337))\"}}: ...\">"),
        S_("<p style=\"ZgotmplZ: ...\">"),
    },
    {
        S_("styleObfuscatedMozBindingBlocked"),
        S_("<p style=\"{{\"  -mo\\\\7a-B\\x00I/**/nding(alert(1337))\"}}: ...\">"),
        S_("<p style=\"ZgotmplZ: ...\">"),
    },
    {
        S_("styleFontNameString"),
        S_("<p style='font-family: \"{{\"Times New Roman\"}}\"'>"),
        S_("<p style='font-family: \"Times New Roman\"'>"),
    },
    {
        S_("styleFontNameString"),
        S_("<p style='font-family: \"{{\"Times New Roman\"}}\", \"{{\"sans-serif\"}}"
           "\"'>"),
        S_("<p style='font-family: \"Times New Roman\", \"sans-serif\"'>"),
    },
    {
        S_("styleFontNameUnquoted"),
        S_("<p style='font-family: {{\"Times New Roman\"}}'>"),
        S_("<p style='font-family: Times New Roman'>"),
    },
    {
        S_("styleURLQueryEncoded"),
        S_("<p style=\"background: url(/img?name={{\"O'Reilly Animal(1)<2>.png\"}})\""
           ">"),
        S_("<p style=\"background: url(/img?name=O%27Reilly%20Animal%281%29%3c2%3e.p"
           "ng)\">"),
    },
    {
        S_("styleQuotedURLQueryEncoded"),
        S_("<p style=\"background: url('/img?name={{\"O'Reilly Animal(1)<2>.png\"}}'"
           ")\">"),
        S_("<p style=\"background: url('/img?name=O%27Reilly%20Animal%281%29%3c2%3e."
           "png')\">"),
    },
    {
        S_("styleStrQueryEncoded"),
        S_("<p style=\"background: '/img?name={{\"O'Reilly Animal(1)<2>.png\"}}'\">"),
        S_("<p style=\"background: '/img?name=O%27Reilly%20Animal%281%29%3c2%3e.png'"
           "\">"),
    },
    {
        S_("styleURLBadProtocolBlocked"),
        S_("<a style=\"background: url('{{\"javascript:alert(1337)\"}}')\">"),
        S_("<a style=\"background: url('#ZgotmplZ')\">"),
    },
    {
        S_("styleStrBadProtocolBlocked"),
        S_("<a style=\"background: '{{\"vbscript:alert(1337)\"}}'\">"),
        S_("<a style=\"background: '#ZgotmplZ'\">"),
    },
    {
        S_("styleStrEncodedProtocolEncoded"),
        S_("<a style=\"background: '{{\"javascript\\\\3a alert(1337)\"}}'\">"),
        S_("<a style=\"background: 'javascript\\\\3a alert\\28 1337\\29 '\">"),
    },
    {
        S_("styleURLGoodProtocolPassed"),
        S_("<a style=\"background: url('{{\"http://oreilly.com/O'Reilly Animals(1)<2"
           ">;{}.html\"}}')\">"),
        S_("<a style=\"background: url('http://oreilly.com/O%27Reilly%20Animals%281%"
           "29%3c2%3e;%7b%7d.html')\">"),
    },
    {
        S_("styleStrGoodProtocolPassed"),
        S_("<a style=\"background: '{{\"http://oreilly.com/O'Reilly Animals(1)<2>;{}"
           ".html\"}}'\">"),
        S_("<a style=\"background: 'http\\3a\\2f\\2foreilly.com\\2fO\\27Reilly Anima"
           "ls\\28 1\\29\\3c 2\\3e\\3b\\7b\\7d.html'\">"),
    },
    {
        S_("styleURLEncodedForHTMLInAttr"),
        S_("<a style=\"background: url('{{\"/search?img=foo&size=icon\"}}')\">"),
        S_("<a style=\"background: url('/search?img=foo&amp;size=icon')\">"),
    },
    {
        S_("styleURLNotEncodedForHTMLInCdata"),
        S_("<style>body { background: url('{{\"/search?img=foo&size=icon\"}}') }</st"
           "yle>"),
        S_("<style>body { background: url('/search?img=foo&size=icon') }</style>"),
    },
    {
        S_("styleURLMixedCase"),
        S_("<p style=\"background: URL(#{{.H}})\">"),
        S_("<p style=\"background: URL(#%3cHello%3e)\">"),
    },
    {
        S_("stylePropertyPairPassed"),
        S_("<a style='{{\"color: red\"}}'>"),
        S_("<a style='color: red'>"),
    },
    {
        S_("styleStrSpecialsEncoded"),
        S_("<a style=\"font-family: '{{\"/**/'\\\";:// \\\\\"}}', &quot;{{\"/**/'\\\""
           ";:// \\\\\"}}&quot;\">"),
        S_("<a style=\"font-family: '\\2f**\\2f\\27\\22\\3b\\3a\\2f\\2f  \\\\', &quo"
           "t;\\2f**\\2f\\27\\22\\3b\\3a\\2f\\2f  \\\\&quot;\">"),
    },
    {
        S_("styleURLSpecialsEncoded"),
        S_("<a style=\"border-image: url({{\"/**/'\\\";:// \\\\\"}}), url(&quot;{{\""
           "/**/'\\\";:// \\\\\"}}&quot;), url('{{\"/**/'\\\";:// \\\\\"}}'), 'http:"
           "//www.example.com/?q={{\"/**/'\\\";:// \\\\\"}}''\">"),
        S_("<a style=\"border-image: url(/**/%27%22;://%20%5c), url(&quot;/**/%27%22"
           ";://%20%5c&quot;), url('/**/%27%22;://%20%5c'), 'http://www.example.com/"
           "?q=%2f%2a%2a%2f%27%22%3b%3a%2f%2f%20%5c''\">"),
    },
    {
        S_("HTML comment"),
        S_("<b>Hello, <!-- name of world -->{{.C}}</b>"),
        S_("<b>Hello, &lt;Cincinnati&gt;</b>"),
    },
    {
        S_("HTML comment not first < in text node."),
        S_("<<!-- -->!--"),
        S_("&lt;!--"),
    },
    {
        S_("HTML normalization 1"),
        S_("a < b"),
        S_("a &lt; b"),
    },
    {
        S_("HTML normalization 2"),
        S_("a << b"),
        S_("a &lt;&lt; b"),
    },
    {
        S_("HTML normalization 3"),
        S_("a<<!-- --><!-- -->b"),
        S_("a&lt;b"),
    },
    {
        S_("HTML doctype not normalized"),
        S_("<!DOCTYPE html>Hello, World!"),
        S_("<!DOCTYPE html>Hello, World!"),
    },
    {
        S_("HTML doctype not case-insensitive"),
        S_("<!doCtYPE htMl>Hello, World!"),
        S_("<!doCtYPE htMl>Hello, World!"),
    },
    {
        S_("No doctype injection"),
        S_("<!{{\"DOCTYPE\"}}"),
        S_("&lt;!DOCTYPE"),
    },
    {
        S_("Split HTML comment"),
        S_("<b>Hello, <!-- name of {{if .T}}city -->{{.C}}{{else}}world -->{{.W}}{{e"
           "nd}}</b>"),
        S_("<b>Hello, &lt;Cincinnati&gt;</b>"),
    },
    {
        S_("JS line comment"),
        S_("<script>for (;;) { if (c()) break// foo not a label\nfoo({{.T}});}</scri"
           "pt>"),
        S_("<script>for (;;) { if (c()) break\nfoo( true );}</script>"),
    },
    {
        S_("JS multiline block comment"),
        S_("<script>for (;;) { if (c()) break/* foo not a label\n */foo({{.T}});}</s"
           "cript>"),
        S_("<script>for (;;) { if (c()) break\nfoo( true );}</script>"),
    },
    {
        S_("JS single-line block comment"),
        S_("<script>for (;;) {\nif (c()) break/* foo a label */foo;x({{.T}});}</scri"
           "pt>"),
        S_("<script>for (;;) {\nif (c()) break foo;x( true );}</script>"),
    },
    {
        S_("JS block comment flush with mathematical division"),
        S_("<script>var a/*b*//c\nd</script>"),
        S_("<script>var a /c\nd</script>"),
    },
    {
        S_("JS mixed comments"),
        S_("<script>var a/*b*///c\nd</script>"),
        S_("<script>var a \nd</script>"),
    },
    {
        S_("JS HTML-like comments"),
        S_("<script>before <!-- beep\nbetween\nbefore-->boop\n</script>"),
        S_("<script>before \nbetween\nbefore\n</script>"),
    },
    {
        S_("JS hashbang comment"),
        S_("<script>#! beep\n</script>"),
        S_("<script>\n</script>"),
    },
    {
        S_("Special tags in <script> string literals"),
        S_("<script>var a = \"asd < 123 <!-- 456 < fgh <script jkl < 789 </script\"<"
           "/script>"),
        S_("<script>var a = \"asd < 123 \\x3C!-- 456 < fgh \\x3Cscript jkl < 789 \\x"
           "3C/script\"</script>"),
    },
    {
        S_("Special tags in <script> string literals (mixed case)"),
        S_("<script>var a = \"<!-- <ScripT </ScripT\"</script>"),
        S_("<script>var a = \"\\x3C!-- \\x3CScripT \\x3C/ScripT\"</script>"),
    },
    {
        S_("Special tags in <script> regex literals (mixed case)"),
        S_("<script>var a = /<!-- <ScripT </ScripT/</script>"),
        S_("<script>var a = /\\x3C!-- \\x3CScripT \\x3C/ScripT/</script>"),
    },
    {
        S_("CSS comments"),
        S_("<style>p// paragraph\n{border: 1px/* color */{{\"#00f\"}}}</style>"),
        S_("<style>p\n{border: 1px #00f}</style>"),
    },
    {
        S_("JS attr block comment"),
        S_("<a onclick=\"f(&quot;&quot;); /* alert({{.H}}) */\">"),
        S_("<a onclick=\"f(&quot;&quot;); /* alert() */\">"),
    },
    {
        S_("JS attr line comment"),
        S_("<a onclick=\"// alert({{.G}})\">"),
        S_("<a onclick=\"// alert()\">"),
    },
    {
        S_("CSS attr block comment"),
        S_("<a style=\"/* color: {{.H}} */\">"),
        S_("<a style=\"/* color:  */\">"),
    },
    {
        S_("CSS attr line comment"),
        S_("<a style=\"// color: {{.G}}\">"),
        S_("<a style=\"// color: \">"),
    },
    {
        S_("HTML substitution commented out"),
        S_("<p><!-- {{.H}} --></p>"),
        S_("<p></p>"),
    },
    {
        S_("Comment ends flush with start"),
        S_("<!--{{.}}--><script>/*{{.}}*///{{.}}\n</script><style>/*{{.}}*///{{.}}\n"
           "</style><a onclick='/*{{.}}*///{{.}}' style='/*{{.}}*///{{.}}'>"),
        S_("<script> \n</script><style> \n</style><a onclick='/**///' style='/**///'"
           ">"),
    },
    {
        S_("typed HTML in text"),
        S_("{{.W}}"),
        S_("&iexcl;<b class=\"foo\">Hello</b>, <textarea>O'World</textarea>!"),
    },
    {
        S_("typed HTML in attribute"),
        S_("<div title=\"{{.W}}\">"),
        S_("<div title=\"&iexcl;Hello, O&#39;World!\">"),
    },
    {
        S_("typed HTML in script"),
        S_("<button onclick=\"alert({{.W}})\">"),
        S_("<button onclick=\"alert(&#34;\\u0026iexcl;\\u003cb class=\\&#34;foo\\&#3"
           "4;\\u003eHello\\u003c/b\\u003e, \\u003ctextarea\\u003eO&#39;World\\u003c"
           "/textarea\\u003e!&#34;)\">"),
    },
    {
        S_("typed HTML in RCDATA"),
        S_("<textarea>{{.W}}</textarea>"),
        S_("<textarea>&iexcl;&lt;b class=&#34;foo&#34;&gt;Hello&lt;/b&gt;, &lt;texta"
           "rea&gt;O&#39;World&lt;/textarea&gt;!</textarea>"),
    },
    {
        S_("range in textarea"),
        S_("<textarea>{{range .A}}{{.}}{{end}}</textarea>"),
        S_("<textarea>&lt;a&gt;&lt;b&gt;</textarea>"),
    },
    {
        S_("No tag injection"),
        S_("{{\"10$\"}}<{{\"script src,evil.org/pwnd.js\"}}..."),
        S_("10$&lt;script src,evil.org/pwnd.js..."),
    },
    {
        S_("No comment injection"),
        S_("<{{\"!--\"}}"),
        S_("&lt;!--"),
    },
    {
        S_("No RCDATA end tag injection"),
        S_("<textarea><{{\"/textarea \"}}...</textarea>"),
        S_("<textarea>&lt;/textarea ...</textarea>"),
    },
    {
        S_("optional attrs"),
        S_("<img class=\"{{\"iconClass\"}}\"{{if .T}} id=\"{{\"<iconId>\"}}\"{{end}}"
           " src={{if .T}}\"?{{\"<iconPath>\"}}\"{{else}}\"images/cleardot.gif\"{{en"
           "d}}{{if .T}}title=\"{{\"<title>\"}}\"{{end}} alt=\"{{if .T}}{{\"<alt>\"}"
           "}{{else}}{{if .F}}{{\"<title>\"}}{{end}}{{end}}\">"),
        S_("<img class=\"iconClass\" id=\"&lt;iconId&gt;\" src=\"?%3ciconPath%3e\"ti"
           "tle=\"&lt;title&gt;\" alt=\"&lt;alt&gt;\">"),
    },
    {
        S_("conditional valueless attr name"),
        S_("<input{{if .T}} checked{{end}} name=n>"),
        S_("<input checked name=n>"),
    },
    {
        S_("conditional dynamic valueless attr name 1"),
        S_("<input{{if .T}} {{\"checked\"}}{{end}} name=n>"),
        S_("<input checked name=n>"),
    },
    {
        S_("conditional dynamic valueless attr name 2"),
        S_("<input {{if .T}}{{\"checked\"}} {{end}}name=n>"),
        S_("<input checked name=n>"),
    },
    {
        S_("dynamic attribute name"),
        S_("<img on{{\"load\"}}=\"alert({{\"loaded\"}})\">"),
        S_("<img onload=\"alert(&#34;loaded&#34;)\">"),
    },
    {
        S_("bad dynamic attribute name 1"),
        S_("<input {{\"onchange\"}}=\"{{\"doEvil()\"}}\">"),
        S_("<input ZgotmplZ=\"doEvil()\">"),
    },
    {
        S_("bad dynamic attribute name 2"),
        S_("<div {{\"sTyle\"}}=\"{{\"color: expression(alert(1337))\"}}\">"),
        S_("<div ZgotmplZ=\"color: expression(alert(1337))\">"),
    },
    {
        S_("bad dynamic attribute name 3"),
        S_("<img {{\"src\"}}=\"{{\"javascript:doEvil()\"}}\">"),
        S_("<img ZgotmplZ=\"javascript:doEvil()\">"),
    },
    {
        S_("bad dynamic attribute name 4"),
        S_("<input checked {{\"\"}}=\"Whose value am I?\">"),
        S_("<input checked ZgotmplZ=\"Whose value am I?\">"),
    },
    {
        S_("dynamic element name"),
        S_("<h{{3}}><table><t{{\"head\"}}>...</h{{3}}>"),
        S_("<h3><table><thead>...</h3>"),
    },
    {
        S_("bad dynamic element name"),
        S_("<{{\"script\"}}>{{\"doEvil()\"}}</{{\"script\"}}>"),
        S_("&lt;script>doEvil()&lt;/script>"),
    },
    {
        S_("srcset bad URL in second position"),
        S_("<img srcset=\"{{\"/not-an-image#,javascript:alert(1)\"}}\">"),
        S_("<img srcset=\"/not-an-image#,#ZgotmplZ\">"),
    },
    {
        S_("srcset buffer growth"),
        S_("<img srcset={{\",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\"}}>"),
        S_("<img srcset=,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,>"),
    },
    {
        S_("unquoted empty attribute value (plaintext)"),
        S_("<p name={{.U}}>"),
        S_("<p name=ZgotmplZ>"),
    },
    {
        S_("unquoted empty attribute value (url)"),
        S_("<p href={{.U}}>"),
        S_("<p href=ZgotmplZ>"),
    },
    {
        S_("quoted empty attribute value"),
        S_("<p name=\"{{.U}}\">"),
        S_("<p name=\"\">"),
    },
    {
        S_("JS template lit special characters"),
        S_("<script>var a = `{{.I}}`</script>"),
        S_("<script>var a = `\\u0024\\u007b asd \\u0060\\u0060 \\u007d`</script>"),
    },
    {
        S_("JS template lit special characters, nested lit"),
        S_("<script>var a = `${ `{{.I}}` }`</script>"),
        S_("<script>var a = `${ `\\u0024\\u007b asd \\u0060\\u0060 \\u007d` }`</scri"
           "pt>"),
    },
    {
        S_("JS template lit, nested JS"),
        S_("<script>var a = `${ var a = \"{{\"a \\\" d\"}}\" }`</script>"),
        S_("<script>var a = `${ var a = \"a \\u0022 d\" }`</script>"),
    },
    {
        S_("meta content attribute url"),
        S_("<meta http-equiv=\"refresh\" content=\"asd; url={{\"javascript:alert(1)\""
           "}}; asd; url={{\"vbscript:alert(1)\"}}; asd\">"),
        S_("<meta http-equiv=\"refresh\" content=\"asd; url=#ZgotmplZ; asd; url=#Zgo"
           "tmplZ; asd\">"),
    },
    {
        S_("meta content string"),
        S_("<meta http-equiv=\"refresh\" content=\"{{\"asd: 123\"}}\">"),
        S_("<meta http-equiv=\"refresh\" content=\"asd: 123\">"),
    },
    {
        S_("meta content url with whitespace before equals"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url ={{\"javascript:alert(1)\"}"
           "}\">"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url =#ZgotmplZ\">"),
    },
    {
        S_("meta content url with tab before equals"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url\t={{\"javascript:alert(1)\""
           "}}\">"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url\t=#ZgotmplZ\">"),
    },
    {
        S_("meta content url with space after equals"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url= {{\"javascript:alert(1)\"}"
           "}\">"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url= #ZgotmplZ\">"),
    },
    {
        S_("meta content url with whitespace both sides of equals"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url \t= {{\"javascript:alert(1)"
           "\"}}\">"),
        S_("<meta http-equiv=\"refresh\" content=\"0;url \t= #ZgotmplZ\">"),
    },
};

static const EscapeSetTest escape_set_tests[] = {
    {
        1,
        {S_("main")},
        {S_("")},
        S_(""),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("Hello, {{template \"helper\"}}!"), S_("{{\"<World>\"}}")},
        S_("Hello, &lt;World&gt;!"),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("<a onclick='a = {{template \"helper\"}};'>"), S_("{{\"<a>\"}}<b")},
        S_("<a onclick='a = &#34;\\u003ca\\u003e&#34;<b;'>"),
    },
    {
        1,
        {S_("main")},
        {S_("{{range .Children}}{{template \"main\" .}}{{else}}{{.X}} {{end}}")},
        S_("foo &lt;bar&gt; baz "),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("{{template \"helper\" .}}"),
         S_("{{if .Children}}<ul>{{range .Children}}<li>{{template \"main\" .}}</li>{"
            "{end}}</ul>{{else}}{{.X}}{{end}}")},
        S_("<ul><li>foo</li><li>&lt;bar&gt;</li><li><ul><li>baz</li></ul></li></ul>"),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("<blockquote>{{range .Children}}{{template \"helper\" .}}{{end}}</blockqu"
            "ote>"),
         S_("{{if .Children}}{{template \"main\" .}}{{else}}{{.X}}<br>{{end}}")},
        S_("<blockquote>foo<br>&lt;bar&gt;<br><blockquote>baz<br></blockquote></bloc"
           "kquote>"),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("<button onclick=\"title='{{template \"helper\"}}'; ...\">{{template \"he"
            "lper\"}}</button>"),
         S_("{{11}} of {{\"<100>\"}}")},
        S_("<button onclick=\"title='11 of \\u003c100\\u003e'; ...\">11 of &lt;100&g"
           "t;</button>"),
    },
    {
        2,
        {S_("main"), S_("helper")},
        {S_("<script>var x={{template \"helper\"}}/{{\"42\"}};</script>"),
         S_("{{126}}")},
        S_("<script>var x= 126 /\"42\";</script>"),
    },
    {
        2,
        {S_("main"), S_("countdown")},
        {S_("<script>var x=[{{template \"countdown\" 4}}];</script>"),
         S_("{{.}}{{if .}},{{template \"countdown\" . | pred}}{{end}}")},
        S_("<script>var x=[ 4 , 3 , 2 , 1 , 0 ];</script>"),
    },
};

static const ErrorTest error_tests[] = {
    {
        S_("{{if .Cond}}<a>{{else}}<b>{{end}}"),
        S_(""),
    },
    {
        S_("{{if .Cond}}<a>{{end}}"),
        S_(""),
    },
    {
        S_("{{if .Cond}}{{else}}<b>{{end}}"),
        S_(""),
    },
    {
        S_("{{with .Cond}}<div>{{end}}"),
        S_(""),
    },
    {
        S_("{{range .Items}}<a>{{end}}"),
        S_(""),
    },
    {
        S_("<a href='/foo?{{range .Items}}&{{.K}}={{.V}}{{end}}'>"),
        S_(""),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{end}}>{{end}}"),
        S_(""),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{end}}>{{continue}}{{end}}"),
        S_(""),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{end}}>{{break}}{{end}}"),
        S_(""),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{end}}>{{if .X}}{{break}}{{end}}{{end}}"),
        S_(""),
    },
    {
        S_("<script>var a = `${a+b}`</script>`"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `asd`;</script>"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `${1}`;</script>"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `${return ``}`;</script>"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `${return {{.}} }`;</script>"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `${ let a = {1:1} {{.}} }`;</script>"),
        S_(""),
    },
    {
        S_("<script>var tmpl = `asd ${return \"{\"}`;</script>"),
        S_(""),
    },
    {
        S_("{{if eq \"\" \"\"}}<meta>{{end}}"),
        S_(""),
    },
    {
        S_("{{if eq \"\" \"\"}}<meta content=\"url={{\"asd\"}}\">{{end}}"),
        S_(""),
    },
    {
        S_("{{if .Cond}}<a{{end}}"),
        S_("z:1:5: {{if}} branches"),
    },
    {
        S_("{{if .Cond}}\n{{else}}\n<a{{end}}"),
        S_("z:1:5: {{if}} branches"),
    },
    {
        S_("{{if .Cond}}<a href=\"foo\">{{else}}<a href=\"bar>{{end}}"),
        S_("z:1:5: {{if}} branches"),
    },
    {
        S_("<a {{if .Cond}}href='{{else}}title='{{end}}{{.X}}'>"),
        S_("z:1:8: {{if}} branches"),
    },
    {
        S_("\n{{with .X}}<a{{end}}"),
        S_("z:2:7: {{with}} branches"),
    },
    {
        S_("\n{{with .X}}<a>{{else}}<a{{end}}"),
        S_("z:2:7: {{with}} branches"),
    },
    {
        S_("{{range .Items}}<a{{end}}"),
        S_("z:1: on range loop re-entry: \"<\" in attribute name: \"<a\""),
    },
    {
        S_("\n{{range .Items}} x='<a{{end}}"),
        S_("z:2:8: on range loop re-entry: {{range}} branches"),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{break}}{{end}}>{{end}}"),
        S_("z:1:29: at range loop break: {{range}} branches end in different context"
           "s"),
    },
    {
        S_("{{range .Items}}<a{{if .X}}{{continue}}{{end}}>{{end}}"),
        S_("z:1:29: at range loop continue: {{range}} branches end in different cont"
           "exts"),
    },
    {
        S_("{{range .Items}}{{if .X}}{{break}}{{end}}<a{{if .Y}}{{continue}}{{end}}>"
           "{{if .Z}}{{continue}}{{end}}{{end}}"),
        S_("z:1:54: at range loop continue: {{range}} branches end in different cont"
           "exts"),
    },
    {
        S_("<a b=1 c={{.H}}"),
        S_("z: ends in a non-text context: {stateAttr delimSpaceOrTagEnd"),
    },
    {
        S_("<script>foo();"),
        S_("z: ends in a non-text context: {stateJS"),
    },
    {
        S_("<a href=\"{{if .F}}/foo?a={{else}}/bar/{{end}}{{.H}}\">"),
        S_("z:1:47: {{.H}} appears in an ambiguous context within a URL"),
    },
    {
        S_("<a onclick=\"alert('Hello \\"),
        S_("unfinished escape sequence in JS string: \"Hello \\\\\""),
    },
    {
        S_("<a onclick='alert(\"Hello\\, World\\"),
        S_("unfinished escape sequence in JS string: \"Hello\\\\, World\\\\\""),
    },
    {
        S_("<a onclick='alert(/x+\\"),
        S_("unfinished escape sequence in JS string: \"x+\\\\\""),
    },
    {
        S_("<a onclick=\"/foo[\\]/"),
        S_("unfinished JS regexp charset: \"foo[\\\\]/\""),
    },
    {
        S_("<script>{{if false}}var x = 1{{end}}/-{{\"1.5\"}}/i.test(x)</script>"),
        S_("'/' could start a division or regexp: \"/-\""),
    },
    {
        S_("{{template \"foo\"}}"),
        S_("z:1:11: no such template \"foo\""),
    },
    {
        S_("<div{{template \"y\"}}>{{define \"y\"}} foo<b{{end}}"),
        S_("\"<\" in attribute name: \" foo<b\""),
    },
    {
        S_("<script>reverseList = [{{template \"t\"}}]</script>{{define \"t\"}}{{if "
           ".Tail}}{{template \"t\" .Tail}}{{end}}{{.Head}}\",{{end}}"),
        S_(": cannot compute output context for template t$htmltemplate_stateJS_elem"
           "entScript"),
    },
    {
        S_("<input type=button value=onclick=>"),
        S_("html/template:z: \"=\" in unquoted attr: \"onclick=\""),
    },
    {
        S_("<input type=button value= onclick=>"),
        S_("html/template:z: \"=\" in unquoted attr: \"onclick=\""),
    },
    {
        S_("<input type=button value= 1+1=2>"),
        S_("html/template:z: \"=\" in unquoted attr: \"1+1=2\""),
    },
    {
        S_("<a class=`foo>"),
        S_("html/template:z: \"`\" in unquoted attr: \"`foo\""),
    },
    {
        S_("<a style=font:'Arial'>"),
        S_("html/template:z: \"'\" in unquoted attr: \"font:'Arial'\""),
    },
    {
        S_("<a=foo>"),
        S_(": expected space, attr name, or end of tag, but got \"=foo>\""),
    },
    {
        S_("Hello, {{. | urlquery | print}}!"),
        S_("predefined escaper \"urlquery\" disallowed in template"),
    },
    {
        S_("Hello, {{. | html | print}}!"),
        S_("predefined escaper \"html\" disallowed in template"),
    },
    {
        S_("Hello, {{html . | print}}!"),
        S_("predefined escaper \"html\" disallowed in template"),
    },
    {
        S_("<div class={{. | html}}>Hello<div>"),
        S_("predefined escaper \"html\" disallowed in template"),
    },
    {
        S_("Hello, {{. | urlquery | html}}!"),
        S_("predefined escaper \"urlquery\" disallowed in template"),
    },
    {
        S_("<script>var a = `{{if .X}}`{{end}}"),
        S_("{{if}} branches end in different contexts"),
    },
    {
        S_("<script>var a = `{{if .X}}a{{else}}`{{end}}"),
        S_("{{if}} branches end in different contexts"),
    },
    {
        S_("<script>var a = `{{if .X}}a{{else}}b{{end}}`</script>"),
        S_(""),
    },
};

static const EscapeTextTest escape_text_tests[] = {
    {
        S_(""),
        {.state = ST(TEXT)},
    },
    {
        S_("Hello, World!"),
        {.state = ST(TEXT)},
    },
    {
        S_("I <3 Ponies!"),
        {.state = ST(TEXT)},
    },
    {
        S_("<a"),
        {.state = ST(TAG)},
    },
    {
        S_("<a "),
        {.state = ST(TAG)},
    },
    {
        S_("<a>"),
        {.state = ST(TEXT)},
    },
    {
        S_("<a href"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a on"),
        {.state = ST(ATTR_NAME), .attr = AT(SCRIPT)},
    },
    {
        S_("<a href "),
        {.state = ST(AFTER_NAME), .attr = AT(URL)},
    },
    {
        S_("<a style  =  "),
        {.state = ST(BEFORE_VALUE), .attr = AT(STYLE)},
    },
    {
        S_("<a href="),
        {.state = ST(BEFORE_VALUE), .attr = AT(URL)},
    },
    {
        S_("<a href=x"),
        {.state = ST(URL),
         .delim = DL(SPACE_OR_TAG_END),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href=x "),
        {.state = ST(TAG)},
    },
    {
        S_("<a href=>"),
        {.state = ST(TEXT)},
    },
    {
        S_("<a href=x>"),
        {.state = ST(TEXT)},
    },
    {
        S_("<a href ='"),
        {.state = ST(URL), .delim = DL(SINGLE_QUOTE), .attr = AT(URL)},
    },
    {
        S_("<a href=''"),
        {.state = ST(TAG)},
    },
    {
        S_("<a href= \""),
        {.state = ST(URL), .delim = DL(DOUBLE_QUOTE), .attr = AT(URL)},
    },
    {
        S_("<a href=\"\""),
        {.state = ST(TAG)},
    },
    {
        S_("<a title=\""),
        {.state = ST(ATTR), .delim = DL(DOUBLE_QUOTE)},
    },
    {
        S_("<a HREF='http:"),
        {.state = ST(URL),
         .delim = DL(SINGLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a Href='/"),
        {.state = ST(URL),
         .delim = DL(SINGLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href='\""),
        {.state = ST(URL),
         .delim = DL(SINGLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href=\"'"),
        {.state = ST(URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href='&apos;"),
        {.state = ST(URL),
         .delim = DL(SINGLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href=\"&quot;"),
        {.state = ST(URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href=\"&#34;"),
        {.state = ST(URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<a href=&quot;"),
        {.state = ST(URL),
         .delim = DL(SPACE_OR_TAG_END),
         .url_part = UP(PRE_QUERY),
         .attr = AT(URL)},
    },
    {
        S_("<img alt=\"1\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<img alt=\"1>\""),
        {.state = ST(TAG)},
    },
    {
        S_("<img alt=\"1>\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<input checked type=\"checkbox\""),
        {.state = ST(TAG)},
    },
    {
        S_("<a onclick=\""),
        {.state = ST(JS), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"//foo"),
        {.state = ST(JS_LINE_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick='//\n"),
        {.state = ST(JS), .delim = DL(SINGLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick='//\r\n"),
        {.state = ST(JS), .delim = DL(SINGLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick='//\xe2\x80\xa8"),
        {.state = ST(JS), .delim = DL(SINGLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/*"),
        {.state = ST(JS_BLOCK_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/*/"),
        {.state = ST(JS_BLOCK_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/**/"),
        {.state = ST(JS), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onkeypress=\"&quot;"),
        {.state = ST(JS_DQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick='&quot;foo&quot;"),
        {.state = ST(JS),
         .delim = DL(SINGLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=&#39;foo&#39;"),
        {.state = ST(JS),
         .delim = DL(SPACE_OR_TAG_END),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=&#39;foo"),
        {.state = ST(JS_SQ_STR), .delim = DL(SPACE_OR_TAG_END), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"&quot;foo'"),
        {.state = ST(JS_DQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"'foo&quot;"),
        {.state = ST(JS_SQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"`foo"),
        {.state = ST(JS_TMPL_LIT), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<A ONCLICK=\"'"),
        {.state = ST(JS_SQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/"),
        {.state = ST(JS_REGEXP), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"'foo'"),
        {.state = ST(JS),
         .delim = DL(DOUBLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"'foo\\'"),
        {.state = ST(JS_SQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"'foo\\'"),
        {.state = ST(JS_SQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/foo/"),
        {.state = ST(JS),
         .delim = DL(DOUBLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<script>/foo/ /="),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<a onclick=\"1 /foo"),
        {.state = ST(JS),
         .delim = DL(DOUBLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"1 /*c*/ /foo"),
        {.state = ST(JS),
         .delim = DL(DOUBLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/foo[/]"),
        {.state = ST(JS_REGEXP), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/foo\\/"),
        {.state = ST(JS_REGEXP), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<a onclick=\"/foo/"),
        {.state = ST(JS),
         .delim = DL(DOUBLE_QUOTE),
         .js_ctx = JC(DIV_OP),
         .attr = AT(SCRIPT)},
    },
    {
        S_("<input checked style=\""),
        {.state = ST(CSS), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"//"),
        {.state = ST(CSS_LINE_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"//</script>"),
        {.state = ST(CSS_LINE_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style='//\n"),
        {.state = ST(CSS), .delim = DL(SINGLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style='//\r"),
        {.state = ST(CSS), .delim = DL(SINGLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"/*"),
        {.state = ST(CSS_BLOCK_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"/*/"),
        {.state = ST(CSS_BLOCK_CMT), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"/**/"),
        {.state = ST(CSS), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: '"),
        {.state = ST(CSS_SQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: &quot;"),
        {.state = ST(CSS_DQ_STR), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: '/foo?img="),
        {.state = ST(CSS_SQ_STR),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(QUERY_OR_FRAG),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: '/"),
        {.state = ST(CSS_SQ_STR),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url(&#x22;/"),
        {.state = ST(CSS_DQ_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url('/"),
        {.state = ST(CSS_SQ_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url('/)"),
        {.state = ST(CSS_SQ_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url('/ "),
        {.state = ST(CSS_SQ_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url(/"),
        {.state = ST(CSS_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(PRE_QUERY),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url( "),
        {.state = ST(CSS_URL), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url( /image?name="),
        {.state = ST(CSS_URL),
         .delim = DL(DOUBLE_QUOTE),
         .url_part = UP(QUERY_OR_FRAG),
         .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url(x)"),
        {.state = ST(CSS), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url('x'"),
        {.state = ST(CSS), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<a style=\"background: url( x "),
        {.state = ST(CSS), .delim = DL(DOUBLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<!-- foo"),
        {.state = ST(HTML_CMT)},
    },
    {
        S_("<!-->"),
        {.state = ST(HTML_CMT)},
    },
    {
        S_("<!--->"),
        {.state = ST(HTML_CMT)},
    },
    {
        S_("<!-- foo -->"),
        {.state = ST(TEXT)},
    },
    {
        S_("<script"),
        {.state = ST(TAG), .element = EL(SCRIPT)},
    },
    {
        S_("<script "),
        {.state = ST(TAG), .element = EL(SCRIPT)},
    },
    {
        S_("<script src=\"foo.js\" "),
        {.state = ST(TAG), .element = EL(SCRIPT)},
    },
    {
        S_("<script src='foo.js' "),
        {.state = ST(TAG), .element = EL(SCRIPT)},
    },
    {
        S_("<script type=text/javascript "),
        {.state = ST(TAG), .element = EL(SCRIPT)},
    },
    {
        S_("<script>"),
        {.state = ST(JS), .js_ctx = JC(REGEXP), .element = EL(SCRIPT)},
    },
    {
        S_("<script>foo"),
        {.state = ST(JS), .js_ctx = JC(DIV_OP), .element = EL(SCRIPT)},
    },
    {
        S_("<script>foo</script>"),
        {.state = ST(TEXT)},
    },
    {
        S_("<script>foo</script><!--"),
        {.state = ST(HTML_CMT)},
    },
    {
        S_("<script>document.write(\"<p>foo</p>\");"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<p>foo<\\/script>\");"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<script>alert(1)</script>\");"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<script>"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<script>alert(1)</script>"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<script>alert(1)<!--"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<script>alert(1)</Script>\");"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>document.write(\"<!--\");"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>let a = /</script"),
        {.state = ST(JS_REGEXP), .element = EL(SCRIPT)},
    },
    {
        S_("<script>let a = /</script/"),
        {.state = ST(JS), .element = EL(SCRIPT), .js_ctx = JC(DIV_OP)},
    },
    {
        S_("<script type=\"text/template\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<script type=\"TEXT/JAVASCRIPT\">"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script TYPE=\"text/template\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<script type=\"notjs\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<Script>"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<SCRIPT>foo"),
        {.state = ST(JS), .js_ctx = JC(DIV_OP), .element = EL(SCRIPT)},
    },
    {
        S_("<textarea>value"),
        {.state = ST(RCDATA), .element = EL(TEXTAREA)},
    },
    {
        S_("<textarea>value</TEXTAREA>"),
        {.state = ST(TEXT)},
    },
    {
        S_("<textarea name=html><b"),
        {.state = ST(RCDATA), .element = EL(TEXTAREA)},
    },
    {
        S_("<title>value"),
        {.state = ST(RCDATA), .element = EL(TITLE)},
    },
    {
        S_("<style>value"),
        {.state = ST(CSS), .element = EL(STYLE)},
    },
    {
        S_("<a xlink:href"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a xmlns"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a xmlns:foo"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a xmlnsxyz"),
        {.state = ST(ATTR_NAME)},
    },
    {
        S_("<a data-url"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a data-iconUri"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a data-urlItem"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a g:"),
        {.state = ST(ATTR_NAME)},
    },
    {
        S_("<a g:url"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a g:iconUri"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a g:urlItem"),
        {.state = ST(ATTR_NAME), .attr = AT(URL)},
    },
    {
        S_("<a g:value"),
        {.state = ST(ATTR_NAME)},
    },
    {
        S_("<a svg:style='"),
        {.state = ST(CSS), .delim = DL(SINGLE_QUOTE), .attr = AT(STYLE)},
    },
    {
        S_("<svg:font-face"),
        {.state = ST(TAG)},
    },
    {
        S_("<svg:a svg:onclick=\""),
        {.state = ST(JS), .delim = DL(DOUBLE_QUOTE), .attr = AT(SCRIPT)},
    },
    {
        S_("<svg:a svg:onclick=\"x()\">"),
        {.state = ST(TEXT)},
    },
    {
        S_("<script>var a = `"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT)},
    },
    {
        S_("<script>var a = `${"),
        {.state = ST(JS), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${}"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT)},
    },
    {
        S_("<script>var a = `${`"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${var a = \""),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${var a = \"`"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${var a = \"}"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${``"),
        {.state = ST(JS), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${`}"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>`${ {} } asd`</script><script>`${ {} }"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT)},
    },
    {
        S_("<script>var foo = `${ (_ => { return \"x\" })() + \"${"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var a = `${ {</script><script>var b = `${ x }"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT), .js_ctx = JC(DIV_OP)},
    },
    {
        S_("<script>var foo = `x` + \"${"),
        {.state = ST(JS_DQ_STR), .element = EL(SCRIPT)},
    },
    {
        S_("<script>function f() { var a = `${}`; }"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>{`${}`}"),
        {.state = ST(JS), .element = EL(SCRIPT)},
    },
    {
        S_("<script>`${ function f() { return `${1}` }() }`"),
        {.state = ST(JS), .element = EL(SCRIPT), .js_ctx = JC(DIV_OP)},
    },
    {
        S_("<script>function f() {`${ function f() { `${1}` } }`}"),
        {.state = ST(JS), .element = EL(SCRIPT), .js_ctx = JC(REGEXP)},
    },
    {
        S_("<script>`${ { `` }"),
        {.state = ST(JS), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>`${ { }`"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>var foo = `${ foo({ a: { c: `${"),
        {.state = ST(JS), .element = EL(SCRIPT), DEPTH(depth_2_0)},
    },
    {
        S_("<script>var foo = `${ foo({ a: { c: `${ {{.}} }` }, b: "),
        {.state = ST(JS), .element = EL(SCRIPT), DEPTH(depth_1)},
    },
    {
        S_("<script>`${ `}"),
        {.state = ST(JS_TMPL_LIT), .element = EL(SCRIPT), DEPTH(depth_0)},
    },
    {
        S_("<script>`${1}${"),
        {.state = ST(JS), .element = EL(SCRIPT), .js_ctx = JC(REGEXP), DEPTH(depth_0)},
    },
    {
        S_("<script>`${`${1}${"),
        {.state = ST(JS),
         .element = EL(SCRIPT),
         .js_ctx = JC(REGEXP),
         DEPTH(depth_0_0)},
    },
};

static const PipelineTest pipeline_tests[] = {
    {
        S_("{{.X}}"),
        S_(".X"),
        0,
        {S_("")},
    },
    {
        S_("{{.X | html}}"),
        S_(".X | html"),
        0,
        {S_("")},
    },
    {
        S_("{{.X}}"),
        S_(".X | html"),
        1,
        {S_("html")},
    },
    {
        S_("{{html .X}}"),
        S_("_eval_args_ .X | html | urlquery"),
        2,
        {S_("html"), S_("urlquery")},
    },
    {
        S_("{{html .X .Y .Z}}"),
        S_("_eval_args_ .X .Y .Z | html | urlquery"),
        2,
        {S_("html"), S_("urlquery")},
    },
    {
        S_("{{.X | print}}"),
        S_(".X | print | urlquery"),
        1,
        {S_("urlquery")},
    },
    {
        S_("{{.X | print | urlquery}}"),
        S_(".X | print | urlquery"),
        1,
        {S_("urlquery")},
    },
    {
        S_("{{.X | urlquery}}"),
        S_(".X | html | urlquery"),
        2,
        {S_("html"), S_("urlquery")},
    },
    {
        S_("{{.X | print 2 | .f 3}}"),
        S_(".X | print 2 | .f 3 | urlquery | html"),
        2,
        {S_("urlquery"), S_("html")},
    },
    {
        S_("{{.X | println.x }}"),
        S_(".X | println.x | urlquery | html"),
        2,
        {S_("urlquery"), S_("html")},
    },
    {
        S_("{{.X | (print 12 | println).x }}"),
        S_(".X | (print 12 | println).x | urlquery | html"),
        2,
        {S_("urlquery"), S_("html")},
    },
    {
        S_("{{.X | urlquery}}"),
        S_(".X | _html_template_urlfilter | urlquery"),
        2,
        {S_("_html_template_urlfilter"), S_("_html_template_urlnormalizer")},
    },
    {
        S_("{{.X | urlquery}}"),
        S_(".X | urlquery | _html_template_urlfilter | _html_template_cssescaper"),
        2,
        {S_("_html_template_urlfilter"), S_("_html_template_cssescaper")},
    },
    {
        S_("{{.X | urlquery}}"),
        S_(".X | urlquery"),
        1,
        {S_("_html_template_urlnormalizer")},
    },
    {
        S_("{{.X | urlquery}}"),
        S_(".X | urlquery"),
        1,
        {S_("_html_template_urlescaper")},
    },
    {
        S_("{{.X | html}}"),
        S_(".X | html"),
        1,
        {S_("_html_template_htmlescaper")},
    },
};

static const CveTest cve_tests[] = {
    {
        S_("regexp after open brace in if block"),
        S_("<script>if(true){/{{.}}/g.test(\"x\")}</script>"),
        S_("a.b"),
        S_("<script>if(true){/a\\.b/g.test(\"x\")}</script>"),
    },
    {
        S_("regexp after close brace"),
        S_("<script>if(true){x=1}/{{.}}/g.test(\"x\")</script>"),
        S_("a.b"),
        S_("<script>if(true){x=1}/a\\.b/g.test(\"x\")</script>"),
    },
    {
        S_("regexp pathological attacker input"),
        S_("<script>if(true){/{{.}}/g.test(\"x\")}</script>"),
        S_("./;alert(1);var q=/."),
        S_("<script>if(true){/\\.\\/;alert\\(1\\);var q=\\/\\./g.test(\"x\")}</scrip"
           "t>"),
    },
    {
        S_("regexp after open brace in template literal"),
        S_("<script>`${ (function(){/{{.}}/g.test(x)}) }`</script>"),
        S_("a.b"),
        S_("<script>`${ (function(){/a\\.b/g.test(x)}) }`</script>"),
    },
};

typedef struct EscapeCase {
    const EscapeTest *test;
    Any data;
    Any pdata;
} EscapeCase;

static void escape_case(void *env, TestingT *t) {
    const EscapeCase *tc = env;
    const EscapeTest *test = tc->test;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    HtmlTemplate *tmpl = must_parse(t, a, test->name, test->input);
    if (tmpl == NULL)
        goto done;
    /* Check for bug 6459: Tree field was not set in Parse. */
    if (tmpl->tree != tmpl->text->tree) {
        testing_t_errorf_v(t, "%s: tree not set properly", test->name);
        goto done;
    }
    Error err = html_template_execute(tmpl, strings_builder_as_io_writer(&b), tc->data);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: template execution failed: %s", test->name, err);
        goto done;
    }
    Str got = strings_builder_string(&b);
    if (!str_eq(test->output, got)) {
        testing_t_errorf_v(t, "%s: escaped output: want\n\t%q\ngot\n\t%q", test->name,
                           test->output, got);
        goto done;
    }
    strings_builder_reset(&b);
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&b), tc->pdata);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: template execution failed for pointer: %s",
                           test->name, err);
        goto done;
    }
    got = strings_builder_string(&b);
    if (!str_eq(test->output, got)) {
        testing_t_errorf_v(t, "%s: escaped output for pointer: want\n\t%q\ngot\n\t%q",
                           test->name, test->output, got);
        goto done;
    }
    if (tmpl->tree != tmpl->text->tree)
        testing_t_errorf_v(t, "%s: tree mismatch", test->name);
done:
    arena_free(&ar);
}

static void TestEscape(TestingT *t) {
    static Str a_elems[] = {S_("<a>"), S_("<b>")};
    static Str no_strs[1];
    BadMarshaler bad = {0};
    GoodMarshaler good = {0};
    BadMarshalerPtr bad_p = &bad;
    GoodMarshalerPtr good_p = &good;
    EscData data = {
        .F = false,
        .T = true,
        .C = BURROW_S("<Cincinnati>"),
        .G = BURROW_S("<Goodbye>"),
        .H = BURROW_S("<Hello>"),
        .A = slice_from(a_elems, 2, 2, TYPE_STRING),
        .E = slice_from(no_strs, 0, 0, TYPE_STRING),
        .N = 42,
        .B = BURROW_ANY(TYPE_OF(BadMarshalerPtr), &bad_p),
        .M = BURROW_ANY(TYPE_OF(GoodMarshalerPtr), &good_p),
        .U = {NULL, NULL},
        .Z = NULL,
        .W = BURROW_S(
            "&iexcl;<b class=\"foo\">Hello</b>, <textarea>O'World</textarea>!"),
        .I = BURROW_S("${ asd `` }"),
    };
    EscDataPtr pdata = &data;
    for (Int i = 0; i < NELEM(escape_tests); i++) {
        EscapeCase tc = {&escape_tests[i], BURROW_ANY(TYPE_OF(EscData), &data),
                         BURROW_ANY(TYPE_OF(EscDataPtr), &pdata)};
        testing_t_run(t, escape_tests[i].name,
                      BURROW_FN(TestingTFunc, escape_case, &tc));
    }
}

/* -------------------------------------------------------- TestEscapeMap */

static void TestEscapeMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StrStrMap data = map_make(a, TYPE_STRING, TYPE_STRING, 2);
    Str k1 = BURROW_S("html"), v1 = BURROW_S("<h1>Hi!</h1>");
    Str k2 = BURROW_S("urlquery"),
        v2 = BURROW_S("http://www.foo.com/index.html?title=main");
    map_set(data, &k1, &v1);
    map_set(data, &k2, &v2);
    static const EscapeTest tests[] = {
        /* covering issue 20323 */
        {S_("field with predefined escaper name 1"), S_("{{.html | print}}"),
         S_("&lt;h1&gt;Hi!&lt;/h1&gt;")},
        /* covering issue 20323 */
        {S_("field with predefined escaper name 2"), S_("{{.urlquery | print}}"),
         S_("http://www.foo.com/index.html?title=main")},
    };
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < NELEM(tests); i++) {
        const EscapeTest *test = &tests[i];
        HtmlTemplate *tmpl = must_parse(t, a, BURROW_STR_EMPTY, test->input);
        if (tmpl == NULL)
            continue;
        strings_builder_reset(&b);
        Error err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                          BURROW_ANY(TYPE_OF(StrStrMap), &data));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: template execution failed: %s", test->name, err);
            continue;
        }
        Str got = strings_builder_string(&b);
        if (!str_eq(test->output, got)) {
            testing_t_errorf_v(t, "%s: escaped output: want\n\t%q\ngot\n\t%q",
                               test->name, test->output, got);
            continue;
        }
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- TestEscapeSet */

/* pred is a template function that returns the predecessor of a natural number
 * for testing recursive templates. */
static Any pred(void *env, Slice a, Error *err) {
    (void)env;
    if (a.len == 1) {
        Any x = BURROW_AT(Any, a, 0);
        if (x.t == TYPE_INT && *(const Int *)x.data > 0) {
            Int *r = BURROW_NEW(esc_a, Int);
            if (r == NULL) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return (Any){NULL, NULL};
            }
            *r = *(const Int *)x.data - 1;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return BURROW_ANY(TYPE_INT, r);
        }
    }
    BURROW_OUT(err, fmt_errorf_v("undefined pred(%v)", BURROW_ANY(TYPE_OF(Anys), &a)));
    return (Any){NULL, NULL};
}

static void TestEscapeSet(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    esc_a = a;
    DataItem foo = {slice_nil(TYPE_OF(DataItemPtr)), BURROW_S("foo")};
    DataItem bar = {slice_nil(TYPE_OF(DataItemPtr)), BURROW_S("<bar>")};
    DataItem baz = {slice_nil(TYPE_OF(DataItemPtr)), BURROW_S("baz")};
    DataItemPtr inner[] = {&baz};
    DataItem nested = {slice_from(inner, 1, 1, TYPE_OF(DataItemPtr)), BURROW_STR_EMPTY};
    DataItemPtr children[] = {&foo, &bar, &nested};
    DataItem data = {slice_from(children, 3, 3, TYPE_OF(DataItemPtr)),
                     BURROW_STR_EMPTY};

    PredFn pred_fn = {pred, NULL};
    Map *fns = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
    Str pred_name = BURROW_S("pred");
    Any pred_v = BURROW_ANY(TYPE_OF(PredFn), &pred_fn);
    map_set(fns, &pred_name, &pred_v);

    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < NELEM(escape_set_tests); i++) {
        const EscapeSetTest *test = &escape_set_tests[i];
        Str source = BURROW_STR_EMPTY;
        for (Int j = 0; j < test->n; j++)
            source = fmt_sprintf_v(a, "%s{{define %q}}%s{{end}} ", source,
                                   test->names[j], test->bodies[j]);
        Error err = BURROW_NO_ERROR;
        HtmlTemplate *tmpl = html_template_parse(
            html_template_funcs(html_template_new(a, BURROW_S("root")), fns), source,
            &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "error parsing %q: %v", source, err);
            continue;
        }
        strings_builder_reset(&b);
        err = html_template_execute_template(tmpl, strings_builder_as_io_writer(&b),
                                             BURROW_S("main"),
                                             BURROW_ANY(TYPE_OF(DataItem), &data));
        if (BURROW_FAILED(err)) {
            HtmlTemplate *main = html_template_lookup(tmpl, BURROW_S("main"));
            testing_t_errorf_v(t, "%q executing %s", error_text(err),
                               parse_node_string((ParseNode)main->tree->root, a));
            continue;
        }
        Str got = strings_builder_string(&b);
        if (!str_eq(test->want, got))
            testing_t_errorf_v(t, "want\n\t%q\ngot\n\t%q", test->want, got);
    }
    esc_a = NULL;
    arena_free(&ar);
}

/* ----------------------------------------------------------- TestErrors */

static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder buf = STRINGS_BUILDER(a);
    for (Int i = 0; i < NELEM(error_tests); i++) {
        const ErrorTest *test = &error_tests[i];
        Error err = BURROW_NO_ERROR;
        HtmlTemplate *tmpl =
            html_template_parse(html_template_new(a, BURROW_S("z")), test->input, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "input=%q: unexpected parse error %s\n", test->input,
                               err);
            continue;
        }
        err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    (Any){NULL, NULL});
        Str got = BURROW_STR_EMPTY;
        if (BURROW_FAILED(err))
            got = error_text(err);
        if (test->err.len == 0) {
            if (got.len != 0)
                testing_t_errorf_v(t, "input=%q: unexpected error %q", test->input,
                                   got);
            continue;
        }
        if (!strings_contains(got, test->err)) {
            testing_t_errorf_v(t,
                               "input=%q: error\n\t%q\ndoes not contain expected "
                               "string\n\t%q",
                               test->input, got, test->err);
            continue;
        }
        /* Check that we get the same error if we call Execute again. */
        err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    (Any){NULL, NULL});
        if (BURROW_OK(err) || !str_eq(error_text(err), got))
            testing_t_errorf_v(t, "input=%q: unexpected error on second call %q",
                               test->input, err);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------- TestEscapeText */

static void TestEscapeText(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(escape_text_tests); i++) {
        const EscapeTextTest *test = &escape_text_tests[i];
        Bytes b = slice_from_str(a, test->input);
        ParseTextNode n = {PARSE_NODE_TEXT, 0, NULL, b};
        burrow__HtContext c = burrow__ht_escape_text_node(a, burrow__ht_context(), &n);
        if (!burrow__ht_context_eq(test->output, c)) {
            testing_t_errorf_v(t, "input %q: want context\n\t%s\ngot\n\t%s",
                               test->input, burrow__ht_context_string(a, test->output),
                               burrow__ht_context_string(a, c));
            continue;
        }
        Str got = str_from_bytes(b.p, b.len);
        if (!str_eq(test->input, got)) {
            testing_t_errorf_v(t, "input %q: text node was modified: want %q got %q",
                               test->input, test->input, got);
            continue;
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------- TestEnsurePipelineContains */

/* Go's %v of a []string. */
static Str strs_string(Alloc *a, const Str *s, Int n) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, BURROW_S("["), NULL);
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            strings_builder_write_string(&b, BURROW_S(" "), NULL);
        strings_builder_write_string(&b, s[i], NULL);
    }
    strings_builder_write_string(&b, BURROW_S("]"), NULL);
    return strings_builder_string(&b);
}

static void TestEnsurePipelineContains(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(pipeline_tests); i++) {
        const PipelineTest *test = &pipeline_tests[i];
        Error err = BURROW_NO_ERROR;
        /* Not in Go: Go wraps the Parse call in Must. In this file the parse is a
         * statement of its own, here and below, since C leaves the order of a
         * call's arguments unspecified, and err could be read before the parse
         * sets it. */
        TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("test")),
                                                 test->input, &err);
        tmpl = text_template_must(tmpl, err);
        ParseNode first = BURROW_AT(ParseNode, tmpl->tree->root->nodes, 0);
        if (*first != PARSE_NODE_ACTION) {
            testing_t_errorf_v(t, "First node is not an action: %s", test->input);
            continue;
        }
        ParsePipeNode *pipe = ((ParseActionNode *)first)->pipe;
        Str ids[2] = {test->ids[0], test->ids[1]};
        burrow__ht_ensure_pipeline_contains(
            a, pipe, slice_from(ids, test->nids, 2, TYPE_STRING));
        Str got = parse_node_string((ParseNode)pipe, a);
        if (!str_eq(got, test->output))
            testing_t_errorf_v(t, "#%d: %s, %s: want\n\t%s\ngot\n\t%s", i, test->input,
                               strs_string(a, test->ids, test->nids), test->output,
                               got);
    }
    arena_free(&ar);
}

/* ------------------------------------------------ malformed and errors */

static void TestEscapeMalformedPipelines(TestingT *t) {
    static const Str tests[] = {
        S_("{{ 0 | $ }}"),
        S_("{{ 0 | $ | urlquery }}"),
        S_("{{ 0 | (nil) }}"),
        S_("{{ 0 | (nil) | html }}"),
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        Error err = BURROW_NO_ERROR;
        HtmlTemplate *tmpl =
            html_template_parse(html_template_new(a, BURROW_S("test")), tests[i], &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed to parse set: %q", err);
            continue;
        }
        err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                    (Any){NULL, NULL});
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Expected error for %q", tests[i]);
    }
    arena_free(&ar);
}

static void TestEscapeErrorsNotIgnorable(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(
        html_template_new(a, BURROW_S("dangerous")), BURROW_S("<a"), &err);
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                (Any){NULL, NULL});
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Expected error");
    else if (strings_builder_len(&b) != 0)
        testing_t_errorf_v(t, "Emitted output despite escaping failure");
    arena_free(&ar);
}

static void TestEscapeSetErrorsNotIgnorable(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl =
        html_template_parse(html_template_new(a, BURROW_S("root")),
                            BURROW_S("{{define \"t\"}}<a{{end}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to parse set: %q", err);
        arena_free(&ar);
        return;
    }
    err = html_template_execute_template(tmpl, strings_builder_as_io_writer(&b),
                                         BURROW_S("t"), (Any){NULL, NULL});
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Expected error");
    else if (strings_builder_len(&b) != 0)
        testing_t_errorf_v(t, "Emitted output despite escaping failure");
    arena_free(&ar);
}

/* ---------------------------------------------------- TestRedundantFuncs */

static void TestRedundantFuncs(TestingT *t) {
    static Str s0 = S_("\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f"
                       "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d"
                       "\x1e\x1f !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNO"
                       "PQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\x7f"
                       "\xc2\xa0\xc4\x80\xe2\x80\xa8\xe2\x80\xa9\xef\xbb\xbf\xef"
                       "\xb7\xac\xef\xbf\xbd\xef\xbf\xbf\xf0\x9d\x84\x9e&amp;%22\\");
    static HtmlTemplateCSS css = S_("a[href =~ \"//example.com\"]#foo");
    static HtmlTemplateHTML html = S_("Hello, <b>World</b> &amp;tc!");
    static HtmlTemplateHTMLAttr attr = S_(" dir=\"ltr\"");
    static HtmlTemplateJS js = S_("c && alert(\"Hello, World!\");");
    static HtmlTemplateJSStr js_str = S_("Hello, World & O'Reilly\\x21");
    static HtmlTemplateURL url = S_("greeting=H%69&addressee=(World)");
    Any inputs[] = {
        BURROW_ANY(TYPE_STRING, &s0),
        BURROW_ANY(TYPE_OF(HtmlTemplateCSS), &css),
        BURROW_ANY(TYPE_OF(HtmlTemplateHTML), &html),
        BURROW_ANY(TYPE_OF(HtmlTemplateHTMLAttr), &attr),
        BURROW_ANY(TYPE_OF(HtmlTemplateJS), &js),
        BURROW_ANY(TYPE_OF(HtmlTemplateJSStr), &js_str),
        BURROW_ANY(TYPE_OF(HtmlTemplateURL), &url),
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str n0, n1;
    for (Int i = 0; burrow__ht_redundant_pair(i, &n0, &n1); i++) {
        for (Int j = 0; j < NELEM(inputs); j++) {
            Str want =
                burrow__ht_call_func(a, n0, slice_from(&inputs[j], 1, 1, TYPE_ANY));
            Any w = BURROW_ANY(TYPE_STRING, &want);
            Str got = burrow__ht_call_func(a, n1, slice_from(&w, 1, 1, TYPE_ANY));
            if (!str_eq(want, got))
                testing_t_errorf_v(t, "%s %s with %T %q: want\n\t%q,\ngot\n\t%q", n0,
                                   n1, inputs[j], inputs[j], want, got);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* --------------------------------------------------- the smaller tests */

static void TestIndirectPrint(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int ia = 3;
    IntPtr ap = &ia;
    Str sb = BURROW_S("hello");
    StrPtr bp = &sb;
    StrPtrPtr bpp = &bp;
    HtmlTemplate *tmpl = must_parse(t, a, BURROW_S("t"), BURROW_S("{{.}}"));
    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                      BURROW_ANY(TYPE_OF(IntPtr), &ap));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unexpected error: %s", err);
    else if (!str_eq(strings_builder_string(&buf), BURROW_S("3")))
        testing_t_errorf_v(t, "Expected \"3\"; got %q", strings_builder_string(&buf));
    strings_builder_reset(&buf);
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                BURROW_ANY(TYPE_OF(StrPtrPtr), &bpp));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unexpected error: %s", err);
    else if (!str_eq(strings_builder_string(&buf), BURROW_S("hello")))
        testing_t_errorf_v(t, "Expected \"hello\"; got %q",
                           strings_builder_string(&buf));
    arena_free(&ar);
}

/* This is a test for issue 3272. */
static void TestEmptyTemplateHTML(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str files[] = {BURROW_S(OS_DEV_NULL)};
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *page =
        html_template_parse_files(html_template_new(a, BURROW_S("page")),
                                  slice_from(files, 1, 1, TYPE_STRING), &err);
    html_template_must(page, err);
    Str nothing = BURROW_S("nothing");
    err = html_template_execute_template(page, io_discard, BURROW_S("page"),
                                         BURROW_ANY(TYPE_STRING, &nothing));
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error");
    arena_free(&ar);
}

/* This is a test for issue 7379: type assertion error caused panic, and then
 * the code to handle the panic breaks escaping. It's hard to see the second
 * problem once the first is fixed, but its fix is trivial so we let that go.
 * See the discussion for issue 7379. */
static void TestPipeToMethodIsEscaped(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    esc_a = a;
    HtmlTemplate *tmpl =
        must_parse(t, a, BURROW_S("x"), BURROW_S("<html>{{0 | .SomeMethod}}</html>\n"));
    Issue7379 v = 0;
    for (int i = 0; tmpl != NULL && i < 3; i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        BURROW_TRY {
            (void)html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                        BURROW_ANY(&burrow_type_Issue7379, &v));
        }
        BURROW_CATCH(r) {
            testing_t_errorf_v(t, "panicked: %v\n", r);
        }
        BURROW_TRY_END;
        Str expect = BURROW_S("<html>&lt;0&gt;</html>\n");
        Str str = strings_builder_string(&b);
        if (!str_eq(str, expect))
            testing_t_errorf_v(t, "expected %q got %q", expect, str);
    }
    esc_a = NULL;
    arena_free(&ar);
}

/* Unlike text/template, html/template crashed if given an incomplete template,
 * that is, a template that had been named but not given any content. This is
 * issue #10204. */
static void TestErrorOnUndefined(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HtmlTemplate *tmpl = html_template_new(a, BURROW_S("undefined"));
    Error err = html_template_execute(tmpl, io_discard, (Any){NULL, NULL});
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error");
    else if (!strings_contains(error_text(err), BURROW_S("incomplete")))
        testing_t_errorf_v(t, "expected error about incomplete template; got %s", err);
    arena_free(&ar);
}

/* This covers issue #20842. */
static void TestIdempotentExecute(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(
        html_template_new(a, BURROW_STR_EMPTY),
        BURROW_S("{{define \"main\"}}<body>{{template \"hello\"}}</body>{{end}}"),
        &err);
    tmpl = html_template_must(tmpl, err);
    html_template_parse(
        tmpl, BURROW_S("{{define \"hello\"}}Hello, {{\"Ladies & Gentlemen!\"}}{{end}}"),
        &err);
    html_template_must(tmpl, err);
    StringsBuilder got = STRINGS_BUILDER(a);
    /* Ensure that "hello" produces the same output when executed twice. */
    Str want = BURROW_S("Hello, Ladies &amp; Gentlemen!");
    for (int i = 0; i < 2; i++) {
        err = html_template_execute_template(tmpl, strings_builder_as_io_writer(&got),
                                             BURROW_S("hello"), (Any){NULL, NULL});
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "unexpected error: %s", err);
        if (!str_eq(strings_builder_string(&got), want))
            testing_t_errorf_v(
                t, "after executing template \"hello\", got:\n\t%q\nwant:\n\t%q\n",
                strings_builder_string(&got), want);
        strings_builder_reset(&got);
    }
    /* Ensure that the implicit re-execution of "hello" during the execution of
     * "main" does not cause the output of "hello" to change. */
    err = html_template_execute_template(tmpl, strings_builder_as_io_writer(&got),
                                         BURROW_S("main"), (Any){NULL, NULL});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %s", err);
    /* If the HTML escaper is added again to the action {{"Ladies & Gentlemen!"}},
     * we would expected to see the ampersand overescaped to "&amp;amp;". */
    want = BURROW_S("<body>Hello, Ladies &amp; Gentlemen!</body>");
    if (!str_eq(strings_builder_string(&got), want))
        testing_t_errorf_v(
            t, "after executing template \"main\", got:\n\t%q\nwant:\n\t%q\n",
            strings_builder_string(&got), want);
    arena_free(&ar);
}

static void BenchmarkEscapedExecute(TestingB *b) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl =
        html_template_parse(html_template_new(a, BURROW_S("t")),
                            BURROW_S("<a onclick=\"alert('{{.}}')\">{{.}}</a>"), &err);
    tmpl = html_template_must(tmpl, err);
    Str data = BURROW_S("foo & 'bar' & baz");
    Arena out;
    arena_init(&out, heap_allocator(), 0);
    Alloc *oa = arena_allocator(&out);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        StringsBuilder buf = STRINGS_BUILDER(oa);
        (void)html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_STRING, &data));
        if (i % 1024 == 1023)
            arena_reset(&out);
    }
    arena_free(&out);
    arena_free(&ar);
}

/* Covers issue 22780. */
static void TestOrphanedTemplate(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t1 =
        html_template_parse(html_template_new(a, BURROW_S("foo")),
                            BURROW_S("<a href=\"{{.}}\">link1</a>"), &err);
    t1 = html_template_must(t1, err);
    HtmlTemplate *t2 = html_template_parse(
        html_template_new_associated(t1, BURROW_S("foo")), BURROW_S("bar"), &err);
    t2 = html_template_must(t2, err);
    StringsBuilder b = STRINGS_BUILDER(a);
    Str want_error = BURROW_S("template: \"foo\" is an incomplete or empty template");
    Str js = BURROW_S("javascript:alert(1)");
    err = html_template_execute(t1, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_STRING, &js));
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "expected error executing t1");
        goto done;
    }
    if (!str_eq(error_text(err), want_error)) {
        testing_t_errorf_v(t, "got t1 execution error:\n\t%s\nwant:\n\t%s", err,
                           want_error);
        goto done;
    }
    strings_builder_reset(&b);
    err =
        html_template_execute(t2, strings_builder_as_io_writer(&b), (Any){NULL, NULL});
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error executing t2: %s", err);
        goto done;
    }
    Str want = BURROW_S("bar");
    if (!str_eq(strings_builder_string(&b), want))
        testing_t_errorf_v(t, "t2 rendered %q, want %q", strings_builder_string(&b),
                           want);
done:
    arena_free(&ar);
}

/* Covers issue 21844. */
static void TestAliasedParseTreeDoesNotOverescape(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str tmpl_text = BURROW_S("{{.}}");
    Str data = BURROW_S("<baz>");
    Str want = BURROW_S("&lt;baz&gt;");
    /* Templates "foo" and "bar" both alias the same underlying parse tree. */
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tpl =
        html_template_parse(html_template_new(a, BURROW_S("foo")), tmpl_text, &err);
    tpl = html_template_must(tpl, err);
    html_template_add_parse_tree(tpl, BURROW_S("bar"), tpl->tree, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "AddParseTree error: %v", err);
        goto done;
    }
    StringsBuilder b1 = STRINGS_BUILDER(a);
    StringsBuilder b2 = STRINGS_BUILDER(a);
    err =
        html_template_execute_template(tpl, strings_builder_as_io_writer(&b1),
                                       BURROW_S("foo"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ExecuteTemplate failed for \"foo\": %v", err);
        goto done;
    }
    err =
        html_template_execute_template(tpl, strings_builder_as_io_writer(&b2),
                                       BURROW_S("bar"), BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ExecuteTemplate failed for \"foo\": %v", err);
        goto done;
    }
    Str got1 = strings_builder_string(&b1);
    Str got2 = strings_builder_string(&b2);
    if (!str_eq(got1, want)) {
        testing_t_errorf_v(t, "Template \"foo\" rendered %q, want %q", got1, want);
        goto done;
    }
    if (!str_eq(got1, got2))
        testing_t_errorf_v(
            t,
            "Template \"foo\" and \"bar\" rendered %q and %q respectively, "
            "expected equal values",
            got1, got2);
done:
    arena_free(&ar);
}

static void TestMetaContentEscapeGODEBUG(TestingT *t) {
    burrow__ht_godebug_set("htmlmetacontenturlescape=0");
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HtmlTemplate *tmpl =
        must_parse(t, a, BURROW_STR_EMPTY,
                   BURROW_S("<meta http-equiv=\"refresh\" content=\"asd; "
                            "url={{\"javascript:alert(1)\"}}; "
                            "asd; url={{\"vbscript:alert(1)\"}}; asd\">"));
    StringsBuilder b = STRINGS_BUILDER(a);
    if (tmpl == NULL)
        goto done;
    Error err = html_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                      (Any){NULL, NULL});
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected error: %s", err);
        goto done;
    }
    Str want =
        BURROW_S("<meta http-equiv=\"refresh\" content=\"asd; url=javascript:alert(1); "
                 "asd; url=vbscript:alert(1); asd\">");
    Str got = strings_builder_string(&b);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
done:
    arena_free(&ar);
    burrow__ht_godebug_set(NULL);
}

static void TestIssue81821(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl = html_template_parse(
        html_template_new(a, BURROW_S("test")),
        BURROW_S("<script>const s = `${1}${/{{.}}/g}`</script>"), &err);
    tmpl = html_template_must(tmpl, err);
    Str input = BURROW_S("x/.exec(alert(1))}");
    err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                BURROW_ANY(TYPE_STRING, &input));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Execute: %v", err);
        goto done;
    }
    Str want = BURROW_S(
        "<script>const s = `${1}${/x\\/\\.exec\\(alert\\(1\\)\\)\\}/g}`</script>");
    Str got = strings_builder_string(&buf);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got:  %s\nwant: %s", got, want);
done:
    arena_free(&ar);
}

static void cve_case(void *env, TestingT *t) {
    const CveTest *tt = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder buf = STRINGS_BUILDER(a);
    HtmlTemplate *tmpl = must_parse(t, a, BURROW_S("test"), tt->tmpl);
    if (tmpl == NULL)
        goto done;
    Error err =
        html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                              BURROW_ANY(TYPE_STRING, (void *)(uintptr_t)&tt->input));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Execute: %v", err);
        goto done;
    }
    Str got = strings_builder_string(&buf);
    if (!str_eq(got, tt->want))
        testing_t_errorf_v(t, "got:  %s\nwant: %s", got, tt->want);
done:
    arena_free(&ar);
}

static void TestCVE202656858(TestingT *t) {
    for (Int i = 0; i < NELEM(cve_tests); i++)
        testing_t_run(
            t, cve_tests[i].name,
            BURROW_FN(TestingTFunc, cve_case, (void *)(uintptr_t)&cve_tests[i]));
}

static const CveTest issue81823_tests[] = {
    {
        S_("yield"),
        S_("<script>function* rules(){yield /{{.}}/g}</script>"),
        S_("/;alert(1)//"),
        S_("<script>function* rules(){yield /\\/;alert\\(1\\)\\/\\//g}</script>"),
    },
    {
        S_("property yield"),
        S_("<script>const pct = bond.yield / 100;\nconst user = {{.}}; // "
           "state\n</script>"),
        S_("1;pwned=1;0"),
        S_("<script>const pct = bond.yield / 100;\nconst user = \"1;pwned=1;0\"; "
           "\n</script>"),
    },
    {
        S_("private yield"),
        S_("<script>class Bond { #yield = 4; pct() { return this.#yield / 100; } "
           "}\nconst "
           "user = {{.}}; // state\n</script>"),
        S_("1;pwned=1;0"),
        S_("<script>class Bond { #yield = 4; pct() { return this.#yield / 100; } "
           "}\nconst "
           "user = \"1;pwned=1;0\"; \n</script>"),
    },
    {
        S_("property in"),
        S_("<script>const kb = traffic.in / 1024;\nconst user = {{.}}; // "
           "state\n</script>"),
        S_("1;pwned=1;0"),
        S_("<script>const kb = traffic.in / 1024;\nconst user = \"1;pwned=1;0\"; "
           "\n</script>"),
    },
};

/* Not in Go: the cases are CveTests, which have the same fields, and run
 * through TestCVE202656858's cve_case, which is the same as Go's loop body. */
static void TestIssue81823(TestingT *t) {
    for (Int i = 0; i < NELEM(issue81823_tests); i++)
        testing_t_run(
            t, issue81823_tests[i].name,
            BURROW_FN(TestingTFunc, cve_case, (void *)(uintptr_t)&issue81823_tests[i]));
}

/* ------------------------------------------ BenchmarkTemplateSpecialTags */

#define NAME_GIFT_FIELDS(F, T) F(T, Str, Name, "") F(T, Str, Gift, "")
BURROW_STRUCT(NameGift, NAME_GIFT_FIELDS);

static void BenchmarkTemplateSpecialTags(TestingB *b) {
    NameGift r = {BURROW_S("Aunt Mildred"), BURROW_S("bone china tea set")};
    Str h1 = BURROW_S("<textarea> Hello Hello Hello </textarea> ");
    Str h2 = BURROW_S("<textarea> <p> Dear {{.Name}},\n{{with .Gift}}Thank you for the "
                      "lovely {{.}}. {{end}}\nBest wishes. </p>\n</textarea>");
    Arena keep;
    arena_init(&keep, heap_allocator(), 0);
    Alloc *ka = arena_allocator(&keep);
    Str rep = strings_repeat(ka, h1, 100);
    Str html = fmt_sprintf_v(ka, "%s%s%s%s", rep, h2, rep, h2);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        HtmlTemplate *tmpl =
            html_template_parse(html_template_new(a, BURROW_S("foo")), html, &err);
        tmpl = html_template_must(tmpl, err);
        StringsBuilder buf = STRINGS_BUILDER(a);
        err = html_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_OF(NameGift), &r));
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            arena_free(&keep);
            testing_b_fatal_v(b, err);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keep);
}

#define TESTS(X)                                                                       \
    X(TestEscape)                                                                      \
    X(TestEscapeMap)                                                                   \
    X(TestEscapeSet)                                                                   \
    X(TestErrors)                                                                      \
    X(TestEscapeText)                                                                  \
    X(TestEnsurePipelineContains)                                                      \
    X(TestEscapeMalformedPipelines)                                                    \
    X(TestEscapeErrorsNotIgnorable)                                                    \
    X(TestEscapeSetErrorsNotIgnorable)                                                 \
    X(TestRedundantFuncs)                                                              \
    X(TestIndirectPrint)                                                               \
    X(TestEmptyTemplateHTML)                                                           \
    X(TestPipeToMethodIsEscaped)                                                       \
    X(TestErrorOnUndefined)                                                            \
    X(TestIdempotentExecute)                                                           \
    X(BenchmarkEscapedExecute)                                                         \
    X(TestOrphanedTemplate)                                                            \
    X(TestAliasedParseTreeDoesNotOverescape)                                           \
    X(TestMetaContentEscapeGODEBUG)                                                    \
    X(TestIssue81821)                                                                  \
    X(TestCVE202656858)                                                                \
    X(TestIssue81823)                                                                  \
    X(BenchmarkTemplateSpecialTags)

TESTING_MAIN(TESTS)
