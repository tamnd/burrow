/* Derived from Go's src/encoding/xml/marshal_test.go, the tests that go
 * through Marshal. The values in Go's marshalTests, marshalErrorTests and
 * marshalIndentTests that need no methods come from the generated header;
 * the rest are ported by hand below.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "../src/encoding/jsonv2_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/strings.h"

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S BURROW_S
#define LEN(x) (sizeof(x) / sizeof((x)[0]))

static void *gen_alloc(Alloc *a, const Type *t) {
    return mem_alloc(a, t->size > 0 ? t->size : 1, t->align > 0 ? t->align : 1);
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
/* A value left out of the table can leave its type's descriptor unused. */
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "encoding_xml_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, s == NULL ? 0 : (Int)strlen(s)};
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : S("<nil>");
}

#include "json_dump.h"

static bool err_matches(Error err, const char *want) {
    if (want == NULL)
        return BURROW_OK(err);
    return BURROW_FAILED(err) && str_eq(error_text(err), cstr(want));
}

/* ------------------------------------------------------- generated cases */

static void TestMarshalGenerated(TestingT *t) {
    gen_init();
    for (size_t i = 0; i < LEN(x_cases); i++) {
        const XCase *c = &x_cases[i];
        if (getenv("XML_TRACE") != NULL)
            fprintf(stderr, "%s\n", c->name);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Any in = {NULL, NULL};
        if (c->type != NULL) {
            void *v = gen_alloc(a, c->type);
            c->make(a, v);
            in = BURROW_ANY(c->type, v);
        }
        Error err = BURROW_NO_ERROR;
        Slice out = c->indent ? xml_marshal_indent(a, in, qstr(c->prefix),
                                                   qstr(c->indent_str), &err)
                              : xml_marshal(a, in, &err);
        Str got = {(const Byte *)out.p, out.len};
        if (!str_eq(got, qstr(c->out)))
            testing_t_errorf_v(t, "%s: output = %q, want %q", c->name, got,
                               qstr(c->out));
        if (!err_matches(err, c->err))
            testing_t_errorf_v(t, "%s: error = %q, want %q", c->name, err_str(err),
                               cstr(c->err == NULL ? "<nil>" : c->err));
        arena_free(&ar);
    }
}

/* Go's TestUnmarshal: each value's XML read back into a new value of its
 * type, compared with what Go's Unmarshal made of it by the dump the json
 * tests use. */
static void TestUnmarshalGenerated(TestingT *t) {
    gen_init();
    for (size_t i = 0; i < LEN(u_cases); i++) {
        const UCase *c = &u_cases[i];
        if (getenv("XML_TRACE") != NULL)
            fprintf(stderr, "%s\n", c->name);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        void *v = gen_alloc(a, c->type);
        if (c->make != NULL)
            c->make(a, v);
        Slice in = {(void *)(uintptr_t)c->in.p, (Int)c->in.n, (Int)c->in.n, TYPE_BYTE};
        Error err;
        if (c->ns.n > 0) {
            BytesReader r;
            bytes_reader_reset(&r, in);
            XmlDecoder *d = xml_new_decoder(a, bytes_reader_as_io_reader(&r));
            d->default_space = (Str){(const Byte *)c->ns.p, (Int)c->ns.n};
            err = xml_decoder_decode(d, BURROW_ANY(c->type, v));
            xml_decoder_free(d);
        } else {
            err = xml_unmarshal(a, in, BURROW_ANY(c->type, v));
        }
        if (!err_matches(err, c->err))
            testing_t_errorf_v(t, "%s: error = %q, want %q", c->name, err_str(err),
                               cstr(c->err == NULL ? "<nil>" : c->err));
        JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
        DumpPath path;
        path.n = 0;
        dump(&b, c->type, v, &path);
        Str got = {b.p, b.len};
        if (!str_eq(got, cstr(c->dump)))
            testing_t_errorf_v(t, "%s: value = %s, want %s", c->name, got,
                               cstr(c->dump));
        burrow__jsonbuf_free(&b);
        arena_free(&ar);
    }
}

