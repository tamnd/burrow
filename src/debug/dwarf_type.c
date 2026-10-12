/* debug/dwarf: the types the info section and the type units describe, and
 * their String and Size methods.
 *
 * Derived from Go's src/debug/dwarf/type.go and typeunit.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "dwarf_internal.h"

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/stack.h"
#include "burrow/strings.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DWT_S(lit) BURROW_S(lit)

/* ----------------------------------------------------------- stack depth */

/* Go reads nested types by recursion on a stack that grows to a gigabyte, and
 * prints them the same way. A C stack is a few megabytes, so both watch how
 * much is left: reading stops with an error, and printing, which in Go only
 * runs out on a type that contains itself, dies the way Go does then. Where
 * the stack cannot be measured a count stands in for it. */
enum {
    DWT_MAX_DEPTH = 10000,
    DWT_THREAD_CHECK_AFTER = 32,
};

#define DWT_STACK_MARGIN ((uintptr_t)64 << 10)

static uintptr_t dwt_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

/* Whether the stack is too low to go one level deeper. *floor starts at 0. */
static bool dwt_stack_low(uintptr_t *floor, int depth) {
    if (*floor == 0) {
        burrow__Stack *s = burrow__stack_current();
        if (s != NULL && s->lo != NULL)
            *floor = (uintptr_t)s->lo + DWT_STACK_MARGIN;
        else
            *floor = 2;
    }
    if (*floor == 2) {
        if (depth < DWT_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        if (!burrow__thread_stack_limit(&lo))
            lo = NULL;
        *floor = lo == NULL ? 1 : (uintptr_t)lo + DWT_STACK_MARGIN;
    }
    if (*floor == 1)
        return depth > DWT_MAX_DEPTH;
    return dwt_stack_here() < *floor;
}

/* ------------------------------------------------------------------- Size */

/* The type a chain of qualifiers, typedefs, arrays and pointers goes on to,
 * or NULL at the end of one. */
static DwarfType dwt_inner(DwarfType t) {
    switch (t->type_kind) {
    case DWARF_TYPE_KIND_QUAL:
        return ((DwarfQualType *)t)->type;
    case DWARF_TYPE_KIND_TYPEDEF:
        return ((DwarfTypedefType *)t)->type;
    case DWARF_TYPE_KIND_ARRAY:
        return ((DwarfArrayType *)t)->type;
    case DWARF_TYPE_KIND_PTR:
        return ((DwarfPtrType *)t)->type;
    case DWARF_TYPE_KIND_COMMON:
    case DWARF_TYPE_KIND_ADDR:
    case DWARF_TYPE_KIND_BASIC:
    case DWARF_TYPE_KIND_BOOL:
    case DWARF_TYPE_KIND_CHAR:
    case DWARF_TYPE_KIND_COMPLEX:
    case DWARF_TYPE_KIND_DOT_DOT_DOT:
    case DWARF_TYPE_KIND_ENUM:
    case DWARF_TYPE_KIND_FLOAT:
    case DWARF_TYPE_KIND_FUNC:
    case DWARF_TYPE_KIND_INT:
    case DWARF_TYPE_KIND_STRUCT:
    case DWARF_TYPE_KIND_UCHAR:
    case DWARF_TYPE_KIND_UINT:
    case DWARF_TYPE_KIND_UNSPECIFIED:
    case DWARF_TYPE_KIND_UNSUPPORTED:
    case DWARF_TYPE_KIND_VOID:
    default:
        return NULL;
    }
}

/* A type that leads back to itself is one Go recurses on until the stack is
 * gone. The second pointer moves at half speed and meets the first if so. */
static void dwt_check_cycle(DwarfType *slow, Int steps, DwarfType t) {
    if ((steps & 1) == 0 && *slow != NULL)
        *slow = dwt_inner(*slow);
    if (*slow == t)
        runtime_throw(DWT_S("stack overflow"));
}

int64_t dwarf_type_size(DwarfType t) {
    /* QualType, TypedefType and ArrayType ask the type under them, and wrap
     * around as Go's int64 does, which is the same whichever end the
     * multiplying starts from. */
    uint64_t mul = 1;
    DwarfType slow = t;
    for (Int steps = 1;; steps++) {
        if (t == NULL)
            burrow__dwarf_nil_panic();
        switch (t->type_kind) {
        case DWARF_TYPE_KIND_QUAL:
        case DWARF_TYPE_KIND_TYPEDEF:
            break;
        case DWARF_TYPE_KIND_ARRAY: {
            const DwarfArrayType *at = (const DwarfArrayType *)t;
            if (at->count == -1)
                return 0;
            mul *= (uint64_t)at->count;
            break;
        }
        case DWARF_TYPE_KIND_COMMON:
        case DWARF_TYPE_KIND_ADDR:
        case DWARF_TYPE_KIND_BASIC:
        case DWARF_TYPE_KIND_BOOL:
        case DWARF_TYPE_KIND_CHAR:
        case DWARF_TYPE_KIND_COMPLEX:
        case DWARF_TYPE_KIND_DOT_DOT_DOT:
        case DWARF_TYPE_KIND_ENUM:
        case DWARF_TYPE_KIND_FLOAT:
        case DWARF_TYPE_KIND_FUNC:
        case DWARF_TYPE_KIND_INT:
        case DWARF_TYPE_KIND_PTR:
        case DWARF_TYPE_KIND_STRUCT:
        case DWARF_TYPE_KIND_UCHAR:
        case DWARF_TYPE_KIND_UINT:
        case DWARF_TYPE_KIND_UNSPECIFIED:
        case DWARF_TYPE_KIND_UNSUPPORTED:
        case DWARF_TYPE_KIND_VOID:
        default:
            return (int64_t)(mul * (uint64_t)t->byte_size);
        }
        t = dwt_inner(t);
        dwt_check_cycle(&slow, steps, t);
    }
}

DwarfCommonType *dwarf_type_common(DwarfType t) {
    if (t == NULL)
        burrow__dwarf_nil_panic();
    return t;
}

/* ----------------------------------------------------------------- String */

typedef struct DwtWriter {
    StringsBuilder sb;
    uintptr_t floor;
    int depth;
} DwtWriter;

static void dwt_put(DwtWriter *w, Str s) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_string(&w->sb, s, &err);
    if (BURROW_FAILED(err))
        burrow__dwarf_oom();
}

