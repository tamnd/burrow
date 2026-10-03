/* Tests for flag.
 *
 * Go's flag_test.go, test for test and in the same order. The ones that
 * assign os.Args parse the same arguments through flag_command_line instead,
 * and TestExitCode starts this binary again, as Go's does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/error.h"
#include "burrow/flag.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/netip.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(s) BURROW_S(s)

static Slice args_of(Str *a, Int n) {
    return slice_from(a, n, n, TYPE_STRING);
}

#define ARGS(...)                                                                      \
    args_of((Str[]){__VA_ARGS__}, (Int)(sizeof((Str[]){__VA_ARGS__}) / sizeof(Str)))

/* A builder in an arena, which a test lets go of in one call. */
typedef struct Buf {
    Arena ar;
    StringsBuilder b;
} Buf;

static void buf_init(Buf *b) {
    arena_init(&b->ar, NULL, 4096);
    b->b = STRINGS_BUILDER(arena_allocator(&b->ar));
}

static IoWriter buf_writer(Buf *b) {
    return strings_builder_as_io_writer(&b->b);
}

static Str buf_string(Buf *b) {
    return strings_builder_string(&b->b);
}

static void buf_free(Buf *b) {
    arena_free(&b->ar);
}

/* Go's ResetForTesting. The sets it makes are freed by the next one, and the
 * last lives until the program ends, as flag_command_line's own set does. */
static FlagFlagSet *testing_set;
static Func default_usage;
static bool have_default_usage;

static void command_line_usage(void *env) {
    (void)env;
    if (flag_usage.f != NULL)
        flag_usage.f(flag_usage.env);
}

