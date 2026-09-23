#include "burrow/proc.h"

static void run(void *env) {
    (void)env;
    /* This is func main. */
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}
