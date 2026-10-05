/* Derived from Go's src/encoding/xml/read.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "xml_internal.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <string.h>

/* ------------------------------------------------------------- descriptors */

const Type burrow_type_XmlDecoderArg = {
    {(const Byte *)"*xml.Decoder", 12},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(XmlDecoderArg),
    (uint16_t)_Alignof(XmlDecoderArg),
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

#define XU_IFACE_TYPE(cname, gonm)                                                     \
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

XU_IFACE_TYPE(XmlUnmarshaler, "Unmarshaler");
XU_IFACE_TYPE(XmlUnmarshalerAttr, "UnmarshalerAttr");

/* A value for fmt's %T, which reads the type and never the data. */
static Any xu_type_arg(const Type *t) {
    static const uint64_t dummy[2] = {0, 0};
    return (Any){t, (void *)(uintptr_t)dummy};
}

/* msg followed by the type's name as Go's reflect prints it. fmt's %T
 * would look inside an interface for what it holds, so an interface type
 * is spelled out here. */
static Error xu_type_error(const char *msg, const Type *t) {
    if (t->kind == KIND_INTERFACE) {
        Str name = t->name.len > 0 ? t->name : BURROW_S("interface {}");
        return fmt_errorf_v("%s%s", str_from_cstr(msg), name);
    }
    return fmt_errorf_v("%s%T", str_from_cstr(msg), xu_type_arg(t));
}

BURROW_SENTINEL_ERROR(burrow__xml_err_unmarshal_depth, "exceeded max depth");

/* Go's maxUnmarshalDepth, and the lower one it keeps for wasm, where the
 * engine's stack runs out first (go.dev/issue/56498). */
#if defined(BURROW_ARCH_WASM)
enum { XU_MAX_DEPTH = 5000 };
#else
enum { XU_MAX_DEPTH = 10000 };
#endif

/* ---------------------------------------------------------- UnmarshalError */

static Str xml_unmarshal_error_message(const void *self) {
    return *(const Str *)self;
}

static const Type xml_unmarshal_error_desc = {
    {(const Byte *)"UnmarshalError", 14},
    {(const Byte *)"encoding/xml", 12},
    KIND_STRING,
    (uint32_t)sizeof(XmlUnmarshalError),
    (uint16_t)_Alignof(XmlUnmarshalError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x78756d65U, /* "xume" */
    NULL,
};

const Type *const TYPE_XML_UNMARSHAL_ERROR = &xml_unmarshal_error_desc;

Str xml_unmarshal_error_error(XmlUnmarshalError e, Alloc *a) {
    return str_clone(a, e);
}

static Error xml_unmarshal_error_clone(const void *self, Alloc *a);

static const ErrorVT xml_unmarshal_error_vt = {
    &xml_unmarshal_error_desc, xml_unmarshal_error_message, NULL, NULL, NULL, NULL,
    xml_unmarshal_error_clone,
};

/* The Str and then its bytes, in one block. */
static Error xml_unmarshal_error_new(Alloc *a, Str msg) {
    Str *b = (Str *)mem_alloc_nozero(a, sizeof(Str) + (size_t)msg.len, _Alignof(Str));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (msg.len > 0)
        memcpy(p, msg.p, (size_t)msg.len);
    *b = str_from_bytes(p, msg.len);
    return (Error){&xml_unmarshal_error_vt, b};
}

static Error xml_unmarshal_error_clone(const void *self, Alloc *a) {
    return xml_unmarshal_error_new(a, *(const Str *)self);
}

/* UnmarshalError(a + b + c + d + e), with the pieces joined in the error
 * arena. */
static Error xu_unmarshal_error(const Str *parts, int n) {
    Int len = 0;
    for (int i = 0; i < n; i++)
        len += parts[i].len;
    Alloc *a = error_allocator();
    Str *b = (Str *)mem_alloc_nozero(a, sizeof(Str) + (size_t)len, _Alignof(Str));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    Int at = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i].len > 0)
            memcpy(p + at, parts[i].p, (size_t)parts[i].len);
        at += parts[i].len;
    }
    *b = str_from_bytes(p, len);
    return (Error){&xml_unmarshal_error_vt, b};
}

/* ------------------------------------------------------------------ values */

static Alloc *xu_scratch(XmlDecoder *d) {
    return arena_allocator(&d->scratch);
}

/* Bytes gathered across tokens, in the scratch arena. */
typedef struct XuBuf {
    Byte *p;
    Int len, cap;
} XuBuf;

