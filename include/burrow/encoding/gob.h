/* encoding/gob: Go's self-describing binary stream of values.
 *
 * An encoder writes a stream of values, each preceded the first time its type
 * appears by a description of that type. A decoder reads the descriptions and
 * matches what it receives against the value it is asked to fill in, by field
 * name, so the two ends do not need identical types. The bytes are Go's, the
 * same ones Go's encoding/gob writes for the same values, and a burrow program
 * and a Go program can talk to each other over one.
 *
 * Writing a value and reading it back:
 *
 *     typedef struct { Int X; Int Y; Str Name; } Point;    (with a descriptor)
 *
 *     BytesBuffer buf = BYTES_BUFFER(a);
 *     GobEncoder *enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
 *     Point p = {3, 4, BURROW_S("corner")};
 *     Error err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Point), &p));
 *     gob_encoder_free(enc);
 *
 *     GobDecoder *dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&buf));
 *     Point q = {0};
 *     err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Point), &q));
 *     gob_decoder_free(dec);
 *
 * A value to encode is an Any holding the value, and a value to decode into is
 * an Any pointing at where it goes, which is Go's pointer argument to Decode.
 * Everything the decoder allocates, strings, slices, maps and pointers, comes
 * from the allocator the decoder was made with, so an arena is the natural one.
 *
 * Types are worked out from their descriptors, so a struct needs one, from
 * BURROW_STRUCT or burrow-gen. Only exported fields travel, that is fields
 * whose names start with an upper case letter, and fields of channel or
 * function type are left out, as in Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/gob */

/* A value held in an interface field travels with the name its concrete type
 * was registered under, so both ends have to call gob_register for it first.
 * The decoding end can only put it in an Any field. C has no way to make up a
 * vtable for any other interface, so a value arriving for one is an error.
 *
 * Go's stack grows to fit and a C stack does not, so the encoder and the
 * decoder stop with "nesting too deep" when the next level would leave less
 * than 64 KB of stack, where Go would carry on. Decoding takes about 1.8 KB
 * a level on arm64 at -O2, so a goroutine's default 256 KB stack holds about
 * a hundred levels, and go_stack buys more. That only matters for recursive
 * types, a long linked list for example, and a program that sends those
 * should send a slice instead. */

#ifndef BURROW_ENCODING_GOB_H
#define BURROW_ENCODING_GOB_H

#include "burrow/core.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gob.CommonType, the part of every type description that says what the type
 * is called and which number it goes by in the stream. It is exported because
 * Go exports it, so that a program can decode the description messages
 * themselves, and it is a struct with a descriptor for the same reason. */
typedef struct GobCommonType {
    Str name;
    int32_t id;
} GobCommonType;

extern const Type burrow_type_GobCommonType;

/* ----------------------------------------------------------------- methods
 *
 * gob.GobEncoder and gob.GobDecoder: a type that wants to put its own bytes on
 * the wire lists GobEncode and GobDecode among its methods, with these shapes:
 *
 *     Slice gob_encode(T *self, Alloc *a, Error *err);
 *     Error gob_decode(T *self, Alloc *a, Slice data);
 *
 *     #define STAMP_METHODS(M, T)                                \
 *         M(T, GobDecode, stamp_gob_decode, GOB_SIG_GOB_DECODE)  \
 *         M(T, GobEncode, stamp_gob_encode, GOB_SIG_GOB_ENCODE)
 *
 * A type with MarshalBinary and UnmarshalBinary from encoding is sent the same
 * way when it has no GobEncode, which is Go's rule too. The methods are looked
 * for on the type itself, whether the value is a T or a pointer to one. */
#define GOB_SIG_GOB_ENCODE(IN, OUT) ENCODING_SIG_MARSHAL_BINARY(IN, OUT)
#define GOB_SIG_GOB_DECODE(IN, OUT) ENCODING_SIG_UNMARSHAL_BINARY(IN, OUT)

/* The two interfaces, for code that wants to hold one. */
typedef struct GobGobEncoderVT {
    const Type *self_type;
    Slice (*gob_encode)(void *self, Alloc *a, Error *err);
} GobGobEncoderVT;

