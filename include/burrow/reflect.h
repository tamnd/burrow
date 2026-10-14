/* reflect, looking at values whose types are not known until run time.
 *
 * Go's reflect package, built on the descriptors in burrow/type.h. A
 * ReflectType is a pointer to one of those descriptors, and a ReflectValue is a
 * descriptor and a pointer to the bytes of a value of that type:
 *
 *     Point p = {1, 2};
 *     ReflectValue v = reflect_value_of(BURROW_ANY(TYPE_OF(Point), &p));
 *     for (Int i = 0; i < reflect_value_num_field(v); i++) {
 *         ReflectStructField f = reflect_type_field(reflect_value_type(v), i);
 *         ReflectValue x = reflect_value_field(v, i);
 *         // f.name is "X", then "Y", and reflect_value_int(x) is 1, then 2
 *     }
 *
 * Reflection only reaches types that have a descriptor, which means the
 * builtins, types declared with burrow/declare.h and types somebody described
 * by hand. docs/design/07-reflect.md says why, and what to do about a struct
 * from a header you cannot change.
 *
 * A Value points and never copies
 *
 * Go's ValueOf copies its argument into the interface it receives, so the
 * Value it gives back owns what it describes. Any in burrow always points, so a
 * ReflectValue made from one points at the caller's variable. That has two
 * consequences worth knowing:
 *
 *   - The variable has to outlive the Value, the same rule as for the Any.
 *   - Writing through a settable Value, one reached through a pointer or the
 *     element of a slice, writes the variable. That part is what Go does too.
 *
 * reflect_value_interface hands back an Any that points at the same bytes, and
 * reflect_value_map_index and reflect_value_map_keys hand back Values that
 * point into the map, which stay good until the map is next written to.
 *
 * Misuse panics
 *
 * Asking a Value for something its kind does not have, Int of a string or
 * Field of a slice, is a mistake in the program rather than a condition to
 * handle, and Go panics with a *ValueError saying which call it was. So does
 * this, with a ReflectValueError, which BURROW_CATCH can take apart with
 * any_assert. The other panics carry Go's text.
 *
 * Interfaces
 *
 * An interface value in burrow is a vtable and a pointer to the value behind
 * it, and the vtable names the value's type. Looking through one, which
 * reflect_value_of, reflect_type_of and reflect_value_elem all do, gives that
 * type and that pointer. For an error made by errors_new, whose vtable names
 * no type, the answer stays the error itself.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package reflect */

#ifndef BURROW_REFLECT_H
#define BURROW_REFLECT_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/iter.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ kinds */

/* Kind, which is burrow's Kind under Go's name. The numbers are Go's. */
typedef Kind ReflectKind;

#define REFLECT_INVALID KIND_INVALID
#define REFLECT_BOOL KIND_BOOL
#define REFLECT_INT KIND_INT
#define REFLECT_INT8 KIND_INT8
#define REFLECT_INT16 KIND_INT16
#define REFLECT_INT32 KIND_INT32
#define REFLECT_INT64 KIND_INT64
#define REFLECT_UINT KIND_UINT
#define REFLECT_UINT8 KIND_UINT8
#define REFLECT_UINT16 KIND_UINT16
#define REFLECT_UINT32 KIND_UINT32
#define REFLECT_UINT64 KIND_UINT64
#define REFLECT_UINTPTR KIND_UINTPTR
#define REFLECT_FLOAT32 KIND_FLOAT32
#define REFLECT_FLOAT64 KIND_FLOAT64
#define REFLECT_COMPLEX64 KIND_COMPLEX64
#define REFLECT_COMPLEX128 KIND_COMPLEX128
#define REFLECT_ARRAY KIND_ARRAY
#define REFLECT_CHAN KIND_CHAN
#define REFLECT_FUNC KIND_FUNC
#define REFLECT_INTERFACE KIND_INTERFACE
#define REFLECT_MAP KIND_MAP
#define REFLECT_POINTER KIND_POINTER
#define REFLECT_SLICE KIND_SLICE
#define REFLECT_STRING KIND_STRING
#define REFLECT_STRUCT KIND_STRUCT
#define REFLECT_UNSAFE_POINTER KIND_UNSAFE_POINTER