static void dwt_put_int(DwtWriter *w, int64_t v) {
    DwMsg m = {.len = 0};
    dw_msg_int(&m, v);
    dwt_put(w, dw_msg_text(&m));
}

static void dwt_write(DwtWriter *w, DwarfType t);

static Str dwt_basic_name(const DwarfCommonType *c) {
    return c->name.len > 0 ? c->name : DWT_S("?");
}

static void dwt_write_defn(DwtWriter *w, const DwarfStructType *t) {
    dwt_put(w, t->kind);
    if (t->struct_name.len > 0) {
        dwt_put(w, DWT_S(" "));
        dwt_put(w, t->struct_name);
    }
    if (t->incomplete) {
        dwt_put(w, DWT_S(" /*incomplete*/"));
        return;
    }
    dwt_put(w, DWT_S(" {"));
    DwarfStructField *const *fields = (DwarfStructField *const *)t->field.p;
    for (Int i = 0; i < t->field.len; i++) {
        const DwarfStructField *f = fields[i];
        if (i > 0)
            dwt_put(w, DWT_S("; "));
        dwt_put(w, f->name);
        dwt_put(w, DWT_S(" "));
        dwt_write(w, f->type);
        dwt_put(w, DWT_S("@"));
        dwt_put_int(w, f->byte_offset);
        if (f->bit_size > 0) {
            dwt_put(w, DWT_S(" : "));
            dwt_put_int(w, f->bit_size);
            dwt_put(w, DWT_S("@"));
            dwt_put_int(w, f->bit_offset != 0 ? f->bit_offset : f->data_bit_offset);
        }
    }
    dwt_put(w, DWT_S("}"));
}

static void dwt_write_enum(DwtWriter *w, const DwarfEnumType *t) {
    dwt_put(w, DWT_S("enum"));
    if (t->enum_name.len > 0) {
        dwt_put(w, DWT_S(" "));
        dwt_put(w, t->enum_name);
    }
    dwt_put(w, DWT_S(" {"));
    DwarfEnumValue *const *vals = (DwarfEnumValue *const *)t->val.p;
    for (Int i = 0; i < t->val.len; i++) {
        if (i > 0)
            dwt_put(w, DWT_S("; "));
        dwt_put(w, vals[i]->name);
        dwt_put(w, DWT_S("="));
        dwt_put_int(w, vals[i]->val);
    }
    dwt_put(w, DWT_S("}"));
}

static void dwt_write_func(DwtWriter *w, const DwarfFuncType *t) {
    dwt_put(w, DWT_S("func("));
    DwarfType const *params = (DwarfType const *)t->param_type.p;
    for (Int i = 0; i < t->param_type.len; i++) {
        if (i > 0)
            dwt_put(w, DWT_S(", "));
        dwt_write(w, params[i]);
    }
    dwt_put(w, DWT_S(")"));
    if (t->return_type != NULL) {
        dwt_put(w, DWT_S(" "));
        dwt_write(w, t->return_type);
    }
}

/* What String gives for t. Qualifiers, arrays and pointers only put something
 * in front of the type under them, so a chain of them is a loop, and only the
 * types with parts of their own go deeper. */
static void dwt_write(DwtWriter *w, DwarfType t) {
    w->depth++;
    if (dwt_stack_low(&w->floor, w->depth))
        runtime_throw(DWT_S("stack overflow"));
    DwarfType slow = t;
    for (Int steps = 1;; steps++) {
        if (t == NULL)
            burrow__dwarf_nil_panic();
        switch (t->type_kind) {
        case DWARF_TYPE_KIND_QUAL:
            dwt_put(w, ((const DwarfQualType *)t)->qual);
            dwt_put(w, DWT_S(" "));
            break;
        case DWARF_TYPE_KIND_ARRAY:
            dwt_put(w, DWT_S("["));
            dwt_put_int(w, ((const DwarfArrayType *)t)->count);
            dwt_put(w, DWT_S("]"));
            break;
        case DWARF_TYPE_KIND_PTR:
            dwt_put(w, DWT_S("*"));
            break;
        case DWARF_TYPE_KIND_STRUCT: {
            const DwarfStructType *st = (const DwarfStructType *)t;
            if (st->struct_name.len > 0) {
                dwt_put(w, st->kind);
                dwt_put(w, DWT_S(" "));
                dwt_put(w, st->struct_name);
            } else {
                dwt_write_defn(w, st);
            }
            w->depth--;
            return;
        }
        case DWARF_TYPE_KIND_ENUM:
            dwt_write_enum(w, (const DwarfEnumType *)t);
            w->depth--;
            return;
        case DWARF_TYPE_KIND_FUNC:
            dwt_write_func(w, (const DwarfFuncType *)t);
            w->depth--;
            return;
        case DWARF_TYPE_KIND_TYPEDEF:
        case DWARF_TYPE_KIND_COMMON:
            dwt_put(w, t->name);
            w->depth--;
            return;
        case DWARF_TYPE_KIND_VOID:
            dwt_put(w, DWT_S("void"));
            w->depth--;
            return;
        case DWARF_TYPE_KIND_DOT_DOT_DOT:
            dwt_put(w, DWT_S("..."));
            w->depth--;
            return;
        case DWARF_TYPE_KIND_UNSUPPORTED:
            dwt_put(w, t->name);
            if (t->name.len == 0) {
                dwt_put(w, DWT_S("(unsupported type "));
                DwMsg m = {.len = 0};
                dw_msg_tag(&m, ((const DwarfUnsupportedType *)t)->tag);
                dwt_put(w, dw_msg_text(&m));
                dwt_put(w, DWT_S(")"));
            }
            w->depth--;
            return;
        case DWARF_TYPE_KIND_ADDR:
        case DWARF_TYPE_KIND_BASIC:
        case DWARF_TYPE_KIND_BOOL:
        case DWARF_TYPE_KIND_CHAR:
        case DWARF_TYPE_KIND_COMPLEX:
        case DWARF_TYPE_KIND_FLOAT:
        case DWARF_TYPE_KIND_INT:
        case DWARF_TYPE_KIND_UCHAR:
        case DWARF_TYPE_KIND_UINT:
        case DWARF_TYPE_KIND_UNSPECIFIED:
        default:
            dwt_put(w, dwt_basic_name(t));
            w->depth--;
            return;
        }
        t = dwt_inner(t);
        dwt_check_cycle(&slow, steps, t);
    }
}

