/* debug/elf, reading ELF object files.
 *
 * ELF is the format of executables, shared libraries, object files and core
 * dumps on Linux, the BSDs and most other Unix systems. This reads the file
 * header, the section and program headers, the symbol tables, the dynamic
 * section and its version tables, and the DWARF in the debug sections, from
 * anything that is an IoReaderAt.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     ElfFile *f = elf_open(a, BURROW_S("/bin/ls"), &err);
 *     if (f == NULL)
 *         ...
 *     for (Int i = 0; i < f->sections.len; i++) {
 *         ElfSection *s = ((ElfSection **)f->sections.p)[i];
 *         ...
 *     }
 *     Slice syms = elf_file_dynamic_symbols(f, a, &err);
 *     elf_file_free(f);
 *     arena_free(&ar);
 *
 * An ElfFile keeps what it reads at the start, and the version tables it reads
 * later, in an arena of its own on the allocator it was made with, and
 * elf_file_free gives all of it back. What a method returns, such as the
 * symbols or a section's contents, comes from the allocator that method is
 * given, and can be freed separately or with an arena.
 *
 * Like Go's, this checks only enough to read a file safely, not that the file
 * makes sense, and a malformed file can make it allocate a lot.
 *
 * The constants and the named integer types are in burrow/debug/elf_const.h,
 * which this includes.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/elf */

#ifndef BURROW_DEBUG_ELF_H
#define BURROW_DEBUG_ELF_H

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/declare.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#include "burrow/debug/elf_const.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ file formats */

/* elf.Header32, the ELF32 file header as it is in the file. */
typedef struct ElfHeader32 {
    uint8_t ident[ELF_EI_NIDENT]; /* File identification. */
    uint16_t type;                /* File type. */
    uint16_t machine;             /* Machine architecture. */
    uint32_t version;             /* ELF format version. */
    uint32_t entry;               /* Entry point. */
    uint32_t phoff;               /* Program header file offset. */
    uint32_t shoff;               /* Section header file offset. */
    uint32_t flags;               /* Architecture-specific flags. */
    uint16_t ehsize;              /* Size of ELF header in bytes. */
    uint16_t phentsize;           /* Size of program header entry. */
    uint16_t phnum;               /* Number of program header entries. */
    uint16_t shentsize;           /* Size of section header entry. */
    uint16_t shnum;               /* Number of section header entries. */
    uint16_t shstrndx;            /* Section name strings section. */
} ElfHeader32;

/* elf.Section32, an ELF32 section header. */
typedef struct ElfSection32 {
    uint32_t name;      /* Section name (index into the section header string table). */
    uint32_t type;      /* Section type. */
    uint32_t flags;     /* Section flags. */
    uint32_t addr;      /* Address in memory image. */
    uint32_t off;       /* Offset in file. */
    uint32_t size;      /* Size in bytes. */
    uint32_t link;      /* Index of a related section. */
    uint32_t info;      /* Depends on section type. */
    uint32_t addralign; /* Alignment in bytes. */
    uint32_t entsize;   /* Size of each entry in section. */
} ElfSection32;

/* elf.Prog32, an ELF32 program header. */
typedef struct ElfProg32 {
    uint32_t type;   /* Entry type. */
    uint32_t off;    /* File offset of contents. */
    uint32_t vaddr;  /* Virtual address in memory image. */
    uint32_t paddr;  /* Physical address (not used). */
    uint32_t filesz; /* Size of contents in file. */
    uint32_t memsz;  /* Size of contents in memory. */
    uint32_t flags;  /* Access permission flags. */
    uint32_t align;  /* Alignment in memory and file. */
} ElfProg32;

/* elf.Dyn32, an ELF32 dynamic structure. The ".dynamic" section contains an
 * array of them. */
typedef struct ElfDyn32 {
    int32_t tag;  /* Entry type. */
    uint32_t val; /* Integer/Address value. */
} ElfDyn32;

/* elf.Chdr32, the ELF32 compression header. */
typedef struct ElfChdr32 {
    uint32_t type;
    uint32_t size;
    uint32_t addralign;
} ElfChdr32;

/* elf.Rel32, an ELF32 relocation entry that needs no addend. */
typedef struct ElfRel32 {
    uint32_t off;  /* Location to be relocated. */
    uint32_t info; /* Relocation type and symbol index. */
} ElfRel32;

