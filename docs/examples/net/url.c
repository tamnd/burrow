#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

static void parse(Alloc *a) {
    // doc: parse
    Error err;
    Url *u = url_parse(
        a, BURROW_S("https://ann:secret@go.dev:8443/doc/a%20b?q=url&m=text#top"), &err);
    printf("%.*s | %.*s | %.*s | %.*s\n", P(u->scheme), P(url_hostname(u)),
           P(url_port(u)), P(u->path));
    printf("%.*s | %.*s | %.*s\n", P(url_escaped_path(u, a)), P(u->raw_query),
           P(u->fragment));
    printf("%.*s\n", P(url_redacted(u, a)));
    url_parse(a, BURROW_S("http://x/%zz"), &err);
    fmt_printf_v("%v\n", err);
    // doc: end
}

static void resolve(Alloc *a) {
    // doc: resolve
    Url *base = url_parse(a, BURROW_S("https://go.dev/doc/effective_go"), NULL);
    Url *next = url_parse_ref(base, a, BURROW_S("../blog/?page=2"), NULL);
    printf("%.*s\n", P(url_string(next, a)));
    Url *joined = url_join_path_v(base, a, 2, BURROW_S("../ref"), BURROW_S("spec/"));
    printf("%.*s\n", P(url_string(joined, a)));
    // doc: end
}

static void query(Alloc *a) {
    // doc: query
    Url *u = url_parse(a, BURROW_S("/search?q=c+library&tag=go&tag=c"), NULL);
    UrlValues q = url_query(u, a);
    Slice tags = url_values_get_all(q, BURROW_S("tag"));
    printf("%.*s, %d tags\n", P(url_values_get(q, BURROW_S("q"))), (int)tags.len);
    url_values_set(q, BURROW_S("page"), BURROW_S("2"));
    url_values_del(q, BURROW_S("tag"));
    printf("%.*s\n", P(url_values_encode(q, a)));
    printf("%.*s\n", P(url_path_escape(a, BURROW_S("a b/c"))));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    parse(a);
    resolve(a);
    query(a);
    arena_free(&ar);
    return 0;
}

/* Output:
https | go.dev | 8443 | /doc/a b
/doc/a%20b | q=url&m=text | top
https://ann:xxxxx@go.dev:8443/doc/a%20b?q=url&m=text#top
parse "http://x/%zz": invalid URL escape "%zz"
https://go.dev/blog/?page=2
https://go.dev/doc/ref/spec/
c library, 2 tags
page=2&q=c+library
a%20b%2Fc
*/
