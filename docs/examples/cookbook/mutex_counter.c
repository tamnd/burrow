// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

enum { nvisitors = 4, visits = 1000 };

static const Str pages[] = {BURROW_S_INIT("/"), BURROW_S_INIT("/about"),
                            BURROW_S_INIT("/blog")};

/* Hits for each page, shared by every goroutine. A map is not safe to change
 * from two goroutines at once, so every use of hits holds mu. */
static SyncMutex mu;
static Map *hits;
static SyncOnce once;

/* Makes the map the first time anyone needs it. However many goroutines get
 * here together, it runs once, and the others wait until it has finished. */
static void setup(void *env) {
    (void)env;
    hits = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
}

static void hit(Str page) {
    sync_once_do(&once, FN(Func, setup, NULL));
    sync_mutex_lock(&mu);
    Int *n = MAP_GET(Str, Int, hits, page);
    MAP_SET(Str, Int, hits, page, n != NULL ? *n + 1 : 1);
    sync_mutex_unlock(&mu);
}

static void visitor(void *env) {
    Int id = (Int)(intptr_t)env;
    for (Int i = 0; i < visits; i++)
        hit(pages[(id + i) % 3]);
}

static void run(void *env) {
    (void)env;
    SyncWaitGroup wg = {0};
    for (Int id = 0; id < nvisitors; id++)
        sync_wait_group_go(&wg, FN(Func, visitor, (void *)(intptr_t)id));
    sync_wait_group_wait(&wg);

    /* Every visitor has finished, so nothing else touches the map now. */
    Int total = 0;
    for (Int i = 0; i < 3; i++) {
        Int n = *MAP_GET(Str, Int, hits, pages[i]);
        fmt_printf_v("%-7s %d\n", pages[i], n);
        total += n;
    }
    fmt_println_v("total", total);
    map_free(hits);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
/       1334
/about  1333
/blog   1333
total 4000
*/
