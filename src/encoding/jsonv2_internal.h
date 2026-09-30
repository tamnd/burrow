/* What the encoding/json/v2 files share: the error constructors from Go's
 * errors.go and the entry points into the arshalers.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_JSONV2_INTERNAL_H
#define BURROW_SRC_ENCODING_JSONV2_INTERNAL_H

#include "burrow/encoding/json.h"
#include "burrow/encoding/json/v2.h"

#include "json_internal.h"

#define JV_LIT(s) ((Str){(const Byte *)("" s), (Int)(sizeof(s) - 1)})

/* Go's internal.ErrCycle, ErrNonNilReference and ErrNilInterface, and the
 * unexported errors of arshal.go and arshal_default.go. */
extern const Error burrow__jsonv2_err_cycle;
extern const Error burrow__jsonv2_err_non_nil_reference;
extern const Error burrow__jsonv2_err_nil_interface;
extern const Error burrow__jsonv2_err_ambiguous_name;
extern const Error burrow__jsonv2_err_invalid_string_tag;
extern const Error burrow__jsonv2_err_nil_field;
extern const Error burrow__jsonv2_err_raw_embed_not_object;
extern const Error burrow__jsonv2_err_array_underflow;
extern const Error burrow__jsonv2_err_array_overflow;
extern const Error burrow__jsonv2_err_no_exported_fields;
extern const Error burrow__jsonv2_err_changing_duplicate_names;
extern const Error burrow__jsonv2_err_changing_invalid_utf8;
extern const Error burrow__jsonv2_err_changing_whitespace;

/* errNonSingularValue and errUnsupportedMutation from arshal_funcs.go, and
 * errNonStringValue from arshal_methods.go. */
extern const Error burrow__jsonv2_err_non_singular_value;
extern const Error burrow__jsonv2_err_unsupported_mutation;
extern const Error burrow__jsonv2_err_non_string_value;

/* A SemanticError with its message built, in the error arena. The pointer
 * and the value are copied, so they may point anywhere. When the arena
 * refuses, err alone, and burrow_err_out_of_memory without one. */
Error burrow__jsonv2_semantic_new(const Jsonv2SemanticError *e);

/* The SemanticError inside err, when err is one, or NULL. */
const Jsonv2SemanticError *burrow__jsonv2_as_semantic(Error err);

/* errors.New in the error arena, for messages built at run time. */
Error burrow__jsonv2_errorf(Str msg);

/* toUnexpectedEOF. */
Error burrow__jsonv2_to_unexpected_eof(Error err);

/* isFatalError. */
bool burrow__jsonv2_is_fatal(Error err, const JsontextOptions *o);

/* The constructors of errors.go. */
Error burrow__jsonv2_marshal_error_before(JsontextEncoder *e, const Type *t, Error err);
Error burrow__jsonv2_unmarshal_error_before(JsontextDecoder *d, const Type *t,
                                            Error err);
Error burrow__jsonv2_unmarshal_error_before_skipping(JsontextDecoder *d, const Type *t,
                                                     Error err);
Error burrow__jsonv2_unmarshal_error_after(JsontextDecoder *d, const Type *t,
                                           Error err);
Error burrow__jsonv2_unmarshal_error_after_value(JsontextDecoder *d, const Type *t,
                                                 Error err);
Error burrow__jsonv2_unmarshal_error_after_skipping(JsontextDecoder *d, const Type *t,
                                                    Error err);
Error burrow__jsonv2_invalid_format_enc(JsontextEncoder *e, const Type *t,
                                        const JsontextOptions *o);
Error burrow__jsonv2_invalid_format_dec(JsontextDecoder *d, const Type *t,
                                        const JsontextOptions *o);
Error burrow__jsonv2_duplicate_name_error(JsontextDecoder *d, int64_t offset);
/* The same for a name the encoder was about to write, given quoted. */
Error burrow__jsonv2_duplicate_name_error_enc(JsontextEncoder *e, Slice quoted);

/* isSemanticError and isSyntacticError. */
bool burrow__jsonv2_is_semantic(Error err);
bool burrow__jsonv2_is_syntactic(Error err);

/* wrapErrUnsupported: what, followed by " may not return
 * errors.ErrUnsupported", when err is or wraps errors_err_unsupported. */
