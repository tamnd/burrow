/* Derived from Go's src/debug/buildinfo/buildinfo.go. Go source: go1.27.1.
 *
 * Everything the search and the reads make is temporary and goes in an arena
 * that is given back before buildinfo_read returns, along with the parsed
 * file, so only the DebugBuildInfo is left.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/buildinfo.h"

#include "buildinfo_internal.h"
#include "objfile_internal.h"
#include "xcoff_internal.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/debug/elf.h"
#include "burrow/debug/macho.h"
#include "burrow/debug/pe.h"
#include "burrow/debug/plan9obj.h"
#include "burrow/declare.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/runtime/debug.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BI_ERR(name, text)                                                             \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

BI_ERR(bi_err_unrecognized, "unrecognized file format");
BI_ERR(bi_err_not_go, "not a Go executable");
BI_ERR(bi_err_not_mapped, "address not mapped");

/* "\xff Go buildinf:", which starts the 32 bytes of the header, at an address
 * that is a multiple of 16. */
static const Byte bi_magic[14] = {0xff, ' ', 'G', 'o', ' ', 'b', 'u',
                                  'i',  'l', 'd', 'i', 'n', 'f', ':'};

enum {
    BI_ALIGN = 16,
    BI_HEADER_SIZE = 32,
    BI_PTR_SIZE_OFFSET = 14,
    BI_FLAGS_OFFSET = 15,
    BI_VERS_PTR_OFFSET = 16,
    BI_FLAGS_ENDIAN_MASK = 0x1,
    BI_FLAGS_ENDIAN_BIG = 0x1,
    BI_FLAGS_VERSION_MASK = 0x2,
    BI_FLAGS_VERSION_INL = 0x2,
};

