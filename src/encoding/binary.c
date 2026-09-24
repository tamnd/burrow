/* Derived from Go's src/encoding/binary/binary.go, native_endian_little.go,
 * native_endian_big.go and varint.go.
 * Go source: go1.27.1.
 *
 * Go walks the value with reflect and calls the byte order through its
 * interface for every number. This walks the type descriptor instead, and when
 * the order is one of the three here it copies or swaps in place rather than
 * calling out, so an array of numbers in the machine's own order is a memcpy.
 * An order from somewhere else still goes through its vtable, one number at a
 * time, the way Go's does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/binary.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

/* ------------------------------------------------------------ byte orders */

#define BINARY_ORDER_DESC(sym, gname, tag)                                             \
    static const Type sym = {                                                          \
        {(const Byte *)gname, (Int)(sizeof gname - 1)},                                \
        {(const Byte *)"encoding/binary", 15},                                         \
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
        tag,                                                                           \
        NULL,                                                                          \
    }

BINARY_ORDER_DESC(binary_le_desc, "littleEndian", 0x62696c65U /* "bile" */);
BINARY_ORDER_DESC(binary_be_desc, "bigEndian", 0x62696265U /* "bibe" */);
BINARY_ORDER_DESC(binary_ne_desc, "nativeEndian", 0x62696e65U /* "bine" */);

Str binary_little_endian_string(void) {
    return BURROW_S("LittleEndian");
}
Str binary_big_endian_string(void) {
    return BURROW_S("BigEndian");
}
Str binary_native_endian_string(void) {
    return BURROW_S("NativeEndian");
}
Str binary_little_endian_go_string(void) {
    return BURROW_S("binary.LittleEndian");
}
Str binary_big_endian_go_string(void) {
    return BURROW_S("binary.BigEndian");
}
Str binary_native_endian_go_string(void) {
    return BURROW_S("binary.NativeEndian");
}

/* Grows b by n bytes and returns where they start, or NULL when it could not.
 * A zero Slice is Go's nil []byte. */
Slice binary__append(Alloc *a, Slice b, const void *p, Int n) {
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append(a, b, p, n);
}

static Byte *binary_grow(Alloc *a, Slice *b, Int n) {
    if (b->elem != NULL && n <= b->cap - b->len) {
        Byte *p = (Byte *)b->p + b->len;
        b->len += n;
        return p;
    }
    if (b->elem == NULL)
        *b = slice_nil(TYPE_BYTE);
    Int old = b->len;
    *b = slice_append(a, *b, NULL, n);
    return b->p == NULL ? NULL : (Byte *)b->p + old;
}

/* The methods, the vtables they fill and the values that point at those, for
 * each of the three. name is the prefix of the direct calls in binary.h. */
