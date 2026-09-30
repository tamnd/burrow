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
#include "burrow/encoding.h"
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

/* ------------------------------------------------------------------ methods
 *
 * A type decides how it becomes JSON by listing one of Go's methods in its
 * descriptor, the same way it takes part in the encoding package:
 *
 *     static Error celsius_marshal_json_to(Celsius *c, JsontextEncoder *enc) {
 *         return jsontext_encoder_write_token(enc, jsontext_float(c->deg));
 *     }
 *
 *     #define CELSIUS_METHODS(M, T) \
 *         M(T, MarshalJSONTo, celsius_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)
 *     BURROW_STRUCT_DEFINE_METHODS(Celsius, CELSIUS_FIELDS, CELSIUS_METHODS);
 *
 * Marshal looks for MarshalJSONTo, then MarshalJSON, then AppendText, then
 * MarshalText, and uses the first one the type has. Unmarshal looks for
 * UnmarshalJSONFrom, then UnmarshalJSON, then UnmarshalText. Text is written
 * and read as a JSON string. The encoding package's signatures are the ones
 * for the three text methods, ENCODING_SIG_APPEND_TEXT and the others.
 *
 * MarshalJSONTo and UnmarshalJSONFrom have to write or read exactly one JSON
 * value. Either may return errors_err_unsupported without touching the coder,
 * and the next method down the list is used instead, or the type's own rules
 * when there is none. The other methods may not return it.
 *
 * Every burrow method takes its receiver as a pointer, so there is no split
 * between the methods of T and those of *T here. A method is called on the
 * value it belongs to, never on a nil pointer, and a pointer type is followed
 * to the value first, as Go does. The same goes for IsZero, which omitzero
 * asks when a field's type has it, as bool f(T *self).
 *
 * The Alloc an unmarshal method gets is the one Unmarshal was given, so what
 * it keeps goes where the rest of the value goes. The bytes MarshalJSON and
 * MarshalText return come from scratch space that is freed once they have
 * been copied out. */

/* JsontextEncoder * and JsontextDecoder * as one token each, for the
 * signature lists. */
typedef JsontextEncoder *Jsonv2EncoderArg;
typedef JsontextDecoder *Jsonv2DecoderArg;
extern const Type burrow_type_Jsonv2EncoderArg;
extern const Type burrow_type_Jsonv2DecoderArg;

/* Error marshal_json_to(T *self, JsontextEncoder *enc) */
#define JSONV2_SIG_MARSHAL_JSON_TO(IN, OUT) IN(0, Jsonv2EncoderArg) OUT(Error)

/* Slice marshal_json(T *self, Alloc *a, Error *err) */
#define JSONV2_SIG_MARSHAL_JSON(IN, OUT) ENCODING_SIG_MARSHAL_BINARY(IN, OUT)

/* Error unmarshal_json_from(T *self, Alloc *a, JsontextDecoder *dec) */
#define JSONV2_SIG_UNMARSHAL_JSON_FROM(IN, OUT)                                        \
    IN(0, EncodingAllocArg) IN(1, Jsonv2DecoderArg) OUT(Error)

/* Error unmarshal_json(T *self, Alloc *a, Slice data) */
#define JSONV2_SIG_UNMARSHAL_JSON(IN, OUT) ENCODING_SIG_UNMARSHAL_BINARY(IN, OUT)

/* bool is_zero(T *self) */
#define JSONV2_SIG_IS_ZERO(IN, OUT) OUT(bool)

/* json.Marshaler, json.MarshalerTo, json.Unmarshaler and
 * json.UnmarshalerFrom, for code that wants to hold one. Marshal and
 * Unmarshal go by the methods the held value's own type lists, as they do for
 * any other interface value, so the vtable is for your code and not theirs. */
typedef struct Jsonv2MarshalerVT {
    const Type *self_type;
    Slice (*marshal_json)(void *self, Alloc *a, Error *err);
} Jsonv2MarshalerVT;

typedef struct Jsonv2Marshaler {
    const Jsonv2MarshalerVT *vt;
    void *data;
} Jsonv2Marshaler;

typedef struct Jsonv2MarshalerToVT {
    const Type *self_type;
    Error (*marshal_json_to)(void *self, JsontextEncoder *enc);
} Jsonv2MarshalerToVT;

typedef struct Jsonv2MarshalerTo {
    const Jsonv2MarshalerToVT *vt;
    void *data;
} Jsonv2MarshalerTo;

typedef struct Jsonv2UnmarshalerVT {
    const Type *self_type;
    Error (*unmarshal_json)(void *self, Alloc *a, Slice data);
} Jsonv2UnmarshalerVT;

typedef struct Jsonv2Unmarshaler {
    const Jsonv2UnmarshalerVT *vt;
    void *data;
} Jsonv2Unmarshaler;

typedef struct Jsonv2UnmarshalerFromVT {
    const Type *self_type;
    Error (*unmarshal_json_from)(void *self, Alloc *a, JsontextDecoder *dec);
} Jsonv2UnmarshalerFromVT;

typedef struct Jsonv2UnmarshalerFrom {
    const Jsonv2UnmarshalerFromVT *vt;
    void *data;
} Jsonv2UnmarshalerFrom;

extern const Type burrow_type_Jsonv2Marshaler;
extern const Type burrow_type_Jsonv2MarshalerTo;
extern const Type burrow_type_Jsonv2Unmarshaler;
extern const Type burrow_type_Jsonv2UnmarshalerFrom;

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
