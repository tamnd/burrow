/* package reflect, the Value half that reads: kinds, scalars, elements,
 * fields, maps, iterators, Equal and Swapper. Go's reflect/value.go, map.go,
 * iter.go and swapper.go.
 *
 * A ReflectValue always points at its value; nothing here copies one. Go's
 * flagIndir is therefore always set, and the only flags left are whether the
 * value can be written (ADDR) and whether it came out of an unexported field
 * (RO).
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <stddef.h>
#include <string.h>

#include "burrow/chan.h"
#include "burrow/error.h"
#include "burrow/mem/fixed.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/utf8.h"

/* ------------------------------------------------------------- descriptors */

static const Field rf_value_error_fields[] = {
    {{(const Byte *)"Method", 6},
     {NULL, 0},
     &burrow_type_Str,
     offsetof(ReflectValueError, method)},
    {{(const Byte *)"Kind", 4},
     {NULL, 0},
     &burrow_type_int32_t,
     offsetof(ReflectValueError, kind)},
};

const Type burrow_type_ReflectValueError = {
    {(const Byte *)"ValueError", 10},
    {(const Byte *)"reflect", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ReflectValueError),
    (uint16_t)_Alignof(ReflectValueError),
    2,
    0,
    rf_value_error_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x72667665U, /* "rfve" */
    NULL,
};

static const Field rf_value_fields[] = {
    {{(const Byte *)"typ", 3},
     {NULL, 0},
     &burrow_type_UnsafePointer,
     offsetof(ReflectValue, typ)},
    {{(const Byte *)"ptr", 3},
     {NULL, 0},
     &burrow_type_UnsafePointer,
     offsetof(ReflectValue, ptr)},
    {{(const Byte *)"flag", 4},
     {NULL, 0},
     &burrow_type_uint32_t,
     offsetof(ReflectValue, flag)},
};

const Type burrow_type_ReflectValue = {
    {(const Byte *)"Value", 5},
    {(const Byte *)"reflect", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ReflectValue),
    (uint16_t)_Alignof(ReflectValue),
    3,
    0,
    rf_value_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x72667676U, /* "rfvv" */
    NULL,
};

/* []int, for StructField.Index. */
static const Type rf_int_slice = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Int,
    NULL,
    0,
    0x72666973U, /* "rfis" */
    NULL,
};

static const Field rf_struct_field_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     &burrow_type_Str,
     offsetof(ReflectStructField, name)},
    {{(const Byte *)"PkgPath", 7},
     {NULL, 0},
     &burrow_type_Str,
     offsetof(ReflectStructField, pkg_path)},
    {{(const Byte *)"Type", 4},
     {NULL, 0},
     &burrow_type_UnsafePointer,
     offsetof(ReflectStructField, type)},
    {{(const Byte *)"Tag", 3},
     {NULL, 0},
     &burrow_type_Str,
     offsetof(ReflectStructField, tag)},
    {{(const Byte *)"Offset", 6},
     {NULL, 0},
     &burrow_type_Uintptr,
     offsetof(ReflectStructField, offset)},
    {{(const Byte *)"Index", 5},
     {NULL, 0},
     &rf_int_slice,
     offsetof(ReflectStructField, index)},
    {{(const Byte *)"Anonymous", 9},
     {NULL, 0},
     &burrow_type_bool,
     offsetof(ReflectStructField, anonymous)},
};

const Type burrow_type_ReflectStructField = {
    {(const Byte *)"StructField", 11},
    {(const Byte *)"reflect", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ReflectStructField),
    (uint16_t)_Alignof(ReflectStructField),
    7,
    0,
    rf_struct_field_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x72667366U, /* "rfsf" */
    NULL,
};

/* ------------------------------------------------------------------ helpers */

#define RF_RO(v) BURROW__RF_RO_OF(v)

static Kind rf_kind(ReflectValue v) {
    return v.typ == NULL ? KIND_INVALID : v.typ->kind;
}

static void rf_must_be(ReflectValue v, Kind k, const char *method) {
    if (rf_kind(v) != k)
        burrow__reflect_panic_kind(str_from_cstr(method), rf_kind(v));
}

BURROW_NORETURN static void rf_panic_value(const char *method, ReflectValue v) {
    burrow__reflect_panic_kind(str_from_cstr(method), rf_kind(v));
}

static ReflectValue rf_make(const Type *t, const void *p, uint32_t flag) {
    return (ReflectValue){t, (void *)(uintptr_t)p, flag};
}

/* The pointer stored at p, for the kinds that are one. */
static void *rf_word(const void *p) {
    void *w;
    memcpy(&w, p, sizeof w);
    return w;
}

/* The code pointer of the function value at p. */
static Uintptr rf_code(const void *p) {
    void (*f)(void);
    memcpy(&f, p, sizeof f);
    return (Uintptr)f;
}

