/* debug/dwarf, reading the DWARF debugging information in an executable.
 *
 * DWARF is the format compilers write debugging information in: a tree of
 * entries describing the program's compilation units, functions, variables
 * and types, and a line table for each unit that maps machine addresses back to
 * source lines. This reads DWARF 2 to 5 from the raw section contents. Getting
 * those out of a file is the object file package's job, and elf_file_dwarf does
 * it for an ELF file.
 *
 *     Error err = BURROW_NO_ERROR;
 *     DwarfData *d = elf_file_dwarf(f, a, &err);
 *     DwarfReader *r = dwarf_data_reader(d, a);
 *     for (DwarfEntry *e; (e = dwarf_reader_next(r, &err)) != NULL;) {
 *         if (e->tag != DWARF_TAG_SUBPROGRAM)
 *             continue;
 *         Str *name = any_assert(dwarf_entry_val(e, DWARF_ATTR_NAME), TYPE_STRING);
 *         ...
 *     }
 *
 * A DwarfData keeps the allocator it was made with. The units, abbreviation
 * tables and types it reads go into it as they are first needed and stay until
 * it is freed, as do the errors it gives back, so it wants an arena. A reader
 * takes an allocator of its own for the entries it returns, which can be the
 * same one.
 *
 * Go's Type is an interface that the type structs below all satisfy. Here it is
 * a pointer to the DwarfCommonType at the start of each of them, and type_kind
 * in it says which struct it is, which is what a type switch looks at in Go:
 *
 *     DwarfType t = dwarf_data_type(d, off, &err);
 *     if (t->type_kind == DWARF_TYPE_KIND_TYPEDEF) {
 *         DwarfTypedefType *td = (DwarfTypedefType *)t;
 *         ...
 *     }
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/dwarf */

#ifndef BURROW_DEBUG_DWARF_H
#define BURROW_DEBUG_DWARF_H

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#include "burrow/debug/dwarf_const.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- constants */

/* dwarf.Attr, the attribute of a DwarfField. The constants are in
 * burrow/debug/dwarf_const.h. */
typedef uint32_t DwarfAttr;
extern const Type burrow_type_DwarfAttr;
#define TYPE_DWARF_ATTR TYPE_OF(DwarfAttr)

/* The name without the Attr, such as "Name", or "Attr(N)" for a value with no
 * name, which is the only case that needs a. */
BURROW_OWNS(ret) Str dwarf_attr_string(DwarfAttr i, Alloc *a);

/* "dwarf.AttrName", or "dwarf.Attr(N)". */
BURROW_OWNS(ret) Str dwarf_attr_go_string(DwarfAttr i, Alloc *a);

/* dwarf.Tag, the kind of a DwarfEntry. The constants are in
 * burrow/debug/dwarf_const.h. */
typedef uint32_t DwarfTag;
extern const Type burrow_type_DwarfTag;
#define TYPE_DWARF_TAG TYPE_OF(DwarfTag)

/* The name without the Tag, such as "Subprogram", or "Tag(N)". */
BURROW_OWNS(ret) Str dwarf_tag_string(DwarfTag i, Alloc *a);

/* "dwarf.TagSubprogram" up to DWARF_TAG_TEMPLATE_ALIAS, and "dwarf." and the
 * name after it, which is how Go's GoString has it. */
BURROW_OWNS(ret) Str dwarf_tag_go_string(DwarfTag i, Alloc *a);

