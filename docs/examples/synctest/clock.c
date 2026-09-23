#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

// doc: body
static void body(void *env) {
    int64_t start = burrow_nanotime();

    time_sleep(TIME_HOUR);

    // Exactly an hour later, on the bubble's clock. The test took no time.
    assert(burrow_nanotime() - start == TIME_HOUR);
}
// doc: end

static void top(void *env) {
    int64_t start = burrow_nanotime();
    synctest_run(BURROW_FN(Func, body, NULL));
    printf("an hour in a bubble took under a second: %s\n",
           burrow_nanotime() - start < TIME_SECOND ? "yes" : "no");
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
an hour in a bubble took under a second: yes
*/
