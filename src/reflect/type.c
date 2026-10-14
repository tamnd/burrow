/* package reflect, the Type half: kinds, names, strings, fields and tags. Go's
 * reflect/type.go and visiblefields.go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "burrow/chan.h"
#include "burrow/lock.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/strconv.h"

/* ------------------------------------------------------------ the keepsakes
 *
 * Everything the package makes once and keeps: interned strings, each type's
 * String, and the long tail of one element field indexes. All of it lives in
 * one arena that is never freed, under one lock. */

static burrow__Lock rf_lock;
static Arena rf_arena;
static bool rf_ready;
static Map *rf_interned;     /* Str to the same Str, the copy in rf_arena */
static Map *rf_type_string;  /* const Type * to its String */
static Map *rf_value_errors; /* message to its RfValueErrorBox */
static Int *rf_iota[256];    /* rf_iota[c][k] is c*256 + k, made on first use */
static Map *rf_made;         /* RfMadeKey to the type PointerTo and the rest made */
static Byte *rf_zero_big;    /* zeros for Zero of a type too big for rf_zero_small */
static size_t rf_zero_big_size;

static const Str rf_oom = {(const Byte *)"reflect: out of memory", 22};

/* The key rf_made is keyed by, [4]unsafe.Pointer: the kind, the element, the
 * key and the length of the type made. */
static const Type rf_made_key = {
    {NULL, 0},
    {NULL, 0},
    KIND_ARRAY,
    (uint32_t)(4 * sizeof(void *)),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_UnsafePointer,
    NULL,
    4,
    0,
    NULL,
};

/* Takes the lock and makes sure the arena and the maps are there. */
static void rf_enter(void) {
    burrow__lock(&rf_lock);
    if (rf_ready)
        return;
    arena_init(&rf_arena, heap_allocator(), 4096);
    Alloc *a = arena_allocator(&rf_arena);
    rf_interned = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    rf_type_string = map_make(a, TYPE_UNSAFE_POINTER, TYPE_STRING, 0);
    rf_value_errors = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    rf_made = map_make(a, &rf_made_key, TYPE_UNSAFE_POINTER, 0);
    if (rf_interned == NULL || rf_type_string == NULL || rf_value_errors == NULL ||
        rf_made == NULL) {
        burrow__unlock(&rf_lock);
        panic_str(rf_oom);
    }
    rf_ready = true;
}

BURROW_NORETURN static void rf_out_of_memory(void) {
    burrow__unlock(&rf_lock);
    panic_str(rf_oom);
}

/* The interned copy of s. Called with the lock held. */
static Str rf_intern_locked(Str s) {
    Str *have = map_get(rf_interned, &s);
    if (have != NULL)
        return *have;
    Byte *p = NULL;
    if (s.len > 0) {
        p = mem_alloc_nozero(arena_allocator(&rf_arena), (size_t)s.len, 1);
        if (p == NULL)
            rf_out_of_memory();
        memcpy(p, s.p, (size_t)s.len);
    }
    Str c = str_from_bytes(p, s.len);
    if (!map_set(rf_interned, &c, &c))
        rf_out_of_memory();
    return c;
}

Str burrow__reflect_intern(Str s) {
    rf_enter();
    Str c = rf_intern_locked(s);
    burrow__unlock(&rf_lock);
    return c;
}

/* A growing buffer on the heap, for building a string to intern. */
typedef struct RfBuf {
    Byte *p;
    Int len;
    Int cap;
    bool failed;
} RfBuf;

