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

## CSV

`burrow/encoding/csv.h` is Go's `encoding/csv`, comma separated values as RFC 4180 describes them. A `CsvReader` hands you one record at a time as a `Slice` of `Str`, and `csv_reader_field_pos` says where each field started, which is handy for error messages of your own:

<!-- example: ../examples/encoding/csv.c#read -->
```c
StringsReader sr;
strings_reader_reset(&sr, BURROW_S("name,city\n"
                                   "\"Pike, Rob\",Sydney\n"
                                   "Ken,\"New\nJersey\"\n"));
CsvReader *r = csv_new_reader(a, strings_reader_as_io_reader(&sr));
for (;;) {
    Error err = BURROW_NO_ERROR;
    Slice rec = csv_reader_read(r, a, &err);
    if (errors_is(err, io_eof))
        break;
    Int col;
    Int line = csv_reader_field_pos(r, 1, &col);
    fmt_printf_v("%q at %d:%d\n", rec, line, col);
}
csv_reader_free(r);
```

That prints `["name" "city"] at 1:6`, then `["Pike, Rob" "Sydney"] at 2:13` and `["Ken" "New\nJersey"] at 3:5`. A quoted field keeps its commas and line breaks, and the `\r\n` inside one becomes `\n`, as in Go.

Each record is one block from the allocator you pass, the `Str` array and the text together. With an arena there is nothing to free. With any other allocator give each record back with `csv_record_free`, or set `reuse_record` and the reader hands you its own block every time, good until the next read. The options Go has on its `Reader`, such as `comma`, `comment`, `fields_per_record` and `lazy_quotes`, are fields of the same names on `CsvReader`, set after `csv_new_reader` and before the first read.

A malformed record gives a `CsvParseError` with the line and column, and `errors_is` sees through it to the reason:

<!-- example: ../examples/encoding/csv.c#bad -->
```c
strings_reader_reset(&sr, BURROW_S("a,b\nc,d,e\n"));
r = csv_new_reader(a, strings_reader_as_io_reader(&sr));
Error err = BURROW_NO_ERROR;
Slice all = csv_reader_read_all(r, a, &err);
const CsvParseError *pe = errors_as(err, TYPE_CSV_PARSE_ERROR);
```

`all` is the nil slice, `pe` says line 2, column 1, and the error reads `record on line 2: wrong number of fields`. `csv_reader_read` on its own would have returned the three field record along with that error, which is what Go does, so you can decide to keep it.

Writing quotes only the fields that need it:

<!-- example: ../examples/encoding/csv.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
CsvWriter *w = csv_new_writer(a, bytes_buffer_as_io_writer(&out));
Str row1[] = {BURROW_S("id"), BURROW_S("quote")};
Str row2[] = {BURROW_S("1"), BURROW_S("say \"hi\", then go")};
csv_writer_write(w, slice_from(row1, 2, 2, TYPE_STRING));
csv_writer_write(w, slice_from(row2, 2, 2, TYPE_STRING));
csv_writer_flush(w);
err = csv_writer_error(w);
csv_writer_free(w);
```

The buffer holds `id,quote` and `1,"say ""hi"", then go`. The writer is buffered, so nothing reaches the underlying writer until it fills up or you flush, and `csv_writer_error` is where a failed write shows up. Set `use_crlf` for `\r\n` line endings.

`CsvParseError` has one difference from Go. Its `Error` method, `csv_parse_error_error`, takes the allocator to build the text in when you made the struct yourself. An error that came out of the reader already has its text, and `error_text` gives it to you.

## PEM

`burrow/encoding/pem.h` is Go's `encoding/pem`, the text wrapping that keys and certificates come in, the kind that starts with `-----BEGIN CERTIFICATE-----`. `pem_decode` finds the next block, skipping anything before it, and points `rest` at what follows:

<!-- example: ../examples/encoding/pem.c#decode -->
```c
Slice rest = BURROW_B("Some text before the block\n"
                      "-----BEGIN MESSAGE-----\n"
                      "Proc-Type: 4,ENCRYPTED\n"
                      "Comment: hello there\n"
                      "\n"
                      "aGVsbG8sIHdvcmxk\n"
                      "-----END MESSAGE-----\n"
                      "and some after\n");
PemBlock *b = pem_decode(a, rest, &rest);
const Str *comment = BURROW_MAP_GET(Str, Str, b->headers, BURROW_S("Comment"));
```

`b->type` is `MESSAGE`, the two headers are in `b->headers`, a `map[string]string`, and `b->bytes` holds `hello, world`, already decoded from base64. `rest` is `and some after`. With no block to find you get NULL and `rest` stays all of the input, so calling it in a loop until NULL reads every block in a file.

The block owns everything in it, so the input can go away while it lives. The block and its text are a single allocation, the bytes are a second and the map a third. `pem_block_free` gives all of them back, and an arena needs nothing.

Encoding writes the headers with `Proc-Type` first and the rest sorted, then the data in base64 lines of 64:

<!-- example: ../examples/encoding/pem.c#encode -->
```c
Map *h = map_make(a, TYPE_STRING, TYPE_STRING, 2);
BURROW_MAP_SET(Str, Str, h, BURROW_S("Name"), BURROW_S("demo"));
BURROW_MAP_SET(Str, Str, h, BURROW_S("Proc-Type"), BURROW_S("4,ENCRYPTED"));
PemBlock out = {
    .type = BURROW_S("MESSAGE"), .headers = h, .bytes = BURROW_B("hello, world")};
Slice text = pem_encode_to_memory(a, &out);
```

That gives

```text
-----BEGIN MESSAGE-----
Proc-Type: 4,ENCRYPTED
Name: demo

aGVsbG8sIHdvcmxk
-----END MESSAGE-----
```

`headers` can be NULL when there are none. `pem_encode` writes to any `IoWriter` without allocating, and a header key with a colon in it is an error before anything is written.

## JSON text

