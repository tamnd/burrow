/* internal/profile: profile.proto in memory, and its encoder, decoder and
 * merger.
 *
 * Derived from Go's src/internal/profile: profile.go, encode.go, proto.go,
 * merge.go, filter.go and prune.go. Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "pprof_profile.h"

#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PF_S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- support */

static const Type pf_line_desc = {
    .name = BURROW_S_INIT("Line"),
    .pkg_path = BURROW_S_INIT("internal/profile"),
    .kind = KIND_STRUCT,
    .size = (uint32_t)sizeof(PfLine),
    .align = (uint16_t)_Alignof(PfLine),
};

static const Type pf_label_desc = {
    .name = BURROW_S_INIT("Label"),
    .pkg_path = BURROW_S_INIT("internal/profile"),
    .kind = KIND_STRUCT,
    .size = (uint32_t)sizeof(PfLabel),
    .align = (uint16_t)_Alignof(PfLabel),
};

static BURROW_NORETURN void pf_panic_err(Error e) {
    Error *box = mem_alloc(error_allocator(), sizeof *box, _Alignof(Error));
    if (box == NULL)
        panic_str(error_text(e));
    *box = e;
    panic(BURROW_ANY(TYPE_ERROR, box));
}

/* BURROW_NEW that panics when a says no, the way Go does when memory runs
 * out. */
static void *pf_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        panic_str(PF_S("internal/profile: out of memory"));
    return p;
}

#define PF_NEW(a, T) ((T *)pf_alloc((a), sizeof(T), _Alignof(T)))

static Error pf_err(Str text) {
    return errors_new(error_allocator(), text);
}

static Slice pf_ptrs(void) {
    return slice_nil(TYPE_UNSAFE_POINTER);
}

static void *pf_ptr_at(Slice s, Int i) {
    return BURROW_AT(void *, s, i);
}

static Slice pf_append_ptr(Alloc *a, Slice s, void *p) {
    return BURROW_APPEND(void *, a, s, p);
}

/* Go's int64(f) for a float64 f. Out of range and NaN give what amd64 gives,
 * the most negative int64, since C leaves them undefined. */
static int64_t pf_f64_to_i64(double f) {
    if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)f;
}

static void *pf_map_get_ptr(Map *m, const void *key) {
    void *v = NULL;
    if (!map_get2(m, key, &v))
        return NULL;
    return v;
}

static void pf_map_set_ptr(Map *m, const void *key, void *v) {
    if (!map_set(m, key, &v))
        panic_str(PF_S("internal/profile: out of memory"));
}

const Type *burrow__pf_line_type(void) {
    return &pf_line_desc;
}

static PfValueType *pf_new_value_type(Alloc *a) {
    return PF_NEW(a, PfValueType);
}

static PfSample *pf_new_sample(Alloc *a) {
    PfSample *s = PF_NEW(a, PfSample);
    s->location = pf_ptrs();
    s->value = slice_nil(TYPE_INT64);
    s->label = pf_ptrs();
    s->num_label = pf_ptrs();
    s->num_unit = pf_ptrs();
    s->location_idx = slice_nil(TYPE_UINT64);
    s->label_x = slice_nil(&pf_label_desc);
    return s;
}

static PfLocation *pf_new_location(Alloc *a) {
    PfLocation *l = PF_NEW(a, PfLocation);
    l->line = slice_nil(&pf_line_desc);
    return l;
}

PfProfile *burrow__pf_new_profile(Alloc *a) {
    PfProfile *p = PF_NEW(a, PfProfile);
    p->a = a;
    p->sample_type = pf_ptrs();
    p->sample = pf_ptrs();
    p->mapping = pf_ptrs();
    p->location = pf_ptrs();
    p->function = pf_ptrs();
    p->comments = slice_nil(TYPE_STRING);
    p->comment_x = slice_nil(TYPE_INT64);
    p->string_table = slice_nil(TYPE_STRING);
    return p;
}

PfStrLabels *burrow__pf_labels_get(Slice labels, Str key) {
    for (Int i = 0; i < labels.len; i++) {
        PfStrLabels *l = pf_ptr_at(labels, i);
        if (str_eq(l->key, key))
            return l;
    }
    return NULL;
}

PfNumLabels *burrow__pf_num_labels_get(Slice labels, Str key) {
    for (Int i = 0; i < labels.len; i++) {
        PfNumLabels *l = pf_ptr_at(labels, i);
        if (str_eq(l->key, key))
            return l;
    }
    return NULL;
}

static PfStrLabels *pf_labels_entry(Alloc *a, Slice *labels, Str key) {
    PfStrLabels *l = burrow__pf_labels_get(*labels, key);
    if (l == NULL) {
        l = PF_NEW(a, PfStrLabels);
        l->key = key;
        l->values = slice_nil(TYPE_STRING);
        *labels = pf_append_ptr(a, *labels, l);
    }
    return l;
}

static PfNumLabels *pf_num_labels_entry(Alloc *a, Slice *labels, Str key) {
    PfNumLabels *l = burrow__pf_num_labels_get(*labels, key);
    if (l == NULL) {
        l = PF_NEW(a, PfNumLabels);
        l->key = key;
        l->values = slice_nil(TYPE_INT64);
        *labels = pf_append_ptr(a, *labels, l);
    }
    return l;
}

void burrow__pf_labels_add(Alloc *a, Slice *labels, Str key, Str v) {
    PfStrLabels *l = pf_labels_entry(a, labels, key);
    l->values = BURROW_APPEND(Str, a, l->values, v);
}

void burrow__pf_num_labels_add(Alloc *a, Slice *labels, Str key, int64_t v) {
    PfNumLabels *l = pf_num_labels_entry(a, labels, key);
    l->values = BURROW_APPEND(int64_t, a, l->values, v);
}

/* ------------------------------------------------------------------ proto */

Slice burrow__pf_marshal(Alloc *a, const PfMessage *mt, const void *m) {
    PfBuffer b = {0};
    b.a = a;
    b.data = slice_nil(TYPE_BYTE);
    mt->encode(&b, m);
    return b.data;
}

static void pf_append_byte(PfBuffer *b, Byte c) {
    b->data = BURROW_APPEND(Byte, b->a, b->data, c);
}

void burrow__pf_encode_varint(PfBuffer *b, uint64_t x) {
    while (x >= 128) {
        pf_append_byte(b, (Byte)x | 0x80);
        x >>= 7;
    }
    pf_append_byte(b, (Byte)x);
}

void burrow__pf_encode_length(PfBuffer *b, Int tag, Int len) {
    burrow__pf_encode_varint(b, (uint64_t)tag << 3 | 2);
    burrow__pf_encode_varint(b, (uint64_t)len);
}

void burrow__pf_encode_uint64(PfBuffer *b, Int tag, uint64_t x) {
    /* append varint to b.data */
    burrow__pf_encode_varint(b, (uint64_t)tag << 3 | 0);
    burrow__pf_encode_varint(b, x);
}

/* Moves the length prefix written at data[n2:] in front of the n2-n1 bytes of
 * the message or packed values at data[n1:n2]. */
static void pf_move_length(PfBuffer *b, Int n1, Int n2) {
    Int n3 = b->data.len;
    Byte *d = b->data.p;
    memcpy(b->tmp, d + n2, (size_t)(n3 - n2));
    memmove(d + n1 + (n3 - n2), d + n1, (size_t)(n2 - n1));
    memcpy(d + n1, b->tmp, (size_t)(n3 - n2));
}

void burrow__pf_encode_uint64s(PfBuffer *b, Int tag, Slice x) {
    if (x.len > 2) {
        /* Use packed encoding */
        Int n1 = b->data.len;
        for (Int i = 0; i < x.len; i++)
            burrow__pf_encode_varint(b, BURROW_AT(uint64_t, x, i));
        Int n2 = b->data.len;
        burrow__pf_encode_length(b, tag, n2 - n1);
        pf_move_length(b, n1, n2);
        return;
    }
    for (Int i = 0; i < x.len; i++)
        burrow__pf_encode_uint64(b, tag, BURROW_AT(uint64_t, x, i));
}

void burrow__pf_encode_uint64_opt(PfBuffer *b, Int tag, uint64_t x) {
    if (x == 0)
        return;
    burrow__pf_encode_uint64(b, tag, x);
}

void burrow__pf_encode_int64(PfBuffer *b, Int tag, int64_t x) {
    uint64_t u = (uint64_t)x;
    burrow__pf_encode_uint64(b, tag, u);
}

void burrow__pf_encode_int64_opt(PfBuffer *b, Int tag, int64_t x) {
    if (x == 0)
        return;
    burrow__pf_encode_int64(b, tag, x);
}

void burrow__pf_encode_int64s(PfBuffer *b, Int tag, Slice x) {
    if (x.len > 2) {
        /* Use packed encoding */
        Int n1 = b->data.len;
        for (Int i = 0; i < x.len; i++)
            burrow__pf_encode_varint(b, (uint64_t)BURROW_AT(int64_t, x, i));
        Int n2 = b->data.len;
        burrow__pf_encode_length(b, tag, n2 - n1);
        pf_move_length(b, n1, n2);
        return;
    }
    for (Int i = 0; i < x.len; i++)
        burrow__pf_encode_int64(b, tag, BURROW_AT(int64_t, x, i));
}

void burrow__pf_encode_string(PfBuffer *b, Int tag, Str x) {
    burrow__pf_encode_length(b, tag, x.len);
    b->data = slice_append(b->a, b->data, x.p, x.len);
}

void burrow__pf_encode_strings(PfBuffer *b, Int tag, Slice x) {
    for (Int i = 0; i < x.len; i++)
        burrow__pf_encode_string(b, tag, BURROW_AT(Str, x, i));
}

void burrow__pf_encode_bool(PfBuffer *b, Int tag, bool x) {
    if (x)
        burrow__pf_encode_uint64(b, tag, 1);
    else
        burrow__pf_encode_uint64(b, tag, 0);
}

void burrow__pf_encode_bool_opt(PfBuffer *b, Int tag, bool x) {
    if (!x)
        return;
    burrow__pf_encode_bool(b, tag, x);
}

void burrow__pf_encode_message(PfBuffer *b, Int tag, const PfMessage *mt,
                               const void *m) {
    Int n1 = b->data.len;
    mt->encode(b, m);
    Int n2 = b->data.len;
    burrow__pf_encode_length(b, tag, n2 - n1);
    pf_move_length(b, n1, n2);
}

Error burrow__pf_unmarshal(Alloc *a, Slice data, const PfMessage *mt, void *m) {
    PfBuffer b = {0};
    b.data = data;
    b.typ = 2;
    b.a = a;
    return burrow__pf_decode_message(&b, mt, m);
}

