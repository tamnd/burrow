# Encoding

`burrow/encoding.h` is Go's `encoding` package: the six interfaces a type implements to say how it turns into bytes or text and back. The encoders that come later, `encoding/json` and `encoding/xml` among them, look for these on every value they are given and use them in place of their own rules, the same way Go's do.

| Go | burrow method name | Signature macro |
|---|---|---|
| `BinaryMarshaler` | `MarshalBinary` | `ENCODING_SIG_MARSHAL_BINARY` |
| `BinaryUnmarshaler` | `UnmarshalBinary` | `ENCODING_SIG_UNMARSHAL_BINARY` |
| `BinaryAppender` | `AppendBinary` | `ENCODING_SIG_APPEND_BINARY` |
| `TextMarshaler` | `MarshalText` | `ENCODING_SIG_MARSHAL_TEXT` |
| `TextUnmarshaler` | `UnmarshalText` | `ENCODING_SIG_UNMARSHAL_TEXT` |
| `TextAppender` | `AppendText` | `ENCODING_SIG_APPEND_TEXT` |

## Implementing one

A type implements an interface by listing the method in its descriptor, the way a type becomes a Stringer for [fmt](fmt.md). The signature macro spells out the shape, so the method list reads the way Go's method set does:

<!-- example: ../examples/encoding/interfaces.c#declare -->
```c
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT_DECL(Point, POINT_FIELDS);

static Slice point_marshal_text(Point *p, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "%d,%d", p->X, p->Y);
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Error point_unmarshal_text(Point *p, Alloc *a, Slice text) {
    (void)a;
    Str y;
    bool found;
    Str x = strings_cut(str_from_bytes(text.p, text.len), BURROW_S(","), &y, &found);
    if (!found)
        return errors_new(error_allocator(), BURROW_S("point: want x,y"));
    Error err = BURROW_NO_ERROR;
    p->X = strconv_atoi(x, &err);
    if (!BURROW_FAILED(err))
        p->Y = strconv_atoi(y, &err);
    return err;
}

#define POINT_METHODS(M, T)                                                            \
    M(T, MarshalText, point_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                   \
    M(T, UnmarshalText, point_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);
```

The methods take an allocator where Go's lean on the collector. The bytes a marshal method returns belong to the caller, and an unmarshal method copies anything it keeps out of the input, because the input is only borrowed. A method with the right name and a different signature does not count, which is Go's rule too.

## Using one

`encoding_marshal_text` and the other five calls find the method on an `Any` and call it. A pointer finds the methods of what it points at, and an interface value such as an `Error` finds the methods of the type inside it:

<!-- example: ../examples/encoding/interfaces.c#use -->
```c
Point p = {3, 4};
Any v = BURROW_ANY(TYPE_OF(Point), &p);
Error err = BURROW_NO_ERROR;
Slice text = encoding_marshal_text(a, v, &err);

Point q = {0, 0};
err = encoding_unmarshal_text(a, BURROW_ANY(TYPE_OF(Point), &q), BURROW_B("7,8"));
```

`text` is `3,4` and `q` is `{7 8}`. Go would write `v.(encoding.TextMarshaler)` and check the second result. Here that is `encoding_is_text_marshaler`, and calling the method on a value that lacks it returns the nil slice and Go's type assertion error instead of panicking:

<!-- example: ../examples/encoding/interfaces.c#missing -->
```c
Int n = 5;
Any iv = BURROW_ANY(TYPE_INT, &n);
bool ok = encoding_is_text_marshaler(iv);
encoding_marshal_text(a, iv, &err);
```

`ok` is false and the error reads `interface conversion: int is not encoding.TextMarshaler: missing method MarshalText`.

## Holding one

`EncodingTextMarshaler` and the other five are ordinary vtable interfaces, for code that wants to take one as a parameter or keep one in a struct, as described in [Interfaces](interfaces.md). An `Any` holding one of them, with `TYPE_OF(EncodingTextMarshaler)` as its type, works with the calls above, which go straight through the vtable.

## Hex

`burrow/encoding/hex.h` is Go's `encoding/hex`. Each byte becomes two lowercase digits, and decoding takes either case:

<!-- example: ../examples/encoding/hex.c#oneshot -->
```c
Str s = hex_encode_to_string(a, BURROW_B("Hello"));

Error err = BURROW_NO_ERROR;
Slice b = hex_decode_string(a, BURROW_S("48656C6C6F"), &err);
```

`s` is `48656c6c6f` and `b` is `Hello`. When `hex_encode` and `hex_decode` write into a slice you already have, `hex_encoded_len` and `hex_decoded_len` tell you how big it needs to be. The `hex_append_` forms grow the slice for you.

Bad input gives back the bytes that decoded before the problem. The error is `hex_err_length` for a lone digit at the end, or a `HexInvalidByteError` for the first byte that is not a digit:

<!-- example: ../examples/encoding/hex.c#bad -->
```c
Slice part = hex_decode_string(a, BURROW_S("4865zz"), &err);
const HexInvalidByteError *bad = errors_as(err, TYPE_HEX_INVALID_BYTE_ERROR);
```

