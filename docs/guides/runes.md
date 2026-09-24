# Runes and UTF-8

A `Str` holds bytes. Text is what you get when you agree to read those bytes as UTF-8, and that agreement is the whole of this guide.

Go takes the same position. A Go string is a byte slice with a different type name, `len(s)` counts bytes, `s[i]` gives you a byte, and the only place the encoding shows up is in the two constructs that read runes out of it: the `range` loop and the `unicode/utf8` package. burrow has both, in `burrow/core.h` and `burrow/utf8.h`.

## The rune loop

This is the one to reach for. It is `for i, r := range s`.

<!-- example: ../examples/runes/runes.c#loop -->
```c
Int i;
Rune r;
for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);)
    printf("%lld: %lx\n", (long long)i, (unsigned long)r);
```

`i` is the byte offset the rune started at, not a count of runes. That is what Go's loop gives you and it is what you need in order to slice, because the next thing you want after finding a rune is usually the text on either side of it.

Either pointer may be `NULL` if you only want the other one, which is the same rule every multiple result in the library follows:

<!-- example: ../examples/runes/runes.c#count -->
```c
Int count = 0;
for (StrIter it = str_runes(s); str_next_rune(&it, NULL, NULL);)
    count++;
```

That particular loop has a name, `utf8_rune_count_in_string`, and you should use it instead. It is written here to show that the out parameters are genuinely optional.

Keep the `StrIter` on the stack. The fields are visible because C has no other way to let you declare one, and they are not yours to read.

## Walking the bytes instead

For ASCII data, do not decode. Index `s.p` directly and let the loop condition be your bounds check:

<!-- example: ../examples/runes/runes.c#bytes -->
```c
for (Int i = 0; i < s.len; i++)
    if (s.p[i] == '\n')
        lines++;
```

This is not cheating and it is not a shortcut. Go's own library does exactly this everywhere the answer cannot depend on the encoding, which is most places, because UTF-8 has the property that a byte below 0x80 only ever means itself. A newline is a newline, a comma is a comma, and no multi byte sequence can contain either one. `strings_index_byte` is a byte loop in Go and it is a byte loop here.

Decode when the answer depends on what the characters are: upper casing, counting display width, cutting a string at a character boundary, validating input.

## The package

`burrow/utf8.h` is `unicode/utf8`, ported whole. Every function has two spellings, one over a `Str` and one over a byte `Slice`, because Go has two and code ported from Go will reach for whichever it used there.

| Go | burrow |
| --- | --- |
| `utf8.DecodeRuneInString` | `utf8_decode_rune_in_string(Str s, Int *size)` |
| `utf8.DecodeRune` | `utf8_decode_rune(Slice p, Int *size)` |
| `utf8.DecodeLastRuneInString` | `utf8_decode_last_rune_in_string(Str s, Int *size)` |
| `utf8.DecodeLastRune` | `utf8_decode_last_rune(Slice p, Int *size)` |
| `utf8.FullRuneInString` | `utf8_full_rune_in_string(Str s)` |
| `utf8.FullRune` | `utf8_full_rune(Slice p)` |
| `utf8.RuneLen` | `utf8_rune_len(Rune r)` |
| `utf8.EncodeRune` | `utf8_encode_rune(Slice p, Rune r)` |
| `utf8.AppendRune` | `utf8_append_rune(Alloc *a, Slice p, Rune r)` |
| `utf8.RuneCountInString` | `utf8_rune_count_in_string(Str s)` |
| `utf8.RuneCount` | `utf8_rune_count(Slice p)` |
| `utf8.RuneStart` | `utf8_rune_start(Byte b)` |
| `utf8.ValidString` | `utf8_valid_string(Str s)` |
| `utf8.Valid` | `utf8_valid(Slice p)` |
| `utf8.ValidRune` | `utf8_valid_rune(Rune r)` |
| `utf8.RuneError` | `UTF8_RUNE_ERROR` |
| `utf8.RuneSelf` | `UTF8_RUNE_SELF` |
| `utf8.MaxRune` | `UTF8_MAX_RUNE` |
| `utf8.UTFMax` | `UTF8_UTF_MAX` |

The size comes back through an out parameter rather than as a second return value, which is the library's rule for every Go function with more than one result, and it may be `NULL`:

<!-- example: ../examples/runes/runes.c#decode -->
```c
Int size;
Rune r = utf8_decode_rune_in_string(s, &size); /* r, size := ... */
r = utf8_decode_rune_in_string(s, NULL);       /* r, _ = ... */
```

## Nothing here fails

There is no error return anywhere in this package. A decode of bytes that are not UTF-8 gives you `UTF8_RUNE_ERROR`, which is U+FFFD, the replacement character, and a width of one byte.

That is Go's answer and it is the right one, but it is worth understanding rather than just accepting. The width of one is what makes a loop over corrupt input terminate: every call consumes at least one byte, so a program handed a truncated file prints mojibake and keeps going instead of hanging or crashing. It also means the loop never skips over a byte that might be the start of something valid, since a bad three byte sequence gives up after the first byte rather than swallowing all three.

The cost is one ambiguity, and it is small. A decode that returns `UTF8_RUNE_ERROR` with a size of 3 read a real U+FFFD that was sitting in the input, because the replacement character is itself a perfectly ordinary rune that encodes in three bytes. The same rune with a size of 1 is an error. Check the size when you need to tell them apart. Go has the identical pair and the identical answer.