static bool bi_same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static Slice bi_bytes(Byte *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

/* ------------------------------------------------------------ the formats */

typedef enum BiKind {
    BI_ELF,
    BI_PE,
    BI_MACHO,
    BI_FAT,
    BI_XCOFF,
    BI_PLAN9,
} BiKind;

typedef struct BiFile {
    BiKind kind;
    void *f;
    MachoFile *macho;
} BiFile;

static bool bi_elf_reader(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err) {
    ElfFile *f = (ElfFile *)ctx;
    for (Int i = 0; i < f->progs.len; i++) {
        ElfProg *p = ((ElfProg **)f->progs.p)[i];
        uint64_t vaddr = p->prog_header.vaddr;
        uint64_t end = vaddr + p->prog_header.filesz;
        if (vaddr <= addr && addr <= end - 1) {
            *sr = io_new_section_reader(p->reader_at, (int64_t)(addr - vaddr),
                                        (int64_t)(end - addr));
            return true;
        }
    }
    BURROW_OUT(err, bi_err_unrecognized);
    return false;
}

static void bi_elf_start(ElfFile *f, uint64_t *addr, uint64_t *size) {
    for (Int i = 0; i < f->sections.len; i++) {
        ElfSection *s = ((ElfSection **)f->sections.p)[i];
        if (str_eq(s->section_header.name, BURROW_S(".go.buildinfo"))) {
            *addr = s->section_header.addr;
            *size = s->section_header.size;
            return;
        }
    }
}

static uint64_t bi_pe_image_base(const PeFile *f) {
    const Any oh = f->optional_header;
    if (oh.t == TYPE_PE_OPTIONAL_HEADER32 && oh.data != NULL)
        return ((const PeOptionalHeader32 *)oh.data)->image_base;
    if (oh.t == TYPE_PE_OPTIONAL_HEADER64 && oh.data != NULL)
        return ((const PeOptionalHeader64 *)oh.data)->image_base;
    return 0;
}

/* The section arithmetic is in 32 bits, as Go's is, and wraps as it does. */
static bool bi_pe_reader(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err) {
    PeFile *f = (PeFile *)ctx;
    addr -= bi_pe_image_base(f);
    for (Int i = 0; i < f->sections.len; i++) {
        PeSection *s = ((PeSection **)f->sections.p)[i];
        uint32_t va = s->section_header.virtual_address;
        uint32_t end = va + s->section_header.size;
        if ((uint64_t)va <= addr && addr <= (uint64_t)(uint32_t)(end - 1)) {
            *sr = io_new_section_reader(s->reader_at, (int64_t)(addr - va),
                                        (int64_t)((uint64_t)end - addr));
            return true;
        }
    }
    BURROW_OUT(err, bi_err_unrecognized);
    return false;
}

/* The first writable data section. */
static void bi_pe_start(PeFile *f, uint64_t *addr, uint64_t *size) {
    const uint32_t align32 = 0x600000;
    const uint32_t want = PE_IMAGE_SCN_CNT_INITIALIZED_DATA | PE_IMAGE_SCN_MEM_READ |
                          (uint32_t)PE_IMAGE_SCN_MEM_WRITE;
    for (Int i = 0; i < f->sections.len; i++) {
        PeSection *s = ((PeSection **)f->sections.p)[i];
        const PeSectionHeader *h = &s->section_header;
        if (h->virtual_address != 0 && h->size != 0 &&
            (h->characteristics & ~align32) == want) {
            *addr = (uint64_t)h->virtual_address + bi_pe_image_base(f);
            *size = h->virtual_size;
            return;
        }
    }
}

static bool bi_macho_reader(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err) {
    MachoFile *f = (MachoFile *)ctx;
    for (Int i = 0; i < f->loads.len; i++) {
        MachoLoad l = ((MachoLoad *)f->loads.p)[i];
        if (l->kind != MACHO_LOAD_KIND_SEGMENT)
            continue;
        MachoSegment *seg = (MachoSegment *)l;
        uint64_t start = seg->segment_header.addr;
        uint64_t end = start + seg->segment_header.filesz;
        if (start <= addr && addr <= end - 1) {
            if (str_eq(seg->segment_header.name, BURROW_S("__PAGEZERO")))
                continue;
            *sr = io_new_section_reader(seg->reader_at, (int64_t)(addr - start),
                                        (int64_t)(end - addr));
            return true;
        }
    }
    BURROW_OUT(err, bi_err_unrecognized);
    return false;
}

static void bi_macho_start(MachoFile *f, uint64_t *addr, uint64_t *size) {
    for (Int i = 0; i < f->sections.len; i++) {
        MachoSection *s = ((MachoSection **)f->sections.p)[i];
        if (str_eq(s->section_header.name, BURROW_S("__go_buildinfo"))) {
            *addr = s->section_header.addr;
            *size = s->section_header.size;
            return;
        }
    }
}

static bool bi_xcoff_reader(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err) {
    XcoffFile *f = (XcoffFile *)ctx;
    for (Int i = 0; i < f->sections.len; i++) {
        XcoffSection *s = ((XcoffSection **)f->sections.p)[i];
        uint64_t va = s->section_header.virtual_address;
        uint64_t end = va + s->section_header.size;
        if (va <= addr && addr <= end - 1) {
            *sr = io_new_section_reader(s->reader_at, (int64_t)(addr - va),
                                        (int64_t)(end - addr));
            return true;
        }
    }
    BURROW_OUT(err, bi_err_not_mapped);
    return false;
}

/* The arithmetic is in 32 bits, as for PE. */
static bool bi_plan9_reader(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err) {
    Plan9objFile *f = (Plan9objFile *)ctx;
    for (Int i = 0; i < f->sections.len; i++) {
        Plan9objSection *s = ((Plan9objSection **)f->sections.p)[i];
        uint32_t off = s->section_header.offset;
        uint32_t end = off + s->section_header.size;
        if ((uint64_t)off <= addr && addr <= (uint64_t)(uint32_t)(end - 1)) {
            *sr = io_new_section_reader(s->reader_at, (int64_t)(addr - off),
                                        (int64_t)((uint64_t)end - addr));
            return true;
        }
    }
    BURROW_OUT(err, bi_err_not_mapped);
    return false;
}

static bool bi_has_plan9_magic(const Byte *ident) {
    uint32_t m = (uint32_t)ident[0] << 24 | (uint32_t)ident[1] << 16 |
                 (uint32_t)ident[2] << 8 | (uint32_t)ident[3];
    return m == PLAN9OBJ_MAGIC386 || m == PLAN9OBJ_MAGIC_AMD64 ||
           m == PLAN9OBJ_MAGIC_ARM;
}

static bool bi_prefix(const Byte *ident, const char *p, size_t n) {
    return memcmp(ident, p, n) == 0;
}

/* Reads the headers of whichever kind of file r is into *bf and sets up x,
 * or gives false for a file that is none of them, or a malformed one. */
static bool bi_open(Alloc *a, IoReaderAt r, BiFile *bf, BuildinfoExe *x) {
    Byte ident[16];
    memset(ident, 0, sizeof ident);
    Error e = BURROW_NO_ERROR;
    Int n = r.vt->read_at(r.data, bi_bytes(ident, 16), 0, &e);
    if (n < 16 || BURROW_FAILED(e))
        return false;
    if (bi_prefix(ident,
                  "\x7f"
                  "ELF",
                  4)) {
        ElfFile *f = elf_new_file(a, r, &e);
        if (f == NULL)
            return false;
        *bf = (BiFile){BI_ELF, f, NULL};
        *x = (BuildinfoExe){f, bi_elf_reader};
    } else if (bi_prefix(ident, "MZ", 2)) {
        PeFile *f = pe_new_file(a, r, &e);
        if (f == NULL)
            return false;
        *bf = (BiFile){BI_PE, f, NULL};
        *x = (BuildinfoExe){f, bi_pe_reader};
    } else if (bi_prefix(ident, "\xfe\xed\xfa", 3) ||
               bi_prefix(ident + 1, "\xfa\xed\xfe", 3)) {
        MachoFile *f = macho_new_file(a, r, &e);
        if (f == NULL)
            return false;
        *bf = (BiFile){BI_MACHO, f, f};
        *x = (BuildinfoExe){f, bi_macho_reader};
    } else if (bi_prefix(ident, "\xca\xfe\xba\xbe", 4) ||
               bi_prefix(ident, "\xca\xfe\xba\xbf", 4)) {
        MachoFatFile *f = macho_new_fat_file(a, r, &e);
        if (f == NULL)
            return false;
        if (f->arches.len == 0) {
            macho_fat_file_free(f);
            return false;
        }
        MachoFile *first = ((MachoFatArch *)f->arches.p)[0].file;
        *bf = (BiFile){BI_FAT, f, first};
        *x = (BuildinfoExe){first, bi_macho_reader};
    } else if (bi_prefix(ident, "\x01\xdf", 2) || bi_prefix(ident, "\x01\xf7", 2)) {
        XcoffFile *f = burrow__xcoff_new_file(a, r, &e);
        if (f == NULL)
            return false;
        *bf = (BiFile){BI_XCOFF, f, NULL};
        *x = (BuildinfoExe){f, bi_xcoff_reader};
    } else if (bi_has_plan9_magic(ident)) {
        Plan9objFile *f = plan9obj_new_file(a, r, &e);
        if (f == NULL)
            return false;
        *bf = (BiFile){BI_PLAN9, f, NULL};
        *x = (BuildinfoExe){f, bi_plan9_reader};
    } else {
        return false;
    }
    return true;
}

/* DataStart: where the build information should be, which is a section of
 * its own, or the first writable data, near the start of which the linker
 * puts it. */
static void bi_data_start(const BiFile *bf, uint64_t *addr, uint64_t *size) {
    *addr = 0;
    *size = 0;
    switch (bf->kind) {
    case BI_ELF:
        bi_elf_start((ElfFile *)bf->f, addr, size);
        break;
    case BI_PE:
        bi_pe_start((PeFile *)bf->f, addr, size);
        break;
    case BI_MACHO:
    case BI_FAT:
        bi_macho_start(bf->macho, addr, size);
        break;
    case BI_XCOFF: {
        XcoffSection *s =
            burrow__xcoff_file_section_by_type((XcoffFile *)bf->f, XCOFF_STYP_DATA);
        if (s != NULL) {
            *addr = s->section_header.virtual_address;
            *size = s->section_header.size;
        }
        break;
    }
    case BI_PLAN9:
    default: {
        Plan9objSection *s =
            plan9obj_file_section((Plan9objFile *)bf->f, BURROW_S("data"));
        if (s != NULL) {
            *addr = s->section_header.offset;
            *size = s->section_header.size;
        }
        break;
    }
    }
}

static void bi_close(const BiFile *bf) {
    switch (bf->kind) {
    case BI_ELF:
        elf_file_free((ElfFile *)bf->f);
        break;
    case BI_PE:
        pe_file_free((PeFile *)bf->f);
        break;
    case BI_MACHO:
        macho_file_free((MachoFile *)bf->f);
        break;
    case BI_FAT:
        macho_fat_file_free((MachoFatFile *)bf->f);
        break;
    case BI_XCOFF:
        burrow__xcoff_file_free((XcoffFile *)bf->f);
        break;
    case BI_PLAN9:
    default:
        plan9obj_file_free((Plan9objFile *)bf->f);
        break;
    }
}

/* ------------------------------------------------------------- the reads */

/* readData: up to size bytes from addr, in a. */
static Slice bi_read_data(Alloc *a, const BuildinfoExe *x, uint64_t addr, uint64_t size,
                          Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    IoSectionReader sr = {{NULL, NULL}, 0, 0, 0, 0};
    Error e = BURROW_NO_ERROR;
    if (!x->data_reader(x->ctx, addr, &sr, &e)) {
        BURROW_OUT(err, e);
        return none;
    }
    Slice b = burrow__saferio_read_data_at(a, io_section_reader_as_io_reader_at(&sr),
                                           size, 0, &e);
    if (b.len > 0 && bi_same_error(e, io_eof))
        e = BURROW_NO_ERROR;
    BURROW_OUT(err, e);
    return b;
}

/* readDataInto: as much of b as there is from addr. */
static Int bi_read_data_into(const BuildinfoExe *x, uint64_t addr, Slice b,
                             Error *err) {
    IoSectionReader sr = {{NULL, NULL}, 0, 0, 0, 0};
    Error e = BURROW_NO_ERROR;
    if (!x->data_reader(x->ctx, addr, &sr, &e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    Int n = io_section_reader_read_at(&sr, b, 0, &e);
    if (n > 0 && bi_same_error(e, io_eof))
        e = BURROW_NO_ERROR;
    BURROW_OUT(err, e);
    return n;
}

uint64_t burrow__buildinfo_search_magic(Alloc *a, const BuildinfoExe *x, uint64_t start,
                                        uint64_t size, Error *err) {
    uint64_t end = start + size;
    if (end < start) {
        BURROW_OUT(err, bi_err_unrecognized);
        return 0;
    }
    /* The magic cannot be in the unaligned part at the start. */
    start = (start + BI_ALIGN - 1) & ~(uint64_t)(BI_ALIGN - 1);
    if (start >= end) {
        BURROW_OUT(err, bi_err_not_go);
        return 0;
    }
    Slice magic = {(void *)(uintptr_t)bi_magic, 14, 14, TYPE_BYTE};
    Byte *buf = NULL;
    size_t buf_size = 0;
    Error result = bi_err_not_go;
    uint64_t found = 0;
    while (start < end) {
        /* The magic is aligned to 16 bytes, so it is never cut in two by the
         * edge of a chunk. */
        uint64_t remaining = end - start;
        uint64_t chunk = BURROW__BUILDINFO_SEARCH_CHUNK;
        if (chunk > remaining)
            chunk = remaining;
        if (buf == NULL) {
            buf_size = (size_t)chunk;
            buf = (Byte *)mem_alloc(a, buf_size, 1);
            if (buf == NULL) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return 0;
            }
        } else {
            memset(buf, 0, (size_t)chunk);
        }
        Error e = BURROW_NO_ERROR;
        Int n = bi_read_data_into(x, start, bi_bytes(buf, (Int)chunk), &e);
        if (BURROW_FAILED(e)) {
            /* EOF before the magic means it is not a Go binary. */
            if (!bi_same_error(e, io_eof))
                result = e;
            goto done;
        }
        Int pos = 0;
        while (pos < n) {
            Int i = bytes_index(bi_bytes(buf + pos, n - pos), magic);
            if (i < 0)
                break;
            if (remaining - (uint64_t)(pos + i) < BI_HEADER_SIZE)
                goto done;
            if ((pos + i) % BI_ALIGN != 0) {
                /* Misaligned, so keep looking from the next aligned byte. */
                Int next = (i + BI_ALIGN - 1) & ~(Int)(BI_ALIGN - 1);
                if (next > n - pos)
                    goto done;
                pos += next;
                continue;
            }
            found = start + (uint64_t)(pos + i);
            result = BURROW_NO_ERROR;
            goto done;
        }
        start += chunk;
    }
done:
    mem_free(a, buf, buf_size, 1);
    BURROW_OUT(err, result);
    return found;
}

/* decodeString: a varint length and that many bytes, at addr, in a. The
 * address after them goes in *next. */
static Str bi_decode_string(Alloc *a, const BuildinfoExe *x, uint64_t addr,
                            uint64_t *next, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice b = bi_read_data(a, x, addr, 10, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, bi_same_error(e, io_eof) ? bi_err_not_go : e);
        return BURROW_STR_EMPTY;
    }
    Int n = 0;
    uint64_t length = binary_uvarint(b, &n);
    if (n <= 0) {
        BURROW_OUT(err, bi_err_not_go);
        return BURROW_STR_EMPTY;
    }
    addr += (uint64_t)n;
    b = bi_read_data(a, x, addr, length, &e);
    if (BURROW_FAILED(e)) {
        /* An unexpected EOF is a length too large to allocate, which is
         * clearly bogus. */
        bool bogus =
            bi_same_error(e, io_eof) || bi_same_error(e, io_err_unexpected_eof);
        BURROW_OUT(err, bogus ? bi_err_not_go : e);
        return BURROW_STR_EMPTY;
    }
    if ((uint64_t)b.len < length) {
        BURROW_OUT(err, bi_err_not_go);
        return BURROW_STR_EMPTY;
    }
    *next = addr + length;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Str){(const Byte *)b.p, b.len};
}