static uint64_t pf_le64(const Byte *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static uint32_t pf_le32(const Byte *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* Decodes the varint at the start of *data into *u and moves *data past it. */
static Error pf_decode_varint(Slice *data, uint64_t *u) {
    const Byte *d = data->p;
    uint64_t x = 0;
    for (Int i = 0;; i++) {
        if (i >= 10 || i >= data->len)
            return pf_err(PF_S("bad varint"));
        x |= (uint64_t)(d[i] & 0x7F) << (unsigned)(7 * i);
        if ((d[i] & 0x80) == 0) {
            *u = x;
            *data = slice_sub(*data, i + 1, data->len);
            return BURROW_NO_ERROR;
        }
    }
}

static Int pf_peek_num_varints(Slice data) {
    Int num_varints = 0;
    for (; data.len > 0; num_varints++) {
        uint64_t u = 0;
        if (BURROW_FAILED(pf_decode_varint(&data, &u)))
            break;
    }
    return num_varints;
}

static Error pf_decode_field(PfBuffer *b, Slice *data) {
    uint64_t x = 0;
    Error err = pf_decode_varint(data, &x);
    if (BURROW_FAILED(err))
        return err;
    b->field = (Int)(x >> 3);
    b->typ = (Int)(x & 7);
    b->data = slice_nil(TYPE_BYTE);
    b->u64 = 0;
    switch (b->typ) {
    case 0:
        err = pf_decode_varint(data, &b->u64);
        if (BURROW_FAILED(err))
            return err;
        break;
    case 1:
        if (data->len < 8)
            return pf_err(PF_S("not enough data"));
        b->u64 = pf_le64(data->p);
        *data = slice_sub(*data, 8, data->len);
        break;
    case 2: {
        uint64_t n = 0;
        err = pf_decode_varint(data, &n);
        if (BURROW_FAILED(err))
            return err;
        if (n > (uint64_t)data->len)
            return pf_err(PF_S("too much data"));
        b->data = slice_sub(*data, 0, (Int)n);
        *data = slice_sub(*data, (Int)n, data->len);
        break;
    }
    case 5:
        if (data->len < 4)
            return pf_err(PF_S("not enough data"));
        b->u64 = (uint64_t)pf_le32(data->p);
        *data = slice_sub(*data, 4, data->len);
        break;
    default:
        return fmt_errorf_v("unknown wire type: %d", (int64_t)b->typ);
    }
    return BURROW_NO_ERROR;
}

static Error pf_check_type(PfBuffer *b, Int typ) {
    if (b->typ != typ)
        return pf_err(PF_S("type mismatch"));
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_message(PfBuffer *b, const PfMessage *mt, void *m) {
    Error err = pf_check_type(b, 2);
    if (BURROW_FAILED(err))
        return err;
    Slice data = b->data;
    while (data.len > 0) {
        /* pull varint field# + type */
        err = pf_decode_field(b, &data);
        if (BURROW_FAILED(err))
            return err;
        if (b->field >= mt->ndecoders || mt->decoders[b->field] == NULL)
            continue;
        err = mt->decoders[b->field](b, m);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_int64(PfBuffer *b, int64_t *x) {
    Error err = pf_check_type(b, 0);
    if (BURROW_FAILED(err))
        return err;
    *x = (int64_t)b->u64;
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_int64s(PfBuffer *b, Slice *x) {
    if (b->typ == 2) {
        /* Packed encoding */
        Int data_len = pf_peek_num_varints(b->data);
        *x = slices_grow(b->a, *x, data_len);

        Slice data = b->data;
        while (data.len > 0) {
            uint64_t u = 0;
            Error err = pf_decode_varint(&data, &u);
            if (BURROW_FAILED(err))
                return err;
            *x = BURROW_APPEND(int64_t, b->a, *x, (int64_t)u);
        }
        return BURROW_NO_ERROR;
    }
    int64_t i;
    Error err = burrow__pf_decode_int64(b, &i);
    if (BURROW_FAILED(err))
        return err;
    *x = BURROW_APPEND(int64_t, b->a, *x, i);
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_uint64(PfBuffer *b, uint64_t *x) {
    Error err = pf_check_type(b, 0);
    if (BURROW_FAILED(err))
        return err;
    *x = b->u64;
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_uint64s(PfBuffer *b, Slice *x) {
    if (b->typ == 2) {
        /* Packed encoding */
        Int data_len = pf_peek_num_varints(b->data);
        *x = slices_grow(b->a, *x, data_len);

        Slice data = b->data;
        while (data.len > 0) {
            uint64_t u = 0;
            Error err = pf_decode_varint(&data, &u);
            if (BURROW_FAILED(err))
                return err;
            *x = BURROW_APPEND(uint64_t, b->a, *x, u);
        }
        return BURROW_NO_ERROR;
    }
    uint64_t u = 0;
    Error err = burrow__pf_decode_uint64(b, &u);
    if (BURROW_FAILED(err))
        return err;
    *x = BURROW_APPEND(uint64_t, b->a, *x, u);
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_string(PfBuffer *b, Str *x) {
    Error err = pf_check_type(b, 2);
    if (BURROW_FAILED(err))
        return err;
    *x = str_from_slice(b->a, b->data);
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_strings(PfBuffer *b, Slice *x) {
    Str s;
    Error err = burrow__pf_decode_string(b, &s);
    if (BURROW_FAILED(err))
        return err;
    *x = BURROW_APPEND(Str, b->a, *x, s);
    return BURROW_NO_ERROR;
}

Error burrow__pf_decode_bool(PfBuffer *b, bool *x) {
    Error err = pf_check_type(b, 0);
    if (BURROW_FAILED(err))
        return err;
    if ((int64_t)b->u64 == 0)
        *x = false;
    else
        *x = true;
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- encode */

static int64_t pf_add_string(Alloc *a, Map *strings, Slice *table, Str s) {
    int32_t i;
    if (!map_get2(strings, &s, &i)) {
        i = (int32_t)map_len(strings);
        if (!map_set(strings, &s, &i))
            panic_str(PF_S("internal/profile: out of memory"));
        *table = BURROW_APPEND(Str, a, *table, s);
    }
    return (int64_t)i;
}

static Slice pf_sorted_keys(Alloc *a, Slice labels) {
    Slice keys = slice_nil(TYPE_STRING);
    for (Int i = 0; i < labels.len; i++)
        keys = BURROW_APPEND(Str, a, keys, ((PfStrLabels *)pf_ptr_at(labels, i))->key);
    sort_strings(keys);
    return keys;
}

static Slice pf_sorted_num_keys(Alloc *a, Slice labels) {
    Slice keys = slice_nil(TYPE_STRING);
    for (Int i = 0; i < labels.len; i++)
        keys = BURROW_APPEND(Str, a, keys, ((PfNumLabels *)pf_ptr_at(labels, i))->key);
    sort_strings(keys);
    return keys;
}

/* preEncode populates the unexported fields to be used by encode
 * (with suffix X) from the corresponding exported fields. The
 * exported fields are cleared up to facilitate testing. */
static void pf_pre_encode(PfProfile *p) {
    Alloc *a = p->a;
    Map *strings = map_make(a, TYPE_STRING, TYPE_INT32, 0);
    Slice table = slice_nil(TYPE_STRING);
    pf_add_string(a, strings, &table, PF_S(""));

    for (Int i = 0; i < p->sample_type.len; i++) {
        PfValueType *st = pf_ptr_at(p->sample_type, i);
        st->type_x = pf_add_string(a, strings, &table, st->type);
        st->unit_x = pf_add_string(a, strings, &table, st->unit);
    }

    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        s->label_x = slice_nil(&pf_label_desc);
        Slice keys = pf_sorted_keys(a, s->label);
        for (Int j = 0; j < keys.len; j++) {
            Str k = BURROW_AT(Str, keys, j);
            /* The keys came from these labels, so this always finds one. */
            const PfStrLabels *ls = burrow__pf_labels_get(s->label, k);
            if (ls == NULL)
                continue;
            Slice vs = ls->values;
            for (Int n = 0; n < vs.len; n++) {
                PfLabel l = {0};
                l.key_x = pf_add_string(a, strings, &table, k);
                l.str_x = pf_add_string(a, strings, &table, BURROW_AT(Str, vs, n));
                s->label_x = BURROW_APPEND(PfLabel, a, s->label_x, l);
            }
        }
        Slice num_keys = pf_sorted_num_keys(a, s->num_label);
        for (Int j = 0; j < num_keys.len; j++) {
            Str k = BURROW_AT(Str, num_keys, j);
            /* The keys came from these labels, so this always finds one. */
            const PfNumLabels *ls = burrow__pf_num_labels_get(s->num_label, k);
            if (ls == NULL)
                continue;
            Slice vs = ls->values;
            for (Int n = 0; n < vs.len; n++) {
                PfLabel l = {0};
                l.key_x = pf_add_string(a, strings, &table, k);
                l.num_x = BURROW_AT(int64_t, vs, n);
                s->label_x = BURROW_APPEND(PfLabel, a, s->label_x, l);
            }
        }
        s->location_idx = slice_nil(TYPE_UINT64);
        for (Int j = 0; j < s->location.len; j++) {
            PfLocation *l = pf_ptr_at(s->location, j);
            s->location_idx = BURROW_APPEND(uint64_t, a, s->location_idx, l->id);
        }
    }

    for (Int i = 0; i < p->mapping.len; i++) {
        PfMapping *m = pf_ptr_at(p->mapping, i);
        m->file_x = pf_add_string(a, strings, &table, m->file);
        m->build_id_x = pf_add_string(a, strings, &table, m->build_id);
    }

    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        for (Int j = 0; j < l->line.len; j++) {
            PfLine *ln = &BURROW_AT(PfLine, l->line, j);
            if (ln->function != NULL)
                ln->function_idx = ln->function->id;
            else
                ln->function_idx = 0;
        }
        if (l->mapping != NULL)
            l->mapping_idx = l->mapping->id;
        else
            l->mapping_idx = 0;
    }
    for (Int i = 0; i < p->function.len; i++) {
        PfFunction *f = pf_ptr_at(p->function, i);
        f->name_x = pf_add_string(a, strings, &table, f->name);
        f->system_name_x = pf_add_string(a, strings, &table, f->system_name);
        f->filename_x = pf_add_string(a, strings, &table, f->filename);
    }

    p->drop_frames_x = pf_add_string(a, strings, &table, p->drop_frames);
    p->keep_frames_x = pf_add_string(a, strings, &table, p->keep_frames);

    PfValueType *pt = p->period_type;
    if (pt != NULL) {
        pt->type_x = pf_add_string(a, strings, &table, pt->type);
        pt->unit_x = pf_add_string(a, strings, &table, pt->unit);
    }

    p->string_table = table;
    map_free(strings);
}

static Str pf_get_string(Slice strings, int64_t *strng, Error *err) {
    if (BURROW_FAILED(*err))
        return PF_S("");
    int64_t s = *strng;
    if (s < 0 || s >= strings.len) {
        *err = burrow__pf_err_malformed;
        return PF_S("");
    }
    *strng = 0;
    return BURROW_AT(Str, strings, (Int)s);
}

/* postDecode takes the unexported fields populated by decode (with
 * suffix X) and populates the corresponding exported fields.
 * The unexported fields are cleared up to facilitate testing. */
static Error pf_post_decode(PfProfile *p) {
    Alloc *a = p->a;
    Error err = BURROW_NO_ERROR;
    Slice st = p->string_table;

    Map *mappings = map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, 0);
    for (Int i = 0; i < p->mapping.len; i++) {
        PfMapping *m = pf_ptr_at(p->mapping, i);
        m->file = pf_get_string(st, &m->file_x, &err);
        m->build_id = pf_get_string(st, &m->build_id_x, &err);
        pf_map_set_ptr(mappings, &m->id, m);
    }

    Map *functions = map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, 0);
    for (Int i = 0; i < p->function.len; i++) {
        PfFunction *f = pf_ptr_at(p->function, i);
        f->name = pf_get_string(st, &f->name_x, &err);
        f->system_name = pf_get_string(st, &f->system_name_x, &err);
        f->filename = pf_get_string(st, &f->filename_x, &err);
        pf_map_set_ptr(functions, &f->id, f);
    }

    Map *locations = map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, 0);
    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        l->mapping = pf_map_get_ptr(mappings, &l->mapping_idx);
        l->mapping_idx = 0;
        for (Int j = 0; j < l->line.len; j++) {
            PfLine *ln = &BURROW_AT(PfLine, l->line, j);
            uint64_t id = ln->function_idx;
            if (id != 0) {
                ln->function = pf_map_get_ptr(functions, &id);
                if (ln->function == NULL)
                    return fmt_errorf_v("function ID %d not found", id);
                ln->function_idx = 0;
            }
        }
        pf_map_set_ptr(locations, &l->id, l);
    }

    for (Int i = 0; i < p->sample_type.len; i++) {
        PfValueType *vt = pf_ptr_at(p->sample_type, i);
        vt->type = pf_get_string(st, &vt->type_x, &err);
        vt->unit = pf_get_string(st, &vt->unit_x, &err);
    }

    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        Slice labels = pf_ptrs();
        Slice num_labels = pf_ptrs();
        for (Int j = 0; j < s->label_x.len; j++) {
            PfLabel *l = &BURROW_AT(PfLabel, s->label_x, j);
            Str key = pf_get_string(st, &l->key_x, &err);
            if (l->str_x != 0) {
                Str value = pf_get_string(st, &l->str_x, &err);
                burrow__pf_labels_add(a, &labels, key, value);
            } else {
                burrow__pf_num_labels_add(a, &num_labels, key, l->num_x);
            }
        }
        if (labels.len > 0)
            s->label = labels;
        if (num_labels.len > 0)
            s->num_label = num_labels;
        s->location = pf_ptrs();
        for (Int j = 0; j < s->location_idx.len; j++) {
            uint64_t lid = BURROW_AT(uint64_t, s->location_idx, j);
            s->location =
                pf_append_ptr(a, s->location, pf_map_get_ptr(locations, &lid));
        }
        s->location_idx = slice_nil(TYPE_UINT64);
    }

    p->drop_frames = pf_get_string(st, &p->drop_frames_x, &err);
    p->keep_frames = pf_get_string(st, &p->keep_frames_x, &err);

    if (p->period_type == NULL)
        p->period_type = pf_new_value_type(a);

    PfValueType *pt = p->period_type;
    pt->type = pf_get_string(st, &pt->type_x, &err);
    pt->unit = pf_get_string(st, &pt->unit_x, &err);
    for (Int i = 0; i < p->comment_x.len; i++) {
        int64_t idx = BURROW_AT(int64_t, p->comment_x, i);
        Str c = pf_get_string(st, &idx, &err);
        p->comments = BURROW_APPEND(Str, a, p->comments, c);
    }

    p->comment_x = slice_nil(TYPE_INT64);
    p->default_sample_type = pf_get_string(st, &p->default_sample_type_x, &err);
    p->string_table = slice_nil(TYPE_STRING);
    map_free(mappings);
    map_free(functions);
    map_free(locations);
    return err;
}

static void pf_value_type_encode(PfBuffer *b, const void *m) {
    const PfValueType *p = m;
    burrow__pf_encode_int64_opt(b, 1, p->type_x);
    burrow__pf_encode_int64_opt(b, 2, p->unit_x);
}

/* optional int64 type = 1 */
static Error pf_value_type_dec_type(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfValueType *)m)->type_x);
}

/* optional int64 unit = 2 */
static Error pf_value_type_dec_unit(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfValueType *)m)->unit_x);
}

