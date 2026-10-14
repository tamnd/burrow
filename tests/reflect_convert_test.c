/* Tests for reflect's conversions: Convert, CanConvert, ConvertibleTo,
 * AssignableTo and Comparable.
 *
 * These are Go's tests from all_test.go and set_test.go, under Go's names.
 * Some entries put a value into an interface with methods, such as a
 * *bytes.Buffer into an io.Reader. An interface with methods is a vtable here,
 * and reflect finds the one a type has from what BURROW_IMPLEMENTS recorded,
 * which bytes.Buffer and strings.Builder do for io.Reader and io.Writer.
 *
 * Go's MakeRO and IsRO come from its export_test.go. Here they set and read the
 * same flag bit, from the package's internal header.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"
#include "burrow/utf8.h"

#include "../src/reflect/internal.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A descriptor written out by hand, with the size given, since C has no
 * zero-length arrays to take the size of. */
#define DESC(name, pkg, kind, size, align, nfield, fields, elem, key, len)             \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        nfield,                                                                        \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        len,                                                                           \
        0,                                                                             \
        NULL,                                                                          \
    }

#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

/* A named type of reflect_test, and an unnamed type, with no fields. */
#define NAMED(id, nm, kind, C, elem, key, len)                                         \
    static const Type id =                                                             \
        DESC(S_(nm), TEST_PKG, kind, sizeof(C), _Alignof(C), 0, NULL, elem, key, len)
#define UNNAMED(id, kind, C, elem, key, len)                                           \
    static const Type id =                                                             \
        DESC(NONAME, NONAME, kind, sizeof(C), _Alignof(C), 0, NULL, elem, key, len)

/* The channel directions as a descriptor's len holds them. */
#define BOTH 0
#define RECV REFLECT_RECV_DIR
#define SEND REFLECT_SEND_DIR

/* ------------------------------------------------------------------- types */

/* type integer int, from the top of all_test.go. */
NAMED(ty_integer, "integer", KIND_INT, Int, NULL, NULL, 0);

/* The types declared above convertTests. */
NAMED(ty_MyString, "MyString", KIND_STRING, Str, NULL, NULL, 0);
NAMED(ty_MyByte, "MyByte", KIND_UINT8, uint8_t, NULL, NULL, 0);
NAMED(ty_MyRune, "MyRune", KIND_INT32, int32_t, NULL, NULL, 0);
NAMED(ty_MyBytes, "MyBytes", KIND_SLICE, Slice, &burrow_type_uint8_t, NULL, 0);
NAMED(ty_MyRunes, "MyRunes", KIND_SLICE, Slice, &burrow_type_int32_t, NULL, 0);
NAMED(ty_MyBytes2, "MyBytes2", KIND_SLICE, Slice, &ty_MyByte, NULL, 0);
NAMED(ty_MyRunes2, "MyRunes2", KIND_SLICE, Slice, &ty_MyRune, NULL, 0);
NAMED(ty_MyFunc, "MyFunc", KIND_FUNC, Func, NULL, NULL, 0);

UNNAMED(ty_runes, KIND_SLICE, Slice, &burrow_type_int32_t, NULL, 0);
UNNAMED(ty_sl_MyByte, KIND_SLICE, Slice, &ty_MyByte, NULL, 0);
UNNAMED(ty_sl_MyRune, KIND_SLICE, Slice, &ty_MyRune, NULL, 0);
UNNAMED(ty_sl_uint, KIND_SLICE, Slice, &burrow_type_Uint, NULL, 0);
UNNAMED(ty_sl_int, KIND_SLICE, Slice, &burrow_type_Int, NULL, 0);
UNNAMED(ty_func, KIND_FUNC, Func, NULL, NULL, 0);

static const Type ty_arr0 =
    DESC(NONAME, NONAME, KIND_ARRAY, 0, 1, 0, NULL, &burrow_type_uint8_t, NULL, 0);
static const Type ty_arr1 =
    DESC(NONAME, NONAME, KIND_ARRAY, 1, 1, 0, NULL, &burrow_type_uint8_t, NULL, 1);
static const Type ty_arr2 =
    DESC(NONAME, NONAME, KIND_ARRAY, 2, 1, 0, NULL, &burrow_type_uint8_t, NULL, 2);
static const Type ty_arr3 =
    DESC(NONAME, NONAME, KIND_ARRAY, 3, 1, 0, NULL, &burrow_type_uint8_t, NULL, 3);
static const Type ty_arr4 =
    DESC(NONAME, NONAME, KIND_ARRAY, 4, 1, 0, NULL, &burrow_type_uint8_t, NULL, 4);
static const Type ty_arr0_MyByte =
    DESC(NONAME, NONAME, KIND_ARRAY, 0, 1, 0, NULL, &ty_MyByte, NULL, 0);
static const Type ty_arr2_MyByte =
    DESC(NONAME, NONAME, KIND_ARRAY, 2, 1, 0, NULL, &ty_MyByte, NULL, 2);
static const Type ty_MyBytesArray0 = DESC(S_("MyBytesArray0"), TEST_PKG, KIND_ARRAY, 0,
                                          1, 0, NULL, &burrow_type_uint8_t, NULL, 0);
static const Type ty_MyBytesArray = DESC(S_("MyBytesArray"), TEST_PKG, KIND_ARRAY, 4, 1,
                                         0, NULL, &burrow_type_uint8_t, NULL, 4);

UNNAMED(ty_p_arr0, KIND_POINTER, void *, &ty_arr0, NULL, 0);
UNNAMED(ty_p_arr1, KIND_POINTER, void *, &ty_arr1, NULL, 0);
NAMED(ty_MyBytesArrayPtr0, "MyBytesArrayPtr0", KIND_POINTER, void *, &ty_arr0, NULL, 0);
NAMED(ty_MyBytesArrayPtr, "MyBytesArrayPtr", KIND_POINTER, void *, &ty_arr4, NULL, 0);
UNNAMED(ty_p_MyBytesArray0, KIND_POINTER, void *, &ty_MyBytesArray0, NULL, 0);
UNNAMED(ty_p_MyBytesArray, KIND_POINTER, void *, &ty_MyBytesArray, NULL, 0);

UNNAMED(ty_p_int, KIND_POINTER, void *, &burrow_type_Int, NULL, 0);
UNNAMED(ty_pp_int, KIND_POINTER, void *, &ty_p_int, NULL, 0);
UNNAMED(ty_ppp_int, KIND_POINTER, void *, &ty_pp_int, NULL, 0);
UNNAMED(ty_p_integer, KIND_POINTER, void *, &ty_integer, NULL, 0);
UNNAMED(ty_p_byte, KIND_POINTER, void *, &burrow_type_uint8_t, NULL, 0);
UNNAMED(ty_pp_byte, KIND_POINTER, void *, &ty_p_byte, NULL, 0);
UNNAMED(ty_ppp_byte, KIND_POINTER, void *, &ty_pp_byte, NULL, 0);
UNNAMED(ty_p_MyByte, KIND_POINTER, void *, &ty_MyByte, NULL, 0);
UNNAMED(ty_pp_MyByte, KIND_POINTER, void *, &ty_p_MyByte, NULL, 0);
UNNAMED(ty_p_int32, KIND_POINTER, void *, &burrow_type_int32_t, NULL, 0);
UNNAMED(ty_pp_int32, KIND_POINTER, void *, &ty_p_int32, NULL, 0);
UNNAMED(ty_ppp_int32, KIND_POINTER, void *, &ty_pp_int32, NULL, 0);
UNNAMED(ty_p_int64, KIND_POINTER, void *, &burrow_type_int64_t, NULL, 0);
UNNAMED(ty_pp_int64, KIND_POINTER, void *, &ty_p_int64, NULL, 0);
UNNAMED(ty_ppp_int64, KIND_POINTER, void *, &ty_pp_int64, NULL, 0);
UNNAMED(ty_p_any, KIND_POINTER, void *, &burrow_type_Any, NULL, 0);
UNNAMED(ty_p_reader, KIND_POINTER, void *, &burrow_type_IoReader, NULL, 0);
UNNAMED(ty_p_writer, KIND_POINTER, void *, &burrow_type_IoWriter, NULL, 0);

UNNAMED(ty_chan_byte, KIND_CHAN, void *, &burrow_type_uint8_t, NULL, BOTH);
UNNAMED(ty_chan_MyByte, KIND_CHAN, void *, &ty_MyByte, NULL, BOTH);
UNNAMED(ty_map_int_byte, KIND_MAP, Map *, &burrow_type_uint8_t, &burrow_type_Int, 0);
UNNAMED(ty_map_int_MyByte, KIND_MAP, Map *, &ty_MyByte, &burrow_type_Int, 0);
UNNAMED(ty_map_byte_int, KIND_MAP, Map *, &burrow_type_Int, &burrow_type_uint8_t, 0);
UNNAMED(ty_map_MyByte_int, KIND_MAP, Map *, &burrow_type_Int, &ty_MyByte, 0);
UNNAMED(ty_map_int_bool, KIND_MAP, Map *, &burrow_type_bool, &burrow_type_Int, 0);
UNNAMED(ty_map_uint_bool, KIND_MAP, Map *, &burrow_type_bool, &burrow_type_Uint, 0);

