#include <stdio.h>

#include "burrow/burrow.h"

static int taken;

static void take_the_work(void) {
    taken++;
}

// doc: cond
static SyncMutex mu;
static SyncCond ready;
static bool has_work;

void consume(void) {
    sync_mutex_lock(&mu);
    while (!has_work)
        sync_cond_wait(&ready);
    take_the_work();
    sync_mutex_unlock(&mu);
}

void produce(void) {
    sync_mutex_lock(&mu);
    has_work = true;
    sync_cond_signal(&ready);
    sync_mutex_unlock(&mu);
}
// doc: end

// doc: cond-static
static SyncCond ready;

void init_ready(void) {
    ready = SYNC_COND(sync_mutex_locker(&mu));
}
// doc: end

static SyncWaitGroup wg;

static void consumer(void *env) {
    (void)env;
    consume();
}

static void run(void *env) {
    (void)env;
    init_ready();
    sync_wait_group_go(&wg, BURROW_FN(Func, consumer, NULL));
    produce();
    sync_wait_group_wait(&wg);
    printf("the consumer took %d piece of work\n", taken);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
the consumer took 1 piece of work
*/
