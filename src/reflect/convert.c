/* package reflect: Convert, CanConvert, ConvertibleTo and AssignableTo. Go's
 * convertOp and the cvt functions from reflect/value.go, and the type tests
 * from reflect/type.go.
 *
 * A conversion that makes a new value puts it in the Alloc it is given. One
 * between types with the same underlying type only relabels the bytes, so it
 * shares v's when v cannot be written and copies them when it can, as Go's
 * cvtDirect does.
 *
 * Conversion to an interface works for the empty interface. An interface with
 * methods is a vtable here, and reflect has no way yet to find the vtable a
 * type has for an interface, so the only value that converts to one is a value
 * of that same interface type.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "burrow/panic.h"
#include "burrow/utf8.h"

/* What convertOp picks. */
typedef enum RfcOp {
    RFC_NONE,
    RFC_INT,
    RFC_UINT,
    RFC_FLOAT_INT,
    RFC_FLOAT_UINT,
    RFC_INT_FLOAT,
    RFC_UINT_FLOAT,
    RFC_FLOAT,
    RFC_COMPLEX,
    RFC_INT_STRING,
    RFC_UINT_STRING,
    RFC_BYTES_STRING,
    RFC_STRING_BYTES,
    RFC_RUNES_STRING,
    RFC_STRING_RUNES,
    RFC_SLICE_ARRAY_PTR,
    RFC_SLICE_ARRAY,
    RFC_DIRECT,
    RFC_T2E,
    RFC_I2E,
} RfcOp;

static bool rfc_is_int(Kind k) {
    return k >= KIND_INT && k <= KIND_INT64;
}

static bool rfc_is_uint(Kind k) {
    return k >= KIND_UINT && k <= KIND_UINTPTR;
}

static bool rfc_is_float(Kind k) {
    return k == KIND_FLOAT32 || k == KIND_FLOAT64;
}

static bool rfc_is_complex(Kind k) {
    return k == KIND_COMPLEX64 || k == KIND_COMPLEX128;
}

/* Go's specialChannelAssignability: a chan T goes into a <-chan T or a chan<- T
 * when one of the two types is unnamed. */
static bool rfc_chan_assignable(const Type *dst, const Type *src) {
    return reflect_type_chan_dir(src) == REFLECT_BOTH_DIR &&
           (dst->name.len == 0 || src->name.len == 0) &&
           burrow__reflect_identical(dst->elem, src->elem);
}

/* Go's convertOp: how a value of type src becomes a dst, or RFC_NONE. */
static RfcOp rfc_op(const Type *dst, const Type *src) {
    Kind dk = dst->kind;
    Kind sk = src->kind;
    if (rfc_is_int(sk) || rfc_is_uint(sk)) {
        bool sint = rfc_is_int(sk);
        if (rfc_is_int(dk) || rfc_is_uint(dk))
            return sint ? RFC_INT : RFC_UINT;
        if (rfc_is_float(dk))
            return sint ? RFC_INT_FLOAT : RFC_UINT_FLOAT;
        if (dk == KIND_STRING)
            return sint ? RFC_INT_STRING : RFC_UINT_STRING;
    } else if (rfc_is_float(sk)) {
        if (rfc_is_int(dk))
            return RFC_FLOAT_INT;
        if (rfc_is_uint(dk))
            return RFC_FLOAT_UINT;
        if (rfc_is_float(dk))
            return RFC_FLOAT;
    } else if (rfc_is_complex(sk)) {
        if (rfc_is_complex(dk))
            return RFC_COMPLEX;
    } else if (sk == KIND_STRING) {
        if (dk == KIND_SLICE && dst->elem->kind == KIND_UINT8)
            return RFC_STRING_BYTES;
        if (dk == KIND_SLICE && dst->elem->kind == KIND_INT32)
            return RFC_STRING_RUNES;
    } else if (sk == KIND_SLICE) {
        if (dk == KIND_STRING && src->elem->kind == KIND_UINT8)
            return RFC_BYTES_STRING;
        if (dk == KIND_STRING && src->elem->kind == KIND_INT32)
            return RFC_RUNES_STRING;
        /* "x is a slice, T is a pointer-to-array type, and the slice and
         * array types have identical element types." */
        if (dk == KIND_POINTER && dst->elem->kind == KIND_ARRAY &&
            burrow__reflect_identical(src->elem, dst->elem->elem))
            return RFC_SLICE_ARRAY_PTR;
        /* "x is a slice, T is an array type, and the slice and array types
         * have identical element types." */
        if (dk == KIND_ARRAY && burrow__reflect_identical(src->elem, dst->elem))
            return RFC_SLICE_ARRAY;
    } else if (sk == KIND_CHAN) {
        if (dk == KIND_CHAN && rfc_chan_assignable(dst, src))
            return RFC_DIRECT;
    }

    /* dst and src have the same underlying type. */
    if (burrow__reflect_same_underlying(dst, src))
        return RFC_DIRECT;

    /* dst and src are unnamed pointer types with the same underlying base
     * type. */
    if (dk == KIND_POINTER && dst->name.len == 0 && sk == KIND_POINTER &&
        src->name.len == 0 && burrow__reflect_same_underlying(dst->elem, src->elem))
        return RFC_DIRECT;

    /* Everything implements the empty interface. */
    if (dst == TYPE_ANY)
        return sk == KIND_INTERFACE ? RFC_I2E : RFC_T2E;
    return RFC_NONE;
}