static void reset_for_testing(Func usage) {
    if (!have_default_usage) {
        default_usage = flag_usage;
        have_default_usage = true;
    }
    Str name = flag_flag_set_name(flag_command_line);
    FlagFlagSet *f = flag_new_flag_set(NULL, name, FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(f, io_discard);
    f->usage = BURROW_FN(Func, command_line_usage, NULL);
    flag_command_line = f;
    flag_usage = usage;
    if (testing_set != NULL)
        flag_flag_set_free(testing_set);
    testing_set = f;
}

static const Func no_usage = {NULL, NULL};

/* ---------------------------------------------------------- TestEverything */

static Error nop_func(void *env, Str s) {
    (void)env;
    (void)s;
    return BURROW_NO_ERROR;
}

typedef struct Visited {
    TestingT *t;
    const char *desired;
    Int n;
    Str names[16];
} Visited;

static void visitor(void *env, Flag *f) {
    Visited *v = env;
    TestingT *t = v->t;
    if (f->name.len <= 5 || memcmp(f->name.p, "test_", 5) != 0)
        return;
    if (v->n < 16)
        v->names[v->n] = f->name;
    v->n++;
    Arena ar;
    arena_init(&ar, NULL, 256);
    Alloc *a = arena_allocator(&ar);
    Str got = f->value.vt->string(f->value.data, a);
    Str desired = str_from_cstr(v->desired);
    Str bool_desired = strcmp(v->desired, "0") == 0 ? S("false") : S("true");
    bool ok = str_eq(got, desired) ||
              (str_eq(f->name, S("test_bool")) && str_eq(got, bool_desired)) ||
              (str_eq(f->name, S("test_duration")) &&
               str_eq(got, fmt_sprintf_v(a, "%ss", desired))) ||
              (str_eq(f->name, S("test_func")) && got.len == 0) ||
              (str_eq(f->name, S("test_boolfunc")) && got.len == 0);
    if (!ok)
        testing_t_errorf_v(t, "Visit: bad value %s for %s", got, f->name);
    arena_free(&ar);
}

static void TestEverything(TestingT *t) {
    reset_for_testing(no_usage);
    flag_bool(S("test_bool"), false, S("bool value"));
    flag_int(S("test_int"), 0, S("int value"));
    flag_int64(S("test_int64"), 0, S("int64 value"));
    flag_uint(S("test_uint"), 0, S("uint value"));
    flag_uint64(S("test_uint64"), 0, S("uint64 value"));
    flag_string(S("test_string"), S("0"), S("string value"));
    flag_float64(S("test_float64"), 0, S("float64 value"));
    flag_duration(S("test_duration"), 0, S("time.Duration value"));
    flag_func(S("test_func"), S("func value"), BURROW_FN(FlagFunc, nop_func, NULL));
    flag_bool_func(S("test_boolfunc"), S("func"), BURROW_FN(FlagFunc, nop_func, NULL));

    Visited v = {t, "0", 0, {{0}}};
    flag_visit_all(BURROW_FN(FlagVisitFunc, visitor, &v));
    if (v.n != 10)
        testing_t_errorf_v(t, "VisitAll misses some flags: saw %d", v.n);
    v.n = 0;
    flag_visit(BURROW_FN(FlagVisitFunc, visitor, &v));
    if (v.n != 0)
        testing_t_errorf_v(t, "Visit sees unset flags: saw %d", v.n);

    /* Now set all flags */
    flag_set(S("test_bool"), S("true"));
    flag_set(S("test_int"), S("1"));
    flag_set(S("test_int64"), S("1"));
    flag_set(S("test_uint"), S("1"));
    flag_set(S("test_uint64"), S("1"));
    flag_set(S("test_string"), S("1"));
    flag_set(S("test_float64"), S("1"));
    flag_set(S("test_duration"), S("1s"));
    flag_set(S("test_func"), S("1"));
    flag_set(S("test_boolfunc"), S(""));
    v.desired = "1";
    v.n = 0;
    flag_visit(BURROW_FN(FlagVisitFunc, visitor, &v));
    if (v.n != 10)
        testing_t_errorf_v(t, "Visit fails after set: saw %d", v.n);
    /* Now test they're visited in sort order. */
    for (Int i = 1; i < v.n && i < 16; i++)
        if (str_cmp(v.names[i - 1], v.names[i]) >= 0)
            testing_t_errorf_v(t, "flag names not sorted: %s before %s", v.names[i - 1],
                               v.names[i]);
}

/* ------------------------------------------------------------------ TestGet */

static void get_visitor(void *env, Flag *f) {
    TestingT *t = env;
    if (f->name.len <= 5 || memcmp(f->name.p, "test_", 5) != 0)
        return;
    if (f->value.vt->get == NULL) {
        testing_t_errorf_v(t, "Visit: value does not satisfy Getter: %s", f->name);
        return;
    }
    Any g = f->value.vt->get(f->value.data);
    bool ok = false;
    if (str_eq(f->name, S("test_bool")))
        ok = g.t == TYPE_BOOL && *(bool *)g.data == true;
    else if (str_eq(f->name, S("test_int")))
        ok = g.t == TYPE_INT && *(Int *)g.data == 1;
    else if (str_eq(f->name, S("test_int64")))
        ok = g.t == TYPE_INT64 && *(int64_t *)g.data == 2;
    else if (str_eq(f->name, S("test_uint")))
        ok = g.t == TYPE_UINT && *(Uint *)g.data == 3;
    else if (str_eq(f->name, S("test_uint64")))
        ok = g.t == TYPE_UINT64 && *(uint64_t *)g.data == 4;
    else if (str_eq(f->name, S("test_string")))
        ok = g.t == TYPE_STRING && str_eq(*(Str *)g.data, S("5"));
    else if (str_eq(f->name, S("test_float64")))
        ok = g.t == TYPE_FLOAT64 && *(double *)g.data == 6;
    else if (str_eq(f->name, S("test_duration")))
        ok = g.t == TYPE_DURATION && *(Duration *)g.data == 7;
    if (!ok)
        testing_t_errorf_v(t, "Visit: bad value %v for %s", g, f->name);
}

static void TestGet(TestingT *t) {
    reset_for_testing(no_usage);
    flag_bool(S("test_bool"), true, S("bool value"));
    flag_int(S("test_int"), 1, S("int value"));
    flag_int64(S("test_int64"), 2, S("int64 value"));
    flag_uint(S("test_uint"), 3, S("uint value"));
    flag_uint64(S("test_uint64"), 4, S("uint64 value"));
    flag_string(S("test_string"), S("5"), S("string value"));
    flag_float64(S("test_float64"), 6, S("float64 value"));
    flag_duration(S("test_duration"), 7, S("time.Duration value"));
    flag_visit_all(BURROW_FN(FlagVisitFunc, get_visitor, t));
}

/* ---------------------------------------------------------------- TestUsage */

static void set_true(void *env) {
    *(bool *)env = true;
}

static void TestUsage(TestingT *t) {
    bool called = false;
    reset_for_testing(BURROW_FN(Func, set_true, &called));
    if (BURROW_OK(flag_flag_set_parse(flag_command_line, ARGS(S("-x")))))
        testing_t_error_v(t, "parse did not fail for unknown flag");
    if (!called)
        testing_t_error_v(t, "did not call Usage for unknown flag");
}

static void test_parse(FlagFlagSet *f, TestingT *t) {
    if (flag_flag_set_parsed(f))
        testing_t_error_v(t, "f.Parse() = true before Parse");
    bool *bool_flag = flag_flag_set_bool(f, S("bool"), false, S("bool value"));
    bool *bool2_flag = flag_flag_set_bool(f, S("bool2"), false, S("bool2 value"));
    Int *int_flag = flag_flag_set_int(f, S("int"), 0, S("int value"));
    int64_t *int64_flag = flag_flag_set_int64(f, S("int64"), 0, S("int64 value"));
    Uint *uint_flag = flag_flag_set_uint(f, S("uint"), 0, S("uint value"));
    uint64_t *uint64_flag = flag_flag_set_uint64(f, S("uint64"), 0, S("uint64 value"));
    Str *string_flag = flag_flag_set_string(f, S("string"), S("0"), S("string value"));
    double *float64_flag =
        flag_flag_set_float64(f, S("float64"), 0, S("float64 value"));
    Duration *duration_flag = flag_flag_set_duration(f, S("duration"), 5 * TIME_SECOND,
                                                     S("time.Duration value"));
    Str extra = S("one-extra-argument");
    Slice args =
        ARGS(S("-bool"), S("-bool2=true"), S("--int"), S("22"), S("--int64"), S("0x23"),
             S("-uint"), S("24"), S("--uint64"), S("25"), S("-string"), S("hello"),
             S("-float64"), S("2718e28"), S("-duration"), S("2m"), extra);
    Error err = flag_flag_set_parse(f, args);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, err);
    if (!flag_flag_set_parsed(f))
        testing_t_error_v(t, "f.Parse() = false after Parse");
    if (*bool_flag != true)
        testing_t_error_v(t, "bool flag should be true, is ", *bool_flag);
    if (*bool2_flag != true)
        testing_t_error_v(t, "bool2 flag should be true, is ", *bool2_flag);
    if (*int_flag != 22)
        testing_t_error_v(t, "int flag should be 22, is ", *int_flag);
    if (*int64_flag != 0x23)
        testing_t_error_v(t, "int64 flag should be 0x23, is ", *int64_flag);
    if (*uint_flag != 24)
        testing_t_error_v(t, "uint flag should be 24, is ", *uint_flag);
    if (*uint64_flag != 25)
        testing_t_error_v(t, "uint64 flag should be 25, is ", *uint64_flag);
    if (!str_eq(*string_flag, S("hello")))
        testing_t_error_v(t, "string flag should be `hello`, is ", *string_flag);
    if (*float64_flag != 2718e28)
        testing_t_error_v(t, "float64 flag should be 2718e28, is ", *float64_flag);
    if (*duration_flag != 2 * TIME_MINUTE)
        testing_t_error_v(t, "duration flag should be 2m, is ", *duration_flag);
    Slice rest = flag_flag_set_args(f);
    if (rest.len != 1)
        testing_t_error_v(t, "expected one argument, got", rest.len);
    else if (!str_eq(((Str *)rest.p)[0], extra))
        testing_t_errorf_v(t, "expected argument %q got %q", extra, ((Str *)rest.p)[0]);
}

static void bad_parse(void *env) {
    testing_t_error_v((TestingT *)env, "bad parse");
}

static void TestParse(TestingT *t) {
    reset_for_testing(BURROW_FN(Func, bad_parse, t));
    test_parse(flag_command_line, t);
}

static void TestFlagSetParse(TestingT *t) {
    FlagFlagSet *f = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    test_parse(f, t);
    flag_flag_set_free(f);
}

/* ------------------------------------------------------ user-defined values */

/* Go's flagVar, a []string that Set appends to. */
typedef struct FlagVar {
    Str v[8];
    Int n;
} FlagVar;

static Str flag_var_text(const FlagVar *f, Alloc *a) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < f->n; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ' ');
        strings_builder_write_string(&b, f->v[i], NULL);
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static Str flag_var_string(void *self, Alloc *a) {
    return flag_var_text(self, a);
}

