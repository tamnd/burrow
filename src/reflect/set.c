/* package reflect, the Value half that writes: the setters, New, Zero,
 * MakeSlice and the other makers, Append, Copy, Slice and the channel
 * operations. Go's reflect/value.go and map.go.
 *
 * A Value points at its bytes, so anything that makes a new value needs
 * somewhere to put it, and takes an Alloc for that. The rest write through the
 * pointer the Value already has.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "burrow/chan.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"

/* ------------------------------------------------------------------ helpers */

static Kind rfs_kind(ReflectValue v) {
    return v.typ == NULL ? KIND_INVALID : v.typ->kind;
}

static ReflectValue rfs_make(const Type *t, const void *p, uint32_t flag) {
    return (ReflectValue){t, (void *)(uintptr_t)p, flag};
}

static void *rfs_word(const void *p) {
    void *w;
    memcpy(&w, p, sizeof w);
    return w;
}

static void rfs_set_word(void *p, const void *w) {
    memcpy(p, &w, sizeof w);
}

static void rfs_must_be(ReflectValue v, Kind k, const char *method) {
    if (rfs_kind(v) != k)
        burrow__reflect_panic_kind(str_from_cstr(method), rfs_kind(v));
}

/* Go's mustBeExported: a Value that came out of an unexported field may not
 * be handed to anything that would let it escape. */
static void rfs_must_be_exported(ReflectValue v, const char *method) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(str_from_cstr(method), KIND_INVALID);
    if (v.flag & BURROW__RF_RO)
        burrow__reflect_panic(BURROW_S("reflect: "), str_from_cstr(method),
                              BURROW_S(" using value obtained using unexported field"));
}

/* Go's mustBeAssignable: exported, and reached through something that let it
 * be written. */
static void rfs_must_be_assignable(ReflectValue v, const char *method) {
    rfs_must_be_exported(v, method);
    if (!(v.flag & BURROW__RF_ADDR))
        burrow__reflect_panic(BURROW_S("reflect: "), str_from_cstr(method),
                              BURROW_S(" using unaddressable value"));
}

/* size bytes from a, zeroed, or a panic. Never NULL, even for size 0, since a
 * Value has to point somewhere. */
static void *rfs_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    return p;
}

/* A new word in a holding p, which is what a pointer, map or channel Value
 * points at. */
static void *rfs_new_word(Alloc *a, const void *p) {
    void *w = rfs_alloc(a, sizeof(void *), alignof(void *));
    rfs_set_word(w, p);
    return w;
}

static void rfs_store_uint(void *p, uint32_t size, uint64_t x) {
    switch (size) {
    case 1: {
        uint8_t v = (uint8_t)x;
        memcpy(p, &v, 1);
        return;
    }
    case 2: {
        uint16_t v = (uint16_t)x;
        memcpy(p, &v, 2);
        return;
    }
    case 4: {
        uint32_t v = (uint32_t)x;
        memcpy(p, &v, 4);
        return;
    }
    default:
        memcpy(p, &x, 8);
        return;
    }
}

const void *burrow__reflect_assign(ReflectValue x, const Type *dst, RfBox *box,
                                   const char *context) {
    if (burrow__reflect_assignable(dst, x.typ))
        return x.ptr;
    if (dst->kind == KIND_INTERFACE && burrow__reflect_implements(dst, x.typ)) {
        /* The empty interface is Any, the one interface whose first word is
         * a Type rather than a vtable. */
        if (dst == TYPE_ANY) {
            const Type *t = x.typ;
            void *p = x.ptr;
            if (t->kind == KIND_INTERFACE && !burrow__reflect_unwrap(&t, &p))
                box->any = (Any){NULL, NULL};
            else
                box->any = BURROW_ANY(t, p);
            return &box->any;
        }
        burrow__reflect_to_iface(dst, x.typ, x.ptr, &box->iface, context);
        return &box->iface;
    }
    Str a = burrow__reflect_concat(str_from_cstr(context), BURROW_S(": value of type "),
                                   reflect_type_string(x.typ));
    burrow__reflect_panic(a, BURROW_S(" is not assignable to type "),
                          reflect_type_string(dst));
}

/* Copies the t at src to dst, which may be the same place. */
static void rfs_copy(const Type *t, void *dst, const void *src) {
    if (dst != src)
        type_copy(t, dst, src);
}