static uint64_t bi_read_ptr(const Byte *p, Int size, bool big) {
    uint64_t v = 0;
    for (Int i = 0; i < size; i++) {
        Int k = big ? i : size - 1 - i;
        v = v << 8 | p[k];
    }
    return v;
}

/* readString: the Go string whose header is at addr, or "" for anything that
 * cannot be read. */
static Str bi_read_string(Alloc *a, const BuildinfoExe *x, Int ptr_size, bool big,
                          uint64_t addr) {
    Error e = BURROW_NO_ERROR;
    Slice hdr = bi_read_data(a, x, addr, (uint64_t)(2 * ptr_size), &e);
    if (BURROW_FAILED(e) || hdr.len < 2 * ptr_size || hdr.p == NULL)
        return BURROW_STR_EMPTY;
    const Byte *h = (const Byte *)hdr.p;
    uint64_t data_addr = bi_read_ptr(h, ptr_size, big);
    uint64_t data_len = bi_read_ptr(h + ptr_size, ptr_size, big);
    Slice data = bi_read_data(a, x, data_addr, data_len, &e);
    if (BURROW_FAILED(e) || (uint64_t)data.len < data_len)
        return BURROW_STR_EMPTY;
    return (Str){(const Byte *)data.p, data.len};
}

/* readRawBuildInfo: the Go version and the module information of the binary
 * r reads, in a. */