`burrow/encoding/json/jsontext.h` is Go's `encoding/json/jsontext`, the lower half of json v2. It reads and writes JSON as tokens and whole values and checks the grammar as it goes, and it knows nothing about structs or maps. A `JsontextDecoder` hands out one token at a time, and `jsontext_decoder_stack_pointer` says where in the document it is:

<!-- example: ../examples/encoding/jsontext.c#read -->
```c
StringsReader sr;
strings_reader_reset(&sr,
                     BURROW_S("{\"name\": \"gopher\", \"tags\": [\"go\", 1.5e3]}"));
JsontextDecoder *d = jsontext_new_decoder_v(a, strings_reader_as_io_reader(&sr), 0);
for (;;) {
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (errors_is(err, io_eof))
        break;
    JsontextPointer at = jsontext_decoder_stack_pointer(d, a);
    Str kind = jsontext_kind_string(jsontext_token_kind(tok));
    fmt_printf_v("%-8s %-10s %s\n", kind, at, jsontext_token_string(tok, a));
}
jsontext_decoder_free(d);
```

The kind comes first, then the JSON pointer, then the token. The `{` sits at the top, so its pointer is empty, and a member name and its value share one pointer. Numbers come back as they were written, `1.5e3` here, until you ask for them with `jsontext_token_float` or `jsontext_token_int`. A token from the decoder points into its buffer and is good until the next read. `jsontext_token_clone` makes one that lasts.

Anything that breaks the grammar is a `JsontextSyntacticError` with the byte offset and the pointer, and `errors_is` sees through it to the reason:

<!-- example: ../examples/encoding/jsontext.c#bad -->
```c
strings_reader_reset(&sr, BURROW_S("{\"a\": 1, \"a\": 2}"));
d = jsontext_new_decoder_v(a, strings_reader_as_io_reader(&sr), 0);
Error err = BURROW_NO_ERROR;
jsontext_decoder_read_value(d, &err);
const JsontextSyntacticError *se = errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR);
```

The offset is 9, the pointer is `/a` and the error reads `jsontext: duplicate object member name "a"`. Duplicate names are rejected by default, as in Go, and `jsontext_allow_duplicate_names(true)` lets them through. Invalid UTF-8 is the same, with `jsontext_allow_invalid_utf8`.

An encoder writes tokens and whole values and checks them the same way, so it cannot produce broken JSON. Options are values you pass when you make one:

<!-- example: ../examples/encoding/jsontext.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
JsontextEncoder *e = jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&out), 1,
                                            jsontext_with_indent(BURROW_S("  ")));
jsontext_encoder_write_token(e, jsontext_begin_object);
jsontext_encoder_write_token(e, jsontext_string(BURROW_S("name")));
jsontext_encoder_write_token(e, jsontext_string(BURROW_S("<gopher>")));
jsontext_encoder_write_token(e, jsontext_string(BURROW_S("sizes")));
jsontext_encoder_write_value(e, BURROW_B("[1, 2.50, 1e3]"));
err = jsontext_encoder_write_token(e, jsontext_end_object);
jsontext_encoder_free(e);
```

That writes

```text
{
  "name": "<gopher>",
  "sizes": [
    1,
    2.50,
    1e3
  ]
}
```

The value keeps its numbers as written and only gets indented. `<` is not escaped unless you ask for `jsontext_escape_for_html`. The encoder writes to the underlying writer as each top-level value completes, so there is no flush.

A `JsontextValue` is a `Slice` of JSON text with methods to check and reformat it. `jsontext_value_canonicalize` gives the RFC 8785 form, which is what you want before hashing or signing JSON:

<!-- example: ../examples/encoding/jsontext.c#value -->
```c
JsontextValue v = jsontext_value_clone(
    BURROW_B("{\"b\": 2.0, \"a\": [true, 1E2], \"\\u00e9\": \"x\"}"), a);
Error cerr = jsontext_value_canonicalize_v(&v, a, 0);
```

The result is `{"a":[true,100],"b":2,"é":"x"}`: members sorted, numbers printed as float64s, escapes that were not needed undone. These functions rewrite the value in place when there is room, as Go's do, so they need memory they can write to. A `BURROW_B` literal points at read-only text, which is why the example clones it first.

## JSON values

`burrow/encoding/json/v2.h` is the upper half of Go's json v2. It turns C values into JSON and back, and the type descriptor is what tells it how. A struct declared with `BURROW_STRUCT` carries its field names and tags, so it behaves the way the same struct does in Go:

<!-- example: ../examples/encoding/jsonv2.c#types -->
```c
#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, double, Price, "json:\"price,string\"")                                       \
    F(T, Strs, Tags, "json:\"tags,omitempty\"")                                        \
    F(T, bool, Hidden, "json:\"-\"")                                                   \
    F(T, Int, Stock, "json:\"stock,omitzero\"")
BURROW_STRUCT(Item, ITEM_FIELDS);
```

The tags mean what they mean in Go: `string` writes the number as a JSON string, `omitempty` drops an empty slice, `omitzero` drops a zero value, and `-` leaves the field out altogether. Marshalling takes an `Any`, which is a type and a pointer to a value of it:

<!-- example: ../examples/encoding/jsonv2.c#marshal -->
```c
Str tags[] = {BURROW_S("tea"), BURROW_S("green")};
Item it = {BURROW_S("sencha"), 12.5, {tags, 2, 2, TYPE_OF(Str)}, true, 0};
Error err = BURROW_NO_ERROR;
Slice out = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Item), &it), &err, 0);
```

That gives `{"name":"sencha","price":"12.5","tags":["tea","green"]}`. `Hidden` is gone because of its tag, and `Stock` because it is zero. Unmarshalling takes an `Any` pointing at the value to fill in, which plays the part of the pointer you would hand to Go's `Unmarshal`:

<!-- example: ../examples/encoding/jsonv2.c#unmarshal -->
```c
Item back = {0};
err = jsonv2_unmarshal_v(
    a, BURROW_B("{\"name\":\"matcha\",\"price\":\"30\",\"stock\":4}"),
    BURROW_ANY(TYPE_OF(Item), &back), 0);