static int64_t rf_load_int(const void *p, uint32_t size) {
    switch (size) {
    case 1: {
        int8_t x;
        memcpy(&x, p, 1);
        return x;
    }
    case 2: {
        int16_t x;
        memcpy(&x, p, 2);
        return x;
    }
    case 4: {
        int32_t x;
        memcpy(&x, p, 4);
        return x;
    }
    default: {
        int64_t x;
        memcpy(&x, p, 8);
        return x;
    }
    }
}

static uint64_t rf_load_uint(const void *p, uint32_t size) {
    switch (size) {
    case 1: {
        uint8_t x;
        memcpy(&x, p, 1);
        return x;
    }
    case 2: {
        uint16_t x;
        memcpy(&x, p, 2);
        return x;
    }
    case 4: {
        uint32_t x;
        memcpy(&x, p, 4);
        return x;
    }
    default: {
        uint64_t x;
        memcpy(&x, p, 8);
        return x;
    }
    }
}

/* Stores the low size bytes of x at p, the way a conversion to that type
 * would. */
static void rf_store_uint(void *p, uint32_t size, uint64_t x) {
    switch (size) {
    case 1: {
        uint8_t v = (uint8_t)x;
        memcpy(p, &v, 1);
        return;
    }
    case 2: {
        uint16_t v = (uint16_t)x;
        memcpy(p, &v, 2);
        return;
    }
    case 4: {
        uint32_t v = (uint32_t)x;
        memcpy(p, &v, 4);
        return;
    }
    default:
        memcpy(p, &x, 8);
        return;
    }
}

static bool rf_is_int(Kind k) {
    return k >= KIND_INT && k <= KIND_INT64;
}

static bool rf_is_uint(Kind k) {
    return k >= KIND_UINT && k <= KIND_UINTPTR;
}

/* What an interface value holds, for Elem and the rest. */
static ReflectValue rf_unpack(ReflectValue v) {
    const Type *t = v.typ;
    void *p = v.ptr;
    if (!burrow__reflect_unwrap(&t, &p))
        return (ReflectValue){0};
    return rf_make(t, p, RF_RO(v));
}

/* --------------------------------------------------------------- the basics */

ReflectValue reflect_value_of(Any i) {
    const Type *t = i.t;
    void *p = i.data;
    if (t == NULL)
        return (ReflectValue){0};
    while (t->kind == KIND_INTERFACE) {
        const Type *was = t;
        if (!burrow__reflect_unwrap(&t, &p))
            return (ReflectValue){0};
        if (t == was)
            break;
    }
    return rf_make(t, p, 0);
}

ReflectValue reflect_indirect(ReflectValue v) {
    if (rf_kind(v) != KIND_POINTER)
        return v;
    return reflect_value_elem(v);
}

ReflectKind reflect_value_kind(ReflectValue v) {
    return rf_kind(v);
}

ReflectType reflect_value_type(ReflectValue v) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Type"), KIND_INVALID);
    return v.typ;
}

bool reflect_value_is_valid(ReflectValue v) {
    return v.typ != NULL;
}

bool reflect_value_is_nil(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_CHAN:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_UNSAFE_POINTER:
    case KIND_INTERFACE:
    case KIND_SLICE:
        /* The first word of each: the pointer, the vtable or type, the data. */
        return rf_word(v.ptr) == NULL;
    case KIND_FUNC:
        return rf_code(v.ptr) == 0;
    default:
        rf_panic_value("reflect.Value.IsNil", v);
    }
}

bool reflect_value_is_zero(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_BOOL:
        return !reflect_value_bool(v);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return reflect_value_int(v) == 0;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return reflect_value_uint(v) == 0;
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return reflect_value_float(v) == 0;
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        Complex128 c = reflect_value_complex(v);
        return c.re == 0 && c.im == 0;
    }
    case KIND_ARRAY:
        for (Int i = 0; i < (Int)v.typ->len; i++)
            if (!reflect_value_is_zero(reflect_value_index(v, i)))
                return false;
        return true;
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_INTERFACE:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_SLICE:
    case KIND_UNSAFE_POINTER:
        return reflect_value_is_nil(v);
    case KIND_STRING:
        return ((const Str *)v.ptr)->len == 0;
    case KIND_STRUCT:
        for (uint16_t i = 0; i < v.typ->nfield; i++) {
            if (str_eq(v.typ->fields[i].name, BURROW_S("_")))
                continue;
            if (!reflect_value_is_zero(reflect_value_field(v, (Int)i)))
                return false;
        }
        return true;
    default:
        rf_panic_value("reflect.Value.IsZero", v);
    }
}

bool reflect_value_can_addr(ReflectValue v) {
    return (v.flag & BURROW__RF_ADDR) != 0;
}

bool reflect_value_can_set(ReflectValue v) {
    return (v.flag & (BURROW__RF_ADDR | BURROW__RF_RO)) == BURROW__RF_ADDR;
}