/* The old name for REFLECT_POINTER, which Go keeps. */
#define REFLECT_PTR KIND_POINTER

/* Kind.String: "int", "ptr", "struct", and "kind" and the number for one that
 * is out of range. */
BURROW_STATIC(ret) Str reflect_kind_string(ReflectKind k);

/* A channel type's direction. A channel descriptor keeps it in its len field,
 * where 0 means both ways, so a descriptor that never set it is the plain
 * chan T it most likely is. */
typedef enum ReflectChanDir {
    REFLECT_RECV_DIR = 1,
    REFLECT_SEND_DIR = 2,
    REFLECT_BOTH_DIR = REFLECT_RECV_DIR | REFLECT_SEND_DIR
} ReflectChanDir;

/* ChanDir.String: "<-chan", "chan<-", "chan", or "ChanDir" and the number. */
BURROW_STATIC(ret) Str reflect_chan_dir_string(ReflectChanDir d);

/* ------------------------------------------------------------------- types */

/* Type. Go's is an interface with one implementation, and burrow already has
 * the one implementation, so this is a pointer to a descriptor and the
 * functions below are its methods. Two types are the same type when the
 * pointers are equal, which is also true in Go. NULL is the nil Type. */
typedef const Type *ReflectType;

/* StructTag, a struct tag exactly as it was written. */
typedef Str ReflectStructTag;

/* StructTag.Get and StructTag.Lookup, which are tag_get and tag_lookup from
 * burrow/type.h: the value comes back unquoted and made in a. Lookup tells an
 * empty value apart from a missing key. */
BURROW_OWNS(ret) Str reflect_struct_tag_get(ReflectStructTag tag, Alloc *a, Str key);
BURROW_OWNS(value) bool reflect_struct_tag_lookup(ReflectStructTag tag, Alloc *a,
                                                  Str key, Str *value);

/* StructField, one field of a struct as reflect describes it.
 *
 * index is a Slice of Int, the path of field numbers from the outer struct,
 * which is longer than one for a field promoted from an embedded struct. For a
 * field straight from reflect_type_field it is one long and points at memory
 * the package keeps, so nothing needs freeing. */
typedef struct ReflectStructField {
    Str name;
    /* Empty for an exported field, and the struct's package for one that is
     * not, which is how Go tells them apart. */
    Str pkg_path;
    ReflectType type;
    ReflectStructTag tag;
    Uintptr offset;
    Slice index;
    bool anonymous;
} ReflectStructField;

/* StructField.IsExported. */
bool reflect_struct_field_is_exported(ReflectStructField f);

/* The Type methods. Each panics the way Go's does when asked about a kind that
 * does not have the thing: Elem of an int, Key of a slice. */
Int reflect_type_align(ReflectType t);
Int reflect_type_field_align(ReflectType t);
Uintptr reflect_type_size(ReflectType t);
ReflectKind reflect_type_kind(ReflectType t);

/* Name is "" for an unnamed type such as []int, and PkgPath is "" for that and
 * for the builtins. */
BURROW_BORROWS(ret, t) Str reflect_type_name(ReflectType t);
BURROW_BORROWS(ret, t) Str reflect_type_pkg_path(ReflectType t);

/* String, Go's spelling of the type: "int", "[]uint8", "map[string]*image.Point".
 * The package is named by the last element of its path. The text is made once
 * per type and kept, so it is good for as long as the program runs. */
BURROW_STATIC(ret) Str reflect_type_string(ReflectType t);

/* The size in bits of a number type. */
Int reflect_type_bits(ReflectType t);

bool reflect_type_comparable(ReflectType t);
ReflectChanDir reflect_type_chan_dir(ReflectType t);

/* Elem works for arrays, channels, maps, pointers and slices, and Key for
 * maps. */
