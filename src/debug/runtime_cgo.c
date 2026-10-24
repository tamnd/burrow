/* runtime/cgo. See include/burrow/runtime/cgo.h.
 *
 * Derived from Go's src/runtime/cgo/handle.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/cgo.h"

#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

/* map[Handle]interface{}, as in Go. A SyncMap has to be told its types, and
 * that is a call, so the first use does it. */
static SyncMap cgo_handles;
static SyncOnce cgo_handles_once;
static SyncAtomicUintptr cgo_handle_idx;

static void cgo_handles_init(void *env) {
    (void)env;
    cgo_handles = SYNC_MAP(heap_allocator(), TYPE_UINTPTR, TYPE_ANY);
}

static SyncMap *cgo_handle_map(void) {
    sync_once_do(&cgo_handles_once, BURROW_FN(Func, cgo_handles_init, NULL));
    return &cgo_handles;
}

CgoHandle cgo_new_handle(Any v) {
    Uintptr h = sync_atomic_uintptr_add(&cgo_handle_idx, 1);
    if (h == 0)
        panic_str(BURROW_S("runtime/cgo: ran out of handle space"));

    if (!sync_map_store(cgo_handle_map(), &h, &v))
        panic_str(BURROW_S("runtime/cgo: out of memory"));
    return (CgoHandle)h;
}

Any cgo_handle_value(CgoHandle h) {
    Uintptr k = (Uintptr)h;
    Any v = {NULL, NULL};
    if (!sync_map_load(cgo_handle_map(), &k, &v))
        panic_str(BURROW_S("runtime/cgo: misuse of an invalid Handle"));
    return v;
}

void cgo_handle_delete(CgoHandle h) {
    Uintptr k = (Uintptr)h;
    if (!sync_map_load_and_delete(cgo_handle_map(), &k, NULL))
        panic_str(BURROW_S("runtime/cgo: misuse of an invalid Handle"));
}

static bool cgo_handle_count_one(const void *key, const void *val, void *arg) {
    (void)key;
    (void)val;
    *(Int *)arg += 1;
    return true;
}

/* How many handles are live, for TestHandle, which walks Go's map to check
 * the same thing. */
Int burrow__cgo_handle_count(void);
Int burrow__cgo_handle_count(void) {
    Int n = 0;
    sync_map_range(cgo_handle_map(), cgo_handle_count_one, &n);
    return n;
}
