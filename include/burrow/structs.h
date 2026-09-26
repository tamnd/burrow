/* structs, marker types for struct fields.
 *
 * Go's structs, which has one member. structs.HostLayout asks the Go compiler
 * to lay a struct out the way the host's C ABI would. A C compiler does that
 * for every struct already, so in C the marker has nothing left to do.
 *
 * The type is here so that code ported from Go has something to name, and so
 * that a descriptor can say a field is one. ISO C has no empty structs, so it
 * is one byte where Go's is zero. Code that wants the marker without the byte
 * writes STRUCTS_HOST_LAYOUT at the top of the struct, which expands to
 * nothing, as a note for the reader.
 *
 *     typedef struct Sockaddr {
 *         STRUCTS_HOST_LAYOUT
 *         uint16_t family;
 *         Byte data[14];
 *     } Sockaddr;
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package structs */

#ifndef BURROW_STRUCTS_H
#define BURROW_STRUCTS_H

#include "burrow/core.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* structs.HostLayout. See the top of this header. */
typedef struct StructsHostLayout {
    Byte unused_;
} StructsHostLayout;

/* The field form of the marker, which adds nothing to the struct. */
#define STRUCTS_HOST_LAYOUT

extern const Type burrow_type_StructsHostLayout;
#define TYPE_STRUCTS_HOST_LAYOUT TYPE_OF(StructsHostLayout)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_STRUCTS_H */
