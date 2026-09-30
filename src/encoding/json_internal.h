/* What the encoding/json files share with each other and with their tests:
 * the option flags, the wire level helpers from Go's internal/jsonwire, and
 * the encoder and decoder state that json v2 drives directly the way Go's
 * json package reaches into jsontext through its export hook.
 *
 * Derived from Go's src/encoding/json/internal/jsonflags/flags.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_JSON_INTERNAL_H
#define BURROW_SRC_ENCODING_JSON_INTERNAL_H

#include "burrow/encoding/json/jsontext.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------- flags
 *
 * jsonflags.Bools. Bit 0 is the value a setter carries and every other bit is
 * one option. A JsontextOptions holds two of these words, which options were
 * given and what they were set to, and joining two of them is jsonflags.Join. */

#define JSONFLAG_ALLOW_DUPLICATE_NAMES ((uint64_t)1 << 1)
#define JSONFLAG_ALLOW_INVALID_UTF8 ((uint64_t)1 << 2)
#define JSONFLAG_WITHIN_ARSHAL_CALL ((uint64_t)1 << 3)
#define JSONFLAG_OMIT_TOP_LEVEL_NEWLINE ((uint64_t)1 << 4)
#define JSONFLAG_PRESERVE_RAW_STRINGS ((uint64_t)1 << 5)
#define JSONFLAG_CANONICALIZE_RAW_INTS ((uint64_t)1 << 6)
#define JSONFLAG_CANONICALIZE_RAW_FLOATS ((uint64_t)1 << 7)
#define JSONFLAG_REORDER_RAW_OBJECTS ((uint64_t)1 << 8)
#define JSONFLAG_ESCAPE_FOR_HTML ((uint64_t)1 << 9)
#define JSONFLAG_ESCAPE_FOR_JS ((uint64_t)1 << 10)
#define JSONFLAG_MULTILINE ((uint64_t)1 << 11)
#define JSONFLAG_SPACE_AFTER_COLON ((uint64_t)1 << 12)
#define JSONFLAG_SPACE_AFTER_COMMA ((uint64_t)1 << 13)
#define JSONFLAG_INDENT ((uint64_t)1 << 14)
#define JSONFLAG_INDENT_PREFIX ((uint64_t)1 << 15)
#define JSONFLAG_BYTE_LIMIT ((uint64_t)1 << 16)
#define JSONFLAG_DEPTH_LIMIT ((uint64_t)1 << 17)

#define JSONFLAG_STRINGIFY_NUMBERS ((uint64_t)1 << 18)
#define JSONFLAG_DETERMINISTIC ((uint64_t)1 << 19)
#define JSONFLAG_FORMAT_NIL_MAP_AS_NULL ((uint64_t)1 << 20)
#define JSONFLAG_FORMAT_NIL_SLICE_AS_NULL ((uint64_t)1 << 21)
#define JSONFLAG_OMIT_ZERO_STRUCT_FIELDS ((uint64_t)1 << 22)
#define JSONFLAG_MATCH_CASE_INSENSITIVE_NAMES ((uint64_t)1 << 23)
#define JSONFLAG_REJECT_UNKNOWN_MEMBERS ((uint64_t)1 << 24)
#define JSONFLAG_MARSHALERS ((uint64_t)1 << 25)
#define JSONFLAG_UNMARSHALERS ((uint64_t)1 << 26)
#define JSONFLAG_STRING_TAG ((uint64_t)1 << 27)
#define JSONFLAG_FORMAT_TAG ((uint64_t)1 << 28)
#define JSONFLAG_FORMAT_TAG_SUPPORTED ((uint64_t)1 << 29)