static Error flag_var_set(void *self, Str s) {
    FlagVar *f = self;
    if (f->n < 8)
        f->v[f->n++] = s;
    return BURROW_NO_ERROR;
}

#define TEST_TYPE(var, cname, T, hash)                                                 \
    static const Type var = {                                                          \
        {(const Byte *)(cname), (Int)sizeof(cname) - 1},                               \
        {(const Byte *)"flag_test", 9},                                                \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (hash),                                                                        \
        NULL,                                                                          \
    }

TEST_TYPE(flag_var_type, "flagVar", FlagVar, 0x66747631U);

static const FlagValueVT flag_var_vt = {&flag_var_type, flag_var_string, flag_var_set,
                                        NULL, NULL};

static void TestUserDefined(TestingT *t) {
    FlagFlagSet flags = {0};
    flag_flag_set_init(&flags, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(&flags, io_discard);
    FlagVar v = {0};
    flag_flag_set_var(&flags, (FlagValue){&flag_var_vt, &v}, S("v"), S("usage"));
    Error err =
        flag_flag_set_parse(&flags, ARGS(S("-v"), S("1"), S("-v"), S("2"), S("-v=3")));
    if (!BURROW_OK(err))
        testing_t_error_v(t, err);
    if (v.n != 3)
        testing_t_fatal_v(t, "expected 3 args; got ", v.n);
    Arena ar;
    arena_init(&ar, NULL, 256);
    Str got = flag_var_text(&v, arena_allocator(&ar));
    if (!str_eq(got, S("[1 2 3]")))
        testing_t_errorf_v(t, "expected value %q got %q", S("[1 2 3]"), got);
    arena_free(&ar);
    flag_flag_set_destroy(&flags);
}

static Error append_func(void *env, Str s) {
    return flag_var_set(env, s);
}

static Error test_error_func(void *env, Str s) {
    (void)env;
    (void)s;
    return fmt_errorf_v("test error");
}

static void TestUserDefinedFunc(TestingT *t) {
    FlagFlagSet *flags = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(flags, io_discard);
    FlagVar ss = {0};
    flag_flag_set_func(flags, S("v"), S("usage"),
                       BURROW_FN(FlagFunc, append_func, &ss));
    Error err =
        flag_flag_set_parse(flags, ARGS(S("-v"), S("1"), S("-v"), S("2"), S("-v=3")));
    if (!BURROW_OK(err))
        testing_t_error_v(t, err);
    if (ss.n != 3)
        testing_t_fatal_v(t, "expected 3 args; got ", ss.n);
    Buf buf;
    buf_init(&buf);
    Str got = flag_var_text(&ss, arena_allocator(&buf.ar));
    if (!str_eq(got, S("[1 2 3]")))
        testing_t_errorf_v(t, "expected value %q got %q", S("[1 2 3]"), got);
    /* test usage */
    flag_flag_set_set_output(flags, buf_writer(&buf));
    (void)flag_flag_set_parse(flags, ARGS(S("-h")));
    Str usage = buf_string(&buf);
    if (!strings_contains(usage, S("usage")))
        testing_t_errorf_v(t, "usage string not included: %q", usage);
    buf_free(&buf);
    flag_flag_set_free(flags);

    /* test Func error */
    flags = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(flags, io_discard);
    flag_flag_set_func(flags, S("v"), S("usage"),
                       BURROW_FN(FlagFunc, test_error_func, NULL));
    /* flag not set, so no error */
    err = flag_flag_set_parse(flags, (Slice){NULL, 0, 0, TYPE_STRING});
    if (!BURROW_OK(err))
        testing_t_error_v(t, err);
    /* flag set, expect error */
    err = flag_flag_set_parse(flags, ARGS(S("-v"), S("1")));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error; got none");
    else if (!strings_contains(error_text(err), S("test error")))
        testing_t_errorf_v(t, "error should contain \"test error\"; got %q",
                           error_text(err));
    flag_flag_set_free(flags);
}

static void set_help(void *env) {
    *(Str *)env = S("HELP");
}

static void TestUserDefinedForCommandLine(TestingT *t) {
    Str result = BURROW_STR_EMPTY;
    reset_for_testing(BURROW_FN(Func, set_help, &result));
    flag_usage.f(flag_usage.env);
    if (!str_eq(result, S("HELP")))
        testing_t_fatalf_v(t, "got %q; expected %q", result, S("HELP"));
}

/* Go's boolFlagVar, a bool flag until it has been set four times. */
typedef struct BoolFlagVar {
    Int count;
} BoolFlagVar;

static Str bool_flag_var_string(void *self, Alloc *a) {
    return fmt_sprintf_v(a, "%d", ((BoolFlagVar *)self)->count);
}

static Error bool_flag_var_set(void *self, Str value) {
    if (str_eq(value, S("true")))
        ((BoolFlagVar *)self)->count++;
    return BURROW_NO_ERROR;
}

static bool bool_flag_var_is_bool_flag(void *self) {
    return ((BoolFlagVar *)self)->count < 4;
}

TEST_TYPE(bool_flag_var_type, "boolFlagVar", BoolFlagVar, 0x66747632U);

static const FlagValueVT bool_flag_var_vt = {&bool_flag_var_type, bool_flag_var_string,
                                             bool_flag_var_set, NULL,
                                             bool_flag_var_is_bool_flag};

static void TestUserDefinedBool(TestingT *t) {
    FlagFlagSet flags = {0};
    flag_flag_set_init(&flags, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(&flags, io_discard);
    BoolFlagVar b = {0};
    flag_flag_set_var(&flags, (FlagValue){&bool_flag_var_vt, &b}, S("b"), S("usage"));
    Error err =
        flag_flag_set_parse(&flags, ARGS(S("-b"), S("-b"), S("-b"), S("-b=true"),
                                         S("-b=false"), S("-b"), S("barg"), S("-b")));
    if (!BURROW_OK(err) && b.count < 4)
        testing_t_error_v(t, err);
    if (b.count != 4)
        testing_t_errorf_v(t, "want: %d; got: %d", 4, b.count);
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error; got none");
    flag_flag_set_destroy(&flags);
}

static void TestUserDefinedBoolUsage(TestingT *t) {
    FlagFlagSet flags = {0};
    flag_flag_set_init(&flags, S("test"), FLAG_CONTINUE_ON_ERROR);
    Buf buf;
    buf_init(&buf);
    flag_flag_set_set_output(&flags, buf_writer(&buf));
    BoolFlagVar b = {0};
    flag_flag_set_var(&flags, (FlagValue){&bool_flag_var_vt, &b}, S("b"), S("X"));
    b.count = 0;
    /* b.IsBoolFlag() will return true and usage will look boolean. */
    flag_flag_set_print_defaults(&flags);
    Str got = buf_string(&buf);
    Str want = S("  -b\tX\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "false: want %q; got %q", want, got);
    b.count = 4;
    /* b.IsBoolFlag() will return false and usage will look non-boolean. */
    flag_flag_set_print_defaults(&flags);
    got = buf_string(&buf);
    want = S("  -b\tX\n  -b value\n    \tX\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "false: want %q; got %q", want, got);
    buf_free(&buf);
    flag_flag_set_destroy(&flags);
}

static void TestSetOutput(TestingT *t) {
    FlagFlagSet flags = {0};
    Buf buf;
    buf_init(&buf);
    flag_flag_set_set_output(&flags, buf_writer(&buf));
    flag_flag_set_init(&flags, S("test"), FLAG_CONTINUE_ON_ERROR);
    (void)flag_flag_set_parse(&flags, ARGS(S("-unknown")));
    Str out = buf_string(&buf);
    if (!strings_contains(out, S("-unknown")))
        testing_t_logf_v(t, "expected output mentioning unknown; got %q", out);
    buf_free(&buf);
    flag_flag_set_destroy(&flags);
}

/* This tests that one can reset the flags. Go's assigns os.Args and calls
 * Parse, which parses os.Args[1:], and here the same arguments go to
 * flag_command_line directly. */
static void TestChangingArgs(TestingT *t) {
    reset_for_testing(BURROW_FN(Func, bad_parse, t));
    Slice args = ARGS(S("cmd"), S("-before"), S("subcmd"), S("-after"), S("args"));
    bool *before = flag_bool(S("before"), false, S(""));
    Error err = flag_flag_set_parse(flag_command_line, slice_sub(args, 1, args.len));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, err);
    Str cmd = flag_arg(0);
    Slice os_args = flag_args();
    bool *after = flag_bool(S("after"), false, S(""));
    err = flag_flag_set_parse(flag_command_line, slice_sub(os_args, 1, os_args.len));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, err);
    Slice rest = flag_args();

    if (!*before || !str_eq(cmd, S("subcmd")) || !*after || rest.len != 1 ||
        !str_eq(((Str *)rest.p)[0], S("args")))
        testing_t_fatalf_v(t, "expected true subcmd true [args] got %v %s %v %d",
                           *before, cmd, *after, rest.len);
}

/* Test that -help invokes the usage message and returns ErrHelp. */
static void TestHelp(TestingT *t) {
    bool help_called = false;
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("help test"), FLAG_CONTINUE_ON_ERROR);
    fs->usage = BURROW_FN(Func, set_true, &help_called);
    bool flag;
    flag_flag_set_bool_var(fs, &flag, S("flag"), false, S("regular flag"));
    /* Regular flag invocation should work */
    Error err = flag_flag_set_parse(fs, ARGS(S("-flag=true")));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, "expected no error; got ", err);
    if (!flag)
        testing_t_error_v(t, "flag was not set by -flag");
    if (help_called) {
        testing_t_error_v(t, "help called for regular flag");
        help_called = false; /* reset for next test */
    }
    /* Help flag should work as expected. */
    err = flag_flag_set_parse(fs, ARGS(S("-help")));
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "error expected");
    if (!errors_is(err, flag_err_help) || err.data != flag_err_help.data)
        testing_t_fatal_v(t, "expected ErrHelp; got ", err);
    if (!help_called)
        testing_t_fatal_v(t, "help was not called");
    /* If we define a help flag, that should override. */
    bool help;
    flag_flag_set_bool_var(fs, &help, S("help"), false, S("help flag"));
    help_called = false;
    err = flag_flag_set_parse(fs, ARGS(S("-help")));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, "expected no error for defined -help; got ", err);
    if (help_called)
        testing_t_fatal_v(
            t, "help was called; should not have been for defined help flag");
    flag_flag_set_free(fs);
}