Str dwarf_type_string(DwarfType t, Alloc *a) {
    DwtWriter w = {.sb = STRINGS_BUILDER(a), .floor = 0, .depth = 0};
    dwt_write(&w, t);
    return strings_builder_string(&w.sb);
}

Str dwarf_struct_type_defn(DwarfStructType *t, Alloc *a) {
    if (t == NULL)
        burrow__dwarf_nil_panic();
    DwtWriter w = {.sb = STRINGS_BUILDER(a), .floor = 0, .depth = 0};
    dwt_write_defn(&w, t);
    return strings_builder_string(&w.sb);
}

/* ------------------------------------------------- the methods, per struct */

DwarfCommonType *dwarf_common_type_common(DwarfCommonType *c) {
    return dwarf_type_common(c);
}

int64_t dwarf_common_type_size(DwarfCommonType *c) {
    if (c == NULL)
        burrow__dwarf_nil_panic();
    return c->byte_size;
}

DwarfBasicType *dwarf_basic_type_basic(DwarfBasicType *b) {
    return b;
}

DwarfCommonType *dwarf_basic_type_common(DwarfBasicType *b) {
    if (b == NULL)
        burrow__dwarf_nil_panic();
    return &b->common_type;
}

int64_t dwarf_basic_type_size(DwarfBasicType *b) {
    return dwarf_common_type_size(dwarf_basic_type_common(b));
}

Str dwarf_basic_type_string(DwarfBasicType *t, Alloc *a) {
    (void)a;
    return dwt_basic_name(dwarf_basic_type_common(t));
}

/* The basic types differ only in name. */
#define DWT_BASIC_METHODS(T, pre)                                                      \
    DwarfBasicType *pre##_basic(T *t) {                                                \
        if (t == NULL)                                                                 \
            burrow__dwarf_nil_panic();                                                 \
        return &t->basic_type;                                                         \
    }                                                                                  \
    DwarfCommonType *pre##_common(T *t) {                                              \
        return dwarf_basic_type_common(pre##_basic(t));                                \
    }                                                                                  \
    int64_t pre##_size(T *t) {                                                         \
        return dwarf_basic_type_size(pre##_basic(t));                                  \
    }                                                                                  \
    Str pre##_string(T *t, Alloc *a) {                                                 \
        return dwarf_basic_type_string(pre##_basic(t), a);                             \
    }

DWT_BASIC_METHODS(DwarfCharType, dwarf_char_type)
DWT_BASIC_METHODS(DwarfUcharType, dwarf_uchar_type)
DWT_BASIC_METHODS(DwarfIntType, dwarf_int_type)
DWT_BASIC_METHODS(DwarfUintType, dwarf_uint_type)
DWT_BASIC_METHODS(DwarfFloatType, dwarf_float_type)
DWT_BASIC_METHODS(DwarfComplexType, dwarf_complex_type)
DWT_BASIC_METHODS(DwarfBoolType, dwarf_bool_type)
DWT_BASIC_METHODS(DwarfAddrType, dwarf_addr_type)
DWT_BASIC_METHODS(DwarfUnspecifiedType, dwarf_unspecified_type)

/* The others have a CommonType first and take Size and String from the
 * generic functions, which look at type_kind. */
#define DWT_METHODS(T, pre)                                                            \
    DwarfCommonType *pre##_common(T *t) {                                              \
        if (t == NULL)                                                                 \
            burrow__dwarf_nil_panic();                                                 \
        return &t->common_type;                                                        \
    }                                                                                  \
    int64_t pre##_size(T *t) {                                                         \
        return dwarf_type_size(pre##_common(t));                                       \
    }                                                                                  \
    Str pre##_string(T *t, Alloc *a) {                                                 \
        return dwarf_type_string(pre##_common(t), a);                                  \
    }

DWT_METHODS(DwarfQualType, dwarf_qual_type)
DWT_METHODS(DwarfArrayType, dwarf_array_type)
DWT_METHODS(DwarfVoidType, dwarf_void_type)
DWT_METHODS(DwarfPtrType, dwarf_ptr_type)
DWT_METHODS(DwarfStructType, dwarf_struct_type)
DWT_METHODS(DwarfEnumType, dwarf_enum_type)
DWT_METHODS(DwarfFuncType, dwarf_func_type)
DWT_METHODS(DwarfDotDotDotType, dwarf_dot_dot_dot_type)
DWT_METHODS(DwarfTypedefType, dwarf_typedef_type)
DWT_METHODS(DwarfUnsupportedType, dwarf_unsupported_type)

/* ------------------------------------------------------------- type units */

Error burrow__dwarf_parse_types(DwarfData *d, Str name, Slice types) {
    DwBuf b = dw_make_buf(d, NULL, name, 0, (Byte *)types.p, types.len);
    while (b.len > 0) {
        DwarfOffset base = b.off;
        bool is64 = false;
        DwarfOffset n = dw_buf_unit_length(&b, &is64);
        DwarfOffset hdroff = b.off;
        Int vers = dw_buf_uint16(&b);
        if (vers != 4) {
            DwMsg m = {.len = 0};
            dw_msg_str(&m, DWT_S("unsupported DWARF version "));
            dw_msg_int(&m, vers);
            burrow__dwarf_buf_error(&b, dw_msg_text(&m));
            return b.err;
        }
        uint64_t ao = is64 ? dw_buf_uint64(&b) : dw_buf_uint32(&b);
        Error err = BURROW_NO_ERROR;
        Map *atable = burrow__dwarf_parse_abbrev(d, ao, vers, &err);
        if (BURROW_FAILED(err))
            return err;
        uint8_t asize = dw_buf_uint8(&b);
        uint64_t sig = dw_buf_uint64(&b);
        uint32_t toff;
        if (!is64) {
            toff = dw_buf_uint32(&b);
        } else {
            uint64_t to64 = dw_buf_uint64(&b);
            if (to64 != (uint64_t)(uint32_t)to64) {
                burrow__dwarf_buf_error(&b, DWT_S("type unit type offset overflow"));
                return b.err;
            }
            toff = (uint32_t)to64;
        }
        DwTypeUnit *tu = DW_NEW(d->a, DwTypeUnit);
        tu->unit.base = base;
        tu->unit.off = b.off;
        Int dn = (Int)(DwarfOffset)(n - (b.off - hdroff));
        tu->unit.data = dw_buf_bytes(&b, dn);
        tu->unit.len = tu->unit.data != NULL ? dn : 0;
        tu->unit.atable = atable;
        tu->unit.asize = asize;
        tu->unit.vers = vers;
        tu->unit.is64 = is64;
        tu->toff = toff;
        tu->name = name;
        dw_map_put(d->type_sigs, &sig, tu);
        if (BURROW_FAILED(b.err))
            return b.err;
    }
    return BURROW_NO_ERROR;
}

