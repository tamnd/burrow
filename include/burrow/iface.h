/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Interfaces: the two word value, the vtable in front of it, and Any.
 *
 * Go has two kinds of interface and they need two representations in C, because
 * they are two different things wearing the same syntax.
 *
 * A non empty interface, io.Reader or sort.Interface or fmt.Stringer, is a
 * vtable pointer and a data pointer. The vtable is a static const struct per
 * implementing type, filled in at compile time, so satisfying an interface
 * costs one object in read only memory and a constructor that is two
 * assignments. Calling through it is a load and an indirect call, the same as
 * Go, with no lookup and no allocation.
 *
 *     typedef struct IoReaderVT {
 *         const Type *self_type;
 *         Int (*read)(void *self, Slice p, Error *err);
 *     } IoReaderVT;
 *
 *     typedef struct IoReader {
 *         const IoReaderVT *vt;
 *         void *data;
 *     } IoReader;
 *
 * An empty interface, Go's any, is a type descriptor and a data pointer, which
 * is Any below. Nothing is called through it, so there is no vtable to carry.
 * Any is what fmt's arguments, json.Marshal's parameter, sync.Map's values and
 * context.WithValue's value all become.
 *
 * Three rules hold for every interface in the library, and this header is where
 * they are written down.
 *
 * The vtable's first member is const Type *self_type. Always, and first. It is
 * what makes a type assertion possible on a value whose vtable has already
 * thrown away the concrete type, and because it is first, one cast reaches it
 * from any interface at all. A vtable may set it to NULL, which means the type
 * declines to be asserted to, the same thing an unexported type gets you in Go.
 *
 * A zeroed interface value is nil. vt == NULL is Go's nil interface and calling
 * a method on one is a nil dereference in both languages. This is why the vt
 * pointer comes first in the struct as well: a zeroed struct is a nil
 * interface, so a struct field of interface type starts out nil without anybody
 * writing a line to say so.
 *
 * Embedding is by member, not by cast. io.ReadWriter embeds io.Reader and
 * io.Writer, and the C vtable holds those vtables as named members rather than
 * repeating their function pointers:
 *
 *     typedef struct IoReadWriterVT {
 *         IoReaderVT reader;
 *         IoWriterVT writer;
 *     } IoReadWriterVT;
 *
 * Converting down is then taking the address of a member, with no cast and no
 * assumption about layout: (IoReader){&rw.vt->reader, rw.data}. The design
 * document originally called for a prefix compatible superset reached by a
 * pointer cast, which works for the first embedded interface and quietly does
 * not for the second. See docs/design/04-core-types.md section 5.
 */

#ifndef BURROW_IFACE_H
#define BURROW_IFACE_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The head of every vtable in the library.
 *
 * No vtable is declared as this type. It is what a vtable is cast to when
 * something needs the concrete type out of an interface whose own type is not
 * known, which is exactly what a type assertion and a type switch are. Every
 * vtable starting with the same member makes that cast defined rather than
 * clever: C guarantees a pointer to a struct points at its first member. */
typedef struct IfaceVT {
    const Type *self_type;
} IfaceVT;

/* Any interface value, with its own type forgotten.
 *
 * Every interface value in the library is a vtable pointer followed by a data
 * pointer, so every one of them converts to this and back without copying
 * anything. It is what the helpers below take, so that there is one type
 * assertion in the library rather than one per interface. */
typedef struct Iface {
    const IfaceVT *vt;
    void *data;
} Iface;

/* Convert any interface value to an Iface. The cast on the vtable is the one
 * described above and is why self_type has to be first. */
#define BURROW_IFACE(v) ((Iface){(const IfaceVT *)(v).vt, (void *)(v).data})

/* nil, the way Go means it for an interface. */
#define BURROW_IFACE_IS_NIL(v) ((v).vt == NULL)

/* Call a method. The receiver goes in first, which is what Go does too, it just
 * does not make you write it.
 *
 *     Int n = BURROW_CALL(r, read, buf, &err);
 *     Str s = BURROW_CALL0(v, string);
 *
 * Two macros rather than one because C99 needs at least one argument for the
 * ellipsis, and the trick that gets around that, __VA_OPT__, is C23. A zero
 * argument method is common enough (String, Close, Len) that the alternative
 * was worse.
 *
 * No nil check. Calling a method on a nil interface is a nil dereference, which
 * is what Go does as well, and a check here would cost a branch on every call
 * in the library to turn one crash into a different crash. */
#define BURROW_CALL(v, m, ...) ((v).vt->m((v).data, __VA_ARGS__))
#define BURROW_CALL0(v, m) ((v).vt->m((v).data))

/* The concrete type behind an interface value, or NULL when the value is nil or
 * its vtable declines to say. */
BURROW_STATIC(ret) const Type *iface_type(Iface v);