/* -------------------------------------------------------- hand-ported cases */

static Slice sbytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static void check_marshal(TestingT *t, const char *name, Any v, const char *want,
                          const char *want_err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice out = xml_marshal(a, v, &err);
    Str got = {(const Byte *)out.p, out.len};
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "%s: output = %q, want %q", name, got, cstr(want));
    if (!err_matches(err, want_err))
        testing_t_errorf_v(t, "%s: error = %q, want %q", name, err_str(err),
                           cstr(want_err == NULL ? "<nil>" : want_err));
    arena_free(&ar);
}

/* MyMarshalerTest writes itself with MarshalXML. Go's has no fields and C
 * needs one, so it has an unexported one that marshaling skips. */
#define MY_MARSHALER_FIELDS(F, T) F(T, Int, pad, "")
BURROW_STRUCT_DECL(MyMarshalerTest, MY_MARSHALER_FIELDS);

static Error my_marshal_xml(MyMarshalerTest *m, XmlEncoder *e, XmlStartElement start) {
    (void)m;
    xml_encoder_encode_token(e, (XmlToken){.kind = XML_START_ELEMENT, .start = start});
    xml_encoder_encode_token(
        e, (XmlToken){.kind = XML_CHAR_DATA, .char_data = sbytes(S("hello world"))});
    xml_encoder_encode_token(
        e, (XmlToken){.kind = XML_END_ELEMENT, .end = xml_start_element_end(start)});
    return BURROW_NO_ERROR;
}

#define MY_MARSHALER_METHODS(M, T) M(T, MarshalXML, my_marshal_xml, XML_SIG_MARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(MyMarshalerTest, MY_MARSHALER_FIELDS,
                             MY_MARSHALER_METHODS);

#define MY_MARSHALER_ATTR_FIELDS(F, T) F(T, Int, pad, "")
BURROW_STRUCT_DECL(MyMarshalerAttrTest, MY_MARSHALER_ATTR_FIELDS);

static XmlAttr my_marshal_xml_attr(MyMarshalerAttrTest *m, Alloc *a, XmlName name,
                                   Error *err) {
    (void)m;
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (XmlAttr){name, S("hello world")};
}

#define MY_MARSHALER_ATTR_METHODS(M, T)                                                \
    M(T, MarshalXMLAttr, my_marshal_xml_attr, XML_SIG_MARSHAL_XML_ATTR)
BURROW_STRUCT_DEFINE_METHODS(MyMarshalerAttrTest, MY_MARSHALER_ATTR_FIELDS,
                             MY_MARSHALER_ATTR_METHODS);

#define MARSHALER_STRUCT_FIELDS(F, T) F(T, MyMarshalerAttrTest, Foo, "xml:\",attr\"")
BURROW_STRUCT(MarshalerStruct, MARSHALER_STRUCT_FIELDS);

/* Go's Service has fields Domain *Domain and Port *Port. A field named after
 * the type it points at reads as embedded in C, so the C types get an
 * underscore. The element names come from XMLName, so the output is Go's. */
#define PORT_FIELDS(F, T)                                                              \
    F(T, XmlName, XMLName, "xml:\"port\"")                                             \
    F(T, Str, Type, "xml:\"type,attr,omitempty\"")                                     \
    F(T, Str, Comment, "xml:\",comment\"")                                             \
    F(T, Str, Number, "xml:\",chardata\"")
BURROW_STRUCT(Port_, PORT_FIELDS);
BURROW_PTR_TYPE(PortPtr, Port_);

#define DOMAIN_FIELDS(F, T)                                                            \
    F(T, XmlName, XMLName, "xml:\"domain\"")                                           \
    F(T, Str, Country, "xml:\",attr,omitempty\"")                                      \
    F(T, Bytes, Name, "xml:\",chardata\"")                                             \
    F(T, Bytes, Comment, "xml:\",comment\"")
