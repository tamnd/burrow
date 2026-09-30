/* encoding/json/v2: Go values to JSON and back.
 *
 * This is the upper half of Go's json v2, built on encoding/json/jsontext. A
 * value goes in as an Any, which says what type it is and where it lives, and
 * the type's descriptor is what says how to write it, so a struct declared
 * with BURROW_STRUCT marshals the way the same struct does in Go, field tags
 * and all:
 *
 *     #define POINT_FIELDS(F, T)             \
 *         F(T, Int, X, "json:\"x\"")         \
 *         F(T, Int, Y, "json:\"y,omitzero\"")
 *
 *     BURROW_STRUCT(Point, POINT_FIELDS);
 *
 *     Point p = {1, 0};
 *     Error err = BURROW_NO_ERROR;
 *     Slice out = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Point), &p), &err, 0);
 *
 * gives {"x":1}, and reading it back is
 *
 *     Point q = {0};
 *     err = jsonv2_unmarshal_v(a, out, BURROW_ANY(TYPE_OF(Point), &q), 0);
 *
 * where the Any points at the value to fill in, which is what Go's pointer
 * argument to Unmarshal is.
 *
 * Unmarshal allocates whatever the value needs, strings, slices, maps and the
 * values pointers and interfaces point at, from the allocator it is given.
 * It never frees what a value held before, since that memory might not be
 * its to free, which is what a garbage collector would sort out in Go. An
 * arena is the right thing to hand it. Scratch space needed along the way,
 * for sorting map keys and the like, comes from the heap and is given back
 * before the call returns.
 *
 * Options are the same JsontextOptions jsontext uses, so the ones from there
 * work here too, and later options win over earlier ones.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/json/v2 */

#ifndef BURROW_ENCODING_JSON_V2_H
#define BURROW_ENCODING_JSON_V2_H

#include "burrow/core.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ options */

/* json.StringifyNumbers. Writes numbers as JSON strings holding the number,
 * and reads them back only from strings. */
JsontextOptions jsonv2_stringify_numbers(bool v);

/* json.Deterministic. Writes the members of a map in sorted order, so the
 * same value always gives the same bytes. */
JsontextOptions jsonv2_deterministic(bool v);

/* json.FormatNilSliceAsNull. A nil slice is null rather than []. */
JsontextOptions jsonv2_format_nil_slice_as_null(bool v);

/* json.FormatNilMapAsNull. A nil map is null rather than {}. */
JsontextOptions jsonv2_format_nil_map_as_null(bool v);

/* json.OmitZeroStructFields. Leaves out every struct field holding its zero
 * value, as if each had the omitzero tag option. */
JsontextOptions jsonv2_omit_zero_struct_fields(bool v);

/* json.MatchCaseInsensitiveNames. Lets a member name match a struct field
 * whose name differs only in case and in dashes and underscores. */
JsontextOptions jsonv2_match_case_insensitive_names(bool v);

/* json.RejectUnknownMembers. A member name that matches no struct field is an
 * error that wraps jsonv2_err_unknown_name, rather than being skipped. */
JsontextOptions jsonv2_reject_unknown_members(bool v);

/* json.JoinOptions. The options in order, later ones winning, as one. */
JsontextOptions jsonv2_join_options(Slice srcs);
JsontextOptions jsonv2_join_options_v(int n, ...);

/* json.DefaultOptionsV2. Every option json v2 has an opinion on, set to what
 * it does when nothing is said. */
JsontextOptions jsonv2_default_options_v2(void);

/* json.GetOption for the options that are true or false, which is all of the
 * setters above and the boolean ones in jsontext. Whether opts says anything
 * about the option setter makes, and in *value what it says, false when it
 * says nothing. value may be NULL. */
bool jsonv2_get_option(JsontextOptions opts, JsontextOptions (*setter)(bool v),
                       bool *value);

/* json.GetOption for jsontext_with_indent and jsontext_with_indent_prefix. The
 * Str points into whatever opts was built from. */
bool jsonv2_get_option_str(JsontextOptions opts, JsontextOptions (*setter)(Str v),
                           Str *value);

/* ------------------------------------------------------------------- errors */

/* json.ErrUnknownName, "unknown object member name". */
extern const Error jsonv2_err_unknown_name;

