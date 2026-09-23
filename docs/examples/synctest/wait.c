#include <stdio.h>

#include "burrow/burrow.h"

// doc: body
static void worker(void *env) {
    Chan *c = env;
    Int v = 1;
    chan_send(c, &v);
}

static void body(void *env) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, worker, c));

    synctest_wait();

    /* The worker is sitting on the send. Not probably, not usually. */
    Int v;
    chan_recv(c, &v);
    printf("received %d\n", (int)v);
    chan_free(c);
}
// doc: end

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
received 1
*/