BURROW_STRUCT(Domain_, DOMAIN_FIELDS);
BURROW_PTR_TYPE(DomainPtr, Domain_);

#define SERVICE_FIELDS(F, T)                                                           \
    F(T, XmlName, XMLName, "xml:\"service\"")                                          \
    F(T, DomainPtr, Domain, "xml:\"host>domain\"")                                     \
    F(T, PortPtr, Port, "xml:\"host>port\"")                                           \
    F(T, Any, Extra1, "")                                                              \
    F(T, Any, Extra2, "xml:\"host>extra2\"")
BURROW_STRUCT(Service, SERVICE_FIELDS);

#define EMBEDC_FIELDS(F, T)                                                            \
    F(T, Str, FieldA1, "xml:\"FieldA>A1\"")                                            \
    F(T, Str, FieldA2, "xml:\"FieldA>A2\"")                                            \
    F(T, Str, FieldB, "")                                                              \
    F(T, Str, FieldC, "")
BURROW_STRUCT(EmbedC, EMBEDC_FIELDS);
BURROW_PTR_TYPE(EmbedCPtr, EmbedC);

#define EMBEDB_FIELDS(F, T)                                                            \
    F(T, Str, FieldB, "")                                                              \
    F(T, EmbedCPtr, EmbedC, "")
BURROW_STRUCT(EmbedB, EMBEDB_FIELDS);

static void TestMarshalMethods(TestingT *t) {
    MyMarshalerTest m = {0};
    check_marshal(t, "MyMarshalerTest", BURROW_ANY(TYPE_OF(MyMarshalerTest), &m),
                  "<MyMarshalerTest>hello world</MyMarshalerTest>", NULL);
    MarshalerStruct ms = {{0}};
    check_marshal(t, "MarshalerStruct", BURROW_ANY(TYPE_OF(MarshalerStruct), &ms),
                  "<MarshalerStruct Foo=\"hello world\"></MarshalerStruct>", NULL);
}

static void TestMarshalService(TestingT *t) {
    Port_ port = {.Number = S("80")};
    Str sa = S("A"), sb = S("B"), sex = S("example");

    Service s1 = {.Port = &port};
    check_marshal(t, "Service port", BURROW_ANY(TYPE_OF(Service), &s1),
                  "<service><host><port>80</port></host></service>", NULL);

    Service s2 = {0};
    check_marshal(t, "Service empty", BURROW_ANY(TYPE_OF(Service), &s2),
                  "<service></service>", NULL);

    Service s3 = {.Port = &port,
                  .Extra1 = BURROW_ANY(TYPE_STRING, &sa),
                  .Extra2 = BURROW_ANY(TYPE_STRING, &sb)};
    check_marshal(t, "Service extras", BURROW_ANY(TYPE_OF(Service), &s3),
                  "<service><host><port>80</port></host><Extra1>A</Extra1>"
                  "<host><extra2>B</extra2></host></service>",
                  NULL);

    Service s4 = {.Port = &port, .Extra2 = BURROW_ANY(TYPE_STRING, &sex)};
    check_marshal(t, "Service extra2", BURROW_ANY(TYPE_OF(Service), &s4),
                  "<service><host><port>80</port></host>"
                  "<host><extra2>example</extra2></host></service>",
                  NULL);
}

/* Anonymous struct pointer field which is nil. */
static void TestMarshalNilEmbedded(TestingT *t) {
    EmbedB b = {0};
    check_marshal(t, "EmbedB", BURROW_ANY(TYPE_OF(EmbedB), &b),
                  "<EmbedB><FieldB></FieldB></EmbedB>", NULL);
}

/* ---------------------------------------------------------------- errors */

typedef void *ChanBool;
static const Type burrow_type_ChanBool = {
    {NULL, 0},
    {NULL, 0},
    KIND_CHAN,
    (uint32_t)sizeof(ChanBool),
    _Alignof(ChanBool),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_bool,
    NULL,
    0,
    0,
    NULL,
};

BURROW_MAP_TYPE(StrStrMap, Str, Str);

