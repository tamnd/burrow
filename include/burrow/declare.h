/* Declaring a type and its descriptor in one place.
 *
 * Everything in burrow that walks a value it has never seen before, which is
 * fmt, encoding/json, encoding/gob, text/template, database/sql and sort, works
 * by asking the value's descriptor what it is. A C compiler will not write one
 * of those for you, so it has to come from somewhere, and where it comes from
 * decides whether burrow keeps its promise that compiling it is a compiler
 * invocation and nothing else.
 *
 * So here it comes from the declaration:
 *
 *     #define POINT_FIELDS(F, T)                    \
 *         F(T, Int, X, "json:\"x\"")                \
 *         F(T, Int, Y, "json:\"y\"")                \
 *         F(T, Str, Label, "json:\"label,omitempty\"")
 *
 *     BURROW_STRUCT(Point, POINT_FIELDS);
 *
 * The semicolon belongs to you rather than to the macro, which is why it is
 * there. A macro that swallowed it would leave a stray one behind at file
 * scope, and burrow builds with -Wpedantic -Werror, where a stray semicolon at
 * file scope is an error.
 *
 * That gives you the struct, exactly as if you had written it out, and the
 * descriptor for it under the name TYPE_OF(Point):
 *
 *     Point p = {3, 4, BURROW_S("origin")};
 *
 *     const Type *t = TYPE_OF(Point);
 *     const Field *f = type_field_by_name(t, BURROW_S("Label"));
 *     printf("%u\n", f->offset);
 *
 * No generator, no build step, no annotations to keep in step with anything.
 * The struct and the descriptor are expanded from the same list, so they cannot
 * drift, and that is the property that makes this better than a generator
 * rather than merely cheaper than one. A field added to the list appears in
 * both or in neither.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DECLARE_H
#define BURROW_DECLARE_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WHAT IT COSTS
 *
 * The descriptor is a static const object. It costs no code, no startup work
 * and no registration: it is in read only memory before main starts, the same
 * as a string literal, and a linker asked to drop unused sections drops the
 * ones for types the program never mentions.
 *
 * WHY THE LIST LOOKS LIKE THAT
 *
 * The F is an X macro, which is the oldest trick in C and still the only one
 * that expands a list twice without a tool. The list is a macro taking the
 * thing to do to each line, and BURROW_STRUCT calls it twice with two different
 * things: once to emit members and once to emit descriptor entries.
 *
 * The T is there because an entry needs offsetof, and offsetof needs the struct
 * type. T is a parameter of the list rather than the type's name written into
 * every line, so there is nothing to keep in step and nothing to get wrong when
 * a type is renamed. Both of the names in the list header, F and T, are yours
 * to call whatever you like; these are the conventional spellings.
 *
 * It is not pretty. That is the honest cost, it is paid once per type in a
 * place nobody reads twice, and what it buys is a library that needs no build
 * system. For a codebase that will not rewrite its structs there is a second
 * way in, which is the libclang generator described in
 * docs/design/07-reflect.md section 4, and it produces the same descriptors.
 *
 * ONE TRANSLATION UNIT, OR TWO
 *
 * BURROW_STRUCT emits a definition, so it belongs in exactly one place. For a
 * type used in one .c file, put it there and stop reading.
 *
 * For a type in a header, the struct has to be in the header and the descriptor
 * has to be in one .c file, so it splits:
 *
 *     point.h:   BURROW_STRUCT_DECL(Point, POINT_FIELDS);
 *     point.c:   BURROW_STRUCT_DEFINE(Point, POINT_FIELDS);
 *
 * The same list both times. BURROW_STRUCT is the two of them together.
 *
 * WHAT IS NOT HERE
 *
 * There is no BURROW_ENUM and no BURROW_ALIAS. Both want the underlying type's
 * kind, and the only way to reach it from a name is TYPE_OF(U)->kind, which is
 * a load and not a constant expression, so it cannot go in the static
 * initialiser these descriptors are. Making them work needs either the kind
 * spelled out at the call site, which is a second place to get it wrong, or a
 * pass at startup, which is the registration this whole approach exists to
 * avoid. Neither is worth doing badly to fill a gap nothing has asked for yet.
 * See docs/design/07-reflect.md section 3. */