```

`back` ends up as matcha, 30 and 4. Whatever the value needs, strings, slices, maps and the things pointers point at, comes from the allocator you pass, and nothing the value held before is freed, so an arena is the natural thing to use.

An `Any` holding nothing is Go's `any`. A JSON object becomes a `Map *` of type `TYPE_JSONV2_MAP_STRING_ANY`, an array a `Slice` of type `TYPE_JSONV2_SLICE_ANY`, a number a `double`, and so on down:

<!-- example: ../examples/encoding/jsonv2.c#any -->
```c
Any v = {NULL, NULL};
err = jsonv2_unmarshal_v(a, BURROW_B("{\"b\":[1,\"two\",null],\"a\":true}"),
                         BURROW_ANY(TYPE_ANY, &v), 0);
Map *obj = *(Map **)v.data;
Str key = BURROW_S("b");
const Any *b = map_get(obj, &key);
Slice again = jsonv2_marshal_v(a, v, &err, 1, jsonv2_deterministic(true));
```

Maps come out in whatever order the map iterates in, as in Go. `jsonv2_deterministic(true)` sorts them, and `again` is `{"a":true,"b":[1,"two",null]}`.

When the JSON and the C value do not line up, the error is a `Jsonv2SemanticError` saying where and why, and `errors_as` with `TYPE_JSONV2_SEMANTIC_ERROR` gets the struct out:

<!-- example: ../examples/encoding/jsonv2.c#errors -->
```c
int8_t small = 0;
err =
    jsonv2_unmarshal_v(a, BURROW_B("300"), BURROW_ANY(TYPE_OF(int8_t), &small), 0);
```

The text is `json: cannot unmarshal JSON number 300 into Go int8: value out of range`, which is Go's word for word. A struct type names itself without the package, so where Go says `main.Item` burrow says `Item`. Unknown member names are skipped unless you pass `jsonv2_reject_unknown_members(true)`, and then the error wraps `jsonv2_err_unknown_name`.

A struct can keep the members it has no field for in an embedded map with string keys:

<!-- example: ../examples/encoding/jsonv2.c#fallback -->
```c
#define LOOSE_FIELDS(F, T)                                                             \
    F(T, Str, ID, "json:\"id\"")                                                       \
    F(T, Extra, Rest, "json:\",embed\"")
BURROW_STRUCT(Loose, LOOSE_FIELDS);
```

<!-- example: ../examples/encoding/jsonv2.c#loose -->
```c
Loose l = {0};
err = jsonv2_unmarshal_v(a, BURROW_B("{\"id\":\"7\",\"size\":2,\"hot\":true}"),
                         BURROW_ANY(TYPE_OF(Loose), &l), 0);
Slice loose = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Loose), &l), &err, 1,
                               jsonv2_deterministic(true));
```

`size` and `hot` land in `Rest`, and marshalling writes them back after `id`.

A type can take over its own encoding by listing Go's methods in its descriptor. Here a version goes out as a string like `"v0.2"` through `MarshalText` and comes back through `UnmarshalText`, which are the encoding package's methods:

<!-- example: ../examples/encoding/jsonv2.c#methods -->
```c
#define VERSION_FIELDS(F, T)                                                           \
    F(T, Int, Major, "")                                                               \
    F(T, Int, Minor, "")
BURROW_STRUCT_DECL(Version, VERSION_FIELDS);

static Slice version_marshal_text(Version *v, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "v%d.%d", v->Major, v->Minor);
    return slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
}

static Error version_unmarshal_text(Version *v, Alloc *a, Slice text) {
    (void)a;
    Str s = strings_trim_prefix(str_from_bytes(text.p, text.len), BURROW_S("v"));
    Str minor;
    bool found;
    Str major = strings_cut(s, BURROW_S("."), &minor, &found);
    Error err = BURROW_NO_ERROR;
    if (found)
        v->Major = strconv_atoi(major, &err);
    if (found && BURROW_OK(err))
        v->Minor = strconv_atoi(minor, &err);
    if (!found || BURROW_FAILED(err))
        return errors_new(error_allocator(), BURROW_S("not a version"));
    return BURROW_NO_ERROR;
}

#define VERSION_METHODS(M, T)                                                          \
    M(T, MarshalText, version_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                 \
    M(T, UnmarshalText, version_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Version, VERSION_FIELDS, VERSION_METHODS);

#define RELEASE_FIELDS(F, T)                                                           \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Version, Version, "json:\"version\"")
BURROW_STRUCT(Release, RELEASE_FIELDS);
```

<!-- example: ../examples/encoding/jsonv2.c#release -->
```c
Release r = {BURROW_S("burrow"), {0, 2}};
Slice rel = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Release), &r), &err, 0);
Release r2 = {0};
err = jsonv2_unmarshal_v(a, rel, BURROW_ANY(TYPE_OF(Release), &r2), 0);
```

`rel` is `{"name":"burrow","version":"v0.2"}` and `r2.Version` is `{0 2}` again. When `UnmarshalText` fails, the error says where: `json: cannot unmarshal JSON string into Go Version within "/version": not a version`.

Marshal tries `MarshalJSONTo`, `MarshalJSON`, `AppendText` and `MarshalText` in that order and uses the first one the type has. Unmarshal tries `UnmarshalJSONFrom`, `UnmarshalJSON` and `UnmarshalText`. The two that take a coder, `MarshalJSONTo` and `UnmarshalJSONFrom`, have to write or read exactly one value, and either can return `errors_err_unsupported` without touching the coder to hand over to the next method in the list. The signatures are `JSONV2_SIG_MARSHAL_JSON_TO`, `JSONV2_SIG_MARSHAL_JSON`, `JSONV2_SIG_UNMARSHAL_JSON_FROM` and `JSONV2_SIG_UNMARSHAL_JSON`, next to the encoding package's for the text methods. A field tagged `omitzero` whose type has an `IsZero` method, with `JSONV2_SIG_IS_ZERO`, is left out when the method says so.

A type can only list methods for itself. To change how some other type is written, or to change it for one call and not the next, hand Marshal a function for it with `jsonv2_with_marshalers`, and Unmarshal one with `jsonv2_with_unmarshalers`. These write every bool as `"yes"` or `"no"` and read it back the same way:

<!-- example: ../examples/encoding/jsonv2.c#yesno -->
```c
static Slice yes_no(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = *(const bool *)v.data ? BURROW_S("\"yes\"") : BURROW_S("\"no\"");
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static Error parse_yes_no(void *ctx, Alloc *a, Slice data, Any v) {
    (void)ctx;
    (void)a;
    Str s = str_from_bytes(data.p, data.len);
    if (!str_eq(s, BURROW_S("\"yes\"")) && !str_eq(s, BURROW_S("\"no\"")))
        return errors_new(error_allocator(), BURROW_S("want yes or no"));
    *(bool *)v.data = str_eq(s, BURROW_S("\"yes\""));
    return BURROW_NO_ERROR;
}

BURROW_PTR_TYPE(BoolPtr, bool);
BURROW_SLICE_TYPE(Bools, bool);
```

<!-- example: ../examples/encoding/jsonv2.c#funcs -->
```c
Jsonv2Marshalers *ms = jsonv2_marshal_func(a, TYPE_BOOL, yes_no, NULL);
Jsonv2Unmarshalers *us =
    jsonv2_unmarshal_func(a, TYPE_OF(BoolPtr), parse_yes_no, NULL);
bool flags[] = {true, false};
Slice fl = {flags, 2, 2, TYPE_BOOL};
Slice words = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Bools), &fl), &err, 1,
                               jsonv2_with_marshalers(ms));
