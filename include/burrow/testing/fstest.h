/* testing/fstest, an FS in memory for tests, and a check for any FS.
 *
 * A MapFS is a map from names to files. Only the files need to be in it: the
 * directories they are in exist without being listed, the way the directories
 * in a zip file do. A file with FS_MODE_DIR set in its mode is a directory,
 * which is how an empty directory or one with a mode of its own gets in, and a
 * file with FS_MODE_SYMLINK set is a symbolic link to the name in its data.
 *
 *     FstestMapFS fsys = fstest_map_fs_make(a);
 *     FstestMapFile hello = {.data = bytes_of("hello, world\n")};
 *     fstest_map_fs_set(fsys, BURROW_S("dir/hello.txt"), &hello);
 *     Slice data = fs_read_file(a, fstest_map_fs_as_fs(fsys), BURROW_S("dir/hello.txt"), &err);
 *
 * The map holds the names and pointers to the files, not copies, so both have
 * to outlive it. Opening a file does not copy it either, so a file changed
 * while it is open reads as changed.
 *
 * fstest_test_fs checks an FS implementation, any FS and not only a MapFS:
 *
 *     Error err = fstest_test_fs_v(os_dir_fs(a, dir), BURROW_S("hello.txt"));
 *     if (BURROW_FAILED(err))
 *         testing_t_fatal_v(t, err);
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package testing/fstest */

#ifndef BURROW_TESTING_FSTEST_H
#define BURROW_TESTING_FSTEST_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fstest.MapFile: one file in a MapFS. */
typedef struct FstestMapFile {
    Slice data;      /* the contents, or where a symbolic link points */
    FsFileMode mode; /* what the FileInfo's mode says */
    Time mod_time;   /* what the FileInfo's mod_time says */
    Any sys;         /* what the FileInfo's sys says */
} FstestMapFile;

/* fstest.MapFS: a Map from Str to FstestMapFile *. It is a value holding the
 * map, the way Go's map is, so copies share the files. */
typedef struct FstestMapFS {
    Map *files;
} FstestMapFS;

/* An empty MapFS with its map from a. A nil map when a refuses. */
BURROW_OWNS(ret) FstestMapFS fstest_map_fs_make(Alloc *a);

/* fsys[name] = f. name and f are kept as they are and not copied. false when
 * the map could not grow. */
bool fstest_map_fs_set(FstestMapFS fsys, Str name, FstestMapFile *f);

/* The MapFS as an Fs, with every one of the optional methods. It shares the
 * map. */
Fs fstest_map_fs_as_fs(FstestMapFS fsys);

/* The methods, which are what fstest_map_fs_as_fs's slots call. Open fails
 * with an FsPathError wrapping fs_err_not_exist for a name that is not there or
 * is not valid. A chain of symbolic links more than 255 long is taken as not
 * there rather than followed forever. */
BURROW_OWNS(ret) FsFile fstest_map_fs_open(FstestMapFS fsys, Alloc *a, Str name,
                                           Error *err);
BURROW_OWNS(ret) Slice fstest_map_fs_read_file(FstestMapFS fsys, Alloc *a, Str name,
                                               Error *err);
BURROW_OWNS(ret) FsFileInfo fstest_map_fs_stat(FstestMapFS fsys, Alloc *a, Str name,
                                               Error *err);
BURROW_OWNS(ret) Slice fstest_map_fs_read_dir(FstestMapFS fsys, Alloc *a, Str name,
                                              Error *err);
BURROW_OWNS(ret) Slice fstest_map_fs_glob(FstestMapFS fsys, Alloc *a, Str pattern,
                                          Error *err);
BURROW_OWNS(ret) Fs fstest_map_fs_sub(FstestMapFS fsys, Alloc *a, Str dir, Error *err);
BURROW_OWNS(ret) Str fstest_map_fs_read_link(FstestMapFS fsys, Alloc *a, Str name,
                                             Error *err);
BURROW_OWNS(ret) FsFileInfo fstest_map_fs_lstat(FstestMapFS fsys, Alloc *a, Str name,
                                                Error *err);

/* fstest.TestFS. Walks every file and directory in fsys and checks that each
 * behaves the way the fs package says it should, through every extension
 * method fsys has. Symbolic links are not followed, but when fsys has
 * read_link and lstat, their Lstat is checked. It also checks that fsys holds
 * at least the names in expected, a Slice of Str. With none, fsys has to be
 * empty.
 *
 * The error is nil when nothing was wrong. Otherwise it starts "TestFS found
 * errors:" and wraps every problem found, one to a line or more, and
 * errors_is and errors_as look through it. fsys must not change while this
 * runs. */
BURROW_STATIC(ret) Error fstest_test_fs(Fs fsys, Slice expected); /* of Str */

/* The same with the names listed in the call. */
#define fstest_test_fs_v(fsys, ...)                                                    \
    fstest_test_fs((fsys),                                                             \
                   slice_from((Str[]){__VA_ARGS__},                                    \
                              (Int)(sizeof((Str[]){__VA_ARGS__}) / sizeof(Str)),       \
                              (Int)(sizeof((Str[]){__VA_ARGS__}) / sizeof(Str)),       \
                              TYPE_STRING))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_FSTEST_H */