An empty input gives `UTF8_RUNE_ERROR` with a size of 0, which is the third case and the one that means stop rather than continue.

## What counts as invalid

More than you might expect, and all of it on purpose:

- A malformed sequence: a starter byte followed by something that is not a continuation byte.
- A truncated sequence at the end of the input.
- A rune above U+10FFFF, which is the largest code point Unicode has.
- A surrogate half, U+D800 through U+DFFF. These exist so UTF-16 can spell code points above U+FFFF using two 16 bit units, and they are not valid in UTF-8. The usual way one turns up in a byte stream is a program that converted from UTF-16 carelessly.
- An overlong encoding: a rune written in more bytes than it needed.

That last one is the security case and it is the reason to care about this list. U+002F is a slash and it encodes as one byte, `0x2F`. It is possible to write the same code point as `0xC0 0xAF`, two bytes, and a decoder that accepts that will hand a slash to the caller from input that a filter looking for `0x2F` never saw. Path traversal checks have been walked straight past this way. `utf8_decode_rune_in_string` rejects it, and so does `utf8_valid_string`.

## Validating

Check bytes that came from outside your program once, at the boundary:

<!-- example: ../examples/runes/runes.c#validate -->
```c
static Error accept(Str body) {
    if (!utf8_valid_string(body))
        return err_bad_request;
    return BURROW_NO_ERROR;
}
```

After that, decode freely. The point of validating at the edge is that everything downstream can stop worrying, which is worth more than the one pass it costs.

`utf8_valid_string` is stricter than a decode loop that ignores the result. Both walk the whole input, but the validator says no to anything the decoder would have replaced, so the two answer different questions: "is this text" and "what does this turn into".

## Encoding

`utf8_encode_rune` writes into a buffer you provide and answers how many bytes that took:

<!-- example: ../examples/runes/runes.c#encode -->
```c
Byte buf[UTF8_UTF_MAX];
Slice out = slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE);
Int n = utf8_encode_rune(out, r);
```

The buffer has to have room. Go's version indexes without checking and so does this one, because the caller always knows: either the destination is at least `UTF8_UTF_MAX` bytes, which is four, or it was sized with `utf8_rune_len` first.

A rune that cannot be encoded, meaning a negative one, one above U+10FFFF, or a surrogate half, is written as `UTF8_RUNE_ERROR` instead. So the result is never zero and never negative, and you never have to check it. `utf8_rune_len` is the function that tells you the rune was bad, by returning -1.

When you are building a string rather than filling a fixed buffer, append:

<!-- example: ../examples/runes/runes.c#append -->
```c
Slice out = slice_nil(TYPE_BYTE);
for (Int i = 0; i < n; i++)
    out = utf8_append_rune(a, out, runes[i]);
```

## Rune is signed

`Rune` is `int32_t`, matching Go's `rune`, and the signedness is not an accident. Decoders return a length next to a rune and a negative length is how they say the input ran out, so the two have to be comparable. It also means `utf8_rune_len` can return -1 and `-1` is a rune value you can pass around and check.

Do not use `Rune` as a byte. `Byte` is `uint8_t` and it is a different thing. Assigning a byte above 0x7F into a `Rune` and calling it a character is the single most common encoding bug there is, because it silently works for the first hundred ASCII inputs and then produces nonsense for the first name with an accent in it.

## Finding your footing in the middle of a buffer

`utf8_rune_start` answers whether a byte could begin an encoding. Continuation bytes always have their top two bits set to `10` and nothing else does, so this is one mask and one compare:

<!-- example: ../examples/runes/runes.c#start -->
```c
while (i > 0 && !utf8_rune_start(buf[i]))
    i--;
```

This is how you back up to a character boundary after seeking into a file at an arbitrary offset, and it is why the backwards decoders are O(1): a rune is at most four bytes, so the search for the start of the previous one gives up after four.

## UTF-16

Windows, Java and JavaScript keep text as UTF-16, so you meet it at the edge of any of them. `burrow/unicode/utf16.h` is Go's unicode/utf16. A rune below U+10000 is one 16 bit unit, and anything above takes two, called a surrogate pair. The slices are typed: `utf16_encode` takes a slice of `Rune` and gives back a slice of `uint16_t`, and `utf16_decode` goes the other way.

<!-- example: ../examples/runes/runes.c#utf16 -->
```c
Rune in[] = {'h', 'i', 0x1F600};
Slice units = utf16_encode(a, slice_from(in, 3, 3, TYPE_RUNE));
Slice back = utf16_decode(a, units);
```

The emoji takes two units, so `units` holds four: `0068 0069 d83d de00`. For one rune at a time there are `utf16_encode_rune` and `utf16_decode_rune`. Go returns the pair as two results, and here the second half comes back through a pointer:

<!-- example: ../examples/runes/runes.c#pair -->
```c
Rune lo;
Rune hi = utf16_encode_rune(0x1F600, &lo);
Rune r = utf16_decode_rune(hi, lo); /* 0x1F600 again */
```

Like the UTF-8 side, nothing fails. A rune that cannot be encoded becomes U+FFFD, and so does a surrogate half without its partner when you decode. `utf16_append_rune` appends to a slice you are building, and `utf16_rune_len` tells you 1, 2, or -1 for a rune UTF-16 cannot hold.

## What is not here yet

`unicode` itself, with the character class tables, is a larger job and is scheduled with the rest of the pure packages.
