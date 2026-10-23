/* text/template's builtin functions, the escapers, and the helpers the
 * executor shares with them.
 *
 * The builtins are function values with the signatures Go gives them, so that
 * the executor checks and converts their arguments the way it does for a
 * function in a FuncMap. Their env is the execution, whose arena holds what
 * they make.
 *
 * Derived from Go's src/text/template/funcs.go and exec.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/template.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/reflect.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"
#include "template_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(burrow__tt_err_bad_comparison_type,
                      "invalid type for comparison");
BURROW_SENTINEL_ERROR(burrow__tt_err_no_comparison, "missing argument for comparison");

BURROW_NORETURN void burrow__tt_oom(void) {
    panic(BURROW_ANY(TYPE_ERROR, (uintptr_t)&burrow_err_out_of_memory));
}

static const ReflectValue ttf_zero = {NULL, NULL, 0};

/* ------------------------------------------------------------ the helpers */

ReflectValue burrow__tt_value_of(Alloc *a, ReflectType t, const void *p) {
    size_t size = t->size == 0 ? 1 : (size_t)t->size;
    void *m = mem_alloc_nozero(a, size, t->align == 0 ? 1 : (size_t)t->align);
    if (m == NULL)
        burrow__tt_oom();
    if (t->size > 0)
        memcpy(m, p, (size_t)t->size);
    return reflect_value_of(BURROW_ANY(t, m));
}

ReflectValue burrow__tt_unwrap(ReflectValue v) {
    if (reflect_value_is_valid(v) && reflect_value_type(v) == TYPE_REFLECT_VALUE)
        return *(const ReflectValue *)v.ptr;
    return v;
}

static const Str ttf_invalid_value = BURROW_S_INIT("<invalid reflect.Value>");
static const Str ttf_no_value = BURROW_S_INIT("<no value>");

Any burrow__tt_fmt_arg(ReflectValue v) {
    if (!reflect_value_is_valid(v))
        return BURROW_ANY(TYPE_STRING, (uintptr_t)&ttf_invalid_value);
    if (reflect_value_kind(v) == REFLECT_INTERFACE) {
        if (reflect_value_is_nil(v))
            return (Any){NULL, NULL};
        /* fmt prints an error itself, from the interface. */
        if (reflect_value_type(v) == TYPE_ERROR)
            return BURROW_ANY(TYPE_ERROR, v.ptr);
    }
    if (!reflect_value_can_interface(v))
        return BURROW_ANY(reflect_value_type(v), v.ptr);
    return reflect_value_interface(v);
}

bool burrow__tt_can_be_nil(ReflectType typ) {
    switch ((int)reflect_type_kind(typ)) {
    case REFLECT_CHAN:
    case REFLECT_FUNC:
    case REFLECT_INTERFACE:
    case REFLECT_MAP:
    case REFLECT_POINTER:
    case REFLECT_SLICE:
        return true;
    case REFLECT_STRUCT:
        return typ == TYPE_REFLECT_VALUE;
    default:
        return false;
    }
}

bool burrow__tt_good_name(Str name) {
    if (name.len == 0)
        return false;
    for (Int i = 0; i < name.len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string((Str){name.p + i, name.len - i}, &size);
        if (r != '_') {
            if (i == 0 && !unicode_is_letter(r))
                return false;
            if (!unicode_is_letter(r) && !unicode_is_digit(r))
                return false;
        }
        i += size;
    }
    return true;
}

Error burrow__tt_good_func(Str name, ReflectType typ) {
    /* We allow functions with 1 result or 2 results where the second is an
     * error. */
    Int num_out = reflect_type_num_out(typ);
    if (num_out == 1)
        return BURROW_NO_ERROR;
    if (num_out == 2 && reflect_type_out(typ, 1) == TYPE_ERROR)
        return BURROW_NO_ERROR;
    if (num_out == 2)
        return fmt_errorf_v("invalid function signature for %s: second return value "
                            "should be error; is %s",
                            name, reflect_type_string(reflect_type_out(typ, 1)));
    return fmt_errorf_v("function %s has %d return values; should be 1 or 2", name,
                        num_out);
}

bool burrow__tt_is_true(ReflectValue val, bool *ok) {
    *ok = true;
    if (!reflect_value_is_valid(val))
        /* Something like var x interface{}, never set. It's a form of nil. */
        return false;
    switch ((int)reflect_value_kind(val)) {
    case REFLECT_ARRAY:
    case REFLECT_MAP:
    case REFLECT_SLICE:
    case REFLECT_STRING:
        return reflect_value_len(val) > 0;
    case REFLECT_BOOL:
        return reflect_value_bool(val);
    case REFLECT_COMPLEX64:
    case REFLECT_COMPLEX128: {
        Complex128 c = reflect_value_complex(val);
        return c.re != 0 || c.im != 0;
    }
    case REFLECT_CHAN:
    case REFLECT_FUNC:
    case REFLECT_POINTER:
    case REFLECT_UNSAFE_POINTER:
    case REFLECT_INTERFACE:
        return !reflect_value_is_nil(val);
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
        return reflect_value_int(val) != 0;
    case REFLECT_FLOAT32:
    case REFLECT_FLOAT64:
        return reflect_value_float(val) != 0;
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        return reflect_value_uint(val) != 0;
    case REFLECT_STRUCT:
        return true; /* Struct values are always true. */
    default:
        *ok = false;
        return false;
    }
}

static bool ttf_truth(ReflectValue arg) {
    bool ok = false;
    return burrow__tt_is_true(burrow__tt_indirect_interface(arg), &ok);
}

