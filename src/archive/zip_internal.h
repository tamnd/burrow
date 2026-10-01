/* What zip.c has inside, for tests/zip_test.c, which reaches into the pieces
 * the way Go's tests reach into the unexported ones they are named after.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ARCHIVE_ZIP_INTERNAL_H
#define BURROW_SRC_ARCHIVE_ZIP_INTERNAL_H

#include "burrow/archive/zip.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/io.h"

#include <stdint.h>

enum {
    ZIP_FILE_HEADER_SIGNATURE = 0x04034b50,
    ZIP_DIRECTORY_HEADER_SIGNATURE = 0x02014b50,
    ZIP_DIRECTORY_END_SIGNATURE = 0x06054b50,
    ZIP_DIRECTORY64_LOC_SIGNATURE = 0x07064b50,
    ZIP_DIRECTORY64_END_SIGNATURE = 0x06064b50,
    ZIP_DATA_DESCRIPTOR_SIGNATURE = 0x08074b50,
    ZIP_FILE_HEADER_LEN = 30,
    ZIP_DIRECTORY_HEADER_LEN = 46,
    ZIP_DIRECTORY_END_LEN = 22,
    ZIP_DATA_DESCRIPTOR_LEN = 16,
    ZIP_DATA_DESCRIPTOR64_LEN = 24,
    ZIP_DIRECTORY64_LOC_LEN = 20,
    ZIP_DIRECTORY64_END_LEN = 56,

    ZIP_CREATOR_FAT = 0,
    ZIP_CREATOR_UNIX = 3,
    ZIP_CREATOR_NTFS = 11,
    ZIP_CREATOR_VFAT = 14,
    ZIP_CREATOR_MACOSX = 19,

    ZIP_VERSION20 = 20,
    ZIP_VERSION45 = 45,

    ZIP_EXTRA_ZIP64 = 0x0001,
    ZIP_EXTRA_NTFS = 0x000a,
    ZIP_EXTRA_UNIX = 0x000d,
    ZIP_EXTRA_EXT_TIME = 0x5455,
    ZIP_EXTRA_INFO_ZIP_UNIX = 0x5855,
};

#define ZIP_UINT16_MAX 0xffffU
#define ZIP_UINT32_MAX 0xffffffffU

/* fileListEntry. file is NULL for a directory that only appears in names. */
struct ZipFileListEntry {
    Str name;
    ZipFile *file;
    bool is_dir;
    bool is_dup;
    bool own_name; /* name came from the reader's allocator */
};

struct ZipDecompressorEntry {
    ZipDecompressorEntry *next;
    uint16_t method;
    ZipDecompressor d;
};

typedef struct ZipCompressorEntry {
    struct ZipCompressorEntry *next;
    uint16_t method;
    ZipCompressor c;
} ZipCompressorEntry;

/* countWriter. discard drops what is written and counts it all the same, for
 * closing a compressor nobody will read the output of. */
typedef struct ZipCountWriter {
    IoWriter w;
    int64_t count;
    bool discard;
} ZipCountWriter;

typedef struct ZipHeader ZipHeader;

/* fileWriter. It lives in its header, so that a writer the caller kept still
 * says it is closed after the next file has started. */
typedef struct ZipFileWriter {
    ZipHeader *header;
    IoWriter zipw;
    ZipCountWriter raw_count; /* into comp */
    IoWriteCloser comp;
    ZipCountWriter comp_count; /* into zipw */
    uint32_t crc32;
    bool fake_crc32; /* Go's tests swap in a hash that is always 0 */
    bool comp_open;
    bool closed;
} ZipFileWriter;

/* header: the writer's own copy of a FileHeader, with its offset. */
struct ZipHeader {
    ZipFileHeader fh;
    uint64_t offset;
    bool raw;
    bool has_writer;
    bool own_name; /* else it is empty or the start of an earlier header's */
    bool own_comment;
    Int extra_cap;
    ZipFileWriter fw;
};

struct ZipWriter {
    Alloc *a;
    BufioWriter *bw;
    ZipCountWriter cw;
    ZipHeader **dir;
    Int dir_len;
    Int dir_cap;
    ZipFileWriter *last;
    bool closed;
    ZipCompressorEntry *compressors;
    Str comment;

    void (*test_hook_close_size_offset)(void *env, uint64_t size, uint64_t offset);
    void *test_hook_env;
};

/* errLongName and errLongExtra, which zip_writer_create_header gives for a
 * name or extra of 64 KiB or more. */
extern const Error burrow__zip_err_long_name;
extern const Error burrow__zip_err_long_extra;

/* findSignatureInBlock and findDirectory64End: where the end record starts in
 * the n bytes at b, or -1, and where the ZIP64 end record is for an end record
 * at end_offset in r, or -1. */
Int burrow__zip_find_signature_in_block(const Byte *b, Int n);
int64_t burrow__zip_find_directory64_end(IoReaderAt r, int64_t end_offset, Error *err);

/* readDirectoryHeader on r, into a file that is thrown away after. */
Error burrow__zip_read_directory_header(Alloc *a, IoReader r);

/* Swaps in a CRC-32 that is always 0 for the file w writes, or for the file
 * rc reads, as Go's tests do with fakeHash32 to skip hashing gigabytes of
 * zeros. w has to be what zip_writer_create or zip_writer_create_header gave
 * for a file, and rc what zip_file_open gave for one. */
void burrow__zip_file_writer_fake_crc32(IoWriter w);
void burrow__zip_checksum_reader_fake_crc32(IoReadCloser rc);

/* timeToMsDosTime, msDosTimeToTime and timeZone. The location may come from
 * a, and is time_location_free's to give back. */
void burrow__zip_time_to_msdos_time(Time t, uint16_t *date, uint16_t *tm);
Time burrow__zip_msdos_time_to_time(uint16_t date, uint16_t tm);
TimeLocation *burrow__zip_time_zone(Alloc *a, Duration offset);

/* detectUTF8, toValidName and fileEntryCompare. */
void burrow__zip_detect_utf8(Str s, bool *valid, bool *require);
BURROW_OWNS(ret) Str burrow__zip_to_valid_name(Alloc *a, Str name);
int burrow__zip_file_entry_compare(Str x, Str y);

/* Sets GODEBUG as the package sees it, or goes back to the environment for
 * NULL. */
void burrow__zip_godebug_set(const char *value);

#endif /* BURROW_SRC_ARCHIVE_ZIP_INTERNAL_H */
