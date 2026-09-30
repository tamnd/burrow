/* io/fs, the interfaces a file system is seen through.
 *
 * An FS is anything that can open a file by name: a directory on disk, the
 * files inside a zip or tar archive, a map in memory. Code written against
 * Fs works with all of them. The names are slash-separated paths with no
 * leading slash, no "." or ".." elements and no empty ones, which is what
 * fs_valid_path checks, so "a/b.txt" and "." are names and "/a" and "a/../b"
 * are not.
 *
 *     Slice data = fs_read_file(a, fsys, BURROW_S("notes/today.txt"), &err);
 *     Slice names = fs_glob(a, fsys, BURROW_S("*.txt"), &err);
 *     err = fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, visit, &env));
 *
 * Go adds abilities to an FS with more interfaces, ReadFileFS, StatFS, GlobFS
 * and the rest, and the functions here ask for them with a type assertion.
 * A C vtable cannot be asked for a method it was not built with, so FsVT
 * has a slot for each of them and a NULL slot is an FS without that method.
 * The functions fall back to open, as Go's do. FsReadFileFS and the others are
 * other names for Fs, kept so that the Go names mean something here. File
 * and ReadDirFile are the same: read_dir is a slot in FsFileVT that may be
 * NULL.
 *
 * Everything a function or a slot gives back is allocated from the Alloc it
 * was handed, and nothing here has a free. Give it an arena and drop the arena
 * when you are done with the results.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package io/fs */

#ifndef BURROW_IO_FS_H
#define BURROW_IO_FS_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- FileMode */

/* fs.FileMode: the type of a file in the top bits and the Unix permission bits
 * in the bottom nine. The values are Go's, so a mode means the same thing on
 * both sides of anything that stores one. */
typedef uint32_t FsFileMode;

#define FS_MODE_DIR ((FsFileMode)1U << 31)        /* d: is a directory */
#define FS_MODE_APPEND ((FsFileMode)1U << 30)     /* a: append-only */
#define FS_MODE_EXCLUSIVE ((FsFileMode)1U << 29)  /* l: exclusive use */
#define FS_MODE_TEMPORARY ((FsFileMode)1U << 28)  /* T: temporary file, Plan 9 only */
#define FS_MODE_SYMLINK ((FsFileMode)1U << 27)    /* L: symbolic link */
#define FS_MODE_DEVICE ((FsFileMode)1U << 26)     /* D: device file */
#define FS_MODE_NAMED_PIPE ((FsFileMode)1U << 25) /* p: named pipe */
#define FS_MODE_SOCKET ((FsFileMode)1U << 24)     /* S: Unix domain socket */
#define FS_MODE_SETUID ((FsFileMode)1U << 23)     /* u: setuid */
#define FS_MODE_SETGID ((FsFileMode)1U << 22)     /* g: setgid */
#define FS_MODE_CHAR_DEVICE                                                            \
    ((FsFileMode)1U << 21)                       /* c: character device, with DEVICE */
#define FS_MODE_STICKY ((FsFileMode)1U << 20)    /* t: sticky */
#define FS_MODE_IRREGULAR ((FsFileMode)1U << 19) /* ?: not a regular file */

/* The bits that say what type of file it is. None of them set is a regular
 * file. */
#define FS_MODE_TYPE                                                                   \
    (FS_MODE_DIR | FS_MODE_SYMLINK | FS_MODE_NAMED_PIPE | FS_MODE_SOCKET |             \
     FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE | FS_MODE_IRREGULAR)

/* The Unix permission bits. */
#define FS_MODE_PERM ((FsFileMode)0777)

/* FileMode.String: a letter for each type bit that is set, from "dalTLDpSugct?",
 * or "-" when none is, then rwxrwxrwx with "-" for the permission bits that are
 * not. So a directory anyone can read is "dr-xr-xr-x". Empty when a refuses. */
BURROW_OWNS(ret) Str fs_file_mode_string(FsFileMode m, Alloc *a);

/* FileMode.IsDir, IsRegular, Perm and Type. */
bool fs_file_mode_is_dir(FsFileMode m);
bool fs_file_mode_is_regular(FsFileMode m);
FsFileMode fs_file_mode_perm(FsFileMode m);
FsFileMode fs_file_mode_type(FsFileMode m);

/* ---------------------------------------------------------------- FileInfo */

/* fs.FileInfo: what a stat says about a file. */
typedef struct FsFileInfoVT {
    const Type *self_type;
    Str (*name)(void *self); /* the base name */
    int64_t (*size)(void *self);
    FsFileMode (*mode)(void *self);
    Time (*mod_time)(void *self);
    bool (*is_dir)(void *self);
    Any (*sys)(void *self); /* whatever the FS keeps underneath, or a nil Any */
} FsFileInfoVT;

typedef struct FsFileInfo {
    const FsFileInfoVT *vt;
    void *data;
} FsFileInfo;

extern const Type *const TYPE_FS_FILE_INFO;

/* fs.FormatFileInfo: the mode, the size, the time and the name, with a "/"
 * after a directory's name, as in
 *
 *     -rw-r--r-- 100 1970-01-01 12:00:00 hello.go
 *
 * The time is written with TIME_DATE_TIME in its own location. */
