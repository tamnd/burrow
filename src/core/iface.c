/* The interface helpers, and Any.
 *
 * Not derived from Go's source. The representation is C's problem rather than
 * Go's: Go's interface value is a pair the compiler builds and this is a pair
 * the programmer builds, and nothing about the layout could be copied even if
 * it were worth copying. What is Go's is the behaviour, which is what the tests
 * check: what nil means, when a type assertion succeeds, when comparing two
 * interface values is allowed and what happens when it is not.
 *
 * There is very little code here and that is the point. An interface call goes
 * straight through the vtable at the call site with no function in the middle,
 * so the only things that need to exist are the ones that have to look at a
 * type descriptor.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/iface.h"

#include "burrow/runtime.h"

#include <stdio.h>

const Type *iface_type(Iface v) {
    if (v.vt == NULL)
        return NULL;
    return v.vt->self_type;
}

void *iface_assert(Iface v, const Type *want) {
    if (v.vt == NULL || want == NULL)
        return NULL;
    /* Descriptors are unique per type and static, so identity is a pointer
     * comparison. Comparing names instead would make two types from different
     * packages with the same name the same type, which is the bug Go's PkgPath
     * exists to prevent. */
    if (v.vt->self_type != want)
        return NULL;
    return v.data;
}

void *any_assert(Any v, const Type *want) {
    if (v.t == NULL || want == NULL || v.t != want)
        return NULL;
    return v.data;
}

Any any_box(Alloc *a, Any v) {
    Any out = {NULL, NULL};
    void *p;

    if (v.t == NULL || v.data == NULL)
        return out;

    /* A zero sized type has nothing to copy and no bytes anybody may read, so
     * it gets a descriptor and no storage. Go points every such value at one
     * shared address for the same reason. */
    if (v.t->size == 0) {
        out.t = v.t;
        return out;
    }

    p = mem_alloc(a, v.t->size, v.t->align);
    if (p == NULL)
        return out;

    /* Through the descriptor, so a type with its own copy gets it. Shallow, the
     * way assignment is shallow in Go: boxing an Any that holds a Slice copies
     * the three word header and not the elements. */
    type_copy(v.t, p, v.data);

    out.t = v.t;
    out.data = p;
    return out;
}

bool any_equal(Any a, Any b) {
    if (a.t == NULL || b.t == NULL)
        return a.t == b.t;
    if (a.t != b.t)
        return false;

    /* Go decides this at run time and so must this, because the static type on
     * both sides is any and neither compiler can see what is inside. The
     * message is Go's, including the prefix, because that is what somebody
     * searches for after they hit it. */
    if (!type_is_comparable(a.t)) {
        char buf[128];
        Str name = type_name(a.t);
        int n = snprintf(buf, sizeof(buf),
                         "runtime error: comparing uncomparable type %.*s",
                         (int)name.len, (const char *)name.p);
        if (n < 0)
            runtime_throw(BURROW_S("runtime error: comparing uncomparable type"));
        runtime_throw(str_from_bytes(
            buf, (Int)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)));
    }

    return type_equal(a.t, a.data, b.data);
}

/* ---------------------------------------------------------- the descriptor */

static bool any_ops_equal(const void *a, const void *b) {
    return any_equal(*(const Any *)a, *(const Any *)b);
}

static uint64_t any_ops_hash(const void *p, uint64_t seed) {
    const Any *v = (const Any *)p;

    if (v->t == NULL)
        return seed;

    /* The dynamic type goes into the seed rather than into the value, so an Any
     * holding an Int 1 and an Any holding an Int8 1 are different keys. They are
     * different keys in Go too, because an interface value compares equal only
     * when the dynamic types match. */
    return type_hash(v->t, v->data, seed ^ (uint64_t)v->t->hash);
}

static const TypeOps any_ops = {
    any_ops_equal, any_ops_hash, NULL, /* copy: two pointers, memcpy is right */
    NULL,                              /* zero: two pointers, memset is right */
};

/* "interface {}" is what Go's reflect prints for the empty interface, since any
 * is an alias for it rather than a type of its own. */
static const Type any_type = {
    {(const Byte *)"interface {}", 12},
    {NULL, 0},
    KIND_INTERFACE,
    (uint32_t)sizeof(Any),
    (uint16_t)_Alignof(Any),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x616e7900U, /* "any\0", distinct from every builtin's and from error's */
    &any_ops,
};

const Type *const TYPE_ANY = &any_type;
