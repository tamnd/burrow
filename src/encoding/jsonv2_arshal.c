/* encoding/json/v2: the default arshalers, which say how each kind of Go value
 * becomes JSON and back when the type has no methods of its own for it.
 *
 * Derived from Go's src/encoding/json/v2/arshal_default.go and arshal_any.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/encoding/base32.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/map.h"
#include "burrow/math.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"

#include "jsonv2_internal.h"

#include <string.h>

#define JV_START_DETECTING_CYCLES_AFTER 1000

/* ---------------------------------------------------------------- the types */

/* map[string]any and []any, which is what an empty interface becomes when it
 * is handed a JSON object or array. */
static const Type jv_type_map_string_any = {
    {NULL, 0},
    {NULL, 0},
    KIND_MAP,
    (uint32_t)sizeof(Map *),
    (uint16_t)_Alignof(Map *),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Any,
    &burrow_type_Str,
    0,
    0,
    NULL,
};

static const Type jv_type_slice_any = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Any,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_JSONV2_MAP_STRING_ANY = &jv_type_map_string_any;
const Type *const TYPE_JSONV2_SLICE_ANY = &jv_type_slice_any;

/* What errors_new gives back has no self_type, and Go would call it an
 * *errors.errorString, a pointer to a struct with nothing exported in it. */
static const Field jv_error_string_fields[] = {
    {{(const Byte *)"s", 1}, {NULL, 0}, &burrow_type_Str, 0},
};

static const Type jv_type_error_string = {
    {(const Byte *)"errorString", 11},
    {(const Byte *)"errors", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    1,
    0,
    jv_error_string_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type jv_type_ptr_error_string = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &jv_type_error_string,
    NULL,
    0,
    0,
    NULL,
};

static bool jv_is_map_string_any(const Type *t) {
    return t->kind == KIND_MAP && t->name.len == 0 && t->key == TYPE_STRING &&
           t->elem == TYPE_ANY;
}

static bool jv_is_slice_any(const Type *t) {
    return t->kind == KIND_SLICE && t->name.len == 0 && t->elem == TYPE_ANY;
}

/* ------------------------------------------------------------------ helpers */

#define JV_GET(o, f) jsonflags_get((o), (f))
#define JV_HAS(o, f) jsonflags_has((o), (f))

static bool jv_arshal_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* cmp.Or for errors. */
static Error jv_or(Error a, Error b) {
    return BURROW_FAILED(a) ? a : b;
}

static bool jv_fmt_is(const JsontextOptions *o, const char *s) {
    size_t n = strlen(s);
    return (size_t)o->format.len == n && memcmp(o->format.p, s, n) == 0;
}

static bool jv_str_is(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)s.len == n && (n == 0 || memcmp(s.p, lit, n) == 0);
}

static bool jv_enc_need_name(const JsontextEncoder *e) {
    return jt_e_need_name(e->st.last);
}

static bool jv_dec_need_name(const JsontextDecoder *d) {
    return jt_e_need_name(d->st.last);
}

static void *jv_alloc(Alloc *a, const Type *t) {
    size_t size = t->size > 0 ? t->size : 1;
    return mem_alloc(a, size, t->align > 0 ? t->align : 1);
}

static void jv_zero(const Type *t, void *p) {
    if (t->size > 0)
        memset(p, 0, t->size);
}

static int64_t jv_get_int(const Type *t, const void *p) {
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

static void jv_set_int(const Type *t, void *p, int64_t v) {
    switch (t->size) {
    case 1:
        *(int8_t *)p = (int8_t)v;
        break;
    case 2:
        *(int16_t *)p = (int16_t)v;
        break;
    case 4:
        *(int32_t *)p = (int32_t)v;
        break;
    default:
        *(int64_t *)p = v;
        break;
    }
}

static uint64_t jv_get_uint(const Type *t, const void *p) {
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

static void jv_set_uint(const Type *t, void *p, uint64_t v) {
    switch (t->size) {
    case 1:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case 2:
        *(uint16_t *)p = (uint16_t)v;
        break;
    case 4:
        *(uint32_t *)p = (uint32_t)v;
        break;
    default:
        *(uint64_t *)p = v;
        break;
    }
}

static double jv_get_float(const Type *t, const void *p) {
    if (t->kind == KIND_FLOAT32)
        return (double)*(const float *)p;
    return *(const double *)p;
}

static void jv_set_float(const Type *t, void *p, double v) {
    if (t->kind == KIND_FLOAT32)
        *(float *)p = (float)v;
    else
        *(double *)p = v;
}

/* A copy of s the unmarshalled value can keep. */
static Str jv_keep_str(JsontextDecoder *d, Str s, bool *oom) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(d->out_alloc, (size_t)s.len, 1);
    if (p == NULL) {
        *oom = true;
        return BURROW_STR_EMPTY;
    }
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

/* A scratch buffer for unquoting, on the stack until it outgrows it. */
#define JV_SCRATCH(name)                                                               \
    Byte name##_stack[256];                                                            \
    JsonBuf name = {name##_stack,     0,     (Int)sizeof(name##_stack),                \
                    heap_allocator(), false, false}

/* The errors a number that does not parse unwraps to. */
static Error jv_unwrap(Error err) {
    Error u = errors_unwrap(err);
    return BURROW_FAILED(u) ? u : err;
}

/* The pointers being visited, for finding cycles once things get deep. */
static Error jv_visit(JsontextEncoder *e, const Type *t, const void *p, Int len) {
    JsonSeen *seen = (JsonSeen *)e->seen;
    for (Int i = 0; i < e->seen_len; i++) {
        if (seen[i].t == t && seen[i].p == p && seen[i].len == len)
            return burrow__jsonv2_err_cycle;
    }
    if (e->seen_len == e->seen_cap) {
        Int nc = e->seen_cap == 0 ? 16 : e->seen_cap * 2;
        JsonSeen *nv = (JsonSeen *)mem_realloc(
            e->a, e->seen, (size_t)e->seen_cap * sizeof(JsonSeen),
            (size_t)nc * sizeof(JsonSeen), _Alignof(JsonSeen));
        if (nv == NULL)
            return burrow_err_out_of_memory;
        e->seen = nv;
        e->seen_cap = nc;
        seen = nv;
    }
    seen[e->seen_len].t = t;
    seen[e->seen_len].p = p;
    seen[e->seen_len].len = len;
    e->seen_len++;
    return BURROW_NO_ERROR;
}

static void jv_leave(JsontextEncoder *e) {
    e->seen_len--;
}

/* Whether the checks for the string and format tag options leave anything to
 * do, returning an error when they do not. This is the preamble most of the
 * marshal functions share. */
static bool jv_string_tag_invalid(const JsontextOptions *o) {
    return JV_GET(o, JSONFLAG_STRING_TAG) &&
           !JV_GET(o, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS);
}

/* The two JSON writes numbers need: digits, or digits in a string. */
typedef struct JvNum {
    int kind; /* 0 int, 1 uint, 2 float */
    int64_t i;
    uint64_t u;
    double f;
    int bits;
} JvNum;

static Error jv_append_num(JsonBuf *b, void *ctx) {
    const JvNum *n = (const JvNum *)ctx;
    Byte tmp[24];
    Int k = 0;
    uint64_t u;
    switch (n->kind) {
    case 0:
        if (n->i < 0)
            jsonbuf_byte(b, '-');
        u = n->i < 0 ? (uint64_t)0 - (uint64_t)n->i : (uint64_t)n->i;
        break;
    case 1:
        u = n->u;
        break;
    default:
        burrow__jsonwire_append_float(b, n->f, n->bits);
        return b->failed ? burrow_err_out_of_memory : BURROW_NO_ERROR;
    }
    do {
        tmp[k++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    while (k > 0)
        jsonbuf_byte(b, tmp[--k]);
    return b->failed ? burrow_err_out_of_memory : BURROW_NO_ERROR;
}

static Error jv_write_num(JsontextEncoder *e, bool stringify, JvNum *n) {
    return burrow__jsontext_append_raw(e, stringify ? '"' : '0', true, jv_append_num,
                                       n);
}

/* ------------------------------------------------------------------- is zero */

static bool jv_bytes_zero(const void *p, size_t n) {
    const Byte *b = (const Byte *)p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return false;
    return true;
}

/* reflect.Value.IsZero. */
static bool jv_is_zero(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_STRING:
        return ((const Str *)p)->len == 0;
    case KIND_SLICE:
        return ((const Slice *)p)->p == NULL;
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_CHAN:
    case KIND_FUNC:
        return *(void *const *)p == NULL;
    case KIND_INTERFACE:
        if (t == TYPE_ANY)
            return ((const Any *)p)->t == NULL;
        return *(void *const *)p == NULL;
    case KIND_ARRAY:
        for (uint32_t i = 0; i < t->len; i++)
            if (!jv_is_zero(t->elem, (const Byte *)p + (size_t)i * t->elem->size))
                return false;
        return true;
    case KIND_STRUCT:
        for (uint32_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (field_is_blank(f))
                continue;
            if (!jv_is_zero(f->type, (const Byte *)p + f->offset))
                return false;
        }
        return true;
    default:
        return jv_bytes_zero(p, t->size);
    }
}

/* isLegacyEmpty. */
static bool jv_is_legacy_empty(const Type *t, const void *p) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        return !*(const bool *)p;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return jv_get_int(t, p) == 0;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return jv_get_uint(t, p) == 0;
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return jv_get_float(t, p) == 0;
    case KIND_STRING:
        return ((const Str *)p)->len == 0;
    case KIND_MAP:
        return map_len(*(Map *const *)p) == 0;
    case KIND_SLICE:
        return ((const Slice *)p)->len == 0;
    case KIND_ARRAY:
        return t->len == 0;
    case KIND_POINTER:
    case KIND_INTERFACE:
        return jv_is_zero(t, p);
    default:
        return false;
    }
}

/* The fast omitempty check of a field whose kind has a length or a nil. */
static bool jv_is_empty(const Type *t, const void *p, bool *has) {
    *has = true;
    switch ((int)t->kind) {
    case KIND_STRING:
        return ((const Str *)p)->len == 0;
    case KIND_MAP:
        return map_len(*(Map *const *)p) == 0;
    case KIND_SLICE:
        return ((const Slice *)p)->len == 0;
    case KIND_ARRAY:
        return t->len == 0;
    case KIND_POINTER:
    case KIND_INTERFACE:
        return jv_is_zero(t, p);
    default:
        *has = false;
        return false;
    }
}

/* ---------------------------------------------------------------------- bool */

static Error jv_marshal_bool(JsontextEncoder *e, const Type *t, void *p,
                             JsontextOptions *mo) {
    bool stringify = false;
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        stringify = JV_GET(mo, JSONFLAG_STRING_TAG) &&
                    JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS);
        if (JV_GET(mo, JSONFLAG_STRING_TAG) &&
            !JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS |
                            JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    bool v = *(const bool *)p;
    if (stringify)
        return jsontext_encoder_write_token(
            e, jsontext_string(v ? JV_LIT("true") : JV_LIT("false")));
    return jsontext_encoder_write_token(e, jsontext_bool(v));
}

static Error jv_unmarshal_bool(JsontextDecoder *d, const Type *t, void *p,
                               JsontextOptions *uo) {
    bool stringify = false;
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        stringify = JV_GET(uo, JSONFLAG_STRING_TAG) &&
                    JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS);
        if (JV_GET(uo, JSONFLAG_STRING_TAG) &&
            !JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS |
                            JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    switch (jsontext_token_kind(tok)) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            *(bool *)p = false;
        return BURROW_NO_ERROR;
    case 't':
    case 'f':
        if (!stringify) {
            *(bool *)p = jsontext_token_bool(tok);
            return BURROW_NO_ERROR;
        }
        break;
    case '"':
        if (stringify) {
            Str s = jsontext_token_string(tok, heap_allocator());
            Error r = BURROW_NO_ERROR;
            if (jv_str_is(s, "true")) {
                *(bool *)p = true;
            } else if (jv_str_is(s, "false")) {
                *(bool *)p = false;
            } else if (JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS) &&
                       jv_str_is(s, "null")) {
                if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                    *(bool *)p = false;
            } else {
                r = burrow__jsonv2_unmarshal_error_after_value(d, t,
                                                               strconv_err_syntax);
            }
            if (s.len > 0)
                mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            return r;
        }
        break;
    default:
        break;
    }
    return burrow__jsonv2_unmarshal_error_after_skipping(d, t, BURROW_NO_ERROR);
}