/* Go's zeroPanicker, a flag.Value whose String method panics if its
 * dontPanic field is false. */
typedef struct ZeroPanicker {
    bool dont_panic;
    Str v;
} ZeroPanicker;

static Error zero_panicker_set(void *self, Str s) {
    ((ZeroPanicker *)self)->v = s;
    return BURROW_NO_ERROR;
}

static Str zero_panicker_string(void *self, Alloc *a) {
    (void)a;
    ZeroPanicker *f = self;
    if (!f->dont_panic)
        panic_str(S("panic!"));
    return f->v;
}

TEST_TYPE(zero_panicker_type, "zeroPanicker", ZeroPanicker, 0x66747633U);

static const FlagValueVT zero_panicker_vt = {&zero_panicker_type, zero_panicker_string,
                                             zero_panicker_set, NULL, NULL};

static const char default_output[] =
    "  -A\tfor bootstrapping, allow 'any' type\n"
    "  -Alongflagname\n"
    "    \tdisable bounds checking\n"
    "  -C\ta boolean defaulting to true (default true)\n"
    "  -D path\n"
    "    \tset relative path for local imports\n"
    "  -E string\n"
    "    \tissue 23543 (default \"0\")\n"
    "  -F number\n"
    "    \ta non-zero number (default 2.7)\n"
    "  -G float\n"
    "    \ta float that defaults to zero\n"
    "  -M string\n"
    "    \ta multiline\n"
    "    \thelp\n"
    "    \tstring\n"
    "  -N int\n"
    "    \ta non-zero int (default 27)\n"
    "  -O\ta flag\n"
    "    \tmultiline help string (default true)\n"
    "  -V list\n"
    "    \ta list of strings (default [a b])\n"
    "  -Z int\n"
    "    \tan int that defaults to zero\n"
    "  -ZP0 value\n"
    "    \ta flag whose String method panics when it is zero\n"
    "  -ZP1 value\n"
    "    \ta flag whose String method panics when it is zero\n"
    "  -maxT timeout\n"
    "    \tset timeout for dial\n"
    "\n"
    "panic calling String method on zero flag_test.zeroPanicker for flag ZP0: panic!\n"
    "panic calling String method on zero flag_test.zeroPanicker for flag ZP1: panic!\n";

