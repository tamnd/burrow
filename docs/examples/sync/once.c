#include <stdio.h>

#include "burrow/burrow.h"

typedef struct Job {
    Int n;
    Int result;
} Job;

static void process(Job *j) {
    j->result = j->n * j->n;
}

// doc: wait-group
static SyncWaitGroup wg;

static void work(void *env) {
    Job *j = env;
    process(j);
}

void run_all(Job *jobs, int n) {
    for (int i = 0; i < n; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, work, &jobs[i]));

    sync_wait_group_wait(&wg);
}
// doc: end

static int starts;

static void start(void *env) {
    (void)env;
    starts++;
}

// doc: once
static SyncOnce started;

void ensure_started(void) {
    sync_once_do(&started, BURROW_FN(Func, start, NULL));
}
// doc: end

static int loads;

static void load_plugins(void *env) {
    (void)env;
    loads++;
}

// doc: once-func
static SyncOnceFunc setup = SYNC_ONCE_FUNC(BURROW_FN(Func, load_plugins, NULL));

void prepare(void) {
    sync_once_func_call(&setup);
}
// doc: end

#define CONFIG_FIELDS(F, T) F(T, Int, port, "")
BURROW_STRUCT(Config, CONFIG_FIELDS);

static int reads;

static Config read_config(void) {
    reads++;
    return (Config){8080};
}

// doc: once-value
static Config cfg;

static Any load(void *env) {
    (void)env;
    cfg = read_config();
    return BURROW_ANY(TYPE_OF(Config), &cfg);
}

static SyncOnceValue config = SYNC_ONCE_VALUE(BURROW_FN(AnyFunc, load, NULL));

Config *current_config(void) {
    return any_assert(sync_once_value_get(&config), TYPE_OF(Config));
}
// doc: end

static void run(void *env) {
    (void)env;
    Job jobs[4] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
    run_all(jobs, 4);
    printf("results %lld %lld %lld %lld\n", (long long)jobs[0].result,
           (long long)jobs[1].result, (long long)jobs[2].result,
           (long long)jobs[3].result);

    for (int i = 0; i < 3; i++) {
        ensure_started();
        prepare();
    }
    printf("started %d time, loaded %d time\n", starts, loads);

    Config *c = current_config();
    c = current_config();
    printf("port %lld, read %d time\n", (long long)c->port, reads);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
results 1 4 9 16
started 1 time, loaded 1 time
port 8080, read 1 time
*/
