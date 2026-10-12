/* Derived from Go's src/debug/macho/file.go, fat.go, macho.go, reloctype.go
 * and reloctype_string.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/macho.h"

#include "objfile_internal.h"

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The sizes of the structures in the file. */
enum {
    MACHO_FILE_HEADER_SIZE32 = 7 * 4,
    MACHO_FILE_HEADER_SIZE64 = 8 * 4,
    MACHO_SEGMENT32_SIZE = 56,
    MACHO_SEGMENT64_SIZE = 72,
    MACHO_SECTION32_SIZE = 68,
    MACHO_SECTION64_SIZE = 80,
    MACHO_SYMTAB_CMD_SIZE = 24,
    MACHO_DYSYMTAB_CMD_SIZE = 80,
    MACHO_DYLIB_CMD_SIZE = 24,
    MACHO_RPATH_CMD_SIZE = 12,
    MACHO_FAT_ARCH_HEADER_SIZE = 5 * 4,
};

static uint32_t macho_u32(bool big, const Byte *p) {
    if (big)
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint16_t macho_u16(bool big, const Byte *p) {
    if (big)
        return (uint16_t)((unsigned)p[0] << 8 | p[1]);
    return (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static uint64_t macho_u64(bool big, const Byte *p) {
    if (big)
        return (uint64_t)macho_u32(true, p) << 32 | macho_u32(true, p + 4);
    return (uint64_t)macho_u32(false, p + 4) << 32 | macho_u32(false, p);
}

static Slice macho_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static Int macho_put(Byte *p, Int at, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p + at, s.p, (size_t)s.len);
    return at + s.len;
}

/* v in decimal, at the end of buf. */
static Str macho_dec(Byte buf[24], uint64_t v, bool neg) {
    Int i = 24;
    do {
        buf[--i] = (Byte)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);
    if (neg)
        buf[--i] = '-';
    return str_from_bytes(buf + i, 24 - i);
}

static Str macho_udec(Byte buf[24], uint64_t v) {
    return macho_dec(buf, v, false);
}

static Str macho_idec(Byte buf[24], int64_t v) {
    return v < 0 ? macho_dec(buf, 0 - (uint64_t)v, true)
                 : macho_dec(buf, (uint64_t)v, false);
}

static void macho_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* Appends the pointer p to s, false when out of memory. */
static bool macho_push(Alloc *a, Slice *s, void *p) {
    Slice out = slice_append(a, *s, &p, 1);
    if (out.len != s->len + 1)
        return false;
    *s = out;
    return true;
}

/* ------------------------------------------------------------ named numbers */

typedef struct MachoIntName {
    uint32_t i;
    Str s;
} MachoIntName;

#define MACHO_NNAME(t) ((Int)(sizeof(t) / sizeof((t)[0])))

static const MachoIntName macho_type_strings[] = {
    {1, BURROW_S_INIT("Obj")},
    {2, BURROW_S_INIT("Exec")},
    {6, BURROW_S_INIT("Dylib")},
    {8, BURROW_S_INIT("Bundle")},
};

static const MachoIntName macho_cpu_strings[] = {
    {MACHO_CPU386, BURROW_S_INIT("Cpu386")},
    {MACHO_CPU_AMD64, BURROW_S_INIT("CpuAmd64")},
    {MACHO_CPU_ARM, BURROW_S_INIT("CpuArm")},
    {MACHO_CPU_ARM64, BURROW_S_INIT("CpuArm64")},
    {MACHO_CPU_PPC, BURROW_S_INIT("CpuPpc")},
    {MACHO_CPU_PPC64, BURROW_S_INIT("CpuPpc64")},
};

/* Go's has no Symtab, Dysymtab or Dylinker, so neither does this. */
static const MachoIntName macho_cmd_strings[] = {
    {MACHO_LOAD_CMD_SEGMENT, BURROW_S_INIT("LoadCmdSegment")},
    {MACHO_LOAD_CMD_THREAD, BURROW_S_INIT("LoadCmdThread")},
    {MACHO_LOAD_CMD_UNIX_THREAD, BURROW_S_INIT("LoadCmdUnixThread")},
    {MACHO_LOAD_CMD_DYLIB, BURROW_S_INIT("LoadCmdDylib")},
    {MACHO_LOAD_CMD_SEGMENT64, BURROW_S_INIT("LoadCmdSegment64")},
    {MACHO_LOAD_CMD_RPATH, BURROW_S_INIT("LoadCmdRpath")},
};

/* prefix then s, in a. */
static Str macho_concat(Alloc *a, Str prefix, Str s, Str suffix) {
    Int n = prefix.len + s.len + suffix.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    macho_put(p, macho_put(p, macho_put(p, 0, prefix), s), suffix);
    return str_from_bytes(p, n);
}

/* stringName. */
static Str macho_string_name(uint32_t i, const MachoIntName *names, Int n,
                             bool go_syntax, Alloc *a) {
    for (Int j = 0; j < n; j++) {
        if (names[j].i == i) {
            if (!go_syntax)
                return names[j].s;
            return macho_concat(a, BURROW_S("macho."), names[j].s, BURROW_STR_EMPTY);
        }
    }
    Byte buf[24];
    return macho_concat(a, BURROW_STR_EMPTY, macho_udec(buf, i), BURROW_STR_EMPTY);
}

Str macho_type_string(MachoType t, Alloc *a) {
    return macho_string_name(t, macho_type_strings, MACHO_NNAME(macho_type_strings),
                             false, a);
}

Str macho_type_go_string(MachoType t, Alloc *a) {
    return macho_string_name(t, macho_type_strings, MACHO_NNAME(macho_type_strings),
                             true, a);
}

Str macho_cpu_string(MachoCpu i, Alloc *a) {
    return macho_string_name(i, macho_cpu_strings, MACHO_NNAME(macho_cpu_strings),
                             false, a);
}

Str macho_cpu_go_string(MachoCpu i, Alloc *a) {
    return macho_string_name(i, macho_cpu_strings, MACHO_NNAME(macho_cpu_strings), true,
                             a);
}

Str macho_load_cmd_string(MachoLoadCmd i, Alloc *a) {
    return macho_string_name(i, macho_cmd_strings, MACHO_NNAME(macho_cmd_strings),
                             false, a);
}

Str macho_load_cmd_go_string(MachoLoadCmd i, Alloc *a) {
    return macho_string_name(i, macho_cmd_strings, MACHO_NNAME(macho_cmd_strings), true,
                             a);
}

static const Str macho_reloc_generic_names[] = {
    BURROW_S_INIT("GENERIC_RELOC_VANILLA"),
    BURROW_S_INIT("GENERIC_RELOC_PAIR"),
    BURROW_S_INIT("GENERIC_RELOC_SECTDIFF"),
    BURROW_S_INIT("GENERIC_RELOC_PB_LA_PTR"),
    BURROW_S_INIT("GENERIC_RELOC_LOCAL_SECTDIFF"),
    BURROW_S_INIT("GENERIC_RELOC_TLV"),
};

static const Str macho_reloc_x8664_names[] = {
    BURROW_S_INIT("X86_64_RELOC_UNSIGNED"), BURROW_S_INIT("X86_64_RELOC_SIGNED"),
    BURROW_S_INIT("X86_64_RELOC_BRANCH"),   BURROW_S_INIT("X86_64_RELOC_GOT_LOAD"),
    BURROW_S_INIT("X86_64_RELOC_GOT"),      BURROW_S_INIT("X86_64_RELOC_SUBTRACTOR"),
    BURROW_S_INIT("X86_64_RELOC_SIGNED_1"), BURROW_S_INIT("X86_64_RELOC_SIGNED_2"),
    BURROW_S_INIT("X86_64_RELOC_SIGNED_4"), BURROW_S_INIT("X86_64_RELOC_TLV"),
};

static const Str macho_reloc_arm_names[] = {
    BURROW_S_INIT("ARM_RELOC_VANILLA"),    BURROW_S_INIT("ARM_RELOC_PAIR"),
    BURROW_S_INIT("ARM_RELOC_SECTDIFF"),   BURROW_S_INIT("ARM_RELOC_LOCAL_SECTDIFF"),
    BURROW_S_INIT("ARM_RELOC_PB_LA_PTR"),  BURROW_S_INIT("ARM_RELOC_BR24"),
    BURROW_S_INIT("ARM_THUMB_RELOC_BR22"), BURROW_S_INIT("ARM_THUMB_32BIT_BRANCH"),
    BURROW_S_INIT("ARM_RELOC_HALF"),       BURROW_S_INIT("ARM_RELOC_HALF_SECTDIFF"),
};

static const Str macho_reloc_arm64_names[] = {
    BURROW_S_INIT("ARM64_RELOC_UNSIGNED"),
    BURROW_S_INIT("ARM64_RELOC_SUBTRACTOR"),
    BURROW_S_INIT("ARM64_RELOC_BRANCH26"),
    BURROW_S_INIT("ARM64_RELOC_PAGE21"),
    BURROW_S_INIT("ARM64_RELOC_PAGEOFF12"),
    BURROW_S_INIT("ARM64_RELOC_GOT_LOAD_PAGE21"),
    BURROW_S_INIT("ARM64_RELOC_GOT_LOAD_PAGEOFF12"),
    BURROW_S_INIT("ARM64_RELOC_POINTER_TO_GOT"),
    BURROW_S_INIT("ARM64_RELOC_TLVP_LOAD_PAGE21"),
    BURROW_S_INIT("ARM64_RELOC_TLVP_LOAD_PAGEOFF12"),
    BURROW_S_INIT("ARM64_RELOC_ADDEND"),
};

/* What stringer makes: the name, or the type's name and the number in
 * parentheses. GoString puts "macho." in front of either. */
static Str macho_reloc_name(Int r, const Str *names, Int n, Str type, bool go_syntax,
                            Alloc *a) {
    Str gp = go_syntax ? BURROW_S("macho.") : BURROW_STR_EMPTY;
    if (r >= 0 && r < n) {
        if (!go_syntax)
            return names[r];
        return macho_concat(a, gp, names[r], BURROW_STR_EMPTY);
    }
    Byte buf[24];
    Str num = macho_idec(buf, (int64_t)r);
    Int len = gp.len + type.len + 1 + num.len + 1;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int k = macho_put(p, 0, gp);
    k = macho_put(p, k, type);
    k = macho_put(p, k, BURROW_S("("));
    k = macho_put(p, k, num);
    macho_put(p, k, BURROW_S(")"));
    return str_from_bytes(p, len);
}

Str macho_reloc_type_generic_string(MachoRelocTypeGeneric r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_generic_names,
                            MACHO_NNAME(macho_reloc_generic_names),
                            BURROW_S("RelocTypeGeneric"), false, a);
}

Str macho_reloc_type_generic_go_string(MachoRelocTypeGeneric r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_generic_names,
                            MACHO_NNAME(macho_reloc_generic_names),
                            BURROW_S("RelocTypeGeneric"), true, a);
}

