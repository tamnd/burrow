/* Derived from Go's src/database/sql/driver/types.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/database/sql/driver.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/uuid.h"

#include "driver_internal.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

#define DR_IFACE_TYPE(cname, gonm)                                                     \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"database/sql/driver", 19},                                     \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

DR_IFACE_TYPE(DriverValuer, "Valuer");

/* Only ever named in a signature, so unsafe pointers as far as reflection can
 * tell, with names that say what they point at. */
#define DR_ARG_TYPE(cname, gonm)                                                       \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {NULL, 0},                                                                     \
        KIND_UNSAFE_POINTER,                                                           \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

DR_ARG_TYPE(DriverAllocArg, "*Alloc");
DR_ARG_TYPE(DriverErrorArg, "*error");

static const Field dr_decomposed_fields[] = {
    {{(const Byte *)"Form", 4},
     {NULL, 0},
     TYPE_OF(Byte),
     (uint32_t)offsetof(DriverDecomposed, form)},
    {{(const Byte *)"Negative", 8},
     {NULL, 0},
     TYPE_OF(bool),
     (uint32_t)offsetof(DriverDecomposed, negative)},
    {{(const Byte *)"Coefficient", 11},
     {NULL, 0},
     TYPE_OF(Bytes),
     (uint32_t)offsetof(DriverDecomposed, coefficient)},
    {{(const Byte *)"Exponent", 8},
     {NULL, 0},
     TYPE_OF(int32_t),
     (uint32_t)offsetof(DriverDecomposed, exponent)},
};

const Type burrow_type_DriverDecomposed = {
    {(const Byte *)"Decomposed", 10},
    {(const Byte *)"database/sql/driver", 19},
    KIND_STRUCT,
    (uint32_t)sizeof(DriverDecomposed),
    (uint16_t)_Alignof(DriverDecomposed),
    4,
    0,
    dr_decomposed_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x64726463U, /* "drdc" */
    NULL,
};

/* A zero size struct, which is what Go's converter types are. */
#define DR_EMPTY_TYPE(var, gonm, h)                                                    \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"database/sql/driver", 19},                                     \
        KIND_STRUCT,                                                                   \
        0,                                                                             \
        1,                                                                             \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (h),                                                                           \
        NULL,                                                                          \
    }

DR_EMPTY_TYPE(dr_bool_desc, "boolType", 0x64726274U);
DR_EMPTY_TYPE(dr_int32_desc, "int32Type", 0x64726933U);
DR_EMPTY_TYPE(dr_string_desc, "stringType", 0x64727374U);
DR_EMPTY_TYPE(dr_default_desc, "defaultConverter", 0x64726466U);

static const Type dr_null_desc = {
    {(const Byte *)"Null", 4},
    {(const Byte *)"database/sql/driver", 19},
    KIND_STRUCT,
    (uint32_t)sizeof(DriverNull),
    (uint16_t)_Alignof(DriverNull),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64726e6cU, /* "drnl" */
    NULL,
};

static const Type dr_not_null_desc = {
    {(const Byte *)"NotNull", 7},
    {(const Byte *)"database/sql/driver", 19},
    KIND_STRUCT,
    (uint32_t)sizeof(DriverNotNull),
    (uint16_t)_Alignof(DriverNotNull),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64726e6eU, /* "drnn" */
    NULL,
};

/* ----------------------------------------------------------- finding methods */

Any burrow__driver_unwrap(Any v) {
    while (v.t == TYPE_ANY && v.data != NULL)
        v = *(const Any *)v.data;
    return v;
}

bool burrow__driver_find(Any v, Str name, const Type *iface,
                         bool (*shape)(const Method *m), burrow__DriverFound *out) {
    out->vt = NULL;
    out->m = NULL;
    out->recv = NULL;
    out->nil_elem = false;
    v = burrow__driver_unwrap(v);
    const Type *t = v.t;
    void *d = v.data;
    if (t == NULL || d == NULL)
        return false;
    if (t->kind == KIND_INTERFACE) {
        /* Every interface value is a vtable and a data pointer, and every
         * vtable starts with self_type. */
        const Iface *in = (const Iface *)d;
        if (in->vt == NULL)
            return false;
        if (iface != NULL && t == iface) {
            out->vt = in->vt;
            out->recv = in->data;
            return true;
        }
        t = in->vt->self_type;
        d = in->data;
        if (t == NULL)
            return false;
        const Method *m = type_method_by_name(t, name);
        if (!shape(m))
            return false;
        out->m = m;
        out->recv = d;
        return true;
    }
    const Method *m = type_method_by_name(t, name);
    if (shape(m)) {
        out->m = m;
        out->recv = d;
        return true;
    }
    if (t->kind == KIND_POINTER && t->elem != NULL) {
        m = type_method_by_name(t->elem, name);
        if (shape(m)) {
            out->m = m;
            out->recv = *(void **)d;
            out->nil_elem = out->recv == NULL;
            return true;
        }
    }
    return false;
}

