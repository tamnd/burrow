/* Derived from Go's src/encoding/gob/decoder.go, decode.go and dec_helpers.go.
 * Go source: go1.27.1.
 *
 * This is Go's decoder with its shape kept: a type description arriving on the
 * stream is compiled, together with the local type it is being read into, to
 * an engine of operations, and the engine is cached by the pair. Errors are
 * raised the way Go raises them, with a panic of a private type that the few
 * places Go recovers in recover here too, so each message comes out of the
 * same check in the same order as it does in Go.
 *
 * A value is a type and a pointer to where it lives. The operations are handed
 * the value with its pointers already followed, which is where Go's decAlloc
 * leaves it, and allocate on the way down when a pointer is nil.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/gob.h"

#include "gob_internal.h"

#include "burrow/bufio.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/sync.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* saferio's chunk: the most a length read off the wire gets allocated up
 * front before the bytes behind it have shown up. */
#define GOB_CHUNK ((Int)10 << 20)

typedef struct GobDecBuf {
    const Byte *data;
    Int len;
    Int off;
} GobDecBuf;

typedef struct GobDecOp GobDecOp;
typedef struct GobEngine GobEngine;

struct GobDecoder {
    Alloc *a;
    SyncMutex mu;
    IoReader r;
    BufioReader *owned;
    Byte *store;
    Int cap;
    GobDecBuf buf;
    Map *wire;     /* int32_t id to GobWireType * */
    Map *cache;    /* canonical local type to a Map of int32_t id to GobEngine ** */
    Map *ignorer;  /* int32_t id to GobEngine ** */
    Arena ar;      /* engines, operations and received types, for the decoder's life */
    Arena scratch; /* what one call needs and nothing after it */
    Alloc *ca;
    Alloc *sa;
    Error err;
    int ignore_depth;
    int depth;
    uintptr_t stack_floor;
};

typedef enum GobOpKind {
    GOB_OP_NONE,
    GOB_OP_BOOL,
    GOB_OP_INT,
    GOB_OP_UINT,
    GOB_OP_FLOAT32,
    GOB_OP_FLOAT64,
    GOB_OP_COMPLEX64,
    GOB_OP_COMPLEX128,
    GOB_OP_STRING,
    GOB_OP_BYTES,
    GOB_OP_ARRAY,
    GOB_OP_SLICE,
    GOB_OP_MAP,
    GOB_OP_STRUCT,
    GOB_OP_INTERFACE,
    GOB_OP_GOB_DECODER,
    GOB_OP_IGNORE_UINT,
    GOB_OP_IGNORE_TWO_UINTS,
    GOB_OP_IGNORE_BYTES,
    GOB_OP_IGNORE_INTERFACE,
    GOB_OP_IGNORE_ARRAY,
    GOB_OP_IGNORE_MAP,
    GOB_OP_IGNORE_SLICE,
    GOB_OP_IGNORE_STRUCT,
    GOB_OP_IGNORE_GOB_DECODER,
} GobOpKind;

/* decOp. Go's are closures; this is what they close over. */
struct GobDecOp {
    GobOpKind kind;
    const Type *t; /* the local type, with its pointers off */
    const GobUserType *ut;
    GobDecOp *elem;
    GobDecOp *key;
    Str ovfl; /* the name in an element's out of range error */
    int64_t len;
    bool helper;
    GobEngine **engine;
};

/* decInstr. index is the path to the field, NULL for one being skipped. */
typedef struct GobInstr {
    GobDecOp *op;
    const Field **index;
    int nindex;
    Str ovfl;
} GobInstr;

struct GobEngine {
    GobInstr *instr;
    Int ninstr;
    Int num_instr;
};

/* ------------------------------------------------------------------ errors */

/* gobError, the panic error_ raises and catchError turns back into an error.
 * The type is private, so nothing else can pass for one. */
typedef struct GobDecError {
    Error err;
} GobDecError;

