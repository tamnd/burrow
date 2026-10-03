/* embed, files built into the program.
 *
 * Go has the //go:embed directive and the compiler fills the variable in. C has
 * no such thing before C23's #embed, and #embed gives one file as bytes, not a
 * tree of them with names. So here the declaration is a macro and a generator
 * writes the definition:
 *
 *     BURROW_EMBED_FILE(motd, "motd.txt");
 *     BURROW_EMBED_BYTES(logo, "logo.png");
 *     BURROW_EMBED_FS(assets, "static", "templates");
 *
 * Each line declares a variable, an extern const Str, a Slice of bytes and an
 * extern const EmbedFS. Running
 *
 *     burrow-gen embed server.c -o server_embed.c
 *
 * reads the macros in server.c, finds the files, and writes server_embed.c
 * with the definitions in it. Build that alongside server.c and the variables
 * are there, the same as in Go.
 *
 * An EmbedFS is an fs FS through embed_fs_as_fs, with Open, ReadDir and
 * ReadFile, so everything in io/fs works on it:
 *
 *     Slice page = fs_read_file(a, embed_fs_as_fs(&assets), BURROW_S("static/index.html"), &err);
 *
 * The zero EmbedFS is an empty FS, which still has a "." to open.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package embed */

#ifndef BURROW_EMBED_H
#define BURROW_EMBED_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One file or directory in an EmbedFS. A directory's name ends in a slash. The
 * hash is the first 16 bytes of the SHA-256 of the data, as in Go, where
 * nothing outside the package reads it either. */
typedef struct EmbedFile {
    Str name;
    Str data;
    Byte hash[16];
} EmbedFile;

/* embed.FS: the files sorted by directory and then by name in it, the order
 * the lookups need. The generator writes them in that order and an FS written
 * by hand has to keep to it. */
typedef struct EmbedFS {
    const EmbedFile *files;
    Int n;
} EmbedFS;

/* The declarations burrow-gen embed looks for. The patterns are for the
 * generator and the compiler never sees them.
 *
 * The patterns follow //go:embed. They are relative to the directory of the
 * source file, or to --dir. Each string can hold several patterns separated by
 * spaces, and a pattern with a space in it can be quoted, as in
 * "\"my file.txt\"". A pattern naming a directory takes the whole tree under it
 * except names starting with . or _, unless it starts with all:. A Str or a
 * Slice takes exactly one file. A pattern that matches nothing is an error from
 * the generator, as it is from go build.
 *
 * The Str is read only. The Slice can be written to, like Go's []byte, and is
 * shared by everything that reads it the way any other global is. The files in
 * an EmbedFS are read only, and read_file hands out copies.
 *
 * The generator writes the bytes as a C array, and also as #embed for a
 * compiler that has it, so a big file does not have to go through the
 * compiler as a list of numbers. --mode array or --mode embed picks one. */
#define BURROW_EMBED_FILE(sym, ...) extern const Str sym
#define BURROW_EMBED_BYTES(sym, ...) extern Slice sym
#define BURROW_EMBED_FS(sym, ...) extern const EmbedFS sym

/* The FS as an fs FS. The FS has to outlive what is returned, which a
 * generated one does since it is a global. */
Fs embed_fs_as_fs(const EmbedFS *fsys);

/* FS.Open: a file to read, seek and read at, or a directory to read. */
BURROW_OWNS(ret) FsFile embed_fs_open(const EmbedFS *fsys, Alloc *a, Str name,
                                      Error *err);

/* FS.ReadDir: the entries of a directory, a Slice of FsDirEntry sorted by
 * name. */
BURROW_OWNS(ret) Slice embed_fs_read_dir(const EmbedFS *fsys, Alloc *a, Str name,
                                         Error *err);

/* FS.ReadFile: a copy of a file's contents, made in a. */
BURROW_OWNS(ret) Slice embed_fs_read_file(const EmbedFS *fsys, Alloc *a, Str name,
                                          Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_EMBED_H */
