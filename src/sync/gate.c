/* Derived from Go's src/internal/gate/gate.go.
 * Go source: go1.27.3.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "gate.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>

/* Go's chan struct{}. Only the presence of a value matters. */
static const bool gate_token = true;

bool burrow__gate_init(Gate *g, Alloc *a, bool set) {
    g->set = chan_make(a, TYPE_BOOL, 1);
    g->unset = chan_make(a, TYPE_BOOL, 1);
    if (g->set == NULL || g->unset == NULL) {
        burrow__gate_destroy(g);
        return false;
    }
    burrow__gate_unlock(g, set);
    return true;
}

void burrow__gate_destroy(Gate *g) {
    chan_free(g->set);
    chan_free(g->unset);
    g->set = NULL;
    g->unset = NULL;
}

bool burrow__gate_lock(Gate *g) {
    SelectCase cases[2] = {
        BURROW_RECV(g->set, NULL),
        BURROW_RECV(g->unset, NULL),
    };
    return chan_select(cases, 2) == 0;
}

Error burrow__gate_wait_and_lock(Gate *g, Context ctx) {
    if (chan_try_recv(g->set, NULL, NULL))
        return BURROW_NO_ERROR;
    SelectCase cases[2] = {
        BURROW_RECV(g->set, NULL),
        BURROW_RECV(context_done(ctx), NULL),
    };
    if (chan_select(cases, 2) == 0)
        return BURROW_NO_ERROR;
    return context_err(ctx);
}

bool burrow__gate_lock_if_set(Gate *g) {
    return chan_try_recv(g->set, NULL, NULL);
}

void burrow__gate_unlock(Gate *g, bool set) {
    if (set)
        chan_send(g->set, &gate_token);
    else
        chan_send(g->unset, &gate_token);
}
