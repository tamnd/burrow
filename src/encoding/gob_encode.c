/* Derived from Go's src/encoding/gob/encoder.go and encode.go.
 * Go source: go1.27.1.
 *
 * Go compiles a program of operations for each type and runs it over values.
 * This walks the value and its descriptor together instead, making the same
 * decisions in the same order, so the bytes come out the same. The checks Go
 * makes while compiling are in gob.c and run once per type before any value of
 * it is written, which is when Go makes them.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/gob.h"

#include "gob_internal.h"

#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/math/bits.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/sync.h"

#include <string.h>

/* maxLength: the room left at the front of a message for its length. */
#define GOB_MAX_LENGTH 9

/* No encInstr, which is what the elements of arrays and maps have. */
#define GOB_NO_INSTR INT32_MIN

typedef struct GobEncBuf {
    Byte *p;
    Int len;
    Int cap;
} GobEncBuf;

/* An io.Writer, or while an interface value is being written, the buffer of
 * the message it is part of. */
typedef struct GobOut {
    IoWriter w;
    GobEncBuf *b;
} GobOut;

struct GobEncoder {
    Alloc *a;
    SyncMutex mu;
    GobOut *w;
    Int nw;
    Int capw;
    Map *sent;
    GobEncBuf byte_buf;
    Error err;
    int depth;
    uintptr_t stack_floor;
};

/* encoderState. */
typedef struct GobEncState {
    GobEncoder *e;
    GobEncBuf *b;
    bool send_zero;
    int32_t fieldnum;
} GobEncState;

/* A value and its type. p is NULL for reflect's invalid Value. */
typedef struct GobVal {
    const Type *t;
    void *p;
} GobVal;

static bool gob_failed(const GobEncoder *e) {
    return BURROW_FAILED(e->err);
}

static void gob_set_error(GobEncoder *e, Error err) {
    if (BURROW_OK(e->err))
        e->err = err;
}

/* ----------------------------------------------------------------- buffers */

static bool gob_buf_grow(GobEncoder *e, GobEncBuf *b, Int n) {
    if (b->len + n <= b->cap)
        return true;
    Int nc = b->cap < 64 ? 64 : b->cap;
    while (nc < b->len + n) {
        if (nc > INTPTR_MAX / 2) {
            gob_set_error(e, burrow_err_out_of_memory);
            return false;
        }
        nc *= 2;
    }
    Byte *p = (Byte *)mem_realloc(e->a, b->p, (size_t)b->cap, (size_t)nc, 1);
    if (p == NULL) {
        gob_set_error(e, burrow_err_out_of_memory);
        return false;
    }
    b->p = p;
    b->cap = nc;
    return true;
}

