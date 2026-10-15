# HTML

`burrow/html.h` is Go's `html` package, which escapes text so it can go into an HTML page and turns character references back into text. `burrow/html/template.h` is Go's `html/template`, templates that escape what they print for the place in the page it lands.

## Escaping

`html_escape_string` replaces the five characters that mean something in HTML, `<`, `>`, `&`, `'` and `"`, with character references. Nothing else changes, so the result is safe between tags and inside a quoted attribute value:

<!-- example: ../examples/html/escape.c#escape -->
```c
Str s = BURROW_S("\"Fran & Freddie's Diner\" <tasty@example.com>");
fmt_println_v(html_escape_string(a, s));
```

That prints:

```
&#34;Fran &amp; Freddie&#39;s Diner&#34; &lt;tasty@example.com&gt;
```

When there is nothing to escape you get the same string back, pointing at the same bytes, and nothing is allocated. Otherwise the result is made in the allocator you pass, and comes back empty if the allocator refuses.

## Unescaping

`html_unescape_string` goes the other way, and it knows far more than the five references escaping makes. Every named reference in HTML5 works, as do decimal and hex numeric references:

<!-- example: ../examples/html/escape.c#unescape -->
```c
Str s = BURROW_S("&quot;Fran &amp; Freddie&#39;s Diner&quot; "
                 "&lt;tasty@example.com&gt;");
fmt_println_v(html_unescape_string(a, s));
fmt_println_v(html_unescape_string(a, BURROW_S("caf&eacute; caf&#233; caf&#xE9;")));
fmt_println_v(html_unescape_string(a, BURROW_S("&copy 2026, &notanentity;")));
```

That prints:

```
"Fran & Freddie's Diner" <tasty@example.com>
café café café
© 2026, ¬anentity;
```

The last line shows the rules browsers follow for text that only looks like a reference. A few old names, `&copy` and `&not` among them, are recognised without their semicolon, and the longest such name at the start of a word wins, which is why `&notanentity;` comes out starting with `¬`. Anything that matches nothing is left alone.

Unescaping what escaping made always gives back the text you started with. The reverse is not true, since `&eacute;`, `&#233;` and `&#xE9;` all unescape to the same `é` and escaping leaves that `é` as it is.

## Templates

An `HtmlTemplate` is a `text/template` template that escapes its output, and it is used the same way. The template language, the data, the function maps and the errors are all text/template's, which the [text guide](text.md) covers. Here is Go's first example, with a struct to hold the data:

<!-- example: ../examples/html/template.c#types -->
```c
BURROW_SLICE_TYPE(Items, Str);
#define PAGE_FIELDS(F, T) F(T, Str, Title, "") F(T, Items, Items, "")
BURROW_STRUCT(Page, PAGE_FIELDS);
```

<!-- example: ../examples/html/template.c#webpage -->
```c
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
```

The title and the items are plain text, so nothing in them needed escaping, and the output is what text/template would have made:

```
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
```

The difference shows when the data holds something that means something in HTML:

<!-- example: ../examples/html/template.c#autoescaping -->
```c
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
```

```
Hello, &lt;script&gt;alert(&#39;you have been pwned&#39;)&lt;/script&gt;!
```

## Escaping for the context

The first run of a template escapes it. That walks the template's text the way a browser would read it, works out where each action's output lands, and rewrites the action so its value goes through the right escapers for that place. Text between tags is HTML escaped. A URL gets percent encoding, a script gets a JavaScript value, and an attribute value gets HTML escaping on top of that. The same string comes out three different ways in one line:

<!-- example: ../examples/html/template.c#contexts -->
```c
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
```

```
<a href="/search?q=O%27Reilly%20%26%20%3cSons%3e" onclick="alert(&#34;O&#39;Reilly \u0026 \u003cSons\u003e&#34;)">O&#39;Reilly &amp; &lt;Sons&gt;</a>
```

Escaping happens once, and the template is not escaped again on later runs. After the first run, Parse and Clone fail on the template and on every template associated with it, because a change could make the escaping wrong.

## Trusted content

A value of one of the content types, `HtmlTemplateHTML`, `HtmlTemplateJS`, `HtmlTemplateURL` and the rest, is a `Str` the template trusts to be safe where that kind of content goes. It is left as it is there, where a plain `Str` with the same bytes is escaped:

<!-- example: ../examples/html/template.c#trusted -->
```c
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
```

```
<p>&lt;b&gt;World&lt;/b&gt;</p><p><b>World</b></p>
```

Using one of these types is a promise that the content is safe, and nothing checks it. Text that came from a user should never go in one.

A value that can't be made safe for its place is replaced. A URL with a scheme other than http, https or mailto becomes `#ZgotmplZ`, so a link can't run script:

<!-- example: ../examples/html/template.c#unsafe -->
```c
Error err = BURROW_NO_ERROR;
HtmlTemplate *t = html_template_parse(html_template_new(a, BURROW_S("unsafe")),
                                      BURROW_S("<a href=\"{{.}}\">x</a>"), &err);
t = html_template_must(t, err);
Str url = BURROW_S("javascript:alert(1)");
err = html_template_execute(t, out, BURROW_ANY(TYPE_STRING, &url));
if (BURROW_FAILED(err))
    fmt_fprintln_v(out, "executing template:", err);
```

```
<a href="#ZgotmplZ">x</a>
```

## Errors

Some templates can't be escaped at all, such as one where the branches of an `{{if}}` leave the page in different places. Running one gives an `HtmlTemplateError` that says where the problem is, and the template gives the same error every time it runs:

<!-- example: ../examples/html/template.c#broken -->
```c
Error err = BURROW_NO_ERROR;
HtmlTemplate *t =
    html_template_parse(html_template_new(a, BURROW_S("broken")),
                        BURROW_S("<a href=\"{{if .}}/x\">{{end}}"), &err);
t = html_template_must(t, err);
bool yes = true;
err = html_template_execute(t, out, BURROW_ANY(TYPE_BOOL, &yes));
if (BURROW_FAILED(err))
    fmt_fprintln_v(out, err);
```

```
html/template:broken:1:14: {{if}} branches end in different contexts: {stateText delimNone urlPartNone jsCtxRegexp [] attrNone elementNone <nil>}, {stateURL delimDoubleQuote urlPartNone jsCtxRegexp [] attrURL elementNone <nil>}
```
