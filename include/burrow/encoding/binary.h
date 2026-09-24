/* encoding/binary, numbers to and from bytes in a fixed order, and varints.
 *
 * The byte orders read and write one unsigned integer at a time:
 *
 *     uint32_t n = binary_big_endian_uint32(b);
 *     binary_little_endian_put_uint16(b, 0x1234);
 *
 * Those are inline and cost a load or a store and a byte swap. The same calls
 * are also behind the BinaryByteOrder interface, for code that takes the order
 * as a parameter, and binary_little_endian, binary_big_endian and
 * binary_native_endian are the three values of it.
 *
 * binary_read, binary_write and the rest take a value of any fixed size type,
 * which is a bool, an integer or float with its size in its name, a complex
 * number, or an array or struct made only of those, or a slice of any of them.
 * The value goes in as an Any. Go's version takes a pointer for the functions
 * that fill the value in, and here an Any already points at its value, so it
 * is the variable itself, the same as fmt's scan operands:
 *
 *     Header h;
 *     Error err = binary_read(a, r, binary_big_endian, BURROW_ANY(TYPE_OF(Header), &h));
 *
 * An Any holding a pointer works too and means what it means in Go: the
 * pointer is followed once, and a nil one is Go's nil pointer.
 *
 * Struct padding is not written. The fields go out one after another, so a
 * struct of an int32 and an int16 is six bytes here as it is in Go. A blank
 * field, one that field_is_blank says yes to, is written as zeros and skipped
 * on the way in. Decoding into any other field that is not exported panics the
 * way Go's reflect does, since the bytes had nowhere they were allowed to go.
 *
 * The varint functions are the protocol buffer encoding: seven bits a byte,
 * least significant group first, and zig-zag for the signed forms.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/binary */

#ifndef BURROW_ENCODING_BINARY_H
#define BURROW_ENCODING_BINARY_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ byte orders */

/* binary.ByteOrder. Each call panics with Go's index message when b is too
 * short, before it reads or writes anything. */
typedef struct BinaryByteOrderVT {
    const Type *self_type;
    uint16_t (*uint16)(void *self, Slice b);
    uint32_t (*uint32)(void *self, Slice b);
    uint64_t (*uint64)(void *self, Slice b);
    void (*put_uint16)(void *self, Slice b, uint16_t v);
    void (*put_uint32)(void *self, Slice b, uint32_t v);
    void (*put_uint64)(void *self, Slice b, uint64_t v);
    Str (*string)(void *self);
} BinaryByteOrderVT;

typedef struct BinaryByteOrder {
    const BinaryByteOrderVT *vt;
    void *data;
} BinaryByteOrder;

/* binary.AppendByteOrder. Each call appends to b, growing it from a when it
 * is full, and returns the result, as append does. */
typedef struct BinaryAppendByteOrderVT {
    const Type *self_type;
    Slice (*append_uint16)(void *self, Alloc *a, Slice b, uint16_t v);
    Slice (*append_uint32)(void *self, Alloc *a, Slice b, uint32_t v);
    Slice (*append_uint64)(void *self, Alloc *a, Slice b, uint64_t v);
    Str (*string)(void *self);
} BinaryAppendByteOrderVT;

typedef struct BinaryAppendByteOrder {
    const BinaryAppendByteOrderVT *vt;
    void *data;
} BinaryAppendByteOrder;

/* Go's LittleEndian, BigEndian and NativeEndian as a ByteOrder, and the same
 * three as an AppendByteOrder. NativeEndian is whichever of the other two the
 * machine is, and says "NativeEndian" when asked its name. */
extern const BinaryByteOrder binary_little_endian;
extern const BinaryByteOrder binary_big_endian;
extern const BinaryByteOrder binary_native_endian;

extern const BinaryAppendByteOrder binary_little_endian_append;
extern const BinaryAppendByteOrder binary_big_endian_append;
extern const BinaryAppendByteOrder binary_native_endian_append;

/* The methods of the three, called directly. */

static inline uint16_t binary__load16(Slice b) {
    if (b.len < 2)
        runtime_index_out_of_range(1, b.len);
    uint16_t v;
    memcpy(&v, b.p, 2);
    return v;
}

static inline uint32_t binary__load32(Slice b) {
    if (b.len < 4)
        runtime_index_out_of_range(3, b.len);
    uint32_t v;
    memcpy(&v, b.p, 4);
    return v;
}

static inline uint64_t binary__load64(Slice b) {
    if (b.len < 8)
        runtime_index_out_of_range(7, b.len);
    uint64_t v;
    memcpy(&v, b.p, 8);
    return v;
}