/* json.SemanticError, which is every problem with how a Go value and a JSON
 * value line up, as opposed to a problem with the JSON itself. action is
 * "marshal" or "unmarshal", which Go keeps to itself and prints. byte_offset
 * and json_pointer say where in the JSON, json_kind and json_value what was
 * there, which are zero when unknown, go_type what it was going to or coming
 * from, and err what went wrong, which may be nil. errors_is sees through to
 * err and errors_as with TYPE_JSONV2_SEMANTIC_ERROR gets you the struct. The
 * ones marshal and unmarshal return live in the calling goroutine's error
 * arena. */
typedef struct Jsonv2SemanticError {
    Str action;
    int64_t byte_offset;
    JsontextPointer json_pointer;
    JsontextKind json_kind;
    JsontextValue json_value;
    const Type *go_type;
    Error err;
} Jsonv2SemanticError;

extern const Type *const TYPE_JSONV2_SEMANTIC_ERROR;

/* Go's Error method, built in a, such as
 *
 *     json: cannot unmarshal JSON string into Go int within "/x" */
BURROW_OWNS(ret) Str jsonv2_semantic_error_error(const Jsonv2SemanticError *e,
                                                 Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error jsonv2_semantic_error_unwrap(const Jsonv2SemanticError *e);

/* An Error for a Jsonv2SemanticError you filled in yourself. The message and
 * copies of the pointer and the value are built in a, and err is kept as it
 * is. Out of memory gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error jsonv2_semantic_error_as_error(const Jsonv2SemanticError *e,
                                                      Alloc *a);

/* ------------------------------------------------------------------- types */

/* map[string]any and []any, which is what an Any holding nothing becomes when
 * it is unmarshalled from a JSON object or array. The map is a Map * and the
 * slice a Slice of Any, and each Any in them holds a bool, a Str, a double
 * (TYPE_FLOAT64), one of these two again, or nothing for null. */
extern const Type *const TYPE_JSONV2_MAP_STRING_ANY;
extern const Type *const TYPE_JSONV2_SLICE_ANY;

/* ---------------------------------------------------------------- marshal */

/* json.Marshal. in as JSON, in a new array from a. A nil Any, or one holding
 * a nil pointer, is null. On an error *err says why and the result is what
 * was written before it went wrong, as in Go; err may be NULL. */
BURROW_OWNS(ret) Slice jsonv2_marshal(Alloc *a, Any in, Slice opts, Error *err);
BURROW_OWNS(ret) Slice jsonv2_marshal_v(Alloc *a, Any in, Error *err, int n, ...);

/* json.MarshalWrite. in as JSON, written to out, with a buffering encoder
 * from a. No newline goes after it. */
BURROW_STATIC(ret) Error jsonv2_marshal_write(Alloc *a, IoWriter out, Any in,
                                              Slice opts);
BURROW_STATIC(ret) Error jsonv2_marshal_write_v(Alloc *a, IoWriter out, Any in, int n,
                                                ...);

/* json.MarshalEncode. in as the next JSON value out writes. The options are
 * joined onto the encoder's own for this call and no longer. */
BURROW_STATIC(ret) Error jsonv2_marshal_encode(JsontextEncoder *out, Any in,
                                               Slice opts);
BURROW_STATIC(ret) Error jsonv2_marshal_encode_v(JsontextEncoder *out, Any in, int n,
                                                 ...);

/* -------------------------------------------------------------- unmarshal */

/* json.Unmarshal. Reads the one JSON value in into the value out points at,
 * which has to be of type out.t. What the value needs is allocated from a.
 * Anything after the value other than white space is an error. */
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal(Alloc *a, Slice in, Any out,
                                                           Slice opts);
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal_v(Alloc *a, Slice in,
                                                             Any out, int n, ...);

/* json.UnmarshalRead. The same, reading from in until io_eof. */
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal_read(Alloc *a, IoReader in,
                                                                Any out, Slice opts);
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal_read_v(Alloc *a, IoReader in,
                                                                  Any out, int n, ...);

/* json.UnmarshalDecode. Reads the next JSON value in has, and nothing more,
 * with in's options and these joined for this call. */
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal_decode(Alloc *a,
                                                                  JsontextDecoder *in,
                                                                  Any out, Slice opts);
BURROW_OWNS(out) BURROW_STATIC(ret) Error jsonv2_unmarshal_decode_v(Alloc *a,
                                                                    JsontextDecoder *in,
                                                                    Any out, int n,
                                                                    ...);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_JSON_V2_H */
