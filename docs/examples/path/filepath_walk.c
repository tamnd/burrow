#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"

typedef struct Lister {
    Alloc *a;
    Str root;
} Lister;

// doc: walk
/* Prints each name under the root with slashes, and leaves out .git. */
static Error list(void *env, Str path, FsDirEntry d, Error err) {
    Lister *l = env;
    if (BURROW_FAILED(err))
        return err;
    if (d.vt->is_dir(d.data) && str_eq(d.vt->name(d.data), BURROW_S(".git")))
        return filepath_skip_dir;
    Str rel = filepath_rel(l->a, l->root, path, &err);
    Str s = filepath_to_slash(l->a, rel);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
    return BURROW_NO_ERROR;
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    Str root = os_mkdir_temp(a, BURROW_S(""), BURROW_S("filepath-example-*"), &err);
    if (BURROW_FAILED(err))
        return 1;
    (void)os_mkdir_all(filepath_join_v(a, 2, root, BURROW_S(".git/objects")), 0755);
    (void)os_mkdir_all(filepath_join_v(a, 2, root, BURROW_S("docs")), 0755);
    (void)os_write_file(filepath_join_v(a, 2, root, BURROW_S("README.md")),
                        slice_nil(TYPE_BYTE), 0644);
    (void)os_write_file(filepath_join_v(a, 2, root, BURROW_S("CHANGES.md")),
                        slice_nil(TYPE_BYTE), 0644);
    (void)os_write_file(filepath_join_v(a, 2, root, BURROW_S("docs/intro.md")),
                        slice_nil(TYPE_BYTE), 0644);

    Lister l = {a, root};
    err = filepath_walk_dir(a, root, BURROW_FN(FsWalkDirFunc, list, &l));
    if (BURROW_FAILED(err))
        printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);

    // doc: glob
    Slice md = filepath_glob(a, filepath_join_v(a, 2, root, BURROW_S("*.md")), &err);
    for (Int i = 0; i < md.len; i++) {
        Str base = filepath_base(*(Str *)slice_at(md, i));
        printf("%.*s\n", (int)base.len,
               (const char *)base.p); /* CHANGES.md, README.md */
    }
    // doc: end

    // doc: abs
    Str abs = filepath_abs(a, BURROW_S("docs/../README.md"), &err);
    bool ok = BURROW_OK(err) && filepath_is_abs(abs); /* true, and abs is clean */
    Str real = filepath_eval_symlinks(a, root, &err); /* root with no links in it */
    // doc: end
    printf("%d %d\n", ok, BURROW_OK(err) && filepath_is_abs(real));

    (void)os_remove_all(root);
    arena_free(&ar);
    return 0;
}

/* Output:
.
CHANGES.md
README.md
docs
docs/intro.md
CHANGES.md
README.md
1 1
*/
