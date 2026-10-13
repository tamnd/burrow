/* What database/sql needs from database/sql/driver that Go's compiler does for
 * it: finding a Value or Decompose method on a value it holds as an Any.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DATABASE_DRIVER_INTERNAL_H
#define BURROW_DATABASE_DRIVER_INTERNAL_H

#include "burrow/database/sql/driver.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/type.h"

/* A method found on a value: through the interface's own vtable when the Any
 * held the interface value, or through a descriptor's method and the receiver
 * to call it on. nil_elem says the value was a NULL pointer to a type that has
 * the method, which Go treats as SQL's NULL for a Valuer. */
typedef struct burrow__DriverFound {
    const void *vt;
    const Method *m;
    void *recv;
    bool nil_elem;
} burrow__DriverFound;

/* v with any Any it holds looked through, since Go has no interface inside an
 * interface. */
Any burrow__driver_unwrap(Any v);

/* Looks for the method called name on v, the way Go's v.(I) does: on v's
 * type, and when v is a pointer on the type it points at. An Any whose type
 * is iface, and iface may be NULL, holds the interface value itself. shape
 * says whether a method has the signature the interface wants. */
bool burrow__driver_find(Any v, Str name, const Type *iface,
                         bool (*shape)(const Method *m), burrow__DriverFound *out);

/* Go's v.(driver.Valuer) and callValuerValue: false when v has no Value
 * method, and otherwise the call, with a NULL pointer to a type that has the
 * method giving the nil Any. */
bool burrow__driver_is_valuer(Any v);
BURROW_OWNS(ret) DriverValue burrow__driver_call_valuer(Alloc *a, Any v, Error *err);

/* Go's v.(decimalDecompose) and the call. */
bool burrow__driver_is_decimal(Any v);
BURROW_BORROWS(ret, v) DriverDecomposed burrow__driver_decompose(Any v, Bytes buf);

#endif /* BURROW_DATABASE_DRIVER_INTERNAL_H */
