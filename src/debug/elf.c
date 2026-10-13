/* debug/elf: opening a file, its sections and segments, the symbol tables, the
 * version tables, the dynamic section, and the DWARF in it.
 *
 * Derived from Go's src/debug/elf/file.go and reader.go, and the parts of
 * src/internal/saferio they use.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/elf.h"

#include "../compress/zstd_internal.h"
#include "burrow/compress/zlib.h"
#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* saferio's chunk: the most read up front for a length a file claims. */
#define ELF_CHUNK ((uint64_t)10 << 20)

/* ---------------------------------------------------------------- helpers */

static uint16_t elf_u16(bool big, const Byte *p) {
    if (big)
        return (uint16_t)((unsigned)p[0] << 8 | p[1]);
    return (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static uint32_t elf_u32(bool big, const Byte *p) {
    if (big)
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint64_t elf_u64(bool big, const Byte *p) {
    if (big)
        return (uint64_t)elf_u32(true, p) << 32 | elf_u32(true, p + 4);
    return (uint64_t)elf_u32(false, p + 4) << 32 | elf_u32(false, p);
}

static void elf_put_u32(bool big, Byte *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        int sh = big ? 24 - 8 * i : 8 * i;
        p[i] = (Byte)(v >> sh);
    }
}

static void elf_put_u64(bool big, Byte *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        int sh = big ? 56 - 8 * i : 8 * i;
        p[i] = (Byte)(v >> sh);
    }
}

static bool elf_big(const ElfFile *f) {
    return f->file_header.data == ELF_ELFDATA2MSB;
}

static Slice elf_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static Int elf_put(Byte *p, Int at, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p + at, s.p, (size_t)s.len);
    return at + s.len;
}

/* v in decimal, at the end of buf. */
static Str elf_dec(Byte buf[24], uint64_t v, bool neg) {
    Int i = 24;
    do {
        buf[--i] = (Byte)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);
    if (neg)
        buf[--i] = '-';
    return str_from_bytes(buf + i, 24 - i);
}

static Str elf_udec(Byte buf[24], uint64_t v) {
    return elf_dec(buf, v, false);
}

static Str elf_idec(Byte buf[24], int64_t v) {
    return v < 0 ? elf_dec(buf, 0 - (uint64_t)v, true)
                 : elf_dec(buf, (uint64_t)v, false);
}

static void elf_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* getString: the NUL terminated string at start in a string table, pointing
 * into it. */
static bool elf_get_string(Slice section, uint64_t start, Str *out) {
    *out = BURROW_STR_EMPTY;
    if (start >= (uint64_t)section.len)
        return false;
    const Byte *p = (const Byte *)section.p;
    const Byte *end =
        (const Byte *)memchr(p + start, 0, (size_t)((uint64_t)section.len - start));
    if (end == NULL)
        return false;
    *out = str_from_bytes(p + start, (Int)(end - (p + start)));
    return true;
}

/* ------------------------------------------------------------------ errors */

#define ELF_ERR(name, text)                                                            \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

BURROW_SENTINEL_ERROR(elf_err_no_symbols, "no symbol section");

ELF_ERR(elf_err_strtab_link, "section has invalid string table link");
ELF_ERR(elf_err_sym32_len, "length of symbol section is not a multiple of SymSize");
ELF_ERR(elf_err_sym64_len, "length of symbol section is not a multiple of Sym64Size");
ELF_ERR(elf_err_not_implemented, "not implemented");
ELF_ERR(elf_err_reloc_not_implemented, "applyRelocations: not implemented");
ELF_ERR(elf_err_rel8_len, "length of relocation section is not a multiple of 8");
ELF_ERR(elf_err_rel12_len, "length of relocation section is not a multiple of 12");
ELF_ERR(elf_err_rel24_len, "length of relocation section is not a multiple of 24");
ELF_ERR(elf_err_no_verdef_table, "DynamicVersions: missing version table");
ELF_ERR(elf_err_no_verneed_table, "DynamicVersionNeeds: missing version table");
ELF_ERR(elf_err_dyn_len,
        "length of dynamic section is not a multiple of dynamic entry size");
ELF_ERR(elf_err_nobits, "unexpected read from SHT_NOBITS section");

/* The public struct first, so that errors_as hands back an ElfFormatError,
 * and the message after it. */
typedef struct ElfFormatErrorBox {
    ElfFormatError e;
    Str message;
} ElfFormatErrorBox;

/* FormatError.Error into p, or only its length when p is NULL. With no value
 * Go puts nothing between the message and "in", and neither does this. */
static Int elf_format_error_put(Byte *p, const ElfFormatError *e) {
    Int n = elf_put(p, 0, e->msg);
    if (e->has_val) {
        n = elf_put(p, n, BURROW_S(" '"));
        n = elf_put(p, n, e->val);
        n = elf_put(p, n, BURROW_S("' "));
    }
    n = elf_put(p, n, BURROW_S("in record at byte "));
    uint64_t v = (uint64_t)e->off;
    if (e->off < 0) {
        n = elf_put(p, n, BURROW_S("-"));
        v = 0 - v;
    }
    n = elf_put(p, n, BURROW_S("0x"));
    Byte hex[16];
    Int nh = 0;
    do {
        hex[nh++] = (Byte) "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    while (nh > 0) {
        if (p != NULL)
            p[n] = hex[nh - 1];
        n++;
        nh--;
    }
    return n;
}

