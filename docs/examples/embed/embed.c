#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

// doc: declare
BURROW_EMBED_FILE(motd, "motd.txt");
BURROW_EMBED_FS(site, "static");
// doc: end

static Error show(void *env, Str path, FsDirEntry d, Error err) {
    (void)env;
    if (BURROW_FAILED(err))
        return err;
    printf(BURROW_STR_FMT "%s\n", BURROW_STR_ARG(path),
           d.vt->is_dir(d.data) ? "/" : "");
    return BURROW_NO_ERROR;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    // doc: string
    printf(BURROW_STR_FMT, BURROW_STR_ARG(motd));
    // doc: end

    // doc: fs
    Fs fsys = embed_fs_as_fs(&site);
    Slice page = fs_read_file(a, fsys, BURROW_S("static/index.html"), &err);
    printf("%.*s", (int)page.len, (const char *)page.p);
    // doc: end

    // doc: walk
    err = fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, show, NULL));
    // doc: end

    // doc: sub
    Fs root = fs_sub(a, fsys, BURROW_S("static"), &err);
    Slice css = fs_read_file(a, root, BURROW_S("style.css"), &err);
    printf("%.*s", (int)css.len, (const char *)css.p);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
Welcome back. The build is green.
<h1>hello</h1>
./
static/
static/index.html
static/style.css
h1 { color: teal; }
*/