static bool dr_valuer_shape(const Method *m) {
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    const Type *f = m->ftype;
    return type_num_in(f) == 2 && type_num_out(f) == 1 &&
           type_in(f, 0) == &burrow_type_DriverAllocArg &&
           type_in(f, 1) == &burrow_type_DriverErrorArg && type_out(f, 0) == TYPE_ANY;
}

static bool dr_decompose_shape(const Method *m) {
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    const Type *f = m->ftype;
    return type_num_in(f) == 1 && type_num_out(f) == 1 && type_in(f, 0) == TYPE_BYTES &&
           type_out(f, 0) == &burrow_type_DriverDecomposed;
}

bool burrow__driver_is_valuer(Any v) {
    burrow__DriverFound f;
    return burrow__driver_find(v, BURROW_S("Value"), &burrow_type_DriverValuer,
                               dr_valuer_shape, &f);
}

DriverValue burrow__driver_call_valuer(Alloc *a, Any v, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    burrow__DriverFound f;
    if (!burrow__driver_find(v, BURROW_S("Value"), &burrow_type_DriverValuer,
                             dr_valuer_shape, &f))
        return (DriverValue){NULL, NULL};
    /* callValuerValue: a nil pointer to a type with a value receiver Value
     * is NULL, rather than a call that would dereference it. */
    if (f.nil_elem)
        return (DriverValue){NULL, NULL};
    Error e = BURROW_NO_ERROR;
    DriverValue out = {NULL, NULL};
    if (f.vt != NULL) {
        out = ((const DriverValuerVT *)f.vt)->value(f.recv, a, &e);
    } else {
        DriverAllocArg aa = a;
        DriverErrorArg ea = &e;
        void *args[2] = {(void *)&aa, (void *)&ea};
        void *rets[1] = {&out};
        method_call(f.m, f.recv, args, rets);
    }
    BURROW_OUT(err, e);
    return out;
}

bool burrow__driver_is_decimal(Any v) {
    burrow__DriverFound f;
    return burrow__driver_find(v, BURROW_S("Decompose"), NULL, dr_decompose_shape, &f);
}

DriverDecomposed burrow__driver_decompose(Any v, Bytes buf) {
    DriverDecomposed out;
    memset(&out, 0, sizeof out);
    burrow__DriverFound f;
    if (!burrow__driver_find(v, BURROW_S("Decompose"), NULL, dr_decompose_shape, &f))
        return out;
    void *args[1] = {(void *)&buf};
    void *rets[1] = {&out};
    method_call(f.m, f.recv, args, rets);
    return out;
}

/* ------------------------------------------------------------ values */

bool driver_is_value(Any v) {
    v = burrow__driver_unwrap(v);
    if (v.t == NULL)
        return true;
    if (v.t == TYPE_BYTES || v.t == TYPE_BOOL || v.t == TYPE_FLOAT64 ||
        v.t == TYPE_INT64 || v.t == TYPE_STRING || v.t == TYPE_TIME)
        return true;
    return burrow__driver_is_decimal(v);
}

bool driver_is_scan_value(Any v) {
    return driver_is_value(v);
}

/* A copy of the size bytes at src in a, as a value of type t. */
static DriverValue dr_box(Alloc *a, const Type *t, const void *src, size_t size,
                          Error *err) {
    void *p = mem_alloc(a, size, t->align);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (DriverValue){NULL, NULL};
    }
    memcpy(p, src, size);
    return (DriverValue){t, p};
}

static DriverValue dr_int64(Alloc *a, int64_t n, Error *err) {
    return dr_box(a, TYPE_INT64, &n, sizeof n, err);
}

static DriverValue dr_bool_value(Alloc *a, bool b, Error *err) {
    return dr_box(a, TYPE_BOOL, &b, sizeof b, err);
}

/* reflect.Value.Int and Uint, for a type of the kind that has them. */
static int64_t dr_int_of(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const int8_t *)p;
    case 2:
        return *(const int16_t *)p;
    case 4:
        return *(const int32_t *)p;
    default:
        return *(const int64_t *)p;
    }
}

static uint64_t dr_uint_of(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const uint8_t *)p;
    case 2:
        return *(const uint16_t *)p;
    case 4:
        return *(const uint32_t *)p;
    default:
        return *(const uint64_t *)p;
    }
}

static bool dr_is_int_kind(Kind k) {
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64;
}

/* Go's lists of unsigned kinds leave out uintptr. */
static bool dr_is_uint_kind(Kind k) {
    return k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 || k == KIND_UINT32 ||
           k == KIND_UINT64;
}

