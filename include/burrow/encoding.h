/* encoding: the interfaces for turning a value into bytes or text and back.
 *
 * Go's encoding package is six interfaces and nothing else. encoding/json,
 * encoding/xml, encoding/gob and the hashes all look for them on a value they
 * were handed, and use them in place of their own rules when they are there.
 *
 * A type implements one the way it implements fmt's Stringer: by listing the
 * method in its descriptor. The signature macros below spell out the shape, so
 * the method list reads like Go's:
 *
 *     static Slice point_marshal_text(Point *p, Alloc *a, Error *err) {
 *         BURROW_OUT(err, BURROW_NO_ERROR);
 *         Str s = fmt_sprintf_v(a, "%d,%d", p->X, p->Y);
 *         return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
 *     }
 *
 *     #define POINT_METHODS(M, T) \
 *         M(T, MarshalText, point_marshal_text, ENCODING_SIG_MARSHAL_TEXT)
 *     BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);
 *
 * and anything holding the value as an Any can ask for it:
 *
 *     Point p = {1, 2};
 *     Any v = BURROW_ANY(TYPE_OF(Point), &p);
 *     if (encoding_is_text_marshaler(v))
 *         text = encoding_marshal_text(a, v, &err);
 *
 * The methods take an allocator where Go's lean on the collector. The bytes a
 * marshal method returns belong to the caller, and an unmarshal method puts
 * anything it has to keep in the allocator it is given.
 *
 * The interface types exist for code that wants to hold one, the same as
 * fmt's FmtStringer. An Any whose type is one of their descriptors is found
 * too, so the calls below work whichever way the value arrived.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding */

#ifndef BURROW_ENCODING_H
#define BURROW_ENCODING_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- interfaces */

/* encoding.BinaryMarshaler: the value in a binary form of the type's own
 * choosing. */
typedef struct EncodingBinaryMarshalerVT {
    const Type *self_type;
    Slice (*marshal_binary)(void *self, Alloc *a, Error *err);
} EncodingBinaryMarshalerVT;

typedef struct EncodingBinaryMarshaler {
    const EncodingBinaryMarshalerVT *vt;
    void *data;
} EncodingBinaryMarshaler;

/* encoding.BinaryUnmarshaler: the value set from what MarshalBinary made. It
 * has to copy data if it keeps any of it. */
typedef struct EncodingBinaryUnmarshalerVT {
    const Type *self_type;
    Error (*unmarshal_binary)(void *self, Alloc *a, Slice data);
} EncodingBinaryUnmarshalerVT;

typedef struct EncodingBinaryUnmarshaler {
    const EncodingBinaryUnmarshalerVT *vt;
    void *data;
} EncodingBinaryUnmarshaler;

/* encoding.BinaryAppender: MarshalBinary onto the end of b, growing it from a
 * if it has to. It must not keep b or change the bytes already in it. */
typedef struct EncodingBinaryAppenderVT {
    const Type *self_type;
    Slice (*append_binary)(void *self, Alloc *a, Slice b, Error *err);
} EncodingBinaryAppenderVT;

typedef struct EncodingBinaryAppender {
    const EncodingBinaryAppenderVT *vt;
    void *data;
} EncodingBinaryAppender;

/* encoding.TextMarshaler: the value as UTF-8 text. */
typedef struct EncodingTextMarshalerVT {
    const Type *self_type;
    Slice (*marshal_text)(void *self, Alloc *a, Error *err);
} EncodingTextMarshalerVT;

typedef struct EncodingTextMarshaler {
    const EncodingTextMarshalerVT *vt;
    void *data;
} EncodingTextMarshaler;

/* encoding.TextUnmarshaler: the value set from text MarshalText made. It has
 * to copy text if it keeps any of it. */
typedef struct EncodingTextUnmarshalerVT {
    const Type *self_type;
    Error (*unmarshal_text)(void *self, Alloc *a, Slice text);
} EncodingTextUnmarshalerVT;

typedef struct EncodingTextUnmarshaler {
    const EncodingTextUnmarshalerVT *vt;
    void *data;
} EncodingTextUnmarshaler;

/* encoding.TextAppender: MarshalText onto the end of b, with the same rules
 * as BinaryAppender. */
