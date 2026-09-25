/* container/ring.
 *
 * Derived from Go's src/container/ring/ring.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/container/ring.h"

#include <stdalign.h>
#include <stddef.h>

static Ring *ring_init(Ring *r) {
    r->next = r;
    r->prev = r;
    return r;
}

Ring *ring_next(Ring *r) {
    if (r->next == NULL)
        return ring_init(r);
    return r->next;
}

Ring *ring_prev(Ring *r) {
    if (r->next == NULL)
        return ring_init(r);
    return r->prev;
}

Ring *ring_move(Ring *r, Int n) {
    if (r->next == NULL)
        return ring_init(r);
    if (n < 0) {
        for (; n < 0; n++)
            r = r->prev;
    } else {
        for (; n > 0; n--)
            r = r->next;
    }
    return r;
}

Ring *ring_new(Alloc *a, Int n) {
    if (n <= 0)
        return NULL;
    Ring *r = mem_alloc(a, sizeof *r, alignof(Ring));
    if (r == NULL)
        return NULL;
    Ring *p = r;
    for (Int i = 1; i < n; i++) {
        Ring *q = mem_alloc(a, sizeof *q, alignof(Ring));
        if (q == NULL) {
            /* Close what there is into a ring so ring_free can walk it. */
            p->next = r;
            r->prev = p;
            ring_free(a, r);
            return NULL;
        }
        q->prev = p;
        p->next = q;
        p = q;
    }
    p->next = r;
    r->prev = p;
    return r;
}

/* ring_link with s known to be a ring. ring_unlink goes through here rather
 * than through ring_link, because gcc's -Wnull-dereference cannot see that
 * ring_move never returns NULL and warns about the NULL case ring_link has to
 * handle. */
static Ring *ring_splice(Ring *r, Ring *s) {
    Ring *n = ring_next(r);
    Ring *p = ring_prev(s);
    /* Note: Cannot use multiple assignment because evaluation order of LHS is
     * not specified. */
    r->next = s;
    s->prev = r;
    n->prev = p;
    p->next = n;
    return n;
}

Ring *ring_link(Ring *r, Ring *s) {
    if (s == NULL)
        return ring_next(r);
    return ring_splice(r, s);
}

Ring *ring_unlink(Ring *r, Int n) {
    if (n <= 0)
        return NULL;
    return ring_splice(r, ring_move(r, n + 1));
}

Int ring_len(const Ring *r) {
    Int n = 0;
    if (r != NULL) {
        n = 1;
        for (const Ring *p = r->next; p != NULL && p != r; p = p->next)
            n++;
    }
    return n;
}

void ring_do(Ring *r, RingDoFunc f) {
    if (r != NULL) {
        f.f(f.env, r->value);
        for (Ring *p = ring_next(r); p != r; p = p->next)
            f.f(f.env, p->value);
    }
}

void ring_free(Alloc *a, Ring *r) {
    if (r == NULL)
        return;
    Ring *p = r->next;
    while (p != NULL && p != r) {
        Ring *next = p->next;
        mem_free(a, p, sizeof *p, alignof(Ring));
        p = next;
    }
    mem_free(a, r, sizeof *r, alignof(Ring));
}
