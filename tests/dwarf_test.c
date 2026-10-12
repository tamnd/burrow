/* Derived from Go's src/debug/dwarf/entry_test.go, line_test.go and
 * type_test.go. Go source: go1.27.1.
 *
 * tests/elf_test.c already reads every DWARF file in Go's testdata through a
 * dump that has to match Go's line for line, which covers what most of Go's
 * tests here check one field at a time. These are the rest: the ones that look
 * at a particular PC, a particular typedef or a fresh type cache, and the ones
 * that build their own DWARF. TestDwarf5Ranges is left out, since it calls an
 * unexported method and its check is the wrong way round, so it cannot fail.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/debug/dwarf.h"
#include "burrow/debug/elf.h"
#include "burrow/encoding/base64.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#include "../src/debug/dwarf_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct DebugDamage {
    int pos;
    int xor_;
    const char *hash;
} DebugDamage;

typedef struct DebugFile {
    const char *name;
    bool gz;
    const char *const *data;
    const char *const *dump;
    const DebugDamage *damage;
} DebugFile;

typedef struct DebugName {
    int kind;
    long long value;
    const char *str;
    const char *go;
} DebugName;

#include "debug_test_gen.h"

static Slice gen_bytes(Alloc *a, const char *const *lines) {
    Int n = 0;
    for (const char *const *l = lines; *l != NULL; l++)
        n += (Int)strlen(*l);
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    if (p == NULL)
        abort();
    Int off = 0;
    for (const char *const *l = lines; *l != NULL; l++) {
        size_t k = strlen(*l);
        memcpy(p + off, *l, k);
        off += (Int)k;
    }
    Error err = BURROW_NO_ERROR;
    Slice out = base64_encoding_decode_string(base64_std_encoding, a,
                                              str_from_bytes(p, n), &err);
    if (BURROW_FAILED(err))
        abort();
    return out;
}

/* Go's elfData: the DWARF in one of the test files. The files here are none of
 * them gzipped, but the elf ones can be. */
static DwarfData *elf_data(TestingT *t, Alloc *a, const char *name) {
    const DebugFile *df = NULL;
    for (size_t i = 0; i < sizeof debug_files / sizeof debug_files[0]; i++)
        if (strcmp(debug_files[i].name, name) == 0)
            df = &debug_files[i];
    if (df == NULL)
        testing_t_fatalf_v(t, "no test file %s", name);
    Slice data = gen_bytes(a, df->data);
    Error err = BURROW_NO_ERROR;
    if (df->gz) {
        GzipReader *zr = gzip_new_reader(
            a, bytes_reader_as_io_reader(bytes_new_reader(a, data)), &err);
        if (BURROW_OK(err))
            data = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s: %s", name, error_text(err));
    }
    ElfFile *f =
        elf_new_file(a, bytes_reader_as_io_reader_at(bytes_new_reader(a, data)), &err);
    if (BURROW_FAILED(err) || f == NULL)
        testing_t_fatalf_v(t, "%s: %s", name, error_text(err));
    DwarfData *d = elf_file_dwarf(f, a, &err);
    if (BURROW_FAILED(err) || d == NULL)
        testing_t_fatalf_v(t, "%s: %s", name, error_text(err));
    return d;
}

static DwarfEntry *must_next(TestingT *t, DwarfReader *r) {
    Error err = BURROW_NO_ERROR;
    DwarfEntry *e = dwarf_reader_next(r, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "r.Next: %s", error_text(err));
    return e;
}

static Str entry_name(const DwarfEntry *e) {
    Str *s = (Str *)any_assert(dwarf_entry_val(e, DWARF_ATTR_NAME), TYPE_STRING);
    return s != NULL ? *s : BURROW_STR_EMPTY;
}

/* ---------------------------------------------------------------- type_test */

static const char *const typedef_tests[][2] = {
    {"t_ptr_volatile_int", "*volatile int"},
    {"t_ptr_const_char", "*const char"},
    {"t_long", "long int"},
    {"t_ushort", "short unsigned int"},
    {"t_func_int_of_float_double", "func(float, double) int"},
    {"t_ptr_func_int_of_float_double", "*func(float, double) int"},
    {"t_ptr_func_int_of_float_complex", "*func(complex float) int"},
    {"t_ptr_func_int_of_double_complex", "*func(complex double) int"},
    {"t_ptr_func_int_of_long_double_complex", "*func(complex long double) int"},
    {"t_func_ptr_int_of_char_schar_uchar",
     "func(char, signed char, unsigned char) *int"},
    {"t_func_void_of_char", "func(char) void"},
    {"t_func_void_of_void", "func() void"},
    {"t_func_void_of_ptr_char_dots", "func(*char, ...) void"},
    {"t_my_struct", "struct my_struct {vi volatile int@0; x char@4 : 1@7; y int@4 : "
                    "4@27; z [0]int@8; array [40]long long int@8; zz [0]int@328}"},
    {"t_my_struct1", "struct my_struct1 {zz [1]int@0}"},
    {"t_my_union", "union my_union {vi volatile int@0; x char@0 : 1@7; y int@0 : 4@28; "
                   "array [40]long long int@0}"},
    {"t_my_enum", "enum my_enum {e1=1; e2=2; e3=-5; e4=1000000000000000}"},
    {"t_my_list", "struct list {val short int@0; next *t_my_list@8}"},
    {"t_my_tree", "struct tree {left *struct tree@0; right *struct tree@8; val long "
                  "long unsigned int@16}"},
};

