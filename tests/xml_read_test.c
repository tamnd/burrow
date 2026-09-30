/* Derived from Go's src/encoding/xml/read_test.go, the tests whose types have
 * methods or that need something the generated tables can't say. The tables
 * in read_test.go (pathTests, badPathTests, tables, tableAttrs and the empty
 * and whitespace values) are generated into encoding_xml_test_gen.h and run
 * by xml_marshal_test.c. TestUnmarshalFeed is left out: Feed has time.Time
 * fields.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "../src/encoding/xml_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include "check.h"

#include <stdlib.h>
#include <string.h>

#define S BURROW_S

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, (Int)strlen(s)};
}

static Slice sbytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : S("<nil>");
}

/* s followed by n copies of open and n of close. */
static Slice nested(Alloc *a, const char *open, const char *close, Int n) {
    Int lo = (Int)strlen(open), lc = (Int)strlen(close);
    Byte *p = mem_alloc(a, (size_t)(n * (lo + lc)), 1);
    for (Int i = 0; i < n; i++)
        memcpy(p + i * lo, open, (size_t)lo);
    for (Int i = 0; i < n; i++)
        memcpy(p + n * lo + i * lc, close, (size_t)lc);
    return slice_from(p, n * (lo + lc), n * (lo + lc), TYPE_BYTE);
}

/* ---------------------------------------------------------- TestUnmarshaler */

#define MY_CHAR_DATA_FIELDS(F, T) F(T, Str, body, "")
BURROW_STRUCT_DECL(MyCharData, MY_CHAR_DATA_FIELDS);
BURROW_PTR_TYPE(MyCharDataPtr, MyCharData);

static Error my_char_data_unmarshal_xml(MyCharData *m, XmlDecoder *d,
                                        XmlStartElement start) {
    (void)start;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        XmlToken t = xml_decoder_token(d, &err);
        if (errors_is(err, io_eof)) /* found end of element */
            break;
        if (BURROW_FAILED(err))
            return err;
        if (t.kind == XML_CHAR_DATA) {
            Int n = m->body.len + t.char_data.len;
            Byte *p = mem_alloc(d->a, n > 0 ? (size_t)n : 1, 1);
            if (p == NULL)
                return burrow_err_out_of_memory;
            if (m->body.len > 0)
                memcpy(p, m->body.p, (size_t)m->body.len);
            if (t.char_data.len > 0)
                memcpy(p + m->body.len, t.char_data.p, (size_t)t.char_data.len);
            m->body = (Str){p, n};
        }
    }
    return BURROW_NO_ERROR;
}

static Error my_char_data_unmarshal_xml_attr(MyCharData *m, Alloc *a, XmlAttr attr) {
    (void)m;
    (void)a;
    (void)attr;
    abort(); /* must not call */
}

#define MY_CHAR_DATA_METHODS(M, T)                                                     \
    M(T, UnmarshalXML, my_char_data_unmarshal_xml, XML_SIG_UNMARSHAL_XML)              \
    M(T, UnmarshalXMLAttr, my_char_data_unmarshal_xml_attr, XML_SIG_UNMARSHAL_XML_ATTR)
BURROW_STRUCT_DEFINE_METHODS(MyCharData, MY_CHAR_DATA_FIELDS, MY_CHAR_DATA_METHODS);

#define MY_ATTR_FIELDS(F, T) F(T, Str, attr, "")
BURROW_STRUCT_DECL(MyAttr, MY_ATTR_FIELDS);
BURROW_PTR_TYPE(MyAttrPtr, MyAttr);

static Error my_attr_unmarshal_xml_attr(MyAttr *m, Alloc *a, XmlAttr attr) {
    m->attr = str_clone(a, attr.value);
    return BURROW_NO_ERROR;
}

#define MY_ATTR_METHODS(M, T)                                                          \
    M(T, UnmarshalXMLAttr, my_attr_unmarshal_xml_attr, XML_SIG_UNMARSHAL_XML_ATTR)
BURROW_STRUCT_DEFINE_METHODS(MyAttr, MY_ATTR_FIELDS, MY_ATTR_METHODS);

