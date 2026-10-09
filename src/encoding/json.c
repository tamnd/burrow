/* encoding/json: the v1 API, as a layer over encoding/json/v2.
 *
 * Derived from Go's src/encoding/json/v2_decode.go, v2_encode.go,
 * v2_indent.go, v2_inject.go, v2_options.go and v2_scanner.go.
 * Go source: go1.27.1.
 *
 * Go's v1 hands v2 its error transforms and the MarshalerError constructor
 * through encoding/json/internal. Here v2 calls the burrow__json_ functions
 * at the bottom of this file directly, since the two are always linked
 * together.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json.h"

#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/utf8.h"

#include "json_internal.h"
#include "jsonv2_internal.h"

#include <string.h>

static JsonBuf jx_heap_buf(void) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    return b;
}

/* The same error, not merely one errors_is would match, which is Go's ==. */
static bool jx_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool jx_has_prefix(Str s, Str p) {
    return s.len >= p.len && (p.len == 0 || memcmp(s.p, p.p, (size_t)p.len) == 0);
}

static Str jx_cstr(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

/* ------------------------------------------------------------------ options */

static JsontextOptions jx_bool_option(uint64_t flag, bool v) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = flag;
    o.values = v ? flag : 0;
    return o;
}

JsontextOptions json_default_options_v1(void) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = JSONFLAG_DEFAULT_V1;
    o.values = JSONFLAG_DEFAULT_V1;
    return o;
}

JsontextOptions json_call_methods_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_CALL_METHODS_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_format_byte_array_as_array(bool v) {
    return jx_bool_option(JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY, v);
}

JsontextOptions json_format_bytes_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_format_duration_as_nano(bool v) {
    return jx_bool_option(JSONFLAG_FORMAT_DURATION_AS_NANO, v);
}

JsontextOptions json_match_case_sensitive_delimiter(bool v) {
    return jx_bool_option(JSONFLAG_MATCH_CASE_SENSITIVE_DELIMITER, v);
}

JsontextOptions json_merge_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_omit_empty_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_parse_bytes_with_loose_rfc4648(bool v) {
    return jx_bool_option(JSONFLAG_PARSE_BYTES_WITH_LOOSE_RFC4648, v);
}

JsontextOptions json_parse_time_with_loose_rfc3339(bool v) {
    return jx_bool_option(JSONFLAG_PARSE_TIME_WITH_LOOSE_RFC3339, v);
}

JsontextOptions json_report_errors_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_stringify_with_legacy_semantics(bool v) {
    return jx_bool_option(JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS, v);
}

JsontextOptions json_unmarshal_array_from_any_length(bool v) {
    return jx_bool_option(JSONFLAG_UNMARSHAL_ARRAY_FROM_ANY_LENGTH, v);
}

/* ------------------------------------------------------------------- errors
 *
 * Each error is its struct followed by its message, in one allocation that
 * also holds the text of every Str in the struct. */

#define JX_ERROR_DESC(var, name, T, hash)                                              \
    static const Type var = {                                                          \
        {(const Byte *)(name), (Int)(sizeof(name) - 1)},                               \
        {(const Byte *)"encoding/json", 13},                                           \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        hash,                                                                          \
        NULL,                                                                          \
    }

JX_ERROR_DESC(jx_syntax_desc, "SyntaxError", JsonSyntaxError, 0x6a787379U);
JX_ERROR_DESC(jx_unmarshal_type_desc, "UnmarshalTypeError", JsonUnmarshalTypeError,
              0x6a787574U);
JX_ERROR_DESC(jx_unmarshal_field_desc, "UnmarshalFieldError", JsonUnmarshalFieldError,
              0x6a787566U);
JX_ERROR_DESC(jx_invalid_unmarshal_desc, "InvalidUnmarshalError",
              JsonInvalidUnmarshalError, 0x6a786975U);
JX_ERROR_DESC(jx_unsupported_type_desc, "UnsupportedTypeError",
              JsonUnsupportedTypeError, 0x6a787374U);
JX_ERROR_DESC(jx_unsupported_value_desc, "UnsupportedValueError",
              JsonUnsupportedValueError, 0x6a787376U);
JX_ERROR_DESC(jx_invalid_utf8_desc, "InvalidUTF8Error", JsonInvalidUTF8Error,
              0x6a787538U);
JX_ERROR_DESC(jx_marshaler_desc, "MarshalerError", JsonMarshalerError, 0x6a786d65U);

const Type *const TYPE_JSON_SYNTAX_ERROR = &jx_syntax_desc;
const Type *const TYPE_JSON_UNMARSHAL_TYPE_ERROR = &jx_unmarshal_type_desc;
const Type *const TYPE_JSON_UNMARSHAL_FIELD_ERROR = &jx_unmarshal_field_desc;
const Type *const TYPE_JSON_INVALID_UNMARSHAL_ERROR = &jx_invalid_unmarshal_desc;
const Type *const TYPE_JSON_UNSUPPORTED_TYPE_ERROR = &jx_unsupported_type_desc;
const Type *const TYPE_JSON_UNSUPPORTED_VALUE_ERROR = &jx_unsupported_value_desc;
const Type *const TYPE_JSON_INVALID_UTF8_ERROR = &jx_invalid_utf8_desc;
const Type *const TYPE_JSON_MARSHALER_ERROR = &jx_marshaler_desc;

/* The messages, each written into a buffer. */

static void jx_syntax_text(JsonBuf *b, const void *self) {
    jsonbuf_str(b, ((const JsonSyntaxError *)self)->msg);
}

static void jx_unmarshal_type_text(JsonBuf *b, const void *self) {
    const JsonUnmarshalTypeError *e = (const JsonUnmarshalTypeError *)self;
    jsonbuf_str(b, JV_LIT("json: cannot unmarshal "));
    jsonbuf_str(b, e->value);
    if (e->struct_name.len > 0 || e->field.len > 0) {
        /* A last part that is all digits is likely an index into a Go slice
         * or array, not a struct field. */
        Int i = e->field.len;
        while (i > 0 && e->field.p[i - 1] != '.')
            i--;
        bool digits = i < e->field.len;
        for (Int j = i; j < e->field.len; j++)
            if (e->field.p[j] < '0' || e->field.p[j] > '9')
                digits = false;
        jsonbuf_str(b, digits ? JV_LIT(" into ") : JV_LIT(" into Go struct field "));
        jsonbuf_str(b, e->struct_name);
        jsonbuf_byte(b, '.');
        jsonbuf_str(b, e->field);
    } else {
        jsonbuf_str(b, JV_LIT(" into Go value"));
    }
    jsonbuf_str(b, JV_LIT(" of type "));
    burrow__jsonv2_put_type(b, e->type);
    if (BURROW_FAILED(e->err)) {
        jsonbuf_str(b, JV_LIT(": "));
        jsonbuf_str(b, error_text(e->err));
    }
}