static bool bi_read_raw(Alloc *a, IoReaderAt r, Str *vers, Str *mod, Error *err) {
    BiFile bf = {BI_ELF, NULL, NULL};
    BuildinfoExe x = {NULL, NULL};
    if (!bi_open(a, r, &bf, &x)) {
        BURROW_OUT(err, bi_err_unrecognized);
        return false;
    }
    bool ok = false;
    Error e = BURROW_NO_ERROR;
    uint64_t data_addr = 0, data_size = 0;
    bi_data_start(&bf, &data_addr, &data_size);
    if (data_size == 0) {
        e = bi_err_not_go;
        goto done;
    }
    uint64_t addr = burrow__buildinfo_search_magic(a, &x, data_addr, data_size, &e);
    if (BURROW_FAILED(e))
        goto done;
    Slice header = bi_read_data(a, &x, addr, BI_HEADER_SIZE, &e);
    if (BURROW_FAILED(e)) {
        if (bi_same_error(e, io_eof))
            e = bi_err_not_go;
        goto done;
    }
    if (header.len < BI_HEADER_SIZE || header.p == NULL) {
        e = bi_err_not_go;
        goto done;
    }
    /* A 32-byte header: the magic, a pointer size and flags, then, before Go
     * 1.18, pointers to the version and the module information as Go strings.
     * From Go 1.18 on the two strings follow the header, each a varint length
     * and the bytes. */
    const Byte *h = (const Byte *)header.p;
    Byte flags = h[BI_FLAGS_OFFSET];
    if ((flags & BI_FLAGS_VERSION_MASK) == BI_FLAGS_VERSION_INL) {
        uint64_t next = 0;
        *vers = bi_decode_string(a, &x, addr + BI_HEADER_SIZE, &next, &e);
        if (BURROW_FAILED(e))
            goto done;
        *mod = bi_decode_string(a, &x, next, &next, &e);
        if (BURROW_FAILED(e))
            goto done;
    } else {
        Int ptr_size = h[BI_PTR_SIZE_OFFSET];
        bool big = (flags & BI_FLAGS_ENDIAN_MASK) == BI_FLAGS_ENDIAN_BIG;
        if (ptr_size != 4 && ptr_size != 8) {
            e = bi_err_not_go;
            goto done;
        }
        *vers = bi_read_string(a, &x, ptr_size, big,
                               bi_read_ptr(h + BI_VERS_PTR_OFFSET, ptr_size, big));
        *mod = bi_read_string(
            a, &x, ptr_size, big,
            bi_read_ptr(h + BI_VERS_PTR_OFFSET + ptr_size, ptr_size, big));
    }
    if (vers->len == 0) {
        e = bi_err_not_go;
        goto done;
    }
    /* The module information is framed by 16 bytes at each end, which are
     * cmd/go's infoStart and infoEnd. */
    if (mod->len >= 33 && mod->p[mod->len - 17] == '\n')
        *mod = (Str){mod->p + 16, mod->len - 32};
    else
        *mod = BURROW_STR_EMPTY;
    ok = true;
done:
    bi_close(&bf);
    BURROW_OUT(err, e);
    return ok;
}