Str macho_reloc_type_x8664_string(MachoRelocTypeX86_64 r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_x8664_names,
                            MACHO_NNAME(macho_reloc_x8664_names),
                            BURROW_S("RelocTypeX86_64"), false, a);
}

Str macho_reloc_type_x8664_go_string(MachoRelocTypeX86_64 r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_x8664_names,
                            MACHO_NNAME(macho_reloc_x8664_names),
                            BURROW_S("RelocTypeX86_64"), true, a);
}

Str macho_reloc_type_arm_string(MachoRelocTypeARM r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_arm_names,
                            MACHO_NNAME(macho_reloc_arm_names),
                            BURROW_S("RelocTypeARM"), false, a);
}

Str macho_reloc_type_arm_go_string(MachoRelocTypeARM r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_arm_names,
                            MACHO_NNAME(macho_reloc_arm_names),
                            BURROW_S("RelocTypeARM"), true, a);
}

Str macho_reloc_type_arm64_string(MachoRelocTypeARM64 r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_arm64_names,
                            MACHO_NNAME(macho_reloc_arm64_names),
                            BURROW_S("RelocTypeARM64"), false, a);
}

Str macho_reloc_type_arm64_go_string(MachoRelocTypeARM64 r, Alloc *a) {
    return macho_reloc_name(r, macho_reloc_arm64_names,
                            MACHO_NNAME(macho_reloc_arm64_names),
                            BURROW_S("RelocTypeARM64"), true, a);
}

#define MACHO_SIG_STRING(IN, OUT) OUT(Str)

static Str macho_type_m_go_string(MachoType *self) {
    return macho_type_go_string(*self, error_allocator());
}

static Str macho_type_m_string(MachoType *self) {
    return macho_type_string(*self, error_allocator());
}

#define MACHO_TYPE_METHODS(M, T)                                                       \
    M(T, GoString, macho_type_m_go_string, MACHO_SIG_STRING)                           \
    M(T, String, macho_type_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoType, MACHO_TYPE_METHODS);

const Type burrow_type_MachoType = {
    {(const Byte *)"Type", 4},
    {(const Byte *)"debug/macho", 11},
    KIND_UINT32,
    (uint32_t)sizeof(MachoType),
    (uint16_t)_Alignof(MachoType),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoType) /
               sizeof(burrow__methods_MachoType[0])),
    NULL,
    burrow__methods_MachoType,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_cpu_m_go_string(MachoCpu *self) {
    return macho_cpu_go_string(*self, error_allocator());
}

static Str macho_cpu_m_string(MachoCpu *self) {
    return macho_cpu_string(*self, error_allocator());
}

#define MACHO_CPU_METHODS(M, T)                                                        \
    M(T, GoString, macho_cpu_m_go_string, MACHO_SIG_STRING)                            \
    M(T, String, macho_cpu_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoCpu, MACHO_CPU_METHODS);

const Type burrow_type_MachoCpu = {
    {(const Byte *)"Cpu", 3},
    {(const Byte *)"debug/macho", 11},
    KIND_UINT32,
    (uint32_t)sizeof(MachoCpu),
    (uint16_t)_Alignof(MachoCpu),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoCpu) / sizeof(burrow__methods_MachoCpu[0])),
    NULL,
    burrow__methods_MachoCpu,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_load_cmd_m_go_string(MachoLoadCmd *self) {
    return macho_load_cmd_go_string(*self, error_allocator());
}

static Str macho_load_cmd_m_string(MachoLoadCmd *self) {
    return macho_load_cmd_string(*self, error_allocator());
}

#define MACHO_LOAD_CMD_METHODS(M, T)                                                   \
    M(T, GoString, macho_load_cmd_m_go_string, MACHO_SIG_STRING)                       \
    M(T, String, macho_load_cmd_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoLoadCmd, MACHO_LOAD_CMD_METHODS);

const Type burrow_type_MachoLoadCmd = {
    {(const Byte *)"LoadCmd", 7},
    {(const Byte *)"debug/macho", 11},
    KIND_UINT32,
    (uint32_t)sizeof(MachoLoadCmd),
    (uint16_t)_Alignof(MachoLoadCmd),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoLoadCmd) /
               sizeof(burrow__methods_MachoLoadCmd[0])),
    NULL,
    burrow__methods_MachoLoadCmd,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_reloc_type_generic_m_go_string(MachoRelocTypeGeneric *self) {
    return macho_reloc_type_generic_go_string(*self, error_allocator());
}

static Str macho_reloc_type_generic_m_string(MachoRelocTypeGeneric *self) {
    return macho_reloc_type_generic_string(*self, error_allocator());
}

#define MACHO_RELOC_TYPE_GENERIC_METHODS(M, T)                                         \
    M(T, GoString, macho_reloc_type_generic_m_go_string, MACHO_SIG_STRING)             \
    M(T, String, macho_reloc_type_generic_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoRelocTypeGeneric, MACHO_RELOC_TYPE_GENERIC_METHODS);

const Type burrow_type_MachoRelocTypeGeneric = {
    {(const Byte *)"RelocTypeGeneric", 16},
    {(const Byte *)"debug/macho", 11},
    KIND_INT,
    (uint32_t)sizeof(MachoRelocTypeGeneric),
    (uint16_t)_Alignof(MachoRelocTypeGeneric),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoRelocTypeGeneric) /
               sizeof(burrow__methods_MachoRelocTypeGeneric[0])),
    NULL,
    burrow__methods_MachoRelocTypeGeneric,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_reloc_type_x8664_m_go_string(MachoRelocTypeX86_64 *self) {
    return macho_reloc_type_x8664_go_string(*self, error_allocator());
}

static Str macho_reloc_type_x8664_m_string(MachoRelocTypeX86_64 *self) {
    return macho_reloc_type_x8664_string(*self, error_allocator());
}

#define MACHO_RELOC_TYPE_X8664_METHODS(M, T)                                           \
    M(T, GoString, macho_reloc_type_x8664_m_go_string, MACHO_SIG_STRING)               \
    M(T, String, macho_reloc_type_x8664_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoRelocTypeX86_64, MACHO_RELOC_TYPE_X8664_METHODS);

const Type burrow_type_MachoRelocTypeX86_64 = {
    {(const Byte *)"RelocTypeX86_64", 15},
    {(const Byte *)"debug/macho", 11},
    KIND_INT,
    (uint32_t)sizeof(MachoRelocTypeX86_64),
    (uint16_t)_Alignof(MachoRelocTypeX86_64),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoRelocTypeX86_64) /
               sizeof(burrow__methods_MachoRelocTypeX86_64[0])),
    NULL,
    burrow__methods_MachoRelocTypeX86_64,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_reloc_type_arm_m_go_string(MachoRelocTypeARM *self) {
    return macho_reloc_type_arm_go_string(*self, error_allocator());
}

static Str macho_reloc_type_arm_m_string(MachoRelocTypeARM *self) {
    return macho_reloc_type_arm_string(*self, error_allocator());
}

