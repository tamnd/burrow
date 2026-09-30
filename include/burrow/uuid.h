/* uuid, RFC 9562 universally unique identifiers.
 *
 * Go 1.27's uuid. uuid_new gives a random version 4 UUID, uuid_new_v7 one that
 * starts with the time so that later ones sort after earlier ones, and
 * uuid_parse_str and uuid_string go between a Uuid and its text:
 *
 *     Uuid id = uuid_new();
 *     Str s = uuid_string(id, a);          // "f81d4fae-7dec-4d0a-a765-00a0c91e6bf6"
 *     Uuid back = uuid_parse_str(s, &err);
 *
 * A Uuid is sixteen bytes in a struct, so it is passed and returned by value
 * and copied with =. Go compares UUIDs with ==. Here that is
 * uuid_cmp(u, v) == 0, or memcmp of the two.
 *
 * The random bits come from the operating system's generator, which is what
 * Go's crypto/rand reads too. Inside a synctest bubble uuid_new_v7 reads the
 * bubble's clock, as Go's does.
 *
 * Every function here is safe to call from any number of threads.
 *
 * Derived from Go's src/uuid/uuid.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package uuid */

#ifndef BURROW_UUID_H
#define BURROW_UUID_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* uuid.UUID. Go's is a [16]byte, and so is the descriptor: a Uuid inside a
 * struct marshals to JSON as its text, through the MarshalText method the
 * descriptor carries. */
typedef struct Uuid {
    Byte b[16];
} Uuid;

extern const Type burrow_type_Uuid;
#define TYPE_UUID TYPE_OF(Uuid)

/* uuid.Parse. Takes the four forms Go does, in either case:
 *
 *     f81d4fae-7dec-11d0-a765-00a0c91e6bf6
 *     {f81d4fae-7dec-11d0-a765-00a0c91e6bf6}
 *     urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6
 *     f81d4fae7dec11d0a76500a0c91e6bf6
 *
 * Anything else sets *err to an error that reads "invalid uuid" and gives the
 * zero Uuid.
 *
 * It is not called uuid_parse, and Compare is not uuid_compare, because libuuid
 * and the macOS C library already define both with other signatures. */
Uuid uuid_parse_str(Str s, Error *err);

/* uuid.MustParse. uuid_parse_str, panicking on an error. For tests and
 * constants. */
Uuid uuid_must_parse(Str s);

/* uuid.New. A new UUID for a program with no reason to want a particular
 * version. Today it is uuid_new_v4. */
Uuid uuid_new(void);

/* uuid.NewV4. 122 random bits, with the version and variant set. */
Uuid uuid_new_v4(void);

/* uuid.NewV7. The Unix time in milliseconds in the top 48 bits, a 12 bit
 * fraction of a millisecond after the version, and 62 random bits. Each one
 * sorts after the one before, even when two come in the same 1/4096 of a
 * millisecond, unless the system clock goes backwards. */
Uuid uuid_new_v7(void);

/* uuid.Nil, all zero bits, and uuid.Max, all one bits. The Nil UUID is also
 * what a zeroed Uuid is. */
Uuid uuid_nil(void);
Uuid uuid_max(void);

/* uuid.UUID.String. The 36 character lowercase form with dashes. */
BURROW_OWNS(ret) Str uuid_string(Uuid u, Alloc *a);

/* uuid.UUID.MarshalText and AppendText. The text uuid_string gives, as bytes,
 * and appended to b. Neither can fail, and err is set to no error. */
BURROW_OWNS(ret) Slice uuid_marshal_text(Uuid u, Alloc *a, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice uuid_append_text(Uuid u, Alloc *a,
                                                               Slice b, Error *err);

/* uuid.UUID.UnmarshalText. uuid_parse_str over bytes, into *u. *u is left alone
 * on an error. */
BURROW_BORROWS(ret) Error uuid_unmarshal_text(Uuid *u, Slice text);

/* uuid.UUID.Compare. -1, 0 or +1, comparing the bytes in order, which is the
 * order RFC 9562 sorts in. */
Int uuid_cmp(Uuid u, Uuid v);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_UUID_H */
