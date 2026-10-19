# Profiling

`burrow/runtime/pprof.h` is Go's `runtime/pprof`. So far it has profiling labels, profiles of your own, the goroutine and threadcreate profiles, and the writer for the profile format that `go tool pprof` reads. The CPU, heap, allocs, block and mutex profiles are not ported yet.

## Labels

A label is a key and a value attached to a goroutine, so that samples taken while it runs can be told apart by what the goroutine was doing, for example which request it was serving. `pprof_labels_v` makes a set from pairs of strings, and `pprof_do` runs a function with them added to the context and to the goroutine. When the function returns, the goroutine gets back the labels it had before. Goroutines started inside the function inherit the labels.

<!-- example: ../examples/pprof/pprof.c#labels -->
```c
PprofLabelSet labels = pprof_labels_v(a, 4, BURROW_S("route"), BURROW_S("/users"),
                                      BURROW_S("method"), BURROW_S("GET"));
pprof_do(a, context_background(), labels, BURROW_FN(PprofDoFunc, handle, NULL));
```

The function reads them back from the context with `pprof_label`, or walks all of them with `pprof_for_labels`:

<!-- example: ../examples/pprof/pprof.c#do -->
```c
static void handle(void *env, Context ctx) {
    (void)env;
    Str route;
    if (pprof_label(ctx, BURROW_S("route"), &route))
        fmt_printf_v("handling %s\n", route);
}
```

`pprof_with_labels` and `pprof_set_goroutine_labels` are the two halves of `pprof_do`, for when the labels should outlive one call. The context and the merged set are allocated from the allocator you pass and never freed by the package, because a goroutine can keep its labels after the context has gone. Pass an allocator that outlives every goroutine that might hold them.

## Profiles of your own

A profile is a set of values, each with the stack that added it. Go's documentation gives the most common use as tracking resources that must be closed, such as files or network connections. Add the resource when it is opened and remove it when it is closed, and the profile shows where every one still open came from.

<!-- example: ../examples/pprof/pprof.c#open -->
```c
/* Every open connection is in the profile, under the stack that opened it. */
static PprofProfile *conns;

static void open_conn(Int *id) {
    pprof_profile_add(conns, BURROW_ANY(TYPE_INT, id), 0);
}

static void close_conn(Int *id) {
    pprof_profile_remove(conns, BURROW_ANY(TYPE_INT, id));
}
```

`pprof_new_profile` registers the name, and a second profile with the same name panics, as in Go. The value is copied when it is added, so it is the value and not the pointer that identifies it, and adding a value that is already there panics. `pprof_profile_remove` takes the same value.

<!-- example: ../examples/pprof/pprof.c#profile -->
```c
conns = pprof_new_profile(BURROW_S("example.com/conns"));
Int ids[] = {1, 2, 3};
for (int i = 0; i < 3; i++)
    open_conn(&ids[i]);
close_conn(&ids[1]);
fmt_printf_v("%s has %d\n", pprof_profile_name(conns), pprof_profile_count(conns));

StringsBuilder b = STRINGS_BUILDER(a);
Error err = pprof_profile_write_to(conns, strings_builder_as_io_writer(&b), 1);
if (BURROW_FAILED(err))
    return 1;
Str text = strings_builder_string(&b);
fmt_printf_v("%s\n", strings_cut(text, BURROW_S("\n"), NULL, NULL));
```

That prints:

```text
example.com/conns has 2
example.com/conns profile: total 2
```

`pprof_profile_write_to` with debug 1 writes the text form, one record per distinct stack with how many values it holds, followed by the stack. A frame is shown by name only when burrow's symbol table has one, which covers the library and not your program, so most frames in your own code come out as bare addresses. With debug 0 it writes the gzipped protocol buffer that `go tool pprof` reads.

## The goroutine profile

`pprof_lookup` finds the profiles the runtime fills in by name, the same way it finds your own. "goroutine" has a stack for every goroutine that exists when it is written, and "threadcreate" has the stack that started each OS thread the scheduler made. Both are built in, so `pprof_profile_add` panics on them.

<!-- example: ../examples/pprof/pprof.c#goroutines -->
```c
/* Three goroutines wait on a channel while the one that started them looks. */
static void wait_on(void *env) {
    (void)chan_recv((Chan *)env, NULL);
}

static void look(void *env) {
    Alloc *a = (Alloc *)env;
    Chan *c = chan_make(a, TYPE_INT, 0);
    SyncWaitGroup wg = {0};
    for (int i = 0; i < 3; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, wait_on, c));

    PprofProfile *p = pprof_lookup(BURROW_S("goroutine"));
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)pprof_profile_write_to(p, strings_builder_as_io_writer(&b), 1);
    Str text = strings_builder_string(&b);
    fmt_printf_v("%s\n", strings_cut(text, BURROW_S("\n"), NULL, NULL));
    chan_close(c);
    sync_wait_group_wait(&wg);
}
```

Goroutines run inside the scheduler, so `main` hands the work to `runtime_main`:

<!-- example: ../examples/pprof/pprof.c#main -->
```c
runtime_main(BURROW_FN(Func, look, a));
```

That counts four goroutines, the one that wrote the profile and the three it started:

```text
goroutine profile: total 4
```

The stack of a goroutine that is waiting is read from where it stopped, on amd64 and arm64 outside Windows. On other systems, and for a goroutine still running after the profile has waited twenty milliseconds for it to stop, the stack is the function the goroutine was started with. With debug 2 the goroutine profile is what `runtime_stack` prints with all set, which reads the stacks the same way.
