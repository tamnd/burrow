// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/container/list.h"

#define ENTRY_FIELDS(F, T)                                                             \
    F(T, Str, Key, "")                                                                 \
    F(T, Int, Value, "")
BURROW_STRUCT(Entry, ENTRY_FIELDS);

/* A cache that holds at most max entries and, when it is full, drops the one
 * used longest ago. The list keeps the entries in the order they were used,
 * most recent at the front, and the map finds an entry's place in the list. */
typedef struct Lru {
    Alloc *a;
    Int max;
    List order;
    Map *where; /* Str to ListElement * */
} Lru;

static Entry *entry_of(ListElement *e) {
    return e->value.data;
}

static bool lru_get(Lru *c, Str key, Int *value) {
    UnsafePointer *e = MAP_GET(Str, UnsafePointer, c->where, key);
    if (e == NULL)
        return false;
    list_move_to_front(&c->order, *e);
    *value = entry_of(*e)->Value;
    return true;
}

static void lru_put(Lru *c, Str key, Int value) {
    UnsafePointer *e = MAP_GET(Str, UnsafePointer, c->where, key);
    if (e != NULL) {
        entry_of(*e)->Value = value;
        list_move_to_front(&c->order, *e);
        return;
    }
    if (list_len(&c->order) == c->max) {
        ListElement *oldest = list_back(&c->order);
        fmt_println_v("dropping", entry_of(oldest)->Key);
        MAP_DEL(Str, c->where, entry_of(oldest)->Key);
        list_remove(&c->order, oldest);
    }
    Entry *n = BURROW_NEW(c->a, Entry);
    n->Key = key;
    n->Value = value;
    UnsafePointer at = list_push_front(&c->order, ANY(TYPE_OF(Entry), n));
    MAP_SET(Str, UnsafePointer, c->where, key, at);
}

static void show(Lru *c, Str key) {
    Int v;
    if (lru_get(c, key, &v))
        fmt_printf_v("%s = %d\n", key, v);
    else
        fmt_printf_v("%s is not in the cache\n", key);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Lru c = {a, 2, LIST(a), map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0)};
    lru_put(&c, S("a"), 1);
    lru_put(&c, S("b"), 2);
    show(&c, S("a")); /* a is now the most recent, so b goes next */
    lru_put(&c, S("c"), 3);
    show(&c, S("b"));
    lru_put(&c, S("d"), 4);

    fmt_print_v("most recent first:");
    for (ListElement *e = list_front(&c.order); e != NULL; e = list_element_next(e))
        fmt_print_v(" ", entry_of(e)->Key);
    fmt_print_v("\n");
    arena_free(&ar);
}
// doc: end

/* Output:
a = 1
dropping b
b is not in the cache
dropping a
most recent first: d c
*/