static const Type gob_dec_error_desc = {
    BURROW_S_INIT("gobError"),
    BURROW_S_INIT("encoding/gob"),
    KIND_STRUCT,
    (uint32_t)sizeof(GobDecError),
    (uint16_t)_Alignof(GobDecError),
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

/* emptyStruct, what an ignored struct is compiled against. */
static const Type gob_empty_struct_desc = {
    BURROW_S_INIT("emptyStruct"),
    BURROW_S_INIT("encoding/gob"),
    KIND_STRUCT,
    0,
    1,
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

BURROW_NORETURN static void gob_error_(Error err) {
    GobDecError e = {err};
    panic((Any){&gob_dec_error_desc, &e});
}

#define gob_errorf(...) gob_error_(burrow__gob_errorf(__VA_ARGS__))

BURROW_NORETURN static void gob_error_text(const char *msg) {
    gob_error_(errors_new(error_allocator(), str_from_cstr(msg)));
}

BURROW_NORETURN static void gob_dec_oom(void) {
    gob_error_(burrow_err_out_of_memory);
}

/* What Go does on a nil function or nil pointer, which a stream that confuses
 * the compiler into a half built engine can lead to. */
BURROW_NORETURN static void gob_nil_deref(void) {
    panic_str(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* catchError. */
static void gob_catch(Any p, Error *err) {
    if (p.t != &gob_dec_error_desc)
        panic(p);
    *err = ((const GobDecError *)p.data)->err;
}

/* overflow: no gob: in front, as in Go. */
BURROW_NORETURN static void gob_overflow(Str name) {
    gob_error_(fmt_errorf_v("value for \"%s\" out of range", name));
}

static bool gob_err_eq(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static Str gob_dec_tstr(const Type *t) {
    return burrow__gob_type_string(error_allocator(), t);
}

static const GobUserType *gob_user_type(const Type *t) {
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = burrow__gob_user_type(t, &err);
    if (ut == NULL)
        gob_error_(err);
    return ut;
}

/* ------------------------------------------------------------------ memory */

static void *gob_calloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align == 0 ? 1 : align);
    if (p == NULL)
        gob_dec_oom();
    return p;
}

static Str gob_copy_str(Alloc *a, const Byte *p, Int n) {
    if (n == 0)
        return (Str){NULL, 0};
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (q == NULL)
        gob_dec_oom();
    memcpy(q, p, (size_t)n);
    return (Str){q, n};
}

static Str gob_prefix(GobDecoder *d, const char *pre, Str s) {
    Int pn = (Int)strlen(pre);
    Byte *q = (Byte *)mem_alloc_nozero(d->ca, (size_t)(pn + s.len), 1);
    if (q == NULL)
        gob_dec_oom();
    memcpy(q, pre, (size_t)pn);
    if (s.len > 0)
        memcpy(q + pn, s.p, (size_t)s.len);
    return (Str){q, pn + s.len};
}

/* reflect.New(t).Elem(), from the decoder's allocator. */
static void *gob_new_value(GobDecoder *d, const Type *t) {
    return gob_calloc(d->a, t->size, t->align);
}

/* decAlloc. */
static void *gob_dec_alloc(GobDecoder *d, const Type *t, void *p) {
    while (t->kind == KIND_POINTER) {
        void **pp = (void **)p;
        if (*pp == NULL)
            *pp = gob_new_value(d, t->elem);
        p = *pp;
        t = t->elem;
    }
    return p;
}

/* saferio.SliceCapWithSize. */
static Int gob_safe_cap(uint64_t size, uint64_t c) {
    if ((int64_t)c < 0 || c > (uint64_t)INTPTR_MAX)
        return -1;
    if (size > 0 && c > UINT64_MAX / size)
        return -1;
    if (c * size > (uint64_t)GOB_CHUNK) {
        c = (uint64_t)GOB_CHUNK / size;
        if (c == 0)
            c = 1;
    }
    return (Int)c;
}

/* ------------------------------------------------------------------ numbers */

static Int gob_buf_len(const GobDecBuf *b) {
    return b->len - b->off;
}

/* decodeUint. */
static uint64_t gob_decode_uint(GobDecBuf *b) {
    if (b->off >= b->len)
        gob_error_(io_eof);
    Byte c = b->data[b->off++];
    if (c <= 0x7F)
        return c;
    int n = -(int)(int8_t)c;
    if (n > 8)
        gob_error_text("gob: encoded unsigned integer out of range");
    Int avail = gob_buf_len(b);
    if (avail < n)
        gob_errorf("invalid uint data length %d: exceeds input size %d", n, avail);
    uint64_t x = 0;
    for (int i = 0; i < n; i++)
        x = x << 8 | b->data[b->off + i];
    b->off += n;
    return x;
}

static int64_t gob_decode_int(GobDecBuf *b) {
    uint64_t x = gob_decode_uint(b);
    if ((x & 1) != 0)
        return ~(int64_t)(x >> 1);
    return (int64_t)(x >> 1);
}

/* getLength. n is left at zero when it says no, which is what Go's errors
 * then print. */
static bool gob_get_length(GobDecBuf *b, Int *n) {
    uint64_t u = gob_decode_uint(b);
    *n = 0;
    if (u > (uint64_t)INTPTR_MAX || gob_buf_len(b) < (Int)u || GOB_TOO_BIG <= u)
        return false;
    *n = (Int)u;
    return true;
}

static void gob_drop(GobDecBuf *b, Int n) {
    b->off += n;
}

static double gob_float64_from_bits(uint64_t u) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (u & 0xFF);
        u >>= 8;
    }
    double f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static double gob_float32_from_bits(uint64_t u, Str ovfl) {
    double v = gob_float64_from_bits(u);
    double av = v < 0 ? -v : v;
    if ((double)FLT_MAX < av && av <= DBL_MAX)
        gob_overflow(ovfl);
    return v;
}

static void gob_store_int(const Type *t, void *p, int64_t v, Str ovfl) {
    switch (t->size) {
    case 1:
        if (v < INT8_MIN || v > INT8_MAX)
            gob_overflow(ovfl);
        *(int8_t *)p = (int8_t)v;
        return;
    case 2:
        if (v < INT16_MIN || v > INT16_MAX)
            gob_overflow(ovfl);
        *(int16_t *)p = (int16_t)v;
        return;
    case 4:
        if (v < INT32_MIN || v > INT32_MAX)
            gob_overflow(ovfl);
        *(int32_t *)p = (int32_t)v;
        return;
    default:
        *(int64_t *)p = v;
        return;
    }
}

static void gob_store_uint(const Type *t, void *p, uint64_t v, Str ovfl) {
    switch (t->size) {
    case 1:
        if (v > UINT8_MAX)
            gob_overflow(ovfl);
        *(uint8_t *)p = (uint8_t)v;
        return;
    case 2:
        if (v > UINT16_MAX)
            gob_overflow(ovfl);
        *(uint16_t *)p = (uint16_t)v;
        return;
    case 4:
        if (v > UINT32_MAX)
            gob_overflow(ovfl);
        *(uint32_t *)p = (uint32_t)v;
        return;
    default:
        *(uint64_t *)p = v;
        return;
    }
}

/* ------------------------------------------------------------------ reading */

/* io.ReadFull from the stream, or from the message buffer, which is what
 * decodeUintReader is handed in the two places it is used. */
static Int gob_read_full(GobDecoder *d, bool from_buf, Byte *p, Int n, Error *err) {
    *err = BURROW_NO_ERROR;
    if (!from_buf)
        return io_read_full(d->r, slice_from(p, n, n, TYPE_BYTE), err);
    Int m = gob_buf_len(&d->buf);
    if (m > n)
        m = n;
    if (m > 0)
        memcpy(p, d->buf.data + d->buf.off, (size_t)m);
    d->buf.off += m;
    if (m == 0 && n > 0)
        *err = io_eof;
    else if (m < n)
        *err = io_err_unexpected_eof;
    return m;
}

/* decodeUintReader. */
static uint64_t gob_decode_uint_reader(GobDecoder *d, bool from_buf, Error *err) {
    Byte tmp[8] = {0};
    Int n = gob_read_full(d, from_buf, tmp, 1, err);
    if (n == 0)
        return 0;
    Byte c = tmp[0];
    if (c <= 0x7F)
        return c;
    int m = -(int)(int8_t)c;
    if (m > 8) {
        *err = errors_new(error_allocator(),
                          BURROW_S("gob: encoded unsigned integer out of range"));
        return 0;
    }
    Int w = gob_read_full(d, from_buf, tmp, m, err);
    if (BURROW_FAILED(*err)) {
        if (gob_err_eq(*err, io_eof))
            *err = io_err_unexpected_eof;
        return 0;
    }
    uint64_t x = 0;
    for (Int i = 0; i < w; i++)
        x = x << 8 | tmp[i];
    return x;
}

static bool gob_store_grow(GobDecoder *d, Int n) {
    if (n <= d->cap)
        return true;
    Int nc = d->cap < 64 ? 64 : d->cap;
    while (nc < n)
        nc = nc > INTPTR_MAX / 2 ? n : nc * 2;
    Byte *p = (Byte *)mem_realloc(d->a, d->store, (size_t)d->cap, (size_t)nc, 1);
    if (p == NULL)
        return false;
    d->store = p;
    d->cap = nc;
    return true;
}

/* saferio.ReadData into the decoder's buffer, which grows a chunk at a time
 * when the length is large, so a length that lies costs no more memory than
 * the bytes that really arrive. */
static Error gob_read_data(GobDecoder *d, Int n) {
    Error err = BURROW_NO_ERROR;
    Int have = 0;
    while (have < n) {
        Int next = n - have;
        if (next > GOB_CHUNK)
            next = GOB_CHUNK;
        if (!gob_store_grow(d, have + next))
            return burrow_err_out_of_memory;
        (void)io_read_full(d->r, slice_from(d->store + have, next, next, TYPE_BYTE),
                           &err);
        if (BURROW_FAILED(err)) {
            if (have > 0 && gob_err_eq(err, io_eof))
                err = io_err_unexpected_eof;
            return err;
        }
        have += next;
    }
    d->buf = (GobDecBuf){d->store, n, 0};
    return BURROW_NO_ERROR;
}

/* readMessage. */
static void gob_read_message(GobDecoder *d, Int n) {
    if (gob_buf_len(&d->buf) != 0)
        panic_str(BURROW_S("non-empty decoder buffer"));
    d->buf = (GobDecBuf){d->store, 0, 0};
    d->err = gob_read_data(d, n);
    if (BURROW_FAILED(d->err))
        d->buf = (GobDecBuf){d->store, 0, 0};
    if (gob_err_eq(d->err, io_eof))
        d->err = io_err_unexpected_eof;
}

/* recvMessage. */
static bool gob_recv_message(GobDecoder *d) {
    Error err = BURROW_NO_ERROR;
    uint64_t nbytes = gob_decode_uint_reader(d, false, &err);
    if (BURROW_FAILED(err)) {
        d->err = err;
        return false;
    }
    if (nbytes >= GOB_TOO_BIG) {
        d->err = errors_new(error_allocator(), BURROW_S("invalid message length"));
        return false;
    }
    gob_read_message(d, (Int)nbytes);
    return BURROW_OK(d->err);
}

static int64_t gob_next_int(GobDecoder *d) {
    Error err = BURROW_NO_ERROR;
    uint64_t x = gob_decode_uint_reader(d, true, &err);
    if (BURROW_FAILED(err))
        d->err = err;
    int64_t i = (int64_t)(x >> 1);
    if ((x & 1) != 0)
        i = ~i;
    return i;
}

static uint64_t gob_next_uint(GobDecoder *d) {
    Error err = BURROW_NO_ERROR;
    uint64_t x = gob_decode_uint_reader(d, true, &err);
    if (BURROW_FAILED(err))
        d->err = err;
    return x;
}

/* ------------------------------------------------------------ wire types */

static GobWireType *gob_wire(GobDecoder *d, int32_t id) {
    const Uintptr *p = (const Uintptr *)map_get(d->wire, &id);
    return p == NULL ? NULL : (GobWireType *)*p;
}

/* wireType.string(). */
static Str gob_wire_string(const GobWireType *w) {
    if (w == NULL)
        return BURROW_S("unknown type");
    for (int k = 0; k < GOB_W_COUNT; k++)
        if (w->t[k] != NULL)
            return w->t[k]->name;
    return BURROW_S("unknown type");
}

/* typeId.string(). */
static Str gob_id_string(int32_t id) {
    const GobType *t = burrow__gob_id_to_type(id);
    if (t == NULL)
        return BURROW_S("<nil>");
    return burrow__gob_type_str(error_allocator(), t);
}

/* Decoder.typeString. */
static Str gob_type_string_remote(GobDecoder *d, int32_t id) {
    const GobType *t = burrow__gob_id_to_type(id);
    if (t != NULL)
        return burrow__gob_type_str(error_allocator(), t);
    return gob_wire_string(gob_wire(d, id));
}

/* The wire types are read the way Go reads them, which is by decoding a
 * wireType struct with the engine for the builtin description of it. The
 * shapes are fixed, so the decoding here is written out, keeping decodeStruct's
 * checks and the messages its operations would give. */
typedef void (*GobWireField)(GobDecoder *d, void *ctx, Int field);

static void gob_wire_struct(GobDecoder *d, Int nfield, GobWireField f, void *ctx) {
    GobDecBuf *b = &d->buf;
    Int fieldnum = -1;
    while (gob_buf_len(b) > 0) {
        Int delta = (Int)gob_decode_uint(b);
        if (delta < 0)
            gob_errorf("decode: corrupted data: negative delta");
        if (delta == 0)
            break;
        if (fieldnum >= nfield - delta)
            gob_error_text("gob: bad data: field numbers out of bounds");
        fieldnum += delta;
        f(d, ctx, fieldnum);
    }
}

static Str gob_wire_name(GobDecoder *d) {
    GobDecBuf *b = &d->buf;
    Int n = 0;
    if (!gob_get_length(b, &n))
        gob_errorf("bad string slice length: %d", n);
    Str s = gob_copy_str(d->ca, b->data + b->off, n);
    gob_drop(b, n);
    return s;
}

static int32_t gob_wire_id(GobDecoder *d, const char *name) {
    int64_t v = gob_decode_int(&d->buf);
    if (v < INT32_MIN || v > INT32_MAX)
        gob_overflow(str_from_cstr(name));
    return (int32_t)v;
}

static void gob_wire_common(GobDecoder *d, void *ctx, Int field) {
    GobType *t = (GobType *)ctx;
    if (field == 0)
        t->name = gob_wire_name(d);
    else
        t->id = gob_wire_id(d, "Id");
}

static void gob_wire_field_type(GobDecoder *d, void *ctx, Int field) {
    GobFieldType *f = (GobFieldType *)ctx;
    if (field == 0)
        f->name = gob_wire_name(d);
    else
        f->id = gob_wire_id(d, "Id");
}

/* decodeSlice for structType.Field. */
static void gob_wire_fields(GobDecoder *d, GobType *t) {
    GobDecBuf *b = &d->buf;
    uint64_t u = gob_decode_uint(b);
    uint64_t size = sizeof(GobFieldType);
    uint64_t nbytes = u * size;
    if (u > (uint64_t)INTPTR_MAX || nbytes > GOB_TOO_BIG || nbytes / size != u)
        gob_errorf("gob.fieldType slice too big: %d elements of %d bytes", u, size);
    Int n = (Int)u;
    if (t->capfield < n) {
        Int safe = gob_safe_cap(size, u);
        t->field = (GobFieldType *)gob_calloc(
            d->ca, (size_t)safe * sizeof(GobFieldType), _Alignof(GobFieldType));
        t->nfield = t->capfield = safe;
    } else {
        t->nfield = n;
    }
    for (Int i = 0; i < n; i++) {
        if (gob_buf_len(b) == 0)
            gob_errorf(
                "decoding array or slice: length exceeds input size (%d elements)", n);
        if (i >= t->nfield) {
            Int nc = t->capfield * 2;
            GobFieldType *nf = (GobFieldType *)gob_calloc(
                d->ca, (size_t)nc * sizeof(GobFieldType), _Alignof(GobFieldType));
            memcpy(nf, t->field, (size_t)t->nfield * sizeof(GobFieldType));
            t->field = nf;
            t->capfield = nc;
            t->nfield = nc < n ? nc : n;
        }
        gob_wire_struct(d, 2, gob_wire_field_type, &t->field[i]);
    }
}

typedef struct GobWirePart {
    GobType *t;
    int slot;
} GobWirePart;

static void gob_wire_part(GobDecoder *d, void *ctx, Int field) {
    GobWirePart *wp = (GobWirePart *)ctx;
    GobType *t = wp->t;
    if (field == 0) {
        gob_wire_struct(d, 2, gob_wire_common, t);
        return;
    }
    switch (wp->slot) {
    case GOB_W_ARRAY:
        if (field == 1)
            t->elem = gob_wire_id(d, "Elem");
        else
            t->len = gob_decode_int(&d->buf);
        return;
    case GOB_W_SLICE:
        t->elem = gob_wire_id(d, "Elem");
        return;
    case GOB_W_STRUCT:
        gob_wire_fields(d, t);
        return;
    case GOB_W_MAP:
        if (field == 1)
            t->key = gob_wire_id(d, "Key");
        else
            t->elem = gob_wire_id(d, "Elem");
        return;
    default:
        return;
    }
}

static void gob_wire_top(GobDecoder *d, void *ctx, Int field) {
    GobWireType *w = (GobWireType *)ctx;
    static const GobKind kinds[GOB_W_COUNT] = {
        GOB_ARRAY,       GOB_SLICE,       GOB_STRUCT,      GOB_MAP,
        GOB_GOB_ENCODER, GOB_GOB_ENCODER, GOB_GOB_ENCODER,
    };
    static const Int nfields[GOB_W_COUNT] = {3, 2, 2, 3, 1, 1, 1};
    if (w->t[field] == NULL) {
        w->t[field] = (GobType *)gob_calloc(d->ca, sizeof(GobType), _Alignof(GobType));
        w->t[field]->kind = kinds[field];
    }
    GobWirePart wp = {w->t[field], (int)field};
    gob_wire_struct(d, nfields[field], gob_wire_part, &wp);
}

/* decodeValue(tWireType, wire), catchError included. */
static void gob_decode_wire_type(GobDecoder *d, GobWireType *w) {
    int saved = d->depth;
    Error err = BURROW_NO_ERROR;
    volatile bool failed = false;
    BURROW_TRY {
        gob_wire_struct(d, GOB_W_COUNT, gob_wire_top, w);
    }
    BURROW_CATCH(p) {
        d->depth = saved;
        gob_catch(p, &err);
        failed = true;
    }
    BURROW_TRY_END;
    if (failed)
        d->err = err;
}

/* recvType. */
static void gob_recv_type(GobDecoder *d, int32_t id) {
    if (id < GOB_FIRST_USER_ID || gob_wire(d, id) != NULL) {
        d->err =
            errors_new(error_allocator(), BURROW_S("gob: duplicate type received"));
        return;
    }
    GobWireType *w = (GobWireType *)mem_alloc(d->ca, sizeof *w, _Alignof(GobWireType));
    if (w == NULL) {
        d->err = burrow_err_out_of_memory;
        return;
    }
    gob_decode_wire_type(d, w);
    if (BURROW_FAILED(d->err))
        return;
    Uintptr v = (Uintptr)w;
    if (!map_set(d->wire, &id, &v))
        d->err = burrow_err_out_of_memory;
}

/* decodeTypeSequence. */
static int32_t gob_decode_type_sequence(GobDecoder *d, bool is_interface) {
    bool first = true;
    while (BURROW_OK(d->err)) {
        if (gob_buf_len(&d->buf) == 0) {
            if (!gob_recv_message(d)) {
                if (!first && gob_err_eq(d->err, io_eof))
                    d->err = io_err_unexpected_eof;
                break;
            }
        }
        int32_t id = (int32_t)(uint32_t)(uint64_t)gob_next_int(d);
        if (id >= 0)
            return id;
        gob_recv_type(d, (int32_t)(0U - (uint32_t)id));
        if (BURROW_FAILED(d->err))
            break;
        if (gob_buf_len(&d->buf) > 0) {
            if (!is_interface) {
                d->err =
                    errors_new(error_allocator(), BURROW_S("extra data in buffer"));
                break;
            }
            (void)gob_next_uint(d);
        }
        first = false;
    }
    return -1;
}

/* ----------------------------------------------------------------- running */

static void gob_run(GobDecoder *d, const GobDecOp *op, Str ovfl, void *p);
static void gob_decode_value(GobDecoder *d, int32_t wire_id, const Type *vt, void *p);

static void gob_dec_enter(GobDecoder *d) {
    if (burrow__gob_stack_low(&d->stack_floor, d->depth))
        gob_errorf("decoder: nesting too deep");
    d->depth++;
}

static const GobEngine *gob_engine(GobEngine *const *ep) {
    if (ep == NULL || *ep == NULL)
        gob_nil_deref();
    return *ep;
}

/* FieldByIndex, which goes through a pointer to an embedded struct and panics
 * when it is nil. */
static void *gob_field_by_index(const GobInstr *in, void *p) {
    Byte *q = (Byte *)p;
    for (int i = 0; i < in->nindex; i++) {
        if (i > 0) {
            const Type *ft = in->index[i - 1]->type;
            if (ft->kind == KIND_POINTER && ft->elem->kind == KIND_STRUCT) {
                memcpy(&q, (const void *)q, sizeof(Byte *));
                if (q == NULL)
                    panic_str(BURROW_S(
                        "reflect: indirection through nil pointer to embedded struct"));
            }
        }
        q += in->index[i]->offset;
    }
    return q;
}

/* decodeStruct. */
static void gob_decode_struct(GobDecoder *d, const GobEngine *e, void *p) {
    gob_dec_enter(d);
    GobDecBuf *b = &d->buf;
    Int fieldnum = -1;
    while (gob_buf_len(b) > 0) {
        Int delta = (Int)gob_decode_uint(b);
        if (delta < 0)
            gob_errorf("decode: corrupted data: negative delta");
        if (delta == 0)
            break;
        if (fieldnum >= e->ninstr - delta)
            gob_error_text("gob: bad data: field numbers out of bounds");
        fieldnum += delta;
        const GobInstr *in = &e->instr[fieldnum];
        void *field = NULL;
        if (in->index != NULL) {
            field = gob_field_by_index(in, p);
            const Type *ft = in->index[in->nindex - 1]->type;
            if (ft->kind == KIND_POINTER)
                field = gob_dec_alloc(d, ft, field);
        }
        gob_run(d, in->op, in->ovfl, field);
    }
    d->depth--;
}

/* ignoreStruct. */
static void gob_ignore_struct(GobDecoder *d, const GobEngine *e) {
    gob_dec_enter(d);
    GobDecBuf *b = &d->buf;
    Int fieldnum = -1;
    while (gob_buf_len(b) > 0) {
        Int delta = (Int)gob_decode_uint(b);
        if (delta < 0)
            gob_errorf("ignore decode: corrupted data: negative delta");
        if (delta == 0)
            break;
        fieldnum += delta;
        if (fieldnum >= e->ninstr)
            gob_error_text("gob: bad data: field numbers out of bounds");
        const GobInstr *in = &e->instr[fieldnum];
        gob_run(d, in->op, in->ovfl, NULL);
    }
    d->depth--;
}

/* decodeSingle and ignoreSingle. */
static void gob_decode_single(GobDecoder *d, const GobEngine *e, void *p) {
    if (gob_decode_uint(&d->buf) != 0)
        gob_errorf("decode: corrupted data: non-zero delta for singleton");
    gob_run(d, e->instr[0].op, e->instr[0].ovfl, p);
}

/* Go's decHelper types: an unnamed slice of a predeclared type, or an array of
 * one, which it decodes in a loop of its own with its own messages. */
static bool gob_predeclared(const Type *t) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        return t == TYPE_BOOL;
    case KIND_INT:
        return t == TYPE_INT;
    case KIND_INT8:
        return t == TYPE_INT8;
    case KIND_INT16:
        return t == TYPE_INT16;
    case KIND_INT32:
        return t == TYPE_INT32;
    case KIND_INT64:
        return t == TYPE_INT64;
    case KIND_UINT:
        return t == TYPE_UINT;
    case KIND_UINT16:
        return t == TYPE_UINT16;
    case KIND_UINT32:
        return t == TYPE_UINT32;
    case KIND_UINT64:
        return t == TYPE_UINT64;
    case KIND_UINTPTR:
        return t == TYPE_UINTPTR;
    case KIND_FLOAT32:
        return t == TYPE_FLOAT32;
    case KIND_FLOAT64:
        return t == TYPE_FLOAT64;
    case KIND_COMPLEX64:
        return t == TYPE_COMPLEX64;
    case KIND_COMPLEX128:
        return t == TYPE_COMPLEX128;
    case KIND_STRING:
        return t == TYPE_STRING;
    default:
        return false;
    }
}

