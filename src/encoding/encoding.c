/* Derived from Go's src/encoding/encoding.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/slice.h"
#include "burrow/type.h"

/* Go's encoding.go is the six interface declarations. What is here is the part
 * Go's compiler does for it: finding the method on a value and calling it. */

/* ------------------------------------------------------------- descriptors */

#define ENCODING_IFACE_TYPE(cname, gonm)                                               \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"encoding", 8},                                                 \
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

ENCODING_IFACE_TYPE(EncodingBinaryMarshaler, "BinaryMarshaler");
ENCODING_IFACE_TYPE(EncodingBinaryUnmarshaler, "BinaryUnmarshaler");
ENCODING_IFACE_TYPE(EncodingBinaryAppender, "BinaryAppender");
ENCODING_IFACE_TYPE(EncodingTextMarshaler, "TextMarshaler");
ENCODING_IFACE_TYPE(EncodingTextUnmarshaler, "TextUnmarshaler");
ENCODING_IFACE_TYPE(EncodingTextAppender, "TextAppender");

/* The two argument types exist only to be named in a signature, so they are
 * unsafe pointers as far as reflection can tell, with names that say what
 * they point at. */
#define ENCODING_ARG_TYPE(cname, gonm)                                                 \
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

ENCODING_ARG_TYPE(EncodingAllocArg, "*Alloc");
ENCODING_ARG_TYPE(EncodingErrorArg, "*error");

/* ------------------------------------------------------------ the methods */

typedef enum EncodingShape {
    ENCODING_SHAPE_MARSHAL,
    ENCODING_SHAPE_UNMARSHAL,
    ENCODING_SHAPE_APPEND
} EncodingShape;

/* One of the six interfaces: its Go name, its method's name and shape, and
 * its descriptor, for an Any that holds the interface value itself. */
typedef struct EncodingWant {
    Str iface;
    Str method;
    EncodingShape shape;
    const Type *t;
} EncodingWant;

#define ENCODING_WANT(iname, mname, shape, cname)                                      \
    {{(const Byte *)(iname), (Int)(sizeof(iname) - 1)},                                \
     {(const Byte *)(mname), (Int)(sizeof(mname) - 1)},                                \
     (shape),                                                                          \
     &burrow_type_##cname}

static const EncodingWant encoding_wants[] = {
    ENCODING_WANT("BinaryMarshaler", "MarshalBinary", ENCODING_SHAPE_MARSHAL,
                  EncodingBinaryMarshaler),
    ENCODING_WANT("BinaryUnmarshaler", "UnmarshalBinary", ENCODING_SHAPE_UNMARSHAL,
                  EncodingBinaryUnmarshaler),
    ENCODING_WANT("BinaryAppender", "AppendBinary", ENCODING_SHAPE_APPEND,
                  EncodingBinaryAppender),
    ENCODING_WANT("TextMarshaler", "MarshalText", ENCODING_SHAPE_MARSHAL,
                  EncodingTextMarshaler),
    ENCODING_WANT("TextUnmarshaler", "UnmarshalText", ENCODING_SHAPE_UNMARSHAL,
                  EncodingTextUnmarshaler),
    ENCODING_WANT("TextAppender", "AppendText", ENCODING_SHAPE_APPEND,
                  EncodingTextAppender),
};

#define want_binary_marshaler encoding_wants[0]
#define want_binary_unmarshaler encoding_wants[1]
#define want_binary_appender encoding_wants[2]
#define want_text_marshaler encoding_wants[3]
#define want_text_unmarshaler encoding_wants[4]
#define want_text_appender encoding_wants[5]

static bool encoding_shape_matches(const Method *m, EncodingShape shape) {
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    const Type *f = m->ftype;
    if (type_num_out(f) != 1)
        return false;
    switch (shape) {
    case ENCODING_SHAPE_MARSHAL:
        return type_num_in(f) == 2 && type_in(f, 0) == &burrow_type_EncodingAllocArg &&
               type_in(f, 1) == &burrow_type_EncodingErrorArg &&
               type_out(f, 0) == TYPE_BYTES;
    case ENCODING_SHAPE_UNMARSHAL:
        return type_num_in(f) == 2 && type_in(f, 0) == &burrow_type_EncodingAllocArg &&
               type_in(f, 1) == TYPE_BYTES && type_out(f, 0) == TYPE_ERROR;
    case ENCODING_SHAPE_APPEND:
        return type_num_in(f) == 3 && type_in(f, 0) == &burrow_type_EncodingAllocArg &&
               type_in(f, 1) == TYPE_BYTES &&
               type_in(f, 2) == &burrow_type_EncodingErrorArg &&
               type_out(f, 0) == TYPE_BYTES;
    default:
        return false;
    }
}

/* What a call goes through: the interface's own vtable when v holds the
 * interface value, or a method from a descriptor and the receiver to call it
 * on. */
typedef struct EncodingFound {
    const void *vt;
    const Method *m;
    void *recv;
} EncodingFound;

static bool encoding_find(Any v, const EncodingWant *w, EncodingFound *out) {
    out->vt = NULL;
    out->m = NULL;
    out->recv = NULL;
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
        if (t == w->t) {
            out->vt = in->vt;
            out->recv = in->data;
            return true;
        }
        t = in->vt->self_type;
        d = in->data;
        if (t == NULL)
            return false;
        const Method *m = type_method_by_name(t, w->method);
        if (!encoding_shape_matches(m, w->shape))
            return false;
        out->m = m;
        out->recv = d;
        return true;
    }
    const Method *m = type_method_by_name(t, w->method);
    if (encoding_shape_matches(m, w->shape)) {
        out->m = m;
        out->recv = d;
        return true;
    }
    if (t->kind == KIND_POINTER && t->elem != NULL) {
        m = type_method_by_name(t->elem, w->method);
        if (encoding_shape_matches(m, w->shape)) {
            out->m = m;
            out->recv = *(void **)d;
            return true;
        }
    }
    return false;
}

