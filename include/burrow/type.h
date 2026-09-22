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
 * BURROW_STRUCT in burrow/declare.h is for. What is here is the shape of a
 * descriptor and the descriptors for the types the language already has,
 * because Slice cannot exist without something to describe its elements.
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
 * rest each parse it their own way and Go does not centralise that either.
 *
 * Whether a field is exported and whether it is embedded are not stored. Both
 * follow from the name, both are questions rather than facts in Go's reflect
 * too, and a stored copy of an answer that can be worked out is a stored copy
 * that can be wrong. See field_is_exported and field_is_embedded below. */
typedef struct Field {
    Str name;
    Str tag;
    const Type *type;
    uint32_t offset;
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

    /* A number that differs between types and is stable within one build, or
     * zero for a type that has not been given one.
     *
     * The builtins have one because they are numbered by hand in one file. A
     * type declared with BURROW_STRUCT does not, because there is no way in C
     * to hash a name at compile time, and a number that is not actually unique
     * would be worse here than no number: everything that reads this reads it
     * to avoid a slower comparison, and a collision would make the slower
     * comparison wrong rather than slow.
     *
     * So identity within one program is the descriptor's address, which is
     * unique by construction because the definition is emitted once. The
     * number is for the case the address cannot answer, which is the same type
     * reaching a program twice through two shared libraries, and filling it in
     * for declared types is part of the type registry rather than part of the
     * declaration.
     *
     * Not stable across builds and nothing may persist it. Go's is not either,
     * for the same reason: it comes out of the layout of one binary. */
    uint32_t hash;

    const TypeOps *ops;
};

/* The descriptor for a type, by the type's C spelling.
 *
 *     const Type *t = TYPE_OF(Int);
 *     const Type *p = TYPE_OF(Point);
 *
 * Point there is a type somebody declared with BURROW_STRUCT, which emits the
 * descriptor under that name for exactly this reason.
 *
 * One rule underneath it: the descriptor for a type spelled T is an object
 * named burrow_type_T. Nothing looks anything up and nothing is registered, so
 * this is an address constant and can therefore appear in a static initialiser,
 * which is what makes a struct descriptor able to name the types of its fields.
 *
 * The argument is a token and not a type, so it has to be a single identifier.
 * A pointer or an array field needs a typedef, and this is not a hardship: C
 * wants the typedef anyway the moment the type appears twice.
 *
 * Asking for a type that has no descriptor is a link error naming
 * burrow_type_Whatever, which is not the friendliest message in the world but
 * does say exactly what is missing. */
#define TYPE_OF(T) (&burrow_type_##T)

/* The builtins.
 *
 * These are the types Go's language has rather than types its library declares,
 * so they exist whether or not anybody asked for them. Named by C spelling,
 * which is why there is a burrow_type_Int and a separate burrow_type_int64_t:
 * they are the same C type and two different Go types, and a struct with an int
 * field marshals differently from one with an int64 field. */
extern const Type burrow_type_bool;
extern const Type burrow_type_Int;
extern const Type burrow_type_int8_t;
extern const Type burrow_type_int16_t;
extern const Type burrow_type_int32_t;
extern const Type burrow_type_int64_t;
extern const Type burrow_type_Uint;
extern const Type burrow_type_uint8_t;
extern const Type burrow_type_uint16_t;
extern const Type burrow_type_uint32_t;
extern const Type burrow_type_uint64_t;
extern const Type burrow_type_Uintptr;
extern const Type burrow_type_float;
extern const Type burrow_type_double;
extern const Type burrow_type_Complex64;
extern const Type burrow_type_Complex128;
extern const Type burrow_type_Str;

/* Go's unsafe.Pointer is void * in C, which is not an identifier and so cannot
 * be pasted. This is the name to hand TYPE_OF instead, and it is also a real
 * typedef so that a field holding one has something to be declared as. */
typedef void *UnsafePointer;
extern const Type burrow_type_UnsafePointer;

/* Go spells byte and rune as aliases of uint8 and int32, and reflect reports
 * them that way: reflect.TypeOf(byte(0)).Kind() is reflect.Uint8 and its
 * String() is "uint8". So these are the same descriptors rather than new ones,
 * which is the faithful thing even though it means a byte prints as a uint8.
 *
 * Aliases at the descriptor name rather than at TYPE_OF, so that TYPE_OF(Byte)
 * and TYPE_OF(uint8_t) come out as the same address and not merely as two
 * descriptors that agree. */
