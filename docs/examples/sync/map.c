#include <stdio.h>

#include "burrow/burrow.h"

// doc: map
static SyncMap cache;

void setup(void) {
    cache = SYNC_MAP(heap_allocator(), TYPE_STRING, TYPE_INT);
}

void record(Str name, Int n) {
    sync_map_store(&cache, &name, &n);
}

bool lookup(Str name, Int *out) {
    return sync_map_load(&cache, &name, out);
}
// doc: end

static void use(Int n) {
    printf("hits is %lld\n", (long long)n);
}

static void typed(void) {
    // doc: macros
    BURROW_SYNC_MAP_STORE(Str, Int, &cache, BURROW_S("hits"), 1);

    Int n;
    if (BURROW_SYNC_MAP_LOAD(Str, &cache, BURROW_S("hits"), &n))
        use(n);
    // doc: end
}

static Error remember(Str k, Int n) {
    // doc: load-or-store
    bool loaded;
    Int actual;
    if (!sync_map_load_or_store(&cache, &k, &n, &actual, &loaded))
        return burrow_err_out_of_memory;
    // doc: end
    printf(BURROW_STR_FMT " was there already %d, holds %lld\n", BURROW_STR_ARG(k),
           loaded, (long long)actual);
    return BURROW_NO_ERROR;
}

// doc: range
static bool print_one(const void *key, const void *val, void *arg) {
    const Str *k = key;
    (void)arg;
    printf(BURROW_STR_FMT " = %lld\n", BURROW_STR_ARG(*k),
           (long long)*(const Int *)val);
    return true;
}

void dump(void) {
    sync_map_range(&cache, print_one, NULL);
}
// doc: end

int main(void) {
    setup();
    record(BURROW_S("misses"), 7);
    Int got = 0;
    bool found = lookup(BURROW_S("misses"), &got);
    printf("found misses %d, it is %lld\n", found, (long long)got);
    typed();
    remember(BURROW_S("hits"), 5);
    remember(BURROW_S("errors"), 2);
    BURROW_SYNC_MAP_DELETE(Str, &cache, BURROW_S("misses"));
    BURROW_SYNC_MAP_DELETE(Str, &cache, BURROW_S("errors"));
    dump();
    sync_map_free(&cache);
    return 0;
}

/* Output:
found misses 1, it is 7
hits is 1
hits was there already 1, holds 1
errors was there already 0, holds 2
hits = 1
*/
