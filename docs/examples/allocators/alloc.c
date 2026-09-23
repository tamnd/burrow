#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

// doc: annotations
BURROW_OWNS(ret) Str str_clone(Alloc *a, Str s);
BURROW_BORROWS(ret, p) Str str_from_bytes(const void *p, Int n);
BURROW_STATIC(ret) const char *burrow_version(void);
// doc: end

// clang-format off
// doc: append
BURROW_OWNS(ret) BURROW_BORROWS(ret, s)
Slice slice_append(Alloc *a, Slice s, const void *elems, Int n);
// doc: end
// clang-format on

typedef struct Request {
    Int id;
} Request;

static Request pending[3] = {{1}, {2}, {3}};
static Int next_request;

static Request *accept_one(void) {
    return next_request < 3 ? &pending[next_request++] : NULL;
}

static void handle(Alloc *a, Request *req) {
    Int *scratch = mem_alloc(a, 1024 * sizeof(Int), _Alignof(Int));
    scratch[0] = req->id;
    printf("handled request %lld\n", (long long)scratch[0]);
}

static void map_keeps(Alloc *a) {
    Str key = BURROW_S("answer");
    Int val = 42;

    // doc: map
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    map_set(m, &key, &val); /* this can grow the table */
    // doc: end
    printf("the map has %lld entry\n", (long long)map_len(m));
}

static void arena(void) {
    {
        // doc: arena
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        /* allocate from a as much as you like */
        arena_free(&ar);
        // doc: end
        (void)a;
    }
    {
        // doc: server
        Arena ar;
        arena_init(&ar, NULL, 0);

        Request *req;
        while ((req = accept_one()) != NULL) {
            handle(arena_allocator(&ar), req);
            arena_reset(&ar);
        }
        // doc: end
        arena_free(&ar);
    }
    {
        // doc: nest
        Arena conn;
        arena_init(&conn, NULL, 0);

        Arena req;
        arena_init(&req, arena_allocator(&conn), 0);
        // doc: end
        map_keeps(arena_allocator(&req));
        arena_free(&req);
        arena_free(&conn);
    }
}

static void heap(void) {
    // doc: heap
    Alloc *a = heap_allocator();
    // doc: end
    Byte *p = mem_alloc(a, 16, 1);
    mem_free(a, p, 16, 1);
    printf("the heap is never NULL %d\n", a != NULL);
}

static void gc(Str line) {
    // doc: gc
    Alloc *a = gc_allocator();
    if (a == NULL) {
        printf("this build does not have the collector in it\n");
        return;
    }

    Str kept = str_clone(a, line);
    /* never free anything */
    // doc: end
    printf("kept %lld bytes\n", (long long)kept.len);
}

static void track(void) {
    Int n = 64;

    // doc: track
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);

    /* run the thing under test, passing a */

    if (track_check(&tr) != 0)
        printf("something is wrong and track_check has already said what\n");
    track_free(&tr);
    // doc: end

    track_init(&tr, heap_allocator());
    a = track_allocator(&tr);
    // doc: here
    Byte *p = mem_alloc(TRACK_HERE(a), n, 1);
    // doc: end
    mem_free(a, p, (size_t)n, 1);
    printf("faults after a clean alloc and free %llu\n",
           (unsigned long long)track_check(&tr));
    track_free(&tr);
}

static void fixed(void) {
    // doc: fixed
    unsigned char buf[4096];
    Fixed fx;
    fixed_init(&fx, buf, sizeof(buf));
    Alloc *a = fixed_allocator(&fx);
    // doc: end

    void *fits = mem_alloc(a, 4000, 1);
    void *too_much = mem_alloc(a, 4000, 1);
    printf("the first fits %d, the second fails %d\n", fits != NULL, too_much == NULL);
}

// doc: oom
static bool out_of_room(void *ctx, size_t size, size_t align) {
    (void)ctx;
    (void)align;
    fprintf(stderr, "could not get %zu bytes\n", size);
    return false;
}

static void setup(Alloc *a) {
    mem_set_oom(a, out_of_room, NULL);
}
// doc: end

/* A small allocator that counts, over the heap. */
typedef struct MyState {
    Int allocs;
} MyState;

static MyState my_state;

static void *my_alloc(void *self, size_t size, size_t align) {
    ((MyState *)self)->allocs++;
    return mem_alloc_nozero(heap_allocator(), size, align);
}

static void *my_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    ((MyState *)self)->allocs++;
    return mem_realloc(heap_allocator(), p, old, nsz, align);
}

static void my_free(void *self, void *p, size_t size, size_t align) {
    (void)self;
    mem_free(heap_allocator(), p, size, align);
}

// doc: custom
static const AllocVT my_vt = {
    my_alloc, NULL, my_realloc, my_free, NULL, NULL,
};

Alloc my_allocator = {.vt = &my_vt, .self = &my_state};
// doc: end

static void custom(void) {
    Arena ar;
    arena_init(&ar, &my_allocator, 0);
    for (Int i = 0; i < 100; i++)
        mem_alloc(arena_allocator(&ar), 64, 8);
    arena_free(&ar);
    printf("the arena got its chunks from my allocator %d\n", my_state.allocs > 0);
}

int main(void) {
    arena();
    heap();
    gc(BURROW_S("a,b,c"));
    track();
    fixed();
    setup(heap_allocator());
    mem_set_oom(heap_allocator(), NULL, NULL);
    custom();
    return 0;
}

/* Output:
handled request 1
handled request 2
handled request 3
the map has 1 entry
the heap is never NULL 1
this build does not have the collector in it
faults after a clean alloc and free 0
the first fits 1, the second fails 1
the arena got its chunks from my allocator 1
*/
