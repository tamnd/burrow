/* Derived from Go's src/internal/xcoff/file.go, xcoff.go and ar.go. Go source:
 * go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "xcoff_internal.h"

#include "objfile_internal.h"

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
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static void xcoff_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static void xcoff_bounds_panic(void) {
    runtime_panic(BURROW_S("runtime error: slice bounds out of range"));
}

static void xcoff_makeslice_panic(void) {
    runtime_panic(BURROW_S("runtime error: makeslice: len out of range"));
}

static Slice xcoff_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static uint16_t xcoff_u16(const Byte *p) {
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static uint32_t xcoff_u32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t xcoff_u64(const Byte *p) {
    return (uint64_t)xcoff_u32(p) << 32 | xcoff_u32(p + 4);
}

/* binary.Read of n bytes: io.ReadFull, so EOF when nothing was there and
 * ErrUnexpectedEOF when only some of it was. */
static Error xcoff_read(IoSectionReader *sr, Byte *b, Int n) {
    Error e = BURROW_NO_ERROR;
    io_read_full(io_section_reader_as_io_reader(sr), xcoff_bytes(b, n), &e);
    return e;
}

static Error xcoff_seek(IoSectionReader *sr, int64_t off, Int whence) {
    Error e = BURROW_NO_ERROR;
    (void)io_section_reader_seek(sr, off, whence, &e);
    return e;
}

/* cstring: the bytes of p up to the first NUL or the end. */
static Int xcoff_cstring_len(const Byte *p, Int n) {
    const Byte *end = n > 0 ? (const Byte *)memchr(p, 0, (size_t)n) : NULL;
    return end == NULL ? n : (Int)(end - p);
}

/* getString: the name at offset in the string table st, which stays the
 * File's, so the name is not copied. */
static bool xcoff_get_string(Slice st, uint32_t offset, Str *s) {
    if (offset < 4 || (Int)offset >= st.len)
        return false;
    const Byte *p = (const Byte *)st.p + offset;
    *s = str_from_bytes(p, xcoff_cstring_len(p, st.len - (Int)offset));
    return true;
}

/* A growing array of pointers, for the parts of a file whose count the file
 * gives but cannot be trusted with. */
typedef struct XcoffPtrs {
    void **p;
    Int len;
    Int cap;
} XcoffPtrs;

static bool xcoff_ptrs_push(Alloc *a, XcoffPtrs *v, void *x) {
    if (v->len == v->cap) {
        Int ncap = v->cap == 0 ? 8 : v->cap * 2;
        void **np = (void **)mem_alloc(a, (size_t)ncap * sizeof *np, _Alignof(void *));
        if (np == NULL)
            return false;
        if (v->len > 0)
            memcpy((void *)np, (const void *)v->p, (size_t)v->len * sizeof *np);
        mem_free(a, (void *)v->p, (size_t)v->cap * sizeof *np, _Alignof(void *));
        v->p = np;
        v->cap = ncap;
    }
    v->p[v->len++] = x;
    return true;
}

#define XCOFF_ERR(name, text)                                                          \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

XCOFF_ERR(xcoff_err_no_symtab, "no symbol table");
XCOFF_ERR(xcoff_err_nobits, "unexpected read from section with uninitialized data");
XCOFF_ERR(xcoff_err_small_archive, "small AIX archive not supported");
XCOFF_ERR(xcoff_err_no_fmag, "AIAFMAG not found after member header");

#define XCOFF_RELOC_FIELDS(F, T)                                                       \
    F(T, uint64_t, virtual_address, VirtualAddress, "")                                \
    F(T, UnsafePointer, symbol, Symbol, "")                                            \
    F(T, bool, signed_, Signed, "")                                                    \
    F(T, bool, instruction_fixed, InstructionFixed, "")                                \
    F(T, uint8_t, length, Length, "")                                                  \
    F(T, uint8_t, type, Type, "")

#define XCOFF_IMPORTED_SYMBOL_FIELDS(F, T)                                             \
    F(T, Str, name, Name, "")                                                          \
    F(T, Str, library, Library, "")

BURROW_STRUCT_AS_DEFINE(XcoffReloc, XCOFF_RELOC_FIELDS);
BURROW_STRUCT_AS_DEFINE(XcoffImportedSymbol, XCOFF_IMPORTED_SYMBOL_FIELDS);

