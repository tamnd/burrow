#include <stdio.h>

#include "burrow/burrow.h"

static SyncAtomicBool stop;

static void spin(void *env) {
    // A loop with no blocking call in it has to give the thread up by hand.
    while (!sync_atomic_bool_load(&stop)) {
        // doc: gosched
        runtime_gosched();
        // doc: end
    }
}

static void run(void *env) {
    go(BURROW_FN(Func, spin, NULL));

    // doc: count
    int cpus = runtime_numcpu();       /* processors on the machine */
    int live = runtime_numgoroutine(); /* goroutines that exist right now */
    // doc: end

    sync_atomic_bool_store(&stop, true);
    printf("gomaxprocs %d, some processors: %s, goroutines %d\n", runtime_gomaxprocs(0),
           cpus > 0 ? "yes" : "no", live);
}

int main(void) {
    // doc: gomaxprocs
    runtime_gomaxprocs(4); /* before runtime_main */
    runtime_main(BURROW_FN(Func, run, NULL));
    // doc: end
    printf("outside runtime_main: some processors: %s, goroutines %d\n",
           runtime_numcpu() > 0 ? "yes" : "no", runtime_numgoroutine());
    return 0;
}

/* Output:
gomaxprocs 4, some processors: yes, goroutines 2
outside runtime_main: some processors: yes, goroutines 0
*/
