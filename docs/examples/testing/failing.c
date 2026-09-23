#include <stdio.h>

#include "burrow/burrow.h"

static void open_config(void *env, TestingT *t) {
    testing_t_fatal_v(t, "no config file");
    testing_t_log_v(t, "never printed, Fatal ended the subtest");
}

static void parse_config(void *env, TestingT *t) {
    testing_t_errorf_v(t, "line %d: want a key", 3);
    testing_t_errorf_v(t, "line %d: want a value", 7);
}

static void TestConfig(TestingT *t) {
    testing_t_run(t, BURROW_S("open"), BURROW_FN(TestingTFunc, open_config, NULL));
    testing_t_run(t, BURROW_S("parse"), BURROW_FN(TestingTFunc, parse_config, NULL));
}

static void TestFine(TestingT *t) {
    testing_t_log_v(t, "a passing test's log is not printed");
}

/* A TestMain gets the M and returns the exit status. This one prints the
 * status and exits 0 so that the example itself counts as passing. */
static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("exit status %d\n", code);
    return 0;
}

#define TESTS(X) X(TestConfig) X(TestFine)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
--- FAIL: TestConfig (0.00s)
    --- FAIL: TestConfig/open (0.00s)
        failing.c:6: no config file
    --- FAIL: TestConfig/parse (0.00s)
        failing.c:11: line 3: want a key
        failing.c:12: line 7: want a value
FAIL
exit status 1
*/