Error burrow__jsonv2_wrap_unsupported(Error err, const char *what);

/* newSemanticErrorWithPosition, for an error a method or a function returned
 * after the coder had prev_depth and prev_len at the start of the call. */
Error burrow__jsonv2_error_with_position_enc(JsontextEncoder *e, const Type *t,
                                             Int prev_depth, int64_t prev_len,
                                             Error err);
Error burrow__jsonv2_error_with_position_dec(JsontextDecoder *d, const Type *t,
                                             Int prev_depth, int64_t prev_len,
                                             Error err);

/* collapseSemanticErrors. */
Error burrow__jsonv2_collapse_semantic(Error err);

/* The arshaler for t on the value at p, which is lookupArshaler(t) and a
 * call, with the options in o and changed by the callee for the length of
 * the call the way Go's do. */
/* The methods json looks for, in the order they are tried. */
enum {
    JV_M_MARSHAL_JSON_TO,
    JV_M_MARSHAL_JSON,
    JV_M_APPEND_TEXT,
    JV_M_MARSHAL_TEXT,
    JV_M_UNMARSHAL_JSON_FROM,
    JV_M_UNMARSHAL_JSON,
    JV_M_UNMARSHAL_TEXT,
    JV_M_IS_ZERO,
    JV_M_COUNT
};

#define JV_MASK_MARSHALERS                                                             \
    ((1U << JV_M_MARSHAL_JSON_TO) | (1U << JV_M_MARSHAL_JSON) |                        \
     (1U << JV_M_APPEND_TEXT) | (1U << JV_M_MARSHAL_TEXT))
#define JV_MASK_UNMARSHALERS                                                           \
    ((1U << JV_M_UNMARSHAL_JSON_FROM) | (1U << JV_M_UNMARSHAL_JSON) |                  \
     (1U << JV_M_UNMARSHAL_TEXT))
#define JV_MASK_ARSHALERS (JV_MASK_MARSHALERS | JV_MASK_UNMARSHALERS)

/* The methods of one type that have the right shape, NULL where it has none. */
typedef struct JvMethods {
    const Method *m[JV_M_COUNT];
} JvMethods;

void burrow__jsonv2_methods(const Type *t, JvMethods *out);
bool burrow__jsonv2_implements(const Type *t, unsigned mask);
Error burrow__jsonv2_marshal_methods(JsontextEncoder *e, const Type *t, void *p,
                                     JsontextOptions *mo, const JvMethods *ms);
Error burrow__jsonv2_unmarshal_methods(JsontextDecoder *d, const Type *t, void *p,
                                       JsontextOptions *uo, const JvMethods *ms);
bool burrow__jsonv2_call_is_zero(const Method *m, void *p);

/* The four ways of handing a value to code outside json, shared by the
 * methods and by WithMarshalers. t is the type errors name, and on the
 * marshal side vt is the type of the value itself and src the name v1 gives
 * the call in a MarshalerError. */
typedef Slice (*JvMarshalCall)(const void *ctx, Alloc *a, Error *err);
typedef Error (*JvToCall)(const void *ctx, JsontextEncoder *e);
typedef Error (*JvUnmarshalCall)(const void *ctx, Alloc *a, Slice data);
typedef Error (*JvFromCall)(const void *ctx, Alloc *a, JsontextDecoder *d);
Error burrow__jsonv2_call_marshal(JsontextEncoder *e, const Type *t, const Type *vt,
                                  const JsontextOptions *mo, const char *what,
                                  const char *src, JvMarshalCall call, const void *ctx);
Error burrow__jsonv2_call_to(JsontextEncoder *e, const Type *t, const Type *vt,
                             const char *src, JvToCall call, const void *ctx,
                             bool *skip);
Error burrow__jsonv2_call_unmarshal(JsontextDecoder *d, const Type *t,
                                    const JsontextOptions *uo, const char *what,
                                    JvUnmarshalCall call, const void *ctx);
Error burrow__jsonv2_call_from(JsontextDecoder *d, const Type *t,
                               const JsontextOptions *uo, JvFromCall call,
                               const void *ctx, bool *skip);