/* typeReader: the info section's Reader, or a typeUnitReader when tu is set.
 * Clones live on the stack of the readType that wants one. */
typedef struct DwtReader {
    DwarfReader info;
    DwTypeUnit *tu;
    DwBuf b;
    Error err;
    Alloc *a; /* where the entries go */
} DwtReader;

static void dwt_reader_init(DwtReader *r, DwarfData *d, DwTypeUnit *tu, Alloc *a) {
    memset(r, 0, sizeof *r);
    r->info.err = BURROW_NO_ERROR;
    r->err = BURROW_NO_ERROR;
    r->tu = tu;
    r->a = a;
    if (tu == NULL) {
        r->info.d = d;
        r->info.a = a;
        dwarf_reader_seek(&r->info, 0);
    } else {
        r->b = dw_make_buf(d, &tu->unit, tu->name, tu->unit.off, tu->unit.data,
                           tu->unit.len);
    }
}

static void dwt_reader_seek(DwtReader *r, DwarfOffset off) {
    if (r->tu == NULL) {
        dwarf_reader_seek(&r->info, off);
        return;
    }
    DwTypeUnit *tu = r->tu;
    r->err = BURROW_NO_ERROR;
    DwarfOffset doff = off - tu->unit.off;
    if ((Int)doff >= tu->unit.len) {
        DwMsg m = {.len = 0};
        dw_msg_str(&m, tu->name);
        dw_msg_str(&m, DWT_S(": offset "));
        dw_msg_uint(&m, doff);
        dw_msg_str(&m, DWT_S(" out of range; max "));
        dw_msg_int(&m, (int64_t)tu->unit.len);
        r->err = burrow__dwarf_error(r->b.dwarf, dw_msg_text(&m));
        return;
    }
    r->b = dw_make_buf(r->b.dwarf, &tu->unit, tu->name, off, tu->unit.data + doff,
                       tu->unit.len - (Int)doff);
}

