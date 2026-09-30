/* Derived from Go's src/encoding/json/jsontext and
 * src/encoding/json/internal/jsonopts/options.go.
 * Go source: go1.27.1.
 *
 * The encoder and decoder share a state machine that says what may come next,
 * a stack of the object names seen at each depth for building JSON Pointers,
 * and a stack of name sets for rejecting duplicate names. The decoder reads
 * into a buffer it grows and slides forward; tokens and values it hands out
 * point into that buffer. The encoder builds output in a buffer and writes it
 * out each time a top-level value is complete.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "json_internal.h"

#include "burrow/bytes.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/math.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/strconv.h"
#include "burrow/utf8.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#define JT_MAX_NESTING_DEPTH 10000
#define JT_INVALIDATE_BYTE '#'
#define JT_LIT(s) ((Str){(const Byte *)("" s), (Int)(sizeof(s) - 1)})

/* ------------------------------------------------------------------ options */

static JsontextOptions jt_bool_option(uint64_t flag, bool v) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = flag;
    o.values = v ? flag : 0;
    return o;
}

JsontextOptions jsontext_allow_duplicate_names(bool v) {
    return jt_bool_option(JSONFLAG_ALLOW_DUPLICATE_NAMES, v);
}

JsontextOptions jsontext_allow_invalid_utf8(bool v) {
    return jt_bool_option(JSONFLAG_ALLOW_INVALID_UTF8, v);
}

JsontextOptions jsontext_escape_for_html(bool v) {
    return jt_bool_option(JSONFLAG_ESCAPE_FOR_HTML, v);
}

JsontextOptions jsontext_escape_for_js(bool v) {
    return jt_bool_option(JSONFLAG_ESCAPE_FOR_JS, v);
}

JsontextOptions jsontext_preserve_raw_strings(bool v) {
    return jt_bool_option(JSONFLAG_PRESERVE_RAW_STRINGS, v);
}

JsontextOptions jsontext_canonicalize_raw_ints(bool v) {
    return jt_bool_option(JSONFLAG_CANONICALIZE_RAW_INTS, v);
}

JsontextOptions jsontext_canonicalize_raw_floats(bool v) {
    return jt_bool_option(JSONFLAG_CANONICALIZE_RAW_FLOATS, v);
}

JsontextOptions jsontext_reorder_raw_objects(bool v) {
    return jt_bool_option(JSONFLAG_REORDER_RAW_OBJECTS, v);
}

JsontextOptions jsontext_space_after_colon(bool v) {
    return jt_bool_option(JSONFLAG_SPACE_AFTER_COLON, v);
}

JsontextOptions jsontext_space_after_comma(bool v) {
    return jt_bool_option(JSONFLAG_SPACE_AFTER_COMMA, v);
}

JsontextOptions jsontext_multiline(bool v) {
    return jt_bool_option(JSONFLAG_MULTILINE, v);
}

/* A panic message made of three parts. A panic can outlive the frame that
 * raised it, so the message goes in the goroutine's error arena, which lasts
 * as long as the goroutine does. */
BURROW_NORETURN static void jt_panic3(Str a, Str b, Str c) {
    Int n = a.len + b.len + c.len;
    Byte *p = (Byte *)mem_alloc_nozero(error_allocator(), (size_t)n, 1);
    if (p == NULL)
        panic_str(a);
    memcpy(p, a.p, (size_t)a.len);
    if (b.len > 0)
        memcpy(p + a.len, b.p, (size_t)b.len);
    if (c.len > 0)
        memcpy(p + a.len + b.len, c.p, (size_t)c.len);
    panic_str(str_from_bytes(p, n));
}

/* strings.Trim(s, " \t"). */
static Str jt_trim_space_tab(Str s) {
    Int i = 0;
    Int j = s.len;
    while (i < j && (s.p[i] == ' ' || s.p[i] == '\t'))
        i++;
    while (j > i && (s.p[j - 1] == ' ' || s.p[j - 1] == '\t'))
        j--;
    return str_from_bytes(s.p + i, j - i);
}

static void jt_check_indent(Str s, Str where) {
    Str rest = jt_trim_space_tab(s);
    if (rest.len > 0) {
        Byte room[16];
        Str q = burrow__jsonwire_quote_rune(rest.p, rest.len, room);
        jt_panic3(JT_LIT("json: invalid character "), q, where);
    }
}

JsontextOptions jsontext_with_indent(Str indent) {
    jt_check_indent(indent, JT_LIT(" in indent"));
    JsontextOptions o = jt_bool_option(JSONFLAG_MULTILINE | JSONFLAG_INDENT, true);
    o.indent = indent;
    return o;
}

JsontextOptions jsontext_with_indent_prefix(Str prefix) {
    jt_check_indent(prefix, JT_LIT(" in indent prefix"));
    JsontextOptions o =
        jt_bool_option(JSONFLAG_MULTILINE | JSONFLAG_INDENT_PREFIX, true);
    o.indent_prefix = prefix;
    return o;
}

void burrow__jsonopts_join(JsontextOptions *dst, const JsontextOptions *src) {
    dst->presence |= src->presence;
    dst->values &= ~src->presence;
    dst->values |= src->values;
    if ((src->presence & JSONFLAG_NON_BOOLEAN) == 0)
        return;
    if (src->presence & JSONFLAG_INDENT)
        dst->indent = src->indent;
    if (src->presence & JSONFLAG_INDENT_PREFIX)
        dst->indent_prefix = src->indent_prefix;
    if (src->presence & JSONFLAG_BYTE_LIMIT)
        dst->byte_limit = src->byte_limit;
    if (src->presence & JSONFLAG_DEPTH_LIMIT)
        dst->depth_limit = src->depth_limit;
    if (src->presence & JSONFLAG_MARSHALERS)
        dst->marshalers = src->marshalers;
    if (src->presence & JSONFLAG_UNMARSHALERS)
        dst->unmarshalers = src->unmarshalers;
    if (src->presence & JSONFLAG_FORMAT_TAG)
        dst->format = src->format;
}

void burrow__jsonopts_join_slice(JsontextOptions *dst, Slice opts) {
    const JsontextOptions *o = (const JsontextOptions *)opts.p;
    for (Int i = 0; i < opts.len; i++)
        burrow__jsonopts_join(dst, &o[i]);
}

void burrow__jsonopts_initialize_multiline(JsontextOptions *o) {
    if (!jsonflags_has(o, JSONFLAG_SPACE_AFTER_COLON))
        jsonflags_set(o, JSONFLAG_SPACE_AFTER_COLON | 1);
    if (!jsonflags_has(o, JSONFLAG_SPACE_AFTER_COMMA))
        jsonflags_set(o, JSONFLAG_SPACE_AFTER_COMMA | 0);
    if (!jsonflags_has(o, JSONFLAG_INDENT)) {
        jsonflags_set(o, JSONFLAG_INDENT | 1);
        o->indent = JT_LIT("\t");
    }
}

static JsontextOptions jt_join_va(int n, va_list ap) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    for (int i = 0; i < n; i++) {
        JsontextOptions x = va_arg(ap, JsontextOptions);
        burrow__jsonopts_join(&o, &x);
    }
    return o;
}

static JsontextOptions jt_join_slice(Slice opts) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    burrow__jsonopts_join_slice(&o, opts);
    return o;
}

static const Type jt_options_desc = {
    {(const Byte *)"Options", 7},
    {(const Byte *)"jsontext", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsontextOptions),
    (uint16_t)_Alignof(JsontextOptions),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a746f70U, /* "jtop" */
    NULL,
};

const Type *const TYPE_JSONTEXT_OPTIONS = &jt_options_desc;

/* -------------------------------------------------------------------- kinds */

static const Byte jt_norm_kind[256] = {
    ['n'] = 'n', ['f'] = 'f', ['t'] = 't', ['"'] = '"', ['{'] = '{',
    ['}'] = '}', ['['] = '[', [']'] = ']', ['-'] = '0', ['0'] = '0',
    ['1'] = '0', ['2'] = '0', ['3'] = '0', ['4'] = '0', ['5'] = '0',
    ['6'] = '0', ['7'] = '0', ['8'] = '0', ['9'] = '0',
};

static inline JsontextKind jt_norm(Byte c) {
    return jt_norm_kind[c];
}

/* What Kind.String gives for a byte that is not a kind, one per byte so that
 * the result can be static. */
static const char *const jt_bad_kind_names[256] = {
    "<invalid jsontext.Kind: '\\x00'>", "<invalid jsontext.Kind: '\\x01'>",
    "<invalid jsontext.Kind: '\\x02'>", "<invalid jsontext.Kind: '\\x03'>",
    "<invalid jsontext.Kind: '\\x04'>", "<invalid jsontext.Kind: '\\x05'>",
    "<invalid jsontext.Kind: '\\x06'>", "<invalid jsontext.Kind: '\\a'>",
    "<invalid jsontext.Kind: '\\b'>",   "<invalid jsontext.Kind: '\\t'>",
    "<invalid jsontext.Kind: '\\n'>",   "<invalid jsontext.Kind: '\\v'>",
    "<invalid jsontext.Kind: '\\f'>",   "<invalid jsontext.Kind: '\\r'>",
    "<invalid jsontext.Kind: '\\x0e'>", "<invalid jsontext.Kind: '\\x0f'>",
    "<invalid jsontext.Kind: '\\x10'>", "<invalid jsontext.Kind: '\\x11'>",
    "<invalid jsontext.Kind: '\\x12'>", "<invalid jsontext.Kind: '\\x13'>",
    "<invalid jsontext.Kind: '\\x14'>", "<invalid jsontext.Kind: '\\x15'>",
    "<invalid jsontext.Kind: '\\x16'>", "<invalid jsontext.Kind: '\\x17'>",
    "<invalid jsontext.Kind: '\\x18'>", "<invalid jsontext.Kind: '\\x19'>",
    "<invalid jsontext.Kind: '\\x1a'>", "<invalid jsontext.Kind: '\\x1b'>",
    "<invalid jsontext.Kind: '\\x1c'>", "<invalid jsontext.Kind: '\\x1d'>",
    "<invalid jsontext.Kind: '\\x1e'>", "<invalid jsontext.Kind: '\\x1f'>",
    "<invalid jsontext.Kind: ' '>",     "<invalid jsontext.Kind: '!'>",
    "<invalid jsontext.Kind: '\"'>",    "<invalid jsontext.Kind: '#'>",
    "<invalid jsontext.Kind: '$'>",     "<invalid jsontext.Kind: '%'>",
    "<invalid jsontext.Kind: '&'>",     "<invalid jsontext.Kind: '\\''>",
    "<invalid jsontext.Kind: '('>",     "<invalid jsontext.Kind: ')'>",
    "<invalid jsontext.Kind: '*'>",     "<invalid jsontext.Kind: '+'>",
    "<invalid jsontext.Kind: ','>",     "<invalid jsontext.Kind: '-'>",
    "<invalid jsontext.Kind: '.'>",     "<invalid jsontext.Kind: '/'>",
    "<invalid jsontext.Kind: '0'>",     "<invalid jsontext.Kind: '1'>",
    "<invalid jsontext.Kind: '2'>",     "<invalid jsontext.Kind: '3'>",
    "<invalid jsontext.Kind: '4'>",     "<invalid jsontext.Kind: '5'>",
    "<invalid jsontext.Kind: '6'>",     "<invalid jsontext.Kind: '7'>",
    "<invalid jsontext.Kind: '8'>",     "<invalid jsontext.Kind: '9'>",
    "<invalid jsontext.Kind: ':'>",     "<invalid jsontext.Kind: ';'>",
    "<invalid jsontext.Kind: '<'>",     "<invalid jsontext.Kind: '='>",
    "<invalid jsontext.Kind: '>'>",     "<invalid jsontext.Kind: '?'>",
    "<invalid jsontext.Kind: '@'>",     "<invalid jsontext.Kind: 'A'>",
    "<invalid jsontext.Kind: 'B'>",     "<invalid jsontext.Kind: 'C'>",
    "<invalid jsontext.Kind: 'D'>",     "<invalid jsontext.Kind: 'E'>",
    "<invalid jsontext.Kind: 'F'>",     "<invalid jsontext.Kind: 'G'>",
    "<invalid jsontext.Kind: 'H'>",     "<invalid jsontext.Kind: 'I'>",
    "<invalid jsontext.Kind: 'J'>",     "<invalid jsontext.Kind: 'K'>",
    "<invalid jsontext.Kind: 'L'>",     "<invalid jsontext.Kind: 'M'>",
    "<invalid jsontext.Kind: 'N'>",     "<invalid jsontext.Kind: 'O'>",
    "<invalid jsontext.Kind: 'P'>",     "<invalid jsontext.Kind: 'Q'>",
    "<invalid jsontext.Kind: 'R'>",     "<invalid jsontext.Kind: 'S'>",
    "<invalid jsontext.Kind: 'T'>",     "<invalid jsontext.Kind: 'U'>",
    "<invalid jsontext.Kind: 'V'>",     "<invalid jsontext.Kind: 'W'>",
    "<invalid jsontext.Kind: 'X'>",     "<invalid jsontext.Kind: 'Y'>",
    "<invalid jsontext.Kind: 'Z'>",     "<invalid jsontext.Kind: '['>",
    "<invalid jsontext.Kind: '\\\\'>",  "<invalid jsontext.Kind: ']'>",
    "<invalid jsontext.Kind: '^'>",     "<invalid jsontext.Kind: '_'>",
    "<invalid jsontext.Kind: '`'>",     "<invalid jsontext.Kind: 'a'>",
    "<invalid jsontext.Kind: 'b'>",     "<invalid jsontext.Kind: 'c'>",
    "<invalid jsontext.Kind: 'd'>",     "<invalid jsontext.Kind: 'e'>",
    "<invalid jsontext.Kind: 'f'>",     "<invalid jsontext.Kind: 'g'>",
    "<invalid jsontext.Kind: 'h'>",     "<invalid jsontext.Kind: 'i'>",
    "<invalid jsontext.Kind: 'j'>",     "<invalid jsontext.Kind: 'k'>",
    "<invalid jsontext.Kind: 'l'>",     "<invalid jsontext.Kind: 'm'>",
    "<invalid jsontext.Kind: 'n'>",     "<invalid jsontext.Kind: 'o'>",
    "<invalid jsontext.Kind: 'p'>",     "<invalid jsontext.Kind: 'q'>",
    "<invalid jsontext.Kind: 'r'>",     "<invalid jsontext.Kind: 's'>",
    "<invalid jsontext.Kind: 't'>",     "<invalid jsontext.Kind: 'u'>",
    "<invalid jsontext.Kind: 'v'>",     "<invalid jsontext.Kind: 'w'>",
    "<invalid jsontext.Kind: 'x'>",     "<invalid jsontext.Kind: 'y'>",
    "<invalid jsontext.Kind: 'z'>",     "<invalid jsontext.Kind: '{'>",
    "<invalid jsontext.Kind: '|'>",     "<invalid jsontext.Kind: '}'>",
    "<invalid jsontext.Kind: '~'>",     "<invalid jsontext.Kind: '\\x7f'>",
    "<invalid jsontext.Kind: '\\x80'>", "<invalid jsontext.Kind: '\\x81'>",
    "<invalid jsontext.Kind: '\\x82'>", "<invalid jsontext.Kind: '\\x83'>",
    "<invalid jsontext.Kind: '\\x84'>", "<invalid jsontext.Kind: '\\x85'>",
    "<invalid jsontext.Kind: '\\x86'>", "<invalid jsontext.Kind: '\\x87'>",
    "<invalid jsontext.Kind: '\\x88'>", "<invalid jsontext.Kind: '\\x89'>",
    "<invalid jsontext.Kind: '\\x8a'>", "<invalid jsontext.Kind: '\\x8b'>",
    "<invalid jsontext.Kind: '\\x8c'>", "<invalid jsontext.Kind: '\\x8d'>",
    "<invalid jsontext.Kind: '\\x8e'>", "<invalid jsontext.Kind: '\\x8f'>",
    "<invalid jsontext.Kind: '\\x90'>", "<invalid jsontext.Kind: '\\x91'>",
    "<invalid jsontext.Kind: '\\x92'>", "<invalid jsontext.Kind: '\\x93'>",
    "<invalid jsontext.Kind: '\\x94'>", "<invalid jsontext.Kind: '\\x95'>",
    "<invalid jsontext.Kind: '\\x96'>", "<invalid jsontext.Kind: '\\x97'>",
    "<invalid jsontext.Kind: '\\x98'>", "<invalid jsontext.Kind: '\\x99'>",
    "<invalid jsontext.Kind: '\\x9a'>", "<invalid jsontext.Kind: '\\x9b'>",
    "<invalid jsontext.Kind: '\\x9c'>", "<invalid jsontext.Kind: '\\x9d'>",
    "<invalid jsontext.Kind: '\\x9e'>", "<invalid jsontext.Kind: '\\x9f'>",
    "<invalid jsontext.Kind: '\\xa0'>", "<invalid jsontext.Kind: '\\xa1'>",
    "<invalid jsontext.Kind: '\\xa2'>", "<invalid jsontext.Kind: '\\xa3'>",
    "<invalid jsontext.Kind: '\\xa4'>", "<invalid jsontext.Kind: '\\xa5'>",
    "<invalid jsontext.Kind: '\\xa6'>", "<invalid jsontext.Kind: '\\xa7'>",
    "<invalid jsontext.Kind: '\\xa8'>", "<invalid jsontext.Kind: '\\xa9'>",
    "<invalid jsontext.Kind: '\\xaa'>", "<invalid jsontext.Kind: '\\xab'>",
    "<invalid jsontext.Kind: '\\xac'>", "<invalid jsontext.Kind: '\\xad'>",
    "<invalid jsontext.Kind: '\\xae'>", "<invalid jsontext.Kind: '\\xaf'>",
    "<invalid jsontext.Kind: '\\xb0'>", "<invalid jsontext.Kind: '\\xb1'>",
    "<invalid jsontext.Kind: '\\xb2'>", "<invalid jsontext.Kind: '\\xb3'>",
    "<invalid jsontext.Kind: '\\xb4'>", "<invalid jsontext.Kind: '\\xb5'>",
    "<invalid jsontext.Kind: '\\xb6'>", "<invalid jsontext.Kind: '\\xb7'>",
    "<invalid jsontext.Kind: '\\xb8'>", "<invalid jsontext.Kind: '\\xb9'>",
    "<invalid jsontext.Kind: '\\xba'>", "<invalid jsontext.Kind: '\\xbb'>",
    "<invalid jsontext.Kind: '\\xbc'>", "<invalid jsontext.Kind: '\\xbd'>",
    "<invalid jsontext.Kind: '\\xbe'>", "<invalid jsontext.Kind: '\\xbf'>",
    "<invalid jsontext.Kind: '\\xc0'>", "<invalid jsontext.Kind: '\\xc1'>",
    "<invalid jsontext.Kind: '\\xc2'>", "<invalid jsontext.Kind: '\\xc3'>",
    "<invalid jsontext.Kind: '\\xc4'>", "<invalid jsontext.Kind: '\\xc5'>",
    "<invalid jsontext.Kind: '\\xc6'>", "<invalid jsontext.Kind: '\\xc7'>",
    "<invalid jsontext.Kind: '\\xc8'>", "<invalid jsontext.Kind: '\\xc9'>",
    "<invalid jsontext.Kind: '\\xca'>", "<invalid jsontext.Kind: '\\xcb'>",
    "<invalid jsontext.Kind: '\\xcc'>", "<invalid jsontext.Kind: '\\xcd'>",
    "<invalid jsontext.Kind: '\\xce'>", "<invalid jsontext.Kind: '\\xcf'>",
    "<invalid jsontext.Kind: '\\xd0'>", "<invalid jsontext.Kind: '\\xd1'>",
    "<invalid jsontext.Kind: '\\xd2'>", "<invalid jsontext.Kind: '\\xd3'>",
    "<invalid jsontext.Kind: '\\xd4'>", "<invalid jsontext.Kind: '\\xd5'>",
    "<invalid jsontext.Kind: '\\xd6'>", "<invalid jsontext.Kind: '\\xd7'>",
    "<invalid jsontext.Kind: '\\xd8'>", "<invalid jsontext.Kind: '\\xd9'>",
    "<invalid jsontext.Kind: '\\xda'>", "<invalid jsontext.Kind: '\\xdb'>",
    "<invalid jsontext.Kind: '\\xdc'>", "<invalid jsontext.Kind: '\\xdd'>",
    "<invalid jsontext.Kind: '\\xde'>", "<invalid jsontext.Kind: '\\xdf'>",
    "<invalid jsontext.Kind: '\\xe0'>", "<invalid jsontext.Kind: '\\xe1'>",
    "<invalid jsontext.Kind: '\\xe2'>", "<invalid jsontext.Kind: '\\xe3'>",
    "<invalid jsontext.Kind: '\\xe4'>", "<invalid jsontext.Kind: '\\xe5'>",
    "<invalid jsontext.Kind: '\\xe6'>", "<invalid jsontext.Kind: '\\xe7'>",
    "<invalid jsontext.Kind: '\\xe8'>", "<invalid jsontext.Kind: '\\xe9'>",
    "<invalid jsontext.Kind: '\\xea'>", "<invalid jsontext.Kind: '\\xeb'>",
    "<invalid jsontext.Kind: '\\xec'>", "<invalid jsontext.Kind: '\\xed'>",
    "<invalid jsontext.Kind: '\\xee'>", "<invalid jsontext.Kind: '\\xef'>",
    "<invalid jsontext.Kind: '\\xf0'>", "<invalid jsontext.Kind: '\\xf1'>",
    "<invalid jsontext.Kind: '\\xf2'>", "<invalid jsontext.Kind: '\\xf3'>",
    "<invalid jsontext.Kind: '\\xf4'>", "<invalid jsontext.Kind: '\\xf5'>",
    "<invalid jsontext.Kind: '\\xf6'>", "<invalid jsontext.Kind: '\\xf7'>",
    "<invalid jsontext.Kind: '\\xf8'>", "<invalid jsontext.Kind: '\\xf9'>",
    "<invalid jsontext.Kind: '\\xfa'>", "<invalid jsontext.Kind: '\\xfb'>",
    "<invalid jsontext.Kind: '\\xfc'>", "<invalid jsontext.Kind: '\\xfd'>",
    "<invalid jsontext.Kind: '\\xfe'>", "<invalid jsontext.Kind: '\\xff'>",
};

