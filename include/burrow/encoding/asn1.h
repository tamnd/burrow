/* encoding/asn1, DER encoded ASN.1 to and from C values.
 *
 * ASN.1 describes data, and DER is the one encoding of it that is unique for
 * every value, which is why X.509 certificates and the keys inside them use
 * it. This package reads and writes DER the way encoding/json reads and
 * writes JSON: it walks a value's type descriptor, so a struct declared with
 * BURROW_STRUCT is a SEQUENCE, its fields are the elements in order, and the
 * asn1 struct tag on a field says how that one is tagged.
 *
 *     #define ALGO_FIELDS(F, T)                       \
 *         F(T, Asn1ObjectIdentifier, Algorithm, "")   \
 *         F(T, Int, Version, "asn1:\"optional,explicit,tag:0\"")
 *     BURROW_STRUCT(Algo, ALGO_FIELDS);
 *
 *     Algo in = {ASN1_OID(1, 2, 840, 113549, 1, 1, 11), 2};
 *     Slice der = asn1_marshal(a, BURROW_ANY(TYPE_OF(Algo), &in), &err);
 *
 *     Algo out = {0};
 *     Slice rest = asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(Algo), &out), &err);
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/asn1 */

#ifndef BURROW_ENCODING_ASN1_H
#define BURROW_ENCODING_ASN1_H

/* What maps to what, both ways:
 *
 *     BOOLEAN                  bool
 *     INTEGER                  Int, int32_t, int64_t, and a pointer to BigInt
 *     ENUMERATED               Asn1Enumerated
 *     BIT STRING               Asn1BitString
 *     OCTET STRING             a slice of uint8_t, such as Bytes
 *     OBJECT IDENTIFIER        Asn1ObjectIdentifier
 *     UTCTime, GeneralizedTime Time
 *     the string types         Str
 *     SEQUENCE                 a struct
 *     SEQUENCE OF, SET OF      any other slice
 *     anything at all          Asn1RawValue, which keeps it undecoded
 *
 * A field of type Any takes whichever of the simple ones it finds, and an
 * INTEGER goes in it as an int64_t. Marshalling also takes int8_t and int16_t.
 *
 * A pointer to BigInt is any pointer type whose element is BigInt, so
 * BURROW_PTR_TYPE(BigIntPtr, BigInt) declares one to use in a field list.
 *
 * The tag on a field is a comma separated list of these:
 *
 *     optional     the element may be missing
 *     default:x    the value of a missing optional integer, and the value that
 *                  is left out when marshalling
 *     explicit     an extra tag wraps the element, rather than replacing its
 *                  own
 *     tag:x        the tag number, context specific unless one of the next two
 *                  says otherwise
 *     application  the tag is APPLICATION
 *     private      the tag is PRIVATE
 *     set          a SET rather than a SEQUENCE
 *     omitempty    an empty slice is left out when marshalling
 *     ia5, printable, numeric, utf8
 *                  the string type to write, and to expect under an implicit
 *                  tag, where a Str is PrintableString unless the tag says
 *     utc, generalized
 *                  the time type, in the same way
 *
 * A slice type whose name ends in SET is a SET OF without the tag, which is
 * the only way to say so for a slice inside a slice. A type has a name when it
 * is declared with BURROW_NAMED_SLICE_TYPE. If the first field of a struct is
 * an Asn1RawContent, unmarshalling stores the struct's own DER there, and
 * marshalling writes that DER instead of the fields when it is not empty. */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The universal tag numbers this package knows. */
enum {
    ASN1_TAG_BOOLEAN = 1,
    ASN1_TAG_INTEGER = 2,
    ASN1_TAG_BIT_STRING = 3,
    ASN1_TAG_OCTET_STRING = 4,
    ASN1_TAG_NULL = 5,
    ASN1_TAG_OID = 6,
    ASN1_TAG_ENUM = 10,
    ASN1_TAG_UTF8_STRING = 12,
    ASN1_TAG_SEQUENCE = 16,
    ASN1_TAG_SET = 17,
    ASN1_TAG_NUMERIC_STRING = 18,
    ASN1_TAG_PRINTABLE_STRING = 19,
    ASN1_TAG_T61_STRING = 20,
    ASN1_TAG_IA5_STRING = 22,
    ASN1_TAG_UTC_TIME = 23,
    ASN1_TAG_GENERALIZED_TIME = 24,
    ASN1_TAG_GENERAL_STRING = 27,
    ASN1_TAG_BMP_STRING = 30,
};

