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

## Base32

`burrow/encoding/base32.h` is Go's `encoding/base32`. Five bytes become eight characters, so the text is longer than base64, but it has no lower case and no punctuation beyond the `=` padding, which makes it safe where case gets folded and easy to read out. `base32_std_encoding` is the RFC 4648 alphabet and `base32_hex_encoding` the "extended hex" one, which sorts in the same order as the bytes it encodes:

<!-- example: ../examples/encoding/base32.c#oneshot -->
```c
Str s = base32_encoding_encode_to_string(base32_std_encoding, a,
                                         BURROW_B("Hello, Gophers"));

Error err = BURROW_NO_ERROR;
Slice b = base32_encoding_decode_string(base32_hex_encoding, a,
                                        BURROW_S("91IMOR3F41BMUSJCCG======"), &err);
```

`s` is `JBSWY3DPFQQEO33QNBSXE4Y=` and `b` is `Hello World`. The rest of the calls match base64's one for one, with `base32_` in front: the lengths, the calls that work into a slice you have, the `append_` forms, and the streaming encoder and decoder. Decoding skips `\r` and `\n` here as well, and does it without copying the input first, which Go's `Decode` does.

`base32_new_encoding` takes a 32 byte alphabet, and `base32_encoding_with_padding` changes the padding or drops it. There is no strict mode, as Go's base32 has none. Both return the `Base32Encoding` by value:

<!-- example: ../examples/encoding/base32.c#raw -->
```c
Base32Encoding raw =
    base32_encoding_with_padding(base32_std_encoding, BASE32_NO_PADDING);
Str r = base32_encoding_encode_to_string(&raw, a, BURROW_B("key"));
Slice c = base32_encoding_decode_string(&raw, a, BURROW_S("NNSXS"), &err);
```

`r` is `NNSXS` and `c` is `key`. Bad input gives back what decoded before it and a `Base32CorruptInputError` with the offset, counted with any newlines left out:

<!-- example: ../examples/encoding/base32.c#bad -->
```c
Slice part = base32_encoding_decode_string(base32_std_encoding, a,
                                           BURROW_S("NBSWY3DPEB3W64TMMQ1="), &err);
const Base32CorruptInputError *off =
    errors_as(err, TYPE_BASE32_CORRUPT_INPUT_ERROR);
```

`part` is `hello worl`, `*off` is 18, and the error reads `illegal base32 data at input byte 18`. The encoder from `base32_new_encoder` holds back up to four bytes until it has five, so Close it at the end:

<!-- example: ../examples/encoding/base32.c#stream -->
```c
StringsBuilder out = STRINGS_BUILDER(a);
IoWriteCloser enc =
    base32_new_encoder(a, base32_std_encoding, strings_builder_as_io_writer(&out));
fmt_fprintf_v(io_write_closer_as_io_writer(enc), "%d apples", 12);
enc.vt->closer.close(enc.data);
```

`out` now holds `GEZCAYLQOBWGK4Y=`.

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

## Ascii85

`burrow/encoding/ascii85.h` is Go's `encoding/ascii85`, the encoding PostScript and PDF use. Four bytes become five characters, and a group of four zero bytes becomes a single `z`. There is only one alphabet and no padding, so there is no encoding value to pass around, just functions:

<!-- example: ../examples/encoding/ascii85.c#encode -->
```c
Slice src = BURROW_B("Hello, world");
Int max = ascii85_max_encoded_len(src.len);
Slice dst = slice_make(a, TYPE_BYTE, max, max);
Int n = ascii85_encode(dst, src);
```

`max` is 15 and the text is `87cURD_*#TDfTZ)`. `ascii85_max_encoded_len` is an upper bound because of the `z` groups, so use the count `ascii85_encode` returns. It writes a whole group into `dst` before it knows how many of the characters it keeps, the same as Go, so `dst` needs room for that group even at the end.

`ascii85_decode` returns how many bytes it wrote and, through `nsrc`, how many bytes of input it used. Pass `NULL` if you don't need that count. It skips spaces, newlines and the other control bytes, and it stops early, without an error, once `dst` has less than four bytes of room. With `flush` false it leaves a group it can't finish for the next call. With `flush` true it decodes that group as the end of the input:

<!-- example: ../examples/encoding/ascii85.c#decode -->
```c
Slice in = BURROW_B("87cURD]i,\"Ebo80");
Slice out = slice_make(a, TYPE_BYTE, 4 * in.len, 4 * in.len);
Int nsrc;
Error err = BURROW_NO_ERROR;
Int ndst = ascii85_decode(out, in, true, &nsrc, &err);
```

That gives `Hello World!`, 12 bytes from all 15 characters. The `<~` and `~>` markers that Adobe puts around the text are not part of the encoding, so strip them first, as Go leaves that to you too. A byte out of range is an `Ascii85CorruptInputError` holding its offset, and nothing that came before it is returned:

<!-- example: ../examples/encoding/ascii85.c#bad -->
```c
ascii85_decode(out, BURROW_B("87cUR~>"), true, NULL, &err);
const Ascii85CorruptInputError *off =
    errors_as(err, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
```

`*off` is 5 and the error reads `illegal ascii85 data at input byte 5`. `ascii85_new_encoder` and `ascii85_new_decoder` do the same over a writer and a reader. The encoder needs a Close at the end to write a short last group, and the decoder takes care of groups split across reads:

