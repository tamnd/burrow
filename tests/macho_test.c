/* Derived from Go's src/debug/macho/file_test.go. Go source: go1.27.1.
 *
 * Go's tests check a few fields of each file in its testdata against tables
 * written by hand. Here every file goes through a dump of everything
 * debug/macho and debug/dwarf can say about it, read as a universal binary and
 * as a plain one, which tools/gen-debug-tests.sh macho made with Go's packages,
 * and the dump this makes has to be the same text. The files are damaged a few
 * dozen ways each too, and then the dumps have to hash the same. TestNames
 * checks String and GoString of every named constant and the numbers around
 * them. The rest are Go's tests that are not about the testdata tables.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/dwarf.h"
#include "burrow/debug/macho.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_dump.h"

#include "macho_test_gen.h"

static MachoFile *find_macho(Alloc *a, const char *name) {
    for (size_t i = 0; i < sizeof macho_files / sizeof macho_files[0]; i++) {
        if (strcmp(macho_files[i].name, name) != 0)
            continue;
        Slice b = file_bytes(a, &macho_files[i]);
        Error err = BURROW_NO_ERROR;
        MachoFile *f = macho_new_file(
            a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)), &err);
        if (f == NULL)
            abort();
        return f;
    }
    abort();
}

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

static const char *const segment_names[] = {"__TEXT",     "__DATA",  "__LINKEDIT",
                                            "__PAGEZERO", "__DWARF", ""};
static const char *const section_names[] = {
    "__text", "__data", "__cstring", "__debug_info", "__nl_symbol_ptr", ""};

static void dump_read_at(Alloc *a, IoWriter w, Int n, Byte *p, Error err) {
    Str hx = hex_encode_to_string(a, (Slice){p, n, n, TYPE_BYTE});
    fmt_fprintf_v(w, "  readat n=%d %s%s\n", n, hx, errs(a, err));
}

static void dump_load(Alloc *a, IoWriter w, MachoLoad l) {
    switch (l->kind) {
    case MACHO_LOAD_KIND_SEGMENT: {
        MachoSegment *s = (MachoSegment *)(void *)l;
        const MachoSegmentHeader *sh = &s->segment_header;
        fmt_fprintf_v(w,
                      "  segment cmd=%s len=%d %q addr=%#x memsz=%#x offset=%#x "
                      "filesz=%#x maxprot=%#x prot=%#x nsect=%d flag=%#x\n",
                      macho_load_cmd_string(sh->cmd, a), sh->len, sh->name, sh->addr,
                      sh->memsz, sh->offset, sh->filesz, sh->maxprot, sh->prot,
                      sh->nsect, sh->flag);
        Error err = BURROW_NO_ERROR;
        Slice b = macho_segment_data(s, a, &err);
        fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
        IoReadSeeker rs = macho_segment_open(s, a);
        err = BURROW_NO_ERROR;
        b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
        fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
        macho_reader_free(rs);
        Byte p[16];
        err = BURROW_NO_ERROR;
        Int n = macho_segment_read_at(s, (Slice){p, 16, 16, TYPE_BYTE}, 8, &err);
        dump_read_at(a, w, n, p, err);
        break;
    }
    case MACHO_LOAD_KIND_DYLIB: {
        const MachoDylib *d = (const MachoDylib *)(void *)l;
        fmt_fprintf_v(w, "  dylib %q time=%d current=%#x compat=%#x\n", d->name,
                      d->time, d->current_version, d->compat_version);
        break;
    }
    case MACHO_LOAD_KIND_SYMTAB: {
        const MachoSymtab *st = (const MachoSymtab *)(void *)l;
        const MachoSymtabCmd *c = &st->symtab_cmd;
        fmt_fprintf_v(
            w,
            "  symtab cmd=%d len=%d symoff=%#x nsyms=%d stroff=%#x strsize=%d "
            "syms=%d\n",
            c->cmd, c->len, c->symoff, c->nsyms, c->stroff, c->strsize, st->syms.len);
        break;
    }
    case MACHO_LOAD_KIND_DYSYMTAB: {
        const MachoDysymtab *dt = (const MachoDysymtab *)(void *)l;
        const MachoDysymtabCmd *c = &dt->dysymtab_cmd;
        const uint32_t v[] = {
            c->cmd,          c->len,         c->ilocalsym,      c->nlocalsym,
            c->iextdefsym,   c->nextdefsym,  c->iundefsym,      c->nundefsym,
            c->tocoffset,    c->ntoc,        c->modtaboff,      c->nmodtab,
            c->extrefsymoff, c->nextrefsyms, c->indirectsymoff, c->nindirectsyms,
            c->extreloff,    c->nextrel,     c->locreloff,      c->nlocrel,
        };
        fmt_fprintf_v(w, "  dysymtab");
        for (size_t k = 0; k < sizeof v / sizeof v[0]; k++)
            fmt_fprintf_v(w, " %d", v[k]);
        fmt_fprintf_v(w, "\n  indirect n=%d", dt->indirect_syms.len);
        for (Int k = 0; k < dt->indirect_syms.len; k++)
            fmt_fprintf_v(w, " %#x", ((const uint32_t *)dt->indirect_syms.p)[k]);
        fmt_fprintf_v(w, "\n");
        break;
    }
    case MACHO_LOAD_KIND_RPATH:
        fmt_fprintf_v(w, "  rpath %q\n", ((const MachoRpath *)(void *)l)->path);
        break;
    case MACHO_LOAD_KIND_BYTES:
        fmt_fprintf_v(w, "  bytes\n");
        break;
    default:
        break;
    }
}

static void dump_section(Alloc *a, IoWriter w, Int i, MachoSection *s) {
    const MachoSectionHeader *sh = &s->section_header;
    fmt_fprintf_v(w,
                  "section %d %q seg=%q addr=%#x size=%#x offset=%#x align=%d "
                  "reloff=%#x nreloc=%d flags=%#x\n",
                  i, sh->name, sh->seg, sh->addr, sh->size, sh->offset, sh->align,
                  sh->reloff, sh->nreloc, sh->flags);
    Error err = BURROW_NO_ERROR;
    Slice b = macho_section_data(s, a, &err);
    fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
    IoReadSeeker rs = macho_section_open(s, a);
    err = BURROW_NO_ERROR;
    b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
    fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
    macho_reader_free(rs);
    Byte p[16];
    err = BURROW_NO_ERROR;
    Int n = macho_section_read_at(s, (Slice){p, 16, 16, TYPE_BYTE}, 4, &err);
    dump_read_at(a, w, n, p, err);
    for (Int k = 0; k < s->relocs.len; k++) {
        const MachoReloc *r = (const MachoReloc *)s->relocs.p + k;
        fmt_fprintf_v(w,
                      "  reloc addr=%#x value=%#x type=%d len=%d pcrel=%t extern=%t "
                      "scattered=%t\n",
                      r->addr, r->value, r->type, r->len, r->pcrel, r->extern_,
                      r->scattered);
    }
}

static void dump_macho_file(Alloc *a, IoWriter w, MachoFile *f) {
    const MachoFileHeader *h = &f->file_header;
    fmt_fprintf_v(w,
                  "header magic=%#x cpu=%s subcpu=%#x type=%s ncmd=%d cmdsz=%d "
                  "flags=%#x byteorder=%s\n",
                  h->magic, macho_cpu_string(h->cpu, a), h->sub_cpu,
                  macho_type_string(h->type, a), h->ncmd, h->cmdsz, h->flags,
                  f->byte_order.vt->string(f->byte_order.data));
    for (Int i = 0; i < f->loads.len; i++) {
        MachoLoad l = ((MachoLoad *)f->loads.p)[i];
        fmt_fprintf_v(w, "load %d raw %s\n", i,
                      blob(a, macho_load_raw(l), BURROW_NO_ERROR));
        dump_load(a, w, l);
    }
    for (Int i = 0; i < f->sections.len; i++)
        dump_section(a, w, i, ((MachoSection **)f->sections.p)[i]);
    if (f->symtab != NULL) {
        Slice syms = f->symtab->syms;
        fmt_fprintf_v(w, "symbols n=%d\n", syms.len);
        for (Int i = 0; i < syms.len; i++) {
            const MachoSymbol *s = (const MachoSymbol *)syms.p + i;
            fmt_fprintf_v(w, "  %d %q type=%#x sect=%d desc=%#x value=%#x\n", i,
                          s->name, s->type, s->sect, s->desc, s->value);
        }
    }
    fmt_fprintf_v(w, "dysymtab %t\n", f->dysymtab != NULL);
    for (size_t i = 0; i < sizeof segment_names / sizeof segment_names[0]; i++) {
        Str name = str_from_cstr(segment_names[i]);
        const MachoSegment *s = macho_file_segment(f, name);
        if (s != NULL)
            fmt_fprintf_v(w, "segment %q addr=%#x\n", name, s->segment_header.addr);
        else
            fmt_fprintf_v(w, "segment %q nil\n", name);
    }
    for (size_t i = 0; i < sizeof section_names / sizeof section_names[0]; i++) {
        Str name = str_from_cstr(section_names[i]);
        const MachoSection *s = macho_file_section(f, name);
        if (s != NULL)
            fmt_fprintf_v(w, "section %q addr=%#x\n", name, s->section_header.addr);
        else
            fmt_fprintf_v(w, "section %q nil\n", name);
    }
    Error err = BURROW_NO_ERROR;
    Slice imp = macho_file_imported_symbols(f, a, &err);
    dump_strings(a, w, BURROW_S("imported"), imp, err);
    err = BURROW_NO_ERROR;
    Slice libs = macho_file_imported_libraries(f, a, &err);
    dump_strings(a, w, BURROW_S("libraries"), libs, err);
    err = BURROW_NO_ERROR;
    DwarfData *d = macho_file_dwarf(f, a, &err);
    if (BURROW_FAILED(err) || d == NULL)
        fmt_fprintf_v(w, "dwarf err=%q\n", error_text(err));
    else
        dump_dwarf(a, w, d);
}

static void dump(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    IoReaderAt r = bytes_reader_as_io_reader_at(bytes_new_reader(a, data));
    MachoFatFile *ff = macho_new_fat_file(a, r, &err);
    if (ff == NULL) {
        fmt_fprintf_v(w, "fat err=%q notfat=%t\n", error_text(err),
                      errors_is(err, macho_err_not_fat));
    } else {
        fmt_fprintf_v(w, "fat magic=%#x arches=%d\n", ff->magic, ff->arches.len);
        for (Int i = 0; i < ff->arches.len; i++) {
            const MachoFatArch *fa = (const MachoFatArch *)ff->arches.p + i;
            const MachoFatArchHeader *h = &fa->fat_arch_header;
            fmt_fprintf_v(w, "arch %d cpu=%s subcpu=%#x offset=%#x size=%#x align=%d\n",
                          i, macho_cpu_string(h->cpu, a), h->sub_cpu, h->offset,
                          h->size, h->align);
            dump_macho_file(a, w, fa->file);
        }
        macho_fat_file_free(ff);
    }
    err = BURROW_NO_ERROR;
    MachoFile *f = macho_new_file(a, r, &err);
    if (f == NULL) {
        fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
        return;
    }
    dump_macho_file(a, w, f);
    macho_file_free(f);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    debug_check_dumps(t, macho_files, sizeof macho_files / sizeof macho_files[0], dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, macho_files, sizeof macho_files / sizeof macho_files[0],
                             dump) > 300);
}

/* -------------------------------------------------------------------- names */

