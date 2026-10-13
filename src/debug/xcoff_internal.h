/* internal/xcoff, reading the XCOFF files of AIX.
 *
 * Go keeps this out of the public library, and so does this: debug/buildinfo
 * reads AIX executables with it and nothing else is meant to. It reads the
 * file header, the sections and their relocations, the symbol table and the
 * imports in the loader section of a 32-bit or 64-bit XCOFF file, and the
 * members of an AIX big archive.
 *
 * An XcoffFile keeps what it reads at the start in an arena of its own on the
 * allocator it was made with, and burrow__xcoff_file_free gives all of it
 * back. What a function returns comes from the allocator that function is
 * given. Like Go's, this checks only enough to read a file safely, and the
 * places where Go's would panic on a malformed file panic here too.
 *
 * Derived from Go's src/internal/xcoff. Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_DEBUG_XCOFF_INTERNAL_H
#define BURROW_SRC_DEBUG_XCOFF_INTERNAL_H

/* burrow:package internal/xcoff */

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

/* The sizes of the two file headers. */
#define XCOFF_FILHSZ_32 20
#define XCOFF_FILHSZ_64 24

/* The magic numbers, which say whether the file is 32-bit or 64-bit. */
#define XCOFF_U802TOCMAGIC 0737
#define XCOFF_U64_TOCMAGIC 0767

/* The flags of the file header. */
#define XCOFF_F_RELFLG 0x0001
#define XCOFF_F_EXEC 0x0002
#define XCOFF_F_LNNO 0x0004
#define XCOFF_F_FDPR_PROF 0x0010
#define XCOFF_F_FDPR_OPTI 0x0020
#define XCOFF_F_DSA 0x0040
#define XCOFF_F_VARPG 0x0100
#define XCOFF_F_DYNLOAD 0x1000
#define XCOFF_F_SHROBJ 0x2000
#define XCOFF_F_LOADONLY 0x4000

/* Section types, the low 16 bits of a section's flags. */
#define XCOFF_STYP_DWARF 0x0010
#define XCOFF_STYP_TEXT 0x0020
#define XCOFF_STYP_DATA 0x0040
#define XCOFF_STYP_BSS 0x0080
#define XCOFF_STYP_EXCEPT 0x0100
#define XCOFF_STYP_INFO 0x0200
#define XCOFF_STYP_TDATA 0x0400
#define XCOFF_STYP_TBSS 0x0800
#define XCOFF_STYP_LOADER 0x1000
#define XCOFF_STYP_DEBUG 0x2000
#define XCOFF_STYP_TYPCHK 0x4000
#define XCOFF_STYP_OVRFLO 0x8000

/* The kinds of DWARF section, in the high 16 bits of the flags. */
#define XCOFF_SSUBTYP_DWINFO 0x10000
#define XCOFF_SSUBTYP_DWLINE 0x20000
#define XCOFF_SSUBTYP_DWPBNMS 0x30000
#define XCOFF_SSUBTYP_DWPBTYP 0x40000
#define XCOFF_SSUBTYP_DWARNGE 0x50000
#define XCOFF_SSUBTYP_DWABREV 0x60000
#define XCOFF_SSUBTYP_DWSTR 0x70000
#define XCOFF_SSUBTYP_DWRNGES 0x80000
#define XCOFF_SSUBTYP_DWLOC 0x90000
#define XCOFF_SSUBTYP_DWFRAME 0xA0000
#define XCOFF_SSUBTYP_DWMAC 0xB0000

/* The size of a symbol table entry, auxiliary ones included. */
#define XCOFF_SYMESZ 18

/* Special section numbers, and the bits of a symbol's type. */
#define XCOFF_N_DEBUG (-2)
#define XCOFF_N_ABS (-1)
#define XCOFF_N_UNDEF 0
#define XCOFF_SYM_V_INTERNAL 0x1000
#define XCOFF_SYM_V_HIDDEN 0x2000
#define XCOFF_SYM_V_PROTECTED 0x3000
#define XCOFF_SYM_V_EXPORTED 0x4000
#define XCOFF_SYM_TYPE_FUNC 0x0020

/* Storage classes. */
#define XCOFF_C_NULL 0
#define XCOFF_C_EXT 2
#define XCOFF_C_STAT 3
#define XCOFF_C_BLOCK 100
#define XCOFF_C_FCN 101
#define XCOFF_C_FILE 103
#define XCOFF_C_HIDEXT 107
#define XCOFF_C_BINCL 108
#define XCOFF_C_EINCL 109
#define XCOFF_C_WEAKEXT 111
#define XCOFF_C_DWARF 112
#define XCOFF_C_GSYM 128
#define XCOFF_C_LSYM 129
#define XCOFF_C_PSYM 130
#define XCOFF_C_RSYM 131
#define XCOFF_C_RPSYM 132
#define XCOFF_C_STSYM 133
#define XCOFF_C_BCOMM 135
#define XCOFF_C_ECOML 136
#define XCOFF_C_ECOMM 137
#define XCOFF_C_DECL 140
#define XCOFF_C_ENTRY 141
#define XCOFF_C_FUN 142
#define XCOFF_C_BSTAT 143
#define XCOFF_C_ESTAT 144
#define XCOFF_C_GTLS 145
#define XCOFF_C_STTLS 146

