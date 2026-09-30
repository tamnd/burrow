/* Derived from Go's src/encoding/json/v2/arshal_funcs.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"

#include "json_internal.h"
#include "jsonv2_internal.h"

#include <stdarg.h>
#include <string.h>

/* Go wraps each function in a closure and caches, per type, the list of
 * closures that apply to it. Here the list is walked on every value, which
 * costs a scan of the functions for every value but needs no cache to keep
 * in step, and a set of functions is usually short. */

/* ------------------------------------------------------------------ the sets */

enum { JV_FN_MARSHAL, JV_FN_MARSHAL_TO, JV_FN_UNMARSHAL, JV_FN_UNMARSHAL_FROM };

typedef struct JvFunc {
    const Type *typ;
    int kind;
    union {
        Jsonv2MarshalFn marshal;
        Jsonv2MarshalToFn marshal_to;
        Jsonv2UnmarshalFn unmarshal;
        Jsonv2UnmarshalFromFn unmarshal_from;
    } fn;
    void *ctx;
} JvFunc;

/* typedArshalers. Both public types are this and nothing else, so the
 * option can hold either as a const void * and still be read as one. */
typedef struct JvFuncs {
    const JvFunc *fns;
    Int n;
    bool from_any;
} JvFuncs;

struct Jsonv2Marshalers {
    JvFuncs f;
};

struct Jsonv2Unmarshalers {
    JvFuncs f;
};

/* ---------------------------------------------------------------- castableTo */

/* An interface json knows the method of without being told. */
typedef struct JvKnownIface {
    const Type *t;
    Str method;
} JvKnownIface;

#define JV_KNOWN(cname, m) {&burrow_type_##cname, BURROW_S_INIT(m)}

static const JvKnownIface jv_known_ifaces[] = {
    JV_KNOWN(Error, "Error"),
    JV_KNOWN(Jsonv2Marshaler, "MarshalJSON"),
    JV_KNOWN(Jsonv2MarshalerTo, "MarshalJSONTo"),
    JV_KNOWN(Jsonv2Unmarshaler, "UnmarshalJSON"),
    JV_KNOWN(Jsonv2UnmarshalerFrom, "UnmarshalJSONFrom"),
    JV_KNOWN(EncodingBinaryMarshaler, "MarshalBinary"),
    JV_KNOWN(EncodingBinaryUnmarshaler, "UnmarshalBinary"),
    JV_KNOWN(EncodingBinaryAppender, "AppendBinary"),
    JV_KNOWN(EncodingTextMarshaler, "MarshalText"),
    JV_KNOWN(EncodingTextUnmarshaler, "UnmarshalText"),
    JV_KNOWN(EncodingTextAppender, "AppendText"),
};

/* reflect.PointerTo(from).Implements(to). Every burrow method takes a pointer
 * receiver, so *T has the methods T lists, while a pointer to a pointer or to
 * an interface has none. */
static bool jv_ptr_implements(const Type *from, const Type *to) {
    if (type_same(to, TYPE_ANY) || (to->name.len == 0 && to->nmethod == 0))
        return true;
    if (from->kind == KIND_POINTER || from->kind == KIND_INTERFACE)
        return false;
    if (to->nmethod > 0 && to->methods != NULL) {
        for (uint16_t i = 0; i < to->nmethod; i++)
            if (type_method_by_name(from, to->methods[i].name) == NULL)
                return false;
        return true;
    }
    for (size_t i = 0; i < sizeof(jv_known_ifaces) / sizeof(jv_known_ifaces[0]); i++)
        if (type_same(to, jv_known_ifaces[i].t))
            return type_method_by_name(from, jv_known_ifaces[i].method) != NULL;
    return false;
}

static bool jv_castable(const Type *from, const Type *to) {
    if (to->kind == KIND_INTERFACE)
        return jv_ptr_implements(from, to);
    if (to->kind == KIND_POINTER)
        return to->elem != NULL && type_same(to->elem, from);
    return type_same(from, to);
}

/* castableToFromAny: whether the function could want one of the values the
 * any fast paths make and write without asking. */