static const char *const join_tests[][3] = {
    {"a", "b", "a/b"},
    {"a", "", "a"},
    {"", "b", "b"},
    {"/a", "b", "/a/b"},
    {"/a/", "b", "/a/b"},
    {"C:\\Windows\\", "System32", "C:\\Windows\\System32"},
    {"C:\\Windows\\", "", "C:\\Windows\\"},
    {"C:\\", "Windows", "C:\\Windows"},
    {"C:\\Windows\\", "C:System32", "C:\\Windows\\System32"},
    {"C:\\Windows", "a/b", "C:\\Windows\\a/b"},
    {"\\\\host\\share\\", "foo", "\\\\host\\share\\foo"},
    {"\\\\host\\share\\", "foo\\bar", "\\\\host\\share\\foo\\bar"},
    {"//host/share/", "foo/bar", "//host/share/foo/bar"},
    {"c:/workdir/go/src/x", "y.go", "c:/workdir/go/src/x/y.go"},
    {"d:/some/thing/", "b.go", "d:/some/thing/b.go"},
    {"e:\\blah\\", "foo.c", "e:\\blah\\foo.c"},
    {"C:", "a", "C:a"},
    {"C:", "a\\b", "C:a\\b"},
    {"C:.", "a", "C:.\\a"},
    {"C:a", "b", "C:a\\b"},
};

static const Byte issue51758_abbrev[] = {
    0x21, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x22, 0x5c, 0x6e, 0x20, 0x20, 0x20, 0x20, 0x69, 0x6e, 0x66, 0x6f, 0x3a,
    0x20, 0x5c, 0x22, 0x5c, 0x5c, 0x30, 0x30, 0x35, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c,
    0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30, 0x34,
    0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x2d, 0x5c, 0x5c, 0x30,
    0x30, 0x30, 0x5c, 0x22, 0x5c, 0x6e, 0x20, 0x20, 0x7d, 0x5c, 0x6e, 0x7d, 0x5c, 0x6e,
    0x22, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x3a, 0x20, 0x22,
    0x21, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x22, 0x0a, 0x20, 0x20, 0x20,
    0x20, 0x69, 0x6e, 0x66, 0x6f, 0x3a, 0x20, 0x22, 0x5c, 0x30, 0x30, 0x35, 0x5c, 0x30,
    0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x34,
    0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x2d, 0x5c, 0x30, 0x30, 0x30, 0x22,
    0x0a, 0x20, 0x20, 0x7d, 0x0a, 0x7d, 0x0a, 0x6c, 0x69, 0x73, 0x74, 0x20, 0x7b, 0x0a,
    0x7d, 0x0a, 0x6c, 0x69, 0x73, 0x74, 0x20, 0x7b, 0x0a, 0x7d, 0x0a, 0x6c, 0x69, 0x73,
    0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x4e, 0x65, 0x77, 0x20, 0x7b, 0x0a, 0x20, 0x20,
    0x20, 0x20, 0x61, 0x62, 0x62, 0x72, 0x65, 0x76, 0x3a, 0x20, 0x22, 0x5c, 0x30, 0x30,
    0x35, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x5c,
    0x30, 0x30, 0x34, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x30, 0x30, 0x30, 0x2d, 0x5c, 0x30,
    0x30, 0x30, 0x6c, 0x69, 0x73, 0x74, 0x20, 0x7b, 0x5c, 0x6e, 0x20, 0x20, 0x4e, 0x65,
    0x77, 0x20, 0x7b, 0x5c, 0x6e, 0x20, 0x20, 0x20, 0x20, 0x61, 0x62, 0x62, 0x72, 0x65,
    0x76, 0x3a, 0x20, 0x5c, 0x22, 0x21, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c,
    0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37,
    0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x22, 0x5c, 0x6e, 0x20, 0x20, 0x20, 0x20, 0x69, 0x6e, 0x66, 0x6f,
    0x3a, 0x20, 0x5c, 0x22, 0x5c, 0x5c, 0x30, 0x30, 0x35, 0x5c, 0x5c, 0x30, 0x30, 0x30,
    0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30,
    0x34, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x5c, 0x5c, 0x30, 0x30, 0x30, 0x2d, 0x5c, 0x5c,
    0x30, 0x30, 0x30, 0x5c, 0x22, 0x5c, 0x6e, 0x20, 0x20, 0x7d, 0x5c, 0x6e, 0x7d, 0x5c,
    0x6e, 0x22, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x3a, 0x20,
    0x22, 0x21, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37,
    0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33,
    0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0x5c, 0x33, 0x37, 0x37, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void test_typedefs(TestingT *t, const char *file, const char *const (*tests)[2],
                          size_t ntests) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, file);
    DwarfReader *r = dwarf_data_reader(d, a);
    bool seen[32] = {0};
    CHECK(ntests <= 32);
    for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
        if (e->tag == DWARF_TAG_TYPEDEF) {
            Error err = BURROW_NO_ERROR;
            DwarfType typ = dwarf_data_type(d, e->offset, &err);
            if (BURROW_FAILED(err) || typ == NULL)
                testing_t_fatalf_v(t, "d.Type: %s", error_text(err));
            CHECK(typ->type_kind == DWARF_TYPE_KIND_TYPEDEF);
            const DwarfTypedefType *t1 = (const DwarfTypedefType *)typ;
            Str typstr = t1->type->type_kind == DWARF_TYPE_KIND_STRUCT
                             ? dwarf_struct_type_defn((DwarfStructType *)t1->type, a)
                             : dwarf_type_string(t1->type, a);
            for (size_t i = 0; i < ntests; i++) {
                if (!str_eq(t1->common_type.name, str_from_cstr(tests[i][0])))
                    continue;
                if (seen[i])
                    testing_t_errorf_v(t, "multiple definitions for %s", tests[i][0]);
                seen[i] = true;
                if (!str_eq(typstr, str_from_cstr(tests[i][1])))
                    testing_t_errorf_v(t, "%s:\n\thave %s\n\twant %s", tests[i][0],
                                       typstr, tests[i][1]);
            }
        }
        if (e->tag != DWARF_TAG_COMPILE_UNIT)
            dwarf_reader_skip_children(r);
    }
    for (size_t i = 0; i < ntests; i++)
        if (!seen[i])
            testing_t_errorf_v(t, "missing %s", tests[i][0]);
    arena_free(&ar);
}

