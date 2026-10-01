/* Derived from Go's src/encoding/xml/marshal.go, the reflection half.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "xml_internal.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

const Type burrow_type_XmlEncoderArg = {
    {(const Byte *)"*xml.Encoder", 12},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(XmlEncoderArg),
    (uint16_t)_Alignof(XmlEncoderArg),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

#define XM_IFACE_TYPE(cname, gonm)                                                     \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"encoding/xml", 12},                                            \
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

XM_IFACE_TYPE(XmlMarshaler, "Marshaler");
XM_IFACE_TYPE(XmlMarshalerAttr, "MarshalerAttr");

/* What an error made by errors_new holds, which Go would see as an
 * *errors.errorString, a struct with nothing exported. */
static const Type xm_error_string_desc = {
    {(const Byte *)"errorString", 11},
    {(const Byte *)"errors", 6},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* A value for fmt's %T, which reads the type and never the data for the
 * kinds that get here. */
static Any xm_type_arg(const Type *t) {
    static const uint64_t dummy[2] = {0, 0};
    return (Any){t, (void *)(uintptr_t)dummy};
}

/* ------------------------------------------------------ UnsupportedTypeError */

typedef struct XmlUnsupportedTypeErrorBox {
    XmlUnsupportedTypeError e;
    Str message;
} XmlUnsupportedTypeErrorBox;

static Str xml_unsupported_type_error_message(const void *self) {
    return ((const XmlUnsupportedTypeErrorBox *)self)->message;
}

static const Type xml_unsupported_type_error_desc = {
    {(const Byte *)"UnsupportedTypeError", 20},
    {(const Byte *)"encoding/xml", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlUnsupportedTypeError),
    (uint16_t)_Alignof(XmlUnsupportedTypeError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x78757465U, /* "xute" */
    NULL,
};

const Type *const TYPE_XML_UNSUPPORTED_TYPE_ERROR = &xml_unsupported_type_error_desc;

Str xml_unsupported_type_error_error(const XmlUnsupportedTypeError *e, Alloc *a) {
    return fmt_sprintf_v(a, "xml: unsupported type: %T", xm_type_arg(e->type));
}

static Error xml_unsupported_type_error_clone(const void *self, Alloc *a);

static const ErrorVT xml_unsupported_type_error_vt = {
    &xml_unsupported_type_error_desc,
    xml_unsupported_type_error_message,
    NULL,
    NULL,
    NULL,
    NULL,
    xml_unsupported_type_error_clone,
};

static Error xml_unsupported_type_error_new(Alloc *a, const Type *t) {
    XmlUnsupportedTypeError e = {t};
    Str msg = xml_unsupported_type_error_error(&e, a);
    if (msg.p == NULL)
        return burrow_err_out_of_memory;
    XmlUnsupportedTypeErrorBox *b = (XmlUnsupportedTypeErrorBox *)mem_alloc_nozero(
        a, sizeof(XmlUnsupportedTypeErrorBox), _Alignof(XmlUnsupportedTypeErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = e;
    b->message = msg;
    return (Error){&xml_unsupported_type_error_vt, b};
}

static Error xml_unsupported_type_error_clone(const void *self, Alloc *a) {
    return xml_unsupported_type_error_new(
        a, ((const XmlUnsupportedTypeError *)self)->type);
}

/* ------------------------------------------------------------------ values */

static Alloc *xm_scratch(XmlEncoder *e) {
    return arena_allocator(&e->scratch);
}

static bool xm_is_nil(const Type *t, const void *p) {
    if (t == TYPE_ANY)
        return ((const Any *)p)->t == NULL;
    if (t == TYPE_ERROR)
        return ((const Error *)p)->vt == NULL;
    return *(void *const *)p == NULL;
}

/* val.Elem() of an interface: the type it holds, and where the value is, or
 * NULL for a nil one. */
static const Type *xm_iface_elem(const Type *t, void *p, void **vp) {
    if (t == TYPE_ANY) {
        const Any *a = (const Any *)p;
        *vp = a->data;
        return a->t;
    }
    if (t == TYPE_ERROR) {
        const Error *err = (const Error *)p;
        if (err->vt == NULL)
            return NULL;
        if (err->vt->self_type == NULL) {
            *vp = (void *)(uintptr_t)&xm_error_string_desc;
            return &xm_error_string_desc;
        }
        *vp = (void *)(uintptr_t)err->data;
        return err->vt->self_type;
    }
    Iface *v = (Iface *)p;
    if (v->vt == NULL)
        return NULL;
    if (t->size > sizeof(Iface))
        *vp = (Byte *)p + sizeof(void *);
    else
        *vp = v->data;
    return v->vt->self_type;
}

/* One step of Go's loop over pointers and interfaces. False at a nil. */
static bool xm_elem(const Type **tp, void **pp) {
    const Type *t = *tp;
    if (t->kind == KIND_POINTER) {
        void *p = *(void **)*pp;
        if (p == NULL)
            return false;
        *tp = t->elem;
        *pp = p;
        return true;
    }
    void *vp = NULL;
    const Type *et = xm_iface_elem(t, *pp, &vp);
    if (et == NULL)
        return false;
    *tp = et;
    *pp = vp;
    return true;
}

static bool xm_is_ptr_or_iface(const Type *t) {
    return t->kind == KIND_POINTER || t->kind == KIND_INTERFACE;
}

/* indirect: follows pointers and interfaces down to a value, stopping at a
 * nil one. */
static void xm_indirect(const Type **tp, void **pp) {
    while (xm_is_ptr_or_iface(*tp)) {
        if (xm_is_nil(*tp, *pp))
            return;
        xm_elem(tp, pp);
    }
}

static int64_t xm_int(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_INT8:
        return *(const int8_t *)p;
    case KIND_INT16:
        return *(const int16_t *)p;
    case KIND_INT32:
        return *(const int32_t *)p;
    case KIND_INT64:
        return *(const int64_t *)p;
    default:
        return (int64_t)*(const Int *)p;
    }
}

static uint64_t xm_uint(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_UINT8:
        return *(const uint8_t *)p;
    case KIND_UINT16:
        return *(const uint16_t *)p;
    case KIND_UINT32:
        return *(const uint32_t *)p;
    case KIND_UINT64:
        return *(const uint64_t *)p;
    case KIND_UINTPTR:
        return (uint64_t)*(const uintptr_t *)p;
    default:
        return (uint64_t)*(const Uint *)p;
    }
}

static bool xm_is_int(Kind k) {
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64;
}

static bool xm_is_uint(Kind k) {
    return k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 || k == KIND_UINT32 ||
           k == KIND_UINT64 || k == KIND_UINTPTR;
}

static bool xm_is_float(Kind k) {
    return k == KIND_FLOAT32 || k == KIND_FLOAT64;
}

/* A []byte or [N]byte, whatever the element type's name, which is what Go's
 * Elem().Kind() == reflect.Uint8 asks. */
static bool xm_byte_elem(const Type *t) {
    return t->elem != NULL && t->elem->kind == KIND_UINT8;
}

/* The unnamed []byte, which is what a type assertion to []byte matches. */
static bool xm_is_bytes(const Type *t) {
    return t->kind == KIND_SLICE && t->name.len == 0 && xm_byte_elem(t) &&
           t->elem->pkg_path.len == 0;
}

/* The bytes of a []byte or [N]byte. */
static Str xm_bytes_of(const Type *t, void *p) {
    if (t->kind == KIND_ARRAY)
        return str_from_bytes((const Byte *)p, (Int)t->len);
    const Slice *s = (const Slice *)p;
    return str_from_bytes((const Byte *)s->p, s->len);
}

/* A number as decimal digits at the end of buf, which has room for 24. */
static Str xm_fmt_uint(Byte *buf, uint64_t u, bool neg) {
    Int i = 24;
    do {
        buf[--i] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (neg)
        buf[--i] = '-';
    return str_from_bytes(buf + i, 24 - i);
}

static Str xm_fmt_int(Byte *buf, int64_t v) {
    return v < 0 ? xm_fmt_uint(buf, 0 - (uint64_t)v, true)
                 : xm_fmt_uint(buf, (uint64_t)v, false);
}

static Str xm_fmt_float(XmlEncoder *e, const Type *t, const void *p) {
    if (t->kind == KIND_FLOAT32)
        return strconv_format_float(xm_scratch(e), (double)*(const float *)p, 'g', -1,
                                    32);
    return strconv_format_float(xm_scratch(e), *(const double *)p, 'g', -1, 64);
}

/* isEmptyValue. */
/* signbit, done on the bits. mingw's signbit is a macro with a float branch,
 * which warns about a double argument under -Wfloat-conversion. A float
 * widens to a double with its sign, so one function covers both. */
static bool xm_signbit(double x) {
    uint64_t b;
    memcpy(&b, &x, sizeof b);
    return b >> 63 != 0;
}

static bool xm_is_empty_value(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_ARRAY:
        return t->len == 0;
    case KIND_MAP:
        return map_len(*(Map *const *)p) == 0;
    case KIND_SLICE:
        return ((const Slice *)p)->len == 0;
    case KIND_STRING:
        return ((const Str *)p)->len == 0;
    case KIND_BOOL:
        return !*(const bool *)p;
    case KIND_FLOAT32:
        return *(const float *)p == 0 && !xm_signbit((double)*(const float *)p);
    case KIND_FLOAT64:
        return *(const double *)p == 0 && !xm_signbit(*(const double *)p);
    case KIND_INTERFACE:
    case KIND_POINTER:
        return xm_is_nil(t, p);
    default:
        if (xm_is_int(t->kind))
            return xm_int(t, p) == 0;
        if (xm_is_uint(t->kind))
            return xm_uint(t, p) == 0;
        return false;
    }
}

/* ----------------------------------------------------------------- methods */

enum { XM_MARSHAL_XML, XM_MARSHAL_XML_ATTR };

static bool xm_shape_matches(const Method *m, int shape) {
    const Type *f = m->ftype;
    if (f == NULL || m->thunk == NULL || type_num_out(f) != 1)
        return false;
    if (shape == XM_MARSHAL_XML)
        return type_num_in(f) == 2 && type_in(f, 0) == &burrow_type_XmlEncoderArg &&
               type_in(f, 1) == &burrow_type_XmlStartElement &&
               type_out(f, 0) == TYPE_ERROR;
    return type_num_in(f) == 3 && type_in(f, 0) == &burrow_type_EncodingAllocArg &&
           type_in(f, 1) == &burrow_type_XmlName &&
           type_in(f, 2) == &burrow_type_EncodingErrorArg &&
           type_out(f, 0) == &burrow_type_XmlAttr;
}

static const Method *xm_method(const Type *t, int shape) {
    if (t == NULL || t->nmethod == 0)
        return NULL;
    const Method *m =
        type_method_by_name(t, shape == XM_MARSHAL_XML ? BURROW_S("MarshalXML")
                                                       : BURROW_S("MarshalXMLAttr"));
    return m != NULL && xm_shape_matches(m, shape) ? m : NULL;
}

/* The method on the value itself, or when it is a pointer on what that
 * points at, which is where a method with a pointer receiver lives here, or
 * when it is an interface on what that holds. *recv is what to call it on,
 * NULL for a nil pointer as Go passes a nil receiver. */
static const Method *xm_find(const Type *t, void *p, int shape, void **recv,
                             const Type **mt) {
    const Method *m = xm_method(t, shape);
    if (m != NULL) {
        *recv = p;
        *mt = t;
        return m;
    }
    if (t->kind == KIND_POINTER && t->name.len == 0) {
        m = xm_method(t->elem, shape);
        if (m != NULL) {
            *recv = *(void **)p;
            *mt = t->elem;
        }
        return m;
    }
    if (t->kind == KIND_INTERFACE) {
        void *vp = NULL;
        const Type *et = xm_iface_elem(t, p, &vp);
        if (et != NULL && et->kind != KIND_INTERFACE)
            return xm_find(et, vp, shape, recv, mt);
    }
    return NULL;
}

/* MarshalText, into the scratch arena. */
static Str xm_marshal_text(XmlEncoder *e, const Type *t, void *p, Error *err) {
    Slice b = encoding_marshal_text(xm_scratch(e), BURROW_ANY(t, p), err);
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* ------------------------------------------------------------- attributes */

typedef struct XmAttrs {
    XmlAttr *p;
    Int len, cap;
} XmAttrs;

static Error xm_attrs_add(XmlEncoder *e, XmAttrs *as, XmlAttr at) {
    if (as->len == as->cap) {
        Int cap = as->cap == 0 ? 4 : as->cap * 2;
        XmlAttr *np = (XmlAttr *)mem_alloc_nozero(
            xm_scratch(e), (size_t)cap * sizeof(XmlAttr), _Alignof(XmlAttr));
        if (np == NULL)
            return burrow_err_out_of_memory;
        if (as->len > 0)
            memcpy(np, as->p, (size_t)as->len * sizeof(XmlAttr));
        as->p = np;
        as->cap = cap;
    }
    as->p[as->len++] = at;
    return BURROW_NO_ERROR;
}

/* marshalSimple: the text of a value with no structure, and whether it came
 * from bytes, which EscapeText writes rather than EscapeString. */
static Error xm_marshal_simple(XmlEncoder *e, const Type *t, void *p, Byte *buf,
                               Str *out, bool *is_bytes) {
    *is_bytes = false;
    Kind k = t->kind;
    if (xm_is_int(k)) {
        *out = xm_fmt_int(buf, xm_int(t, p));
        return BURROW_NO_ERROR;
    }
    if (xm_is_uint(k)) {
        *out = xm_fmt_uint(buf, xm_uint(t, p), false);
        return BURROW_NO_ERROR;
    }
    if (xm_is_float(k)) {
        *out = xm_fmt_float(e, t, p);
        return out->p == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR;
    }
    switch ((int)k) {
    case KIND_STRING:
        *out = *(const Str *)p;
        return BURROW_NO_ERROR;
    case KIND_BOOL:
        *out = *(const bool *)p ? BURROW_S("true") : BURROW_S("false");
        return BURROW_NO_ERROR;
    case KIND_ARRAY:
    case KIND_SLICE:
        if (!xm_byte_elem(t))
            break;
        *out = xm_bytes_of(t, p);
        *is_bytes = true;
        return BURROW_NO_ERROR;
    default:
        break;
    }
    return xml_unsupported_type_error_new(error_allocator(), t);
}

/* marshalAttr: the attribute or attributes for one field. */
static Error xm_marshal_attr(XmlEncoder *e, XmAttrs *as, XmlName name, const Type *t,
                             void *p) {
    void *recv = NULL;
    const Type *mt = NULL;
    const Method *m = xm_find(t, p, XM_MARSHAL_XML_ATTR, &recv, &mt);
    if (m != NULL) {
        EncodingAllocArg aa = xm_scratch(e);
        Error err = BURROW_NO_ERROR;
        EncodingErrorArg ea = &err;
        XmlAttr at = {{{NULL, 0}, {NULL, 0}}, {NULL, 0}};
        void *args[3] = {(void *)&aa, &name, (void *)&ea};
        void *rets[1] = {&at};
        method_call(m, recv, args, rets);
        if (BURROW_FAILED(err))
            return err;
        if (at.name.local.len > 0)
            return xm_attrs_add(e, as, at);
        return BURROW_NO_ERROR;
    }

    if (encoding_is_text_marshaler(BURROW_ANY(t, p))) {
        Error err = BURROW_NO_ERROR;
        Str text = xm_marshal_text(e, t, p, &err);
        if (BURROW_FAILED(err))
            return err;
        return xm_attrs_add(e, as, (XmlAttr){name, text});
    }

    /* Dereference or skip nil pointer, interface values. */
    if (xm_is_ptr_or_iface(t)) {
        if (!xm_elem(&t, &p))
            return BURROW_NO_ERROR;
    }

    /* Walk slices. */
    if (t->kind == KIND_SLICE && !xm_byte_elem(t)) {
        const Slice *s = (const Slice *)p;
        for (Int i = 0; i < s->len; i++) {
            Error err = xm_marshal_attr(e, as, name, t->elem,
                                        (Byte *)s->p + (size_t)i * t->elem->size);
            if (BURROW_FAILED(err))
                return err;
        }
        return BURROW_NO_ERROR;
    }

    if (t == &burrow_type_XmlAttr)
        return xm_attrs_add(e, as, *(const XmlAttr *)p);

    Byte buf[24];
    Str s = {NULL, 0};
    bool is_bytes = false;
    Error err = xm_marshal_simple(e, t, p, buf, &s, &is_bytes);
    if (BURROW_FAILED(err))
        return err;
    if (s.p == buf + 24 - s.len || is_bytes) {
        /* The digits are on the stack, and bytes may change under us. */
        Byte *c = (Byte *)mem_alloc_nozero(xm_scratch(e), (size_t)s.len + 1, 1);
        if (c == NULL)
            return burrow_err_out_of_memory;
        if (s.len > 0)
            memcpy(c, s.p, (size_t)s.len);
        s = str_from_bytes(c, s.len);
    }
    return xm_attrs_add(e, as, (XmlAttr){name, s});
}

/* ---------------------------------------------------------------- values */

static Error xm_marshal_value(XmlEncoder *e, const Type *t, void *p,
                              const XmlFieldInfo *finfo,
                              const XmlStartElement *start_template);

/* defaultStart. */
static XmlStartElement xm_default_start(const Type *t, const XmlFieldInfo *finfo,
                                        const XmlStartElement *start_template) {
    XmlStartElement start = {{{NULL, 0}, {NULL, 0}},
                             {NULL, 0, 0, &burrow_type_XmlAttr}};
    if (start_template != NULL) {
        start.name = start_template->name;
        start.attr = start_template->attr;
    } else if (finfo != NULL && finfo->name.len > 0) {
        start.name.local = finfo->name;
        start.name.space = finfo->xmlns;
    } else if (t->name.len > 0) {
        start.name.local = t->name;
    } else {
        /* Must be a pointer to a named type, since it has the Marshaler
         * methods. */
        start.name.local = t->elem->name;
    }
    return start;
}

/* marshalInterface. The mark on the tag stack keeps MarshalXML from closing
 * what it didn't open. */
static Error xm_marshal_interface(XmlEncoder *e, const Method *m, void *recv,
                                  const Type *mt, XmlStartElement start) {
    Error err = burrow__xml_enc_push_mark(e);
    if (BURROW_FAILED(err))
        return err;
    Int n = e->ntags;

    XmlEncoderArg ea = e;
    void *args[2] = {(void *)&ea, &start};
    void *rets[1] = {&err};
    err = BURROW_NO_ERROR;
    method_call(m, recv, args, rets);
    if (BURROW_FAILED(err))
        return err;

    /* Make sure MarshalXML closed all its tags. e->tags[n-1] is the mark. */
    if (e->ntags > n) {
        /* Every method here has a pointer receiver, which Go writes as
         * (*xml.T). */
        return fmt_errorf_v("xml: (*%T).MarshalXML wrote invalid XML: <%s> not closed",
                            xm_type_arg(mt),
                            burrow__xml_enc_tag(e, e->ntags - 1).local);
    }
    e->ntags = n - 1;
    e->ntag_bytes = e->tags[n - 1].off;
    return BURROW_NO_ERROR;
}

/* marshalTextInterface. */
static Error xm_marshal_text_interface(XmlEncoder *e, const Type *t, void *p,
                                       XmlStartElement start) {
    Error err = burrow__xml_enc_write_start(e, &start);
    if (BURROW_FAILED(err))
        return err;
    Str text = xm_marshal_text(e, t, p, &err);
    if (BURROW_FAILED(err))
        return err;
    (void)burrow__xml_escape_to(burrow__xml_put_encoder, e, text.p, text.len, true);
    return burrow__xml_enc_write_end(e, start.name);
}

/* emitCDATA. */
static Error xm_emit_cdata(XmlEncoder *e, Str s) {
    if (s.len == 0)
        return BURROW_NO_ERROR;
    burrow__xml_enc_write_str(e, BURROW_S("<![CDATA["));
    Int last = 0;
    for (Int i = 0; i + 3 <= s.len; i++) {
        if (s.p[i] == ']' && s.p[i + 1] == ']' && s.p[i + 2] == '>') {
            /* Escape ]]> by splitting it across two CDATA sections. */
            burrow__xml_enc_write(e, s.p + last, i - last);
            burrow__xml_enc_write_str(e, BURROW_S("]]]]><![CDATA[>"));
            i += 2;
            last = i + 1;
        }
    }
    burrow__xml_enc_write(e, s.p + last, s.len - last);
    burrow__xml_enc_write_str(e, BURROW_S("]]>"));
    return e->err;
}

static Error xm_emit(XmlEncoder *e, bool cdata, Str s) {
    if (cdata)
        return xm_emit_cdata(e, s);
    return burrow__xml_escape_to(burrow__xml_put_encoder, e, s.p, s.len, true);
}

/* parentStack: the elements a>b>c tags have opened around the fields. */
typedef struct XmParents {
    const Str *stack;
    Int len, cap;
} XmParents;

static Error xm_parents_trim(XmlEncoder *e, XmParents *s, const Str *parents,
                             Int nparents) {
    Int split = 0;
    for (; split < nparents && split < s->len; split++)
        if (!str_eq(parents[split], s->stack[split]))
            break;
    for (Int i = s->len - 1; i >= split; i--) {
        Error err = burrow__xml_enc_write_end(e, (XmlName){{NULL, 0}, s->stack[i]});
        if (BURROW_FAILED(err))
            return err;
    }
    s->len = split;
    return BURROW_NO_ERROR;
}

/* push, which only ever adds the rest of one field's parents, so the stack
 * is that field's parents array itself. */
static Error xm_parents_push(XmlEncoder *e, XmParents *s, const Str *parents,
                             Int nparents) {
    for (Int i = s->len; i < nparents; i++) {
        XmlStartElement start = {{{NULL, 0}, parents[i]},
                                 {NULL, 0, 0, &burrow_type_XmlAttr}};
        Error err = burrow__xml_enc_write_start(e, &start);
        if (BURROW_FAILED(err))
            return err;
    }
    s->stack = parents;
    s->len = nparents;
    return BURROW_NO_ERROR;
}

/* marshalStruct. */
static Error xm_marshal_struct(XmlEncoder *e, const XmlTypeInfo *ti, const Type *st,
                               void *sp) {
    XmParents s = {NULL, 0, 0};
    Byte buf[24];
    for (Int i = 0; i < ti->nfields; i++) {
        const XmlFieldInfo *finfo = &ti->fields[i];
        if ((finfo->flags & XF_ATTR) != 0)
            continue;
        const Type *ft = NULL;
        void *fp = burrow__xml_field_value(finfo, st, sp, &ft);
        if (fp == NULL)
            continue;

        Error err;
        switch (finfo->flags & XF_MODE) {
        case XF_CDATA:
        case XF_CHARDATA: {
            bool cdata = (finfo->flags & XF_MODE) == XF_CDATA;
            err = xm_parents_trim(e, &s, finfo->parents, finfo->nparents);
            if (BURROW_FAILED(err))
                return err;
            if (encoding_is_text_marshaler(BURROW_ANY(ft, fp))) {
                Str data = xm_marshal_text(e, ft, fp, &err);
                if (BURROW_FAILED(err))
                    return err;
                err = xm_emit(e, cdata, data);
                if (BURROW_FAILED(err))
                    return err;
                continue;
            }
            xm_indirect(&ft, &fp);
            Str text = {NULL, 0};
            bool have = true;
            if (xm_is_int(ft->kind)) {
                text = xm_fmt_int(buf, xm_int(ft, fp));
            } else if (xm_is_uint(ft->kind)) {
                text = xm_fmt_uint(buf, xm_uint(ft, fp), false);
            } else if (xm_is_float(ft->kind)) {
                text = xm_fmt_float(e, ft, fp);
                if (text.p == NULL)
                    return burrow_err_out_of_memory;
            } else if (ft->kind == KIND_BOOL) {
                text = *(const bool *)fp ? BURROW_S("true") : BURROW_S("false");
            } else if (ft->kind == KIND_STRING) {
                text = *(const Str *)fp;
            } else if (xm_is_bytes(ft)) {
                text = xm_bytes_of(ft, fp);
            } else {
                have = false;
            }
            if (have) {
                err = xm_emit(e, cdata, text);
                if (BURROW_FAILED(err))
                    return err;
            }
            continue;
        }

        case XF_COMMENT: {
            err = xm_parents_trim(e, &s, finfo->parents, finfo->nparents);
            if (BURROW_FAILED(err))
                return err;
            xm_indirect(&ft, &fp);
            Kind k = ft->kind;
            if (!(k == KIND_STRING || (k == KIND_SLICE && xm_byte_elem(ft))))
                return fmt_errorf_v("xml: bad type for comment field of %T",
                                    xm_type_arg(st));
            Str c = k == KIND_STRING ? *(const Str *)fp : xm_bytes_of(ft, fp);
            if (c.len == 0)
                continue;
            burrow__xml_enc_write_indent(e, 0);
            burrow__xml_enc_write_str(e, BURROW_S("<!--"));
            bool dash_dash = false;
            for (Int j = 0; j + 1 < c.len; j++) {
                if (c.p[j] == '-' && c.p[j + 1] == '-') {
                    dash_dash = true;
                    break;
                }
            }
            bool dash_last = c.p[c.len - 1] == '-';
            if (dash_dash)
                return errors_new(error_allocator(),
                                  BURROW_S("xml: comments must not contain \"--\""));
            burrow__xml_enc_write_str(e, c);
            if (dash_last) {
                /* "--->" is invalid grammar. Make it "- -->" */
                burrow__xml_enc_write_byte(e, ' ');
            }
            burrow__xml_enc_write_str(e, BURROW_S("-->"));
            continue;
        }

        case XF_INNERXML:
            xm_indirect(&ft, &fp);
            if (xm_is_bytes(ft)) {
                const Slice *raw = (const Slice *)fp;
                burrow__xml_enc_write(e, (const Byte *)raw->p, raw->len);
                continue;
            }
            if (ft == TYPE_STRING) {
                burrow__xml_enc_write_str(e, *(const Str *)fp);
                continue;
            }
            break;

        case XF_ELEMENT:
        case XF_ELEMENT | XF_ANY:
            err = xm_parents_trim(e, &s, finfo->parents, finfo->nparents);
            if (BURROW_FAILED(err))
                return err;
            if (finfo->nparents > s.len) {
                if (!xm_is_ptr_or_iface(ft) || !xm_is_nil(ft, fp)) {
                    err = xm_parents_push(e, &s, finfo->parents, finfo->nparents);
                    if (BURROW_FAILED(err))
                        return err;
                }
            }
            break;
        default:
            break;
        }
        err = xm_marshal_value(e, ft, fp, finfo, NULL);
        if (BURROW_FAILED(err))
            return err;
    }
    (void)xm_parents_trim(e, &s, NULL, 0);
    return burrow__xml_enc_cached_write_error(e);
}

/* marshalValue. */
static Error xm_marshal_value(XmlEncoder *e, const Type *t, void *p,
                              const XmlFieldInfo *finfo,
                              const XmlStartElement *start_template) {
    if (start_template != NULL && start_template->name.local.len == 0)
        return errors_new(
            error_allocator(),
            BURROW_S("xml: EncodeElement of StartElement with missing name"));

    if (t == NULL)
        return BURROW_NO_ERROR;
    if (finfo != NULL && (finfo->flags & XF_OMITEMPTY) != 0 && xm_is_empty_value(t, p))
        return BURROW_NO_ERROR;

    /* Drill into interfaces and pointers. This can turn into an infinite loop
     * given a cyclic chain, but it matches the Go 1 behavior. */
    while (xm_is_ptr_or_iface(t)) {
        if (!xm_elem(&t, &p))
            return BURROW_NO_ERROR;
    }

    /* Check for marshaler. */
    void *recv = NULL;
    const Type *mt = NULL;
    const Method *m = xm_find(t, p, XM_MARSHAL_XML, &recv, &mt);
    if (m != NULL)
        return xm_marshal_interface(e, m, recv, mt,
                                    xm_default_start(t, finfo, start_template));

    /* Check for text marshaler. */
    if (t->nmethod > 0 && encoding_is_text_marshaler(BURROW_ANY(t, p)))
        return xm_marshal_text_interface(e, t, p,
                                         xm_default_start(t, finfo, start_template));

    /* Slices and arrays iterate over the elements. They do not have an
     * enclosing tag. */
    if ((t->kind == KIND_SLICE || t->kind == KIND_ARRAY) && !xm_byte_elem(t)) {
        Byte *base;
        Int n;
        if (t->kind == KIND_SLICE) {
            base = (Byte *)((const Slice *)p)->p;
            n = ((const Slice *)p)->len;
        } else {
            base = (Byte *)p;
            n = (Int)t->len;
        }
        for (Int i = 0; i < n; i++) {
            Error err = xm_marshal_value(e, t->elem, base + (size_t)i * t->elem->size,
                                         finfo, start_template);
            if (BURROW_FAILED(err))
                return err;
        }
        return BURROW_NO_ERROR;
    }

    Error err = BURROW_NO_ERROR;
    const XmlTypeInfo *ti = burrow__xml_type_info(t, &err);
    if (ti == NULL)
        return err;

    /* Create start element. Precedence for the XML element name is: 0.
     * startTemplate 1. XMLName field in underlying struct; 2. field name/tag
     * in the struct field; and 3. type name */
    XmlStartElement start = {{{NULL, 0}, {NULL, 0}},
                             {NULL, 0, 0, &burrow_type_XmlAttr}};
    XmAttrs as = {NULL, 0, 0};

    if (start_template != NULL) {
        start.name = start_template->name;
        const XmlAttr *ta = (const XmlAttr *)start_template->attr.p;
        for (Int i = 0; i < start_template->attr.len; i++) {
            err = xm_attrs_add(e, &as, ta[i]);
            if (BURROW_FAILED(err))
                return err;
        }
    } else if (ti->xmlname != NULL) {
        const XmlFieldInfo *xmlname = ti->xmlname;
        if (xmlname->name.len > 0) {
            start.name.space = xmlname->xmlns;
            start.name.local = xmlname->name;
        } else {
            const Type *nt = NULL;
            void *np = burrow__xml_field_value(xmlname, t, p, &nt);
            if (np != NULL && nt == &burrow_type_XmlName &&
                ((const XmlName *)np)->local.len > 0)
                start.name = *(const XmlName *)np;
        }
    }
    if (start.name.local.len == 0 && finfo != NULL) {
        start.name.space = finfo->xmlns;
        start.name.local = finfo->name;
    }
    if (start.name.local.len == 0) {
        Str name = t->name;
        for (Int i = 0; i < name.len; i++) {
            if (name.p[i] == '[') {
                name.len = i;
                break;
            }
        }
        if (name.len == 0)
            return xml_unsupported_type_error_new(error_allocator(), t);
        start.name.local = name;
    }

    /* Attributes */
    for (Int i = 0; i < ti->nfields; i++) {
        const XmlFieldInfo *fi = &ti->fields[i];
        if ((fi->flags & XF_ATTR) == 0)
            continue;
        const Type *ft = NULL;
        void *fp = burrow__xml_field_value(fi, t, p, &ft);

        if ((fi->flags & XF_OMITEMPTY) != 0 &&
            (fp == NULL || xm_is_empty_value(ft, fp)))
            continue;
        if (fp == NULL)
            continue;

        if (ft->kind == KIND_INTERFACE && xm_is_nil(ft, fp))
            continue;

        XmlName name = {fi->xmlns, fi->name};
        err = xm_marshal_attr(e, &as, name, ft, fp);
        if (BURROW_FAILED(err))
            return err;
    }

    /* If an empty name was found, namespace is overridden with an empty
     * space. */
    if (ti->xmlname != NULL && start.name.space.len == 0 &&
        ti->xmlname->xmlns.len == 0 && ti->xmlname->name.len == 0 && e->ntags != 0 &&
        burrow__xml_enc_tag(e, e->ntags - 1).space.len > 0) {
        err =
            xm_attrs_add(e, &as, (XmlAttr){{{NULL, 0}, BURROW_S("xmlns")}, {NULL, 0}});
        if (BURROW_FAILED(err))
            return err;
    }
    start.attr = (Slice){as.p, as.len, as.cap, &burrow_type_XmlAttr};
    err = burrow__xml_enc_write_start(e, &start);
    if (BURROW_FAILED(err))
        return err;

    if (t->kind == KIND_STRUCT) {
        err = xm_marshal_struct(e, ti, t, p);
    } else {
        Byte buf[24];
        Str s = {NULL, 0};
        bool is_bytes = false;
        err = xm_marshal_simple(e, t, p, buf, &s, &is_bytes);
        if (BURROW_OK(err))
            (void)burrow__xml_escape_to(burrow__xml_put_encoder, e, s.p, s.len, true);
    }
    if (BURROW_FAILED(err))
        return err;

    err = burrow__xml_enc_write_end(e, start.name);
    if (BURROW_FAILED(err))
        return err;

    return burrow__xml_enc_cached_write_error(e);
}

/* ------------------------------------------------------------ entry points */

/* Encode and EncodeElement. The scratch arena is emptied when the outermost
 * call returns, and not by one a MarshalXML method makes. */
static Error xm_encode(XmlEncoder *e, Any v, const XmlStartElement *start) {
    e->marshal_depth++;
    Error err = xm_marshal_value(e, v.t, v.data, NULL, start);
    if (--e->marshal_depth == 0)
        arena_reset(&e->scratch);
    if (BURROW_FAILED(err))
        return err;
    return bufio_writer_flush(e->w);
}

Error xml_encoder_encode(XmlEncoder *e, Any v) {
    return xm_encode(e, v, NULL);
}

Error xml_encoder_encode_element(XmlEncoder *e, Any v, XmlStartElement start) {
    return xm_encode(e, v, &start);
}

static Slice xm_marshal(Alloc *a, Any v, bool indent, Str prefix, Str ind, Error *err) {
    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    if (a == NULL)
        a = heap_allocator();
    BytesBuffer b = BYTES_BUFFER(a);
    XmlEncoder *e = xml_new_encoder(a, bytes_buffer_as_io_writer(&b));
    if (e == NULL) {
        *err = burrow_err_out_of_memory;
        return nil;
    }
    if (indent)
        xml_encoder_indent(e, prefix, ind);
    Error r = xml_encoder_encode(e, v);
    if (BURROW_OK(r))
        r = xml_encoder_close(e);
    xml_encoder_free(e);
    if (BURROW_FAILED(r)) {
        bytes_buffer_free(&b);
        *err = r;
        return nil;
    }
    return bytes_buffer_bytes(&b);
}

Slice xml_marshal(Alloc *a, Any v, Error *err) {
    return xm_marshal(a, v, false, (Str){NULL, 0}, (Str){NULL, 0}, err);
}

Slice xml_marshal_indent(Alloc *a, Any v, Str prefix, Str indent, Error *err) {
    return xm_marshal(a, v, true, prefix, indent, err);
}
