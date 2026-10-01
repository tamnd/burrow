/* archive/zip, reading and writing ZIP archives.
 *
 * A ZIP archive is a run of files, each a local header and its contents, and
 * then a central directory at the end that lists them all again with where
 * each one starts. A reader goes to the directory first, so it needs to read
 * at any offset, and a writer writes the directory when it is closed. ZIP64
 * extensions are read and written as needed, for files and archives of 4 GiB
 * and more and for more than 65535 files. Spanning an archive over several
 * disks is not supported.
 *
 * FileHeader has both 32 and 64 bit sizes. The 64 bit ones are always right,
 * and for a normal archive the two agree. For a file that needs ZIP64 the 32
 * bit ones are 0xffffffff.
 *
 *     BytesBuffer buf = BYTES_BUFFER(a);
 *     ZipWriter *zw = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
 *     IoWriter f = zip_writer_create(zw, BURROW_S("hello.txt"), &err);
 *     io_write_string(f, BURROW_S("hello, zip\n"), &err);
 *     err = zip_writer_close(zw);
 *
 *     BytesReader br;
 *     bytes_reader_reset(&br, bytes_buffer_bytes(&buf));
 *     ZipReader *zr = zip_new_reader(a, bytes_reader_as_io_reader_at(&br),
 *                                    bytes_buffer_len(&buf), &err);
 *     for (Int i = 0; i < zr->file.len; i++) {
 *         ZipFile *zf = ((ZipFile **)zr->file.p)[i];
 *         IoReadCloser rc = zip_file_open(zf, a, &err);
 *         ...read it, then rc.vt->closer.close(rc.data)...
 *     }
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package archive/zip */

#ifndef BURROW_ARCHIVE_ZIP_H
#define BURROW_ARCHIVE_ZIP_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* zip.ErrFormat: not a ZIP archive, or a broken one. */
extern const Error zip_err_format;

/* zip.ErrAlgorithm: a compression method nothing is registered for. */
extern const Error zip_err_algorithm;

/* zip.ErrChecksum: a file whose contents do not match its CRC-32. */
extern const Error zip_err_checksum;

/* zip.ErrInsecurePath: what zip_new_reader and zip_open_reader say about an
 * archive with a name that is not local, when GODEBUG has zipinsecurepath=0. */
extern const Error zip_err_insecure_path;

/* The compression methods built in. */
enum {
    ZIP_STORE = 0,   /* no compression */
    ZIP_DEFLATE = 8, /* DEFLATE */
};

/* zip.Compressor: a writer that compresses into w, with its memory from a.
 * Closing it finishes the stream, without closing w, and gives back what it
 * took from a. */
BURROW_FUNC(ZipCompressor, IoWriteCloser, Alloc *a, IoWriter w, Error *err);

/* zip.Decompressor: a reader of the decompressed form of r, with its memory
 * from a, or a nil IoReadCloser when a refuses. Closing it gives back what it
 * took from a. */
BURROW_FUNC(ZipDecompressor, IoReadCloser, Alloc *a, IoReader r);

/* zip.RegisterCompressor and zip.RegisterDecompressor: what every writer and
 * reader in the process uses for method, unless one has its own. A method
 * that has one already, including ZIP_STORE and ZIP_DEFLATE, panics. */
void zip_register_compressor(uint16_t method, ZipCompressor comp);
void zip_register_decompressor(uint16_t method, ZipDecompressor dcomp);

/* zip.FileHeader: one file in an archive.
 *
 * modified is the time to a second, and the time a reader gives has a fixed
 * zone guessed from the gap between the extended timestamp and the MS-DOS one,
 * or UTC when there is only an extended one. modified_time and modified_date
 * are the MS-DOS time and date, which have no time zone, and are deprecated
 * in favour of modified. compressed_size and uncompressed_size are deprecated
 * in favour of the 64 bit ones. */
typedef struct ZipFileHeader {
    Str name;    /* a relative path with forward slashes, a directory's ending in one */
    Str comment; /* under 64 KiB */

    /* The name and comment are not UTF-8. When false and they are not plain
     * ASCII, a writer sets the UTF-8 flag. */
    bool non_utf8;

    uint16_t creator_version;
    uint16_t reader_version;
    uint16_t flags;
    uint16_t method; /* ZIP_STORE, ZIP_DEFLATE, or one registered */

    Time modified;
    uint16_t modified_time;
    uint16_t modified_date;

    /* The CRC-32 of the contents. A writer works it out, unless the file was
     * made with zip_writer_create_raw. */
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint64_t compressed_size64;
    uint64_t uncompressed_size64;

    Slice extra; /* of Byte, the extra fields, under 64 KiB */

    uint32_t external_attrs; /* what it means depends on creator_version */
} ZipFileHeader;