#define NTYPEDEF (sizeof typedef_tests / sizeof typedef_tests[0])

static void TestTypedefsELF(TestingT *t) {
    test_typedefs(t, "dwarf/typedef.elf", typedef_tests, NTYPEDEF);
}

static void TestTypedefsELFDwarf4(TestingT *t) {
    test_typedefs(t, "dwarf/typedef.elf4", typedef_tests, NTYPEDEF);
}

/* A bit field right after a zero length array used to make the array's
 * length -1. */
static void TestBitFieldZeroArrayIssue50685(TestingT *t) {
    static const char *const bitfield_tests[][2] = {
        {"t_another_struct",
         "struct another_struct {quix short unsigned int@0; xyz [0]int@4; "
         "x unsigned int@4 : 1@31; array [40]long long int@8}"},
    };
    test_typedefs(t, "dwarf/bitfields.elf4", bitfield_tests, 1);
}

/* Reading a typedef cycle from somewhere other than where its size has to be
 * worked out used to crash. Each type gets a fresh cache. */
static void TestTypedefCycle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, "dwarf/cycle.elf");
    DwarfReader *r = dwarf_data_reader(d, a);
    DwarfOffset offsets[256];
    size_t n = 0;
    for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
        switch (e->tag) {
        case DWARF_TAG_BASE_TYPE:
        case DWARF_TAG_TYPEDEF:
        case DWARF_TAG_POINTER_TYPE:
        case DWARF_TAG_STRUCT_TYPE:
            if (n < sizeof offsets / sizeof offsets[0])
                offsets[n++] = e->offset;
            break;
        default:
            break;
        }
    }
    CHECK(n > 0);
    for (size_t i = 0; i < n; i++) {
        Arena far;
        arena_init(&far, NULL, 0);
        Alloc *fa = arena_allocator(&far);
        DwarfData *fd = elf_data(t, fa, "dwarf/cycle.elf");
        Error err = BURROW_NO_ERROR;
        (void)dwarf_data_type(fd, offsets[i], &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "d.Type(%#x): %s", offsets[i], error_text(err));
        arena_free(&far);
    }
    arena_free(&ar);
}

/* C++ has type entries this cannot read yet, which come back as an
 * UnsupportedType. */
