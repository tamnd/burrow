# Public variables

`burrow/expvar.h` is Go's `expvar` package. A program publishes counters and other values under names, and a handler serves all of them as one JSON object, by default at `/debug/vars`. Each variable is updated atomically, so the code that counts and the handler that reads never share a lock.

## Publishing variables

`expvar_new_int`, `expvar_new_float`, `expvar_new_string` and `expvar_new_map` each make a variable and publish it in one call. A published variable lives as long as the program, as in Go, and a second variable with the same name panics. A map's keys come out sorted, and `expvar_map_add` makes the counter for a key the first time it sees it:

<!-- example: ../examples/expvar/expvar.c#publish -->
```c
ExpvarInt *requests = expvar_new_int(BURROW_S("requests"));
ExpvarMap *by_path = expvar_new_map(BURROW_S("by_path"));
ExpvarString *version = expvar_new_string(BURROW_S("version"));
if (requests == NULL || by_path == NULL || version == NULL ||
    !expvar_string_set(version, BURROW_S("1.4.2")))
    return 1;

const char *paths[] = {"/", "/about", "/", "/"};
for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    expvar_int_add(requests, 1);
    expvar_map_add(by_path, str_from_cstr(paths[i]), 1);
}
fmt_printf_v("requests %s\n", expvar_int_string(requests, a));
fmt_printf_v("by_path %s\n", expvar_map_string(by_path, a));
```

That prints:

```text
requests 4
by_path {"/": 3, "/about": 1}
```

Every variable has a string form that is valid JSON. A `String` comes out quoted, with `<`, `>` and `&` escaped the way `encoding/json` escapes them, so the output is safe to put in a page.

## Values worked out on demand

An `ExpvarFunc` is a function whose result is marshalled with `encoding/json` each time the variables are read. It returns an `Any`, made from the allocator it is given:

<!-- example: ../examples/expvar/expvar.c#func -->
```c
/* How many jobs are waiting, worked out again each time the variables are
 * read. */
static Int queued = 3;

static Any queue_depth(void *env, Alloc *a) {
    (void)env;
    Int *n = BURROW_NEW(a, Int);
    if (n == NULL)
        return (Any){NULL, NULL};
    *n = queued;
    return (Any){TYPE_INT, n};
}
```

`expvar_func_as_var` points at the `ExpvarFunc`, so it has to stay alive as long as it is published. `expvar_publish` puts any `ExpvarVar` under a name, which is also how a type of your own gets published: give it an `ExpvarVarVT` whose `string` returns JSON.

<!-- example: ../examples/expvar/expvar.c#publish-func -->
```c
ExpvarFunc queue = BURROW_FN(ExpvarFunc, queue_depth, NULL);
expvar_publish(BURROW_S("queue"), expvar_func_as_var(&queue));
queued = 5;
fmt_printf_v("queue %s\n", expvar_var_string(expvar_get(BURROW_S("queue")), a));
```

That prints `queue 5`, the value at the time of the read rather than at the time of publishing.

## Serving them

In Go, importing the package puts the handler on `http.DefaultServeMux`. C has nothing that runs on import, so `expvar_register` does it, and only the first call counts. A program with its own mux mounts `expvar_handler` wherever it likes instead. Two variables are always there, as in Go: `cmdline`, the program's arguments, and `memstats`, from `runtime_read_mem_stats`.

<!-- example: ../examples/expvar/expvar.c#serve -->
```c
expvar_register();

HttpRequest *r = httptest_new_request(a, BURROW_S("GET"), BURROW_S("/debug/vars"),
                                      (IoReader){0});
HttptestResponseRecorder *rec = httptest_new_recorder(a);
http_handler_serve_http(http_serve_mux_as_handler(http_default_serve_mux),
                        httptest_response_recorder_as_response_writer(rec), r);
fmt_printf_v("%d %s\n", rec->code,
             http_header_get(rec->header_map, BURROW_S("Content-Type")));

/* cmdline and memstats change from run to run, so only their names. */
Slice body = bytes_buffer_bytes(rec->body);
Str rest = str_from_bytes(body.p, body.len);
while (rest.len > 0) {
    bool found;
    Str line = strings_cut(rest, BURROW_S("\n"), &rest, &found);
    if (strings_has_prefix(line, BURROW_S("\"cmdline\":")) ||
        strings_has_prefix(line, BURROW_S("\"memstats\":")))
        line = strings_cut(line, BURROW_S(" "), NULL, &found);
    fmt_printf_v("%s\n", line);
}
httptest_response_recorder_free(rec);
http_request_free(r);
```

With the variables from the sections above published, that prints:

```text
200 application/json; charset=utf-8
{
"by_path": {"/": 3, "/about": 1},
"cmdline":
"memstats":
"queue": 5,
"requests": 4,
"version": "1.4.2"
}
```

The pattern is `GET /debug/vars`, so other methods get 405. With `GODEBUG=httpmuxgo121=1` it is `/debug/vars` with no method, which is what Go does under that setting.

## What memstats holds

`runtime_read_mem_stats` fills Go's `runtime.MemStats` from what burrow's allocators count, which is less than Go's runtime knows. `Mallocs` and `TotalAlloc` count what `heap_allocator` has handed out while benchmark counting is on. When the collector from `burrow/mem/gc.h` is running, `Alloc`, `HeapAlloc` and `HeapInuse` are the bytes it has in use, `TotalAlloc` adds what it has handed out, `NumGC` is its collection count and `EnableGC` is true. Everything else is zero. Unlike Go's, reading the numbers does not stop the world.

## See also

- [net](net.md) for the HTTP server and `httptest`.
- [allocators](allocators.md) for `heap_allocator` and the collector.
