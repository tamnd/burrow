/* Derived from Go's src/encoding/asn1/asn1.go, common.go and marshal.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/asn1.h"

#include "asn1_internal.h"

#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/mem/heap.h"
#include "burrow/stack.h"
#include "burrow/strconv.h"
#include "burrow/thread.h"
#include "burrow/utf8.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* ------------------------------------------------------------------ errors
 *
 * StructuralError and SyntaxError are both a struct holding one string, so
 * the two share a box and differ in their prefix and their table. */

typedef struct Asn1ErrorBox {
    Str msg;
    Str message;
} Asn1ErrorBox;

static const Field asn1_msg_fields[] = {
    {BURROW_S_INIT("Msg"), {NULL, 0}, TYPE_STRING, 0},
};

static const Type asn1_structural_desc = {
    BURROW_S_INIT("StructuralError"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_STRUCT,
    (uint32_t)sizeof(Asn1StructuralError),
    (uint16_t)_Alignof(Asn1StructuralError),
    1,
    0,
    asn1_msg_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x61736e73U, /* "asns" */
    NULL,
};

static const Type asn1_syntax_desc = {
    BURROW_S_INIT("SyntaxError"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_STRUCT,
    (uint32_t)sizeof(Asn1SyntaxError),
    (uint16_t)_Alignof(Asn1SyntaxError),
    1,
    0,
    asn1_msg_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x61736e78U, /* "asnx" */
    NULL,
};

const Type *const TYPE_ASN1_STRUCTURAL_ERROR = &asn1_structural_desc;
const Type *const TYPE_ASN1_SYNTAX_ERROR = &asn1_syntax_desc;

#define ASN1_STRUCTURAL_PREFIX "asn1: structure error: "
#define ASN1_SYNTAX_PREFIX "asn1: syntax error: "

static Str asn1_error_message(const void *self) {
    return ((const Asn1ErrorBox *)self)->message;
}

static bool asn1_structural_is(const void *self, Error target);
static bool asn1_syntax_is(const void *self, Error target);
static Error asn1_structural_clone(const void *self, Alloc *a);
static Error asn1_syntax_clone(const void *self, Alloc *a);

static const ErrorVT asn1_structural_vt = {
    .self_type = &asn1_structural_desc,
    .message = asn1_error_message,
    .is = asn1_structural_is,
    .clone = asn1_structural_clone,
};

static const ErrorVT asn1_syntax_vt = {
    .self_type = &asn1_syntax_desc,
    .message = asn1_error_message,
    .is = asn1_syntax_is,
    .clone = asn1_syntax_clone,
};

static bool asn1_error_is(const ErrorVT *vt, const void *self, Error target) {
    if (target.vt != vt || target.data == NULL)
        return false;
    return str_eq(((const Asn1ErrorBox *)self)->msg,
                  ((const Asn1ErrorBox *)target.data)->msg);
}

static bool asn1_structural_is(const void *self, Error target) {
    return asn1_error_is(&asn1_structural_vt, self, target);
}

static bool asn1_syntax_is(const void *self, Error target) {
    return asn1_error_is(&asn1_syntax_vt, self, target);
}

static Error asn1_error_make(const ErrorVT *vt, const char *prefix, Str msg, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Int mlen = plen + msg.len;
    Asn1ErrorBox *b = (Asn1ErrorBox *)mem_alloc_nozero(
        a, sizeof(Asn1ErrorBox) + (size_t)mlen, _Alignof(Asn1ErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    memcpy(p, prefix, (size_t)plen);
    if (msg.len > 0)
        memcpy(p + plen, msg.p, (size_t)msg.len);
    b->message = str_from_bytes(p, mlen);
    b->msg = str_from_bytes(p + plen, msg.len);
    return (Error){vt, b};
}

static Str asn1_error_text(const char *prefix, Str msg, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + msg.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix, (size_t)plen);
    if (msg.len > 0)
        memcpy(p + plen, msg.p, (size_t)msg.len);
    return str_from_bytes(p, plen + msg.len);
}

Error asn1_structural_error_as_error(Asn1StructuralError e, Alloc *a) {
    return asn1_error_make(&asn1_structural_vt, ASN1_STRUCTURAL_PREFIX, e.msg, a);
}

Error asn1_syntax_error_as_error(Asn1SyntaxError e, Alloc *a) {
    return asn1_error_make(&asn1_syntax_vt, ASN1_SYNTAX_PREFIX, e.msg, a);
}

Str asn1_structural_error_error(Asn1StructuralError e, Alloc *a) {
    return asn1_error_text(ASN1_STRUCTURAL_PREFIX, e.msg, a);
}

Str asn1_syntax_error_error(Asn1SyntaxError e, Alloc *a) {
    return asn1_error_text(ASN1_SYNTAX_PREFIX, e.msg, a);
}

static Error asn1_structural_clone(const void *self, Alloc *a) {
    return asn1_structural_error_as_error(
        (Asn1StructuralError){((const Asn1ErrorBox *)self)->msg}, a);
}

static Error asn1_syntax_clone(const void *self, Alloc *a) {
    return asn1_syntax_error_as_error(
        (Asn1SyntaxError){((const Asn1ErrorBox *)self)->msg}, a);
}

/* The errors with a fixed message, in read only memory. text is pasted onto
 * the prefix, so it cannot have parentheses around it. */
/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define ASN1_ERROR(name, vt, prefix, text)                                             \
    static const Asn1ErrorBox name##__box = {                                          \
        {(const Byte *)prefix text + sizeof prefix - 1, sizeof text - 1},              \
        {(const Byte *)prefix text, sizeof prefix text - 1}};                          \
    static const Error name = {&vt, &name##__box}
#define ASN1_STRUCTURAL(name, text)                                                    \
    ASN1_ERROR(name, asn1_structural_vt, ASN1_STRUCTURAL_PREFIX, text)
#define ASN1_SYNTAX(name, text)                                                        \
    ASN1_ERROR(name, asn1_syntax_vt, ASN1_SYNTAX_PREFIX, text)
#define ASN1_PLAIN(name, text)                                                         \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}
/* NOLINTEND(bugprone-macro-parentheses) */

ASN1_SYNTAX(asn1_err_invalid_boolean, "invalid boolean");
ASN1_SYNTAX(asn1_err_zero_bit_string, "zero length BIT STRING");
ASN1_SYNTAX(asn1_err_bit_string_padding, "invalid padding bits in BIT STRING");
ASN1_SYNTAX(asn1_err_zero_oid, "zero length OBJECT IDENTIFIER");
ASN1_SYNTAX(asn1_err_base128_not_minimal, "integer is not minimally encoded");
ASN1_SYNTAX(asn1_err_base128_truncated, "truncated base 128 integer");
ASN1_SYNTAX(asn1_err_numeric_syntax, "NumericString contains invalid character");
ASN1_SYNTAX(asn1_err_printable_syntax, "PrintableString contains invalid character");
ASN1_SYNTAX(asn1_err_ia5_syntax, "IA5String contains invalid character");
ASN1_SYNTAX(asn1_err_non_minimal_tag, "non-minimal tag");
ASN1_SYNTAX(asn1_err_truncated_tag, "truncated tag or length");
ASN1_SYNTAX(asn1_err_indefinite, "indefinite length found (not DER)");
ASN1_SYNTAX(asn1_err_truncated_sequence, "truncated sequence");
ASN1_SYNTAX(asn1_err_sequence_truncated, "sequence truncated");
ASN1_SYNTAX(asn1_err_data_truncated, "data truncated");

ASN1_STRUCTURAL(asn1_err_empty_integer, "empty integer");
ASN1_STRUCTURAL(asn1_err_integer_not_minimal, "integer not minimally-encoded");
ASN1_STRUCTURAL(asn1_err_integer_too_large, "integer too large");
ASN1_STRUCTURAL(asn1_err_base128_too_large, "base 128 integer too large");
ASN1_STRUCTURAL(asn1_err_length_too_large, "length too large");
ASN1_STRUCTURAL(asn1_err_leading_zeros, "superfluous leading zeros in length");
ASN1_STRUCTURAL(asn1_err_non_minimal_length, "non-minimal length");
ASN1_STRUCTURAL(asn1_err_slice_type, "unknown Go type for slice");
ASN1_STRUCTURAL(asn1_err_sequence_mismatch, "sequence tag mismatch");
ASN1_STRUCTURAL(asn1_err_depth, "nesting depth exceeded");
ASN1_STRUCTURAL(asn1_err_explicit_no_child, "explicit tag has no child");
ASN1_STRUCTURAL(asn1_err_explicit_not_flag,
                "zero length explicit tag was not an asn1.Flag");
ASN1_STRUCTURAL(asn1_err_explicit_mismatch, "explicitly tagged member didn't match");
ASN1_STRUCTURAL(asn1_err_unexported, "struct contains unexported fields");
ASN1_STRUCTURAL(asn1_err_invalid_oid, "invalid object identifier");
ASN1_STRUCTURAL(asn1_err_printable_value, "PrintableString contains invalid character");
ASN1_STRUCTURAL(asn1_err_ia5_value, "IA5String contains invalid character");
ASN1_STRUCTURAL(asn1_err_numeric_value, "NumericString contains invalid character");
ASN1_STRUCTURAL(asn1_err_utc_range, "cannot represent time as UTCTime");
ASN1_STRUCTURAL(asn1_err_generalized_range, "cannot represent time as GeneralizedTime");
ASN1_STRUCTURAL(asn1_err_unknown_type, "unknown Go type");
ASN1_STRUCTURAL(asn1_err_time_type, "explicit time type given to non-time member");
ASN1_STRUCTURAL(asn1_err_string_type,
                "explicit string type given to non-string member");
ASN1_STRUCTURAL(asn1_err_set, "non sequence tagged as set");

ASN1_PLAIN(asn1_err_utf8_string, "asn1: invalid UTF-8 string");
ASN1_PLAIN(asn1_err_bmp, "invalid BMPString");
ASN1_PLAIN(asn1_err_internal_tag, "asn1: internal error in parseTagAndLength");
ASN1_PLAIN(asn1_err_nil_value, "asn1: cannot marshal nil value");
ASN1_PLAIN(asn1_err_marshal_utf8, "asn1: string not valid UTF-8");
ASN1_PLAIN(asn1_err_nil_recipient, "asn1: Unmarshal recipient value is nil");

/* A StructuralError or a SyntaxError with a message built at run time. Both
 * the message and the error live in the error arena. */
static Error asn1_structural(Str msg) {
    return asn1_structural_error_as_error((Asn1StructuralError){msg},
                                          error_allocator());
}

static Error asn1_syntax(Str msg) {
    return asn1_syntax_error_as_error((Asn1SyntaxError){msg}, error_allocator());
}

/* A value for fmt's %T, which reads the type and never the data. */
static Any asn1_type_arg(const Type *t) {
    static const uint64_t dummy[2] = {0, 0};
    return (Any){t, (void *)(uintptr_t)dummy};
}

/* ------------------------------------------------------------------- types */

static const Field asn1_bit_string_fields[] = {
    {BURROW_S_INIT("Bytes"),
     {NULL, 0},
     TYPE_BYTES,
     (uint32_t)offsetof(Asn1BitString, bytes)},
    {BURROW_S_INIT("BitLength"),
     {NULL, 0},
     TYPE_INT,
     (uint32_t)offsetof(Asn1BitString, bit_length)},
};

const Type burrow_type_Asn1BitString = {
    BURROW_S_INIT("BitString"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_STRUCT,
    (uint32_t)sizeof(Asn1BitString),
    (uint16_t)_Alignof(Asn1BitString),
    2,
    0,
    asn1_bit_string_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str asn1_oid_m_string(Asn1ObjectIdentifier *self) {
    return asn1_object_identifier_string(*self, error_allocator());
}

#define ASN1_SIG_STRING(IN, OUT) OUT(Str)

#define ASN1_OID_METHODS(M, T) M(T, String, asn1_oid_m_string, ASN1_SIG_STRING)

BURROW_METHODS_DEFINE(Asn1ObjectIdentifier, ASN1_OID_METHODS);

const Type burrow_type_Asn1ObjectIdentifier = {
    BURROW_S_INIT("ObjectIdentifier"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    (uint16_t)(sizeof burrow__methods_Asn1ObjectIdentifier /
               sizeof burrow__methods_Asn1ObjectIdentifier[0]),
    NULL,
    burrow__methods_Asn1ObjectIdentifier,
    TYPE_INT,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_Asn1Enumerated = {
    BURROW_S_INIT("Enumerated"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_Asn1Flag = {
    BURROW_S_INIT("Flag"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_BOOL,
    (uint32_t)sizeof(bool),
    (uint16_t)_Alignof(bool),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_Asn1RawContent = {
    BURROW_S_INIT("RawContent"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};

static const Field asn1_raw_value_fields[] = {
    {BURROW_S_INIT("Class"),
     {NULL, 0},
     TYPE_INT,
     (uint32_t)offsetof(Asn1RawValue, cls)},
    {BURROW_S_INIT("Tag"), {NULL, 0}, TYPE_INT, (uint32_t)offsetof(Asn1RawValue, tag)},
    {BURROW_S_INIT("IsCompound"),
     {NULL, 0},
     TYPE_BOOL,
     (uint32_t)offsetof(Asn1RawValue, is_compound)},
    {BURROW_S_INIT("Bytes"),
     {NULL, 0},
     TYPE_BYTES,
     (uint32_t)offsetof(Asn1RawValue, bytes)},
    {BURROW_S_INIT("FullBytes"),
     {NULL, 0},
     TYPE_BYTES,
     (uint32_t)offsetof(Asn1RawValue, full_bytes)},
};

const Type burrow_type_Asn1RawValue = {
    BURROW_S_INIT("RawValue"),
    BURROW_S_INIT("encoding/asn1"),
    KIND_STRUCT,
    (uint32_t)sizeof(Asn1RawValue),
    (uint16_t)_Alignof(Asn1RawValue),
    5,
    0,
    asn1_raw_value_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Asn1RawValue asn1_null_raw_value = {
    ASN1_CLASS_UNIVERSAL,    ASN1_TAG_NULL,           false,
    {NULL, 0, 0, TYPE_BYTE}, {NULL, 0, 0, TYPE_BYTE},
};

static const Byte asn1_null_bytes_data[2] = {ASN1_TAG_NULL, 0};
const Slice asn1_null_bytes = {(void *)(uintptr_t)asn1_null_bytes_data, 2, 2,
                               TYPE_BYTE};

/* ----------------------------------------------------------------- helpers */

static Slice asn1_bytes(const Byte *p, Int n) {
    return (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE};
}

static const Byte *asn1_ptr(Slice b) {
    return (const Byte *)b.p;
}

static Str asn1_str_copy(Alloc *a, const Byte *p, Int n, Error *err) {
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    memcpy(q, p, (size_t)n);
    return str_from_bytes(q, n);
}

static bool asn1_is_int_kind(Kind k) {
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64;
}

static bool asn1_is_any(const Type *t) {
    return t->kind == KIND_INTERFACE && t->nmethod == 0;
}

static bool asn1_is_big_int(const Type *t) {
    return t->kind == KIND_POINTER && t->elem == TYPE_OF(BigInt);
}

static int64_t asn1_get_int(const Type *t, const void *v) {
    switch (t->size) {
    case 1:
        return *(const int8_t *)v;
    case 2:
        return *(const int16_t *)v;
    case 4:
        return *(const int32_t *)v;
    default:
        return *(const int64_t *)v;
    }
}

static void asn1_set_int(const Type *t, void *v, int64_t x) {
    switch (t->size) {
    case 1:
        *(int8_t *)v = (int8_t)x;
        return;
    case 2:
        *(int16_t *)v = (int16_t)x;
        return;
    case 4:
        *(int32_t *)v = (int32_t)x;
        return;
    default:
        *(int64_t *)v = x;
        return;
    }
}

/* x as the type t would hold it, which is what Go's SetInt leaves behind. */
static int64_t asn1_trunc_int(const Type *t, int64_t x) {
    switch (t->size) {
    case 1:
        return (int8_t)x;
    case 2:
        return (int16_t)x;
    case 4:
        return (int32_t)x;
    default:
        return x;
    }
}

/* ------------------------------------------------------------ stack room
 *
 * The same check encoding/gob makes: Go's stack grows to fit any nesting and a
 * C stack does not, so each level asks whether there is room for the next.
 * On a goroutine the bounds are one read away, and on a thread of the system's
 * own they are only looked up once the nesting is deep enough to matter. */

/* Go limits decoding to 10000 levels and encoding not at all. Where the stack
 * cannot be measured, encoding stops at a bound well past anything real. */
enum {
    ASN1_MAX_DEPTH = 10000,
    ASN1_MAX_ENCODE_DEPTH = 100000,
    ASN1_THREAD_CHECK_AFTER = 32,
};

#define ASN1_STACK_MARGIN ((uintptr_t)64 << 10)

static uintptr_t asn1_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

/* Whether the stack is too low to go one level deeper. *floor starts at 0. */
static bool asn1_stack_low(uintptr_t *floor, int depth, int max) {
    if (*floor == 0) {
        burrow__Stack *s = burrow__stack_current();
        if (s != NULL)
            *floor = s->lo == NULL ? 1 : (uintptr_t)s->lo + ASN1_STACK_MARGIN;
        else
            *floor = 2;
    }
    if (*floor == 2) {
        if (depth < ASN1_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        void *hi = NULL;
        if (!burrow__thread_stack_bounds(&lo, &hi))
            lo = NULL;
        *floor = lo == NULL ? 1 : (uintptr_t)lo + ASN1_STACK_MARGIN;
    }
    if (*floor == 1)
        return depth > max;
    return asn1_stack_here() < *floor;
}

/* --------------------------------------------------------- field parameters */

Asn1FieldParameters burrow__asn1_parse_field_parameters(Str str) {
    Asn1FieldParameters ret;
    memset(&ret, 0, sizeof ret);
    const Byte *p = str.p;
    Int n = str.len;
    while (n > 0) {
        Int i = 0;
        while (i < n && p[i] != ',')
            i++;
        Str part = str_from_bytes(p, i);
        if (i < n) {
            p += i + 1;
            n -= i + 1;
        } else {
            n = 0;
        }
        if (str_eq(part, BURROW_S("optional"))) {
            ret.optional = true;
        } else if (str_eq(part, BURROW_S("explicit"))) {
            ret.is_explicit = true;
            ret.has_tag = true;
        } else if (str_eq(part, BURROW_S("generalized"))) {
            ret.time_type = ASN1_TAG_GENERALIZED_TIME;
        } else if (str_eq(part, BURROW_S("utc"))) {
            ret.time_type = ASN1_TAG_UTC_TIME;
        } else if (str_eq(part, BURROW_S("ia5"))) {
            ret.string_type = ASN1_TAG_IA5_STRING;
        } else if (str_eq(part, BURROW_S("printable"))) {
            ret.string_type = ASN1_TAG_PRINTABLE_STRING;
        } else if (str_eq(part, BURROW_S("numeric"))) {
            ret.string_type = ASN1_TAG_NUMERIC_STRING;
        } else if (str_eq(part, BURROW_S("utf8"))) {
            ret.string_type = ASN1_TAG_UTF8_STRING;
        } else if (part.len >= 8 && memcmp(part.p, "default:", 8) == 0) {
            Error err = BURROW_NO_ERROR;
            int64_t v = strconv_parse_int(str_from_bytes(part.p + 8, part.len - 8), 10,
                                          64, &err);
            if (BURROW_OK(err)) {
                ret.has_default = true;
                ret.default_value = v;
            }
        } else if (part.len >= 4 && memcmp(part.p, "tag:", 4) == 0) {
            Error err = BURROW_NO_ERROR;
            Int v = strconv_atoi(str_from_bytes(part.p + 4, part.len - 4), &err);
            if (BURROW_OK(err)) {
                ret.has_tag = true;
                ret.tag = v;
            }
        } else if (str_eq(part, BURROW_S("set"))) {
            ret.set = true;
        } else if (str_eq(part, BURROW_S("application"))) {
            ret.application = true;
            ret.has_tag = true;
        } else if (str_eq(part, BURROW_S("private"))) {
            ret.is_private = true;
            ret.has_tag = true;
        } else if (str_eq(part, BURROW_S("omitempty"))) {
            ret.omit_empty = true;
        }
    }
    return ret;
}

/* The parameters in a struct field's asn1 tag. */
static Asn1FieldParameters asn1_field_params(const Field *f) {
    return burrow__asn1_parse_field_parameters(
        tag_get(error_allocator(), f->tag, BURROW_S("asn1")));
}

/* getUniversalType. */
static bool asn1_universal_type(const Type *t, bool *match_any, Int *tag,
                                bool *compound) {
    *match_any = false;
    *compound = false;
    if (t == TYPE_ASN1_RAW_VALUE) {
        *match_any = true;
        *tag = -1;
        return true;
    }
    if (t == TYPE_ASN1_OBJECT_IDENTIFIER) {
        *tag = ASN1_TAG_OID;
        return true;
    }
    if (t == TYPE_ASN1_BIT_STRING) {
        *tag = ASN1_TAG_BIT_STRING;
        return true;
    }
    if (t == TYPE_TIME) {
        *tag = ASN1_TAG_UTC_TIME;
        return true;
    }
    if (t == TYPE_ASN1_ENUMERATED) {
        *tag = ASN1_TAG_ENUM;
        return true;
    }
    if (asn1_is_big_int(t)) {
        *tag = ASN1_TAG_INTEGER;
        return true;
    }
    switch ((int)t->kind) {
    case KIND_BOOL:
        *tag = ASN1_TAG_BOOLEAN;
        return true;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        *tag = ASN1_TAG_INTEGER;
        return true;
    case KIND_STRUCT:
        *tag = ASN1_TAG_SEQUENCE;
        *compound = true;
        return true;
    case KIND_SLICE:
        if (t->elem->kind == KIND_UINT8) {
            *tag = ASN1_TAG_OCTET_STRING;
            return true;
        }
        *compound = true;
        if (t->name.len >= 3 && memcmp(t->name.p + t->name.len - 3, "SET", 3) == 0)
            *tag = ASN1_TAG_SET;
        else
            *tag = ASN1_TAG_SEQUENCE;
        return true;
    case KIND_STRING:
        *tag = ASN1_TAG_PRINTABLE_STRING;
        return true;
    default:
        *tag = 0;
        return false;
    }
}

/* ------------------------------------------------------------ the parsers */

bool burrow__asn1_parse_bool(Slice b, Error *err) {
    *err = BURROW_NO_ERROR;
    if (b.len != 1) {
        *err = asn1_err_invalid_boolean;
        return false;
    }
    switch (asn1_ptr(b)[0]) {
    case 0:
        return false;
    case 0xff:
        return true;
    default:
        *err = asn1_err_invalid_boolean;
        return false;
    }
}

/* checkInteger: whether b is a valid, minimally encoded INTEGER. */
static Error asn1_check_integer(Slice b) {
    const Byte *p = asn1_ptr(b);
    if (b.len == 0)
        return asn1_err_empty_integer;
    if (b.len == 1)
        return BURROW_NO_ERROR;
    if ((p[0] == 0 && (p[1] & 0x80) == 0) || (p[0] == 0xff && (p[1] & 0x80) == 0x80))
        return asn1_err_integer_not_minimal;
    return BURROW_NO_ERROR;
}

int64_t burrow__asn1_parse_int64(Slice b, Error *err) {
    *err = asn1_check_integer(b);
    if (BURROW_FAILED(*err))
        return 0;
    if (b.len > 8) {
        *err = asn1_err_integer_too_large;
        return 0;
    }
    const Byte *p = asn1_ptr(b);
    uint64_t ret = 0;
    for (Int i = 0; i < b.len; i++)
        ret = ret << 8 | p[i];
    /* Sign extend from the top bit of the first byte. */
    unsigned shift = 64 - (unsigned)b.len * 8;
    if (shift > 0 && (p[0] & 0x80) != 0)
        ret |= ~(uint64_t)0 << (64 - shift);
    return (int64_t)ret;
}

int32_t burrow__asn1_parse_int32(Slice b, Error *err) {
    *err = asn1_check_integer(b);
    if (BURROW_FAILED(*err))
        return 0;
    int64_t ret64 = burrow__asn1_parse_int64(b, err);
    if (BURROW_FAILED(*err))
        return 0;
    if (ret64 != (int64_t)(int32_t)ret64) {
        *err = asn1_err_integer_too_large;
        return 0;
    }
    return (int32_t)ret64;
}

BigInt *burrow__asn1_parse_big_int(Alloc *a, Slice b, Error *err) {
    *err = asn1_check_integer(b);
    if (BURROW_FAILED(*err))
        return NULL;
    BigInt *ret = big_new_int(a, 0);
    const Byte *p = asn1_ptr(b);
    if (b.len > 0 && (p[0] & 0x80) == 0x80) {
        Byte *not_bytes = (Byte *)mem_alloc_nozero(a, (size_t)b.len, 1);
        if (not_bytes == NULL) {
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        for (Int i = 0; i < b.len; i++)
            not_bytes[i] = (Byte)~p[i];
        BigInt one = BIG_INT(a);
        big_int_set_int64(&one, 1);
        big_int_set_bytes(ret, asn1_bytes(not_bytes, b.len));
        big_int_add(ret, ret, &one);
        big_int_neg(ret, ret);
        return ret;
    }
    big_int_set_bytes(ret, b);
    return ret;
}

Int asn1_bit_string_at(Asn1BitString b, Int i) {
    if (i < 0 || i >= b.bit_length)
        return 0;
    Int x = i / 8;
    unsigned y = 7 - (unsigned)(i % 8);
    return (Int)(((const Byte *)b.bytes.p)[x] >> y) & 1;
}

Slice asn1_bit_string_right_align(Asn1BitString b, Alloc *a) {
    unsigned shift = (unsigned)(8 - (b.bit_length % 8));
    if (shift == 8 || b.bytes.len == 0)
        return b.bytes;
    Slice ret = slice_make(a, TYPE_BYTE, b.bytes.len, b.bytes.len);
    if (ret.p == NULL)
        return ret;
    const Byte *s = (const Byte *)b.bytes.p;
    Byte *d = (Byte *)ret.p;
    d[0] = (Byte)(s[0] >> shift);
    for (Int i = 1; i < b.bytes.len; i++) {
        d[i] = (Byte)(s[i - 1] << (8 - shift));
        d[i] |= (Byte)(s[i] >> shift);
    }
    return ret;
}

Asn1BitString burrow__asn1_parse_bit_string(Slice b, Error *err) {
    Asn1BitString ret = {slice_nil(TYPE_BYTE), 0};
    *err = BURROW_NO_ERROR;
    if (b.len == 0) {
        *err = asn1_err_zero_bit_string;
        return ret;
    }
    const Byte *p = asn1_ptr(b);
    Int padding = p[0];
    if (padding > 7 || (b.len == 1 && padding > 0) ||
        (p[b.len - 1] & ((1U << p[0]) - 1)) != 0) {
        *err = asn1_err_bit_string_padding;
        return ret;
    }
    ret.bit_length = (b.len - 1) * 8 - padding;
    ret.bytes = asn1_bytes(p + 1, b.len - 1);
    return ret;
}

bool asn1_object_identifier_equal(Asn1ObjectIdentifier oi, Asn1ObjectIdentifier other) {
    if (oi.len != other.len)
        return false;
    for (Int i = 0; i < oi.len; i++)
        if (((const Int *)oi.p)[i] != ((const Int *)other.p)[i])
            return false;
    return true;
}

Str asn1_object_identifier_string(Asn1ObjectIdentifier oi, Alloc *a) {
    /* 20 bytes holds any Int and its dot. */
    Int cap = oi.len * 21;
    if (cap == 0)
        return BURROW_STR_EMPTY;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (buf == NULL)
        return BURROW_STR_EMPTY;
    Slice s = {buf, 0, cap, TYPE_BYTE};
    for (Int i = 0; i < oi.len; i++) {
        if (i > 0)
            ((Byte *)s.p)[s.len++] = '.';
        s = strconv_append_int(a, s, (int64_t)((const Int *)oi.p)[i], 10);
    }
    return str_from_bytes(s.p, s.len);
}

/* parseBase128Int: the value in *ret and the offset after it. */
static Int asn1_parse_base128_int(Slice b, Int offset, Int *ret, Error *err) {
    const Byte *p = asn1_ptr(b);
    int64_t ret64 = 0;
    *ret = 0;
    for (int shifted = 0; offset < b.len; shifted++) {
        if (shifted == 5) {
            *err = asn1_err_base128_too_large;
            return offset;
        }
        ret64 <<= 7;
        Byte c = p[offset];
        if (shifted == 0 && c == 0x80) {
            *err = asn1_err_base128_not_minimal;
            return offset;
        }
        ret64 |= (int64_t)(c & 0x7f);
        offset++;
        if ((c & 0x80) == 0) {
            *ret = (Int)ret64;
            if (ret64 > INT32_MAX)
                *err = asn1_err_base128_too_large;
            return offset;
        }
    }
    *err = asn1_err_base128_truncated;
    return offset;
}

Asn1ObjectIdentifier burrow__asn1_parse_object_identifier(Alloc *a, Slice b,
                                                          Error *err) {
    *err = BURROW_NO_ERROR;
    if (b.len == 0) {
        *err = asn1_err_zero_oid;
        return slice_nil(TYPE_INT);
    }
    Slice s = slice_make(a, TYPE_INT, b.len + 1, b.len + 1);
    if (s.p == NULL) {
        *err = burrow_err_out_of_memory;
        return s;
    }
    Int *v = (Int *)s.p;
    Int x = 0;
    Int offset = asn1_parse_base128_int(b, 0, &x, err);
    if (BURROW_FAILED(*err))
        return s;
    if (x < 80) {
        v[0] = x / 40;
        v[1] = x % 40;
    } else {
        v[0] = 2;
        v[1] = x - 80;
    }
    Int i = 2;
    for (; offset < b.len; i++) {
        offset = asn1_parse_base128_int(b, offset, &x, err);
        if (BURROW_FAILED(*err))
            return s;
        v[i] = x;
    }
    s.len = i;
    return s;
}

/* Parses s with layout, and fails unless formatting the result gives s back. */
static Time asn1_parse_time_exact(Alloc *a, Str layout, Str s, Error *err) {
    Time ret = time_parse(a, layout, s, err);
    if (BURROW_FAILED(*err))
        return ret;
    Byte buf[64];
    Slice out =
        time_append_format(ret, a, (Slice){buf, 0, (Int)sizeof buf, TYPE_BYTE}, layout);
    Str serialized = str_from_bytes(out.p, out.len);
    if (!str_eq(serialized, s))
        *err = fmt_errorf_v(
            "asn1: time did not serialize back to the original value and may "
            "be invalid: given %q, but serialized as %q",
            s, serialized);
    return ret;
}

Time burrow__asn1_parse_utc_time(Alloc *a, Slice b, Error *err) {
    Str s = str_from_bytes(b.p, b.len);
    Str layout = BURROW_S("0601021504Z0700");
    Time ret = time_parse(a, layout, s, err);
    if (BURROW_FAILED(*err)) {
        layout = BURROW_S("060102150405Z0700");
        ret = time_parse(a, layout, s, err);
    }
    if (BURROW_FAILED(*err))
        return ret;
    Byte buf[64];
    Slice out =
        time_append_format(ret, a, (Slice){buf, 0, (Int)sizeof buf, TYPE_BYTE}, layout);
    Str serialized = str_from_bytes(out.p, out.len);
    if (!str_eq(serialized, s)) {
        *err = fmt_errorf_v(
            "asn1: time did not serialize back to the original value and may "
            "be invalid: given %q, but serialized as %q",
            s, serialized);
        return ret;
    }
    if (time_year(ret) >= 2050)
        ret = time_add_date(ret, -100, 0, 0);
    return ret;
}

Time burrow__asn1_parse_generalized_time(Alloc *a, Slice b, Error *err) {
    return asn1_parse_time_exact(a, BURROW_S("20060102150405.999999999Z0700"),
                                 str_from_bytes(b.p, b.len), err);
}

static bool asn1_is_numeric(Byte b) {
    return ('0' <= b && b <= '9') || b == ' ';
}

static bool asn1_is_printable(Byte b, bool asterisk, bool ampersand) {
    return ('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') || ('0' <= b && b <= '9') ||
           ('\'' <= b && b <= ')') || ('+' <= b && b <= '/') || b == ' ' || b == ':' ||
           b == '=' || b == '?' || (asterisk && b == '*') || (ampersand && b == '&');
}

static Str asn1_parse_numeric_string(Alloc *a, Slice b, Error *err) {
    const Byte *p = asn1_ptr(b);
    for (Int i = 0; i < b.len; i++) {
        if (!asn1_is_numeric(p[i])) {
            *err = asn1_err_numeric_syntax;
            return BURROW_STR_EMPTY;
        }
    }
    return asn1_str_copy(a, p, b.len, err);
}

static Str asn1_parse_printable_string(Alloc *a, Slice b, Error *err) {
    const Byte *p = asn1_ptr(b);
    for (Int i = 0; i < b.len; i++) {
        if (!asn1_is_printable(p[i], true, true)) {
            *err = asn1_err_printable_syntax;
            return BURROW_STR_EMPTY;
        }
    }
    return asn1_str_copy(a, p, b.len, err);
}

static Str asn1_parse_ia5_string(Alloc *a, Slice b, Error *err) {
    const Byte *p = asn1_ptr(b);
    for (Int i = 0; i < b.len; i++) {
        if (p[i] >= UTF8_RUNE_SELF) {
            *err = asn1_err_ia5_syntax;
            return BURROW_STR_EMPTY;
        }
    }
    return asn1_str_copy(a, p, b.len, err);
}

/* T61String, read as Latin-1, which is what Go does too. */
static Str asn1_parse_t61_string(Alloc *a, Slice b, Error *err) {
    const Byte *p = asn1_ptr(b);
    if (b.len == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)b.len * 2, 1);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    Int n = 0;
    for (Int i = 0; i < b.len; i++)
        n += utf8_encode_rune(asn1_bytes(q + n, 2), (Rune)p[i]);
    return str_from_bytes(q, n);
}

static Str asn1_parse_utf8_string(Alloc *a, Slice b, Error *err) {
    if (!utf8_valid(b)) {
        *err = asn1_err_utf8_string;
        return BURROW_STR_EMPTY;
    }
    return asn1_str_copy(a, asn1_ptr(b), b.len, err);
}

Str burrow__asn1_parse_bmp_string(Alloc *a, Slice b, Error *err) {
    *err = BURROW_NO_ERROR;
    const Byte *p = asn1_ptr(b);
    Int l = b.len;
    if (l % 2 != 0) {
        *err = asn1_err_bmp;
        return BURROW_STR_EMPTY;
    }
    /* Strip a terminator, which Go does too. */
    if (l >= 2 && p[l - 1] == 0 && p[l - 2] == 0)
        l -= 2;
    if (l == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(l / 2) * 3, 1);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    Int n = 0;
    for (Int i = 0; i < l; i += 2) {
        unsigned point = (unsigned)p[i] << 8 | p[i + 1];
        /* Not a character, and with the surrogates ruled out, every code
         * unit left is a rune on its own. */
        if (point == 0xfffe || point == 0xffff ||
            (point >= 0xfdd0 && point <= 0xfdef) ||
            (point >= 0xd800 && point <= 0xdfff)) {
            *err = asn1_err_bmp;
            return BURROW_STR_EMPTY;
        }
        n += utf8_encode_rune(asn1_bytes(q + n, 3), (Rune)point);
    }
    return str_from_bytes(q, n);
}

Int burrow__asn1_parse_tag_and_length(Slice b, Int offset, Asn1TagAndLength *ret,
                                      Error *err) {
    const Byte *p = asn1_ptr(b);
    *err = BURROW_NO_ERROR;
    memset(ret, 0, sizeof *ret);
    if (offset >= b.len) {
        *err = asn1_err_internal_tag;
        return offset;
    }
    Byte c = p[offset++];
    ret->cls = c >> 6;
    ret->is_compound = (c & 0x20) == 0x20;
    ret->tag = c & 0x1f;

    /* A tag of 31 means the real one follows in base 128. */
    if (ret->tag == 0x1f) {
        offset = asn1_parse_base128_int(b, offset, &ret->tag, err);
        if (BURROW_FAILED(*err))
            return offset;
        /* Short tags have to be encoded in the short form. */
        if (ret->tag < 0x1f) {
            *err = asn1_err_non_minimal_tag;
            return offset;
        }
    }
    if (offset >= b.len) {
        *err = asn1_err_truncated_tag;
        return offset;
    }
    c = p[offset++];
    if ((c & 0x80) == 0) {
        ret->length = c & 0x7f;
        return offset;
    }
    Int num_bytes = c & 0x7f;
    if (num_bytes == 0) {
        *err = asn1_err_indefinite;
        return offset;
    }
    ret->length = 0;
    for (Int i = 0; i < num_bytes; i++) {
        if (offset >= b.len) {
            *err = asn1_err_truncated_tag;
            return offset;
        }
        c = p[offset++];
        if (ret->length >= (Int)1 << 23) {
            /* Past 2^31 on a 32 bit system it would overflow. */
            *err = asn1_err_length_too_large;
            return offset;
        }
        ret->length <<= 8;
        ret->length |= c;
        if (ret->length == 0) {
            /* DER uses the fewest bytes it can. */
            *err = asn1_err_leading_zeros;
            return offset;
        }
    }
    /* A short length has to be encoded in the short form. */
    if (ret->length < 0x80)
        *err = asn1_err_non_minimal_length;
    return offset;
}

static bool asn1_invalid_length(Int offset, Int length, Int slice_length) {
    return length < 0 || offset > slice_length - length;
}

/* ------------------------------------------------------------ unmarshal */

typedef struct Asn1Dec {
    Alloc *a;
    uintptr_t floor;
} Asn1Dec;

static Int asn1_parse_field(Asn1Dec *d, const Type *t, void *v, Slice b,
                            Int init_offset, const Asn1FieldParameters *params,
                            int depth, Error *err);

/* parseSequenceOf: the elements in b, each of type elem, as a slice. */
static Slice asn1_parse_sequence_of(Asn1Dec *d, Slice b, const Type *elem, int depth,
                                    Error *err) {
    bool match_any = false;
    bool compound = false;
    Int expected = 0;
    if (!asn1_universal_type(elem, &match_any, &expected, &compound)) {
        *err = asn1_err_slice_type;
        return slice_nil(elem);
    }

    /* Count first, so that the slice is made once. */
    Int num = 0;
    for (Int offset = 0; offset < b.len;) {
        Asn1TagAndLength t;
        offset = burrow__asn1_parse_tag_and_length(b, offset, &t, err);
        if (BURROW_FAILED(*err))
            return slice_nil(elem);
        switch (t.tag) {
        case ASN1_TAG_IA5_STRING:
        case ASN1_TAG_GENERAL_STRING:
        case ASN1_TAG_T61_STRING:
        case ASN1_TAG_UTF8_STRING:
        case ASN1_TAG_NUMERIC_STRING:
        case ASN1_TAG_BMP_STRING:
            t.tag = ASN1_TAG_PRINTABLE_STRING;
            break;
        case ASN1_TAG_GENERALIZED_TIME:
        case ASN1_TAG_UTC_TIME:
            t.tag = ASN1_TAG_UTC_TIME;
            break;
        default:
            break;
        }
        if (!match_any && (t.cls != ASN1_CLASS_UNIVERSAL || t.is_compound != compound ||
                           t.tag != expected)) {
            *err = asn1_err_sequence_mismatch;
            return slice_nil(elem);
        }
        if (asn1_invalid_length(offset, t.length, b.len)) {
            *err = asn1_err_truncated_sequence;
            return slice_nil(elem);
        }
        offset += t.length;
        num++;
    }

    /* Go's saferio cap: no more than 10 MiB up front, however many elements
     * the input claims, and append grows it from there. */
    Int cap = num;
    if (elem->size > 0 && (uint64_t)num * elem->size > (uint64_t)10 << 20) {
        cap = (Int)(((uint64_t)10 << 20) / elem->size);
        if (cap == 0)
            cap = 1;
    }
    Slice ret = slice_make(d->a, elem, 0, cap);
    if (ret.p == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_nil(elem);
    }
    Asn1FieldParameters params;
    memset(&params, 0, sizeof params);
    Int offset = 0;
    for (Int i = 0; i < num; i++) {
        if (ret.len == ret.cap) {
            Int ncap = ret.cap * 2;
            if (ncap > num)
                ncap = num;
            Slice grown = slice_make(d->a, elem, ret.len, ncap);
            if (grown.p == NULL) {
                *err = burrow_err_out_of_memory;
                return slice_nil(elem);
            }
            if (ret.len > 0)
                memcpy(grown.p, ret.p, (size_t)ret.len * elem->size);
            ret = grown;
        }
        ret.len++;
        offset = asn1_parse_field(d, elem, (Byte *)ret.p + (size_t)i * elem->size, b,
                                  offset, &params, depth, err);
        if (BURROW_FAILED(*err))
            return slice_nil(elem);
    }
    return ret;
}

/* setDefaultValue: whether the field may be missing, setting it to its
 * default if it has one. */
static bool asn1_set_default_value(const Type *t, void *v,
                                   const Asn1FieldParameters *params) {
    if (!params->optional)
        return false;
    if (!params->has_default)
        return true;
    if (asn1_is_int_kind(t->kind))
        asn1_set_int(t, v, params->default_value);
    return true;
}

/* An Any holding a copy of *p, of type t, from a. */
static bool asn1_set_any(Alloc *a, void *v, const Type *t, const void *p) {
    Any boxed = any_box(a, (Any){t, (void *)(uintptr_t)p});
    if (boxed.t == NULL)
        return false;
    *(Any *)v = boxed;
    return true;
}

/* The Any case of parseField: whatever simple value is there, or nothing. */
static Int asn1_parse_any(Asn1Dec *d, void *v, Slice b, Int offset, Error *err) {
    Asn1TagAndLength t;
    offset = burrow__asn1_parse_tag_and_length(b, offset, &t, err);
    if (BURROW_FAILED(*err))
        return offset;
    if (asn1_invalid_length(offset, t.length, b.len)) {
        *err = asn1_err_data_truncated;
        return offset;
    }
    Slice inner = asn1_bytes(asn1_ptr(b) + offset, t.length);
    offset += t.length;
    if (t.is_compound || t.cls != ASN1_CLASS_UNIVERSAL)
        return offset;

    Alloc *a = d->a;
    const Type *rt = NULL;
    bool bv = false;
    int64_t iv = 0;
    Str sv = BURROW_STR_EMPTY;
    Asn1BitString bs;
    Slice sl;
    Time tv;
    const void *src = NULL;
    switch (t.tag) {
    case ASN1_TAG_BOOLEAN:
        bv = burrow__asn1_parse_bool(inner, err);
        rt = TYPE_BOOL;
        src = &bv;
        break;
    case ASN1_TAG_PRINTABLE_STRING:
        sv = asn1_parse_printable_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    case ASN1_TAG_NUMERIC_STRING:
        sv = asn1_parse_numeric_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    case ASN1_TAG_IA5_STRING:
        sv = asn1_parse_ia5_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    case ASN1_TAG_T61_STRING:
        sv = asn1_parse_t61_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    case ASN1_TAG_UTF8_STRING:
        sv = asn1_parse_utf8_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    case ASN1_TAG_INTEGER:
        iv = burrow__asn1_parse_int64(inner, err);
        rt = TYPE_INT64;
        src = &iv;
        break;
    case ASN1_TAG_BIT_STRING:
        bs = burrow__asn1_parse_bit_string(inner, err);
        rt = TYPE_ASN1_BIT_STRING;
        src = &bs;
        break;
    case ASN1_TAG_OID:
        sl = burrow__asn1_parse_object_identifier(a, inner, err);
        rt = TYPE_ASN1_OBJECT_IDENTIFIER;
        src = &sl;
        break;
    case ASN1_TAG_UTC_TIME:
        tv = burrow__asn1_parse_utc_time(a, inner, err);
        rt = TYPE_TIME;
        src = &tv;
        break;
    case ASN1_TAG_GENERALIZED_TIME:
        tv = burrow__asn1_parse_generalized_time(a, inner, err);
        rt = TYPE_TIME;
        src = &tv;
        break;
    case ASN1_TAG_OCTET_STRING:
        sl = inner;
        rt = TYPE_BYTES;
        src = &sl;
        break;
    case ASN1_TAG_BMP_STRING:
        sv = burrow__asn1_parse_bmp_string(a, inner, err);
        rt = TYPE_STRING;
        src = &sv;
        break;
    default:
        /* Not a type this package knows, which leaves the Any alone. */
        break;
    }
    if (BURROW_FAILED(*err) || rt == NULL)
        return offset;
    if (!asn1_set_any(a, v, rt, src))
        *err = burrow_err_out_of_memory;
    return offset;
}

/* "{class:0 tag:2 length:1 isCompound:false}", which is Go's %+v for one. */
static Str asn1_tag_and_length_string(const Asn1TagAndLength *t) {
    return fmt_sprintf_v(error_allocator(), "{class:%d tag:%d length:%d isCompound:%t}",
                         t->cls, t->tag, t->length, t->is_compound);
}

/* Go's %+v for fieldParameters. Go prints the two pointers as addresses,
 * which here are the addresses of the values in params. */
static Str asn1_params_string(const Asn1FieldParameters *p) {
    Alloc *ea = error_allocator();
    Str dv = p->has_default
                 ? fmt_sprintf_v(ea, "%#x", (uint64_t)(uintptr_t)&p->default_value)
                 : BURROW_S("<nil>");
    Str tag = p->has_tag ? fmt_sprintf_v(ea, "%#x", (uint64_t)(uintptr_t)&p->tag)
                         : BURROW_S("<nil>");
    return fmt_sprintf_v(
        ea,
        "{optional:%t explicit:%t application:%t private:%t defaultValue:%s "
        "tag:%s stringType:%d timeType:%d set:%t omitEmpty:%t}",
        p->optional, p->is_explicit, p->application, p->is_private, dv, tag,
        p->string_type, p->time_type, p->set, p->omit_empty);
}

/* parseField: reads the element at init_offset in b into v, of type t, and
 * gives the offset after it. */
static Int asn1_parse_field(Asn1Dec *d, const Type *ft, void *v, Slice b,
                            Int init_offset, const Asn1FieldParameters *params,
                            int depth, Error *err) {
    depth++;
    if (depth > ASN1_MAX_DEPTH || asn1_stack_low(&d->floor, depth, ASN1_MAX_DEPTH)) {
        *err = asn1_err_depth;
        return init_offset;
    }
    Int offset = init_offset;
    const Byte *bp = asn1_ptr(b);

    /* At the end of the data, the field has to be optional. */
    if (offset == b.len) {
        if (!asn1_set_default_value(ft, v, params))
            *err = asn1_err_sequence_truncated;
        return offset;
    }

    if (asn1_is_any(ft))
        return asn1_parse_any(d, v, b, offset, err);

    Asn1TagAndLength t;
    offset = burrow__asn1_parse_tag_and_length(b, offset, &t, err);
    if (BURROW_FAILED(*err))
        return offset;
    if (params->is_explicit) {
        Int expected_class = ASN1_CLASS_CONTEXT_SPECIFIC;
        if (params->application)
            expected_class = ASN1_CLASS_APPLICATION;
        if (offset == b.len) {
            *err = asn1_err_explicit_no_child;
            return offset;
        }
        if (t.cls == expected_class && t.tag == params->tag &&
            (t.length == 0 || t.is_compound)) {
            if (ft == TYPE_ASN1_RAW_VALUE) {
                /* The inner element is the value. */
            } else if (t.length > 0) {
                offset = burrow__asn1_parse_tag_and_length(b, offset, &t, err);
                if (BURROW_FAILED(*err))
                    return offset;
            } else {
                if (ft != TYPE_ASN1_FLAG) {
                    *err = asn1_err_explicit_not_flag;
                    return offset;
                }
                *(bool *)v = true;
                return offset;
            }
        } else {
            /* The tags did not match, so it may be an optional element. */
            if (asn1_set_default_value(ft, v, params))
                offset = init_offset;
            else
                *err = asn1_err_explicit_mismatch;
            return offset;
        }
    }

    bool match_any = false;
    bool compound = false;
    Int universal_tag = 0;
    if (!asn1_universal_type(ft, &match_any, &universal_tag, &compound)) {
        *err = asn1_structural(
            fmt_sprintf_v(error_allocator(), "unknown Go type: %T", asn1_type_arg(ft)));
        return offset;
    }

    /* Any of the string types fits a string, and the tag says which it is,
     * unless it was replaced by an implicit one, when the parameters say. */
    if (universal_tag == ASN1_TAG_PRINTABLE_STRING) {
        if (t.cls == ASN1_CLASS_UNIVERSAL) {
            switch (t.tag) {
            case ASN1_TAG_IA5_STRING:
            case ASN1_TAG_GENERAL_STRING:
            case ASN1_TAG_T61_STRING:
            case ASN1_TAG_UTF8_STRING:
            case ASN1_TAG_NUMERIC_STRING:
            case ASN1_TAG_BMP_STRING:
                universal_tag = t.tag;
                break;
            default:
                break;
            }
        } else if (params->string_type != 0) {
            universal_tag = params->string_type;
        }
    }

    /* The same for the two time types. */
    if (universal_tag == ASN1_TAG_UTC_TIME) {
        if (t.cls == ASN1_CLASS_UNIVERSAL) {
            if (t.tag == ASN1_TAG_GENERALIZED_TIME)
                universal_tag = t.tag;
        } else if (params->time_type != 0) {
            universal_tag = params->time_type;
        }
    }

    if (params->set)
        universal_tag = ASN1_TAG_SET;

    bool match_any_class_and_tag = match_any;
    Int expected_class = ASN1_CLASS_UNIVERSAL;
    Int expected_tag = universal_tag;

    if (!params->is_explicit && params->has_tag) {
        expected_class = ASN1_CLASS_CONTEXT_SPECIFIC;
        expected_tag = params->tag;
        match_any_class_and_tag = false;
    }
    if (!params->is_explicit && params->application && params->has_tag) {
        expected_class = ASN1_CLASS_APPLICATION;
        expected_tag = params->tag;
        match_any_class_and_tag = false;
    }
    if (!params->is_explicit && params->is_private && params->has_tag) {
        expected_class = ASN1_CLASS_PRIVATE;
        expected_tag = params->tag;
        match_any_class_and_tag = false;
    }

    /* The tag has to match unless the type is RawValue, which takes any. */
    if ((!match_any_class_and_tag &&
         (t.cls != expected_class || t.tag != expected_tag)) ||
        (!match_any && t.is_compound != compound)) {
        if (asn1_set_default_value(ft, v, params)) {
            offset = init_offset;
        } else {
            Str name = ft->name;
            *err = asn1_structural(fmt_sprintf_v(
                error_allocator(), "tags don't match (%d vs %s) %s %s @%d",
                expected_tag, asn1_tag_and_length_string(&t),
                asn1_params_string(params), name, offset));
        }
        return offset;
    }
    if (asn1_invalid_length(offset, t.length, b.len)) {
        *err = asn1_err_data_truncated;
        return offset;
    }
    Slice inner = asn1_bytes(bp + offset, t.length);
    offset += t.length;

    Alloc *a = d->a;
    if (ft == TYPE_ASN1_RAW_VALUE) {
        *(Asn1RawValue *)v =
            (Asn1RawValue){t.cls, t.tag, t.is_compound, inner,
                           asn1_bytes(bp + init_offset, offset - init_offset)};
        return offset;
    }
    if (ft == TYPE_ASN1_OBJECT_IDENTIFIER) {
        *(Slice *)v = burrow__asn1_parse_object_identifier(a, inner, err);
        return offset;
    }
    if (ft == TYPE_ASN1_BIT_STRING) {
        *(Asn1BitString *)v = burrow__asn1_parse_bit_string(inner, err);
        return offset;
    }
    if (ft == TYPE_TIME) {
        if (universal_tag == ASN1_TAG_UTC_TIME)
            *(Time *)v = burrow__asn1_parse_utc_time(a, inner, err);
        else
            *(Time *)v = burrow__asn1_parse_generalized_time(a, inner, err);
        return offset;
    }
    if (ft == TYPE_ASN1_ENUMERATED) {
        int32_t x = burrow__asn1_parse_int32(inner, err);
        if (BURROW_OK(*err))
            *(Asn1Enumerated *)v = x;
        return offset;
    }
    if (ft == TYPE_ASN1_FLAG) {
        *(bool *)v = true;
        return offset;
    }
    if (asn1_is_big_int(ft)) {
        BigInt *x = burrow__asn1_parse_big_int(a, inner, err);
        if (BURROW_OK(*err))
            *(BigInt **)v = x;
        return offset;
    }

    switch ((int)ft->kind) {
    case KIND_BOOL: {
        bool x = burrow__asn1_parse_bool(inner, err);
        if (BURROW_OK(*err))
            *(bool *)v = x;
        return offset;
    }
    case KIND_INT:
    case KIND_INT32:
    case KIND_INT64:
        if (ft->size == 4) {
            int32_t x = burrow__asn1_parse_int32(inner, err);
            if (BURROW_OK(*err))
                asn1_set_int(ft, v, x);
        } else {
            int64_t x = burrow__asn1_parse_int64(inner, err);
            if (BURROW_OK(*err))
                asn1_set_int(ft, v, x);
        }
        return offset;
    case KIND_STRUCT: {
        for (uint16_t i = 0; i < ft->nfield; i++) {
            if (!field_is_exported(&ft->fields[i])) {
                *err = asn1_err_unexported;
                return offset;
            }
        }
        uint16_t start = 0;
        if (ft->nfield > 0 && ft->fields[0].type == TYPE_ASN1_RAW_CONTENT) {
            *(Slice *)(void *)((Byte *)v + ft->fields[0].offset) =
                asn1_bytes(bp + init_offset, offset - init_offset);
            start = 1;
        }
        Int inner_offset = 0;
        for (uint16_t i = start; i < ft->nfield; i++) {
            const Field *f = &ft->fields[i];
            Asn1FieldParameters fp = asn1_field_params(f);
            inner_offset = asn1_parse_field(d, f->type, (Byte *)v + f->offset, inner,
                                            inner_offset, &fp, depth, err);
            if (BURROW_FAILED(*err))
                return offset;
        }
        /* Like Go, trailing elements are allowed, for forward
         * compatibility. */
        return offset;
    }
    case KIND_SLICE: {
        if (ft->elem->kind == KIND_UINT8) {
            Slice s = slice_make(a, ft->elem, inner.len, inner.len);
            if (s.p == NULL) {
                *err = burrow_err_out_of_memory;
                return offset;
            }
            if (inner.len > 0)
                memcpy(s.p, inner.p, (size_t)inner.len);
            *(Slice *)v = s;
            return offset;
        }
        Slice s = asn1_parse_sequence_of(d, inner, ft->elem, depth, err);
        if (BURROW_OK(*err))
            *(Slice *)v = s;
        return offset;
    }
    case KIND_STRING: {
        Str s = BURROW_STR_EMPTY;
        switch (universal_tag) {
        case ASN1_TAG_PRINTABLE_STRING:
            s = asn1_parse_printable_string(a, inner, err);
            break;
        case ASN1_TAG_NUMERIC_STRING:
            s = asn1_parse_numeric_string(a, inner, err);
            break;
        case ASN1_TAG_IA5_STRING:
            s = asn1_parse_ia5_string(a, inner, err);
            break;
        case ASN1_TAG_T61_STRING:
            s = asn1_parse_t61_string(a, inner, err);
            break;
        case ASN1_TAG_UTF8_STRING:
            s = asn1_parse_utf8_string(a, inner, err);
            break;
        case ASN1_TAG_GENERAL_STRING:
            /* GeneralString is ISO-2022 in theory and close enough to
             * T61String in the certificates that use it. */
            s = asn1_parse_t61_string(a, inner, err);
            break;
        case ASN1_TAG_BMP_STRING:
            s = burrow__asn1_parse_bmp_string(a, inner, err);
            break;
        default:
            *err = asn1_syntax(fmt_sprintf_v(error_allocator(),
                                             "internal error: unknown string type %d",
                                             universal_tag));
            break;
        }
        if (BURROW_OK(*err))
            *(Str *)v = s;
        return offset;
    }
    default:
        break;
    }
    *err = asn1_structural(
        fmt_sprintf_v(error_allocator(), "unsupported: %T", asn1_type_arg(ft)));
    return offset;
}

Slice asn1_unmarshal(Alloc *a, Slice b, Any v, Error *err) {
    return asn1_unmarshal_with_params(a, b, v, BURROW_STR_EMPTY, err);
}

Slice asn1_unmarshal_with_params(Alloc *a, Slice b, Any v, Str params, Error *err) {
    *err = BURROW_NO_ERROR;
    if (v.t == NULL) {
        *err = asn1_err_nil_recipient;
        return slice_nil(TYPE_BYTE);
    }
    if (v.data == NULL) {
        *err = fmt_errorf_v("asn1: Unmarshal recipient value is nil *%T",
                            asn1_type_arg(v.t));
        return slice_nil(TYPE_BYTE);
    }
    Asn1Dec d = {a, 0};
    Asn1FieldParameters fp = burrow__asn1_parse_field_parameters(params);
    Int offset = asn1_parse_field(&d, v.t, v.data, b, 0, &fp, 0, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    return (Slice){(Byte *)b.p + offset, b.len - offset, b.cap - offset, b.elem};
}

/* -------------------------------------------------------------- marshal
 *
 * Go builds a tree of encoders, sizes it and then writes it. This writes as it
 * goes instead, leaving one byte for each length and opening a gap for the
 * rest of it in the rare case that the body comes out 128 bytes or longer. A
 * SET is written in order and then sorted in place. */

typedef struct Asn1Enc {
    Alloc *h;
    Byte *p;
    Int len;
    Int cap;
    bool oom;
    uintptr_t floor;
} Asn1Enc;

static bool asn1_enc_reserve(Asn1Enc *e, Int n) {
    if (e->oom)
        return false;
    if (e->p != NULL && e->len + n <= e->cap)
        return true;
    Int ncap = e->cap < 64 ? 64 : e->cap * 2;
    while (ncap < e->len + n)
        ncap *= 2;
    Byte *np = (Byte *)mem_realloc(e->h, e->p, (size_t)e->cap, (size_t)ncap, 1);
    if (np == NULL) {
        e->oom = true;
        return false;
    }
    e->p = np;
    e->cap = ncap;
    return true;
}

static void asn1_enc_put(Asn1Enc *e, const void *p, Int n) {
    if (n == 0 || !asn1_enc_reserve(e, n))
        return;
    memcpy(e->p + e->len, p, (size_t)n);
    e->len += n;
}

static void asn1_enc_byte(Asn1Enc *e, Byte c) {
    if (!asn1_enc_reserve(e, 1))
        return;
    e->p[e->len++] = c;
}

static Int asn1_base128_int_length(int64_t n) {
    if (n == 0)
        return 1;
    Int l = 0;
    for (int64_t i = n; i > 0; i >>= 7)
        l++;
    return l;
}

static void asn1_enc_base128_int(Asn1Enc *e, int64_t n) {
    Int l = asn1_base128_int_length(n);
    for (Int i = l - 1; i >= 0; i--) {
        Byte o = (Byte)(n >> (unsigned)(i * 7));
        o &= 0x7f;
        if (i != 0)
            o |= 0x80;
        asn1_enc_byte(e, o);
    }
}

static Int asn1_length_length(Int i) {
    Int n = 1;
    while (i > 255) {
        n++;
        i >>= 8;
    }
    return n;
}

/* The identifier octets of appendTagAndLength. */
static void asn1_enc_tag(Asn1Enc *e, Int cls, Int tag, bool compound) {
    Byte c = (Byte)((uint8_t)cls << 6);
    if (compound)
        c |= 0x20;
    if (tag >= 31) {
        c |= 0x1f;
        asn1_enc_byte(e, c);
        asn1_enc_base128_int(e, (int64_t)tag);
    } else {
        c |= (Byte)tag;
        asn1_enc_byte(e, c);
    }
}

/* Leaves a byte for a length and gives its position, for asn1_enc_close. */
static Int asn1_enc_open(Asn1Enc *e) {
    Int pos = e->len;
    asn1_enc_byte(e, 0);
    return pos;
}

/* Writes the length of what came after pos, opening a gap for it if it is
 * long. */
static void asn1_enc_close(Asn1Enc *e, Int pos) {
    if (e->oom)
        return;
    Int body = e->len - (pos + 1);
    if (body < 128) {
        e->p[pos] = (Byte)body;
        return;
    }
    Int l = asn1_length_length(body);
    if (!asn1_enc_reserve(e, l))
        return;
    memmove(e->p + pos + 1 + l, e->p + pos + 1, (size_t)body);
    e->len += l;
    e->p[pos] = (Byte)(0x80 | l);
    for (Int i = 0; i < l; i++)
        e->p[pos + 1 + i] = (Byte)(body >> (unsigned)((l - 1 - i) * 8));
}

/* int64Encoder. */
static void asn1_enc_int64(Asn1Enc *e, int64_t i) {
    Int n = 1;
    for (int64_t x = i; x > 127; x >>= 8)
        n++;
    for (int64_t x = i; x < -128; x >>= 8)
        n++;
    for (Int j = 0; j < n; j++)
        asn1_enc_byte(e, (Byte)(i >> (unsigned)((n - 1 - j) * 8)));
}

static Error asn1_enc_big_int(Asn1Enc *e, const BigInt *n) {
    if (n == NULL)
        return asn1_err_empty_integer;
    Alloc *h = e->h;
    if (big_int_sign(n) < 0) {
        /* A negative number has to be converted to two's complement, which
         * is the bytes of -n-1 flipped, with 0xff on the front if the top
         * bit came out clear. */
        BigInt m = BIG_INT(h);
        BigInt one = BIG_INT(h);
        big_int_set_int64(&one, 1);
        big_int_neg(&m, n);
        big_int_sub(&m, &m, &one);
        Slice bytes = big_int_bytes(&m, h);
        Byte *bp = (Byte *)bytes.p;
        for (Int i = 0; i < bytes.len; i++)
            bp[i] ^= 0xff;
        if (bytes.len == 0 || (bp[0] & 0x80) == 0)
            asn1_enc_byte(e, 0xff);
        asn1_enc_put(e, bp, bytes.len);
        mem_free(h, bytes.p, (size_t)bytes.cap, 1);
        big_int_free(&m);
        big_int_free(&one);
        return BURROW_NO_ERROR;
    }
    if (big_int_sign(n) == 0) {
        asn1_enc_byte(e, 0x00);
        return BURROW_NO_ERROR;
    }
    Slice bytes = big_int_bytes(n, h);
    const Byte *bp = (const Byte *)bytes.p;
    /* Positive with the top bit set would read back as negative. */
    if (bytes.len > 0 && (bp[0] & 0x80) != 0)
        asn1_enc_byte(e, 0x00);
    asn1_enc_put(e, bp, bytes.len);
    mem_free(h, bytes.p, (size_t)bytes.cap, 1);
    return BURROW_NO_ERROR;
}

static void asn1_enc_two_digits(Asn1Enc *e, Int v) {
    asn1_enc_byte(e, (Byte)('0' + (v / 10) % 10));
    asn1_enc_byte(e, (Byte)('0' + v % 10));
}

static void asn1_enc_four_digits(Asn1Enc *e, Int v) {
    asn1_enc_byte(e, (Byte)('0' + (v / 1000) % 10));
    asn1_enc_byte(e, (Byte)('0' + (v / 100) % 10));
    asn1_enc_byte(e, (Byte)('0' + (v / 10) % 10));
    asn1_enc_byte(e, (Byte)('0' + v % 10));
}

static bool asn1_outside_utc_range(Time t) {
    Int year = time_year(t);
    return year < 1950 || year >= 2050;
}

static void asn1_enc_time_common(Asn1Enc *e, Time t) {
    TimeDateRet date = time_date_of(t);
    asn1_enc_two_digits(e, (Int)date.month);
    asn1_enc_two_digits(e, date.day);

    TimeClockRet clock = time_clock(t);
    asn1_enc_two_digits(e, clock.hour);
    asn1_enc_two_digits(e, clock.min);
    asn1_enc_two_digits(e, clock.sec);

    Int offset = 0;
    time_zone(t, &offset);
    if (offset / 60 == 0) {
        asn1_enc_byte(e, 'Z');
        return;
    }
    asn1_enc_byte(e, offset > 0 ? '+' : '-');
    Int minutes = offset / 60;
    if (minutes < 0)
        minutes = -minutes;
    asn1_enc_two_digits(e, minutes / 60);
    asn1_enc_two_digits(e, minutes % 60);
}

static Error asn1_enc_utc_time(Asn1Enc *e, Time t) {
    Int year = time_year(t);
    if (1950 <= year && year < 2000)
        asn1_enc_two_digits(e, year - 1900);
    else if (2000 <= year && year < 2050)
        asn1_enc_two_digits(e, year - 2000);
    else
        return asn1_err_utc_range;
    asn1_enc_time_common(e, t);
    return BURROW_NO_ERROR;
}

static Error asn1_enc_generalized_time(Asn1Enc *e, Time t) {
    Int year = time_year(t);
    if (year < 0 || year > 9999)
        return asn1_err_generalized_range;
    asn1_enc_four_digits(e, year);
    asn1_enc_time_common(e, t);
    return BURROW_NO_ERROR;
}

/* The whole of b after its tag and length, or b if they do not parse. */
static Slice asn1_strip_tag_and_length(Slice b) {
    Asn1TagAndLength t;
    Error err = BURROW_NO_ERROR;
    Int offset = burrow__asn1_parse_tag_and_length(b, 0, &t, &err);
    if (BURROW_FAILED(err))
        return b;
    return asn1_bytes(asn1_ptr(b) + offset, b.len - offset);
}

/* reflect.DeepEqual(v, zero value of t). */
static bool asn1_deep_zero(const Type *t, const void *v) {
    switch ((int)t->kind) {
    case KIND_FLOAT32:
        return *(const float *)v == 0;
    case KIND_FLOAT64:
        return *(const double *)v == 0;
    case KIND_COMPLEX64: {
        const float *c = (const float *)v;
        return c[0] == 0 && c[1] == 0;
    }
    case KIND_COMPLEX128: {
        const double *c = (const double *)v;
        return c[0] == 0 && c[1] == 0;
    }
    case KIND_STRING:
        return ((const Str *)v)->len == 0;
    case KIND_SLICE:
        return ((const Slice *)v)->p == NULL;
    case KIND_INTERFACE:
        if (t->nmethod == 0)
            return ((const Any *)v)->t == NULL;
        break;
    case KIND_ARRAY:
        for (uint32_t i = 0; i < t->len; i++)
            if (!asn1_deep_zero(t->elem, (const Byte *)v + (size_t)i * t->elem->size))
                return false;
        return true;
    case KIND_STRUCT:
        if (t->nfield == 0)
            break;
        for (uint16_t i = 0; i < t->nfield; i++)
            if (!asn1_deep_zero(t->fields[i].type,
                                (const Byte *)v + t->fields[i].offset))
                return false;
        return true;
    default:
        break;
    }
    const Byte *p = (const Byte *)v;
    for (uint32_t i = 0; i < t->size; i++)
        if (p[i] != 0)
            return false;
    return true;
}

static Error asn1_make_field(Asn1Enc *e, const Type *t, const void *v,
                             Asn1FieldParameters params, int depth);

/* The elements of a SET OF, which start at the offsets in starts and run to
 * the next one, are put in byte order, as DER requires. */
typedef struct Asn1Span {
    Int off;
    Int len;
} Asn1Span;

static int asn1_span_cmp(const Byte *base, Asn1Span x, Asn1Span y) {
    Int n = x.len < y.len ? x.len : y.len;
    int c = n > 0 ? memcmp(base + x.off, base + y.off, (size_t)n) : 0;
    if (c != 0)
        return c;
    return x.len < y.len ? -1 : x.len > y.len ? 1 : 0;
}

static void asn1_span_sort(const Byte *base, Asn1Span *s, Asn1Span *tmp, Int n) {
    if (n < 2)
        return;
    Int m = n / 2;
    asn1_span_sort(base, s, tmp, m);
    asn1_span_sort(base, s + m, tmp, n - m);
    Int i = 0;
    Int j = m;
    Int k = 0;
    while (i < m && j < n)
        tmp[k++] = asn1_span_cmp(base, s[j], s[i]) < 0 ? s[j++] : s[i++];
    while (i < m)
        tmp[k++] = s[i++];
    while (j < n)
        tmp[k++] = s[j++];
    memcpy(s, tmp, (size_t)n * sizeof *s);
}

static Error asn1_make_set(Asn1Enc *e, const Slice *s, int depth) {
    Alloc *h = e->h;
    const Type *elem = s->elem;
    Asn1Span *spans = (Asn1Span *)mem_alloc_nozero(
        h, (size_t)s->len * 2 * sizeof(Asn1Span), _Alignof(Asn1Span));
    if (spans == NULL)
        return burrow_err_out_of_memory;
    Asn1FieldParameters fp;
    memset(&fp, 0, sizeof fp);
    Int start = e->len;
    Error err = BURROW_NO_ERROR;
    for (Int i = 0; i < s->len; i++) {
        spans[i].off = e->len;
        err = asn1_make_field(e, elem, (const Byte *)s->p + (size_t)i * elem->size, fp,
                              depth);
        if (BURROW_FAILED(err))
            goto done;
        spans[i].len = e->len - spans[i].off;
    }
    if (e->oom)
        goto done;
    Int total = e->len - start;
    Byte *copy = (Byte *)mem_alloc_nozero(h, (size_t)total, 1);
    if (copy == NULL) {
        err = burrow_err_out_of_memory;
        goto done;
    }
    memcpy(copy, e->p + start, (size_t)total);
    for (Int i = 0; i < s->len; i++)
        spans[i].off -= start;
    asn1_span_sort(copy, spans, spans + s->len, s->len);
    Int off = start;
    for (Int i = 0; i < s->len; i++) {
        memcpy(e->p + off, copy + spans[i].off, (size_t)spans[i].len);
        off += spans[i].len;
    }
    mem_free(h, copy, (size_t)total, 1);
done:
    mem_free(h, spans, (size_t)s->len * 2 * sizeof(Asn1Span), _Alignof(Asn1Span));
    return err;
}

/* makeBody: the contents of v, without a tag or length. */
static Error asn1_make_body(Asn1Enc *e, const Type *t, const void *v,
                            const Asn1FieldParameters *params, int depth) {
    if (t == TYPE_ASN1_FLAG)
        return BURROW_NO_ERROR;
    if (t == TYPE_TIME) {
        Time tv = *(const Time *)v;
        if (params->time_type == ASN1_TAG_GENERALIZED_TIME ||
            asn1_outside_utc_range(tv))
            return asn1_enc_generalized_time(e, tv);
        return asn1_enc_utc_time(e, tv);
    }
    if (t == TYPE_ASN1_BIT_STRING) {
        const Asn1BitString *bs = (const Asn1BitString *)v;
        asn1_enc_byte(e, (Byte)((8 - bs->bit_length % 8) % 8));
        asn1_enc_put(e, bs->bytes.p, bs->bytes.len);
        return BURROW_NO_ERROR;
    }
    if (t == TYPE_ASN1_OBJECT_IDENTIFIER) {
        const Slice *oid = (const Slice *)v;
        const Int *arcs = (const Int *)oid->p;
        if (oid->len < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] >= 40))
            return asn1_err_invalid_oid;
        asn1_enc_base128_int(e, (int64_t)(arcs[0] * 40 + arcs[1]));
        for (Int i = 2; i < oid->len; i++)
            asn1_enc_base128_int(e, (int64_t)arcs[i]);
        return BURROW_NO_ERROR;
    }
    if (asn1_is_big_int(t))
        return asn1_enc_big_int(e, *(BigInt *const *)v);

    switch ((int)t->kind) {
    case KIND_BOOL:
        asn1_enc_byte(e, *(const bool *)v ? 0xff : 0x00);
        return BURROW_NO_ERROR;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        asn1_enc_int64(e, asn1_get_int(t, v));
        return BURROW_NO_ERROR;
    case KIND_STRUCT: {
        for (uint16_t i = 0; i < t->nfield; i++)
            if (!field_is_exported(&t->fields[i]))
                return asn1_err_unexported;
        uint16_t start = 0;
        if (t->nfield > 0 && t->fields[0].type == TYPE_ASN1_RAW_CONTENT) {
            const Slice *raw =
                (const Slice *)(const void *)((const Byte *)v + t->fields[0].offset);
            if (raw->len > 0) {
                /* The raw contents have their own tag and length, and the
                 * caller writes those. */
                Slice body = asn1_strip_tag_and_length(*raw);
                asn1_enc_put(e, body.p, body.len);
                return BURROW_NO_ERROR;
            }
            start = 1;
        }
        for (uint16_t i = start; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            Error err = asn1_make_field(e, f->type, (const Byte *)v + f->offset,
                                        asn1_field_params(f), depth);
            if (BURROW_FAILED(err))
                return err;
        }
        return BURROW_NO_ERROR;
    }
    case KIND_SLICE: {
        const Slice *s = (const Slice *)v;
        if (t->elem->kind == KIND_UINT8) {
            asn1_enc_put(e, s->p, s->len);
            return BURROW_NO_ERROR;
        }
        if (params->set && s->len > 1)
            return asn1_make_set(e, s, depth);
        Asn1FieldParameters fp;
        memset(&fp, 0, sizeof fp);
        for (Int i = 0; i < s->len; i++) {
            Error err = asn1_make_field(
                e, t->elem, (const Byte *)s->p + (size_t)i * t->elem->size, fp, depth);
            if (BURROW_FAILED(err))
                return err;
        }
        return BURROW_NO_ERROR;
    }
    case KIND_STRING: {
        Str s = *(const Str *)v;
        const Byte *p = s.p;
        switch (params->string_type) {
        case ASN1_TAG_IA5_STRING:
            for (Int i = 0; i < s.len; i++)
                if (p[i] > 127)
                    return asn1_err_ia5_value;
            break;
        case ASN1_TAG_PRINTABLE_STRING:
            for (Int i = 0; i < s.len; i++)
                if (!asn1_is_printable(p[i], true, false))
                    return asn1_err_printable_value;
            break;
        case ASN1_TAG_NUMERIC_STRING:
            for (Int i = 0; i < s.len; i++)
                if (!asn1_is_numeric(p[i]))
                    return asn1_err_numeric_value;
            break;
        default:
            break;
        }
        asn1_enc_put(e, s.p, s.len);
        return BURROW_NO_ERROR;
    }
    default:
        break;
    }
    return asn1_err_unknown_type;
}

/* makeField: v, of type t, with its tag and length. */
static Error asn1_make_field(Asn1Enc *e, const Type *t, const void *v,
                             Asn1FieldParameters params, int depth) {
    if (t == NULL || v == NULL)
        return asn1_err_nil_value;
    depth++;
    if (asn1_stack_low(&e->floor, depth, ASN1_MAX_ENCODE_DEPTH))
        return asn1_err_depth;
    if (asn1_is_any(t)) {
        const Any *x = (const Any *)v;
        return asn1_make_field(e, x->t, x->data, params, depth);
    }

    if (t->kind == KIND_SLICE && ((const Slice *)v)->len == 0 && params.omit_empty)
        return BURROW_NO_ERROR;

    if (params.optional && params.has_default && asn1_is_int_kind(t->kind) &&
        asn1_get_int(t, v) == asn1_trunc_int(t, params.default_value))
        return BURROW_NO_ERROR;

    /* An optional field without a default is left out when it is the zero
     * value, which Go decides with reflect.DeepEqual. */
    if (params.optional && !params.has_default && asn1_deep_zero(t, v))
        return BURROW_NO_ERROR;

    if (t == TYPE_ASN1_RAW_VALUE) {
        const Asn1RawValue *rv = (const Asn1RawValue *)v;
        if (rv->full_bytes.len != 0) {
            asn1_enc_put(e, rv->full_bytes.p, rv->full_bytes.len);
            return BURROW_NO_ERROR;
        }
        asn1_enc_tag(e, rv->cls, rv->tag, rv->is_compound);
        Int pos = asn1_enc_open(e);
        asn1_enc_put(e, rv->bytes.p, rv->bytes.len);
        asn1_enc_close(e, pos);
        return BURROW_NO_ERROR;
    }

    bool match_any = false;
    bool compound = false;
    Int tag = 0;
    if (!asn1_universal_type(t, &match_any, &tag, &compound) || match_any)
        return asn1_structural(
            fmt_sprintf_v(error_allocator(), "unknown Go type: %T", asn1_type_arg(t)));

    if (params.time_type != 0 && tag != ASN1_TAG_UTC_TIME)
        return asn1_err_time_type;
    if (params.string_type != 0 && tag != ASN1_TAG_PRINTABLE_STRING)
        return asn1_err_string_type;

    switch (tag) {
    case ASN1_TAG_PRINTABLE_STRING:
        if (params.string_type == 0) {
            /* A string with anything PrintableString cannot hold goes out as
             * a UTF8String instead. */
            Str s = *(const Str *)v;
            const Byte *p = s.p;
            for (Int i = 0; i < s.len; i++) {
                if (p[i] >= UTF8_RUNE_SELF || !asn1_is_printable(p[i], false, false)) {
                    if (!utf8_valid_string(s))
                        return asn1_err_marshal_utf8;
                    tag = ASN1_TAG_UTF8_STRING;
                    break;
                }
            }
        } else {
            tag = params.string_type;
        }
        break;
    case ASN1_TAG_UTC_TIME:
        if (params.time_type == ASN1_TAG_GENERALIZED_TIME ||
            asn1_outside_utc_range(*(const Time *)v))
            tag = ASN1_TAG_GENERALIZED_TIME;
        break;
    default:
        break;
    }

    if (params.set) {
        if (tag != ASN1_TAG_SEQUENCE)
            return asn1_err_set;
        tag = ASN1_TAG_SET;
    }

    /* A slice type named for a SET is sorted like one. */
    if (tag == ASN1_TAG_SET && !params.set)
        params.set = true;

    Int cls = ASN1_CLASS_UNIVERSAL;
    Int outer = -1;
    if (params.has_tag) {
        if (params.application)
            cls = ASN1_CLASS_APPLICATION;
        else if (params.is_private)
            cls = ASN1_CLASS_PRIVATE;
        else
            cls = ASN1_CLASS_CONTEXT_SPECIFIC;
        if (params.is_explicit) {
            asn1_enc_tag(e, cls, params.tag, true);
            outer = asn1_enc_open(e);
            cls = ASN1_CLASS_UNIVERSAL;
        } else {
            tag = params.tag;
        }
    }

    asn1_enc_tag(e, cls, tag, compound);
    Int pos = asn1_enc_open(e);
    Error err = asn1_make_body(e, t, v, &params, depth);
    if (BURROW_FAILED(err))
        return err;
    asn1_enc_close(e, pos);
    if (outer >= 0)
        asn1_enc_close(e, outer);
    return BURROW_NO_ERROR;
}

/* The bytes written, trimmed to fit, or the nil slice after an error. */
static Slice asn1_enc_finish(Asn1Enc *e, Error *err) {
    if (BURROW_OK(*err) && e->oom)
        *err = burrow_err_out_of_memory;
    if (BURROW_FAILED(*err) || e->len == 0) {
        if (e->p != NULL)
            mem_free(e->h, e->p, (size_t)e->cap, 1);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        return slice_make(e->h, TYPE_BYTE, 0, 0);
    }
    if (e->len < e->cap) {
        Byte *np = (Byte *)mem_realloc(e->h, e->p, (size_t)e->cap, (size_t)e->len, 1);
        if (np != NULL) {
            e->p = np;
            e->cap = e->len;
        }
    }
    return (Slice){e->p, e->len, e->cap, TYPE_BYTE};
}

Slice asn1_marshal(Alloc *a, Any v, Error *err) {
    return asn1_marshal_with_params(a, v, BURROW_STR_EMPTY, err);
}

Slice asn1_marshal_with_params(Alloc *a, Any v, Str params, Error *err) {
    Asn1Enc e = {a, NULL, 0, 0, false, 0};
    *err = asn1_make_field(&e, v.t, v.data, burrow__asn1_parse_field_parameters(params),
                           0);
    return asn1_enc_finish(&e, err);
}

Slice burrow__asn1_make_big_int(Alloc *a, const BigInt *n, Error *err) {
    Asn1Enc e = {a, NULL, 0, 0, false, 0};
    *err = asn1_enc_big_int(&e, n);
    return asn1_enc_finish(&e, err);
}