static void TestUnsupportedTypes(TestingT *t) {
    static const char *const want[] = {
        "culprit::(unsupported type ReferenceType):8",
        "pdm::(unsupported type PtrToMemberType):-1",
    };
    bool seen[2] = {false, false};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, "dwarf/cppunsuptypes.elf");
    DwarfReader *r = dwarf_data_reader(d, a);
    for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
        if (e->tag != DWARF_TAG_VARIABLE)
            continue;
        DwarfOffset *off = (DwarfOffset *)any_assert(
            dwarf_entry_val(e, DWARF_ATTR_TYPE), TYPE_DWARF_OFFSET);
        if (off == NULL) {
            testing_t_errorf_v(t, "variable at offset %d has no type", e->offset);
            continue;
        }
        Error err = BURROW_NO_ERROR;
        DwarfType typ = dwarf_data_type(d, *off, &err);
        if (BURROW_FAILED(err) || typ == NULL) {
            testing_t_errorf_v(t, "err in type decode: %s", error_text(err));
            continue;
        }
        if (typ->type_kind != DWARF_TYPE_KIND_UNSUPPORTED)
            continue;
        Str tag = fmt_sprintf_v(a, "%s:%s:%s:%d", entry_name(e), typ->name,
                                dwarf_type_string(typ, a), dwarf_type_size(typ));
        for (size_t i = 0; i < 2; i++)
            if (str_eq(tag, str_from_cstr(want[i])))
                seen[i] = true;
    }
    for (size_t i = 0; i < 2; i++)
        if (!seen[i])
            testing_t_errorf_v(t, "missing %s", want[i]);
    arena_free(&ar);
}

/* Bit field offsets: DWARF 2 and 3 give BitOffset and 4 and later
 * DataBitOffset. */
static void test_bit_offsets(TestingT *t, const char *file, const char *x,
                             const char *y) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, file);
    DwarfReader *r = dwarf_data_reader(d, a);
    for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
        if (e->tag == DWARF_TAG_STRUCT_TYPE) {
            Error err = BURROW_NO_ERROR;
            DwarfType typ = dwarf_data_type(d, e->offset, &err);
            if (BURROW_FAILED(err) || typ == NULL)
                testing_t_fatalf_v(t, "d.Type: %s", error_text(err));
            CHECK(typ->type_kind == DWARF_TYPE_KIND_STRUCT);
            const DwarfStructType *t1 = (const DwarfStructType *)typ;
            for (Int i = 0; i < t1->field.len; i++) {
                const DwarfStructField *f = ((DwarfStructField **)t1->field.p)[i];
                if (f->bit_size == 0)
                    continue;
                Str got = fmt_sprintf_v(a, "S:%d", f->bit_size);
                if (f->bit_offset != 0)
                    got = fmt_sprintf_v(a, "%s BO:%d", got, f->bit_offset);
                if (f->data_bit_offset != 0)
                    got = fmt_sprintf_v(a, "%s DBO:%d", got, f->data_bit_offset);
                const char *want = str_eq(f->name, BURROW_S("x"))   ? x
                                   : str_eq(f->name, BURROW_S("y")) ? y
                                                                    : "";
                if (!str_eq(got, str_from_cstr(want)))
                    testing_t_errorf_v(t, "%s: field %s in %s: got info %q want %q",
                                       file, f->name, t1->struct_name, got, want);
            }
        }
        if (e->tag != DWARF_TAG_COMPILE_UNIT)
            dwarf_reader_skip_children(r);
    }
    arena_free(&ar);
}

static void TestBitOffsetsELF(TestingT *t) {
    test_bit_offsets(t, "dwarf/typedef.elf", "S:1 BO:7", "S:4 BO:27");
}

static void TestBitOffsetsELFDwarf4(TestingT *t) {
    test_bit_offsets(t, "dwarf/typedef.elf4", "S:1 DBO:32", "S:4 DBO:33");
}

static void TestBitOffsetsELFDwarf5(TestingT *t) {
    test_bit_offsets(t, "dwarf/typedef.elf5", "S:1 DBO:32", "S:4 DBO:33");
}

/* --------------------------------------------------------------- entry_test */

/* Split DWARF is not read, but the attributes pointing at it must not stop
 * the rest from loading. */
static void TestSplit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, "dwarf/split.elf");
    DwarfEntry *e = must_next(t, dwarf_data_reader(d, a));
    CHECK(e != NULL);
    if (e == NULL)
        return;
    if (e->tag != DWARF_TAG_COMPILE_UNIT)
        testing_t_fatalf_v(t, "bad tag: have %s, want %s", dwarf_tag_string(e->tag, a),
                           dwarf_tag_string(DWARF_TAG_COMPILE_UNIT, a));
    DwarfField *f = dwarf_entry_attr_field(e, (DwarfAttr)0x2133);
    CHECK(f != NULL);
    if (f == NULL)
        return;
    if (f->val.t != TYPE_INT64)
        testing_t_errorf_v(t, "bad attribute value type: want int64");
    if (f->cls != DWARF_CLASS_UNKNOWN)
        testing_t_errorf_v(t, "bad class: have %s, want ClassUnknown",
                           dwarf_class_string(f->cls, a));
    arena_free(&ar);
}

/* A PC and the ranges of the unit that has it, or none. */
typedef struct WantRange {
    uint64_t pc;
    int n;
    uint64_t r[2][2];
} WantRange;

