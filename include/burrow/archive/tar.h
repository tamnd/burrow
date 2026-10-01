/* archive/tar, reading and writing tar archives.
 *
 * A tar archive is a run of files, each one a 512 byte header block and then
 * its contents padded out to a whole block. The formats that grew out of the
 * original one (USTAR, PAX and GNU) add fields and ways of storing names and
 * numbers too big for the old header, and a reader here reads all of them. A
 * writer picks the oldest format that can hold each header, or the one the
 * header asks for.
 *
 *     TarWriter *tw = tar_new_writer(a, out);
 *     TarHeader h = {.typeflag = TAR_TYPE_REG, .name = BURROW_S("hello.txt"),
 *                    .mode = 0644, .size = 6};
 *     err = tar_writer_write_header(tw, &h);
 *     tar_writer_write(tw, slice_from_str(a, BURROW_S("hello\n")), &err);
 *     err = tar_writer_close(tw);
 *
 *     TarReader *tr = tar_new_reader(a, in);
 *     for (;;) {
 *         TarHeader *h = tar_reader_next(tr, &err);
 *         if (errors_is(err, io_eof))
 *             break;
 *         ...read the file with tar_reader_read...
 *     }
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package archive/tar */

#ifndef BURROW_ARCHIVE_TAR_H
#define BURROW_ARCHIVE_TAR_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tar.ErrHeader: a header that cannot be parsed. */
extern const Error tar_err_header;

/* tar.ErrWriteTooLong: more written to a file than its header's size. */
extern const Error tar_err_write_too_long;

/* tar.ErrFieldTooLong: a name or a PAX header too long to store. */
extern const Error tar_err_field_too_long;

/* tar.ErrWriteAfterClose: a write to a closed Writer. */
extern const Error tar_err_write_after_close;

/* tar.ErrInsecurePath: what tar_reader_next says about a name that is not
 * local, when GODEBUG has tarinsecurepath=0. */
extern const Error tar_err_insecure_path;

/* The type flags, Go's TypeReg and the rest. */
enum {
    TAR_TYPE_REG = '0', /* a regular file */

    /* A regular file too, in old archives. Deprecated: a reader turns it into
     * TAR_TYPE_REG, or TAR_TYPE_DIR when the name ends in a slash. */
    TAR_TYPE_REG_A = '\0',

    TAR_TYPE_LINK = '1',    /* a hard link, to linkname */
    TAR_TYPE_SYMLINK = '2', /* a symbolic link, to linkname */
    TAR_TYPE_CHAR = '3',    /* a character device, devmajor and devminor */
    TAR_TYPE_BLOCK = '4',   /* a block device, devmajor and devminor */
    TAR_TYPE_DIR = '5',     /* a directory */
    TAR_TYPE_FIFO = '6',    /* a named pipe */

    TAR_TYPE_CONT = '7', /* reserved */

    /* PAX records for the next file. A reader folds them into that file's
     * header and a writer makes them when it has to. */
    TAR_TYPE_X_HEADER = 'x',

    /* PAX records for every file after it. A reader returns the header with
     * only name, typeflag, xattrs, pax_records and format set, and a writer
     * takes one with nothing else set. */
    TAR_TYPE_X_GLOBAL_HEADER = 'g',

    /* A GNU sparse file. */
    TAR_TYPE_GNU_SPARSE = 'S',

    /* GNU's long name and long link, for the next file. A reader folds them
     * in, and a writer makes them for a GNU header. */
    TAR_TYPE_GNU_LONG_NAME = 'L',
    TAR_TYPE_GNU_LONG_LINK = 'K',
};

/* tar.Format: which formats a header is in or can be written in, as a set of
 * bits. A reader says which one it found, as best it can tell, and a writer
 * takes one to insist on. */
typedef int TarFormat;

enum {
    TAR_FORMAT_UNKNOWN = 0, /* not known, or no format fits */

    /* USTAR, from POSIX.1-1988. Names up to 256 bytes and numbers that fit
     * in octal fields, ASCII only. */
    TAR_FORMAT_USTAR = 2,

    /* PAX, from POSIX.1-2001. USTAR with a header of records in front for
     * whatever USTAR cannot hold, including sub-second times and
     * extended attributes. */
    TAR_FORMAT_PAX = 4,

    /* GNU tar's own format. Base-256 numbers, long names and access and
     * change times. */
    TAR_FORMAT_GNU = 8,
};

/* Format.String: "USTAR", "(USTAR | PAX)", or "<unknown>" for none. */
BURROW_OWNS(ret) Str tar_format_string(TarFormat f, Alloc *a);

/* tar.Header: one file in an archive.
 *
 * xattrs and pax_records are maps from Str to Str, and NULL when there are
 * none. xattrs is deprecated in Go in favour of the SCHILY.xattr. records in
 * pax_records, and works the same here. */
typedef struct TarHeader {
    Byte typeflag;

    Str name;     /* the path in the archive */
    Str linkname; /* what a TAR_TYPE_LINK or TAR_TYPE_SYMLINK points at */

    int64_t size; /* the length of the contents in bytes */
    int64_t mode; /* the permission and mode bits */
    Int uid;
    Int gid;
    Str uname;
    Str gname;

    /* A writer rounds mod_time to a second and drops the other two unless a
     * format is set. access_time and change_time need PAX or GNU. */
    Time mod_time;
    Time access_time;
    Time change_time;

    int64_t devmajor;
    int64_t devminor;

    Map *xattrs;
    Map *pax_records;

    TarFormat format;
} TarHeader;

extern const Type *const TYPE_TAR_HEADER;

