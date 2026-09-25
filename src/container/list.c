/* container/list.
 *
 * Derived from Go's src/container/list/list.go.
 * Go source: go1.27.1.
 *
 * The one real difference from Go is where a value lives. Go's element holds
 * an interface, and the interface points at a copy of the value on the heap
 * when the value is not already a pointer. Here the copy goes in the same
 * block as the element, just after it, so a push is one allocation and so is
 * a free.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/container/list.h"

#include "burrow/mem/heap.h"
#include "burrow/type.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

static Alloc *list_alloc(const List *l) {
    return l->a != NULL ? l->a : heap_allocator();
}

List *list_new(Alloc *a) {
    List *l = mem_alloc(a, sizeof *l, alignof(List));
    if (l == NULL)
        return NULL;
    l->a = a;
    return list_init(l);
}

List *list_init(List *l) {
    l->root.next = &l->root;
    l->root.prev = &l->root;
    l->len = 0;
    return l;
}

Int list_len(const List *l) {
    return l->len;
}

ListElement *list_front(const List *l) {
    if (l->len == 0)
        return NULL;
    return l->root.next;
}

ListElement *list_back(const List *l) {
    if (l->len == 0)
        return NULL;
    return l->root.prev;
}

ListElement *list_element_next(const ListElement *e) {
    ListElement *p = e->next;
    if (e->list != NULL && p != &e->list->root)
        return p;
    return NULL;
}

ListElement *list_element_prev(const ListElement *e) {
    ListElement *p = e->prev;
    if (e->list != NULL && p != &e->list->root)
        return p;
    return NULL;
}

static void list_lazy_init(List *l) {
    if (l->root.next == NULL)
        list_init(l);
}

static ListElement *list_insert(List *l, ListElement *e, ListElement *at) {
    e->prev = at;
    e->next = at->next;
    e->prev->next = e;
    e->next->prev = e;
    e->list = l;
    l->len++;
    return e;
}

/* An element with a copy of v after it, in one block. A nil value and a zero
 * sized one need no room, and get the same Any that any_box would give them. */
static ListElement *list_element_make(List *l, Any v) {
    size_t size = sizeof(ListElement);
    size_t align = alignof(ListElement);
    size_t off = 0;
    bool copy = v.t != NULL && v.data != NULL && v.t->size != 0;
    if (copy) {
        size_t va = v.t->align != 0 ? v.t->align : 1;
        off = (size + va - 1) / va * va;
        size = off + v.t->size;
        if (va > align)
            align = va;
    }

    ListElement *e = mem_alloc(list_alloc(l), size, align);
    if (e == NULL)
        return NULL;
    e->size = size;
    e->align = align;
    e->value.t = v.t;
    e->value.data = NULL;
    if (copy) {
        void *p = (unsigned char *)e + off;
        type_copy(v.t, p, v.data);
        e->value.data = p;
    }
    return e;
}

static ListElement *list_insert_value(List *l, Any v, ListElement *at) {
    ListElement *e = list_element_make(l, v);
    if (e == NULL)
        return NULL;
    return list_insert(l, e, at);
}

static void list_unlink(List *l, ListElement *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->next = NULL;
    e->prev = NULL;
    e->list = NULL;
    l->len--;
}

static void list_move(ListElement *e, ListElement *at) {
    if (e == at)
        return;
    e->prev->next = e->next;
    e->next->prev = e->prev;

    e->prev = at;
    e->next = at->next;
    e->prev->next = e;
    e->next->prev = e;
}

Any list_remove(List *l, ListElement *e) {
    /* If e->list == l, l must have been initialised when e was inserted in l
     * or l == NULL (e is a zero Element) and l.remove will crash. */
    if (e->list == l)
        list_unlink(l, e);
    return e->value;
}

ListElement *list_push_front(List *l, Any v) {
    list_lazy_init(l);
    return list_insert_value(l, v, &l->root);
}

ListElement *list_push_back(List *l, Any v) {
    list_lazy_init(l);
    return list_insert_value(l, v, l->root.prev);
}

ListElement *list_insert_before(List *l, Any v, ListElement *mark) {
    if (mark->list != l)
        return NULL;
    /* see comment in list_remove about initialization of l */
    return list_insert_value(l, v, mark->prev);
}

ListElement *list_insert_after(List *l, Any v, ListElement *mark) {
    if (mark->list != l)
        return NULL;
    /* see comment in list_remove about initialization of l */
    return list_insert_value(l, v, mark);
}

void list_move_to_front(List *l, ListElement *e) {
    if (e->list != l || l->root.next == e)
        return;
    /* see comment in list_remove about initialization of l */
    list_move(e, &l->root);
}

void list_move_to_back(List *l, ListElement *e) {
    if (e->list != l || l->root.prev == e)
        return;
    /* see comment in list_remove about initialization of l */
    list_move(e, l->root.prev);
}

void list_move_before(List *l, ListElement *e, ListElement *mark) {
    if (e->list != l || e == mark || mark->list != l)
        return;
    list_move(e, mark->prev);
}

void list_move_after(List *l, ListElement *e, ListElement *mark) {
    if (e->list != l || e == mark || mark->list != l)
        return;
    list_move(e, mark);
}

/* Both count first and walk after, as Go does, so a list pushed onto itself
 * copies what it held at the start and stops. */
bool list_push_back_list(List *l, const List *other) {
    list_lazy_init(l);
    ListElement *e = list_front(other);
    for (Int i = list_len(other); i > 0; i--, e = list_element_next(e)) {
        if (list_insert_value(l, e->value, l->root.prev) == NULL)
            return false;
    }
    return true;
}

bool list_push_front_list(List *l, const List *other) {
    list_lazy_init(l);
    ListElement *e = list_back(other);
    for (Int i = list_len(other); i > 0; i--, e = list_element_prev(e)) {
        if (list_insert_value(l, e->value, &l->root) == NULL)
            return false;
    }
    return true;
}

void list_free_element(List *l, ListElement *e) {
    if (e->list == l)
        list_unlink(l, e);
    mem_free(list_alloc(l), e, e->size, e->align);
}

void list_free(List *l) {
    if (l->root.next == NULL)
        return;
    Alloc *a = list_alloc(l);
    ListElement *e = l->root.next;
    while (e != &l->root) {
        ListElement *next = e->next;
        mem_free(a, e, e->size, e->align);
        e = next;
    }
    list_init(l);
}