#define CONFLICT_FIELDS(F, T)                                                          \
    F(T, Str, A, "xml:\"x>y\"")                                                        \
    F(T, Str, B, "xml:\"x\"")
BURROW_STRUCT(Conflict, CONFLICT_FIELDS);

static void TestMarshalErrors(TestingT *t) {
    /* Go's marshalErrorTests[0], left out of the generated table because Go
     * cannot write a channel literal into it. */
    int dummy = 0;
    ChanBool ch = &dummy;
    Error err = BURROW_NO_ERROR;
    Slice out = xml_marshal(heap_allocator(), BURROW_ANY(TYPE_OF(ChanBool), &ch), &err);
    if (out.p != NULL || !err_matches(err, "xml: unsupported type: chan bool"))
        testing_t_errorf_v(t, "chan: error = %q", err_str(err));
    const XmlUnsupportedTypeError *u = (const XmlUnsupportedTypeError *)errors_as(
        err, TYPE_XML_UNSUPPORTED_TYPE_ERROR);
    if (u == NULL || u->type->kind != KIND_CHAN)
        testing_t_errorf_v(t, "chan: errors_as did not find the kind");

    StrStrMap m = NULL;
    out = xml_marshal(heap_allocator(), BURROW_ANY(TYPE_OF(StrStrMap), &m), &err);
    u = (const XmlUnsupportedTypeError *)errors_as(err,
                                                   TYPE_XML_UNSUPPORTED_TYPE_ERROR);
    if (out.p != NULL || u == NULL || u->type->kind != KIND_MAP)
        testing_t_errorf_v(t, "map: error = %q", err_str(err));

    Conflict c = {0};
    out = xml_marshal(heap_allocator(), BURROW_ANY(TYPE_OF(Conflict), &c), &err);
    if (out.p != NULL ||
        !err_matches(err, "Conflict field \"A\" with tag \"x>y\" conflicts with field "
                          "\"B\" with tag \"x\""))
        testing_t_errorf_v(t, "conflict: error = %q", err_str(err));
    const XmlTagPathError *pe =
        (const XmlTagPathError *)errors_as(err, TYPE_XML_TAG_PATH_ERROR);
    if (pe == NULL || pe->struct_type != TYPE_OF(Conflict) ||
        !str_eq(pe->field1, S("A")) || !str_eq(pe->tag2, S("x")))
        testing_t_errorf_v(t, "conflict: errors_as did not find the TagPathError");
}

/* A MarshalXML that leaves an element open. */
#define OPEN_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Open, OPEN_FIELDS);

static Error open_marshal_xml(Open *o, XmlEncoder *e, XmlStartElement start) {
    (void)o;
    XmlStartElement inner = {{{0}, S("inner")}, {0}};
    xml_encoder_encode_token(e, (XmlToken){.kind = XML_START_ELEMENT, .start = start});
    xml_encoder_encode_token(e, (XmlToken){.kind = XML_START_ELEMENT, .start = inner});
    xml_encoder_encode_token(
        e, (XmlToken){.kind = XML_END_ELEMENT, .end = xml_start_element_end(inner)});
    return BURROW_NO_ERROR;
}

#define OPEN_METHODS(M, T) M(T, MarshalXML, open_marshal_xml, XML_SIG_MARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(Open, OPEN_FIELDS, OPEN_METHODS);

static void TestMarshalNotClosed(TestingT *t) {
    Open o = {0};
    check_marshal(t, "Open", BURROW_ANY(TYPE_OF(Open), &o), "",
                  "xml: (*Open).MarshalXML wrote invalid XML: <Open> not closed");
}

/* Go's Example_customMarshalXML: a MarshalXML on a named int that writes
 * itself with EncodeElement. */
typedef Int Animal;

static Error animal_marshal_xml(Animal *a, XmlEncoder *e, XmlStartElement start) {
    Str s = S("unknown");
    if (*a == 1)
        s = S("gopher");
    else if (*a == 2)
        s = S("zebra");
    return xml_encoder_encode_element(e, BURROW_ANY(TYPE_STRING, &s), start);
}

