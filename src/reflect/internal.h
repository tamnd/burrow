/* What the files of the reflect package share.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_REFLECT_INTERNAL_H
#define BURROW_SRC_REFLECT_INTERNAL_H

#include "burrow/reflect.h"

#include "burrow/core.h"

#include <stddef.h>

/* The bits of ReflectValue.flag. ADDR: the value is somewhere it can be
 * written, because it was reached through a pointer or a slice. STICKY_RO: it
 * came out of an unexported field, so it may be read and not set or handed
 * out. EMBED_RO: it is an unexported embedded struct, which is read-only
 * itself while its exported fields are not, as in Go. RO is either. */
#define BURROW__RF_ADDR 1u
#define BURROW__RF_STICKY_RO 2u
#define BURROW__RF_EMBED_RO 4u
#define BURROW__RF_RO (BURROW__RF_STICKY_RO | BURROW__RF_EMBED_RO)

/* Go's flag.ro: what a value made from v carries, which is STICKY_RO when v
 * was read-only in either way. */
#define BURROW__RF_RO_OF(v) (((v).flag & BURROW__RF_RO) ? BURROW__RF_STICKY_RO : 0u)

/* A copy of s that lasts as long as the program, made once per distinct s.
 * Type strings and panic messages go through this, since both have to outlive
 * the frame that made them and neither comes in more than a handful of kinds. */
BURROW_STATIC(ret) Str burrow__reflect_intern(Str s);

/* a, b and c run together, interned. */
BURROW_STATIC(ret) Str burrow__reflect_concat(Str a, Str b, Str c);

/* Panic with a, b and c run together, as Go's reflect does with a message and a
 * type's String. */
BURROW_NORETURN void burrow__reflect_panic(Str a, Str b, Str c);

/* Panic with a ReflectValueError for method on a value of kind k, as an Error
 * so that it prints the way Go's does. */
BURROW_NORETURN void burrow__reflect_panic_kind(Str method, Kind k);

/* What an interface value at *p of type *t holds, looked through once. False
 * for a nil interface. When the vtable does not name the dynamic type, *t and
 * *p are left alone and the answer is still true. */
bool burrow__reflect_unwrap(const Type **t, void **p);

/* Whether a field is embedded, which unlike field_is_embedded also covers a
 * pointer to a named type, Go's embedded *T. */
bool burrow__reflect_embedded(const Field *f);

/* The StructField for field i of the struct t. */
ReflectStructField burrow__reflect_field(const Type *t, Int i);

/* Whether t and u are the same type. The same descriptor always is, and two
 * descriptors for the same unnamed type, which separate files can each make,
 * are too. */
bool burrow__reflect_identical(const Type *t, const Type *u);

/* Whether a value of type v can be stored as a dst without a conversion, Go's
 * directlyAssignable. Interfaces it leaves to the caller. */
bool burrow__reflect_assignable(const Type *dst, const Type *v);

/* Where a ReflectMapIter is: before the first Next, on an entry, or past the
 * last one. */
#define BURROW__RF_ITER_FRESH 0
#define BURROW__RF_ITER_ON 1
#define BURROW__RF_ITER_DONE 2

/* Where the bytes of x are as a value of type dst, for storing them there:
 * x's own when its type is assignable to dst, and *box, filled in, when dst is
 * the empty interface. Anything else panics the way Go's assignTo does, with
 * context at the front of the message. */
const void *burrow__reflect_assign(ReflectValue x, const Type *dst, Any *box,
                                   const char *context);

/* At least size bytes of zeros, which nothing may write, for Zero to point at.
 * They last the program. */
BURROW_STATIC(ret) const void *burrow__reflect_zero(size_t size);

#endif /* BURROW_SRC_REFLECT_INTERNAL_H */