/* ------------------------------------------------------------------ Bool */

static DriverValue dr_bool_convert(void *self, Alloc *a, Any src, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    src = burrow__driver_unwrap(src);
    if (src.t == TYPE_BOOL)
        return src;
    if (src.t == TYPE_STRING || src.t == TYPE_BYTES) {
        Str s;
        if (src.t == TYPE_STRING) {
            s = *(const Str *)src.data;
        } else {
            const Slice *b = (const Slice *)src.data;
            s = str_from_bytes((const Byte *)b->p, b->len);
        }
        Error perr = BURROW_NO_ERROR;
        bool b = strconv_parse_bool(s, &perr);
        if (BURROW_FAILED(perr)) {
            BURROW_OUT(err, fmt_errorf_v(
                                "sql/driver: couldn't convert %q into type bool", src));
            return (DriverValue){NULL, NULL};
        }
        return dr_bool_value(a, b, err);
    }
    if (src.t != NULL && src.data != NULL) {
        if (dr_is_int_kind(src.t->kind)) {
            int64_t iv = dr_int_of(src.t, src.data);
            if (iv == 1 || iv == 0)
                return dr_bool_value(a, iv == 1, err);
            BURROW_OUT(err, fmt_errorf_v(
                                "sql/driver: couldn't convert %d into type bool", iv));
            return (DriverValue){NULL, NULL};
        }
        if (dr_is_uint_kind(src.t->kind)) {
            uint64_t uv = dr_uint_of(src.t, src.data);
            if (uv == 1 || uv == 0)
                return dr_bool_value(a, uv == 1, err);
            BURROW_OUT(err, fmt_errorf_v(
                                "sql/driver: couldn't convert %d into type bool", uv));
            return (DriverValue){NULL, NULL};
        }
    }
    BURROW_OUT(err, fmt_errorf_v("sql/driver: couldn't convert %v (%T) into type bool",
                                 src, src));
    return (DriverValue){NULL, NULL};
}

static const DriverValueConverterVT dr_bool_vt = {&dr_bool_desc, dr_bool_convert};
const DriverValueConverter driver_bool = {&dr_bool_vt, NULL};

/* ----------------------------------------------------------------- Int32 */

static DriverValue dr_int32_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    v = burrow__driver_unwrap(v);
    if (v.t != NULL && v.data != NULL) {
        Kind k = v.t->kind;
        if (dr_is_int_kind(k)) {
            int64_t i64 = dr_int_of(v.t, v.data);
            if (i64 > INT32_MAX || i64 < INT32_MIN) {
                BURROW_OUT(err,
                           fmt_errorf_v("sql/driver: value %d overflows int32", v));
                return (DriverValue){NULL, NULL};
            }
            return dr_int64(a, i64, err);
        }
        if (dr_is_uint_kind(k)) {
            uint64_t u64 = dr_uint_of(v.t, v.data);
            if (u64 > INT32_MAX) {
                BURROW_OUT(err,
                           fmt_errorf_v("sql/driver: value %d overflows int32", v));
                return (DriverValue){NULL, NULL};
            }
            return dr_int64(a, (int64_t)u64, err);
        }
        if (k == KIND_STRING) {
            Error aerr = BURROW_NO_ERROR;
            Int i = strconv_atoi(*(const Str *)v.data, &aerr);
            if (BURROW_FAILED(aerr)) {
                BURROW_OUT(err,
                           fmt_errorf_v(
                               "sql/driver: value %q can't be converted to int32", v));
                return (DriverValue){NULL, NULL};
            }
            return dr_int64(a, (int64_t)i, err);
        }
    }
    BURROW_OUT(
        err,
        fmt_errorf_v("sql/driver: unsupported value %v (type %T) converting to int32",
                     v, v));
    return (DriverValue){NULL, NULL};
}

static const DriverValueConverterVT dr_int32_vt = {&dr_int32_desc, dr_int32_convert};
const DriverValueConverter driver_int32 = {&dr_int32_vt, NULL};

/* ---------------------------------------------------------------- String */

static DriverValue dr_string_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    v = burrow__driver_unwrap(v);
    if (v.t == TYPE_STRING || v.t == TYPE_BYTES)
        return v;
    Str s = fmt_sprintf_v(a, "%v", v);
    return dr_box(a, TYPE_STRING, &s, sizeof s, err);
}

static const DriverValueConverterVT dr_string_vt = {&dr_string_desc, dr_string_convert};
const DriverValueConverter driver_string = {&dr_string_vt, NULL};

/* ------------------------------------------------------------ Null, NotNull */

DriverValue driver_null_convert_value(DriverNull n, Alloc *a, Any v, Error *err) {
    v = burrow__driver_unwrap(v);
    if (v.t == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (DriverValue){NULL, NULL};
    }
    return n.converter.vt->convert_value(n.converter.data, a, v, err);
}

