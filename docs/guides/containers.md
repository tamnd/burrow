# Containers

`burrow/container/list.h`, `burrow/container/ring.h` and `burrow/container/heap.h` are Go's `container/list`, `container/ring` and `container/heap`: a doubly linked list and a circular list, each holding values of any type, and a priority queue over a slice you own.

The names follow the usual rule. `list.List` is `List` and `ring.Ring` is `Ring`, since the type has the same name as its package, and the methods are functions with the type in front: `l.PushBack(v)` is `list_push_back(&l, v)` and `e.Next()` is `list_element_next(e)`.

## Lists

A `List` holds `Any` values. `LIST(a)` makes an empty one that takes its elements from `a`, and a zeroed `List` is empty too and uses `heap_allocator`, which is as close as C gets to Go's `var l list.List`:

<!-- example: ../examples/container/container.c#list -->
```c
List l = LIST(a);
ListElement *four = list_push_back(&l, BURROW_ANY_OF(4));
list_push_front(&l, BURROW_ANY_OF("one"));
list_insert_before(&l, BURROW_ANY_OF(3.5), four);
list_move_to_back(&l, list_front(&l));

for (ListElement *e = list_front(&l); e != NULL; e = list_element_next(e))
    fmt_println_v(e->value); /* 3.5, then 4, then one */
```

The list keeps its own copy of each value, in the same allocation as the element. That is what a Go interface does when you store a value in it, and it means `BURROW_ANY_OF` on a temporary is fine. `e->value` is Go's `Value` field, and you can set it yourself, but a value you set is not copied, so what it points at has to outlive the element.

Everything Go's list has is here: `list_front` and `list_back`, `list_push_front` and `list_push_back`, `list_insert_before` and `list_insert_after`, the four moves, and `list_push_back_list` and `list_push_front_list`. Operations that name an element from a different list do nothing, as in Go. A push returns `NULL` when the allocator is out of memory, and the two list pushes return `false` when that happens partway through.

## Removing and freeing

Go code reads `e.Value` after `l.Remove(e)`, and the value `Remove` returns stays valid for as long as you hold it. So `list_remove` takes the element out of the list but leaves its memory alone:

<!-- example: ../examples/container/container.c#remove -->
```c
Any v = list_remove(&l, four);
fmt_println_v("removed", v, "and", list_len(&l), "left"); /* removed 4 and 2 left */
```

With an arena that's all you need, since the arena gives everything back at once. With the heap there are two calls Go doesn't need. `list_free_element` gives back one element, removing it first if it's still in the list, and `list_free` gives back everything still in the list and leaves it empty. `list_init` is Go's `Init`, and like Go's it only forgets the elements, so free them first if they need freeing.

## Rings

A ring has no start and no end. A pointer to any element stands for the whole ring, `NULL` is the empty ring, and a zeroed `Ring` is a ring of one. `ring_new` makes `n` elements with nil values:

<!-- example: ../examples/container/container.c#ring -->
```c
Ring *r = ring_new(a, 4);
for (int i = 1; i <= 4; i++) {
    r->value = any_box(a, BURROW_ANY_OF(i * 10));
    r = ring_next(r);
}
r = ring_move(r, 2);
bool first = true;
ring_do(r, BURROW_FN(RingDoFunc, print_value, &first)); /* 30 40 10 20 */
printf("\n");
```

Unlike a list, a ring never copies or even looks at its values, which is Go's "for use by client; untouched by this library". A value set from a temporary has to be boxed with `any_box` first, as above, or it will point at something that has gone away.

`ring_link` and `ring_unlink` are Go's `Link` and `Unlink`: linking two rings splices them together, linking two elements of the same ring cuts out the elements between them, and unlinking `n` elements returns them as a ring of their own. `ring_do` takes a `RingDoFunc`, which is Go's `func(any)` with an environment pointer. `ring_free` gives back every element of a ring made by `ring_new`. All of them have to come from the allocator you pass, so don't free a ring you have linked a stack `Ring` into.

## See also

- [interfaces.md](interfaces.md), for `Any` and `any_box`.
- [allocators.md](allocators.md), for arenas and the heap.

## Heaps

A heap is not a type of its own. It is any `HeapInterface`, which is a `SortInterface` with `push` and `pop` added, and the functions in `burrow/container/heap.h` keep it in heap order so that the smallest element as `less` sees it is at index 0. This is Go's priority queue example, where `less` compares with greater than so the highest priority comes out first:

<!-- example: ../examples/container/container.c#heap -->
```c
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
```

`push` appends at index `len` and `pop` removes the element at `len - 1`. They are how the heap grows and shrinks, and the heap functions call them at the right moment, so call `heap_push` and `heap_pop` yourself and never the methods. `heap_remove` takes out any element by index and `heap_fix` puts one back in order after its value has changed, which is why the example keeps each item's index up to date in `swap`.

The `Any` that `pop` returns has to stay valid after the slot is gone. Here the queue holds pointers to items the caller owns, which is the usual way, and a queue of plain values can return a pointer to the slot it just gave up, as long as the caller reads it before the next push.