/* -------------------------------------------------------------------- string */

static Error jv_marshal_string(JsontextEncoder *e, const Type *t, void *p,
                               JsontextOptions *mo) {
    bool stringify = false;
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        stringify = JV_GET(mo, JSONFLAG_STRING_TAG) &&
                    JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS);
        if (JV_GET(mo, JSONFLAG_STRING_TAG) &&
            !JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS |
                            JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    Str s = *(const Str *)p;
    if (stringify) {
        JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
        Error err = burrow__jsonwire_append_quote(&b, s.p, s.len, mo);
        if (BURROW_FAILED(err)) {
            burrow__jsonbuf_free(&b);
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsontext_syntactic_new(0, BURROW_STR_EMPTY, err));
        }
        JsonBuf q = {NULL, 0, 0, heap_allocator(), true, false};
        JsontextOptions none;
        memset(&none, 0, sizeof(none));
        (void)burrow__jsonwire_append_quote(&q, b.p, b.len, &none);
        burrow__jsonbuf_free(&b);
        if (q.failed) {
            burrow__jsonbuf_free(&q);
            return burrow_err_out_of_memory;
        }
        err = jsontext_encoder_write_value(e, jsonbuf_slice(&q));
        burrow__jsonbuf_free(&q);
        return err;
    }
    return jsontext_encoder_write_token(e, jsontext_string(s));
}

static Error jv_unmarshal_string(JsontextDecoder *d, const Type *t, void *p,
                                 JsontextOptions *uo) {
    bool stringify = false;
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        stringify = JV_GET(uo, JSONFLAG_STRING_TAG) &&
                    JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS);
        if (JV_GET(uo, JSONFLAG_STRING_TAG) &&
            !JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS |
                            JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    switch (jsontext_value_kind(val)) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            *(Str *)p = BURROW_STR_EMPTY;
        return BURROW_NO_ERROR;
    case '"': {
        JV_SCRATCH(scratch);
        Str s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        JsonBuf un = {NULL, 0, 0, heap_allocator(), true, false};
        Error r = BURROW_NO_ERROR;
        bool oom = scratch.failed;
        if (stringify && !oom) {
            if (jv_str_is(s, "null")) {
                if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                    *(Str *)p = BURROW_STR_EMPTY;
                goto done;
            }
            Error uerr = burrow__jsonwire_append_unquote(&un, s.p, s.len);
            if (BURROW_FAILED(uerr)) {
                r = burrow__jsonv2_unmarshal_error_after(d, t, uerr);
                goto done;
            }
            s = str_from_bytes(un.p, un.len);
        }
        if (!oom) {
            Str kept = jv_keep_str(d, s, &oom);
            if (!oom)
                *(Str *)p = kept;
        }
        if (oom)
            r = burrow_err_out_of_memory;
    done:
        burrow__jsonbuf_free(&un);
        burrow__jsonbuf_free(&scratch);
        return r;
    }
    default:
        break;
    }
    return burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
}

/* --------------------------------------------------------------------- bytes */

enum { JV_B64, JV_B64URL, JV_B32, JV_B32HEX, JV_B16 };

static Int jv_encoded_len(int enc, Int n) {
    switch (enc) {
    case JV_B64:
        return base64_encoding_encoded_len(base64_std_encoding, n);
    case JV_B64URL:
        return base64_encoding_encoded_len(base64_url_encoding, n);
    case JV_B32:
        return base32_encoding_encoded_len(base32_std_encoding, n);
    case JV_B32HEX:
        return base32_encoding_encoded_len(base32_hex_encoding, n);
    default:
        return hex_encoded_len(n);
    }
}

static Int jv_decoded_len(int enc, Int n) {
    switch (enc) {
    case JV_B64:
        return base64_encoding_decoded_len(base64_std_encoding, n);
    case JV_B64URL:
        return base64_encoding_decoded_len(base64_url_encoding, n);
    case JV_B32:
        return base32_encoding_decoded_len(base32_std_encoding, n);
    case JV_B32HEX:
        return base32_encoding_decoded_len(base32_hex_encoding, n);
    default:
        return hex_decoded_len(n);
    }
}

static Int jv_decode(int enc, Slice dst, Slice src, Error *err) {
    switch (enc) {
    case JV_B64:
        return base64_encoding_decode(base64_std_encoding, dst, src, err);
    case JV_B64URL:
        return base64_encoding_decode(base64_url_encoding, dst, src, err);
    case JV_B32:
        return base32_encoding_decode(base32_std_encoding, dst, src, err);
    case JV_B32HEX:
        return base32_encoding_decode(base32_hex_encoding, dst, src, err);
    default:
        return hex_decode(dst, src, err);
    }
}

typedef struct JvBytes {
    int enc;
    const Byte *p;
    Int n;
} JvBytes;

static Error jv_append_encoded(JsonBuf *b, void *ctx) {
    const JvBytes *x = (const JvBytes *)ctx;
    Int n = jv_encoded_len(x->enc, x->n);
    if (!jsonbuf_reserve(b, n))
        return burrow_err_out_of_memory;
    Slice dst = {b->p + b->len, n, n, TYPE_BYTE};
    Slice src = {(void *)(uintptr_t)x->p, x->n, x->n, TYPE_BYTE};
    switch (x->enc) {
    case JV_B64:
        base64_encoding_encode(base64_std_encoding, dst, src);
        break;
    case JV_B64URL:
        base64_encoding_encode(base64_url_encoding, dst, src);
        break;
    case JV_B32:
        base32_encoding_encode(base32_std_encoding, dst, src);
        break;
    case JV_B32HEX:
        base32_encoding_encode(base32_hex_encoding, dst, src);
        break;
    default:
        (void)hex_encode(dst, src);
        break;
    }
    b->len += n;
    return BURROW_NO_ERROR;
}

static int jv_bytes_format(const JsontextOptions *o) {
    if (jv_fmt_is(o, "base64"))
        return JV_B64;
    if (jv_fmt_is(o, "base64url"))
        return JV_B64URL;
    if (jv_fmt_is(o, "base32"))
        return JV_B32;
    if (jv_fmt_is(o, "base32hex"))
        return JV_B32HEX;
    if (jv_fmt_is(o, "base16") || jv_fmt_is(o, "hex"))
        return JV_B16;
    if (jv_fmt_is(o, "array"))
        return -1;
    return -2;
}

static Error jv_marshal_slice(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo);
static Error jv_unmarshal_slice(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo);
static Error jv_marshal_array(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo);
static Error jv_unmarshal_array(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo);

static Error jv_marshal_list(JsontextEncoder *e, const Type *t, void *p,
                             JsontextOptions *mo) {
    return t->kind == KIND_ARRAY ? jv_marshal_array(e, t, p, mo)
                                 : jv_marshal_slice(e, t, p, mo);
}

static Error jv_unmarshal_list(JsontextDecoder *d, const Type *t, void *p,
                               JsontextOptions *uo) {
    return t->kind == KIND_ARRAY ? jv_unmarshal_array(d, t, p, uo)
                                 : jv_unmarshal_slice(d, t, p, uo);
}

static Error jv_marshal_bytes(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo) {
    bool named_byte = t->elem->pkg_path.len > 0;
    if (!JV_GET(mo, JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS) && named_byte)
        return jv_marshal_list(e, t, p, mo);
    int enc = JV_B64;
    bool is_array = t->kind == KIND_ARRAY;
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS | JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY |
                       JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS |
                       JSONFLAG_FORMAT_NIL_SLICE_AS_NULL)) {
        if (JV_GET(mo, JSONFLAG_STRING_TAG) &&
            !JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
        }
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG)) {
            enc = jv_bytes_format(mo);
            if (enc == -1) {
                jsonflags_clear(mo, JSONFLAG_FORMAT_TAG);
                return jv_marshal_list(e, t, p, mo);
            }
            if (enc == -2)
                return burrow__jsonv2_invalid_format_enc(e, t, mo);
        } else if ((JV_GET(mo, JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY) && is_array) ||
                   (JV_GET(mo, JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS) &&
                    burrow__jsonv2_implements_any(t->elem))) {
            return jv_marshal_list(e, t, p, mo);
        }
        if (JV_GET(mo, JSONFLAG_FORMAT_NIL_SLICE_AS_NULL) && !is_array &&
            ((const Slice *)p)->p == NULL)
            return jsontext_encoder_write_token(e, jsontext_null);
    }
    JvBytes x;
    x.enc = enc;
    if (is_array) {
        x.p = (const Byte *)p;
        x.n = (Int)t->len;
    } else {
        x.p = (const Byte *)((const Slice *)p)->p;
        x.n = ((const Slice *)p)->len;
    }
    return burrow__jsontext_append_raw(e, '"', true, jv_append_encoded, &x);
}