static void gob_buf_write(GobEncoder *e, GobEncBuf *b, const void *p, Int n) {
    if (n <= 0 || (b->cap - b->len < n && !gob_buf_grow(e, b, n)))
        return;
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

/* Reset and Write(spaceForLength). */
static void gob_buf_reset(GobEncoder *e, GobEncBuf *b) {
    b->len = 0;
    if (gob_buf_grow(e, b, GOB_MAX_LENGTH)) {
        memset(b->p, 0, GOB_MAX_LENGTH);
        b->len = GOB_MAX_LENGTH;
    }
}

static void gob_buf_free(GobEncoder *e, GobEncBuf *b) {
    mem_free(e->a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---------------------------------------------------------------- numbers */

static Int gob_put_uint(Byte out[GOB_MAX_LENGTH], uint64_t x) {
    if (x <= 0x7F) {
        out[0] = (Byte)x;
        return 1;
    }
    /* The byte count, negated, then the bytes big-endian. */
    int n = 8 - (int)(bits_leading_zeros64(x) / 8);
    out[0] = (Byte)(256 - n);
    for (int i = n; i >= 1; i--) {
        out[i] = (Byte)x;
        x >>= 8;
    }
    return n + 1;
}

static void gob_enc_uint(GobEncState *st, uint64_t x) {
    GobEncBuf *b = st->b;
    if (b->cap - b->len < GOB_MAX_LENGTH && !gob_buf_grow(st->e, b, GOB_MAX_LENGTH))
        return;
    if (x <= 0x7F) {
        b->p[b->len++] = (Byte)x;
        return;
    }
    b->len += gob_put_uint(b->p + b->len, x);
}

static void gob_enc_int(GobEncState *st, int64_t i) {
    uint64_t x;
    if (i < 0)
        x = ((uint64_t)~i << 1) | 1;
    else
        x = (uint64_t)i << 1;
    gob_enc_uint(st, x);
}

/* floatBits: the bits reversed, so that the exponent comes first and small
 * integers take few bytes. */
static uint64_t gob_float_bits(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof u);
    return bits_reverse_bytes64(u);
}

static void gob_update(GobEncState *st, int32_t field) {
    if (field == GOB_NO_INSTR)
        return;
    gob_enc_uint(st, (uint64_t)(int64_t)(field - st->fieldnum));
    st->fieldnum = field;
}

static int64_t gob_load_int(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const int8_t *)p;
    case 2:
        return *(const int16_t *)p;
    case 4:
        return *(const int32_t *)p;
    default:
        return *(const int64_t *)p;
    }
}

static uint64_t gob_load_uint(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const uint8_t *)p;
    case 2:
        return *(const uint16_t *)p;
    case 4:
        return *(const uint32_t *)p;
    default:
        return *(const uint64_t *)p;
    }
}

/* --------------------------------------------------------------- messages */

static void gob_out_write(GobEncoder *e, GobOut w, const Byte *p, Int n) {
    if (w.b != NULL) {
        gob_buf_write(e, w.b, p, n);
        return;
    }
    Error err = BURROW_NO_ERROR;
    Slice s = slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
    Int m = w.w.vt->write(w.w.data, s, &err);
    if (BURROW_OK(err) && m < n)
        err = io_err_short_write;
    if (BURROW_FAILED(err))
        gob_set_error(e, err);
}

/* writeMessage. */
static void gob_write_message(GobEncoder *e, GobOut w, GobEncBuf *b) {
    if (b->len < GOB_MAX_LENGTH)
        return;
    uint64_t n = (uint64_t)(b->len - GOB_MAX_LENGTH);
    if (n >= GOB_TOO_BIG) {
        gob_set_error(e, errors_new(error_allocator(),
                                    BURROW_S("gob: encoder: message too big")));
        return;
    }
    Byte count[GOB_MAX_LENGTH];
    Int c = gob_put_uint(count, n);
    Int offset = GOB_MAX_LENGTH - c;
    memcpy(b->p + offset, count, (size_t)c);
    Error before = e->err;
    e->err = BURROW_NO_ERROR;
    gob_out_write(e, w, b->p + offset, b->len - offset);
    Error werr = e->err;
    e->err = before;
    gob_buf_reset(e, b);
    if (BURROW_FAILED(werr))
        gob_set_error(e, werr);
}

static GobOut gob_writer(const GobEncoder *e) {
    return e->w[e->nw - 1];
}

static bool gob_push_writer(GobEncoder *e, GobOut w) {
    if (e->nw == e->capw) {
        Int nc = e->capw * 2;
        GobOut *n = (GobOut *)mem_realloc(e->a, e->w, (size_t)e->capw * sizeof *n,
                                          (size_t)nc * sizeof *n, _Alignof(GobOut));
        if (n == NULL) {
            gob_set_error(e, burrow_err_out_of_memory);
            return false;
        }
        e->w = n;
        e->capw = nc;
    }
    e->w[e->nw++] = w;
    return true;
}

/* ---------------------------------------------------------------- sent map */

