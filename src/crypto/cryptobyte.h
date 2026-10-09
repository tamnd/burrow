/* Parsing and building length prefixed and DER encoded byte strings, the way
 * the crypto packages read and write signatures, certificates and handshake
 * messages. Go vendors this from golang.org/x/crypto/cryptobyte into the
 * standard library, where nothing outside it can import it, so here it is a
 * private header.
 *
 * A CryptobyteString is a Slice of bytes that the read functions take a
 * pointer to. Each one reads a value off the front, moves the slice past it
 * and returns true, or returns false and leaves the slice alone when what is
 * there is not a valid value. What they read out shares the bytes of the
 * string.
 *
 * A CryptobyteBuilder appends values to a byte slice. A length prefixed value
 * is built by a continuation, a function the builder calls with a child
 * builder for the contents, and the builder writes the length once the
 * function returns. The first error stops the building and comes back from
 * cryptobyte_builder_bytes. A zero CryptobyteBuilder is ready to use and grows
 * on the heap.
 *
 * Go's ReadASN1Integer takes a pointer to any integer type, a *big.Int or a
 * *[]byte and decides at run time which it is. Here there is one function for
 * each: Int, int64_t, uint64_t, BigInt and bytes. Go recovers a BuildError
 * that a continuation panics with and turns it into the builder's error. C has
 * no recover to do that with, so a continuation that fails calls
 * cryptobyte_builder_set_error on the child it was given, which passes it up.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_CRYPTOBYTE_H
#define BURROW_CRYPTO_CRYPTOBYTE_H

/* crypto.h comes first so that the amalgamation files this header with
 * package crypto, which the packages that use it all import. */
#include "burrow/crypto.h"

#include "burrow/core.h"
#include "burrow/encoding/asn1.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------- asn1 tags */

/* asn1.Tag: the identifier octet of a DER element, low tag numbers only. */
typedef uint8_t CryptobyteAsn1Tag;

#define CRYPTOBYTE_ASN1_BOOLEAN ((CryptobyteAsn1Tag)1)
#define CRYPTOBYTE_ASN1_INTEGER ((CryptobyteAsn1Tag)2)
#define CRYPTOBYTE_ASN1_BIT_STRING ((CryptobyteAsn1Tag)3)
#define CRYPTOBYTE_ASN1_OCTET_STRING ((CryptobyteAsn1Tag)4)
#define CRYPTOBYTE_ASN1_NULL ((CryptobyteAsn1Tag)5)
#define CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER ((CryptobyteAsn1Tag)6)
#define CRYPTOBYTE_ASN1_ENUM ((CryptobyteAsn1Tag)10)
#define CRYPTOBYTE_ASN1_UTF8_STRING ((CryptobyteAsn1Tag)12)
#define CRYPTOBYTE_ASN1_SEQUENCE ((CryptobyteAsn1Tag)(16 | 0x20))
#define CRYPTOBYTE_ASN1_SET ((CryptobyteAsn1Tag)(17 | 0x20))
#define CRYPTOBYTE_ASN1_PRINTABLE_STRING ((CryptobyteAsn1Tag)19)
#define CRYPTOBYTE_ASN1_T61_STRING ((CryptobyteAsn1Tag)20)
#define CRYPTOBYTE_ASN1_IA5_STRING ((CryptobyteAsn1Tag)22)
#define CRYPTOBYTE_ASN1_UTC_TIME ((CryptobyteAsn1Tag)23)
#define CRYPTOBYTE_ASN1_GENERALIZED_TIME ((CryptobyteAsn1Tag)24)
#define CRYPTOBYTE_ASN1_GENERAL_STRING ((CryptobyteAsn1Tag)27)

/* Tag.Constructed and Tag.ContextSpecific. */
static inline CryptobyteAsn1Tag cryptobyte_asn1_tag_constructed(CryptobyteAsn1Tag t) {
    return (CryptobyteAsn1Tag)(t | 0x20);
}

static inline CryptobyteAsn1Tag
cryptobyte_asn1_tag_context_specific(CryptobyteAsn1Tag t) {
    return (CryptobyteAsn1Tag)(t | 0x80);
}

/* ------------------------------------------------------------------ String */

/* cryptobyte.String: the bytes still to be read, a Slice of bytes. */
typedef Slice CryptobyteString;

/* Skip: drop n bytes. */
bool cryptobyte_string_skip(CryptobyteString *s, Int n);