bool reflect_value_can_interface(ReflectValue v) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.CanInterface"),
                                   KIND_INVALID);
    return RF_RO(v) == 0;
}

Any reflect_value_interface(ReflectValue v) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Interface"), KIND_INVALID);
    if (RF_RO(v))
        panic_str(BURROW_S("reflect.Value.Interface: cannot return value obtained from "
                           "unexported field or method"));
    if (v.typ->kind == KIND_INTERFACE) {
        if (v.typ == TYPE_ANY)
            return *(const Any *)v.ptr;
        const Type *t = v.typ;
        void *p = v.ptr;
        if (!burrow__reflect_unwrap(&t, &p))
            return (Any){NULL, NULL};
        return BURROW_ANY(t, p);
    }
    return BURROW_ANY(v.typ, v.ptr);
}

ReflectInterfaceData reflect_value_interface_data(ReflectValue v) {
    rf_must_be(v, KIND_INTERFACE, "reflect.Value.InterfaceData");
    ReflectInterfaceData d;
    memcpy(d.word, v.ptr, sizeof d.word);
    return d;
}

/* ------------------------------------------------------------------ scalars */

bool reflect_value_can_int(ReflectValue v) {
    return rf_is_int(rf_kind(v));
}

bool reflect_value_can_uint(ReflectValue v) {
    return rf_is_uint(rf_kind(v));
}

bool reflect_value_can_float(ReflectValue v) {
    Kind k = rf_kind(v);
    return k == KIND_FLOAT32 || k == KIND_FLOAT64;
}

bool reflect_value_can_complex(ReflectValue v) {
    Kind k = rf_kind(v);
    return k == KIND_COMPLEX64 || k == KIND_COMPLEX128;
}

bool reflect_value_bool(ReflectValue v) {
    rf_must_be(v, KIND_BOOL, "reflect.Value.Bool");
    return *(const bool *)v.ptr;
}

int64_t reflect_value_int(ReflectValue v) {
    if (!rf_is_int(rf_kind(v)))
        rf_panic_value("reflect.Value.Int", v);
    return rf_load_int(v.ptr, v.typ->size);
}

uint64_t reflect_value_uint(ReflectValue v) {
    if (!rf_is_uint(rf_kind(v)))
        rf_panic_value("reflect.Value.Uint", v);
    return rf_load_uint(v.ptr, v.typ->size);
}

double reflect_value_float(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_FLOAT32:
        return (double)*(const float *)v.ptr;
    case KIND_FLOAT64:
        return *(const double *)v.ptr;
    default:
        rf_panic_value("reflect.Value.Float", v);
    }
}

Complex128 reflect_value_complex(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_COMPLEX64: {
        const Complex64 *c = v.ptr;
        return (Complex128){(double)c->re, (double)c->im};
    }
    case KIND_COMPLEX128:
        return *(const Complex128 *)v.ptr;
    default:
        rf_panic_value("reflect.Value.Complex", v);
    }
}

Str reflect_value_string(ReflectValue v) {
    if (rf_kind(v) == KIND_STRING)
        return *(const Str *)v.ptr;
    if (v.typ == NULL)
        return BURROW_S("<invalid Value>");
    return burrow__reflect_concat(BURROW_S("<"), reflect_type_string(v.typ),
                                  BURROW_S(" Value>"));
}

Slice reflect_value_bytes(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_SLICE:
        if (v.typ->elem == NULL || v.typ->elem->kind != KIND_UINT8)
            panic_str(BURROW_S("reflect.Value.Bytes of non-byte slice"));
        return *(const Slice *)v.ptr;
    case KIND_ARRAY:
        if (v.typ->elem == NULL || v.typ->elem->kind != KIND_UINT8)
            panic_str(BURROW_S("reflect.Value.Bytes of non-byte array"));
        if (!reflect_value_can_addr(v))
            panic_str(BURROW_S("reflect.Value.Bytes of unaddressable byte array"));
        return slice_from(v.ptr, (Int)v.typ->len, (Int)v.typ->len, v.typ->elem);
    default:
        rf_panic_value("reflect.Value.Bytes", v);
    }
}

bool reflect_value_overflow_int(ReflectValue v, int64_t x) {
    if (!rf_is_int(rf_kind(v)))
        rf_panic_value("reflect.Value.OverflowInt", v);
    return reflect_type_overflow_int(v.typ, x);
}

bool reflect_value_overflow_uint(ReflectValue v, uint64_t x) {
    if (!rf_is_uint(rf_kind(v)))
        rf_panic_value("reflect.Value.OverflowUint", v);
    return reflect_type_overflow_uint(v.typ, x);
}

bool reflect_value_overflow_float(ReflectValue v, double x) {
    if (!reflect_value_can_float(v))
        rf_panic_value("reflect.Value.OverflowFloat", v);
    return reflect_type_overflow_float(v.typ, x);
}