static const Type xcoff_nobits_desc = {
    {(const Byte *)"nobitsSectionReader", 19},
    {(const Byte *)"internal/xcoff", 14},
    KIND_STRUCT,
    1,
    1,
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

/* nobitsSectionReader, what a section with no bytes in the file reads from. */
static Int xcoff_nobits_read_at(void *self, Slice p, int64_t off, Error *err) {
    (void)self;
    (void)p;
    (void)off;
    BURROW_OUT(err, xcoff_err_nobits);
    return 0;
}

static const IoReaderAtVT xcoff_nobits_vt = {&xcoff_nobits_desc, xcoff_nobits_read_at};

/* ------------------------------------------------------------------ files */

/* idxToSym: the symbols kept so far by their index in the table. The indexes
 * only grow, so a binary search finds one. */
typedef struct XcoffIndex {
    int64_t *keys;
    XcoffSymbol **syms;
    Int len;
    Int cap;
} XcoffIndex;

static bool xcoff_index_put(Alloc *a, XcoffIndex *m, int64_t i, XcoffSymbol *sym) {
    if (m->len == m->cap) {
        Int ncap = m->cap == 0 ? 16 : m->cap * 2;
        int64_t *nk =
            (int64_t *)mem_alloc(a, (size_t)ncap * sizeof *nk, _Alignof(int64_t));
        XcoffSymbol **ns = (XcoffSymbol **)mem_alloc(a, (size_t)ncap * sizeof *ns,
                                                     _Alignof(XcoffSymbol *));
        if (nk == NULL || ns == NULL)
            return false;
        if (m->len > 0) {
            memcpy(nk, m->keys, (size_t)m->len * sizeof *nk);
            memcpy((void *)ns, (const void *)m->syms, (size_t)m->len * sizeof *ns);
        }
        m->keys = nk;
        m->syms = ns;
        m->cap = ncap;
    }
    m->keys[m->len] = i;
    m->syms[m->len] = sym;
    m->len++;
    return true;
}

static XcoffSymbol *xcoff_index_get(const XcoffIndex *m, int64_t i) {
    Int lo = 0, hi = m->len;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (m->keys[mid] < i)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < m->len && m->keys[lo] == i ? m->syms[lo] : NULL;
}

static Error xcoff_read_sections(XcoffFile *f, IoReaderAt r, IoSectionReader *sr,
                                 Alloc *fa, Int nscns) {
    bool is32 = f->file_header.target_machine == XCOFF_U802TOCMAGIC;
    XcoffSection **secs = NULL;
    if (nscns > 0) {
        secs = (XcoffSection **)mem_alloc(fa, (size_t)nscns * sizeof *secs,
                                          _Alignof(XcoffSection *));
        if (secs == NULL)
            return burrow_err_out_of_memory;
    }
    f->sections = (Slice){(void *)secs, 0, nscns, TYPE_UNSAFE_POINTER};
    for (Int i = 0; i < nscns; i++) {
        Byte b[72];
        Error e = xcoff_read(sr, b, is32 ? 40 : 72);
        if (BURROW_FAILED(e))
            return e;
        XcoffSection *s =
            (XcoffSection *)mem_alloc(fa, sizeof *s, _Alignof(XcoffSection));
        if (s == NULL)
            return burrow_err_out_of_memory;
        XcoffSectionHeader *h = &s->section_header;
        h->name = burrow__objfile_cstring(fa, b, 8);
        uint64_t scnptr = 0;
        if (is32) {
            h->virtual_address = xcoff_u32(b + 12);
            h->size = xcoff_u32(b + 16);
            scnptr = xcoff_u32(b + 20);
            h->relptr = xcoff_u32(b + 24);
            h->nreloc = xcoff_u16(b + 32);
            h->type = xcoff_u32(b + 36);
        } else {
            h->virtual_address = xcoff_u64(b + 16);
            h->size = xcoff_u64(b + 24);
            scnptr = xcoff_u64(b + 32);
            h->relptr = xcoff_u64(b + 40);
            h->nreloc = xcoff_u32(b + 56);
            h->type = xcoff_u32(b + 64);
        }
        IoReaderAt r2 = r;
        if (scnptr == 0) { /* .bss must have all 0s */
            r2.vt = &xcoff_nobits_vt;
            r2.data = NULL;
        }
        s->relocs = (Slice){NULL, 0, 0, TYPE_XCOFF_RELOC};
        s->sr = io_new_section_reader(r2, (int64_t)scnptr, (int64_t)h->size);
        s->reader_at = io_section_reader_as_io_reader_at(&s->sr);
        secs[i] = s;
        f->sections.len = i + 1;
    }
    return BURROW_NO_ERROR;
}

static Error xcoff_read_symbols(XcoffFile *f, IoSectionReader *sr, Alloc *fa, Alloc *ta,
                                uint64_t symptr, uint32_t nsyms, XcoffIndex *idx) {
    bool is32 = f->file_header.target_machine == XCOFF_U802TOCMAGIC;
    Int nscns = f->sections.len;
    XcoffSection **secs = (XcoffSection **)f->sections.p;
    Error e = xcoff_seek(sr, (int64_t)symptr, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e))
        return e;
    XcoffPtrs syms = {NULL, 0, 0};
    for (int64_t i = 0; i < (int64_t)nsyms; i++) {
        Int numaux = 0;
        bool need_aux_fcn = false;
        XcoffSymbol sym;
        memset(&sym, 0, sizeof sym);
        Byte b[18];
        e = xcoff_read(sr, b, 18);
        if (BURROW_FAILED(e))
            return e;
        numaux = b[17];
        sym.section_number = xcoff_u16(b + 12);
        sym.storage_class = b[16];
        need_aux_fcn = (xcoff_u16(b + 14) & XCOFF_SYM_TYPE_FUNC) != 0 && numaux > 1;
        if (is32) {
            sym.value = xcoff_u32(b + 8);
            if (xcoff_u32(b) != 0) {
                sym.name = burrow__objfile_cstring(fa, b, 8);
            } else if (!xcoff_get_string(f->string_table, xcoff_u32(b + 4),
                                         &sym.name)) {
                goto skip;
            }
        } else {
            sym.value = xcoff_u64(b);
            if (!xcoff_get_string(f->string_table, xcoff_u32(b + 8), &sym.name))
                goto skip;
        }
        if (sym.storage_class != XCOFF_C_EXT && sym.storage_class != XCOFF_C_WEAKEXT &&
            sym.storage_class != XCOFF_C_HIDEXT)
            goto skip;
        /* Must have at least one csect auxiliary entry. */
        if (numaux < 1 || i + numaux >= (int64_t)nsyms)
            goto skip;

        if (sym.section_number > nscns)
            goto skip;
        if (sym.section_number == 0)
            sym.value = 0;
        else
            sym.value -= secs[sym.section_number - 1]->section_header.virtual_address;

        XcoffSymbol *kept =
            (XcoffSymbol *)mem_alloc(fa, sizeof *kept, _Alignof(XcoffSymbol));
        if (kept == NULL || !xcoff_index_put(ta, idx, i, kept))
            return burrow_err_out_of_memory;

        /* If this symbol is a function, it must retrieve its size from its
         * AUX_FCN entry. It can happen that a function symbol doesn't have
         * any AUX_FCN. In this case, need_aux_fcn is false and their size
         * will be set to 0. */
        if (need_aux_fcn) {
            e = xcoff_read(sr, b, 18);
            if (BURROW_FAILED(e))
                return e;
            sym.aux_fcn.size = xcoff_u32(is32 ? b + 4 : b + 8);
        }

        /* Read csect auxiliary entry (by convention, it is the last). */
        if (!need_aux_fcn) {
            e = xcoff_seek(sr, (int64_t)(numaux - 1) * XCOFF_SYMESZ,
                           BURROW_IO_SEEK_CURRENT);
            if (BURROW_FAILED(e))
                return e;
        }
        i += numaux;
        numaux = 0;
        e = xcoff_read(sr, b, 18);
        if (BURROW_FAILED(e))
            return e;
        sym.aux_csect.symbol_type = b[10] & 0x7;
        sym.aux_csect.storage_mapping_class = b[11];
        if (is32)
            sym.aux_csect.length = xcoff_u32(b);
        else
            sym.aux_csect.length =
                (int64_t)((uint64_t)xcoff_u32(b + 12) << 32 | xcoff_u32(b));
        *kept = sym;
        if (!xcoff_ptrs_push(ta, &syms, kept))
            return burrow_err_out_of_memory;
    skip:
        i += numaux; /* Skip auxiliary entries */
        e = xcoff_seek(sr, (int64_t)numaux * XCOFF_SYMESZ, BURROW_IO_SEEK_CURRENT);
        if (BURROW_FAILED(e))
            return e;
    }
    XcoffSymbol **out = NULL;
    if (syms.len > 0) {
        out = (XcoffSymbol **)mem_alloc(fa, (size_t)syms.len * sizeof *out,
                                        _Alignof(XcoffSymbol *));
        if (out == NULL)
            return burrow_err_out_of_memory;
        memcpy((void *)out, (const void *)syms.p, (size_t)syms.len * sizeof *out);
    }
    f->symbols = (Slice){(void *)out, syms.len, syms.len, TYPE_UNSAFE_POINTER};
    return BURROW_NO_ERROR;
}