/* ----------------------------------------------------------- Zero and New */

ReflectValue reflect_zero(ReflectType t) {
    if (t == NULL)
        panic_str(BURROW_S("reflect: Zero(nil)"));
    return rfs_make(t, burrow__reflect_zero(t->size), 0);
}

ReflectValue reflect_new(Alloc *a, ReflectType t) {
    if (t == NULL)
        panic_str(BURROW_S("reflect: New(nil)"));
    ReflectType pt = reflect_pointer_to(t);
    /* The value and the pointer to it in one block, the value first so that
     * it gets the alignment it needs. */
    size_t align = t->align > alignof(void *) ? t->align : alignof(void *);
    size_t at = ((size_t)t->size + alignof(void *) - 1) & ~(alignof(void *) - 1);
    Byte *p = rfs_alloc(a, at + sizeof(void *), align);
    rfs_set_word(p + at, p);
    return rfs_make(pt, p + at, 0);
}

ReflectValue reflect_new_at(Alloc *a, ReflectType t, void *p) {
    return rfs_make(reflect_pointer_to(t), rfs_new_word(a, p), 0);
}

ReflectValue reflect_value_addr(ReflectValue v, Alloc *a) {
    if (!(v.flag & BURROW__RF_ADDR))
        panic_str(BURROW_S("reflect.Value.Addr of unaddressable value"));
    return rfs_make(reflect_pointer_to(v.typ), rfs_new_word(a, v.ptr),
                    v.flag & BURROW__RF_RO);
}

/* ----------------------------------------------------------------- setters */

void reflect_value_set(ReflectValue v, ReflectValue x) {
    rfs_must_be_assignable(v, "reflect.Value.Set");
    rfs_must_be_exported(x, "reflect.Value.Set");
    RfBox box;
    const void *src = burrow__reflect_assign(x, v.typ, &box, "reflect.Set");
    rfs_copy(v.typ, v.ptr, src);
}

void reflect_value_set_bool(ReflectValue v, bool x) {
    rfs_must_be_assignable(v, "reflect.Value.SetBool");
    rfs_must_be(v, KIND_BOOL, "reflect.Value.SetBool");
    memcpy(v.ptr, &x, sizeof x);
}

void reflect_value_set_int(ReflectValue v, int64_t x) {
    rfs_must_be_assignable(v, "reflect.Value.SetInt");
    switch ((int)rfs_kind(v)) {
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        rfs_store_uint(v.ptr, v.typ->size, (uint64_t)x);
        return;
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.SetInt"), rfs_kind(v));
    }
}

void reflect_value_set_uint(ReflectValue v, uint64_t x) {
    rfs_must_be_assignable(v, "reflect.Value.SetUint");
    switch ((int)rfs_kind(v)) {
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        rfs_store_uint(v.ptr, v.typ->size, x);
        return;
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.SetUint"), rfs_kind(v));
    }
}

void reflect_value_set_float(ReflectValue v, double x) {
    rfs_must_be_assignable(v, "reflect.Value.SetFloat");
    switch ((int)rfs_kind(v)) {
    case KIND_FLOAT32: {
        float f = (float)x;
        memcpy(v.ptr, &f, sizeof f);
        return;
    }
    case KIND_FLOAT64:
        memcpy(v.ptr, &x, sizeof x);
        return;
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.SetFloat"), rfs_kind(v));
    }
}

void reflect_value_set_complex(ReflectValue v, Complex128 x) {
    rfs_must_be_assignable(v, "reflect.Value.SetComplex");
    switch ((int)rfs_kind(v)) {
    case KIND_COMPLEX64: {
        Complex64 c = {(float)x.re, (float)x.im};
        memcpy(v.ptr, &c, sizeof c);
        return;
    }
    case KIND_COMPLEX128:
        memcpy(v.ptr, &x, sizeof x);
        return;
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.SetComplex"), rfs_kind(v));
    }
}

void reflect_value_set_string(ReflectValue v, Str x) {
    rfs_must_be_assignable(v, "reflect.Value.SetString");
    rfs_must_be(v, KIND_STRING, "reflect.Value.SetString");
    memcpy(v.ptr, &x, sizeof x);
}