#define ANIMAL_METHODS(M, T) M(T, MarshalXML, animal_marshal_xml, XML_SIG_MARSHAL_XML)
BURROW_METHODS_DEFINE(Animal, ANIMAL_METHODS);

static const Type burrow_type_Animal = {
    BURROW_S_INIT("Animal"),
    {NULL, 0},
    KIND_INT,
    (uint32_t)sizeof(Animal),
    (uint16_t)_Alignof(Animal),
    0,
    (uint16_t)(sizeof burrow__methods_Animal / sizeof burrow__methods_Animal[0]),
    NULL,
    burrow__methods_Animal,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

BURROW_SLICE_TYPE(Animals, Animal);

#define ZOO_FIELDS(F, T)                                                               \
    F(T, XmlName, XMLName, "xml:\"zoo\"")                                              \
    F(T, Animals, Animals, "xml:\"animal\"")
BURROW_STRUCT(Zoo, ZOO_FIELDS);

static void TestEncodeElementFromMarshalXML(TestingT *t) {
    Animal list[] = {1, 0, 2};
    Zoo z = {.Animals = slice_from(list, 3, 3, TYPE_OF(Animal))};
    check_marshal(t, "Zoo", BURROW_ANY(TYPE_OF(Zoo), &z),
                  "<zoo><animal>gopher</animal><animal>unknown</animal>"
                  "<animal>zebra</animal></zoo>",
                  NULL);
}

static void TestEncodeElement(TestingT *t) {
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    XmlEncoder *e = xml_new_encoder(heap_allocator(), bytes_buffer_as_io_writer(&b));
    Str v = S("x<y");
    XmlAttr attrs[] = {{{{0}, S("k")}, S("v")}};
    XmlStartElement start = {{{0}, S("item")},
                             slice_from(attrs, 1, 1, TYPE_OF(XmlAttr))};
    Error err = xml_encoder_encode_element(e, BURROW_ANY(TYPE_STRING, &v), start);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "EncodeElement: %q", err_str(err));
    err = xml_encoder_close(e);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close: %q", err_str(err));
    Slice got = bytes_buffer_bytes(&b);
    if (!str_eq(str_from_bytes(got.p, got.len), S("<item k=\"v\">x&lt;y</item>")))
        testing_t_errorf_v(t, "EncodeElement wrote %q", str_from_bytes(got.p, got.len));
    xml_encoder_free(e);
    bytes_buffer_free(&b);
}

/* --------------------------------------------------------- write errors */

BURROW_SLICE_TYPE(Strs, Str);

#define PASSENGER_FIELDS(F, T)                                                         \
    F(T, Strs, Name, "xml:\"name\"")                                                   \
    F(T, float, Weight, "xml:\"weight\"")
BURROW_STRUCT(Passenger, PASSENGER_FIELDS);

typedef struct LimitedWriter {
    BytesBuffer *w;
    Int remain;
} LimitedWriter;

static Int limited_write(void *self, Slice p, Error *err) {
    LimitedWriter *lw = self;
    if (lw->remain <= 0) {
        *err = errors_new(error_allocator(), S("write limit hit"));
        return 0;
    }
    if (p.len > lw->remain) {
        p.len = lw->remain;
        Int n = bytes_buffer_write(lw->w, p, err);
        lw->remain = 0;
        *err = errors_new(error_allocator(), S("write limit hit"));
        return n;
    }
    Int n = bytes_buffer_write(lw->w, p, err);
    lw->remain -= n;
    return n;
}

static const IoWriterVT limited_writer_vt = {NULL, limited_write};

