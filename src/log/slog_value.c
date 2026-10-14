/* log/slog: Kind, Value, Attr, Level, LevelVar, Source and Record.
 *
 * A Value is a tagged union here where Go packs a number and an any into two
 * words. The tag is Go's Kind, so everything that switches on Kind reads the
 * same, and nothing that Go keeps without allocating allocates here either.
 *
 * Go recovers from a LogValue method that panics and logs an error in its
 * place. That is a BURROW_TRY here, set up only when there is a LogValuer to
 * call, so resolving an ordinary value costs a comparison.
 *
 * Derived from Go's src/log/slog/value.go, attr.go, level.go and record.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log/slog.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "slog_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(burrow__SlogBox) >= sizeof(SlogAttr),
               "SlogAttr must fit the box");

/* ------------------------------------------------------------------- kind */

static const Str slogv_kind_strings[] = {
    BURROW_S_INIT("Any"),       BURROW_S_INIT("Bool"),   BURROW_S_INIT("Duration"),
    BURROW_S_INIT("Float64"),   BURROW_S_INIT("Int64"),  BURROW_S_INIT("String"),
    BURROW_S_INIT("Time"),      BURROW_S_INIT("Uint64"), BURROW_S_INIT("Group"),
    BURROW_S_INIT("LogValuer"),
};

Str slog_kind_string(SlogKind k) {
    if ((int)k >= 0 &&
        (size_t)k < sizeof slogv_kind_strings / sizeof slogv_kind_strings[0])
        return slogv_kind_strings[k];
    return BURROW_S("<unknown slog.Kind>");
}

static Str slogv_m_kind_string(SlogKind *self) {
    return slog_kind_string(*self);
}

#define SLOGV_SIG_STRING(IN, OUT) OUT(Str)
#define SLOGV_KIND_METHODS(M, T) M(T, String, slogv_m_kind_string, SLOGV_SIG_STRING)

BURROW_METHODS_DEFINE(SlogKind, SLOGV_KIND_METHODS);

const Type burrow_type_SlogKind = {
    BURROW_S_INIT("Kind"),
    BURROW_S_INIT("log/slog"),
    KIND_INT32,
    (uint32_t)sizeof(SlogKind),
    (uint16_t)_Alignof(SlogKind),
    0,
    (uint16_t)(sizeof(burrow__methods_SlogKind) / sizeof(burrow__methods_SlogKind[0])),
    NULL,
    burrow__methods_SlogKind,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------ constructors */

SlogValue slog_string_value(Str value) {
    SlogValue v = {SLOG_KIND_STRING, {0}};
    v.u.s = value;
    return v;
}

SlogValue slog_int_value(Int v) {
    return slog_int64_value((int64_t)v);
}

SlogValue slog_int64_value(int64_t v) {
    SlogValue r = {SLOG_KIND_INT64, {0}};
    r.u.i = v;
    return r;
}

SlogValue slog_uint64_value(uint64_t v) {
    SlogValue r = {SLOG_KIND_UINT64, {0}};
    r.u.u = v;
    return r;
}

SlogValue slog_float64_value(double v) {
    SlogValue r = {SLOG_KIND_FLOAT64, {0}};
    r.u.f = v;
    return r;
}

SlogValue slog_bool_value(bool v) {
    SlogValue r = {SLOG_KIND_BOOL, {0}};
    r.u.b = v;
    return r;
}

/* Go keeps the zero time as a nil location so that UnixNano is never asked
 * about it, and gives back time.Time{} for it. Anything else loses its
 * monotonic reading. */
SlogValue slog_time_value(Time v) {
    SlogValue r = {SLOG_KIND_TIME, {0}};
    r.u.t = time_is_zero(v) ? (Time){0} : time_round(v, 0);
    return r;
}

SlogValue slog_duration_value(Duration v) {
    SlogValue r = {SLOG_KIND_DURATION, {0}};
    r.u.d = v;
    return r;
}

bool burrow__slog_value_is_empty_group(SlogValue v) {
    return v.kind == SLOG_KIND_GROUP && v.u.group.len == 0;
}

Int burrow__slog_count_empty_groups(Slice as) {
    const SlogAttr *p = (const SlogAttr *)as.p;
    Int n = 0;
    for (Int i = 0; i < as.len; i++)
        if (burrow__slog_value_is_empty_group(p[i].value))
            n++;
    return n;
}

SlogValue slog_group_value(Slice as) {
    if (burrow__slog_count_empty_groups(as) > 0) {
        SlogAttr *p = (SlogAttr *)as.p;
        Int n = 0;
        for (Int i = 0; i < as.len; i++)
            if (!burrow__slog_value_is_empty_group(p[i].value))
                p[n++] = p[i];
        as.len = n;
    }
    SlogValue r = {SLOG_KIND_GROUP, {0}};
    r.u.group = as;
    return r;
}

/* What a LogValue method looks like: no arguments and one SlogValue back. */
static bool slogv_is_log_value_method(const Method *m) {
    return m != NULL && m->ftype != NULL && type_num_in(m->ftype) == 0 &&
           type_num_out(m->ftype) == 1 && type_out(m->ftype, 0) == TYPE_SLOG_VALUE;
}

/* The LogValue method of the value in a, and what to call it on. A pointer's
 * methods are its element's, called on what it points at, and an error's are
 * those of the type inside it, the way fmt finds a String method. */
static const Method *slogv_log_value_method(Any a, void **recv) {
    const Type *t = a.t;
    void *d = a.data;
    if (t == NULL)
        return NULL;
    if (t == TYPE_ERROR) {
        const Error *e = (const Error *)d;
        if (e->vt == NULL || e->vt->self_type == NULL)
            return NULL;
        t = e->vt->self_type;
        d = (void *)(uintptr_t)e->data;
    } else if (t->nmethod == 0 && t->kind == KIND_POINTER && t->elem != NULL) {
        t = t->elem;
        d = *(void **)d;
    }
    if (t->nmethod == 0)
        return NULL;
    const Method *m = type_method_by_name(t, BURROW_S("LogValue"));
    if (!slogv_is_log_value_method(m))
        return NULL;
    *recv = d;
    return m;
}

static bool slogv_is_log_valuer(Any a) {
    void *recv = NULL;
    return a.t == TYPE_SLOG_LOG_VALUER || slogv_log_value_method(a, &recv) != NULL;
}

/* A Slice of SlogAttr, for Value.Any to hand a group back as. */
static const Type slogv_attrs_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_SlogAttr,
    NULL,
    0,
    0,
    NULL,
};

