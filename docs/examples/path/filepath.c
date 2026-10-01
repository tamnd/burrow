#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"

/* The results use the host's separator, so this prints them with slashes to
 * give the same output everywhere. */
static void show(Alloc *a, Str s) {
    Str t = filepath_to_slash(a, s);
    printf("%.*s\n", (int)t.len, (const char *)t.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: join
    Str dir = filepath_join_v(a, 3, BURROW_S("build"), BURROW_S("tests/../lib"),
                              BURROW_S("libburrow.a")); /* build/lib/libburrow.a */
    Str ext = filepath_ext(dir);                        /* ".a" */
    Str up = filepath_dir(a, dir);                      /* build/lib */
    // doc: end
    show(a, dir);
    show(a, ext);
    show(a, up);

    // doc: rel
    Error err;
    Str rel =
        filepath_rel(a, BURROW_S("/srv/www"), BURROW_S("/srv/www/static/site.css"),
                     &err); /* static/site.css */
    Str out = filepath_rel(a, BURROW_S("/srv/www"), BURROW_S("www"), &err);
    if (BURROW_FAILED(err))
        printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
    // doc: end
    show(a, rel);
    printf("%d\n", (int)out.len);

    // doc: local
    bool inside = filepath_is_local(BURROW_S("uploads/a.png")); /* true */
    bool escapes = filepath_is_local(BURROW_S("a/../../etc"));  /* false */
    Str name = filepath_localize(a, BURROW_S("uploads/a.png"), &err);
    // doc: end
    printf("%d %d\n", inside, escapes);
    show(a, name);

    arena_free(&ar);
    return 0;
}

/* Output:
build/lib/libburrow.a
.a
build/lib
Rel: can't make www relative to /srv/www
static/site.css
0
1 0
uploads/a.png
*/