typedef struct EncodingTextAppenderVT {
    const Type *self_type;
    Slice (*append_text)(void *self, Alloc *a, Slice b, Error *err);
} EncodingTextAppenderVT;

typedef struct EncodingTextAppender {
    const EncodingTextAppenderVT *vt;
    void *data;
} EncodingTextAppender;

/* The descriptors of the six interface types, so that an Any can hold one. */
extern const Type burrow_type_EncodingBinaryMarshaler;
extern const Type burrow_type_EncodingBinaryUnmarshaler;
extern const Type burrow_type_EncodingBinaryAppender;
extern const Type burrow_type_EncodingTextMarshaler;
extern const Type burrow_type_EncodingTextUnmarshaler;
extern const Type burrow_type_EncodingTextAppender;

/* ------------------------------------------------------------ method shapes
 *
 * What a method in a descriptor has to look like to count. The C function
 * takes the receiver as a pointer, then the arguments in this order:
 *
 *     Slice marshal(T *self, Alloc *a, Error *err);
 *     Slice append(T *self, Alloc *a, Slice b, Error *err);
 *     Error unmarshal(T *self, Alloc *a, Slice data);
 *
 * A method with the right name and some other signature is not the
 * interface's method, which is Go's rule too. */

/* Alloc * and Error * under one word each, since a signature list names its
 * types by a single token. Nothing else needs these. */
typedef Alloc *EncodingAllocArg;
typedef Error *EncodingErrorArg;
extern const Type burrow_type_EncodingAllocArg;
extern const Type burrow_type_EncodingErrorArg;

#define ENCODING_SIG_MARSHAL_BINARY(IN, OUT)                                           \
    IN(0, EncodingAllocArg) IN(1, EncodingErrorArg) OUT(Bytes)
#define ENCODING_SIG_UNMARSHAL_BINARY(IN, OUT)                                         \
    IN(0, EncodingAllocArg) IN(1, Bytes) OUT(Error)
#define ENCODING_SIG_APPEND_BINARY(IN, OUT)                                            \
    IN(0, EncodingAllocArg) IN(1, Bytes) IN(2, EncodingErrorArg) OUT(Bytes)
#define ENCODING_SIG_MARSHAL_TEXT(IN, OUT) ENCODING_SIG_MARSHAL_BINARY(IN, OUT)
#define ENCODING_SIG_UNMARSHAL_TEXT(IN, OUT) ENCODING_SIG_UNMARSHAL_BINARY(IN, OUT)
#define ENCODING_SIG_APPEND_TEXT(IN, OUT) ENCODING_SIG_APPEND_BINARY(IN, OUT)

/* ------------------------------------------------------------ asking a value
 *
 * Go's v.(encoding.TextMarshaler) and the call after it. The is functions are
 * the comma ok form. The others make the call, and on a value that does not
 * implement the interface they return the nil slice and Go's type assertion
 * error rather than panicking, which is the rule iface_assert follows too.
 *
 * The method is looked for on v's type, and when v is a pointer, on the type
 * it points at, called on what it points at. A nil pointer is passed to the
 * method as NULL, as Go passes a nil receiver, so a method that can be called
 * on nil has to check. When v holds an interface value, error included, the
 * method is looked for on the type inside it. */

bool encoding_is_binary_marshaler(Any v);
bool encoding_is_binary_unmarshaler(Any v);
bool encoding_is_binary_appender(Any v);
bool encoding_is_text_marshaler(Any v);
bool encoding_is_text_unmarshaler(Any v);
bool encoding_is_text_appender(Any v);

BURROW_OWNS(ret) Slice encoding_marshal_binary(Alloc *a, Any v, Error *err);
BURROW_BORROWS(ret) Error encoding_unmarshal_binary(Alloc *a, Any v, Slice data);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice encoding_append_binary(Alloc *a, Any v,
                                                                     Slice b,
                                                                     Error *err);
BURROW_OWNS(ret) Slice encoding_marshal_text(Alloc *a, Any v, Error *err);
BURROW_BORROWS(ret) Error encoding_unmarshal_text(Alloc *a, Any v, Slice text);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice encoding_append_text(Alloc *a, Any v,
                                                                   Slice b, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_H */
