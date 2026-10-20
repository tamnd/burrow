#include "burrow/burrow.h"

/* Something to record: a few goroutines that each spend a moment in a region. */
static SyncWaitGroup brews;

static void brew(void *env) {
    (void)env;
    TraceRegion r = trace_start_region(context_background(), BURROW_S("brew"));
    time_sleep(TIME_MILLISECOND);
    trace_region_end(r);
    sync_wait_group_done(&brews);
}

static void run(void *env) {
    Alloc *a = arena_allocator((Arena *)env);
    // doc: start
    TraceFlightRecorderConfig cfg = {.min_age = 5 * TIME_SECOND, .max_bytes = 4 << 20};
    TraceFlightRecorder *fr = trace_new_flight_recorder(a, cfg);
    if (fr == NULL)
        return;
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err))
        return;
    fmt_printf_v("recording: %t\n", trace_flight_recorder_enabled(fr));
    // doc: end
    for (int i = 0; i < 3; i++) {
        sync_wait_group_add(&brews, 1);
        (void)go(BURROW_FN(Func, brew, NULL));
    }
    sync_wait_group_wait(&brews);
    // doc: snapshot
    BytesBuffer buf = BYTES_BUFFER(a);
    int64_t n =
        trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(&buf), &err);
    fmt_printf_v("snapshot written: %t\n", (bool)(n > 0 && !BURROW_FAILED(err)));
    // doc: end
    /* A snapshot is a whole trace, header and all. */
    Bytes out = bytes_buffer_bytes(&buf);
    fmt_printf_v("header: %q\n", str_from_bytes(out.p, out.len < 13 ? out.len : 13));
    // doc: stop
    trace_flight_recorder_stop(fr);
    fmt_printf_v("recording: %t\n", trace_flight_recorder_enabled(fr));
    (void)trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(&buf), &err);
    fmt_printf_v("after stop: %v\n", err);
    trace_flight_recorder_free(fr);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, &ar));
    arena_free(&ar);
    return 0;
}

/* Output:
recording: true
snapshot written: true
header: "go 1.26 trace"
recording: false
after stop: cannot snapshot a disabled flight recorder
*/