BURROW_STATIC(ret) ReflectType reflect_type_elem(ReflectType t);
BURROW_STATIC(ret) ReflectType reflect_type_key(ReflectType t);
Int reflect_type_len(ReflectType t);

Int reflect_type_num_field(ReflectType t);
ReflectStructField reflect_type_field(ReflectType t, Int i);

/* FieldByIndex: the field at the end of a path of field numbers, going through
 * embedded structs. The index of what comes back points at the one passed in. */
BURROW_BORROWS(ret, index) ReflectStructField reflect_type_field_by_index(ReflectType t,
                                                                          Slice index);

/* Go's function type for FieldByNameFunc's match, func(string) bool. */
BURROW_FUNC(ReflectNameMatch, bool, Str name);

/* FieldByName and FieldByNameFunc: the field with that name, looking into
 * embedded structs the way Go's selector rules do, so that the shallowest
 * match wins and two at the same depth cancel each other out. Returns false
 * when there is no such field. A promoted field's index is new memory in a. */
BURROW_OWNS(f) bool reflect_type_field_by_name(Alloc *a, ReflectType t, Str name,
                                               ReflectStructField *f);
BURROW_OWNS(f) bool reflect_type_field_by_name_func(Alloc *a, ReflectType t,
                                                    ReflectNameMatch match,
                                                    ReflectStructField *f);

/* VisibleFields: every field reachable from t without being hidden by another,
 * embedded ones included, in Go's order. A Slice of ReflectStructField made in
 * a, whose indexes are made in a too, so an arena is the easy way to let go of
 * all of it. */
BURROW_OWNS(ret) Slice reflect_visible_fields(Alloc *a, ReflectType t);

/* Function types. In and Out count from zero, IsVariadic says whether the last
 * parameter is Go's ...T, which in C is a Slice. */
Int reflect_type_num_in(ReflectType t);
Int reflect_type_num_out(ReflectType t);
BURROW_STATIC(ret) ReflectType reflect_type_in(ReflectType t, Int i);
BURROW_STATIC(ret) ReflectType reflect_type_out(ReflectType t, Int i);
bool reflect_type_is_variadic(ReflectType t);

/* NumMethod counts exported methods, which is what Go counts. */
Int reflect_type_num_method(ReflectType t);

/* Whether x fits in the type, which has to be a number type of that family. */
bool reflect_type_overflow_int(ReflectType t, int64_t x);
bool reflect_type_overflow_uint(ReflectType t, uint64_t x);
bool reflect_type_overflow_float(ReflectType t, double x);
bool reflect_type_overflow_complex(ReflectType t, Complex128 x);

/* Whether a range loop could go over a value of the type with one variable or
 * with two. */
bool reflect_type_can_seq(ReflectType t);
bool reflect_type_can_seq2(ReflectType t);

/* TypeOf: the dynamic type of i, NULL for a nil one. */
BURROW_STATIC(ret) ReflectType reflect_type_of(Any i);

/* ------------------------------------------------------------------ values */

/* Value: a type and where a value of it is. The zero ReflectValue is Go's zero
 * Value, the one IsValid says no to. flag is the package's own business. */
typedef struct ReflectValue {
    ReflectType typ;
    void *ptr;
    uint32_t flag;
} ReflectValue;

/* ValueError, what a Value method panics with when it is called on a Value of
 * the wrong kind. method is the Go name of the method, as in
 * "reflect.Value.Int". */
typedef struct ReflectValueError {
    Str method;
    ReflectKind kind;
} ReflectValueError;

/* ValueError.Error: "reflect: call of reflect.Value.Int on string Value". Made
 * in a. */
BURROW_OWNS(ret) Str reflect_value_error_error(ReflectValueError *e, Alloc *a);

extern const Type burrow_type_ReflectValueError;
#define TYPE_REFLECT_VALUE_ERROR TYPE_OF(ReflectValueError)

extern const Type burrow_type_ReflectValue;
#define TYPE_REFLECT_VALUE TYPE_OF(ReflectValue)

extern const Type burrow_type_ReflectStructField;
#define TYPE_REFLECT_STRUCT_FIELD TYPE_OF(ReflectStructField)

