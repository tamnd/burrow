/* archive/zip, from struct.go, reader.go, register.go and writer.go.
 *
 * A reader finds the end of central directory record in the last 64 KiB or
 * so, follows it to the ZIP64 one when the counts or offsets are maxed out,
 * and reads the directory headers into files. Contents are read through a
 * section of the underlying ReaderAt, so several files can be read at once.
 *
 * A writer buffers what it writes, writes each file's local header and then
 * its contents, compressed through the method's compressor, and a data
 * descriptor with the CRC-32 and sizes after them. The central directory goes
 * out on close.
 *
 * Everything a reader holds comes from its allocator, and so does everything
 * a writer holds. The writer keeps its own copy of each header it is given.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "zip_internal.h"

#include "burrow/atomic.h"
#include "burrow/compress/flate.h"
#include "burrow/fmt.h"
#include "burrow/hash/crc32.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/path.h"
#include "burrow/runtime.h"
#include "burrow/sort.h"
#include "burrow/utf8.h"

#include <string.h>

BURROW_SENTINEL_ERROR(zip_err_format, "zip: not a valid zip file");
BURROW_SENTINEL_ERROR(zip_err_algorithm, "zip: unsupported compression algorithm");
BURROW_SENTINEL_ERROR(zip_err_checksum, "zip: checksum error");
BURROW_SENTINEL_ERROR(zip_err_insecure_path, "zip: insecure file path");
BURROW_SENTINEL_ERROR(burrow__zip_err_long_name, "zip: FileHeader.Name too long");
BURROW_SENTINEL_ERROR(burrow__zip_err_long_extra, "zip: FileHeader.Extra too long");
BURROW_SENTINEL_ERROR(zip_err_negative_size, "zip: size cannot be negative");
BURROW_SENTINEL_ERROR(zip_err_comment_length, "zip: invalid comment length");
BURROW_SENTINEL_ERROR(zip_err_closed_twice, "zip: writer closed twice");
BURROW_SENTINEL_ERROR(zip_err_long_comment, "zip: Writer.Comment too long");
BURROW_SENTINEL_ERROR(zip_err_non_regular, "zip: cannot add non-regular file");
BURROW_SENTINEL_ERROR(zip_err_write_dir, "zip: write to directory");
BURROW_SENTINEL_ERROR(zip_err_write_closed, "zip: write to closed file");
BURROW_SENTINEL_ERROR(zip_err_file_closed_twice, "zip: file closed twice");
BURROW_SENTINEL_ERROR(zip_err_is_dir, "is a directory");

/* What an empty string from the reader points at, so that no Str this package
 * hands out has a NULL pointer. */
static const Byte zip_empty[1] = {0};

