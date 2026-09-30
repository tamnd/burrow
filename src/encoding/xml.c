/* Derived from Go's src/encoding/xml/xml.go and the token half of marshal.go.
 * Go source: go1.27.1.
 *
 * The parser is Go's, byte by byte: getc, ungetc and a scratch buffer that
 * text, names and comments are collected in. What Go leaves to the garbage
 * collector is split up by how long it has to live. A token's strings go in
 * an arena that is reset when the next token is asked for, and its text stays
 * in the scratch buffer, so a token is good until the next call. The names on
 * the element stack, the namespace URLs in scope and a token held back by
 * auto_close outlive that, and each has memory of its own.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "xml_internal.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <string.h>

static const Str xml_url = BURROW_S_INIT("http://www.w3.org/XML/1998/namespace");
static const Str xmlns_prefix = BURROW_S_INIT("xmlns");
static const Str xml_prefix = BURROW_S_INIT("xml");

static bool xml_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------------------- types */

static const Type xml_attr_slice_desc;

static const Field xml_name_fields[] = {
    {{(const Byte *)"Space", 5},
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(XmlName, space)},
    {{(const Byte *)"Local", 5},
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(XmlName, local)},
};

const Type burrow_type_XmlName = {
    {(const Byte *)"Name", 4},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlName),
    (uint16_t)_Alignof(XmlName),
    2,
    0,
    xml_name_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Field xml_attr_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     &burrow_type_XmlName,
     (uint32_t)offsetof(XmlAttr, name)},
    {{(const Byte *)"Value", 5},
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(XmlAttr, value)},
};

const Type burrow_type_XmlAttr = {
    {(const Byte *)"Attr", 4},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlAttr),
    (uint16_t)_Alignof(XmlAttr),
    2,
    0,
    xml_attr_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* []xml.Attr. */
static const Type xml_attr_slice_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_XmlAttr,
    NULL,
    0,
    0,
    NULL,
};

static const Field xml_start_element_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     &burrow_type_XmlName,
     (uint32_t)offsetof(XmlStartElement, name)},
    {{(const Byte *)"Attr", 4},
     {NULL, 0},
     &xml_attr_slice_desc,
     (uint32_t)offsetof(XmlStartElement, attr)},
};

const Type burrow_type_XmlStartElement = {
    {(const Byte *)"StartElement", 12},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlStartElement),
    (uint16_t)_Alignof(XmlStartElement),
    2,
    0,
    xml_start_element_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Field xml_end_element_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     &burrow_type_XmlName,
     (uint32_t)offsetof(XmlEndElement, name)},
};

const Type burrow_type_XmlEndElement = {
    {(const Byte *)"EndElement", 10},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlEndElement),
    (uint16_t)_Alignof(XmlEndElement),
    1,
    0,
    xml_end_element_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Field xml_proc_inst_fields[] = {
    {{(const Byte *)"Target", 6},
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(XmlProcInst, target)},
    {{(const Byte *)"Inst", 4},
     {NULL, 0},
     TYPE_BYTES,
     (uint32_t)offsetof(XmlProcInst, inst)},
};

const Type burrow_type_XmlProcInst = {
    {(const Byte *)"ProcInst", 8},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlProcInst),
    (uint16_t)_Alignof(XmlProcInst),
    2,
    0,
    xml_proc_inst_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------------ copies */

XmlEndElement xml_start_element_end(XmlStartElement e) {
    return (XmlEndElement){e.name};
}

static Str xml_put_str(Byte **p, Str s) {
    if (s.len == 0)
        return (Str){NULL, 0};
    memcpy(*p, s.p, (size_t)s.len);
    Str out = {*p, s.len};
    *p += s.len;
    return out;
}

static Slice xml_put_bytes(Byte **p, Slice s) {
    if (s.len == 0)
        return (Slice){NULL, 0, 0, TYPE_BYTE};
    memcpy(*p, s.p, (size_t)s.len);
    Slice out = {*p, s.len, s.len, TYPE_BYTE};
    *p += s.len;
    return out;
}

/* The bytes a copy of e's strings takes, not counting the attribute array. */
static size_t xml_start_element_strings(XmlStartElement e) {
    size_t n = (size_t)e.name.space.len + (size_t)e.name.local.len;
    const XmlAttr *at = (const XmlAttr *)e.attr.p;
    for (Int i = 0; i < e.attr.len; i++)
        n += (size_t)at[i].name.space.len + (size_t)at[i].name.local.len +
             (size_t)at[i].value.len;
    return n;
}

/* The block a copy was made in, which starts with whichever part of the copy
 * came first. NULL when the copy took no memory. */
static void *xml_start_element_base(XmlStartElement e) {
    if (e.attr.len > 0)
        return e.attr.p;
    if (e.name.space.len > 0)
        return (void *)(uintptr_t)e.name.space.p;
    if (e.name.local.len > 0)
        return (void *)(uintptr_t)e.name.local.p;
    return NULL;
}

XmlStartElement xml_start_element_copy(XmlStartElement e, Alloc *a) {
    size_t arr = (size_t)e.attr.len * sizeof(XmlAttr);
    size_t size = arr + xml_start_element_strings(e);
    XmlStartElement out = {{{NULL, 0}, {NULL, 0}}, {NULL, 0, 0, &burrow_type_XmlAttr}};
    if (size == 0)
        return out;
    Byte *block = (Byte *)mem_alloc_nozero(a, size, _Alignof(XmlAttr));
    if (block == NULL)
        return out;
    XmlAttr *dst = (XmlAttr *)(void *)block;
    Byte *p = block + arr;
    out.name.space = xml_put_str(&p, e.name.space);
    out.name.local = xml_put_str(&p, e.name.local);
    const XmlAttr *src = (const XmlAttr *)e.attr.p;
    for (Int i = 0; i < e.attr.len; i++) {
        dst[i].name.space = xml_put_str(&p, src[i].name.space);
        dst[i].name.local = xml_put_str(&p, src[i].name.local);
        dst[i].value = xml_put_str(&p, src[i].value);
    }
    if (e.attr.len > 0)
        out.attr = (Slice){dst, e.attr.len, e.attr.len, &burrow_type_XmlAttr};
    return out;
}

static Slice xml_bytes_copy(Alloc *a, Slice s) {
    if (s.len == 0)
        return (Slice){NULL, 0, 0, TYPE_BYTE};
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return (Slice){NULL, 0, 0, TYPE_BYTE};
    return xml_put_bytes(&p, s);
}

XmlCharData xml_char_data_copy(XmlCharData c, Alloc *a) {
    return xml_bytes_copy(a, c);
}

XmlComment xml_comment_copy(XmlComment c, Alloc *a) {
    return xml_bytes_copy(a, c);
}

XmlDirective xml_directive_copy(XmlDirective d, Alloc *a) {
    return xml_bytes_copy(a, d);
}

XmlProcInst xml_proc_inst_copy(XmlProcInst pi, Alloc *a) {
    XmlProcInst out = {{NULL, 0}, {NULL, 0, 0, TYPE_BYTE}};
    size_t size = (size_t)pi.target.len + (size_t)pi.inst.len;
    if (size == 0)
        return out;
    Byte *p = (Byte *)mem_alloc_nozero(a, size, 1);
    if (p == NULL)
        return out;
    out.target = xml_put_str(&p, pi.target);
    out.inst = xml_put_bytes(&p, pi.inst);
    return out;
}

static XmlName xml_name_copy(Alloc *a, XmlName n) {
    XmlName out = {{NULL, 0}, {NULL, 0}};
    size_t size = (size_t)n.space.len + (size_t)n.local.len;
    if (size == 0)
        return out;
    Byte *p = (Byte *)mem_alloc_nozero(a, size, 1);
    if (p == NULL)
        return out;
    out.space = xml_put_str(&p, n.space);
    out.local = xml_put_str(&p, n.local);
    return out;
}

XmlToken xml_copy_token(Alloc *a, XmlToken t) {
    XmlToken out = t;
    switch (t.kind) {
    case XML_START_ELEMENT:
        out.start = xml_start_element_copy(t.start, a);
        break;
    case XML_END_ELEMENT:
        out.end.name = xml_name_copy(a, t.end.name);
        break;
    case XML_CHAR_DATA:
        out.char_data = xml_char_data_copy(t.char_data, a);
        break;
    case XML_COMMENT:
        out.comment = xml_comment_copy(t.comment, a);
        break;
    case XML_PROC_INST:
        out.proc_inst = xml_proc_inst_copy(t.proc_inst, a);
        break;
    case XML_DIRECTIVE:
        out.directive = xml_directive_copy(t.directive, a);
        break;
    case XML_TOKEN_NONE:
    default:
        break;
    }
    return out;
}

void xml_token_free(Alloc *a, XmlToken t) {
    void *base = NULL;
    size_t size = 0;
    size_t align = 1;
    switch (t.kind) {
    case XML_START_ELEMENT:
        base = xml_start_element_base(t.start);
        size = (size_t)t.start.attr.len * sizeof(XmlAttr) +
               xml_start_element_strings(t.start);
        align = _Alignof(XmlAttr);
        break;
    case XML_END_ELEMENT:
        base = t.end.name.space.len > 0 ? (void *)(uintptr_t)t.end.name.space.p
                                        : (void *)(uintptr_t)t.end.name.local.p;
        size = (size_t)t.end.name.space.len + (size_t)t.end.name.local.len;
        break;
    case XML_CHAR_DATA:
    case XML_COMMENT:
    case XML_DIRECTIVE:
        base = t.char_data.p;
        size = (size_t)t.char_data.len;
        break;
    case XML_PROC_INST:
        base = t.proc_inst.target.len > 0 ? (void *)(uintptr_t)t.proc_inst.target.p
                                          : t.proc_inst.inst.p;
        size = (size_t)t.proc_inst.target.len + (size_t)t.proc_inst.inst.len;
        break;
    case XML_TOKEN_NONE:
    default:
        break;
    }
    if (base != NULL && size > 0)
        mem_free(a, base, size, align);
}

/* ------------------------------------------------------------------ errors */

/* The public struct first, so errors_as can hand it straight back, then the
 * message, which the message slot cannot build because it cannot allocate. */
typedef struct XmlSyntaxErrorBox {
    XmlSyntaxError e;
    Str message;
} XmlSyntaxErrorBox;

static Str xml_syntax_error_message(const void *self) {
    return ((const XmlSyntaxErrorBox *)self)->message;
}

static const Type xml_syntax_error_desc = {
    {(const Byte *)"SyntaxError", 11},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlSyntaxError),
    (uint16_t)_Alignof(XmlSyntaxError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x786d7365U, /* "xmse" */
    NULL,
};

const Type *const TYPE_XML_SYNTAX_ERROR = &xml_syntax_error_desc;

static Error xml_syntax_error_clone(const void *self, Alloc *a);

static const ErrorVT xml_syntax_error_vt = {
    &xml_syntax_error_desc, xml_syntax_error_message, NULL, NULL, NULL, NULL,
    xml_syntax_error_clone,
};

/* "XML syntax error on line N: msg" into p, or only measured when p is
 * NULL. */
