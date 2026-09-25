#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/container/heap.h"
#include "burrow/container/list.h"
#include "burrow/container/ring.h"
#include "burrow/mem/arena.h"

/* Prints each value with a space before all but the first. */
static void print_value(void *env, Any v) {
    bool *first = env;
    if (!*first)
        fmt_print_v(" ");
    fmt_print_v(v);
    *first = false;
}

/* Go's Item and PriorityQueue from the container/heap example. The queue holds
 * pointers, so pop can hand back an Item that outlives its slot. */
typedef struct Item {
    const char *value;
    Int priority;
    Int index; /* kept up to date by swap, for heap_fix */
} Item;

static const Type item_type = {BURROW_S_INIT("Item"),
                               BURROW_S_INIT("main"),
                               KIND_STRUCT,
                               sizeof(Item),
                               _Alignof(Item),
                               0,
                               0,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               0,
                               0,
                               NULL};

typedef struct Queue {
    Item *items[8];
    Int n;
} Queue;

static Int queue_len(void *self) {
    return ((Queue *)self)->n;
}

/* Greater than, so that pop gives the highest priority first. */
static bool queue_less(void *self, Int i, Int j) {
    Queue *q = self;
    return q->items[i]->priority > q->items[j]->priority;
}

static void queue_swap(void *self, Int i, Int j) {
    Queue *q = self;
    Item *t = q->items[i];
    q->items[i] = q->items[j];
    q->items[j] = t;
    q->items[i]->index = i;
    q->items[j]->index = j;
}

static void queue_push(void *self, Any x) {
    Queue *q = self;
    Item *item = x.data;
    item->index = q->n;
    q->items[q->n++] = item;
}

static Any queue_pop(void *self) {
    Queue *q = self;
    Item *item = q->items[--q->n];
    item->index = -1;
    return BURROW_ANY(&item_type, item);
}

static const HeapInterfaceVT queue_vt = {
    {NULL, queue_len, queue_less, queue_swap}, queue_push, queue_pop};

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: list
    List l = LIST(a);
    ListElement *four = list_push_back(&l, BURROW_ANY_OF(4));
    list_push_front(&l, BURROW_ANY_OF("one"));
    list_insert_before(&l, BURROW_ANY_OF(3.5), four);
    list_move_to_back(&l, list_front(&l));

    for (ListElement *e = list_front(&l); e != NULL; e = list_element_next(e))
        fmt_println_v(e->value); /* 3.5, then 4, then one */
    // doc: end

    // doc: remove
    Any v = list_remove(&l, four);
    fmt_println_v("removed", v, "and", list_len(&l), "left"); /* removed 4 and 2 left */
    // doc: end

    // doc: ring
    Ring *r = ring_new(a, 4);
    for (int i = 1; i <= 4; i++) {
        r->value = any_box(a, BURROW_ANY_OF(i * 10));
        r = ring_next(r);
    }
    r = ring_move(r, 2);
    bool first = true;
    ring_do(r, BURROW_FN(RingDoFunc, print_value, &first)); /* 30 40 10 20 */
    printf("\n");
    // doc: end

    // doc: heap
    Item banana = {"banana", 3, 0}, apple = {"apple", 2, 1}, pear = {"pear", 4, 2};
    Queue q = {{&banana, &apple, &pear}, 3};
    HeapInterface h = {&queue_vt, &q};
    heap_init(h);

    Item orange = {"orange", 1, 0};
    heap_push(h, BURROW_ANY(&item_type, &orange));
    orange.priority = 5;
    heap_fix(h, orange.index);

    while (q.n > 0) {
        Item *item = any_assert(heap_pop(h), &item_type);
        printf("%.2lld:%s%s", (long long)item->priority, item->value,
               q.n > 0 ? " " : "\n");
    } /* 05:orange 04:pear 03:banana 02:apple */
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
3.5
4
one
removed 4 and 2 left
30 40 10 20
05:orange 04:pear 03:banana 02:apple
*/
