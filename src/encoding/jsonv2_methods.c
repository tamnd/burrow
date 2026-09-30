/* Derived from Go's src/encoding/json/v2/arshal_methods.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "jsonv2_internal.h"

#include <string.h>

/* Go builds an arshaler per type once, with each method it finds wrapped
 * around the one before. Here the methods are looked up in the descriptor on
 * the way in, and a method that steps aside hands over to the next one down
 * the list rather than to a closure. */

/* ------------------------------------------------------------- descriptors */

#define JV_ARG_TYPE(cname, gonm)                                                       \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {NULL, 0},                                                                     \
        KIND_UNSAFE_POINTER,                                                           \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

JV_ARG_TYPE(Jsonv2EncoderArg, "*jsontext.Encoder");
JV_ARG_TYPE(Jsonv2DecoderArg, "*jsontext.Decoder");

#define JV_IFACE_TYPE(cname, gonm)                                                     \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"encoding/json", 13},                                           \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

JV_IFACE_TYPE(Jsonv2Marshaler, "Marshaler");
JV_IFACE_TYPE(Jsonv2MarshalerTo, "MarshalerTo");
JV_IFACE_TYPE(Jsonv2Unmarshaler, "Unmarshaler");
JV_IFACE_TYPE(Jsonv2UnmarshalerFrom, "UnmarshalerFrom");

/* ------------------------------------------------------------ the methods */

typedef struct JvMethodName {
    Str name;
    int shape;
} JvMethodName;

enum {
    JV_SHAPE_TO,        /* (Encoder) Error */
    JV_SHAPE_MARSHAL,   /* (Alloc, *Error) Bytes */
    JV_SHAPE_APPEND,    /* (Alloc, Bytes, *Error) Bytes */
    JV_SHAPE_FROM,      /* (Alloc, Decoder) Error */
    JV_SHAPE_UNMARSHAL, /* (Alloc, Bytes) Error */
    JV_SHAPE_IS_ZERO    /* () bool */
};

/* In the order of JV_M_*, which is the order they are tried in. */
static const JvMethodName jv_method_names[JV_M_COUNT] = {
    {BURROW_S_INIT("MarshalJSONTo"), JV_SHAPE_TO},
    {BURROW_S_INIT("MarshalJSON"), JV_SHAPE_MARSHAL},
    {BURROW_S_INIT("AppendText"), JV_SHAPE_APPEND},
    {BURROW_S_INIT("MarshalText"), JV_SHAPE_MARSHAL},
    {BURROW_S_INIT("UnmarshalJSONFrom"), JV_SHAPE_FROM},
    {BURROW_S_INIT("UnmarshalJSON"), JV_SHAPE_UNMARSHAL},
    {BURROW_S_INIT("UnmarshalText"), JV_SHAPE_UNMARSHAL},
    {BURROW_S_INIT("IsZero"), JV_SHAPE_IS_ZERO},
};

/* Whether m has the signature its name calls for. Like Go, a method with the
 * right name and the wrong signature is not the interface's method. */
static bool jv_shape_matches(const Method *m, int shape) {
    const Type *f = m->ftype;
    if (f == NULL || m->thunk == NULL || type_num_out(f) != 1)
        return false;
    const Type *alloc = &burrow_type_EncodingAllocArg;
    const Type *perr = &burrow_type_EncodingErrorArg;
    switch (shape) {
    case JV_SHAPE_TO:
        return type_num_in(f) == 1 && type_in(f, 0) == &burrow_type_Jsonv2EncoderArg &&
               type_out(f, 0) == TYPE_ERROR;
    case JV_SHAPE_MARSHAL:
        return type_num_in(f) == 2 && type_in(f, 0) == alloc && type_in(f, 1) == perr &&
               type_out(f, 0) == TYPE_BYTES;
    case JV_SHAPE_APPEND:
        return type_num_in(f) == 3 && type_in(f, 0) == alloc &&
               type_in(f, 1) == TYPE_BYTES && type_in(f, 2) == perr &&
               type_out(f, 0) == TYPE_BYTES;
    case JV_SHAPE_FROM:
        return type_num_in(f) == 2 && type_in(f, 0) == alloc &&
               type_in(f, 1) == &burrow_type_Jsonv2DecoderArg &&
               type_out(f, 0) == TYPE_ERROR;
    case JV_SHAPE_UNMARSHAL:
        return type_num_in(f) == 2 && type_in(f, 0) == alloc &&
               type_in(f, 1) == TYPE_BYTES && type_out(f, 0) == TYPE_ERROR;
    case JV_SHAPE_IS_ZERO:
        return type_num_in(f) == 0 && type_out(f, 0) == TYPE_BOOL;
    default:
        return false;
    }
}