/* Symbol types in a csect auxiliary entry. */
#define XCOFF_XTY_ER 0
#define XCOFF_XTY_SD 1
#define XCOFF_XTY_LD 2
#define XCOFF_XTY_CM 3

/* Storage mapping classes in a csect auxiliary entry. */
#define XCOFF_XMC_PR 0
#define XCOFF_XMC_RO 1
#define XCOFF_XMC_DB 2
#define XCOFF_XMC_TC 3
#define XCOFF_XMC_UA 4
#define XCOFF_XMC_RW 5
#define XCOFF_XMC_GL 6
#define XCOFF_XMC_XO 7
#define XCOFF_XMC_SV 8
#define XCOFF_XMC_BS 9
#define XCOFF_XMC_DS 10
#define XCOFF_XMC_UC 11
#define XCOFF_XMC_TC0 15
#define XCOFF_XMC_TD 16
#define XCOFF_XMC_SV64 17
#define XCOFF_XMC_SV3264 18
#define XCOFF_XMC_TL 20
#define XCOFF_XMC_UL 21
#define XCOFF_XMC_TE 22

/* The sizes of the two loader section headers. */
#define XCOFF_LDHDRSZ_32 32
#define XCOFF_LDHDRSZ_64 56

/* The magic strings of an AIX archive and of each member's header. Only the
 * big format is read. */
#define XCOFF_SAIAMAG 8
#define XCOFF_AIAFMAG "`\n"
#define XCOFF_AIAMAG "<aiaff>\n"
#define XCOFF_AIAMAGBIG "<bigaf>\n"
#define XCOFF_FL_HSZ_BIG 0x80
#define XCOFF_AR_HSZ_BIG 0x70

/* xcoff.SectionHeader. */
typedef struct XcoffSectionHeader {
    Str name;
    uint64_t virtual_address;
    uint64_t size;
    uint32_t type;
    uint64_t relptr;
    uint32_t nreloc;
} XcoffSectionHeader;

typedef struct XcoffSymbol XcoffSymbol;

/* xcoff.Reloc. symbol is the symbol the relocation names, or NULL when that
 * symbol was not one the file kept. */
typedef struct XcoffReloc {
    uint64_t virtual_address;
    XcoffSymbol *symbol;
    bool signed_;
    bool instruction_fixed;
    uint8_t length;
    uint8_t type;
} XcoffReloc;
extern const Type burrow_type_XcoffReloc;
#define TYPE_XCOFF_RELOC TYPE_OF(XcoffReloc)

/* xcoff.Section. relocs holds an XcoffReloc for each relocation of a .text
 * or .data section. reader_at reads the section's bytes in the file. */
typedef struct XcoffSection {
    XcoffSectionHeader section_header;
    Slice relocs;
    IoReaderAt reader_at;

    IoSectionReader sr;
} XcoffSection;

/* xcoff.AuxiliaryCSect, from a symbol's csect auxiliary entry. */
typedef struct XcoffAuxiliaryCSect {
    int64_t length;
    Int storage_mapping_class;
    Int symbol_type;
} XcoffAuxiliaryCSect;

/* xcoff.AuxiliaryFcn, from a function symbol's auxiliary entry. */
typedef struct XcoffAuxiliaryFcn {
    int64_t size;
} XcoffAuxiliaryFcn;

/* xcoff.Symbol. value is relative to the start of the symbol's section. */
struct XcoffSymbol {
    Str name;
    uint64_t value;
    Int section_number;
    Int storage_class;
    XcoffAuxiliaryFcn aux_fcn;
    XcoffAuxiliaryCSect aux_csect;
};

/* xcoff.ImportedSymbol. */
typedef struct XcoffImportedSymbol {
    Str name;
    Str library;
} XcoffImportedSymbol;
extern const Type burrow_type_XcoffImportedSymbol;
#define TYPE_XCOFF_IMPORTED_SYMBOL TYPE_OF(XcoffImportedSymbol)

/* xcoff.FileHeader. */
typedef struct XcoffFileHeader {
    uint16_t target_machine;
} XcoffFileHeader;

/* xcoff.File. sections holds an XcoffSection * for each section and symbols
 * an XcoffSymbol * for each symbol the file keeps, the external ones with a
 * csect entry. library_paths holds Strs and is set by
 * burrow__xcoff_file_imported_symbols and
 * burrow__xcoff_file_imported_libraries. The rest is the File's. */