/* Go's runtime.TypeAssertionError, for the missing method case. */
static Error encoding_not_implemented(Any v, const EncodingWant *w) {
    if (v.t == NULL)
        return fmt_errorf_v("interface conversion: interface is nil, not encoding.%s",
                            w->iface);
    return fmt_errorf_v(
        "interface conversion: %T is not encoding.%s: missing method %s", v, w->iface,
        w->method);
}

static Slice encoding_do_marshal(Alloc *a, Any v, const EncodingWant *w, Error *err) {
    EncodingFound f;
    if (!encoding_find(v, w, &f)) {
        BURROW_OUT(err, encoding_not_implemented(v, w));
        return slice_nil(TYPE_BYTE);
    }
    Error e = BURROW_NO_ERROR;
    Slice out;
    if (f.vt != NULL) {
        if (w == &want_binary_marshaler)
            out = ((const EncodingBinaryMarshalerVT *)f.vt)
                      ->marshal_binary(f.recv, a, &e);
        else
            out = ((const EncodingTextMarshalerVT *)f.vt)->marshal_text(f.recv, a, &e);
    } else {
        EncodingAllocArg aa = a;
        EncodingErrorArg ea = &e;
        void *args[2] = {&aa, &ea};
        void *rets[1] = {&out};
        method_call(f.m, f.recv, args, rets);
    }
    BURROW_OUT(err, e);
    return out;
}

static Slice encoding_do_append(Alloc *a, Any v, const EncodingWant *w, Slice b,
                                Error *err) {
    EncodingFound f;
    if (!encoding_find(v, w, &f)) {
        BURROW_OUT(err, encoding_not_implemented(v, w));
        return b;
    }
    Error e = BURROW_NO_ERROR;
    Slice out;
    if (f.vt != NULL) {
        if (w == &want_binary_appender)
            out = ((const EncodingBinaryAppenderVT *)f.vt)
                      ->append_binary(f.recv, a, b, &e);
        else
            out = ((const EncodingTextAppenderVT *)f.vt)->append_text(f.recv, a, b, &e);
    } else {
        EncodingAllocArg aa = a;
        EncodingErrorArg ea = &e;
        void *args[3] = {&aa, &b, &ea};
        void *rets[1] = {&out};
        method_call(f.m, f.recv, args, rets);
    }
    BURROW_OUT(err, e);
    return out;
}

static Error encoding_do_unmarshal(Alloc *a, Any v, const EncodingWant *w, Slice data) {
    EncodingFound f;
    if (!encoding_find(v, w, &f))
        return encoding_not_implemented(v, w);
    if (f.vt != NULL) {
        if (w == &want_binary_unmarshaler)
            return ((const EncodingBinaryUnmarshalerVT *)f.vt)
                ->unmarshal_binary(f.recv, a, data);
        return ((const EncodingTextUnmarshalerVT *)f.vt)
            ->unmarshal_text(f.recv, a, data);
    }
    Error out = BURROW_NO_ERROR;
    EncodingAllocArg aa = a;
    void *args[2] = {&aa, &data};
    void *rets[1] = {&out};
    method_call(f.m, f.recv, args, rets);
    return out;
}

/* ------------------------------------------------------------- the calls */

bool encoding_is_binary_marshaler(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_binary_marshaler, &f);
}

bool encoding_is_binary_unmarshaler(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_binary_unmarshaler, &f);
}

bool encoding_is_binary_appender(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_binary_appender, &f);
}

bool encoding_is_text_marshaler(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_text_marshaler, &f);
}

bool encoding_is_text_unmarshaler(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_text_unmarshaler, &f);
}

bool encoding_is_text_appender(Any v) {
    EncodingFound f;
    return encoding_find(v, &want_text_appender, &f);
}

Slice encoding_marshal_binary(Alloc *a, Any v, Error *err) {
    return encoding_do_marshal(a, v, &want_binary_marshaler, err);
}

Error encoding_unmarshal_binary(Alloc *a, Any v, Slice data) {
    return encoding_do_unmarshal(a, v, &want_binary_unmarshaler, data);
}

Slice encoding_append_binary(Alloc *a, Any v, Slice b, Error *err) {
    return encoding_do_append(a, v, &want_binary_appender, b, err);
}

Slice encoding_marshal_text(Alloc *a, Any v, Error *err) {
    return encoding_do_marshal(a, v, &want_text_marshaler, err);
}

Error encoding_unmarshal_text(Alloc *a, Any v, Slice text) {
    return encoding_do_unmarshal(a, v, &want_text_unmarshaler, text);
}

Slice encoding_append_text(Alloc *a, Any v, Slice b, Error *err) {
    return encoding_do_append(a, v, &want_text_appender, b, err);
}
