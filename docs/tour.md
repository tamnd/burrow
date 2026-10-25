# A tour of burrow

burrow is Go's standard library written in C. This page is four small programs, each a whole file that compiles and runs as it is, and each a thing you would do in your first hour with Go: read a file and count its words, read JSON and write it back out, split work over three goroutines, and serve HTTP. If these read the way you hoped, the rest of the library will too. If they don't, this is the cheapest place to find out.

Every program here is a file under [docs/examples/tour](examples/tour), and CI builds and runs each one on every pull request and checks that it prints what this page says it prints.

To build one, take the two files from a release, `burrow.c` and `burrow.h`, and compile them with the program:

```sh
cc -std=c11 -O2 -o words words.c burrow.c -pthread
```

On Windows with MinGW, link `-lws2_32` instead of `-pthread`. [The building guide](guides/building.md) has the rest, including CMake, MSVC, and how to leave out the packages you don't use. The programs below include `burrow/burrow.h`, which is the spelling for a checkout, where `make` builds `build/libburrow.a` and the command is `cc -std=c11 -O2 -Iinclude -o words words.c build/libburrow.a -pthread`. With the release pair, that line is `#include "burrow.h"`.

## Counting words

<!-- example: examples/tour/words.c#words -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = os_write_file(S("poem.txt"),
                              BURROW_B("the cat saw the dog\nand the dog ran\n"), 0644);

    Slice data = os_read_file(a, S("poem.txt"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Slice words = strings_fields(a, str_from_bytes(data.p, data.len));
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
    for (Int i = 0; i < words.len; i++) {
        Int *n = MAP_GET(Str, Int, counts, AT(Str, words, i));
        MAP_SET(Str, Int, counts, AT(Str, words, i), n != NULL ? *n + 1 : 1);
    }
    fmt_printf_v("%d words: %v\n", words.len, counts);
    arena_free(&ar);
}
```

The first thing to see is the arena. Every function in burrow that can allocate takes an allocator first, `os_read_file` and `strings_fields` and `map_make` included, and here they all get the same one. Nothing in the program frees anything on its own. `arena_free` at the end gives it all back at once. That is the one idea in burrow you need before anything else, and [the allocators guide](guides/allocators.md) is the page to read about it.

The second is the error. `os_write_file` returns its `Error`, as Go's `os.WriteFile` does, and `os_read_file` writes one through its last argument, since it also has the data to return. A zero `Error` means it worked, and `FAILED` asks whether it didn't. Printing one with `fmt_println_v` prints Go's message, such as `open poem.txt: no such file or directory`.

`Str` is Go's string, a pointer and a length, and `Slice` is Go's slice. `S("...")` makes a `Str` from a literal and costs nothing. `strings_fields` is Go's `strings.Fields`, which splits on runs of white space. `MAP_GET` hands back a pointer to the value, or `NULL` when the key isn't there, which is how C says what Go says with `v, ok := m[k]`. `fmt` prints a map the way Go does, in key order, which is why the output comes out sorted with no sort in the program.

`#define BURROW_SHORT 1` turns on the short macro names, `S`, `AT`, `FAILED` and the rest. Without it they are `BURROW_S`, `BURROW_AT` and so on, because a library that defines `S` in every file that includes it will break somebody's program. The functions have no prefix at all. Their package name is already one.

## Reading and writing JSON

<!-- example: examples/tour/json.c#json -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

BURROW_SLICE_TYPE(Strs, Str);
#define REPO_FIELDS(F, T)                                                              \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Stars, "json:\"stars\"")                                                 \
    F(T, Strs, Topics, "json:\"topics\"")
BURROW_STRUCT(Repo, REPO_FIELDS);

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Repo r = {0};
    Error err = json_unmarshal(
        a, BURROW_B("{\"name\": \"burrow\", \"stars\": 41, \"topics\": [\"c\"]}"),
        ANY(TYPE_OF(Repo), &r));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    r.Stars++;
    r.Topics = APPEND(Str, a, r.Topics, S("go"));
    Slice out = json_marshal_indent(a, ANY(TYPE_OF(Repo), &r), S(""), S("  "), &err);
    fmt_println_v(str_from_bytes(out.p, out.len));
    arena_free(&ar);
}
```

C has no struct tags and no reflection, so `BURROW_STRUCT` gives it both. It takes the list of fields once and writes the struct and the type descriptor that `encoding/json`, `fmt` and `reflect` read, so the two can't drift apart. The third column is the tag, in Go's syntax, and `json_unmarshal` reads it the way Go does: `name` in the JSON goes in `Name`, unknown keys are skipped, and a field with no key stays as it was.

`ANY(TYPE_OF(Repo), &r)` is Go's `any`, a value and its type, which is what `json.Unmarshal(data, &r)` passes without you seeing it. `APPEND` is Go's `append`, and it takes the allocator for the same reason everything else does. The rest is Go's API with the dots turned into underscores: `json_marshal_indent` is `json.MarshalIndent`, with the same arguments and the same output.

[The types guide](guides/types.md) covers `BURROW_STRUCT` and descriptors, and [the encoding guide](guides/encoding.md) has JSON, XML, gob and the rest.

## Three goroutines and a channel

<!-- example: examples/tour/goroutines.c#goroutines -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static Chan *sums;

static void add(void *env) {
    Int from = (Int)(intptr_t)env, sum = 0;
    for (Int i = from; i < from + 100; i++)
        sum += i;
    chan_send(sums, &sum);
}

static void run(void *env) {
    sums = chan_make(heap_allocator(), TYPE_INT, 0);
    for (Int from = 1; from <= 300; from += 100)
        go(FN(Func, add, (void *)(intptr_t)from));

    Int total = 0, sum;
    for (int i = 0; i < 3; i++) {
        chan_recv(sums, &sum);
        total += sum;
    }
    fmt_println_v("1 + 2 + ... + 300 =", total);
    chan_free(sums);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

A goroutine is a function and a pointer for it to work on, and `go` starts one. They are Go's goroutines, not threads: each has a small stack of its own, and the scheduler runs them on as many threads as there are cores and moves them between threads as they block.

`runtime_main` starts the scheduler and runs `run` as the main goroutine. When `run` returns, the program is over, the way it is in Go when `main` returns.

The channel is unbuffered, because the last argument to `chan_make` is zero, so each `chan_send` waits until the main goroutine is there to take the value. The three sums arrive in whatever order the goroutines finish, and the total doesn't care. `select`, buffered channels, `close` and ranging over a channel are all there too, in [the goroutines guide](guides/goroutines.md) and [the channels guide](guides/channels.md).

## Serving HTTP

<!-- example: examples/tour/http.c#http -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello, %s\n",
                  http_request_path_value(r, S("name")));
}

static void run(void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    http_handle_func(S("GET /hello/{name}"), FN(HttpHandlerFunc, hello, NULL));
    HttptestServer *ts = httptest_new_server(heap_allocator(), (HttpHandler){0});
    Error err = NO_ERROR;
    Str url = fmt_sprintf_v(a, "%s/hello/gopher", ts->url);
    HttpResponse *res = http_client_get(httptest_server_client(ts), url, &err);
    if (res != NULL) {
        Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("%s: %s", res->status, str_from_bytes(body.p, body.len));
        http_response_free(res);
    }
    httptest_server_free(ts);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

This is `net/http`, Go's server and Go's client. `http_handle_func` registers a handler on the default mux, with the method and the `{name}` wildcard from Go 1.22's patterns, and `http_request_path_value` reads the wildcard back. The handler writes its response through an `IoWriter`, the same interface a file or a buffer gives, so `fmt_fprintf_v` works on it unchanged.

A real server would end with `http_listen_and_serve(S(":8080"), (HttpHandler){0})` and stay up. This program has to stop so that CI can check what it printed, so it uses `httptest`, which serves on a port of 127.0.0.1, and then asks the server for a page with the client `httptest` hands it. The server is the real one either way, each connection on a goroutine of its own, with keep-alive, and HTTP/2 and TLS when they are asked for.

[The net guide](guides/net.md) has servers, clients, TLS, cookies, reverse proxies and the rest.

## What's next

The [README](../README.md) has a section for each part of the core: numbers, strings, slices, interfaces, errors, maps, goroutines and channels. Each guide in [docs/guides](guides) is one package or one idea, with programs that CI runs the same way it runs these. If you know Go, most of what you need is the naming rule: the Go name with the dot turned into an underscore and the case changed, so `strings.Contains` is `strings_contains`, `strings.Builder` is `StringsBuilder` and `io.EOF` is `io_eof`.