#define BINARY_ORDER(name, desc, vt, avt)                                              \
    static uint16_t name##_m16(void *self, Slice b) {                                  \
        (void)self;                                                                    \
        return binary_##name##_uint16(b);                                              \
    }                                                                                  \
    static uint32_t name##_m32(void *self, Slice b) {                                  \
        (void)self;                                                                    \
        return binary_##name##_uint32(b);                                              \
    }                                                                                  \
    static uint64_t name##_m64(void *self, Slice b) {                                  \
        (void)self;                                                                    \
        return binary_##name##_uint64(b);                                              \
    }                                                                                  \
    static void name##_mput16(void *self, Slice b, uint16_t v) {                       \
        (void)self;                                                                    \
        binary_##name##_put_uint16(b, v);                                              \
    }                                                                                  \
    static void name##_mput32(void *self, Slice b, uint32_t v) {                       \
        (void)self;                                                                    \
        binary_##name##_put_uint32(b, v);                                              \
    }                                                                                  \
    static void name##_mput64(void *self, Slice b, uint64_t v) {                       \
        (void)self;                                                                    \
        binary_##name##_put_uint64(b, v);                                              \
    }                                                                                  \
    static Str name##_mstring(void *self) {                                            \
        (void)self;                                                                    \
        return binary_##name##_string();                                               \
    }                                                                                  \
    static Slice name##_mappend16(void *self, Alloc *a, Slice b, uint16_t v) {         \
        (void)self;                                                                    \
        return binary_##name##_append_uint16(a, b, v);                                 \
    }                                                                                  \
    static Slice name##_mappend32(void *self, Alloc *a, Slice b, uint32_t v) {         \
        (void)self;                                                                    \
        return binary_##name##_append_uint32(a, b, v);                                 \
    }                                                                                  \
    static Slice name##_mappend64(void *self, Alloc *a, Slice b, uint64_t v) {         \
        (void)self;                                                                    \
        return binary_##name##_append_uint64(a, b, v);                                 \
    }                                                                                  \
    static const BinaryByteOrderVT vt = {&desc,         name##_m16,    name##_m32,     \
                                         name##_m64,    name##_mput16, name##_mput32,  \
                                         name##_mput64, name##_mstring};               \
    static const BinaryAppendByteOrderVT avt = {                                       \
        &desc, name##_mappend16, name##_mappend32, name##_mappend64, name##_mstring};  \
    const BinaryByteOrder binary_##name = {&vt, NULL};                                 \
    const BinaryAppendByteOrder binary_##name##_append = {&avt, NULL}

BINARY_ORDER(little_endian, binary_le_desc, binary_little_endian_vt,
             binary_little_endian_avt);
BINARY_ORDER(big_endian, binary_be_desc, binary_big_endian_vt, binary_big_endian_avt);
BINARY_ORDER(native_endian, binary_ne_desc, binary_native_endian_vt,
             binary_native_endian_avt);

/* ---------------------------------------------------------------- varints */

#define BINARY_OVERFLOW "binary: varint overflows a 64-bit integer"
static const Str binary_ovf_text = {(const Byte *)BINARY_OVERFLOW,
                                    sizeof BINARY_OVERFLOW - 1};
static const Error binary_err_overflow = {&burrow_sentinel_error_vt, &binary_ovf_text};

static const Str binary_too_small_text = {(const Byte *)"buffer too small", 16};
static const Error binary_err_buffer_too_small = {&burrow_sentinel_error_vt,
                                                  &binary_too_small_text};

Slice binary_append_uvarint(Alloc *a, Slice buf, uint64_t x) {
    Byte tmp[BINARY_MAX_VARINT_LEN64];
    Int n = 0;
    while (x >= 0x80) {
        tmp[n++] = (Byte)(x | 0x80);
        x >>= 7;
    }
    tmp[n++] = (Byte)x;
    Byte *p = binary_grow(a, &buf, n);
    if (p != NULL)
        memcpy(p, tmp, (size_t)n);
    return buf;
}

static uint64_t binary_zigzag(int64_t x) {
    uint64_t ux = (uint64_t)x << 1;
    if (x < 0)
        ux = ~ux;
    return ux;
}

static int64_t binary_unzigzag(uint64_t ux) {
    int64_t x = (int64_t)(ux >> 1);
    if (ux & 1)
        x = ~x;
    return x;
}

Slice binary_append_varint(Alloc *a, Slice buf, int64_t x) {
    return binary_append_uvarint(a, buf, binary_zigzag(x));
}

uint64_t binary_uvarint(Slice buf, Int *n) {
    const Byte *p = (const Byte *)buf.p;
    uint64_t x = 0;
    unsigned s = 0;
    for (Int i = 0; i < buf.len; i++) {
        Byte b = p[i];
        if (i == BINARY_MAX_VARINT_LEN64) {
            /* Go's guard against reading past ten bytes, issue 41185. */
            BURROW_OUT(n, -(i + 1));
            return 0;
        }
        if (b < 0x80) {
            if (i == BINARY_MAX_VARINT_LEN64 - 1 && b > 1) {
                BURROW_OUT(n, -(i + 1));
                return 0;
            }
            BURROW_OUT(n, i + 1);
            return x | (uint64_t)b << s;
        }
        x |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    }
    BURROW_OUT(n, 0);
    return 0;
}

int64_t binary_varint(Slice buf, Int *n) {
    return binary_unzigzag(binary_uvarint(buf, n));
}

static bool binary_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

uint64_t binary_read_uvarint(IoByteReader r, Error *err) {
    if (r.vt == NULL)
        runtime_panic(BURROW_S(
            "runtime error: invalid memory address or nil pointer dereference"));
    uint64_t x = 0;
    unsigned s = 0;
    for (int i = 0; i < BINARY_MAX_VARINT_LEN64; i++) {
        Error e = BURROW_NO_ERROR;
        Byte b = r.vt->read_byte(r.data, &e);
        if (BURROW_FAILED(e)) {
            if (i > 0 && binary_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            BURROW_OUT(err, e);
            return x;
        }
        if (b < 0x80) {
            if (i == BINARY_MAX_VARINT_LEN64 - 1 && b > 1) {
                BURROW_OUT(err, binary_err_overflow);
                return x;
            }
            BURROW_OUT(err, BURROW_NO_ERROR);
            return x | (uint64_t)b << s;
        }
        x |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    }
    BURROW_OUT(err, binary_err_overflow);
    return x;
}

int64_t binary_read_varint(IoByteReader r, Error *err) {
    return binary_unzigzag(binary_read_uvarint(r, err));
}

/* ------------------------------------------------------ fixed size values */

BURROW_NORETURN static void binary_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* The encoded size of one value of t, or -1 when t has no fixed size. */
static Int binary_type_size(const Type *t) {
    switch ((int)t->kind) {
    case KIND_BOOL:
    case KIND_INT8:
    case KIND_UINT8:
        return 1;
    case KIND_INT16:
    case KIND_UINT16:
        return 2;
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_FLOAT32:
        return 4;
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT64:
    case KIND_COMPLEX64:
        return 8;
    case KIND_COMPLEX128:
        return 16;
    case KIND_ARRAY: {
        Int s = binary_type_size(t->elem);
        return s < 0 ? -1 : s * (Int)t->len;
    }
    case KIND_STRUCT: {
        Int sum = 0;
        for (uint16_t i = 0; i < t->nfield; i++) {
            Int s = binary_type_size(t->fields[i].type);
            if (s < 0)
                return -1;
            sum += s;
        }
        return sum;
    }
    case KIND_INVALID:
    case KIND_INT:
    case KIND_UINT:
    case KIND_UINTPTR:
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_INTERFACE:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_SLICE:
    case KIND_STRING:
    case KIND_UNSAFE_POINTER:
    case KIND_MAX:
    default:
        return -1;
    }
}

/* Go keeps the sizes of struct types it has seen in a sync.Map. This is the
 * same idea kept per thread, so it needs no locking, in a small table where a
 * clash just means walking the type again. */
#define BINARY_SIZE_CACHE 64

static BURROW_THREAD_LOCAL struct {
    const Type *t;
    Int size;
} binary_size_cache[BINARY_SIZE_CACHE];

static Int binary_cached_size(const Type *t) {
    if (t->kind != KIND_STRUCT && t->kind != KIND_ARRAY)
        return binary_type_size(t);
    size_t h = ((uintptr_t)t >> 4) % BINARY_SIZE_CACHE;
    if (binary_size_cache[h].t == t)
        return binary_size_cache[h].size;
    Int size = binary_type_size(t);
    binary_size_cache[h].t = t;
    binary_size_cache[h].size = size;
    return size;
}

/* The types Go's fast path handles by type switch, whose nil pointers are
 * dereferenced rather than reported. */
static bool binary_fast_kind(const Type *t) {
    switch ((int)t->kind) {
    case KIND_BOOL:
    case KIND_INT8:
    case KIND_UINT8:
    case KIND_INT16:
    case KIND_UINT16:
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return true;
    default:
        return false;
    }
}

/* What an Any names, as n values of t starting at p. */
typedef struct BinaryTarget {
    const Type *t;
    Byte *p;
    Int n;
    Int size; /* of all n, or -1 */
} BinaryTarget;

enum {
    BINARY_TARGET_OK,
    BINARY_TARGET_INVALID, /* no fixed size, or a nil pointer to one that has */
    BINARY_TARGET_NIL,     /* a nil pointer to a fast kind, size is set */
};

static int binary_target(Any data, BinaryTarget *tg) {
    const Type *t = data.t;
    Byte *p = (Byte *)data.data;
    if (t == NULL || p == NULL)
        binary_nil_panic();
    if (t->kind == KIND_POINTER) {
        t = t->elem;
        p = *(Byte **)(void *)p;
        if (p == NULL) {
            if (binary_fast_kind(t)) {
                tg->size = binary_type_size(t);
                return BINARY_TARGET_NIL;
            }
            return BINARY_TARGET_INVALID;
        }
    }
    if (t->kind == KIND_SLICE) {
        Slice s = *(const Slice *)(const void *)p;
        tg->t = t->elem;
        tg->p = (Byte *)s.p;
        tg->n = s.len;
    } else {
        tg->t = t;
        tg->p = p;
        tg->n = 1;
    }
    Int es = binary_cached_size(tg->t);
    tg->size = es < 0 ? -1 : es * tg->n;
    return tg->size < 0 ? BINARY_TARGET_INVALID : BINARY_TARGET_OK;
}

/* How numbers of more than one byte get written: as they sit in memory, byte
 * swapped, or through somebody else's vtable. */
enum { BINARY_COPY, BINARY_SWAP, BINARY_CALL };

typedef struct BinaryCoder {
    int mode;
    BinaryByteOrder order;
    Byte *buf;
    Int off;
} BinaryCoder;

static int binary_mode(BinaryByteOrder o) {
    if (o.vt == &binary_native_endian_vt)
        return BINARY_COPY;
#if BURROW_LITTLE_ENDIAN
    if (o.vt == &binary_little_endian_vt)
        return BINARY_COPY;
    if (o.vt == &binary_big_endian_vt)
        return BINARY_SWAP;
#else
    if (o.vt == &binary_big_endian_vt)
        return BINARY_COPY;
    if (o.vt == &binary_little_endian_vt)
        return BINARY_SWAP;
#endif
    return BINARY_CALL;
}

static BinaryCoder binary_coder(BinaryByteOrder order, Byte *buf) {
    BinaryCoder c = {binary_mode(order), order, buf, 0};
    return c;
}

static Slice binary_at(BinaryCoder *c, Int n) {
    return (Slice){c->buf + c->off, n, n, TYPE_BYTE};
}

static void binary_check_order(BinaryCoder *c) {
    if (c->order.vt == NULL)
        binary_nil_panic();
}

static void binary_put16(BinaryCoder *c, const Byte *src) {
    uint16_t v;
    memcpy(&v, src, 2);
    if (c->mode == BINARY_SWAP)
        v = bits_reverse_bytes16(v);
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        c->order.vt->put_uint16(c->order.data, binary_at(c, 2), v);
    } else {
        memcpy(c->buf + c->off, &v, 2);
    }
    c->off += 2;
}

static void binary_put32(BinaryCoder *c, const Byte *src) {
    uint32_t v;
    memcpy(&v, src, 4);
    if (c->mode == BINARY_SWAP)
        v = bits_reverse_bytes32(v);
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        c->order.vt->put_uint32(c->order.data, binary_at(c, 4), v);
    } else {
        memcpy(c->buf + c->off, &v, 4);
    }
    c->off += 4;
}

static void binary_put64(BinaryCoder *c, const Byte *src) {
    uint64_t v;
    memcpy(&v, src, 8);
    if (c->mode == BINARY_SWAP)
        v = bits_reverse_bytes64(v);
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        c->order.vt->put_uint64(c->order.data, binary_at(c, 8), v);
    } else {
        memcpy(c->buf + c->off, &v, 8);
    }
    c->off += 8;
}

