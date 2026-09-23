#include <string.h>

#include "burrow/burrow.h"

/* n copies of "word ", in one allocation from a. */
static Str join(Alloc *a, Int n) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)n * 5, 1);
    if (p == NULL)
        return BURROW_S("");
    for (Int i = 0; i < n; i++)
        memcpy(p + i * 5, "word ", 5);
    return str_from_bytes(p, n * 5);
}

// doc: bench
static void BenchmarkJoin(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    while (testing_b_loop(b)) {
        join(arena_allocator(&ar), 100);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void join_size(void *env, TestingB *b) {
    Int n = *(const Int *)env;
    testing_b_report_allocs(b);
    testing_b_set_bytes(b, n * 5);
    while (testing_b_loop(b)) {
        Str s = join(heap_allocator(), n);
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

static void BenchmarkJoinSizes(TestingB *b) {
    static const Int sizes[] = {10, 1000};
    testing_b_run(b, BURROW_S("10"),
                  BURROW_FN(TestingBFunc, join_size, (void *)&sizes[0]));
    testing_b_run(b, BURROW_S("1000"),
                  BURROW_FN(TestingBFunc, join_size, (void *)&sizes[1]));
}
// doc: end

static int TestMain(TestingM *m) {
    static char prog[] = "bench";
    static char bench[] = "-test.bench=.";
    static char benchtime[] = "-test.benchtime=100ms";
    static char *argv[] = {prog, bench, benchtime, NULL};
    testing_init(3, argv);
    return testing_m_run(m);
}

#define TESTS(X) X(BenchmarkJoin) X(BenchmarkJoinSizes)
TESTING_MAIN_WITH(TestMain, TESTS)
