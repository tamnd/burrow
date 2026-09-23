#include <stdio.h>

#include "burrow/burrow.h"

static void say(void *word) {
    printf("%s\n", (const char *)word);
}

static void done(void *wg) {
    sync_wait_group_done(wg);
}

static void give_up_early(void) {
    // doc: goexit
    runtime_goexit();
    // doc: end
}

static void worker(void *env) {
    SyncWaitGroup *wg = env;
    BURROW_SCOPE {
        BURROW_DEFER(done, wg);
        BURROW_DEFER(say, "the deferred call ran");
        give_up_early();
        say("never printed");
    }
    BURROW_SCOPE_END;
}

static void run(void *env) {
    SyncWaitGroup wg = {0};
    sync_wait_group_add(&wg, 1);
    go(BURROW_FN(Func, worker, &wg));
    sync_wait_group_wait(&wg);
    say("the worker is gone");
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
the deferred call ran
the worker is gone
*/