static inline bool zip_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static inline Slice zip_bytes(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static inline uint16_t zip_le16(const Byte *b) {
    return (uint16_t)((unsigned)b[0] | (unsigned)b[1] << 8);
}

static inline uint32_t zip_le32(const Byte *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static inline uint64_t zip_le64(const Byte *b) {
    return (uint64_t)zip_le32(b) | (uint64_t)zip_le32(b + 4) << 32;
}

static inline Byte *zip_put16(Byte *b, uint16_t v) {
    b[0] = (Byte)v;
    b[1] = (Byte)(v >> 8);
    return b + 2;
}

static inline Byte *zip_put32(Byte *b, uint32_t v) {
    b[0] = (Byte)v;
    b[1] = (Byte)(v >> 8);
    b[2] = (Byte)(v >> 16);
    b[3] = (Byte)(v >> 24);
    return b + 4;
}

static inline Byte *zip_put64(Byte *b, uint64_t v) {
    zip_put32(b, (uint32_t)v);
    return zip_put32(b + 4, (uint32_t)(v >> 32));
}

static inline uint64_t zip_min64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static inline bool zip_has_slash_suffix(Str s) {
    return s.len > 0 && s.p[s.len - 1] == '/';
}

/* A copy of s in a. Empty is not an allocation. */
static Str zip_dup(Alloc *a, Str s, bool *ok) {
    if (s.len == 0)
        return (Str){zip_empty, 0};
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL) {
        *ok = false;
        return (Str){zip_empty, 0};
    }
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

static void zip_str_free(Alloc *a, Str s) {
    if (s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

#define ZIP_DESC(name, go_name, ctype, hash)                                           \
    static const Type name = {                                                         \
        {(const Byte *)(go_name), (Int)(sizeof(go_name) - 1)},                         \
        {(const Byte *)"archive/zip", 11},                                             \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        hash,                                                                          \
        NULL,                                                                          \
    }

ZIP_DESC(zip_file_header_desc, "FileHeader", ZipFileHeader, 0x7a666864U);
ZIP_DESC(zip_file_desc, "File", ZipFile, 0x7a66696cU);
ZIP_DESC(zip_reader_desc, "Reader", ZipReader, 0x7a726472U);
ZIP_DESC(zip_read_closer_desc, "ReadCloser", ZipReadCloser, 0x7a72636cU);
ZIP_DESC(zip_writer_desc, "Writer", ZipWriter, 0x7a777472U);
ZIP_DESC(zip_header_file_info_desc, "headerFileInfo", ZipFileHeader *, 0x7a686669U);
ZIP_DESC(zip_file_list_entry_desc, "fileListEntry", ZipFileListEntry, 0x7a666c65U);
ZIP_DESC(zip_count_writer_desc, "countWriter", ZipCountWriter, 0x7a637772U);
ZIP_DESC(zip_file_writer_desc, "fileWriter", ZipFileWriter, 0x7a667772U);
ZIP_DESC(zip_dir_writer_desc, "dirWriter", Byte, 0x7a647772U);
ZIP_DESC(zip_dir_reader_desc, "dirReader", Error, 0x7a647264U);
ZIP_DESC(zip_open_dir_desc, "openDir", ZipFileListEntry *, 0x7a6f6469U);
ZIP_DESC(zip_nop_closer_desc, "nopCloser", IoWriter, 0x7a6e6370U);
ZIP_DESC(zip_flate_writer_desc, "pooledFlateWriter", FlateWriter *, 0x7a706677U);
ZIP_DESC(zip_flate_reader_desc, "pooledFlateReader", IoReadCloser, 0x7a706672U);
ZIP_DESC(zip_store_reader_desc, "nopCloser", IoReader, 0x7a6e6372U);

const Type *const TYPE_ZIP_FILE_HEADER = &zip_file_header_desc;
const Type *const TYPE_ZIP_FILE = &zip_file_desc;
const Type *const TYPE_ZIP_READER = &zip_reader_desc;
const Type *const TYPE_ZIP_READ_CLOSER = &zip_read_closer_desc;
const Type *const TYPE_ZIP_WRITER = &zip_writer_desc;

/* ------------------------------------------------------------------ GODEBUG */

enum {
    ZIP_DEBUG_KNOWN = 1 << 0,
    ZIP_DEBUG_INSECURE_OFF = 1 << 1, /* zipinsecurepath=0 */
};

static uint32_t zip_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool zip_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

static uint32_t zip_debug_parse(const char *v) {
    uint32_t f = ZIP_DEBUG_KNOWN;
    Str s;
    if (v != NULL && zip_godebug(v, "zipinsecurepath", &s) && s.len == 1 &&
        s.p[0] == '0')
        f |= ZIP_DEBUG_INSECURE_OFF;
    burrow__atomic_store_relaxed_u32(&zip_debug_flags, f);
    return f;
}

static uint32_t zip_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&zip_debug_flags);
    if ((f & ZIP_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return zip_debug_parse(v);
}

void burrow__zip_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&zip_debug_flags, 0);
    else
        (void)zip_debug_parse(value);
}

/* --------------------------------------------------------------- the store */

/* nopCloser around a writer, and io.NopCloser around a reader, each from the
 * allocator it is given and giving itself back when closed. */
typedef struct ZipNopWriter {
    Alloc *a;
    IoWriter w;
} ZipNopWriter;

static Int zip_nop_write(void *self, Slice p, Error *err) {
    ZipNopWriter *n = (ZipNopWriter *)self;
    return n->w.vt->write(n->w.data, p, err);
}

static Error zip_nop_write_close(void *self) {
    ZipNopWriter *n = (ZipNopWriter *)self;
    mem_free(n->a, n, sizeof *n, _Alignof(ZipNopWriter));
    return BURROW_NO_ERROR;
}

static const IoWriteCloserVT zip_nop_writer_vt = {
    {&zip_nop_closer_desc, zip_nop_write},
    {&zip_nop_closer_desc, zip_nop_write_close},
};

static IoWriteCloser zip_store_compressor(void *env, Alloc *a, IoWriter w, Error *err) {
    (void)env;
    ZipNopWriter *n = (ZipNopWriter *)mem_alloc(a, sizeof *n, _Alignof(ZipNopWriter));
    if (n == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriteCloser){NULL, NULL};
    }
    n->a = a;
    n->w = w;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (IoWriteCloser){&zip_nop_writer_vt, n};
}

typedef struct ZipNopReader {
    Alloc *a;
    IoReader r;
} ZipNopReader;

static Int zip_nop_read(void *self, Slice p, Error *err) {
    ZipNopReader *n = (ZipNopReader *)self;
    return n->r.vt->read(n->r.data, p, err);
}

static Error zip_nop_read_close(void *self) {
    ZipNopReader *n = (ZipNopReader *)self;
    mem_free(n->a, n, sizeof *n, _Alignof(ZipNopReader));
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT zip_nop_reader_vt = {
    {&zip_store_reader_desc, zip_nop_read},
    {&zip_store_reader_desc, zip_nop_read_close},
};

static IoReadCloser zip_store_decompressor(void *env, Alloc *a, IoReader r) {
    (void)env;
    ZipNopReader *n = (ZipNopReader *)mem_alloc(a, sizeof *n, _Alignof(ZipNopReader));
    if (n == NULL)
        return (IoReadCloser){NULL, NULL};
    n->a = a;
    n->r = r;
    return (IoReadCloser){&zip_nop_reader_vt, n};
}

/* ------------------------------------------------------------- the deflate */

/* Go pools its flate writers and readers. These take one from the allocator
 * for each file and give it back on close. */
typedef struct ZipFlateWriter {
    Alloc *a;
    FlateWriter *fw;
} ZipFlateWriter;

static Int zip_flate_write(void *self, Slice p, Error *err) {
    return flate_writer_write(((ZipFlateWriter *)self)->fw, p, err);
}

static Error zip_flate_write_close(void *self) {
    ZipFlateWriter *z = (ZipFlateWriter *)self;
    Error e = flate_writer_close(z->fw);
    flate_writer_free(z->fw);
    mem_free(z->a, z, sizeof *z, _Alignof(ZipFlateWriter));
    return e;
}

static const IoWriteCloserVT zip_flate_writer_vt = {
    {&zip_flate_writer_desc, zip_flate_write},
    {&zip_flate_writer_desc, zip_flate_write_close},
};

static IoWriteCloser zip_deflate_compressor(void *env, Alloc *a, IoWriter w,
                                            Error *err) {
    (void)env;
    ZipFlateWriter *z =
        (ZipFlateWriter *)mem_alloc(a, sizeof *z, _Alignof(ZipFlateWriter));
    if (z == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriteCloser){NULL, NULL};
    }
    Error e = BURROW_NO_ERROR;
    z->a = a;
    z->fw = flate_new_writer(a, w, 5, &e);
    if (z->fw == NULL) {
        mem_free(a, z, sizeof *z, _Alignof(ZipFlateWriter));
        BURROW_OUT(err, e);
        return (IoWriteCloser){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (IoWriteCloser){&zip_flate_writer_vt, z};
}

typedef struct ZipFlateReader {
    Alloc *a;
    IoReadCloser fr;
} ZipFlateReader;

static Int zip_flate_read(void *self, Slice p, Error *err) {
    IoReadCloser fr = ((ZipFlateReader *)self)->fr;
    return fr.vt->reader.read(fr.data, p, err);
}

static Error zip_flate_read_close(void *self) {
    ZipFlateReader *z = (ZipFlateReader *)self;
    Error e = z->fr.vt->closer.close(z->fr.data);
    flate_reader_free(z->fr);
    mem_free(z->a, z, sizeof *z, _Alignof(ZipFlateReader));
    return e;
}

static const IoReadCloserVT zip_flate_reader_vt = {
    {&zip_flate_reader_desc, zip_flate_read},
    {&zip_flate_reader_desc, zip_flate_read_close},
};

static IoReadCloser zip_deflate_decompressor(void *env, Alloc *a, IoReader r) {
    (void)env;
    ZipFlateReader *z =
        (ZipFlateReader *)mem_alloc(a, sizeof *z, _Alignof(ZipFlateReader));
    if (z == NULL)
        return (IoReadCloser){NULL, NULL};
    z->a = a;
    z->fr = flate_new_reader(a, r);
    if (z->fr.vt == NULL) {
        mem_free(a, z, sizeof *z, _Alignof(ZipFlateReader));
        return (IoReadCloser){NULL, NULL};
    }
    return (IoReadCloser){&zip_flate_reader_vt, z};
}

/* ---------------------------------------------------------------- register */

static SyncMutex zip_registry_mu;
/* ZipCompressorEntry * and ZipDecompressorEntry *, as void * for the atomics.
 * Pushed under zip_registry_mu with a release store and read with an acquire
 * load, and an entry never changes once it is on a list. */
static void *zip_compressors;
static void *zip_decompressors;

static ZipCompressor zip_compressor(uint16_t method) {
    for (const ZipCompressorEntry *e =
             (const ZipCompressorEntry *)burrow__atomic_load_acquire_ptr(
                 &zip_compressors);
         e != NULL; e = e->next)
        if (e->method == method)
            return e->c;
    if (method == ZIP_STORE)
        return BURROW_FN(ZipCompressor, zip_store_compressor, NULL);
    if (method == ZIP_DEFLATE)
        return BURROW_FN(ZipCompressor, zip_deflate_compressor, NULL);
    return (ZipCompressor){NULL, NULL};
}

static ZipDecompressor zip_decompressor(uint16_t method) {
    for (const ZipDecompressorEntry *e =
             (const ZipDecompressorEntry *)burrow__atomic_load_acquire_ptr(
                 &zip_decompressors);
         e != NULL; e = e->next)
        if (e->method == method)
            return e->d;
    if (method == ZIP_STORE)
        return BURROW_FN(ZipDecompressor, zip_store_decompressor, NULL);
    if (method == ZIP_DEFLATE)
        return BURROW_FN(ZipDecompressor, zip_deflate_decompressor, NULL);
    return (ZipDecompressor){NULL, NULL};
}

void zip_register_compressor(uint16_t method, ZipCompressor comp) {
    ZipCompressorEntry *n = (ZipCompressorEntry *)mem_alloc(
        heap_allocator(), sizeof *n, _Alignof(ZipCompressorEntry));
    if (n == NULL)
        runtime_panic(BURROW_S("zip: RegisterCompressor: out of memory"));
    n->method = method;
    n->c = comp;
    sync_mutex_lock(&zip_registry_mu);
    if (!BURROW_FUNC_IS_NIL(zip_compressor(method))) {
        sync_mutex_unlock(&zip_registry_mu);
        mem_free(heap_allocator(), n, sizeof *n, _Alignof(ZipCompressorEntry));
        runtime_panic(BURROW_S("compressor already registered"));
    }
    n->next = (ZipCompressorEntry *)burrow__atomic_load_acquire_ptr(&zip_compressors);
    burrow__atomic_store_release_ptr(&zip_compressors, n);
    sync_mutex_unlock(&zip_registry_mu);
}

void zip_register_decompressor(uint16_t method, ZipDecompressor dcomp) {
    ZipDecompressorEntry *n = (ZipDecompressorEntry *)mem_alloc(
        heap_allocator(), sizeof *n, _Alignof(ZipDecompressorEntry));
    if (n == NULL)
        runtime_panic(BURROW_S("zip: RegisterDecompressor: out of memory"));
    n->method = method;
    n->d = dcomp;
    sync_mutex_lock(&zip_registry_mu);
    if (!BURROW_FUNC_IS_NIL(zip_decompressor(method))) {
        sync_mutex_unlock(&zip_registry_mu);
        mem_free(heap_allocator(), n, sizeof *n, _Alignof(ZipDecompressorEntry));
        runtime_panic(BURROW_S("decompressor already registered"));
    }
    n->next =
        (ZipDecompressorEntry *)burrow__atomic_load_acquire_ptr(&zip_decompressors);
    burrow__atomic_store_release_ptr(&zip_decompressors, n);
    sync_mutex_unlock(&zip_registry_mu);
}

/* -------------------------------------------------------------- the header */

void burrow__zip_time_to_msdos_time(Time t, uint16_t *date, uint16_t *tm) {
    Int d = time_day(t) + (Int)time_month(t) * 32 + (time_year(t) - 1980) * 512;
    Int s = time_second(t) / 2 + time_minute(t) * 32 + time_hour(t) * 2048;
    *date = (uint16_t)(uint64_t)d;
    *tm = (uint16_t)(uint64_t)s;
}

Time burrow__zip_msdos_time_to_time(uint16_t date, uint16_t tm) {
    return time_date((Int)(date >> 9) + 1980, (TimeMonth)(date >> 5 & 0xf),
                     (Int)(date & 0x1f), (Int)(tm >> 11), (Int)(tm >> 5 & 0x3f),
                     (Int)(tm & 0x1f) * 2, 0, time_utc_loc);
}

TimeLocation *burrow__zip_time_zone(Alloc *a, Duration offset) {
    offset = duration_round(offset, 15 * TIME_MINUTE);
    if (offset < -12 * TIME_HOUR || 14 * TIME_HOUR < offset)
        offset = 0;
    return time_fixed_zone(a, BURROW_S(""), (Int)(offset / TIME_SECOND));
}

Time zip_file_header_mod_time(const ZipFileHeader *h) {
    return burrow__zip_msdos_time_to_time(h->modified_date, h->modified_time);
}

void zip_file_header_set_mod_time(ZipFileHeader *h, Time t) {
    t = time_utc(t);
    h->modified = t;
    burrow__zip_time_to_msdos_time(t, &h->modified_date, &h->modified_time);
}

enum {
    ZIP_S_IFMT = 0xf000,
    ZIP_S_IFSOCK = 0xc000,
    ZIP_S_IFLNK = 0xa000,
    ZIP_S_IFREG = 0x8000,
    ZIP_S_IFBLK = 0x6000,
    ZIP_S_IFDIR = 0x4000,
    ZIP_S_IFCHR = 0x2000,
    ZIP_S_IFIFO = 0x1000,
    ZIP_S_ISUID = 0x800,
    ZIP_S_ISGID = 0x400,
    ZIP_S_ISVTX = 0x200,

    ZIP_MSDOS_DIR = 0x10,
    ZIP_MSDOS_READ_ONLY = 0x01,
};

static FsFileMode zip_msdos_mode_to_file_mode(uint32_t m) {
    FsFileMode mode;
    if ((m & ZIP_MSDOS_DIR) != 0)
        mode = FS_MODE_DIR | 0777;
    else
        mode = 0666;
    if ((m & ZIP_MSDOS_READ_ONLY) != 0)
        mode &= ~(FsFileMode)0222;
    return mode;
}

static uint32_t zip_file_mode_to_unix_mode(FsFileMode mode) {
    uint32_t m;
    FsFileMode t = mode & FS_MODE_TYPE;
    if (t == FS_MODE_DIR)
        m = ZIP_S_IFDIR;
    else if (t == FS_MODE_SYMLINK)
        m = ZIP_S_IFLNK;
    else if (t == FS_MODE_NAMED_PIPE)
        m = ZIP_S_IFIFO;
    else if (t == FS_MODE_SOCKET)
        m = ZIP_S_IFSOCK;
    else if (t == FS_MODE_DEVICE)
        m = ZIP_S_IFBLK;
    else if (t == (FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE))
        m = ZIP_S_IFCHR;
    else
        m = ZIP_S_IFREG;
    if ((mode & FS_MODE_SETUID) != 0)
        m |= ZIP_S_ISUID;
    if ((mode & FS_MODE_SETGID) != 0)
        m |= ZIP_S_ISGID;
    if ((mode & FS_MODE_STICKY) != 0)
        m |= ZIP_S_ISVTX;
    return m | (uint32_t)(mode & 0777);
}

static FsFileMode zip_unix_mode_to_file_mode(uint32_t m) {
    FsFileMode mode = (FsFileMode)(m & 0777);
    switch (m & ZIP_S_IFMT) {
    case ZIP_S_IFBLK:
        mode |= FS_MODE_DEVICE;
        break;
    case ZIP_S_IFCHR:
        mode |= FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE;
        break;
    case ZIP_S_IFDIR:
        mode |= FS_MODE_DIR;
        break;
    case ZIP_S_IFIFO:
        mode |= FS_MODE_NAMED_PIPE;
        break;
    case ZIP_S_IFLNK:
        mode |= FS_MODE_SYMLINK;
        break;
    case ZIP_S_IFSOCK:
        mode |= FS_MODE_SOCKET;
        break;
    default:
        break;
    }
    if ((m & ZIP_S_ISGID) != 0)
        mode |= FS_MODE_SETGID;
    if ((m & ZIP_S_ISUID) != 0)
        mode |= FS_MODE_SETUID;
    if ((m & ZIP_S_ISVTX) != 0)
        mode |= FS_MODE_STICKY;
    return mode;
}

FsFileMode zip_file_header_mode(const ZipFileHeader *h) {
    FsFileMode mode = 0;
    switch (h->creator_version >> 8) {
    case ZIP_CREATOR_UNIX:
    case ZIP_CREATOR_MACOSX:
        mode = zip_unix_mode_to_file_mode(h->external_attrs >> 16);
        break;
    case ZIP_CREATOR_NTFS:
    case ZIP_CREATOR_VFAT:
    case ZIP_CREATOR_FAT:
        mode = zip_msdos_mode_to_file_mode(h->external_attrs);
        break;
    default:
        break;
    }
    if (zip_has_slash_suffix(h->name))
        mode |= FS_MODE_DIR;
    return mode;
}

void zip_file_header_set_mode(ZipFileHeader *h, FsFileMode mode) {
    h->creator_version =
        (uint16_t)((h->creator_version & 0xff) | ZIP_CREATOR_UNIX << 8);
    h->external_attrs = zip_file_mode_to_unix_mode(mode) << 16;
    if ((mode & FS_MODE_DIR) != 0)
        h->external_attrs |= ZIP_MSDOS_DIR;
    if ((mode & 0200) == 0)
        h->external_attrs |= ZIP_MSDOS_READ_ONLY;
}

static bool zip_has_data_descriptor(const ZipFileHeader *h) {
    return (h->flags & 0x8) != 0;
}

/* headerFileInfo, which is the header itself behind the vtables. */
static Str zip_hfi_name(void *self) {
    return path_base(((const ZipFileHeader *)self)->name);
}

static int64_t zip_hfi_size(void *self) {
    const ZipFileHeader *h = (const ZipFileHeader *)self;
    if (h->uncompressed_size64 > 0)
        return (int64_t)h->uncompressed_size64;
    return (int64_t)h->uncompressed_size;
}

static FsFileMode zip_hfi_mode(void *self) {
    return zip_file_header_mode((const ZipFileHeader *)self);
}

static bool zip_hfi_is_dir(void *self) {
    return fs_file_mode_is_dir(zip_hfi_mode(self));
}

static Time zip_hfi_mod_time(void *self) {
    const ZipFileHeader *h = (const ZipFileHeader *)self;
    if (time_is_zero(h->modified))
        return zip_file_header_mod_time(h);
    return time_utc(h->modified);
}

static Any zip_hfi_sys(void *self) {
    return BURROW_ANY(TYPE_ZIP_FILE_HEADER, self);
}

static FsFileMode zip_hfi_type(void *self) {
    return fs_file_mode_type(zip_hfi_mode(self));
}

static const FsFileInfoVT zip_hfi_vt = {
    &zip_header_file_info_desc, zip_hfi_name,   zip_hfi_size, zip_hfi_mode,
    zip_hfi_mod_time,           zip_hfi_is_dir, zip_hfi_sys,
};

static FsFileInfo zip_hfi_info(void *self, Alloc *a, Error *err) {
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (FsFileInfo){&zip_hfi_vt, self};
}

static const FsDirEntryVT zip_hfi_dir_entry_vt = {
    &zip_header_file_info_desc,
    zip_hfi_name,
    zip_hfi_is_dir,
    zip_hfi_type,
    zip_hfi_info,
};

FsFileInfo zip_file_header_file_info(ZipFileHeader *h) {
    return (FsFileInfo){&zip_hfi_vt, h};
}

ZipFileHeader *zip_file_info_header(Alloc *a, FsFileInfo fi, Error *err) {
    ZipFileHeader *fh =
        (ZipFileHeader *)mem_alloc(a, sizeof *fh, _Alignof(ZipFileHeader));
    if (fh == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    bool ok = true;
    int64_t size = fi.vt->size(fi.data);
    fh->name = zip_dup(a, fi.vt->name(fi.data), &ok);
    if (!ok) {
        mem_free(a, fh, sizeof *fh, _Alignof(ZipFileHeader));
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    fh->comment = (Str){zip_empty, 0};
    fh->extra = slice_nil(TYPE_BYTE);
    fh->uncompressed_size64 = (uint64_t)size;
    zip_file_header_set_mod_time(fh, fi.vt->mod_time(fi.data));
    zip_file_header_set_mode(fh, fi.vt->mode(fi.data));
    fh->uncompressed_size =
        (uint32_t)zip_min64(fh->uncompressed_size64, ZIP_UINT32_MAX);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return fh;
}

void zip_file_header_free(Alloc *a, ZipFileHeader *h) {
    if (h == NULL)
        return;
    zip_str_free(a, h->name);
    mem_free(a, h, sizeof *h, _Alignof(ZipFileHeader));
}

/* The header's methods, promoted to File as Go's embedding does. */
FsFileInfo zip_file_file_info(ZipFile *f) {
    return zip_file_header_file_info(&f->file_header);
}

FsFileMode zip_file_mode(const ZipFile *f) {
    return zip_file_header_mode(&f->file_header);
}

void zip_file_set_mode(ZipFile *f, FsFileMode mode) {
    zip_file_header_set_mode(&f->file_header, mode);
}

Time zip_file_mod_time(const ZipFile *f) {
    return zip_file_header_mod_time(&f->file_header);
}

void zip_file_set_mod_time(ZipFile *f, Time t) {
    zip_file_header_set_mod_time(&f->file_header, t);
}

/* ------------------------------------------------------------------- UTF-8 */

void burrow__zip_detect_utf8(Str s, bool *valid, bool *require) {
    *require = false;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        i += size;
        if (r < 0x20 || r > 0x7d || r == 0x5c) {
            if (!utf8_valid_rune(r) || (r == UTF8_RUNE_ERROR && size == 1)) {
                *valid = false;
                *require = false;
                return;
            }
            *require = true;
        }
    }
    *valid = true;
}

/* ----------------------------------------------------------------- reading */

/* What reading a ZipFile took from the reader's allocator, after the struct:
 * the name, extra and comment in one block, and the location of modified when
 * one had to be made. */
typedef struct ZipFileMem {
    ZipFile f;
    Byte *buf;
    Int buf_len;
    TimeLocation *loc;
} ZipFileMem;

static void zip_file_mem_free(Alloc *a, ZipFileMem *m) {
    if (m == NULL)
        return;
    if (m->buf_len > 0)
        mem_free(a, m->buf, (size_t)m->buf_len, 1);
    time_location_free(m->loc);
    mem_free(a, m, sizeof *m, _Alignof(ZipFileMem));
}

/* readDirectoryHeader, into m from r. Gives io_err_unexpected_eof for a short
 * header and zip_err_format for one without the signature. */
static Error zip_read_directory_header(Alloc *a, ZipFileMem *m, IoReader r) {
    ZipFile *f = &m->f;
    ZipFileHeader *h = &f->file_header;
    /* Zeroed only for gcc, which cannot see that io_read_full writes it. */
    Byte buf[ZIP_DIRECTORY_HEADER_LEN] = {0};
    Error e = BURROW_NO_ERROR;
    io_read_full(r, zip_bytes(buf, (Int)sizeof buf), &e);
    if (BURROW_FAILED(e))
        return e;
    if (zip_le32(buf) != ZIP_DIRECTORY_HEADER_SIGNATURE)
        return zip_err_format;
    h->creator_version = zip_le16(buf + 4);
    h->reader_version = zip_le16(buf + 6);
    h->flags = zip_le16(buf + 8);
    h->method = zip_le16(buf + 10);
    h->modified_time = zip_le16(buf + 12);
    h->modified_date = zip_le16(buf + 14);
    h->crc32 = zip_le32(buf + 16);
    h->compressed_size = zip_le32(buf + 20);
    h->uncompressed_size = zip_le32(buf + 24);
    h->compressed_size64 = h->compressed_size;
    h->uncompressed_size64 = h->uncompressed_size;
    Int filename_len = zip_le16(buf + 28);
    Int extra_len = zip_le16(buf + 30);
    Int comment_len = zip_le16(buf + 32);
    h->external_attrs = zip_le32(buf + 38);
    f->header_offset = (int64_t)zip_le32(buf + 42);
    Int n = filename_len + extra_len + comment_len;
    Byte *d = NULL;
    if (n > 0) {
        d = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
        if (d == NULL)
            return burrow_err_out_of_memory;
        m->buf = d;
        m->buf_len = n;
        io_read_full(r, zip_bytes(d, n), &e);
        if (BURROW_FAILED(e))
            return e;
    }
    const Byte *base = d != NULL ? d : zip_empty;
    h->name = (Str){base, filename_len};
    h->extra = extra_len > 0 ? zip_bytes(base + filename_len, extra_len)
                             : slice_nil(TYPE_BYTE);
    h->comment = (Str){base + filename_len + extra_len, comment_len};

    /* Determine the character encoding. */
    bool valid1, require1, valid2, require2;
    burrow__zip_detect_utf8(h->name, &valid1, &require1);
    burrow__zip_detect_utf8(h->comment, &valid2, &require2);
    if (!valid1 || !valid2)
        h->non_utf8 = true;
    else if (!require1 && !require2)
        h->non_utf8 = false;
    else
        h->non_utf8 = (h->flags & 0x800) == 0;

    /* Best effort to find what we need. Other zip authors might not even
     * follow the basic format, and we ignore the extra content then. */
    Time modified = {0};
    const Byte *x = base + filename_len;
    Int xl = extra_len;
    while (xl >= 4) {
        uint16_t tag = zip_le16(x);
        Int size = zip_le16(x + 2);
        x += 4;
        xl -= 4;
        if (xl < size)
            break;
        const Byte *fb = x;
        Int fl = size;
        x += size;
        xl -= size;
        switch (tag) {
        case ZIP_EXTRA_ZIP64:
            /* Only consulted when the sizes read earlier are maxed out. See
             * go.dev/issue/13367 and go.dev/issue/31692. */
            if (h->uncompressed_size == ZIP_UINT32_MAX) {
                if (fl < 8)
                    return zip_err_format;
                h->uncompressed_size64 = zip_le64(fb);
                fb += 8;
                fl -= 8;
            }
            if (h->compressed_size == ZIP_UINT32_MAX) {
                if (fl < 8)
                    return zip_err_format;
                h->compressed_size64 = zip_le64(fb);
                fb += 8;
                fl -= 8;
            }
            if (f->header_offset == (int64_t)ZIP_UINT32_MAX) {
                if (fl < 8)
                    return zip_err_format;
                f->header_offset = (int64_t)zip_le64(fb);
            }
            break;
        case ZIP_EXTRA_NTFS:
            if (fl < 4)
                break;
            fb += 4; /* reserved */
            fl -= 4;
            while (fl >= 4) {
                uint16_t attr_tag = zip_le16(fb);
                Int attr_size = zip_le16(fb + 2);
                fb += 4;
                fl -= 4;
                if (fl < attr_size)
                    break;
                const Byte *ab = fb;
                fb += attr_size;
                fl -= attr_size;
                if (attr_tag != 1 || attr_size != 24)
                    continue;
                /* ModTime in 100ns ticks since 1601. */
                int64_t ts = (int64_t)zip_le64(ab);
                int64_t secs = ts / 10000000;
                int64_t nsecs = 100 * (ts % 10000000);
                modified = time_from_unix(-11644473600 + secs, nsecs);
            }
            break;
        case ZIP_EXTRA_UNIX:
        case ZIP_EXTRA_INFO_ZIP_UNIX:
            if (fl < 8)
                break;
            modified = time_from_unix((int64_t)zip_le32(fb + 4), 0);
            break;
        case ZIP_EXTRA_EXT_TIME:
            if (fl < 5 || (fb[0] & 1) == 0)
                break;
            modified = time_from_unix((int64_t)zip_le32(fb + 1), 0);
            break;
        default:
            break;
        }
    }

    Time msdos = burrow__zip_msdos_time_to_time(h->modified_date, h->modified_time);
    h->modified = msdos;
    if (!time_is_zero(modified)) {
        h->modified = time_utc(modified);
        /* A non-UTC zone is always used, even for an offset of zero, so that
         * a location of UTC says there was only an extended timestamp. */
        if (h->modified_time != 0 || h->modified_date != 0) {
            TimeLocation *loc = burrow__zip_time_zone(a, time_sub(msdos, modified));
            if (loc == NULL)
                return burrow_err_out_of_memory;
            m->loc = loc;
            h->modified = time_in(modified, loc);
        }
    }
    return BURROW_NO_ERROR;
}

Error burrow__zip_read_directory_header(Alloc *a, IoReader r) {
    ZipFileMem tmp;
    memset(&tmp, 0, sizeof tmp);
    Error e = zip_read_directory_header(a, &tmp, r);
    if (tmp.buf_len > 0)
        mem_free(a, tmp.buf, (size_t)tmp.buf_len, 1);
    time_location_free(tmp.loc);
    return e;
}

typedef struct ZipDirectoryEnd {
    uint64_t directory_records;
    uint64_t directory_size;
    uint64_t directory_offset;
    Str comment; /* borrowed from the buffer it was read from */
} ZipDirectoryEnd;

static Int zip_find_signature_in_block(const Byte *b, Int n) {
    for (Int i = n - ZIP_DIRECTORY_END_LEN; i >= 0; i--) {
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            /* n is the length of the comment */
            Int cl = zip_le16(b + i + ZIP_DIRECTORY_END_LEN - 2);
            if (cl + ZIP_DIRECTORY_END_LEN + i > n)
                /* A truncated comment. Some parsers, such as Info-ZIP, ignore
                 * it rather than treat it as a hard error. */
                return -1;
            return i;
        }
    }
    return -1;
}

static Int zip_read_at(IoReaderAt r, Byte *p, Int n, int64_t off, Error *err) {
    *err = BURROW_NO_ERROR;
    return r.vt->read_at(r.data, zip_bytes(p, n), off, err);
}

/* findDirectory64End: the offset of the ZIP64 end record, or -1. */
static int64_t zip_find_directory64_end(IoReaderAt r, int64_t end_offset, Error *err) {
    *err = BURROW_NO_ERROR;
    int64_t loc_offset = end_offset - ZIP_DIRECTORY64_LOC_LEN;
    if (loc_offset < 0)
        return -1;
    Byte b[ZIP_DIRECTORY64_LOC_LEN];
    Error e;
    zip_read_at(r, b, (Int)sizeof b, loc_offset, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return -1;
    }
    if (zip_le32(b) != ZIP_DIRECTORY64_LOC_SIGNATURE)
        return -1;
    if (zip_le32(b + 4) != 0)
        return -1;
    uint64_t p = zip_le64(b + 8);
    if (zip_le32(b + 16) != 1)
        return -1;
    return (int64_t)p;
}

Int burrow__zip_find_signature_in_block(const Byte *b, Int n) {
    return zip_find_signature_in_block(b, n);
}

int64_t burrow__zip_find_directory64_end(IoReaderAt r, int64_t end_offset, Error *err) {
    return zip_find_directory64_end(r, end_offset, err);
}

static Error zip_read_directory64_end(IoReaderAt r, int64_t offset,
                                      ZipDirectoryEnd *d) {
    Byte b[ZIP_DIRECTORY64_END_LEN];
    Error e;
    zip_read_at(r, b, (Int)sizeof b, offset, &e);
    if (BURROW_FAILED(e))
        return e;
    if (zip_le32(b) != ZIP_DIRECTORY64_END_SIGNATURE)
        return zip_err_format;
    d->directory_records = zip_le64(b + 32);
    d->directory_size = zip_le64(b + 40);
    d->directory_offset = zip_le64(b + 48);
    return BURROW_NO_ERROR;
}

/* readDirectoryEnd. The comment is copied into a. */
static Error zip_read_directory_end(Alloc *a, IoReaderAt r, int64_t size,
                                    ZipDirectoryEnd *d, int64_t *base_offset,
                                    Str *comment) {
    static const int64_t lens[2] = {1024, (int64_t)65 * 1024};
    Byte *buf = NULL;
    Int buf_cap = 0;
    Int p = -1;
    Int blen = 0;
    int64_t end_offset = 0;
    Error e = BURROW_NO_ERROR;
    for (int i = 0; i < 2; i++) {
        int64_t bl = lens[i] > size ? size : lens[i];
        blen = (Int)bl;
        if (blen > buf_cap) {
            Byte *nb = (Byte *)mem_realloc(a, buf, (size_t)buf_cap, (size_t)blen, 1);
            if (nb == NULL) {
                mem_free(a, buf, (size_t)buf_cap, 1);
                return burrow_err_out_of_memory;
            }
            buf = nb;
            buf_cap = blen;
        }
        zip_read_at(r, buf, blen, size - bl, &e);
        if (BURROW_FAILED(e) && !zip_same(e, io_eof))
            goto fail;
        e = BURROW_NO_ERROR;
        p = blen >= ZIP_DIRECTORY_END_LEN ? zip_find_signature_in_block(buf, blen) : -1;
        if (p >= 0) {
            end_offset = size - bl + p;
            break;
        }
        if (i == 1 || bl == size) {
            e = zip_err_format;
            goto fail;
        }
    }

    {
        const Byte *b = buf + p + 4;
        Int left = blen - p - 4;
        d->directory_records = zip_le16(b + 6);
        d->directory_size = zip_le32(b + 8);
        d->directory_offset = zip_le32(b + 12);
        Int l = zip_le16(b + 16);
        b += 18;
        left -= 18;
        if (l > left) {
            e = zip_err_comment_length;
            goto fail;
        }
        bool ok = true;
        *comment = zip_dup(a, str_from_bytes(b, l), &ok);
        if (!ok) {
            e = burrow_err_out_of_memory;
            goto fail;
        }
    }
    mem_free(a, buf, (size_t)buf_cap, 1);
    buf = NULL;
    buf_cap = 0;

    /* These values mean that the file can be a zip64 file. */
    if (d->directory_records == 0xffff || d->directory_size == 0xffffffff ||
        d->directory_offset == 0xffffffff) {
        int64_t p64 = zip_find_directory64_end(r, end_offset, &e);
        if (BURROW_OK(e) && p64 >= 0) {
            end_offset = p64;
            e = zip_read_directory64_end(r, p64, d);
        }
        if (BURROW_FAILED(e))
            goto fail;
    }

    {
        uint64_t max_int64 = (uint64_t)INT64_MAX;
        if (d->directory_size > max_int64 || d->directory_offset > max_int64) {
            e = zip_err_format;
            goto fail;
        }
    }

    /* Go's int64 arithmetic wraps, and so does this. */
    *base_offset =
        (int64_t)((uint64_t)end_offset - d->directory_size - d->directory_offset);

    /* Make sure directory_offset points to somewhere in the file. */
    {
        int64_t o = (int64_t)((uint64_t)*base_offset + d->directory_offset);
        if (o < 0 || o >= size) {
            e = zip_err_format;
            goto fail;
        }
    }

    /* When the end record says the base offset is not zero, but a valid
     * directory header is there with a base of zero, use zero. Files have
     * been seen with an end record that gets the base wrong. */
    if (*base_offset > 0) {
        int64_t off = (int64_t)d->directory_offset;
        IoSectionReader rs = io_new_section_reader(r, off, size - off);
        Error he =
            burrow__zip_read_directory_header(a, io_section_reader_as_io_reader(&rs));
        if (BURROW_OK(he))
            *base_offset = 0;
    }
    return BURROW_NO_ERROR;

fail:
    if (buf != NULL)
        mem_free(a, buf, (size_t)buf_cap, 1);
    zip_str_free(a, *comment);
    *comment = (Str){zip_empty, 0};
    return e;
}

/* filepath.IsLocal for this system's kind of path, and no backslash. */
static bool zip_is_sep(Byte c) {
#if defined(BURROW_OS_WINDOWS)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#if defined(BURROW_OS_WINDOWS)
static bool zip_is_reserved_name(Str el) {
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL"};
    Int n = el.len;
    for (Int i = 0; i < el.len; i++) {
        if (el.p[i] == '.' || el.p[i] == ':') {
            n = i;
            break;
        }
    }
    while (n > 0 && el.p[n - 1] == ' ')
        n--;
    Byte up[8];
    if (n == 3 || n == 4) {
        for (Int i = 0; i < n; i++) {
            Byte c = el.p[i];
            up[i] = c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
        }
        if (n == 3) {
            for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
                if (memcmp(up, names[k], 3) == 0)
                    return true;
            if (memcmp(up, "CONIN$", 3) == 0)
                return false;
        }
        if (n == 4 && (memcmp(up, "COM", 3) == 0 || memcmp(up, "LPT", 3) == 0) &&
            up[3] >= '1' && up[3] <= '9')
            return true;
    }
    if (n == 6) {
        for (Int i = 0; i < n; i++) {
            Byte c = el.p[i];
            up[i] = c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
        }
        if (memcmp(up, "CONIN$", 6) == 0)
            return true;
    }
    if (n == 7) {
        for (Int i = 0; i < n; i++) {
            Byte c = el.p[i];
            up[i] = c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
        }
        if (memcmp(up, "CONOUT$", 7) == 0)
            return true;
    }
    return false;
}
#endif

static bool zip_is_local(Str path) {
    if (path.len == 0 || zip_is_sep(path.p[0]))
        return false;
#if defined(BURROW_OS_WINDOWS)
    if (memchr(path.p, ':', (size_t)path.len) != NULL)
        return false;
#endif
    Int depth = 0;
    bool escapes = false;
    Int i = 0;
    while (i < path.len) {
        Int start = i;
        while (i < path.len && !zip_is_sep(path.p[i]))
            i++;
        Str el = str_from_bytes(path.p + start, i - start);
        if (i < path.len)
            i++;
#if defined(BURROW_OS_WINDOWS)
        if (zip_is_reserved_name(el))
            return false;
#endif
        if (el.len == 0 || str_eq(el, BURROW_S(".")))
            continue;
        if (str_eq(el, BURROW_S(".."))) {
            if (depth == 0)
                escapes = true;
            else
                depth--;
            continue;
        }
        depth++;
    }
    return !escapes;
}

static ZipFile **zip_files(const ZipReader *r) {
    return (ZipFile **)r->file.p;
}

static bool zip_append_file(ZipReader *r, ZipFile *f) {
    if (r->file.len == r->file.cap) {
        Int ncap = r->file.cap == 0 ? 8 : r->file.cap * 2;
        void *np = mem_realloc(r->a, r->file.p, (size_t)r->file.cap * sizeof(ZipFile *),
                               (size_t)ncap * sizeof(ZipFile *), _Alignof(ZipFile *));
        if (np == NULL)
            return false;
        r->file.p = np;
        r->file.cap = ncap;
    }
    zip_files(r)[r->file.len++] = f;
    return true;
}

static Error zip_reader_init(ZipReader *r, IoReaderAt rdr, int64_t size) {
    ZipDirectoryEnd end;
    memset(&end, 0, sizeof end);
    int64_t base_offset = 0;
    Error e = zip_read_directory_end(r->a, rdr, size, &end, &base_offset, &r->comment);
    if (BURROW_FAILED(e))
        return e;
    r->r = rdr;
    r->base_offset = base_offset;
    /* The count of directory records is not validated, so it only sizes the
     * list up front when the archive is big enough to hold that many headers
     * of at least 30 bytes. */
    if (end.directory_size < (uint64_t)size &&
        ((uint64_t)size - end.directory_size) / 30 >= end.directory_records &&
        end.directory_records > 0) {
        Int n = (Int)end.directory_records;
        void *np = mem_alloc(r->a, (size_t)n * sizeof(ZipFile *), _Alignof(ZipFile *));
        if (np == NULL)
            return burrow_err_out_of_memory;
        r->file = slice_from(np, 0, n, TYPE_UNSAFE_POINTER);
    }
    IoSectionReader rs = io_new_section_reader(rdr, 0, size);
    io_section_reader_seek(&rs,
                           (int64_t)((uint64_t)r->base_offset + end.directory_offset),
                           BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e))
        return e;
    BufioReader *buf = bufio_new_reader(r->a, io_section_reader_as_io_reader(&rs));
    if (buf == NULL)
        return burrow_err_out_of_memory;

    /* The count of files is truncated to fit in 16 bits. Read headers until
     * a bad one, and only report it when the count modulo 65536 is off. */
    for (;;) {
        ZipFileMem *m = (ZipFileMem *)mem_alloc(r->a, sizeof *m, _Alignof(ZipFileMem));
        if (m == NULL) {
            e = burrow_err_out_of_memory;
            break;
        }
        m->f.zip = r;
        m->f.zipr = rdr;
        e = zip_read_directory_header(r->a, m, bufio_reader_as_io_reader(buf));
        if (BURROW_FAILED(e)) {
            zip_file_mem_free(r->a, m);
            break;
        }
        m->f.header_offset += r->base_offset;
        if (!zip_append_file(r, &m->f)) {
            zip_file_mem_free(r->a, m);
            e = burrow_err_out_of_memory;
            break;
        }
    }
    bufio_reader_free(buf);
    if (!zip_same(e, zip_err_format) && !zip_same(e, io_err_unexpected_eof))
        return e;
    if ((uint16_t)r->file.len != (uint16_t)end.directory_records)
        return e;
    if ((zip_debug_load() & ZIP_DEBUG_INSECURE_OFF) != 0) {
        for (Int i = 0; i < r->file.len; i++) {
            Str name = zip_files(r)[i]->file_header.name;
            if (name.len == 0)
                continue; /* zip permits an empty name */
            /* The specification says names use forward slashes, so any
             * backslash is insecure. */
            if (!zip_is_local(name) || memchr(name.p, '\\', (size_t)name.len) != NULL)
                return zip_err_insecure_path;
        }
    }
    return BURROW_NO_ERROR;
}

static void zip_reader_release(ZipReader *r) {
    Alloc *a = r->a;
    for (Int i = 0; i < r->file.len; i++)
        zip_file_mem_free(a, (ZipFileMem *)(void *)zip_files(r)[i]);
    if (r->file.cap > 0)
        mem_free(a, r->file.p, (size_t)r->file.cap * sizeof(ZipFile *),
                 _Alignof(ZipFile *));
    r->file = slice_nil(TYPE_UNSAFE_POINTER);
    zip_str_free(a, r->comment);
    r->comment = (Str){zip_empty, 0};
    ZipDecompressorEntry *d = r->decompressors;
    while (d != NULL) {
        ZipDecompressorEntry *next = d->next;
        mem_free(a, d, sizeof *d, _Alignof(ZipDecompressorEntry));
        d = next;
    }
    r->decompressors = NULL;
    for (Int i = 0; i < r->file_list_len; i++)
        if (r->file_list[i].own_name)
            zip_str_free(a, r->file_list[i].name);
    if (r->file_list != NULL)
        mem_free(a, r->file_list, (size_t)r->file_list_len * sizeof(ZipFileListEntry),
                 _Alignof(ZipFileListEntry));
    r->file_list = NULL;
    r->file_list_len = 0;
}

static void zip_reader_setup(ZipReader *r, Alloc *a) {
    memset(r, 0, sizeof *r);
    r->a = a;
    r->file = slice_nil(TYPE_UNSAFE_POINTER);
    r->comment = (Str){zip_empty, 0};
}

ZipReader *zip_new_reader(Alloc *a, IoReaderAt rdr, int64_t size, Error *err) {
    if (size < 0) {
        BURROW_OUT(err, zip_err_negative_size);
        return NULL;
    }
    ZipReader *r = (ZipReader *)mem_alloc(a, sizeof *r, _Alignof(ZipReader));
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    zip_reader_setup(r, a);
    Error e = zip_reader_init(r, rdr, size);
    if (BURROW_FAILED(e) && !zip_same(e, zip_err_insecure_path)) {
        zip_reader_free(r);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, e);
    return r;
}

void zip_reader_free(ZipReader *r) {
    if (r == NULL)
        return;
    Alloc *a = r->a;
    zip_reader_release(r);
    mem_free(a, r, sizeof *r, _Alignof(ZipReader));
}

void zip_reader_register_decompressor(ZipReader *r, uint16_t method,
                                      ZipDecompressor dcomp) {
    for (ZipDecompressorEntry *d = r->decompressors; d != NULL; d = d->next) {
        if (d->method == method) {
            d->d = dcomp;
            return;
        }
    }
    ZipDecompressorEntry *n = (ZipDecompressorEntry *)mem_alloc(
        r->a, sizeof *n, _Alignof(ZipDecompressorEntry));
    if (n == NULL)
        runtime_panic(BURROW_S("zip: RegisterDecompressor: out of memory"));
    n->method = method;
    n->d = dcomp;
    n->next = r->decompressors;
    r->decompressors = n;
}

static ZipDecompressor zip_reader_decompressor(ZipReader *r, uint16_t method) {
    for (ZipDecompressorEntry *d = r->decompressors; d != NULL; d = d->next)
        if (d->method == method && !BURROW_FUNC_IS_NIL(d->d))
            return d->d;
    return zip_decompressor(method);
}

/* findBodyOffset: the minimum to check the file has a header, and where its
 * body starts after it. */
static int64_t zip_find_body_offset(ZipFile *f, Error *err) {
    Byte buf[ZIP_FILE_HEADER_LEN];
    Error e;
    zip_read_at(f->zipr, buf, (Int)sizeof buf, f->header_offset, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return 0;
    }
    if (zip_le32(buf) != ZIP_FILE_HEADER_SIGNATURE) {
        *err = zip_err_format;
        return 0;
    }
    int64_t filename_len = zip_le16(buf + 26);
    int64_t extra_len = zip_le16(buf + 28);
    *err = BURROW_NO_ERROR;
    return ZIP_FILE_HEADER_LEN + filename_len + extra_len;
}

int64_t zip_file_data_offset(ZipFile *f, Error *err) {
    Error e;
    int64_t body = zip_find_body_offset(f, &e);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return 0;
    return f->header_offset + body;
}

IoSectionReader zip_file_open_raw(ZipFile *f, Error *err) {
    Error e;
    int64_t body = zip_find_body_offset(f, &e);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return io_new_section_reader((IoReaderAt){NULL, NULL}, 0, 0);
    return io_new_section_reader(f->zipr, f->header_offset + body,
                                 (int64_t)f->file_header.compressed_size64);
}

/* dirReader. */
typedef struct ZipDirReader {
    Alloc *a;
    Error err;
} ZipDirReader;

static Int zip_dir_read(void *self, Slice p, Error *err) {
    (void)p;
    *err = ((ZipDirReader *)self)->err;
    return 0;
}

static Error zip_dir_read_close(void *self) {
    ZipDirReader *d = (ZipDirReader *)self;
    mem_free(d->a, d, sizeof *d, _Alignof(ZipDirReader));
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT zip_dir_reader_vt = {
    {&zip_dir_reader_desc, zip_dir_read},
    {&zip_dir_reader_desc, zip_dir_read_close},
};

/* checksumReader. body is the section the decompressor reads, and desr the
 * one the data descriptor is in. */
typedef struct ZipChecksumReader {
    Alloc *a;
    IoReadCloser rc;
    uint32_t crc32;
    bool fake_crc32;
    uint64_t nread;
    ZipFile *f;
    IoSectionReader body;
    IoSectionReader desr;
    bool has_desr;
    Error err;
} ZipChecksumReader;

ZIP_DESC(zip_checksum_reader_desc, "checksumReader", ZipChecksumReader, 0x7a63726bU);

/* readDataDescriptor. */
static Error zip_read_data_descriptor(IoReader r, ZipFile *f) {
    /* Zeroed only for gcc, which cannot see that io_read_full writes it. */
    Byte buf[ZIP_DATA_DESCRIPTOR_LEN] = {0};
    Error e = BURROW_NO_ERROR;
    /* The signature is optional, so read its four bytes first and keep them
     * when they are not it. */
    io_read_full(r, zip_bytes(buf, 4), &e);
    if (BURROW_FAILED(e))
        return e;
    Int off = 0;
    if (zip_le32(buf) != ZIP_DATA_DESCRIPTOR_SIGNATURE)
        off += 4;
    io_read_full(r, zip_bytes(buf + off, 12 - off), &e);
    if (BURROW_FAILED(e))
        return e;
    if (zip_le32(buf) != f->file_header.crc32)
        return zip_err_checksum;
    /* The sizes that follow can be 32 or 64 bits, and the directory has them
     * anyway, so they are not read. */
    return BURROW_NO_ERROR;
}

static Int zip_checksum_read(void *self, Slice b, Error *err) {
    ZipChecksumReader *r = (ZipChecksumReader *)self;
    if (BURROW_FAILED(r->err)) {
        *err = r->err;
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = r->rc.vt->reader.read(r->rc.data, b, &e);
    if (!r->fake_crc32)
        r->crc32 = crc32_update(r->crc32, crc32_ieee_table, slice_sub(b, 0, n));
    r->nread += (uint64_t)n;
    const ZipFileHeader *h = &r->f->file_header;
    if (r->nread > h->uncompressed_size64) {
        *err = zip_err_format;
        return 0;
    }
    if (BURROW_OK(e)) {
        *err = e;
        return n;
    }
    if (zip_same(e, io_eof)) {
        if (r->nread != h->uncompressed_size64) {
            *err = io_err_unexpected_eof;
            return 0;
        }
        if (r->has_desr) {
            Error e1 = zip_read_data_descriptor(
                io_section_reader_as_io_reader(&r->desr), r->f);
            if (BURROW_FAILED(e1)) {
                if (zip_same(e1, io_eof))
                    e = io_err_unexpected_eof;
                else
                    e = e1;
            } else if (r->crc32 != h->crc32) {
                e = zip_err_checksum;
            }
        } else {
            /* Without a data descriptor, still check the CRC-32 of what was
             * read against the header's, when it seems to have been set. */
            if (h->crc32 != 0 && r->crc32 != h->crc32)
                e = zip_err_checksum;
        }
    }
    r->err = e;
    *err = e;
    return n;
}

static Error zip_checksum_close(void *self) {
    ZipChecksumReader *r = (ZipChecksumReader *)self;
    Error e = r->rc.vt->closer.close(r->rc.data);
    mem_free(r->a, r, sizeof *r, _Alignof(ZipChecksumReader));
    return e;
}

static FsFileInfo zip_checksum_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return zip_file_header_file_info(&((ZipChecksumReader *)self)->f->file_header);
}

static const FsFileVT zip_checksum_reader_vt = {
    {{&zip_checksum_reader_desc, zip_checksum_read},
     {&zip_checksum_reader_desc, zip_checksum_close}},
    zip_checksum_stat,
    NULL,
};

void burrow__zip_checksum_reader_fake_crc32(IoReadCloser rc) {
    if (rc.vt == &zip_checksum_reader_vt.read_closer)
        ((ZipChecksumReader *)rc.data)->fake_crc32 = true;
}

IoReadCloser zip_file_open(ZipFile *f, Alloc *a, Error *err) {
    Error e;
    int64_t body = zip_find_body_offset(f, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (IoReadCloser){NULL, NULL};
    }
    if (zip_has_slash_suffix(f->file_header.name)) {
        /* Directories should have no data, but the Java jar tool, among
         * others, does not always set the method on them, so only a
         * directory with uncompressed data fails. */
        ZipDirReader *d =
            (ZipDirReader *)mem_alloc(a, sizeof *d, _Alignof(ZipDirReader));
        if (d == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (IoReadCloser){NULL, NULL};
        }
        d->a = a;
        d->err = f->file_header.uncompressed_size64 != 0 ? zip_err_format : io_eof;
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (IoReadCloser){&zip_dir_reader_vt, d};
    }
    int64_t size = (int64_t)f->file_header.compressed_size64;
    ZipDecompressor dcomp = zip_reader_decompressor(f->zip, f->file_header.method);
    if (BURROW_FUNC_IS_NIL(dcomp)) {
        BURROW_OUT(err, zip_err_algorithm);
        return (IoReadCloser){NULL, NULL};
    }
    ZipChecksumReader *r =
        (ZipChecksumReader *)mem_alloc(a, sizeof *r, _Alignof(ZipChecksumReader));
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoReadCloser){NULL, NULL};
    }
    r->a = a;
    r->f = f;
    r->body = io_new_section_reader(f->zipr, f->header_offset + body, size);
    r->rc = dcomp.f(dcomp.env, a, io_section_reader_as_io_reader(&r->body));
    if (r->rc.vt == NULL) {
        mem_free(a, r, sizeof *r, _Alignof(ZipChecksumReader));
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoReadCloser){NULL, NULL};
    }
    if (zip_has_data_descriptor(&f->file_header)) {
        r->desr = io_new_section_reader(f->zipr, f->header_offset + body + size,
                                        ZIP_DATA_DESCRIPTOR_LEN);
        r->has_desr = true;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (IoReadCloser){&zip_checksum_reader_vt.read_closer, r};
}

/* -------------------------------------------------------------- the fs.FS */

/* split: the directory, the last element and whether name ended in a
 * slash. */
static void zip_split(Str name, Str *dir, Str *elem) {
    if (zip_has_slash_suffix(name))
        name.len--;
    Int i = name.len - 1;
    while (i >= 0 && name.p[i] != '/')
        i--;
    if (i < 0) {
        *dir = BURROW_S(".");
        *elem = name;
        return;
    }
    *dir = str_from_bytes(name.p, i);
    *elem = str_from_bytes(name.p + i + 1, name.len - i - 1);
}

int burrow__zip_file_entry_compare(Str x, Str y) {
    Str xdir, xelem, ydir, yelem;
    zip_split(x, &xdir, &xelem);
    zip_split(y, &ydir, &yelem);
    if (!str_eq(xdir, ydir))
        return str_cmp(xdir, ydir);
    return str_cmp(xelem, yelem);
}

Str burrow__zip_to_valid_name(Alloc *a, Str name) {
    Byte *tmp = NULL;
    if (name.len > 0) {
        tmp = (Byte *)mem_alloc_nozero(a, (size_t)name.len, 1);
        if (tmp == NULL)
            return (Str){NULL, 0};
        for (Int i = 0; i < name.len; i++)
            tmp[i] = name.p[i] == '\\' ? '/' : name.p[i];
    }
    Str c = path_clean(a, str_from_bytes(tmp == NULL ? zip_empty : tmp, name.len));
    bool owned = c.p != NULL && (c.p < tmp || c.p >= tmp + name.len);
    if (c.p == NULL) {
        if (tmp != NULL)
            mem_free(a, tmp, (size_t)name.len, 1);
        return (Str){NULL, 0};
    }
    Str p = c;
    if (p.len > 0 && p.p[0] == '/') {
        p.p++;
        p.len--;
    }
    while (p.len >= 3 && p.p[0] == '.' && p.p[1] == '.' && p.p[2] == '/') {
        p.p += 3;
        p.len -= 3;
    }
    bool ok = true;
    Str out = zip_dup(a, p, &ok);
    if (owned)
        zip_str_free(a, c);
    if (tmp != NULL)
        mem_free(a, tmp, (size_t)name.len, 1);
    if (!ok)
        return (Str){NULL, 0};
    return out;
}

/* A map from names to file list indexes, open addressed, for building the
 * list. */
typedef struct ZipNameSet {
    Alloc *a;
    Str *keys;
    Int *vals;
    Int cap;
    Int len;
} ZipNameSet;

static uint64_t zip_hash(Str s) {
    uint64_t h = 1469598103934665603ULL;
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static Int zip_set_find(const ZipNameSet *s, Str k) {
    if (s->cap == 0)
        return -1;
    uint64_t mask = (uint64_t)s->cap - 1;
    for (uint64_t i = zip_hash(k) & mask;; i = (i + 1) & mask) {
        if (s->keys[i].p == NULL)
            return -1;
        if (str_eq(s->keys[i], k))
            return (Int)i;
    }
}

/* The value for k, or NULL when k is not in the set. */
static const Int *zip_set_get(const ZipNameSet *s, Str k) {
    Int i = zip_set_find(s, k);
    return i < 0 || s->vals == NULL ? NULL : &s->vals[i];
}

static bool zip_set_put(ZipNameSet *s, Str k, Int v) {
    if ((s->len + 1) * 2 > s->cap) {
        Int ncap = s->cap == 0 ? 16 : s->cap * 2;
        Str *nk = (Str *)mem_alloc(s->a, (size_t)ncap * sizeof(Str), _Alignof(Str));
        Int *nv = (Int *)mem_alloc(s->a, (size_t)ncap * sizeof(Int), _Alignof(Int));
        if (nk == NULL || nv == NULL) {
            if (nk != NULL)
                mem_free(s->a, nk, (size_t)ncap * sizeof(Str), _Alignof(Str));
            if (nv != NULL)
                mem_free(s->a, nv, (size_t)ncap * sizeof(Int), _Alignof(Int));
            return false;
        }
        uint64_t mask = (uint64_t)ncap - 1;
        for (Int i = 0; i < s->cap; i++) {
            if (s->keys[i].p == NULL)
                continue;
            uint64_t j = zip_hash(s->keys[i]) & mask;
            while (nk[j].p != NULL)
                j = (j + 1) & mask;
            nk[j] = s->keys[i];
            nv[j] = s->vals[i];
        }
        if (s->cap > 0) {
            mem_free(s->a, s->keys, (size_t)s->cap * sizeof(Str), _Alignof(Str));
            mem_free(s->a, s->vals, (size_t)s->cap * sizeof(Int), _Alignof(Int));
        }
        s->keys = nk;
        s->vals = nv;
        s->cap = ncap;
    }
    uint64_t mask = (uint64_t)s->cap - 1;
    uint64_t i = zip_hash(k) & mask;
    while (s->keys[i].p != NULL) {
        if (str_eq(s->keys[i], k)) {
            s->vals[i] = v;
            return true;
        }
        i = (i + 1) & mask;
    }
    s->keys[i] = k;
    s->vals[i] = v;
    s->len++;
    return true;
}

static void zip_set_free(ZipNameSet *s) {
    if (s->cap > 0) {
        mem_free(s->a, s->keys, (size_t)s->cap * sizeof(Str), _Alignof(Str));
        mem_free(s->a, s->vals, (size_t)s->cap * sizeof(Int), _Alignof(Int));
    }
}

typedef struct ZipFileList {
    Alloc *a;
    ZipFileListEntry *p;
    Int len;
    Int cap;
} ZipFileList;

static bool zip_list_push(ZipFileList *l, ZipFileListEntry e) {
    if (l->len == l->cap) {
        Int ncap = l->cap == 0 ? 8 : l->cap * 2;
        void *np = mem_realloc(l->a, l->p, (size_t)l->cap * sizeof(ZipFileListEntry),
                               (size_t)ncap * sizeof(ZipFileListEntry),
                               _Alignof(ZipFileListEntry));
        if (np == NULL)
            return false;
        l->p = (ZipFileListEntry *)np;
        l->cap = ncap;
    }
    l->p[l->len++] = e;
    return true;
}

static Int zip_list_len(void *self) {
    return ((ZipFileList *)self)->len;
}

static bool zip_list_less(void *self, Int i, Int j) {
    ZipFileList *l = (ZipFileList *)self;
    return burrow__zip_file_entry_compare(l->p[i].name, l->p[j].name) < 0;
}

static void zip_list_swap(void *self, Int i, Int j) {
    ZipFileList *l = (ZipFileList *)self;
    ZipFileListEntry t = l->p[i];
    l->p[i] = l->p[j];
    l->p[j] = t;
}

ZIP_DESC(zip_file_list_desc, "fileList", ZipFileList, 0x7a666c73U);

static const SortInterfaceVT zip_list_sort_vt = {&zip_file_list_desc, zip_list_len,
                                                 zip_list_less, zip_list_swap};

static void zip_init_file_list(void *env) {
    ZipReader *r = (ZipReader *)env;
    Alloc *a = r->a;
    ZipFileList l = {a, NULL, 0, 0};
    /* names maps a file or directory name to its index in the list, to mark
     * duplicates, and is_dirs holds every directory a path has in it. */
    ZipNameSet names = {a, NULL, NULL, 0, 0};
    ZipNameSet dirs = {a, NULL, NULL, 0, 0};
    Error e = BURROW_NO_ERROR;
    for (Int i = 0; i < r->file.len; i++) {
        ZipFile *file = zip_files(r)[i];
        bool is_dir = zip_has_slash_suffix(file->file_header.name);
        Str name = burrow__zip_to_valid_name(a, file->file_header.name);
        if (name.p == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
        if (name.len == 0)
            continue;
        const Int *at = zip_set_get(&names, name);
        if (at != NULL) {
            l.p[*at].is_dup = true;
            zip_str_free(a, name);
            continue;
        }
        Str dir = name;
        for (;;) {
            Int idx = dir.len - 1;
            while (idx >= 0 && dir.p[idx] != '/')
                idx--;
            if (idx < 0)
                break;
            dir.len = idx;
            if (zip_set_find(&dirs, dir) >= 0)
                break;
            if (!zip_set_put(&dirs, dir, 0)) {
                zip_str_free(a, name);
                e = burrow_err_out_of_memory;
                goto done;
            }
        }
        ZipFileListEntry entry = {name, file, is_dir, false, true};
        if (!zip_list_push(&l, entry)) {
            zip_str_free(a, name);
            e = burrow_err_out_of_memory;
            goto done;
        }
        /* An entry that is a directory and one that is a file are in two maps
         * in Go, and a name is only ever in one of them. */
        if (!zip_set_put(&names, name, l.len - 1)) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }
    for (Int i = 0; i < dirs.cap; i++) {
        if (dirs.keys[i].p == NULL)
            continue;
        Str dir = dirs.keys[i];
        const Int *at = zip_set_get(&names, dir);
        if (at != NULL) {
            ZipFileListEntry *known = &l.p[*at];
            if (!known->is_dir)
                known->is_dup = true;
            continue;
        }
        ZipFileListEntry entry = {dir, NULL, true, false, false};
        if (!zip_list_push(&l, entry)) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }
    sort_sort((SortInterface){&zip_list_sort_vt, &l});

done:
    zip_set_free(&names);
    zip_set_free(&dirs);
    if (BURROW_FAILED(e)) {
        for (Int i = 0; i < l.len; i++)
            if (l.p[i].own_name)
                zip_str_free(a, l.p[i].name);
        if (l.cap > 0)
            mem_free(a, l.p, (size_t)l.cap * sizeof(ZipFileListEntry),
                     _Alignof(ZipFileListEntry));
        r->file_list_err = e;
        return;
    }
    /* Trim to the length, so the reader frees what it has. */
    if (l.len == 0 && l.cap > 0) {
        mem_free(a, l.p, (size_t)l.cap * sizeof(ZipFileListEntry),
                 _Alignof(ZipFileListEntry));
        l.p = NULL;
        l.cap = 0;
    } else if (l.cap > l.len) {
        void *np = mem_realloc(a, l.p, (size_t)l.cap * sizeof(ZipFileListEntry),
                               (size_t)l.len * sizeof(ZipFileListEntry),
                               _Alignof(ZipFileListEntry));
        if (np != NULL) {
            l.p = (ZipFileListEntry *)np;
            l.cap = l.len;
        }
    }
    if (l.cap > l.len) {
        /* realloc refused to shrink: keep the block and remember its size by
         * padding the list with entries that own nothing. */
        while (l.len < l.cap)
            l.p[l.len++] =
                (ZipFileListEntry){{zip_empty, 0}, NULL, false, false, false};
    }
    r->file_list = l.p;
    r->file_list_len = l.len;
}

static const ZipFileListEntry zip_dot_file = {
    {(const Byte *)"./", 2}, NULL, true, false, false};

/* The entries that are real, which leaves out any padding at the end. */
static Int zip_list_real_len(const ZipReader *r) {
    Int n = r->file_list_len;
    while (n > 0 && r->file_list[n - 1].name.len == 0)
        n--;
    return n;
}

static ZipFileListEntry *zip_open_lookup(ZipReader *r, Str name) {
    if (str_eq(name, BURROW_S(".")))
        return (ZipFileListEntry *)(uintptr_t)&zip_dot_file;
    Str dir, elem;
    zip_split(name, &dir, &elem);
    ZipFileListEntry *files = r->file_list;
    Int lo = 0, hi = zip_list_real_len(r);
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        Str idir, ielem;
        zip_split(files[mid].name, &idir, &ielem);
        int c = !str_eq(dir, idir) ? str_cmp(idir, dir) : str_cmp(ielem, elem);
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < zip_list_real_len(r)) {
        Str fname = files[lo].name;
        if (str_eq(fname, name) ||
            (fname.len == name.len + 1 && fname.p[name.len] == '/' &&
             memcmp(fname.p, name.p, (size_t)name.len) == 0))
            return &files[lo];
    }
    return NULL;
}

static void zip_open_read_dir(ZipReader *r, Str dir, Int *from, Int *to) {
    ZipFileListEntry *files = r->file_list;
    Int n = zip_list_real_len(r);
    Int lo = 0, hi = n;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        Str idir, ielem;
        zip_split(files[mid].name, &idir, &ielem);
        int c = !str_eq(dir, idir) ? str_cmp(idir, dir) : 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *from = lo;
    lo = 0;
    hi = n;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        Str jdir, jelem;
        zip_split(files[mid].name, &jdir, &jelem);
        int c = !str_eq(dir, jdir) ? str_cmp(jdir, dir) : -1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *to = lo;
}

/* fileListEntry as a FileInfo and a DirEntry, for directories only. */
static Str zip_fle_name(void *self) {
    Str dir, elem;
    zip_split(((const ZipFileListEntry *)self)->name, &dir, &elem);
    return elem;
}

static int64_t zip_fle_size(void *self) {
    (void)self;
    return 0;
}

static FsFileMode zip_fle_mode(void *self) {
    (void)self;
    return FS_MODE_DIR | 0555;
}

static FsFileMode zip_fle_type(void *self) {
    (void)self;
    return FS_MODE_DIR;
}

static bool zip_fle_is_dir(void *self) {
    (void)self;
    return true;
}

static Any zip_fle_sys(void *self) {
    (void)self;
    return (Any){0};
}

static Time zip_fle_mod_time(void *self) {
    const ZipFileListEntry *f = (const ZipFileListEntry *)self;
    if (f->file == NULL)
        return (Time){0};
    return time_utc(f->file->file_header.modified);
}

static const FsFileInfoVT zip_fle_info_vt = {
    &zip_file_list_entry_desc, zip_fle_name,   zip_fle_size, zip_fle_mode,
    zip_fle_mod_time,          zip_fle_is_dir, zip_fle_sys,
};

static FsFileInfo zip_fle_info(void *self, Alloc *a, Error *err) {
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (FsFileInfo){&zip_fle_info_vt, self};
}

static const FsDirEntryVT zip_fle_dir_entry_vt = {
    &zip_file_list_entry_desc, zip_fle_name, zip_fle_is_dir, zip_fle_type, zip_fle_info,
};

/* fileListEntry.stat, as a FileInfo when info is set and as a DirEntry when
 * entry is. */
static Error zip_fle_stat(Alloc *a, ZipFileListEntry *f, FsFileInfo *info,
                          FsDirEntry *entry) {
    if (f->is_dup)
        return fmt_errorf_v("%s: duplicate entries in zip file", f->name);
    if (!f->is_dir) {
        if (info != NULL)
            *info = (FsFileInfo){&zip_hfi_vt, &f->file->file_header};
        if (entry != NULL)
            *entry = (FsDirEntry){&zip_hfi_dir_entry_vt, &f->file->file_header};
        return BURROW_NO_ERROR;
    }
    (void)a;
    if (info != NULL)
        *info = (FsFileInfo){&zip_fle_info_vt, f};
    if (entry != NULL)
        *entry = (FsDirEntry){&zip_fle_dir_entry_vt, f};
    return BURROW_NO_ERROR;
}

/* openDir. */
typedef struct ZipOpenDir {
    Alloc *a;
    ZipFileListEntry *e;
    ZipFileListEntry *files;
    Int n;
    Int offset;
} ZipOpenDir;

static Error zip_open_dir_close(void *self) {
    ZipOpenDir *d = (ZipOpenDir *)self;
    mem_free(d->a, d, sizeof *d, _Alignof(ZipOpenDir));
    return BURROW_NO_ERROR;
}

static FsFileInfo zip_open_dir_stat(void *self, Alloc *a, Error *err) {
    ZipOpenDir *d = (ZipOpenDir *)self;
    FsFileInfo info = {NULL, NULL};
    Error e = zip_fle_stat(a, d->e, &info, NULL);
    BURROW_OUT(err, e);
    return info;
}

static Int zip_open_dir_read(void *self, Slice p, Error *err) {
    (void)p;
    ZipOpenDir *d = (ZipOpenDir *)self;
    *err = fs_path_error_new(d->a, BURROW_S("read"), d->e->name, zip_err_is_dir);
    return 0;
}

static Slice zip_open_dir_read_dir(void *self, Alloc *a, Int count, Error *err) {
    ZipOpenDir *d = (ZipOpenDir *)self;
    Int n = d->n - d->offset;
    if (count > 0 && n > count)
        n = count;
    if (n == 0) {
        BURROW_OUT(err, count <= 0 ? BURROW_NO_ERROR : io_eof);
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    FsDirEntry *list = (FsDirEntry *)mem_alloc(a, (size_t)n * sizeof(FsDirEntry),
                                               _Alignof(FsDirEntry));
    if (list == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    for (Int i = 0; i < n; i++) {
        ZipFileListEntry *f = &d->files[d->offset + i];
        Error e = zip_fle_stat(a, f, NULL, &list[i]);
        Str name = BURROW_S("");
        if (BURROW_OK(e))
            name = list[i].vt->name(list[i].data);
        if (BURROW_OK(e) && (str_eq(name, BURROW_S(".")) || !fs_valid_path(name)))
            e = fs_path_error_new(a, BURROW_S("readdir"), d->e->name,
                                  fmt_errorf_v("invalid file name: %v", f->name));
        if (BURROW_FAILED(e)) {
            mem_free(a, list, (size_t)n * sizeof(FsDirEntry), _Alignof(FsDirEntry));
            BURROW_OUT(err, e);
            return slice_nil(TYPE_FS_DIR_ENTRY);
        }
    }
    d->offset += n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slice_from(list, n, n, TYPE_FS_DIR_ENTRY);
}

static const FsFileVT zip_open_dir_vt = {
    {{&zip_open_dir_desc, zip_open_dir_read}, {&zip_open_dir_desc, zip_open_dir_close}},
    zip_open_dir_stat,
    zip_open_dir_read_dir,
};

FsFile zip_reader_open(ZipReader *r, Alloc *a, Str name, Error *err) {
    sync_once_do(&r->file_list_once, BURROW_FN(Func, zip_init_file_list, r));
    if (BURROW_FAILED(r->file_list_err)) {
        BURROW_OUT(err, r->file_list_err);
        return (FsFile){NULL, NULL};
    }
    if (!fs_valid_path(name)) {
        BURROW_OUT(err, fs_path_error_new(a, BURROW_S("open"), name, fs_err_invalid));
        return (FsFile){NULL, NULL};
    }
    ZipFileListEntry *e = zip_open_lookup(r, name);
    if (e == NULL) {
        BURROW_OUT(err, fs_path_error_new(a, BURROW_S("open"), name, fs_err_not_exist));
        return (FsFile){NULL, NULL};
    }
    if (e->is_dir) {
        ZipOpenDir *d = (ZipOpenDir *)mem_alloc(a, sizeof *d, _Alignof(ZipOpenDir));
        if (d == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (FsFile){NULL, NULL};
        }
        Int from, to;
        zip_open_read_dir(r, name, &from, &to);
        d->a = a;
        d->e = e;
        d->files = r->file_list + from;
        d->n = to - from;
        d->offset = 0;
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (FsFile){&zip_open_dir_vt, d};
    }
    Error oe;
    IoReadCloser rc = zip_file_open(e->file, a, &oe);
    if (BURROW_FAILED(oe)) {
        BURROW_OUT(err, oe);
        return (FsFile){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    /* A file that is not a directory is always a checksum reader. */
    return (FsFile){&zip_checksum_reader_vt, rc.data};
}

static FsFile zip_fs_open(void *self, Alloc *a, Str name, Error *err) {
    return zip_reader_open((ZipReader *)self, a, name, err);
}

static const FsVT zip_reader_fs_vt = {
    &zip_reader_desc, zip_fs_open, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

Fs zip_reader_as_fs(ZipReader *r) {
    return (Fs){&zip_reader_fs_vt, r};
}

/* -------------------------------------------------------------- ReadCloser */

typedef struct ZipFd {
    int64_t fd;
    Str path;
} ZipFd;

static Int zip_fd_read_at(void *self, Slice p, int64_t off, Error *err) {
    ZipReadCloser *rc = (ZipReadCloser *)self;
    Int n = 0;
    *err = BURROW_NO_ERROR;
    if (off < 0) {
        *err = fmt_errorf_v("readat %s: negative offset", rc->path);
        return 0;
    }
    while (n < p.len) {
        PalErrno e = PAL_OK;
        int64_t got = pal_pread(rc->fd, (Byte *)p.p + n, p.len - n, off + n, &e);
        if (got < 0) {
            *err = fmt_errorf_v("read %s: %s", rc->path,
                                str_from_cstr(pal_errno_string(e)));
            return n;
        }
        if (got == 0) {
            *err = io_eof;
            return n;
        }
        n += (Int)got;
    }
    return n;
}

static const IoReaderAtVT zip_fd_reader_at_vt = {&zip_read_closer_desc, zip_fd_read_at};

ZipReadCloser *zip_open_reader(Alloc *a, Str name, Error *err) {
    ZipReadCloser *rc =
        (ZipReadCloser *)mem_alloc(a, sizeof *rc, _Alignof(ZipReadCloser));
    if (rc == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    zip_reader_setup(&rc->reader, a);
    rc->fd = -1;
    /* The path, with a NUL after it for the system. */
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)name.len + 1, 1);
    if (p == NULL) {
        mem_free(a, rc, sizeof *rc, _Alignof(ZipReadCloser));
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    p[name.len] = 0;
    rc->path = (Str){p, name.len};
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    PalStat st;
    rc->fd = pal_open((const char *)p, PAL_O_RDONLY, 0, &pe);
    if (rc->fd < 0) {
        e = fs_path_error_new(
            error_allocator(), BURROW_S("open"), name,
            errors_new(error_allocator(), str_from_cstr(pal_errno_string(pe))));
        goto fail;
    }
    if (!pal_fstat(rc->fd, &st, &pe)) {
        e = fs_path_error_new(
            error_allocator(), BURROW_S("stat"), name,
            errors_new(error_allocator(), str_from_cstr(pal_errno_string(pe))));
        goto fail;
    }
    e = zip_reader_init(&rc->reader, (IoReaderAt){&zip_fd_reader_at_vt, rc}, st.size);
    if (BURROW_FAILED(e) && !zip_same(e, zip_err_insecure_path))
        goto fail;
    BURROW_OUT(err, e);
    return rc;

fail:
    (void)zip_read_closer_close(rc);
    BURROW_OUT(err, e);
    return NULL;
}

FsFile zip_read_closer_open(ZipReadCloser *rc, Alloc *a, Str name, Error *err) {
    return zip_reader_open(&rc->reader, a, name, err);
}

void zip_read_closer_register_decompressor(ZipReadCloser *rc, uint16_t method,
                                           ZipDecompressor dcomp) {
    zip_reader_register_decompressor(&rc->reader, method, dcomp);
}

Error zip_read_closer_close(ZipReadCloser *rc) {
    if (rc == NULL)
        return BURROW_NO_ERROR;
    Alloc *a = rc->reader.a;
    Error e = BURROW_NO_ERROR;
    if (rc->fd >= 0) {
        PalErrno pe = PAL_OK;
        if (!pal_close(rc->fd, &pe))
            e = fs_path_error_new(
                error_allocator(), BURROW_S("close"), rc->path,
                errors_new(error_allocator(), str_from_cstr(pal_errno_string(pe))));
    }
    zip_reader_release(&rc->reader);
    mem_free(a, (void *)(uintptr_t)rc->path.p, (size_t)rc->path.len + 1, 1);
    mem_free(a, rc, sizeof *rc, _Alignof(ZipReadCloser));
    return e;
}

/* ----------------------------------------------------------------- writing */

static Int zip_count_write(void *self, Slice p, Error *err) {
    ZipCountWriter *w = (ZipCountWriter *)self;
    if (w->discard) {
        *err = BURROW_NO_ERROR;
        w->count += p.len;
        return p.len;
    }
    *err = BURROW_NO_ERROR;
    Int n = w->w.vt->write(w->w.data, p, err);
    w->count += n;
    return n;
}

static const IoWriterVT zip_count_writer_vt = {&zip_count_writer_desc, zip_count_write};

static IoWriter zip_count_as_writer(ZipCountWriter *w) {
    return (IoWriter){&zip_count_writer_vt, w};
}

static Int zip_cw_write(ZipWriter *w, const Byte *p, Int n, Error *err) {
    return zip_count_write(&w->cw, zip_bytes(p, n), err);
}

ZipWriter *zip_new_writer(Alloc *a, IoWriter w) {
    ZipWriter *zw = (ZipWriter *)mem_alloc(a, sizeof *zw, _Alignof(ZipWriter));
    if (zw == NULL)
        return NULL;
    zw->a = a;
    zw->bw = bufio_new_writer(a, w);
    if (zw->bw == NULL) {
        mem_free(a, zw, sizeof *zw, _Alignof(ZipWriter));
        return NULL;
    }
    zw->cw.w = bufio_writer_as_io_writer(zw->bw);
    zw->comment = (Str){zip_empty, 0};
    return zw;
}

void zip_writer_set_offset(ZipWriter *w, int64_t n) {
    if (w->cw.count != 0)
        runtime_panic(BURROW_S("zip: SetOffset called after data was written"));
    w->cw.count = n;
}

Error zip_writer_flush(ZipWriter *w) {
    return bufio_writer_flush(w->bw);
}

Error zip_writer_set_comment(ZipWriter *w, Str comment) {
    if (comment.len > (Int)ZIP_UINT16_MAX)
        return zip_err_long_comment;
    bool ok = true;
    Str c = zip_dup(w->a, comment, &ok);
    if (!ok)
        return burrow_err_out_of_memory;
    zip_str_free(w->a, w->comment);
    w->comment = c;
    return BURROW_NO_ERROR;
}

void zip_writer_register_compressor(ZipWriter *w, uint16_t method, ZipCompressor comp) {
    for (ZipCompressorEntry *c = w->compressors; c != NULL; c = c->next) {
        if (c->method == method) {
            c->c = comp;
            return;
        }
    }
    ZipCompressorEntry *n =
        (ZipCompressorEntry *)mem_alloc(w->a, sizeof *n, _Alignof(ZipCompressorEntry));
    if (n == NULL)
        runtime_panic(BURROW_S("zip: RegisterCompressor: out of memory"));
    n->method = method;
    n->c = comp;
    n->next = w->compressors;
    w->compressors = n;
}

static ZipCompressor zip_writer_compressor(ZipWriter *w, uint16_t method) {
    for (ZipCompressorEntry *c = w->compressors; c != NULL; c = c->next)
        if (c->method == method && !BURROW_FUNC_IS_NIL(c->c))
            return c->c;
    return zip_compressor(method);
}

/* Appends n bytes to the header's own extra. */
static bool zip_header_append_extra(Alloc *a, ZipHeader *h, const Byte *p, Int n) {
    Slice *x = &h->fh.extra;
    if (x->len + n > h->extra_cap) {
        Int ncap = h->extra_cap == 0 ? 16 : h->extra_cap;
        while (ncap < x->len + n)
            ncap *= 2;
        Byte *np = (Byte *)mem_alloc_nozero(a, (size_t)ncap, 1);
        if (np == NULL)
            return false;
        if (x->len > 0)
            memcpy(np, x->p, (size_t)x->len);
        if (h->extra_cap > 0)
            mem_free(a, x->p, (size_t)h->extra_cap, 1);
        x->p = np;
        h->extra_cap = ncap;
    }
    if (n > 0)
        memcpy((Byte *)x->p + x->len, p, (size_t)n);
    x->len += n;
    x->cap = h->extra_cap;
    return true;
}

static void zip_header_free(Alloc *a, ZipHeader *h) {
    if (h->own_name)
        zip_str_free(a, h->fh.name);
    if (h->own_comment)
        zip_str_free(a, h->fh.comment);
    if (h->extra_cap > 0)
        mem_free(a, h->fh.extra.p, (size_t)h->extra_cap, 1);
    mem_free(a, h, sizeof *h, _Alignof(ZipHeader));
}

/* A copy of s, or the start of prev when s is the same bytes as that, so an
 * archive of many files with one long name or comment holds it once. *own says
 * which. Headers are only given back all together, so a string shared with an
 * earlier header lives as long as it does. */
static Str zip_dup_or_share(Alloc *a, Str prev, Str s, bool *own, bool *ok) {
    *own = false;
    if (s.len > 0 && s.len <= prev.len && memcmp(prev.p, s.p, (size_t)s.len) == 0)
        return (Str){prev.p, s.len};
    Str r = zip_dup(a, s, ok);
    *own = r.len > 0;
    return r;
}

/* A header that is a copy of fh, with its own extra, and strings of its own or
 * shared with prev, the header before it. */
static ZipHeader *zip_header_new(Alloc *a, const ZipFileHeader *fh,
                                 const ZipHeader *prev) {
    ZipHeader *h = (ZipHeader *)mem_alloc(a, sizeof *h, _Alignof(ZipHeader));
    if (h == NULL)
        return NULL;
    bool ok = true;
    h->fh = *fh;
    Str empty = {zip_empty, 0};
    h->fh.name = zip_dup_or_share(a, prev != NULL ? prev->fh.name : empty, fh->name,
                                  &h->own_name, &ok);
    h->fh.comment = zip_dup_or_share(a, prev != NULL ? prev->fh.comment : empty,
                                     fh->comment, &h->own_comment, &ok);
    h->fh.extra = slice_nil(TYPE_BYTE);
    if (!ok ||
        !zip_header_append_extra(a, h, (const Byte *)fh->extra.p, fh->extra.len)) {
        zip_header_free(a, h);
        return NULL;
    }
    if (h->extra_cap == 0)
        h->fh.extra = slice_nil(TYPE_BYTE);
    return h;
}

static bool zip_dir_push(ZipWriter *w, ZipHeader *h) {
    if (w->dir_len == w->dir_cap) {
        Int ncap = w->dir_cap == 0 ? 8 : w->dir_cap * 2;
        void *np =
            mem_realloc(w->a, w->dir, (size_t)w->dir_cap * sizeof(ZipHeader *),
                        (size_t)ncap * sizeof(ZipHeader *), _Alignof(ZipHeader *));
        if (np == NULL)
            return false;
        w->dir = (ZipHeader **)np;
        w->dir_cap = ncap;
    }
    w->dir[w->dir_len++] = h;
    return true;
}

static Error zip_write_data_descriptor(ZipFileWriter *fw) {
    const ZipFileHeader *h = &fw->header->fh;
    if (!zip_has_data_descriptor(h))
        return BURROW_NO_ERROR;
    Byte buf[ZIP_DATA_DESCRIPTOR64_LEN];
    Byte *b = zip_put32(buf, ZIP_DATA_DESCRIPTOR_SIGNATURE);
    b = zip_put32(b, h->crc32);
    if (h->compressed_size64 > ZIP_UINT32_MAX ||
        h->uncompressed_size64 > ZIP_UINT32_MAX) {
        b = zip_put64(b, h->compressed_size64);
        b = zip_put64(b, h->uncompressed_size64);
    } else {
        b = zip_put32(b, h->compressed_size);
        b = zip_put32(b, h->uncompressed_size);
    }
    Error e = BURROW_NO_ERROR;
    fw->zipw.vt->write(fw->zipw.data, zip_bytes(buf, (Int)(b - buf)), &e);
    return e;
}

static Error zip_file_writer_close(ZipFileWriter *fw) {
    if (fw->closed)
        return zip_err_file_closed_twice;
    fw->closed = true;
    if (fw->header->raw)
        return zip_write_data_descriptor(fw);
    fw->comp_open = false;
    Error e = fw->comp.vt->closer.close(fw->comp.data);
    if (BURROW_FAILED(e))
        return e;
    ZipFileHeader *h = &fw->header->fh;
    h->crc32 = fw->crc32;
    h->compressed_size64 = (uint64_t)fw->comp_count.count;
    h->uncompressed_size64 = (uint64_t)fw->raw_count.count;
    if (h->compressed_size64 > ZIP_UINT32_MAX ||
        h->uncompressed_size64 > ZIP_UINT32_MAX) {
        h->compressed_size = ZIP_UINT32_MAX;
        h->uncompressed_size = ZIP_UINT32_MAX;
        h->reader_version = ZIP_VERSION45; /* ZIP64 needs 4.5 */
    } else {
        h->compressed_size = (uint32_t)h->compressed_size64;
        h->uncompressed_size = (uint32_t)h->uncompressed_size64;
    }
    return zip_write_data_descriptor(fw);
}

static Int zip_file_writer_write(void *self, Slice p, Error *err) {
    ZipFileWriter *fw = (ZipFileWriter *)self;
    if (fw->closed) {
        *err = zip_err_write_closed;
        return 0;
    }
    if (fw->header->raw) {
        *err = BURROW_NO_ERROR;
        return fw->zipw.vt->write(fw->zipw.data, p, err);
    }
    if (!fw->fake_crc32)
        fw->crc32 = crc32_update(fw->crc32, crc32_ieee_table, p);
    return zip_count_write(&fw->raw_count, p, err);
}

static const IoWriterVT zip_file_writer_vt = {&zip_file_writer_desc,
                                              zip_file_writer_write};

void burrow__zip_file_writer_fake_crc32(IoWriter w) {
    if (w.vt == &zip_file_writer_vt)
        ((ZipFileWriter *)w.data)->fake_crc32 = true;
}

static Int zip_dir_write(void *self, Slice p, Error *err) {
    (void)self;
    if (p.len == 0) {
        *err = BURROW_NO_ERROR;
        return 0;
    }
    *err = zip_err_write_dir;
    return 0;
}

static const IoWriterVT zip_dir_writer_vt = {&zip_dir_writer_desc, zip_dir_write};

/* Go also refuses the FileHeader it was given for the file before, since its
 * writer keeps that pointer and fills in the sizes when the file is done. This
 * writer keeps a copy, so a header on the stack can be used again for the next
 * file, and nothing is checked. */
static Error zip_prepare(ZipWriter *w) {
    if (w->last != NULL && !w->last->closed) {
        Error e = zip_file_writer_close(w->last);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

static Error zip_write_header(ZipWriter *w, const ZipHeader *h) {
    const ZipFileHeader *fh = &h->fh;
    if (fh->name.len > (Int)ZIP_UINT16_MAX)
        return burrow__zip_err_long_name;
    if (fh->extra.len > (Int)ZIP_UINT16_MAX)
        return burrow__zip_err_long_extra;

    Byte zip64[20];
    Int zip64_len = 0;
    uint16_t reader_version = fh->reader_version;
    bool no_descriptor = h->raw && !zip_has_data_descriptor(fh);
    if (no_descriptor && (fh->compressed_size64 > ZIP_UINT32_MAX ||
                          fh->uncompressed_size64 > ZIP_UINT32_MAX)) {
        if (reader_version < ZIP_VERSION45)
            reader_version = ZIP_VERSION45;
        Byte *b = zip_put16(zip64, ZIP_EXTRA_ZIP64);
        b = zip_put16(b, 16);
        b = zip_put64(b, fh->uncompressed_size64);
        zip_put64(b, fh->compressed_size64);
        zip64_len = 20;
    }

    Byte buf[ZIP_FILE_HEADER_LEN];
    Byte *b = zip_put32(buf, ZIP_FILE_HEADER_SIGNATURE);
    b = zip_put16(b, reader_version);
    b = zip_put16(b, fh->flags);
    b = zip_put16(b, fh->method);
    b = zip_put16(b, fh->modified_time);
    b = zip_put16(b, fh->modified_date);
    if (no_descriptor) {
        b = zip_put32(b, fh->crc32);
        if (zip64_len > 0) {
            b = zip_put32(b, ZIP_UINT32_MAX);
            b = zip_put32(b, ZIP_UINT32_MAX);
        } else {
            b = zip_put32(b, (uint32_t)fh->compressed_size64);
            b = zip_put32(b, (uint32_t)fh->uncompressed_size64);
        }
    } else {
        b = zip_put32(b, 0);
        b = zip_put32(b, 0);
        b = zip_put32(b, 0);
    }
    b = zip_put16(b, (uint16_t)fh->name.len);
    zip_put16(b, (uint16_t)(fh->extra.len + zip64_len));
    Error e = BURROW_NO_ERROR;
    zip_cw_write(w, buf, (Int)sizeof buf, &e);
    if (BURROW_OK(e))
        zip_cw_write(w, fh->name.p, fh->name.len, &e);
    if (BURROW_OK(e))
        zip_cw_write(w, (const Byte *)fh->extra.p, fh->extra.len, &e);
    if (BURROW_OK(e))
        zip_cw_write(w, zip64, zip64_len, &e);
    return e;
}

static IoWriter zip_create_header(ZipWriter *w, ZipFileHeader *fh, Error *err) {
    Error e = zip_prepare(w);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (IoWriter){NULL, NULL};
    }

    bool valid1, require1, valid2, require2;
    burrow__zip_detect_utf8(fh->name, &valid1, &require1);
    burrow__zip_detect_utf8(fh->comment, &valid2, &require2);
    if (fh->non_utf8)
        fh->flags = (uint16_t)(fh->flags & ~0x800U);
    else if ((require1 || require2) && valid1 && valid2)
        fh->flags |= 0x800;

    /* Keep the compatibility byte. */
    fh->creator_version = (uint16_t)((fh->creator_version & 0xff00) | ZIP_VERSION20);
    fh->reader_version = ZIP_VERSION20;

    Byte mbuf[9];
    bool ext_time = !time_is_zero(fh->modified);
    if (ext_time) {
        /* Contrary to the FileHeader.SetModTime method, the time is not
         * converted to UTC here, so that the MS-DOS fields hold local time
         * in whatever zone modified was given in. */
        burrow__zip_time_to_msdos_time(fh->modified, &fh->modified_date,
                                       &fh->modified_time);
        Byte *b = zip_put16(mbuf, ZIP_EXTRA_EXT_TIME);
        b = zip_put16(b, 5);
        *b++ = 1; /* the flags: ModTime */
        zip_put32(b, (uint32_t)time_unix(fh->modified));
    }

    bool is_dir = zip_has_slash_suffix(fh->name);
    ZipCompressor comp = {NULL, NULL};
    if (is_dir) {
        fh->method = ZIP_STORE;
        fh->flags = (uint16_t)(fh->flags & ~0x8U); /* no data descriptor */
        fh->compressed_size = 0;
        fh->compressed_size64 = 0;
        fh->uncompressed_size = 0;
        fh->uncompressed_size64 = 0;
    } else {
        fh->flags |= 0x8; /* a data descriptor follows */
    }

    ZipHeader *h =
        zip_header_new(w->a, fh, w->dir_len > 0 ? w->dir[w->dir_len - 1] : NULL);
    if (h == NULL || (ext_time && !zip_header_append_extra(w->a, h, mbuf, 9))) {
        if (h != NULL)
            zip_header_free(w->a, h);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriter){NULL, NULL};
    }
    h->offset = (uint64_t)w->cw.count;

    IoWriter ow = {&zip_dir_writer_vt, NULL};
    ZipFileWriter *fw = NULL;
    if (!is_dir) {
        comp = zip_writer_compressor(w, fh->method);
        if (BURROW_FUNC_IS_NIL(comp)) {
            zip_header_free(w->a, h);
            BURROW_OUT(err, zip_err_algorithm);
            return (IoWriter){NULL, NULL};
        }
        fw = &h->fw;
        fw->header = h;
        fw->zipw = zip_count_as_writer(&w->cw);
        fw->comp_count.w = fw->zipw;
        e = BURROW_NO_ERROR;
        fw->comp = comp.f(comp.env, w->a, zip_count_as_writer(&fw->comp_count), &e);
        if (BURROW_FAILED(e) || fw->comp.vt == NULL) {
            zip_header_free(w->a, h);
            BURROW_OUT(err, BURROW_FAILED(e) ? e : burrow_err_out_of_memory);
            return (IoWriter){NULL, NULL};
        }
        fw->comp_open = true;
        fw->raw_count.w = (IoWriter){&fw->comp.vt->writer, fw->comp.data};
        h->has_writer = true;
        ow = (IoWriter){&zip_file_writer_vt, fw};
    }
    if (!zip_dir_push(w, h)) {
        if (fw != NULL) {
            fw->comp_count.discard = true;
            (void)fw->comp.vt->closer.close(fw->comp.data);
        }
        zip_header_free(w->a, h);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriter){NULL, NULL};
    }
    e = zip_write_header(w, h);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (IoWriter){NULL, NULL};
    }
    w->last = fw;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ow;
}

IoWriter zip_writer_create_header(ZipWriter *w, ZipFileHeader *fh, Error *err) {
    return zip_create_header(w, fh, err);
}

IoWriter zip_writer_create(ZipWriter *w, Str name, Error *err) {
    ZipFileHeader fh;
    memset(&fh, 0, sizeof fh);
    fh.name = name;
    fh.comment = (Str){zip_empty, 0};
    fh.extra = slice_nil(TYPE_BYTE);
    fh.method = ZIP_DEFLATE;
    return zip_create_header(w, &fh, err);
}

static IoWriter zip_create_raw(ZipWriter *w, ZipFileHeader *fh, Error *err) {
    Error e = zip_prepare(w);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (IoWriter){NULL, NULL};
    }
    fh->compressed_size = (uint32_t)zip_min64(fh->compressed_size64, ZIP_UINT32_MAX);
    fh->uncompressed_size =
        (uint32_t)zip_min64(fh->uncompressed_size64, ZIP_UINT32_MAX);

    ZipHeader *h =
        zip_header_new(w->a, fh, w->dir_len > 0 ? w->dir[w->dir_len - 1] : NULL);
    if (h == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriter){NULL, NULL};
    }
    h->offset = (uint64_t)w->cw.count;
    h->raw = true;
    if (!zip_dir_push(w, h)) {
        zip_header_free(w->a, h);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (IoWriter){NULL, NULL};
    }
    e = zip_write_header(w, h);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (IoWriter){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (zip_has_slash_suffix(fh->name)) {
        w->last = NULL;
        return (IoWriter){&zip_dir_writer_vt, NULL};
    }
    ZipFileWriter *fw = &h->fw;
    fw->header = h;
    fw->zipw = zip_count_as_writer(&w->cw);
    h->has_writer = true;
    w->last = fw;
    return (IoWriter){&zip_file_writer_vt, fw};
}

IoWriter zip_writer_create_raw(ZipWriter *w, ZipFileHeader *fh, Error *err) {
    return zip_create_raw(w, fh, err);
}

Error zip_writer_copy(ZipWriter *w, ZipFile *f) {
    Error e;
    IoSectionReader r = zip_file_open_raw(f, &e);
    if (BURROW_FAILED(e))
        return e;
    ZipFileHeader fh = f->file_header;
    IoWriter fw = zip_create_raw(w, &fh, &e);
    if (BURROW_FAILED(e))
        return e;
    io_copy(w->a, fw, io_section_reader_as_io_reader(&r), &e);
    return e;
}

typedef struct ZipAddFs {
    ZipWriter *w;
    Fs fsys;
    Arena scratch;
} ZipAddFs;

static Error zip_add_fs_entry(ZipAddFs *env, Alloc *a, Str name, FsDirEntry d) {
    Error e = BURROW_NO_ERROR;
    FsFileInfo info = d.vt->info(d.data, a, &e);
    if (BURROW_FAILED(e))
        return e;
    bool is_dir = d.vt->is_dir(d.data);
    if (!is_dir && !fs_file_mode_is_regular(info.vt->mode(info.data)))
        return zip_err_non_regular;
    ZipFileHeader *h = zip_file_info_header(a, info, &e);
    if (BURROW_FAILED(e))
        return e;
    h->name = name;
    if (is_dir) {
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)name.len + 1, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        memcpy(p, name.p, (size_t)name.len);
        p[name.len] = '/';
        h->name = (Str){p, name.len + 1};
    }
    h->method = ZIP_DEFLATE;
    IoWriter fw = zip_create_header(env->w, h, &e);
    if (BURROW_FAILED(e))
        return e;
    if (is_dir)
        return BURROW_NO_ERROR;
    FsFile f = env->fsys.vt->open(env->fsys.data, a, name, &e);
    if (BURROW_FAILED(e))
        return e;
    io_copy(a, fw, fs_file_as_io_reader(f), &e);
    Error ce = f.vt->read_closer.closer.close(f.data);
    (void)ce;
    return e;
}

static Error zip_add_fs_visit(void *envp, Str name, FsDirEntry d, Error err) {
    ZipAddFs *env = (ZipAddFs *)envp;
    if (BURROW_FAILED(err))
        return err;
    if (str_eq(name, BURROW_S(".")))
        return BURROW_NO_ERROR;
    Error e = zip_add_fs_entry(env, arena_allocator(&env->scratch), name, d);
    arena_reset(&env->scratch);
    return e;
}

/* Each entry's scratch comes from an arena that is emptied after it, which
 * takes care of the headers and names the FS hands out too. */
Error zip_writer_add_fs(ZipWriter *w, Fs fsys) {
    ZipAddFs env;
    env.w = w;
    env.fsys = fsys;
    arena_init(&env.scratch, w->a, 0);
    Error e = fs_walk_dir(w->a, fsys, BURROW_S("."),
                          BURROW_FN(FsWalkDirFunc, zip_add_fs_visit, &env));
    arena_free(&env.scratch);
    return e;
}

Error zip_writer_close(ZipWriter *w) {
    if (w->last != NULL && !w->last->closed) {
        Error e = zip_file_writer_close(w->last);
        if (BURROW_FAILED(e))
            return e;
        w->last = NULL;
    }
    if (w->closed)
        return zip_err_closed_twice;
    w->closed = true;

    Error e = BURROW_NO_ERROR;
    int64_t start = w->cw.count;
    bool used_zip64 = false;
    for (Int i = 0; i < w->dir_len; i++) {
        ZipHeader *h = w->dir[i];
        ZipFileHeader *fh = &h->fh;
        uint16_t reader_version = fh->reader_version;
        if (fh->compressed_size64 >= ZIP_UINT32_MAX ||
            fh->uncompressed_size64 >= ZIP_UINT32_MAX || h->offset >= ZIP_UINT32_MAX) {
            used_zip64 = true;
            if (reader_version < ZIP_VERSION45)
                reader_version = ZIP_VERSION45;
            Byte buf[28];
            uint16_t size = 0;
            Byte *b = zip_put16(buf, ZIP_EXTRA_ZIP64);
            b += 2; /* the size, filled out below */
            if (fh->uncompressed_size64 >= ZIP_UINT32_MAX) {
                b = zip_put64(b, fh->uncompressed_size64);
                size += 8;
            }
            if (fh->compressed_size64 >= ZIP_UINT32_MAX) {
                b = zip_put64(b, fh->compressed_size64);
                size += 8;
            }
            if (h->offset >= ZIP_UINT32_MAX) {
                zip_put64(b, h->offset);
                size += 8;
            }
            zip_put16(buf + 2, size);
            if (!zip_header_append_extra(w->a, h, buf, 4 + (Int)size))
                return burrow_err_out_of_memory;
        }

        Byte buf[ZIP_DIRECTORY_HEADER_LEN];
        Byte *b = zip_put32(buf, ZIP_DIRECTORY_HEADER_SIGNATURE);
        b = zip_put16(b, fh->creator_version);
        b = zip_put16(b, reader_version);
        b = zip_put16(b, fh->flags);
        b = zip_put16(b, fh->method);
        b = zip_put16(b, fh->modified_time);
        b = zip_put16(b, fh->modified_date);
        b = zip_put32(b, fh->crc32);
        b = zip_put32(b, (uint32_t)zip_min64(fh->compressed_size64, ZIP_UINT32_MAX));
        b = zip_put32(b, (uint32_t)zip_min64(fh->uncompressed_size64, ZIP_UINT32_MAX));
        b = zip_put16(b, (uint16_t)fh->name.len);
        b = zip_put16(b, (uint16_t)fh->extra.len);
        b = zip_put16(b, (uint16_t)fh->comment.len);
        b = zip_put32(b, 0); /* disk number start and internal attributes */
        b = zip_put32(b, fh->external_attrs);
        zip_put32(b, (uint32_t)zip_min64(h->offset, ZIP_UINT32_MAX));
        zip_cw_write(w, buf, (Int)sizeof buf, &e);
        if (BURROW_OK(e))
            zip_cw_write(w, fh->name.p, fh->name.len, &e);
        if (BURROW_OK(e))
            zip_cw_write(w, (const Byte *)fh->extra.p, fh->extra.len, &e);
        if (BURROW_OK(e))
            zip_cw_write(w, fh->comment.p, fh->comment.len, &e);
        if (BURROW_FAILED(e))
            return e;
    }
    int64_t end = w->cw.count;

    uint64_t records = (uint64_t)w->dir_len;
    uint64_t size = (uint64_t)(end - start);
    uint64_t offset = (uint64_t)start;

    if (w->test_hook_close_size_offset != NULL)
        w->test_hook_close_size_offset(w->test_hook_env, size, offset);

    if (used_zip64 || records >= ZIP_UINT16_MAX || size >= ZIP_UINT32_MAX ||
        offset >= ZIP_UINT32_MAX) {
        Byte buf[ZIP_DIRECTORY64_END_LEN + ZIP_DIRECTORY64_LOC_LEN];
        Byte *b = zip_put32(buf, ZIP_DIRECTORY64_END_SIGNATURE);
        /* the length less the signature and the length itself */
        b = zip_put64(b, ZIP_DIRECTORY64_END_LEN - 12);
        b = zip_put16(b, ZIP_VERSION45); /* version made by */
        b = zip_put16(b, ZIP_VERSION45); /* version needed to extract */
        b = zip_put32(b, 0);             /* number of this disk */
        b = zip_put32(b, 0);             /* the disk the central directory starts on */
        b = zip_put64(b, records);       /* entries on this disk */
        b = zip_put64(b, records);       /* entries in all */
        b = zip_put64(b, size);          /* size of the central directory */
        b = zip_put64(b, offset);        /* where it starts */

        b = zip_put32(b, ZIP_DIRECTORY64_LOC_SIGNATURE);
        b = zip_put32(b, 0);             /* the disk the zip64 end record is on */
        b = zip_put64(b, (uint64_t)end); /* where the zip64 end record is */
        zip_put32(b, 1);                 /* total number of disks */
        zip_cw_write(w, buf, (Int)sizeof buf, &e);
        if (BURROW_FAILED(e))
            return e;
    }

    Byte buf[ZIP_DIRECTORY_END_LEN];
    Byte *b = zip_put32(buf, ZIP_DIRECTORY_END_SIGNATURE);
    b = zip_put32(b, 0); /* disk number and the directory's first disk */
    b = zip_put16(b, (uint16_t)zip_min64(ZIP_UINT16_MAX, records));
    b = zip_put16(b, (uint16_t)zip_min64(ZIP_UINT16_MAX, records));
    b = zip_put32(b, (uint32_t)zip_min64(ZIP_UINT32_MAX, size));
    b = zip_put32(b, (uint32_t)zip_min64(ZIP_UINT32_MAX, offset));
    zip_put16(b, (uint16_t)w->comment.len);
    zip_cw_write(w, buf, (Int)sizeof buf, &e);
    if (BURROW_OK(e))
        zip_cw_write(w, w->comment.p, w->comment.len, &e);
    if (BURROW_FAILED(e))
        return e;
    return bufio_writer_flush(w->bw);
}

void zip_writer_free(ZipWriter *w) {
    if (w == NULL)
        return;
    Alloc *a = w->a;
    for (Int i = 0; i < w->dir_len; i++) {
        ZipHeader *h = w->dir[i];
        if (h->has_writer && h->fw.comp_open) {
            /* A compressor that was never closed holds memory, and closing it
             * is the only way to get that back. Nothing reads what it writes
             * now. */
            h->fw.comp_count.discard = true;
            (void)h->fw.comp.vt->closer.close(h->fw.comp.data);
        }
        zip_header_free(a, h);
    }
    if (w->dir_cap > 0)
        mem_free(a, w->dir, (size_t)w->dir_cap * sizeof(ZipHeader *),
                 _Alignof(ZipHeader *));
    ZipCompressorEntry *c = w->compressors;
    while (c != NULL) {
        ZipCompressorEntry *next = c->next;
        mem_free(a, c, sizeof *c, _Alignof(ZipCompressorEntry));
        c = next;
    }
    zip_str_free(a, w->comment);
    bufio_writer_free(w->bw);
    mem_free(a, w, sizeof *w, _Alignof(ZipWriter));
}