/* WithMarshalers and WithUnmarshalers. The calls set *done when a function
 * took the value, and leave it false for the methods and the defaults to
 * have their turn. has_func is lookup's ok, and from_any says whether a
 * function could want what the any fast paths would skip over. */
Error burrow__jsonv2_marshal_funcs(JsontextEncoder *e, const Type *t, void *p,
                                   JsontextOptions *mo, bool *done);
Error burrow__jsonv2_unmarshal_funcs(JsontextDecoder *d, const Type *t, void *p,
                                     JsontextOptions *uo, bool *done);
bool burrow__jsonv2_has_func(const void *fs, const Type *t);
bool burrow__jsonv2_from_any(const void *fs);
Error burrow__jsonv2_marshal_default(JsontextEncoder *e, const Type *t, void *p,
                                     JsontextOptions *mo);
Error burrow__jsonv2_unmarshal_default(JsontextDecoder *d, const Type *t, void *p,
                                       JsontextOptions *uo);

Error burrow__jsonv2_marshal_value(JsontextEncoder *e, const Type *t, void *p,
                                   JsontextOptions *mo);
Error burrow__jsonv2_unmarshal_value(JsontextDecoder *d, const Type *t, void *p,
                                     JsontextOptions *uo);

/* Go's structField and structFields. index is the path of field numbers
 * from the outer struct in, through embedded structs. */
#define JV_CASE_IGNORE 1
#define JV_CASE_STRICT 2

typedef struct JvField {
    Int id;
    const Type *typ;
    const Int *index;
    Int nindex;
    Str name;
    Str quoted_name;
    Str folded;
    bool has_name;
    bool name_need_escape;
    int8_t casing;
    bool embed;
    bool omitzero;
    bool omitempty;
    bool string_;
    bool can_empty;
    Str format;
} JvField;

typedef struct JvFields {
    JvField *flat;
    Int nflat;
    Int *by_name; /* flat index + 1, 0 for an empty slot */
    Int by_name_cap;
    Int *by_folded; /* flat indexes sorted by folded name, then id */
    bool has_fallback;
    JvField fallback; /* the dominant embedded map, when has_fallback */
    bool has_err_init;
    const Type *err_init_type;
    Error err_init;
    bool has_err_fmt;
    const Type *err_fmt_type;
    Error err_fmt;
    void *arena; /* the Arena all of the above lives in */
} JvFields;

/* The fields of struct type t, built once and kept. NULL when out of memory. */
const JvFields *burrow__jsonv2_fields(const Type *t);

/* byActualName. */
const JvField *burrow__jsonv2_fields_by_name(const JvFields *fs, Str name);

/* byFoldedName: how many fields fold to what name folds to, which are
 * fs->flat[fs->by_folded[*start + i]]. */
Int burrow__jsonv2_fields_by_folded(const JvFields *fs, Str name, Int *start);

/* matchFoldedName. */
bool burrow__jsonv2_match_folded(const JvField *f, Str name, const JsontextOptions *o);

/* foldName, appended to out. */
void burrow__jsonv2_append_folded(JsonBuf *out, Str in);

/* reflect's Anonymous for a field. */
bool burrow__jsonv2_field_anonymous(const Field *f);

/* Whether t has a method json v2 would call. */
bool burrow__jsonv2_implements_any(const Type *t);

/* A small fmt.Errorf into a: %s a Str, %q a Str quoted, %r a Rune quoted, %d
 * an int64_t, %T a const Type * and %e an Error's text. */
Error burrow__jsonv2_errorf_in(Alloc *a, const char *fmt, ...);

/* What Go's v1 package hands v2 through encoding/json/internal, which lives
 * in json.c: the error transforms Marshal and Unmarshal apply under
 * ReportErrorsWithLegacySemantics, with root being what Unmarshal was given,
 * and the MarshalerError constructor, with t the type of the value the method
 * was called on and src the name of what failed. */
Error burrow__json_transform_marshal_error(Error err);
Error burrow__json_transform_unmarshal_error(Any root, Error err);
Error burrow__json_new_marshaler_error(const Type *t, Error err, const char *src);

/* Go's type string for t, as reflect.Type.String gives it, into b. */
void burrow__jsonv2_put_type(JsonBuf *b, const Type *t);

#endif /* BURROW_SRC_ENCODING_JSONV2_INTERNAL_H */
