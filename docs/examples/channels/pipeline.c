// doc: pipeline
#include <stdio.h>

#include "burrow/chan.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"

static void producer(void *env) {
    Chan *c = env;
    for (Int i = 0; i < 10; i++)
        chan_send(c, &i);
    chan_close(c);
}

static void run(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, producer, c));

    Int v;
    while (chan_recv(c, &v))
        printf("%lld\n", (long long)v);

    chan_free(c);
}
// doc: end

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
0
1
2
3
4
5
6
7
8
9
*/