ReflectValue burrow__tt_indirect(ReflectValue v, bool *is_nil) {
    for (;;) {
        ReflectKind k = reflect_value_kind(v);
        if (k != REFLECT_POINTER && k != REFLECT_INTERFACE)
            break;
        if (reflect_value_is_nil(v)) {
            *is_nil = true;
            return v;
        }
        v = reflect_value_elem(v);
    }
    *is_nil = false;
    return v;
}

ReflectValue burrow__tt_indirect_interface(ReflectValue v) {
    if (reflect_value_kind(v) != REFLECT_INTERFACE)
        return v;
    if (reflect_value_is_nil(v))
        return ttf_zero;
    return reflect_value_elem(v);
}

/* Whether fmt prints a value of type t with a method, Error or String. A
 * receiver is always a pointer in burrow, so T has the methods Go gives *T and
 * printableValue's step to the address changes nothing. */
static bool ttf_has_print_method(ReflectType t) {
    ReflectMethod m;
    return reflect_type_implements(t, TYPE_ERROR) ||
           reflect_type_method_by_name(t, BURROW_S("String"), &m);
}

bool burrow__tt_printable(ReflectValue v, Any *out) {
    if (reflect_value_kind(v) == REFLECT_POINTER) {
        bool is_nil = false;
        v = burrow__tt_indirect(v, &is_nil); /* fmt_fprint handles nil. */
    }
    if (!reflect_value_is_valid(v)) {
        *out = BURROW_ANY(TYPE_STRING, (uintptr_t)&ttf_no_value);
        return true;
    }
    if (!ttf_has_print_method(reflect_value_type(v))) {
        ReflectKind k = reflect_value_kind(v);
        if (k == REFLECT_CHAN || k == REFLECT_FUNC)
            return false;
    }
    *out = burrow__tt_fmt_arg(v);
    return true;
}

/* ---------------------------------------------------------------- calling */

/* The call itself, apart from safeCall's recovery, which keeps what setjmp
 * could clobber out of the function that calls it. */
static void ttf_call_into(ReflectValue fun, Alloc *a, Slice args, Slice *ret) {
    *ret = reflect_value_call(fun, a, args);
}

/* The error a panic in a called function becomes. */
static Error ttf_panic_error(Any p) {
    if (p.t == TYPE_ERROR)
        return *(const Error *)p.data;
    return fmt_errorf_v("%v", p);
}

ReflectValue burrow__tt_safe_call(Alloc *a, ReflectValue fun, Slice args, Error *err) {
    Slice ret = slice_nil(TYPE_REFLECT_VALUE);
    Error e = BURROW_NO_ERROR;
    volatile bool failed = false;
    BURROW_TRY {
        ttf_call_into(fun, a, args, &ret);
    }
    BURROW_CATCH(p) {
        e = ttf_panic_error(p);
        failed = true;
    }
    BURROW_TRY_END;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (failed) {
        *err = e;
        return ttf_zero;
    }
    if (ret.len == 2) {
        ReflectValue ev = BURROW_AT(ReflectValue, ret, 1);
        Error re = *(const Error *)ev.ptr;
        if (BURROW_FAILED(re))
            *err = re;
    }
    return ret.len > 0 ? BURROW_AT(ReflectValue, ret, 0) : ttf_zero;
}

static bool ttf_int_like(ReflectKind k) {
    switch ((int)k) {
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        return true;
    default:
        return false;
    }
}

/* prepareArg: whether value can be used as an argument of type arg_type, and
 * the zero arg_type in place of an invalid value when it can be nil. */
static ReflectValue ttf_prepare_arg(Alloc *a, ReflectValue value, ReflectType arg_type,
                                    Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!reflect_value_is_valid(value)) {
        if (!burrow__tt_can_be_nil(arg_type)) {
            *err = fmt_errorf_v("value is nil; should be of type %s",
                                reflect_type_string(arg_type));
            return ttf_zero;
        }
        value = reflect_zero(arg_type);
    }
    ReflectType vt = reflect_value_type(value);
    if (reflect_type_assignable_to(vt, arg_type))
        return value;
    if (ttf_int_like(reflect_value_kind(value)) &&
        ttf_int_like(reflect_type_kind(arg_type)) &&
        reflect_type_convertible_to(vt, arg_type))
        return reflect_value_convert(value, a, arg_type);
    *err = fmt_errorf_v("value has type %s; should be %s", reflect_type_string(vt),
                        reflect_type_string(arg_type));
    return ttf_zero;
}

/* indexArg: index as an int no greater than cap, in *x. */
static bool ttf_index_arg(ReflectValue index, Int cap, Int *x, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    int64_t v = 0;
    switch ((int)reflect_value_kind(index)) {
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
        v = reflect_value_int(index);
        break;
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        v = (int64_t)reflect_value_uint(index);
        break;
    case REFLECT_INVALID:
        *err = fmt_errorf_v("cannot index slice/array with nil");
        return false;
    default:
        *err = fmt_errorf_v("cannot index slice/array with type %s",
                            reflect_type_string(reflect_value_type(index)));
        return false;
    }
    if (v < 0 || (int64_t)(Int)v != v || (Int)v > cap) {
        *err = fmt_errorf_v("index out of range: %d", v);
        return false;
    }
    *x = (Int)v;
    return true;
}

/* -------------------------------------------------------------- the types */

typedef Slice TtfValues;
typedef Slice TtfAnys;
static BURROW_SLICE_TYPE_DEFINE(TtfValues, ReflectValue);
static BURROW_SLICE_TYPE_DEFINE(TtfAnys, Any);

