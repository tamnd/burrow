#include <stdio.h>

#include "burrow/burrow.h"

// doc: examples
static void ExampleStrClone(void) {
    Str s = str_clone(heap_allocator(), BURROW_S("gopher"));
    fmt_println_v(s, s.len);
    mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

static void ExampleFruit(void) {
    printf("banana\n");
    printf("apple\n");
    printf("cherry\n");
}

static void ExampleSum(void) {
    fmt_println_v(2 + 2);
}

static void ExampleDraft(void) {
    printf("not checked yet\n");
}

#define TESTS(X)                                                                       \
    X(ExampleStrClone, "gopher 6")                                                     \
    X(ExampleFruit, TESTING_UNORDERED("apple\nbanana\ncherry"))                        \
    X(ExampleSum, "5")                                                                 \
    X(ExampleDraft)
// doc: end

/* Prints the status and exits 0, so that the example itself counts as
 * passing. */
static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("exit status %d\n", code);
    return 0;
}

TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
--- FAIL: ExampleSum (0.00s)
got:
4
want:
5
FAIL
exit status 1
*/
