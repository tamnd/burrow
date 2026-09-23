#include <stdio.h>

#include "burrow/burrow.h"

static SyncWaitGroup wg;
static Chan *jobs;
static Int job;
static Int done_jobs;

static void do_the_job(Int *j) {
    done_jobs += *j;
}

// doc: work
static void work(void *env) {
    Context ctx = *(Context *)env;

    SelectCase cases[2];
    cases[0] = BURROW_RECV(context_done(ctx), NULL);
    cases[1] = BURROW_RECV(jobs, &job);

    for (;;) {
        if (chan_select(cases, 2) == 0)
            return;
        do_the_job(&job);
    }
}
// doc: end

static void queue_the_jobs(void) {
    for (Int i = 1; i <= 3; i++)
        chan_send(jobs, &i);
}

static void run(void *env) {
    Arena ar;
    (void)env;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    jobs = chan_make(a, TYPE_INT, 0);

    // doc: cancel
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);

    sync_wait_group_go(&wg, BURROW_FN(Func, work, &ctx));
    queue_the_jobs();
    BURROW_CALLF0(cancel);
    sync_wait_group_wait(&wg);
    context_release(ctx);
    // doc: end

    printf("the jobs added up to %lld\n", (long long)done_jobs);
    chan_free(jobs);
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
the jobs added up to 6
*/