/* dwarf.Class, the DWARF 4 class of an attribute's value. It says which C type
 * DwarfField.val holds:
 *
 *     DWARF class       C type          Class
 *     -----------       ------          -----
 *     address           uint64_t        DWARF_CLASS_ADDRESS
 *     block             Slice of Byte   DWARF_CLASS_BLOCK
 *     constant          int64_t         DWARF_CLASS_CONSTANT
 *     flag              bool            DWARF_CLASS_FLAG
 *     reference
 *       to info         DwarfOffset     DWARF_CLASS_REFERENCE
 *       to type unit    uint64_t        DWARF_CLASS_REFERENCE_SIG
 *     string            Str             DWARF_CLASS_STRING
 *     exprloc           Slice of Byte   DWARF_CLASS_EXPR_LOC
 *     lineptr           int64_t         DWARF_CLASS_LINE_PTR
 *     loclistptr        int64_t         DWARF_CLASS_LOC_LIST_PTR
 *     macptr            int64_t         DWARF_CLASS_MAC_PTR
 *     rangelistptr      int64_t         DWARF_CLASS_RANGE_LIST_PTR
 *
 * An attribute that is unknown or a vendor's own may have DWARF_CLASS_UNKNOWN.
 * DWARF 2 and 3 used constant for section offsets too, and the reader turns
 * those into the right *_PTR class from the attribute. */
typedef Int DwarfClass;
extern const Type burrow_type_DwarfClass;
#define TYPE_DWARF_CLASS TYPE_OF(DwarfClass)

enum {
    DWARF_CLASS_UNKNOWN,         /* a value of unknown class */
    DWARF_CLASS_ADDRESS,         /* uint64_t, an address on the target */
    DWARF_CLASS_BLOCK,           /* bytes whose meaning depends on the attribute */
    DWARF_CLASS_CONSTANT,        /* int64_t, a constant */
    DWARF_CLASS_EXPR_LOC,        /* bytes of a DWARF expression or location */
    DWARF_CLASS_FLAG,            /* bool */
    DWARF_CLASS_LINE_PTR,        /* int64_t, an offset into the line section */
    DWARF_CLASS_LOC_LIST_PTR,    /* int64_t, an offset into the loclist section */
    DWARF_CLASS_MAC_PTR,         /* int64_t, an offset into the mac section */
    DWARF_CLASS_RANGE_LIST_PTR,  /* int64_t, an offset into the rangelist section */
    DWARF_CLASS_REFERENCE,       /* DwarfOffset, of an entry in the info section */
    DWARF_CLASS_REFERENCE_SIG,   /* uint64_t, the signature of a type entry */
    DWARF_CLASS_STRING,          /* Str, UTF-8 if the unit has DWARF_ATTR_USE_UTF8 */
    DWARF_CLASS_REFERENCE_ALT,   /* int64_t, an offset into another file's info */
    DWARF_CLASS_STRING_ALT,      /* int64_t, an offset into another file's strings */
    DWARF_CLASS_ADDR_PTR,        /* int64_t, an offset into the addr section */
    DWARF_CLASS_LOC_LIST,        /* int64_t, an offset into the loclists section */
    DWARF_CLASS_RNG_LIST,        /* uint64_t, from the start of rnglists */
    DWARF_CLASS_RNG_LISTS_PTR,   /* int64_t, a base for DWARF_CLASS_RNG_LIST */
    DWARF_CLASS_STR_OFFSETS_PTR, /* int64_t, an offset into str_offsets */
};

/* "ClassAddress" and so on, with the Class, or "Class(N)". */
BURROW_OWNS(ret) Str dwarf_class_string(DwarfClass i, Alloc *a);

/* "dwarf.ClassAddress", or "dwarf.Class(N)". */
BURROW_OWNS(ret) Str dwarf_class_go_string(DwarfClass i, Alloc *a);

/* dwarf.Offset, where an entry is in the info section. */
typedef uint32_t DwarfOffset;
extern const Type burrow_type_DwarfOffset;
#define TYPE_DWARF_OFFSET TYPE_OF(DwarfOffset)

/* ------------------------------------------------------------------ errors */

/* dwarf.DecodeError, what every malformed section gives. The DwarfData's
 * readers return it as an Error, which errors_as with TYPE_DWARF_DECODE_ERROR
 * gets back. */
typedef struct DwarfDecodeError {
    Str name;           /* the section, such as "info" */
    DwarfOffset offset; /* where in it */
    Str err;            /* what was wrong */
} DwarfDecodeError;

