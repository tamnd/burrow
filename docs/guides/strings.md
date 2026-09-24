# Strings

A burrow string is a pointer and a length, passed by value, and it is not NUL terminated.

<!-- not compiled: the definition in burrow/core.h, shown for reference -->
```c
typedef struct Str {
    const Byte *p;
    Int len;
} Str;
```

Sixteen bytes on a 64 bit machine, which fits in two registers, so passing one costs the same as passing a pointer and an integer, because that is what it is.

## Making one

From a literal, which is the common case:

<!-- example: ../examples/strings/str.c#literal -->
```c
Str name = BURROW_S("burrow");
```

`BURROW_S` compiles to a constant. There is no call and no `strlen`, so it costs nothing to write one inside a condition:

<!-- example: ../examples/strings/str.c#condition -->
```c
if (str_eq(method, BURROW_S("GET"))) {
    printf("a read\n");
}
```

Only ever hand it a string literal. Handing it a `char *` variable will not compile, which is deliberate, because the version that did compile would silently give you the size of a pointer.

From a C string, when something else in your program produced one:

<!-- example: ../examples/strings/str.c#cstr -->
```c
Str s = str_from_cstr(argv[1]);
```

That is O(n), since it has to find the NUL, and it does not copy. The result points at the same bytes `argv[1]` does and lives exactly as long as they do. `NULL` gives you the empty string rather than a crash, because the code on the other side of this boundary is not yours and it returns `NULL`.

From bytes you already have:

<!-- example: ../examples/strings/str.c#bytes -->
```c
Byte header[4] = {0xDE, 0xAD, 0xBE, 0xEF};
Str s = str_from_bytes(header, 4);
```

Also no copy.

## Why not char star

This is the decision the whole API rests on, so it is worth being blunt about it.

A Go string can contain a NUL byte. `strings.Split("a\x00b", "\x00")` is a meaningful call with a meaningful answer. `os.ReadFile` on a JPEG hands you bytes that are a perfectly legal Go string. If burrow used `char *`, every one of those cases would be a truncation, and truncations of attacker supplied input are a category of bug rather than a bug.

There is a second reason that matters just as much day to day. Taking a substring of a `Str` is two arithmetic operations and no allocation, because the result just points into the middle of the original. With `char *` every substring is a copy, which is why every C string library ends up with an ownership convention, and why every one of them has a different convention.

The cost is that you cannot hand a `Str` to `printf` with `%s`. You hand it to `printf` like this instead:

<!-- example: ../examples/strings/str.c#print -->
```c
printf("path is " BURROW_STR_FMT "\n", BURROW_STR_ARG(path));
```

## Going back to C

When you need a real C string, for `open` or for somebody else's library, you ask for one, and because that allocates, you pass an allocator:

<!-- example: ../examples/strings/str.c#to-cstr -->
```c
char *path = str_to_cstr(a, name);
```

It copies, it appends the NUL, and it returns `NULL` if the allocation failed.

There is a trap here and it is worth knowing about before you hit it. If the `Str` contains a NUL, the C string you get back is a valid C string that says something shorter than the truth:

<!-- example: ../examples/strings/str.c#truncate -->
```c
Str s = BURROW_S("safe\0/../../etc/passwd");
char *c = str_to_cstr(a, s); /* c is "safe" */
```

That is not a bug in `str_to_cstr`. It is the truncation that `Str` exists to make visible instead of automatic. When the bytes came from outside your program, ask first:

<!-- example: ../examples/strings/str.c#has-nul -->
```c
if (str_has_nul(s))
    return err_bad_request;
```

## Comparing

<!-- example: ../examples/strings/str.c#compare -->
```c
bool same = str_eq(a, b);
int order = str_cmp(a, b);
```

`str_cmp` compares bytes, unsigned, and returns a negative number, zero, or a positive number. That is what Go's `<` on strings does and what `sort.Strings` uses, so a sort in burrow puts things in the same order a sort in Go does. Nothing is case folded and nothing is normalised. A byte above `0x7F` sorts after every ASCII byte.

A zeroed `Str` and a `Str` pointing at zero bytes are both the empty string, and `str_eq` says so. That is why nothing in burrow ever checks `s.p == NULL`, and neither should you. Use `str_is_empty`.

## Indexing

<!-- example: ../examples/strings/str.c#at -->
```c
Byte b = str_at(s, 3);
```

Go's `s[i]`, including what Go does when `i` is out of range, which is stop. The check is not optional and it is not behind a build flag, because code written against Go relies on never reading past the end, and a version of this that trusted the caller would be a different language with the same spelling. It costs a compare and a branch the processor predicts perfectly.

It panics, carrying the message Go's panic carries, so a `BURROW_TRY` around the call catches it and `runtime_error_from` says it was the runtime's doing and not somebody else's. See [failure.md](failure.md) for the three ways burrow reports failure and which one you get.

When you are walking a string you have already bounds checked, index `s.p` directly and let the loop condition be the check:

<!-- example: ../examples/strings/str.c#walk -->
```c
for (Int i = 0; i < s.len; i++)
    total += s.p[i];
```

That is what the library does internally and it is not cheating.

## Lifetimes

Every function that returns a `Str` says where the bytes came from, on the declaration:

<!-- example: ../examples/strings/str.c#lifetimes -->
```c
BURROW_OWNS(ret) Str str_clone(Alloc *a, Str s);
BURROW_BORROWS(ret, p) Str str_from_bytes(const void *p, Int n);
```

`str_from_bytes` has no allocator parameter, and that is the tell. A function that cannot allocate cannot give you new bytes, so what it returns has to point into what you gave it. The result is valid exactly as long as the input is. The `strings` package follows the same rule, so `strings_to_upper` will take an allocator and `strings_trim_space` will not.

When you need bytes that outlive their source, clone them:

<!-- example: ../examples/strings/str.c#clone -->
```c
Str kept = str_clone(a, borrowed);
```

That matters more than it sounds. A twelve byte `Str` cut out of a ten megabyte file keeps nothing alive on its own, because a `Str` is just a pointer, but the arena that file was read into is a different story. Clone into a longer lived allocator and let the short one go.

`str_clone` returns the empty string if the allocation failed. There is no spare value in a `Str` to mean failure the way a pointer has `NULL`, so compare the lengths if you need to tell that apart from cloning something that was empty to begin with.

## Mutating

You do not. `Str.p` is `const` and no function in burrow writes through it.

Go enforces that in the compiler and C cannot, so here it is a contract rather than a guarantee, and casting the const away is you deciding to break it. When you need to build a string, the tool is `StringsBuilder`, covered below, which hands you a `Str` at the end that borrows from its own storage.

## The strings package

`burrow/strings.h` is Go's `strings` package, all of it, with the same behaviour down to the edge cases, because the tests are Go's own tests ported line by line. The functions follow the lifetime rule above. The ones that search take plain values and allocate nothing. The ones that cut a string up hand back views into the string you gave them. The ones that make a new string take an allocator first.

### Searching

<!-- example: ../examples/strings/package.c#search -->
```c
Str line = BURROW_S("  GET /index.html HTTP/1.1  ");
Str req = strings_trim_space(line);
bool get = strings_has_prefix(req, BURROW_S("GET "));
Int slash = strings_index_byte(req, '/');
Int html = strings_count(req, BURROW_S(".html"));
```

`strings_index`, `strings_index_byte`, `strings_index_rune`, `strings_index_any`, `strings_index_func` and their `last` versions return a byte offset or -1. `strings_contains` and friends are the same search answered as a bool. Offsets are in bytes, not runes, the same as in Go, so they are safe to slice with.

### Cutting

`strings_cut` is the one to reach for when a string has two halves. It returns the part before the separator, writes the part after it through a pointer, and says whether the separator was there at all:

<!-- example: ../examples/strings/package.c#cut -->
```c
Str host, port;
bool found;
host = strings_cut(BURROW_S("example.com:8080"), BURROW_S(":"), &port, &found);
```

Both halves point into the original, so this allocates nothing. `strings_cut_last`, `strings_cut_prefix` and `strings_cut_suffix` work the same way, and so do all the trim functions.

### Splitting

`strings_split` returns a `Slice` of `Str`, from the allocator, with every piece pointing into the input:

<!-- example: ../examples/strings/package.c#split -->
```c
Slice parts = strings_split(a, BURROW_S("a,b,,c"), BURROW_S(","));
for (Int i = 0; i < parts.len; i++) {
    Str p = BURROW_AT(Str, parts, i);
    printf("[" BURROW_STR_FMT "]", BURROW_STR_ARG(p));
}
printf("\n");
```

The empty field between the two commas is there, as it is in Go. When you want words rather than fields, `strings_fields` splits on any run of white space and drops the empty ones, and `strings_join` puts a slice back together:

<!-- example: ../examples/strings/package.c#fields -->
```c
Slice words = strings_fields(a, BURROW_S("  the quick\tbrown\n fox "));
Str joined = strings_join(a, words, BURROW_S("-"));
```

A failed allocation gives the nil slice, which has length 0, so a loop over the result does nothing rather than crashing.

### Sequences

Go 1.24 added `Lines`, `SplitSeq`, `SplitAfterSeq`, `FieldsSeq` and `FieldsFuncSeq`, which walk a string one piece at a time without building a slice first. Here they return an `IterSeq` whose yield gets a `const Str *`. See [Iterators](iter.md) for the ways to consume one. Calling it with your own yield is the cheapest:

<!-- example: ../examples/strings/package.c#yield -->
```c
static bool print_line(void *env, const void *v) {
    Int *n = (Int *)env;
    Str line = *(const Str *)v;
    printf("%lld: " BURROW_STR_FMT, (long long)++*n, BURROW_STR_ARG(line));
    return true;
}
```