Slice fl2 = slice_nil(TYPE_BOOL);
err = jsonv2_unmarshal_v(a, BURROW_B("[\"no\",\"maybe\"]"),
                         BURROW_ANY(TYPE_OF(Bools), &fl2), 1,
                         jsonv2_with_unmarshalers(us));
```

`words` is `["yes","no"]`, and the second element fails with `json: cannot unmarshal JSON string into Go *bool within "/1": want yes or no`. The type a function is for says which values it gets. A plain type like `TYPE_BOOL` means values of exactly that type, an unnamed pointer to T means values of T, which is what an unmarshal function has to be set up with since it fills the value in, and an interface means values whose type has its methods. The function is asked about every value on the way down, fields, elements, map keys and what pointers and interfaces hold, and it comes before the type's own methods. `jsonv2_marshal_to_func` and `jsonv2_unmarshal_from_func` work on the coder instead of on bytes and may return `errors_err_unsupported` to hand the value on, and `jsonv2_join_marshalers` puts several sets into one, with the earlier ones asked first.

A field of type `JsontextValue` is left as JSON text. Unmarshal copies the member's text into it without decoding it, and Marshal writes the text back out, checked and compacted like everything else it writes. That is the way to hold on to a payload whose shape depends on another field:

<!-- example: ../examples/encoding/jsonv2.c#event -->
```c
#define EVENT_FIELDS(F, T)                                                             \
    F(T, Str, Kind, "json:\"kind\"")                                                   \
    F(T, JsontextValue, Data, "json:\"data\"")
BURROW_STRUCT(Event, EVENT_FIELDS);
```

<!-- example: ../examples/encoding/jsonv2.c#raw -->
```c
Event ev = {0};
err = jsonv2_unmarshal_v(
    a, BURROW_B("{\"kind\":\"click\", \"data\": {\"x\": 1, \"y\": 2}}"),
    BURROW_ANY(TYPE_OF(Event), &ev), 0);
Slice evb = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Event), &ev), &err, 0);
```

`ev.Data` is `{"x": 1, "y": 2}`, spaces and all, and `evb` is `{"kind":"click","data":{"x":1,"y":2}}`. Tagged `json:",embed"`, a `JsontextValue` field is the fallback for members no other field claims, like the map in `Loose` above, except that it collects them as one JSON object.

## JSON, the v1 API

`burrow/encoding/json.h` is Go's `encoding/json`. In Go 1.25 and later that package is a thin layer over v2, and here it is the same: `json_marshal` and `json_unmarshal` call the v2 code with v1's defaults switched on. Those defaults are the old behaviour people already rely on. Map keys come out sorted, `<`, `>` and `&` are escaped, and member names match field names without regard to case.

<!-- example: ../examples/encoding/json.c#types -->
```c
#define PLAYER_FIELDS(F, T)                                                            \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Level, "json:\"level\"")                                                 \
    F(T, Scores, Scores, "json:\"scores\"")
BURROW_STRUCT(Player, PLAYER_FIELDS);
```

<!-- example: ../examples/encoding/json.c#marshal -->
```c
Player p = {BURROW_S("ada <admin>"), 3, map_make(a, TYPE_OF(Str), TYPE_INT, 2)};
BURROW_MAP_SET(Str, Int, p.Scores, BURROW_S("b"), 20);
BURROW_MAP_SET(Str, Int, p.Scores, BURROW_S("a"), 10);
Error err = BURROW_NO_ERROR;
Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(Player), &p), &err);
```

`out` is `{"Name":"ada \u003cadmin\u003e","level":3,"scores":{"a":10,"b":20}}`, the same bytes Go writes for the same struct. Going the other way, `LEVEL` in the input still finds the `level` field:

<!-- example: ../examples/encoding/json.c#unmarshal -->
```c
Player q = {0};
err = json_unmarshal(a, BURROW_B("{\"name\":\"grace\",\"LEVEL\":7}"),
                     BURROW_ANY(TYPE_OF(Player), &q));
```

The errors are v1's errors, with v1's wording. A wrong type gives a `JsonUnmarshalTypeError`, and bad JSON gives a `JsonSyntaxError`:

<!-- example: ../examples/encoding/json.c#errors -->
```c
err = json_unmarshal(a, BURROW_B("{\"level\":\"high\"}"),
                     BURROW_ANY(TYPE_OF(Player), &q));