void reflect_value_set_bytes(ReflectValue v, Slice x) {
    rfs_must_be_assignable(v, "reflect.Value.SetBytes");
    rfs_must_be(v, KIND_SLICE, "reflect.Value.SetBytes");
    if (v.typ->elem->kind != KIND_UINT8)
        panic_str(BURROW_S("reflect.Value.SetBytes of non-byte slice"));
    /* The header names the element type of the slice it now belongs to,
     * which may be a named byte type. */
    x.elem = v.typ->elem;
    memcpy(v.ptr, &x, sizeof x);
}

void reflect_value_set_pointer(ReflectValue v, void *x) {
    rfs_must_be_assignable(v, "reflect.Value.SetPointer");
    rfs_must_be(v, KIND_UNSAFE_POINTER, "reflect.Value.SetPointer");
    rfs_set_word(v.ptr, x);
}

void reflect_value_set_len(ReflectValue v, Int n) {
    rfs_must_be_assignable(v, "reflect.Value.SetLen");
    rfs_must_be(v, KIND_SLICE, "reflect.Value.SetLen");
    Slice *s = v.ptr;
    if ((uint64_t)n > (uint64_t)s->cap)
        panic_str(BURROW_S("reflect: slice length out of range in SetLen"));
    s->len = n;
}

void reflect_value_set_cap(ReflectValue v, Int n) {
    rfs_must_be_assignable(v, "reflect.Value.SetCap");
    rfs_must_be(v, KIND_SLICE, "reflect.Value.SetCap");
    Slice *s = v.ptr;
    if (n < s->len || n > s->cap)
        panic_str(BURROW_S("reflect: slice capacity out of range in SetCap"));
    s->cap = n;
}

void reflect_value_set_zero(ReflectValue v) {
    rfs_must_be_assignable(v, "reflect.Value.SetZero");
    type_zero(v.typ, v.ptr);
}

/* -------------------------------------------------------------------- maps */

void reflect_value_set_map_index(ReflectValue v, ReflectValue key, ReflectValue elem) {
    rfs_must_be(v, KIND_MAP, "reflect.Value.SetMapIndex");
    rfs_must_be_exported(v, "reflect.Value.SetMapIndex");
    rfs_must_be_exported(key, "reflect.Value.SetMapIndex");
    RfBox kbox;
    const void *k =
        burrow__reflect_assign(key, v.typ->key, &kbox, "reflect.Value.SetMapIndex");
    Map *m = rfs_word(v.ptr);
    if (elem.typ == NULL) {
        if (m != NULL)
            map_del(m, k);
        return;
    }
    rfs_must_be_exported(elem, "reflect.Value.SetMapIndex");
    RfBox ebox;
    const void *e =
        burrow__reflect_assign(elem, v.typ->elem, &ebox, "reflect.Value.SetMapIndex");
    if (!map_set(m, k, e))
        panic_str(BURROW_S("reflect: out of memory"));
}

/* Where the iterator is, for SetIterKey and SetIterValue, which say which of
 * them was called when it is nowhere. */
static void rfs_iter_check(const ReflectMapIter *it, const char *method) {
    if (it->state == BURROW__RF_ITER_FRESH)
        burrow__reflect_panic(BURROW_S("reflect: Value."), str_from_cstr(method),
                              BURROW_S(" called before Next"));
    if (it->state == BURROW__RF_ITER_DONE)
        burrow__reflect_panic(BURROW_S("reflect: Value."), str_from_cstr(method),
                              BURROW_S(" called on exhausted iterator"));
}

void reflect_value_set_iter_key(ReflectValue v, ReflectMapIter *it) {
    rfs_iter_check(it, "SetIterKey");
    rfs_must_be_assignable(v, "reflect.Value.SetIterKey");
    rfs_must_be_exported(it->m, "reflect.Value.SetIterKey");
    ReflectValue k = rfs_make(it->m.typ->key, it->key, BURROW__RF_RO_OF(it->m));
    RfBox box;
    const void *src = burrow__reflect_assign(k, v.typ, &box, "reflect.MapIter.SetKey");
    rfs_copy(v.typ, v.ptr, src);
}

