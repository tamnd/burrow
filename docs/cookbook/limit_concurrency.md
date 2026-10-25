# Run at most a few things at once

A channel with room for n values works as a semaphore. Each goroutine sends to take a slot before it starts and receives to give the slot back when it's done, and a send blocks while every slot is taken. Starting all the goroutines at once is fine: only n of them run at a time.

<!-- example: ../examples/cookbook/limit_concurrency.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

enum { jobs = 10, at_once = 3 };

/* A channel with room for at_once values works as a semaphore: a goroutine
 * sends to take a slot, and the send blocks while all of them are taken.
 * Receiving gives the slot back. */
static Chan *slots;

static SyncAtomicInt64 running, most, done;

static void job(void *env) {
    (void)env;
    Int token = 0;
    chan_send(slots, &token);

    /* Keep track of the most that were ever running together. */
    int64_t now = sync_atomic_int64_add(&running, 1);
    for (int64_t m = sync_atomic_int64_load(&most); now > m;
         m = sync_atomic_int64_load(&most)) {
        if (sync_atomic_int64_compare_and_swap(&most, m, now))
            break;
    }
    time_sleep(5 * TIME_MILLISECOND); /* the work, such as a download */
    sync_atomic_int64_add(&running, -1);
    sync_atomic_int64_add(&done, 1);

    chan_recv(slots, &token);
}

static void run(void *env) {
    (void)env;
    slots = chan_make(heap_allocator(), TYPE_INT, at_once);

    /* All ten goroutines start at once, but only three get past the send
     * at a time. The rest wait their turn. */
    SyncWaitGroup wg = {0};
    for (Int i = 0; i < jobs; i++)
        sync_wait_group_go(&wg, FN(Func, job, NULL));
    sync_wait_group_wait(&wg);

    bool within = sync_atomic_int64_load(&most) <= at_once;
    fmt_println_v("jobs done:", (Int)sync_atomic_int64_load(&done));
    fmt_printf_v("never more than %d at once: %t\n", (Int)at_once, within);
    chan_free(slots);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

It prints:

```text
jobs done: 10
never more than 3 at once: true
```

The program is [docs/examples/cookbook/limit_concurrency.c](../examples/cookbook/limit_concurrency.c). CI builds and runs it and checks that it prints this.
