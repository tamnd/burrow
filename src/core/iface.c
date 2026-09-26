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

#include "burrow/error.h"
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
        char buf[BURROW_RUNTIME_ERROR_MAX];
        Str name = type_name(a.t);
        int n = snprintf(buf, sizeof(buf),
                         "runtime error: comparing uncomparable type %.*s",
                         (int)name.len, (const char *)name.p);
        if (n < 0)
            runtime_panic(BURROW_S("runtime error: comparing uncomparable type"));
        /* runtime_panic copies, so this buffer only has to outlive the call and
         * not the jump the call turns into. */
        runtime_panic(str_from_bytes(
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
    if (!type_is_comparable(v->t))
        runtime_panic_unhashable(v->t);

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

/* ------------------------------------------------------------ any from a value
 *
 * The targets of BURROW_ANY_OF. Each copies its argument into the box and
 * points the Any at the copy, so the result is good for as long as the box is,
 * which is the enclosing block of whoever wrote the macro. */

#define BOX_AS(name, ctype, member, desc)                                              \
    Any burrow__any_of_##name(ctype v, burrow__AnyBox *box) {                          \
        box->v.member = v;                                                             \
        return (Any){(desc), (void *)&box->v.member};                                  \
    }

BOX_AS(str, Str, s, TYPE_STRING)
BOX_AS(c64, Complex64, c64, TYPE_COMPLEX64)
BOX_AS(c128, Complex128, c128, TYPE_COMPLEX128)
BOX_AS(bool, bool, b, TYPE_BOOL)
BOX_AS(f32, float, f32, TYPE_FLOAT32)
BOX_AS(f64, double, f64, TYPE_FLOAT64)
BOX_AS(int, Int, i, TYPE_INT)
BOX_AS(uint, Uint, u, TYPE_UINT)
BOX_AS(i8, int8_t, i8, TYPE_INT8)
BOX_AS(i16, int16_t, i16, TYPE_INT16)
BOX_AS(i32, int32_t, i32, TYPE_INT32)
BOX_AS(i64, int64_t, i64, TYPE_INT64)
BOX_AS(u8, uint8_t, u8, TYPE_UINT8)
BOX_AS(u16, uint16_t, u16, TYPE_UINT16)
BOX_AS(u32, uint32_t, u32, TYPE_UINT32)
BOX_AS(u64, uint64_t, u64, TYPE_UINT64)

#undef BOX_AS

Any burrow__any_of_any(Any v, burrow__AnyBox *box) {
    (void)box;
    return v;
}

/* A nil error in an interface is a nil interface in Go, not an interface
 * holding a nil error, so it comes out as a nil Any and prints as <nil>. */
Any burrow__any_of_error(Error v, burrow__AnyBox *box) {
    if (v.vt == NULL)
        return (Any){NULL, NULL};
    box->v.e = v;
    return (Any){TYPE_ERROR, (void *)&box->v.e};
}

/* []elem has no descriptor anywhere, so the box carries one. It is unnamed, as
 * Go's is, and says only what the element type is. */
Any burrow__any_of_slice(Slice v, burrow__AnyBox *box) {
    box->v.sl = v;
    box->t.kind = KIND_SLICE;
    box->t.size = (uint32_t)sizeof(Slice);
    box->t.align = (uint16_t)_Alignof(Slice);
    box->t.elem = v.elem;
    return (Any){&box->t, (void *)&box->v.sl};
}

/* The same for a map, with both types read off the map itself. A nil map knows
 * neither, and prints as map[] regardless. */
Any burrow__any_of_map(Map *v, burrow__AnyBox *box) {
    box->v.m = v;
    box->t.kind = KIND_MAP;
    box->t.size = (uint32_t)sizeof(Map *);
    box->t.align = (uint16_t)_Alignof(Map *);
    if (v != NULL) {
        box->t.key = map_key_type(v);
        box->t.elem = map_val_type(v);
    }
    return (Any){&box->t, (void *)&box->v.m};
}

Any burrow__any_of_cstr(const char *v, burrow__AnyBox *box) {
    if (v == NULL)
        return (Any){NULL, NULL};
    box->v.s = str_from_cstr(v);
    return (Any){TYPE_STRING, (void *)&box->v.s};
}

/* The C integer types that are not one of the typedefs above on this platform.
 * Which Go type they become depends on how wide they turned out to be. */
static Any any_of_signed(int64_t v, size_t size, burrow__AnyBox *box) {
    if (size == 4) {
        box->v.i32 = (int32_t)v;
        return (Any){TYPE_INT32, (void *)&box->v.i32};
    }
    box->v.i64 = v;
    return (Any){TYPE_INT64, (void *)&box->v.i64};
}

static Any any_of_unsigned(uint64_t v, size_t size, burrow__AnyBox *box) {
    if (size == 4) {
        box->v.u32 = (uint32_t)v;
        return (Any){TYPE_UINT32, (void *)&box->v.u32};
    }
    box->v.u64 = v;
    return (Any){TYPE_UINT64, (void *)&box->v.u64};
}

/* C's int is Go's int, widened, because the place it turns up is a literal and
 * an untyped constant in Go is an int too. So 42 prints the same both ways and
 * %T of it says int. int32_t is usually the same C type and gets the same
 * answer, which is the price of the literal coming out right. */
Any burrow__any_of_cint(int v, burrow__AnyBox *box) {
    box->v.i = (Int)v;
    return (Any){TYPE_INT, (void *)&box->v.i};
}

Any burrow__any_of_cuint(unsigned v, burrow__AnyBox *box) {
    box->v.u = (Uint)v;
    return (Any){TYPE_UINT, (void *)&box->v.u};
}

/* char is a byte whichever way the platform signs it. */
Any burrow__any_of_char(char v, burrow__AnyBox *box) {
    box->v.u8 = (uint8_t)v;
    return (Any){TYPE_UINT8, (void *)&box->v.u8};
}

Any burrow__any_of_long(long v, burrow__AnyBox *box) {
    return any_of_signed(v, sizeof v, box);
}

Any burrow__any_of_ulong(unsigned long v, burrow__AnyBox *box) {
    return any_of_unsigned(v, sizeof v, box);
}

Any burrow__any_of_llong(long long v, burrow__AnyBox *box) {
    return any_of_signed(v, sizeof v, box);
}

Any burrow__any_of_ullong(unsigned long long v, burrow__AnyBox *box) {
    return any_of_unsigned(v, sizeof v, box);
}

Any burrow__any_of_ptr(const volatile void *v, burrow__AnyBox *box) {
    box->v.p = v;
    return (Any){TYPE_UNSAFE_POINTER, (void *)&box->v.p};
}