#define NAME_KINDS(X)                                                                  \
    X(macho_type, MachoType)                                                           \
    X(macho_cpu, MachoCpu)                                                             \
    X(macho_load_cmd, MachoLoadCmd)                                                    \
    X(macho_reloc_type_generic, MachoRelocTypeGeneric)                                 \
    X(macho_reloc_type_x8664, MachoRelocTypeX86_64)                                    \
    X(macho_reloc_type_arm, MachoRelocTypeARM)                                         \
    X(macho_reloc_type_arm64, MachoRelocTypeARM64)

#define NAME_WRAP(f, T)                                                                \
    static Str f##_str(long long v, Alloc *a) {                                        \
        return f##_string((T)v, a);                                                    \
    }                                                                                  \
    static Str f##_go(long long v, Alloc *a) {                                         \
        return f##_go_string((T)v, a);                                                 \
    }
NAME_KINDS(NAME_WRAP)

typedef struct NameKind {
    const char *name;
    Str (*str)(long long v, Alloc *a);
    Str (*go)(long long v, Alloc *a);
} NameKind;

#define NAME_ENTRY(f, T) {#T, f##_str, f##_go},
static const NameKind name_kinds[] = {NAME_KINDS(NAME_ENTRY)};

static void TestNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof macho_names / sizeof macho_names[0]; i++) {
        const DebugName *n = &macho_names[i];
        const NameKind *k = &name_kinds[n->kind];
        Str s = k->str(n->value, a);
        if (!str_eq(s, str_from_cstr(n->str)))
            testing_t_errorf_v(t, "%s(%d).String() = %q, want %q", k->name,
                               (int64_t)n->value, s, n->str);
        Str g = k->go(n->value, a);
        if (!str_eq(g, str_from_cstr(n->go)))
            testing_t_errorf_v(t, "%s(%d).GoString() = %q, want %q", k->name,
                               (int64_t)n->value, g, n->go);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- Go's other tests */

static void TestOpenFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("machotest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    /* Not a Mach-O file. */
    Str name = fmt_sprintf_v(a, "%s/file.go", dir);
    Str text = BURROW_S("package macho\n");
    err = os_write_file(
        name, (Slice){(void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE}, 0644);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    MachoFile *f = macho_open(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "open %s: succeeded unexpectedly", name);
        macho_file_free(f);
    }
    err = BURROW_NO_ERROR;
    MachoFatFile *ff = macho_open_fat(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "OpenFat %s: succeeded unexpectedly", name);
        macho_fat_file_free(ff);
    }
    err = BURROW_NO_ERROR;
    f = macho_open(a, fmt_sprintf_v(a, "%s/missing", dir), &err);
    CHECK(f == NULL && BURROW_FAILED(err));
    (void)os_remove_all(dir);
    arena_free(&ar);
}

