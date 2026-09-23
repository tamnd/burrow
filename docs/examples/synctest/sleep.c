#include <assert.h>
#include <stdio.h>

#include "burrow/burrow.h"

// doc: body
static int refreshes;

static void refresh_every_minute(void *env) {
    for (int i = 0; i < 3; i++) {
        time_sleep(TIME_MINUTE);
        refreshes++;
    }
}

static void body(void *env) {
    go(BURROW_FN(Func, refresh_every_minute, NULL));

    // The refresher wakes at the same instant this does each time. The wait
    // inside the sleep lets it finish the refresh before the check looks.
    for (int want = 1; want <= 3; want++) {
        synctest_sleep(TIME_MINUTE);
        assert(refreshes == want);
    }
}
// doc: end

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
    printf("%d refreshes, each one seen\n", refreshes);
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
3 refreshes, each one seen
*/