Str jsontext_kind_string(JsontextKind k) {
    switch (k) {
    case 0:
        return JT_LIT("invalid");
    case 'n':
        return JT_LIT("null");
    case 'f':
        return JT_LIT("false");
    case 't':
        return JT_LIT("true");
    case '"':
        return JT_LIT("string");
    case '0':
        return JT_LIT("number");
    case '{':
        return JT_LIT("{");
    case '}':
        return JT_LIT("}");
    case '[':
        return JT_LIT("[");
    case ']':
        return JT_LIT("]");
    default:
        return str_from_cstr(jt_bad_kind_names[k]);
    }
}

BURROW_NORETURN static void jt_panic_kind(JsontextKind k) {
    jt_panic3(JT_LIT("invalid JSON token kind: "), jsontext_kind_string(k),
              BURROW_STR_EMPTY);
}

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(jsontext_err_duplicate_name, "duplicate object member name");
BURROW_SENTINEL_ERROR(jsontext_err_non_string_name,
                      "object member name must be a string");
BURROW_SENTINEL_ERROR(jt_err_missing_value, "missing value after object name");
BURROW_SENTINEL_ERROR(jt_err_mismatch_delim,
                      "mismatching structural token for object or array");
BURROW_SENTINEL_ERROR(jt_err_max_depth, "exceeded max depth");
BURROW_SENTINEL_ERROR(jt_err_invalid_namespace,
                      "object namespace is in an invalid state");
BURROW_SENTINEL_ERROR(jt_err_invalid_token, "invalid jsontext.Token");
BURROW_SENTINEL_ERROR(jt_err_unmarshal_nil,
                      "jsontext.Value: UnmarshalJSON on nil pointer");

static inline bool jt_is(Error a, Error b) {
    return jsonwire_same_error(a, b);
}

/* The decimal digits of v into b. */
static void jt_put_uint(JsonBuf *b, uint64_t v) {
    Byte digits[24];
    Int d = (Int)sizeof(digits);
    do {
        digits[--d] = (Byte)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    jsonbuf_put(b, digits + d, (Int)sizeof(digits) - d);
}

static void jt_put_int(JsonBuf *b, int64_t v) {
    if (v < 0) {
        jsonbuf_byte(b, '-');
        jt_put_uint(b, 0 - (uint64_t)v);
        return;
    }
    jt_put_uint(b, (uint64_t)v);
}

/* strconv.AppendQuote. */
static void jt_put_go_quote(JsonBuf *b, Str s) {
    Int n = burrow__strconv_quote_into(NULL, s);
    if (!jsonbuf_reserve(b, n))
        return;
    burrow__strconv_quote_into(b->p + b->len, s);
    b->len += n;
}

static JsonBuf jt_heap_buf(void) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    return b;
}

/* Go's ioError and numError: a message built up front and the error they
 * wrap. Neither is exported, so neither can be pulled out with errors_as. */
typedef struct JtWrapError {
    Str message;
    Error err;
} JtWrapError;

static Str jt_wrap_message(const void *self) {
    return ((const JtWrapError *)self)->message;
}

static Error jt_wrap_unwrap(const void *self) {
    return ((const JtWrapError *)self)->err;
}

static const ErrorVT jt_io_error_vt = {
    NULL, jt_wrap_message, jt_wrap_unwrap, NULL, NULL, NULL, NULL,
};

static const ErrorVT jt_num_error_vt = {
    NULL, jt_wrap_message, jt_wrap_unwrap, NULL, NULL, NULL, NULL,
};

static Error jt_wrap_build(const ErrorVT *vt, const Str *parts, int nparts,
                           Error inner) {
    Int n = 0;
    for (int i = 0; i < nparts; i++)
        n += parts[i].len;
    JtWrapError *w = (JtWrapError *)mem_alloc_nozero(
        error_allocator(), sizeof(JtWrapError) + (size_t)n, _Alignof(JtWrapError));
    if (w == NULL)
        return inner;
    Byte *p = (Byte *)(w + 1);
    Int at = 0;
    for (int i = 0; i < nparts; i++) {
        if (parts[i].len > 0)
            memcpy(p + at, parts[i].p, (size_t)parts[i].len);
        at += parts[i].len;
    }
    w->message = str_from_bytes(p, n);
    w->err = inner;
    return (Error){vt, w};
}

/* &ioError{action, err}: "jsontext: read error: " and the reason. */
static Error jt_io_error(Str action, Error err) {
    Str parts[4] = {JT_LIT("jsontext: "), action, JT_LIT(" error: "), error_text(err)};
    return jt_wrap_build(&jt_io_error_vt, parts, 4, err);
}

static bool jt_is_io_error(Error err) {
    return err.vt == &jt_io_error_vt;
}

bool burrow__jsontext_is_io_error(Error err) {
    return jt_is_io_error(err);
}

/* ------------------------------------------------------- SyntacticError */

typedef struct JtSyntacticBox {
    JsontextSyntacticError e;
    Str message;
} JtSyntacticBox;

static Str jt_syntactic_message(const void *self) {
    return ((const JtSyntacticBox *)self)->message;
}

static Error jt_syntactic_unwrap(const void *self) {
    return ((const JsontextSyntacticError *)self)->err;
}