/* The tag classes. */
enum {
    ASN1_CLASS_UNIVERSAL = 0,
    ASN1_CLASS_APPLICATION = 1,
    ASN1_CLASS_CONTEXT_SPECIFIC = 2,
    ASN1_CLASS_PRIVATE = 3,
};

/* ------------------------------------------------------------------ errors */

/* asn1.StructuralError: the DER is valid and the C type it is going into does
 * not fit it. The message is "asn1: structure error: " and then msg. errors_is
 * matches two with the same msg, and errors_as with
 * TYPE_ASN1_STRUCTURAL_ERROR gives a pointer to one. */
typedef struct Asn1StructuralError {
    Str msg;
} Asn1StructuralError;

extern const Type *const TYPE_ASN1_STRUCTURAL_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str asn1_structural_error_error(Asn1StructuralError e, Alloc *a);

/* An Error for e that owns a copy of msg, from a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error asn1_structural_error_as_error(Asn1StructuralError e, Alloc *a);

/* asn1.SyntaxError: the DER itself is broken. The message is "asn1: syntax
 * error: " and then msg, and the rest is as for Asn1StructuralError. */
typedef struct Asn1SyntaxError {
    Str msg;
} Asn1SyntaxError;

extern const Type *const TYPE_ASN1_SYNTAX_ERROR;

BURROW_OWNS(ret) Str asn1_syntax_error_error(Asn1SyntaxError e, Alloc *a);
BURROW_OWNS(ret) Error asn1_syntax_error_as_error(Asn1SyntaxError e, Alloc *a);

/* ------------------------------------------------------------------- types */

/* asn1.BitString: bits packed into bytes from the top bit down, and how many
 * of them there are. The bits past bit_length in the last byte are zero. */
typedef struct Asn1BitString {
    Slice bytes;
    Int bit_length;
} Asn1BitString;

extern const Type burrow_type_Asn1BitString;
#define TYPE_ASN1_BIT_STRING TYPE_OF(Asn1BitString)

/* BitString.At: bit i, counting from the top bit of the first byte, or 0 for
 * an i out of range. */
Int asn1_bit_string_at(Asn1BitString b, Int i);

/* BitString.RightAlign: the bits with the padding at the start rather than at
 * the end. When there is no padding this is b.bytes itself, and otherwise a
 * copy from a, which is the nil slice if a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
asn1_bit_string_right_align(Asn1BitString b, Alloc *a);

/* asn1.ObjectIdentifier: the arcs of an OID, a slice of Int. ASN1_OID makes
 * one from a list of numbers, in storage that lasts as long as the enclosing
 * block, or forever at file scope. */
typedef Slice Asn1ObjectIdentifier;

extern const Type burrow_type_Asn1ObjectIdentifier;
#define TYPE_ASN1_OBJECT_IDENTIFIER TYPE_OF(Asn1ObjectIdentifier)

#define ASN1_OID(...)                                                                  \
    ((Asn1ObjectIdentifier){                                                           \
        (Int[]){__VA_ARGS__}, (Int)(sizeof((Int[]){__VA_ARGS__}) / sizeof(Int)),       \
        (Int)(sizeof((Int[]){__VA_ARGS__}) / sizeof(Int)), TYPE_INT})

/* ObjectIdentifier.Equal: the same arcs in the same order. */
bool asn1_object_identifier_equal(Asn1ObjectIdentifier oi, Asn1ObjectIdentifier other);

/* ObjectIdentifier.String: the arcs in decimal with dots between, such as
 * "1.2.840.113549", from a. */
