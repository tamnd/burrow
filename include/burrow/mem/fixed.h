/* An allocator over a buffer you already have.
 *
 * Point it at an array on the stack, or at a block of static storage, or at a
 * region a bootloader handed you, and it hands pieces of that out. It never
 * calls the parent, because it has no parent. When the buffer is full,
 * allocation returns NULL and the caller deals with it.
 *
 *     unsigned char buf[4096];
 *     Fixed fx;
 *     fixed_init(&fx, buf, sizeof(buf));
 *     Alloc *a = fixed_allocator(&fx);
 *
 * There are two reasons to want this. One is a target with no allocator at all,
 * where a firmware image gets a region at link time and that is the whole story.
 * The other is a test that wants to prove a function stays inside a budget,
 * which it does by giving it exactly that many bytes and watching it either fit
 * or fail.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MEM_FIXED_H
#define BURROW_MEM_FIXED_H

#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fields are visible so the struct can live on the stack. Do not touch them. */
typedef struct Fixed {
    Alloc alloc;
    unsigned char *base;
    size_t cap;
    size_t used;
    uint64_t bytes_peak;
    uint64_t bytes_total;
    uint64_t allocs;
    uint64_t frees;
} Fixed;

/* The buffer stays yours. Nothing is copied and nothing is freed, and the
 * buffer has to outlive every allocation made from it. Contents are not
 * examined, so a buffer with old data in it is fine. */
void fixed_init(Fixed *fx, void *buf, size_t size);

Alloc *fixed_allocator(Fixed *fx);

/* Everything allocated becomes invalid and the whole buffer is available again.
 * Reachable through mem_reset too. */
void fixed_reset(Fixed *fx);

/* How many bytes are left, which is the question people actually ask of this
 * one. It is a bound rather than a promise, since alignment can eat a few. */
size_t fixed_available(const Fixed *fx);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MEM_FIXED_H */