/* ValueOf: a Value for what i holds, looking through an interface, and the
 * zero Value for a nil one. It points at i's data rather than copying it. */
BURROW_BORROWS(ret, i) ReflectValue reflect_value_of(Any i);

/* Indirect: what v points at when it is a pointer, and v otherwise. */
ReflectValue reflect_indirect(ReflectValue v);

ReflectKind reflect_value_kind(ReflectValue v);
BURROW_STATIC(ret) ReflectType reflect_value_type(ReflectValue v);
bool reflect_value_is_valid(ReflectValue v);

/* IsNil works for channels, functions, interfaces, maps, pointers, unsafe
 * pointers and slices, and panics for the rest. IsZero works for everything
 * but the zero Value. */
bool reflect_value_is_nil(ReflectValue v);
bool reflect_value_is_zero(ReflectValue v);

/* Whether the value can be had by address, and set, which also needs it not to
 * have come out of an unexported field. */
bool reflect_value_can_addr(ReflectValue v);
bool reflect_value_can_set(ReflectValue v);

/* Whether reflect_value_interface will hand the value out, which it will not
 * for one that came out of an unexported field. */
bool reflect_value_can_interface(ReflectValue v);

/* Interface: the value as an Any. It points at the value rather than copying
 * it. An interface value comes back as what it holds. */
BURROW_BORROWS(ret, v) Any reflect_value_interface(ReflectValue v);

/* InterfaceData, which Go deprecated: the two words of an interface value. */
typedef struct ReflectInterfaceData {
    Uintptr word[2];
} ReflectInterfaceData;
ReflectInterfaceData reflect_value_interface_data(ReflectValue v);

/* Whether Int, Uint, Float and Complex would work. */
bool reflect_value_can_int(ReflectValue v);
bool reflect_value_can_uint(ReflectValue v);
bool reflect_value_can_float(ReflectValue v);
bool reflect_value_can_complex(ReflectValue v);

/* The value itself, for each family of kinds. Int takes every signed integer
 * kind and gives it back as int64, and so on. */
bool reflect_value_bool(ReflectValue v);
int64_t reflect_value_int(ReflectValue v);
uint64_t reflect_value_uint(ReflectValue v);
double reflect_value_float(ReflectValue v);
Complex128 reflect_value_complex(ReflectValue v);

/* String: the string for a string, and for anything else Go's "<T Value>",
 * which is why it does not panic the way Int does. */
BURROW_BORROWS(ret, v) Str reflect_value_string(ReflectValue v);

/* Bytes: a []byte, or an addressable [N]byte as a slice over it. */
BURROW_BORROWS(ret, v) Slice reflect_value_bytes(ReflectValue v);

/* Whether x fits in v's type. */
bool reflect_value_overflow_int(ReflectValue v, int64_t x);
bool reflect_value_overflow_uint(ReflectValue v, uint64_t x);
bool reflect_value_overflow_float(ReflectValue v, double x);
bool reflect_value_overflow_complex(ReflectValue v, Complex128 x);

/* Pointer and UnsafePointer: the pointer behind a channel, function, map,
 * pointer, slice or unsafe pointer. A function gives its code pointer. */
Uintptr reflect_value_pointer(ReflectValue v);
BURROW_BORROWS(ret, v) void *reflect_value_unsafe_pointer(ReflectValue v);

/* UnsafeAddr: where an addressable value is. */
Uintptr reflect_value_unsafe_addr(ReflectValue v);

/* Len and Cap: arrays, channels, maps (Len only), slices, strings (Len only),
 * and pointers to arrays. */
Int reflect_value_len(ReflectValue v);
Int reflect_value_cap(ReflectValue v);

/* Index: element i of an array, slice or string. A slice's elements are
 * addressable and so are an addressable array's. */
BURROW_BORROWS(ret, v) ReflectValue reflect_value_index(ReflectValue v, Int i);

/* Elem: what a pointer points at, or what an interface holds. The zero Value
 * for a nil one. */
