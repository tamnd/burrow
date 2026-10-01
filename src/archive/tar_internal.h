/* What tar.c has inside, for tests/tar_test.c, which checks the pieces the way
 * Go's tests check the unexported functions they are named after.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ARCHIVE_TAR_INTERNAL_H
#define BURROW_SRC_ARCHIVE_TAR_INTERNAL_H

#include "burrow/archive/tar.h"

#include "burrow/core.h"
#include "burrow/io.h"
#include "burrow/map.h"

#include <stdint.h>

/* The formats Go does not export. */
enum {
    TAR_FORMAT_V7 = 1,
    TAR_FORMAT_STAR = 16,
    TAR_FORMAT_MAX = 32,
};

enum {
    TAR_BLOCK_SIZE = 512,
    TAR_NAME_SIZE = 100,
    TAR_PREFIX_SIZE = 155,
    TAR_MAX_SPECIAL_FILE_SIZE = 1 << 20,
    TAR_MAX_SPARSE_FILE_ENTRIES = 1 << 20,
};

/* Where each field is in a header block, as offset and length. */
enum {
    TAR_V7_NAME = 0, /* 100 */
    TAR_V7_MODE = 100,
    TAR_V7_UID = 108,
    TAR_V7_GID = 116,
    TAR_V7_SIZE = 124,     /* 12 */
    TAR_V7_MOD_TIME = 136, /* 12 */
    TAR_V7_CHKSUM = 148,
    TAR_V7_TYPE_FLAG = 156,
    TAR_V7_LINK_NAME = 157, /* 100 */
    TAR_USTAR_MAGIC = 257,  /* 6 */
    TAR_USTAR_VERSION = 263,
    TAR_USTAR_USER_NAME = 265, /* 32 */
    TAR_USTAR_GROUP_NAME = 297,
    TAR_USTAR_DEV_MAJOR = 329, /* 8 */
    TAR_USTAR_DEV_MINOR = 337,
    TAR_USTAR_PREFIX = 345, /* 155 */
    TAR_GNU_ACCESS_TIME = 345,
    TAR_GNU_CHANGE_TIME = 357,
    TAR_GNU_SPARSE = 386, /* 4 entries of 24 and the extended byte */
    TAR_GNU_REAL_SIZE = 483,
    TAR_STAR_PREFIX = 345, /* 131 */
    TAR_STAR_ACCESS_TIME = 476,
    TAR_STAR_CHANGE_TIME = 488,
    TAR_STAR_TRAILER = 508, /* 4 */
};

/* The errors Go does not export. */
extern const Error burrow__tar_err_miss_data;
extern const Error burrow__tar_err_unref_data;
extern const Error burrow__tar_err_write_hole;
extern const Error burrow__tar_err_sparse_too_long;

/* headerError's descriptor, for telling one from other errors. */
extern const Type *const burrow__TYPE_TAR_HEADER_ERROR;

/* sparseEntry, and a list of them that grows from an allocator. */
typedef struct TarSparseEntry {
    int64_t offset;
    int64_t length;
} TarSparseEntry;

typedef struct TarSparseList {
    TarSparseEntry *p;
    Int len;
    Int cap;
} TarSparseList;

void burrow__tar_sparse_list_free(Alloc *a, TarSparseList *l);

bool burrow__tar_validate_sparse_entries(const TarSparseEntry *sp, Int n, int64_t size);

/* alignSparseEntries and invertSparseEntries, in place, returning the new
 * length. The inversion can be one longer than what it started with, so sp
 * needs room for n + 1. */
Int burrow__tar_align_sparse_entries(TarSparseEntry *sp, Int n, int64_t size);
Int burrow__tar_invert_sparse_entries(TarSparseEntry *sp, Int n, int64_t size);

/* The parser and the formatter. Each sets *perr or *ferr to what Go's sets
 * its err field to and leaves it alone otherwise. */
Str burrow__tar_parse_string(const Byte *b, Int n);
int64_t burrow__tar_parse_numeric(const Byte *b, Int n, Error *perr);
int64_t burrow__tar_parse_octal(const Byte *b, Int n, Error *perr);
void burrow__tar_format_string(Byte *b, Int n, Str s, Error *ferr);
void burrow__tar_format_numeric(Byte *b, Int n, int64_t x, Error *ferr);
void burrow__tar_format_octal(Byte *b, Int n, int64_t x, Error *ferr);
bool burrow__tar_fits_in_base256(Int n, int64_t x);
bool burrow__tar_fits_in_octal(Int n, int64_t x);