static Error jv_unmarshal_bytes(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo) {
    bool named_byte = t->elem->pkg_path.len > 0;
    if (!JV_GET(uo, JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS) && named_byte)
        return jv_unmarshal_list(d, t, p, uo);
    int enc = JV_B64;
    bool is_array = t->kind == KIND_ARRAY;
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS | JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY |
                       JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS)) {
        if (JV_GET(uo, JSONFLAG_STRING_TAG) &&
            !JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        }
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG)) {
            enc = jv_bytes_format(uo);
            if (enc == -1) {
                jsonflags_clear(uo, JSONFLAG_FORMAT_TAG);
                return jv_unmarshal_list(d, t, p, uo);
            }
            if (enc == -2)
                return burrow__jsonv2_invalid_format_dec(d, t, uo);
        } else if ((JV_GET(uo, JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY) && is_array) ||
                   (JV_GET(uo, JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS) &&
                    jsontext_decoder_peek_kind(d) == '[')) {
            return jv_unmarshal_list(d, t, p, uo);
        }
    }
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    switch (jsontext_value_kind(val)) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS) || !is_array)
            jv_zero(t, p);
        return BURROW_NO_ERROR;
    case '"':
        break;
    default:
        return burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
    }
    JV_SCRATCH(scratch);
    Str s = burrow__jsonwire_unquote_may_copy(
        (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
        &scratch);
    if (scratch.failed) {
        burrow__jsonbuf_free(&scratch);
        return burrow_err_out_of_memory;
    }
    Int max = jv_decoded_len(enc, s.len);
    /* Go appends to va.Bytes()[:0], which for a slice with room to spare is
     * the slice's own array, and for an array is the array. */
    Byte *dst = NULL;
    Byte *tmp = NULL;
    Slice *sp = is_array ? NULL : (Slice *)p;
    if (is_array || sp->cap < max) {
        Alloc *a = is_array ? heap_allocator() : d->out_alloc;
        if (max > 0) {
            dst = (Byte *)mem_alloc_nozero(a, (size_t)max, 1);
            if (dst == NULL) {
                burrow__jsonbuf_free(&scratch);
                return burrow_err_out_of_memory;
            }
        }
        if (is_array)
            tmp = dst;
    } else {
        dst = (Byte *)sp->p;
    }
    Slice dsts = {dst, max, max, TYPE_BYTE};
    Slice srcs = {(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
    Error derr = BURROW_NO_ERROR;
    Int n = jv_decode(enc, dsts, srcs, &derr);
    Error r = BURROW_NO_ERROR;
    if (BURROW_FAILED(derr)) {
        r = burrow__jsonv2_unmarshal_error_after(d, t, derr);
        goto done;
    }
    if (s.len != jv_encoded_len(enc, n) &&
        !JV_GET(uo, JSONFLAG_PARSE_BYTES_WITH_LOOSE_RFC4648)) {
        Int i = 0;
        while (i < s.len && s.p[i] != '\r' && s.p[i] != '\n')
            i++;
        if (i == s.len)
            i = 0;
        Byte q[16];
        Str qr = burrow__jsonwire_quote_rune(s.p + i, s.len - i, q);
        r = burrow__jsonv2_unmarshal_error_after(
            d, t,
            burrow__jsonv2_errorf_in(error_allocator(),
                                     "illegal character %s at offset %d", qr,
                                     (int64_t)i));
        goto done;
    }
    if (is_array) {
        Int alen = (Int)t->len;
        Int c = n < alen ? n : alen;
        if (c > 0)
            memcpy(p, dst, (size_t)c);
        if (c < alen)
            memset((Byte *)p + c, 0, (size_t)(alen - c));
        if (n != alen && !JV_GET(uo, JSONFLAG_UNMARSHAL_ARRAY_FROM_ANY_LENGTH))
            r = burrow__jsonv2_unmarshal_error_after(
                d, t,
                burrow__jsonv2_errorf_in(
                    error_allocator(),
                    "decoded length of %d mismatches array length of %d", (int64_t)n,
                    (int64_t)alen));
    } else {
        if (dst == NULL) {
            Slice empty = slice_make(d->out_alloc, t->elem, 0, 0);
            *sp = empty;
        } else {
            Int cap = dst == (Byte *)sp->p ? sp->cap : max;
            sp->p = dst;
            sp->len = n;
            sp->cap = cap;
            sp->elem = t->elem;
        }
    }
done:
    if (tmp != NULL)
        mem_free(heap_allocator(), tmp, (size_t)max, 1);
    burrow__jsonbuf_free(&scratch);
    return r;
}

/* ------------------------------------------------------------------- numbers */

static Error jv_marshal_int(JsontextEncoder *e, const Type *t, void *p,
                            JsontextOptions *mo) {
    bool stringify = jv_enc_need_name(e) ||
                     JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
        return burrow__jsonv2_invalid_format_enc(e, t, mo);
    JvNum n = {0, jv_get_int(t, p), 0, 0, 0};
    return jv_write_num(e, stringify, &n);
}

static Error jv_marshal_uint(JsontextEncoder *e, const Type *t, void *p,
                             JsontextOptions *mo) {
    bool stringify = jv_enc_need_name(e) ||
                     JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
        return burrow__jsonv2_invalid_format_enc(e, t, mo);
    JvNum n = {1, 0, jv_get_uint(t, p), 0, 0};
    return jv_write_num(e, stringify, &n);
}

static Error jv_unmarshal_int(JsontextDecoder *d, const Type *t, void *p,
                              JsontextOptions *uo) {
    bool stringify = jv_dec_need_name(d) ||
                     JV_GET(uo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
        return burrow__jsonv2_invalid_format_dec(d, t, uo);
    int bits = (int)t->size * 8;
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_value_kind(val);
    JV_SCRATCH(scratch);
    Str s = str_from_bytes(val.p, val.len);
    Error r = BURROW_NO_ERROR;
    switch (k) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_set_int(t, p, 0);
        goto done;
    case '"':
        if (!stringify)
            break;
        s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        if (JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS)) {
            Error perr = BURROW_NO_ERROR;
            int64_t n = strconv_parse_int(s, 10, bits, &perr);
            if (BURROW_FAILED(perr)) {
                if (jv_str_is(s, "null")) {
                    if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                        jv_set_int(t, p, 0);
                    goto done;
                }
                r = burrow__jsonv2_unmarshal_error_after_value(d, t, jv_unwrap(perr));
                goto done;
            }
            jv_set_int(t, p, n);
            goto done;
        }
        goto number;
    case '0':
        if (stringify)
            break;
    number: {
        bool neg = s.len > 0 && s.p[0] == '-';
        Int off = neg ? 1 : 0;
        uint64_t n = 0;
        bool ok =
            s.p != NULL && burrow__jsonwire_parse_uint(s.p + off, s.len - off, &n);
        uint64_t max_int = bits > 0 ? (uint64_t)1 << (bits - 1) : 0;
        bool overflow = (neg && n > max_int) || (!neg && n > max_int - 1);
        if (!ok) {
            if (n != UINT64_MAX) {
                r = burrow__jsonv2_unmarshal_error_after_value(d, t,
                                                               strconv_err_syntax);
                goto done;
            }
            overflow = true;
        }
        if (overflow) {
            r = burrow__jsonv2_unmarshal_error_after_value(d, t, strconv_err_range);
            goto done;
        }
        jv_set_int(t, p, neg ? (int64_t)((uint64_t)0 - n) : (int64_t)n);
        goto done;
    }
    default:
        break;
    }
    r = burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
done:
    burrow__jsonbuf_free(&scratch);
    return r;
}

static Error jv_unmarshal_uint(JsontextDecoder *d, const Type *t, void *p,
                               JsontextOptions *uo) {
    bool stringify = jv_dec_need_name(d) ||
                     JV_GET(uo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
        return burrow__jsonv2_invalid_format_dec(d, t, uo);
    int bits = (int)t->size * 8;
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_value_kind(val);
    JV_SCRATCH(scratch);
    Str s = str_from_bytes(val.p, val.len);
    Error r = BURROW_NO_ERROR;
    switch (k) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_set_uint(t, p, 0);
        goto done;
    case '"':
        if (!stringify)
            break;
        s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        if (JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS)) {
            Error perr = BURROW_NO_ERROR;
            uint64_t n = strconv_parse_uint(s, 10, bits, &perr);
            if (BURROW_FAILED(perr)) {
                if (jv_str_is(s, "null")) {
                    if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                        jv_set_uint(t, p, 0);
                    goto done;
                }
                r = burrow__jsonv2_unmarshal_error_after_value(d, t, jv_unwrap(perr));
                goto done;
            }
            jv_set_uint(t, p, n);
            goto done;
        }
        goto number;
    case '0':
        if (stringify)
            break;
    number: {
        uint64_t n = 0;
        bool ok = burrow__jsonwire_parse_uint(s.p, s.len, &n);
        bool overflow = bits < 64 && n > ((uint64_t)1 << bits) - 1;
        if (!ok) {
            if (n != UINT64_MAX) {
                r = burrow__jsonv2_unmarshal_error_after_value(d, t,
                                                               strconv_err_syntax);
                goto done;
            }
            overflow = true;
        }
        if (overflow) {
            r = burrow__jsonv2_unmarshal_error_after_value(d, t, strconv_err_range);
            goto done;
        }
        jv_set_uint(t, p, n);
        goto done;
    }
    default:
        break;
    }
    r = burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
done:
    burrow__jsonbuf_free(&scratch);
    return r;
}

static Error jv_marshal_float(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo) {
    bool stringify = jv_enc_need_name(e) ||
                     JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    bool allow_non_finite = false;
    if (JV_HAS(mo, JSONFLAG_FORMAT_TAG)) {
        if (jv_fmt_is(mo, "nonfinite"))
            allow_non_finite = true;
        else
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    double fv = jv_get_float(t, p);
    if (burrow__json_isnan(fv) || burrow__json_isinf(fv)) {
        if (!allow_non_finite) {
            Str v = burrow__json_isnan(fv) ? JV_LIT("NaN")
                    : fv > 0               ? JV_LIT("+Inf")
                                           : JV_LIT("-Inf");
            return burrow__jsonv2_marshal_error_before(
                e, t,
                burrow__jsonv2_errorf_in(error_allocator(), "unsupported value: %s",
                                         v));
        }
        return jsontext_encoder_write_token(e, jsontext_float(fv));
    }
    JvNum n = {2, 0, 0, fv, (int)t->size * 8};
    return jv_write_num(e, stringify, &n);
}

static Error jv_unmarshal_float(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo) {
    bool stringify = jv_dec_need_name(d) ||
                     JV_GET(uo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    bool allow_non_finite = false;
    if (JV_HAS(uo, JSONFLAG_FORMAT_TAG)) {
        if (jv_fmt_is(uo, "nonfinite"))
            allow_non_finite = true;
        else
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    int bits = (int)t->size * 8;
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_value_kind(val);
    JV_SCRATCH(scratch);
    Str s = str_from_bytes(val.p, val.len);
    Error r = BURROW_NO_ERROR;
    switch (k) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_set_float(t, p, 0);
        goto done;
    case '"':
        s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        if (allow_non_finite) {
            if (jv_str_is(s, "NaN")) {
                jv_set_float(t, p, math_nan());
                goto done;
            }
            if (jv_str_is(s, "Infinity")) {
                jv_set_float(t, p, math_inf(1));
                goto done;
            }
            if (jv_str_is(s, "-Infinity")) {
                jv_set_float(t, p, math_inf(-1));
                goto done;
            }
        }
        if (!stringify)
            break;
        if (JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS)) {
            Error perr = BURROW_NO_ERROR;
            double n = strconv_parse_float(s, bits, &perr);
            if (BURROW_FAILED(perr)) {
                if (jv_str_is(s, "null")) {
                    if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                        jv_set_float(t, p, 0);
                    goto done;
                }
                r = burrow__jsonv2_unmarshal_error_after_value(d, t, jv_unwrap(perr));
                goto done;
            }
            jv_set_float(t, p, n);
            goto done;
        }
        {
            Error cerr = BURROW_NO_ERROR;
            Int n = burrow__jsonwire_consume_number(s.p, s.len, &cerr);
            if (n != s.len || BURROW_FAILED(cerr)) {
                r = burrow__jsonv2_unmarshal_error_after_value(d, t,
                                                               strconv_err_syntax);
                goto done;
            }
        }
        goto number;
    case '0':
        if (stringify)
            break;
    number: {
        Error perr = BURROW_NO_ERROR;
        double fv = strconv_parse_float(s, bits, &perr);
        jv_set_float(t, p, fv);
        if (BURROW_FAILED(perr))
            r = burrow__jsonv2_unmarshal_error_after_value(d, t, jv_unwrap(perr));
        goto done;
    }
    default:
        break;
    }
    r = burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
done:
    burrow__jsonbuf_free(&scratch);
    return r;
}

/* ----------------------------------------------------------------------- map */

/* mapKeyWithUniqueRepresentation. */
static bool jv_unique_key(const Type *k, bool allow_invalid_utf8) {
    switch ((int)k->kind) {
    case KIND_BOOL:
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return true;
    case KIND_STRING:
        return !allow_invalid_utf8;
    default:
        return false;
    }
}

static int jv_str_cmp(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    int c = n > 0 ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

typedef struct JvMember {
    Str name;
    void *val;
} JvMember;

static void jv_sort_members(JvMember *v, Int n, JvMember *tmp) {
    if (n < 2)
        return;
    Int mid = n / 2;
    jv_sort_members(v, mid, tmp);
    jv_sort_members(v + mid, n - mid, tmp);
    Int i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (jv_str_cmp(v[j].name, v[i].name) < 0)
            tmp[k++] = v[j++];
        else
            tmp[k++] = v[i++];
    }
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * sizeof(JvMember));
}