/* ------------------------------------------------- marking a plain struct
 *
 * The other way in. Write the struct the way you would have written it anyway,
 * mark it, and let tools/burrow-gen emit the descriptor:
 *
 *     typedef struct {
 *         Int X     BURROW_TAG("json:\"x\"");
 *         Int Y     BURROW_TAG("json:\"y\"");
 *         Str Label BURROW_TAG("json:\"label,omitempty\"");
 *     } Point BURROW_REFLECT;
 *
 *     burrow-gen reflect point.h -o point_gen.c -Iinclude
 *
 * A block comment reading burrow:reflect immediately above the typedef marks it
 * just as well, and that is the form docs/design/07-reflect.md uses because it
 * leaves the declaration untouched. Whichever you pick, the descriptor that
 * comes out is the one BURROW_STRUCT would have emitted from the same fields,
 * and burrow's own tests compare the two.
 *
 * In an ordinary build both of these expand to nothing at all, so the struct is
 * exactly the struct you wrote and there is no cost and nothing for another
 * compiler to choke on. They turn into annotations only when burrow-gen is the
 * one doing the parsing, because it defines BURROW_GEN and it is always clang.
 *
 * The reason the tag has to be a macro rather than a comment, when the marker
 * can be a comment, is that a comment attaches to a declaration and there is
 * one declaration for the whole struct. A tag belongs to a field. */
#ifdef BURROW_GEN
#define BURROW_TAG(s) __attribute__((annotate("burrow:tag:" s)))
#define BURROW_REFLECT __attribute__((annotate("burrow:reflect")))
#else
#define BURROW_TAG(s)
#define BURROW_REFLECT
#endif

/* The two things done to each line of a field list.
 *
 * Both take the same four arguments in the same order, which is what lets one
 * list serve both. T is the struct being declared, ctype is the field's type as
 * you would write it in C, fname is the field's name, and tag is its struct tag
 * or "" for none.
 *
 * These are spelled with the internal double underscore because you never write
 * one. You write the list; BURROW_STRUCT passes these in. */
#define BURROW__MEMBER(T, ctype, fname, tag) ctype fname;