static void TestOpenFatFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof macho_files / sizeof macho_files[0]; i++) {
        if (strcmp(macho_files[i].name, "macho/gcc-386-darwin-exec") != 0)
            continue;
        Slice b = file_bytes(a, &macho_files[i]);
        Error err = BURROW_NO_ERROR;
        MachoFatFile *ff = macho_new_fat_file(
            a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)), &err);
        if (!errors_is(err, macho_err_not_fat))
            testing_t_errorf_v(t, "OpenFat %s: got %q, want ErrNotFat",
                               macho_files[i].name, error_text(err));
        CHECK(ff == NULL);
        /* ErrNotFat is a FormatError, and a copy of it is still ErrNotFat. */
        const MachoFormatError *fe =
            (const MachoFormatError *)errors_as(err, TYPE_MACHO_FORMAT_ERROR);
        CHECK(fe != NULL);
        Error c = error_retain(a, err);
        CHECK(errors_is(c, macho_err_not_fat));
        CHECK(str_eq(error_text(c),
                     BURROW_S("not a fat Mach-O file in record at byte 0x0")));
    }
    arena_free(&ar);
}

static void TestOpenFat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MachoFile *f386 = find_macho(a, "macho/gcc-386-darwin-exec");
    MachoFile *famd64 = find_macho(a, "macho/gcc-amd64-darwin-exec");
    for (size_t i = 0; i < sizeof macho_files / sizeof macho_files[0]; i++) {
        if (strcmp(macho_files[i].name, "macho/fat-gcc-386-amd64-darwin-exec") != 0)
            continue;
        Slice b = file_bytes(a, &macho_files[i]);
        Error err = BURROW_NO_ERROR;
        MachoFatFile *ff = macho_new_fat_file(
            a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)), &err);
        if (ff == NULL) {
            testing_t_fatalf_v(t, "%s", error_text(err));
            return;
        }
        CHECK(ff->magic == MACHO_MAGIC_FAT);
        CHECK(ff->arches.len == 2);
        MachoFile *want[2] = {f386, famd64};
        for (Int k = 0; k < ff->arches.len && k < 2; k++) {
            MachoFatArch *fa = (MachoFatArch *)ff->arches.p + k;
            const MachoFileHeader *got = &fa->file->file_header;
            const MachoFileHeader *w = &want[k]->file_header;
            CHECK(fa->fat_arch_header.cpu == w->cpu);
            CHECK(fa->fat_arch_header.sub_cpu == w->sub_cpu);
            CHECK(memcmp(got, w, sizeof *got) == 0);
            CHECK(macho_fat_arch_segment(fa, BURROW_S("__TEXT")) != NULL);
            CHECK(macho_fat_arch_section(fa, BURROW_S("__text")) != NULL);
        }
        macho_fat_file_free(ff);
    }
    arena_free(&ar);
}

