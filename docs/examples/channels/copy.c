#include <stdio.h>

#include "burrow/burrow.h"

static void sender(void *env) {
    Chan *c = env;
    Int v = 42;
    chan_send(c, &v);

    // doc: loop
    for (Int i = 0; i < 3; i++)
        chan_send(c, &i); /* fine: the value is copied, not the address */
    // doc: end

    v = 7;
    // doc: handoff
    chan_send(c, &v);
    /* by the time this line runs, some other goroutine has v */
    // doc: end

    // doc: close
    chan_close(c);
    // doc: end
}

// A buffer of one, so a single goroutine can both send and receive.
static void round_trip(Chan *c) {
    // doc: copy
    Int v = 42;
    chan_send(c, &v); /* copies sizeof(Int) bytes out of v */

    Int got;
    chan_recv(c, &got); /* copies them into got */
    // doc: end
    printf("round trip %lld\n", (long long)got);
}

static void run(void *env) {
    Chan *one = chan_make(heap_allocator(), TYPE_INT, 1);
    round_trip(one);
    chan_free(one);

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, sender, c));

    Int got;
    chan_recv(c, &got);
    printf("%lld\n", (long long)got);
    while (chan_recv(c, &got))
        printf("%lld\n", (long long)got);
    printf("closed\n");
    chan_free(c);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
round trip 42
42
0
1
2
7
closed
*/