#define BURROW__FIELD(T, ctype, fname, tag)                                            \
    {                                                                                  \
        BURROW_S_INIT(#fname),                                                         \
        BURROW_S_INIT(tag),                                                            \
        TYPE_OF(ctype),                                                                \
        (uint32_t)offsetof(T, fname),                                                  \
    },

/* The struct and the declaration of its descriptor. Header safe: nothing here
 * defines an object, so it can be included as many times as you like.
 *
 * The struct tag and the typedef get the same name, which is legal C and is
 * what lets a field point at the type it is declared in:
 *
 *     #define NODE_FIELDS(F, T)       \
 *         F(T, Int, Value, "")        \
 *         F(T, NodePtr, Next, "")
 *
 * with NodePtr a typedef for struct Node *, declared before the list. A pointer
 * cannot be written inline because TYPE_OF pastes its argument into a name and
 * a star is not part of one. */
#define BURROW_STRUCT_DECL(T, FIELDS)                                                  \
    typedef struct T {                                                                 \
        FIELDS(BURROW__MEMBER, T)                                                      \
    } T;                                                                               \
    extern const Type burrow_type_##T

/* The descriptor. Exactly one translation unit, because it defines two objects.
 *
 * The field array is static and the descriptor is not. That is the split the
 * rest of the library needs: the array is an implementation detail of this
 * type, and the descriptor has to be reachable from another file's static
 * initialiser so that a struct in one file can have a field of a type declared
 * in another. */
#define BURROW_STRUCT_DEFINE(T, FIELDS)                                                \
    static const Field burrow__fields_##T[] = {FIELDS(BURROW__FIELD, T)};              \
    BURROW__STRUCT_TYPE(T)

/* The descriptor around a field array already defined as burrow__fields_T. */
#define BURROW__STRUCT_TYPE(T)                                                         \
    const Type burrow_type_##T = {                                                     \
        BURROW_S_INIT(#T),                                                             \
        {NULL, 0},                                                                     \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        (uint16_t)(sizeof burrow__fields_##T / sizeof burrow__fields_##T[0]),          \
        0,                                                                             \
        burrow__fields_##T,                                                            \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* Both at once, for a type that lives in one file. */
#define BURROW_STRUCT(T, FIELDS)                                                       \
    BURROW_STRUCT_DECL(T, FIELDS);                                                     \
    BURROW_STRUCT_DEFINE(T, FIELDS)

/* A struct whose fields have one name in C and another in its descriptor.
 *
 * burrow's own structs name their fields the C way, so a PkixExtension has
 * .critical and not .Critical. A descriptor that said "critical" would make
 * the field unexported to every package that asks, the same as a lowercase
 * name in Go, and encoding/asn1 would refuse the struct the way Go's does. So
 * each line of the list carries the name the descriptor gets as well, which is
 * the Go name, after the C one:
 *
 *     #define EXTENSION_FIELDS(F, T)                              \
 *         F(T, Asn1ObjectIdentifier, id, Id, "")                  \
 *         F(T, bool, critical, Critical, "asn1:\"optional\"")
 *
 *     BURROW_STRUCT_AS(Extension, EXTENSION_FIELDS);
 *
 * Everything else is as for BURROW_STRUCT, including the DECL and DEFINE
 * split. */
#define BURROW__MEMBER_AS(T, ctype, fname, name, tag) ctype fname;

#define BURROW__FIELD_AS(T, ctype, fname, name, tag)                                   \
    {                                                                                  \
        BURROW_S_INIT(#name),                                                          \
        BURROW_S_INIT(tag),                                                            \
        TYPE_OF(ctype),                                                                \
        (uint32_t)offsetof(T, fname),                                                  \
    },

#define BURROW_STRUCT_AS_DECL(T, FIELDS)                                               \
    typedef struct T {                                                                 \
        FIELDS(BURROW__MEMBER_AS, T)                                                   \
    } T;                                                                               \
    extern const Type burrow_type_##T

#define BURROW_STRUCT_AS_DEFINE(T, FIELDS)                                             \
    static const Field burrow__fields_##T[] = {FIELDS(BURROW__FIELD_AS, T)};           \
    BURROW__STRUCT_TYPE(T)

#define BURROW_STRUCT_AS(T, FIELDS)                                                    \
    BURROW_STRUCT_AS_DECL(T, FIELDS);                                                  \
    BURROW_STRUCT_AS_DEFINE(T, FIELDS)

/* ------------------------------------------------------- composite types
 *
 * A struct field is not always a struct or a number. It can be a slice, an
 * array, a pointer or a map, and TYPE_OF needs a name to paste, so each of
 * those needs a descriptor with a name of its own.
 *
 * All four take the name first, because the name is yours to pick and C gives
 * no way to derive one. Go has no such problem: []int is a type and its own
 * spelling. The convention that reads best is the Go name with the punctuation
 * spelled out, so IntSlice, StrPtr, StrIntMap, but nothing enforces it.
 *
 * Each of these emits a definition, so the same one translation unit rule
 * applies. For a composite that appears in a header, use the DECL form and put
 * the plain one in a .c file, the same split BURROW_STRUCT has. */

/* A slice of T. The size is the size of a Slice header and not of the elements,
 * because that is what a slice value is: three words that point at elements
 * somewhere else. */
#define BURROW_SLICE_TYPE_DECL(Name, T)                                                \
    typedef Slice Name;                                                                \
    extern const Type burrow_type_##Name

/* The descriptor alone, for a slice declared earlier in the same file. That is
 * how a struct gets a field that is a slice of itself: declare the slice, then
 * the struct, then define the slice, which needs the struct to be complete. */
#define BURROW_SLICE_TYPE_DEFINE(Name, T)                                              \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_SLICE,                                                                    \
        (uint32_t)sizeof(Slice),                                                       \
        (uint16_t)_Alignof(Slice),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define BURROW_SLICE_TYPE(Name, T)                                                     \
    BURROW_SLICE_TYPE_DECL(Name, T);                                                   \
    BURROW_SLICE_TYPE_DEFINE(Name, T)

/* The same with a name, the one Go's type Name []T would have. The slices
 * above are unnamed like Go's []T, and most code cannot tell the difference,
 * but a package that looks at the name can, such as encoding/asn1 with a slice
 * whose name ends in SET. */
#define BURROW_NAMED_SLICE_TYPE_DECL(Name, T)                                          \
    typedef Slice Name;                                                                \
    extern const Type burrow_type_##Name

#define BURROW_NAMED_SLICE_TYPE(Name, T)                                               \
    BURROW_NAMED_SLICE_TYPE_DECL(Name, T);                                             \
    const Type burrow_type_##Name = {                                                  \
        BURROW_S_INIT(#Name),                                                          \
        {NULL, 0},                                                                     \
        KIND_SLICE,                                                                    \
        (uint32_t)sizeof(Slice),                                                       \
        (uint16_t)_Alignof(Slice),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* An array of N of them, which unlike a slice is its elements and is therefore
 * N times the size. The typedef is a struct with the array inside it rather
 * than a bare array, so that the name can be used as a field type, passed by
 * value and returned, none of which a C array can do. */
#define BURROW_ARRAY_TYPE_DECL(Name, T, N)                                             \
    typedef struct Name {                                                              \
        T v[N];                                                                        \
    } Name;                                                                            \
    extern const Type burrow_type_##Name

/* The descriptor alone, for an array whose DECL is in a header. */
#define BURROW_ARRAY_TYPE_DEFINE(Name, T, N)                                           \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_ARRAY,                                                                    \
        (uint32_t)sizeof(Name),                                                        \
        (uint16_t)_Alignof(Name),                                                      \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        (uint32_t)(N),                                                                 \
        0,                                                                             \
        NULL,                                                                          \
    }

#define BURROW_ARRAY_TYPE(Name, T, N)                                                  \
    BURROW_ARRAY_TYPE_DECL(Name, T, N);                                                \
    BURROW_ARRAY_TYPE_DEFINE(Name, T, N)

/* A pointer to T. This is the one a struct needs most often, because a struct
 * that refers to itself or to another struct does it through one, and a star
 * cannot be written inline anywhere TYPE_OF is used. */
#define BURROW_PTR_TYPE_DECL(Name, T)                                                  \
    typedef T *Name;                                                                   \
    extern const Type burrow_type_##Name

#define BURROW_PTR_TYPE(Name, T)                                                       \
    BURROW_PTR_TYPE_DECL(Name, T);                                                     \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(void *),                                                      \
        (uint16_t)_Alignof(void *),                                                    \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* A map from K to V. The descriptor carries both, since a map needs the key's
 * hash and equality as much as it needs the value's size, and the key goes in
 * the field called key while the value goes in elem, matching Go's reflect. */
#define BURROW_MAP_TYPE_DECL(Name, K, V)                                               \
    typedef Map *Name;                                                                 \
    extern const Type burrow_type_##Name

#define BURROW_MAP_TYPE(Name, K, V)                                                    \
    BURROW_MAP_TYPE_DECL(Name, K, V);                                                  \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_MAP,                                                                      \
        (uint32_t)sizeof(Map *),                                                       \
        (uint16_t)_Alignof(Map *),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(V),                                                                    \
        TYPE_OF(K),                                                                    \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* ------------------------------------------------------------------ methods
 *
 * A method is two things at once. It is data, so that something can ask a type
 * what it can do, and it is a call, made with a signature that the caller does
 * not know until it runs. net/rpc is handed a method name off a socket.
 * text/template is handed one out of a template. Neither of them can write the
 * call, so the call has to already be there.
 *
 * What is already there is a thunk: a function of one fixed shape, emitted next
 * to the method, that takes the receiver, an array of pointers to the arguments
 * and an array of pointers to where the results go, and makes the real call.
 * You write none of them. The signature list does:
 *
 *     static Str point_string(Point *p);
 *     static void point_move(Point *p, Int dx, Int dy);
 *
 *     #define POINT_SIG_String(IN, OUT)  OUT(Str)
 *     #define POINT_SIG_Move(IN, OUT)    IN(0, Int) IN(1, Int)
 *
 *     #define POINT_METHODS(M, T)                       \
 *         M(T, Move, point_move, POINT_SIG_Move)        \
 *         M(T, String, point_string, POINT_SIG_String)
 *
 *     BURROW_STRUCT_DECL(Point, POINT_FIELDS);
 *     ... the two functions above, written by hand ...
 *     BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);
 *
 * Then a caller with a name and no knowledge of any of this can do the call:
 *
 *     const Method *m = type_method_by_name(TYPE_OF(Point), BURROW_S("String"));
 *     Str out;
 *     void *rets[] = {&out};
 *     method_call(m, &p, NULL, rets);
 *
 * THE PARAMETER INDEX, WHICH YOU HAVE TO WRITE
 *
 * IN takes the position as well as the type, and the position is the one thing
 * here that is written twice. The preprocessor cannot count: there is no way to
 * turn a list into 0, 1, 2 without either a fixed table of arities or a
 * counter, and a counter would have to be incremented inside an expression
 * whose evaluation order is not defined. Writing it is the honest version, and
 * it is one number per parameter in a list you are already writing. A test can
 * hold you to it, because the positions end up in the descriptor: burrow's own
 * does exactly that.
 *
 * ONE RESULT, AND AN ERROR AFTER IT
 *
 * OUT appears once or not at all, with one exception. A C function returns one
 * value, and a burrow function that returns several does what every other one in
 * the library does and returns a struct holding them, so the count of results a
 * caller sees here is usually the count C has rather than the count Go would
 * have.
 *
 * The exception is Go's (T, error), which is common enough in the methods and
 * functions that text/template calls that it gets its own spelling. Write the
 * error as a second OUT naming ErrorResult, and the C function takes it the way
 * the rest of the library does, as a trailing Error *:
 *
 *     static bool point_inside(Point *p, Int r, Error *err);
 *     #define POINT_SIG_Inside(IN, OUT) IN(0, Int) OUT(bool) OUT(ErrorResult)
 *
 * The descriptor then lists two results, the second of type error, and a caller
 * passes two result pointers. ErrorResult only ever follows another OUT. A
 * function whose one result is an error says OUT(Error) and returns it.
 *
 * THE RECEIVER IS A POINTER
 *
 * The thunk passes the receiver as T *, so the method takes T *. Go has value
 * receivers too and this does not, because a dynamic call arrives holding a
 * pointer to the value either way, and a method that wants a copy can take one
 * on its first line. What that costs is the Go distinction between the method
 * set of T and the method set of *T, which is a distinction about what
 * satisfies an interface, and in C an interface is satisfied by a vtable you
 * filled in by hand. Nothing in the library can tell the difference. */

/* The signature, read in the five places it has to be read. Two spellings of
 * nothing, because a macro passed as IN is called with two arguments and one
 * passed as OUT is called with one. */
#define BURROW__SIG_SKIP1(ctype)
#define BURROW__SIG_SKIP2(i, ctype)

#define BURROW__SIG_IN_FIELD(i, ctype)                                                 \
    {BURROW_S_INIT(""), BURROW_S_INIT(""), TYPE_OF(ctype), (uint32_t)(i)},
#define BURROW__SIG_OUT_FIELD(ctype)                                                   \
    {BURROW_S_INIT(""), BURROW_S_INIT(""), TYPE_OF(ctype), 0},

/* One "+ 1" per parameter, summed into an enum constant, which is how a list
 * gets counted without the preprocessor being able to count. An enum constant
 * rather than an array of bytes because it occupies nothing: the count is
 * wanted in one static initialiser and nowhere at runtime. */
#define BURROW__SIG_IN_PLUS(i, ctype) +1

/* ErrorResult is the error type under another name, so that TYPE_OF finds the
 * error descriptor, and a name the macros below can tell apart from every other
 * result type. The test is the usual preprocessor probe: pasting the type onto
 * BURROW__SIG_ERR_ gives a macro with a comma in it only for ErrorResult, which
 * moves the 1 into the place where every other type finds the 0. */
#define burrow_type_ErrorResult burrow_type_Error

#define BURROW__SIG_CAT(a, b) BURROW__SIG_CAT_(a, b)
#define BURROW__SIG_CAT_(a, b) a##b
#define BURROW__SIG_PROBE_N(x, n, ...) n
#define BURROW__SIG_PROBE(...) BURROW__SIG_PROBE_N(__VA_ARGS__, 0, )
#define BURROW__SIG_ERR_ErrorResult ~, 1,
#define BURROW__SIG_IS_ERR(ctype) BURROW__SIG_PROBE(BURROW__SIG_ERR_##ctype)

/* The arguments go in as values and the first result comes back as the return
 * value. An ErrorResult is not returned: the function is handed the second
 * result slot as its last argument instead. */
#define BURROW__SIG_PASS(i, ctype) , *(ctype *)args[i]
#define BURROW__SIG_STORE(ctype)                                                       \
    BURROW__SIG_CAT(BURROW__SIG_STORE_, BURROW__SIG_IS_ERR(ctype))(ctype)
#define BURROW__SIG_STORE_0(ctype) *(ctype *)rets[0] =
#define BURROW__SIG_STORE_1(ctype)
#define BURROW__SIG_PASS_OUT(ctype)                                                    \
    BURROW__SIG_CAT(BURROW__SIG_PASS_OUT_, BURROW__SIG_IS_ERR(ctype))
#define BURROW__SIG_PASS_OUT_0
#define BURROW__SIG_PASS_OUT_1 , (Error *)rets[1]

/* Everything one method needs, emitted next to it.
 *
 * The terminator on the signature array is not a sentinel anybody reads. It is
 * there because a method with no parameters and no result is an ordinary thing
 * to want, Close being the obvious one, and an empty initialiser list is not
 * legal C. The field count below subtracts it. */
#define BURROW__METHOD_FTYPE(T, mname, SIG)                                            \
    static const Field burrow__sig_##T##_##mname[] = {                                 \
        SIG(BURROW__SIG_IN_FIELD, BURROW__SIG_OUT_FIELD){BURROW_S_INIT(""),            \
                                                         BURROW_S_INIT(""), NULL, 0},  \
    };                                                                                 \
    enum {                                                                             \
        burrow__nin_##T##_##mname = 0 SIG(BURROW__SIG_IN_PLUS, BURROW__SIG_SKIP1)      \
    };                                                                                 \
    static const Type burrow__ftype_##T##_##mname = {                                  \
        BURROW_S_INIT(""),                                                             \
        {NULL, 0},                                                                     \
        KIND_FUNC,                                                                     \
        (uint32_t)sizeof(Func),                                                        \
        (uint16_t)_Alignof(Func),                                                      \
        (uint16_t)(sizeof burrow__sig_##T##_##mname /                                  \
                       sizeof burrow__sig_##T##_##mname[0] -                           \
                   1),                                                                 \
        0,                                                                             \
        burrow__sig_##T##_##mname,                                                     \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        (uint32_t)burrow__nin_##T##_##mname,                                           \
        0,                                                                             \
        NULL,                                                                          \
    };

#define BURROW__METHOD_DEFS(T, mname, fn, SIG)                                         \
    BURROW__METHOD_FTYPE(T, mname, SIG)                                                \
    static void burrow__thunk_##T##_##mname(void *recv, void **args, void **rets) {    \
        (void)args;                                                                    \
        (void)rets;                                                                    \
        SIG(BURROW__SIG_SKIP2, BURROW__SIG_STORE)                                      \
        fn((T *)recv SIG(BURROW__SIG_PASS, BURROW__SIG_PASS_OUT));                     \
    }