/* The error a map key that did not come out as a string gets. */
static Error jv_map_key_error(JsontextEncoder *e, const Type *kt, Error err) {
    if (err.vt != NULL && err.vt->self_type == TYPE_JSONTEXT_SYNTACTIC_ERROR) {
        const JsontextSyntacticError *se = (const JsontextSyntacticError *)err.data;
        if (jv_arshal_same(se->err, jsontext_err_non_string_name))
            return burrow__jsonv2_marshal_error_before(e, kt, err);
    }
    return err;
}

static Error jv_marshal_map_body(JsontextEncoder *e, const Type *t, Map *m,
                                 JsontextOptions *mo) {
    Int n = map_len(m);
    Error err = jsontext_encoder_write_token(e, jsontext_begin_object);
    if (BURROW_FAILED(err))
        return err;
    if (n > 0) {
        const Type *kt = t->key, *vt = t->elem;
        if (jv_unique_key(kt, JV_GET(mo, JSONFLAG_ALLOW_INVALID_UTF8)))
            jsonstate_disable_namespace(&e->st);
        if (!JV_GET(mo, JSONFLAG_DETERMINISTIC) || n <= 1) {
            MapIter it = map_iter(m);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v)) {
                err = burrow__jsonv2_marshal_value(e, kt, (void *)(uintptr_t)k, mo);
                if (BURROW_FAILED(err))
                    return jv_map_key_error(e, kt, err);
                err = burrow__jsonv2_marshal_value(e, vt, v, mo);
                if (BURROW_FAILED(err))
                    return err;
            }
        } else {
            bool strings = kt->kind == KIND_STRING;
            JvMember *members = (JvMember *)mem_alloc(
                heap_allocator(), (size_t)n * 2 * sizeof(JvMember), _Alignof(JvMember));
            if (members == NULL)
                return burrow_err_out_of_memory;
            Int i = 0;
            MapIter it = map_iter(m);
            const void *k;
            void *v;
            while (i < n && map_next(&it, &k, &v)) {
                if (strings) {
                    members[i].name = *(const Str *)k;
                } else {
                    err = burrow__jsonv2_marshal_value(e, kt, (void *)(uintptr_t)k, mo);
                    if (BURROW_FAILED(err)) {
                        err = jv_map_key_error(e, kt, err);
                        break;
                    }
                    members[i].name = burrow__jsontext_unwrite_only_object_member_name(
                        e, heap_allocator());
                }
                members[i].val = v;
                i++;
            }
            if (BURROW_OK(err)) {
                jv_sort_members(members, i, members + n);
                for (Int j = 0; j < i && BURROW_OK(err); j++) {
                    err = jsontext_encoder_write_token(
                        e, jsontext_string(members[j].name));
                    if (BURROW_OK(err))
                        err = burrow__jsonv2_marshal_value(e, vt, members[j].val, mo);
                }
            }
            if (!strings) {
                for (Int j = 0; j < i; j++)
                    if (members[j].name.len > 0)
                        mem_free(heap_allocator(), (void *)(uintptr_t)members[j].name.p,
                                 (size_t)members[j].name.len, 1);
            }
            mem_free(heap_allocator(), members, (size_t)n * 2 * sizeof(JvMember),
                     _Alignof(JvMember));
            if (BURROW_FAILED(err))
                return err;
        }
    }
    return jsontext_encoder_write_token(e, jsontext_end_object);
}

static Error jv_marshal_map(JsontextEncoder *e, const Type *t, void *p,
                            JsontextOptions *mo) {
    Map *m = *(Map **)p;
    bool deep = jt_depth(&e->st) > JV_START_DETECTING_CYCLES_AFTER;
    if (deep) {
        Error err = jv_visit(e, t, m, 0);
        if (BURROW_FAILED(err))
            return burrow__jsonv2_marshal_error_before(e, t, err);
    }
    Error err = BURROW_NO_ERROR;
    bool emit_null = JV_GET(mo, JSONFLAG_FORMAT_NIL_MAP_AS_NULL);
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(mo)) {
            err = burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
            goto out;
        }
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG)) {
            if (jv_fmt_is(mo, "emitnull")) {
                emit_null = true;
            } else if (jv_fmt_is(mo, "emitempty")) {
                emit_null = false;
            } else {
                err = burrow__jsonv2_invalid_format_enc(e, t, mo);
                goto out;
            }
        }
    }
    if (map_len(m) == 0 && emit_null && m == NULL) {
        err = jsontext_encoder_write_token(e, jsontext_null);
        goto out;
    }
    err = jv_marshal_map_body(e, t, m, mo);
out:
    if (deep)
        jv_leave(e);
    return err;
}

static Error jv_unmarshal_map(JsontextDecoder *d, const Type *t, void *p,
                              JsontextOptions *uo) {
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(uo))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG) && !jv_fmt_is(uo, "emitnull") &&
            !jv_fmt_is(uo, "emitempty"))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_token_kind(tok);
    if (k == 'n') {
        *(Map **)p = NULL;
        return BURROW_NO_ERROR;
    }
    if (k != '{')
        return burrow__jsonv2_unmarshal_error_after_skipping(d, t, BURROW_NO_ERROR);
    Map *m = *(Map **)p;
    if (m == NULL) {
        m = map_make(d->out_alloc, t->key, t->elem, 0);
        if (m == NULL)
            return burrow_err_out_of_memory;
        *(Map **)p = m;
    }
    const Type *kt = t->key, *vt = t->elem;
    if (jv_unique_key(kt, JV_GET(uo, JSONFLAG_ALLOW_INVALID_UTF8)))
        jsonstate_disable_namespace(&d->st);
    Map *seen = NULL;
    if (!JV_GET(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES) && map_len(m) > 0) {
        seen = map_make(heap_allocator(), kt, TYPE_BOOL, 0);
        if (seen == NULL)
            return burrow_err_out_of_memory;
    }
    void *kbuf = jv_alloc(heap_allocator(), kt);
    void *vbuf = jv_alloc(heap_allocator(), vt);
    Error r = BURROW_NO_ERROR;
    Error err_unmarshal = BURROW_NO_ERROR;
    if (kbuf == NULL || vbuf == NULL) {
        r = burrow_err_out_of_memory;
        goto done;
    }
    while (jsontext_decoder_peek_kind(d) != '}') {
        jv_zero(kt, kbuf);
        err = burrow__jsonv2_unmarshal_value(d, kt, kbuf, uo);
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo)) {
                r = err;
                goto done;
            }
            Error serr = jsontext_decoder_skip_value(d);
            if (BURROW_FAILED(serr)) {
                r = serr;
                goto done;
            }
            err_unmarshal = jv_or(err_unmarshal, err);
            continue;
        }
        if (kt == TYPE_ANY) {
            const Any *ka = (const Any *)kbuf;
            if (ka->t != NULL &&
                (ka->t->kind == KIND_MAP || ka->t->kind == KIND_SLICE ||
                 ka->t->kind == KIND_FUNC)) {
                JsonBuf tb = {NULL, 0, 0, heap_allocator(), true, false};
                burrow__jsonv2_put_type(&tb, ka->t);
                Error kerr = burrow__jsonv2_unmarshal_error_after(
                    d, t,
                    burrow__jsonv2_errorf_in(error_allocator(),
                                             "invalid incomparable key type %s",
                                             str_from_bytes(tb.p, tb.len)));
                burrow__jsonbuf_free(&tb);
                if (!JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
                    r = kerr;
                    goto done;
                }
                Error serr = jsontext_decoder_skip_value(d);
                if (BURROW_FAILED(serr)) {
                    r = serr;
                    goto done;
                }
                err_unmarshal = jv_or(err_unmarshal, kerr);
                continue;
            }
        }
        void *v2 = map_get(m, kbuf);
        if (v2 != NULL) {
            if (!JV_GET(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES) &&
                (seen == NULL || map_get(seen, kbuf) != NULL)) {
                Slice name = burrow__jsontext_previous_token_or_value(d);
                r = burrow__jsonv2_duplicate_name_error(
                    d, jsontext_decoder_input_offset(d) - (int64_t)name.len);
                goto done;
            }
            if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
                memcpy(vbuf, v2, vt->size);
            else
                jv_zero(vt, vbuf);
        } else {
            jv_zero(vt, vbuf);
        }
        err = burrow__jsonv2_unmarshal_value(d, vt, vbuf, uo);
        if (!map_set(m, kbuf, vbuf)) {
            r = burrow_err_out_of_memory;
            goto done;
        }
        if (seen != NULL) {
            bool yes = true;
            if (!map_set(seen, kbuf, &yes)) {
                r = burrow_err_out_of_memory;
                goto done;
            }
        }
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo)) {
                r = err;
                goto done;
            }
            err_unmarshal = jv_or(err_unmarshal, err);
        }
    }
    (void)jsontext_decoder_read_token(d, &err);
    r = BURROW_FAILED(err) ? err : err_unmarshal;
done:
    if (kbuf != NULL)
        mem_free(heap_allocator(), kbuf, kt->size > 0 ? kt->size : 1,
                 kt->align > 0 ? kt->align : 1);
    if (vbuf != NULL)
        mem_free(heap_allocator(), vbuf, vt->size > 0 ? vt->size : 1,
                 vt->align > 0 ? vt->align : 1);
    if (seen != NULL)
        map_free(seen);
    return r;
}

/* -------------------------------------------------------------------- struct */

/* A set of field ids, Go's uintSet. */
typedef struct JvIdSet {
    uint64_t small[4];
    uint64_t *w;
    Int nw;
} JvIdSet;

