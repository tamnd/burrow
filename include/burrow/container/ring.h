/* container/ring, circular lists.
 *
 * Go's container/ring. A ring has no beginning and no end, and a pointer to
 * any element of it stands for the whole ring. An empty ring is a NULL
 * pointer, and a zeroed Ring is a ring of one element:
 *
 *     Ring *r = ring_new(a, 3);
 *     for (Int i = 1; i <= 3; i++) {
 *         r->value = any_box(a, BURROW_ANY_OF(i));
 *         r = ring_next(r);
 *     }
 *
 * value is Go's Value field. The ring never reads or copies it, so whatever it
 * points at has to live as long as the element does. any_box is the way to
 * give it a copy of its own.
 *
 * Go has a collector and C does not, so ring_free is extra: it gives back
 * every element of a ring that came from ring_new. With an arena it is not
 * needed.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package container/ring */

#ifndef BURROW_CONTAINER_RING_H
#define BURROW_CONTAINER_RING_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ring Ring;

/* ring.Ring, one element of a ring. */
struct Ring {
    /* The implementation's. Both NULL in a Ring nobody has linked yet, which
     * the functions below treat as a ring of one. */
    Ring *next;
    Ring *prev;

    Any value;
};

/* What ring_do calls with each value, which is Go's func(any). */
BURROW_FUNC(RingDoFunc, void, Any v);

/* A ring of n elements with nil values, allocated from a. NULL when n is zero
 * or less, which is Go's answer, and when a is out of memory. */
BURROW_OWNS(ret) Ring *ring_new(Alloc *a, Int n);

/* The next and the previous element. r must not be NULL. */
BURROW_BORROWS(ret, r) Ring *ring_next(Ring *r);
BURROW_BORROWS(ret, r) Ring *ring_prev(Ring *r);

/* The element n steps forward, or back when n is negative. r must not be
 * NULL. */
BURROW_BORROWS(ret, r) Ring *ring_move(Ring *r, Int n);

/* Links r with s so that s comes right after r, and returns what used to come
 * after r. If r and s are in the same ring this cuts the elements between them
 * out into a ring of their own, which is what it returns. If they are in
 * different rings it splices the two together, with s's ring inserted after
 * r. r must not be NULL. */
BURROW_BORROWS(ret, r) Ring *ring_link(Ring *r, Ring *s);

/* Takes n % ring_len(r) elements out, starting just after r, and returns them
 * as a ring of their own. NULL when n is zero or less. */
BURROW_BORROWS(ret, r) Ring *ring_unlink(Ring *r, Int n);

/* The number of elements, found by walking the ring. Zero for NULL. */
Int ring_len(const Ring *r);

/* Calls f with each value, going forward from r. f must not change the ring. */
void ring_do(Ring *r, RingDoFunc f);

/* Gives every element of the ring r back to a, which must be the allocator
 * all of them came from. Nothing in the ring can be used afterwards. */
void ring_free(Alloc *a, Ring *r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CONTAINER_RING_H */