static bool jv_castable_from_any(const Type *to) {
    const Type *froms[] = {TYPE_ANY,
                           TYPE_BOOL,
                           TYPE_STRING,
                           TYPE_FLOAT64,
                           TYPE_JSONV2_MAP_STRING_ANY,
                           TYPE_JSONV2_SLICE_ANY};
    for (size_t i = 0; i < sizeof(froms) / sizeof(froms[0]); i++)
        if (jv_castable(froms[i], to))
            return true;
    return false;
}

/* assertCastableTo. */
static void jv_assert_castable(const Type *to, bool marshal) {
    if (to == NULL)
        panic_str(BURROW_S("json: input type is nil"));
    if (to->kind == KIND_INTERFACE)
        return;
    if (to->kind == KIND_POINTER) {
        if (to->name.len == 0)
            return;
    } else if (marshal) {
        return;
    }
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    jsonbuf_str(&b, BURROW_S("input type "));
    burrow__jsonv2_put_type(&b, to);
    jsonbuf_str(&b, marshal ? BURROW_S(" must be an interface type, an unnamed pointer "
                                       "type, or a non-pointer type")
                            : BURROW_S(" must be an interface type or an unnamed "
                                       "pointer type"));
    if (b.failed)
        panic_str(BURROW_S("input type is not castable"));
    panic_str(str_from_bytes(b.p, b.len));
}

/* ------------------------------------------------------------ constructors */

/* The set and its functions are two allocations, since they have to come
 * from a and a has no way to hand back one block with two alignments. */
static JvFuncs *jv_funcs_new(Alloc *a, size_t size, Int n) {
    JvFuncs *fs = (JvFuncs *)mem_alloc(a, size, _Alignof(JvFuncs));
    if (fs == NULL)
        return NULL;
    JvFunc *fns =
        (JvFunc *)mem_alloc_array(a, (size_t)n, sizeof(JvFunc), _Alignof(JvFunc));
    if (fns == NULL) {
        mem_free(a, fs, size, _Alignof(JvFuncs));
        return NULL;
    }
    fs->fns = fns;
    fs->n = n;
    fs->from_any = false;
    return fs;
}

static JvFuncs *jv_funcs_one(Alloc *a, size_t size, const JvFunc *f) {
    JvFuncs *fs = jv_funcs_new(a, size, 1);
    if (fs == NULL)
        return NULL;
    memcpy((JvFunc *)(uintptr_t)fs->fns, f, sizeof(*f));
    fs->from_any = jv_castable_from_any(f->typ);
    return fs;
}

BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_marshal_func(Alloc *a, const Type *t,
                                                       Jsonv2MarshalFn fn, void *ctx) {
    jv_assert_castable(t, true);
    JvFunc f = {t, JV_FN_MARSHAL, {.marshal = fn}, ctx};
    return (Jsonv2Marshalers *)jv_funcs_one(a, sizeof(Jsonv2Marshalers), &f);
}

BURROW_OWNS(ret) Jsonv2Marshalers *
jsonv2_marshal_to_func(Alloc *a, const Type *t, Jsonv2MarshalToFn fn, void *ctx) {
    jv_assert_castable(t, true);
    JvFunc f = {t, JV_FN_MARSHAL_TO, {.marshal_to = fn}, ctx};
    return (Jsonv2Marshalers *)jv_funcs_one(a, sizeof(Jsonv2Marshalers), &f);
}

BURROW_OWNS(ret) Jsonv2Unmarshalers *
jsonv2_unmarshal_func(Alloc *a, const Type *t, Jsonv2UnmarshalFn fn, void *ctx) {
    jv_assert_castable(t, false);
    JvFunc f = {t, JV_FN_UNMARSHAL, {.unmarshal = fn}, ctx};
    return (Jsonv2Unmarshalers *)jv_funcs_one(a, sizeof(Jsonv2Unmarshalers), &f);
}

BURROW_OWNS(ret) Jsonv2Unmarshalers *
jsonv2_unmarshal_from_func(Alloc *a, const Type *t, Jsonv2UnmarshalFromFn fn,
                           void *ctx) {
    jv_assert_castable(t, false);
    JvFunc f = {t, JV_FN_UNMARSHAL_FROM, {.unmarshal_from = fn}, ctx};
    return (Jsonv2Unmarshalers *)jv_funcs_one(a, sizeof(Jsonv2Unmarshalers), &f);
}