static void TestRelocTypeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(macho_reloc_type_x8664_string(MACHO_X86_64_RELOC_BRANCH, a),
                 BURROW_S("X86_64_RELOC_BRANCH")));
    CHECK(str_eq(macho_reloc_type_x8664_go_string(MACHO_X86_64_RELOC_BRANCH, a),
                 BURROW_S("macho.X86_64_RELOC_BRANCH")));
    arena_free(&ar);
}

static void TestTypeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(macho_type_string(MACHO_TYPE_EXEC, a), BURROW_S("Exec")));
    CHECK(str_eq(macho_type_go_string(MACHO_TYPE_EXEC, a), BURROW_S("macho.Exec")));
    arena_free(&ar);
}

static void TestOpenBadDysymCmd(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof macho_files / sizeof macho_files[0]; i++) {
        if (strcmp(macho_files[i].name, "macho/gcc-amd64-darwin-exec-with-bad-dysym") !=
            0)
            continue;
        Slice b = file_bytes(a, &macho_files[i]);
        Error err = BURROW_NO_ERROR;
        MachoFile *f = macho_new_file(
            a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)), &err);
        if (f != NULL || BURROW_OK(err))
            testing_t_errorf_v(t, "NewFile did not fail when opening a file with an "
                                  "invalid dynamic symbol table command");
    }
    arena_free(&ar);
}