static bool jv_idset_init(JvIdSet *s, Int n) {
    memset(s->small, 0, sizeof(s->small));
    s->nw = (n + 63) / 64;
    if (s->nw <= 4) {
        s->w = s->small;
        return true;
    }
    s->w = (uint64_t *)mem_alloc(heap_allocator(), (size_t)s->nw * sizeof(uint64_t),
                                 _Alignof(uint64_t));
    return s->w != NULL;
}

static void jv_idset_free(JvIdSet *s) {
    if (s->w != NULL && s->w != s->small)
        mem_free(heap_allocator(), s->w, (size_t)s->nw * sizeof(uint64_t),
                 _Alignof(uint64_t));
}

/* Adds i and says whether it was not there already. */
static bool jv_idset_insert(JvIdSet *s, Int i) {
    uint64_t bit = (uint64_t)1 << (i % 64);
    uint64_t *w = &s->w[i / 64];
    bool had = (*w & bit) != 0;
    *w |= bit;
    return !had;
}

/* fieldByIndex: the field f names inside the struct at p, following the
 * embedded pointers on the way. With alloc a nil pointer is filled in when
 * every field on the way is exported, which is when Go can set it, and
 * otherwise the answer is NULL, as it is for a nil pointer without alloc. */
static void *jv_field_ptr(const Type *t, void *p, const JvField *f, Alloc *alloc) {
    const Type *st = t;
    bool settable = true;
    for (Int i = 0; i < f->nindex; i++) {
        if (i > 0) {
            if (st->kind == KIND_POINTER) {
                void **pp = (void **)p;
                if (*pp == NULL) {
                    if (alloc == NULL || !settable)
                        return NULL;
                    *pp = jv_alloc(alloc, st->elem);
                    if (*pp == NULL)
                        return NULL;
                }
                p = *pp;
                st = st->elem;
            }
        }
        const Field *sf = &st->fields[f->index[i]];
        settable = settable && field_is_exported(sf);
        p = (Byte *)p + sf->offset;
        st = sf->type;
    }
    return p;
}

static Error jv_marshal_struct_members(JsontextEncoder *e, const Type *t, void *p,
                                       JsontextOptions *mo, const JvFields *fs,
                                       JvIdSet *seen);
static Error jv_marshal_fallback(JsontextEncoder *e, const Type *t, void *p,
                                 JsontextOptions *mo, const JvFields *fs,
                                 JvIdSet *seen);

static Error jv_marshal_struct(JsontextEncoder *e, const Type *t, void *p,
                               JsontextOptions *mo) {
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(mo))
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    const JvFields *fs = burrow__jsonv2_fields(t);
    if (fs == NULL)
        return burrow_err_out_of_memory;
    if (fs->has_err_init && !JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_marshal_error_before(e, fs->err_init_type, fs->err_init);
    if (fs->has_err_fmt && !JV_GET(mo, JSONFLAG_FORMAT_TAG_SUPPORTED))
        return burrow__jsonv2_marshal_error_before(e, fs->err_fmt_type, fs->err_fmt);
    Error err = jsontext_encoder_write_token(e, jsontext_begin_object);
    if (BURROW_FAILED(err))
        return err;
    /* The ids of the fields written, which only matter for spotting a name
     * from the embedded fallback that a field already wrote. */
    JvIdSet seen;
    bool track = fs->has_fallback && !JV_GET(mo, JSONFLAG_ALLOW_DUPLICATE_NAMES);
    if (track && !jv_idset_init(&seen, fs->nflat))
        return burrow_err_out_of_memory;
    err = jv_marshal_struct_members(e, t, p, mo, fs, track ? &seen : NULL);
    if (BURROW_OK(err) && fs->has_fallback)
        err = jv_marshal_fallback(e, t, p, mo, fs, track ? &seen : NULL);
    if (track)
        jv_idset_free(&seen);
    if (BURROW_FAILED(err))
        return err;
    return jsontext_encoder_write_token(e, jsontext_end_object);
}

static Error jv_marshal_struct_members(JsontextEncoder *e, const Type *t, void *p,
                                       JsontextOptions *mo, const JvFields *fs,
                                       JvIdSet *seen) {
    Error err = BURROW_NO_ERROR;
    Int prev_idx = -1;
    jsonstate_disable_namespace(&e->st);
    for (Int i = 0; i < fs->nflat; i++) {
        const JvField *f = &fs->flat[i];
        void *v = jv_field_ptr(t, p, f, NULL);
        if (v == NULL)
            continue;
        if ((f->omitzero || JV_GET(mo, JSONFLAG_OMIT_ZERO_STRUCT_FIELDS)) &&
            jv_is_zero(f->typ, v))
            continue;
        if (f->omitempty && JV_GET(mo, JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS) &&
            jv_is_legacy_empty(f->typ, v))
            continue;
        if (f->omitempty && !JV_GET(mo, JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS)) {
            bool has = false;
            if (jv_is_empty(f->typ, v, &has) && has)
                continue;
        }
        err = jsontext_encoder_write_token(e, jsontext_string(f->name));
        if (BURROW_FAILED(err))
            return err;
        JsontextOptions saved = *mo;
        if (f->string_)
            jsonflags_set(mo, JSONFLAG_STRING_TAG | 1);
        if (f->format.len > 0) {
            jsonflags_set(mo, JSONFLAG_FORMAT_TAG | 1);
            mo->format = f->format;
        }
        err = burrow__jsonv2_marshal_value(e, f->typ, v, mo);
        mo->presence = saved.presence;
        mo->values = saved.values;
        mo->format = BURROW_STR_EMPTY;
        if (BURROW_FAILED(err))
            return err;
        if (f->omitempty && !JV_GET(mo, JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS)) {
            const Str *prev_name = prev_idx >= 0 ? &fs->flat[prev_idx].name : NULL;
            if (burrow__jsontext_unwrite_empty_object_member(e, prev_name))
                continue;
        }
        if (seen != NULL)
            (void)jv_idset_insert(seen, f->id);
        prev_idx = f->id;
    }
    return BURROW_NO_ERROR;
}

/* insertUnquotedName: whether name is new to the object being written, when
 * the embedded fallback is about to write it. Negative is out of memory. */
static int jv_fallback_name_ok(JsontextEncoder *e, const JvFields *fs, JvIdSet *seen,
                               Str name, const JsontextOptions *mo) {
    Int start = 0;
    Int nf = burrow__jsonv2_fields_by_folded(fs, name, &start);
    if (nf > 0) {
        const JvField *f = burrow__jsonv2_fields_by_name(fs, name);
        if (f != NULL)
            return jv_idset_insert(seen, f->id);
        for (Int j = 0; j < nf; j++) {
            const JvField *f2 = &fs->flat[fs->by_folded[start + j]];
            if (burrow__jsonv2_match_folded(f2, name, mo))
                return jv_idset_insert(seen, f2->id);
        }
    }
    return burrow__jsontext_encoder_insert_unquoted(e, name);
}

/* marshalEmbeddedFallbackAll, for a Go map of string key. jsontext.Value
 * comes with methods. */
static Error jv_marshal_fallback(JsontextEncoder *e, const Type *t, void *p,
                                 JsontextOptions *mo, const JvFields *fs,
                                 JvIdSet *seen) {
    const JvField *f = &fs->fallback;
    void *v = jv_field_ptr(t, p, f, NULL);
    if (v == NULL)
        return BURROW_NO_ERROR;
    const Type *mt = f->typ;
    if (mt->kind == KIND_POINTER) {
        v = *(void **)v;
        if (v == NULL)
            return BURROW_NO_ERROR;
        mt = mt->elem;
    }
    Map *m = *(Map **)v;
    Int n = m == NULL ? 0 : map_len(m);
    if (n == 0)
        return BURROW_NO_ERROR;
    const Type *kt = mt->key, *vt = mt->elem;
    Str *names =
        (Str *)mem_alloc(heap_allocator(), (size_t)n * sizeof(Str), _Alignof(Str));
    if (names == NULL)
        return burrow_err_out_of_memory;
    Int nn = 0;
    MapIter it = map_iter(m);
    const void *k;
    void *mv;
    while (nn < n && map_next(&it, &k, &mv))
        names[nn++] = *(const Str *)k;
    if (JV_GET(mo, JSONFLAG_DETERMINISTIC) && nn > 1) {
        JvMember *tmp = (JvMember *)mem_alloc(
            heap_allocator(), (size_t)nn * 2 * sizeof(JvMember), _Alignof(JvMember));
        if (tmp == NULL) {
            mem_free(heap_allocator(), names, (size_t)n * sizeof(Str), _Alignof(Str));
            return burrow_err_out_of_memory;
        }
        for (Int i = 0; i < nn; i++)
            tmp[i].name = names[i];
        jv_sort_members(tmp, nn, tmp + nn);
        for (Int i = 0; i < nn; i++)
            names[i] = tmp[i].name;
        mem_free(heap_allocator(), tmp, (size_t)nn * 2 * sizeof(JvMember),
                 _Alignof(JvMember));
    }
    Error err = BURROW_NO_ERROR;
    JsonBuf q = {NULL, 0, 0, heap_allocator(), true, false};
    for (Int i = 0; i < nn; i++) {
        Str name = names[i];
        q.len = 0;
        err = burrow__jsonwire_append_quote(&q, name.p, name.len, mo);
        if (BURROW_FAILED(err)) {
            err = burrow__jsonv2_marshal_error_before(e, kt, err);
            break;
        }
        if (q.failed) {
            err = burrow_err_out_of_memory;
            break;
        }
        if (seen != NULL) {
            /* The quoted name unquoted again, which is name unless the
             * options let invalid UTF-8 through and it got replaced. */
            JV_SCRATCH(scratch);
            Str uq = burrow__jsonwire_unquote_may_copy(
                q.p, q.len, memchr(q.p, '\\', (size_t)q.len) == NULL, &scratch);
            int ok = scratch.failed ? -1 : jv_fallback_name_ok(e, fs, seen, uq, mo);
            burrow__jsonbuf_free(&scratch);
            if (ok < 0) {
                err = burrow_err_out_of_memory;
                break;
            }
            if (ok == 0) {
                err = burrow__jsonv2_duplicate_name_error_enc(e, jsonbuf_slice(&q));
                break;
            }
        }
        err = jsontext_encoder_write_value(e, jsonbuf_slice(&q));
        if (BURROW_FAILED(err))
            break;
        err = burrow__jsonv2_marshal_value(e, vt, map_get(m, &names[i]), mo);
        if (BURROW_FAILED(err))
            break;
    }
    burrow__jsonbuf_free(&q);
    mem_free(heap_allocator(), names, (size_t)n * sizeof(Str), _Alignof(Str));
    return err;
}

static Error jv_skip(JsontextDecoder *d) {
    return jsontext_decoder_skip_value(d);
}

/* unmarshalEmbeddedFallbackNext, for a Go map of string key: the member
 * named name goes into the map, made if it is nil, on top of whatever the map
 * already held for it. */
