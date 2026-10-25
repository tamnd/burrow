// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Three stages joined by channels. Each stage owns the channel it sends on
 * and closes it when it has nothing more, which is how the next stage knows
 * to stop. */

typedef struct {
    Chan *in, *out;
} Stage;

static void numbers(void *env) {
    Chan *out = env;
    for (Int n = 1; n <= 10; n++)
        chan_send(out, &n);
    chan_close(out);
}

static void keep_odd(void *env) {
    Stage *s = env;
    Int n;
    while (chan_recv(s->in, &n))
        if (n % 2 == 1)
            chan_send(s->out, &n);
    chan_close(s->out);
}

static void square(void *env) {
    Stage *s = env;
    Int n;
    while (chan_recv(s->in, &n)) {
        Int sq = n * n;
        chan_send(s->out, &sq);
    }
    chan_close(s->out);
}

static void run(void *env) {
    (void)env;
    Alloc *a = heap_allocator();
    Chan *nums = chan_make(a, TYPE_INT, 0);
    Chan *odds = chan_make(a, TYPE_INT, 0);
    Chan *squares = chan_make(a, TYPE_INT, 0);
    Stage odd = {nums, odds}, sq = {odds, squares};
    go(FN(Func, numbers, nums));
    go(FN(Func, keep_odd, &odd));
    go(FN(Func, square, &sq));

    /* The last stage runs here. A channel keeps its order, so the values come
     * out in the order numbers sent them. */
    Int n, total = 0;
    while (chan_recv(squares, &n)) {
        fmt_println_v(n);
        total += n;
    }
    fmt_println_v("total:", total);
    chan_free(nums);
    chan_free(odds);
    chan_free(squares);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
1
9
25
49
81
total: 165
*/