/* --------------------------------------------------------------- the types */

BURROW_NORETURN static void rfc_nil_type(const char *method) {
    burrow__reflect_panic(BURROW_S("reflect: nil type passed to Type."),
                          str_from_cstr(method), BURROW_STR_EMPTY);
}

bool reflect_type_assignable_to(ReflectType t, ReflectType u) {
    if (u == NULL)
        rfc_nil_type("AssignableTo");
    return burrow__reflect_assignable(u, t) || u == TYPE_ANY;
}

bool reflect_type_convertible_to(ReflectType t, ReflectType u) {
    if (u == NULL)
        rfc_nil_type("ConvertibleTo");
    return rfc_op(u, t) != RFC_NONE;
}

/* -------------------------------------------------------------- the values */

/* size bytes from a, zeroed, never NULL. */
static void *rfc_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    return p;
}

static void *rfc_new(Alloc *a, const Type *t) {
    return rfc_alloc(a, t->size, t->align);
}

static ReflectValue rfc_make(const Type *t, const void *p, uint32_t flag) {
    return (ReflectValue){t, (void *)(uintptr_t)p, flag};
}

/* Go's makeInt: bits, cut down to the size of t. */
static ReflectValue rfc_make_int(Alloc *a, uint32_t flag, uint64_t bits,
                                 const Type *t) {
    void *p = rfc_new(a, t);
    switch (t->size) {
    case 1: {
        uint8_t x = (uint8_t)bits;
        memcpy(p, &x, 1);
        break;
    }
    case 2: {
        uint16_t x = (uint16_t)bits;
        memcpy(p, &x, 2);
        break;
    }
    case 4: {
        uint32_t x = (uint32_t)bits;
        memcpy(p, &x, 4);
        break;
    }
    default:
        memcpy(p, &bits, 8);
        break;
    }
    return rfc_make(t, p, flag);
}

/* Go's makeFloat, which rounds to float32 for a 4 byte t. */
static ReflectValue rfc_make_float(Alloc *a, uint32_t flag, double x, const Type *t) {
    void *p = rfc_new(a, t);
    if (t->size == 4) {
        float f = (float)x;
        memcpy(p, &f, 4);
    } else {
        memcpy(p, &x, 8);
    }
    return rfc_make(t, p, flag);
}

static ReflectValue rfc_make_complex(Alloc *a, uint32_t flag, Complex128 x,
                                     const Type *t) {
    void *p = rfc_new(a, t);
    if (t->size == sizeof(Complex64)) {
        Complex64 c = {(float)x.re, (float)x.im};
        memcpy(p, &c, sizeof c);
    } else {
        memcpy(p, &x, sizeof x);
    }
    return rfc_make(t, p, flag);
}

static ReflectValue rfc_make_string(Alloc *a, uint32_t flag, Str s, const Type *t) {
    void *p = rfc_new(a, t);
    memcpy(p, &s, sizeof s);
    return rfc_make(t, p, flag);
}

static ReflectValue rfc_make_slice(Alloc *a, uint32_t flag, Slice s, const Type *t) {
    void *p = rfc_new(a, t);
    memcpy(p, &s, sizeof s);
    return rfc_make(t, p, flag);
}

/* float64 to int64 as Go does it for a value in range. C leaves the rest
 * undefined, so those come out as the smallest int64, which is what an x86
 * conversion gives. */
static int64_t rfc_float_int(double x) {
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0)
        return (int64_t)x;
    return INT64_MIN;
}

static uint64_t rfc_float_uint(double x) {
    if (x >= 9223372036854775808.0 && x < 18446744073709551616.0)
        return (uint64_t)(x - 9223372036854775808.0) | (UINT64_C(1) << 63);
    return (uint64_t)rfc_float_int(x);
}

/* A copy of the n bytes at p in a, which is never NULL, so that an empty
 * result is an empty slice rather than a nil one, as in Go. */
static void *rfc_copy_bytes(Alloc *a, const void *p, size_t n, size_t align) {
    void *q = rfc_alloc(a, n, align);
    if (n > 0)
        memcpy(q, p, n);
    return q;
}