static Error jv_unmarshal_fallback(JsontextDecoder *d, const Type *t, void *p,
                                   JsontextOptions *uo, const JvFields *fs, Str name) {
    const JvField *f = &fs->fallback;
    void *v = jv_field_ptr(t, p, f, d->out_alloc);
    if (v == NULL) {
        Error nerr =
            burrow__jsonv2_unmarshal_error_before(d, t, burrow__jsonv2_err_nil_field);
        Error serr = jv_skip(d);
        return BURROW_FAILED(serr) ? serr : nerr;
    }
    const Type *mt = f->typ;
    if (mt->kind == KIND_POINTER) {
        void **pp = (void **)v;
        if (*pp == NULL) {
            *pp = jv_alloc(d->out_alloc, mt->elem);
            if (*pp == NULL)
                return burrow_err_out_of_memory;
        }
        v = *pp;
        mt = mt->elem;
    }
    Map *m = *(Map **)v;
    if (m == NULL) {
        m = map_make(d->out_alloc, mt->key, mt->elem, 0);
        if (m == NULL)
            return burrow_err_out_of_memory;
        *(Map **)v = m;
    }
    bool oom = false;
    Str key = jv_keep_str(d, name, &oom);
    if (oom)
        return burrow_err_out_of_memory;
    const Type *vt = mt->elem;
    void *mv = jv_alloc(heap_allocator(), vt);
    if (mv == NULL)
        return burrow_err_out_of_memory;
    const void *old = map_get(m, &key);
    if (old != NULL)
        memcpy(mv, old, vt->size);
    Error err = burrow__jsonv2_unmarshal_value(d, vt, mv, uo);
    if (!map_set(m, &key, mv))
        err = burrow_err_out_of_memory;
    mem_free(heap_allocator(), mv, vt->size > 0 ? vt->size : 1,
             vt->align > 0 ? vt->align : 1);
    return err;
}

static Error jv_unmarshal_struct(JsontextDecoder *d, const Type *t, void *p,
                                 JsontextOptions *uo) {
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(uo))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_token_kind(tok);
    if (k == 'n') {
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_zero(t, p);
        return BURROW_NO_ERROR;
    }
    if (k != '{')
        return burrow__jsonv2_unmarshal_error_after_skipping(d, t, BURROW_NO_ERROR);
    const JvFields *fs = burrow__jsonv2_fields(t);
    if (fs == NULL)
        return burrow_err_out_of_memory;
    if (fs->has_err_init && !JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_unmarshal_error_after(d, fs->err_init_type, fs->err_init);
    if (fs->has_err_fmt && !JV_GET(uo, JSONFLAG_FORMAT_TAG_SUPPORTED))
        return burrow__jsonv2_unmarshal_error_after(d, fs->err_fmt_type, fs->err_fmt);
    JvIdSet seen;
    if (!jv_idset_init(&seen, fs->nflat))
        return burrow_err_out_of_memory;
    jsonstate_disable_namespace(&d->st);
    Error err_unmarshal = BURROW_NO_ERROR;
    Error r = BURROW_NO_ERROR;
    JV_SCRATCH(scratch);
    while (jsontext_decoder_peek_kind(d) != '}') {
        unsigned flags = 0;
        Slice val = burrow__jsontext_read_value(d, &flags, &err);
        if (BURROW_FAILED(err)) {
            r = err;
            goto done;
        }
        scratch.len = 0;
        Str name = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        if (scratch.failed) {
            r = burrow_err_out_of_memory;
            goto done;
        }
        const JvField *f = burrow__jsonv2_fields_by_name(fs, name);
        if (f == NULL) {
            Int start = 0;
            Int nf = burrow__jsonv2_fields_by_folded(fs, name, &start);
            Int num_match = 0;
            for (Int j = 0; j < nf; j++) {
                const JvField *f2 = &fs->flat[fs->by_folded[start + j]];
                if (burrow__jsonv2_match_folded(f2, name, uo)) {
                    if (f == NULL)
                        f = f2;
                    num_match++;
                }
            }
            if (num_match > 1 &&
                !JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
                r = burrow__jsonv2_unmarshal_error_after(
                    d, t, burrow__jsonv2_err_ambiguous_name);
                goto done;
            }
            if (f == NULL) {
                if (JV_GET(uo, JSONFLAG_REJECT_UNKNOWN_MEMBERS) && !fs->has_fallback) {
                    Error uerr = burrow__jsonv2_unmarshal_error_after(
                        d, t, jsonv2_err_unknown_name);
                    if (!JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
                        r = uerr;
                        goto done;
                    }
                    err_unmarshal = jv_or(err_unmarshal, uerr);
                }
                if (!JV_GET(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
                    int ins = burrow__jsontext_insert_unquoted(d, name);
                    if (ins < 0) {
                        r = burrow_err_out_of_memory;
                        goto done;
                    }
                    if (ins == 0) {
                        r = burrow__jsonv2_duplicate_name_error(
                            d, jsontext_decoder_input_offset(d) - (int64_t)val.len);
                        goto done;
                    }
                }
                if (!fs->has_fallback) {
                    Error serr = jv_skip(d);
                    if (BURROW_FAILED(serr)) {
                        r = serr;
                        goto done;
                    }
                } else {
                    Error ferr = jv_unmarshal_fallback(d, t, p, uo, fs, name);
                    if (BURROW_FAILED(ferr)) {
                        if (burrow__jsonv2_is_fatal(ferr, uo)) {
                            r = ferr;
                            goto done;
                        }
                        err_unmarshal = jv_or(err_unmarshal, ferr);
                    }
                }
                continue;
            }
        }
        if (!JV_GET(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES) &&
            !jv_idset_insert(&seen, f->id)) {
            r = burrow__jsonv2_duplicate_name_error(
                d, jsontext_decoder_input_offset(d) - (int64_t)val.len);
            goto done;
        }
        JsontextOptions saved = *uo;
        if (f->string_)
            jsonflags_set(uo, JSONFLAG_STRING_TAG | 1);
        if (f->format.len > 0) {
            jsonflags_set(uo, JSONFLAG_FORMAT_TAG | 1);
            uo->format = f->format;
        }
        void *v = jv_field_ptr(t, p, f, d->out_alloc);
        if (v == NULL) {
            Error nerr = burrow__jsonv2_unmarshal_error_before(
                d, t, burrow__jsonv2_err_nil_field);
            if (!JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
                uo->presence = saved.presence;
                uo->values = saved.values;
                uo->format = BURROW_STR_EMPTY;
                r = nerr;
                goto done;
            }
            err_unmarshal = jv_or(err_unmarshal, nerr);
            err = jv_skip(d);
        } else {
            err = burrow__jsonv2_unmarshal_value(d, f->typ, v, uo);
        }
        uo->presence = saved.presence;
        uo->values = saved.values;
        uo->format = BURROW_STR_EMPTY;
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo)) {
                r = err;
                goto done;
            }
            err_unmarshal = jv_or(err_unmarshal, err);
        }
    }
    (void)jsontext_decoder_read_token(d, &err);
    r = BURROW_FAILED(err) ? err : err_unmarshal;
done:
    burrow__jsonbuf_free(&scratch);
    jv_idset_free(&seen);
    return r;
}

/* ------------------------------------------------------------ slice and array */

static Error jv_marshal_slice(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo) {
    const Slice *s = (const Slice *)p;
    bool deep = jt_depth(&e->st) > JV_START_DETECTING_CYCLES_AFTER;
    if (deep) {
        Error err = jv_visit(e, t, s->p, s->len);
        if (BURROW_FAILED(err))
            return burrow__jsonv2_marshal_error_before(e, t, err);
    }
    Error err = BURROW_NO_ERROR;
    bool emit_null = JV_GET(mo, JSONFLAG_FORMAT_NIL_SLICE_AS_NULL);
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(mo)) {
            err = burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
            goto out;
        }
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG)) {
            if (jv_fmt_is(mo, "emitnull")) {
                emit_null = true;
            } else if (jv_fmt_is(mo, "emitempty")) {
                emit_null = false;
            } else {
                err = burrow__jsonv2_invalid_format_enc(e, t, mo);
                goto out;
            }
        }
    }
    if (s->len == 0 && emit_null && s->p == NULL) {
        err = jsontext_encoder_write_token(e, jsontext_null);
        goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_begin_array);
    if (BURROW_FAILED(err))
        goto out;
    for (Int i = 0; i < s->len; i++) {
        err = burrow__jsonv2_marshal_value(
            e, t->elem, (Byte *)s->p + (size_t)i * t->elem->size, mo);
        if (BURROW_FAILED(err))
            goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_end_array);
out:
    if (deep)
        jv_leave(e);
    return err;
}

static Error jv_unmarshal_slice(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo) {
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(uo))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG) && !jv_fmt_is(uo, "emitnull") &&
            !jv_fmt_is(uo, "emitempty"))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_token_kind(tok);
    Slice *s = (Slice *)p;
    if (k == 'n') {
        s->p = NULL;
        s->len = 0;
        s->cap = 0;
        return BURROW_NO_ERROR;
    }
    if (k != '[')
        return burrow__jsonv2_unmarshal_error_after_skipping(d, t, BURROW_NO_ERROR);
    const Type *et = t->elem;
    size_t esz = et->size;
    bool must_zero = true;
    Int cap = s->cap;
    s->elem = et;
    if (cap > 0)
        s->len = cap;
    Int i = 0;
    Error err_unmarshal = BURROW_NO_ERROR;
    while (jsontext_decoder_peek_kind(d) != ']') {
        if (i == cap) {
            Slice grown = slice_append(d->out_alloc, *s, NULL, 1);
            if (grown.cap <= cap)
                return burrow_err_out_of_memory;
            *s = grown;
            cap = s->cap;
            s->len = cap;
            must_zero = false;
        }
        void *v = (Byte *)s->p + (size_t)i * esz;
        i++;
        if (must_zero && !JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_zero(et, v);
        err = burrow__jsonv2_unmarshal_value(d, et, v, uo);
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo)) {
                s->len = i;
                return err;
            }
            err_unmarshal = jv_or(err_unmarshal, err);
        }
    }
    if (i == 0)
        *s = slice_make(d->out_alloc, et, 0, 0);
    else
        s->len = i;
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    return err_unmarshal;
}

static Error jv_marshal_array(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo) {
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(mo))
            return burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    Error err = jsontext_encoder_write_token(e, jsontext_begin_array);
    if (BURROW_FAILED(err))
        return err;
    for (uint32_t i = 0; i < t->len; i++) {
        err = burrow__jsonv2_marshal_value(e, t->elem,
                                           (Byte *)p + (size_t)i * t->elem->size, mo);
        if (BURROW_FAILED(err))
            return err;
    }
    return jsontext_encoder_write_token(e, jsontext_end_array);
}