static bool xu_buf_add(XmlDecoder *d, XuBuf *b, Slice s) {
    if (s.len == 0)
        return true;
    if (b->p == NULL || b->len + s.len > b->cap) {
        Int cap = b->cap * 2;
        if (cap < b->len + s.len)
            cap = b->len + s.len;
        if (cap < 64)
            cap = 64;
        Byte *np = (Byte *)mem_alloc_nozero(xu_scratch(d), (size_t)cap, 1);
        if (np == NULL)
            return false;
        if (b->len > 0)
            memcpy(np, b->p, (size_t)b->len);
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s.p, (size_t)s.len);
    b->len += s.len;
    return true;
}

static Str xu_buf_str(const XuBuf *b) {
    return str_from_bytes(b->p, b->len);
}

/* What a nil pointer of type t at p is set to: a new zero value. Returns
 * what the pointer points at, or NULL when the allocation fails. */
static void *xu_deref(XmlDecoder *d, const Type *t, void *p) {
    void **pp = (void **)p;
    if (*pp == NULL) {
        size_t size = t->elem->size > 0 ? t->elem->size : 1;
        void *np = mem_alloc(d->a, size, t->elem->align > 0 ? t->elem->align : 1);
        if (np == NULL)
            return NULL;
        *pp = np;
    }
    return *pp;
}

/* finfo.value(v, initNilPointers). NULL when an allocation fails. */
static void *xu_field(XmlDecoder *d, const XmlFieldInfo *f, const Type *t, void *p,
                      const Type **ft) {
    for (Int i = 0; i < f->nidx; i++) {
        if (i > 0 && t->kind == KIND_POINTER && t->elem->kind == KIND_STRUCT) {
            p = xu_deref(d, t, p);
            if (p == NULL)
                return NULL;
            t = t->elem;
        }
        const Field *fd = &t->fields[f->idx[i]];
        p = (Byte *)p + fd->offset;
        t = fd->type;
    }
    *ft = t;
    return p;
}

/* Grows the slice at s by one element and returns where it is. As with
 * Go's Grow and SetLen, spare capacity is reused as it is. */
static void *xu_slice_grow(XmlDecoder *d, const Type *t, Slice *s) {
    s->elem = t->elem;
    if (s->len == s->cap) {
        Slice g = slice_append(d->a, *s, NULL, 1);
        if (g.len != s->len + 1)
            return NULL;
        *s = g;
    } else {
        s->len++;
    }
    return (Byte *)s->p + (size_t)(s->len - 1) * t->elem->size;
}

static Error xu_str_set(XmlDecoder *d, Str *dst, Str src) {
    if (src.len == 0) {
        *dst = (Str){NULL, 0};
        return BURROW_NO_ERROR;
    }
    Str c = str_clone(d->a, src);
    if (c.p == NULL)
        return burrow_err_out_of_memory;
    *dst = c;
    return BURROW_NO_ERROR;
}

/* A []byte holding a copy of src, never the nil slice. */
static Error xu_bytes_set(XmlDecoder *d, const Type *t, Slice *dst, Str src) {
    Slice s = slice_make(d->a, t->elem, src.len, src.len);
    if (s.p == NULL)
        return burrow_err_out_of_memory;
    if (src.len > 0)
        memcpy(s.p, src.p, (size_t)src.len);
    *dst = s;
    return BURROW_NO_ERROR;
}

static Error xu_name_set(XmlDecoder *d, XmlName *dst, XmlName src) {
    Error err = xu_str_set(d, &dst->space, src.space);
    if (BURROW_OK(err))
        err = xu_str_set(d, &dst->local, src.local);
    return err;
}

static void xu_set_int(const Type *t, void *p, int64_t v) {
    switch ((int)t->kind) {
    case KIND_INT8:
        *(int8_t *)p = (int8_t)v;
        break;
    case KIND_INT16:
        *(int16_t *)p = (int16_t)v;
        break;
    case KIND_INT32:
        *(int32_t *)p = (int32_t)v;
        break;
    case KIND_INT64:
        *(int64_t *)p = v;
        break;
    default:
        *(Int *)p = (Int)v;
        break;
    }
}

static void xu_set_uint(const Type *t, void *p, uint64_t v) {
    switch ((int)t->kind) {
    case KIND_UINT8:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case KIND_UINT16:
        *(uint16_t *)p = (uint16_t)v;
        break;
    case KIND_UINT32:
        *(uint32_t *)p = (uint32_t)v;
        break;
    case KIND_UINT64:
        *(uint64_t *)p = v;
        break;
    case KIND_UINTPTR:
        *(uintptr_t *)p = (uintptr_t)v;
        break;
    default:
        *(Uint *)p = (Uint)v;
        break;
    }
}