#define burrow_type_Byte burrow_type_uint8_t
#define burrow_type_Rune burrow_type_int32_t

/* The Go spelling of each, which is what most code reaches for and what the
 * rest of burrow is written in. TYPE_OF is for a field list, where the C
 * spelling is what is already written down. */
#define TYPE_BOOL TYPE_OF(bool)
#define TYPE_INT TYPE_OF(Int)
#define TYPE_INT8 TYPE_OF(int8_t)
#define TYPE_INT16 TYPE_OF(int16_t)
#define TYPE_INT32 TYPE_OF(int32_t)
#define TYPE_INT64 TYPE_OF(int64_t)
#define TYPE_UINT TYPE_OF(Uint)
#define TYPE_UINT8 TYPE_OF(uint8_t)
#define TYPE_UINT16 TYPE_OF(uint16_t)
#define TYPE_UINT32 TYPE_OF(uint32_t)
#define TYPE_UINT64 TYPE_OF(uint64_t)
#define TYPE_UINTPTR TYPE_OF(Uintptr)
#define TYPE_FLOAT32 TYPE_OF(float)
#define TYPE_FLOAT64 TYPE_OF(double)
#define TYPE_COMPLEX64 TYPE_OF(Complex64)
#define TYPE_COMPLEX128 TYPE_OF(Complex128)
#define TYPE_STRING TYPE_OF(Str)
#define TYPE_UNSAFE_POINTER TYPE_OF(UnsafePointer)
#define TYPE_BYTE TYPE_OF(Byte)
#define TYPE_RUNE TYPE_OF(Rune)

/* "int", "[]uint8", "image.Point". Borrows from the descriptor, which is static,
 * so the result outlives everything.
 *
 * A composite type does not carry its spelling, so a []int reports "slice"
 * rather than "[]int". That is not a gap waiting to be filled by the composite
 * constructors in declare.h. Those know the element type as a C token and the
 * spelling wanted is the Go one, which lives in the element's descriptor and is
 * not something the preprocessor can reach. Building "[]int" is a walk from the
 * slice descriptor to its element and back, at the point somebody asks, and
 * that is where it belongs: it is exactly what Go's reflect.Type.String does
 * for an unnamed type, and it arrives with fmt's %T. */
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

/* Go's rule, which is that a name beginning with an upper case letter is
 * exported and anything else is not. It is worth porting rather than dropping,
 * because it is what decides whether encoding/json writes a field, and a
 * struct ported from Go has to marshal to the same bytes.
 *
 * This is reflect.StructField.IsExported, which is a method there too. */
bool field_is_exported(const Field *f);

/* True for an embedded field, which Go's reflect calls Anonymous.
 *
 * Go's rule is that an embedded field has no name of its own and takes the
 * name of its type, so that is what is checked: the field's name and its
 * type's name are the same string. A field called Point of type Point is
 * embedded and there is no way to write one that is not, which is exactly the
 * situation in Go. */
bool field_is_embedded(const Field *f);

/* The method with this name, or NULL.
 *
 * Linear, over an array that a type has a handful of entries in. It used to be
 * a binary search, on the strength of the array being sorted by name, and the
 * array is only sorted by name if whoever wrote the list happened to write it
 * in order. The preprocessor cannot sort, so nothing enforces that, and a
 * binary search over an array that is nearly sorted finds most of what it looks
 * for and silently misses the rest, which is the worst way for this to be
 * wrong. Comparing a handful of short strings in order is cheaper than the
 * branch mispredictions a binary search costs at these sizes anyway.
 *
 * The order still matters for walking the array, because that is the order a
 * caller enumerating a type's methods sees, and Go's is by name. See
 * type_methods_sorted. */
BURROW_BORROWS(ret, t) const Method *type_method_by_name(const Type *t, Str name);

/* Whether a type's methods are in the order Go would enumerate them in, which
 * is by name. For a test to assert about a type it declares, since the macro
 * that built the array could not check it. */
bool type_methods_sorted(const Type *t);