/* ReadUint8 to ReadUint64: a big endian number of 1, 2, 3, 4, 6 or 8 bytes. */
bool cryptobyte_string_read_uint8(CryptobyteString *s, uint8_t *out);
bool cryptobyte_string_read_uint16(CryptobyteString *s, uint16_t *out);
bool cryptobyte_string_read_uint24(CryptobyteString *s, uint32_t *out);
bool cryptobyte_string_read_uint32(CryptobyteString *s, uint32_t *out);
bool cryptobyte_string_read_uint48(CryptobyteString *s, uint64_t *out);
bool cryptobyte_string_read_uint64(CryptobyteString *s, uint64_t *out);

/* ReadUint8LengthPrefixed to ReadUint24LengthPrefixed: the bytes after a big
 * endian length of 1, 2 or 3 bytes. */
bool cryptobyte_string_read_uint8_length_prefixed(CryptobyteString *s,
                                                  CryptobyteString *out);
bool cryptobyte_string_read_uint16_length_prefixed(CryptobyteString *s,
                                                   CryptobyteString *out);
bool cryptobyte_string_read_uint24_length_prefixed(CryptobyteString *s,
                                                   CryptobyteString *out);

/* ReadBytes: the next n bytes. */
bool cryptobyte_string_read_bytes(CryptobyteString *s, Slice *out, Int n);

/* CopyBytes: the next out.len bytes, copied into out. */
bool cryptobyte_string_copy_bytes(CryptobyteString *s, Slice out);

/* Empty: whether there is nothing left. */
bool cryptobyte_string_empty(CryptobyteString s);

/* ReadASN1Boolean: a BOOLEAN, which DER has as 0 or 0xff. */
bool cryptobyte_string_read_asn1_boolean(CryptobyteString *s, bool *out);

/* ReadASN1Integer: an INTEGER, minimally encoded, that fits in the type.
 * The bytes version takes only zero and positive values, without the leading
 * zero bytes, and zero as one zero byte. */
bool cryptobyte_string_read_asn1_integer_int(CryptobyteString *s, Int *out);
bool cryptobyte_string_read_asn1_integer_int64(CryptobyteString *s, int64_t *out);
bool cryptobyte_string_read_asn1_integer_uint64(CryptobyteString *s, uint64_t *out);
bool cryptobyte_string_read_asn1_integer_big(CryptobyteString *s, BigInt *out);
bool cryptobyte_string_read_asn1_integer_bytes(CryptobyteString *s, Slice *out);

/* ReadASN1Int64WithTag: an INTEGER under another tag. */
bool cryptobyte_string_read_asn1_int64_with_tag(CryptobyteString *s, int64_t *out,
                                                CryptobyteAsn1Tag tag);

/* ReadASN1Enum: an ENUMERATED. */
bool cryptobyte_string_read_asn1_enum(CryptobyteString *s, Int *out);

/* ReadASN1ObjectIdentifier: an OBJECT IDENTIFIER, its arcs from a. Each arc
 * must fit in 31 bits. */
bool cryptobyte_string_read_asn1_object_identifier(CryptobyteString *s, Alloc *a,
                                                   Asn1ObjectIdentifier *out);

/* ReadASN1GeneralizedTime and ReadASN1UTCTime: the times, with any zone they
 * give from a. A UTCTime of 50 to 99 is 1950 to 1999. */
bool cryptobyte_string_read_asn1_generalized_time(CryptobyteString *s, Alloc *a,
                                                  Time *out);
bool cryptobyte_string_read_asn1_utc_time(CryptobyteString *s, Alloc *a, Time *out);

/* ReadASN1BitString: a BIT STRING. ReadASN1BitStringAsBytes: one that is a
 * whole number of bytes, as the bytes. */
bool cryptobyte_string_read_asn1_bit_string(CryptobyteString *s, Asn1BitString *out);
bool cryptobyte_string_read_asn1_bit_string_as_bytes(CryptobyteString *s, Slice *out);

/* ReadASN1Bytes and ReadASN1: the contents of an element with tag, without
 * its tag and length. ReadASN1Element: the whole element. */
bool cryptobyte_string_read_asn1_bytes(CryptobyteString *s, Slice *out,
                                       CryptobyteAsn1Tag tag);
bool cryptobyte_string_read_asn1(CryptobyteString *s, CryptobyteString *out,
                                 CryptobyteAsn1Tag tag);
bool cryptobyte_string_read_asn1_element(CryptobyteString *s, CryptobyteString *out,
                                         CryptobyteAsn1Tag tag);

/* ReadAnyASN1 and ReadAnyASN1Element: the same with any tag, which goes in
 * out_tag unless it is NULL. */
