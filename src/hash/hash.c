/* Derived from Go's src/hash/hash.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/type.h"

/* Go's hash.go is the interface declarations. What is here is the part Go's
 * compiler does for them: the descriptors, and finding Clone on a value. */

/* ------------------------------------------------------------- descriptors */

/* Each method gets a thunk that calls through the vtable of the interface
 * value it is handed, as BURROW_IFACE_TYPE_DEFINE's do. */
#define HASH_HASH_METHODS(M, T)                                                        \
    M(T, BlockSize, block_size, HASH_SIG_BLOCK_SIZE)                                   \
    M(T, Reset, reset, HASH_SIG_RESET)                                                 \
    M(T, Size, size, HASH_SIG_SIZE)                                                    \
    M(T, Sum, sum, HASH_SIG_SUM)                                                       \
    M(T, Write, writer.write, IO_SIG_WRITE)
#define HASH_CLONER_METHODS(M, T)                                                      \
    M(T, BlockSize, hash.block_size, HASH_SIG_BLOCK_SIZE)                              \
    M(T, Clone, clone, HASH_SIG_CLONE)                                                 \
    M(T, Reset, hash.reset, HASH_SIG_RESET)                                            \
    M(T, Size, hash.size, HASH_SIG_SIZE)                                               \
    M(T, Sum, hash.sum, HASH_SIG_SUM)                                                  \
    M(T, Write, hash.writer.write, IO_SIG_WRITE)
HASH_HASH_METHODS(BURROW__IFACE_METHOD_DEFS, Hash)
HASH_CLONER_METHODS(BURROW__IFACE_METHOD_DEFS, HashCloner)
static const Method hash_hash_methods[] = {
    HASH_HASH_METHODS(BURROW__METHOD_ENTRY, Hash)};
static const Method hash_cloner_methods[] = {
    HASH_CLONER_METHODS(BURROW__METHOD_ENTRY, HashCloner)};

#define HASH_IFACE_TYPE(cname, gonm, methods)                                          \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"hash", 4},                                                     \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        (uint16_t)(sizeof(methods) / sizeof((methods)[0])),                            \
        NULL,                                                                          \
        methods,                                                                       \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

HASH_IFACE_TYPE(Hash, "Hash", hash_hash_methods);
HASH_IFACE_TYPE(HashCloner, "Cloner", hash_cloner_methods);

/* Only here to be named in a signature, so as far as reflection can tell it is
 * an unsafe pointer, with a name that says what it points at. */
const Type burrow_type_HashAllocArg = {
    {(const Byte *)"*Alloc", 6},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(HashAllocArg),
    (uint16_t)_Alignof(HashAllocArg),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------------ Clone */

static const Str hash_name_clone = {(const Byte *)"Clone", 5};

/* The Clone method on t, if it has one with HASH_SIG_CLONE's shape. One with
 * the right name and another shape is not Cloner's, as in Go, where it would
 * not satisfy the interface. */
static const Method *hash_clone_method(const Type *t) {
    if (t == NULL)
        return NULL;
    const Method *m = type_method_by_name(t, hash_name_clone);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 1 || type_num_out(f) != 2)
        return NULL;
    if (type_in(f, 0) != &burrow_type_HashAllocArg ||
        type_out(f, 0) != &burrow_type_HashCloner || type_out(f, 1) != TYPE_ERROR)
        return NULL;
    return m;
}

bool hash_is_cloner(Hash h) {
    return h.vt != NULL && hash_clone_method(h.vt->writer.self_type) != NULL;
}

HashCloner hash_clone(Alloc *a, Hash h, Error *err) {
    HashCloner out = {NULL, NULL};
    if (h.vt == NULL) {
        BURROW_OUT(err, fmt_errorf_v("interface conversion: interface is nil, not "
                                     "hash.Cloner"));
        return out;
    }
    const Type *t = h.vt->writer.self_type;
    const Method *m = hash_clone_method(t);
    if (m == NULL) {
        /* A vtable with no self_type declines to be asserted to, and has no
         * name to give either. */
        if (t == NULL)
            BURROW_OUT(err, fmt_errorf_v("interface conversion: hash.Hash is not "
                                         "hash.Cloner: missing method Clone"));
        else
            BURROW_OUT(err, fmt_errorf_v("interface conversion: %T is not hash.Cloner: "
                                         "missing method Clone",
                                         BURROW_ANY(t, h.data)));
        return out;
    }
    Error e = BURROW_NO_ERROR;
    HashAllocArg aa = a;
    void *args[1] = {(void *)&aa};
    void *rets[2] = {&out, &e};
    method_call(m, h.data, args, rets);
    BURROW_OUT(err, e);
    return out;
}
