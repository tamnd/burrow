/* Derived from Go's src/internal/xcoff/file_test.go and ar_test.go. Go source:
 * go1.27.1.
 *
 * Go's tests check the section headers and imported libraries of its two AIX
 * executables, the members of its two big archives, and that a file that is
 * not XCOFF fails to open. All of that is here. On top of that, every file goes
 * through a dump of everything internal/xcoff can say about it, which
 * tools/gen-debug-tests.sh xcoff made with Go's package, and the dump this
 * makes has to be the same text. Each file is damaged about a hundred ways too,
 * and then the dumps have to hash the same.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/dwarf.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/debug/xcoff_internal.h"
#include "debug_dump.h"

#include "xcoff_test_gen.h"

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

#define XCOFF_BIG (INT64_C(1) << 24)

static const char *const section_names[] = {
    ".text",    ".data",      ".bss",   ".loader", ".dwinfo",
    ".dwabrev", ".dwabrevxx", ".debug", "",        ".nosuch"};

static const uint32_t section_types[] = {XCOFF_STYP_TEXT,
                                         XCOFF_STYP_DATA,
                                         XCOFF_STYP_BSS,
                                         XCOFF_STYP_LOADER,
                                         XCOFF_STYP_DEBUG,
                                         XCOFF_STYP_TYPCHK,
                                         XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWINFO,
                                         XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWLINE};

static bool csect_big(const XcoffFile *f, Str name) {
    XcoffSymbol *const *syms = (XcoffSymbol *const *)f->symbols.p;
    for (Int i = 0; i < f->symbols.len; i++) {
        if (str_eq(syms[i]->name, name) &&
            syms[i]->aux_csect.symbol_type == XCOFF_XTY_SD)
            return (uint64_t)syms[i]->aux_csect.length > (uint64_t)XCOFF_BIG;
    }
    return false;
}

static bool imports_big(XcoffFile *f) {
    XcoffSection *s = burrow__xcoff_file_section_by_type(f, XCOFF_STYP_LOADER);
    if (s == NULL)
        return false;
    Byte h[56] = {0};
    Int n = f->file_header.target_machine == XCOFF_U64_TOCMAGIC ? 56 : 32;
    Error err = BURROW_NO_ERROR;
    (void)io_section_reader_read_at(&s->sr, (Slice){h, n, n, TYPE_BYTE}, 0, &err);
    if (BURROW_FAILED(err))
        return false;
    uint32_t istlen = binary_big_endian_uint32((Slice){h + 12, 4, 4, TYPE_BYTE});
    uint32_t stlen = binary_big_endian_uint32((Slice){h + 24, 4, 4, TYPE_BYTE});
    if (n == 56)
        stlen = binary_big_endian_uint32((Slice){h + 20, 4, 4, TYPE_BYTE});
    return istlen > XCOFF_BIG || stlen > XCOFF_BIG;
}

static bool dwarf_big(XcoffFile *f) {
    static const uint32_t subtypes[] = {XCOFF_SSUBTYP_DWABREV, XCOFF_SSUBTYP_DWINFO,
                                        XCOFF_SSUBTYP_DWLINE, XCOFF_SSUBTYP_DWRNGES,
                                        XCOFF_SSUBTYP_DWSTR};
    for (size_t i = 0; i < sizeof subtypes / sizeof subtypes[0]; i++) {
        XcoffSection *s =
            burrow__xcoff_file_section_by_type(f, XCOFF_STYP_DWARF | subtypes[i]);
        if (s != NULL && io_section_reader_size(&s->sr) > XCOFF_BIG)
            return true;
    }
    return false;
}

static void dump_section(Alloc *a, IoWriter w, Int i, XcoffSection *s) {
    const XcoffSectionHeader *h = &s->section_header;
    fmt_fprintf_v(w, "section %d %q va=%#x size=%#x type=%#x relptr=%#x nreloc=%d\n", i,
                  h->name, h->virtual_address, h->size, h->type, h->relptr, h->nreloc);
    BytesBuffer rl = BYTES_BUFFER(a);
    IoWriter rw = bytes_buffer_as_io_writer(&rl);
    for (Int k = 0; k < s->relocs.len; k++) {
        const XcoffReloc *r = (const XcoffReloc *)s->relocs.p + k;
        if (r->symbol != NULL)
            fmt_fprintf_v(rw, "%#x %q", r->virtual_address, r->symbol->name);
        else
            fmt_fprintf_v(rw, "%#x nil", r->virtual_address);
        fmt_fprintf_v(rw, " %t %t %d %d\n", (bool)(r->signed_),
                      (bool)(r->instruction_fixed), r->length, r->type);
    }
    fmt_fprintf_v(w, "  relocs n=%d %s\n", s->relocs.len,
                  blob(a, bytes_buffer_bytes(&rl), BURROW_NO_ERROR));
    Error err = BURROW_NO_ERROR;
    if (io_section_reader_size(&s->sr) > XCOFF_BIG) {
        fmt_fprintf_v(w, "  data big\n");
    } else {
        Slice b = burrow__xcoff_section_data(s, a, &err);
        fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
    }
    Byte p[8];
    err = BURROW_NO_ERROR;
    Int n = io_section_reader_read_at(&s->sr, (Slice){p, 8, 8, TYPE_BYTE}, 4, &err);
    Slice got = {p, n, n, TYPE_BYTE};
    fmt_fprintf_v(w, "  readat n=%d % x%s\n", n, got, errs(a, err));
}

static void dump_csects(Alloc *a, IoWriter w, XcoffFile *f) {
    /* CSect of the first dozen names of SD symbols, then of one that is not
     * there. */
    Str names[13];
    Int nn = 0;
    XcoffSymbol *const *syms = (XcoffSymbol *const *)f->symbols.p;
    for (Int i = 0; i < f->symbols.len && nn < 12; i++) {
        if (syms[i]->aux_csect.symbol_type != XCOFF_XTY_SD)
            continue;
        bool seen = false;
        for (Int k = 0; k < nn; k++)
            seen = seen || str_eq(names[k], syms[i]->name);
        if (!seen)
            names[nn++] = syms[i]->name;
    }
    names[nn++] = BURROW_S("nosuch");
    for (Int i = 0; i < nn; i++) {
        if (csect_big(f, names[i])) {
            fmt_fprintf_v(w, "csect %q big\n", names[i]);
            continue;
        }
        bool found = false;
        Slice b = burrow__xcoff_file_csect(f, a, names[i], &found);
        if (found)
            fmt_fprintf_v(w, "csect %q %s\n", names[i], blob(a, b, BURROW_NO_ERROR));
        else
            fmt_fprintf_v(w, "csect %q nil\n", names[i]);
    }
}