static void binary_get16(BinaryCoder *c, Byte *dst) {
    uint16_t v;
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        v = c->order.vt->uint16(c->order.data, binary_at(c, 2));
    } else {
        memcpy(&v, c->buf + c->off, 2);
        if (c->mode == BINARY_SWAP)
            v = bits_reverse_bytes16(v);
    }
    memcpy(dst, &v, 2);
    c->off += 2;
}

static void binary_get32(BinaryCoder *c, Byte *dst) {
    uint32_t v;
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        v = c->order.vt->uint32(c->order.data, binary_at(c, 4));
    } else {
        memcpy(&v, c->buf + c->off, 4);
        if (c->mode == BINARY_SWAP)
            v = bits_reverse_bytes32(v);
    }
    memcpy(dst, &v, 4);
    c->off += 4;
}

static void binary_get64(BinaryCoder *c, Byte *dst) {
    uint64_t v;
    if (c->mode == BINARY_CALL) {
        binary_check_order(c);
        v = c->order.vt->uint64(c->order.data, binary_at(c, 8));
    } else {
        memcpy(&v, c->buf + c->off, 8);
        if (c->mode == BINARY_SWAP)
            v = bits_reverse_bytes64(v);
    }
    memcpy(dst, &v, 8);
    c->off += 8;
}