static const PfDecoder pf_value_type_decoder[] = {
    NULL, /* 0 */
    pf_value_type_dec_type,
    pf_value_type_dec_unit,
};

static const PfMessage pf_value_type_msg = {
    pf_value_type_decoder,
    (Int)(sizeof pf_value_type_decoder / sizeof pf_value_type_decoder[0]),
    pf_value_type_encode,
};

static void pf_label_encode(PfBuffer *b, const void *m) {
    const PfLabel *p = m;
    burrow__pf_encode_int64_opt(b, 1, p->key_x);
    burrow__pf_encode_int64_opt(b, 2, p->str_x);
    burrow__pf_encode_int64_opt(b, 3, p->num_x);
}

/* optional int64 key = 1 */
static Error pf_label_dec_key(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfLabel *)m)->key_x);
}

/* optional int64 str = 2 */
static Error pf_label_dec_str(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfLabel *)m)->str_x);
}

/* optional int64 num = 3 */
static Error pf_label_dec_num(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfLabel *)m)->num_x);
}

static const PfDecoder pf_label_decoder[] = {
    NULL, /* 0 */
    pf_label_dec_key,
    pf_label_dec_str,
    pf_label_dec_num,
};

static const PfMessage pf_label_msg = {
    pf_label_decoder,
    (Int)(sizeof pf_label_decoder / sizeof pf_label_decoder[0]),
    pf_label_encode,
};

static void pf_sample_encode(PfBuffer *b, const void *m) {
    const PfSample *p = m;
    burrow__pf_encode_uint64s(b, 1, p->location_idx);
    for (Int i = 0; i < p->value.len; i++)
        burrow__pf_encode_int64(b, 2, BURROW_AT(int64_t, p->value, i));
    for (Int i = 0; i < p->label_x.len; i++)
        burrow__pf_encode_message(b, 3, &pf_label_msg,
                                  &BURROW_AT(PfLabel, p->label_x, i));
}

/* repeated uint64 location = 1 */
static Error pf_sample_dec_location(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64s(b, &((PfSample *)m)->location_idx);
}

/* repeated int64 value = 2 */
static Error pf_sample_dec_value(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64s(b, &((PfSample *)m)->value);
}

/* repeated Label label = 3 */
static Error pf_sample_dec_label(PfBuffer *b, void *m) {
    PfSample *s = m;
    Int n = s->label_x.len;
    s->label_x = BURROW_APPEND(PfLabel, b->a, s->label_x, (PfLabel){0});
    return burrow__pf_decode_message(b, &pf_label_msg,
                                     &BURROW_AT(PfLabel, s->label_x, n));
}

static const PfDecoder pf_sample_decoder[] = {
    NULL, /* 0 */
    pf_sample_dec_location,
    pf_sample_dec_value,
    pf_sample_dec_label,
};

static const PfMessage pf_sample_msg = {
    pf_sample_decoder,
    (Int)(sizeof pf_sample_decoder / sizeof pf_sample_decoder[0]),
    pf_sample_encode,
};

static void pf_mapping_encode(PfBuffer *b, const void *m) {
    const PfMapping *p = m;
    burrow__pf_encode_uint64_opt(b, 1, p->id);
    burrow__pf_encode_uint64_opt(b, 2, p->start);
    burrow__pf_encode_uint64_opt(b, 3, p->limit);
    burrow__pf_encode_uint64_opt(b, 4, p->offset);
    burrow__pf_encode_int64_opt(b, 5, p->file_x);
    burrow__pf_encode_int64_opt(b, 6, p->build_id_x);
    burrow__pf_encode_bool_opt(b, 7, p->has_functions);
    burrow__pf_encode_bool_opt(b, 8, p->has_filenames);
    burrow__pf_encode_bool_opt(b, 9, p->has_line_numbers);
    burrow__pf_encode_bool_opt(b, 10, p->has_inline_frames);
}

/* optional uint64 id = 1 */
static Error pf_mapping_dec_id(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfMapping *)m)->id);
}

/* optional uint64 memory_offset = 2 */
static Error pf_mapping_dec_start(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfMapping *)m)->start);
}

/* optional uint64 memory_limit = 3 */
static Error pf_mapping_dec_limit(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfMapping *)m)->limit);
}

/* optional uint64 file_offset = 4 */
static Error pf_mapping_dec_offset(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfMapping *)m)->offset);
}

/* optional int64 filename = 5 */
static Error pf_mapping_dec_file(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfMapping *)m)->file_x);
}

/* optional int64 build_id = 6 */
static Error pf_mapping_dec_build_id(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfMapping *)m)->build_id_x);
}

/* optional bool has_functions = 7 */
static Error pf_mapping_dec_has_functions(PfBuffer *b, void *m) {
    return burrow__pf_decode_bool(b, &((PfMapping *)m)->has_functions);
}

/* optional bool has_filenames = 8 */
static Error pf_mapping_dec_has_filenames(PfBuffer *b, void *m) {
    return burrow__pf_decode_bool(b, &((PfMapping *)m)->has_filenames);
}

/* optional bool has_line_numbers = 9 */
static Error pf_mapping_dec_has_line_numbers(PfBuffer *b, void *m) {
    return burrow__pf_decode_bool(b, &((PfMapping *)m)->has_line_numbers);
}

/* optional bool has_inline_frames = 10 */
static Error pf_mapping_dec_has_inline_frames(PfBuffer *b, void *m) {
    return burrow__pf_decode_bool(b, &((PfMapping *)m)->has_inline_frames);
}

static const PfDecoder pf_mapping_decoder[] = {
    NULL, /* 0 */
    pf_mapping_dec_id,
    pf_mapping_dec_start,
    pf_mapping_dec_limit,
    pf_mapping_dec_offset,
    pf_mapping_dec_file,
    pf_mapping_dec_build_id,
    pf_mapping_dec_has_functions,
    pf_mapping_dec_has_filenames,
    pf_mapping_dec_has_line_numbers,
    pf_mapping_dec_has_inline_frames,
};

static const PfMessage pf_mapping_msg = {
    pf_mapping_decoder,
    (Int)(sizeof pf_mapping_decoder / sizeof pf_mapping_decoder[0]),
    pf_mapping_encode,
};

static void pf_line_encode(PfBuffer *b, const void *m) {
    const PfLine *p = m;
    burrow__pf_encode_uint64_opt(b, 1, p->function_idx);
    burrow__pf_encode_int64_opt(b, 2, p->line);
}

/* optional uint64 function_id = 1 */
static Error pf_line_dec_function(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfLine *)m)->function_idx);
}

/* optional int64 line = 2 */
static Error pf_line_dec_line(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfLine *)m)->line);
}

static const PfDecoder pf_line_decoder[] = {
    NULL, /* 0 */
    pf_line_dec_function,
    pf_line_dec_line,
};

static const PfMessage pf_line_msg = {
    pf_line_decoder,
    (Int)(sizeof pf_line_decoder / sizeof pf_line_decoder[0]),
    pf_line_encode,
};

static void pf_location_encode(PfBuffer *b, const void *m) {
    const PfLocation *p = m;
    burrow__pf_encode_uint64_opt(b, 1, p->id);
    burrow__pf_encode_uint64_opt(b, 2, p->mapping_idx);
    burrow__pf_encode_uint64_opt(b, 3, p->address);
    for (Int i = 0; i < p->line.len; i++)
        burrow__pf_encode_message(b, 4, &pf_line_msg, &BURROW_AT(PfLine, p->line, i));
}