/* One string of decStringSlice. */
static void gob_helper_string(GobDecoder *d, Str *out) {
    GobDecBuf *b = &d->buf;
    uint64_t u = gob_decode_uint(b);
    if (u > (uint64_t)gob_buf_len(b))
        gob_errorf("length of string exceeds input size (%d bytes)", u);
    Int n = (Int)u;
    *out = gob_copy_str(d->a, b->data + b->off, n);
    gob_drop(b, n);
}

/* decodeArrayHelper. s is the slice for a slice and NULL for an array, which
 * is at p and already length long. */
static void gob_decode_array_helper(GobDecoder *d, const GobDecOp *op, const Type *et,
                                    void *p, Slice *s, Int length) {
    gob_dec_enter(d);
    GobDecBuf *b = &d->buf;
    bool is_ptr = et->kind == KIND_POINTER;
    Int ln = s != NULL ? s->len : length;
    Str kname = kind_name(et->kind);
    for (Int i = 0; i < length; i++) {
        if (gob_buf_len(b) == 0) {
            if (op->helper)
                gob_errorf("decoding %s array or slice: length exceeds input size (%d "
                           "elements)",
                           kname, length);
            gob_errorf(
                "decoding array or slice: length exceeds input size (%d elements)",
                length);
        }
        if (i >= ln) {
            /* A slice that was only partly allocated, because its length
             * came off the wire. It grows as the elements really arrive. */
            Int nc = s->cap < 1 ? 1 : s->cap * 2;
            void *np = gob_calloc(d->a, (size_t)nc * et->size, et->align);
            memcpy(np, s->p, (size_t)s->len * et->size);
            mem_free(d->a, s->p, (size_t)s->cap * et->size,
                     et->align == 0 ? 1 : et->align);
            s->p = np;
            s->cap = nc;
            s->len = nc < length ? nc : length;
            ln = s->len;
        }
        Byte *base = s != NULL ? (Byte *)s->p : (Byte *)p;
        void *v = base + (size_t)i * et->size;
        if (op->helper && et->kind == KIND_STRING) {
            gob_helper_string(d, (Str *)v);
            continue;
        }
        if (is_ptr)
            v = gob_dec_alloc(d, et, v);
        gob_run(d, op->elem, op->ovfl, v);
    }
    d->depth--;
}