#define MY_STRUCT_FIELDS(F, T)                                                         \
    F(T, MyCharDataPtr, Data, "")                                                      \
    F(T, MyAttrPtr, Attr, "xml:\",attr\"")                                             \
    F(T, MyCharData, Data2, "")                                                        \
    F(T, MyAttr, Attr2, "xml:\",attr\"")
BURROW_STRUCT(MyStruct, MY_STRUCT_FIELDS);

static void TestUnmarshaler(TestingT *t) {
    Str xml = S("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "\t\t<MyStruct Attr=\"attr1\" Attr2=\"attr2\">\n"
                "\t\t<Data>hello <!-- comment -->world</Data>\n"
                "\t\t<Data2>howdy <!-- comment -->world</Data2>\n"
                "\t\t</MyStruct>\n\t");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MyStruct m = {0};
    Error err = xml_unmarshal(a, sbytes(xml), BURROW_ANY(TYPE_OF(MyStruct), &m));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", err_str(err));
    else if (m.Data == NULL || m.Attr == NULL ||
             !str_eq(m.Data->body, S("hello world")) ||
             !str_eq(m.Attr->attr, S("attr1")) ||
             !str_eq(m.Data2.body, S("howdy world")) ||
             !str_eq(m.Attr2.attr, S("attr2")))
        testing_t_errorf_v(t, "m = {Data: %q, Attr: %q, Data2: %q, Attr2: %q}",
                           m.Data == NULL ? S("<nil>") : m.Data->body,
                           m.Attr == NULL ? S("<nil>") : m.Attr->attr, m.Data2.body,
                           m.Attr2.attr);
    arena_free(&ar);
}

/* ------------------------------------------------ TestUnmarshalIntoInterface */

#define PEA_FIELDS(F, T) F(T, Str, Cotelydon, "")
BURROW_STRUCT(Pea, PEA_FIELDS);
BURROW_PTR_TYPE(PeaPtr, Pea);

#define POD_FIELDS(F, T) F(T, Any, Pea, "xml:\"Pea\"")
BURROW_STRUCT(Pod, POD_FIELDS);

/* https://golang.org/issue/6836 */
static void TestUnmarshalIntoInterface(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Pea pea = {0};
    PeaPtr pp = &pea;
    Pod pod = {BURROW_ANY(TYPE_OF(PeaPtr), &pp)};
    Str xml = S("<Pod><Pea><Cotelydon>Green stuff</Cotelydon></Pea></Pod>");
    Error err = xml_unmarshal(a, sbytes(xml), BURROW_ANY(TYPE_OF(Pod), &pod));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to unmarshal %q: %s", xml, err_str(err));
    else if (pod.Pea.t != TYPE_OF(PeaPtr) || pod.Pea.data != &pp || pp != &pea)
        testing_t_fatalf_v(t, "unmarshaled into wrong type");
    else if (!str_eq(pea.Cotelydon, S("Green stuff")))
        testing_t_errorf_v(t, "failed to unmarshal into interface, have %q want %q",
                           pea.Cotelydon, S("Green stuff"));
    arena_free(&ar);
}

/* ---------------------------------------------------- TestUnmarshalIntoNil */

#define INTO_NIL_FIELDS(F, T) F(T, Int, A, "xml:\"A\"")
BURROW_STRUCT(IntoNil, INTO_NIL_FIELDS);

static void TestUnmarshalIntoNil(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = sbytes(S("<T><A>1</A></T>"));
    Error err = xml_unmarshal(a, in, BURROW_ANY(TYPE_OF(IntoNil), NULL));
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "no error in unmarshaling");
    else if (!str_eq(error_text(err), S("nil pointer passed to Unmarshal")))
        testing_t_errorf_v(t, "error = %q", error_text(err));
    err = xml_unmarshal(a, in, (Any){0});
    if (!BURROW_FAILED(err) ||
        !str_eq(error_text(err), S("non-pointer passed to Unmarshal")))
        testing_t_errorf_v(t, "untyped: error = %q", err_str(err));
    arena_free(&ar);
}

