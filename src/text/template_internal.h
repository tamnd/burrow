/* text/template internals shared by the template, the executor and the
 * builtin functions.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TEXT_TEMPLATE_INTERNAL_H
#define BURROW_TEXT_TEMPLATE_INTERNAL_H

#include "burrow/text/template.h"

#include "burrow/panic.h"
#include "burrow/reflect.h"
#include "burrow/sync.h"

#include <stdint.h>

/* missingKeyAction, with Go's order. */
enum {
    BURROW__TT_MAP_INVALID, /* return an invalid value */
    BURROW__TT_MAP_ZERO,    /* return the zero value for the map element */
    BURROW__TT_MAP_ERROR    /* error out */
};

/* Go's common. tmpl maps a Str to a TextTemplate *. parse_funcs maps a Str to
 * an Any and holds the builtins' names too, so that the parser sees them
 * without a second map, and exec_funcs maps a Str to the ReflectValue of the
 * function, whose bytes were copied into the template's allocator. */
struct burrow__TextTemplateCommon {
    Map *tmpl;
    SyncRWMutex mu_tmpl;
    int missing_key;
    SyncRWMutex mu_funcs;
    Map *parse_funcs;
    Map *exec_funcs;
};

typedef struct burrow__TtExec burrow__TtExec;

/* What every state of one execution shares: the arena, the stack floor the
 * depth check compares with, and the builtins, as function values whose env is
 * the execution so that what they make goes in its arena. builtins is made the
 * first time one is looked up. */
struct burrow__TtExec {
    Alloc *a;
    uintptr_t stack_floor;
    void *builtins;
};

/* The env that asks for the execution's allocator. A function in a FuncMap
 * whose value has &burrow__tt_exec_env as its env is called with the Alloc *
 * of the execution that calls it as its env instead, so that what it gives back
 * goes in that execution's arena. html/template's escapers are made that way.
 * The function value is a function pointer and then the env, as BURROW_FUNC
 * lays it out. */
extern const char burrow__tt_exec_env;

/* errBadComparisonType and errNoComparison. */
extern const Error burrow__tt_err_bad_comparison_type;
extern const Error burrow__tt_err_no_comparison;

/* Panics with burrow_err_out_of_memory. */
BURROW_NORETURN void burrow__tt_oom(void);

/* The builtin called name, in *fn, or false when there is none. */
bool burrow__tt_find_builtin(burrow__TtExec *x, Str name, ReflectValue *fn);

/* The function the call builtin turns into, which takes the callee's name
 * first. */
ReflectValue burrow__tt_call_fn(burrow__TtExec *x);

/* goodName and goodFunc. goodFunc gives the error in the error arena, or no
 * error. */
bool burrow__tt_good_name(Str name);
Error burrow__tt_good_func(Str name, ReflectType typ);

/* canBeNil, isTrue, indirect, indirectInterface and printableValue. */
bool burrow__tt_can_be_nil(ReflectType typ);
bool burrow__tt_is_true(ReflectValue val, bool *ok);
ReflectValue burrow__tt_indirect(ReflectValue v, bool *is_nil);
ReflectValue burrow__tt_indirect_interface(ReflectValue v);
bool burrow__tt_printable(ReflectValue v, Any *out);

/* safeCall: fun called with args, a Slice of ReflectValue, in a. A panic in the
 * call, or the error the function returned, comes back in *err. */
ReflectValue burrow__tt_safe_call(Alloc *a, ReflectValue fun, Slice args, Error *err);

/* A ReflectValue of type t holding a copy of the bytes at p, made in a. Panics
 * on out of memory. */
ReflectValue burrow__tt_value_of(Alloc *a, ReflectType t, const void *p);

/* A ReflectValue as an Any for fmt, which prints the value it holds the way
 * Go's fmt prints a reflect.Value. */
Any burrow__tt_fmt_arg(ReflectValue v);

/* The ReflectValue a function gave back when its result type is
 * ReflectValue, and v otherwise. */
ReflectValue burrow__tt_unwrap(ReflectValue v);

#endif /* BURROW_TEXT_TEMPLATE_INTERNAL_H */