/* optional uint64 id = 1; */
static Error pf_location_dec_id(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfLocation *)m)->id);
}

/* optional uint64 mapping_id = 2; */
static Error pf_location_dec_mapping(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfLocation *)m)->mapping_idx);
}

/* optional uint64 address = 3; */
static Error pf_location_dec_address(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfLocation *)m)->address);
}

/* repeated Line line = 4 */
static Error pf_location_dec_line(PfBuffer *b, void *m) {
    PfLocation *pp = m;
    Int n = pp->line.len;
    pp->line = BURROW_APPEND(PfLine, b->a, pp->line, (PfLine){0});
    return burrow__pf_decode_message(b, &pf_line_msg, &BURROW_AT(PfLine, pp->line, n));
}

static const PfDecoder pf_location_decoder[] = {
    NULL, /* 0 */
    pf_location_dec_id,
    pf_location_dec_mapping,
    pf_location_dec_address,
    pf_location_dec_line,
};

static const PfMessage pf_location_msg = {
    pf_location_decoder,
    (Int)(sizeof pf_location_decoder / sizeof pf_location_decoder[0]),
    pf_location_encode,
};

static void pf_function_encode(PfBuffer *b, const void *m) {
    const PfFunction *p = m;
    burrow__pf_encode_uint64_opt(b, 1, p->id);
    burrow__pf_encode_int64_opt(b, 2, p->name_x);
    burrow__pf_encode_int64_opt(b, 3, p->system_name_x);
    burrow__pf_encode_int64_opt(b, 4, p->filename_x);
    burrow__pf_encode_int64_opt(b, 5, p->start_line);
}

/* optional uint64 id = 1 */
static Error pf_function_dec_id(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64(b, &((PfFunction *)m)->id);
}

/* optional int64 function_name = 2 */
static Error pf_function_dec_name(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfFunction *)m)->name_x);
}

/* optional int64 function_system_name = 3 */
static Error pf_function_dec_system_name(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfFunction *)m)->system_name_x);
}

/* repeated int64 filename = 4 */
static Error pf_function_dec_filename(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfFunction *)m)->filename_x);
}

/* optional int64 start_line = 5 */
static Error pf_function_dec_start_line(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfFunction *)m)->start_line);
}

static const PfDecoder pf_function_decoder[] = {
    NULL, /* 0 */
    pf_function_dec_id,
    pf_function_dec_name,
    pf_function_dec_system_name,
    pf_function_dec_filename,
    pf_function_dec_start_line,
};

static const PfMessage pf_function_msg = {
    pf_function_decoder,
    (Int)(sizeof pf_function_decoder / sizeof pf_function_decoder[0]),
    pf_function_encode,
};

static void pf_profile_encode(PfBuffer *b, const void *m) {
    const PfProfile *p = m;
    for (Int i = 0; i < p->sample_type.len; i++)
        burrow__pf_encode_message(b, 1, &pf_value_type_msg,
                                  pf_ptr_at(p->sample_type, i));
    for (Int i = 0; i < p->sample.len; i++)
        burrow__pf_encode_message(b, 2, &pf_sample_msg, pf_ptr_at(p->sample, i));
    for (Int i = 0; i < p->mapping.len; i++)
        burrow__pf_encode_message(b, 3, &pf_mapping_msg, pf_ptr_at(p->mapping, i));
    for (Int i = 0; i < p->location.len; i++)
        burrow__pf_encode_message(b, 4, &pf_location_msg, pf_ptr_at(p->location, i));
    for (Int i = 0; i < p->function.len; i++)
        burrow__pf_encode_message(b, 5, &pf_function_msg, pf_ptr_at(p->function, i));
    burrow__pf_encode_strings(b, 6, p->string_table);
    burrow__pf_encode_int64_opt(b, 7, p->drop_frames_x);
    burrow__pf_encode_int64_opt(b, 8, p->keep_frames_x);
    burrow__pf_encode_int64_opt(b, 9, p->time_nanos);
    burrow__pf_encode_int64_opt(b, 10, p->duration_nanos);
    const PfValueType *pt = p->period_type;
    if (pt != NULL && (pt->type_x != 0 || pt->unit_x != 0))
        burrow__pf_encode_message(b, 11, &pf_value_type_msg, p->period_type);
    burrow__pf_encode_int64_opt(b, 12, p->period);
}

/* repeated ValueType sample_type = 1 */
static Error pf_profile_dec_sample_type(PfBuffer *b, void *m) {
    PfValueType *x = pf_new_value_type(b->a);
    PfProfile *pp = m;
    pp->sample_type = pf_append_ptr(b->a, pp->sample_type, x);
    return burrow__pf_decode_message(b, &pf_value_type_msg, x);
}

/* repeated Sample sample = 2 */
static Error pf_profile_dec_sample(PfBuffer *b, void *m) {
    PfSample *x = pf_new_sample(b->a);
    PfProfile *pp = m;
    pp->sample = pf_append_ptr(b->a, pp->sample, x);
    return burrow__pf_decode_message(b, &pf_sample_msg, x);
}

/* repeated Mapping mapping = 3 */
static Error pf_profile_dec_mapping(PfBuffer *b, void *m) {
    PfMapping *x = PF_NEW(b->a, PfMapping);
    PfProfile *pp = m;
    pp->mapping = pf_append_ptr(b->a, pp->mapping, x);
    return burrow__pf_decode_message(b, &pf_mapping_msg, x);
}

/* repeated Location location = 4 */
static Error pf_profile_dec_location(PfBuffer *b, void *m) {
    PfLocation *x = pf_new_location(b->a);
    PfProfile *pp = m;
    pp->location = pf_append_ptr(b->a, pp->location, x);
    return burrow__pf_decode_message(b, &pf_location_msg, x);
}

/* repeated Function function = 5 */
static Error pf_profile_dec_function(PfBuffer *b, void *m) {
    PfFunction *x = PF_NEW(b->a, PfFunction);
    PfProfile *pp = m;
    pp->function = pf_append_ptr(b->a, pp->function, x);
    return burrow__pf_decode_message(b, &pf_function_msg, x);
}

/* repeated string string_table = 6 */
static Error pf_profile_dec_string_table(PfBuffer *b, void *m) {
    PfProfile *pp = m;
    Error err = burrow__pf_decode_strings(b, &pp->string_table);
    if (BURROW_FAILED(err))
        return err;
    if (BURROW_AT(Str, pp->string_table, 0).len != 0)
        return pf_err(PF_S("string_table[0] must be ''"));
    return BURROW_NO_ERROR;
}

/* repeated int64 drop_frames = 7 */
static Error pf_profile_dec_drop_frames(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->drop_frames_x);
}

/* repeated int64 keep_frames = 8 */
static Error pf_profile_dec_keep_frames(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->keep_frames_x);
}

/* repeated int64 time_nanos = 9 */
static Error pf_profile_dec_time_nanos(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->time_nanos);
}

/* repeated int64 duration_nanos = 10 */
static Error pf_profile_dec_duration_nanos(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->duration_nanos);
}

/* optional string period_type = 11 */
static Error pf_profile_dec_period_type(PfBuffer *b, void *m) {
    PfValueType *x = pf_new_value_type(b->a);
    PfProfile *pp = m;
    pp->period_type = x;
    return burrow__pf_decode_message(b, &pf_value_type_msg, x);
}

/* repeated int64 period = 12 */
static Error pf_profile_dec_period(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->period);
}

/* repeated int64 comment = 13 */
static Error pf_profile_dec_comment(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64s(b, &((PfProfile *)m)->comment_x);
}

/* int64 defaultSampleType = 14 */
static Error pf_profile_dec_default_sample_type(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64(b, &((PfProfile *)m)->default_sample_type_x);
}

static const PfDecoder pf_profile_decoder[] = {
    NULL, /* 0 */
    pf_profile_dec_sample_type,
    pf_profile_dec_sample,
    pf_profile_dec_mapping,
    pf_profile_dec_location,
    pf_profile_dec_function,
    pf_profile_dec_string_table,
    pf_profile_dec_drop_frames,
    pf_profile_dec_keep_frames,
    pf_profile_dec_time_nanos,
    pf_profile_dec_duration_nanos,
    pf_profile_dec_period_type,
    pf_profile_dec_period,
    pf_profile_dec_comment,
    pf_profile_dec_default_sample_type,
};

static const PfMessage pf_profile_msg = {
    pf_profile_decoder,
    (Int)(sizeof pf_profile_decoder / sizeof pf_profile_decoder[0]),
    pf_profile_encode,
};

/* ---------------------------------------------------------------- profile */

BURROW_SENTINEL_ERROR(burrow__pf_err_malformed, "malformed profile format");
BURROW_SENTINEL_ERROR(burrow__pf_err_no_data, "empty input file");

static PfProfile *pf_parse_uncompressed(Alloc *a, Slice data, Error *err) {
    if (data.len == 0) {
        *err = burrow__pf_err_no_data;
        return NULL;
    }

    PfProfile *p = burrow__pf_new_profile(a);
    *err = burrow__pf_unmarshal(a, data, &pf_profile_msg, p);
    if (BURROW_FAILED(*err))
        return NULL;

    *err = pf_post_decode(p);
    if (BURROW_FAILED(*err))
        return NULL;

    return p;
}

PfProfile *burrow__pf_parse(Alloc *a, IoReader r, Error *err) {
    *err = BURROW_NO_ERROR;
    Slice orig = io_read_all(a, r, err);
    if (BURROW_FAILED(*err))
        return NULL;

    const Byte *o = orig.p;
    if (orig.len >= 2 && o[0] == 0x1f && o[1] == 0x8b) {
        Error e = BURROW_NO_ERROR;
        GzipReader *gz = gzip_new_reader(
            a, bytes_reader_as_io_reader(bytes_new_reader(a, orig)), &e);
        if (BURROW_FAILED(e)) {
            *err = fmt_errorf_v("decompressing profile: %v", e);
            return NULL;
        }
        Slice data = io_read_all(a, gzip_reader_as_io_reader(gz), &e);
        gzip_reader_free(gz);
        if (BURROW_FAILED(e)) {
            *err = fmt_errorf_v("decompressing profile: %v", e);
            return NULL;
        }
        orig = data;
    }

    Error e = BURROW_NO_ERROR;
    PfProfile *p = pf_parse_uncompressed(a, orig, &e);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("parsing profile: %w", e);
        return NULL;
    }

    e = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("malformed profile: %v", e);
        return NULL;
    }
    return p;
}

Error burrow__pf_profile_write(PfProfile *p, IoWriter w) {
    pf_pre_encode(p);
    Slice b = burrow__pf_marshal(p->a, &pf_profile_msg, p);
    GzipWriter *zw = gzip_new_writer(p->a, w);
    if (zw == NULL)
        return burrow_err_out_of_memory;
    Error err = BURROW_NO_ERROR;
    gzip_writer_write(zw, b, &err);
    if (BURROW_FAILED(err)) {
        (void)gzip_writer_close(zw);
        gzip_writer_free(zw);
        return err;
    }
    err = gzip_writer_close(zw);
    gzip_writer_free(zw);
    return err;
}