/* Gives back everything a header holds and then the header itself, when they
 * came from a as the headers from tar_reader_next and tar_file_info_header do.
 * NULL is fine. */
void tar_header_free(Alloc *a, TarHeader *h);

/* Header.FileInfo: the header as an FsFileInfo. Its name is the base name of
 * the header's, and its sys is BURROW_ANY(TYPE_TAR_HEADER, h). It borrows h
 * and allocates nothing. */
BURROW_BORROWS(ret, h) FsFileInfo tar_header_file_info(TarHeader *h);

/* tar.FileInfoNames: a FileInfo that knows its owner's names. A FileInfo
 * whose self_type has Uname and Gname methods of this shape is one:
 *
 *     Str my_uname(MyInfo *fi, Error *err);
 *
 *     #define MY_INFO_METHODS(M, T)                                         \
 *         M(T, Gname, my_gname, TAR_SIG_FILE_INFO_NAME)                     \
 *         M(T, Uname, my_uname, TAR_SIG_FILE_INFO_NAME)
 *
 * The names only have to last until tar_file_info_header copies them. */
#define TAR_SIG_FILE_INFO_NAME(IN, OUT) IN(0, IoErrorArg) OUT(Str)

/* tar.FileInfoNames as an interface value, an FsFileInfo with the two names
 * as well, for code that wants to call through one. tar_file_info_header does
 * not take it, as it finds the names through the methods above. */
typedef struct TarFileInfoNamesVT {
    FsFileInfoVT info;
    Str (*uname)(void *self, Error *err);
    Str (*gname)(void *self, Error *err);
} TarFileInfoNamesVT;

typedef struct TarFileInfoNames {
    const TarFileInfoNamesVT *vt;
    void *data;
} TarFileInfoNames;

/* tar.FileInfoHeader: a header for the file fi describes, with the name only
 * its base name. link is where a symbolic link points. When fi came from
 * tar_header_file_info the owner, the times, the attributes and a hard link
 * come from that header, and when it has the FileInfoNames methods the names
 * come from them. A socket and an unknown type are errors, as is a nil fi. The
 * header comes from a. */
BURROW_OWNS(ret) TarHeader *tar_file_info_header(Alloc *a, FsFileInfo fi, Str link,
                                                 Error *err);

/* ---------------------------------------------------------------- reading */

/* tar.Reader: reads the files in an archive, one after another. */
typedef struct TarReader TarReader;

extern const Type *const TYPE_TAR_READER;

/* tar.NewReader: a reader of r, with its memory from a. NULL when a refuses.
 * When r has a Seek method, the reader skips what it does not read with it. */
BURROW_OWNS(ret) TarReader *tar_new_reader(Alloc *a, IoReader r);

/* Reader.Next: moves to the next file and returns its header, or NULL with
 * io_eof at the end of the archive. The header and all it holds come from the
 * reader's allocator and are the caller's, for tar_header_free. Whatever was
 * left of the file before is skipped.
 *
 * With tarinsecurepath=0 in GODEBUG, a name that is absolute, goes up with
 * "..", or on Windows is a reserved name, gives the header and
 * tar_err_insecure_path, and the next call carries on. */
BURROW_OWNS(ret) TarHeader *tar_reader_next(TarReader *tr, Error *err);

/* Reader.Read: reads the current file, and gives io_eof at its end. A sparse
 * file reads with its holes as zeros. Any other error sticks. */
Int tar_reader_read(TarReader *tr, Slice b, Error *err);

/* The reader as an IoReader, borrowing tr. */
IoReader tar_reader_as_io_reader(TarReader *tr);

/* Gives the reader back to its allocator. NULL is fine. */
void tar_reader_free(TarReader *tr);

/* ---------------------------------------------------------------- writing */

/* tar.Writer: writes an archive, a header and then the contents for each
 * file, and the end of the archive on close. */
typedef struct TarWriter TarWriter;

extern const Type *const TYPE_TAR_WRITER;

/* tar.NewWriter: a writer into w, with its memory from a. NULL when a
 * refuses. */
BURROW_OWNS(ret) TarWriter *tar_new_writer(Alloc *a, IoWriter w);

/* Writer.Flush: pads the current file out to a block. The file has to have
 * been written in full. */
BURROW_STATIC(ret) Error tar_writer_flush(TarWriter *tw);

/* Writer.WriteHeader: flushes the file before and writes h, in the first of
 * USTAR, PAX and GNU that can hold it, or the one h's format asks for. The
 * writer only reads h during the call. A header no format can hold gives an
 * error saying why, and the writer carries on. */
BURROW_STATIC(ret) Error tar_writer_write_header(TarWriter *tw, const TarHeader *h);

/* Writer.Write: writes to the current file. More than its header's size
 * writes what fits and gives tar_err_write_too_long, which does not stick.
 * Any other error does. */
Int tar_writer_write(TarWriter *tw, Slice b, Error *err);

/* Writer.AddFS: adds every file and directory in fsys, walking it from ".",
 * with the directory names ending in a slash. Symbolic links go in as links,
 * and anything else that is not a regular file is an error. */
BURROW_STATIC(ret) Error tar_writer_add_fs(TarWriter *tw, Fs fsys);

/* Writer.Close: flushes and writes the two zero blocks that end the archive.
 * It does not close the underlying writer. A second close does nothing. */
BURROW_STATIC(ret) Error tar_writer_close(TarWriter *tw);

/* The writer as an IoWriter, borrowing tw. */
IoWriter tar_writer_as_io_writer(TarWriter *tw);

/* Gives the writer back to its allocator. NULL is fine. */
void tar_writer_free(TarWriter *tw);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ARCHIVE_TAR_H */
