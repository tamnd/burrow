/* Derived from Go's src/debug/pe/file_test.go and symbols_test.go. Go source:
 * go1.27.1.
 *
 * Go's tests check a few fields of each file in its testdata against tables
 * written by hand, and most of the rest build programs with the go command.
 * Here every file goes through a dump of everything debug/pe and debug/dwarf
 * can say about it, which tools/gen-debug-tests.sh pe made with Go's packages,
 * and the dump this makes has to be the same text. The files are damaged a few
 * dozen ways each too, and then the dumps have to hash the same. The rest are
 * Go's tests that need neither a go command nor Windows.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/dwarf.h"
#include "burrow/debug/pe.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_dump.h"

#include "pe_test_gen.h"

static PeFile *find_pe(Alloc *a, const char *name) {
    for (size_t i = 0; i < sizeof pe_files / sizeof pe_files[0]; i++) {
        if (strcmp(pe_files[i].name, name) != 0)
            continue;
        Slice b = file_bytes(a, &pe_files[i]);
        Error err = BURROW_NO_ERROR;
        PeFile *f =
            pe_new_file(a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)), &err);
        if (f == NULL)
            abort();
        return f;
    }
    abort();
}

static Slice bytes_of(const char *p, size_t n) {
    return (Slice){(void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE};
}

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

static const char *const section_names[] = {".text",       ".data",  ".bss", ".idata",
                                            ".debug_info", ".rdata", ""};

static void dump_data_directories(IoWriter w, const PeDataDirectory *dd) {
    for (int i = 0; i < 16; i++) {
        if (dd[i].virtual_address != 0 || dd[i].size != 0)
            fmt_fprintf_v(w, "  dd %d va=%#x size=%#x\n", i, dd[i].virtual_address,
                          dd[i].size);
    }
}

static void dump_optional32(IoWriter w, const PeOptionalHeader32 *h) {
    fmt_fprintf_v(w,
                  "optional32 magic=%#x linker=%d.%d code=%#x idata=%#x udata=%#x "
                  "entry=%#x codebase=%#x database=%#x image=%#x\n",
                  h->magic, h->major_linker_version, h->minor_linker_version,
                  h->size_of_code, h->size_of_initialized_data,
                  h->size_of_uninitialized_data, h->address_of_entry_point,
                  h->base_of_code, h->base_of_data, h->image_base);
    fmt_fprintf_v(
        w,
        "  align=%#x/%#x os=%d.%d image=%d.%d subsystem=%d.%d win32=%d "
        "size=%#x headers=%#x sum=%#x\n",
        h->section_alignment, h->file_alignment, h->major_operating_system_version,
        h->minor_operating_system_version, h->major_image_version,
        h->minor_image_version, h->major_subsystem_version, h->minor_subsystem_version,
        h->win32_version_value, h->size_of_image, h->size_of_headers, h->check_sum);
    fmt_fprintf_v(w,
                  "  subsystem=%d dll=%#x stack=%#x/%#x heap=%#x/%#x loader=%#x "
                  "ndd=%d\n",
                  h->subsystem, h->dll_characteristics, h->size_of_stack_reserve,
                  h->size_of_stack_commit, h->size_of_heap_reserve,
                  h->size_of_heap_commit, h->loader_flags, h->number_of_rva_and_sizes);
    dump_data_directories(w, h->data_directory);
}

static void dump_optional64(IoWriter w, const PeOptionalHeader64 *h) {
    fmt_fprintf_v(w,
                  "optional64 magic=%#x linker=%d.%d code=%#x idata=%#x udata=%#x "
                  "entry=%#x codebase=%#x image=%#x\n",
                  h->magic, h->major_linker_version, h->minor_linker_version,
                  h->size_of_code, h->size_of_initialized_data,
                  h->size_of_uninitialized_data, h->address_of_entry_point,
                  h->base_of_code, h->image_base);
    fmt_fprintf_v(
        w,
        "  align=%#x/%#x os=%d.%d image=%d.%d subsystem=%d.%d win32=%d "
        "size=%#x headers=%#x sum=%#x\n",
        h->section_alignment, h->file_alignment, h->major_operating_system_version,
        h->minor_operating_system_version, h->major_image_version,
        h->minor_image_version, h->major_subsystem_version, h->minor_subsystem_version,
        h->win32_version_value, h->size_of_image, h->size_of_headers, h->check_sum);
    fmt_fprintf_v(w,
                  "  subsystem=%d dll=%#x stack=%#x/%#x heap=%#x/%#x loader=%#x "
                  "ndd=%d\n",
                  h->subsystem, h->dll_characteristics, h->size_of_stack_reserve,
                  h->size_of_stack_commit, h->size_of_heap_reserve,
                  h->size_of_heap_commit, h->loader_flags, h->number_of_rva_and_sizes);
    dump_data_directories(w, h->data_directory);
}

static void dump_section(Alloc *a, IoWriter w, Int i, PeSection *s) {
    const PeSectionHeader *sh = &s->section_header;
    fmt_fprintf_v(w,
                  "section %d %q vsize=%#x va=%#x size=%#x offset=%#x relptr=%#x "
                  "lineptr=%#x nrel=%d nline=%d chars=%#x\n",
                  i, sh->name, sh->virtual_size, sh->virtual_address, sh->size,
                  sh->offset, sh->pointer_to_relocations, sh->pointer_to_line_numbers,
                  sh->number_of_relocations, sh->number_of_line_numbers,
                  sh->characteristics);
    Error err = BURROW_NO_ERROR;
    Slice b = pe_section_data(s, a, &err);
    fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
    IoReadSeeker rs = pe_section_open(s, a);
    err = BURROW_NO_ERROR;
    b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
    fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
    pe_reader_free(rs);
    Byte p[16];
    err = BURROW_NO_ERROR;
    Int n = pe_section_read_at(s, (Slice){p, 16, 16, TYPE_BYTE}, 4, &err);
    Str hx = hex_encode_to_string(a, (Slice){p, n, n, TYPE_BYTE});
    fmt_fprintf_v(w, "  readat n=%d %s%s\n", n, hx, errs(a, err));
    for (Int k = 0; k < s->relocs.len; k++) {
        const PeReloc *r = (const PeReloc *)s->relocs.p + k;
        fmt_fprintf_v(w, "  reloc va=%#x sym=%d type=%d\n", r->virtual_address,
                      r->symbol_table_index, r->type);
    }
}

static void dump_symbols(Alloc *a, IoWriter w, PeFile *f) {
    PeStringTable st = f->string_table;
    fmt_fprintf_v(w, "strings %s\n", blob(a, st, BURROW_NO_ERROR));
    uint32_t n = (uint32_t)st.len;
    const uint32_t offs[] = {0, 3, 4, 5, 8, n + 3, n + 4, n + 5};
    for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Str s = pe_string_table_string(st, offs[i], &err);
        fmt_fprintf_v(w, "  string %d %q%s\n", offs[i], s, errs(a, err));
    }
    const PeCOFFSymbol *coff = (const PeCOFFSymbol *)f->coff_symbols.p;
    fmt_fprintf_v(w, "coff n=%d\n", f->coff_symbols.len);
    for (Int i = 0; i < f->coff_symbols.len; i++) {
        const PeCOFFSymbol *s = &coff[i];
        Error err = BURROW_NO_ERROR;
        Str name = pe_coff_symbol_full_name(s, st, &err);
        Str hx = hex_encode_to_string(
            a, (Slice){(void *)(uintptr_t)s->name, 8, 8, TYPE_BYTE});
        fmt_fprintf_v(w, "  %d %s %q%s value=%#x sect=%d type=%#x class=%d naux=%d\n",
                      i, hx, name, errs(a, err), s->value, s->section_number, s->type,
                      s->storage_class, s->number_of_aux_symbols);
    }
    Int ncoff = f->coff_symbols.len;
    for (Int k = -3; k < ncoff; k++) {
        /* -1, 0 and the count first, then every static symbol. */
        Int i = k == -3 ? -1 : k == -2 ? 0 : k == -1 ? ncoff : k;
        if (k >= 0 && coff[k].storage_class != 3)
            continue;
        Error err = BURROW_NO_ERROR;
        const PeCOFFSymbolAuxFormat5 *x =
            pe_file_coff_symbol_read_section_def_aux(f, i, &err);
        if (BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "aux %d err=%q\n", i, error_text(err));
            continue;
        }
        /* Go prints the struct with %+v, blank field and all. */
        fmt_fprintf_v(w,
                      "aux %d {Size:%d NumRelocs:%d NumLineNumbers:%d Checksum:%d "
                      "SecNum:%d Selection:%d _:[%d %d %d]}\n",
                      i, x->size, x->num_relocs, x->num_line_numbers, x->checksum,
                      x->sec_num, x->selection, x->unused_[0], x->unused_[1],
                      x->unused_[2]);
    }
    fmt_fprintf_v(w, "symbols n=%d\n", f->symbols.len);
    for (Int i = 0; i < f->symbols.len; i++) {
        const PeSymbol *s = ((PeSymbol **)f->symbols.p)[i];
        fmt_fprintf_v(w, "  %d %q value=%#x sect=%d type=%#x class=%d\n", i, s->name,
                      s->value, s->section_number, s->type, s->storage_class);
    }
}