/* elf.Rela32, an ELF32 relocation entry with an addend. */
typedef struct ElfRela32 {
    uint32_t off;   /* Location to be relocated. */
    uint32_t info;  /* Relocation type and symbol index. */
    int32_t addend; /* Addend. */
} ElfRela32;

/* R_SYM32, R_TYPE32 and R_INFO32, the symbol index and the type in the info
 * of an ELF32 relocation and the info they make. */
uint32_t elf_r_sym32(uint32_t info);
uint32_t elf_r_type32(uint32_t info);
uint32_t elf_r_info32(uint32_t sym, uint32_t typ);

/* elf.Sym32, an ELF32 symbol table entry. */
typedef struct ElfSym32 {
    uint32_t name;
    uint32_t value;
    uint32_t size;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
} ElfSym32;

/* ST_BIND, ST_TYPE and ST_VISIBILITY, the parts of a symbol's info and
 * other, and ST_INFO, the info a binding and a type make. */
ElfSymBind elf_st_bind(uint8_t info);
ElfSymType elf_st_type(uint8_t info);
uint8_t elf_st_info(ElfSymBind bind, ElfSymType typ);
ElfSymVis elf_st_visibility(uint8_t other);

/* elf.Header64, the ELF64 file header as it is in the file. */
typedef struct ElfHeader64 {
    uint8_t ident[ELF_EI_NIDENT]; /* File identification. */
    uint16_t type;                /* File type. */
    uint16_t machine;             /* Machine architecture. */
    uint32_t version;             /* ELF format version. */
    uint64_t entry;               /* Entry point. */
    uint64_t phoff;               /* Program header file offset. */
    uint64_t shoff;               /* Section header file offset. */
    uint32_t flags;               /* Architecture-specific flags. */
    uint16_t ehsize;              /* Size of ELF header in bytes. */
    uint16_t phentsize;           /* Size of program header entry. */
    uint16_t phnum;               /* Number of program header entries. */
    uint16_t shentsize;           /* Size of section header entry. */
    uint16_t shnum;               /* Number of section header entries. */
    uint16_t shstrndx;            /* Section name strings section. */
} ElfHeader64;

/* elf.Section64, an ELF64 section header. */
typedef struct ElfSection64 {
    uint32_t name;      /* Section name (index into the section header string table). */
    uint32_t type;      /* Section type. */
    uint64_t flags;     /* Section flags. */
    uint64_t addr;      /* Address in memory image. */
    uint64_t off;       /* Offset in file. */
    uint64_t size;      /* Size in bytes. */
    uint32_t link;      /* Index of a related section. */
    uint32_t info;      /* Depends on section type. */
    uint64_t addralign; /* Alignment in bytes. */
    uint64_t entsize;   /* Size of each entry in section. */
} ElfSection64;

/* elf.Prog64, an ELF64 program header. */
typedef struct ElfProg64 {
    uint32_t type;   /* Entry type. */
    uint32_t flags;  /* Access permission flags. */
    uint64_t off;    /* File offset of contents. */
    uint64_t vaddr;  /* Virtual address in memory image. */
    uint64_t paddr;  /* Physical address (not used). */
    uint64_t filesz; /* Size of contents in file. */
    uint64_t memsz;  /* Size of contents in memory. */
    uint64_t align;  /* Alignment in memory and file. */
} ElfProg64;

/* elf.Dyn64, an ELF64 dynamic structure. */
typedef struct ElfDyn64 {
    int64_t tag;  /* Entry type. */
    uint64_t val; /* Integer/address value. */
} ElfDyn64;

/* elf.Chdr64, the ELF64 compression header. */
typedef struct ElfChdr64 {
    uint32_t type;
    uint32_t reserved; /* Go's _ field, which keeps the layout the file's */
    uint64_t size;
    uint64_t addralign;
} ElfChdr64;

/* elf.Rel64, an ELF64 relocation entry that needs no addend. */
typedef struct ElfRel64 {
    uint64_t off;  /* Location to be relocated. */
    uint64_t info; /* Relocation type and symbol index. */
} ElfRel64;

