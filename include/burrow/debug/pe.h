/* debug/pe, reading PE files.
 *
 * PE, the Portable Executable format, is what executables, DLLs and object
 * files are on Windows, and what UEFI programs are too. This reads the file
 * header, the optional header with its data directories, the sections and
 * their relocations, the COFF symbol table and string table, the DWARF that
 * GCC and Clang put in the debug sections, and the symbols the file imports,
 * from anything that is an IoReaderAt.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     PeFile *f = pe_open(a, BURROW_S("hello.exe"), &err);
 *     if (f == NULL)
 *         ...
 *     for (Int i = 0; i < f->sections.len; i++) {
 *         PeSection *s = ((PeSection **)f->sections.p)[i];
 *         ...
 *     }
 *     Slice imports = pe_file_imported_symbols(f, a, &err);
 *     pe_file_free(f);
 *     arena_free(&ar);
 *
 * A PeFile keeps everything it reads at the start in an arena of its own on
 * the allocator it was made with, and pe_file_free gives all of it back. What
 * a function returns, such as a section's contents, comes from the allocator
 * that function is given, and can be freed separately or with an arena.
 *
 * Like Go's, this checks only enough to read a file safely, not that the file
 * makes sense, and a malformed file can make it allocate a lot.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/pe */

#ifndef BURROW_DEBUG_PE_H
#define BURROW_DEBUG_PE_H

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/error.h"
#include "burrow/iface.h"
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

/* ---------------------------------------------------------------- headers */

/* pe.FileHeader, the COFF file header. */
typedef struct PeFileHeader {
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} PeFileHeader;

/* pe.DataDirectory, where one of the tables in the optional header is. */
typedef struct PeDataDirectory {
    uint32_t virtual_address;
    uint32_t size;
} PeDataDirectory;

/* pe.OptionalHeader32, the optional header of a PE32 file. */
typedef struct PeOptionalHeader32 {
    uint16_t magic;
    uint8_t major_linker_version;
    uint8_t minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;
    uint32_t base_of_data;
    uint32_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_operating_system_version;
    uint16_t minor_operating_system_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t check_sum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint32_t size_of_stack_reserve;
    uint32_t size_of_stack_commit;
    uint32_t size_of_heap_reserve;
    uint32_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    PeDataDirectory data_directory[16];
} PeOptionalHeader32;
extern const Type burrow_type_PeOptionalHeader32;
#define TYPE_PE_OPTIONAL_HEADER32 TYPE_OF(PeOptionalHeader32)

/* pe.OptionalHeader64, the optional header of a PE32+ file. */
typedef struct PeOptionalHeader64 {
    uint16_t magic;
    uint8_t major_linker_version;
    uint8_t minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_operating_system_version;
    uint16_t minor_operating_system_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t check_sum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    PeDataDirectory data_directory[16];
} PeOptionalHeader64;
extern const Type burrow_type_PeOptionalHeader64;
#define TYPE_PE_OPTIONAL_HEADER64 TYPE_OF(PeOptionalHeader64)

/* FileHeader.Machine values. */
#define PE_IMAGE_FILE_MACHINE_UNKNOWN 0x0
#define PE_IMAGE_FILE_MACHINE_AM33 0x1d3
#define PE_IMAGE_FILE_MACHINE_AMD64 0x8664
#define PE_IMAGE_FILE_MACHINE_ARM 0x1c0
#define PE_IMAGE_FILE_MACHINE_ARMNT 0x1c4
#define PE_IMAGE_FILE_MACHINE_ARM64 0xaa64
#define PE_IMAGE_FILE_MACHINE_EBC 0xebc
#define PE_IMAGE_FILE_MACHINE_I386 0x14c
#define PE_IMAGE_FILE_MACHINE_IA64 0x200
#define PE_IMAGE_FILE_MACHINE_LOONGARCH32 0x6232
#define PE_IMAGE_FILE_MACHINE_LOONGARCH64 0x6264
#define PE_IMAGE_FILE_MACHINE_M32R 0x9041
#define PE_IMAGE_FILE_MACHINE_MIPS16 0x266
#define PE_IMAGE_FILE_MACHINE_MIPSFPU 0x366
#define PE_IMAGE_FILE_MACHINE_MIPSFPU16 0x466
#define PE_IMAGE_FILE_MACHINE_POWERPC 0x1f0
#define PE_IMAGE_FILE_MACHINE_POWERPCFP 0x1f1
#define PE_IMAGE_FILE_MACHINE_R4000 0x166
#define PE_IMAGE_FILE_MACHINE_SH3 0x1a2
#define PE_IMAGE_FILE_MACHINE_SH3DSP 0x1a3
#define PE_IMAGE_FILE_MACHINE_SH4 0x1a6
#define PE_IMAGE_FILE_MACHINE_SH5 0x1a8
#define PE_IMAGE_FILE_MACHINE_THUMB 0x1c2
#define PE_IMAGE_FILE_MACHINE_WCEMIPSV2 0x169
#define PE_IMAGE_FILE_MACHINE_RISCV32 0x5032
#define PE_IMAGE_FILE_MACHINE_RISCV64 0x5064
#define PE_IMAGE_FILE_MACHINE_RISCV128 0x5128