/* newTypedArshalers. Every public set starts with a JvFuncs, so the join is
 * the same for both. */
static JvFuncs *jv_join(Alloc *a, size_t size, const JvFuncs *const *sets, Int nsets) {
    Int n = 0;
    for (Int i = 0; i < nsets; i++)
        if (sets[i] != NULL)
            n += sets[i]->n;
    if (n == 0)
        return NULL;
    JvFuncs *fs = jv_funcs_new(a, size, n);
    if (fs == NULL)
        return NULL;
    JvFunc *dst = (JvFunc *)(uintptr_t)fs->fns;
    for (Int i = 0; i < nsets; i++) {
        if (sets[i] == NULL)
            continue;
        memcpy(dst, sets[i]->fns, (size_t)sets[i]->n * sizeof(JvFunc));
        dst += sets[i]->n;
        fs->from_any = fs->from_any || sets[i]->from_any;
    }
    return fs;
}

BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_join_marshalers(Alloc *a, Slice ms) {
    return (Jsonv2Marshalers *)jv_join(a, sizeof(Jsonv2Marshalers),
                                       (const JvFuncs *const *)ms.p, ms.len);
}

BURROW_OWNS(ret) Jsonv2Unmarshalers *jsonv2_join_unmarshalers(Alloc *a, Slice us) {
    return (Jsonv2Unmarshalers *)jv_join(a, sizeof(Jsonv2Unmarshalers),
                                         (const JvFuncs *const *)us.p, us.len);
}

/* The variadic joins, which gather their arguments on the stack in groups
 * so any count works without an allocation of its own. */
#define JV_JOIN_MAX 16

BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_join_marshalers_v(Alloc *a, int n, ...) {
    const JvFuncs *sets[JV_JOIN_MAX];
    const JvFuncs *acc = NULL;
    va_list ap;
    va_start(ap, n);
    int i = 0;
    while (i < n) {
        int k = 0;
        if (acc != NULL)
            sets[k++] = acc;
        while (k < JV_JOIN_MAX && i < n) {
            const Jsonv2Marshalers *m = va_arg(ap, const Jsonv2Marshalers *);
            sets[k++] = m != NULL ? &m->f : NULL;
            i++;
        }
        acc = jv_join(a, sizeof(Jsonv2Marshalers), sets, k);
    }
    va_end(ap);
    return (Jsonv2Marshalers *)(uintptr_t)acc;
}

BURROW_OWNS(ret) Jsonv2Unmarshalers *jsonv2_join_unmarshalers_v(Alloc *a, int n, ...) {
    const JvFuncs *sets[JV_JOIN_MAX];
    const JvFuncs *acc = NULL;
    va_list ap;
    va_start(ap, n);
    int i = 0;
    while (i < n) {
        int k = 0;
        if (acc != NULL)
            sets[k++] = acc;
        while (k < JV_JOIN_MAX && i < n) {
            const Jsonv2Unmarshalers *u = va_arg(ap, const Jsonv2Unmarshalers *);
            sets[k++] = u != NULL ? &u->f : NULL;
            i++;
        }
        acc = jv_join(a, sizeof(Jsonv2Unmarshalers), sets, k);
    }
    va_end(ap);
    return (Jsonv2Unmarshalers *)(uintptr_t)acc;
}

/* ----------------------------------------------------------------- options */

JsontextOptions jsonv2_with_marshalers(const Jsonv2Marshalers *m) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = JSONFLAG_MARSHALERS;
    o.values = JSONFLAG_MARSHALERS;
    o.marshalers = m;
    return o;
}

JsontextOptions jsonv2_with_unmarshalers(const Jsonv2Unmarshalers *u) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = JSONFLAG_UNMARSHALERS;
    o.values = JSONFLAG_UNMARSHALERS;
    o.unmarshalers = u;
    return o;
}

bool jsonv2_get_marshalers(JsontextOptions opts, const Jsonv2Marshalers **value) {
    bool ok = jsonflags_has(&opts, JSONFLAG_MARSHALERS);
    if (value != NULL)
        *value = ok ? (const Jsonv2Marshalers *)opts.marshalers : NULL;
    return ok;
}