bool reflect_value_overflow_complex(ReflectValue v, Complex128 x) {
    if (!reflect_value_can_complex(v))
        rf_panic_value("reflect.Value.OverflowComplex", v);
    return reflect_type_overflow_complex(v.typ, x);
}

/* ----------------------------------------------------------------- pointers */

static bool rf_pointer(ReflectValue v, Uintptr *out) {
    switch ((int)rf_kind(v)) {
    case KIND_POINTER:
    case KIND_CHAN:
    case KIND_MAP:
    case KIND_UNSAFE_POINTER:
        *out = (Uintptr)rf_word(v.ptr);
        return true;
    case KIND_FUNC:
        *out = rf_code(v.ptr);
        return true;
    case KIND_SLICE:
        *out = (Uintptr)((const Slice *)v.ptr)->p;
        return true;
    case KIND_STRING:
        *out = (Uintptr)((const Str *)v.ptr)->p;
        return true;
    default:
        return false;
    }
}

Uintptr reflect_value_pointer(ReflectValue v) {
    Uintptr p = 0;
    if (!rf_pointer(v, &p))
        rf_panic_value("reflect.Value.Pointer", v);
    return p;
}

void *reflect_value_unsafe_pointer(ReflectValue v) {
    Uintptr p = 0;
    if (!rf_pointer(v, &p))
        rf_panic_value("reflect.Value.UnsafePointer", v);
    return (void *)p;
}

Uintptr reflect_value_unsafe_addr(ReflectValue v) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.UnsafeAddr"), KIND_INVALID);
    if (!reflect_value_can_addr(v))
        panic_str(BURROW_S("reflect.Value.UnsafeAddr of unaddressable value"));
    return (Uintptr)v.ptr;
}

/* ------------------------------------------------------------- collections */

Int reflect_value_len(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_ARRAY:
        return (Int)v.typ->len;
    case KIND_CHAN: {
        const Chan *c = rf_word(v.ptr);
        return c == NULL ? 0 : chan_len(c);
    }
    case KIND_MAP: {
        const Map *m = rf_word(v.ptr);
        return m == NULL ? 0 : map_len(m);
    }
    case KIND_SLICE:
        return ((const Slice *)v.ptr)->len;
    case KIND_STRING:
        return ((const Str *)v.ptr)->len;
    case KIND_POINTER:
        if (v.typ->elem != NULL && v.typ->elem->kind == KIND_ARRAY)
            return (Int)v.typ->elem->len;
        panic_str(
            BURROW_S("reflect: call of reflect.Value.Len on ptr to non-array Value"));
    default:
        rf_panic_value("reflect.Value.Len", v);
    }
}

Int reflect_value_cap(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_ARRAY:
        return (Int)v.typ->len;
    case KIND_CHAN: {
        const Chan *c = rf_word(v.ptr);
        return c == NULL ? 0 : chan_cap(c);
    }
    case KIND_SLICE:
        return ((const Slice *)v.ptr)->cap;
    case KIND_POINTER:
        if (v.typ->elem != NULL && v.typ->elem->kind == KIND_ARRAY)
            return (Int)v.typ->elem->len;
        panic_str(
            BURROW_S("reflect: call of reflect.Value.Cap on ptr to non-array Value"));
    default:
        rf_panic_value("reflect.Value.Cap", v);
    }
}

ReflectValue reflect_value_index(ReflectValue v, Int i) {
    switch ((int)rf_kind(v)) {
    case KIND_ARRAY: {
        if (i < 0 || i >= (Int)v.typ->len)
            panic_str(BURROW_S("reflect: array index out of range"));
        const Type *e = v.typ->elem;
        return rf_make(e, (Byte *)v.ptr + (size_t)i * e->size,
                       (v.flag & BURROW__RF_ADDR) | RF_RO(v));
    }
    case KIND_SLICE: {
        const Slice *s = v.ptr;
        if (i < 0 || i >= s->len)
            panic_str(BURROW_S("reflect: slice index out of range"));
        const Type *e = v.typ->elem;
        return rf_make(e, (Byte *)s->p + (size_t)i * e->size,
                       BURROW__RF_ADDR | RF_RO(v));
    }
    case KIND_STRING: {
        const Str *s = v.ptr;
        if (i < 0 || i >= s->len)
            panic_str(BURROW_S("reflect: string index out of range"));
        return rf_make(TYPE_UINT8, s->p + i, RF_RO(v));
    }
    default:
        rf_panic_value("reflect.Value.Index", v);
    }
}

ReflectValue reflect_value_elem(ReflectValue v) {
    switch ((int)rf_kind(v)) {
    case KIND_INTERFACE:
        return rf_unpack(v);
    case KIND_POINTER: {
        void *p = rf_word(v.ptr);
        if (p == NULL)
            return (ReflectValue){0};
        return rf_make(v.typ->elem, p, BURROW__RF_ADDR | (v.flag & BURROW__RF_RO));
    }
    default:
        rf_panic_value("reflect.Value.Elem", v);
    }
}

