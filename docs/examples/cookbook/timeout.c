// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Stands in for work that takes d, such as a query, and stops early when ctx
 * is done. */
static Error slow_answer(Alloc *a, Context ctx, Duration d, Int *answer) {
    Chan *timer = time_after_chan(a, d);
    SelectCase cases[] = {
        BURROW_RECV(timer, NULL),
        BURROW_RECV(context_done(ctx), NULL),
    };
    Int which = chan_select(cases, 2);
    chan_free(timer);
    if (which == 1)
        return context_err(ctx);
    *answer = 42;
    return NO_ERROR;
}

/* Asks for the answer, but waits no longer than limit. */
static void ask(Alloc *a, Duration work, Duration limit) {
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(a, context_background(), limit, &cancel);
    Int answer = 0;
    Error err = slow_answer(a, ctx, work, &answer);
    if (FAILED(err))
        fmt_println_v("gave up:", err);
    else
        fmt_println_v("answer:", answer);
    CALLF0(cancel);
    context_release(ctx);
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ask(a, 10 * TIME_MILLISECOND, TIME_SECOND);
    ask(a, TIME_SECOND, 50 * TIME_MILLISECOND);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
answer: 42
gave up: context deadline exceeded
*/
