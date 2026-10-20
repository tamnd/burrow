#include <stdio.h>

#include "burrow/burrow.h"

// doc: do
static void handle(void *env, Context ctx) {
    (void)env;
    Str route;
    if (pprof_label(ctx, BURROW_S("route"), &route))
        fmt_printf_v("handling %s\n", route);
}
// doc: end

// doc: open
/* Every open connection is in the profile, under the stack that opened it. */
static PprofProfile *conns;

static void open_conn(Int *id) {
    pprof_profile_add(conns, BURROW_ANY(TYPE_INT, id), 0);
}

static void close_conn(Int *id) {
    pprof_profile_remove(conns, BURROW_ANY(TYPE_INT, id));
}
// doc: end

// doc: goroutines
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
// doc: end

// doc: cpu
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
// doc: end

// doc: heap
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
// doc: end

// doc: contention
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
// doc: end

int main(void) {
    // doc: rate
    runtime_set_mem_profile_rate(1);
    // doc: end
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: labels
    PprofLabelSet labels = pprof_labels_v(a, 4, BURROW_S("route"), BURROW_S("/users"),
                                          BURROW_S("method"), BURROW_S("GET"));
    pprof_do(a, context_background(), labels, BURROW_FN(PprofDoFunc, handle, NULL));
    // doc: end

    // doc: profile
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
    // doc: end

    close_conn(&ids[0]);
    close_conn(&ids[2]);

    // doc: main
    runtime_main(BURROW_FN(Func, look, a));
    // doc: end
    runtime_main(BURROW_FN(Func, profile_cpu, a));
    if (!profile_heap(a))
        return 1;
    // doc: run
    runtime_main(BURROW_FN(Func, contend, a));
    // doc: end
    arena_free(&ar);
    return 0;
}

/* Output:
handling /users
example.com/conns has 2
example.com/conns profile: total 2
goroutine profile: total 4
cpu profile written: true
heap profile
buffers in the profile: true
block profile has records: true
mutex profile has records: true
*/
