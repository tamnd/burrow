/* What the files of debug/dwarf share: the constants Go keeps unexported, the
 * buffer every section is decoded through, the units, and the insides of
 * DwarfData and DwarfReader.
 *
 * Derived from Go's src/debug/dwarf/buf.go, const.go, open.go and unit.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_DEBUG_DWARF_INTERNAL_H
#define BURROW_SRC_DEBUG_DWARF_INTERNAL_H

#include "burrow/debug/dwarf.h"

#include "burrow/core.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- constants */

/* The attribute forms, Go's format. */
enum {
    DW_FORM_ADDR = 0x01,
    DW_FORM_DWARF_BLOCK2 = 0x03,
    DW_FORM_DWARF_BLOCK4 = 0x04,
    DW_FORM_DATA2 = 0x05,
    DW_FORM_DATA4 = 0x06,
    DW_FORM_DATA8 = 0x07,
    DW_FORM_STRING = 0x08,
    DW_FORM_DWARF_BLOCK = 0x09,
    DW_FORM_DWARF_BLOCK1 = 0x0A,
    DW_FORM_DATA1 = 0x0B,
    DW_FORM_FLAG = 0x0C,
    DW_FORM_SDATA = 0x0D,
    DW_FORM_STRP = 0x0E,
    DW_FORM_UDATA = 0x0F,
    DW_FORM_REF_ADDR = 0x10,
    DW_FORM_REF1 = 0x11,
    DW_FORM_REF2 = 0x12,
    DW_FORM_REF4 = 0x13,
    DW_FORM_REF8 = 0x14,
    DW_FORM_REF_UDATA = 0x15,
    DW_FORM_INDIRECT = 0x16,
    /* DWARF 4. */
    DW_FORM_SEC_OFFSET = 0x17,
    DW_FORM_EXPRLOC = 0x18,
    DW_FORM_FLAG_PRESENT = 0x19,
    DW_FORM_REF_SIG8 = 0x20,
    /* DWARF 5. */
    DW_FORM_STRX = 0x1A,
    DW_FORM_ADDRX = 0x1B,
    DW_FORM_REF_SUP4 = 0x1C,
    DW_FORM_STRP_SUP = 0x1D,
    DW_FORM_DATA16 = 0x1E,
    DW_FORM_LINE_STRP = 0x1F,
    DW_FORM_IMPLICIT_CONST = 0x21,
    DW_FORM_LOCLISTX = 0x22,
    DW_FORM_RNGLISTX = 0x23,
    DW_FORM_REF_SUP8 = 0x24,
    DW_FORM_STRX1 = 0x25,
    DW_FORM_STRX2 = 0x26,
    DW_FORM_STRX3 = 0x27,
    DW_FORM_STRX4 = 0x28,
    DW_FORM_ADDRX1 = 0x29,
    DW_FORM_ADDRX2 = 0x2A,
    DW_FORM_ADDRX3 = 0x2B,
    DW_FORM_ADDRX4 = 0x2C,
    /* The GNU extensions for dwz's shared files. */
    DW_FORM_GNU_REF_ALT = 0x1f20,
    DW_FORM_GNU_STRP_ALT = 0x1f21,
};

/* The two location opcodes readType looks at. */
enum {
    DW_OP_CONSTS = 0x11,
    DW_OP_PLUS_UCONST = 0x23,
};

/* The base type encodings. */
enum {
    DW_ENC_ADDRESS = 0x01,
    DW_ENC_BOOLEAN = 0x02,
    DW_ENC_COMPLEX_FLOAT = 0x03,
    DW_ENC_FLOAT = 0x04,
    DW_ENC_SIGNED = 0x05,
    DW_ENC_SIGNED_CHAR = 0x06,
    DW_ENC_UNSIGNED = 0x07,
    DW_ENC_UNSIGNED_CHAR = 0x08,
    DW_ENC_IMAGINARY_FLOAT = 0x09,
    DW_ENC_PACKED_DECIMAL = 0x0A,
    DW_ENC_NUMERIC_STRING = 0x0B,
    DW_ENC_EDITED = 0x0C,
    DW_ENC_SIGNED_FIXED = 0x0D,
    DW_ENC_UNSIGNED_FIXED = 0x0E,
    DW_ENC_DECIMAL_FLOAT = 0x0F,
    DW_ENC_UTF = 0x10,
    DW_ENC_UCS = 0x11,
    DW_ENC_ASCII = 0x12,
};