static void TestMarshalWriteErrors(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    enum { write_cap = 1024, n = 4000 };
    LimitedWriter lw = {&buf, write_cap};
    XmlEncoder *enc =
        xml_new_encoder(heap_allocator(), (IoWriter){&limited_writer_vt, &lw});
    Str names[] = {S("Alice"), S("Bob")};
    Error err = BURROW_NO_ERROR;
    int i;
    for (i = 1; i <= n; i++) {
        Passenger p = {slice_from(names, 2, 2, TYPE_STRING), 5};
        err = xml_encoder_encode(enc, BURROW_ANY(TYPE_OF(Passenger), &p));
        if (BURROW_FAILED(err))
            break;
    }
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected an error");
    if (i == n)
        testing_t_errorf_v(t, "expected to fail before the end");
    if (bytes_buffer_len(&buf) != write_cap)
        testing_t_errorf_v(t, "buf.Len() = %d; want %d", bytes_buffer_len(&buf),
                           (Int)write_cap);
    xml_encoder_free(enc);
    bytes_buffer_free(&buf);
}

static Int err_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = errors_new(error_allocator(), S("unwritable"));
    return 0;
}

static const IoWriterVT err_writer_vt = {NULL, err_write};

static void TestMarshalWriteIOErrors(TestingT *t) {
    XmlEncoder *enc =
        xml_new_encoder(heap_allocator(), (IoWriter){&err_writer_vt, NULL});
    Passenger p = {0};
    Error err = xml_encoder_encode(enc, BURROW_ANY(TYPE_OF(Passenger), &p));
    if (!err_matches(err, "unwritable"))
        testing_t_errorf_v(t, "EscapeTest = [error] %q, want unwritable", err_str(err));
    xml_encoder_free(enc);
}

/* ------------------------------------------------------------ issues */

/* golang.org/issue/6556 */
#define SPA_FIELDS(F, T)                                                               \
    F(T, Str, XMLName, "xml:\"a\"")                                                    \
    F(T, Anys, B, "")
BURROW_SLICE_TYPE(Anys, Any);
BURROW_STRUCT(StructPtrA, SPA_FIELDS);

#define SPC_FIELDS(F, T)                                                               \
    F(T, XmlName, XMLName, "")                                                         \
    F(T, Str, Value, "xml:\"value\"")
BURROW_STRUCT(StructPtrC, SPC_FIELDS);
BURROW_PTR_TYPE(StructPtrCPtr, StructPtrC);

static void TestStructPointerMarshal(TestingT *t) {
    StructPtrC c = {{{0}, S("c")}, S("x")};
    StructPtrCPtr cp = &c;
    Any items[] = {BURROW_ANY(TYPE_OF(StructPtrCPtr), &cp)};
    StructPtrA a = {{0}, slice_from(items, 1, 1, TYPE_OF(Any))};
    check_marshal(t, "StructPointerMarshal", BURROW_ANY(TYPE_OF(StructPtrA), &a),
                  "<a><c><value>x</value></c></a>", NULL);
}

/* Issue 20953. Crash on invalid XMLName attribute. */
#define IXN_TYPE_FIELDS(F, T) F(T, XmlName, XMLName, "xml:\"type,attr\"")
BURROW_STRUCT(InvalidXMLNameType, IXN_TYPE_FIELDS);

#define IXN_FIELDS(F, T)                                                               \
    F(T, XmlName, XMLName, "xml:\"error\"")                                            \
    F(T, InvalidXMLNameType, Type, "")
BURROW_STRUCT(InvalidXMLName, IXN_FIELDS);

static void TestInvalidXMLName(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    XmlEncoder *enc =
        xml_new_encoder(heap_allocator(), bytes_buffer_as_io_writer(&buf));
    InvalidXMLName v = {0};
    Error err = xml_encoder_encode(enc, BURROW_ANY(TYPE_OF(InvalidXMLName), &v));
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "unexpected success");
    else if (!strings_contains(error_text(err), S("invalid tag")))
        testing_t_errorf_v(t, "error %q does not contain \"invalid tag\"",
                           error_text(err));
    xml_encoder_free(enc);
    bytes_buffer_free(&buf);
}

/* Issue 50164. Crash on zero value XML attribute. Go gets the value by
 * unmarshaling proofXml, which C cannot do yet, so it is built by hand. */
