/* encoding/json/v2: the options, the errors, and Marshal and Unmarshal.
 *
 * Derived from Go's src/encoding/json/v2/arshal.go, errors.go and options.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"

#include "jsonv2_internal.h"

#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ options */

static JsontextOptions jv_bool_option(uint64_t flag, bool v) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = flag;
    o.values = v ? flag : 0;
    return o;
}

JsontextOptions jsonv2_stringify_numbers(bool v) {
    return jv_bool_option(JSONFLAG_STRINGIFY_NUMBERS, v);
}

JsontextOptions jsonv2_deterministic(bool v) {
    return jv_bool_option(JSONFLAG_DETERMINISTIC, v);
}

JsontextOptions jsonv2_format_nil_slice_as_null(bool v) {
    return jv_bool_option(JSONFLAG_FORMAT_NIL_SLICE_AS_NULL, v);
}

JsontextOptions jsonv2_format_nil_map_as_null(bool v) {
    return jv_bool_option(JSONFLAG_FORMAT_NIL_MAP_AS_NULL, v);
}

JsontextOptions jsonv2_omit_zero_struct_fields(bool v) {
    return jv_bool_option(JSONFLAG_OMIT_ZERO_STRUCT_FIELDS, v);
}

JsontextOptions jsonv2_match_case_insensitive_names(bool v) {
    return jv_bool_option(JSONFLAG_MATCH_CASE_INSENSITIVE_NAMES, v);
}

JsontextOptions jsonv2_reject_unknown_members(bool v) {
    return jv_bool_option(JSONFLAG_REJECT_UNKNOWN_MEMBERS, v);
}

JsontextOptions jsonv2_join_options(Slice srcs) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    burrow__jsonopts_join_slice(&o, srcs);
    return o;
}

static JsontextOptions jv_join_va(int n, va_list ap) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    for (int i = 0; i < n; i++) {
        JsontextOptions x = va_arg(ap, JsontextOptions);
        burrow__jsonopts_join(&o, &x);
    }
    return o;
}

JsontextOptions jsonv2_join_options_v(int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    return o;
}

JsontextOptions jsonv2_default_options_v2(void) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = JSONFLAG_DEFAULT_V1;
    return o;
}

/* The one flag a setter's result says something about. */
static uint64_t jv_setter_flag(JsontextOptions probe) {
    return probe.presence & ~(uint64_t)1;
}

bool jsonv2_get_option(JsontextOptions opts, JsontextOptions (*setter)(bool v),
                       bool *value) {
    uint64_t flag = jv_setter_flag(setter(false));
    bool v = jsonflags_get(&opts, flag);
    bool ok = jsonflags_has(&opts, flag);
    if (!ok && flag == JSONFLAG_STRINGIFY_NUMBERS &&
        jsonflags_get(&opts, JSONFLAG_STRING_TAG)) {
        v = true;
        ok = true;
    }
    if (value != NULL)
        *value = v;
    return ok;
}

bool jsonv2_get_option_str(JsontextOptions opts, JsontextOptions (*setter)(Str v),
                           Str *value) {
    uint64_t flag = jv_setter_flag(setter(BURROW_STR_EMPTY));
    Str v = BURROW_STR_EMPTY;
    bool ok = false;
    if (flag == JSONFLAG_INDENT && jsonflags_has(&opts, JSONFLAG_INDENT)) {
        v = opts.indent;
        ok = true;
    } else if (flag == JSONFLAG_INDENT_PREFIX &&
               jsonflags_has(&opts, JSONFLAG_INDENT_PREFIX)) {
        v = opts.indent_prefix;
        ok = true;
    }
    if (value != NULL)
        *value = v;
    return ok;
}

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(jsonv2_err_unknown_name, "unknown object member name");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_ambiguous_name,
                      "ambiguous object member name");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_cycle, "encountered a cycle");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_non_nil_reference,
                      "value must be passed as a non-nil pointer reference");
BURROW_SENTINEL_ERROR(
    burrow__jsonv2_err_nil_interface,
    "cannot derive concrete type for nil interface with finite type set");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_invalid_string_tag,
                      "invalid use of `string` tag option");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_nil_field,
                      "cannot set embedded pointer to unexported struct type");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_array_underflow, "too few array elements");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_array_overflow, "too many array elements");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_no_exported_fields,
                      "Go struct has no exported fields");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_changing_duplicate_names,
                      "cannot change duplicate name checks after a JSON object has "
                      "already begun processing");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_changing_invalid_utf8,
                      "cannot change UTF-8 checks after a JSON object has already "
                      "begun processing");
BURROW_SENTINEL_ERROR(
    burrow__jsonv2_err_changing_whitespace,
    "cannot change whitespace formatting within a MarshalEncode call");

BURROW_SENTINEL_ERROR(burrow__jsonv2_err_non_singular_value,
                      "must read or write exactly one value");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_unsupported_mutation,
                      "unsupported calls must not read or write any tokens");
BURROW_SENTINEL_ERROR(burrow__jsonv2_err_non_string_value,
                      "JSON value must be string type");

static bool jv_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

Error burrow__jsonv2_errorf(Str msg) {
    Error err = errors_new(error_allocator(), msg);
    return BURROW_FAILED(err) ? err : burrow_err_out_of_memory;
}