typedef struct XcoffFile {
    XcoffFileHeader file_header;
    Slice sections;
    Slice symbols;
    Slice string_table;
    Slice library_paths;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
} XcoffFile;

/* xcoff.NewFile: reads the XCOFF file at the start of r, which has to stay
 * readable while the result is used. */
BURROW_OWNS(ret) XcoffFile *burrow__xcoff_new_file(Alloc *a, IoReaderAt r, Error *err);

/* xcoff.Open: opens the file at name and reads it as burrow__xcoff_new_file
 * does, keeping it open until the File is closed or freed. */
BURROW_OWNS(ret) XcoffFile *burrow__xcoff_open(Alloc *a, Str name, Error *err);

/* File.Close: closes the file burrow__xcoff_open opened, and does nothing for
 * a File from burrow__xcoff_new_file. */
BURROW_STATIC(ret) Error burrow__xcoff_file_close(XcoffFile *f);

/* Closes f and gives back everything it holds. NULL is fine. */
void burrow__xcoff_file_free(XcoffFile *f);

/* File.Section: the first section called name, or NULL. Section names are
 * cut to 8 bytes in the file, so a longer name finds the section its first 8
 * bytes name. */
BURROW_BORROWS(ret, f) XcoffSection *burrow__xcoff_file_section(XcoffFile *f, Str name);

/* File.SectionByType: the first section of type typ, or NULL. */
BURROW_BORROWS(ret, f) XcoffSection *burrow__xcoff_file_section_by_type(XcoffFile *f,
                                                                        uint32_t typ);

/* Section.Data: the section's bytes, in a. A section the file does not have
 * all of gives what there is and the error. */
BURROW_OWNS(ret) Slice burrow__xcoff_section_data(XcoffSection *s, Alloc *a,
                                                  Error *err);

/* File.CSect: the contents of the csect called name, in a, or a nil slice.
 * *found says whether there was a result, since one can be empty. */
BURROW_OWNS(ret) Slice burrow__xcoff_file_csect(XcoffFile *f, Alloc *a, Str name,
                                                bool *found);

/* File.DWARF: the DWARF data in the file's DWARF sections. The section
 * contents are read into a and the result keeps them. */
BURROW_OWNS(ret) DwarfData *burrow__xcoff_file_dwarf(XcoffFile *f, Alloc *a,
                                                     Error *err);

/* File.ImportedSymbols: an XcoffImportedSymbol for each symbol the loader
 * section says comes from another library, in a. A file with no loader
 * section gives a nil slice and no error. */
BURROW_OWNS(ret) Slice burrow__xcoff_file_imported_symbols(XcoffFile *f, Alloc *a,
                                                           Error *err);

/* File.ImportedLibraries: the libraries the loader section names, as Strs in
 * a. */
BURROW_OWNS(ret) Slice burrow__xcoff_file_imported_libraries(XcoffFile *f, Alloc *a,
                                                             Error *err);

/* xcoff.ArchiveHeader. */
typedef struct XcoffArchiveHeader {
    Str magic;
} XcoffArchiveHeader;

/* xcoff.MemberHeader. */
typedef struct XcoffMemberHeader {
    Str name;
    uint64_t size;
} XcoffMemberHeader;

/* xcoff.Member, one file in an archive. */
typedef struct XcoffMember {
    XcoffMemberHeader member_header;

    IoSectionReader sr;
} XcoffMember;

/* xcoff.Archive. members holds an XcoffMember * for each member. The rest is
 * the Archive's. */
typedef struct XcoffArchive {
    XcoffArchiveHeader archive_header;
    Slice members;

    IoSectionReader sr;
    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
} XcoffArchive;

/* xcoff.NewArchive: reads the AIX big archive at the start of r, which has to
 * stay readable while the result is used. */
BURROW_OWNS(ret) XcoffArchive *burrow__xcoff_new_archive(Alloc *a, IoReaderAt r,
                                                         Error *err);

/* xcoff.OpenArchive: opens the file at name and reads it as
 * burrow__xcoff_new_archive does. */
BURROW_OWNS(ret) XcoffArchive *burrow__xcoff_open_archive(Alloc *a, Str name,
                                                          Error *err);

/* Archive.Close. */
BURROW_STATIC(ret) Error burrow__xcoff_archive_close(XcoffArchive *arch);

/* Closes arch and gives back everything it holds. NULL is fine. Files from
 * burrow__xcoff_archive_get_file have to be freed first. */
void burrow__xcoff_archive_free(XcoffArchive *arch);

/* Archive.GetFile: reads the first member called name as an XCOFF file, from
 * a. */
BURROW_OWNS(ret) XcoffFile *burrow__xcoff_archive_get_file(XcoffArchive *arch, Alloc *a,
                                                           Str name, Error *err);

#endif