Error burrow__pf_profile_check_valid(PfProfile *p) {
    /* Check that sample values are consistent */
    Int sample_len = p->sample_type.len;
    if (sample_len == 0 && p->sample.len != 0)
        return fmt_errorf_v("missing sample type information");
    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        if (s->value.len != sample_len)
            return fmt_errorf_v("mismatch: sample has: %d values vs. %d types",
                                (int64_t)s->value.len, (int64_t)p->sample_type.len);
    }

    /* Check that all mappings/locations/functions are in the tables
     * Check that there are no duplicate ids */
    Error err = BURROW_NO_ERROR;
    Map *mappings = map_make(p->a, TYPE_UINT64, TYPE_UNSAFE_POINTER, p->mapping.len);
    Map *functions = map_make(p->a, TYPE_UINT64, TYPE_UNSAFE_POINTER, p->function.len);
    Map *locations = map_make(p->a, TYPE_UINT64, TYPE_UNSAFE_POINTER, p->location.len);
    for (Int i = 0; i < p->mapping.len; i++) {
        PfMapping *m = pf_ptr_at(p->mapping, i);
        if (m->id == 0) {
            err = fmt_errorf_v("found mapping with reserved ID=0");
            goto out;
        }
        if (pf_map_get_ptr(mappings, &m->id) != NULL) {
            err = fmt_errorf_v("multiple mappings with same id: %d", m->id);
            goto out;
        }
        pf_map_set_ptr(mappings, &m->id, m);
    }
    for (Int i = 0; i < p->function.len; i++) {
        PfFunction *f = pf_ptr_at(p->function, i);
        if (f->id == 0) {
            err = fmt_errorf_v("found function with reserved ID=0");
            goto out;
        }
        if (pf_map_get_ptr(functions, &f->id) != NULL) {
            err = fmt_errorf_v("multiple functions with same id: %d", f->id);
            goto out;
        }
        pf_map_set_ptr(functions, &f->id, f);
    }
    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        if (l->id == 0) {
            err = fmt_errorf_v("found location with reserved id=0");
            goto out;
        }
        if (pf_map_get_ptr(locations, &l->id) != NULL) {
            err = fmt_errorf_v("multiple locations with same id: %d", l->id);
            goto out;
        }
        pf_map_set_ptr(locations, &l->id, l);
        PfMapping *m = l->mapping;
        if (m != NULL) {
            if (m->id == 0 || pf_map_get_ptr(mappings, &m->id) != m) {
                /* Go prints the pointer with %p. */
                err = fmt_errorf_v("inconsistent mapping %#x: %d",
                                   (uint64_t)(uintptr_t)m, m->id);
                goto out;
            }
        }
        for (Int j = 0; j < l->line.len; j++) {
            PfFunction *f = BURROW_AT(PfLine, l->line, j).function;
            if (f != NULL) {
                if (f->id == 0 || pf_map_get_ptr(functions, &f->id) != f) {
                    err = fmt_errorf_v("inconsistent function %#x: %d",
                                       (uint64_t)(uintptr_t)f, f->id);
                    goto out;
                }
            }
        }
    }
out:
    map_free(mappings);
    map_free(functions);
    map_free(locations);
    return err;
}

Error burrow__pf_profile_aggregate(PfProfile *p, bool inline_frame, bool function,
                                   bool filename, bool linenumber, bool address) {
    for (Int i = 0; i < p->mapping.len; i++) {
        PfMapping *m = pf_ptr_at(p->mapping, i);
        m->has_inline_frames = m->has_inline_frames && inline_frame;
        m->has_functions = m->has_functions && function;
        m->has_filenames = m->has_filenames && filename;
        m->has_line_numbers = m->has_line_numbers && linenumber;
    }

    /* Aggregate functions */
    if (!function || !filename) {
        for (Int i = 0; i < p->function.len; i++) {
            PfFunction *f = pf_ptr_at(p->function, i);
            if (!function) {
                f->name = PF_S("");
                f->system_name = PF_S("");
            }
            if (!filename)
                f->filename = PF_S("");
        }
    }

    /* Aggregate locations */
    if (!inline_frame || !address || !linenumber) {
        for (Int i = 0; i < p->location.len; i++) {
            PfLocation *l = pf_ptr_at(p->location, i);
            if (!inline_frame && l->line.len > 1)
                l->line = slice_sub(l->line, l->line.len - 1, l->line.len);
            if (!linenumber) {
                for (Int j = 0; j < l->line.len; j++)
                    BURROW_AT(PfLine, l->line, j).line = 0;
            }
            if (!address)
                l->address = 0;
        }
    }

    return burrow__pf_profile_check_valid(p);
}

/* fmt's %v of a []string or a []int64: the elements between brackets with a
 * space between each. */
static void pf_write_strs_v(StringsBuilder *sb, Slice v) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_byte(sb, '[');
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            strings_builder_write_byte(sb, ' ');
        strings_builder_write_string(sb, BURROW_AT(Str, v, i), &err);
    }
    strings_builder_write_byte(sb, ']');
}

static void pf_write_int64s(StringsBuilder *sb, Slice v, Int base) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_byte(sb, '[');
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            strings_builder_write_byte(sb, ' ');
        strings_builder_write_string(
            sb, strconv_format_int(sb->a, BURROW_AT(int64_t, v, i), base), &err);
    }
    strings_builder_write_byte(sb, ']');
}

Str burrow__pf_profile_string(PfProfile *p, Alloc *a) {
    StringsBuilder sb = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
#define PF_LINE(s)                                                                     \
    do {                                                                               \
        strings_builder_write_string(&sb, (s), &err);                                  \
        strings_builder_write_byte(&sb, '\n');                                         \
    } while (0)

    PfValueType *pt = p->period_type;
    if (pt != NULL)
        PF_LINE(fmt_sprintf_v(a, "PeriodType: %s %s", pt->type, pt->unit));
    PF_LINE(fmt_sprintf_v(a, "Period: %d", p->period));
    if (p->time_nanos != 0)
        PF_LINE(fmt_sprintf_v(a, "Time: %s",
                              time_string(time_from_unix(0, p->time_nanos), a)));
    if (p->duration_nanos != 0)
        PF_LINE(fmt_sprintf_v(a, "Duration: %s",
                              duration_string((Duration)p->duration_nanos, a)));

    PF_LINE(PF_S("Samples:"));
    StringsBuilder sh1 = STRINGS_BUILDER(a);
    for (Int i = 0; i < p->sample_type.len; i++) {
        PfValueType *s = pf_ptr_at(p->sample_type, i);
        strings_builder_write_string(&sh1, fmt_sprintf_v(a, "%s/%s ", s->type, s->unit),
                                     &err);
    }
    PF_LINE(strings_trim_space(strings_builder_string(&sh1)));
    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        StringsBuilder sv = STRINGS_BUILDER(a);
        for (Int j = 0; j < s->value.len; j++)
            strings_builder_write_string(
                &sv, fmt_sprintf_v(a, " %10d", BURROW_AT(int64_t, s->value, j)), &err);
        strings_builder_write_string(&sv, PF_S(": "), &err);
        for (Int j = 0; j < s->location.len; j++) {
            PfLocation *l = pf_ptr_at(s->location, j);
            strings_builder_write_string(&sv, fmt_sprintf_v(a, "%d ", l->id), &err);
        }
        PF_LINE(strings_builder_string(&sv));
        static const Str label_header = BURROW_S_INIT("                ");
        if (s->label.len > 0) {
            StringsBuilder ls = STRINGS_BUILDER(a);
            strings_builder_write_string(&ls, label_header, &err);
            for (Int j = 0; j < s->label.len; j++) {
                PfStrLabels *l = pf_ptr_at(s->label, j);
                strings_builder_write_string(&ls, l->key, &err);
                strings_builder_write_byte(&ls, ':');
                pf_write_strs_v(&ls, l->values);
                strings_builder_write_byte(&ls, ' ');
            }
            PF_LINE(strings_builder_string(&ls));
        }
        if (s->num_label.len > 0) {
            StringsBuilder ls = STRINGS_BUILDER(a);
            strings_builder_write_string(&ls, label_header, &err);
            for (Int j = 0; j < s->num_label.len; j++) {
                PfNumLabels *l = pf_ptr_at(s->num_label, j);
                strings_builder_write_string(&ls, l->key, &err);
                strings_builder_write_byte(&ls, ':');
                pf_write_int64s(&ls, l->values, 10);
                strings_builder_write_byte(&ls, ' ');
            }
            PF_LINE(strings_builder_string(&ls));
        }
    }

    PF_LINE(PF_S("Locations"));
    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        Str loc_str = fmt_sprintf_v(a, "%6d: %#x ", l->id, l->address);
        PfMapping *m = l->mapping;
        if (m != NULL)
            loc_str = fmt_sprintf_v(a, "%sM=%d ", loc_str, m->id);
        if (l->line.len == 0)
            PF_LINE(loc_str);
        for (Int li = 0; li < l->line.len; li++) {
            PfLine *ln = &BURROW_AT(PfLine, l->line, li);
            Str ln_str = PF_S("?\?");
            PfFunction *fn = ln->function;
            if (fn != NULL) {
                ln_str = fmt_sprintf_v(a, "%s %s:%d s=%d", fn->name, fn->filename,
                                       ln->line, fn->start_line);
                if (!str_eq(fn->name, fn->system_name))
                    ln_str = fmt_sprintf_v(a, "%s(%s)", ln_str, fn->system_name);
            }
            PF_LINE(fmt_sprintf_v(a, "%s%s", loc_str, ln_str));
            /* Do not print location details past the first line */
            loc_str = PF_S("             ");
        }
    }

    PF_LINE(PF_S("Mappings"));
    for (Int i = 0; i < p->mapping.len; i++) {
        PfMapping *m = pf_ptr_at(p->mapping, i);
        StringsBuilder bits = STRINGS_BUILDER(a);
        if (m->has_functions)
            strings_builder_write_string(&bits, PF_S("[FN]"), &err);
        if (m->has_filenames)
            strings_builder_write_string(&bits, PF_S("[FL]"), &err);
        if (m->has_line_numbers)
            strings_builder_write_string(&bits, PF_S("[LN]"), &err);
        if (m->has_inline_frames)
            strings_builder_write_string(&bits, PF_S("[IN]"), &err);
        PF_LINE(fmt_sprintf_v(a, "%d: %#x/%#x/%#x %s %s %s", m->id, m->start, m->limit,
                              m->offset, m->file, m->build_id,
                              strings_builder_string(&bits)));
    }
#undef PF_LINE

    return strings_builder_string(&sb);
}

static PfProfile *pf_copy(Alloc *a, PfProfile *p);