/* func(reflect.Value, ...reflect.Value) reflect.Value: and, or and emptyCall. */
BURROW_FUNC(burrow__TtfAndFn, ReflectValue, ReflectValue arg0, Slice args);
#define TTF_AND_SIG(IN, OUT) IN(0, ReflectValue) IN(1, TtfValues) OUT(ReflectValue)
BURROW_FUNC_TYPE_DECL(burrow__TtfAndFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfAndFn, ReflectValue, TTF_AND_SIG);

/* func(...any) string: html, js, print, println and urlquery. */
BURROW_FUNC(burrow__TtfPrintFn, Str, Slice args);
#define TTF_PRINT_SIG(IN, OUT) IN(0, TtfAnys) OUT(Str)
BURROW_FUNC_TYPE_DECL(burrow__TtfPrintFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfPrintFn, Any, TTF_PRINT_SIG);

/* func(string, ...any) string: printf. */
BURROW_FUNC(burrow__TtfPrintfFn, Str, Str format, Slice args);
#define TTF_PRINTF_SIG(IN, OUT) IN(0, Str) IN(1, TtfAnys) OUT(Str)
BURROW_FUNC_TYPE_DECL(burrow__TtfPrintfFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfPrintfFn, Any, TTF_PRINTF_SIG);

/* func(reflect.Value, ...reflect.Value) (reflect.Value, error): index and
 * slice. */
BURROW_FUNC(burrow__TtfIndexFn, ReflectValue, ReflectValue item, Slice indexes,
            Error *err);
#define TTF_INDEX_SIG(IN, OUT)                                                         \
    IN(0, ReflectValue) IN(1, TtfValues) OUT(ReflectValue) OUT(ErrorResult)
BURROW_FUNC_TYPE_DECL(burrow__TtfIndexFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfIndexFn, ReflectValue, TTF_INDEX_SIG);

/* func(reflect.Value) (int, error): len. */
BURROW_FUNC(burrow__TtfLenFn, Int, ReflectValue item, Error *err);
#define TTF_LEN_SIG(IN, OUT) IN(0, ReflectValue) OUT(Int) OUT(ErrorResult)
BURROW_FUNC_TYPE_DECL(burrow__TtfLenFn);
BURROW_FUNC_TYPE_DEFINE(burrow__TtfLenFn, TTF_LEN_SIG);

/* func(reflect.Value) bool: not. */
BURROW_FUNC(burrow__TtfNotFn, bool, ReflectValue arg);
#define TTF_NOT_SIG(IN, OUT) IN(0, ReflectValue) OUT(bool)
BURROW_FUNC_TYPE_DECL(burrow__TtfNotFn);
BURROW_FUNC_TYPE_DEFINE(burrow__TtfNotFn, TTF_NOT_SIG);

/* func(reflect.Value, ...reflect.Value) (bool, error): eq. */
BURROW_FUNC(burrow__TtfEqFn, bool, ReflectValue arg1, Slice arg2, Error *err);
#define TTF_EQ_SIG(IN, OUT)                                                            \
    IN(0, ReflectValue) IN(1, TtfValues) OUT(bool) OUT(ErrorResult)
BURROW_FUNC_TYPE_DECL(burrow__TtfEqFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfEqFn, ReflectValue, TTF_EQ_SIG);

/* func(reflect.Value, reflect.Value) (bool, error): ge, gt, le, lt and ne. */
BURROW_FUNC(burrow__TtfCmpFn, bool, ReflectValue arg1, ReflectValue arg2, Error *err);
#define TTF_CMP_SIG(IN, OUT)                                                           \
    IN(0, ReflectValue) IN(1, ReflectValue) OUT(bool) OUT(ErrorResult)
BURROW_FUNC_TYPE_DECL(burrow__TtfCmpFn);
BURROW_FUNC_TYPE_DEFINE(burrow__TtfCmpFn, TTF_CMP_SIG);

/* func(string, reflect.Value, ...reflect.Value) (reflect.Value, error): the
 * call that the call builtin turns into. */
BURROW_FUNC(burrow__TtfCallFn, ReflectValue, Str name, ReflectValue fn, Slice args,
            Error *err);
#define TTF_CALL_SIG(IN, OUT)                                                          \
    IN(0, Str) IN(1, ReflectValue) IN(2, TtfValues) OUT(ReflectValue) OUT(ErrorResult)
