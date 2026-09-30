# Command line flags

`burrow/flag.h` is Go's `flag` package. You define flags, parse a list of arguments, and then read the flags and whatever arguments are left. The syntax is the same as in Go, so a C tool built on it takes `-port 9000`, `-port=9000` and `--port=9000`, stops at the first argument that isn't a flag, and prints the usage Go programs print.

## A set of flags

A `FlagFlagSet` holds flags and parses arguments into them. Each definition names the flag, gives its default and a line of help, and either returns a pointer to storage the set owns or stores through a pointer you pass to the `_var` form:

<!-- example: ../examples/flag/flag.c#set -->
```c
FlagFlagSet *fs =
    flag_new_flag_set(NULL, BURROW_S("serve"), FLAG_CONTINUE_ON_ERROR);
Int *port = flag_flag_set_int(fs, BURROW_S("port"), 8080,
                              BURROW_S("port to listen on"));
bool verbose;
flag_flag_set_bool_var(fs, &verbose, BURROW_S("v"), false,
                       BURROW_S("log every request"));
Duration *timeout =
    flag_flag_set_duration(fs, BURROW_S("timeout"), 30 * TIME_SECOND,
                           BURROW_S("how long a request may take"));

Str argv[] = {BURROW_S("-port=9000"), BURROW_S("-v"),
              BURROW_S("--timeout"),  BURROW_S("1m30s"),
              BURROW_S("site"),       BURROW_S("-not-a-flag")};
Error err = flag_flag_set_parse(fs, slice_from(argv, 6, 6, TYPE_STRING));
if (!BURROW_OK(err))
    return 1;

printf("port=%lld v=%d timeout=%lld\n", (long long)*port, verbose,
       (long long)(*timeout / TIME_SECOND)); /* port=9000 v=1 timeout=90 */
for (Int i = 0; i < flag_flag_set_n_arg(fs); i++) {
    Str arg = flag_flag_set_arg(fs, i);
    printf("arg %.*s\n", (int)arg.len, arg.p);
}
flag_flag_set_free(fs);
```

The arguments are a `Slice` of `Str`. Parsing stops at `site`, the first argument that isn't a flag, so `-not-a-flag` after it is an argument too. `flag_flag_set_arg` and `flag_flag_set_n_arg` read what is left, and `flag_flag_set_args` hands it back as a slice.

There is a definition for every type Go has one for: `bool`, `Int`, `int64_t`, `Uint`, `uint64_t`, `Str`, `double` and `Duration`. Integers take anything `strconv_parse_int` takes with base 0, so `0x1f`, `0o17` and `1_000` work. Durations take `time_parse_duration`'s syntax, such as `1m30s`. A boolean flag is set to true by `-v` alone, and needs `-v=false` to be turned off, because `-v false` would read `false` as the next argument.

A set gets its memory from the allocator you give `flag_new_flag_set`, NULL for the heap, and `flag_flag_set_free` gives all of it back, flags and storage included. A set in memory of your own, such as a struct member, is set up with `flag_flag_set_init` and let go with `flag_flag_set_destroy`.

A string flag doesn't copy its value. After parsing, it points into the argument it came from, so the arguments have to outlive your use of the set. The program's own arguments live until it ends, which is why this rarely matters.

## The program's own arguments

The functions without `flag_set` in their names work on `flag_command_line`, the set for the program's own arguments. `flag_parse` parses them, from the one after the program's name:

<!-- example: ../examples/flag/flag.c#command-line -->
```c
Int *n = flag_int(BURROW_S("n"), 3, BURROW_S("how many times"));
flag_parse();
printf("n=%lld with %lld arguments\n", (long long)*n, (long long)flag_n_arg());
```

The library reads the arguments from the operating system, so `main` doesn't have to pass `argc` and `argv` along. On Windows they come from `GetCommandLineW`, split by the same rules Go uses there.

`flag_command_line` exits on a bad command line, the way Go's does. It prints the problem and the usage to standard error, then ends the program with status 2, or 0 for `-help` and `-h`. It flushes stdio first, since a C program may have output buffered. To change what the usage says, assign a `Func` to `flag_usage`. To parse the arguments some other way, point `flag_command_line` at a set of your own.

## Flags of your own type

A flag is a `FlagValue`, a vtable and a pointer, and any type can be one. This one is the list of durations from Go's own example:

<!-- example: ../examples/flag/flag.c#value -->
```c
/* A list of durations, set from a comma separated list, which is the
 * user-defined flag type in Go's own example. */
typedef struct Interval {
    Duration d[8];
    Int n;
} Interval;

static Str interval_string(void *self, Alloc *a) {
    Interval *iv = self;
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < iv->n; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ',');
        Byte buf[DURATION_STRING_MAX];
        Int n = duration_format(iv->d[i], buf);
        strings_builder_write(&b, slice_from(buf, n, n, TYPE_BYTE), NULL);
    }
    return strings_builder_string(&b);
}

static Error interval_set(void *self, Str value) {
    Interval *iv = self;
    if (iv->n > 0)
        return errors_new(error_allocator(), BURROW_S("interval flag already set"));
    while (value.len > 0) {
        Str rest;
        bool found;
        Str dt = strings_cut(value, BURROW_S(","), &rest, &found);
        Error err;
        Duration d = time_parse_duration(dt, &err);
        if (!BURROW_OK(err))
            return err;
        if (iv->n == 8)
            return errors_new(error_allocator(), BURROW_S("too many intervals"));
        iv->d[iv->n++] = d;
        value = rest;
    }
    return BURROW_NO_ERROR;
}

static const FlagValueVT interval_vt = {NULL, interval_string, interval_set, NULL,
                                        NULL};
```