/* Go's v.(T), the one result form: the data pointer when the dynamic type is
 * want, and NULL otherwise. In C the pointer is the answer to both questions,
 * so there is no second result and no separate comma ok form.
 *
 *     OsFile *f = iface_assert(BURROW_IFACE(r), TYPE_OS_FILE);
 *     if (f != NULL) { ... }
 *
 * Go's one result assertion stops the program when it fails. This one does not,
 * because a C caller has the pointer in hand and can see for itself, and
 * because a library that ends the process over a failed conversion is not one
 * people can build on. */
BURROW_BORROWS(ret, v) void *iface_assert(Iface v, const Type *want);

/* ------------------------------------------------------------------ any
 *
 * Go's empty interface. A descriptor and a pointer, and a zeroed one is nil.
 *
 * The pointer is the part to be careful with. Go's interface values sometimes
 * hold a small value inside the word and sometimes point at a heap copy, and it
 * decides which invisibly. Any always points, so whatever it points at has to
 * outlive it. For the overwhelmingly common case, an argument to a printf, the
 * value is a compound literal whose lifetime is the enclosing block and that is
 * exactly long enough. When it has to outlive the block, any_box copies it into
 * an allocator. */
typedef struct Any {
    const Type *t;
    void *data;
} Any;

/* From a pointer you already have. The pointee has to outlive the Any. */
#define BURROW_ANY(t, ptr) ((Any){(t), (void *)(ptr)})

/* From a value, through a compound literal, which is the form that reads well
 * at a call site:
 *
 *     fmt_println(a, BURROW_ANY_VAL(TYPE_INT, Int, 42));
 *
 * The literal lives until the end of the enclosing block, so this is right for
 * an argument and wrong for anything stored. Storing it is what any_box is
 * for.
 *
 * T is a type and not an expression, so it cannot be wrapped in parentheses the
 * way the other two arguments are, which is what the suppression below is
 * about. */
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define BURROW_ANY_VAL(t, T, v) ((Any){(t), (void *)(T[]){(v)}})

#define BURROW_ANY_IS_NIL(v) ((v).t == NULL)

/* Copy what the Any points at into a, so the result no longer depends on the
 * caller's stack frame. Returns a nil Any when the allocator says no.
 *
 * The copy goes through the descriptor, so a type with its own copy operation
 * gets it. Nothing here deep copies: boxing an Any holding a Slice copies the
 * slice header and not its elements, which is what assigning a slice in Go does
 * too. */
BURROW_OWNS(ret) Any any_box(Alloc *a, Any v);

/* Go's v.(T) for the empty interface, with the same shape as iface_assert. */
BURROW_BORROWS(ret, v) void *any_assert(Any v, const Type *want);

/* Go's == on two interface values.
 *
 * Equal means the same dynamic type and equal values, and it goes through the
 * descriptor, so two Str values with different pointers and the same bytes are
 * equal. Two nil Any values are equal to each other and to nothing else.
 *
 * Go makes comparing two uncomparable values a run time panic rather than a
 * compile error, because it cannot know the dynamic types until it runs. This
 * does the same, through runtime_panic, with Go's message, so it is catchable
 * the same way Go's is recoverable. */
bool any_equal(Any a, Any b);

/* The descriptor for Any itself, so that map[any]T works, which is what json
 * decodes into and what sync.Map holds.
 *
 * Its equality is any_equal and its hash mixes the dynamic type into the hash
 * of the value, so an Any holding an Int(1) and an Any holding an Int8(1) are
 * different keys, which is what Go does. */
extern const Type *const TYPE_ANY;

/* A function that produces one value of any type, which is Go's `func() any`.
 *
 * It is here rather than in burrow/func.h because that file cannot see Any, and
 * it is one type rather than one per package because the packages that want it
 * all want the same thing: sync.OnceValue computes a value once, sync.Pool
 * makes one when the pool is empty. */
BURROW_FUNC0(AnyFunc, Any);

/* ------------------------------------------------------------ any from a value
 *
 * An Any from a plain C value, with the descriptor picked by the value's C type:
 *
 *     Int n = 42;
 *     Any a = BURROW_ANY_OF(n);        an int
 *     Any b = BURROW_ANY_OF(2.5);      a float64
 *     Any c = BURROW_ANY_OF("hello");  a string
 *
 * This is what fmt's _v forms do to every argument, and it is the reason those
 * read like a Go call. It copies the value into a compound literal, so an
 * rvalue works and the result lives until the end of the enclosing block, the
 * same rule BURROW_ANY_VAL has.
 *
 * C types do not map one to one onto Go types, so the choices are written down:
 *
 *     Int, and int64_t where it is the same type      int
 *     Uint, and uint64_t where it is the same type    uint
 *     int, unsigned, and int32_t where it is int      int, uint
 *     the other exact width integers                  int8 to uint64
 *     char                                            uint8, Go's byte
 *     long and long long                              by their size
 *     float, double                                   float32, float64
 *     bool, Complex64, Complex128, Str                themselves
 *     char * and const char *                         string, NULL is nil
 *     Error                                           error, a nil one is nil
 *     Slice, Map *                                    []elem and map[key]elem
 *     Any                                             passed through
 *     any other pointer                               unsafe.Pointer
 *
 * A struct has no entry because C cannot tell one struct from another here, and
 * passing one is a compile error rather than a guess. BURROW_ANY with the
 * struct's descriptor is the way to hand one over. */

