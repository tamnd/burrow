#include <stdio.h>

#include "burrow/burrow.h"

#define BUF_FIELDS(F, T)                                                               \
    F(T, Int, len, "")                                                                 \
    F(T, Int, sum, "")
BURROW_STRUCT(Buf, BUF_FIELDS);

static int made;

static void fill(Buf *b) {
    for (Int i = 1; i <= 4; i++) {
        b->sum += i;
        b->len++;
    }
}

// doc: pool
static Any make_buf(void *env) {
    (void)env;
    Buf *b = BURROW_NEW(heap_allocator(), Buf);
    return BURROW_ANY(TYPE_OF(Buf), b);
}

static void drop_buf(void *env, Any v) {
    (void)env;
    mem_free(heap_allocator(), v.data, sizeof(Buf), _Alignof(Buf));
}

static SyncPool bufs;

void setup(void) {
    bufs = SYNC_POOL(heap_allocator(), BURROW_FN(SyncPoolNewFunc, make_buf, NULL),
                     BURROW_FN(SyncPoolFreeFunc, drop_buf, NULL));
}

void work(void) {
    Any v = sync_pool_get(&bufs);
    Buf *b = any_assert(v, TYPE_OF(Buf));

    b->len = 0;
    fill(b);

    sync_pool_put(&bufs, v);
}
// doc: end

int main(void) {
    setup();
    work();
    Any v = sync_pool_get(&bufs);
    Buf *b = any_assert(v, TYPE_OF(Buf));
    made = b != NULL;
    printf("got a buffer %d\n", made);
    sync_pool_put(&bufs, v);
    sync_pool_free(&bufs);
    return 0;
}

/* Output:
got a buffer 1
*/