static void TestPrintDefaults(TestingT *t) {
    FlagFlagSet *fs =
        flag_new_flag_set(NULL, S("print defaults test"), FLAG_CONTINUE_ON_ERROR);
    Buf buf;
    buf_init(&buf);
    flag_flag_set_set_output(fs, buf_writer(&buf));
    flag_flag_set_bool(fs, S("A"), false, S("for bootstrapping, allow 'any' type"));
    flag_flag_set_bool(fs, S("Alongflagname"), false, S("disable bounds checking"));
    flag_flag_set_bool(fs, S("C"), true, S("a boolean defaulting to true"));
    flag_flag_set_string(fs, S("D"), S(""), S("set relative `path` for local imports"));
    flag_flag_set_string(fs, S("E"), S("0"), S("issue 23543"));
    flag_flag_set_float64(fs, S("F"), 2.7, S("a non-zero `number`"));
    flag_flag_set_float64(fs, S("G"), 0, S("a float that defaults to zero"));
    flag_flag_set_string(fs, S("M"), S(""), S("a multiline\nhelp\nstring"));
    flag_flag_set_int(fs, S("N"), 27, S("a non-zero int"));
    flag_flag_set_bool(fs, S("O"), true, S("a flag\nmultiline help string"));
    FlagVar v = {{S("a"), S("b")}, 2};
    flag_flag_set_var(fs, (FlagValue){&flag_var_vt, &v}, S("V"),
                      S("a `list` of strings"));
    flag_flag_set_int(fs, S("Z"), 0, S("an int that defaults to zero"));
    ZeroPanicker zp0 = {true, S("")};
    ZeroPanicker zp1 = {true, S("something")};
    flag_flag_set_var(fs, (FlagValue){&zero_panicker_vt, &zp0}, S("ZP0"),
                      S("a flag whose String method panics when it is zero"));
    flag_flag_set_var(fs, (FlagValue){&zero_panicker_vt, &zp1}, S("ZP1"),
                      S("a flag whose String method panics when it is zero"));
    flag_flag_set_duration(fs, S("maxT"), 0, S("set `timeout` for dial"));
    flag_flag_set_print_defaults(fs);
    Str got = buf_string(&buf);
    Str want = str_from_cstr(default_output);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got:\n%q\nwant:\n%q", got, want);
    buf_free(&buf);
    flag_flag_set_free(fs);
}

/* Issue 19230: validate range of Int and Uint flag values. */
static void TestIntFlagOverflow(TestingT *t) {
    if (sizeof(Int) != 4)
        return;
    reset_for_testing(no_usage);
    flag_int(S("i"), 0, S(""));
    flag_uint(S("u"), 0, S(""));
    if (BURROW_OK(flag_set(S("i"), S("2147483648"))))
        testing_t_error_v(t, "unexpected success setting Int");
    if (BURROW_OK(flag_set(S("u"), S("4294967296"))))
        testing_t_error_v(t, "unexpected success setting Uint");
}

/* Issue 20998: Usage should respect CommandLine.output. Go sets os.Args to
 * "app" and two flags, and the program name here is the real one. */
static void TestUsageOutput(TestingT *t) {
    reset_for_testing(no_usage);
    reset_for_testing(default_usage);
    Buf buf;
    buf_init(&buf);
    flag_flag_set_set_output(flag_command_line, buf_writer(&buf));
    (void)flag_flag_set_parse(flag_command_line, ARGS(S("-i=1"), S("-unknown")));
    Str got = buf_string(&buf);
    Str prefix = S("flag provided but not defined: -i\nUsage of ");
    if (!strings_has_prefix(got, prefix) || !strings_has_suffix(got, S(":\n")) ||
        got.len <= prefix.len + 2)
        testing_t_errorf_v(t, "output = %q; want %q, the program name and %q", got,
                           prefix, S(":\n"));
    buf_free(&buf);
}

static void TestGetters(TestingT *t) {
    Str expected_name = S("flag set");
    FlagErrorHandling expected_error_handling = FLAG_CONTINUE_ON_ERROR;
    FlagFlagSet *fs = flag_new_flag_set(NULL, expected_name, expected_error_handling);

    if (!str_eq(flag_flag_set_name(fs), expected_name))
        testing_t_errorf_v(t, "unexpected name: got %s, expected %s",
                           flag_flag_set_name(fs), expected_name);
    if (flag_flag_set_error_handling(fs) != expected_error_handling)
        testing_t_errorf_v(t, "unexpected ErrorHandling: got %d, expected %d",
                           (Int)flag_flag_set_error_handling(fs),
                           (Int)expected_error_handling);
    IoWriter out = flag_flag_set_output(fs);
    if (out.vt == NULL || out.vt->write == NULL)
        testing_t_error_v(
            t, "unexpected output: the zero writer, expected standard error");

    expected_name = S("gopher");
    expected_error_handling = FLAG_EXIT_ON_ERROR;
    flag_flag_set_init(fs, expected_name, expected_error_handling);
    flag_flag_set_set_output(fs, io_discard);

    if (!str_eq(flag_flag_set_name(fs), expected_name))
        testing_t_errorf_v(t, "unexpected name: got %s, expected %s",
                           flag_flag_set_name(fs), expected_name);
    if (flag_flag_set_error_handling(fs) != expected_error_handling)
        testing_t_errorf_v(t, "unexpected ErrorHandling: got %d, expected %d",
                           (Int)flag_flag_set_error_handling(fs),
                           (Int)expected_error_handling);
    out = flag_flag_set_output(fs);
    if (out.vt != io_discard.vt || out.data != io_discard.data)
        testing_t_error_v(t, "unexpected output: expected io_discard");
    flag_flag_set_free(fs);
}

static void TestParseError(TestingT *t) {
    static const char *const types[] = {"bool",   "int",     "int64",   "uint",
                                        "uint64", "float64", "duration"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, S("parse error test"), FLAG_CONTINUE_ON_ERROR);
        flag_flag_set_set_output(fs, io_discard);
        flag_flag_set_bool(fs, S("bool"), false, S(""));
        flag_flag_set_int(fs, S("int"), 0, S(""));
        flag_flag_set_int64(fs, S("int64"), 0, S(""));
        flag_flag_set_uint(fs, S("uint"), 0, S(""));
        flag_flag_set_uint64(fs, S("uint64"), 0, S(""));
        flag_flag_set_float64(fs, S("float64"), 0, S(""));
        flag_flag_set_duration(fs, S("duration"), 0, S(""));
        /* Strings cannot give errors. */
        Arena ar;
        arena_init(&ar, NULL, 256);
        Str arg = fmt_sprintf_v(arena_allocator(&ar), "-%s=x", types[i]);
        Error err = flag_flag_set_parse(fs, ARGS(arg)); /* x is not a valid setting */
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Parse(%q)=nil; expected parse error", arg);
        else if (!strings_contains(error_text(err), S("invalid")) ||
                 !strings_contains(error_text(err), S("parse error")))
            testing_t_errorf_v(t, "Parse(%q)=%v; expected parse error", arg, err);
        arena_free(&ar);
        flag_flag_set_free(fs);
    }
}