BURROW_OWNS(ret) Str fs_format_file_info(Alloc *a, FsFileInfo info);

/* ---------------------------------------------------------------- DirEntry */

/* fs.DirEntry: one entry read from a directory. Most FS types can say what an
 * entry's name and type are without a stat, and info is the stat, which may
 * cost more and may fail if the file went away after the directory was read. */
typedef struct FsDirEntryVT {
    const Type *self_type;
    Str (*name)(void *self);
    bool (*is_dir)(void *self);
    FsFileMode (*type)(void *self); /* the type bits of the mode only */
    FsFileInfo (*info)(void *self, Alloc *a, Error *err);
} FsDirEntryVT;

typedef struct FsDirEntry {
    const FsDirEntryVT *vt;
    void *data;
} FsDirEntry;

extern const Type *const TYPE_FS_DIR_ENTRY;

/* fs.FormatDirEntry: the type letters and the name, with a "/" after a
 * directory's, as in "d subdir/" or "- hello.go". */
BURROW_OWNS(ret) Str fs_format_dir_entry(Alloc *a, FsDirEntry dir);

/* fs.FileInfoToDirEntry: info as a DirEntry, whose info gives info back. A nil
 * info gives a nil entry, and so does an a that refuses. */
BURROW_OWNS(ret) FsDirEntry fs_file_info_to_dir_entry(Alloc *a, FsFileInfo info);

/* -------------------------------------------------------------------- File */

/* fs.File, an open file: io.Reader and io.Closer and a stat.
 *
 * read_dir is fs.ReadDirFile's method and may be NULL, which is a file that is
 * not a directory. It reads up to n entries in the order the directory has
 * them, and each call carries on from the last. With n > 0 it gives io_eof and
 * no entries at the end. With n <= 0 it gives everything left and no error,
 * even at the end. */
typedef struct FsFileVT {
    IoReadCloserVT read_closer;
    FsFileInfo (*stat)(void *self, Alloc *a, Error *err);
    Slice (*read_dir)(void *self, Alloc *a, Int n, Error *err); /* of FsDirEntry */
} FsFileVT;

typedef struct FsFile {
    const FsFileVT *vt;
    void *data;
} FsFile;

extern const Type *const TYPE_FS_FILE;

/* fs.ReadDirFile, which is an FsFile with read_dir set. */
typedef FsFileVT FsReadDirFileVT;
typedef FsFile FsReadDirFile;

/* The file as an io.Reader, borrowing it. */
IoReader fs_file_as_io_reader(FsFile f);

/* ---------------------------------------------------------------------- FS */

typedef struct FsVT FsVT;

/* fs.FS. */
typedef struct Fs {
    const FsVT *vt;
    void *data;
} Fs;

/* The methods of an FS. open is the only one that has to be there. Each of the
 * others is the method of one of Go's extension interfaces, and a NULL slot is
 * an FS that does not have it. The package functions of the same names call
 * the slot when it is there and do the work with open when it is not, so call
 * those rather than the slots. */
struct FsVT {
    const Type *self_type;

    /* fs.FS.Open. A name that fs_valid_path refuses should fail with an
     * FsPathError wrapping fs_err_invalid or fs_err_not_exist. */
    FsFile (*open)(void *self, Alloc *a, Str name, Error *err);

    /* fs.ReadDirFS.ReadDir: the entries sorted by name. */
    Slice (*read_dir)(void *self, Alloc *a, Str name, Error *err);

    /* fs.ReadFileFS.ReadFile: the whole file, and no error at the end of it. */
    Slice (*read_file)(void *self, Alloc *a, Str name, Error *err);

    /* fs.StatFS.Stat. */
    FsFileInfo (*stat)(void *self, Alloc *a, Str name, Error *err);

    /* fs.SubFS.Sub. */
    Fs (*sub)(void *self, Alloc *a, Str dir, Error *err);

    /* fs.GlobFS.Glob: a Slice of Str. */
    Slice (*glob)(void *self, Alloc *a, Str pattern, Error *err);

    /* fs.ReadLinkFS.ReadLink and Lstat. Set both or neither. */
    Str (*read_link)(void *self, Alloc *a, Str name, Error *err);
    FsFileInfo (*lstat)(void *self, Alloc *a, Str name, Error *err);
};

extern const Type *const TYPE_FS;

/* Go's extension interfaces, which are Fs with the matching slot set. */
typedef FsVT FsReadDirFSVT;
typedef Fs FsReadDirFS;
typedef FsVT FsReadFileFSVT;
typedef Fs FsReadFileFS;
typedef FsVT FsStatFSVT;
typedef Fs FsStatFS;
typedef FsVT FsSubFSVT;
typedef Fs FsSubFS;
typedef FsVT FsGlobFSVT;
typedef Fs FsGlobFS;
typedef FsVT FsReadLinkFSVT;
typedef Fs FsReadLinkFS;

/* fs.ValidPath: whether name is a name an FS takes. It has to be UTF-8, and it
 * is "." or elements split by single slashes, none of them empty, "." or "..".
 * A backslash or a colon is an ordinary character. */