static DriverValue dr_null_convert(void *self, Alloc *a, Any v, Error *err) {
    return driver_null_convert_value(*(const DriverNull *)self, a, v, err);
}

static const DriverValueConverterVT dr_null_vt = {&dr_null_desc, dr_null_convert};

DriverValueConverter driver_null_as_value_converter(DriverNull *n) {
    return (DriverValueConverter){&dr_null_vt, n};
}

DriverValue driver_not_null_convert_value(DriverNotNull n, Alloc *a, Any v,
                                          Error *err) {
    v = burrow__driver_unwrap(v);
    if (v.t == NULL) {
        BURROW_OUT(err, fmt_errorf_v("nil value not allowed"));
        return (DriverValue){NULL, NULL};
    }
    return n.converter.vt->convert_value(n.converter.data, a, v, err);
}

static DriverValue dr_not_null_convert(void *self, Alloc *a, Any v, Error *err) {
    return driver_not_null_convert_value(*(const DriverNotNull *)self, a, v, err);
}

static const DriverValueConverterVT dr_not_null_vt = {&dr_not_null_desc,
                                                      dr_not_null_convert};

DriverValueConverter driver_not_null_as_value_converter(DriverNotNull *n) {
    return (DriverValueConverter){&dr_not_null_vt, n};
}

/* ------------------------------------------------- DefaultParameterConverter */

static DriverValue dr_default_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    v = burrow__driver_unwrap(v);
    if (driver_is_value(v))
        return v;
    if (burrow__driver_is_valuer(v)) {
        Error verr = BURROW_NO_ERROR;
        DriverValue sv = burrow__driver_call_valuer(a, v, &verr);
        if (BURROW_FAILED(verr)) {
            BURROW_OUT(err, verr);
            return (DriverValue){NULL, NULL};
        }
        if (!driver_is_value(sv)) {
            BURROW_OUT(err, fmt_errorf_v("non-Value type %T returned from Value", sv));
            return (DriverValue){NULL, NULL};
        }
        return sv;
    }
    if (v.t == TYPE_UUID) {
        Str s = uuid_string(*(const Uuid *)v.data, a);
        return dr_box(a, TYPE_STRING, &s, sizeof s, err);
    }
    const Type *t = v.t;
    Kind k = t->kind;
    if (v.data == NULL) {
        BURROW_OUT(err, fmt_errorf_v("unsupported type %T, a %s", v, kind_name(k)));
        return (DriverValue){NULL, NULL};
    }
    if (k == KIND_POINTER) {
        void *p = *(void **)v.data;
        if (p == NULL)
            return (DriverValue){NULL, NULL};
        if (t->elem != NULL)
            return dr_default_convert(self, a, (Any){t->elem, p}, err);
    } else if (dr_is_int_kind(k)) {
        return dr_int64(a, dr_int_of(t, v.data), err);
    } else if (k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 ||
               k == KIND_UINT32) {
        return dr_int64(a, (int64_t)dr_uint_of(t, v.data), err);
    } else if (k == KIND_UINT64) {
        uint64_t u64 = dr_uint_of(t, v.data);
        if (u64 >= (uint64_t)1 << 63) {
            BURROW_OUT(
                err, fmt_errorf_v("uint64 values with high bit set are not supported"));
            return (DriverValue){NULL, NULL};
        }
        return dr_int64(a, (int64_t)u64, err);
    } else if (k == KIND_FLOAT32 || k == KIND_FLOAT64) {
        double f = t->size == sizeof(float) ? (double)*(const float *)v.data
                                            : *(const double *)v.data;
        return dr_box(a, TYPE_FLOAT64, &f, sizeof f, err);
    } else if (k == KIND_BOOL) {
        return dr_bool_value(a, *(const bool *)v.data, err);
    } else if (k == KIND_SLICE) {
        Kind ek = t->elem != NULL ? t->elem->kind : KIND_INVALID;
        if (ek == KIND_UINT8)
            return dr_box(a, TYPE_BYTES, v.data, sizeof(Slice), err);
        BURROW_OUT(
            err, fmt_errorf_v("unsupported type %T, a slice of %s", v, kind_name(ek)));
        return (DriverValue){NULL, NULL};
    } else if (k == KIND_STRING) {
        return dr_box(a, TYPE_STRING, v.data, sizeof(Str), err);
    }
    BURROW_OUT(err, fmt_errorf_v("unsupported type %T, a %s", v, kind_name(k)));
    return (DriverValue){NULL, NULL};
}

static const DriverValueConverterVT dr_default_vt = {&dr_default_desc,
                                                     dr_default_convert};
const DriverValueConverter driver_default_parameter_converter = {&dr_default_vt, NULL};