/* ------------------------------------------------------------------- fields */

Int reflect_value_num_field(ReflectValue v) {
    rf_must_be(v, KIND_STRUCT, "reflect.Value.NumField");
    return (Int)v.typ->nfield;
}

ReflectValue reflect_value_field(ReflectValue v, Int i) {
    rf_must_be(v, KIND_STRUCT, "reflect.Value.Field");
    if (i < 0 || i >= (Int)v.typ->nfield)
        panic_str(BURROW_S("reflect: Field index out of range"));
    const Field *f = &v.typ->fields[i];
    /* An unexported embedded struct is read-only and its exported fields are
     * not, so only the sticky kind comes down from v. */
    uint32_t flag = v.flag & (BURROW__RF_ADDR | BURROW__RF_STICKY_RO);
    if (!field_is_exported(f))
        flag |= field_is_embedded(f) ? BURROW__RF_EMBED_RO : BURROW__RF_STICKY_RO;
    return rf_make(f->type, (Byte *)v.ptr + f->offset, flag);
}

/* FieldByIndex, which when err is not NULL reports a nil embedded pointer
 * there rather than panicking. */
static ReflectValue rf_field_by_index(ReflectValue v, Slice index, Error *err,
                                      const char *method) {
    const Int *ix = index.p;
    if (index.len == 1)
        return reflect_value_field(v, ix[0]);
    rf_must_be(v, KIND_STRUCT, method);
    for (Int i = 0; i < index.len; i++) {
        if (i > 0 && rf_kind(v) == KIND_POINTER && v.typ->elem != NULL &&
            v.typ->elem->kind == KIND_STRUCT) {
            if (reflect_value_is_nil(v)) {
                if (err == NULL)
                    panic_str(BURROW_S(
                        "reflect: indirection through nil pointer to embedded struct"));
                Str msg = burrow__reflect_concat(
                    BURROW_S("reflect: indirection through nil pointer to embedded "
                             "struct field "),
                    v.typ->elem->name, BURROW_STR_EMPTY);
                *err = errors_new(error_allocator(), msg);
                return (ReflectValue){0};
            }
            v = reflect_value_elem(v);
        }
        v = reflect_value_field(v, ix[i]);
    }
    return v;
}

ReflectValue reflect_value_field_by_index(ReflectValue v, Slice index) {
    return rf_field_by_index(v, index, NULL, "reflect.Value.FieldByIndex");
}

ReflectValue reflect_value_field_by_index_err(ReflectValue v, Slice index, Error *err) {
    *err = BURROW_NO_ERROR;
    return rf_field_by_index(v, index, err, "reflect.Value.FieldByIndexErr");
}

/* The type's search hands back a path; the path lives on this stack for as
 * long as it takes to follow it. */
#define RF_PATH_BYTES 1024

ReflectValue reflect_value_field_by_name(ReflectValue v, Str name) {
    rf_must_be(v, KIND_STRUCT, "reflect.Value.FieldByName");
    _Alignas(16) Byte buf[RF_PATH_BYTES];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    ReflectStructField f;
    if (!reflect_type_field_by_name(fixed_allocator(&fx), v.typ, name, &f))
        return (ReflectValue){0};
    return reflect_value_field_by_index(v, f.index);
}

ReflectValue reflect_value_field_by_name_func(ReflectValue v, ReflectNameMatch match) {
    rf_must_be(v, KIND_STRUCT, "reflect.Value.FieldByNameFunc");
    _Alignas(16) Byte buf[RF_PATH_BYTES];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    ReflectStructField f;
    if (!reflect_type_field_by_name_func(fixed_allocator(&fx), v.typ, match, &f))
        return (ReflectValue){0};
    return reflect_value_field_by_index(v, f.index);
}

/* --------------------------------------------------------------------- maps */

/* Where the key for a map with key type kt is, for key, or a panic like Go's
 * assignTo. An interface key gets boxed into *box. */