/* decodeSlice. */
static void gob_decode_slice(GobDecoder *d, const GobDecOp *op, Slice *s) {
    uint64_t u = gob_decode_uint(&d->buf);
    const Type *et = op->t->elem;
    uint64_t size = et->size;
    uint64_t nbytes = u * size;
    if (u > (uint64_t)INTPTR_MAX || nbytes > GOB_TOO_BIG ||
        (size > 0 && nbytes / size != u))
        gob_errorf("%s slice too big: %d elements of %d bytes", gob_dec_tstr(et), u,
                   size);
    Int n = (Int)u;
    if (s->cap < n) {
        Int safe = gob_safe_cap(size, u);
        if (safe < 0)
            gob_errorf("%s slice too big: %d elements of %d bytes", gob_dec_tstr(et), u,
                       size);
        s->p = gob_calloc(d->a, (size_t)safe * et->size, et->align);
        s->len = s->cap = safe;
    } else {
        s->len = n;
    }
    s->elem = et;
    gob_decode_array_helper(d, op, et, NULL, s, n);
}

/* decUint8Slice. */
static void gob_decode_bytes(GobDecoder *d, const Type *t, Slice *s) {
    GobDecBuf *b = &d->buf;
    Int n = 0;
    if (!gob_get_length(b, &n))
        gob_errorf("bad %s slice length: %d", gob_dec_tstr(t), n);
    if (s->cap < n) {
        s->p = gob_calloc(d->a, (size_t)n, 1);
        s->cap = n;
    }
    s->len = n;
    s->elem = t->elem;
    if (n > 0)
        memcpy(s->p, b->data + b->off, (size_t)n);
    gob_drop(b, n);
}

/* decodeMap. */
static void gob_decode_map(GobDecoder *d, const GobDecOp *op, Map **mp) {
    gob_dec_enter(d);
    const Type *mt = op->t;
    Int n = (Int)gob_decode_uint(&d->buf);
    if (*mp == NULL) {
        Int safe = gob_safe_cap(mt->elem->size, (uint64_t)n);
        if (safe < 0)
            safe = 1;
        *mp = map_make(d->a, mt->key, mt->elem, safe);
        if (*mp == NULL)
            gob_dec_oom();
    }
    bool key_ptr = mt->key->kind == KIND_POINTER;
    bool elem_ptr = mt->elem->kind == KIND_POINTER;
    void *kp = gob_calloc(d->sa, mt->key->size, mt->key->align);
    void *ep = gob_calloc(d->sa, mt->elem->size, mt->elem->align);
    for (Int i = 0; i < n; i++) {
        gob_run(d, op->key, op->ovfl, key_ptr ? gob_dec_alloc(d, mt->key, kp) : kp);
        gob_run(d, op->elem, op->ovfl, elem_ptr ? gob_dec_alloc(d, mt->elem, ep) : ep);
        if (!map_set(*mp, kp, ep))
            gob_dec_oom();
        memset(kp, 0, mt->key->size);
        memset(ep, 0, mt->elem->size);
    }
    d->depth--;
}

/* decodeInterface. Only an Any can take the value, because nothing can make
 * the table of methods another interface type needs. */
static void gob_decode_interface(GobDecoder *d, const Type *ityp, void *p) {
    GobDecBuf *b = &d->buf;
    uint64_t nr = gob_decode_uint(b);
    if (nr > (uint64_t)1 << 31)
        gob_errorf("invalid type name length %d", nr);
    if (nr > (uint64_t)gob_buf_len(b))
        gob_errorf("invalid type name length %d: exceeds input size", nr);
    Int n = (Int)nr;
    Str name = {b->data + b->off, n};
    gob_drop(b, n);
    if (n == 0) {
        memset(p, 0, ityp->size);
        return;
    }
    if (n > 1024)
        gob_errorf("name too long (%d bytes): %.20q...", n, name);
    const Type *typ = burrow__gob_type_of_name(name);
    if (typ == NULL)
        gob_errorf("name not registered for interface: %q", name);
    /* name points into the message and is not used past here, which matters,
     * because reading the type can bring in a new message. */
    gob_dec_enter(d);
    int32_t concrete = gob_decode_type_sequence(d, true);
    if (concrete < 0)
        gob_error_(d->err);
    (void)gob_decode_uint(b);
    void *v = gob_new_value(d, typ);
    gob_decode_value(d, concrete, typ, v);
    if (BURROW_FAILED(d->err))
        gob_error_(d->err);
    if (ityp != TYPE_ANY)
        gob_errorf(
            "%s cannot be stored in %s: only interface {} can receive a decoded value",
            gob_dec_tstr(typ), gob_dec_tstr(ityp));
    Any *a = (Any *)p;
    a->t = typ;
    a->data = v;
    d->depth--;
}

/* ignoreInterface. */
static void gob_ignore_interface(GobDecoder *d) {
    GobDecBuf *b = &d->buf;
    Int n = 0;
    if (!gob_get_length(b, &n))
        gob_errorf("bad interface encoding: name too large for buffer");
    Int bn = gob_buf_len(b);
    if (bn < n)
        gob_errorf("invalid interface value length %d: exceeds input size %d", n, bn);
    gob_drop(b, n);
    int32_t id = gob_decode_type_sequence(d, true);
    if (id < 0)
        gob_error_(d->err);
    if (!gob_get_length(b, &n))
        gob_errorf("bad interface encoding: data length too large for buffer");
    gob_drop(b, n);
}

/* decodeGobDecoder. p is the value with its pointers followed, which is what
 * the method takes. */
static void gob_decode_gob_decoder(GobDecoder *d, const GobUserType *ut, void *p) {
    GobDecBuf *b = &d->buf;
    Int n = 0;
    if (!gob_get_length(b, &n))
        gob_errorf("GobDecoder: length too large for buffer");
    Int bn = gob_buf_len(b);
    if (bn < n)
        gob_errorf("GobDecoder: invalid data length %d: exceeds input size %d", n, bn);
    Bytes data = slice_from((void *)(uintptr_t)(b->data + b->off), n, n, TYPE_BYTE);
    gob_drop(b, n);
    Error err = BURROW_NO_ERROR;
    EncodingAllocArg aa = d->a;
    void *args[2] = {&aa, &data};
    void *rets[1] = {&err};
    method_call(ut->dec_method, p, args, rets);
    if (BURROW_FAILED(err))
        gob_error_(err);
}