static SlogValue slogv_kind_any(Any v) {
    SlogValue r = {SLOG_KIND_ANY, {0}};
    r.u.any = v;
    return r;
}

SlogValue slog_any_value(Any v) {
    const Type *t = v.t;
    const void *d = v.data;
    if (t == NULL || d == NULL)
        return slogv_kind_any(v);
    if (t == TYPE_STRING)
        return slog_string_value(*(const Str *)d);
    if (t == TYPE_INT)
        return slog_int64_value((int64_t)*(const Int *)d);
    if (t == TYPE_INT8)
        return slog_int64_value(*(const int8_t *)d);
    if (t == TYPE_INT16)
        return slog_int64_value(*(const int16_t *)d);
    if (t == TYPE_INT32)
        return slog_int64_value(*(const int32_t *)d);
    if (t == TYPE_INT64)
        return slog_int64_value(*(const int64_t *)d);
    if (t == TYPE_UINT)
        return slog_uint64_value((uint64_t)*(const Uint *)d);
    if (t == TYPE_UINT8)
        return slog_uint64_value(*(const uint8_t *)d);
    if (t == TYPE_UINT16)
        return slog_uint64_value(*(const uint16_t *)d);
    if (t == TYPE_UINT32)
        return slog_uint64_value(*(const uint32_t *)d);
    if (t == TYPE_UINT64)
        return slog_uint64_value(*(const uint64_t *)d);
    if (t == TYPE_UINTPTR)
        return slog_uint64_value((uint64_t)*(const Uintptr *)d);
    if (t == TYPE_FLOAT32)
        return slog_float64_value((double)*(const float *)d);
    if (t == TYPE_FLOAT64)
        return slog_float64_value(*(const double *)d);
    if (t == TYPE_BOOL)
        return slog_bool_value(*(const bool *)d);
    if (t == TYPE_DURATION)
        return slog_duration_value(*(const Duration *)d);
    if (t == TYPE_TIME)
        return slog_time_value(*(const Time *)d);
    if (t == TYPE_SLOG_VALUE)
        return *(const SlogValue *)d;
    if (t->kind == KIND_SLICE && t->elem == TYPE_SLOG_ATTR)
        return slog_group_value(*(const Slice *)d);
    if (slogv_is_log_valuer(v)) {
        SlogValue r = {SLOG_KIND_LOG_VALUER, {0}};
        r.u.any = v;
        return r;
    }
    return slogv_kind_any(v);
}

/* --------------------------------------------------------------- accessors */

SlogKind slog_value_kind(SlogValue v) {
    return v.kind;
}

BURROW_NORETURN static void slogv_bad_kind(SlogKind k) {
    panic_str(fmt_sprintf_v(error_allocator(), "bad kind: %s", slog_kind_string(k)));
}

Any slog_value_any(const SlogValue *v) {
    switch (v->kind) {
    case SLOG_KIND_ANY:
    case SLOG_KIND_LOG_VALUER:
        return v->u.any;
    case SLOG_KIND_GROUP:
        return (Any){&slogv_attrs_type, (void *)(uintptr_t)&v->u.group};
    case SLOG_KIND_INT64:
        return (Any){TYPE_INT64, (void *)(uintptr_t)&v->u.i};
    case SLOG_KIND_UINT64:
        return (Any){TYPE_UINT64, (void *)(uintptr_t)&v->u.u};
    case SLOG_KIND_FLOAT64:
        return (Any){TYPE_FLOAT64, (void *)(uintptr_t)&v->u.f};
    case SLOG_KIND_STRING:
        return (Any){TYPE_STRING, (void *)(uintptr_t)&v->u.s};
    case SLOG_KIND_BOOL:
        return (Any){TYPE_BOOL, (void *)(uintptr_t)&v->u.b};
    case SLOG_KIND_DURATION:
        return (Any){TYPE_DURATION, (void *)(uintptr_t)&v->u.d};
    case SLOG_KIND_TIME:
        return (Any){TYPE_TIME, (void *)(uintptr_t)&v->u.t};
    default:
        slogv_bad_kind(v->kind);
    }
}

BURROW_NORETURN static void slogv_wrong_kind(SlogKind got, SlogKind want) {
    panic_str(fmt_sprintf_v(error_allocator(), "Value kind is %s, not %s",
                            slog_kind_string(got), slog_kind_string(want)));
}

int64_t slog_value_int64(SlogValue v) {
    if (v.kind != SLOG_KIND_INT64)
        slogv_wrong_kind(v.kind, SLOG_KIND_INT64);
    return v.u.i;
}