BURROW_FUNC_TYPE_DECL(burrow__TtfCallFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(burrow__TtfCallFn, ReflectValue, TTF_CALL_SIG);

/* --------------------------------------------------------------- indexing */

static ReflectValue ttf_index(void *env, ReflectValue item, Slice indexes, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    burrow__TtExec *x = (burrow__TtExec *)env;
    item = burrow__tt_indirect_interface(item);
    if (!reflect_value_is_valid(item)) {
        *err = fmt_errorf_v("index of untyped nil");
        return ttf_zero;
    }
    for (Int i = 0; i < indexes.len; i++) {
        ReflectValue index =
            burrow__tt_indirect_interface(BURROW_AT(ReflectValue, indexes, i));
        bool is_nil = false;
        item = burrow__tt_indirect(item, &is_nil);
        if (is_nil) {
            *err = fmt_errorf_v("index of nil pointer");
            return ttf_zero;
        }
        switch ((int)reflect_value_kind(item)) {
        case REFLECT_ARRAY:
        case REFLECT_SLICE:
        case REFLECT_STRING: {
            Int n = 0;
            if (!ttf_index_arg(index, reflect_value_len(item), &n, err))
                return ttf_zero;
            item = reflect_value_index(item, n);
            break;
        }
        case REFLECT_MAP: {
            ReflectType mt = reflect_value_type(item);
            Error e = BURROW_NO_ERROR;
            ReflectValue key = ttf_prepare_arg(x->a, index, reflect_type_key(mt), &e);
            if (BURROW_FAILED(e)) {
                *err = e;
                return ttf_zero;
            }
            ReflectValue v = reflect_value_map_index(item, key);
            item = reflect_value_is_valid(v) ? v : reflect_zero(reflect_type_elem(mt));
            break;
        }
        default:
            *err = fmt_errorf_v("can't index item of type %s",
                                reflect_type_string(reflect_value_type(item)));
            return ttf_zero;
        }
    }
    return item;
}

/* ---------------------------------------------------------------- slicing */

static ReflectValue ttf_slice(void *env, ReflectValue item, Slice indexes, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    burrow__TtExec *x = (burrow__TtExec *)env;
    item = burrow__tt_indirect_interface(item);
    if (!reflect_value_is_valid(item)) {
        *err = fmt_errorf_v("slice of untyped nil");
        return ttf_zero;
    }
    bool is_nil = false;
    item = burrow__tt_indirect(item, &is_nil);
    if (is_nil) {
        *err = fmt_errorf_v("slice of nil pointer");
        return ttf_zero;
    }
    if (indexes.len > 3) {
        *err = fmt_errorf_v("too many slice indexes: %d", indexes.len);
        return ttf_zero;
    }
    Int cap = 0;
    switch ((int)reflect_value_kind(item)) {
    case REFLECT_STRING:
        if (indexes.len == 3) {
            *err = fmt_errorf_v("cannot 3-index slice a string");
            return ttf_zero;
        }
        cap = reflect_value_len(item);
        break;
    case REFLECT_ARRAY:
    case REFLECT_SLICE:
        cap = reflect_value_cap(item);
        break;
    default:
        *err = fmt_errorf_v("can't slice item of type %s",
                            reflect_type_string(reflect_value_type(item)));
        return ttf_zero;
    }
    /* set default values for cases item[:], item[i:]. */
    Int idx[3] = {0, reflect_value_len(item), 0};
    for (Int i = 0; i < indexes.len; i++) {
        if (!ttf_index_arg(BURROW_AT(ReflectValue, indexes, i), cap, &idx[i], err))
            return ttf_zero;
    }
    /* given item[i:j], make sure i <= j. */
    if (idx[0] > idx[1]) {
        *err = fmt_errorf_v("invalid slice index: %d > %d", idx[0], idx[1]);
        return ttf_zero;
    }
    if (indexes.len < 3)
        return reflect_value_slice(item, x->a, idx[0], idx[1]);
    /* given item[i:j:k], make sure i <= j <= k. */
    if (idx[1] > idx[2]) {
        *err = fmt_errorf_v("invalid slice index: %d > %d", idx[1], idx[2]);
        return ttf_zero;
    }
    return reflect_value_slice3(item, x->a, idx[0], idx[1], idx[2]);
}

/* ----------------------------------------------------------------- length */

static Int ttf_length(void *env, ReflectValue item, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)env;
    bool is_nil = false;
    item = burrow__tt_indirect(item, &is_nil);
    if (is_nil) {
        *err = fmt_errorf_v("len of nil pointer");
        return 0;
    }
    switch ((int)reflect_value_kind(item)) {
    case REFLECT_ARRAY:
    case REFLECT_CHAN:
    case REFLECT_MAP:
    case REFLECT_SLICE:
    case REFLECT_STRING:
        return reflect_value_len(item);
    default:
        break;
    }
    *err =
        fmt_errorf_v("len of type %s", reflect_type_string(reflect_value_type(item)));
    return 0;
}

/* ---------------------------------------------------- function invocation */

/* and, or and emptyCall, which the executor never calls: it handles them
 * itself. */
static ReflectValue ttf_unreachable(void *env, ReflectValue arg0, Slice args) {
    (void)env;
    (void)arg0;
    (void)args;
    panic_str(BURROW_S("unreachable"));
}

/* call: the result of calling fn with args. fn must return 1 result, or 2
 * results, the second of which is an error. */
static ReflectValue ttf_call(void *env, Str name, ReflectValue fn, Slice args,
                             Error *err) {
    burrow__TtExec *x = (burrow__TtExec *)env;
    fn = burrow__tt_indirect_interface(fn);
    if (!reflect_value_is_valid(fn)) {
        *err = fmt_errorf_v("call of nil");
        return ttf_zero;
    }
    ReflectType typ = reflect_value_type(fn);
    if (reflect_type_kind(typ) != REFLECT_FUNC) {
        *err =
            fmt_errorf_v("non-function %s of type %s", name, reflect_type_string(typ));
        return ttf_zero;
    }
    Error e = burrow__tt_good_func(name, typ);
    if (BURROW_FAILED(e)) {
        *err = e;
        return ttf_zero;
    }
    Int num_in = reflect_type_num_in(typ);
    bool variadic = reflect_type_is_variadic(typ);
    ReflectType ddd_type = NULL;
    if (variadic) {
        if (args.len < num_in - 1) {
            *err = fmt_errorf_v("wrong number of args for %s: got %d want at least %d",
                                name, args.len, num_in - 1);
            return ttf_zero;
        }
        ddd_type = reflect_type_elem(reflect_type_in(typ, num_in - 1));
    } else if (args.len != num_in) {
        *err = fmt_errorf_v("wrong number of args for %s: got %d want %d", name,
                            args.len, num_in);
        return ttf_zero;
    }
    Slice argv = slice_make(x->a, TYPE_REFLECT_VALUE, args.len, args.len);
    if (args.len > 0 && argv.p == NULL)
        burrow__tt_oom();
    for (Int i = 0; i < args.len; i++) {
        ReflectValue arg =
            burrow__tt_indirect_interface(BURROW_AT(ReflectValue, args, i));
        /* Compute the expected type. Clumsy because of variadics. */
        ReflectType arg_type = ddd_type;
        if (!variadic || i < num_in - 1)
            arg_type = reflect_type_in(typ, i);
        Error ae = BURROW_NO_ERROR;
        BURROW_AT(ReflectValue, argv, i) = ttf_prepare_arg(x->a, arg, arg_type, &ae);
        if (BURROW_FAILED(ae)) {
            *err = fmt_errorf_v("arg %d: %w", i, ae);
            return ttf_zero;
        }
    }
    return burrow__tt_safe_call(x->a, fn, argv, err);
}