/* Go's stacks grow and ours do not, and these nest ten thousand elements
 * deep, so they run on a goroutine with room for it. */
typedef struct BigStack {
    TestingT *t;
    void (*fn)(TestingT *t);
    SyncWaitGroup wg;
} BigStack;

static void big_stack_body(void *env) {
    BigStack *b = env;
    b->fn(b->t);
    sync_wait_group_done(&b->wg);
}

static void on_big_stack(TestingT *t, void (*fn)(TestingT *t)) {
    static BigStack b;
    memset(&b, 0, sizeof b);
    b.t = t;
    b.fn = fn;
    sync_wait_group_add(&b.wg, 1);
    if (!go_stack(BURROW_FN(Func, big_stack_body, &b), (size_t)64 << 20))
        testing_t_fatalf_v(t, "go_stack failed");
    sync_wait_group_wait(&b.wg);
}

/* ------------------------------------------------------------ depth limits */

/* A type that refers to itself: the pointer's name comes first, the struct,
 * then the pointer's descriptor, which needs the struct's. */
typedef struct Nested *NestedPtr;
#define NESTED_FIELDS(F, T) F(T, NestedPtr, Parent, "xml:\",any\"")
BURROW_STRUCT_DECL(Nested, NESTED_FIELDS);
BURROW_PTR_TYPE(NestedPtr, Nested);
BURROW_STRUCT_DEFINE(Nested, NESTED_FIELDS);

static void TestCVE202228131_body(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Nested n = {0};
    Slice in = nested(a, "<a>", "", 10000 + 1);
    Error err = xml_unmarshal(a, in, BURROW_ANY(TYPE_OF(Nested), &n));
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unmarshal did not fail");
    else if (!errors_is(err, burrow__xml_err_unmarshal_depth))
        testing_t_errorf_v(t, "Unmarshal unexpected error: got %q, want %q",
                           error_text(err),
                           error_text(burrow__xml_err_unmarshal_depth));
    arena_free(&ar);
}

static void TestCVE202228131(TestingT *t) {
    on_big_stack(t, TestCVE202228131_body);
}

BURROW_SLICE_TYPE(StrSlice, Str);
#define THINGS_FIELDS(F, T) F(T, StrSlice, Things, "")
BURROW_STRUCT(Things, THINGS_FIELDS);

/* Go's version reads 17,000,000 open tags and skips itself under -short. A
 * million says the same thing: an element Unmarshal has no place for is
 * skipped without recursion, however deep it goes. */
static void TestCVE202230633(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Things v = {0};
    Slice in = nested(a, "<a>", "", 1000000);
    Error err = xml_unmarshal(a, in, BURROW_ANY(TYPE_OF(Things), &v));
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unmarshal of an unclosed document did not fail");
    arena_free(&ar);
}

/* recursiveNode, whose UnmarshalXML decodes into an alias without the method. */
typedef Slice RecursiveNodeSlice;

#define RECURSIVE_NODE_FIELDS(F, T)                                                    \
    F(T, XmlName, XMLName, "")                                                         \
    F(T, RecursiveNodeSlice, Children, "xml:\",any\"")
BURROW_STRUCT_DECL(RecursiveNode, RECURSIVE_NODE_FIELDS);
BURROW_SLICE_TYPE(RecursiveNodeSlice, RecursiveNode);
BURROW_STRUCT(RecursiveAlias, RECURSIVE_NODE_FIELDS);

static Error recursive_node_unmarshal_xml(RecursiveNode *n, XmlDecoder *d,
                                          XmlStartElement start) {
    RecursiveAlias a = {0};
    Error err =
        xml_decoder_decode_element(d, BURROW_ANY(TYPE_OF(RecursiveAlias), &a), &start);
    if (BURROW_FAILED(err))
        return err;
    memcpy(n, &a, sizeof *n);
    return BURROW_NO_ERROR;
}