uint64_t slog_value_uint64(SlogValue v) {
    if (v.kind != SLOG_KIND_UINT64)
        slogv_wrong_kind(v.kind, SLOG_KIND_UINT64);
    return v.u.u;
}

double slog_value_float64(SlogValue v) {
    if (v.kind != SLOG_KIND_FLOAT64)
        slogv_wrong_kind(v.kind, SLOG_KIND_FLOAT64);
    return v.u.f;
}

bool slog_value_bool(SlogValue v) {
    if (v.kind != SLOG_KIND_BOOL)
        slogv_wrong_kind(v.kind, SLOG_KIND_BOOL);
    return v.u.b;
}

Duration slog_value_duration(SlogValue v) {
    if (v.kind != SLOG_KIND_DURATION)
        slogv_wrong_kind(v.kind, SLOG_KIND_DURATION);
    return v.u.d;
}

Time slog_value_time(SlogValue v) {
    if (v.kind != SLOG_KIND_TIME)
        slogv_wrong_kind(v.kind, SLOG_KIND_TIME);
    return v.u.t;
}

Slice slog_value_group(SlogValue v) {
    if (v.kind != SLOG_KIND_GROUP)
        panic_str(BURROW_S("Group: bad kind"));
    return v.u.group;
}

/* Go's v.any.(LogValuer), whose failure is a type assertion panic. */
Any slog_value_log_valuer(SlogValue v) {
    if (v.kind != SLOG_KIND_LOG_VALUER)
        panic_str(
            fmt_sprintf_v(error_allocator(),
                          "interface conversion: interface is not slog.LogValuer"));
    return v.u.any;
}

/* ----------------------------------------------------------------- string */

Slice burrow__slog_value_append(SlogValue v, Alloc *a, Slice dst) {
    switch (v.kind) {
    case SLOG_KIND_STRING:
        return slice_append(a, dst, v.u.s.p, v.u.s.len);
    case SLOG_KIND_INT64:
        return strconv_append_int(a, dst, v.u.i, 10);
    case SLOG_KIND_UINT64:
        return strconv_append_uint(a, dst, v.u.u, 10);
    case SLOG_KIND_FLOAT64:
        return strconv_append_float(a, dst, v.u.f, 'g', -1, 64);
    case SLOG_KIND_BOOL:
        return strconv_append_bool(a, dst, v.u.b);
    case SLOG_KIND_DURATION: {
        Str s = duration_string(v.u.d, a);
        return slice_append(a, dst, s.p, s.len);
    }
    case SLOG_KIND_TIME:
        return time_append_format(v.u.t, a, dst,
                                  BURROW_S("2006-01-02 15:04:05.999999999 -0700 MST"));
    case SLOG_KIND_GROUP:
    case SLOG_KIND_ANY:
    case SLOG_KIND_LOG_VALUER: {
        Any x = slog_value_any(&v);
        return fmt_append(a, dst, slice_from(&x, 1, 1, TYPE_ANY));
    }
    default:
        slogv_bad_kind(v.kind);
    }
}

Str slog_value_string(SlogValue v, Alloc *a) {
    if (v.kind == SLOG_KIND_STRING)
        return v.u.s;
    Slice b = burrow__slog_value_append(v, a, slice_nil(TYPE_BYTE));
    return str_from_bytes(b.p, b.len);
}

static Str slogv_m_value_string(SlogValue *self) {
    return slog_value_string(*self, error_allocator());
}

/* ------------------------------------------------------------------ equal */

static bool slogv_any_equal(Any a, Any b) {
    if (a.t == TYPE_SLOG_LOG_VALUER && b.t == TYPE_SLOG_LOG_VALUER) {
        const SlogLogValuer *x = (const SlogLogValuer *)a.data;
        const SlogLogValuer *y = (const SlogLogValuer *)b.data;
        return x->vt == y->vt && x->data == y->data;
    }
    return any_equal(a, b);
}

bool slog_value_equal(SlogValue v, SlogValue w) {
    if (v.kind != w.kind)
        return false;
    switch (v.kind) {
    case SLOG_KIND_INT64:
        return v.u.i == w.u.i;
    case SLOG_KIND_UINT64:
        return v.u.u == w.u.u;
    case SLOG_KIND_BOOL:
        return v.u.b == w.u.b;
    case SLOG_KIND_DURATION:
        return v.u.d == w.u.d;
    case SLOG_KIND_STRING:
        return str_eq(v.u.s, w.u.s);
    case SLOG_KIND_FLOAT64:
        return v.u.f == w.u.f;
    case SLOG_KIND_TIME:
        return time_equal(v.u.t, w.u.t);
    case SLOG_KIND_ANY:
    case SLOG_KIND_LOG_VALUER:
        return slogv_any_equal(v.u.any, w.u.any);
    case SLOG_KIND_GROUP: {
        if (v.u.group.len != w.u.group.len)
            return false;
        const SlogAttr *x = (const SlogAttr *)v.u.group.p;
        const SlogAttr *y = (const SlogAttr *)w.u.group.p;
        for (Int i = 0; i < v.u.group.len; i++)
            if (!slog_attr_equal(x[i], y[i]))
                return false;
        return true;
    }
    default:
        slogv_bad_kind(v.kind);
    }
}

/* ---------------------------------------------------------------- resolve */

enum { SLOGV_MAX_LOG_VALUES = 100 };

static SlogValue slogv_call_log_value(Any a) {
    if (a.t == TYPE_SLOG_LOG_VALUER) {
        const SlogLogValuer *lv = (const SlogLogValuer *)a.data;
        return lv->vt->log_value(lv->data);
    }
    void *recv = NULL;
    const Method *m = slogv_log_value_method(a, &recv);
    SlogValue out = {SLOG_KIND_ANY, {0}};
    void *rets[1] = {&out};
    (void)method_call(m, recv, NULL, rets);
    return out;
}

