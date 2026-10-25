# Build a tool with subcommands

A tool like `go` or `git` takes a command as its first word, and each command has flags of its own. Give each command its own `FlagFlagSet` and hand it the words after the command name. Here the tool runs a few command lines of its own, so it needs no arguments, and the flag sets write their usage to `io_discard` so only the error shows. A real tool would pass `os_args` without the program name.

<!-- example: ../examples/cookbook/subcommands.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* greet [-name who] [-n times] [-loud] */
static Error greet(Alloc *a, Slice args) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("greet"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(fs, io_discard);
    Str *name = flag_flag_set_string(fs, S("name"), S("world"), S("who to greet"));
    Int *times = flag_flag_set_int(fs, S("n"), 1, S("how many times"));
    bool *loud = flag_flag_set_bool(fs, S("loud"), false, S("shout it"));
    Error err = flag_flag_set_parse(fs, args);
    if (OK(err)) {
        Str msg = fmt_sprintf_v(a, "hello, %s", *name);
        if (*loud)
            msg = strings_to_upper(a, msg);
        for (Int i = 0; i < *times; i++)
            fmt_println_v(msg);
    }
    flag_flag_set_free(fs);
    return err;
}

/* sum number... */
static Error sum(Slice args) {
    Int total = 0;
    for (Int i = 0; i < args.len; i++) {
        Error err = NO_ERROR;
        Int n = strconv_atoi(AT(Str, args, i), &err);
        if (FAILED(err))
            return err;
        total += n;
    }
    fmt_println_v(total);
    return NO_ERROR;
}

/* The first word picks the command, and the rest are its own arguments, the
 * way go build and git commit work. */
static void dispatch(Alloc *a, Str line) {
    Slice words = strings_fields(a, line);
    fmt_printf_v("$ tool %s\n", line);
    Str cmd = AT(Str, words, 0);
    Slice rest = slice_sub(words, 1, words.len);
    Error err = NO_ERROR;
    if (str_eq(cmd, S("greet")))
        err = greet(a, rest);
    else if (str_eq(cmd, S("sum")))
        err = sum(rest);
    else
        err = fmt_errorf_v("unknown command %q", cmd);
    if (FAILED(err))
        fmt_println_v("error:", err);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    dispatch(a, S("greet"));
    dispatch(a, S("greet -name Ada -n 2"));
    dispatch(a, S("greet -loud -name=Grace"));
    dispatch(a, S("sum 1 2 3 4"));
    dispatch(a, S("sum 1 two"));
    dispatch(a, S("greet -n lots"));
    dispatch(a, S("deploy"));
    arena_free(&ar);
}
```

It prints:

```text
$ tool greet
hello, world
$ tool greet -name Ada -n 2
hello, Ada
hello, Ada
$ tool greet -loud -name=Grace
HELLO, GRACE
$ tool sum 1 2 3 4
10
$ tool sum 1 two
error: strconv.Atoi: parsing "two": invalid syntax
$ tool greet -n lots
error: invalid value "lots" for flag -n: parse error
$ tool deploy
error: unknown command "deploy"
```

The program is [docs/examples/cookbook/subcommands.c](../examples/cookbook/subcommands.c). CI builds and runs it and checks that it prints this.