static void jx_unmarshal_field_text(JsonBuf *b, const void *self) {
    const JsonUnmarshalFieldError *e = (const JsonUnmarshalFieldError *)self;
    jsonbuf_str(b, JV_LIT("json: cannot unmarshal object key "));
    burrow__jsontext_put_go_quote(b, e->key);
    jsonbuf_str(b, JV_LIT(" into unexported field "));
    jsonbuf_str(b, e->field_name);
    jsonbuf_str(b, JV_LIT(" of type "));
    burrow__jsonv2_put_type(b, e->type);
}

static void jx_invalid_unmarshal_text(JsonBuf *b, const void *self) {
    const JsonInvalidUnmarshalError *e = (const JsonInvalidUnmarshalError *)self;
    if (e->type == NULL) {
        jsonbuf_str(b, JV_LIT("json: Unmarshal(nil)"));
        return;
    }
    jsonbuf_str(b, JV_LIT("json: Unmarshal(nil *"));
    burrow__jsonv2_put_type(b, e->type);
    jsonbuf_byte(b, ')');
}

static void jx_unsupported_type_text(JsonBuf *b, const void *self) {
    jsonbuf_str(b, JV_LIT("json: unsupported type: "));
    burrow__jsonv2_put_type(b, ((const JsonUnsupportedTypeError *)self)->type);
}

static void jx_unsupported_value_text(JsonBuf *b, const void *self) {
    jsonbuf_str(b, JV_LIT("json: unsupported value: "));
    jsonbuf_str(b, ((const JsonUnsupportedValueError *)self)->str);
}

static void jx_invalid_utf8_text(JsonBuf *b, const void *self) {
    jsonbuf_str(b, JV_LIT("json: invalid UTF-8 in string: "));
    burrow__jsontext_put_go_quote(b, ((const JsonInvalidUTF8Error *)self)->s);
}

static void jx_marshaler_text(JsonBuf *b, const void *self) {
    const JsonMarshalerError *e = (const JsonMarshalerError *)self;
    jsonbuf_str(b, JV_LIT("json: error calling "));
    jsonbuf_str(b, e->source_func.len > 0 ? e->source_func : JV_LIT("MarshalJSON"));
    jsonbuf_str(b, JV_LIT(" for type *"));
    burrow__jsonv2_put_type(b, e->type);
    jsonbuf_str(b, JV_LIT(": "));
    jsonbuf_str(b, error_text(e->err));
}

/* A message built in a, or empty when a refuses. */
static Str jx_text_in(Alloc *a, void (*text)(JsonBuf *, const void *), const void *e) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    text(&b, e);
    if (b.failed) {
        burrow__jsonbuf_free(&b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(b.p, b.len);
}

Str json_syntax_error_error(const JsonSyntaxError *e) {
    return e->msg;
}

Str json_unmarshal_type_error_error(const JsonUnmarshalTypeError *e, Alloc *a) {
    return jx_text_in(a, jx_unmarshal_type_text, e);
}

Error json_unmarshal_type_error_unwrap(const JsonUnmarshalTypeError *e) {
    return e->err;
}

Str json_unmarshal_field_error_error(const JsonUnmarshalFieldError *e, Alloc *a) {
    return jx_text_in(a, jx_unmarshal_field_text, e);
}

Str json_invalid_unmarshal_error_error(const JsonInvalidUnmarshalError *e, Alloc *a) {
    return jx_text_in(a, jx_invalid_unmarshal_text, e);
}

Str json_unsupported_type_error_error(const JsonUnsupportedTypeError *e, Alloc *a) {
    return jx_text_in(a, jx_unsupported_type_text, e);
}

Str json_unsupported_value_error_error(const JsonUnsupportedValueError *e, Alloc *a) {
    return jx_text_in(a, jx_unsupported_value_text, e);
}

Str json_invalid_utf8_error_error(const JsonInvalidUTF8Error *e, Alloc *a) {
    return jx_text_in(a, jx_invalid_utf8_text, e);
}

Str json_marshaler_error_error(const JsonMarshalerError *e, Alloc *a) {
    return jx_text_in(a, jx_marshaler_text, e);
}

Error json_marshaler_error_unwrap(const JsonMarshalerError *e) {
    return e->err;
}

/* What the boxes have in common: where the struct keeps its Strs and its
 * wrapped error, so that one builder and one clone serve every type. */
typedef struct JxKind {
    const ErrorVT *vt;
    size_t size;
    void (*text)(JsonBuf *, const void *);
    size_t strs[2];
    int nstr;
    size_t err; /* offset of the Error, or 0 when there is none */
} JxKind;

/* The message goes after the struct, rounded up so the Str is aligned. */
static size_t jx_msg_offset(const JxKind *k) {
    size_t al = _Alignof(Str) > _Alignof(int64_t) ? _Alignof(Str) : _Alignof(int64_t);
    return (k->size + al - 1) / al * al;
}

static Str jx_box_message(const JxKind *k, const void *self) {
    Str s;
    memcpy(&s, (const Byte *)self + jx_msg_offset(k), sizeof(s));
    return s;
}

static Error jx_build(Alloc *a, const JxKind *k, const void *e);

#define JX_KIND_FUNCS(name)                                                            \
    static const JxKind *name##_kind_get(void);                                        \
    static Str name##_message(const void *self) {                                      \
        return jx_box_message(name##_kind_get(), self);                                \
    }                                                                                  \
    static Error name##_clone(const void *self, Alloc *a) {                            \
        return jx_clone(name##_kind_get(), self, a);                                   \
    }

static Error jx_clone(const JxKind *k, const void *self, Alloc *a);

static Error jx_unwrap_type(const void *self) {
    return ((const JsonUnmarshalTypeError *)self)->err;
}

static Error jx_unwrap_marshaler(const void *self) {
    return ((const JsonMarshalerError *)self)->err;
}

JX_KIND_FUNCS(jx_syntax)
JX_KIND_FUNCS(jx_unmarshal_type)
JX_KIND_FUNCS(jx_unmarshal_field)
JX_KIND_FUNCS(jx_invalid_unmarshal)
JX_KIND_FUNCS(jx_unsupported_type)
JX_KIND_FUNCS(jx_unsupported_value)
JX_KIND_FUNCS(jx_invalid_utf8)
JX_KIND_FUNCS(jx_marshaler)

#define JX_VT(name, desc, unwrap)                                                      \
    static const ErrorVT name##_vt = {&(desc), name##_message, unwrap,      NULL,      \
                                      NULL,    NULL,           name##_clone}

JX_VT(jx_syntax, jx_syntax_desc, NULL);
JX_VT(jx_unmarshal_type, jx_unmarshal_type_desc, jx_unwrap_type);
JX_VT(jx_unmarshal_field, jx_unmarshal_field_desc, NULL);
JX_VT(jx_invalid_unmarshal, jx_invalid_unmarshal_desc, NULL);
JX_VT(jx_unsupported_type, jx_unsupported_type_desc, NULL);
JX_VT(jx_unsupported_value, jx_unsupported_value_desc, NULL);
JX_VT(jx_invalid_utf8, jx_invalid_utf8_desc, NULL);
JX_VT(jx_marshaler, jx_marshaler_desc, jx_unwrap_marshaler);

static const JxKind jx_syntax_kind = {
    &jx_syntax_vt,
    sizeof(JsonSyntaxError),
    jx_syntax_text,
    {offsetof(JsonSyntaxError, msg), 0},
    1,
    0,
};
static const JxKind jx_unmarshal_type_kind = {
    &jx_unmarshal_type_vt,
    sizeof(JsonUnmarshalTypeError),
    jx_unmarshal_type_text,
    {offsetof(JsonUnmarshalTypeError, value), offsetof(JsonUnmarshalTypeError, field)},
    2,
    offsetof(JsonUnmarshalTypeError, err),
};
static const JxKind jx_unmarshal_field_kind = {
    &jx_unmarshal_field_vt,
    sizeof(JsonUnmarshalFieldError),
    jx_unmarshal_field_text,
    {offsetof(JsonUnmarshalFieldError, key),
     offsetof(JsonUnmarshalFieldError, field_name)},
    2,
    0,
};
static const JxKind jx_invalid_unmarshal_kind = {
    &jx_invalid_unmarshal_vt,
    sizeof(JsonInvalidUnmarshalError),
    jx_invalid_unmarshal_text,
    {0, 0},
    0,
    0,
};
static const JxKind jx_unsupported_type_kind = {
    &jx_unsupported_type_vt,
    sizeof(JsonUnsupportedTypeError),
    jx_unsupported_type_text,
    {0, 0},
    0,
    0,
};
static const JxKind jx_unsupported_value_kind = {
    &jx_unsupported_value_vt,
    sizeof(JsonUnsupportedValueError),
    jx_unsupported_value_text,
    {offsetof(JsonUnsupportedValueError, str), 0},
    1,
    0,
};
static const JxKind jx_invalid_utf8_kind = {
    &jx_invalid_utf8_vt,
    sizeof(JsonInvalidUTF8Error),
    jx_invalid_utf8_text,
    {offsetof(JsonInvalidUTF8Error, s), 0},
    1,
    0,
};
static const JxKind jx_marshaler_kind = {
    &jx_marshaler_vt,
    sizeof(JsonMarshalerError),
    jx_marshaler_text,
    {offsetof(JsonMarshalerError, source_func), 0},
    1,
    offsetof(JsonMarshalerError, err),
};

/* The kinds refer to the vtables and the vtables' functions to the kinds, so
 * the functions reach them through these. MSVC won't take a forward
 * declaration of a static const object. */
#define JX_KIND_GET(name)                                                              \
    static const JxKind *name##_kind_get(void) {                                       \
        return &name##_kind;                                                           \
    }
