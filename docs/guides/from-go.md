# Coming from Go

You know Go and you want to write C with it. Most of what you know carries over without change. The packages are Go's, ported from Go's sources, with Go's names in a C spelling, Go's tests and Go's error strings. `strconv_atoi("12a")` fails with `strconv.Atoi: parsing "12a": invalid syntax`, the same text Go prints.

This page is the short list of what does not carry over, then the rules for turning a Go name into a C one, then how results, generics, interfaces and goroutines look on this side.

## Five things that aren't Go

### Nothing is collected

There is no garbage collector. Every function that can allocate takes an allocator as its first parameter, and you decide when the memory goes back. Most programs make one arena, allocate from it freely and free it once:

<!-- example: ../examples/allocators/alloc.c#arena -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
/* allocate from a as much as you like */
arena_free(&ar);
```

That one is the difference that changes how you write code, and [allocators](allocators.md) is the one guide worth reading before anything else.

### defer belongs to a block

Go runs a deferred call when the function returns. `BURROW_DEFER` runs it when control leaves the `BURROW_SCOPE` block it sits in, by any way out of it:

<!-- example: ../examples/defer/handle.c#handle -->
```c
static Error count_lines(const char *path, Int *lines) {
    BURROW_SCOPE {
        FILE *f = open_file(path);
        if (f == NULL)
            return err_cannot_open;
        BURROW_DEFER(close_file, f);

        for (int c; (c = fgetc(f)) != EOF;)
            if (c == '\n')
                ++*lines;
    }
    BURROW_SCOPE_END;

    return BURROW_NO_ERROR;
}
```

Put the scope inside a loop body and each pass closes what it opened, where the same loop in Go holds everything open until the function returns. Put it around the whole function body, as here, and you have Go's rule. [defer](defer.md) has the rest.

### A map that grows while you range over it stops the program

Deleting during a range is fine, and so is an insert that fits. An insert that makes the table grow while an iterator is live moves every entry, and `map_next` stops the program with `map grew during iteration`. Go keeps the old table alive for the iterator, which a collector makes cheap and C does not. The order of what you would have seen is unspecified in Go too, so the fix is the same in both: collect the new keys and insert them after the loop. [maps](maps.md) says more.

### reflect and encoding/json only see types you declared

C has no type information at run time, so a struct that `reflect` or `json_marshal` should see is declared with `BURROW_STRUCT`, which lists its fields with their Go types and tags:

<!-- example: ../examples/fromgo/fromgo.c#struct -->
```c
#define ACCOUNT_FIELDS(F, T)                                                           \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Age, "json:\"age,omitempty\"")
BURROW_STRUCT(Account, ACCOUNT_FIELDS);
```

After that it marshals the way the Go struct would, tags and all:

<!-- example: ../examples/fromgo/fromgo.c#json -->
```c
Account u = {S("gopher"), 0};
Slice out = json_marshal(a, ANY(TYPE_OF(Account), &u), &err);
fmt_println_v(str_from_bytes(out.p, out.len));
```

That prints `{"name":"gopher"}`. A plain C struct that was never declared is something `reflect` cannot see, and it tells you so with an error rather than guessing. [reflect](reflect.md) and [encoding](encoding.md) have the details.

### Weak pointers follow the allocator

`weak_pointer_value` reads `NULL` once the memory has gone, and when that happens depends on the allocator. Under the gc allocator it is when nothing reaches the memory any more, as in Go. Under the others it is when the memory is given back: a `mem_free`, or an `arena_reset` or `arena_free` for everything in the arena. [weak](weak.md) has a table, one row per allocator.

Two rarer ones are worth knowing about. A `sync_map_range` callback must not block, and `plugin_open` loads any shared library and checks no types. [The ledger](../ledger.md) lists every place burrow differs from Go and says why.

## Names

A Go name becomes its package name, an underscore and the name in snake case. Types are CamelCase with the package name in front, unless the type is named after its package. Constants are upper case. A method's name has its type's name in front, package and all.

| Go | burrow |
|---|---|
| `strconv.Atoi` | `strconv_atoi` |
| `net.ParseIP` | `net_parse_ip` |
| `crypto/rand.Read` | `crypto_rand_read` |
| `strings.Builder` | `StringsBuilder` |
| `(*strings.Builder).WriteString` | `strings_builder_write_string` |
| `url.URL` | `Url` |
| `time.Second` | `TIME_SECOND` |
| `http.StatusNotFound` | `HTTP_STATUS_NOT_FOUND` |
| `io.EOF` | `io_eof` |
| `os.ErrNotExist` | `os_err_not_exist` |
| `err.Error()` | `error_text(err)` |

A package whose last path element would collide with another keeps more of its path, so `crypto/rand` is `crypto_rand_`, `math/rand` is `math_rand_` and `text/template` is `text_template_`. Each header says at the top which Go package it is. With a Go tree to read, `tools/burrow-coverage -v PKG` lists every declaration in a Go package with its C name, and `--missing` lists only the ones burrow doesn't have.

`#define BURROW_SHORT 1` before the include turns on the short macro names this page uses, `S`, `ANY`, `FAILED`, `NO_ERROR` and the rest. Without it they are `BURROW_S`, `BURROW_ANY` and so on.