BURROW_OWNS(ret) Str asn1_object_identifier_string(Asn1ObjectIdentifier oi, Alloc *a);

/* asn1.Enumerated: an ENUMERATED, which is an Int on this side. */
typedef Int Asn1Enumerated;

extern const Type burrow_type_Asn1Enumerated;
#define TYPE_ASN1_ENUMERATED TYPE_OF(Asn1Enumerated)

/* asn1.Flag: true when the element is there at all, whatever it holds. Use it
 * with optional and a tag. */
typedef bool Asn1Flag;

extern const Type burrow_type_Asn1Flag;
#define TYPE_ASN1_FLAG TYPE_OF(Asn1Flag)

/* asn1.RawContent: the DER of the struct it is the first field of. */
typedef Slice Asn1RawContent;

extern const Type burrow_type_Asn1RawContent;
#define TYPE_ASN1_RAW_CONTENT TYPE_OF(Asn1RawContent)

/* asn1.RawValue: an element left undecoded. cls is Go's Class, renamed since
 * class is a keyword in C++. bytes is the contents and full_bytes the whole
 * element with its tag and length. Marshalling writes full_bytes when it is
 * not empty, and otherwise builds the tag and length from the other fields. */
typedef struct Asn1RawValue {
    Int cls;
    Int tag;
    bool is_compound;
    Slice bytes;
    Slice full_bytes;
} Asn1RawValue;

extern const Type burrow_type_Asn1RawValue;
#define TYPE_ASN1_RAW_VALUE TYPE_OF(Asn1RawValue)

/* asn1.NullRawValue, a RawValue with the NULL tag, and asn1.NullBytes, the
 * DER for NULL. */
extern const Asn1RawValue asn1_null_raw_value;
extern const Slice asn1_null_bytes;

/* --------------------------------------------------------------- functions */

/* asn1.Marshal: the DER for the value v points at, from a. On an error the
 * result is the nil slice. The errors are an Asn1StructuralError for a value
 * that cannot be written, such as an OID with fewer than two arcs or a string
 * with a character its string type does not allow, and plain errors for an
 * Any with nothing in it and a Str that is not UTF-8. They live in the error
 * arena. */
BURROW_OWNS(ret) Slice asn1_marshal(Alloc *a, Any v, Error *err);

/* asn1.MarshalWithParams: asn1_marshal with params, in the struct tag syntax,
 * for the top level element. */
BURROW_OWNS(ret) Slice asn1_marshal_with_params(Alloc *a, Any v, Str params,
                                                Error *err);

/* asn1.Unmarshal: reads one element from the start of b into the value v
 * points at, and returns what is left of b after it. A SEQUENCE may have more
 * elements than the struct has fields, and those count as part of it rather
 * than as what is left.
 *
 * Strings, OIDs, slices, octet strings and big integers are made from a. An
 * Asn1RawValue, an Asn1BitString and an Asn1RawContent point into b, as an
 * octet string in an Any does, so b has to outlive them. Use an arena for a,
 * since an error part way through leaves what was made so far in it and in
 * the half filled value.
 *
 * An Any with no type is "asn1: Unmarshal recipient value is nil", and one
 * with a type and no data is the same with the pointer type after it. Any is
 * a pointer already, so Go's error for a value that is not a pointer cannot
 * happen. Otherwise the error is an Asn1SyntaxError, an Asn1StructuralError,
 * or a plain error for broken text in a UTF8String, BMPString or time, all in
 * the error arena. On an error the result is the nil slice. */
BURROW_OWNS(v) BURROW_BORROWS(ret, b) Slice asn1_unmarshal(Alloc *a, Slice b, Any v,
                                                           Error *err);

/* asn1.UnmarshalWithParams: asn1_unmarshal with params for the top level
 * element. */
BURROW_OWNS(v) BURROW_BORROWS(ret, b) Slice asn1_unmarshal_with_params(Alloc *a,
                                                                       Slice b, Any v,
                                                                       Str params,
                                                                       Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_ASN1_H */