#define JSONFLAG_CALL_METHODS_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 30)
#define JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY ((uint64_t)1 << 31)
#define JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 32)
#define JSONFLAG_FORMAT_DURATION_AS_NANO ((uint64_t)1 << 33)
#define JSONFLAG_MATCH_CASE_SENSITIVE_DELIMITER ((uint64_t)1 << 34)
#define JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 35)
#define JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 36)
#define JSONFLAG_PARSE_BYTES_WITH_LOOSE_RFC4648 ((uint64_t)1 << 37)
#define JSONFLAG_PARSE_TIME_WITH_LOOSE_RFC3339 ((uint64_t)1 << 38)
#define JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 39)
#define JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS ((uint64_t)1 << 40)
#define JSONFLAG_UNMARSHAL_ANY_WITH_RAW_NUMBER ((uint64_t)1 << 41)
#define JSONFLAG_UNMARSHAL_ARRAY_FROM_ANY_LENGTH ((uint64_t)1 << 42)

#define JSONFLAG_NON_BOOLEAN                                                           \
    (JSONFLAG_INDENT | JSONFLAG_INDENT_PREFIX | JSONFLAG_BYTE_LIMIT |                  \
     JSONFLAG_DEPTH_LIMIT | JSONFLAG_MARSHALERS | JSONFLAG_UNMARSHALERS |              \
     JSONFLAG_FORMAT_TAG)
#define JSONFLAG_ANY_WHITESPACE                                                        \
    (JSONFLAG_MULTILINE | JSONFLAG_SPACE_AFTER_COLON | JSONFLAG_SPACE_AFTER_COMMA)
#define JSONFLAG_ANY_ESCAPE (JSONFLAG_ESCAPE_FOR_HTML | JSONFLAG_ESCAPE_FOR_JS)
#define JSONFLAG_CANONICALIZE_NUMBERS                                                  \
    (JSONFLAG_CANONICALIZE_RAW_INTS | JSONFLAG_CANONICALIZE_RAW_FLOATS)
#define JSONFLAG_TAG_FLAGS (JSONFLAG_STRING_TAG | JSONFLAG_FORMAT_TAG)
#define JSONFLAG_DEFAULT_V1                                                            \
    (JSONFLAG_ALLOW_DUPLICATE_NAMES | JSONFLAG_ALLOW_INVALID_UTF8 |                    \
     JSONFLAG_ESCAPE_FOR_HTML | JSONFLAG_ESCAPE_FOR_JS |                               \
     JSONFLAG_PRESERVE_RAW_STRINGS | JSONFLAG_DETERMINISTIC |                          \
     JSONFLAG_FORMAT_NIL_MAP_AS_NULL | JSONFLAG_FORMAT_NIL_SLICE_AS_NULL |             \
     JSONFLAG_MATCH_CASE_INSENSITIVE_NAMES |                                           \
     JSONFLAG_CALL_METHODS_WITH_LEGACY_SEMANTICS |                                     \
     JSONFLAG_FORMAT_BYTE_ARRAY_AS_ARRAY |                                             \
     JSONFLAG_FORMAT_BYTES_WITH_LEGACY_SEMANTICS | JSONFLAG_FORMAT_DURATION_AS_NANO |  \
     JSONFLAG_MATCH_CASE_SENSITIVE_DELIMITER | JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS |  \
     JSONFLAG_OMIT_EMPTY_WITH_LEGACY_SEMANTICS |                                       \
     JSONFLAG_PARSE_BYTES_WITH_LOOSE_RFC4648 |                                         \
     JSONFLAG_PARSE_TIME_WITH_LOOSE_RFC3339 |                                          \
     JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS |                                    \
     JSONFLAG_STRINGIFY_WITH_LEGACY_SEMANTICS |                                        \
     JSONFLAG_UNMARSHAL_ARRAY_FROM_ANY_LENGTH)

/* isnan and isinf without the <math.h> macros, whose float branch on mingw
 * warns about a double argument under -Wfloat-conversion. */
static inline bool burrow__json_isnan(double f) {
    return f != f;
}

static inline bool burrow__json_isinf(double f) {
    return f > DBL_MAX || f < -DBL_MAX;
}

/* Whether f has no fractional part, which is trunc(f) == f without libm.
 * Every double of magnitude 2^52 or more is a whole number. */
