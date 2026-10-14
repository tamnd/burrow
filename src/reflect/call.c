/* package reflect, calls: methods, Call and CallSlice, method values and
 * MakeFunc. Go's reflect/value.go and makefunc.go.
 *
 * A function value is a code pointer and an env, the layout every BURROW_FUNC
 * type has. A function reflect makes, which is a method value, a method's Func
 * or what MakeFunc returns, has rfn_marker as its code pointer and an RfnClosure
 * as its env, and Call goes through the closure. Any other function is called
 * through the call thunk its type's descriptor carries, which BURROW_FUNC_TYPE
 * makes. A type without one cannot be called through reflect.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"

/* ------------------------------------------------------------------ helpers */

static Kind rfn_kind(ReflectValue v) {
    return v.typ == NULL ? KIND_INVALID : v.typ->kind;
}

static void *rfn_word(const void *p) {
    void *w;
    memcpy(&w, p, sizeof w);
    return w;
}

/* size bytes from a, zeroed, never NULL. */
static void *rfn_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(BURROW_S("reflect: out of memory"));
    return p;
}

/* Go's mustBeExported, for the Value a call is made on and its arguments. */
static void rfn_must_be_exported(ReflectValue v, const char *method) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(str_from_cstr(method), KIND_INVALID);
    if (v.flag & BURROW__RF_RO)
        burrow__reflect_panic(BURROW_S("reflect: "), str_from_cstr(method),
                              BURROW_S(" using value obtained using unexported field"));
}

/* ----------------------------------------------------------------- closures */

typedef struct RfnClosure RfnClosure;

struct RfnClosure {
    void (*call)(const RfnClosure *c, void **args, void **rets);
};

/* The code pointer of every function reflect makes. There is no C function of
 * the right signature behind one, so a call from C rather than through reflect
 * lands here, whatever it passed. */
static void rfn_marker(void *env) {
    (void)env;
    panic_str(BURROW_S("reflect: a function made by reflect can only be called "
                       "through reflect"));
}

/* Calls the function value at fn, of type ft. */
static void rfn_invoke(const Type *ft, const void *fn, void **args, void **rets) {
    Func f;
    memcpy(&f, fn, sizeof f);
    if (f.f == rfn_marker) {
        const RfnClosure *c = f.env;
        c->call(c, args, rets);
        return;
    }
    if (ft->ops == NULL || ft->ops->call == NULL)
        burrow__reflect_panic(BURROW_S("reflect: call of "), reflect_type_string(ft),
                              BURROW_S(", whose descriptor has no call thunk"));
    ft->ops->call(fn, args, rets);
}

/* A method value: the method and the receiver it was taken from. iface says
 * the receiver is an interface value, whose method goes through its vtable. */
typedef struct RfnMethodValue {
    RfnClosure c;
    const Method *m;
    void *recv;
    bool iface;
    Func fn;
} RfnMethodValue;

static void rfn_method_value_call(const RfnClosure *c, void **args, void **rets) {
    const RfnMethodValue *mv = (const RfnMethodValue *)c;
    /* Go's methodReceiver, which checks these when the call is made rather
     * than when the method value is. */
    if (mv->iface) {
        if (!burrow__reflect_exported(mv->m->name))
            panic_str(BURROW_S("reflect: Call of unexported method"));
        Iface i;
        memcpy(&i, mv->recv, sizeof i);
        if (i.vt == NULL)
            panic_str(BURROW_S("reflect: Call of method on nil interface value"));
    }
    mv->m->thunk(mv->recv, args, rets);
}

/* A method's Func, which takes the receiver as its first argument. For a
 * pointer type borrowing the methods of what it points at, the argument is the
 * pointer, which is already what the thunk wants. */
typedef struct RfnMethodFunc {
    RfnClosure c;
    const Method *m;
    bool indirect;
    Func fn;
} RfnMethodFunc;

static void rfn_method_func_call(const RfnClosure *c, void **args, void **rets) {
    const RfnMethodFunc *mf = (const RfnMethodFunc *)c;
    void *recv = mf->indirect ? rfn_word(args[0]) : args[0];
    mf->m->thunk(recv, args + 1, rets);
}