static void gob_ignore_gob_decoder(GobDecoder *d) {
    GobDecBuf *b = &d->buf;
    Int n = 0;
    if (!gob_get_length(b, &n))
        gob_errorf("GobDecoder: length too large for buffer");
    Int bn = gob_buf_len(b);
    if (bn < n)
        gob_errorf("GobDecoder: invalid data length %d: exceeds input size %d", n, bn);
    gob_drop(b, n);
}

/* ignoreArrayHelper. */
static void gob_ignore_array_helper(GobDecoder *d, const GobDecOp *elem, Int length) {
    gob_dec_enter(d);
    for (Int i = 0; i < length; i++) {
        if (gob_buf_len(&d->buf) == 0)
            gob_errorf(
                "decoding array or slice: length exceeds input size (%d elements)",
                length);
        gob_run(d, elem, BURROW_S("no error"), NULL);
    }
    d->depth--;
}

static void gob_run(GobDecoder *d, const GobDecOp *op, Str ovfl, void *p) {
    GobDecBuf *b = &d->buf;
    if (op == NULL)
        gob_nil_deref();
    /* Only the ignore ops, which come last, run with nowhere to put a value. */
    if (p == NULL && op->kind < GOB_OP_IGNORE_UINT)
        gob_nil_deref();
    switch (op->kind) {
    case GOB_OP_BOOL:
        *(bool *)p = gob_decode_uint(b) != 0;
        return;
    case GOB_OP_INT:
        gob_store_int(op->t, p, gob_decode_int(b), ovfl);
        return;
    case GOB_OP_UINT:
        gob_store_uint(op->t, p, gob_decode_uint(b), ovfl);
        return;
    case GOB_OP_FLOAT32:
        *(float *)p = (float)gob_float32_from_bits(gob_decode_uint(b), ovfl);
        return;
    case GOB_OP_FLOAT64:
        *(double *)p = gob_float64_from_bits(gob_decode_uint(b));
        return;
    case GOB_OP_COMPLEX64: {
        double re = gob_float32_from_bits(gob_decode_uint(b), ovfl);
        double im = gob_float32_from_bits(gob_decode_uint(b), ovfl);
        ((Complex64 *)p)->re = (float)re;
        ((Complex64 *)p)->im = (float)im;
        return;
    }
    case GOB_OP_COMPLEX128: {
        double re = gob_float64_from_bits(gob_decode_uint(b));
        double im = gob_float64_from_bits(gob_decode_uint(b));
        ((Complex128 *)p)->re = re;
        ((Complex128 *)p)->im = im;
        return;
    }
    case GOB_OP_STRING: {
        Int n = 0;
        if (!gob_get_length(b, &n))
            gob_errorf("bad %s slice length: %d", gob_dec_tstr(op->t), n);
        if (gob_buf_len(b) < n)
            gob_errorf("invalid string length %d: exceeds input size %d", n,
                       gob_buf_len(b));
        *(Str *)p = gob_copy_str(d->a, b->data + b->off, n);
        gob_drop(b, n);
        return;
    }
    case GOB_OP_BYTES:
        gob_decode_bytes(d, op->t, (Slice *)p);
        return;
    case GOB_OP_ARRAY:
        if (gob_decode_uint(b) != (uint64_t)op->t->len)
            gob_errorf("length mismatch in decodeArray");
        gob_decode_array_helper(d, op, op->t->elem, p, NULL, (Int)op->t->len);
        return;
    case GOB_OP_SLICE:
        gob_decode_slice(d, op, (Slice *)p);
        return;
    case GOB_OP_MAP:
        gob_decode_map(d, op, (Map **)p);
        return;
    case GOB_OP_STRUCT:
        gob_decode_struct(d, gob_engine(op->engine), p);
        return;
    case GOB_OP_INTERFACE:
        gob_decode_interface(d, op->t, p);
        return;
    case GOB_OP_GOB_DECODER:
        gob_decode_gob_decoder(d, op->ut, p);
        return;
    case GOB_OP_IGNORE_UINT:
        (void)gob_decode_uint(b);
        return;
    case GOB_OP_IGNORE_TWO_UINTS:
        (void)gob_decode_uint(b);
        (void)gob_decode_uint(b);
        return;
    case GOB_OP_IGNORE_BYTES: {
        Int n = 0;
        if (!gob_get_length(b, &n))
            gob_errorf("slice length too large");
        Int bn = gob_buf_len(b);
        if (bn < n)
            gob_errorf("invalid slice length %d: exceeds input size %d", n, bn);
        gob_drop(b, n);
        return;
    }
    case GOB_OP_IGNORE_INTERFACE:
        gob_ignore_interface(d);
        return;
    case GOB_OP_IGNORE_ARRAY:
        if (gob_decode_uint(b) != (uint64_t)op->len)
            gob_errorf("length mismatch in ignoreArray");
        gob_ignore_array_helper(d, op->elem, (Int)op->len);
        return;
    case GOB_OP_IGNORE_MAP: {
        Int n = (Int)gob_decode_uint(b);
        gob_dec_enter(d);
        for (Int i = 0; i < n; i++) {
            gob_run(d, op->key, BURROW_S("no error"), NULL);
            gob_run(d, op->elem, BURROW_S("no error"), NULL);
        }
        d->depth--;
        return;
    }
    case GOB_OP_IGNORE_SLICE:
        gob_ignore_array_helper(d, op->elem, (Int)gob_decode_uint(b));
        return;
    case GOB_OP_IGNORE_STRUCT:
        gob_ignore_struct(d, gob_engine(op->engine));
        return;
    case GOB_OP_IGNORE_GOB_DECODER:
        gob_ignore_gob_decoder(d);
        return;
    case GOB_OP_NONE:
    default:
        gob_nil_deref();
    }
}

/* --------------------------------------------------------------- compiling */

/* The inProgress and seen maps of the compiler, which are small and live for
 * one compilation. */
typedef struct GobMemo {
    struct GobMemo *next;
    const Type *t;
    int32_t id;
    GobDecOp *op;
} GobMemo;

static GobMemo *gob_memo_type(GobMemo *m, const Type *t) {
    const Type *c = burrow__gob_canon(t);
    for (; m != NULL; m = m->next)
        if (m->t == c)
            return m;
    return NULL;
}

static GobMemo *gob_memo_id(GobMemo *m, int32_t id) {
    for (; m != NULL; m = m->next)
        if (m->t == NULL && m->id == id)
            return m;
    return NULL;
}

static void gob_memo_put(GobDecoder *d, GobMemo **head, const Type *t, int32_t id,
                         GobDecOp *op) {
    GobMemo *m = (GobMemo *)gob_calloc(d->sa, sizeof *m, _Alignof(GobMemo));
    m->t = t != NULL ? burrow__gob_canon(t) : NULL;
    m->id = id;
    m->op = op;
    m->next = *head;
    *head = m;
}

static GobDecOp *gob_new_op(GobDecoder *d, GobOpKind kind, const Type *t) {
    GobDecOp *op = (GobDecOp *)gob_calloc(d->ca, sizeof *op, _Alignof(GobDecOp));
    op->kind = kind;
    op->t = t;
    return op;
}

static Error gob_get_dec_engine_ptr(GobDecoder *d, int32_t remote,
                                    const GobUserType *ut, GobEngine ***out);
static Error gob_get_ignore_engine_ptr(GobDecoder *d, int32_t id, GobEngine ***out);

static GobWireType *gob_must_wire(GobDecoder *d, int32_t id, int slot) {
    GobWireType *w = gob_wire(d, id);
    if (w == NULL || w->t[slot] == NULL)
        gob_nil_deref();
    return w;
}