static bool xu_is_int(Kind k) {
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64;
}

static bool xu_is_uint(Kind k) {
    return k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 || k == KIND_UINT32 ||
           k == KIND_UINT64 || k == KIND_UINTPTR;
}

static bool xu_byte_elem(const Type *t) {
    return t->elem != NULL && t->elem->kind == KIND_UINT8;
}

/* copyValue. A NULL t is Go's invalid Value, where there is nowhere to put
 * the text. */
static Error xu_copy_value(XmlDecoder *d, const Type *t, void *p, Str src) {
    const Type *t0 = t;
    if (t == NULL)
        return BURROW_NO_ERROR;
    if (t->kind == KIND_POINTER) {
        p = xu_deref(d, t, p);
        if (p == NULL)
            return burrow_err_out_of_memory;
        t = t->elem;
    }

    /* Save accumulated data. */
    Error err = BURROW_NO_ERROR;
    Kind k = t->kind;
    Int bits = (Int)t->size * 8;
    if (xu_is_int(k)) {
        if (src.len == 0) {
            xu_set_int(t, p, 0);
            return BURROW_NO_ERROR;
        }
        int64_t v = strconv_parse_int(strings_trim_space(src), 10, bits, &err);
        if (BURROW_FAILED(err))
            return err;
        xu_set_int(t, p, v);
        return BURROW_NO_ERROR;
    }
    if (xu_is_uint(k)) {
        if (src.len == 0) {
            xu_set_uint(t, p, 0);
            return BURROW_NO_ERROR;
        }
        uint64_t v = strconv_parse_uint(strings_trim_space(src), 10, bits, &err);
        if (BURROW_FAILED(err))
            return err;
        xu_set_uint(t, p, v);
        return BURROW_NO_ERROR;
    }
    switch ((int)k) {
    case KIND_FLOAT32:
    case KIND_FLOAT64: {
        double v = 0;
        if (src.len > 0) {
            v = strconv_parse_float(strings_trim_space(src), bits, &err);
            if (BURROW_FAILED(err))
                return err;
        }
        if (k == KIND_FLOAT32)
            *(float *)p = (float)v;
        else
            *(double *)p = v;
        return BURROW_NO_ERROR;
    }
    case KIND_BOOL: {
        bool v = false;
        if (src.len > 0) {
            v = strconv_parse_bool(strings_trim_space(src), &err);
            if (BURROW_FAILED(err))
                return err;
        }
        *(bool *)p = v;
        return BURROW_NO_ERROR;
    }
    case KIND_STRING:
        return xu_str_set(d, (Str *)p, src);
    case KIND_SLICE:
        /* Go's SetBytes, which only a []byte gets past. The copy is never
         * the nil slice, to flag that the element was there. */
        if (xu_byte_elem(t))
            return xu_bytes_set(d, t, (Slice *)p, src);
        break;
    default:
        break;
    }
    return xu_type_error("cannot unmarshal into ", t0);
}

/* ----------------------------------------------------------------- methods */

enum { XU_UNMARSHAL_XML, XU_UNMARSHAL_XML_ATTR };

static const Method *xu_method(const Type *t, int shape) {
    if (t == NULL || t->nmethod == 0)
        return NULL;
    const Method *m = type_method_by_name(t, shape == XU_UNMARSHAL_XML
                                                 ? BURROW_S("UnmarshalXML")
                                                 : BURROW_S("UnmarshalXMLAttr"));
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 2 || type_num_out(f) != 1 || type_out(f, 0) != TYPE_ERROR)
        return NULL;
    if (shape == XU_UNMARSHAL_XML)
        return type_in(f, 0) == &burrow_type_XmlDecoderArg &&
                       type_in(f, 1) == &burrow_type_XmlStartElement
                   ? m
                   : NULL;
    return type_in(f, 0) == &burrow_type_EncodingAllocArg &&
                   type_in(f, 1) == &burrow_type_XmlAttr
               ? m
               : NULL;
}

/* What an interface value holds: its type and where the value is, or NULL
 * for a nil one. An error made by errors_new counts as nil here, since Go's
 * *errors.errorString has nothing Unmarshal could set. */