bool fs_valid_path(Str name);

/* fs.ReadFile: the whole of the named file. At the end of the file is not an
 * error. */
BURROW_OWNS(ret) Slice fs_read_file(Alloc *a, Fs fsys, Str name, Error *err);

/* fs.ReadDir: the named directory's entries, a Slice of FsDirEntry sorted by
 * name. What was read before an error comes back with it. A directory that
 * opens to a file with no read_dir fails with an FsPathError of op "readdir"
 * saying "not implemented". */
BURROW_OWNS(ret) Slice fs_read_dir(Alloc *a, Fs fsys, Str name, Error *err);

/* fs.Stat. */
BURROW_OWNS(ret) FsFileInfo fs_stat(Alloc *a, Fs fsys, Str name, Error *err);

/* fs.Lstat: a stat that does not follow a final symbolic link. An FS without
 * lstat gives what fs_stat gives. */
BURROW_OWNS(ret) FsFileInfo fs_lstat(Alloc *a, Fs fsys, Str name, Error *err);

/* fs.ReadLink: where a symbolic link points. An FS without read_link fails
 * with an FsPathError of op "readlink" wrapping fs_err_invalid. */
BURROW_OWNS(ret) Str fs_read_link(Alloc *a, Fs fsys, Str name, Error *err);

/* fs.Sub: the FS under dir, where "x" means dir/x in fsys. "." gives fsys
 * back. A dir that fs_valid_path refuses fails with an FsPathError of op
 * "sub". Without a sub slot the result wraps fsys and names in its errors are
 * made relative to dir again. The result borrows fsys. */
BURROW_OWNS(ret) Fs fs_sub(Alloc *a, Fs fsys, Str dir, Error *err);

/* fs.Glob: the names that match pattern, as a Slice of Str, with the syntax of
 * path_match. Errors reading directories are ignored, and the only error is
 * path_err_bad_pattern. A pattern that has no meta characters and names a
 * file that exists is given back as it is. More than 10000 separators in a
 * row is a bad pattern, which stops a pattern from recursing without end. */
BURROW_OWNS(ret) Slice fs_glob(Alloc *a, Fs fsys, Str pattern, Error *err);

/* ------------------------------------------------------------------ errors */

/* fs.PathError: op, the name it was on, and why it failed. The text is
 * op + " " + path + ": " + the text of err. errors_is sees through to err, and
 * errors_as with TYPE_FS_PATH_ERROR gets you the struct. */
typedef struct FsPathError {
    Str op;
    Str path;
    Error err;
} FsPathError;

extern const Type *const TYPE_FS_PATH_ERROR;

/* The text Go's Error method gives, built in a. */
BURROW_OWNS(ret) Str fs_path_error_error(const FsPathError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error fs_path_error_unwrap(const FsPathError *e);

/* PathError.Timeout: whether e->err has a Timeout method returning true,
 * found through the methods on the error's type. */
bool fs_path_error_timeout(const FsPathError *e);

/* An Error for an FsPathError you filled in yourself. op, path and the message
 * are copied into a and err is kept as it is. An allocation failure gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error fs_path_error_as_error(const FsPathError *e, Alloc *a);

/* The shorter way to make one. */
BURROW_OWNS(ret) Error fs_path_error_new(Alloc *a, Str op, Str path, Error err);

/* The errors an FS gives, which os gives too. Test for them with errors_is,
 * since they usually come wrapped in an FsPathError. */
extern const Error fs_err_invalid;    /* "invalid argument" */
extern const Error fs_err_permission; /* "permission denied" */
extern const Error fs_err_exist;      /* "file already exists" */
extern const Error fs_err_not_exist;  /* "file does not exist" */
extern const Error fs_err_closed;     /* "file already closed" */

/* -------------------------------------------------------------------- walk */

/* What a FsWalkDirFunc returns to skip the rest of a directory, or to stop the
 * walk. fs_walk_dir compares with identity, as Go does, so return these
 * themselves and not something wrapping them. */
extern const Error fs_skip_dir; /* "skip this directory" */
extern const Error fs_skip_all; /* "skip everything and stop the walk" */

/* fs.WalkDirFunc: called with each file or directory fs_walk_dir visits.
 *
 * path is root joined with the name, so it starts with root. d is the entry.
 * When err is set, either the root could not be stat'ed, in which case d is
 * nil, or a directory's entries could not be read, in which case the function
 * was already called for that directory once with no error and this second
 * call is about the read.
 *
 * Return fs_skip_dir to skip a directory's entries, or the rest of the
 * directory the file is in when d is a file, fs_skip_all to stop, any other
 * error to stop with that error, and no error to go on. */
BURROW_FUNC(FsWalkDirFunc, Error, Str path, FsDirEntry d, Error err);

/* fs.WalkDir: calls fn for root and everything under it, in lexical order,
 * directory before its entries. Symbolic links are not followed. Everything
 * it allocates, the paths included, comes from a. */
BURROW_STATIC(ret) Error fs_walk_dir(Alloc *a, Fs fsys, Str root, FsWalkDirFunc fn);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IO_FS_H */