static bool gob_sent_get(GobEncoder *e, const Type *t, int32_t *id) {
    uintptr_t k = (uintptr_t)burrow__gob_canon(t);
    const int32_t *v = (const int32_t *)map_get(e->sent, &k);
    if (v == NULL)
        return false;
    if (id != NULL)
        *id = *v;
    return true;
}

static void gob_sent_set(GobEncoder *e, const Type *t, int32_t id) {
    uintptr_t k = (uintptr_t)burrow__gob_canon(t);
    if (!map_set(e->sent, &k, &id))
        gob_set_error(e, burrow_err_out_of_memory);
}

/* --------------------------------------------------------------- wireType */

/* CommonType and fieldType, which are the same shape on the wire. */
static void gob_enc_name_id(GobEncoder *e, GobEncBuf *b, Str name, int32_t id) {
    GobEncState st = {e, b, false, -1};
    if (name.len > 0) {
        gob_update(&st, 0);
        gob_enc_uint(&st, (uint64_t)name.len);
        gob_buf_write(e, b, name.p, name.len);
    }
    if (id != 0) {
        gob_update(&st, 1);
        gob_enc_int(&st, id);
    }
    gob_enc_uint(&st, 0);
}

static void gob_enc_wire_part(GobEncoder *e, GobEncBuf *b, int slot, const GobType *t) {
    GobEncState st = {e, b, false, -1};
    gob_update(&st, 0);
    gob_enc_name_id(e, b, t->name, t->id);
    switch (slot) {
    case GOB_W_ARRAY:
        if (t->elem != 0) {
            gob_update(&st, 1);
            gob_enc_int(&st, t->elem);
        }
        if (t->len != 0) {
            gob_update(&st, 2);
            gob_enc_int(&st, t->len);
        }
        break;
    case GOB_W_SLICE:
        if (t->elem != 0) {
            gob_update(&st, 1);
            gob_enc_int(&st, t->elem);
        }
        break;
    case GOB_W_STRUCT:
        if (t->nfield > 0) {
            gob_update(&st, 1);
            GobEncState as = {e, b, true, -1};
            gob_enc_uint(&as, (uint64_t)t->nfield);
            for (Int i = 0; i < t->nfield; i++)
                gob_enc_name_id(e, b, t->field[i].name, t->field[i].id);
        }
        break;
    case GOB_W_MAP:
        if (t->key != 0) {
            gob_update(&st, 1);
            gob_enc_int(&st, t->key);
        }
        if (t->elem != 0) {
            gob_update(&st, 2);
            gob_enc_int(&st, t->elem);
        }
        break;
    default:
        break;
    }
    gob_enc_uint(&st, 0);
}

static void gob_enc_wire(GobEncoder *e, GobEncBuf *b, const GobWireType *w) {
    GobEncState st = {e, b, false, -1};
    for (int k = 0; k < GOB_W_COUNT; k++) {
        if (w->t[k] == NULL)
            continue;
        gob_update(&st, k);
        gob_enc_wire_part(e, b, k, w->t[k]);
    }
    gob_enc_uint(&st, 0);
}

/* ------------------------------------------------------------ descriptors */

static bool gob_send_type(GobEncoder *e, GobOut w, GobEncState *st, const Type *origt);

static bool gob_send_actual_type(GobEncoder *e, GobOut w, GobEncState *st,
                                 const GobUserType *ut, const Type *actual) {
    if (gob_sent_get(e, actual, NULL))
        return false;
    Error err = BURROW_NO_ERROR;
    const GobTypeInfo *info = burrow__gob_type_info(ut, &err);
    if (info == NULL) {
        gob_set_error(e, err);
        return false;
    }
    gob_enc_int(st, -(int64_t)info->id);
    gob_enc_wire(e, st->b, &info->wire);
    gob_write_message(e, w, st->b);
    if (gob_failed(e))
        return false;
    gob_sent_set(e, ut->base, info->id);
    if (ut->user != ut->base)
        gob_sent_set(e, ut->user, info->id);
    switch ((int)actual->kind) {
    case KIND_STRUCT:
        for (Int i = 0; i < actual->nfield; i++)
            if (burrow__gob_is_exported(actual->fields[i].name))
                gob_send_type(e, w, st, actual->fields[i].type);
        break;
    case KIND_ARRAY:
    case KIND_SLICE:
        gob_send_type(e, w, st, actual->elem);
        break;
    case KIND_MAP:
        gob_send_type(e, w, st, actual->key);
        gob_send_type(e, w, st, actual->elem);
        break;
    default:
        break;
    }
    return true;
}