extern const Type *const TYPE_ZIP_FILE_HEADER;

/* FileHeader.FileInfo: the header as an FsFileInfo, borrowing h. Its name is
 * the base name of h's, and its sys is BURROW_ANY(TYPE_ZIP_FILE_HEADER, h). */
BURROW_BORROWS(ret, h) FsFileInfo zip_file_header_file_info(ZipFileHeader *h);

/* FileHeader.Mode and SetMode: the permission and type bits, from the
 * external attributes, for headers made on Unix and MS-DOS. A name ending in a
 * slash is a directory. SetMode makes the header a Unix one. */
FsFileMode zip_file_header_mode(const ZipFileHeader *h);
void zip_file_header_set_mode(ZipFileHeader *h, FsFileMode mode);

/* FileHeader.ModTime and SetModTime, the MS-DOS time and date. Deprecated:
 * use modified. SetModTime sets modified too, in UTC. */
Time zip_file_header_mod_time(const ZipFileHeader *h);
void zip_file_header_set_mod_time(ZipFileHeader *h, Time t);

/* zip.FileInfoHeader: a header for the file fi describes, named for its base
 * name, with its size, mode and time. The method is left at ZIP_STORE, so set
 * it to ZIP_DEFLATE to compress. The header and its name come from a. */
BURROW_OWNS(ret) ZipFileHeader *zip_file_info_header(Alloc *a, FsFileInfo fi,
                                                     Error *err);

/* Gives back a header from zip_file_info_header and its name. NULL is fine. */
void zip_file_header_free(Alloc *a, ZipFileHeader *h);

/* ---------------------------------------------------------------- reading */

typedef struct ZipReader ZipReader;

/* zip.File: one file in an archive a reader has read. The header comes first
 * and the rest is the reader's. Its strings and extra live as long as the
 * reader does. */
typedef struct ZipFile {
    ZipFileHeader file_header;

    ZipReader *zip;        /* the reader it is in */
    IoReaderAt zipr;       /* where the contents are */
    int64_t header_offset; /* of its local header, from the start of zipr */
} ZipFile;

extern const Type *const TYPE_ZIP_FILE;

/* An entry in the sorted list zip_reader_open looks names up in. */
typedef struct ZipFileListEntry ZipFileListEntry;

/* A decompressor one reader has of its own. */
typedef struct ZipDecompressorEntry ZipDecompressorEntry;

/* zip.Reader: the files in an archive, read from its central directory.
 *
 * file is a Slice of ZipFile *, in the order the directory has them, and
 * comment is the archive's comment. The other fields are the reader's. */
struct ZipReader {
    Slice file;
    Str comment;

    Alloc *a;
    IoReaderAt r;
    int64_t base_offset; /* where the archive starts in r, past any prefix */
    ZipDecompressorEntry *decompressors;
    SyncOnce file_list_once;
    ZipFileListEntry *file_list;
    Int file_list_len;
    Error file_list_err;
};

extern const Type *const TYPE_ZIP_READER;

/* zip.NewReader: reads the central directory of the archive in r, which is
 * size bytes long. Everything the reader holds comes from a. A negative size,
 * a broken archive or a failed read gives NULL and the error.
 *
 * With zipinsecurepath=0 in GODEBUG, an archive with a name that is absolute,
 * goes up with "..", has a backslash or on Windows is a reserved name gives
 * the reader and zip_err_insecure_path. A caller that does not mind such names
 * can carry on with the reader. */
BURROW_OWNS(ret) ZipReader *zip_new_reader(Alloc *a, IoReaderAt r, int64_t size,
                                           Error *err);

/* Reader.RegisterDecompressor: what this reader uses for method, ahead of the
 * ones registered for the process. */
void zip_reader_register_decompressor(ZipReader *r, uint16_t method,
                                      ZipDecompressor dcomp);

/* Reader.Open: the file or directory called name, in the way of fs.FS.Open:
 * slash separated, with no leading slash or "..". Names in the archive are
 * cleaned up to match, and directories that only appear in names exist too.
 * The file comes from a and closing it gives it back. */
BURROW_OWNS(ret) FsFile zip_reader_open(ZipReader *r, Alloc *a, Str name, Error *err);

/* The reader as an Fs, borrowing r. */
Fs zip_reader_as_fs(ZipReader *r);

/* Gives back the reader, its files and all they hold. NULL is fine. */
void zip_reader_free(ZipReader *r);

/* zip.ReadCloser: a reader of a file it opened, which it closes. */
typedef struct ZipReadCloser {
    ZipReader reader;

    int64_t fd;
    Str path;
} ZipReadCloser;

extern const Type *const TYPE_ZIP_READ_CLOSER;

/* zip.OpenReader: opens the file at name and reads it as zip_new_reader does,
 * with zip_err_insecure_path the same way. */