`part` is `He`, `*bad` is `z`, and the error reads `encoding/hex: invalid byte: U+007A 'z'`. Each of the 256 possible byte errors is a single static value, so two for the same byte are `errors_is` each other the way Go's compare equal, and none of them needs `error_retain`.

`hex_dump` lays bytes out the way `hexdump -C` does:

<!-- example: ../examples/encoding/hex.c#dump -->
```c
Str d = hex_dump(a, BURROW_B("Go is an open source programming language."));
```

```text
00000000  47 6f 20 69 73 20 61 6e  20 6f 70 65 6e 20 73 6f  |Go is an open so|
00000010  75 72 63 65 20 70 72 6f  67 72 61 6d 6d 69 6e 67  |urce programming|
00000020  20 6c 61 6e 67 75 61 67  65 2e                    | language.|
```

To work on a stream, wrap it. `hex_new_encoder` gives an `IoWriter` that encodes into another one, `hex_new_decoder` gives an `IoReader` that decodes, and `hex_dumper` gives an `IoWriteCloser` that writes a dump and finishes the last line on Close:

<!-- example: ../examples/encoding/hex.c#stream -->
```c
StringsBuilder out = STRINGS_BUILDER(a);
IoWriter enc = hex_new_encoder(a, strings_builder_as_io_writer(&out));
fmt_fprintf_v(enc, "%d apples", 12);
```

`out` now holds `3132206170706c6573`. As in Go, the decoder reports a lone digit at the end as `io_err_unexpected_eof`, since a stream that stops in the middle of a byte has been cut short.

## Base64

`burrow/encoding/base64.h` is Go's `encoding/base64`. The four encodings in common use are ready made: `base64_std_encoding`, `base64_url_encoding` with `-` and `_` in place of `+` and `/`, and the raw forms of both, which leave the `=` padding off. Each call takes the encoding first, then the allocator:

<!-- example: ../examples/encoding/base64.c#oneshot -->
```c
Str s = base64_encoding_encode_to_string(base64_std_encoding, a,
                                         BURROW_B("any carnal pleas"));

Error err = BURROW_NO_ERROR;
Slice b = base64_encoding_decode_string(base64_url_encoding, a,
                                        BURROW_S("PDw_Pz8-Pg=="), &err);
```

`s` is `YW55IGNhcm5hbCBwbGVhcw==` and `b` is `<<???>>`. Decoding skips `\r` and `\n` wherever they turn up, since MIME wraps base64 into lines. As with hex, `base64_encoding_encode` and `base64_encoding_decode` work into a slice you already have, sized with `base64_encoding_encoded_len` and `base64_encoding_decoded_len`, and the `append_` forms grow one for you.

Any other alphabet or padding is a `Base64Encoding` you make. Go hands these out as pointers, but one holds no pointers of its own, so here the calls return it by value and you keep it on the stack, in a struct or in a static:

<!-- example: ../examples/encoding/base64.c#raw -->
```c
Base64Encoding raw =
    base64_encoding_with_padding(base64_url_encoding, BASE64_NO_PADDING);
Slice c = base64_encoding_decode_string(&raw, a, BURROW_S("PDw_Pz8-Pg"), &err);
```

`base64_new_encoding` takes a 64 byte alphabet and `base64_encoding_strict` gives a copy that rejects input whose unused bits at the end are not zero. A bad alphabet or padding character panics with Go's message, as it does in Go.

Bad input gives back what decoded before it, and a `Base64CorruptInputError` holding the offset of the byte that was wrong:

<!-- example: ../examples/encoding/base64.c#bad -->
```c
Slice part = base64_encoding_decode_string(base64_std_encoding, a,
                                           BURROW_S("aGVsbG8*"), &err);
const Base64CorruptInputError *off =
    errors_as(err, TYPE_BASE64_CORRUPT_INPUT_ERROR);
```

`part` is `hel`, `*off` is 7, and the error reads `illegal base64 data at input byte 7`. The error lives in the goroutine's error arena, like the errors strconv returns, so `error_retain` it to keep it past the end of the goroutine.

`base64_new_encoder` gives an `IoWriteCloser` that encodes into another writer. It holds back up to two bytes until it has a whole group of three, so Close it at the end to write those and the padding:

<!-- example: ../examples/encoding/base64.c#stream -->
```c
StringsBuilder out = STRINGS_BUILDER(a);
IoWriteCloser enc =
    base64_new_encoder(a, base64_std_encoding, strings_builder_as_io_writer(&out));
fmt_fprintf_v(io_write_closer_as_io_writer(enc), "%d apples", 12);
enc.vt->closer.close(enc.data);
```

`out` now holds `MTIgYXBwbGVz`. `base64_new_decoder` goes the other way, as an `IoReader`.

## What is not here

Nothing from Go's `encoding`, `encoding/base64` or `encoding/hex` is missing.