/* A number type whose C layout is its encoding in the machine's order, so a
 * run of them can be copied as one block when no swapping is needed. */
static bool binary_plain(const Type *t) {
    switch ((int)t->kind) {
    case KIND_INT8:
    case KIND_UINT8:
    case KIND_INT16:
    case KIND_UINT16:
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_FLOAT32:
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT64:
    case KIND_COMPLEX64:
    case KIND_COMPLEX128:
        return (Int)t->size == binary_type_size(t);
    default:
        return false;
    }
}

static void binary_encode_values(BinaryCoder *c, const Type *t, const Byte *p, Int n);

static void binary_encode_value(BinaryCoder *c, const Type *t, const Byte *p) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        c->buf[c->off++] = *p != 0 ? 1 : 0;
        break;
    case KIND_INT8:
    case KIND_UINT8:
        c->buf[c->off++] = *p;
        break;
    case KIND_INT16:
    case KIND_UINT16:
        binary_put16(c, p);
        break;
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_FLOAT32:
        binary_put32(c, p);
        break;
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT64:
        binary_put64(c, p);
        break;
    case KIND_COMPLEX64:
        binary_put32(c, p);
        binary_put32(c, p + 4);
        break;
    case KIND_COMPLEX128:
        binary_put64(c, p);
        binary_put64(c, p + 8);
        break;
    case KIND_ARRAY:
        binary_encode_values(c, t->elem, p, (Int)t->len);
        break;
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (field_is_blank(f)) {
                Int s = binary_type_size(f->type);
                memset(c->buf + c->off, 0, (size_t)s);
                c->off += s;
            } else {
                binary_encode_value(c, f->type, p + f->offset);
            }
        }
        break;
    default:
        break;
    }
}

static void binary_encode_values(BinaryCoder *c, const Type *t, const Byte *p, Int n) {
    if (n <= 0)
        return;
    if (c->mode == BINARY_COPY && binary_plain(t)) {
        size_t sz = (size_t)n * t->size;
        memcpy(c->buf + c->off, p, sz);
        c->off += (Int)sz;
        return;
    }
    for (Int i = 0; i < n; i++)
        binary_encode_value(c, t, p + (size_t)i * t->size);
}

/* Go's decoder sets each number through reflect, and reflect refuses to set
 * one reached through an unexported field. This is its panic. */
BURROW_NORETURN static void binary_unexported(Kind k) {
    switch ((int)k) {
    case KIND_BOOL:
        panic_str(BURROW_S("reflect: reflect.Value.SetBool using value obtained using "
                           "unexported field"));
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        panic_str(BURROW_S("reflect: reflect.Value.SetInt using value obtained using "
                           "unexported field"));
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
        panic_str(BURROW_S("reflect: reflect.Value.SetUint using value obtained using "
                           "unexported field"));
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        panic_str(BURROW_S("reflect: reflect.Value.SetFloat using value obtained using "
                           "unexported field"));
    default:
        panic_str(BURROW_S("reflect: reflect.Value.SetComplex using value obtained "
                           "using unexported field"));
    }
}