extern const Type burrow_type_DwarfDecodeError;
#define TYPE_DWARF_DECODE_ERROR TYPE_OF(DwarfDecodeError)

/* "decoding dwarf section info at offset 0x1c: too short", in a. */
BURROW_OWNS(ret) Str dwarf_decode_error_error(DwarfDecodeError e, Alloc *a);

/* dwarf.ErrUnknownPC, which the SeekPC functions give for a pc nothing
 * covers. */
extern const Error dwarf_err_unknown_pc;

/* -------------------------------------------------------------------- data */

/* dwarf.Data, the DWARF of one executable. */
typedef struct DwarfData DwarfData;

/* dwarf.New. Each Slice is the contents of the section of that name, such as
 * .debug_abbrev for abbrev, and may be empty if the file has no such section,
 * except for info. They are borrowed, and have to outlive the result. The
 * object file packages call this for you.
 *
 * Only the info section is read at once, to find the units in it and the byte
 * order, and a mistake there is the error you get, with NULL. */
BURROW_OWNS(ret) DwarfData *dwarf_new(Alloc *a, Slice abbrev, Slice aranges,
                                      Slice frame, Slice info, Slice line,
                                      Slice pubnames, Slice ranges, Slice str,
                                      Error *err);

/* Data.AddTypes: one .debug_types section of DWARF 4. A file can have several,
 * and name tells them apart in errors. types is borrowed as in dwarf_new. */
BURROW_STATIC(ret) Error dwarf_data_add_types(DwarfData *d, Str name, Slice types);

/* Data.AddSection: a section DWARF 5 added, by its name, which is one of
 * .debug_addr, .debug_line_str, .debug_str_offsets and .debug_rnglists. Other
 * names are ignored. contents is borrowed as in dwarf_new. */
BURROW_STATIC(ret) Error dwarf_data_add_section(DwarfData *d, Str name, Slice contents);

/* ------------------------------------------------------------------ entries */

/* dwarf.Field, one attribute of an entry and its value. val holds the C type
 * the class table above gives for cls, and points into the entry, so it lives
 * as long as the entry does. */
typedef struct DwarfField {
    DwarfAttr attr;
    Any val;
    DwarfClass cls; /* Go's Class, which is a keyword in C++ */
} DwarfField;

extern const Type burrow_type_DwarfField;
#define TYPE_DWARF_FIELD TYPE_OF(DwarfField)

/* dwarf.Entry, one node of the tree in the info section. */
typedef struct DwarfEntry {
    DwarfOffset offset; /* where it is in the info section */
    DwarfTag tag;       /* what kind of entry it is */
    bool children;      /* whether entries for its children follow it */
    Slice field;        /* of DwarfField */
} DwarfEntry;

/* Entry.Val: the value of attribute a, or a nil Any if e has none. Go's
 * e.Val(AttrName).(string) is
 *
 *     Str *name = any_assert(dwarf_entry_val(e, DWARF_ATTR_NAME), TYPE_STRING);
 *
 * which is NULL for a missing attribute and for one of another type alike. */
BURROW_BORROWS(ret, e) Any dwarf_entry_val(const DwarfEntry *e, DwarfAttr a);

/* Entry.AttrField: the field for attribute a, or NULL. */
BURROW_BORROWS(ret, e) DwarfField *dwarf_entry_attr_field(const DwarfEntry *e,
                                                          DwarfAttr a);

/* dwarf.Reader, which reads the entries of the info section in the order of a
 * walk of the tree, each node before its children. An entry with children is
 * followed by them and then by an entry with tag 0. */
typedef struct DwarfReader DwarfReader;

/* Data.Reader, at the start of the info section. The reader and the entries it
 * returns are made in a. */
BURROW_OWNS(ret) DwarfReader *dwarf_data_reader(DwarfData *d, Alloc *a);

