/* testing/quick, utility functions to help with black box testing.
 *
 * Package quick implements utility functions to help with black box testing.
 *
 * The testing/quick package is frozen and is not accepting new features.
 *
 *     BURROW_FUNC(PropFn, bool, Int x);
 *     #define PROP_SIG(IN, OUT) IN(0, Int) OUT(bool)
 *     BURROW_FUNC_TYPE_DECL(PropFn);
 *     BURROW_FUNC_TYPE_DEFINE(PropFn, PROP_SIG);
 *
 * A function quick calls is a value of a type declared like that, with
 * BURROW_FUNC and then BURROW_FUNC_TYPE_DEFINE from burrow/declare.h, since
 * the descriptor is what knows how to call it, and it goes in as an Any.
 *
 * Everything quick makes, the arguments, what the function returns, the
 * random source when the Config has none and the error, comes from the
 * allocator it is handed, and none of it is given back, so that is best an
 * arena. Go's -quickchecks flag is left out, since burrow's testing does not
 * read flags it was not built with, so the default count is always 100.
 *
 * A value of a recursive type is made as deep as Go would make it, which rand
 * decides, and a goroutine's stack does not grow the way Go's does. Checking a
 * function that takes one can need a goroutine started with go_stack and a
 * bigger stack than the default.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package testing/quick */

#ifndef BURROW_TESTING_QUICK_H
#define BURROW_TESTING_QUICK_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/math/rand.h"
#include "burrow/mem.h"
#include "burrow/reflect.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A Generator can generate random values of its own type.
 *
 * A type is one when a vtable for it was recorded with BURROW_IMPLEMENTS, as
 * burrow/type.h describes, and quick_value calls it on the zero value of the
 * type, the way Go calls the method. Generate takes the allocator the value
 * should come from, which Go's does not. */
typedef struct QuickGeneratorVT {
    const Type *self_type;
    /* Generate returns a random instance of the type on which it is a
     * method using the size as a size hint. */
    ReflectValue (*generate)(void *self, Alloc *a, MathRandRand *rand, Int size);
} QuickGeneratorVT;

typedef struct QuickGenerator {
    const QuickGeneratorVT *vt;
    void *data;
} QuickGenerator;

extern const Type burrow_type_QuickGenerator;
#define TYPE_QUICK_GENERATOR TYPE_OF(QuickGenerator)

/* quick.Value: returns an arbitrary value of the given type, made in a, and
 * whether it could, in *ok. If the type implements the Generator interface,
 * that will be used. Note: To create arbitrary values for structs, all the
 * fields must be exported. */
BURROW_OWNS(ret) ReflectValue quick_value(Alloc *a, ReflectType t, MathRandRand *rand,
                                          bool *ok);

/* Config.Values, which is handed a Slice of ReflectValue to fill in. */
BURROW_FUNC(QuickValuesFunc, void, Slice values, MathRandRand *rand);

/* A Config structure contains options for running a test. A NULL Config is
 * the zero one. */
typedef struct QuickConfig {
    /* MaxCount sets the maximum number of iterations.
     * If zero, MaxCountScale is used. */
    Int max_count;
    /* MaxCountScale is a non-negative scale factor applied to the
     * default maximum.
     * A count of zero implies the default, which is usually 100
     * but can be set by the -quickchecks flag. */
    double max_count_scale;
    /* Rand specifies a source of random numbers.
     * If nil, a default pseudo-random source will be used. */
    MathRandRand *rand;
    /* Values specifies a function to generate a slice of
     * arbitrary reflect.Values that are congruent with the
     * arguments to the function being tested.
     * If nil, the top-level Value function is used to generate them. */
    QuickValuesFunc values;
} QuickConfig;

/* A SetupError is the result of an error in the way that check is being
 * used, independent of the functions being tested. errors_as with
 * TYPE_QUICK_SETUP_ERROR gives a pointer to the text. */
typedef Str QuickSetupError;

extern const Type *const TYPE_QUICK_SETUP_ERROR;

/* SetupError.Error, which is s itself. */
BURROW_BORROWS(ret, s) Str quick_setup_error_error(QuickSetupError s);

/* An Error for s, which is copied into a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error quick_setup_error_as_error(QuickSetupError s, Alloc *a);

/* A CheckError is the result of Check finding an error. In is a Slice of Any.
 * errors_as with TYPE_QUICK_CHECK_ERROR gives a pointer to one. */
typedef struct QuickCheckError {
    Int count;
    Slice in;
} QuickCheckError;

extern const Type *const TYPE_QUICK_CHECK_ERROR;

/* CheckError.Error, built in a. */
BURROW_OWNS(ret) Str quick_check_error_error(const QuickCheckError *s, Alloc *a);

/* An Error for a copy of *s, made in a with its message. The copy shares s's
 * In. Out of memory gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error quick_check_error_as_error(const QuickCheckError *s, Alloc *a);

/* A CheckEqualError is the result CheckEqual finding an error. Out1 and Out2
 * are Slices of Any. errors_as with TYPE_QUICK_CHECK_EQUAL_ERROR gives a
 * pointer to one, and errors_as with TYPE_QUICK_CHECK_ERROR does not, the same
 * as in Go, where the CheckError is embedded and not the error's type. */
typedef struct QuickCheckEqualError {
    QuickCheckError check_error;
    Slice out1;
    Slice out2;
} QuickCheckEqualError;

extern const Type *const TYPE_QUICK_CHECK_EQUAL_ERROR;

/* CheckEqualError.Error, built in a. */
BURROW_OWNS(ret) Str quick_check_equal_error_error(const QuickCheckEqualError *s,
                                                   Alloc *a);

/* An Error for a copy of *s, made in a with its message, sharing s's slices.
 * Out of memory gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error quick_check_equal_error_as_error(const QuickCheckEqualError *s,
                                                        Alloc *a);

/* quick.Check: looks for an input to f, any function that returns bool,
 * such that f returns false. It calls f repeatedly, with arbitrary
 * values for each argument. If f returns false on a given input,
 * Check returns that input as a *CheckError.
 * For example:
 *
 *     static bool odd_multiple(void *env, Int x) {
 *         Int y = odd_multiple_of_three(x);
 *         return y % 2 == 1 && y % 3 == 0;
 *     }
 *
 *     static void TestOddMultipleOfThree(TestingT *t) {
 *         Arena ar;
 *         arena_init(&ar, NULL, 0);
 *         PropFn f = {odd_multiple, NULL};
 *         Error err = quick_check(arena_allocator(&ar),
 *                                 BURROW_ANY(TYPE_OF(PropFn), &f), NULL);
 *         if (BURROW_FAILED(err))
 *             testing_t_error_v(t, err);
 *         arena_free(&ar);
 *     }
 *
 * f is the function value itself in an Any, and everything Check makes comes
 * from a. */
BURROW_OWNS(ret) Error quick_check(Alloc *a, Any f, const QuickConfig *config);

/* quick.CheckEqual: looks for an input on which f and g return different
 * results. It calls f and g repeatedly with arbitrary values for each argument.
 * If f and g return different answers, CheckEqual returns a *CheckEqualError
 * describing the input and the outputs.
 *
 * f and g have to have the same type, which here means the same descriptor. */
BURROW_OWNS(ret) Error quick_check_equal(Alloc *a, Any f, Any g,
                                         const QuickConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_QUICK_H */
