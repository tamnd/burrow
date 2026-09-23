#include <stdio.h>

#include "burrow/burrow.h"

static void worker(void *env) {
    Chan *c = env;
    Int v = 42;
    chan_send(c, &v);
}

// doc: synctest
static void body(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, worker, c));

    synctest_wait();

    /* The worker is sitting on the send. Not probably, not usually. */
    Int v;
    chan_recv(c, &v);
    chan_free(c);
}

void test_worker_blocks(void) {
    synctest_run(BURROW_FN(Func, body, NULL));
}
// doc: end

static void top(void *env) {
    (void)env;
    test_worker_blocks();
    printf("the bubble finished\n");
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
the bubble finished
*/
