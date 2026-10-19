/* Derived from Go's src/go/types/trie_test.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Not in Go: Go's trie is generic in its values. burrow's holds void *, and
 * the strings here are Str * values. */

#include "check.h"

#include "../src/go/types_check.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"

#include <stdarg.h>

#define S(lit) BURROW_S(lit)

typedef struct {
    TestingT *t;
    Alloc *a;
    Slice strie;
} TrieEnv;

/* Not in Go: fmt.Sprintf("%x", index). */
static Str key_string(Alloc *a, Slice key) {
    BytesBuffer b = BYTES_BUFFER(a);
    (void)bytes_buffer_write_byte(&b, '[');
    for (Int i = 0; i < key.len; i++) {
        if (i > 0)
            (void)bytes_buffer_write_byte(&b, ' ');
        bytes_buffer_write_string(&b, fmt_sprintf_v(a, "%x", BURROW_AT(Int, key, i)),
                                  NULL);
    }
    (void)bytes_buffer_write_byte(&b, ']');
    return bytes_buffer_string(&b, a);
}

/* Not in Go: index... as a Slice of Int, ended by -1. */
static Slice index_slice(Alloc *a, va_list ap) {
    Slice key = slice_nil(TYPE_INT);
    for (;;) {
        Int x = va_arg(ap, int);
        if (x < 0)
            break;
        key = slice_append(a, key, &x, 1);
    }
    return key;
}

static void okay_(TrieEnv *env, ...) {
    va_list ap;
    va_start(ap, env);
    Slice index = index_slice(env->a, ap);
    va_end(ap);
    Str *s = BURROW_NEW(env->a, Str);
    *s = key_string(env->a, index);
    Int n;
    Str *p = burrow__tc_trie_insert(env->a, &env->strie, index, s, &n);
    if (n != 0)
        testing_t_errorf_v(env->t, "%s collided with %s (n = %d)", *s, *p, n);
}

static void fail_(TrieEnv *env, const char *collision, ...) {
    va_list ap;
    va_start(ap, collision);
    Slice index = index_slice(env->a, ap);
    va_end(ap);
    Str *s = BURROW_NEW(env->a, Str);
    *s = key_string(env->a, index);
    Int n;
    Str *p = burrow__tc_trie_insert(env->a, &env->strie, index, s, &n);
    if (n == 0)
        testing_t_errorf_v(env->t, "%s did not collide", *s);
    else if (!str_eq(*p, str_from_cstr(collision)))
        testing_t_errorf_v(env->t, "%s collided with %s (n == %d), want %s", *s, *p, n,
                           str_from_cstr(collision));
}

#define okay(...) okay_(&env, __VA_ARGS__, -1)
#define fail(...) fail_(&env, __VA_ARGS__, -1)
#define clear(tr) ((tr) = slice_nil(TYPE_UNSAFE_POINTER))

static void TestTrie(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    TrieEnv env = {t, arena_allocator(&ar), slice_nil(TYPE_UNSAFE_POINTER)};

    clear(env.strie);
    okay(0);
    fail("[0]", 0);

    clear(env.strie);
    okay(0);
    fail("[0]", 0, 1, 2, 3, 4, 5);

    clear(env.strie);
    okay(1, 2);
    okay(1, 3);
    okay(1, 4, 5);
    okay(1, 4, 2);
    fail("[1 4 2]", 1, 4);
    fail("[1 4 5]", 1, 4, 5);
    okay(1, 4, 3);
    okay(2, 1);
    okay(2, 2);
    fail("[2 2]", 2, 2, 3);

    clear(env.strie);
    okay(0, 1, 2, 3, 4, 5);
    okay(0, 1, 2, 3, 4, 6);
    okay(0, 1, 2, 3, 4, 7);
    okay(0, 1, 2, 3, 4, 8, 1);
    okay(0, 1, 2, 3, 4, 4);
    fail("[0 1 2 3 4 4]", 0, 1, 2, 3);
    arena_free(&ar);
}

static void TestAnyValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice atrie = slice_nil(TYPE_UNSAFE_POINTER); /* allow values of any type */

    int *val = BURROW_NEW(a, int);
    *val = 42;
    Int key0 = 0;
    Slice key = slice_append(a, slice_nil(TYPE_INT), &key0, 1);
    Int n;
    void *alt = burrow__tc_trie_insert(a, &atrie, key, val, &n);
    if (n != 0)
        testing_t_errorf_v(t, "unexpected collision (n = %d)", n);
    if (alt != val)
        testing_t_errorf_v(t, "unexpected result (alt = %p, val = %p)", alt,
                           (void *)val);

    alt = burrow__tc_trie_insert(a, &atrie, key, val, &n); /* nil is a valid value */
    if (n == 0)
        testing_t_errorf_v(t, "expected collision");
    if (alt != val)
        testing_t_errorf_v(t, "unexpected result (alt = %p, val = %p)", alt,
                           (void *)val);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestTrie)                                                                        \
    X(TestAnyValue)

TESTING_MAIN(TESTS)