static void dump_pe_file(Alloc *a, IoWriter w, PeFile *f) {
    const PeFileHeader *h = &f->file_header;
    fmt_fprintf_v(w,
                  "header machine=%#x nsec=%d time=%d symptr=%#x nsyms=%d optsize=%d "
                  "chars=%#x\n",
                  h->machine, h->number_of_sections, h->time_date_stamp,
                  h->pointer_to_symbol_table, h->number_of_symbols,
                  h->size_of_optional_header, h->characteristics);
    if (f->optional_header.t == TYPE_PE_OPTIONAL_HEADER32)
        dump_optional32(w, (const PeOptionalHeader32 *)f->optional_header.data);
    else if (f->optional_header.t == TYPE_PE_OPTIONAL_HEADER64)
        dump_optional64(w, (const PeOptionalHeader64 *)f->optional_header.data);
    else
        fmt_fprintf_v(w, "optional nil\n");
    for (Int i = 0; i < f->sections.len; i++)
        dump_section(a, w, i, ((PeSection **)f->sections.p)[i]);
    dump_symbols(a, w, f);
    for (size_t i = 0; i < sizeof section_names / sizeof section_names[0]; i++) {
        Str name = str_from_cstr(section_names[i]);
        const PeSection *s = pe_file_section(f, name);
        if (s != NULL)
            fmt_fprintf_v(w, "section %q va=%#x\n", name,
                          s->section_header.virtual_address);
        else
            fmt_fprintf_v(w, "section %q nil\n", name);
    }
    Error err = BURROW_NO_ERROR;
    Slice imp = pe_file_imported_symbols(f, a, &err);
    dump_strings(a, w, BURROW_S("imported"), imp, err);
    err = BURROW_NO_ERROR;
    Slice libs = pe_file_imported_libraries(f, a, &err);
    dump_strings(a, w, BURROW_S("libraries"), libs, err);
    err = BURROW_NO_ERROR;
    DwarfData *d = pe_file_dwarf(f, a, &err);
    if (BURROW_FAILED(err) || d == NULL)
        fmt_fprintf_v(w, "dwarf err=%q\n", error_text(err));
    else
        dump_dwarf(a, w, d);
}