/* Indexes into the data_directory of an optional header. */
#define PE_IMAGE_DIRECTORY_ENTRY_EXPORT 0
#define PE_IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define PE_IMAGE_DIRECTORY_ENTRY_RESOURCE 2
#define PE_IMAGE_DIRECTORY_ENTRY_EXCEPTION 3
#define PE_IMAGE_DIRECTORY_ENTRY_SECURITY 4
#define PE_IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define PE_IMAGE_DIRECTORY_ENTRY_DEBUG 6
#define PE_IMAGE_DIRECTORY_ENTRY_ARCHITECTURE 7
#define PE_IMAGE_DIRECTORY_ENTRY_GLOBALPTR 8
#define PE_IMAGE_DIRECTORY_ENTRY_TLS 9
#define PE_IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG 10
#define PE_IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT 11
#define PE_IMAGE_DIRECTORY_ENTRY_IAT 12
#define PE_IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define PE_IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14

/* FileHeader.Characteristics flags, which can be combined. */
#define PE_IMAGE_FILE_RELOCS_STRIPPED 0x0001
#define PE_IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define PE_IMAGE_FILE_LINE_NUMS_STRIPPED 0x0004
#define PE_IMAGE_FILE_LOCAL_SYMS_STRIPPED 0x0008
#define PE_IMAGE_FILE_AGGRESIVE_WS_TRIM 0x0010
#define PE_IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define PE_IMAGE_FILE_BYTES_REVERSED_LO 0x0080
#define PE_IMAGE_FILE_32BIT_MACHINE 0x0100
#define PE_IMAGE_FILE_DEBUG_STRIPPED 0x0200
#define PE_IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP 0x0400
#define PE_IMAGE_FILE_NET_RUN_FROM_SWAP 0x0800
#define PE_IMAGE_FILE_SYSTEM 0x1000
#define PE_IMAGE_FILE_DLL 0x2000
#define PE_IMAGE_FILE_UP_SYSTEM_ONLY 0x4000
#define PE_IMAGE_FILE_BYTES_REVERSED_HI 0x8000

/* The subsystem of an optional header. */
#define PE_IMAGE_SUBSYSTEM_UNKNOWN 0
#define PE_IMAGE_SUBSYSTEM_NATIVE 1
#define PE_IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define PE_IMAGE_SUBSYSTEM_WINDOWS_CUI 3
#define PE_IMAGE_SUBSYSTEM_OS2_CUI 5
#define PE_IMAGE_SUBSYSTEM_POSIX_CUI 7
#define PE_IMAGE_SUBSYSTEM_NATIVE_WINDOWS 8
#define PE_IMAGE_SUBSYSTEM_WINDOWS_CE_GUI 9
#define PE_IMAGE_SUBSYSTEM_EFI_APPLICATION 10
#define PE_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER 11
#define PE_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER 12
#define PE_IMAGE_SUBSYSTEM_EFI_ROM 13
#define PE_IMAGE_SUBSYSTEM_XBOX 14
#define PE_IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION 16

/* The dll_characteristics flags of an optional header, which can be
 * combined. */
#define PE_IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA 0x0020
#define PE_IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE 0x0040
#define PE_IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY 0x0080
#define PE_IMAGE_DLLCHARACTERISTICS_NX_COMPAT 0x0100
#define PE_IMAGE_DLLCHARACTERISTICS_NO_ISOLATION 0x0200
#define PE_IMAGE_DLLCHARACTERISTICS_NO_SEH 0x0400
#define PE_IMAGE_DLLCHARACTERISTICS_NO_BIND 0x0800
#define PE_IMAGE_DLLCHARACTERISTICS_APPCONTAINER 0x1000
#define PE_IMAGE_DLLCHARACTERISTICS_WDM_DRIVER 0x2000
#define PE_IMAGE_DLLCHARACTERISTICS_GUARD_CF 0x4000
#define PE_IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE 0x8000

/* ------------------------------------------------------- the string table */