Error burrow__pf_profile_merge(PfProfile *p, PfProfile *pb, double r) {
    Error err = burrow__pf_profile_compatible(p, pb);
    if (BURROW_FAILED(err))
        return err;

    pb = pf_copy(p->a, pb);

    /* Keep the largest of the two periods. */
    if (pb->period > p->period)
        p->period = pb->period;

    p->duration_nanos += pb->duration_nanos;

    p->mapping = slice_append_slice(p->a, p->mapping, pb->mapping);
    for (Int i = 0; i < p->mapping.len; i++)
        ((PfMapping *)pf_ptr_at(p->mapping, i))->id = (uint64_t)(i + 1);
    p->location = slice_append_slice(p->a, p->location, pb->location);
    for (Int i = 0; i < p->location.len; i++)
        ((PfLocation *)pf_ptr_at(p->location, i))->id = (uint64_t)(i + 1);
    p->function = slice_append_slice(p->a, p->function, pb->function);
    for (Int i = 0; i < p->function.len; i++)
        ((PfFunction *)pf_ptr_at(p->function, i))->id = (uint64_t)(i + 1);

    if (r != 1.0) {
        for (Int i = 0; i < pb->sample.len; i++) {
            PfSample *s = pf_ptr_at(pb->sample, i);
            for (Int j = 0; j < s->value.len; j++) {
                int64_t *v = &BURROW_AT(int64_t, s->value, j);
                *v = pf_f64_to_i64((double)*v * r);
            }
        }
    }
    p->sample = slice_append_slice(p->a, p->sample, pb->sample);
    return burrow__pf_profile_check_valid(p);
}

/* fmt's %v of a *ValueType: &{Type Unit typeX unitX}, or <nil>. */
static Str pf_value_type_v(Alloc *a, const PfValueType *v) {
    if (v == NULL)
        return PF_S("<nil>");
    return fmt_sprintf_v(a, "&{%s %s %d %d}", v->type, v->unit, v->type_x, v->unit_x);
}

/* fmt's %v of a []*ValueType: the pointers, as %p prints them. */
static Str pf_value_types_v(Alloc *a, Slice v) {
    StringsBuilder sb = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    strings_builder_write_byte(&sb, '[');
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            strings_builder_write_byte(&sb, ' ');
        strings_builder_write_string(
            &sb, fmt_sprintf_v(a, "%#x", (uint64_t)(uintptr_t)pf_ptr_at(v, i)), &err);
    }
    strings_builder_write_byte(&sb, ']');
    return strings_builder_string(&sb);
}

static bool pf_compatible_value_types(const PfValueType *v1, const PfValueType *v2) {
    if (v1 == NULL || v2 == NULL)
        return true; /* No grounds to disqualify. */
    return str_eq(v1->type, v2->type) && str_eq(v1->unit, v2->unit);
}

Error burrow__pf_profile_compatible(PfProfile *p, PfProfile *pb) {
    Alloc *a = p->a;
    if (!pf_compatible_value_types(p->period_type, pb->period_type))
        return fmt_errorf_v("incompatible period types %s and %s",
                            pf_value_type_v(a, p->period_type),
                            pf_value_type_v(a, pb->period_type));

    if (p->sample_type.len != pb->sample_type.len)
        return fmt_errorf_v("incompatible sample types %s and %s",
                            pf_value_types_v(a, p->sample_type),
                            pf_value_types_v(a, pb->sample_type));

    for (Int i = 0; i < p->sample_type.len; i++) {
        if (!pf_compatible_value_types(pf_ptr_at(p->sample_type, i),
                                       pf_ptr_at(pb->sample_type, i)))
            return fmt_errorf_v("incompatible sample types %s and %s",
                                pf_value_types_v(a, p->sample_type),
                                pf_value_types_v(a, pb->sample_type));
    }

    return BURROW_NO_ERROR;
}

bool burrow__pf_profile_has_functions(PfProfile *p) {
    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        if (l->mapping == NULL || !l->mapping->has_functions)
            return false;
    }
    return true;
}

bool burrow__pf_profile_has_file_lines(PfProfile *p) {
    for (Int i = 0; i < p->location.len; i++) {
        PfLocation *l = pf_ptr_at(p->location, i);
        if (l->mapping == NULL ||
            (!l->mapping->has_filenames || !l->mapping->has_line_numbers))
            return false;
    }
    return true;
}

static PfProfile *pf_copy(Alloc *a, PfProfile *p) {
    pf_pre_encode(p);
    Slice b = burrow__pf_marshal(a, &pf_profile_msg, p);

    PfProfile *pp = burrow__pf_new_profile(a);
    Error err = burrow__pf_unmarshal(a, b, &pf_profile_msg, pp);
    if (BURROW_FAILED(err))
        pf_panic_err(err);
    err = pf_post_decode(pp);
    if (BURROW_FAILED(err))
        pf_panic_err(err);

    return pp;
}

PfProfile *burrow__pf_profile_copy(PfProfile *p) {
    return pf_copy(p->a, p);
}

Error burrow__pf_profile_demangle(PfProfile *p, PfDemangler d) {
    /* Collect names to demangle. */
    Slice names = slice_nil(TYPE_STRING);
    for (Int i = 0; i < p->function.len; i++) {
        PfFunction *fn = pf_ptr_at(p->function, i);
        names = BURROW_APPEND(Str, p->a, names, fn->system_name);
    }

    /* Update profile with demangled names. */
    Error err = BURROW_NO_ERROR;
    Map *demangled = d.f(d.env, p->a, names, &err);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < p->function.len; i++) {
        PfFunction *fn = pf_ptr_at(p->function, i);
        Str dd;
        if (demangled != NULL && map_get2(demangled, &fn->system_name, &dd))
            fn->name = dd;
    }
    return BURROW_NO_ERROR;
}

bool burrow__pf_profile_empty(PfProfile *p) {
    return p->sample.len == 0;
}

void burrow__pf_profile_scale(PfProfile *p, double ratio) {
    if (ratio == 1)
        return;
    Slice ratios =
        slice_make(p->a, TYPE_FLOAT64, p->sample_type.len, p->sample_type.len);
    for (Int i = 0; i < p->sample_type.len; i++)
        BURROW_AT(double, ratios, i) = ratio;
    (void)burrow__pf_profile_scale_n(p, ratios);
}

Error burrow__pf_profile_scale_n(PfProfile *p, Slice ratios) {
    if (p->sample_type.len != ratios.len)
        return fmt_errorf_v("mismatched scale ratios, got %d, want %d",
                            (int64_t)ratios.len, (int64_t)p->sample_type.len);
    bool all_ones = true;
    for (Int i = 0; i < ratios.len; i++) {
        if (BURROW_AT(double, ratios, i) != 1) {
            all_ones = false;
            break;
        }
    }
    if (all_ones)
        return BURROW_NO_ERROR;
    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        for (Int j = 0; j < s->value.len; j++) {
            double r = BURROW_AT(double, ratios, j);
            if (r != 1) {
                int64_t *v = &BURROW_AT(int64_t, s->value, j);
                *v = pf_f64_to_i64((double)*v * r);
            }
        }
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ merge */

typedef struct PfMapInfo {
    PfMapping *m;
    int64_t offset;
} PfMapInfo;

typedef struct PfMerger {
    PfProfile *p;
    Alloc *a;

    /* Memoization tables within a profile. */
    Map *locations_by_id; /* uint64 -> PfLocation * */
    Map *functions_by_id; /* uint64 -> PfFunction * */
    Map *mappings_by_id;  /* uint64 -> PfMapInfo * */

    /* Memoization tables for profile entities. */
    Map *samples;   /* key -> PfSample * */
    Map *locations; /* key -> PfLocation * */
    Map *functions; /* key -> PfFunction * */
    Map *mappings;  /* key -> PfMapping * */
} PfMerger;

/* Go keys its tables with structs of strings and numbers. These are strings
 * built from the same fields, each number in hex followed by a ';' and each
 * string with its length in front, so two different keys never give the same
 * string. */
static void pf_key_u64(StringsBuilder *sb, uint64_t x) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_string(sb, strconv_format_uint(sb->a, x, 16), &err);
    strings_builder_write_byte(sb, ';');
}

static void pf_key_str(StringsBuilder *sb, Str s) {
    Error err = BURROW_NO_ERROR;
    pf_key_u64(sb, (uint64_t)s.len);
    strings_builder_write_string(sb, s, &err);
}

static bool pf_is_zero_sample(const PfSample *s) {
    for (Int i = 0; i < s->value.len; i++) {
        if (BURROW_AT(int64_t, s->value, i) != 0)
            return false;
    }
    return true;
}

static PfLocation *pf_map_location(PfMerger *pm, PfLocation *src);
static PfMapInfo pf_map_mapping(PfMerger *pm, PfMapping *src);
static PfFunction *pf_map_function(PfMerger *pm, PfFunction *src);

/* fmt's %x of a []string: each string in hex, between brackets with a space
 * between each. */
static void pf_write_strs_x(StringsBuilder *sb, Slice v) {
    static const char hex[] = "0123456789abcdef";
    strings_builder_write_byte(sb, '[');
    for (Int i = 0; i < v.len; i++) {
        if (i > 0)
            strings_builder_write_byte(sb, ' ');
        Str s = BURROW_AT(Str, v, i);
        for (Int j = 0; j < s.len; j++) {
            strings_builder_write_byte(sb, (Byte)hex[s.p[j] >> 4]);
            strings_builder_write_byte(sb, (Byte)hex[s.p[j] & 15]);
        }
    }
    strings_builder_write_byte(sb, ']');
}

/* key generates sampleKey to be used as a key for maps. */
static Str pf_sample_key(Alloc *a, const PfSample *sample) {
    Error err = BURROW_NO_ERROR;
    Slice ids = slice_make(a, TYPE_STRING, sample->location.len, sample->location.len);
    for (Int i = 0; i < sample->location.len; i++) {
        PfLocation *l = pf_ptr_at(sample->location, i);
        BURROW_AT(Str, ids, i) = strconv_format_uint(a, l->id, 16);
    }

    Slice labels = slice_make(a, TYPE_STRING, 0, sample->label.len);
    for (Int i = 0; i < sample->label.len; i++) {
        PfStrLabels *l = pf_ptr_at(sample->label, i);
        StringsBuilder sb = STRINGS_BUILDER(a);
        strings_builder_write_string(&sb, strconv_quote(a, l->key), &err);
        strings_builder_write_byte(&sb, '[');
        for (Int j = 0; j < l->values.len; j++) {
            if (j > 0)
                strings_builder_write_byte(&sb, ' ');
            strings_builder_write_string(
                &sb, strconv_quote(a, BURROW_AT(Str, l->values, j)), &err);
        }
        strings_builder_write_byte(&sb, ']');
        labels = BURROW_APPEND(Str, a, labels, strings_builder_string(&sb));
    }
    sort_strings(labels);

    Slice numlabels = slice_make(a, TYPE_STRING, 0, sample->num_label.len);
    for (Int i = 0; i < sample->num_label.len; i++) {
        PfNumLabels *l = pf_ptr_at(sample->num_label, i);
        PfStrLabels *u = burrow__pf_labels_get(sample->num_unit, l->key);
        StringsBuilder sb = STRINGS_BUILDER(a);
        strings_builder_write_string(&sb, strconv_quote(a, l->key), &err);
        pf_write_int64s(&sb, l->values, 16);
        pf_write_strs_x(&sb, u != NULL ? u->values : slice_nil(TYPE_STRING));
        numlabels = BURROW_APPEND(Str, a, numlabels, strings_builder_string(&sb));
    }
    sort_strings(numlabels);

    StringsBuilder key = STRINGS_BUILDER(a);
    pf_key_str(&key, strings_join(a, ids, PF_S("|")));
    pf_key_str(&key, strings_join(a, labels, PF_S("")));
    pf_key_str(&key, strings_join(a, numlabels, PF_S("")));
    return strings_builder_string(&key);
}