Error burrow__jsonv2_to_unexpected_eof(Error err) {
    return jv_same(err, io_eof) ? io_err_unexpected_eof : err;
}

static bool jv_is_syntactic(Error err) {
    return err.vt != NULL && err.vt->self_type == TYPE_JSONTEXT_SYNTACTIC_ERROR;
}

bool burrow__jsonv2_is_syntactic(Error err) {
    return jv_is_syntactic(err);
}

bool burrow__jsonv2_is_fatal(Error err, const JsontextOptions *o) {
    return !jsonflags_get(o, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS) ||
           jv_is_syntactic(err) || burrow__jsontext_is_io_error(err);
}

static void jv_put_int(JsonBuf *b, int64_t v) {
    Byte tmp[24];
    Int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    if (v < 0)
        jsonbuf_byte(b, '-');
    while (n > 0)
        jsonbuf_byte(b, tmp[--n]);
}

/* reflect.Type.String. A named type is its package name, which is the last
 * element of its path, a dot and its name, and the rest are spelled out. */
void burrow__jsonv2_put_type(JsonBuf *b, const Type *t) {
    if (t == NULL) {
        jsonbuf_str(b, JV_LIT("<nil>"));
        return;
    }
    if (t->name.len > 0) {
        if (t->pkg_path.len > 0) {
            Int i = t->pkg_path.len;
            while (i > 0 && t->pkg_path.p[i - 1] != '/')
                i--;
            jsonbuf_put(b, t->pkg_path.p + i, t->pkg_path.len - i);
            jsonbuf_byte(b, '.');
        }
        jsonbuf_str(b, t->name);
        return;
    }
    switch ((int)t->kind) {
    case KIND_SLICE:
        jsonbuf_str(b, JV_LIT("[]"));
        burrow__jsonv2_put_type(b, t->elem);
        return;
    case KIND_ARRAY:
        jsonbuf_byte(b, '[');
        jv_put_int(b, (int64_t)t->len);
        jsonbuf_byte(b, ']');
        burrow__jsonv2_put_type(b, t->elem);
        return;
    case KIND_POINTER:
        jsonbuf_byte(b, '*');
        burrow__jsonv2_put_type(b, t->elem);
        return;
    case KIND_MAP:
        jsonbuf_str(b, JV_LIT("map["));
        burrow__jsonv2_put_type(b, t->key);
        jsonbuf_byte(b, ']');
        burrow__jsonv2_put_type(b, t->elem);
        return;
    case KIND_CHAN:
        jsonbuf_str(b, JV_LIT("chan "));
        burrow__jsonv2_put_type(b, t->elem);
        return;
    case KIND_FUNC:
        jsonbuf_str(b, JV_LIT("func()"));
        return;
    case KIND_STRUCT:
        if (t->nfield == 0) {
            jsonbuf_str(b, JV_LIT("struct {}"));
            return;
        }
        jsonbuf_str(b, JV_LIT("struct {"));
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0)
                jsonbuf_byte(b, ';');
            jsonbuf_byte(b, ' ');
            if (!field_is_embedded(f)) {
                jsonbuf_str(b, f->name);
                jsonbuf_byte(b, ' ');
            }
            burrow__jsonv2_put_type(b, f->type);
            if (f->tag.len > 0) {
                jsonbuf_byte(b, ' ');
                burrow__jsontext_put_go_quote(b, f->tag);
            }
        }
        jsonbuf_str(b, JV_LIT(" }"));
        return;
    case KIND_INTERFACE:
        jsonbuf_str(b, JV_LIT("interface {}"));
        return;
    default:
        jsonbuf_str(b, kind_name(t->kind));
        return;
    }
}

static JsonBuf jv_heap_buf(void) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    return b;
}

/* The last reference token of a JSON Pointer with ~1 and ~0 undone, and the
 * pointer without it. */
static void jv_put_last_token(JsonBuf *b, Str p) {
    Int i = p.len - 1;
    while (i >= 0 && p.p[i] != '/')
        i--;
    Str tok = str_from_bytes(p.p + i + 1, p.len - i - 1);
    Int start = b->len;
    jsonbuf_str(b, tok);
    if (b->failed)
        return;
    for (int pass = 0; pass < 2; pass++) {
        Byte from = pass == 0 ? '1' : '0';
        Byte to = pass == 0 ? '/' : '~';
        Int w = start;
        for (Int r = start; r < b->len; r++) {
            if (b->p[r] == '~' && r + 1 < b->len && b->p[r + 1] == from) {
                b->p[w++] = to;
                r++;
            } else {
                b->p[w++] = b->p[r];
            }
        }
        b->len = w;
    }
}

static Str jv_pointer_parent(Str p) {
    Int i = p.len - 1;
    while (i >= 0 && p.p[i] != '/')
        i--;
    return str_from_bytes(p.p, i < 0 ? 0 : i);
}

/* Quoted the way strconv.Quote does it, after TruncatePointer. */
static void jv_put_quoted_pointer(JsonBuf *b, Str p) {
    JsonBuf t = jv_heap_buf();
    burrow__jsonwire_truncate_pointer(&t, p, 100);
    burrow__jsontext_put_go_quote(b, str_from_bytes(t.p, t.len));
    if (t.failed)
        b->failed = true;
    burrow__jsonbuf_free(&t);
}

