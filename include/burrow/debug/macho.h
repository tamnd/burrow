/* debug/macho, reading Mach-O object files.
 *
 * Mach-O is the format of executables, libraries and object files on macOS,
 * iOS and the other Apple systems. This reads the file header, the load
 * commands with the segments and sections in them, the symbol tables and the
 * DWARF in the debug sections, from anything that is an IoReaderAt. It reads
 * universal binaries, which hold one Mach-O file for each architecture, too.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     MachoFile *f = macho_open(a, BURROW_S("/usr/lib/dyld"), &err);
 *     if (f == NULL)
 *         ...
 *     for (Int i = 0; i < f->sections.len; i++) {
 *         MachoSection *s = ((MachoSection **)f->sections.p)[i];
 *         ...
 *     }
 *     Slice libs = macho_file_imported_libraries(f, a, &err);
 *     macho_file_free(f);
 *     arena_free(&ar);
 *
 * A MachoFile keeps everything it reads at the start in an arena of its own
 * on the allocator it was made with, and macho_file_free gives all of it back.
 * What a function returns, such as a section's contents, comes from the
 * allocator that function is given, and can be freed separately or with an
 * arena.
 *
 * Like Go's, this checks only enough to read a file safely, not that the file
 * makes sense, and a malformed file can make it allocate a lot.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/macho */

#ifndef BURROW_DEBUG_MACHO_H
#define BURROW_DEBUG_MACHO_H

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

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- named numbers
 *
 * Each named type has a String and a GoString function. String gives the
 * name of a constant, without the type's name in front for Cpu, Type and
 * LoadCmd and with it for the relocation types, such as "CpuAmd64" and
 * "X86_64_RELOC_BRANCH", and GoString puts "macho." in front of that. A value
 * that is not a constant gives its number in decimal for Cpu, Type and
 * LoadCmd, and "RelocTypeARM(12)" and the like for the relocation types. A
 * name straight from a table is static and anything else is made in a. */

/* The magic numbers at the start of a file. */
#define MACHO_MAGIC32 ((uint32_t)0xfeedface)
#define MACHO_MAGIC64 ((uint32_t)0xfeedfacf)
#define MACHO_MAGIC_FAT ((uint32_t)0xcafebabe)

/* macho.Type, the Mach-O file type of a file. */
typedef uint32_t MachoType;
extern const Type burrow_type_MachoType;
#define TYPE_MACHO_TYPE TYPE_OF(MachoType)

enum {
    MACHO_TYPE_OBJ = 1,
    MACHO_TYPE_EXEC = 2,
    MACHO_TYPE_DYLIB = 6,
    MACHO_TYPE_BUNDLE = 8,
};

BURROW_OWNS(ret) Str macho_type_string(MachoType t, Alloc *a);
BURROW_OWNS(ret) Str macho_type_go_string(MachoType t, Alloc *a);

/* macho.Cpu, a Mach-O cpu type. */
typedef uint32_t MachoCpu;
extern const Type burrow_type_MachoCpu;
#define TYPE_MACHO_CPU TYPE_OF(MachoCpu)

enum {
    MACHO_CPU386 = 7,
    MACHO_CPU_AMD64 = 7 | 0x01000000,
    MACHO_CPU_ARM = 12,
    MACHO_CPU_ARM64 = 12 | 0x01000000,
    MACHO_CPU_PPC = 18,
    MACHO_CPU_PPC64 = 18 | 0x01000000,
};

BURROW_OWNS(ret) Str macho_cpu_string(MachoCpu i, Alloc *a);
BURROW_OWNS(ret) Str macho_cpu_go_string(MachoCpu i, Alloc *a);

/* macho.LoadCmd, a Mach-O load command. String knows only some of them, the
 * same ones Go's does. */
typedef uint32_t MachoLoadCmd;
extern const Type burrow_type_MachoLoadCmd;
#define TYPE_MACHO_LOAD_CMD TYPE_OF(MachoLoadCmd)

