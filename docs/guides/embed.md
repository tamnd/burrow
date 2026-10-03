# Embedding files

`burrow/embed.h` is Go's `embed` package. In Go you put a `//go:embed` comment above a variable and the compiler fills the variable in. C has nothing like that before C23's `#embed`, and `#embed` only gives you the bytes of one file, not a tree of files with names. So here a macro declares the variable and `burrow-gen embed` writes its definition into a second C file that you build along with the first.

## Declaring the variables

There are three macros, one for each kind of variable Go can embed into:

<!-- example: ../examples/embed/embed.c#declare -->
```c
BURROW_EMBED_FILE(motd, "motd.txt");
BURROW_EMBED_FS(site, "static");
```

`BURROW_EMBED_FILE` declares an `extern const Str`, which is Go's `string`. `BURROW_EMBED_BYTES` declares a `Slice` of bytes, which is Go's `[]byte`. Like Go's, that one can be written to, and every copy of the `Slice` shares the same bytes. `BURROW_EMBED_FS` declares an `extern const EmbedFS`, which is Go's `embed.FS`.

The compiler ignores the patterns, because they are only there for the generator. Run it on the source file and build what it writes:

```sh
tools/burrow-gen embed server.c -o server_embed.c
cc server.c server_embed.c burrow.c -o server
```

Once both files are built, the variables are just globals:

<!-- example: ../examples/embed/embed.c#string -->
```c
printf(BURROW_STR_FMT, BURROW_STR_ARG(motd));
```

## Patterns

Patterns work the way they do in `//go:embed`, and `burrow-gen embed` checks them the way `go build` does, with the same error messages:

| Pattern | What it takes |
|---|---|
| `motd.txt` | that file |
| `static` | every file under `static`, except names starting with `.` or `_` |
| `all:static` | every file under `static`, including those |
| `static/*` | everything in `static` that the glob matches, hidden names included |
| `"my file.txt"` | a quoted name, for one with a space in it |

Patterns are relative to the source file's directory, or to `--dir` if you pass one. They can't contain `..`, and they can't start or end with a slash. A single string can hold several patterns separated by spaces, and the macro can take several strings. A `Str` or a `Slice` has to come out to exactly one file. A pattern that matches nothing is an error, and so is a directory with nothing in it to embed. Names that Go refuses are errors too, such as `aux.txt`, which is a reserved name on Windows.

The generator writes each file's bytes twice. One copy uses `#embed`, for a C23 compiler that has it. The other is a plain array, for every other compiler. Pick one with `--mode embed` or `--mode array`. `--deps FILE` writes a make rule that lists the files it read, so a build can regenerate when they change.

## The file system

`embed_fs_as_fs` turns an `EmbedFS` into an `Fs`, so everything in the io guide works on it:

<!-- example: ../examples/embed/embed.c#fs -->
```c
Fs fsys = embed_fs_as_fs(&site);
Slice page = fs_read_file(a, fsys, BURROW_S("static/index.html"), &err);
printf("%.*s", (int)page.len, (const char *)page.p);
```

The names inside it keep the path from the pattern, so the files from `static` are under `static/`. The walk shows that, and it also shows that `static/.draft.html` was left out:

<!-- example: ../examples/embed/embed.c#walk -->
```c
err = fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, show, NULL));
```

To serve the directory as if it were the root, use `fs_sub` on it, the same as `fs.Sub` in Go:

<!-- example: ../examples/embed/embed.c#sub -->
```c
Fs root = fs_sub(a, fsys, BURROW_S("static"), &err);
Slice css = fs_read_file(a, root, BURROW_S("style.css"), &err);
printf("%.*s", (int)css.len, (const char *)css.p);
```

Files report a mode of `0444`, directories `dr-xr-xr-x`, and the modification time is the zero time, as in Go. An opened file can `Seek` and `ReadAt` through its type's method set, which is where `iotest_test_reader` and `fstest_test_fs` look for them. `embed_fs_read_file` returns a copy, so the caller can't write into the embedded bytes. The zero `EmbedFS` is an empty file system that still has a `.` you can open.

## Differences from Go

In Go the go command and the compiler do the work. Here a generator does it, and you have to run it as a step in your build. Go's rule that a pattern can't reach into another module has no C equivalent, so it isn't checked. Go's `embed.FS` is a struct holding a pointer to its file list. `EmbedFS` holds the list and its length directly, and the generator writes the list sorted the way the lookups need it. An `EmbedFS` written by hand has to be sorted the same way.
