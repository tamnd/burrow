// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

#define COMMENT_FIELDS(F, T)                                                           \
    F(T, Str, Author, "")                                                              \
    F(T, Str, Body, "")                                                                \
    F(T, Str, Website, "")                                                             \
    F(T, Str, Topic, "")
BURROW_STRUCT(Comment, COMMENT_FIELDS);

/* html/template knows where in the page each value lands and escapes it for
 * that place: text, an attribute, a URL, or a query parameter in a URL. */
static const char page[] = "<p class=\"comment\" title=\"{{.Author}}\">{{.Body}}</p>\n"
                           "<a href=\"{{.Website}}\">{{.Author}}'s site</a>\n"
                           "<a href=\"/search?q={{.Topic}}\">more on {{.Topic}}</a>\n";

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    HtmlTemplate *t = html_template_new(a, S("page"));
    Error err = NO_ERROR;
    t = html_template_parse(t, str_from_cstr(page), &err);
    t = html_template_must(t, err);

    IoWriter out = os_file_as_io_writer(os_stdout);
    Comment good = {S("Ada"), S("Nice post."), S("https://example.com/ada"),
                    S("fish & chips")};
    err = html_template_execute(t, out, ANY(TYPE_OF(Comment), &good));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* Someone trying to break out of the page gets text, not markup. A URL
     * with a scheme that could run code is replaced with #ZgotmplZ. */
    Comment bad = {S("\"><b>Mallory"), S("<script>alert('hi')</script>"),
                   S("javascript:alert(1)"), S("a\"b")};
    err = html_template_execute(t, out, ANY(TYPE_OF(Comment), &bad));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    arena_free(&ar);
}
// doc: end

/* Output:
<p class="comment" title="Ada">Nice post.</p>
<a href="https://example.com/ada">Ada's site</a>
<a href="/search?q=fish%20%26%20chips">more on fish &amp; chips</a>
<p class="comment" title="&#34;&gt;&lt;b&gt;Mallory">&lt;script&gt;alert(&#39;hi&#39;)&lt;/script&gt;</p>
<a href="#ZgotmplZ">&#34;&gt;&lt;b&gt;Mallory's site</a>
<a href="/search?q=a%22b">more on a&#34;b</a>
*/