/* ImportedSymbols slices the symbol table with the dynamic symbol table's
 * numbers without checking them, so Go panics when they are past its end. */
static void TestImportedSymbolsPanic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MachoFile *f = find_macho(a, "macho/gcc-amd64-darwin-exec");
    if (f->dysymtab == NULL || f->symtab == NULL) {
        testing_t_fatalf_v(t, "no dynamic symbol table");
        return;
    }
    MachoDysymtabCmd keep = f->dysymtab->dysymtab_cmd;
    Int n = f->symtab->syms.cap;
    f->dysymtab->dysymtab_cmd.iundefsym = 0;
    f->dysymtab->dysymtab_cmd.nundefsym = (uint32_t)n + 1;
    Error err = BURROW_NO_ERROR;
    char want[96];
    snprintf(want, sizeof want,
             "runtime error: slice bounds out of range [:%d] with capacity %d",
             (int)n + 1, (int)n);
    CHECK_PANIC((void)macho_file_imported_symbols(f, a, &err), want);
    f->dysymtab->dysymtab_cmd.iundefsym = 3;
    f->dysymtab->dysymtab_cmd.nundefsym = UINT32_MAX;
    CHECK_PANIC((void)macho_file_imported_symbols(f, a, &err),
                "runtime error: slice bounds out of range [3:2]");
    f->dysymtab->dysymtab_cmd = keep;
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestNames)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestOpenFailure)                                                                 \
    X(TestOpenFat)                                                                     \
    X(TestOpenFatFailure)                                                              \
    X(TestRelocTypeString)                                                             \
    X(TestTypeString)                                                                  \
    X(TestOpenBadDysymCmd)                                                             \
    X(TestImportedSymbolsPanic)

TESTING_MAIN(TESTS)