static bool gob_send_type(GobEncoder *e, GobOut w, GobEncState *st, const Type *origt) {
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = burrow__gob_user_type(origt, &err);
    if (ut == NULL) {
        gob_set_error(e, err);
        return false;
    }
    if (ut->external_enc != 0)
        return gob_send_actual_type(e, w, st, ut, ut->base);
    const Type *rt = ut->base;
    switch ((int)rt->kind) {
    case KIND_SLICE:
        if (rt->elem->kind == KIND_UINT8)
            return false;
        break;
    case KIND_ARRAY:
    case KIND_MAP:
    case KIND_STRUCT:
        break;
    default:
        return false;
    }
    return gob_send_actual_type(e, w, st, ut, ut->base);
}

static void gob_send_type_descriptor(GobEncoder *e, GobOut w, GobEncState *st,
                                     const GobUserType *ut) {
    const Type *rt = ut->external_enc != 0 ? ut->user : ut->base;
    if (gob_sent_get(e, rt, NULL))
        return;
    bool sent = gob_send_type(e, w, st, rt);
    if (gob_failed(e))
        return;
    if (!sent) {
        Error err = BURROW_NO_ERROR;
        const GobTypeInfo *info = burrow__gob_type_info(ut, &err);
        if (info == NULL) {
            gob_set_error(e, err);
            return;
        }
        gob_sent_set(e, rt, info->id);
    }
}

static void gob_send_type_id(GobEncoder *e, GobEncState *st, const GobUserType *ut) {
    int32_t id = 0;
    (void)gob_sent_get(e, ut->base, &id);
    gob_enc_int(st, id);
}

/* ------------------------------------------------------------------ values */

static int gob_op_indir(const GobUserType *ut) {
    return ut->external_enc != 0 ? ut->enc_indir : ut->indir;
}

/* encIndirect followed by valid(): NULL when a pointer on the way, or the
 * one arrived at, is nil. */
static GobVal gob_enc_indirect(GobVal v, int n) {
    for (int i = 0; i < n; i++) {
        void *q = *(void **)v.p;
        if (q == NULL)
            return (GobVal){v.t, NULL};
        v.t = v.t->elem;
        v.p = q;
    }
    if (v.t->kind == KIND_POINTER && *(void **)v.p == NULL)
        return (GobVal){v.t, NULL};
    return v;
}

static bool gob_is_zero(const Type *t, const void *p);

static bool gob_is_zero_bytes(const void *p, size_t n) {
    const Byte *b = (const Byte *)p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return false;
    return true;
}

/* reflect.Value.IsZero, which a float is zero for only when all of its bits
 * are. */
static bool gob_is_zero(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        return !*(const bool *)p;
    case KIND_STRING:
        return ((const Str *)p)->len == 0;
    case KIND_SLICE:
        return ((const Slice *)p)->p == NULL;
    case KIND_INTERFACE:
        if (t == TYPE_ANY)
            return ((const Any *)p)->t == NULL;
        return *(void *const *)p == NULL;
    case KIND_ARRAY:
        for (uint32_t i = 0; i < t->len; i++)
            if (!gob_is_zero(t->elem, (const Byte *)p + (size_t)i * t->elem->size))
                return false;
        return true;
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (str_eq(f->name, BURROW_S("_")))
                continue;
            if (!gob_is_zero(f->type, (const Byte *)p + f->offset))
                return false;
        }
        return true;
    default:
        return gob_is_zero_bytes(p, t->size);
    }
}

