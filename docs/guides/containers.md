# Containers

`burrow/container/list.h` and `burrow/container/ring.h` are Go's `container/list` and `container/ring`: a doubly linked list and a circular list, each holding values of any type. `container/heap` comes with `sort`, because a heap is defined by `sort.Interface`.

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