static const Type jt_syntactic_desc = {
    {(const Byte *)"SyntacticError", 14},
    {(const Byte *)"jsontext", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsontextSyntacticError),
    (uint16_t)_Alignof(JsontextSyntacticError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a747365U, /* "jtse" */
    NULL,
};

const Type *const TYPE_JSONTEXT_SYNTACTIC_ERROR = &jt_syntactic_desc;

static Error jt_syntactic_clone(const void *self, Alloc *a);

static const ErrorVT jt_syntactic_vt = {
    &jt_syntactic_desc, jt_syntactic_message, jt_syntactic_unwrap, NULL, NULL, NULL,
    jt_syntactic_clone,
};

static void jt_put_unescaped_token(JsonBuf *b, Str tok);

/* What follows the last '/' of p, or all of p without one. */
static Str jt_pointer_last_raw(Str p) {
    Int i = p.len - 1;
    while (i >= 0 && p.p[i] != '/')
        i--;
    return str_from_bytes(p.p + i + 1, p.len - i - 1);
}

static Str jt_pointer_parent_raw(Str p) {
    Int i = p.len - 1;
    while (i >= 0 && p.p[i] != '/')
        i--;
    return str_from_bytes(p.p, i < 0 ? 0 : i);
}

/* SyntacticError.Error into b. */
static void jt_syntactic_text(JsonBuf *b, const JsontextSyntacticError *e) {
    Str pointer = e->json_pointer;
    int64_t offset = e->byte_offset;
    jsonbuf_str(b, JT_LIT("jsontext: "));
    if (BURROW_FAILED(e->err)) {
        jsonbuf_str(b, error_text(e->err));
        if (jt_is(e->err, jsontext_err_duplicate_name)) {
            jsonbuf_byte(b, ' ');
            JsonBuf tok = jt_heap_buf();
            jt_put_unescaped_token(&tok, jt_pointer_last_raw(pointer));
            jt_put_go_quote(b, str_from_bytes(tok.p, tok.len));
            if (tok.failed)
                b->failed = true;
            burrow__jsonbuf_free(&tok);
            pointer = jt_pointer_parent_raw(pointer);
            offset = 0;
        }
    } else {
        jsonbuf_str(b, JT_LIT("syntactic error"));
    }
    if (pointer.len > 0) {
        jsonbuf_str(b, JT_LIT(" within "));
        JsonBuf t = jt_heap_buf();
        burrow__jsonwire_truncate_pointer(&t, pointer, 100);
        jt_put_go_quote(b, str_from_bytes(t.p, t.len));
        if (t.failed)
            b->failed = true;
        burrow__jsonbuf_free(&t);
    }
    if (offset > 0) {
        jsonbuf_str(b, JT_LIT(" after offset "));
        jt_put_int(b, offset);
    }
}

/* One allocation holding the box, the pointer and the message. The zero
 * Error when a refuses. */
static Error jt_syntactic_build(Alloc *a, const JsontextSyntacticError *e) {
    JsonBuf msg = jt_heap_buf();
    jt_syntactic_text(&msg, e);
    if (msg.failed) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    Int plen = e->json_pointer.len;
    JtSyntacticBox *box = (JtSyntacticBox *)mem_alloc_nozero(
        a, sizeof(JtSyntacticBox) + (size_t)plen + (size_t)msg.len,
        _Alignof(JtSyntacticBox));
    if (box == NULL) {
        burrow__jsonbuf_free(&msg);
        return BURROW_NO_ERROR;
    }
    Byte *p = (Byte *)(box + 1);
    if (plen > 0)
        memcpy(p, e->json_pointer.p, (size_t)plen);
    if (msg.len > 0)
        memcpy(p + plen, msg.p, (size_t)msg.len);
    box->e.byte_offset = e->byte_offset;
    box->e.json_pointer = str_from_bytes(p, plen);
    box->e.err = e->err;
    box->message = str_from_bytes(p + plen, msg.len);
    burrow__jsonbuf_free(&msg);
    return (Error){&jt_syntactic_vt, box};
}

Str jsontext_syntactic_error_error(const JsontextSyntacticError *e, Alloc *a) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jt_syntactic_text(&b, e);
    if (b.failed) {
        burrow__jsonbuf_free(&b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(b.p, b.len);
}

Error jsontext_syntactic_error_unwrap(const JsontextSyntacticError *e) {
    return e->err;
}

Error jsontext_syntactic_error_as_error(const JsontextSyntacticError *e, Alloc *a) {
    Error made = jt_syntactic_build(a, e);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

static Error jt_syntactic_clone(const void *self, Alloc *a) {
    JsontextSyntacticError copy = *(const JsontextSyntacticError *)self;
    copy.err = error_retain(a, copy.err);
    return jsontext_syntactic_error_as_error(&copy, a);
}

/* A SyntacticError in the error arena, or err alone if that cannot be had. */
static Error jt_syntactic_new(int64_t offset, Str pointer, Error err) {
    JsontextSyntacticError e = {offset, pointer, err};
    Error made = jt_syntactic_build(error_allocator(), &e);
    return BURROW_FAILED(made) ? made : err;
}

/* ----------------------------------------------------------------- pointers */

/* unescapePointerToken: ~1 becomes '/' and then ~0 becomes '~'. */
static void jt_put_unescaped_token(JsonBuf *b, Str tok) {
    Int start = b->len;
    jsonbuf_str(b, tok);
    if (b->failed || tok.len <= 0 || memchr(tok.p, '~', (size_t)tok.len) == NULL)
        return;
    /* Two passes, as the two ReplaceAll calls do. */
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

/* appendEscapePointerName. The name goes rune by rune, so invalid UTF-8
 * comes out as U+FFFD. */
static void jt_put_escaped_name(JsonBuf *b, Str name) {
    Int i = 0;
    while (i < name.len) {
        Byte c = name.p[i];
        if (c < 0x80) {
            if (c == '~')
                jsonbuf_str(b, JT_LIT("~0"));
            else if (c == '/')
                jsonbuf_str(b, JT_LIT("~1"));
            else
                jsonbuf_byte(b, c);
            i++;
            continue;
        }
        Int size;
        Rune r =
            utf8_decode_rune_in_string(str_from_bytes(name.p + i, name.len - i), &size);
        if (!jsonbuf_reserve(b, 4))
            return;
        Slice room = {b->p + b->len, 0, 4, TYPE_BYTE};
        b->len += utf8_encode_rune(room, r);
        i += size;
    }
}

bool jsontext_pointer_is_valid(JsontextPointer p) {
    Int i = 0;
    while (i < p.len) {
        Byte c = p.p[i];
        if (c == '~' && (i + 1 == p.len || (p.p[i + 1] != '0' && p.p[i + 1] != '1')))
            return false;
        if (c < 0x80) {
            i++;
            continue;
        }
        Int size;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(p.p + i, p.len - i), &size);
        if (r == UTF8_RUNE_ERROR && size == 1)
            return false;
        i += size;
    }
    return p.len == 0 || p.p[0] == '/';
}

bool jsontext_pointer_contains(JsontextPointer p, JsontextPointer pc) {
    if (pc.len < p.len || (p.len > 0 && memcmp(pc.p, p.p, (size_t)p.len) != 0))
        return false;
    return pc.len == p.len || pc.p[p.len] == '/';
}

JsontextPointer jsontext_pointer_parent(JsontextPointer p) {
    return jt_pointer_parent_raw(p);
}

static Str jt_buf_str_or_empty(JsonBuf *b) {
    if (b->failed) {
        burrow__jsonbuf_free(b);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(b->p, b->len);
}

Str jsontext_pointer_last_token(JsontextPointer p, Alloc *a) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jt_put_unescaped_token(&b, jt_pointer_last_raw(p));
    return jt_buf_str_or_empty(&b);
}

JsontextPointer jsontext_pointer_append_token(JsontextPointer p, Alloc *a, Str tok) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jsonbuf_reserve(&b, p.len + 1 + tok.len);
    jsonbuf_str(&b, p);
    jsonbuf_byte(&b, '/');
    jt_put_escaped_name(&b, tok);
    return jt_buf_str_or_empty(&b);
}

/* What a Tokens sequence holds: the pointer and room to unescape the longest
 * token into. */
typedef struct JtTokensEnv {
    Str p;
    Byte *room;
    Int cap;
} JtTokensEnv;

static void jt_tokens_run(void *env, IterYield yield) {
    JtTokensEnv *st = (JtTokensEnv *)env;
    Str p = st->p;
    while (p.len > 0) {
        if (p.p[0] == '/')
            p = str_from_bytes(p.p + 1, p.len - 1);
        Int i = 0;
        while (i < p.len && p.p[i] != '/')
            i++;
        JsonBuf b = {st->room, 0, st->cap, heap_allocator(), false, false};
        jt_put_unescaped_token(&b, str_from_bytes(p.p, i));
        Str tok = str_from_bytes(b.p, b.len);
        if (!BURROW_CALLF(yield, &tok) || i == p.len)
            return;
        p = str_from_bytes(p.p + i, p.len - i);
    }
}

IterSeq jsontext_pointer_tokens(JsontextPointer p, Alloc *a) {
    IterSeq seq = {NULL, NULL};
    JtTokensEnv *st = (JtTokensEnv *)mem_alloc_nozero(
        a, sizeof(JtTokensEnv) + (size_t)p.len, _Alignof(JtTokensEnv));
    if (st == NULL)
        return seq;
    st->p = p;
    st->room = (Byte *)(st + 1);
    st->cap = p.len;
    seq.f = jt_tokens_run;
    seq.env = st;
    return seq;
}

void jsontext_pointer_tokens_free(IterSeq seq, Alloc *a) {
    JtTokensEnv *st = (JtTokensEnv *)seq.env;
    if (st != NULL)
        mem_free(a, st, sizeof(JtTokensEnv) + (size_t)st->cap, _Alignof(JtTokensEnv));
}

/* -------------------------------------------------------------------- state */

#define JT_TYPE_MASK ((uint64_t)0x8000000000000000U)
#define JT_TYPE_OBJECT ((uint64_t)0x8000000000000000U)
#define JT_TYPE_ARRAY ((uint64_t)0)
#define JT_DISABLE_NAMESPACE ((uint64_t)0x4000000000000000U)
#define JT_INVALID_NAMESPACE ((uint64_t)0x2000000000000000U)
#define JT_COUNT_MASK ((uint64_t)0x1fffffffffffffffU)

static inline int64_t jt_e_len(uint64_t e) {
    return (int64_t)(e & JT_COUNT_MASK);
}

static inline bool jt_e_is_object(uint64_t e) {
    return (e & JT_TYPE_MASK) == JT_TYPE_OBJECT;
}

static inline bool jt_e_is_array(uint64_t e) {
    return (e & JT_TYPE_MASK) == JT_TYPE_ARRAY;
}

static inline bool jt_e_need_name(uint64_t e) {
    return (e & (JT_TYPE_MASK | 1)) == JT_TYPE_OBJECT;
}

static inline bool jt_e_need_value(uint64_t e) {
    return (e & (JT_TYPE_MASK | 1)) == (JT_TYPE_OBJECT | 1);
}

static inline bool jt_e_need_comma(uint64_t e, JsontextKind next) {
    return !jt_e_need_value(e) && jt_e_len(e) > 0 && next != '}' && next != ']';
}

static inline bool jt_e_active_ns(uint64_t e) {
    return (e & JT_DISABLE_NAMESPACE) == 0;
}

static inline bool jt_e_valid_ns(uint64_t e) {
    return (e & JT_INVALID_NAMESPACE) == 0;
}

static inline Int jt_depth(const JsonState *s) {
    return s->stack_len + 1;
}

static inline uint64_t jt_index(const JsonState *s, Int i) {
    return i == s->stack_len ? s->last : s->stack[i];
}

/* Room for one more element in an array of elem-sized things from a. */
static bool jt_grow_array(Alloc *a, void **p, Int *cap, Int len, size_t elem,
                          size_t align) {
    if (len < *cap)
        return true;
    Int ncap = *cap < 8 ? 8 : *cap * 2;
    void *q = mem_realloc(a, *p, (size_t)*cap * elem, (size_t)ncap * elem, align);
    if (q == NULL)
        return false;
    *p = q;
    *cap = ncap;
    return true;
}

static Error jt_append_literal(JsonState *s) {
    if (jt_e_need_name(s->last))
        return jsontext_err_non_string_name;
    if (!jt_e_valid_ns(s->last))
        return jt_err_invalid_namespace;
    s->last++;
    return BURROW_NO_ERROR;
}

/* appendNumber is appendLiteral in Go too. */
#define jt_append_number jt_append_literal

static Error jt_append_string(JsonState *s) {
    if (!jt_e_valid_ns(s->last))
        return jt_err_invalid_namespace;
    s->last++;
    return BURROW_NO_ERROR;
}

static Error jt_push(JsonState *s, uint64_t type) {
    if (jt_e_need_name(s->last))
        return jsontext_err_non_string_name;
    if (!jt_e_valid_ns(s->last))
        return jt_err_invalid_namespace;
    if (s->stack_len == JT_MAX_NESTING_DEPTH)
        return jt_err_max_depth;
    void *p = s->stack;
    if (!jt_grow_array(s->a, &p, &s->stack_cap, s->stack_len, sizeof(uint64_t),
                       _Alignof(uint64_t)))
        return burrow_err_out_of_memory;
    s->stack = (uint64_t *)p;
    s->last++;
    s->stack[s->stack_len++] = s->last;
    s->last = type;
    return BURROW_NO_ERROR;
}

static Error jt_pop_object(JsonState *s) {
    if (!jt_e_is_object(s->last))
        return jt_err_mismatch_delim;
    if (jt_e_need_value(s->last))
        return jt_err_missing_value;
    if (!jt_e_valid_ns(s->last))
        return jt_err_invalid_namespace;
    s->last = s->stack[--s->stack_len];
    return BURROW_NO_ERROR;
}

static Error jt_pop_array(JsonState *s) {
    if (!jt_e_is_array(s->last) || s->stack_len == 0)
        return jt_err_mismatch_delim;
    if (!jt_e_valid_ns(s->last))
        return jt_err_invalid_namespace;
    s->last = s->stack[--s->stack_len];
    return BURROW_NO_ERROR;
}

static Int jt_need_indent(const JsonState *s, JsontextKind next) {
    bool will_end = next == '}' || next == ']';
    if (jt_depth(s) == 1)
        return 0;
    if (jt_e_len(s->last) == 0 && will_end)
        return 0;
    if (jt_e_len(s->last) == 0 || jt_e_need_comma(s->last, next))
        return jt_depth(s);
    if (will_end)
        return jt_depth(s) - 1;
    return 0;
}

static Byte jt_need_delim(const JsonState *s, JsontextKind next) {
    if (jt_e_need_value(s->last))
        return ':';
    if (jt_e_need_comma(s->last, next) && s->stack_len != 0)
        return ',';
    return 0;
}

/* objectNameStack. offsets holds, for each open object, where its current
 * name ends in unquoted, or the bitwise complement of where the quoted name
 * starts in the coder's buffer until it has been copied, or the invalid
 * offset before there is a name at all. */
#define JT_INVALID_OFFSET BURROW_INT_MIN

static bool jt_names_push(JsonState *s) {
    void *p = s->offsets;
    if (!jt_grow_array(s->a, &p, &s->offsets_cap, s->offsets_len, sizeof(Int),
                       _Alignof(Int)))
        return false;
    s->offsets = (Int *)p;
    s->offsets[s->offsets_len++] = JT_INVALID_OFFSET;
    return true;
}

static inline void jt_names_replace_last_quoted(JsonState *s, Int pos) {
    s->offsets[s->offsets_len - 1] = ~pos;
}

static inline void jt_names_clear_last(JsonState *s) {
    s->offsets[s->offsets_len - 1] = JT_INVALID_OFFSET;
}

static inline void jt_names_pop(JsonState *s) {
    s->offsets_len--;
}

static Str jt_names_get(const JsonState *s, Int i) {
    Int lo = i == 0 ? 0 : s->offsets[i - 1];
    if (s->offsets[i] == lo)
        return str_from_bytes(NULL, 0);
    return str_from_bytes(s->unquoted.p + lo, s->offsets[i] - lo);
}

/* copyQuotedBuffer: turn every name still pointing into b into a copy. */
static void jt_names_copy_quoted(JsonState *s, Byte *b, Int blen) {
    Int i;
    for (i = s->offsets_len - 1; i >= 0 && s->offsets[i] < 0; i--)
        continue;
    for (i = i + 1; i < s->offsets_len; i++) {
        if (i == s->offsets_len - 1 && s->offsets[i] == JT_INVALID_OFFSET) {
            s->offsets[i] = i == 0 ? 0 : s->offsets[i - 1];
            break;
        }
        Int at = ~s->offsets[i];
        Byte *q = b + at;
        Int qn = blen - at;
        if (q[0] == JT_INVALIDATE_BYTE)
            q[0] = '"';
        Int start = i > 0 ? s->offsets[i - 1] : 0;
        s->unquoted.len = start;
        Int n = jsonwire_consume_simple_string(q, qn);
        if (n > 0)
            jsonbuf_put(&s->unquoted, q + 1, n - 2);
        else
            (void)burrow__jsonwire_append_unquote(&s->unquoted, q, qn);
        s->offsets[i] = s->unquoted.len;
    }
}

static void jt_names_replace_last_unquoted(JsonState *s, Str name) {
    Int start = s->offsets_len > 1 ? s->offsets[s->offsets_len - 2] : 0;
    s->unquoted.len = start;
    jsonbuf_str(&s->unquoted, name);
    s->offsets[s->offsets_len - 1] = s->unquoted.len;
}

/* objectNamespace. Names are kept unquoted end to end in names, and past 64
 * of them or 1 KiB a hash set of their indexes sits alongside, which is what
 * Go's map does. */
static Str jt_ns_name(const JsonNamespace *ns, Int i) {
    Int lo = i == 0 ? 0 : ns->end_offsets[i - 1];
    return str_from_bytes(ns->names + lo, ns->end_offsets[i] - lo);
}

static uint64_t jt_ns_hash(const JsonNamespace *ns, Str name) {
    return runtime_memhash(name.p, (size_t)name.len, ns->seed);
}

/* The slot holding name, or where it would go: an empty slot or the first
 * removed one on the way. *found says which. */
static Int jt_ns_probe(const JsonNamespace *ns, Str name, bool *found) {
    Int mask = ns->slots_cap - 1;
    Int i = (Int)(jt_ns_hash(ns, name) & (uint64_t)mask);
    Int spare = -1;
    for (;;) {
        Int v = ns->slots[i];
        if (v == 0) {
            *found = false;
            return spare >= 0 ? spare : i;
        }
        if (v < 0) {
            if (spare < 0)
                spare = i;
        } else {
            Str have = jt_ns_name(ns, v - 1);
            if (have.len == name.len && memcmp(have.p, name.p, (size_t)name.len) == 0) {
                *found = true;
                return i;
            }
        }
        i = (i + 1) & mask;
    }
}

static bool jt_ns_rehash(JsonNamespace *ns, Alloc *a, Int want) {
    Int cap = 128;
    while (cap < want * 2)
        cap *= 2;
    Int *slots = (Int *)mem_alloc(a, (size_t)cap * sizeof(Int), _Alignof(Int));
    if (slots == NULL)
        return false;
    if (ns->slots != NULL)
        mem_free(a, ns->slots, (size_t)ns->slots_cap * sizeof(Int), _Alignof(Int));
    ns->slots = slots;
    ns->slots_cap = cap;
    ns->slots_used = 0;
    for (Int k = 0; k < ns->len; k++) {
        bool found;
        Int at = jt_ns_probe(ns, jt_ns_name(ns, k), &found);
        if (!found) {
            ns->slots[at] = k + 1;
            ns->slots_used++;
        }
    }
    return true;
}

static void jt_ns_reset(JsonNamespace *ns, Alloc *a) {
    ns->len = 0;
    ns->names_len = 0;
    if (ns->slots != NULL)
        mem_free(a, ns->slots, (size_t)ns->slots_cap * sizeof(Int), _Alignof(Int));
    ns->slots = NULL;
    ns->slots_cap = 0;
    ns->slots_used = 0;
}

static void jt_ns_release(JsonNamespace *ns, Alloc *a) {
    jt_ns_reset(ns, a);
    if (ns->end_offsets != NULL)
        mem_free(a, ns->end_offsets, (size_t)ns->cap * sizeof(Int), _Alignof(Int));
    if (ns->names != NULL)
        mem_free(a, ns->names, (size_t)ns->names_cap, 1);
    memset(ns, 0, sizeof(*ns));
}

/* insert: 1 when name is new and now in the set, 0 when it was already
 * there, -1 when memory ran out. quoted says name is JSON string text. */
static int jt_ns_insert(JsonNamespace *ns, Alloc *a, const Byte *name, Int n,
                        bool quoted) {
    JsonBuf all = {ns->names, ns->names_len, ns->names_cap, a, true, false};
    if (quoted)
        (void)burrow__jsonwire_append_unquote(&all, name, n);
    else
        jsonbuf_put(&all, name, n);
    ns->names = all.p;
    ns->names_cap = all.cap;
    if (all.failed)
        return -1;
    Str added = all.len == ns->names_len
                    ? str_from_bytes(NULL, 0)
                    : str_from_bytes(all.p + ns->names_len, all.len - ns->names_len);
    if (ns->slots == NULL && (ns->len > 64 || ns->names_len > 1024)) {
        if (ns->seed == 0)
            ns->seed = runtime_rand64() | 1;
        if (!jt_ns_rehash(ns, a, ns->len + 1))
            return -1;
    }
    if (ns->slots == NULL) {
        Int lo = 0;
        for (Int k = 0; k < ns->len; k++) {
            Int hi = ns->end_offsets[k];
            if (hi - lo == added.len &&
                (added.len == 0 ||
                 memcmp(ns->names + lo, added.p, (size_t)added.len) == 0))
                return 0;
            lo = hi;
        }
    } else {
        bool found;
        Int at = jt_ns_probe(ns, added, &found);
        if (found)
            return 0;
        if ((ns->slots_used + 1) * 4 > ns->slots_cap * 3) {
            if (!jt_ns_rehash(ns, a, ns->len + 1))
                return -1;
            at = jt_ns_probe(ns, added, &found);
        }
        if (ns->slots[at] == 0)
            ns->slots_used++;
        ns->slots[at] = ns->len + 1;
    }
    void *p = ns->end_offsets;
    if (!jt_grow_array(a, &p, &ns->cap, ns->len, sizeof(Int), _Alignof(Int))) {
        if (ns->slots != NULL) {
            bool found;
            Int at = jt_ns_probe(ns, added, &found);
            if (found)
                ns->slots[at] = -1;
        }
        return -1;
    }
    ns->end_offsets = (Int *)p;
    ns->names_len = all.len;
    ns->end_offsets[ns->len++] = all.len;
    return 1;
}

static int jt_ns_insert_quoted(JsonNamespace *ns, Alloc *a, const Byte *name, Int n,
                               bool is_verbatim) {
    if (is_verbatim)
        return jt_ns_insert(ns, a, name + 1, n - 2, false);
    return jt_ns_insert(ns, a, name, n, true);
}

static void jt_ns_remove_last(JsonNamespace *ns) {
    if (ns->slots != NULL) {
        bool found;
        Int at = jt_ns_probe(ns, jt_ns_name(ns, ns->len - 1), &found);
        if (found)
            ns->slots[at] = -1;
    }
    ns->len--;
    ns->names_len = ns->len == 0 ? 0 : ns->end_offsets[ns->len - 1];
}

static bool jt_spaces_push(JsonState *s) {
    if (s->spaces_len < s->spaces_cap) {
        jt_ns_reset(&s->spaces[s->spaces_len++], s->a);
        return true;
    }
    Int old = s->spaces_cap;
    void *p = s->spaces;
    if (!jt_grow_array(s->a, &p, &s->spaces_cap, s->spaces_len, sizeof(JsonNamespace),
                       _Alignof(JsonNamespace)))
        return false;
    s->spaces = (JsonNamespace *)p;
    memset(s->spaces + old, 0, (size_t)(s->spaces_cap - old) * sizeof(JsonNamespace));
    s->spaces_len++;
    return true;
}

static inline JsonNamespace *jt_spaces_last(JsonState *s) {
    return &s->spaces[s->spaces_len - 1];
}

static inline void jt_spaces_pop(JsonState *s) {
    s->spaces_len--;
}

static void jt_state_reset(JsonState *s) {
    s->stack_len = 0;
    s->last = JT_TYPE_ARRAY;
    s->offsets_len = 0;
    s->unquoted.len = 0;
    s->unquoted.failed = false;
    s->spaces_len = 0;
    s->rev.len = 0;
    s->rev.failed = false;
}

static void jt_state_init(JsonState *s, Alloc *a) {
    memset(s, 0, sizeof(*s));
    s->a = a;
    s->unquoted.a = a;
    s->unquoted.owned = true;
    s->rev.a = a;
    s->rev.owned = true;
}

static void jt_state_release(JsonState *s) {
    Alloc *a = s->a;
    if (s->stack != NULL)
        mem_free(a, s->stack, (size_t)s->stack_cap * sizeof(uint64_t),
                 _Alignof(uint64_t));
    if (s->offsets != NULL)
        mem_free(a, s->offsets, (size_t)s->offsets_cap * sizeof(Int), _Alignof(Int));
    burrow__jsonbuf_free(&s->unquoted);
    burrow__jsonbuf_free(&s->rev);
    for (Int i = 0; i < s->spaces_cap; i++)
        jt_ns_release(&s->spaces[i], a);
    if (s->spaces != NULL)
        mem_free(a, s->spaces, (size_t)s->spaces_cap * sizeof(JsonNamespace),
                 _Alignof(JsonNamespace));
    memset(s, 0, sizeof(*s));
}

/* appendStackPointer. The names have to have been copied already. */
static void jt_append_stack_pointer(const JsonState *s, JsonBuf *b, int where) {
    Int object_depth = 0;
    for (Int i = 1; i < jt_depth(s); i++) {
        uint64_t e = jt_index(s, i);
        int64_t delta = -1;
        if (i == jt_depth(s) - 1) {
            if ((where < 0 && jt_e_len(e) == 0) ||
                (where == 0 && !jt_e_need_value(e)) || (where > 0 && jt_e_need_name(e)))
                return;
            if (where > 0 && jt_e_is_array(e))
                delta = 0;
        }
        if (jt_e_is_object(e)) {
            jsonbuf_byte(b, '/');
            jt_put_escaped_name(b, jt_names_get(s, object_depth));
            object_depth++;
        } else {
            jsonbuf_byte(b, '/');
            jt_put_uint(b, (uint64_t)(jt_e_len(e) + delta));
        }
    }
}

/* wrapWithObjectName and wrapWithArrayIndex. The suffix builds up in rev from
 * the innermost value out. */
static void jt_wrap_with_object_name(JsonState *s, const Byte *quoted, Int n) {
    JsonBuf scratch = jt_heap_buf();
    Str name = burrow__jsonwire_unquote_may_copy(quoted, n, false, &scratch);
    jsonbuf_byte(&s->rev, '/');
    jt_put_escaped_name(&s->rev, name);
    burrow__jsonbuf_free(&scratch);
}

static void jt_wrap_with_array_index(JsonState *s, int64_t idx) {
    jsonbuf_byte(&s->rev, '/');
    jt_put_uint(&s->rev, (uint64_t)idx);
}

/* pointerSuffixError.appendPointer: the segments of rev onto b, last first. */
static void jt_append_rev(JsonState *s, JsonBuf *b) {
    Int end = s->rev.len;
    while (end > 0) {
        Int i = end - 1;
        while (i > 0 && s->rev.p[i] != '/')
            i--;
        jsonbuf_put(b, s->rev.p + i, end - i);
        end = i;
    }
    s->rev.len = 0;
}

/* ------------------------------------------------------------------- tokens */

/* The fixed tokens' text. A buffer's pointer is not const because a decoder
 * writes into its own, but nothing writes through these. */
static const Byte jt_lit_null[] = "null";
static const Byte jt_lit_false[] = "false";
static const Byte jt_lit_true[] = "true";
static const Byte jt_lit_begin_object[] = "{";
static const Byte jt_lit_end_object[] = "}";
static const Byte jt_lit_begin_array[] = "[";
static const Byte jt_lit_end_array[] = "]";
static const Byte jt_lit_zero_string[] = "\"\"";
static const Byte jt_lit_zero_number[] = "0";

#define JT_RAW(lit)                                                                    \
    {(Byte *)(uintptr_t)(lit), (Int)sizeof(lit) - 1,                                   \
     (Int)sizeof(lit) - 1,     0,                                                      \
     (Int)sizeof(lit) - 1,     0}

static const burrow__JsontextBuffer jt_raw_null = JT_RAW(jt_lit_null);
static const burrow__JsontextBuffer jt_raw_false = JT_RAW(jt_lit_false);
static const burrow__JsontextBuffer jt_raw_true = JT_RAW(jt_lit_true);
static const burrow__JsontextBuffer jt_raw_begin_object = JT_RAW(jt_lit_begin_object);
static const burrow__JsontextBuffer jt_raw_end_object = JT_RAW(jt_lit_end_object);
static const burrow__JsontextBuffer jt_raw_begin_array = JT_RAW(jt_lit_begin_array);
static const burrow__JsontextBuffer jt_raw_end_array = JT_RAW(jt_lit_end_array);
static const burrow__JsontextBuffer jt_raw_zero_string = JT_RAW(jt_lit_zero_string);
static const burrow__JsontextBuffer jt_raw_zero_number = JT_RAW(jt_lit_zero_number);

const JsontextToken jsontext_null = {&jt_raw_null, {NULL, 0}, 0};
const JsontextToken jsontext_false = {&jt_raw_false, {NULL, 0}, 0};
const JsontextToken jsontext_true = {&jt_raw_true, {NULL, 0}, 0};
const JsontextToken jsontext_begin_object = {&jt_raw_begin_object, {NULL, 0}, 0};
const JsontextToken jsontext_end_object = {&jt_raw_end_object, {NULL, 0}, 0};
const JsontextToken jsontext_begin_array = {&jt_raw_begin_array, {NULL, 0}, 0};
const JsontextToken jsontext_end_array = {&jt_raw_end_array, {NULL, 0}, 0};

static const Type jt_token_desc = {
    {(const Byte *)"Token", 5},
    {(const Byte *)"jsontext", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsontextToken),
    (uint16_t)_Alignof(JsontextToken),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a74746bU, /* "jttk" */
    NULL,
};

const Type *const TYPE_JSONTEXT_TOKEN = &jt_token_desc;

static JsontextToken jt_raw_token(const burrow__JsontextBuffer *raw) {
    JsontextToken t = {raw, {NULL, 0}, 0};
    return t;
}

static JsontextToken jt_num_token(Str tag, uint64_t num) {
    JsontextToken t = {NULL, tag, num};
    return t;
}

JsontextToken jsontext_bool(bool b) {
    return b ? jsontext_true : jsontext_false;
}

JsontextToken jsontext_string(Str s) {
    if (s.len == 0)
        return jt_raw_token(&jt_raw_zero_string);
    JsontextToken t = {NULL, s, 0};
    return t;
}

static uint64_t jt_f64_bits(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static double jt_f64_from_bits(uint64_t u) {
    double f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static uint32_t jt_f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float jt_f32_from_bits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

JsontextToken jsontext_float(double n) {
    if (jt_f64_bits(n) == 0)
        return jt_raw_token(&jt_raw_zero_number);
    if (burrow__json_isnan(n))
        return jsontext_string(JT_LIT("NaN"));
    if (burrow__json_isinf(n))
        return jsontext_string(n > 0 ? JT_LIT("Infinity") : JT_LIT("-Infinity"));
    return jt_num_token(JT_LIT("f"), jt_f64_bits(n));
}

JsontextToken jsontext_float32(float n) {
    if (n != 0 && !burrow__json_isnan((double)n) && !burrow__json_isinf((double)n))
        return jt_num_token(JT_LIT("F"), jt_f32_bits(n));
    return jsontext_float((double)n);
}

JsontextToken jsontext_int(int64_t n) {
    if (n == 0)
        return jt_raw_token(&jt_raw_zero_number);
    return jt_num_token(JT_LIT("i"), (uint64_t)n);
}

JsontextToken jsontext_uint(uint64_t n) {
    if (n == 0)
        return jt_raw_token(&jt_raw_zero_number);
    return jt_num_token(JT_LIT("u"), n);
}

/* The raw buffer's bytes for the token, after checking that the decoder has
 * not moved on since. */
static Str jt_token_raw_bytes(JsontextToken t) {
    const burrow__JsontextBuffer *raw = t.raw;
    if ((uint64_t)(raw->base_offset + raw->prev_start) != t.num)
        panic_str(JT_LIT("invalid jsontext.Token; it has been voided by a subsequent "
                         "json.Decoder call"));
    return str_from_bytes(raw->buf + raw->prev_start, raw->prev_end - raw->prev_start);
}

static bool jt_is_const_raw(const burrow__JsontextBuffer *raw) {
    return raw == &jt_raw_null || raw == &jt_raw_false || raw == &jt_raw_true ||
           raw == &jt_raw_begin_object || raw == &jt_raw_end_object ||
           raw == &jt_raw_begin_array || raw == &jt_raw_end_array;
}

JsontextToken jsontext_token_clone(JsontextToken t, Alloc *a) {
    const burrow__JsontextBuffer *raw = t.raw;
    if (raw == NULL)
        return t;
    if (raw->prev_start == 0 && jt_is_const_raw(raw))
        return t;
    Str b = jt_token_raw_bytes(t);
    burrow__JsontextBuffer *c = (burrow__JsontextBuffer *)mem_alloc_nozero(
        a, sizeof(burrow__JsontextBuffer) + (size_t)b.len,
        _Alignof(burrow__JsontextBuffer));
    if (c == NULL) {
        JsontextToken zero = {NULL, {NULL, 0}, 0};
        return zero;
    }
    c->buf = (Byte *)(c + 1);
    memcpy(c->buf, b.p, (size_t)b.len);
    c->len = b.len;
    c->cap = b.len;
    c->prev_start = 0;
    c->prev_end = b.len;
    c->base_offset = 0;
    return jt_raw_token(c);
}

void jsontext_token_free(JsontextToken t, Alloc *a) {
    const burrow__JsontextBuffer *raw = t.raw;
    if (raw == NULL || raw->buf != (const Byte *)(raw + 1))
        return;
    mem_free(a, (void *)(uintptr_t)raw,
             sizeof(burrow__JsontextBuffer) + (size_t)raw->cap,
             _Alignof(burrow__JsontextBuffer));
}

JsontextKind jsontext_token_kind(JsontextToken t) {
    if (t.raw != NULL) {
        Str b = jt_token_raw_bytes(t);
        return jt_norm(b.p[0]);
    }
    if (t.num != 0)
        return '0';
    if (t.str.len != 0)
        return '"';
    return 0;
}

bool jsontext_token_bool(JsontextToken t) {
    if (t.raw == &jt_raw_true)
        return true;
    if (t.raw == &jt_raw_false)
        return false;
    jt_panic_kind(jsontext_token_kind(t));
}

/* Token.String into b. */
static void jt_token_text(JsontextToken t, JsonBuf *b) {
    if (t.raw != NULL) {
        Str raw = jt_token_raw_bytes(t);
        if (raw.p[0] == '"') {
            bool verbatim = jsonwire_consume_simple_string(raw.p, raw.len) == raw.len;
            if (verbatim)
                jsonbuf_put(b, raw.p + 1, raw.len - 2);
            else
                (void)burrow__jsonwire_append_unquote(b, raw.p, raw.len);
            return;
        }
        jsonbuf_str(b, raw);
        return;
    }
    if (t.str.len != 0 && t.num == 0) {
        jsonbuf_str(b, t.str);
        return;
    }
    if (t.num > 0) {
        switch (t.str.p[0]) {
        case 'F':
            burrow__jsonwire_append_float(b, (double)jt_f32_from_bits((uint32_t)t.num),
                                          32);
            return;
        case 'f':
            burrow__jsonwire_append_float(b, jt_f64_from_bits(t.num), 64);
            return;
        case 'i':
            jt_put_int(b, (int64_t)t.num);
            return;
        case 'u':
            jt_put_uint(b, t.num);
            return;
        default:
            break;
        }
    }
    jsonbuf_str(b, JT_LIT("<invalid jsontext.Token>"));
}

Str jsontext_token_string(JsontextToken t, Alloc *a) {
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jt_token_text(t, &b);
    return jt_buf_str_or_empty(&b);
}

/* appendString: the token as a JSON string. */
static Error jt_token_append_string(JsontextToken t, JsonBuf *dst,
                                    const JsontextOptions *flags) {
    if (t.raw != NULL) {
        const burrow__JsontextBuffer *raw = t.raw;
        const Byte *p = raw->buf + raw->prev_start;
        Int n = raw->prev_end - raw->prev_start;
        if (p[0] == '"') {
            if (jsonwire_consume_simple_string(p, n) == n) {
                jsonbuf_put(dst, p, n);
                return BURROW_NO_ERROR;
            }
            Error err = BURROW_NO_ERROR;
            (void)burrow__jsonwire_reformat_string(dst, p, n, flags, &err);
            return err;
        }
    } else if (t.str.len != 0 && t.num == 0) {
        return burrow__jsonwire_append_quote(dst, t.str.p, t.str.len, flags);
    }
    jt_panic_kind(jsontext_token_kind(t));
}

/* appendNumber: the token as a JSON number. */
static Error jt_token_append_number(JsontextToken t, JsonBuf *dst,
                                    const JsontextOptions *flags) {
    if (t.raw != NULL) {
        const burrow__JsontextBuffer *raw = t.raw;
        const Byte *p = raw->buf + raw->prev_start;
        Int n = raw->prev_end - raw->prev_start;
        if (jt_norm(p[0]) == '0') {
            Error err = BURROW_NO_ERROR;
            (void)burrow__jsonwire_reformat_number(dst, p, n, flags, &err);
            return err;
        }
    } else if (t.num != 0) {
        switch (t.str.p[0]) {
        case 'F':
            burrow__jsonwire_append_float(
                dst, (double)jt_f32_from_bits((uint32_t)t.num), 32);
            return BURROW_NO_ERROR;
        case 'f':
            burrow__jsonwire_append_float(dst, jt_f64_from_bits(t.num), 64);
            return BURROW_NO_ERROR;
        case 'i':
            jt_put_int(dst, (int64_t)t.num);
            return BURROW_NO_ERROR;
        case 'u':
            jt_put_uint(dst, t.num);
            return BURROW_NO_ERROR;
        default:
            break;
        }
    }
    jt_panic_kind(jsontext_token_kind(t));
}

/* &numError{accessor, t.String(), err}. */
static Error jt_num_error(JsontextToken t, Str accessor, Error err) {
    JsonBuf v = jt_heap_buf();
    jt_token_text(t, &v);
    Str parts[6] = {JT_LIT("jsontext.Token("),
                    str_from_bytes(v.p, v.len),
                    JT_LIT(")."),
                    accessor,
                    JT_LIT(" error: "),
                    error_text(err)};
    Error made = jt_wrap_build(&jt_num_error_vt, parts, 6, err);
    burrow__jsonbuf_free(&v);
    return made;
}

static inline void jt_set_err(Error *err, Error e) {
    if (err != NULL)
        *err = e;
}

static int64_t jt_f64toi64(double f) {
    if (burrow__json_isnan(f))
        return 0;
    if (f >= 9223372036854775808.0)
        return INT64_MAX;
    if (f < -9223372036854775808.0)
        return INT64_MIN;
    return (int64_t)f;
}

static uint64_t jt_f64tou64(double f) {
    if (burrow__json_isnan(f))
        return 0;
    if (f >= 18446744073709551616.0)
        return UINT64_MAX;
    if (f < 0)
        return 0;
    return (uint64_t)f;
}

static double jt_token_float_bits(JsontextToken t, int bits, Error *err) {
    jt_set_err(err, BURROW_NO_ERROR);
    if (t.raw != NULL) {
        Str b = jt_token_raw_bytes(t);
        if (jt_norm(b.p[0]) == '0') {
            Error perr = BURROW_NO_ERROR;
            double fv = strconv_parse_float(b, bits, &perr);
            if (BURROW_FAILED(perr))
                jt_set_err(err, jt_num_error(t, JT_LIT("Float"), strconv_err_range));
            return fv;
        }
    } else if (t.num != 0) {
        switch (t.str.p[0]) {
        case 'F':
            return (double)jt_f32_from_bits((uint32_t)t.num);
        case 'f': {
            double f = jt_f64_from_bits(t.num);
            if (bits == 32 && !burrow__json_isinf(f) &&
                burrow__json_isinf((double)(float)f))
                jt_set_err(err, jt_num_error(t, JT_LIT("Float"), strconv_err_range));
            return f;
        }
        case 'i':
            return (double)(int64_t)t.num;
        case 'u':
            return (double)t.num;
        default:
            break;
        }
    }
    if (jsontext_token_kind(t) == '"') {
        JsonBuf s = jt_heap_buf();
        jt_token_text(t, &s);
        Str v = str_from_bytes(s.p, s.len);
        double out = 0;
        bool ok = true;
        if (str_eq(v, JT_LIT("NaN")))
            out = math_nan();
        else if (str_eq(v, JT_LIT("Infinity")))
            out = (double)INFINITY;
        else if (str_eq(v, JT_LIT("-Infinity")))
            out = -(double)INFINITY;
        else
            ok = false;
        burrow__jsonbuf_free(&s);
        if (ok)
            return out;
    }
    jt_panic_kind(jsontext_token_kind(t));
}

double jsontext_token_float(JsontextToken t, Error *err) {
    return jt_token_float_bits(t, 64, err);
}

float jsontext_token_float32(JsontextToken t, Error *err) {
    return (float)jt_token_float_bits(t, 32, err);
}

int64_t jsontext_token_int(JsontextToken t, Error *err) {
    jt_set_err(err, BURROW_NO_ERROR);
    if (t.raw != NULL) {
        Str b = jt_token_raw_bytes(t);
        uint64_t abs;
        bool ok;
        if (b.len > 0 && b.p[0] == '-') {
            ok = burrow__jsonwire_parse_uint(b.p + 1, b.len - 1, &abs);
            if (abs > (uint64_t)INT64_MAX + 1) {
                jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_range));
                return INT64_MIN;
            }
            if (ok)
                return (int64_t)(0 - abs);
        } else {
            ok = burrow__jsonwire_parse_uint(b.p, b.len, &abs);
            if (abs > (uint64_t)INT64_MAX) {
                jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_range));
                return INT64_MAX;
            }
            if (ok)
                return (int64_t)abs;
        }
        if (jt_norm(b.p[0]) == '0') {
            double f = strconv_parse_float(b, 64, NULL);
            jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_syntax));
            return jt_f64toi64(f);
        }
    } else if (t.num != 0) {
        switch (t.str.p[0]) {
        case 'i':
            return (int64_t)t.num;
        case 'u':
            if (t.num > (uint64_t)INT64_MAX) {
                jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_range));
                return INT64_MAX;
            }
            return (int64_t)t.num;
        case 'f':
        case 'F': {
            double f = t.str.p[0] == 'F' ? (double)jt_f32_from_bits((uint32_t)t.num)
                                         : jt_f64_from_bits(t.num);
            int64_t i = jt_f64toi64(f);
            if (!burrow__json_is_integral(f))
                jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_syntax));
            else if ((i == INT64_MIN && f < -9223372036854775808.0) ||
                     (i == INT64_MAX && f > 9223372036854775807.0))
                jt_set_err(err, jt_num_error(t, JT_LIT("Int"), strconv_err_range));
            return i;
        }
        default:
            break;
        }
    }
    jt_panic_kind(jsontext_token_kind(t));
}