static void dump_file(Alloc *a, IoWriter w, XcoffFile *f) {
    fmt_fprintf_v(w, "header magic=%#x\n", f->file_header.target_machine);
    for (Int i = 0; i < f->sections.len; i++)
        dump_section(a, w, i, ((XcoffSection **)f->sections.p)[i]);
    fmt_fprintf_v(w, "strtab %s\n", blob(a, f->string_table, BURROW_NO_ERROR));
    fmt_fprintf_v(w, "symbols n=%d\n", f->symbols.len);
    for (Int i = 0; i < f->symbols.len; i++) {
        const XcoffSymbol *s = ((XcoffSymbol **)f->symbols.p)[i];
        fmt_fprintf_v(w, "  %q value=%#x sect=%d class=%d fcn=%d csect=%d/%d/%d\n",
                      s->name, s->value, s->section_number, s->storage_class,
                      s->aux_fcn.size, s->aux_csect.length,
                      s->aux_csect.storage_mapping_class, s->aux_csect.symbol_type);
    }
    for (size_t i = 0; i < sizeof section_names / sizeof section_names[0]; i++) {
        Str name = str_from_cstr(section_names[i]);
        XcoffSection *s = burrow__xcoff_file_section(f, name);
        if (s != NULL)
            fmt_fprintf_v(w, "section %q size=%#x\n", name, s->section_header.size);
        else
            fmt_fprintf_v(w, "section %q nil\n", name);
    }
    for (size_t i = 0; i < sizeof section_types / sizeof section_types[0]; i++) {
        XcoffSection *s = burrow__xcoff_file_section_by_type(f, section_types[i]);
        if (s != NULL)
            fmt_fprintf_v(w, "type %#x %q\n", section_types[i], s->section_header.name);
        else
            fmt_fprintf_v(w, "type %#x nil\n", section_types[i]);
    }
    dump_csects(a, w, f);
    if (imports_big(f)) {
        fmt_fprintf_v(w, "imports skipped\n");
    } else {
        Error err = BURROW_NO_ERROR;
        Slice syms = burrow__xcoff_file_imported_symbols(f, a, &err);
        fmt_fprintf_v(w, "imported n=%d%s\n", syms.len, errs(a, err));
        for (Int i = 0; i < syms.len; i++) {
            const XcoffImportedSymbol *s = (const XcoffImportedSymbol *)syms.p + i;
            fmt_fprintf_v(w, "  %q %q\n", s->name, s->library);
        }
        err = BURROW_NO_ERROR;
        Slice libs = burrow__xcoff_file_imported_libraries(f, a, &err);
        dump_strings(a, w, BURROW_S("libraries"), libs, err);
        dump_strings(a, w, BURROW_S("libpaths"), f->library_paths, BURROW_NO_ERROR);
    }
    if (dwarf_big(f)) {
        fmt_fprintf_v(w, "dwarf big\n");
        return;
    }
    Error err = BURROW_NO_ERROR;
    DwarfData *d = burrow__xcoff_file_dwarf(f, a, &err);
    if (BURROW_FAILED(err) || d == NULL) {
        fmt_fprintf_v(w, "dwarf err=%q\n", error_text(err));
        return;
    }
    /* The DWARF dump is the one the ELF and Mach-O tests check line by line,
     * so only its hash is kept here. */
    BytesBuffer dw = BYTES_BUFFER(a);
    dump_dwarf(a, bytes_buffer_as_io_writer(&dw), d);
    fmt_fprintf_v(w, "dwarf %s\n", blob(a, bytes_buffer_bytes(&dw), BURROW_NO_ERROR));
}