static Int xml_syntax_error_write(Byte *p, const XmlSyntaxError *e) {
    static const Str head = BURROW_S_INIT("XML syntax error on line ");
    Byte digits[24];
    Int d = (Int)sizeof(digits);
    uint64_t u = e->line < 0 ? 0 - (uint64_t)e->line : (uint64_t)e->line;
    do {
        digits[--d] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (e->line < 0)
        digits[--d] = '-';
    Int nd = (Int)sizeof(digits) - d;
    Int n = head.len + nd + 2 + e->msg.len;
    if (p != NULL) {
        memcpy(p, head.p, (size_t)head.len);
        memcpy(p + head.len, digits + d, (size_t)nd);
        p[head.len + nd] = ':';
        p[head.len + nd + 1] = ' ';
        if (e->msg.len > 0)
            memcpy(p + head.len + nd + 2, e->msg.p, (size_t)e->msg.len);
    }
    return n;
}

Str xml_syntax_error_error(const XmlSyntaxError *e, Alloc *a) {
    Int n = xml_syntax_error_write(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    xml_syntax_error_write(p, e);
    return str_from_bytes(p, n);
}

/* One block: the box, the message, then msg. */
Error xml_syntax_error_as_error(Alloc *a, const XmlSyntaxError *e) {
    Int mlen = xml_syntax_error_write(NULL, e);
    XmlSyntaxErrorBox *b = (XmlSyntaxErrorBox *)mem_alloc_nozero(
        a, sizeof(XmlSyntaxErrorBox) + (size_t)mlen + (size_t)e->msg.len,
        _Alignof(XmlSyntaxErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    xml_syntax_error_write(p, e);
    b->message = str_from_bytes(p, mlen);
    b->e.line = e->line;
    b->e.msg = str_from_bytes(p + mlen, e->msg.len);
    if (e->msg.len > 0)
        memcpy(p + mlen, e->msg.p, (size_t)e->msg.len);
    return (Error){&xml_syntax_error_vt, b};
}

static Error xml_syntax_error_clone(const void *self, Alloc *a) {
    return xml_syntax_error_as_error(a, (const XmlSyntaxError *)self);
}

/* ----------------------------------------------------------- decoder state */

enum { STK_START, STK_NS, STK_EOF };

/* Go's stack. A start's name lives in buf, which the record keeps when it goes
 * on the free list. A namespace record holds the prefix, interned, and the URL
 * it had before, which the record owns while it is on the stack. */
struct XmlStack {
    XmlStack *next;
    int kind;
    XmlName name;
    bool ok;
    Byte *buf;
    Int cap;
};

static void xml_oom(XmlDecoder *d) {
    d->oom = true;
}

/* s in the token arena, good until the next call. */
static Str xml_tok_str(XmlDecoder *d, Str s) {
    if (s.len == 0)
        return (Str){NULL, 0};
    Byte *p = (Byte *)mem_alloc_nozero(arena_allocator(&d->tok), (size_t)s.len, 1);
    if (p == NULL) {
        xml_oom(d);
        return (Str){NULL, 0};
    }
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

static XmlName xml_tok_name(XmlDecoder *d, XmlName n) {
    return (XmlName){xml_tok_str(d, n.space), xml_tok_str(d, n.local)};
}

/* s in memory from the decoder's allocator, for a namespace URL. */
static Str xml_heap_str(XmlDecoder *d, Str s) {
    if (s.len == 0)
        return (Str){NULL, 0};
    Byte *p = (Byte *)mem_alloc_nozero(d->a, (size_t)s.len, 1);
    if (p == NULL) {
        xml_oom(d);
        return (Str){NULL, 0};
    }
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

static void xml_heap_str_free(XmlDecoder *d, Str s) {
    if (s.len > 0)
        mem_free(d->a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* The one copy of a namespace prefix, which the ns map and the stack share. */
static Str xml_intern(XmlDecoder *d, Str s) {
    Str *have = BURROW_MAP_GET(Str, Str, d->prefixes, s);
    if (have != NULL)
        return *have;
    Str k = {NULL, 0};
    if (s.len > 0) {
        Byte *p =
            (Byte *)mem_alloc_nozero(arena_allocator(&d->names), (size_t)s.len, 1);
        if (p == NULL) {
            xml_oom(d);
            return s;
        }
        memcpy(p, s.p, (size_t)s.len);
        k = (Str){p, s.len};
    }
    if (!BURROW_MAP_SET(Str, Str, d->prefixes, k, k))
        xml_oom(d);
    return k;
}

static bool xml_buf_grow(XmlDecoder *d, Int need) {
    Int cap = d->bcap < 64 ? 64 : d->bcap;
    while (cap < need)
        cap *= 2;
    Byte *nb = (Byte *)mem_realloc(d->a, d->buf, (size_t)d->bcap, (size_t)cap, 1);
    if (nb == NULL) {
        xml_oom(d);
        return false;
    }
    d->buf = nb;
    d->bcap = cap;
    return true;
}

static inline void xml_buf_put(XmlDecoder *d, Byte b) {
    if (BURROW_UNLIKELY(d->blen == d->bcap) && !xml_buf_grow(d, d->blen + 1))
        return;
    d->buf[d->blen++] = b;
}

static void xml_buf_write(XmlDecoder *d, const Byte *p, Int n) {
    if (n == 0)
        return;
    if (d->blen + n > d->bcap && !xml_buf_grow(d, d->blen + n))
        return;
    memcpy(d->buf + d->blen, p, (size_t)n);
    d->blen += n;
}

static Slice xml_buf_bytes(XmlDecoder *d, Int from, Int to) {
    return (Slice){d->buf + from, to - from, to - from, TYPE_BYTE};
}

static Error xml_syntax_error(XmlDecoder *d, Str msg) {
    XmlSyntaxError e = {msg, d->line};
    return xml_syntax_error_as_error(error_allocator(), &e);
}

/* The Go source builds its messages with +. These put up to four pieces
 * together in the error arena first. */
static Error xml_syntax_error4(XmlDecoder *d, Str a, Str b, Str c, Str e) {
    Alloc *ea = error_allocator();
    Int n = a.len + b.len + c.len + e.len;
    Byte *p = (Byte *)mem_alloc_nozero(ea, (size_t)(n > 0 ? n : 1), 1);
    if (p == NULL)
        return burrow_err_out_of_memory;
    Byte *q = p;
    xml_put_str(&q, a);
    xml_put_str(&q, b);
    xml_put_str(&q, c);
    xml_put_str(&q, e);
    return xml_syntax_error(d, (Str){p, n});
}

static Error xml_syntax_errorf(XmlDecoder *d, Str msg) {
    return xml_syntax_error(d, msg);
}

#define XML_SYNTAX(d, lit) xml_syntax_errorf((d), BURROW_S(lit))

/* ------------------------------------------------------------------- stack */

static XmlStack *xml_push(XmlDecoder *d, int kind) {
    XmlStack *s = d->free;
    if (s != NULL) {
        d->free = s->next;
        if (s->kind == STK_NS && s->ok)
            xml_heap_str_free(d, s->name.space);
    } else {
        s = (XmlStack *)mem_alloc(d->a, sizeof *s, _Alignof(XmlStack));
        if (s == NULL) {
            xml_oom(d);
            return NULL;
        }
    }
    s->next = d->stk;
    s->kind = kind;
    s->ok = false;
    s->name = (XmlName){{NULL, 0}, {NULL, 0}};
    if (kind == STK_START)
        d->stk_depth++;
    d->stk = s;
    return s;
}

static XmlStack *xml_pop(XmlDecoder *d) {
    XmlStack *s = d->stk;
    if (s != NULL) {
        if (s->kind == STK_START)
            d->stk_depth--;
        d->stk = s->next;
        s->next = d->free;
        d->free = s;
    }
    return s;
}

static void xml_push_element(XmlDecoder *d, XmlName name) {
    Int need = name.space.len + name.local.len;
    XmlStack *s = xml_push(d, STK_START);
    if (s == NULL)
        return;
    if (need > s->cap) {
        Byte *nb = (Byte *)mem_realloc(d->a, s->buf, (size_t)s->cap, (size_t)need, 1);
        if (nb == NULL) {
            xml_oom(d);
            return;
        }
        s->buf = nb;
        s->cap = need;
    }
    Byte *p = s->buf;
    s->name.space = xml_put_str(&p, name.space);
    s->name.local = xml_put_str(&p, name.local);
}

/* url, which the record now owns, is what local mapped to before, if ok. */
static bool xml_push_ns(XmlDecoder *d, Str local, Str url, bool ok) {
    XmlStack *s = xml_push(d, STK_NS);
    if (s == NULL)
        return false;
    s->name.local = local;
    s->name.space = url;
    s->ok = ok;
    return true;
}

/* d.ns[key] = url, where url is the decoder's from now on and whatever was
 * there has been taken by a namespace record. */
static void xml_ns_put(XmlDecoder *d, Str key, Str url) {
    Str *slot = BURROW_MAP_GET(Str, Str, d->ns, key);
    if (slot != NULL) {
        *slot = url;
        return;
    }
    if (!BURROW_MAP_SET(Str, Str, d->ns, key, url)) {
        xml_heap_str_free(d, url);
        xml_oom(d);
    }
}

/* Sets up the namespace an xmlns attribute declares, as Token does. */
static void xml_declare(XmlDecoder *d, Str prefix, Str value) {
    Str key = xml_intern(d, prefix);
    Str *old = BURROW_MAP_GET(Str, Str, d->ns, key);
    if (!xml_push_ns(d, key, old != NULL ? *old : (Str){NULL, 0}, old != NULL))
        return;
    xml_ns_put(d, key, xml_heap_str(d, value));
}

/* ---------------------------------------------------------------- decoding */

static const XmlTokenReaderVT xml_decoder_token_reader_vt;

static const Type xml_decoder_desc = {
    {(const Byte *)"Decoder", 7},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlDecoder),
    (uint16_t)_Alignof(XmlDecoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x786d6464U, /* "xmdd" */
    NULL,
};

const Type *const TYPE_XML_DECODER = &xml_decoder_desc;

XmlToken xml_token_reader_token(XmlTokenReader r, Error *err) {
    return r.vt->token(r.data, err);
}

/* switchToReader. The bytes come from r itself when it is a BufioReader or
 * has ReadByte, and through a BufioReader of the decoder's own when not. */
static bool xml_switch_to_reader(XmlDecoder *d, IoReader r) {
    d->src = r;
    d->direct = NULL;
    d->bytes_src = NULL;
    d->str_src = NULL;
    d->read_byte = NULL;
    if (r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER) {
        d->direct = (BufioReader *)r.data;
        return true;
    }
    /* The two in-memory readers are read in place, which is what their
     * ReadByte would do, without a call through the method table per byte. */
    if (r.vt != NULL && r.vt->self_type == TYPE_BYTES_READER) {
        d->bytes_src = (BytesReader *)r.data;
        return true;
    }
    if (r.vt != NULL && r.vt->self_type == TYPE_STRINGS_READER) {
        d->str_src = (StringsReader *)r.data;
        return true;
    }
    const Method *m = burrow__io_read_byte_method(r);
    if (m != NULL) {
        d->read_byte = m;
        return true;
    }
    if (d->nowned == d->capowned) {
        Int cap = d->capowned == 0 ? 2 : d->capowned * 2;
        BufioReader **no = (BufioReader **)mem_realloc(
            d->a, d->owned, (size_t)d->capowned * sizeof *no, (size_t)cap * sizeof *no,
            _Alignof(BufioReader *));
        if (no == NULL)
            return false;
        d->owned = no;
        d->capowned = cap;
    }
    BufioReader *b = bufio_new_reader(d->a, r);
    if (b == NULL)
        return false;
    d->owned[d->nowned++] = b;
    d->direct = b;
    d->src = bufio_reader_as_io_reader(b);
    return true;
}

static XmlDecoder *xml_decoder_alloc(Alloc *a) {
    if (a == NULL)
        a = heap_allocator();
    XmlDecoder *d = (XmlDecoder *)mem_alloc(a, sizeof *d, _Alignof(XmlDecoder));
    if (d == NULL)
        return NULL;
    d->a = a;
    d->strict = true;
    d->next_byte = -1;
    d->line = 1;
    d->ns = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    d->prefixes = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    if (d->ns == NULL || d->prefixes == NULL) {
        map_free(d->ns);
        map_free(d->prefixes);
        mem_free(a, d, sizeof *d, _Alignof(XmlDecoder));
        return NULL;
    }
    arena_init(&d->tok, a, 0);
    arena_init(&d->saved[0], a, 1024);
    arena_init(&d->saved[1], a, 1024);
    arena_init(&d->names, a, 512);
    return d;
}

XmlDecoder *xml_new_decoder(Alloc *a, IoReader r) {
    XmlDecoder *d = xml_decoder_alloc(a);
    if (d == NULL)
        return NULL;
    if (!xml_switch_to_reader(d, r)) {
        xml_decoder_free(d);
        return NULL;
    }
    return d;
}

XmlDecoder *xml_new_token_decoder(Alloc *a, XmlTokenReader t) {
    if (t.vt == &xml_decoder_token_reader_vt)
        return (XmlDecoder *)t.data;
    XmlDecoder *d = xml_decoder_alloc(a);
    if (d == NULL)
        return NULL;
    d->t = t;
    return d;
}

static void xml_stack_free_list(XmlDecoder *d, XmlStack *s) {
    while (s != NULL) {
        XmlStack *next = s->next;
        if (s->kind == STK_NS && s->ok)
            xml_heap_str_free(d, s->name.space);
        if (s->cap > 0)
            mem_free(d->a, s->buf, (size_t)s->cap, 1);
        mem_free(d->a, s, sizeof *s, _Alignof(XmlStack));
        s = next;
    }
}

void xml_decoder_free(XmlDecoder *d) {
    if (d == NULL)
        return;
    Alloc *a = d->a;
    for (Int i = 0; i < d->nowned; i++)
        bufio_reader_free(d->owned[i]);
    if (d->capowned > 0)
        mem_free(a, d->owned, (size_t)d->capowned * sizeof(BufioReader *),
                 _Alignof(BufioReader *));
    if (d->bcap > 0)
        mem_free(a, d->buf, (size_t)d->bcap, 1);
    if (d->capattrs > 0)
        mem_free(a, d->attrs, (size_t)d->capattrs * sizeof(XmlAttr), _Alignof(XmlAttr));
    if (d->to_close_cap > 0)
        mem_free(a, d->to_close_buf, (size_t)d->to_close_cap, 1);
    xml_stack_free_list(d, d->stk);
    xml_stack_free_list(d, d->free);
    const void *k;
    void *v;
    for (MapIter it = map_iter(d->ns); map_next(&it, &k, &v);)
        xml_heap_str_free(d, *(Str *)v);
    map_free(d->ns);
    map_free(d->prefixes);
    arena_free(&d->tok);
    arena_free(&d->saved[0]);
    arena_free(&d->saved[1]);
    arena_free(&d->names);
    mem_free(a, d, sizeof *d, _Alignof(XmlDecoder));
}

static XmlToken xml_decoder_token_thunk(void *self, Error *err) {
    return xml_decoder_token((XmlDecoder *)self, err);
}

static const XmlTokenReaderVT xml_decoder_token_reader_vt = {&xml_decoder_desc,
                                                             xml_decoder_token_thunk};

XmlTokenReader xml_decoder_as_token_reader(XmlDecoder *d) {
    return (XmlTokenReader){&xml_decoder_token_reader_vt, d};
}

int64_t xml_decoder_input_offset(const XmlDecoder *d) {
    return d->offset;
}

Int xml_decoder_input_pos(const XmlDecoder *d, Int *column) {
    if (column != NULL)
        *column = (Int)(d->offset - d->linestart) + 1;
    return d->line;
}

/* d.toClose = name, in the decoder's own buffer. name may already be in it,
 * when it is the end tag the last call made from it. */
static void xml_set_to_close(XmlDecoder *d, XmlName name) {
    Byte *b = d->to_close_buf;
    if (name.local.len > 0 && name.local.p >= b && name.local.p < b + d->to_close_cap) {
        d->to_close = name;
        return;
    }
    Int need = name.space.len + name.local.len;
    if (need > d->to_close_cap) {
        Byte *nb =
            (Byte *)mem_realloc(d->a, b, (size_t)d->to_close_cap, (size_t)need, 1);
        if (nb == NULL) {
            xml_oom(d);
            return;
        }
        d->to_close_buf = nb;
        d->to_close_cap = need;
    }
    Byte *p = d->to_close_buf;
    d->to_close.space = xml_put_str(&p, name.space);
    d->to_close.local = xml_put_str(&p, name.local);
}

/* translate: the namespace prefix in n replaced by its URL. */
static void xml_translate(XmlDecoder *d, XmlName *n, bool is_element_name) {
    if (str_eq(n->space, xmlns_prefix))
        return;
    if (n->space.len == 0 && !is_element_name)
        return;
    if (str_eq(n->space, xml_prefix))
        n->space = xml_url;
    else if (n->space.len == 0 && str_eq(n->local, xmlns_prefix))
        return;
    Str *v = BURROW_MAP_GET(Str, Str, d->ns, n->space);
    if (v != NULL)
        n->space = xml_tok_str(d, *v);
    else if (n->space.len == 0)
        n->space = d->default_space;
}

/* popElement: checks t against the start tag it closes and ends the
 * namespaces that start tag declared. */
static bool xml_pop_element(XmlDecoder *d, XmlEndElement *t) {
    XmlStack *s = xml_pop(d);
    XmlName name = t->name;
    if (s == NULL || s->kind != STK_START) {
        d->err = xml_syntax_error4(d, BURROW_S("unexpected end element </"), name.local,
                                   BURROW_S(">"), BURROW_S(""));
        return false;
    }
    if (!str_eq(s->name.local, name.local)) {
        if (!d->strict) {
            d->need_close = true;
            xml_set_to_close(d, t->name);
            t->name = xml_tok_name(d, s->name);
            return true;
        }
        Str slocal = s->name.local;
        Str nlocal = name.local;
        d->err = xml_syntax_error(d, fmt_sprintf_v(error_allocator(),
                                                   "element <%s> closed by </%s>",
                                                   slocal, nlocal));
        return false;
    }
    if (!str_eq(s->name.space, name.space)) {
        Str ns = name.space.len == 0 ? BURROW_S("\"\"") : name.space;
        Str local = s->name.local;
        Str space = s->name.space;
        Str nlocal = name.local;
        Str msg = fmt_sprintf_v(error_allocator(),
                                "element <%s> in space %s closed by </%s> in space %s",
                                local, space, nlocal, ns);
        d->err = xml_syntax_error(d, msg);
        return false;
    }

    xml_translate(d, &t->name, true);

    /* Pop stack until a Start or EOF is on the top, undoing the translations
     * that were associated with the element we just closed. */
    while (d->stk != NULL && d->stk->kind != STK_START && d->stk->kind != STK_EOF) {
        XmlStack *r = xml_pop(d);
        Str *slot = BURROW_MAP_GET(Str, Str, d->ns, r->name.local);
        if (slot != NULL)
            xml_heap_str_free(d, *slot);
        if (r->ok) {
            if (slot != NULL)
                *slot = r->name.space;
            else
                xml_ns_put(d, r->name.local, r->name.space);
        } else if (slot != NULL) {
            BURROW_MAP_DEL(Str, d->ns, r->name.local);
        }
        r->name.space = (Str){NULL, 0};
        r->ok = false;
    }
    return true;
}

/* autoClose: if the top of the stack is an element that closes itself and t
 * isn't its end tag, the end tag to hand out first. */
static bool xml_auto_close(XmlDecoder *d, XmlToken t, XmlToken *out) {
    if (d->stk == NULL || d->stk->kind != STK_START)
        return false;
    const Str *names = (const Str *)d->auto_close.p;
    for (Int i = 0; i < d->auto_close.len; i++) {
        if (strings_equal_fold(names[i], d->stk->name.local)) {
            if (t.kind != XML_END_ELEMENT ||
                !strings_equal_fold(t.end.name.local, d->stk->name.local)) {
                out->kind = XML_END_ELEMENT;
                out->end.name = xml_tok_name(d, d->stk->name);
                return true;
            }
            break;
        }
    }
    return false;
}

/* Holds t back for the next call, in the saved arena that isn't holding the
 * token this call may be handing out. */
static void xml_save_next(XmlDecoder *d, XmlToken t) {
    int other = 1 - d->cur;
    arena_reset(&d->saved[other]);
    XmlToken c = xml_copy_token(arena_allocator(&d->saved[other]), t);
    if (c.kind == XML_START_ELEMENT && t.start.attr.len > 0 && c.start.attr.len == 0)
        xml_oom(d);
    d->next_token = c;
    d->cur = other;
}

/* ------------------------------------------------------------- byte input */

static bool xml_getc(XmlDecoder *d, Byte *out) {
    if (BURROW_FAILED(d->err))
        return false;
    Byte b;
    if (d->next_byte >= 0) {
        b = (Byte)d->next_byte;
        d->next_byte = -1;
    } else {
        BufioReader *br = d->direct;
        if (br != NULL && br->r < br->w) {
            b = br->buf[br->r++];
            br->last_byte = b;
            br->last_rune_size = -1;
        } else {
            Error e = BURROW_NO_ERROR;
            if (br != NULL) {
                b = bufio_reader_read_byte(br, &e);
            } else if (d->bytes_src != NULL || d->str_src != NULL) {
                BytesReader *mb = d->bytes_src;
                StringsReader *ms = d->str_src;
                const Byte *p = mb != NULL ? (const Byte *)mb->s.p : ms->s.p;
                Int n = mb != NULL ? mb->s.len : ms->s.len;
                int64_t *at = mb != NULL ? &mb->i : &ms->i;
                if (mb != NULL)
                    mb->prev_rune = -1;
                else
                    ms->prev_rune = -1;
                if (*at >= (int64_t)n) {
                    e = io_eof;
                    b = 0;
                } else {
                    b = p[(*at)++];
                }
            } else {
                IoErrorArg ea = &e;
                void *args[1] = {(void *)(uintptr_t)&ea};
                void *rets[1] = {&b};
                d->read_byte->thunk(d->src.data, args, rets);
            }
            if (BURROW_FAILED(e)) {
                d->err = e;
                return false;
            }
        }
    }
    if (b == '\n') {
        d->line++;
        d->linestart = d->offset + 1;
    }
    d->offset++;
    *out = b;
    return true;
}

static bool xml_mustgetc(XmlDecoder *d, Byte *b) {
    if (!xml_getc(d, b)) {
        if (xml_same_error(d->err, io_eof))
            d->err = XML_SYNTAX(d, "unexpected EOF");
        return false;
    }
    return true;
}

static void xml_ungetc(XmlDecoder *d, Byte b) {
    if (b == '\n')
        d->line--;
    d->next_byte = b;
    d->offset--;
}

static void xml_space(XmlDecoder *d) {
    for (;;) {
        Byte b;
        if (!xml_getc(d, &b))
            return;
        switch (b) {
        case ' ':
        case '\r':
        case '\n':
        case '\t':
            break;
        default:
            xml_ungetc(d, b);
            return;
        }
    }
}

/* ------------------------------------------------------------------- names */

/* Go's first and second, the characters a name can start with and the ones
 * it can go on with, and entity, the HTML 4 entities. */

typedef struct XmlEntityPair {
    Str name;
    Str value;
} XmlEntityPair;

static const UnicodeRange16 xml_first_r16[] = {
    {0x003A, 0x003A, 1},
    {0x0041, 0x005A, 1},
    {0x005F, 0x005F, 1},
    {0x0061, 0x007A, 1},
    {0x00C0, 0x00D6, 1},
    {0x00D8, 0x00F6, 1},
    {0x00F8, 0x00FF, 1},
    {0x0100, 0x0131, 1},
    {0x0134, 0x013E, 1},
    {0x0141, 0x0148, 1},
    {0x014A, 0x017E, 1},
    {0x0180, 0x01C3, 1},
    {0x01CD, 0x01F0, 1},
    {0x01F4, 0x01F5, 1},
    {0x01FA, 0x0217, 1},
    {0x0250, 0x02A8, 1},
    {0x02BB, 0x02C1, 1},
    {0x0386, 0x0386, 1},
    {0x0388, 0x038A, 1},
    {0x038C, 0x038C, 1},
    {0x038E, 0x03A1, 1},
    {0x03A3, 0x03CE, 1},
    {0x03D0, 0x03D6, 1},
    {0x03DA, 0x03E0, 2},
    {0x03E2, 0x03F3, 1},
    {0x0401, 0x040C, 1},
    {0x040E, 0x044F, 1},
    {0x0451, 0x045C, 1},
    {0x045E, 0x0481, 1},
    {0x0490, 0x04C4, 1},
    {0x04C7, 0x04C8, 1},
    {0x04CB, 0x04CC, 1},
    {0x04D0, 0x04EB, 1},
    {0x04EE, 0x04F5, 1},
    {0x04F8, 0x04F9, 1},
    {0x0531, 0x0556, 1},
    {0x0559, 0x0559, 1},
    {0x0561, 0x0586, 1},
    {0x05D0, 0x05EA, 1},
    {0x05F0, 0x05F2, 1},
    {0x0621, 0x063A, 1},
    {0x0641, 0x064A, 1},
    {0x0671, 0x06B7, 1},
    {0x06BA, 0x06BE, 1},
    {0x06C0, 0x06CE, 1},
    {0x06D0, 0x06D3, 1},
    {0x06D5, 0x06D5, 1},
    {0x06E5, 0x06E6, 1},
    {0x0905, 0x0939, 1},
    {0x093D, 0x093D, 1},
    {0x0958, 0x0961, 1},
    {0x0985, 0x098C, 1},
    {0x098F, 0x0990, 1},
    {0x0993, 0x09A8, 1},
    {0x09AA, 0x09B0, 1},
    {0x09B2, 0x09B2, 1},
    {0x09B6, 0x09B9, 1},
    {0x09DC, 0x09DD, 1},
    {0x09DF, 0x09E1, 1},
    {0x09F0, 0x09F1, 1},
    {0x0A05, 0x0A0A, 1},
    {0x0A0F, 0x0A10, 1},
    {0x0A13, 0x0A28, 1},
    {0x0A2A, 0x0A30, 1},
    {0x0A32, 0x0A33, 1},
    {0x0A35, 0x0A36, 1},
    {0x0A38, 0x0A39, 1},
    {0x0A59, 0x0A5C, 1},
    {0x0A5E, 0x0A5E, 1},
    {0x0A72, 0x0A74, 1},
    {0x0A85, 0x0A8B, 1},
    {0x0A8D, 0x0A8D, 1},
    {0x0A8F, 0x0A91, 1},
    {0x0A93, 0x0AA8, 1},
    {0x0AAA, 0x0AB0, 1},
    {0x0AB2, 0x0AB3, 1},
    {0x0AB5, 0x0AB9, 1},
    {0x0ABD, 0x0AE0, 0x23},
    {0x0B05, 0x0B0C, 1},
    {0x0B0F, 0x0B10, 1},
    {0x0B13, 0x0B28, 1},
    {0x0B2A, 0x0B30, 1},
    {0x0B32, 0x0B33, 1},
    {0x0B36, 0x0B39, 1},
    {0x0B3D, 0x0B3D, 1},
    {0x0B5C, 0x0B5D, 1},
    {0x0B5F, 0x0B61, 1},
    {0x0B85, 0x0B8A, 1},
    {0x0B8E, 0x0B90, 1},
    {0x0B92, 0x0B95, 1},
    {0x0B99, 0x0B9A, 1},
    {0x0B9C, 0x0B9C, 1},
    {0x0B9E, 0x0B9F, 1},
    {0x0BA3, 0x0BA4, 1},
    {0x0BA8, 0x0BAA, 1},
    {0x0BAE, 0x0BB5, 1},
    {0x0BB7, 0x0BB9, 1},
    {0x0C05, 0x0C0C, 1},
    {0x0C0E, 0x0C10, 1},
    {0x0C12, 0x0C28, 1},
    {0x0C2A, 0x0C33, 1},
    {0x0C35, 0x0C39, 1},
    {0x0C60, 0x0C61, 1},
    {0x0C85, 0x0C8C, 1},
    {0x0C8E, 0x0C90, 1},
    {0x0C92, 0x0CA8, 1},
    {0x0CAA, 0x0CB3, 1},
    {0x0CB5, 0x0CB9, 1},
    {0x0CDE, 0x0CDE, 1},
    {0x0CE0, 0x0CE1, 1},
    {0x0D05, 0x0D0C, 1},
    {0x0D0E, 0x0D10, 1},
    {0x0D12, 0x0D28, 1},
    {0x0D2A, 0x0D39, 1},
    {0x0D60, 0x0D61, 1},
    {0x0E01, 0x0E2E, 1},
    {0x0E30, 0x0E30, 1},
    {0x0E32, 0x0E33, 1},
    {0x0E40, 0x0E45, 1},
    {0x0E81, 0x0E82, 1},
    {0x0E84, 0x0E84, 1},
    {0x0E87, 0x0E88, 1},
    {0x0E8A, 0x0E8D, 3},
    {0x0E94, 0x0E97, 1},
    {0x0E99, 0x0E9F, 1},
    {0x0EA1, 0x0EA3, 1},
    {0x0EA5, 0x0EA7, 2},
    {0x0EAA, 0x0EAB, 1},
    {0x0EAD, 0x0EAE, 1},
    {0x0EB0, 0x0EB0, 1},
    {0x0EB2, 0x0EB3, 1},
    {0x0EBD, 0x0EBD, 1},
    {0x0EC0, 0x0EC4, 1},
    {0x0F40, 0x0F47, 1},
    {0x0F49, 0x0F69, 1},
    {0x10A0, 0x10C5, 1},
    {0x10D0, 0x10F6, 1},
    {0x1100, 0x1100, 1},
    {0x1102, 0x1103, 1},
    {0x1105, 0x1107, 1},
    {0x1109, 0x1109, 1},
    {0x110B, 0x110C, 1},
    {0x110E, 0x1112, 1},
    {0x113C, 0x1140, 2},
    {0x114C, 0x1150, 2},
    {0x1154, 0x1155, 1},
    {0x1159, 0x1159, 1},
    {0x115F, 0x1161, 1},
    {0x1163, 0x1169, 2},
    {0x116D, 0x116E, 1},
    {0x1172, 0x1173, 1},
    {0x1175, 0x119E, 0x119E - 0x1175},
    {0x11A8, 0x11AB, 0x11AB - 0x11A8},
    {0x11AE, 0x11AF, 1},
    {0x11B7, 0x11B8, 1},
    {0x11BA, 0x11BA, 1},
    {0x11BC, 0x11C2, 1},
    {0x11EB, 0x11F0, 0x11F0 - 0x11EB},
    {0x11F9, 0x11F9, 1},
    {0x1E00, 0x1E9B, 1},
    {0x1EA0, 0x1EF9, 1},
    {0x1F00, 0x1F15, 1},
    {0x1F18, 0x1F1D, 1},
    {0x1F20, 0x1F45, 1},
    {0x1F48, 0x1F4D, 1},
    {0x1F50, 0x1F57, 1},
    {0x1F59, 0x1F5B, 0x1F5B - 0x1F59},
    {0x1F5D, 0x1F5D, 1},
    {0x1F5F, 0x1F7D, 1},
    {0x1F80, 0x1FB4, 1},
    {0x1FB6, 0x1FBC, 1},
    {0x1FBE, 0x1FBE, 1},
    {0x1FC2, 0x1FC4, 1},
    {0x1FC6, 0x1FCC, 1},
    {0x1FD0, 0x1FD3, 1},
    {0x1FD6, 0x1FDB, 1},
    {0x1FE0, 0x1FEC, 1},
    {0x1FF2, 0x1FF4, 1},
    {0x1FF6, 0x1FFC, 1},
    {0x2126, 0x2126, 1},
    {0x212A, 0x212B, 1},
    {0x212E, 0x212E, 1},
    {0x2180, 0x2182, 1},
    {0x3007, 0x3007, 1},
    {0x3021, 0x3029, 1},
    {0x3041, 0x3094, 1},
    {0x30A1, 0x30FA, 1},
    {0x3105, 0x312C, 1},
    {0x4E00, 0x9FA5, 1},
    {0xAC00, 0xD7A3, 1},
};

static const UnicodeRange16 xml_second_r16[] = {
    {0x002D, 0x002E, 1},
    {0x0030, 0x0039, 1},
    {0x00B7, 0x00B7, 1},
    {0x02D0, 0x02D1, 1},
    {0x0300, 0x0345, 1},
    {0x0360, 0x0361, 1},
    {0x0387, 0x0387, 1},
    {0x0483, 0x0486, 1},
    {0x0591, 0x05A1, 1},
    {0x05A3, 0x05B9, 1},
    {0x05BB, 0x05BD, 1},
    {0x05BF, 0x05BF, 1},
    {0x05C1, 0x05C2, 1},
    {0x05C4, 0x0640, 0x0640 - 0x05C4},
    {0x064B, 0x0652, 1},
    {0x0660, 0x0669, 1},
    {0x0670, 0x0670, 1},
    {0x06D6, 0x06DC, 1},
    {0x06DD, 0x06DF, 1},
    {0x06E0, 0x06E4, 1},
    {0x06E7, 0x06E8, 1},
    {0x06EA, 0x06ED, 1},
    {0x06F0, 0x06F9, 1},
    {0x0901, 0x0903, 1},
    {0x093C, 0x093C, 1},
    {0x093E, 0x094C, 1},
    {0x094D, 0x094D, 1},
    {0x0951, 0x0954, 1},
    {0x0962, 0x0963, 1},
    {0x0966, 0x096F, 1},
    {0x0981, 0x0983, 1},
    {0x09BC, 0x09BC, 1},
    {0x09BE, 0x09BF, 1},
    {0x09C0, 0x09C4, 1},
    {0x09C7, 0x09C8, 1},
    {0x09CB, 0x09CD, 1},
    {0x09D7, 0x09D7, 1},
    {0x09E2, 0x09E3, 1},
    {0x09E6, 0x09EF, 1},
    {0x0A02, 0x0A3C, 0x3A},
    {0x0A3E, 0x0A3F, 1},
    {0x0A40, 0x0A42, 1},
    {0x0A47, 0x0A48, 1},
    {0x0A4B, 0x0A4D, 1},
    {0x0A66, 0x0A6F, 1},
    {0x0A70, 0x0A71, 1},
    {0x0A81, 0x0A83, 1},
    {0x0ABC, 0x0ABC, 1},
    {0x0ABE, 0x0AC5, 1},
    {0x0AC7, 0x0AC9, 1},
    {0x0ACB, 0x0ACD, 1},
    {0x0AE6, 0x0AEF, 1},
    {0x0B01, 0x0B03, 1},
    {0x0B3C, 0x0B3C, 1},
    {0x0B3E, 0x0B43, 1},
    {0x0B47, 0x0B48, 1},
    {0x0B4B, 0x0B4D, 1},
    {0x0B56, 0x0B57, 1},
    {0x0B66, 0x0B6F, 1},
    {0x0B82, 0x0B83, 1},
    {0x0BBE, 0x0BC2, 1},
    {0x0BC6, 0x0BC8, 1},
    {0x0BCA, 0x0BCD, 1},
    {0x0BD7, 0x0BD7, 1},
    {0x0BE7, 0x0BEF, 1},
    {0x0C01, 0x0C03, 1},
    {0x0C3E, 0x0C44, 1},
    {0x0C46, 0x0C48, 1},
    {0x0C4A, 0x0C4D, 1},
    {0x0C55, 0x0C56, 1},
    {0x0C66, 0x0C6F, 1},
    {0x0C82, 0x0C83, 1},
    {0x0CBE, 0x0CC4, 1},
    {0x0CC6, 0x0CC8, 1},
    {0x0CCA, 0x0CCD, 1},
    {0x0CD5, 0x0CD6, 1},
    {0x0CE6, 0x0CEF, 1},
    {0x0D02, 0x0D03, 1},
    {0x0D3E, 0x0D43, 1},
    {0x0D46, 0x0D48, 1},
    {0x0D4A, 0x0D4D, 1},
    {0x0D57, 0x0D57, 1},
    {0x0D66, 0x0D6F, 1},
    {0x0E31, 0x0E31, 1},
    {0x0E34, 0x0E3A, 1},
    {0x0E46, 0x0E46, 1},
    {0x0E47, 0x0E4E, 1},
    {0x0E50, 0x0E59, 1},
    {0x0EB1, 0x0EB1, 1},
    {0x0EB4, 0x0EB9, 1},
    {0x0EBB, 0x0EBC, 1},
    {0x0EC6, 0x0EC6, 1},
    {0x0EC8, 0x0ECD, 1},
    {0x0ED0, 0x0ED9, 1},
    {0x0F18, 0x0F19, 1},
    {0x0F20, 0x0F29, 1},
    {0x0F35, 0x0F39, 2},
    {0x0F3E, 0x0F3F, 1},
    {0x0F71, 0x0F84, 1},
    {0x0F86, 0x0F8B, 1},
    {0x0F90, 0x0F95, 1},
    {0x0F97, 0x0F97, 1},
    {0x0F99, 0x0FAD, 1},
    {0x0FB1, 0x0FB7, 1},
    {0x0FB9, 0x0FB9, 1},
    {0x20D0, 0x20DC, 1},
    {0x20E1, 0x3005, 0x3005 - 0x20E1},
    {0x302A, 0x302F, 1},
    {0x3031, 0x3035, 1},
    {0x3099, 0x309A, 1},
    {0x309D, 0x309E, 1},
    {0x30FC, 0x30FE, 1},
};

/* 252 entries, sorted by name. */
static const XmlEntityPair xml_html_entities[] = {
    {BURROW_S_INIT("AElig"), BURROW_S_INIT("\xc3\x86")},
    {BURROW_S_INIT("Aacute"), BURROW_S_INIT("\xc3\x81")},
    {BURROW_S_INIT("Acirc"), BURROW_S_INIT("\xc3\x82")},
    {BURROW_S_INIT("Agrave"), BURROW_S_INIT("\xc3\x80")},
    {BURROW_S_INIT("Alpha"), BURROW_S_INIT("\xce\x91")},
    {BURROW_S_INIT("Aring"), BURROW_S_INIT("\xc3\x85")},
    {BURROW_S_INIT("Atilde"), BURROW_S_INIT("\xc3\x83")},
    {BURROW_S_INIT("Auml"), BURROW_S_INIT("\xc3\x84")},
    {BURROW_S_INIT("Beta"), BURROW_S_INIT("\xce\x92")},
    {BURROW_S_INIT("Ccedil"), BURROW_S_INIT("\xc3\x87")},
    {BURROW_S_INIT("Chi"), BURROW_S_INIT("\xce\xa7")},
    {BURROW_S_INIT("Dagger"), BURROW_S_INIT("\xe2\x80\xa1")},
    {BURROW_S_INIT("Delta"), BURROW_S_INIT("\xce\x94")},
    {BURROW_S_INIT("ETH"), BURROW_S_INIT("\xc3\x90")},
    {BURROW_S_INIT("Eacute"), BURROW_S_INIT("\xc3\x89")},
    {BURROW_S_INIT("Ecirc"), BURROW_S_INIT("\xc3\x8a")},
    {BURROW_S_INIT("Egrave"), BURROW_S_INIT("\xc3\x88")},
    {BURROW_S_INIT("Epsilon"), BURROW_S_INIT("\xce\x95")},
    {BURROW_S_INIT("Eta"), BURROW_S_INIT("\xce\x97")},
    {BURROW_S_INIT("Euml"), BURROW_S_INIT("\xc3\x8b")},
    {BURROW_S_INIT("Gamma"), BURROW_S_INIT("\xce\x93")},
    {BURROW_S_INIT("Iacute"), BURROW_S_INIT("\xc3\x8d")},
    {BURROW_S_INIT("Icirc"), BURROW_S_INIT("\xc3\x8e")},
    {BURROW_S_INIT("Igrave"), BURROW_S_INIT("\xc3\x8c")},
    {BURROW_S_INIT("Iota"), BURROW_S_INIT("\xce\x99")},
    {BURROW_S_INIT("Iuml"), BURROW_S_INIT("\xc3\x8f")},
    {BURROW_S_INIT("Kappa"), BURROW_S_INIT("\xce\x9a")},
    {BURROW_S_INIT("Lambda"), BURROW_S_INIT("\xce\x9b")},
    {BURROW_S_INIT("Mu"), BURROW_S_INIT("\xce\x9c")},
    {BURROW_S_INIT("Ntilde"), BURROW_S_INIT("\xc3\x91")},
    {BURROW_S_INIT("Nu"), BURROW_S_INIT("\xce\x9d")},
    {BURROW_S_INIT("OElig"), BURROW_S_INIT("\xc5\x92")},
    {BURROW_S_INIT("Oacute"), BURROW_S_INIT("\xc3\x93")},
    {BURROW_S_INIT("Ocirc"), BURROW_S_INIT("\xc3\x94")},
    {BURROW_S_INIT("Ograve"), BURROW_S_INIT("\xc3\x92")},
    {BURROW_S_INIT("Omega"), BURROW_S_INIT("\xce\xa9")},
    {BURROW_S_INIT("Omicron"), BURROW_S_INIT("\xce\x9f")},
    {BURROW_S_INIT("Oslash"), BURROW_S_INIT("\xc3\x98")},
    {BURROW_S_INIT("Otilde"), BURROW_S_INIT("\xc3\x95")},
    {BURROW_S_INIT("Ouml"), BURROW_S_INIT("\xc3\x96")},
    {BURROW_S_INIT("Phi"), BURROW_S_INIT("\xce\xa6")},
    {BURROW_S_INIT("Pi"), BURROW_S_INIT("\xce\xa0")},
    {BURROW_S_INIT("Prime"), BURROW_S_INIT("\xe2\x80\xb3")},
    {BURROW_S_INIT("Psi"), BURROW_S_INIT("\xce\xa8")},
    {BURROW_S_INIT("Rho"), BURROW_S_INIT("\xce\xa1")},
    {BURROW_S_INIT("Scaron"), BURROW_S_INIT("\xc5\xa0")},
    {BURROW_S_INIT("Sigma"), BURROW_S_INIT("\xce\xa3")},
    {BURROW_S_INIT("THORN"), BURROW_S_INIT("\xc3\x9e")},
    {BURROW_S_INIT("Tau"), BURROW_S_INIT("\xce\xa4")},
    {BURROW_S_INIT("Theta"), BURROW_S_INIT("\xce\x98")},
    {BURROW_S_INIT("Uacute"), BURROW_S_INIT("\xc3\x9a")},
    {BURROW_S_INIT("Ucirc"), BURROW_S_INIT("\xc3\x9b")},
    {BURROW_S_INIT("Ugrave"), BURROW_S_INIT("\xc3\x99")},
    {BURROW_S_INIT("Upsilon"), BURROW_S_INIT("\xce\xa5")},
    {BURROW_S_INIT("Uuml"), BURROW_S_INIT("\xc3\x9c")},
    {BURROW_S_INIT("Xi"), BURROW_S_INIT("\xce\x9e")},
    {BURROW_S_INIT("Yacute"), BURROW_S_INIT("\xc3\x9d")},
    {BURROW_S_INIT("Yuml"), BURROW_S_INIT("\xc5\xb8")},
    {BURROW_S_INIT("Zeta"), BURROW_S_INIT("\xce\x96")},
    {BURROW_S_INIT("aacute"), BURROW_S_INIT("\xc3\xa1")},
    {BURROW_S_INIT("acirc"), BURROW_S_INIT("\xc3\xa2")},
    {BURROW_S_INIT("acute"), BURROW_S_INIT("\xc2\xb4")},
    {BURROW_S_INIT("aelig"), BURROW_S_INIT("\xc3\xa6")},
    {BURROW_S_INIT("agrave"), BURROW_S_INIT("\xc3\xa0")},
    {BURROW_S_INIT("alefsym"), BURROW_S_INIT("\xe2\x84\xb5")},
    {BURROW_S_INIT("alpha"), BURROW_S_INIT("\xce\xb1")},
    {BURROW_S_INIT("amp"), BURROW_S_INIT("\x26")},
    {BURROW_S_INIT("and"), BURROW_S_INIT("\xe2\x88\xa7")},
    {BURROW_S_INIT("ang"), BURROW_S_INIT("\xe2\x88\xa0")},
    {BURROW_S_INIT("aring"), BURROW_S_INIT("\xc3\xa5")},
    {BURROW_S_INIT("asymp"), BURROW_S_INIT("\xe2\x89\x88")},
    {BURROW_S_INIT("atilde"), BURROW_S_INIT("\xc3\xa3")},
    {BURROW_S_INIT("auml"), BURROW_S_INIT("\xc3\xa4")},
    {BURROW_S_INIT("bdquo"), BURROW_S_INIT("\xe2\x80\x9e")},
    {BURROW_S_INIT("beta"), BURROW_S_INIT("\xce\xb2")},
    {BURROW_S_INIT("brvbar"), BURROW_S_INIT("\xc2\xa6")},
    {BURROW_S_INIT("bull"), BURROW_S_INIT("\xe2\x80\xa2")},
    {BURROW_S_INIT("cap"), BURROW_S_INIT("\xe2\x88\xa9")},
    {BURROW_S_INIT("ccedil"), BURROW_S_INIT("\xc3\xa7")},
    {BURROW_S_INIT("cedil"), BURROW_S_INIT("\xc2\xb8")},
    {BURROW_S_INIT("cent"), BURROW_S_INIT("\xc2\xa2")},
    {BURROW_S_INIT("chi"), BURROW_S_INIT("\xcf\x87")},
    {BURROW_S_INIT("circ"), BURROW_S_INIT("\xcb\x86")},
    {BURROW_S_INIT("clubs"), BURROW_S_INIT("\xe2\x99\xa3")},
    {BURROW_S_INIT("cong"), BURROW_S_INIT("\xe2\x89\x85")},
    {BURROW_S_INIT("copy"), BURROW_S_INIT("\xc2\xa9")},
    {BURROW_S_INIT("crarr"), BURROW_S_INIT("\xe2\x86\xb5")},
    {BURROW_S_INIT("cup"), BURROW_S_INIT("\xe2\x88\xaa")},
    {BURROW_S_INIT("curren"), BURROW_S_INIT("\xc2\xa4")},
    {BURROW_S_INIT("dArr"), BURROW_S_INIT("\xe2\x87\x93")},
    {BURROW_S_INIT("dagger"), BURROW_S_INIT("\xe2\x80\xa0")},
    {BURROW_S_INIT("darr"), BURROW_S_INIT("\xe2\x86\x93")},
    {BURROW_S_INIT("deg"), BURROW_S_INIT("\xc2\xb0")},
    {BURROW_S_INIT("delta"), BURROW_S_INIT("\xce\xb4")},
    {BURROW_S_INIT("diams"), BURROW_S_INIT("\xe2\x99\xa6")},
    {BURROW_S_INIT("divide"), BURROW_S_INIT("\xc3\xb7")},
    {BURROW_S_INIT("eacute"), BURROW_S_INIT("\xc3\xa9")},
    {BURROW_S_INIT("ecirc"), BURROW_S_INIT("\xc3\xaa")},
    {BURROW_S_INIT("egrave"), BURROW_S_INIT("\xc3\xa8")},
    {BURROW_S_INIT("empty"), BURROW_S_INIT("\xe2\x88\x85")},
    {BURROW_S_INIT("emsp"), BURROW_S_INIT("\xe2\x80\x83")},
    {BURROW_S_INIT("ensp"), BURROW_S_INIT("\xe2\x80\x82")},
    {BURROW_S_INIT("epsilon"), BURROW_S_INIT("\xce\xb5")},
    {BURROW_S_INIT("equiv"), BURROW_S_INIT("\xe2\x89\xa1")},
    {BURROW_S_INIT("eta"), BURROW_S_INIT("\xce\xb7")},
    {BURROW_S_INIT("eth"), BURROW_S_INIT("\xc3\xb0")},
    {BURROW_S_INIT("euml"), BURROW_S_INIT("\xc3\xab")},
    {BURROW_S_INIT("euro"), BURROW_S_INIT("\xe2\x82\xac")},
    {BURROW_S_INIT("exist"), BURROW_S_INIT("\xe2\x88\x83")},
    {BURROW_S_INIT("fnof"), BURROW_S_INIT("\xc6\x92")},
    {BURROW_S_INIT("forall"), BURROW_S_INIT("\xe2\x88\x80")},
    {BURROW_S_INIT("frac12"), BURROW_S_INIT("\xc2\xbd")},
    {BURROW_S_INIT("frac14"), BURROW_S_INIT("\xc2\xbc")},
    {BURROW_S_INIT("frac34"), BURROW_S_INIT("\xc2\xbe")},
    {BURROW_S_INIT("frasl"), BURROW_S_INIT("\xe2\x81\x84")},
    {BURROW_S_INIT("gamma"), BURROW_S_INIT("\xce\xb3")},
    {BURROW_S_INIT("ge"), BURROW_S_INIT("\xe2\x89\xa5")},
    {BURROW_S_INIT("gt"), BURROW_S_INIT("\x3e")},
    {BURROW_S_INIT("hArr"), BURROW_S_INIT("\xe2\x87\x94")},
    {BURROW_S_INIT("harr"), BURROW_S_INIT("\xe2\x86\x94")},
    {BURROW_S_INIT("hearts"), BURROW_S_INIT("\xe2\x99\xa5")},
    {BURROW_S_INIT("hellip"), BURROW_S_INIT("\xe2\x80\xa6")},
    {BURROW_S_INIT("iacute"), BURROW_S_INIT("\xc3\xad")},
    {BURROW_S_INIT("icirc"), BURROW_S_INIT("\xc3\xae")},
    {BURROW_S_INIT("iexcl"), BURROW_S_INIT("\xc2\xa1")},
    {BURROW_S_INIT("igrave"), BURROW_S_INIT("\xc3\xac")},
    {BURROW_S_INIT("image"), BURROW_S_INIT("\xe2\x84\x91")},
    {BURROW_S_INIT("infin"), BURROW_S_INIT("\xe2\x88\x9e")},
    {BURROW_S_INIT("int"), BURROW_S_INIT("\xe2\x88\xab")},
    {BURROW_S_INIT("iota"), BURROW_S_INIT("\xce\xb9")},
    {BURROW_S_INIT("iquest"), BURROW_S_INIT("\xc2\xbf")},
    {BURROW_S_INIT("isin"), BURROW_S_INIT("\xe2\x88\x88")},
    {BURROW_S_INIT("iuml"), BURROW_S_INIT("\xc3\xaf")},
    {BURROW_S_INIT("kappa"), BURROW_S_INIT("\xce\xba")},
    {BURROW_S_INIT("lArr"), BURROW_S_INIT("\xe2\x87\x90")},
    {BURROW_S_INIT("lambda"), BURROW_S_INIT("\xce\xbb")},
    {BURROW_S_INIT("lang"), BURROW_S_INIT("\xe2\x8c\xa9")},
    {BURROW_S_INIT("laquo"), BURROW_S_INIT("\xc2\xab")},
    {BURROW_S_INIT("larr"), BURROW_S_INIT("\xe2\x86\x90")},
    {BURROW_S_INIT("lceil"), BURROW_S_INIT("\xe2\x8c\x88")},
    {BURROW_S_INIT("ldquo"), BURROW_S_INIT("\xe2\x80\x9c")},
    {BURROW_S_INIT("le"), BURROW_S_INIT("\xe2\x89\xa4")},
    {BURROW_S_INIT("lfloor"), BURROW_S_INIT("\xe2\x8c\x8a")},
    {BURROW_S_INIT("lowast"), BURROW_S_INIT("\xe2\x88\x97")},
    {BURROW_S_INIT("loz"), BURROW_S_INIT("\xe2\x97\x8a")},
    {BURROW_S_INIT("lrm"), BURROW_S_INIT("\xe2\x80\x8e")},
    {BURROW_S_INIT("lsaquo"), BURROW_S_INIT("\xe2\x80\xb9")},
    {BURROW_S_INIT("lsquo"), BURROW_S_INIT("\xe2\x80\x98")},
    {BURROW_S_INIT("lt"), BURROW_S_INIT("\x3c")},
    {BURROW_S_INIT("macr"), BURROW_S_INIT("\xc2\xaf")},
    {BURROW_S_INIT("mdash"), BURROW_S_INIT("\xe2\x80\x94")},
    {BURROW_S_INIT("micro"), BURROW_S_INIT("\xc2\xb5")},
    {BURROW_S_INIT("middot"), BURROW_S_INIT("\xc2\xb7")},
    {BURROW_S_INIT("minus"), BURROW_S_INIT("\xe2\x88\x92")},
    {BURROW_S_INIT("mu"), BURROW_S_INIT("\xce\xbc")},
    {BURROW_S_INIT("nabla"), BURROW_S_INIT("\xe2\x88\x87")},
    {BURROW_S_INIT("nbsp"), BURROW_S_INIT("\xc2\xa0")},
    {BURROW_S_INIT("ndash"), BURROW_S_INIT("\xe2\x80\x93")},
    {BURROW_S_INIT("ne"), BURROW_S_INIT("\xe2\x89\xa0")},
    {BURROW_S_INIT("ni"), BURROW_S_INIT("\xe2\x88\x8b")},
    {BURROW_S_INIT("not"), BURROW_S_INIT("\xc2\xac")},
    {BURROW_S_INIT("notin"), BURROW_S_INIT("\xe2\x88\x89")},
    {BURROW_S_INIT("nsub"), BURROW_S_INIT("\xe2\x8a\x84")},
    {BURROW_S_INIT("ntilde"), BURROW_S_INIT("\xc3\xb1")},
    {BURROW_S_INIT("nu"), BURROW_S_INIT("\xce\xbd")},
    {BURROW_S_INIT("oacute"), BURROW_S_INIT("\xc3\xb3")},
    {BURROW_S_INIT("ocirc"), BURROW_S_INIT("\xc3\xb4")},
    {BURROW_S_INIT("oelig"), BURROW_S_INIT("\xc5\x93")},
    {BURROW_S_INIT("ograve"), BURROW_S_INIT("\xc3\xb2")},
    {BURROW_S_INIT("oline"), BURROW_S_INIT("\xe2\x80\xbe")},
    {BURROW_S_INIT("omega"), BURROW_S_INIT("\xcf\x89")},
    {BURROW_S_INIT("omicron"), BURROW_S_INIT("\xce\xbf")},
    {BURROW_S_INIT("oplus"), BURROW_S_INIT("\xe2\x8a\x95")},
    {BURROW_S_INIT("or"), BURROW_S_INIT("\xe2\x88\xa8")},
    {BURROW_S_INIT("ordf"), BURROW_S_INIT("\xc2\xaa")},
    {BURROW_S_INIT("ordm"), BURROW_S_INIT("\xc2\xba")},
    {BURROW_S_INIT("oslash"), BURROW_S_INIT("\xc3\xb8")},
    {BURROW_S_INIT("otilde"), BURROW_S_INIT("\xc3\xb5")},
    {BURROW_S_INIT("otimes"), BURROW_S_INIT("\xe2\x8a\x97")},
    {BURROW_S_INIT("ouml"), BURROW_S_INIT("\xc3\xb6")},
    {BURROW_S_INIT("para"), BURROW_S_INIT("\xc2\xb6")},
    {BURROW_S_INIT("part"), BURROW_S_INIT("\xe2\x88\x82")},
    {BURROW_S_INIT("permil"), BURROW_S_INIT("\xe2\x80\xb0")},
    {BURROW_S_INIT("perp"), BURROW_S_INIT("\xe2\x8a\xa5")},
    {BURROW_S_INIT("phi"), BURROW_S_INIT("\xcf\x86")},
    {BURROW_S_INIT("pi"), BURROW_S_INIT("\xcf\x80")},
    {BURROW_S_INIT("piv"), BURROW_S_INIT("\xcf\x96")},
    {BURROW_S_INIT("plusmn"), BURROW_S_INIT("\xc2\xb1")},
    {BURROW_S_INIT("pound"), BURROW_S_INIT("\xc2\xa3")},
    {BURROW_S_INIT("prime"), BURROW_S_INIT("\xe2\x80\xb2")},
    {BURROW_S_INIT("prod"), BURROW_S_INIT("\xe2\x88\x8f")},
    {BURROW_S_INIT("prop"), BURROW_S_INIT("\xe2\x88\x9d")},
    {BURROW_S_INIT("psi"), BURROW_S_INIT("\xcf\x88")},
    {BURROW_S_INIT("quot"), BURROW_S_INIT("\x22")},
    {BURROW_S_INIT("rArr"), BURROW_S_INIT("\xe2\x87\x92")},
    {BURROW_S_INIT("radic"), BURROW_S_INIT("\xe2\x88\x9a")},
    {BURROW_S_INIT("rang"), BURROW_S_INIT("\xe2\x8c\xaa")},
    {BURROW_S_INIT("raquo"), BURROW_S_INIT("\xc2\xbb")},
    {BURROW_S_INIT("rarr"), BURROW_S_INIT("\xe2\x86\x92")},
    {BURROW_S_INIT("rceil"), BURROW_S_INIT("\xe2\x8c\x89")},
    {BURROW_S_INIT("rdquo"), BURROW_S_INIT("\xe2\x80\x9d")},
    {BURROW_S_INIT("real"), BURROW_S_INIT("\xe2\x84\x9c")},
    {BURROW_S_INIT("reg"), BURROW_S_INIT("\xc2\xae")},
    {BURROW_S_INIT("rfloor"), BURROW_S_INIT("\xe2\x8c\x8b")},
    {BURROW_S_INIT("rho"), BURROW_S_INIT("\xcf\x81")},
    {BURROW_S_INIT("rlm"), BURROW_S_INIT("\xe2\x80\x8f")},
    {BURROW_S_INIT("rsaquo"), BURROW_S_INIT("\xe2\x80\xba")},
    {BURROW_S_INIT("rsquo"), BURROW_S_INIT("\xe2\x80\x99")},
    {BURROW_S_INIT("sbquo"), BURROW_S_INIT("\xe2\x80\x9a")},
    {BURROW_S_INIT("scaron"), BURROW_S_INIT("\xc5\xa1")},
    {BURROW_S_INIT("sdot"), BURROW_S_INIT("\xe2\x8b\x85")},
    {BURROW_S_INIT("sect"), BURROW_S_INIT("\xc2\xa7")},
    {BURROW_S_INIT("shy"), BURROW_S_INIT("\xc2\xad")},
    {BURROW_S_INIT("sigma"), BURROW_S_INIT("\xcf\x83")},
    {BURROW_S_INIT("sigmaf"), BURROW_S_INIT("\xcf\x82")},
    {BURROW_S_INIT("sim"), BURROW_S_INIT("\xe2\x88\xbc")},
    {BURROW_S_INIT("spades"), BURROW_S_INIT("\xe2\x99\xa0")},
    {BURROW_S_INIT("sub"), BURROW_S_INIT("\xe2\x8a\x82")},
    {BURROW_S_INIT("sube"), BURROW_S_INIT("\xe2\x8a\x86")},
    {BURROW_S_INIT("sum"), BURROW_S_INIT("\xe2\x88\x91")},
    {BURROW_S_INIT("sup"), BURROW_S_INIT("\xe2\x8a\x83")},
    {BURROW_S_INIT("sup1"), BURROW_S_INIT("\xc2\xb9")},
    {BURROW_S_INIT("sup2"), BURROW_S_INIT("\xc2\xb2")},
    {BURROW_S_INIT("sup3"), BURROW_S_INIT("\xc2\xb3")},
    {BURROW_S_INIT("supe"), BURROW_S_INIT("\xe2\x8a\x87")},
    {BURROW_S_INIT("szlig"), BURROW_S_INIT("\xc3\x9f")},
    {BURROW_S_INIT("tau"), BURROW_S_INIT("\xcf\x84")},
    {BURROW_S_INIT("there4"), BURROW_S_INIT("\xe2\x88\xb4")},
    {BURROW_S_INIT("theta"), BURROW_S_INIT("\xce\xb8")},
    {BURROW_S_INIT("thetasym"), BURROW_S_INIT("\xcf\x91")},
    {BURROW_S_INIT("thinsp"), BURROW_S_INIT("\xe2\x80\x89")},
    {BURROW_S_INIT("thorn"), BURROW_S_INIT("\xc3\xbe")},
    {BURROW_S_INIT("tilde"), BURROW_S_INIT("\xcb\x9c")},
    {BURROW_S_INIT("times"), BURROW_S_INIT("\xc3\x97")},
    {BURROW_S_INIT("trade"), BURROW_S_INIT("\xe2\x84\xa2")},
    {BURROW_S_INIT("uArr"), BURROW_S_INIT("\xe2\x87\x91")},
    {BURROW_S_INIT("uacute"), BURROW_S_INIT("\xc3\xba")},
    {BURROW_S_INIT("uarr"), BURROW_S_INIT("\xe2\x86\x91")},
    {BURROW_S_INIT("ucirc"), BURROW_S_INIT("\xc3\xbb")},
    {BURROW_S_INIT("ugrave"), BURROW_S_INIT("\xc3\xb9")},
    {BURROW_S_INIT("uml"), BURROW_S_INIT("\xc2\xa8")},
    {BURROW_S_INIT("upsih"), BURROW_S_INIT("\xcf\x92")},
    {BURROW_S_INIT("upsilon"), BURROW_S_INIT("\xcf\x85")},
    {BURROW_S_INIT("uuml"), BURROW_S_INIT("\xc3\xbc")},
    {BURROW_S_INIT("weierp"), BURROW_S_INIT("\xe2\x84\x98")},
    {BURROW_S_INIT("xi"), BURROW_S_INIT("\xce\xbe")},
    {BURROW_S_INIT("yacute"), BURROW_S_INIT("\xc3\xbd")},
    {BURROW_S_INIT("yen"), BURROW_S_INIT("\xc2\xa5")},
    {BURROW_S_INIT("yuml"), BURROW_S_INIT("\xc3\xbf")},
    {BURROW_S_INIT("zeta"), BURROW_S_INIT("\xce\xb6")},
    {BURROW_S_INIT("zwj"), BURROW_S_INIT("\xe2\x80\x8d")},
    {BURROW_S_INIT("zwnj"), BURROW_S_INIT("\xe2\x80\x8c")},
};

static const UnicodeRangeTable xml_first = {
    xml_first_r16, (Int)(sizeof xml_first_r16 / sizeof xml_first_r16[0]), NULL, 0, 0};

static const UnicodeRangeTable xml_second = {
    xml_second_r16, (Int)(sizeof xml_second_r16 / sizeof xml_second_r16[0]), NULL, 0,
    0};

static bool xml_is_name_byte(Byte c) {
    return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('0' <= c && c <= '9') ||
           c == '_' || c == ':' || c == '.' || c == '-';
}

static bool xml_is_name(Str s) {
    if (s.len == 0)
        return false;
    Int n;
    Rune c = utf8_decode_rune_in_string(s, &n);
    if (c == UTF8_RUNE_ERROR && n == 1)
        return false;
    if (!unicode_is(&xml_first, c))
        return false;
    while (n < s.len) {
        s = (Str){s.p + n, s.len - n};
        c = utf8_decode_rune_in_string(s, &n);
        if (c == UTF8_RUNE_ERROR && n == 1)
            return false;
        if (!unicode_is(&xml_first, c) && !unicode_is(&xml_second, c))
            return false;
    }
    return true;
}

bool burrow__xml_is_in_character_range(Rune r) {
    return r == 0x09 || r == 0x0A || r == 0x0D || (r >= 0x20 && r <= 0xD7FF) ||
           (r >= 0xE000 && r <= 0xFFFD) || (r >= 0x10000 && r <= 0x10FFFF);
}

/* readName: a name's bytes onto the end of buf. */
static bool xml_read_name(XmlDecoder *d) {
    Byte b;
    if (!xml_mustgetc(d, &b))
        return false;
    if (b < 0x80 && !xml_is_name_byte(b)) {
        xml_ungetc(d, b);
        return false;
    }
    xml_buf_put(d, b);
    for (;;) {
        if (!xml_mustgetc(d, &b))
            return false;
        if (b < 0x80 && !xml_is_name_byte(b)) {
            xml_ungetc(d, b);
            break;
        }
        xml_buf_put(d, b);
    }
    return true;
}

/* name: the next name, in the token arena. */
static bool xml_name(XmlDecoder *d, Str *out) {
    d->blen = 0;
    if (!xml_read_name(d))
        return false;
    Str b = {d->buf, d->blen};
    if (!xml_is_name(b)) {
        d->err = xml_syntax_error4(d, BURROW_S("invalid XML name: "), b, BURROW_S(""),
                                   BURROW_S(""));
        return false;
    }
    *out = xml_tok_str(d, b);
    return true;
}

/* nsname: a name, split at its colon into prefix and local part. */
static bool xml_nsname(XmlDecoder *d, XmlName *name) {
    Str s;
    if (!xml_name(d, &s))
        return false;
    *name = (XmlName){{NULL, 0}, {NULL, 0}};
    if (strings_count(s, BURROW_S(":")) > 1)
        return false;
    Str local;
    bool found;
    Str space = strings_cut(s, BURROW_S(":"), &local, &found);
    if (!found || space.len == 0 || local.len == 0) {
        name->local = s;
    } else {
        name->space = space;
        name->local = local;
    }
    return true;
}

/* ------------------------------------------------------------------- text */

static bool xml_builtin_entity(Str s, Byte *r) {
    static const struct {
        Str name;
        Byte r;
    } ents[] = {
        {BURROW_S_INIT("lt"), '<'},   {BURROW_S_INIT("gt"), '>'},
        {BURROW_S_INIT("amp"), '&'},  {BURROW_S_INIT("apos"), '\''},
        {BURROW_S_INIT("quot"), '"'},
    };
    for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++)
        if (str_eq(ents[i].name, s)) {
            *r = ents[i].r;
            return true;
        }
    return false;
}

/* text: character data up to the next <, or up to quote when quote is a
 * byte, or up to ]]> in a CDATA section, with entities replaced. The result
 * is in buf. */
static bool xml_text(XmlDecoder *d, int quote, bool cdata, Slice *out) {
    Byte b0 = 0, b1 = 0;
    Int trunc = 0;
    d->blen = 0;
    for (;;) {
        Byte b;
        if (!xml_getc(d, &b)) {
            if (cdata) {
                if (xml_same_error(d->err, io_eof))
                    d->err = XML_SYNTAX(d, "unexpected EOF in CDATA section");
                return false;
            }
            break;
        }

        /* <![CDATA[ section ends with ]]>.
         * It is an error for ]]> to appear in ordinary text,
         * but it is allowed in quoted strings. */
        if (quote < 0 && b0 == ']' && b1 == ']' && b == '>') {
            if (cdata) {
                trunc = 2;
                break;
            }
            d->err = XML_SYNTAX(d, "unescaped ]]> not in CDATA section");
            return false;
        }

        /* Stop reading text if we see a <. */
        if (b == '<' && !cdata) {
            if (quote >= 0) {
                d->err = XML_SYNTAX(d, "unescaped < inside quoted string");
                return false;
            }
            xml_ungetc(d, '<');
            break;
        }
        if (quote >= 0 && b == (Byte)quote)
            break;
        if (b == '&' && !cdata) {
            /* Read escaped character expression up to semicolon. XML in all
             * its glory allows a document to define and use its own character
             * names with <!ENTITY ...> directives. Parsers are required to
             * recognize lt, gt, amp, apos, and quot even if they have not been
             * declared. */
            Int before = d->blen;
            xml_buf_put(d, '&');
            Str text = {NULL, 0};
            Byte one[4];
            bool have_text = false;
            if (!xml_mustgetc(d, &b))
                return false;
            if (b == '#') {
                xml_buf_put(d, b);
                if (!xml_mustgetc(d, &b))
                    return false;
                Int base = 10;
                if (b == 'x') {
                    base = 16;
                    xml_buf_put(d, b);
                    if (!xml_mustgetc(d, &b))
                        return false;
                }
                Int start = d->blen;
                while (('0' <= b && b <= '9') || (base == 16 && 'a' <= b && b <= 'f') ||
                       (base == 16 && 'A' <= b && b <= 'F')) {
                    xml_buf_put(d, b);
                    if (!xml_mustgetc(d, &b))
                        return false;
                }
                if (b != ';') {
                    xml_ungetc(d, b);
                } else {
                    Error perr = BURROW_NO_ERROR;
                    uint64_t n = strconv_parse_uint(
                        (Str){d->buf + start, d->blen - start}, base, 64, &perr);
                    xml_buf_put(d, ';');
                    if (BURROW_OK(perr) && n <= (uint64_t)UNICODE_MAX_RUNE) {
                        Int w =
                            utf8_encode_rune((Slice){one, 4, 4, TYPE_BYTE}, (Rune)n);
                        text = (Str){one, w};
                        have_text = true;
                    }
                }
            } else {
                xml_ungetc(d, b);
                if (!xml_read_name(d)) {
                    if (BURROW_FAILED(d->err))
                        return false;
                }
                if (!xml_mustgetc(d, &b))
                    return false;
                if (b != ';') {
                    xml_ungetc(d, b);
                } else {
                    Int nlen = d->blen - (before + 1);
                    xml_buf_put(d, ';');
                    Str name = {d->buf + before + 1, nlen};
                    if (xml_is_name(name)) {
                        Byte r;
                        if (xml_builtin_entity(name, &r)) {
                            one[0] = r;
                            text = (Str){one, 1};
                            have_text = true;
                        } else if (d->entity != NULL) {
                            Str *v = BURROW_MAP_GET(Str, Str, d->entity, name);
                            if (v != NULL) {
                                text = *v;
                                have_text = true;
                            }
                        }
                    }
                }
            }

            if (have_text) {
                d->blen = before;
                xml_buf_write(d, text.p, text.len);
                b0 = 0;
                b1 = 0;
                continue;
            }
            if (!d->strict) {
                b0 = 0;
                b1 = 0;
                continue;
            }
            Str ent = {d->buf + before, d->blen - before};
            Str more =
                ent.p[ent.len - 1] != ';' ? BURROW_S(" (no semicolon)") : BURROW_S("");
            d->err = xml_syntax_error4(d, BURROW_S("invalid character entity "), ent,
                                       more, BURROW_S(""));
            return false;
        }

        /* We must rewrite unescaped \r and \r\n into \n. */
        if (b == '\r') {
            xml_buf_put(d, '\n');
        } else if (b1 == '\r' && b == '\n') {
            /* Skip \r\n--we already wrote \n. */
        } else {
            xml_buf_put(d, b);
        }

        b0 = b1;
        b1 = b;
    }
    Slice data = xml_buf_bytes(d, 0, d->blen - trunc);

    /* Inspect each rune for being a disallowed character. */
    Slice buf = data;
    while (buf.len > 0) {
        Int size;
        Rune r = utf8_decode_rune(buf, &size);
        if (r == UTF8_RUNE_ERROR && size == 1) {
            d->err = XML_SYNTAX(d, "invalid UTF-8");
            return false;
        }
        buf = (Slice){(Byte *)buf.p + size, buf.len - size, buf.len - size, TYPE_BYTE};
        if (!burrow__xml_is_in_character_range(r)) {
            /* "illegal character code %U", by hand. */
            static const char hex[] = "0123456789ABCDEF";
            Byte code[8];
            Int n = 0;
            for (int shift = r > 0xFFFF ? (r > 0xFFFFF ? 20 : 16) : 12; shift >= 0;
                 shift -= 4)
                code[n++] = (Byte)hex[(r >> shift) & 0xF];
            d->err = xml_syntax_error4(d, BURROW_S("illegal character code U+"),
                                       (Str){code, n}, BURROW_S(""), BURROW_S(""));
            return false;
        }
    }
    *out = data;
    return true;
}

/* attrval: an attribute's value, quoted or, when not strict, not. */
static bool xml_attrval(XmlDecoder *d, Slice *out) {
    Byte b;
    if (!xml_mustgetc(d, &b))
        return false;
    /* Handle quoted attribute values */
    if (b == '"' || b == '\'')
        return xml_text(d, b, false, out);
    /* Handle unquoted attribute values for strict parsers */
    if (d->strict) {
        d->err = XML_SYNTAX(d, "unquoted or missing attribute value in element");
        return false;
    }
    /* Handle unquoted attribute values for unstrict parsers */
    xml_ungetc(d, b);
    d->blen = 0;
    for (;;) {
        if (!xml_mustgetc(d, &b))
            return false;
        /* https://www.w3.org/TR/REC-html40/intro/sgmltut.html#h-3.2.2 */
        if (('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') ||
            ('0' <= b && b <= '9') || b == '_' || b == ':' || b == '-') {
            xml_buf_put(d, b);
        } else {
            xml_ungetc(d, b);
            break;
        }
    }
    *out = xml_buf_bytes(d, 0, d->blen);
    return true;
}

/* procInst: the value of param in a processing instruction such as
 * version="1.0", or empty. */
Str burrow__xml_proc_inst(Str param_name, Str s) {
    Byte pb[16];
    Int lenp = param_name.len + 1;
    memcpy(pb, param_name.p, (size_t)param_name.len);
    pb[param_name.len] = '=';
    Str param = {pb, lenp};
    Int i = 0;
    Byte sep = 0;
    while (i < s.len) {
        Str sub = {s.p + i, s.len - i};
        Int k = strings_index(sub, param);
        if (k < 0 || lenp + k >= sub.len)
            return (Str){NULL, 0};
        i += lenp + k + 1;
        Byte c = sub.p[lenp + k];
        if (c == '\'' || c == '"') {
            sep = c;
            break;
        }
    }
    if (sep == 0)
        return (Str){NULL, 0};
    Int j = strings_index_byte((Str){s.p + i, s.len - i}, sep);
    if (j < 0)
        return (Str){NULL, 0};
    return (Str){s.p + i, j};
}

/* ------------------------------------------------------------------ tokens */

#define XML_FAIL()                                                                     \
    do {                                                                               \
        *err = d->err;                                                                 \
        return (XmlToken){0};                                                          \
    } while (0)

/* Adds an attribute to the ones the start tag being read has. */
static bool xml_add_attr(XmlDecoder *d, XmlAttr a) {
    if (d->nattrs == d->capattrs) {
        Int cap = d->capattrs == 0 ? 4 : d->capattrs * 2;
        XmlAttr *na = (XmlAttr *)mem_realloc(
            d->a, d->attrs, (size_t)d->capattrs * sizeof(XmlAttr),
            (size_t)cap * sizeof(XmlAttr), _Alignof(XmlAttr));
        if (na == NULL) {
            xml_oom(d);
            return false;
        }
        d->attrs = na;
        d->capattrs = cap;
    }
    d->attrs[d->nattrs++] = a;
    return true;
}

static XmlToken xml_raw_token(XmlDecoder *d, Error *err) {
    *err = BURROW_NO_ERROR;
    if (d->t.vt != NULL)
        return xml_token_reader_token(d->t, err);
    if (BURROW_FAILED(d->err))
        XML_FAIL();
    if (d->need_close) {
        /* The last element we read was self-closing and
         * we returned just the StartElement half.
         * Return the EndElement half now. */
        d->need_close = false;
        XmlToken t = {XML_END_ELEMENT, .end = {d->to_close}};
        return t;
    }

    Byte b;
    if (!xml_getc(d, &b))
        XML_FAIL();

    if (b != '<') {
        /* Text section. */
        xml_ungetc(d, b);
        Slice data;
        if (!xml_text(d, -1, false, &data))
            XML_FAIL();
        return (XmlToken){XML_CHAR_DATA, .char_data = data};
    }

    if (!xml_mustgetc(d, &b))
        XML_FAIL();
    switch (b) {
    case '/': {
        /* </: End element */
        XmlName name;
        if (!xml_nsname(d, &name)) {
            if (BURROW_OK(d->err))
                d->err = XML_SYNTAX(d, "expected element name after </");
            XML_FAIL();
        }
        xml_space(d);
        if (!xml_mustgetc(d, &b))
            XML_FAIL();
        if (b != '>') {
            d->err = xml_syntax_error4(d, BURROW_S("invalid characters between </"),
                                       name.local, BURROW_S(" and >"), BURROW_S(""));
            XML_FAIL();
        }
        return (XmlToken){XML_END_ELEMENT, .end = {name}};
    }

    case '?': {
        /* <?: Processing instruction. */
        Str target;
        if (!xml_name(d, &target)) {
            if (BURROW_OK(d->err))
                d->err = XML_SYNTAX(d, "expected target name after <?");
            XML_FAIL();
        }
        xml_space(d);
        d->blen = 0;
        Byte b0 = 0;
        for (;;) {
            if (!xml_mustgetc(d, &b))
                XML_FAIL();
            xml_buf_put(d, b);
            if (b0 == '?' && b == '>')
                break;
            b0 = b;
        }
        Slice data = xml_buf_bytes(d, 0, d->blen - 2); /* chop ?> */

        if (str_eq(target, xml_prefix)) {
            Str content = {(const Byte *)data.p, data.len};
            Str ver = burrow__xml_proc_inst(BURROW_S("version"), content);
            if (ver.len > 0 && !str_eq(ver, BURROW_S("1.0"))) {
                d->err = fmt_errorf_v(
                    "xml: unsupported version %q; only version 1.0 is supported", ver);
                XML_FAIL();
            }
            Str enc = burrow__xml_proc_inst(BURROW_S("encoding"), content);
            if (enc.len > 0 && !str_eq(enc, BURROW_S("utf-8")) &&
                !str_eq(enc, BURROW_S("UTF-8")) &&
                !strings_equal_fold(enc, BURROW_S("utf-8"))) {
                if (d->charset_reader.f == NULL) {
                    d->err = fmt_errorf_v(
                        "xml: encoding %q declared but Decoder.CharsetReader is nil",
                        enc);
                    XML_FAIL();
                }
                enc = xml_tok_str(d, enc);
                Error cerr = BURROW_NO_ERROR;
                IoReader newr = BURROW_CALLF(d->charset_reader, enc, d->src, &cerr);
                if (BURROW_FAILED(cerr)) {
                    d->err = fmt_errorf_v("xml: opening charset %q: %w", enc, cerr);
                    XML_FAIL();
                }
                if (newr.vt == NULL) {
                    Str msg = fmt_sprintf_v(
                        error_allocator(),
                        "CharsetReader returned a nil Reader for charset %s", enc);
                    panic_str(msg);
                }
                if (!xml_switch_to_reader(d, newr)) {
                    d->err = burrow_err_out_of_memory;
                    XML_FAIL();
                }
            }
        }
        return (XmlToken){XML_PROC_INST, .proc_inst = {target, data}};
    }

    case '!':
        /* <!: Maybe comment, maybe CDATA. */
        if (!xml_mustgetc(d, &b))
            XML_FAIL();
        switch (b) {
        case '-': { /* <!- */
            /* Probably a comment. */
            if (!xml_mustgetc(d, &b))
                XML_FAIL();
            if (b != '-') {
                d->err = XML_SYNTAX(d, "invalid sequence <!- not part of <!--");
                XML_FAIL();
            }
            /* Look for terminator. */
            d->blen = 0;
            Byte b0 = 0, b1 = 0;
            for (;;) {
                if (!xml_mustgetc(d, &b))
                    XML_FAIL();
                xml_buf_put(d, b);
                if (b0 == '-' && b1 == '-') {
                    if (b != '>') {
                        d->err = XML_SYNTAX(
                            d, "invalid sequence \"--\" not allowed in comments");
                        XML_FAIL();
                    }
                    break;
                }
                b0 = b1;
                b1 = b;
            }
            Slice data = xml_buf_bytes(d, 0, d->blen - 3); /* chop --> */
            return (XmlToken){XML_COMMENT, .comment = data};
        }

        case '[': { /* <![ */
            /* Probably a CDATA section. */
            for (int i = 0; i < 6; i++) {
                if (!xml_mustgetc(d, &b))
                    XML_FAIL();
                if (b != (Byte) "CDATA["[i]) {
                    d->err = XML_SYNTAX(d, "invalid <![ sequence");
                    XML_FAIL();
                }
            }
            /* Have <![CDATA[.  Read text until ]]>. */
            Slice data;
            if (!xml_text(d, -1, true, &data))
                XML_FAIL();
            return (XmlToken){XML_CHAR_DATA, .char_data = data};
        }
        default:
            break;
        }

        /* Probably a directive: <!DOCTYPE ...>, <!ENTITY ...>, etc.
         * We don't care, but accumulate for caller. Quoted angle
         * brackets do not count for nesting. */
        d->blen = 0;
        xml_buf_put(d, b);
        {
            Byte inquote = 0;
            int depth = 0;
            for (;;) {
                if (!xml_mustgetc(d, &b))
                    XML_FAIL();
                if (inquote == 0 && b == '>' && depth == 0)
                    break;
            handle_b:
                xml_buf_put(d, b);
                if (b == inquote) {
                    inquote = 0;
                } else if (inquote != 0) {
                    /* in quotes, no special action */
                } else if (b == '\'' || b == '"') {
                    inquote = b;
                } else if (b == '>' && inquote == 0) {
                    depth--;
                } else if (b == '<' && inquote == 0) {
                    /* Look for <!-- to begin comment. */
                    static const char s[] = "!--";
                    for (int i = 0; i < 3; i++) {
                        if (!xml_mustgetc(d, &b))
                            XML_FAIL();
                        if (b != (Byte)s[i]) {
                            for (int j = 0; j < i; j++)
                                xml_buf_put(d, (Byte)s[j]);
                            depth++;
                            goto handle_b;
                        }
                    }

                    /* Remove < that was written above. */
                    d->blen--;

                    /* Look for terminator. */
                    Byte b0 = 0, b1 = 0;
                    for (;;) {
                        if (!xml_mustgetc(d, &b))
                            XML_FAIL();
                        if (b0 == '-' && b1 == '-' && b == '>')
                            break;
                        b0 = b1;
                        b1 = b;
                    }

                    /* Replace the comment with a space in the returned Directive
                     * body, so that markup parts that were separated by the comment
                     * (like a "<" and a "!") don't get joined when re-encoding the
                     * Directive, taking new semantic meaning. */
                    xml_buf_put(d, ' ');
                }
            }
        }
        return (XmlToken){XML_DIRECTIVE, .directive = xml_buf_bytes(d, 0, d->blen)};

    default:
        break;
    }

    /* Must be an open element like <a href="foo"> */
    xml_ungetc(d, b);

    XmlName name;
    bool empty = false;
    if (!xml_nsname(d, &name)) {
        if (BURROW_OK(d->err))
            d->err = XML_SYNTAX(d, "expected element name after <");
        XML_FAIL();
    }

    d->nattrs = 0;
    for (;;) {
        xml_space(d);
        if (!xml_mustgetc(d, &b))
            XML_FAIL();
        if (b == '/') {
            empty = true;
            if (!xml_mustgetc(d, &b))
                XML_FAIL();
            if (b != '>') {
                d->err = XML_SYNTAX(d, "expected /> in element");
                XML_FAIL();
            }
            break;
        }
        if (b == '>')
            break;
        xml_ungetc(d, b);

        XmlAttr a = {{{NULL, 0}, {NULL, 0}}, {NULL, 0}};
        if (!xml_nsname(d, &a.name)) {
            if (BURROW_OK(d->err))
                d->err = XML_SYNTAX(d, "expected attribute name in element");
            XML_FAIL();
        }
        xml_space(d);
        if (!xml_mustgetc(d, &b))
            XML_FAIL();
        if (b != '=') {
            if (d->strict) {
                d->err = XML_SYNTAX(d, "attribute name without = in element");
                XML_FAIL();
            }
            xml_ungetc(d, b);
            a.value = a.name.local;
        } else {
            xml_space(d);
            Slice data;
            if (!xml_attrval(d, &data))
                XML_FAIL();
            a.value = xml_tok_str(d, (Str){(const Byte *)data.p, data.len});
        }
        xml_add_attr(d, a);
    }
    if (empty) {
        d->need_close = true;
        xml_set_to_close(d, name);
    }
    XmlToken t = {
        XML_START_ELEMENT,
        .start = {name, {d->attrs, d->nattrs, d->nattrs, &burrow_type_XmlAttr}}};
    return t;
}

/* An allocation that failed part way through a call spoils what the call
 * made, so the call fails instead. */
static XmlToken xml_check_oom(XmlDecoder *d, XmlToken t, Error *err) {
    if (BURROW_UNLIKELY(d->oom)) {
        d->oom = false;
        d->err = burrow_err_out_of_memory;
        *err = d->err;
        return (XmlToken){0};
    }
    return t;
}

XmlToken xml_decoder_raw_token(XmlDecoder *d, Error *err) {
    arena_reset(&d->tok);
    XmlToken t = xml_raw_token(d, err);
    return xml_check_oom(d, t, err);
}

static XmlToken xml_token(XmlDecoder *d, Error *err) {
    XmlToken t;
    *err = BURROW_NO_ERROR;
    if (d->stk != NULL && d->stk->kind == STK_EOF) {
        *err = io_eof;
        return (XmlToken){0};
    }
    if (d->next_token.kind != XML_TOKEN_NONE) {
        t = d->next_token;
        d->next_token = (XmlToken){0};
    } else {
        Error e = BURROW_NO_ERROR;
        t = xml_raw_token(d, &e);
        if (t.kind == XML_TOKEN_NONE && BURROW_FAILED(e)) {
            if (xml_same_error(e, io_eof) && d->stk != NULL && d->stk->kind != STK_EOF)
                e = XML_SYNTAX(d, "unexpected EOF");
            *err = e;
            return (XmlToken){0};
        }
    }
    if (!d->strict) {
        XmlToken t1;
        if (xml_auto_close(d, t, &t1)) {
            xml_save_next(d, t);
            t = t1;
        }
    }
    switch (t.kind) {
    case XML_START_ELEMENT: {
        /* A token from a TokenReader belongs to it, so the translation is
         * done on a copy of its attributes. */
        if (d->t.vt != NULL && t.start.attr.len > 0) {
            const XmlAttr *src = (const XmlAttr *)t.start.attr.p;
            Int n = t.start.attr.len;
            d->nattrs = 0;
            for (Int i = 0; i < n; i++)
                if (!xml_add_attr(d, src[i]))
                    break;
            t.start.attr =
                (Slice){d->attrs, d->nattrs, d->nattrs, &burrow_type_XmlAttr};
        }
        /* In XML name spaces, the translations listed in the
         * attributes apply to the element name and
         * to the other attribute names, so process
         * the translations first. */
        XmlAttr *at = (XmlAttr *)t.start.attr.p;
        for (Int i = 0; i < t.start.attr.len; i++) {
            if (str_eq(at[i].name.space, xmlns_prefix))
                xml_declare(d, at[i].name.local, at[i].value);
            if (at[i].name.space.len == 0 && str_eq(at[i].name.local, xmlns_prefix))
                xml_declare(d, (Str){NULL, 0}, at[i].value);
        }

        xml_push_element(d, t.start.name);
        xml_translate(d, &t.start.name, true);
        for (Int i = 0; i < t.start.attr.len; i++)
            xml_translate(d, &at[i].name, false);
        break;
    }

    case XML_END_ELEMENT:
        if (!xml_pop_element(d, &t.end)) {
            *err = d->err;
            return (XmlToken){0};
        }
        break;

    case XML_TOKEN_NONE:
    case XML_CHAR_DATA:
    case XML_COMMENT:
    case XML_PROC_INST:
    case XML_DIRECTIVE:
    default:
        break;
    }
    return t;
}

XmlToken xml_decoder_token(XmlDecoder *d, Error *err) {
    arena_reset(&d->tok);
    XmlToken t = xml_token(d, err);
    return xml_check_oom(d, t, err);
}

Error xml_decoder_skip(XmlDecoder *d) {
    int64_t depth = 0;
    for (;;) {
        Error err;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        switch (tok.kind) {
        case XML_START_ELEMENT:
            depth++;
            break;
        case XML_END_ELEMENT:
            if (depth == 0)
                return BURROW_NO_ERROR;
            depth--;
            break;
        case XML_TOKEN_NONE:
        case XML_CHAR_DATA:
        case XML_COMMENT:
        case XML_PROC_INST:
        case XML_DIRECTIVE:
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------ HTML helpers */

static Map *xml_html_entity_map;
static SyncOnce xml_html_entity_once;

static void xml_html_entity_init(void *env) {
    (void)env;
    Int n = (Int)(sizeof xml_html_entities / sizeof xml_html_entities[0]);
    Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_STRING, n);
    if (m == NULL)
        return;
    for (Int i = 0; i < n; i++) {
        if (!BURROW_MAP_SET(Str, Str, m, xml_html_entities[i].name,
                            xml_html_entities[i].value)) {
            map_free(m);
            return;
        }
    }
    xml_html_entity_map = m;
}

Map *xml_html_entity(void) {
    sync_once_do(&xml_html_entity_once, BURROW_FN(Func, xml_html_entity_init, NULL));
    return xml_html_entity_map;
}

static const Str xml_html_auto_close_names[] = {
    BURROW_S_INIT("basefont"), BURROW_S_INIT("br"),      BURROW_S_INIT("area"),
    BURROW_S_INIT("link"),     BURROW_S_INIT("img"),     BURROW_S_INIT("param"),
    BURROW_S_INIT("hr"),       BURROW_S_INIT("input"),   BURROW_S_INIT("col"),
    BURROW_S_INIT("frame"),    BURROW_S_INIT("isindex"), BURROW_S_INIT("base"),
    BURROW_S_INIT("meta"),
};

const Slice xml_html_auto_close = {(void *)(uintptr_t)xml_html_auto_close_names, 13, 13,
                                   TYPE_STRING};

/* ---------------------------------------------------------------- escaping */

/* Where escaped text goes: an IoWriter for EscapeText, the encoder's buffer
 * for the encoder. */
typedef Error (*XmlPut)(void *ctx, const Byte *p, Int n);

static const Str esc_quot = BURROW_S_INIT("&#34;"); /* shorter than "&quot;" */
static const Str esc_apos = BURROW_S_INIT("&#39;"); /* shorter than "&apos;" */
static const Str esc_amp = BURROW_S_INIT("&amp;");
static const Str esc_lt = BURROW_S_INIT("&lt;");
static const Str esc_gt = BURROW_S_INIT("&gt;");
static const Str esc_tab = BURROW_S_INIT("&#x9;");
static const Str esc_nl = BURROW_S_INIT("&#xA;");
static const Str esc_cr = BURROW_S_INIT("&#xD;");
static const Str esc_fffd =
    BURROW_S_INIT("\xef\xbf\xbd"); /* Unicode replacement character */

/* escapeText, which EscapeString is too, with escape_newline true. */
static Error xml_escape_to(XmlPut put, void *ctx, const Byte *s, Int len,
                           bool escape_newline) {
    Str esc;
    Int last = 0;
    for (Int i = 0; i < len;) {
        /* Most text is ASCII that needs nothing, so step over that a byte at
         * a time without decoding. Control characters, the five that have
         * entities and anything past ASCII stop the scan. */
        while (i < len && s[i] >= 0x20 && s[i] < 0x80 && s[i] != '"' && s[i] != '\'' &&
               s[i] != '&' && s[i] != '<' && s[i] != '>')
            i++;
        if (i == len)
            break;
        Int width;
        Rune r;
        if (s[i] < 0x80) {
            r = s[i];
            width = 1;
        } else {
            r = utf8_decode_rune(
                (Slice){(void *)(uintptr_t)(s + i), len - i, len - i, TYPE_BYTE},
                &width);
        }
        i += width;
        switch (r) {
        case '"':
            esc = esc_quot;
            break;
        case '\'':
            esc = esc_apos;
            break;
        case '&':
            esc = esc_amp;
            break;
        case '<':
            esc = esc_lt;
            break;
        case '>':
            esc = esc_gt;
            break;
        case '\t':
            esc = esc_tab;
            break;
        case '\n':
            if (!escape_newline)
                continue;
            esc = esc_nl;
            break;
        case '\r':
            esc = esc_cr;
            break;
        default:
            if (!burrow__xml_is_in_character_range(r) || (r == 0xFFFD && width == 1)) {
                esc = esc_fffd;
                break;
            }
            continue;
        }
        Error err = put(ctx, s + last, i - width - last);
        if (BURROW_FAILED(err))
            return err;
        err = put(ctx, esc.p, esc.len);
        if (BURROW_FAILED(err))
            return err;
        last = i;
    }
    return put(ctx, s + last, len - last);
}

static Error xml_put_writer(void *ctx, const Byte *p, Int n) {
    IoWriter *w = (IoWriter *)ctx;
    Error err = BURROW_NO_ERROR;
    w->vt->write(w->data, (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE}, &err);
    return err;
}

Error xml_escape_text(IoWriter w, Slice s) {
    return xml_escape_to(xml_put_writer, &w, (const Byte *)s.p, s.len, true);
}

void xml_escape(IoWriter w, Slice s) {
    (void)xml_escape_text(w, s);
}

/* ---------------------------------------------------------------- encoding */

/* A prefix createAttrPrefix made, and the URL it stands for, in one block.
 * An entry with no block is markPrefix's mark. */
typedef struct XmlPrefix {
    Byte *block;
    Str prefix;
    Str url;
} XmlPrefix;

typedef struct XmlTag {
    Int off;
    Int space_len;
    Int local_len;
} XmlTag;

struct XmlEncoder {
    Alloc *a;
    BufioWriter *w;
    Int seq;
    Str indent;
    Str prefix;
    Int depth;
    bool indented_in;
    bool put_newline;
    Map *attr_ns;     /* prefix to URL */
    Map *attr_prefix; /* URL to prefix */
    XmlPrefix *prefixes;
    Int nprefixes, capprefixes;
    /* The open tags, a stack of offsets into tag_bytes, where each one's
     * space and then its local name are kept. */
    XmlTag *tags;
    Int ntags, captags;
    Byte *tag_bytes;
    Int ntag_bytes, captag_bytes;
    bool closed;
    Error err;
};

static const Type xml_encoder_desc = {
    {(const Byte *)"Encoder", 7},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlEncoder),
    (uint16_t)_Alignof(XmlEncoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x786d6565U, /* "xmee" */
    NULL,
};

const Type *const TYPE_XML_ENCODER = &xml_encoder_desc;

XmlEncoder *xml_new_encoder(Alloc *a, IoWriter w) {
    if (a == NULL)
        a = heap_allocator();
    XmlEncoder *e = (XmlEncoder *)mem_alloc(a, sizeof *e, _Alignof(XmlEncoder));
    if (e == NULL)
        return NULL;
    e->a = a;
    e->w = bufio_new_writer(a, w);
    if (e->w == NULL) {
        mem_free(a, e, sizeof *e, _Alignof(XmlEncoder));
        return NULL;
    }
    return e;
}

static void xml_enc_str_free(XmlEncoder *e, Str s) {
    if (s.len > 0)
        mem_free(e->a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* Open tag i, pointing into tag_bytes, so good until the next push. */
static XmlName xml_enc_tag(const XmlEncoder *e, Int i) {
    XmlTag t = e->tags[i];
    const Byte *p = e->tag_bytes + t.off;
    return (XmlName){{p, t.space_len}, {p + t.space_len, t.local_len}};
}

static void xml_enc_prefix_free(XmlEncoder *e, XmlPrefix p) {
    if (p.block != NULL)
        mem_free(e->a, p.block, (size_t)p.prefix.len + (size_t)p.url.len, 1);
}

void xml_encoder_free(XmlEncoder *e) {
    if (e == NULL)
        return;
    Alloc *a = e->a;
    bufio_writer_free(e->w);
    xml_enc_str_free(e, e->indent);
    xml_enc_str_free(e, e->prefix);
    for (Int i = 0; i < e->nprefixes; i++)
        xml_enc_prefix_free(e, e->prefixes[i]);
    if (e->capprefixes > 0)
        mem_free(a, e->prefixes, (size_t)e->capprefixes * sizeof(XmlPrefix),
                 _Alignof(XmlPrefix));
    if (e->captags > 0)
        mem_free(a, e->tags, (size_t)e->captags * sizeof(XmlTag), _Alignof(XmlTag));
    if (e->captag_bytes > 0)
        mem_free(a, e->tag_bytes, (size_t)e->captag_bytes, 1);
    map_free(e->attr_ns);
    map_free(e->attr_prefix);
    mem_free(a, e, sizeof *e, _Alignof(XmlEncoder));
}

static Str xml_enc_clone(XmlEncoder *e, Str s) {
    if (s.len == 0)
        return (Str){NULL, 0};
    Byte *p = (Byte *)mem_alloc_nozero(e->a, (size_t)s.len, 1);
    if (p == NULL)
        return (Str){NULL, 0};
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

void xml_encoder_indent(XmlEncoder *e, Str prefix, Str indent) {
    xml_enc_str_free(e, e->prefix);
    xml_enc_str_free(e, e->indent);
    e->prefix = xml_enc_clone(e, prefix);
    e->indent = xml_enc_clone(e, indent);
}

/* The printer's Write, WriteString and WriteByte, which keep the first
 * error. */
static bool xml_enc_live(XmlEncoder *e) {
    if (e->closed && BURROW_OK(e->err))
        e->err = errors_new(error_allocator(), BURROW_S("use of closed Encoder"));
    return BURROW_OK(e->err);
}

static void xml_enc_write(XmlEncoder *e, const Byte *p, Int n) {
    if (xml_enc_live(e))
        bufio_writer_write(e->w, (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE},
                           &e->err);
}

static void xml_enc_write_str(XmlEncoder *e, Str s) {
    if (xml_enc_live(e))
        bufio_writer_write_string(e->w, s, &e->err);
}

static void xml_enc_write_byte(XmlEncoder *e, Byte c) {
    if (xml_enc_live(e))
        e->err = bufio_writer_write_byte(e->w, c);
}

static Error xml_enc_cached_write_error(XmlEncoder *e) {
    xml_enc_live(e);
    return e->err;
}

static Error xml_put_encoder(void *ctx, const Byte *p, Int n) {
    XmlEncoder *e = (XmlEncoder *)ctx;
    xml_enc_write(e, p, n);
    return e->err;
}

/* EscapeString. */
static void xml_enc_escape_string(XmlEncoder *e, Str s) {
    (void)xml_escape_to(xml_put_encoder, e, s.p, s.len, true);
}

static void xml_enc_write_indent(XmlEncoder *e, int depth_delta) {
    if (e->prefix.len == 0 && e->indent.len == 0)
        return;
    if (depth_delta < 0) {
        e->depth--;
        if (e->indented_in) {
            e->indented_in = false;
            return;
        }
        e->indented_in = false;
    }
    if (e->put_newline)
        xml_enc_write_byte(e, '\n');
    else
        e->put_newline = true;
    if (e->prefix.len > 0)
        xml_enc_write_str(e, e->prefix);
    if (e->indent.len > 0)
        for (Int i = 0; i < e->depth; i++)
            xml_enc_write_str(e, e->indent);
    if (depth_delta > 0) {
        e->depth++;
        e->indented_in = true;
    }
}

static bool xml_enc_push_prefix(XmlEncoder *e, XmlPrefix p) {
    if (e->nprefixes == e->capprefixes) {
        Int cap = e->capprefixes == 0 ? 8 : e->capprefixes * 2;
        XmlPrefix *np = (XmlPrefix *)mem_realloc(
            e->a, e->prefixes, (size_t)e->capprefixes * sizeof(XmlPrefix),
            (size_t)cap * sizeof(XmlPrefix), _Alignof(XmlPrefix));
        if (np == NULL)
            return false;
        e->prefixes = np;
        e->capprefixes = cap;
    }
    e->prefixes[e->nprefixes++] = p;
    return true;
}

static Str xml_enc_map_get(Map *m, Str k) {
    if (m == NULL)
        return (Str){NULL, 0};
    Str *v = BURROW_MAP_GET(Str, Str, m, k);
    return v != NULL ? *v : (Str){NULL, 0};
}

/* createAttrPrefix: the prefix that stands for url in an attribute name,
 * declaring it first when it is new. */
static Str xml_enc_create_attr_prefix(XmlEncoder *e, Str url) {
    /* Pick a name. We try to use the final element of the path
     * but fall back to _. */
    Str have = xml_enc_map_get(e->attr_prefix, url);
    if (have.len > 0)
        return have;

    /* The "http://www.w3.org/XML/1998/namespace" name space is predefined as
     * "xml" and does not need to be declared. */
    if (str_eq(url, xml_url))
        return xml_prefix;

    if (e->attr_prefix == NULL) {
        e->attr_prefix = map_make(e->a, TYPE_STRING, TYPE_STRING, 0);
        e->attr_ns = map_make(e->a, TYPE_STRING, TYPE_STRING, 0);
        if (e->attr_prefix == NULL || e->attr_ns == NULL) {
            e->err = burrow_err_out_of_memory;
            return (Str){NULL, 0};
        }
    }

    Str prefix = strings_trim_right(url, BURROW_S("/"));
    Int i = strings_last_index(prefix, BURROW_S("/"));
    if (i >= 0)
        prefix = (Str){prefix.p + i + 1, prefix.len - i - 1};
    bool underscore = false;
    if (prefix.len == 0 || !xml_is_name(prefix) || strings_index_byte(prefix, ':') >= 0)
        prefix = BURROW_S("_");
    /* xmlanything is reserved and any variant of it regardless of case should
     * be matched, so:
     *    (('X'|'x') ('M'|'m') ('L'|'l'))
     * See Section 2.3 of https://www.w3.org/TR/REC-xml/ */
    else if (prefix.len >= 3 && strings_equal_fold((Str){prefix.p, 3}, BURROW_S("xml")))
        underscore = true;

    /* The prefix, and a number if that is taken, and the URL after it, all in
     * one block. */
    Byte num[24];
    Int numlen = 0;
    Int base = prefix.len + (underscore ? 1 : 0);
    Byte *tmp = (Byte *)mem_alloc_nozero(e->a, (size_t)(base + 1 + 24), 1);
    if (tmp == NULL) {
        e->err = burrow_err_out_of_memory;
        return (Str){NULL, 0};
    }
    Int tl = 0;
    if (underscore)
        tmp[tl++] = '_';
    memcpy(tmp + tl, prefix.p, (size_t)prefix.len);
    tl += prefix.len;
    Str cand = {tmp, tl};
    if (xml_enc_map_get(e->attr_ns, cand).len > 0) {
        for (e->seq++;; e->seq++) {
            Slice ns =
                strconv_append_int(NULL, (Slice){num, 0, 24, TYPE_BYTE}, e->seq, 10);
            numlen = ns.len;
            if (ns.p != num)
                memcpy(num, ns.p, (size_t)numlen);
            tmp[tl] = '_';
            memcpy(tmp + tl + 1, num, (size_t)numlen);
            cand = (Str){tmp, tl + 1 + numlen};
            if (xml_enc_map_get(e->attr_ns, cand).len == 0)
                break;
        }
    }
    Byte *block = (Byte *)mem_alloc_nozero(e->a, (size_t)(cand.len + url.len), 1);
    if (block == NULL) {
        mem_free(e->a, tmp, (size_t)(base + 1 + 24), 1);
        e->err = burrow_err_out_of_memory;
        return (Str){NULL, 0};
    }
    memcpy(block, cand.p, (size_t)cand.len);
    memcpy(block + cand.len, url.p, (size_t)url.len);
    mem_free(e->a, tmp, (size_t)(base + 1 + 24), 1);
    XmlPrefix p = {block, {block, cand.len}, {block + cand.len, url.len}};

    if (!xml_enc_push_prefix(e, p) ||
        !BURROW_MAP_SET(Str, Str, e->attr_prefix, p.url, p.prefix) ||
        !BURROW_MAP_SET(Str, Str, e->attr_ns, p.prefix, p.url)) {
        e->err = burrow_err_out_of_memory;
        return p.prefix;
    }

    xml_enc_write_str(e, BURROW_S("xmlns:"));
    xml_enc_write_str(e, p.prefix);
    xml_enc_write_str(e, BURROW_S("=\""));
    (void)xml_escape_to(xml_put_encoder, e, p.url.p, p.url.len, true);
    xml_enc_write_str(e, BURROW_S("\" "));

    return p.prefix;
}

/* deleteAttrPrefix. */
static void xml_enc_delete_attr_prefix(XmlEncoder *e, Str prefix) {
    Str url = xml_enc_map_get(e->attr_ns, prefix);
    BURROW_MAP_DEL(Str, e->attr_prefix, url);
    BURROW_MAP_DEL(Str, e->attr_ns, prefix);
}

static void xml_enc_mark_prefix(XmlEncoder *e) {
    if (!xml_enc_push_prefix(e, (XmlPrefix){NULL, {NULL, 0}, {NULL, 0}}))
        e->err = burrow_err_out_of_memory;
}

static void xml_enc_pop_prefix(XmlEncoder *e) {
    while (e->nprefixes > 0) {
        XmlPrefix p = e->prefixes[--e->nprefixes];
        if (p.block == NULL)
            break;
        xml_enc_delete_attr_prefix(e, p.prefix);
        xml_enc_prefix_free(e, p);
    }
}

/* writeStart. */
static Error xml_enc_write_start(XmlEncoder *e, const XmlStartElement *start) {
    if (start->name.local.len == 0)
        return errors_new(error_allocator(), BURROW_S("xml: start tag with no name"));

    if (e->ntags == e->captags) {
        Int cap = e->captags == 0 ? 8 : e->captags * 2;
        XmlTag *nt =
            (XmlTag *)mem_realloc(e->a, e->tags, (size_t)e->captags * sizeof(XmlTag),
                                  (size_t)cap * sizeof(XmlTag), _Alignof(XmlTag));
        if (nt == NULL)
            return burrow_err_out_of_memory;
        e->tags = nt;
        e->captags = cap;
    }
    Int need = e->ntag_bytes + start->name.space.len + start->name.local.len;
    if (need > e->captag_bytes) {
        Int cap = e->captag_bytes == 0 ? 256 : e->captag_bytes * 2;
        while (cap < need)
            cap *= 2;
        Byte *nb = (Byte *)mem_realloc(e->a, e->tag_bytes, (size_t)e->captag_bytes,
                                       (size_t)cap, 1);
        if (nb == NULL)
            return burrow_err_out_of_memory;
        e->tag_bytes = nb;
        e->captag_bytes = cap;
    }
    Byte *p = e->tag_bytes + e->ntag_bytes;
    xml_put_str(&p, start->name.space);
    xml_put_str(&p, start->name.local);
    e->tags[e->ntags++] =
        (XmlTag){e->ntag_bytes, start->name.space.len, start->name.local.len};
    e->ntag_bytes = need;
    xml_enc_mark_prefix(e);

    xml_enc_write_indent(e, 1);
    xml_enc_write_byte(e, '<');
    xml_enc_write_str(e, start->name.local);

    if (start->name.space.len > 0) {
        xml_enc_write_str(e, BURROW_S(" xmlns=\""));
        xml_enc_escape_string(e, start->name.space);
        xml_enc_write_byte(e, '"');
    }

    /* Attributes */
    const XmlAttr *at = (const XmlAttr *)start->attr.p;
    for (Int i = 0; i < start->attr.len; i++) {
        XmlName name = at[i].name;
        if (name.local.len == 0)
            continue;
        xml_enc_write_byte(e, ' ');
        if (name.space.len > 0) {
            xml_enc_write_str(e, xml_enc_create_attr_prefix(e, name.space));
            xml_enc_write_byte(e, ':');
        }
        xml_enc_write_str(e, name.local);
        xml_enc_write_str(e, BURROW_S("=\""));
        xml_enc_escape_string(e, at[i].value);
        xml_enc_write_byte(e, '"');
    }
    xml_enc_write_byte(e, '>');
    return BURROW_NO_ERROR;
}

/* writeEnd. */
static Error xml_enc_write_end(XmlEncoder *e, XmlName name) {
    if (name.local.len == 0)
        return errors_new(error_allocator(), BURROW_S("xml: end tag with no name"));
    if (e->ntags == 0)
        return fmt_errorf_v("xml: end tag </%s> without start tag", name.local);
    XmlName top = xml_enc_tag(e, e->ntags - 1);
    if (!str_eq(top.local, name.local) || !str_eq(top.space, name.space)) {
        if (!str_eq(top.local, name.local))
            return fmt_errorf_v("xml: end tag </%s> does not match start tag <%s>",
                                name.local, top.local);
        return fmt_errorf_v("xml: end tag </%s> in namespace %s does not match start "
                            "tag <%s> in namespace %s",
                            name.local, name.space, top.local, top.space);
    }
    e->ntags--;
    e->ntag_bytes = e->tags[e->ntags].off;

    xml_enc_write_indent(e, -1);
    xml_enc_write_byte(e, '<');
    xml_enc_write_byte(e, '/');
    xml_enc_write_str(e, name.local);
    xml_enc_write_byte(e, '>');
    xml_enc_pop_prefix(e);
    return BURROW_NO_ERROR;
}

static Slice xml_lit(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

/* isValidDirective reports whether dir is a valid directive text,
 * meaning angle brackets are matched, ignoring comments and strings. */
bool burrow__xml_is_valid_directive(Slice dir) {
    int depth = 0;
    Byte inquote = 0;
    bool incomment = false;
    const Byte *c = (const Byte *)dir.p;
    for (Int i = 0; i < dir.len; i++) {
        if (incomment) {
            if (c[i] == '>') {
                Int n = 1 + i - 3;
                if (n >= 0 && memcmp(c + n, "-->", 3) == 0)
                    incomment = false;
            }
            /* Just ignore anything in comment */
        } else if (inquote != 0) {
            if (c[i] == inquote)
                inquote = 0;
            /* Just ignore anything within quotes */
        } else if (c[i] == '\'' || c[i] == '"') {
            inquote = c[i];
        } else if (c[i] == '<') {
            if (i + 4 < dir.len && memcmp(c + i, "<!--", 4) == 0)
                incomment = true;
            else
                depth++;
        } else if (c[i] == '>') {
            if (depth == 0)
                return false;
            depth--;
        }
    }
    return depth == 0 && inquote == 0 && !incomment;
}

Error xml_encoder_encode_token(XmlEncoder *e, XmlToken t) {
    Error err;
    switch (t.kind) {
    case XML_START_ELEMENT:
        err = xml_enc_write_start(e, &t.start);
        if (BURROW_FAILED(err))
            return err;
        break;
    case XML_END_ELEMENT:
        err = xml_enc_write_end(e, t.end.name);
        if (BURROW_FAILED(err))
            return err;
        break;
    case XML_CHAR_DATA:
        (void)xml_escape_to(xml_put_encoder, e, (const Byte *)t.char_data.p,
                            t.char_data.len, false);
        break;
    case XML_COMMENT:
        if (bytes_contains(t.comment, xml_lit(BURROW_S("-->"))))
            return errors_new(
                error_allocator(),
                BURROW_S("xml: EncodeToken of Comment containing --> marker"));
        xml_enc_write_str(e, BURROW_S("<!--"));
        xml_enc_write(e, (const Byte *)t.comment.p, t.comment.len);
        xml_enc_write_str(e, BURROW_S("-->"));
        return xml_enc_cached_write_error(e);
    case XML_PROC_INST:
        /* First token to be encoded which is also a ProcInst with target of
         * xml is the xml declaration. The only ProcInst where target of xml
         * is allowed. */
        if (str_eq(t.proc_inst.target, xml_prefix) && bufio_writer_buffered(e->w) != 0)
            return errors_new(
                error_allocator(),
                BURROW_S("xml: EncodeToken of ProcInst xml target only valid for "
                         "xml declaration, first token encoded"));
        if (!xml_is_name(t.proc_inst.target))
            return errors_new(
                error_allocator(),
                BURROW_S("xml: EncodeToken of ProcInst with invalid Target"));
        if (bytes_contains(t.proc_inst.inst, xml_lit(BURROW_S("?>"))))
            return errors_new(
                error_allocator(),
                BURROW_S("xml: EncodeToken of ProcInst containing ?> marker"));
        xml_enc_write_str(e, BURROW_S("<?"));
        xml_enc_write_str(e, t.proc_inst.target);
        if (t.proc_inst.inst.len > 0) {
            xml_enc_write_byte(e, ' ');
            xml_enc_write(e, (const Byte *)t.proc_inst.inst.p, t.proc_inst.inst.len);
        }
        xml_enc_write_str(e, BURROW_S("?>"));
        break;
    case XML_DIRECTIVE:
        if (!burrow__xml_is_valid_directive(t.directive))
            return errors_new(
                error_allocator(),
                BURROW_S(
                    "xml: EncodeToken of Directive containing wrong < or > markers"));
        xml_enc_write_str(e, BURROW_S("<!"));
        xml_enc_write(e, (const Byte *)t.directive.p, t.directive.len);
        xml_enc_write_str(e, BURROW_S(">"));
        break;
    case XML_TOKEN_NONE:
    default:
        return errors_new(error_allocator(),
                          BURROW_S("xml: EncodeToken of invalid token type"));
    }
    return xml_enc_cached_write_error(e);
}

Error xml_encoder_flush(XmlEncoder *e) {
    return bufio_writer_flush(e->w);
}

Error xml_encoder_close(XmlEncoder *e) {
    if (e->closed)
        return BURROW_NO_ERROR;
    e->closed = true;
    Error err = bufio_writer_flush(e->w);
    if (BURROW_FAILED(err))
        return err;
    if (e->ntags > 0) {
        Str local = xml_enc_tag(e, e->ntags - 1).local;
        return fmt_errorf_v("unclosed tag <%s>", local);
    }
    return BURROW_NO_ERROR;
}