/* SemanticError.Error into b. Go picks between "cannot" and "unable to" at
 * random, once per process, so that nobody matches on the text. This always
 * says "cannot". */
static void jv_semantic_text(JsonBuf *b, const Jsonv2SemanticError *e) {
    Str preposition;
    jsonbuf_str(b, JV_LIT("json: cannot"));
    if (str_eq(e->action, JV_LIT("marshal"))) {
        jsonbuf_str(b, JV_LIT(" marshal"));
        preposition = JV_LIT(" from");
    } else if (str_eq(e->action, JV_LIT("unmarshal"))) {
        jsonbuf_str(b, JV_LIT(" unmarshal"));
        preposition = JV_LIT(" into");
    } else {
        jsonbuf_str(b, JV_LIT(" handle"));
        preposition = JV_LIT(" with");
    }
    switch (e->json_kind) {
    case 'n':
        jsonbuf_str(b, JV_LIT(" JSON null"));
        break;
    case 'f':
    case 't':
        jsonbuf_str(b, JV_LIT(" JSON boolean"));
        break;
    case '"':
        jsonbuf_str(b, JV_LIT(" JSON string"));
        break;
    case '0':
        jsonbuf_str(b, JV_LIT(" JSON number"));
        break;
    case '{':
    case '}':
        jsonbuf_str(b, JV_LIT(" JSON object"));
        break;
    case '[':
    case ']':
        jsonbuf_str(b, JV_LIT(" JSON array"));
        break;
    default:
        if (e->action.len == 0)
            preposition = BURROW_STR_EMPTY;
        break;
    }
    if (e->json_value.len > 0 && e->json_value.len < 100) {
        jsonbuf_byte(b, ' ');
        jsonbuf_put(b, e->json_value.p, e->json_value.len);
    }
    if (e->go_type != NULL) {
        JsonBuf ts = jv_heap_buf();
        burrow__jsonv2_put_type(&ts, e->go_type);
        if (ts.failed)
            b->failed = true;
        jsonbuf_str(b, preposition);
        jsonbuf_str(b, JV_LIT(" Go "));
        if (ts.len > 100)
            jsonbuf_str(b, kind_name(e->go_type->kind));
        else
            jsonbuf_put(b, ts.p, ts.len);
        burrow__jsonbuf_free(&ts);
    }
    if (jv_same(e->err, jsonv2_err_unknown_name) ||
        jv_same(e->err, burrow__jsonv2_err_ambiguous_name)) {
        jsonbuf_str(b, JV_LIT(": "));
        jsonbuf_str(b, error_text(e->err));
        jsonbuf_byte(b, ' ');
        JsonBuf tok = jv_heap_buf();
        jv_put_last_token(&tok, e->json_pointer);
        burrow__jsontext_put_go_quote(b, str_from_bytes(tok.p, tok.len));
        if (tok.failed)
            b->failed = true;
        burrow__jsonbuf_free(&tok);
        Str parent = jv_pointer_parent(e->json_pointer);
        if (parent.len > 0) {
            jsonbuf_str(b, JV_LIT(" within "));
            jv_put_quoted_pointer(b, parent);
        }
        return;
    }
    const JsontextSyntacticError *serr = NULL;
    if (jv_is_syntactic(e->err))
        serr = (const JsontextSyntacticError *)e->err.data;
    if (e->json_pointer.len > 0) {
        if (serr == NULL ||
            !jsontext_pointer_contains(e->json_pointer, serr->json_pointer)) {
            jsonbuf_str(b, JV_LIT(" within "));
            jv_put_quoted_pointer(b, e->json_pointer);
        }
    } else if (e->byte_offset > 0) {
        if (serr == NULL || !(e->byte_offset <= serr->byte_offset)) {
            jsonbuf_str(b, JV_LIT(" after offset "));
            jv_put_int(b, e->byte_offset);
        }
    }
    if (BURROW_FAILED(e->err)) {
        Str s = error_text(e->err);
        if (serr != NULL && s.len >= 10 && memcmp(s.p, "jsontext: ", 10) == 0)
            s = str_from_bytes(s.p + 10, s.len - 10);
        jsonbuf_str(b, JV_LIT(": "));
        jsonbuf_str(b, s);
    }
}

typedef struct JvSemanticBox {
    Jsonv2SemanticError e;
    Str message;
} JvSemanticBox;

static Str jv_semantic_message(const void *self) {
    return ((const JvSemanticBox *)self)->message;
}

static Error jv_semantic_unwrap(const void *self) {
    return ((const Jsonv2SemanticError *)self)->err;
}