static Str rfc_rune_string(Alloc *a, int64_t r) {
    Byte buf[UTF8_UTF_MAX];
    Rune x = r >= 0 && r <= INT32_MAX ? (Rune)r : UTF8_RUNE_ERROR;
    Int n = utf8_encode_rune((Slice){buf, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_UINT8}, x);
    return str_from_bytes(rfc_copy_bytes(a, buf, (size_t)n, 1), n);
}

static Str rfc_runes_string(Alloc *a, Slice s) {
    const Rune *r = s.p;
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        Int k = utf8_rune_len(r[i]);
        n += k < 0 ? 3 : k;
    }
    Byte *p = rfc_alloc(a, (size_t)n, 1);
    Int at = 0;
    for (Int i = 0; i < s.len; i++)
        at += utf8_encode_rune((Slice){p + at, n - at, n - at, TYPE_UINT8}, r[i]);
    return str_from_bytes(p, n);
}

static Slice rfc_string_runes(Alloc *a, Str s, const Type *elem) {
    Int n = 0;
    for (Int i = 0; i < s.len; n++) {
        Int size;
        (void)utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        i += size;
    }
    Rune *r = rfc_alloc(a, (size_t)n * sizeof(Rune), _Alignof(Rune));
    Int k = 0;
    for (Int i = 0; i < s.len; k++) {
        Int size;
        r[k] = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        i += size;
    }
    return (Slice){r, n, n, elem};
}

static Str rfc_itoa(char *buf, size_t n, Int x) {
    size_t i = n;
    uint64_t u = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    do {
        buf[--i] = (char)('0' + u % 10U);
        u /= 10U;
    } while (u > 0U && i > 1);
    if (x < 0)
        buf[--i] = '-';
    return str_from_bytes(buf + i, (Int)(n - i));
}

/* Go's panic for a slice too short for the array it is converted to. */
BURROW_NORETURN static void rfc_too_short(Int have, Int want, bool pointer) {
    char b1[24];
    char b2[24];
    Str head =
        burrow__reflect_concat(BURROW_S("reflect: cannot convert slice with length "),
                               rfc_itoa(b1, sizeof b1, have),
                               pointer ? BURROW_S(" to pointer to array with length ")
                                       : BURROW_S(" to array with length "));
    burrow__reflect_panic(head, rfc_itoa(b2, sizeof b2, want), BURROW_STR_EMPTY);
}

static Slice rfc_slice(ReflectValue v) {
    Slice s;
    memcpy(&s, v.ptr, sizeof s);
    return s;
}

static Str rfc_str(ReflectValue v) {
    Str s;
    memcpy(&s, v.ptr, sizeof s);
    return s;
}

/* float32 to float32 copies the bits, so that a signalling NaN stays one, as
 * Go's cvtFloat takes care to. */
static ReflectValue rfc_float(Alloc *a, ReflectValue v, uint32_t ro, const Type *t) {
    if (v.typ->kind == KIND_FLOAT32 && t->kind == KIND_FLOAT32) {
        void *p = rfc_new(a, t);
        memcpy(p, v.ptr, 4);
        return rfc_make(t, p, ro);
    }
    return rfc_make_float(a, ro, reflect_value_float(v), t);
}

static ReflectValue rfc_direct(Alloc *a, ReflectValue v, const Type *t) {
    uint32_t f = v.flag;
    const void *p = v.ptr;
    if (f & BURROW__RF_ADDR) {
        /* Somewhere that can be written, so the result gets its own copy. */
        void *c = rfc_new(a, t);
        type_copy(t, c, p);
        p = c;
        f &= ~BURROW__RF_ADDR;
    }
    return rfc_make(t, p, f | BURROW__RF_RO_OF(v));
}

/* A concrete value into the empty interface, which holds a copy of it. */
static ReflectValue rfc_t2e(Alloc *a, ReflectValue v, uint32_t ro, const Type *t) {
    void *c = rfc_new(a, v.typ);
    type_copy(v.typ, c, v.ptr);
    Any *box = rfc_new(a, t);
    *box = BURROW_ANY(v.typ, c);
    return rfc_make(t, box, ro);
}

/* An interface value into the empty interface, which holds what it holds. */
static ReflectValue rfc_i2e(Alloc *a, ReflectValue v, uint32_t ro, const Type *t) {
    const Type *dt = v.typ;
    void *p = v.ptr;
    Any *box = rfc_new(a, t);
    if (!burrow__reflect_unwrap(&dt, &p))
        *box = (Any){NULL, NULL};
    else
        *box = BURROW_ANY(dt, p);
    return rfc_make(t, box, ro);
}