/* ---------------------------------------------------------- boolean logic */

static bool ttf_not(void *env, ReflectValue arg) {
    (void)env;
    return !ttf_truth(arg);
}

/* ------------------------------------------------------------- comparison */

enum {
    TTF_INVALID_KIND,
    TTF_BOOL_KIND,
    TTF_COMPLEX_KIND,
    TTF_INT_KIND,
    TTF_FLOAT_KIND,
    TTF_STRING_KIND,
    TTF_UINT_KIND
};

static int ttf_basic_kind(ReflectValue v) {
    switch ((int)reflect_value_kind(v)) {
    case REFLECT_BOOL:
        return TTF_BOOL_KIND;
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
        return TTF_INT_KIND;
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        return TTF_UINT_KIND;
    case REFLECT_FLOAT32:
    case REFLECT_FLOAT64:
        return TTF_FLOAT_KIND;
    case REFLECT_COMPLEX64:
    case REFLECT_COMPLEX128:
        return TTF_COMPLEX_KIND;
    case REFLECT_STRING:
        return TTF_STRING_KIND;
    default:
        return TTF_INVALID_KIND;
    }
}

/* isNil: whether v is the zero Value, or nil of its type. */
static bool ttf_is_nil(ReflectValue v) {
    if (!reflect_value_is_valid(v))
        return true;
    switch ((int)reflect_value_kind(v)) {
    case REFLECT_CHAN:
    case REFLECT_FUNC:
    case REFLECT_INTERFACE:
    case REFLECT_MAP:
    case REFLECT_POINTER:
    case REFLECT_SLICE:
        return reflect_value_is_nil(v);
    default:
        return false;
    }
}

/* canCompare: whether v1 and v2 are both the same kind, or one is nil. */
static bool ttf_can_compare(ReflectValue v1, ReflectValue v2) {
    ReflectKind k1 = reflect_value_kind(v1);
    ReflectKind k2 = reflect_value_kind(v2);
    if (k1 == k2)
        return true;
    /* We know the type can be compared to nil. */
    return k1 == REFLECT_INVALID || k2 == REFLECT_INVALID;
}

static Str ttf_type_string(ReflectValue v) {
    return reflect_value_is_valid(v) ? reflect_type_string(reflect_value_type(v))
                                     : BURROW_S("<nil>");
}

/* Go's arg1.Interface() == arg.Interface(), for values of the same kind that
 * are neither nil nor basic. */
static bool ttf_iface_equal(ReflectValue a, ReflectValue b) {
    if (reflect_value_type(a) != reflect_value_type(b))
        return false;
    return reflect_value_equal(a, b);
}

static bool ttf_eq1(ReflectValue arg1, int k1, ReflectValue arg, bool *truth,
                    Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    arg = burrow__tt_indirect_interface(arg);
    int k2 = ttf_basic_kind(arg);
    *truth = false;
    if (k1 != k2) {
        /* Special case: Can compare integer values regardless of type's sign. */
        if (k1 == TTF_INT_KIND && k2 == TTF_UINT_KIND) {
            int64_t i = reflect_value_int(arg1);
            *truth = i >= 0 && (uint64_t)i == reflect_value_uint(arg);
        } else if (k1 == TTF_UINT_KIND && k2 == TTF_INT_KIND) {
            int64_t i = reflect_value_int(arg);
            *truth = i >= 0 && reflect_value_uint(arg1) == (uint64_t)i;
        } else if (reflect_value_is_valid(arg1) && reflect_value_is_valid(arg)) {
            *err = fmt_errorf_v("incompatible types for comparison: %v and %v",
                                ttf_type_string(arg1), ttf_type_string(arg));
            return false;
        }
        return true;
    }
    switch (k1) {
    case TTF_BOOL_KIND:
        *truth = reflect_value_bool(arg1) == reflect_value_bool(arg);
        break;
    case TTF_COMPLEX_KIND: {
        Complex128 c1 = reflect_value_complex(arg1);
        Complex128 c2 = reflect_value_complex(arg);
        *truth = c1.re == c2.re && c1.im == c2.im;
        break;
    }
    case TTF_FLOAT_KIND:
        *truth = reflect_value_float(arg1) == reflect_value_float(arg);
        break;
    case TTF_INT_KIND:
        *truth = reflect_value_int(arg1) == reflect_value_int(arg);
        break;
    case TTF_STRING_KIND:
        *truth = str_eq(reflect_value_string(arg1), reflect_value_string(arg));
        break;
    case TTF_UINT_KIND:
        *truth = reflect_value_uint(arg1) == reflect_value_uint(arg);
        break;
    default:
        if (!ttf_can_compare(arg1, arg)) {
            *err = fmt_errorf_v("non-comparable types %s: %v, %s: %v",
                                burrow__tt_fmt_arg(arg1), ttf_type_string(arg1),
                                ttf_type_string(arg), burrow__tt_fmt_arg(arg));
            return false;
        }
        if (ttf_is_nil(arg1) || ttf_is_nil(arg)) {
            *truth = ttf_is_nil(arg) == ttf_is_nil(arg1);
        } else {
            if (!reflect_type_comparable(reflect_value_type(arg))) {
                *err = fmt_errorf_v("non-comparable type %s: %v",
                                    burrow__tt_fmt_arg(arg), ttf_type_string(arg));
                return false;
            }
            *truth = ttf_iface_equal(arg1, arg);
        }
        break;
    }
    return true;
}

