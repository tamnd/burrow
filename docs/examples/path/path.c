#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/path.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: clean
    Str c = path_clean(a, BURROW_S("a/c/../b//./d/")); /* a/b/d */
    Str r = path_clean(a, BURROW_S("/../x"));          /* /x */
    Str j = path_join_v(a, 3, BURROW_S("usr"), BURROW_S(""),
                        BURROW_S("lib/")); /* usr/lib */
    // doc: end
    printf("%.*s %.*s %.*s\n", (int)c.len, (const char *)c.p, (int)r.len,
           (const char *)r.p, (int)j.len, (const char *)j.p);

    // doc: parts
    Str p = BURROW_S("static/css/site.min.css");
    Str file;
    Str dir = path_split(p, &file); /* "static/css/" and "site.min.css" */
    Str ext = path_ext(p);          /* ".css" */
    Str base = path_base(p);        /* "site.min.css" */
    Str up = path_dir(a, p);        /* "static/css" */
    // doc: end
    printf("%.*s|%.*s %.*s %.*s %.*s\n", (int)dir.len, (const char *)dir.p,
           (int)file.len, (const char *)file.p, (int)ext.len, (const char *)ext.p,
           (int)base.len, (const char *)base.p, (int)up.len, (const char *)up.p);

    // doc: match
    Error err;
    bool css = path_match(BURROW_S("static/*/*.css"), p, &err);  /* true */
    bool deep = path_match(BURROW_S("static/*.css"), p, &err);   /* false */
    bool bad = path_match(BURROW_S("[z-"), BURROW_S("z"), &err); /* false */
    if (BURROW_FAILED(err))
        printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
    // doc: end
    printf("%d %d %d\n", css, deep, bad);

    arena_free(&ar);
    return 0;
}

/* Output:
a/b/d /x usr/lib
static/css/|site.min.css .css site.min.css static/css
syntax error in pattern
1 0 0
*/