/* decOpFor. */
static GobDecOp *gob_dec_op_for(GobDecoder *d, int32_t wire_id, const Type *rt,
                                Str name, GobMemo **in_progress) {
    const GobUserType *ut = gob_user_type(rt);
    if (ut->external_dec != 0) {
        GobDecOp *op = gob_new_op(d, GOB_OP_GOB_DECODER, ut->base);
        op->ut = ut;
        return op;
    }
    GobMemo *m = gob_memo_type(*in_progress, rt);
    if (m != NULL)
        return m->op;
    const Type *typ = ut->base;
    switch ((int)typ->kind) {
    case KIND_BOOL:
        return gob_new_op(d, GOB_OP_BOOL, typ);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return gob_new_op(d, GOB_OP_INT, typ);
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return gob_new_op(d, GOB_OP_UINT, typ);
    case KIND_FLOAT32:
        return gob_new_op(d, GOB_OP_FLOAT32, typ);
    case KIND_FLOAT64:
        return gob_new_op(d, GOB_OP_FLOAT64, typ);
    case KIND_COMPLEX64:
        return gob_new_op(d, GOB_OP_COMPLEX64, typ);
    case KIND_COMPLEX128:
        return gob_new_op(d, GOB_OP_COMPLEX128, typ);
    case KIND_STRING:
        return gob_new_op(d, GOB_OP_STRING, typ);
    default:
        break;
    }
    GobDecOp *op = gob_new_op(d, GOB_OP_NONE, typ);
    gob_memo_put(d, in_progress, rt, 0, op);
    switch ((int)typ->kind) {
    case KIND_ARRAY: {
        Str ename = gob_prefix(d, "element of ", name);
        int32_t elem_id = gob_must_wire(d, wire_id, GOB_W_ARRAY)->t[GOB_W_ARRAY]->elem;
        op->elem = gob_dec_op_for(d, elem_id, typ->elem, ename, in_progress);
        op->ovfl = ename;
        op->helper = gob_predeclared(typ->elem);
        op->kind = GOB_OP_ARRAY;
        break;
    }
    case KIND_MAP: {
        const GobType *mt = gob_must_wire(d, wire_id, GOB_W_MAP)->t[GOB_W_MAP];
        op->key = gob_dec_op_for(d, mt->key, typ->key, gob_prefix(d, "key of ", name),
                                 in_progress);
        op->elem = gob_dec_op_for(d, mt->elem, typ->elem,
                                  gob_prefix(d, "element of ", name), in_progress);
        op->ovfl = name;
        op->kind = GOB_OP_MAP;
        break;
    }
    case KIND_SLICE: {
        Str ename = gob_prefix(d, "element of ", name);
        if (typ->elem->kind == KIND_UINT8) {
            op->kind = GOB_OP_BYTES;
            break;
        }
        int32_t elem_id;
        const GobType *bt = burrow__gob_builtin_id_to_type(wire_id);
        if (bt != NULL) {
            if (bt->kind != GOB_SLICE)
                panic_str(BURROW_S(
                    "interface conversion: gob.gobType is not *gob.sliceType"));
            elem_id = bt->elem;
        } else {
            elem_id = gob_must_wire(d, wire_id, GOB_W_SLICE)->t[GOB_W_SLICE]->elem;
        }
        op->elem = gob_dec_op_for(d, elem_id, typ->elem, ename, in_progress);
        op->ovfl = ename;
        op->helper = gob_predeclared(typ->elem) && burrow__gob_type_name(typ).len == 0;
        op->kind = GOB_OP_SLICE;
        break;
    }
    case KIND_STRUCT: {
        GobEngine **ep = NULL;
        Error err = gob_get_dec_engine_ptr(d, wire_id, gob_user_type(typ), &ep);
        if (BURROW_FAILED(err))
            gob_error_(err);
        op->engine = ep;
        op->kind = GOB_OP_STRUCT;
        break;
    }
    case KIND_INTERFACE:
        op->kind = GOB_OP_INTERFACE;
        break;
    default:
        gob_errorf("decode can't handle type %s", gob_dec_tstr(rt));
    }
    return op;
}

static GobDecOp *gob_dec_ignore_op_for_l(GobDecoder *d, int32_t wire_id,
                                         GobMemo **in_progress);

/* decIgnoreOpFor. */
static GobDecOp *gob_dec_ignore_op_for(GobDecoder *d, int32_t wire_id,
                                       GobMemo **in_progress) {
    if (d->ignore_depth >= GOB_MAX_DEPTH)
        gob_error_text("invalid nesting depth");
    if (burrow__gob_stack_low(&d->stack_floor, d->ignore_depth))
        gob_errorf("decoder: nesting too deep");
    d->ignore_depth++;
    GobDecOp *op = gob_dec_ignore_op_for_l(d, wire_id, in_progress);
    d->ignore_depth--;
    return op;
}

static GobDecOp *gob_dec_ignore_op_for_l(GobDecoder *d, int32_t wire_id,
                                         GobMemo **in_progress) {
    GobMemo *m = gob_memo_id(*in_progress, wire_id);
    if (m != NULL)
        return m->op;
    switch (wire_id) {
    case GOB_T_BOOL:
    case GOB_T_INT:
    case GOB_T_UINT:
    case GOB_T_FLOAT:
        return gob_new_op(d, GOB_OP_IGNORE_UINT, NULL);
    case GOB_T_BYTES:
    case GOB_T_STRING:
        return gob_new_op(d, GOB_OP_IGNORE_BYTES, NULL);
    case GOB_T_COMPLEX:
        return gob_new_op(d, GOB_OP_IGNORE_TWO_UINTS, NULL);
    default:
        break;
    }
    GobDecOp *op = gob_new_op(d, GOB_OP_NONE, NULL);
    gob_memo_put(d, in_progress, NULL, wire_id, op);
    if (wire_id == GOB_T_INTERFACE) {
        op->kind = GOB_OP_IGNORE_INTERFACE;
        return op;
    }
    const GobWireType *w = gob_wire(d, wire_id);
    if (w == NULL)
        gob_errorf("bad data: undefined type %s", gob_id_string(wire_id));
    if (w->t[GOB_W_ARRAY] != NULL) {
        op->elem = gob_dec_ignore_op_for(d, w->t[GOB_W_ARRAY]->elem, in_progress);
        op->len = w->t[GOB_W_ARRAY]->len;
        op->kind = GOB_OP_IGNORE_ARRAY;
    } else if (w->t[GOB_W_MAP] != NULL) {
        op->key = gob_dec_ignore_op_for(d, w->t[GOB_W_MAP]->key, in_progress);
        op->elem = gob_dec_ignore_op_for(d, w->t[GOB_W_MAP]->elem, in_progress);
        op->kind = GOB_OP_IGNORE_MAP;
    } else if (w->t[GOB_W_SLICE] != NULL) {
        op->elem = gob_dec_ignore_op_for(d, w->t[GOB_W_SLICE]->elem, in_progress);
        op->kind = GOB_OP_IGNORE_SLICE;
    } else if (w->t[GOB_W_STRUCT] != NULL) {
        GobEngine **ep = NULL;
        Error err = gob_get_ignore_engine_ptr(d, wire_id, &ep);
        if (BURROW_FAILED(err))
            gob_error_(err);
        op->engine = ep;
        op->kind = GOB_OP_IGNORE_STRUCT;
    } else if (w->t[GOB_W_GOB_ENCODER] != NULL ||
               w->t[GOB_W_BINARY_MARSHALER] != NULL ||
               w->t[GOB_W_TEXT_MARSHALER] != NULL) {
        op->kind = GOB_OP_IGNORE_GOB_DECODER;
    } else {
        gob_errorf("bad data: ignore can't handle type %s", gob_id_string(wire_id));
    }
    return op;
}

/* compatibleType. */
static bool gob_compatible(GobDecoder *d, const Type *fr, int32_t fw,
                           GobMemo **in_progress) {
    GobMemo *m = gob_memo_type(*in_progress, fr);
    if (m != NULL)
        return m->id == fw;
    gob_memo_put(d, in_progress, fr, fw, NULL);
    const GobUserType *ut = gob_user_type(fr);
    const GobWireType *w = gob_wire(d, fw);
    bool ok = w != NULL;
    if ((ut->external_dec == GOB_X_GOB) != (ok && w->t[GOB_W_GOB_ENCODER] != NULL) ||
        (ut->external_dec == GOB_X_BINARY) !=
            (ok && w->t[GOB_W_BINARY_MARSHALER] != NULL) ||
        (ut->external_dec == GOB_X_TEXT) != (ok && w->t[GOB_W_TEXT_MARSHALER] != NULL))
        return false;
    if (ut->external_dec != 0)
        return true;
    const Type *t = ut->base;
    switch ((int)t->kind) {
    case KIND_BOOL:
        return fw == GOB_T_BOOL;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return fw == GOB_T_INT;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return fw == GOB_T_UINT;
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return fw == GOB_T_FLOAT;
    case KIND_COMPLEX64:
    case KIND_COMPLEX128:
        return fw == GOB_T_COMPLEX;
    case KIND_STRING:
        return fw == GOB_T_STRING;
    case KIND_INTERFACE:
        return fw == GOB_T_INTERFACE;
    case KIND_ARRAY: {
        if (!ok || w->t[GOB_W_ARRAY] == NULL)
            return false;
        const GobType *at = w->t[GOB_W_ARRAY];
        return (int64_t)t->len == at->len &&
               gob_compatible(d, t->elem, at->elem, in_progress);
    }
    case KIND_MAP: {
        if (!ok || w->t[GOB_W_MAP] == NULL)
            return false;
        const GobType *mt = w->t[GOB_W_MAP];
        return gob_compatible(d, t->key, mt->key, in_progress) &&
               gob_compatible(d, t->elem, mt->elem, in_progress);
    }
    case KIND_SLICE: {
        if (t->elem->kind == KIND_UINT8)
            return fw == GOB_T_BYTES;
        const GobType *sw = NULL;
        const GobType *bt = burrow__gob_builtin_id_to_type(fw);
        if (bt != NULL)
            sw = bt->kind == GOB_SLICE ? bt : NULL;
        else if (w != NULL)
            sw = w->t[GOB_W_SLICE];
        const Type *elem = gob_user_type(t->elem)->base;
        return sw != NULL && gob_compatible(d, elem, sw->elem, in_progress);
    }
    case KIND_STRUCT:
        return true;
    default:
        return false;
    }
}

static GobEngine *gob_new_engine(GobDecoder *d, Int n) {
    GobEngine *e = (GobEngine *)gob_calloc(d->ca, sizeof *e, _Alignof(GobEngine));
    e->instr =
        (GobInstr *)gob_calloc(d->ca, (size_t)n * sizeof(GobInstr), _Alignof(GobInstr));
    e->ninstr = n;
    return e;
}

