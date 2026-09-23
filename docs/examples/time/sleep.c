#include <stdio.h>

#include "burrow/burrow.h"

static int64_t started;
static SyncAtomicBool stopping;

static void check_the_thing(void) {
    printf("checked at %llds\n",
           (long long)((burrow_nanotime() - started) / TIME_SECOND));
}

// doc: poll
static void poll(void *env) {
    (void)env;
    while (!sync_atomic_bool_load(&stopping)) {
        check_the_thing();
        time_sleep(30 * TIME_SECOND);
    }
}
// doc: end

// In a synctest bubble, so the two minutes below take no time at all.
static void body(void *env) {
    started = burrow_nanotime();
    go(BURROW_FN(Func, poll, NULL));
    time_sleep(100 * TIME_SECOND);
    sync_atomic_bool_store(&stopping, true);

    // poll is asleep until 120s. The bubble cannot end with it still asleep, so
    // wait for it to wake up, see the flag and return.
    time_sleep(30 * TIME_SECOND);
}

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
checked at 0s
checked at 30s
checked at 60s
checked at 90s
*/