const JsonUnmarshalTypeError *te = errors_as(err, TYPE_JSON_UNMARSHAL_TYPE_ERROR);
```

The message is `json: cannot unmarshal string into Go struct field Player.level of type int`, `te->value` is `string` and `te->offset` is 15. The "Go struct" wording is kept on purpose, so that code matching on Go's messages works unchanged. Cutting the input off after `"level":` gives `unexpected end of JSON input`, and the target is left as it was.

The functions that work on JSON text without a type are here too:

<!-- example: ../examples/encoding/json.c#indent -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
Slice src = BURROW_B("{\"a\": [1, 2], \"b\": {}}");
bool ok = json_valid(src);
err = json_indent(&buf, src, BURROW_S(""), BURROW_S("  "));
```

`json_valid` returns true, and the buffer ends up holding the value spread over several lines with two spaces per level, the way `json.Indent` lays it out. `json_compact` and `json_html_escape` work the same way.

A `JsonDecoder` reads one value after another from a reader, and a `JsonEncoder` writes each value it is given on a line of its own. Both keep the `Alloc` they were made with and put everything they hand out there, so an arena that outlives the stream is the easy choice:

<!-- example: ../examples/encoding/json.c#stream -->
```c
BytesReader in;
bytes_reader_reset(&in, BURROW_B("{\"Name\":\"a\"} {\"Name\":\"b\"}"));
BytesBuffer lines = BYTES_BUFFER(a);
JsonDecoder *dec = json_new_decoder(a, bytes_reader_as_io_reader(&in));
JsonEncoder *enc = json_new_encoder(a, bytes_buffer_as_io_writer(&lines));
for (;;) {
    Player each = {0};
    err = json_decoder_decode(dec, BURROW_ANY(TYPE_OF(Player), &each));
    if (BURROW_FAILED(err))
        break;
    each.Level = 1;
    err = json_encoder_encode(enc, BURROW_ANY(TYPE_OF(Player), &each));
}
```

The loop stops when `json_decoder_decode` returns `io_eof`, and `lines` holds `{"Name":"a","level":1,"scores":null}` and `{"Name":"b","level":1,"scores":null}`, one per line. A nil map is `null` in v1. Input that stops in the middle of a value gives `io_err_unexpected_eof` instead. That error sticks, and so does a syntax error: as in Go, the decoder returns it again from every later call.

For input too big to decode in one go, `json_decoder_token` reads it a token at a time. A token is a `JsonToken`, which is an `Any`. Its type says what came back: a `JsonDelim` for a bracket or brace, a `Str`, a `double`, a `bool`, or nothing for `null`:

<!-- example: ../examples/encoding/json.c#tokens -->
```c
bytes_reader_reset(&in, BURROW_B("[\"x\", 2]"));
dec = json_new_decoder(a, bytes_reader_as_io_reader(&in));
for (;;) {
    JsonToken tok = json_decoder_token(dec, &err);
    if (BURROW_FAILED(err))
        break;
    if (tok.t == TYPE_JSON_DELIM)
        printf("delim %c\n", (char)*(const JsonDelim *)tok.data);
    else if (tok.t == TYPE_OF(Str))
        printf("string " BURROW_STR_FMT "\n",
               BURROW_STR_ARG(*(const Str *)tok.data));
    else if (tok.t == TYPE_FLOAT64)
        printf("number %g\n", *(const double *)tok.data);
}
```

This prints `delim [`, `string x`, `number 2` and `delim ]`. Commas and colons never show up as tokens. `json_decoder_more` says whether the array or object being read has another element, and `json_decoder_use_number` makes numbers come back as `JsonNumber` text instead of doubles.

Each v1 behaviour also has its own option, such as `json_format_byte_array_as_array` or `json_match_case_sensitive_delimiter`, for code that calls v2's functions but wants only some of the old rules. `json_default_options_v1` turns all of them on at once.

## XML

`burrow/encoding/xml.h` is Go's `encoding/xml`: a decoder that reads a document one token at a time, an encoder that writes tokens back out, and `xml_marshal` and `xml_unmarshal`, which write a value as XML and read it back by its type's descriptor.

A token is an `XmlToken`, a tagged union with a member for each of Go's six token types. A zeroed one is Go's nil. `xml_decoder_token` resolves namespace prefixes and checks that the tags nest:

<!-- example: ../examples/encoding/xml.c#read -->
```c
StringsReader sr;
strings_reader_reset(&sr, BURROW_S("<feed xmlns=\"http://www.w3.org/2005/Atom\">"
                                   "<title>Burrow &amp; friends</title>"
                                   "<link href=\"https://example.com/\"/>"
                                   "</feed>"));
XmlDecoder *d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
for (;;) {
    Error err = BURROW_NO_ERROR;
    XmlToken tok = xml_decoder_token(d, &err);
    if (errors_is(err, io_eof))
        break;
    if (tok.kind == XML_START_ELEMENT) {
        Str space = tok.start.name.space, local = tok.start.name.local;
        fmt_printf_v("start %s in %s, %d attributes\n", local, space,
                     tok.start.attr.len);
    } else if (tok.kind == XML_CHAR_DATA) {
        Slice text = tok.char_data;
        fmt_printf_v("text %q\n", text);
    } else if (tok.kind == XML_END_ELEMENT) {
        Str local = tok.end.name.local;
        fmt_printf_v("end %s\n", local);
    }
}
xml_decoder_free(d);
```

That prints `start feed in http://www.w3.org/2005/Atom, 1 attributes`, then the title's start, its text `"Burrow & friends"` and the rest. The `xmlns` attribute stays in the start tag's list, as in Go. `xml_decoder_raw_token` is the same without the namespaces and the nesting check.

A token points into the decoder and is good until the next call that reads from it. Go's tokens have the same rule for their bytes, but Go's names are strings and outlive the call, so this is the place a port is most likely to go wrong. To keep one, `xml_copy_token` makes a copy that is one block from the allocator you pass, and `xml_token_free` gives it back.

The decoder's options are fields, as in Go, set after `xml_new_decoder` and before the first read. For HTML that isn't well formed, turn off `strict` and give it Go's lists of elements that close themselves and of entities:

<!-- example: ../examples/encoding/xml.c#html -->
```c
strings_reader_reset(&sr, BURROW_S("<p>caf&eacute;<br>menu</p>"));
d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
d->strict = false;
d->auto_close = xml_html_auto_close;
d->entity = xml_html_entity();
Error err = BURROW_NO_ERROR;
Int n = 0;
for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err))
    n++;
xml_decoder_free(d);
```

That reads six tokens. The `<br>` gets an end tag it never had, and `&eacute;` turns into `é`. `xml_html_entity` builds its map the first time it is called and shares it after that, so don't change or free it.

A document that is not well formed gives an `XmlSyntaxError` with the line it went wrong on:

<!-- example: ../examples/encoding/xml.c#bad -->
```c
strings_reader_reset(&sr, BURROW_S("<a>\n<b></a>"));
d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
}
const XmlSyntaxError *se = errors_as(err, TYPE_XML_SYNTAX_ERROR);
xml_decoder_free(d);
```

`se->line` is 2 and the error reads `XML syntax error on line 2: element <b> closed by </a>`.

The encoder checks the same things as it writes, escapes text and attribute values, and declares the namespaces the names need. `xml_encoder_indent` sets a prefix and an indent, which is what Go's `MarshalIndent` uses:

<!-- example: ../examples/encoding/xml.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
XmlEncoder *e = xml_new_encoder(a, bytes_buffer_as_io_writer(&out));
xml_encoder_indent(e, BURROW_S(""), BURROW_S("  "));
XmlAttr lang[] = {{{BURROW_S(""), BURROW_S("lang")}, BURROW_S("en")}};
XmlStartElement note = {{BURROW_S(""), BURROW_S("note")}, {0}};
XmlStartElement body = {{BURROW_S(""), BURROW_S("body")},
                        slice_from(lang, 1, 1, &burrow_type_XmlAttr)};
xml_encoder_encode_token(e, (XmlToken){XML_START_ELEMENT, .start = note});
xml_encoder_encode_token(e, (XmlToken){XML_START_ELEMENT, .start = body});
xml_encoder_encode_token(e,
                         (XmlToken){XML_CHAR_DATA, .char_data = BURROW_B("1 < 2")});
xml_encoder_encode_token(
    e, (XmlToken){XML_END_ELEMENT, .end = xml_start_element_end(body)});
xml_encoder_encode_token(
    e, (XmlToken){XML_END_ELEMENT, .end = xml_start_element_end(note)});
err = xml_encoder_close(e);
xml_encoder_free(e);
```

The buffer holds `<note>`, then `  <body lang="en">1 &lt; 2</body>` and `</note>` on lines of their own. The encoder is buffered like Go's. `xml_encoder_flush` writes what it has, and `xml_encoder_close` flushes and fails with `unclosed tag <...>` when an element is still open.

The tokens and their names are copied into the encoder as they are written, so the ones you pass need only last the call. `XmlSyntaxError`'s `Error` method, `xml_syntax_error_error`, takes an allocator for its text. An error that came out of the decoder already has the text, and `error_text` gives it to you.

### Marshal

`xml_marshal` takes any value with a descriptor and writes it the way Go's reflection would. A struct's fields say where they go with an `xml` tag, and the tag rules are Go's: `attr`, `chardata`, `cdata`, `innerxml`, `comment`, `omitempty`, `a>b>c` for nesting and a namespace before the name. The element's name comes from an `XMLName` field, or else the type's name:

<!-- example: ../examples/encoding/xml.c#types -->
```c
BURROW_SLICE_TYPE(Emails, Str);

#define PERSON_FIELDS(F, T)                                                            \
    F(T, XmlName, XMLName, "xml:\"person\"")                                           \
    F(T, Int, Id, "xml:\"id,attr\"")                                                   \
    F(T, Str, First, "xml:\"name>first\"")                                             \
    F(T, Str, Last, "xml:\"name>last\"")                                               \
    F(T, Emails, Email, "xml:\"email\"")                                               \
    F(T, Str, Nickname, "xml:\"nickname,omitempty\"")                                  \
    F(T, Str, Note, "xml:\",comment\"")
BURROW_STRUCT(Person, PERSON_FIELDS);
BURROW_MAP_TYPE(Scores, Str, Int);
```

<!-- example: ../examples/encoding/xml.c#marshal -->
```c
Str emails[] = {BURROW_S("jd@example.com"), BURROW_S("john@work.example")};
Person p = {.Id = 13,
            .First = BURROW_S("John"),
            .Last = BURROW_S("Doe"),
            .Email = slice_from(emails, 2, 2, TYPE_STRING),
            .Note = BURROW_S(" Need more details. ")};
Slice doc = xml_marshal_indent(a, BURROW_ANY(TYPE_OF(Person), &p), BURROW_S(""),
                               BURROW_S("  "), &err);
```

That gives:

```
<person id="13">
  <name>
    <first>John</first>
    <last>Doe</last>
  </name>
  <email>jd@example.com</email>
  <email>john@work.example</email>
  <!-- Need more details. -->
</person>
```

The empty nickname is left out because of `omitempty`. `xml_marshal_indent` is `xml_marshal` with the encoder's indent set, and `xml_encoder_encode` writes a value to an encoder you already have.

A type that wants to write itself has a `MarshalXML` method, declared with `XML_SIG_MARSHAL_XML`. It gets the encoder and the start element Marshal would have used, and it can change the element before it writes:

<!-- example: ../examples/encoding/xml.c#method -->
```c
#define TEMP_FIELDS(F, T) F(T, double, Celsius, "")
BURROW_STRUCT_DECL(Temp, TEMP_FIELDS);

static Error temp_marshal_xml(Temp *t, XmlEncoder *e, XmlStartElement start) {
    XmlAttr unit[] = {{{BURROW_S(""), BURROW_S("unit")}, BURROW_S("F")}};
    start.attr = slice_from(unit, 1, 1, &burrow_type_XmlAttr);
    double f = t->Celsius * 9 / 5 + 32;
    return xml_encoder_encode_element(e, BURROW_ANY(TYPE_OF(double), &f), start);
}