/* Parameter i and result i of ft, for an i known to be in range. They read the
 * fields rather than going through type_in and type_out, whose NULL for a bad
 * index GCC sees once they are inlined into one file, and then warns about at
 * every use. */
static const Type *rfn_in(const Type *ft, Int i) {
    return ft->fields[i].type;
}

static const Type *rfn_out(const Type *ft, Int i) {
    return ft->fields[(Int)ft->len + i].type;
}

/* ------------------------------------------------------------------ methods */

/* Whether the method called name is one Method and NumMethod count: an
 * exported one, or any method of an interface, as in Go. */
static bool rfn_counts(const Type *t, Str name) {
    return t->kind == KIND_INTERFACE || burrow__reflect_exported(name);
}

/* The i'th counted method in t's method set, counting from zero, or NULL. */
static const Method *rfn_method(const Type *t, Int i) {
    uint16_t n;
    const Method *ms = burrow__reflect_method_set(t, &n);
    for (uint16_t j = 0; j < n; j++) {
        if (!rfn_counts(t, ms[j].name))
            continue;
        if (i-- == 0)
            return &ms[j];
    }
    return NULL;
}

/* Where the counted method called name is among t's counted methods, or -1. */
static Int rfn_method_index(const Type *t, Str name) {
    uint16_t n;
    const Method *ms = burrow__reflect_method_set(t, &n);
    Int k = 0;
    for (uint16_t j = 0; j < n; j++) {
        if (!rfn_counts(t, ms[j].name))
            continue;
        if (str_eq(ms[j].name, name))
            return k;
        k++;
    }
    return -1;
}

/* The type of a function taking first, if there is one, then the parameters of
 * ft, and returning what ft returns. Made by FuncOf, so asking twice gives one
 * descriptor. */
static const Type *rfn_func_type(const Type *first, const Type *ft) {
    Int nin = type_num_in(ft);
    Int nout = type_num_out(ft);
    Int skip = first != NULL ? 1 : 0;
    const Type *small[16];
    const Type **ts = small;
    Int n = skip + nin + nout;
    if (n > (Int)(sizeof small / sizeof small[0])) {
        ts =
            mem_alloc(heap_allocator(), (size_t)n * sizeof *ts, _Alignof(const Type *));
        if (ts == NULL)
            panic_str(BURROW_S("reflect: out of memory"));
    }
    if (first != NULL)
        ts[0] = first;
    for (Int i = 0; i < nin; i++)
        ts[skip + i] = rfn_in(ft, i);
    for (Int i = 0; i < nout; i++)
        ts[skip + nin + i] = rfn_out(ft, i);
    const Type *t = reflect_func_of(
        slice_from(ts, skip + nin, skip + nin, TYPE_UNSAFE_POINTER),
        slice_from(ts + skip + nin, nout, nout, TYPE_UNSAFE_POINTER), ft->elem != NULL);
    if (ts != small)
        mem_free(heap_allocator(), ts, (size_t)n * sizeof *ts, _Alignof(const Type *));
    return t;
}

typedef struct RfnMethodKey {
    const Method *m;
    bool indirect;
} RfnMethodKey;

static bool rfn_method_func_init(void *p, Alloc *a, const void *ctx) {
    (void)a;
    const RfnMethodKey *k = ctx;
    RfnMethodFunc *mf = p;
    mf->c.call = rfn_method_func_call;
    mf->m = k->m;
    mf->indirect = k->indirect;
    mf->fn = (Func){rfn_marker, &mf->c};
    return true;
}

/* Go's interfaceType.Method: the method with no receiver and no Func, and the
 * zero Method for an index out of range. An unexported one has the
 * interface's package path. */
static ReflectMethod rfn_iface_method(const Type *t, Int i) {
    const Method *m = i < 0 ? NULL : rfn_method(t, i);
    if (m == NULL)
        return (ReflectMethod){0};
    Str pkg = BURROW_STR_EMPTY;
    if (!burrow__reflect_exported(m->name))
        pkg = t->pkg_path.len > 0 ? t->pkg_path : BURROW_S("main");
    return (ReflectMethod){
        m->name, pkg, rfn_func_type(NULL, m->ftype), {NULL, NULL, 0}, i};
}