static void test_ranges(TestingT *t, const char *file, const WantRange *want,
                        size_t nwant) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, file);
    DwarfReader *r = dwarf_data_reader(d, a);
    for (size_t i = 0; i < nwant; i++) {
        const WantRange *w = &want[i];
        Error err = BURROW_NO_ERROR;
        DwarfEntry *e = dwarf_reader_seek_pc(r, w->pc, &err);
        if (BURROW_FAILED(err)) {
            if (w->n != 0)
                testing_t_errorf_v(t, "%s: missing Entry for %#x", file, w->pc);
            if (!errors_is(err, dwarf_err_unknown_pc))
                testing_t_errorf_v(t, "%s: expected ErrUnknownPC for %#x, got %s", file,
                                   w->pc, error_text(err));
            continue;
        }
        Slice rs = dwarf_data_ranges(d, a, e, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", file, error_text(err));
            continue;
        }
        bool same = rs.len == w->n;
        for (Int k = 0; same && k < rs.len; k++) {
            const DwarfPCRange *x = (const DwarfPCRange *)rs.p + k;
            same = x->v[0] == w->r[k][0] && x->v[1] == w->r[k][1];
        }
        if (!same)
            testing_t_errorf_v(t,
                               "%s: for %#x got %d ranges, expected %d as in Go's test",
                               file, w->pc, rs.len, w->n);
    }
    arena_free(&ar);
}

#define NWANT(w) (sizeof(w) / sizeof(w)[0])

static void TestReaderSeek(TestingT *t) {
    static const WantRange gcc[] = {
        {0x40059d, 1, {{0x40059d, 0x400601}}},
        {0x400600, 1, {{0x40059d, 0x400601}}},
        {0x400601, 1, {{0x400601, 0x400611}}},
        {0x4005f0, 1, {{0x40059d, 0x400601}}},
        {0x10, 0, {{0}}},
        {0x400611, 0, {{0}}},
    };
    test_ranges(t, "dwarf/line-gcc.elf", gcc, NWANT(gcc));
    static const WantRange gcc5[] = {
        {0x401122, 1, {{0x401122, 0x401166}}},
        {0x401165, 1, {{0x401122, 0x401166}}},
        {0x401166, 1, {{0x401166, 0x401179}}},
    };
    test_ranges(t, "dwarf/line-gcc-dwarf5.elf", gcc5, NWANT(gcc5));
    static const WantRange clang5[] = {
        {0x401130, 1, {{0x401130, 0x40117e}}},
        {0x40117d, 1, {{0x401130, 0x40117e}}},
        {0x40117e, 0, {{0}}},
    };
    test_ranges(t, "dwarf/line-clang-dwarf5.elf", clang5, NWANT(clang5));
    static const WantRange zstd[] = {
        {0x401126, 1, {{0x401126, 0x40116a}}},
        {0x40116a, 1, {{0x40116a, 0x401180}}},
    };
    test_ranges(t, "dwarf/line-gcc-zstd.elf", zstd, NWANT(zstd));
}

static void TestRangesSection(TestingT *t) {
    static const WantRange want[] = {
        {0x400500, 2, {{0x400500, 0x400549}, {0x400400, 0x400408}}},
        {0x400400, 2, {{0x400500, 0x400549}, {0x400400, 0x400408}}},
        {0x400548, 2, {{0x400500, 0x400549}, {0x400400, 0x400408}}},
        {0x400407, 2, {{0x400500, 0x400549}, {0x400400, 0x400408}}},
        {0x400408, 0, {{0}}},
        {0x400449, 0, {{0}}},
        {0x4003ff, 0, {{0}}},
    };
    test_ranges(t, "dwarf/ranges.elf", want, NWANT(want));
}

static void TestRangesRnglistx(TestingT *t) {
    static const WantRange want[] = {
        {0x401000, 2, {{0x401020, 0x40102c}, {0x401000, 0x40101d}}},
        {0x40101c, 2, {{0x401020, 0x40102c}, {0x401000, 0x40101d}}},
        {0x40101d, 0, {{0}}},
        {0x40101f, 0, {{0}}},
        {0x401020, 2, {{0x401020, 0x40102c}, {0x401000, 0x40101d}}},
        {0x40102b, 2, {{0x401020, 0x40102c}, {0x401000, 0x40101d}}},
        {0x40102c, 0, {{0}}},
    };
    test_ranges(t, "dwarf/rnglistx.elf", want, NWANT(want));
}

typedef struct Subprogram {
    const char *name;
    int n;
    uint64_t r[2];
} Subprogram;