static void binary_decode_values(BinaryCoder *c, const Type *t, Byte *p, Int n,
                                 bool ro);

static void binary_decode_value(BinaryCoder *c, const Type *t, Byte *p, bool ro) {
    if (ro && t->kind != KIND_ARRAY && t->kind != KIND_STRUCT)
        binary_unexported(t->kind);
    switch ((int)t->kind) {
    case KIND_BOOL:
        *p = c->buf[c->off++] != 0 ? 1 : 0;
        break;
    case KIND_INT8:
    case KIND_UINT8:
        *p = c->buf[c->off++];
        break;
    case KIND_INT16:
    case KIND_UINT16:
        binary_get16(c, p);
        break;
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_FLOAT32:
        binary_get32(c, p);
        break;
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT64:
        binary_get64(c, p);
        break;
    case KIND_COMPLEX64:
        binary_get32(c, p);
        binary_get32(c, p + 4);
        break;
    case KIND_COMPLEX128:
        binary_get64(c, p);
        binary_get64(c, p + 8);
        break;
    case KIND_ARRAY:
        binary_decode_values(c, t->elem, p, (Int)t->len, ro);
        break;
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (field_is_blank(f))
                c->off += binary_type_size(f->type);
            else
                binary_decode_value(c, f->type, p + f->offset,
                                    ro || !field_is_exported(f));
        }
        break;
    default:
        break;
    }
}

static void binary_decode_values(BinaryCoder *c, const Type *t, Byte *p, Int n,
                                 bool ro) {
    if (n <= 0)
        return;
    if (!ro && c->mode == BINARY_COPY && binary_plain(t)) {
        size_t sz = (size_t)n * t->size;
        memcpy(p, c->buf + c->off, sz);
        c->off += (Int)sz;
        return;
    }
    for (Int i = 0; i < n; i++)
        binary_decode_value(c, t, p + (size_t)i * t->size, ro);
}

/* The walk above goes through the type for every value, which is slow for a
 * slice of structs. A plan is the same walk done once, flattened into runs
 * that are each handled by a tight loop, and then applied to every element.
 * Runs next to each other in memory that are handled the same way merge, so
 * a struct with no padding and nothing to swap is one memcpy. */
enum {
    BINARY_OP_BYTES, /* n bytes as they are */
    BINARY_OP_BOOL,  /* n bools */
    BINARY_OP_SWAP2, /* n numbers, byte swapped */
    BINARY_OP_SWAP4,
    BINARY_OP_SWAP8,
    BINARY_OP_CALL2, /* n numbers through the order's vtable */
    BINARY_OP_CALL4,
    BINARY_OP_CALL8,
    BINARY_OP_SKIP, /* n bytes of blank field, zeros when encoding */
    BINARY_OP_RO,   /* an unexported number, which decoding panics on */
};

typedef struct BinaryOp {
    uint32_t mem; /* offset in the element */
    uint32_t n;
    uint8_t op;
    uint8_t kind; /* for BINARY_OP_RO */
} BinaryOp;

#define BINARY_MAX_OPS 64

typedef struct BinaryPlan {
    BinaryOp ops[BINARY_MAX_OPS];
    int nops;
    int mode;
    bool decode;
    bool stop; /* an RO op was added, and nothing after it can run */
} BinaryPlan;

static const uint8_t binary_op_width[] = {1, 1, 2, 4, 8, 2, 4, 8, 1, 0};

static bool binary_plan_add(BinaryPlan *pl, uint32_t mem, uint32_t n, uint8_t op) {
    if (n == 0 || pl->stop)
        return true;
    if (pl->nops > 0) {
        BinaryOp *last = &pl->ops[pl->nops - 1];
        if (last->op == op && op != BINARY_OP_RO &&
            last->mem + last->n * binary_op_width[op] == mem) {
            last->n += n;
            return true;
        }
    }
    if (pl->nops == BINARY_MAX_OPS)
        return false;
    pl->ops[pl->nops++] = (BinaryOp){mem, n, op, 0};
    return true;
}

/* The op for count numbers of width w. */
static bool binary_plan_numbers(BinaryPlan *pl, uint32_t mem, uint32_t count, int w) {
    if (w == 1 || pl->mode == BINARY_COPY)
        return binary_plan_add(pl, mem, count * (uint32_t)w, BINARY_OP_BYTES);
    uint8_t base = pl->mode == BINARY_SWAP ? BINARY_OP_SWAP2 : BINARY_OP_CALL2;
    uint8_t op = (uint8_t)(base + (w == 2 ? 0 : w == 4 ? 1 : 2));
    return binary_plan_add(pl, mem, count, op);
}

