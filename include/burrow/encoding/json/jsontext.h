/* encoding/json/jsontext: JSON syntax without any Go values attached.
 *
 * This is the lower half of Go's json v2. An encoder writes JSON one token or
 * one whole value at a time and a decoder reads it back the same way, and both
 * check the grammar as they go, so a stream that makes it through either is
 * valid JSON. Neither knows about structs or maps. That is json v2's job, and
 * it is built on this.
 *
 * Reading every token in a document:
 *
 *     JsontextDecoder *d = jsontext_new_decoder_v(a, r, 0);
 *     for (;;) {
 *         Error err = BURROW_NO_ERROR;
 *         JsontextToken tok = jsontext_decoder_read_token(d, &err);
 *         if (errors_is(err, io_eof))
 *             break;
 *         if (!BURROW_FAILED(err) && jsontext_token_kind(tok) == JSONTEXT_KIND_STRING)
 *             ... jsontext_token_string(tok, a) ...;
 *     }
 *     jsontext_decoder_free(d);
 *
 * and writing some:
 *
 *     JsontextEncoder *e = jsontext_new_encoder_v(a, w, 1, jsontext_multiline(true));
 *     jsontext_encoder_write_token(e, jsontext_begin_object);
 *     jsontext_encoder_write_token(e, jsontext_string(BURROW_S("name")));
 *     jsontext_encoder_write_token(e, jsontext_string(BURROW_S("gopher")));
 *     jsontext_encoder_write_token(e, jsontext_end_object);
 *     jsontext_encoder_free(e);
 *
 * The encoder writes to w as each top-level value completes, with no flush.
 *
 * Each option setter returns a JsontextOptions, passed as a Slice or, in the
 * _v forms, as arguments. Later options win. Tokens and values from a decoder
 * point into its buffer until the next read; the _clone functions copy them.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/json/jsontext */

#ifndef BURROW_ENCODING_JSON_JSONTEXT_H
#define BURROW_ENCODING_JSON_JSONTEXT_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ options */

/* jsontext.Options, which is also json v2's Options. Every setter returns one
 * of these with just its own option present, and joining them in order is
 * how a list of options becomes one set. presence says which options were
 * given and values what they were set to. The rest are the options that are
 * not true or false, kept as they were given, so an indent string has to
 * outlive whatever uses the options. Build these with the setters rather than
 * by hand. */
typedef struct JsontextOptions {
    uint64_t presence;
    uint64_t values;
    Str indent;
    Str indent_prefix;
    int64_t byte_limit;
    Int depth_limit;
    const void *marshalers;
    const void *unmarshalers;
    Str format;
} JsontextOptions;

extern const Type *const TYPE_JSONTEXT_OPTIONS;

/* jsontext.AllowDuplicateNames. Lets an object have the same name twice.
 * Without it the encoder and decoder both report ErrDuplicateName. */
JsontextOptions jsontext_allow_duplicate_names(bool v);

/* jsontext.AllowInvalidUTF8. Lets strings hold invalid UTF-8, which the
 * encoder writes as U+FFFD. Without it they are an error. */
JsontextOptions jsontext_allow_invalid_utf8(bool v);

/* jsontext.EscapeForHTML. Writes <, > and & in strings as \u003c, \u003e and
 * \u0026, so the output can go inside an HTML <script> tag. */
JsontextOptions jsontext_escape_for_html(bool v);

/* jsontext.EscapeForJS. Writes U+2028 and U+2029 as \u2028 and \u2029, which
 * older JavaScript cannot have raw in a string literal. */
JsontextOptions jsontext_escape_for_js(bool v);

/* jsontext.PreserveRawStrings. Copies strings in values the encoder is given
 * as they are instead of rewriting their escapes the canonical way. */
JsontextOptions jsontext_preserve_raw_strings(bool v);

/* jsontext.CanonicalizeRawInts. Rewrites integers of 16 digits or more in
 * values the encoder is given the way a float64 prints, as RFC 8785 asks. */
JsontextOptions jsontext_canonicalize_raw_ints(bool v);

/* jsontext.CanonicalizeRawFloats. Rewrites numbers with a fraction or an
 * exponent in values the encoder is given the way a float64 prints. */
JsontextOptions jsontext_canonicalize_raw_floats(bool v);

/* jsontext.ReorderRawObjects. Sorts the members of objects in values the
 * encoder is given by name, as RFC 8785 asks. */
JsontextOptions jsontext_reorder_raw_objects(bool v);

/* jsontext.SpaceAfterColon. A space after each colon. */
JsontextOptions jsontext_space_after_colon(bool v);

