/* encoding/json: the original JSON API, kept working on top of json v2.
 *
 * Go 1.25 rebuilt encoding/json as a thin layer over encoding/json/v2, with
 * v2 running under a set of options that bring back each thing v1 did
 * differently. This is that layer. json_marshal is jsonv2_marshal with
 * json_default_options_v1, and the errors come back as the types v1 always
 * had, SyntaxError and UnmarshalTypeError and the rest, with the same text:
 *
 *     #define POINT_FIELDS(F, T)  \
 *         F(T, Int, X, "")        \
 *         F(T, Int, Y, "json:\"y\"")
 *
 *     BURROW_STRUCT(Point, POINT_FIELDS);
 *
 *     Point p = {1, 2};
 *     Error err = BURROW_NO_ERROR;
 *     Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(Point), &p), &err);
 *
 * gives {"X":1,"y":2}, and
 *
 *     err = json_unmarshal(a, BURROW_B("{\"x\":\"one\"}"),
 *                          BURROW_ANY(TYPE_OF(Point), &p));
 *
 * fails with json: cannot unmarshal string into Go struct field Point.X of
 * type int, since v1 matches names without regard to case.
 *
 * What v1 does that v2 does not, in short: map keys are sorted, nil slices
 * and maps are null, <, > and & are escaped, names match without regard to
 * case, a [N]byte is an array of numbers, the string tag option follows the
 * old rules, and a method on a map key is not called for the key. The options
 * below turn each of these on or off one at a time, for use with v2's
 * functions when only some of them are wanted.
 *
 * Unmarshal allocates from the Alloc it is given in the same way v2 does, so
 * an arena is the natural thing to give it. The errors live in the calling
 * goroutine's error arena, like every other error.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/json */

#ifndef BURROW_ENCODING_JSON_H
#define BURROW_ENCODING_JSON_H

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- functions */

/* json.Marshal. v as JSON, from a. On an error nothing is returned, which is
 * where v1 differs from v2, which returns what it wrote before the error. */
BURROW_OWNS(ret) Slice json_marshal(Alloc *a, Any v, Error *err);

/* json.MarshalIndent. json_marshal followed by json_indent, with the result
 * starting on no prefix and each later line on prefix and one indent per
 * level. */
BURROW_OWNS(ret) Slice json_marshal_indent(Alloc *a, Any v, Str prefix, Str indent,
                                           Error *err);

/* json.Unmarshal. Reads the JSON in data into the value v points at, which
 * has to point somewhere: an Any with no type is json: Unmarshal(nil), and
 * one with a type and no data is json: Unmarshal(nil *T). Any is a pointer
 * already, so the non-pointer case Go has cannot happen. The whole of data is
 * checked before anything is stored, so a syntax error leaves v as it was. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error json_unmarshal(Alloc *a, Slice data, Any v);

/* json.Valid. Whether data is one JSON value with nothing but whitespace
 * around it. */
bool json_valid(Slice data);

/* json.Compact. src with the insignificant whitespace taken out, appended to
 * dst. On an error nothing is appended. */
BURROW_STATIC(ret) Error json_compact(BytesBuffer *dst, Slice src);

/* json.Indent. src with each element of an object or array on its own line,
 * starting with prefix and then one indent per level, appended to dst. The
 * first line gets no prefix, so the result can be pasted into other indented
 * JSON. Whitespace at the start of src is dropped and whitespace at the end
 * is kept. On an error nothing is appended. */
BURROW_STATIC(ret) Error json_indent(BytesBuffer *dst, Slice src, Str prefix,
                                     Str indent);

/* json.HTMLEscape. src with <, >, &, U+2028 and U+2029 written as \u escapes,
 * appended to dst, which makes JSON safe to put inside a <script> tag. Only
 * strings can hold these, so src is not parsed. Out of memory leaves dst as
 * it was. */
void json_html_escape(BytesBuffer *dst, Slice src);

/* ----------------------------------------------------------------- options
 *
 * Each of these is one of the things v1 does differently, as a JsontextOptions
 * to hand to jsonv2_marshal and the rest. json_default_options_v1 is all of
 * them together, plus the jsontext and v2 options that v1 needs, and it is
 * what json_marshal and json_unmarshal use. */

/* json.DefaultOptionsV1. Every option below set to true, along with
 * jsonv2_deterministic, jsonv2_format_nil_map_as_null,
 * jsonv2_format_nil_slice_as_null, jsonv2_match_case_insensitive_names,
 * jsontext_allow_duplicate_names, jsontext_allow_invalid_utf8,
 * jsontext_escape_for_html, jsontext_escape_for_js and
 * jsontext_preserve_raw_strings. */
JsontextOptions json_default_options_v1(void);

