#include <stdio.h>

// doc: add
#include "burrow/sync/atomic.h"

static int64_t hits;

void serve(void) {
    sync_atomic_add_int64(&hits, 1);
}
// doc: end

static void count(void) {
    // doc: now
    int64_t now = sync_atomic_add_int64(&hits, 1);
    // doc: end
    printf("hits %lld\n", (long long)now);
}

static void subtract(uint64_t n, uint64_t k) {
    // doc: sub
    sync_atomic_add_uint64(&n, ~(uint64_t)0); /* n = n - 1 */
    sync_atomic_add_uint64(&n, -(uint64_t)k); /* n = n - k */
    // doc: end
    printf("n %llu\n", (unsigned long long)n);
}

#define WRITABLE 0x2u

static uint32_t flags;
static int opened;

static void open_for_writing(void) {
    opened++;
}

static void set_writable(void) {
    // doc: or
    uint32_t before = sync_atomic_or_uint32(&flags, WRITABLE);
    if ((before & WRITABLE) == 0)
        open_for_writing(); /* we are the one who set it */
    // doc: end
}

static int64_t n = 21;

static void double_n(void) {
    // doc: cas
    for (;;) {
        int64_t old = sync_atomic_load_int64(&n);

        if (sync_atomic_compare_and_swap_int64(&n, old, old * 2))
            break;
    }
    // doc: end
}

typedef struct Node {
    struct Node *next;
    int value;
} Node;

// doc: push
static void *head;

static void push(Node *n) {
    n->next = sync_atomic_load_pointer(&head);
    while (!sync_atomic_compare_and_swap_pointer(&head, n->next, n))
        n->next = sync_atomic_load_pointer(&head);
}
// doc: end

int main(void) {
    serve();
    serve();
    count();
    subtract(10, 3);

    set_writable();
    set_writable();
    printf("opened %d time\n", opened);

    double_n();
    printf("doubled %lld\n", (long long)n);

    Node nodes[3] = {{NULL, 1}, {NULL, 2}, {NULL, 3}};
    for (int i = 0; i < 3; i++)
        push(&nodes[i]);
    for (Node *p = head; p != NULL; p = p->next)
        printf("node %d\n", p->value);
    return 0;
}

/* Output:
hits 3
n 6
opened 1 time
doubled 42
node 3
node 2
node 1
*/