static void dump_archive(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    IoReaderAt r = bytes_reader_as_io_reader_at(bytes_new_reader(a, data));
    XcoffArchive *arch = burrow__xcoff_new_archive(a, r, &err);
    if (arch == NULL) {
        fmt_fprintf_v(w, "archive err=%q\n", error_text(err));
        return;
    }
    fmt_fprintf_v(w, "archive members=%d\n", arch->members.len);
    for (Int i = 0; i < arch->members.len; i++) {
        const XcoffMember *m = ((XcoffMember **)arch->members.p)[i];
        fmt_fprintf_v(w, "member %d %q size=%d\n", i, m->member_header.name,
                      m->member_header.size);
        err = BURROW_NO_ERROR;
        XcoffFile *f =
            burrow__xcoff_archive_get_file(arch, a, m->member_header.name, &err);
        if (f == NULL) {
            fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
            continue;
        }
        dump_file(a, w, f);
        burrow__xcoff_file_free(f);
    }
    err = BURROW_NO_ERROR;
    XcoffFile *f = burrow__xcoff_archive_get_file(arch, a, BURROW_S("nosuch.o"), &err);
    fmt_fprintf_v(w, "getfile%s\n", errs(a, err));
    burrow__xcoff_file_free(f);
    burrow__xcoff_archive_free(arch);
}

/* Every file goes through NewFile and NewArchive both, and one of them says it
 * is not the right kind. */
static void dump(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    IoReaderAt r = bytes_reader_as_io_reader_at(bytes_new_reader(a, data));
    XcoffFile *f = burrow__xcoff_new_file(a, r, &err);
    if (f == NULL) {
        fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
    } else {
        dump_file(a, w, f);
        burrow__xcoff_file_free(f);
    }
    dump_archive(a, w, data);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    debug_check_dumps(t, xcoff_files, sizeof xcoff_files / sizeof xcoff_files[0], dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, xcoff_files, sizeof xcoff_files / sizeof xcoff_files[0],
                             dump) > 300);
}

/* -------------------------------------------------------- Go's other tests */

static const DebugFile *find_file(const char *name) {
    for (size_t i = 0; i < sizeof xcoff_files / sizeof xcoff_files[0]; i++) {
        if (strcmp(xcoff_files[i].name, name) == 0)
            return &xcoff_files[i];
    }
    abort();
}

/* Writes the generated file name into dir as file id and gives its path. */
static Str write_file(Alloc *a, TestingT *t, Str dir, int id, const char *name) {
    Str path = fmt_sprintf_v(a, "%s/%d", dir, id);
    Error err = os_write_file(path, file_bytes(a, find_file(name)), 0644);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    return path;
}

typedef struct FileTest {
    const char *file;
    uint16_t magic;
    XcoffSectionHeader sections[10];
    const char *needed;
} FileTest;

#define SH(name, va, size, type, relptr, nreloc)                                       \
    {BURROW_S_INIT(name), va, size, type, relptr, nreloc}