/* elf.Rela64, an ELF64 relocation entry with an addend. */
typedef struct ElfRela64 {
    uint64_t off;   /* Location to be relocated. */
    uint64_t info;  /* Relocation type and symbol index. */
    int64_t addend; /* Addend. */
} ElfRela64;

/* R_SYM64, R_TYPE64 and R_INFO, the same three for ELF64. */
uint32_t elf_r_sym64(uint64_t info);
uint32_t elf_r_type64(uint64_t info);
uint64_t elf_r_info(uint32_t sym, uint32_t typ);

/* elf.Sym64, an ELF64 symbol table entry. */
typedef struct ElfSym64 {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} ElfSym64;

/* ------------------------------------------------------------------ errors */

/* elf.FormatError, what a malformed file gives: what was wrong, the value
 * that was, when there is one, and where. Go keeps the value as an any and
 * formats it when Error is called. Here val is that text already, made with
 * %v when the error was, and has_val says whether there was a value. */
typedef struct ElfFormatError {
    int64_t off;
    Str msg;
    Str val;
    bool has_val;
} ElfFormatError;

extern const Type burrow_type_ElfFormatError;
#define TYPE_ELF_FORMAT_ERROR TYPE_OF(ElfFormatError)

/* FormatError.Error: "unknown ELF class 'ELFCLASSNONE' in record at byte 0x0",
 * in a. */
BURROW_OWNS(ret) Str elf_format_error_error(const ElfFormatError *e, Alloc *a);

/* elf.ErrNoSymbols, which elf_file_symbols and elf_file_dynamic_symbols give
 * for a file with no such section. */
extern const Error elf_err_no_symbols;

/* ------------------------------------------------------------------- files */

/* elf.FileHeader, the parts of the file header the File keeps. Go's field
 * Class is cls here, since class is a keyword in C++. */
typedef struct ElfFileHeader {
    ElfClass cls;
    ElfData data;
    ElfVersion version;
    ElfOSABI osabi;
    uint8_t abi_version;
    BinaryByteOrder byte_order;
    ElfType type;
    ElfMachine machine;
    uint64_t entry;
} ElfFileHeader;

/* elf.SectionHeader. */
typedef struct ElfSectionHeader {
    Str name;
    ElfSectionType type;
    ElfSectionFlag flags;
    uint64_t addr;
    uint64_t offset;
    /* The size of the contents, which for a compressed section is the size
     * after decompressing them. */
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
    /* The size of the section in the file, which for a compressed section is
     * the size of the compressed contents. */
    uint64_t file_size;
} ElfSectionHeader;

/* elf.Section, one section of the file.
 *
 * reader_at reads the contents as they are in the file, and is a nil
 * interface for a compressed section, whose contents need elf_section_open or
 * elf_section_data to read them decompressed. The rest is the File's. */
typedef struct ElfSection {
    ElfSectionHeader section_header;
    IoReaderAt reader_at;

    IoSectionReader sr;
    ElfCompressionType compression_type;
    int64_t compression_offset;
} ElfSection;

/* elf.ProgHeader. */
typedef struct ElfProgHeader {
    ElfProgType type;
    ElfProgFlag flags;
    uint64_t off;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} ElfProgHeader;

/* elf.Prog, one program header of the file and the segment it describes.
 * reader_at reads the segment's bytes in the file. */
typedef struct ElfProg {
    ElfProgHeader prog_header;
    IoReaderAt reader_at;

    IoSectionReader sr;
} ElfProg;

/* elf.VersionIndex, a symbol's index into the version tables. */
typedef uint16_t ElfVersionIndex;
extern const Type burrow_type_ElfVersionIndex;
#define TYPE_ELF_VERSION_INDEX TYPE_OF(ElfVersionIndex)

/* VersionIndex.IsHidden: whether the symbol can only be seen by asking for
 * this exact version. */
bool elf_version_index_is_hidden(ElfVersionIndex vi);

/* VersionIndex.Index: the index without the hidden bit. 0 is a local symbol,
 * 1 is the base version, and anything else is the index of an
 * ElfDynamicVersion or an ElfDynamicVersionDep. */
uint16_t elf_version_index_index(ElfVersionIndex vi);

/* elf.Symbol, one entry of a symbol table. has_version, version_index,
 * version and library are filled in only by elf_file_dynamic_symbols, and
 * only for a file with version tables. */