/* True when v came out as something other than a LogValuer. */
static bool slogv_resolve_loop(SlogValue *v) {
    for (int i = 0; i < SLOGV_MAX_LOG_VALUES; i++) {
        if (v->kind != SLOG_KIND_LOG_VALUER)
            return true;
        *v = slogv_call_log_value(v->u.any);
    }
    return false;
}

/* Go's stack(skip, nFrames), from the frame that called this. Go's starts at
 * the LogValue that panicked, which it can see because it runs inside the
 * panic. The catch block here runs after the unwinding, so the deepest frame
 * left is the one that caught it. */
BURROW_NOINLINE static Str slogv_stack(Alloc *a, int nframes) {
    Uintptr pcs[8];
    Int n = runtime_callers(2, slice_from(pcs, nframes + 1, nframes + 1, TYPE_UINTPTR));
    if (n == 0)
        return BURROW_S("(no stack)");
    RuntimeFrames it = runtime_callers_frames(slice_from(pcs, n, n, TYPE_UINTPTR));
    RuntimeFrame f;
    Slice b = slice_nil(TYPE_BYTE);
    int i = 0;
    while (runtime_frames_next(&it, &f)) {
        b = fmt_appendf_v(a, b, "called from %s (%s:%d)\n", f.function, f.file, f.line);
        i++;
        if (it.at < it.len && i >= nframes) {
            b = fmt_appendf_v(a, b, "(rest of stack elided)\n");
            break;
        }
    }
    return str_from_bytes(b.p, b.len);
}

/* An error as a Value. The Error has to outlive the call, so it goes in the
 * goroutine's error arena with its message. */
static SlogValue slogv_error_value(Error err) {
    Error *box = mem_alloc(error_allocator(), sizeof *box, alignof(Error));
    if (box == NULL)
        return slog_string_value(error_text(err));
    *box = err;
    return slog_any_value((Any){TYPE_ERROR, box});
}

/* 0 when *v is resolved, 1 when it is still a LogValuer after the limit and 2
 * when a LogValue panicked. A function of its own so that what the try block
 * changes lives in the caller's frame and not in the one setjmp saved. */
BURROW_NOINLINE static int slogv_resolve_guarded(SlogValue *v) {
    volatile int r = 0;
    BURROW_TRY {
        r = slogv_resolve_loop(v) ? 0 : 1;
    }
    BURROW_CATCH(p) {
        (void)p;
        r = 2;
    }
    BURROW_TRY_END;
    return r;
}

SlogValue slog_value_resolve(SlogValue v) {
    if (v.kind != SLOG_KIND_LOG_VALUER)
        return v;
    SlogValue orig = v;
    switch (slogv_resolve_guarded(&v)) {
    case 0:
        return v;
    case 1: {
        Any o = slog_value_any(&orig);
        return slogv_error_value(
            fmt_errorf_v("LogValue called too many times on Value of type %T", o));
    }
    default: {
        Str st = slogv_stack(error_allocator(), 5);
        return slogv_error_value(fmt_errorf_v("LogValue panicked\n%s", st));
    }
    }
}

/* ------------------------------------------------------------- log valuer */

static bool slogv_lv_equal(const void *a, const void *b) {
    const SlogLogValuer *x = (const SlogLogValuer *)a;
    const SlogLogValuer *y = (const SlogLogValuer *)b;
    return x->vt == y->vt && x->data == y->data;
}

static uint64_t slogv_lv_hash(const void *p, uint64_t seed) {
    const SlogLogValuer *x = (const SlogLogValuer *)p;
    uint64_t h = seed ^ 0x9e3779b97f4a7c15U;
    h = (h ^ (uint64_t)(Uintptr)x->vt) * 0x100000001b3U;
    h = (h ^ (uint64_t)(Uintptr)x->data) * 0x100000001b3U;
    return h;
}

static const TypeOps slogv_lv_ops = {slogv_lv_equal, slogv_lv_hash, NULL, NULL, NULL};

const Type burrow_type_SlogLogValuer = {
    BURROW_S_INIT("LogValuer"),
    BURROW_S_INIT("log/slog"),
    KIND_INTERFACE,
    (uint32_t)sizeof(SlogLogValuer),
    (uint16_t)_Alignof(SlogLogValuer),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    &slogv_lv_ops,
};

/* ------------------------------------------------------------------- attr */

SlogAttr slog_string(Str key, Str value) {
    return (SlogAttr){key, slog_string_value(value)};
}

SlogAttr slog_int64(Str key, int64_t value) {
    return (SlogAttr){key, slog_int64_value(value)};
}

SlogAttr slog_int(Str key, Int value) {
    return (SlogAttr){key, slog_int_value(value)};
}

SlogAttr slog_uint64(Str key, uint64_t v) {
    return (SlogAttr){key, slog_uint64_value(v)};
}

SlogAttr slog_float64(Str key, double v) {
    return (SlogAttr){key, slog_float64_value(v)};
}

SlogAttr slog_bool(Str key, bool v) {
    return (SlogAttr){key, slog_bool_value(v)};
}

SlogAttr slog_time(Str key, Time v) {
    return (SlogAttr){key, slog_time_value(v)};
}

SlogAttr slog_duration(Str key, Duration v) {
    return (SlogAttr){key, slog_duration_value(v)};
}

SlogAttr slog_any(Str key, Any value) {
    return (SlogAttr){key, slog_any_value(value)};
}