## Results and errors

The first result comes back. Every other result is a pointer at the end of the parameter list, the error last, and any of them can be `NULL` when you don't want it:

<!-- example: ../examples/fromgo/fromgo.c#results -->
```c
Error err = NO_ERROR;
Int n = strconv_atoi(S("12a"), &err);
if (FAILED(err))
    fmt_println_v(err);
n = strconv_atoi(S("42"), NULL);
fmt_println_v(n);
```

So `before, after, found := strings.Cut(s, "=")` is:

<!-- example: ../examples/fromgo/fromgo.c#cut -->
```c
Str after;
bool found;
Str before = strings_cut(S("user=gopher"), S("="), &after, &found);
fmt_println_v(before, after, found);
```

A function with nothing else to return returns its `Error`. One that can't fail in Go but has to allocate here returns `bool`, `true` when the work got done. [Zero values and multiple results](conventions.md) has the whole rule, and [errors](errors.md) and [failure](failure.md) say what an `Error` is and when you get a panic instead.

## Generics

A `Slice` carries the type of its elements, so the functions of Go's `slices` package are ordinary functions that read it from there:

<!-- example: ../examples/fromgo/fromgo.c#generic -->
```c
Int xs[] = {3, 1, 2};
Slice s = slice_from(xs, 3, 3, TYPE_INT);
slices_sort(s);
Int two = 2;
fmt_println_v(s, slices_index(s, &two));
```

That prints `[1 2 3] 1`. Values go in by pointer, as `&two` does here, because a C function can't take an argument of any type. A function that needs an ordered type, such as `slices_sort`, panics when it is handed a slice of something that isn't one, where Go would refuse to compile. `cmp_compare`, `cmp_less` and `cmp_or` are macros that pick the right comparison for the type you give them. [slices](slices.md) and [types](types.md) go further.

## Interfaces

An interface value is a pointer to the thing and a table of its methods. A type that satisfies `io.Reader` has a function that gives you one, named for the type and the interface:

<!-- example: ../examples/fromgo/fromgo.c#iface -->
```c
StringsReader *r = strings_new_reader(a, S("hello, reader\n"));
io_copy(a, os_file_as_io_writer(os_stdout), strings_reader_as_io_reader(r), &err);
```

Your own types get the same with a table of your functions. [interfaces](interfaces.md) shows how.

## Goroutines and channels

They are Go's. `runtime_main` starts the scheduler and runs your first goroutine, `go` starts another, and channels, `select`, `SyncMutex` and `SyncWaitGroup` work the way Go's do. The one thing to add is the `runtime_main` call in your C `main`, because there is no Go toolchain here to arrange it. [goroutines](goroutines.md) and [channels](channels.md) have it all.