static void gob_encode_struct(GobEncoder *e, GobEncBuf *b, const GobUserType *sut,
                              void *p);
static void gob_encode_interface(GobEncoder *e, GobEncBuf *b, const Type *t, void *p);
static void gob_encode_array(GobEncoder *e, GobEncBuf *b, const GobUserType *aut,
                             void *p, Int n);
static void gob_encode_map(GobEncoder *e, GobEncBuf *b, const Type *t, Map *m);

static bool gob_enc_enter(GobEncoder *e) {
    if (burrow__gob_stack_low(&e->stack_floor, e->depth)) {
        gob_set_error(e, burrow__gob_errorf("encoder: nesting too deep"));
        return false;
    }
    e->depth++;
    return true;
}

static void gob_encode_gob_encoder(GobEncoder *e, GobEncBuf *b, const GobUserType *ut,
                                   GobVal v) {
    while (v.t != ut->base && v.t->kind == KIND_POINTER) {
        void *q = *(void **)v.p;
        if (q == NULL)
            panic_str(fmt_sprintf_v(
                error_allocator(), "value method %s called using nil %s pointer",
                ut->enc_method->name, burrow__gob_type_string(error_allocator(), v.t)));
        v.t = v.t->elem;
        v.p = q;
    }
    Slice data = {NULL, 0, 0, TYPE_BYTE};
    Error merr = BURROW_NO_ERROR;
    EncodingAllocArg aa = e->a;
    EncodingErrorArg ea = &merr;
    void *args[2] = {&aa, &ea};
    void *rets[1] = {&data};
    method_call(ut->enc_method, v.p, args, rets);
    if (BURROW_FAILED(merr)) {
        gob_set_error(e, merr);
        return;
    }
    GobEncState st = {e, b, false, -1};
    gob_enc_uint(&st, (uint64_t)data.len);
    gob_buf_write(e, b, data.p, data.len);
    mem_free(e->a, data.p, (size_t)data.cap, 1);
}

/* The op for ut, run on v, which is at the level the op wants it: the base
 * type, or for a type with its own encoding, enc_indir pointers down. */