static Error jv_unmarshal_array(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo) {
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        if (jv_string_tag_invalid(uo))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    JsontextKind k = jsontext_token_kind(tok);
    if (k == 'n') {
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_zero(t, p);
        return BURROW_NO_ERROR;
    }
    if (k != '[')
        return burrow__jsonv2_unmarshal_error_after_skipping(d, t, BURROW_NO_ERROR);
    const Type *et = t->elem;
    Int n = (Int)t->len;
    Int i = 0;
    Error err_unmarshal = BURROW_NO_ERROR;
    /* Go's err here is the one declared outside the loop, which the overflow
     * and underflow set and which is looked at after the closing bracket. */
    Error err_len = BURROW_NO_ERROR;
    while (jsontext_decoder_peek_kind(d) != ']') {
        if (i >= n) {
            Error serr = jsontext_decoder_skip_value(d);
            if (BURROW_FAILED(serr))
                return serr;
            err_len = burrow__jsonv2_err_array_overflow;
            continue;
        }
        void *v = (Byte *)p + (size_t)i * et->size;
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            jv_zero(et, v);
        err = burrow__jsonv2_unmarshal_value(d, et, v, uo);
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo))
                return err;
            err_unmarshal = jv_or(err_unmarshal, err);
        }
        i++;
    }
    for (; i < n; i++) {
        jv_zero(et, (Byte *)p + (size_t)i * et->size);
        err_len = burrow__jsonv2_err_array_underflow;
    }
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    if (BURROW_FAILED(err_len) && !JV_GET(uo, JSONFLAG_UNMARSHAL_ARRAY_FROM_ANY_LENGTH))
        return burrow__jsonv2_unmarshal_error_after(d, t, err_len);
    return err_unmarshal;
}

/* ------------------------------------------------------------------- pointer */

static Error jv_marshal_pointer(JsontextEncoder *e, const Type *t, void *p,
                                JsontextOptions *mo) {
    void *v = *(void **)p;
    bool deep = jt_depth(&e->st) > JV_START_DETECTING_CYCLES_AFTER;
    if (deep) {
        Error err = jv_visit(e, t, v, 0);
        if (BURROW_FAILED(err))
            return burrow__jsonv2_marshal_error_before(e, t, err);
    }
    Error err;
    if (JV_GET(mo, JSONFLAG_STRING_TAG) &&
        JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS) &&
        t->elem->kind == KIND_POINTER) {
        if (!JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
            err = burrow__jsonv2_marshal_error_before(
                e, t, burrow__jsonv2_err_invalid_string_tag);
            goto out;
        }
        jsonflags_clear(mo, JSONFLAG_STRING_TAG);
    }
    if (v == NULL)
        err = jsontext_encoder_write_token(e, jsontext_null);
    else
        err = burrow__jsonv2_marshal_value(e, t->elem, v, mo);
out:
    if (deep)
        jv_leave(e);
    return err;
}

static Error jv_unmarshal_pointer(JsontextDecoder *d, const Type *t, void *p,
                                  JsontextOptions *uo) {
    Error err = BURROW_NO_ERROR;
    if (jsontext_decoder_peek_kind(d) == 'n') {
        (void)jsontext_decoder_read_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        *(void **)p = NULL;
        return BURROW_NO_ERROR;
    }
    if (JV_GET(uo, JSONFLAG_STRING_TAG) &&
        JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS) &&
        t->elem->kind == KIND_POINTER) {
        if (!JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_invalid_string_tag);
        jsonflags_clear(uo, JSONFLAG_STRING_TAG);
    }
    void *v = *(void **)p;
    if (v == NULL) {
        v = jv_alloc(d->out_alloc, t->elem);
        if (v == NULL)
            return burrow_err_out_of_memory;
        *(void **)p = v;
    }
    err = burrow__jsonv2_unmarshal_value(d, t->elem, v, uo);
    if (BURROW_FAILED(err))
        return err;
    if (JV_GET(uo, JSONFLAG_STRING_TAG) &&
        JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS)) {
        Slice prev = burrow__jsontext_previous_token_or_value(d);
        if (prev.len == 6 && memcmp(prev.p, "\"null\"", 6) == 0)
            *(void **)p = NULL;
    }
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- interface */

/* The dynamic type and value of the interface at p, NULL for nil. What an
 * error made by errors_new holds is Go's *errors.errorString, and *vp is then
 * where its pointer lives. */
static const Type *jv_iface_elem(const Type *t, void *p, void **vp) {
    if (t == TYPE_ANY) {
        const Any *a = (const Any *)p;
        *vp = a->data;
        return a->t;
    }
    if (t == TYPE_ERROR) {
        Error *err = (Error *)p;
        if (err->vt == NULL)
            return NULL;
        if (err->vt->self_type == NULL) {
            *vp = (void *)&err->data;
            return &jv_type_ptr_error_string;
        }
        *vp = (void *)(uintptr_t)err->data;
        return err->vt->self_type;
    }
    Iface *v = (Iface *)p;
    if (v->vt == NULL)
        return NULL;
    if (t->size > sizeof(Iface))
        *vp = (Byte *)p + sizeof(void *);
    else
        *vp = v->data;
    return v->vt->self_type;
}

static Error jv_marshal_any_value(JsontextEncoder *e, const Type *t, void *v,
                                  JsontextOptions *mo);

static Error jv_marshal_object_any(JsontextEncoder *e, Map *obj, JsontextOptions *mo) {
    bool deep = jt_depth(&e->st) > JV_START_DETECTING_CYCLES_AFTER;
    if (deep) {
        Error err = jv_visit(e, TYPE_JSONV2_MAP_STRING_ANY, obj, 0);
        if (BURROW_FAILED(err))
            return burrow__jsonv2_marshal_error_before(e, TYPE_JSONV2_MAP_STRING_ANY,
                                                       err);
    }
    Error err = BURROW_NO_ERROR;
    Int n = map_len(obj);
    if (n == 0 && JV_GET(mo, JSONFLAG_FORMAT_NIL_MAP_AS_NULL) && obj == NULL) {
        err = jsontext_encoder_write_token(e, jsontext_null);
        goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_begin_object);
    if (BURROW_FAILED(err))
        goto out;
    if (!JV_GET(mo, JSONFLAG_ALLOW_INVALID_UTF8))
        jsonstate_disable_namespace(&e->st);
    if (!JV_GET(mo, JSONFLAG_DETERMINISTIC) || n <= 1) {
        MapIter it = map_iter(obj);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            err = jsontext_encoder_write_token(e, jsontext_string(*(const Str *)k));
            if (BURROW_FAILED(err))
                goto out;
            const Any *a = (const Any *)v;
            err = jv_marshal_any_value(e, a->t, a->data, mo);
            if (BURROW_FAILED(err))
                goto out;
        }
    } else {
        JvMember *members = (JvMember *)mem_alloc(
            heap_allocator(), (size_t)n * 2 * sizeof(JvMember), _Alignof(JvMember));
        if (members == NULL) {
            err = burrow_err_out_of_memory;
            goto out;
        }
        Int i = 0;
        MapIter it = map_iter(obj);
        const void *k;
        void *v;
        while (i < n && map_next(&it, &k, &v)) {
            members[i].name = *(const Str *)k;
            members[i].val = v;
            i++;
        }
        jv_sort_members(members, i, members + n);
        for (Int j = 0; j < i && BURROW_OK(err); j++) {
            err = jsontext_encoder_write_token(e, jsontext_string(members[j].name));
            if (BURROW_OK(err)) {
                const Any *a = (const Any *)members[j].val;
                err = jv_marshal_any_value(e, a->t, a->data, mo);
            }
        }
        mem_free(heap_allocator(), members, (size_t)n * 2 * sizeof(JvMember),
                 _Alignof(JvMember));
        if (BURROW_FAILED(err))
            goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_end_object);
out:
    if (deep)
        jv_leave(e);
    return err;
}

static Error jv_marshal_array_any(JsontextEncoder *e, const Slice *arr,
                                  JsontextOptions *mo) {
    bool deep = jt_depth(&e->st) > JV_START_DETECTING_CYCLES_AFTER;
    if (deep) {
        Error err = jv_visit(e, TYPE_JSONV2_SLICE_ANY, arr->p, arr->len);
        if (BURROW_FAILED(err))
            return burrow__jsonv2_marshal_error_before(e, TYPE_JSONV2_SLICE_ANY, err);
    }
    Error err = BURROW_NO_ERROR;
    if (arr->len == 0 && JV_GET(mo, JSONFLAG_FORMAT_NIL_SLICE_AS_NULL) &&
        arr->p == NULL) {
        err = jsontext_encoder_write_token(e, jsontext_null);
        goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_begin_array);
    if (BURROW_FAILED(err))
        goto out;
    for (Int i = 0; i < arr->len; i++) {
        const Any *a = (const Any *)arr->p + i;
        err = jv_marshal_any_value(e, a->t, a->data, mo);
        if (BURROW_FAILED(err))
            goto out;
    }
    err = jsontext_encoder_write_token(e, jsontext_end_array);
out:
    if (deep)
        jv_leave(e);
    return err;
}

/* marshalValueAny. */
static Error jv_marshal_any_value(JsontextEncoder *e, const Type *t, void *v,
                                  JsontextOptions *mo) {
    if (t == NULL)
        return jsontext_encoder_write_token(e, jsontext_null);
    if (t == TYPE_BOOL)
        return jsontext_encoder_write_token(e, jsontext_bool(*(const bool *)v));
    if (t == TYPE_STRING)
        return jsontext_encoder_write_token(e, jsontext_string(*(const Str *)v));
    if (t == TYPE_FLOAT64) {
        double f = *(const double *)v;
        if (!burrow__json_isnan(f) && !burrow__json_isinf(f))
            return jsontext_encoder_write_token(e, jsontext_float(f));
    } else if (jv_is_map_string_any(t)) {
        return jv_marshal_object_any(e, *(Map **)v, mo);
    } else if (jv_is_slice_any(t)) {
        return jv_marshal_array_any(e, (const Slice *)v, mo);
    }
    return burrow__jsonv2_marshal_value(e, t, v, mo);
}