static void TestRangeError(TestingT *t) {
    static const char *const bad[] = {
        "-int=123456789012345678901",
        "-int64=123456789012345678901",
        "-uint=123456789012345678901",
        "-uint64=123456789012345678901",
        "-float64=1e1000",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, S("parse error test"), FLAG_CONTINUE_ON_ERROR);
        flag_flag_set_set_output(fs, io_discard);
        flag_flag_set_int(fs, S("int"), 0, S(""));
        flag_flag_set_int64(fs, S("int64"), 0, S(""));
        flag_flag_set_uint(fs, S("uint"), 0, S(""));
        flag_flag_set_uint64(fs, S("uint64"), 0, S(""));
        flag_flag_set_float64(fs, S("float64"), 0, S(""));
        /* Strings cannot give errors, and bools and durations do not return
         * strconv.NumError. */
        Str arg = str_from_cstr(bad[i]);
        Error err = flag_flag_set_parse(fs, ARGS(arg));
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Parse(%q)=nil; expected range error", arg);
        else if (!strings_contains(error_text(err), S("invalid")) ||
                 !strings_contains(error_text(err), S("value out of range")))
            testing_t_errorf_v(t, "Parse(%q)=%v; expected range error", arg, err);
        flag_flag_set_free(fs);
    }
}

/* ------------------------------------------------------------ TestExitCode */

static const char *self_path;
enum { MAGIC = 123 };

/* The child: a set that exits on error, given one argument, and a bool flag
 * called handle when there is one. */
static int child(int argc, char **argv) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("test"), FLAG_EXIT_ON_ERROR);
    bool b; /* outlives the parse, which sets it */
    if (argc > 3)
        flag_flag_set_bool_var(fs, &b, str_from_cstr(argv[3]), false, S(""));
    Str arg = str_from_cstr(argv[2]);
    (void)flag_flag_set_parse(fs, ARGS(arg));
    flag_flag_set_free(fs);
    return MAGIC;
}

static void TestExitCode(TestingT *t) {
    static const struct {
        const char *flag;
        const char *flag_handle;
        int32_t expect_exit;
    } tests[] = {
        {"-h", NULL, 0},    {"-help", NULL, 0},       {"-undefined", NULL, 2},
        {"-h", "h", MAGIC}, {"-help", "help", MAGIC},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const char *argv[] = {self_path, "child", tests[i].flag, tests[i].flag_handle,
                              NULL};
        PalErrno err = PAL_OK;
        int64_t p[2];
        if (!pal_pipe(p, 0, &err))
            testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
        /* The child's usage goes to the pipe, which is drained and dropped. */
        int64_t fds[3] = {PAL_INVALID_HANDLE, p[1], p[1]};
        PalSpawn req = {self_path, argv, NULL, NULL, fds, 3, 0, 0};
        int64_t pid = pal_spawn(&req, &err);
        pal_close(p[1], NULL);
        if (pid < 0) {
            pal_close(p[0], NULL);
            testing_t_fatalf_v(t, "spawn: %s", pal_errno_string(err));
        }
        char drain[512];
        while (pal_read(p[0], drain, (int64_t)sizeof drain, &err) > 0) {
        }
        pal_close(p[0], NULL);
        int32_t status = -1;
        if (pal_wait(pid, &status, 0, &err) != pid)
            testing_t_fatalf_v(t, "wait: %s", pal_errno_string(err));
        if (status != tests[i].expect_exit)
            testing_t_errorf_v(
                t, "unexpected exit code for -%s with handle %s: got %d, expect %d",
                tests[i].flag, tests[i].flag_handle ? tests[i].flag_handle : "", status,
                tests[i].expect_exit);
    }
}

/* ---------------------------------------------------------------- panics */

static char panic_buf[512];

