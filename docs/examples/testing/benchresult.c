#include <stdio.h>

#include "burrow/burrow.h"

// doc: body
static void copy_name(void *env, TestingB *b) {
    (void)env;
    while (testing_b_loop(b)) {
        Str s = str_clone(heap_allocator(), BURROW_S("gopher"));
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

int main(void) {
    TestingBenchmarkResult r =
        testing_benchmark(BURROW_FN(TestingBFunc, copy_name, NULL));
    printf("ran it %s times\n", r.n > 1000 ? "plenty of" : "too few");
    printf("%lld B/op, %lld allocs/op\n",
           (long long)testing_benchmark_result_alloced_bytes_per_op(r),
           (long long)testing_benchmark_result_allocs_per_op(r));
    testing_benchmark_result_free(&r);
    return 0;
}
// doc: end

/* Output:
ran it plenty of times
6 B/op, 1 allocs/op
*/