static Error xcoff_read_relocs(XcoffFile *f, IoSectionReader *sr, Alloc *fa,
                               const XcoffIndex *idx) {
    bool is32 = f->file_header.target_machine == XCOFF_U802TOCMAGIC;
    XcoffSection **secs = (XcoffSection **)f->sections.p;
    for (Int k = 0; k < f->sections.len; k++) {
        XcoffSection *sect = secs[k];
        const XcoffSectionHeader *h = &sect->section_header;
        if (h->type != XCOFF_STYP_TEXT && h->type != XCOFF_STYP_DATA)
            continue;
        if (h->relptr == 0)
            continue;
        Error e = xcoff_seek(sr, (int64_t)h->relptr, BURROW_IO_SEEK_START);
        if (BURROW_FAILED(e))
            return e;
        XcoffReloc *rs = NULL;
        Int n = 0, cap = 0;
        for (uint32_t i = 0; i < h->nreloc; i++) {
            Byte b[14];
            e = xcoff_read(sr, b, is32 ? 10 : 14);
            if (BURROW_FAILED(e))
                return e;
            if (n == cap) {
                Int ncap = cap == 0 ? 16 : cap * 2;
                XcoffReloc *nr = (XcoffReloc *)mem_alloc(fa, (size_t)ncap * sizeof *nr,
                                                         _Alignof(XcoffReloc));
                if (nr == NULL)
                    return burrow_err_out_of_memory;
                if (n > 0)
                    memcpy(nr, rs, (size_t)n * sizeof *nr);
                rs = nr;
                cap = ncap;
            }
            XcoffReloc *reloc = &rs[n++];
            Byte size = 0;
            if (is32) {
                reloc->virtual_address = xcoff_u32(b);
                reloc->symbol = xcoff_index_get(idx, (int64_t)xcoff_u32(b + 4));
                size = b[8];
                reloc->type = b[9];
            } else {
                reloc->virtual_address = xcoff_u64(b);
                reloc->symbol = xcoff_index_get(idx, (int64_t)xcoff_u32(b + 8));
                size = b[12];
                reloc->type = b[13];
            }
            reloc->length = (uint8_t)((size & 0x3F) + 1);
            reloc->signed_ = (size & 0x80) != 0;
            reloc->instruction_fixed = (size & 0x40) != 0;
        }
        sect->relocs = (Slice){rs, n, cap, TYPE_XCOFF_RELOC};
    }
    return BURROW_NO_ERROR;
}