static void gob_op(GobEncState *st, int32_t field, const GobUserType *ut, GobVal v) {
    GobEncoder *e = st->e;
    if (ut->external_enc != 0) {
        bool zero =
            v.t->kind == KIND_POINTER ? *(void **)v.p == NULL : gob_is_zero(v.t, v.p);
        if (!st->send_zero && zero)
            return;
        gob_update(st, field);
        gob_encode_gob_encoder(e, st->b, ut, v);
        return;
    }
    const Type *t = ut->base;
    void *p = v.p;
    switch ((int)t->kind) {
    case KIND_BOOL: {
        bool x = *(const bool *)p;
        if (x || st->send_zero) {
            gob_update(st, field);
            gob_enc_uint(st, x ? 1 : 0);
        }
        return;
    }
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64: {
        int64_t x = gob_load_int(t, p);
        if (x != 0 || st->send_zero) {
            gob_update(st, field);
            gob_enc_int(st, x);
        }
        return;
    }
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR: {
        uint64_t x = gob_load_uint(t, p);
        if (x != 0 || st->send_zero) {
            gob_update(st, field);
            gob_enc_uint(st, x);
        }
        return;
    }
    case KIND_FLOAT32:
    case KIND_FLOAT64: {
        double f =
            t->kind == KIND_FLOAT32 ? (double)*(const float *)p : *(const double *)p;
        if (f != 0 || st->send_zero) {
            gob_update(st, field);
            gob_enc_uint(st, gob_float_bits(f));
        }
        return;
    }
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        double re;
        double im;
        if (t->kind == KIND_COMPLEX64) {
            re = (double)((const Complex64 *)p)->re;
            im = (double)((const Complex64 *)p)->im;
        } else {
            re = ((const Complex128 *)p)->re;
            im = ((const Complex128 *)p)->im;
        }
        if (re != 0 || im != 0 || st->send_zero) {
            gob_update(st, field);
            gob_enc_uint(st, gob_float_bits(re));
            gob_enc_uint(st, gob_float_bits(im));
        }
        return;
    }
    case KIND_STRING: {
        Str s = *(const Str *)p;
        if (s.len > 0 || st->send_zero) {
            gob_update(st, field);
            gob_enc_uint(st, (uint64_t)s.len);
            gob_buf_write(e, st->b, s.p, s.len);
        }
        return;
    }
    case KIND_SLICE: {
        const Slice *s = (const Slice *)p;
        if (t->elem->kind == KIND_UINT8) {
            if (s->len > 0 || st->send_zero) {
                gob_update(st, field);
                gob_enc_uint(st, (uint64_t)s->len);
                gob_buf_write(e, st->b, s->p, s->len);
            }
            return;
        }
        if (!st->send_zero && s->len == 0)
            return;
        gob_update(st, field);
        gob_encode_array(e, st->b, ut, s->p, s->len);
        return;
    }
    case KIND_ARRAY:
        gob_update(st, field);
        gob_encode_array(e, st->b, ut, p, (Int)t->len);
        return;
    case KIND_MAP: {
        Map *m = *(Map **)p;
        if (!st->send_zero && m == NULL)
            return;
        gob_update(st, field);
        gob_encode_map(e, st->b, t, m);
        return;
    }
    case KIND_STRUCT:
        gob_update(st, field);
        gob_encode_struct(e, st->b, ut, p);
        return;
    case KIND_INTERFACE: {
        void *vp = NULL;
        if (!st->send_zero && burrow__gob_iface_elem(t, p, &vp) == NULL)
            return;
        gob_update(st, field);
        gob_encode_interface(e, st->b, t, p);
        return;
    }
    default:
        gob_set_error(
            e, burrow__gob_errorf("can't happen: encode type %s",
                                  burrow__gob_type_string(error_allocator(), t)));
        return;
    }
}

static void gob_encode_struct(GobEncoder *e, GobEncBuf *b, const GobUserType *sut,
                              void *p) {
    Error err = BURROW_NO_ERROR;
    const GobEncPlan *plan = burrow__gob_enc_plan(sut, &err);
    if (plan == NULL) {
        gob_set_error(e, err);
        return;
    }
    if (!gob_enc_enter(e))
        return;
    GobEncState st = {e, b, false, -1};
    for (Int i = 0; i < plan->n && !gob_failed(e); i++) {
        const GobEncField *f = &plan->f[i];
        GobVal v = {f->type, (Byte *)p + f->offset};
        int indir = gob_op_indir(f->ut);
        if (indir > 0) {
            v = gob_enc_indirect(v, indir);
            if (v.p == NULL)
                continue;
        }
        gob_op(&st, f->wire, f->ut, v);
    }
    if (!gob_failed(e))
        gob_enc_uint(&st, 0);
    e->depth--;
}

static void gob_encode_array(GobEncoder *e, GobEncBuf *b, const GobUserType *aut,
                             void *p, Int n) {
    if (!gob_enc_enter(e))
        return;
    GobEncState st = {e, b, true, -1};
    gob_enc_uint(&st, (uint64_t)n);
    const Type *elem = aut->base->elem;
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = burrow__gob_elem_ut(aut, &err);
    if (ut == NULL) {
        gob_set_error(e, err);
        e->depth--;
        return;
    }
    int indir = gob_op_indir(ut);
    for (Int i = 0; i < n && !gob_failed(e); i++) {
        GobVal v = {elem, (Byte *)p + (size_t)i * elem->size};
        if (indir > 0) {
            v = gob_enc_indirect(v, indir);
            if (v.p == NULL) {
                gob_set_error(e, burrow__gob_errorf("encodeArray: nil element"));
                break;
            }
        }
        gob_op(&st, GOB_NO_INSTR, ut, v);
    }
    e->depth--;
}

