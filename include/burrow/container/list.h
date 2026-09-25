/* container/list, a doubly linked list.
 *
 * Go's container/list. Every element holds one value of any type, as an Any,
 * and the list keeps its own copy of that value, the way a Go interface does:
 *
 *     List l = LIST(a);
 *     list_push_back(&l, BURROW_ANY_OF(1));
 *     list_push_back(&l, BURROW_ANY_OF("two"));
 *     for (ListElement *e = list_front(&l); e != NULL; e = list_element_next(e))
 *         fmt_println_v(e->value);
 *
 * A zeroed List is an empty list ready to use, as in Go, and gets its elements
 * from heap_allocator. LIST names a different allocator.
 *
 * Go has a collector and C does not, so there are two calls Go does not have.
 * list_remove leaves the element and its value where they are, because Go code
 * reads e.Value after removing e and expects it to be there. list_free_element
 * gives one of them back, and list_free gives back everything still in a list.
 * With an arena neither is needed.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package container/list */

#ifndef BURROW_CONTAINER_LIST_H
#define BURROW_CONTAINER_LIST_H

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/mem.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct List List;
typedef struct ListElement ListElement;

/* list.Element. value is Go's Value field and is yours to read and to set. A
 * value you set yourself is not copied, so whatever it points at has to
 * outlive the element. */
struct ListElement {
    /* The implementation's. The links, the list this element is in, which is
     * NULL once it has been removed, and the size and alignment of the block
     * it was allocated as, for list_free_element. */
    ListElement *next;
    ListElement *prev;
    List *list;
    size_t size;
    size_t align;

    Any value;
};

/* list.List. The root is a sentinel that is never handed out, and a zeroed
 * root is how an unused list says it has not been set up yet. */
struct List {
    /* Where the elements come from. NULL means heap_allocator. */
    Alloc *a;

    /* The implementation's. */
    ListElement root;
    Int len;
};

/* An empty list that allocates from alloc. Works in a block and at file scope,
 * where alloc has to be a constant or NULL. */
#define LIST(alloc) ((List){.a = (alloc)})

/* Go's New. A list allocated from a, which also holds its elements. NULL when
 * a is out of memory. Give it back with list_free and then mem_free. */
BURROW_OWNS(ret) List *list_new(Alloc *a);

/* Go's Init: empties l and returns it. Like Go this only forgets the elements,
 * so free them first with list_free if they are not in an arena. */
BURROW_BORROWS(ret, l) List *list_init(List *l);

/* The number of elements, in constant time. */
Int list_len(const List *l);

/* The first and last element, or NULL for an empty list. */
BURROW_BORROWS(ret, l) ListElement *list_front(const List *l);
BURROW_BORROWS(ret, l) ListElement *list_back(const List *l);

/* The element after and before e, or NULL at either end and for an element
 * that is not in a list. */
BURROW_BORROWS(ret, e) ListElement *list_element_next(const ListElement *e);
BURROW_BORROWS(ret, e) ListElement *list_element_prev(const ListElement *e);

/* Takes e out of l if it is in l, and returns its value either way. e and the
 * value stay allocated, so both are still good to read afterwards. */
BURROW_BORROWS(ret, e) Any list_remove(List *l, ListElement *e);

/* Puts a copy of v at the front or the back and returns its element, or NULL
 * when the allocator is out of memory. */
BURROW_BORROWS(ret, l) ListElement *list_push_front(List *l, Any v);
BURROW_BORROWS(ret, l) ListElement *list_push_back(List *l, Any v);

/* Puts a copy of v just before or just after mark and returns its element.
 * Returns NULL and leaves l alone when mark is not in l, which is Go's answer,
 * and also when the allocator is out of memory. */
BURROW_BORROWS(ret, l) ListElement *list_insert_before(List *l, Any v,
                                                       ListElement *mark);
BURROW_BORROWS(ret, l) ListElement *list_insert_after(List *l, Any v,
                                                      ListElement *mark);

/* Moves e to the front or the back. Nothing happens when e is not in l. */
void list_move_to_front(List *l, ListElement *e);
void list_move_to_back(List *l, ListElement *e);

/* Moves e to just before or just after mark. Nothing happens when e and mark
 * are the same element or either of them is not in l. */
void list_move_before(List *l, ListElement *e, ListElement *mark);
void list_move_after(List *l, ListElement *e, ListElement *mark);

/* Appends or prepends a copy of every value in other, which may be l itself.
 * Go's versions return nothing. These return false when the allocator ran out
 * partway, in which case some of the values were added and the rest were not. */
bool list_push_back_list(List *l, const List *other);
bool list_push_front_list(List *l, const List *other);

/* Gives e back to the allocator of list l, taking it out of l first if it is
 * still in it. e must have come from l, and neither e nor the value it held
 * can be used afterwards. */
void list_free_element(List *l, ListElement *e);

/* Gives every element still in l back and leaves l empty and usable. */
void list_free(List *l);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CONTAINER_LIST_H */