/* The standard line number opcodes. */
enum {
    DW_LNS_COPY = 1,
    DW_LNS_ADVANCE_PC = 2,
    DW_LNS_ADVANCE_LINE = 3,
    DW_LNS_SET_FILE = 4,
    DW_LNS_SET_COLUMN = 5,
    DW_LNS_NEGATE_STMT = 6,
    DW_LNS_SET_BASIC_BLOCK = 7,
    DW_LNS_CONST_ADD_PC = 8,
    DW_LNS_FIXED_ADVANCE_PC = 9,
    DW_LNS_SET_PROLOGUE_END = 10,
    DW_LNS_SET_EPILOGUE_BEGIN = 11,
    DW_LNS_SET_ISA = 12,
};

/* The extended line number opcodes. */
enum {
    DW_LNE_END_SEQUENCE = 1,
    DW_LNE_SET_ADDRESS = 2,
    DW_LNE_DEFINE_FILE = 3,
    DW_LNE_SET_DISCRIMINATOR = 4,
};

/* The line number header entry formats of DWARF 5. */
enum {
    DW_LNCT_PATH = 0x01,
    DW_LNCT_DIRECTORY_INDEX = 0x02,
    DW_LNCT_TIMESTAMP = 0x03,
    DW_LNCT_SIZE = 0x04,
    DW_LNCT_MD5 = 0x05,
};

/* The unit header types of DWARF 5. */
enum {
    DW_UT_COMPILE = 0x01,
    DW_UT_TYPE = 0x02,
    DW_UT_PARTIAL = 0x03,
    DW_UT_SKELETON = 0x04,
    DW_UT_SPLIT_COMPILE = 0x05,
    DW_UT_SPLIT_TYPE = 0x06,
};

/* The range list entry kinds of DWARF 5. */
enum {
    DW_RLE_END_OF_LIST = 0x0,
    DW_RLE_BASE_ADDRESSX = 0x1,
    DW_RLE_STARTX_ENDX = 0x2,
    DW_RLE_STARTX_LENGTH = 0x3,
    DW_RLE_OFFSET_PAIR = 0x4,
    DW_RLE_BASE_ADDRESS = 0x5,
    DW_RLE_START_END = 0x6,
    DW_RLE_START_LENGTH = 0x7,
};

/* ------------------------------------------------------------------- names */

/* A row of the tables dwarf_string.c has, which tools/gen-debug-consts.sh
 * writes. */
typedef struct DwarfName {
    uint32_t v;
    Str name;
} DwarfName;

/* The name of a, without the Attr, or the empty string if it has none. */
Str burrow__dwarf_attr_name(DwarfAttr a);

/* The same for a tag. */
Str burrow__dwarf_tag_name(DwarfTag t);

/* ------------------------------------------------------------------- units */

/* An abbreviation, the shape every entry that names it has. */
typedef struct DwAbbrevField {
    DwarfAttr attr;
    uint32_t fmt;
    DwarfClass cls;
    int64_t val; /* for DW_FORM_IMPLICIT_CONST */
} DwAbbrevField;

typedef struct DwAbbrev {
    DwarfTag tag;
    bool children;
    Int nfield;
    DwAbbrevField *field;
} DwAbbrev;

/* What only a DWARF 5 unit has, read from its first entry the first time a
 * reader comes to it. */
typedef struct DwUnit5 {
    uint64_t addr_base;
    uint64_t str_offsets_base;
    uint64_t rng_lists_base;
    uint64_t loc_lists_base;
} DwUnit5;

/* unit, a compilation unit, and Go's dataFormat for the buffers that read
 * it. A NULL format is Go's unknownFormat. */
typedef struct DwUnit {
    DwarfOffset base; /* where its header is in the info section */
    DwarfOffset off;  /* where its entries are */
    Byte *data;
    Int len;
    Map *atable; /* uint32_t to DwAbbrev pointer */
    DwUnit5 *unit5;
    Int asize;
    Int vers;
    bool is64;
    uint8_t utype;
} DwUnit;

