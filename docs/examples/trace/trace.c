#include "burrow/burrow.h"

// doc: order
/* One order, made on its own goroutine, as a subtask of the task the context
 * carries. The subtask lives in an arena of the goroutine's own. */
static SyncWaitGroup orders;

static void make_cappuccino(void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    TraceTask *task;
    Context ctx = trace_new_task(arena_allocator(&ar), *(Context *)env,
                                 BURROW_S("makeCappuccino"), &task);

    TraceRegion r = trace_start_region(ctx, BURROW_S("steamMilk"));
    time_sleep(TIME_MILLISECOND);
    trace_region_end(r);

    trace_logf_v(ctx, BURROW_S("orderID"), BURROW_S("%d"), 1234);
    trace_task_end(task);
    arena_free(&ar);
    sync_wait_group_done(&orders);
}
// doc: end

static void run(void *env) {
    Arena *ar = (Arena *)env;
    Alloc *a = arena_allocator(&ar[0]);
    // doc: start
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = trace_start(bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        return;
    // doc: end
    // doc: task
    TraceTask *task;
    Context ctx = trace_new_task(arena_allocator(&ar[1]), context_background(),
                                 BURROW_S("orders"), &task);
    for (int i = 0; i < 3; i++) {
        sync_wait_group_add(&orders, 1);
        (void)go(BURROW_FN(Func, make_cappuccino, &ctx));
    }
    sync_wait_group_wait(&orders);
    trace_task_end(task);
    // doc: end
    // doc: stop
    trace_stop();
    Bytes out = bytes_buffer_bytes(&buf);
    fmt_printf_v("trace written: %t\n", (bool)(out.len > 0));
    fmt_printf_v("tracing: %t\n", trace_is_enabled());
    // doc: end
    /* The format's name and version, which go tool trace checks first. */
    fmt_printf_v("header: %q\n", str_from_bytes(out.p, out.len < 13 ? out.len : 13));
}

int main(void) {
    /* One for the trace and one for the task, since the trace is written on
     * another goroutine while this one makes the task. */
    Arena ar[2];
    arena_init(&ar[0], NULL, 0);
    arena_init(&ar[1], NULL, 0);
    runtime_main(BURROW_FN(Func, run, ar));
    arena_free(&ar[0]);
    arena_free(&ar[1]);
    return 0;
}

/* Output:
trace written: true
tracing: false
header: "go 1.26 trace"
*/
