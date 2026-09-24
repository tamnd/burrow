/* iter: sequences you range over.
 *
 * Go 1.23 added range over func, and with it a convention: a function that
 * produces a sequence takes a yield callback, calls it once per value, and stops
 * as soon as yield returns false. That is an IterSeq here. strings_lines,
 * strings_split_seq and the rest of the functions ending in Seq return one, and
 * so will maps and slices.
 *
 * There are two ways to consume one. Call it with a yield of your own, which
 * costs nothing beyond the calls:
 *
 *     static bool print_line(void *env, const void *v) {
 *         const Str *line = v;
 *         printf(BURROW_STR_FMT, BURROW_STR_ARG(*line));
 *         return true;
 *     }
 *
 *     BURROW_CALLF(strings_lines(a, text), BURROW_FN(IterYield, print_line, NULL));
 *
 * Or write a loop, which reads like Go and runs the sequence on a coroutine:
 *
 *     BURROW_RANGE(Str, line, strings_lines(a, text)) {
 *         printf(BURROW_STR_FMT, BURROW_STR_ARG(line));
 *     }
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* ------------------------------------------------------------ the values
 *
 * Go's Seq is generic and C is not, so a yield gets a pointer to the value
 * rather than the value, and which type it points at is part of the documented
 * contract of whoever made the sequence, the same way the element type of a
 * Slice is. The pointer is good until the yield returns. A caller that wants to
 * keep the value copies it, which is what Go does when it passes a value.
 *
 * ------------------------------------------------------------ Pull
 *
 * iter_pull turns a sequence you push through into one you pull from, the same
 * as Go's Pull. The sequence runs on a coroutine, a goroutine that the caller
 * switches to and from directly with no scheduler in between, so each value
 * costs two switches. The caller has to be on a goroutine, somewhere under
 * runtime_main or go, and so does a BURROW_RANGE loop. A panic in the sequence
 * comes out of the iter_pull_next or iter_pull_stop that was running it, the
 * same as in Go.
 *
 * An IterPull has to stay where it is from iter_pull until the sequence is
 * over, because the coroutine keeps a pointer to it, and iter_pull_stop has to
 * be called unless iter_pull_next has already said there is nothing left. A
 * pull that is never stopped is a goroutine that never exits, which is the same
 * leak it is in Go.
 */

/* burrow:package iter */

#ifndef BURROW_ITER_H
#define BURROW_ITER_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/* func(V) bool, the yield a sequence calls. v points at the value and is good
 * until the call returns. Returning false asks the sequence to stop. */
BURROW_FUNC(IterYield, bool, const void *v);

/* iter.Seq[V], a sequence of single values. */
BURROW_FUNC(IterSeq, void, IterYield yield);

/* func(K, V) bool, the yield of a sequence of pairs. */
BURROW_FUNC(IterYield2, bool, const void *k, const void *v);

/* iter.Seq2[K, V], a sequence of pairs. */
BURROW_FUNC(IterSeq2, void, IterYield2 yield);

/* The state of iter.Pull. The fields are the runtime's. */
typedef struct IterPull {
    burrow__Coro coro;
    IterSeq seq;
    const void *v;
    bool ok;
    bool done;
    bool yield_next;
    bool seq_done;
    bool goexit;
    Any panic_value;
    burrow__PanicValue storage;
} IterPull;

/* The state of iter.Pull2. */
typedef struct IterPull2 {
    burrow__Coro coro;
    IterSeq2 seq;
    const void *k;
    const void *v;
    bool ok;
    bool done;
    bool yield_next;
    bool seq_done;
    bool goexit;
    Any panic_value;
    burrow__PanicValue storage;
} IterPull2;

/* iter.Pull. Starts pulling from seq through *p, and answers false only if
 * there was no memory for the coroutine, in which case there is nothing to
 * stop. The sequence does not start running until the first iter_pull_next. */
bool iter_pull(IterPull *p, IterSeq seq);

/* The next value, Go's next(). Answers false once the sequence has finished or
 * been stopped, and from then on every time. On true, *v points at the value
 * and stays good until the next call on p. */
bool iter_pull_next(IterPull *p, const void **v);

/* Go's stop(). Ends the sequence if it is still running: the yield it is
 * waiting in returns false. Calling it again, or after the end, does nothing. */
void iter_pull_stop(IterPull *p);

/* iter.Pull2, the same over pairs. */
bool iter_pull2(IterPull2 *p, IterSeq2 seq);
bool iter_pull2_next(IterPull2 *p, const void **k, const void **v);
void iter_pull2_stop(IterPull2 *p);

/* ------------------------------------------------------------ BURROW_RANGE
 *
 * Go's for v := range seq, for a sequence of T:
 *
 *     BURROW_RANGE(Str, field, strings_fields_seq(s)) {
 *         if (str_eq(field, BURROW_S("stop")))
 *             break;
 *         ...
 *     }
 *
 * v is a copy of the value, so it is yours to keep. break and continue do what
 * they do in Go. Leaving the loop any other way, with return or goto, skips
 * the stop and leaks the coroutine, so put what needs to leave early in a
 * flag, break, and act on it after the loop. BURROW_RANGE2 is the same over an
 * IterSeq2, with a key and a value.
 *
 * Nested loops work, and each gets its own coroutine. */

typedef struct burrow__IterRange {
    IterPull p;
    IterSeq seq;
    bool started;
    bool live;
} burrow__IterRange;

typedef struct burrow__IterRange2 {
    IterPull2 p;
    IterSeq2 seq;
    bool started;
    bool live;
} burrow__IterRange2;

bool burrow__iter_range_next(burrow__IterRange *r, void *out, size_t size);
void burrow__iter_range_end(burrow__IterRange *r);
bool burrow__iter_range2_next(burrow__IterRange2 *r, void *kout, size_t ksize,
                              void *vout, size_t vsize);
void burrow__iter_range2_end(burrow__IterRange2 *r);

/* The loop state is named after the line it is on, so that a loop nested in
 * another one does not shadow it. */
#define BURROW__ITER_CAT2(a, b) a##b
#define BURROW__ITER_CAT(a, b) BURROW__ITER_CAT2(a, b)
#define BURROW__ITER_R BURROW__ITER_CAT(burrow__range_, __LINE__)
#define BURROW__ITER_K BURROW__ITER_CAT(burrow__rangek_, __LINE__)

/* T is a type and cannot be parenthesised. */
/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define BURROW_RANGE(T, v, s)                                                          \
    for (burrow__IterRange BURROW__ITER_R = {.seq = (s), .live = true};                \
         BURROW__ITER_R.live; burrow__iter_range_end(&BURROW__ITER_R))                 \
        for (T v; burrow__iter_range_next(&BURROW__ITER_R, &v, sizeof(T));)

#define BURROW_RANGE2(K, k, V, v, s)                                                   \
    for (burrow__IterRange2 BURROW__ITER_R = {.seq = (s), .live = true};               \
         BURROW__ITER_R.live; burrow__iter_range2_end(&BURROW__ITER_R))                \
        for (K k, *BURROW__ITER_K = &k; BURROW__ITER_K != NULL; BURROW__ITER_K = NULL) \
            for (V v; burrow__iter_range2_next(&BURROW__ITER_R, &k, sizeof(K), &v,     \
                                               sizeof(V));)
/* NOLINTEND(bugprone-macro-parentheses) */

#if defined(BURROW_SHORT) && BURROW_SHORT
#define RANGE(T, v, s) BURROW_RANGE(T, v, s)
#define RANGE2(K, k, V, v, s) BURROW_RANGE2(K, k, V, v, s)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ITER_H */