/* pe.StringTable, the COFF string table, without the four bytes of length it
 * starts with in the file. */
typedef Slice PeStringTable;

/* StringTable.String: the NUL-terminated string at start in st, where start
 * counts the four bytes of length the way the offsets in the file do. The
 * Str points into st. */
BURROW_BORROWS(ret, st) Str pe_string_table_string(PeStringTable st, uint32_t start,
                                                   Error *err);

/* ---------------------------------------------------------------- symbols */

/* The size of a COFF symbol table record in the file. */
#define PE_COFF_SYMBOL_SIZE 18

/* pe.COFFSymbol, one record of the COFF symbol table as it is in the file.
 * The auxiliary records after a symbol are COFFSymbols too, and hold what Go
 * gets by reading a COFFSymbolAuxFormat5 over the record: the first 15 bytes
 * of it, with storage_class, number_of_aux_symbols and the top byte of type
 * zero. Go does that on every machine, so on a big-endian one its fields come
 * out byte-swapped; here they are always what a little-endian machine gets. */
typedef struct PeCOFFSymbol {
    Byte name[8];
    uint32_t value;
    int16_t section_number;
    uint16_t type;
    uint8_t storage_class;
    uint8_t number_of_aux_symbols;
} PeCOFFSymbol;

/* COFFSymbol.FullName: the symbol's name, which is in name when it fits and
 * in st when it does not. The Str points into sym or st. */
BURROW_BORROWS(ret, sym, st) Str pe_coff_symbol_full_name(const PeCOFFSymbol *sym,
                                                          PeStringTable st, Error *err);

/* pe.Symbol, a COFFSymbol with its name looked up and without the auxiliary
 * records. */
typedef struct PeSymbol {
    Str name;
    uint32_t value;
    int16_t section_number;
    uint16_t type;
    uint8_t storage_class;
} PeSymbol;

/* pe.COFFSymbolAuxFormat5, the auxiliary record of a section definition
 * symbol, which says how big the section is and, in an object file, how the
 * linker picks between COMDAT copies of it. */
typedef struct PeCOFFSymbolAuxFormat5 {
    uint32_t size;
    uint16_t num_relocs;
    uint16_t num_line_numbers;
    uint32_t checksum;
    uint16_t sec_num;
    uint8_t selection;
    Byte unused_[3];
} PeCOFFSymbolAuxFormat5;

/* COFFSymbolAuxFormat5.Selection values. */
#define PE_IMAGE_COMDAT_SELECT_NODUPLICATES 1
#define PE_IMAGE_COMDAT_SELECT_ANY 2
#define PE_IMAGE_COMDAT_SELECT_SAME_SIZE 3
#define PE_IMAGE_COMDAT_SELECT_EXACT_MATCH 4
#define PE_IMAGE_COMDAT_SELECT_ASSOCIATIVE 5
#define PE_IMAGE_COMDAT_SELECT_LARGEST 6

/* --------------------------------------------------------------- sections */

/* pe.SectionHeader32, a section header as it is in the file. */
typedef struct PeSectionHeader32 {
    Byte name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_line_numbers;
    uint16_t number_of_relocations;
    uint16_t number_of_line_numbers;
    uint32_t characteristics;
} PeSectionHeader32;

/* pe.Reloc, one COFF relocation of a section. */
typedef struct PeReloc {
    uint32_t virtual_address;
    uint32_t symbol_table_index;
    uint16_t type;
} PeReloc;

/* pe.SectionHeader, a SectionHeader32 with its name looked up. size and
 * offset are SizeOfRawData and PointerToRawData. */
typedef struct PeSectionHeader {
    Str name;
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size;
    uint32_t offset;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_line_numbers;
    uint16_t number_of_relocations;
    uint16_t number_of_line_numbers;
    uint32_t characteristics;
} PeSectionHeader;

/* pe.Section, one section. relocs holds PeReloc and reader_at reads the
 * section's bytes in the file. A section whose offset is 0 has no bytes in
 * the file, and every read of it fails. The rest is the File's. */
typedef struct PeSection {
    PeSectionHeader section_header;
    Slice relocs;
    IoReaderAt reader_at;

    IoSectionReader sr;
} PeSection;

/* Section.Data: the section's size bytes, in a. */
BURROW_OWNS(ret) Slice pe_section_data(PeSection *s, Alloc *a, Error *err);

/* Section.Open: a reader of the section's bytes with its own offset. The box
 * comes from a and pe_reader_free gives it back. */
BURROW_OWNS(ret) IoReadSeeker pe_section_open(PeSection *s, Alloc *a);