void reflect_value_set_iter_value(ReflectValue v, ReflectMapIter *it) {
    rfs_iter_check(it, "SetIterValue");
    rfs_must_be_assignable(v, "reflect.Value.SetIterValue");
    rfs_must_be_exported(it->m, "reflect.Value.SetIterValue");
    ReflectValue e = rfs_make(it->m.typ->elem, it->val, BURROW__RF_RO_OF(it->m));
    RfBox box;
    const void *src =
        burrow__reflect_assign(e, v.typ, &box, "reflect.MapIter.SetValue");
    rfs_copy(v.typ, v.ptr, src);
}

/* ------------------------------------------------------------------- slices */

/* Grows the slice at s, of slice type t, to hold n more elements past its
 * length, as Go's growslice does. */
static void rfs_grow(Alloc *a, const Type *t, Slice *s, Int n, const char *what) {
    if (n < 0)
        burrow__reflect_panic(str_from_cstr(what), BURROW_S(": negative len"),
                              BURROW_STR_EMPTY);
    if (s->len > BURROW_INT_MAX - n)
        burrow__reflect_panic(str_from_cstr(what), BURROW_S(": slice overflow"),
                              BURROW_STR_EMPTY);
    if (s->len + n <= s->cap)
        return;
    Slice in = *s;
    in.elem = t->elem;
    Slice out = slice_append(a, in, NULL, n);
    if (out.p == NULL && t->elem->size > 0)
        panic_str(BURROW_S("reflect: out of memory"));
    out.len = in.len;
    *s = out;
}

void reflect_value_grow(ReflectValue v, Alloc *a, Int n) {
    rfs_must_be_assignable(v, "reflect.Value.Grow");
    rfs_must_be(v, KIND_SLICE, "reflect.Value.Grow");
    rfs_grow(a, v.typ, v.ptr, n, "reflect.Value.Grow");
}

void reflect_value_clear(ReflectValue v) {
    switch ((int)rfs_kind(v)) {
    case KIND_SLICE: {
        const Slice *s = v.ptr;
        const Type *e = v.typ->elem;
        for (Int i = 0; i < s->len; i++)
            type_zero(e, (Byte *)s->p + (size_t)i * e->size);
        return;
    }
    case KIND_MAP: {
        Map *m = rfs_word(v.ptr);
        if (m != NULL)
            map_clear(m);
        return;
    }
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Clear"), rfs_kind(v));
    }
}

/* Slice and Slice3 once the bounds are known to be good: a header in a over
 * base[i:j:k], as a value of slice type t. */
static ReflectValue rfs_slice(Alloc *a, const Type *t, ReflectValue v, void *base,
                              Int i, Int j, Int k) {
    Slice *h = rfs_alloc(a, sizeof(Slice), alignof(Slice));
    const Type *e = t->elem;
    *h =
        (Slice){k - i > 0 ? (Byte *)base + (size_t)i * e->size : base, j - i, k - i, e};
    return rfs_make(t, h, BURROW__RF_RO_OF(v));
}

ReflectValue reflect_value_slice(ReflectValue v, Alloc *a, Int i, Int j) {
    const Type *t;
    void *base;
    Int cap;
    switch ((int)rfs_kind(v)) {
    case KIND_ARRAY:
        if (!(v.flag & BURROW__RF_ADDR))
            panic_str(BURROW_S("reflect.Value.Slice: slice of unaddressable array"));
        t = reflect_slice_of(v.typ->elem);
        base = v.ptr;
        cap = (Int)v.typ->len;
        break;
    case KIND_SLICE: {
        const Slice *s = v.ptr;
        t = v.typ;
        base = s->p;
        cap = s->cap;
        break;
    }
    case KIND_STRING: {
        const Str *s = v.ptr;
        if (i < 0 || j < i || j > s->len)
            panic_str(
                BURROW_S("reflect.Value.Slice: string slice index out of bounds"));
        Str *h = rfs_alloc(a, sizeof(Str), alignof(Str));
        if (i < s->len)
            *h = str_from_bytes(s->p + i, j - i);
        return rfs_make(v.typ, h, BURROW__RF_RO_OF(v));
    }
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Slice"), rfs_kind(v));
    }
    if (i < 0 || j < i || j > cap)
        panic_str(BURROW_S("reflect.Value.Slice: slice index out of bounds"));
    return rfs_slice(a, t, v, base, i, j, cap);
}