static Error xcoff_read_file(XcoffFile *f, IoReaderAt r, Alloc *fa, Alloc *ta) {
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);
    /* Read XCOFF target machine */
    Byte b[24];
    Error e = xcoff_read(&sr, b, 2);
    if (BURROW_FAILED(e))
        return e;
    uint16_t magic = xcoff_u16(b);
    if (magic != XCOFF_U802TOCMAGIC && magic != XCOFF_U64_TOCMAGIC)
        return fmt_errorf_v("unrecognised XCOFF magic: 0x%x", magic);
    f->file_header.target_machine = magic;
    bool is32 = magic == XCOFF_U802TOCMAGIC;

    /* Read XCOFF file header */
    e = xcoff_seek(&sr, 0, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e))
        return e;
    uint16_t nscns = 0;
    uint64_t symptr = 0;
    uint32_t nsyms = 0;
    uint16_t opthdr = 0;
    Int hdrsz = 0;
    if (is32) {
        e = xcoff_read(&sr, b, XCOFF_FILHSZ_32);
        if (BURROW_FAILED(e))
            return e;
        nscns = xcoff_u16(b + 2);
        symptr = xcoff_u32(b + 8);
        nsyms = xcoff_u32(b + 12);
        opthdr = xcoff_u16(b + 16);
        hdrsz = XCOFF_FILHSZ_32;
    } else {
        e = xcoff_read(&sr, b, XCOFF_FILHSZ_64);
        if (BURROW_FAILED(e))
            return e;
        nscns = xcoff_u16(b + 2);
        symptr = xcoff_u64(b + 8);
        opthdr = xcoff_u16(b + 16);
        nsyms = xcoff_u32(b + 20);
        hdrsz = XCOFF_FILHSZ_64;
    }

    if (symptr == 0 || nsyms == 0)
        return xcoff_err_no_symtab;

    /* Read string table (located right after symbol table). */
    uint64_t offset = symptr + (uint64_t)nsyms * XCOFF_SYMESZ;
    e = xcoff_seek(&sr, (int64_t)offset, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e))
        return e;
    /* The first 4 bytes contain the length (in bytes). */
    e = xcoff_read(&sr, b, 4);
    if (BURROW_FAILED(e))
        return e;
    uint32_t l = xcoff_u32(b);
    if (l > 4) {
        Slice st = burrow__saferio_read_data_at(
            fa, io_section_reader_as_io_reader_at(&sr), l, (int64_t)offset, &e);
        if (BURROW_FAILED(e))
            return e;
        f->string_table = st;
    }

    /* Read section headers */
    e = xcoff_seek(&sr, (int64_t)hdrsz + (int64_t)opthdr, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e))
        return e;
    e = xcoff_read_sections(f, r, &sr, fa, nscns);
    if (BURROW_FAILED(e))
        return e;

    /* Symbol map needed by relocation */
    XcoffIndex idx = {NULL, NULL, 0, 0};
    e = xcoff_read_symbols(f, &sr, fa, ta, symptr, nsyms, &idx);
    if (BURROW_FAILED(e))
        return e;

    /* Read relocations, only for .data or .text section */
    return xcoff_read_relocs(f, &sr, fa, &idx);
}

static void xcoff_file_release(XcoffFile *f) {
    Alloc *a = f->a;
    arena_free(&f->arena);
    mem_free(a, f, sizeof *f, _Alignof(XcoffFile));
}

XcoffFile *burrow__xcoff_new_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        xcoff_nil_panic();
    XcoffFile *f = (XcoffFile *)mem_alloc(a, sizeof *f, _Alignof(XcoffFile));
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->a = a;
    arena_init(&f->arena, a, 0);
    f->sections.elem = TYPE_UNSAFE_POINTER;
    f->symbols.elem = TYPE_UNSAFE_POINTER;
    f->string_table.elem = TYPE_BYTE;
    f->library_paths.elem = TYPE_STRING;
    Arena tmp;
    arena_init(&tmp, a, 0);
    Error e = xcoff_read_file(f, r, arena_allocator(&f->arena), arena_allocator(&tmp));
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        xcoff_file_release(f);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