UNNAMED(ty_chan_int, KIND_CHAN, void *, &burrow_type_Int, NULL, BOTH);
UNNAMED(ty_chan_int_recv, KIND_CHAN, void *, &burrow_type_Int, NULL, RECV);
UNNAMED(ty_chan_int_send, KIND_CHAN, void *, &burrow_type_Int, NULL, SEND);
NAMED(ty_IntChan, "IntChan", KIND_CHAN, void *, &burrow_type_Int, NULL, BOTH);
NAMED(ty_IntChanRecv, "IntChanRecv", KIND_CHAN, void *, &burrow_type_Int, NULL, RECV);
NAMED(ty_IntChanSend, "IntChanSend", KIND_CHAN, void *, &burrow_type_Int, NULL, SEND);
UNNAMED(ty_chan_bytes, KIND_CHAN, void *, &burrow_type_Bytes, NULL, BOTH);
UNNAMED(ty_chan_bytes_recv, KIND_CHAN, void *, &burrow_type_Bytes, NULL, RECV);
UNNAMED(ty_chan_bytes_send, KIND_CHAN, void *, &burrow_type_Bytes, NULL, SEND);
NAMED(ty_BytesChan, "BytesChan", KIND_CHAN, void *, &burrow_type_Bytes, NULL, BOTH);
NAMED(ty_BytesChanRecv, "BytesChanRecv", KIND_CHAN, void *, &burrow_type_Bytes, NULL,
      RECV);
NAMED(ty_BytesChanSend, "BytesChanSend", KIND_CHAN, void *, &burrow_type_Bytes, NULL,
      SEND);

/* type Empty struct{} and struct{} itself. */
static const Type ty_Empty =
    DESC(S_("Empty"), TEST_PKG, KIND_STRUCT, 0, 1, 0, NULL, NULL, NULL, 0);
static const Type ty_struct0 =
    DESC(NONAME, NONAME, KIND_STRUCT, 0, 1, 0, NULL, NULL, NULL, 0);
UNNAMED(ty_p_Empty, KIND_POINTER, void *, &ty_Empty, NULL, 0);
UNNAMED(ty_p_struct0, KIND_POINTER, void *, &ty_struct0, NULL, 0);

/* struct { x int `some:"foo"` }, the same with "bar", and MyStruct, which is
 * the same with "tag". */
static const Field fx_foo[] = {{S_("x"), S_("some:\"foo\""), &burrow_type_Int, 0}};
static const Field fx_bar[] = {{S_("x"), S_("some:\"bar\""), &burrow_type_Int, 0}};
static const Field fx_tag[] = {{S_("x"), S_("some:\"tag\""), &burrow_type_Int, 0}};
static const Type ty_sx_foo = DESC(NONAME, TEST_PKG, KIND_STRUCT, sizeof(Int),
                                   _Alignof(Int), 1, fx_foo, NULL, NULL, 0);
static const Type ty_sx_bar = DESC(NONAME, TEST_PKG, KIND_STRUCT, sizeof(Int),
                                   _Alignof(Int), 1, fx_bar, NULL, NULL, 0);
static const Type ty_MyStruct = DESC(S_("MyStruct"), TEST_PKG, KIND_STRUCT, sizeof(Int),
                                     _Alignof(Int), 1, fx_tag, NULL, NULL, 0);

/* MyStruct1 and MyStruct2: struct { x struct { int `some:"bar"` } } and the
 * same with "foo". The inner field is an embedded int. */
static const Field fi_bar[] = {{S_("int"), S_("some:\"bar\""), &burrow_type_Int, 0}};
static const Field fi_foo[] = {{S_("int"), S_("some:\"foo\""), &burrow_type_Int, 0}};
static const Type ty_si_bar = DESC(NONAME, TEST_PKG, KIND_STRUCT, sizeof(Int),
                                   _Alignof(Int), 1, fi_bar, NULL, NULL, 0);
static const Type ty_si_foo = DESC(NONAME, TEST_PKG, KIND_STRUCT, sizeof(Int),
                                   _Alignof(Int), 1, fi_foo, NULL, NULL, 0);
static const Field fm1[] = {{S_("x"), NONAME, &ty_si_bar, 0}};
static const Field fm2[] = {{S_("x"), NONAME, &ty_si_foo, 0}};
static const Type ty_MyStruct1 =
    DESC(S_("MyStruct1"), TEST_PKG, KIND_STRUCT, sizeof(Int), _Alignof(Int), 1, fm1,
         NULL, NULL, 0);
static const Type ty_MyStruct2 =
    DESC(S_("MyStruct2"), TEST_PKG, KIND_STRUCT, sizeof(Int), _Alignof(Int), 1, fm2,
         NULL, NULL, 0);

/* ----------------------------------------------------------------- helpers */

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

/* ValueOf(&x).Elem() for the t at p. */
static ReflectValue settable(const Type *t, void *p) {
    return reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(t), &p)));
}

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

/* Fresh zeroed room for a t, never NULL. */
static void *mk(Alloc *a, const Type *t) {
    void *p = mem_alloc(a, t->size == 0 ? 1 : t->size, t->align);
    if (p == NULL)
        abort();
    return p;
}

/* V(T(n)) for a number type T. */
static ReflectValue vnum(Alloc *a, const Type *t, int64_t n) {
    void *p = mk(a, t);
    ReflectValue s = settable(t, p);
    switch ((int)t->kind) {
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        reflect_value_set_float(s, (double)n);
        break;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        reflect_value_set_uint(s, (uint64_t)n);
        break;
    default:
        reflect_value_set_int(s, n);
        break;
    }
    return val(t, p);
}

static ReflectValue vfloat(Alloc *a, const Type *t, double x) {
    void *p = mk(a, t);
    reflect_value_set_float(settable(t, p), x);
    return val(t, p);
}

static ReflectValue vcomplex(Alloc *a, const Type *t, double re, double im) {
    void *p = mk(a, t);
    reflect_value_set_complex(settable(t, p), (Complex128){re, im});
    return val(t, p);
}

/* V(T(s)) for a string type T. */
static ReflectValue vstr(Alloc *a, const Type *t, const char *s) {
    void *p = mk(a, t);
    Str x = str_from_bytes(s, (Int)strlen(s));
    memcpy(p, &x, sizeof x);
    return val(t, p);
}

/* V(T(b)) for a slice type T of bytes, which is never nil. */
static ReflectValue vbytes(Alloc *a, const Type *t, const void *b, Int n) {
    void *e = mem_alloc(a, n == 0 ? 1 : (size_t)n, 1);
    if (e == NULL)
        abort();
    if (n > 0)
        memcpy(e, b, (size_t)n);
    Slice x = {e, n, n, t->elem};
    void *p = mk(a, t);
    memcpy(p, &x, sizeof x);
    return val(t, p);
}

static ReflectValue vbytes_s(Alloc *a, const Type *t, const char *s) {
    return vbytes(a, t, s, (Int)strlen(s));
}

/* V(T(s)) for a slice type T of runes. */
static ReflectValue vrunes(Alloc *a, const Type *t, const char *s) {
    Str in = str_from_bytes(s, (Int)strlen(s));
    Rune *r = mem_alloc(a, (size_t)(in.len + 1) * sizeof(Rune), _Alignof(Rune));
    if (r == NULL)
        abort();
    Int n = 0;
    for (Int i = 0; i < in.len; n++) {
        Int size;
        r[n] = utf8_decode_rune_in_string(str_from_bytes(in.p + i, in.len - i), &size);
        i += size;
    }
    Slice x = {r, n, n, t->elem};
    void *p = mk(a, t);
    memcpy(p, &x, sizeof x);
    return val(t, p);
}

/* V(T{b...}) for an array type T of bytes. */
static ReflectValue varr(Alloc *a, const Type *t, const void *b) {
    void *p = mk(a, t);
    if (t->size > 0)
        memcpy(p, b, t->size);
    return val(t, p);
}

/* V(&E{b...}) for a pointer type T to an array of bytes. */
static ReflectValue vptr(Alloc *a, const Type *t, const void *b) {
    void *e = mk(a, t->elem);
    if (b != NULL && t->elem->size > 0)
        memcpy(e, b, t->elem->size);
    void *p = mk(a, t);
    memcpy(p, &e, sizeof e);
    return val(t, p);
}

/* V(new(E)) for a pointer type T. */
static ReflectValue vnew(Alloc *a, const Type *t) {
    return vptr(a, t, NULL);
}

/* V(T(nil)), or V(T{}) for an array or a struct. */
static ReflectValue vnil(const Type *t) {
    return reflect_zero(t);
}

/* EmptyInterfaceV(x). */
static ReflectValue vany(Alloc *a, ReflectValue x) {
    Any *box = mk(a, TYPE_ANY);
    *box = BURROW_ANY(x.typ, x.ptr);
    return settable(TYPE_ANY, box);
}

/* Go's MakeRO and IsRO, from export_test.go. */
static ReflectValue make_ro(ReflectValue v) {
    v.flag |= BURROW__RF_STICKY_RO;
    return v;
}

static bool is_ro(ReflectValue v) {
    return (v.flag & BURROW__RF_STICKY_RO) != 0;
}

static Str ts(ReflectType t) {
    return reflect_type_string(t);
}

