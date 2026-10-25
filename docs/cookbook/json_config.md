# Load a JSON config file

Read a config file into a struct, with defaults for the fields the file leaves out. The struct is declared with `BURROW_STRUCT` and Go's json tags, `json_unmarshal` fills it in, and the program checks what the file said before it uses it. The four files show a good config, one that fails the check, one with the wrong type and one that isn't there.

<!-- example: ../examples/cookbook/json_config.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

BURROW_SLICE_TYPE(Strs, Str);
#define CONFIG_FIELDS(F, T)                                                            \
    F(T, Str, Addr, "json:\"addr\"")                                                   \
    F(T, Int, Workers, "json:\"workers\"")                                             \
    F(T, bool, Debug, "json:\"debug,omitempty\"")                                      \
    F(T, Strs, Origins, "json:\"origins\"")
BURROW_STRUCT(Config, CONFIG_FIELDS);

/* Reads the config at name over the defaults in c. A field the file leaves
 * out keeps its default. */
static Error load(Alloc *a, Str name, Config *c) {
    Error err = NO_ERROR;
    Slice data = os_read_file(a, name, &err);
    if (FAILED(err))
        return err;
    err = json_unmarshal(a, data, ANY(TYPE_OF(Config), c));
    if (FAILED(err))
        return fmt_errorf_v("%s: %w", name, err);
    if (c->Workers < 1)
        return fmt_errorf_v("%s: workers is %d, and needs to be at least 1", name,
                            c->Workers);
    return NO_ERROR;
}

static void try_load(Alloc *a, Str name) {
    Config c = {.Addr = S(":8080"), .Workers = 4};
    Error err = load(a, name, &c);
    if (FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    fmt_printf_v("%+v\n", ANY(TYPE_OF(Config), &c));
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    os_write_file(S("good.json"),
                  BURROW_B("{\"workers\": 8, \"origins\": [\"example.com\"]}"), 0644);
    os_write_file(S("zero.json"), BURROW_B("{\"workers\": 0}"), 0644);
    os_write_file(S("typo.json"), BURROW_B("{\"workers\": \"eight\"}"), 0644);

    try_load(a, S("good.json"));
    try_load(a, S("zero.json"));
    try_load(a, S("typo.json"));
    try_load(a, S("none.json"));
    arena_free(&ar);
}
```

It prints:

```text
{Addr::8080 Workers:8 Debug:false Origins:[example.com]}
zero.json: workers is 0, and needs to be at least 1
typo.json: json: cannot unmarshal string into Go struct field Config.workers of type int
open none.json: no such file or directory
```

The program is [docs/examples/cookbook/json_config.c](../examples/cookbook/json_config.c). CI builds and runs it and checks that it prints this.