/* Reader.Next: the next entry, or NULL at the end of the section with *err
 * left alone. A bad offset or an entry that cannot be decoded is NULL with the
 * error, and the reader gives that error from then on until it seeks. */
BURROW_OWNS(ret) DwarfEntry *dwarf_reader_next(DwarfReader *r, Error *err);

/* Reader.Seek: go to the entry at off in the info section. 0 is the first. */
void dwarf_reader_seek(DwarfReader *r, DwarfOffset off);

/* Reader.SkipChildren: skip the children of the last entry Next returned, if
 * it has any. */
void dwarf_reader_skip_children(DwarfReader *r);

/* Reader.AddressSize: how many bytes an address is in the current unit. */
Int dwarf_reader_address_size(DwarfReader *r);

/* Reader.ByteOrder: the byte order of the current unit. */
BinaryByteOrder dwarf_reader_byte_order(DwarfReader *r);

/* Reader.SeekPC: the compilation unit entry that covers pc, with the reader
 * left to read its children. dwarf_err_unknown_pc if no unit does, and then
 * where the reader is is not defined. Each call starts looking at the unit the
 * last one found, so a run of sorted pcs goes fastest. */
BURROW_OWNS(ret) DwarfEntry *dwarf_reader_seek_pc(DwarfReader *r, uint64_t pc,
                                                  Error *err);

/* PC ranges, Go's [2]uint64: v[0] is the first address and v[1] the one after
 * the last. */
BURROW_ARRAY_TYPE_DECL(DwarfPCRange, uint64_t, 2);

/* Data.Ranges: the PC ranges e covers, a Slice of DwarfPCRange made in a. Only
 * some entries, such as compilation units and subprograms, have any, and for
 * the others the Slice is empty with no error. */
BURROW_OWNS(ret) Slice dwarf_data_ranges(DwarfData *d, Alloc *a, const DwarfEntry *e,
                                         Error *err);

/* -------------------------------------------------------------------- types */

/* Which type struct a DwarfType is. Go has no such thing, and asks with a type
 * switch instead. */
typedef enum DwarfTypeKind {
    DWARF_TYPE_KIND_COMMON = 0, /* a bare DwarfCommonType */
    DWARF_TYPE_KIND_ADDR,
    DWARF_TYPE_KIND_ARRAY,
    DWARF_TYPE_KIND_BASIC,
    DWARF_TYPE_KIND_BOOL,
    DWARF_TYPE_KIND_CHAR,
    DWARF_TYPE_KIND_COMPLEX,
    DWARF_TYPE_KIND_DOT_DOT_DOT,
    DWARF_TYPE_KIND_ENUM,
    DWARF_TYPE_KIND_FLOAT,
    DWARF_TYPE_KIND_FUNC,
    DWARF_TYPE_KIND_INT,
    DWARF_TYPE_KIND_PTR,
    DWARF_TYPE_KIND_QUAL,
    DWARF_TYPE_KIND_STRUCT,
    DWARF_TYPE_KIND_TYPEDEF,
    DWARF_TYPE_KIND_UCHAR,
    DWARF_TYPE_KIND_UINT,
    DWARF_TYPE_KIND_UNSPECIFIED,
    DWARF_TYPE_KIND_UNSUPPORTED,
    DWARF_TYPE_KIND_VOID,
} DwarfTypeKind;

/* dwarf.CommonType, the fields every type has, and the first member of every
 * type struct. A field that is not known or does not apply is zero. */
typedef struct DwarfCommonType {
    int64_t byte_size; /* the size of a value of the type, in bytes */
    Str name;          /* a name that can be used to refer to the type */
    DwarfTypeKind type_kind;
} DwarfCommonType;

/* dwarf.Type: any of the type structs, by its DwarfCommonType. */
typedef DwarfCommonType *DwarfType;

/* The Type methods, for whichever struct t is. String's result is made in a,
 * and Common is t itself. */
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_type_common(DwarfType t);
int64_t dwarf_type_size(DwarfType t);
BURROW_OWNS(ret) Str dwarf_type_string(DwarfType t, Alloc *a);