/* json.CallMethodsWithLegacySemantics. The JSON methods, v1's and v2's, are
 * not called for a map key, which falls back to the text methods or to its
 * own kind. Go's other half of this, calling a method on a pointer receiver
 * only when the value is addressable, has nothing to act on here, since every
 * burrow method takes its receiver as a pointer and is always called. */
JsontextOptions json_call_methods_with_legacy_semantics(bool v);

/* json.FormatByteArrayAsArray. A [N]byte is written as an array of numbers
 * rather than as base64. */
JsontextOptions json_format_byte_array_as_array(bool v);

/* json.FormatBytesWithLegacySemantics. A []byte of a named byte type, or with
 * methods, is base64 like any other []byte, and an array of bytes that is not
 * base64 is read into a []byte. */
JsontextOptions json_format_bytes_with_legacy_semantics(bool v);

/* json.FormatDurationAsNano. A time.Duration is a number of nanoseconds. */
JsontextOptions json_format_duration_as_nano(bool v);

/* json.MatchCaseSensitiveDelimiter. Matching names without regard to case
 * also ignores dashes and underscores unless this is set, which v1 sets. */
JsontextOptions json_match_case_sensitive_delimiter(bool v);

/* json.MergeWithLegacySemantics. Unmarshal merges the way v1 did. A null
 * leaves a bool, number, string, array or struct as it was and zeroes the
 * rest, and any other value is read into the elements of an array or slice
 * that are already there, even past a slice's length, rather than replacing
 * them. */
JsontextOptions json_merge_with_legacy_semantics(bool v);

/* json.OmitEmptyWithLegacySemantics. omitempty means the Go value is empty,
 * false, 0, a nil pointer or a zero length string, slice or map, rather than
 * the JSON value being empty. */
JsontextOptions json_omit_empty_with_legacy_semantics(bool v);

/* json.ParseBytesWithLooseRFC4648. base64 may have \r and \n in it. */
JsontextOptions json_parse_bytes_with_loose_rfc4648(bool v);

/* json.ParseTimeWithLooseRFC3339. A time.Time is read with the looser rules
 * Go's time.Parse has for RFC 3339. */
JsontextOptions json_parse_time_with_loose_rfc3339(bool v);

/* json.ReportErrorsWithLegacySemantics. Errors are the v1 types below, each
 * JSON value is checked in full before it is read, and some mistakes that v2
 * stops at are carried on past. */
JsontextOptions json_report_errors_with_legacy_semantics(bool v);

/* json.StringifyWithLegacySemantics. The string tag option quotes bools and
 * strings as well as numbers, and only acts on a field whose type is one of
 * those or a pointer to one. Reading such a field accepts Go's number grammar
 * inside the quotes and a quoted null. */
JsontextOptions json_stringify_with_legacy_semantics(bool v);

/* json.UnmarshalArrayFromAnyLength. A Go array is filled from a JSON array of
 * any length, the rest zeroed or the extra elements dropped. */
JsontextOptions json_unmarshal_array_from_any_length(bool v);

/* ------------------------------------------------------------------- types */

/* json.Number, the text of a JSON number, which is what an Any holds for a
 * number when a Decoder is told to use numbers. Marshalling writes the text
 * as it is, or 0 when it is empty, and fails when it is not a number.
 * Unmarshalling reads a number, or a string that holds one. */
typedef Str JsonNumber;

extern const Type burrow_type_JsonNumber;

#define TYPE_JSON_NUMBER TYPE_OF(JsonNumber)

/* Number.String. */
BURROW_BORROWS(ret, n) Str json_number_string(JsonNumber n);

/* Number.Float64, which is strconv_parse_float with 64 bits. */
double json_number_float64(JsonNumber n, Error *err);

/* Number.Int64, which is strconv_parse_int in base 10. */
int64_t json_number_int64(JsonNumber n, Error *err);

/* json.RawMessage, which is jsontext.Value under another name. */
typedef JsontextValue JsonRawMessage;

#define burrow_type_JsonRawMessage burrow_type_JsontextValue
#define TYPE_JSON_RAW_MESSAGE TYPE_OF(JsonRawMessage)

/* ------------------------------------------------------------------ errors
 *
 * Each error type has its struct, which errors_as with the TYPE_ constant
 * gets you, and a _error function that builds Go's message for one you filled
 * in yourself. As with every error type in burrow, the message of one that
 * came back from a call is also what error_text gives. */

/* json.SyntaxError. The JSON could not be parsed. offset is the number of
 * bytes read before the problem. */
typedef struct JsonSyntaxError {
    Str msg;
    int64_t offset;
} JsonSyntaxError;

extern const Type *const TYPE_JSON_SYNTAX_ERROR;

/* msg, which is all a SyntaxError has to say. */
BURROW_BORROWS(ret, e) Str json_syntax_error_error(const JsonSyntaxError *e);