static void dump(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    IoReaderAt r = bytes_reader_as_io_reader_at(bytes_new_reader(a, data));
    PeFile *f = pe_new_file(a, r, &err);
    if (f == NULL) {
        fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
        return;
    }
    dump_pe_file(a, w, f);
    pe_file_free(f);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    debug_check_dumps(t, pe_files, sizeof pe_files / sizeof pe_files[0], dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, pe_files, sizeof pe_files / sizeof pe_files[0], dump) >
          200);
}

/* -------------------------------------------------------- Go's other tests */

static void TestOpenFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("petest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    /* Not a PE file. */
    Str name = fmt_sprintf_v(a, "%s/file.go", dir);
    Str text = BURROW_S("package pe\n");
    err = os_write_file(
        name, (Slice){(void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE}, 0644);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    PeFile *f = pe_open(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "open %s: succeeded unexpectedly", name);
        pe_file_free(f);
    }
    err = BURROW_NO_ERROR;
    f = pe_open(a, fmt_sprintf_v(a, "%s/missing", dir), &err);
    CHECK(f == NULL && BURROW_FAILED(err));
    (void)os_remove_all(dir);
    arena_free(&ar);
}

typedef struct AuxPoint {
    Int idx;
    const char *name;
    const char *err;
    PeCOFFSymbolAuxFormat5 aux;
} AuxPoint;