static void TestReaderRanges(TestingT *t) {
    static const struct {
        const char *file;
        int n;
        Subprogram sp[4];
    } tests[] = {
        {"dwarf/line-gcc.elf",
         3,
         {{"f1", 1, {0x40059d, 0x4005e7}},
          {"main", 1, {0x4005e7, 0x400601}},
          {"f2", 1, {0x400601, 0x400611}}}},
        {"dwarf/line-gcc-dwarf5.elf",
         3,
         {{"main", 1, {0x401147, 0x401166}},
          {"f1", 1, {0x401122, 0x401147}},
          {"f2", 1, {0x401166, 0x401179}}}},
        {"dwarf/line-clang-dwarf5.elf",
         3,
         {{"main", 1, {0x401130, 0x401144}},
          {"f1", 1, {0x401150, 0x40117e}},
          {"f2", 1, {0x401180, 0x401197}}}},
        {"dwarf/line-gcc-zstd.elf",
         4,
         {{"f2", 0, {0, 0}},
          {"main", 1, {0x40114b, 0x40116a}},
          {"f1", 1, {0x401126, 0x40114b}},
          {"f2", 1, {0x40116a, 0x401180}}}},
    };
    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        DwarfData *d = elf_data(t, a, tests[k].file);
        DwarfReader *r = dwarf_data_reader(d, a);
        int i = 0;
        for (;;) {
            Error err = BURROW_NO_ERROR;
            DwarfEntry *e = dwarf_reader_next(r, &err);
            if (e == NULL || BURROW_FAILED(err))
                break;
            if (e->tag != DWARF_TAG_SUBPROGRAM)
                continue;
            if (i >= tests[k].n)
                testing_t_fatalf_v(t, "%s: too many subprograms (expected at most %d)",
                                   tests[k].file, tests[k].n);
            const Subprogram *sp = &tests[k].sp[i];
            if (!str_eq(entry_name(e), str_from_cstr(sp->name)))
                testing_t_errorf_v(t, "%s: subprogram %d name is %s, expected %s",
                                   tests[k].file, i, entry_name(e), sp->name);
            Slice rs = dwarf_data_ranges(d, a, e, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: subprogram %d: %s", tests[k].file, i,
                                   error_text(err));
                continue;
            }
            const DwarfPCRange *x = (const DwarfPCRange *)rs.p;
            if (rs.len != sp->n ||
                (sp->n == 1 && (x->v[0] != sp->r[0] || x->v[1] != sp->r[1])))
                testing_t_errorf_v(t, "%s: subprogram %d ranges are not Go's",
                                   tests[k].file, i);
            i++;
        }
        if (i < tests[k].n)
            testing_t_errorf_v(t, "%s: saw only %d subprograms, expected %d",
                               tests[k].file, i, tests[k].n);
        arena_free(&ar);
    }
}

