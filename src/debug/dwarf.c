/* debug/dwarf: opening the sections, the units, the abbreviation tables, the
 * entries and the reader that walks them, and the PC ranges of an entry.
 *
 * Derived from Go's src/debug/dwarf/buf.go, const.go, entry.go, open.go,
 * unit.go and the stringer output beside them.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "dwarf_internal.h"

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DW_S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- helpers */

void burrow__dwarf_oom(void) {
    panic_str(DW_S("debug/dwarf: out of memory"));
}

void *burrow__dwarf_alloc(Alloc *a, size_t n) {
    void *p = mem_alloc(a, n == 0 ? 1 : n, BURROW_ALIGN_MAX);
    if (p == NULL)
        burrow__dwarf_oom();
    return p;
}

#define DW_NEW(a, T) ((T *)burrow__dwarf_alloc((a), sizeof(T)))

/* A copy of what m holds, in a. */
static Str dw_msg_clone(Alloc *a, const DwMsg *m) {
    if (m->len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)m->len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, m->p, (size_t)m->len);
    return str_from_bytes(p, m->len);
}

Error burrow__dwarf_error(DwarfData *d, Str text) {
    return errors_new(d->a, text);
}

/* -------------------------------------------------------------- the names */

Str dwarf_attr_string(DwarfAttr i, Alloc *a) {
    Str name = burrow__dwarf_attr_name(i);
    if (name.len > 0)
        return name;
    DwMsg m = {.len = 0};
    dw_msg_str(&m, DW_S("Attr("));
    dw_msg_int(&m, (int64_t)i);
    dw_msg_str(&m, DW_S(")"));
    return dw_msg_clone(a, &m);
}

Str dwarf_attr_go_string(DwarfAttr i, Alloc *a) {
    Str name = burrow__dwarf_attr_name(i);
    DwMsg m = {.len = 0};
    dw_msg_str(&m, DW_S("dwarf.Attr"));
    if (name.len > 0) {
        dw_msg_str(&m, name);
    } else {
        dw_msg_str(&m, DW_S("("));
        dw_msg_int(&m, (int64_t)i);
        dw_msg_str(&m, DW_S(")"));
    }
    return dw_msg_clone(a, &m);
}

static void dw_msg_tag(DwMsg *m, DwarfTag i) {
    Str name = burrow__dwarf_tag_name(i);
    if (name.len > 0) {
        dw_msg_str(m, name);
        return;
    }
    dw_msg_str(m, DW_S("Tag("));
    dw_msg_int(m, (int64_t)i);
    dw_msg_str(m, DW_S(")"));
}

Str dwarf_tag_string(DwarfTag i, Alloc *a) {
    Str name = burrow__dwarf_tag_name(i);
    if (name.len > 0)
        return name;
    DwMsg m = {.len = 0};
    dw_msg_tag(&m, i);
    return dw_msg_clone(a, &m);
}

Str dwarf_tag_go_string(DwarfTag i, Alloc *a) {
    DwMsg m = {.len = 0};
    dw_msg_str(&m, i <= DWARF_TAG_TEMPLATE_ALIAS ? DW_S("dwarf.Tag") : DW_S("dwarf."));
    dw_msg_tag(&m, i);
    return dw_msg_clone(a, &m);
}

static const Str dw_class_names[] = {
    BURROW_S_INIT("ClassUnknown"),     BURROW_S_INIT("ClassAddress"),
    BURROW_S_INIT("ClassBlock"),       BURROW_S_INIT("ClassConstant"),
    BURROW_S_INIT("ClassExprLoc"),     BURROW_S_INIT("ClassFlag"),
    BURROW_S_INIT("ClassLinePtr"),     BURROW_S_INIT("ClassLocListPtr"),
    BURROW_S_INIT("ClassMacPtr"),      BURROW_S_INIT("ClassRangeListPtr"),
    BURROW_S_INIT("ClassReference"),   BURROW_S_INIT("ClassReferenceSig"),
    BURROW_S_INIT("ClassString"),      BURROW_S_INIT("ClassReferenceAlt"),
    BURROW_S_INIT("ClassStringAlt"),   BURROW_S_INIT("ClassAddrPtr"),
    BURROW_S_INIT("ClassLocList"),     BURROW_S_INIT("ClassRngList"),
    BURROW_S_INIT("ClassRngListsPtr"), BURROW_S_INIT("ClassStrOffsetsPtr"),
};

static void dw_msg_class(DwMsg *m, DwarfClass i) {
    if (i >= 0 && i < (Int)(sizeof dw_class_names / sizeof dw_class_names[0])) {
        dw_msg_str(m, dw_class_names[i]);
        return;
    }
    dw_msg_str(m, DW_S("Class("));
    dw_msg_int(m, (int64_t)i);
    dw_msg_str(m, DW_S(")"));
}

Str dwarf_class_string(DwarfClass i, Alloc *a) {
    if (i >= 0 && i < (Int)(sizeof dw_class_names / sizeof dw_class_names[0]))
        return dw_class_names[i];
    DwMsg m = {.len = 0};
    dw_msg_class(&m, i);
    return dw_msg_clone(a, &m);
}

Str dwarf_class_go_string(DwarfClass i, Alloc *a) {
    DwMsg m = {.len = 0};
    dw_msg_str(&m, DW_S("dwarf."));
    dw_msg_class(&m, i);
    return dw_msg_clone(a, &m);
}

/* ------------------------------------------------------------ descriptors */

static Str dwarf_attr_m_go_string(DwarfAttr *self) {
    return dwarf_attr_go_string(*self, error_allocator());
}

static Str dwarf_attr_m_string(DwarfAttr *self) {
    return dwarf_attr_string(*self, error_allocator());
}

#define DWARF_ATTR_SIG_STRING(IN, OUT) OUT(Str)
#define DWARF_ATTR_METHODS(M, T)                                                       \
    M(T, GoString, dwarf_attr_m_go_string, DWARF_ATTR_SIG_STRING)                      \
    M(T, String, dwarf_attr_m_string, DWARF_ATTR_SIG_STRING)

BURROW_METHODS_DEFINE(DwarfAttr, DWARF_ATTR_METHODS);

const Type burrow_type_DwarfAttr = {
    {(const Byte *)"Attr", 4},
    {(const Byte *)"debug/dwarf", 11},
    KIND_UINT32,
    (uint32_t)sizeof(DwarfAttr),
    (uint16_t)_Alignof(DwarfAttr),
    0,
    (uint16_t)(sizeof(burrow__methods_DwarfAttr) /
               sizeof(burrow__methods_DwarfAttr[0])),
    NULL,
    burrow__methods_DwarfAttr,
    NULL,
    NULL,
    0,
    0x64776174U, /* "dwat" */
    NULL,
};

static Str dwarf_tag_m_go_string(DwarfTag *self) {
    return dwarf_tag_go_string(*self, error_allocator());
}

static Str dwarf_tag_m_string(DwarfTag *self) {
    return dwarf_tag_string(*self, error_allocator());
}

#define DWARF_TAG_SIG_STRING(IN, OUT) OUT(Str)
#define DWARF_TAG_METHODS(M, T)                                                        \
    M(T, GoString, dwarf_tag_m_go_string, DWARF_TAG_SIG_STRING)                        \
    M(T, String, dwarf_tag_m_string, DWARF_TAG_SIG_STRING)

BURROW_METHODS_DEFINE(DwarfTag, DWARF_TAG_METHODS);

const Type burrow_type_DwarfTag = {
    {(const Byte *)"Tag", 3},
    {(const Byte *)"debug/dwarf", 11},
    KIND_UINT32,
    (uint32_t)sizeof(DwarfTag),
    (uint16_t)_Alignof(DwarfTag),
    0,
    (uint16_t)(sizeof(burrow__methods_DwarfTag) / sizeof(burrow__methods_DwarfTag[0])),
    NULL,
    burrow__methods_DwarfTag,
    NULL,
    NULL,
    0,
    0x64777467U, /* "dwtg" */
    NULL,
};

static Str dwarf_class_m_go_string(DwarfClass *self) {
    return dwarf_class_go_string(*self, error_allocator());
}

static Str dwarf_class_m_string(DwarfClass *self) {
    return dwarf_class_string(*self, error_allocator());
}

