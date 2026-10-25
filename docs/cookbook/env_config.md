# Read settings from environment variables

`os_getenv` gives the empty string both for a variable set to nothing and for one that isn't set. When the difference matters, `os_lookup_env` says which it was, and that's what a default needs. `os_expand_env` fills in `$NAME` and `${NAME}` in a string.

<!-- example: ../examples/cookbook/env_config.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* The variable if it is set, even to nothing, and the default if not. */
static Str env_or(Alloc *a, Str key, Str def) {
    bool found = false;
    Str v = os_lookup_env(a, key, &found);
    return found ? v : def;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A real program gets these from whoever starts it. The recipe sets two of
     * them itself so it prints the same thing wherever it runs. */
    os_unsetenv(S("APP_PORT"));
    os_setenv(S("APP_HOST"), S("db.internal"));
    os_setenv(S("APP_DEBUG"), S(""));

    Str host = env_or(a, S("APP_HOST"), S("localhost"));
    Str port = env_or(a, S("APP_PORT"), S("5432"));
    fmt_printf_v("host %s, port %s\n", host, port);

    /* Getenv cannot tell a variable set to nothing from one that is not set.
     * LookupEnv can. */
    Str debug = os_getenv(a, S("APP_DEBUG"));
    bool found = false;
    os_lookup_env(a, S("APP_DEBUG"), &found);
    fmt_printf_v("APP_DEBUG is %q, set %t\n", debug, found);

    Error err = NO_ERROR;
    Int n = strconv_atoi(port, &err);
    if (FAILED(err)) {
        fmt_println_v("bad APP_PORT:", err);
        return 1;
    }
    fmt_println_v("port as a number:", n);

    /* ExpandEnv fills in $NAME and ${NAME}, and one that is not set becomes
     * the empty string. */
    Str url = os_expand_env(a, S("postgres://${APP_HOST}:5432/app$APP_MISSING"));
    fmt_println_v(url);
    arena_free(&ar);
}
```

It prints:

```text
host db.internal, port 5432
APP_DEBUG is "", set true
port as a number: 5432
postgres://db.internal:5432/app
```

The program is [docs/examples/cookbook/env_config.c](../examples/cookbook/env_config.c). CI builds and runs it and checks that it prints this.