static const Type *xu_iface_elem(const Type *t, void *p, void **vp) {
    if (t == TYPE_ANY) {
        const Any *a = (const Any *)p;
        *vp = a->data;
        return a->data != NULL ? a->t : NULL;
    }
    if (t == TYPE_ERROR) {
        const Error *err = (const Error *)p;
        if (err->vt == NULL || err->vt->self_type == NULL || err->data == NULL)
            return NULL;
        *vp = (void *)(uintptr_t)err->data;
        return err->vt->self_type;
    }
    Iface *v = (Iface *)p;
    if (v->vt == NULL)
        return NULL;
    if (t->size > sizeof(Iface))
        *vp = burrow__iface_inline(t, p);
    else
        *vp = v->data;
    return v->vt->self_type;
}

/* Whether an interface value holds a non-nil pointer, which Go loads so
 * that the value it points at can be set. The C interfaces other than Any
 * hold their value by pointer, so they count when they are not nil. */
static bool xu_iface_ptr(const Type *t, void *p, const Type **et, void **ep) {
    void *vp = NULL;
    const Type *vt = xu_iface_elem(t, p, &vp);
    if (vt == NULL)
        return false;
    if (t == TYPE_ANY) {
        if (vt->kind != KIND_POINTER || *(void **)vp == NULL)
            return false;
        *et = vt->elem;
        *ep = *(void **)vp;
        return true;
    }
    if (t != TYPE_ERROR && t->size > sizeof(Iface))
        return false;
    *et = vt;
    *ep = vp;
    return true;
}

/* unmarshalInterface. The EOF record under start's element stops Token at
 * its end tag, so the method can't read past it. */
static Error xu_unmarshal_interface(XmlDecoder *d, const Method *m, void *recv,
                                    const Type *mt, const XmlStartElement *start) {
    /* start points into the decoder, which the method is about to read
     * from, and Go's is a copy. */
    XmlStartElement s = xml_start_element_copy(*start, xu_scratch(d));
    if (s.name.local.len != start->name.local.len)
        return burrow_err_out_of_memory;
    if (!burrow__xml_dec_push_eof(d))
        return burrow_err_out_of_memory;

    bool saved = d->in_unmarshal_xml;
    d->in_unmarshal_xml = true;
    XmlDecoderArg da = d;
    Error err = BURROW_NO_ERROR;
    void *args[2] = {(void *)&da, &s};
    void *rets[1] = {&err};
    method_call(m, recv, args, rets);
    d->in_unmarshal_xml = saved;
    if (BURROW_FAILED(err)) {
        (void)burrow__xml_dec_pop_eof(d);
        return err;
    }

    if (!burrow__xml_dec_pop_eof(d)) {
        /* Every method here has a pointer receiver, which Go writes as
         * (*xml.T). */
        return fmt_errorf_v(
            "xml: (*%T).UnmarshalXML did not consume entire <%s> element",
            xu_type_arg(mt), s.name.local);
    }
    return BURROW_NO_ERROR;
}

