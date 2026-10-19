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
    arena_free(&ar);
    return 0;
}

/* Output:
handling /users
example.com/conns has 2
example.com/conns profile: total 2
*/