static Error jv_marshal_interface(JsontextEncoder *e, const Type *t, void *p,
                                  JsontextOptions *mo) {
    if (JV_HAS(mo, JSONFLAG_TAG_FLAGS)) {
        if (JV_GET(mo, JSONFLAG_STRING_TAG)) {
            if (!JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
                return burrow__jsonv2_marshal_error_before(
                    e, t, burrow__jsonv2_err_invalid_string_tag);
            if (JV_GET(mo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS))
                jsonflags_clear(mo, JSONFLAG_STRING_TAG);
        }
        if (JV_HAS(mo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    }
    if (jv_is_zero(t, p))
        return jsontext_encoder_write_token(e, jsontext_null);
    void *v = NULL;
    const Type *et = jv_iface_elem(t, p, &v);
    if (et == NULL)
        return burrow__jsonv2_marshal_error_before(e, t, BURROW_NO_ERROR);
    if (t == TYPE_ANY && !JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_TAG_FLAGS))
        return jv_marshal_any_value(e, et, v, mo);
    return burrow__jsonv2_marshal_value(e, et, v, mo);
}

/* An Any holding a copy of what is at v, in the output allocator. */
static bool jv_box(JsontextDecoder *d, const Type *t, const void *v, Any *out) {
    void *p = jv_alloc(d->out_alloc, t);
    if (p == NULL)
        return false;
    memcpy(p, v, t->size);
    out->t = t;
    out->data = p;
    return true;
}

static Error jv_unmarshal_any_value(JsontextDecoder *d, JsontextOptions *uo, Any *out);

/* unmarshalObjectAny. */
static Error jv_unmarshal_object_any(JsontextDecoder *d, JsontextOptions *uo,
                                     Any *out) {
    Error err = BURROW_NO_ERROR;
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    Map *obj = map_make(d->out_alloc, TYPE_STRING, TYPE_ANY, 0);
    if (obj == NULL || !jv_box(d, TYPE_JSONV2_MAP_STRING_ANY, &obj, out))
        return burrow_err_out_of_memory;
    if (!JV_GET(uo, JSONFLAG_ALLOW_INVALID_UTF8))
        jsonstate_disable_namespace(&d->st);
    Error err_unmarshal = BURROW_NO_ERROR;
    while (jsontext_decoder_peek_kind(d) != '}') {
        JsontextToken tok = jsontext_decoder_read_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        Str name = jsontext_token_string(tok, d->out_alloc);
        if (map_get(obj, &name) != NULL) {
            Slice prev = burrow__jsontext_previous_token_or_value(d);
            return burrow__jsonv2_duplicate_name_error(
                d, jsontext_decoder_input_offset(d) - (int64_t)prev.len);
        }
        Any val = {NULL, NULL};
        err = jv_unmarshal_any_value(d, uo, &val);
        if (!map_set(obj, &name, &val))
            return burrow_err_out_of_memory;
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo))
                return err;
            err_unmarshal = jv_or(err, err_unmarshal);
        }
    }
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    return err_unmarshal;
}

/* unmarshalArrayAny. */
static Error jv_unmarshal_array_any(JsontextDecoder *d, JsontextOptions *uo, Any *out) {
    Error err = BURROW_NO_ERROR;
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    Slice arr = slice_make(d->out_alloc, TYPE_ANY, 0, 0);
    Slice *ap;
    if (!jv_box(d, TYPE_JSONV2_SLICE_ANY, &arr, out))
        return burrow_err_out_of_memory;
    ap = (Slice *)out->data;
    Error err_unmarshal = BURROW_NO_ERROR;
    while (jsontext_decoder_peek_kind(d) != ']') {
        Any val = {NULL, NULL};
        err = jv_unmarshal_any_value(d, uo, &val);
        Slice grown = slice_append(d->out_alloc, *ap, &val, 1);
        if (grown.len != ap->len + 1)
            return burrow_err_out_of_memory;
        *ap = grown;
        if (BURROW_FAILED(err)) {
            if (burrow__jsonv2_is_fatal(err, uo))
                return err;
            err_unmarshal = jv_or(err_unmarshal, err);
        }
    }
    (void)jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    return err_unmarshal;
}

/* unmarshalValueAny. */
static Error jv_unmarshal_any_value(JsontextDecoder *d, JsontextOptions *uo, Any *out) {
    JsontextKind k = jsontext_decoder_peek_kind(d);
    if (k == '{')
        return jv_unmarshal_object_any(d, uo, out);
    if (k == '[')
        return jv_unmarshal_array_any(d, uo, out);
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    switch (jsontext_value_kind(val)) {
    case 'n':
        out->t = NULL;
        out->data = NULL;
        return BURROW_NO_ERROR;
    case 'f':
    case 't': {
        bool b = jsontext_value_kind(val) == 't';
        return jv_box(d, TYPE_BOOL, &b, out) ? BURROW_NO_ERROR
                                             : burrow_err_out_of_memory;
    }
    case '"': {
        JV_SCRATCH(scratch);
        Str s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        bool oom = scratch.failed;
        Str kept = oom ? BURROW_STR_EMPTY : jv_keep_str(d, s, &oom);
        burrow__jsonbuf_free(&scratch);
        if (oom || !jv_box(d, TYPE_STRING, &kept, out))
            return burrow_err_out_of_memory;
        return BURROW_NO_ERROR;
    }
    default: {
        Error perr = BURROW_NO_ERROR;
        double fv = strconv_parse_float(str_from_bytes(val.p, val.len), 64, &perr);
        if (!jv_box(d, TYPE_FLOAT64, &fv, out))
            return burrow_err_out_of_memory;
        if (BURROW_FAILED(perr))
            return burrow__jsonv2_unmarshal_error_after_value(d, TYPE_FLOAT64,
                                                              jv_unwrap(perr));
        return BURROW_NO_ERROR;
    }
    }
}

/* Sets the interface at p to hold a value of type et at v. */
static void jv_iface_set(const Type *t, void *p, const Type *et, void *v) {
    if (t == TYPE_ANY) {
        Any *a = (Any *)p;
        a->t = et;
        a->data = v;
    } else if (t == TYPE_ERROR) {
        ((Error *)p)->data = v;
    } else if (t->size <= sizeof(Iface)) {
        ((Iface *)p)->data = v;
    }
}

static Error jv_unmarshal_interface(JsontextDecoder *d, const Type *t, void *p,
                                    JsontextOptions *uo) {
    Error err = BURROW_NO_ERROR;
    if (JV_HAS(uo, JSONFLAG_TAG_FLAGS)) {
        if (JV_GET(uo, JSONFLAG_STRING_TAG)) {
            if (!JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
                return burrow__jsonv2_unmarshal_error_before_skipping(
                    d, t, burrow__jsonv2_err_invalid_string_tag);
            if (JV_GET(uo, JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS))
                jsonflags_clear(uo, JSONFLAG_STRING_TAG);
        }
        if (JV_HAS(uo, JSONFLAG_FORMAT_TAG))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    }
    if (JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS) && !jv_is_zero(t, p)) {
        void *ev = NULL;
        const Type *et = jv_iface_elem(t, p, &ev);
        if (et != NULL && et->kind == KIND_POINTER && *(void **)ev != NULL) {
            if (jsontext_decoder_peek_kind(d) == 'n' &&
                et->elem->kind == KIND_POINTER) {
                (void)jsontext_decoder_read_token(d, &err);
                if (BURROW_FAILED(err))
                    return err;
                jv_zero(et->elem, *(void **)ev);
                return BURROW_NO_ERROR;
            }
        } else {
            jv_zero(t, p);
        }
    }
    if (jsontext_decoder_peek_kind(d) == 'n') {
        (void)jsontext_decoder_read_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
        jv_zero(t, p);
        return BURROW_NO_ERROR;
    }
    const Type *vt;
    void *v;
    if (jv_is_zero(t, p)) {
        if (t == TYPE_ANY &&
            !JV_GET(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES | JSONFLAG_FORMAT_TAG)) {
            Any out = {NULL, NULL};
            err = jv_unmarshal_any_value(d, uo, &out);
            if (out.t != NULL)
                *(Any *)p = out;
            return err;
        }
        JsontextKind k = jsontext_decoder_peek_kind(d);
        if (t != TYPE_ANY)
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_nil_interface);
        switch (k) {
        case 'f':
        case 't':
            vt = TYPE_BOOL;
            break;
        case '"':
            vt = TYPE_STRING;
            break;
        case '0':
            vt = TYPE_FLOAT64;
            break;
        case '{':
            vt = TYPE_JSONV2_MAP_STRING_ANY;
            break;
        case '[':
            vt = TYPE_JSONV2_SLICE_ANY;
            break;
        default: {
            unsigned flags = 0;
            (void)burrow__jsontext_read_value(d, &flags, &err);
            return err;
        }
        }
        v = jv_alloc(d->out_alloc, vt);
        if (v == NULL)
            return burrow_err_out_of_memory;
    } else {
        void *cur = NULL;
        vt = jv_iface_elem(t, p, &cur);
        if (vt == NULL || vt == &jv_type_ptr_error_string)
            return burrow__jsonv2_unmarshal_error_before_skipping(
                d, t, burrow__jsonv2_err_nil_interface);
        if (t != TYPE_ANY && t != TYPE_ERROR && t->size > sizeof(Iface)) {
            /* A value held inline has nowhere else to go. */
            return burrow__jsonv2_unmarshal_value(d, vt, cur, uo);
        }
        v = jv_alloc(d->out_alloc, vt);
        if (v == NULL)
            return burrow_err_out_of_memory;
        memcpy(v, cur, vt->size);
    }
    err = burrow__jsonv2_unmarshal_value(d, vt, v, uo);
    jv_iface_set(t, p, vt, v);
    return err;
}

/* ------------------------------------------------------------------- invalid */

static Error jv_unmarshal_invalid(JsontextDecoder *d, const Type *t) {
    if (JV_GET(&d->opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        unsigned flags = 0;
        Error err = BURROW_NO_ERROR;
        Slice val = burrow__jsontext_read_value(d, &flags, &err);
        if (BURROW_FAILED(err))
            return err;
        if (jsontext_value_kind(val) == 'n')
            return BURROW_NO_ERROR;
        return burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
    }
    return burrow__jsonv2_unmarshal_error_before(d, t, BURROW_NO_ERROR);
}

/* ------------------------------------------------------------------ dispatch */

Error burrow__jsonv2_marshal_value(JsontextEncoder *e, const Type *t, void *p,
                                   JsontextOptions *mo) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        return jv_marshal_bool(e, t, p, mo);
    case KIND_STRING:
        return jv_marshal_string(e, t, p, mo);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return jv_marshal_int(e, t, p, mo);
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return jv_marshal_uint(e, t, p, mo);
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return jv_marshal_float(e, t, p, mo);
    case KIND_MAP:
        return jv_marshal_map(e, t, p, mo);
    case KIND_STRUCT:
        return jv_marshal_struct(e, t, p, mo);
    case KIND_SLICE:
    case KIND_ARRAY:
        if (t->elem->kind == KIND_UINT8)
            return jv_marshal_bytes(e, t, p, mo);
        return jv_marshal_list(e, t, p, mo);
    case KIND_POINTER:
        return jv_marshal_pointer(e, t, p, mo);
    case KIND_INTERFACE:
        return jv_marshal_interface(e, t, p, mo);
    default:
        return burrow__jsonv2_marshal_error_before(e, t, BURROW_NO_ERROR);
    }
}

Error burrow__jsonv2_unmarshal_value(JsontextDecoder *d, const Type *t, void *p,
                                     JsontextOptions *uo) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        return jv_unmarshal_bool(d, t, p, uo);
    case KIND_STRING:
        return jv_unmarshal_string(d, t, p, uo);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return jv_unmarshal_int(d, t, p, uo);
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return jv_unmarshal_uint(d, t, p, uo);
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return jv_unmarshal_float(d, t, p, uo);
    case KIND_MAP:
        return jv_unmarshal_map(d, t, p, uo);
    case KIND_STRUCT:
        return jv_unmarshal_struct(d, t, p, uo);
    case KIND_SLICE:
    case KIND_ARRAY:
        if (t->elem->kind == KIND_UINT8)
            return jv_unmarshal_bytes(d, t, p, uo);
        return jv_unmarshal_list(d, t, p, uo);
    case KIND_POINTER:
        return jv_unmarshal_pointer(d, t, p, uo);
    case KIND_INTERFACE:
        return jv_unmarshal_interface(d, t, p, uo);
    default:
        return jv_unmarshal_invalid(d, t);
    }
}