#define DWARF_CLASS_SIG_STRING(IN, OUT) OUT(Str)
#define DWARF_CLASS_METHODS(M, T)                                                      \
    M(T, GoString, dwarf_class_m_go_string, DWARF_CLASS_SIG_STRING)                    \
    M(T, String, dwarf_class_m_string, DWARF_CLASS_SIG_STRING)

BURROW_METHODS_DEFINE(DwarfClass, DWARF_CLASS_METHODS);

const Type burrow_type_DwarfClass = {
    {(const Byte *)"Class", 5},
    {(const Byte *)"debug/dwarf", 11},
    KIND_INT,
    (uint32_t)sizeof(DwarfClass),
    (uint16_t)_Alignof(DwarfClass),
    0,
    (uint16_t)(sizeof(burrow__methods_DwarfClass) /
               sizeof(burrow__methods_DwarfClass[0])),
    NULL,
    burrow__methods_DwarfClass,
    NULL,
    NULL,
    0,
    0x6477636cU, /* "dwcl" */
    NULL,
};

const Type burrow_type_DwarfOffset = {
    {(const Byte *)"Offset", 6},
    {(const Byte *)"debug/dwarf", 11},
    KIND_UINT32,
    (uint32_t)sizeof(DwarfOffset),
    (uint16_t)_Alignof(DwarfOffset),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64776f66U, /* "dwof" */
    NULL,
};

const Type burrow_type_DwarfField = {
    {(const Byte *)"Field", 5},
    {(const Byte *)"debug/dwarf", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(DwarfField),
    (uint16_t)_Alignof(DwarfField),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6477666cU, /* "dwfl" */
    NULL,
};

/* [2]uint64, which has no name in Go. */
const Type burrow_type_DwarfPCRange = {
    {NULL, 0},
    {NULL, 0},
    KIND_ARRAY,
    (uint32_t)sizeof(DwarfPCRange),
    (uint16_t)_Alignof(DwarfPCRange),
    0,
    0,
    NULL,
    NULL,
    TYPE_UINT64,
    NULL,
    2,
    0,
    NULL,
};

/* ------------------------------------------------------------------ errors */

BURROW_SENTINEL_ERROR(dwarf_err_unknown_pc, "ErrUnknownPC");

/* The public struct first, so that errors_as hands back a DwarfDecodeError,
 * and the message after it, since the message slot cannot allocate. */
typedef struct DwDecodeErrorBox {
    DwarfDecodeError e;
    Str message;
} DwDecodeErrorBox;

static Int dw_put(Byte *p, Int at, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p + at, s.p, (size_t)s.len);
    return at + s.len;
}