/* 64-bit DWARF, written by hand since only XCOFF makes it. */
static void Test64Bit(TestingT *t) {
    static const Byte little32[] = {
        0x30, 0, 0, 0, 4, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    static const Byte little64[] = {
        0xff, 0xff, 0xff, 0xff, 0x30, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    8,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    static const Byte big64[] = {
        0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0x30, 0, 4, 0, 0, 0, 0, 0, 0,
        0,    0,    8,    0,    0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0,
    };
    static const struct {
        const char *name;
        const Byte *info;
        Int n;
        const char *order;
    } tests[] = {
        {"32-bit little", little32, sizeof little32, "LittleEndian"},
        {"64-bit little", little64, sizeof little64, "LittleEndian"},
        {"64-bit big", big64, sizeof big64, "BigEndian"},
    };
    static const Byte abbrev[] = {0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Slice ab = {(void *)(uintptr_t)abbrev, 1, 1, TYPE_BYTE};
        Slice info = {(void *)(uintptr_t)tests[i].info, tests[i].n, tests[i].n,
                      TYPE_BYTE};
        DwarfData *d = dwarf_new(a, ab, none, none, info, none, none, none, none, &err);
        if (BURROW_FAILED(err) || d == NULL) {
            testing_t_errorf_v(t, "%s: %s", tests[i].name, error_text(err));
            continue;
        }
        DwarfReader *r = dwarf_data_reader(d, a);
        if (dwarf_reader_address_size(r) != 8)
            testing_t_errorf_v(t, "%s: got address size %d, want 8", tests[i].name,
                               dwarf_reader_address_size(r));
        BinaryByteOrder bo = dwarf_reader_byte_order(r);
        Str order = bo.vt->string(bo.data);
        if (!str_eq(order, str_from_cstr(tests[i].order)))
            testing_t_errorf_v(t, "%s: got byte order %s, want %s", tests[i].name,
                               order, tests[i].order);
    }
    arena_free(&ar);
}

/* The same units whether their children are skipped or read. */
static void TestUnitIteration(TestingT *t) {
    for (size_t k = 0; k < sizeof debug_files / sizeof debug_files[0]; k++) {
        const char *file = debug_files[k].name;
        size_t len = strlen(file);
        if (strncmp(file, "dwarf/", 6) != 0 || len < 4 ||
            strcmp(file + len - 4, ".elf") != 0)
            continue;
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        DwarfData *d = elf_data(t, a, file);
        Str units[2][64];
        size_t n[2] = {0, 0};
        for (int method = 0; method < 2; method++) {
            DwarfReader *r = dwarf_data_reader(d, a);
            for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
                if (e->tag == DWARF_TAG_COMPILE_UNIT && n[method] < 64)
                    units[method][n[method]++] = entry_name(e);
                if (method == 0) {
                    if (e->tag != DWARF_TAG_COMPILE_UNIT)
                        testing_t_fatalf_v(t,
                                           "%s: found unexpected tag %s on top level",
                                           file, dwarf_tag_string(e->tag, a));
                    dwarf_reader_skip_children(r);
                }
            }
        }
        bool same = n[0] == n[1];
        for (size_t i = 0; same && i < n[0]; i++)
            same = str_eq(units[0][i], units[1][i]);
        if (!same)
            testing_t_errorf_v(t, "%s: set of CUs differ", file);
        arena_free(&ar);
    }
}

/* Malformed input, cut at every length, has to give an error and no Data. */
static void TestIssue51758(TestingT *t) {
    static const Byte aranges[] = {0x2c};
    static const Byte info[] = {0x5, 0x0, 0x0, 0x0, 0x4, 0x0, 0x0, 0x2d, 0x0,
                                0x5, 0x0, 0x0, 0x0, 0x4, 0x0, 0x0, 0x2d, 0x0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Int na = (Int)sizeof issue51758_abbrev;
    Slice ab = {(void *)(uintptr_t)issue51758_abbrev, na, na, TYPE_BYTE};
    Slice ar_ = {(void *)(uintptr_t)aranges, 1, 1, TYPE_BYTE};
    for (Int i = 0; i <= (Int)sizeof info; i++) {
        Slice truncated = {(void *)(uintptr_t)info, i, i, TYPE_BYTE};
        Error err = BURROW_NO_ERROR;
        DwarfData *dw =
            dwarf_new(a, ab, ar_, none, truncated, none, none, none, none, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "cut at %d: expected error", i);
        else if (dw != NULL)
            testing_t_errorf_v(t, "cut at %d: got non-nil dw, wanted nil", i);
    }
    arena_free(&ar);
}

/* A unit header with no entries and an empty abbreviation table. */
static void TestIssue52045(TestingT *t) {
    static const Byte abbrev[] = {0};
    static const Byte info[] = {0x7, 0x0, 0x0, 0x0, 0x2, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Slice ab = {(void *)(uintptr_t)abbrev, 1, 1, TYPE_BYTE};
    Slice in = {(void *)(uintptr_t)info, (Int)sizeof info, (Int)sizeof info, TYPE_BYTE};
    Error err = BURROW_NO_ERROR;
    DwarfData *d = dwarf_new(a, ab, none, none, in, none, none, none, none, &err);
    if (BURROW_FAILED(err) || d == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    err = BURROW_NO_ERROR;
    DwarfEntry *e = dwarf_reader_seek_pc(dwarf_data_reader(d, a), 0, &err);
    if (e != NULL)
        testing_t_errorf_v(t, "got non-nil entry0, wanted nil");
    arena_free(&ar);
}

/* Seeking straight to a subprogram has to read its low PC the way reading
 * through to it does. */
static void TestIssue57046(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, "dwarf/issue57046-clang.elf5");
    DwarfReader *r = dwarf_data_reader(d, a);
    DwarfOffset spdies[64];
    uint64_t lopcs[64];
    size_t n = 0;
    for (DwarfEntry *e; (e = must_next(t, r)) != NULL;) {
        if (e->tag != DWARF_TAG_SUBPROGRAM)
            continue;
        uint64_t *lo =
            (uint64_t *)any_assert(dwarf_entry_val(e, DWARF_ATTR_LOWPC), TYPE_UINT64);
        if (entry_name(e).len == 0 || lo == NULL || *lo == 0 || n == 64)
            continue;
        spdies[n] = e->offset;
        lopcs[n++] = *lo;
    }
    if (n < 2)
        testing_t_fatalf_v(t, "found %d subprograms, want at least 2", (Int)n);
    DwarfReader *r2 = dwarf_data_reader(d, a);
    dwarf_reader_seek(r2, spdies[1]);
    DwarfEntry *e = must_next(t, r2);
    if (e == NULL)
        testing_t_fatalf_v(t, "r2.Next() returned nil");
    uint64_t *got =
        (uint64_t *)any_assert(dwarf_entry_val(e, DWARF_ATTR_LOWPC), TYPE_UINT64);
    if (got == NULL || *got != lopcs[1])
        testing_t_errorf_v(t, "bad lopc for fn2 following seek: want %x", lopcs[1]);
    arena_free(&ar);
}

/* ---------------------------------------------------------------- line_test */

static bool line_eq(const DwarfLineEntry *x, const DwarfLineEntry *y) {
    return x->address == y->address && x->op_index == y->op_index &&
           x->file == y->file && x->line == y->line && x->column == y->column &&
           x->is_stmt == y->is_stmt && x->basic_block == y->basic_block &&
           x->prologue_end == y->prologue_end &&
           x->epilogue_begin == y->epilogue_begin && x->isa == y->isa &&
           x->discriminator == y->discriminator && x->end_sequence == y->end_sequence;
}

static void TestLineSeek(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DwarfData *d = elf_data(t, a, "dwarf/line-gcc.elf");
    DwarfEntry *cu = must_next(t, dwarf_data_reader(d, a));
    CHECK(cu != NULL);
    if (cu == NULL)
        return;
    Error err = BURROW_NO_ERROR;
    DwarfLineReader *lr = dwarf_data_line_reader(d, a, cu, &err);
    if (BURROW_FAILED(err) || lr == NULL)
        testing_t_fatalf_v(t, "d.LineReader: %s", error_text(err));

    enum { MAX = 256 };
    DwarfLineReaderPos pos[MAX];
    DwarfLineEntry table[MAX];
    DwarfLineEntry line;
    size_t n = 0, npos = 0;
    for (;;) {
        if (npos == MAX)
            testing_t_fatalf_v(t, "more than %d rows", MAX);
        pos[npos++] = dwarf_line_reader_tell(lr);
        err = dwarf_line_reader_next(lr, &line);
        if (BURROW_FAILED(err)) {
            if (errors_is(err, io_eof))
                break;
            testing_t_fatalf_v(t, "lr.Next: %s", error_text(err));
        }
        table[n++] = line;
    }
    CHECK(n > 1);

    dwarf_line_reader_reset(lr);
    err = dwarf_line_reader_next(lr, &line);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "lr.Next after Reset failed: %s", error_text(err));
    if (!line_eq(&line, &table[0]))
        testing_t_fatalf_v(t, "lr.Next after Reset did not return the first row");

    for (size_t i = npos; i-- > 0;) {
        dwarf_line_reader_seek(lr, pos[i]);
        err = dwarf_line_reader_next(lr, &line);
        if (i == npos - 1) {
            if (!errors_is(err, io_eof))
                testing_t_fatalf_v(t, "expected io.EOF after seek to end, got %s",
                                   error_text(err));
        } else if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "lr.Next after seek to row %d failed: %s", (Int)i,
                               error_text(err));
        } else if (!line_eq(&line, &table[i])) {
            testing_t_fatalf_v(t, "lr.Next after seek to row %d returned another row",
                               (Int)i);
        }
    }

    err = dwarf_line_reader_seek_pc(lr, table[0].address - 1, &line);
    if (!errors_is(err, dwarf_err_unknown_pc))
        testing_t_fatalf_v(t, "lr.SeekPC to %#x returned %s instead of ErrUnknownPC",
                           table[0].address - 1, error_text(err));
    for (size_t i = 0; i < n; i++) {
        if (table[i].end_sequence) {
            err = dwarf_line_reader_seek_pc(lr, table[i].address, &line);
            if (!errors_is(err, dwarf_err_unknown_pc))
                testing_t_fatalf_v(
                    t, "lr.SeekPC to %#x returned %s instead of ErrUnknownPC",
                    table[i].address, error_text(err));
            continue;
        }
        for (uint64_t pc = table[i].address; pc < table[i + 1].address; pc++) {
            err = dwarf_line_reader_seek_pc(lr, pc, &line);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "lr.SeekPC to %#x failed: %s", pc,
                                   error_text(err));
            if (!line_eq(&line, &table[i]))
                testing_t_fatalf_v(t, "lr.SeekPC to %#x returned another row", pc);
        }
    }
    arena_free(&ar);
}

