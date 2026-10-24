/* Derived from Go's src/testing/quick/quick.go. See
 * include/burrow/testing/quick.h.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/quick.h"

#include "burrow/fmt.h"
#include "burrow/math.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

/* What the -quickchecks flag defaults to in Go. */
static const Int default_max_count = 100;

#define QK_TYPE(var, gonm, kind, T)                                                    \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"testing/quick", 13},                                           \
        kind,                                                                          \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

const Type burrow_type_QuickGenerator = {
    {(const Byte *)"Generator", 9},
    {(const Byte *)"testing/quick", 13},
    KIND_INTERFACE,
    (uint32_t)sizeof(QuickGenerator),
    (uint16_t)_Alignof(QuickGenerator),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* randFloat32 generates a random float taking the full range of a float32. */
static float rand_float32(MathRandRand *rand) {
    double f = math_rand_rand_float64(rand) * MATH_MAX_FLOAT32;
    if ((math_rand_rand_int(rand) & 1) == 1)
        f = -f;
    return (float)f;
}

/* randFloat64 generates a random float taking the full range of a float64. */
static double rand_float64(MathRandRand *rand) {
    double f = math_rand_rand_float64(rand) * MATH_MAX_FLOAT64;
    if ((math_rand_rand_int(rand) & 1) == 1)
        f = -f;
    return f;
}

/* randInt64 returns a random int64. */
static int64_t rand_int64(MathRandRand *rand) {
    return (int64_t)math_rand_rand_uint64(rand);
}

/* complexSize is the maximum length of arbitrary values that contain other
 * values. */
#define COMPLEX_SIZE 50

static ReflectValue sized_value(Alloc *a, ReflectType t, MathRandRand *rand, Int size,
                                bool *ok);

ReflectValue quick_value(Alloc *a, ReflectType t, MathRandRand *rand, bool *ok) {
    return sized_value(a, t, rand, COMPLEX_SIZE, ok);
}

/* sizedValue returns an arbitrary value of the given type. The size
 * hint is used for shrinking as a function of indirection level so
 * that recursive data structures will terminate. */
static ReflectValue sized_value(Alloc *a, ReflectType t, MathRandRand *rand, Int size,
                                bool *ok) {
    ReflectValue none = {0};
    QuickGenerator m = {NULL, NULL};
    if (reflect_type_assert(reflect_zero(t), TYPE_QUICK_GENERATOR, &m)) {
        *ok = true;
        return m.vt->generate(m.data, a, rand, size);
    }

    *ok = false;
    ReflectValue v = reflect_value_elem(reflect_new(a, t));
    ReflectType concrete = t;
    switch (reflect_type_kind(concrete)) {
    case KIND_BOOL:
        reflect_value_set_bool(v, (math_rand_rand_int(rand) & 1) == 0);
        break;
    case KIND_FLOAT32:
        reflect_value_set_float(v, (double)rand_float32(rand));
        break;
    case KIND_FLOAT64:
        reflect_value_set_float(v, rand_float64(rand));
        break;
    case KIND_COMPLEX64: {
        double re = (double)rand_float32(rand);
        double im = (double)rand_float32(rand);
        Complex128 c = {re, im};
        reflect_value_set_complex(v, c);
        break;
    }
    case KIND_COMPLEX128: {
        double re = rand_float64(rand);
        double im = rand_float64(rand);
        Complex128 c = {re, im};
        reflect_value_set_complex(v, c);
        break;
    }
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
    case KIND_INT8:
    case KIND_INT:
        reflect_value_set_int(v, rand_int64(rand));
        break;
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINT8:
    case KIND_UINT:
    case KIND_UINTPTR:
        reflect_value_set_uint(v, (uint64_t)rand_int64(rand));
        break;
    case KIND_MAP: {
        Int num_elems = math_rand_rand_intn(rand, size);
        reflect_value_set(v, reflect_make_map(a, concrete));
        for (Int i = 0; i < num_elems; i++) {
            bool ok1 = false, ok2 = false;
            ReflectValue key =
                sized_value(a, reflect_type_key(concrete), rand, size, &ok1);
            ReflectValue value =
                sized_value(a, reflect_type_elem(concrete), rand, size, &ok2);
            if (!ok1 || !ok2)
                return none;
            reflect_value_set_map_index(v, key, value);
        }
        break;
    }
    case KIND_POINTER:
        if (math_rand_rand_intn(rand, size) == 0) {
            reflect_value_set_zero(v); /* Generate nil pointer. */
        } else {
            bool eok = false;
            ReflectValue elem =
                sized_value(a, reflect_type_elem(concrete), rand, size, &eok);
            if (!eok)
                return none;
            reflect_value_set(v, reflect_new(a, reflect_type_elem(concrete)));
            reflect_value_set(reflect_value_elem(v), elem);
        }
        break;
    case KIND_SLICE: {
        Int num_elems = math_rand_rand_intn(rand, size);
        Int size_left = size - num_elems;
        reflect_value_set(v, reflect_make_slice(a, concrete, num_elems, num_elems));
        for (Int i = 0; i < num_elems; i++) {
            bool eok = false;
            ReflectValue elem =
                sized_value(a, reflect_type_elem(concrete), rand, size_left, &eok);
            if (!eok)
                return none;
            reflect_value_set(reflect_value_index(v, i), elem);
        }
        break;
    }
    case KIND_ARRAY:
        for (Int i = 0; i < reflect_value_len(v); i++) {
            bool eok = false;
            ReflectValue elem =
                sized_value(a, reflect_type_elem(concrete), rand, size, &eok);
            if (!eok)
                return none;
            reflect_value_set(reflect_value_index(v, i), elem);
        }
        break;
    case KIND_STRING: {
        Int num_chars = math_rand_rand_intn(rand, COMPLEX_SIZE);
        /* Not in Go: the string is encoded as the runes are drawn, rather
         * than made from a []rune afterwards, which comes to the same bytes. */
        Byte *p =
            (Byte *)mem_alloc_nozero(a, (size_t)(num_chars * UTF8_UTF_MAX) + 1, 1);
        if (p == NULL)
            panic_str(BURROW_S("testing/quick: out of memory")); /* Not in Go */
        Int n = 0;
        for (Int i = 0; i < num_chars; i++) {
            Rune r = (Rune)math_rand_rand_intn(rand, 0x10ffff);
            n += utf8_encode_rune(
                slice_from(p + n, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
        }
        reflect_value_set_string(v, str_from_bytes(p, n));
        break;
    }
    case KIND_STRUCT: {
        Int n = reflect_value_num_field(v);
        /* Divide sizeLeft evenly among the struct fields. */
        Int size_left = size;
        if (n > size_left) {
            size_left = 1;
        } else if (n > 0) {
            size_left /= n;
        }
        for (Int i = 0; i < n; i++) {
            bool eok = false;
            ReflectValue elem = sized_value(a, reflect_type_field(concrete, i).type,
                                            rand, size_left, &eok);
            if (!eok)
                return none;
            reflect_value_set(reflect_value_field(v, i), elem);
        }
        break;
    }
    case KIND_INVALID:
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_INTERFACE:
    case KIND_UNSAFE_POINTER:
    case KIND_MAX:
    default:
        return none;
    }

    *ok = true;
    return v;
}

/* --------------------------------------------------------------- the errors */

QK_TYPE(qk_setup_desc, "SetupError", KIND_STRING, Str);
QK_TYPE(qk_check_desc, "CheckError", KIND_STRUCT, QuickCheckError);
QK_TYPE(qk_check_equal_desc, "CheckEqualError", KIND_STRUCT, QuickCheckEqualError);

const Type *const TYPE_QUICK_SETUP_ERROR = &qk_setup_desc;
const Type *const TYPE_QUICK_CHECK_ERROR = &qk_check_desc;
const Type *const TYPE_QUICK_CHECK_EQUAL_ERROR = &qk_check_equal_desc;

static Str qk_setup_message(const void *self) {
    return *(const QuickSetupError *)self;
}

static bool qk_setup_is(const void *self, Error target);
static Error qk_setup_clone(const void *self, Alloc *a);

static const ErrorVT qk_setup_vt = {
    &qk_setup_desc, qk_setup_message, NULL, NULL, qk_setup_is, NULL, qk_setup_clone,
};

/* Go compares two of them with ==, which for a string is the text. */
static bool qk_setup_is(const void *self, Error target) {
    if (target.vt != &qk_setup_vt || target.data == NULL)
        return false;
    return str_eq(*(const QuickSetupError *)self,
                  *(const QuickSetupError *)target.data);
}

static Error qk_setup_clone(const void *self, Alloc *a) {
    return quick_setup_error_as_error(*(const QuickSetupError *)self, a);
}

Str quick_setup_error_error(QuickSetupError s) {
    return s;
}

Error quick_setup_error_as_error(QuickSetupError s, Alloc *a) {
    QuickSetupError *b = (QuickSetupError *)mem_alloc_nozero(
        a, sizeof(QuickSetupError) + (size_t)s.len, _Alignof(QuickSetupError));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    *b = str_from_bytes(p, s.len);
    return (Error){&qk_setup_vt, b};
}

static const QuickSetupError qk_not_function =
    BURROW_S_INIT("argument is not a function");
static const QuickSetupError qk_not_one_value =
    BURROW_S_INIT("function does not return one value");
static const QuickSetupError qk_not_bool =
    BURROW_S_INIT("function does not return a bool");
static const QuickSetupError qk_f_not_function = BURROW_S_INIT("f is not a function");
static const QuickSetupError qk_g_not_function = BURROW_S_INIT("g is not a function");
static const QuickSetupError qk_different_types =
    BURROW_S_INIT("functions have different types");

static Error qk_setup(const QuickSetupError *s) {
    Error e = {&qk_setup_vt, s};
    return e;
}

/* toString: the %#v of each, joined by ", ". */
static Str to_string(Alloc *a, Slice interfaces) {
    Slice s = slice_make(a, TYPE_OF(Str), interfaces.len, interfaces.len);
    if (slice_is_nil(s))
        return BURROW_S("");
    for (Int i = 0; i < interfaces.len; i++) {
        Any v = BURROW_AT(Any, interfaces, i);
        BURROW_AT(Str, s, i) = fmt_sprintf_v(a, "%#v", v);
    }
    return strings_join(a, s, BURROW_S(", "));
}

typedef struct QkCheckBox {
    QuickCheckError e;
    Str message;
} QkCheckBox;

typedef struct QkCheckEqualBox {
    QuickCheckEqualError e;
    Str message;
} QkCheckEqualBox;

static Str qk_check_message(const void *self) {
    return ((const QkCheckBox *)self)->message;
}

static Str qk_check_equal_message(const void *self) {
    return ((const QkCheckEqualBox *)self)->message;
}

static const ErrorVT qk_check_vt = {
    &qk_check_desc, qk_check_message, NULL, NULL, NULL, NULL, NULL,
};

static const ErrorVT qk_check_equal_vt = {
    &qk_check_equal_desc, qk_check_equal_message, NULL, NULL, NULL, NULL, NULL,
};

Str quick_check_error_error(const QuickCheckError *s, Alloc *a) {
    Str in = to_string(a, s->in);
    return fmt_sprintf_v(a, "#%d: failed on input %s", s->count, in);
}

Error quick_check_error_as_error(const QuickCheckError *s, Alloc *a) {
    QkCheckBox *b = (QkCheckBox *)mem_alloc(a, sizeof *b, _Alignof(QkCheckBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = *s;
    b->message = quick_check_error_error(s, a);
    return (Error){&qk_check_vt, b};
}

Str quick_check_equal_error_error(const QuickCheckEqualError *s, Alloc *a) {
    Str in = to_string(a, s->check_error.in);
    Str out1 = to_string(a, s->out1);
    Str out2 = to_string(a, s->out2);
    return fmt_sprintf_v(a, "#%d: failed on input %s. Output 1: %s. Output 2: %s",
                         s->check_error.count, in, out1, out2);
}

Error quick_check_equal_error_as_error(const QuickCheckEqualError *s, Alloc *a) {
    QkCheckEqualBox *b =
        (QkCheckEqualBox *)mem_alloc(a, sizeof *b, _Alignof(QkCheckEqualBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = *s;
    b->message = quick_check_equal_error_error(s, a);
    return (Error){&qk_check_equal_vt, b};
}

/* ------------------------------------------------------------------ Config */

static const QuickConfig default_config;

/* getRand returns the *rand.Rand to use for a given Config. NULL when a runs
 * out. */
static MathRandRand *get_rand(const QuickConfig *c, Alloc *a) {
    if (c->rand == NULL)
        return math_rand_new(a, math_rand_new_source(a, time_unix_nano(time_now())));
    return c->rand;
}

/* getMaxCount returns the maximum number of iterations to run for a given
 * Config. */
static Int get_max_count(const QuickConfig *c) {
    Int max_count = c->max_count;
    if (max_count == 0) {
        if (c->max_count_scale != 0) {
            max_count = (Int)(c->max_count_scale * (double)default_max_count);
        } else {
            max_count = default_max_count;
        }
    }
    return max_count;
}

/* ------------------------------------------------------------------- Check */

/* arbitraryValues writes Values to args such that args contains Values
 * suitable for calling f. */
static Error arbitrary_values(Alloc *a, Slice args, ReflectType f,
                              const QuickConfig *config, MathRandRand *rand) {
    if (config->values.f != NULL) {
        config->values.f(config->values.env, args, rand);
        return BURROW_NO_ERROR;
    }

    for (Int j = 0; j < args.len; j++) {
        bool ok = false;
        BURROW_AT(ReflectValue, args, j) =
            quick_value(a, reflect_type_in(f, j), rand, &ok);
        if (!ok) {
            Str ts = reflect_type_string(reflect_type_in(f, j));
            Str msg = fmt_sprintf_v(
                a, "cannot create arbitrary value of type %s for argument %d", ts, j);
            return quick_setup_error_as_error(msg, a);
        }
    }

    return BURROW_NO_ERROR;
}

static bool function_and_type(Any f, ReflectValue *v, ReflectType *t) {
    *v = reflect_value_of(f);
    bool ok = reflect_value_kind(*v) == KIND_FUNC;
    if (!ok)
        return ok;
    *t = reflect_value_type(*v);
    return ok;
}

/* toInterfaces, in *ret. False when a runs out. */
static bool to_interfaces(Alloc *a, Slice values, Slice *ret) {
    *ret = slice_make(a, TYPE_ANY, values.len, values.len);
    if (slice_is_nil(*ret))
        return false;
    for (Int i = 0; i < values.len; i++)
        BURROW_AT(Any, *ret, i) =
            reflect_value_interface(BURROW_AT(ReflectValue, values, i));
    return true;
}

Error quick_check(Alloc *a, Any f, const QuickConfig *config) {
    if (config == NULL)
        config = &default_config;

    ReflectValue f_val = {0};
    ReflectType f_type = NULL;
    if (!function_and_type(f, &f_val, &f_type))
        return qk_setup(&qk_not_function);

    if (reflect_type_num_out(f_type) != 1)
        return qk_setup(&qk_not_one_value);
    if (reflect_type_kind(reflect_type_out(f_type, 0)) != KIND_BOOL)
        return qk_setup(&qk_not_bool);

    Int n = reflect_type_num_in(f_type);
    Slice arguments = slice_make(a, TYPE_REFLECT_VALUE, n, n);
    MathRandRand *rand = get_rand(config, a);
    if (slice_is_nil(arguments) || rand == NULL)
        return burrow_err_out_of_memory;
    Int max_count = get_max_count(config);

    for (Int i = 0; i < max_count; i++) {
        Error err = arbitrary_values(a, arguments, f_type, config, rand);
        if (BURROW_FAILED(err))
            return err;

        Slice out = reflect_value_call(f_val, a, arguments);
        if (!reflect_value_bool(BURROW_AT(ReflectValue, out, 0))) {
            QuickCheckError e = {i + 1, {0}};
            if (!to_interfaces(a, arguments, &e.in))
                return burrow_err_out_of_memory;
            return quick_check_error_as_error(&e, a);
        }
    }

    return BURROW_NO_ERROR;
}

Error quick_check_equal(Alloc *a, Any f, Any g, const QuickConfig *config) {
    if (config == NULL)
        config = &default_config;

    ReflectValue x = {0}, y = {0};
    ReflectType x_type = NULL, y_type = NULL;
    if (!function_and_type(f, &x, &x_type))
        return qk_setup(&qk_f_not_function);
    if (!function_and_type(g, &y, &y_type))
        return qk_setup(&qk_g_not_function);

    if (x_type != y_type)
        return qk_setup(&qk_different_types);

    Int n = reflect_type_num_in(x_type);
    Slice arguments = slice_make(a, TYPE_REFLECT_VALUE, n, n);
    MathRandRand *rand = get_rand(config, a);
    if (slice_is_nil(arguments) || rand == NULL)
        return burrow_err_out_of_memory;
    Int max_count = get_max_count(config);
    ReflectType anys = reflect_slice_of(TYPE_ANY);

    for (Int i = 0; i < max_count; i++) {
        Error err = arbitrary_values(a, arguments, x_type, config, rand);
        if (BURROW_FAILED(err))
            return err;

        QuickCheckEqualError e = {{i + 1, {0}}, {0}, {0}};
        if (!to_interfaces(a, reflect_value_call(x, a, arguments), &e.out1) ||
            !to_interfaces(a, reflect_value_call(y, a, arguments), &e.out2))
            return burrow_err_out_of_memory;

        if (!reflect_deep_equal(BURROW_ANY(anys, &e.out1), BURROW_ANY(anys, &e.out2))) {
            if (!to_interfaces(a, arguments, &e.check_error.in))
                return burrow_err_out_of_memory;
            return quick_check_equal_error_as_error(&e, a);
        }
    }

    return BURROW_NO_ERROR;
}