/* Room for the value and, for a slice or a map, for the descriptor that has to
 * be made up on the spot, since []int is a type with no object of its own. */
typedef struct burrow__AnyBox {
    union {
        bool b;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        int64_t i64;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        Int i;
        Uint u;
        float f32;
        double f64;
        Complex64 c64;
        Complex128 c128;
        Str s;
        Error e;
        Slice sl;
        Map *m;
        const volatile void *p;
    } v;
    Type t;
} burrow__AnyBox;

BURROW_BORROWS(ret, v) Any burrow__any_of_any(Any v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_str(Str v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_error(Error v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_slice(Slice v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_map(Map *v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_c64(Complex64 v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_c128(Complex128 v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_bool(bool v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_f32(float v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_f64(double v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_cstr(const char *v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_int(Int v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_uint(Uint v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_cint(int v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_cuint(unsigned v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_i8(int8_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_i16(int16_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_i32(int32_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_i64(int64_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_u8(uint8_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_u16(uint16_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_u32(uint32_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_u64(uint64_t v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_char(char v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_long(long v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_ulong(unsigned long v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_llong(long long v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_ullong(unsigned long long v,
                                                   burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__any_of_ptr(const volatile void *v,
                                                burrow__AnyBox *box);

/* Five steps rather than one _Generic, because a _Generic may not name the same
 * type twice and the typedefs collide differently on every platform: int64_t is
 * long on one and long long on another, Int is int64_t, and int32_t is int.
 * Each step names only types that cannot be equal to each other, and whatever
 * an earlier step claimed never reaches a later one. */
#define BURROW__ANY_OF5(x)                                                             \
    _Generic((x),                                                                      \
        long: burrow__any_of_long,                                                     \
        unsigned long: burrow__any_of_ulong,                                           \
        long long: burrow__any_of_llong,                                               \
        unsigned long long: burrow__any_of_ullong,                                     \
        default: burrow__any_of_ptr)

#define BURROW__ANY_OF4(x)                                                             \
    _Generic((x),                                                                      \
        int8_t: burrow__any_of_i8,                                                     \
        int16_t: burrow__any_of_i16,                                                   \
        int32_t: burrow__any_of_i32,                                                   \
        int64_t: burrow__any_of_i64,                                                   \
        uint8_t: burrow__any_of_u8,                                                    \
        uint16_t: burrow__any_of_u16,                                                  \
        uint32_t: burrow__any_of_u32,                                                  \
        uint64_t: burrow__any_of_u64,                                                  \
        char: burrow__any_of_char,                                                     \
        default: BURROW__ANY_OF5(x))

#define BURROW__ANY_OF3(x)                                                             \
    _Generic((x),                                                                      \
        int: burrow__any_of_cint,                                                      \
        unsigned: burrow__any_of_cuint,                                                \
        default: BURROW__ANY_OF4(x))

#define BURROW__ANY_OF2(x)                                                             \
    _Generic((x),                                                                      \
        Int: burrow__any_of_int,                                                       \
        Uint: burrow__any_of_uint,                                                     \
        default: BURROW__ANY_OF3(x))

#define BURROW_ANY_OF(x)                                                               \
    _Generic((x),                                                                      \
        Any: burrow__any_of_any,                                                       \
        Str: burrow__any_of_str,                                                       \
        Error: burrow__any_of_error,                                                   \
        Slice: burrow__any_of_slice,                                                   \
        Map *: burrow__any_of_map,                                                     \
        Complex64: burrow__any_of_c64,                                                 \
        Complex128: burrow__any_of_c128,                                               \
        bool: burrow__any_of_bool,                                                     \
        float: burrow__any_of_f32,                                                     \
        double: burrow__any_of_f64,                                                    \
        char *: burrow__any_of_cstr,                                                   \
        const char *: burrow__any_of_cstr,                                             \
        default: BURROW__ANY_OF2(x))((x), &(burrow__AnyBox){.v = {0}})

#if defined(BURROW_SHORT) && BURROW_SHORT
#define ANY_OF(x) BURROW_ANY_OF(x)
#define CALL(v, m, ...) BURROW_CALL(v, m, __VA_ARGS__)
#define CALL0(v, m) BURROW_CALL0(v, m)
#define ANY(t, ptr) BURROW_ANY(t, ptr)
#define ANY_VAL(t, T, v) BURROW_ANY_VAL(t, T, v)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IFACE_H */