JX_KIND_GET(jx_syntax)
JX_KIND_GET(jx_unmarshal_type)
JX_KIND_GET(jx_unmarshal_field)
JX_KIND_GET(jx_invalid_unmarshal)
JX_KIND_GET(jx_unsupported_type)
JX_KIND_GET(jx_unsupported_value)
JX_KIND_GET(jx_invalid_utf8)
JX_KIND_GET(jx_marshaler)
#undef JX_KIND_GET

/* UnmarshalTypeError has a third Str, struct_name, which the table above has
 * no room for. It is always a type's name, which lives as long as the type,
 * so it is left pointing where it points. */

/* One allocation holding the struct, its message and the text of its Strs.
 * The zero Error when a refuses. */
static Error jx_build(Alloc *a, const JxKind *k, const void *e) {
    JsonBuf msg = jx_heap_buf();
    k->text(&msg, e);
    if (msg.failed) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    size_t head = jx_msg_offset(k) + sizeof(Str);
    size_t extra = (size_t)msg.len;
    Str in[2];
    for (int i = 0; i < k->nstr; i++) {
        memcpy(&in[i], (const Byte *)e + k->strs[i], sizeof(Str));
        extra += (size_t)in[i].len;
    }
    Byte *box = (Byte *)mem_alloc_nozero(a, head + extra, BURROW_ALIGN_MAX);
    if (box == NULL) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    memcpy(box, e, k->size);
    Byte *p = box + head;
    for (int i = 0; i < k->nstr; i++) {
        Str s = BURROW_STR_EMPTY;
        if (in[i].len > 0) {
            memcpy(p, in[i].p, (size_t)in[i].len);
            s = str_from_bytes(p, in[i].len);
            p += in[i].len;
        }
        memcpy(box + k->strs[i], &s, sizeof(s));
    }
    Str m = BURROW_STR_EMPTY;
    if (msg.len > 0) {
        memcpy(p, msg.p, (size_t)msg.len);
        m = str_from_bytes(p, msg.len);
    }
    memcpy(box + jx_msg_offset(k), &m, sizeof(m));
    burrow__jsonbuf_free(&msg);
    return (Error){k->vt, box};
}