/* Runs f(env) and copies the text of its panic, if there was one, into buf. */
static bool panics(void (*f)(void *), void *env, char *buf, size_t n) {
    volatile bool got = false;
    buf[0] = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str text = panic_text(r);
        size_t k = (size_t)text.len < n - 1 ? (size_t)text.len : n - 1;
        memcpy(buf, text.p, k);
        buf[k] = 0;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* Go's shouldPanic: f has to panic, and when expect is not empty the text has
 * to start with "reflect" and contain expect. */
static void should_panic(TestingT *t, const char *expect, void (*f)(void *),
                         void *env) {
    char buf[256];
    if (!panics(f, env, buf, sizeof buf)) {
        testing_t_errorf_v(t, "did not panic, want %q", expect);
        return;
    }
    if (expect[0] == 0)
        return;
    if (strncmp(buf, "reflect", 7) != 0)
        testing_t_errorf_v(t, "panic string does not start with \"reflect\": %s", buf);
    if (strstr(buf, expect) == NULL)
        testing_t_errorf_v(t, "%q does not contain %q", buf, expect);
}

/* -------------------------------------------------------------- interfaces
 *
 * The interfaces implementsTests and the interface entries of convertTests
 * use. burrow's io.Reader and io.Writer descriptors list their methods, so
 * those two are the library's own. io.ReaderAt and io.ReadWriter have C types
 * but no descriptors, ast.Expr is a node pointer in burrow's go/ast rather
 * than an interface, and so is ast.Ident's, so each of these is a stand-in
 * written here, with Go's name, package and methods. */

/* A descriptor with methods and no fields. */
#define DESCM(name, pkg, kind, size, align, nmethod, methods)                          \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        0,                                                                             \
        nmethod,                                                                       \
        NULL,                                                                          \
        methods,                                                                       \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* io.ReaderAt. */
#define READER_AT_METHODS(M, T) M(T, ReadAt, read_at, IO_SIG_READ_AT)
READER_AT_METHODS(BURROW__IFACE_METHOD_DEFS, IoReaderAt)
static const Method reader_at_methods[] = {
    READER_AT_METHODS(BURROW__METHOD_ENTRY, IoReaderAt)};
static const Type ty_ReaderAt =
    DESCM(S_("ReaderAt"), S_("io"), KIND_INTERFACE, sizeof(IoReaderAt),
          _Alignof(IoReaderAt), 1, reader_at_methods);

/* io.ReadWriter. */
#define READ_WRITER_METHODS(M, T)                                                      \
    M(T, Read, reader.read, IO_SIG_READ)                                               \
    M(T, Write, writer.write, IO_SIG_WRITE)
READ_WRITER_METHODS(BURROW__IFACE_METHOD_DEFS, IoReadWriter)
static const Method read_writer_methods[] = {
    READ_WRITER_METHODS(BURROW__METHOD_ENTRY, IoReadWriter)};
static const Type ty_ReadWriter =
    DESCM(S_("ReadWriter"), S_("io"), KIND_INTERFACE, sizeof(IoReadWriter),
          _Alignof(IoReadWriter), 2, read_writer_methods);

/* The vtable a bytes.Buffer has as an io.ReadWriter, which is its two others
 * side by side. Recorded at run time, since the parts are only to be had from
 * the functions that hand out the other two. */
static void record_read_writer(void) {
    static IoReadWriterVT vt;
    static TypeImpl impl;
    vt.reader = *bytes_buffer_as_io_reader(NULL).vt;
    vt.writer = *bytes_buffer_as_io_writer(NULL).vt;
    impl = (TypeImpl){TYPE_BYTES_BUFFER, &ty_ReadWriter, &vt};
    Error err = BURROW_NO_ERROR;
    if (!type_register_impl(&impl, &err))
        abort();
}

/* ast.Expr, and notASTExpr, which has the same three methods. Interface methods
 * go in name order, so exprNode comes last. */
typedef struct ExprVT {
    const Type *self_type;
    TokenPos (*end)(void *self);
    TokenPos (*pos)(void *self);
    void (*expr_node)(void *self);
} ExprVT;

typedef struct Expr {
    const ExprVT *vt;
    void *data;
} Expr;

#define EXPR_SIG_Pos(IN, OUT) OUT(TokenPos)
#define EXPR_SIG_None(IN, OUT)
#define EXPR_METHODS(M, T)                                                             \
    M(T, End, end, EXPR_SIG_Pos)                                                       \
    M(T, Pos, pos, EXPR_SIG_Pos)                                                       \
    M(T, exprNode, expr_node, EXPR_SIG_None)
EXPR_METHODS(BURROW__IFACE_METHOD_DEFS, Expr)
static const Method expr_methods[] = {EXPR_METHODS(BURROW__METHOD_ENTRY, Expr)};
static const Type burrow_type_Expr =
    DESCM(S_("Expr"), S_("go/ast"), KIND_INTERFACE, sizeof(Expr), _Alignof(Expr), 3,
          expr_methods);
static const Type ty_notASTExpr = DESCM(S_("notASTExpr"), TEST_PKG, KIND_INTERFACE,
                                        sizeof(Expr), _Alignof(Expr), 3, expr_methods);

/* ast.Ident, which implements ast.Expr, and notAnExpr, which has the methods
 * but is not from go/ast, so its exprNode is not ast.Expr's. Each method
 * returns token.NoPos. */
typedef struct Ident {
    TokenPos name_pos;
} Ident;

typedef struct NotAnExpr {
    char unused;
} NotAnExpr;

static TokenPos no_pos(void *self) {
    (void)self;
    return 0;
}

static void expr_node(void *self) {
    (void)self;
}

#define EXPR_IMPL_METHODS(M, T)                                                        \
    M(T, End, no_pos, EXPR_SIG_Pos)                                                    \
    M(T, Pos, no_pos, EXPR_SIG_Pos)                                                    \
    M(T, exprNode, expr_node, EXPR_SIG_None)
BURROW_METHODS_DEFINE(Ident, EXPR_IMPL_METHODS);
BURROW_METHODS_DEFINE(NotAnExpr, EXPR_IMPL_METHODS);

static const Type burrow_type_Ident =
    DESCM(S_("Ident"), S_("go/ast"), KIND_STRUCT, sizeof(Ident), _Alignof(Ident), 3,
          burrow__methods_Ident);
static const Type ty_notAnExpr =
    DESCM(S_("notAnExpr"), TEST_PKG, KIND_STRUCT, sizeof(NotAnExpr),
          _Alignof(NotAnExpr), 3, burrow__methods_NotAnExpr);

static const ExprVT ident_expr_vt = {&burrow_type_Ident, no_pos, no_pos, expr_node};
BURROW_IMPLEMENTS(Ident, Expr, ident_expr_vt);

typedef struct ImplementsTest {
    ReflectType x, t;
    bool b;
} ImplementsTest;

/* implementsTests, whose x is the type new(x) points at, which is how Go names
 * a type there. Go's second entry, a bytes.Buffer that does not implement
 * io.Reader because Read is on *bytes.Buffer, is left out: a receiver here is
 * always a pointer, so T and *T have the same methods, and bytes.Buffer
 * records its vtable for the struct. */
static Int implements_tests(ImplementsTest *out) {
    ReflectType buf = reflect_pointer_to(TYPE_BYTES_BUFFER);
    ReflectType ident = reflect_pointer_to(&burrow_type_Ident);
    ReflectType not_an_expr = reflect_pointer_to(&ty_notAnExpr);
    ImplementsTest tests[] = {
        {buf, TYPE_OF(IoReader), true},
        {buf, &ty_ReaderAt, false},
        {ident, &burrow_type_Expr, true},
        {not_an_expr, &burrow_type_Expr, false},
        {ident, &ty_notASTExpr, false},
        {&ty_notASTExpr, &burrow_type_Expr, false},
        {&burrow_type_Expr, &ty_notASTExpr, false},
        {not_an_expr, &ty_notASTExpr, true},
    };
    memcpy(out, tests, sizeof tests);
    return NELEM(tests);
}

/* ReaderV(x) and ReadWriterV(x), here for x a new(bytes.Buffer). */
static ReflectValue vreader(Alloc *a) {
    IoReader *r = mk(a, TYPE_OF(IoReader));
    *r = bytes_buffer_as_io_reader(mk(a, TYPE_BYTES_BUFFER));
    return settable(TYPE_OF(IoReader), r);
}

static ReflectValue vread_writer(Alloc *a) {
    IoReader r = bytes_buffer_as_io_reader(mk(a, TYPE_BYTES_BUFFER));
    IoReadWriter *rw = mk(a, &ty_ReadWriter);
    rw->vt = type_vtable(TYPE_BYTES_BUFFER, &ty_ReadWriter);
    rw->data = r.data;
    return settable(&ty_ReadWriter, rw);
}

typedef struct Op {
    Alloc *a;
    ReflectValue v;
    ReflectType t;
} Op;

static void op_convert(void *env) {
    Op *o = env;
    (void)reflect_value_convert(o->v, o->a, o->t);
}

/* ------------------------------------------------------------ convertTests */

typedef struct ConvertTest {
    ReflectValue in, out;
} ConvertTest;

#define E(x, y) (tab[n++] = (ConvertTest){(x), (y)})

/* Go's convertTests. The numbers come from the generator in the comment at
 * the top of Go's table, which is run here rather than pasted. */
static Int convert_tests(Alloc *a, ConvertTest *tab) {
    Int n = 0;
    record_read_writer();

    /* numbers */
    const Type *numbers[] = {
        TYPE_INT8,    TYPE_UINT8,   TYPE_INT16,   TYPE_UINT16, TYPE_INT32,
        TYPE_UINT32,  TYPE_INT64,   TYPE_UINT64,  TYPE_INT,    TYPE_UINT,
        TYPE_UINTPTR, TYPE_FLOAT32, TYPE_FLOAT64,
    };
    int64_t k = 1;
    for (Int i = 0; i < NELEM(numbers); i++) {
        for (Int j = i; j < NELEM(numbers); j++) {
            const Type *f = numbers[i];
            const Type *g = numbers[j];
            E(vnum(a, f, k), vnum(a, g, k));
            k++;
            if (f != g) {
                E(vnum(a, g, k), vnum(a, f, k));
                k++;
            }
        }
    }

    /* truncation */
    E(vfloat(a, TYPE_FLOAT64, 1.5), vnum(a, TYPE_INT, 1));

    /* complex */
    E(vcomplex(a, TYPE_COMPLEX64, 0, 1), vcomplex(a, TYPE_COMPLEX64, 0, 1));
    E(vcomplex(a, TYPE_COMPLEX64, 0, 2), vcomplex(a, TYPE_COMPLEX128, 0, 2));
    E(vcomplex(a, TYPE_COMPLEX128, 0, 3), vcomplex(a, TYPE_COMPLEX64, 0, 3));
    E(vcomplex(a, TYPE_COMPLEX128, 0, 4), vcomplex(a, TYPE_COMPLEX128, 0, 4));

    /* string. The runes are ♝, ♕ and 🙈🙉🙊 written as bytes. */
#define BISHOP "runes\xe2\x99\x9d"
#define QUEEN "runes\xe2\x99\x95"
#define MONKEYS "runes\xf0\x9f\x99\x88\xf0\x9f\x99\x89\xf0\x9f\x99\x8a"
#define BAD "\xef\xbf\xbd"
    E(vstr(a, TYPE_STRING, "hello"), vstr(a, TYPE_STRING, "hello"));
    E(vstr(a, TYPE_STRING, "bytes1"), vbytes_s(a, TYPE_BYTES, "bytes1"));
    E(vbytes_s(a, TYPE_BYTES, "bytes2"), vstr(a, TYPE_STRING, "bytes2"));
    E(vbytes_s(a, TYPE_BYTES, "bytes3"), vbytes_s(a, TYPE_BYTES, "bytes3"));
    E(vstr(a, TYPE_STRING, BISHOP), vrunes(a, &ty_runes, BISHOP));
    E(vrunes(a, &ty_runes, QUEEN), vstr(a, TYPE_STRING, QUEEN));
    E(vrunes(a, &ty_runes, MONKEYS), vrunes(a, &ty_runes, MONKEYS));
    E(vnum(a, TYPE_INT, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_INT8, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_INT16, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_INT32, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_INT64, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINT, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINT8, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINT16, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINT32, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINT64, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_UINTPTR, 'a'), vstr(a, TYPE_STRING, "a"));
    E(vnum(a, TYPE_INT, -1), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT8, -2), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT16, -3), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT32, -4), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT64, -5), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT64, -(INT64_C(1) << 32)), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_INT64, INT64_C(1) << 32), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_UINT, 0x110001), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_UINT32, 0x110002), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_UINT64, 0x110003), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_UINT64, INT64_C(1) << 32), vstr(a, TYPE_STRING, BAD));
    E(vnum(a, TYPE_UINTPTR, 0x110004), vstr(a, TYPE_STRING, BAD));

    /* named string */
    E(vstr(a, &ty_MyString, "hello"), vstr(a, TYPE_STRING, "hello"));
    E(vstr(a, TYPE_STRING, "hello"), vstr(a, &ty_MyString, "hello"));
    E(vstr(a, TYPE_STRING, "hello"), vstr(a, TYPE_STRING, "hello"));
    E(vstr(a, &ty_MyString, "hello"), vstr(a, &ty_MyString, "hello"));
    E(vstr(a, &ty_MyString, "bytes1"), vbytes_s(a, TYPE_BYTES, "bytes1"));
    E(vbytes_s(a, TYPE_BYTES, "bytes2"), vstr(a, &ty_MyString, "bytes2"));
    E(vbytes_s(a, TYPE_BYTES, "bytes3"), vbytes_s(a, TYPE_BYTES, "bytes3"));
    E(vstr(a, &ty_MyString, BISHOP), vrunes(a, &ty_runes, BISHOP));
    E(vrunes(a, &ty_runes, QUEEN), vstr(a, &ty_MyString, QUEEN));
    E(vrunes(a, &ty_runes, MONKEYS), vrunes(a, &ty_runes, MONKEYS));
    E(vrunes(a, &ty_runes, MONKEYS), vrunes(a, &ty_MyRunes, MONKEYS));
    E(vrunes(a, &ty_MyRunes, MONKEYS), vrunes(a, &ty_runes, MONKEYS));
    E(vnum(a, TYPE_INT, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_INT8, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_INT16, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_INT32, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_INT64, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINT, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINT8, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINT16, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINT32, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINT64, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_UINTPTR, 'a'), vstr(a, &ty_MyString, "a"));
    E(vnum(a, TYPE_INT, -1), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_INT8, -2), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_INT16, -3), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_INT32, -4), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_INT64, -5), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_UINT, 0x110001), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_UINT32, 0x110002), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_UINT64, 0x110003), vstr(a, &ty_MyString, BAD));
    E(vnum(a, TYPE_UINTPTR, 0x110004), vstr(a, &ty_MyString, BAD));

    /* named []byte */
    E(vstr(a, TYPE_STRING, "bytes1"), vbytes_s(a, &ty_MyBytes, "bytes1"));
    E(vbytes_s(a, &ty_MyBytes, "bytes2"), vstr(a, TYPE_STRING, "bytes2"));
    E(vbytes_s(a, &ty_MyBytes, "bytes3"), vbytes_s(a, &ty_MyBytes, "bytes3"));
    E(vstr(a, &ty_MyString, "bytes1"), vbytes_s(a, &ty_MyBytes, "bytes1"));
    E(vbytes_s(a, &ty_MyBytes, "bytes2"), vstr(a, &ty_MyString, "bytes2"));

    /* named []rune */
    E(vstr(a, TYPE_STRING, BISHOP), vrunes(a, &ty_MyRunes, BISHOP));
    E(vrunes(a, &ty_MyRunes, QUEEN), vstr(a, TYPE_STRING, QUEEN));
    E(vrunes(a, &ty_MyRunes, MONKEYS), vrunes(a, &ty_MyRunes, MONKEYS));
    E(vstr(a, &ty_MyString, BISHOP), vrunes(a, &ty_MyRunes, BISHOP));
    E(vrunes(a, &ty_MyRunes, QUEEN), vstr(a, &ty_MyString, QUEEN));

    /* []namedByte */
    E(vstr(a, TYPE_STRING, "namedByte1"), vbytes_s(a, &ty_sl_MyByte, "namedByte1"));
    E(vstr(a, &ty_MyString, "namedByte2"), vbytes_s(a, &ty_sl_MyByte, "namedByte2"));
    E(vbytes_s(a, &ty_sl_MyByte, "namedByte3"), vstr(a, TYPE_STRING, "namedByte3"));
    E(vbytes_s(a, &ty_sl_MyByte, "namedByte4"), vstr(a, &ty_MyString, "namedByte4"));

    /* []namedRune */
    E(vstr(a, TYPE_STRING, "namedRune1"), vrunes(a, &ty_sl_MyRune, "namedRune1"));
    E(vstr(a, &ty_MyString, "namedRune2"), vrunes(a, &ty_sl_MyRune, "namedRune2"));
    E(vrunes(a, &ty_sl_MyRune, "namedRune3"), vstr(a, TYPE_STRING, "namedRune3"));
    E(vrunes(a, &ty_sl_MyRune, "namedRune4"), vstr(a, &ty_MyString, "namedRune4"));

    /* named []namedByte */
    E(vstr(a, TYPE_STRING, "namedByte5"), vbytes_s(a, &ty_MyBytes2, "namedByte5"));
    E(vstr(a, &ty_MyString, "namedByte6"), vbytes_s(a, &ty_MyBytes2, "namedByte6"));
    E(vbytes_s(a, &ty_MyBytes2, "namedByte7"), vstr(a, TYPE_STRING, "namedByte7"));
    E(vbytes_s(a, &ty_MyBytes2, "namedByte8"), vstr(a, &ty_MyString, "namedByte8"));

    /* named []namedRune */
    E(vstr(a, TYPE_STRING, "namedRune5"), vrunes(a, &ty_MyRunes2, "namedRune5"));
    E(vstr(a, &ty_MyString, "namedRune6"), vrunes(a, &ty_MyRunes2, "namedRune6"));
    E(vrunes(a, &ty_MyRunes2, "namedRune7"), vstr(a, TYPE_STRING, "namedRune7"));
    E(vrunes(a, &ty_MyRunes2, "namedRune8"), vstr(a, &ty_MyString, "namedRune8"));

    /* random ok conversions of the above types */
    E(vbytes_s(a, &ty_MyBytes2, ""), vnil(&ty_arr0_MyByte));
    E(vbytes_s(a, &ty_MyBytes2, "AA"), varr(a, &ty_arr2_MyByte, "AA"));
    E(vbytes_s(a, &ty_MyBytes2, ""), vbytes_s(a, &ty_sl_MyByte, ""));
    E(vbytes_s(a, &ty_sl_MyByte, ""), vbytes_s(a, &ty_MyBytes2, ""));
    E(vrunes(a, &ty_sl_MyRune, "namedRuneA"), vrunes(a, &ty_MyRunes2, "namedRuneA"));
    E(vrunes(a, &ty_MyRunes2, "namedRuneB"), vrunes(a, &ty_sl_MyRune, "namedRuneB"));

    /* slice to array */
    static const Byte b1[] = {1};
    static const Byte b12[] = {1, 2};
    static const Byte b123[] = {1, 2, 3};
    static const Byte b1234[] = {1, 2, 3, 4};
    static const Byte b5678[] = {5, 6, 7, 8};
    static const Byte b7[] = {7};
    static const Byte b9[] = {9};
    E(vnil(TYPE_BYTES), vnil(&ty_arr0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vnil(&ty_arr0));
    E(vbytes(a, TYPE_BYTES, b1, 1), varr(a, &ty_arr1, b1));
    E(vbytes(a, TYPE_BYTES, b12, 2), varr(a, &ty_arr2, b12));
    E(vbytes(a, TYPE_BYTES, b123, 3), varr(a, &ty_arr3, b123));
    E(vnil(&ty_MyBytes), vnil(&ty_arr0));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vnil(&ty_arr0));
    E(vbytes(a, &ty_MyBytes, b1, 1), varr(a, &ty_arr1, b1));
    E(vbytes(a, &ty_MyBytes, b12, 2), varr(a, &ty_arr2, b12));
    E(vbytes(a, &ty_MyBytes, b123, 3), varr(a, &ty_arr3, b123));
    E(vnil(TYPE_BYTES), vnil(&ty_MyBytesArray0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vnil(&ty_MyBytesArray0));
    E(vbytes(a, TYPE_BYTES, b1234, 4), varr(a, &ty_MyBytesArray, b1234));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vnil(&ty_MyBytesArray0));
    E(vbytes(a, &ty_MyBytes, b5678, 4), varr(a, &ty_MyBytesArray, b5678));
    E(vbytes(a, &ty_sl_MyByte, NULL, 0), vnil(&ty_arr0_MyByte));
    E(vbytes(a, &ty_sl_MyByte, b12, 2), varr(a, &ty_arr2_MyByte, b12));

    /* slice to array pointer */
    E(vnil(TYPE_BYTES), vnil(&ty_p_arr0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vnew(a, &ty_p_arr0));
    E(vbytes(a, TYPE_BYTES, b7, 1), vptr(a, &ty_p_arr1, b7));
    E(vnil(&ty_MyBytes), vnil(&ty_p_arr0));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vnew(a, &ty_p_arr0));
    E(vbytes(a, &ty_MyBytes, b9, 1), vptr(a, &ty_p_arr1, b9));
    E(vnil(TYPE_BYTES), vnil(&ty_MyBytesArrayPtr0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vnew(a, &ty_MyBytesArrayPtr0));
    E(vbytes(a, TYPE_BYTES, b1234, 4), vptr(a, &ty_MyBytesArrayPtr, b1234));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vnew(a, &ty_MyBytesArrayPtr0));
    E(vbytes(a, &ty_MyBytes, b5678, 4), vptr(a, &ty_MyBytesArrayPtr, b5678));

    E(vnil(TYPE_BYTES), vnil(&ty_p_MyBytesArray0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vnew(a, &ty_p_MyBytesArray0));
    E(vbytes(a, TYPE_BYTES, b1234, 4), vptr(a, &ty_p_MyBytesArray, b1234));
    E(vnil(&ty_MyBytes), vnil(&ty_p_MyBytesArray0));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vnew(a, &ty_p_MyBytesArray0));
    E(vbytes(a, &ty_MyBytes, b5678, 4), vptr(a, &ty_p_MyBytesArray, b5678));
    E(vnew(a, &ty_p_arr0), vnew(a, &ty_p_MyBytesArray0));
    E(vnew(a, &ty_p_MyBytesArray0), vnew(a, &ty_p_arr0));
    E(vnil(&ty_MyBytesArrayPtr0), vnil(&ty_p_arr0));
    E(vnil(&ty_p_arr0), vnil(&ty_MyBytesArrayPtr0));

    /* named types and equal underlying types */
    E(vnew(a, &ty_p_int), vnew(a, &ty_p_integer));
    E(vnew(a, &ty_p_integer), vnew(a, &ty_p_int));
    E(vnil(&ty_Empty), vnil(&ty_struct0));
    E(vnew(a, &ty_p_Empty), vnew(a, &ty_p_struct0));
    E(vnil(&ty_struct0), vnil(&ty_Empty));
    E(vnew(a, &ty_p_struct0), vnew(a, &ty_p_Empty));
    E(vnil(&ty_Empty), vnil(&ty_Empty));
    E(vbytes(a, &ty_MyBytes, NULL, 0), vbytes(a, TYPE_BYTES, NULL, 0));
    E(vbytes(a, TYPE_BYTES, NULL, 0), vbytes(a, &ty_MyBytes, NULL, 0));
    E(vnil(&ty_func), vnil(&ty_MyFunc));
    E(vnil(&ty_MyFunc), vnil(&ty_func));

    /* structs with different tags */
    E(vnil(&ty_sx_foo), vnil(&ty_sx_bar));
    E(vnil(&ty_sx_bar), vnil(&ty_sx_foo));
    E(vnil(&ty_MyStruct), vnil(&ty_sx_foo));
    E(vnil(&ty_sx_foo), vnil(&ty_MyStruct));
    E(vnil(&ty_MyStruct), vnil(&ty_sx_bar));
    E(vnil(&ty_sx_bar), vnil(&ty_MyStruct));
    E(vnil(&ty_MyStruct1), vnil(&ty_MyStruct2));
    E(vnil(&ty_MyStruct2), vnil(&ty_MyStruct1));

    /* can convert *byte and *MyByte */
    E(vnil(&ty_p_byte), vnil(&ty_p_MyByte));
    E(vnil(&ty_p_MyByte), vnil(&ty_p_byte));

    /* cannot convert mismatched array sizes */
    E(vnil(&ty_arr2), vnil(&ty_arr2));
    E(vnil(&ty_arr3), vnil(&ty_arr3));
    E(vnil(&ty_MyBytesArray0), vnil(&ty_arr0));
    E(vnil(&ty_arr0), vnil(&ty_MyBytesArray0));

    /* cannot convert other instances */
    E(vnil(&ty_pp_byte), vnil(&ty_pp_byte));
    E(vnil(&ty_pp_MyByte), vnil(&ty_pp_MyByte));
    E(vnil(&ty_chan_byte), vnil(&ty_chan_byte));
    E(vnil(&ty_chan_MyByte), vnil(&ty_chan_MyByte));
    E(vnil(TYPE_BYTES), vnil(TYPE_BYTES));
    E(vnil(&ty_sl_MyByte), vnil(&ty_sl_MyByte));
    E(vnil(&ty_map_int_byte), vnil(&ty_map_int_byte));
    E(vnil(&ty_map_int_MyByte), vnil(&ty_map_int_MyByte));
    E(vnil(&ty_map_byte_int), vnil(&ty_map_byte_int));
    E(vnil(&ty_map_MyByte_int), vnil(&ty_map_MyByte_int));
    E(vnil(&ty_arr2), vnil(&ty_arr2));
    E(vnil(&ty_arr2_MyByte), vnil(&ty_arr2_MyByte));

    /* other */
    E(vnil(&ty_ppp_int), vnil(&ty_ppp_int));
    E(vnil(&ty_ppp_byte), vnil(&ty_ppp_byte));
    E(vnil(&ty_ppp_int32), vnil(&ty_ppp_int32));
    E(vnil(&ty_ppp_int64), vnil(&ty_ppp_int64));
    E(vnil(&ty_chan_byte), vnil(&ty_chan_byte));
    E(vnil(&ty_chan_MyByte), vnil(&ty_chan_MyByte));
    E(vnil(&ty_map_int_bool), vnil(&ty_map_int_bool));
    E(vnil(&ty_map_int_byte), vnil(&ty_map_int_byte));
    E(vnil(&ty_map_uint_bool), vnil(&ty_map_uint_bool));
    E(vnil(&ty_sl_uint), vnil(&ty_sl_uint));
    E(vnil(&ty_sl_int), vnil(&ty_sl_int));
    E(vnew(a, &ty_p_any), vnew(a, &ty_p_any));
    E(vnew(a, &ty_p_reader), vnew(a, &ty_p_reader));
    E(vnew(a, &ty_p_writer), vnew(a, &ty_p_writer));

    /* channels */
    E(vnil(&ty_IntChan), vnil(&ty_chan_int_send));
    E(vnil(&ty_IntChan), vnil(&ty_chan_int_recv));
    E(vnil(&ty_chan_int), vnil(&ty_IntChanRecv));
    E(vnil(&ty_chan_int), vnil(&ty_IntChanSend));
    E(vnil(&ty_IntChanRecv), vnil(&ty_chan_int_recv));
    E(vnil(&ty_chan_int_recv), vnil(&ty_IntChanRecv));
    E(vnil(&ty_IntChanSend), vnil(&ty_chan_int_send));
    E(vnil(&ty_chan_int_send), vnil(&ty_IntChanSend));
    E(vnil(&ty_IntChan), vnil(&ty_chan_int));
    E(vnil(&ty_chan_int), vnil(&ty_IntChan));
    E(vnil(&ty_chan_int), vnil(&ty_chan_int_recv));
    E(vnil(&ty_chan_int), vnil(&ty_chan_int_send));
    E(vnil(&ty_BytesChan), vnil(&ty_chan_bytes_send));
    E(vnil(&ty_BytesChan), vnil(&ty_chan_bytes_recv));
    E(vnil(&ty_chan_bytes), vnil(&ty_BytesChanRecv));
    E(vnil(&ty_chan_bytes), vnil(&ty_BytesChanSend));
    E(vnil(&ty_BytesChanRecv), vnil(&ty_chan_bytes_recv));
    E(vnil(&ty_chan_bytes_recv), vnil(&ty_BytesChanRecv));
    E(vnil(&ty_BytesChanSend), vnil(&ty_chan_bytes_send));
    E(vnil(&ty_chan_bytes_send), vnil(&ty_BytesChanSend));
    E(vnil(&ty_BytesChan), vnil(&ty_chan_bytes));
    E(vnil(&ty_chan_bytes), vnil(&ty_BytesChan));
    E(vnil(&ty_chan_bytes), vnil(&ty_chan_bytes_recv));
    E(vnil(&ty_chan_bytes), vnil(&ty_chan_bytes_send));

    /* cannot convert other instances (channels) */
    E(vnil(&ty_IntChan), vnil(&ty_IntChan));
    E(vnil(&ty_IntChanRecv), vnil(&ty_IntChanRecv));
    E(vnil(&ty_IntChanSend), vnil(&ty_IntChanSend));
    E(vnil(&ty_BytesChan), vnil(&ty_BytesChan));
    E(vnil(&ty_BytesChanRecv), vnil(&ty_BytesChanRecv));
    E(vnil(&ty_BytesChanSend), vnil(&ty_BytesChanSend));

    /* interfaces */
    ReflectType buf = reflect_pointer_to(TYPE_BYTES_BUFFER);
    E(vnum(a, TYPE_INT, 1), vany(a, vnum(a, TYPE_INT, 1)));
    E(vstr(a, TYPE_STRING, "hello"), vany(a, vstr(a, TYPE_STRING, "hello")));
    E(vnew(a, buf), vreader(a));
    E(vread_writer(a), vreader(a));
    E(vnew(a, buf), vread_writer(a));