void burrow__jsonv2_methods(const Type *t, JvMethods *out) {
    memset(out, 0, sizeof(*out));
    if (t == NULL || t->methods == NULL)
        return;
    for (uint16_t i = 0; i < t->nmethod; i++) {
        const Method *m = &t->methods[i];
        for (int k = 0; k < JV_M_COUNT; k++) {
            if (out->m[k] == NULL && str_eq(m->name, jv_method_names[k].name)) {
                if (jv_shape_matches(m, jv_method_names[k].shape))
                    out->m[k] = m;
                break;
            }
        }
    }
}

/* implements, for any of the methods in mask. A method on T counts for an
 * unnamed *T too, which is what Go's method sets give, since every method
 * here takes a pointer receiver. */
bool burrow__jsonv2_implements(const Type *t, unsigned mask) {
    if (t == NULL)
        return false;
    for (int pass = 0; pass < 2; pass++) {
        if (t->nmethod > 0) {
            JvMethods ms;
            burrow__jsonv2_methods(t, &ms);
            for (int k = 0; k < JV_M_COUNT; k++)
                if (ms.m[k] != NULL && (mask & (1U << k)) != 0)
                    return true;
        }
        if (t->kind != KIND_POINTER || t->name.len != 0 || t->elem == NULL)
            break;
        t = t->elem;
    }
    return false;
}

/* ---------------------------------------------------------------- marshal */

/* What AppendRaw's callback needs to call a text method. */
typedef struct JvTextCall {
    const Method *m;
    void *recv;
    bool append;
} JvTextCall;

/* MarshalText or AppendText into b. The method gets an arena for anything it
 * allocates, and AppendText gets the free end of b, so one that fits writes
 * straight into place. */
static Error jv_text_into(JsonBuf *b, void *ctx) {
    const JvTextCall *c = (const JvTextCall *)ctx;
    Arena ar;
    arena_init(&ar, NULL, 0);
    EncodingAllocArg aa = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EncodingErrorArg ea = &err;
    Slice out = {NULL, 0, 0, TYPE_BYTE};
    if (c->append) {
        Slice in = {b->p + b->len, 0, b->cap - b->len, TYPE_BYTE};
        void *args[3] = {(void *)&aa, &in, (void *)&ea};
        void *rets[1] = {&out};
        method_call(c->m, c->recv, args, rets);
        if (BURROW_OK(err)) {
            if (out.p == in.p && out.len <= in.cap)
                b->len += out.len;
            else
                jsonbuf_put(b, (const Byte *)out.p, out.len);
        }
    } else {
        void *args[2] = {(void *)&aa, (void *)&ea};
        void *rets[1] = {&out};
        method_call(c->m, c->recv, args, rets);
        if (BURROW_OK(err))
            jsonbuf_put(b, (const Byte *)out.p, out.len);
    }
    arena_free(&ar);
    if (BURROW_OK(err) && b->failed)
        err = burrow_err_out_of_memory;
    return err;
}

