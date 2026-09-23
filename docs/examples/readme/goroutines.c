#include <stdio.h>

#include "burrow/burrow.h"

typedef struct Job {
    int id;
} Job;

static Job job = {7};

// doc: goroutines
static void worker(void *env) {
    Job *j = env;
    printf("working on job %d\n", j->id);
}

static void run(void *env) {
    (void)env;
    go(BURROW_FN(Func, worker, &job));
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}
// doc: end

/* Output:
working on job 7
*/
