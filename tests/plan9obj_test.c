/* Derived from Go's src/debug/plan9obj/file_test.go. Go source: go1.27.1.
 *
 * Go's tests check the header and sections of its two test files against a
 * table written by hand, and that a file that is not Plan 9 a.out fails to
 * open. Both are here. On top of that, every file goes through a dump of
 * everything debug/plan9obj can say about it, symbols included, which
 * tools/gen-debug-tests.sh plan9obj made with Go's package, and the dump this
 * makes has to be the same text. The files are damaged about eighty ways each
 * too, mostly in the header and the symbol table, and then the dumps have to
 * hash the same.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/plan9obj.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_dump.h"

#include "plan9obj_test_gen.h"

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

static const char *const section_names[] = {"text", "data", "syms", "spsz",
                                            "pcsz", "bss",  ""};

static void dump_section(Alloc *a, IoWriter w, Int i, Plan9objSection *s) {
    const Plan9objSectionHeader *sh = &s->section_header;
    fmt_fprintf_v(w, "section %d %q size=%#x offset=%#x\n", i, sh->name, sh->size,
                  sh->offset);
    Error err = BURROW_NO_ERROR;
    Slice b = plan9obj_section_data(s, a, &err);
    fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
    IoReadSeeker rs = plan9obj_section_open(s, a);
    err = BURROW_NO_ERROR;
    b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
    fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
    plan9obj_reader_free(rs);
    Byte p[8];
    err = BURROW_NO_ERROR;
    Int n = plan9obj_section_read_at(s, (Slice){p, 8, 8, TYPE_BYTE}, 4, &err);
    Slice got = {p, n, n, TYPE_BYTE};
    fmt_fprintf_v(w, "  readat n=%d % x%s\n", n, got, errs(a, err));
}

static void dump(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    IoReaderAt r = bytes_reader_as_io_reader_at(bytes_new_reader(a, data));
    Plan9objFile *f = plan9obj_new_file(a, r, &err);
    if (f == NULL) {
        fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
        return;
    }
    const Plan9objFileHeader *h = &f->file_header;
    fmt_fprintf_v(
        w, "header magic=%#x bss=%#x entry=%#x ptrsize=%d load=%#x hdrsize=%d\n",
        h->magic, h->bss, h->entry, h->ptr_size, h->load_address, h->hdr_size);
    for (Int i = 0; i < f->sections.len; i++)
        dump_section(a, w, i, ((Plan9objSection **)f->sections.p)[i]);
    for (size_t i = 0; i < sizeof section_names / sizeof section_names[0]; i++) {
        Str name = str_from_cstr(section_names[i]);
        Plan9objSection *s = plan9obj_file_section(f, name);
        if (s != NULL)
            fmt_fprintf_v(w, "section %q offset=%#x\n", name, s->section_header.offset);
        else
            fmt_fprintf_v(w, "section %q nil\n", name);
    }
    Slice syms = plan9obj_file_symbols(f, a, &err);
    fmt_fprintf_v(w, "symbols n=%d%s\n", syms.len, errs(a, err));
    for (Int i = 0; i < syms.len; i++) {
        const Plan9objSym *s = (const Plan9objSym *)syms.p + i;
        fmt_fprintf_v(w, "  %#x %d %q\n", s->value, s->type, s->name);
    }
    plan9obj_file_free(f);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    debug_check_dumps(t, plan9obj_files,
                      sizeof plan9obj_files / sizeof plan9obj_files[0], dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, plan9obj_files,
                             sizeof plan9obj_files / sizeof plan9obj_files[0],
                             dump) > 150);
}

/* -------------------------------------------------------- Go's other tests */

typedef struct FileTest {
    const char *file;
    Plan9objFileHeader hdr;
    Plan9objSectionHeader sections[5];
} FileTest;

static const FileTest file_tests[] = {
    {
        "plan9obj/386-plan9-exec",
        {PLAN9OBJ_MAGIC386, 0x324, 0x14, 4, 0x1000, 32},
        {
            {BURROW_S_INIT("text"), 0x4c5f, 0x20},
            {BURROW_S_INIT("data"), 0x94c, 0x4c7f},
            {BURROW_S_INIT("syms"), 0x2c2b, 0x55cb},
            {BURROW_S_INIT("spsz"), 0x0, 0x81f6},
            {BURROW_S_INIT("pcsz"), 0xf7a, 0x81f6},
        },
    },
    {
        "plan9obj/amd64-plan9-exec",
        {PLAN9OBJ_MAGIC_AMD64, 0x618, 0x13, 8, 0x200000, 40},
        {
            {BURROW_S_INIT("text"), 0x4213, 0x28},
            {BURROW_S_INIT("data"), 0xa80, 0x423b},
            {BURROW_S_INIT("syms"), 0x2c8c, 0x4cbb},
            {BURROW_S_INIT("spsz"), 0x0, 0x7947},
            {BURROW_S_INIT("pcsz"), 0xca0, 0x7947},
        },
    },
};

