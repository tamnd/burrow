# Execution traces

`burrow/runtime/trace.h` is Go's `runtime/trace`. A trace records what the scheduler did while it was on: every goroutine created, started, blocked, woken and ended, and every P taken and given back, each with the time and the stack it happened at. The file is in the format Go 1.26 writes, so `go tool trace` opens it and shows the same timeline, goroutine analysis and latency views it shows for a Go program.

## Starting and stopping

`trace_start` takes any `IoWriter` and starts writing to it. The writes happen on a goroutine of their own while the program runs, so the writer has to be safe to use from there. A second `trace_start` while one is running fails with "execution tracer already enabled".

<!-- example: ../examples/trace/trace.c#start -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
Error err = trace_start(bytes_buffer_as_io_writer(&buf));
if (BURROW_FAILED(err))
    return;
```

`trace_stop` returns once everything has been written, so the buffer is complete when it does:

<!-- example: ../examples/trace/trace.c#stop -->
```c
trace_stop();
Bytes out = bytes_buffer_bytes(&buf);
fmt_printf_v("trace written: %t\n", (bool)(out.len > 0));
fmt_printf_v("tracing: %t\n", trace_is_enabled());
```

Write the trace to a file with `os_create` and `os_file_as_io_writer`, and then look at it with

```sh
go tool trace trace.out
```

A test binary does all of this for you with `-test.trace=trace.out`, and a server that registers the `net/http/pprof` handlers serves a trace of the next few seconds at `/debug/pprof/trace?seconds=5`.

## Tasks, regions and logs

The scheduler's events say what each goroutine did but not why. A program can add that. A task is one piece of work the program cares about, such as one request, which may run on several goroutines and which `go tool trace` measures from start to end. A region is a stretch of one goroutine's time inside a task, and a log is a message at a moment, filed under a category.

`trace_new_task` returns a context that carries the task. A task made from a context that already has one is its subtask:

<!-- example: ../examples/trace/trace.c#task -->
```c
TraceTask *task;
Context ctx =
    trace_new_task(gc_allocator(), context_background(), BURROW_S("orders"), &task);
for (int i = 0; i < 3; i++) {
    sync_wait_group_add(&orders, 1);
    (void)go(BURROW_FN(Func, make_cappuccino, &ctx));
}
sync_wait_group_wait(&orders);
trace_task_end(task);
```

Each goroutine marks what it spends time on with a region and logs what is worth knowing:

<!-- example: ../examples/trace/trace.c#order -->
```c
/* One order, made on its own goroutine, inside the task the context carries. */
static SyncWaitGroup orders;

static void make_cappuccino(void *env) {
    Context ctx = *(Context *)env;
    TraceTask *task;
    ctx = trace_new_task(gc_allocator(), ctx, BURROW_S("makeCappuccino"), &task);

    TraceRegion r = trace_start_region(ctx, BURROW_S("steamMilk"));
    time_sleep(TIME_MILLISECOND);
    trace_region_end(r);

    trace_logf_v(ctx, BURROW_S("orderID"), BURROW_S("%d"), 1234);
    trace_task_end(task);
    sync_wait_group_done(&orders);
}
```

`trace_with_region` runs a function inside a region, for when the region is exactly one call. Regions have to nest inside each goroutine, and a region has to end on the goroutine that started it.

The task and the context come from the allocator you pass and are never given back, because other goroutines can still be using the task after the context has gone. `gc_allocator` or an arena that outlives the task are the right choices. When no trace is on, the annotations cost a check of one flag, so they can stay in production code.

## Where it differs from Go

Frames in a trace have no file names or line numbers, the same as in a profile, and a frame has a function name only when burrow's symbol table has one. That covers the library and not your program, so frames in your own code show as bare addresses.

Go stops the world for a moment when a trace starts. burrow cannot, so a goroutine whose state changes at exactly that moment, on a thread that then stalls for most of a second, can be missing that one change from the first second of the trace.

A thread the program started itself can call the annotations, but it is not running a goroutine, so nothing is written for it.

A trace started inside `runtime_main` ends when `runtime_main` returns, since the goroutine writing it ends then too. Call `trace_stop` first. A trace that ended that way still needs `trace_stop` before the next `trace_start`. A trace started before `runtime_main` is written by a thread instead and can span several runs.

The flight recorder is not here yet.
