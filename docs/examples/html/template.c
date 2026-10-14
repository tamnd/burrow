#include "burrow/burrow.h"

// doc: types
BURROW_SLICE_TYPE(Items, Str);
#define PAGE_FIELDS(F, T) F(T, Str, Title, "") F(T, Items, Items, "")
BURROW_STRUCT(Page, PAGE_FIELDS);
// doc: end

static void webpage(Alloc *a, IoWriter out) {
    // doc: webpage
    Str tpl = BURROW_S("\n"
                       "<!DOCTYPE html>\n"
                       "<html>\n"
                       "\t<head>\n"
                       "\t\t<meta charset=\"UTF-8\">\n"
                       "\t\t<title>{{.Title}}</title>\n"
                       "\t</head>\n"
                       "\t<body>\n"
                       "\t\t{{range .Items}}<div>{{ . }}</div>{{else}}<div><strong>no "
                       "rows</strong></div>{{end}}\n"
                       "\t</body>\n"
                       "</html>");
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t =
        html_template_parse(html_template_new(a, BURROW_S("webpage")), tpl, &err);
    t = html_template_must(t, err);

    Str items[] = {BURROW_S("My photos"), BURROW_S("My blog")};
    Page data = {BURROW_S("My page"), slice_from(items, 2, 2, TYPE_STRING)};
    err = html_template_execute(t, out, BURROW_ANY(TYPE_OF(Page), &data));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);

    Page no_items = {BURROW_S("My another page"), slice_nil(TYPE_STRING)};
    err = html_template_execute(t, out, BURROW_ANY(TYPE_OF(Page), &no_items));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);
    // doc: end
    fmt_fprint_v(out, "\n");
}

static void autoescaping(Alloc *a, IoWriter out) {
    // doc: autoescaping
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t =
        html_template_parse(html_template_new(a, BURROW_S("foo")),
                            BURROW_S("{{define \"T\"}}Hello, {{.}}!{{end}}"), &err);
    t = html_template_must(t, err);
    Str evil = BURROW_S("<script>alert('you have been pwned')</script>");
    err = html_template_execute_template(t, out, BURROW_S("T"),
                                         BURROW_ANY(TYPE_STRING, &evil));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);
    // doc: end
    fmt_fprint_v(out, "\n");
}

static void contexts(Alloc *a, IoWriter out) {
    // doc: contexts
    Str tpl =
        BURROW_S("<a href=\"/search?q={{.}}\" onclick=\"alert({{.}})\">{{.}}</a>");
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t =
        html_template_parse(html_template_new(a, BURROW_S("link")), tpl, &err);
    t = html_template_must(t, err);
    Str s = BURROW_S("O'Reilly & <Sons>");
    err = html_template_execute(t, out, BURROW_ANY(TYPE_STRING, &s));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);
    // doc: end
    fmt_fprint_v(out, "\n");
}

static void trusted(Alloc *a, IoWriter out) {
    // doc: trusted
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t = html_template_parse(html_template_new(a, BURROW_S("trusted")),
                                          BURROW_S("<p>{{.}}</p>"), &err);
    t = html_template_must(t, err);
    Str plain = BURROW_S("<b>World</b>");
    HtmlTemplateHTML html = BURROW_S("<b>World</b>");
    err = html_template_execute(t, out, BURROW_ANY(TYPE_STRING, &plain));
    if (BURROW_OK(err))
        err = html_template_execute(t, out, BURROW_ANY(TYPE_HTML_TEMPLATE_HTML, &html));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);
    // doc: end
    fmt_fprint_v(out, "\n");
}

static void unsafe(Alloc *a, IoWriter out) {
    // doc: unsafe
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t = html_template_parse(html_template_new(a, BURROW_S("unsafe")),
                                          BURROW_S("<a href=\"{{.}}\">x</a>"), &err);
    t = html_template_must(t, err);
    Str url = BURROW_S("javascript:alert(1)");
    err = html_template_execute(t, out, BURROW_ANY(TYPE_STRING, &url));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, "executing template:", err);
    // doc: end
    fmt_fprint_v(out, "\n");
}

static void broken(Alloc *a, IoWriter out) {
    // doc: broken
    Error err = BURROW_NO_ERROR;
    HtmlTemplate *t =
        html_template_parse(html_template_new(a, BURROW_S("broken")),
                            BURROW_S("<a href=\"{{if .}}/x\">{{end}}"), &err);
    t = html_template_must(t, err);
    bool yes = true;
    err = html_template_execute(t, out, BURROW_ANY(TYPE_BOOL, &yes));
    if (BURROW_FAILED(err))
        fmt_fprintln_v(out, err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoWriter out = os_file_as_io_writer(os_stdout_file());
    webpage(a, out);
    autoescaping(a, out);
    contexts(a, out);
    trusted(a, out);
    unsafe(a, out);
    broken(a, out);
    arena_free(&ar);
    return 0;
}

/* Output:

<!DOCTYPE html>
<html>
	<head>
		<meta charset="UTF-8">
		<title>My page</title>
	</head>
	<body>
		<div>My photos</div><div>My blog</div>
	</body>
</html>
<!DOCTYPE html>
<html>
	<head>
		<meta charset="UTF-8">
		<title>My another page</title>
	</head>
	<body>
		<div><strong>no rows</strong></div>
	</body>
</html>
Hello, &lt;script&gt;alert(&#39;you have been pwned&#39;)&lt;/script&gt;!
<a href="/search?q=O%27Reilly%20%26%20%3cSons%3e" onclick="alert(&#34;O&#39;Reilly \u0026 \u003cSons\u003e&#34;)">O&#39;Reilly &amp; &lt;Sons&gt;</a>
<p>&lt;b&gt;World&lt;/b&gt;</p><p><b>World</b></p>
<a href="#ZgotmplZ">x</a>
html/template:broken:1:14: {{if}} branches end in different contexts: {stateText delimNone urlPartNone jsCtxRegexp [] attrNone elementNone <nil>}, {stateURL delimDoubleQuote urlPartNone jsCtxRegexp [] attrURL elementNone <nil>}
*/