<!-- example: ../examples/strings/package.c#lines -->
```c
Int n = 0;
IterSeq seq = strings_lines(a, BURROW_S("one\ntwo\nthree\n"));
BURROW_CALLF(seq, BURROW_FN(IterYield, print_line, &n));
```

The sequence keeps a small block of state from the allocator, so it needs one allocation where Go's closure needs one too. With an arena that goes when the arena does. With the heap allocator, give it back with `strings_seq_free`. The line sequence and the split sequences use their state up as they go, so like Go's they can be run once.

### Case and replacement

<!-- example: ../examples/strings/package.c#case -->
```c
Str upper = strings_to_upper(a, BURROW_S("gopher"));
Str same = strings_to_upper(a, upper);
bool fold = strings_equal_fold(BURROW_S("Straße"), BURROW_S("STRASSE"));
bool sigma = strings_equal_fold(BURROW_S("\xcf\x83"), BURROW_S("\xce\xa3"));
```

`strings_to_upper` of a string that is already upper case gives the input back, the same pointer, and allocates nothing. That is Go's behaviour and it is why the result is marked as borrowing from the input as well as coming from the allocator. `strings_equal_fold` uses simple case folding, one rune to one rune, so `σ` matches `Σ` but `ß` does not match `SS`. That is Go's answer too.

<!-- example: ../examples/strings/package.c#replace -->
```c
Str s = strings_replace_all(a, BURROW_S("oink oink oink"), BURROW_S("k"),
                            BURROW_S("ky"));
Str t = strings_replace(a, s, BURROW_S("oinky"), BURROW_S("moo"), 2);
```

The last argument of `strings_replace` is how many to replace, with -1 meaning all of them.

### Building

`StringsBuilder` is Go's `strings.Builder`. It grows a buffer from its allocator and hands the result back as a `Str` without copying it:

<!-- example: ../examples/strings/package.c#builder -->
```c
StringsBuilder b = STRINGS_BUILDER(a);
for (int i = 3; i > 0; i--) {
    strings_builder_write_string(&b, BURROW_S("tick "), NULL);
    strings_builder_write_byte(&b, (Byte)('0' + i));
    strings_builder_write_byte(&b, '\n');
}
strings_builder_write_rune(&b, 0x1F680, NULL);
Str out = strings_builder_string(&b);
```

Go's builder panics if you copy one that has been written to, because two copies would share and then fight over one buffer. This one does the same, which it does by remembering its own address. Keep a builder in one place and pass a pointer to it. The `Str` you get from `strings_builder_string` stays good after later writes and after `strings_builder_reset`, because the builder never writes over bytes it has handed out. With an arena that costs nothing. With the heap allocator, the strings you took are yours to free.

A write that could not get memory returns `burrow_err_out_of_memory` rather than dropping bytes quietly. `strings_builder_as_io_writer` gives you the builder as an `IoWriter`, for anything that writes to one.

### Many replacements at once

For a fixed set of replacements run over a lot of text, `StringsReplacer` is Go's `strings.Replacer`. It picks one of Go's four algorithms from the pairs you give it, a byte table when every old string is one byte, a Boyer-Moore search when there is only one, and a trie for everything else:

<!-- example: ../examples/strings/package.c#replacer -->
```c
static const Str pairs[] = {
    BURROW_S_INIT("<"),    BURROW_S_INIT("&lt;"), BURROW_S_INIT(">"),
    BURROW_S_INIT("&gt;"), BURROW_S_INIT("&"),    BURROW_S_INIT("&amp;"),
};
Slice list = slice_from((void *)(uintptr_t)pairs, 6, 6, TYPE_STRING);
StringsReplacer *r = strings_new_replacer(a, list);
Str safe = strings_replacer_replace(r, a, BURROW_S("a < b && c > d"));
```

The pairs are tried in order and matches do not overlap, so `&` is not escaped twice. The replacer keeps a pointer to your strings rather than a copy, so they have to live as long as it does, which literals do. It builds its tables the first time it is used, and is safe to share between goroutines after that. `strings_replacer_free` gives back everything it allocated.

### Reading

`StringsReader` is Go's `strings.Reader`, a string you can hand to anything that wants an `IoReader` or an `IoSeeker`:

<!-- example: ../examples/strings/package.c#reader -->
```c
StringsReader *r = strings_new_reader(a, BURROW_S("h\xc3\xa9llo"));
Error err = BURROW_NO_ERROR;
for (;;) {
    Int size;
    Rune c = strings_reader_read_rune(r, &size, &err);
    if (BURROW_FAILED(err))
        break;
    printf("U+%04X %lld\n", (unsigned)c, (long long)size);
}
```

It reads bytes, runes, or a range at an offset, and ends with `io_eof` the way every reader does.

## What is not here

Nothing from Go's `strings` package is missing. `core.h` keeps only the handful of things with no Go equivalent, which is everything to do with C strings, since Go has no C string to convert to. The byte slice versions of all of this belong to the `bytes` package, which is next.
