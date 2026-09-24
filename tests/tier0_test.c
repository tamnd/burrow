/* The program P0 has to be able to run, from issue #1: start a goroutine, send
 * on a channel while another goroutine selects on it, run a deferred function
 * while a panic unwinds past it, and print a struct with %v. It is one test
 * rather than four because the point is that the pieces work together, and CI
 * runs it with the rest under ThreadSanitizer.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

#define RESULT_FIELDS(F, T)                                                            \
    F(T, Int, Sum, "")                                                                 \
    F(T, bool, Deferred, "")                                                           \
    F(T, Str, Panic, "")
BURROW_STRUCT(Result, RESULT_FIELDS);

typedef struct Pipe {
    Chan *values;
    Chan *quit;
    Chan *done;
    Result result;
} Pipe;

static void send_values(void *env) {
    Pipe *p = env;
    for (Int v = 1; v <= 3; v++)
        chan_send(p->values, &v);
    bool stop = true;
    chan_send(p->quit, &stop);
}

static void mark_deferred(void *env) {
    Result *r = env;
    r->Deferred = true;
}

static void give_up(Result *r) {
    BURROW_SCOPE {
        BURROW_DEFER(mark_deferred, r);
        panic_str(BURROW_S("quit"));
    }
    BURROW_SCOPE_END;
}

static void select_values(void *env) {
    Pipe *p = env;
    for (;;) {
        Int v;
        bool stop;
        SelectCase cases[] = {
            BURROW_RECV(p->values, &v),
            BURROW_RECV(p->quit, &stop),
        };
        if (chan_select(cases, 2) == 0) {
            p->result.Sum += v;
            continue;
        }
        BURROW_TRY {
            give_up(&p->result);
        }
        BURROW_CATCH(e) {
            p->result.Panic = panic_text(e);
        }
        BURROW_TRY_END;
        break;
    }
    bool ok = true;
    chan_send(p->done, &ok);
}

static void TestTier0(TestingT *t) {
    Alloc *h = heap_allocator();
    Pipe p = {
        .values = chan_make(h, TYPE_INT, 0),
        .quit = chan_make(h, TYPE_BOOL, 0),
        .done = chan_make(h, TYPE_BOOL, 0),
    };

    go(BURROW_FN(Func, select_values, &p));
    go(BURROW_FN(Func, send_values, &p));
    bool ok = false;
    chan_recv(p.done, &ok);
    CHECK(ok);

    Arena ar;
    arena_init(&ar, h, 0);
    Str got = fmt_sprintf_v(arena_allocator(&ar), "%v",
                            BURROW_ANY(TYPE_OF(Result), &p.result));
    if (!str_eq(got, BURROW_S("{6 true quit}")))
        testing_t_errorf_v(t, "%%v printed %s, want {6 true quit}", got);
    arena_free(&ar);

    chan_free(p.values);
    chan_free(p.quit);
    chan_free(p.done);
}

#define TESTS(X) X(TestTier0)

TESTING_MAIN(TESTS)