#undef BISHOP
#undef QUEEN
#undef MONKEYS
#undef BAD
    return n;
}

#undef E

/* ------------------------------------------------------------------- tests */

static bool deep_equal(ReflectValue x, ReflectValue y) {
    return reflect_deep_equal(reflect_value_interface(x), reflect_value_interface(y));
}

/* A set of types, and of pairs of them, small enough to search in a line. */
typedef struct TypeSet {
    const Type *v[256];
    Int n;
} TypeSet;

static void type_set_add(TestingT *t, TypeSet *s, const Type *x) {
    for (Int i = 0; i < s->n; i++)
        if (s->v[i] == x)
            return;
    if (s->n == NELEM(s->v)) {
        testing_t_fatalf_v(t, "too many types");
        return;
    }
    s->v[s->n++] = x;
}

typedef struct PairSet {
    const Type *v[512][2];
    Int n;
} PairSet;

static bool pair_set_has(const PairSet *s, const Type *x, const Type *y) {
    for (Int i = 0; i < s->n; i++)
        if (s->v[i][0] == x && s->v[i][1] == y)
            return true;
    return false;
}

static void pair_set_add(TestingT *t, PairSet *s, const Type *x, const Type *y) {
    if (pair_set_has(s, x, y))
        return;
    if (s->n == NELEM(s->v)) {
        testing_t_fatalf_v(t, "too many pairs");
        return;
    }
    s->v[s->n][0] = x;
    s->v[s->n][1] = y;
    s->n++;
}