`set` parses the text and stores it. If it fails, the error it returns is what the user sees after `invalid value`. `string` gives the value as text in the scratch allocator it's handed, and is how the set learns the default's text when the flag is defined. `get` and `is_bool_flag` are optional. A value whose `is_bool_flag` returns true can be given as `-name` alone, the way a boolean can.

`flag_flag_set_var` defines the flag, and the value's current contents are the default:

<!-- example: ../examples/flag/flag.c#use-value -->
```c
FlagFlagSet *fs =
    flag_new_flag_set(NULL, BURROW_S("events"), FLAG_CONTINUE_ON_ERROR);
Interval iv = {{0}, 0};
flag_flag_set_var(
    fs, (FlagValue){&interval_vt, &iv}, BURROW_S("deltaT"),
    BURROW_S("comma-separated list of intervals to use between events"));

Str argv[] = {BURROW_S("-deltaT"), BURROW_S("10s,1m,250ms")};
Error err = flag_flag_set_parse(fs, slice_from(argv, 2, 2, TYPE_STRING));
if (!BURROW_OK(err))
    return 1;
printf(
    "%d intervals, the last %lldms\n", (int)iv.n,
    (long long)(iv.d[2] / TIME_MILLISECOND)); /* 3 intervals, the last 250ms */
flag_flag_set_free(fs);
```

`self_type` is optional too. Give it the value's type descriptor and the usage message leaves out a default that matches a zeroed value, the way Go leaves out `0` and `""`. Without one, the default is left out only when its text is empty.

## Functions as flags

When a flag should do something each time it appears, rather than hold a value, `flag_flag_set_func` calls a function with each value given. `flag_flag_set_bool_func` is the same for a flag that needs no value:

<!-- example: ../examples/flag/flag.c#func -->
```c
static Error add_include(void *env, Str dir) {
    Int *count = env;
    printf("include %.*s\n", (int)dir.len, dir.p);
    (*count)++;
    return BURROW_NO_ERROR;
}
```

<!-- example: ../examples/flag/flag.c#use-func -->
```c
FlagFlagSet *fs =
    flag_new_flag_set(NULL, BURROW_S("cc"), FLAG_CONTINUE_ON_ERROR);
Int count = 0;
flag_flag_set_func(fs, BURROW_S("I"), BURROW_S("add a `directory` to search"),
                   BURROW_FN(FlagFunc, add_include, &count));

Str argv[] = {BURROW_S("-I"), BURROW_S("include"), BURROW_S("-I=vendor")};
(void)flag_flag_set_parse(fs, slice_from(argv, 3, 3, TYPE_STRING));
printf("%d directories\n", (int)count);
flag_flag_set_free(fs);
```

`flag_flag_set_text_var` is the third way. It makes a flag of any type with `UnmarshalText` and `MarshalText` methods, such as `NetipAddr`, and finds the methods through the type descriptor the way `encoding` does.

## Errors and usage

A set made with `FLAG_CONTINUE_ON_ERROR` returns the error. It also prints the error and the usage to its output first, which is standard error unless you set another. This one writes to standard output through a small writer:

<!-- example: ../examples/flag/flag.c#stdout -->
```c
/* Standard output as an IoWriter, for a set whose messages should go there
 * rather than to standard error. */
static Int stdout_write(void *self, Slice p, Error *err) {
    (void)self;
    if (err)
        *err = BURROW_NO_ERROR;
    return (Int)fwrite(p.p, 1, (size_t)p.len, stdout);
}

static const IoWriterVT stdout_vt = {NULL, stdout_write};
static const IoWriter to_stdout = {&stdout_vt, NULL};
```

<!-- example: ../examples/flag/flag.c#errors -->
```c
FlagFlagSet *fs =
    flag_new_flag_set(NULL, BURROW_S("serve"), FLAG_CONTINUE_ON_ERROR);
flag_flag_set_set_output(fs, to_stdout);
flag_flag_set_int(fs, BURROW_S("port"), 8080, BURROW_S("port to listen on"));
flag_flag_set_string(fs, BURROW_S("root"), BURROW_S("."),
                     BURROW_S("serve files from `dir`"));

Str argv[] = {BURROW_S("-port=http")};
Error err = flag_flag_set_parse(fs, slice_from(argv, 1, 1, TYPE_STRING));
Str msg = error_text(err);
printf("err: %.*s\n", (int)msg.len, msg.p);
flag_flag_set_free(fs);
```

That prints the same message and the same usage Go prints:

<!-- not compiled: what the program above prints -->
```text
invalid value "http" for flag -port: parse error
Usage of serve:
  -port int
    	port to listen on (default 8080)
  -root dir
    	serve files from dir (default ".")
err: invalid value "http" for flag -port: parse error
```

The word after the flag's name comes from the help text when part of it is in back quotes, like `dir` here, and otherwise from the flag's type. `flag_unquote_usage` does that on its own, if you print usage in a layout of your own. `FLAG_PANIC_ON_ERROR` panics with the error instead, and `FLAG_EXIT_ON_ERROR` behaves the way `flag_command_line` does.

Mistakes in defining flags are bugs in the program rather than bad input, and panic as they do in Go. That covers a name that starts with `-` or contains `=`, a name defined twice in one set, and a name given to `flag_flag_set_set` before it was defined. The last one's message says which file and line did the setting.

## Threads

A set isn't safe to use from two threads at once, just as in Go. Define and parse on one thread before starting any others, and only read afterwards.