static bool binary_plan_type(BinaryPlan *pl, const Type *t, uint32_t mem,
                             uint32_t count, bool ro) {
    if (count == 0 || pl->stop)
        return true;
    Kind k = t->kind;
    if (ro && pl->decode && k != KIND_ARRAY && k != KIND_STRUCT) {
        if (pl->nops == BINARY_MAX_OPS)
            return false;
        pl->ops[pl->nops++] = (BinaryOp){mem, 1, BINARY_OP_RO, (uint8_t)k};
        pl->stop = true;
        return true;
    }
    switch ((int)k) {
    case KIND_BOOL:
        return binary_plan_add(pl, mem, count, BINARY_OP_BOOL);
    case KIND_INT8:
    case KIND_UINT8:
        return binary_plan_numbers(pl, mem, count, 1);
    case KIND_INT16:
    case KIND_UINT16:
        return binary_plan_numbers(pl, mem, count, 2);
    case KIND_INT32:
    case KIND_UINT32:
    case KIND_FLOAT32:
        return binary_plan_numbers(pl, mem, count, 4);
    case KIND_INT64:
    case KIND_UINT64:
    case KIND_FLOAT64:
        return binary_plan_numbers(pl, mem, count, 8);
    case KIND_COMPLEX64:
        return binary_plan_numbers(pl, mem, count * 2, 4);
    case KIND_COMPLEX128:
        return binary_plan_numbers(pl, mem, count * 2, 8);
    case KIND_ARRAY: {
        const Type *e = t->elem;
        if (e->kind != KIND_ARRAY && e->kind != KIND_STRUCT)
            return binary_plan_type(pl, e, mem, count * (uint32_t)t->len, ro);
        for (uint32_t i = 0; i < count * (uint32_t)t->len; i++)
            if (!binary_plan_type(pl, e, mem + i * (uint32_t)e->size, 1, ro))
                return false;
        return true;
    }
    case KIND_STRUCT:
        for (uint32_t i = 0; i < count; i++) {
            uint32_t base = mem + i * (uint32_t)t->size;
            for (uint16_t j = 0; j < t->nfield; j++) {
                const Field *f = &t->fields[j];
                bool ok;
                if (field_is_blank(f))
                    ok = binary_plan_add(pl, base + f->offset,
                                         (uint32_t)binary_type_size(f->type),
                                         BINARY_OP_SKIP);
                else
                    ok = binary_plan_type(pl, f->type, base + f->offset, 1,
                                          ro || !field_is_exported(f));
                if (!ok)
                    return false;
            }
        }
        return true;
    default:
        return true;
    }
}

/* Plans one element of t, or says it could not in BINARY_MAX_OPS runs. */
static bool binary_plan(BinaryPlan *pl, const Type *t, int mode, bool decode) {
    pl->nops = 0;
    pl->mode = mode;
    pl->decode = decode;
    pl->stop = false;
    return binary_plan_type(pl, t, 0, 1, false);
}

static void binary_check_nil(BinaryByteOrder order) {
    if (order.vt == NULL)
        binary_nil_panic();
}

static Byte *binary_run_encode(const BinaryPlan *pl, BinaryByteOrder order, Byte *dst,
                               const Byte *p) {
    for (int i = 0; i < pl->nops; i++) {
        const BinaryOp *op = &pl->ops[i];
        const Byte *s = p + op->mem;
        uint32_t n = op->n;
        switch (op->op) {
        case BINARY_OP_BYTES:
            memcpy(dst, s, n);
            dst += n;
            break;
        case BINARY_OP_BOOL:
            for (uint32_t j = 0; j < n; j++)
                dst[j] = s[j] != 0 ? 1 : 0;
            dst += n;
            break;
        case BINARY_OP_SWAP2:
            for (uint32_t j = 0; j < n; j++, dst += 2) {
                uint16_t v;
                memcpy(&v, s + 2 * j, 2);
                v = bits_reverse_bytes16(v);
                memcpy(dst, &v, 2);
            }
            break;
        case BINARY_OP_SWAP4:
            for (uint32_t j = 0; j < n; j++, dst += 4) {
                uint32_t v;
                memcpy(&v, s + 4 * j, 4);
                v = bits_reverse_bytes32(v);
                memcpy(dst, &v, 4);
            }
            break;
        case BINARY_OP_SWAP8:
            for (uint32_t j = 0; j < n; j++, dst += 8) {
                uint64_t v;
                memcpy(&v, s + 8 * j, 8);
                v = bits_reverse_bytes64(v);
                memcpy(dst, &v, 8);
            }
            break;
        case BINARY_OP_CALL2:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, dst += 2) {
                uint16_t v;
                memcpy(&v, s + 2 * j, 2);
                order.vt->put_uint16(order.data, (Slice){dst, 2, 2, TYPE_BYTE}, v);
            }
            break;
        case BINARY_OP_CALL4:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, dst += 4) {
                uint32_t v;
                memcpy(&v, s + 4 * j, 4);
                order.vt->put_uint32(order.data, (Slice){dst, 4, 4, TYPE_BYTE}, v);
            }
            break;
        case BINARY_OP_CALL8:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, dst += 8) {
                uint64_t v;
                memcpy(&v, s + 8 * j, 8);
                order.vt->put_uint64(order.data, (Slice){dst, 8, 8, TYPE_BYTE}, v);
            }
            break;
        case BINARY_OP_SKIP:
            memset(dst, 0, n);
            dst += n;
            break;
        default:
            break;
        }
    }
    return dst;
}