static void TestConvert(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ConvertTest *tab = mem_alloc(a, 512 * sizeof(ConvertTest), _Alignof(ConvertTest));
    static TypeSet all;
    static PairSet can_convert;
    all.n = 0;
    can_convert.n = 0;
    Int n = convert_tests(a, tab);
    for (Int i = 0; i < n; i++) {
        ConvertTest tt = tab[i];
        ReflectType t1 = reflect_value_type(tt.in);
        if (!reflect_type_convertible_to(t1, t1)) {
            testing_t_errorf_v(t, "(%s).ConvertibleTo(%s) = false, want true", ts(t1),
                               ts(t1));
            continue;
        }

        ReflectType t2 = reflect_value_type(tt.out);
        if (!reflect_type_convertible_to(t1, t2)) {
            testing_t_errorf_v(t, "(%s).ConvertibleTo(%s) = false, want true", ts(t1),
                               ts(t2));
            continue;
        }

        type_set_add(t, &all, t1);
        type_set_add(t, &all, t2);
        pair_set_add(t, &can_convert, t1, t2);

        /* vout1 represents the in value converted to the in type. */
        ReflectValue v1 = tt.in;
        if (!reflect_value_can_convert(v1, t1))
            testing_t_errorf_v(t, "ValueOf(%s).CanConvert(%s) = false, want true",
                               ts(t1), ts(t1));
        ReflectValue vout1 = reflect_value_convert(v1, a, t1);
        if (reflect_value_type(vout1) != t1 || !deep_equal(vout1, tt.in))
            testing_t_errorf_v(
                t, "#%d: ValueOf(%s).Convert(%s) = %s, not the value it was", i, ts(t1),
                ts(t1), ts(reflect_value_type(vout1)));

        /* vout2 represents the in value converted to the out type. */
        if (!reflect_value_can_convert(v1, t2))
            testing_t_errorf_v(t, "ValueOf(%s).CanConvert(%s) = false, want true",
                               ts(t1), ts(t2));
        ReflectValue vout2 = reflect_value_convert(v1, a, t2);
        if (reflect_value_type(vout2) != t2 || !deep_equal(vout2, tt.out))
            testing_t_errorf_v(
                t, "#%d: ValueOf(%s).Convert(%s) = %s, not the value wanted", i, ts(t1),
                ts(t2), ts(reflect_value_type(vout2)));
        if (reflect_value_kind(vout2) != reflect_type_kind(reflect_value_type(vout2)))
            testing_t_errorf_v(t,
                               "ValueOf(%s).Convert(%s) has internal kind %v want %v",
                               ts(t1), ts(t2), reflect_value_kind(vout2),
                               reflect_type_kind(reflect_value_type(vout2)));

        /* vout3 represents a new value of the out type, set to vout2. This
         * makes sure the converted value vout2 is really usable as a regular
         * value. */
        ReflectValue vout3 = reflect_value_elem(reflect_new(a, t2));
        reflect_value_set(vout3, vout2);
        if (reflect_value_type(vout3) != t2 || !deep_equal(vout3, tt.out))
            testing_t_errorf_v(
                t, "#%d: Set(ValueOf(%s).Convert(%s)) is not the value wanted", i,
                ts(t1), ts(t2));

        if (is_ro(v1))
            testing_t_errorf_v(t, "table entry %d is RO, should not be", i);
        if (is_ro(vout1))
            testing_t_errorf_v(t, "self-conversion output %d is RO, should not be", i);
        if (is_ro(vout2))
            testing_t_errorf_v(t, "conversion output %d is RO, should not be", i);
        if (is_ro(vout3))
            testing_t_errorf_v(t, "set(conversion output) %d is RO, should not be", i);
        if (!is_ro(reflect_value_convert(make_ro(v1), a, t1)))
            testing_t_errorf_v(t, "RO self-conversion output %d is not RO, should be",
                               i);
        if (!is_ro(reflect_value_convert(make_ro(v1), a, t2)))
            testing_t_errorf_v(t, "RO conversion output %d is not RO, should be", i);
    }

    /* Assume that of all the types we saw during the tests, if there wasn't an
     * explicit entry for a conversion between a pair of types, then it's not to
     * be allowed. This checks for things like 'int64' converting to '*int'. */
    for (Int i = 0; i < all.n; i++) {
        for (Int j = 0; j < all.n; j++) {
            ReflectType t1 = all.v[i];
            ReflectType t2 = all.v[j];
            bool expect_ok = t1 == t2 || pair_set_has(&can_convert, t1, t2) ||
                             (reflect_type_kind(t2) == REFLECT_INTERFACE &&
                              reflect_type_num_method(t2) == 0);
            bool ok = reflect_type_convertible_to(t1, t2);
            if (ok != expect_ok)
                testing_t_errorf_v(t, "(%s).ConvertibleTo(%s) = %t, want %t", ts(t1),
                                   ts(t2), ok, expect_ok);
        }
    }
    arena_free(&ar);
}