/* The text fn panicked with, or NULL when it did not panic. */
static const char *recovered(Func fn) {
    volatile bool panicked = false;
    BURROW_TRY {
        BURROW_CALLF0(fn);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        size_t n = s.len < (Int)sizeof panic_buf ? (size_t)s.len : sizeof panic_buf - 1;
        memcpy(panic_buf, s.p, n);
        panic_buf[n] = 0;
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked ? panic_buf : NULL;
}

typedef struct DefineArgs {
    FlagFlagSet *fs;
    FlagVar *v;
    Str name;
} DefineArgs;

static void define_var(void *env) {
    DefineArgs *d = env;
    flag_flag_set_var(d->fs, (FlagValue){&flag_var_vt, d->v}, d->name, S(""));
}

static void TestInvalidFlags(TestingT *t) {
    static const struct {
        const char *flag;
        const char *error_msg;
    } tests[] = {
        {"-foo", "flag \"-foo\" begins with -"},
        {"foo=bar", "flag \"foo=bar\" contains ="},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        FlagFlagSet *fs = flag_new_flag_set(NULL, S(""), FLAG_CONTINUE_ON_ERROR);
        Buf buf;
        buf_init(&buf);
        flag_flag_set_set_output(fs, buf_writer(&buf));
        FlagVar v = {0};
        DefineArgs d = {fs, &v, str_from_cstr(tests[i].flag)};
        const char *msg = recovered(BURROW_FN(Func, define_var, &d));
        if (msg == NULL || strcmp(msg, tests[i].error_msg) != 0)
            testing_t_errorf_v(
                t, "FlagSet.Var(&v, %q, \"\"): expected panic(%q), got %q",
                tests[i].flag, tests[i].error_msg, msg ? msg : "no panic");
        Arena ar;
        arena_init(&ar, NULL, 256);
        Str want = fmt_sprintf_v(arena_allocator(&ar), "%s\n", tests[i].error_msg);
        if (!str_eq(want, buf_string(&buf)))
            testing_t_errorf_v(t, "unexpected output: expected %q, but got %q", want,
                               buf_string(&buf));
        arena_free(&ar);
        buf_free(&buf);
        flag_flag_set_free(fs);
    }
}

static void TestRedefinedFlags(TestingT *t) {
    static const struct {
        const char *flag_set_name;
        const char *error_msg;
    } tests[] = {
        {"", "flag redefined: foo"},
        {"fs", "fs flag redefined: foo"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        FlagFlagSet *fs = flag_new_flag_set(NULL, str_from_cstr(tests[i].flag_set_name),
                                            FLAG_CONTINUE_ON_ERROR);
        Buf buf;
        buf_init(&buf);
        flag_flag_set_set_output(fs, buf_writer(&buf));
        FlagVar v = {0};
        DefineArgs d = {fs, &v, S("foo")};
        define_var(&d);
        const char *msg = recovered(BURROW_FN(Func, define_var, &d));
        if (msg == NULL || strcmp(msg, tests[i].error_msg) != 0)
            testing_t_errorf_v(
                t, "flag redefined in FlagSet(%q): expected panic(%q), got %q",
                tests[i].flag_set_name, tests[i].error_msg, msg ? msg : "no panic");
        Arena ar;
        arena_init(&ar, NULL, 256);
        Str want = fmt_sprintf_v(arena_allocator(&ar), "%s\n", tests[i].error_msg);
        if (!str_eq(want, buf_string(&buf)))
            testing_t_errorf_v(t, "unexpected output: expected %q, but got %q", want,
                               buf_string(&buf));
        arena_free(&ar);
        buf_free(&buf);
        flag_flag_set_free(fs);
    }
}

static void TestUserDefinedBoolFunc(TestingT *t) {
    FlagFlagSet *flags = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(flags, io_discard);
    FlagVar ss = {0};
    flag_flag_set_bool_func(flags, S("v"), S("usage"),
                            BURROW_FN(FlagFunc, append_func, &ss));
    Error err =
        flag_flag_set_parse(flags, ARGS(S("-v"), S(""), S("-v"), S("1"), S("-v=2")));
    if (!BURROW_OK(err))
        testing_t_error_v(t, err);
    if (ss.n != 1)
        testing_t_fatalf_v(t, "got %d args; want 1 arg", ss.n);
    Buf buf;
    buf_init(&buf);
    Str got = flag_var_text(&ss, arena_allocator(&buf.ar));
    if (!str_eq(got, S("[true]")))
        testing_t_errorf_v(t, "got %q; want %q", got, S("[true]"));
    /* test usage */
    flag_flag_set_set_output(flags, buf_writer(&buf));
    (void)flag_flag_set_parse(flags, ARGS(S("-h")));
    Str usage = buf_string(&buf);
    if (!strings_contains(usage, S("usage")))
        testing_t_errorf_v(t, "usage string not included: %q", usage);
    buf_free(&buf);
    flag_flag_set_free(flags);

    /* test BoolFunc error */
    flags = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_set_output(flags, io_discard);
    flag_flag_set_bool_func(flags, S("v"), S("usage"),
                            BURROW_FN(FlagFunc, test_error_func, NULL));
    /* flag not set, so no error */
    err = flag_flag_set_parse(flags, (Slice){NULL, 0, 0, TYPE_STRING});
    if (!BURROW_OK(err))
        testing_t_error_v(t, err);
    /* flag set, expect error */
    err = flag_flag_set_parse(flags, ARGS(S("-v"), S("")));
    if (BURROW_OK(err))
        testing_t_error_v(t, "got err == nil; want err != nil");
    else if (!strings_contains(error_text(err), S("test error")))
        testing_t_errorf_v(t, "got %q; error should contain \"test error\"",
                           error_text(err));
    flag_flag_set_free(flags);
}

static void define_my_flag(void *env) {
    (void)flag_flag_set_string(env, S("myFlag"), S("default"), S("usage"));
}

static void TestDefineAfterSet(TestingT *t) {
    FlagFlagSet *flags = flag_new_flag_set(NULL, S("test"), FLAG_CONTINUE_ON_ERROR);
    /* Set by itself doesn't panic. */
    (void)flag_flag_set_set(flags, S("myFlag"), S("value"));

    /* Define-after-set panics. */
    const char *msg = recovered(BURROW_FN(Func, define_my_flag, flags));
    Str got = msg ? str_from_cstr(msg) : BURROW_STR_EMPTY;
    if (!strings_has_prefix(got, S("flag myFlag set at ")) ||
        !strings_contains(got, S("flag_test.c:")) ||
        !strings_has_suffix(got, S(" before being defined")))
        testing_t_errorf_v(
            t, "DefineAfterSet: expected panic(%q), got %q",
            S("flag myFlag set at .*/flag_test.c:.* before being defined"),
            msg ? got : S("no panic"));
    flag_flag_set_free(flags);
}

/* ------------------------------------------------------ beyond Go's tests */

/* TextVar, which Go tests only in an example, with netip.Addr as the type. */
static void TestTextVar(TestingT *t) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("text"), FLAG_CONTINUE_ON_ERROR);
    Buf buf;
    buf_init(&buf);
    flag_flag_set_set_output(fs, buf_writer(&buf));
    NetipAddr ip;
    NetipAddr def = netip_must_parse_addr(S("127.0.0.1"));
    flag_flag_set_text_var(fs, BURROW_ANY(TYPE_NETIP_ADDR, &ip), S("ip"),
                           BURROW_ANY(TYPE_NETIP_ADDR, &def), S("`address` to use"));
    if (!netip_addr_eq(ip, def))
        testing_t_error_v(t, "the default was not stored");
    Flag *f = flag_flag_set_lookup(fs, S("ip"));
    if (f == NULL) {
        testing_t_error_v(t, "no flag called ip");
        buf_free(&buf);
        flag_flag_set_free(fs);
        return;
    }
    if (!str_eq(f->def_value, S("127.0.0.1")))
        testing_t_errorf_v(t, "DefValue = %q, want %q", f->def_value, S("127.0.0.1"));
    Error err = flag_flag_set_parse(fs, ARGS(S("-ip"), S("192.168.0.100")));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, err);
    if (!netip_addr_eq(ip, netip_must_parse_addr(S("192.168.0.100"))))
        testing_t_error_v(t, "-ip did not set the address");
    Any got = f->value.vt->get(f->value.data);
    if (got.t != TYPE_NETIP_ADDR || got.data != &ip)
        testing_t_error_v(t, "Get did not return the variable");

    err = flag_flag_set_parse(fs, ARGS(S("-ip=256.0.0.1")));
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), S("invalid value \"256.0.0.1\"")))
        testing_t_errorf_v(t, "a bad address gave %v", err);

    flag_flag_set_print_defaults(fs);
    Str want = S("  -ip address\n    \taddress to use (default 127.0.0.1)\n");
    if (!strings_has_suffix(buf_string(&buf), want))
        testing_t_errorf_v(t, "usage = %q, want it to end %q", buf_string(&buf), want);
    buf_free(&buf);
    flag_flag_set_free(fs);
}