#define RECURSIVE_NODE_METHODS(M, T)                                                   \
    M(T, UnmarshalXML, recursive_node_unmarshal_xml, XML_SIG_UNMARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(RecursiveNode, RECURSIVE_NODE_FIELDS,
                             RECURSIVE_NODE_METHODS);

static void TestDecodeElementRecursion_body(TestingT *t) {
    static const struct {
        const char *name;
        Int depth;
        bool fail;
    } tests[] = {
        {"below limit", 10000, false},
        {"above limit", 10000 + 1, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        RecursiveNode n = {0};
        Slice in = nested(a, "<a>", "</a>", tests[i].depth);
        Error err = xml_unmarshal(a, in, BURROW_ANY(TYPE_OF(RecursiveNode), &n));
        if (tests[i].fail ? !errors_is(err, burrow__xml_err_unmarshal_depth)
                          : BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: unexpected error: got %s", cstr(tests[i].name),
                               err_str(err));
        arena_free(&ar);
    }
}

static void TestDecodeElementRecursion(TestingT *t) {
    on_big_stack(t, TestDecodeElementRecursion_body);
}

/* standardNode and customUnmarshalerNode. */
typedef struct StandardNode *StandardNodePtr;
typedef struct CustomNode *CustomNodePtr;

#define STANDARD_NODE_FIELDS(F, T)                                                     \
    F(T, StandardNodePtr, Sub, "xml:\"section\"")                                      \
    F(T, CustomNodePtr, Custom, "xml:\"extension\"")
BURROW_STRUCT_DECL(StandardNode, STANDARD_NODE_FIELDS);

#define CUSTOM_NODE_FIELDS(F, T) F(T, StandardNode, Body, "")
BURROW_STRUCT_DECL(CustomNode, CUSTOM_NODE_FIELDS);
BURROW_PTR_TYPE(StandardNodePtr, StandardNode);
BURROW_PTR_TYPE(CustomNodePtr, CustomNode);
BURROW_STRUCT_DEFINE(StandardNode, STANDARD_NODE_FIELDS);

static Error custom_node_unmarshal_xml(CustomNode *e, XmlDecoder *d,
                                       XmlStartElement start) {
    StandardNode body = {0};
    Error err =
        xml_decoder_decode_element(d, BURROW_ANY(TYPE_OF(StandardNode), &body), &start);
    if (BURROW_FAILED(err))
        return err;
    e->Body = body;
    return BURROW_NO_ERROR;
}

#define CUSTOM_NODE_METHODS(M, T)                                                      \
    M(T, UnmarshalXML, custom_node_unmarshal_xml, XML_SIG_UNMARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(CustomNode, CUSTOM_NODE_FIELDS, CUSTOM_NODE_METHODS);

/* 3 blocks of 5,000 nested <section> tags separated by <extension> tags, 15,003
 * deep in all. */
static void TestDecodeElementDepthBypass_body(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice sec = nested(a, "<section>", "</section>", 5000);
    Int half = sec.len / 2;
    static const char ext[] = "<extension>", endext[] = "</extension>";
    Int n =
        3 * (half + (Int)strlen(ext)) + 3 * ((Int)strlen(endext) + (sec.len - half));
    Byte *p = mem_alloc(a, (size_t)n, 1);
    Int off = 0;
    for (int i = 0; i < 3; i++) {
        memcpy(p + off, sec.p, (size_t)half);
        off += half;
        memcpy(p + off, ext, strlen(ext));
        off += (Int)strlen(ext);
    }
    for (int i = 0; i < 3; i++) {
        memcpy(p + off, endext, strlen(endext));
        off += (Int)strlen(endext);
        memcpy(p + off, (Byte *)sec.p + half, (size_t)(sec.len - half));
        off += sec.len - half;
    }
    StandardNode node = {0};
    Error err = xml_unmarshal(a, slice_from(p, off, off, TYPE_BYTE),
                              BURROW_ANY(TYPE_OF(StandardNode), &node));
    if (!errors_is(err, burrow__xml_err_unmarshal_depth))
        testing_t_errorf_v(t, "Unexpected error: got %q want %q", err_str(err),
                           error_text(burrow__xml_err_unmarshal_depth));
    arena_free(&ar);
}

static void TestDecodeElementDepthBypass(TestingT *t) {
    on_big_stack(t, TestDecodeElementDepthBypass_body);
}

/* manualNode, whose UnmarshalXML reads tokens and decodes each child. */
typedef struct ManualNode *ManualNodePtr;
#define MANUAL_NODE_FIELDS(F, T) F(T, ManualNodePtr, Child, "")
BURROW_STRUCT_DECL(ManualNode, MANUAL_NODE_FIELDS);
BURROW_PTR_TYPE(ManualNodePtr, ManualNode);

static Error manual_node_unmarshal_xml(ManualNode *m, XmlDecoder *d,
                                       XmlStartElement start) {
    (void)start;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        if (tok.kind == XML_START_ELEMENT) {
            ManualNode *child = mem_alloc(d->a, sizeof *child, _Alignof(ManualNode));
            if (child == NULL)
                return burrow_err_out_of_memory;
            memset(child, 0, sizeof *child);
            err = xml_decoder_decode_element(d, BURROW_ANY(TYPE_OF(ManualNode), child),
                                             &tok.start);
            if (BURROW_FAILED(err))
                return err;
            m->Child = child;
        } else if (tok.kind == XML_END_ELEMENT) {
            return BURROW_NO_ERROR;
        }
    }
}

#define MANUAL_NODE_METHODS(M, T)                                                      \
    M(T, UnmarshalXML, manual_node_unmarshal_xml, XML_SIG_UNMARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(ManualNode, MANUAL_NODE_FIELDS, MANUAL_NODE_METHODS);

static void TestRecursiveUnmarshalInterfaceDepth_body(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ManualNode node = {0};
    Slice in = nested(a, "<a>", "</a>", 10000 + 1);
    Error err = xml_unmarshal(a, in, BURROW_ANY(TYPE_OF(ManualNode), &node));
    if (!errors_is(err, burrow__xml_err_unmarshal_depth))
        testing_t_errorf_v(t, "Unexpected error: got %q want %q", err_str(err),
                           error_text(burrow__xml_err_unmarshal_depth));
    arena_free(&ar);
}

static void TestRecursiveUnmarshalInterfaceDepth(TestingT *t) {
    on_big_stack(t, TestRecursiveUnmarshalInterfaceDepth_body);
}

/* ------------------------------------------------- TestUnmarshalXMLRawToken */

#define RAW_TOKEN_NODE_FIELDS(F, T) F(T, Int, pad, "")
BURROW_STRUCT_DECL(RawTokenNode, RAW_TOKEN_NODE_FIELDS);

static Error raw_token_node_unmarshal_xml(RawTokenNode *r, XmlDecoder *d,
                                          XmlStartElement start) {
    (void)r;
    (void)start;
    Error err = BURROW_NO_ERROR;
    (void)xml_decoder_raw_token(d, &err);
    return err;
}

#define RAW_TOKEN_NODE_METHODS(M, T)                                                   \
    M(T, UnmarshalXML, raw_token_node_unmarshal_xml, XML_SIG_UNMARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(RawTokenNode, RAW_TOKEN_NODE_FIELDS,
                             RAW_TOKEN_NODE_METHODS);

static void TestUnmarshalXMLRawToken(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    RawTokenNode node = {0};
    Error err = xml_unmarshal(a, sbytes(S("<a></a>")),
                              BURROW_ANY(TYPE_OF(RawTokenNode), &node));
    if (!errors_is(err, burrow__xml_err_raw_token))
        testing_t_fatalf_v(t, "UnmarshalXML calling RawToken: got error %s, want %s",
                           err_str(err), error_text(burrow__xml_err_raw_token));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestUnmarshaler)                                                                 \
    X(TestUnmarshalIntoInterface)                                                      \
    X(TestUnmarshalIntoNil)                                                            \
    X(TestCVE202228131)                                                                \
    X(TestCVE202230633)                                                                \
    X(TestDecodeElementRecursion)                                                      \
    X(TestDecodeElementDepthBypass)                                                    \
    X(TestRecursiveUnmarshalInterfaceDepth)                                            \
    X(TestUnmarshalXMLRawToken)

TESTING_MAIN(TESTS)
