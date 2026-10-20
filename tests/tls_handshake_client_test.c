/* Derived from Go's src/crypto/tls/handshake_client_test.go.
 * Go source: go1.27.1.
 *
 * Only TestLRUClientSessionCache for now. The rest of the file needs the
 * client handshake, which comes in later.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"

static TlsClientSessionState *cache_get(TlsClientSessionCache cache, Str key,
                                        bool *ok) {
    return cache.vt->get(cache.data, key, ok);
}

static void cache_put(TlsClientSessionCache cache, Str key, TlsClientSessionState *cs) {
    cache.vt->put(cache.data, key, cs);
}

static void TestLRUClientSessionCache(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Initialize cache of capacity 4. */
    TlsClientSessionCache cache = tls_new_lru_client_session_cache(a, 4);
    if (cache.vt == NULL)
        testing_t_fatal_v(t, "NewLRUClientSessionCache: out of memory");
    TlsClientSessionState cs[6] = {{0}};
    const Str keys[] = {BURROW_S("0"), BURROW_S("1"), BURROW_S("2"), BURROW_S("3"),
                        BURROW_S("4"), BURROW_S("5"), BURROW_S("6")};
    TlsClientSessionState *s;
    bool ok;

    /* Add 4 entries to the cache and look them up. */
    for (int i = 0; i < 4; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 4; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache failed lookup for added key: %s",
                               keys[i]);
    }

    /* Add 2 more entries to the cache. First 2 should be evicted. */
    for (int i = 4; i < 6; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 2; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (ok || s != NULL)
            testing_t_fatalf_v(t, "session cache should have evicted key: %s", keys[i]);
    }

    /* Touch entry 2. LRU should evict 3 next. */
    cache_get(cache, keys[2], &ok);
    cache_put(cache, keys[0], &cs[0]);
    s = cache_get(cache, keys[3], &ok);
    if (ok || s != NULL)
        testing_t_fatalf_v(t, "session cache should have evicted key 3");

    /* Update entry 0 in place. */
    cache_put(cache, keys[0], &cs[3]);
    s = cache_get(cache, keys[0], &ok);
    if (!ok || s != &cs[3])
        testing_t_fatalf_v(t, "session cache failed update for key 0");

    /* Calling Put with a nil entry deletes the key. */
    cache_put(cache, keys[0], NULL);
    cache_get(cache, keys[0], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 0");

    /* Delete entry 2. LRU should keep 4 and 5 */
    cache_put(cache, keys[2], NULL);
    cache_get(cache, keys[2], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 4");
    for (int i = 4; i < 6; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache should not have deleted key: %s",
                               keys[i]);
    }
    arena_free(&ar);
}

#define TESTS(X) X(TestLRUClientSessionCache)

TESTING_MAIN(TESTS)