bool cryptobyte_string_read_any_asn1(CryptobyteString *s, CryptobyteString *out,
                                     CryptobyteAsn1Tag *out_tag);
bool cryptobyte_string_read_any_asn1_element(CryptobyteString *s, CryptobyteString *out,
                                             CryptobyteAsn1Tag *out_tag);

/* PeekASN1Tag: whether the next element has tag. */
bool cryptobyte_string_peek_asn1_tag(CryptobyteString s, CryptobyteAsn1Tag tag);

/* SkipASN1: drop an element with tag. */
bool cryptobyte_string_skip_asn1(CryptobyteString *s, CryptobyteAsn1Tag tag);

/* ReadOptionalASN1: the contents of an element with tag if the next one has
 * it. Whether it did goes in out_present unless that is NULL, and out may be
 * NULL too. */
bool cryptobyte_string_read_optional_asn1(CryptobyteString *s, CryptobyteString *out,
                                          bool *out_present, CryptobyteAsn1Tag tag);

/* SkipOptionalASN1: drop the next element if it has tag. */
bool cryptobyte_string_skip_optional_asn1(CryptobyteString *s, CryptobyteAsn1Tag tag);

/* ReadOptionalASN1Integer: an INTEGER explicitly tagged with tag, or def when
 * the next element does not have the tag. */
bool cryptobyte_string_read_optional_asn1_integer_int(CryptobyteString *s, Int *out,
                                                      CryptobyteAsn1Tag tag, Int def);
bool cryptobyte_string_read_optional_asn1_integer_int64(CryptobyteString *s,
                                                        int64_t *out,
                                                        CryptobyteAsn1Tag tag,
                                                        int64_t def);
bool cryptobyte_string_read_optional_asn1_integer_uint64(CryptobyteString *s,
                                                         uint64_t *out,
                                                         CryptobyteAsn1Tag tag,
                                                         uint64_t def);
bool cryptobyte_string_read_optional_asn1_integer_big(CryptobyteString *s, BigInt *out,
                                                      CryptobyteAsn1Tag tag,
                                                      const BigInt *def);
bool cryptobyte_string_read_optional_asn1_integer_bytes(CryptobyteString *s, Slice *out,
                                                        CryptobyteAsn1Tag tag,
                                                        Slice def);

/* ReadOptionalASN1OctetString: an OCTET STRING explicitly tagged with tag, or
 * the nil slice when the next element does not have the tag. */
bool cryptobyte_string_read_optional_asn1_octet_string(CryptobyteString *s, Slice *out,
                                                       bool *out_present,
                                                       CryptobyteAsn1Tag tag);

/* ReadOptionalASN1Boolean: a BOOLEAN explicitly tagged with tag, or def. */
bool cryptobyte_string_read_optional_asn1_boolean(CryptobyteString *s, bool *out,
                                                  CryptobyteAsn1Tag tag, bool def);

/* ----------------------------------------------------------------- Builder */

typedef struct CryptobyteBuilder CryptobyteBuilder;

/* BuilderContinuation: what writes the contents of a length prefixed value
 * into child. The child is only good until the function returns. */
BURROW_FUNC(CryptobyteBuilderContinuation, void, CryptobyteBuilder *child);

/* MarshalingValue: something that writes itself into b, or fails. */
BURROW_FUNC(CryptobyteMarshalingValue, Error, CryptobyteBuilder *b);

struct CryptobyteBuilder {
    Error err;
    Slice result; /* bytes, what has been written is from offset on */
    Alloc *a;     /* where result grows, NULL for the heap */
    bool owned;   /* result came from a, and goes back when it grows */
    bool fixed_size;
    CryptobyteBuilder *child;
    Int offset;
    Int pending_len_len;
    bool pending_is_asn1;
};

/* NewBuilder: a builder that appends to buffer, growing from a. */
CryptobyteBuilder cryptobyte_new_builder(Alloc *a, Slice buffer);

/* NewFixedBuilder: a builder that appends to buffer and never grows it. Going
 * past its cap is an error. */
CryptobyteBuilder cryptobyte_new_fixed_builder(Slice buffer);

/* The storage b grew, back to its allocator. A builder that never grew, or
 * grew from an arena, need not be freed. */
void cryptobyte_builder_free(CryptobyteBuilder *b);

/* SetError: err comes back from Bytes, and the writes after it do nothing. */
void cryptobyte_builder_set_error(CryptobyteBuilder *b, Error err);

/* Bytes: what has been written, which is b's storage, or the nil slice and
 * the error. BytesOrPanic: the same, panicking with the error. */
