# Profiling

`burrow/runtime/pprof.h` is Go's `runtime/pprof`. So far it has profiling labels, profiles of your own, the goroutine, threadcreate, heap, allocs, block and mutex profiles, the CPU profile, and the writer for the profile format that `go tool pprof` reads.

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

## The CPU profile

`pprof_start_cpu_profile` samples the stacks of whatever the program is running a hundred times a second of CPU time it uses, and `pprof_stop_cpu_profile` stops it and writes the samples to the writer as the gzipped protocol buffer that `go tool pprof` reads. The profile is written by a goroutine, so both have to be called from inside `runtime_main`.

<!-- example: ../examples/pprof/pprof.c#cpu -->
```c
/* Something to sample: a loop that keeps the CPU busy for d. */
static Int spin(Duration d) {
    Time start = time_now();
    Int n = 0;
    while (time_since(start) < d)
        n++;
    return n;
}

static void profile_cpu(void *env) {
    Alloc *a = (Alloc *)env;
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = pprof_start_cpu_profile(bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        return;
    (void)spin(200 * TIME_MILLISECOND);
    pprof_stop_cpu_profile();
    fmt_printf_v("cpu profile written: %t\n", (bool)(bytes_buffer_len(&buf) > 0));
}
```

That prints:

```text
cpu profile written: true
```

A real program would write to a file from `os_create` and give the file to `go tool pprof`. A second `pprof_start_cpu_profile` while one is running returns an error, as in Go.

The samples come from SIGPROF, sent by setitimer's ITIMER_PROF timer. The handler is installed the first time a profile starts and stays installed, so a program with its own SIGPROF handler loses it. Each sample is the stack of the goroutine the signal interrupted, walked from where it stopped. That walk needs frame pointers, and it works on amd64, arm64 and 386. On other machines the handler can't read where the thread stopped, so every sample is a single frame, `burrow__prof_system`. A sample taken while a thread was in the scheduler, or on a thread that runs no goroutine, has the address it stopped at and `burrow__prof_system` or `burrow__prof_external_code` under it, the way Go uses `runtime._System` and `runtime._ExternalCode`. Windows and wasip1 have no SIGPROF, so a profile there is written with no samples in it. Names come from burrow's symbol table, which covers the library and not your program, so `go tool pprof` shows your own functions as addresses.

## The heap profile

"heap" has the allocations made from `heap_allocator` that are still in use, and "allocs" has every one made since the program started. They are the same profile, sampled the same way, and differ only in which numbers `go tool pprof` shows first. One allocation in every `runtime_mem_profile_rate` bytes is sampled, 512 KiB by default. The rate should be set once, as early as the program can, since the tools take it to have been the same all along. This example sets it to 1 at the top of `main`, to keep every allocation:

<!-- example: ../examples/pprof/pprof.c#rate -->
```c
runtime_set_mem_profile_rate(1);
```

Then it makes sixty-four buffers from one place and writes the heap profile in its text form, debug 1:

<!-- example: ../examples/pprof/pprof.c#heap -->
```c
/* Sixty-four buffers that stay in use, made at one place in the program. */
static void *buffers[64];

static void fill(void) {
    for (int i = 0; i < 64; i++)
        buffers[i] = mem_alloc(heap_allocator(), 4096, 1);
}

static bool profile_heap(Alloc *a) {
    fill();
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = pprof_profile_write_to(pprof_lookup(BURROW_S("heap")),
                                       strings_builder_as_io_writer(&b), 1);
    if (BURROW_FAILED(err))
        return false;
    Str text = strings_builder_string(&b);
    fmt_printf_v("%s\n", strings_cut(text, BURROW_S(": "), NULL, NULL));
    fmt_printf_v("buffers in the profile: %t\n",
                 strings_contains(text, BURROW_S("\n64: 262144 [64: 262144] @")));
    for (int i = 0; i < 64; i++)
        mem_free(heap_allocator(), buffers[i], 4096, 1);
    return true;
}
```

That prints:

```text
heap profile
buffers in the profile: true
```

In the text form each record is a line of in-use objects and bytes, then the allocated objects and bytes in brackets, then the stack, so the buffers are a record that starts `64: 262144 [64: 262144] @`. With debug 0 the profile is the gzipped protocol buffer that `go tool pprof` reads, and `pprof_write_heap_profile` writes it in one call.

Go hears about a free when its collector sweeps the object, so its heap profile is as of the last collection. Here a free is a call to `mem_free` and is counted as it happens, so the profile is as of the moment it is written. Memory from an arena is profiled when the arena takes a block from the heap, and the stack is the one that made the arena grow. Frames inside the allocator are left off the top of every stack, as Go leaves off its runtime.

## The block and mutex profiles

"block" has the places goroutines waited: a channel send or receive, a select, a `sync_mutex` or `sync_rw_mutex` lock, `sync_cond_wait` and `sync_wait_group_wait`. "mutex" has the places that made them wait, which is the unlock of a contended `sync_mutex` or `sync_rw_mutex`. Both are off until the program turns them on. `runtime_set_block_profile_rate` takes the average number of nanoseconds blocked per sample, so 1 keeps every event, and `runtime_set_mutex_profile_fraction` takes the 1 in n contention events to keep. This example turns both on, then has one goroutine hold a lock while another waits for it:

<!-- example: ../examples/pprof/pprof.c#contention -->
```c
/* One goroutine holds a lock while another waits for it. */
static SyncMutex mu;

static void hold(void *env) {
    sync_mutex_lock(&mu);
    bool ok = true;
    chan_send((Chan *)env, &ok);
    time_sleep(50 * TIME_MILLISECOND);
    sync_mutex_unlock(&mu);
}

static void contend(void *env) {
    runtime_set_block_profile_rate(1);
    runtime_set_mutex_profile_fraction(1);

    Chan *locked = chan_make((Alloc *)env, TYPE_BOOL, 0);
    if (locked == NULL || !go(BURROW_FN(Func, hold, locked)))
        return;
    (void)chan_recv(locked, NULL);
    sync_mutex_lock(&mu); /* waits until hold lets go */
    sync_mutex_unlock(&mu);
    chan_free(locked);

    const char *names[] = {"block", "mutex"};
    for (int i = 0; i < 2; i++) {
        PprofProfile *p = pprof_lookup(str_from_cstr(names[i]));
        fmt_printf_v("%s profile has records: %t\n", names[i],
                     (bool)(pprof_profile_count(p) > 0));
    }
}
```

`main` runs it as a goroutine:

<!-- example: ../examples/pprof/pprof.c#run -->
```c
runtime_main(BURROW_FN(Func, contend, a));
```

That prints:

```text
block profile has records: true
mutex profile has records: true
```

In the text form, debug 1, a profile starts with `--- contention:` for block and `--- mutex:` for mutex, then `cycles/second=`, and each record is the time spent waiting, the number of waits and the stack. Go counts CPU cycles, and burrow counts nanoseconds, so cycles/second is always 1000000000. With debug 0 the profile is the protocol buffer `go tool pprof` reads, with contentions and delay in nanoseconds. `runtime_block_profile` and `runtime_mutex_profile` hand back the same records as structs.

The stack of a block record starts at the function that blocked, such as `chan_recv` or `burrow__sync_mutex_lock_slow`, and the stack of a mutex record starts at the unlock, `burrow__sync_mutex_unlock_slow` or the `sync_rw_mutex` function that let the lock go. When an unlock wakes one goroutine and others are still queued, the unlock is charged an estimate of their waiting as well, as Go does. Go's mutex profile also has contention on the runtime's own locks, and burrow's scheduler locks are not profiled.
