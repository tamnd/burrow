#include <stdio.h>

#include "burrow/burrow.h"

// The smallest thing that blocks: a receiver waits on a list until a sender
// hands it a value.
typedef struct Waiter {
    Goroutine *g;
    struct Waiter *next;
    Int result;
} Waiter;

typedef struct Chan {
    SyncMutex lock;
    Waiter *waiters;
} Chan;

// doc: callback
static bool unlock_chan(Goroutine *g, void *arg) {
    (void)g;
    sync_mutex_unlock(&((Chan *)arg)->lock);
    return true;
}
// doc: end

static Int receive(Chan *c) {
    Waiter w = {0};
    sync_mutex_lock(&c->lock);
    // doc: wait
    w.g = sched_current();
    w.next = c->waiters;
    c->waiters = &w;
    sched_park(unlock_chan, c);
    /* Woken, and whoever woke us left the answer in w. */
    // doc: end
    return w.result;
}

static bool send(Chan *c, Int value) {
    sync_mutex_lock(&c->lock);
    Waiter *w = c->waiters;
    if (w == NULL) {
        sync_mutex_unlock(&c->lock);
        return false;
    }
    // doc: ready
    c->waiters = w->next;
    w->result = value;
    sched_ready(w->g);
    // doc: end
    sync_mutex_unlock(&c->lock);
    return true;
}

static Chan ch;
static SyncWaitGroup wg;

static void receiver(void *env) {
    printf("received %lld\n", (long long)receive(&ch));
    sync_wait_group_done(&wg);
}

static void run(void *env) {
    sync_wait_group_add(&wg, 1);
    go(BURROW_FN(Func, receiver, NULL));
    while (!send(&ch, 42))
        runtime_gosched();
    sync_wait_group_wait(&wg);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
received 42
*/
