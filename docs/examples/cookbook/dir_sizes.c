// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/path/filepath.h"

typedef struct Totals {
    Alloc *a;
    Map *by_ext; /* extension to bytes */
    int64_t all;
    Int files;
} Totals;

static Error add(void *env, Str path, FsDirEntry d, Error err) {
    Totals *t = env;
    if (FAILED(err))
        return err;
    if (d.vt->is_dir(d.data)) {
        if (str_eq(d.vt->name(d.data), S(".git")))
            return filepath_skip_dir;
        return NO_ERROR;
    }
    FsFileInfo fi = d.vt->info(d.data, t->a, &err);
    if (FAILED(err))
        return err;
    int64_t size = fi.vt->size(fi.data);
    Str ext = filepath_ext(path);
    int64_t *sum = MAP_GET(Str, int64_t, t->by_ext, ext);
    MAP_SET(Str, int64_t, t->by_ext, ext, sum != NULL ? *sum + size : size);
    t->all += size;
    t->files++;
    return NO_ERROR;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A small tree to walk. */
    os_mkdir_all(S("tree/src"), 0755);
    os_mkdir_all(S("tree/.git"), 0755);
    os_write_file(S("tree/README.md"), BURROW_B("# tree\n"), 0644);
    os_write_file(S("tree/src/main.c"), BURROW_B("int main(void) { return 0; }\n"),
                  0644);
    os_write_file(S("tree/src/util.c"), BURROW_B("int two(void) { return 2; }\n"),
                  0644);
    os_write_file(S("tree/src/util.h"), BURROW_B("int two(void);\n"), 0644);
    os_write_file(S("tree/.git/HEAD"), BURROW_B("ref: refs/heads/main\n"), 0644);

    Totals t = {a, map_make(a, TYPE_STRING, TYPE_INT64, 0), 0, 0};
    Error err = filepath_walk_dir(a, S("tree"), FN(FsWalkDirFunc, add, &t));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_println_v(t.by_ext);
    fmt_println_v(t.files, "files,", t.all, "bytes");

    os_remove_all(S("tree"));
    arena_free(&ar);
}
// doc: end

/* Output:
map[.c:57 .h:15 .md:7]
4 files, 79 bytes
*/
