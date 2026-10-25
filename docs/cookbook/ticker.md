# Do something at a steady rate

Run something every 20 milliseconds with a ticker. Each receive on the ticker's channel waits for the next tick. A tick that nobody reads is dropped, so a slow loop doesn't get a backlog of them, and `time_ticker_free` stops the ticker and frees it.

<!-- example: ../examples/cookbook/ticker.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Do something every 20 milliseconds, five times. */
    Time start = time_now();
    TimeTicker *t = time_new_ticker(a, 20 * TIME_MILLISECOND);
    for (Int i = 1; i <= 5; i++) {
        chan_recv(time_ticker_c(t), NULL);
        fmt_println_v("tick", i);
    }
    time_ticker_free(t); /* stops it, too */

    /* The ticks were at least 20 milliseconds apart, so five took at least 100. */
    bool long_enough = time_since(start) >= 100 * TIME_MILLISECOND;
    fmt_println_v("took 100ms or more:", long_enough);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

It prints:

```text
tick 1
tick 2
tick 3
tick 4
tick 5
took 100ms or more: true
```

The program is [docs/examples/cookbook/ticker.c](../examples/cookbook/ticker.c). CI builds and runs it and checks that it prints this.