/* A new error in the error arena, or out of memory. */
static Error jx_new(const JxKind *k, const void *e) {
    Error made = jx_build(error_allocator(), k, e);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

static Error jx_clone(const JxKind *k, const void *self, Alloc *a) {
    Byte copy[sizeof(JsonUnmarshalTypeError) > sizeof(JsonMarshalerError)
                  ? sizeof(JsonUnmarshalTypeError)
                  : sizeof(JsonMarshalerError)];
    memcpy(copy, self, k->size);
    if (k->err != 0) {
        Error inner;
        memcpy(&inner, copy + k->err, sizeof(inner));
        inner = error_retain(a, inner);
        memcpy(copy + k->err, &inner, sizeof(inner));
    }
    Error made = jx_build(a, k, copy);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

/* --------------------------------------------------------------- transforms */

/* strings.NewReplacer's pairs for turning v2's syntax messages into v1's. v2
 * says "object name" to match RFC 8259, where v1 said "object key". */
static const struct {
    Str from, to;
} jx_syntax_replacer[] = {
    {{(const Byte *)"object name", 11}, {(const Byte *)"object key", 10}},
    {{(const Byte *)"at start of value", 17},
     {(const Byte *)"looking for beginning of value", 30}},
    {{(const Byte *)"at start of string", 18},
     {(const Byte *)"looking for beginning of object key string", 42}},
    {{(const Byte *)"after object value", 18},
     {(const Byte *)"after object key:value pair", 27}},
    {{(const Byte *)"in number", 9}, {(const Byte *)"in numeric literal", 18}},
};

static Int jx_index(Str s, Str sub) {
    for (Int i = 0; i + sub.len <= s.len; i++)
        if (memcmp(s.p + i, sub.p, (size_t)sub.len) == 0)
            return i;
    return -1;
}

/* transformSyntacticError. */
static Error jx_transform_syntactic(Error err) {
    if (BURROW_OK(err))
        return err;
    if (err.vt->self_type != TYPE_JSONTEXT_SYNTACTIC_ERROR) {
        /* v1 never wrapped I/O errors. */
        if (burrow__jsontext_is_io_error(err))
            return errors_unwrap(err);
        return err;
    }
    const JsontextSyntacticError *serr = (const JsontextSyntacticError *)err.data;
    if (burrow__jsontext_is_io_error(serr->err))
        return errors_unwrap(serr->err);
    Str msg = jx_same(serr->err, io_err_unexpected_eof)
                  ? JV_LIT("unexpected end of JSON input")
                  : error_text(serr->err);
    Int cut = jx_index(msg, JV_LIT(" (expecting"));
    if (cut >= 0 && jx_index(msg, JV_LIT(" in literal")) < 0)
        msg.len = cut;
    JsonBuf b = jx_heap_buf();
    for (Int i = 0; i < msg.len;) {
        bool hit = false;
        for (size_t r = 0;
             r < sizeof(jx_syntax_replacer) / sizeof(jx_syntax_replacer[0]); r++) {
            Str rest = str_from_bytes(msg.p + i, msg.len - i);
            if (jx_has_prefix(rest, jx_syntax_replacer[r].from)) {
                jsonbuf_str(&b, jx_syntax_replacer[r].to);
                i += jx_syntax_replacer[r].from.len;
                hit = true;
                break;
            }
        }
        if (!hit)
            jsonbuf_byte(&b, msg.p[i++]);
    }
    if (b.failed) {
        burrow__jsonbuf_free(&b);
        return burrow_err_out_of_memory;
    }
    JsonSyntaxError se = {str_from_bytes(b.p, b.len), serr->byte_offset};
    Error r = jx_new(&jx_syntax_kind, &se);
    burrow__jsonbuf_free(&b);
    return r;
}

Error burrow__json_new_marshaler_error(const Type *t, Error err, const char *src) {
    JsonMarshalerError me = {t, err, jx_cstr(src)};
    return jx_new(&jx_marshaler_kind, &me);
}

/* transformMarshalError. */
Error burrow__json_transform_marshal_error(Error err) {
    const Jsonv2SemanticError *se = burrow__jsonv2_as_semantic(err);
    if (se != NULL) {
        /* Historically only reported for types JSON cannot hold, such as
         * channels and functions. */
        if (BURROW_OK(se->err)) {
            JsonUnsupportedTypeError ute = {se->go_type};
            return jx_new(&jx_unsupported_type_kind, &ute);
        }
        /* And this for NaN, the infinities and cycles. */
        JsonBuf b = jx_heap_buf();
        jsonbuf_str(&b, error_text(se->err));
        if (jx_same(se->err, burrow__jsonv2_err_cycle) && se->go_type != NULL) {
            jsonbuf_str(&b, JV_LIT(" via "));
            burrow__jsonv2_put_type(&b, se->go_type);
        }
        if (b.failed) {
            burrow__jsonbuf_free(&b);
            return burrow_err_out_of_memory;
        }
        Str s = str_from_bytes(b.p, b.len);
        Str prefix = JV_LIT("unsupported value: ");
        if (jx_has_prefix(s, prefix))
            s = str_from_bytes(s.p + prefix.len, s.len - prefix.len);
        JsonUnsupportedValueError uve = {s};
        Error r = jx_new(&jx_unsupported_value_kind, &uve);
        burrow__jsonbuf_free(&b);
        return r;
    }
    if (BURROW_FAILED(err) && err.vt == &jx_marshaler_vt) {
        JsonMarshalerError me = *(const JsonMarshalerError *)err.data;
        me.err = jx_transform_syntactic(me.err);
        return jx_new(&jx_marshaler_kind, &me);
    }
    return jx_transform_syntactic(err);
}

static bool jx_numeric_kind(const Type *t) {
    if (t == NULL)
        return false;
    Kind k = t->kind;
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64 || k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 ||
           k == KIND_UINT32 || k == KIND_UINT64 || k == KIND_UINTPTR ||
           k == KIND_FLOAT32 || k == KIND_FLOAT64;
}

/* transformUnmarshalError. Errors from unmarshal methods were never wrapped
 * and come back as they were. */
Error burrow__json_transform_unmarshal_error(Any root, Error err) {
    const Jsonv2SemanticError *se = burrow__jsonv2_as_semantic(err);
    if (se == NULL)
        return jx_transform_syntactic(err);
    if (jx_same(se->err, burrow__jsonv2_err_non_nil_reference)) {
        JsonInvalidUnmarshalError iue = {se->go_type};
        return jx_new(&jx_invalid_unmarshal_kind, &iue);
    }
    if (jx_same(se->err, jsonv2_err_unknown_name)) {
        Str last = jsontext_pointer_last_token(se->json_pointer, heap_allocator());
        Error r =
            burrow__jsonv2_errorf_in(error_allocator(), "json: unknown field %q", last);
        mem_free(heap_allocator(), (void *)(uintptr_t)last.p, (size_t)last.len, 1);
        return r;
    }
    Error inner = se->err;
    if (jx_same(inner, burrow__jsonv2_err_nil_interface))
        inner = BURROW_NO_ERROR; /* not descriptive, for historical reasons */

    /* The position has always been reported unevenly. struct_name is the root
     * type rather than the struct the field is in, which is what the message
     * suggests it meant, and the pointer is written with dots, which is as
     * ambiguous as the old form was. v2's errors are exact. */
    JsonBuf value = jx_heap_buf();
    switch (se->json_kind) {
    case 'n':
    case '"':
    case '0':
        jsonbuf_str(&value, jsontext_kind_string(se->json_kind));
        break;
    case 'f':
    case 't':
        jsonbuf_str(&value, JV_LIT("bool"));
        break;
    case '[':
    case ']':
        jsonbuf_str(&value, JV_LIT("array"));
        break;
    case '{':
    case '}':
        jsonbuf_str(&value, JV_LIT("object"));
        break;
    default:
        break;
    }
    if (se->json_value.len > 0) {
        Str jv = str_from_bytes(se->json_value.p, se->json_value.len);
        JsonBuf unq = jx_heap_buf();
        bool strconv_err =
            jx_same(inner, strconv_err_range) || jx_same(inner, strconv_err_syntax);
        if (strconv_err && jx_numeric_kind(se->go_type)) {
            value.len = 0;
            jsonbuf_str(&value, JV_LIT("number"));
            if (se->json_kind == '"') {
                burrow__jsonwire_append_unquote(&unq, jv.p, jv.len);
                jv = str_from_bytes(unq.p, unq.len);
            }
            inner = BURROW_NO_ERROR;
        }
        jsonbuf_byte(&value, ' ');
        jsonbuf_str(&value, jv);
        burrow__jsonbuf_free(&unq);
    }
    JsonBuf field = jx_heap_buf();
    Str ptr = se->json_pointer;
    for (Int i = ptr.len > 0 && ptr.p[0] == '/' ? 1 : 0; i < ptr.len; i++)
        jsonbuf_byte(&field, ptr.p[i] == '/' ? '.' : ptr.p[i]);
    JsonUnmarshalTypeError ute;
    memset(&ute, 0, sizeof(ute));
    ute.value = str_from_bytes(value.p, value.len);
    ute.type = se->go_type;
    ute.offset = se->byte_offset;
    if (root.t != NULL && ptr.len > 0)
        ute.struct_name = root.t->name;
    ute.field = str_from_bytes(field.p, field.len);
    ute.err = jx_transform_syntactic(inner);
    Error r = value.failed || field.failed ? burrow_err_out_of_memory
                                           : jx_new(&jx_unmarshal_type_kind, &ute);
    burrow__jsonbuf_free(&value);
    burrow__jsonbuf_free(&field);
    return r;
}

/* --------------------------------------------------------------- functions */

Slice json_marshal(Alloc *a, Any v, Error *err) {
    return jsonv2_marshal_v(a, v, err, 1, json_default_options_v1());
}

/* Whether s has anything but spaces and tabs. */
static bool jx_not_blank(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] != ' ' && s.p[i] != '\t')
            return true;
    return false;
}