#define MACHO_RELOC_TYPE_ARM_METHODS(M, T)                                             \
    M(T, GoString, macho_reloc_type_arm_m_go_string, MACHO_SIG_STRING)                 \
    M(T, String, macho_reloc_type_arm_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoRelocTypeARM, MACHO_RELOC_TYPE_ARM_METHODS);

const Type burrow_type_MachoRelocTypeARM = {
    {(const Byte *)"RelocTypeARM", 12},
    {(const Byte *)"debug/macho", 11},
    KIND_INT,
    (uint32_t)sizeof(MachoRelocTypeARM),
    (uint16_t)_Alignof(MachoRelocTypeARM),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoRelocTypeARM) /
               sizeof(burrow__methods_MachoRelocTypeARM[0])),
    NULL,
    burrow__methods_MachoRelocTypeARM,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_reloc_type_arm64_m_go_string(MachoRelocTypeARM64 *self) {
    return macho_reloc_type_arm64_go_string(*self, error_allocator());
}

static Str macho_reloc_type_arm64_m_string(MachoRelocTypeARM64 *self) {
    return macho_reloc_type_arm64_string(*self, error_allocator());
}

#define MACHO_RELOC_TYPE_ARM64_METHODS(M, T)                                           \
    M(T, GoString, macho_reloc_type_arm64_m_go_string, MACHO_SIG_STRING)               \
    M(T, String, macho_reloc_type_arm64_m_string, MACHO_SIG_STRING)

BURROW_METHODS_DEFINE(MachoRelocTypeARM64, MACHO_RELOC_TYPE_ARM64_METHODS);

const Type burrow_type_MachoRelocTypeARM64 = {
    {(const Byte *)"RelocTypeARM64", 14},
    {(const Byte *)"debug/macho", 11},
    KIND_INT,
    (uint32_t)sizeof(MachoRelocTypeARM64),
    (uint16_t)_Alignof(MachoRelocTypeARM64),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoRelocTypeARM64) /
               sizeof(burrow__methods_MachoRelocTypeARM64[0])),
    NULL,
    burrow__methods_MachoRelocTypeARM64,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ---------------------------------------------------------------- structs */

#define MACHO_SYMBOL_FIELDS(F, T)                                                      \
    F(T, Str, name, Name, "")                                                          \
    F(T, uint8_t, type, Type, "")                                                      \
    F(T, uint8_t, sect, Sect, "")                                                      \
    F(T, uint16_t, desc, Desc, "")                                                     \
    F(T, uint64_t, value, Value, "")

#define MACHO_RELOC_FIELDS(F, T)                                                       \
    F(T, uint32_t, addr, Addr, "")                                                     \
    F(T, uint32_t, value, Value, "")                                                   \
    F(T, uint8_t, type, Type, "")                                                      \
    F(T, uint8_t, len, Len, "")                                                        \
    F(T, bool, pcrel, Pcrel, "")                                                       \
    F(T, bool, extern_, Extern, "")                                                    \
    F(T, bool, scattered, Scattered, "")

#define MACHO_FAT_ARCH_HEADER_FIELDS(F, T)                                             \
    F(T, MachoCpu, cpu, Cpu, "")                                                       \
    F(T, uint32_t, sub_cpu, SubCpu, "")                                                \
    F(T, uint32_t, offset, Offset, "")                                                 \
    F(T, uint32_t, size, Size, "")                                                     \
    F(T, uint32_t, align, Align, "")

/* The File is a pointer the descriptor has no type for, so it is an
 * unsafe.Pointer here. */
#define MACHO_FAT_ARCH_FIELDS(F, T)                                                    \
    F(T, MachoFatArchHeader, fat_arch_header, FatArchHeader, "")                       \
    F(T, UnsafePointer, file, File, "")

BURROW_STRUCT_AS_DEFINE(MachoSymbol, MACHO_SYMBOL_FIELDS);
BURROW_STRUCT_AS_DEFINE(MachoReloc, MACHO_RELOC_FIELDS);
BURROW_STRUCT_AS_DEFINE(MachoFatArchHeader, MACHO_FAT_ARCH_HEADER_FIELDS);
BURROW_STRUCT_AS_DEFINE(MachoFatArch, MACHO_FAT_ARCH_FIELDS);

/* ------------------------------------------------------------------ errors */

/* The public struct first, so that errors_as hands back a MachoFormatError,
 * and the message after it. */
typedef struct MachoFormatErrorBox {
    MachoFormatError e;
    Str message;
} MachoFormatErrorBox;

/* FormatError.Error into p, or only its length when p is NULL. */
static Int macho_format_error_put(Byte *p, const MachoFormatError *e) {
    Int n = macho_put(p, 0, e->msg);
    if (e->has_val) {
        n = macho_put(p, n, BURROW_S(" '"));
        n = macho_put(p, n, e->val);
        n = macho_put(p, n, BURROW_S("'"));
    }
    n = macho_put(p, n, BURROW_S(" in record at byte "));
    uint64_t v = (uint64_t)e->off;
    if (e->off < 0) {
        n = macho_put(p, n, BURROW_S("-"));
        v = 0 - v;
    }
    n = macho_put(p, n, BURROW_S("0x"));
    Byte hex[16];
    Int nh = 0;
    do {
        hex[nh++] = (Byte) "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    while (nh > 0) {
        if (p != NULL)
            p[n] = hex[nh - 1];
        n++;
        nh--;
    }
    return n;
}

