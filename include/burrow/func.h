/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Function values: a function pointer and the environment it was made with.
 *
 * Go's func is a closure. It is code plus the variables that code captured, and
 * Go hides the second part so thoroughly that most people who write one never
 * think about it. C has the first part and nothing else, so every function type
 * in a Go signature becomes a named pair here.
 *
 *     typedef struct ReadFn {
 *         Int (*f)(void *env, Slice p, Error *err);
 *         void *env;
 *     } ReadFn;
 *
 * The env word is not optional and it is the whole point. A library that takes
 * a bare function pointer forces every caller who needs state to reach for a
 * global, and then two callers cannot use it at once. qsort is that mistake and
 * every platform has since grown a qsort_r to undo it. Pay the word.
 *
 * Three rules, and they are the same three that interfaces have, because a
 * function value is an interface with one unnamed method.
 *
 * The function pointer is first, so a zeroed value is a nil func. A function
 * value inside a struct that came out of an allocator starts out nil with
 * nobody writing a line to say so, and BURROW_FUNC_IS_NIL asks. Calling a nil
 * func is a nil dereference here and a panic in Go, and neither language checks
 * for you.
 *
 * The env goes in first at the call, the same way the receiver does for a
 * method. The target always takes it, even when it has nothing to remember, in
 * which case it writes (void)env and moves on. This is not negotiable either:
 * calling a function through a pointer of a different type is undefined
 * behaviour, and on the targets that check, wasm and anything built with
 * control flow integrity, it is a trap rather than a theoretical problem. Write
 * the parameter and ignore it. Do not cast the pointer.
 *
 * Whatever env points at has to outlive the function value. Go moves a captured
 * variable to the heap when it sees a closure outlive the frame it was made in,
 * and it does that silently. Nothing here can see that, so the lifetime is the
 * caller's to get right. A value passed to something that finishes before it
 * returns, a sort comparator or a filter, can live on the stack. One stored in
 * a struct, handed to a goroutine or registered as a callback needs an env that
 * lives at least as long, which in practice means the same allocator as the
 * thing holding it.
 *
 * Function values are not comparable, in C or in Go. Go allows func == nil and
 * nothing else, and BURROW_FUNC_IS_NIL is that comparison.
 */

#ifndef BURROW_FUNC_H
#define BURROW_FUNC_H

#include "burrow/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declare a function value type.
 *
 *     BURROW_FUNC(Filter, bool, Str s);
 *     BURROW_FUNC(ReadFn, Int, Slice p, Error *err);
 *     BURROW_FUNC0(Func, void);
 *
 * The arguments after the return type are the parameter list the caller writes,
 * without the env, which the macro puts in front of them. Two macros for the
 * same reason the call macros come in pairs: C99 has no way to write a variadic
 * macro that accepts nothing, and a function value taking no arguments is too
 * common to make people write a comma with nothing after it.
 *
 * Name and Ret are types and cannot be parenthesised, which is what the
 * suppressions are about. */
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define BURROW_FUNC(Name, Ret, ...)                                                    \
    typedef struct Name {                                                              \
        Ret (*f)(void *env, __VA_ARGS__);                                              \
        void *env;                                                                     \
    } Name

/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define BURROW_FUNC0(Name, Ret)                                                        \
    typedef struct Name {                                                              \
        Ret (*f)(void *env);                                                           \
        void *env;                                                                     \
    } Name

/* Go's func(), the plainest function value there is, and the one the rest of
 * the library will pass around most: what sync.Once.Do runs, what a goroutine
 * starts with, what time.AfterFunc fires, what a deferred call is.
 *
 * It is spelled without a package because it has none. It is Go's builtin func
 * type and not any package's, the same way Str, Slice, Map and Error are. */
BURROW_FUNC0(Func, void);

/* Go's func(rune) bool, which is what a scanner's Token takes and what the
 * strings and bytes functions ending in Func take. Builtin in the same sense
 * as Func: Go writes the type out at every use and never names it. */
BURROW_FUNC(RuneFunc, bool, Rune r);

/* Go's func(rune) rune, which is what strings_map and bytes_map take and what
 * the case mappings in unicode have the shape of. A negative result means drop
 * the rune. */
BURROW_FUNC(RuneMapFunc, Rune, Rune r);

/* Build one. The type comes first because C needs it to know what the compound
 * literal is.
 *
 *     Filter f = BURROW_FN(Filter, has_prefix, &env);
 *     Func done = BURROW_FN(Func, close_it, file);
 *
 * Pass NULL for env when the target has nothing to remember.
 *
 * T is a type and cannot be parenthesised. */
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define BURROW_FN(T, fn, env) ((T){(fn), (void *)(env)})

/* nil, the way Go means it for a func. */
#define BURROW_FUNC_IS_NIL(v) ((v).f == NULL)

/* Call one. The env goes in first, which is what Go does too, it just does not
 * make you write it.
 *
 *     bool keep = BURROW_CALLF(f, line);
 *     BURROW_CALLF0(done);
 *
 * No nil check, for the same reason BURROW_CALL has none. */
#define BURROW_CALLF(v, ...) ((v).f((v).env, __VA_ARGS__))
#define BURROW_CALLF0(v) ((v).f((v).env))

#if defined(BURROW_SHORT) && BURROW_SHORT
#define FN(T, fn, env) BURROW_FN(T, fn, env)
#define CALLF(v, ...) BURROW_CALLF(v, __VA_ARGS__)
#define CALLF0(v) BURROW_CALLF0(v)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_FUNC_H */
