// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = NO_ERROR;
    Url *u =
        url_parse(a, S("https://example.com:8443/docs/search?q=go+in+c&page=2"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_println_v("host:", url_hostname(u), "port:", url_port(u), "path:", u->path);

    /* url_query decodes the query, so the plus signs come back as spaces. */
    UrlValues q = url_query(u, a);
    fmt_printf_v("q is %q, page is %s\n", url_values_get(q, S("q")),
                 url_values_get(q, S("page")));

    /* Change it and put it back. url_values_encode sorts the keys. */
    url_values_set(q, S("page"), S("3"));
    url_values_add(q, S("lang"), S("en"));
    u->raw_query = url_values_encode(q, a);
    fmt_println_v(url_string(u, a));

    /* A link on the page, made absolute the way a browser would. */
    Url *ref = url_parse(a, S("../about#team"), &err);
    Url *abs = url_resolve_reference(u, a, ref);
    fmt_println_v(url_string(abs, a));

    /* A string that isn't a URL. */
    url_parse(a, S("http://[::1"), &err);
    fmt_println_v(err);
    arena_free(&ar);
}
// doc: end

/* Output:
host: example.com port: 8443 path: /docs/search
q is "go in c", page is 2
https://example.com:8443/docs/search?lang=en&page=3&q=go+in+c
https://example.com:8443/about#team
parse "http://[::1": missing ']' in host
*/