/* encodeReflectValue. */
static void gob_encode_reflect_value(GobEncState *st, GobVal v, const GobUserType *ut) {
    int indir = gob_op_indir(ut);
    for (int i = 0; i < indir; i++) {
        void *q = *(void **)v.p;
        if (q == NULL) {
            gob_set_error(st->e, burrow__gob_errorf("encodeReflectValue: nil element"));
            return;
        }
        v.t = v.t->elem;
        v.p = q;
    }
    gob_op(st, GOB_NO_INSTR, ut, v);
}

static void gob_encode_map(GobEncoder *e, GobEncBuf *b, const Type *t, Map *m) {
    if (!gob_enc_enter(e))
        return;
    GobEncState st = {e, b, true, -1};
    gob_enc_uint(&st, (uint64_t)(m != NULL ? map_len(m) : 0));
    Error err = BURROW_NO_ERROR;
    const GobUserType *kut = burrow__gob_user_type(t->key, &err);
    const GobUserType *vut = kut != NULL ? burrow__gob_user_type(t->elem, &err) : NULL;
    if (vut == NULL) {
        gob_set_error(e, err);
        e->depth--;
        return;
    }
    if (m != NULL) {
        const void *k = NULL;
        void *val = NULL;
        for (MapIter it = map_iter(m); map_next(&it, &k, &val) && !gob_failed(e);) {
            gob_encode_reflect_value(&st, (GobVal){t->key, (void *)(uintptr_t)k}, kut);
            if (gob_failed(e))
                break;
            gob_encode_reflect_value(&st, (GobVal){t->elem, val}, vut);
        }
    }
    e->depth--;
}

static void gob_encode(GobEncoder *e, GobEncBuf *b, GobVal v, const GobUserType *ut);

static void gob_encode_interface(GobEncoder *e, GobEncBuf *b, const Type *t, void *p) {
    void *vp = NULL;
    const Type *et = burrow__gob_iface_elem(t, p, &vp);
    if (et != NULL && et->kind == KIND_POINTER && *(void **)vp == NULL) {
        gob_set_error(e,
                      burrow__gob_errorf(
                          "gob: cannot encode nil pointer of type %s inside interface",
                          burrow__gob_type_string(error_allocator(), et)));
        return;
    }
    GobEncState st = {e, b, true, -1};
    if (et == NULL) {
        gob_enc_uint(&st, 0);
        return;
    }
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = burrow__gob_user_type(et, &err);
    if (ut == NULL) {
        gob_set_error(e, err);
        return;
    }
    Str name = {NULL, 0};
    if (!burrow__gob_name_of(ut->base, &name)) {
        gob_set_error(e, burrow__gob_errorf(
                             "type not registered for interface: %s",
                             burrow__gob_type_string(error_allocator(), ut->base)));
        return;
    }
    gob_enc_uint(&st, (uint64_t)name.len);
    gob_buf_write(e, b, name.p, name.len);
    /* The descriptor goes to the writer the enclosing message is going to,
     * after the part of it written so far, which is what Go does. */
    gob_send_type_descriptor(e, gob_writer(e), &st, ut);
    if (gob_failed(e))
        return;
    gob_send_type_id(e, &st, ut);
    if (!gob_push_writer(e, (GobOut){{NULL, NULL}, b}))
        return;
    GobEncBuf data = {NULL, 0, 0};
    gob_buf_reset(e, &data);
    gob_encode(e, &data, (GobVal){et, vp}, ut);
    if (!gob_failed(e)) {
        e->nw--;
        gob_write_message(e, (GobOut){{NULL, NULL}, b}, &data);
    }
    gob_buf_free(e, &data);
}

