/* What the files of the reflect package share.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_REFLECT_INTERNAL_H
#define BURROW_SRC_REFLECT_INTERNAL_H

#include "burrow/reflect.h"

#include "burrow/core.h"

/* The bits of ReflectValue.flag. ADDR: the value is somewhere it can be
 * written, because it was reached through a pointer or a slice. RO: it came
 * out of an unexported field, so it may be read and not set or handed out. */
#define BURROW__RF_ADDR 1u
#define BURROW__RF_RO 2u

/* A copy of s that lasts as long as the program, made once per distinct s.
 * Type strings and panic messages go through this, since both have to outlive
 * the frame that made them and neither comes in more than a handful of kinds. */
BURROW_STATIC(ret) Str burrow__reflect_intern(Str s);

/* Panic with a, b and c run together, as Go's reflect does with a message and a
 * type's String. */
BURROW_NORETURN void burrow__reflect_panic(Str a, Str b, Str c);

/* Panic with a ReflectValueError for method on a value of kind k. */
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

#endif /* BURROW_SRC_REFLECT_INTERNAL_H */