static const DebugFile *find_file(const char *name) {
    for (size_t i = 0; i < sizeof plan9obj_files / sizeof plan9obj_files[0]; i++) {
        if (strcmp(plan9obj_files[i].name, name) == 0)
            return &plan9obj_files[i];
    }
    abort();
}

static bool same_header(const Plan9objFileHeader *x, const Plan9objFileHeader *y) {
    return x->magic == y->magic && x->bss == y->bss && x->entry == y->entry &&
           x->ptr_size == y->ptr_size && x->load_address == y->load_address &&
           x->hdr_size == y->hdr_size;
}

/* Go opens the files in its testdata. They are in the generated header here, so
 * this writes each one out first and opens that. */
static void TestOpen(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("plan9objtest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    for (size_t i = 0; i < sizeof file_tests / sizeof file_tests[0]; i++) {
        const FileTest *tt = &file_tests[i];
        Str name = fmt_sprintf_v(a, "%s/%d", dir, (int)i);
        err = os_write_file(name, file_bytes(a, find_file(tt->file)), 0644);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s", error_text(err));
            continue;
        }
        Plan9objFile *f = plan9obj_open(a, name, &err);
        if (f == NULL) {
            testing_t_errorf_v(t, "%s", error_text(err));
            continue;
        }
        if (!same_header(&f->file_header, &tt->hdr)) {
            testing_t_errorf_v(t, "open %s: header magic=%#x entry=%#x ptrsize=%d",
                               tt->file, f->file_header.magic, f->file_header.entry,
                               f->file_header.ptr_size);
            plan9obj_file_free(f);
            continue;
        }
        Plan9objSection **secs = (Plan9objSection **)f->sections.p;
        for (Int k = 0; k < f->sections.len && k < 5; k++) {
            const Plan9objSectionHeader *have = &secs[k]->section_header;
            const Plan9objSectionHeader *want = &tt->sections[k];
            if (!str_eq(have->name, want->name) || have->size != want->size ||
                have->offset != want->offset)
                testing_t_errorf_v(t,
                                   "open %s, section %d:\n\thave %s %#x %#x\n\twant %s "
                                   "%#x %#x",
                                   tt->file, k, have->name, have->size, have->offset,
                                   want->name, want->size, want->offset);
        }
        if (f->sections.len != 5)
            testing_t_errorf_v(t, "open %s: len(Sections) = %d, want 5", tt->file,
                               f->sections.len);
        CHECK(BURROW_OK(plan9obj_file_close(f)));
        /* A second Close does nothing. */
        CHECK(BURROW_OK(plan9obj_file_close(f)));
        plan9obj_file_free(f);
    }
    (void)os_remove_all(dir);
    arena_free(&ar);
}

static void TestOpenFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("plan9objtest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    /* Not a Plan 9 a.out file. */
    Str name = fmt_sprintf_v(a, "%s/file.go", dir);
    Str text = BURROW_S("package plan9obj\n");
    err = os_write_file(
        name, (Slice){(void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE}, 0644);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    Plan9objFile *f = plan9obj_open(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "open %s: succeeded unexpectedly", name);
        plan9obj_file_free(f);
    }
    CHECK(str_eq(error_text(err),
                 BURROW_S("bad magic number '[112 97 99 107]' in record at byte 0x0")));
    err = BURROW_NO_ERROR;
    f = plan9obj_open(a, fmt_sprintf_v(a, "%s/missing", dir), &err);
    CHECK(f == NULL && BURROW_FAILED(err));
    (void)os_remove_all(dir);
    arena_free(&ar);
}

/* ErrNoSymbols has Go's text. No file can give it, since every file has a syms
 * section. */
static void TestErrNoSymbols(TestingT *t) {
    CHECK(str_eq(error_text(plan9obj_err_no_symbols), BURROW_S("no symbol section")));
}

#define TESTS(X)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestOpen)                                                                        \
    X(TestOpenFailure)                                                                 \
    X(TestErrNoSymbols)

TESTING_MAIN(TESTS)