/* appendIndent. v2 allows only spaces and tabs in the prefix and indent,
 * where v1 allowed anything, so anything else is written as spaces of the
 * same length and put in afterwards. v2 also drops the whitespace after the
 * value, which v1 kept. On an error dst comes back as it was. */
static Slice jx_append_indent(Alloc *a, Slice dst, Str src, Str prefix, Str indent,
                              Error *err) {
    Int dst_len = dst.len;
    bool invalid = jx_not_blank(prefix) || jx_not_blank(indent);
    JsonBuf spaces = jx_heap_buf();
    Str use_prefix = prefix;
    Str use_indent = indent;
    if (invalid) {
        Int n = prefix.len > indent.len ? prefix.len : indent.len;
        for (Int i = 0; i < n; i++)
            jsonbuf_byte(&spaces, ' ');
        if (spaces.failed) {
            burrow__jsonbuf_free(&spaces);
            *err = burrow_err_out_of_memory;
            return dst;
        }
        use_prefix = str_from_bytes(spaces.p, prefix.len);
        use_indent = str_from_bytes(spaces.p, indent.len);
    }
    Error e = BURROW_NO_ERROR;
    Slice out = jsontext_append_format_v(
        a, dst, src, &e, 7, json_report_errors_with_legacy_semantics(true),
        jsontext_allow_duplicate_names(true), jsontext_allow_invalid_utf8(true),
        jsontext_preserve_raw_strings(true), jsontext_multiline(true),
        jsontext_with_indent_prefix(use_prefix), jsontext_with_indent(use_indent));
    burrow__jsonbuf_free(&spaces);
    if (BURROW_FAILED(e)) {
        out.len = dst_len;
        *err = jx_transform_syntactic(e);
        return out;
    }
    if (invalid) {
        Byte *b = (Byte *)out.p;
        for (Int i = dst_len; i < out.len; i++) {
            if (b[i] != '\n')
                continue;
            Int start = i + 1;
            Int end = start;
            while (end < out.len && b[end] == ' ')
                end++;
            /* len(prefix) + depth*len(indent) spaces. */
            Int j = start;
            for (Int k = 0; k < prefix.len && j < end; k++)
                b[j++] = prefix.p[k];
            while (j < end && indent.len > 0)
                for (Int k = 0; k < indent.len && j < end; k++)
                    b[j++] = indent.p[k];
            i = end - 1;
        }
    }
    Int n = src.len;
    while (n > 0 && (src.p[n - 1] == ' ' || src.p[n - 1] == '\n' ||
                     src.p[n - 1] == '\r' || src.p[n - 1] == '\t'))
        n--;
    if (n < src.len) {
        Slice tail = {(void *)(uintptr_t)(src.p + n), src.len - n, src.len - n,
                      TYPE_BYTE};
        Slice grown = slice_append_slice(a, out, tail);
        /* A copy leaves the old buffer to us, unless it was dst's. */
        if (grown.p != out.p && out.p != dst.p)
            mem_free(a, out.p, (size_t)out.cap, 1);
        if (grown.p == NULL) {
            *err = burrow_err_out_of_memory;
            return dst;
        }
        out = grown;
    }
    *err = BURROW_NO_ERROR;
    return out;
}

Slice json_marshal_indent(Alloc *a, Any v, Str prefix, Str indent, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice b = json_marshal(a, v, &e);
    Slice none = slice_nil(TYPE_BYTE);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    Slice out =
        jx_append_indent(a, none, str_from_bytes(b.p, b.len), prefix, indent, &e);
    mem_free(a, b.p, (size_t)b.cap, 1);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? none : out;
}

Error json_unmarshal(Alloc *a, Slice data, Any v) {
    return jsonv2_unmarshal_v(a, data, v, 1, json_default_options_v1());
}

/* checkValid. */
static Error jx_check_valid(Slice data) {
    JsontextDecoder d;
    burrow__jsontext_decoder_init(&d, heap_allocator());
    IoReader none = {NULL, NULL};
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    burrow__jsontext_decoder_setup(&d, none, false, (const Byte *)data.p, data.len, &o);
    jsonflags_set(&d.opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS |
                               JSONFLAG_ALLOW_DUPLICATE_NAMES |
                               JSONFLAG_ALLOW_INVALID_UTF8 | 1);
    Error err = BURROW_NO_ERROR;
    (void)jsontext_decoder_read_value(&d, &err);
    if (BURROW_FAILED(err)) {
        if (jx_same(err, io_eof)) {
            int64_t offset = jsontext_decoder_input_offset(&d) +
                             (int64_t)jsontext_decoder_unread_buffer(&d).len;
            err = burrow__jsontext_syntactic_new(offset, BURROW_STR_EMPTY,
                                                 io_err_unexpected_eof);
        }
    } else {
        err = burrow__jsontext_check_eof(&d);
    }
    burrow__jsontext_decoder_release(&d);
    return jx_transform_syntactic(err);
}

bool json_valid(Slice data) {
    return BURROW_OK(jx_check_valid(data));
}

/* Appends b to dst. */
static Error jx_buffer_write(BytesBuffer *dst, Slice b) {
    Error err = BURROW_NO_ERROR;
    bytes_buffer_write(dst, b, &err);
    return err;
}

Error json_compact(BytesBuffer *dst, Slice src) {
    Error e = BURROW_NO_ERROR;
    Slice out = jsontext_append_format_v(
        heap_allocator(), slice_nil(TYPE_BYTE), str_from_bytes(src.p, src.len), &e, 4,
        json_report_errors_with_legacy_semantics(true),
        jsontext_allow_duplicate_names(true), jsontext_allow_invalid_utf8(true),
        jsontext_preserve_raw_strings(true));
    if (BURROW_OK(e))
        e = jx_buffer_write(dst, out);
    else
        e = jx_transform_syntactic(e);
    mem_free(heap_allocator(), out.p, (size_t)out.cap, 1);
    return e;
}

Error json_indent(BytesBuffer *dst, Slice src, Str prefix, Str indent) {
    Error e = BURROW_NO_ERROR;
    Slice out = jx_append_indent(heap_allocator(), slice_nil(TYPE_BYTE),
                                 str_from_bytes(src.p, src.len), prefix, indent, &e);
    /* Go writes whatever appendIndent gave back, which is nothing on an
     * error. */
    Error w = out.len > 0 ? jx_buffer_write(dst, out) : BURROW_NO_ERROR;
    mem_free(heap_allocator(), out.p, (size_t)out.cap, 1);
    return BURROW_FAILED(e) ? e : w;
}