BURROW_BORROWS(ret, v) ReflectValue reflect_value_elem(ReflectValue v);

/* Struct fields. FieldByName and FieldByNameFunc give the zero Value when there
 * is no such field. FieldByIndex panics when the path goes through a nil
 * pointer, and FieldByIndexErr says so instead. */
Int reflect_value_num_field(ReflectValue v);
BURROW_BORROWS(ret, v) ReflectValue reflect_value_field(ReflectValue v, Int i);
BURROW_BORROWS(ret, v) ReflectValue reflect_value_field_by_index(ReflectValue v,
                                                                 Slice index);
BURROW_BORROWS(ret, v) ReflectValue reflect_value_field_by_index_err(ReflectValue v,
                                                                     Slice index,
                                                                     Error *err);
BURROW_BORROWS(ret, v) ReflectValue reflect_value_field_by_name(ReflectValue v,
                                                                Str name);
BURROW_BORROWS(ret, v) ReflectValue
reflect_value_field_by_name_func(ReflectValue v, ReflectNameMatch match);

/* NumMethod: exported methods, as for the type. */
Int reflect_value_num_method(ReflectValue v);

/* Maps. MapIndex gives the zero Value for a key that is not there, and MapKeys
 * a Slice of ReflectValue made in a, in no particular order. Both point into
 * the map. */
BURROW_BORROWS(ret, v) ReflectValue reflect_value_map_index(ReflectValue v,
                                                            ReflectValue key);
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) Slice reflect_value_map_keys(ReflectValue v,
                                                                     Alloc *a);

/* MapIter, an iterator over a map. A zeroed one is ready for
 * reflect_map_iter_reset, so one can live on the stack:
 *
 *     ReflectMapIter it = {0};
 *     reflect_map_iter_reset(&it, m);
 *     while (reflect_map_iter_next(&it)) {
 *         ReflectValue k = reflect_map_iter_key(&it);
 *         ReflectValue e = reflect_map_iter_value(&it);
 *     }
 */
typedef struct ReflectMapIter {
    ReflectValue m;
    MapIter it;
    const void *key;
    void *val;
    /* 0 before the first Next, 1 while on an entry, 2 once the map is done. */
    int state;
} ReflectMapIter;

/* MapRange: a new iterator over the map v, made in a. */
BURROW_OWNS(ret) ReflectMapIter *reflect_value_map_range(ReflectValue v, Alloc *a);
bool reflect_map_iter_next(ReflectMapIter *it);
BURROW_BORROWS(ret, it) ReflectValue reflect_map_iter_key(ReflectMapIter *it);
BURROW_BORROWS(ret, it) ReflectValue reflect_map_iter_value(ReflectMapIter *it);

/* Reset: start again over v, or over nothing when v is the zero Value. */
void reflect_map_iter_reset(ReflectMapIter *it, ReflectValue v);

/* Seq and Seq2: the iterators a range loop over v would make. Seq yields a
 * const ReflectValue * for each element, or for each key of a map, and Seq2
 * yields two, the index or key and then the element. What Seq hands to the
 * callback lives only for that call. Made in a. */
BURROW_OWNS(ret) IterSeq reflect_value_seq(ReflectValue v, Alloc *a);
BURROW_OWNS(ret) IterSeq2 reflect_value_seq2(ReflectValue v, Alloc *a);

/* Comparable: whether Equal would work, which for an interface depends on what
 * it holds. Equal: Go's ==, which panics when the values are not comparable. */
bool reflect_value_comparable(ReflectValue v);
bool reflect_value_equal(ReflectValue v, ReflectValue u);

/* DeepEqual: Go's deep equality, which follows pointers and compares slices
 * and maps element by element, and copes with cycles. */
bool reflect_deep_equal(Any x, Any y);

/* Swapper: a function swapping elements i and j of the slice that s holds. It
 * points at s's data, so the slice header has to stay where it is. */
BURROW_BORROWS(ret, s) SwapFunc reflect_swapper(Any s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_REFLECT_H */
