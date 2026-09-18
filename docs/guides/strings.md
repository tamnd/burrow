# Strings

A burrow string is a pointer and a length, passed by value, and it is not NUL terminated.

```c
typedef struct Str {
    const Byte *p;
    Int len;
} Str;
```

Sixteen bytes on a 64 bit machine, which fits in two registers, so passing one costs the same as passing a pointer and an integer, because that is what it is.

## Making one

From a literal, which is the common case:

```c
Str name = BURROW_S("burrow");
```

`BURROW_S` compiles to a constant. There is no call and no `strlen`, so it costs nothing to write one inside a condition:

```c
if (strings_has_prefix(path, BURROW_S("/api/"))) { ... }
```

Only ever hand it a string literal. Handing it a `char *` variable will not compile, which is deliberate, because the version that did compile would silently give you the size of a pointer.

From a C string, when something else in your program produced one:

```c
Str s = str_from_cstr(argv[1]);
```

That is O(n), since it has to find the NUL, and it does not copy. The result points at the same bytes `argv[1]` does and lives exactly as long as they do. `NULL` gives you the empty string rather than a crash, because the code on the other side of this boundary is not yours and it returns `NULL`.

From bytes you already have:

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

```c
printf("path is " BURROW_STR_FMT "\n", BURROW_STR_ARG(path));
```

## Going back to C

When you need a real C string, for `open` or for somebody else's library, you ask for one, and because that allocates, you pass an allocator:

```c
char *path = str_to_cstr(a, name);
```

It copies, it appends the NUL, and it returns `NULL` if the allocation failed.

There is a trap here and it is worth knowing about before you hit it. If the `Str` contains a NUL, the C string you get back is a valid C string that says something shorter than the truth:

```c
Str s = BURROW_S("safe\0/../../etc/passwd");
char *c = str_to_cstr(a, s);   /* c is "safe" */
```

That is not a bug in `str_to_cstr`. It is the truncation that `Str` exists to make visible instead of automatic. When the bytes came from outside your program, ask first:

```c
if (str_has_nul(s))
    return error_bad_request;
```

## Comparing

```c
bool same = str_eq(a, b);
int order = str_cmp(a, b);
```

`str_cmp` compares bytes, unsigned, and returns a negative number, zero, or a positive number. That is what Go's `<` on strings does and what `sort.Strings` uses, so a sort in burrow puts things in the same order a sort in Go does. Nothing is case folded and nothing is normalised. A byte above `0x7F` sorts after every ASCII byte.

A zeroed `Str` and a `Str` pointing at zero bytes are both the empty string, and `str_eq` says so. That is why nothing in burrow ever checks `s.p == NULL`, and neither should you. Use `str_is_empty`.

## Indexing

```c
Byte b = str_at(s, 3);
```

Go's `s[i]`, including what Go does when `i` is out of range, which is stop. The check is not optional and it is not behind a build flag, because code written against Go relies on never reading past the end, and a version of this that trusted the caller would be a different language with the same spelling. It costs a compare and a branch the processor predicts perfectly.

It is a fatal error rather than a panic today, carrying the message Go's panic carries. See [failure.md](failure.md) for why and for what changes later.

When you are walking a string you have already bounds checked, index `s.p` directly and let the loop condition be the check:

```c
for (Int i = 0; i < s.len; i++)
    total += s.p[i];
```

That is what the library does internally and it is not cheating.

## Lifetimes

Every function that returns a `Str` says where the bytes came from, on the declaration:

```c
BURROW_OWNS(ret)        Str strings_to_upper(Alloc *a, Str s);
BURROW_BORROWS(ret, s)  Str strings_trim_space(Str s);
```

`strings_trim_space` has no allocator parameter, and that is the tell. A function that cannot allocate cannot give you new bytes, so what it returns has to point into what you gave it. The result is valid exactly as long as the input is.

When you need bytes that outlive their source, clone them:

```c
Str kept = str_clone(a, borrowed);
```

That matters more than it sounds. A twelve byte `Str` cut out of a ten megabyte file keeps nothing alive on its own, because a `Str` is just a pointer, but the arena that file was read into is a different story. Clone into a longer lived allocator and let the short one go.

`str_clone` returns the empty string if the allocation failed. There is no spare value in a `Str` to mean failure the way a pointer has `NULL`, so compare the lengths if you need to tell that apart from cloning something that was empty to begin with.

## Mutating

You do not. `Str.p` is `const` and no function in burrow writes through it.

Go enforces that in the compiler and C cannot, so here it is a contract rather than a guarantee, and casting the const away is you deciding to break it. When you need to build a string, use `strings.Builder`, which is ported and which hands you a `Str` at the end that borrows from its own storage.

## What is not here

The interesting operations. `strings_contains`, `strings_split`, `strings_fields`, `strings_replace_all` and the rest live in the `strings` package, where Go put them, and they are ports rather than inventions. `core.h` has only the handful of things with no Go equivalent, which is everything to do with C strings, since Go has no C string to convert to.