static Slice xu_text(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

/* unmarshalTextInterface. The text directly inside the element, and not in
 * its children, goes to UnmarshalText. */
static Error xu_unmarshal_text_interface(XmlDecoder *d, const Type *t, void *p) {
    XuBuf buf = {NULL, 0, 0};
    int64_t depth = 1;
    while (depth > 0) {
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        if (tok.kind == XML_CHAR_DATA) {
            if (depth == 1 && !xu_buf_add(d, &buf, tok.char_data))
                return burrow_err_out_of_memory;
        } else if (tok.kind == XML_START_ELEMENT) {
            depth++;
        } else if (tok.kind == XML_END_ELEMENT) {
            depth--;
        }
    }
    return encoding_unmarshal_text(d->a, BURROW_ANY(t, p), xu_text(xu_buf_str(&buf)));
}

/* unmarshalAttr. */
static Error xu_unmarshal_attr(XmlDecoder *d, const Type *t, void *p, XmlAttr attr) {
    if (t->kind == KIND_POINTER) {
        p = xu_deref(d, t, p);
        if (p == NULL)
            return burrow_err_out_of_memory;
        t = t->elem;
    }
    const Method *m = xu_method(t, XU_UNMARSHAL_XML_ATTR);
    if (m != NULL) {
        EncodingAllocArg aa = d->a;
        Error err = BURROW_NO_ERROR;
        void *args[2] = {(void *)&aa, &attr};
        void *rets[1] = {&err};
        method_call(m, p, args, rets);
        return err;
    }

    /* Not an UnmarshalerAttr; try encoding.TextUnmarshaler. */
    if (encoding_is_text_unmarshaler(BURROW_ANY(t, p)))
        return encoding_unmarshal_text(d->a, BURROW_ANY(t, p), xu_text(attr.value));

    if (t->kind == KIND_SLICE && !xu_byte_elem(t)) {
        /* Slice of element values. Grow slice. */
        Slice *s = (Slice *)p;
        Int n = s->len;
        void *ep = xu_slice_grow(d, t, s);
        if (ep == NULL)
            return burrow_err_out_of_memory;
        /* Recur to read element into slice. */
        Error err = xu_unmarshal_attr(d, t->elem, ep, attr);
        if (BURROW_FAILED(err))
            s->len = n;
        return err;
    }

    if (t == &burrow_type_XmlAttr) {
        XmlAttr *dst = (XmlAttr *)p;
        Error err = xu_name_set(d, &dst->name, attr.name);
        if (BURROW_OK(err))
            err = xu_str_set(d, &dst->value, attr.value);
        return err;
    }

    return xu_copy_value(d, t, p, attr.value);
}

/* ------------------------------------------------------------------ values */

static Error xu_unmarshal(XmlDecoder *d, const Type *t, void *p,
                          const XmlStartElement *start);

/* unmarshalPath. */
static Error xu_unmarshal_path(XmlDecoder *d, const XmlTypeInfo *ti, const Type *st,
                               void *sp, const Str *parents, Int nparents,
                               const XmlStartElement *start, bool *consumed) {
    bool recurse = false;
    *consumed = false;
    for (Int i = 0; i < ti->nfields; i++) {
        const XmlFieldInfo *finfo = &ti->fields[i];
        if ((finfo->flags & XF_ELEMENT) == 0 || finfo->nparents < nparents ||
            (finfo->xmlns.len > 0 && !str_eq(finfo->xmlns, start->name.space)))
            continue;
        bool match = true;
        for (Int j = 0; j < nparents; j++) {
            if (!str_eq(parents[j], finfo->parents[j])) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        if (finfo->nparents == nparents && str_eq(finfo->name, start->name.local)) {
            /* It's a perfect match, unmarshal the field. */
            *consumed = true;
            const Type *ft = NULL;
            void *fp = xu_field(d, finfo, st, sp, &ft);
            if (fp == NULL)
                return burrow_err_out_of_memory;
            return xu_unmarshal(d, ft, fp, start);
        }
        if (finfo->nparents > nparents &&
            str_eq(finfo->parents[nparents], start->name.local)) {
            /* It's a prefix for the field. Break and recurse since it's not
             * ok for one field path to be itself the prefix for another
             * field path. */
            recurse = true;
            parents = finfo->parents;
            nparents++;
            break;
        }
    }
    if (!recurse) {
        /* We have no business with this element. */
        return BURROW_NO_ERROR;
    }
    /* The element is not a perfect match for any field, but one or more
     * fields have the path to this element as a parent prefix. Recurse and
     * attempt to match these. */
    *consumed = true;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        if (tok.kind == XML_START_ELEMENT) {
            bool consumed2 = false;
            err = xu_unmarshal_path(d, ti, st, sp, parents, nparents, &tok.start,
                                    &consumed2);
            if (BURROW_FAILED(err))
                return err;
            if (!consumed2) {
                err = xml_decoder_skip(d);
                if (BURROW_FAILED(err))
                    return err;
            }
        } else if (tok.kind == XML_END_ELEMENT) {
            return BURROW_NO_ERROR;
        }
    }
}

/* Go's savedOffset. */
static Int xu_saved_offset(const XmlDecoder *d) {
    Int n = d->ninner;
    if (d->next_byte >= 0)
        n--;
    return n;
}

/* An interface that holds no pointer. Go asks what it holds for the
 * methods, and calls one on a copy, and otherwise skips the element. */
static Error xu_unmarshal_held(XmlDecoder *d, const Type *t, void *p,
                               const XmlStartElement *start) {
    void *vp = NULL;
    const Type *et = xu_iface_elem(t, p, &vp);
    if (et != NULL && et->kind != KIND_POINTER && et->kind != KIND_INTERFACE &&
        et->nmethod > 0) {
        const Method *m = xu_method(et, XU_UNMARSHAL_XML);
        bool text = m == NULL && encoding_is_text_unmarshaler(BURROW_ANY(et, vp));
        if (m != NULL || text) {
            void *c = mem_alloc(xu_scratch(d), et->size > 0 ? et->size : 1,
                                et->align > 0 ? et->align : 1);
            if (c == NULL)
                return burrow_err_out_of_memory;
            memcpy(c, vp, et->size);
            if (m != NULL)
                return xu_unmarshal_interface(d, m, c, et, start);
            return xu_unmarshal_text_interface(d, et, c);
        }
    }
    /* TODO: For now, simply ignore the field. In the near future we may
     * choose to unmarshal the start element on it, if not nil. */
    return xml_decoder_skip(d);
}

/* Where the struct case of unmarshal keeps what the element's contents go
 * into. A NULL type is Go's invalid Value. */
typedef struct XuSave {
    const Type *data_t;
    void *data_p;
    const Type *comment_t;
    void *comment_p;
    const Type *xml_t;
    void *xml_p;
    Int xml_index;
    const Type *any_t;
    void *any_p;
} XuSave;

/* The struct case: the name check, the attributes, and which fields get the
 * text, the comments, the inner XML and the elements nothing else wants. */
static Error xu_struct_start(XmlDecoder *d, const XmlTypeInfo *ti, const Type *t,
                             void *p, const XmlStartElement *start, XuSave *sv) {
    /* Validate and assign element name. */
    if (ti->xmlname != NULL) {
        const XmlFieldInfo *finfo = ti->xmlname;
        if (finfo->name.len > 0 && !str_eq(finfo->name, start->name.local)) {
            Str parts[] = {BURROW_S("expected element type <"), finfo->name,
                           BURROW_S("> but have <"), start->name.local, BURROW_S(">")};
            return xu_unmarshal_error(parts, 5);
        }
        if (finfo->xmlns.len > 0 && !str_eq(finfo->xmlns, start->name.space)) {
            Str parts[] = {BURROW_S("expected element <"),
                           finfo->name,
                           BURROW_S("> in name space "),
                           finfo->xmlns,
                           BURROW_S(" but have "),
                           start->name.space.len == 0 ? BURROW_S("no name space")
                                                      : start->name.space};
            return xu_unmarshal_error(parts, 6);
        }
        const Type *ft = NULL;
        void *fp = xu_field(d, finfo, t, p, &ft);
        if (fp == NULL)
            return burrow_err_out_of_memory;
        if (ft == &burrow_type_XmlName) {
            Error err = xu_name_set(d, (XmlName *)fp, start->name);
            if (BURROW_FAILED(err))
                return err;
        }
    }

    /* Assign attributes. */
    const XmlAttr *attrs = (const XmlAttr *)start->attr.p;
    for (Int ai = 0; ai < start->attr.len; ai++) {
        XmlAttr a = attrs[ai];
        bool handled = false;
        Int any = -1;
        for (Int i = 0; i < ti->nfields; i++) {
            const XmlFieldInfo *finfo = &ti->fields[i];
            switch (finfo->flags & XF_MODE) {
            case XF_ATTR: {
                const Type *ft = NULL;
                void *fp = xu_field(d, finfo, t, p, &ft);
                if (fp == NULL)
                    return burrow_err_out_of_memory;
                if (str_eq(a.name.local, finfo->name) &&
                    (finfo->xmlns.len == 0 || str_eq(finfo->xmlns, a.name.space))) {
                    Error err = xu_unmarshal_attr(d, ft, fp, a);
                    if (BURROW_FAILED(err))
                        return err;
                    handled = true;
                }
                break;
            }
            case XF_ANY | XF_ATTR:
                if (any == -1)
                    any = i;
                break;
            default:
                break;
            }
        }
        if (!handled && any >= 0) {
            const Type *ft = NULL;
            void *fp = xu_field(d, &ti->fields[any], t, p, &ft);
            if (fp == NULL)
                return burrow_err_out_of_memory;
            Error err = xu_unmarshal_attr(d, ft, fp, a);
            if (BURROW_FAILED(err))
                return err;
        }
    }

    /* Determine whether we need to save character data or comments. */
    for (Int i = 0; i < ti->nfields; i++) {
        const XmlFieldInfo *finfo = &ti->fields[i];
        const Type **wt = NULL;
        void **wp = NULL;
        switch (finfo->flags & XF_MODE) {
        case XF_CDATA:
        case XF_CHARDATA:
            wt = &sv->data_t;
            wp = &sv->data_p;
            break;
        case XF_COMMENT:
            wt = &sv->comment_t;
            wp = &sv->comment_p;
            break;
        case XF_ANY:
        case XF_ANY | XF_ELEMENT:
            wt = &sv->any_t;
            wp = &sv->any_p;
            break;
        case XF_INNERXML:
            wt = &sv->xml_t;
            wp = &sv->xml_p;
            break;
        default:
            break;
        }
        if (wt == NULL || *wt != NULL)
            continue;
        *wp = xu_field(d, finfo, t, p, wt);
        if (*wp == NULL)
            return burrow_err_out_of_memory;
        if ((finfo->flags & XF_MODE) == XF_INNERXML) {
            if (!d->keep_inner) {
                sv->xml_index = 0;
                d->keep_inner = true;
                d->ninner = 0;
            } else {
                sv->xml_index = xu_saved_offset(d);
            }
        }
    }
    return BURROW_NO_ERROR;
}

/* What is left once the end tag is read: the text, the comments and the
 * inner XML go into their fields. */
static Error xu_finish(XmlDecoder *d, XuSave *sv, Str data, Str comment, Str inner) {
    const Type *st = sv->data_t;
    void *sp = sv->data_p;
    if (st != NULL) {
        if (st->kind == KIND_POINTER && *(void **)sp == NULL &&
            encoding_is_text_unmarshaler(BURROW_ANY(st, sp)) &&
            xu_deref(d, st, sp) == NULL)
            return burrow_err_out_of_memory;
        if (encoding_is_text_unmarshaler(BURROW_ANY(st, sp))) {
            Error err =
                encoding_unmarshal_text(d->a, BURROW_ANY(st, sp), xu_text(data));
            if (BURROW_FAILED(err))
                return err;
            st = NULL;
        }
    }
    Error err = xu_copy_value(d, st, sp, data);
    if (BURROW_FAILED(err))
        return err;

    if (sv->comment_t != NULL) {
        const Type *ct = sv->comment_t;
        if (ct->kind == KIND_STRING) {
            err = xu_str_set(d, (Str *)sv->comment_p, comment);
        } else if (ct->kind == KIND_SLICE && xu_byte_elem(ct)) {
            /* Go's comment slice is nil until some text is appended. */
            if (comment.len == 0)
                *(Slice *)sv->comment_p = (Slice){NULL, 0, 0, ct->elem};
            else
                err = xu_bytes_set(d, ct, (Slice *)sv->comment_p, comment);
        }
        if (BURROW_FAILED(err))
            return err;
    }

    if (sv->xml_t != NULL) {
        const Type *xt = sv->xml_t;
        if (xt->kind == KIND_STRING)
            err = xu_str_set(d, (Str *)sv->xml_p, inner);
        else if (xt->kind == KIND_SLICE && xu_byte_elem(xt))
            err = xu_bytes_set(d, xt, (Slice *)sv->xml_p, inner);
    }
    return err;
}

/* Unmarshal a single XML element into the value of type t at p. */
static Error xu_unmarshal(XmlDecoder *d, const Type *t, void *p,
                          const XmlStartElement *start) {
    if (d->stk_depth > XU_MAX_DEPTH)
        return burrow__xml_err_unmarshal_depth;
    /* Find start element if we need it. */
    XmlToken first;
    if (start == NULL) {
        for (;;) {
            Error err = BURROW_NO_ERROR;
            first = xml_decoder_token(d, &err);
            if (BURROW_FAILED(err))
                return err;
            if (first.kind == XML_START_ELEMENT) {
                start = &first.start;
                break;
            }
        }
    }

    /* Load value from interface, but only if the result will be usefully
     * addressable. */
    if (t->kind == KIND_INTERFACE) {
        const Type *et = NULL;
        void *ep = NULL;
        if (!xu_iface_ptr(t, p, &et, &ep))
            return xu_unmarshal_held(d, t, p, start);
        t = et;
        p = ep;
    } else if (t->kind == KIND_POINTER) {
        p = xu_deref(d, t, p);
        if (p == NULL)
            return burrow_err_out_of_memory;
        t = t->elem;
    }

    if (t->nmethod > 0) {
        const Method *m = xu_method(t, XU_UNMARSHAL_XML);
        if (m != NULL)
            return xu_unmarshal_interface(d, m, p, t, start);
    }
    if (t->nmethod > 0 && encoding_is_text_unmarshaler(BURROW_ANY(t, p)))
        return xu_unmarshal_text_interface(d, t, p);

    XuSave sv = {NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL};
    const XmlTypeInfo *ti = NULL;
    Kind k = t->kind;
    if (k == KIND_SLICE) {
        if (xu_byte_elem(t)) {
            sv.data_t = t;
            sv.data_p = p;
        } else {
            /* Slice of element values. Grow slice. */
            Slice *s = (Slice *)p;
            Int n = s->len;
            void *ep = xu_slice_grow(d, t, s);
            if (ep == NULL)
                return burrow_err_out_of_memory;
            /* Recur to read element into slice. */
            Error err = xu_unmarshal(d, t->elem, ep, start);
            if (BURROW_FAILED(err))
                s->len = n;
            return err;
        }
    } else if (k == KIND_BOOL || k == KIND_FLOAT32 || k == KIND_FLOAT64 ||
               k == KIND_STRING || xu_is_int(k) || xu_is_uint(k)) {
        sv.data_t = t;
        sv.data_p = p;
    } else if (k == KIND_STRUCT) {
        if (t == &burrow_type_XmlName) {
            Error err = xu_name_set(d, (XmlName *)p, start->name);
            if (BURROW_FAILED(err))
                return err;
        } else {
            Error err = BURROW_NO_ERROR;
            ti = burrow__xml_type_info(t, &err);
            if (ti == NULL)
                return err;
            err = xu_struct_start(d, ti, t, p, start, &sv);
            if (BURROW_FAILED(err))
                return err;
        }
    } else if (k == KIND_INTERFACE) {
        /* A nil error, or one Go would see as an *errors.errorString. */
        return xml_decoder_skip(d);
    } else {
        return xu_type_error("unknown type ", t);
    }

    /* Find end element. Process sub-elements along the way. */
    XuBuf data = {NULL, 0, 0};
    XuBuf comment = {NULL, 0, 0};
    Str inner = {NULL, 0};
    for (;;) {
        Int saved_offset = 0;
        if (sv.xml_t != NULL)
            saved_offset = xu_saved_offset(d);
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        if (tok.kind == XML_START_ELEMENT) {
            bool consumed = false;
            if (ti != NULL) {
                err = xu_unmarshal_path(d, ti, t, p, NULL, 0, &tok.start, &consumed);
                if (BURROW_FAILED(err))
                    return err;
                if (!consumed && sv.any_t != NULL) {
                    consumed = true;
                    err = xu_unmarshal(d, sv.any_t, sv.any_p, &tok.start);
                    if (BURROW_FAILED(err))
                        return err;
                }
            }
            if (!consumed) {
                err = xml_decoder_skip(d);
                if (BURROW_FAILED(err))
                    return err;
            }
        } else if (tok.kind == XML_END_ELEMENT) {
            if (sv.xml_t != NULL) {
                inner = str_from_bytes(d->inner + sv.xml_index,
                                       saved_offset - sv.xml_index);
                if (sv.xml_index == 0)
                    d->keep_inner = false;
            }
            break;
        } else if (tok.kind == XML_CHAR_DATA) {
            if (sv.data_t != NULL && !xu_buf_add(d, &data, tok.char_data))
                return burrow_err_out_of_memory;
        } else if (tok.kind == XML_COMMENT) {
            if (sv.comment_t != NULL && !xu_buf_add(d, &comment, tok.comment))
                return burrow_err_out_of_memory;
        }
    }
    return xu_finish(d, &sv, xu_buf_str(&data), xu_buf_str(&comment), inner);
}

/* ------------------------------------------------------------ entry points */

Error xml_decoder_decode_element(XmlDecoder *d, Any v, const XmlStartElement *start) {
    if (v.t == NULL)
        return errors_new(error_allocator(),
                          BURROW_S("non-pointer passed to Unmarshal"));
    if (v.data == NULL)
        return errors_new(error_allocator(),
                          BURROW_S("nil pointer passed to Unmarshal"));
    d->unmarshal_depth++;
    Error err = xu_unmarshal(d, v.t, v.data, start);
    if (--d->unmarshal_depth == 0) {
        arena_reset(&d->scratch);
        d->keep_inner = false;
        d->ninner = 0;
    }
    return err;
}

Error xml_decoder_decode(XmlDecoder *d, Any v) {
    return xml_decoder_decode_element(d, v, NULL);
}

Error xml_unmarshal(Alloc *a, Slice data, Any v) {
    BytesReader r;
    bytes_reader_reset(&r, data);
    XmlDecoder *d = xml_new_decoder(a, bytes_reader_as_io_reader(&r));
    if (d == NULL)
        return burrow_err_out_of_memory;
    Error err = xml_decoder_decode(d, v);
    xml_decoder_free(d);
    return err;
}