/* typeUnit, the one type a DWARF 4 .debug_types unit defines. */
typedef struct DwTypeUnit {
    DwUnit unit;
    DwarfOffset toff; /* where the type is */
    Str name;         /* the section it came from */
    DwarfType cache;
} DwTypeUnit;

/* -------------------------------------------------------------------- data */

struct DwarfData {
    Alloc *a;

    /* The sections. A NULL p is a section the file does not have. */
    Slice abbrev;
    Slice aranges;
    Slice frame;
    Slice info;
    Slice line;
    Slice pubnames;
    Slice ranges;
    Slice str;

    /* DWARF 5. */
    Slice addr;
    Slice line_str;
    Slice str_offsets;
    Slice rng_lists;

    Map *abbrev_cache; /* uint64_t to Map pointer */
    bool big_endian;
    BinaryByteOrder order;
    Map *type_cache; /* DwarfOffset to DwarfType */
    Map *type_sigs;  /* uint64_t to DwTypeUnit pointer */
    DwUnit *unit;
    Int nunit;
};

/* ------------------------------------------------------------------ buffer */

/* buf, a section being decoded. After the first error data is NULL, and every
 * read gives zero from then on, which is what lets the decoders read a whole
 * record and look at err once at the end. */
typedef struct DwBuf {
    DwarfData *dwarf;
    const DwUnit *format;
    Str name;
    DwarfOffset off;
    Byte *data;
    Int len;
    Error err;
} DwBuf;

static inline DwBuf dw_make_buf(DwarfData *d, const DwUnit *format, Str name,
                                DwarfOffset off, Byte *data, Int len) {
    DwBuf b = {d, format, name, off, data, len, BURROW_NO_ERROR};
    return b;
}

/* The format's methods. */
static inline Int dw_format_version(const DwUnit *u) {
    return u == NULL ? 0 : u->vers;
}

static inline bool dw_format_dwarf64(const DwUnit *u, bool *known) {
    *known = u != NULL;
    return u != NULL && u->is64;
}

static inline Int dw_format_addrsize(const DwUnit *u) {
    return u == NULL ? 0 : u->asize;
}

/* buf.error: records the first error and empties the buffer. */
void burrow__dwarf_buf_error(DwBuf *b, Str s);

static inline uint8_t dw_buf_uint8(DwBuf *b) {
    if (b->len < 1) {
        burrow__dwarf_buf_error(b, BURROW_S("underflow"));
        return 0;
    }
    uint8_t val = b->data[0];
    b->data++;
    b->len--;
    b->off++;
    return val;
}

/* buf.bytes: the next n bytes, or NULL if there are not that many. */
static inline Byte *dw_buf_bytes(DwBuf *b, Int n) {
    if (n < 0 || b->len < n) {
        burrow__dwarf_buf_error(b, BURROW_S("underflow"));
        return NULL;
    }
    Byte *data = b->data;
    b->data += n;
    b->len -= n;
    b->off += (DwarfOffset)n;
    return data;
}

static inline void dw_buf_skip(DwBuf *b, Int n) {
    (void)dw_buf_bytes(b, n);
}

/* buf.string: up to the next NUL, which is skipped. The result points into the
 * section. */
static inline Str dw_buf_string(DwBuf *b) {
    const Byte *nul =
        b->len > 0 ? (const Byte *)memchr(b->data, 0, (size_t)b->len) : NULL;
    if (nul == NULL) {
        burrow__dwarf_buf_error(b, BURROW_S("underflow"));
        return BURROW_STR_EMPTY;
    }
    Int i = (Int)(nul - b->data);
    Str s = {b->data, i};
    b->data += i + 1;
    b->len -= i + 1;
    b->off += (DwarfOffset)(i + 1);
    return s;
}

static inline uint64_t dw_buf_fixed(DwBuf *b, Int n) {
    const Byte *a = dw_buf_bytes(b, n);
    if (a == NULL)
        return 0;
    uint64_t v = 0;
    if (b->dwarf->big_endian) {
        for (Int i = 0; i < n; i++)
            v = v << 8 | a[i];
    } else {
        for (Int i = n; i > 0; i--)
            v = v << 8 | a[i - 1];
    }
    return v;
}