#define MACHO_LOAD_CMD_SEGMENT ((MachoLoadCmd)0x1)
#define MACHO_LOAD_CMD_SYMTAB ((MachoLoadCmd)0x2)
#define MACHO_LOAD_CMD_THREAD ((MachoLoadCmd)0x4)
#define MACHO_LOAD_CMD_UNIX_THREAD ((MachoLoadCmd)0x5) /* thread+stack */
#define MACHO_LOAD_CMD_DYSYMTAB ((MachoLoadCmd)0xb)
#define MACHO_LOAD_CMD_DYLIB ((MachoLoadCmd)0xc) /* load dylib command */
/* id dylinker command (not load dylinker command) */
#define MACHO_LOAD_CMD_DYLINKER ((MachoLoadCmd)0xf)
#define MACHO_LOAD_CMD_SEGMENT64 ((MachoLoadCmd)0x19)
#define MACHO_LOAD_CMD_RPATH ((MachoLoadCmd)0x8000001c)

BURROW_OWNS(ret) Str macho_load_cmd_string(MachoLoadCmd i, Alloc *a);
BURROW_OWNS(ret) Str macho_load_cmd_go_string(MachoLoadCmd i, Alloc *a);

/* The flags of FileHeader.flags. */
enum {
    MACHO_FLAG_NO_UNDEFS = 0x1,
    MACHO_FLAG_INCR_LINK = 0x2,
    MACHO_FLAG_DYLD_LINK = 0x4,
    MACHO_FLAG_BIND_AT_LOAD = 0x8,
    MACHO_FLAG_PREBOUND = 0x10,
    MACHO_FLAG_SPLIT_SEGS = 0x20,
    MACHO_FLAG_LAZY_INIT = 0x40,
    MACHO_FLAG_TWO_LEVEL = 0x80,
    MACHO_FLAG_FORCE_FLAT = 0x100,
    MACHO_FLAG_NO_MULTI_DEFS = 0x200,
    MACHO_FLAG_NO_FIX_PREBINDING = 0x400,
    MACHO_FLAG_PREBINDABLE = 0x800,
    MACHO_FLAG_ALL_MODS_BOUND = 0x1000,
    MACHO_FLAG_SUBSECTIONS_VIA_SYMBOLS = 0x2000,
    MACHO_FLAG_CANONICAL = 0x4000,
    MACHO_FLAG_WEAK_DEFINES = 0x8000,
    MACHO_FLAG_BINDS_TO_WEAK = 0x10000,
    MACHO_FLAG_ALLOW_STACK_EXECUTION = 0x20000,
    MACHO_FLAG_ROOT_SAFE = 0x40000,
    MACHO_FLAG_SETUID_SAFE = 0x80000,
    MACHO_FLAG_NO_REEXPORTED_DYLIBS = 0x100000,
    MACHO_FLAG_PIE = 0x200000,
    MACHO_FLAG_DEAD_STRIPPABLE_DYLIB = 0x400000,
    MACHO_FLAG_HAS_TLV_DESCRIPTORS = 0x800000,
    MACHO_FLAG_NO_HEAP_EXECUTION = 0x1000000,
    MACHO_FLAG_APP_EXTENSION_SAFE = 0x2000000,
};

/* macho.RelocTypeGeneric, the relocation types of 386 and the other cpus
 * without types of their own. */
typedef Int MachoRelocTypeGeneric;
extern const Type burrow_type_MachoRelocTypeGeneric;
#define TYPE_MACHO_RELOC_TYPE_GENERIC TYPE_OF(MachoRelocTypeGeneric)

enum {
    MACHO_GENERIC_RELOC_VANILLA = 0,
    MACHO_GENERIC_RELOC_PAIR = 1,
    MACHO_GENERIC_RELOC_SECTDIFF = 2,
    MACHO_GENERIC_RELOC_PB_LA_PTR = 3,
    MACHO_GENERIC_RELOC_LOCAL_SECTDIFF = 4,
    MACHO_GENERIC_RELOC_TLV = 5,
};

BURROW_OWNS(ret) Str macho_reloc_type_generic_string(MachoRelocTypeGeneric r, Alloc *a);
BURROW_OWNS(ret) Str macho_reloc_type_generic_go_string(MachoRelocTypeGeneric r,
                                                        Alloc *a);

/* macho.RelocTypeX86_64. */
typedef Int MachoRelocTypeX86_64;
extern const Type burrow_type_MachoRelocTypeX86_64;
#define TYPE_MACHO_RELOC_TYPE_X86_64 TYPE_OF(MachoRelocTypeX86_64)

