#include "burrow/burrow.h"

// doc: order
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
// doc: end

static void run(void *env) {
    Alloc *a = (Alloc *)env;
    // doc: start
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = trace_start(bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        return;
    // doc: end
    // doc: task
    TraceTask *task;
    Context ctx =
        trace_new_task(gc_allocator(), context_background(), BURROW_S("orders"), &task);
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
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}

/* Output:
trace written: true
tracing: false
header: "go 1.26 trace"
*/
