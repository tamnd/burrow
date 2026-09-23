#include <stdio.h>

#include "burrow/burrow.h"

typedef struct Entry {
    Str key;
    Int hits;
} Entry;

static Entry entries[8];
static int used;

static Entry *table_get(Str k) {
    for (int i = 0; i < used; i++)
        if (str_eq(entries[i].key, k))
            return &entries[i];
    return NULL;
}

static void table_put(Str k, Entry *e) {
    Entry *old = table_get(k);
    if (old == NULL)
        old = &entries[used++];
    *old = *e;
}

// doc: rw
static SyncRWMutex mu;

Entry *lookup(Str k) {
    sync_rw_mutex_r_lock(&mu);
    Entry *e = table_get(k);
    sync_rw_mutex_r_unlock(&mu);
    return e;
}

void insert(Str k, Entry *e) {
    sync_rw_mutex_lock(&mu);
    table_put(k, e);
    sync_rw_mutex_unlock(&mu);
}
// doc: end

int main(void) {
    Entry e = {BURROW_S("home"), 3};
    insert(e.key, &e);
    Entry *got = lookup(BURROW_S("home"));
    printf("home has %lld hits, away is missing %d\n", (long long)got->hits,
           lookup(BURROW_S("away")) == NULL);
    return 0;
}

/* Output:
home has 3 hits, away is missing 1
*/