static void TestReadCOFFSymbolAuxInfo(TestingT *t) {
    static const AuxPoint points[] = {
        {39, ".rdata$.refptr.__native_startup_lock", NULL, {8, 1, 0, 0, 16, 2, {0}}},
        {81, ".debug_line", NULL, {994, 1, 0, 1624223678, 32, 0, {0}}},
        {155, ".file", "incorrect symbol storage class", {0, 0, 0, 0, 0, 0, {0}}},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PeFile *f = find_pe(a, "pe/llvm-mingw-20211002-msvcrt-x86_64-crt2");
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        const AuxPoint *tp = &points[i];
        if (tp->idx >= f->coff_symbols.len) {
            testing_t_errorf_v(t, "only %d COFF symbols", f->coff_symbols.len);
            break;
        }
        const PeCOFFSymbol *sym = (const PeCOFFSymbol *)f->coff_symbols.p + tp->idx;
        if (sym->number_of_aux_symbols == 0) {
            testing_t_errorf_v(t, "expected aux symbols for sym %d", tp->idx);
            continue;
        }
        Error err = BURROW_NO_ERROR;
        Str name = pe_coff_symbol_full_name(sym, f->string_table, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "FullName(%d) failed with %s", tp->idx,
                               error_text(err));
            continue;
        }
        if (!str_eq(name, str_from_cstr(tp->name))) {
            testing_t_errorf_v(t, "name check for %d, got %s want %s", tp->idx, name,
                               tp->name);
            continue;
        }
        const PeCOFFSymbolAuxFormat5 *ap =
            pe_file_coff_symbol_read_section_def_aux(f, tp->idx, &err);
        if (tp->err == NULL) {
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "unexpected failure on %d, got error %s", tp->idx,
                                   error_text(err));
                continue;
            }
            CHECK(ap->size == tp->aux.size);
            CHECK(ap->num_relocs == tp->aux.num_relocs);
            CHECK(ap->num_line_numbers == tp->aux.num_line_numbers);
            CHECK(ap->checksum == tp->aux.checksum);
            CHECK(ap->sec_num == tp->aux.sec_num);
            CHECK(ap->selection == tp->aux.selection);
            CHECK(memcmp(ap->unused_, tp->aux.unused_, 3) == 0);
        } else {
            CHECK(ap == NULL);
            CHECK(str_eq(error_text(err), str_from_cstr(tp->err)));
        }
    }
    pe_file_free(f);
    arena_free(&ar);
}

static void TestInvalidOptionalHeaderMagic(TestingT *t) {
    /* Files with invalid optional header magic should return an error from
     * NewFile (see https://golang.org/issue/30250 and
     * https://golang.org/issue/32126 for details). Input generated by gofuzz. */
    static const char data[] =
        "\000\000\000\00000000\000\000\000\000\000\000\00000000000000000000000000"
        "0000000000\000\000\00000000000000000000000000000000000000000000000";
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    PeFile *f = pe_new_file(a,
                            bytes_reader_as_io_reader_at(
                                bytes_new_reader(a, bytes_of(data, sizeof data - 1))),
                            &err);
    if (f != NULL || BURROW_OK(err))
        testing_t_errorf_v(t, "NewFile succeeded unexpectedly");
    arena_free(&ar);
}