Time burrow__tar_parse_pax_time(Str s, Error *err);
BURROW_OWNS(ret) Str burrow__tar_format_pax_time(Alloc *a, Time t);

/* parsePAXRecord: the key and the value, borrowing s, and what is left. */
Str burrow__tar_parse_pax_record(Str s, Str *k, Str *v, Error *err);
BURROW_OWNS(ret) Str burrow__tar_format_pax_record(Alloc *a, Str k, Str v, Error *err);
bool burrow__tar_valid_pax_record(Str k, Str v);

/* A map from Str to Str whose keys and values come from a, and the free that
 * gives them back. */
BURROW_OWNS(ret) Map *burrow__tar_str_map(Alloc *a);
bool burrow__tar_str_map_set(Alloc *a, Map *m, Str k, Str v);
void burrow__tar_str_map_free(Alloc *a, Map *m);

/* parsePAX, the records in the rest of r, from a. */
BURROW_OWNS(ret) Map *burrow__tar_parse_pax(Alloc *a, IoReader r, Error *err);

/* mergePAX. On success hdr keeps pax as its pax_records, and on failure pax is
 * still the caller's. */
Error burrow__tar_merge_pax(Alloc *a, TarHeader *hdr, Map *pax);

/* Header.allowedFormats. *pax gets the records a PAX header would need, from
 * a, when the result has TAR_FORMAT_PAX in it. */
TarFormat burrow__tar_allowed_formats(Alloc *a, const TarHeader *h, Map **pax,
                                      Error *err);

bool burrow__tar_split_ustar_path(Str name, Str *prefix, Str *suffix);

TarFormat burrow__tar_block_get_format(const Byte *blk);
void burrow__tar_block_set_format(Byte *blk, TarFormat f);

/* regFileReader and sparseFileReader. sp is the holes of a sparse file, and
 * NULL for a regular one. It is borrowed. */
typedef struct TarFileReader {
    IoReader r;
    int64_t nb;
    TarSparseEntry *sp;
    Int sp_len;
    int64_t pos;
} TarFileReader;

Int burrow__tar_file_reader_read(TarFileReader *fr, Slice b, Error *err);
int64_t burrow__tar_file_reader_write_to(TarFileReader *fr, Alloc *a, IoWriter w,
                                         Error *err);
int64_t burrow__tar_file_reader_logical_remaining(const TarFileReader *fr);
int64_t burrow__tar_file_reader_physical_remaining(const TarFileReader *fr);

/* regFileWriter and sparseFileWriter, the same way, with sp the data. */
typedef struct TarFileWriter {
    IoWriter w;
    int64_t nb;
    TarSparseEntry *sp;
    Int sp_len;
    int64_t pos;
} TarFileWriter;

Int burrow__tar_file_writer_write(TarFileWriter *fw, Slice b, Error *err);
int64_t burrow__tar_file_writer_read_from(TarFileWriter *fw, Alloc *a, IoReader r,
                                          Error *err);
int64_t burrow__tar_file_writer_logical_remaining(const TarFileWriter *fw);
int64_t burrow__tar_file_writer_physical_remaining(const TarFileWriter *fw);

struct TarReader {
    Alloc *a;
    IoReader r;
    int64_t pad;
    TarFileReader curr;
    TarSparseList holes; /* what curr.sp points into */
    Byte blk[TAR_BLOCK_SIZE];
    Error err;
};

struct TarWriter {
    Alloc *a;
    IoWriter w;
    int64_t pad;
    TarFileWriter curr;
    Byte blk[TAR_BLOCK_SIZE];
    Error err;
};

/* readOldGNUSparseMap, readGNUSparsePAXHeaders and the two map readers. The
 * PAX one gives false when the header is not a sparse file. */
Error burrow__tar_read_old_gnu_sparse_map(TarReader *tr, TarHeader *hdr, Byte *blk,
                                          TarSparseList *out);
bool burrow__tar_read_gnu_sparse_pax_headers(TarReader *tr, TarHeader *hdr,
                                             TarSparseList *out, Error *err);

/* Reader.writeTo and Writer.readFrom. */
int64_t burrow__tar_reader_write_to(TarReader *tr, IoWriter w, Error *err);
int64_t burrow__tar_writer_read_from(TarWriter *tw, IoReader r, Error *err);

/* filepath.IsLocal, for this system's kind of path. */
bool burrow__tar_is_local(Str path);

/* Sets GODEBUG as the package sees it, or goes back to the environment for
 * NULL. */
void burrow__tar_godebug_set(const char *value);

#endif /* BURROW_SRC_ARCHIVE_TAR_INTERNAL_H */
