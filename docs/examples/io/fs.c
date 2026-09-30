#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

// doc: walker
static Error print_path(void *env, Str path, FsDirEntry d, Error err) {
    (void)env;
    if (BURROW_FAILED(err))
        return err;
    printf(BURROW_STR_FMT "%s\n", BURROW_STR_ARG(path),
           d.vt->is_dir(d.data) ? "/" : "");
    return BURROW_NO_ERROR;
}
// doc: end

static void print_names(const char *label, Slice names) {
    printf("%s:", label);
    for (Int i = 0; i < names.len; i++)
        printf(" " BURROW_STR_FMT, BURROW_STR_ARG(*(Str *)slice_at(names, i)));
    printf("\n");
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    // doc: mapfs
    FstestMapFile readme = {.data = slice_from_str(a, BURROW_S("read me first\n"))};
    FstestMapFile main_go = {.data = slice_from_str(a, BURROW_S("package main\n")),
                             .mode = 0644};
    FstestMapFile util_go = {.data = slice_from_str(a, BURROW_S("package util\n")),
                             .mode = 0644};
    FstestMapFS files = fstest_map_fs_make(a);
    fstest_map_fs_set(files, BURROW_S("README"), &readme);
    fstest_map_fs_set(files, BURROW_S("cmd/main.go"), &main_go);
    fstest_map_fs_set(files, BURROW_S("internal/util/util.go"), &util_go);
    Fs fsys = fstest_map_fs_as_fs(files);
    // doc: end

    // doc: readfile
    Slice data = fs_read_file(a, fsys, BURROW_S("README"), &err);
    // doc: end
    printf("README: %.*s", (int)data.len, (const char *)data.p);

    // doc: missing
    fs_read_file(a, fsys, BURROW_S("LICENSE"), &err);
    if (errors_is(err, fs_err_not_exist))
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    // doc: end

    // doc: walk
    err =
        fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, print_path, NULL));
    // doc: end

    // doc: glob
    Slice matches = fs_glob(a, fsys, BURROW_S("*/*.go"), &err);
    // doc: end
    print_names("glob", matches);

    // doc: sub
    Fs internal = fs_sub(a, fsys, BURROW_S("internal"), &err);
    Slice entries = fs_read_dir(a, internal, BURROW_S("util"), &err);
    for (Int i = 0; i < entries.len; i++) {
        FsDirEntry *e = (FsDirEntry *)slice_at(entries, i);
        FsFileInfo info = e->vt->info(e->data, a, &err);
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(fs_format_file_info(a, info)));
    }
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
README: read me first
open LICENSE: file does not exist
./
README
cmd/
cmd/main.go
internal/
internal/util/
internal/util/util.go
glob: cmd/main.go
-rw-r--r-- 13 0001-01-01 00:00:00 util.go
*/
