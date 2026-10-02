# Paths

`burrow/path.h` is Go's `path`: cleaning, joining and taking apart paths that use forward slashes, like the ones in URLs, in tar and zip archives and in `io/fs`. It never touches the file system and it does not know about backslashes or drive letters, so it gives the same answer on Linux, macOS and Windows. The paths of the operating system you are running on are the job of `burrow/path/filepath.h`, Go's `path/filepath`, which is the second half of this guide.

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

## The operating system's paths

`filepath` has the same functions as `path` and a few more, with the rules of the system the program runs on. On Linux and macOS that changes almost nothing. On Windows the separator is a backslash, a forward slash counts as one too, and a path can start with a volume name: a drive letter such as `C:`, or a share such as `\\host\share`. `FILEPATH_SEPARATOR` and `FILEPATH_LIST_SEPARATOR` are the two characters, and every result uses the first:

<!-- example: ../examples/path/filepath.c#join -->
```c
Str dir = filepath_join_v(a, 3, BURROW_S("build"), BURROW_S("tests/../lib"),
                          BURROW_S("libburrow.a")); /* build/lib/libburrow.a */
Str ext = filepath_ext(dir);                        /* ".a" */
Str up = filepath_dir(a, dir);                      /* build/lib */
```

On Windows `dir` comes back as `build\lib\libburrow.a`. `filepath_to_slash` and `filepath_from_slash` convert between the two forms, and do nothing outside Windows.

`filepath_rel` has no counterpart in `path`. It finds the relative path that leads from one directory to another, and fails when there is none, for example when one path is absolute and the other is not:

<!-- example: ../examples/path/filepath.c#rel -->
```c
Error err;
Str rel =
    filepath_rel(a, BURROW_S("/srv/www"), BURROW_S("/srv/www/static/site.css"),
                 &err); /* static/site.css */
Str out = filepath_rel(a, BURROW_S("/srv/www"), BURROW_S("www"), &err);
if (BURROW_FAILED(err))
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
```

When a path comes from someone else, such as a name in an archive or part of a URL, `filepath_is_local` tells you whether it stays inside the directory you will open it in: it is not empty, not absolute, does not climb out with `..`, and on Windows does not name a device such as `NUL` or `COM1`. `filepath_localize` goes the other way, turning a slash separated name of the kind `io/fs` uses into one for this system, and fails on a name that cannot be said here:

<!-- example: ../examples/path/filepath.c#local -->
```c
bool inside = filepath_is_local(BURROW_S("uploads/a.png")); /* true */
bool escapes = filepath_is_local(BURROW_S("a/../../etc"));  /* false */
Str name = filepath_localize(a, BURROW_S("uploads/a.png"), &err);
```

Both are lexical, so a symbolic link inside the directory can still lead out of it. `filepath_match` is `path_match` with the system's separator, and on Windows a backslash in a pattern is a separator rather than a quote. It has its own error, `filepath_err_bad_pattern`, as Go's has.

## Touching the file system

Five functions look at the disk rather than the string. `filepath_walk_dir` visits every file and directory under a root in lexical order and hands each one to your callback as an `FsDirEntry`. Return `filepath_skip_dir` to leave a directory out, or `filepath_skip_all` to stop the walk early without it counting as an error:

<!-- example: ../examples/path/filepath_walk.c#walk -->
```c
/* Prints each name under the root with slashes, and leaves out .git. */
static Error list(void *env, Str path, FsDirEntry d, Error err) {
    Lister *l = env;
    if (BURROW_FAILED(err))
        return err;
    if (d.vt->is_dir(d.data) && str_eq(d.vt->name(d.data), BURROW_S(".git")))
        return filepath_skip_dir;
    Str rel = filepath_rel(l->a, l->root, path, &err);
    Str s = filepath_to_slash(l->a, rel);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
    return BURROW_NO_ERROR;
}
```

`filepath_walk` is the older form that hands over an `FsFileInfo` instead, which costs an lstat for every name. Neither follows symbolic links. `filepath_glob` matches a pattern against the names on disk and returns them sorted, or nothing at all if none match:

<!-- example: ../examples/path/filepath_walk.c#glob -->
```c
Slice md = filepath_glob(a, filepath_join_v(a, 2, root, BURROW_S("*.md")), &err);
for (Int i = 0; i < md.len; i++) {
    Str base = filepath_base(*(Str *)slice_at(md, i));
    printf("%.*s\n", (int)base.len,
           (const char *)base.p); /* CHANGES.md, README.md */
}
```

`filepath_abs` joins a relative path onto the working directory and cleans the result, and `filepath_eval_symlinks` goes further and resolves every link along the way, so the path it returns names the same file with no links in it:

<!-- example: ../examples/path/filepath_walk.c#abs -->
```c
Str abs = filepath_abs(a, BURROW_S("docs/../README.md"), &err);
bool ok = BURROW_OK(err) && filepath_is_abs(abs); /* true, and abs is clean */
Str real = filepath_eval_symlinks(a, root, &err); /* root with no links in it */
```

## See also

- [strings.md](strings.md), for splitting and trimming in general.