enum {
    MACHO_X86_64_RELOC_UNSIGNED = 0,
    MACHO_X86_64_RELOC_SIGNED = 1,
    MACHO_X86_64_RELOC_BRANCH = 2,
    MACHO_X86_64_RELOC_GOT_LOAD = 3,
    MACHO_X86_64_RELOC_GOT = 4,
    MACHO_X86_64_RELOC_SUBTRACTOR = 5,
    MACHO_X86_64_RELOC_SIGNED_1 = 6,
    MACHO_X86_64_RELOC_SIGNED_2 = 7,
    MACHO_X86_64_RELOC_SIGNED_4 = 8,
    MACHO_X86_64_RELOC_TLV = 9,
};

BURROW_OWNS(ret) Str macho_reloc_type_x8664_string(MachoRelocTypeX86_64 r, Alloc *a);
BURROW_OWNS(ret) Str macho_reloc_type_x8664_go_string(MachoRelocTypeX86_64 r, Alloc *a);

/* macho.RelocTypeARM. */
typedef Int MachoRelocTypeARM;
extern const Type burrow_type_MachoRelocTypeARM;
#define TYPE_MACHO_RELOC_TYPE_ARM TYPE_OF(MachoRelocTypeARM)

enum {
    MACHO_ARM_RELOC_VANILLA = 0,
    MACHO_ARM_RELOC_PAIR = 1,
    MACHO_ARM_RELOC_SECTDIFF = 2,
    MACHO_ARM_RELOC_LOCAL_SECTDIFF = 3,
    MACHO_ARM_RELOC_PB_LA_PTR = 4,
    MACHO_ARM_RELOC_BR24 = 5,
    MACHO_ARM_THUMB_RELOC_BR22 = 6,
    MACHO_ARM_THUMB_32BIT_BRANCH = 7,
    MACHO_ARM_RELOC_HALF = 8,
    MACHO_ARM_RELOC_HALF_SECTDIFF = 9,
};

BURROW_OWNS(ret) Str macho_reloc_type_arm_string(MachoRelocTypeARM r, Alloc *a);
BURROW_OWNS(ret) Str macho_reloc_type_arm_go_string(MachoRelocTypeARM r, Alloc *a);

/* macho.RelocTypeARM64. */
typedef Int MachoRelocTypeARM64;
extern const Type burrow_type_MachoRelocTypeARM64;
#define TYPE_MACHO_RELOC_TYPE_ARM64 TYPE_OF(MachoRelocTypeARM64)

enum {
    MACHO_ARM64_RELOC_UNSIGNED = 0,
    MACHO_ARM64_RELOC_SUBTRACTOR = 1,
    MACHO_ARM64_RELOC_BRANCH26 = 2,
    MACHO_ARM64_RELOC_PAGE21 = 3,
    MACHO_ARM64_RELOC_PAGEOFF12 = 4,
    MACHO_ARM64_RELOC_GOT_LOAD_PAGE21 = 5,
    MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6,
    MACHO_ARM64_RELOC_POINTER_TO_GOT = 7,
    MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21 = 8,
    MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9,
    MACHO_ARM64_RELOC_ADDEND = 10,
};

BURROW_OWNS(ret) Str macho_reloc_type_arm64_string(MachoRelocTypeARM64 r, Alloc *a);
BURROW_OWNS(ret) Str macho_reloc_type_arm64_go_string(MachoRelocTypeARM64 r, Alloc *a);

/* ------------------------------------------------------------ file formats
 *
 * The structures as they are in the file, in the file's byte order. The
 * reader does not hand these out, apart from the DysymtabCmd of a Dysymtab,
 * but they are here for code that reads the commands it does not know. */

/* macho.Segment32, a 32-bit segment load command. */
typedef struct MachoSegment32 {
    MachoLoadCmd cmd;
    uint32_t len;
    uint8_t name[16];
    uint32_t addr;
    uint32_t memsz;
    uint32_t offset;
    uint32_t filesz;
    uint32_t maxprot;
    uint32_t prot;
    uint32_t nsect;
    uint32_t flag;
} MachoSegment32;

