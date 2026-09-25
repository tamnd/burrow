# Paths

`burrow/path.h` is Go's `path`: cleaning, joining and taking apart paths that use forward slashes, like the ones in URLs, in tar and zip archives and in `io/fs`. It never touches the file system and it does not know about backslashes or drive letters, so it gives the same answer on Linux, macOS and Windows. The paths of the operating system you are running on are the job of `path/filepath`, which comes later in the same milestone.

The names are Go's with the package in front: `path.Clean` is `path_clean` and `path.IsAbs` is `path_is_abs`.

## Cleaning and joining

`path_clean` rewrites a path into the shortest one that means the same thing, working only on the text. It collapses runs of slashes, drops `.` elements, cancels each `..` against the element before it and drops a trailing slash. `path_join` puts elements together with slashes, skips the empty ones and cleans the result:

<!-- example: ../examples/path/path.c#clean -->
```c
Str c = path_clean(a, BURROW_S("a/c/../b//./d/")); /* a/b/d */
Str r = path_clean(a, BURROW_S("/../x"));          /* /x */
Str j = path_join_v(a, 3, BURROW_S("usr"), BURROW_S(""),
                    BURROW_S("lib/")); /* usr/lib */
```

Go's `Join` is variadic. `path_join` takes a `Slice` of `Str`, which is what you have when the elements come from somewhere else, and `path_join_v` takes a count and then the elements, which is what you want when you are writing them out. Joining nothing, or only empty strings, gives the empty string and not `.`, as in Go.

These take an allocator, but a path that is already clean comes back as it is and costs nothing, the same as in Go. The result can point into your input, so treat it as borrowed from both. If the allocator runs out the answer is the empty string.

## Taking a path apart

The rest only cut the path you give them and return views into it:

<!-- example: ../examples/path/path.c#parts -->
```c
Str p = BURROW_S("static/css/site.min.css");
Str file;
Str dir = path_split(p, &file); /* "static/css/" and "site.min.css" */
Str ext = path_ext(p);          /* ".css" */
Str base = path_base(p);        /* "site.min.css" */
Str up = path_dir(a, p);        /* "static/css" */
```

`path_split` returns two things in Go, so the directory comes back and the file goes to the out parameter, which may be `NULL`. The directory keeps its trailing slash, so the two pieces put back together are always the path you started with. `path_dir` is the directory cleaned, which is why it takes an allocator, though it only needs it when the directory has something to clean.

## Matching

`path_match` is Go's shell pattern matcher. `*` matches any run of bytes except a slash, `?` matches one character except a slash, `[a-z]` and `[^a-z]` are character classes, and a backslash quotes the character after it. The whole name has to match:

<!-- example: ../examples/path/path.c#match -->
```c
Error err;
bool css = path_match(BURROW_S("static/*/*.css"), p, &err);  /* true */
bool deep = path_match(BURROW_S("static/*.css"), p, &err);   /* false */
bool bad = path_match(BURROW_S("[z-"), BURROW_S("z"), &err); /* false */
if (BURROW_FAILED(err))
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
```

A malformed pattern gives `path_err_bad_pattern`, which prints as `syntax error in pattern`. Go only checks the part of the pattern it did not get to when the match fails, so a pattern can match before it reaches its broken part and report no error, and burrow does the same so that the two agree on every input.

## See also

- [strings.md](strings.md), for splitting and trimming in general.
