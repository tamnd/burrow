#include <stdio.h>

#include "burrow/burrow.h"

#define CONFIG_FIELDS(F, T)                                                            \
    F(T, Int, workers, "")                                                             \
    F(T, Str, name, "")
BURROW_STRUCT(Config, CONFIG_FIELDS);
#define TYPE_CONFIG TYPE_OF(Config)

// doc: typed
static SyncAtomicInt64 hits;

void serve(void) {
    sync_atomic_int64_add(&hits, 1);
}
// doc: end

static int shutdowns;

static void begin_shutdown(void) {
    shutdowns++;
}

// doc: bool
static SyncAtomicBool stopping;

static void stop(void) {
    if (sync_atomic_bool_compare_and_swap(&stopping, false, true))
        begin_shutdown(); /* only the first caller gets in */
}
// doc: end

// doc: value
static SyncAtomicValue config;

void reload(Config *next) {
    sync_atomic_value_store(&config, BURROW_ANY(TYPE_CONFIG, next));
}

Config *current(void) {
    return any_assert(sync_atomic_value_load(&config), TYPE_CONFIG);
}
// doc: end

int main(void) {
    serve();
    serve();
    printf("hits %lld\n", (long long)sync_atomic_int64_load(&hits));

    stop();
    stop();
    printf("shut down %d time\n", shutdowns);

    Config first = {4, BURROW_S("first")};
    Config second = {8, BURROW_S("second")};
    reload(&first);
    reload(&second);
    Config *c = current();
    printf("%.*s with %lld workers\n", (int)c->name.len, (const char *)c->name.p,
           (long long)c->workers);
    return 0;
}

/* Output:
hits 2
shut down 1 time
second with 8 workers
*/
