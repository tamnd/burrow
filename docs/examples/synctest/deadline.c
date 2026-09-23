#include <assert.h>
#include <stdio.h>

#include "burrow/burrow.h"

// doc: body
static void body(void *env) {
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       30 * TIME_SECOND, &cancel);

    // Nothing in the bubble can run, so the clock jumps to the deadline and the
    // context expires. A real thirty second wait, for free, and nothing to be
    // flaky about.
    Int v;
    chan_recv(context_done(ctx), &v);
    assert(errors_is(context_err(ctx), context_deadline_exceeded));

    BURROW_CALLF0(cancel);
    context_release(ctx);
}
// doc: end

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
    printf("the deadline passed\n");
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
the deadline passed
*/