uint64_t jsontext_token_uint(JsontextToken t, Error *err) {
    jt_set_err(err, BURROW_NO_ERROR);
    if (t.raw != NULL) {
        Str b = jt_token_raw_bytes(t);
        uint64_t abs;
        bool ok = burrow__jsonwire_parse_uint(b.p, b.len, &abs);
        if (ok)
            return abs;
        if (abs == UINT64_MAX) {
            jt_set_err(err, jt_num_error(t, JT_LIT("Uint"), strconv_err_range));
            return UINT64_MAX;
        }
        if (jt_norm(b.p[0]) == '0') {
            double f = strconv_parse_float(b, 64, NULL);
            jt_set_err(err, jt_num_error(t, JT_LIT("Uint"), strconv_err_syntax));
            return jt_f64tou64(f);
        }
    } else if (t.num != 0) {
        switch (t.str.p[0]) {
        case 'u':
            return t.num;
        case 'i':
            if ((int64_t)t.num < 0) {
                jt_set_err(err, jt_num_error(t, JT_LIT("Uint"), strconv_err_syntax));
                return 0;
            }
            return t.num;
        case 'f':
        case 'F': {
            double f = t.str.p[0] == 'F' ? (double)jt_f32_from_bits((uint32_t)t.num)
                                         : jt_f64_from_bits(t.num);
            uint64_t u = jt_f64tou64(f);
            if (!burrow__json_is_integral(f) || (jt_f64_bits(f) >> 63) != 0)
                jt_set_err(err, jt_num_error(t, JT_LIT("Uint"), strconv_err_syntax));
            else if ((u == 0 && f < 0) ||
                     (u == UINT64_MAX && f > 18446744073709551615.0))
                jt_set_err(err, jt_num_error(t, JT_LIT("Uint"), strconv_err_range));
            return u;
        }
        default:
            break;
        }
    }
    jt_panic_kind(jsontext_token_kind(t));
}

/* ----------------------------------------------------- wrapSyntacticError */

/* wrapSyntacticError for either coder: buf is the coder's buffer, base what
 * offset its first byte is at. The pointer suffix collected in s->rev is
 * used up whatever happens. */
static Error jt_wrap_syntactic(JsonState *s, const JsontextOptions *opts, int64_t base,
                               Byte *buf, Int blen, bool decoder, Error err, Int pos,
                               int where) {
    if (jt_is(err, burrow_err_out_of_memory)) {
        s->rev.len = 0;
        s->rev.failed = false;
        return err;
    }
    if (s->rev.len == 0 && (jt_is(err, io_eof) || jt_is_io_error(err)))
        return err;
    int64_t offset = base + pos;
    jt_names_copy_quoted(s, buf, blen);
    JsonBuf ptr = jt_heap_buf();
    jt_append_stack_pointer(s, &ptr, where);
    jt_append_rev(s, &ptr);
    bool failed = ptr.failed || s->rev.failed || s->unquoted.failed;
    s->rev.failed = false;
    if (failed) {
        burrow__jsonbuf_free(&ptr);
        return burrow_err_out_of_memory;
    }
    Str p = str_from_bytes(ptr.p, ptr.len);
    if (decoder && jt_is(err, jt_err_mismatch_delim)) {
        Str w = JT_LIT("at start of value");
        if (s->stack_len > 0 && jt_e_len(s->last) > 0) {
            if (jt_e_is_array(s->last)) {
                w = JT_LIT("after array element (expecting ',' or ']')");
                p = jt_pointer_parent_raw(p);
            } else {
                w = JT_LIT("after object value (expecting ',' or '}')");
                p = jt_pointer_parent_raw(p);
            }
        }
        err = burrow__jsonwire_new_invalid_character_error(buf + pos, blen - pos, w);
    }
    if (jsonflags_get(opts, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS) &&
        !jt_is(err, io_err_unexpected_eof)) {
        if (err.vt == &burrow__jsonwire_invalid_text_error_vt && err.data != NULL)
            offset += ((const JsonwireInvalidTextError *)err.data)->what.len;
        else
            offset++;
    }
    Error out = jt_syntactic_new(offset, p, err);
    burrow__jsonbuf_free(&ptr);
    return out;
}

/* ------------------------------------------------------------------ frames */

/* One open object or array while a whole value is read or reformatted. wrap
 * says whether an error found now belongs under this level's name or index
 * in the JSON Pointer. */
typedef struct JtFrame {
    int64_t name_at;
    Int name_len;
    int64_t idx;
    Int out_start;
    Int names_mark;
    bool object;
    bool wrap;
} JtFrame;

static JtFrame *jt_frame_push(Alloc *a, void **frames, Int *cap, Int *n) {
    void *p = *frames;
    if (!jt_grow_array(a, &p, cap, *n, sizeof(JtFrame), _Alignof(JtFrame)))
        return NULL;
    *frames = p;
    JtFrame *f = (JtFrame *)p + (*n)++;
    memset(f, 0, sizeof(*f));
    return f;
}

static void jt_frames_free(Alloc *a, void **frames, Int *cap) {
    if (*frames != NULL)
        mem_free(a, *frames, (size_t)*cap * sizeof(JtFrame), _Alignof(JtFrame));
    *frames = NULL;
    *cap = 0;
}

/* Room for the name and namespace entries of one more object, so pushing
 * them after the state machine has moved on cannot fail. */
static bool jt_reserve_object(JsonState *s, bool ns) {
    if (!jt_names_push(s))
        return false;
    jt_names_pop(s);
    if (ns) {
        if (!jt_spaces_push(s))
            return false;
        jt_spaces_pop(s);
    }
    return true;
}

/* ------------------------------------------------------------------ decoder */

BURROW_SENTINEL_ERROR(
    jt_err_buffer_write_after_next,
    "invalid bytes.Buffer.Write call after calling bytes.Buffer.Next");