BURROW_BORROWS(ret, b) Slice cryptobyte_builder_bytes(CryptobyteBuilder *b, Error *err);
BURROW_BORROWS(ret, b) Slice cryptobyte_builder_bytes_or_panic(CryptobyteBuilder *b);

/* AddUint8 to AddUint64: big endian numbers of 1, 2, 3, 4, 6 and 8 bytes. The
 * top byte of v is dropped for AddUint24, and the top two for AddUint48. */
void cryptobyte_builder_add_uint8(CryptobyteBuilder *b, uint8_t v);
void cryptobyte_builder_add_uint16(CryptobyteBuilder *b, uint16_t v);
void cryptobyte_builder_add_uint24(CryptobyteBuilder *b, uint32_t v);
void cryptobyte_builder_add_uint32(CryptobyteBuilder *b, uint32_t v);
void cryptobyte_builder_add_uint48(CryptobyteBuilder *b, uint64_t v);
void cryptobyte_builder_add_uint64(CryptobyteBuilder *b, uint64_t v);

/* AddBytes: v as it is. */
void cryptobyte_builder_add_bytes(CryptobyteBuilder *b, Slice v);

/* AddUint8LengthPrefixed to AddUint32LengthPrefixed: what f writes, after its
 * length in 1, 2, 3 or 4 bytes. */
void cryptobyte_builder_add_uint8_length_prefixed(CryptobyteBuilder *b,
                                                  CryptobyteBuilderContinuation f);
void cryptobyte_builder_add_uint16_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f);
void cryptobyte_builder_add_uint24_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f);
void cryptobyte_builder_add_uint32_length_prefixed(CryptobyteBuilder *b,
                                                   CryptobyteBuilderContinuation f);

/* Unwrite: drop the last n bytes written to b itself. */
void cryptobyte_builder_unwrite(CryptobyteBuilder *b, Int n);

/* AddValue: v writes itself, and its error becomes b's. */
void cryptobyte_builder_add_value(CryptobyteBuilder *b, CryptobyteMarshalingValue v);

/* AddASN1Int64, AddASN1Int64WithTag, AddASN1Enum, AddASN1Uint64 and
 * AddASN1BigInt: INTEGERs, and an ENUMERATED. */
void cryptobyte_builder_add_asn1_int64(CryptobyteBuilder *b, int64_t v);
void cryptobyte_builder_add_asn1_int64_with_tag(CryptobyteBuilder *b, int64_t v,
                                                CryptobyteAsn1Tag tag);
void cryptobyte_builder_add_asn1_enum(CryptobyteBuilder *b, int64_t v);
void cryptobyte_builder_add_asn1_uint64(CryptobyteBuilder *b, uint64_t v);
void cryptobyte_builder_add_asn1_big_int(CryptobyteBuilder *b, const BigInt *n);

/* AddASN1OctetString: an OCTET STRING. */
void cryptobyte_builder_add_asn1_octet_string(CryptobyteBuilder *b, Slice bytes);

/* AddASN1GeneralizedTime and AddASN1UTCTime: the times, the first for the
 * years 0 to 9999 and the second for 1950 to 2049. */
void cryptobyte_builder_add_asn1_generalized_time(CryptobyteBuilder *b, Time t);
void cryptobyte_builder_add_asn1_utc_time(CryptobyteBuilder *b, Time t);

/* AddASN1BitString: a BIT STRING of whole bytes. */
void cryptobyte_builder_add_asn1_bit_string(CryptobyteBuilder *b, Slice data);

/* AddASN1ObjectIdentifier: an OBJECT IDENTIFIER, which is an error unless it
 * has two arcs or more and they are valid. */
void cryptobyte_builder_add_asn1_object_identifier(CryptobyteBuilder *b,
                                                   Asn1ObjectIdentifier oid);

/* AddASN1Boolean and AddASN1NULL. */
void cryptobyte_builder_add_asn1_boolean(CryptobyteBuilder *b, bool v);
void cryptobyte_builder_add_asn1_null(CryptobyteBuilder *b);

/* MarshalASN1: what asn1_marshal makes of v. */
void cryptobyte_builder_marshal_asn1(CryptobyteBuilder *b, Any v);

/* AddASN1: an element with tag, whose contents f writes. A tag with the low
 * five bits all set is an error, since that is the high tag number form. */
void cryptobyte_builder_add_asn1(CryptobyteBuilder *b, CryptobyteAsn1Tag tag,
                                 CryptobyteBuilderContinuation f);

#endif /* BURROW_CRYPTO_CRYPTOBYTE_H */