static const void *rf_map_key(ReflectValue key, const Type *kt, RfBox *box,
                              const char *context) {
    /* Go's assignTo reads the type of a zero Value without looking. */
    if (key.typ == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    return burrow__reflect_assign(key, kt, box, context);
}

ReflectValue reflect_value_map_index(ReflectValue v, ReflectValue key) {
    rf_must_be(v, KIND_MAP, "reflect.Value.MapIndex");
    RfBox box;
    const void *k = rf_map_key(key, v.typ->key, &box, "reflect.Value.MapIndex");
    Map *m = rf_word(v.ptr);
    if (m == NULL)
        return (ReflectValue){0};
    void *e = map_get(m, k);
    if (e == NULL)
        return (ReflectValue){0};
    return rf_make(v.typ->elem, e, RF_RO(v) | RF_RO(key));
}

Slice reflect_value_map_keys(ReflectValue v, Alloc *a) {
    rf_must_be(v, KIND_MAP, "reflect.Value.MapKeys");
    Map *m = rf_word(v.ptr);
    Int n = m == NULL ? 0 : map_len(m);
    if (n == 0)
        return slice_nil(TYPE_REFLECT_VALUE);
    ReflectValue *out = BURROW_NEW_N(a, ReflectValue, (size_t)n);
    if (out == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    MapIter it = map_iter(m);
    const void *k;
    void *e;
    Int i = 0;
    while (i < n && map_next(&it, &k, &e))
        out[i++] = rf_make(v.typ->key, k, RF_RO(v));
    return (Slice){out, i, n, TYPE_REFLECT_VALUE};
}

enum {
    RF_ITER_FRESH = BURROW__RF_ITER_FRESH,
    RF_ITER_ON = BURROW__RF_ITER_ON,
    RF_ITER_DONE = BURROW__RF_ITER_DONE
};

ReflectMapIter *reflect_value_map_range(ReflectValue v, Alloc *a) {
    rf_must_be(v, KIND_MAP, "reflect.Value.MapRange");
    ReflectMapIter *it = BURROW_NEW(a, ReflectMapIter);
    if (it == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    reflect_map_iter_reset(it, v);
    return it;
}

bool reflect_map_iter_next(ReflectMapIter *it) {
    if (it->m.typ == NULL)
        panic_str(BURROW_S("MapIter.Next called on an iterator that does not have an "
                           "associated map Value"));
    if (it->state == RF_ITER_DONE)
        panic_str(BURROW_S("MapIter.Next called on exhausted iterator"));
    Map *m = rf_word(it->m.ptr);
    if (it->state == RF_ITER_FRESH && m != NULL)
        it->it = map_iter(m);
    if (m != NULL && map_next(&it->it, &it->key, &it->val)) {
        it->state = RF_ITER_ON;
        return true;
    }
    it->state = RF_ITER_DONE;
    return false;
}

ReflectValue reflect_map_iter_key(ReflectMapIter *it) {
    if (it->state == RF_ITER_FRESH)
        panic_str(BURROW_S("MapIter.Key called before Next"));
    if (it->state == RF_ITER_DONE)
        panic_str(BURROW_S("MapIter.Key called on exhausted iterator"));
    return rf_make(it->m.typ->key, it->key, RF_RO(it->m));
}

ReflectValue reflect_map_iter_value(ReflectMapIter *it) {
    if (it->state == RF_ITER_FRESH)
        panic_str(BURROW_S("MapIter.Value called before Next"));
    if (it->state == RF_ITER_DONE)
        panic_str(BURROW_S("MapIter.Value called on exhausted iterator"));
    return rf_make(it->m.typ->elem, it->val, RF_RO(it->m));
}

void reflect_map_iter_reset(ReflectMapIter *it, ReflectValue v) {
    if (v.typ != NULL)
        rf_must_be(v, KIND_MAP, "reflect.MapIter.Reset");
    memset(it, 0, sizeof *it);
    it->m = v;
    it->state = RF_ITER_FRESH;
}

/* ---------------------------------------------------------------- iterators */

/* The yielded Values point into the iterator, at its counter or at the element
 * it just received, so each one is good until yield returns. Go's are copies;
 * reflect_value_interface or a copy of the bytes keeps one for longer. */
typedef struct RfSeqEnv {
    ReflectValue v;
    /* How many there are, for a number or an array, worked out when the
     * iterator is made, as Go's does. */
    uint64_t n;
    /* Room for one element received from a channel. */
    void *buf;
} RfSeqEnv;

/* The Int i as a Value, which is what Go's ValueOf(i) is for a range index. */
#define RF_INT_VALUE(ip) rf_make(TYPE_INT, (ip), 0)

static void rf_seq_num(void *env, IterYield yield) {
    const RfSeqEnv *e = env;
    ReflectValue v = e->v;
    _Alignas(8) Byte tmp[8];
    for (uint64_t i = 0; i < e->n; i++) {
        rf_store_uint(tmp, v.typ->size, i);
        ReflectValue x = rf_make(v.typ, tmp, 0);
        if (!yield.f(yield.env, &x))
            return;
    }
}

static void rf_seq_index(void *env, IterYield yield) {
    const RfSeqEnv *e = env;
    Int n = rf_kind(e->v) == KIND_SLICE ? reflect_value_len(e->v) : (Int)e->n;
    for (Int i = 0; i < n; i++) {
        ReflectValue x = RF_INT_VALUE(&i);
        if (!yield.f(yield.env, &x))
            return;
    }
}

static void rf_seq_string(void *env, IterYield yield) {
    const RfSeqEnv *e = env;
    Str s = *(const Str *)e->v.ptr;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        (void)utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        Int at = i;
        ReflectValue x = RF_INT_VALUE(&at);
        if (!yield.f(yield.env, &x))
            return;
        i += size > 0 ? size : 1;
    }
}

static void rf_seq_map(void *env, IterYield yield) {
    const RfSeqEnv *e = env;
    ReflectMapIter it;
    reflect_map_iter_reset(&it, e->v);
    while (reflect_map_iter_next(&it)) {
        ReflectValue k = reflect_map_iter_key(&it);
        if (!yield.f(yield.env, &k))
            return;
    }
}

static void rf_seq_chan(void *env, IterYield yield) {
    const RfSeqEnv *e = env;
    if ((reflect_type_chan_dir(e->v.typ) & REFLECT_RECV_DIR) == 0)
        panic_str(BURROW_S("reflect: recv on send-only channel"));
    Chan *c = rf_word(e->v.ptr);
    while (chan_recv(c, e->buf)) {
        ReflectValue x = rf_make(e->v.typ->elem, e->buf, 0);
        if (!yield.f(yield.env, &x))
            return;
    }
}

static RfSeqEnv *rf_seq_env(ReflectValue v, Alloc *a, size_t extra) {
    RfSeqEnv *e = mem_alloc(a, sizeof(RfSeqEnv) + extra, 16);
    if (e == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    e->v = v;
    e->n = 0;
    e->buf = extra > 0 ? (Byte *)e + sizeof(RfSeqEnv) : NULL;
    return e;
}

IterSeq reflect_value_seq(ReflectValue v, Alloc *a) {
    void (*f)(void *, IterYield) = NULL;
    size_t extra = 0;
    uint64_t n = 0;
    switch ((int)rf_kind(v)) {
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64: {
        int64_t x = reflect_value_int(v);
        n = x < 0 ? 0 : (uint64_t)x;
        f = rf_seq_num;
        break;
    }
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        n = reflect_value_uint(v);
        f = rf_seq_num;
        break;
    case KIND_POINTER:
        /* Go goes by the type here, so a nil *[N]T still counts to N. */
        if (v.typ->elem != NULL && v.typ->elem->kind == KIND_ARRAY) {
            n = v.typ->elem->len;
            f = rf_seq_index;
        }
        break;
    case KIND_ARRAY:
        n = v.typ->len;
        f = rf_seq_index;
        break;
    case KIND_SLICE:
        f = rf_seq_index;
        break;
    case KIND_STRING:
        f = rf_seq_string;
        break;
    case KIND_MAP:
        f = rf_seq_map;
        break;
    case KIND_CHAN:
        f = rf_seq_chan;
        extra = v.typ->elem->size > 0 ? v.typ->elem->size : 1;
        break;
    default:
        break;
    }
    if (f == NULL)
        burrow__reflect_panic(BURROW_S("reflect: "),
                              reflect_type_string(reflect_value_type(v)),
                              BURROW_S(" cannot produce iter.Seq[Value]"));
    RfSeqEnv *e = rf_seq_env(v, a, extra);
    e->n = n;
    return (IterSeq){f, e};
}

static void rf_seq2_index(void *env, IterYield2 yield) {
    const RfSeqEnv *e = env;
    ReflectValue v = e->v;
    if (rf_kind(v) == KIND_POINTER)
        v = reflect_value_elem(v);
    Int n = reflect_value_len(v);
    for (Int i = 0; i < n; i++) {
        ReflectValue k = RF_INT_VALUE(&i);
        ReflectValue x = reflect_value_index(v, i);
        if (!yield.f(yield.env, &k, &x))
            return;
    }
}

static void rf_seq2_string(void *env, IterYield2 yield) {
    const RfSeqEnv *e = env;
    Str s = *(const Str *)e->v.ptr;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        int32_t r =
            utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        Int at = i;
        ReflectValue k = RF_INT_VALUE(&at);
        ReflectValue x = rf_make(TYPE_INT32, &r, 0);
        if (!yield.f(yield.env, &k, &x))
            return;
        i += size > 0 ? size : 1;
    }
}

static void rf_seq2_map(void *env, IterYield2 yield) {
    const RfSeqEnv *e = env;
    ReflectMapIter it;
    reflect_map_iter_reset(&it, e->v);
    while (reflect_map_iter_next(&it)) {
        ReflectValue k = reflect_map_iter_key(&it);
        ReflectValue x = reflect_map_iter_value(&it);
        if (!yield.f(yield.env, &k, &x))
            return;
    }
}

IterSeq2 reflect_value_seq2(ReflectValue v, Alloc *a) {
    void (*f)(void *, IterYield2) = NULL;
    switch ((int)rf_kind(v)) {
    case KIND_POINTER:
        /* Unlike Seq, Go looks at the element itself here, so a nil *[N]T
         * cannot produce one. */
        if (rf_kind(reflect_value_elem(v)) == KIND_ARRAY)
            f = rf_seq2_index;
        break;
    case KIND_ARRAY:
    case KIND_SLICE:
        f = rf_seq2_index;
        break;
    case KIND_STRING:
        f = rf_seq2_string;
        break;
    case KIND_MAP:
        f = rf_seq2_map;
        break;
    default:
        break;
    }
    if (f == NULL)
        burrow__reflect_panic(BURROW_S("reflect: "),
                              reflect_type_string(reflect_value_type(v)),
                              BURROW_S(" cannot produce iter.Seq2[Value, Value]"));
    return (IterSeq2){f, rf_seq_env(v, a, 0)};
}

/* ---------------------------------------------------------------- equality */

bool reflect_value_comparable(ReflectValue v) {
    const Type *t = v.typ;
    if (t == NULL)
        return false;
    switch ((int)t->kind) {
    case KIND_ARRAY: {
        Kind ek = t->elem->kind;
        if (ek == KIND_INTERFACE || ek == KIND_ARRAY || ek == KIND_STRUCT) {
            for (Int i = 0; i < (Int)t->len; i++)
                if (!reflect_value_comparable(reflect_value_index(v, i)))
                    return false;
            return true;
        }
        return reflect_type_comparable(t);
    }
    case KIND_INTERFACE:
        return reflect_value_is_nil(v) ||
               reflect_value_comparable(reflect_value_elem(v));
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++)
            if (!reflect_value_comparable(reflect_value_field(v, (Int)i)))
                return false;
        return true;
    default:
        return reflect_type_comparable(t);
    }
}

bool reflect_value_equal(ReflectValue v, ReflectValue u) {
    if (rf_kind(v) == KIND_INTERFACE)
        v = reflect_value_elem(v);
    if (rf_kind(u) == KIND_INTERFACE)
        u = reflect_value_elem(u);
    if (v.typ == NULL || u.typ == NULL)
        return (v.typ == NULL) == (u.typ == NULL);
    if (v.typ->kind != u.typ->kind || !burrow__reflect_identical(v.typ, u.typ))
        return false;
    switch ((int)v.typ->kind) {
    case KIND_BOOL:
        return reflect_value_bool(v) == reflect_value_bool(u);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return reflect_value_int(v) == reflect_value_int(u);
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return reflect_value_uint(v) == reflect_value_uint(u);
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return reflect_value_float(v) == reflect_value_float(u);
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        Complex128 a = reflect_value_complex(v);
        Complex128 b = reflect_value_complex(u);
        return a.re == b.re && a.im == b.im;
    }
    case KIND_STRING:
        return str_eq(reflect_value_string(v), reflect_value_string(u));
    case KIND_CHAN:
    case KIND_POINTER:
    case KIND_UNSAFE_POINTER:
        return reflect_value_pointer(v) == reflect_value_pointer(u);
    case KIND_ARRAY: {
        Int n = (Int)v.typ->len;
        if (n == 0) {
            if (!reflect_type_comparable(v.typ->elem))
                break;
            return true;
        }
        for (Int i = 0; i < n; i++)
            if (!reflect_value_equal(reflect_value_index(v, i),
                                     reflect_value_index(u, i)))
                return false;
        return true;
    }
    case KIND_STRUCT:
        for (uint16_t i = 0; i < v.typ->nfield; i++)
            if (!reflect_value_equal(reflect_value_field(v, (Int)i),
                                     reflect_value_field(u, (Int)i)))
                return false;
        return true;
    default:
        break;
    }
    burrow__reflect_panic(BURROW_S("reflect.Value.Equal: values of type "),
                          reflect_type_string(v.typ), BURROW_S(" are not comparable"));
}

/* ------------------------------------------------------------------ swapper */

static void rf_swap(void *env, Int i, Int j) {
    Slice *s = env;
    if (i < 0 || j < 0 || i >= s->len || j >= s->len)
        panic_str(BURROW_S("reflect: slice index out of range"));
    if (i == j)
        return;
    size_t size = s->elem->size;
    Byte *a = (Byte *)s->p + (size_t)i * size;
    Byte *b = (Byte *)s->p + (size_t)j * size;
    Byte tmp[64];
    while (size > 0) {
        size_t n = size < sizeof tmp ? size : sizeof tmp;
        memcpy(tmp, a, n);
        memcpy(a, b, n);
        memcpy(b, tmp, n);
        a += n;
        b += n;
        size -= n;
    }
}

SwapFunc reflect_swapper(Any s) {
    ReflectValue v = reflect_value_of(s);
    if (rf_kind(v) != KIND_SLICE)
        burrow__reflect_panic_kind(BURROW_S("Swapper"), rf_kind(v));
    return (SwapFunc){rf_swap, v.ptr};
}