static const Type jt_decoder_desc = {
    {(const Byte *)"Decoder", 7},
    {(const Byte *)"jsontext", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsontextDecoder),
    (uint16_t)_Alignof(JsontextDecoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a746463U, /* "jtdc" */
    NULL,
};

const Type *const TYPE_JSONTEXT_DECODER = &jt_decoder_desc;

static bool jt_is_bytes_buffer(IoReader r) {
    return r.vt != NULL && r.vt->self_type == TYPE_BYTES_BUFFER;
}

static Error jt_dwrap(JsontextDecoder *d, Error err, Int pos, int where) {
    return jt_wrap_syntactic(&d->st, &d->opts, d->db.base_offset, d->db.buf, d->db.len,
                             true, err, pos, where);
}

/* What an empty decoder buffer points at. It stands in for NULL because the
 * scanning code does buf + pos arithmetic even when len is 0. Nothing writes
 * through it: the decoder only writes a buffer it owns. */
static const Byte jt_no_bytes[1] = {0};
#define JT_NO_BUF ((Byte *)(uintptr_t)jt_no_bytes)

static void jt_decoder_drop_buf(JsontextDecoder *d) {
    if (d->owns_buf && d->db.buf != NULL)
        mem_free(d->a, d->db.buf, (size_t)d->db.cap, 1);
    d->db.buf = JT_NO_BUF;
    d->db.len = 0;
    d->db.cap = 0;
    d->owns_buf = false;
}

/* decoderState.fetch. */
static Error jt_fetch(JsontextDecoder *d) {
    burrow__JsontextBuffer *b = &d->db;
    if (!d->has_rd)
        return io_err_unexpected_eof;
    jt_names_copy_quoted(&d->st, b->buf, b->len);
    if (d->st.unquoted.failed)
        return burrow_err_out_of_memory;
    if (jt_is_bytes_buffer(d->rd)) {
        BytesBuffer *bb = (BytesBuffer *)d->rd.data;
        Int n = bytes_buffer_len(bb);
        if (n == 0)
            return io_err_unexpected_eof;
        if (b->len == 0) {
            jt_decoder_drop_buf(d);
            Slice s = bytes_buffer_next(bb, n);
            b->buf = (Byte *)s.p;
            b->len = s.len;
            b->cap = s.len;
            return BURROW_NO_ERROR;
        }
        return jt_io_error(JT_LIT("read"), jt_err_buffer_write_after_next);
    }
    if (b->cap == 0) {
        Byte *p = (Byte *)mem_alloc_nozero(d->a, 64, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        jt_decoder_drop_buf(d);
        b->buf = p;
        b->cap = 64;
        d->owns_buf = true;
    }
    bool grow = b->cap <= 2048 && (int64_t)b->cap < (b->base_offset + b->prev_end) / 2;
    grow = grow || (b->prev_start == 0 && b->len >= 3 * b->cap / 4);
    Int keep = b->len - b->prev_start;
    if (grow || !d->owns_buf) {
        Int ncap = b->cap * 2;
        Byte *p = (Byte *)mem_alloc_nozero(d->a, (size_t)ncap, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        if (keep > 0)
            memcpy(p, b->buf + b->prev_start, (size_t)keep);
        jt_decoder_drop_buf(d);
        b->buf = p;
        b->cap = ncap;
        d->owns_buf = true;
    } else if (keep > 0) {
        memmove(b->buf, b->buf + b->prev_start, (size_t)keep);
    }
    b->len = keep;
    b->base_offset += b->prev_start;
    b->prev_end -= b->prev_start;
    b->prev_start = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Slice room = {b->buf + b->len, b->cap - b->len, b->cap - b->len, TYPE_BYTE};
        Int n = d->rd.vt->read(d->rd.data, room, &err);
        if (n > 0) {
            b->len += n;
            return BURROW_NO_ERROR;
        }
        if (jt_is(err, io_eof))
            return io_err_unexpected_eof;
        if (BURROW_FAILED(err))
            return jt_io_error(JT_LIT("read"), err);
    }
}

static void jt_invalidate_previous_read(JsontextDecoder *d) {
    burrow__JsontextBuffer *b = &d->db;
    if (d->has_rd && !jt_is_bytes_buffer(d->rd) && b->prev_start < b->prev_end &&
        b->prev_start < b->len) {
        b->buf[b->prev_start] = JT_INVALIDATE_BYTE;
        b->prev_start = b->prev_end;
    }
}

static Error jt_consume_ws(JsontextDecoder *d, Int *pos) {
    for (;;) {
        *pos += jsonwire_consume_whitespace(d->db.buf + *pos, d->db.len - *pos);
        if (*pos == d->db.len) {
            int64_t abs = d->db.base_offset + *pos;
            Error err = jt_fetch(d);
            *pos = (Int)(abs - d->db.base_offset);
            if (BURROW_FAILED(err))
                return err;
            continue;
        }
        return BURROW_NO_ERROR;
    }
}

/* Whitespace, and then whatever else it takes to have something at pos. */
static inline Error jt_skip_ws(JsontextDecoder *d, Int *pos) {
    *pos += jsonwire_consume_whitespace(d->db.buf + *pos, d->db.len - *pos);
    if (*pos == d->db.len)
        return jt_consume_ws(d, pos);
    return BURROW_NO_ERROR;
}

static Error jt_check_delim(JsontextDecoder *d, Byte delim, JsontextKind next) {
    Str where = JT_LIT("at start of value");
    Byte need = jt_need_delim(&d->st, next);
    if (need == delim)
        return BURROW_NO_ERROR;
    if (need == ':')
        where = JT_LIT("after object name (expecting ':')");
    else if (need == ',')
        where = jt_e_is_object(d->st.last)
                    ? JT_LIT("after object value (expecting ',' or '}')")
                    : JT_LIT("after array element (expecting ',' or ']')");
    Int pos = d->db.prev_end;
    pos += jsonwire_consume_whitespace(d->db.buf + pos, d->db.len - pos);
    Error err = burrow__jsonwire_new_invalid_character_error(d->db.buf + pos,
                                                             d->db.len - pos, where);
    return jt_dwrap(d, err, pos, 0);
}

static Error jt_check_delim_before_io_error(JsontextDecoder *d, Byte delim, Error err) {
    if (jt_need_delim(&d->st, '"') != delim)
        err = jt_check_delim(d, delim, '"');
    return err;
}

/* The start PeekKind, ReadToken and ReadValue share: skip to the next token
 * and check the delimiter in front of it. */
static Error jt_advance(JsontextDecoder *d, Int *out) {
    jt_invalidate_previous_read(d);
    Int pos = d->db.prev_end;
    pos += jsonwire_consume_whitespace(d->db.buf + pos, d->db.len - pos);
    if (pos == d->db.len) {
        Error err = jt_consume_ws(d, &pos);
        if (BURROW_FAILED(err)) {
            if (jt_is(err, io_err_unexpected_eof) && jt_depth(&d->st) == 1)
                err = io_eof;
            return jt_dwrap(d, err, pos, 0);
        }
    }
    Byte delim = 0;
    Byte c = d->db.buf[pos];
    if (c == ':' || c == ',') {
        delim = c;
        pos++;
        pos += jsonwire_consume_whitespace(d->db.buf + pos, d->db.len - pos);
        if (pos == d->db.len) {
            Error err = jt_consume_ws(d, &pos);
            if (BURROW_FAILED(err)) {
                err = jt_dwrap(d, err, pos, 0);
                return jt_check_delim_before_io_error(d, delim, err);
            }
        }
    }
    JsontextKind next = jt_norm(d->db.buf[pos]);
    if (jt_need_delim(&d->st, next) != delim)
        return jt_check_delim(d, delim, next);
    *out = pos;
    return BURROW_NO_ERROR;
}

/* The start of ReadToken and ReadValue: the peeked token if there is one. */
static Error jt_next(JsontextDecoder *d, Int *pos, JsontextKind *next) {
    if (d->peek_pos != 0) {
        if (BURROW_FAILED(d->peek_err)) {
            Error err = d->peek_err;
            d->peek_pos = 0;
            d->peek_err = BURROW_NO_ERROR;
            return err;
        }
        *pos = d->peek_pos;
        *next = jt_norm(d->db.buf[*pos]);
        d->peek_pos = 0;
        return BURROW_NO_ERROR;
    }
    Error err = jt_advance(d, pos);
    if (BURROW_FAILED(err))
        return err;
    *next = jt_norm(d->db.buf[*pos]);
    return BURROW_NO_ERROR;
}

JsontextKind jsontext_decoder_peek_kind(JsontextDecoder *d) {
    if (d->peek_pos > 0)
        return jt_norm(d->db.buf[d->peek_pos]);
    Int pos = 0;
    Error err = jt_advance(d, &pos);
    if (BURROW_FAILED(err)) {
        d->peek_pos = -1;
        d->peek_err = err;
        return 0;
    }
    d->peek_pos = pos;
    d->peek_err = BURROW_NO_ERROR;
    return jt_norm(d->db.buf[pos]);
}

static Error jt_consume_literal(JsontextDecoder *d, Int *pos, Str lit) {
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = burrow__jsonwire_consume_literal(d->db.buf + *pos, d->db.len - *pos,
                                                 lit, &err);
        if (jt_is(err, io_err_unexpected_eof)) {
            int64_t abs = d->db.base_offset + *pos;
            err = jt_fetch(d);
            *pos = (Int)(abs - d->db.base_offset);
            if (BURROW_FAILED(err)) {
                *pos += n;
                return err;
            }
            continue;
        }
        *pos += n;
        return err;
    }
}

static Error jt_consume_string(JsontextDecoder *d, unsigned *flags, Int *pos) {
    bool validate = !jsonflags_get(&d->opts, JSONFLAG_ALLOW_INVALID_UTF8);
    Int n = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        n = burrow__jsonwire_consume_string_resumable(
            flags, d->db.buf + *pos, d->db.len - *pos, n, validate, &err);
        if (jt_is(err, io_err_unexpected_eof)) {
            int64_t abs = d->db.base_offset + *pos;
            err = jt_fetch(d);
            *pos = (Int)(abs - d->db.base_offset);
            if (BURROW_FAILED(err)) {
                *pos += n;
                return err;
            }
            continue;
        }
        *pos += n;
        return err;
    }
}

static Error jt_consume_number(JsontextDecoder *d, Int *pos) {
    Int n = 0;
    int state = JSONWIRE_NUMBER_INIT;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        n = burrow__jsonwire_consume_number_resumable(
            d->db.buf + *pos, d->db.len - *pos, n, &state, &err);
        if (jt_is(err, io_err_unexpected_eof) || *pos + n == d->db.len) {
            bool may_terminate = BURROW_OK(err);
            int64_t abs = d->db.base_offset + *pos;
            err = jt_fetch(d);
            *pos = (Int)(abs - d->db.base_offset);
            if (BURROW_FAILED(err)) {
                if (may_terminate && jt_is(err, io_err_unexpected_eof)) {
                    *pos += n;
                    return BURROW_NO_ERROR;
                }
                return err;
            }
            continue;
        }
        *pos += n;
        return err;
    }
}

/* consumeValue, consumeObject and consumeArray as one loop over an explicit
 * stack of open objects and arrays. */
static Error jt_consume_value(JsontextDecoder *d, unsigned *flags, Int *posp,
                              Int depth) {
    JsonState *s = &d->st;
    bool dup_check = !jsonflags_get(&d->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES);
    Int spaces_mark = s->spaces_len;
    Int nframes = 0;
    Int pos = *posp;
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    JtFrame *f = NULL;
    JsontextKind next = 0;
    s->rev.len = 0;

value:
    for (;;) {
        Error e = BURROW_NO_ERROR;
        next = jt_norm(d->db.buf[pos]);
        switch (next) {
        case 'n':
            n = jsonwire_consume_null(d->db.buf + pos, d->db.len - pos);
            if (n == 0)
                n = burrow__jsonwire_consume_literal(d->db.buf + pos, d->db.len - pos,
                                                     JT_LIT("null"), &e);
            break;
        case 'f':
            n = jsonwire_consume_false(d->db.buf + pos, d->db.len - pos);
            if (n == 0)
                n = burrow__jsonwire_consume_literal(d->db.buf + pos, d->db.len - pos,
                                                     JT_LIT("false"), &e);
            break;
        case 't':
            n = jsonwire_consume_true(d->db.buf + pos, d->db.len - pos);
            if (n == 0)
                n = burrow__jsonwire_consume_literal(d->db.buf + pos, d->db.len - pos,
                                                     JT_LIT("true"), &e);
            break;
        case '"':
            n = jsonwire_consume_simple_string(d->db.buf + pos, d->db.len - pos);
            if (n == 0) {
                err = jt_consume_string(d, flags, &pos);
                if (BURROW_FAILED(err))
                    goto fail;
                goto after;
            }
            break;
        case '0':
            n = jsonwire_consume_simple_number(d->db.buf + pos, d->db.len - pos);
            if (n == 0 || pos + n == d->db.len) {
                err = jt_consume_number(d, &pos);
                if (BURROW_FAILED(err))
                    goto fail;
                goto after;
            }
            break;
        case '{':
            goto open_object;
        case '[':
            goto open_array;
        default:
            if ((jt_e_is_object(s->last) && next == ']') ||
                (jt_e_is_array(s->last) && next == '}'))
                err = jt_err_mismatch_delim;
            else
                err = burrow__jsonwire_new_invalid_character_error(
                    d->db.buf + pos, d->db.len - pos, JT_LIT("at start of value"));
            goto fail;
        }
        if (jt_is(e, io_err_unexpected_eof)) {
            int64_t abs = d->db.base_offset + pos;
            e = jt_fetch(d);
            pos = (Int)(abs - d->db.base_offset);
            if (BURROW_FAILED(e)) {
                pos += n;
                err = e;
                goto fail;
            }
            continue;
        }
        pos += n;
        if (BURROW_FAILED(e)) {
            err = e;
            goto fail;
        }
        goto after;
    }

open_object:
    if (dup_check && !jt_spaces_push(s)) {
        err = burrow_err_out_of_memory;
        goto fail;
    }
    if (depth == JT_MAX_NESTING_DEPTH + 1) {
        err = jt_err_max_depth;
        goto fail;
    }
    pos++;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    if (d->db.buf[pos] == '}') {
        pos++;
        if (dup_check)
            jt_spaces_pop(s);
        goto after;
    }
    depth++;
    f = jt_frame_push(d->a, &d->frames, &d->frames_cap, &nframes);
    if (f == NULL) {
        err = burrow_err_out_of_memory;
        goto fail;
    }
    f->object = true;

member:
    f = (JtFrame *)d->frames + nframes - 1;
    f->wrap = false;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    {
        unsigned flags2 = 0;
        n = jsonwire_consume_simple_string(d->db.buf + pos, d->db.len - pos);
        if (n == 0) {
            int64_t old = d->db.base_offset + pos;
            err = jt_consume_string(d, &flags2, &pos);
            n = (Int)(d->db.base_offset + pos - old);
            *flags |= flags2;
            if (BURROW_FAILED(err))
                goto fail;
        } else {
            pos += n;
        }
        f = (JtFrame *)d->frames + nframes - 1;
        f->name_at = d->db.base_offset + pos - n;
        f->name_len = n;
        if (dup_check) {
            int r = jt_ns_insert_quoted(jt_spaces_last(s), s->a, d->db.buf + pos - n, n,
                                        (flags2 & JSONWIRE_STRING_NON_VERBATIM) == 0);
            if (r < 0) {
                err = burrow_err_out_of_memory;
                goto fail;
            }
            if (r == 0) {
                f->wrap = true;
                pos -= n;
                err = jsontext_err_duplicate_name;
                goto fail;
            }
        }
    }
    f->wrap = true;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    if (d->db.buf[pos] != ':') {
        err = burrow__jsonwire_new_invalid_character_error(
            d->db.buf + pos, d->db.len - pos,
            JT_LIT("after object name (expecting ':')"));
        goto fail;
    }
    pos++;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    goto value;

open_array:
    if (depth == JT_MAX_NESTING_DEPTH + 1) {
        err = jt_err_max_depth;
        goto fail;
    }
    pos++;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    if (d->db.buf[pos] == ']') {
        pos++;
        goto after;
    }
    depth++;
    f = jt_frame_push(d->a, &d->frames, &d->frames_cap, &nframes);
    if (f == NULL) {
        err = burrow_err_out_of_memory;
        goto fail;
    }

element:
    f = (JtFrame *)d->frames + nframes - 1;
    f->wrap = false;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    f->wrap = true;
    goto value;

after:
    if (nframes == 0) {
        *posp = pos;
        return BURROW_NO_ERROR;
    }
    f = (JtFrame *)d->frames + nframes - 1;
    f->wrap = false;
    err = jt_skip_ws(d, &pos);
    if (BURROW_FAILED(err))
        goto fail;
    if (f->object) {
        switch (d->db.buf[pos]) {
        case ',':
            pos++;
            goto member;
        case '}':
            pos++;
            nframes--;
            depth--;
            if (dup_check)
                jt_spaces_pop(s);
            goto after;
        default:
            err = burrow__jsonwire_new_invalid_character_error(
                d->db.buf + pos, d->db.len - pos,
                JT_LIT("after object value (expecting ',' or '}')"));
            goto fail;
        }
    }
    switch (d->db.buf[pos]) {
    case ',':
        pos++;
        f->idx++;
        goto element;
    case ']':
        pos++;
        nframes--;
        depth--;
        goto after;
    default:
        err = burrow__jsonwire_new_invalid_character_error(
            d->db.buf + pos, d->db.len - pos,
            JT_LIT("after array element (expecting ',' or ']')"));
        goto fail;
    }

fail:
    for (Int i = nframes - 1; i >= 0; i--) {
        const JtFrame *g = (const JtFrame *)d->frames + i;
        if (!g->wrap)
            continue;
        if (g->object)
            jt_wrap_with_object_name(s, d->db.buf + (g->name_at - d->db.base_offset),
                                     g->name_len);
        else
            jt_wrap_with_array_index(s, g->idx);
    }
    s->spaces_len = spaces_mark;
    *posp = pos;
    return err;
}

static JsontextToken jt_zero_token(void) {
    JsontextToken t = {NULL, {NULL, 0}, 0};
    return t;
}

static JsontextToken jt_read_literal(JsontextDecoder *d, Int pos, Str lit,
                                     JsontextToken tok, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (burrow__jsonwire_consume_literal(d->db.buf + pos, d->db.len - pos, lit, &e) ==
            lit.len &&
        BURROW_OK(e)) {
        pos += lit.len;
    } else {
        e = jt_consume_literal(d, &pos, lit);
        if (BURROW_FAILED(e)) {
            jt_set_err(err, jt_dwrap(d, e, pos, 1));
            return jt_zero_token();
        }
    }
    e = jt_append_literal(&d->st);
    if (BURROW_FAILED(e)) {
        jt_set_err(err, jt_dwrap(d, e, pos - lit.len, 1));
        return jt_zero_token();
    }
    d->db.prev_start = pos;
    d->db.prev_end = pos;
    return tok;
}

static JsontextToken jt_raw_decoder_token(JsontextDecoder *d) {
    JsontextToken t = {
        &d->db, {NULL, 0}, (uint64_t)(d->db.base_offset + d->db.prev_start)};
    return t;
}