static void TestPathJoin(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof join_tests / sizeof join_tests[0]; i++) {
        Str got = burrow__dwarf_path_join(a, str_from_cstr(join_tests[i][0]),
                                          str_from_cstr(join_tests[i][1]));
        if (!str_eq(got, str_from_cstr(join_tests[i][2])))
            testing_t_errorf_v(t, "pathJoin(%q, %q) = %q, want %q", join_tests[i][0],
                               join_tests[i][1], got, join_tests[i][2]);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestTypedefsELF)                                                                 \
    X(TestTypedefsELFDwarf4)                                                           \
    X(TestBitFieldZeroArrayIssue50685)                                                 \
    X(TestTypedefCycle)                                                                \
    X(TestUnsupportedTypes)                                                            \
    X(TestBitOffsetsELF)                                                               \
    X(TestBitOffsetsELFDwarf4)                                                         \
    X(TestBitOffsetsELFDwarf5)                                                         \
    X(TestSplit)                                                                       \
    X(TestReaderSeek)                                                                  \
    X(TestRangesSection)                                                               \
    X(TestRangesRnglistx)                                                              \
    X(TestReaderRanges)                                                                \
    X(Test64Bit)                                                                       \
    X(TestUnitIteration)                                                               \
    X(TestIssue51758)                                                                  \
    X(TestIssue52045)                                                                  \
    X(TestIssue57046)                                                                  \
    X(TestLineSeek)                                                                    \
    X(TestPathJoin)

TESTING_MAIN(TESTS)