static const Byte *binary_run_decode(const BinaryPlan *pl, BinaryByteOrder order,
                                     const Byte *src, Byte *p) {
    for (int i = 0; i < pl->nops; i++) {
        const BinaryOp *op = &pl->ops[i];
        Byte *d = p + op->mem;
        uint32_t n = op->n;
        switch (op->op) {
        case BINARY_OP_BYTES:
            memcpy(d, src, n);
            src += n;
            break;
        case BINARY_OP_BOOL:
            for (uint32_t j = 0; j < n; j++)
                d[j] = src[j] != 0 ? 1 : 0;
            src += n;
            break;
        case BINARY_OP_SWAP2:
            for (uint32_t j = 0; j < n; j++, src += 2) {
                uint16_t v;
                memcpy(&v, src, 2);
                v = bits_reverse_bytes16(v);
                memcpy(d + 2 * j, &v, 2);
            }
            break;
        case BINARY_OP_SWAP4:
            for (uint32_t j = 0; j < n; j++, src += 4) {
                uint32_t v;
                memcpy(&v, src, 4);
                v = bits_reverse_bytes32(v);
                memcpy(d + 4 * j, &v, 4);
            }
            break;
        case BINARY_OP_SWAP8:
            for (uint32_t j = 0; j < n; j++, src += 8) {
                uint64_t v;
                memcpy(&v, src, 8);
                v = bits_reverse_bytes64(v);
                memcpy(d + 8 * j, &v, 8);
            }
            break;
        case BINARY_OP_CALL2:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, src += 2) {
                uint16_t v = order.vt->uint16(
                    order.data, (Slice){(Byte *)(uintptr_t)src, 2, 2, TYPE_BYTE});
                memcpy(d + 2 * j, &v, 2);
            }
            break;
        case BINARY_OP_CALL4:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, src += 4) {
                uint32_t v = order.vt->uint32(
                    order.data, (Slice){(Byte *)(uintptr_t)src, 4, 4, TYPE_BYTE});
                memcpy(d + 4 * j, &v, 4);
            }
            break;
        case BINARY_OP_CALL8:
            binary_check_nil(order);
            for (uint32_t j = 0; j < n; j++, src += 8) {
                uint64_t v = order.vt->uint64(
                    order.data, (Slice){(Byte *)(uintptr_t)src, 8, 8, TYPE_BYTE});
                memcpy(d + 8 * j, &v, 8);
            }
            break;
        case BINARY_OP_SKIP:
            src += n;
            break;
        case BINARY_OP_RO:
            binary_unexported((Kind)op->kind);
        default:
            break;
        }
    }
    return src;
}

/* A plan that is one run covering the whole element can cover n elements in
 * one go, which is what makes a []int32 a single loop. */
static bool binary_plan_scales(BinaryPlan *pl, const Type *t, Int n) {
    if (pl->nops != 1 || pl->ops[0].op == BINARY_OP_RO || pl->ops[0].mem != 0 ||
        (size_t)pl->ops[0].n * binary_op_width[pl->ops[0].op] != t->size ||
        n > (Int)(UINT32_MAX / t->size))
        return false;
    pl->ops[0].n *= (uint32_t)n;
    return true;
}

static void binary_encode_target(BinaryByteOrder order, Byte *buf,
                                 const BinaryTarget *tg) {
    BinaryCoder c = binary_coder(order, buf);
    if (tg->n <= 0)
        return;
    if (tg->n == 1 && binary_fast_kind(tg->t)) {
        binary_encode_value(&c, tg->t, tg->p);
        return;
    }
    BinaryPlan pl;
    if (binary_plan(&pl, tg->t, c.mode, false)) {
        if (binary_plan_scales(&pl, tg->t, tg->n)) {
            binary_run_encode(&pl, order, buf, tg->p);
            return;
        }
        for (Int i = 0; i < tg->n; i++)
            buf = binary_run_encode(&pl, order, buf, tg->p + (size_t)i * tg->t->size);
        return;
    }
    binary_encode_values(&c, tg->t, tg->p, tg->n);
}

static void binary_decode_target(BinaryByteOrder order, Byte *buf,
                                 const BinaryTarget *tg) {
    BinaryCoder c = binary_coder(order, buf);
    if (tg->n <= 0)
        return;
    if (tg->n == 1 && binary_fast_kind(tg->t)) {
        binary_decode_value(&c, tg->t, tg->p, false);
        return;
    }
    BinaryPlan pl;
    if (binary_plan(&pl, tg->t, c.mode, true)) {
        const Byte *src = buf;
        if (binary_plan_scales(&pl, tg->t, tg->n)) {
            binary_run_decode(&pl, order, src, tg->p);
            return;
        }
        for (Int i = 0; i < tg->n; i++)
            src = binary_run_decode(&pl, order, src, tg->p + (size_t)i * tg->t->size);
        return;
    }
    binary_decode_values(&c, tg->t, tg->p, tg->n, false);
}