Buildinfo *buildinfo_read(Alloc *a, IoReaderAt r, Error *err) {
    Arena tmp;
    arena_init(&tmp, a, 0);
    Str vers = BURROW_STR_EMPTY, mod = BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    Buildinfo *bi = NULL;
    if (bi_read_raw(arena_allocator(&tmp), r, &vers, &mod, &e)) {
        bi = debug_parse_build_info(a, mod, &e);
        if (bi != NULL) {
            Byte *p = (Byte *)mem_alloc_nozero(bi->a, (size_t)vers.len, 1);
            if (p == NULL) {
                debug_build_info_free(bi);
                bi = NULL;
                e = burrow_err_out_of_memory;
            } else {
                memcpy(p, vers.p, (size_t)vers.len);
                bi->go_version = (Str){p, vers.len};
            }
        }
    }
    arena_free(&tmp);
    BURROW_OUT(err, e);
    return bi;
}

Buildinfo *buildinfo_read_file(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    Buildinfo *bi = NULL;
    OsFile *f = os_open(a, name, &e);
    if (f != NULL) {
        bi = buildinfo_read(a, os_file_as_io_reader_at(f), &e);
        (void)os_file_close(f);
        os_file_free(f);
    }
    if (BURROW_FAILED(e)) {
        if (errors_as(e, TYPE_FS_PATH_ERROR) != NULL)
            e = fmt_errorf_v("could not read Go build info: %w", e);
        else
            e = fmt_errorf_v("could not read Go build info from %s: %w", name, e);
    }
    BURROW_OUT(err, e);
    return bi;
}