static inline uint16_t dw_buf_uint16(DwBuf *b) {
    return (uint16_t)dw_buf_fixed(b, 2);
}

static inline uint32_t dw_buf_uint24(DwBuf *b) {
    return (uint32_t)dw_buf_fixed(b, 3);
}

static inline uint32_t dw_buf_uint32(DwBuf *b) {
    return (uint32_t)dw_buf_fixed(b, 4);
}

static inline uint64_t dw_buf_uint64(DwBuf *b) {
    return dw_buf_fixed(b, 8);
}

/* buf.varint: 7 bits a byte, little end first, with the top bit set on every
 * byte but the last. bits is 0 if the buffer ends first. Go's shifts give 0
 * once they pass 63, and so do these. */
static inline uint64_t dw_buf_varint(DwBuf *b, unsigned *bits) {
    uint64_t c = 0;
    unsigned n = 0;
    for (Int i = 0; i < b->len; i++) {
        Byte x = b->data[i];
        if (n < 64)
            c |= (uint64_t)(x & 0x7F) << n;
        n += 7;
        if ((x & 0x80) == 0) {
            b->off += (DwarfOffset)(i + 1);
            b->data += i + 1;
            b->len -= i + 1;
            *bits = n;
            return c;
        }
    }
    *bits = 0;
    return 0;
}

static inline uint64_t dw_buf_uint(DwBuf *b) {
    unsigned bits;
    uint64_t x = dw_buf_varint(b, &bits);
    if (bits == 0)
        burrow__dwarf_buf_error(b, BURROW_S("underflow"));
    return x;
}

/* buf.int, the varint sign extended from its last bit. */
static inline int64_t dw_buf_int(DwBuf *b) {
    unsigned bits;
    uint64_t ux = dw_buf_varint(b, &bits);
    if (bits > 0 && bits < 64 && (ux >> (bits - 1) & 1) != 0)
        ux |= ~(uint64_t)0 << bits;
    if (bits == 0)
        burrow__dwarf_buf_error(b, BURROW_S("underflow"));
    return (int64_t)ux;
}

/* buf.addr, an address the size the format says. */
static inline uint64_t dw_buf_addr(DwBuf *b) {
    switch (dw_format_addrsize(b->format)) {
    case 1:
        return dw_buf_uint8(b);
    case 2:
        return dw_buf_uint16(b);
    case 4:
        return dw_buf_uint32(b);
    case 8:
        return dw_buf_uint64(b);
    default:
        break;
    }
    burrow__dwarf_buf_error(b, BURROW_S("unknown address size"));
    return 0;
}

/* buf.unitLength. A 64-bit length is cut to an Offset, as in Go. */
static inline DwarfOffset dw_buf_unit_length(DwBuf *b, bool *dwarf64) {
    DwarfOffset length = dw_buf_uint32(b);
    *dwarf64 = false;
    if (length == 0xffffffffU) {
        *dwarf64 = true;
        length = (DwarfOffset)dw_buf_uint64(b);
    } else if (length >= 0xfffffff0U) {
        burrow__dwarf_buf_error(b, BURROW_S("unit length has reserved value"));
    }
    return length;
}

/* ---------------------------------------------------------------- messages */

/* A message put together on the stack, for the errors Go builds with
 * fmt.Errorf. Anything past the end is dropped. */
typedef struct DwMsg {
    Byte p[512];
    Int len;
} DwMsg;

static inline void dw_msg_str(DwMsg *m, Str s) {
    Int n = s.len;
    if (n > (Int)sizeof m->p - m->len)
        n = (Int)sizeof m->p - m->len;
    if (n > 0)
        memcpy(m->p + m->len, s.p, (size_t)n);
    m->len += n;
}

static inline void dw_msg_uint_base(DwMsg *m, uint64_t u, unsigned base) {
    Byte digits[24];
    Int n = 0;
    do {
        digits[n++] = (Byte) "0123456789abcdef"[u % base];
        u /= base;
    } while (u != 0);
    while (n > 0 && m->len < (Int)sizeof m->p)
        m->p[m->len++] = digits[--n];
}