Str macho_format_error_error(const MachoFormatError *e, Alloc *a) {
    if (e == NULL)
        macho_nil_panic();
    Int n = macho_format_error_put(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    macho_format_error_put(p, e);
    return str_from_bytes(p, n);
}

static Str macho_format_error_m_error(MachoFormatError *self) {
    return macho_format_error_error(self, error_allocator());
}

#define MACHO_FORMAT_ERROR_SIG_ERROR(IN, OUT) OUT(Str)
#define MACHO_FORMAT_ERROR_METHODS(M, T)                                               \
    M(T, Error, macho_format_error_m_error, MACHO_FORMAT_ERROR_SIG_ERROR)

BURROW_METHODS_DEFINE(MachoFormatError, MACHO_FORMAT_ERROR_METHODS);

const Type burrow_type_MachoFormatError = {
    {(const Byte *)"FormatError", 11},
    {(const Byte *)"debug/macho", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(MachoFormatError),
    (uint16_t)_Alignof(MachoFormatError),
    0,
    (uint16_t)(sizeof(burrow__methods_MachoFormatError) /
               sizeof(burrow__methods_MachoFormatError[0])),
    NULL,
    burrow__methods_MachoFormatError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str macho_format_error_message(const void *self) {
    return ((const MachoFormatErrorBox *)self)->message;
}

static Error macho_format_error_clone(const void *self, Alloc *a);

static const ErrorVT macho_format_error_vt = {
    &burrow_type_MachoFormatError, macho_format_error_message, NULL, NULL, NULL, NULL,
    macho_format_error_clone,
};

/* One allocation: the box, then the message, the value and the text. */
static Error macho_format_error_build(Alloc *a, int64_t off, Str msg, Str val,
                                      bool has_val) {
    if (!has_val)
        val = BURROW_STR_EMPTY;
    MachoFormatError e = {off, msg, val, has_val};
    Int tlen = macho_format_error_put(NULL, &e);
    size_t size =
        sizeof(MachoFormatErrorBox) + (size_t)msg.len + (size_t)val.len + (size_t)tlen;
    MachoFormatErrorBox *b =
        (MachoFormatErrorBox *)mem_alloc_nozero(a, size, _Alignof(MachoFormatErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.off = off;
    b->e.has_val = has_val;
    b->e.msg = str_from_bytes(p, msg.len);
    p += macho_put(p, 0, msg);
    b->e.val = str_from_bytes(p, val.len);
    p += macho_put(p, 0, val);
    macho_format_error_put(p, &b->e);
    b->message = str_from_bytes(p, tlen);
    return (Error){&macho_format_error_vt, b};
}

static Error macho_format_error_clone(const void *self, Alloc *a) {
    const MachoFormatError *e = (const MachoFormatError *)self;
    return macho_format_error_build(a, e->off, e->msg, e->val, e->has_val);
}

/* &FormatError{off, msg, val}, with val already formatted as %v would. */
static Error macho_fe(int64_t off, Str msg, Str val) {
    return macho_format_error_build(error_allocator(), off, msg, val, true);
}

static Error macho_fe_nil(int64_t off, Str msg) {
    return macho_format_error_build(error_allocator(), off, msg, BURROW_STR_EMPTY,
                                    false);
}

static Error macho_fe_u(int64_t off, Str msg, uint64_t v) {
    Byte buf[24];
    return macho_fe(off, msg, macho_udec(buf, v));
}

/* ErrNotFat is one pointer in Go, which errors.Is compares, so it is one box
 * here with a vtable whose clone hands it back as it is. */
static const MachoFormatErrorBox macho_not_fat_box = {
    {0, BURROW_S_INIT("not a fat Mach-O file"), BURROW_S_INIT(""), false},
    BURROW_S_INIT("not a fat Mach-O file in record at byte 0x0"),
};

static Error macho_not_fat_clone(const void *self, Alloc *a) {
    (void)self;
    (void)a;
    return macho_err_not_fat;
}

static const ErrorVT macho_not_fat_vt = {
    &burrow_type_MachoFormatError, macho_format_error_message, NULL, NULL, NULL, NULL,
    macho_not_fat_clone,
};

const Error macho_err_not_fat = {&macho_not_fat_vt, &macho_not_fat_box};

/* ------------------------------------------------------------------- loads */

Slice macho_load_bytes_raw(const MachoLoadBytes *b) {
    if (b == NULL)
        macho_nil_panic();
    return b->raw;
}

Slice macho_load_raw(MachoLoad l) {
    return macho_load_bytes_raw(l);
}

Slice macho_segment_raw(const MachoSegment *s) {
    if (s == NULL)
        macho_nil_panic();
    return s->load_bytes.raw;
}

Slice macho_dylib_raw(const MachoDylib *d) {
    if (d == NULL)
        macho_nil_panic();
    return d->load_bytes.raw;
}

Slice macho_symtab_raw(const MachoSymtab *s) {
    if (s == NULL)
        macho_nil_panic();
    return s->load_bytes.raw;
}

Slice macho_dysymtab_raw(const MachoDysymtab *d) {
    if (d == NULL)
        macho_nil_panic();
    return d->load_bytes.raw;
}

Slice macho_rpath_raw(const MachoRpath *r) {
    if (r == NULL)
        macho_nil_panic();
    return r->load_bytes.raw;
}

Slice macho_segment_data(MachoSegment *s, Alloc *a, Error *err) {
    if (s == NULL || s->sr.r.vt == NULL)
        macho_nil_panic();
    return burrow__saferio_read_data_at(a, io_section_reader_as_io_reader_at(&s->sr),
                                        s->segment_header.filesz, 0, err);
}

IoReadSeeker macho_segment_open(MachoSegment *s, Alloc *a) {
    if (s == NULL || s->sr.r.vt == NULL)
        macho_nil_panic();
    return burrow__objfile_open(a, &s->sr);
}

Int macho_segment_read_at(MachoSegment *s, Slice p, int64_t off, Error *err) {
    if (s == NULL || s->reader_at.vt == NULL)
        macho_nil_panic();
    return s->reader_at.vt->read_at(s->reader_at.data, p, off, err);
}

Slice macho_section_data(MachoSection *s, Alloc *a, Error *err) {
    if (s == NULL || s->sr.r.vt == NULL)
        macho_nil_panic();
    return burrow__saferio_read_data_at(a, io_section_reader_as_io_reader_at(&s->sr),
                                        s->section_header.size, 0, err);
}

IoReadSeeker macho_section_open(MachoSection *s, Alloc *a) {
    if (s == NULL || s->sr.r.vt == NULL)
        macho_nil_panic();
    return burrow__objfile_open(a, &s->sr);
}

Int macho_section_read_at(MachoSection *s, Slice p, int64_t off, Error *err) {
    if (s == NULL || s->reader_at.vt == NULL)
        macho_nil_panic();
    return s->reader_at.vt->read_at(s->reader_at.data, p, off, err);
}

void macho_reader_free(IoReadSeeker r) {
    burrow__objfile_reader_free(r);
}

/* ------------------------------------------------------------------- files */

/* What a NewFile in progress needs: the file, its byte order, the allocator
 * of the File's arena and one for what is thrown away. */
typedef struct MachoReader {
    MachoFile *f;
    IoReaderAt r;
    bool big;
    Alloc *fa;
    Alloc *tmp;
} MachoReader;

/* The error binary.Read gives for a struct of need bytes from a bytes.Reader
 * with left bytes still to read, or no error when they are enough. */
static Error macho_short(Int left, Int need) {
    if (left >= need)
        return BURROW_NO_ERROR;
    return left == 0 ? io_eof : io_err_unexpected_eof;
}

static Error macho_oom(void) {
    return burrow_err_out_of_memory;
}

/* pushSection. */
static Error macho_push_section(MachoReader *m, MachoSection *sh) {
    MachoFile *f = m->f;
    if (!macho_push(m->fa, &f->sections, sh))
        return macho_oom();
    sh->sr = io_new_section_reader(m->r, (int64_t)sh->section_header.offset,
                                   (int64_t)sh->section_header.size);
    sh->reader_at = io_section_reader_as_io_reader_at(&sh->sr);
    sh->relocs.elem = TYPE_MACHO_RELOC;
    uint32_t nreloc = sh->section_header.nreloc;
    if (nreloc == 0)
        return BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    Slice reldat = burrow__saferio_read_data_at(m->tmp, m->r, (uint64_t)nreloc * 8,
                                                (int64_t)sh->section_header.reloff, &e);
    if (BURROW_FAILED(e))
        return e;
    MachoReloc *rel = (MachoReloc *)mem_alloc(m->fa, (size_t)nreloc * sizeof *rel,
                                              _Alignof(MachoReloc));
    if (rel == NULL)
        return macho_oom();
    const Byte *p = (const Byte *)reldat.p;
    for (uint32_t i = 0; i < nreloc; i++, p += 8) {
        uint32_t addr = macho_u32(m->big, p);
        uint32_t symnum = macho_u32(m->big, p + 4);
        MachoReloc *r = &rel[i];
        if ((addr & (UINT32_C(1) << 31)) != 0) { /* scattered */
            r->addr = addr & ((UINT32_C(1) << 24) - 1);
            r->type = (uint8_t)((addr >> 24) & 0xfU);
            r->len = (uint8_t)((addr >> 28) & 3U);
            r->pcrel = (addr & (UINT32_C(1) << 30)) != 0;
            r->value = symnum;
            r->scattered = true;
        } else if (!m->big) {
            r->addr = addr;
            r->value = symnum & ((UINT32_C(1) << 24) - 1);
            r->pcrel = (symnum & (UINT32_C(1) << 24)) != 0;
            r->len = (uint8_t)((symnum >> 25) & 3U);
            r->extern_ = (symnum & (UINT32_C(1) << 27)) != 0;
            r->type = (uint8_t)((symnum >> 28) & 0xfU);
        } else {
            r->addr = addr;
            r->value = symnum >> 8;
            r->pcrel = (symnum & (UINT32_C(1) << 7)) != 0;
            r->len = (uint8_t)((symnum >> 5) & 3U);
            r->extern_ = (symnum & (UINT32_C(1) << 4)) != 0;
            r->type = (uint8_t)(symnum & 0xfU);
        }
    }
    Slice rs = {rel, (Int)nreloc, (Int)nreloc, TYPE_MACHO_RELOC};
    sh->relocs = rs;
    return BURROW_NO_ERROR;
}

static Str macho_cstr(MachoReader *m, const Byte *p, Int n, Error *e) {
    Str s = burrow__objfile_cstring(m->fa, p, n);
    if (s.len == 0 && n > 0 && p[0] != 0)
        *e = macho_oom();
    return s;
}

/* A segment command and the sections in it, 64-bit or not. */
static Error macho_segment(MachoReader *m, MachoLoadCmd cmd, Slice cmddat, bool is64,
                           MachoSegment **out) {
    const Byte *d = (const Byte *)cmddat.p;
    Int siz = cmddat.len;
    bool big = m->big;
    Int hsize = is64 ? MACHO_SEGMENT64_SIZE : MACHO_SEGMENT32_SIZE;
    Error e = macho_short(siz, hsize);
    if (BURROW_FAILED(e))
        return e;
    MachoSegment *s =
        (MachoSegment *)mem_alloc(m->fa, sizeof *s, _Alignof(MachoSegment));
    if (s == NULL)
        return macho_oom();
    s->load_bytes.kind = MACHO_LOAD_KIND_SEGMENT;
    s->load_bytes.raw = cmddat;
    MachoSegmentHeader *h = &s->segment_header;
    h->cmd = cmd;
    h->len = (uint32_t)siz;
    h->name = macho_cstr(m, d + 8, 16, &e);
    if (is64) {
        h->addr = macho_u64(big, d + 24);
        h->memsz = macho_u64(big, d + 32);
        h->offset = macho_u64(big, d + 40);
        h->filesz = macho_u64(big, d + 48);
        h->maxprot = macho_u32(big, d + 56);
        h->prot = macho_u32(big, d + 60);
        h->nsect = macho_u32(big, d + 64);
        h->flag = macho_u32(big, d + 68);
    } else {
        h->addr = macho_u32(big, d + 24);
        h->memsz = macho_u32(big, d + 28);
        h->offset = macho_u32(big, d + 32);
        h->filesz = macho_u32(big, d + 36);
        h->maxprot = macho_u32(big, d + 40);
        h->prot = macho_u32(big, d + 44);
        h->nsect = macho_u32(big, d + 48);
        h->flag = macho_u32(big, d + 52);
    }
    if (BURROW_FAILED(e) || !macho_push(m->fa, &m->f->loads, s))
        return macho_oom();
    *out = s;

    Int ssize = is64 ? MACHO_SECTION64_SIZE : MACHO_SECTION32_SIZE;
    Int pos = hsize;
    for (uint32_t i = 0; i < h->nsect; i++, pos += ssize) {
        e = macho_short(siz - pos, ssize);
        if (BURROW_FAILED(e))
            return e;
        const Byte *q = d + pos;
        MachoSection *sh =
            (MachoSection *)mem_alloc(m->fa, sizeof *sh, _Alignof(MachoSection));
        if (sh == NULL)
            return macho_oom();
        MachoSectionHeader *sv = &sh->section_header;
        sv->name = macho_cstr(m, q, 16, &e);
        sv->seg = macho_cstr(m, q + 16, 16, &e);
        if (BURROW_FAILED(e))
            return e;
        if (is64) {
            sv->addr = macho_u64(big, q + 32);
            sv->size = macho_u64(big, q + 40);
            q += 48;
        } else {
            sv->addr = macho_u32(big, q + 32);
            sv->size = macho_u32(big, q + 36);
            q += 40;
        }
        sv->offset = macho_u32(big, q);
        sv->align = macho_u32(big, q + 4);
        sv->reloff = macho_u32(big, q + 8);
        sv->nreloc = macho_u32(big, q + 12);
        sv->flags = macho_u32(big, q + 16);
        e = macho_push_section(m, sh);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

/* parseSymtab, and the reading of the tables before it. */
static Error macho_symtab(MachoReader *m, Slice cmddat, int64_t offset) {
    const Byte *d = (const Byte *)cmddat.p;
    bool big = m->big;
    Error e = macho_short(cmddat.len, MACHO_SYMTAB_CMD_SIZE);
    if (BURROW_FAILED(e))
        return e;
    uint32_t symoff = macho_u32(big, d + 8);
    uint32_t nsyms = macho_u32(big, d + 12);
    uint32_t stroff = macho_u32(big, d + 16);
    uint32_t strsize = macho_u32(big, d + 20);
    Slice strtab = burrow__saferio_read_data_at(m->tmp, m->r, strsize, stroff, &e);
    if (BURROW_FAILED(e))
        return e;
    bool is64 = m->f->file_header.magic == MACHO_MAGIC64;
    uint64_t symsz = is64 ? 16 : 12;
    Slice symdat =
        burrow__saferio_read_data_at(m->tmp, m->r, (uint64_t)nsyms * symsz, symoff, &e);
    if (BURROW_FAILED(e))
        return e;

    if (burrow__saferio_slice_cap(sizeof(MachoSymbol), nsyms) < 0)
        return macho_fe_nil(offset, BURROW_S("too many symbols"));
    MachoSymbol *syms = NULL;
    if (nsyms > 0) {
        syms = (MachoSymbol *)mem_alloc(m->fa, (size_t)nsyms * sizeof *syms,
                                        _Alignof(MachoSymbol));
        if (syms == NULL)
            return macho_oom();
    }
    const Byte *p = (const Byte *)symdat.p;
    const Byte *st = (const Byte *)strtab.p;
    for (uint32_t i = 0; i < nsyms; i++, p += symsz) {
        uint32_t name = macho_u32(big, p);
        MachoSymbol *s = &syms[i];
        s->type = p[4];
        s->sect = p[5];
        s->desc = macho_u16(big, p + 6);
        s->value = is64 ? macho_u64(big, p + 8) : macho_u32(big, p + 8);
        if (name >= (uint64_t)strtab.len)
            return macho_fe_u(offset, BURROW_S("invalid name in symbol table"), name);
        /* We add "_" to Go symbols. Strip it here. See issue 33808. */
        Str n = macho_cstr(m, st + name, strtab.len - (Int)name, &e);
        if (BURROW_FAILED(e))
            return e;
        if (strings_contains(n, BURROW_S(".")) && n.p[0] == '_')
            n = str_from_bytes(n.p + 1, n.len - 1);
        s->name = n;
    }
    MachoSymtab *t = (MachoSymtab *)mem_alloc(m->fa, sizeof *t, _Alignof(MachoSymtab));
    if (t == NULL || !macho_push(m->fa, &m->f->loads, t))
        return macho_oom();
    t->load_bytes.kind = MACHO_LOAD_KIND_SYMTAB;
    t->load_bytes.raw = cmddat;
    Slice ss = {syms, (Int)nsyms, (Int)nsyms, TYPE_MACHO_SYMBOL};
    t->syms = ss;
    m->f->symtab = t;
    return BURROW_NO_ERROR;
}

/* "...length (a > b)", for the two messages made with Sprintf. */
static Error macho_fe_len(int64_t offset, Str msg, uint64_t x, uint64_t y) {
    Byte b1[24];
    Byte b2[24];
    Str xs = macho_udec(b1, x);
    Str ys = macho_udec(b2, y);
    Byte buf[256];
    Int k = macho_put(buf, 0, msg);
    k = macho_put(buf, k, BURROW_S(" ("));
    k = macho_put(buf, k, xs);
    k = macho_put(buf, k, BURROW_S(" > "));
    k = macho_put(buf, k, ys);
    k = macho_put(buf, k, BURROW_S(")"));
    return macho_fe_nil(offset, str_from_bytes(buf, k));
}

static Error macho_dysymtab(MachoReader *m, Slice cmddat, int64_t offset) {
    const Byte *d = (const Byte *)cmddat.p;
    bool big = m->big;
    Error e = macho_short(cmddat.len, MACHO_DYSYMTAB_CMD_SIZE);
    if (BURROW_FAILED(e))
        return e;
    uint32_t v[20];
    for (size_t i = 0; i < 20; i++)
        v[i] = macho_u32(big, d + 4 * i);
    MachoDysymtabCmd hdr = {
        v[0],  v[1],  v[2],  v[3],  v[4],  v[5],  v[6],  v[7],  v[8],  v[9],
        v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19],
    };
    MachoSymtab *st = m->f->symtab;
    if (st == NULL)
        return macho_fe_nil(
            offset,
            BURROW_S("dynamic symbol table seen before any ordinary symbol table"));
    uint64_t nsyms = (uint64_t)st->syms.len;
    if (hdr.iundefsym > nsyms)
        return macho_fe_len(offset,
                            BURROW_S("undefined symbols index in dynamic symbol table "
                                     "command is greater than symbol table length"),
                            hdr.iundefsym, nsyms);
    uint32_t end = hdr.iundefsym + hdr.nundefsym;
    if (end > nsyms)
        return macho_fe_len(offset,
                            BURROW_S("number of undefined symbols after index in "
                                     "dynamic symbol table command is greater than "
                                     "symbol table length"),
                            end, nsyms);
    Slice dat = burrow__saferio_read_data_at(
        m->tmp, m->r, (uint64_t)hdr.nindirectsyms * 4, hdr.indirectsymoff, &e);
    if (BURROW_FAILED(e))
        return e;
    uint32_t *x = (uint32_t *)mem_alloc(
        m->fa, hdr.nindirectsyms == 0 ? 1 : (size_t)hdr.nindirectsyms * sizeof *x,
        _Alignof(uint32_t));
    if (x == NULL)
        return macho_oom();
    const Byte *p = (const Byte *)dat.p;
    for (uint32_t i = 0; i < hdr.nindirectsyms; i++)
        x[i] = macho_u32(big, p + 4 * (size_t)i);
    MachoDysymtab *t =
        (MachoDysymtab *)mem_alloc(m->fa, sizeof *t, _Alignof(MachoDysymtab));
    if (t == NULL || !macho_push(m->fa, &m->f->loads, t))
        return macho_oom();
    t->load_bytes.kind = MACHO_LOAD_KIND_DYSYMTAB;
    t->load_bytes.raw = cmddat;
    t->dysymtab_cmd = hdr;
    Slice is = {x, (Int)hdr.nindirectsyms, (Int)hdr.nindirectsyms, TYPE_UINT32};
    t->indirect_syms = is;
    m->f->dysymtab = t;
    return BURROW_NO_ERROR;
}

/* The load commands, after the file header. */
static Error macho_read_loads(MachoReader *m, int64_t offset) {
    MachoFile *f = m->f;
    bool big = m->big;
    Error e = BURROW_NO_ERROR;
    Slice dat =
        burrow__saferio_read_data_at(m->fa, m->r, f->file_header.cmdsz, offset, &e);
    if (BURROW_FAILED(e))
        return e;
    if (burrow__saferio_slice_cap(sizeof(MachoLoad) * 2, f->file_header.ncmd) < 0)
        return macho_fe_nil(offset, BURROW_S("too many load commands"));
    Byte *dp = (Byte *)dat.p;
    Int rest = dat.len;
    Int restcap = dat.cap;
    for (uint32_t i = 0; i < f->file_header.ncmd; i++) {
        /* Each load command begins with uint32 command and length. */
        if (rest < 8)
            return macho_fe_nil(offset, BURROW_S("command block too small"));
        MachoLoadCmd cmd = macho_u32(big, dp);
        uint32_t siz = macho_u32(big, dp + 4);
        if (siz < 8 || siz > (uint64_t)rest)
            return macho_fe_nil(offset, BURROW_S("invalid command block size"));
        Slice cmddat = {dp, (Int)siz, restcap, TYPE_BYTE};
        dp += siz;
        rest -= (Int)siz;
        restcap -= (Int)siz;
        offset += siz;
        const Byte *d = (const Byte *)cmddat.p;
        MachoSegment *s = NULL;
        switch (cmd) {
        case MACHO_LOAD_CMD_RPATH: {
            e = macho_short(cmddat.len, MACHO_RPATH_CMD_SIZE);
            if (BURROW_FAILED(e))
                return e;
            uint32_t path = macho_u32(big, d + 8);
            if (path >= siz)
                return macho_fe_u(offset, BURROW_S("invalid path in rpath command"),
                                  path);
            MachoRpath *l =
                (MachoRpath *)mem_alloc(m->fa, sizeof *l, _Alignof(MachoRpath));
            if (l == NULL)
                return macho_oom();
            l->path = macho_cstr(m, d + path, (Int)(siz - path), &e);
            l->load_bytes.kind = MACHO_LOAD_KIND_RPATH;
            l->load_bytes.raw = cmddat;
            if (BURROW_FAILED(e) || !macho_push(m->fa, &f->loads, l))
                return macho_oom();
            break;
        }
        case MACHO_LOAD_CMD_DYLIB: {
            e = macho_short(cmddat.len, MACHO_DYLIB_CMD_SIZE);
            if (BURROW_FAILED(e))
                return e;
            uint32_t name = macho_u32(big, d + 8);
            if (name >= siz)
                return macho_fe_u(
                    offset, BURROW_S("invalid name in dynamic library command"), name);
            MachoDylib *l =
                (MachoDylib *)mem_alloc(m->fa, sizeof *l, _Alignof(MachoDylib));
            if (l == NULL)
                return macho_oom();
            l->name = macho_cstr(m, d + name, (Int)(siz - name), &e);
            l->time = macho_u32(big, d + 12);
            l->current_version = macho_u32(big, d + 16);
            l->compat_version = macho_u32(big, d + 20);
            l->load_bytes.kind = MACHO_LOAD_KIND_DYLIB;
            l->load_bytes.raw = cmddat;
            if (BURROW_FAILED(e) || !macho_push(m->fa, &f->loads, l))
                return macho_oom();
            break;
        }
        case MACHO_LOAD_CMD_SYMTAB:
            e = macho_symtab(m, cmddat, offset);
            if (BURROW_FAILED(e))
                return e;
            break;
        case MACHO_LOAD_CMD_DYSYMTAB:
            e = macho_dysymtab(m, cmddat, offset);
            if (BURROW_FAILED(e))
                return e;
            break;
        case MACHO_LOAD_CMD_SEGMENT:
        case MACHO_LOAD_CMD_SEGMENT64:
            e = macho_segment(m, cmd, cmddat, cmd == MACHO_LOAD_CMD_SEGMENT64, &s);
            if (BURROW_FAILED(e))
                return e;
            break;
        default: {
            MachoLoadBytes *l =
                (MachoLoadBytes *)mem_alloc(m->fa, sizeof *l, _Alignof(MachoLoadBytes));
            if (l == NULL || !macho_push(m->fa, &f->loads, l))
                return macho_oom();
            l->kind = MACHO_LOAD_KIND_BYTES;
            l->raw = cmddat;
            break;
        }
        }
        if (s != NULL) {
            MachoSegmentHeader *h = &s->segment_header;
            if ((int64_t)h->offset < 0)
                return macho_fe_u(offset, BURROW_S("invalid section offset"),
                                  h->offset);
            if ((int64_t)h->filesz < 0)
                return macho_fe_u(offset, BURROW_S("invalid section file size"),
                                  h->filesz);
            s->sr = io_new_section_reader(m->r, (int64_t)h->offset, (int64_t)h->filesz);
            s->reader_at = io_section_reader_as_io_reader_at(&s->sr);
        }
    }
    return BURROW_NO_ERROR;
}

static void macho_file_release(MachoFile *f) {
    Alloc *a = f->a;
    arena_free(&f->arena);
    mem_free(a, f, sizeof *f, _Alignof(MachoFile));
}

MachoFile *macho_new_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        macho_nil_panic();
    /* Read and decode Mach magic to determine byte order, size. Magic32 and
     * Magic64 differ only in the bottom bit. */
    Byte ident[4];
    Error e = BURROW_NO_ERROR;
    r.vt->read_at(r.data, macho_bytes(ident, 4), 0, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    uint32_t be = macho_u32(true, ident);
    uint32_t le = macho_u32(false, ident);
    bool big;
    if ((be & ~1U) == (MACHO_MAGIC32 & ~1U)) {
        big = true;
    } else if ((le & ~1U) == (MACHO_MAGIC32 & ~1U)) {
        big = false;
    } else {
        BURROW_OUT(err, macho_fe_nil(0, BURROW_S("invalid magic number")));
        return NULL;
    }

    /* Read entire file header. */
    Byte hb[MACHO_FILE_HEADER_SIZE32];
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);
    io_read_full(io_section_reader_as_io_reader(&sr),
                 macho_bytes(hb, MACHO_FILE_HEADER_SIZE32), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    MachoFileHeader fh = {
        macho_u32(big, hb),      macho_u32(big, hb + 4),  macho_u32(big, hb + 8),
        macho_u32(big, hb + 12), macho_u32(big, hb + 16), macho_u32(big, hb + 20),
        macho_u32(big, hb + 24),
    };

    MachoFile *f = (MachoFile *)mem_alloc(a, sizeof *f, _Alignof(MachoFile));
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->a = a;
    arena_init(&f->arena, a, 0);
    f->file_header = fh;
    f->byte_order = big ? binary_big_endian : binary_little_endian;
    f->loads.elem = TYPE_UNSAFE_POINTER;
    f->sections.elem = TYPE_UNSAFE_POINTER;

    /* Then load commands. */
    int64_t offset =
        fh.magic == MACHO_MAGIC64 ? MACHO_FILE_HEADER_SIZE64 : MACHO_FILE_HEADER_SIZE32;
    Arena tmp;
    arena_init(&tmp, a, 0);
    MachoReader m = {f, r, big, arena_allocator(&f->arena), arena_allocator(&tmp)};
    e = macho_read_loads(&m, offset);
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        macho_file_release(f);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

MachoFile *macho_open(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    MachoFile *f = macho_new_file(a, os_file_as_io_reader_at(file), &e);
    if (f == NULL) {
        os_file_close(file);
        os_file_free(file);
        BURROW_OUT(err, e);
        return NULL;
    }
    f->file = file;
    f->open = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

Error macho_file_close(MachoFile *f) {
    if (f == NULL)
        macho_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (f->open) {
        e = os_file_close(f->file);
        f->open = false;
    }
    return e;
}

void macho_file_free(MachoFile *f) {
    if (f == NULL)
        return;
    OsFile *file = f->file;
    if (f->open)
        os_file_close(file);
    os_file_free(file);
    macho_file_release(f);
}

MachoSegment *macho_file_segment(MachoFile *f, Str name) {
    if (f == NULL)
        macho_nil_panic();
    for (Int i = 0; i < f->loads.len; i++) {
        MachoLoad l = ((MachoLoad *)f->loads.p)[i];
        if (l->kind != MACHO_LOAD_KIND_SEGMENT)
            continue;
        MachoSegment *s = (MachoSegment *)(void *)l;
        if (str_eq(s->segment_header.name, name))
            return s;
    }
    return NULL;
}

MachoSection *macho_file_section(MachoFile *f, Str name) {
    if (f == NULL)
        macho_nil_panic();
    for (Int i = 0; i < f->sections.len; i++) {
        MachoSection *s = ((MachoSection **)f->sections.p)[i];
        if (str_eq(s->section_header.name, name))
            return s;
    }
    return NULL;
}

/* ------------------------------------------------------------------ dwarf */

/* dwarfSuffix: the part of a DWARF section's name after "__debug_" or
 * "__zdebug_", or nothing for another section. */
static Str macho_dwarf_suffix(const MachoSection *s) {
    Str name = s->section_header.name;
    Int pfx;
    if (strings_has_prefix(name, BURROW_S("__debug_")))
        pfx = 8;
    else if (strings_has_prefix(name, BURROW_S("__zdebug_")))
        pfx = 9;
    else
        return BURROW_STR_EMPTY;
    /* Mach-O executables truncate section names to 16 characters, mangling
     * some DWARF sections. As of DWARFv5 these are the only problematic
     * section names (see DWARFv5 Appendix G). */
    static const Str longnames[] = {
        BURROW_S_INIT("__debug_str_offsets"),  BURROW_S_INIT("__zdebug_line_str"),
        BURROW_S_INIT("__zdebug_loclists"),    BURROW_S_INIT("__zdebug_pubnames"),
        BURROW_S_INIT("__zdebug_pubtypes"),    BURROW_S_INIT("__zdebug_rnglists"),
        BURROW_S_INIT("__zdebug_str_offsets"),
    };
    for (Int k = 0; k < MACHO_NNAME(longnames); k++) {
        if (str_eq(name, str_from_bytes(longnames[k].p, 16))) {
            name = longnames[k];
            break;
        }
    }
    return str_from_bytes(name.p + pfx, name.len - pfx);
}

/* sectionData. */
static Slice macho_dwarf_section_data(MachoSection *s, Alloc *a, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Error e = BURROW_NO_ERROR;
    Slice b = macho_section_data(s, a, &e);
    if (BURROW_FAILED(e) && (uint64_t)b.len < s->section_header.size) {
        BURROW_OUT(err, e);
        return none;
    }
    return burrow__objfile_unzlib(a, b, err);
}

/* The sections the debug/dwarf package started with, which dwarf_new takes. */
enum {
    MACHO_DW_ABBREV,
    MACHO_DW_INFO,
    MACHO_DW_STR,
    MACHO_DW_LINE,
    MACHO_DW_RANGES,
    MACHO_DW_N
};

static int macho_dwarf_slot(Str suffix) {
    static const char *const names[MACHO_DW_N] = {"abbrev", "info", "str", "line",
                                                  "ranges"};
    for (int k = 0; k < MACHO_DW_N; k++) {
        if (str_eq(suffix, str_from_cstr(names[k])))
            return k;
    }
    return -1;
}

DwarfData *macho_file_dwarf(MachoFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        macho_nil_panic();
    Slice dat[MACHO_DW_N];
    for (int k = 0; k < MACHO_DW_N; k++) {
        Slice none = {NULL, 0, 0, TYPE_BYTE};
        dat[k] = none;
    }
    Error e = BURROW_NO_ERROR;
    MachoSection **secs = (MachoSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = macho_dwarf_suffix(secs[i]);
        if (suffix.len == 0)
            continue;
        int k = macho_dwarf_slot(suffix);
        if (k < 0)
            continue;
        Slice b = macho_dwarf_section_data(secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        dat[k] = b;
    }

    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    DwarfData *d =
        dwarf_new(a, dat[MACHO_DW_ABBREV], nil, nil, dat[MACHO_DW_INFO],
                  dat[MACHO_DW_LINE], nil, dat[MACHO_DW_RANGES], dat[MACHO_DW_STR], &e);
    if (d == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }

    /* Look for DWARF4 .debug_types sections and DWARF5 sections. */
    for (Int i = 0; i < f->sections.len; i++) {
        Str suffix = macho_dwarf_suffix(secs[i]);
        if (suffix.len == 0 || macho_dwarf_slot(suffix) >= 0)
            continue;
        Slice b = macho_dwarf_section_data(secs[i], a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        bool types = str_eq(suffix, BURROW_S("types"));
        Byte num[24];
        Str name = types
                       ? macho_concat(a, BURROW_S("types-"), macho_idec(num, i),
                                      BURROW_STR_EMPTY)
                       : macho_concat(a, BURROW_S(".debug_"), suffix, BURROW_STR_EMPTY);
        if (name.len == 0) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        e = types ? dwarf_data_add_types(d, name, b)
                  : dwarf_data_add_section(d, name, b);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d;
}

/* ---------------------------------------------------------------- imports */

/* Panics as Go does for s[lo:hi] out of range of a slice with capacity cap. */
static void macho_slice_panic(uint64_t lo, uint64_t hi, uint64_t cap) {
    Byte b1[24];
    Byte b2[24];
    Byte buf[96];
    Int k = macho_put(buf, 0, BURROW_S("runtime error: slice bounds out of range ["));
    if (hi > cap) {
        k = macho_put(buf, k, BURROW_S(":"));
        k = macho_put(buf, k, macho_udec(b1, hi));
        k = macho_put(buf, k, BURROW_S("] with capacity "));
        k = macho_put(buf, k, macho_udec(b2, cap));
    } else {
        k = macho_put(buf, k, macho_udec(b1, lo));
        k = macho_put(buf, k, BURROW_S(":"));
        k = macho_put(buf, k, macho_udec(b2, hi));
        k = macho_put(buf, k, BURROW_S("]"));
    }
    runtime_panic(str_from_bytes(buf, k));
}

Slice macho_file_imported_symbols(MachoFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        macho_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_STRING};
    MachoSymtab *st = f->symtab;
    if (st == NULL) {
        BURROW_OUT(err, macho_fe_nil(0, BURROW_S("missing symbol table")));
        return none;
    }
    const MachoSymbol *syms = (const MachoSymbol *)st->syms.p;
    uint64_t lo = 0;
    uint64_t hi = (uint64_t)st->syms.len;
    MachoDysymtab *dt = f->dysymtab;
    if (dt != NULL) {
        lo = dt->dysymtab_cmd.iundefsym;
        /* A uint32 sum, so it wraps as Go's does. */
        hi = dt->dysymtab_cmd.iundefsym + dt->dysymtab_cmd.nundefsym;
        if (hi > (uint64_t)st->syms.cap || lo > hi)
            macho_slice_panic(lo, hi, (uint64_t)st->syms.cap);
    }
    /* From Darwin's include/mach-o/nlist.h. */
    enum { N_TYPE = 0x0e, N_UNDF = 0x0, N_EXT = 0x01 };
    Int n = 0;
    for (uint64_t i = lo; i < hi; i++) {
        if (dt != NULL || ((syms[i].type & N_TYPE) == N_UNDF && (syms[i].type & N_EXT)))
            n++;
    }
    if (n == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return none;
    }
    Str *all = (Str *)mem_alloc(a, (size_t)n * sizeof *all, _Alignof(Str));
    if (all == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    Int k = 0;
    for (uint64_t i = lo; i < hi; i++) {
        if (dt != NULL || ((syms[i].type & N_TYPE) == N_UNDF && (syms[i].type & N_EXT)))
            all[k++] = syms[i].name;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {all, n, n, TYPE_STRING};
    return out;
}

Slice macho_file_imported_libraries(MachoFile *f, Alloc *a, Error *err) {
    if (f == NULL)
        macho_nil_panic();
    Slice none = {NULL, 0, 0, TYPE_STRING};
    MachoLoad *loads = (MachoLoad *)f->loads.p;
    Int n = 0;
    for (Int i = 0; i < f->loads.len; i++)
        n += loads[i]->kind == MACHO_LOAD_KIND_DYLIB;
    if (n == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return none;
    }
    Str *all = (Str *)mem_alloc(a, (size_t)n * sizeof *all, _Alignof(Str));
    if (all == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    Int k = 0;
    for (Int i = 0; i < f->loads.len; i++) {
        if (loads[i]->kind == MACHO_LOAD_KIND_DYLIB)
            all[k++] = ((MachoDylib *)(void *)loads[i])->name;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {all, n, n, TYPE_STRING};
    return out;
}

/* ---------------------------------------------------------- universal files */

/* The architectures seen so far, a set of cpu<<32|subcpu. */
typedef struct MachoSeen {
    uint64_t *keys;
    Byte *used;
    Int cap;
    Int n;
} MachoSeen;

static uint64_t macho_seen_hash(uint64_t k) {
    k ^= k >> 33;
    k *= UINT64_C(0xff51afd7ed558ccd);
    k ^= k >> 33;
    return k;
}

/* Whether k was in the set, adding it when it was not. -1 when out of
 * memory. */
static int macho_seen_add(MachoSeen *s, Alloc *a, uint64_t k) {
    if (2 * (s->n + 1) > s->cap) {
        Int ncap = s->cap == 0 ? 16 : 2 * s->cap;
        uint64_t *keys =
            (uint64_t *)mem_alloc(a, (size_t)ncap * sizeof *keys, _Alignof(uint64_t));
        Byte *used = (Byte *)mem_alloc(a, (size_t)ncap, 1);
        if (keys == NULL || used == NULL)
            return -1;
        for (Int i = 0; i < s->cap; i++) {
            if (!s->used[i])
                continue;
            uint64_t j = macho_seen_hash(s->keys[i]) & (uint64_t)(ncap - 1);
            while (used[j])
                j = (j + 1) & (uint64_t)(ncap - 1);
            keys[j] = s->keys[i];
            used[j] = 1;
        }
        s->keys = keys;
        s->used = used;
        s->cap = ncap;
    }
    uint64_t j = macho_seen_hash(k) & (uint64_t)(s->cap - 1);
    while (s->used[j]) {
        if (s->keys[j] == k)
            return 1;
        j = (j + 1) & (uint64_t)(s->cap - 1);
    }
    s->keys[j] = k;
    s->used[j] = 1;
    s->n++;
    return 0;
}

/* %#x of a Type, which is a Stringer, so the hex of the bytes of its name. */
static Int macho_put_sharp_x(Byte *p, Int k, MachoType t, Alloc *a) {
    Str s = macho_type_string(t, a);
    k = macho_put(p, k, BURROW_S("0x"));
    for (Int i = 0; i < s.len; i++) {
        p[k++] = (Byte) "0123456789abcdef"[s.p[i] >> 4];
        p[k++] = (Byte) "0123456789abcdef"[s.p[i] & 15];
    }
    return k;
}

static void macho_fat_release(MachoFatFile *ff) {
    MachoFatArch *arches = (MachoFatArch *)ff->arches.p;
    for (Int i = 0; i < ff->arches.len; i++)
        macho_file_free(arches[i].file);
    Alloc *a = ff->a;
    arena_free(&ff->arena);
    mem_free(a, ff, sizeof *ff, _Alignof(MachoFatFile));
}

/* The part of NewFatFile after the magic number. */
static Error macho_read_fat(MachoFatFile *ff, IoReaderAt r, IoSectionReader *sr,
                            Alloc *tmp) {
    Alloc *al = arena_allocator(&ff->arena);
    IoReader rd = io_section_reader_as_io_reader(sr);
    int64_t offset = 4;

    /* Read the number of FatArchHeaders that come after the fat_header. */
    Byte b[MACHO_FAT_ARCH_HEADER_SIZE];
    Error e = BURROW_NO_ERROR;
    io_read_full(rd, macho_bytes(b, 4), &e);
    if (BURROW_FAILED(e))
        return macho_fe_nil(offset, BURROW_S("invalid fat_header"));
    uint32_t narch = macho_u32(true, b);
    offset += 4;
    if (narch < 1)
        return macho_fe_nil(offset, BURROW_S("file contains no images"));

    /* Combine the Cpu and SubCpu (both uint32) into a uint64 to make sure
     * there are not duplicate architectures. */
    MachoSeen seen = {NULL, NULL, 0, 0};
    /* Make sure that all images are for the same MH_ type. */
    MachoType macho_type = 0;

    /* Following the fat_header comes narch fat_arch structs that index Mach-O
     * images further in the file. */
    if (burrow__saferio_slice_cap(sizeof(MachoFatArch), narch) < 0)
        return macho_fe_nil(offset, BURROW_S("too many images"));
    for (uint32_t i = 0; i < narch; i++) {
        io_read_full(rd, macho_bytes(b, MACHO_FAT_ARCH_HEADER_SIZE), &e);
        if (BURROW_FAILED(e))
            return macho_fe_nil(offset, BURROW_S("invalid fat_arch header"));
        MachoFatArch fa = {{macho_u32(true, b), macho_u32(true, b + 4),
                            macho_u32(true, b + 8), macho_u32(true, b + 12),
                            macho_u32(true, b + 16)},
                           NULL};
        offset += MACHO_FAT_ARCH_HEADER_SIZE;

        IoSectionReader *fr =
            (IoSectionReader *)mem_alloc(al, sizeof *fr, _Alignof(IoSectionReader));
        if (fr == NULL)
            return burrow_err_out_of_memory;
        *fr = io_new_section_reader(r, (int64_t)fa.fat_arch_header.offset,
                                    (int64_t)fa.fat_arch_header.size);
        fa.file = macho_new_file(ff->a, io_section_reader_as_io_reader_at(fr), &e);
        if (fa.file == NULL)
            return e;
        Slice out = slice_append(al, ff->arches, &fa, 1);
        if (out.len != ff->arches.len + 1) {
            macho_file_free(fa.file);
            return burrow_err_out_of_memory;
        }
        ff->arches = out;

        /* Make sure the architecture for this image is not duplicate. */
        const MachoFatArchHeader *h = &fa.fat_arch_header;
        uint64_t seen_arch = (uint64_t)h->cpu << 32 | h->sub_cpu;
        int dup = macho_seen_add(&seen, tmp, seen_arch);
        if (dup < 0)
            return burrow_err_out_of_memory;
        if (dup > 0) {
            Byte buf[256];
            Byte num[24];
            Int k = macho_put(buf, 0, BURROW_S("duplicate architecture cpu="));
            k = macho_put(buf, k, macho_cpu_string(h->cpu, tmp));
            k = macho_put(buf, k, BURROW_S(", subcpu=0x"));
            Int nh = 0;
            uint32_t v = h->sub_cpu;
            do {
                num[nh++] = (Byte) "0123456789abcdef"[v & 15];
                v >>= 4;
            } while (v != 0);
            while (nh > 0)
                buf[k++] = num[--nh];
            return macho_fe_nil(offset, str_from_bytes(buf, k));
        }

        /* Make sure the Mach-O type matches that of the first image. */
        MachoType t = fa.file->file_header.type;
        if (i == 0) {
            macho_type = t;
        } else if (t != macho_type) {
            Byte buf[256];
            Byte num[24];
            Int k = macho_put(buf, 0, BURROW_S("Mach-O type for architecture #"));
            k = macho_put(buf, k, macho_udec(num, i));
            k = macho_put(buf, k, BURROW_S(" (type="));
            k = macho_put_sharp_x(buf, k, t, tmp);
            k = macho_put(buf, k, BURROW_S(") does not match first (type="));
            k = macho_put_sharp_x(buf, k, macho_type, tmp);
            k = macho_put(buf, k, BURROW_S(")"));
            return macho_fe_nil(offset, str_from_bytes(buf, k));
        }
    }
    return BURROW_NO_ERROR;
}

MachoFatFile *macho_new_fat_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        macho_nil_panic();
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);

    /* Read the fat_header struct, which is always in big endian. Start with
     * the magic number. */
    Byte b[4];
    Error e = BURROW_NO_ERROR;
    io_read_full(io_section_reader_as_io_reader(&sr), macho_bytes(b, 4), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, macho_fe_nil(0, BURROW_S("error reading magic number")));
        return NULL;
    }
    uint32_t magic = macho_u32(true, b);
    if (magic != MACHO_MAGIC_FAT) {
        /* See if this is a Mach-O file via its magic number. The magic must
         * be converted to little endian first though. */
        uint32_t le_magic = macho_u32(false, b);
        if (le_magic == MACHO_MAGIC32 || le_magic == MACHO_MAGIC64)
            BURROW_OUT(err, macho_err_not_fat);
        else
            BURROW_OUT(err, macho_fe_nil(0, BURROW_S("invalid magic number")));
        return NULL;
    }

    MachoFatFile *ff = (MachoFatFile *)mem_alloc(a, sizeof *ff, _Alignof(MachoFatFile));
    if (ff == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    ff->a = a;
    arena_init(&ff->arena, a, 0);
    ff->magic = magic;
    ff->arches.elem = TYPE_MACHO_FAT_ARCH;

    Arena tmp;
    arena_init(&tmp, a, 0);
    e = macho_read_fat(ff, r, &sr, arena_allocator(&tmp));
    arena_free(&tmp);
    if (BURROW_FAILED(e)) {
        macho_fat_release(ff);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ff;
}

MachoFatFile *macho_open_fat(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    MachoFatFile *ff = macho_new_fat_file(a, os_file_as_io_reader_at(file), &e);
    if (ff == NULL) {
        os_file_close(file);
        os_file_free(file);
        BURROW_OUT(err, e);
        return NULL;
    }
    ff->file = file;
    ff->open = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ff;
}

Error macho_fat_file_close(MachoFatFile *ff) {
    if (ff == NULL)
        macho_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (ff->open) {
        e = os_file_close(ff->file);
        ff->open = false;
    }
    return e;
}

void macho_fat_file_free(MachoFatFile *ff) {
    if (ff == NULL)
        return;
    OsFile *file = ff->file;
    if (ff->open)
        os_file_close(file);
    os_file_free(file);
    macho_fat_release(ff);
}

static MachoFile *macho_fat_arch_file(MachoFatArch *fa) {
    if (fa == NULL || fa->file == NULL)
        macho_nil_panic();
    return fa->file;
}

Error macho_fat_arch_close(MachoFatArch *fa) {
    return macho_file_close(macho_fat_arch_file(fa));
}

MachoSegment *macho_fat_arch_segment(MachoFatArch *fa, Str name) {
    return macho_file_segment(macho_fat_arch_file(fa), name);
}

MachoSection *macho_fat_arch_section(MachoFatArch *fa, Str name) {
    return macho_file_section(macho_fat_arch_file(fa), name);
}

DwarfData *macho_fat_arch_dwarf(MachoFatArch *fa, Alloc *a, Error *err) {
    return macho_file_dwarf(macho_fat_arch_file(fa), a, err);
}

Slice macho_fat_arch_imported_symbols(MachoFatArch *fa, Alloc *a, Error *err) {
    return macho_file_imported_symbols(macho_fat_arch_file(fa), a, err);
}

Slice macho_fat_arch_imported_libraries(MachoFatArch *fa, Alloc *a, Error *err) {
    return macho_file_imported_libraries(macho_fat_arch_file(fa), a, err);
}