static const Type jv_semantic_desc = {
    {(const Byte *)"SemanticError", 13},
    {(const Byte *)"encoding/json/v2", 16},
    KIND_STRUCT,
    (uint32_t)sizeof(Jsonv2SemanticError),
    (uint16_t)_Alignof(Jsonv2SemanticError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a767365U, /* "jvse" */
    NULL,
};

const Type *const TYPE_JSONV2_SEMANTIC_ERROR = &jv_semantic_desc;

static Error jv_semantic_clone(const void *self, Alloc *a);

static const ErrorVT jv_semantic_vt = {
    &jv_semantic_desc, jv_semantic_message, jv_semantic_unwrap, NULL, NULL, NULL,
    jv_semantic_clone,
};

/* One allocation holding the box, the pointer, the value and the message.
 * The zero Error when a refuses. */
static Error jv_semantic_build(Alloc *a, const Jsonv2SemanticError *e) {
    JsonBuf msg = jv_heap_buf();
    jv_semantic_text(&msg, e);
    if (msg.failed) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    Int plen = e->json_pointer.len;
    Int vlen = e->json_value.len;
    JvSemanticBox *box = (JvSemanticBox *)mem_alloc_nozero(
        a, sizeof(JvSemanticBox) + (size_t)plen + (size_t)vlen + (size_t)msg.len,
        _Alignof(JvSemanticBox));
    if (box == NULL) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    Byte *p = (Byte *)(box + 1);
    if (plen > 0)
        memcpy(p, e->json_pointer.p, (size_t)plen);
    if (vlen > 0)
        memcpy(p + plen, e->json_value.p, (size_t)vlen);
    if (msg.len > 0)
        memcpy(p + plen + vlen, msg.p, (size_t)msg.len);
    box->e = *e;
    box->e.json_pointer = str_from_bytes(p, plen);
    box->e.json_value.p = vlen > 0 ? p + plen : NULL;
    box->e.json_value.len = vlen;
    box->e.json_value.cap = vlen;
    box->e.json_value.elem = TYPE_BYTE;
    box->message = str_from_bytes(p + plen + vlen, msg.len);
    burrow__jsonbuf_free(&msg);
    return (Error){&jv_semantic_vt, box};
}

Str jsonv2_semantic_error_error(const Jsonv2SemanticError *e, Alloc *a) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jv_semantic_text(&b, e);
    if (b.failed) {
        burrow__jsonbuf_free(&b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(b.p, b.len);
}

Error jsonv2_semantic_error_unwrap(const Jsonv2SemanticError *e) {
    return e->err;
}

Error jsonv2_semantic_error_as_error(const Jsonv2SemanticError *e, Alloc *a) {
    Error made = jv_semantic_build(a, e);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

static Error jv_semantic_clone(const void *self, Alloc *a) {
    Jsonv2SemanticError copy = *(const Jsonv2SemanticError *)self;
    copy.err = error_retain(a, copy.err);
    return jsonv2_semantic_error_as_error(&copy, a);
}

Error burrow__jsonv2_semantic_new(const Jsonv2SemanticError *e) {
    Error made = jv_semantic_build(error_allocator(), e);
    if (BURROW_FAILED(made))
        return made;
    return BURROW_FAILED(e->err) ? e->err : burrow_err_out_of_memory;
}

const Jsonv2SemanticError *burrow__jsonv2_as_semantic(Error err) {
    if (err.vt != &jv_semantic_vt)
        return NULL;
    return (const Jsonv2SemanticError *)err.data;
}

/* The JSON Pointer the coder is at, in a heap buffer. */
static JsonBuf jv_enc_pointer(JsontextEncoder *e, int where) {
    JsonBuf b = jv_heap_buf();
    burrow__jsontext_encoder_append_stack_pointer(e, &b, where);
    return b;
}

static JsonBuf jv_dec_pointer(JsontextDecoder *d, int where) {
    JsonBuf b = jv_heap_buf();
    burrow__jsontext_decoder_append_stack_pointer(d, &b, where);
    return b;
}

static Jsonv2SemanticError jv_semantic_zero(void) {
    Jsonv2SemanticError s;
    memset(&s, 0, sizeof(s));
    return s;
}

Error burrow__jsonv2_marshal_error_before(JsontextEncoder *e, const Type *t,
                                          Error err) {
    Jsonv2SemanticError s = jv_semantic_zero();
    JsonBuf ptr = jv_enc_pointer(e, +1);
    s.action = JV_LIT("marshal");
    s.go_type = t;
    s.err = burrow__jsonv2_to_unexpected_eof(err);
    s.byte_offset = jsontext_encoder_output_offset(e) +
                    (int64_t)burrow__jsontext_encoder_count_next_delim_whitespace(e);
    s.json_pointer = str_from_bytes(ptr.p, ptr.len);
    Error r = burrow__jsonv2_semantic_new(&s);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_unmarshal_error_before(JsontextDecoder *d, const Type *t,
                                            Error err) {
    Jsonv2SemanticError s = jv_semantic_zero();
    if (jsonflags_get(&d->opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        s.json_kind = jsontext_decoder_peek_kind(d);
    JsonBuf ptr = jv_dec_pointer(d, +1);
    s.action = JV_LIT("unmarshal");
    s.go_type = t;
    s.err = burrow__jsonv2_to_unexpected_eof(err);
    s.byte_offset = jsontext_decoder_input_offset(d) +
                    (int64_t)burrow__jsontext_decoder_count_next_delim_whitespace(d);
    s.json_pointer = str_from_bytes(ptr.p, ptr.len);
    Error r = burrow__jsonv2_semantic_new(&s);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_unmarshal_error_before_skipping(JsontextDecoder *d, const Type *t,
                                                     Error err) {
    err = burrow__jsonv2_unmarshal_error_before(d, t, err);
    if (jsonflags_get(&d->opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        Error err2 = jsontext_decoder_skip_value(d);
        if (BURROW_FAILED(err2))
            return err2;
    }
    return err;
}

static Error jv_unmarshal_error_after(JsontextDecoder *d, const Type *t, Error err,
                                      bool with_value) {
    Slice tok = burrow__jsontext_previous_token_or_value(d);
    int64_t offset = jsontext_decoder_input_offset(d) - (int64_t)tok.len;
    JsontextKind k = jsontext_value_kind(tok);
    if (jsonflags_get(&d->opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        if (k == '[' || k == '{')
            offset++;
        else
            offset += (int64_t)tok.len;
    }
    Jsonv2SemanticError s = jv_semantic_zero();
    JsonBuf ptr = jv_dec_pointer(d, -1);
    s.action = JV_LIT("unmarshal");
    s.go_type = t;
    s.err = burrow__jsonv2_to_unexpected_eof(err);
    s.byte_offset = offset;
    s.json_pointer = str_from_bytes(ptr.p, ptr.len);
    s.json_kind = k;
    if (with_value && (k == '"' || k == '0'))
        s.json_value = tok;
    Error r = burrow__jsonv2_semantic_new(&s);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_unmarshal_error_after(JsontextDecoder *d, const Type *t,
                                           Error err) {
    return jv_unmarshal_error_after(d, t, err, false);
}

Error burrow__jsonv2_unmarshal_error_after_value(JsontextDecoder *d, const Type *t,
                                                 Error err) {
    return jv_unmarshal_error_after(d, t, err, true);
}

Error burrow__jsonv2_unmarshal_error_after_skipping(JsontextDecoder *d, const Type *t,
                                                    Error err) {
    err = jv_unmarshal_error_after(d, t, err, false);
    if (jsonflags_get(&d->opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        Error err2 = burrow__jsontext_skip_value_remainder(d);
        if (BURROW_FAILED(err2))
            return err2;
    }
    return err;
}

bool burrow__jsonv2_is_semantic(Error err) {
    return err.vt == &jv_semantic_vt;
}

Error burrow__jsonv2_wrap_unsupported(Error err, const char *what) {
    if (!errors_is(err, errors_err_unsupported))
        return err;
    JsonBuf b = jv_heap_buf();
    jsonbuf_str(&b, str_from_bytes(what, (Int)strlen(what)));
    jsonbuf_str(&b, JV_LIT(" may not return errors.ErrUnsupported"));
    Error r = b.failed ? burrow_err_out_of_memory
                       : burrow__jsonv2_errorf(str_from_bytes(b.p, b.len));
    burrow__jsonbuf_free(&b);
    return r;
}

/* Where newSemanticErrorWithPosition points when the error does not say:
 * at the member just written or read when the call wrote or read exactly
 * one, before the next one when it touched nothing, and at the parent
 * otherwise, since there is no telling. */
static int jv_position_where(Int prev_depth, int64_t prev_len, Int depth, int64_t len) {
    if (prev_depth == depth && prev_len == len)
        return +1;
    if (prev_depth == depth && prev_len + 1 == len)
        return -1;
    return 0;
}

/* The shared half of newSemanticErrorWithPosition. Go fills in the fields of
 * the SemanticError it was handed that are still zero. Errors here do not
 * change once made, so this makes a new one with the fields filled in. */
static Error jv_with_position(Error err, const Type *t, Str action, int64_t offset,
                              const JsonBuf *ptr) {
    const Jsonv2SemanticError *in = burrow__jsonv2_as_semantic(err);
    Jsonv2SemanticError s = jv_semantic_zero();
    if (in != NULL)
        s = *in;
    else
        s.err = err;
    s.err = burrow__jsonv2_to_unexpected_eof(s.err);
    if (s.action.len == 0)
        s.action = action;
    if (s.byte_offset == 0)
        s.byte_offset = offset;
    if (s.json_pointer.len == 0)
        s.json_pointer = str_from_bytes(ptr->p, ptr->len);
    if (s.go_type == NULL)
        s.go_type = t;
    return burrow__jsonv2_semantic_new(&s);
}

Error burrow__jsonv2_error_with_position_enc(JsontextEncoder *e, const Type *t,
                                             Int prev_depth, int64_t prev_len,
                                             Error err) {
    Int depth = jt_depth(&e->st);
    int64_t len = jt_e_len(e->st.last);
    int64_t offset = jsontext_encoder_output_offset(e) +
                     (int64_t)burrow__jsontext_encoder_count_next_delim_whitespace(e);
    JsonBuf ptr =
        jv_enc_pointer(e, jv_position_where(prev_depth, prev_len, depth, len));
    Error r = jv_with_position(err, t, JV_LIT("marshal"), offset, &ptr);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_error_with_position_dec(JsontextDecoder *d, const Type *t,
                                             Int prev_depth, int64_t prev_len,
                                             Error err) {
    Int depth = jt_depth(&d->st);
    int64_t len = jt_e_len(d->st.last);
    Slice tok = burrow__jsontext_previous_token_or_value(d);
    int64_t offset = jsontext_decoder_input_offset(d) - (int64_t)tok.len;
    if ((prev_depth == depth && prev_len == len) || tok.len == 0)
        offset = jsontext_decoder_input_offset(d) +
                 (int64_t)burrow__jsontext_decoder_count_next_delim_whitespace(d);
    JsonBuf ptr =
        jv_dec_pointer(d, jv_position_where(prev_depth, prev_len, depth, len));
    Error r = jv_with_position(err, t, JV_LIT("unmarshal"), offset, &ptr);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_collapse_semantic(Error err) {
    const Jsonv2SemanticError *outer = burrow__jsonv2_as_semantic(err);
    if (outer == NULL)
        return err;
    const Jsonv2SemanticError *inner = burrow__jsonv2_as_semantic(outer->err);
    if (inner == NULL)
        return err;
    Jsonv2SemanticError s = *inner;
    s.byte_offset = outer->byte_offset + inner->byte_offset;
    JsonBuf ptr = jv_heap_buf();
    jsonbuf_str(&ptr, outer->json_pointer);
    jsonbuf_str(&ptr, inner->json_pointer);
    if (ptr.failed) {
        burrow__jsonbuf_free(&ptr);
        return burrow_err_out_of_memory;
    }
    s.json_pointer = str_from_bytes(ptr.p, ptr.len);
    Error r = burrow__jsonv2_semantic_new(&s);
    burrow__jsonbuf_free(&ptr);
    return r;
}

/* fmt.Errorf("invalid format flag %q", format). */
static Error jv_invalid_format_flag(const JsontextOptions *o) {
    JsonBuf b = jv_heap_buf();
    jsonbuf_str(&b, JV_LIT("invalid format flag "));
    burrow__jsontext_put_go_quote(&b, o->format);
    Error err = b.failed ? burrow_err_out_of_memory
                         : burrow__jsonv2_errorf(str_from_bytes(b.p, b.len));
    burrow__jsonbuf_free(&b);
    return err;
}

Error burrow__jsonv2_invalid_format_enc(JsontextEncoder *e, const Type *t,
                                        const JsontextOptions *o) {
    return burrow__jsonv2_marshal_error_before(e, t, jv_invalid_format_flag(o));
}

Error burrow__jsonv2_invalid_format_dec(JsontextDecoder *d, const Type *t,
                                        const JsontextOptions *o) {
    return burrow__jsonv2_unmarshal_error_before_skipping(d, t,
                                                          jv_invalid_format_flag(o));
}

/* newDuplicateNameError(dec.StackPointer(), nil, offset). */
Error burrow__jsonv2_duplicate_name_error(JsontextDecoder *d, int64_t offset) {
    JsonBuf ptr = jv_dec_pointer(d, 0);
    Error r = burrow__jsontext_syntactic_new(offset, str_from_bytes(ptr.p, ptr.len),
                                             jsontext_err_duplicate_name);
    burrow__jsonbuf_free(&ptr);
    return r;
}

Error burrow__jsonv2_duplicate_name_error_enc(JsontextEncoder *e, Slice quoted) {
    JsonBuf ptr = jv_enc_pointer(e, 0);
    JsonBuf name = jv_heap_buf();
    (void)burrow__jsonwire_append_unquote(&name, (const Byte *)quoted.p, quoted.len);
    Error r = burrow_err_out_of_memory;
    if (!ptr.failed && !name.failed) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        JsontextPointer parent =
            jsontext_pointer_parent(str_from_bytes(ptr.p, ptr.len));
        JsontextPointer full = jsontext_pointer_append_token(
            parent, arena_allocator(&ar), str_from_bytes(name.p, name.len));
        if (full.len > 0)
            r = burrow__jsontext_syntactic_new(jsontext_encoder_output_offset(e), full,
                                               jsontext_err_duplicate_name);
        arena_free(&ar);
    }
    burrow__jsonbuf_free(&name);
    burrow__jsonbuf_free(&ptr);
    return r;
}

/* ------------------------------------------------------------------ marshal */

static bool jv_any_is_nil(Any in) {
    if (in.t == NULL || in.data == NULL)
        return true;
    return in.t->kind == KIND_POINTER && *(void *const *)in.data == NULL;
}

/* marshalEncode. A pointer is followed once, so that marshalling a value and a
 * pointer to it give the same thing, as they do in Go. */
static Error jv_marshal_encode(JsontextEncoder *e, Any in, JsontextOptions *mo) {
    if (jv_any_is_nil(in))
        return jsontext_encoder_write_token(e, jsontext_null);
    const Type *t = in.t;
    void *p = in.data;
    if (t->kind == KIND_POINTER) {
        p = *(void **)p;
        t = t->elem;
    }
    Error err = burrow__jsonv2_marshal_value(e, t, p, mo);
    if (BURROW_FAILED(err)) {
        if (!jsonflags_get(mo, JSONFLAG_ALLOW_DUPLICATE_NAMES))
            jsonstate_invalidate_disabled_namespaces(&e->st);
        return err;
    }
    return BURROW_NO_ERROR;
}

static Slice jv_marshal(Alloc *a, Any in, const JsontextOptions *o, Error *err) {
    Slice out = {NULL, 0, 0, TYPE_BYTE};
    JsontextEncoder e;
    IoWriter none;
    memset(&none, 0, sizeof(none));
    burrow__jsontext_encoder_init(&e, heap_allocator());
    burrow__jsontext_encoder_setup(&e, none, false, o);
    jsonflags_set(&e.opts, JSONFLAG_OMIT_TOP_LEVEL_NEWLINE | 1);
    Error r = jv_marshal_encode(&e, in, &e.opts);
    /* Go hands back what was written before the error along with it, and
     * nothing only when v1 is asking. */
    if (BURROW_OK(r) ||
        !jsonflags_get(&e.opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        Byte *p =
            (Byte *)mem_alloc_nozero(a, (size_t)(e.buf.len > 0 ? e.buf.len : 1), 1);
        if (p == NULL) {
            if (BURROW_OK(r))
                r = burrow_err_out_of_memory;
        } else {
            if (e.buf.len > 0)
                memcpy(p, e.buf.p, (size_t)e.buf.len);
            out.p = p;
            out.len = e.buf.len;
            out.cap = e.buf.len;
        }
    }
    burrow__jsontext_encoder_release(&e);
    if (err != NULL)
        *err = r;
    return out;
}

Slice jsonv2_marshal(Alloc *a, Any in, Slice opts, Error *err) {
    JsontextOptions o = jsonv2_join_options(opts);
    return jv_marshal(a, in, &o, err);
}

Slice jsonv2_marshal_v(Alloc *a, Any in, Error *err, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    return jv_marshal(a, in, &o, err);
}

static Error jv_marshal_write(Alloc *a, IoWriter out, Any in,
                              const JsontextOptions *o) {
    (void)a;
    JsontextEncoder e;
    burrow__jsontext_encoder_init(&e, heap_allocator());
    burrow__jsontext_encoder_setup(&e, out, true, o);
    jsonflags_set(&e.opts, JSONFLAG_OMIT_TOP_LEVEL_NEWLINE | 1);
    Error r = jv_marshal_encode(&e, in, &e.opts);
    if (BURROW_OK(r) && e.buf.len > 0)
        r = burrow__jsontext_flush(&e);
    burrow__jsontext_encoder_release(&e);
    return r;
}

Error jsonv2_marshal_write(Alloc *a, IoWriter out, Any in, Slice opts) {
    JsontextOptions o = jsonv2_join_options(opts);
    return jv_marshal_write(a, out, in, &o);
}

Error jsonv2_marshal_write_v(Alloc *a, IoWriter out, Any in, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    return jv_marshal_write(a, out, in, &o);
}

static bool jv_changed_whitespace(const JsontextOptions *s1,
                                  const JsontextOptions *s2) {
    return jsonflags_get(s1, JSONFLAG_MULTILINE) !=
               jsonflags_get(s2, JSONFLAG_MULTILINE) ||
           jsonflags_get(s1, JSONFLAG_SPACE_AFTER_COLON) !=
               jsonflags_get(s2, JSONFLAG_SPACE_AFTER_COLON) ||
           jsonflags_get(s1, JSONFLAG_SPACE_AFTER_COMMA) !=
               jsonflags_get(s2, JSONFLAG_SPACE_AFTER_COMMA) ||
           (jsonflags_get(s2, JSONFLAG_MULTILINE) &&
            (!str_eq(s1->indent, s2->indent) ||
             !str_eq(s1->indent_prefix, s2->indent_prefix)));
}

static Error jv_marshal_encode_opts(JsontextEncoder *out, Any in,
                                    const JsontextOptions *add, bool has) {
    if (!has)
        return jv_marshal_encode(out, in, &out->opts);
    JsontextOptions orig = out->opts;
    burrow__jsonopts_join(&out->opts, add);
    Error err = BURROW_NO_ERROR;
    if (jt_e_need_name(out->st.last)) {
        if (jsonflags_get(&orig, JSONFLAG_ALLOW_DUPLICATE_NAMES) !=
            jsonflags_get(&out->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
            err = burrow__jsonv2_marshal_error_before(
                out, in.t, burrow__jsonv2_err_changing_duplicate_names);
            goto done;
        }
        if (jsonflags_get(&orig, JSONFLAG_ALLOW_INVALID_UTF8) !=
            jsonflags_get(&out->opts, JSONFLAG_ALLOW_INVALID_UTF8)) {
            err = burrow__jsonv2_marshal_error_before(
                out, in.t, burrow__jsonv2_err_changing_invalid_utf8);
            goto done;
        }
    }
    if (jsonflags_has(&out->opts, JSONFLAG_ANY_WHITESPACE)) {
        if (jsonflags_get(&out->opts, JSONFLAG_MULTILINE))
            burrow__jsonopts_initialize_multiline(&out->opts);
        if (jv_changed_whitespace(&orig, &out->opts)) {
            err = burrow__jsonv2_marshal_error_before(
                out, in.t, burrow__jsonv2_err_changing_whitespace);
            goto done;
        }
    }
    err = jv_marshal_encode(out, in, &out->opts);
done:
    out->opts = orig;
    return err;
}

Error jsonv2_marshal_encode(JsontextEncoder *out, Any in, Slice opts) {
    JsontextOptions o = jsonv2_join_options(opts);
    return jv_marshal_encode_opts(out, in, &o, opts.len > 0);
}

Error jsonv2_marshal_encode_v(JsontextEncoder *out, Any in, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    return jv_marshal_encode_opts(out, in, &o, n > 0);
}

/* ---------------------------------------------------------------- unmarshal */

static Error jv_unexpected_eof_at_end(JsontextDecoder *d) {
    int64_t offset = jsontext_decoder_input_offset(d) +
                     (int64_t)jsontext_decoder_unread_buffer(d).len;
    return burrow__jsontext_syntactic_new(offset, BURROW_STR_EMPTY,
                                          io_err_unexpected_eof);
}

/* unmarshalDecode. */
static Error jv_unmarshal_decode(JsontextDecoder *in, Any out, JsontextOptions *uo,
                                 bool last) {
    if (out.t == NULL || out.data == NULL) {
        Jsonv2SemanticError s = jv_semantic_zero();
        s.action = JV_LIT("unmarshal");
        s.go_type = out.t;
        s.err = burrow__jsonv2_err_non_nil_reference;
        return burrow__jsonv2_semantic_new(&s);
    }
    if (jsonflags_get(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS)) {
        Error err = burrow__jsontext_check_next_value(in, last);
        if (BURROW_FAILED(err)) {
            if (jv_same(err, io_eof) && last)
                return jv_unexpected_eof_at_end(in);
            return err;
        }
    }
    Error err = burrow__jsonv2_unmarshal_value(in, out.t, out.data, uo);
    if (BURROW_FAILED(err)) {
        if (!jsonflags_get(uo, JSONFLAG_ALLOW_DUPLICATE_NAMES))
            jsonstate_invalidate_disabled_namespaces(&in->st);
        if (jv_same(err, io_eof) && last)
            return jv_unexpected_eof_at_end(in);
        return err;
    }
    if (last)
        return burrow__jsontext_check_eof(in);
    return BURROW_NO_ERROR;
}

static Error jv_unmarshal(Alloc *a, IoReader r, bool has_rd, Slice in, Any out,
                          const JsontextOptions *o) {
    JsontextDecoder d;
    burrow__jsontext_decoder_init(&d, heap_allocator());
    burrow__jsontext_decoder_setup(&d, r, has_rd, (const Byte *)in.p, in.len, o);
    d.out_alloc = a;
    Error err = jv_unmarshal_decode(&d, out, &d.opts, true);
    burrow__jsontext_decoder_release(&d);
    return err;
}

Error jsonv2_unmarshal(Alloc *a, Slice in, Any out, Slice opts) {
    JsontextOptions o = jsonv2_join_options(opts);
    IoReader none;
    memset(&none, 0, sizeof(none));
    return jv_unmarshal(a, none, false, in, out, &o);
}

Error jsonv2_unmarshal_v(Alloc *a, Slice in, Any out, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    IoReader none;
    memset(&none, 0, sizeof(none));
    return jv_unmarshal(a, none, false, in, out, &o);
}

Error jsonv2_unmarshal_read(Alloc *a, IoReader in, Any out, Slice opts) {
    JsontextOptions o = jsonv2_join_options(opts);
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    return jv_unmarshal(a, in, true, none, out, &o);
}

Error jsonv2_unmarshal_read_v(Alloc *a, IoReader in, Any out, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    return jv_unmarshal(a, in, true, none, out, &o);
}

static Error jv_unmarshal_decode_opts(Alloc *a, JsontextDecoder *in, Any out,
                                      const JsontextOptions *add, bool has) {
    JsontextOptions orig = in->opts;
    Alloc *orig_alloc = in->out_alloc;
    in->out_alloc = a;
    Error err = BURROW_NO_ERROR;
    if (has) {
        burrow__jsonopts_join(&in->opts, add);
        if (jt_e_need_name(in->st.last)) {
            if (jsonflags_get(&orig, JSONFLAG_ALLOW_DUPLICATE_NAMES) !=
                jsonflags_get(&in->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
                err = burrow__jsonv2_unmarshal_error_before(
                    in, out.t, burrow__jsonv2_err_changing_duplicate_names);
                goto done;
            }
            if (jsonflags_get(&orig, JSONFLAG_ALLOW_INVALID_UTF8) !=
                jsonflags_get(&in->opts, JSONFLAG_ALLOW_INVALID_UTF8)) {
                err = burrow__jsonv2_unmarshal_error_before(
                    in, out.t, burrow__jsonv2_err_changing_invalid_utf8);
                goto done;
            }
        }
    }
    err = jv_unmarshal_decode(in, out, &in->opts, false);
done:
    if (has)
        in->opts = orig;
    in->out_alloc = orig_alloc;
    return err;
}

Error jsonv2_unmarshal_decode(Alloc *a, JsontextDecoder *in, Any out, Slice opts) {
    JsontextOptions o = jsonv2_join_options(opts);
    return jv_unmarshal_decode_opts(a, in, out, &o, opts.len > 0);
}

Error jsonv2_unmarshal_decode_v(Alloc *a, JsontextDecoder *in, Any out, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jv_join_va(n, ap);
    va_end(ap);
    return jv_unmarshal_decode_opts(a, in, out, &o, n > 0);
}