static void TestImportedSymbolsNoPanicMissingOptionalHeader(TestingT *t) {
    /* https://golang.org/issue/30250: ImportedSymbols shouldn't panic if the
     * optional header is missing. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PeFile *f = find_pe(a, "pe/gcc-amd64-mingw-obj");
    if (f->optional_header.t != NULL) {
        testing_t_fatalf_v(t, "expected f.OptionalHeader to be nil, received non-nil "
                              "optional header");
        return;
    }
    Error err = BURROW_NO_ERROR;
    Slice syms = pe_file_imported_symbols(f, a, &err);
    CHECK(BURROW_OK(err));
    CHECK(syms.len == 0);
    pe_file_free(f);
    arena_free(&ar);
}

static void TestImportedSymbolsNoPanicWithSliceOutOfBound(TestingT *t) {
    /* https://golang.org/issue/30253: ImportedSymbols shouldn't panic with
     * slice out of bounds. Input generated by gofuzz. */
    static const char data[] =
        "L\001\010\000regi\000\000\000\000\000\000\000\000\340\000\017\003\013"
        "\001\002\030\000\016\000\000\000\036\000\000\000\002\000\000\200\022\000"
        "\000\000\020\000\000\000 \000\000\000\000@\000\000\020\000\000\000\002"
        "\000\000\004\000\000\000\001\000\000\000\004\000\000\000\000\000\000\000"
        "\000\220\000\000\000\004\000\000\006S\000\000\003\000\000\000\000\000 "
        "\000\000\020\000\000\000\000\020\000\000\020\000\000\000\000\000\000\020"
        "\000\000\000\000\000\000\000\000\000\000\000\000`\000\000x\003\000\000"
        "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\004\200\000\000\030\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\000\000\000\000\000\000\270`\000\000|\000\000\000\000\000\000"
        "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\000.text\000\000\000d\014\000\000\000\020\000\000\000\016\000"
        "\000\000\004\000\000\000\000\000\000\000\000\000\000\000\000\000\000`"
        "\000P`.data\000\000\000\020\000\000\000\000 \000\000\000\002\000\000\000"
        "\022\000\000\000\000\000\000\000\000\000\000\000\000\000\000@\0000\300.r"
        "data\000\0004\001\000\000\0000\000\000\000\002\000\000\000\024\000\000"
        "\000\000\000\000\000\000\000\000\000\000\000\000@\0000@.eh_fram\240\003"
        "\000\000\000@\000\000\000\004\000\000\000\026\000\000\000\000\000\000"
        "\000\000\000\000\000\000\000\000@\0000@.bss\000\000\000\000`\000\000\000"
        "\000P\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000\000\000\000\000\200\0000\300.idata\000\000x\003\000\000\000`"
        "\000\000\004\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000\000@\0000\300.CRT\000\000\000\000\030\000\000\000\000p\000\000\000"
        "\002\000\000\000\036\000\000\000\000\000\000\000\000\000\000\000\000\000"
        "\000@\0000\300.tls\000\000\000\000 \000\000\000\000\200\000\000\000\002"
        "\000\000\000 \000\000\000\000\000\000\000\000\000\000\000\000\000\0001"
        "\311H\2115\035";
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    PeFile *f = pe_new_file(a,
                            bytes_reader_as_io_reader_at(
                                bytes_new_reader(a, bytes_of(data, sizeof data - 1))),
                            &err);
    if (f == NULL) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    Slice syms = pe_file_imported_symbols(f, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    if (syms.len != 0)
        testing_t_errorf_v(t, "expected len(syms) == 0, received len(syms) = %d",
                           syms.len);
    pe_file_free(f);
    arena_free(&ar);
}

static void TestFormatError(TestingT *t) {
    PeFormatError e = {0};
    CHECK(str_eq(pe_format_error_error(&e), BURROW_S("unknown error")));
}

#define TESTS(X)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestOpenFailure)                                                                 \
    X(TestReadCOFFSymbolAuxInfo)                                                       \
    X(TestInvalidOptionalHeaderMagic)                                                  \
    X(TestImportedSymbolsNoPanicMissingOptionalHeader)                                 \
    X(TestImportedSymbolsNoPanicWithSliceOutOfBound)                                   \
    X(TestFormatError)

TESTING_MAIN(TESTS)