static Slice pf_copy_strs(Alloc *a, Slice v) {
    Slice vv = slice_make(a, TYPE_STRING, v.len, v.len);
    slice_copy(vv, v);
    return vv;
}

static PfSample *pf_map_sample(PfMerger *pm, PfSample *src) {
    Alloc *a = pm->a;
    PfSample *s = pf_new_sample(a);
    s->location =
        slice_make(a, TYPE_UNSAFE_POINTER, src->location.len, src->location.len);
    s->value = slice_make(a, TYPE_INT64, src->value.len, src->value.len);
    for (Int i = 0; i < src->location.len; i++)
        BURROW_AT(void *, s->location, i) =
            pf_map_location(pm, pf_ptr_at(src->location, i));
    for (Int i = 0; i < src->label.len; i++) {
        PfStrLabels *l = pf_ptr_at(src->label, i);
        pf_labels_entry(a, &s->label, l->key)->values = pf_copy_strs(a, l->values);
    }
    for (Int i = 0; i < src->num_label.len; i++) {
        PfNumLabels *l = pf_ptr_at(src->num_label, i);
        PfStrLabels *u = burrow__pf_labels_get(src->num_unit, l->key);
        Slice vv = slice_make(a, TYPE_INT64, l->values.len, l->values.len);
        Slice uu = pf_copy_strs(a, u != NULL ? u->values : slice_nil(TYPE_STRING));
        slice_copy(vv, l->values);
        pf_num_labels_entry(a, &s->num_label, l->key)->values = vv;
        pf_labels_entry(a, &s->num_unit, l->key)->values = uu;
    }
    /* Check memoization table. Must be done on the remapped location to
     * account for the remapped mapping. Add current values to the
     * existing sample. */
    Str k = pf_sample_key(a, s);
    PfSample *ss = pf_map_get_ptr(pm->samples, &k);
    if (ss != NULL) {
        for (Int i = 0; i < src->value.len; i++)
            BURROW_AT(int64_t, ss->value, i) += BURROW_AT(int64_t, src->value, i);
        return ss;
    }
    slice_copy(s->value, src->value);
    pf_map_set_ptr(pm->samples, &k, s);
    pm->p->sample = pf_append_ptr(a, pm->p->sample, s);
    return s;
}

/* key generates locationKey to be used as a key for maps. */
static Str pf_location_key(Alloc *a, const PfLocation *l) {
    uint64_t addr = l->address;
    uint64_t mapping_id = 0;
    if (l->mapping != NULL) {
        /* Normalizes address to handle address space randomization. */
        addr -= l->mapping->start;
        mapping_id = l->mapping->id;
    }
    Slice lines = slice_make(a, TYPE_STRING, l->line.len * 2, l->line.len * 2);
    for (Int i = 0; i < l->line.len; i++) {
        PfLine *line = &BURROW_AT(PfLine, l->line, i);
        if (line->function != NULL)
            BURROW_AT(Str, lines, i * 2) =
                strconv_format_uint(a, line->function->id, 16);
        BURROW_AT(Str, lines, i * 2 + 1) = strconv_format_int(a, line->line, 16);
    }
    StringsBuilder key = STRINGS_BUILDER(a);
    pf_key_u64(&key, addr);
    pf_key_u64(&key, mapping_id);
    pf_key_str(&key, strings_join(a, lines, PF_S("|")));
    pf_key_u64(&key, l->is_folded);
    return strings_builder_string(&key);
}

static PfLocation *pf_map_location(PfMerger *pm, PfLocation *src) {
    if (src == NULL)
        return NULL;

    PfLocation *found = pf_map_get_ptr(pm->locations_by_id, &src->id);
    if (found != NULL) {
        pf_map_set_ptr(pm->locations_by_id, &src->id, found);
        return found;
    }

    PfMapInfo mi = pf_map_mapping(pm, src->mapping);
    PfLocation *l = pf_new_location(pm->a);
    l->id = (uint64_t)(pm->p->location.len + 1);
    l->mapping = mi.m;
    l->address = (uint64_t)((int64_t)src->address + mi.offset);
    l->line = slice_make(pm->a, &pf_line_desc, src->line.len, src->line.len);
    l->is_folded = src->is_folded;
    for (Int i = 0; i < src->line.len; i++) {
        PfLine ln = BURROW_AT(PfLine, src->line, i);
        PfLine *dst = &BURROW_AT(PfLine, l->line, i);
        dst->function = pf_map_function(pm, ln.function);
        dst->line = ln.line;
    }
    /* Check memoization table. Must be done on the remapped location to
     * account for the remapped mapping ID. */
    Str k = pf_location_key(pm->a, l);
    PfLocation *ll = pf_map_get_ptr(pm->locations, &k);
    if (ll != NULL) {
        pf_map_set_ptr(pm->locations_by_id, &src->id, ll);
        return ll;
    }
    pf_map_set_ptr(pm->locations_by_id, &src->id, l);
    pf_map_set_ptr(pm->locations, &k, l);
    pm->p->location = pf_append_ptr(pm->a, pm->p->location, l);
    return l;
}

/* key generates encoded strings of Mapping to be used as a key for
 * maps. */
static Str pf_mapping_key(Alloc *a, const PfMapping *m) {
    /* Normalize addresses to handle address space randomization.
     * Round up to next 4K boundary to avoid minor discrepancies. */
    enum { mapsize_rounding = 0x1000 };

    uint64_t size = m->limit - m->start;
    size = size + mapsize_rounding - 1;
    size = size - (size % mapsize_rounding);

    Str build_id_or_file = PF_S("");
    if (m->build_id.len != 0) {
        build_id_or_file = m->build_id;
    } else if (m->file.len != 0) {
        build_id_or_file = m->file;
    } else {
        /* A mapping containing neither build ID nor file name is a fake mapping. A
         * key with empty buildIDOrFile is used for fake mappings so that they are
         * treated as the same mapping during merging. */
    }
    StringsBuilder key = STRINGS_BUILDER(a);
    pf_key_u64(&key, size);
    pf_key_u64(&key, m->offset);
    pf_key_str(&key, build_id_or_file);
    return strings_builder_string(&key);
}

static PfMapInfo pf_map_mapping(PfMerger *pm, PfMapping *src) {
    if (src == NULL)
        return (PfMapInfo){0};

    PfMapInfo *found = pf_map_get_ptr(pm->mappings_by_id, &src->id);
    if (found != NULL)
        return *found;

    /* Check memoization tables. */
    Str mk = pf_mapping_key(pm->a, src);
    PfMapping *m = pf_map_get_ptr(pm->mappings, &mk);
    if (m != NULL) {
        PfMapInfo *mi = PF_NEW(pm->a, PfMapInfo);
        *mi = (PfMapInfo){m, (int64_t)m->start - (int64_t)src->start};
        pf_map_set_ptr(pm->mappings_by_id, &src->id, mi);
        return *mi;
    }
    m = PF_NEW(pm->a, PfMapping);
    m->id = (uint64_t)(pm->p->mapping.len + 1);
    m->start = src->start;
    m->limit = src->limit;
    m->offset = src->offset;
    m->file = src->file;
    m->build_id = src->build_id;
    m->has_functions = src->has_functions;
    m->has_filenames = src->has_filenames;
    m->has_line_numbers = src->has_line_numbers;
    m->has_inline_frames = src->has_inline_frames;
    pm->p->mapping = pf_append_ptr(pm->a, pm->p->mapping, m);

    /* Update memoization tables. */
    pf_map_set_ptr(pm->mappings, &mk, m);
    PfMapInfo *mi = PF_NEW(pm->a, PfMapInfo);
    *mi = (PfMapInfo){m, 0};
    pf_map_set_ptr(pm->mappings_by_id, &src->id, mi);
    return *mi;
}

/* key generates a struct to be used as a key for maps. */
static Str pf_function_key(Alloc *a, const PfFunction *f) {
    StringsBuilder key = STRINGS_BUILDER(a);
    pf_key_u64(&key, (uint64_t)f->start_line);
    pf_key_str(&key, f->name);
    pf_key_str(&key, f->system_name);
    pf_key_str(&key, f->filename);
    return strings_builder_string(&key);
}

static PfFunction *pf_map_function(PfMerger *pm, PfFunction *src) {
    if (src == NULL)
        return NULL;
    PfFunction *f = pf_map_get_ptr(pm->functions_by_id, &src->id);
    if (f != NULL)
        return f;
    Str k = pf_function_key(pm->a, src);
    f = pf_map_get_ptr(pm->functions, &k);
    if (f != NULL) {
        pf_map_set_ptr(pm->functions_by_id, &src->id, f);
        return f;
    }
    f = PF_NEW(pm->a, PfFunction);
    f->id = (uint64_t)(pm->p->function.len + 1);
    f->name = src->name;
    f->system_name = src->system_name;
    f->filename = src->filename;
    f->start_line = src->start_line;
    pf_map_set_ptr(pm->functions, &k, f);
    pf_map_set_ptr(pm->functions_by_id, &src->id, f);
    pm->p->function = pf_append_ptr(pm->a, pm->p->function, f);
    return f;
}

/* equalValueType returns true if the two value types are semantically
 * equal. It ignores the internal fields used during encode/decode. */
static bool pf_equal_value_type(const PfValueType *st1, const PfValueType *st2) {
    return str_eq(st1->type, st2->type) && str_eq(st1->unit, st2->unit);
}

/* compatible determines if two profiles can be compared/merged.
 * returns nil if the profiles are compatible; otherwise an error with
 * details on the incompatibility. */
static Error pf_compatible(Alloc *a, PfProfile *p, PfProfile *pb) {
    if (!pf_equal_value_type(p->period_type, pb->period_type))
        return fmt_errorf_v("incompatible period types %s and %s",
                            pf_value_type_v(a, p->period_type),
                            pf_value_type_v(a, pb->period_type));

    if (p->sample_type.len != pb->sample_type.len)
        return fmt_errorf_v("incompatible sample types %s and %s",
                            pf_value_types_v(a, p->sample_type),
                            pf_value_types_v(a, pb->sample_type));

    for (Int i = 0; i < p->sample_type.len; i++) {
        if (!pf_equal_value_type(pf_ptr_at(p->sample_type, i),
                                 pf_ptr_at(pb->sample_type, i)))
            return fmt_errorf_v("incompatible sample types %s and %s",
                                pf_value_types_v(a, p->sample_type),
                                pf_value_types_v(a, pb->sample_type));
    }
    return BURROW_NO_ERROR;
}

/* combineHeaders checks that all profiles can be merged and returns
 * their combined profile. */