static const FileTest file_tests[] = {
    {
        "xcoff/gcc-ppc32-aix-dwarf2-exec",
        XCOFF_U802TOCMAGIC,
        {
            SH(".text", 0x10000290, 0x00000bbd, XCOFF_STYP_TEXT, 0x7ae6, 0x36),
            SH(".data", 0x20000e4d, 0x00000437, XCOFF_STYP_DATA, 0x7d02, 0x2b),
            SH(".bss", 0x20001284, 0x0000021c, XCOFF_STYP_BSS, 0, 0),
            SH(".loader", 0x00000000, 0x000004b3, XCOFF_STYP_LOADER, 0, 0),
            SH(".dwline", 0x00000000, 0x000000df,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWLINE, 0x7eb0, 0x7),
            SH(".dwinfo", 0x00000000, 0x00000314,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWINFO, 0x7ef6, 0xa),
            SH(".dwabrev", 0x00000000, 0x000000d6,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWABREV, 0, 0),
            SH(".dwarnge", 0x00000000, 0x00000020,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWARNGE, 0x7f5a, 0x2),
            SH(".dwloc", 0x00000000, 0x00000074, XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWLOC,
               0, 0),
            SH(".debug", 0x00000000, 0x00005e4f, XCOFF_STYP_DEBUG, 0, 0),
        },
        "libc.a/shr.o",
    },
    {
        "xcoff/gcc-ppc64-aix-dwarf2-exec",
        XCOFF_U64_TOCMAGIC,
        {
            SH(".text", 0x10000480, 0x00000afd, XCOFF_STYP_TEXT, 0x8322, 0x34),
            SH(".data", 0x20000f7d, 0x000002f3, XCOFF_STYP_DATA, 0x85fa, 0x25),
            SH(".bss", 0x20001270, 0x00000428, XCOFF_STYP_BSS, 0, 0),
            SH(".loader", 0x00000000, 0x00000535, XCOFF_STYP_LOADER, 0, 0),
            SH(".dwline", 0x00000000, 0x000000b4,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWLINE, 0x8800, 0x4),
            SH(".dwinfo", 0x00000000, 0x0000036a,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWINFO, 0x8838, 0x7),
            SH(".dwabrev", 0x00000000, 0x000000b5,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWABREV, 0, 0),
            SH(".dwarnge", 0x00000000, 0x00000040,
               XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWARNGE, 0x889a, 0x2),
            SH(".dwloc", 0x00000000, 0x00000062, XCOFF_STYP_DWARF | XCOFF_SSUBTYP_DWLOC,
               0, 0),
            SH(".debug", 0x00000000, 0x00006605, XCOFF_STYP_DEBUG, 0, 0),
        },
        "libc.a/shr_64.o",
    },
};

static bool same_section(const XcoffSectionHeader *x, const XcoffSectionHeader *y) {
    return str_eq(x->name, y->name) && x->virtual_address == y->virtual_address &&
           x->size == y->size && x->type == y->type && x->relptr == y->relptr &&
           x->nreloc == y->nreloc;
}

/* Go opens the files in its testdata. They are in the generated header here, so
 * this writes each one out first and opens that. */