#define BURROW__METHOD_ENTRY(T, mname, fn, SIG)                                        \
    {BURROW_S_INIT(#mname), &burrow__ftype_##T##_##mname, burrow__thunk_##T##_##mname},

/* The thunks, the signatures and the array tying them to names.
 *
 * Separate from the descriptor because a type can have methods without this
 * file knowing how its descriptor gets built, and because a test wants to be
 * able to look at the array on its own. */
#define BURROW_METHODS_DEFINE(T, METHODS)                                              \
    METHODS(BURROW__METHOD_DEFS, T)                                                    \
    static const Method burrow__methods_##T[] = {METHODS(BURROW__METHOD_ENTRY, T)}

/* A struct, its fields and its methods.
 *
 * The methods have to be named after the struct is declared, since a thunk
 * casts the receiver to it, so this is the define half of the pair and
 * BURROW_STRUCT_DECL is still the declare half. Between the two go the method
 * functions themselves, which are the only part of this you write.
 *
 * List the methods in name order. Nothing makes you, because the preprocessor
 * cannot sort any more than it can count, but Go enumerates a type's methods in
 * name order and anything walking this array inherits whatever order you used.
 * type_methods_sorted is there for a test to say so out loud. */
#define BURROW_STRUCT_DEFINE_METHODS(T, FIELDS, METHODS)                               \
    BURROW_METHODS_DEFINE(T, METHODS);                                                 \
    static const Field burrow__fields_##T[] = {FIELDS(BURROW__FIELD, T)};              \
    const Type burrow_type_##T = {                                                     \
        BURROW_S_INIT(#T),                                                             \
        {NULL, 0},                                                                     \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        (uint16_t)(sizeof burrow__fields_##T / sizeof burrow__fields_##T[0]),          \
        (uint16_t)(sizeof burrow__methods_##T / sizeof burrow__methods_##T[0]),        \
        burrow__fields_##T,                                                            \
        burrow__methods_##T,                                                           \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* -------------------------------------------------------------- interfaces
 *
 * A descriptor for an interface with methods that lists them, which is what
 * lets reflect tell whether a type implements it and call a method on a value
 * held in one:
 *
 *     typedef struct ShapeVT {
 *         const Type *self_type;
 *         double (*area)(void *self);
 *     } ShapeVT;
 *     typedef struct Shape {
 *         const ShapeVT *vt;
 *         void *data;
 *     } Shape;
 *
 *     #define SHAPE_SIG_Area(IN, OUT) OUT(double)
 *     #define SHAPE_METHODS(M, T) M(T, Area, area, SHAPE_SIG_Area)
 *     BURROW_IFACE_TYPE_DEFINE(Shape, SHAPE_METHODS);
 *
 * The list is the one a struct's methods take, with the vtable member where the
 * function went. A member of an embedded vtable is written as a path, so
 * io.ReadWriter's Read would be reader.read. Each method gets a thunk that calls
 * through the vtable of the interface value it is handed, so calling one through
 * reflect reaches the method of whatever the value holds.
 *
 * Name order again, which is the order Go's reflect gives an interface's methods
 * in too, whatever order the source declared them in. */
#define BURROW__IFACE_METHOD_DEFS(I, mname, member, SIG)                               \
    BURROW__METHOD_FTYPE(I, mname, SIG)                                                \
    static void burrow__thunk_##I##_##mname(void *recv, void **args, void **rets) {    \
        const I *burrow__i = (const I *)recv;                                          \
        (void)args;                                                                    \
        (void)rets;                                                                    \
        SIG(BURROW__SIG_SKIP2, BURROW__SIG_STORE)                                      \
        burrow__i->vt->member(                                                         \
            burrow__i->data SIG(BURROW__SIG_PASS, BURROW__SIG_PASS_OUT));              \
    }

#define BURROW_IFACE_TYPE_DECL(I) extern const Type burrow_type_##I

#define BURROW_IFACE_TYPE_DEFINE(I, METHODS)                                           \
    METHODS(BURROW__IFACE_METHOD_DEFS, I)                                              \
    static const Method burrow__methods_##I[] = {METHODS(BURROW__METHOD_ENTRY, I)};    \
    const Type burrow_type_##I = {                                                     \
        BURROW_S_INIT(#I),                                                             \
        {NULL, 0},                                                                     \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(I),                                                           \
        (uint16_t)_Alignof(I),                                                         \
        0,                                                                             \
        (uint16_t)(sizeof burrow__methods_##I / sizeof burrow__methods_##I[0]),        \
        NULL,                                                                          \
        burrow__methods_##I,                                                           \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* ----------------------------------------------------------- function types
 *
 * A descriptor for a function type made with BURROW_FUNC from burrow/func.h,
 * which is what lets reflect describe a value of it and call one:
 *
 *     BURROW_FUNC(IntOp, Int, Int a, Int b);
 *     #define INT_OP_SIG(IN, OUT) IN(0, Int) IN(1, Int) OUT(Int)
 *     BURROW_FUNC_TYPE_DEFINE(IntOp, INT_OP_SIG);
 *
 * The signature is spelled the way a method's is above, and it has to say what
 * BURROW_FUNC was told, which nothing can check for you. The call thunk the
 * descriptor carries passes the env and the arguments to the function and
 * stores what it returns, the same as a method thunk does with a receiver.
 *
 * The type is unnamed, Go's func(int, int) int, like the slices and maps above.
 *
 * The VARIADIC form is Go's func(...E). Its last parameter is a slice of E,
 * which in C is a Slice, and reflect's Call gathers the arguments after the
 * fixed ones into it. */
#define BURROW_FUNC_TYPE_DECL(Name) extern const Type burrow_type_##Name

#define BURROW__FUNC_TYPE_DEFINE(Name, SIG, E)                                         \
    static const Field burrow__fsig_##Name[] = {                                       \
        SIG(BURROW__SIG_IN_FIELD, BURROW__SIG_OUT_FIELD){BURROW_S_INIT(""),            \
                                                         BURROW_S_INIT(""), NULL, 0},  \
    };                                                                                 \
    enum { burrow__fnin_##Name = 0 SIG(BURROW__SIG_IN_PLUS, BURROW__SIG_SKIP1) };      \
    static void burrow__fcall_##Name(const void *fn, void **args, void **rets) {       \
        const Name *burrow__fn = (const Name *)fn;                                     \
        (void)args;                                                                    \
        (void)rets;                                                                    \
        SIG(BURROW__SIG_SKIP2, BURROW__SIG_STORE)                                      \
        burrow__fn->f(burrow__fn->env SIG(BURROW__SIG_PASS, BURROW__SIG_PASS_OUT));    \
    }                                                                                  \
    static const TypeOps burrow__fops_##Name = {NULL, NULL, NULL, NULL,                \
                                                burrow__fcall_##Name};                 \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_FUNC,                                                                     \
        (uint32_t)sizeof(Name),                                                        \
        (uint16_t)_Alignof(Name),                                                      \
        (uint16_t)(sizeof burrow__fsig_##Name / sizeof burrow__fsig_##Name[0] - 1),    \
        0,                                                                             \
        burrow__fsig_##Name,                                                           \
        NULL,                                                                          \
        E,                                                                             \
        NULL,                                                                          \
        (uint32_t)burrow__fnin_##Name,                                                 \
        0,                                                                             \
        &burrow__fops_##Name,                                                          \
    }

#define BURROW_FUNC_TYPE_DEFINE(Name, SIG) BURROW__FUNC_TYPE_DEFINE(Name, SIG, NULL)

#define BURROW_FUNC_TYPE_DEFINE_VARIADIC(Name, E, SIG)                                 \
    BURROW__FUNC_TYPE_DEFINE(Name, SIG, TYPE_OF(E))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DECLARE_H */