/* jsontext.SpaceAfterComma. A space after each comma, when not multiline. */
JsontextOptions jsontext_space_after_comma(bool v);

/* jsontext.Multiline. One member or element per line, indented. With nothing
 * else said the indent is a tab and there is a space after each colon. */
JsontextOptions jsontext_multiline(bool v);

/* jsontext.WithIndent. The indent for each level, which also turns on
 * multiline. Anything other than spaces and tabs panics, as in Go, with
 * "json: invalid character 'x' in indent". */
JsontextOptions jsontext_with_indent(Str indent);

/* jsontext.WithIndentPrefix. What each line after the first starts with,
 * which also turns on multiline. The same characters are allowed as for
 * jsontext_with_indent. */
JsontextOptions jsontext_with_indent_prefix(Str prefix);

/* ------------------------------------------------------------------- errors */

/* jsontext.ErrDuplicateName, "duplicate object member name". */
extern const Error jsontext_err_duplicate_name;

/* jsontext.ErrNonStringName, "object member name must be a string". */
extern const Error jsontext_err_non_string_name;

/* jsontext.Pointer, a JSON Pointer as RFC 6901 has them, such as
 * "/fruits/0/name". A Str, so the functions below that return one either
 * point into the one you gave them or say they allocate. */
typedef Str JsontextPointer;

/* jsontext.SyntacticError, which is what every grammar problem comes back as.
 * byte_offset is where in the input or output the problem is, json_pointer
 * says which value, and err is the problem itself, one of the errors above or
 * something that says what character was wrong. errors_is sees through to err
 * and errors_as with TYPE_JSONTEXT_SYNTACTIC_ERROR gets you the struct. The
 * ones the coders make live in the calling goroutine's error arena. */
typedef struct JsontextSyntacticError {
    int64_t byte_offset;
    JsontextPointer json_pointer;
    Error err;
} JsontextSyntacticError;

extern const Type *const TYPE_JSONTEXT_SYNTACTIC_ERROR;

/* Go's Error method, built in a, such as
 *
 *     jsontext: invalid character ',' at start of value within "/0" after offset 1 */