/* Data.Type: the type at off in the info section. The types are made in the
 * DwarfData's allocator and kept, so asking again gives the same one. */
BURROW_BORROWS(ret, d) DwarfType dwarf_data_type(DwarfData *d, DwarfOffset off,
                                                 Error *err);

/* dwarf.BasicType, the fields common to all basic types. See DwarfStructField
 * for what bit_size, bit_offset and data_bit_offset mean. */
typedef struct DwarfBasicType {
    DwarfCommonType common_type;
    int64_t bit_size;
    int64_t bit_offset;
    int64_t data_bit_offset;
} DwarfBasicType;

/* The basic types, each a DwarfBasicType and nothing more: signed and unsigned
 * characters and integers, floating point and complex numbers, booleans,
 * machine addresses, and a type that is implicit, unknown, ambiguous or not
 * there at all. */
typedef struct DwarfCharType {
    DwarfBasicType basic_type;
} DwarfCharType;

typedef struct DwarfUcharType {
    DwarfBasicType basic_type;
} DwarfUcharType;

typedef struct DwarfIntType {
    DwarfBasicType basic_type;
} DwarfIntType;

typedef struct DwarfUintType {
    DwarfBasicType basic_type;
} DwarfUintType;

typedef struct DwarfFloatType {
    DwarfBasicType basic_type;
} DwarfFloatType;

typedef struct DwarfComplexType {
    DwarfBasicType basic_type;
} DwarfComplexType;

typedef struct DwarfBoolType {
    DwarfBasicType basic_type;
} DwarfBoolType;

typedef struct DwarfAddrType {
    DwarfBasicType basic_type;
} DwarfAddrType;

typedef struct DwarfUnspecifiedType {
    DwarfBasicType basic_type;
} DwarfUnspecifiedType;

/* dwarf.QualType: a C or C++ const, restrict or volatile type. */
typedef struct DwarfQualType {
    DwarfCommonType common_type;
    Str qual;
    DwarfType type;
} DwarfQualType;

/* dwarf.ArrayType, a fixed size array. */
typedef struct DwarfArrayType {
    DwarfCommonType common_type;
    DwarfType type;
    int64_t stride_bit_size; /* if > 0, how many bits each element takes */
    int64_t count;           /* -1 for an incomplete array, like char x[] */
} DwarfArrayType;

/* dwarf.VoidType, C's void. */
typedef struct DwarfVoidType {
    DwarfCommonType common_type;
} DwarfVoidType;

/* dwarf.PtrType, a pointer. */
typedef struct DwarfPtrType {
    DwarfCommonType common_type;
    DwarfType type;
} DwarfPtrType;

/* dwarf.StructField, a field of a struct, union or C++ class.
 *
 * For a bit field, bit_size is how many bits it has, and it is 0 for any other
 * field. data_bit_offset, if not 0, is how many bits there are from the start
 * of the struct to the start of the field, DWARF 4's DW_AT_data_bit_offset.
 * bit_offset, if not 0, is how many bits there are from the most significant
 * bit of the storage unit holding the field to the field's own most significant
 * bit, the DW_AT_bit_offset DWARF 4 deprecated, and it depends on the byte
 * order. At most one of the two is set, and only for a bit field. For
 *
 *     struct S {
 *         int q;
 *         int j:5;
 *         int k:6;
 *         int m:5;
 *         int n:8;
 *     } s;
 *
 * GCC 8 gives j, k, m and n the bit_offset 27, 21, 16 and 8 little endian and
 * 0, 5, 11 and 16 big endian, and a compiler that writes data_bit_offset gives
 * them 32, 37, 43 and 48. */
typedef struct DwarfStructField {
    Str name;
    DwarfType type;
    int64_t byte_offset;
    int64_t byte_size; /* usually 0, and dwarf_type_size(type) is the size */
    int64_t bit_offset;
    int64_t data_bit_offset;
    int64_t bit_size; /* 0 if it is not a bit field */
} DwarfStructField;