static inline bool burrow__json_is_integral(double f) {
    if (burrow__json_isnan(f))
        return false;
    if (f >= 4503599627370496.0 || f <= -4503599627370496.0)
        return true;
    return (double)(int64_t)f == f;
}

/* Flags.Get, Flags.Has, Flags.Set, Flags.Clear and Flags.Join. */
static inline bool jsonflags_get(const JsontextOptions *o, uint64_t f) {
    return (o->values & f) != 0;
}

static inline bool jsonflags_has(const JsontextOptions *o, uint64_t f) {
    return (o->presence & f) != 0;
}

static inline void jsonflags_set(JsontextOptions *o, uint64_t f) {
    uint64_t id = f & ~(uint64_t)1;
    o->presence |= id;
    o->values &= ~id;
    o->values |= (f & 1) * id;
}

static inline void jsonflags_clear(JsontextOptions *o, uint64_t f) {
    o->presence &= ~f;
    o->values &= ~f;
}

/* jsonopts.Struct.Join for one source, and for a Slice of JsontextOptions. */
void burrow__jsonopts_join(JsontextOptions *dst, const JsontextOptions *src);
void burrow__jsonopts_join_slice(JsontextOptions *dst, Slice opts);

/* jsonopts.Struct.InitializeMultiline. */
void burrow__jsonopts_initialize_multiline(JsontextOptions *o);

/* Whether err is the coders' ioError, which json v2 passes through as it is. */
bool burrow__jsontext_is_io_error(Error err);

/* ------------------------------------------------------------------ buffer
 *
 * A growable []byte. Every append that runs out of memory sets failed and
 * drops what it was given, so a run of appends needs one check at the end.
 * owned says whether p came from a and can be given back when it grows,
 * which is true for a coder's own buffer and false for a dst the caller
 * passed in, since Go's append leaves the old array alone. */
typedef struct JsonBuf {
    Byte *p;
    Int len;
    Int cap;
    Alloc *a;
    bool owned;
    bool failed;
} JsonBuf;

bool burrow__jsonbuf_grow(JsonBuf *b, Int n);

static inline bool jsonbuf_reserve(JsonBuf *b, Int n) {
    if (b->cap - b->len >= n)
        return true;
    return burrow__jsonbuf_grow(b, n);
}