<!-- example: ../examples/encoding/ascii85.c#stream -->
```c
StringsReader sr;
strings_reader_reset(&sr, BURROW_S("87cURD]i,\n\"Ebo80"));
IoReader dec = ascii85_new_decoder(a, strings_reader_as_io_reader(&sr));
BytesBuffer got = BYTES_BUFFER(a);
io_copy(a, bytes_buffer_as_io_writer(&got), dec, &err);
```

`got` holds `Hello World!`.

## Binary

`burrow/encoding/binary.h` is Go's `encoding/binary`: numbers in a fixed byte order, fixed size values made of them, and varints. The byte orders are `binary_little_endian`, `binary_big_endian` and `binary_native_endian`, each a `BinaryByteOrder` you can pass around. Each order's methods are also plain inline functions, which is what you want in a hot loop:

<!-- example: ../examples/encoding/binary.c#order -->
```c
Byte b[4];
Slice buf = slice_from(b, 4, 4, TYPE_BYTE);
binary_big_endian_put_uint32(buf, 0xcafe0102);
uint32_t le = binary_little_endian_uint32(buf);
Slice out = binary_big_endian_append_uint16(a, slice_nil(TYPE_BYTE), 0xbeef);
```

`b` holds `ca fe 01 02`, and reading it back little endian gives `0x201feca`. The getters and setters panic with Go's index message when the slice is too short, and they check that before touching anything. The append functions grow the slice from the allocator only when it is full, so `out` holds `be ef`.

`binary_write`, `binary_read`, `binary_encode`, `binary_decode`, `binary_append` and `binary_size` take the value as an `Any`, so they work on any type declared with the macros in `burrow/declare.h`. A value can be a bool, a sized number, a complex number, an array or struct of those, a slice of them, or a pointer to any of these. Structs are written field by field with no padding, and a field named `_` is written as zeros and skipped when read. Declare the type first:

<!-- example: ../examples/encoding/binary.c#declare -->
```c
BURROW_ARRAY_TYPE(Magic, uint8_t, 4);

#define HEADER_FIELDS(F, T)                                                            \
    F(T, Magic, Magic, "")                                                             \
    F(T, uint16_t, Version, "")                                                        \
    F(T, uint32_t, Count, "")                                                          \
    F(T, double, Scale, "")
BURROW_STRUCT(Header, HEADER_FIELDS);
```

Then write it. The value is encoded into a buffer and handed to the writer in one call. The buffer is on the stack for values up to 256 bytes, and bigger ones borrow it from the allocator:

<!-- example: ../examples/encoding/binary.c#write -->
```c
Header h = {{{'B', 'R', 'W', '1'}}, 2, 1000, 0.5};
BytesBuffer w = BYTES_BUFFER(a);
Error err = binary_write(a, bytes_buffer_as_io_writer(&w), binary_big_endian,
                         BURROW_ANY(TYPE_OF(Header), &h));
```

Those are 18 bytes, the same as `binary_size` gives: `42 52 57 31 00 02 00 00 03 e8 3f e0 00 00 00 00 00 00`. Reading them back fills in a `Header` again:

<!-- example: ../examples/encoding/binary.c#read -->
```c
Header back;
BytesReader r;
bytes_reader_reset(&r, wb);
err = binary_read(a, bytes_reader_as_io_reader(&r), binary_big_endian,
                  BURROW_ANY(TYPE_OF(Header), &back));
```

A read that runs out of input partway through gives `io_err_unexpected_eof`, and one that gets nothing gives `io_eof`, as `io_read_full` does. Go's rule about unexported fields comes along too. Reading into a struct with a lowercase field panics the way reflect does, so give fields that are read capital letters, or name padding `_`, `_1`, `_2` and so on.

Varints are the same as Go's. Unsigned ones are seven bits a byte, and signed ones are zigzag encoded first so small negative numbers stay short:

<!-- example: ../examples/encoding/binary.c#varint -->
```c
Slice v = binary_append_uvarint(a, slice_nil(TYPE_BYTE), 300);
v = binary_append_varint(a, v, -3);
Int n1, n2;
uint64_t x = binary_uvarint(v, &n1);
int64_t y = binary_varint(slice_sub(v, n1, v.len), &n2);
```

That is `ac 02 05`, and it decodes back to 300 from two bytes and -3 from one. `binary_uvarint` reports a short buffer as 0 bytes read and an overflow as a negative count. `binary_read_uvarint` and `binary_read_varint` read from an `IoByteReader`, which you get from `bytes_reader_as_io_byte_reader`, `bytes_buffer_as_io_byte_reader` or `strings_reader_as_io_byte_reader`.

Anything without a fixed size is an error rather than a guess. That includes `Int`, `Uint` and `Uintptr`, which is Go's rule for `int` and `uint`:

<!-- example: ../examples/encoding/binary.c#bad -->
```c
Int nums[2] = {1, 2};
IntSlice ns = slice_from(nums, 2, 2, TYPE_INT);
err = binary_write(a, bytes_buffer_as_io_writer(&w), binary_little_endian,
                   BURROW_ANY(TYPE_OF(IntSlice), &ns));
```

The error reads `binary.Write: some values are not fixed-sized in type []int`.

## What is not here

Nothing from Go's `encoding`, `encoding/ascii85`, `encoding/base32`, `encoding/base64`, `encoding/binary` or `encoding/hex` is missing.