typedef struct ElfSymbol {
    Str name;
    uint8_t info;
    uint8_t other;
    bool has_version;
    ElfVersionIndex version_index;
    ElfSectionIndex section;
    uint64_t value;
    uint64_t size;
    Str version;
    Str library;
} ElfSymbol;
extern const Type burrow_type_ElfSymbol;
#define TYPE_ELF_SYMBOL TYPE_OF(ElfSymbol)

/* elf.ImportedSymbol, a symbol the file expects another library to supply. */
typedef struct ElfImportedSymbol {
    Str name;
    Str version;
    Str library;
} ElfImportedSymbol;
extern const Type burrow_type_ElfImportedSymbol;
#define TYPE_ELF_IMPORTED_SYMBOL TYPE_OF(ElfImportedSymbol)

/* The []string of ElfDynamicVersion's deps. */
BURROW_SLICE_TYPE_DECL(ElfStrSlice, Str);

/* elf.DynamicVersion, a version the file defines, from SHT_GNU_verdef. deps
 * holds the Str names of the versions it depends on. */
typedef struct ElfDynamicVersion {
    Str name;
    uint16_t index;
    ElfDynamicVersionFlag flags;
    ElfStrSlice deps;
} ElfDynamicVersion;
extern const Type burrow_type_ElfDynamicVersion;
#define TYPE_ELF_DYNAMIC_VERSION TYPE_OF(ElfDynamicVersion)

/* elf.DynamicVersionDep, a version the file needs from a library. */
typedef struct ElfDynamicVersionDep {
    ElfDynamicVersionFlag flags;
    uint16_t index;
    Str dep;
} ElfDynamicVersionDep;
extern const Type burrow_type_ElfDynamicVersionDep;
#define TYPE_ELF_DYNAMIC_VERSION_DEP TYPE_OF(ElfDynamicVersionDep)

/* The []DynamicVersionDep of ElfDynamicVersionNeed's needs. */
BURROW_SLICE_TYPE_DECL(ElfDynamicVersionDepSlice, ElfDynamicVersionDep);

/* elf.DynamicVersionNeed, a library the file needs, from SHT_GNU_verneed,
 * and the versions of it that it needs. */
typedef struct ElfDynamicVersionNeed {
    Str name;
    ElfDynamicVersionDepSlice needs;
} ElfDynamicVersionNeed;
extern const Type burrow_type_ElfDynamicVersionNeed;
#define TYPE_ELF_DYNAMIC_VERSION_NEED TYPE_OF(ElfDynamicVersionNeed)

/* elf.File, an open ELF file. sections and progs hold ElfSection * and
 * ElfProg * in the order the file has them. The rest is the File's. */
typedef struct ElfFile {
    ElfFileHeader file_header;
    Slice sections;
    Slice progs;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
    bool have_versym;
    Slice dyn_vers;
    Slice dyn_ver_needs;
    Slice gnu_versym;
} ElfFile;

/* elf.NewFile: reads the headers of the ELF file at the start of r, which
 * has to stay readable while the result is used. A file that is not ELF, or a
 * malformed one, gives NULL and the error, which for a malformed one is
 * usually an ElfFormatError. */
BURROW_OWNS(ret) ElfFile *elf_new_file(Alloc *a, IoReaderAt r, Error *err);

/* elf.Open: opens the file at name and reads it as elf_new_file does. The
 * ElfFile keeps the file open until elf_file_close or elf_file_free. */
BURROW_OWNS(ret) ElfFile *elf_open(Alloc *a, Str name, Error *err);

/* File.Close: closes the file elf_open opened, and does nothing for a File
 * from elf_new_file. f can still be freed afterwards. */
BURROW_STATIC(ret) Error elf_file_close(ElfFile *f);

/* Closes f as elf_file_close does and gives back everything it holds. NULL
 * is fine. */
void elf_file_free(ElfFile *f);

/* File.SectionByType: the first section of the type, or NULL. */
BURROW_BORROWS(ret, f) ElfSection *elf_file_section_by_type(ElfFile *f,
                                                            ElfSectionType typ);

/* File.Section: the section called name, or NULL. */
BURROW_BORROWS(ret, f) ElfSection *elf_file_section(ElfFile *f, Str name);