/* json.UnmarshalTypeError. A JSON value did not fit the Go type it was being
 * read into. value says what the JSON was, "bool", "array", "number -5",
 * type is the Go type, offset is where the value ended, struct_name is the
 * name of the type Unmarshal was given, when the error is under it, and field
 * is the path to the value from there, with dots between the parts. err is
 * the reason, when there is one beyond the mismatch. Go calls struct_name
 * Struct, which C has taken. */
typedef struct JsonUnmarshalTypeError {
    Str value;
    const Type *type;
    int64_t offset;
    Str struct_name;
    Str field;
    Error err;
} JsonUnmarshalTypeError;

extern const Type *const TYPE_JSON_UNMARSHAL_TYPE_ERROR;

/* Go's message, built in a, such as
 *
 *     json: cannot unmarshal string into Go struct field Point.X of type Int */
BURROW_OWNS(ret) Str json_unmarshal_type_error_error(const JsonUnmarshalTypeError *e,
                                                     Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error
json_unmarshal_type_error_unwrap(const JsonUnmarshalTypeError *e);

/* json.UnmarshalFieldError. Go stopped returning this in Go 1.2 and keeps it
 * for code that names it, as this does. key is the JSON name, type the type
 * of the field and field_name its name. */
typedef struct JsonUnmarshalFieldError {
    Str key;
    const Type *type;
    Str field_name;
} JsonUnmarshalFieldError;

extern const Type *const TYPE_JSON_UNMARSHAL_FIELD_ERROR;

BURROW_OWNS(ret) Str json_unmarshal_field_error_error(const JsonUnmarshalFieldError *e,
                                                      Alloc *a);

/* json.InvalidUnmarshalError. Unmarshal was given nowhere to put the value.
 * type is the type the Any said it pointed at, or NULL for an Any with no
 * type. Go has the pointer type here and the message spells it that way. */
typedef struct JsonInvalidUnmarshalError {
    const Type *type;
} JsonInvalidUnmarshalError;

extern const Type *const TYPE_JSON_INVALID_UNMARSHAL_ERROR;

/* json: Unmarshal(nil) or json: Unmarshal(nil *T), built in a. */
BURROW_OWNS(ret) Str
json_invalid_unmarshal_error_error(const JsonInvalidUnmarshalError *e, Alloc *a);

/* json.UnsupportedTypeError. Marshal met a type JSON has no way to write,
 * such as a channel or a function. */
typedef struct JsonUnsupportedTypeError {
    const Type *type;
} JsonUnsupportedTypeError;

extern const Type *const TYPE_JSON_UNSUPPORTED_TYPE_ERROR;

/* json: unsupported type: chan int, built in a. */
BURROW_OWNS(ret) Str
json_unsupported_type_error_error(const JsonUnsupportedTypeError *e, Alloc *a);

/* json.UnsupportedValueError. Marshal met a value JSON has no way to write,
 * NaN or an infinity, or a cycle. Go's Value field has not been filled in
 * since v1 moved onto v2 and is left out here. */
typedef struct JsonUnsupportedValueError {
    Str str;
} JsonUnsupportedValueError;

extern const Type *const TYPE_JSON_UNSUPPORTED_VALUE_ERROR;

/* json: unsupported value: NaN, built in a. */
BURROW_OWNS(ret) Str
json_unsupported_value_error_error(const JsonUnsupportedValueError *e, Alloc *a);

/* json.InvalidUTF8Error. Go stopped returning this in Go 1.2, when invalid
 * UTF-8 started being written as U+FFFD, and keeps it for code that names
 * it, as this does. */
typedef struct JsonInvalidUTF8Error {
    Str s;
} JsonInvalidUTF8Error;

extern const Type *const TYPE_JSON_INVALID_UTF8_ERROR;

BURROW_OWNS(ret) Str json_invalid_utf8_error_error(const JsonInvalidUTF8Error *e,
                                                   Alloc *a);

/* json.MarshalerError. A MarshalJSON, MarshalText or other marshal method,
 * or a marshal function from v2's options, failed or wrote something that is
 * not JSON. type is the type the method belongs to, err what went wrong, and
 * source_func the name of what failed, "MarshalJSON" when empty. Go calls
 * the method through a pointer and so has *T here, and the message spells it
 * that way to match. */
typedef struct JsonMarshalerError {
    const Type *type;
    Error err;
    Str source_func;
} JsonMarshalerError;

extern const Type *const TYPE_JSON_MARSHALER_ERROR;

/* Go's message, built in a, such as
 *
 *     json: error calling MarshalJSON for type *main.Boom: boom */
BURROW_OWNS(ret) Str json_marshaler_error_error(const JsonMarshalerError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error json_marshaler_error_unwrap(const JsonMarshalerError *e);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_JSON_H */