BURROW_OWNS(ret) ZipReadCloser *zip_open_reader(Alloc *a, Str name, Error *err);

/* ReadCloser.Open and RegisterDecompressor, the reader's. */
BURROW_OWNS(ret) FsFile zip_read_closer_open(ZipReadCloser *rc, Alloc *a, Str name,
                                             Error *err);
void zip_read_closer_register_decompressor(ZipReadCloser *rc, uint16_t method,
                                           ZipDecompressor dcomp);

/* ReadCloser.Close: closes the file and gives back everything the reader
 * holds, rc too. */
BURROW_STATIC(ret) Error zip_read_closer_close(ZipReadCloser *rc);

/* File.Open: a reader of the file's contents, decompressed, which checks the
 * CRC-32 at the end and gives zip_err_checksum when it is wrong. A directory
 * reads nothing. It comes from a, and closing it gives it back. Several files
 * can be read at once. */
BURROW_OWNS(ret) IoReadCloser zip_file_open(ZipFile *f, Alloc *a, Error *err);

/* File.OpenRaw: the file's contents as they are stored, without
 * decompressing them. */
IoSectionReader zip_file_open_raw(ZipFile *f, Error *err);

/* File.DataOffset: where the stored contents start, from the start of the
 * archive. Most callers want zip_file_open. */
int64_t zip_file_data_offset(ZipFile *f, Error *err);

/* The header's methods, for a file. */
BURROW_BORROWS(ret, f) FsFileInfo zip_file_file_info(ZipFile *f);
FsFileMode zip_file_mode(const ZipFile *f);
void zip_file_set_mode(ZipFile *f, FsFileMode mode);
Time zip_file_mod_time(const ZipFile *f);
void zip_file_set_mod_time(ZipFile *f, Time t);

/* ---------------------------------------------------------------- writing */

/* zip.Writer: writes an archive, a file at a time, and the central directory
 * when closed. */
typedef struct ZipWriter ZipWriter;

extern const Type *const TYPE_ZIP_WRITER;

/* zip.NewWriter: a writer into w, buffered, with its memory from a. NULL when
 * a refuses. */
BURROW_OWNS(ret) ZipWriter *zip_new_writer(Alloc *a, IoWriter w);

/* Writer.SetOffset: where the archive starts in w, for one added to the end
 * of something else. Panics once anything has been written. */
void zip_writer_set_offset(ZipWriter *w, int64_t n);

/* Writer.Flush: writes out what the buffer holds. */
BURROW_STATIC(ret) Error zip_writer_flush(ZipWriter *w);

/* Writer.SetComment: the archive's comment, which close writes. The writer
 * keeps a copy. */
BURROW_STATIC(ret) Error zip_writer_set_comment(ZipWriter *w, Str comment);

/* Writer.RegisterCompressor: what this writer uses for method, ahead of the
 * ones registered for the process. */
void zip_writer_register_compressor(ZipWriter *w, uint16_t method, ZipCompressor comp);

/* Writer.Create: adds a file called name, compressed with ZIP_DEFLATE, and
 * returns where its contents go. The name has to be relative, with forward
 * slashes, and one ending in a slash is a directory, which takes no contents.
 * The IoWriter borrows w and works until the next create or close. */
IoWriter zip_writer_create(ZipWriter *w, Str name, Error *err);

/* Writer.CreateHeader: adds a file with the header fh. As in Go, the call sets
 * fh's flags, versions and MS-DOS time, and a directory's method and sizes,
 * and the writer keeps a copy of fh with the extended timestamp added to its
 * extra. The CRC-32 and sizes the writer works out go into that copy and not
 * into fh, so fh is free to go once this returns. */
IoWriter zip_writer_create_header(ZipWriter *w, ZipFileHeader *fh, Error *err);

/* Writer.CreateRaw: adds a file whose contents are written as they are to be
 * stored, already compressed. fh's CRC-32 and sizes have to be right, and the
 * writer keeps a copy. */
IoWriter zip_writer_create_raw(ZipWriter *w, ZipFileHeader *fh, Error *err);

/* Writer.Copy: adds f, from a reader, without decompressing it. */
BURROW_STATIC(ret) Error zip_writer_copy(ZipWriter *w, ZipFile *f);

/* Writer.AddFS: adds every file and directory in fsys, walking it from ".".
 * Anything that is neither is an error. */
BURROW_STATIC(ret) Error zip_writer_add_fs(ZipWriter *w, Fs fsys);

/* Writer.Close: finishes the last file and writes the central directory. It
 * does not close the underlying writer. A second close is an error. */
BURROW_STATIC(ret) Error zip_writer_close(ZipWriter *w);

/* Gives the writer and all it holds back to its allocator. NULL is fine. */
void zip_writer_free(ZipWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ARCHIVE_ZIP_H */