void json_html_escape(BytesBuffer *dst, Slice src) {
    static const char hex[] = "0123456789abcdef";
    const Byte *s = (const Byte *)src.p;
    JsonBuf b = jx_heap_buf();
    Int start = 0;
    /* The characters can only appear in string literals, so one byte at a
     * time does. */
    for (Int i = 0; i < src.len; i++) {
        Byte c = s[i];
        if (c == '<' || c == '>' || c == '&') {
            jsonbuf_put(&b, s + start, i - start);
            Byte esc[6] = {'\\', 'u', '0', '0', (Byte)hex[c >> 4], (Byte)hex[c & 0xF]};
            jsonbuf_put(&b, esc, 6);
            start = i + 1;
        }
        /* U+2028 and U+2029, E2 80 A8 and E2 80 A9. */
        if (c == 0xE2 && i + 2 < src.len && s[i + 1] == 0x80 &&
            (s[i + 2] & ~1) == 0xA8) {
            jsonbuf_put(&b, s + start, i - start);
            Byte esc[6] = {'\\', 'u', '2', '0', '2', (Byte)hex[s[i + 2] & 0xF]};
            jsonbuf_put(&b, esc, 6);
            start = i + 3;
        }
    }
    if (start < src.len)
        jsonbuf_put(&b, s + start, src.len - start);
    if (!b.failed) {
        Slice out = {b.p, b.len, b.len, TYPE_BYTE};
        (void)jx_buffer_write(dst, out);
    }
    burrow__jsonbuf_free(&b);
}

/* ------------------------------------------------------------------- Number */

Str json_number_string(JsonNumber n) {
    return n;
}

double json_number_float64(JsonNumber n, Error *err) {
    return strconv_parse_float(n, 64, err);
}

int64_t json_number_int64(JsonNumber n, Error *err) {
    return strconv_parse_int(n, 10, 64, err);
}

/* Whether the coder is where an object name goes next. */
static bool jx_at_name(JsontextKind k, int64_t n) {
    return k == '{' && n % 2 == 0;
}

static bool jx_is_number(const Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    Int got = burrow__jsonwire_consume_number(p, n, &err);
    return got == n && BURROW_OK(err);
}

/* Number.MarshalJSONTo. */
static Error jx_number_marshal_json_to(JsonNumber *np, Jsonv2EncoderArg enc) {
    bool stringify = false;
    (void)jsonv2_get_option(enc->opts, jsonv2_stringify_numbers, &stringify);
    int64_t len = 0;
    JsontextKind k =
        jsontext_encoder_stack_index(enc, jsontext_encoder_stack_depth(enc), &len);
    if (jx_at_name(k, len))
        stringify = true;
    Str n = np->len > 0 ? *np : JV_LIT("0");
    JsonBuf val = jx_heap_buf();
    if (stringify)
        jsonbuf_byte(&val, '"');
    jsonbuf_str(&val, n);
    if (stringify)
        jsonbuf_byte(&val, '"');
    if (val.failed) {
        burrow__jsonbuf_free(&val);
        return burrow_err_out_of_memory;
    }
    Error r;
    Int off = stringify ? 1 : 0;
    if (!jx_is_number(val.p + off, val.len - 2 * off)) {
        Error syntax = strconv_err_syntax;
        r = fmt_errorf_v("cannot parse %q as JSON number: %w",
                         str_from_bytes(val.p, val.len), syntax);
    } else {
        JsontextValue v = {val.p, val.len, val.len, TYPE_BYTE};
        r = jsontext_encoder_write_value(enc, v);
    }
    burrow__jsonbuf_free(&val);
    return r;
}

static Error jx_number_semantic(JsontextKind k, JsontextValue value, Error err) {
    Jsonv2SemanticError s;
    memset(&s, 0, sizeof(s));
    s.json_kind = k;
    s.json_value = value;
    s.go_type = TYPE_JSON_NUMBER;
    s.err = err;
    return burrow__jsonv2_semantic_new(&s);
}