static void TestConvertPanic(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectValue v = vbytes(a, TYPE_BYTES, "\0\0\0\0", 4);
    ReflectType pt = reflect_pointer_to(reflect_array_of(8, TYPE_UINT8));
    if (!reflect_type_convertible_to(reflect_value_type(v), pt))
        testing_t_errorf_v(t, "[]byte should be convertible to *[8]byte");
    if (reflect_value_can_convert(v, pt))
        testing_t_errorf_v(t,
                           "slice with length 4 should not be convertible to *[8]byte");
    Op o = {a, v, pt};
    should_panic(t,
                 "reflect: cannot convert slice with length 4 to pointer to array with "
                 "length 8",
                 op_convert, &o);

    if (reflect_value_can_convert(v, reflect_type_elem(pt)))
        testing_t_errorf_v(t,
                           "slice with length 4 should not be convertible to [8]byte");
    o.t = reflect_type_elem(pt);
    should_panic(t,
                 "reflect: cannot convert slice with length 4 to array with length 8",
                 op_convert, &o);
    arena_free(&ar);
}

static void TestConvertSlice2Array(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Int zero[4] = {0, 0, 0, 0};
    ReflectValue ov = vbytes(a, reflect_slice_of(TYPE_INT), zero, 0);
    Slice s = slice_make(a, TYPE_INT, 4, 4);
    memcpy(ov.ptr, &s, sizeof s);
    ReflectType pt = reflect_array_of(4, TYPE_INT);
    ReflectValue v = reflect_value_convert(ov, a, pt);
    /* Converting a slice to non-empty array needs to return a non-addressable
     * copy of the original memory. */
    if (reflect_value_can_addr(v)) {
        testing_t_fatalf_v(
            t, "convert slice to non-empty array returns an addressable copy "
               "array");
        arena_free(&ar);
        return;
    }
    for (Int i = 0; i < s.len; i++) {
        Int x = i + 1;
        reflect_value_set(reflect_value_index(ov, i), val(TYPE_INT, &x));
    }
    for (Int i = 0; i < s.len; i++) {
        if (reflect_value_int(reflect_value_index(v, i)) != 0) {
            testing_t_fatalf_v(t, "slice mutation visible in converted result");
            break;
        }
    }
    arena_free(&ar);
}

/* gFloat32 in Go: a global, so the store and the load are not optimised away. */
static volatile float g_float32;

#define SNAN UINT32_C(0x7f800001)

