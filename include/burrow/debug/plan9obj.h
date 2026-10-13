/* debug/plan9obj, reading Plan 9 a.out files.
 *
 * Plan 9's executables are a.out files: a header of eight big-endian words,
 * then the text, the initialised data, the symbol table, and two tables that
 * map program counters to stack sizes and to line numbers. This reads the
 * header, gives each of those parts as a section, and decodes the symbol
 * table, from anything that is an IoReaderAt.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     Plan9objFile *f = plan9obj_open(a, BURROW_S("8.out"), &err);
 *     if (f == NULL)
 *         ...
 *     Slice syms = plan9obj_file_symbols(f, a, &err);
 *     for (Int i = 0; i < syms.len; i++) {
 *         Plan9objSym *s = &((Plan9objSym *)syms.p)[i];
 *         ...
 *     }
 *     plan9obj_file_free(f);
 *     arena_free(&ar);
 *
 * A Plan9objFile keeps what it reads at the start in an arena of its own on
 * the allocator it was made with, and plan9obj_file_free gives all of it back.
 * What a function returns, such as a section's contents or the symbols, comes
 * from the allocator that function is given.
 *
 * Like Go's, this checks only enough to read a file safely, not that the file
 * makes sense, and a malformed file can make it allocate a lot.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/plan9obj */

#ifndef BURROW_DEBUG_PLAN9OBJ_H
#define BURROW_DEBUG_PLAN9OBJ_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The magic numbers. MAGIC64 is the bit that says the header has a 64-bit
 * entry point after it. */
#define PLAN9OBJ_MAGIC64 0x8000
#define PLAN9OBJ_MAGIC386 ((4 * 11 + 0) * 11 + 7)
#define PLAN9OBJ_MAGIC_AMD64 ((4 * 26 + 0) * 26 + 7 + PLAN9OBJ_MAGIC64)
#define PLAN9OBJ_MAGIC_ARM ((4 * 20 + 0) * 20 + 7)

/* plan9obj.FileHeader. ptr_size is 4, or 8 for a 64-bit file, and hdr_size is
 * the size of the header, 32 or 40 bytes. */
typedef struct Plan9objFileHeader {
    uint32_t magic;
    uint32_t bss;
    uint64_t entry;
    Int ptr_size;
    uint64_t load_address;
    uint64_t hdr_size;
} Plan9objFileHeader;

/* plan9obj.SectionHeader. The file has no section headers; these are made up
 * from the sizes in its header. */
typedef struct Plan9objSectionHeader {
    Str name;
    uint32_t size;
    uint32_t offset;
} Plan9objSectionHeader;

/* plan9obj.Section, one of the five parts of the file: "text", "data",
 * "syms", "spsz" and "pcsz". reader_at reads the section's bytes in the file.
 * The rest is the File's. */
typedef struct Plan9objSection {
    Plan9objSectionHeader section_header;
    IoReaderAt reader_at;

    IoSectionReader sr;
} Plan9objSection;

/* Section.Data: the section's size bytes, in a. */
BURROW_OWNS(ret) Slice plan9obj_section_data(Plan9objSection *s, Alloc *a, Error *err);

/* Section.Open: a reader of the section's bytes with its own offset. The box
 * comes from a and plan9obj_reader_free gives it back. */
BURROW_OWNS(ret) IoReadSeeker plan9obj_section_open(Plan9objSection *s, Alloc *a);

/* Section.ReadAt, through reader_at. */
Int plan9obj_section_read_at(Plan9objSection *s, Slice p, int64_t off, Error *err);

/* Gives back a reader plan9obj_section_open made. */
void plan9obj_reader_free(IoReadSeeker r);

/* plan9obj.Sym, an entry of the symbol table. type is the symbol's type
 * letter, such as 'T' for text or 'f' for a file name. The name of a 'z' or
 * 'Z' symbol is a path, put together from the 'f' symbols before it. */
typedef struct Plan9objSym {
    uint64_t value;
    Rune type;
    Str name;
} Plan9objSym;
extern const Type burrow_type_Plan9objSym;
#define TYPE_PLAN9OBJ_SYM TYPE_OF(Plan9objSym)

/* plan9obj.File, an open Plan 9 a.out file. sections holds a
 * Plan9objSection * for each section, always five of them. The rest is the
 * File's. */
typedef struct Plan9objFile {
    Plan9objFileHeader file_header;
    Slice sections;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
} Plan9objFile;

/* plan9obj.ErrNoSymbols, which plan9obj_file_symbols gives for a file with no
 * symbol section. Every file has one, so Go never gives it either. */
extern const Error plan9obj_err_no_symbols;

/* plan9obj.NewFile: reads the Plan 9 a.out file at the start of r, which has
 * to stay readable while the result is used. A file with the wrong magic
 * number, or too short a header, gives NULL and the error. */
BURROW_OWNS(ret) Plan9objFile *plan9obj_new_file(Alloc *a, IoReaderAt r, Error *err);

/* plan9obj.Open: opens the file at name and reads it as plan9obj_new_file
 * does. The Plan9objFile keeps the file open until plan9obj_file_close or
 * plan9obj_file_free. */
BURROW_OWNS(ret) Plan9objFile *plan9obj_open(Alloc *a, Str name, Error *err);

/* File.Close: closes the file plan9obj_open opened, and does nothing for a
 * File from plan9obj_new_file. f can still be freed afterwards. */
BURROW_STATIC(ret) Error plan9obj_file_close(Plan9objFile *f);

/* Closes f as plan9obj_file_close does and gives back everything it holds.
 * NULL is fine. */
void plan9obj_file_free(Plan9objFile *f);

/* File.Symbols: the symbol table, a Plan9objSym for each entry, in a. The
 * names are in a too. */
BURROW_OWNS(ret) Slice plan9obj_file_symbols(Plan9objFile *f, Alloc *a, Error *err);

/* File.Section: the section called name, or NULL. */
BURROW_BORROWS(ret, f) Plan9objSection *plan9obj_file_section(Plan9objFile *f,
                                                              Str name);

#ifdef __cplusplus
}
#endif

#endif