Int binary_size(Any data) {
    if (data.t == NULL || data.data == NULL)
        return -1;
    BinaryTarget tg;
    return binary_target(data, &tg) == BINARY_TARGET_OK ? tg.size : -1;
}

/* Values up to this size are encoded on the stack rather than in a buffer
 * from the caller's allocator. */
#define BINARY_STACK 256

Error binary_read(Alloc *a, IoReader r, BinaryByteOrder order, Any data) {
    BinaryTarget tg;
    int k = binary_target(data, &tg);
    if (k == BINARY_TARGET_INVALID)
        return fmt_errorf_v("binary.Read: invalid type %T", data);
    Byte stack[BINARY_STACK];
    Byte *buf = stack;
    if (tg.size > BINARY_STACK) {
        buf = (Byte *)mem_alloc_nozero(a, (size_t)tg.size, 1);
        if (buf == NULL)
            return burrow_err_out_of_memory;
    }
    Error err = BURROW_NO_ERROR;
    io_read_full(r, (Slice){buf, tg.size, tg.size, TYPE_BYTE}, &err);
    if (BURROW_OK(err)) {
        /* Go reads before it finds out the pointer was nil. */
        if (k == BINARY_TARGET_NIL)
            binary_nil_panic();
        binary_decode_target(order, buf, &tg);
    }
    if (buf != stack)
        mem_free(a, buf, (size_t)tg.size, 1);
    return err;
}

Error binary_write(Alloc *a, IoWriter w, BinaryByteOrder order, Any data) {
    BinaryTarget tg;
    int k = binary_target(data, &tg);
    if (k == BINARY_TARGET_NIL)
        binary_nil_panic();
    if (k == BINARY_TARGET_INVALID)
        return fmt_errorf_v("binary.Write: some values are not fixed-sized in type %T",
                            data);
    if (w.vt == NULL)
        binary_nil_panic();
    Error err = BURROW_NO_ERROR;
    /* A []byte goes to the writer as it is, the way Go hands it over. */
    if (data.t->kind == KIND_SLICE && tg.t->kind == KIND_UINT8) {
        w.vt->write(w.data, *(const Slice *)data.data, &err);
        return err;
    }
    Byte stack[BINARY_STACK];
    Byte *buf = stack;
    if (tg.size > BINARY_STACK) {
        buf = (Byte *)mem_alloc_nozero(a, (size_t)tg.size, 1);
        if (buf == NULL)
            return burrow_err_out_of_memory;
    }
    binary_encode_target(order, buf, &tg);
    w.vt->write(w.data, (Slice){buf, tg.size, tg.size, TYPE_BYTE}, &err);
    if (buf != stack)
        mem_free(a, buf, (size_t)tg.size, 1);
    return err;
}

Int binary_decode(Slice buf, BinaryByteOrder order, Any data, Error *err) {
    BinaryTarget tg;
    int k = binary_target(data, &tg);
    if (k == BINARY_TARGET_INVALID) {
        BURROW_OUT(err, fmt_errorf_v("binary.Decode: invalid type %T", data));
        return 0;
    }
    if (buf.len < tg.size) {
        BURROW_OUT(err, binary_err_buffer_too_small);
        return 0;
    }
    if (k == BINARY_TARGET_NIL)
        binary_nil_panic();
    binary_decode_target(order, (Byte *)buf.p, &tg);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return tg.size;
}

Int binary_encode(Slice buf, BinaryByteOrder order, Any data, Error *err) {
    BinaryTarget tg;
    int k = binary_target(data, &tg);
    if (k == BINARY_TARGET_INVALID) {
        BURROW_OUT(err, fmt_errorf_v(
                            "binary.Encode: some values are not fixed-sized in type %T",
                            data));
        return 0;
    }
    if (buf.len < tg.size) {
        BURROW_OUT(err, binary_err_buffer_too_small);
        return 0;
    }
    if (k == BINARY_TARGET_NIL)
        binary_nil_panic();
    binary_encode_target(order, (Byte *)buf.p, &tg);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return tg.size;
}

Slice binary_append(Alloc *a, Slice buf, BinaryByteOrder order, Any data, Error *err) {
    BinaryTarget tg;
    int k = binary_target(data, &tg);
    if (k == BINARY_TARGET_INVALID) {
        BURROW_OUT(err, fmt_errorf_v(
                            "binary.Append: some values are not fixed-sized in type %T",
                            data));
        return (Slice){NULL, 0, 0, NULL};
    }
    if (k == BINARY_TARGET_NIL)
        binary_nil_panic();
    Byte *p = binary_grow(a, &buf, tg.size);
    if (p == NULL && tg.size > 0) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Slice){NULL, 0, 0, NULL};
    }
    if (tg.size > 0)
        binary_encode_target(order, p, &tg);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return buf;
}
