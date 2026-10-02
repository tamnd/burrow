#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/testing/fstest.h"

// doc: cache
/* The code under test: an FS over a MapFS that keeps the last file ReadFile
 * read, and has a bug. It hands out the cached bytes themselves, so a caller
 * that changes them changes the cache. */
typedef struct CachingFS {
    FstestMapFS files;
    Alloc *a;
    Str last_name;
    Slice last_data;
} CachingFS;

static FsFile caching_open(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_open(((CachingFS *)self)->files, a, name, err);
}

static Slice caching_read_file(void *self, Alloc *a, Str name, Error *err) {
    (void)a;
    CachingFS *c = (CachingFS *)self;
    if (c->last_data.p != NULL && str_eq(name, c->last_name)) {
        *err = BURROW_NO_ERROR;
        return c->last_data;
    }
    Slice data = fstest_map_fs_read_file(c->files, c->a, name, err);
    if (BURROW_OK(*err)) {
        c->last_name = str_clone(c->a, name);
        c->last_data = data;
    }
    return data;
}

static const FsVT caching_fs_vt = {.open = caching_open,
                                   .read_file = caching_read_file};
// doc: end

static FstestMapFS sample_files(Alloc *a) {
    static FstestMapFile hello, readme;
    hello.data = slice_from_str(a, BURROW_S("hi\n"));
    readme.data = slice_from_str(a, BURROW_S("read me\n"));
    FstestMapFS files = fstest_map_fs_make(a);
    fstest_map_fs_set(files, BURROW_S("docs/hello.txt"), &hello);
    fstest_map_fs_set(files, BURROW_S("README"), &readme);
    return files;
}

// doc: testfs
static void TestMapFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Fs fsys = fstest_map_fs_as_fs(sample_files(arena_allocator(&ar)));
    Error err = fstest_test_fs_v(fsys, BURROW_S("README"), BURROW_S("docs/hello.txt"));
    arena_free(&ar);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    fmt_println_v("MapFS passes");
}

static void TestCachingFS(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CachingFS c = {.files = sample_files(a), .a = a};
    Fs fsys = {&caching_fs_vt, &c};
    Error err = fstest_test_fs_v(fsys, BURROW_S("README"), BURROW_S("docs/hello.txt"));
    fmt_println_v(err);
    arena_free(&ar);
}
// doc: end

#define TESTS(X) X(TestMapFS) X(TestCachingFS)
TESTING_MAIN(TESTS)

/* Output:
MapFS passes
TestFS found errors:
README: Readall vs second fsys.ReadFile: different data returned
	"read me\n"
	"sfbe!nf\v"
README: ReadAll vs fs.ReadFile: different data returned
	"read me\n"
	"sfbe!nf\v"
docs/hello.txt: Readall vs second fsys.ReadFile: different data returned
	"hi\n"
	"ij\v"
docs/hello.txt: ReadAll vs fs.ReadFile: different data returned
	"hi\n"
	"ij\v"
PASS
*/
