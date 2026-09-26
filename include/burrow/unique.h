/* unique, canonical copies of comparable values.
 *
 * Go's unique. unique_make takes a value and hands back a UniqueHandle for
 * it, and two handles are the same exactly when the values they came from
 * are equal. Comparing two handles is comparing two pointers, however big the
 * values are, which is what net/netip uses it for: an address carries its
 * IPv6 zone as a handle, so comparing addresses never compares strings.
 *
 *     Str zone = BURROW_S("eth0");
 *     UniqueHandle h = UNIQUE_MAKE(Str, &zone);
 *     UniqueHandle g = UNIQUE_MAKE(Str, &(Str){BURROW_S("eth0")});
 *     bool same = unique_handle_eq(h, g);          // true
 *     Str back = UNIQUE_VALUE(Str, h);            // "eth0"
 *
 * The value goes in by pointer with its type descriptor, since the table is
 * kept per type and hashes and compares values the way a Map does. Strings in
 * the value, at the top level or in struct fields and array elements, are
 * copied, as Go copies them, so the value you passed in can go away. Anything
 * a pointer in the value points at is not copied.
 *
 * Go drops a canonical value once no handle to it is left. C has no way to
 * know that, so here the canonical copies live until the process ends. Make
 * handles for values that come from a bounded set, such as zone names and
 * header keys, and not for every string an attacker can send you.
 *
 * Every function here is safe to call from any number of threads.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package unique */

#ifndef BURROW_UNIQUE_H
#define BURROW_UNIQUE_H

#include "burrow/core.h"
#include "burrow/own.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* unique.Handle. A zeroed handle is Go's zero Handle, which no call to
 * unique_make returns. Handles are equal when their value pointers are, so
 * they can be compared with unique_handle_eq, used as map keys, and put in
 * structs that are themselves given to unique_make. */
typedef struct UniqueHandle {
    const void *value;
} UniqueHandle;

/* unique.Make. The handle for the value at value, whose type is t. t must be
 * comparable, as for a Map key, and the program stops if it is not. Panics if
 * the process is out of memory, where Go would stop. */
BURROW_STATIC(ret) UniqueHandle unique_make(const Type *t, const void *value);

/* unique.Handle.Value. The canonical copy, which lives as long as the
 * process. Do not write to it. */
BURROW_STATIC(ret) const void *unique_handle_value(UniqueHandle h);

/* Go's h == g. */
static inline bool unique_handle_eq(UniqueHandle h, UniqueHandle g) {
    return h.value == g.value;
}

/* unique_make and unique_handle_value with the type spelled once, the way
 * Go's type parameter is. p is a pointer to a T. */
#define UNIQUE_MAKE(T, p) unique_make(TYPE_OF(T), (const T *)(p))
#define UNIQUE_VALUE(T, h) (*(const T *)unique_handle_value(h))

extern const Type burrow_type_UniqueHandle;
#define TYPE_UNIQUE_HANDLE TYPE_OF(UniqueHandle)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_UNIQUE_H */
