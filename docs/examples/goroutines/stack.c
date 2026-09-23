#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static SyncWaitGroup wg;

// Recursive descent with a frame big enough that nesting a few thousand deep
// wants more than the default stack.
static Int depth(const char *s, Int i) {
    char frame[512];
    memset(frame, s[i], sizeof frame);
    if (s[i] != '(')
        return 0;
    return 1 + depth(s, i + 1) + (frame[0] == '(' ? 0 : 1);
}

static void parse(void *env) {
    const char *input = env;
    printf("nested %lld deep\n", (long long)depth(input, 0));
    sync_wait_group_done(&wg);
}

static char input[5001];

static void run(void *env) {
    memset(input, '(', 5000);
    sync_wait_group_add(&wg, 1);
    // doc: stack
    go_stack(BURROW_FN(Func, parse, input), 4 * 1024 * 1024);
    // doc: end
    sync_wait_group_wait(&wg);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
nested 5000 deep
*/