SlogAttr slog_group_attrs(Str key, Slice attrs) {
    return (SlogAttr){key, slog_group_value(attrs)};
}

SlogAttr burrow__slog_args_to_attr(Slice args, Int *i) {
    const Any *p = (const Any *)args.p;
    Any x = p[*i];
    if (x.t == TYPE_STRING && x.data != NULL) {
        Str key = *(const Str *)x.data;
        if (*i + 1 >= args.len) {
            *i += 1;
            return slog_string(BURROW__SLOG_BAD_KEY, key);
        }
        *i += 2;
        return slog_any(key, p[*i - 1]);
    }
    *i += 1;
    if (x.t == TYPE_SLOG_ATTR && x.data != NULL)
        return *(const SlogAttr *)x.data;
    return slog_any(BURROW__SLOG_BAD_KEY, x);
}

Int burrow__slog_count_attrs(Slice args) {
    const Any *p = (const Any *)args.p;
    Int n = 0;
    for (Int i = 0; i < args.len; i++) {
        n++;
        if (p[i].t == TYPE_STRING)
            i++;
    }
    return n;
}

Slice burrow__slog_args_to_attrs(Alloc *a, Slice args) {
    Int n = burrow__slog_count_attrs(args);
    if (n == 0)
        return slice_nil(TYPE_SLOG_ATTR);
    Slice out = slice_make(a, TYPE_SLOG_ATTR, n, n);
    if (out.p == NULL)
        return slice_nil(TYPE_SLOG_ATTR);
    SlogAttr *p = (SlogAttr *)out.p;
    Int i = 0;
    Int k = 0;
    while (i < args.len && k < n)
        p[k++] = burrow__slog_args_to_attr(args, &i);
    out.len = k;
    return out;
}

SlogAttr slog_group(Alloc *a, Str key, Slice args) {
    return slog_group_attrs(key, burrow__slog_args_to_attrs(a, args));
}

SlogAttr burrow__slog_group(Str key, Slice args, SlogAttr *scratch) {
    Int i = 0;
    Int k = 0;
    while (i < args.len)
        scratch[k++] = burrow__slog_args_to_attr(args, &i);
    return slog_group_attrs(key, slice_from(scratch, k, k, TYPE_SLOG_ATTR));
}

bool slog_attr_equal(SlogAttr a, SlogAttr b) {
    return str_eq(a.key, b.key) && slog_value_equal(a.value, b.value);
}

Str slog_attr_string(SlogAttr a, Alloc *al) {
    Slice b = slice_append(al, slice_nil(TYPE_BYTE), a.key.p, a.key.len);
    b = slice_append(al, b, "=", 1);
    b = burrow__slog_value_append(a.value, al, b);
    return str_from_bytes(b.p, b.len);
}

/* Go's test is on the representation: no key, no number and no any. The zero
 * SlogValue is SLOG_KIND_ANY holding nothing, which is the same thing. */
bool burrow__slog_attr_is_empty(SlogAttr a) {
    return a.key.len == 0 && a.value.kind == SLOG_KIND_ANY && a.value.u.any.t == NULL &&
           a.value.u.any.data == NULL;
}

Any burrow__slog_any_of_attr(SlogAttr v, burrow__AnyBox *box) {
    burrow__SlogBox *b = (burrow__SlogBox *)(void *)box;
    b->attr = v;
    return (Any){TYPE_SLOG_ATTR, &b->attr};
}

Any burrow__slog_any_of_value(SlogValue v, burrow__AnyBox *box) {
    burrow__SlogBox *b = (burrow__SlogBox *)(void *)box;
    b->value = v;
    return (Any){TYPE_SLOG_VALUE, &b->value};
}

static Str slogv_m_attr_string(SlogAttr *self) {
    return slog_attr_string(*self, error_allocator());
}

static bool slogv_m_attr_equal(SlogAttr *self, SlogAttr b) {
    return slog_attr_equal(*self, b);
}

#define SLOGV_SIG_ATTR_EQUAL(IN, OUT) IN(0, SlogAttr) OUT(bool)
#define SLOGV_ATTR_METHODS(M, T)                                                       \
    M(T, Equal, slogv_m_attr_equal, SLOGV_SIG_ATTR_EQUAL)                              \
    M(T, String, slogv_m_attr_string, SLOGV_SIG_STRING)
#define SLOGV_VALUE_METHODS(M, T) M(T, String, slogv_m_value_string, SLOGV_SIG_STRING)

BURROW_METHODS_DEFINE(SlogAttr, SLOGV_ATTR_METHODS);
BURROW_METHODS_DEFINE(SlogValue, SLOGV_VALUE_METHODS);