/* compileSingle. */
static Error gob_compile_single(GobDecoder *d, int32_t remote, const GobUserType *ut,
                                GobEngine **out) {
    const Type *rt = ut->user;
    GobEngine *e = gob_new_engine(d, 1);
    Str name = burrow__gob_type_string(d->ca, rt);
    GobMemo *seen = NULL;
    if (!gob_compatible(d, rt, remote, &seen)) {
        Str remote_type = gob_type_string_remote(d, remote);
        if (ut->base->kind == KIND_INTERFACE && remote != GOB_T_INTERFACE)
            return fmt_errorf_v(
                "gob: local interface type %s can only be decoded from remote "
                "interface type; received concrete type %s",
                name, remote_type);
        return fmt_errorf_v("gob: decoding into local type %s, received remote type %s",
                            name, remote_type);
    }
    GobMemo *in_progress = NULL;
    GobDecOp *op = gob_dec_op_for(d, remote, rt, name, &in_progress);
    e->instr[0] = (GobInstr){op, NULL, 0, name};
    e->num_instr = 1;
    *out = e;
    return BURROW_NO_ERROR;
}

/* compileIgnoreSingle. */
static GobEngine *gob_compile_ignore_single(GobDecoder *d, int32_t remote) {
    GobEngine *e = gob_new_engine(d, 1);
    GobMemo *in_progress = NULL;
    GobDecOp *op = gob_dec_ignore_op_for(d, remote, &in_progress);
    Str name = gob_type_string_remote(d, remote);
    e->instr[0] = (GobInstr){op, NULL, 0, gob_copy_str(d->ca, name.p, name.len)};
    e->num_instr = 1;
    return e;
}

/* reflect's FieldByName, embedded structs and all: the shallowest match, and
 * none when two are equally shallow. The path is left in *path. */
typedef struct GobScan {
    const Type *t;
    const Field **index;
    int n;
} GobScan;

typedef struct GobCount {
    const Type *t;
    int n;
} GobCount;

static bool gob_anonymous(const Field *f) {
    if (field_is_embedded(f))
        return true;
    const Type *t = f->type;
    return t != NULL && t->kind == KIND_POINTER && t->name.len == 0 &&
           t->elem != NULL && str_eq(t->elem->name, f->name);
}

static int gob_count_get(const GobCount *c, Int n, const Type *t) {
    for (Int i = 0; i < n; i++)
        if (c[i].t == t)
            return c[i].n;
    return 0;
}

static const Field **gob_path(GobDecoder *d, const GobScan *s, const Field *f) {
    const Field **p = (const Field **)gob_calloc(d->ca, (size_t)(s->n + 1) * sizeof *p,
                                                 _Alignof(const Field *));
    for (int i = 0; i < s->n; i++)
        p[i] = s->index[i];
    p[s->n] = f;
    return p;
}

static bool gob_field_by_name(GobDecoder *d, const Type *st, Str name,
                              const Field ***path, int *npath) {
    bool has_embeds = false;
    for (uint16_t i = 0; i < st->nfield; i++) {
        const Field *f = &st->fields[i];
        if (str_eq(f->name, name)) {
            GobScan top = {st, NULL, 0};
            *path = gob_path(d, &top, f);
            *npath = 1;
            return true;
        }
        if (gob_anonymous(f))
            has_embeds = true;
    }
    if (!has_embeds)
        return false;

    /* FieldByNameFunc, a level of embedding at a time. Every array here is
     * bounded by the number of struct types reachable from st. */
    Alloc *sa = d->sa;
    Int cap = 16;
    GobScan *current =
        (GobScan *)gob_calloc(sa, (size_t)cap * sizeof(GobScan), _Alignof(GobScan));
    GobScan *next =
        (GobScan *)gob_calloc(sa, (size_t)cap * sizeof(GobScan), _Alignof(GobScan));
    GobCount *count =
        (GobCount *)gob_calloc(sa, (size_t)cap * sizeof(GobCount), _Alignof(GobCount));
    GobCount *next_count =
        (GobCount *)gob_calloc(sa, (size_t)cap * sizeof(GobCount), _Alignof(GobCount));
    const Type **visited = (const Type **)gob_calloc(
        sa, (size_t)cap * sizeof(const Type *), _Alignof(const Type *));
    Int ncur = 0;
    Int nnext = 1;
    Int ncount = 0;
    Int nnext_count = 0;
    Int nvisited = 0;
    next[0] = (GobScan){burrow__gob_canon(st), NULL, 0};
    bool ok = false;
    while (nnext > 0) {
        GobScan *tmp = current;
        current = next;
        next = tmp;
        ncur = nnext;
        nnext = 0;
        GobCount *tc = count;
        count = next_count;
        next_count = tc;
        ncount = nnext_count;
        nnext_count = 0;
        for (Int si = 0; si < ncur; si++) {
            GobScan scan = current[si];
            const Type *t = scan.t;
            bool seen = false;
            for (Int v = 0; v < nvisited; v++)
                if (visited[v] == t)
                    seen = true;
            if (seen)
                continue;
            if (nvisited == cap)
                goto grow;
            visited[nvisited++] = t;
            for (uint16_t i = 0; i < t->nfield; i++) {
                const Field *f = &t->fields[i];
                const Type *ntyp = NULL;
                if (gob_anonymous(f)) {
                    ntyp = f->type;
                    if (ntyp->kind == KIND_POINTER)
                        ntyp = ntyp->elem;
                }
                if (str_eq(f->name, name)) {
                    if (gob_count_get(count, ncount, t) > 1 || ok)
                        return false;
                    *path = gob_path(d, &scan, f);
                    *npath = scan.n + 1;
                    ok = true;
                    continue;
                }
                if (ok || ntyp == NULL || ntyp->kind != KIND_STRUCT)
                    continue;
                const Type *styp = burrow__gob_canon(ntyp);
                bool found = false;
                for (Int c = 0; c < nnext_count; c++) {
                    if (next_count[c].t == styp) {
                        next_count[c].n = 2;
                        found = true;
                    }
                }
                if (found)
                    continue;
                if (nnext_count == cap || nnext == cap)
                    goto grow;
                next_count[nnext_count++] =
                    (GobCount){styp, gob_count_get(count, ncount, t) > 1 ? 2 : 1};
                next[nnext++] = (GobScan){styp, gob_path(d, &scan, f), scan.n + 1};
            }
        }
        if (ok)
            break;
    }
    return ok;
grow:
    /* More than 16 struct types embedded at one level, or visited in all:
     * not a real program, and the answer Go gives there is the one it gives
     * when the name is not found, which it is not found as here. */
    return false;
}

/* compileDec, catchError included. */
static Error gob_compile_dec_l(GobDecoder *d, int32_t remote, const GobUserType *ut,
                               GobEngine **out) {
    const Type *rt = ut->base;
    if (rt->kind != KIND_STRUCT || ut->external_dec != 0)
        return gob_compile_single(d, remote, ut, out);
    const GobType *ws = NULL;
    const GobType *bt = burrow__gob_builtin_id_to_type(remote);
    if (bt != NULL) {
        ws = bt->kind == GOB_STRUCT ? bt : NULL;
    } else {
        const GobWireType *w = gob_wire(d, remote);
        if (w == NULL)
            gob_error_text("gob: unknown type id or corrupted data");
        ws = w->t[GOB_W_STRUCT];
    }
    if (ws == NULL)
        gob_errorf("type mismatch in decoder: want struct type %s; got non-struct",
                   gob_dec_tstr(rt));
    GobEngine *e = gob_new_engine(d, ws->nfield);
    GobMemo *seen = NULL;
    for (Int i = 0; i < ws->nfield; i++) {
        const GobFieldType *wf = &ws->field[i];
        if (wf->name.len == 0)
            gob_errorf("empty name for remote field of type %s", ws->name);
        Str ovfl = wf->name;
        const Field **path = NULL;
        int npath = 0;
        bool present = gob_field_by_name(d, rt, wf->name, &path, &npath);
        if (!present || !burrow__gob_is_exported(wf->name)) {
            GobMemo *ip = NULL;
            GobDecOp *op = gob_dec_ignore_op_for(d, wf->id, &ip);
            e->instr[i] = (GobInstr){op, NULL, 0, ovfl};
            continue;
        }
        const Field *lf = path[npath - 1];
        GobMemo *ip = NULL;
        if (!gob_compatible(d, lf->type, wf->id, &ip))
            gob_errorf("wrong type (%s) for received field %s.%s",
                       gob_dec_tstr(lf->type), ws->name, wf->name);
        GobDecOp *op = gob_dec_op_for(d, wf->id, lf->type, lf->name, &seen);
        e->instr[i] = (GobInstr){op, path, npath, ovfl};
        e->num_instr++;
    }
    *out = e;
    return BURROW_NO_ERROR;
}

/* gob_compile_dec_l with the result through a pointer, so that the err below
 * lives in memory and not in a register gcc warns the longjmp may clobber. */
static void gob_compile_dec_into(GobDecoder *d, int32_t remote, const GobUserType *ut,
                                 GobEngine **out, Error *err) {
    *err = gob_compile_dec_l(d, remote, ut, out);
}

