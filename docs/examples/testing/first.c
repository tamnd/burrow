#include "burrow/burrow.h"

static Int abs_int(Int v) {
    return v < 0 ? -v : v;
}

// doc: tests
static void TestAbs(TestingT *t) {
    Int got = abs_int(-1);
    if (got != 1)
        testing_t_errorf_v(t, "abs_int(-1) = %d; want 1", got);
}

static void TestAbsZero(TestingT *t) {
    if (abs_int(0) != 0)
        testing_t_error_v(t, "abs_int(0) is not 0");
}

#define TESTS(X) X(TestAbs) X(TestAbsZero)
TESTING_MAIN(TESTS)
// doc: end

/* Output:
PASS
*/
