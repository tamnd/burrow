/* Derived from Go's src/go/types/mono_test.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct {
    Alloc *a;
    BytesBuffer *buf;
} MonoEnv;

static void mono_error(void *env, const TypesError *err) {
    MonoEnv *e = env;
    Str s = types_error_error(err, e->a);
    (void)bytes_buffer_write_string(e->buf, s, NULL);
    (void)bytes_buffer_write_byte(e->buf, '\n');
}

/* checkMono: the errors from checking body, one to a line, or "" for none.
 *
 * Not in Go: Go returns an error, nil when there are none. */
static Str check_mono(Alloc *a, Str body) {
    Str src =
        fmt_sprintf_v(a, "package x; import `unsafe`; var _ unsafe.Pointer;\n%s", body);

    BytesBuffer buf = BYTES_BUFFER(a);
    MonoEnv env = {a, &buf};
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){mono_error, &env};
    /* TODO(adonovan): use same FileSet as typecheck */
    conf.importer = default_importer(NULL);
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, src, &conf, NULL, &err);
    Str s = bytes_buffer_string(&buf, a);
    return strings_trim_right(s, S("\n"));
}

static const Str goods[] = {
    S_("func F[T any](x T) { F(x) }"),
    S_("func F[T, U, V any]() { F[U, V, T](); F[V, T, U]() }"),
    S_("type Ring[A, B, C any] struct { L *Ring[B, C, A]; R *Ring[C, A, B] }"),
    S_("func F[T any]() { type U[T any] [unsafe.Sizeof(F[*T])]byte }"),
    S_("func F[T any]() { type U[T any] [unsafe.Sizeof(F[*T])]byte; var _ U[int] }"),
    S_("type U[T any] [unsafe.Sizeof(F[*T])]byte; func F[T any]() { var _ U[U[int]] }"),
    S_("func F[T any]() { type A = int; F[A]() }"),
};

/* TODO(mdempsky): Validate specific error messages and positioning. */

static const Str bads[] = {
    S_("func F[T any](x T) { F(&x) }"),
    S_("func F[T any]() { F[*T]() }"),
    S_("func F[T any]() { F[[]T]() }"),
    S_("func F[T any]() { F[[1]T]() }"),
    S_("func F[T any]() { F[chan T]() }"),
    S_("func F[T any]() { F[map[*T]int]() }"),
    S_("func F[T any]() { F[map[error]T]() }"),
    S_("func F[T any]() { F[func(T)]() }"),
    S_("func F[T any]() { F[func() T]() }"),
    S_("func F[T any]() { F[struct{ t T }]() }"),
    S_("func F[T any]() { F[interface{ t() T }]() }"),
    S_("type U[_ any] int; func F[T any]() { F[U[T]]() }"),
    S_("func F[T any]() { type U int; F[U]() }"),
    S_("func F[T any]() { type U int; F[*U]() }"),
    S_("type U[T any] int; func (U[T]) m() { var _ U[*T] }"),
    S_("type U[T any] int; func (*U[T]) m() { var _ U[*T] }"),
    S_("type U[T1 any] [unsafe.Sizeof(F[*T1])]byte; func F[T2 any]() { var _ U[T2] }"),
    S_("func F[A, B, C, D, E any]() { F[B, C, D, E, *A]() }"),
    S_("type U[_ any] int; const X = unsafe.Sizeof(func() { type A[T any] U[A[*T]] })"),
    S_("func F[T any]() { type A = *T; F[A]() }"),
    S_("type A[T any] struct { _ A[*T] }"),
    S_("type T struct{}; func (t T) M[P any]() { t.M[*P]() }"),
    S_("type T struct{}; func (t T) A[P any]() { t.B[*P]() }; func (t T) B[Q any]() { "
       "t.A[*Q]() }"),
    S_("type G[T any] int; func (g G[T]) M[P any]() { g.M[*P]() }"),
    S_("type G[T any] int; func (g G[T]) A[P any]() { g.B[*P]() }; func (g G[T]) B[Q "
       "any]() { g.A[*Q]() }"),
};

static void TestMonoGood(TestingT *t) {
    for (Int i = 0; i < NELEM(goods); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str err = check_mono(arena_allocator(&ar), goods[i]);
        if (err.len > 0)
            testing_t_errorf_v(t, "%d: unexpected failure: %v", i, err);
        arena_free(&ar);
    }
}

static void TestMonoBad(TestingT *t) {
    for (Int i = 0; i < NELEM(bads); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str err = check_mono(arena_allocator(&ar), bads[i]);
        if (err.len == 0)
            testing_t_errorf_v(t, "%d: unexpected success", i);
        else
            testing_t_log_v(t, err);
        arena_free(&ar);
    }
}

#define TESTS(X)                                                                       \
    X(TestMonoGood)                                                                    \
    X(TestMonoBad)

TESTING_MAIN(TESTS)