/* eq: arg1 == arg2[0] || arg1 == arg2[1] || ... */
static bool ttf_eq(void *env, ReflectValue arg1, Slice arg2, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)env;
    arg1 = burrow__tt_indirect_interface(arg1);
    if (arg2.len == 0) {
        *err = burrow__tt_err_no_comparison;
        return false;
    }
    int k1 = ttf_basic_kind(arg1);
    for (Int i = 0; i < arg2.len; i++) {
        bool truth = false;
        if (!ttf_eq1(arg1, k1, BURROW_AT(ReflectValue, arg2, i), &truth, err))
            return false;
        if (truth)
            return true;
    }
    return false;
}

/* ne: != is the inverse of ==. */
static bool ttf_ne(void *env, ReflectValue arg1, ReflectValue arg2, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool equal = ttf_eq(env, arg1, slice_from(&arg2, 1, 1, TYPE_REFLECT_VALUE), &e);
    *err = e;
    return !equal;
}

static bool ttf_lt(void *env, ReflectValue arg1, ReflectValue arg2, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)env;
    arg1 = burrow__tt_indirect_interface(arg1);
    int k1 = ttf_basic_kind(arg1);
    if (k1 == TTF_INVALID_KIND) {
        *err = burrow__tt_err_bad_comparison_type;
        return false;
    }
    arg2 = burrow__tt_indirect_interface(arg2);
    int k2 = ttf_basic_kind(arg2);
    if (k2 == TTF_INVALID_KIND) {
        *err = burrow__tt_err_bad_comparison_type;
        return false;
    }
    if (k1 != k2) {
        /* Special case: Can compare integer values regardless of type's sign. */
        if (k1 == TTF_INT_KIND && k2 == TTF_UINT_KIND) {
            int64_t i = reflect_value_int(arg1);
            return i < 0 || (uint64_t)i < reflect_value_uint(arg2);
        }
        if (k1 == TTF_UINT_KIND && k2 == TTF_INT_KIND) {
            int64_t i = reflect_value_int(arg2);
            return i >= 0 && reflect_value_uint(arg1) < (uint64_t)i;
        }
        *err = fmt_errorf_v("incompatible types for comparison: %v and %v",
                            ttf_type_string(arg1), ttf_type_string(arg2));
        return false;
    }
    switch (k1) {
    case TTF_FLOAT_KIND:
        return reflect_value_float(arg1) < reflect_value_float(arg2);
    case TTF_INT_KIND:
        return reflect_value_int(arg1) < reflect_value_int(arg2);
    case TTF_STRING_KIND:
        return str_cmp(reflect_value_string(arg1), reflect_value_string(arg2)) < 0;
    case TTF_UINT_KIND:
        return reflect_value_uint(arg1) < reflect_value_uint(arg2);
    default: /* bool and complex */
        *err = burrow__tt_err_bad_comparison_type;
        return false;
    }
}

/* le: <= is < or ==. */
static bool ttf_le(void *env, ReflectValue arg1, ReflectValue arg2, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool less_than = ttf_lt(env, arg1, arg2, &e);
    if (less_than || BURROW_FAILED(e)) {
        *err = e;
        return less_than;
    }
    return ttf_eq(env, arg1, slice_from(&arg2, 1, 1, TYPE_REFLECT_VALUE), err);
}

/* gt: > is the inverse of <=. */
static bool ttf_gt(void *env, ReflectValue arg1, ReflectValue arg2, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    bool less_or_equal = ttf_le(env, arg1, arg2, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return false;
    }
    return !less_or_equal;
}

/* ge: >= is the inverse of <. */
static bool ttf_ge(void *env, ReflectValue arg1, ReflectValue arg2, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    bool less_than = ttf_lt(env, arg1, arg2, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return false;
    }
    return !less_than;
}

/* --------------------------------------------------------------- printing */

/* evalArgs: fmt_sprint of args, with each argument indirected the way the
 * executor does before it prints a value. args is copied first, so the
 * caller's Slice is left alone. */
static Str ttf_eval_args(Alloc *a, Slice args) {
    /* Fast path for simple common case. */
    if (args.len == 1) {
        Any arg = BURROW_AT(Any, args, 0);
        if (arg.t == TYPE_STRING)
            return *(const Str *)arg.data;
    }
    Slice c = slice_make(a, TYPE_ANY, args.len, args.len);
    if (args.len > 0 && c.p == NULL)
        burrow__tt_oom();
    for (Int i = 0; i < args.len; i++) {
        Any arg = BURROW_AT(Any, args, i);
        Any p;
        if (burrow__tt_printable(reflect_value_of(arg), &p))
            arg = p; /* else let fmt do its thing */
        BURROW_AT(Any, c, i) = arg;
    }
    return fmt_sprint(a, c);
}

static Str ttf_print(void *env, Slice args) {
    return fmt_sprint(((burrow__TtExec *)env)->a, args);
}

static Str ttf_println(void *env, Slice args) {
    return fmt_sprintln(((burrow__TtExec *)env)->a, args);
}

static Str ttf_printf(void *env, Str format, Slice args) {
    return fmt_sprintf(((burrow__TtExec *)env)->a, format, args);
}

static Str ttf_html(void *env, Slice args) {
    return text_template_html_escaper(((burrow__TtExec *)env)->a, args);
}