JsontextToken jsontext_decoder_read_token(JsontextDecoder *d, Error *err) {
    JsonState *s = &d->st;
    Int pos = 0;
    JsontextKind next = 0;
    Error e = jt_next(d, &pos, &next);
    jt_set_err(err, e);
    if (BURROW_FAILED(e))
        return jt_zero_token();
    Int n;
    switch (next) {
    case 'n':
        return jt_read_literal(d, pos, JT_LIT("null"), jsontext_null, err);
    case 'f':
        return jt_read_literal(d, pos, JT_LIT("false"), jsontext_false, err);
    case 't':
        return jt_read_literal(d, pos, JT_LIT("true"), jsontext_true, err);
    case '"': {
        unsigned flags = 0;
        n = jsonwire_consume_simple_string(d->db.buf + pos, d->db.len - pos);
        if (n == 0) {
            int64_t old = d->db.base_offset + pos;
            s->rev.len = 0;
            e = jt_consume_string(d, &flags, &pos);
            n = (Int)(d->db.base_offset + pos - old);
            if (BURROW_FAILED(e))
                break;
        } else {
            pos += n;
        }
        if (jt_e_need_name(s->last)) {
            if (!jsonflags_get(&d->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
                if (!jt_e_valid_ns(s->last)) {
                    e = jt_err_invalid_namespace;
                    pos -= n;
                    break;
                }
                if (jt_e_active_ns(s->last)) {
                    int r = jt_ns_insert_quoted(
                        jt_spaces_last(s), s->a, d->db.buf + pos - n, n,
                        (flags & JSONWIRE_STRING_NON_VERBATIM) == 0);
                    if (r < 0) {
                        e = burrow_err_out_of_memory;
                        break;
                    }
                    if (r == 0) {
                        jt_wrap_with_object_name(s, d->db.buf + pos - n, n);
                        e = jsontext_err_duplicate_name;
                        pos -= n;
                        break;
                    }
                }
            }
            jt_names_replace_last_quoted(s, pos - n);
        }
        e = jt_append_string(s);
        if (BURROW_FAILED(e)) {
            pos -= n;
            break;
        }
        d->db.prev_start = pos - n;
        d->db.prev_end = pos;
        return jt_raw_decoder_token(d);
    }
    case '0':
        n = jsonwire_consume_simple_number(d->db.buf + pos, d->db.len - pos);
        if (n == 0 || pos + n == d->db.len) {
            int64_t old = d->db.base_offset + pos;
            e = jt_consume_number(d, &pos);
            n = (Int)(d->db.base_offset + pos - old);
            if (BURROW_FAILED(e))
                break;
        } else {
            pos += n;
        }
        e = jt_append_number(s);
        if (BURROW_FAILED(e)) {
            pos -= n;
            break;
        }
        d->db.prev_start = pos - n;
        d->db.prev_end = pos;
        return jt_raw_decoder_token(d);
    case '{': {
        bool ns = !jsonflags_get(&d->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES);
        if (!jt_reserve_object(s, ns)) {
            e = burrow_err_out_of_memory;
            break;
        }
        e = jt_push(s, JT_TYPE_OBJECT);
        if (BURROW_FAILED(e))
            break;
        (void)jt_names_push(s);
        if (ns)
            (void)jt_spaces_push(s);
        jsonflags_clear(&d->opts, JSONFLAG_TAG_FLAGS);
        pos++;
        d->db.prev_start = pos;
        d->db.prev_end = pos;
        return jsontext_begin_object;
    }
    case '}':
        e = jt_pop_object(s);
        if (BURROW_FAILED(e))
            break;
        jt_names_pop(s);
        if (!jsonflags_get(&d->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES))
            jt_spaces_pop(s);
        pos++;
        d->db.prev_start = pos;
        d->db.prev_end = pos;
        return jsontext_end_object;
    case '[':
        e = jt_push(s, JT_TYPE_ARRAY);
        if (BURROW_FAILED(e))
            break;
        jsonflags_clear(&d->opts, JSONFLAG_TAG_FLAGS);
        pos++;
        d->db.prev_start = pos;
        d->db.prev_end = pos;
        return jsontext_begin_array;
    case ']':
        e = jt_pop_array(s);
        if (BURROW_FAILED(e))
            break;
        pos++;
        d->db.prev_start = pos;
        d->db.prev_end = pos;
        return jsontext_end_array;
    default:
        e = burrow__jsonwire_new_invalid_character_error(
            d->db.buf + pos, d->db.len - pos, JT_LIT("at start of value"));
        break;
    }
    jt_set_err(err, jt_dwrap(d, e, pos, 1));
    return jt_zero_token();
}

/* decoderState.ReadValue, with the value's flags. */
static Slice jt_read_value(JsontextDecoder *d, unsigned *flags, Error *err) {
    JsonState *s = &d->st;
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    Int pos = 0;
    JsontextKind next = 0;
    Error e = jt_next(d, &pos, &next);
    jt_set_err(err, e);
    if (BURROW_FAILED(e))
        return none;
    int64_t old = d->db.base_offset + pos;
    e = jt_consume_value(d, flags, &pos, jt_depth(s));
    Int n = (Int)(d->db.base_offset + pos - old);
    if (BURROW_FAILED(e)) {
        jt_set_err(err, jt_dwrap(d, e, pos, 1));
        return none;
    }
    switch (next) {
    case 'n':
    case 't':
    case 'f':
        e = jt_append_literal(s);
        break;
    case '"':
        if (jt_e_need_name(s->last)) {
            if (!jsonflags_get(&d->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
                if (!jt_e_valid_ns(s->last)) {
                    e = jt_err_invalid_namespace;
                    break;
                }
                if (jt_e_active_ns(s->last)) {
                    int r = jt_ns_insert_quoted(
                        jt_spaces_last(s), s->a, d->db.buf + pos - n, n,
                        (*flags & JSONWIRE_STRING_NON_VERBATIM) == 0);
                    if (r < 0) {
                        e = burrow_err_out_of_memory;
                        break;
                    }
                    if (r == 0) {
                        jt_wrap_with_object_name(s, d->db.buf + pos - n, n);
                        e = jsontext_err_duplicate_name;
                        break;
                    }
                }
            }
            jt_names_replace_last_quoted(s, pos - n);
        }
        e = jt_append_string(s);
        break;
    case '0':
        e = jt_append_number(s);
        break;
    case '{':
        e = jt_push(s, JT_TYPE_OBJECT);
        if (BURROW_FAILED(e))
            break;
        e = jt_pop_object(s);
        if (BURROW_FAILED(e))
            panic_str(JT_LIT(
                "BUG: popObject should never fail immediately after pushObject"));
        break;
    case '[':
        e = jt_push(s, JT_TYPE_ARRAY);
        if (BURROW_FAILED(e))
            break;
        e = jt_pop_array(s);
        if (BURROW_FAILED(e))
            panic_str(
                JT_LIT("BUG: popArray should never fail immediately after pushArray"));
        break;
    default:
        break;
    }
    if (BURROW_FAILED(e)) {
        jt_set_err(err, jt_dwrap(d, e, pos - n, 1));
        return none;
    }
    d->db.prev_end = pos;
    d->db.prev_start = pos - n;
    Slice v = {d->db.buf + pos - n, n, n, TYPE_BYTE};
    return v;
}

JsontextValue jsontext_decoder_read_value(JsontextDecoder *d, Error *err) {
    unsigned flags = 0;
    return jt_read_value(d, &flags, err);
}

Error jsontext_decoder_skip_value(JsontextDecoder *d) {
    Error err = BURROW_NO_ERROR;
    switch (jsontext_decoder_peek_kind(d)) {
    case '{':
    case '[': {
        Int depth = jt_depth(&d->st);
        for (;;) {
            (void)jsontext_decoder_read_token(d, &err);
            if (BURROW_FAILED(err))
                return err;
            if (depth >= jt_depth(&d->st))
                return BURROW_NO_ERROR;
        }
    }
    default:
        (void)jsontext_decoder_read_value(d, &err);
        return err;
    }
}

/* decoderState.checkEOF. */
static Error jt_check_eof(JsontextDecoder *d, Int pos) {
    Error err = jt_consume_ws(d, &pos);
    if (BURROW_OK(err)) {
        err = burrow__jsonwire_new_invalid_character_error(
            d->db.buf + pos, d->db.len - pos, JT_LIT("after top-level value"));
        return jt_dwrap(d, err, pos, 0);
    }
    if (jt_is(err, io_err_unexpected_eof))
        return BURROW_NO_ERROR;
    return err;
}

/* Put d at the start of a fresh input: r when has_rd, or else the len bytes
 * at b, which are only read. */
static void jt_decoder_setup(JsontextDecoder *d, IoReader r, bool has_rd, Byte *b,
                             Int len, const JsontextOptions *opts) {
    jt_state_reset(&d->st);
    if (!d->owns_buf) {
        d->db.buf = JT_NO_BUF;
        d->db.cap = 0;
    }
    d->db.len = 0;
    d->db.prev_start = 0;
    d->db.prev_end = 0;
    d->db.base_offset = 0;
    d->peek_pos = 0;
    d->peek_err = BURROW_NO_ERROR;
    d->rd = r;
    d->has_rd = has_rd;
    if (!has_rd) {
        jt_decoder_drop_buf(d);
        d->db.buf = b != NULL ? b : JT_NO_BUF;
        d->db.len = len;
        d->db.cap = len;
    }
    d->opts = *opts;
}

static void jt_decoder_init(JsontextDecoder *d, Alloc *a) {
    memset(d, 0, sizeof(*d));
    d->a = a;
    d->db.buf = JT_NO_BUF;
    jt_state_init(&d->st, a);
}

static void jt_decoder_release(JsontextDecoder *d) {
    jt_decoder_drop_buf(d);
    jt_frames_free(d->a, &d->frames, &d->frames_cap);
    jt_state_release(&d->st);
}

static void jt_decoder_reset_opts(JsontextDecoder *d, IoReader r,
                                  const JsontextOptions *opts) {
    if (d == NULL)
        panic_str(JT_LIT("jsontext: invalid nil Decoder"));
    if (r.vt == NULL)
        panic_str(JT_LIT("jsontext: invalid nil io.Reader"));
    if (jsonflags_get(&d->opts, JSONFLAG_WITHIN_ARSHAL_CALL))
        panic_str(
            JT_LIT("jsontext: cannot reset Decoder passed to json.UnmarshalerFrom"));
    jt_decoder_setup(d, r, true, NULL, 0, opts);
}

static JsontextDecoder *jt_new_decoder(Alloc *a, IoReader r,
                                       const JsontextOptions *opts) {
    JsontextDecoder *d = (JsontextDecoder *)mem_alloc(a, sizeof(JsontextDecoder),
                                                      _Alignof(JsontextDecoder));
    if (d == NULL)
        return NULL;
    jt_decoder_init(d, a);
    jt_decoder_reset_opts(d, r, opts);
    return d;
}

JsontextDecoder *jsontext_new_decoder(Alloc *a, IoReader r, Slice opts) {
    JsontextOptions o = jt_join_slice(opts);
    return jt_new_decoder(a, r, &o);
}

JsontextDecoder *jsontext_new_decoder_v(Alloc *a, IoReader r, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    return jt_new_decoder(a, r, &o);
}

void jsontext_decoder_free(JsontextDecoder *d) {
    if (d == NULL)
        return;
    Alloc *a = d->a;
    jt_decoder_release(d);
    mem_free(a, d, sizeof(JsontextDecoder), _Alignof(JsontextDecoder));
}

void jsontext_decoder_reset(JsontextDecoder *d, IoReader r, Slice opts) {
    JsontextOptions o = jt_join_slice(opts);
    jt_decoder_reset_opts(d, r, &o);
}

void jsontext_decoder_reset_v(JsontextDecoder *d, IoReader r, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    jt_decoder_reset_opts(d, r, &o);
}

JsontextOptions jsontext_decoder_options(const JsontextDecoder *d) {
    return d->opts;
}

int64_t jsontext_decoder_input_offset(const JsontextDecoder *d) {
    return d->db.base_offset + d->db.prev_end;
}

Slice jsontext_decoder_unread_buffer(const JsontextDecoder *d) {
    Slice s = {d->db.buf + d->db.prev_end, d->db.len - d->db.prev_end,
               d->db.len - d->db.prev_end, TYPE_BYTE};
    return s;
}

Int jsontext_decoder_stack_depth(const JsontextDecoder *d) {
    return jt_depth(&d->st) - 1;
}

/* Tokens.index, which panics the way a Go slice index does. */
static uint64_t jt_checked_index(const JsonState *s, Int i) {
    if (i < 0 || i > s->stack_len)
        panic_str(JT_LIT("runtime error: index out of range"));
    return jt_index(s, i);
}

static JsontextKind jt_stack_index(const JsonState *s, Int i, int64_t *length) {
    uint64_t e = jt_checked_index(s, i);
    if (length != NULL)
        *length = jt_e_len(e);
    if (i > 0 && jt_e_is_object(e))
        return '{';
    if (i > 0 && jt_e_is_array(e))
        return '[';
    return 0;
}

JsontextKind jsontext_decoder_stack_index(const JsontextDecoder *d, Int i,
                                          int64_t *length) {
    return jt_stack_index(&d->st, i, length);
}

static JsontextPointer jt_stack_pointer(JsonState *s, Byte *buf, Int blen, Alloc *a) {
    jt_names_copy_quoted(s, buf, blen);
    JsonBuf b = {NULL, 0, 0, a, true, false};
    jt_append_stack_pointer(s, &b, -1);
    return jt_buf_str_or_empty(&b);
}

JsontextPointer jsontext_decoder_stack_pointer(JsontextDecoder *d, Alloc *a) {
    return jt_stack_pointer(&d->st, d->db.buf, d->db.len, a);
}

/* ------------------------------------------------------------------ encoder */

static const Type jt_encoder_desc = {
    {(const Byte *)"Encoder", 7},
    {(const Byte *)"jsontext", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(JsontextEncoder),
    (uint16_t)_Alignof(JsontextEncoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6a746563U, /* "jtec" */
    NULL,
};

const Type *const TYPE_JSONTEXT_ENCODER = &jt_encoder_desc;

static Error jt_ewrap(JsontextEncoder *e, Error err, Int pos, int where) {
    return jt_wrap_syntactic(&e->st, &e->opts, e->base_offset, e->buf.p, e->buf.len,
                             false, err, pos, where);
}

static bool jt_avoid_flush(const JsontextEncoder *e) {
    uint64_t last = e->st.last;
    if (jt_e_len(last) == 0 || jt_e_need_value(last))
        return true;
    if (jt_e_need_name(last) && e->buf.len >= 2) {
        const Byte *t = e->buf.p + e->buf.len - 2;
        if ((t[0] == 'l' && t[1] == 'l') || (t[0] == '"' && t[1] == '"') ||
            (t[0] == '{' && t[1] == '}') || (t[0] == '[' && t[1] == ']'))
            return true;
    }
    return false;
}

static bool jt_need_flush(const JsontextEncoder *e) {
    return e->has_wr && (jt_depth(&e->st) == 1 || e->buf.len > 3 * e->buf.cap / 4);
}

/* encoderState.Flush. */
static Error jt_flush(JsontextEncoder *e) {
    if (!e->has_wr || jt_avoid_flush(e))
        return BURROW_NO_ERROR;
    if (jt_depth(&e->st) == 1 &&
        !jsonflags_get(&e->opts, JSONFLAG_OMIT_TOP_LEVEL_NEWLINE)) {
        jsonbuf_byte(&e->buf, '\n');
        if (e->buf.failed) {
            e->buf.failed = false;
            return burrow_err_out_of_memory;
        }
    }
    jt_names_copy_quoted(&e->st, e->buf.p, e->buf.len);
    if (e->st.unquoted.failed)
        return burrow_err_out_of_memory;
    Error err = BURROW_NO_ERROR;
    Slice p = {e->buf.p, e->buf.len, e->buf.len, TYPE_BYTE};
    Int n = e->wr.vt->write(e->wr.data, p, &err);
    e->base_offset += n;
    if (BURROW_FAILED(err)) {
        if (n > 0) {
            memmove(e->buf.p, e->buf.p + n, (size_t)(e->buf.len - n));
            e->buf.len -= n;
        }
        return jt_io_error(JT_LIT("write"), err);
    }
    e->buf.len = 0;
    return BURROW_NO_ERROR;
}

/* encoderState.AppendIndent. */
static void jt_append_indent(const JsontextEncoder *e, JsonBuf *b, Int n) {
    if (n == 0)
        return;
    jsonbuf_byte(b, '\n');
    jsonbuf_str(b, e->opts.indent_prefix);
    for (; n > 1; n--)
        jsonbuf_str(b, e->opts.indent);
}

/* encoderState.appendWhitespace. */
static void jt_append_whitespace(const JsontextEncoder *e, JsonBuf *b,
                                 JsontextKind next) {
    Byte delim = jt_need_delim(&e->st, next);
    if (delim == ':') {
        if (jsonflags_get(&e->opts, JSONFLAG_SPACE_AFTER_COLON))
            jsonbuf_byte(b, ' ');
        return;
    }
    if (delim == ',' && jsonflags_get(&e->opts, JSONFLAG_SPACE_AFTER_COMMA))
        jsonbuf_byte(b, ' ');
    if (jsonflags_get(&e->opts, JSONFLAG_MULTILINE))
        jt_append_indent(e, b, jt_need_indent(&e->st, next));
}

/* The delimiter and white space in front of a token of kind k. */
static void jt_append_before(const JsontextEncoder *e, JsonBuf *b, JsontextKind k) {
    Byte delim = jt_need_delim(&e->st, k);
    if (delim != 0)
        jsonbuf_byte(b, delim);
    if (jsonflags_get(&e->opts, JSONFLAG_ANY_WHITESPACE))
        jt_append_whitespace(e, b, k);
}

/* The name checks WriteToken and WriteValue do for a string that was just
 * written at pos. */
static Error jt_encoder_check_name(JsontextEncoder *e, Int pos) {
    JsonState *s = &e->st;
    if (jt_e_need_name(s->last)) {
        if (!jsonflags_get(&e->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
            if (!jt_e_valid_ns(s->last))
                return jt_err_invalid_namespace;
            if (jt_e_active_ns(s->last)) {
                int r = jt_ns_insert_quoted(jt_spaces_last(s), s->a, e->buf.p + pos,
                                            e->buf.len - pos, false);
                if (r < 0)
                    return burrow_err_out_of_memory;
                if (r == 0) {
                    jt_wrap_with_object_name(s, e->buf.p + pos, e->buf.len - pos);
                    return jsontext_err_duplicate_name;
                }
            }
        }
        jt_names_replace_last_quoted(s, pos);
    }
    return jt_append_string(s);
}

/* The end of WriteToken and WriteValue: undo the output on an error, or
 * else flush if it is time to. */
static Error jt_encoder_finish(JsontextEncoder *e, Int saved, Error err, Int pos) {
    if (BURROW_OK(err) && e->buf.failed)
        err = burrow_err_out_of_memory;
    if (BURROW_FAILED(err)) {
        e->buf.len = saved;
        e->buf.failed = false;
        return jt_ewrap(e, err, pos, 1);
    }
    if (jt_need_flush(e))
        return jt_flush(e);
    return BURROW_NO_ERROR;
}

Error jsontext_encoder_write_token(JsontextEncoder *e, JsontextToken t) {
    JsonState *s = &e->st;
    JsonBuf *b = &e->buf;
    JsontextKind k = jsontext_token_kind(t);
    Int saved = b->len;
    jt_append_before(e, b, k);
    Int pos = b->len;
    Error err = BURROW_NO_ERROR;
    bool ns = !jsonflags_get(&e->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES);
    switch (k) {
    case 'n':
    case 'f':
    case 't':
        jsonbuf_str(b, k == 'n'   ? JT_LIT("null")
                       : k == 'f' ? JT_LIT("false")
                                  : JT_LIT("true"));
        if (b->failed)
            break;
        err = jt_append_literal(s);
        break;
    case '"':
        err = jt_token_append_string(t, b, &e->opts);
        if (BURROW_FAILED(err) || b->failed)
            break;
        err = jt_encoder_check_name(e, pos);
        break;
    case '0':
        err = jt_token_append_number(t, b, &e->opts);
        if (BURROW_FAILED(err) || b->failed)
            break;
        err = jt_append_number(s);
        break;
    case '{':
        jsonbuf_byte(b, '{');
        if (b->failed)
            break;
        if (!jt_reserve_object(s, ns)) {
            err = burrow_err_out_of_memory;
            break;
        }
        err = jt_push(s, JT_TYPE_OBJECT);
        if (BURROW_FAILED(err))
            break;
        (void)jt_names_push(s);
        if (ns)
            (void)jt_spaces_push(s);
        jsonflags_clear(&e->opts, JSONFLAG_TAG_FLAGS);
        break;
    case '}':
        jsonbuf_byte(b, '}');
        if (b->failed)
            break;
        err = jt_pop_object(s);
        if (BURROW_FAILED(err))
            break;
        jt_names_pop(s);
        if (ns)
            jt_spaces_pop(s);
        break;
    case '[':
        jsonbuf_byte(b, '[');
        if (b->failed)
            break;
        err = jt_push(s, JT_TYPE_ARRAY);
        jsonflags_clear(&e->opts, JSONFLAG_TAG_FLAGS);
        break;
    case ']':
        jsonbuf_byte(b, ']');
        if (b->failed)
            break;
        err = jt_pop_array(s);
        break;
    default:
        err = jt_err_invalid_token;
        break;
    }
    return jt_encoder_finish(e, saved, err, pos);
}

/* reformatValue, reformatObject and reformatArray as one loop over an
 * explicit stack. *ip is where in src the value starts and ends up where
 * it stopped, which on an error is where the problem is. */
static Error jt_reformat_value(JsontextEncoder *e, JsonBuf *dst, const Byte *src,
                               Int slen, Int *ip, Int depth) {
    JsonState *s = &e->st;
    const JsontextOptions *o = &e->opts;
    bool dup_check = !jsonflags_get(o, JSONFLAG_ALLOW_DUPLICATE_NAMES);
    bool multiline = jsonflags_get(o, JSONFLAG_MULTILINE);
    Int spaces_mark = s->spaces_len;
    Int nframes = 0;
    Int i = *ip;
    Int m = 0;
    Error err = BURROW_NO_ERROR;
    JtFrame *f = NULL;
    s->rev.len = 0;

value:
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    switch (jt_norm(src[i])) {
    case 'n':
    case 'f':
    case 't': {
        Str lit = src[i] == 'n'   ? JT_LIT("null")
                  : src[i] == 'f' ? JT_LIT("false")
                                  : JT_LIT("true");
        if (slen - i >= lit.len && memcmp(src + i, lit.p, (size_t)lit.len) == 0) {
            jsonbuf_str(dst, lit);
            i += lit.len;
            goto after;
        }
        i += burrow__jsonwire_consume_literal(src + i, slen - i, lit, &err);
        goto fail;
    }
    case '"':
        m = jsonwire_consume_simple_string(src + i, slen - i);
        if (m > 0) {
            jsonbuf_put(dst, src + i, m);
            i += m;
            goto after;
        }
        i += burrow__jsonwire_reformat_string(dst, src + i, slen - i, o, &err);
        if (BURROW_FAILED(err))
            goto fail;
        goto after;
    case '0':
        m = jsonwire_consume_simple_number(src + i, slen - i);
        if (m > 0 && !jsonflags_get(o, JSONFLAG_CANONICALIZE_NUMBERS)) {
            jsonbuf_put(dst, src + i, m);
            i += m;
            goto after;
        }
        i += burrow__jsonwire_reformat_number(dst, src + i, slen - i, o, &err);
        if (BURROW_FAILED(err))
            goto fail;
        goto after;
    case '{':
        goto open_object;
    case '[':
        goto open_array;
    default:
        err = burrow__jsonwire_new_invalid_character_error(src + i, slen - i,
                                                           JT_LIT("at start of value"));
        goto fail;
    }

open_object:
    if (depth == JT_MAX_NESTING_DEPTH + 1) {
        err = jt_err_max_depth;
        goto fail;
    }
    jsonbuf_byte(dst, '{');
    i++;
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    if (src[i] == '}') {
        jsonbuf_byte(dst, '}');
        i++;
        goto after;
    }
    if (dup_check && !jt_spaces_push(s)) {
        err = burrow_err_out_of_memory;
        goto fail;
    }
    depth++;
    f = jt_frame_push(e->a, &e->frames, &e->frames_cap, &nframes);
    if (f == NULL) {
        err = burrow_err_out_of_memory;
        goto fail;
    }
    f->object = true;

member:
    f = (JtFrame *)e->frames + nframes - 1;
    f->wrap = false;
    if (multiline)
        jt_append_indent(e, dst, depth);
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    {
        m = jsonwire_consume_simple_string(src + i, slen - i);
        bool verbatim = m > 0;
        if (verbatim) {
            jsonbuf_put(dst, src + i, m);
        } else {
            m = burrow__jsonwire_reformat_string(dst, src + i, slen - i, o, &err);
            if (BURROW_FAILED(err)) {
                i += m;
                goto fail;
            }
        }
        f->name_at = i;
        f->name_len = m;
        if (dup_check) {
            int r = jt_ns_insert_quoted(jt_spaces_last(s), s->a, src + i, m, verbatim);
            if (r < 0) {
                err = burrow_err_out_of_memory;
                goto fail;
            }
            if (r == 0) {
                f->wrap = true;
                err = jsontext_err_duplicate_name;
                goto fail;
            }
        }
    }
    f->wrap = true;
    i += m;
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    if (src[i] != ':') {
        err = burrow__jsonwire_new_invalid_character_error(
            src + i, slen - i, JT_LIT("after object name (expecting ':')"));
        goto fail;
    }
    jsonbuf_byte(dst, ':');
    i++;
    if (jsonflags_get(o, JSONFLAG_SPACE_AFTER_COLON))
        jsonbuf_byte(dst, ' ');
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    goto value;

open_array:
    if (depth == JT_MAX_NESTING_DEPTH + 1) {
        err = jt_err_max_depth;
        goto fail;
    }
    jsonbuf_byte(dst, '[');
    i++;
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    if (src[i] == ']') {
        jsonbuf_byte(dst, ']');
        i++;
        goto after;
    }
    depth++;
    f = jt_frame_push(e->a, &e->frames, &e->frames_cap, &nframes);
    if (f == NULL) {
        err = burrow_err_out_of_memory;
        goto fail;
    }

element:
    f = (JtFrame *)e->frames + nframes - 1;
    f->wrap = false;
    if (multiline)
        jt_append_indent(e, dst, depth);
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    f->wrap = true;
    goto value;

after:
    if (nframes == 0) {
        *ip = i;
        return BURROW_NO_ERROR;
    }
    f = (JtFrame *)e->frames + nframes - 1;
    f->wrap = false;
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (i >= slen) {
        err = io_err_unexpected_eof;
        goto fail;
    }
    if (src[i] == ',') {
        jsonbuf_byte(dst, ',');
        if (jsonflags_get(o, JSONFLAG_SPACE_AFTER_COMMA))
            jsonbuf_byte(dst, ' ');
        i++;
        if (f->object)
            goto member;
        f->idx++;
        goto element;
    }
    if (src[i] == (f->object ? '}' : ']')) {
        if (multiline)
            jt_append_indent(e, dst, depth - 1);
        jsonbuf_byte(dst, src[i]);
        i++;
        if (f->object && dup_check)
            jt_spaces_pop(s);
        nframes--;
        depth--;
        goto after;
    }
    err = burrow__jsonwire_new_invalid_character_error(
        src + i, slen - i,
        f->object ? JT_LIT("after object value (expecting ',' or '}')")
                  : JT_LIT("after array value (expecting ',' or ']')"));

fail:
    for (Int k = nframes - 1; k >= 0; k--) {
        const JtFrame *g = (const JtFrame *)e->frames + k;
        if (!g->wrap)
            continue;
        if (g->object)
            jt_wrap_with_object_name(s, src + g->name_at, g->name_len);
        else
            jt_wrap_with_array_index(s, g->idx);
    }
    s->spaces_len = spaces_mark;
    *ip = i;
    return err;
}

static Error jt_must_reorder_objects(Byte *b, Int n);

Error jsontext_encoder_write_value(JsontextEncoder *e, JsontextValue v) {
    JsonState *s = &e->st;
    JsonBuf *b = &e->buf;
    const Byte *src = (const Byte *)v.p;
    Int slen = v.len;
    e->max_value |= slen;
    JsontextKind k = jsontext_value_kind(v);
    Int saved = b->len;
    jt_append_before(e, b, k);
    Int pos = b->len;
    Int i = jsonwire_consume_whitespace(src, slen);
    Error err = jt_reformat_value(e, b, src, slen, &i, jt_depth(s));
    if (BURROW_OK(err) && b->failed)
        err = burrow_err_out_of_memory;
    if (BURROW_FAILED(err)) {
        b->len = saved;
        b->failed = false;
        return jt_ewrap(e, err, pos + i, 1);
    }
    i += jsonwire_consume_whitespace(src + i, slen - i);
    if (slen > i) {
        b->len = saved;
        err = burrow__jsonwire_new_invalid_character_error(
            src + i, slen - i, JT_LIT("after top-level value"));
        return jt_ewrap(e, err, pos + i, 0);
    }
    switch (k) {
    case 'n':
    case 'f':
    case 't':
        err = jt_append_literal(s);
        break;
    case '"':
        err = jt_encoder_check_name(e, pos);
        break;
    case '0':
        err = jt_append_number(s);
        break;
    case '{':
        err = jt_push(s, JT_TYPE_OBJECT);
        if (BURROW_FAILED(err))
            break;
        err = jt_pop_object(s);
        if (BURROW_FAILED(err))
            panic_str(JT_LIT(
                "BUG: popObject should never fail immediately after pushObject"));
        if (jsonflags_get(&e->opts, JSONFLAG_REORDER_RAW_OBJECTS))
            err = jt_must_reorder_objects(b->p + pos, b->len - pos);
        break;
    case '[':
        err = jt_push(s, JT_TYPE_ARRAY);
        if (BURROW_FAILED(err))
            break;
        err = jt_pop_array(s);
        if (BURROW_FAILED(err))
            panic_str(
                JT_LIT("BUG: popArray should never fail immediately after pushArray"));
        if (jsonflags_get(&e->opts, JSONFLAG_REORDER_RAW_OBJECTS))
            err = jt_must_reorder_objects(b->p + pos, b->len - pos);
        break;
    default:
        break;
    }
    return jt_encoder_finish(e, saved, err, pos);
}

int64_t jsontext_encoder_output_offset(const JsontextEncoder *e) {
    return e->base_offset + e->buf.len;
}

Slice jsontext_encoder_available_buffer(JsontextEncoder *e) {
    uint64_t x = (uint64_t)(e->max_value | 63);
    int bitlen = 0;
    while (x != 0) {
        bitlen++;
        x >>= 1;
    }
    Int n = (Int)1 << bitlen;
    if (e->avail.cap < n) {
        burrow__jsonbuf_free(&e->avail);
        e->avail.a = e->a;
        e->avail.owned = true;
        (void)jsonbuf_reserve(&e->avail, n);
    }
    e->avail.len = 0;
    Slice s = {e->avail.p, 0, e->avail.cap, TYPE_BYTE};
    return s;
}

Int jsontext_encoder_stack_depth(const JsontextEncoder *e) {
    return jt_depth(&e->st) - 1;
}

JsontextKind jsontext_encoder_stack_index(const JsontextEncoder *e, Int i,
                                          int64_t *length) {
    return jt_stack_index(&e->st, i, length);
}

JsontextPointer jsontext_encoder_stack_pointer(JsontextEncoder *e, Alloc *a) {
    return jt_stack_pointer(&e->st, e->buf.p, e->buf.len, a);
}

static void jt_encoder_init(JsontextEncoder *e, Alloc *a) {
    memset(e, 0, sizeof(*e));
    e->a = a;
    jt_state_init(&e->st, a);
    e->buf.a = a;
    e->buf.owned = true;
    e->avail.a = a;
    e->avail.owned = true;
}

static void jt_encoder_release(JsontextEncoder *e) {
    burrow__jsonbuf_free(&e->buf);
    burrow__jsonbuf_free(&e->avail);
    jt_frames_free(e->a, &e->frames, &e->frames_cap);
    jt_state_release(&e->st);
}

/* encoderState.reset with the options already joined. */
static void jt_encoder_setup(JsontextEncoder *e, IoWriter w, bool has_wr,
                             const JsontextOptions *opts) {
    jt_state_reset(&e->st);
    e->buf.len = 0;
    e->buf.failed = false;
    e->base_offset = 0;
    e->wr = w;
    e->has_wr = has_wr;
    e->max_value = 0;
    e->opts = *opts;
    if (jsonflags_get(&e->opts, JSONFLAG_MULTILINE))
        burrow__jsonopts_initialize_multiline(&e->opts);
}

static void jt_encoder_reset_opts(JsontextEncoder *e, IoWriter w,
                                  const JsontextOptions *opts) {
    if (e == NULL)
        panic_str(JT_LIT("jsontext: invalid nil Encoder"));
    if (w.vt == NULL)
        panic_str(JT_LIT("jsontext: invalid nil io.Writer"));
    if (jsonflags_get(&e->opts, JSONFLAG_WITHIN_ARSHAL_CALL))
        panic_str(JT_LIT("jsontext: cannot reset Encoder passed to json.MarshalerTo"));
    jt_encoder_setup(e, w, true, opts);
}

static JsontextEncoder *jt_new_encoder(Alloc *a, IoWriter w,
                                       const JsontextOptions *opts) {
    JsontextEncoder *e = (JsontextEncoder *)mem_alloc(a, sizeof(JsontextEncoder),
                                                      _Alignof(JsontextEncoder));
    if (e == NULL)
        return NULL;
    jt_encoder_init(e, a);
    jt_encoder_reset_opts(e, w, opts);
    return e;
}

JsontextEncoder *jsontext_new_encoder(Alloc *a, IoWriter w, Slice opts) {
    JsontextOptions o = jt_join_slice(opts);
    return jt_new_encoder(a, w, &o);
}

JsontextEncoder *jsontext_new_encoder_v(Alloc *a, IoWriter w, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    return jt_new_encoder(a, w, &o);
}

void jsontext_encoder_free(JsontextEncoder *e) {
    if (e == NULL)
        return;
    Alloc *a = e->a;
    jt_encoder_release(e);
    mem_free(a, e, sizeof(JsontextEncoder), _Alignof(JsontextEncoder));
}

void jsontext_encoder_reset(JsontextEncoder *e, IoWriter w, Slice opts) {
    JsontextOptions o = jt_join_slice(opts);
    jt_encoder_reset_opts(e, w, &o);
}

void jsontext_encoder_reset_v(JsontextEncoder *e, IoWriter w, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    jt_encoder_reset_opts(e, w, &o);
}

JsontextOptions jsontext_encoder_options(const JsontextEncoder *e) {
    return e->opts;
}

/* ------------------------------------------------------------ reordering */

/* One object member for mustReorderObjects. The name is either in b or,
 * when it had escapes, unquoted into the names buffer. The member's bytes
 * run from start, which is just after the previous value or the '{', to
 * end, just after its value. */
typedef struct JtMember {
    Int name_at;
    Int name_len;
    Int start;
    Int end;
    bool name_copied;
} JtMember;

typedef struct JtReorderFrame {
    Int members_start;
    Int names_mark;
    Int before_body;
    Int before_name;
    Int name_at;
    Int name_len;
    bool name_copied;
    bool object;
    bool sorted;
} JtReorderFrame;

typedef struct JtReorder {
    const Byte *b;
    const JsonBuf *names;
    JtMember *members;
    Int nmembers;
    Int members_cap;
} JtReorder;

#define JT_COMMA_AND_WHITESPACE(c)                                                     \
    ((c) == ',' || (c) == ' ' || (c) == '\n' || (c) == '\r' || (c) == '\t')

static Int jt_trim_comma_ws(const Byte *p, Int n) {
    Int i = 0;
    while (i < n && JT_COMMA_AND_WHITESPACE(p[i]))
        i++;
    return i;
}

static const Byte *jt_member_name(const JtReorder *r, const JtMember *m) {
    return m->name_copied ? r->names->p + m->name_at : r->b + m->name_at;
}

/* objectMember.Compare. */
static int jt_member_compare(const JtReorder *r, const JtMember *x, const JtMember *y) {
    int c = burrow__jsonwire_compare_utf16(jt_member_name(r, x), x->name_len,
                                           jt_member_name(r, y), y->name_len);
    if (c != 0)
        return c;
    Int xi = jt_trim_comma_ws(r->b + x->start, x->end - x->start);
    Int yi = jt_trim_comma_ws(r->b + y->start, y->end - y->start);
    return burrow__jsonwire_compare_utf16(r->b + x->start + xi, x->end - x->start - xi,
                                          r->b + y->start + yi, y->end - y->start - yi);
}

static void jt_member_sort(const JtReorder *r, JtMember *m, JtMember *tmp, Int n) {
    if (n < 2)
        return;
    if (n <= 8) {
        for (Int i = 1; i < n; i++) {
            JtMember x = m[i];
            Int j = i;
            for (; j > 0 && jt_member_compare(r, &m[j - 1], &x) > 0; j--)
                m[j] = m[j - 1];
            m[j] = x;
        }
        return;
    }
    Int h = n / 2;
    jt_member_sort(r, m, tmp, h);
    jt_member_sort(r, m + h, tmp, n - h);
    Int i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = jt_member_compare(r, &m[j], &m[i]) < 0 ? m[j++] : m[i++];
    while (i < h)
        tmp[k++] = m[i++];
    while (j < n)
        tmp[k++] = m[j++];
    memcpy(m, tmp, (size_t)n * sizeof(JtMember));
}

/* Rewrites the members of one closed object in sorted order. */
static bool jt_reorder_one(JtReorder *r, Byte *b, const JtReorderFrame *f,
                           Int after_body, JsonBuf *scratch) {
    JtMember *m = r->members + f->members_start;
    Int n = r->nmembers - f->members_start;
    JtMember *tmp = (JtMember *)mem_alloc_nozero(
        heap_allocator(), (size_t)n * sizeof(JtMember), _Alignof(JtMember));
    if (tmp == NULL)
        return false;
    JtMember first_before = m[0];
    jt_member_sort(r, m, tmp, n);
    mem_free(heap_allocator(), tmp, (size_t)n * sizeof(JtMember), _Alignof(JtMember));
    JtMember first_after = m[0];
    scratch->len = 0;
    for (Int i = 0; i < n; i++) {
        const Byte *p = b + m[i].start;
        Int len = m[i].end - m[i].start;
        Int t = jt_trim_comma_ws(p, len);
        if (i == 0 && m[i].start != first_before.start) {
            jsonbuf_put(scratch, b + first_before.start,
                        jt_trim_comma_ws(b + first_before.start,
                                         first_before.end - first_before.start));
            jsonbuf_put(scratch, p + t, len - t);
        } else if (i != 0 && m[i].start == first_before.start) {
            jsonbuf_put(scratch, b + first_after.start,
                        jt_trim_comma_ws(b + first_after.start,
                                         first_after.end - first_after.start));
            jsonbuf_put(scratch, p + t, len - t);
        } else {
            jsonbuf_put(scratch, p, len);
        }
    }
    if (scratch->failed)
        return false;
    if (after_body - f->before_body != scratch->len)
        panic_str(JT_LIT("BUG: length invariant violated"));
    memcpy(b + f->before_body, scratch->p, (size_t)scratch->len);
    return true;
}

/* The end of a scalar in text that is known to be valid. */
static Int jt_scalar_end(const Byte *b, Int n, Int i) {
    switch (b[i]) {
    case 'n':
    case 't':
        return i + 4;
    case 'f':
        return i + 5;
    default: {
        Error err = BURROW_NO_ERROR;
        return i + burrow__jsonwire_consume_number(b + i, n - i, &err);
    }
    }
}

/* mustReorderObjects. b holds one valid value that the encoder just wrote,
 * and every object in it is sorted by name in place, the way RFC 8785
 * section 3.2.3 asks. Written as a loop so deep nesting costs heap and not
 * C stack. The only error is running out of memory. */
static Error jt_must_reorder_objects(Byte *b, Int n) {
    Alloc *a = heap_allocator();
    JsonBuf names = {NULL, 0, 0, a, true, false};
    JsonBuf scratch = {NULL, 0, 0, a, true, false};
    JtReorder r = {b, &names, NULL, 0, 0};
    JtReorderFrame *frames = NULL;
    Int nframes = 0, frames_cap = 0;
    Error err = BURROW_NO_ERROR;
    Int i = jsonwire_consume_whitespace(b, n);

    for (;;) {
        /* At the start of a value. */
        bool opened = false;
        if (b[i] == '{' || b[i] == '[') {
            void *p = frames;
            if (!jt_grow_array(a, &p, &frames_cap, nframes, sizeof(JtReorderFrame),
                               _Alignof(JtReorderFrame))) {
                err = burrow_err_out_of_memory;
                break;
            }
            frames = (JtReorderFrame *)p;
            JtReorderFrame *f = &frames[nframes++];
            memset(f, 0, sizeof(*f));
            f->object = b[i] == '{';
            f->sorted = true;
            f->members_start = r.nmembers;
            f->names_mark = names.len;
            i++;
            f->before_body = i;
            opened = true;
        } else if (b[i] == '"') {
            unsigned flags = 0;
            Error e2 = BURROW_NO_ERROR;
            i += burrow__jsonwire_consume_string_resumable(&flags, b + i, n - i, 0,
                                                           false, &e2);
        } else {
            i = jt_scalar_end(b, n, i);
        }

        /* After a value, or just inside a container. Close containers and
         * finish members until another value starts. */
        for (;;) {
            if (nframes == 0)
                goto done;
            JtReorderFrame *f = &frames[nframes - 1];
            if (!opened && f->object) {
                void *p = r.members;
                if (!jt_grow_array(a, &p, &r.members_cap, r.nmembers, sizeof(JtMember),
                                   _Alignof(JtMember))) {
                    err = burrow_err_out_of_memory;
                    goto done;
                }
                r.members = (JtMember *)p;
                JtMember cur = {f->name_at, f->name_len, f->before_name, i,
                                f->name_copied};
                if (f->sorted && r.nmembers > f->members_start)
                    f->sorted =
                        jt_member_compare(&r, &r.members[r.nmembers - 1], &cur) < 0;
                r.members[r.nmembers++] = cur;
            }
            opened = false;
            Int before = i;
            i += jsonwire_consume_whitespace(b + i, n - i);
            if (b[i] == '}' || b[i] == ']') {
                if (f->object && !f->sorted &&
                    !jt_reorder_one(&r, b, f, before, &scratch)) {
                    err = burrow_err_out_of_memory;
                    goto done;
                }
                i++;
                r.nmembers = f->members_start;
                names.len = f->names_mark;
                nframes--;
                continue;
            }
            if (b[i] == ',') {
                i++;
                i += jsonwire_consume_whitespace(b + i, n - i);
            }
            if (f->object) {
                f->before_name = before;
                unsigned flags = 0;
                Error e2 = BURROW_NO_ERROR;
                Int m = burrow__jsonwire_consume_string_resumable(&flags, b + i, n - i,
                                                                  0, false, &e2);
                if ((flags & JSONWIRE_STRING_NON_VERBATIM) == 0) {
                    f->name_at = i + 1;
                    f->name_len = m - 2;
                    f->name_copied = false;
                } else {
                    Int start = names.len;
                    (void)burrow__jsonwire_unquote_may_copy(b + i, m, false, &names);
                    if (names.failed) {
                        err = burrow_err_out_of_memory;
                        goto done;
                    }
                    f->name_at = start;
                    f->name_len = names.len - start;
                    f->name_copied = true;
                }
                i += m;
                i += jsonwire_consume_whitespace(b + i, n - i);
                i++; /* ':' */
                i += jsonwire_consume_whitespace(b + i, n - i);
            }
            break;
        }
    }

done:
    if (frames != NULL)
        mem_free(a, frames, (size_t)frames_cap * sizeof(JtReorderFrame),
                 _Alignof(JtReorderFrame));
    if (r.members != NULL)
        mem_free(a, r.members, (size_t)r.members_cap * sizeof(JtMember),
                 _Alignof(JtMember));
    burrow__jsonbuf_free(&names);
    burrow__jsonbuf_free(&scratch);
    return err;
}

/* ------------------------------------------------- internals for json v2 */

/* encoderState.UnwriteEmptyObjectMember. prev_name may be NULL. */
bool burrow__jsontext_unwrite_empty_object_member(JsontextEncoder *e,
                                                  const Str *prev_name) {
    JsonState *s = &e->st;
    uint64_t last = s->last;
    if (!jt_e_is_object(last) || !jt_e_need_name(last) || jt_e_len(last) == 0)
        panic_str(JT_LIT("BUG: must be called on an object after writing a value"));
    const Byte *b = e->buf.p;
    Int blen = e->buf.len;
    Int n = 0;
    if (blen >= 3) {
        Byte x = b[blen - 2], y = b[blen - 1];
        if (x == 'l' && y == 'l') {
            n = 4;
        } else if (x == '"' && y == '"') {
            if (b[blen - 3] == '\\')
                return false;
            n = 2;
        } else if ((x == '{' && y == '}') || (x == '[' && y == ']')) {
            n = 2;
        }
    }
    if (n == 0)
        return false;
    blen -= n;
    blen = burrow__jsonwire_trim_suffix_whitespace(b, blen);
    blen = jsonwire_trim_suffix_byte(b, blen, ':');
    blen = burrow__jsonwire_trim_suffix_string(b, blen);
    blen = burrow__jsonwire_trim_suffix_whitespace(b, blen);
    blen = jsonwire_trim_suffix_byte(b, blen, ',');
    e->buf.len = blen;
    s->last -= 2;
    if (!jsonflags_get(&e->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES) &&
        jt_e_active_ns(s->last))
        jt_ns_remove_last(jt_spaces_last(s));
    jt_names_clear_last(s);
    if (prev_name != NULL) {
        jt_names_copy_quoted(s, e->buf.p, e->buf.len);
        jt_names_replace_last_unquoted(s, *prev_name);
    }
    return true;
}

/* encoderState.UnwriteOnlyObjectMemberName. The name comes back in a. */
Str burrow__jsontext_unwrite_only_object_member_name(JsontextEncoder *e, Alloc *a) {
    JsonState *s = &e->st;
    if (!jt_e_is_object(s->last) || jt_e_len(s->last) != 1)
        panic_str(JT_LIT("BUG: must be called on an object after writing first name"));
    Int blen = burrow__jsonwire_trim_suffix_string(e->buf.p, e->buf.len);
    const Byte *q = e->buf.p + blen;
    Int qn = e->buf.len - blen;
    bool verbatim = memchr(q, '\\', (size_t)qn) == NULL;
    JsonBuf tmp = {NULL, 0, 0, a, true, false};
    Str name = burrow__jsonwire_unquote_may_copy(q, qn, verbatim, &tmp);
    if (verbatim) {
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(name.len > 0 ? name.len : 1), 1);
        if (p != NULL && name.len > 0)
            memcpy(p, name.p, (size_t)name.len);
        name = str_from_bytes(p, p == NULL ? 0 : name.len);
    }
    e->buf.len = burrow__jsonwire_trim_suffix_whitespace(e->buf.p, blen);
    s->last--;
    if (!jsonflags_get(&e->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES) &&
        jt_e_active_ns(s->last))
        jt_ns_remove_last(jt_spaces_last(s));
    jt_names_clear_last(s);
    return name;
}

/* encoderState.AppendRaw. fn appends the token's text: for a string, the
 * part between the quotes. */
Error burrow__jsontext_append_raw(JsontextEncoder *e, JsontextKind k, bool safe_ascii,
                                  Error (*fn)(JsonBuf *b, void *ctx), void *ctx) {
    JsonState *s = &e->st;
    JsonBuf *b = &e->buf;
    Int saved = b->len;
    jt_append_before(e, b, k);
    Int pos = b->len;
    Error err = BURROW_NO_ERROR;
    switch (k) {
    case '"': {
        jsonbuf_byte(b, '"');
        err = fn(b, ctx);
        if (BURROW_FAILED(err)) {
            b->len = saved;
            b->failed = false;
            return err;
        }
        jsonbuf_byte(b, '"');
        if (b->failed)
            break;
        Int in = b->len - pos - 2;
        bool verbatim = safe_ascii || !burrow__jsonwire_need_escape(b->p + pos + 1, in);
        if (!verbatim) {
            e->avail.len = 0;
            jsonbuf_put(&e->avail, b->p + pos + 1, in);
            if (e->avail.failed) {
                e->avail.failed = false;
                err = burrow_err_out_of_memory;
                break;
            }
            b->len = pos;
            err = burrow__jsonwire_append_quote(b, e->avail.p, e->avail.len, &e->opts);
            e->avail.len = 0;
            if (BURROW_FAILED(err))
                break;
        }
        if (jt_e_need_name(s->last)) {
            if (!jsonflags_get(&e->opts, JSONFLAG_ALLOW_DUPLICATE_NAMES)) {
                if (!jt_e_valid_ns(s->last)) {
                    err = jt_err_invalid_namespace;
                    break;
                }
                if (jt_e_active_ns(s->last)) {
                    int r = jt_ns_insert_quoted(jt_spaces_last(s), s->a, b->p + pos,
                                                b->len - pos, verbatim);
                    if (r < 0) {
                        err = burrow_err_out_of_memory;
                        break;
                    }
                    if (r == 0) {
                        jt_wrap_with_object_name(s, b->p + pos, b->len - pos);
                        err = jsontext_err_duplicate_name;
                        break;
                    }
                }
            }
            jt_names_replace_last_quoted(s, pos);
        }
        err = jt_append_string(s);
        break;
    }
    case '0':
        err = fn(b, ctx);
        if (BURROW_FAILED(err)) {
            b->len = saved;
            b->failed = false;
            return err;
        }
        if (b->failed)
            break;
        err = jt_append_number(s);
        break;
    default:
        panic_str(JT_LIT("BUG: invalid kind"));
    }
    return jt_encoder_finish(e, saved, err, pos);
}

/* encoderState.AppendIndent, on the encoder's own buffer. */
void burrow__jsontext_append_indent(JsontextEncoder *e, Int n) {
    jt_append_indent(e, &e->buf, n);
}

/* encoderState.NeedFlush and Flush. */
bool burrow__jsontext_need_flush(const JsontextEncoder *e) {
    return jt_need_flush(e);
}

Error burrow__jsontext_flush(JsontextEncoder *e) {
    return jt_flush(e);
}

/* decoderState.SkipUntil. */
Error burrow__jsontext_skip_until(JsontextDecoder *d, Int depth, int64_t length) {
    for (;;) {
        Int dd = jt_depth(&d->st);
        if (!(dd > depth || (dd == depth && jt_e_len(d->st.last) < length)))
            return BURROW_NO_ERROR;
        Error err = BURROW_NO_ERROR;
        (void)jsontext_decoder_read_token(d, &err);
        if (BURROW_FAILED(err))
            return err;
    }
}

/* decoderState.AtEOF and CheckEOF. */
bool burrow__jsontext_at_eof(JsontextDecoder *d) {
    Int pos = d->db.prev_end;
    return jt_is(jt_consume_ws(d, &pos), io_err_unexpected_eof);
}

Error burrow__jsontext_check_eof(JsontextDecoder *d) {
    return jt_check_eof(d, d->db.prev_end);
}

/* decodeBuffer.PreviousTokenOrValue. Empty when there is none to give. */
Slice burrow__jsontext_previous_token_or_value(const JsontextDecoder *d) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    const Byte *b = d->db.buf + d->db.prev_start;
    Int n = d->db.prev_end - d->db.prev_start;
    if (d->peek_pos > 0 || (n > 0 && b[0] == JT_INVALIDATE_BYTE))
        return none;
    if (n == 0) {
        static const char *const toks[] = {"null", "false", "true", "{", "}", "[", "]"};
        b = d->db.buf;
        n = d->db.prev_end;
        for (size_t i = 0; i < sizeof(toks) / sizeof(toks[0]); i++) {
            Int tl = (Int)strlen(toks[i]);
            if (n >= tl && memcmp(b + n - tl, toks[i], (size_t)tl) == 0) {
                Slice r = {(void *)(uintptr_t)(b + n - tl), tl, tl, TYPE_BYTE};
                return r;
            }
        }
    }
    Slice r = {(void *)(uintptr_t)b, n, n, TYPE_BYTE};
    return r;
}

/* ------------------------------------------------------------------- values */

JsontextKind jsontext_value_kind(JsontextValue v) {
    const Byte *p = (const Byte *)v.p;
    Int i = jsonwire_consume_whitespace(p, v.len);
    return i < v.len ? jt_norm(p[i]) : 0;
}

JsontextValue jsontext_value_clone(JsontextValue v, Alloc *a) {
    JsontextValue out = {NULL, 0, 0, TYPE_BYTE};
    if (v.p == NULL)
        return out;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(v.len > 0 ? v.len : 1), 1);
    if (p == NULL)
        return out;
    if (v.len > 0)
        memcpy(p, v.p, (size_t)v.len);
    out.p = p;
    out.len = v.len;
    out.cap = v.len;
    return out;
}

Str jsontext_value_string(JsontextValue v) {
    if (v.p == NULL)
        return JT_LIT("null");
    return str_from_bytes((const Byte *)v.p, v.len);
}

static bool jt_is_valid(JsontextValue v, const JsontextOptions *o) {
    JsontextDecoder d;
    jt_decoder_init(&d, heap_allocator());
    IoReader none = {NULL, NULL};
    jt_decoder_setup(&d, none, false, (Byte *)v.p, v.len, o);
    Error err = BURROW_NO_ERROR;
    unsigned flags = 0;
    (void)jt_read_value(&d, &flags, &err);
    bool ok = false;
    if (BURROW_OK(err)) {
        (void)jsontext_decoder_read_token(&d, &err);
        ok = jt_is(err, io_eof);
    }
    jt_decoder_release(&d);
    return ok;
}

bool jsontext_value_is_valid(JsontextValue v, Slice opts) {
    JsontextOptions o = jt_join_slice(opts);
    return jt_is_valid(v, &o);
}

bool jsontext_value_is_valid_v(JsontextValue v, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    return jt_is_valid(v, &o);
}

/* Copies n bytes over *v, in its own array when there is room and in a new
 * one from a when there is not, which is append((*v)[:0], p...). */
static Error jt_value_assign(JsontextValue *v, Alloc *a, const Byte *p, Int n) {
    if (v->p == NULL || v->cap < n) {
        Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
        if (q == NULL)
            return burrow_err_out_of_memory;
        v->p = q;
        v->cap = n;
    }
    if (n > 0)
        memmove(v->p, p, (size_t)n);
    v->len = n;
    return BURROW_NO_ERROR;
}

/* Value.format. o1 is what the buffered encoder is made with and o2 what
 * gets joined after, which matters because only o1 sees
 * InitializeMultiline. */
static Error jt_value_format(JsontextValue *v, Alloc *a, const JsontextOptions *o1,
                             const JsontextOptions *o2) {
    JsontextEncoder e;
    jt_encoder_init(&e, heap_allocator());
    IoWriter none = {NULL, NULL};
    jt_encoder_setup(&e, none, false, o1);
    burrow__jsonopts_join(&e.opts, o2);
    jsonflags_set(&e.opts, JSONFLAG_OMIT_TOP_LEVEL_NEWLINE | 1);
    Error err = jsontext_encoder_write_value(&e, *v);
    if (BURROW_OK(err) && (e.buf.len != v->len ||
                           (v->len > 0 && memcmp(e.buf.p, v->p, (size_t)v->len) != 0)))
        err = jt_value_assign(v, a, e.buf.p, e.buf.len);
    jt_encoder_release(&e);
    return err;
}

enum { JT_FORMAT, JT_COMPACT, JT_INDENT, JT_CANONICALIZE };

static JsontextOptions jt_format_defaults(int which) {
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    JsontextOptions x;
    switch (which) {
    case JT_COMPACT:
    case JT_INDENT:
        x = jsontext_allow_duplicate_names(true);
        burrow__jsonopts_join(&o, &x);
        x = jsontext_allow_invalid_utf8(true);
        burrow__jsonopts_join(&o, &x);
        x = jsontext_preserve_raw_strings(true);
        burrow__jsonopts_join(&o, &x);
        if (which == JT_INDENT) {
            x = jsontext_multiline(true);
            burrow__jsonopts_join(&o, &x);
        }
        break;
    case JT_CANONICALIZE:
        x = jsontext_canonicalize_raw_ints(true);
        burrow__jsonopts_join(&o, &x);
        x = jsontext_canonicalize_raw_floats(true);
        burrow__jsonopts_join(&o, &x);
        x = jsontext_reorder_raw_objects(true);
        burrow__jsonopts_join(&o, &x);
        break;
    default:
        break;
    }
    return o;
}

static Error jt_value_format_slice(JsontextValue *v, Alloc *a, int which, Slice opts) {
    JsontextOptions o1 = jt_format_defaults(which);
    JsontextOptions o2 = jt_join_slice(opts);
    return jt_value_format(v, a, &o1, &o2);
}

static Error jt_value_format_va(JsontextValue *v, Alloc *a, int which, int n,
                                va_list ap) {
    JsontextOptions o1 = jt_format_defaults(which);
    JsontextOptions o2 = jt_join_va(n, ap);
    return jt_value_format(v, a, &o1, &o2);
}

Error jsontext_value_format(JsontextValue *v, Alloc *a, Slice opts) {
    return jt_value_format_slice(v, a, JT_FORMAT, opts);
}

Error jsontext_value_format_v(JsontextValue *v, Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Error err = jt_value_format_va(v, a, JT_FORMAT, n, ap);
    va_end(ap);
    return err;
}

Error jsontext_value_compact(JsontextValue *v, Alloc *a, Slice opts) {
    return jt_value_format_slice(v, a, JT_COMPACT, opts);
}

Error jsontext_value_compact_v(JsontextValue *v, Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Error err = jt_value_format_va(v, a, JT_COMPACT, n, ap);
    va_end(ap);
    return err;
}

Error jsontext_value_indent(JsontextValue *v, Alloc *a, Slice opts) {
    return jt_value_format_slice(v, a, JT_INDENT, opts);
}

Error jsontext_value_indent_v(JsontextValue *v, Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Error err = jt_value_format_va(v, a, JT_INDENT, n, ap);
    va_end(ap);
    return err;
}

Error jsontext_value_canonicalize(JsontextValue *v, Alloc *a, Slice opts) {
    return jt_value_format_slice(v, a, JT_CANONICALIZE, opts);
}

Error jsontext_value_canonicalize_v(JsontextValue *v, Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Error err = jt_value_format_va(v, a, JT_CANONICALIZE, n, ap);
    va_end(ap);
    return err;
}

Slice jsontext_value_marshal_json(JsontextValue v, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    if (v.p == NULL) {
        Slice s = {(void *)(uintptr_t)jt_raw_null.buf, 4, 4, TYPE_BYTE};
        return s;
    }
    return v;
}

Error jsontext_value_unmarshal_json(JsontextValue *v, Alloc *a, Slice b) {
    if (v == NULL)
        return jt_err_unmarshal_nil;
    return jt_value_assign(v, a, (const Byte *)b.p, b.len);
}

/* ---------------------------------------------------------------- appending */

static JsonBuf jt_dst_buf(Alloc *a, Slice dst) {
    JsonBuf b = {(Byte *)dst.p, dst.len, dst.cap, a, false, false};
    return b;
}

static Slice jt_buf_slice(const JsonBuf *b) {
    Slice s = {b->p, b->len, b->cap, TYPE_BYTE};
    return s;
}

Slice jsontext_append_float(Alloc *a, Slice dst, double src, Int bits) {
    if (bits != 32 && bits != 64)
        panic_str(JT_LIT("illegal AppendFloat bit size"));
    JsonBuf b = jt_dst_buf(a, dst);
    burrow__jsonwire_append_float(&b, src, (int)bits);
    return b.failed ? dst : jt_buf_slice(&b);
}

Slice jsontext_append_quote(Alloc *a, Slice dst, Str src, Error *err) {
    JsonBuf b = jt_dst_buf(a, dst);
    JsontextOptions none;
    memset(&none, 0, sizeof(none));
    Error e = burrow__jsonwire_append_quote(&b, src.p, src.len, &none);
    if (b.failed) {
        *err = burrow_err_out_of_memory;
        return dst;
    }
    *err =
        BURROW_FAILED(e) ? jt_syntactic_new(0, BURROW_STR_EMPTY, e) : BURROW_NO_ERROR;
    return jt_buf_slice(&b);
}

Slice jsontext_append_unquote(Alloc *a, Slice dst, Str src, Error *err) {
    JsonBuf b = jt_dst_buf(a, dst);
    Error e = burrow__jsonwire_append_unquote(&b, src.p, src.len);
    if (b.failed) {
        *err = burrow_err_out_of_memory;
        return dst;
    }
    *err =
        BURROW_FAILED(e) ? jt_syntactic_new(0, BURROW_STR_EMPTY, e) : BURROW_NO_ERROR;
    return jt_buf_slice(&b);
}

static Slice jt_append_format(Alloc *a, Slice dst, Str src, const JsontextOptions *o,
                              Error *err) {
    JsontextEncoder e;
    jt_encoder_init(&e, heap_allocator());
    IoWriter none = {NULL, NULL};
    jt_encoder_setup(&e, none, false, o);
    jsonflags_set(&e.opts, JSONFLAG_OMIT_TOP_LEVEL_NEWLINE | 1);
    JsontextValue v = {(void *)(uintptr_t)src.p, src.len, src.len, TYPE_BYTE};
    *err = jsontext_encoder_write_value(&e, v);
    JsonBuf b = jt_dst_buf(a, dst);
    if (BURROW_FAILED(*err))
        jsonbuf_str(&b, src);
    else
        jsonbuf_put(&b, e.buf.p, e.buf.len);
    jt_encoder_release(&e);
    if (b.failed) {
        *err = burrow_err_out_of_memory;
        return dst;
    }
    return jt_buf_slice(&b);
}

Slice jsontext_append_format(Alloc *a, Slice dst, Str src, Slice opts, Error *err) {
    JsontextOptions o = jt_join_slice(opts);
    return jt_append_format(a, dst, src, &o, err);
}

Slice jsontext_append_format_v(Alloc *a, Slice dst, Str src, Error *err, int n, ...) {
    va_list ap;
    va_start(ap, n);
    JsontextOptions o = jt_join_va(n, ap);
    va_end(ap);
    return jt_append_format(a, dst, src, &o, err);
}