Str elf_format_error_error(const ElfFormatError *e, Alloc *a) {
    if (e == NULL)
        elf_nil_panic();
    Int n = elf_format_error_put(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    elf_format_error_put(p, e);
    return str_from_bytes(p, n);
}

static Str elf_format_error_m_error(ElfFormatError *self) {
    return elf_format_error_error(self, error_allocator());
}

#define ELF_FORMAT_ERROR_SIG_ERROR(IN, OUT) OUT(Str)
#define ELF_FORMAT_ERROR_METHODS(M, T)                                                 \
    M(T, Error, elf_format_error_m_error, ELF_FORMAT_ERROR_SIG_ERROR)

BURROW_METHODS_DEFINE(ElfFormatError, ELF_FORMAT_ERROR_METHODS);

const Type burrow_type_ElfFormatError = {
    {(const Byte *)"FormatError", 11},
    {(const Byte *)"debug/elf", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(ElfFormatError),
    (uint16_t)_Alignof(ElfFormatError),
    0,
    (uint16_t)(sizeof(burrow__methods_ElfFormatError) /
               sizeof(burrow__methods_ElfFormatError[0])),
    NULL,
    burrow__methods_ElfFormatError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str elf_format_error_message(const void *self) {
    return ((const ElfFormatErrorBox *)self)->message;
}

static Error elf_format_error_clone(const void *self, Alloc *a);

static const ErrorVT elf_format_error_vt = {
    &burrow_type_ElfFormatError, elf_format_error_message, NULL, NULL, NULL, NULL,
    elf_format_error_clone,
};

/* One allocation: the box, then the message, the value and the text. */
static Error elf_format_error_build(Alloc *a, int64_t off, Str msg, Str val,
                                    bool has_val) {
    if (!has_val)
        val = BURROW_STR_EMPTY;
    ElfFormatError e = {off, msg, val, has_val};
    Int tlen = elf_format_error_put(NULL, &e);
    size_t size =
        sizeof(ElfFormatErrorBox) + (size_t)msg.len + (size_t)val.len + (size_t)tlen;
    ElfFormatErrorBox *b =
        (ElfFormatErrorBox *)mem_alloc_nozero(a, size, _Alignof(ElfFormatErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.off = off;
    b->e.has_val = has_val;
    b->e.msg = str_from_bytes(p, msg.len);
    p += elf_put(p, 0, msg);
    b->e.val = str_from_bytes(p, val.len);
    p += elf_put(p, 0, val);
    elf_format_error_put(p, &b->e);
    b->message = str_from_bytes(p, tlen);
    return (Error){&elf_format_error_vt, b};
}

static Error elf_format_error_clone(const void *self, Alloc *a) {
    const ElfFormatError *e = (const ElfFormatError *)self;
    return elf_format_error_build(a, e->off, e->msg, e->val, e->has_val);
}

/* &FormatError{off, msg, val}, with val already formatted as %v would. */
static Error elf_fe(int64_t off, Str msg, Str val) {
    return elf_format_error_build(error_allocator(), off, msg, val, true);
}

static Error elf_fe_nil(int64_t off, Str msg) {
    return elf_format_error_build(error_allocator(), off, msg, BURROW_STR_EMPTY, false);
}

static Error elf_fe_u(int64_t off, Str msg, uint64_t v) {
    Byte buf[24];
    return elf_fe(off, msg, elf_udec(buf, v));
}

static Error elf_fe_i(int64_t off, Str msg, int64_t v) {
    Byte buf[24];
    return elf_fe(off, msg, elf_idec(buf, v));
}

/* ------------------------------------------------------------------ types */

bool elf_version_index_is_hidden(ElfVersionIndex vi) {
    return (vi & 0x8000) != 0;
}

uint16_t elf_version_index_index(ElfVersionIndex vi) {
    return (uint16_t)(vi & 0x7fff);
}

static bool elf_version_index_m_is_hidden(ElfVersionIndex *self) {
    return elf_version_index_is_hidden(*self);
}

static uint16_t elf_version_index_m_index(ElfVersionIndex *self) {
    return elf_version_index_index(*self);
}

#define ELF_VERSION_INDEX_SIG_BOOL(IN, OUT) OUT(bool)
#define ELF_VERSION_INDEX_SIG_UINT16(IN, OUT) OUT(uint16_t)
#define ELF_VERSION_INDEX_METHODS(M, T)                                                \
    M(T, Index, elf_version_index_m_index, ELF_VERSION_INDEX_SIG_UINT16)               \
    M(T, IsHidden, elf_version_index_m_is_hidden, ELF_VERSION_INDEX_SIG_BOOL)

BURROW_METHODS_DEFINE(ElfVersionIndex, ELF_VERSION_INDEX_METHODS);

const Type burrow_type_ElfVersionIndex = {
    {(const Byte *)"VersionIndex", 12},
    {(const Byte *)"debug/elf", 9},
    KIND_UINT16,
    (uint32_t)sizeof(ElfVersionIndex),
    (uint16_t)_Alignof(ElfVersionIndex),
    0,
    (uint16_t)(sizeof(burrow__methods_ElfVersionIndex) /
               sizeof(burrow__methods_ElfVersionIndex[0])),
    NULL,
    burrow__methods_ElfVersionIndex,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

#define ELF_SYMBOL_FIELDS(F, T)                                                        \
    F(T, Str, name, Name, "")                                                          \
    F(T, uint8_t, info, Info, "")                                                      \
    F(T, uint8_t, other, Other, "")                                                    \
    F(T, bool, has_version, HasVersion, "")                                            \
    F(T, ElfVersionIndex, version_index, VersionIndex, "")                             \
    F(T, ElfSectionIndex, section, Section, "")                                        \
    F(T, uint64_t, value, Value, "")                                                   \
    F(T, uint64_t, size, Size, "")                                                     \
    F(T, Str, version, Version, "")                                                    \
    F(T, Str, library, Library, "")

#define ELF_IMPORTED_SYMBOL_FIELDS(F, T)                                               \
    F(T, Str, name, Name, "")                                                          \
    F(T, Str, version, Version, "")                                                    \
    F(T, Str, library, Library, "")

#define ELF_DYNAMIC_VERSION_FIELDS(F, T)                                               \
    F(T, Str, name, Name, "")                                                          \
    F(T, uint16_t, index, Index, "")                                                   \
    F(T, ElfDynamicVersionFlag, flags, Flags, "")                                      \
    F(T, ElfStrSlice, deps, Deps, "")

#define ELF_DYNAMIC_VERSION_DEP_FIELDS(F, T)                                           \
    F(T, ElfDynamicVersionFlag, flags, Flags, "")                                      \
    F(T, uint16_t, index, Index, "")                                                   \
    F(T, Str, dep, Dep, "")

#define ELF_DYNAMIC_VERSION_NEED_FIELDS(F, T)                                          \
    F(T, Str, name, Name, "")                                                          \
    F(T, ElfDynamicVersionDepSlice, needs, Needs, "")

BURROW_STRUCT_AS_DEFINE(ElfSymbol, ELF_SYMBOL_FIELDS);
BURROW_STRUCT_AS_DEFINE(ElfImportedSymbol, ELF_IMPORTED_SYMBOL_FIELDS);
BURROW_SLICE_TYPE_DEFINE(ElfStrSlice, Str);
BURROW_STRUCT_AS_DEFINE(ElfDynamicVersion, ELF_DYNAMIC_VERSION_FIELDS);
BURROW_STRUCT_AS_DEFINE(ElfDynamicVersionDep, ELF_DYNAMIC_VERSION_DEP_FIELDS);
BURROW_SLICE_TYPE_DEFINE(ElfDynamicVersionDepSlice, ElfDynamicVersionDep);
BURROW_STRUCT_AS_DEFINE(ElfDynamicVersionNeed, ELF_DYNAMIC_VERSION_NEED_FIELDS);

uint32_t elf_r_sym32(uint32_t info) {
    return info >> 8;
}

uint32_t elf_r_type32(uint32_t info) {
    return info & 0xff;
}

uint32_t elf_r_info32(uint32_t sym, uint32_t typ) {
    return sym << 8 | typ;
}

uint32_t elf_r_sym64(uint64_t info) {
    return (uint32_t)(info >> 32);
}

uint32_t elf_r_type64(uint64_t info) {
    return (uint32_t)info;
}

uint64_t elf_r_info(uint32_t sym, uint32_t typ) {
    return (uint64_t)sym << 32 | typ;
}

ElfSymBind elf_st_bind(uint8_t info) {
    return (ElfSymBind)(info >> 4);
}

ElfSymType elf_st_type(uint8_t info) {
    return (ElfSymType)(info & 0xf);
}

uint8_t elf_st_info(ElfSymBind bind, ElfSymType typ) {
    return (uint8_t)((unsigned)bind << 4 | ((unsigned)typ & 0xf));
}

ElfSymVis elf_st_visibility(uint8_t other) {
    return (ElfSymVis)(other & 3);
}

/* ---------------------------------------------------------------- saferio */

static bool elf_same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

/* saferio.ReadData: n bytes from r into a, read a chunk at a time when n is
 * large, so that a length that lies costs only the bytes that are there. */
static Slice elf_read_data(Alloc *a, IoReader r, uint64_t n, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    if (n > (uint64_t)INTPTR_MAX) {
        BURROW_OUT(err, io_err_unexpected_eof);
        return none;
    }
    Error e = BURROW_NO_ERROR;
    if (n < ELF_CHUNK) {
        Byte *buf = (Byte *)mem_alloc_nozero(a, n == 0 ? 1 : (size_t)n, 1);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        io_read_full(r, elf_bytes(buf, (Int)n), &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, n == 0 ? 1 : (size_t)n, 1);
            BURROW_OUT(err, e);
            return none;
        }
        BURROW_OUT(err, BURROW_NO_ERROR);
        return elf_bytes(buf, (Int)n);
    }
    Byte *buf = NULL;
    uint64_t len = 0, cap = 0;
    while (n > 0) {
        uint64_t next = n > ELF_CHUNK ? ELF_CHUNK : n;
        if (len + next > cap) {
            uint64_t ncap = cap == 0 ? ELF_CHUNK : cap * 2;
            if (ncap < len + next)
                ncap = len + next;
            if (ncap > len + n)
                ncap = len + n;
            Byte *nb = (Byte *)mem_realloc(a, buf, (size_t)cap, (size_t)ncap, 1);
            if (nb == NULL) {
                mem_free(a, buf, (size_t)cap, 1);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return none;
            }
            buf = nb;
            cap = ncap;
        }
        io_read_full(r, elf_bytes(buf + len, (Int)next), &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, (size_t)cap, 1);
            if (len > 0 && elf_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            BURROW_OUT(err, e);
            return none;
        }
        len += next;
        n -= next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {buf, (Int)len, (Int)cap, TYPE_BYTE};
    return out;
}

/* saferio.ReadDataAt, the same from off in a ReaderAt. */
static Slice elf_read_data_at(Alloc *a, IoReaderAt r, uint64_t n, int64_t off,
                              Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    if (n > (uint64_t)INTPTR_MAX) {
        BURROW_OUT(err, io_err_unexpected_eof);
        return none;
    }
    Error e = BURROW_NO_ERROR;
    if (n < ELF_CHUNK) {
        Byte *buf = (Byte *)mem_alloc(a, n == 0 ? 1 : (size_t)n, 1);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        r.vt->read_at(r.data, elf_bytes(buf, (Int)n), off, &e);
        if (BURROW_FAILED(e) && (!elf_same_error(e, io_eof) || n > 0)) {
            mem_free(a, buf, n == 0 ? 1 : (size_t)n, 1);
            BURROW_OUT(err, e);
            return none;
        }
        BURROW_OUT(err, BURROW_NO_ERROR);
        return elf_bytes(buf, (Int)n);
    }
    Byte *buf = NULL;
    uint64_t len = 0, cap = 0;
    while (n > 0) {
        uint64_t next = n > ELF_CHUNK ? ELF_CHUNK : n;
        if (len + next > cap) {
            uint64_t ncap = cap == 0 ? ELF_CHUNK : cap * 2;
            if (ncap < len + next)
                ncap = len + next;
            if (ncap > len + n)
                ncap = len + n;
            Byte *nb = (Byte *)mem_realloc(a, buf, (size_t)cap, (size_t)ncap, 1);
            if (nb == NULL) {
                mem_free(a, buf, (size_t)cap, 1);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return none;
            }
            buf = nb;
            cap = ncap;
        }
        r.vt->read_at(r.data, elf_bytes(buf + len, (Int)next), off, &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, (size_t)cap, 1);
            BURROW_OUT(err, e);
            return none;
        }
        len += next;
        n -= next;
        off += (int64_t)next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {buf, (Int)len, (Int)cap, TYPE_BYTE};
    return out;
}

/* saferio.SliceCapWithSize. */
static Int elf_slice_cap(uint64_t size, uint64_t c) {
    if (c > (uint64_t)INTPTR_MAX)
        return -1;
    if (size > 0 && c > UINT64_MAX / size)
        return -1;
    if (c * size > ELF_CHUNK) {
        c = ELF_CHUNK / size;
        if (c == 0)
            c = 1;
    }
    return (Int)c;
}

/* ---------------------------------------------------------------- readers
 *
 * Every reader Open hands out is one of these, allocated in the allocator Open
 * was given. A plain one is the section reader at its start, handed out with
 * io's own vtable so that it is an io.SectionReader as it is in Go. The other
 * two are reader.go's errorReader and readSeekerFromReader. */

enum { ELF_RD_SECTION, ELF_RD_ERROR, ELF_RD_DECOMPRESS };

/* What readSeekerFromReader's r holds: nothing yet, the decompressor, or an
 * errorReader. */
enum { ELF_R_NIL, ELF_R_READER, ELF_R_ERROR };

typedef struct ElfReaderBox {
    IoSectionReader sr;
    Alloc *a;
    uint8_t kind;
    uint8_t state;
    Error err;
    ElfSection *s;
    ElfCompressionType ct;
    IoReadCloser zr;
    ZstdReader *zs;
    int64_t size;
    int64_t offset;
} ElfReaderBox;

static const Type elf_error_reader_desc = {
    {(const Byte *)"errorReader", 11},
    {(const Byte *)"debug/elf", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(ElfReaderBox),
    (uint16_t)_Alignof(ElfReaderBox),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type elf_rsfr_desc = {
    {(const Byte *)"readSeekerFromReader", 20},
    {(const Byte *)"debug/elf", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(ElfReaderBox),
    (uint16_t)_Alignof(ElfReaderBox),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type elf_nobits_desc = {
    {(const Byte *)"nobitsSectionReader", 19},
    {(const Byte *)"debug/elf", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(ElfReaderBox),
    (uint16_t)_Alignof(ElfReaderBox),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* errorReader. A NULL self is the reader Open hands out when it could not
 * allocate one, which fails with burrow_err_out_of_memory. */
static Int elf_error_reader_read(void *self, Slice p, Error *err) {
    (void)p;
    const ElfReaderBox *b = (const ElfReaderBox *)self;
    BURROW_OUT(err, b == NULL ? burrow_err_out_of_memory : b->err);
    return 0;
}

static int64_t elf_error_reader_seek(void *self, int64_t offset, int whence,
                                     Error *err) {
    (void)offset;
    (void)whence;
    const ElfReaderBox *b = (const ElfReaderBox *)self;
    BURROW_OUT(err, b == NULL ? burrow_err_out_of_memory : b->err);
    return 0;
}

static const IoReadSeekerVT elf_error_reader_vt = {
    {&elf_error_reader_desc, elf_error_reader_read},
    {&elf_error_reader_desc, elf_error_reader_seek},
};

static Int elf_nobits_read_at(void *self, Slice p, int64_t off, Error *err) {
    (void)self;
    (void)p;
    (void)off;
    BURROW_OUT(err, elf_err_nobits);
    return 0;
}

static const IoReaderAtVT elf_nobits_vt = {&elf_nobits_desc, elf_nobits_read_at};

/* Closes and frees the decompressor, if there is one. */
static void elf_rsfr_drop(ElfReaderBox *b) {
    if (b->zr.vt != NULL) {
        zlib_reader_free(b->zr);
        b->zr.vt = NULL;
        b->zr.data = NULL;
    }
    if (b->zs != NULL) {
        burrow__zstd_reader_free(b->zs);
        b->zs = NULL;
    }
}

/* readSeekerFromReader.start, with reset inlined: a new decompressor over the
 * compressed bytes, which come after the header. */
static void elf_rsfr_start(ElfReaderBox *b) {
    elf_rsfr_drop(b);
    ElfSection *s = b->s;
    b->sr = io_new_section_reader(
        io_section_reader_as_io_reader_at(&s->sr), s->compression_offset,
        (int64_t)s->section_header.file_size - s->compression_offset);
    IoReader fr = io_section_reader_as_io_reader(&b->sr);
    Error e = BURROW_NO_ERROR;
    if (b->ct == ELF_COMPRESS_ZLIB) {
        b->zr = zlib_new_reader(b->a, fr, &e);
    } else {
        b->zs = burrow__zstd_new_reader(b->a, fr);
        if (b->zs == NULL)
            e = burrow_err_out_of_memory;
    }
    if (BURROW_FAILED(e)) {
        b->state = ELF_R_ERROR;
        b->err = e;
    } else {
        b->state = ELF_R_READER;
    }
    b->offset = 0;
}

static Int elf_rsfr_read(void *self, Slice p, Error *err) {
    ElfReaderBox *b = (ElfReaderBox *)self;
    if (b->state == ELF_R_NIL)
        elf_rsfr_start(b);
    if (b->state == ELF_R_ERROR) {
        BURROW_OUT(err, b->err);
        return 0;
    }
    Int n = 0;
    if (b->zr.vt != NULL)
        n = b->zr.vt->reader.read(b->zr.data, p, err);
    else
        n = burrow__zstd_reader_read(b->zs, p, err);
    b->offset += n;
    return n;
}

static int64_t elf_rsfr_seek(void *self, int64_t offset, int whence, Error *err) {
    ElfReaderBox *b = (ElfReaderBox *)self;
    int64_t noff = 0;
    switch (whence) {
    case BURROW_IO_SEEK_START:
        noff = offset;
        break;
    case BURROW_IO_SEEK_CURRENT:
        noff = b->offset + offset;
        break;
    case BURROW_IO_SEEK_END:
        noff = b->size + offset;
        break;
    default:
        BURROW_OUT(err, os_err_invalid);
        return 0;
    }
    if (noff == b->offset) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return noff;
    }
    if (noff < 0 || noff > b->size) {
        BURROW_OUT(err, os_err_invalid);
        return 0;
    }
    if (noff == 0) {
        b->state = ELF_R_NIL;
    } else if (noff == b->size) {
        b->state = ELF_R_ERROR;
        b->err = io_eof;
    } else {
        if (noff < b->offset)
            elf_rsfr_start(b);
        Byte buf[512];
        while (b->offset < noff) {
            Int n = (Int)sizeof buf;
            if (noff - b->offset < (int64_t)n)
                n = (Int)(noff - b->offset);
            Error e = BURROW_NO_ERROR;
            elf_rsfr_read(b, elf_bytes(buf, n), &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, e);
                return 0;
            }
        }
    }
    b->offset = noff;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return noff;
}

static const IoReadSeekerVT elf_rsfr_vt = {
    {&elf_rsfr_desc, elf_rsfr_read},
    {&elf_rsfr_desc, elf_rsfr_seek},
};

static ElfReaderBox *elf_new_reader_box(Alloc *a, uint8_t kind) {
    ElfReaderBox *b = (ElfReaderBox *)mem_alloc(a, sizeof *b, _Alignof(ElfReaderBox));
    if (b == NULL)
        return NULL;
    b->a = a;
    b->kind = kind;
    b->err = BURROW_NO_ERROR;
    return b;
}

static IoReadSeeker elf_oom_reader(void) {
    IoReadSeeker r = {&elf_error_reader_vt, NULL};
    return r;
}

static IoReadSeeker elf_error_reader(Alloc *a, Error e) {
    ElfReaderBox *b = elf_new_reader_box(a, ELF_RD_ERROR);
    if (b == NULL)
        return elf_oom_reader();
    b->err = e;
    IoReadSeeker r = {&elf_error_reader_vt, b};
    return r;
}

/* io.NewSectionReader(of, 0, size), in a box. */
static IoReadSeeker elf_section_reader(Alloc *a, IoReaderAt of, int64_t size,
                                       bool nobits) {
    ElfReaderBox *b = elf_new_reader_box(a, ELF_RD_SECTION);
    if (b == NULL)
        return elf_oom_reader();
    if (nobits) {
        of.vt = &elf_nobits_vt;
        of.data = b;
    }
    b->sr = io_new_section_reader(of, 0, size);
    return io_section_reader_as_io_read_seeker(&b->sr);
}

IoReadSeeker elf_section_open(ElfSection *s, Alloc *a) {
    if (s == NULL)
        elf_nil_panic();
    ElfSectionHeader *h = &s->section_header;
    IoReaderAt nil_at = {NULL, NULL};
    if (h->type == ELF_SHT_NOBITS)
        return elf_section_reader(a, nil_at, (int64_t)h->size, true);
    if ((h->flags & ELF_SHF_COMPRESSED) == 0) {
        IoReaderAt sr = io_section_reader_as_io_reader_at(&s->sr);
        if (!strings_has_prefix(h->name, BURROW_S(".zdebug")))
            return elf_section_reader(a, sr, INT64_MAX, false);
        Byte b[12];
        Error e = BURROW_NO_ERROR;
        Int n = io_section_reader_read_at(&s->sr, elf_bytes(b, 12), 0, &e);
        if (n != 12 || memcmp(b, "ZLIB", 4) != 0)
            return elf_section_reader(a, sr, INT64_MAX, false);
        s->compression_offset = 12;
        s->compression_type = ELF_COMPRESS_ZLIB;
        h->size = elf_u64(true, b + 4);
    } else if ((h->flags & ELF_SHF_ALLOC) != 0) {
        Error e =
            elf_fe((int64_t)h->offset,
                   BURROW_S("SHF_COMPRESSED applies only to non-allocable sections"),
                   elf_compression_type_string(s->compression_type, error_allocator()));
        return elf_error_reader(a, e);
    }
    if (s->compression_type != ELF_COMPRESS_ZLIB &&
        s->compression_type != ELF_COMPRESS_ZSTD) {
        Error e =
            elf_fe((int64_t)h->offset, BURROW_S("unknown compression type"),
                   elf_compression_type_string(s->compression_type, error_allocator()));
        return elf_error_reader(a, e);
    }
    ElfReaderBox *b = elf_new_reader_box(a, ELF_RD_DECOMPRESS);
    if (b == NULL)
        return elf_oom_reader();
    b->s = s;
    b->ct = s->compression_type;
    b->state = ELF_R_NIL;
    b->size = (int64_t)h->size;
    IoReadSeeker r = {&elf_rsfr_vt, b};
    return r;
}

IoReadSeeker elf_prog_open(ElfProg *p, Alloc *a) {
    if (p == NULL)
        elf_nil_panic();
    return elf_section_reader(a, io_section_reader_as_io_reader_at(&p->sr), INT64_MAX,
                              false);
}

void elf_reader_free(IoReadSeeker r) {
    ElfReaderBox *b = (ElfReaderBox *)r.data;
    if (b == NULL)
        return;
    if (b->kind == ELF_RD_DECOMPRESS)
        elf_rsfr_drop(b);
    mem_free(b->a, b, sizeof *b, _Alignof(ElfReaderBox));
}

Slice elf_section_data(ElfSection *s, Alloc *a, Error *err) {
    IoReadSeeker r = elf_section_open(s, a);
    IoReader rd = {&r.vt->reader, r.data};
    Slice data = elf_read_data(a, rd, s->section_header.size, err);
    elf_reader_free(r);
    return data;
}

Int elf_section_read_at(ElfSection *s, Slice p, int64_t off, Error *err) {
    if (s == NULL || s->reader_at.vt == NULL)
        elf_nil_panic();
    return s->reader_at.vt->read_at(s->reader_at.data, p, off, err);
}

Int elf_prog_read_at(ElfProg *p, Slice b, int64_t off, Error *err) {
    if (p == NULL || p->reader_at.vt == NULL)
        elf_nil_panic();
    return p->reader_at.vt->read_at(p->reader_at.data, b, off, err);
}

/* ------------------------------------------------------------------- files */

/* "[127 69 76 70]", which is %v of a []byte. */
static Str elf_bytes_v(Byte buf[24], const Byte *p, Int n) {
    Int k = 0;
    buf[k++] = '[';
    for (Int i = 0; i < n; i++) {
        Byte d[24];
        Str s = elf_udec(d, p[i]);
        if (i > 0)
            buf[k++] = ' ';
        memcpy(buf + k, s.p, (size_t)s.len);
        k += s.len;
    }
    buf[k++] = ']';
    return str_from_bytes(buf, k);
}

static void elf_file_release(ElfFile *f) {
    Alloc *a = f->a;
    arena_free(&f->arena);
    mem_free(a, f, sizeof *f, _Alignof(ElfFile));
}

/* The part of NewFile after the identifier, filling in f. */
static Error elf_read_headers(ElfFile *f, IoReaderAt r, Alloc *tmp) {
    Alloc *fa = arena_allocator(&f->arena);
    ElfFileHeader *fh = &f->file_header;
    bool big = elf_big(f);
    bool is64 = fh->cls == ELF_ELFCLASS64;
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);
    IoReaderAt sra = io_section_reader_as_io_reader_at(&sr);
    Error e = BURROW_NO_ERROR;

    /* Read ELF file header. */
    Byte hdr[sizeof(ElfHeader64)];
    Int hsize = is64 ? (Int)sizeof(ElfHeader64) : (Int)sizeof(ElfHeader32);
    io_section_reader_read_at(&sr, elf_bytes(hdr, hsize), 0, &e);
    if (BURROW_FAILED(e))
        return e;
    int64_t phoff = 0, shoff = 0;
    Int phentsize = 0, phnum = 0, shentsize = 0, shnum = 0, shstrndx = 0;
    ElfVersion v = 0;
    if (!is64) {
        fh->type = elf_u16(big, hdr + offsetof(ElfHeader32, type));
        fh->machine = elf_u16(big, hdr + offsetof(ElfHeader32, machine));
        fh->entry = elf_u32(big, hdr + offsetof(ElfHeader32, entry));
        v = (ElfVersion)elf_u32(big, hdr + offsetof(ElfHeader32, version));
        phoff = (int64_t)elf_u32(big, hdr + offsetof(ElfHeader32, phoff));
        phentsize = elf_u16(big, hdr + offsetof(ElfHeader32, phentsize));
        phnum = elf_u16(big, hdr + offsetof(ElfHeader32, phnum));
        shoff = (int64_t)elf_u32(big, hdr + offsetof(ElfHeader32, shoff));
        shentsize = elf_u16(big, hdr + offsetof(ElfHeader32, shentsize));
        shnum = elf_u16(big, hdr + offsetof(ElfHeader32, shnum));
        shstrndx = elf_u16(big, hdr + offsetof(ElfHeader32, shstrndx));
    } else {
        fh->type = elf_u16(big, hdr + offsetof(ElfHeader64, type));
        fh->machine = elf_u16(big, hdr + offsetof(ElfHeader64, machine));
        fh->entry = elf_u64(big, hdr + offsetof(ElfHeader64, entry));
        v = (ElfVersion)elf_u32(big, hdr + offsetof(ElfHeader64, version));
        phoff = (int64_t)elf_u64(big, hdr + offsetof(ElfHeader64, phoff));
        phentsize = elf_u16(big, hdr + offsetof(ElfHeader64, phentsize));
        phnum = elf_u16(big, hdr + offsetof(ElfHeader64, phnum));
        shoff = (int64_t)elf_u64(big, hdr + offsetof(ElfHeader64, shoff));
        shentsize = elf_u16(big, hdr + offsetof(ElfHeader64, shentsize));
        shnum = elf_u16(big, hdr + offsetof(ElfHeader64, shnum));
        shstrndx = elf_u16(big, hdr + offsetof(ElfHeader64, shstrndx));
    }
    if (v != fh->version)
        return elf_fe(0, BURROW_S("mismatched ELF version"),
                      elf_version_string(v, error_allocator()));

    if (shoff < 0)
        return elf_fe_i(0, BURROW_S("invalid shoff"), shoff);
    if (phoff < 0)
        return elf_fe_i(0, BURROW_S("invalid phoff"), phoff);
    if (shoff == 0 && shnum != 0)
        return elf_fe_i(0, BURROW_S("invalid ELF shnum for shoff=0"), shnum);
    if (shnum > 0 && shstrndx >= shnum)
        return elf_fe_i(0, BURROW_S("invalid ELF shstrndx"), shstrndx);

    Int want_phentsize = is64 ? 2 * 4 + 6 * 8 : 8 * 4;
    Int want_shentsize = is64 ? 4 * 4 + 6 * 8 : 10 * 4;
    if (phnum > 0 && phentsize < want_phentsize)
        return elf_fe_i(0, BURROW_S("invalid ELF phentsize"), phentsize);

    /* If the number of sections is greater than or equal to SHN_LORESERVE
     * (0xff00), shnum has the value zero and the actual number of section
     * header table entries is contained in the sh_size field of the section
     * header at index 0.
     *
     * If the number of segments is greater than or equal to 0xffff, phnum has
     * the value 0xffff, and the actual number of segments is contained in the
     * sh_info field of the section header at index 0. */
    const Int pn_xnum = 0xffff;
    if (shoff > 0 && (shnum == 0 || phnum == pn_xnum)) {
        Byte sh[sizeof(ElfSection64)];
        Int shsize = is64 ? (Int)sizeof(ElfSection64) : (Int)sizeof(ElfSection32);
        Error serr = BURROW_NO_ERROR;
        io_section_reader_seek(&sr, shoff, BURROW_IO_SEEK_START, &serr);
        io_read_full(io_section_reader_as_io_reader(&sr), elf_bytes(sh, shsize), &e);
        if (BURROW_FAILED(e))
            return e;
        uint32_t typ = 0, link = 0, info = 0;
        uint64_t size = 0;
        if (!is64) {
            size = elf_u32(big, sh + offsetof(ElfSection32, size));
            typ = elf_u32(big, sh + offsetof(ElfSection32, type));
            link = elf_u32(big, sh + offsetof(ElfSection32, link));
            info = elf_u32(big, sh + offsetof(ElfSection32, info));
        } else {
            size = elf_u64(big, sh + offsetof(ElfSection64, size));
            typ = elf_u32(big, sh + offsetof(ElfSection64, type));
            link = elf_u32(big, sh + offsetof(ElfSection64, link));
            info = elf_u32(big, sh + offsetof(ElfSection64, info));
        }
        if (typ != ELF_SHT_NULL)
            return elf_fe(shoff, BURROW_S("invalid type of the initial section"),
                          elf_section_type_string(typ, error_allocator()));
        if (shnum == 0) {
            if (size < ELF_SHN_LORESERVE)
                return elf_fe_i(
                    shoff, BURROW_S("invalid ELF shnum contained in sh_size"), shnum);
            shnum = (Int)size;
        }
        if (phnum == pn_xnum) {
            if (info < 0xffff)
                return elf_fe_u(
                    shoff, BURROW_S("invalid ELF phnum contained in sh_info"), info);
            phnum = (Int)info;
        }
        /* If the section name string table section index is greater than or
         * equal to SHN_LORESERVE (0xff00), this member has the value
         * SHN_XINDEX (0xffff) and the actual index of the section name string
         * table section is contained in the sh_link field of the section
         * header at index 0. */
        if (shstrndx == ELF_SHN_XINDEX) {
            shstrndx = (Int)link;
            if (shstrndx < ELF_SHN_LORESERVE || shstrndx >= shnum)
                return elf_fe_i(shoff,
                                BURROW_S("invalid ELF shstrndx contained in sh_link"),
                                shstrndx);
        }
    }

    /* Read program headers. */
    if (elf_slice_cap(sizeof(void *), (uint64_t)phnum) < 0)
        return elf_fe_i(0, BURROW_S("too many segments"), phnum);
    if (phnum > 0 && UINT64_MAX / (uint64_t)phnum < (uint64_t)phentsize)
        return elf_fe_i(0, BURROW_S("segment header overflow"), phnum);
    Slice phdata =
        elf_read_data_at(tmp, sra, (uint64_t)phnum * (uint64_t)phentsize, phoff, &e);
    if (BURROW_FAILED(e))
        return e;
    /* A read of a positive size that worked always has bytes, which clang's
     * analyzer cannot see through the chunked path. */
    if (phnum > 0 && phdata.p == NULL)
        return io_err_unexpected_eof;
    ElfProg *progs = NULL;
    void **pprogs = NULL;
    if (phnum > 0) {
        progs =
            (ElfProg *)mem_alloc(fa, (size_t)phnum * sizeof *progs, _Alignof(ElfProg));
        pprogs =
            (void **)mem_alloc(fa, (size_t)phnum * sizeof *pprogs, _Alignof(void *));
        if (progs == NULL || pprogs == NULL)
            return burrow_err_out_of_memory;
    }
    for (Int i = 0; i < phnum; i++) {
        size_t off = (size_t)i * (size_t)phentsize;
        const Byte *d = (const Byte *)phdata.p + off;
        ElfProg *p = &progs[i];
        ElfProgHeader *ph = &p->prog_header;
        if (!is64) {
            ph->type = (ElfProgType)elf_u32(big, d + offsetof(ElfProg32, type));
            ph->flags = elf_u32(big, d + offsetof(ElfProg32, flags));
            ph->off = elf_u32(big, d + offsetof(ElfProg32, off));
            ph->vaddr = elf_u32(big, d + offsetof(ElfProg32, vaddr));
            ph->paddr = elf_u32(big, d + offsetof(ElfProg32, paddr));
            ph->filesz = elf_u32(big, d + offsetof(ElfProg32, filesz));
            ph->memsz = elf_u32(big, d + offsetof(ElfProg32, memsz));
            ph->align = elf_u32(big, d + offsetof(ElfProg32, align));
        } else {
            ph->type = (ElfProgType)elf_u32(big, d + offsetof(ElfProg64, type));
            ph->flags = elf_u32(big, d + offsetof(ElfProg64, flags));
            ph->off = elf_u64(big, d + offsetof(ElfProg64, off));
            ph->vaddr = elf_u64(big, d + offsetof(ElfProg64, vaddr));
            ph->paddr = elf_u64(big, d + offsetof(ElfProg64, paddr));
            ph->filesz = elf_u64(big, d + offsetof(ElfProg64, filesz));
            ph->memsz = elf_u64(big, d + offsetof(ElfProg64, memsz));
            ph->align = elf_u64(big, d + offsetof(ElfProg64, align));
        }
        if ((int64_t)ph->off < 0)
            return elf_fe_u(phoff + (int64_t)off,
                            BURROW_S("invalid program header offset"), ph->off);
        if ((int64_t)ph->filesz < 0)
            return elf_fe_u(phoff + (int64_t)off,
                            BURROW_S("invalid program header file size"), ph->filesz);
        p->sr = io_new_section_reader(r, (int64_t)ph->off, (int64_t)ph->filesz);
        p->reader_at = io_section_reader_as_io_reader_at(&p->sr);
        pprogs[i] = p;
    }
    Slice ps = {pprogs, phnum, phnum, TYPE_UNSAFE_POINTER};
    f->progs = ps;

    if (shnum > 0 && shentsize < want_shentsize)
        return elf_fe_i(0, BURROW_S("invalid ELF shentsize"), shentsize);

    /* Read section headers. */
    if (elf_slice_cap(sizeof(ElfSection), (uint64_t)shnum) < 0)
        return elf_fe_i(0, BURROW_S("too many sections"), shnum);
    if (shnum > 0 && UINT64_MAX / (uint64_t)shnum < (uint64_t)shentsize)
        return elf_fe_i(0, BURROW_S("section header overflow"), shnum);
    Slice shdata =
        elf_read_data_at(tmp, sra, (uint64_t)shnum * (uint64_t)shentsize, shoff, &e);
    if (BURROW_FAILED(e))
        return e;
    if (shnum > 0 && shdata.p == NULL)
        return io_err_unexpected_eof;
    ElfSection *secs = NULL;
    void **psecs = NULL;
    uint32_t *names = NULL;
    if (shnum > 0) {
        secs = (ElfSection *)mem_alloc(fa, (size_t)shnum * sizeof *secs,
                                       _Alignof(ElfSection));
        psecs = (void **)mem_alloc(fa, (size_t)shnum * sizeof *psecs, _Alignof(void *));
        names = (uint32_t *)mem_alloc(tmp, (size_t)shnum * sizeof *names,
                                      _Alignof(uint32_t));
        if (secs == NULL || psecs == NULL || names == NULL)
            return burrow_err_out_of_memory;
    }
    for (Int i = 0; i < shnum; i++) {
        size_t off = (size_t)i * (size_t)shentsize;
        const Byte *d = (const Byte *)shdata.p + off;
        ElfSection *s = &secs[i];
        ElfSectionHeader *sh = &s->section_header;
        if (!is64) {
            names[i] = elf_u32(big, d + offsetof(ElfSection32, name));
            sh->type = elf_u32(big, d + offsetof(ElfSection32, type));
            sh->flags = elf_u32(big, d + offsetof(ElfSection32, flags));
            sh->addr = elf_u32(big, d + offsetof(ElfSection32, addr));
            sh->offset = elf_u32(big, d + offsetof(ElfSection32, off));
            sh->file_size = elf_u32(big, d + offsetof(ElfSection32, size));
            sh->link = elf_u32(big, d + offsetof(ElfSection32, link));
            sh->info = elf_u32(big, d + offsetof(ElfSection32, info));
            sh->addralign = elf_u32(big, d + offsetof(ElfSection32, addralign));
            sh->entsize = elf_u32(big, d + offsetof(ElfSection32, entsize));
        } else {
            names[i] = elf_u32(big, d + offsetof(ElfSection64, name));
            sh->type = elf_u32(big, d + offsetof(ElfSection64, type));
            sh->flags = (ElfSectionFlag)elf_u64(big, d + offsetof(ElfSection64, flags));
            sh->offset = elf_u64(big, d + offsetof(ElfSection64, off));
            sh->file_size = elf_u64(big, d + offsetof(ElfSection64, size));
            sh->addr = elf_u64(big, d + offsetof(ElfSection64, addr));
            sh->link = elf_u32(big, d + offsetof(ElfSection64, link));
            sh->info = elf_u32(big, d + offsetof(ElfSection64, info));
            sh->addralign = elf_u64(big, d + offsetof(ElfSection64, addralign));
            sh->entsize = elf_u64(big, d + offsetof(ElfSection64, entsize));
        }
        if ((int64_t)sh->offset < 0)
            return elf_fe_i(shoff + (int64_t)off, BURROW_S("invalid section offset"),
                            (int64_t)sh->offset);
        if ((int64_t)sh->file_size < 0)
            return elf_fe_i(shoff + (int64_t)off, BURROW_S("invalid section size"),
                            (int64_t)sh->file_size);
        s->sr = io_new_section_reader(r, (int64_t)sh->offset, (int64_t)sh->file_size);

        if ((sh->flags & ELF_SHF_COMPRESSED) == 0) {
            s->reader_at = io_section_reader_as_io_reader_at(&s->sr);
            sh->size = sh->file_size;
        } else {
            /* Read the compression header. */
            Byte ch[sizeof(ElfChdr64)];
            Int chsize = is64 ? (Int)sizeof(ElfChdr64) : (Int)sizeof(ElfChdr32);
            io_section_reader_read_at(&s->sr, elf_bytes(ch, chsize), 0, &e);
            if (BURROW_FAILED(e))
                return e;
            if (!is64) {
                s->compression_type =
                    (ElfCompressionType)elf_u32(big, ch + offsetof(ElfChdr32, type));
                sh->size = elf_u32(big, ch + offsetof(ElfChdr32, size));
                sh->addralign = elf_u32(big, ch + offsetof(ElfChdr32, addralign));
            } else {
                s->compression_type =
                    (ElfCompressionType)elf_u32(big, ch + offsetof(ElfChdr64, type));
                sh->size = elf_u64(big, ch + offsetof(ElfChdr64, size));
                sh->addralign = elf_u64(big, ch + offsetof(ElfChdr64, addralign));
            }
            s->compression_offset = (int64_t)chsize;
        }
        psecs[i] = s;
    }
    Slice ss = {psecs, shnum, shnum, TYPE_UNSAFE_POINTER};
    f->sections = ss;

    if (shnum == 0)
        return BURROW_NO_ERROR;

    /* Load section header string table. If the file has no section name
     * string table, shstrndx holds the value SHN_UNDEF (0). */
    if (shstrndx == 0)
        return BURROW_NO_ERROR;
    if (shstrndx >= shnum)
        runtime_index_out_of_range(shstrndx, shnum);
    ElfSection *shstr = &secs[shstrndx];
    if (shstr->section_header.type != ELF_SHT_STRTAB)
        return elf_fe(
            shoff + (int64_t)(shstrndx * shentsize),
            BURROW_S("invalid ELF section name string table type"),
            elf_section_type_string(shstr->section_header.type, error_allocator()));
    Slice shstrtab = elf_section_data(shstr, fa, &e);
    if (BURROW_FAILED(e))
        return e;
    for (Int i = 0; i < shnum; i++) {
        if (!elf_get_string(shstrtab, names[i], &secs[i].section_header.name))
            return elf_fe_u(shoff + (int64_t)(i * shentsize),
                            BURROW_S("bad section name index"), names[i]);
    }
    return BURROW_NO_ERROR;
}

ElfFile *elf_new_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        elf_nil_panic();
    /* Read and decode ELF identifier. */
    Byte ident[16];
    Byte buf[24];
    Error e = BURROW_NO_ERROR;
    r.vt->read_at(r.data, elf_bytes(ident, 16), 0, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err,
                   elf_fe(0, BURROW_S("cannot read ELF identifier"), error_text(e)));
        return NULL;
    }
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') {
        BURROW_OUT(err,
                   elf_fe(0, BURROW_S("bad magic number"), elf_bytes_v(buf, ident, 4)));
        return NULL;
    }

    ElfFileHeader fh = {0};
    fh.cls = ident[ELF_EI_CLASS];
    if (fh.cls != ELF_ELFCLASS32 && fh.cls != ELF_ELFCLASS64) {
        BURROW_OUT(err, elf_fe(0, BURROW_S("unknown ELF class"),
                               elf_class_string(fh.cls, error_allocator())));
        return NULL;
    }
    fh.data = ident[ELF_EI_DATA];
    if (fh.data == ELF_ELFDATA2LSB) {
        fh.byte_order = binary_little_endian;
    } else if (fh.data == ELF_ELFDATA2MSB) {
        fh.byte_order = binary_big_endian;
    } else {
        BURROW_OUT(err, elf_fe(0, BURROW_S("unknown ELF data encoding"),
                               elf_data_string(fh.data, error_allocator())));
        return NULL;
    }
    fh.version = ident[ELF_EI_VERSION];
    if (fh.version != ELF_EV_CURRENT) {
        BURROW_OUT(err, elf_fe(0, BURROW_S("unknown ELF version"),
                               elf_version_string(fh.version, error_allocator())));
        return NULL;
    }
    fh.osabi = ident[ELF_EI_OSABI];
    fh.abi_version = ident[ELF_EI_ABIVERSION];

    ElfFile *f = (ElfFile *)mem_alloc(a, sizeof *f, _Alignof(ElfFile));
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->a = a;
    arena_init(&f->arena, a, 0);
    f->file_header = fh;
    f->sections.elem = TYPE_UNSAFE_POINTER;
    f->progs.elem = TYPE_UNSAFE_POINTER;
    f->dyn_vers.elem = TYPE_ELF_DYNAMIC_VERSION;
    f->dyn_ver_needs.elem = TYPE_ELF_DYNAMIC_VERSION_NEED;
    f->gnu_versym.elem = TYPE_BYTE;

    Arena tmp;
    arena_init(&tmp, a, 0);
    e = elf_read_headers(f, r, arena_allocator(&tmp));
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        elf_file_release(f);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

ElfFile *elf_open(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    ElfFile *f = elf_new_file(a, os_file_as_io_reader_at(file), &e);
    if (f == NULL) {
        os_file_close(file);
        os_file_free(file);
        BURROW_OUT(err, e);
        return NULL;
    }
    f->file = file;
    f->open = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

Error elf_file_close(ElfFile *f) {
    if (f == NULL)
        elf_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (f->open) {
        e = os_file_close(f->file);
        f->open = false;
    }
    return e;
}

void elf_file_free(ElfFile *f) {
    if (f == NULL)
        return;
    OsFile *file = f->file;
    if (f->open)
        os_file_close(file);
    os_file_free(file);
    elf_file_release(f);
}

ElfSection *elf_file_section_by_type(ElfFile *f, ElfSectionType typ) {
    if (f == NULL)
        elf_nil_panic();
    for (Int i = 0; i < f->sections.len; i++) {
        ElfSection *s = ((ElfSection **)f->sections.p)[i];
        if (s->section_header.type == typ)
            return s;
    }
    return NULL;
}

ElfSection *elf_file_section(ElfFile *f, Str name) {
    if (f == NULL)
        elf_nil_panic();
    for (Int i = 0; i < f->sections.len; i++) {
        ElfSection *s = ((ElfSection **)f->sections.p)[i];
        if (str_eq(s->section_header.name, name))
            return s;
    }
    return NULL;
}

/* ----------------------------------------------------------------- symbols */

static void elf_free_bytes(Alloc *a, Slice b) {
    if (b.p != NULL)
        mem_free(a, b.p, b.cap == 0 ? 1 : (size_t)b.cap, 1);
}

static void elf_free_symbols(Alloc *a, Slice syms) {
    if (syms.p != NULL)
        mem_free(a, syms.p, (size_t)syms.cap * sizeof(ElfSymbol), _Alignof(ElfSymbol));
}

/* File.stringTable: the data of the string table section link, in a. */
static Slice elf_string_table(ElfFile *f, uint32_t link, Alloc *a, Error *err) {
    if (link == 0 || link >= (uint64_t)f->sections.len) {
        Slice none = {NULL, 0, 0, TYPE_BYTE};
        BURROW_OUT(err, elf_err_strtab_link);
        return none;
    }
    return elf_section_data(((ElfSection **)f->sections.p)[link], a, err);
}

/* File.getSymbols: the symbols in a and the string table their names point
 * into in stra, which *strp gets when it is not NULL. */
static Slice elf_get_symbols(ElfFile *f, ElfSectionType typ, Alloc *a, Alloc *stra,
                             Slice *strp, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_ELF_SYMBOL};
    Slice strdata = {NULL, 0, 0, TYPE_BYTE};
    if (strp != NULL)
        *strp = strdata;
    ElfClass cls = f->file_header.cls;
    if (cls != ELF_ELFCLASS32 && cls != ELF_ELFCLASS64) {
        BURROW_OUT(err, elf_err_not_implemented);
        return none;
    }
    bool is64 = cls == ELF_ELFCLASS64;
    Int symsize = is64 ? 24 : 16;
    bool big = elf_big(f);

    ElfSection *symtab = elf_file_section_by_type(f, typ);
    if (symtab == NULL) {
        BURROW_OUT(err, elf_err_no_symbols);
        return none;
    }
    Arena tmp;
    arena_init(&tmp, a, 0);
    Error e = BURROW_NO_ERROR;
    Slice data = elf_section_data(symtab, arena_allocator(&tmp), &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, fmt_errorf_v("cannot load symbol section: %w", e));
        return none;
    }
    if (data.len == 0) {
        arena_free(&tmp);
        BURROW_OUT(err, elf_err_no_symbols);
        return none;
    }
    if (data.len % symsize != 0) {
        arena_free(&tmp);
        BURROW_OUT(err, is64 ? elf_err_sym64_len : elf_err_sym32_len);
        return none;
    }
    strdata = elf_string_table(f, symtab->section_header.link, stra, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, fmt_errorf_v("cannot load string table section: %w", e));
        return none;
    }

    /* The first entry is all zeros. */
    Int n = data.len / symsize - 1;
    ElfSymbol *syms = NULL;
    if (n > 0) {
        syms = (ElfSymbol *)mem_alloc(a, (size_t)n * sizeof *syms, _Alignof(ElfSymbol));
        if (syms == NULL) {
            arena_free(&tmp);
            elf_free_bytes(stra, strdata);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
    }
    for (Int i = 0; i < n; i++) {
        const Byte *d = (const Byte *)data.p + (i + 1) * symsize;
        ElfSymbol *s = &syms[i];
        uint32_t name = elf_u32(big, d);
        if (!is64) {
            s->value = elf_u32(big, d + 4);
            s->size = elf_u32(big, d + 8);
            s->info = d[12];
            s->other = d[13];
            s->section = elf_u16(big, d + 14);
        } else {
            s->info = d[4];
            s->other = d[5];
            s->section = elf_u16(big, d + 6);
            s->value = elf_u64(big, d + 8);
            s->size = elf_u64(big, d + 16);
        }
        elf_get_string(strdata, name, &s->name);
    }
    arena_free(&tmp);
    if (strp != NULL)
        *strp = strdata;
    else if (n == 0)
        elf_free_bytes(stra, strdata);
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {syms, n, n, TYPE_ELF_SYMBOL};
    return out;
}

/* ------------------------------------------------------------- relocations */

/* One of Go's applyRelocations functions, as a row. Rel32 entries are 8 bytes
 * and add to what is there; Rela32 entries are 12 bytes and Rela64 entries 24,
 * and both write the symbol plus the addend. */
typedef struct ElfRelocArch {
    ElfClass cls;
    ElfMachine machine;
    uint8_t entsize;
    bool can_apply;
    uint32_t type_mask;
    uint32_t r64, r64b; /* the 8-byte relocations, for Rela64 */
    uint32_t r32, r32b; /* the 4-byte ones */
} ElfRelocArch;

static const ElfRelocArch elf_reloc_archs[] = {
    {ELF_ELFCLASS64, ELF_EM_X86_64, 24, true, 0xffff, ELF_R_X86_64_64, ELF_R_X86_64_64,
     ELF_R_X86_64_32, ELF_R_X86_64_32},
    {ELF_ELFCLASS32, ELF_EM_386, 8, false, 0xff, 0, 0, ELF_R_386_32, ELF_R_386_32},
    {ELF_ELFCLASS32, ELF_EM_ARM, 8, false, 0xff, 0, 0, ELF_R_ARM_ABS32,
     ELF_R_ARM_ABS32},
    {ELF_ELFCLASS64, ELF_EM_AARCH64, 24, true, 0xffff, ELF_R_AARCH64_ABS64,
     ELF_R_AARCH64_ABS64, ELF_R_AARCH64_ABS32, ELF_R_AARCH64_ABS32},
    {ELF_ELFCLASS32, ELF_EM_PPC, 12, true, 0xff, 0, 0, ELF_R_PPC_ADDR32,
     ELF_R_PPC_ADDR32},
    {ELF_ELFCLASS64, ELF_EM_PPC64, 24, true, 0xffff, ELF_R_PPC64_ADDR64,
     ELF_R_PPC64_ADDR64, ELF_R_PPC64_ADDR32, ELF_R_PPC64_ADDR32},
    {ELF_ELFCLASS32, ELF_EM_MIPS, 8, false, 0xff, 0, 0, ELF_R_MIPS_32, ELF_R_MIPS_32},
    {ELF_ELFCLASS64, ELF_EM_MIPS, 24, true, 0xff, ELF_R_MIPS_64, ELF_R_MIPS_64,
     ELF_R_MIPS_32, ELF_R_MIPS_32},
    {ELF_ELFCLASS64, ELF_EM_LOONGARCH, 24, true, 0xffff, ELF_R_LARCH_64, ELF_R_LARCH_64,
     ELF_R_LARCH_32, ELF_R_LARCH_32},
    {ELF_ELFCLASS64, ELF_EM_RISCV, 24, true, 0xffff, ELF_R_RISCV_64, ELF_R_RISCV_64,
     ELF_R_RISCV_32, ELF_R_RISCV_32},
    {ELF_ELFCLASS64, ELF_EM_S390, 24, true, 0xffff, ELF_R_390_64, ELF_R_390_64,
     ELF_R_390_32, ELF_R_390_32},
    {ELF_ELFCLASS64, ELF_EM_SPARCV9, 24, true, 0xff, ELF_R_SPARC_64, ELF_R_SPARC_UA64,
     ELF_R_SPARC_32, ELF_R_SPARC_UA32},
};

/* putUint: does nothing when the write would not fit in dst, the addend is
 * negative, or start+length overflows. */
static void elf_put_uint(bool big, Slice dst, uint64_t start, uint64_t length,
                         uint64_t sym, int64_t addend, bool read_uint) {
    if (start + length > (uint64_t)dst.len || UINT64_MAX - start < length)
        return;
    if (addend < 0)
        return;
    Byte *s = (Byte *)dst.p + start;
    if (length == 4) {
        uint32_t ae = (uint32_t)addend;
        if (read_uint)
            ae += elf_u32(big, s);
        elf_put_u32(big, s, (uint32_t)sym + ae);
    } else {
        uint64_t ae = (uint64_t)addend;
        if (read_uint)
            ae += elf_u64(big, s);
        elf_put_u64(big, s, sym + ae);
    }
}

/* File.applyRelocations: applies the REL or RELA section rels to dst. */
static Error elf_apply_relocations(ElfFile *f, Slice dst, Slice rels, Alloc *tmp) {
    const ElfRelocArch *arch = NULL;
    for (size_t k = 0; k < sizeof elf_reloc_archs / sizeof elf_reloc_archs[0]; k++) {
        const ElfRelocArch *x = &elf_reloc_archs[k];
        if (x->cls == f->file_header.cls && x->machine == f->file_header.machine) {
            arch = x;
            break;
        }
    }
    if (arch == NULL)
        return elf_err_reloc_not_implemented;
    if (rels.len % arch->entsize != 0) {
        switch (arch->entsize) {
        case 8:
            return elf_err_rel8_len;
        case 12:
            return elf_err_rel12_len;
        default:
            return elf_err_rel24_len;
        }
    }

    Error e = BURROW_NO_ERROR;
    Slice syms = elf_get_symbols(f, ELF_SHT_SYMTAB, tmp, tmp, NULL, &e);
    if (BURROW_FAILED(e))
        return e;
    const ElfSymbol *symbols = (const ElfSymbol *)syms.p;
    uint64_t nsym = symbols == NULL ? 0 : (uint64_t)syms.len;
    bool big = elf_big(f);
    bool mips64 = arch->machine == ELF_EM_MIPS && arch->cls == ELF_ELFCLASS64;

    for (Int at = 0; at < rels.len; at += arch->entsize) {
        const Byte *d = (const Byte *)rels.p + at;
        uint64_t off = 0, symno = 0;
        uint32_t t = 0;
        int64_t addend = 0;
        switch (arch->entsize) {
        case 8: {
            uint32_t info = elf_u32(big, d + 4);
            off = elf_u32(big, d);
            symno = info >> 8;
            t = info & 0xff;
            break;
        }
        case 12: {
            uint32_t info = elf_u32(big, d + 4);
            off = elf_u32(big, d);
            symno = info >> 8;
            t = info & 0xff;
            addend = (int32_t)elf_u32(big, d + 8);
            break;
        }
        default: {
            uint64_t info = elf_u64(big, d + 8);
            off = elf_u64(big, d);
            addend = (int64_t)elf_u64(big, d + 16);
            if (mips64 && !big) {
                symno = info & 0xffffffff;
                t = (uint32_t)(info >> 56);
            } else {
                symno = info >> 32;
                t = (uint32_t)(info & arch->type_mask);
            }
            break;
        }
        }

        if (symno == 0 || symno > nsym)
            continue;
        const ElfSymbol *sym = &symbols[symno - 1];
        if (arch->can_apply &&
            (sym->section == ELF_SHN_UNDEF || sym->section >= ELF_SHN_LORESERVE))
            continue;

        /* There are relocations, so this must be a normal object file. Only
         * the basic relocations of the form S + A (symbol plus addend) are
         * handled. */
        bool read_uint = arch->entsize == 8;
        if (arch->entsize == 24 && (t == arch->r64 || t == arch->r64b))
            elf_put_uint(big, dst, off, 8, sym->value, addend, read_uint);
        else if (t == arch->r32 || t == arch->r32b)
            elf_put_uint(big, dst, off, 4, sym->value, addend, read_uint);
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------- dwarf */

static Str elf_dwarf_suffix(const ElfSection *s) {
    Str name = s->section_header.name;
    if (strings_has_prefix(name, BURROW_S(".debug_")))
        return str_from_bytes(name.p + 7, name.len - 7);
    if (strings_has_prefix(name, BURROW_S(".zdebug_")))
        return str_from_bytes(name.p + 8, name.len - 8);
    return BURROW_STR_EMPTY;
}

/* The data of section i, in a, with the relocations for it applied. */
static Slice elf_dwarf_section_data(ElfFile *f, Int i, ElfSection *s, Alloc *a,
                                    Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Error e = BURROW_NO_ERROR;
    Slice b = elf_section_data(s, a, &e);
    if (BURROW_FAILED(e) && (uint64_t)b.len < s->section_header.size) {
        BURROW_OUT(err, e);
        return none;
    }

    /* Do not apply relocations to DWARF sections for ET_EXEC binaries.
     * Relocations should already be applied, and .rela sections may contain
     * incorrect data. */
    if (f->file_header.type == ELF_ET_EXEC) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return b;
    }

    for (Int k = 0; k < f->sections.len; k++) {
        ElfSection *r = ((ElfSection **)f->sections.p)[k];
        if (r->section_header.type != ELF_SHT_RELA &&
            r->section_header.type != ELF_SHT_REL)
            continue;
        if ((Int)r->section_header.info != i)
            continue;
        Arena tmp;
        arena_init(&tmp, a, 0);
        Slice rd = elf_section_data(r, arena_allocator(&tmp), &e);
        if (!BURROW_FAILED(e))
            e = elf_apply_relocations(f, b, rd, arena_allocator(&tmp));
        arena_free(&tmp);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return none;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

/* The sections the debug/dwarf package started with, which dwarf_new takes. */
enum { ELF_DW_ABBREV, ELF_DW_INFO, ELF_DW_STR, ELF_DW_LINE, ELF_DW_RANGES, ELF_DW_N };

static int elf_dwarf_slot(Str suffix) {
    static const char *const names[ELF_DW_N] = {"abbrev", "info", "str", "line",
                                                "ranges"};
    for (int k = 0; k < ELF_DW_N; k++) {
        if (str_eq(suffix, str_from_cstr(names[k])))
            return k;
    }
    return -1;
}

DwarfData *elf_file_dwarf(ElfFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    Slice dat[ELF_DW_N];
    for (int k = 0; k < ELF_DW_N; k++) {
        Slice none = {NULL, 0, 0, TYPE_BYTE};
        dat[k] = none;
    }
    Error e = BURROW_NO_ERROR;
    ElfSection **secs = (ElfSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = elf_dwarf_suffix(secs[i]);
        if (suffix.len == 0)
            continue;
        int k = elf_dwarf_slot(suffix);
        if (k < 0)
            continue;
        Slice b = elf_dwarf_section_data(f, i, secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        dat[k] = b;
    }

    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    DwarfData *d =
        dwarf_new(a, dat[ELF_DW_ABBREV], nil, nil, dat[ELF_DW_INFO], dat[ELF_DW_LINE],
                  nil, dat[ELF_DW_RANGES], dat[ELF_DW_STR], &e);
    if (d == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }

    /* Look for DWARF4 .debug_types sections and DWARF5 sections. */
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = elf_dwarf_suffix(secs[i]);
        if (suffix.len == 0 || elf_dwarf_slot(suffix) >= 0)
            continue;
        Slice b = elf_dwarf_section_data(f, i, secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        bool types = str_eq(suffix, BURROW_S("types"));
        Byte num[24];
        Str prefix = types ? BURROW_S("types-") : BURROW_S(".debug_");
        Str rest = types ? elf_idec(num, i) : suffix;
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(prefix.len + rest.len), 1);
        if (p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        elf_put(p, elf_put(p, 0, prefix), rest);
        Str name = str_from_bytes(p, prefix.len + rest.len);
        e = types ? dwarf_data_add_types(d, name, b)
                  : dwarf_data_add_section(d, name, b);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d;
}

/* ---------------------------------------------------------------- versions */

/* The names in the aux chain of a verdef entry, at most cnt of them from j,
 * into names, cloned into fa. With names NULL it only counts them. */
static Int elf_verdaux(bool big, Slice d, int64_t j, uint16_t cnt, Slice str, Alloc *fa,
                       Str *names) {
    const Byte *p = (const Byte *)d.p;
    Int n = 0;
    for (int c = 0; c < cnt; c++) {
        if (j + 8 > d.len)
            break;
        if (names != NULL) {
            Str name;
            elf_get_string(str, elf_u32(big, p + j), &name);
            names[n] = str_clone(fa, name);
            if (name.len > 0 && names[n].p == NULL)
                return -1;
        }
        n++;
        uint32_t vnext = elf_u32(big, p + j + 4);
        if (vnext == 0)
            break;
        j += vnext;
    }
    return n;
}

/* Walks the verdef table in d. With out NULL it only checks the table and
 * counts the versions; with out it fills them in, from fa. */
static Error elf_walk_verdef(ElfFile *f, const ElfSection *vd, Slice d, Slice str,
                             Alloc *fa, ElfDynamicVersion *out, Int *count) {
    bool big = elf_big(f);
    const Byte *p = (const Byte *)d.p;
    Int n = 0;
    for (int64_t i = 0; i + 20 <= d.len;) {
        uint16_t version = elf_u16(big, p + i);
        if (version != 1)
            return elf_fe_u((int64_t)(vd->section_header.offset + (uint64_t)i),
                            BURROW_S("unexpected dynamic version"), version);
        uint16_t cnt = elf_u16(big, p + i + 6);
        uint32_t aux = elf_u32(big, p + i + 12);
        uint32_t next = elf_u32(big, p + i + 16);
        if (cnt == 0)
            return elf_fe_nil((int64_t)(vd->section_header.offset + (uint64_t)i),
                              BURROW_S("dynamic version has no name"));
        if (out != NULL) {
            ElfDynamicVersion *v = &out[n];
            v->flags = elf_u16(big, p + i + 2);
            v->index = elf_u16(big, p + i + 4);
            v->deps.elem = TYPE_STRING;
            /* The first name is the version's and the rest are its deps. */
            Int nnames = elf_verdaux(big, d, i + aux, cnt, str, fa, NULL);
            if (nnames > 0) {
                Str *names =
                    (Str *)mem_alloc(fa, (size_t)nnames * sizeof(Str), _Alignof(Str));
                if (names == NULL ||
                    elf_verdaux(big, d, i + aux, cnt, str, fa, names) < 0)
                    return burrow_err_out_of_memory;
                v->name = names[0];
                if (nnames > 1) {
                    v->deps.p = names + 1;
                    v->deps.len = v->deps.cap = nnames - 1;
                }
            }
        }
        n++;
        if (next == 0)
            break;
        i += next;
    }
    *count = n;
    return BURROW_NO_ERROR;
}

/* The same for the verneed table. */
static Error elf_walk_verneed(ElfFile *f, const ElfSection *vn, Slice d, Slice str,
                              Alloc *fa, ElfDynamicVersionNeed *out, Int *count) {
    bool big = elf_big(f);
    const Byte *p = (const Byte *)d.p;
    Int n = 0;
    for (int64_t i = 0; i + 16 <= d.len;) {
        uint16_t vers = elf_u16(big, p + i);
        if (vers != 1)
            return elf_fe_u((int64_t)(vn->section_header.offset + (uint64_t)i),
                            BURROW_S("unexpected dynamic need version"), vers);
        uint16_t cnt = elf_u16(big, p + i + 2);
        uint32_t fileoff = elf_u32(big, p + i + 4);
        uint32_t aux = elf_u32(big, p + i + 8);
        uint32_t next = elf_u32(big, p + i + 12);
        if (out != NULL) {
            ElfDynamicVersionNeed *v = &out[n];
            Str file;
            elf_get_string(str, fileoff, &file);
            v->name = str_clone(fa, file);
            if (file.len > 0 && v->name.p == NULL)
                return burrow_err_out_of_memory;
            v->needs.elem = TYPE_ELF_DYNAMIC_VERSION_DEP;
            /* Two rounds: count the deps, then fill them in. */
            ElfDynamicVersionDep *deps = NULL;
            for (int round = 0; round < 2; round++) {
                Int ndeps = 0;
                int64_t j = i + aux;
                for (int c = 0; c < cnt; c++) {
                    if (j + 16 > d.len)
                        break;
                    if (deps != NULL) {
                        ElfDynamicVersionDep *dep = &deps[ndeps];
                        Str name;
                        dep->flags = elf_u16(big, p + j + 4);
                        dep->index = elf_u16(big, p + j + 6);
                        elf_get_string(str, elf_u32(big, p + j + 8), &name);
                        dep->dep = str_clone(fa, name);
                        if (name.len > 0 && dep->dep.p == NULL)
                            return burrow_err_out_of_memory;
                    }
                    ndeps++;
                    uint32_t dnext = elf_u32(big, p + j + 12);
                    if (dnext == 0)
                        break;
                    j += dnext;
                }
                if (ndeps == 0)
                    break;
                if (deps == NULL) {
                    deps = (ElfDynamicVersionDep *)mem_alloc(
                        fa, (size_t)ndeps * sizeof *deps,
                        _Alignof(ElfDynamicVersionDep));
                    if (deps == NULL)
                        return burrow_err_out_of_memory;
                    v->needs.p = deps;
                    v->needs.len = v->needs.cap = ndeps;
                }
            }
        }
        n++;
        if (next == 0)
            break;
        i += next;
    }
    *count = n;
    return BURROW_NO_ERROR;
}

/* File.dynamicVersions: reads the verdef table into f, once. */
static Error elf_dynamic_versions_init(ElfFile *f, Slice str) {
    if (f->dyn_vers.p != NULL)
        return BURROW_NO_ERROR;
    ElfSection *vd = elf_file_section_by_type(f, ELF_SHT_GNU_VERDEF);
    if (vd == NULL)
        return BURROW_NO_ERROR;
    Alloc *fa = arena_allocator(&f->arena);
    Arena tmp;
    arena_init(&tmp, f->a, 0);
    Error ignored = BURROW_NO_ERROR;
    Slice d = elf_section_data(vd, arena_allocator(&tmp), &ignored);
    Int n = 0;
    Error e = elf_walk_verdef(f, vd, d, str, fa, NULL, &n);
    if (!BURROW_FAILED(e) && n > 0) {
        ElfDynamicVersion *vers = (ElfDynamicVersion *)mem_alloc(
            fa, (size_t)n * sizeof *vers, _Alignof(ElfDynamicVersion));
        if (vers == NULL)
            e = burrow_err_out_of_memory;
        else
            e = elf_walk_verdef(f, vd, d, str, fa, vers, &n);
        if (!BURROW_FAILED(e)) {
            f->dyn_vers.p = vers;
            f->dyn_vers.len = f->dyn_vers.cap = n;
        }
    }
    arena_free(&tmp);
    return e;
}

/* File.dynamicVersionNeeds: reads the verneed table into f, once. */
static Error elf_dynamic_version_needs_init(ElfFile *f, Slice str) {
    if (f->dyn_ver_needs.p != NULL)
        return BURROW_NO_ERROR;
    ElfSection *vn = elf_file_section_by_type(f, ELF_SHT_GNU_VERNEED);
    if (vn == NULL)
        return BURROW_NO_ERROR;
    Alloc *fa = arena_allocator(&f->arena);
    Arena tmp;
    arena_init(&tmp, f->a, 0);
    Error ignored = BURROW_NO_ERROR;
    Slice d = elf_section_data(vn, arena_allocator(&tmp), &ignored);
    Int n = 0;
    Error e = elf_walk_verneed(f, vn, d, str, fa, NULL, &n);
    if (!BURROW_FAILED(e) && n > 0) {
        ElfDynamicVersionNeed *needs = (ElfDynamicVersionNeed *)mem_alloc(
            fa, (size_t)n * sizeof *needs, _Alignof(ElfDynamicVersionNeed));
        if (needs == NULL)
            e = burrow_err_out_of_memory;
        else
            e = elf_walk_verneed(f, vn, d, str, fa, needs, &n);
        if (!BURROW_FAILED(e)) {
            f->dyn_ver_needs.p = needs;
            f->dyn_ver_needs.len = f->dyn_ver_needs.cap = n;
        }
    }
    arena_free(&tmp);
    return e;
}

/* File.gnuVersionInit: reads the version tables, and says whether the file
 * has them. */
static bool elf_gnu_version_init(ElfFile *f, Slice str, Error *err) {
    /* Versym parallels symbol table, indexing into verneed. */
    ElfSection *vs = elf_file_section_by_type(f, ELF_SHT_GNU_VERSYM);
    if (vs == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return false;
    }
    if (!f->have_versym) {
        Error ignored = BURROW_NO_ERROR;
        Slice d = elf_section_data(vs, arena_allocator(&f->arena), &ignored);
        f->gnu_versym = d;
        f->have_versym = true;
    }
    Error e = elf_dynamic_versions_init(f, str);
    if (!BURROW_FAILED(e))
        e = elf_dynamic_version_needs_init(f, str);
    BURROW_OUT(err, e);
    return !BURROW_FAILED(e);
}

/* File.gnuVersion: the version of the symbol at index i of the symbol table,
 * without the null symbol. */
static bool elf_gnu_version(const ElfFile *f, Int i, ElfVersionIndex *vi, Str *version,
                            Str *library) {
    *vi = 0;
    *version = BURROW_STR_EMPTY;
    *library = BURROW_STR_EMPTY;
    /* Each entry is two bytes; skip undef entry at beginning. */
    i = (i + 1) * 2;
    if (i + 2 > f->gnu_versym.len)
        return false;
    ElfVersionIndex v = elf_u16(elf_big(f), (const Byte *)f->gnu_versym.p + i);
    uint16_t ndx = elf_version_index_index(v);
    if (ndx == 0 || ndx == 1) {
        *vi = v;
        return true;
    }
    const ElfDynamicVersionNeed *needs =
        (const ElfDynamicVersionNeed *)f->dyn_ver_needs.p;
    for (Int k = 0; k < f->dyn_ver_needs.len; k++) {
        const ElfDynamicVersionDep *deps =
            (const ElfDynamicVersionDep *)needs[k].needs.p;
        for (Int m = 0; m < needs[k].needs.len; m++) {
            if (ndx == deps[m].index) {
                *vi = v;
                *version = deps[m].dep;
                *library = needs[k].name;
                return true;
            }
        }
    }
    const ElfDynamicVersion *vers = (const ElfDynamicVersion *)f->dyn_vers.p;
    for (Int k = 0; k < f->dyn_vers.len; k++) {
        if (ndx == vers[k].index) {
            *vi = v;
            *version = vers[k].name;
            return true;
        }
    }
    return false;
}

Slice elf_file_symbols(ElfFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    return elf_get_symbols(f, ELF_SHT_SYMTAB, a, a, NULL, err);
}

Slice elf_file_dynamic_symbols(ElfFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_ELF_SYMBOL};
    Slice str;
    Error e = BURROW_NO_ERROR;
    Slice syms = elf_get_symbols(f, ELF_SHT_DYNSYM, a, a, &str, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    bool has_versions = elf_gnu_version_init(f, str, &e);
    if (BURROW_FAILED(e)) {
        elf_free_symbols(a, syms);
        elf_free_bytes(a, str);
        BURROW_OUT(err, e);
        return none;
    }
    if (has_versions) {
        ElfSymbol *s = (ElfSymbol *)syms.p;
        for (Int i = 0; i < syms.len; i++)
            s[i].has_version = elf_gnu_version(f, i, &s[i].version_index, &s[i].version,
                                               &s[i].library);
    }
    if (syms.len == 0)
        elf_free_bytes(a, str);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return syms;
}

Slice elf_file_imported_symbols(ElfFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_ELF_IMPORTED_SYMBOL};
    Arena tmp;
    arena_init(&tmp, a, 0);
    Slice str;
    Error e = BURROW_NO_ERROR;
    Slice syms = elf_get_symbols(f, ELF_SHT_DYNSYM, arena_allocator(&tmp), a, &str, &e);
    if (!BURROW_FAILED(e))
        elf_gnu_version_init(f, str, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        elf_free_bytes(a, str);
        BURROW_OUT(err, e);
        return none;
    }
    const ElfSymbol *s = (const ElfSymbol *)syms.p;
    Int n = 0;
    for (Int i = 0; i < syms.len; i++) {
        if (elf_st_bind(s[i].info) == ELF_STB_GLOBAL && s[i].section == ELF_SHN_UNDEF)
            n++;
    }
    ElfImportedSymbol *all = NULL;
    if (n > 0) {
        all = (ElfImportedSymbol *)mem_alloc(a, (size_t)n * sizeof *all,
                                             _Alignof(ElfImportedSymbol));
        if (all == NULL) {
            arena_free(&tmp);
            elf_free_bytes(a, str);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
    }
    for (Int i = 0, k = 0; i < syms.len; i++) {
        if (elf_st_bind(s[i].info) != ELF_STB_GLOBAL || s[i].section != ELF_SHN_UNDEF)
            continue;
        ElfVersionIndex vi;
        all[k].name = s[i].name;
        elf_gnu_version(f, i, &vi, &all[k].version, &all[k].library);
        k++;
    }
    arena_free(&tmp);
    if (n == 0)
        elf_free_bytes(a, str);
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {all, n, n, TYPE_ELF_IMPORTED_SYMBOL};
    return out;
}

Slice elf_file_dynamic_versions(ElfFile *f, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    if (f->dyn_vers.p == NULL) {
        Slice none = {NULL, 0, 0, TYPE_ELF_DYNAMIC_VERSION};
        Arena tmp;
        arena_init(&tmp, f->a, 0);
        Slice str;
        Error e = BURROW_NO_ERROR;
        elf_get_symbols(f, ELF_SHT_DYNSYM, arena_allocator(&tmp), arena_allocator(&tmp),
                        &str, &e);
        bool has_versions = false;
        if (!BURROW_FAILED(e))
            has_versions = elf_gnu_version_init(f, str, &e);
        arena_free(&tmp);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return none;
        }
        if (!has_versions) {
            BURROW_OUT(err, elf_err_no_verdef_table);
            return none;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f->dyn_vers;
}

Slice elf_file_dynamic_version_needs(ElfFile *f, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    if (f->dyn_ver_needs.p == NULL) {
        Slice none = {NULL, 0, 0, TYPE_ELF_DYNAMIC_VERSION_NEED};
        Arena tmp;
        arena_init(&tmp, f->a, 0);
        Slice str;
        Error e = BURROW_NO_ERROR;
        elf_get_symbols(f, ELF_SHT_DYNSYM, arena_allocator(&tmp), arena_allocator(&tmp),
                        &str, &e);
        bool has_versions = false;
        if (!BURROW_FAILED(e))
            has_versions = elf_gnu_version_init(f, str, &e);
        arena_free(&tmp);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return none;
        }
        if (!has_versions) {
            BURROW_OUT(err, elf_err_no_verneed_table);
            return none;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f->dyn_ver_needs;
}

/* --------------------------------------------------------------- dynamic */

/* The dynamic section's data, in a, checked for whole entries. The nil slice
 * and no error when the file has none. */
static Slice elf_dynamic_data(ElfFile *f, Alloc *a, ElfSection **dsp, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    ElfSection *ds = elf_file_section_by_type(f, ELF_SHT_DYNAMIC);
    *dsp = ds;
    if (ds == NULL) {
        /* Not dynamic, so no libraries. */
        BURROW_OUT(err, BURROW_NO_ERROR);
        return none;
    }
    Error e = BURROW_NO_ERROR;
    Slice d = elf_section_data(ds, a, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    Int dynsize = f->file_header.cls == ELF_ELFCLASS64 ? 16 : 8;
    if (d.len % dynsize != 0) {
        BURROW_OUT(err, elf_err_dyn_len);
        return none;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d;
}

/* Entry k of the dynamic section in d. */
static ElfDynTag elf_dyn_entry(const ElfFile *f, Slice d, Int k, uint64_t *v) {
    bool big = elf_big(f);
    if (f->file_header.cls == ELF_ELFCLASS64) {
        const Byte *p = (const Byte *)d.p + k * 16;
        *v = elf_u64(big, p + 8);
        return (ElfDynTag)elf_u64(big, p);
    }
    const Byte *p = (const Byte *)d.p + k * 8;
    *v = elf_u32(big, p + 4);
    return (ElfDynTag)elf_u32(big, p);
}

static Int elf_dyn_count(const ElfFile *f, Slice d) {
    if (f->file_header.cls == ELF_ELFCLASS64)
        return d.len / 16;
    if (f->file_header.cls == ELF_ELFCLASS32)
        return d.len / 8;
    return 0;
}

Slice elf_file_dyn_string(ElfFile *f, Alloc *a, ElfDynTag tag, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_STRING};
    switch (tag) {
    case ELF_DT_NEEDED:
    case ELF_DT_SONAME:
    case ELF_DT_RPATH:
    case ELF_DT_RUNPATH:
        break;
    default: {
        Str name = elf_dyn_tag_string(tag, error_allocator());
        BURROW_OUT(err, fmt_errorf_v("non-string-valued tag %s", name));
        return none;
    }
    }
    Arena tmp;
    arena_init(&tmp, a, 0);
    ElfSection *ds;
    Error e = BURROW_NO_ERROR;
    Slice d = elf_dynamic_data(f, arena_allocator(&tmp), &ds, &e);
    if (BURROW_FAILED(e) || ds == NULL) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }
    Slice str = elf_string_table(f, ds->section_header.link, a, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }
    Int ndyn = elf_dyn_count(f, d);
    Int n = 0;
    for (Int k = 0; k < ndyn; k++) {
        uint64_t v;
        Str s;
        if (elf_dyn_entry(f, d, k, &v) == tag && elf_get_string(str, v, &s))
            n++;
    }
    Str *all = NULL;
    if (n > 0) {
        all = (Str *)mem_alloc(a, (size_t)n * sizeof *all, _Alignof(Str));
        if (all == NULL) {
            arena_free(&tmp);
            elf_free_bytes(a, str);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
    }
    for (Int k = 0, m = 0; k < ndyn && m < n; k++) {
        uint64_t v;
        if (elf_dyn_entry(f, d, k, &v) == tag && elf_get_string(str, v, &all[m]))
            m++;
    }
    arena_free(&tmp);
    if (n == 0)
        elf_free_bytes(a, str);
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {all, n, n, TYPE_STRING};
    return out;
}

Slice elf_file_imported_libraries(ElfFile *f, Alloc *a, Error *err) {
    return elf_file_dyn_string(f, a, ELF_DT_NEEDED, err);
}

Slice elf_file_dyn_value(ElfFile *f, Alloc *a, ElfDynTag tag, Error *err) {
    if (f == NULL)
        elf_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_UINT64};
    Arena tmp;
    arena_init(&tmp, a, 0);
    ElfSection *ds;
    Error e = BURROW_NO_ERROR;
    Slice d = elf_dynamic_data(f, arena_allocator(&tmp), &ds, &e);
    if (BURROW_FAILED(e) || ds == NULL) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }
    /* Parse the .dynamic section as a string of bytes. */
    Int ndyn = elf_dyn_count(f, d);
    Int n = 0;
    for (Int k = 0; k < ndyn; k++) {
        uint64_t v;
        if (elf_dyn_entry(f, d, k, &v) == tag)
            n++;
    }
    uint64_t *vals = NULL;
    if (n > 0) {
        vals = (uint64_t *)mem_alloc(a, (size_t)n * sizeof *vals, _Alignof(uint64_t));
        if (vals == NULL) {
            arena_free(&tmp);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
    }
    for (Int k = 0, m = 0; k < ndyn; k++) {
        uint64_t v;
        if (elf_dyn_entry(f, d, k, &v) == tag)
            vals[m++] = v;
    }
    arena_free(&tmp);
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {vals, n, n, TYPE_UINT64};
    return out;
}