/* DecodeError.Error into p, or only its length when p is NULL. */
static Int dw_decode_error_put(Byte *p, Str name, DwarfOffset off, Str err) {
    Byte hex[8];
    Int nh = 0;
    uint32_t v = off;
    do {
        hex[nh++] = (Byte) "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    Int n = dw_put(p, 0, DW_S("decoding dwarf section "));
    n = dw_put(p, n, name);
    n = dw_put(p, n, DW_S(" at offset 0x"));
    while (nh > 0) {
        if (p != NULL)
            p[n] = hex[nh - 1];
        n++;
        nh--;
    }
    n = dw_put(p, n, DW_S(": "));
    return dw_put(p, n, err);
}

Str dwarf_decode_error_error(DwarfDecodeError e, Alloc *a) {
    Int n = dw_decode_error_put(NULL, e.name, e.offset, e.err);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    dw_decode_error_put(p, e.name, e.offset, e.err);
    return str_from_bytes(p, n);
}

static Str dw_decode_error_m_error(DwarfDecodeError *self) {
    return dwarf_decode_error_error(*self, error_allocator());
}

#define DW_DECODE_ERROR_SIG_ERROR(IN, OUT) OUT(Str)
#define DW_DECODE_ERROR_METHODS(M, T)                                                  \
    M(T, Error, dw_decode_error_m_error, DW_DECODE_ERROR_SIG_ERROR)

BURROW_METHODS_DEFINE(DwarfDecodeError, DW_DECODE_ERROR_METHODS);

const Type burrow_type_DwarfDecodeError = {
    {(const Byte *)"DecodeError", 11},
    {(const Byte *)"debug/dwarf", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(DwarfDecodeError),
    (uint16_t)_Alignof(DwarfDecodeError),
    0,
    (uint16_t)(sizeof(burrow__methods_DwarfDecodeError) /
               sizeof(burrow__methods_DwarfDecodeError[0])),
    NULL,
    burrow__methods_DwarfDecodeError,
    NULL,
    NULL,
    0,
    0x64776465U, /* "dwde" */
    NULL,
};

static Str dw_decode_error_message(const void *self) {
    return ((const DwDecodeErrorBox *)self)->message;
}

static Error dw_decode_error_clone(const void *self, Alloc *a);

static const ErrorVT dw_decode_error_vt = {
    &burrow_type_DwarfDecodeError, dw_decode_error_message, NULL, NULL, NULL, NULL,
    dw_decode_error_clone,
};

/* One allocation: the box, then the name, the text and the message. */
static Error dw_decode_error_build(Alloc *a, Str name, DwarfOffset off, Str err) {
    Int mlen = dw_decode_error_put(NULL, name, off, err);
    size_t size =
        sizeof(DwDecodeErrorBox) + (size_t)name.len + (size_t)err.len + (size_t)mlen;
    DwDecodeErrorBox *b =
        (DwDecodeErrorBox *)mem_alloc_nozero(a, size, _Alignof(DwDecodeErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.name = str_from_bytes(p, name.len);
    p += dw_put(p, 0, name);
    b->e.offset = off;
    b->e.err = str_from_bytes(p, err.len);
    p += dw_put(p, 0, err);
    dw_decode_error_put(p, name, off, err);
    b->message = str_from_bytes(p, mlen);
    return (Error){&dw_decode_error_vt, b};
}

static Error dw_decode_error_clone(const void *self, Alloc *a) {
    const DwarfDecodeError *e = (const DwarfDecodeError *)self;
    return dw_decode_error_build(a, e->name, e->offset, e->err);
}

Error burrow__dwarf_decode_error(DwarfData *d, Str name, DwarfOffset off, Str err) {
    return dw_decode_error_build(d->a, name, off, err);
}

void burrow__dwarf_buf_error(DwBuf *b, Str s) {
    if (BURROW_OK(b->err)) {
        b->data = NULL;
        b->len = 0;
        b->err = burrow__dwarf_decode_error(b->dwarf, b->name, b->off, s);
    }
}

/* ------------------------------------------------------------------- units */

/* parseUnits. The first pass counts the units, skipping the empty ones, and
 * the second reads their headers. */
static Error dw_parse_units(DwarfData *d) {
    Int nunit = 0;
    DwBuf b = dw_make_buf(d, NULL, DW_S("info"), 0, (Byte *)d->info.p, d->info.len);
    while (b.len > 0) {
        bool is64;
        DwarfOffset len = dw_buf_unit_length(&b, &is64);
        dw_buf_skip(&b, (Int)len);
        if (len > 0)
            nunit++;
    }
    if (BURROW_FAILED(b.err))
        return b.err;

    b = dw_make_buf(d, NULL, DW_S("info"), 0, (Byte *)d->info.p, d->info.len);
    DwUnit *units = (DwUnit *)burrow__dwarf_alloc(d->a, (size_t)nunit * sizeof(DwUnit));
    for (Int i = 0; i < nunit; i++) {
        DwUnit *u = &units[i];
        u->base = b.off;
        DwarfOffset n = 0;
        if (BURROW_FAILED(b.err))
            return b.err;
        while (n == 0 && BURROW_OK(b.err))
            n = dw_buf_unit_length(&b, &u->is64);
        DwarfOffset data_off = b.off;
        uint16_t vers = dw_buf_uint16(&b);
        if (vers < 2 || vers > 5) {
            DwMsg m = {.len = 0};
            dw_msg_str(&m, DW_S("unsupported DWARF version "));
            dw_msg_int(&m, vers);
            burrow__dwarf_buf_error(&b, dw_msg_text(&m));
            break;
        }
        u->vers = vers;
        if (vers >= 5) {
            u->utype = dw_buf_uint8(&b);
            u->asize = dw_buf_uint8(&b);
        }
        uint64_t abbrev_off = u->is64 ? dw_buf_uint64(&b) : dw_buf_uint32(&b);
        Error err = BURROW_NO_ERROR;
        Map *atable = burrow__dwarf_parse_abbrev(d, abbrev_off, u->vers, &err);
        if (BURROW_FAILED(err)) {
            if (BURROW_OK(b.err))
                b.err = err;
            break;
        }
        u->atable = atable;
        if (vers < 5)
            u->asize = dw_buf_uint8(&b);
        switch (u->utype) {
        case DW_UT_SKELETON:
        case DW_UT_SPLIT_COMPILE:
            (void)dw_buf_uint64(&b); /* unit ID */
            break;
        case DW_UT_TYPE:
        case DW_UT_SPLIT_TYPE:
            (void)dw_buf_uint64(&b); /* type signature */
            if (u->is64)             /* type offset */
                (void)dw_buf_uint64(&b);
            else
                (void)dw_buf_uint32(&b);
            break;
        default:
            break;
        }
        u->off = b.off;
        Int dn = (Int)(DwarfOffset)(n - (b.off - data_off));
        u->data = dw_buf_bytes(&b, dn);
        u->len = u->data != NULL ? dn : 0;
    }
    if (BURROW_FAILED(b.err))
        return b.err;
    d->unit = units;
    d->nunit = nunit;
    return BURROW_NO_ERROR;
}

Int burrow__dwarf_offset_to_unit(const DwarfData *d, DwarfOffset off) {
    /* The first unit after off. */
    Int lo = 0;
    Int hi = d->nunit;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (d->unit[mid].off > off)
            hi = mid;
        else
            lo = mid + 1;
    }
    if (lo == 0)
        return -1;
    const DwUnit *u = &d->unit[lo - 1];
    if (u->off <= off && off < u->off + (DwarfOffset)u->len)
        return lo - 1;
    return -1;
}

/* ------------------------------------------------------------------- open */

DwarfData *dwarf_new(Alloc *a, Slice abbrev, Slice aranges, Slice frame, Slice info,
                     Slice line, Slice pubnames, Slice ranges, Slice str, Error *err) {
    DwarfData *d = DW_NEW(a, DwarfData);
    d->a = a;
    d->abbrev = abbrev;
    d->aranges = aranges;
    d->frame = frame;
    d->info = info;
    d->line = line;
    d->pubnames = pubnames;
    d->ranges = ranges;
    d->str = str;
    d->abbrev_cache = dw_map_make(a, TYPE_UINT64);
    d->type_cache = dw_map_make(a, TYPE_UINT32);
    d->type_sigs = dw_map_make(a, TYPE_UINT64);

    if (info.len < 6) {
        BURROW_OUT(err, burrow__dwarf_decode_error(
                            d, DW_S("info"), (DwarfOffset)info.len, DW_S("too short")));
        return NULL;
    }
    const Byte *p = (const Byte *)info.p;
    Int offset = 4;
    if (p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff) {
        if (info.len < 14) {
            BURROW_OUT(err, burrow__dwarf_decode_error(d, DW_S("info"),
                                                       (DwarfOffset)info.len,
                                                       DW_S("too short")));
            return NULL;
        }
        offset = 12;
    }
    Byte x = p[offset];
    Byte y = p[offset + 1];
    if (x == 0 && y == 0) {
        BURROW_OUT(err, burrow__dwarf_decode_error(d, DW_S("info"), 4,
                                                   DW_S("unsupported version 0")));
        return NULL;
    }
    if (x == 0) {
        d->big_endian = true;
        d->order = binary_big_endian;
    } else if (y == 0) {
        d->big_endian = false;
        d->order = binary_little_endian;
    } else {
        BURROW_OUT(err, burrow__dwarf_decode_error(
                            d, DW_S("info"), 4, DW_S("cannot determine byte order")));
        return NULL;
    }

    Error e = dw_parse_units(d);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d;
}

Error dwarf_data_add_types(DwarfData *d, Str name, Slice types) {
    Byte *p = (Byte *)burrow__dwarf_alloc(d->a, (size_t)name.len);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    return burrow__dwarf_parse_types(d, str_from_bytes(p, name.len), types);
}

Error dwarf_data_add_section(DwarfData *d, Str name, Slice contents) {
    if (str_eq(name, DW_S(".debug_addr")))
        d->addr = contents;
    else if (str_eq(name, DW_S(".debug_line_str")))
        d->line_str = contents;
    else if (str_eq(name, DW_S(".debug_str_offsets")))
        d->str_offsets = contents;
    else if (str_eq(name, DW_S(".debug_rnglists")))
        d->rng_lists = contents;
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------- abbreviations */

/* attrIsExprloc: the attributes whose blocks are really exprlocs in DWARF 2
 * and 3, from figure 20 of DWARF 4. */
static bool dw_attr_is_exprloc(DwarfAttr a) {
    switch (a) {
    case DWARF_ATTR_LOCATION:
    case DWARF_ATTR_BYTE_SIZE:
    case DWARF_ATTR_BIT_OFFSET:
    case DWARF_ATTR_BIT_SIZE:
    case DWARF_ATTR_STRING_LENGTH:
    case DWARF_ATTR_LOWER_BOUND:
    case DWARF_ATTR_RETURN_ADDR:
    case DWARF_ATTR_STRIDE_SIZE:
    case DWARF_ATTR_UPPER_BOUND:
    case DWARF_ATTR_COUNT:
    case DWARF_ATTR_DATA_MEMBER_LOC:
    case DWARF_ATTR_FRAME_BASE:
    case DWARF_ATTR_SEGMENT:
    case DWARF_ATTR_STATIC_LINK:
    case DWARF_ATTR_USE_LOCATION:
    case DWARF_ATTR_VTABLE_ELEM_LOC:
    case DWARF_ATTR_ALLOCATED:
    case DWARF_ATTR_ASSOCIATED:
    case DWARF_ATTR_DATA_LOCATION:
    case DWARF_ATTR_STRIDE:
        return true;
    default:
        return false;
    }
}

/* attrPtrClass: the *ptr class of the attributes that are a DW_FORM_sec_offset
 * in DWARF 4 or a DW_FORM_data* in 2 and 3. */
static bool dw_attr_ptr_class(DwarfAttr a, DwarfClass *cls) {
    switch (a) {
    case DWARF_ATTR_LOCATION:
    case DWARF_ATTR_STRING_LENGTH:
    case DWARF_ATTR_RETURN_ADDR:
    case DWARF_ATTR_DATA_MEMBER_LOC:
    case DWARF_ATTR_FRAME_BASE:
    case DWARF_ATTR_SEGMENT:
    case DWARF_ATTR_STATIC_LINK:
    case DWARF_ATTR_USE_LOCATION:
    case DWARF_ATTR_VTABLE_ELEM_LOC:
    case DWARF_ATTR_LOCLISTS_BASE:
        *cls = DWARF_CLASS_LOC_LIST_PTR;
        return true;
    case DWARF_ATTR_STMT_LIST:
        *cls = DWARF_CLASS_LINE_PTR;
        return true;
    case DWARF_ATTR_START_SCOPE:
    case DWARF_ATTR_RANGES:
        *cls = DWARF_CLASS_RANGE_LIST_PTR;
        return true;
    case DWARF_ATTR_MACRO_INFO:
        *cls = DWARF_CLASS_MAC_PTR;
        return true;
    case DWARF_ATTR_STR_OFFSETS_BASE:
        *cls = DWARF_CLASS_STR_OFFSETS_PTR;
        return true;
    case DWARF_ATTR_ADDR_BASE:
        *cls = DWARF_CLASS_ADDR_PTR;
        return true;
    case DWARF_ATTR_RNGLISTS_BASE:
        *cls = DWARF_CLASS_RNG_LISTS_PTR;
        return true;
    default:
        return false;
    }
}

/* formToClass: the DWARF 4 class of a form, which before DWARF 4 depends on the
 * attribute for some forms. */
static DwarfClass dw_form_to_class(uint32_t form, DwarfAttr attr, Int vers, DwBuf *b) {
    DwarfClass cls = DWARF_CLASS_UNKNOWN;
    switch (form) {
    case DW_FORM_INDIRECT:
        return DWARF_CLASS_UNKNOWN;
    case DW_FORM_ADDR:
    case DW_FORM_ADDRX:
    case DW_FORM_ADDRX1:
    case DW_FORM_ADDRX2:
    case DW_FORM_ADDRX3:
    case DW_FORM_ADDRX4:
        return DWARF_CLASS_ADDRESS;
    case DW_FORM_DWARF_BLOCK1:
    case DW_FORM_DWARF_BLOCK2:
    case DW_FORM_DWARF_BLOCK4:
    case DW_FORM_DWARF_BLOCK:
        /* DWARF 2 and 3 wrote exprlocs as blocks. No attribute can be both,
         * so a DWARF 4 block that should have been an exprloc is one too. */
        return dw_attr_is_exprloc(attr) ? DWARF_CLASS_EXPR_LOC : DWARF_CLASS_BLOCK;
    case DW_FORM_DATA1:
    case DW_FORM_DATA2:
    case DW_FORM_DATA4:
    case DW_FORM_DATA8:
    case DW_FORM_SDATA:
    case DW_FORM_UDATA:
    case DW_FORM_DATA16:
    case DW_FORM_IMPLICIT_CONST:
        /* DWARF 2 and 3 wrote the *ptr classes as constants, and DWARF 4 has
         * attributes that tell them apart, so this is for 2 and 3 only. */
        if (vers < 4 && dw_attr_ptr_class(attr, &cls))
            return cls;
        return DWARF_CLASS_CONSTANT;
    case DW_FORM_FLAG:
    case DW_FORM_FLAG_PRESENT:
        return DWARF_CLASS_FLAG;
    case DW_FORM_REF_ADDR:
    case DW_FORM_REF1:
    case DW_FORM_REF2:
    case DW_FORM_REF4:
    case DW_FORM_REF8:
    case DW_FORM_REF_UDATA:
    case DW_FORM_REF_SUP4:
    case DW_FORM_REF_SUP8:
        return DWARF_CLASS_REFERENCE;
    case DW_FORM_REF_SIG8:
        return DWARF_CLASS_REFERENCE_SIG;
    case DW_FORM_STRING:
    case DW_FORM_STRP:
    case DW_FORM_STRX:
    case DW_FORM_STRP_SUP:
    case DW_FORM_LINE_STRP:
    case DW_FORM_STRX1:
    case DW_FORM_STRX2:
    case DW_FORM_STRX3:
    case DW_FORM_STRX4:
        return DWARF_CLASS_STRING;
    case DW_FORM_SEC_OFFSET:
        /* DWARF 4 has four *ptr classes and one form for them all, so the
         * attribute says which. */
        if (dw_attr_ptr_class(attr, &cls))
            return cls;
        return DWARF_CLASS_UNKNOWN;
    case DW_FORM_EXPRLOC:
        return DWARF_CLASS_EXPR_LOC;
    case DW_FORM_GNU_REF_ALT:
        return DWARF_CLASS_REFERENCE_ALT;
    case DW_FORM_GNU_STRP_ALT:
        return DWARF_CLASS_STRING_ALT;
    case DW_FORM_LOCLISTX:
        return DWARF_CLASS_LOC_LIST;
    case DW_FORM_RNGLISTX:
        return DWARF_CLASS_RNG_LIST;
    default:
        burrow__dwarf_buf_error(
            b, DW_S("cannot determine class of unknown attribute form"));
        return 0;
    }
}

Map *burrow__dwarf_parse_abbrev(DwarfData *d, uint64_t off, Int vers, Error *err) {
    Map *cached = (Map *)dw_map_lookup(d->abbrev_cache, &off);
    if (cached != NULL)
        return cached;

    Byte *data = NULL;
    Int len = 0;
    if (off <= (uint64_t)d->abbrev.len) {
        data = (Byte *)d->abbrev.p + off;
        len = d->abbrev.len - (Int)off;
    }
    DwBuf b = dw_make_buf(d, NULL, DW_S("abbrev"), 0, data, len);

    /* The buffer gives zeros after an error, which ends both loops. */
    Map *m = dw_map_make(d->a, TYPE_UINT32);
    for (;;) {
        /* A table ends with id 0. */
        uint32_t id = (uint32_t)dw_buf_uint(&b);
        if (id == 0)
            break;

        /* Count the attributes on a copy of b. */
        Int n = 0;
        DwBuf b1 = b;
        (void)dw_buf_uint(&b1);
        (void)dw_buf_uint8(&b1);
        for (;;) {
            uint64_t tag = dw_buf_uint(&b1);
            uint64_t fmt = dw_buf_uint(&b1);
            if (tag == 0 && fmt == 0)
                break;
            if ((uint32_t)fmt == DW_FORM_IMPLICIT_CONST)
                (void)dw_buf_int(&b1);
            n++;
        }
        if (BURROW_FAILED(b1.err)) {
            BURROW_OUT(err, b1.err);
            return NULL;
        }

        /* And again, writing them down. */
        DwAbbrev *a = DW_NEW(d->a, DwAbbrev);
        a->tag = (DwarfTag)dw_buf_uint(&b);
        a->children = dw_buf_uint8(&b) != 0;
        a->nfield = n;
        a->field = (DwAbbrevField *)burrow__dwarf_alloc(
            d->a, (size_t)n * sizeof(DwAbbrevField));
        for (Int i = 0; i < n; i++) {
            DwAbbrevField *f = &a->field[i];
            f->attr = (DwarfAttr)dw_buf_uint(&b);
            f->fmt = (uint32_t)dw_buf_uint(&b);
            f->cls = dw_form_to_class(f->fmt, f->attr, vers, &b);
            if (f->fmt == DW_FORM_IMPLICIT_CONST)
                f->val = dw_buf_int(&b);
        }
        (void)dw_buf_uint(&b);
        (void)dw_buf_uint(&b);
        dw_map_put(m, &id, a);
    }
    if (BURROW_FAILED(b.err)) {
        BURROW_OUT(err, b.err);
        return NULL;
    }
    dw_map_put(d->abbrev_cache, &off, m);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return m;
}

/* ----------------------------------------------------------------- entries */

/* Where a field's value lives. Each entry has one of these per field, next to
 * the fields, and each field's Any points at its own. */
typedef union DwVal {
    uint64_t u64;
    uint32_t u32;
    int64_t i64;
    bool b;
    DwarfOffset off;
    Str s;
    Bytes bytes;
} DwVal;

static Bytes dw_bytes(Byte *p, Int n) {
    Bytes s = {p, p != NULL ? n : 0, p != NULL ? n : 0, TYPE_BYTE};
    return s;
}

static Str dw_hex_text(DwMsg *m, Str prefix, uint64_t v) {
    dw_msg_str(m, prefix);
    dw_msg_uint_base(m, v, 16);
    return dw_msg_text(m);
}

static uint64_t dw_debug_addr(DwarfData *d, const DwUnit *format, uint64_t addr_base,
                              uint64_t idx, Error *err) {
    uint64_t off = idx * (uint64_t)dw_format_addrsize(format) + addr_base;
    if (!dw_fits_int(off)) {
        *err = burrow__dwarf_error(d, DW_S("offset out of range"));
        return 0;
    }
    DwBuf b = dw_make_buf(d, format, DW_S("addr"), 0, (Byte *)d->addr.p, d->addr.len);
    dw_buf_skip(&b, (Int)off);
    uint64_t val = dw_buf_addr(&b);
    if (BURROW_FAILED(b.err)) {
        *err = b.err;
        return 0;
    }
    return val;
}

/* resolveStrx: the string at index off, already scaled, of the unit's string
 * offsets. */
static Str dw_resolve_strx(DwBuf *b, uint64_t str_base, uint64_t off) {
    DwarfData *d = b->dwarf;
    off += str_base;
    if (!dw_fits_int(off))
        burrow__dwarf_buf_error(b, DW_S("DW_FORM_strx offset out of range"));
    DwBuf b1 = dw_make_buf(d, b->format, DW_S("str_offsets"), 0,
                           (Byte *)d->str_offsets.p, d->str_offsets.len);
    dw_buf_skip(&b1, (Int)off);
    bool known;
    bool is64 = dw_format_dwarf64(b->format, &known);
    off = is64 ? dw_buf_uint64(&b1) : dw_buf_uint32(&b1);
    if (BURROW_FAILED(b1.err)) {
        b->err = b1.err;
        return BURROW_STR_EMPTY;
    }
    if (!dw_fits_int(off))
        burrow__dwarf_buf_error(b, DW_S("DW_FORM_strx indirect offset out of range"));
    b1 = dw_make_buf(d, b->format, DW_S("str"), 0, (Byte *)d->str.p, d->str.len);
    dw_buf_skip(&b1, (Int)off);
    Str val = dw_buf_string(&b1);
    if (BURROW_FAILED(b1.err))
        b->err = b1.err;
    return val;
}

/* resolveRnglistx: the offset of range list off of the unit's. */
static uint64_t dw_resolve_rnglistx(DwBuf *b, uint64_t rnglists_base, uint64_t off) {
    DwarfData *d = b->dwarf;
    bool known;
    bool is64 = dw_format_dwarf64(b->format, &known);
    off *= is64 ? 8 : 4;
    off += rnglists_base;
    if (!dw_fits_int(off))
        burrow__dwarf_buf_error(b, DW_S("DW_FORM_rnglistx offset out of range"));
    DwBuf b1 = dw_make_buf(d, b->format, DW_S("rnglists"), 0, (Byte *)d->rng_lists.p,
                           d->rng_lists.len);
    dw_buf_skip(&b1, (Int)off);
    off = is64 ? dw_buf_uint64(&b1) : dw_buf_uint32(&b1);
    if (BURROW_FAILED(b1.err)) {
        b->err = b1.err;
        return 0;
    }
    if (!dw_fits_int(off))
        burrow__dwarf_buf_error(b,
                                DW_S("DW_FORM_rnglistx indirect offset out of range"));
    return rnglists_base + off;
}

static uint64_t dw_unit_addr_base(const DwUnit *u) {
    return u->unit5 != NULL ? u->unit5->addr_base : 0;
}

static uint64_t dw_unit_str_offsets_base(const DwUnit *u) {
    return u->unit5 != NULL ? u->unit5->str_offsets_base : 0;
}

static uint64_t dw_unit_rng_lists_base(const DwUnit *u) {
    return u->unit5 != NULL ? u->unit5->rng_lists_base : 0;
}

/* The index forms all read an unsigned number of some width. */
static uint64_t dw_buf_index(DwBuf *b, uint32_t fmt) {
    switch (fmt) {
    case DW_FORM_ADDRX1:
    case DW_FORM_STRX1:
        return dw_buf_uint8(b);
    case DW_FORM_ADDRX2:
    case DW_FORM_STRX2:
        return dw_buf_uint16(b);
    case DW_FORM_ADDRX3:
    case DW_FORM_STRX3:
        return dw_buf_uint24(b);
    case DW_FORM_ADDRX4:
    case DW_FORM_STRX4:
        return dw_buf_uint32(b);
    default:
        return dw_buf_uint(b);
    }
}

DwarfEntry *burrow__dwarf_entry(DwBuf *b, Alloc *a, const DwUnit *u) {
    DwarfData *d = b->dwarf;
    DwarfOffset ubase = u->base;
    Int vers = u->vers;
    DwarfOffset off = b->off;
    uint32_t id = (uint32_t)dw_buf_uint(b);
    if (id == 0)
        return DW_NEW(a, DwarfEntry);
    DwAbbrev *ab = (DwAbbrev *)dw_map_lookup(u->atable, &id);
    if (ab == NULL) {
        burrow__dwarf_buf_error(b, DW_S("unknown abbreviation table index"));
        return NULL;
    }

    Int n = ab->nfield;
    DwarfEntry *e = DW_NEW(a, DwarfEntry);
    DwarfField *fields =
        (DwarfField *)burrow__dwarf_alloc(a, (size_t)n * sizeof(DwarfField));
    DwVal *vals = (DwVal *)burrow__dwarf_alloc(a, (size_t)n * sizeof(DwVal));
    e->offset = off;
    e->tag = ab->tag;
    e->children = ab->children;
    e->field = slice_from(fields, n, n, TYPE_DWARF_FIELD);

    for (Int i = 0; i < n; i++) {
        DwarfField *f = &fields[i];
        DwVal *v = &vals[i];
        f->attr = ab->field[i].attr;
        f->cls = ab->field[i].cls;
        uint32_t fmt = ab->field[i].fmt;
        if (fmt == DW_FORM_INDIRECT) {
            fmt = (uint32_t)dw_buf_uint(b);
            f->cls = dw_form_to_class(fmt, ab->field[i].attr, vers, b);
        }
        bool known;
        bool is64;
        DwMsg m = {.len = 0};
        switch (fmt) {
        /* address */
        case DW_FORM_ADDR:
            v->u64 = dw_buf_addr(b);
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;
        case DW_FORM_ADDRX:
        case DW_FORM_ADDRX1:
        case DW_FORM_ADDRX2:
        case DW_FORM_ADDRX3:
        case DW_FORM_ADDRX4: {
            uint64_t aoff = dw_buf_index(b, fmt);
            if (d->addr.p == NULL)
                burrow__dwarf_buf_error(
                    b, DW_S("DW_FORM_addrx with no .debug_addr section"));
            if (BURROW_FAILED(b->err))
                return NULL;
            Error err = BURROW_NO_ERROR;
            v->u64 = dw_debug_addr(d, b->format, dw_unit_addr_base(u), aoff, &err);
            if (BURROW_FAILED(err)) {
                if (BURROW_OK(b->err))
                    b->err = err;
                return NULL;
            }
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;
        }

        /* block */
        case DW_FORM_DWARF_BLOCK1: {
            Int bn = dw_buf_uint8(b);
            v->bytes = dw_bytes(dw_buf_bytes(b, bn), bn);
            f->val = BURROW_ANY(TYPE_BYTES, &v->bytes);
            break;
        }
        case DW_FORM_DWARF_BLOCK2: {
            Int bn = dw_buf_uint16(b);
            v->bytes = dw_bytes(dw_buf_bytes(b, bn), bn);
            f->val = BURROW_ANY(TYPE_BYTES, &v->bytes);
            break;
        }
        case DW_FORM_DWARF_BLOCK4: {
            Int bn = (Int)dw_buf_uint32(b);
            v->bytes = dw_bytes(dw_buf_bytes(b, bn), bn);
            f->val = BURROW_ANY(TYPE_BYTES, &v->bytes);
            break;
        }
        case DW_FORM_DWARF_BLOCK:
        case DW_FORM_EXPRLOC: {
            Int bn = (Int)dw_buf_uint(b);
            v->bytes = dw_bytes(dw_buf_bytes(b, bn), bn);
            f->val = BURROW_ANY(TYPE_BYTES, &v->bytes);
            break;
        }

        /* constant */
        case DW_FORM_DATA1:
            v->i64 = dw_buf_uint8(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_DATA2:
            v->i64 = dw_buf_uint16(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_DATA4:
            v->i64 = dw_buf_uint32(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_DATA8:
            v->i64 = (int64_t)dw_buf_uint64(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_DATA16:
            v->bytes = dw_bytes(dw_buf_bytes(b, 16), 16);
            f->val = BURROW_ANY(TYPE_BYTES, &v->bytes);
            break;
        case DW_FORM_SDATA:
            v->i64 = dw_buf_int(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_UDATA:
            v->i64 = (int64_t)dw_buf_uint(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;
        case DW_FORM_IMPLICIT_CONST:
            v->i64 = ab->field[i].val;
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;

        /* flag */
        case DW_FORM_FLAG:
            v->b = dw_buf_uint8(b) == 1;
            f->val = BURROW_ANY(TYPE_BOOL, &v->b);
            break;
        case DW_FORM_FLAG_PRESENT:
            /* DWARF 4: present, with nothing written for it. */
            v->b = true;
            f->val = BURROW_ANY(TYPE_BOOL, &v->b);
            break;

        /* reference to another entry */
        case DW_FORM_REF_ADDR: {
            Int fvers = dw_format_version(b->format);
            if (fvers == 0) {
                burrow__dwarf_buf_error(b,
                                        DW_S("unknown version for DW_FORM_ref_addr"));
                break;
            }
            if (fvers == 2) {
                v->off = (DwarfOffset)dw_buf_addr(b);
            } else {
                is64 = dw_format_dwarf64(b->format, &known);
                if (!known) {
                    burrow__dwarf_buf_error(b,
                                            DW_S("unknown size for DW_FORM_ref_addr"));
                    break;
                }
                v->off = is64 ? (DwarfOffset)dw_buf_uint64(b) : dw_buf_uint32(b);
            }
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;
        }
        case DW_FORM_REF1:
            v->off = (DwarfOffset)dw_buf_uint8(b) + ubase;
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;
        case DW_FORM_REF2:
            v->off = (DwarfOffset)dw_buf_uint16(b) + ubase;
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;
        case DW_FORM_REF4:
            v->off = dw_buf_uint32(b) + ubase;
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;
        case DW_FORM_REF8:
            v->off = (DwarfOffset)dw_buf_uint64(b) + ubase;
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;
        case DW_FORM_REF_UDATA:
            v->off = (DwarfOffset)dw_buf_uint(b) + ubase;
            f->val = BURROW_ANY(TYPE_DWARF_OFFSET, &v->off);
            break;

        /* string */
        case DW_FORM_STRING:
            v->s = dw_buf_string(b);
            f->val = BURROW_ANY(TYPE_STRING, &v->s);
            break;
        case DW_FORM_STRP:
        case DW_FORM_LINE_STRP: {
            uint64_t soff = 0; /* into .debug_str */
            is64 = dw_format_dwarf64(b->format, &known);
            if (!known)
                burrow__dwarf_buf_error(
                    b, DW_S("unknown size for DW_FORM_strp/line_strp"));
            else
                soff = is64 ? dw_buf_uint64(b) : dw_buf_uint32(b);
            if (!dw_fits_int(soff))
                burrow__dwarf_buf_error(
                    b, DW_S("DW_FORM_strp/line_strp offset out of range"));
            if (BURROW_FAILED(b->err))
                return NULL;
            DwBuf b1;
            if (fmt == DW_FORM_STRP) {
                b1 = dw_make_buf(d, b->format, DW_S("str"), 0, (Byte *)d->str.p,
                                 d->str.len);
            } else {
                if (d->line_str.len == 0) {
                    burrow__dwarf_buf_error(
                        b, DW_S("DW_FORM_line_strp with no .debug_line_str section"));
                    return NULL;
                }
                b1 = dw_make_buf(d, b->format, DW_S("line_str"), 0,
                                 (Byte *)d->line_str.p, d->line_str.len);
            }
            dw_buf_skip(&b1, (Int)soff);
            v->s = dw_buf_string(&b1);
            if (BURROW_FAILED(b1.err)) {
                b->err = b1.err;
                return NULL;
            }
            f->val = BURROW_ANY(TYPE_STRING, &v->s);
            break;
        }
        case DW_FORM_STRX:
        case DW_FORM_STRX1:
        case DW_FORM_STRX2:
        case DW_FORM_STRX3:
        case DW_FORM_STRX4: {
            uint64_t soff = dw_buf_index(b, fmt);
            if (d->str_offsets.len == 0)
                burrow__dwarf_buf_error(
                    b, DW_S("DW_FORM_strx with no .debug_str_offsets section"));
            is64 = dw_format_dwarf64(b->format, &known);
            if (!known)
                burrow__dwarf_buf_error(b,
                                        DW_S("unknown offset size for DW_FORM_strx"));
            if (BURROW_FAILED(b->err))
                return NULL;
            soff *= is64 ? 8 : 4;
            v->s = dw_resolve_strx(b, dw_unit_str_offsets_base(u), soff);
            f->val = BURROW_ANY(TYPE_STRING, &v->s);
            break;
        }
        case DW_FORM_STRP_SUP:
            is64 = dw_format_dwarf64(b->format, &known);
            if (!known) {
                burrow__dwarf_buf_error(b, DW_S("unknown size for DW_FORM_strp_sup"));
            } else if (is64) {
                v->u64 = dw_buf_uint64(b);
                f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            } else {
                v->u32 = dw_buf_uint32(b);
                f->val = BURROW_ANY(TYPE_UINT32, &v->u32);
            }
            break;

        /* lineptr, loclistptr, macptr, rangelistptr. DWARF 4 put these in
         * place of the data4 and data8 forms, and clang writes them for
         * -gdwarf-2 too. */
        case DW_FORM_SEC_OFFSET:
        case DW_FORM_GNU_REF_ALT:
        case DW_FORM_GNU_STRP_ALT:
            is64 = dw_format_dwarf64(b->format, &known);
            if (!known) {
                burrow__dwarf_buf_error(
                    b, dw_hex_text(&m, DW_S("unknown size for form 0x"), fmt));
                break;
            }
            v->i64 = is64 ? (int64_t)dw_buf_uint64(b) : (int64_t)dw_buf_uint32(b);
            f->val = BURROW_ANY(TYPE_INT64, &v->i64);
            break;

        /* reference, DWARF 4 */
        case DW_FORM_REF_SIG8:
            /* A 64-bit type signature. */
            v->u64 = dw_buf_uint64(b);
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;
        case DW_FORM_REF_SUP4:
            v->u32 = dw_buf_uint32(b);
            f->val = BURROW_ANY(TYPE_UINT32, &v->u32);
            break;
        case DW_FORM_REF_SUP8:
            v->u64 = dw_buf_uint64(b);
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;

        /* loclist */
        case DW_FORM_LOCLISTX:
            v->u64 = dw_buf_uint(b);
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;

        /* rnglist */
        case DW_FORM_RNGLISTX: {
            uint64_t roff = dw_buf_uint(b);
            v->u64 = dw_resolve_rnglistx(b, dw_unit_rng_lists_base(u), roff);
            f->val = BURROW_ANY(TYPE_UINT64, &v->u64);
            break;
        }

        default:
            burrow__dwarf_buf_error(
                b, dw_hex_text(&m, DW_S("unknown entry attr format 0x"), fmt));
            break;
        }
    }
    if (BURROW_FAILED(b->err))
        return NULL;
    return e;
}

DwarfField *dwarf_entry_attr_field(const DwarfEntry *e, DwarfAttr a) {
    DwarfField *f = (DwarfField *)e->field.p;
    for (Int i = 0; i < e->field.len; i++) {
        if (f[i].attr == a)
            return &f[i];
    }
    return NULL;
}

Any dwarf_entry_val(const DwarfEntry *e, DwarfAttr a) {
    DwarfField *f = dwarf_entry_attr_field(e, a);
    if (f != NULL)
        return f->val;
    return (Any){NULL, NULL};
}

void *burrow__dwarf_entry_val_as(const DwarfEntry *e, DwarfAttr a, const Type *t) {
    return any_assert(dwarf_entry_val(e, a), t);
}

/* ------------------------------------------------------------------ reader */

/* Data.collectDwarf5BaseOffsets: the bases a DWARF 5 unit's first entry
 * gives. */
static Error dw_collect_dwarf5_base_offsets(DwarfData *d, Alloc *a, DwUnit *u) {
    DwBuf b = dw_make_buf(d, u, DW_S("info"), u->off, u->data, u->len);
    DwarfEntry *cu = burrow__dwarf_entry(&b, a, u);
    if (cu == NULL) {
        /* A bad abbreviation or worse. Something later will say so. */
        return b.err;
    }
    int64_t *v =
        (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_ADDR_BASE, TYPE_INT64);
    if (v != NULL)
        u->unit5->addr_base = (uint64_t)*v;
    v = (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_STR_OFFSETS_BASE,
                                              TYPE_INT64);
    if (v != NULL)
        u->unit5->str_offsets_base = (uint64_t)*v;
    v = (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_RNGLISTS_BASE, TYPE_INT64);
    if (v != NULL)
        u->unit5->rng_lists_base = (uint64_t)*v;
    v = (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_LOCLISTS_BASE, TYPE_INT64);
    if (v != NULL)
        u->unit5->loc_lists_base = (uint64_t)*v;
    return BURROW_NO_ERROR;
}

static void dw_reader_collect(DwarfReader *r, DwUnit *u) {
    if (u->vers < 5 || u->unit5 != NULL)
        return;
    u->unit5 = DW_NEW(r->d->a, DwUnit5);
    Error err = dw_collect_dwarf5_base_offsets(r->d, r->a, u);
    if (BURROW_FAILED(err))
        r->err = err;
}

DwarfReader *dwarf_data_reader(DwarfData *d, Alloc *a) {
    DwarfReader *r = DW_NEW(a, DwarfReader);
    r->d = d;
    r->a = a;
    dwarf_reader_seek(r, 0);
    return r;
}

Int dwarf_reader_address_size(DwarfReader *r) {
    if (r->unit < 0 || r->unit >= r->d->nunit)
        runtime_index_out_of_range(r->unit, r->d->nunit);
    return r->d->unit[r->unit].asize;
}

BinaryByteOrder dwarf_reader_byte_order(DwarfReader *r) {
    /* Go hands back the order of the reader's buffer, which is nil until a
     * unit has been seen. */
    if (r->b.dwarf == NULL) {
        BinaryByteOrder none = {NULL, NULL};
        return none;
    }
    return r->d->order;
}

void dwarf_reader_seek(DwarfReader *r, DwarfOffset off) {
    DwarfData *d = r->d;
    r->err = BURROW_NO_ERROR;
    r->last_children = false;
    if (off == 0) {
        if (d->nunit == 0)
            return;
        DwUnit *u = &d->unit[0];
        r->unit = 0;
        r->b = dw_make_buf(d, u, DW_S("info"), u->off, u->data, u->len);
        dw_reader_collect(r, u);
        r->cu = NULL;
        return;
    }
    Int i = burrow__dwarf_offset_to_unit(d, off);
    if (i == -1) {
        r->err = burrow__dwarf_error(d, DW_S("offset out of range"));
        return;
    }
    if (i != r->unit)
        r->cu = NULL;
    DwUnit *u = &d->unit[i];
    r->unit = i;
    Int skip = (Int)(off - u->off);
    r->b = dw_make_buf(d, u, DW_S("info"), off, u->data + skip, u->len - skip);
    dw_reader_collect(r, u);
}

/* nextUnit. */
static void dw_reader_next_unit(DwarfReader *r) {
    r->unit++;
    DwUnit *u = &r->d->unit[r->unit];
    r->b = dw_make_buf(r->d, u, DW_S("info"), u->off, u->data, u->len);
    r->cu = NULL;
    dw_reader_collect(r, u);
}

/* maybeNextUnit: on to the next unit if this one is done. */
static void dw_reader_maybe_next_unit(DwarfReader *r) {
    while (r->b.len == 0 && r->unit + 1 < r->d->nunit)
        dw_reader_next_unit(r);
}

DwarfEntry *dwarf_reader_next(DwarfReader *r, Error *err) {
    if (BURROW_FAILED(r->err)) {
        BURROW_OUT(err, r->err);
        return NULL;
    }
    dw_reader_maybe_next_unit(r);
    if (r->b.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return NULL;
    }
    DwUnit *u = &r->d->unit[r->unit];
    DwarfEntry *e = burrow__dwarf_entry(&r->b, r->a, u);
    if (BURROW_FAILED(r->b.err)) {
        r->err = r->b.err;
        BURROW_OUT(err, r->err);
        return NULL;
    }
    r->last_unit = false;
    if (e != NULL) {
        r->last_children = e->children;
        if (r->last_children) {
            DwarfOffset *sib = (DwarfOffset *)burrow__dwarf_entry_val_as(
                e, DWARF_ATTR_SIBLING, TYPE_DWARF_OFFSET);
            r->last_sibling = sib != NULL ? *sib : 0;
        }
        if (e->tag == DWARF_TAG_COMPILE_UNIT || e->tag == DWARF_TAG_PARTIAL_UNIT) {
            r->last_unit = true;
            r->cu = e;
        }
    } else {
        r->last_children = false;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return e;
}

void dwarf_reader_skip_children(DwarfReader *r) {
    if (BURROW_FAILED(r->err) || !r->last_children)
        return;

    /* A sibling attribute says where the next sibling is, which saves
     * decoding the children. */
    if (r->last_sibling >= r->b.off) {
        dwarf_reader_seek(r, r->last_sibling);
        return;
    }

    if (r->last_unit && r->unit + 1 < r->d->nunit) {
        dw_reader_next_unit(r);
        return;
    }

    for (;;) {
        Error err = BURROW_NO_ERROR;
        DwarfEntry *e = dwarf_reader_next(r, &err);
        if (BURROW_FAILED(err) || e == NULL || e->tag == 0)
            break;
        if (e->children)
            dwarf_reader_skip_children(r);
    }
}

DwarfEntry *dwarf_reader_seek_pc(DwarfReader *r, uint64_t pc, Error *err) {
    Int unit = r->unit;
    for (Int i = 0; i < r->d->nunit; i++) {
        if (unit >= r->d->nunit)
            unit = 0;
        r->err = BURROW_NO_ERROR;
        r->last_children = false;
        r->unit = unit;
        r->cu = NULL;
        DwUnit *u = &r->d->unit[unit];
        r->b = dw_make_buf(r->d, u, DW_S("info"), u->off, u->data, u->len);
        dw_reader_collect(r, u);
        Error e2 = BURROW_NO_ERROR;
        DwarfEntry *e = dwarf_reader_next(r, &e2);
        if (BURROW_FAILED(e2)) {
            BURROW_OUT(err, e2);
            return NULL;
        }
        if (e == NULL || e->tag == 0) {
            BURROW_OUT(err, dwarf_err_unknown_pc);
            return NULL;
        }
        Slice ranges = dwarf_data_ranges(r->d, r->a, e, &e2);
        if (BURROW_FAILED(e2)) {
            BURROW_OUT(err, e2);
            return NULL;
        }
        const DwarfPCRange *pcs = (const DwarfPCRange *)ranges.p;
        for (Int j = 0; j < ranges.len; j++) {
            if (pcs[j].v[0] <= pc && pc < pcs[j].v[1]) {
                BURROW_OUT(err, BURROW_NO_ERROR);
                return e;
            }
        }
        unit++;
    }
    BURROW_OUT(err, dwarf_err_unknown_pc);
    return NULL;
}

/* ------------------------------------------------------------------ ranges */

static void dw_append_range(Alloc *a, Slice *ret, uint64_t lo, uint64_t hi) {
    DwarfPCRange r = {{lo, hi}};
    Slice next = slice_append(a, *ret, &r, 1);
    if (next.len != ret->len + 1)
        burrow__dwarf_oom();
    *ret = next;
}

/* baseAddressForEntry: where the range lists of e start counting from, which
 * DWARF says is the low pc of its unit. gdb says some versions of GCC used the
 * entry pc instead, so that comes first. */
static Error dw_base_address_for_entry(DwarfData *d, Alloc *a, const DwarfEntry *e,
                                       const DwarfEntry **cu_out, uint64_t *base) {
    const DwarfEntry *cu;
    *cu_out = NULL;
    *base = 0;
    if (e->tag == DWARF_TAG_COMPILE_UNIT) {
        cu = e;
    } else {
        Int i = burrow__dwarf_offset_to_unit(d, e->offset);
        if (i == -1)
            return burrow__dwarf_error(d, DW_S("no unit for entry"));
        DwUnit *u = &d->unit[i];
        DwBuf b = dw_make_buf(d, u, DW_S("info"), u->off, u->data, u->len);
        cu = burrow__dwarf_entry(&b, a, u);
        if (BURROW_FAILED(b.err))
            return b.err;
    }
    *cu_out = cu;
    uint64_t *v =
        (uint64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_ENTRYPC, TYPE_UINT64);
    if (v == NULL)
        v = (uint64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_LOWPC, TYPE_UINT64);
    if (v != NULL)
        *base = *v;
    return BURROW_NO_ERROR;
}

static Error dw_dwarf2_ranges(DwarfData *d, Alloc *a, const DwUnit *u, uint64_t base,
                              int64_t ranges, Slice *ret) {
    if (ranges < 0 || ranges > (int64_t)d->ranges.len) {
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DW_S("invalid range offset "));
        dw_msg_int(&m, ranges);
        dw_msg_str(&m, DW_S(" (max "));
        dw_msg_int(&m, (int64_t)d->ranges.len);
        dw_msg_str(&m, DW_S(")"));
        return burrow__dwarf_error(d, dw_msg_text(&m));
    }
    DwBuf buf = dw_make_buf(d, u, DW_S("ranges"), (DwarfOffset)ranges,
                            (Byte *)d->ranges.p + ranges, d->ranges.len - (Int)ranges);
    while (buf.len > 0) {
        if (u == NULL)
            panic_str(DW_S(
                "runtime error: invalid memory address or nil pointer dereference"));
        uint64_t low = dw_buf_addr(&buf);
        uint64_t high = dw_buf_addr(&buf);
        if (low == 0 && high == 0)
            break;
        Int shift = (8 - u->asize) * 8;
        uint64_t all = shift >= 0 && shift < 64 ? ~(uint64_t)0 >> shift : 0;
        if (low == all)
            base = high;
        else
            dw_append_range(a, ret, base + low, base + high);
    }
    return BURROW_NO_ERROR;
}

/* dwarf5Ranges: a .debug_rnglists list, section 2.17.3 of DWARF 5. */
static Error dw_dwarf5_ranges(DwarfData *d, Alloc *a, const DwUnit *u,
                              const DwarfEntry *cu, uint64_t base, int64_t ranges,
                              Slice *ret) {
    if (ranges < 0 || ranges > (int64_t)d->rng_lists.len) {
        /* Go names the length of the wrong section here, and so does this. */
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DW_S("invalid rnglist offset "));
        dw_msg_int(&m, ranges);
        dw_msg_str(&m, DW_S(" (max "));
        dw_msg_int(&m, (int64_t)d->ranges.len);
        dw_msg_str(&m, DW_S(")"));
        return burrow__dwarf_error(d, dw_msg_text(&m));
    }
    int64_t addr_base = 0;
    if (cu != NULL) {
        int64_t *v =
            (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_ADDR_BASE, TYPE_INT64);
        if (v != NULL)
            addr_base = *v;
    }
    DwBuf buf = dw_make_buf(d, u, DW_S("rnglists"), 0, (Byte *)d->rng_lists.p,
                            d->rng_lists.len);
    dw_buf_skip(&buf, (Int)ranges);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        uint8_t opcode = dw_buf_uint8(&buf);
        switch (opcode) {
        case DW_RLE_END_OF_LIST:
            return buf.err;
        case DW_RLE_BASE_ADDRESSX: {
            uint64_t idx = dw_buf_uint(&buf);
            base = dw_debug_addr(d, u, (uint64_t)addr_base, idx, &err);
            if (BURROW_FAILED(err))
                return err;
            break;
        }
        case DW_RLE_STARTX_ENDX: {
            uint64_t start_idx = dw_buf_uint(&buf);
            uint64_t end_idx = dw_buf_uint(&buf);
            uint64_t start = dw_debug_addr(d, u, (uint64_t)addr_base, start_idx, &err);
            if (BURROW_FAILED(err))
                return err;
            uint64_t end = dw_debug_addr(d, u, (uint64_t)addr_base, end_idx, &err);
            if (BURROW_FAILED(err))
                return err;
            dw_append_range(a, ret, start, end);
            break;
        }
        case DW_RLE_STARTX_LENGTH: {
            uint64_t start_idx = dw_buf_uint(&buf);
            uint64_t len = dw_buf_uint(&buf);
            uint64_t start = dw_debug_addr(d, u, (uint64_t)addr_base, start_idx, &err);
            if (BURROW_FAILED(err))
                return err;
            dw_append_range(a, ret, start, start + len);
            break;
        }
        case DW_RLE_OFFSET_PAIR: {
            uint64_t off1 = dw_buf_uint(&buf);
            uint64_t off2 = dw_buf_uint(&buf);
            dw_append_range(a, ret, base + off1, base + off2);
            break;
        }
        case DW_RLE_BASE_ADDRESS:
            base = dw_buf_addr(&buf);
            break;
        case DW_RLE_START_END: {
            uint64_t start = dw_buf_addr(&buf);
            uint64_t end = dw_buf_addr(&buf);
            dw_append_range(a, ret, start, end);
            break;
        }
        case DW_RLE_START_LENGTH: {
            uint64_t start = dw_buf_addr(&buf);
            uint64_t len = dw_buf_uint(&buf);
            dw_append_range(a, ret, start, start + len);
            break;
        }
        default:
            break;
        }
    }
}

Slice dwarf_data_ranges(DwarfData *d, Alloc *a, const DwarfEntry *e, Error *err) {
    Slice ret = slice_nil(TYPE_OF(DwarfPCRange));
    Error e2 = BURROW_NO_ERROR;
    uint64_t low = 0;
    uint64_t high = 0;
    uint64_t *lowp =
        (uint64_t *)burrow__dwarf_entry_val_as(e, DWARF_ATTR_LOWPC, TYPE_UINT64);
    bool high_ok = false;
    if (lowp != NULL)
        low = *lowp;
    DwarfField *high_field = dwarf_entry_attr_field(e, DWARF_ATTR_HIGHPC);
    if (high_field != NULL) {
        if (high_field->cls == DWARF_CLASS_ADDRESS) {
            uint64_t *hp = (uint64_t *)any_assert(high_field->val, TYPE_UINT64);
            if (hp != NULL) {
                high = *hp;
                high_ok = true;
            }
        } else if (high_field->cls == DWARF_CLASS_CONSTANT) {
            int64_t *off = (int64_t *)any_assert(high_field->val, TYPE_INT64);
            if (off != NULL) {
                high = low + (uint64_t)*off;
                high_ok = true;
            }
        }
    }
    if (lowp != NULL && high_ok)
        dw_append_range(a, &ret, low, high);

    const DwUnit *u = NULL;
    Int uidx = burrow__dwarf_offset_to_unit(d, e->offset);
    if (uidx >= 0 && uidx < d->nunit)
        u = &d->unit[uidx];

    const DwarfEntry *cu = NULL;
    uint64_t base = 0;
    if (u != NULL && u->vers >= 5 && d->rng_lists.p != NULL) {
        /* DWARF 5 and later. */
        DwarfField *field = dwarf_entry_attr_field(e, DWARF_ATTR_RANGES);
        if (field == NULL) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return ret;
        }
        int64_t ranges;
        if (field->cls == DWARF_CLASS_RANGE_LIST_PTR) {
            int64_t *v = (int64_t *)any_assert(field->val, TYPE_INT64);
            if (v == NULL) {
                BURROW_OUT(err, BURROW_NO_ERROR);
                return ret;
            }
            ranges = *v;
        } else if (field->cls == DWARF_CLASS_RNG_LIST) {
            uint64_t *v = (uint64_t *)any_assert(field->val, TYPE_UINT64);
            if (v == NULL) {
                BURROW_OUT(err, BURROW_NO_ERROR);
                return ret;
            }
            ranges = (int64_t)*v;
        } else {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return ret;
        }
        e2 = dw_base_address_for_entry(d, a, e, &cu, &base);
        if (BURROW_OK(e2))
            e2 = dw_dwarf5_ranges(d, a, u, cu, base, ranges, &ret);
        if (BURROW_FAILED(e2)) {
            BURROW_OUT(err, e2);
            return slice_nil(TYPE_OF(DwarfPCRange));
        }
        BURROW_OUT(err, BURROW_NO_ERROR);
        return ret;
    }

    /* DWARF 2 to 4. */
    int64_t *ranges =
        (int64_t *)burrow__dwarf_entry_val_as(e, DWARF_ATTR_RANGES, TYPE_INT64);
    if (ranges != NULL && d->ranges.p != NULL) {
        e2 = dw_base_address_for_entry(d, a, e, &cu, &base);
        if (BURROW_OK(e2))
            e2 = dw_dwarf2_ranges(d, a, u, base, *ranges, &ret);
        if (BURROW_FAILED(e2)) {
            BURROW_OUT(err, e2);
            return slice_nil(TYPE_OF(DwarfPCRange));
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}