static uint32_t float32_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static float float32_from_bits(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static void TestConvertNaNs(TestingT *t) {
    /* Test to see if a store followed by a load of a signaling NaN maintains
     * the signaling bit. (This used to fail on the 387 port.) */
    g_float32 = float32_from_bits(SNAN);
    if (float32_bits(g_float32) != SNAN)
        testing_t_errorf_v(t, "store/load of sNaN not faithful, got %x want %x",
                           float32_bits(g_float32), SNAN);
    /* Test reflect's conversion between float32s. See issue 36400. */
    static const Type myFloat32 =
        DESC(S_("myFloat32"), TEST_PKG, KIND_FLOAT32, 4, 4, 0, NULL, NULL, NULL, 0);
    uint32_t bits = SNAN;
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectValue x = val(&myFloat32, &bits);
    ReflectValue y = reflect_value_convert(x, a, TYPE_FLOAT32);
    Any z = reflect_value_interface(y);
    float f;
    memcpy(&f, z.data, sizeof f);
    if (float32_bits(f) != SNAN)
        testing_t_errorf_v(t, "signaling nan conversion got %x, want %x",
                           float32_bits(f), SNAN);
    arena_free(&ar);
}

#define COMPARABLESTRUCT_FIELDS(F, Ty) F(Ty, Int, X, "")
BURROW_STRUCT(ComparableStruct, COMPARABLESTRUCT_FIELDS);

static void TestComparable(TestingT *t) {
    /* NonComparableStruct's Y is a map[string]int, written out here since a
     * Map * field in a declared struct would come out as a pointer. */
    ReflectType m = reflect_map_of(TYPE_STRING, TYPE_INT);
    const Field ncs_fields[] = {
        {S_("X"), NONAME, TYPE_INT, 0},
        {S_("Y"), NONAME, m, (uint32_t)sizeof(Int)},
    };
    const Type ncs =
        DESC(S_("NonComparableStruct"), TEST_PKG, KIND_STRUCT,
             sizeof(Int) + sizeof(Map *), _Alignof(Int), 2, ncs_fields, NULL, NULL, 0);
    struct {
        ReflectType typ;
        bool ok;
    } tests[] = {
        {TYPE_INT, true},
        {TYPE_STRING, true},
        {reflect_pointer_to(TYPE_UINT8), true},
        {&ty_func, false},
        {TYPE_BYTES, false},
        {m, false},
        {reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT), true},
        {TYPE_FLOAT64, true},
        {TYPE_BOOL, true},
        {TYPE_COMPLEX128, true},
        {TYPE_OF(ComparableStruct), true},
        {&ncs, false},
        {reflect_array_of(10, m), false},
        {reflect_array_of(10, TYPE_STRING), true},
        {reflect_type_elem(reflect_pointer_to(TYPE_ANY)), true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool ok = reflect_type_comparable(tests[i].typ);
        if (ok != tests[i].ok)
            testing_t_errorf_v(t, "TypeOf(%s).Comparable() = %t, want %t",
                               ts(tests[i].typ), ok, tests[i].ok);
    }
}

/* example1.MyStruct and example2.MyStruct, from reflect/internal: the same
 * declaration in two packages.
 *
 *     type MyStruct struct {
 *         MyStructs []MyStruct
 *         MyStruct  *MyStruct
 *     } */
static const Type ex1_MyStruct;
static const Type ex2_MyStruct;
UNNAMED(ex1_slice, KIND_SLICE, Slice, &ex1_MyStruct, NULL, 0);
UNNAMED(ex1_ptr, KIND_POINTER, void *, &ex1_MyStruct, NULL, 0);
UNNAMED(ex2_slice, KIND_SLICE, Slice, &ex2_MyStruct, NULL, 0);
UNNAMED(ex2_ptr, KIND_POINTER, void *, &ex2_MyStruct, NULL, 0);
static const Field ex1_fields[] = {
    {S_("MyStructs"), NONAME, &ex1_slice, 0},
    {S_("MyStruct"), NONAME, &ex1_ptr, (uint32_t)sizeof(Slice)},
};
static const Field ex2_fields[] = {
    {S_("MyStructs"), NONAME, &ex2_slice, 0},
    {S_("MyStruct"), NONAME, &ex2_ptr, (uint32_t)sizeof(Slice)},
};
static const Type ex1_MyStruct =
    DESC(S_("MyStruct"), S_("reflect/internal/example1"), KIND_STRUCT,
         sizeof(Slice) + sizeof(void *), _Alignof(Slice), 2, ex1_fields, NULL, NULL, 0);
static const Type ex2_MyStruct =
    DESC(S_("MyStruct"), S_("reflect/internal/example2"), KIND_STRUCT,
         sizeof(Slice) + sizeof(void *), _Alignof(Slice), 2, ex2_fields, NULL, NULL, 0);

static void TestConvertibleTo(TestingT *t) {
    ReflectType t1 = &ex1_MyStruct;
    ReflectType t2 = &ex2_MyStruct;

    /* Shouldn't raise stack overflow */
    if (reflect_type_convertible_to(t1, t2)) {
        testing_t_fatalf_v(t, "(%s).ConvertibleTo(%s) = true, want false", ts(t1),
                           ts(t2));
        return;
    }

    ReflectType t3 = &ex1_slice;
    ReflectType t4 = &ex2_slice;

    if (reflect_type_convertible_to(t3, t4))
        testing_t_fatalf_v(t, "(%s).ConvertibleTo(%s) = true, want false", ts(t3),
                           ts(t4));
}

/* type IntPtr *int, type IntPtr1 *int and type Ch <-chan any, from
 * set_test.go. */
NAMED(ty_IntPtr, "IntPtr", KIND_POINTER, void *, &burrow_type_Int, NULL, 0);
NAMED(ty_IntPtr1, "IntPtr1", KIND_POINTER, void *, &burrow_type_Int, NULL, 0);
NAMED(ty_Ch, "Ch", KIND_CHAN, void *, &burrow_type_Any, NULL, RECV);
UNNAMED(ty_chan_any_recv, KIND_CHAN, void *, &burrow_type_Any, NULL, RECV);

static void TestImplements(TestingT *t) {
    ImplementsTest tests[16];
    Int n = implements_tests(tests);
    for (Int i = 0; i < n; i++) {
        bool b = reflect_type_implements(tests[i].x, tests[i].t);
        if (b != tests[i].b)
            testing_t_errorf_v(t, "(%s).Implements(%s) = %t, want %t", ts(tests[i].x),
                               ts(tests[i].t), b, tests[i].b);
    }
}

static void TestAssignableTo(TestingT *t) {
    ImplementsTest tests[32] = {
        {&ty_chan_int, &ty_chan_int_recv, true},
        {&ty_chan_int_recv, &ty_chan_int, false},
        {&ty_p_int, &ty_IntPtr, true},
        {&ty_IntPtr, &ty_p_int, true},
        {&ty_IntPtr, &ty_IntPtr1, false},
        {&ty_Ch, &ty_chan_any_recv, true},
        /* test runs implementsTests too */
    };
    Int n = 6 + implements_tests(tests + 6);
    for (Int i = 0; i < n; i++) {
        bool b = reflect_type_assignable_to(tests[i].x, tests[i].t);
        if (b != tests[i].b)
            testing_t_errorf_v(t, "(%s).AssignableTo(%s) = %t, want %t", ts(tests[i].x),
                               ts(tests[i].t), b, tests[i].b);
    }
}

static void TestImplicitMapConversion(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Int one = 1;
    Int two = 2;
    ReflectValue v1 = val(TYPE_INT, &one);
    ReflectValue v2 = val(TYPE_INT, &two);
    {
        /* direct */
        ReflectValue mv = reflect_make_map(a, reflect_map_of(TYPE_INT, TYPE_INT));
        reflect_value_set_map_index(mv, v1, v2);
        Map *m = *(Map **)mv.ptr;
        const Int *x = map_get(m, &one);
        if (x == NULL || *x != 2)
            testing_t_errorf_v(t, "#1 after SetMapIndex(1,2): %d", x == NULL ? 0 : *x);
        Any n = reflect_value_interface(reflect_value_map_index(mv, v1));
        if (n.t != TYPE_INT || *(const Int *)n.data != 2)
            testing_t_errorf_v(t, "#1 MapIndex(1) is not 2");
    }
    {
        /* convert interface key */
        ReflectValue mv = reflect_make_map(a, reflect_map_of(TYPE_ANY, TYPE_INT));
        reflect_value_set_map_index(mv, v1, v2);
        Map *m = *(Map **)mv.ptr;
        Any k = BURROW_ANY(TYPE_INT, &one);
        const Int *x = map_get(m, &k);
        if (x == NULL || *x != 2)
            testing_t_errorf_v(t, "#2 after SetMapIndex(1,2): %d", x == NULL ? 0 : *x);
        Any n = reflect_value_interface(reflect_value_map_index(mv, v1));
        if (n.t != TYPE_INT || *(const Int *)n.data != 2)
            testing_t_errorf_v(t, "#2 MapIndex(1) is not 2");
    }
    {
        /* convert interface value */
        ReflectValue mv = reflect_make_map(a, reflect_map_of(TYPE_INT, TYPE_ANY));
        reflect_value_set_map_index(mv, v1, v2);
        Map *m = *(Map **)mv.ptr;
        const Any *x = map_get(m, &one);
        if (x == NULL || x->t != TYPE_INT || *(const Int *)x->data != 2)
            testing_t_errorf_v(t, "#3 after SetMapIndex(1,2): not 2");
        Any n = reflect_value_interface(reflect_value_map_index(mv, v1));
        if (n.t != TYPE_INT || *(const Int *)n.data != 2)
            testing_t_errorf_v(t, "#3 MapIndex(1) is not 2");
    }
    {
        /* convert both interface key and interface value */
        ReflectValue mv = reflect_make_map(a, reflect_map_of(TYPE_ANY, TYPE_ANY));
        reflect_value_set_map_index(mv, v1, v2);
        Map *m = *(Map **)mv.ptr;
        Any k = BURROW_ANY(TYPE_INT, &one);
        const Any *x = map_get(m, &k);
        if (x == NULL || x->t != TYPE_INT || *(const Int *)x->data != 2)
            testing_t_errorf_v(t, "#4 after SetMapIndex(1,2): not 2");
        Any n = reflect_value_interface(reflect_value_map_index(mv, v1));
        if (n.t != TYPE_INT || *(const Int *)n.data != 2)
            testing_t_errorf_v(t, "#4 MapIndex(1) is not 2");
    }
    {
        /* convert both, with non-empty interfaces */
        ReflectValue mv =
            reflect_make_map(a, reflect_map_of(TYPE_OF(IoReader), TYPE_OF(IoWriter)));
        ReflectType pb = reflect_pointer_to(TYPE_BYTES_BUFFER);
        BytesBuffer *b1 = mk(a, TYPE_BYTES_BUFFER);
        BytesBuffer *b2 = mk(a, TYPE_BYTES_BUFFER);
        reflect_value_set_map_index(mv, val(pb, &b1), val(pb, &b2));
        Map *m = *(Map **)mv.ptr;
        IoReader k = bytes_buffer_as_io_reader(b1);
        const IoWriter *x = map_get(m, &k);
        if (x == NULL || x->vt != bytes_buffer_as_io_writer(b2).vt || x->data != b2)
            testing_t_errorf_v(t, "#5 after SetMapIndex(b1, b2): not b2");
        /* Go's Elem gives back the *bytes.Buffer. An interface here holds a
         * pointer to the value and its vtable names bytes.Buffer, so Elem
         * gives the Buffer b2 points at. */
        ReflectValue e = reflect_value_elem(reflect_value_map_index(mv, val(pb, &b1)));
        if (e.ptr != b2)
            testing_t_errorf_v(t, "#5 MapIndex(b1) is not b2");
    }
    {
        /* convert channel direction */
        ReflectType ct = reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT);
        ReflectValue mv = reflect_make_map(
            a, reflect_map_of(reflect_chan_of(REFLECT_RECV_DIR, TYPE_INT), ct));
        ReflectValue c1 = reflect_make_chan(a, ct, 0);
        ReflectValue c2 = reflect_make_chan(a, ct, 0);
        reflect_value_set_map_index(mv, c1, c2);
        Map *m = *(Map **)mv.ptr;
        void *const *x = map_get(m, c1.ptr);
        if (x == NULL || *x != *(void **)c2.ptr)
            testing_t_errorf_v(t, "#6 after SetMapIndex(c1, c2): not c2");
        if (reflect_value_unsafe_pointer(reflect_value_map_index(mv, c1)) !=
            reflect_value_unsafe_pointer(c2))
            testing_t_errorf_v(t, "#6 MapIndex(c1) is not c2");
        reflect_value_close(c1);
        reflect_value_close(c2);
    }
    {
        /* convert identical underlying types: type MyBuffer bytes.Buffer */
        /* Static, since the pointer type made from it lives on. It is
         * filled in here as bytes.Buffer's descriptor is not a constant. */
        static Type my_buffer;
        my_buffer = (Type)DESC(S_("MyBuffer"), TEST_PKG, TYPE_BYTES_BUFFER->kind,
                               TYPE_BYTES_BUFFER->size, TYPE_BYTES_BUFFER->align,
                               TYPE_BYTES_BUFFER->nfield, TYPE_BYTES_BUFFER->fields,
                               NULL, NULL, 0);
        ReflectType pb = reflect_pointer_to(&my_buffer);
        ReflectType pbb = reflect_pointer_to(TYPE_BYTES_BUFFER);
        ReflectValue mv = reflect_make_map(a, reflect_map_of(pb, pbb));
        ReflectValue b1 = reflect_new(a, &my_buffer);
        ReflectValue b2 = reflect_new(a, TYPE_BYTES_BUFFER);
        reflect_value_set_map_index(mv, b1, b2);
        Map *m = *(Map **)mv.ptr;
        void *const *x = map_get(m, b1.ptr);
        if (x == NULL || *x != *(void **)b2.ptr)
            testing_t_errorf_v(t, "#7 after SetMapIndex(b1, b2): not b2");
        if (reflect_value_unsafe_pointer(reflect_value_map_index(mv, b1)) !=
            *(void **)b2.ptr)
            testing_t_errorf_v(t, "#7 MapIndex(b1) is not b2");
    }
    arena_free(&ar);
}

