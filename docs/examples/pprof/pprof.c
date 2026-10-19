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

int main(void) {
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
    arena_free(&ar);
    return 0;
}

/* Output:
handling /users
example.com/conns has 2
example.com/conns profile: total 2
goroutine profile: total 4
*/
