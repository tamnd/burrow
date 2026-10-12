/* Derived from Go's src/debug/pe/file.go, pe.go, section.go, string.go and
 * symbol.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/pe.h"

#include "objfile_internal.h"

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
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

static void pe_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static Slice pe_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static uint16_t pe_u16(const Byte *p) {
    return (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static uint32_t pe_u32(const Byte *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* The n little-endian bytes at p, for n up to 8. */
static uint64_t pe_uint(const Byte *p, Int n) {
    uint64_t v = 0;
    for (Int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static Int pe_put(Byte *p, Int at, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p + at, s.p, (size_t)s.len);
    return at + s.len;
}

static Str pe_concat(Alloc *a, Str prefix, Str s, Str suffix) {
    Int n = prefix.len + s.len + suffix.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    pe_put(p, pe_put(p, pe_put(p, 0, prefix), s), suffix);
    return str_from_bytes(p, n);
}

static Str pe_idec(Byte buf[24], Int v) {
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    Int i = 24;
    do {
        buf[--i] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (v < 0)
        buf[--i] = '-';
    return str_from_bytes(buf + i, 24 - i);
}

/* cstring, as a Str into p. */
static Str pe_cstring(const Byte *p, Int n) {
    const Byte *end = (const Byte *)memchr(p, 0, (size_t)n);
    return str_from_bytes(p, end == NULL ? n : (Int)(end - p));
}

/* ------------------------------------------------------------------ errors */

#define PE_ERR(name, text)                                                             \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

PE_ERR(pe_err_too_many_symbols, "too many symbols; file may be corrupt");
PE_ERR(pe_err_magic_size,
       "optional header size is less than optional header magic size");
PE_ERR(pe_err_nobits, "unexpected read from section with uninitialized data");
PE_ERR(pe_err_symbol_index, "invalid symbol index");
PE_ERR(pe_err_storage_class, "incorrect symbol storage class");
PE_ERR(pe_err_aux_unavailable, "aux symbol unavailable");
PE_ERR(pe_err_dd_seek,
       "optional header data directory virtual size doesn't fit within data seek");
PE_ERR(pe_err_thunk_seek,
       "import directory original first thunk doesn't fit within data seek");
PE_ERR(pe_err_thunk8, "thunk parsing needs at least 8-bytes");
PE_ERR(pe_err_thunk5, "thunk parsing needs at least 5-bytes");

Str pe_format_error_error(const PeFormatError *e) {
    if (e == NULL)
        pe_nil_panic();
    return BURROW_S("unknown error");
}

static Str pe_format_error_m_error(PeFormatError *self) {
    return pe_format_error_error(self);
}

#define PE_FORMAT_ERROR_SIG_ERROR(IN, OUT) OUT(Str)
#define PE_FORMAT_ERROR_METHODS(M, T)                                                  \
    M(T, Error, pe_format_error_m_error, PE_FORMAT_ERROR_SIG_ERROR)

BURROW_METHODS_DEFINE(PeFormatError, PE_FORMAT_ERROR_METHODS);

const Type burrow_type_PeFormatError = {
    {(const Byte *)"FormatError", 11},
    {(const Byte *)"debug/pe", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(PeFormatError),
    (uint16_t)_Alignof(PeFormatError),
    0,
    (uint16_t)(sizeof(burrow__methods_PeFormatError) /
               sizeof(burrow__methods_PeFormatError[0])),
    NULL,
    burrow__methods_PeFormatError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_PeOptionalHeader32 = {
    {(const Byte *)"OptionalHeader32", 16},
    {(const Byte *)"debug/pe", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(PeOptionalHeader32),
    (uint16_t)_Alignof(PeOptionalHeader32),
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

const Type burrow_type_PeOptionalHeader64 = {
    {(const Byte *)"OptionalHeader64", 16},
    {(const Byte *)"debug/pe", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(PeOptionalHeader64),
    (uint16_t)_Alignof(PeOptionalHeader64),
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

/* ----------------------------------------------------------- string table */

Str pe_string_table_string(PeStringTable st, uint32_t start, Error *err) {
    /* start includes 4 bytes of string table length */
    if (start < 4) {
        BURROW_OUT(
            err, fmt_errorf_v("offset %d is before the start of string table", start));
        return BURROW_STR_EMPTY;
    }
    start -= 4;
    if ((int64_t)start > (int64_t)st.len) {
        BURROW_OUT(err,
                   fmt_errorf_v("offset %d is beyond the end of string table", start));
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if ((Int)start == st.len)
        return BURROW_STR_EMPTY;
    return pe_cstring((const Byte *)st.p + start, st.len - (Int)start);
}

/* readStringTable. The COFF string table is right after the COFF symbol
 * table. */
static Error pe_read_string_table(PeStringTable *st, Alloc *fa, const PeFileHeader *fh,
                                  IoSectionReader *sr) {
    if (fh->pointer_to_symbol_table == 0)
        return BURROW_NO_ERROR;
    uint32_t offset = fh->pointer_to_symbol_table +
                      (uint32_t)PE_COFF_SYMBOL_SIZE * fh->number_of_symbols;
    Error e = BURROW_NO_ERROR;
    io_section_reader_seek(sr, (int64_t)offset, BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("fail to seek to string table: %v", e);
    Byte lb[4];
    io_read_full(io_section_reader_as_io_reader(sr), pe_bytes(lb, 4), &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("fail to read string table length: %v", e);
    /* string table length includes itself */
    uint32_t l = pe_u32(lb);
    if (l <= 4)
        return BURROW_NO_ERROR;
    l -= 4;
    Slice buf =
        burrow__saferio_read_data(fa, io_section_reader_as_io_reader(sr), l, &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("fail to read string table: %v", e);
    *st = buf;
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- symbols */

/* isSymNameOffset: whether the name is an offset into the string table, and
 * which. */
static uint32_t pe_sym_name_offset(const Byte name[8]) {
    if (name[0] == 0 && name[1] == 0 && name[2] == 0 && name[3] == 0)
        return pe_u32(name + 4);
    return 0;
}

Str pe_coff_symbol_full_name(const PeCOFFSymbol *sym, PeStringTable st, Error *err) {
    if (sym == NULL)
        pe_nil_panic();
    uint32_t offset = pe_sym_name_offset(sym->name);
    if (offset != 0)
        return pe_string_table_string(st, offset, err);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return pe_cstring(sym->name, 8);
}

/* A primary symbol record. */
static PeCOFFSymbol pe_coff_symbol(const Byte *b) {
    PeCOFFSymbol s;
    memcpy(s.name, b, 8);
    s.value = pe_u32(b + 8);
    s.section_number = (int16_t)pe_u16(b + 12);
    s.type = pe_u16(b + 14);
    s.storage_class = b[16];
    s.number_of_aux_symbols = b[17];
    return s;
}

/* An auxiliary record, which Go reads as a COFFSymbolAuxFormat5 into the
 * memory of a zero COFFSymbol. The format 5 fields cover the first 15 bytes
 * of the record and land on the same bytes of the COFFSymbol, on a
 * little-endian machine, and the padding after them is skipped, so the last
 * three bytes stay zero. */
static PeCOFFSymbol pe_coff_aux_symbol(const Byte *b) {
    PeCOFFSymbol s;
    memcpy(s.name, b, 8);
    s.value = pe_u32(b + 8);
    s.section_number = (int16_t)pe_u16(b + 12);
    s.type = b[14];
    s.storage_class = 0;
    s.number_of_aux_symbols = 0;
    return s;
}

/* The COFFSymbolAuxFormat5 over the memory of s, as unsafe.Pointer gives it
 * on a little-endian machine. */
static PeCOFFSymbolAuxFormat5 pe_aux_view(const PeCOFFSymbol *s) {
    PeCOFFSymbolAuxFormat5 x;
    x.size = pe_u32(s->name);
    x.num_relocs = pe_u16(s->name + 4);
    x.num_line_numbers = pe_u16(s->name + 6);
    x.checksum = s->value;
    x.sec_num = (uint16_t)s->section_number;
    x.selection = (uint8_t)(s->type & 0xff);
    x.unused_[0] = (Byte)(s->type >> 8);
    x.unused_[1] = s->storage_class;
    x.unused_[2] = s->number_of_aux_symbols;
    return x;
}

/* readCOFFSymbols. Each record is read on its own, as Go does, so that a
 * table that ends early fails at the same record with the same error. */
static Error pe_read_coff_symbols(PeFile *f, Alloc *fa, IoSectionReader *sr) {
    const PeFileHeader *fh = &f->file_header;
    if (fh->pointer_to_symbol_table == 0 || fh->number_of_symbols == 0)
        return BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    io_section_reader_seek(sr, (int64_t)fh->pointer_to_symbol_table,
                           BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("fail to seek to symbol table: %v", e);
    Int c = burrow__saferio_slice_cap(sizeof(PeCOFFSymbol), fh->number_of_symbols);
    if (c < 0)
        return pe_err_too_many_symbols;
    PeCOFFSymbol *syms =
        (PeCOFFSymbol *)mem_alloc(fa, (size_t)c * sizeof *syms, _Alignof(PeCOFFSymbol));
    if (syms == NULL)
        return burrow_err_out_of_memory;
    Int len = 0;
    int naux = 0;
    IoReader r = io_section_reader_as_io_reader(sr);
    for (uint32_t k = 0; k < fh->number_of_symbols; k++) {
        Byte b[PE_COFF_SYMBOL_SIZE];
        io_read_full(r, pe_bytes(b, PE_COFF_SYMBOL_SIZE), &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("fail to read symbol table: %v", e);
        PeCOFFSymbol sym;
        if (naux == 0) {
            /* Read a primary symbol and record how many auxiliary symbols it
             * has. */
            sym = pe_coff_symbol(b);
            naux = sym.number_of_aux_symbols;
        } else {
            /* Read an aux symbol. All aux symbols are taken to be format 5. */
            naux--;
            sym = pe_coff_aux_symbol(b);
        }
        if (len == c) {
            Int nc = c * 2;
            PeCOFFSymbol *ns = (PeCOFFSymbol *)mem_alloc(fa, (size_t)nc * sizeof *ns,
                                                         _Alignof(PeCOFFSymbol));
            if (ns == NULL)
                return burrow_err_out_of_memory;
            memcpy(ns, syms, (size_t)len * sizeof *ns);
            syms = ns;
            c = nc;
        }
        syms[len++] = sym;
    }
    if (naux != 0)
        return fmt_errorf_v("fail to read symbol table: %d aux symbols unread", naux);
    Slice s = {syms, len, c, NULL};
    f->coff_symbols = s;
    return BURROW_NO_ERROR;
}

/* removeAuxSymbols, and the format 5 view of every record for
 * COFFSymbolReadSectionDefAux. */
static Error pe_remove_aux_symbols(PeFile *f, Alloc *fa) {
    Int n = f->coff_symbols.len;
    if (n == 0)
        return BURROW_NO_ERROR;
    const PeCOFFSymbol *all = (const PeCOFFSymbol *)f->coff_symbols.p;
    Int count = 0;
    for (Int i = 0; i < n; i += 1 + all[i].number_of_aux_symbols)
        count++;
    PeSymbol *syms =
        (PeSymbol *)mem_alloc(fa, (size_t)count * sizeof *syms, _Alignof(PeSymbol));
    PeSymbol **ptrs =
        (PeSymbol **)mem_alloc(fa, (size_t)count * sizeof *ptrs, _Alignof(PeSymbol *));
    PeCOFFSymbolAuxFormat5 *aux = (PeCOFFSymbolAuxFormat5 *)mem_alloc(
        fa, (size_t)n * sizeof *aux, _Alignof(PeCOFFSymbolAuxFormat5));
    if (syms == NULL || ptrs == NULL || aux == NULL)
        return burrow_err_out_of_memory;
    Int k = 0;
    for (Int i = 0; i < n; i += 1 + all[i].number_of_aux_symbols) {
        Error e = BURROW_NO_ERROR;
        Str name = pe_coff_symbol_full_name(&all[i], f->string_table, &e);
        if (BURROW_FAILED(e))
            return e;
        PeSymbol s = {name, all[i].value, all[i].section_number, all[i].type,
                      all[i].storage_class};
        syms[k] = s;
        ptrs[k] = &syms[k];
        k++;
    }
    for (Int i = 0; i < n; i++)
        aux[i] = pe_aux_view(&all[i]);
    Slice s = {ptrs, count, count, TYPE_UNSAFE_POINTER};
    f->symbols = s;
    f->aux = aux;
    return BURROW_NO_ERROR;
}

PeCOFFSymbolAuxFormat5 *pe_file_coff_symbol_read_section_def_aux(PeFile *f, Int idx,
                                                                 Error *err) {
    if (f == NULL)
        pe_nil_panic();
    if (idx < 0 || idx >= f->coff_symbols.len) {
        BURROW_OUT(err, pe_err_symbol_index);
        return NULL;
    }
    const PeCOFFSymbol *all = (const PeCOFFSymbol *)f->coff_symbols.p;
    enum { IMAGE_SYM_CLASS_STATIC = 3 };
    if (all[idx].storage_class != IMAGE_SYM_CLASS_STATIC) {
        BURROW_OUT(err, pe_err_storage_class);
        return NULL;
    }
    if (all[idx].number_of_aux_symbols == 0 || idx + 1 >= f->coff_symbols.len) {
        BURROW_OUT(err, pe_err_aux_unavailable);
        return NULL;
    }
    /* Locate and return a pointer to the successor aux symbol. */
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &f->aux[idx + 1];
}

/* ------------------------------------------------------- optional headers */

/* The minimum sizes of the optional headers, which are their sizes without
 * the data directories. */
enum { PE_OH32_MIN_SIZE = 96, PE_OH64_MIN_SIZE = 112 };

/* Reads one field after another the way Go's read closure does, stopping at
 * the first that fails. */
typedef struct PeFieldReader {
    IoReader r;
    Error e;
} PeFieldReader;

static uint64_t pe_field(PeFieldReader *fr, Int n) {
    Byte b[8];
    if (BURROW_FAILED(fr->e))
        return 0;
    io_read_full(fr->r, pe_bytes(b, n), &fr->e);
    if (BURROW_FAILED(fr->e))
        return 0;
    return pe_uint(b, n);
}

/* readDataDirectories. */
static Error pe_read_data_directories(PeDataDirectory dst[16], IoReader r, Alloc *tmp,
                                      uint16_t sz, uint32_t n) {
    if ((uint64_t)sz != (uint64_t)n * 8) {
        return fmt_errorf_v("size of data directories(%d) is inconsistent with number "
                            "of data directories(%d)",
                            sz, n);
    }
    if (n == 0)
        return BURROW_NO_ERROR;
    Byte *b = (Byte *)mem_alloc_nozero(tmp, sz, 1);
    if (b == NULL)
        return burrow_err_out_of_memory;
    Error e = BURROW_NO_ERROR;
    io_read_full(r, pe_bytes(b, sz), &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("failure to read data directories: %v", e);
    for (uint32_t i = 0; i < n && i < 16; i++) {
        dst[i].virtual_address = pe_u32(b + 8 * i);
        dst[i].size = pe_u32(b + 8 * i + 4);
    }
    return BURROW_NO_ERROR;
}

static Error pe_read_oh32(PeFile *f, Alloc *fa, Alloc *tmp, IoReader r, uint16_t sz) {
    if (sz < PE_OH32_MIN_SIZE) {
        return fmt_errorf_v("optional header size(%d) is less minimum size (%d) of "
                            "PE32 optional header",
                            sz, (int)PE_OH32_MIN_SIZE);
    }
    PeOptionalHeader32 *oh = (PeOptionalHeader32 *)mem_alloc(
        fa, sizeof(PeOptionalHeader32), _Alignof(PeOptionalHeader32));
    if (oh == NULL)
        return burrow_err_out_of_memory;
    PeFieldReader fr = {r, BURROW_NO_ERROR};
    oh->magic = 0x10b;
    oh->major_linker_version = (uint8_t)pe_field(&fr, 1);
    oh->minor_linker_version = (uint8_t)pe_field(&fr, 1);
    oh->size_of_code = (uint32_t)pe_field(&fr, 4);
    oh->size_of_initialized_data = (uint32_t)pe_field(&fr, 4);
    oh->size_of_uninitialized_data = (uint32_t)pe_field(&fr, 4);
    oh->address_of_entry_point = (uint32_t)pe_field(&fr, 4);
    oh->base_of_code = (uint32_t)pe_field(&fr, 4);
    oh->base_of_data = (uint32_t)pe_field(&fr, 4);
    oh->image_base = (uint32_t)pe_field(&fr, 4);
    oh->section_alignment = (uint32_t)pe_field(&fr, 4);
    oh->file_alignment = (uint32_t)pe_field(&fr, 4);
    oh->major_operating_system_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_operating_system_version = (uint16_t)pe_field(&fr, 2);
    oh->major_image_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_image_version = (uint16_t)pe_field(&fr, 2);
    oh->major_subsystem_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_subsystem_version = (uint16_t)pe_field(&fr, 2);
    oh->win32_version_value = (uint32_t)pe_field(&fr, 4);
    oh->size_of_image = (uint32_t)pe_field(&fr, 4);
    oh->size_of_headers = (uint32_t)pe_field(&fr, 4);
    oh->check_sum = (uint32_t)pe_field(&fr, 4);
    oh->subsystem = (uint16_t)pe_field(&fr, 2);
    oh->dll_characteristics = (uint16_t)pe_field(&fr, 2);
    oh->size_of_stack_reserve = (uint32_t)pe_field(&fr, 4);
    oh->size_of_stack_commit = (uint32_t)pe_field(&fr, 4);
    oh->size_of_heap_reserve = (uint32_t)pe_field(&fr, 4);
    oh->size_of_heap_commit = (uint32_t)pe_field(&fr, 4);
    oh->loader_flags = (uint32_t)pe_field(&fr, 4);
    oh->number_of_rva_and_sizes = (uint32_t)pe_field(&fr, 4);
    if (BURROW_FAILED(fr.e))
        return fmt_errorf_v("failure to read PE32 optional header: %v", fr.e);
    Error e = pe_read_data_directories(oh->data_directory, r, tmp,
                                       (uint16_t)(sz - PE_OH32_MIN_SIZE),
                                       oh->number_of_rva_and_sizes);
    if (BURROW_FAILED(e))
        return e;
    f->optional_header = BURROW_ANY(TYPE_PE_OPTIONAL_HEADER32, oh);
    return BURROW_NO_ERROR;
}

static Error pe_read_oh64(PeFile *f, Alloc *fa, Alloc *tmp, IoReader r, uint16_t sz) {
    if (sz < PE_OH64_MIN_SIZE) {
        return fmt_errorf_v("optional header size(%d) is less minimum size (%d) for "
                            "PE32+ optional header",
                            sz, (int)PE_OH64_MIN_SIZE);
    }
    PeOptionalHeader64 *oh = (PeOptionalHeader64 *)mem_alloc(
        fa, sizeof(PeOptionalHeader64), _Alignof(PeOptionalHeader64));
    if (oh == NULL)
        return burrow_err_out_of_memory;
    PeFieldReader fr = {r, BURROW_NO_ERROR};
    oh->magic = 0x20b;
    oh->major_linker_version = (uint8_t)pe_field(&fr, 1);
    oh->minor_linker_version = (uint8_t)pe_field(&fr, 1);
    oh->size_of_code = (uint32_t)pe_field(&fr, 4);
    oh->size_of_initialized_data = (uint32_t)pe_field(&fr, 4);
    oh->size_of_uninitialized_data = (uint32_t)pe_field(&fr, 4);
    oh->address_of_entry_point = (uint32_t)pe_field(&fr, 4);
    oh->base_of_code = (uint32_t)pe_field(&fr, 4);
    oh->image_base = pe_field(&fr, 8);
    oh->section_alignment = (uint32_t)pe_field(&fr, 4);
    oh->file_alignment = (uint32_t)pe_field(&fr, 4);
    oh->major_operating_system_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_operating_system_version = (uint16_t)pe_field(&fr, 2);
    oh->major_image_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_image_version = (uint16_t)pe_field(&fr, 2);
    oh->major_subsystem_version = (uint16_t)pe_field(&fr, 2);
    oh->minor_subsystem_version = (uint16_t)pe_field(&fr, 2);
    oh->win32_version_value = (uint32_t)pe_field(&fr, 4);
    oh->size_of_image = (uint32_t)pe_field(&fr, 4);
    oh->size_of_headers = (uint32_t)pe_field(&fr, 4);
    oh->check_sum = (uint32_t)pe_field(&fr, 4);
    oh->subsystem = (uint16_t)pe_field(&fr, 2);
    oh->dll_characteristics = (uint16_t)pe_field(&fr, 2);
    oh->size_of_stack_reserve = pe_field(&fr, 8);
    oh->size_of_stack_commit = pe_field(&fr, 8);
    oh->size_of_heap_reserve = pe_field(&fr, 8);
    oh->size_of_heap_commit = pe_field(&fr, 8);
    oh->loader_flags = (uint32_t)pe_field(&fr, 4);
    oh->number_of_rva_and_sizes = (uint32_t)pe_field(&fr, 4);
    if (BURROW_FAILED(fr.e))
        return fmt_errorf_v("failure to read PE32+ optional header: %v", fr.e);
    Error e = pe_read_data_directories(oh->data_directory, r, tmp,
                                       (uint16_t)(sz - PE_OH64_MIN_SIZE),
                                       oh->number_of_rva_and_sizes);
    if (BURROW_FAILED(e))
        return e;
    f->optional_header = BURROW_ANY(TYPE_PE_OPTIONAL_HEADER64, oh);
    return BURROW_NO_ERROR;
}

/* readOptionalHeader. A size of 0 leaves the optional header nil. */
static Error pe_read_optional_header(PeFile *f, Alloc *fa, Alloc *tmp, IoReader r,
                                     uint16_t sz) {
    if (sz == 0)
        return BURROW_NO_ERROR;
    if (sz < 2)
        return pe_err_magic_size;
    Byte mb[2];
    Error e = BURROW_NO_ERROR;
    io_read_full(r, pe_bytes(mb, 2), &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("failure to read optional header magic: %v", e);
    uint16_t magic = pe_u16(mb);
    switch (magic) {
    case 0x10b: /* PE32 */
        return pe_read_oh32(f, fa, tmp, r, sz);
    case 0x20b: /* PE32+ */
        return pe_read_oh64(f, fa, tmp, r, sz);
    default:
        return fmt_errorf_v("optional header has unexpected Magic of 0x%x", magic);
    }
}

/* --------------------------------------------------------------- sections */

static const Type pe_nobits_desc = {
    {(const Byte *)"nobitsSectionReader", 19},
    {(const Byte *)"debug/pe", 8},
    KIND_STRUCT,
    0,
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

static Int pe_nobits_read_at(void *self, Slice p, int64_t off, Error *err) {
    (void)self;
    (void)p;
    (void)off;
    BURROW_OUT(err, pe_err_nobits);
    return 0;
}

static const IoReaderAtVT pe_nobits_vt = {&pe_nobits_desc, pe_nobits_read_at};

/* SectionHeader32.fullName: the name, which is in the header when it fits
 * and is "/" and a decimal offset into the string table when it does not. */
static Str pe_section_name(Alloc *fa, const Byte name[8], PeStringTable st,
                           Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (name[0] != '/')
        return burrow__objfile_cstring(fa, name, 8);
    Error e = BURROW_NO_ERROR;
    Int i = strconv_atoi(pe_cstring(name + 1, 7), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    return pe_string_table_string(st, (uint32_t)i, err);
}

/* readRelocs. */
static Error pe_read_relocs(PeSection *s, Alloc *fa, Alloc *tmp, IoSectionReader *sr) {
    const PeSectionHeader *sh = &s->section_header;
    if (sh->number_of_relocations == 0)
        return BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    io_section_reader_seek(sr, (int64_t)sh->pointer_to_relocations,
                           BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e)) {
        return fmt_errorf_v("fail to seek to %q section relocations: %v", sh->name, e);
    }
    Int n = sh->number_of_relocations;
    Byte *b = (Byte *)mem_alloc_nozero(tmp, (size_t)n * 10, 1);
    PeReloc *rs = (PeReloc *)mem_alloc(fa, (size_t)n * sizeof *rs, _Alignof(PeReloc));
    if (b == NULL || rs == NULL)
        return burrow_err_out_of_memory;
    io_read_full(io_section_reader_as_io_reader(sr), pe_bytes(b, n * 10), &e);
    if (BURROW_FAILED(e))
        return fmt_errorf_v("fail to read section relocations: %v", e);
    for (Int i = 0; i < n; i++) {
        const Byte *p = b + 10 * i;
        PeReloc r = {pe_u32(p), pe_u32(p + 4), pe_u16(p + 8)};
        rs[i] = r;
    }
    Slice rel = {rs, n, n, NULL};
    s->relocs = rel;
    return BURROW_NO_ERROR;
}

static Error pe_read_sections(PeFile *f, IoReaderAt r, Alloc *fa, Alloc *tmp,
                              IoSectionReader *sr) {
    Int n = f->file_header.number_of_sections;
    PeSection **secs =
        (PeSection **)mem_alloc(fa, (size_t)n * sizeof *secs, _Alignof(PeSection *));
    PeSection *all =
        (PeSection *)mem_alloc(fa, (size_t)n * sizeof *all, _Alignof(PeSection));
    if (n > 0 && (secs == NULL || all == NULL))
        return burrow_err_out_of_memory;
    IoReader rd = io_section_reader_as_io_reader(sr);
    for (Int i = 0; i < n; i++) {
        Byte b[40];
        Error e = BURROW_NO_ERROR;
        io_read_full(rd, pe_bytes(b, 40), &e);
        if (BURROW_FAILED(e))
            return e;
        Str name = pe_section_name(fa, b, f->string_table, &e);
        if (BURROW_FAILED(e))
            return e;
        PeSection *s = &all[i];
        PeSectionHeader h = {
            name,           pe_u32(b + 8),  pe_u32(b + 12), pe_u32(b + 16),
            pe_u32(b + 20), pe_u32(b + 24), pe_u32(b + 28), pe_u16(b + 32),
            pe_u16(b + 34), pe_u32(b + 36),
        };
        s->section_header = h;
        IoReaderAt r2 = r;
        if (h.offset == 0) { /* .bss must have all 0s */
            r2.vt = &pe_nobits_vt;
            r2.data = NULL;
        }
        s->sr = io_new_section_reader(r2, (int64_t)h.offset, (int64_t)h.size);
        s->reader_at = io_section_reader_as_io_reader_at(&s->sr);
        s->relocs.elem = NULL;
        secs[i] = s;
    }
    Slice ss = {secs, n, n, TYPE_UNSAFE_POINTER};
    f->sections = ss;
    for (Int i = 0; i < n; i++) {
        Error e = pe_read_relocs(secs[i], fa, tmp, sr);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

Slice pe_section_data(PeSection *s, Alloc *a, Error *err) {
    if (s == NULL || s->sr.r.vt == NULL)
        pe_nil_panic();
    return burrow__saferio_read_data_at(a, io_section_reader_as_io_reader_at(&s->sr),
                                        s->section_header.size, 0, err);
}

IoReadSeeker pe_section_open(PeSection *s, Alloc *a) {
    if (s == NULL || s->sr.r.vt == NULL)
        pe_nil_panic();
    return burrow__objfile_open(a, &s->sr);
}

Int pe_section_read_at(PeSection *s, Slice p, int64_t off, Error *err) {
    if (s == NULL || s->reader_at.vt == NULL)
        pe_nil_panic();
    return s->reader_at.vt->read_at(s->reader_at.data, p, off, err);
}

void pe_reader_free(IoReadSeeker r) {
    burrow__objfile_reader_free(r);
}

/* ------------------------------------------------------------------ files */

static void pe_file_release(PeFile *f) {
    Alloc *a = f->a;
    arena_free(&f->arena);
    mem_free(a, f, sizeof *f, _Alignof(PeFile));
}

static Error pe_read_file(PeFile *f, IoReaderAt r, Alloc *fa, Alloc *tmp) {
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);
    Byte dos[96];
    Error e = BURROW_NO_ERROR;
    r.vt->read_at(r.data, pe_bytes(dos, 96), 0, &e);
    if (BURROW_FAILED(e))
        return e;
    int64_t base = 0;
    if (dos[0] == 'M' && dos[1] == 'Z') {
        int64_t signoff = (int64_t)pe_u32(dos + 0x3c);
        Byte sign[4];
        r.vt->read_at(r.data, pe_bytes(sign, 4), signoff, &e);
        if (BURROW_FAILED(e))
            return e;
        if (!(sign[0] == 'P' && sign[1] == 'E' && sign[2] == 0 && sign[3] == 0))
            return fmt_errorf_v("invalid PE file signature: % x", pe_bytes(sign, 4));
        base = signoff + 4;
    }
    io_section_reader_seek(&sr, base, BURROW_IO_SEEK_START, &e);
    e = BURROW_NO_ERROR;
    Byte hb[20];
    io_read_full(io_section_reader_as_io_reader(&sr), pe_bytes(hb, 20), &e);
    if (BURROW_FAILED(e))
        return e;
    PeFileHeader fh = {
        pe_u16(hb),      pe_u16(hb + 2),  pe_u32(hb + 4),  pe_u32(hb + 8),
        pe_u32(hb + 12), pe_u16(hb + 16), pe_u16(hb + 18),
    };
    f->file_header = fh;
    switch (fh.machine) {
    case PE_IMAGE_FILE_MACHINE_AMD64:
    case PE_IMAGE_FILE_MACHINE_ARM64:
    case PE_IMAGE_FILE_MACHINE_ARMNT:
    case PE_IMAGE_FILE_MACHINE_I386:
    case PE_IMAGE_FILE_MACHINE_RISCV32:
    case PE_IMAGE_FILE_MACHINE_RISCV64:
    case PE_IMAGE_FILE_MACHINE_RISCV128:
    case PE_IMAGE_FILE_MACHINE_UNKNOWN:
        break;
    default:
        return fmt_errorf_v("unrecognized PE machine: %#x", fh.machine);
    }

    e = pe_read_string_table(&f->string_table, fa, &fh, &sr);
    if (BURROW_FAILED(e))
        return e;
    e = pe_read_coff_symbols(f, fa, &sr);
    if (BURROW_FAILED(e))
        return e;
    e = pe_remove_aux_symbols(f, fa);
    if (BURROW_FAILED(e))
        return e;

    /* Seek past file header. */
    io_section_reader_seek(&sr, base + 20, BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e))
        return e;
    e = pe_read_optional_header(f, fa, tmp, io_section_reader_as_io_reader(&sr),
                                fh.size_of_optional_header);
    if (BURROW_FAILED(e))
        return e;
    return pe_read_sections(f, r, fa, tmp, &sr);
}

PeFile *pe_new_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        pe_nil_panic();
    PeFile *f = (PeFile *)mem_alloc(a, sizeof *f, _Alignof(PeFile));
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->a = a;
    arena_init(&f->arena, a, 0);
    f->sections.elem = TYPE_UNSAFE_POINTER;
    f->symbols.elem = TYPE_UNSAFE_POINTER;
    f->string_table.elem = TYPE_BYTE;
    Arena tmp;
    arena_init(&tmp, a, 0);
    Error e = pe_read_file(f, r, arena_allocator(&f->arena), arena_allocator(&tmp));
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        pe_file_release(f);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

PeFile *pe_open(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    PeFile *f = pe_new_file(a, os_file_as_io_reader_at(file), &e);
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

Error pe_file_close(PeFile *f) {
    if (f == NULL)
        pe_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (f->open) {
        e = os_file_close(f->file);
        f->open = false;
    }
    return e;
}

void pe_file_free(PeFile *f) {
    if (f == NULL)
        return;
    OsFile *file = f->file;
    if (f->open)
        os_file_close(file);
    os_file_free(file);
    pe_file_release(f);
}

PeSection *pe_file_section(PeFile *f, Str name) {
    if (f == NULL)
        pe_nil_panic();
    PeSection **secs = (PeSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        if (str_eq(secs[i]->section_header.name, name))
            return secs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ DWARF */

static Str pe_dwarf_suffix(const PeSection *s) {
    Str name = s->section_header.name;
    if (strings_has_prefix(name, BURROW_S(".debug_")))
        return str_from_bytes(name.p + 7, name.len - 7);
    if (strings_has_prefix(name, BURROW_S(".zdebug_")))
        return str_from_bytes(name.p + 8, name.len - 8);
    return BURROW_STR_EMPTY;
}

/* sectionData: the data for s, with its size checked. */
static Slice pe_dwarf_section_data(PeSection *s, Alloc *a, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Error e = BURROW_NO_ERROR;
    Slice b = pe_section_data(s, a, &e);
    const PeSectionHeader *sh = &s->section_header;
    if (BURROW_FAILED(e) && (uint32_t)b.len < sh->size) {
        BURROW_OUT(err, e);
        return none;
    }
    if (0 < sh->virtual_size && sh->virtual_size < sh->size)
        b.len = (Int)sh->virtual_size;
    return burrow__objfile_unzlib(a, b, err);
}

/* The sections the debug/dwarf package uses, which dwarf_new takes. */
enum { PE_DW_ABBREV, PE_DW_INFO, PE_DW_STR, PE_DW_LINE, PE_DW_RANGES, PE_DW_N };

static int pe_dwarf_slot(Str suffix) {
    static const char *const names[PE_DW_N] = {"abbrev", "info", "str", "line",
                                               "ranges"};
    for (int k = 0; k < PE_DW_N; k++) {
        if (str_eq(suffix, str_from_cstr(names[k])))
            return k;
    }
    return -1;
}

DwarfData *pe_file_dwarf(PeFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        pe_nil_panic();
    Slice dat[PE_DW_N];
    for (int k = 0; k < PE_DW_N; k++) {
        Slice none = {NULL, 0, 0, TYPE_BYTE};
        dat[k] = none;
    }
    Error e = BURROW_NO_ERROR;
    PeSection **secs = (PeSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = pe_dwarf_suffix(secs[i]);
        if (suffix.len == 0)
            continue;
        int k = pe_dwarf_slot(suffix);
        if (k < 0)
            continue;
        Slice b = pe_dwarf_section_data(secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        dat[k] = b;
    }

    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    DwarfData *d =
        dwarf_new(a, dat[PE_DW_ABBREV], nil, nil, dat[PE_DW_INFO], dat[PE_DW_LINE], nil,
                  dat[PE_DW_RANGES], dat[PE_DW_STR], &e);
    if (d == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }

    /* Look for DWARF4 .debug_types sections and DWARF5 sections. */
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = pe_dwarf_suffix(secs[i]);
        if (suffix.len == 0 || pe_dwarf_slot(suffix) >= 0)
            continue;
        Slice b = pe_dwarf_section_data(secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        bool types = str_eq(suffix, BURROW_S("types"));
        Byte num[24];
        Str name =
            types ? pe_concat(a, BURROW_S("types-"), pe_idec(num, i), BURROW_STR_EMPTY)
                  : pe_concat(a, BURROW_S(".debug_"), suffix, BURROW_STR_EMPTY);
        if (name.len == 0) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
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

/* ---------------------------------------------------------------- imports */

/* getString: the NUL-terminated string at start in section, or nothing when
 * there is no NUL after it. The Str points into section. */
static Str pe_get_string(Slice section, int64_t start) {
    if (start < 0 || start >= (int64_t)section.len)
        return BURROW_STR_EMPTY;
    const Byte *p = (const Byte *)section.p + start;
    const Byte *end = (const Byte *)memchr(p, 0, (size_t)(section.len - (Int)start));
    if (end == NULL)
        return BURROW_STR_EMPTY;
    return str_from_bytes(p, (Int)(end - p));
}

/* Appends fn:dll, in a, to the Str slice all, whose backing grows in tmp. */
static bool pe_append_import(Slice *all, Alloc *a, Alloc *tmp, Str fn, Str dll) {
    if (all->len == all->cap) {
        Int nc = all->cap == 0 ? 16 : all->cap * 2;
        Str *ns = (Str *)mem_alloc(tmp, (size_t)nc * sizeof *ns, _Alignof(Str));
        if (ns == NULL)
            return false;
        if (all->len > 0)
            memcpy(ns, all->p, (size_t)all->len * sizeof *ns);
        all->p = ns;
        all->cap = nc;
    }
    Str s = pe_concat(a, fn, BURROW_S(":"), dll);
    if (s.p == NULL && fn.len + dll.len + 1 > 0)
        return false;
    ((Str *)all->p)[all->len++] = s;
    return true;
}

/* The thunks of one import directory entry, from seek in names. */
static Error pe_read_thunks(Slice *all, Alloc *a, Alloc *tmp, Slice names,
                            uint32_t seek, uint32_t va0, bool pe64, Str dll) {
    const Byte *d = (const Byte *)names.p + seek;
    Int n = names.len - (Int)seek;
    while (n > 0) {
        uint32_t off;
        if (pe64) {
            if (n < 8)
                return pe_err_thunk8;
            uint64_t va = pe_uint(d, 8);
            d += 8;
            n -= 8;
            if (va == 0)
                break;
            if ((va & 0x8000000000000000U) != 0)
                continue; /* is Ordinal */
            off = (uint32_t)va - va0 + 2;
        } else {
            if (n <= 4)
                return pe_err_thunk5;
            uint32_t va = pe_u32(d);
            d += 4;
            n -= 4;
            if (va == 0)
                break;
            if ((va & 0x80000000U) != 0)
                continue; /* is Ordinal */
            off = va - va0 + 2;
        }
        Str fn = pe_get_string(names, (int64_t)off);
        if (!pe_append_import(all, a, tmp, fn, dll))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

static Error pe_imported_symbols(PeFile *f, Alloc *a, Alloc *tmp, Slice *all) {
    bool pe64 = f->optional_header.t == TYPE_PE_OPTIONAL_HEADER64;
    /* grab the number of data directory entries and the import one */
    uint32_t dd_length;
    PeDataDirectory idd;
    if (pe64) {
        const PeOptionalHeader64 *oh =
            (const PeOptionalHeader64 *)f->optional_header.data;
        dd_length = oh->number_of_rva_and_sizes;
        idd = oh->data_directory[PE_IMAGE_DIRECTORY_ENTRY_IMPORT];
    } else {
        const PeOptionalHeader32 *oh =
            (const PeOptionalHeader32 *)f->optional_header.data;
        dd_length = oh->number_of_rva_and_sizes;
        idd = oh->data_directory[PE_IMAGE_DIRECTORY_ENTRY_IMPORT];
    }
    if (dd_length < PE_IMAGE_DIRECTORY_ENTRY_IMPORT + 1)
        return BURROW_NO_ERROR;

    /* Find the section that holds the import directory table, by the distance
     * between the addresses so that adding the size cannot overflow. */
    PeSection *ds = NULL;
    PeSection **secs = (PeSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        const PeSectionHeader *sh = &secs[i]->section_header;
        if (sh->offset == 0)
            continue;
        if (sh->virtual_address <= idd.virtual_address &&
            idd.virtual_address - sh->virtual_address < sh->virtual_size) {
            ds = secs[i];
            break;
        }
    }
    /* didn't find a section, so no import libraries were found */
    if (ds == NULL)
        return BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    Slice names = pe_section_data(ds, tmp, &e);
    if (BURROW_FAILED(e))
        return e;
    uint32_t va0 = ds->section_header.virtual_address;
    uint32_t seek = idd.virtual_address - va0;
    if (seek >= (uint32_t)names.len)
        return pe_err_dd_seek;

    /* Decode the import directory, which ends at an entry whose
     * OriginalFirstThunk is 0. */
    const Byte *d = (const Byte *)names.p + seek;
    Int n = names.len - (Int)seek;
    Int count = 0;
    for (Int k = 0; k + 20 <= n; k += 20) {
        if (pe_u32(d + k) == 0)
            break;
        count++;
    }
    for (Int i = 0; i < count; i++) {
        const Byte *p = d + 20 * i;
        PeImportDirectory dt = {pe_u32(p), pe_u32(p + 4), pe_u32(p + 8), pe_u32(p + 12),
                                pe_u32(p + 16)};
        Str dll = pe_get_string(names, (int64_t)(uint32_t)(dt.name - va0));
        /* seek to OriginalFirstThunk */
        seek = dt.original_first_thunk - va0;
        if (seek >= (uint32_t)names.len)
            return pe_err_thunk_seek;
        e = pe_read_thunks(all, a, tmp, names, seek, va0, pe64, dll);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

Slice pe_file_imported_symbols(PeFile *f, Alloc *a, Error *err) {
    Slice nil = {NULL, 0, 0, TYPE_STRING};
    if (f == NULL)
        pe_nil_panic();
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f->optional_header.t == NULL)
        return nil;
    Arena tmp;
    arena_init(&tmp, a, 0);
    Slice all = nil;
    Error e = pe_imported_symbols(f, a, arena_allocator(&tmp), &all);
    Slice out = nil;
    if (!BURROW_FAILED(e) && all.len > 0) {
        Str *p = (Str *)mem_alloc(a, (size_t)all.len * sizeof *p, _Alignof(Str));
        if (p == NULL) {
            e = burrow_err_out_of_memory;
        } else {
            memcpy(p, all.p, (size_t)all.len * sizeof *p);
            out.p = p;
            out.len = all.len;
            out.cap = all.len;
        }
    }
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    return out;
}

Slice pe_file_imported_libraries(PeFile *f, Alloc *a, Error *err) {
    (void)a;
    if (f == NULL)
        pe_nil_panic();
    Slice nil = {NULL, 0, 0, TYPE_STRING};
    BURROW_OUT(err, BURROW_NO_ERROR);
    return nil;
}
