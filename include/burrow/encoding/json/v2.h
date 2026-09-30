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

/* ------------------------------------------------------------- functions
 *
 * json.MarshalFunc and the rest: code of yours that takes over for a type,
 * set up by the caller rather than by the type, and handed to Marshal with
 * jsonv2_with_marshalers. It is how a type you cannot add methods to gets
 * its own form, and how one call writes a type differently from the next:
 *
 *     static Slice yes_no(void *ctx, Alloc *a, Any v, Error *err) {
 *         (void)ctx;
 *         BURROW_OUT(err, BURROW_NO_ERROR);
 *         Str s = *(bool *)v.data ? BURROW_S("\"yes\"") : BURROW_S("\"no\"");
 *         return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
 *     }
 *
 *     Jsonv2Marshalers *m = jsonv2_marshal_func(a, TYPE_BOOL, yes_no, NULL);
 *     out = jsonv2_marshal_v(a, in, &err, 1, jsonv2_with_marshalers(m));
 *
 * The function is asked about every value on the way down, not only the top
 * one: struct fields, slice and array elements, map keys and values, what a
 * pointer points at and what an interface holds.
 *
 * t says which values a function is for, the way Go's type parameter does:
 *
 *   - A type that is not a pointer or an interface takes values of exactly
 *     that type.
 *   - An unnamed pointer to T, one from BURROW_PTR_TYPE, takes values of T.
 *     This is how a function that fills a value in has to be set up, so the
 *     unmarshal side takes only this and interfaces.
 *   - An interface takes values whose type has its methods. TYPE_ANY takes
 *     everything. json's four interfaces, the six in encoding and error are
 *     known by the methods Go gives them, and an interface of your own lists
 *     its methods in its descriptor, where only the names are compared.
 *
 * Anything else, a named pointer or a non-pointer on the unmarshal side,
 * panics, as it does in Go.
 *
 * The function gets the value as an Any, with v.t the value's own type and
 * v.data where it lives, whichever of the three t was. A marshal function
 * should leave it alone. An unmarshal function fills it in, and anything it
 * allocates for it comes from the Alloc it is given, which is the one
 * Unmarshal was given. The bytes a marshal function returns may come from
 * its Alloc, which is scratch space freed once they have been copied out.
 *
 * The rules for what the functions return are the methods' rules. A
 * function that writes to the encoder or reads from the decoder must write or
 * read exactly one value, and may return errors_err_unsupported without
 * touching it to step aside. The value then goes to the next function that
 * takes it, then to the type's methods, then to its default form. The other
 * two may not step aside, and the first of them that takes a value ends the
 * search.
 *
 * Every constructor allocates from a and returns NULL when it runs out of
 * memory. A NULL set is no functions at all. None of it is freed by json;
 * free it with a, once no options that name it are left in use. */
typedef struct Jsonv2Marshalers Jsonv2Marshalers;
typedef struct Jsonv2Unmarshalers Jsonv2Unmarshalers;

/* func(T) ([]byte, error): the JSON for v, which must be one valid value. */
typedef Slice (*Jsonv2MarshalFn)(void *ctx, Alloc *a, Any v, Error *err);

/* func(*jsontext.Encoder, T) error: write v to enc. */
typedef Error (*Jsonv2MarshalToFn)(void *ctx, JsontextEncoder *enc, Any v);

/* func([]byte, T) error: fill in v from data, the next JSON value whole. */
typedef Error (*Jsonv2UnmarshalFn)(void *ctx, Alloc *a, Slice data, Any v);

/* func(*jsontext.Decoder, T) error: fill in v by reading from dec. */
typedef Error (*Jsonv2UnmarshalFromFn)(void *ctx, Alloc *a, JsontextDecoder *dec,
                                       Any v);

/* json.MarshalFunc. ctx is passed to fn untouched on every call. */
BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_marshal_func(Alloc *a, const Type *t,
                                                       Jsonv2MarshalFn fn, void *ctx);

/* json.MarshalToFunc. */
BURROW_OWNS(ret) Jsonv2Marshalers *
jsonv2_marshal_to_func(Alloc *a, const Type *t, Jsonv2MarshalToFn fn, void *ctx);

/* json.UnmarshalFunc. */
BURROW_OWNS(ret) Jsonv2Unmarshalers *
jsonv2_unmarshal_func(Alloc *a, const Type *t, Jsonv2UnmarshalFn fn, void *ctx);

/* json.UnmarshalFromFunc. */
BURROW_OWNS(ret) Jsonv2Unmarshalers *
jsonv2_unmarshal_from_func(Alloc *a, const Type *t, Jsonv2UnmarshalFromFn fn,
                           void *ctx);

/* json.JoinMarshalers. The functions of each set in ms, a Slice of
 * Jsonv2Marshalers *, in order, so an earlier one gets the first say. NULL
 * entries are skipped, and NULL comes back when there are no functions. */
BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_join_marshalers(Alloc *a, Slice ms);
BURROW_OWNS(ret) Jsonv2Marshalers *jsonv2_join_marshalers_v(Alloc *a, int n, ...);

/* json.JoinUnmarshalers, the same for the unmarshal side. */
BURROW_OWNS(ret) Jsonv2Unmarshalers *jsonv2_join_unmarshalers(Alloc *a, Slice us);
BURROW_OWNS(ret) Jsonv2Unmarshalers *jsonv2_join_unmarshalers_v(Alloc *a, int n, ...);

/* json.WithMarshalers and json.WithUnmarshalers. The option holds the
 * pointer, not a copy, and each is ignored by the other side. */
JsontextOptions jsonv2_with_marshalers(const Jsonv2Marshalers *m);
JsontextOptions jsonv2_with_unmarshalers(const Jsonv2Unmarshalers *u);

/* json.GetOption for the two above. Whether opts says anything about them,
 * and in *value what it says. value may be NULL. */
bool jsonv2_get_marshalers(JsontextOptions opts, const Jsonv2Marshalers **value);
bool jsonv2_get_unmarshalers(JsontextOptions opts, const Jsonv2Unmarshalers **value);

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