static Error gob_compile_dec(GobDecoder *d, int32_t remote, const GobUserType *ut,
                             GobEngine **out) {
    int saved = d->ignore_depth;
    Error err = BURROW_NO_ERROR;
    Error caught = BURROW_NO_ERROR;
    volatile bool failed = false;
    BURROW_TRY {
        gob_compile_dec_into(d, remote, ut, out, &err);
    }
    BURROW_CATCH(p) {
        d->ignore_depth = saved;
        gob_catch(p, &caught);
        failed = true;
    }
    BURROW_TRY_END;
    return failed ? caught : err;
}

static Map *gob_cache_for(GobDecoder *d, const Type *t) {
    Uintptr k = (Uintptr)burrow__gob_canon(t);
    const Uintptr *v = (const Uintptr *)map_get(d->cache, &k);
    if (v != NULL)
        return (Map *)*v;
    Map *m = map_make(d->a, TYPE_INT32, TYPE_UINTPTR, 4);
    if (m == NULL)
        gob_dec_oom();
    Uintptr mv = (Uintptr)m;
    if (!map_set(d->cache, &k, &mv)) {
        map_free(m);
        gob_dec_oom();
    }
    return m;
}

/* getDecEnginePtr. */
static Error gob_get_dec_engine_ptr(GobDecoder *d, int32_t remote,
                                    const GobUserType *ut, GobEngine ***out) {
    Map *m = gob_cache_for(d, ut->user);
    const Uintptr *v = (const Uintptr *)map_get(m, &remote);
    if (v != NULL) {
        *out = (GobEngine **)*v;
        return BURROW_NO_ERROR;
    }
    GobEngine **ep = (GobEngine **)gob_calloc(d->ca, sizeof *ep, _Alignof(GobEngine *));
    Uintptr ev = (Uintptr)ep;
    if (!map_set(m, &remote, &ev))
        gob_dec_oom();
    *out = ep;
    Error err = gob_compile_dec(d, remote, ut, ep);
    if (BURROW_FAILED(err))
        map_del(m, &remote);
    return err;
}

/* getIgnoreEnginePtr. */
static Error gob_get_ignore_engine_ptr(GobDecoder *d, int32_t id, GobEngine ***out) {
    const Uintptr *v = (const Uintptr *)map_get(d->ignorer, &id);
    if (v != NULL) {
        *out = (GobEngine **)*v;
        return BURROW_NO_ERROR;
    }
    GobEngine **ep = (GobEngine **)gob_calloc(d->ca, sizeof *ep, _Alignof(GobEngine *));
    Uintptr ev = (Uintptr)ep;
    if (!map_set(d->ignorer, &id, &ev))
        gob_dec_oom();
    *out = ep;
    Error err = BURROW_NO_ERROR;
    const GobWireType *w = gob_wire(d, id);
    if (w != NULL && w->t[GOB_W_STRUCT] != NULL)
        err = gob_compile_dec(d, id, gob_user_type(&gob_empty_struct_desc), ep);
    else
        *ep = gob_compile_ignore_single(d, id);
    if (BURROW_FAILED(err))
        map_del(d->ignorer, &id);
    return err;
}

/* ------------------------------------------------------------------ values */

/* decodeIgnoredValue. */
static void gob_decode_ignored_value(GobDecoder *d, int32_t wire_id) {
    GobEngine **ep = NULL;
    d->err = gob_get_ignore_engine_ptr(d, wire_id, &ep);
    if (BURROW_FAILED(d->err))
        return;
    const GobWireType *w = gob_wire(d, wire_id);
    const GobEngine *e = gob_engine(ep);
    if (w != NULL && w->t[GOB_W_STRUCT] != NULL) {
        gob_ignore_struct(d, e);
        return;
    }
    if (gob_decode_uint(&d->buf) != 0)
        gob_errorf("decode: corrupted data: non-zero delta for singleton");
    gob_run(d, e->instr[0].op, e->instr[0].ovfl, NULL);
}

static void gob_decode_value_l(GobDecoder *d, int32_t wire_id, const Type *vt,
                               void *p) {
    if (vt == NULL) {
        gob_decode_ignored_value(d, wire_id);
        return;
    }
    const GobUserType *ut = gob_user_type(vt);
    const Type *base = ut->base;
    GobEngine **ep = NULL;
    d->err = gob_get_dec_engine_ptr(d, wire_id, ut, &ep);
    if (BURROW_FAILED(d->err))
        return;
    p = gob_dec_alloc(d, vt, p);
    const GobEngine *e = gob_engine(ep);
    if (base->kind == KIND_STRUCT && ut->external_dec == 0) {
        const GobWireType *wt = gob_wire(d, wire_id);
        if (e->num_instr == 0 && base->nfield > 0 && wt != NULL) {
            if (wt->t[GOB_W_STRUCT] == NULL)
                gob_nil_deref();
            if (wt->t[GOB_W_STRUCT]->nfield > 0)
                gob_errorf("type mismatch: no fields matched compiling decoder for %s",
                           burrow__gob_type_name(base));
        }
        gob_decode_struct(d, e, p);
    } else {
        gob_decode_single(d, e, p);
    }
}

/* decodeValue: vt NULL is Go's invalid Value, and p is where a vt lives. */
static void gob_decode_value(GobDecoder *d, int32_t wire_id, const Type *vt, void *p) {
    int saved_depth = d->depth;
    int saved_ignore = d->ignore_depth;
    Error caught = BURROW_NO_ERROR;
    volatile bool failed = false;
    BURROW_TRY {
        gob_decode_value_l(d, wire_id, vt, p);
    }
    BURROW_CATCH(e) {
        d->depth = saved_depth;
        d->ignore_depth = saved_ignore;
        gob_catch(e, &caught);
        failed = true;
    }
    BURROW_TRY_END;
    if (failed)
        d->err = caught;
}

/* ----------------------------------------------------------------- the API */

GobDecoder *gob_new_decoder(Alloc *a, IoReader r) {
    GobDecoder *d = BURROW_NEW(a, GobDecoder);
    if (d == NULL)
        return NULL;
    d->a = a;
    arena_init(&d->ar, a, 0);
    arena_init(&d->scratch, a, 0);
    d->ca = arena_allocator(&d->ar);
    d->sa = arena_allocator(&d->scratch);
    if (burrow__io_read_byte_method(r) != NULL) {
        d->r = r;
    } else {
        d->owned = bufio_new_reader(a, r);
        if (d->owned != NULL)
            d->r = bufio_reader_as_io_reader(d->owned);
    }
    d->wire = map_make(a, TYPE_INT32, TYPE_UINTPTR, 8);
    d->cache = map_make(a, TYPE_UINTPTR, TYPE_UINTPTR, 8);
    d->ignorer = map_make(a, TYPE_INT32, TYPE_UINTPTR, 4);
    if (d->r.vt == NULL || d->wire == NULL || d->cache == NULL || d->ignorer == NULL) {
        gob_decoder_free(d);
        return NULL;
    }
    return d;
}

void gob_decoder_free(GobDecoder *d) {
    if (d == NULL)
        return;
    Alloc *a = d->a;
    if (d->cache != NULL) {
        const void *k = NULL;
        void *v = NULL;
        for (MapIter it = map_iter(d->cache); map_next(&it, &k, &v);)
            map_free((Map *)*(Uintptr *)v);
        map_free(d->cache);
    }
    if (d->wire != NULL)
        map_free(d->wire);
    if (d->ignorer != NULL)
        map_free(d->ignorer);
    if (d->owned != NULL)
        bufio_reader_free(d->owned);
    mem_free(a, d->store, (size_t)d->cap, 1);
    arena_free(&d->ar);
    arena_free(&d->scratch);
    mem_free(a, d, sizeof *d, _Alignof(GobDecoder));
}

static void gob_decode_top(GobDecoder *d, Any v) {
    int32_t id = gob_decode_type_sequence(d, false);
    if (BURROW_FAILED(d->err))
        return;
    if (v.t == NULL) {
        gob_decode_value(d, id, NULL, NULL);
        return;
    }
    const Type *pt = burrow__gob_ptr_to(v.t);
    if (pt == NULL) {
        d->err = burrow_err_out_of_memory;
        return;
    }
    void *pv = v.data;
    gob_decode_value(d, id, pt, &pv);
}

Error gob_decoder_decode_value(GobDecoder *d, Any v) {
    if (v.t != NULL && v.data == NULL)
        return errors_new(error_allocator(),
                          BURROW_S("gob: DecodeValue of unassignable value"));
    sync_mutex_lock(&d->mu);
    d->buf = (GobDecBuf){d->store, 0, 0};
    d->err = BURROW_NO_ERROR;
    d->depth = 0;
    d->ignore_depth = 0;
    d->stack_floor = 0;
    BURROW_TRY {
        gob_decode_top(d, v);
    }
    BURROW_CATCH(p) {
        /* Not one of gob's own, which are all caught further in: a panic in
         * a GobDecode method, say. It goes on up without the lock held, as
         * Go's deferred Unlock would leave it. */
        arena_reset(&d->scratch);
        sync_mutex_unlock(&d->mu);
        panic(p);
    }
    BURROW_TRY_END;
    arena_reset(&d->scratch);
    Error err = d->err;
    sync_mutex_unlock(&d->mu);
    return err;
}

Error gob_decoder_decode(GobDecoder *d, Any v) {
    return gob_decoder_decode_value(d, v);
}