/* macho.Segment64, a 64-bit segment load command. */
typedef struct MachoSegment64 {
    MachoLoadCmd cmd;
    uint32_t len;
    uint8_t name[16];
    uint64_t addr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t maxprot;
    uint32_t prot;
    uint32_t nsect;
    uint32_t flag;
} MachoSegment64;

/* macho.SymtabCmd, a symbol table load command. */
typedef struct MachoSymtabCmd {
    MachoLoadCmd cmd;
    uint32_t len;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} MachoSymtabCmd;

/* macho.DysymtabCmd, a dynamic symbol table load command. */
typedef struct MachoDysymtabCmd {
    MachoLoadCmd cmd;
    uint32_t len;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoffset;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} MachoDysymtabCmd;

/* macho.DylibCmd, a dynamic library load command. */
typedef struct MachoDylibCmd {
    MachoLoadCmd cmd;
    uint32_t len;
    uint32_t name;
    uint32_t time;
    uint32_t current_version;
    uint32_t compat_version;
} MachoDylibCmd;

/* macho.RpathCmd, an rpath load command. */
typedef struct MachoRpathCmd {
    MachoLoadCmd cmd;
    uint32_t len;
    uint32_t path;
} MachoRpathCmd;

/* macho.Thread, a thread state command. data holds uint32_t. */
typedef struct MachoThread {
    MachoLoadCmd cmd;
    uint32_t len;
    uint32_t type;
    Slice data;
} MachoThread;

/* macho.Section32, a 32-bit section header. */
typedef struct MachoSection32 {
    uint8_t name[16];
    uint8_t seg[16];
    uint32_t addr;
    uint32_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserve1;
    uint32_t reserve2;
} MachoSection32;