ReflectMethod reflect_type_method(ReflectType t, Int i) {
    if (t->kind == KIND_INTERFACE)
        return rfn_iface_method(t, i);
    const Method *m = i < 0 ? NULL : rfn_method(t, i);
    if (m == NULL)
        panic_str(BURROW_S("reflect: Method index out of range"));
    /* One Func per method and receiver type, kept, so that the Value can point
     * at it for as long as the program runs. */
    bool indirect = t->nmethod == 0;
    RfnMethodKey k = {m, indirect};
    const void *key[4] = {(const void *)BURROW__RF_KEEP_METHOD, m, t, NULL};
    RfnMethodFunc *mf = burrow__reflect_keep(
        key, sizeof(RfnMethodFunc), _Alignof(RfnMethodFunc), rfn_method_func_init, &k);
    const Type *ft = rfn_func_type(t, m->ftype);
    return (ReflectMethod){m->name, BURROW_STR_EMPTY, ft, {ft, &mf->fn, 0}, i};
}

bool reflect_type_method_by_name(ReflectType t, Str name, ReflectMethod *m) {
    Int i = rfn_method_index(t, name);
    if (i < 0)
        return false;
    *m = reflect_type_method(t, i);
    return true;
}

bool reflect_method_is_exported(ReflectMethod m) {
    return m.pkg_path.len == 0;
}

ReflectValue reflect_value_method(ReflectValue v, Alloc *a, Int i) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.Method"), KIND_INVALID);
    const Method *m = NULL;
    if (!(v.flag & BURROW__RF_METHOD) && i >= 0)
        m = rfn_method(v.typ, i);
    if (m == NULL)
        panic_str(BURROW_S("reflect: Method index out of range"));
    bool iface = v.typ->kind == KIND_INTERFACE;
    if (iface) {
        Iface x;
        memcpy(&x, v.ptr, sizeof x);
        if (x.vt == NULL)
            panic_str(BURROW_S("reflect: Method on nil interface value"));
    }
    RfnMethodValue *mv = rfn_alloc(a, sizeof *mv, _Alignof(RfnMethodValue));
    mv->c.call = rfn_method_value_call;
    mv->m = m;
    /* A method's own type answers the method set, and then the receiver is the
     * value, which for an interface is the interface value the method's thunk
     * calls through. Otherwise v is a pointer borrowing its element's
     * methods. */
    mv->recv = v.typ->nmethod > 0 ? v.ptr : rfn_word(v.ptr);
    mv->iface = iface;
    mv->fn = (Func){rfn_marker, &mv->c};
    return (ReflectValue){rfn_func_type(NULL, m->ftype), &mv->fn,
                          BURROW__RF_RO_OF(v) | BURROW__RF_METHOD};
}

ReflectValue reflect_value_method_by_name(ReflectValue v, Alloc *a, Str name) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.MethodByName"),
                                   KIND_INVALID);
    if (v.flag & BURROW__RF_METHOD)
        panic_str(BURROW_S("reflect: MethodByName of method value"));
    Int i = rfn_method_index(v.typ, name);
    if (i < 0)
        return (ReflectValue){0};
    return reflect_value_method(v, a, i);
}

Int reflect_value_num_method(ReflectValue v) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.Value.NumMethod"), KIND_INVALID);
    if (v.flag & BURROW__RF_METHOD)
        return 0;
    return reflect_type_num_method(v.typ);
}

/* --------------------------------------------------------------------- calls */

/* Where result i of ft goes in a block holding all of them, and the block's
 * size in *size. */
static size_t rfn_out_offset(const Type *ft, Int i, size_t *size) {
    size_t off = 0;
    size_t at = 0;
    for (Int j = 0; j < type_num_out(ft); j++) {
        const Type *t = rfn_out(ft, j);
        size_t al = t->align == 0 ? 1 : t->align;
        off = (off + al - 1) / al * al;
        if (j == i)
            at = off;
        off += t->size;
    }
    *size = off;
    return at;
}