/* Go's Value has no exported fields, so neither does this one. */
const Type burrow_type_SlogValue = {
    BURROW_S_INIT("Value"),
    BURROW_S_INIT("log/slog"),
    KIND_STRUCT,
    (uint32_t)sizeof(SlogValue),
    (uint16_t)_Alignof(SlogValue),
    0,
    (uint16_t)(sizeof(burrow__methods_SlogValue) /
               sizeof(burrow__methods_SlogValue[0])),
    NULL,
    burrow__methods_SlogValue,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Field slogv_attr_fields[] = {
    {BURROW_S_INIT("Key"),
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(SlogAttr, key)},
    {BURROW_S_INIT("Value"),
     {NULL, 0},
     &burrow_type_SlogValue,
     (uint32_t)offsetof(SlogAttr, value)},
};

const Type burrow_type_SlogAttr = {
    BURROW_S_INIT("Attr"),
    BURROW_S_INIT("log/slog"),
    KIND_STRUCT,
    (uint32_t)sizeof(SlogAttr),
    (uint16_t)_Alignof(SlogAttr),
    (uint16_t)(sizeof slogv_attr_fields / sizeof slogv_attr_fields[0]),
    (uint16_t)(sizeof(burrow__methods_SlogAttr) / sizeof(burrow__methods_SlogAttr[0])),
    slogv_attr_fields,
    burrow__methods_SlogAttr,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------------ level */

static Str slogv_level_name(SlogLevel l, SlogLevel *offset) {
    if (l < SLOG_LEVEL_INFO) {
        *offset = l - SLOG_LEVEL_DEBUG;
        return BURROW_S("DEBUG");
    }
    if (l < SLOG_LEVEL_WARN) {
        *offset = l - SLOG_LEVEL_INFO;
        return BURROW_S("INFO");
    }
    if (l < SLOG_LEVEL_ERROR) {
        *offset = l - SLOG_LEVEL_WARN;
        return BURROW_S("WARN");
    }
    *offset = l - SLOG_LEVEL_ERROR;
    return BURROW_S("ERROR");
}

/* Level.String added to b. */
static Slice slogv_level_append(SlogLevel l, Alloc *a, Slice b) {
    SlogLevel off = 0;
    Str base = slogv_level_name(l, &off);
    b = slice_append(a, b, base.p, base.len);
    if (off == 0)
        return b;
    if (off > 0)
        b = slice_append(a, b, "+", 1);
    return strconv_append_int(a, b, (int64_t)off, 10);
}

Str slog_level_string(SlogLevel l, Alloc *a) {
    SlogLevel off = 0;
    Str base = slogv_level_name(l, &off);
    if (off == 0)
        return base;
    Slice b = slogv_level_append(l, a, slice_nil(TYPE_BYTE));
    return str_from_bytes(b.p, b.len);
}

Slice slog_level_marshal_json(SlogLevel l, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice b = slice_nil(TYPE_BYTE);
    return strconv_append_quote(a, b, slog_level_string(l, a));
}

/* Level.parse, with Go's wrapping of whatever went wrong. */
static Error slogv_level_parse(SlogLevel *l, Alloc *a, Str s) {
    Error err = BURROW_NO_ERROR;
    Str name = s;
    Int offset = 0;
    Int i = strings_index_any(s, BURROW_S("+-"));
    if (i >= 0) {
        name = (Str){s.p, i};
        offset = strconv_atoi((Str){s.p + i, s.len - i}, &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("slog: level string %q: %w", s, err);
    }
    Str up = strings_to_upper(a, name);
    if (str_eq(up, BURROW_S("DEBUG")))
        *l = SLOG_LEVEL_DEBUG;
    else if (str_eq(up, BURROW_S("INFO")))
        *l = SLOG_LEVEL_INFO;
    else if (str_eq(up, BURROW_S("WARN")))
        *l = SLOG_LEVEL_WARN;
    else if (str_eq(up, BURROW_S("ERROR")))
        *l = SLOG_LEVEL_ERROR;
    else
        return fmt_errorf_v("slog: level string %q: %w", s,
                            errors_new(error_allocator(), BURROW_S("unknown name")));
    *l += (SlogLevel)offset;
    return BURROW_NO_ERROR;
}

Error slog_level_unmarshal_json(SlogLevel *l, Alloc *a, Slice data) {
    Error err = BURROW_NO_ERROR;
    Str s = strconv_unquote(a, str_from_bytes(data.p, data.len), &err);
    if (BURROW_FAILED(err))
        return err;
    return slogv_level_parse(l, a, s);
}

Slice slog_level_append_text(SlogLevel l, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slogv_level_append(l, a, b);
}

Slice slog_level_marshal_text(SlogLevel l, Alloc *a, Error *err) {
    return slog_level_append_text(l, a, slice_nil(TYPE_BYTE), err);
}

Error slog_level_unmarshal_text(SlogLevel *l, Alloc *a, Slice data) {
    return slogv_level_parse(l, a, str_from_bytes(data.p, data.len));
}

SlogLevel slog_level_level(SlogLevel l) {
    return l;
}

static Slice slogv_m_level_append_text(SlogLevel *self, Alloc *a, Slice b, Error *err) {
    return slog_level_append_text(*self, a, b, err);
}
static SlogLevel slogv_m_level_level(SlogLevel *self) {
    return *self;
}
static Slice slogv_m_level_marshal_json(SlogLevel *self, Alloc *a, Error *err) {
    return slog_level_marshal_json(*self, a, err);
}
static Slice slogv_m_level_marshal_text(SlogLevel *self, Alloc *a, Error *err) {
    return slog_level_marshal_text(*self, a, err);
}
static Str slogv_m_level_string(SlogLevel *self) {
    return slog_level_string(*self, error_allocator());
}
static Error slogv_m_level_unmarshal_json(SlogLevel *self, Alloc *a, Slice data) {
    return slog_level_unmarshal_json(self, a, data);
}
static Error slogv_m_level_unmarshal_text(SlogLevel *self, Alloc *a, Slice data) {
    return slog_level_unmarshal_text(self, a, data);
}

#define SLOGV_SIG_LEVEL(IN, OUT) OUT(SlogLevel)
#define SLOGV_LEVEL_METHODS(M, T)                                                      \
    M(T, AppendText, slogv_m_level_append_text, ENCODING_SIG_APPEND_TEXT)              \
    M(T, Level, slogv_m_level_level, SLOGV_SIG_LEVEL)                                  \
    M(T, MarshalJSON, slogv_m_level_marshal_json, ENCODING_SIG_MARSHAL_BINARY)         \
    M(T, MarshalText, slogv_m_level_marshal_text, ENCODING_SIG_MARSHAL_TEXT)           \
    M(T, String, slogv_m_level_string, SLOGV_SIG_STRING)                               \
    M(T, UnmarshalJSON, slogv_m_level_unmarshal_json, ENCODING_SIG_UNMARSHAL_BINARY)   \
    M(T, UnmarshalText, slogv_m_level_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(SlogLevel, SLOGV_LEVEL_METHODS);

const Type burrow_type_SlogLevel = {
    BURROW_S_INIT("Level"),
    BURROW_S_INIT("log/slog"),
    KIND_INT,
    (uint32_t)sizeof(SlogLevel),
    (uint16_t)_Alignof(SlogLevel),
    0,
    (uint16_t)(sizeof(burrow__methods_SlogLevel) /
               sizeof(burrow__methods_SlogLevel[0])),
    NULL,
    burrow__methods_SlogLevel,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* --------------------------------------------------------------- levelvar */

SlogLevel slog_level_var_level(SlogLevelVar *v) {
    return (SlogLevel)sync_atomic_int64_load(&v->val);
}

void slog_level_var_set(SlogLevelVar *v, SlogLevel l) {
    sync_atomic_int64_store(&v->val, (int64_t)l);
}

Str slog_level_var_string(SlogLevelVar *v, Alloc *a) {
    Str l = slog_level_string(slog_level_var_level(v), a);
    return fmt_sprintf_v(a, "LevelVar(%s)", l);
}

Slice slog_level_var_append_text(SlogLevelVar *v, Alloc *a, Slice b, Error *err) {
    return slog_level_append_text(slog_level_var_level(v), a, b, err);
}

Slice slog_level_var_marshal_text(SlogLevelVar *v, Alloc *a, Error *err) {
    return slog_level_marshal_text(slog_level_var_level(v), a, err);
}

Error slog_level_var_unmarshal_text(SlogLevelVar *v, Alloc *a, Slice data) {
    SlogLevel l = 0;
    Error err = slog_level_unmarshal_text(&l, a, data);
    if (BURROW_FAILED(err))
        return err;
    slog_level_var_set(v, l);
    return BURROW_NO_ERROR;
}

static Slice slogv_m_lv_append_text(SlogLevelVar *self, Alloc *a, Slice b, Error *err) {
    return slog_level_var_append_text(self, a, b, err);
}
static SlogLevel slogv_m_lv_level(SlogLevelVar *self) {
    return slog_level_var_level(self);
}
static Slice slogv_m_lv_marshal_text(SlogLevelVar *self, Alloc *a, Error *err) {
    return slog_level_var_marshal_text(self, a, err);
}
static void slogv_m_lv_set(SlogLevelVar *self, SlogLevel l) {
    slog_level_var_set(self, l);
}
static Str slogv_m_lv_string(SlogLevelVar *self) {
    return slog_level_var_string(self, error_allocator());
}
static Error slogv_m_lv_unmarshal_text(SlogLevelVar *self, Alloc *a, Slice data) {
    return slog_level_var_unmarshal_text(self, a, data);
}

#define SLOGV_SIG_SET(IN, OUT) IN(0, SlogLevel)
#define SLOGV_LEVEL_VAR_METHODS(M, T)                                                  \
    M(T, AppendText, slogv_m_lv_append_text, ENCODING_SIG_APPEND_TEXT)                 \
    M(T, Level, slogv_m_lv_level, SLOGV_SIG_LEVEL)                                     \
    M(T, MarshalText, slogv_m_lv_marshal_text, ENCODING_SIG_MARSHAL_TEXT)              \
    M(T, Set, slogv_m_lv_set, SLOGV_SIG_SET)                                           \
    M(T, String, slogv_m_lv_string, SLOGV_SIG_STRING)                                  \
    M(T, UnmarshalText, slogv_m_lv_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(SlogLevelVar, SLOGV_LEVEL_VAR_METHODS);

/* Go's LevelVar is a struct with one unexported field, so this one shows none. */
const Type burrow_type_SlogLevelVar = {
    BURROW_S_INIT("LevelVar"),
    BURROW_S_INIT("log/slog"),
    KIND_STRUCT,
    (uint32_t)sizeof(SlogLevelVar),
    (uint16_t)_Alignof(SlogLevelVar),
    0,
    (uint16_t)(sizeof(burrow__methods_SlogLevelVar) /
               sizeof(burrow__methods_SlogLevelVar[0])),
    NULL,
    burrow__methods_SlogLevelVar,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* -------------------------------------------------------------- leveler */

/* The fixed level rides in the data pointer, so the Leveler holds nothing that
 * has to be kept alive. */
static SlogLevel slogv_fixed_level(void *self) {
    return (SlogLevel)(intptr_t)self;
}

static const SlogLevelerVT slogv_fixed_leveler_vt = {&burrow_type_SlogLevel,
                                                     slogv_fixed_level};

SlogLeveler slog_level_as_leveler(SlogLevel l) {
    return (SlogLeveler){&slogv_fixed_leveler_vt, (void *)(intptr_t)l};
}

static SlogLevel slogv_var_level(void *self) {
    return slog_level_var_level((SlogLevelVar *)self);
}

static const SlogLevelerVT slogv_var_leveler_vt = {&burrow_type_SlogLevelVar,
                                                   slogv_var_level};

SlogLeveler slog_level_var_as_leveler(SlogLevelVar *v) {
    return (SlogLeveler){&slogv_var_leveler_vt, v};
}

/* ----------------------------------------------------------------- source */

/* The header writes the struct out so that it reads as one. This is the same
 * three fields again, with their JSON names. */
#define SLOGV_SOURCE_FIELDS(F, T)                                                      \
    F(T, Str, function, Function, "json:\"function\"")                                 \
    F(T, Str, file, File, "json:\"file\"")                                             \
    F(T, Int, line, Line, "json:\"line\"")

static const Field slogv_source_fields[] = {
    SLOGV_SOURCE_FIELDS(BURROW__FIELD_AS, SlogSource)};

const Type burrow_type_SlogSource = {
    BURROW_S_INIT("Source"),
    BURROW_S_INIT("log/slog"),
    KIND_STRUCT,
    (uint32_t)sizeof(SlogSource),
    (uint16_t)_Alignof(SlogSource),
    (uint16_t)(sizeof slogv_source_fields / sizeof slogv_source_fields[0]),
    0,
    slogv_source_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

SlogValue burrow__slog_source_group(Alloc *a, const SlogSource *s) {
    SlogAttr as[3];
    Int n = 0;
    if (s->function.len > 0)
        as[n++] = slog_string(BURROW_S("function"), s->function);
    if (s->file.len > 0)
        as[n++] = slog_string(BURROW_S("file"), s->file);
    if (s->line != 0)
        as[n++] = slog_int(BURROW_S("line"), s->line);
    if (n == 0)
        return slog_group_value(slice_nil(TYPE_SLOG_ATTR));
    Slice out = slice_make(a, TYPE_SLOG_ATTR, n, n);
    if (out.p == NULL)
        return slog_group_value(slice_nil(TYPE_SLOG_ATTR));
    memcpy(out.p, as, (size_t)n * sizeof as[0]);
    return slog_group_value(out);
}

/* ----------------------------------------------------------------- record */

SlogRecord slog_new_record(Time t, SlogLevel level, Str msg, Uintptr pc) {
    SlogRecord r;
    memset(&r, 0, sizeof r);
    r.time = t;
    r.message = msg;
    r.level = level;
    r.pc = pc;
    r.back = slice_nil(TYPE_SLOG_ATTR);
    return r;
}

SlogRecord slog_record_clone(SlogRecord r) {
    r.back.cap = r.back.len; /* slices.Clip */
    return r;
}

Int slog_record_num_attrs(const SlogRecord *r) {
    return r->nfront + r->back.len;
}

void slog_record_attrs(const SlogRecord *r, bool (*f)(void *ctx, SlogAttr a),
                       void *ctx) {
    for (Int i = 0; i < r->nfront; i++)
        if (!f(ctx, r->front[i]))
            return;
    const SlogAttr *b = (const SlogAttr *)r->back.p;
    for (Int i = 0; i < r->back.len; i++)
        if (!f(ctx, b[i]))
            return;
}

/* Go's check that this record is not a plain copy of one that has already
 * added to back's spare room, which would be overwritten by this add. It
 * looks one past the end, and anything there that is not empty means
 * somebody else got there first. */
static void slogv_check_back(SlogRecord *r, Alloc *a) {
    if (r->back.cap <= r->back.len)
        return;
    const SlogAttr *p = (const SlogAttr *)r->back.p;
    if (burrow__slog_attr_is_empty(p[r->back.len]))
        return;
    r->back.cap = r->back.len;
    SlogAttr bug = slog_string(BURROW_S("!BUG"),
                               BURROW_S("AddAttrs unsafely called on copy of Record "
                                        "made without using Record.Clone"));
    r->back = slice_append(a, r->back, &bug, 1);
}

void slog_record_add_attrs(SlogRecord *r, Alloc *a, Slice attrs) {
    const SlogAttr *as = (const SlogAttr *)attrs.p;
    Int i = 0;
    for (; i < attrs.len && r->nfront < BURROW__SLOG_NATTRS_INLINE; i++) {
        if (burrow__slog_value_is_empty_group(as[i].value))
            continue;
        r->front[r->nfront++] = as[i];
    }
    slogv_check_back(r, a);
    for (; i < attrs.len; i++)
        if (!burrow__slog_value_is_empty_group(as[i].value))
            r->back = slice_append(a, r->back, &as[i], 1);
}

void slog_record_add(SlogRecord *r, Alloc *a, Slice args) {
    Int i = 0;
    while (i < args.len) {
        SlogAttr at = burrow__slog_args_to_attr(args, &i);
        if (burrow__slog_value_is_empty_group(at.value))
            continue;
        if (r->nfront < BURROW__SLOG_NATTRS_INLINE) {
            r->front[r->nfront++] = at;
            continue;
        }
        if (r->back.p == NULL) {
            Int n = burrow__slog_count_attrs(slice_sub(args, i, args.len)) + 1;
            r->back = slice_make(a, TYPE_SLOG_ATTR, 0, n);
        }
        r->back = slice_append(a, r->back, &at, 1);
    }
}

bool slog_record_source(const SlogRecord *r, SlogSource *out) {
    if (r->source.file.len > 0 || r->source.function.len > 0 || r->source.line != 0) {
        *out = r->source;
        return true;
    }
    if (r->pc == 0)
        return false;
    Uintptr pcs[1] = {r->pc};
    RuntimeFrames it = runtime_callers_frames(slice_from(pcs, 1, 1, TYPE_UINTPTR));
    RuntimeFrame f;
    memset(out, 0, sizeof *out);
    if (runtime_frames_next(&it, &f)) {
        out->function = f.function;
        out->file = f.file;
        out->line = f.line;
    }
    return true;
}