static inline void binary__store16(Slice b, uint16_t v) {
    if (b.len < 2)
        runtime_index_out_of_range(1, b.len);
    memcpy(b.p, &v, 2);
}

static inline void binary__store32(Slice b, uint32_t v) {
    if (b.len < 4)
        runtime_index_out_of_range(3, b.len);
    memcpy(b.p, &v, 4);
}

static inline void binary__store64(Slice b, uint64_t v) {
    if (b.len < 8)
        runtime_index_out_of_range(7, b.len);
    memcpy(b.p, &v, 8);
}

#if BURROW_LITTLE_ENDIAN
#define BINARY__LE16(v) (v)
#define BINARY__LE32(v) (v)
#define BINARY__LE64(v) (v)
#define BINARY__BE16(v) bits_reverse_bytes16(v)
#define BINARY__BE32(v) bits_reverse_bytes32(v)
#define BINARY__BE64(v) bits_reverse_bytes64(v)
#else
#define BINARY__LE16(v) bits_reverse_bytes16(v)
#define BINARY__LE32(v) bits_reverse_bytes32(v)
#define BINARY__LE64(v) bits_reverse_bytes64(v)
#define BINARY__BE16(v) (v)
#define BINARY__BE32(v) (v)
#define BINARY__BE64(v) (v)
#endif

static inline uint16_t binary_little_endian_uint16(Slice b) {
    return BINARY__LE16(binary__load16(b));
}
static inline uint32_t binary_little_endian_uint32(Slice b) {
    return BINARY__LE32(binary__load32(b));
}
static inline uint64_t binary_little_endian_uint64(Slice b) {
    return BINARY__LE64(binary__load64(b));
}
static inline void binary_little_endian_put_uint16(Slice b, uint16_t v) {
    binary__store16(b, BINARY__LE16(v));
}
static inline void binary_little_endian_put_uint32(Slice b, uint32_t v) {
    binary__store32(b, BINARY__LE32(v));
}
static inline void binary_little_endian_put_uint64(Slice b, uint64_t v) {
    binary__store64(b, BINARY__LE64(v));
}

static inline uint16_t binary_big_endian_uint16(Slice b) {
    return BINARY__BE16(binary__load16(b));
}
static inline uint32_t binary_big_endian_uint32(Slice b) {
    return BINARY__BE32(binary__load32(b));
}
static inline uint64_t binary_big_endian_uint64(Slice b) {
    return BINARY__BE64(binary__load64(b));
}
static inline void binary_big_endian_put_uint16(Slice b, uint16_t v) {
    binary__store16(b, BINARY__BE16(v));
}
static inline void binary_big_endian_put_uint32(Slice b, uint32_t v) {
    binary__store32(b, BINARY__BE32(v));
}
static inline void binary_big_endian_put_uint64(Slice b, uint64_t v) {
    binary__store64(b, BINARY__BE64(v));
}

static inline uint16_t binary_native_endian_uint16(Slice b) {
    return binary__load16(b);
}
static inline uint32_t binary_native_endian_uint32(Slice b) {
    return binary__load32(b);
}
static inline uint64_t binary_native_endian_uint64(Slice b) {
    return binary__load64(b);
}
static inline void binary_native_endian_put_uint16(Slice b, uint16_t v) {
    binary__store16(b, v);
}
static inline void binary_native_endian_put_uint32(Slice b, uint32_t v) {
    binary__store32(b, v);
}
static inline void binary_native_endian_put_uint64(Slice b, uint64_t v) {
    binary__store64(b, v);
}

/* AppendUint16 and the rest. The common case, where b has room, is done here
 * so it inlines, and growing b is left to binary__append. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice binary__append(Alloc *a, Slice b,
                                                             const void *p, Int n);

#define BINARY__APPEND(name, bits, conv)                                               \
    static inline Slice binary_##name##_append_uint##bits(Alloc *a, Slice b,           \
                                                          uint##bits##_t v) {          \
        v = conv(v);                                                                   \
        if (b.elem != NULL && b.cap - b.len >= (bits) / 8) {                           \
            memcpy((Byte *)b.p + b.len, &v, (bits) / 8);                               \
            b.len += (bits) / 8;                                                       \
            return b;                                                                  \
        }                                                                              \
        return binary__append(a, b, &v, (bits) / 8);                                   \
    }

#define BINARY__NATIVE(v) (v)
BINARY__APPEND(little_endian, 16, BINARY__LE16)
BINARY__APPEND(little_endian, 32, BINARY__LE32)
BINARY__APPEND(little_endian, 64, BINARY__LE64)
BINARY__APPEND(big_endian, 16, BINARY__BE16)
BINARY__APPEND(big_endian, 32, BINARY__BE32)
BINARY__APPEND(big_endian, 64, BINARY__BE64)
BINARY__APPEND(native_endian, 16, BINARY__NATIVE)
BINARY__APPEND(native_endian, 32, BINARY__NATIVE)
BINARY__APPEND(native_endian, 64, BINARY__NATIVE)

/* "LittleEndian", "BigEndian" and "NativeEndian", and the GoString forms with
 * "binary." in front. */