ReflectValue reflect_value_slice3(ReflectValue v, Alloc *a, Int i, Int j, Int k) {
    const Type *t;
    void *base;
    Int cap;
    switch ((int)rfs_kind(v)) {
    case KIND_ARRAY:
        if (!(v.flag & BURROW__RF_ADDR))
            panic_str(BURROW_S("reflect.Value.Slice3: slice of unaddressable array"));
        t = reflect_slice_of(v.typ->elem);
        base = v.ptr;
        cap = (Int)v.typ->len;
        break;
    case KIND_SLICE: {
        const Slice *s = v.ptr;
        t = v.typ;
        base = s->p;
        cap = s->cap;
        break;
    }
    default:
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Slice3"), rfs_kind(v));
    }
    if (i < 0 || j < i || k < j || k > cap)
        panic_str(BURROW_S("reflect.Value.Slice3: slice index out of bounds"));
    return rfs_slice(a, t, v, base, i, j, k);
}

ReflectValue reflect_make_slice(Alloc *a, ReflectType t, Int len, Int cap) {
    if (t == NULL || t->kind != KIND_SLICE)
        panic_str(BURROW_S("reflect.MakeSlice of non-slice type"));
    if (len < 0)
        panic_str(BURROW_S("reflect.MakeSlice: negative len"));
    if (cap < 0)
        panic_str(BURROW_S("reflect.MakeSlice: negative cap"));
    if (len > cap)
        panic_str(BURROW_S("reflect.MakeSlice: len > cap"));
    Slice s = slice_make(a, t->elem, len, cap);
    if (s.p == NULL && cap > 0 && t->elem->size > 0)
        panic_str(BURROW_S("reflect: out of memory"));
    Slice *h = rfs_alloc(a, sizeof(Slice), alignof(Slice));
    *h = s;
    return rfs_make(t, h, 0);
}

/* What Append and AppendSlice start from: a copy of the header of s, in a, with
 * room for n more elements and n more in its length. Go's extendSlice. */
static Slice *rfs_extend(Alloc *a, ReflectValue s, Int n) {
    rfs_must_be_exported(s, "unknown method");
    rfs_must_be(s, KIND_SLICE, "unknown method");
    Slice *h = rfs_alloc(a, sizeof(Slice), alignof(Slice));
    *h = *(const Slice *)s.ptr;
    rfs_grow(a, s.typ, h, n, "reflect.Value.Grow");
    h->len += n;
    return h;
}

ReflectValue reflect_append(Alloc *a, ReflectValue s, Slice x) {
    /* Go's mustBe here runs outside any Value method, so its ValueError names
     * no method. */
    rfs_must_be(s, KIND_SLICE, "unknown method");
    Int n = ((const Slice *)s.ptr)->len;
    Slice *h = rfs_extend(a, s, x.len);
    ReflectValue out = rfs_make(s.typ, h, 0);
    const ReflectValue *xs = x.p;
    const Type *e = s.typ->elem;
    for (Int i = 0; i < x.len; i++) {
        ReflectValue at =
            rfs_make(e, (Byte *)h->p + (size_t)(n + i) * e->size, BURROW__RF_ADDR);
        reflect_value_set(at, xs[i]);
    }
    return out;
}

ReflectValue reflect_append_slice(Alloc *a, ReflectValue s, ReflectValue t) {
    rfs_must_be(s, KIND_SLICE, "unknown method");
    rfs_must_be(t, KIND_SLICE, "unknown method");
    if (!burrow__reflect_identical(s.typ->elem, t.typ->elem)) {
        Str x =
            burrow__reflect_concat(BURROW_S("reflect.AppendSlice: "),
                                   reflect_type_string(s.typ->elem), BURROW_S(" != "));
        burrow__reflect_panic(x, reflect_type_string(t.typ->elem), BURROW_STR_EMPTY);
    }
    Int ns = ((const Slice *)s.ptr)->len;
    Int nt = ((const Slice *)t.ptr)->len;
    Slice *h = rfs_extend(a, s, nt);
    ReflectValue out = rfs_make(s.typ, h, 0);
    Slice tail = *h;
    /* A nil slice with nothing appended has no pointer to offset. */
    if (h->p != NULL)
        tail.p = (Byte *)h->p + (size_t)ns * s.typ->elem->size;
    tail.len = nt;
    tail.cap = h->cap - ns;
    reflect_copy(rfs_make(s.typ, &tail, 0), t);
    return out;
}

