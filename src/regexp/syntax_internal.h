/* regexp/syntax: what the parser, the printer and the compiler share.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_REGEXP_SYNTAX_INTERNAL_H
#define BURROW_REGEXP_SYNTAX_INTERNAL_H

#include "burrow/regexp/syntax.h"

#include "burrow/mem/arena.h"

#include <stdint.h>

/* The smallest and largest runes that case fold to anything but themselves. */
#define SYN_MIN_FOLD ((Rune)0x0041)
#define SYN_MAX_FOLD ((Rune)0x1e943)

/* Where the parser's own operators start: the left paren and the vertical
 * bar, which only ever sit on its stack. */
#define SYN_OP_PSEUDO 128
#define SYN_OP_LEFT_PAREN 128
#define SYN_OP_VERTICAL_BAR 129

#define SYN_RUNES(s) ((Rune *)(s).p)
#define SYN_SUBS(s) ((SyntaxRegexp **)(s).p)

/* A map from a node to a number, which is what Go's map[*Regexp]int is used
 * for in the parser and the printer. Open addressing, with deletion. The
 * memory comes from a scratch arena, so nothing is ever given back one table
 * at a time. */
typedef struct SynMap {
    Alloc *a;
    const void **keys;
    int64_t *vals;
    Int cap;  /* a power of two, or 0 */
    Int used; /* live keys and tombstones */
} SynMap;

/* Whether k is in m, and its value in *v when it is. */
bool syn_map_get(const SynMap *m, const void *k, int64_t *v);

/* Sets k to v. False when the allocator refuses. */
bool syn_map_put(SynMap *m, const void *k, int64_t v);

void syn_map_delete(SynMap *m, const void *k);

/* A copy of the tree at root in one block from a, with shared nodes still
 * shared, which syntax_regexp_free takes back. NULL when a refuses. scratch is
 * for the copy's own bookkeeping. */
SyntaxRegexp *syn_copy_tree(Alloc *a, Alloc *scratch, const SyntaxRegexp *root);

/* The head of a block from syn_copy_tree or the compiler: who to give it back
 * to and how big it is. What the block holds starts SYN_BLOCK_HDR bytes in. */
typedef struct SynBlock {
    Alloc *a;
    size_t size;
} SynBlock;

#define SYN_BLOCK_ALIGN _Alignof(SyntaxRegexp)
#define SYN_BLOCK_HDR                                                                  \
    ((sizeof(SynBlock) + SYN_BLOCK_ALIGN - 1) / SYN_BLOCK_ALIGN * SYN_BLOCK_ALIGN)

/* Working memory for the printer, Simplify and the compiler, which all stop
 * the same way when it runs out: syn_scratch_alloc panics, and syn_guard
 * catches that and says false. Any other panic frees the scratch memory and
 * goes on up. */
typedef struct SynScratch {
    Arena arena;
    Alloc *a;
    bool oom;
} SynScratch;

void syn_scratch_init(SynScratch *s, Alloc *parent);
void syn_scratch_free(SynScratch *s);
void *syn_scratch_alloc(SynScratch *s, size_t size, size_t align);
bool syn_guard(SynScratch *s, void (*fn)(void *), void *arg);

/* A stack of frames for walking a tree without recursion, in scratch memory.
 * The tree can be 1000 deep, which as C recursion would take most of a
 * goroutine's stack, so the printer, Simplify and the compiler keep their own.
 * elem is the frame size. */
typedef struct SynStack {
    Byte *p;
    Int len;
    Int cap;
    size_t elem;
} SynStack;

/* A zeroed frame on top of k, which may move the ones below it. */
void *syn_stack_push(SynScratch *s, SynStack *k);

/* The frame on top of k, or NULL when k is empty. */
static inline void *syn_stack_top(SynStack *k) {
    return k->len == 0 ? NULL : k->p + (size_t)(k->len - 1) * k->elem;
}

/* inCharClass: whether r is in the sorted class c of len runes. */
bool syn_in_char_class(Rune r, const Rune *c, Int len);

#endif /* BURROW_REGEXP_SYNTAX_INTERNAL_H */