#define TEMP_METHODS(M, T) M(T, MarshalXML, temp_marshal_xml, XML_SIG_MARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(Temp, TEMP_FIELDS, TEMP_METHODS);
```

<!-- example: ../examples/encoding/xml.c#marshal-method -->
```c
Temp t = {21.5};
Slice temp = xml_marshal(a, BURROW_ANY(TYPE_OF(Temp), &t), &err);
```

`temp` holds `<Temp unit="F">70.7</Temp>`. A `MarshalXMLAttr` method, declared with `XML_SIG_MARSHAL_XML_ATTR`, does the same for a field marshaled as an attribute, and a type with `encoding`'s `MarshalText` is written as its text. Every C method takes a pointer receiver, so a method is found on a `T` as well as on a `*T`. Go only finds a pointer method on a value it can take the address of, so a value Go would write field by field may be written by its method here.

A method that leaves an element open fails the whole call with `xml: (*T).MarshalXML wrote invalid XML: <T> not closed`. Maps, channels and functions have no XML form, and marshaling one fails with an `XmlUnsupportedTypeError`:

<!-- example: ../examples/encoding/xml.c#marshal-error -->
```c
Scores m = map_make(a, TYPE_STRING, TYPE_OF(Int), 0);
xml_marshal(a, BURROW_ANY(TYPE_OF(Scores), &m), &err);
const XmlUnsupportedTypeError *ue = errors_as(err, TYPE_XML_UNSUPPORTED_TYPE_ERROR);
```

The error reads `xml: unsupported type: map[string]int` and `ue->type` is the map's descriptor. Two fields that want the same path fail with an `XmlTagPathError`, which names both.

Go allows a field named after its type without embedding it, `Port Port`. In C a field of type `T` or `T *` named `T` is how embedding is spelled, so give the C type another name when you mean an ordinary field.

### Unmarshal

`xml_unmarshal` reads the first element of a document into a value, by the same descriptor and the same tags. Reading back the person from above:

<!-- example: ../examples/encoding/xml.c#unmarshal -->
```c
Person q = {0};
err = xml_unmarshal(a, doc, BURROW_ANY(TYPE_OF(Person), &q));
```

That fills in all of `q`: the id from the attribute, the names from inside `<name>`, one email per `<email>` element and the note from the comment. Every string and slice it stores is allocated from `a`, so an arena is the easy thing to hand it. A nil pointer field gets a new value, a slice field grows by one for each element that matches it, and anything in the document the value has no place for is skipped. `xml_decoder_decode` does the same from a decoder, allocating from the decoder's allocator, and reads one element per call.

An `XMLName` field with a name in its tag is a check. The element has to have that name, or the call fails with an `XmlUnmarshalError`:

<!-- example: ../examples/encoding/xml.c#unmarshal-name -->
```c
err = xml_unmarshal(a, BURROW_B("<people><first>Ann</first></people>"),
                    BURROW_ANY(TYPE_OF(Person), &q));
```

The error reads `expected element type <person> but have <people>`.

A type that wants to read itself has an `UnmarshalXML` method, declared with `XML_SIG_UNMARSHAL_XML`. It gets the decoder and the start element, reads tokens up to the matching end, and gets `io_eof` there, since the decoder won't let it read past. Here one collects the names of the elements inside it:

<!-- example: ../examples/encoding/xml.c#unmarshal-method -->
```c
#define TAGS_FIELDS(F, T) F(T, Str, joined, "")
BURROW_STRUCT_DECL(TagList, TAGS_FIELDS);

static Error tags_unmarshal_xml(TagList *t, XmlDecoder *d, XmlStartElement start) {
    (void)start;
    StringsBuilder b = STRINGS_BUILDER(d->a);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (errors_is(err, io_eof))
            break;
        if (BURROW_FAILED(err))
            return err;
        if (tok.kind != XML_START_ELEMENT)
            continue;
        if (strings_builder_len(&b) > 0)
            err = strings_builder_write_byte(&b, ',');
        if (BURROW_OK(err))
            strings_builder_write_string(&b, tok.start.name.local, &err);
        if (BURROW_FAILED(err))
            return err;
    }
    t->joined = strings_builder_string(&b);
    return BURROW_NO_ERROR;
}

#define TAGS_METHODS(M, T) M(T, UnmarshalXML, tags_unmarshal_xml, XML_SIG_UNMARSHAL_XML)
BURROW_STRUCT_DEFINE_METHODS(TagList, TAGS_FIELDS, TAGS_METHODS);

#define POST_FIELDS(F, T)                                                              \
    F(T, Str, Title, "xml:\"title\"")                                                  \
    F(T, TagList, Tags, "xml:\"tags\"")
BURROW_STRUCT(Post, POST_FIELDS);
```

<!-- example: ../examples/encoding/xml.c#unmarshal-post -->
```c
Post post = {0};
err = xml_unmarshal(a,
                    BURROW_B("<post><title>Hello</title>"
                             "<tags><c/><xml/><go/></tags></post>"),
                    BURROW_ANY(TYPE_OF(Post), &post));
```

`post.Tags.joined` is `c,xml,go`. The start element and the tokens are only good until the method returns, so what it keeps it allocates, here from `d->a`. A method that returns before the end of its element fails the call with `xml: (*T).UnmarshalXML did not consume entire <tags> element`, and one that calls `xml_decoder_raw_token` gets an error back. `xml_decoder_decode_element` reads the element a method was given into another value, which is how a method hands the work back. `UnmarshalXMLAttr`, declared with `XML_SIG_UNMARSHAL_XML_ATTR`, does the same for an attribute, and `encoding`'s `UnmarshalText` reads a type from the element's text or an attribute's value.

Unmarshal recurses once for each element it reads into a value and stops with `exceeded max depth` past 10,000, Go's limit. Go's stack grows to fit and a goroutine's here does not, so to read documents nested thousands deep, read them on a goroutine started with `go_stack` and a few megabytes.

## Gob

`burrow/encoding/gob.h` is Go's `encoding/gob`, a stream of values where each type is described the first time it appears. The bytes are Go's, so a burrow program and a Go program can send each other values over a socket or a file. Types come from their descriptors, like everything else that takes an `Any`:

<!-- example: ../examples/encoding/gob.c#declare -->
```c
BURROW_SLICE_TYPE(StrSlice, Str);