typedef struct GobGobEncoder {
    const GobGobEncoderVT *vt;
    void *data;
} GobGobEncoder;

typedef struct GobGobDecoderVT {
    const Type *self_type;
    Error (*gob_decode)(void *self, Alloc *a, Slice data);
} GobGobDecoderVT;

typedef struct GobGobDecoder {
    const GobGobDecoderVT *vt;
    void *data;
} GobGobDecoder;

extern const Type burrow_type_GobGobEncoder;
extern const Type burrow_type_GobGobDecoder;

/* ------------------------------------------------------------ registration
 *
 * gob.Register and gob.RegisterName. A value sent in an interface is sent with
 * a name, and the receiving end turns the name back into a type through this
 * table, so every concrete type that travels inside an interface has to be in
 * it on both ends. Only v.t is read; v.data may be NULL.
 *
 * gob_register picks the name the way Go does: the package path and the type
 * name for a named type, main.Point for example, and the type written out for
 * an unnamed one, such as []int or *main.Point.
 *
 * Registering one type under two names, or two types under one name, is a
 * mistake in the program and panics, as it does in Go. The basic types and
 * slices of them are registered already. */
void gob_register(Any v);
void gob_register_name(Str name, Any v);

/* ------------------------------------------------------------------ encoder */

/* gob.Encoder. It remembers which types it has described, so the stream it
 * writes has to be read by one decoder from the start. Safe to use from more
 * than one thread; each value goes out whole. */
typedef struct GobEncoder GobEncoder;

/* NULL when a is out of memory. Everything the encoder needs comes from a. */
BURROW_OWNS(ret) GobEncoder *gob_new_encoder(Alloc *a, IoWriter w);
void gob_encoder_free(GobEncoder *e);

/* Encoder.Encode. Writes v, and the description of its type the first time
 * the type is seen. An Any holding an Any is looked through, as Go's Encode
 * looks through its interface argument. A nil pointer panics, as in Go, and
 * an Any with no type is "gob: cannot encode nil value". */
BURROW_BORROWS(ret) Error gob_encoder_encode(GobEncoder *e, Any v);

/* Encoder.EncodeValue. The same, except that an Any holding an Any is sent as
 * an interface value, which is what reflect.Value of an interface is in Go. */
BURROW_BORROWS(ret) Error gob_encoder_encode_value(GobEncoder *e, Any v);

/* ------------------------------------------------------------------ decoder */

/* gob.Decoder. Like Go's, it reads through a bufio.Reader of its own unless r
 * can already read a byte at a time, so it may read past the last value it
 * returns. */
typedef struct GobDecoder GobDecoder;

BURROW_OWNS(ret) GobDecoder *gob_new_decoder(Alloc *a, IoReader r);
void gob_decoder_free(GobDecoder *d);

/* Decoder.Decode. Reads the next value from the stream into the value v
 * points at, allocating from the decoder's allocator. v.t is the type of what
 * v.data points at, so BURROW_ANY(TYPE_OF(Point), &p) reads into p.
 *
 * An Any with no type reads the value and throws it away. One with a type and
 * no data is Go's nil pointer, "gob: DecodeValue of unassignable value". At the
 * end of the stream the error is io_eof, and a stream that stops in the middle
 * of a value is io_err_unexpected_eof. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error gob_decoder_decode(GobDecoder *d, Any v);

/* Decoder.DecodeValue, which in C is the same call. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error gob_decoder_decode_value(GobDecoder *d, Any v);

/* Decoder.Decode, with the strings, slices and maps that go into v made in a
 * rather than in the decoder's allocator. Not a Go function. A stream can go
 * on for as long as a connection does, and a caller that gives back what each
 * value holds once it is done with it, as net/rpc does after every request,
 * needs those to come from somewhere other than an allocator that lasts as
 * long as the decoder. What the decoder keeps for itself, such as the types it
 * has been sent, still comes from the decoder's allocator. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error gob_decoder_decode_in(GobDecoder *d, Alloc *a,
                                                              Any v);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_GOB_H */