bool jsonv2_get_unmarshalers(JsontextOptions opts, const Jsonv2Unmarshalers **value) {
    bool ok = jsonflags_has(&opts, JSONFLAG_UNMARSHALERS);
    if (value != NULL)
        *value = ok ? (const Jsonv2Unmarshalers *)opts.unmarshalers : NULL;
    return ok;
}

/* ------------------------------------------------------------------ lookup */

bool burrow__jsonv2_has_func(const void *fs, const Type *t) {
    const JvFuncs *f = (const JvFuncs *)fs;
    if (f == NULL)
        return false;
    for (Int i = 0; i < f->n; i++)
        if (jv_castable(t, f->fns[i].typ))
            return true;
    return false;
}

bool burrow__jsonv2_from_any(const void *fs) {
    return fs != NULL && ((const JvFuncs *)fs)->from_any;
}

/* One function and the value it is called on, as the ctx of the calls. */
typedef struct JvFuncCall {
    const JvFunc *f;
    Any v;
} JvFuncCall;

static Slice jv_marshal_call(const void *ctx, Alloc *a, Error *err) {
    const JvFuncCall *c = (const JvFuncCall *)ctx;
    return c->f->fn.marshal(c->f->ctx, a, c->v, err);
}

static Error jv_marshal_to_call(const void *ctx, JsontextEncoder *e) {
    const JvFuncCall *c = (const JvFuncCall *)ctx;
    return c->f->fn.marshal_to(c->f->ctx, e, c->v);
}

static Error jv_unmarshal_call(const void *ctx, Alloc *a, Slice data) {
    const JvFuncCall *c = (const JvFuncCall *)ctx;
    return c->f->fn.unmarshal(c->f->ctx, a, data, c->v);
}

static Error jv_unmarshal_from_call(const void *ctx, Alloc *a, JsontextDecoder *d) {
    const JvFuncCall *c = (const JvFuncCall *)ctx;
    return c->f->fn.unmarshal_from(c->f->ctx, a, d, c->v);
}

/* lookup and the closure it builds, in one. Go stops collecting at the first
 * function that cannot step aside, which comes to the same thing as calling
 * each in turn and stopping at the first that does not. */
Error burrow__jsonv2_marshal_funcs(JsontextEncoder *e, const Type *t, void *p,
                                   JsontextOptions *mo, bool *done) {
    const JvFuncs *fs = (const JvFuncs *)mo->marshalers;
    for (Int i = 0; i < fs->n; i++) {
        const JvFunc *f = &fs->fns[i];
        if (!jv_castable(t, f->typ))
            continue;
        JvFuncCall c = {f, BURROW_ANY(t, p)};
        if (f->kind == JV_FN_MARSHAL_TO) {
            bool skip = false;
            Error err = burrow__jsonv2_call_to(e, f->typ, t, "MarshalToFunc",
                                               jv_marshal_to_call, &c, &skip);
            if (skip)
                continue;
            *done = true;
            return err;
        }
        *done = true;
        return burrow__jsonv2_call_marshal(
            e, f->typ, t, mo, "marshal function of type func(T) ([]byte, error)",
            "MarshalFunc", jv_marshal_call, &c);
    }
    return BURROW_NO_ERROR;
}

Error burrow__jsonv2_unmarshal_funcs(JsontextDecoder *d, const Type *t, void *p,
                                     JsontextOptions *uo, bool *done) {
    const JvFuncs *fs = (const JvFuncs *)uo->unmarshalers;
    for (Int i = 0; i < fs->n; i++) {
        const JvFunc *f = &fs->fns[i];
        if (!jv_castable(t, f->typ))
            continue;
        JvFuncCall c = {f, BURROW_ANY(t, p)};
        if (f->kind == JV_FN_UNMARSHAL_FROM) {
            bool skip = false;
            Error err = burrow__jsonv2_call_from(d, f->typ, uo, jv_unmarshal_from_call,
                                                 &c, &skip);
            if (skip)
                continue;
            *done = true;
            return err;
        }
        *done = true;
        return burrow__jsonv2_call_unmarshal(
            d, f->typ, uo, "unmarshal function of type func([]byte, T) error",
            jv_unmarshal_call, &c);
    }
    return BURROW_NO_ERROR;
}