static PfProfile *pf_combine_headers(Alloc *a, Slice srcs, Error *err) {
    PfProfile *s0 = pf_ptr_at(srcs, 0);
    for (Int i = 1; i < srcs.len; i++) {
        *err = pf_compatible(a, s0, pf_ptr_at(srcs, i));
        if (BURROW_FAILED(*err))
            return NULL;
    }

    int64_t time_nanos = 0, duration_nanos = 0, period = 0;
    Slice comments = slice_nil(TYPE_STRING);
    Map *seen_comments = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    Str default_sample_type = PF_S("");
    for (Int i = 0; i < srcs.len; i++) {
        PfProfile *s = pf_ptr_at(srcs, i);
        if (time_nanos == 0 || s->time_nanos < time_nanos)
            time_nanos = s->time_nanos;
        duration_nanos += s->duration_nanos;
        if (period == 0 || period < s->period)
            period = s->period;
        for (Int j = 0; j < s->comments.len; j++) {
            Str c = BURROW_AT(Str, s->comments, j);
            bool seen = false;
            if (!map_get2(seen_comments, &c, &seen) || !seen) {
                comments = BURROW_APPEND(Str, a, comments, c);
                seen = true;
                if (!map_set(seen_comments, &c, &seen))
                    panic_str(PF_S("internal/profile: out of memory"));
            }
        }
        if (default_sample_type.len == 0)
            default_sample_type = s->default_sample_type;
    }
    map_free(seen_comments);

    PfProfile *p = burrow__pf_new_profile(a);
    p->sample_type =
        slice_make(a, TYPE_UNSAFE_POINTER, s0->sample_type.len, s0->sample_type.len);

    p->drop_frames = s0->drop_frames;
    p->keep_frames = s0->keep_frames;

    p->time_nanos = time_nanos;
    p->duration_nanos = duration_nanos;
    p->period_type = s0->period_type;
    p->period = period;

    p->comments = comments;
    p->default_sample_type = default_sample_type;
    slice_copy(p->sample_type, s0->sample_type);
    return p;
}

PfProfile *burrow__pf_merge(Alloc *a, Slice srcs, Error *err) {
    *err = BURROW_NO_ERROR;
    if (srcs.len == 0) {
        *err = fmt_errorf_v("no profiles to merge");
        return NULL;
    }
    PfProfile *p = pf_combine_headers(a, srcs, err);
    if (BURROW_FAILED(*err))
        return NULL;

    PfProfile *s0 = pf_ptr_at(srcs, 0);
    PfMerger pm = {
        .p = p,
        .a = a,
        .samples = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, s0->sample.len),
        .locations = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, s0->location.len),
        .functions = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, s0->function.len),
        .mappings = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, s0->mapping.len),
    };

    for (Int i = 0; i < srcs.len; i++) {
        PfProfile *src = pf_ptr_at(srcs, i);
        /* Clear the profile-specific hash tables */
        map_free(pm.locations_by_id);
        map_free(pm.functions_by_id);
        map_free(pm.mappings_by_id);
        pm.locations_by_id =
            map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, src->location.len);
        pm.functions_by_id =
            map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, src->function.len);
        pm.mappings_by_id =
            map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, src->mapping.len);

        if (map_len(pm.mappings) == 0 && src->mapping.len > 0) {
            /* The Mapping list has the property that the first mapping
             * represents the main binary. Take the first Mapping we see,
             * otherwise the operations below will add mappings in an
             * arbitrary order. */
            pf_map_mapping(&pm, pf_ptr_at(src->mapping, 0));
        }

        for (Int j = 0; j < src->sample.len; j++) {
            PfSample *s = pf_ptr_at(src->sample, j);
            if (!pf_is_zero_sample(s))
                pf_map_sample(&pm, s);
        }
    }
    map_free(pm.locations_by_id);
    map_free(pm.functions_by_id);
    map_free(pm.mappings_by_id);
    map_free(pm.samples);
    map_free(pm.locations);
    map_free(pm.functions);
    map_free(pm.mappings);

    for (Int i = 0; i < p->sample.len; i++) {
        if (pf_is_zero_sample(pf_ptr_at(p->sample, i))) {
            /* If there are any zero samples, re-merge the profile to GC
             * them. */
            Slice one = slice_make(a, TYPE_UNSAFE_POINTER, 1, 1);
            BURROW_AT(void *, one, 0) = p;
            return burrow__pf_merge(a, one, err);
        }
    }

    return p;
}

Error burrow__pf_profile_normalize(PfProfile *p, PfProfile *pb) {
    Error err = pf_compatible(p->a, p, pb);
    if (BURROW_FAILED(err))
        return err;

    Int n = p->sample_type.len;
    Slice base_vals = slice_make(p->a, TYPE_INT64, n, n);
    for (Int i = 0; i < pb->sample.len; i++) {
        PfSample *s = pf_ptr_at(pb->sample, i);
        for (Int j = 0; j < s->value.len; j++)
            BURROW_AT(int64_t, base_vals, j) += BURROW_AT(int64_t, s->value, j);
    }

    Slice src_vals = slice_make(p->a, TYPE_INT64, n, n);
    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        for (Int j = 0; j < s->value.len; j++)
            BURROW_AT(int64_t, src_vals, j) += BURROW_AT(int64_t, s->value, j);
    }

    Slice norm_scale = slice_make(p->a, TYPE_FLOAT64, base_vals.len, base_vals.len);
    for (Int i = 0; i < base_vals.len; i++) {
        if (BURROW_AT(int64_t, src_vals, i) == 0)
            BURROW_AT(double, norm_scale, i) = 0.0;
        else
            BURROW_AT(double, norm_scale, i) =
                (double)BURROW_AT(int64_t, base_vals, i) /
                (double)BURROW_AT(int64_t, src_vals, i);
    }
    (void)burrow__pf_profile_scale_n(p, norm_scale);
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- filter */

/* focusedSample checks a sample against focus and ignore regexps.
 * Returns whether the focus/ignore regexps match any tags. */
static void pf_focused_sample(const PfSample *s, PfTagMatch focus, PfTagMatch ignore,
                              bool *fm, bool *im) {
    *fm = BURROW_FUNC_IS_NIL(focus);
    *im = false;
    for (Int i = 0; i < s->label.len; i++) {
        PfStrLabels *l = pf_ptr_at(s->label, i);
        for (Int j = 0; j < l->values.len; j++) {
            Str val = BURROW_AT(Str, l->values, j);
            if (!BURROW_FUNC_IS_NIL(ignore) && ignore.f(ignore.env, l->key, val, 0))
                *im = true;
            if (!*fm && focus.f(focus.env, l->key, val, 0))
                *fm = true;
        }
    }
    for (Int i = 0; i < s->num_label.len; i++) {
        PfNumLabels *l = pf_ptr_at(s->num_label, i);
        for (Int j = 0; j < l->values.len; j++) {
            int64_t val = BURROW_AT(int64_t, l->values, j);
            if (!BURROW_FUNC_IS_NIL(ignore) &&
                ignore.f(ignore.env, l->key, PF_S(""), val))
                *im = true;
            if (!*fm && focus.f(focus.env, l->key, PF_S(""), val))
                *fm = true;
        }
    }
}

void burrow__pf_profile_filter_samples_by_tag(PfProfile *p, PfTagMatch focus,
                                              PfTagMatch ignore, bool *fm, bool *im) {
    bool any_fm = false, any_im = false;
    Slice samples = slice_make(p->a, TYPE_UNSAFE_POINTER, 0, p->sample.len);
    for (Int i = 0; i < p->sample.len; i++) {
        PfSample *s = pf_ptr_at(p->sample, i);
        bool focused, ignored;
        pf_focused_sample(s, focus, ignore, &focused, &ignored);
        any_fm = any_fm || focused;
        any_im = any_im || ignored;
        if (focused && !ignored)
            samples = pf_append_ptr(p->a, samples, s);
    }
    p->sample = samples;
    if (fm != NULL)
        *fm = any_fm;
    if (im != NULL)
        *im = any_im;
}

/* ------------------------------------------------------------------ prune */

void burrow__pf_profile_prune(PfProfile *p, const Regexp *drop_rx,
                              const Regexp *keep_rx) {
    Map *prune = map_make(p->a, TYPE_UINT64, TYPE_BOOL, 0);
    Map *prune_beneath = map_make(p->a, TYPE_UINT64, TYPE_BOOL, 0);
    bool yes = true;

    for (Int n = 0; n < p->location.len; n++) {
        PfLocation *loc = pf_ptr_at(p->location, n);
        Int i;
        for (i = loc->line.len - 1; i >= 0; i--) {
            PfFunction *fn = BURROW_AT(PfLine, loc->line, i).function;
            if (fn != NULL && fn->name.len != 0) {
                Str func_name = fn->name;
                /* Account for leading '.' on the PPC ELF v1 ABI. */
                if (func_name.p[0] == '.')
                    func_name = (Str){func_name.p + 1, func_name.len - 1};
                if (regexp_match_string(drop_rx, func_name)) {
                    if (keep_rx == NULL || !regexp_match_string(keep_rx, func_name))
                        break;
                }
            }
        }

        if (i >= 0) {
            /* Found matching entry to prune. */
            if (!map_set(prune_beneath, &loc->id, &yes))
                panic_str(PF_S("internal/profile: out of memory"));

            /* Remove the matching location. */
            if (i == loc->line.len - 1) {
                /* Matched the top entry: prune the whole location. */
                if (!map_set(prune, &loc->id, &yes))
                    panic_str(PF_S("internal/profile: out of memory"));
            } else {
                loc->line = slice_sub(loc->line, i + 1, loc->line.len);
            }
        }
    }

    /* Prune locs from each Sample */
    for (Int n = 0; n < p->sample.len; n++) {
        PfSample *sample = pf_ptr_at(p->sample, n);
        /* Scan from the root to the leaves to find the prune location.
         * Do not prune frames before the first user frame, to avoid
         * pruning everything. */
        bool found_user = false;
        for (Int i = sample->location.len - 1; i >= 0; i--) {
            uint64_t id = ((PfLocation *)pf_ptr_at(sample->location, i))->id;
            bool pruned = false, beneath = false;
            map_get2(prune, &id, &pruned);
            map_get2(prune_beneath, &id, &beneath);
            if (!pruned && !beneath) {
                found_user = true;
                continue;
            }
            if (!found_user)
                continue;
            if (pruned) {
                sample->location =
                    slice_sub(sample->location, i + 1, sample->location.len);
                break;
            }
            if (beneath) {
                sample->location = slice_sub(sample->location, i, sample->location.len);
                break;
            }
        }
    }
    map_free(prune);
    map_free(prune_beneath);
}

Error burrow__pf_profile_remove_uninteresting(PfProfile *p) {
    Regexp *keep = NULL, *drop = NULL;
    Error err = BURROW_NO_ERROR;

    if (p->drop_frames.len != 0) {
        drop =
            regexp_compile(p->a, fmt_sprintf_v(p->a, "^(%s)$", p->drop_frames), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("failed to compile regexp %s: %v", p->drop_frames, err);
        if (p->keep_frames.len != 0) {
            keep = regexp_compile(p->a, fmt_sprintf_v(p->a, "^(%s)$", p->keep_frames),
                                  &err);
            if (BURROW_FAILED(err))
                return fmt_errorf_v("failed to compile regexp %s: %v", p->keep_frames,
                                    err);
        }
        burrow__pf_profile_prune(p, drop, keep);
    }
    return BURROW_NO_ERROR;
}