/* The elements of an array, slice or string Value as a slice header, for
 * Copy. */
static Slice rfs_elems(ReflectValue v) {
    switch ((int)rfs_kind(v)) {
    case KIND_ARRAY:
        return (Slice){v.ptr, (Int)v.typ->len, (Int)v.typ->len, v.typ->elem};
    case KIND_STRING: {
        const Str *s = v.ptr;
        return (Slice){(void *)(uintptr_t)s->p, s->len, s->len, TYPE_UINT8};
    }
    default:
        return *(const Slice *)v.ptr;
    }
}

Int reflect_copy(ReflectValue dst, ReflectValue src) {
    Kind dk = rfs_kind(dst);
    if (dk != KIND_ARRAY && dk != KIND_SLICE)
        burrow__reflect_panic_kind(BURROW_S("reflect.Copy"), dk);
    if (dk == KIND_ARRAY)
        rfs_must_be_assignable(dst, "unknown method");
    rfs_must_be_exported(dst, "unknown method");
    Kind sk = rfs_kind(src);
    bool string_copy = false;
    if (sk != KIND_ARRAY && sk != KIND_SLICE) {
        string_copy = sk == KIND_STRING && dst.typ->elem->kind == KIND_UINT8;
        if (!string_copy)
            burrow__reflect_panic_kind(BURROW_S("reflect.Copy"), sk);
    }
    rfs_must_be_exported(src, "unknown method");
    const Type *de = dst.typ->elem;
    if (!string_copy && !burrow__reflect_identical(de, src.typ->elem)) {
        Str x = burrow__reflect_concat(BURROW_S("reflect.Copy: "),
                                       reflect_type_string(de), BURROW_S(" != "));
        burrow__reflect_panic(x, reflect_type_string(src.typ->elem), BURROW_STR_EMPTY);
    }
    Slice ds = rfs_elems(dst);
    Slice ss = rfs_elems(src);
    Int n = ds.len < ss.len ? ds.len : ss.len;
    if (n > 0 && de->size > 0 && ds.p != NULL && ss.p != NULL)
        memmove(ds.p, ss.p, (size_t)n * de->size);
    return n;
}

/* ------------------------------------------------------- maps and channels */

ReflectValue reflect_slice_at(Alloc *a, ReflectType t, void *p, Int n) {
    if (n < 0)
        runtime_panic(BURROW_S("unsafe.Slice: len out of range"));
    if (t->size == 0 && p == NULL && n > 0)
        runtime_panic(BURROW_S("unsafe.Slice: ptr is nil and len is not zero"));
    /* Go's check that the n elements fit between p and the end of memory,
     * which a nil p never has room for. */
    uintptr_t room = (uintptr_t)0 - (uintptr_t)p;
    if (t->size > 0 && (uint64_t)n > (uint64_t)(room / t->size)) {
        if (p == NULL)
            runtime_panic(BURROW_S("unsafe.Slice: ptr is nil and len is not zero"));
        runtime_panic(BURROW_S("unsafe.Slice: len out of range"));
    }
    Slice *h = rfs_alloc(a, sizeof(Slice), alignof(Slice));
    *h = (Slice){p, n, n, t};
    return rfs_make(reflect_slice_of(t), h, 0);
}

ReflectValue reflect_make_map(Alloc *a, ReflectType t) {
    return reflect_make_map_with_size(a, t, 0);
}