BURROW_OWNS(ret) Str jsontext_syntactic_error_error(const JsontextSyntacticError *e,
                                                    Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error
jsontext_syntactic_error_unwrap(const JsontextSyntacticError *e);

/* An Error for a JsontextSyntacticError you filled in yourself. The message
 * and a copy of the pointer are built in a, and err is kept as it is. Out of
 * memory gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error
jsontext_syntactic_error_as_error(const JsontextSyntacticError *e, Alloc *a);

/* -------------------------------------------------------------------- kinds */

/* jsontext.Kind, the first byte of a token with every number folded into '0'. */
typedef uint8_t JsontextKind;

#define JSONTEXT_KIND_INVALID ((JsontextKind)0)
#define JSONTEXT_KIND_NULL ((JsontextKind)'n')
#define JSONTEXT_KIND_FALSE ((JsontextKind)'f')
#define JSONTEXT_KIND_TRUE ((JsontextKind)'t')
#define JSONTEXT_KIND_STRING ((JsontextKind)'"')
#define JSONTEXT_KIND_NUMBER ((JsontextKind)'0')
#define JSONTEXT_KIND_BEGIN_OBJECT ((JsontextKind)'{')
#define JSONTEXT_KIND_END_OBJECT ((JsontextKind)'}')
#define JSONTEXT_KIND_BEGIN_ARRAY ((JsontextKind)'[')
#define JSONTEXT_KIND_END_ARRAY ((JsontextKind)']')

/* Kind.String: "null", "string", "{" and so on, "invalid" for zero and
 * "<invalid jsontext.Kind: 'x'>" for anything else. Static text. */
BURROW_STATIC(ret) Str jsontext_kind_string(JsontextKind k);

/* ------------------------------------------------------------------- tokens */

/* The decoder's buffer, which a token read from it points at. Not API. */
typedef struct burrow__JsontextBuffer {
    Byte *buf;
    Int len;
    Int cap;
    Int prev_start;
    Int prev_end;
    int64_t base_offset;
} burrow__JsontextBuffer;

/* jsontext.Token: a null, a boolean, a string, a number, or one of the four
 * brackets. A token read from a decoder is only good until that decoder reads
 * again, and using it after that panics the way Go does. The fields are the
 * token's own; use the functions. */
typedef struct JsontextToken {
    const burrow__JsontextBuffer *raw;
    Str str;
    uint64_t num;
} JsontextToken;

extern const Type *const TYPE_JSONTEXT_TOKEN;

extern const JsontextToken jsontext_null;
extern const JsontextToken jsontext_false;
extern const JsontextToken jsontext_true;
extern const JsontextToken jsontext_begin_object;
extern const JsontextToken jsontext_end_object;
extern const JsontextToken jsontext_begin_array;
extern const JsontextToken jsontext_end_array;

/* jsontext.Bool, String, Float, Float32, Int and Uint. A string token keeps s
 * as it is, so s has to live as long as the token. NaN and the infinities
 * become the strings "NaN", "Infinity" and "-Infinity". */
JsontextToken jsontext_bool(bool b);
JsontextToken jsontext_string(Str s);
JsontextToken jsontext_float(double n);
JsontextToken jsontext_float32(float n);
JsontextToken jsontext_int(int64_t n);
JsontextToken jsontext_uint(uint64_t n);

/* Token.Clone. A copy of a token read from a decoder that no longer depends on
 * the decoder, built in a as one block. Tokens that are not from a decoder
 * come back as they are. Out of memory gives the zero token. */
BURROW_OWNS(ret) JsontextToken jsontext_token_clone(JsontextToken t, Alloc *a);

/* Gives back what jsontext_token_clone allocated. Other tokens are left
 * alone. */
void jsontext_token_free(JsontextToken t, Alloc *a);

/* Token.Kind. */
JsontextKind jsontext_token_kind(JsontextToken t);

/* Token.Bool. Panics unless t is true or false. */
bool jsontext_token_bool(JsontextToken t);

/* Token.String. For a string the unquoted text, for anything else its JSON
 * text, built in a. The zero token is "<invalid jsontext.Token>". */
BURROW_OWNS(ret) Str jsontext_token_string(JsontextToken t, Alloc *a);

/* Token.Float, Float32, Int and Uint. A number that does not fit gives the
 * nearest value that does and an error that errors_is matches with
 * strconv_err_range, and one with a fraction asked for as an integer gives
 * strconv_err_syntax. The strings "NaN", "Infinity" and "-Infinity" work for
 * the float forms. Anything else that is not a number panics. */
double jsontext_token_float(JsontextToken t, Error *err);
float jsontext_token_float32(JsontextToken t, Error *err);
int64_t jsontext_token_int(JsontextToken t, Error *err);
uint64_t jsontext_token_uint(JsontextToken t, Error *err);

/* ------------------------------------------------------------------- values */

/* jsontext.Value, the raw text of one JSON value, a []byte. */
typedef Slice JsontextValue;

/* Value.Clone, a copy in a. */
BURROW_OWNS(ret) JsontextValue jsontext_value_clone(JsontextValue v, Alloc *a);

/* Value.String. The text as a Str over the same bytes, or "null" for a nil
 * value. */
BURROW_BORROWS(ret, v) Str jsontext_value_string(JsontextValue v);

/* Value.IsValid. Whether v is exactly one valid JSON value, with white space
 * around it allowed. Options such as jsontext_allow_duplicate_names relax
 * what valid means. */
bool jsontext_value_is_valid(JsontextValue v, Slice opts);
bool jsontext_value_is_valid_v(JsontextValue v, int n, ...);

/* Value.Format, Compact, Indent and Canonicalize. Each rewrites *v in place
 * when it has room and in a new array from a when it does not, the way Go's
 * append does, and leaves *v alone on an error. That means *v has to be
 * writable, so clone a BURROW_B literal before handing it over.
 *
 * Format uses only the options you give it. Compact drops the white space
 * and Indent puts it in as jsontext_multiline would, and both keep strings
 * and duplicate names as they are unless the options say otherwise.
 * Canonicalize rewrites v in the RFC 8785 form, which sorts object members
 * and prints numbers as float64s. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_format(JsontextValue *v,
                                                              Alloc *a, Slice opts);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_format_v(JsontextValue *v,
                                                                Alloc *a, int n, ...);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_compact(JsontextValue *v,
                                                               Alloc *a, Slice opts);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_compact_v(JsontextValue *v,
                                                                 Alloc *a, int n, ...);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_indent(JsontextValue *v,
                                                              Alloc *a, Slice opts);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_indent_v(JsontextValue *v,
                                                                Alloc *a, int n, ...);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_canonicalize(JsontextValue *v,
                                                                    Alloc *a,
                                                                    Slice opts);
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_canonicalize_v(JsontextValue *v,
                                                                      Alloc *a, int n,
                                                                      ...);

/* Value.MarshalJSON. v itself, or "null" for a nil value. Never fails. */
BURROW_BORROWS(ret, v) Slice jsontext_value_marshal_json(JsontextValue v, Error *err);

/* Value.UnmarshalJSON. Copies b into *v, reusing its array when it has room.
 * A NULL v is the error Go gives for a nil pointer. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error jsontext_value_unmarshal_json(JsontextValue *v,
                                                                      Alloc *a,
                                                                      Slice b);

/* Value.Kind, the kind of the first token after any white space. */
JsontextKind jsontext_value_kind(JsontextValue v);

/* --------------------------------------------------------------- appending */

/* jsontext.AppendFloat. src as the shortest JSON number that reads back as
 * the same float64 or, with bits 32, float32. Any other bits panics. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice jsontext_append_float(Alloc *a,
                                                                      Slice dst,
                                                                      double src,
                                                                      Int bits);

/* jsontext.AppendQuote. src as a JSON string. Invalid UTF-8 is written as
 * U+FFFD and reported as a SyntacticError, with the string still appended. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice jsontext_append_quote(Alloc *a,
                                                                      Slice dst,
                                                                      Str src,
                                                                      Error *err);

/* jsontext.AppendUnquote. The text of the JSON string src. Anything after the
 * closing quote is an error, as is anything wrong inside the string, and dst
 * then has what came before the problem. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice jsontext_append_unquote(Alloc *a,
                                                                        Slice dst,
                                                                        Str src,
                                                                        Error *err);

/* jsontext.AppendFormat. src formatted with the options, the way
 * jsontext_value_format would, appended to dst. On an error src itself is
 * appended. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice
jsontext_append_format(Alloc *a, Slice dst, Str src, Slice opts, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice
jsontext_append_format_v(Alloc *a, Slice dst, Str src, Error *err, int n, ...);

/* ----------------------------------------------------------------- pointers */

/* Pointer.IsValid. Whether p is empty or starts with '/', has only ~0 and ~1
 * as escapes, and is valid UTF-8. */
bool jsontext_pointer_is_valid(JsontextPointer p);

/* Pointer.Contains. Whether pc is p or something inside it. */
bool jsontext_pointer_contains(JsontextPointer p, JsontextPointer pc);

/* Pointer.Parent. p without its last token, pointing into p. */
BURROW_BORROWS(ret, p) JsontextPointer jsontext_pointer_parent(JsontextPointer p);

/* Pointer.LastToken. The last token with its escapes undone, in a. */
BURROW_OWNS(ret) Str jsontext_pointer_last_token(JsontextPointer p, Alloc *a);

/* Pointer.AppendToken. p with tok added as a new last token, escaped, in a. */
BURROW_OWNS(ret) JsontextPointer jsontext_pointer_append_token(JsontextPointer p,
                                                               Alloc *a, Str tok);

/* Pointer.Tokens. A sequence of the tokens with their escapes undone, each
 * yielded as a pointer to a Str that is good until the yield returns. The
 * sequence holds a copy of nothing, so p has to outlive it. Give it back with
 * jsontext_pointer_tokens_free, or use an arena. */
BURROW_OWNS(ret) IterSeq jsontext_pointer_tokens(JsontextPointer p, Alloc *a);
void jsontext_pointer_tokens_free(IterSeq seq, Alloc *a);

/* ----------------------------------------------------------------- Encoder */

/* jsontext.Encoder. */
typedef struct JsontextEncoder JsontextEncoder;

extern const Type *const TYPE_JSONTEXT_ENCODER;

/* jsontext.NewEncoder. An encoder writing to w, with its buffer from a. NULL
 * when the allocator refuses. */
BURROW_OWNS(ret) JsontextEncoder *jsontext_new_encoder(Alloc *a, IoWriter w,
                                                       Slice opts);
BURROW_OWNS(ret) JsontextEncoder *jsontext_new_encoder_v(Alloc *a, IoWriter w, int n,
                                                         ...);

/* Gives the encoder back. Anything not yet written stays unwritten. NULL is
 * fine. */
void jsontext_encoder_free(JsontextEncoder *e);

/* Encoder.Reset. Starts over writing to w with the options given, keeping the
 * buffer. */
void jsontext_encoder_reset(JsontextEncoder *e, IoWriter w, Slice opts);
void jsontext_encoder_reset_v(JsontextEncoder *e, IoWriter w, int n, ...);

/* Encoder.Options. The options the encoder is using. */
JsontextOptions jsontext_encoder_options(const JsontextEncoder *e);

/* Encoder.WriteToken. Writes one token, with the comma or colon that goes in
 * front of it. A token that does not fit where it is, such as a number for an
 * object name, is a SyntacticError and nothing is written. */
BURROW_STATIC(ret) Error jsontext_encoder_write_token(JsontextEncoder *e,
                                                      JsontextToken t);

/* Encoder.WriteValue. Writes one whole value, reformatted to the encoder's
 * options and checked the way a decoder would check it. */
BURROW_STATIC(ret) Error jsontext_encoder_write_value(JsontextEncoder *e,
                                                      JsontextValue v);

/* Encoder.OutputOffset. How many bytes have been written, counting what is
 * still in the buffer. */
int64_t jsontext_encoder_output_offset(const JsontextEncoder *e);

/* Encoder.AvailableBuffer. An empty []byte with some room, owned by the
 * encoder, for building a value to pass to jsontext_encoder_write_value
 * without allocating. Good until the next call on the encoder. */
BURROW_BORROWS(ret, e) Slice jsontext_encoder_available_buffer(JsontextEncoder *e);

/* Encoder.StackDepth. How many objects and arrays are open. */
Int jsontext_encoder_stack_depth(const JsontextEncoder *e);

/* Encoder.StackIndex. For i from 1 to the depth, JSONTEXT_KIND_BEGIN_OBJECT
 * or JSONTEXT_KIND_BEGIN_ARRAY for the open value at that level and in
 * *length how many names and values or elements it has so far. For 0 the
 * kind is JSONTEXT_KIND_INVALID and the length counts top-level values.
 * length may be NULL. */
JsontextKind jsontext_encoder_stack_index(const JsontextEncoder *e, Int i,
                                          int64_t *length);

/* Encoder.StackPointer. Where the encoder is, as a JSON Pointer, in a. */
BURROW_OWNS(ret) JsontextPointer jsontext_encoder_stack_pointer(JsontextEncoder *e,
                                                                Alloc *a);

/* ----------------------------------------------------------------- Decoder */

/* jsontext.Decoder. */
typedef struct JsontextDecoder JsontextDecoder;

extern const Type *const TYPE_JSONTEXT_DECODER;

/* jsontext.NewDecoder. A decoder reading from r, with its buffer from a.
 * NULL when the allocator refuses. */
BURROW_OWNS(ret) JsontextDecoder *jsontext_new_decoder(Alloc *a, IoReader r,
                                                       Slice opts);
BURROW_OWNS(ret) JsontextDecoder *jsontext_new_decoder_v(Alloc *a, IoReader r, int n,
                                                         ...);

/* Gives the decoder back. Tokens and values read from it go with it. NULL is
 * fine. */
void jsontext_decoder_free(JsontextDecoder *d);

/* Decoder.Reset. Starts over reading from r with the options given. */
void jsontext_decoder_reset(JsontextDecoder *d, IoReader r, Slice opts);
void jsontext_decoder_reset_v(JsontextDecoder *d, IoReader r, int n, ...);

/* Decoder.Options. The options the decoder is using. */
JsontextOptions jsontext_decoder_options(const JsontextDecoder *d);

/* Decoder.PeekKind. The kind of the next token without reading it, or
 * JSONTEXT_KIND_INVALID when there is an error, which the next read then
 * returns. */
JsontextKind jsontext_decoder_peek_kind(JsontextDecoder *d);

/* Decoder.SkipValue. Reads past the next value, all of it if it is an object
 * or array. */
BURROW_STATIC(ret) Error jsontext_decoder_skip_value(JsontextDecoder *d);

/* Decoder.ReadToken. The next token. At the end of the input err gets io_eof,
 * and a token that does not fit the grammar is a SyntacticError. */
JsontextToken jsontext_decoder_read_token(JsontextDecoder *d, Error *err);

/* Decoder.ReadValue. The next whole value, pointing into the decoder's buffer
 * and good until the next read. */
BURROW_BORROWS(ret, d) JsontextValue jsontext_decoder_read_value(JsontextDecoder *d,
                                                                 Error *err);

/* Decoder.InputOffset. How many bytes of the input have been read, up to the
 * end of the last token or value. */
int64_t jsontext_decoder_input_offset(const JsontextDecoder *d);

/* Decoder.UnreadBuffer. The part of the buffer read from the input and not
 * yet decoded, pointing into the decoder. */
BURROW_BORROWS(ret, d) Slice jsontext_decoder_unread_buffer(const JsontextDecoder *d);

/* Decoder.StackDepth, StackIndex and StackPointer, as for the encoder. */
Int jsontext_decoder_stack_depth(const JsontextDecoder *d);
JsontextKind jsontext_decoder_stack_index(const JsontextDecoder *d, Int i,
                                          int64_t *length);
BURROW_OWNS(ret) JsontextPointer jsontext_decoder_stack_pointer(JsontextDecoder *d,
                                                                Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_JSON_JSONTEXT_H */