/* Make the call.
 *
 * args points at nin pointers, each to an argument, and rets points at one
 * pointer to somewhere the result fits. Either may be NULL when the count is
 * zero. False means there was nothing to call, which is a NULL method or one
 * with no thunk, and is the answer to asking a type for a method it does not
 * have rather than a reason to stop.
 *
 * Nothing here checks the types, because the caller has the signature and is in
 * a position to. reflect's Value.Call is where that check belongs and it is the
 * layer above this one. */
bool method_call(const Method *m, void *recv, void **args, void **rets);

/* A function type's parameters and results.
 *
 * KIND_FUNC uses fields for both, parameters first and then results, with
 * nfield the total of the two and len the number of parameters. That is one
 * array rather than two because a descriptor has one pointer for a list of
 * fields and adding a second would grow every descriptor of every kind to pay
 * for a kind most programs never reflect on.
 *
 * There is at most one result. A C function returns one value, and a burrow
 * function returning several returns a struct of them, so this counts what C
 * counts. Anything else gives 0 and NULL rather than a wrong answer. */
Int type_num_in(const Type *t);
Int type_num_out(const Type *t);
BURROW_BORROWS(ret, t) const Type *type_in(const Type *t, Int i);
BURROW_BORROWS(ret, t) const Type *type_out(const Type *t, Int i);

/* --------------------------------------------------------------- the registry
 *
 * Reflection on a value you are holding needs no registry. You have the
 * descriptor, it is a static object, and that is the whole of it. The registry
 * answers the other question, which is what a program does when it has a type's
 * name and nothing else:
 *
 *     BURROW_REGISTER_TYPE(Point);   at file scope, once
 *
 *     const Type *t = type_by_name(BURROW_S("image.Point"));
 *
 * encoding/gob puts type names on the wire, net/rpc names the type of an
 * argument, and text/template resolves a method on a name it was handed. None
 * of those can work from an address, so somewhere there has to be a map from a
 * name to a descriptor, and a program has to say which of its types go in it.
 *
 * Registering is opt in and stays that way. A registry that held every type in
 * the binary would be a table nobody asked for, and would keep every descriptor
 * the linker would otherwise drop. */

/* Where a registered descriptor's pointer goes.
 *
 * One pointer per registered type, in a section of its own, so that the
 * registry can find all of them by reading the section between the two symbols
 * the linker puts either side of it. Nothing runs to put them there. The
 * section is part of the image the same way a string literal is.
 *
 * Three spellings, because the three object formats name sections differently
 * and Mach-O wants a segment as well. The PE spelling relies on the linker
 * sorting sections whose names differ after a dollar sign, which is how the C
 * runtime's own initialiser lists have worked since before C99.
 *
 * BURROW__TYPE_SECTION on its own is the attribute. Nothing outside burrow
 * should need it; BURROW_REGISTER_TYPE is the thing to use. */
/* Do not let the address sanitizer put a redzone around one of these.
 *
 * This is not a nicety. The registry reads the section as an array, one pointer
 * after another, and a sanitizer that puts twenty four bytes of poison between
 * two globals turns that array into something with holes in it. The walk then
 * reads the first hole and the sanitizer reports the overflow it created. There
 * is no way to walk around it, because the padding is only knowable from inside
 * the sanitizer, so the answer is to ask for none.
 *
 * clang only. gcc needs nothing here, because its sanitizer already leaves a
 * global with a section of its own alone, and it rejects the attribute on
 * anything that is not a function, so asking would be an error rather than a
 * no-op. __has_attribute says yes on gcc either way, which is why this asks
 * which compiler it is instead.
 *
 * What it gives up is the redzone on one pointer per registered type, and that
 * pointer is written by the linker and never touched again. It expands to
 * nothing in a build without the sanitizer. */
#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(no_sanitize_address)
#define BURROW__TYPE_NO_REDZONE __attribute__((no_sanitize_address))
#endif
#endif
#if !defined(BURROW__TYPE_NO_REDZONE)
#define BURROW__TYPE_NO_REDZONE
#endif

#if defined(__APPLE__) && defined(__GNUC__)
#define BURROW__TYPE_SECTION                                                           \
    BURROW__TYPE_NO_REDZONE __attribute__((used, section("__DATA,__burrowtype")))
#elif defined(_WIN32) && defined(__GNUC__)
#define BURROW__TYPE_SECTION                                                           \
    BURROW__TYPE_NO_REDZONE __attribute__((used, section(".brwt$b")))
