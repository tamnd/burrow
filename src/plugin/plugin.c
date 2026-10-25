/* plugin. See include/burrow/plugin.h.
 *
 * Derived from Go's src/plugin/plugin_dlopen.go.
 * Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/plugin.h"

#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/sync.h"

#include <stddef.h>
#include <string.h>

/* Room for any path realpath can give, and for any path Windows can once it is
 * UTF-8, which is three bytes a unit at most. */
#define PLG_PATH_CAP (PAL_WPATH_MAX * 3 + 1)

/* Go's Plugin has the package path, the error from a failed load, the channel
 * that is closed once it has loaded, and the symbols. Here a Plugin is only
 * put where plugin_open can find it once the library is open, so there is
 * nothing to wait for and no failure to keep, and the symbols are the
 * loader's to find. The path is the file realpath found, with a NUL after
 * it. */
struct Plugin {
    int64_t handle;
    Plugin *next;
    Int len;
    char path[];
};

/* Go's pluginsMu and plugins. A list rather than a map, since it only grows
 * and a program opens a handful of plugins. */
static SyncMutex plg_mu;
static Plugin *plg_list;

/* The plugin for the file path, or NULL. Call it with plg_mu held. */
static Plugin *plg_find(const char *path, size_t n) {
    for (Plugin *p = plg_list; p != NULL; p = p->next)
        if ((size_t)p->len == n && memcmp(p->path, path, n) == 0)
            return p;
    return NULL;
}

/* The pieces of a message joined into an error from the error arena. */
static Error plg_error(const Str *parts, int n) {
    Alloc *a = error_allocator();
    Int len = 0;
    for (int i = 0; i < n; i++)
        len += parts[i].len;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
    if (buf == NULL)
        return burrow_err_out_of_memory;
    Int at = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i].len > 0)
            memcpy(buf + at, parts[i].p, (size_t)parts[i].len);
        at += parts[i].len;
    }
    Str text = {buf, len};
    Error e = errors_new(a, text);
    mem_free(a, buf, (size_t)len, 1);
    return e;
}

Plugin *plugin_open(Str path, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *heap = heap_allocator();
    char *rel = str_to_cstr(heap, path);
    if (rel == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    char full[PLG_PATH_CAP];
    bool found = pal_dl_realpath(rel, full, sizeof full, NULL);
    mem_free(heap, rel, (size_t)path.len + 1, 1);
    if (!found) {
        Str parts[3] = {BURROW_S("plugin.Open(\""), path,
                        BURROW_S("\"): realpath failed")};
        BURROW_OUT(err, plg_error(parts, 3));
        return NULL;
    }
    size_t n = strlen(full);

    sync_mutex_lock(&plg_mu);
    Plugin *p = plg_find(full, n);
    sync_mutex_unlock(&plg_mu);
    if (p != NULL)
        return p;

    /* Go holds pluginsMu while it calls dlopen, and lets go of it before the
     * plugin's init functions run, because one of them may open a plugin too.
     * Here those are the library's constructors, and dlopen runs them, so the
     * lock is let go across the whole call. Two goroutines that open the same
     * file at once both load it, the loader counts that as two opens of one
     * library, and whichever gets back to the list second takes the Plugin the
     * first one put there. */
    char msg[512];
    int64_t h = pal_dl_open(full, msg, sizeof msg);
    if (h == 0) {
        Str parts[4] = {BURROW_S("plugin.Open(\""), path, BURROW_S("\"): "),
                        str_from_cstr(msg)};
        BURROW_OUT(err, plg_error(parts, 4));
        return NULL;
    }

    size_t size = offsetof(Plugin, path) + n + 1;
    Plugin *np = (Plugin *)mem_alloc(heap, size, _Alignof(Plugin));
    if (np == NULL) {
        (void)pal_dl_close(h, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    np->handle = h;
    np->len = (Int)n;
    memcpy(np->path, full, n + 1);

    sync_mutex_lock(&plg_mu);
    p = plg_find(full, n);
    if (p == NULL) {
        np->next = plg_list;
        plg_list = np;
        p = np;
        np = NULL;
    }
    sync_mutex_unlock(&plg_mu);
    if (np != NULL)
        mem_free(heap, np, size, _Alignof(Plugin));
    return p;
}

PluginSymbol plugin_lookup(Plugin *p, Str sym_name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *heap = heap_allocator();
    char *name = str_to_cstr(heap, sym_name);
    if (name == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    PalErrno e = PAL_OK;
    void *r = pal_dl_sym(p->handle, name, &e);
    mem_free(heap, name, (size_t)sym_name.len + 1, 1);
    if (e != PAL_OK) {
        Str path = {(const Byte *)p->path, p->len};
        Str parts[4] = {BURROW_S("plugin: symbol "), sym_name,
                        BURROW_S(" not found in plugin "), path};
        BURROW_OUT(err, plg_error(parts, 4));
        return NULL;
    }
    return r;
}
