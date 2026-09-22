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

#define BURROW_SLICE_TYPE(Name, T)                                                     \
    BURROW_SLICE_TYPE_DECL(Name, T);                                                   \
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

/* An array of N of them, which unlike a slice is its elements and is therefore
 * N times the size. The typedef is a struct with the array inside it rather
 * than a bare array, so that the name can be used as a field type, passed by
 * value and returned, none of which a C array can do. */
#define BURROW_ARRAY_TYPE_DECL(Name, T, N)                                             \
    typedef struct Name {                                                              \
        T v[N];                                                                        \
    } Name;                                                                            \
    extern const Type burrow_type_##Name

#define BURROW_ARRAY_TYPE(Name, T, N)                                                  \
    BURROW_ARRAY_TYPE_DECL(Name, T, N);                                                \
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

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DECLARE_H */
