#include <stdio.h>

#include "burrow/burrow.h"

static void say(void *env) {
    printf("cleanup: %s\n", (const char *)env);
}

static void TestTempFiles(TestingT *t) {
    testing_t_cleanup(t, BURROW_FN(Func, say, "remove the first file"));
    testing_t_cleanup(t, BURROW_FN(Func, say, "remove the second file"));
    printf("test body done\n");
}

static void TestNeedsNetwork(TestingT *t) {
    testing_t_skip_v(t, "no network in this example");
}

static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("skipped tests do not fail the run: %d\n", code);
    return code;
}

#define TESTS(X) X(TestTempFiles) X(TestNeedsNetwork)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
test body done
cleanup: remove the second file
cleanup: remove the first file
PASS
skipped tests do not fail the run: 0
*/