static Slice rfn_call(ReflectValue v, Alloc *a, Slice in, bool is_slice) {
    const char *op = is_slice ? "CallSlice" : "Call";
    const char *method = is_slice ? "reflect.Value.CallSlice" : "reflect.Value.Call";
    if (rfn_kind(v) != KIND_FUNC)
        burrow__reflect_panic_kind(str_from_cstr(method), rfn_kind(v));
    rfn_must_be_exported(v, method);
    const Type *ft = v.typ;
    Func f;
    memcpy(&f, v.ptr, sizeof f);
    if (f.f == NULL)
        panic_str(BURROW_S("reflect.Value.Call: call of nil function"));

    const ReflectValue *xs = in.p;
    Int n = type_num_in(ft);
    bool variadic = ft->elem != NULL;
    if (is_slice) {
        if (!variadic)
            panic_str(BURROW_S("reflect: CallSlice of non-variadic function"));
        if (in.len < n)
            panic_str(BURROW_S("reflect: CallSlice with too few input arguments"));
        if (in.len > n)
            panic_str(BURROW_S("reflect: CallSlice with too many input arguments"));
    } else {
        if (variadic)
            n--;
        if (in.len < n)
            panic_str(BURROW_S("reflect: Call with too few input arguments"));
        if (!variadic && in.len > n)
            panic_str(BURROW_S("reflect: Call with too many input arguments"));
    }
    Str ops = str_from_cstr(op);
    for (Int i = 0; i < in.len; i++)
        if (xs[i].typ == NULL)
            burrow__reflect_panic(BURROW_S("reflect: "), ops,
                                  BURROW_S(" using zero Value argument"));
    for (Int i = 0; i < n; i++) {
        const Type *targ = rfn_in(ft, i);
        if (!reflect_type_assignable_to(xs[i].typ, targ)) {
            Str m =
                burrow__reflect_concat(BURROW_S("reflect: "), ops, BURROW_S(" using "));
            m = burrow__reflect_concat(m, reflect_type_string(xs[i].typ),
                                       BURROW_S(" as type "));
            burrow__reflect_panic(m, reflect_type_string(targ), BURROW_STR_EMPTY);
        }
    }

    Int nin = type_num_in(ft);
    void *argv_small[16];
    RfBox box_small[16];
    void **argv = argv_small;
    RfBox *box = box_small;
    if (nin > 16) {
        argv = rfn_alloc(a, (size_t)nin * sizeof(void *), _Alignof(void *));
        box = rfn_alloc(a, (size_t)nin * sizeof(RfBox), _Alignof(RfBox));
    }
    for (Int i = 0; i < n; i++) {
        rfn_must_be_exported(xs[i], method);
        argv[i] = (void *)(uintptr_t)burrow__reflect_assign(
            xs[i], rfn_in(ft, i), &box[i], "reflect.Value.Call");
    }
    Slice rest = {NULL, 0, 0, NULL};
    if (!is_slice && variadic) {
        const Type *st = rfn_in(ft, n);
        const Type *elem = st->elem;
        Int m = in.len - n;
        rest = (Slice){NULL, m, m, elem};
        if (m > 0)
            rest.p = rfn_alloc(a, (size_t)m * elem->size, elem->align);
        for (Int i = 0; i < m; i++) {
            ReflectValue x = xs[n + i];
            if (!reflect_type_assignable_to(x.typ, elem)) {
                Str s = burrow__reflect_concat(BURROW_S("reflect: cannot use "),
                                               reflect_type_string(x.typ),
                                               BURROW_S(" as type "));
                s = burrow__reflect_concat(s, reflect_type_string(elem),
                                           BURROW_S(" in "));
                burrow__reflect_panic(s, ops, BURROW_STR_EMPTY);
            }
            rfn_must_be_exported(x, method);
            RfBox b;
            const void *p = burrow__reflect_assign(x, elem, &b, "reflect.Set");
            type_copy(elem, (Byte *)rest.p + (size_t)i * elem->size, p);
        }
        argv[n] = &rest;
    }

    Int nout = type_num_out(ft);
    if (nout == 0) {
        rfn_invoke(ft, v.ptr, argv, NULL);
        return (Slice){NULL, 0, 0, TYPE_REFLECT_VALUE};
    }
    size_t size;
    rfn_out_offset(ft, 0, &size);
    Byte *block = rfn_alloc(a, size, _Alignof(max_align_t));
    ReflectValue *outs =
        rfn_alloc(a, (size_t)nout * sizeof(ReflectValue), _Alignof(ReflectValue));
    void *retv_small[16];
    void **retv = nout > 16
                      ? rfn_alloc(a, (size_t)nout * sizeof(void *), _Alignof(void *))
                      : retv_small;
    for (Int i = 0; i < nout; i++) {
        retv[i] = block + rfn_out_offset(ft, i, &size);
        outs[i] = (ReflectValue){rfn_out(ft, i), retv[i], 0};
    }
    rfn_invoke(ft, v.ptr, argv, retv);
    return (Slice){outs, nout, nout, TYPE_REFLECT_VALUE};
}

