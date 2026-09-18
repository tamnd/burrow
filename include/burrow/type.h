/* Type descriptors, which are how a C library gets to have reflection.
 *
 * Go's standard library leans on the type system far more than it looks. fmt
 * prints any value because it can ask what the value is. encoding/json walks a
 * struct it has never seen. sort works on a slice of anything. database/sql
 * scans into whatever you pass it. None of that is possible in C unless the
 * types describe themselves, so in burrow they do.
 *
 * A Type is a static const struct in read only memory, one per type, shared by
 * everything that mentions that type. It costs one word to point at and nothing
 * to initialise. A program using a dozen types pays for a dozen descriptors,
 * and a program using none pays for the handful of builtins the linker keeps.
 *
 *     const Type *t = TYPE_INT;
 *     printf("%u bytes, aligned to %u\n", t->size, t->align);
 *
 * You will not write one of these by hand for your own structs. That is what
 * the BURROW_STRUCT macro is for, and it is not here yet. What is here is the
 * shape of a descriptor and the descriptors for the types the language already
 * has, because Slice cannot exist without something to describe its elements.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TYPE_H
#define BURROW_TYPE_H

#include "burrow/core.h"
#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The same twenty six kinds Go has, in the same order, with the same zero
 * value meaning invalid.
 *
 * The order matters more than it looks. reflect.Kind's numeric values are
 * printed by fmt, compared in test code, and switched on in code ported from
 * Go, so a descriptor that numbered them differently would produce a library
 * that behaves like Go until somebody prints one. */
typedef enum Kind {
    KIND_INVALID = 0,
    KIND_BOOL,
    KIND_INT,
    KIND_INT8,
    KIND_INT16,
    KIND_INT32,
    KIND_INT64,
    KIND_UINT,
    KIND_UINT8,
    KIND_UINT16,
    KIND_UINT32,
    KIND_UINT64,
    KIND_UINTPTR,
    KIND_FLOAT32,
    KIND_FLOAT64,
    KIND_COMPLEX64,
    KIND_COMPLEX128,
    KIND_ARRAY,
    KIND_CHAN,
    KIND_FUNC,
    KIND_INTERFACE,
    KIND_MAP,
    KIND_POINTER,
    KIND_SLICE,
    KIND_STRING,
    KIND_STRUCT,
    KIND_UNSAFE_POINTER,
    KIND_MAX /* not a kind, just the count, for bounds checking a table */
} Kind;

/* The descriptor is named before it is defined, because Field and Method both
 * point at one and both are part of it. */
typedef struct Type Type;

/* What a type can do, for the operations that cannot be worked out from size
 * and alignment alone.
 *
 * Every one of these is optional and NULL means the obvious default, which for
 * equal is memcmp, for hash is a hash of the bytes, for copy is memcpy and for
 * zero is memset. Those defaults are correct for every type that is just its
 * bytes, which is most of them. They are wrong for anything holding a pointer
 * to something that has its own idea of equality, Str being the first example:
 * two Str values with different pointers and the same bytes are equal in Go and
 * memcmp says they are not. */
typedef struct TypeOps {
    bool (*equal)(const void *a, const void *b);
    uint64_t (*hash)(const void *p, uint64_t seed);
    void (*copy)(void *dst, const void *src);
    void (*zero)(void *p);
} TypeOps;

/* One field of a struct.
 *
 * tag is the struct tag exactly as written, unparsed. encoding/json and the
 * rest each parse it their own way and Go does not centralise that either. */
typedef struct Field {
    Str name;
    Str tag;
    const Type *type;
    uint32_t offset;
    bool exported;
    bool anonymous; /* embedded, which Go's reflect calls Anonymous */
} Field;

/* One method, and the thunk that calls it.
 *
 * The thunk exists because a dynamic call needs one shape of function pointer
 * no matter what the method's real signature is. It takes the receiver, an
 * array of pointers to the arguments and an array of pointers to where the
 * results go, which is enough for net/rpc and for text/template and is what
 * reflect.Value.Call needs underneath. */
typedef struct Method {
    Str name;
    const Type *ftype;
    void (*thunk)(void *recv, void **args, void **rets);
} Method;

struct Type {
    /* "Point". Empty for an unnamed type such as []int. */
    Str name;

    /* "image". Empty for a builtin and for an unnamed type. This is Go's
     * PkgPath and it is what makes two identically named types from different
     * packages different types. */
    Str pkg_path;

    Kind kind;

    /* Bytes, and the alignment in bytes. uint32 and uint16 rather than size_t
     * because a descriptor is a static object that gets duplicated per type and
     * no Go type is four gigabytes. */
    uint32_t size;
    uint16_t align;

    uint16_t nfield;
    uint16_t nmethod;