/* Section.Data: the contents of the section, decompressed if they are
 * compressed, in a. An SHT_NOBITS section always gives an error. */
BURROW_OWNS(ret) Slice elf_section_data(ElfSection *s, Alloc *a, Error *err);

/* Section.Open: a reader of the contents of the section, decompressed if they
 * are compressed. Every read from an SHT_NOBITS section fails. The reader
 * comes from a and elf_reader_free gives it back. Seeking backwards in a
 * compressed section starts decompressing again from the beginning. */
BURROW_OWNS(ret) IoReadSeeker elf_section_open(ElfSection *s, Alloc *a);

/* Section.ReadAt, which reads the contents as they are in the file through
 * reader_at, and panics as a nil interface does in Go for a compressed
 * section. */
Int elf_section_read_at(ElfSection *s, Slice p, int64_t off, Error *err);

/* Prog.Open: a reader of the segment, from a. */
BURROW_OWNS(ret) IoReadSeeker elf_prog_open(ElfProg *p, Alloc *a);

/* Prog.ReadAt, through reader_at. */
Int elf_prog_read_at(ElfProg *p, Slice b, int64_t off, Error *err);

/* Gives back a reader elf_section_open or elf_prog_open made, and the
 * decompressor inside it if there is one, to the allocator it came from. A nil
 * reader is fine. */
void elf_reader_free(IoReadSeeker r);

/* File.DWARF: the DWARF in the file's debug sections, which may be
 * compressed. In an object file that is not an executable, the relocations
 * for the sections are applied first, for the machines Go applies them for.
 * The section contents come from a and the DwarfData borrows them, so the two
 * live and die together, in an arena most easily. */
BURROW_OWNS(ret) DwarfData *elf_file_dwarf(ElfFile *f, Alloc *a, Error *err);

/* File.Symbols: the symbol table, as ElfSymbol in a, in the order the file has
 * them and without the null symbol at index 0, so that index x in the file
 * is element x-1 here. The names point into a copy of the string table that
 * is in a too. elf_err_no_symbols if there is no symbol table. */
BURROW_OWNS(ret) Slice elf_file_symbols(ElfFile *f, Alloc *a, Error *err);

/* File.DynamicSymbols: the dynamic symbol table in the same way, with the
 * version fields filled in when the file has version tables. The version and
 * library names are the File's, as for elf_file_dynamic_versions. */
BURROW_OWNS(ret) Slice elf_file_dynamic_symbols(ElfFile *f, Alloc *a, Error *err);

/* File.ImportedSymbols: the global symbols the file leaves undefined for
 * other libraries to supply, as ElfImportedSymbol in a. Weak symbols are not
 * in it. The version and library names are the File's. */
BURROW_OWNS(ret) Slice elf_file_imported_symbols(ElfFile *f, Alloc *a, Error *err);

/* File.ImportedLibraries: the DT_NEEDED libraries, as Str in a. */
BURROW_OWNS(ret) Slice elf_file_imported_libraries(ElfFile *f, Alloc *a, Error *err);

/* File.DynString: the strings in the dynamic section for tag, which has to be
 * DT_NEEDED, DT_SONAME, DT_RPATH or DT_RUNPATH, as Str in a. A file with no
 * dynamic section gives the nil slice and no error. */
BURROW_OWNS(ret) Slice elf_file_dyn_string(ElfFile *f, Alloc *a, ElfDynTag tag,
                                           Error *err);

/* File.DynValue: the values in the dynamic section for tag, as uint64_t in
 * a. */
BURROW_OWNS(ret) Slice elf_file_dyn_value(ElfFile *f, Alloc *a, ElfDynTag tag,
                                          Error *err);

/* File.DynamicVersions: the versions the file defines, as ElfDynamicVersion.
 * The slice and what it holds are the File's, and stay until it is freed. */
BURROW_BORROWS(ret, f) Slice elf_file_dynamic_versions(ElfFile *f, Error *err);

/* File.DynamicVersionNeeds: the libraries the file needs versions of, as
 * ElfDynamicVersionNeed, the File's in the same way. */
BURROW_BORROWS(ret, f) Slice elf_file_dynamic_version_needs(ElfFile *f, Error *err);

#ifdef __cplusplus
}
#endif

#endif