static DwarfEntry *dwt_reader_next(DwtReader *r, Error *err) {
    if (r->tu == NULL)
        return dwarf_reader_next(&r->info, err);
    if (BURROW_FAILED(r->err)) {
        *err = r->err;
        return NULL;
    }
    if (r->tu->unit.len == 0) {
        *err = BURROW_NO_ERROR;
        return NULL;
    }
    DwarfEntry *e = burrow__dwarf_entry(&r->b, r->a, &r->tu->unit);
    if (BURROW_FAILED(r->b.err)) {
        r->err = r->b.err;
        *err = r->err;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return e;
}

static void dwt_reader_clone(const DwtReader *r, DwtReader *out) {
    dwt_reader_init(out, r->tu == NULL ? r->info.d : r->b.dwarf, r->tu, r->a);
}

static DwarfOffset dwt_reader_offset(const DwtReader *r) {
    return r->tu == NULL ? r->info.b.off : r->b.off;
}

static Int dwt_reader_address_size(DwtReader *r) {
    if (r->tu == NULL)
        return dwarf_reader_address_size(&r->info);
    return r->tu->unit.asize;
}

/* ------------------------------------------------------------- readType */

/* typeFixer: what has to wait until the whole graph is read. The arrays live
 * in the scratch allocator of the outermost readType. */
typedef struct DwtFixer {
    Alloc *a;
    DwarfTypedefType **typedefs;
    Int ntypedef;
    Int captypedef;
    DwarfType **arrays;
    Int narray;
    Int caparray;
    uintptr_t floor;
} DwtFixer;

static void *dwt_grow(Alloc *a, void *p, Int *cap, Int n, size_t size) {
    if (n < *cap)
        return p;
    Int ncap = *cap == 0 ? 8 : *cap * 2;
    void *q =
        mem_realloc(a, p, (size_t)*cap * size, (size_t)ncap * size, BURROW_ALIGN_MAX);
    if (q == NULL)
        burrow__dwarf_oom();
    *cap = ncap;
    return q;
}

static void dwt_fixer_typedef(DwtFixer *fx, DwarfTypedefType *t) {
    fx->typedefs = (DwarfTypedefType **)dwt_grow(
        fx->a, fx->typedefs, &fx->captypedef, fx->ntypedef, sizeof(DwarfTypedefType *));
    fx->typedefs[fx->ntypedef++] = t;
}

/* recordArrayType. */
static void dwt_fixer_array(DwtFixer *fx, DwarfType *t) {
    if (t == NULL || *t == NULL || (*t)->type_kind != DWARF_TYPE_KIND_ARRAY)
        return;
    fx->arrays = (DwarfType **)dwt_grow(fx->a, fx->arrays, &fx->caparray, fx->narray,
                                        sizeof(DwarfType *));
    fx->arrays[fx->narray++] = t;
}

/* apply: the sizes of the typedefs, and the arrays that were really empty. */
static void dwt_fixer_apply(DwarfData *d, DwtFixer *fx) {
    for (Int i = 0; i < fx->ntypedef; i++) {
        DwarfTypedefType *t = fx->typedefs[i];
        t->common_type.byte_size = dwarf_type_size(t->type);
    }
    for (Int i = 0; i < fx->narray; i++) {
        /* zeroArray. DWARF writes a zero length array as one of length 1. */
        DwarfType *slot = fx->arrays[i];
        DwarfArrayType *at = (DwarfArrayType *)*slot;
        if (dwarf_type_size(at->type) == 0)
            continue;
        /* A copy, since the cache still has the original. */
        DwarfArrayType *tt = DW_NEW(d->a, DwarfArrayType);
        *tt = *at;
        tt->count = 0;
        *slot = &tt->common_type;
    }
}

/* What readType's closures share with it. */
typedef struct DwtRead {
    DwarfData *d;
    Str name;
    DwtReader *r;
    Map *cache;
    DwtFixer *fx;
    int depth;
    DwarfEntry *e;
    Int next_depth;
    Error err;
} DwtRead;

static DwarfType dwt_read_type(DwarfData *d, Str name, DwtReader *r, DwarfOffset off,
                               Map *cache, DwtFixer *fx, int depth, Error *err);
static DwarfType dwt_sig_to_type(DwarfData *d, uint64_t sig, Error *err);

/* next: the next child of the entry, skipping whatever is nested deeper, which
 * clang writes sometimes (golang.org/issue/6472). */
static DwarfEntry *dwt_next(DwtRead *s) {
    if (!s->e->children)
        return NULL;
    for (;;) {
        Error err1 = BURROW_NO_ERROR;
        DwarfEntry *kid = dwt_reader_next(s->r, &err1);
        if (BURROW_FAILED(err1)) {
            s->err = err1;
            return NULL;
        }
        if (kid == NULL) {
            s->err =
                burrow__dwarf_decode_error(s->d, s->name, dwt_reader_offset(s->r),
                                           DWT_S("unexpected end of DWARF entries"));
            return NULL;
        }
        if (kid->tag == 0) {
            if (s->next_depth > 0) {
                s->next_depth--;
                continue;
            }
            return NULL;
        }
        if (kid->children)
            s->next_depth++;
        if (s->next_depth > 0)
            continue;
        return kid;
    }
}

static DwarfType dwt_new(DwarfData *d, size_t size, DwarfTypeKind kind) {
    DwarfType t = (DwarfType)burrow__dwarf_alloc(d->a, size);
    t->type_kind = kind;
    return t;
}

/* typeOf: the type e's AttrType names, where no AttrType means void. */
static DwarfType dwt_type_of(DwtRead *s, const DwarfEntry *e) {
    Any tval = dwarf_entry_val(e, DWARF_ATTR_TYPE);
    DwarfOffset *toff = (DwarfOffset *)any_assert(tval, TYPE_DWARF_OFFSET);
    if (toff != NULL) {
        DwtReader c;
        dwt_reader_clone(s->r, &c);
        DwarfType t = dwt_read_type(s->d, s->name, &c, *toff, s->cache, s->fx,
                                    s->depth + 1, &s->err);
        return BURROW_FAILED(s->err) ? NULL : t;
    }
    uint64_t *sig = (uint64_t *)any_assert(tval, TYPE_UINT64);
    if (sig != NULL) {
        DwarfType t = dwt_sig_to_type(s->d, *sig, &s->err);
        return BURROW_FAILED(s->err) ? NULL : t;
    }
    return dwt_new(s->d, sizeof(DwarfVoidType), DWARF_TYPE_KIND_VOID);
}

static int64_t dwt_int(const DwarfEntry *e, DwarfAttr a, bool *ok) {
    int64_t *v = (int64_t *)burrow__dwarf_entry_val_as(e, a, TYPE_INT64);
    if (ok != NULL)
        *ok = v != NULL;
    return v != NULL ? *v : 0;
}

static Str dwt_str(const DwarfEntry *e, DwarfAttr a) {
    Str *v = (Str *)burrow__dwarf_entry_val_as(e, a, TYPE_STRING);
    return v != NULL ? *v : BURROW_STR_EMPTY;
}

static void dwt_append(Alloc *a, Slice *s, void *p) {
    Slice next = slice_append(a, *s, &p, 1);
    if (next.len != s->len + 1)
        burrow__dwarf_oom();
    *s = next;
}

static void dwt_cache_put(DwtRead *s, DwarfOffset off, DwarfType t) {
    dw_map_put(s->cache, &off, t);
}

/* TagArrayType: a multi-dimensional array, DWARF 2 section 5.4. Each child is
 * a TagSubrangeType or TagEnumerationType giving one dimension, left to
 * right. */
static DwarfType dwt_array(DwtRead *s, DwarfOffset off) {
    DwarfData *d = s->d;
    DwarfArrayType *t =
        (DwarfArrayType *)dwt_new(d, sizeof(DwarfArrayType), DWARF_TYPE_KIND_ARRAY);
    dwt_cache_put(s, off, &t->common_type);
    t->type = dwt_type_of(s, s->e);
    if (BURROW_FAILED(s->err))
        return NULL;
    t->stride_bit_size = dwt_int(s->e, DWARF_ATTR_STRIDE_SIZE, NULL);

    Slice dims = slice_nil(TYPE_INT64);
    for (DwarfEntry *kid = dwt_next(s); kid != NULL; kid = dwt_next(s)) {
        if (kid->tag == DWARF_TAG_SUBRANGE_TYPE) {
            bool ok;
            int64_t count = dwt_int(kid, DWARF_ATTR_COUNT, &ok);
            if (!ok) {
                /* Old binaries may have an upper bound instead. */
                count = dwt_int(kid, DWARF_ATTR_UPPER_BOUND, &ok);
                if (ok)
                    count = (int64_t)((uint64_t)count + 1);
                else if (dims.len == 0)
                    count = -1; /* as in x[] */
            }
            Slice next = slice_append(s->fx->a, dims, &count, 1);
            if (next.len != dims.len + 1)
                burrow__dwarf_oom();
            dims = next;
        } else if (kid->tag == DWARF_TAG_ENUMERATION_TYPE) {
            s->err = burrow__dwarf_decode_error(
                d, s->name, kid->offset,
                DWT_S("cannot handle enumeration type as array bound"));
            return NULL;
        }
    }
    /* LLVM writes this for x[]. */
    int64_t none = -1;
    const int64_t *dv = dims.len > 0 ? (const int64_t *)dims.p : &none;
    Int nd = dims.len > 0 ? dims.len : 1;
    t->count = dv[0];
    for (Int i = nd - 1; i >= 1; i--) {
        DwarfArrayType *in =
            (DwarfArrayType *)dwt_new(d, sizeof(DwarfArrayType), DWARF_TYPE_KIND_ARRAY);
        in->type = t->type;
        in->count = dv[i];
        t->type = &in->common_type;
    }
    return &t->common_type;
}

/* TagBaseType, DWARF 2 section 5.1. */
static DwarfType dwt_base(DwtRead *s, DwarfOffset off) {
    DwarfData *d = s->d;
    const DwarfEntry *e = s->e;
    /* Go's name here hides the section's, so the errors name the type. */
    Str name = dwt_str(e, DWARF_ATTR_NAME);
    bool ok;
    int64_t enc = dwt_int(e, DWARF_ATTR_ENCODING, &ok);
    if (!ok) {
        StringsBuilder sb = STRINGS_BUILDER(d->a);
        Error werr = BURROW_NO_ERROR;
        strings_builder_write_string(&sb, DWT_S("missing encoding attribute for "),
                                     &werr);
        strings_builder_write_string(&sb, name, &werr);
        if (BURROW_FAILED(werr))
            burrow__dwarf_oom();
        s->err =
            burrow__dwarf_decode_error(d, name, e->offset, strings_builder_string(&sb));
        return NULL;
    }
    DwarfTypeKind kind;
    switch (enc) {
    case DW_ENC_ADDRESS:
        kind = DWARF_TYPE_KIND_ADDR;
        break;
    case DW_ENC_BOOLEAN:
        kind = DWARF_TYPE_KIND_BOOL;
        break;
    case DW_ENC_COMPLEX_FLOAT:
        kind = DWARF_TYPE_KIND_COMPLEX;
        if (str_eq(name, DWT_S("complex"))) {
            /* clang writes complex for both complex float and complex double,
             * and a byte size that tells them apart (golang.org/issue/8694). */
            int64_t byte_size = dwt_int(e, DWARF_ATTR_BYTE_SIZE, NULL);
            if (byte_size == 8)
                name = DWT_S("complex float");
            else if (byte_size == 16)
                name = DWT_S("complex double");
        }
        break;
    case DW_ENC_FLOAT:
        kind = DWARF_TYPE_KIND_FLOAT;
        break;
    case DW_ENC_SIGNED:
        kind = DWARF_TYPE_KIND_INT;
        break;
    case DW_ENC_UNSIGNED:
        kind = DWARF_TYPE_KIND_UINT;
        break;
    case DW_ENC_SIGNED_CHAR:
        kind = DWARF_TYPE_KIND_CHAR;
        break;
    case DW_ENC_UNSIGNED_CHAR:
        kind = DWARF_TYPE_KIND_UCHAR;
        break;
    default:
        s->err = burrow__dwarf_decode_error(
            d, name, e->offset, DWT_S("unrecognized encoding attribute value"));
        return NULL;
    }
    DwarfBasicType *t = (DwarfBasicType *)dwt_new(d, sizeof(DwarfBasicType), kind);
    dwt_cache_put(s, off, &t->common_type);
    t->common_type.name = name;
    t->bit_size = dwt_int(e, DWARF_ATTR_BIT_SIZE, NULL);
    bool have_bit_offset;
    bool have_data_bit_offset;
    t->bit_offset = dwt_int(e, DWARF_ATTR_BIT_OFFSET, &have_bit_offset);
    t->data_bit_offset = dwt_int(e, DWARF_ATTR_DATA_BIT_OFFSET, &have_data_bit_offset);
    if (have_bit_offset && have_data_bit_offset) {
        s->err = burrow__dwarf_decode_error(d, name, e->offset,
                                            DWT_S("duplicate bit offset attributes"));
        return NULL;
    }
    return &t->common_type;
}

/* TagClassType, TagStructType and TagUnionType, DWARF 2 section 5.5. Each
 * TagMember child is a field. The C++ parts are left out. */
static DwarfType dwt_struct(DwtRead *s, DwarfOffset off) {
    DwarfData *d = s->d;
    const DwarfEntry *e = s->e;
    DwarfStructType *t =
        (DwarfStructType *)dwt_new(d, sizeof(DwarfStructType), DWARF_TYPE_KIND_STRUCT);
    dwt_cache_put(s, off, &t->common_type);
    if (e->tag == DWARF_TAG_CLASS_TYPE)
        t->kind = DWT_S("class");
    else if (e->tag == DWARF_TAG_STRUCT_TYPE)
        t->kind = DWT_S("struct");
    else
        t->kind = DWT_S("union");
    bool is_union = e->tag == DWARF_TAG_UNION_TYPE;
    t->struct_name = dwt_str(e, DWARF_ATTR_NAME);
    t->incomplete = dwarf_entry_val(e, DWARF_ATTR_DECLARATION).t != NULL;
    t->field = slice_make(d->a, TYPE_UNSAFE_POINTER, 0, 8);
    if (t->field.cap != 8)
        burrow__dwarf_oom();

    DwarfType *last_field_type = NULL;
    int64_t last_field_bit_size = 0;
    int64_t last_field_byte_offset = 0;
    for (DwarfEntry *kid = dwt_next(s); kid != NULL; kid = dwt_next(s)) {
        if (kid->tag != DWARF_TAG_MEMBER)
            continue;
        DwarfStructField *f = DW_NEW(d->a, DwarfStructField);
        f->type = dwt_type_of(s, kid);
        if (BURROW_FAILED(s->err))
            return NULL;
        Any loc = dwarf_entry_val(kid, DWARF_ATTR_DATA_MEMBER_LOC);
        Bytes *block = (Bytes *)any_assert(loc, TYPE_BYTES);
        int64_t *num = (int64_t *)any_assert(loc, TYPE_INT64);
        if (block != NULL) {
            /* Go has a TODO here to use the unit's format. */
            DwBuf b = dw_make_buf(d, NULL, DWT_S("location"), 0, (Byte *)block->p,
                                  block->len);
            if (dw_buf_uint8(&b) != DW_OP_PLUS_UCONST) {
                s->err = burrow__dwarf_decode_error(d, s->name, kid->offset,
                                                    DWT_S("unexpected opcode"));
                return NULL;
            }
            f->byte_offset = (int64_t)dw_buf_uint(&b);
            if (BURROW_FAILED(b.err)) {
                s->err = b.err;
                return NULL;
            }
        } else if (num != NULL) {
            f->byte_offset = *num;
        }
        f->name = dwt_str(kid, DWARF_ATTR_NAME);
        f->byte_size = dwt_int(kid, DWARF_ATTR_BYTE_SIZE, NULL);
        bool have_bit_offset;
        bool have_data_bit_offset;
        f->bit_offset = dwt_int(kid, DWARF_ATTR_BIT_OFFSET, &have_bit_offset);
        f->data_bit_offset =
            dwt_int(kid, DWARF_ATTR_DATA_BIT_OFFSET, &have_data_bit_offset);
        if (have_bit_offset && have_data_bit_offset) {
            s->err = burrow__dwarf_decode_error(
                d, s->name, e->offset, DWT_S("duplicate bit offset attributes"));
            return NULL;
        }
        f->bit_size = dwt_int(kid, DWARF_ATTR_BIT_SIZE, NULL);
        dwt_append(d->a, &t->field, f);
        if (last_field_bit_size == 0 && last_field_byte_offset == f->byte_offset &&
            !is_union) {
            /* The last field took no room, so it was an empty array, which
             * DWARF writes as one of length 1. */
            dwt_fixer_array(s->fx, last_field_type);
        }
        last_field_type = &f->type;
        last_field_byte_offset = f->byte_offset;
        last_field_bit_size = f->bit_size;
    }
    if (!is_union) {
        bool ok;
        int64_t b = dwt_int(e, DWARF_ATTR_BYTE_SIZE, &ok);
        if (ok && b == last_field_byte_offset) {
            /* And the same for the last field. */
            dwt_fixer_array(s->fx, last_field_type);
        }
    }
    return &t->common_type;
}

/* TagConstType, TagVolatileType and TagRestrictType, DWARF 2 section 5.2. */
static DwarfType dwt_qual(DwtRead *s, DwarfOffset off) {
    DwarfQualType *t =
        (DwarfQualType *)dwt_new(s->d, sizeof(DwarfQualType), DWARF_TYPE_KIND_QUAL);
    dwt_cache_put(s, off, &t->common_type);
    t->type = dwt_type_of(s, s->e);
    if (BURROW_FAILED(s->err))
        return NULL;
    if (s->e->tag == DWARF_TAG_CONST_TYPE)
        t->qual = DWT_S("const");
    else if (s->e->tag == DWARF_TAG_RESTRICT_TYPE)
        t->qual = DWT_S("restrict");
    else
        t->qual = DWT_S("volatile");
    return &t->common_type;
}

/* TagEnumerationType, DWARF 2 section 5.6. */
static DwarfType dwt_enum(DwtRead *s, DwarfOffset off) {
    DwarfData *d = s->d;
    DwarfEnumType *t =
        (DwarfEnumType *)dwt_new(d, sizeof(DwarfEnumType), DWARF_TYPE_KIND_ENUM);
    dwt_cache_put(s, off, &t->common_type);
    t->enum_name = dwt_str(s->e, DWARF_ATTR_NAME);
    t->val = slice_make(d->a, TYPE_UNSAFE_POINTER, 0, 8);
    if (t->val.cap != 8)
        burrow__dwarf_oom();
    for (DwarfEntry *kid = dwt_next(s); kid != NULL; kid = dwt_next(s)) {
        if (kid->tag != DWARF_TAG_ENUMERATOR)
            continue;
        DwarfEnumValue *f = DW_NEW(d->a, DwarfEnumValue);
        f->name = dwt_str(kid, DWARF_ATTR_NAME);
        f->val = dwt_int(kid, DWARF_ATTR_CONST_VALUE, NULL);
        dwt_append(d->a, &t->val, f);
    }
    return &t->common_type;
}

/* TagPointerType, DWARF 2 section 5.2. void * has no AttrType. */
static DwarfType dwt_ptr(DwtRead *s, DwarfOffset off) {
    DwarfPtrType *t =
        (DwarfPtrType *)dwt_new(s->d, sizeof(DwarfPtrType), DWARF_TYPE_KIND_PTR);
    dwt_cache_put(s, off, &t->common_type);
    if (dwarf_entry_val(s->e, DWARF_ATTR_TYPE).t == NULL) {
        t->type = dwt_new(s->d, sizeof(DwarfVoidType), DWARF_TYPE_KIND_VOID);
        return &t->common_type;
    }
    t->type = dwt_type_of(s, s->e);
    return &t->common_type;
}

/* TagSubroutineType, DWARF 2 section 5.7. The children are the parameters, a
 * TagFormalParameter each and a TagUnspecifiedParameters for a final .... */
static DwarfType dwt_func(DwtRead *s, DwarfOffset off) {
    DwarfData *d = s->d;
    DwarfFuncType *t =
        (DwarfFuncType *)dwt_new(d, sizeof(DwarfFuncType), DWARF_TYPE_KIND_FUNC);
    dwt_cache_put(s, off, &t->common_type);
    t->return_type = dwt_type_of(s, s->e);
    if (BURROW_FAILED(s->err))
        return NULL;
    t->param_type = slice_make(d->a, TYPE_UNSAFE_POINTER, 0, 8);
    if (t->param_type.cap != 8)
        burrow__dwarf_oom();
    for (DwarfEntry *kid = dwt_next(s); kid != NULL; kid = dwt_next(s)) {
        DwarfType tkid;
        if (kid->tag == DWARF_TAG_FORMAL_PARAMETER) {
            tkid = dwt_type_of(s, kid);
            if (BURROW_FAILED(s->err))
                return NULL;
        } else if (kid->tag == DWARF_TAG_UNSPECIFIED_PARAMETERS) {
            tkid = dwt_new(d, sizeof(DwarfDotDotDotType), DWARF_TYPE_KIND_DOT_DOT_DOT);
        } else {
            continue;
        }
        dwt_append(d->a, &t->param_type, tkid);
    }
    return &t->common_type;
}

static DwarfType dwt_read_type(DwarfData *d, Str name, DwtReader *r, DwarfOffset off,
                               Map *cache, DwtFixer *fx, int depth, Error *err) {
    DwarfType cached = (DwarfType)dw_map_lookup(cache, &off);
    if (cached != NULL) {
        *err = BURROW_NO_ERROR;
        return cached;
    }
    if (dwt_stack_low(&fx->floor, depth)) {
        *err =
            burrow__dwarf_decode_error(d, name, off, DWT_S("types nested too deeply"));
        return NULL;
    }
    dwt_reader_seek(r, off);
    Error e2 = BURROW_NO_ERROR;
    DwarfEntry *e = dwt_reader_next(r, &e2);
    if (BURROW_FAILED(e2)) {
        *err = e2;
        return NULL;
    }
    Int address_size = dwt_reader_address_size(r);
    if (e == NULL || e->offset != off) {
        *err = burrow__dwarf_decode_error(d, name, off, DWT_S("no type at offset"));
        return NULL;
    }

    /* Every case puts the type in the cache before reading any type it
     * refers to, which is what ends a circular type. */
    DwtRead s = {d, name, r, cache, fx, depth, e, 0, BURROW_NO_ERROR};
    DwarfType typ;
    switch (e->tag) {
    case DWARF_TAG_ARRAY_TYPE:
        typ = dwt_array(&s, off);
        break;
    case DWARF_TAG_BASE_TYPE:
        typ = dwt_base(&s, off);
        break;
    case DWARF_TAG_CLASS_TYPE:
    case DWARF_TAG_STRUCT_TYPE:
    case DWARF_TAG_UNION_TYPE:
        typ = dwt_struct(&s, off);
        break;
    case DWARF_TAG_CONST_TYPE:
    case DWARF_TAG_VOLATILE_TYPE:
    case DWARF_TAG_RESTRICT_TYPE:
        typ = dwt_qual(&s, off);
        break;
    case DWARF_TAG_ENUMERATION_TYPE:
        typ = dwt_enum(&s, off);
        break;
    case DWARF_TAG_POINTER_TYPE:
        typ = dwt_ptr(&s, off);
        break;
    case DWARF_TAG_SUBROUTINE_TYPE:
        typ = dwt_func(&s, off);
        break;
    case DWARF_TAG_TYPEDEF: {
        /* DWARF 2 section 5.3. */
        DwarfTypedefType *t = (DwarfTypedefType *)dwt_new(d, sizeof(DwarfTypedefType),
                                                          DWARF_TYPE_KIND_TYPEDEF);
        typ = &t->common_type;
        dwt_cache_put(&s, off, typ);
        t->common_type.name = dwt_str(e, DWARF_ATTR_NAME);
        t->type = dwt_type_of(&s, e);
        break;
    }
    case DWARF_TAG_UNSPECIFIED_TYPE: {
        /* DWARF 3 section 5.2. */
        DwarfUnspecifiedType *t = (DwarfUnspecifiedType *)dwt_new(
            d, sizeof(DwarfUnspecifiedType), DWARF_TYPE_KIND_UNSPECIFIED);
        typ = &t->basic_type.common_type;
        dwt_cache_put(&s, off, typ);
        typ->name = dwt_str(e, DWARF_ATTR_NAME);
        break;
    }
    default: {
        /* A kind of type entry this cannot read yet. */
        DwarfUnsupportedType *t = (DwarfUnsupportedType *)dwt_new(
            d, sizeof(DwarfUnsupportedType), DWARF_TYPE_KIND_UNSUPPORTED);
        typ = &t->common_type;
        dwt_cache_put(&s, off, typ);
        t->tag = e->tag;
        typ->name = dwt_str(e, DWARF_ATTR_NAME);
        break;
    }
    }
    if (BURROW_FAILED(s.err)) {
        /* Out of the cache, so that asking again does not find it and call
         * that a success. */
        map_del(cache, &off);
        *err = s.err;
        return NULL;
    }

    bool ok;
    int64_t b = dwt_int(e, DWARF_ATTR_BYTE_SIZE, &ok);
    if (!ok) {
        b = -1;
        if (typ->type_kind == DWARF_TYPE_KIND_TYPEDEF) {
            /* Its size has to wait until the whole graph is there. */
            dwt_fixer_typedef(fx, (DwarfTypedefType *)typ);
        } else if (typ->type_kind == DWARF_TYPE_KIND_PTR) {
            b = (int64_t)address_size;
        }
    }
    typ->byte_size = b;
    *err = BURROW_NO_ERROR;
    return typ;
}

/* The outermost readType, which owns the fixups and the scratch space the
 * entries are read into. A NULL cache means a fresh one. */
static DwarfType dwt_read_type_root(DwarfData *d, Str name, DwTypeUnit *tu, Map *cache,
                                    DwarfOffset off, Error *err) {
    Arena ar;
    arena_init(&ar, d->a, 0);
    DwtFixer fx;
    memset(&fx, 0, sizeof fx);
    fx.a = arena_allocator(&ar);
    if (cache == NULL)
        cache = dw_map_make(fx.a, TYPE_DWARF_OFFSET);
    DwtReader r;
    dwt_reader_init(&r, d, tu, fx.a);
    DwarfType t = dwt_read_type(d, name, &r, off, cache, &fx, 0, err);
    dwt_fixer_apply(d, &fx);
    arena_free(&ar);
    return t;
}

/* sigToType. */
static DwarfType dwt_sig_to_type(DwarfData *d, uint64_t sig, Error *err) {
    DwTypeUnit *tu = (DwTypeUnit *)dw_map_lookup(d->type_sigs, &sig);
    if (tu == NULL) {
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DWT_S("no type unit with signature "));
        dw_msg_uint(&m, sig);
        *err = burrow__dwarf_error(d, dw_msg_text(&m));
        return NULL;
    }
    if (tu->cache != NULL) {
        *err = BURROW_NO_ERROR;
        return tu->cache;
    }
    DwarfType t = dwt_read_type_root(d, tu->name, tu, NULL, tu->toff, err);
    if (BURROW_FAILED(*err))
        return NULL;
    tu->cache = t;
    return t;
}

DwarfType dwarf_data_type(DwarfData *d, DwarfOffset off, Error *err) {
    Error e2 = BURROW_NO_ERROR;
    DwarfType t = dwt_read_type_root(d, DWT_S("info"), NULL, d->type_cache, off, &e2);
    BURROW_OUT(err, e2);
    return t;
}