    const Field *fields;   /* nfield of them, KIND_STRUCT only */
    const Method *methods; /* nmethod of them, sorted by name as Go sorts them */

    /* Slice, array, pointer, channel, and the value type of a map. */
    const Type *elem;

    /* The key type of a map, NULL otherwise. */
    const Type *key;

    /* Array length. Meaningless for everything else. */
    uint32_t len;

    /* A number that differs between different types and is stable within one
     * build. Used to make a type assertion a comparison rather than a string
     * compare, and to seed a map's hash.
     *
     * It is not stable across builds and nothing may persist it. Go's is not
     * either, for the same reason: it is derived from the layout of a
     * particular binary. */
    uint32_t hash;

    const TypeOps *ops;
};

/* The builtin descriptors.
 *
 * These are the types Go's language has rather than types its library declares,
 * so they exist whether or not anybody asked for them. They are declared as
 * pointers to const, so the table itself stays in read only memory and a
 * program that never mentions one does not carry it. */
extern const Type *const TYPE_BOOL;
extern const Type *const TYPE_INT;
extern const Type *const TYPE_INT8;
extern const Type *const TYPE_INT16;
extern const Type *const TYPE_INT32;
extern const Type *const TYPE_INT64;
extern const Type *const TYPE_UINT;
extern const Type *const TYPE_UINT8;
extern const Type *const TYPE_UINT16;
extern const Type *const TYPE_UINT32;
extern const Type *const TYPE_UINT64;
extern const Type *const TYPE_UINTPTR;
extern const Type *const TYPE_FLOAT32;
extern const Type *const TYPE_FLOAT64;
extern const Type *const TYPE_COMPLEX64;
extern const Type *const TYPE_COMPLEX128;
extern const Type *const TYPE_STRING;
extern const Type *const TYPE_UNSAFE_POINTER;

/* Go spells these two as aliases of uint8 and int32, and reflect reports them
 * that way: reflect.TypeOf(byte(0)).Kind() is reflect.Uint8 and its String() is
 * "uint8". So these are the same descriptors rather than new ones, which is the
 * faithful thing even though it means a byte prints as a uint8. */
#define TYPE_BYTE TYPE_UINT8
#define TYPE_RUNE TYPE_INT32

/* "int", "[]uint8", "image.Point". Borrows from the descriptor, which is static,
 * so the result outlives everything.
 *
 * Unnamed composite types do not carry their spelling in the descriptor, since
 * storing it would mean a string per instantiation. They get their kind's name
 * instead, so a []int reports "slice" rather than "[]int" until the composite
 * type constructors land and can build the real name. */
BURROW_BORROWS(ret, t) Str type_name(const Type *t);

/* "bool", "int", "slice". This is Kind.String() and it is what fmt's %v prints
 * for a Kind. An out of range kind gives "invalid" rather than reading off the
 * end of the table. */
BURROW_STATIC(ret) Str kind_name(Kind k);

/* The three questions that get asked about a kind constantly, spelled out here
 * so that nobody writes the range check by hand and gets it subtly wrong.
 *
 * Signed and unsigned exclude the float kinds, matching what Go's reflect does
 * when it groups Int through Int64 and Uint through Uintptr. */
bool kind_is_signed(Kind k);
bool kind_is_unsigned(Kind k);
bool kind_is_float(Kind k);

/* True when a value of this type can be compared with == in Go. Slices, maps
 * and functions cannot, and a struct or array cannot when any part of it
 * cannot. Go rejects that at compile time; burrow has to answer it at runtime,
 * because a map with an uncomparable key type is a program that has to fail
 * somewhere and this is where it finds out. */
bool type_is_comparable(const Type *t);

/* equal, hash, copy and zero, going through ops when the type has them and
 * falling back to the byte level default when it does not.
 *
 * Use these rather than memcmp and memcpy on a value whose type you only know
 * at runtime. That is the whole point of them: a Str compares by bytes and not
 * by pointer, and only the ops table knows that. */
bool type_equal(const Type *t, const void *a, const void *b);
uint64_t type_hash(const Type *t, const void *p, uint64_t seed);
void type_copy(const Type *t, void *dst, const void *src);
void type_zero(const Type *t, void *p);

/* The field with this name, or NULL. Linear, because structs have a handful of
 * fields and a linear scan over a contiguous array beats anything cleverer at
 * that size. */
BURROW_BORROWS(ret, t) const Field *type_field_by_name(const Type *t, Str name);

/* The method with this name, or NULL. Methods are sorted by name in a
 * descriptor, the way Go sorts them, so this is a binary search. */
BURROW_BORROWS(ret, t) const Method *type_method_by_name(const Type *t, Str name);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TYPE_H */