typedef struct TextArgs {
    FlagFlagSet *fs;
    NetipAddr *ip;
    Str *def;
} TextArgs;

static void define_mismatched(void *env) {
    TextArgs *a = env;
    flag_flag_set_text_var(a->fs, BURROW_ANY(TYPE_NETIP_ADDR, a->ip), S("ip"),
                           BURROW_ANY(TYPE_STRING, a->def), S(""));
}

static void TestTextVarTypeMismatch(TestingT *t) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("text"), FLAG_CONTINUE_ON_ERROR);
    NetipAddr ip;
    Str def = S("127.0.0.1");
    TextArgs a = {fs, &ip, &def};
    const char *msg = recovered(BURROW_FN(Func, define_mismatched, &a));
    const char *want =
        "default type does not match variable type: string != netip.Addr";
    if (msg == NULL || strcmp(msg, want) != 0)
        testing_t_errorf_v(t, "got panic %q, want %q", msg ? msg : "none", want);
    flag_flag_set_free(fs);
}

static void parse_panicking(void *env) {
    (void)flag_flag_set_parse(env, ARGS(S("-nope")));
}

static void TestPanicOnError(TestingT *t) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("p"), FLAG_PANIC_ON_ERROR);
    flag_flag_set_set_output(fs, io_discard);
    const char *msg = recovered(BURROW_FN(Func, parse_panicking, fs));
    const char *want = "flag provided but not defined: -nope";
    if (msg == NULL || strcmp(msg, want) != 0)
        testing_t_errorf_v(t, "got panic %q, want %q", msg ? msg : "none", want);
    flag_flag_set_free(fs);
}

/* A set's Arg outside the arguments, and a flag given twice, which NFlag
 * counts once. */
static void TestArgAndNFlag(TestingT *t) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("n"), FLAG_CONTINUE_ON_ERROR);
    Int *n = flag_flag_set_int(fs, S("n"), 0, S(""));
    Error err =
        flag_flag_set_parse(fs, ARGS(S("-n=1"), S("-n"), S("2"), S("--"), S("-n")));
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, err);
    CHECK_INT_EQ(*n, 2);
    CHECK_INT_EQ(flag_flag_set_n_flag(fs), 1);
    CHECK_INT_EQ(flag_flag_set_n_arg(fs), 1);
    CHECK(str_eq(flag_flag_set_arg(fs, 0), S("-n")));
    CHECK(flag_flag_set_arg(fs, 1).len == 0);
    CHECK(flag_flag_set_arg(fs, -1).len == 0);
    Flag *f = flag_flag_set_lookup(fs, S("n"));
    Arena ar;
    arena_init(&ar, NULL, 256);
    Str usage;
    CHECK(f != NULL &&
          str_eq(flag_unquote_usage(arena_allocator(&ar), f, &usage), S("int")));
    arena_free(&ar);
    CHECK(flag_flag_set_lookup(fs, S("m")) == NULL);
    flag_flag_set_free(fs);
}

/* %v of a Duration goes through its String method. */
static void TestDurationDescriptor(TestingT *t) {
    Duration d = 1500 * TIME_MILLISECOND;
    Arena ar;
    arena_init(&ar, NULL, 256);
    Str got = fmt_sprintf_v(arena_allocator(&ar), "%v", BURROW_ANY(TYPE_DURATION, &d));
    if (!str_eq(got, S("1.5s")))
        testing_t_errorf_v(t, "%%v of 1.5s is %q", got);
    arena_free(&ar);
}

/* --------------------------------------------------------------- benchmarks */

static void BenchmarkParse(TestingB *b) {
    FlagFlagSet *fs = flag_new_flag_set(NULL, S("bench"), FLAG_CONTINUE_ON_ERROR);
    flag_flag_set_bool(fs, S("v"), false, S(""));
    flag_flag_set_int(fs, S("n"), 0, S(""));
    flag_flag_set_string(fs, S("o"), S(""), S(""));
    flag_flag_set_duration(fs, S("timeout"), 0, S(""));
    Slice args = ARGS(S("-v"), S("-n=10"), S("-o"), S("out.txt"), S("-timeout=1m30s"),
                      S("file"));
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)flag_flag_set_parse(fs, args);
    flag_flag_set_free(fs);
}

#define TESTS(X)                                                                       \
    X(TestEverything)                                                                  \
    X(TestGet)                                                                         \
    X(TestUsage)                                                                       \
    X(TestParse)                                                                       \
    X(TestFlagSetParse)                                                                \
    X(TestUserDefined)                                                                 \
    X(TestUserDefinedFunc)                                                             \
    X(TestUserDefinedForCommandLine)                                                   \
    X(TestUserDefinedBool)                                                             \
    X(TestUserDefinedBoolUsage)                                                        \
    X(TestSetOutput)                                                                   \
    X(TestChangingArgs)                                                                \
    X(TestHelp)                                                                        \
    X(TestPrintDefaults)                                                               \
    X(TestIntFlagOverflow)                                                             \
    X(TestUsageOutput)                                                                 \
    X(TestGetters)                                                                     \
    X(TestParseError)                                                                  \
    X(TestRangeError)                                                                  \
    X(TestExitCode)                                                                    \
    X(TestInvalidFlags)                                                                \
    X(TestRedefinedFlags)                                                              \
    X(TestUserDefinedBoolFunc)                                                         \
    X(TestDefineAfterSet)                                                              \
    X(TestTextVar)                                                                     \
    X(TestTextVarTypeMismatch)                                                         \
    X(TestPanicOnError)                                                                \
    X(TestArgAndNFlag)                                                                 \
    X(TestDurationDescriptor)                                                          \
    X(BenchmarkParse)

/* A relative program path is taken relative to the directory the child starts
 * in, so the child is started by an absolute one. */
static char self_buf[2048];

static const char *absolute(const char *p) {
    bool abs = p[0] == '/' || p[0] == '\\' || (p[0] != 0 && p[1] == ':');
    char cwd[1024];
    if (abs || pal_getcwd(cwd, sizeof cwd, NULL) < 0)
        return p;
    snprintf(self_buf, sizeof self_buf, "%s/%s", cwd, p);
    return self_buf;
}

int main(int argc, char **argv) {
    self_path = absolute(argv[0]);
    if (argc >= 3 && strcmp(argv[1], "child") == 0)
        return child(argc, argv);
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(argc, argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                testing_m_run);
}