#elif defined(__ELF__) && defined(__GNUC__)
#define BURROW__TYPE_SECTION                                                           \
    BURROW__TYPE_NO_REDZONE __attribute__((used, section("burrowtype")))
#elif defined(_MSC_VER)
#define BURROW__TYPE_SECTION __declspec(allocate(".brwt$b"))
#endif

#if defined(BURROW__TYPE_SECTION)
#define BURROW__TYPE_SECTION_ENTRY(T, sym)                                             \
    BURROW__TYPE_SECTION static const Type *const burrow__typeref_##T = &(sym)
#elif defined(__GNUC__)
/* No section, but constructors work, which is the case for a target with an
 * object format burrow has not been told about. One store each, before main. */
#define BURROW__TYPE_SECTION_ENTRY(T, sym)                                             \
    __attribute__((constructor)) static void burrow__register_##T(void) {              \
        (void)type_register(&(sym));                                                   \
    }                                                                                  \
    extern const Type *const burrow__typeref_##T
#else
/* Neither. Call type_register yourself at the top of main; there is nowhere
 * else for it to go, and failing here is better than a registry that is quietly
 * empty. */
#define BURROW__TYPE_SECTION_ENTRY(T, sym)                                             \
    _Static_assert(0, "BURROW_REGISTER_TYPE needs sections or constructors, "          \
                      "neither of which this compiler has. Call type_register "        \
                      "at the start of main instead.")
#endif

/* Put a descriptor in the registry, at file scope.
 *
 * This costs no code and runs nothing at startup on the common platforms. It
 * places one pointer in a section of its own, and the registry reads that
 * section the first time anybody looks a name up. Where that is not available
 * it falls back to a constructor, which is the only part of burrow that runs
 * anything before main, and it runs one store.
 *
 * The semicolon is yours, as it is for BURROW_STRUCT, and for the same reason.
 *
 * T is the type's C spelling, so BURROW_REGISTER_TYPE(Point) for a type named
 * Point. It works on any descriptor named the way TYPE_OF expects, whether it
 * came out of BURROW_STRUCT or was written by hand. */
#define BURROW_REGISTER_TYPE(T) BURROW__TYPE_SECTION_ENTRY(T, burrow_type_##T)

/* The descriptor registered under this qualified name, or NULL.
 *
 * The name is Go's: "image.Point" for a type in a package, "Point" for one with
 * no package path, "int" for a builtin. The split is at the last dot, because a
 * package path can contain dots and a type name cannot.
 *
 * NULL means not registered, and it means that rather than anything else. A
 * name that reached here from a network connection naming a type this program
 * does not have is the case this has to get right, and guessing at it is how a
 * decoder turns someone else's bytes into a call into your program. */
BURROW_STATIC(ret) const Type *type_by_name(Str name);

/* Add a descriptor to the registry while the program is running, for a type
 * that arrived in a module loaded after startup.
 *
 * True when the name is now registered, which includes the case where it
 * already was and to the same type. False when a different type is already
 * registered under that name, and the first one stays: a registry that let the
 * second win would change what a name means underneath whoever is already using
 * it. Go panics here; burrow returns the answer and lets the caller decide,
 * because the caller is usually a plugin loader that has somewhere to put it. */
bool type_register(const Type *t);

/* How many names the registry holds. For tests and for a status page. */
Int type_registry_len(void);

/* Whether two descriptors describe the same type.
 *
 * Within one program this is a pointer comparison and nothing else, because a
 * descriptor is defined once. It is not always one program: the same type
 * compiled into two shared libraries arrives twice, at two addresses, and Go
 * does not have this problem because Go links the two definitions together.
 *
 * So the fallback compares the qualified name, the kind and the size. That is
 * as far as it can honestly go. Two unrelated types with one name, one kind and
 * one size are indistinguishable from here, and the answer for two unnamed
 * types is their addresses, since there is no name to compare. */
bool type_same(const Type *a, const Type *b);

/* Write a type's qualified name into buf and return it.
 *
 * The caller supplies the buffer because a descriptor does not carry the joined
 * name: it holds the package path and the name separately, joining them would
 * mean a string allocated per type at startup, and almost nothing ever asks.
 *
 * A name longer than the buffer is truncated rather than allocated for. 256
 * bytes holds every name in Go's standard library with room to spare. */
BURROW_BORROWS(ret, buf) Str type_qualified_name(const Type *t, Byte *buf, Int cap);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TYPE_H */
