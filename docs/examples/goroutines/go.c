#include <stdio.h>

#include "burrow/burrow.h"

typedef struct Job {
    Int input;
    Int result;
    SyncWaitGroup *done;
} Job;

// doc: go
static void worker(void *env) {
    Job *j = env;
    j->result = j->input * j->input;
    sync_wait_group_done(j->done);
}

static void start(Job *job) {
    go(BURROW_FN(Func, worker, job));
}
// doc: end

static void run(void *env) {
    SyncWaitGroup wg = {0};
    Job jobs[3] = {{2, 0, &wg}, {3, 0, &wg}, {4, 0, &wg}};
    sync_wait_group_add(&wg, 3);
    for (int i = 0; i < 3; i++)
        start(&jobs[i]);
    sync_wait_group_wait(&wg);
    for (int i = 0; i < 3; i++)
        printf("%lld squared is %lld\n", (long long)jobs[i].input,
               (long long)jobs[i].result);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
2 squared is 4
3 squared is 9
4 squared is 16
*/