static void TestOpen(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("xcofftest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    for (size_t i = 0; i < sizeof file_tests / sizeof file_tests[0]; i++) {
        const FileTest *tt = &file_tests[i];
        Str name = write_file(a, t, dir, (int)i, tt->file);
        XcoffFile *f = burrow__xcoff_open(a, name, &err);
        if (f == NULL) {
            testing_t_errorf_v(t, "%s", error_text(err));
            continue;
        }
        if (f->file_header.target_machine != tt->magic) {
            testing_t_errorf_v(t, "open %s: have magic %#x, want %#x", tt->file,
                               f->file_header.target_machine, tt->magic);
            burrow__xcoff_file_free(f);
            continue;
        }
        XcoffSection **secs = (XcoffSection **)f->sections.p;
        for (Int k = 0; k < f->sections.len && k < 10; k++) {
            const XcoffSectionHeader *have = &secs[k]->section_header;
            const XcoffSectionHeader *want = &tt->sections[k];
            if (!same_section(have, want))
                testing_t_errorf_v(t,
                                   "open %s, section %d:\n\thave %q %#x %#x %#x %#x "
                                   "%#x\n\twant %q %#x %#x %#x %#x %#x",
                                   tt->file, k, have->name, have->virtual_address,
                                   have->size, have->type, have->relptr, have->nreloc,
                                   want->name, want->virtual_address, want->size,
                                   want->type, want->relptr, want->nreloc);
        }
        if (f->sections.len != 10)
            testing_t_errorf_v(t, "open %s: len(Sections) = %d, want 10", tt->file,
                               f->sections.len);
        err = BURROW_NO_ERROR;
        Slice libs = burrow__xcoff_file_imported_libraries(f, a, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s", error_text(err));
        if (libs.len != 1 || !str_eq(((Str *)libs.p)[0], str_from_cstr(tt->needed)))
            testing_t_errorf_v(t, "open %s: loader import = %d entries, want [%s]",
                               tt->file, libs.len, tt->needed);
        CHECK(BURROW_OK(burrow__xcoff_file_close(f)));
        /* A second Close does nothing. */
        CHECK(BURROW_OK(burrow__xcoff_file_close(f)));
        burrow__xcoff_file_free(f);
    }
    (void)os_remove_all(dir);
    arena_free(&ar);
}

typedef struct ArchiveTest {
    const char *file;
    Int nmembers;
    XcoffMemberHeader members[2];
    uint16_t member_magic[2];
} ArchiveTest;

static const ArchiveTest arch_tests[] = {
    {
        "xcoff/bigar-ppc64",
        2,
        {{BURROW_S_INIT("printbye.o"), 836}, {BURROW_S_INIT("printhello.o"), 860}},
        {XCOFF_U64_TOCMAGIC, XCOFF_U64_TOCMAGIC},
    },
    {
        "xcoff/bigar-empty",
        0,
        {{BURROW_S_INIT(""), 0}, {BURROW_S_INIT(""), 0}},
        {0, 0},
    },
};

static void TestOpenArchive(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("xcofftest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    for (size_t i = 0; i < sizeof arch_tests / sizeof arch_tests[0]; i++) {
        const ArchiveTest *tt = &arch_tests[i];
        Str name = write_file(a, t, dir, (int)i, tt->file);
        XcoffArchive *arch = burrow__xcoff_open_archive(a, name, &err);
        if (arch == NULL) {
            testing_t_errorf_v(t, "%s", error_text(err));
            continue;
        }
        if (!str_eq(arch->archive_header.magic, BURROW_S(XCOFF_AIAMAGBIG))) {
            testing_t_errorf_v(t, "open archive %s: have magic %q", tt->file,
                               arch->archive_header.magic);
            burrow__xcoff_archive_free(arch);
            continue;
        }
        XcoffMember **ms = (XcoffMember **)arch->members.p;
        for (Int k = 0; k < arch->members.len && k < tt->nmembers; k++) {
            const XcoffMemberHeader *have = &ms[k]->member_header;
            const XcoffMemberHeader *want = &tt->members[k];
            if (!str_eq(have->name, want->name) || have->size != want->size)
                testing_t_errorf_v(t, "open %s, member %d:\n\thave %q %d\n\twant %q %d",
                                   tt->file, k, have->name, have->size, want->name,
                                   want->size);
            err = BURROW_NO_ERROR;
            XcoffFile *f = burrow__xcoff_archive_get_file(arch, a, have->name, &err);
            if (f == NULL) {
                testing_t_errorf_v(t, "%s", error_text(err));
                continue;
            }
            if (f->file_header.target_machine != tt->member_magic[k])
                testing_t_errorf_v(
                    t, "open %s, member file header %d: have %#x, want %#x", tt->file,
                    k, f->file_header.target_machine, tt->member_magic[k]);
            burrow__xcoff_file_free(f);
        }
        if (arch->members.len != tt->nmembers)
            testing_t_errorf_v(t, "open %s: len(Members) = %d, want %d", tt->file,
                               arch->members.len, tt->nmembers);
        CHECK(BURROW_OK(burrow__xcoff_archive_close(arch)));
        CHECK(BURROW_OK(burrow__xcoff_archive_close(arch)));
        burrow__xcoff_archive_free(arch);
    }
    (void)os_remove_all(dir);
    arena_free(&ar);
}

static void TestOpenFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("xcofftest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    /* Not an XCOFF object file. */
    Str name = fmt_sprintf_v(a, "%s/file.go", dir);
    Str text = BURROW_S("package xcoff\n");
    err = os_write_file(
        name, (Slice){(void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE}, 0644);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    XcoffFile *f = burrow__xcoff_open(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "open %s: succeeded unexpectedly", name);
        burrow__xcoff_file_free(f);
    }
    /* "pa" is 0x7061. */
    CHECK(str_eq(error_text(err), BURROW_S("unrecognised XCOFF magic: 0x7061")));
    err = BURROW_NO_ERROR;
    f = burrow__xcoff_open(a, fmt_sprintf_v(a, "%s/missing", dir), &err);
    CHECK(f == NULL && BURROW_FAILED(err));
    err = BURROW_NO_ERROR;
    XcoffArchive *arch = burrow__xcoff_open_archive(a, name, &err);
    CHECK(arch == NULL && BURROW_FAILED(err));
    (void)os_remove_all(dir);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestOpen)                                                                        \
    X(TestOpenArchive)                                                                 \
    X(TestOpenFailure)

TESTING_MAIN(TESTS)