BURROW_STATIC(ret) Str binary_little_endian_string(void);
BURROW_STATIC(ret) Str binary_big_endian_string(void);
BURROW_STATIC(ret) Str binary_native_endian_string(void);
BURROW_STATIC(ret) Str binary_little_endian_go_string(void);
BURROW_STATIC(ret) Str binary_big_endian_go_string(void);
BURROW_STATIC(ret) Str binary_native_endian_go_string(void);

/* ---------------------------------------------------------------- varints */

/* The most bytes a varint of each size takes. */
#define BINARY_MAX_VARINT_LEN16 3
#define BINARY_MAX_VARINT_LEN32 5
#define BINARY_MAX_VARINT_LEN64 10

/* Appends the varint for x to buf and returns the result, as append does. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, buf) Slice binary_append_uvarint(Alloc *a,
                                                                      Slice buf,
                                                                      uint64_t x);
BURROW_OWNS(ret) BURROW_BORROWS(ret, buf) Slice binary_append_varint(Alloc *a,
                                                                     Slice buf,
                                                                     int64_t x);

/* Writes the varint for x at the start of buf and returns how many bytes that
 * took. buf has to be big enough, and a short one panics with Go's index
 * message once the write reaches its end. */
static inline Int binary_put_uvarint(Slice buf, uint64_t x) {
    Byte *p = (Byte *)buf.p;
    Int i = 0;
    while (x >= 0x80) {
        if (i >= buf.len)
            runtime_index_out_of_range(i, buf.len);
        p[i++] = (Byte)(x | 0x80);
        x >>= 7;
    }
    if (i >= buf.len)
        runtime_index_out_of_range(i, buf.len);
    p[i] = (Byte)x;
    return i + 1;
}

static inline Int binary_put_varint(Slice buf, int64_t x) {
    uint64_t ux = (uint64_t)x << 1;
    if (x < 0)
        ux = ~ux;
    return binary_put_uvarint(buf, ux);
}

/* Reads a varint from the start of buf and returns it, with the number of
 * bytes it took through n. n is 0 when buf ends first, and -(the bytes read)
 * when the value is too big for 64 bits, with 0 returned for the value. */
uint64_t binary_uvarint(Slice buf, Int *n);
int64_t binary_varint(Slice buf, Int *n);

/* Reads a varint one byte at a time from r. The error is r's, but running out
 * partway through is io_err_unexpected_eof, and a value too big for 64 bits is
 * an error of its own. What was read so far comes back even on an error. */
uint64_t binary_read_uvarint(IoByteReader r, Error *err);
int64_t binary_read_varint(IoByteReader r, Error *err);

/* ------------------------------------------------------ fixed size values */

/* The bytes data takes when encoded, or -1 when it has no fixed size or is a
 * nil pointer. A slice counts its elements. */
Int binary_size(Any data);

/* Reads exactly binary_size(data) bytes from r and decodes them into data.
 * The bytes are read into a buffer from a first, so nothing in data changes
 * unless all of them arrive. The error is io_eof when nothing arrived and
 * io_err_unexpected_eof when only some did. */
BURROW_BORROWS(ret) Error binary_read(Alloc *a, IoReader r, BinaryByteOrder order,
                                      Any data);

/* Encodes data into a buffer from a and writes it to w in one call. */
BURROW_BORROWS(ret) Error binary_write(Alloc *a, IoWriter w, BinaryByteOrder order,
                                       Any data);

/* Decodes data from the start of buf and returns how many bytes that took. A
 * buf shorter than the value is an error, and nothing is decoded. */
Int binary_decode(Slice buf, BinaryByteOrder order, Any data, Error *err);

/* Encodes data at the start of buf and returns how many bytes that took. A
 * buf shorter than the value is an error, and nothing is written. */
Int binary_encode(Slice buf, BinaryByteOrder order, Any data, Error *err);

/* Appends the encoding of data to buf and returns the result, as append does.
 * On an error the result is a nil slice, as in Go. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, buf) Slice binary_append(Alloc *a, Slice buf,
                                                              BinaryByteOrder order,
                                                              Any data, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_BINARY_H */