static ReflectValue rfc_convert(Alloc *a, ReflectValue v, RfcOp op, const Type *t) {
    uint32_t ro = BURROW__RF_RO_OF(v);
    switch ((int)op) {
    case RFC_INT:
        return rfc_make_int(a, ro, (uint64_t)reflect_value_int(v), t);
    case RFC_UINT:
        return rfc_make_int(a, ro, reflect_value_uint(v), t);
    case RFC_FLOAT_INT:
        return rfc_make_int(a, ro, (uint64_t)rfc_float_int(reflect_value_float(v)), t);
    case RFC_FLOAT_UINT:
        return rfc_make_int(a, ro, rfc_float_uint(reflect_value_float(v)), t);
    case RFC_INT_FLOAT:
        return rfc_make_float(a, ro, (double)reflect_value_int(v), t);
    case RFC_UINT_FLOAT:
        return rfc_make_float(a, ro, (double)reflect_value_uint(v), t);
    case RFC_FLOAT:
        return rfc_float(a, v, ro, t);
    case RFC_COMPLEX:
        return rfc_make_complex(a, ro, reflect_value_complex(v), t);
    case RFC_INT_STRING:
        return rfc_make_string(a, ro, rfc_rune_string(a, reflect_value_int(v)), t);
    case RFC_UINT_STRING: {
        uint64_t x = reflect_value_uint(v);
        int64_t r = x <= (uint64_t)INT32_MAX ? (int64_t)x : -1;
        return rfc_make_string(a, ro, rfc_rune_string(a, r), t);
    }
    case RFC_BYTES_STRING: {
        Slice s = rfc_slice(v);
        Str x = str_from_bytes(rfc_copy_bytes(a, s.p, (size_t)s.len, 1), s.len);
        return rfc_make_string(a, ro, x, t);
    }
    case RFC_STRING_BYTES: {
        Str s = rfc_str(v);
        void *p = rfc_copy_bytes(a, s.p, (size_t)s.len, 1);
        return rfc_make_slice(a, ro, (Slice){p, s.len, s.len, t->elem}, t);
    }
    case RFC_RUNES_STRING:
        return rfc_make_string(a, ro, rfc_runes_string(a, rfc_slice(v)), t);
    case RFC_STRING_RUNES:
        return rfc_make_slice(a, ro, rfc_string_runes(a, rfc_str(v), t->elem), t);
    case RFC_SLICE_ARRAY_PTR: {
        Slice s = rfc_slice(v);
        Int n = (Int)t->elem->len;
        if (n > s.len)
            rfc_too_short(s.len, n, true);
        void *w = rfc_alloc(a, sizeof(void *), _Alignof(void *));
        memcpy(w, &s.p, sizeof s.p);
        return rfc_make(t, w, v.flag & ~BURROW__RF_ADDR);
    }
    case RFC_SLICE_ARRAY: {
        Slice s = rfc_slice(v);
        Int n = (Int)t->len;
        if (n > s.len)
            rfc_too_short(s.len, n, false);
        void *c = rfc_new(a, t);
        if (n > 0)
            type_copy(t, c, s.p);
        return rfc_make(t, c, v.flag & ~BURROW__RF_ADDR);
    }
    case RFC_DIRECT:
        return rfc_direct(a, v, t);
    case RFC_T2E:
        return rfc_t2e(a, v, ro, t);
    case RFC_I2E:
        return rfc_i2e(a, v, ro, t);
    default:
        break;
    }
    burrow__reflect_panic(BURROW_S("reflect: internal error: bad conversion"),
                          BURROW_STR_EMPTY, BURROW_STR_EMPTY);
}

ReflectValue reflect_value_convert(ReflectValue v, Alloc *a, ReflectType t) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Convert"), KIND_INVALID);
    if (t == NULL)
        panic_str(BURROW_S("reflect.Value.Convert: nil type"));
    RfcOp op = rfc_op(t, v.typ);
    if (op == RFC_NONE) {
        Str head = burrow__reflect_concat(
            BURROW_S("reflect.Value.Convert: value of type "),
            reflect_type_string(v.typ), BURROW_S(" cannot be converted to type "));
        burrow__reflect_panic(head, reflect_type_string(t), BURROW_STR_EMPTY);
    }
    return rfc_convert(a, v, op, t);
}

bool reflect_value_can_convert(ReflectValue v, ReflectType t) {
    const Type *vt = reflect_value_type(v);
    if (!reflect_type_convertible_to(vt, t))
        return false;
    /* Converting from a slice to an array or a pointer to one can panic,
     * depending on the length. */
    if (vt->kind == KIND_SLICE && t->kind == KIND_ARRAY)
        return (Int)t->len <= reflect_value_len(v);
    if (vt->kind == KIND_SLICE && t->kind == KIND_POINTER &&
        t->elem->kind == KIND_ARRAY)
        return (Int)t->elem->len <= reflect_value_len(v);
    return true;
}