static Str ttf_js(void *env, Slice args) {
    return text_template_js_escaper(((burrow__TtExec *)env)->a, args);
}

static Str ttf_urlquery(void *env, Slice args) {
    return text_template_url_query_escaper(((burrow__TtExec *)env)->a, args);
}

/* ---------------------------------------------------------- the builtins */

/* One execution's builtins, each a function value whose env is the
 * execution. */
typedef struct TtfBuiltins {
    burrow__TtfAndFn and_fn, or_fn, call_fn;
    burrow__TtfPrintFn html_fn, js_fn, print_fn, println_fn, urlquery_fn;
    burrow__TtfPrintfFn printf_fn;
    burrow__TtfIndexFn index_fn, slice_fn;
    burrow__TtfLenFn len_fn;
    burrow__TtfNotFn not_fn;
    burrow__TtfEqFn eq_fn;
    burrow__TtfCmpFn ge_fn, gt_fn, le_fn, lt_fn, ne_fn;
    burrow__TtfCallFn real_call_fn;
} TtfBuiltins;

typedef struct TtfBuiltin {
    const char *name;
    size_t offset;
    const Type *type;
} TtfBuiltin;

static const TtfBuiltin ttf_builtins[] = {
    {"and", offsetof(TtfBuiltins, and_fn), &burrow_type_burrow__TtfAndFn},
    {"call", offsetof(TtfBuiltins, call_fn), &burrow_type_burrow__TtfAndFn},
    {"html", offsetof(TtfBuiltins, html_fn), &burrow_type_burrow__TtfPrintFn},
    {"index", offsetof(TtfBuiltins, index_fn), &burrow_type_burrow__TtfIndexFn},
    {"slice", offsetof(TtfBuiltins, slice_fn), &burrow_type_burrow__TtfIndexFn},
    {"js", offsetof(TtfBuiltins, js_fn), &burrow_type_burrow__TtfPrintFn},
    {"len", offsetof(TtfBuiltins, len_fn), &burrow_type_burrow__TtfLenFn},
    {"not", offsetof(TtfBuiltins, not_fn), &burrow_type_burrow__TtfNotFn},
    {"or", offsetof(TtfBuiltins, or_fn), &burrow_type_burrow__TtfAndFn},
    {"print", offsetof(TtfBuiltins, print_fn), &burrow_type_burrow__TtfPrintFn},
    {"printf", offsetof(TtfBuiltins, printf_fn), &burrow_type_burrow__TtfPrintfFn},
    {"println", offsetof(TtfBuiltins, println_fn), &burrow_type_burrow__TtfPrintFn},
    {"urlquery", offsetof(TtfBuiltins, urlquery_fn), &burrow_type_burrow__TtfPrintFn},
    {"eq", offsetof(TtfBuiltins, eq_fn), &burrow_type_burrow__TtfEqFn},
    {"ge", offsetof(TtfBuiltins, ge_fn), &burrow_type_burrow__TtfCmpFn},
    {"gt", offsetof(TtfBuiltins, gt_fn), &burrow_type_burrow__TtfCmpFn},
    {"le", offsetof(TtfBuiltins, le_fn), &burrow_type_burrow__TtfCmpFn},
    {"lt", offsetof(TtfBuiltins, lt_fn), &burrow_type_burrow__TtfCmpFn},
    {"ne", offsetof(TtfBuiltins, ne_fn), &burrow_type_burrow__TtfCmpFn},
};

static TtfBuiltins *ttf_builtins_of(burrow__TtExec *x) {
    if (x->builtins != NULL)
        return (TtfBuiltins *)x->builtins;
    TtfBuiltins *b = BURROW_NEW(x->a, TtfBuiltins);
    if (b == NULL)
        burrow__tt_oom();
    b->and_fn = (burrow__TtfAndFn){ttf_unreachable, x};
    b->or_fn = (burrow__TtfAndFn){ttf_unreachable, x};
    b->call_fn = (burrow__TtfAndFn){ttf_unreachable, x};
    b->html_fn = (burrow__TtfPrintFn){ttf_html, x};
    b->js_fn = (burrow__TtfPrintFn){ttf_js, x};
    b->print_fn = (burrow__TtfPrintFn){ttf_print, x};
    b->println_fn = (burrow__TtfPrintFn){ttf_println, x};
    b->urlquery_fn = (burrow__TtfPrintFn){ttf_urlquery, x};
    b->printf_fn = (burrow__TtfPrintfFn){ttf_printf, x};
    b->index_fn = (burrow__TtfIndexFn){ttf_index, x};
    b->slice_fn = (burrow__TtfIndexFn){ttf_slice, x};
    b->len_fn = (burrow__TtfLenFn){ttf_length, x};
    b->not_fn = (burrow__TtfNotFn){ttf_not, x};
    b->eq_fn = (burrow__TtfEqFn){ttf_eq, x};
    b->ge_fn = (burrow__TtfCmpFn){ttf_ge, x};
    b->gt_fn = (burrow__TtfCmpFn){ttf_gt, x};
    b->le_fn = (burrow__TtfCmpFn){ttf_le, x};
    b->lt_fn = (burrow__TtfCmpFn){ttf_lt, x};
    b->ne_fn = (burrow__TtfCmpFn){ttf_ne, x};
    b->real_call_fn = (burrow__TtfCallFn){ttf_call, x};
    x->builtins = b;
    return b;
}

bool burrow__tt_find_builtin(burrow__TtExec *x, Str name, ReflectValue *fn) {
    for (size_t i = 0; i < sizeof ttf_builtins / sizeof ttf_builtins[0]; i++) {
        if (!str_eq(name, str_from_cstr(ttf_builtins[i].name)))
            continue;
        Byte *b = (Byte *)ttf_builtins_of(x);
        *fn = reflect_value_of(
            BURROW_ANY(ttf_builtins[i].type, b + ttf_builtins[i].offset));
        return true;
    }
    return false;
}