ReflectValue reflect_make_map_with_size(Alloc *a, ReflectType t, Int n) {
    if (t == NULL || t->kind != KIND_MAP)
        panic_str(BURROW_S("reflect.MakeMapWithSize of non-map type"));
    Map *m = map_make(a, t->key, t->elem, n);
    if (m == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    return rfs_make(t, rfs_new_word(a, m), 0);
}

ReflectValue reflect_make_chan(Alloc *a, ReflectType t, Int buffer) {
    if (t == NULL || t->kind != KIND_CHAN)
        panic_str(BURROW_S("reflect.MakeChan of non-chan type"));
    if (buffer < 0)
        panic_str(BURROW_S("reflect.MakeChan: negative buffer size"));
    if (reflect_type_chan_dir(t) != REFLECT_BOTH_DIR)
        panic_str(BURROW_S("reflect.MakeChan: unidirectional channel type"));
    Chan *c = chan_make(a, t->elem, buffer);
    if (c == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    return rfs_make(t, rfs_new_word(a, c), 0);
}

static bool rfs_send(ReflectValue v, ReflectValue x, bool nb, const char *method) {
    rfs_must_be(v, KIND_CHAN, method);
    rfs_must_be_exported(v, method);
    if (!(reflect_type_chan_dir(v.typ) & REFLECT_SEND_DIR))
        panic_str(BURROW_S("reflect: send on recv-only channel"));
    rfs_must_be_exported(x, method);
    RfBox box;
    const void *p = burrow__reflect_assign(x, v.typ->elem, &box, "reflect.Value.Send");
    Chan *c = rfs_word(v.ptr);
    if (nb)
        return chan_try_send(c, p);
    chan_send(c, p);
    return true;
}

void reflect_value_send(ReflectValue v, ReflectValue x) {
    rfs_send(v, x, false, "reflect.Value.Send");
}

bool reflect_value_try_send(ReflectValue v, ReflectValue x) {
    return rfs_send(v, x, true, "reflect.Value.TrySend");
}

static ReflectValue rfs_recv(ReflectValue v, Alloc *a, bool *ok, bool nb,
                             const char *method) {
    rfs_must_be(v, KIND_CHAN, method);
    rfs_must_be_exported(v, method);
    if (!(reflect_type_chan_dir(v.typ) & REFLECT_RECV_DIR))
        panic_str(BURROW_S("reflect: recv on send-only channel"));
    const Type *e = v.typ->elem;
    void *out = rfs_alloc(a, e->size, e->align);
    Chan *c = rfs_word(v.ptr);
    bool got;
    if (nb) {
        if (!chan_try_recv(c, out, &got)) {
            *ok = false;
            return (ReflectValue){0};
        }
    } else {
        got = chan_recv(c, out);
    }
    *ok = got;
    return rfs_make(e, out, 0);
}

ReflectValue reflect_value_recv(ReflectValue v, Alloc *a, bool *ok) {
    return rfs_recv(v, a, ok, false, "reflect.Value.Recv");
}

ReflectValue reflect_value_try_recv(ReflectValue v, Alloc *a, bool *ok) {
    return rfs_recv(v, a, ok, true, "reflect.Value.TryRecv");
}

/* Select. The cases are checked first, as Go's select0 does, and only then is
 * anything made, so a case that panics leaves nothing behind. A received value
 * goes into one block of a big enough for any of the receive cases, since only
 * the case that runs writes to it. */
#define RFS_SELECT_STACK 16

static const Type *rfs_select_chan(ReflectValue ch, ReflectChanDir dir,
                                   const char *msg) {
    rfs_must_be(ch, KIND_CHAN, "unknown method");
    rfs_must_be_exported(ch, "unknown method");
    if (!(reflect_type_chan_dir(ch.typ) & dir))
        panic_str(str_from_cstr(msg));
    return ch.typ->elem;
}

/* Checks case i, and when cs is not NULL fills in cs[i], with box as the room
 * for a send's value. Returns the received element's type for a receive on a
 * channel, NULL otherwise. */
static const Type *rfs_select_case(const ReflectSelectCase *rc, SelectCase *cs,
                                   RfBox *box, bool *have_default) {
    SelectCase c = {.op = SELECT_DEFAULT};
    const Type *recv = NULL;
    switch (rc->dir) {
    case REFLECT_SELECT_DEFAULT:
        if (*have_default)
            panic_str(BURROW_S("reflect.Select: multiple default cases"));
        *have_default = true;
        if (rc->chan.typ != NULL)
            panic_str(BURROW_S("reflect.Select: default case has Chan value"));
        if (rc->send.typ != NULL)
            panic_str(BURROW_S("reflect.Select: default case has Send value"));
        break;
    case REFLECT_SELECT_SEND: {
        c.op = SELECT_SEND;
        if (rc->chan.typ == NULL)
            break;
        const Type *e =
            rfs_select_chan(rc->chan, REFLECT_SEND_DIR,
                            "reflect.Select: SendDir case using recv-only channel");
        if (rc->send.typ == NULL)
            panic_str(BURROW_S("reflect.Select: SendDir case missing Send value"));
        rfs_must_be_exported(rc->send, "unknown method");
        c.c = rfs_word(rc->chan.ptr);
        c.send = burrow__reflect_assign(rc->send, e, box, "reflect.Select");
        break;
    }
    case REFLECT_SELECT_RECV:
        c.op = SELECT_RECV;
        if (rc->send.typ != NULL)
            panic_str(BURROW_S("reflect.Select: RecvDir case has Send value"));
        if (rc->chan.typ == NULL)
            break;
        recv = rfs_select_chan(rc->chan, REFLECT_RECV_DIR,
                               "reflect.Select: RecvDir case using send-only channel");
        c.c = rfs_word(rc->chan.ptr);
        break;
    default:
        panic_str(BURROW_S("reflect.Select: invalid Dir"));
    }
    if (cs != NULL)
        *cs = c;
    return recv;
}

/* What reflect_select made in a for one call, given back when the call is
 * done with it, and also when the select panics on a closed channel. */
typedef struct RfsSelect {
    Alloc *a;
    Int n;
    SelectCase *cs;
    RfBox *boxes;
    void *buf;
    size_t size;
} RfsSelect;

static void rfs_select_free(const RfsSelect *s, bool buf) {
    if (s->n > RFS_SELECT_STACK) {
        mem_free(s->a, s->cs, (size_t)s->n * sizeof(SelectCase), alignof(SelectCase));
        mem_free(s->a, s->boxes, (size_t)s->n * sizeof(RfBox), alignof(RfBox));
    }
    if (buf)
        mem_free(s->a, s->buf, s->size == 0 ? 1 : s->size, BURROW_ALIGN_MAX);
}

static Int rfs_select_run(const RfsSelect *s) {
    volatile Int chosen = -1;
    BURROW_TRY {
        chosen = chan_select(s->cs, s->n);
    }
    BURROW_CATCH(r) {
        rfs_select_free(s, true);
        panic(r);
    }
    BURROW_TRY_END;
    return chosen;
}

Int reflect_select(Alloc *a, Slice cases, ReflectValue *recv, bool *recv_ok) {
    if (cases.len > 65536)
        panic_str(BURROW_S("reflect.Select: too many cases (max 65536)"));
    const ReflectSelectCase *rc = cases.p;
    RfsSelect s = {a, cases.len, NULL, NULL, NULL, 0};

    bool have_default = false;
    for (Int i = 0; i < s.n; i++) {
        RfBox box;
        const Type *e = rfs_select_case(&rc[i], NULL, &box, &have_default);
        if (e != NULL && e->size > s.size)
            s.size = e->size;
    }

    SelectCase stack_cases[RFS_SELECT_STACK];
    RfBox stack_boxes[RFS_SELECT_STACK];
    s.cs = stack_cases;
    s.boxes = stack_boxes;
    if (s.n > RFS_SELECT_STACK) {
        s.cs = rfs_alloc(a, (size_t)s.n * sizeof(SelectCase), alignof(SelectCase));
        s.boxes = rfs_alloc(a, (size_t)s.n * sizeof(RfBox), alignof(RfBox));
    }
    s.buf = rfs_alloc(a, s.size, BURROW_ALIGN_MAX);
    bool ok = false;
    have_default = false;
    for (Int i = 0; i < s.n; i++) {
        (void)rfs_select_case(&rc[i], &s.cs[i], &s.boxes[i], &have_default);
        if (s.cs[i].op == SELECT_RECV) {
            s.cs[i].recv = s.buf;
            s.cs[i].ok = &ok;
        }
    }

    Int chosen = rfs_select_run(&s);
    ReflectValue got = {0};
    bool is_recv = s.cs[chosen].op == SELECT_RECV;
    if (is_recv)
        got = rfs_make(rc[chosen].chan.typ->elem, s.buf, 0);
    else
        ok = false;
    rfs_select_free(&s, !is_recv);
    if (recv != NULL)
        *recv = got;
    if (recv_ok != NULL)
        *recv_ok = ok;
    return chosen;
}

void reflect_value_close(ReflectValue v) {
    rfs_must_be(v, KIND_CHAN, "reflect.Value.Close");
    rfs_must_be_exported(v, "reflect.Value.Close");
    if (!(reflect_type_chan_dir(v.typ) & REFLECT_SEND_DIR))
        panic_str(BURROW_S("reflect: close of receive-only channel"));
    chan_close(rfs_word(v.ptr));
}