Slice reflect_value_call(ReflectValue v, Alloc *a, Slice in) {
    return rfn_call(v, a, in, false);
}

Slice reflect_value_call_slice(ReflectValue v, Alloc *a, Slice in) {
    return rfn_call(v, a, in, true);
}

/* ------------------------------------------------------------------ MakeFunc */

typedef struct RfnMakeFunc {
    RfnClosure c;
    const Type *ftype;
    ReflectMakeFuncImpl impl;
    Func fn;
} RfnMakeFunc;

BURROW_NORETURN static void rfn_bad_result(const char *what) {
    burrow__reflect_panic(
        BURROW_S("reflect: function created by MakeFunc using closure "),
        str_from_cstr(what), BURROW_STR_EMPTY);
}

static void rfn_make_func_call(const RfnClosure *c, void **args, void **rets) {
    const RfnMakeFunc *mf = (const RfnMakeFunc *)c;
    const Type *ft = mf->ftype;
    Int nin = type_num_in(ft);
    ReflectValue in_small[16];
    ReflectValue *in = in_small;
    if (nin > 16) {
        in = mem_alloc(heap_allocator(), (size_t)nin * sizeof *in,
                       _Alignof(ReflectValue));
        if (in == NULL)
            panic_str(BURROW_S("reflect: out of memory"));
    }
    for (Int i = 0; i < nin; i++)
        in[i] = (ReflectValue){rfn_in(ft, i), args[i], 0};
    /* The results may be the arguments handed back, as Go's own test does, so
     * the copy of them is freed only once the results are stored. A panic on
     * the way leaks it, which only a function of more than 16 parameters can
     * hit. */
    Slice out = BURROW_CALLF(mf->impl, slice_from(in, nin, nin, TYPE_REFLECT_VALUE));
    Int nout = type_num_out(ft);
    if (out.len != nout)
        panic_str(BURROW_S("reflect: wrong return count from function created by "
                           "MakeFunc"));
    const ReflectValue *vs = out.p;
    for (Int i = 0; i < nout; i++) {
        ReflectValue v = vs[i];
        const Type *t = rfn_out(ft, i);
        if (v.typ == NULL)
            rfn_bad_result("returned zero Value");
        if (v.flag & BURROW__RF_RO)
            rfn_bad_result("returned value obtained from unexported field");
        if (t->size == 0)
            continue;
        RfBox box;
        const void *p = burrow__reflect_assign(v, t, &box, "reflect.MakeFunc");
        type_copy(t, rets[i], p);
    }
    if (in != in_small)
        mem_free(heap_allocator(), in, (size_t)nin * sizeof *in,
                 _Alignof(ReflectValue));
}

ReflectValue reflect_make_func(Alloc *a, ReflectType t, ReflectMakeFuncImpl fn) {
    if (t == NULL || t->kind != KIND_FUNC)
        panic_str(BURROW_S("reflect: call of MakeFunc with non-Func type"));
    RfnMakeFunc *mf = rfn_alloc(a, sizeof *mf, _Alignof(RfnMakeFunc));
    mf->c.call = rfn_make_func_call;
    mf->ftype = t;
    mf->impl = fn;
    mf->fn = (Func){rfn_marker, &mf->c};
    return (ReflectValue){t, &mf->fn, 0};
}
