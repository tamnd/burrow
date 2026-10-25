// doc: goroutines
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static Chan *sums;

static void add(void *env) {
    Int from = (Int)(intptr_t)env, sum = 0;
    for (Int i = from; i < from + 100; i++)
        sum += i;
    chan_send(sums, &sum);
}

static void run(void *env) {
    sums = chan_make(heap_allocator(), TYPE_INT, 0);
    for (Int from = 1; from <= 300; from += 100)
        go(FN(Func, add, (void *)(intptr_t)from));

    Int total = 0, sum;
    for (int i = 0; i < 3; i++) {
        chan_recv(sums, &sum);
        total += sum;
    }
    fmt_println_v("1 + 2 + ... + 300 =", total);
    chan_free(sums);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
1 + 2 + ... + 300 = 45150
*/