/* macho.Section64, a 64-bit section header. */
typedef struct MachoSection64 {
    uint8_t name[16];
    uint8_t seg[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserve1;
    uint32_t reserve2;
    uint32_t reserve3;
} MachoSection64;

/* macho.Nlist32, a 32-bit symbol table entry. */
typedef struct MachoNlist32 {
    uint32_t name;
    uint8_t type;
    uint8_t sect;
    uint16_t desc;
    uint32_t value;
} MachoNlist32;

/* macho.Nlist64, a 64-bit symbol table entry. */
typedef struct MachoNlist64 {
    uint32_t name;
    uint8_t type;
    uint8_t sect;
    uint16_t desc;
    uint64_t value;
} MachoNlist64;

/* macho.Regs386, the registers of a 386 thread. */
typedef struct MachoRegs386 {
    uint32_t ax;
    uint32_t bx;
    uint32_t cx;
    uint32_t dx;
    uint32_t di;
    uint32_t si;
    uint32_t bp;
    uint32_t sp;
    uint32_t ss;
    uint32_t flags;
    uint32_t ip;
    uint32_t cs;
    uint32_t ds;
    uint32_t es;
    uint32_t fs;
    uint32_t gs;
} MachoRegs386;

/* macho.RegsAMD64, the registers of an amd64 thread. */
typedef struct MachoRegsAMD64 {
    uint64_t ax;
    uint64_t bx;
    uint64_t cx;
    uint64_t dx;
    uint64_t di;
    uint64_t si;
    uint64_t bp;
    uint64_t sp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ip;
    uint64_t flags;
    uint64_t cs;
    uint64_t fs;
    uint64_t gs;
} MachoRegsAMD64;

/* ------------------------------------------------------------------ errors */

/* macho.FormatError, what a malformed file gives: what was wrong, the value
 * that was, when there is one, and where. Go keeps the value as an any and
 * formats it when Error is called. Here val is that text already, made with
 * %v when the error was, and has_val says whether there was a value. */
typedef struct MachoFormatError {
    int64_t off;
    Str msg;
    Str val;
    bool has_val;
} MachoFormatError;

extern const Type burrow_type_MachoFormatError;
#define TYPE_MACHO_FORMAT_ERROR TYPE_OF(MachoFormatError)

/* FormatError.Error: "invalid magic number in record at byte 0x0", in a. */
BURROW_OWNS(ret) Str macho_format_error_error(const MachoFormatError *e, Alloc *a);

/* macho.ErrNotFat, which macho_new_fat_file and macho_open_fat give for a
 * file that is not a universal binary but looks like a thin Mach-O file. It
 * is a MachoFormatError, and errors_is tells it apart. */
extern const Error macho_err_not_fat;

/* ------------------------------------------------------------------- loads */

/* Which of the structs below a MachoLoad is. */
typedef enum MachoLoadKind {
    MACHO_LOAD_KIND_BYTES,
    MACHO_LOAD_KIND_SEGMENT,
    MACHO_LOAD_KIND_DYLIB,
    MACHO_LOAD_KIND_SYMTAB,
    MACHO_LOAD_KIND_DYSYMTAB,
    MACHO_LOAD_KIND_RPATH,
} MachoLoadKind;

/* macho.LoadBytes, the uninterpreted bytes of a load command, which every
 * load starts with. raw is the whole command, header included, and kind says
 * which struct the load is, so that a MachoLoad can be cast to it. */
typedef struct MachoLoadBytes {
    MachoLoadKind kind;
    Slice raw;
} MachoLoadBytes;

/* macho.Load, any load command, a pointer to the MachoLoadBytes at the start
 * of a MachoSegment, MachoDylib, MachoSymtab, MachoDysymtab or MachoRpath, or
 * to a MachoLoadBytes of its own for a command of another kind. */
typedef MachoLoadBytes *MachoLoad;

/* Load.Raw and LoadBytes.Raw: the bytes of the command. */
BURROW_BORROWS(ret, l) Slice macho_load_raw(MachoLoad l);
BURROW_BORROWS(ret, b) Slice macho_load_bytes_raw(const MachoLoadBytes *b);

/* macho.SegmentHeader, the header of a 32-bit or 64-bit segment command with
 * the 32-bit fields widened. */
typedef struct MachoSegmentHeader {
    MachoLoadCmd cmd;
    uint32_t len;
    Str name;
    uint64_t addr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t maxprot;
    uint32_t prot;
    uint32_t nsect;
    uint32_t flag;
} MachoSegmentHeader;

/* macho.Segment, a segment load command. reader_at reads the segment's bytes
 * in the file. The rest is the File's. */
typedef struct MachoSegment {
    MachoLoadBytes load_bytes;
    MachoSegmentHeader segment_header;
    IoReaderAt reader_at;

    IoSectionReader sr;
} MachoSegment;

/* Segment.Data: the segment's filesz bytes, in a. */
BURROW_OWNS(ret) Slice macho_segment_data(MachoSegment *s, Alloc *a, Error *err);

/* Segment.Open: a reader of the segment's bytes with its own offset. The box
 * comes from a and macho_reader_free gives it back. */
BURROW_OWNS(ret) IoReadSeeker macho_segment_open(MachoSegment *s, Alloc *a);

/* Segment.ReadAt, through reader_at. */
Int macho_segment_read_at(MachoSegment *s, Slice p, int64_t off, Error *err);

BURROW_BORROWS(ret, s) Slice macho_segment_raw(const MachoSegment *s);

/* macho.SectionHeader. */
typedef struct MachoSectionHeader {
    Str name;
    Str seg;
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
} MachoSectionHeader;

/* macho.Reloc, one relocation of a section. When scattered is false and
 * extern_ is true value is the symbol number, when both are false it is the
 * section number, and when scattered is true it is the value the relocation
 * refers to. len is 0 for a byte, 1 for a word, 2 for a long and 3 for a
 * quad. Go's field Extern is extern_ here, since extern is a keyword in C. */
typedef struct MachoReloc {
    uint32_t addr;
    uint32_t value;
    uint8_t type;
    uint8_t len;
    bool pcrel;
    bool extern_;
    bool scattered;
} MachoReloc;
extern const Type burrow_type_MachoReloc;
#define TYPE_MACHO_RELOC TYPE_OF(MachoReloc)

/* macho.Section, one section of a segment. relocs holds MachoReloc and
 * reader_at reads the section's bytes in the file. The rest is the File's. */
typedef struct MachoSection {
    MachoSectionHeader section_header;
    Slice relocs;
    IoReaderAt reader_at;

    IoSectionReader sr;
} MachoSection;

/* Section.Data: the section's size bytes, in a. */
BURROW_OWNS(ret) Slice macho_section_data(MachoSection *s, Alloc *a, Error *err);

/* Section.Open: a reader of the section's bytes with its own offset. The box
 * comes from a and macho_reader_free gives it back. */
BURROW_OWNS(ret) IoReadSeeker macho_section_open(MachoSection *s, Alloc *a);

/* Section.ReadAt, through reader_at. */
Int macho_section_read_at(MachoSection *s, Slice p, int64_t off, Error *err);

/* Gives back a reader macho_section_open or macho_segment_open made. */
void macho_reader_free(IoReadSeeker r);

/* macho.Dylib, a load dynamic library command. */
typedef struct MachoDylib {
    MachoLoadBytes load_bytes;
    Str name;
    uint32_t time;
    uint32_t current_version;
    uint32_t compat_version;
} MachoDylib;

BURROW_BORROWS(ret, d) Slice macho_dylib_raw(const MachoDylib *d);

/* macho.Symbol, a 32-bit or 64-bit symbol table entry. */
typedef struct MachoSymbol {
    Str name;
    uint8_t type;
    uint8_t sect;
    uint16_t desc;
    uint64_t value;
} MachoSymbol;
extern const Type burrow_type_MachoSymbol;
#define TYPE_MACHO_SYMBOL TYPE_OF(MachoSymbol)

/* macho.Symtab, a symbol table command. syms holds MachoSymbol. Go never
 * fills in its SymtabCmd, and this leaves symtab_cmd zero the same way. */
typedef struct MachoSymtab {
    MachoLoadBytes load_bytes;
    MachoSymtabCmd symtab_cmd;
    Slice syms;
} MachoSymtab;

BURROW_BORROWS(ret, s) Slice macho_symtab_raw(const MachoSymtab *s);

/* macho.Dysymtab, a dynamic symbol table command. indirect_syms holds
 * uint32_t indexes into the Symtab's syms. */
typedef struct MachoDysymtab {
    MachoLoadBytes load_bytes;
    MachoDysymtabCmd dysymtab_cmd;
    Slice indirect_syms;
} MachoDysymtab;

BURROW_BORROWS(ret, d) Slice macho_dysymtab_raw(const MachoDysymtab *d);

/* macho.Rpath, an rpath command. */
typedef struct MachoRpath {
    MachoLoadBytes load_bytes;
    Str path;
} MachoRpath;

BURROW_BORROWS(ret, r) Slice macho_rpath_raw(const MachoRpath *r);

/* ------------------------------------------------------------------- files */

/* macho.FileHeader. */
typedef struct MachoFileHeader {
    uint32_t magic;
    MachoCpu cpu;
    uint32_t sub_cpu;
    MachoType type;
    uint32_t ncmd;
    uint32_t cmdsz;
    uint32_t flags;
} MachoFileHeader;

/* macho.File, an open Mach-O file. loads holds a MachoLoad for each load
 * command and sections a MachoSection * for each section of each segment, in
 * the order the file has them. symtab and dysymtab are the last commands of
 * those kinds, or NULL. The rest is the File's. */
typedef struct MachoFile {
    MachoFileHeader file_header;
    BinaryByteOrder byte_order;
    Slice loads;
    Slice sections;
    MachoSymtab *symtab;
    MachoDysymtab *dysymtab;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
} MachoFile;

/* macho.NewFile: reads the Mach-O file at the start of r, which has to stay
 * readable while the result is used. A file that is not Mach-O, or a
 * malformed one, gives NULL and the error, which for a malformed one is
 * usually a MachoFormatError. */
BURROW_OWNS(ret) MachoFile *macho_new_file(Alloc *a, IoReaderAt r, Error *err);

/* macho.Open: opens the file at name and reads it as macho_new_file does.
 * The MachoFile keeps the file open until macho_file_close or
 * macho_file_free. */
BURROW_OWNS(ret) MachoFile *macho_open(Alloc *a, Str name, Error *err);

/* File.Close: closes the file macho_open opened, and does nothing for a File
 * from macho_new_file. f can still be freed afterwards. */
BURROW_STATIC(ret) Error macho_file_close(MachoFile *f);

/* Closes f as macho_file_close does and gives back everything it holds. NULL
 * is fine. Not for the files of a MachoFatFile, which it frees itself. */
void macho_file_free(MachoFile *f);

/* File.Segment: the first segment called name, or NULL. */
BURROW_BORROWS(ret, f) MachoSegment *macho_file_segment(MachoFile *f, Str name);

/* File.Section: the first section called name, or NULL. */
BURROW_BORROWS(ret, f) MachoSection *macho_file_section(MachoFile *f, Str name);

/* File.DWARF: the DWARF in the __debug and __zdebug sections, whose contents
 * it reads into a. */
BURROW_OWNS(ret) DwarfData *macho_file_dwarf(MachoFile *f, Alloc *a, Error *err);

/* File.ImportedSymbols: the Str names of the symbols the file expects other
 * libraries to supply when it is loaded, in a, or a nil slice when there are
 * none. The names are the File's. */
BURROW_OWNS(ret) Slice macho_file_imported_symbols(MachoFile *f, Alloc *a, Error *err);

/* File.ImportedLibraries: the Str paths of the libraries the file is linked
 * against, in a, or a nil slice when there are none. The paths are the
 * File's. */
BURROW_OWNS(ret) Slice macho_file_imported_libraries(MachoFile *f, Alloc *a,
                                                     Error *err);

/* ---------------------------------------------------------- universal files */

/* macho.FatArchHeader, the header of one architecture of a universal binary,
 * which is always big-endian in the file. */
typedef struct MachoFatArchHeader {
    MachoCpu cpu;
    uint32_t sub_cpu;
    uint32_t offset;
    uint32_t size;
    uint32_t align;
} MachoFatArchHeader;
extern const Type burrow_type_MachoFatArchHeader;
#define TYPE_MACHO_FAT_ARCH_HEADER TYPE_OF(MachoFatArchHeader)

/* macho.FatArch, one architecture of a universal binary and the Mach-O file
 * of it, which the FatFile owns. */
typedef struct MachoFatArch {
    MachoFatArchHeader fat_arch_header;
    MachoFile *file;
} MachoFatArch;
extern const Type burrow_type_MachoFatArch;
#define TYPE_MACHO_FAT_ARCH TYPE_OF(MachoFatArch)

/* macho.FatFile, a universal binary. arches holds a MachoFatArch for each
 * architecture. The rest is the FatFile's. */
typedef struct MachoFatFile {
    uint32_t magic;
    Slice arches;

    Alloc *a;
    Arena arena;
    OsFile *file;
    bool open;
} MachoFatFile;

/* macho.NewFatFile: reads the universal binary at the start of r, and the
 * Mach-O file of each architecture in it, which has to stay readable while
 * the result is used. A thin Mach-O file gives macho_err_not_fat. */
BURROW_OWNS(ret) MachoFatFile *macho_new_fat_file(Alloc *a, IoReaderAt r, Error *err);

/* macho.OpenFat: opens the file at name and reads it as macho_new_fat_file
 * does, keeping it open until macho_fat_file_close or macho_fat_file_free. */
BURROW_OWNS(ret) MachoFatFile *macho_open_fat(Alloc *a, Str name, Error *err);

/* FatFile.Close: closes the file macho_open_fat opened. */
BURROW_STATIC(ret) Error macho_fat_file_close(MachoFatFile *ff);

/* Closes ff and gives back everything it holds, the File of each
 * architecture included. NULL is fine. */
void macho_fat_file_free(MachoFatFile *ff);

/* The methods a FatArch gets from the File in it. */
BURROW_STATIC(ret) Error macho_fat_arch_close(MachoFatArch *fa);
BURROW_BORROWS(ret, fa) MachoSegment *macho_fat_arch_segment(MachoFatArch *fa,
                                                             Str name);
BURROW_BORROWS(ret, fa) MachoSection *macho_fat_arch_section(MachoFatArch *fa,
                                                             Str name);
BURROW_OWNS(ret) DwarfData *macho_fat_arch_dwarf(MachoFatArch *fa, Alloc *a,
                                                 Error *err);
BURROW_OWNS(ret) Slice macho_fat_arch_imported_symbols(MachoFatArch *fa, Alloc *a,
                                                       Error *err);
BURROW_OWNS(ret) Slice macho_fat_arch_imported_libraries(MachoFatArch *fa, Alloc *a,
                                                         Error *err);

#ifdef __cplusplus
}
#endif

#endif