static void TestImplicitSetConversion(TestingT *t) {
    /* Assume TestImplicitMapConversion covered the basics. Just make sure
     * conversions are being applied at all. */
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    IoReader r = {NULL, NULL};
    BytesBuffer *b = mk(a, TYPE_BYTES_BUFFER);
    ReflectValue rv = settable(TYPE_OF(IoReader), &r);
    reflect_value_set(rv, val(reflect_pointer_to(TYPE_BYTES_BUFFER), &b));
    if (r.vt != bytes_buffer_as_io_reader(b).vt || r.data != b)
        testing_t_errorf_v(t, "after Set: r does not hold b");
    arena_free(&ar);
}

static void TestImplicitSendConversion(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectValue c =
        reflect_make_chan(a, reflect_chan_of(REFLECT_BOTH_DIR, TYPE_OF(IoReader)), 10);
    BytesBuffer *b = mk(a, TYPE_BYTES_BUFFER);
    reflect_value_send(c, val(reflect_pointer_to(TYPE_BYTES_BUFFER), &b));
    bool ok = false;
    ReflectValue bb = reflect_value_recv(c, a, &ok);
    const IoReader *r = bb.ptr;
    if (!ok || r->vt != bytes_buffer_as_io_reader(b).vt || r->data != b)
        testing_t_errorf_v(t, "Received something other than b");
    reflect_value_close(c);
    arena_free(&ar);
}

/* io.WriteString, func(w io.Writer, s string) (n int, err error), whose two
 * results take a call thunk written out by hand. */
BURROW_FUNC(WriteStringFn, Int, IoWriter w, Str s, Error *err);

static Int write_string_adapter(void *env, IoWriter w, Str s, Error *err) {
    (void)env;
    return io_write_string(w, s, err);
}

static void write_string_call(const void *fn, void **args, void **rets) {
    const WriteStringFn *f = fn;
    Error err = BURROW_NO_ERROR;
    *(Int *)rets[0] = f->f(f->env, *(IoWriter *)args[0], *(Str *)args[1], &err);
    *(Error *)rets[1] = err;
}

static const TypeOps write_string_ops = {NULL, NULL, NULL, NULL, write_string_call};
static const Field write_string_sig[] = {
    {S_(""), S_(""), TYPE_OF(IoWriter), 0},
    {S_(""), S_(""), TYPE_STRING, 1},
    {S_(""), S_(""), TYPE_INT, 0},
    {S_(""), S_(""), TYPE_OF(Error), 0},
};
static const Type ty_write_string = {
    NONAME,
    NONAME,
    KIND_FUNC,
    sizeof(Func),
    _Alignof(Func),
    4,
    0,
    write_string_sig,
    NULL,
    NULL,
    NULL,
    2,
    0,
    &write_string_ops,
};

static void TestImplicitCallConversion(TestingT *t) {
    /* Arguments must be assignable to parameter types. */
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    WriteStringFn fn = BURROW_FN(WriteStringFn, write_string_adapter, NULL);
    ReflectValue fv = val(&ty_write_string, &fn);
    StringsBuilder b = STRINGS_BUILDER(a);
    StringsBuilder *bp = &b;
    Str hello = BURROW_S("hello world");
    ReflectValue in[] = {val(reflect_pointer_to(TYPE_STRINGS_BUILDER), &bp),
                         val(TYPE_STRING, &hello)};
    reflect_value_call(fv, a, slice_from(in, 2, 2, TYPE_REFLECT_VALUE));
    if (!str_eq(strings_builder_string(&b), BURROW_S("hello world")))
        testing_t_errorf_v(t, "After call: string=%q want %q",
                           strings_builder_string(&b), "hello world");
    arena_free(&ar);
}

static void TestImplicitAppendConversion(TestingT *t) {
    /* Arguments must be assignable to the slice's element type. */
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Slice s = {NULL, 0, 0, TYPE_OF(IoReader)};
    ReflectValue sv = settable(reflect_slice_of(TYPE_OF(IoReader)), &s);
    BytesBuffer *b = mk(a, TYPE_BYTES_BUFFER);
    ReflectValue in[] = {val(reflect_pointer_to(TYPE_BYTES_BUFFER), &b)};
    reflect_value_set(sv,
                      reflect_append(a, sv, slice_from(in, 1, 1, TYPE_REFLECT_VALUE)));
    const IoReader *r = s.p;
    if (s.len != 1 || r[0].vt != bytes_buffer_as_io_reader(b).vt || r[0].data != b)
        testing_t_errorf_v(t, "after append: s does not hold b");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestConvert)                                                                     \
    X(TestConvertPanic)                                                                \
    X(TestConvertSlice2Array)                                                          \
    X(TestConvertNaNs)                                                                 \
    X(TestComparable)                                                                  \
    X(TestConvertibleTo)                                                               \
    X(TestImplements)                                                                  \
    X(TestAssignableTo)                                                                \
    X(TestImplicitMapConversion)                                                       \
    X(TestImplicitSetConversion)                                                       \
    X(TestImplicitSendConversion)                                                      \
    X(TestImplicitCallConversion)                                                      \
    X(TestImplicitAppendConversion)

TESTING_MAIN(TESTS)