BURROW_PTR_TYPE(Float64Ptr, double);
BURROW_PTR_TYPE(IntPtr, Int);

#define LAYER_TWO_FIELDS(F, T)                                                         \
    F(T, IntPtr, ValueTwo, "xml:\"value_two,attr,omitempty\"")
BURROW_STRUCT(LayerTwo, LAYER_TWO_FIELDS);
BURROW_PTR_TYPE(LayerTwoPtr, LayerTwo);

#define LAYER_ONE_FIELDS(F, T)                                                         \
    F(T, XmlName, XMLName, "xml:\"l1\"")                                               \
    F(T, Float64Ptr, Value, "xml:\"value,omitempty\"")                                 \
    F(T, LayerTwoPtr, LayerTwo, "xml:\",omitempty\"")
BURROW_STRUCT(LayerOne, LAYER_ONE_FIELDS);

static void TestMarshalZeroValue(TestingT *t) {
    double f = 1.2345;
    LayerOne l1 = {.Value = &f};
    check_marshal(t, "LayerOne", BURROW_ANY(TYPE_OF(LayerOne), &l1),
                  "<l1><value>1.2345</value></l1>", NULL);
}

/* TestEncodeXMLNS in xml_test.go. */
#define XMLNS1_FIELDS(F, T)                                                            \
    F(T, XmlName, XMLName, "xml:\"Test\"")                                             \
    F(T, Str, Ns, "xml:\"xmlns,attr\"")                                                \
    F(T, Str, Body, "")
BURROW_STRUCT(XmlnsT1, XMLNS1_FIELDS);

#define XMLNS2_FIELDS(F, T) F(T, Str, Body, "xml:\"http://example.com/ns body\"")
BURROW_STRUCT(Test, XMLNS2_FIELDS);

#define XMLNS3_FIELDS(F, T)                                                            \
    F(T, XmlName, XMLName, "xml:\"http://example.com/ns Test\"")                       \
    F(T, Str, Body, "")
BURROW_STRUCT(XmlnsT3, XMLNS3_FIELDS);

#define XMLNS4_FIELDS(F, T)                                                            \
    F(T, Str, Ns, "xml:\"xmlns,attr\"")                                                \
    F(T, Str, Body, "")
BURROW_STRUCT(XmlnsT4, XMLNS4_FIELDS);

static void TestEncodeXMLNS(TestingT *t) {
    const char *want =
        "<Test xmlns=\"http://example.com/ns\"><Body>hello world</Body></Test>";
    XmlnsT1 v1 = {.Ns = S("http://example.com/ns"), .Body = S("hello world")};
    check_marshal(t, "encodeXMLNS1", BURROW_ANY(TYPE_OF(XmlnsT1), &v1), want, NULL);
    Test v2 = {S("hello world")};
    check_marshal(
        t, "encodeXMLNS2", BURROW_ANY(TYPE_OF(Test), &v2),
        "<Test><body xmlns=\"http://example.com/ns\">hello world</body></Test>", NULL);
    XmlnsT3 v3 = {.Body = S("hello world")};
    check_marshal(t, "encodeXMLNS3", BURROW_ANY(TYPE_OF(XmlnsT3), &v3), want, NULL);
}

#define TESTS(X)                                                                       \
    X(TestMarshalGenerated)                                                            \
    X(TestUnmarshalGenerated)                                                          \
    X(TestMarshalMethods)                                                              \
    X(TestMarshalService)                                                              \
    X(TestMarshalNilEmbedded)                                                          \
    X(TestMarshalErrors)                                                               \
    X(TestMarshalNotClosed)                                                            \
    X(TestEncodeElementFromMarshalXML)                                                 \
    X(TestEncodeElement)                                                               \
    X(TestMarshalWriteErrors)                                                          \
    X(TestMarshalWriteIOErrors)                                                        \
    X(TestStructPointerMarshal)                                                        \
    X(TestInvalidXMLName)                                                              \
    X(TestMarshalZeroValue)                                                            \
    X(TestEncodeXMLNS)

TESTING_MAIN(TESTS)
