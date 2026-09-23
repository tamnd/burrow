#include "burrow/burrow.h"

static int32_t running;
static int32_t most;

static void slow(void *env, TestingT *t) {
    testing_t_parallel(t);
    int32_t now = sync_atomic_add_int32(&running, 1);
    for (;;) {
        int32_t seen = sync_atomic_load_int32(&most);
        if (now <= seen || sync_atomic_compare_and_swap_int32(&most, seen, now))
            break;
    }
    time_sleep(20 * TIME_MILLISECOND);
    sync_atomic_add_int32(&running, -1);
}

static void group(void *env, TestingT *t) {
    for (int i = 0; i < 4; i++)
        testing_t_run(t, BURROW_S("slow"), BURROW_FN(TestingTFunc, slow, NULL));
}

static void TestParallel(TestingT *t) {
    /* Run returns once every parallel subtest in the group has finished, so
     * this is the place to look at what they did. */
    testing_t_run(t, BURROW_S("group"), BURROW_FN(TestingTFunc, group, NULL));
    if (sync_atomic_load_int32(&running) != 0)
        testing_t_error_v(t, "a subtest was still running after Run returned");
}

#define TESTS(X) X(TestParallel)
TESTING_MAIN(TESTS)

/* Output:
PASS
*/
