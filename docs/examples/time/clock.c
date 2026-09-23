#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void work(void) {
    time_sleep(2 * TIME_MILLISECOND);
}

int main(void) {
    // doc: durations
    Duration d = 500 * TIME_MILLISECOND;
    Duration total = 2 * TIME_SECOND + 300 * TIME_MILLISECOND;
    // doc: end
    printf("%lld and %lld nanoseconds\n", (long long)d, (long long)total);

    // doc: clock
    int64_t start = burrow_nanotime();
    work();
    Duration took = burrow_nanotime() - start;
    // doc: end
    printf("took at least 2ms: %s\n", took >= 2 * TIME_MILLISECOND ? "yes" : "no");
    return 0;
}

/* Output:
500000000 and 2300000000 nanoseconds
took at least 2ms: yes
*/