/* Encoder.encode. */
static void gob_encode(GobEncoder *e, GobEncBuf *b, GobVal v, const GobUserType *ut) {
    Error err = burrow__gob_check_enc(ut);
    if (BURROW_FAILED(err)) {
        gob_set_error(e, err);
        return;
    }
    int indir = gob_op_indir(ut);
    for (int i = 0; i < indir && v.p != NULL; i++) {
        void *q = *(void **)v.p;
        v.t = v.t->elem;
        v.p = q;
    }
    if (ut->external_enc == 0 && ut->base->kind == KIND_STRUCT) {
        if (v.p == NULL)
            panic_str(BURROW_S("reflect: call of reflect.Value.Type on zero Value"));
        gob_encode_struct(e, b, ut, v.p);
        return;
    }
    /* encodeSingle. */
    if (v.p == NULL)
        return;
    GobEncState st = {e, b, true, 0};
    if (ut->external_enc != 0 && ut->enc_indir > 0) {
        v = gob_enc_indirect(v, ut->enc_indir);
        if (v.p == NULL)
            return;
    }
    gob_op(&st, 0, ut, v);
}

/* ----------------------------------------------------------------- the API */

GobEncoder *gob_new_encoder(Alloc *a, IoWriter w) {
    GobEncoder *e = BURROW_NEW(a, GobEncoder);
    if (e == NULL)
        return NULL;
    memset(e, 0, sizeof *e);
    e->a = a;
    e->w = (GobOut *)mem_alloc(a, 4 * sizeof(GobOut), _Alignof(GobOut));
    e->sent = map_make(a, TYPE_OF(Uintptr), TYPE_OF(int32_t), 16);
    if (e->w == NULL || e->sent == NULL) {
        gob_encoder_free(e);
        return NULL;
    }
    e->capw = 4;
    e->w[0] = (GobOut){w, NULL};
    e->nw = 1;
    return e;
}

void gob_encoder_free(GobEncoder *e) {
    if (e == NULL)
        return;
    Alloc *a = e->a;
    mem_free(a, e->w, (size_t)e->capw * sizeof(GobOut), _Alignof(GobOut));
    if (e->sent != NULL)
        map_free(e->sent);
    gob_buf_free(e, &e->byte_buf);
    mem_free(a, e, sizeof *e, _Alignof(GobEncoder));
}

Error gob_encoder_encode_value(GobEncoder *e, Any v) {
    if (v.t == NULL || v.data == NULL)
        return errors_new(error_allocator(), BURROW_S("gob: cannot encode nil value"));
    if (v.t->kind == KIND_POINTER && *(void **)v.data == NULL)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "gob: cannot encode nil pointer of type %s",
                                burrow__gob_type_string(error_allocator(), v.t)));
    sync_mutex_lock(&e->mu);
    e->nw = 1;
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = burrow__gob_user_type(v.t, &err);
    if (ut == NULL) {
        sync_mutex_unlock(&e->mu);
        return err;
    }
    e->err = BURROW_NO_ERROR;
    e->depth = 0;
    e->stack_floor = 0;
    gob_buf_reset(e, &e->byte_buf);
    GobEncState st = {e, &e->byte_buf, false, 0};
    gob_send_type_descriptor(e, gob_writer(e), &st, ut);
    gob_send_type_id(e, &st, ut);
    if (!gob_failed(e)) {
        gob_encode(e, st.b, (GobVal){v.t, v.data}, ut);
        if (!gob_failed(e))
            gob_write_message(e, gob_writer(e), st.b);
    }
    err = e->err;
    sync_mutex_unlock(&e->mu);
    return err;
}

Error gob_encoder_encode(GobEncoder *e, Any v) {
    while (v.t == TYPE_ANY && v.data != NULL)
        v = *(const Any *)v.data;
    return gob_encoder_encode_value(e, v);
}