/* Section.ReadAt, through reader_at. */
Int pe_section_read_at(PeSection *s, Slice p, int64_t off, Error *err);

/* Gives back a reader pe_section_open made. */
void pe_reader_free(IoReadSeeker r);

/* Section characteristics flags. */
#define PE_IMAGE_SCN_CNT_CODE 0x00000020
#define PE_IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define PE_IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define PE_IMAGE_SCN_LNK_COMDAT 0x00001000
#define PE_IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define PE_IMAGE_SCN_MEM_EXECUTE 0x20000000
#define PE_IMAGE_SCN_MEM_READ 0x40000000
#define PE_IMAGE_SCN_MEM_WRITE 0x80000000U

/* ------------------------------------------------------------------ files */

/* pe.File, an open PE file. optional_header is the optional header, with t
 * TYPE_PE_OPTIONAL_HEADER32 or TYPE_PE_OPTIONAL_HEADER64 and data pointing at
 * it, or a nil Any when the file has none, which object files do not.
 * sections holds a PeSection * for each section, symbols a PeSymbol * for
 * each symbol, and coff_symbols a PeCOFFSymbol for each record of the symbol
 * table, auxiliary records included. The rest is the File's. */
typedef struct PeFile {
    PeFileHeader file_header;
    Any optional_header;
    Slice sections;
    Slice symbols;
    Slice coff_symbols;
    PeStringTable string_table;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
    PeCOFFSymbolAuxFormat5 *aux;
} PeFile;

/* pe.NewFile: reads the PE file at the start of r, which has to stay readable
 * while the result is used. A file that is not PE, or a malformed one, gives
 * NULL and the error. */
BURROW_OWNS(ret) PeFile *pe_new_file(Alloc *a, IoReaderAt r, Error *err);

/* pe.Open: opens the file at name and reads it as pe_new_file does. The
 * PeFile keeps the file open until pe_file_close or pe_file_free. */
BURROW_OWNS(ret) PeFile *pe_open(Alloc *a, Str name, Error *err);

/* File.Close: closes the file pe_open opened, and does nothing for a File
 * from pe_new_file. f can still be freed afterwards. */
BURROW_STATIC(ret) Error pe_file_close(PeFile *f);

/* Closes f as pe_file_close does and gives back everything it holds. NULL is
 * fine. */
void pe_file_free(PeFile *f);

/* File.Section: the first section called name, or NULL. */
BURROW_BORROWS(ret, f) PeSection *pe_file_section(PeFile *f, Str name);

/* File.DWARF: the DWARF in the .debug_ and .zdebug_ sections, whose contents
 * it reads into a. */
BURROW_OWNS(ret) DwarfData *pe_file_dwarf(PeFile *f, Alloc *a, Error *err);

/* File.ImportedSymbols: the names of the symbols the file expects DLLs to
 * supply when it is loaded, as Str "name:dll" in a, or a nil slice when
 * there are none. Symbols imported by ordinal are left out. */
BURROW_OWNS(ret) Slice pe_file_imported_symbols(PeFile *f, Alloc *a, Error *err);

/* File.ImportedLibraries, which Go has never filled in and which always
 * gives a nil slice and no error. */
BURROW_OWNS(ret) Slice pe_file_imported_libraries(PeFile *f, Alloc *a, Error *err);

/* File.COFFSymbolReadSectionDefAux: the auxiliary record after the section
 * definition symbol at idx in coff_symbols, as a COFFSymbolAuxFormat5. It
 * fails for an idx out of range, for a symbol that is not static, and for one
 * with no auxiliary record. The record is the File's. */
BURROW_BORROWS(ret, f) PeCOFFSymbolAuxFormat5 *
pe_file_coff_symbol_read_section_def_aux(PeFile *f, Int idx, Error *err);

/* pe.ImportDirectory, an entry of the import directory table. */
typedef struct PeImportDirectory {
    uint32_t original_first_thunk;
    uint32_t time_date_stamp;
    uint32_t forwarder_chain;
    uint32_t name;
    uint32_t first_thunk;
} PeImportDirectory;

/* pe.FormatError, which Go no longer uses and keeps only so that old code
 * still compiles. ISO C has no empty structs, so it has a byte in it. */
typedef struct PeFormatError {
    Byte unused_;
} PeFormatError;
extern const Type burrow_type_PeFormatError;
#define TYPE_PE_FORMAT_ERROR TYPE_OF(PeFormatError)

/* FormatError.Error, which is always "unknown error". */
BURROW_STATIC(ret) Str pe_format_error_error(const PeFormatError *e);

#ifdef __cplusplus
}
#endif

#endif