static void rfb_write(RfBuf *b, const Byte *p, Int n) {
    if (b->failed || n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int nc = b->cap < 64 ? 64 : b->cap * 2;
        while (nc < b->len + n)
            nc *= 2;
        Byte *np = mem_realloc(heap_allocator(), b->p, (size_t)b->cap, (size_t)nc, 1);
        if (np == NULL) {
            b->failed = true;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void rfb_str(RfBuf *b, Str s) {
    rfb_write(b, s.p, s.len);
}

static void rfb_cstr(RfBuf *b, const char *s) {
    rfb_write(b, (const Byte *)s, (Int)strlen(s));
}

static void rfb_int(RfBuf *b, int64_t v) {
    Byte num[24];
    Int n = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        num[sizeof num - 1 - (size_t)n++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    if (v < 0)
        num[sizeof num - 1 - (size_t)n++] = '-';
    rfb_write(b, num + sizeof num - (size_t)n, n);
}

/* Interns what the buffer holds and lets the buffer go. */
static Str rfb_intern(RfBuf *b) {
    if (b->failed) {
        mem_free(heap_allocator(), b->p, (size_t)b->cap, 1);
        panic_str(rf_oom);
    }
    Str s = burrow__reflect_intern(str_from_bytes(b->p, b->len));
    mem_free(heap_allocator(), b->p, (size_t)b->cap, 1);
    return s;
}

Str burrow__reflect_concat(Str a, Str b, Str c) {
    RfBuf buf = {0};
    rfb_str(&buf, a);
    rfb_str(&buf, b);
    rfb_str(&buf, c);
    return rfb_intern(&buf);
}

void burrow__reflect_panic(Str a, Str b, Str c) {
    panic_str(burrow__reflect_concat(a, b, c));
}

/* ------------------------------------------------------------- ValueError
 *
 * A Value method called on the wrong kind panics with an error, as Go's does,
 * so that the message prints and errors_as finds the ReflectValueError. Each
 * method and kind pair gets one error, made the first time and kept, since a
 * panic's value has to outlive the frame that raised it. */

typedef struct RfValueErrorBox {
    ReflectValueError e;
    Str message;
} RfValueErrorBox;

static Str rf_value_error_message(const void *self) {
    return ((const RfValueErrorBox *)self)->message;
}

static Error rf_value_error_clone(const void *self, Alloc *a);

static const ErrorVT rf_value_error_vt = {
    &burrow_type_ReflectValueError, rf_value_error_message, NULL, NULL, NULL, NULL,
    rf_value_error_clone,
};

/* The box is never freed, so a clone can share it. */
static Error rf_value_error_clone(const void *self, Alloc *a) {
    (void)a;
    return (Error){&rf_value_error_vt, self};
}

static void rf_value_error_text(RfBuf *b, const ReflectValueError *e) {
    rfb_cstr(b, "reflect: call of ");
    rfb_str(b, e->method);
    if (e->kind == KIND_INVALID) {
        rfb_cstr(b, " on zero Value");
        return;
    }
    rfb_cstr(b, " on ");
    rfb_str(b, reflect_kind_string(e->kind));
    rfb_cstr(b, " Value");
}

Str reflect_value_error_error(ReflectValueError *e, Alloc *a) {
    RfBuf b = {0};
    rf_value_error_text(&b, e);
    Str s = BURROW_STR_EMPTY;
    Byte *p = b.failed ? NULL : mem_alloc_nozero(a, (size_t)b.len, 1);
    if (p != NULL) {
        memcpy(p, b.p, (size_t)b.len);
        s = str_from_bytes(p, b.len);
    }
    mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    return s;
}

void burrow__reflect_panic_kind(Str method, Kind k) {
    ReflectValueError e = {method, k};
    RfBuf b = {0};
    rf_value_error_text(&b, &e);
    Str msg = rfb_intern(&b);

    rf_enter();
    const void *const *have = map_get(rf_value_errors, &msg);
    const void *box = have != NULL ? *have : NULL;
    if (box == NULL) {
        RfValueErrorBox *nb = BURROW_NEW(arena_allocator(&rf_arena), RfValueErrorBox);
        if (nb == NULL)
            rf_out_of_memory();
        nb->e.method = rf_intern_locked(method);
        nb->e.kind = k;
        nb->message = msg;
        box = nb;
        if (!map_set(rf_value_errors, &msg, &box))
            rf_out_of_memory();
    }
    burrow__unlock(&rf_lock);
    Error err = {&rf_value_error_vt, box};
    panic(BURROW_ANY(TYPE_ERROR, &err));
}

BURROW_NORETURN static void rf_panic_type(const char *msg, const Type *t) {
    burrow__reflect_panic(str_from_cstr(msg), reflect_type_string(t), BURROW_STR_EMPTY);
}

/* ------------------------------------------------------------------ kinds */

Str reflect_kind_string(ReflectKind k) {
    if ((unsigned)k < (unsigned)KIND_MAX)
        return kind_name(k);
    RfBuf b = {0};
    rfb_cstr(&b, "kind");
    /* Go prints int(k), so a Kind made from -1 says kind-1. */
    rfb_int(&b, (int64_t)(int32_t)(uint32_t)k);
    return rfb_intern(&b);
}

Str reflect_chan_dir_string(ReflectChanDir d) {
    switch ((int)d) {
    case REFLECT_SEND_DIR:
        return BURROW_S("chan<-");
    case REFLECT_RECV_DIR:
        return BURROW_S("<-chan");
    case REFLECT_BOTH_DIR:
        return BURROW_S("chan");
    default: {
        RfBuf b = {0};
        rfb_cstr(&b, "ChanDir");
        rfb_int(&b, (int64_t)d);
        return rfb_intern(&b);
    }
    }
}

/* ----------------------------------------------------------------- strings */

static ReflectChanDir rf_chan_dir(const Type *t) {
    return t->len == 0 ? REFLECT_BOTH_DIR : (ReflectChanDir)t->len;
}

static void rf_write_type(RfBuf *b, const Type *t) {
    if (t == NULL) {
        rfb_cstr(b, "<nil>");
        return;
    }
    if (t->name.len > 0) {
        if (t->pkg_path.len > 0) {
            Int i = t->pkg_path.len;
            while (i > 0 && t->pkg_path.p[i - 1] != '/')
                i--;
            rfb_write(b, t->pkg_path.p + i, t->pkg_path.len - i);
            rfb_cstr(b, ".");
        }
        rfb_str(b, t->name);
        return;
    }
    switch ((int)t->kind) {
    case KIND_SLICE:
        rfb_cstr(b, "[]");
        rf_write_type(b, t->elem);
        return;
    case KIND_ARRAY:
        rfb_cstr(b, "[");
        rfb_int(b, (int64_t)t->len);
        rfb_cstr(b, "]");
        rf_write_type(b, t->elem);
        return;
    case KIND_POINTER:
        rfb_cstr(b, "*");
        rf_write_type(b, t->elem);
        return;
    case KIND_MAP:
        rfb_cstr(b, "map[");
        rf_write_type(b, t->key);
        rfb_cstr(b, "]");
        rf_write_type(b, t->elem);
        return;
    case KIND_CHAN: {
        ReflectChanDir d = rf_chan_dir(t);
        if (d == REFLECT_RECV_DIR)
            rfb_cstr(b, "<-chan ");
        else if (d == REFLECT_SEND_DIR)
            rfb_cstr(b, "chan<- ");
        else
            rfb_cstr(b, "chan ");
        /* chan (<-chan int), since chan <-chan int would read as chan<- chan
         * int. */
        bool paren = d == REFLECT_BOTH_DIR && t->elem != NULL &&
                     t->elem->kind == KIND_CHAN && t->elem->name.len == 0 &&
                     rf_chan_dir(t->elem) == REFLECT_RECV_DIR;
        if (paren)
            rfb_cstr(b, "(");
        rf_write_type(b, t->elem);
        if (paren)
            rfb_cstr(b, ")");
        return;
    }
    case KIND_FUNC: {
        Int nin = type_num_in(t);
        Int nout = type_num_out(t);
        bool variadic = t->elem != NULL;
        rfb_cstr(b, "func(");
        for (Int i = 0; i < nin; i++) {
            const Type *in = type_in(t, i);
            if (i > 0)
                rfb_cstr(b, ", ");
            if (variadic && i == nin - 1 && in != NULL && in->kind == KIND_SLICE) {
                rfb_cstr(b, "...");
                rf_write_type(b, in->elem);
            } else {
                rf_write_type(b, in);
            }
        }
        rfb_cstr(b, ")");
        if (nout == 1) {
            rfb_cstr(b, " ");
            rf_write_type(b, type_out(t, 0));
        } else if (nout > 1) {
            rfb_cstr(b, " (");
            for (Int i = 0; i < nout; i++) {
                if (i > 0)
                    rfb_cstr(b, ", ");
                rf_write_type(b, type_out(t, i));
            }
            rfb_cstr(b, ")");
        }
        return;
    }
    case KIND_STRUCT:
        if (t->nfield == 0) {
            rfb_cstr(b, "struct {}");
            return;
        }
        rfb_cstr(b, "struct {");
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0)
                rfb_cstr(b, ";");
            rfb_cstr(b, " ");
            if (!burrow__reflect_embedded(f)) {
                rfb_str(b, f->name);
                rfb_cstr(b, " ");
            }
            rf_write_type(b, f->type);
            if (f->tag.len > 0) {
                Byte tmp[128];
                Slice q = strconv_append_quote(
                    heap_allocator(), slice_from(tmp, 0, (Int)sizeof tmp, TYPE_BYTE),
                    f->tag);
                rfb_cstr(b, " ");
                rfb_write(b, (const Byte *)q.p, q.len);
                if (q.p != (void *)tmp)
                    mem_free(heap_allocator(), q.p, (size_t)q.cap, 1);
            }
        }
        rfb_cstr(b, " }");
        return;
    case KIND_INTERFACE:
        rfb_cstr(b, "interface {}");
        return;
    default:
        rfb_str(b, kind_name(t->kind));
        return;
    }
}

Str reflect_type_string(ReflectType t) {
    if (t == NULL)
        return BURROW_S("<nil>");
    /* A named builtin needs no work and no lock. */
    if (t->name.len > 0 && t->pkg_path.len == 0)
        return t->name;
    rf_enter();
    const void *key = t;
    Str *have = map_get(rf_type_string, &key);
    burrow__unlock(&rf_lock);
    if (have != NULL)
        return *have;

    RfBuf b = {0};
    rf_write_type(&b, t);
    Str s = rfb_intern(&b);
    rf_enter();
    if (!map_set(rf_type_string, &key, &s))
        rf_out_of_memory();
    burrow__unlock(&rf_lock);
    return s;
}

/* -------------------------------------------------------------- the methods */

Int reflect_type_align(ReflectType t) {
    return (Int)t->align;
}

Int reflect_type_field_align(ReflectType t) {
    return (Int)t->align;
}

Uintptr reflect_type_size(ReflectType t) {
    return (Uintptr)t->size;
}

ReflectKind reflect_type_kind(ReflectType t) {
    return t == NULL ? KIND_INVALID : t->kind;
}

Str reflect_type_name(ReflectType t) {
    return t->name;
}

Str reflect_type_pkg_path(ReflectType t) {
    return t->pkg_path;
}

Int reflect_type_bits(ReflectType t) {
    if (t == NULL)
        panic_str(BURROW_S("reflect: Bits of nil Type"));
    if (t->kind < KIND_INT || t->kind > KIND_COMPLEX128)
        rf_panic_type("reflect: Bits of non-arithmetic Type ", t);
    return (Int)t->size * 8;
}

bool reflect_type_comparable(ReflectType t) {
    return type_is_comparable(t);
}

ReflectChanDir reflect_type_chan_dir(ReflectType t) {
    if (t->kind != KIND_CHAN)
        rf_panic_type("reflect: ChanDir of non-chan type ", t);
    return rf_chan_dir(t);
}

ReflectType reflect_type_elem(ReflectType t) {
    switch ((int)t->kind) {
    case KIND_ARRAY:
    case KIND_CHAN:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_SLICE:
        return t->elem;
    default:
        rf_panic_type("reflect: Elem of invalid type ", t);
    }
}

ReflectType reflect_type_key(ReflectType t) {
    if (t->kind != KIND_MAP)
        rf_panic_type("reflect: Key of non-map type ", t);
    return t->key;
}

Int reflect_type_len(ReflectType t) {
    if (t->kind != KIND_ARRAY)
        rf_panic_type("reflect: Len of non-array type ", t);
    return (Int)t->len;
}

Int reflect_type_num_field(ReflectType t) {
    if (t->kind != KIND_STRUCT)
        rf_panic_type("reflect: NumField of non-struct type ", t);
    return (Int)t->nfield;
}

Int reflect_type_num_in(ReflectType t) {
    if (t->kind != KIND_FUNC)
        rf_panic_type("reflect: NumIn of non-func type ", t);
    return type_num_in(t);
}

Int reflect_type_num_out(ReflectType t) {
    if (t->kind != KIND_FUNC)
        rf_panic_type("reflect: NumOut of non-func type ", t);
    return type_num_out(t);
}

ReflectType reflect_type_in(ReflectType t, Int i) {
    if (t->kind != KIND_FUNC)
        rf_panic_type("reflect: In of non-func type ", t);
    if (i < 0 || i >= type_num_in(t))
        runtime_index_out_of_range(i, type_num_in(t));
    return type_in(t, i);
}

ReflectType reflect_type_out(ReflectType t, Int i) {
    if (t->kind != KIND_FUNC)
        rf_panic_type("reflect: Out of non-func type ", t);
    if (i < 0 || i >= type_num_out(t))
        runtime_index_out_of_range(i, type_num_out(t));
    return type_out(t, i);
}

bool reflect_type_is_variadic(ReflectType t) {
    if (t->kind != KIND_FUNC)
        rf_panic_type("reflect: IsVariadic of non-func type ", t);
    return t->elem != NULL;
}

Int reflect_type_num_method(ReflectType t) {
    Int n = 0;
    if (t->methods == NULL)
        return 0;
    for (uint16_t i = 0; i < t->nmethod; i++) {
        Str name = t->methods[i].name;
        if (name.len > 0 && name.p[0] >= 'A' && name.p[0] <= 'Z')
            n++;
    }
    return n;
}

static bool rf_overflow_float32(double x) {
    if (x < 0)
        x = -x;
    return (double)FLT_MAX < x && x <= DBL_MAX;
}

bool reflect_type_overflow_int(ReflectType t, int64_t x) {
    if (t->kind < KIND_INT || t->kind > KIND_INT64)
        rf_panic_type("reflect: OverflowInt of non-int type ", t);
    unsigned bits = t->size * 8;
    if (bits >= 64)
        return false;
    int64_t lo = -((int64_t)1 << (bits - 1));
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
    return x < lo || x > hi;
}

bool reflect_type_overflow_uint(ReflectType t, uint64_t x) {
    if (t->kind < KIND_UINT || t->kind > KIND_UINTPTR)
        rf_panic_type("reflect: OverflowUint of non-uint type ", t);
    unsigned bits = t->size * 8;
    if (bits >= 64)
        return false;
    return x >> bits != 0;
}

bool reflect_type_overflow_float(ReflectType t, double x) {
    switch ((int)t->kind) {
    case KIND_FLOAT32:
        return rf_overflow_float32(x);
    case KIND_FLOAT64:
        return false;
    default:
        rf_panic_type("reflect: OverflowFloat of non-float type ", t);
    }
}

bool reflect_type_overflow_complex(ReflectType t, Complex128 x) {
    switch ((int)t->kind) {
    case KIND_COMPLEX64:
        return rf_overflow_float32(x.re) || rf_overflow_float32(x.im);
    case KIND_COMPLEX128:
        return false;
    default:
        rf_panic_type("reflect: OverflowComplex of non-complex type ", t);
    }
}

/* Go's canRangeFunc: func(yield func(K[, V]) bool). */
static bool rf_can_range_func(const Type *t, Int seq) {
    if (t->kind != KIND_FUNC || type_num_in(t) != 1 || type_num_out(t) != 0)
        return false;
    const Type *y = type_in(t, 0);
    if (y == NULL || y->kind != KIND_FUNC)
        return false;
    const Type *r = type_num_out(y) == 1 ? type_out(y, 0) : NULL;
    return type_num_in(y) == seq && r != NULL && r->kind == KIND_BOOL &&
           r->pkg_path.len == 0;
}

bool reflect_type_can_seq(ReflectType t) {
    switch ((int)t->kind) {
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
    case KIND_INT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINT:
    case KIND_UINTPTR:
    case KIND_ARRAY:
    case KIND_SLICE:
    case KIND_CHAN:
    case KIND_STRING:
    case KIND_MAP:
        return true;
    case KIND_FUNC:
        return rf_can_range_func(t, 1);
    case KIND_POINTER:
        return t->elem != NULL && t->elem->kind == KIND_ARRAY;
    default:
        return false;
    }
}

bool reflect_type_can_seq2(ReflectType t) {
    switch ((int)t->kind) {
    case KIND_ARRAY:
    case KIND_SLICE:
    case KIND_STRING:
    case KIND_MAP:
        return true;
    case KIND_FUNC:
        return rf_can_range_func(t, 2);
    case KIND_POINTER:
        return t->elem != NULL && t->elem->kind == KIND_ARRAY;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- interfaces */

bool burrow__reflect_unwrap(const Type **tp, void **dp) {
    const Type *t = *tp;
    if (t == TYPE_ANY) {
        Any a = *(const Any *)*dp;
        if (a.t == NULL)
            return false;
        *tp = a.t;
        *dp = a.data;
        return true;
    }
    if (t == TYPE_ERROR) {
        const Error *e = *dp;
        if (e->vt == NULL)
            return false;
        if (e->vt->self_type != NULL) {
            *tp = e->vt->self_type;
            *dp = (void *)(uintptr_t)e->data;
        }
        return true;
    }
    Iface v = *(const Iface *)*dp;
    if (v.vt == NULL)
        return false;
    if (v.vt->self_type != NULL) {
        *tp = v.vt->self_type;
        /* image/color's Color holds its value inline, after the vtable. */
        if (t->size > sizeof(Iface))
            *dp = burrow__iface_inline(t, *dp);
        else
            *dp = v.data;
    }
    return true;
}

ReflectType reflect_type_of(Any i) {
    const Type *t = i.t;
    void *d = i.data;
    while (t != NULL && t->kind == KIND_INTERFACE) {
        const Type *was = t;
        if (!burrow__reflect_unwrap(&t, &d))
            return NULL;
        if (t == was)
            break;
    }
    return t;
}

/* Identity, or with under set, whether the underlying types are identical,
 * which looks past the names of t and u themselves and nothing deeper. With
 * tags clear, struct tags are not compared at any depth, and two named types
 * with the same name and package are the same when their underlying types are,
 * which is how Go's conversions see them. */
static bool rf_identical(const Type *t, const Type *u, bool under, bool tags) {
    if (t == u)
        return true;
    if (t == NULL || u == NULL || t->kind != u->kind)
        return false;
    if (!under && (t->name.len > 0 || u->name.len > 0) &&
        (tags || !str_eq(t->name, u->name) || !str_eq(t->pkg_path, u->pkg_path)))
        return false;
    switch ((int)t->kind) {
    case KIND_ARRAY:
        return t->len == u->len && rf_identical(t->elem, u->elem, false, tags);
    case KIND_CHAN:
        return rf_chan_dir(t) == rf_chan_dir(u) &&
               rf_identical(t->elem, u->elem, false, tags);
    case KIND_POINTER:
    case KIND_SLICE:
        return rf_identical(t->elem, u->elem, false, tags);
    case KIND_MAP:
        return rf_identical(t->key, u->key, false, tags) &&
               rf_identical(t->elem, u->elem, false, tags);
    case KIND_FUNC: {
        Int nin = type_num_in(t);
        Int nout = type_num_out(t);
        if (nin != type_num_in(u) || nout != type_num_out(u) ||
            (t->elem != NULL) != (u->elem != NULL))
            return false;
        for (Int i = 0; i < nin; i++)
            if (!rf_identical(type_in(t, i), type_in(u, i), false, tags))
                return false;
        for (Int i = 0; i < nout; i++)
            if (!rf_identical(type_out(t, i), type_out(u, i), false, tags))
                return false;
        return true;
    }
    case KIND_STRUCT:
        if (t->nfield != u->nfield)
            return false;
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            const Field *g = &u->fields[i];
            if (!str_eq(f->name, g->name) || (tags && !str_eq(f->tag, g->tag)) ||
                burrow__reflect_embedded(f) != burrow__reflect_embedded(g) ||
                !rf_identical(f->type, g->type, false, tags))
                return false;
            /* An unexported name is only the same name in the same package. */
            if (!field_is_exported(f) && !str_eq(t->pkg_path, u->pkg_path))
                return false;
        }
        return true;
    case KIND_INTERFACE:
        /* Only through the descriptor, which the first test above has
         * already tried. The empty interface is the one descriptor TYPE_ANY,
         * and the others are vtable interfaces whose descriptors do not list
         * their methods, so two of them cannot be told apart from two
         * different interfaces. */
        return false;
    default:
        /* The rest are the predeclared kinds, where the kind is the
         * underlying type. */
        return under;
    }
}

bool burrow__reflect_identical(const Type *t, const Type *u) {
    return rf_identical(t, u, false, true);
}

bool burrow__reflect_same_underlying(const Type *t, const Type *u) {
    return rf_identical(t, u, true, false);
}

bool burrow__reflect_assignable(const Type *dst, const Type *v) {
    if (dst == v)
        return true;
    if (dst == NULL || v == NULL || dst->kind != v->kind ||
        (dst->name.len > 0 && v->name.len > 0))
        return false;
    /* A chan T goes into a <-chan T or a chan<- T, Go's
     * specialChannelAssignability. */
    if (dst->kind == KIND_CHAN && rf_chan_dir(v) == REFLECT_BOTH_DIR &&
        (dst->name.len == 0 || v->name.len == 0) &&
        rf_identical(dst->elem, v->elem, false, true))
        return true;
    return rf_identical(dst, v, true, true);
}

/* -------------------------------------------------------------- made types
 *
 * PointerTo, SliceOf, MapOf, ArrayOf and ChanOf. Each type is made once, in
 * rf_arena, and kept in rf_made under its kind and its parts, so that asking
 * twice gives the same descriptor, as it does in Go. Go looks through the types
 * compiled into the program before making one, which C has no way to do. The
 * one such type the package knows about is []byte, which SliceOf hands back. A
 * made type is still identical to any other descriptor for the same unnamed
 * type, in the sense burrow__reflect_identical gives. */

static const Type *rf_made_type(Kind k, const Type *elem, const Type *key, uint32_t len,
                                uint32_t size, uint16_t align) {
    const void *ck[4] = {(const void *)(uintptr_t)k, elem, key,
                         (const void *)(uintptr_t)len};
    rf_enter();
    const Type *const *have = map_get(rf_made, ck);
    if (have != NULL) {
        const Type *t = *have;
        burrow__unlock(&rf_lock);
        return t;
    }
    Type *t = BURROW_NEW(arena_allocator(&rf_arena), Type);
    if (t == NULL)
        rf_out_of_memory();
    *t = (Type){{NULL, 0}, {NULL, 0}, k,    size, align, 0, 0,
                NULL,      NULL,      elem, key,  len,   0, NULL};
    const Type *made = t;
    if (!map_set(rf_made, ck, &made))
        rf_out_of_memory();
    burrow__unlock(&rf_lock);
    return made;
}

ReflectType reflect_pointer_to(ReflectType t) {
    if (t == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    return rf_made_type(KIND_POINTER, t, NULL, 0, (uint32_t)sizeof(void *),
                        (uint16_t)_Alignof(void *));
}

ReflectType reflect_slice_of(ReflectType t) {
    if (t == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    if (t == TYPE_UINT8)
        return TYPE_BYTES;
    return rf_made_type(KIND_SLICE, t, NULL, 0, (uint32_t)sizeof(Slice),
                        (uint16_t)_Alignof(Slice));
}

ReflectType reflect_map_of(ReflectType key, ReflectType elem) {
    if (key == NULL || elem == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    if (!type_is_comparable(key))
        rf_panic_type("reflect.MapOf: invalid key type ", key);
    return rf_made_type(KIND_MAP, elem, key, 0, (uint32_t)sizeof(Map *),
                        (uint16_t)_Alignof(Map *));
}

ReflectType reflect_array_of(Int length, ReflectType elem) {
    if (length < 0)
        panic_str(BURROW_S("reflect: negative length passed to ArrayOf"));
    if (elem == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    /* The descriptor keeps both in 32 bits, which is the space there is. */
    if ((uint64_t)length > UINT32_MAX ||
        (elem->size > 0 && (uint64_t)length > UINT32_MAX / elem->size))
        panic_str(BURROW_S("reflect.ArrayOf: array size would exceed virtual address "
                           "space"));
    return rf_made_type(KIND_ARRAY, elem, NULL, (uint32_t)length,
                        elem->size * (uint32_t)length, elem->align);
}

ReflectType reflect_chan_of(ReflectChanDir dir, ReflectType t) {
    if (t == NULL)
        runtime_panic(BURROW_S("invalid memory address or nil pointer dereference"));
    if (t->size >= 1U << 16)
        panic_str(BURROW_S("reflect.ChanOf: element size too large"));
    uint32_t len;
    switch ((int)dir) {
    case REFLECT_SEND_DIR:
        len = REFLECT_SEND_DIR;
        break;
    case REFLECT_RECV_DIR:
        len = REFLECT_RECV_DIR;
        break;
    case REFLECT_BOTH_DIR:
        /* A channel descriptor says both ways with 0. */
        len = 0;
        break;
    default:
        panic_str(BURROW_S("reflect.ChanOf: invalid dir"));
    }
    return rf_made_type(KIND_CHAN, t, NULL, len, (uint32_t)sizeof(Chan *),
                        (uint16_t)_Alignof(Chan *));
}

/* -------------------------------------------------------------------- zeros */

/* What Zero points at for the types most programs have, never written. */
static _Alignas(max_align_t) const Byte rf_zero_small[1024];

const void *burrow__reflect_zero(size_t size) {
    if (size <= sizeof rf_zero_small)
        return rf_zero_small;
    rf_enter();
    /* A bigger block replaces a smaller one, and the smaller one stays where it
     * is, since Values from Zero may still point at it. */
    if (size > rf_zero_big_size) {
        Byte *p = mem_alloc(arena_allocator(&rf_arena), size, _Alignof(max_align_t));
        if (p == NULL)
            rf_out_of_memory();
        rf_zero_big = p;
        rf_zero_big_size = size;
    }
    const Byte *p = rf_zero_big;
    burrow__unlock(&rf_lock);
    return p;
}

/* ------------------------------------------------------------------ fields */

bool burrow__reflect_embedded(const Field *f) {
    if (field_is_embedded(f))
        return true;
    const Type *t = f->type;
    return t != NULL && t->kind == KIND_POINTER && t->name.len == 0 &&
           t->elem != NULL && t->elem->name.len > 0 && str_eq(f->name, t->elem->name);
}

/* 0 to 255, so that the index of one of the first 256 fields needs no lock. */
#define RF_I4(n) (n), (n) + 1, (n) + 2, (n) + 3
#define RF_I16(n) RF_I4(n), RF_I4((n) + 4), RF_I4((n) + 8), RF_I4((n) + 12)
#define RF_I64(n) RF_I16(n), RF_I16((n) + 16), RF_I16((n) + 32), RF_I16((n) + 48)
static const Int rf_small_iota[256] = {RF_I64(0), RF_I64(64), RF_I64(128), RF_I64(192)};

/* A Slice holding just i, in memory that is never freed. */
static Slice rf_index1(Int i) {
    const Int *p;
    if (i < 256) {
        p = &rf_small_iota[i];
    } else {
        Int c = i / 256;
        rf_enter();
        if (rf_iota[c] == NULL) {
            Int *chunk = BURROW_NEW_N(arena_allocator(&rf_arena), Int, 256);
            if (chunk == NULL)
                rf_out_of_memory();
            for (Int k = 0; k < 256; k++)
                chunk[k] = c * 256 + k;
            rf_iota[c] = chunk;
        }
        p = &rf_iota[c][i % 256];
        burrow__unlock(&rf_lock);
    }
    return (Slice){(void *)(Uintptr)p, 1, 1, TYPE_INT};
}

ReflectStructField burrow__reflect_field(const Type *t, Int i) {
    const Field *f = &t->fields[i];
    ReflectStructField sf;
    memset(&sf, 0, sizeof sf);
    sf.name = f->name;
    if (!field_is_exported(f))
        sf.pkg_path = t->pkg_path.len > 0 ? t->pkg_path : BURROW_S("main");
    sf.type = f->type;
    sf.tag = f->tag;
    sf.offset = (Uintptr)f->offset;
    sf.index = rf_index1(i);
    sf.anonymous = burrow__reflect_embedded(f);
    return sf;
}

bool reflect_struct_field_is_exported(ReflectStructField f) {
    return f.pkg_path.len == 0;
}

ReflectStructField reflect_type_field(ReflectType t, Int i) {
    if (t->kind != KIND_STRUCT)
        rf_panic_type("reflect: Field of non-struct type ", t);
    if (i < 0 || i >= (Int)t->nfield)
        panic_str(BURROW_S("reflect: Field index out of range"));
    return burrow__reflect_field(t, i);
}

ReflectStructField reflect_type_field_by_index(ReflectType t, Slice index) {
    if (t->kind != KIND_STRUCT)
        rf_panic_type("reflect: FieldByIndex of non-struct type ", t);
    const Int *ix = index.p;
    ReflectStructField f;
    memset(&f, 0, sizeof f);
    f.type = t;
    for (Int i = 0; i < index.len; i++) {
        const Type *ft = f.type;
        if (i > 0 && ft->kind == KIND_POINTER && ft->elem != NULL &&
            ft->elem->kind == KIND_STRUCT)
            ft = ft->elem;
        f = reflect_type_field(ft, ix[i]);
    }
    return f;
}

/* Go's selector search, breadth first over embedded structs. Every list here
 * is scratch on the heap; only the answer's index goes to the caller's Alloc. */
typedef struct RfScan {
    const Type *t;
    Int *index;
    Int depth;
} RfScan;

typedef struct RfCount {
    const Type *t;
    int n;
} RfCount;

typedef struct RfVec {
    void *p;
    Int len;
    Int cap;
} RfVec;

/* Room for one more element of size bytes, or false. */
static bool rf_vec_reserve(RfVec *v, size_t size) {
    if (v->len < v->cap)
        return true;
    Int nc = v->cap < 8 ? 8 : v->cap * 2;
    void *np = mem_realloc(heap_allocator(), v->p, (size_t)v->cap * size,
                           (size_t)nc * size, 16);
    if (np == NULL)
        return false;
    v->p = np;
    v->cap = nc;
    return true;
}

static void rf_vec_free(RfVec *v, size_t size) {
    mem_free(heap_allocator(), v->p, (size_t)v->cap * size, 16);
    *v = (RfVec){0};
}

static void rf_scans_clear(RfVec *v) {
    RfScan *s = v->p;
    for (Int i = 0; i < v->len; i++)
        mem_free(heap_allocator(), s[i].index, (size_t)s[i].depth * sizeof(Int), 16);
    v->len = 0;
}

static int rf_count_of(const RfVec *v, const Type *t) {
    const RfCount *c = v->p;
    for (Int i = 0; i < v->len; i++)
        if (c[i].t == t)
            return c[i].n;
    return 0;
}

static bool rf_contains(const RfVec *v, const Type *t) {
    const Type *const *p = v->p;
    for (Int i = 0; i < v->len; i++)
        if (p[i] == t)
            return true;
    return false;
}

typedef struct RfSearch {
    RfVec cur, next;         /* RfScan */
    RfVec count, next_count; /* RfCount */
    RfVec visited;           /* const Type * */
} RfSearch;

static void rf_search_free(RfSearch *s) {
    rf_scans_clear(&s->cur);
    rf_scans_clear(&s->next);
    rf_vec_free(&s->cur, sizeof(RfScan));
    rf_vec_free(&s->next, sizeof(RfScan));
    rf_vec_free(&s->count, sizeof(RfCount));
    rf_vec_free(&s->next_count, sizeof(RfCount));
    rf_vec_free(&s->visited, sizeof(const Type *));
}

BURROW_NORETURN static void rf_search_oom(RfSearch *s) {
    rf_search_free(s);
    panic_str(rf_oom);
}

/* Matches by fn when there is one, else by name. */
static bool rf_field_by_name(Alloc *a, const Type *t, Str name,
                             const ReflectNameMatch *fn, ReflectStructField *out) {
    RfSearch s;
    memset(&s, 0, sizeof s);
    if (!rf_vec_reserve(&s.next, sizeof(RfScan)))
        rf_search_oom(&s);
    ((RfScan *)s.next.p)[s.next.len++] = (RfScan){t, NULL, 0};

    bool ok = false;
    ReflectStructField result;
    memset(&result, 0, sizeof result);
    while (s.next.len > 0) {
        /* current, next = next, current[:0], and the same for the counts. */
        rf_scans_clear(&s.cur);
        RfVec tmp = s.cur;
        s.cur = s.next;
        s.next = tmp;
        tmp = s.count;
        s.count = s.next_count;
        s.next_count = tmp;
        s.next_count.len = 0;

        for (Int si = 0; si < s.cur.len; si++) {
            RfScan scan = ((RfScan *)s.cur.p)[si];
            const Type *st = scan.t;
            if (rf_contains(&s.visited, st))
                continue;
            if (!rf_vec_reserve(&s.visited, sizeof(const Type *)))
                rf_search_oom(&s);
            ((const Type **)s.visited.p)[s.visited.len++] = st;

            for (uint16_t i = 0; i < st->nfield; i++) {
                const Field *f = &st->fields[i];
                const Type *ntyp = NULL;
                if (burrow__reflect_embedded(f)) {
                    ntyp = f->type;
                    if (ntyp->kind == KIND_POINTER)
                        ntyp = ntyp->elem;
                }
                bool m = fn != NULL ? fn->f(fn->env, f->name) : str_eq(f->name, name);
                if (m) {
                    if (rf_count_of(&s.count, st) > 1 || ok) {
                        /* The name is there more than once at this depth, and
                         * those cancel out. */
                        rf_search_free(&s);
                        return false;
                    }
                    result = burrow__reflect_field(st, (Int)i);
                    if (scan.depth > 0) {
                        Int n = scan.depth + 1;
                        Int *ix = BURROW_NEW_N(a, Int, (size_t)n);
                        if (ix == NULL)
                            rf_search_oom(&s);
                        memcpy(ix, scan.index, (size_t)scan.depth * sizeof(Int));
                        ix[scan.depth] = (Int)i;
                        result.index = (Slice){ix, n, n, TYPE_INT};
                    }
                    ok = true;
                    continue;
                }
                if (ok || ntyp == NULL || ntyp->kind != KIND_STRUCT)
                    continue;
                RfCount *nc = s.next_count.p;
                bool again = false;
                for (Int k = 0; k < s.next_count.len; k++)
                    if (nc[k].t == ntyp) {
                        nc[k].n = 2;
                        again = true;
                    }
                if (again)
                    continue;
                if (!rf_vec_reserve(&s.next_count, sizeof(RfCount)) ||
                    !rf_vec_reserve(&s.next, sizeof(RfScan)))
                    rf_search_oom(&s);
                ((RfCount *)s.next_count.p)[s.next_count.len++] =
                    (RfCount){ntyp, rf_count_of(&s.count, st) > 1 ? 2 : 1};
                Int *index =
                    BURROW_NEW_N(heap_allocator(), Int, (size_t)scan.depth + 1);
                if (index == NULL)
                    rf_search_oom(&s);
                if (scan.depth > 0)
                    memcpy(index, scan.index, (size_t)scan.depth * sizeof(Int));
                index[scan.depth] = (Int)i;
                ((RfScan *)s.next.p)[s.next.len++] =
                    (RfScan){ntyp, index, scan.depth + 1};
            }
        }
        if (ok)
            break;
    }
    if (ok)
        *out = result;
    rf_search_free(&s);
    return ok;
}

bool reflect_type_field_by_name(Alloc *a, ReflectType t, Str name,
                                ReflectStructField *f) {
    if (t->kind != KIND_STRUCT)
        rf_panic_type("reflect: FieldByName of non-struct type ", t);
    /* Quick check for a top level name, or a struct with no embedded fields. */
    bool has_embeds = false;
    if (name.len > 0) {
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *tf = &t->fields[i];
            if (str_eq(tf->name, name)) {
                *f = burrow__reflect_field(t, (Int)i);
                return true;
            }
            if (burrow__reflect_embedded(tf))
                has_embeds = true;
        }
    }
    if (!has_embeds)
        return false;
    return rf_field_by_name(a, t, name, NULL, f);
}

bool reflect_type_field_by_name_func(Alloc *a, ReflectType t, ReflectNameMatch match,
                                     ReflectStructField *f) {
    if (t->kind != KIND_STRUCT)
        rf_panic_type("reflect: FieldByNameFunc of non-struct type ", t);
    return rf_field_by_name(a, t, BURROW_STR_EMPTY, &match, f);
}

/* ---------------------------------------------------------- visible fields */

typedef struct RfVisField {
    ReflectStructField f;
    bool hidden;
} RfVisField;

typedef struct RfByName {
    Str name;
    Int at;
} RfByName;

typedef struct RfVisible {
    RfVec fields;   /* RfVisField, each index on the heap */
    RfVec by_name;  /* RfByName */
    RfVec index;    /* Int, the path to where the walk is */
    RfVec visiting; /* const Type * */
} RfVisible;

static void rf_visible_free(RfVisible *w) {
    RfVisField *vf = w->fields.p;
    for (Int i = 0; i < w->fields.len; i++)
        mem_free(heap_allocator(), vf[i].f.index.p,
                 (size_t)vf[i].f.index.len * sizeof(Int), 16);
    rf_vec_free(&w->fields, sizeof(RfVisField));
    rf_vec_free(&w->by_name, sizeof(RfByName));
    rf_vec_free(&w->index, sizeof(Int));
    rf_vec_free(&w->visiting, sizeof(const Type *));
}

BURROW_NORETURN static void rf_visible_oom(RfVisible *w) {
    rf_visible_free(w);
    panic_str(rf_oom);
}

static void rf_visible_walk(RfVisible *w, const Type *t) {
    if (rf_contains(&w->visiting, t))
        return;
    if (!rf_vec_reserve(&w->visiting, sizeof(const Type *)))
        rf_visible_oom(w);
    ((const Type **)w->visiting.p)[w->visiting.len++] = t;
    for (uint16_t i = 0; i < t->nfield; i++) {
        ReflectStructField f = burrow__reflect_field(t, (Int)i);
        if (!rf_vec_reserve(&w->index, sizeof(Int)))
            rf_visible_oom(w);
        ((Int *)w->index.p)[w->index.len++] = (Int)i;
        Int depth = w->index.len;

        bool add = true;
        RfByName *bn = NULL;
        for (Int k = 0; k < w->by_name.len; k++)
            if (str_eq(((RfByName *)w->by_name.p)[k].name, f.name))
                bn = &((RfByName *)w->by_name.p)[k];
        if (bn != NULL) {
            RfVisField *old = &((RfVisField *)w->fields.p)[bn->at];
            if (depth == old->f.index.len) {
                /* The same name at the same depth: the two cancel out. */
                old->hidden = true;
                add = false;
            } else if (depth < old->f.index.len) {
                /* The old field is deeper and loses. */
                old->hidden = true;
            } else {
                /* The old field is shallower and wins. */
                add = false;
            }
        }
        if (add) {
            if (!rf_vec_reserve(&w->fields, sizeof(RfVisField)))
                rf_visible_oom(w);
            if (bn == NULL) {
                if (!rf_vec_reserve(&w->by_name, sizeof(RfByName)))
                    rf_visible_oom(w);
                bn = &((RfByName *)w->by_name.p)[w->by_name.len++];
                bn->name = f.name;
            }
            Int *ix = BURROW_NEW_N(heap_allocator(), Int, (size_t)depth);
            if (ix == NULL)
                rf_visible_oom(w);
            memcpy(ix, w->index.p, (size_t)depth * sizeof(Int));
            f.index = (Slice){ix, depth, depth, TYPE_INT};
            bn->at = w->fields.len;
            ((RfVisField *)w->fields.p)[w->fields.len++] = (RfVisField){f, false};
        }
        if (f.anonymous) {
            const Type *ft = f.type;
            if (ft->kind == KIND_POINTER)
                ft = ft->elem;
            if (ft != NULL && ft->kind == KIND_STRUCT)
                rf_visible_walk(w, ft);
        }
        w->index.len--;
    }
    w->visiting.len--;
}

Slice reflect_visible_fields(Alloc *a, ReflectType t) {
    if (t == NULL)
        panic_str(BURROW_S("reflect: VisibleFields(nil)"));
    if (t->kind != KIND_STRUCT)
        panic_str(BURROW_S("reflect.VisibleFields of non-struct type"));
    RfVisible w;
    memset(&w, 0, sizeof w);
    rf_visible_walk(&w, t);

    RfVisField *vf = w.fields.p;
    Int kept = 0;
    Int ints = 0;
    for (Int i = 0; i < w.fields.len; i++)
        if (!vf[i].hidden) {
            kept++;
            ints += vf[i].f.index.len;
        }
    if (kept == 0) {
        rf_visible_free(&w);
        return slice_nil(TYPE_REFLECT_STRUCT_FIELD);
    }
    /* The fields and all their indexes in one block, freed as one. */
    size_t head = (size_t)kept * sizeof(ReflectStructField);
    ReflectStructField *out = mem_alloc(a, head + (size_t)ints * sizeof(Int), 16);
    if (out == NULL)
        rf_visible_oom(&w);
    Int *ix = (Int *)(void *)((Byte *)out + head);
    Int j = 0;
    for (Int i = 0; i < w.fields.len; i++) {
        if (vf[i].hidden)
            continue;
        Int n = vf[i].f.index.len;
        memcpy(ix, vf[i].f.index.p, (size_t)n * sizeof(Int));
        out[j] = vf[i].f;
        out[j].index = (Slice){ix, n, n, TYPE_INT};
        ix += n;
        j++;
    }
    rf_visible_free(&w);
    return (Slice){out, kept, kept, TYPE_REFLECT_STRUCT_FIELD};
}

/* ------------------------------------------------------------------- tags */

Str reflect_struct_tag_get(ReflectStructTag tag, Alloc *a, Str key) {
    return tag_get(a, tag, key);
}

bool reflect_struct_tag_lookup(ReflectStructTag tag, Alloc *a, Str key, Str *value) {
    return tag_lookup(a, tag, key, value);
}