/* The two text methods, which differ only in the call. */
static Error jv_marshal_text(JsontextEncoder *e, const Type *t, void *p,
                             const Method *m, bool append) {
    JvTextCall c = {m, p, append};
    Error err = burrow__jsontext_append_raw(e, '"', false, jv_text_into, &c);
    if (BURROW_OK(err))
        return err;
    err = burrow__jsonv2_wrap_unsupported(err, append ? "AppendText method"
                                                      : "MarshalText method");
    /* v1 wraps the error in a MarshalerError here, which comes with v1. */
    if (!burrow__jsonv2_is_semantic(err) && !burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_marshal_error_before(e, t, err);
    return err;
}

static Error jv_marshal_json(JsontextEncoder *e, const Type *t, void *p,
                             const Method *m, const JsontextOptions *mo) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    EncodingAllocArg aa = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EncodingErrorArg ea = &err;
    Slice val = {NULL, 0, 0, TYPE_BYTE};
    void *args[2] = {(void *)&aa, (void *)&ea};
    void *rets[1] = {&val};
    method_call(m, p, args, rets);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        err = burrow__jsonv2_wrap_unsupported(err, "MarshalJSON method");
        if (jsonflags_get(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return err;
        return burrow__jsonv2_collapse_semantic(
            burrow__jsonv2_marshal_error_before(e, t, err));
    }
    err = jsontext_encoder_write_value(e, val);
    arena_free(&ar);
    if (BURROW_FAILED(err) &&
        !jsonflags_get(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS) &&
        burrow__jsonv2_is_syntactic(err))
        err = burrow__jsonv2_marshal_error_before(e, t, err);
    return err;
}

/* MarshalJSONTo. Sets *skip when the method stepped aside without writing
 * anything, so the next one down gets its turn. */
static Error jv_marshal_json_to(JsontextEncoder *e, const Type *t, void *p,
                                const Method *m, bool *skip) {
    Int prev_depth = jt_depth(&e->st);
    int64_t prev_len = jt_e_len(e->st.last);
    jsonflags_set(&e->opts, JSONFLAG_WITHIN_ARSHAL_CALL | 1);
    Error err = BURROW_NO_ERROR;
    Jsonv2EncoderArg ep = e;
    void *args[1] = {(void *)&ep};
    void *rets[1] = {&err};
    method_call(m, p, args, rets);
    jsonflags_set(&e->opts, JSONFLAG_WITHIN_ARSHAL_CALL | 0);
    Int depth = jt_depth(&e->st);
    int64_t len = jt_e_len(e->st.last);
    if ((prev_depth != depth || prev_len + 1 != len) && BURROW_OK(err))
        err = burrow__jsonv2_err_non_singular_value;
    if (BURROW_OK(err))
        return err;
    if (errors_is(err, errors_err_unsupported)) {
        if (prev_depth == depth && prev_len == len) {
            *skip = true;
            return BURROW_NO_ERROR;
        }
        err = burrow__jsonv2_err_unsupported_mutation;
    }
    if (!burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_error_with_position_enc(e, t, prev_depth, prev_len, err);
    return err;
}

Error burrow__jsonv2_marshal_methods(JsontextEncoder *e, const Type *t, void *p,
                                     JsontextOptions *mo, const JvMethods *ms) {
    bool legacy = jsonflags_get(mo, JSONFLAG_CALL_METHODS_WITH_LEGACY_SEMANTICS);
    for (int k = JV_M_MARSHAL_JSON_TO; k <= JV_M_MARSHAL_TEXT; k++) {
        const Method *m = ms->m[k];
        if (m == NULL)
            continue;
        switch (k) {
        case JV_M_MARSHAL_JSON_TO: {
            if (legacy && jt_e_need_name(e->st.last))
                continue;
            bool skip = false;
            Error err = jv_marshal_json_to(e, t, p, m, &skip);
            if (skip)
                continue;
            return err;
        }
        case JV_M_MARSHAL_JSON:
            if (legacy && jt_e_need_name(e->st.last))
                continue;
            return jv_marshal_json(e, t, p, m, mo);
        case JV_M_APPEND_TEXT:
            return jv_marshal_text(e, t, p, m, true);
        default:
            return jv_marshal_text(e, t, p, m, false);
        }
    }
    return burrow__jsonv2_marshal_default(e, t, p, mo);
}

/* -------------------------------------------------------------- unmarshal */

static Error jv_unmarshal_text(JsontextDecoder *d, const Type *t, void *p,
                               const Method *m, const JsontextOptions *uo) {
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_value_kind(val);
    if (k == 'n') {
        if (!jsonflags_get(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            memset(p, 0, t->size);
        return BURROW_NO_ERROR;
    }
    if (k != '"')
        return burrow__jsonv2_unmarshal_error_after(
            d, t, burrow__jsonv2_err_non_string_value);
    Byte stack[256];
    JsonBuf scratch = {stack, 0, (Int)sizeof(stack), heap_allocator(), false, false};
    Str s = burrow__jsonwire_unquote_may_copy(
        (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
        &scratch);
    if (scratch.failed) {
        burrow__jsonbuf_free(&scratch);
        return burrow_err_out_of_memory;
    }
    Slice text = {(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
    EncodingAllocArg aa = d->out_alloc;
    void *args[2] = {(void *)&aa, &text};
    void *rets[1] = {&err};
    method_call(m, p, args, rets);
    burrow__jsonbuf_free(&scratch);
    if (BURROW_OK(err))
        return err;
    err = burrow__jsonv2_wrap_unsupported(err, "UnmarshalText method");
    if (jsonflags_get(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return err;
    if (!burrow__jsonv2_is_semantic(err) && !burrow__jsonv2_is_syntactic(err) &&
        !burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_unmarshal_error_after(d, t, err);
    return err;
}

static Error jv_unmarshal_json(JsontextDecoder *d, const Type *t, void *p,
                               const Method *m, const JsontextOptions *uo) {
    Error err = BURROW_NO_ERROR;
    Slice val = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err))
        return err;
    EncodingAllocArg aa = d->out_alloc;
    void *args[2] = {(void *)&aa, &val};
    void *rets[1] = {&err};
    method_call(m, p, args, rets);
    if (BURROW_OK(err))
        return err;
    err = burrow__jsonv2_wrap_unsupported(err, "UnmarshalJSON method");
    if (jsonflags_get(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return err;
    return burrow__jsonv2_collapse_semantic(
        burrow__jsonv2_unmarshal_error_after(d, t, err));
}

static Error jv_unmarshal_json_from(JsontextDecoder *d, const Type *t, void *p,
                                    const Method *m, const JsontextOptions *uo,
                                    bool *skip) {
    Int prev_depth = jt_depth(&d->st);
    int64_t prev_len = jt_e_len(d->st.last);
    /* Checked first so the method never sees an EOF of its own to report. */
    if (prev_depth == 1 && burrow__jsontext_at_eof(d))
        return io_eof;
    jsonflags_set(&d->opts, JSONFLAG_WITHIN_ARSHAL_CALL | 1);
    Error err = BURROW_NO_ERROR;
    EncodingAllocArg aa = d->out_alloc;
    Jsonv2DecoderArg dp = d;
    void *args[2] = {(void *)&aa, (void *)&dp};
    void *rets[1] = {&err};
    method_call(m, p, args, rets);
    jsonflags_set(&d->opts, JSONFLAG_WITHIN_ARSHAL_CALL | 0);
    Int depth = jt_depth(&d->st);
    int64_t len = jt_e_len(d->st.last);
    if ((prev_depth != depth || prev_len + 1 != len) && BURROW_OK(err))
        err = burrow__jsonv2_err_non_singular_value;
    if (BURROW_OK(err))
        return err;
    if (errors_is(err, errors_err_unsupported)) {
        if (prev_depth == depth && prev_len == len) {
            *skip = true;
            return BURROW_NO_ERROR;
        }
        err = burrow__jsonv2_err_unsupported_mutation;
    }
    if (jsonflags_get(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        Error err2 = burrow__jsontext_skip_until(d, prev_depth, prev_len + 1);
        return BURROW_FAILED(err2) ? err2 : err;
    }
    if (!burrow__jsonv2_is_syntactic(err) && !burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_error_with_position_dec(d, t, prev_depth, prev_len, err);
    return err;
}

Error burrow__jsonv2_unmarshal_methods(JsontextDecoder *d, const Type *t, void *p,
                                       JsontextOptions *uo, const JvMethods *ms) {
    bool legacy = jsonflags_get(uo, JSONFLAG_CALL_METHODS_WITH_LEGACY_SEMANTICS);
    for (int k = JV_M_UNMARSHAL_JSON_FROM; k <= JV_M_UNMARSHAL_TEXT; k++) {
        const Method *m = ms->m[k];
        if (m == NULL)
            continue;
        switch (k) {
        case JV_M_UNMARSHAL_JSON_FROM: {
            if (legacy && jt_e_need_name(d->st.last))
                continue;
            bool skip = false;
            Error err = jv_unmarshal_json_from(d, t, p, m, uo, &skip);
            if (skip)
                continue;
            return err;
        }
        case JV_M_UNMARSHAL_JSON:
            if (legacy && jt_e_need_name(d->st.last))
                continue;
            return jv_unmarshal_json(d, t, p, m, uo);
        default:
            return jv_unmarshal_text(d, t, p, m, uo);
        }
    }
    return burrow__jsonv2_unmarshal_default(d, t, p, uo);
}

/* ------------------------------------------------------------------ IsZero */

bool burrow__jsonv2_call_is_zero(const Method *m, void *p) {
    bool zero = false;
    void *rets[1] = {&zero};
    method_call(m, p, NULL, rets);
    return zero;
}