ReflectValue burrow__tt_call_fn(burrow__TtExec *x) {
    TtfBuiltins *b = ttf_builtins_of(x);
    return reflect_value_of(
        BURROW_ANY(&burrow_type_burrow__TtfCallFn, &b->real_call_fn));
}

/* ----------------------------------------------------------- HTML escaping */

static void ttf_write(IoWriter w, const Byte *p, Int n) {
    if (n <= 0)
        return;
    Error e = BURROW_NO_ERROR;
    io_write_string(w, (Str){p, n}, &e);
}

#define TTF_WRITE_LIT(w, s) ttf_write((w), (const Byte *)(s), (Int)sizeof(s) - 1)

void text_template_html_escape(IoWriter w, Slice b) {
    const Byte *p = (const Byte *)b.p;
    Int last = 0;
    for (Int i = 0; i < b.len; i++) {
        const char *html;
        switch (p[i]) {
        case '\0':
            html = "\xEF\xBF\xBD";
            break;
        case '"':
            html = "&#34;"; /* shorter than "&quot;" */
            break;
        case '\'':
            html =
                "&#39;"; /* shorter than "&apos;" and apos was not in HTML until HTML5 */
            break;
        case '&':
            html = "&amp;";
            break;
        case '<':
            html = "&lt;";
            break;
        case '>':
            html = "&gt;";
            break;
        default:
            continue;
        }
        ttf_write(w, p + last, i - last);
        ttf_write(w, (const Byte *)html, (Int)strlen(html));
        last = i + 1;
    }
    ttf_write(w, p + last, b.len - last);
}

Str text_template_html_escape_string(Alloc *a, Str s) {
    /* Avoid allocation if we can. */
    bool special = false;
    for (Int i = 0; i < s.len && !special; i++) {
        switch (s.p[i]) {
        case '\'':
        case '"':
        case '&':
        case '<':
        case '>':
        case '\0':
            special = true;
            break;
        default:
            break;
        }
    }
    if (!special)
        return s;
    StringsBuilder sb = STRINGS_BUILDER(a);
    text_template_html_escape(
        strings_builder_as_io_writer(&sb),
        slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_UINT8));
    return strings_builder_string(&sb);
}

Str text_template_html_escaper(Alloc *a, Slice args) {
    return text_template_html_escape_string(a, ttf_eval_args(a, args));
}

/* ----------------------------------------------------- JavaScript escaping */

static bool ttf_js_is_special(Rune r) {
    switch (r) {
    case '\\':
    case '\'':
    case '"':
    case '<':
    case '>':
    case '&':
    case '=':
        return true;
    default:
        return r < ' ' || UTF8_RUNE_SELF <= r;
    }
}

void text_template_js_escape(IoWriter w, Slice b) {
    static const char hex[] = "0123456789ABCDEF";
    const Byte *p = (const Byte *)b.p;
    Int last = 0;
    for (Int i = 0; i < b.len; i++) {
        Byte c = p[i];
        if (!ttf_js_is_special((Rune)c))
            continue; /* fast path: nothing to do */
        ttf_write(w, p + last, i - last);
        if (c < UTF8_RUNE_SELF) {
            /* Quotes, slashes and angle brackets get quoted. Control characters
             * get written as \u00XX. */
            switch (c) {
            case '\\':
                TTF_WRITE_LIT(w, "\\\\");
                break;
            case '\'':
                TTF_WRITE_LIT(w, "\\'");
                break;
            case '"':
                TTF_WRITE_LIT(w, "\\\"");
                break;
            case '<':
                TTF_WRITE_LIT(w, "\\u003C");
                break;
            case '>':
                TTF_WRITE_LIT(w, "\\u003E");
                break;
            case '&':
                TTF_WRITE_LIT(w, "\\u0026");
                break;
            case '=':
                TTF_WRITE_LIT(w, "\\u003D");
                break;
            default: {
                TTF_WRITE_LIT(w, "\\u00");
                ttf_write(w, (const Byte *)&hex[c >> 4], 1);
                ttf_write(w, (const Byte *)&hex[c & 0x0F], 1);
                break;
            }
            }
        } else {
            /* Unicode rune. */
            Int size = 0;
            Rune r = utf8_decode_rune(slice_from((void *)(uintptr_t)(p + i), b.len - i,
                                                 b.len - i, TYPE_UINT8),
                                      &size);
            if (unicode_is_print(r))
                ttf_write(w, p + i, size);
            else
                fmt_fprintf_v(w, "\\u%04X", r);
            i += size - 1;
        }
        last = i + 1;
    }
    ttf_write(w, p + last, b.len - last);
}

Str text_template_js_escape_string(Alloc *a, Str s) {
    /* Avoid allocation if we can. */
    bool special = false;
    for (Int i = 0; i < s.len && !special;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string((Str){s.p + i, s.len - i}, &size);
        special = ttf_js_is_special(r);
        i += size;
    }
    if (!special)
        return s;
    StringsBuilder sb = STRINGS_BUILDER(a);
    text_template_js_escape(
        strings_builder_as_io_writer(&sb),
        slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_UINT8));
    return strings_builder_string(&sb);
}

Str text_template_js_escaper(Alloc *a, Slice args) {
    return text_template_js_escape_string(a, ttf_eval_args(a, args));
}

Str text_template_url_query_escaper(Alloc *a, Slice args) {
    return url_query_escape(a, ttf_eval_args(a, args));
}