XcoffFile *burrow__xcoff_open(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    XcoffFile *f = burrow__xcoff_new_file(a, os_file_as_io_reader_at(file), &e);
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

Error burrow__xcoff_file_close(XcoffFile *f) {
    if (f == NULL)
        xcoff_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (f->open) {
        e = os_file_close(f->file);
        f->open = false;
    }
    return e;
}

void burrow__xcoff_file_free(XcoffFile *f) {
    if (f == NULL)
        return;
    OsFile *file = f->file;
    if (f->open)
        os_file_close(file);
    os_file_free(file);
    xcoff_file_release(f);
}

XcoffSection *burrow__xcoff_file_section(XcoffFile *f, Str name) {
    if (f == NULL)
        xcoff_nil_panic();
    XcoffSection **secs = (XcoffSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        Str sn = secs[i]->section_header.name;
        if (str_eq(sn, name) || (name.len > 8 && str_eq(sn, str_from_bytes(name.p, 8))))
            return secs[i];
    }
    return NULL;
}

XcoffSection *burrow__xcoff_file_section_by_type(XcoffFile *f, uint32_t typ) {
    if (f == NULL)
        xcoff_nil_panic();
    XcoffSection **secs = (XcoffSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        if (secs[i]->section_header.type == typ)
            return secs[i];
    }
    return NULL;
}

/* Go makes a buffer of the section's size and reads it in one go. That size
 * comes from the file, so this reads a chunk at a time instead, which ends the
 * same way: the bytes there are, and the error of the read that came up
 * short. */
#define XCOFF_CHUNK ((int64_t)1 << 20)

Slice burrow__xcoff_section_data(XcoffSection *s, Alloc *a, Error *err) {
    if (s == NULL)
        xcoff_nil_panic();
    Slice out = {NULL, 0, 0, TYPE_BYTE};
    int64_t size = io_section_reader_size(&s->sr);
    if (size < 0)
        xcoff_makeslice_panic();
    Byte *buf = NULL;
    int64_t n = 0, cap = 0;
    Error e = BURROW_NO_ERROR;
    while (n < size) {
        int64_t want = size - n < XCOFF_CHUNK ? size - n : XCOFF_CHUNK;
        if (n + want > cap) {
            int64_t ncap = cap == 0 ? want : cap * 2;
            if (ncap < n + want)
                ncap = n + want;
            if (ncap > size)
                ncap = size;
            Byte *nb = (Byte *)mem_alloc_nozero(a, (size_t)ncap, 1);
            if (nb == NULL) {
                mem_free(a, buf, (size_t)cap, 1);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return out;
            }
            if (n > 0)
                memcpy(nb, buf, (size_t)n);
            mem_free(a, buf, (size_t)cap, 1);
            buf = nb;
            cap = ncap;
        }
        Int got =
            io_section_reader_read_at(&s->sr, xcoff_bytes(buf + n, (Int)want), n, &e);
        n += got;
        if (BURROW_FAILED(e) || got < (Int)want)
            break;
    }
    if (n == size)
        e = BURROW_NO_ERROR;
    out = (Slice){buf, (Int)n, (Int)cap, TYPE_BYTE};
    BURROW_OUT(err, e);
    return out;
}

Slice burrow__xcoff_file_csect(XcoffFile *f, Alloc *a, Str name, bool *found) {
    if (f == NULL)
        xcoff_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    *found = false;
    XcoffSymbol **syms = (XcoffSymbol **)f->symbols.p;
    XcoffSection **secs = (XcoffSection **)f->sections.p;
    for (Int k = 0; k < f->symbols.len; k++) {
        const XcoffSymbol *sym = syms[k];
        if (!str_eq(sym->name, name) || sym->aux_csect.symbol_type != XCOFF_XTY_SD)
            continue;
        Int i = sym->section_number - 1;
        if (0 <= i && i < f->sections.len) {
            XcoffSection *s = secs[i];
            if (sym->value + (uint64_t)sym->aux_csect.length <=
                s->section_header.size) {
                int64_t n = sym->aux_csect.length;
                if (n < 0 || (uint64_t)n > (uint64_t)INTPTR_MAX)
                    xcoff_makeslice_panic();
                Byte *dat = (Byte *)mem_alloc(a, n == 0 ? 1 : (size_t)n, 1);
                if (dat == NULL)
                    return none;
                Error e = BURROW_NO_ERROR;
                (void)io_section_reader_read_at(&s->sr, xcoff_bytes(dat, (Int)n),
                                                (int64_t)sym->value, &e);
                if (BURROW_FAILED(e)) {
                    mem_free(a, dat, n == 0 ? 1 : (size_t)n, 1);
                    return none;
                }
                *found = true;
                return xcoff_bytes(dat, (Int)n);
            }
        }
        break;
    }
    return none;
}

DwarfData *burrow__xcoff_file_dwarf(XcoffFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        xcoff_nil_panic();
    /* There are many other DWARF sections, but these are the ones the
     * debug/dwarf package uses. Don't bother loading others. */
    static const uint32_t subtypes[5] = {XCOFF_SSUBTYP_DWABREV, XCOFF_SSUBTYP_DWINFO,
                                         XCOFF_SSUBTYP_DWLINE, XCOFF_SSUBTYP_DWRNGES,
                                         XCOFF_SSUBTYP_DWSTR};
    Slice dat[5];
    for (int i = 0; i < 5; i++) {
        Slice nil = {NULL, 0, 0, TYPE_BYTE};
        dat[i] = nil;
        XcoffSection *s =
            burrow__xcoff_file_section_by_type(f, XCOFF_STYP_DWARF | subtypes[i]);
        if (s == NULL)
            continue;
        Error e = BURROW_NO_ERROR;
        Slice b = burrow__xcoff_section_data(s, a, &e);
        if (BURROW_FAILED(e) && (uint64_t)b.len < s->section_header.size) {
            BURROW_OUT(err, e);
            return NULL;
        }
        dat[i] = b;
    }
    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    return dwarf_new(a, dat[0], nil, nil, dat[1], dat[2], nil, dat[3], dat[4], err);
}

/* --------------------------------------------------------- loader section */

/* readImportIDs: the import file IDs stored inside the .loader section, as
 * Strs in a. Library name pattern is either path/base/member or base/member.
 * It sets the File's library_paths too. */
static Slice xcoff_read_import_ids(XcoffFile *f, XcoffSection *s, Alloc *a,
                                   Error *err) {
    Slice none = {NULL, 0, 0, TYPE_STRING};
    bool is32 = f->file_header.target_machine == XCOFF_U802TOCMAGIC;
    /* Read loader header */
    Error e = xcoff_seek(&s->sr, 0, BURROW_IO_SEEK_START);
    Byte b[XCOFF_LDHDRSZ_64];
    if (BURROW_OK(e))
        e = xcoff_read(&s->sr, b, is32 ? XCOFF_LDHDRSZ_32 : XCOFF_LDHDRSZ_64);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    uint32_t istlen = xcoff_u32(b + 12);
    uint32_t nimpid = xcoff_u32(b + 16);
    uint64_t impoff = is32 ? xcoff_u32(b + 20) : xcoff_u64(b + 24);

    /* Read loader import file ID table */
    e = xcoff_seek(&s->sr, (int64_t)impoff, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    Arena tmp;
    arena_init(&tmp, a, 0);
    Slice table = burrow__saferio_read_data(
        arena_allocator(&tmp), io_section_reader_as_io_reader(&s->sr), istlen, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }
    const Byte *t = (const Byte *)table.p;

    Int offset = 0;
    /* First import file ID is the default LIBPATH value */
    Int n = xcoff_cstring_len(t, table.len);
    Str libpath = str_clone(arena_allocator(&f->arena), str_from_bytes(t, n));
    f->library_paths =
        strings_split(arena_allocator(&f->arena), libpath, BURROW_S(":"));
    offset += n + 3; /* 3 null bytes */
    XcoffPtrs all = {NULL, 0, 0};
    Str *out = NULL;
    Int nout = 0;
    for (int64_t i = 1; i < (int64_t)nimpid; i++) {
        Str parts[3];
        for (int k = 0; k < 3; k++) {
            if (offset > table.len)
                xcoff_bounds_panic();
            Int m = xcoff_cstring_len(t + offset, table.len - offset);
            parts[k] = str_from_bytes(t + offset, m);
            offset += m + 1;
        }
        Str path = parts[0].len > 0
                       ? fmt_sprintf_v(a, "%s/%s/%s", parts[0], parts[1], parts[2])
                       : fmt_sprintf_v(a, "%s/%s", parts[1], parts[2]);
        Str *p = (Str *)mem_alloc(arena_allocator(&tmp), sizeof *p, _Alignof(Str));
        if (p == NULL || !xcoff_ptrs_push(arena_allocator(&tmp), &all, p)) {
            arena_free(&tmp);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        *p = path;
    }
    out = (Str *)mem_alloc(
        a, all.len == 0 ? sizeof *out : (size_t)all.len * sizeof *out, _Alignof(Str));
    if (out == NULL) {
        arena_free(&tmp);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    for (Int i = 0; i < all.len; i++)
        out[nout++] = *(Str *)all.p[i];
    arena_free(&tmp);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){out, nout, nout, TYPE_STRING};
}

Slice burrow__xcoff_file_imported_symbols(XcoffFile *f, Alloc *a, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_XCOFF_IMPORTED_SYMBOL};
    XcoffSection *s = burrow__xcoff_file_section_by_type(f, XCOFF_STYP_LOADER);
    if (s == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return none;
    }
    bool is32 = f->file_header.target_machine == XCOFF_U802TOCMAGIC;
    /* Read loader header */
    Error e = xcoff_seek(&s->sr, 0, BURROW_IO_SEEK_START);
    Byte b[XCOFF_LDHDRSZ_64];
    if (BURROW_OK(e))
        e = xcoff_read(&s->sr, b, is32 ? XCOFF_LDHDRSZ_32 : XCOFF_LDHDRSZ_64);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    uint32_t nsyms = xcoff_u32(b + 4);
    uint32_t stlen = is32 ? xcoff_u32(b + 24) : xcoff_u32(b + 20);
    uint64_t stoff = is32 ? xcoff_u32(b + 28) : xcoff_u64(b + 32);
    uint64_t symoff = is32 ? XCOFF_LDHDRSZ_32 : xcoff_u64(b + 40);

    /* Read loader section string table */
    e = xcoff_seek(&s->sr, (int64_t)stoff, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    Arena tmp;
    arena_init(&tmp, a, 0);
    Alloc *ta = arena_allocator(&tmp);
    Slice st = burrow__saferio_read_data(ta, io_section_reader_as_io_reader(&s->sr),
                                         stlen, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }

    /* Read imported libraries */
    Slice libs = xcoff_read_import_ids(f, s, a, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }

    /* Read loader symbol table */
    e = xcoff_seek(&s->sr, (int64_t)symoff, BURROW_IO_SEEK_START);
    if (BURROW_FAILED(e)) {
        arena_free(&tmp);
        BURROW_OUT(err, e);
        return none;
    }
    XcoffImportedSymbol *all = NULL;
    Int n = 0, cap = 0;
    for (int64_t i = 0; i < (int64_t)nsyms; i++) {
        Byte ls[24];
        e = xcoff_read(&s->sr, ls, 24);
        if (BURROW_FAILED(e)) {
            arena_free(&tmp);
            BURROW_OUT(err, e);
            return none;
        }
        if ((ls[14] & 0x40) == 0)
            continue; /* Imported symbols only */
        Str name = BURROW_STR_EMPTY;
        if (is32 && xcoff_u32(ls) != 0) {
            name = str_from_bytes(ls, xcoff_cstring_len(ls, 8));
        } else if (!xcoff_get_string(st, xcoff_u32(is32 ? ls + 4 : ls + 8), &name)) {
            continue;
        }
        uint32_t ifile = xcoff_u32(ls + 16);
        if (n == cap) {
            Int ncap = cap == 0 ? 16 : cap * 2;
            XcoffImportedSymbol *na = (XcoffImportedSymbol *)mem_alloc(
                ta, (size_t)ncap * sizeof *na, _Alignof(XcoffImportedSymbol));
            if (na == NULL) {
                arena_free(&tmp);
                BURROW_OUT(err, burrow_err_out_of_memory);
                return none;
            }
            if (n > 0)
                memcpy(na, all, (size_t)n * sizeof *na);
            all = na;
            cap = ncap;
        }
        XcoffImportedSymbol *sym = &all[n++];
        sym->name = str_clone(a, name);
        sym->library = BURROW_STR_EMPTY;
        if (ifile >= 1 && (int64_t)ifile <= (int64_t)libs.len)
            sym->library = ((const Str *)libs.p)[ifile - 1];
    }
    XcoffImportedSymbol *out = (XcoffImportedSymbol *)mem_alloc(
        a, n == 0 ? sizeof *out : (size_t)n * sizeof *out,
        _Alignof(XcoffImportedSymbol));
    if (out == NULL) {
        arena_free(&tmp);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    if (n > 0)
        memcpy(out, all, (size_t)n * sizeof *out);
    arena_free(&tmp);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){out, n, n, TYPE_XCOFF_IMPORTED_SYMBOL};
}

Slice burrow__xcoff_file_imported_libraries(XcoffFile *f, Alloc *a, Error *err) {
    XcoffSection *s = burrow__xcoff_file_section_by_type(f, XCOFF_STYP_LOADER);
    if (s == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (Slice){NULL, 0, 0, TYPE_STRING};
    }
    return xcoff_read_import_ids(f, s, a, err);
}

/* --------------------------------------------------------------- archives */

/* parseDecimalBytes. */
static int64_t xcoff_parse_decimal(const Byte *b, Int n, Error *err) {
    return strconv_parse_int(strings_trim_space(str_from_bytes(b, n)), 10, 64, err);
}

static Str xcoff_field(const Byte *b, Int off, Int n) {
    return str_from_bytes(b + off, n);
}

/* The %q of bigarFileHeader that Go's errors quote: each field quoted, in
 * braces. */
static Error xcoff_fhdr_error(const Byte *h, Error pe) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str q1 =
        fmt_sprintf_v(a, "{%q %q %q %q", xcoff_field(h, 0, 8), xcoff_field(h, 8, 20),
                      xcoff_field(h, 28, 20), xcoff_field(h, 48, 20));
    Str q2 = fmt_sprintf_v(a, "%q %q %q}", xcoff_field(h, 68, 20),
                           xcoff_field(h, 88, 20), xcoff_field(h, 108, 20));
    Error e = fmt_errorf_v(
        "error parsing offset of first member in archive header(%s %s); %v", q1, q2,
        pe);
    arena_free(&ar);
    return e;
}

/* The same for bigarMemberHeader. */
static Error xcoff_mhdr_error(const char *what, const Byte *h, Error pe) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str q1 =
        fmt_sprintf_v(a, "{%q %q %q %q", xcoff_field(h, 0, 20), xcoff_field(h, 20, 20),
                      xcoff_field(h, 40, 20), xcoff_field(h, 60, 12));
    Str q2 =
        fmt_sprintf_v(a, "%q %q %q %q}", xcoff_field(h, 72, 12), xcoff_field(h, 84, 12),
                      xcoff_field(h, 96, 12), xcoff_field(h, 108, 4));
    Str w = str_from_cstr(what);
    Error e =
        fmt_errorf_v("error parsing %s in member header(%s %s); %v", w, q1, q2, pe);
    arena_free(&ar);
    return e;
}

static Error xcoff_read_archive(XcoffArchive *arch, Alloc *fa) {
    IoSectionReader *sr = &arch->sr;
    /* Read File Header */
    Byte magic[XCOFF_SAIAMAG];
    Error e = BURROW_NO_ERROR;
    (void)io_section_reader_read_at(sr, xcoff_bytes(magic, XCOFF_SAIAMAG), 0, &e);
    if (BURROW_FAILED(e))
        return e;
    if (memcmp(magic, XCOFF_AIAMAGBIG, XCOFF_SAIAMAG) == 0) {
        arch->archive_header.magic = BURROW_S(XCOFF_AIAMAGBIG);
    } else if (memcmp(magic, XCOFF_AIAMAG, XCOFF_SAIAMAG) == 0) {
        return xcoff_err_small_archive;
    } else {
        Str m = str_from_bytes(magic, XCOFF_SAIAMAG);
        return fmt_errorf_v("unrecognised archive magic: 0x%x", m);
    }

    Byte fhdr[XCOFF_FL_HSZ_BIG];
    e = xcoff_seek(sr, 0, BURROW_IO_SEEK_START);
    if (BURROW_OK(e))
        e = xcoff_read(sr, fhdr, XCOFF_FL_HSZ_BIG);
    if (BURROW_FAILED(e))
        return e;

    Error pe = BURROW_NO_ERROR;
    int64_t off = xcoff_parse_decimal(fhdr + 68, 20, &pe);
    if (BURROW_FAILED(pe))
        return xcoff_fhdr_error(fhdr, pe);
    if (off == 0) {
        /* Occurs if the archive is empty. */
        return BURROW_NO_ERROR;
    }

    int64_t lastoff = xcoff_parse_decimal(fhdr + 88, 20, &pe);
    if (BURROW_FAILED(pe))
        return xcoff_fhdr_error(fhdr, pe);

    /* Read members */
    XcoffPtrs members = {NULL, 0, 0};
    for (;;) {
        /* Read Member Header. The member header is normally 2 bytes larger.
         * But it's easier to read the name if the header is read without
         * _ar_nam. However, AIAFMAG must be read afterward. */
        e = xcoff_seek(sr, off, BURROW_IO_SEEK_START);
        if (BURROW_FAILED(e))
            return e;
        Byte mhdr[XCOFF_AR_HSZ_BIG];
        e = xcoff_read(sr, mhdr, XCOFF_AR_HSZ_BIG);
        if (BURROW_FAILED(e))
            return e;

        XcoffMember *member =
            (XcoffMember *)mem_alloc(fa, sizeof *member, _Alignof(XcoffMember));
        if (member == NULL || !xcoff_ptrs_push(fa, &members, member))
            return burrow_err_out_of_memory;
        arch->members =
            (Slice){(void *)members.p, members.len, members.cap, TYPE_UNSAFE_POINTER};

        int64_t size = xcoff_parse_decimal(mhdr, 20, &pe);
        if (BURROW_FAILED(pe))
            return xcoff_mhdr_error("size", mhdr, pe);
        member->member_header.size = (uint64_t)size;

        /* Read name */
        int64_t namlen = xcoff_parse_decimal(mhdr + 108, 4, &pe);
        if (BURROW_FAILED(pe))
            return xcoff_mhdr_error("name length", mhdr, pe);
        if (namlen < 0)
            xcoff_makeslice_panic();
        Byte *name = (Byte *)mem_alloc(fa, namlen == 0 ? 1 : (size_t)namlen, 1);
        if (name == NULL)
            return burrow_err_out_of_memory;
        e = xcoff_read(sr, name, (Int)namlen);
        if (BURROW_FAILED(e))
            return e;
        member->member_header.name = str_from_bytes(name, (Int)namlen);

        int64_t fileoff =
            (int64_t)((uint64_t)off + XCOFF_AR_HSZ_BIG + (uint64_t)namlen);
        if ((fileoff & 1) != 0) {
            fileoff++;
            e = xcoff_seek(sr, 1, BURROW_IO_SEEK_CURRENT);
            if (BURROW_FAILED(e))
                return e;
        }

        /* Read AIAFMAG string */
        Byte fmag[2];
        e = xcoff_read(sr, fmag, 2);
        if (BURROW_FAILED(e))
            return e;
        if (memcmp(fmag, XCOFF_AIAFMAG, 2) != 0)
            return xcoff_err_no_fmag;
        fileoff += 2; /* Add the two bytes of AIAFMAG */

        member->sr =
            io_new_section_reader(io_section_reader_as_io_reader_at(sr), fileoff, size);

        if (off == lastoff)
            break;
        off = xcoff_parse_decimal(mhdr + 20, 20, &pe);
        if (BURROW_FAILED(pe))
            return xcoff_fhdr_error(fhdr, pe);
    }
    return BURROW_NO_ERROR;
}

static void xcoff_archive_release(XcoffArchive *arch) {
    Alloc *a = arch->a;
    arena_free(&arch->arena);
    mem_free(a, arch, sizeof *arch, _Alignof(XcoffArchive));
}

XcoffArchive *burrow__xcoff_new_archive(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        xcoff_nil_panic();
    XcoffArchive *arch =
        (XcoffArchive *)mem_alloc(a, sizeof *arch, _Alignof(XcoffArchive));
    if (arch == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    arch->a = a;
    arena_init(&arch->arena, a, 0);
    arch->members.elem = TYPE_UNSAFE_POINTER;
    arch->sr = io_new_section_reader(r, 0, INT64_MAX);
    Error e = xcoff_read_archive(arch, arena_allocator(&arch->arena));
    if (BURROW_FAILED(e)) {
        xcoff_archive_release(arch);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return arch;
}

XcoffArchive *burrow__xcoff_open_archive(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    XcoffArchive *arch =
        burrow__xcoff_new_archive(a, os_file_as_io_reader_at(file), &e);
    if (arch == NULL) {
        os_file_close(file);
        os_file_free(file);
        BURROW_OUT(err, e);
        return NULL;
    }
    arch->file = file;
    arch->open = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return arch;
}

Error burrow__xcoff_archive_close(XcoffArchive *arch) {
    if (arch == NULL)
        xcoff_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (arch->open) {
        e = os_file_close(arch->file);
        arch->open = false;
    }
    return e;
}

void burrow__xcoff_archive_free(XcoffArchive *arch) {
    if (arch == NULL)
        return;
    OsFile *file = arch->file;
    if (arch->open)
        os_file_close(file);
    os_file_free(file);
    xcoff_archive_release(arch);
}

XcoffFile *burrow__xcoff_archive_get_file(XcoffArchive *arch, Alloc *a, Str name,
                                          Error *err) {
    if (arch == NULL)
        xcoff_nil_panic();
    XcoffMember **ms = (XcoffMember **)arch->members.p;
    for (Int i = 0; i < arch->members.len; i++) {
        if (str_eq(ms[i]->member_header.name, name))
            return burrow__xcoff_new_file(
                a, io_section_reader_as_io_reader_at(&ms[i]->sr), err);
    }
    BURROW_OUT(err, fmt_errorf_v("unknown member %s in archive", name));
    return NULL;
}
