#include "burrow/burrow.h"

typedef struct Case {
    const char *name;
    const char *in;
    Int want;
} Case;

static const Case cases[] = {
    {"empty", "", 0},
    {"ascii", "hello", 5},
    {"accents", "h\xc3\xa9llo", 5},
};

static void run_case(void *env, TestingT *t) {
    const Case *c = env;
    Int got = utf8_rune_count_in_string(str_from_cstr(c->in));
    if (got != c->want)
        testing_t_errorf_v(t, "got %d runes, want %d", got, c->want);
}

static void TestRuneCount(TestingT *t) {
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, run_case, (void *)&cases[i]));
}

static void TestLog(TestingT *t) {
    testing_t_log_v(t, "this line only shows with -test.v or on failure");
}

/* A real test binary gets -test.v from the shell. This one has no command
 * line, so its TestMain hands testing_init one before the run parses it. */
static int TestMain(TestingM *m) {
    static char prog[] = "verbose";
    static char flag[] = "-test.v";
    static char *argv[] = {prog, flag, NULL};
    testing_init(2, argv);
    return testing_m_run(m);
}

#define TESTS(X) X(TestRuneCount) X(TestLog)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
=== RUN   TestRuneCount
=== RUN   TestRuneCount/empty
=== RUN   TestRuneCount/ascii
=== RUN   TestRuneCount/accents
--- PASS: TestRuneCount (0.00s)
    --- PASS: TestRuneCount/empty (0.00s)
    --- PASS: TestRuneCount/ascii (0.00s)
    --- PASS: TestRuneCount/accents (0.00s)
=== RUN   TestLog
    verbose.c:29: this line only shows with -test.v or on failure
--- PASS: TestLog (0.00s)
PASS
*/