#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Count, "")                                                               \
    F(T, double, Price, "")                                                            \
    F(T, StrSlice, Tags, "")
BURROW_STRUCT(Item, ITEM_FIELDS);
```

An encoder writes to an `IoWriter`. The first value of a type carries the type's description, and the value follows:

<!-- example: ../examples/encoding/gob.c#encode -->
```c
Str tags[2] = {BURROW_S("red"), BURROW_S("small")};
Item it = {BURROW_S("widget"), 3, 2.5, slice_from(tags, 2, 2, TYPE_STRING)};
BytesBuffer buf = BYTES_BUFFER(a);
GobEncoder *enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
Error err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Item), &it));
```

That is 110 bytes, the same 110 Go's encoder writes for the same `Item`. A second value of the same type through the same encoder is only the value, which here is 30 bytes:

<!-- example: ../examples/encoding/gob.c#second -->
```c
it.Count = 4;
Int before = bytes_buffer_len(&buf);
err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Item), &it));
gob_encoder_free(enc);
```

A decoder reads from an `IoReader` and fills in the value an `Any` points at. Strings, slices, maps and pointers in it are allocated from the allocator the decoder was made with, so an arena is the easy choice:

<!-- example: ../examples/encoding/gob.c#decode -->
```c
GobDecoder *dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&buf));
Item got = {0};
err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Item), &got));
```

`got` is `widget`, count 3, price 2.5, with two tags. The two ends match fields by name, not by position or type identity, so the second value can be read into a different struct. Fields the stream has and the struct does not are skipped, and fields the struct has and the stream does not are left alone:

<!-- example: ../examples/encoding/gob.c#subset -->
```c
#define LABEL_FIELDS(F, T)                                                             \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Count, "")                                                               \
    F(T, Str, Note, "")
BURROW_STRUCT(Label, LABEL_FIELDS);
```

<!-- example: ../examples/encoding/gob.c#into -->
```c
Label l = {0};
err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Label), &l));
```

`l` comes back with the name `widget`, count 4 and an empty `Note`. One more decode gives `io_eof`, and a stream cut off partway through a value gives `io_err_unexpected_eof`.

A type can take over its own encoding with `GobEncode` and `GobDecode` methods, declared with `GOB_SIG_GOB_ENCODE` and `GOB_SIG_GOB_DECODE`. Without them, `MarshalBinary` and `UnmarshalBinary` from `encoding` are used, which is Go's order too:

<!-- example: ../examples/encoding/gob.c#method -->
```c
#define VERSION_FIELDS(F, T)                                                           \
    F(T, Int, Major, "")                                                               \
    F(T, Int, Minor, "")
BURROW_STRUCT_DECL(Version, VERSION_FIELDS);

static Slice version_gob_encode(Version *v, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte two[2] = {(Byte)v->Major, (Byte)v->Minor};
    return slice_append(a, slice_nil(TYPE_BYTE), two, 2);
}

static Error version_gob_decode(Version *v, Alloc *a, Slice b) {
    (void)a;
    if (b.len != 2)
        return errors_new(error_allocator(), BURROW_S("version: want two bytes"));
    v->Major = ((const Byte *)b.p)[0];
    v->Minor = ((const Byte *)b.p)[1];
    return BURROW_NO_ERROR;
}

#define VERSION_METHODS(M, T)                                                          \
    M(T, GobDecode, version_gob_decode, GOB_SIG_GOB_DECODE)                            \
    M(T, GobEncode, version_gob_encode, GOB_SIG_GOB_ENCODE)
BURROW_STRUCT_DEFINE_METHODS(Version, VERSION_FIELDS, VERSION_METHODS);
```

A value inside an `Any` field travels with the name its type was registered under, so both ends call `gob_register` for it before the first one goes out. `gob_register_name` picks the name yourself, which is what you need to talk to a Go program that registered the type under its own package path:

<!-- example: ../examples/encoding/gob.c#iface -->
```c
gob_register(BURROW_ANY(TYPE_OF(Version), NULL));
Version v = {1, 22};
Box box = {BURROW_ANY(TYPE_OF(Version), &v)};
BytesBuffer vb = BYTES_BUFFER(a);
enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&vb));
err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Box), &box));
gob_encoder_free(enc);
```

<!-- example: ../examples/encoding/gob.c#iface-decode -->
```c
dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&vb));
Box out = {0};
err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Box), &out));
gob_decoder_free(dec);
```

`out.Value` holds a `Version` of 1.22, allocated by the decoder. The decoding end can only put such a value into an `Any`. C has no way to build a vtable for any other interface type at run time, so a value sent for one is an error.

A value whose type does not fit the target is an error that names both types, in Go's words:

<!-- example: ../examples/encoding/gob.c#bad -->
```c
Int n = 0;
dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&ib));
err = gob_decoder_decode(dec, BURROW_ANY(TYPE_INT, &n));
gob_decoder_free(dec);
```

The error reads `gob: decoding into local type *int, received remote type Item = struct { Name string; Count int; Price float; Tags []string; }`.

Go's stack grows as deep as the value needs and a C stack does not. The encoder and the decoder stop with `nesting too deep` when the next level would leave less than 64 KB of stack. Decoding takes about 1.8 KB a level on arm64 at -O2, so the default 256 KB goroutine stack holds about a hundred levels of nesting. To send a long linked list, start the goroutine with `go_stack` and a bigger stack, or send a slice.

## What is not here

Nothing from Go's `encoding`, `encoding/ascii85`, `encoding/base32`, `encoding/base64`, `encoding/binary`, `encoding/csv`, `encoding/gob`, `encoding/hex`, `encoding/json/jsontext`, `encoding/pem` or `encoding/xml` is missing. From `encoding/json/v2`, `time.Time` and `time.Duration` wait on the calendar half of `time`.