static inline void dw_msg_uint(DwMsg *m, uint64_t u) {
    dw_msg_uint_base(m, u, 10);
}

static inline void dw_msg_int(DwMsg *m, int64_t i) {
    if (i < 0) {
        dw_msg_str(m, BURROW_S("-"));
        dw_msg_uint(m, 0 - (uint64_t)i);
        return;
    }
    dw_msg_uint(m, (uint64_t)i);
}

static inline Str dw_msg_text(const DwMsg *m) {
    Str s = {m->p, m->len};
    return s;
}

/* Tag.String, as a message. */
static inline void dw_msg_tag(DwMsg *m, DwarfTag i) {
    Str name = burrow__dwarf_tag_name(i);
    if (name.len > 0) {
        dw_msg_str(m, name);
        return;
    }
    dw_msg_str(m, BURROW_S("Tag("));
    dw_msg_int(m, (int64_t)i);
    dw_msg_str(m, BURROW_S(")"));
}

/* ------------------------------------------------------------------ shared */

/* A DecodeError, made in d's allocator. */
Error burrow__dwarf_decode_error(DwarfData *d, Str name, DwarfOffset off, Str err);

/* errors.New in d's allocator. */
Error burrow__dwarf_error(DwarfData *d, Str text);

/* What every allocation that fails ends in. */
BURROW_NORETURN void burrow__dwarf_oom(void);

/* pathJoin from line.go: filename, which is relative, in the directory
 * dirname, where either can be a Unix or a DOS path. The tests call it the way
 * Go's export_test.go lets its tests call PathJoin. */
BURROW_OWNS(ret) Str burrow__dwarf_path_join(Alloc *a, Str dirname, Str filename);

/* n zeroed bytes from a, or a panic. */
void *burrow__dwarf_alloc(Alloc *a, size_t n);

#define DW_NEW(a, T) ((T *)burrow__dwarf_alloc((a), sizeof(T)))

/* Go's panic for a method called on a nil pointer or a nil interface. */
BURROW_NORETURN void burrow__dwarf_nil_panic(void);

/* The maps here all hold pointers. */
static inline Map *dw_map_make(Alloc *a, const Type *key) {
    Map *m = map_make(a, key, TYPE_UNSAFE_POINTER, 0);
    if (m == NULL)
        burrow__dwarf_oom();
    return m;
}

static inline void dw_map_put(Map *m, const void *key, void *val) {
    if (!map_set(m, key, &val))
        burrow__dwarf_oom();
}

static inline void *dw_map_lookup(Map *m, const void *key) {
    void **p = (void **)map_get(m, key);
    return p == NULL ? NULL : *p;
}

/* Whether Go's uint64(int(v)) == v holds, which it always does where an int
 * has 64 bits. */
static inline bool dw_fits_int(uint64_t v) {
    return (uint64_t)(Int)v == v;
}

/* parseAbbrev: the abbreviation table at off in the abbrev section, as a map
 * from its ids to DwAbbrev pointers. */
Map *burrow__dwarf_parse_abbrev(DwarfData *d, uint64_t off, Int vers, Error *err);

/* buf.entry: the entry at the front of b, made in a. NULL with b->err set if
 * it cannot be read. */
DwarfEntry *burrow__dwarf_entry(DwBuf *b, Alloc *a, const DwUnit *u);

/* offsetToUnit: the index of the unit off is in, or -1. */
Int burrow__dwarf_offset_to_unit(const DwarfData *d, DwarfOffset off);

/* The value of attribute a in e if it holds the C type t, or NULL. This is
 * Go's e.Val(a).(T) with its ok. */
void *burrow__dwarf_entry_val_as(const DwarfEntry *e, DwarfAttr a, const Type *t);

/* parseTypes, in dwarf_type.c. */
Error burrow__dwarf_parse_types(DwarfData *d, Str name, Slice types);

/* ------------------------------------------------------------------ reader */

struct DwarfReader {
    DwBuf b;
    DwarfData *d;
    Alloc *a;
    Error err;
    Int unit;
    bool last_unit;     /* the last entry Next returned was a unit's */
    bool last_children; /* and it had children */
    DwarfOffset last_sibling;
    DwarfEntry *cu;
};

#endif