static inline void jsonbuf_put(JsonBuf *b, const void *p, Int n) {
    if (n <= 0 || !jsonbuf_reserve(b, n))
        return;
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static inline void jsonbuf_byte(JsonBuf *b, Byte c) {
    if (!jsonbuf_reserve(b, 1))
        return;
    b->p[b->len++] = c;
}

static inline void jsonbuf_str(JsonBuf *b, Str s) {
    jsonbuf_put(b, s.p, s.len);
}

/* Gives back an owned buffer. */
void burrow__jsonbuf_free(JsonBuf *b);

/* The buffer as a []byte, and a buffer that appends to a caller's []byte. */
static inline Slice jsonbuf_slice(const JsonBuf *b) {
    Slice s = {b->p, b->len, b->cap, TYPE_BYTE};
    return s;
}

static inline JsonBuf jsonbuf_from_slice(Alloc *a, Slice s) {
    JsonBuf b = {(Byte *)s.p, s.len, s.cap, a, false, false};
    return b;
}

/* ---------------------------------------------------------------- jsonwire */

/* ValueFlags. */
#define JSONWIRE_STRING_NON_VERBATIM 1u
#define JSONWIRE_STRING_NON_CANONICAL 2u

/* ConsumeNumberState. */
enum {
    JSONWIRE_NUMBER_INIT,
    JSONWIRE_BEFORE_INTEGER_DIGITS,
    JSONWIRE_WITHIN_INTEGER_DIGITS,
    JSONWIRE_BEFORE_FRACTIONAL_DIGITS,
    JSONWIRE_WITHIN_FRACTIONAL_DIGITS,
    JSONWIRE_BEFORE_EXPONENT_DIGITS,
    JSONWIRE_WITHIN_EXPONENT_DIGITS,
};

/* The escape table for ASCII: control characters, '"', '&', '<', '>', '\\'. */
extern const Byte burrow__jsonwire_escape_ascii[128];

/* jsonwire.ErrInvalidUTF8. */
extern const Error burrow__jsonwire_err_invalid_utf8;

static inline Int jsonwire_consume_whitespace(const Byte *b, Int n) {
    Int i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
        i++;
    return i;
}

static inline Int jsonwire_consume_null(const Byte *b, Int n) {
    return n >= 4 && memcmp(b, "null", 4) == 0 ? 4 : 0;
}

static inline Int jsonwire_consume_false(const Byte *b, Int n) {
    return n >= 5 && memcmp(b, "false", 5) == 0 ? 5 : 0;
}

static inline Int jsonwire_consume_true(const Byte *b, Int n) {
    return n >= 4 && memcmp(b, "true", 4) == 0 ? 4 : 0;
}

static inline Int jsonwire_consume_simple_string(const Byte *b, Int n) {
    Int i = 0;
    if (n > 0 && b[0] == '"') {
        i++;
        while (i < n && b[i] < 0x80 && burrow__jsonwire_escape_ascii[b[i]] == 0)
            i++;
        if (i < n && b[i] == '"')
            return i + 1;
    }
    return 0;
}

static inline Int jsonwire_consume_simple_number(const Byte *b, Int n) {
    Int i = 0;
    if (n > 0) {
        if (b[0] == '0') {
            i++;
        } else if (b[0] >= '1' && b[0] <= '9') {
            i++;
            while (i < n && b[i] >= '0' && b[i] <= '9')
                i++;
        } else {
            return 0;
        }
        if (i >= n || (b[i] != '.' && b[i] != 'e' && b[i] != 'E'))
            return i;
    }
    return 0;
}

Int burrow__jsonwire_consume_literal(const Byte *b, Int n, Str lit, Error *err);
Int burrow__jsonwire_consume_string_resumable(unsigned *flags, const Byte *b, Int n,
                                              Int resume, bool validate_utf8,
                                              Error *err);
Int burrow__jsonwire_consume_number_resumable(const Byte *b, Int n, Int resume,
                                              int *state, Error *err);
Int burrow__jsonwire_consume_number(const Byte *b, Int n, Error *err);

/* AppendUnquote. The error is the first thing wrong with src, the way Go's
 * is, and dst holds what was unquoted before it. */
Error burrow__jsonwire_append_unquote(JsonBuf *dst, const Byte *src, Int n);

/* UnquoteMayCopy. Verbatim strings come back as a view into b and the rest
 * are unquoted into scratch, which the result then points into. */
Str burrow__jsonwire_unquote_may_copy(const Byte *b, Int n, bool is_verbatim,
                                      JsonBuf *scratch);

/* parseHexUint16, which the tests reach for too. */
bool burrow__jsonwire_parse_hex_uint16(const Byte *b, Int n, uint16_t *v);
bool burrow__jsonwire_parse_uint(const Byte *b, Int n, uint64_t *v);
bool burrow__jsonwire_need_escape(const Byte *src, Int n);
Error burrow__jsonwire_append_quote(JsonBuf *dst, const Byte *src, Int n,
                                    const JsontextOptions *flags);
void burrow__jsonwire_append_float(JsonBuf *dst, double src, int bits);
Int burrow__jsonwire_reformat_string(JsonBuf *dst, const Byte *src, Int n,
                                     const JsontextOptions *flags, Error *err);
Int burrow__jsonwire_reformat_number(JsonBuf *dst, const Byte *src, Int n,
                                     const JsontextOptions *flags, Error *err);

Int burrow__jsonwire_trim_suffix_whitespace(const Byte *b, Int n);
Int burrow__jsonwire_trim_suffix_string(const Byte *b, Int n);
static inline Int jsonwire_trim_suffix_byte(const Byte *b, Int n, Byte c) {
    return n > 0 && b[n - 1] == c ? n - 1 : n;
}

/* QuoteRune of the first rune of b, written into out, which needs 16 bytes. */
Str burrow__jsonwire_quote_rune(const Byte *b, Int n, Byte out[16]);

int burrow__jsonwire_compare_utf16(const Byte *x, Int nx, const Byte *y, Int ny);

/* TruncatePointer, appended to dst. */
void burrow__jsonwire_truncate_pointer(JsonBuf *dst, Str s, Int n);

/* jsonwire.InvalidTextError. The error lives in the error arena. The struct is
 * here so json v1 can see how long What was, which is what its offsets count. */
typedef struct JsonwireInvalidTextError {
    Str label;
    Str what;
    Str where;
    Str message;
} JsonwireInvalidTextError;

extern const ErrorVT burrow__jsonwire_invalid_text_error_vt;

Error burrow__jsonwire_new_invalid_character_error(const Byte *prefix, Int n,
                                                   Str where);
Error burrow__jsonwire_new_invalid_escape_sequence_error(const Byte *what, Int n);

static inline bool jsonwire_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ---------------------------------------------------------------- the coders
 *
 * The state both coders keep: the token state machine, the names of the
 * objects being written or read, and the names already seen in each object
 * for the duplicate check. */

typedef struct JsonNamespace {
    Int *end_offsets;
    Int len;
    Int cap;
    Byte *names;
    Int names_len;
    Int names_cap;
    /* Once an object has more than 64 names or 1 KiB of them, an open
     * addressed set of name indexes, 0 for empty and -1 for removed. */
    Int *slots;
    Int slots_cap;
    Int slots_used;
    uint64_t seed;
} JsonNamespace;

typedef struct JsonState {
    Alloc *a;
    /* stateMachine */
    uint64_t *stack;
    Int stack_len;
    Int stack_cap;
    uint64_t last;
    /* objectNameStack */
    Int *offsets;
    Int offsets_len;
    Int offsets_cap;
    JsonBuf unquoted;
    /* objectNamespaceStack */
    JsonNamespace *spaces;
    Int spaces_len;
    Int spaces_cap;
    /* Go's pointerSuffixError, the JSON Pointer of a problem found while
     * reading or reformatting a whole value, built backwards from the inside
     * out and turned around when the error is wrapped. */
    JsonBuf rev;
} JsonState;

/* One entry of json v2's seenPointers: typedPointer. */
typedef struct JsonSeen {
    const Type *t;
    const void *p;
    Int len;
} JsonSeen;

struct JsontextEncoder {
    JsonState st;
    JsontextOptions opts;
    JsonBuf buf;
    int64_t base_offset;
    IoWriter wr;
    bool has_wr;
    Int max_value;
    JsonBuf avail;
    /* The open objects and arrays while a whole value is reformatted, kept
     * here rather than on the C stack so deep nesting cannot overflow it. */
    void *frames;
    Int frames_cap;
    Alloc *a;
    /* json v2's seenPointers, kept as a stack since what is visited last is
     * left first. Each entry is a type, a pointer and a slice length. */
    void *seen;
    Int seen_len;
    Int seen_cap;
};

struct JsontextDecoder {
    JsonState st;
    JsontextOptions opts;
    burrow__JsontextBuffer db;
    Int peek_pos;
    Error peek_err;
    IoReader rd;
    bool has_rd;
    bool owns_buf;
    /* The open objects and arrays while a whole value is read. */
    void *frames;
    Int frames_cap;
    Alloc *a;
    /* Where json v2 puts what it unmarshals: strings, slices, maps and the
     * values pointers and interfaces point at. */
    Alloc *out_alloc;
};

/* stateEntry: the top bit says object or array, the next two say the
 * object's namespace is turned off or broken, and the rest count the names
 * and values so far. */
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

static inline Int jt_need_indent(const JsonState *s, JsontextKind next) {
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

static inline Byte jt_need_delim(const JsonState *s, JsontextKind next) {
    if (jt_e_need_value(s->last))
        return ':';
    if (jt_e_need_comma(s->last, next) && s->stack_len != 0)
        return ',';
    return 0;
}

/* The name of the object member being written starts at pos in the
 * encoder's buffer, still quoted. */
static inline void jt_names_replace_last_quoted(JsonState *s, Int pos) {
    s->offsets[s->offsets_len - 1] = ~pos;
}

/* What encoding/json/v2 reaches into jsontext for, the same things Go's
 * export.go hands it. */
bool burrow__jsontext_unwrite_empty_object_member(JsontextEncoder *e,
                                                  const Str *prev_name);
Str burrow__jsontext_unwrite_only_object_member_name(JsontextEncoder *e, Alloc *a);
Error burrow__jsontext_append_raw(JsontextEncoder *e, JsontextKind k, bool safe_ascii,
                                  Error (*fn)(JsonBuf *b, void *ctx), void *ctx);
void burrow__jsontext_append_indent(JsontextEncoder *e, Int n);
bool burrow__jsontext_need_flush(const JsontextEncoder *e);
Error burrow__jsontext_flush(JsontextEncoder *e);
Error burrow__jsontext_skip_until(JsontextDecoder *d, Int depth, int64_t length);
bool burrow__jsontext_at_eof(JsontextDecoder *d);
Error burrow__jsontext_check_eof(JsontextDecoder *d);
Slice burrow__jsontext_previous_token_or_value(const JsontextDecoder *d);

void burrow__jsontext_encoder_init(JsontextEncoder *e, Alloc *a);
void burrow__jsontext_encoder_setup(JsontextEncoder *e, IoWriter w, bool has_wr,
                                    const JsontextOptions *opts);
void burrow__jsontext_encoder_release(JsontextEncoder *e);
void burrow__jsontext_decoder_init(JsontextDecoder *d, Alloc *a);
void burrow__jsontext_decoder_setup(JsontextDecoder *d, IoReader r, bool has_rd,
                                    const Byte *b, Int len,
                                    const JsontextOptions *opts);
void burrow__jsontext_decoder_release(JsontextDecoder *d);
Slice burrow__jsontext_read_value(JsontextDecoder *d, unsigned *flags, Error *err);
void burrow__jsontext_encoder_append_stack_pointer(JsontextEncoder *e, JsonBuf *b,
                                                   int where);
void burrow__jsontext_decoder_append_stack_pointer(JsontextDecoder *d, JsonBuf *b,
                                                   int where);
Int burrow__jsontext_encoder_count_next_delim_whitespace(const JsontextEncoder *e);
Int burrow__jsontext_decoder_count_next_delim_whitespace(JsontextDecoder *d);
Error burrow__jsontext_skip_value_remainder(JsontextDecoder *d);
Error burrow__jsontext_check_next_value(JsontextDecoder *d, bool last);
int burrow__jsontext_insert_unquoted(JsontextDecoder *d, Str name);
int burrow__jsontext_encoder_insert_unquoted(JsontextEncoder *e, Str name);
Error burrow__jsontext_syntactic_new(int64_t offset, Str pointer, Error err);
void burrow__jsontext_put_go_quote(JsonBuf *b, Str s);

/* stateMachine.MayAppendDelim, DisableNamespace and
 * InvalidateDisabledNamespaces. */
static inline void jsonstate_may_append_delim(const JsonState *s, JsonBuf *b,
                                              JsontextKind next) {
    Byte delim = jt_need_delim(s, next);
    if (delim != 0)
        jsonbuf_byte(b, delim);
}

static inline void jsonstate_disable_namespace(JsonState *s) {
    s->last |= JT_DISABLE_NAMESPACE;
}

static inline void jsonstate_invalidate_disabled_namespaces(JsonState *s) {
    for (Int i = 0; i < jt_depth(s); i++) {
        uint64_t *e = i == s->stack_len ? &s->last : &s->stack[i];
        if (!jt_e_active_ns(*e))
            *e |= JT_INVALID_NAMESPACE;
    }
}

#endif /* BURROW_SRC_ENCODING_JSON_INTERNAL_H */