/* dwarf.StructType, a struct, union or C++ class. */
typedef struct DwarfStructType {
    DwarfCommonType common_type;
    Str struct_name;
    Str kind;        /* "struct", "union" or "class" */
    Slice field;     /* of DwarfStructField pointers */
    bool incomplete; /* declared but not defined */
} DwarfStructType;

/* dwarf.EnumValue, one value of an enumeration. */
typedef struct DwarfEnumValue {
    Str name;
    int64_t val;
} DwarfEnumValue;

/* dwarf.EnumType, an enumeration. Its byte_size is all it says about the
 * integer type underneath. */
typedef struct DwarfEnumType {
    DwarfCommonType common_type;
    Str enum_name;
    Slice val; /* of DwarfEnumValue pointers */
} DwarfEnumType;

/* dwarf.FuncType, a function type. */
typedef struct DwarfFuncType {
    DwarfCommonType common_type;
    DwarfType return_type;
    Slice param_type; /* of DwarfType */
} DwarfFuncType;

/* dwarf.DotDotDotType, the ... of a variadic function's parameters. */
typedef struct DwarfDotDotDotType {
    DwarfCommonType common_type;
} DwarfDotDotDotType;

/* dwarf.TypedefType, a type with a name. */
typedef struct DwarfTypedefType {
    DwarfCommonType common_type;
    DwarfType type;
} DwarfTypedefType;

/* dwarf.UnsupportedType, what a kind of type entry this cannot read yet gives,
 * such as a C++ reference. */
typedef struct DwarfUnsupportedType {
    DwarfCommonType common_type;
    DwarfTag tag;
} DwarfUnsupportedType;

/* Each struct's methods. Common gives the DwarfCommonType at its start, Basic
 * the DwarfBasicType, Size the size in bytes, and String the C-like spelling
 * of the type, made in a. Defn is a struct's String with its fields spelled
 * out even when it has a name. */
BURROW_BORROWS(ret, c) DwarfCommonType *dwarf_common_type_common(DwarfCommonType *c);
int64_t dwarf_common_type_size(DwarfCommonType *c);