/* Number.UnmarshalJSONFrom. */
static Error jx_number_unmarshal_json_from(JsonNumber *np, Alloc *a,
                                           Jsonv2DecoderArg dec) {
    bool stringify = false;
    (void)jsonv2_get_option(dec->opts, jsonv2_stringify_numbers, &stringify);
    int64_t len = 0;
    JsontextKind k =
        jsontext_decoder_stack_index(dec, jsontext_decoder_stack_depth(dec), &len);
    if (jx_at_name(k, len))
        stringify = true;
    Error err = BURROW_NO_ERROR;
    JsontextValue val = jsontext_decoder_read_value(dec, &err);
    if (BURROW_FAILED(err))
        return err;
    k = jsontext_value_kind(val);
    Str keep;
    switch (k) {
    case 'n':
        if (!jsonflags_get(&dec->opts, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            *np = BURROW_STR_EMPTY;
        return BURROW_NO_ERROR;
    case '"': {
        JsonBuf unq = jx_heap_buf();
        burrow__jsonwire_append_unquote(&unq, (const Byte *)val.p, val.len);
        if (unq.failed) {
            burrow__jsonbuf_free(&unq);
            return burrow_err_out_of_memory;
        }
        if (!jx_is_number(unq.p, unq.len)) {
            burrow__jsonbuf_free(&unq);
            return jx_number_semantic(k, val, strconv_err_syntax);
        }
        Int n = unq.len;
        keep = str_clone(a, str_from_bytes(unq.p, n));
        burrow__jsonbuf_free(&unq);
        if (keep.len != n)
            return burrow_err_out_of_memory;
        *np = keep;
        return BURROW_NO_ERROR;
    }
    case '0':
        if (stringify)
            break;
        keep = str_clone(a, str_from_bytes(val.p, val.len));
        if (keep.len != val.len)
            return burrow_err_out_of_memory;
        *np = keep;
        return BURROW_NO_ERROR;
    default:
        break;
    }
    JsontextValue none = {NULL, 0, 0, TYPE_BYTE};
    return jx_number_semantic(k, none, BURROW_NO_ERROR);
}

#define JX_NUMBER_METHODS(M, T)                                                        \
    M(T, MarshalJSONTo, jx_number_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)         \
    M(T, UnmarshalJSONFrom, jx_number_unmarshal_json_from,                             \
      JSONV2_SIG_UNMARSHAL_JSON_FROM)
BURROW_METHODS_DEFINE(JsonNumber, JX_NUMBER_METHODS);

const Type burrow_type_JsonNumber = {
    {(const Byte *)"Number", 6},
    {(const Byte *)"encoding/json", 13},
    KIND_STRING,
    (uint32_t)sizeof(JsonNumber),
    (uint16_t)_Alignof(JsonNumber),
    0,
    (uint16_t)(sizeof burrow__methods_JsonNumber /
               sizeof burrow__methods_JsonNumber[0]),
    NULL,
    burrow__methods_JsonNumber,
    NULL,
    NULL,
    0,
    0x6a786e75U, /* "jxnu" */
    NULL,
};

/* ------------------------------------------------------------------ Delim */

Str json_delim_string(JsonDelim d, Alloc *a) {
    Slice b = utf8_append_rune(a, slice_nil(TYPE_BYTE), d);
    return str_from_bytes(b.p, b.len);
}

static Str jx_delim_m_string(JsonDelim *self) {
    return json_delim_string(*self, error_allocator());
}

#define JX_DELIM_SIG_STRING(IN, OUT) OUT(Str)

#define JX_DELIM_METHODS(M, T) M(T, String, jx_delim_m_string, JX_DELIM_SIG_STRING)
BURROW_METHODS_DEFINE(JsonDelim, JX_DELIM_METHODS);

const Type burrow_type_JsonDelim = {
    {(const Byte *)"Delim", 5},
    {(const Byte *)"encoding/json", 13},
    KIND_INT32,
    (uint32_t)sizeof(JsonDelim),
    (uint16_t)_Alignof(JsonDelim),
    0,
    (uint16_t)(sizeof burrow__methods_JsonDelim / sizeof burrow__methods_JsonDelim[0]),
    NULL,
    burrow__methods_JsonDelim,
    NULL,
    NULL,
    0,
    0x6a78646cU, /* "jxdl" */
    NULL,
};

/* ---------------------------------------------------------------- Decoder */

struct JsonDecoder {
    JsontextDecoder *dec;
    JsontextOptions opts;
    Error err;
    /* Whether More was called, and whether Token hit the end, since the last
     * Decode or Token. */
    bool had_peeked;
    bool had_eof;
    Alloc *a;
    /* Where err lives, so that it goes when the decoder does. */
    Arena earena;
    BytesReader buffered;
};

JsonDecoder *json_new_decoder(Alloc *a, IoReader r) {
    JsonDecoder *d =
        (JsonDecoder *)mem_alloc(a, sizeof(JsonDecoder), _Alignof(JsonDecoder));
    if (d == NULL)
        return NULL;
    d->a = a;
    arena_init(&d->earena, a, 0);
    d->opts = json_default_options_v1();
    d->dec = jsontext_new_decoder_v(a, r, 1, d->opts);
    if (d->dec == NULL) {
        mem_free(a, d, sizeof(JsonDecoder), _Alignof(JsonDecoder));
        return NULL;
    }
    return d;
}

void json_decoder_free(JsonDecoder *d) {
    if (d == NULL)
        return;
    jsontext_decoder_free(d->dec);
    arena_free(&d->earena);
    mem_free(d->a, d, sizeof(JsonDecoder), _Alignof(JsonDecoder));
}

void json_decoder_use_number(JsonDecoder *d) {
    jsonflags_set(&d->opts, JSONFLAG_UNMARSHAL_ANY_WITH_RAW_NUMBER | 1);
}

void json_decoder_disallow_unknown_fields(JsonDecoder *d) {
    d->opts = jsonv2_join_options_v(2, d->opts, jsonv2_reject_unknown_members(true));
}

/* Keeps err as the decoder's sticky error and returns it. */
static Error jx_decoder_fail(JsonDecoder *d, Error err) {
    d->err = error_retain(arena_allocator(&d->earena), err);
    return d->err;
}

static Error jx_decode_with(JsonDecoder *d, Alloc *va, Any v) {
    if (BURROW_FAILED(d->err))
        return d->err;
    Error err = BURROW_NO_ERROR;
    JsontextValue b = jsontext_decoder_read_value(d->dec, &err);
    if (BURROW_FAILED(err)) {
        err = jx_transform_syntactic(err);
        /* Decode has always said this differently from Unmarshal. */
        if (str_eq(error_text(err), JV_LIT("unexpected end of JSON input")))
            err = io_err_unexpected_eof;
        return jx_decoder_fail(d, err);
    }
    d->had_peeked = false;
    d->had_eof = false;
    Slice in = {(void *)(uintptr_t)b.p, b.len, b.len, TYPE_BYTE};
    return jsonv2_unmarshal_v(va, in, v, 1, d->opts);
}

Error json_decoder_decode(JsonDecoder *d, Any v) {
    return jx_decode_with(d, d->a, v);
}

Error json_decoder_decode_in(JsonDecoder *d, Alloc *a, Any v) {
    return jx_decode_with(d, a, v);
}

IoReader json_decoder_buffered(JsonDecoder *d) {
    bytes_reader_reset(&d->buffered, jsontext_decoder_unread_buffer(d->dec));
    return bytes_reader_as_io_reader(&d->buffered);
}

/* Whether s is nothing but whitespace, commas and colons. */
static bool jx_only_separators(Slice s) {
    const Byte *p = (const Byte *)s.p;
    for (Int i = 0; i < s.len; i++)
        if (p[i] != ' ' && p[i] != '\r' && p[i] != '\n' && p[i] != '\t' &&
            p[i] != ',' && p[i] != ':')
            return false;
    return true;
}

/* A copy of the n bytes at p in d's allocator, as the data of an Any. */
static JsonToken jx_token_copy(JsonDecoder *d, const Type *t, const void *p, size_t n,
                               size_t align, Error *err) {
    JsonToken tok = {NULL, NULL};
    void *q = mem_alloc(d->a, n, align);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return tok;
    }
    memcpy(q, p, n);
    tok.t = t;
    tok.data = q;
    return tok;
}

static const bool jx_false = false;
static const bool jx_true = true;
static const JsonDelim jx_delims[] = {'{', '}', '[', ']'};

JsonToken json_decoder_token(JsonDecoder *d, Error *err) {
    JsonToken none = {NULL, NULL};
    Error e = BURROW_NO_ERROR;
    BURROW_OUT(err, e);
    if (BURROW_FAILED(d->err)) {
        BURROW_OUT(err, d->err);
        return none;
    }
    JsontextToken tok = jsontext_decoder_read_token(d->dec, &e);
    if (BURROW_FAILED(e)) {
        /* v1 reports io_eof when the input stops where a value could have
         * ended, and io_err_unexpected_eof, unwrapped, when it stops inside a
         * token. An io_err_unexpected_eof from the reader itself is passed on
         * as it is. */
        if (errors_is(e, io_err_unexpected_eof) && !burrow__jsontext_is_io_error(e)) {
            if (jx_only_separators(jsontext_decoder_unread_buffer(d->dec))) {
                d->had_eof = true;
                BURROW_OUT(err, io_eof);
                return none;
            }
            BURROW_OUT(err, io_err_unexpected_eof);
            return none;
        }
        BURROW_OUT(err, jx_transform_syntactic(e));
        return none;
    }
    d->had_peeked = false;
    d->had_eof = false;
    JsontextKind k = jsontext_token_kind(tok);
    if (k == JSONTEXT_KIND_NULL)
        return none;
    if (k == JSONTEXT_KIND_FALSE || k == JSONTEXT_KIND_TRUE) {
        JsonToken b = {
            TYPE_BOOL,
            (void *)(uintptr_t)(k == JSONTEXT_KIND_TRUE ? &jx_true : &jx_false)};
        return b;
    }
    if (k == JSONTEXT_KIND_STRING) {
        Str s = jsontext_token_string(tok, d->a);
        JsonToken r = jx_token_copy(d, TYPE_OF(Str), &s, sizeof(s), _Alignof(Str), &e);
        BURROW_OUT(err, e);
        return r;
    }
    if (k == JSONTEXT_KIND_NUMBER) {
        if (jsonflags_get(&d->opts, JSONFLAG_UNMARSHAL_ANY_WITH_RAW_NUMBER)) {
            JsonNumber n = jsontext_token_string(tok, d->a);
            JsonToken r = jx_token_copy(d, TYPE_JSON_NUMBER, &n, sizeof(n),
                                        _Alignof(JsonNumber), &e);
            BURROW_OUT(err, e);
            return r;
        }
        Error fe = BURROW_NO_ERROR;
        double f = jsontext_token_float(tok, &fe);
        if (BURROW_FAILED(fe)) {
            JsonBuf value = jx_heap_buf();
            jsonbuf_str(&value, JV_LIT("number "));
            Str text = jsontext_token_string(tok, heap_allocator());
            jsonbuf_str(&value, text);
            mem_free(heap_allocator(), (void *)(uintptr_t)text.p, (size_t)text.len, 1);
            JsonUnmarshalTypeError ute;
            memset(&ute, 0, sizeof(ute));
            ute.value = str_from_bytes(value.p, value.len);
            ute.type = TYPE_FLOAT64;
            ute.offset = json_decoder_input_offset(d);
            e = value.failed ? burrow_err_out_of_memory
                             : jx_new(&jx_unmarshal_type_kind, &ute);
            burrow__jsonbuf_free(&value);
            BURROW_OUT(err, e);
            return none;
        }
        JsonToken r =
            jx_token_copy(d, TYPE_FLOAT64, &f, sizeof(f), _Alignof(double), &e);
        BURROW_OUT(err, e);
        return r;
    }
    for (size_t i = 0; i < sizeof jx_delims / sizeof jx_delims[0]; i++) {
        if (jx_delims[i] == (JsonDelim)k) {
            JsonToken r = {TYPE_JSON_DELIM, (void *)(uintptr_t)&jx_delims[i]};
            return r;
        }
    }
    panic_str(JV_LIT("json: unreachable"));
}

bool json_decoder_more(JsonDecoder *d) {
    d->had_peeked = true;
    JsontextKind k = jsontext_decoder_peek_kind(d->dec);
    if (k == JSONTEXT_KIND_INVALID) {
        if (BURROW_OK(d->err)) {
            /* PeekKind does not say whether it hit the end or an error, so
             * read the next token to find out. */
            Error e = BURROW_NO_ERROR;
            (void)jsontext_decoder_read_token(d->dec, &e);
            if (BURROW_OK(e))
                e = errors_new(error_allocator(),
                               JV_LIT("json: successful read after failed peek"));
            (void)jx_decoder_fail(d, jx_transform_syntactic(e));
        }
        return !jx_same(d->err, io_eof);
    }
    return k != JSONTEXT_KIND_END_ARRAY && k != JSONTEXT_KIND_END_OBJECT;
}

int64_t json_decoder_input_offset(const JsonDecoder *d) {
    int64_t offset = jsontext_decoder_input_offset(d->dec);
    if (d->had_peeked || d->had_eof) {
        /* v1 reported the end of the last token returned, unless More was
         * called, in which case it reported the start of the next one. */
        Slice unread = jsontext_decoder_unread_buffer(d->dec);
        const Byte *p = (const Byte *)unread.p;
        Int ws = 0;
        while (ws < unread.len &&
               (p[ws] == ' ' || p[ws] == '\n' || p[ws] == '\r' || p[ws] == '\t'))
            ws++;
        if (ws < unread.len) {
            offset += ws;
            /* After Token hit the end, also the comma or colon that follows. */
            if (d->had_eof && (p[ws] == ',' || p[ws] == ':'))
                offset++;
        }
    }
    return offset;
}

/* ---------------------------------------------------------------- Encoder */

struct JsonEncoder {
    IoWriter w;
    JsontextOptions opts;
    Error err;
    Str prefix;
    Str indent;
    Alloc *a;
};

JsonEncoder *json_new_encoder(Alloc *a, IoWriter w) {
    JsonEncoder *e =
        (JsonEncoder *)mem_alloc(a, sizeof(JsonEncoder), _Alignof(JsonEncoder));
    if (e == NULL)
        return NULL;
    e->a = a;
    e->w = w;
    e->opts = json_default_options_v1();
    return e;
}

static void jx_free_str(Alloc *a, Str s) {
    if (s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

void json_encoder_free(JsonEncoder *e) {
    if (e == NULL)
        return;
    jx_free_str(e->a, e->prefix);
    jx_free_str(e->a, e->indent);
    mem_free(e->a, e, sizeof(JsonEncoder), _Alignof(JsonEncoder));
}

Error json_encoder_encode(JsonEncoder *e, Any v) {
    if (BURROW_FAILED(e->err))
        return e->err;
    Alloc *h = heap_allocator();
    Error err = BURROW_NO_ERROR;
    Slice b = jsonv2_marshal_v(h, v, &err, 1, e->opts);
    if (BURROW_FAILED(err)) {
        mem_free(h, b.p, (size_t)b.cap, 1);
        return err;
    }
    if (e->prefix.len + e->indent.len > 0) {
        Slice out =
            jx_append_indent(h, slice_nil(TYPE_BYTE), str_from_bytes(b.p, b.len),
                             e->prefix, e->indent, &err);
        mem_free(h, b.p, (size_t)b.cap, 1);
        b = out;
        if (BURROW_FAILED(err)) {
            mem_free(h, b.p, (size_t)b.cap, 1);
            return err;
        }
    }
    static const Byte nl = '\n';
    Slice tail = {(void *)(uintptr_t)&nl, 1, 1, TYPE_BYTE};
    Slice line = slice_append_slice(h, b, tail);
    /* The append copies b when it has no room left, and leaves b to us. */
    if (line.p != b.p)
        mem_free(h, b.p, (size_t)b.cap, 1);
    if (line.p == NULL)
        return burrow_err_out_of_memory;
    (void)io_write_string(e->w, str_from_bytes(line.p, line.len), &err);
    mem_free(h, line.p, (size_t)line.cap, 1);
    if (BURROW_FAILED(err)) {
        e->err = error_retain(e->a, err);
        return e->err;
    }
    return BURROW_NO_ERROR;
}

void json_encoder_set_indent(JsonEncoder *e, Str prefix, Str indent) {
    /* Not jsontext's indent options, since v1's Indent has old bugs that it
     * has to keep. */
    jx_free_str(e->a, e->prefix);
    jx_free_str(e->a, e->indent);
    e->prefix = str_clone(e->a, prefix);
    e->indent = str_clone(e->a, indent);
}

void json_encoder_set_escape_html(JsonEncoder *e, bool on) {
    e->opts = jsonv2_join_options_v(2, e->opts, jsontext_escape_for_html(on));
}
