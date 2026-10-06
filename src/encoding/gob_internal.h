/* What gob.c, gob_encode.c and gob_decode.c share.
 *
 * gob.c holds the type side, which is Go's type.go: the numbers types go by,
 * the descriptions sent for them, the table of registered names, and what a
 * type looks like to gob once its pointers are taken off. It is process wide,
 * as it is in Go, since the numbers an encoder hands out have to agree with
 * the ones every other encoder in the program hands out.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_GOB_INTERNAL_H
#define BURROW_SRC_ENCODING_GOB_INTERNAL_H

#include "burrow/encoding/gob.h"

#include "burrow/core.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"

/* The builtin type ids. */
enum {
    GOB_T_BOOL = 1,
    GOB_T_INT = 2,
    GOB_T_UINT = 3,
    GOB_T_FLOAT = 4,
    GOB_T_BYTES = 5,
    GOB_T_STRING = 6,
    GOB_T_COMPLEX = 7,
    GOB_T_INTERFACE = 8,
    GOB_T_WIRE_TYPE = 16,
    GOB_FIRST_USER_ID = 64,
};

/* xGob and xBinary. Go's xText is never chosen by anything, so it is not
 * here either. */
enum { GOB_X_GOB = 1, GOB_X_BINARY = 2, GOB_X_TEXT = 3 };

/* Go's cap on nested ignored types, and the cap on nesting in general on a
 * thread whose stack bounds the system will not say. */
#define GOB_MAX_DEPTH 10000

/* How much stack the encoder and the decoder leave alone. Nesting that would
 * go below it is an error rather than a stack overflow. */
#define GOB_STACK_MARGIN ((uintptr_t)64 << 10)

/* The cap on nesting on wasip1. Every goroutine there runs its calls on the
 * engine's own stack, which nothing inside the module can measure and which is
 * 512 KiB in wasmtime unless the host asks for more, so a count is all there is
 * to go on. */
#define GOB_WASI_MAX_DEPTH 1000

/* tooBig: the largest message, 8 GB with a 64 bit int and 1 GB otherwise. */
#define GOB_TOO_BIG ((uint64_t)(1u << 30) << (sizeof(void *) == 8 ? 3 : 0))

/* One of Go's gobType implementations. kind says which, and the fields past
 * name and id are the ones that kind has. */
typedef enum GobKind {
    GOB_COMMON,
    GOB_ARRAY,
    GOB_SLICE,
    GOB_STRUCT,
    GOB_MAP,
    GOB_GOB_ENCODER,
} GobKind;

typedef struct GobFieldType {
    Str name;
    int32_t id;
} GobFieldType;

typedef struct GobType {
    GobKind kind;
    Str name;
    int32_t id;
    int32_t elem;        /* array, slice, map */
    int32_t key;         /* map */
    int64_t len;         /* array */
    GobFieldType *field; /* struct */
    Int nfield;
    Int capfield;
} GobType;

/* wireType: one of the seven set, or in a hostile stream more than one. */
enum {
    GOB_W_ARRAY,
    GOB_W_SLICE,
    GOB_W_STRUCT,
    GOB_W_MAP,
    GOB_W_GOB_ENCODER,
    GOB_W_BINARY_MARSHALER,
    GOB_W_TEXT_MARSHALER,
    GOB_W_COUNT
};

typedef struct GobWireType {
    GobType *t[GOB_W_COUNT];
} GobWireType;

/* userTypeInfo. user is the type as given and base the one under all its
 * pointers. The methods, when there are any, are always on base. */
typedef struct GobUserType {
    const Type *user;
    const Type *base;
    int indir;
    int external_enc;
    int external_dec;
    int enc_indir;
    int dec_indir;
    const Method *enc_method;
    const Method *dec_method;
    /* The GobEncPlan of a struct base, built the first time it is encoded
     * and read without the lock after that. */
    void *enc_plan;
    /* The GobUserType of the element of a slice or array base, the same
     * way. */
    void *elem_ut;
} GobUserType;

/* One field of a struct as the encoder sends it. */
typedef struct GobEncField {
    const Type *type;
    const GobUserType *ut;
    size_t offset;
    int32_t wire;
} GobEncField;

/* The sent fields of a struct, in order, which is what Go's encEngine holds
 * and what the encoder would otherwise look up field by field on every
 * value. */
typedef struct GobEncPlan {
    Int n;
    GobEncField f[];
} GobEncPlan;

/* typeInfo. */
typedef struct GobTypeInfo {
    int32_t id;
    GobWireType wire;
    /* Whether compileEnc has succeeded for this type, which it checks once
     * and which fails the same way every time it fails. */
    bool enc_ok;
    bool enc_building;
} GobTypeInfo;

/* The Go spelling of a type, reflect's String(), into a. */
/* Whether one more level of nesting would take the stack below the margin.
 * *floor caches the line for one Encode or Decode and starts at 0; depth is
 * how deep the caller already is. */
bool burrow__gob_stack_low(uintptr_t *floor, int depth);

Str burrow__gob_type_string(Alloc *a, const Type *t);

/* Go's Name(): empty for an unnamed type. */
Str burrow__gob_type_name(const Type *t);

/* The one descriptor that stands for t, so that two descriptors of one
 * unnamed type such as []int are one type to gob. */
const Type *burrow__gob_canon(const Type *t);

/* A descriptor for *t, made the first time it is asked for. */
const Type *burrow__gob_ptr_to(const Type *t);

/* validUserType. NULL with *err set on failure. */
const GobUserType *burrow__gob_user_type(const Type *t, Error *err);

/* The plan for ut, whose base is a struct. NULL with *err set when a field
 * type is one gob can't handle. */
const GobEncPlan *burrow__gob_enc_plan(const GobUserType *ut, Error *err);

/* The user type of the element of ut's base, a slice or array. */
const GobUserType *burrow__gob_elem_ut(const GobUserType *ut, Error *err);

/* getTypeInfo. */
const GobTypeInfo *burrow__gob_type_info(const GobUserType *ut, Error *err);

/* The whole of compileEnc's checking for ut, which Go does before it sends a
 * value of the type. */
Error burrow__gob_check_enc(const GobUserType *ut);

/* idToType and builtinIdToType. NULL for an id with no type. */
const GobType *burrow__gob_id_to_type(int32_t id);
const GobType *burrow__gob_builtin_id_to_type(int32_t id);

/* gobType.string(). */
Str burrow__gob_type_str(Alloc *a, const GobType *t);

/* The name a concrete type was registered under, and the type a name was
 * registered for. false when there is none. */
bool burrow__gob_name_of(const Type *base, Str *name);
const Type *burrow__gob_type_of_name(Str name);

/* isSent and isExported. */
bool burrow__gob_is_exported(Str name);
bool burrow__gob_is_sent(const Field *f);

/* The encoder's and decoder's view of an interface value: the type it holds
 * and where that value is, or NULL for a nil one. */
const Type *burrow__gob_iface_elem(const Type *t, void *p, void **vp);

/* errorf, which puts gob: in front. */
#define burrow__gob_errorf(...) fmt_errorf_v("gob: " __VA_ARGS__)

#endif /* BURROW_SRC_ENCODING_GOB_INTERNAL_H */