BURROW_BORROWS(ret, b) DwarfBasicType *dwarf_basic_type_basic(DwarfBasicType *b);
BURROW_BORROWS(ret, b) DwarfCommonType *dwarf_basic_type_common(DwarfBasicType *b);
int64_t dwarf_basic_type_size(DwarfBasicType *b);
BURROW_OWNS(ret) Str dwarf_basic_type_string(DwarfBasicType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_char_type_basic(DwarfCharType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_char_type_common(DwarfCharType *t);
int64_t dwarf_char_type_size(DwarfCharType *t);
BURROW_OWNS(ret) Str dwarf_char_type_string(DwarfCharType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_uchar_type_basic(DwarfUcharType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_uchar_type_common(DwarfUcharType *t);
int64_t dwarf_uchar_type_size(DwarfUcharType *t);
BURROW_OWNS(ret) Str dwarf_uchar_type_string(DwarfUcharType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_int_type_basic(DwarfIntType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_int_type_common(DwarfIntType *t);
int64_t dwarf_int_type_size(DwarfIntType *t);
BURROW_OWNS(ret) Str dwarf_int_type_string(DwarfIntType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_uint_type_basic(DwarfUintType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_uint_type_common(DwarfUintType *t);
int64_t dwarf_uint_type_size(DwarfUintType *t);
BURROW_OWNS(ret) Str dwarf_uint_type_string(DwarfUintType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_float_type_basic(DwarfFloatType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_float_type_common(DwarfFloatType *t);
int64_t dwarf_float_type_size(DwarfFloatType *t);
BURROW_OWNS(ret) Str dwarf_float_type_string(DwarfFloatType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_complex_type_basic(DwarfComplexType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_complex_type_common(DwarfComplexType *t);
int64_t dwarf_complex_type_size(DwarfComplexType *t);
BURROW_OWNS(ret) Str dwarf_complex_type_string(DwarfComplexType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_bool_type_basic(DwarfBoolType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_bool_type_common(DwarfBoolType *t);
int64_t dwarf_bool_type_size(DwarfBoolType *t);
BURROW_OWNS(ret) Str dwarf_bool_type_string(DwarfBoolType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *dwarf_addr_type_basic(DwarfAddrType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_addr_type_common(DwarfAddrType *t);
int64_t dwarf_addr_type_size(DwarfAddrType *t);
BURROW_OWNS(ret) Str dwarf_addr_type_string(DwarfAddrType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfBasicType *
dwarf_unspecified_type_basic(DwarfUnspecifiedType *t);
BURROW_BORROWS(ret, t) DwarfCommonType *
dwarf_unspecified_type_common(DwarfUnspecifiedType *t);
int64_t dwarf_unspecified_type_size(DwarfUnspecifiedType *t);
BURROW_OWNS(ret) Str dwarf_unspecified_type_string(DwarfUnspecifiedType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_qual_type_common(DwarfQualType *t);
int64_t dwarf_qual_type_size(DwarfQualType *t);
BURROW_OWNS(ret) Str dwarf_qual_type_string(DwarfQualType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_array_type_common(DwarfArrayType *t);
int64_t dwarf_array_type_size(DwarfArrayType *t);
BURROW_OWNS(ret) Str dwarf_array_type_string(DwarfArrayType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_void_type_common(DwarfVoidType *t);
int64_t dwarf_void_type_size(DwarfVoidType *t);
BURROW_OWNS(ret) Str dwarf_void_type_string(DwarfVoidType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_ptr_type_common(DwarfPtrType *t);
int64_t dwarf_ptr_type_size(DwarfPtrType *t);
BURROW_OWNS(ret) Str dwarf_ptr_type_string(DwarfPtrType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_struct_type_common(DwarfStructType *t);
int64_t dwarf_struct_type_size(DwarfStructType *t);
BURROW_OWNS(ret) Str dwarf_struct_type_string(DwarfStructType *t, Alloc *a);
BURROW_OWNS(ret) Str dwarf_struct_type_defn(DwarfStructType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_enum_type_common(DwarfEnumType *t);
int64_t dwarf_enum_type_size(DwarfEnumType *t);
BURROW_OWNS(ret) Str dwarf_enum_type_string(DwarfEnumType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_func_type_common(DwarfFuncType *t);
int64_t dwarf_func_type_size(DwarfFuncType *t);
BURROW_OWNS(ret) Str dwarf_func_type_string(DwarfFuncType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *
dwarf_dot_dot_dot_type_common(DwarfDotDotDotType *t);
int64_t dwarf_dot_dot_dot_type_size(DwarfDotDotDotType *t);
BURROW_OWNS(ret) Str dwarf_dot_dot_dot_type_string(DwarfDotDotDotType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *dwarf_typedef_type_common(DwarfTypedefType *t);
int64_t dwarf_typedef_type_size(DwarfTypedefType *t);
BURROW_OWNS(ret) Str dwarf_typedef_type_string(DwarfTypedefType *t, Alloc *a);

BURROW_BORROWS(ret, t) DwarfCommonType *
dwarf_unsupported_type_common(DwarfUnsupportedType *t);
int64_t dwarf_unsupported_type_size(DwarfUnsupportedType *t);
BURROW_OWNS(ret) Str dwarf_unsupported_type_string(DwarfUnsupportedType *t, Alloc *a);

/* -------------------------------------------------------------- line tables */

/* dwarf.LineFile, a source file a line table names. */
typedef struct DwarfLineFile {
    Str name;
    uint64_t mtime; /* when it was changed, in a form of the producer's, or 0 */
    Int length;     /* its length, or 0 if that is not known */
} DwarfLineFile;

/* dwarf.LineEntry, a row of a line table. It covers the instructions from
 * address up to the address of the next row. */
typedef struct DwarfLineEntry {
    /* The address of a machine instruction the compiler wrote. */
    uint64_t address;
    /* Which operation of a VLIW instruction, from 0, and always 0 for any
     * other kind of machine. With address it makes an operation pointer. */
    Int op_index;
    /* The source file the instructions came from. */
    DwarfLineFile *file;
    /* The source line, from 1, or 0 if the instructions belong to none. */
    Int line;
    /* The column in the line, from 1, or 0 for the left edge. */
    Int column;
    /* Whether address is a good place for a breakpoint, such as the start of
     * a line or a statement. */
    bool is_stmt;
    /* Whether address starts a basic block. */
    bool basic_block;
    /* Whether address is one of the places to stop on entry to a function
     * before its body runs. DWARF 3. */
    bool prologue_end;
    /* Whether address is one of the places to stop before a function returns.
     * DWARF 3. */
    bool epilogue_begin;
    /* The instruction set, in numbers the ABI defines. DWARF 3. */
    Int isa;
    /* Which of several blocks at the same file, line and column the
     * instructions are in, or 0 when there is only one. DWARF 3. */
    Int discriminator;
    /* Whether address is the first byte after a run of instructions. Only it
     * and address mean anything when this is set. The last row of a table
     * always has it. */
    bool end_sequence;
} DwarfLineEntry;

/* dwarf.LineReaderPos, a place in a line table, from dwarf_line_reader_tell. */
typedef struct DwarfLineReaderPos {
    DwarfOffset off;      /* where in the line section */
    Int num_file_entries; /* how long the file table was there */
    DwarfLineEntry state; /* and the state machine's registers */
    Int file_index;
} DwarfLineReaderPos;

/* dwarf.LineReader, which reads the line table of one compilation unit. The
 * rows come in order of address, and the last has end_sequence set. */
typedef struct DwarfLineReader DwarfLineReader;

/* Data.LineReader: a reader for the line table of cu, which has to be an entry
 * with tag DWARF_TAG_COMPILE_UNIT. NULL and no error if it has none. The reader
 * and the files it reads are made in a. */
BURROW_OWNS(ret) DwarfLineReader *
dwarf_data_line_reader(DwarfData *d, Alloc *a, const DwarfEntry *cu, Error *err);

/* LineReader.Next: the next row into *entry. io_eof at the end of a table that
 * ends the way it should. */
BURROW_STATIC(ret) Error dwarf_line_reader_next(DwarfLineReader *r,
                                                DwarfLineEntry *entry);

/* LineReader.Tell and LineReader.Seek: where the reader is, and going back
 * there. pos has to have come from this reader. */
DwarfLineReaderPos dwarf_line_reader_tell(DwarfLineReader *r);
void dwarf_line_reader_seek(DwarfLineReader *r, DwarfLineReaderPos pos);

/* LineReader.Reset: back to the start of the table. */
void dwarf_line_reader_reset(DwarfLineReader *r);

/* LineReader.Files: the file table as of where the reader is, a Slice of
 * DwarfLineFile pointers. Entry 0 is NULL, since file 0 means no file. The
 * table can grow as the reader goes, but the entries in it never change, and
 * attributes such as DWARF_ATTR_DECL_FILE index it. */
BURROW_BORROWS(ret, r) Slice dwarf_line_reader_files(DwarfLineReader *r);

/* LineReader.SeekPC: the row that covers pc into *entry, with the reader left
 * on the row after it, going back to the start of the table if it has to.
 * dwarf_err_unknown_pc if no row covers pc, and then *entry and where the
 * reader is are not defined. A table can only be read forwards, so this can
 * take as long as reading the whole of it. */
BURROW_STATIC(ret) Error dwarf_line_reader_seek_pc(DwarfLineReader *r, uint64_t pc,
                                                   DwarfLineEntry *entry);

#ifdef __cplusplus
}
#endif

#endif
