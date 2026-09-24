#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/iter.h"

// doc: seq
/* count yields the Ints from 1 to *env, which is the documented contract: each
 * v points at an Int. */
static void count(void *env, IterYield yield) {
    Int n = *(const Int *)env;
    for (Int i = 1; i <= n; i++) {
        if (!BURROW_CALLF(yield, &i))
            return;
    }
}
// doc: end

// doc: push
static bool print_int(void *env, const void *v) {
    (void)env;
    printf(" %lld", (long long)*(const Int *)v);
    return true;
}
// doc: end

static void push(void) {
    // doc: push-call
    Int n = 5;
    IterSeq seq = BURROW_FN(IterSeq, count, &n);
    printf("count:");
    BURROW_CALLF(seq, BURROW_FN(IterYield, print_int, NULL));
    printf("\n");
    // doc: end
}

static void range(void) {
    // doc: range
    Int n = 1000000;
    Int sum = 0;
    BURROW_RANGE(Int, i, BURROW_FN(IterSeq, count, &n)) {
        if (i % 2 == 0)
            continue;
        if (i > 9)
            break;
        sum += i;
    }
    printf("sum of odd numbers up to 9: %lld\n", (long long)sum);
    // doc: end
}

// doc: seq2
static void enumerate(void *env, IterYield2 yield) {
    const char *const *words = env;
    for (Int i = 0; words[i] != NULL; i++) {
        if (!BURROW_CALLF(yield, &i, &words[i]))
            return;
    }
}
// doc: end

static void range2(void) {
    // doc: range2
    static const char *const words[] = {"alpha", "beta", "gamma", NULL};
    BURROW_RANGE2(Int, i, const char *, w,
                  BURROW_FN(IterSeq2, enumerate, (void *)words)) {
        printf("%lld %s\n", (long long)i, w);
    }
    // doc: end
}

static void pull(void) {
    // doc: pull
    Int a = 3, b = 5;
    IterPull pa, pb;
    if (!iter_pull(&pa, BURROW_FN(IterSeq, count, &a)))
        return;
    if (!iter_pull(&pb, BURROW_FN(IterSeq, count, &b))) {
        iter_pull_stop(&pa);
        return;
    }
    printf("zip:");
    for (;;) {
        const void *x;
        const void *y;
        bool okx = iter_pull_next(&pa, &x);
        bool oky = iter_pull_next(&pb, &y);
        if (!okx || !oky)
            break;
        printf(" (%lld, %lld)", (long long)*(const Int *)x, (long long)*(const Int *)y);
    }
    printf("\n");
    iter_pull_stop(&pa);
    iter_pull_stop(&pb);
    // doc: end
}

static void run(void *env) {
    (void)env;
    push();
    range();
    range2();
    pull();
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
}

/* Output:
count: 1 2 3 4 5
sum of odd numbers up to 9: 25
0 alpha
1 beta
2 gamma
zip: (1, 1) (2, 2) (3, 3)
*/
