// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

enum { njobs = 10, nworkers = 3 };

static Chan *jobs;
static Int steps[njobs + 1];

/* How many steps the Collatz sequence takes from n down to 1. */
static Int collatz(Int n) {
    Int k = 0;
    for (; n != 1; k++)
        n = n % 2 == 0 ? n / 2 : 3 * n + 1;
    return k;
}

/* Takes jobs until the channel is closed. Each job has its own slot in steps,
 * so the workers never write the same memory and need no lock. */
static void worker(void *env) {
    (void)env;
    Int n;
    while (chan_recv(jobs, &n))
        steps[n] = collatz(n);
}

static void run(void *env) {
    (void)env;
    jobs = chan_make(heap_allocator(), TYPE_INT, njobs);
    SyncWaitGroup wg = {0};
    for (int i = 0; i < nworkers; i++)
        sync_wait_group_go(&wg, FN(Func, worker, NULL));

    for (Int n = 1; n <= njobs; n++)
        chan_send(jobs, &n);
    chan_close(jobs);
    sync_wait_group_wait(&wg);

    for (Int n = 1; n <= njobs; n++)
        fmt_printf_v("steps from %d: %d\n", n, steps[n]);
    chan_free(jobs);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
steps from 1: 0
steps from 2: 1
steps from 3: 7
steps from 4: 2
steps from 5: 5
steps from 6: 8
steps from 7: 16
steps from 8: 3
steps from 9: 19
steps from 10: 6
*/
