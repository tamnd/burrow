/* Tests for text/template, ported from Go's exec_test.go and multi_test.go.
 *
 * The data the templates run on is Go's, given descriptors so that reflect can
 * see it. Where C makes something come out differently, the test says what C
 * does and why:
 *
 * The execTests entries that range over an iter.Seq or an iter.Seq2 are left
 * out, eleven of them, because C has no range over a function and the executor
 * does not try one. TestIssue48215 is left out too: its struct embeds a
 * pointer, and an embedded field is named after its type, which an unnamed
 * pointer descriptor does not have.
 *
 * fmt does not call a method on a nil pointer, so (*V)(nil).String() and
 * (*W)(nil).Error() print <nil> where Go prints nilV and nilW.
 *
 * Type names in messages have no package in front of them, so Go's *template.T
 * is *T here. Go's interface I is Ifc, since I is the imaginary unit once
 * complex.h is in, and the message is about Ifc.Method0.
 *
 * A method takes its receiver as a pointer, so T and *T have the same methods.
 * T has no Tmpl field, which nothing in the tests reads.
 *
 * The parse trees the add_parse_tree tests make are parsed with no functions
 * where Go hands parse.Parse the builtins, since none of their texts call one.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/text/template_internal.h"
#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/reflect.h"
#include "burrow/runtime.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/text/template.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)

/* A descriptor written out by hand. */
#define DESC(name, pkg, kind, size, align, nfield, fields, nmethod, methods, elem,     \
             len, ops)                                                                 \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        nfield,                                                                        \
        nmethod,                                                                       \
        fields,                                                                        \
        methods,                                                                       \
        elem,                                                                          \
        NULL,                                                                          \
        len,                                                                           \
        0,                                                                             \
        ops,                                                                           \
    }

#define NONAME {NULL, 0}

/* A parameter or a result of a function type. */
#define PARAM(ty, i) {S_(""), S_(""), ty, (uint32_t)(i)}

/* An unnamed function type taking nin of the nfield entries of fields. */
#define FUNC_DESC(nfield, fields, elem, nin, ops)                                      \
    DESC(NONAME, NONAME, KIND_FUNC, sizeof(Func), _Alignof(Func), nfield, fields, 0,   \
         NULL, elem, nin, ops)

/* An unnamed channel type, with dir 0 for both ways, 1 for receive only and 2
 * for send only. */
#define CHAN_DESC(elem, dir)                                                           \
    DESC(NONAME, NONAME, KIND_CHAN, sizeof(Chan *), _Alignof(Chan *), 0, NULL, 0,      \
         NULL, elem, dir, NULL)

/* []Str{...}. */
#define NARGS(T, ...) ((Int)(sizeof((T[]){__VA_ARGS__}) / sizeof(T)))
#define STRS(...)                                                                      \
    slice_from((Str[]){__VA_ARGS__}, NARGS(Str, __VA_ARGS__), NARGS(Str, __VA_ARGS__), \
               TYPE_STRING)

#define NIL_ANY ((Any){NULL, NULL})

/* Where the methods and functions below make what they return. */
static Arena tt_arena;
static Alloc *tt_a;

/* ------------------------------------------------------------ the types */

#define SIG_STR(IN, OUT) OUT(Str)

/* The non-empty interface, Go's I. */
typedef struct IfcVT {
    const Type *self_type;
    Str (*method0)(void *self);
} IfcVT;
typedef struct Ifc {
    const IfcVT *vt;
    void *data;
} Ifc;
#define IFC_METHODS(M, I) M(I, Method0, method0, SIG_STR)
BURROW_IFACE_TYPE_DECL(Ifc);
BURROW_IFACE_TYPE_DEFINE(Ifc, IFC_METHODS);
BURROW_PTR_TYPE(IfcPtr, Ifc);

/* fmt.Stringer. */
typedef struct StringerVT {
    const Type *self_type;
    Str (*string)(void *self);
} StringerVT;
typedef struct Stringer {
    const StringerVT *vt;
    void *data;
} Stringer;
#define STRINGER_METHODS(M, I) M(I, String, string, SIG_STR)
BURROW_IFACE_TYPE_DECL(Stringer);
BURROW_IFACE_TYPE_DEFINE(Stringer, STRINGER_METHODS);

/* What bytes.NewBuffer gives Go's tests, a Stringer holding text. */
typedef struct Buffer Buffer;
static Str buffer_string(Buffer *b);
#define BUFFER_FIELDS(F, T) F(T, Str, s, "")
#define BUFFER_METHODS(M, T) M(T, String, buffer_string, SIG_STR)
BURROW_STRUCT_DECL(Buffer, BUFFER_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(Buffer, BUFFER_FIELDS, BUFFER_METHODS);

static Str buffer_string(Buffer *b) {
    return b->s;
}

static Str buffer_vt_string(void *self) {
    return buffer_string(self);
}

static const StringerVT buffer_stringer_vt = {&burrow_type_Buffer, buffer_vt_string};
BURROW_IMPLEMENTS(Buffer, Stringer, buffer_stringer_vt);

/* S, a slice of strings with a method. */
typedef Slice S;
static Str s_method0(S *s);
#define S_METHODS(M, T) M(T, Method0, s_method0, SIG_STR)
BURROW_METHODS_DEFINE(S, S_METHODS);
static const Type burrow_type_S =
    DESC(S_("S"), NONAME, KIND_SLICE, sizeof(Slice), _Alignof(Slice), 0, NULL, 1,
         burrow__methods_S, TYPE_STRING, 0, NULL);

static Str s_method0(S *s) {
    (void)s;
    return BURROW_S("M0");
}

/* U, V and W. */
typedef struct U U;
static Str u_true_false(U *u, bool b);
#define U_SIG_TrueFalse(IN, OUT) IN(0, bool) OUT(Str)
#define U_FIELDS(F, T) F(T, Str, V, "")
#define U_METHODS(M, T) M(T, TrueFalse, u_true_false, U_SIG_TrueFalse)
BURROW_STRUCT_DECL(U, U_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(U, U_FIELDS, U_METHODS);
BURROW_PTR_TYPE(UPtr, U);

static Str u_true_false(U *u, bool b) {
    (void)u;
    return b ? BURROW_S("true") : BURROW_STR_EMPTY;
}

typedef struct V V;
static Str v_string(V *v);
#define V_FIELDS(F, T) F(T, Int, j, "")
#define V_METHODS(M, T) M(T, String, v_string, SIG_STR)
BURROW_STRUCT_DECL(V, V_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(V, V_FIELDS, V_METHODS);
BURROW_PTR_TYPE(VPtr, V);

static Str v_string(V *v) {
    if (v == NULL)
        return BURROW_S("nilV");
    return fmt_sprintf_v(tt_a, "<%d>", v->j);
}

typedef struct W W;
static Str w_error(W *w);
#define W_FIELDS(F, T) F(T, Int, k, "")
#define W_METHODS(M, T) M(T, Error, w_error, SIG_STR)
BURROW_STRUCT_DECL(W, W_FIELDS);
BURROW_STRUCT_DEFINE_METHODS(W, W_FIELDS, W_METHODS);
BURROW_PTR_TYPE(WPtr, W);

static Str w_error(W *w) {
    if (w == NULL)
        return BURROW_S("nilW");
    return fmt_sprintf_v(tt_a, "[%d]", w->k);
}

/* The composites T's fields need. */
BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_SLICE_TYPE(BoolSlice, bool);
BURROW_SLICE_TYPE(StrSlice, Str);
BURROW_SLICE_TYPE(VSlice, V);
BURROW_ARRAY_TYPE(IntArray3, Int, 3);
BURROW_ARRAY_TYPE(IntArray2, Int, 2);
BURROW_PTR_TYPE(IntArray3Ptr, IntArray3);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_PTR_TYPE(StrPtr, Str);
BURROW_PTR_TYPE(IntSlicePtr, IntSlice);
BURROW_MAP_TYPE(StrIntMap, Str, Int);
BURROW_MAP_TYPE(AnyIntMap, Any, Int);
BURROW_MAP_TYPE(IntIntMap, Int, Int);
BURROW_MAP_TYPE(I32StrMap, int32_t, Str);
BURROW_MAP_TYPE(I64StrMap, int64_t, Str);
BURROW_MAP_TYPE(U32StrMap, uint32_t, Str);
BURROW_MAP_TYPE(U64StrMap, uint64_t, Str);
BURROW_MAP_TYPE(I8StrMap, int8_t, Str);
BURROW_MAP_TYPE(U8StrMap, uint8_t, Str);
BURROW_MAP_TYPE(StrStrMap, Str, Str);
BURROW_MAP_TYPE(StrAnyMap, Str, Any);
BURROW_PTR_TYPE(StrStrMapPtr, StrStrMap);
BURROW_SLICE_TYPE(StrIntMapSlice, StrIntMap);

typedef Chan *StrChan;
static const Type burrow_type_StrChan = CHAN_DESC(TYPE_STRING, 0);
typedef Chan *IntChan;
static const Type burrow_type_IntChan = CHAN_DESC(TYPE_INT, 0);
typedef Chan *IntSendChan;
static const Type burrow_type_IntSendChan = CHAN_DESC(TYPE_INT, 2);

/* The function types, of T's fields and of the functions the templates call. */
#define SIG_NONE(IN, OUT)
BURROW_FUNC0(VoidFn, void);
BURROW_FUNC_TYPE_DEFINE(VoidFn, SIG_NONE);

BURROW_FUNC0(StrFn, Str);
BURROW_FUNC_TYPE_DEFINE(StrFn, SIG_STR);

#define SIG_STR_STR(IN, OUT) IN(0, Str) OUT(Str)
BURROW_FUNC(StrStrFn, Str, Str a);
BURROW_FUNC_TYPE_DEFINE(StrStrFn, SIG_STR_STR);

#define SIG_BINARY(IN, OUT) IN(0, Str) IN(1, Str) OUT(Str)
BURROW_FUNC(BinaryFn, Str, Str a, Str b);
BURROW_FUNC_TYPE_DEFINE(BinaryFn, SIG_BINARY);

#define SIG_VARIADIC(IN, OUT) IN(0, StrSlice) OUT(Str)
BURROW_FUNC(VariadicFn, Str, Slice s);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(VariadicFn, Str, SIG_VARIADIC);

#define SIG_VARIADIC_INT(IN, OUT) IN(0, Int) IN(1, StrSlice) OUT(Str)
BURROW_FUNC(VariadicIntFn, Str, Int a, Slice s);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(VariadicIntFn, Str, SIG_VARIADIC_INT);

#define SIG_NIL_OK(IN, OUT) IN(0, IntPtr) OUT(bool)
BURROW_FUNC(NilOkFn, bool, IntPtr s);
BURROW_FUNC_TYPE_DEFINE(NilOkFn, SIG_NIL_OK);

#define SIG_ERR(IN, OUT) OUT(Str) OUT(ErrorResult)
BURROW_FUNC(ErrFn, Str, Error *err);
BURROW_FUNC_TYPE_DEFINE(ErrFn, SIG_ERR);

/* func() (string, error, int) and func() (string, bool), which the template
 * refuses to call, so the call op is never reached. */
static void never_call(const void *fn, void **args, void **rets) {
    (void)fn;
    (void)args;
    (void)rets;
}

static const TypeOps never_ops = {NULL, NULL, NULL, NULL, never_call};
static const Field too_many_sig[] = {PARAM(TYPE_STRING, 0), PARAM(TYPE_OF(Error), 0),
                                     PARAM(TYPE_INT, 0)};
static const Field invalid_ret_sig[] = {PARAM(TYPE_STRING, 0), PARAM(TYPE_BOOL, 0)};
typedef VoidFn TooManyFn;
static const Type burrow_type_TooManyFn =
    FUNC_DESC(3, too_many_sig, NULL, 0, &never_ops);
typedef VoidFn InvalidRetFn;
static const Type burrow_type_InvalidRetFn =
    FUNC_DESC(2, invalid_ret_sig, NULL, 0, &never_ops);

/* T has lots of interesting pieces to use to test execution. */
typedef struct T T;

#define T_FIELDS(F, T)                                                                 \
    F(T, bool, True, "")                                                               \
    F(T, Int, I, "")                                                                   \
    F(T, uint16_t, U16, "")                                                            \
    F(T, Str, X, "")                                                                   \
    F(T, Str, S, "")                                                                   \
    F(T, double, FloatZero, "")                                                        \
    F(T, Complex128, ComplexZero, "")                                                  \
    F(T, UPtr, U, "")                                                                  \
    F(T, V, V0, "")                                                                    \
    F(T, VPtr, V1, "")                                                                 \
    F(T, VPtr, V2, "")                                                                 \
    F(T, W, W0, "")                                                                    \
    F(T, WPtr, W1, "")                                                                 \
    F(T, WPtr, W2, "")                                                                 \
    F(T, IntSlice, SI, "")                                                             \
    F(T, IntSlice, SICap, "")                                                          \
    F(T, IntSlice, SIEmpty, "")                                                        \
    F(T, BoolSlice, SB, "")                                                            \
    F(T, IntArray3, AI, "")                                                            \
    F(T, IntArray3Ptr, PAI, "")                                                        \
    F(T, StrIntMap, MSI, "")                                                           \
    F(T, StrIntMap, MSIone, "")                                                        \
    F(T, StrIntMap, MSIEmpty, "")                                                      \
    F(T, AnyIntMap, MXI, "")                                                           \
    F(T, IntIntMap, MII, "")                                                           \
    F(T, I32StrMap, MI32S, "")                                                         \
    F(T, I64StrMap, MI64S, "")                                                         \
    F(T, U32StrMap, MUI32S, "")                                                        \
    F(T, U64StrMap, MUI64S, "")                                                        \
    F(T, I8StrMap, MI8S, "")                                                           \
    F(T, U8StrMap, MUI8S, "")                                                          \
    F(T, StrIntMapSlice, SMSI, "")                                                     \
    F(T, Any, Empty0, "")                                                              \
    F(T, Any, Empty1, "")                                                              \
    F(T, Any, Empty2, "")                                                              \
    F(T, Any, Empty3, "")                                                              \
    F(T, Any, Empty4, "")                                                              \
    F(T, Ifc, NonEmptyInterface, "")                                                   \
    F(T, IfcPtr, NonEmptyInterfacePtS, "")                                             \
    F(T, Ifc, NonEmptyInterfaceNil, "")                                                \
    F(T, Ifc, NonEmptyInterfaceTypedNil, "")                                           \
    F(T, Stringer, Str, "")                                                            \
    F(T, Error, Err, "")                                                               \
    F(T, IntPtr, PI, "")                                                               \
    F(T, StrPtr, PS, "")                                                               \
    F(T, IntSlicePtr, PSI, "")                                                         \
    F(T, IntPtr, NIL, "")                                                              \
    F(T, UnsafePointer, UPI, "")                                                       \
    F(T, UnsafePointer, EmptyUPI, "")                                                  \
    F(T, BinaryFn, BinaryFunc, "")                                                     \
    F(T, VariadicFn, VariadicFunc, "")                                                 \
    F(T, VariadicIntFn, VariadicFuncInt, "")                                           \
    F(T, NilOkFn, NilOKFunc, "")                                                       \
    F(T, ErrFn, ErrFunc, "")                                                           \
    F(T, StrFn, PanicFunc, "")                                                         \
    F(T, VoidFn, TooFewReturnCountFunc, "")                                            \
    F(T, TooManyFn, TooManyReturnCountFunc, "")                                        \
    F(T, InvalidRetFn, InvalidReturnTypeFunc, "")                                      \
    F(T, Int, unexported, "")

BURROW_STRUCT_DECL(T, T_FIELDS);
BURROW_PTR_TYPE(TPtr, T);

static TPtr t_copy(T *t);
static UPtr t_get_u(T *t);
static Slice t_madd(T *t, Int a, Slice b);
static Str t_method0(T *t);
static Int t_method1(T *t, Int a);
static Str t_method2(T *t, uint16_t a, Str b);
static Str t_method3(T *t, Any v);
static bool t_my_error(T *t, bool error, Error *err);

#define T_SIG_Copy(IN, OUT) OUT(TPtr)
#define T_SIG_GetU(IN, OUT) OUT(UPtr)
#define T_SIG_MAdd(IN, OUT) IN(0, Int) IN(1, IntSlice) OUT(IntSlice)
#define T_SIG_Method1(IN, OUT) IN(0, Int) OUT(Int)
#define T_SIG_Method2(IN, OUT) IN(0, uint16_t) IN(1, Str) OUT(Str)
#define T_SIG_Method3(IN, OUT) IN(0, Any) OUT(Str)
#define T_SIG_MyError(IN, OUT) IN(0, bool) OUT(bool) OUT(ErrorResult)
#define T_METHODS(M, T)                                                                \
    M(T, Copy, t_copy, T_SIG_Copy)                                                     \
    M(T, GetU, t_get_u, T_SIG_GetU)                                                    \
    M(T, MAdd, t_madd, T_SIG_MAdd)                                                     \
    M(T, Method0, t_method0, SIG_STR)                                                  \
    M(T, Method1, t_method1, T_SIG_Method1)                                            \
    M(T, Method2, t_method2, T_SIG_Method2)                                            \
    M(T, Method3, t_method3, T_SIG_Method3)                                            \
    M(T, MyError, t_my_error, T_SIG_MyError)
BURROW_STRUCT_DEFINE_METHODS(T, T_FIELDS, T_METHODS);
BURROW_SLICE_TYPE(TPtrSlice, TPtr);

/* Simple methods with and without arguments. */
static Str t_method0(T *t) {
    (void)t;
    return BURROW_S("M0");
}

static Int t_method1(T *t, Int a) {
    (void)t;
    return a;
}

static Str t_method2(T *t, uint16_t a, Str b) {
    (void)t;
    return fmt_sprintf_v(tt_a, "Method2: %d %s", a, b);
}

static Str t_method3(T *t, Any v) {
    (void)t;
    return fmt_sprintf_v(tt_a, "Method3: %v", v);
}

static TPtr t_copy(T *t) {
    T *n = BURROW_NEW(tt_a, T);
    *n = *t;
    return n;
}

static Slice t_madd(T *t, Int a, Slice b) {
    (void)t;
    Slice v = slice_make(tt_a, TYPE_INT, b.len, b.len);
    for (Int i = 0; i < b.len; i++)
        ((Int *)v.p)[i] = ((Int *)b.p)[i] + a;
    return v;
}

BURROW_SENTINEL_ERROR(my_error, "my error");

/* MyError returns a value and an error according to its argument. */
static bool t_my_error(T *t, bool error, Error *err) {
    (void)t;
    if (error) {
        *err = my_error;
        return true;
    }
    return false;
}

/* A few methods to test chaining. */
static UPtr t_get_u(T *t) {
    if (t == NULL)
        runtime_panic(BURROW_S(
            "runtime error: invalid memory address or nil pointer dereference"));
    return t->U;
}

/* Method0 for every value an Ifc holds in these tests, none of which looks at
 * itself. */
static Str ifc_method0(void *self) {
    (void)self;
    return BURROW_S("M0");
}

static const IfcVT tptr_ifc_vt = {&burrow_type_TPtr, ifc_method0};
static const IfcVT s_ifc_vt = {&burrow_type_S, ifc_method0};

/* ------------------------------------------------- T's function fields */

static Str binary_func(void *env, Str a, Str b) {
    (void)env;
    return fmt_sprintf_v(tt_a, "[%s=%s]", a, b);
}

static Str variadic_func(void *env, Slice s) {
    (void)env;
    return fmt_sprintf_v(tt_a, "<%s>", strings_join(tt_a, s, BURROW_S("+")));
}

static Str variadic_func_int(void *env, Int a, Slice s) {
    (void)env;
    return fmt_sprintf_v(tt_a, "%d=<%s>", a, strings_join(tt_a, s, BURROW_S("+")));
}

static bool nil_ok_func(void *env, IntPtr s) {
    (void)env;
    return s == NULL;
}

static Str err_func(void *env, Error *err) {
    (void)env;
    (void)err;
    return BURROW_S("bla");
}

static Str panic_func(void *env) {
    (void)env;
    panic_str(BURROW_S("test panic"));
}

static void void_func(void *env) {
    (void)env;
}

/* ---------------------------------------------- the functions in funcs */

static Str zero_args(void *env) {
    (void)env;
    return BURROW_S("zeroArgs");
}

static Str one_arg(void *env, Str a) {
    (void)env;
    return fmt_sprintf_v(tt_a, "oneArg=%s", a);
}

static Str two_args(void *env, Str a, Str b) {
    (void)env;
    return fmt_sprintf_v(tt_a, "twoArgs=%s%s", a, b);
}

#define SIG_DDD_ARG(IN, OUT) IN(0, Int) IN(1, StrSlice) OUT(Str)
BURROW_FUNC(DddArgFn, Str, Int a, Slice b);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(DddArgFn, Str, SIG_DDD_ARG);

static Str ddd_arg(void *env, Int a, Slice b) {
    (void)env;
    return fmt_sprintln_v(tt_a, a, b);
}

/* count returns a channel that will deliver n sequential 1-letter strings
 * starting at "a". The goroutines are waited for and the channels freed by
 * count_reap. */
typedef struct CountJob {
    Chan *c;
    Int n;
} CountJob;

static CountJob count_jobs[4];
static int count_njobs;
static SyncWaitGroup count_wg;

static void count_run(void *env) {
    CountJob *j = env;
    for (Int i = 0; i < j->n; i++) {
        Str s = str_from_bytes(&"abcdefghijklmnop"[i], 1);
        chan_send(j->c, &s);
    }
    chan_close(j->c);
}

#define SIG_COUNT(IN, OUT) IN(0, Int) OUT(StrChan)
BURROW_FUNC(CountFn, StrChan, Int n);
BURROW_FUNC_TYPE_DEFINE(CountFn, SIG_COUNT);

static StrChan count(void *env, Int n) {
    (void)env;
    if (n == 0)
        return NULL;
    if (count_njobs == (int)(sizeof count_jobs / sizeof count_jobs[0]))
        panic_str(BURROW_S("count: too many channels"));
    CountJob *j = &count_jobs[count_njobs++];
    j->c = chan_make(heap_allocator(), TYPE_STRING, 0);
    j->n = n;
    sync_wait_group_go(&count_wg, BURROW_FN(Func, count_run, j));
    return j->c;
}

static void count_reap(void) {
    sync_wait_group_wait(&count_wg);
    for (int i = 0; i < count_njobs; i++)
        chan_free(count_jobs[i].c);
    count_njobs = 0;
}

/* vfunc takes a V and a *V. */
#define SIG_V(IN, OUT) IN(0, V) IN(1, VPtr) OUT(Str)
BURROW_FUNC(VFn, Str, V v, VPtr p);
BURROW_FUNC_TYPE_DEFINE(VFn, SIG_V);

static Str vfunc(void *env, V v, VPtr p) {
    (void)env;
    (void)v;
    (void)p;
    return BURROW_S("vfunc");
}

/* valueString takes a string, not a pointer. */
static Str value_string(void *env, Str v) {
    (void)env;
    (void)v;
    return BURROW_S("value is ignored");
}

#define SIG_INT(IN, OUT) OUT(Int)
BURROW_FUNC0(IntFn, Int);
BURROW_FUNC_TYPE_DEFINE(IntFn, SIG_INT);

/* returnInt returns an int. */
static Int return_int(void *env) {
    (void)env;
    return 7;
}

#define SIG_ADD(IN, OUT) IN(0, IntSlice) OUT(Int)
BURROW_FUNC(AddFn, Int, Slice args);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(AddFn, Int, SIG_ADD);

static Int add(void *env, Slice args) {
    (void)env;
    Int sum = 0;
    for (Int i = 0; i < args.len; i++)
        sum += ((Int *)args.p)[i];
    return sum;
}

#define SIG_ECHO(IN, OUT) IN(0, Any) OUT(Any)
BURROW_FUNC(EchoFn, Any, Any arg);
BURROW_FUNC_TYPE_DEFINE(EchoFn, SIG_ECHO);

static Any echo(void *env, Any arg) {
    (void)env;
    return arg;
}

#define SIG_MAKEMAP(IN, OUT) IN(0, StrSlice) OUT(StrStrMap)
BURROW_FUNC(MakemapFn, StrStrMap, Slice arg);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(MakemapFn, Str, SIG_MAKEMAP);

static StrStrMap makemap(void *env, Slice arg) {
    (void)env;
    if (arg.len % 2 != 0)
        panic_str(BURROW_S("bad makemap"));
    Map *m = map_make(tt_a, TYPE_STRING, TYPE_STRING, 0);
    Str *s = arg.p;
    for (Int i = 0; i < arg.len; i += 2)
        map_set(m, &s[i], &s[i + 1]);
    return m;
}

#define SIG_STRINGER(IN, OUT) IN(0, Stringer) OUT(Str)
BURROW_FUNC(StringerFn, Str, Stringer s);
BURROW_FUNC_TYPE_DEFINE(StringerFn, SIG_STRINGER);

static Str stringer(void *env, Stringer s) {
    (void)env;
    return s.vt->string(s.data);
}

#define SIG_ANY(IN, OUT) OUT(Any)
BURROW_FUNC0(AnyFn, Any);
BURROW_FUNC_TYPE_DEFINE(AnyFn, SIG_ANY);

static Map *three_map;

static Any map_of_three(void *env) {
    (void)env;
    return BURROW_ANY(TYPE_OF(StrIntMap), &three_map);
}

#define SIG_TYPE_OF(IN, OUT) IN(0, Any) OUT(Str)
BURROW_FUNC(TypeOfFn, Str, Any arg);
BURROW_FUNC_TYPE_DEFINE(TypeOfFn, SIG_TYPE_OF);

static Str type_of(void *env, Any arg) {
    (void)env;
    return fmt_sprintf_v(tt_a, "%T", arg);
}

#define SIG_BOOL(IN, OUT) OUT(bool)
BURROW_FUNC0(BoolFn, bool);
BURROW_FUNC_TYPE_DEFINE(BoolFn, SIG_BOOL);

static bool die(void *env) {
    (void)env;
    panic_str(BURROW_S("die"));
}

static AddFn add_fn = {add, NULL};
static CountFn count_fn = {count, NULL};
static DddArgFn ddd_arg_fn = {ddd_arg, NULL};
static BoolFn die_fn = {die, NULL};
static EchoFn echo_fn = {echo, NULL};
static MakemapFn makemap_fn = {makemap, NULL};
static AnyFn map_of_three_fn = {map_of_three, NULL};
static StrStrFn one_arg_fn = {one_arg, NULL};
static IntFn return_int_fn = {return_int, NULL};
static StringerFn stringer_fn = {stringer, NULL};
static BinaryFn two_args_fn = {two_args, NULL};
static TypeOfFn type_of_fn = {type_of, NULL};
static StrStrFn value_string_fn = {value_string, NULL};
static VFn vfunc_fn = {vfunc, NULL};
static StrFn zero_args_fn = {zero_args, NULL};

static void add_func(Map *m, const char *name, Any f) {
    Str k = str_from_cstr(name);
    map_set(m, &k, &f);
}

/* The FuncMap testExecute gives every template. */
static Map *exec_funcs(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_OF(Any), 16);
    add_func(m, "add", BURROW_ANY(TYPE_OF(AddFn), &add_fn));
    add_func(m, "count", BURROW_ANY(TYPE_OF(CountFn), &count_fn));
    add_func(m, "dddArg", BURROW_ANY(TYPE_OF(DddArgFn), &ddd_arg_fn));
    add_func(m, "die", BURROW_ANY(TYPE_OF(BoolFn), &die_fn));
    add_func(m, "echo", BURROW_ANY(TYPE_OF(EchoFn), &echo_fn));
    add_func(m, "makemap", BURROW_ANY(TYPE_OF(MakemapFn), &makemap_fn));
    add_func(m, "mapOfThree", BURROW_ANY(TYPE_OF(AnyFn), &map_of_three_fn));
    add_func(m, "oneArg", BURROW_ANY(TYPE_OF(StrStrFn), &one_arg_fn));
    add_func(m, "returnInt", BURROW_ANY(TYPE_OF(IntFn), &return_int_fn));
    add_func(m, "stringer", BURROW_ANY(TYPE_OF(StringerFn), &stringer_fn));
    add_func(m, "twoArgs", BURROW_ANY(TYPE_OF(BinaryFn), &two_args_fn));
    add_func(m, "typeOf", BURROW_ANY(TYPE_OF(TypeOfFn), &type_of_fn));
    add_func(m, "valueString", BURROW_ANY(TYPE_OF(StrStrFn), &value_string_fn));
    add_func(m, "vfunc", BURROW_ANY(TYPE_OF(VFn), &vfunc_fn));
    add_func(m, "zeroArgs", BURROW_ANY(TYPE_OF(StrFn), &zero_args_fn));
    return m;
}

/* ------------------------------------------------------------- the data */

#define ANON_FIELDS(F, T)                                                              \
    F(T, Int, a, "")                                                                   \
    F(T, Str, b, "")
BURROW_STRUCT_DECL(Anon, ANON_FIELDS);
BURROW_STRUCT_DEFINE(Anon, ANON_FIELDS);

static T tval;
static T *tval_ptr = &tval;
static T tval_x;
static T *tval_x_ptr = &tval_x;
static T *tval_nil;
static Ifc si_val;
static Ifc ival;
static Ifc *ival_ptr = &ival;
static Str si_store[] = {S_("a"), S_("b")};
static Slice si_store_slice;
static U u_v = {S_("v")};
static U u_in_empty = {S_("UinEmpty")};
static U *u_in_empty_ptr = &u_in_empty;
static V v7777 = {7777};
static W w999 = {999};
static Int si_ints[] = {3, 4, 5};
static Int sicap_ints[10];
static bool sb_bools[] = {true, false};
static IntArray3 pai_array = {{3, 4, 5}};
static Map *smsi_maps[2];
static Int empty1 = 3;
static Str empty2 = S_("empty2");
static Int empty3_ints[] = {7, 8};
static Slice empty3;
static Buffer foozle = {S_("foozle")};
static Int pi_int = 23;
static Str ps_str = S_("a string");
static Int psi_ints[] = {21, 22, 23};
static Slice psi_slice;
static Int upi_int = 23;

static Map *map_s;
static Map *map_cause;
static Map *map22;
static T *tslice_nil_store[1];
static Slice tslice_nil;
static Int isl_ints[] = {-1, -2, -3};
static Slice isl;

static Map *make_str_int(Alloc *a, int n, const char *const *k, const Int *v) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, n);
    for (int i = 0; i < n; i++) {
        Str s = str_from_cstr(k[i]);
        map_set(m, &s, &v[i]);
    }
    return m;
}

/* A map from an integer type to Str, the key in a buffer of eight bytes. */
static Map *make_int_str(Alloc *a, const Type *kt, int n, const void *k, size_t ksize,
                         const char *const *v) {
    Map *m = map_make(a, kt, TYPE_STRING, n);
    for (int i = 0; i < n; i++) {
        Str s = str_from_cstr(v[i]);
        map_set(m, (const char *)k + (size_t)i * ksize, &s);
    }
    return m;
}

static void setup_tval(Alloc *a) {
    tval.True = true;
    tval.I = 17;
    tval.U16 = 16;
    tval.X = BURROW_S("x");
    tval.S = BURROW_S("xyz");
    tval.U = &u_v;
    tval.V0 = (V){6666};
    tval.V1 = &v7777;
    tval.W0 = (W){888};
    tval.W1 = &w999;
    tval.SI = slice_from(si_ints, 3, 3, TYPE_INT);
    tval.SICap = slice_from(sicap_ints, 5, 10, TYPE_INT);
    tval.SIEmpty = (Slice){NULL, 0, 0, TYPE_INT};
    tval.AI = (IntArray3){{3, 4, 5}};
    tval.PAI = &pai_array;
    tval.SB = slice_from(sb_bools, 2, 2, TYPE_BOOL);
    tval.MSI = make_str_int(a, 3, (const char *const[]){"one", "two", "three"},
                            (const Int[]){1, 2, 3});
    tval.MSIone = make_str_int(a, 1, (const char *const[]){"one"}, (const Int[]){1});
    tval.MXI = map_make(a, TYPE_OF(Any), TYPE_INT, 1);
    static Str one = BURROW_S_INIT("one");
    Any one_key = BURROW_ANY(TYPE_STRING, &one);
    Int one_val = 1;
    map_set(tval.MXI, &one_key, &one_val);
    tval.MII = map_make(a, TYPE_INT, TYPE_INT, 1);
    map_set(tval.MII, &one_val, &one_val);
    tval.MI32S = make_int_str(a, TYPE_OF(int32_t), 2, (const int32_t[]){1, 2},
                              sizeof(int32_t), (const char *const[]){"one", "two"});
    tval.MI64S = make_int_str(a, TYPE_OF(int64_t), 2, (const int64_t[]){2, 3},
                              sizeof(int64_t), (const char *const[]){"i642", "i643"});
    tval.MUI32S = make_int_str(a, TYPE_OF(uint32_t), 2, (const uint32_t[]){2, 3},
                               sizeof(uint32_t), (const char *const[]){"u322", "u323"});
    tval.MUI64S =
        make_int_str(a, TYPE_OF(uint64_t), 2, (const uint64_t[]){2, 3},
                     sizeof(uint64_t), (const char *const[]){"ui642", "ui643"});
    tval.MI8S = make_int_str(a, TYPE_OF(int8_t), 2, (const int8_t[]){2, 3},
                             sizeof(int8_t), (const char *const[]){"i82", "i83"});
    tval.MUI8S = make_int_str(a, TYPE_OF(uint8_t), 2, (const uint8_t[]){2, 3},
                              sizeof(uint8_t), (const char *const[]){"u82", "u83"});
    smsi_maps[0] =
        make_str_int(a, 2, (const char *const[]){"one", "two"}, (const Int[]){1, 2});
    smsi_maps[1] = make_str_int(a, 2, (const char *const[]){"eleven", "twelve"},
                                (const Int[]){11, 12});
    tval.SMSI = slice_from(smsi_maps, 2, 2, TYPE_OF(StrIntMap));
    tval.Empty1 = BURROW_ANY(TYPE_INT, &empty1);
    tval.Empty2 = BURROW_ANY(TYPE_STRING, &empty2);
    empty3 = slice_from(empty3_ints, 2, 2, TYPE_INT);
    tval.Empty3 = BURROW_ANY(TYPE_OF(IntSlice), &empty3);
    tval.Empty4 = BURROW_ANY(TYPE_OF(UPtr), &u_in_empty_ptr);
    tval_x.X = BURROW_S("x");
    tval.NonEmptyInterface = (Ifc){&tptr_ifc_vt, &tval_x_ptr};
    si_store_slice = slice_from(si_store, 2, 2, TYPE_STRING);
    si_val = (Ifc){&s_ifc_vt, &si_store_slice};
    tval.NonEmptyInterfacePtS = &si_val;
    tval.NonEmptyInterfaceTypedNil = (Ifc){&tptr_ifc_vt, &tval_nil};
    tval.Str = (Stringer){&buffer_stringer_vt, &foozle};
    tval.Err = errors_new(a, BURROW_S("erroozle"));
    tval.PI = &pi_int;
    tval.PS = &ps_str;
    psi_slice = slice_from(psi_ints, 3, 3, TYPE_INT);
    tval.PSI = &psi_slice;
    tval.UPI = &upi_int;
    tval.BinaryFunc = (BinaryFn){binary_func, NULL};
    tval.VariadicFunc = (VariadicFn){variadic_func, NULL};
    tval.VariadicFuncInt = (VariadicIntFn){variadic_func_int, NULL};
    tval.NilOKFunc = (NilOkFn){nil_ok_func, NULL};
    tval.ErrFunc = (ErrFn){err_func, NULL};
    tval.PanicFunc = (StrFn){panic_func, NULL};
    tval.TooFewReturnCountFunc = (VoidFn){void_func, NULL};
    tval.TooManyReturnCountFunc = (VoidFn){void_func, NULL};
    tval.InvalidReturnTypeFunc = (VoidFn){void_func, NULL};
    ival = (Ifc){&tptr_ifc_vt, &tval_ptr};
}

static void setup(Alloc *a) {
    setup_tval(a);
    three_map = make_str_int(a, 1, (const char *const[]){"three"}, (const Int[]){3});
    map22 = make_str_int(a, 1, (const char *const[]){"two"}, (const Int[]){22});
    map_s = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
    Str k = BURROW_S("S");
    Any v = BURROW_ANY(TYPE_OF(Buffer), &foozle);
    map_set(map_s, &k, &v);
    map_cause = map_make(a, TYPE_STRING, TYPE_STRING, 1);
    Str ck = BURROW_S("cause");
    Str cv = BURROW_S("neglect");
    map_set(map_cause, &ck, &cv);
    tslice_nil = slice_from(tslice_nil_store, 1, 1, TYPE_OF(TPtr));
    isl = slice_from(isl_ints, 3, 3, TYPE_INT);
}

/* What the data column of a table names. */
enum {
    D_NIL,
    D_ZERO,
    D_TVAL,
    D_13,
    D_U14,
    D_F15,
    D_TRUE,
    D_C16,
    D_HELLO,
    D_ISLICE,
    D_MAP22,
    D_ANON,
    D_123,
    D_TSLICE_NIL,
    D_MAP_S,
    D_JS,
    D_I8,
    D_I16,
    D_I32,
    D_I64,
    D_INT5,
    D_U8,
    D_U16,
    D_U32,
    D_U64,
    D_UINT5,
    D_UINTPTR,
    D_UINTPTR0,
    D_IVAL_PTR,
    D_CAUSE,
    D_TZERO,
    D_RUNE_DOT,
    D_RUNE_E,
    D_RUNE_P,
    D_JOE
};

static Int int_vals[] = {0, 13, 123, 5};
static Uint u14 = 14;
static double f15 = 15.1;
static bool b_true = true;
static Complex128 c16 = {16.2, -17};
static Str s_hello = S_("hello");
static Anon anon = {7, S_("seven")};
static Str s_js = S_("It'd be nice.");
static Str s_joe = S_("joe");
static int8_t i8_5 = 5;
static int16_t i16_5 = 5;
static int32_t i32_5 = 5;
static int64_t i64_5 = 5;
static uint8_t u8_5 = 5;
static uint16_t u16_5 = 5;
static uint32_t u32_5 = 5;
static uint64_t u64_5 = 5;
static Uint uint_5 = 5;
static Uintptr uintptr_5 = 5;
static Uintptr uintptr_0 = 0;
static T tzero;
static int32_t runes[] = {'.', 'e', 'P'};

static Any data_of(int d) {
    switch (d) {
    case D_ZERO:
        return BURROW_ANY(TYPE_INT, &int_vals[0]);
    case D_TVAL:
        return BURROW_ANY(TYPE_OF(TPtr), &tval_ptr);
    case D_13:
        return BURROW_ANY(TYPE_INT, &int_vals[1]);
    case D_U14:
        return BURROW_ANY(TYPE_OF(Uint), &u14);
    case D_F15:
        return BURROW_ANY(TYPE_OF(double), &f15);
    case D_TRUE:
        return BURROW_ANY(TYPE_BOOL, &b_true);
    case D_C16:
        return BURROW_ANY(TYPE_OF(Complex128), &c16);
    case D_HELLO:
        return BURROW_ANY(TYPE_STRING, &s_hello);
    case D_ISLICE:
        return BURROW_ANY(TYPE_OF(IntSlice), &isl);
    case D_MAP22:
        return BURROW_ANY(TYPE_OF(StrIntMap), &map22);
    case D_ANON:
        return BURROW_ANY(TYPE_OF(Anon), &anon);
    case D_123:
        return BURROW_ANY(TYPE_INT, &int_vals[2]);
    case D_TSLICE_NIL:
        return BURROW_ANY(TYPE_OF(TPtrSlice), &tslice_nil);
    case D_MAP_S:
        return BURROW_ANY(TYPE_OF(StrAnyMap), &map_s);
    case D_JS:
        return BURROW_ANY(TYPE_STRING, &s_js);
    case D_I8:
        return BURROW_ANY(TYPE_OF(int8_t), &i8_5);
    case D_I16:
        return BURROW_ANY(TYPE_OF(int16_t), &i16_5);
    case D_I32:
        return BURROW_ANY(TYPE_OF(int32_t), &i32_5);
    case D_I64:
        return BURROW_ANY(TYPE_OF(int64_t), &i64_5);
    case D_INT5:
        return BURROW_ANY(TYPE_INT, &int_vals[3]);
    case D_U8:
        return BURROW_ANY(TYPE_OF(uint8_t), &u8_5);
    case D_U16:
        return BURROW_ANY(TYPE_OF(uint16_t), &u16_5);
    case D_U32:
        return BURROW_ANY(TYPE_OF(uint32_t), &u32_5);
    case D_U64:
        return BURROW_ANY(TYPE_OF(uint64_t), &u64_5);
    case D_UINT5:
        return BURROW_ANY(TYPE_OF(Uint), &uint_5);
    case D_UINTPTR:
        return BURROW_ANY(TYPE_OF(Uintptr), &uintptr_5);
    case D_UINTPTR0:
        return BURROW_ANY(TYPE_OF(Uintptr), &uintptr_0);
    case D_IVAL_PTR:
        return BURROW_ANY(TYPE_OF(IfcPtr), &ival_ptr);
    case D_CAUSE:
        return BURROW_ANY(TYPE_OF(StrStrMap), &map_cause);
    case D_TZERO:
        return BURROW_ANY(TYPE_OF(T), &tzero);
    case D_RUNE_DOT:
        return BURROW_ANY(TYPE_OF(int32_t), &runes[0]);
    case D_RUNE_E:
        return BURROW_ANY(TYPE_OF(int32_t), &runes[1]);
    case D_RUNE_P:
        return BURROW_ANY(TYPE_OF(int32_t), &runes[2]);
    case D_JOE:
        return BURROW_ANY(TYPE_STRING, &s_joe);
    default:
        return NIL_ANY;
    }
}

/* ------------------------------------------------------------ the tables */

typedef struct ExecTest {
    const char *name;
    const char *input;
    const char *output;
    int data;
    bool ok;
} ExecTest;

/* rangeTestInt and rangeTestData, which Go makes with a generic function. */
#define RANGE_TEST_INT "{{range $v := .}}{{printf \"%T%d\" $v $v}}{{end}}"
#define RT(t) t "0" t "1" t "2" t "3" t "4"

static const ExecTest exec_tests[] = {
    {"empty", "", "", D_NIL, true},
    {"text", "some text", "some text", D_NIL, true},
    {"nil action", "{{nil}}", "", D_NIL, false},
    {"ideal int", "{{typeOf 3}}", "int", D_ZERO, true},
    {"ideal float", "{{typeOf 1.0}}", "float64", D_ZERO, true},
    {"ideal exp float", "{{typeOf 1e1}}", "float64", D_ZERO, true},
    {"ideal complex", "{{typeOf 1i}}", "complex128", D_ZERO, true},
    {"ideal int",
     "{{typeOf "
     "BIGINT"
     "}}",
     "int", D_ZERO, true},
    {"ideal too big",
     "{{typeOf "
     "BIGUINT"
     "}}",
     "", D_ZERO, false},
    {"ideal nil without type", "{{nil}}", "", D_ZERO, false},
    {".X", "-{{.X}}-", "-x-", D_TVAL, true},
    {".U.V", "-{{.U.V}}-", "-v-", D_TVAL, true},
    {".unexported", "{{.unexported}}", "", D_TVAL, false},
    {"map .one", "{{.MSI.one}}", "1", D_TVAL, true},
    {"map .two", "{{.MSI.two}}", "2", D_TVAL, true},
    {"map .NO", "{{.MSI.NO}}", "<no value>", D_TVAL, true},
    {"map .one interface", "{{.MXI.one}}", "1", D_TVAL, true},
    {"map .WRONG args", "{{.MSI.one 1}}", "", D_TVAL, false},
    {"map .WRONG type", "{{.MII.one}}", "", D_TVAL, false},
    {"dot int", "<{{.}}>", "<13>", D_13, true},
    {"dot uint", "<{{.}}>", "<14>", D_U14, true},
    {"dot float", "<{{.}}>", "<15.1>", D_F15, true},
    {"dot bool", "<{{.}}>", "<true>", D_TRUE, true},
    {"dot complex", "<{{.}}>", "<(16.2-17i)>", D_C16, true},
    {"dot string", "<{{.}}>", "<hello>", D_HELLO, true},
    {"dot slice", "<{{.}}>", "<[-1 -2 -3]>", D_ISLICE, true},
    {"dot map", "<{{.}}>", "<map[two:22]>", D_MAP22, true},
    {"dot struct", "<{{.}}>", "<{7 seven}>", D_ANON, true},
    {"$ int", "{{$}}", "123", D_123, true},
    {"$.I", "{{$.I}}", "17", D_TVAL, true},
    {"$.U.V", "{{$.U.V}}", "v", D_TVAL, true},
    {"declare in action", "{{$x := $.U.V}}{{$x}}", "v", D_TVAL, true},
    {"simple assignment", "{{$x := 2}}{{$x = 3}}{{$x}}", "3", D_TVAL, true},
    {"nested assignment", "{{$x := 2}}{{if true}}{{$x = 3}}{{end}}{{$x}}", "3", D_TVAL,
     true},
    {"nested assignment changes the last declaration",
     "{{$x := 1}}{{if true}}{{$x := 2}}{{if true}}{{$x = 3}}{{end}}{{end}}{{$x}}", "1",
     D_TVAL, true},
    {"V{6666}.String()", "-{{.V0}}-", "-<6666>-", D_TVAL, true},
    {"&V{7777}.String()", "-{{.V1}}-", "-<7777>-", D_TVAL, true},
    {"(*V)(nil).String()", "-{{.V2}}-", "-<nil>-", D_TVAL, true},
    {"W{888}.Error()", "-{{.W0}}-", "-[888]-", D_TVAL, true},
    {"&W{999}.Error()", "-{{.W1}}-", "-[999]-", D_TVAL, true},
    {"(*W)(nil).Error()", "-{{.W2}}-", "-<nil>-", D_TVAL, true},
    {"*int", "{{.PI}}", "23", D_TVAL, true},
    {"*string", "{{.PS}}", "a string", D_TVAL, true},
    {"*[]int", "{{.PSI}}", "[21 22 23]", D_TVAL, true},
    {"*[]int[1]", "{{index .PSI 1}}", "22", D_TVAL, true},
    {"NIL", "{{.NIL}}", "<nil>", D_TVAL, true},
    {"empty nil", "{{.Empty0}}", "<no value>", D_TVAL, true},
    {"empty with int", "{{.Empty1}}", "3", D_TVAL, true},
    {"empty with string", "{{.Empty2}}", "empty2", D_TVAL, true},
    {"empty with slice", "{{.Empty3}}", "[7 8]", D_TVAL, true},
    {"empty with struct", "{{.Empty4}}", "{UinEmpty}", D_TVAL, true},
    {"empty with struct, field", "{{.Empty4.V}}", "UinEmpty", D_TVAL, true},
    {"field on interface", "{{.foo}}", "<no value>", D_NIL, true},
    {"field on parenthesized interface", "{{(.).foo}}", "<no value>", D_NIL, true},
    {"unparenthesized non-function", "{{1 2}}", "", D_NIL, false},
    {"parenthesized non-function", "{{(1) 2}}", "", D_NIL, false},
    {"parenthesized non-function with no args", "{{(1)}}", "1", D_NIL, true},
    {".Method0", "-{{.Method0}}-", "-M0-", D_TVAL, true},
    {".Method1(1234)", "-{{.Method1 1234}}-", "-1234-", D_TVAL, true},
    {".Method1(.I)", "-{{.Method1 .I}}-", "-17-", D_TVAL, true},
    {".Method2(3, .X)", "-{{.Method2 3 .X}}-", "-Method2: 3 x-", D_TVAL, true},
    {".Method2(.U16, `str`)", "-{{.Method2 .U16 `str`}}-", "-Method2: 16 str-", D_TVAL,
     true},
    {".Method2(.U16, $x)", "{{if $x := .X}}-{{.Method2 .U16 $x}}{{end}}-",
     "-Method2: 16 x-", D_TVAL, true},
    {".Method3(nil constant)", "-{{.Method3 nil}}-", "-Method3: <nil>-", D_TVAL, true},
    {".Method3(nil value)", "-{{.Method3 .MXI.unset}}-", "-Method3: <nil>-", D_TVAL,
     true},
    {"method on var", "{{if $x := .}}-{{$x.Method2 .U16 $x.X}}{{end}}-",
     "-Method2: 16 x-", D_TVAL, true},
    {"method on chained var",
     "{{range .MSIone}}{{if $.U.TrueFalse $.True}}{{$.U.TrueFalse "
     "$.True}}{{else}}WRONG{{end}}{{end}}",
     "true", D_TVAL, true},
    {"chained method",
     "{{range .MSIone}}{{if $.GetU.TrueFalse $.True}}{{$.U.TrueFalse "
     "$.True}}{{else}}WRONG{{end}}{{end}}",
     "true", D_TVAL, true},
    {"chained method on variable",
     "{{with $x := .}}{{with .SI}}{{$.GetU.TrueFalse $.True}}{{end}}{{end}}", "true",
     D_TVAL, true},
    {".NilOKFunc not nil", "{{call .NilOKFunc .PI}}", "false", D_TVAL, true},
    {".NilOKFunc nil", "{{call .NilOKFunc nil}}", "true", D_TVAL, true},
    {"method on nil value from slice", "-{{range .}}{{.Method1 1234}}{{end}}-",
     "-1234-", D_TSLICE_NIL, true},
    {"method on typed nil interface value", "{{.NonEmptyInterfaceTypedNil.Method0}}",
     "M0", D_TVAL, true},
    {".BinaryFunc", "{{call .BinaryFunc `1` `2`}}", "[1=2]", D_TVAL, true},
    {".VariadicFunc0", "{{call .VariadicFunc}}", "<>", D_TVAL, true},
    {".VariadicFunc2", "{{call .VariadicFunc `he` `llo`}}", "<he+llo>", D_TVAL, true},
    {".VariadicFuncInt", "{{call .VariadicFuncInt 33 `he` `llo`}}", "33=<he+llo>",
     D_TVAL, true},
    {"if .BinaryFunc call", "{{ if .BinaryFunc}}{{call .BinaryFunc `1` `2`}}{{end}}",
     "[1=2]", D_TVAL, true},
    {"if not .BinaryFunc call",
     "{{ if not .BinaryFunc}}{{call .BinaryFunc `1` `2`}}{{else}}No{{end}}", "No",
     D_TVAL, true},
    {"Interface Call", "{{stringer .S}}", "foozle", D_MAP_S, true},
    {".ErrFunc", "{{call .ErrFunc}}", "bla", D_TVAL, true},
    {"call nil", "{{call nil}}", "", D_TVAL, false},
    {"empty call", "{{call}}", "", D_TVAL, false},
    {"empty call after pipe valid", "{{.ErrFunc | call}}", "bla", D_TVAL, true},
    {"empty call after pipe invalid", "{{1 | call}}", "", D_TVAL, false},
    {".BinaryFuncTooFew", "{{call .BinaryFunc `1`}}", "", D_TVAL, false},
    {".BinaryFuncTooMany", "{{call .BinaryFunc `1` `2` `3`}}", "", D_TVAL, false},
    {".BinaryFuncBad0", "{{call .BinaryFunc 1 3}}", "", D_TVAL, false},
    {".BinaryFuncBad1", "{{call .BinaryFunc `1` 3}}", "", D_TVAL, false},
    {".VariadicFuncBad0", "{{call .VariadicFunc 3}}", "", D_TVAL, false},
    {".VariadicFuncIntBad0", "{{call .VariadicFuncInt}}", "", D_TVAL, false},
    {".VariadicFuncIntBad`", "{{call .VariadicFuncInt `x`}}", "", D_TVAL, false},
    {".VariadicFuncNilBad", "{{call .VariadicFunc nil}}", "", D_TVAL, false},
    {"pipeline", "-{{.Method0 | .Method2 .U16}}-", "-Method2: 16 M0-", D_TVAL, true},
    {"pipeline func", "-{{call .VariadicFunc `llo` | call .VariadicFunc `he` }}-",
     "-<he+<llo>>-", D_TVAL, true},
    {"nil pipeline", "{{ .Empty0 | call .NilOKFunc }}", "true", D_TVAL, true},
    {"nil call arg", "{{ call .NilOKFunc .Empty0 }}", "true", D_TVAL, true},
    {"bad nil pipeline", "{{ .Empty0 | .VariadicFunc }}", "", D_TVAL, false},
    {"parens in pipeline", "{{printf `%d %d %d` (1) (2 | add 3) (add 4 (add 5 6))}}",
     "1 5 15", D_TVAL, true},
    {"parens: $ in paren", "{{($).X}}", "x", D_TVAL, true},
    {"parens: $.GetU in paren", "{{($.GetU).V}}", "v", D_TVAL, true},
    {"parens: $ in paren in pipe", "{{($ | echo).X}}", "x", D_TVAL, true},
    {"parens: spaces and args", "{{(makemap \"up\" \"down\" \"left\" \"right\").left}}",
     "right", D_TVAL, true},
    {"if true", "{{if true}}TRUE{{end}}", "TRUE", D_TVAL, true},
    {"if false", "{{if false}}TRUE{{else}}FALSE{{end}}", "FALSE", D_TVAL, true},
    {"if nil", "{{if nil}}TRUE{{end}}", "", D_TVAL, false},
    {"if on typed nil interface value",
     "{{if .NonEmptyInterfaceTypedNil}}TRUE{{ end }}", "", D_TVAL, true},
    {"if 1", "{{if 1}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D_TVAL, true},
    {"if 0", "{{if 0}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"if 1.5", "{{if 1.5}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D_TVAL, true},
    {"if 0.0", "{{if .FloatZero}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"if 1.5i", "{{if 1.5i}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D_TVAL, true},
    {"if 0.0i", "{{if .ComplexZero}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"if nonNilPointer", "{{if .PI}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D_TVAL,
     true},
    {"if nilPointer", "{{if .NIL}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"if UPI", "{{if .UPI}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D_TVAL, true},
    {"if EmptyUPI", "{{if .EmptyUPI}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL,
     true},
    {"if emptystring", "{{if ``}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D_TVAL, true},
    {"if string", "{{if `notempty`}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D_TVAL,
     true},
    {"if emptyslice", "{{if .SIEmpty}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D_TVAL,
     true},
    {"if slice", "{{if .SI}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D_TVAL, true},
    {"if emptymap", "{{if .MSIEmpty}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D_TVAL,
     true},
    {"if map", "{{if .MSI}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D_TVAL, true},
    {"if map unset", "{{if .MXI.none}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D_TVAL,
     true},
    {"if map not unset", "{{if not .MXI.none}}ZERO{{else}}NON-ZERO{{end}}", "ZERO",
     D_TVAL, true},
    {"if $x with $y int",
     "{{if $x := true}}{{with $y := .I}}{{$x}},{{$y}}{{end}}{{end}}", "true,17", D_TVAL,
     true},
    {"if $x with $x int",
     "{{if $x := true}}{{with $x := .I}}{{$x}},{{end}}{{$x}}{{end}}", "17,true", D_TVAL,
     true},
    {"if else if", "{{if false}}FALSE{{else if true}}TRUE{{end}}", "TRUE", D_TVAL,
     true},
    {"if else chain", "{{if eq 1 3}}1{{else if eq 2 3}}2{{else if eq 3 3}}3{{end}}",
     "3", D_TVAL, true},
    {"print", "{{print \"hello, print\"}}", "hello, print", D_TVAL, true},
    {"print 123", "{{print 1 2 3}}", "1 2 3", D_TVAL, true},
    {"print nil", "{{print nil}}", "<nil>", D_TVAL, true},
    {"println", "{{println 1 2 3}}", "1 2 3\n", D_TVAL, true},
    {"printf int", "{{printf \"%04x\" 127}}", "007f", D_TVAL, true},
    {"printf float", "{{printf \"%g\" 3.5}}", "3.5", D_TVAL, true},
    {"printf complex", "{{printf \"%g\" 1+7i}}", "(1+7i)", D_TVAL, true},
    {"printf string", "{{printf \"%s\" \"hello\"}}", "hello", D_TVAL, true},
    {"printf function", "{{printf \"%#q\" zeroArgs}}", "`zeroArgs`", D_TVAL, true},
    {"printf field", "{{printf \"%s\" .U.V}}", "v", D_TVAL, true},
    {"printf method", "{{printf \"%s\" .Method0}}", "M0", D_TVAL, true},
    {"printf dot", "{{with .I}}{{printf \"%d\" .}}{{end}}", "17", D_TVAL, true},
    {"printf var", "{{with $x := .I}}{{printf \"%d\" $x}}{{end}}", "17", D_TVAL, true},
    {"printf lots", "{{printf \"%d %s %g %s\" 127 \"hello\" 7-3i .Method0}}",
     "127 hello (7-3i) M0", D_TVAL, true},
    {"html", "{{html \"<script>alert(\\\"XSS\\\");</script>\"}}",
     "&lt;script&gt;alert(&#34;XSS&#34;);&lt;/script&gt;", D_NIL, true},
    {"html pipeline", "{{printf \"<script>alert(\\\"XSS\\\");</script>\" | html}}",
     "&lt;script&gt;alert(&#34;XSS&#34;);&lt;/script&gt;", D_NIL, true},
    {"html", "{{html .PS}}", "a string", D_TVAL, true},
    {"html typed nil", "{{html .NIL}}", "&lt;nil&gt;", D_TVAL, true},
    {"html untyped nil", "{{html .Empty0}}", "&lt;no value&gt;", D_TVAL, true},
    {"js", "{{js .}}", "It\\'d be nice.", D_JS, true},
    {"urlquery", "{{\"http://www.example.org/\"|urlquery}}",
     "http%3A%2F%2Fwww.example.org%2F", D_NIL, true},
    {"not", "{{not true}} {{not false}}", "false true", D_NIL, true},
    {"and", "{{and false 0}} {{and 1 0}} {{and 0 true}} {{and 1 1}}", "false 0 0 1",
     D_NIL, true},
    {"or", "{{or 0 0}} {{or 1 0}} {{or 0 true}} {{or 1 1}}", "0 1 true 1", D_NIL, true},
    {"or short-circuit", "{{or 0 1 (die)}}", "1", D_NIL, true},
    {"and short-circuit", "{{and 1 0 (die)}}", "0", D_NIL, true},
    {"or short-circuit2", "{{or 0 0 (die)}}", "", D_NIL, false},
    {"and short-circuit2", "{{and 1 1 (die)}}", "", D_NIL, false},
    {"and pipe-true", "{{1 | and 1}}", "1", D_NIL, true},
    {"and pipe-false", "{{0 | and 1}}", "0", D_NIL, true},
    {"or pipe-true", "{{1 | or 0}}", "1", D_NIL, true},
    {"or pipe-false", "{{0 | or 0}}", "0", D_NIL, true},
    {"and undef", "{{and 1 .Unknown}}", "<no value>", D_NIL, true},
    {"or undef", "{{or 0 .Unknown}}", "<no value>", D_NIL, true},
    {"boolean if", "{{if and true 1 `hi`}}TRUE{{else}}FALSE{{end}}", "TRUE", D_TVAL,
     true},
    {"boolean if not", "{{if and true 1 `hi` | not}}TRUE{{else}}FALSE{{end}}", "FALSE",
     D_NIL, true},
    {"boolean if pipe", "{{if true | not | and 1}}TRUE{{else}}FALSE{{end}}", "FALSE",
     D_NIL, true},
    {"slice[0]", "{{index .SI 0}}", "3", D_TVAL, true},
    {"slice[1]", "{{index .SI 1}}", "4", D_TVAL, true},
    {"slice[HUGE]", "{{index .SI 10}}", "", D_TVAL, false},
    {"slice[WRONG]", "{{index .SI `hello`}}", "", D_TVAL, false},
    {"slice[nil]", "{{index .SI nil}}", "", D_TVAL, false},
    {"map[one]", "{{index .MSI `one`}}", "1", D_TVAL, true},
    {"map[two]", "{{index .MSI `two`}}", "2", D_TVAL, true},
    {"map[NO]", "{{index .MSI `XXX`}}", "0", D_TVAL, true},
    {"map[nil]", "{{index .MSI nil}}", "", D_TVAL, false},
    {"map[``]", "{{index .MSI ``}}", "0", D_TVAL, true},
    {"map[WRONG]", "{{index .MSI 10}}", "", D_TVAL, false},
    {"double index", "{{index .SMSI 1 `eleven`}}", "11", D_TVAL, true},
    {"nil[1]", "{{index nil 1}}", "", D_TVAL, false},
    {"map MI64S", "{{index .MI64S 2}}", "i642", D_TVAL, true},
    {"map MI32S", "{{index .MI32S 2}}", "two", D_TVAL, true},
    {"map MUI64S", "{{index .MUI64S 3}}", "ui643", D_TVAL, true},
    {"map MI8S", "{{index .MI8S 3}}", "i83", D_TVAL, true},
    {"map MUI8S", "{{index .MUI8S 2}}", "u82", D_TVAL, true},
    {"index of an interface field", "{{index .Empty3 0}}", "7", D_TVAL, true},
    {"slice[:]", "{{slice .SI}}", "[3 4 5]", D_TVAL, true},
    {"slice[1:]", "{{slice .SI 1}}", "[4 5]", D_TVAL, true},
    {"slice[1:2]", "{{slice .SI 1 2}}", "[4]", D_TVAL, true},
    {"slice[-1:]", "{{slice .SI -1}}", "", D_TVAL, false},
    {"slice[1:-2]", "{{slice .SI 1 -2}}", "", D_TVAL, false},
    {"slice[1:2:-1]", "{{slice .SI 1 2 -1}}", "", D_TVAL, false},
    {"slice[2:1]", "{{slice .SI 2 1}}", "", D_TVAL, false},
    {"slice[2:2:1]", "{{slice .SI 2 2 1}}", "", D_TVAL, false},
    {"out of range", "{{slice .SI 4 5}}", "", D_TVAL, false},
    {"out of range", "{{slice .SI 2 2 5}}", "", D_TVAL, false},
    {"len(s) < indexes < cap(s)", "{{slice .SICap 6 10}}", "[0 0 0 0]", D_TVAL, true},
    {"len(s) < indexes < cap(s)", "{{slice .SICap 6 10 10}}", "[0 0 0 0]", D_TVAL,
     true},
    {"indexes > cap(s)", "{{slice .SICap 10 11}}", "", D_TVAL, false},
    {"indexes > cap(s)", "{{slice .SICap 6 10 11}}", "", D_TVAL, false},
    {"array[:]", "{{slice .AI}}", "[3 4 5]", D_TVAL, true},
    {"array[1:]", "{{slice .AI 1}}", "[4 5]", D_TVAL, true},
    {"array[1:2]", "{{slice .AI 1 2}}", "[4]", D_TVAL, true},
    {"pointer to array[:]", "{{slice .PAI}}", "[3 4 5]", D_TVAL, true},
    {"pointer to array[1:]", "{{slice .PAI 1}}", "[4 5]", D_TVAL, true},
    {"pointer to array[1:2]", "{{slice .PAI 1 2}}", "[4]", D_TVAL, true},
    {"string[:]", "{{slice .S}}", "xyz", D_TVAL, true},
    {"string[0:1]", "{{slice .S 0 1}}", "x", D_TVAL, true},
    {"string[1:]", "{{slice .S 1}}", "yz", D_TVAL, true},
    {"string[1:2]", "{{slice .S 1 2}}", "y", D_TVAL, true},
    {"out of range", "{{slice .S 1 5}}", "", D_TVAL, false},
    {"3-index slice of string", "{{slice .S 1 2 2}}", "", D_TVAL, false},
    {"slice of an interface field", "{{slice .Empty3 0 1}}", "[7]", D_TVAL, true},
    {"slice", "{{len .SI}}", "3", D_TVAL, true},
    {"map", "{{len .MSI }}", "3", D_TVAL, true},
    {"len of int", "{{len 3}}", "", D_TVAL, false},
    {"len of nothing", "{{len .Empty0}}", "", D_TVAL, false},
    {"len of an interface field", "{{len .Empty3}}", "2", D_TVAL, true},
    {"with true", "{{with true}}{{.}}{{end}}", "true", D_TVAL, true},
    {"with false", "{{with false}}{{.}}{{else}}FALSE{{end}}", "FALSE", D_TVAL, true},
    {"with 1", "{{with 1}}{{.}}{{else}}ZERO{{end}}", "1", D_TVAL, true},
    {"with 0", "{{with 0}}{{.}}{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"with 1.5", "{{with 1.5}}{{.}}{{else}}ZERO{{end}}", "1.5", D_TVAL, true},
    {"with 0.0", "{{with .FloatZero}}{{.}}{{else}}ZERO{{end}}", "ZERO", D_TVAL, true},
    {"with 1.5i", "{{with 1.5i}}{{.}}{{else}}ZERO{{end}}", "(0+1.5i)", D_TVAL, true},
    {"with 0.0i", "{{with .ComplexZero}}{{.}}{{else}}ZERO{{end}}", "ZERO", D_TVAL,
     true},
    {"with emptystring", "{{with ``}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D_TVAL, true},
    {"with string", "{{with `notempty`}}{{.}}{{else}}EMPTY{{end}}", "notempty", D_TVAL,
     true},
    {"with emptyslice", "{{with .SIEmpty}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D_TVAL,
     true},
    {"with slice", "{{with .SI}}{{.}}{{else}}EMPTY{{end}}", "[3 4 5]", D_TVAL, true},
    {"with emptymap", "{{with .MSIEmpty}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D_TVAL,
     true},
    {"with map", "{{with .MSIone}}{{.}}{{else}}EMPTY{{end}}", "map[one:1]", D_TVAL,
     true},
    {"with empty interface, struct field", "{{with .Empty4}}{{.V}}{{end}}", "UinEmpty",
     D_TVAL, true},
    {"with $x int", "{{with $x := .I}}{{$x}}{{end}}", "17", D_TVAL, true},
    {"with $x struct.U.V", "{{with $x := $}}{{$x.U.V}}{{end}}", "v", D_TVAL, true},
    {"with variable and action", "{{with $x := $}}{{$y := $.U.V}}{{$y}}{{end}}", "v",
     D_TVAL, true},
    {"with on typed nil interface value",
     "{{with .NonEmptyInterfaceTypedNil}}TRUE{{ end }}", "", D_TVAL, true},
    {"with else with", "{{with 0}}{{.}}{{else with true}}{{.}}{{end}}", "true", D_TVAL,
     true},
    {"with else with chain",
     "{{with 0}}{{.}}{{else with false}}{{.}}{{else with `notempty`}}{{.}}{{end}}",
     "notempty", D_TVAL, true},
    {"range []int", "{{range .SI}}-{{.}}-{{end}}", "-3--4--5-", D_TVAL, true},
    {"range empty no else", "{{range .SIEmpty}}-{{.}}-{{end}}", "", D_TVAL, true},
    {"range []int else", "{{range .SI}}-{{.}}-{{else}}EMPTY{{end}}", "-3--4--5-",
     D_TVAL, true},
    {"range empty else", "{{range .SIEmpty}}-{{.}}-{{else}}EMPTY{{end}}", "EMPTY",
     D_TVAL, true},
    {"range []int break else",
     "{{range .SI}}-{{.}}-{{break}}NOTREACHED{{else}}EMPTY{{end}}", "-3-", D_TVAL,
     true},
    {"range []int continue else",
     "{{range .SI}}-{{.}}-{{continue}}NOTREACHED{{else}}EMPTY{{end}}", "-3--4--5-",
     D_TVAL, true},
    {"range []bool", "{{range .SB}}-{{.}}-{{end}}", "-true--false-", D_TVAL, true},
    {"range []int method", "{{range .SI | .MAdd .I}}-{{.}}-{{end}}", "-20--21--22-",
     D_TVAL, true},
    {"range map", "{{range .MSI}}-{{.}}-{{end}}", "-1--3--2-", D_TVAL, true},
    {"range empty map no else", "{{range .MSIEmpty}}-{{.}}-{{end}}", "", D_TVAL, true},
    {"range map else", "{{range .MSI}}-{{.}}-{{else}}EMPTY{{end}}", "-1--3--2-", D_TVAL,
     true},
    {"range empty map else", "{{range .MSIEmpty}}-{{.}}-{{else}}EMPTY{{end}}", "EMPTY",
     D_TVAL, true},
    {"range empty interface", "{{range .Empty3}}-{{.}}-{{else}}EMPTY{{end}}", "-7--8-",
     D_TVAL, true},
    {"range empty nil", "{{range .Empty0}}-{{.}}-{{end}}", "", D_TVAL, true},
    {"range $x SI", "{{range $x := .SI}}<{{$x}}>{{end}}", "<3><4><5>", D_TVAL, true},
    {"range $x $y SI", "{{range $x, $y := .SI}}<{{$x}}={{$y}}>{{end}}",
     "<0=3><1=4><2=5>", D_TVAL, true},
    {"range $x MSIone", "{{range $x := .MSIone}}<{{$x}}>{{end}}", "<1>", D_TVAL, true},
    {"range $x $y MSIone", "{{range $x, $y := .MSIone}}<{{$x}}={{$y}}>{{end}}",
     "<one=1>", D_TVAL, true},
    {"range $x PSI", "{{range $x := .PSI}}<{{$x}}>{{end}}", "<21><22><23>", D_TVAL,
     true},
    {"declare in range", "{{range $x := .PSI}}<{{$foo:=$x}}{{$x}}>{{end}}",
     "<21><22><23>", D_TVAL, true},
    {"range count", "{{range $i, $x := count 5}}[{{$i}}]{{$x}}{{end}}",
     "[0]a[1]b[2]c[3]d[4]e", D_TVAL, true},
    {"range nil count", "{{range $i, $x := count 0}}{{else}}empty{{end}}", "empty",
     D_TVAL, true},
    {"range int8", RANGE_TEST_INT, RT("int8"), D_I8, true},
    {"range int16", RANGE_TEST_INT, RT("int16"), D_I16, true},
    {"range int32", RANGE_TEST_INT, RT("int32"), D_I32, true},
    {"range int64", RANGE_TEST_INT, RT("int64"), D_I64, true},
    {"range int", RANGE_TEST_INT, RT("int"), D_INT5, true},
    {"range uint8", RANGE_TEST_INT, RT("uint8"), D_U8, true},
    {"range uint16", RANGE_TEST_INT, RT("uint16"), D_U16, true},
    {"range uint32", RANGE_TEST_INT, RT("uint32"), D_U32, true},
    {"range uint64", RANGE_TEST_INT, RT("uint64"), D_U64, true},
    {"range uint", RANGE_TEST_INT, RT("uint"), D_UINT5, true},
    {"range uintptr", RANGE_TEST_INT, RT("uintptr"), D_UINTPTR, true},
    {"range uintptr(0)", "{{range $v := .}}{{print $v}}{{else}}empty{{end}}", "empty",
     D_UINTPTR0, true},
    {"range 5", "{{range $v := 5}}{{printf \"%T%d\" $v $v}}{{end}}", RT("int"), D_NIL,
     true},
    {"or as if true", "{{or .SI \"slice is empty\"}}", "[3 4 5]", D_TVAL, true},
    {"or as if false", "{{or .SIEmpty \"slice is empty\"}}", "slice is empty", D_TVAL,
     true},
    {"error method, error", "{{.MyError true}}", "", D_TVAL, false},
    {"error method, no error", "{{.MyError false}}", "false", D_TVAL, true},
    {"decimal", "{{print 1234}}", "1234", D_TVAL, true},
    {"decimal _", "{{print 12_34}}", "1234", D_TVAL, true},
    {"binary", "{{print 0b101}}", "5", D_TVAL, true},
    {"binary _", "{{print 0b_1_0_1}}", "5", D_TVAL, true},
    {"BINARY", "{{print 0B101}}", "5", D_TVAL, true},
    {"octal0", "{{print 0377}}", "255", D_TVAL, true},
    {"octal", "{{print 0o377}}", "255", D_TVAL, true},
    {"octal _", "{{print 0o_3_7_7}}", "255", D_TVAL, true},
    {"OCTAL", "{{print 0O377}}", "255", D_TVAL, true},
    {"hex", "{{print 0x123}}", "291", D_TVAL, true},
    {"hex _", "{{print 0x1_23}}", "291", D_TVAL, true},
    {"HEX", "{{print 0X123ABC}}", "1194684", D_TVAL, true},
    {"float", "{{print 123.4}}", "123.4", D_TVAL, true},
    {"float _", "{{print 0_0_1_2_3.4}}", "123.4", D_TVAL, true},
    {"hex float", "{{print +0x1.ep+2}}", "7.5", D_TVAL, true},
    {"hex float _", "{{print +0x_1.e_0p+0_2}}", "7.5", D_TVAL, true},
    {"HEX float", "{{print +0X1.EP+2}}", "7.5", D_TVAL, true},
    {"print multi", "{{print 1_2_3_4 7.5_00_00_00}}", "1234 7.5", D_TVAL, true},
    {"print multi2", "{{print 1234 0x0_1.e_0p+02}}", "1234 7.5", D_TVAL, true},
    {"bug0", "{{range .MSIone}}{{if $.Method1 .}}X{{end}}{{end}}", "X", D_TVAL, true},
    {"bug1", "{{.Method0}}", "M0", D_IVAL_PTR, true},
    {"bug2", "{{$.NonEmptyInterface.Method0}}", "M0", D_TVAL, true},
    {"bug3", "{{with $}}{{.Method0}}{{end}}", "M0", D_TVAL, true},
    {"bug4", "{{if .Empty0}}non-nil{{else}}nil{{end}}", "nil", D_TVAL, true},
    {"bug5", "{{.Str}}", "foozle", D_TVAL, true},
    {"bug5a", "{{.Err}}", "erroozle", D_TVAL, true},
    {"bug6a", "{{vfunc .V0 .V1}}", "vfunc", D_TVAL, true},
    {"bug6b", "{{vfunc .V0 .V0}}", "vfunc", D_TVAL, true},
    {"bug6c", "{{vfunc .V1 .V0}}", "vfunc", D_TVAL, true},
    {"bug6d", "{{vfunc .V1 .V1}}", "vfunc", D_TVAL, true},
    {"bug7a", "{{3 2}}", "", D_TVAL, false},
    {"bug7b", "{{$x := 1}}{{$x 2}}", "", D_TVAL, false},
    {"bug7c", "{{$x := 1}}{{3 | $x}}", "", D_TVAL, false},
    {"bug8a", "{{3|oneArg}}", "", D_TVAL, false},
    {"bug8b", "{{4|dddArg 3}}", "", D_TVAL, false},
    {"bug9", "{{.cause}}", "neglect", D_CAUSE, true},
    {"bug10", "{{mapOfThree.three}}-{{(mapOfThree).three}}", "3-3", D_ZERO, true},
    {"bug11", "{{valueString .PS}}", "", D_TZERO, false},
    {"bug12xe", "{{printf `%T` 0xef}}", "int", D_TZERO, true},
    {"bug12xE", "{{printf `%T` 0xEE}}", "int", D_TZERO, true},
    {"bug12Xe", "{{printf `%T` 0Xef}}", "int", D_TZERO, true},
    {"bug12XE", "{{printf `%T` 0XEE}}", "int", D_TZERO, true},
    {"bug13", "{{print (.Copy).I}}", "17", D_TVAL, true},
    {"bug14a", "{{(nil).True}}", "", D_TVAL, false},
    {"bug14b", "{{$x := nil}}{{$x.anything}}", "", D_TVAL, false},
    {"bug14c", "{{$x := (1.0)}}{{$y := (\"hello\")}}{{$x.anything}}{{$y.true}}", "",
     D_TVAL, false},
    {"bug15", "{{valueString returnInt}}", "", D_TVAL, false},
    {"bug16a", "{{true|printf}}", "", D_TVAL, false},
    {"bug16b", "{{1|printf}}", "", D_TVAL, false},
    {"bug16c", "{{1.1|printf}}", "", D_TVAL, false},
    {"bug16d", "{{'x'|printf}}", "", D_TVAL, false},
    {"bug16e", "{{0i|printf}}", "", D_TVAL, false},
    {"bug16f", "{{true|twoArgs \"xxx\"}}", "", D_TVAL, false},
    {"bug16g", "{{\"aaa\" |twoArgs \"bbb\"}}", "twoArgs=bbbaaa", D_TVAL, true},
    {"bug16h", "{{1|oneArg}}", "", D_TVAL, false},
    {"bug16i", "{{\"aaa\"|oneArg}}", "oneArg=aaa", D_TVAL, true},
    {"bug16j", "{{1+2i|printf \"%v\"}}", "(1+2i)", D_TVAL, true},
    {"bug16k", "{{\"aaa\"|printf }}", "aaa", D_TVAL, true},
    {"bug17a", "{{.NonEmptyInterface.X}}", "x", D_TVAL, true},
    {"bug17b", "-{{.NonEmptyInterface.Method1 1234}}-", "-1234-", D_TVAL, true},
    {"bug17c", "{{len .NonEmptyInterfacePtS}}", "2", D_TVAL, true},
    {"bug17d", "{{index .NonEmptyInterfacePtS 0}}", "a", D_TVAL, true},
    {"bug17e", "{{range .NonEmptyInterfacePtS}}-{{.}}-{{end}}", "-a--b-", D_TVAL, true},
    {"bug18a", "{{eq . '.'}}", "true", D_RUNE_DOT, true},
    {"bug18b", "{{eq . 'e'}}", "true", D_RUNE_E, true},
    {"bug18c", "{{eq . 'P'}}", "true", D_RUNE_P, true},
    {"issue56490", "{{$i := 0}}{{$x := 0}}{{range $i = .AI}}{{end}}{{$i}}", "5", D_TVAL,
     true},
    {"issue60801", "{{$k := 0}}{{$v := 0}}{{range $k, $v = .AI}}{{$k}}={{$v}} {{end}}",
     "0=3 1=4 2=5 ", D_TVAL, true},
};

static const ExecTest multi_exec_tests[] = {
    {"empty", "", "", D_NIL, true},
    {"text", "some text", "some text", D_NIL, true},
    {"invoke x", "{{template \"x\" .SI}}", "TEXT", D_TVAL, true},
    {"invoke x no args", "{{template \"x\"}}", "TEXT", D_TVAL, true},
    {"invoke dot int", "{{template \"dot\" .I}}", "17", D_TVAL, true},
    {"invoke dot []int", "{{template \"dot\" .SI}}", "[3 4 5]", D_TVAL, true},
    {"invoke dotV", "{{template \"dotV\" .U}}", "v", D_TVAL, true},
    {"invoke nested int", "{{template \"nested\" .I}}", "17", D_TVAL, true},
    {"variable declared by template", "{{template \"nested\" $x:=.SI}},{{index $x 1}}",
     "[3 4 5],4", D_TVAL, true},

    /* User-defined function: test argument evaluator. */
    {"testFunc literal", "{{oneArg \"joe\"}}", "oneArg=joe", D_TVAL, true},
    {"testFunc .", "{{oneArg .}}", "oneArg=joe", D_JOE, true},
};

/* In these tests, actual content (not just template definitions) comes from
 * the parsed files. */
static const ExecTest template_file_exec_tests[] = {
    {"test", "{{template \"tmpl1.tmpl\"}}{{template \"tmpl2.tmpl\"}}",
     "template1\n\ny\ntemplate2\n\nx\n", D_ZERO, true},
};

#define NTESTS(a) (sizeof(a) / sizeof((a)[0]))

/* Go's bigInt and bigUint, the largest int and one more than it in hex. */
static Str big_int;
static Str big_uint;

static void test_execute(TestingT *t, const ExecTest *tests, size_t n,
                         TextTemplate *tmpl_set) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Map *funcs = exec_funcs(a);
    for (size_t i = 0; i < n; i++) {
        const ExecTest *test = &tests[i];
        Str name = str_from_cstr(test->name);
        Str input = str_from_cstr(test->input);
        input = strings_replace_all(a, input, BURROW_S("BIGINT"), big_int);
        input = strings_replace_all(a, input, BURROW_S("BIGUINT"), big_uint);
        TextTemplate *tmpl = tmpl_set == NULL
                                 ? text_template_new(a, name)
                                 : text_template_new_associated(tmpl_set, name);
        Error err = BURROW_NO_ERROR;
        tmpl = text_template_parse(text_template_funcs(tmpl, funcs), input, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: parse error: %s", name, err);
            continue;
        }
        strings_builder_reset(&b);
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                    data_of(test->data));
        if (!test->ok && BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: expected error; got none", name);
            continue;
        }
        if (test->ok && BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: unexpected execute error: %s", name, err);
            continue;
        }
        Str result = strings_builder_string(&b);
        Str output = str_from_cstr(test->output);
        if (!str_eq(result, output))
            testing_t_errorf_v(t, "%s: expected\n\t%q\ngot\n\t%q", name, output,
                               result);
    }
    count_reap();
    arena_free(&ar);
}

static void TestExecute(TestingT *t) {
    test_execute(t, exec_tests, NTESTS(exec_tests), NULL);
}

#define DELIM_VALUE_FIELDS(F, T) F(T, Str, Str, "")
BURROW_STRUCT_DECL(DelimValue, DELIM_VALUE_FIELDS);
BURROW_STRUCT_DEFINE(DelimValue, DELIM_VALUE_FIELDS);

static const char *const delim_pairs[] = {
    "",     "",     /* default */
    "{{",   "}}",   /* same as default */
    "<<",   ">>",   /* distinct */
    "|",    "|",    /* same */
    "(日)", "(本)", /* peculiar */
};

static void TestDelims(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str hello = BURROW_S("Hello, world");
    DelimValue value = {hello};
    for (size_t i = 0; i < NTESTS(delim_pairs); i += 2) {
        Str left = str_from_cstr(delim_pairs[i + 0]);
        Str true_left = left;
        Str right = str_from_cstr(delim_pairs[i + 1]);
        Str true_right = right;
        if (left.len == 0) /* default case */
            true_left = BURROW_S("{{");
        if (right.len == 0) /* default case */
            true_right = BURROW_S("}}");
        Str action = fmt_sprintf_v(a, "%s.Str%s", true_left, true_right);
        /* A comment, which is not preserved in the parse tree. */
        Str comment = fmt_sprintf_v(a, "%s/*comment*/%s", true_left, true_right);
        /* An action containing a string that looks like the left delimiter. */
        Str str_action =
            fmt_sprintf_v(a, "%s\"%s\"%s", true_left, true_left, true_right);
        Str text = fmt_sprintf_v(a, "%s%s%s", action, comment, str_action);
        /* At this point text looks like `{{.Str}}{{/\*comment*\/}}{{"{{"}}`. */
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl = text_template_parse(
            text_template_delims(text_template_new(a, BURROW_S("delims")), left, right),
            text, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "delim %q text %q parse err %s", left, text, err);
            break;
        }
        /* The parse tree's String form should roundtrip back to the input,
         * using the custom delimiters, modulo the dropped comment. */
        Str got = parse_list_node_string(tmpl->tree->root, a);
        Str want = fmt_sprintf_v(a, "%s%s", action, str_action);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "delim %q: String() = %q, want %q", left, got, want);
        StringsBuilder b = STRINGS_BUILDER(a);
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                    BURROW_ANY(TYPE_OF(DelimValue), &value));
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "delim %q exec err %s", left, err);
            break;
        }
        Str expect = fmt_sprintf_v(a, "%s%s", hello, true_left);
        if (!str_eq(strings_builder_string(&b), expect))
            testing_t_errorf_v(t, "expected %q got %q", expect,
                               strings_builder_string(&b));
    }
    arena_free(&ar);
}

/* Check that an error from a method flows back to the top. */
static void TestExecuteError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    TextTemplate *tmpl = text_template_new(a, BURROW_S("error"));
    Error err = BURROW_NO_ERROR;
    text_template_parse(tmpl, BURROW_S("{{.MyError true}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "parse error: %s", err);
    } else {
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                    data_of(D_TVAL));
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "expected error; got none");
        else if (!strings_contains(error_text(err), error_text(my_error)))
            testing_t_errorf_v(t, "expected myError; got %s", err);
    }
    arena_free(&ar);
}

static const char exec_error_text[] =
    "line 1\n"
    "line 2\n"
    "line 3\n"
    "{{template \"one\" .}}\n"
    "{{define \"one\"}}{{template \"two\" .}}{{end}}\n"
    "{{define \"two\"}}{{template \"three\" .}}{{end}}\n"
    "{{define \"three\"}}{{index \"hi\" $}}{{end}}";

/* Check that an error from a nested template contains all the relevant
 * information. */
static void TestExecError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("top")),
                                             str_from_cstr(exec_error_text), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "parse error:", err);
        arena_free(&ar);
        return;
    }
    Int five = 5; /* 5 is out of range indexing "hi" */
    err = text_template_execute(tmpl, io_discard, BURROW_ANY(TYPE_INT, &five));
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected error");
        arena_free(&ar);
        return;
    }
    Str want = BURROW_S("template: top:7:20: executing \"three\" at <index \"hi\" $>: "
                        "error calling index: index out of range: 5");
    Str got = error_text(err);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "expected\n%q\ngot\n%q", want, got);
    arena_free(&ar);
}

#define CUSTOM_ERROR_FIELDS(F, T) F(T, Int, pad, "")
BURROW_STRUCT_DECL(CustomError, CUSTOM_ERROR_FIELDS);
BURROW_STRUCT_DEFINE(CustomError, CUSTOM_ERROR_FIELDS);

static Str custom_error_message(const void *self) {
    (void)self;
    return BURROW_S("heyo !");
}

static const ErrorVT custom_error_vt = {.self_type = &burrow_type_CustomError,
                                        .message = custom_error_message};
static CustomError custom_error;

static Str failing_func(void *env, Error *err) {
    (void)env;
    *err = (Error){&custom_error_vt, &custom_error};
    return BURROW_STR_EMPTY;
}

/* Check that a custom error can be returned. */
static void TestExecError_CustomError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static ErrFn failing = {failing_func, NULL};
    Map *funcs = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
    add_func(funcs, "err", BURROW_ANY(TYPE_OF(ErrFn), &failing));
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(
        text_template_funcs(text_template_new(a, BURROW_S("top")), funcs),
        BURROW_S("{{ err }}"), &err);
    tmpl = text_template_must(tmpl, err);

    err = text_template_execute(tmpl, io_discard, NIL_ANY);

    if (errors_as(err, TYPE_OF(CustomError)) == NULL)
        testing_t_fatalf_v(t, "expected custom error; got %s", err);
    arena_free(&ar);
}

static void TestJSEscaping(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        const char *in, *exp;
    } test_cases[] = {
        {"a", "a"},
        {"'foo", "\\'foo"},
        {"Go \"jump\" \\", "Go \\\"jump\\\" \\\\"},
        {"Yukihiro says \"今日は世界\"", "Yukihiro says \\\"今日は世界\\\""},
        {"unprintable \xEF\xBF\xBE", "unprintable \\uFFFE"},
        {"<html>", "\\u003Chtml\\u003E"},
        {"no = in attributes", "no \\u003D in attributes"},
        {"&#x27; does not become HTML entity",
         "\\u0026#x27; does not become HTML entity"},
    };
    for (size_t i = 0; i < NTESTS(test_cases); i++) {
        Str in = str_from_cstr(test_cases[i].in);
        Str exp = str_from_cstr(test_cases[i].exp);
        Str s = text_template_js_escape_string(a, in);
        if (!str_eq(s, exp))
            testing_t_errorf_v(t, "JS escaping [%s] got [%s] want [%s]", in, s, exp);
    }
    arena_free(&ar);
}

/* A nice example: walk a binary tree. */

typedef struct Tree Tree;
BURROW_PTR_TYPE_DECL(TreePtr, Tree);
#define TREE_FIELDS(F, T)                                                              \
    F(T, Int, Val, "")                                                                 \
    F(T, TreePtr, Left, "")                                                            \
    F(T, TreePtr, Right, "")
BURROW_STRUCT_DECL(Tree, TREE_FIELDS);
const Type burrow_type_TreePtr =
    DESC(NONAME, NONAME, KIND_POINTER, sizeof(void *), _Alignof(void *), 0, NULL, 0,
         NULL, TYPE_OF(Tree), 0, NULL);
BURROW_STRUCT_DEFINE(Tree, TREE_FIELDS);

/* Use different delimiters to test Set.Delims. Also test the trimming of
 * leading and trailing spaces. */
static const char tree_template[] = "\n"
                                    "\t(- define \"tree\" -)\n"
                                    "\t[\n"
                                    "\t\t(- .Val -)\n"
                                    "\t\t(- with .Left -)\n"
                                    "\t\t\t(template \"tree\" . -)\n"
                                    "\t\t(- end -)\n"
                                    "\t\t(- with .Right -)\n"
                                    "\t\t\t(- template \"tree\" . -)\n"
                                    "\t\t(- end -)\n"
                                    "\t]\n"
                                    "\t(- end -)\n";

static void TestTree(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Tree n[11];
    for (int i = 0; i < 11; i++)
        n[i] = (Tree){i + 1, NULL, NULL};
    n[0].Left = &n[1];
    n[0].Right = &n[6];
    n[1].Left = &n[2];
    n[1].Right = &n[4];
    n[2].Left = &n[3];
    n[4].Left = &n[5];
    n[6].Left = &n[7];
    n[6].Right = &n[9];
    n[7].Left = &n[8];
    n[9].Left = &n[10];
    TreePtr tree = &n[0];
    Any data = BURROW_ANY(TYPE_OF(TreePtr), &tree);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl =
        text_template_parse(text_template_delims(text_template_new(a, BURROW_S("root")),
                                                 BURROW_S("("), BURROW_S(")")),
                            str_from_cstr(tree_template), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "parse error:", err);
        arena_free(&ar);
        return;
    }
    StringsBuilder b = STRINGS_BUILDER(a);
    Str expect = BURROW_S("[1[2[3[4]][5[6]]][7[8[9]][10[11]]]]");
    /* First by looking up the template. */
    err = text_template_execute(text_template_lookup(tmpl, BURROW_S("tree")),
                                strings_builder_as_io_writer(&b), data);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "exec error:", err);
        arena_free(&ar);
        return;
    }
    Str result = strings_builder_string(&b);
    if (!str_eq(result, expect))
        testing_t_errorf_v(t, "expected %q got %q", expect, result);
    /* Then direct to execution. */
    strings_builder_reset(&b);
    err = text_template_execute_template(tmpl, strings_builder_as_io_writer(&b),
                                         BURROW_S("tree"), data);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "exec error:", err);
        arena_free(&ar);
        return;
    }
    result = strings_builder_string(&b);
    if (!str_eq(result, expect))
        testing_t_errorf_v(t, "expected %q got %q", expect, result);
    arena_free(&ar);
}

static void TestExecuteOnNewTemplate(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    IoWriter nil_writer = {NULL, NULL};
    /* This is issue 3872. */
    (void)text_template_templates(text_template_new(a, BURROW_S("Name")), a);
    /* This is issue 11379. */
    TextTemplate t1 = TEXT_TEMPLATE(a);
    (void)text_template_templates(&t1, a);
    TextTemplate t2 = TEXT_TEMPLATE(a);
    (void)text_template_parse(&t2, BURROW_STR_EMPTY, &err);
    TextTemplate t3 = TEXT_TEMPLATE(a);
    err = BURROW_NO_ERROR;
    (void)text_template_parse(text_template_new_associated(&t3, BURROW_S("abc")),
                              BURROW_STR_EMPTY, &err);
    TextTemplate t4 = TEXT_TEMPLATE(a);
    /* returns an error (but does not crash) */
    (void)text_template_execute(&t4, nil_writer, NIL_ANY);
    TextTemplate t5 = TEXT_TEMPLATE(a);
    /* returns an error (but does not crash) */
    (void)text_template_execute_template(&t5, nil_writer, BURROW_S("XXX"), NIL_ANY);
    arena_free(&ar);
}

static const char test_templates[] =
    "{{define \"one\"}}one{{end}}{{define \"two\"}}two{{end}}";

static void TestMessageForExecuteEmpty(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int zero = 0;
    Any data = BURROW_ANY(TYPE_INT, &zero);
    /* Test a truly empty template. */
    TextTemplate *tmpl = text_template_new(a, BURROW_S("empty"));
    Error err = text_template_execute(tmpl, io_discard, data);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected initial error");
        arena_free(&ar);
        return;
    }
    Str got = error_text(err);
    Str want =
        BURROW_S("template: empty: \"empty\" is an incomplete or empty template");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "expected error %s got %s", want, got);
    /* Add a non-empty template to check that the error is helpful. */
    err = BURROW_NO_ERROR;
    TextTemplate *tests = text_template_parse(text_template_new(a, BURROW_STR_EMPTY),
                                              str_from_cstr(test_templates), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    text_template_add_parse_tree(tmpl, BURROW_S("secondary"), tests->tree, &err);
    err = text_template_execute(tmpl, io_discard, data);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected second error");
        arena_free(&ar);
        return;
    }
    got = error_text(err);
    want = BURROW_S("template: empty: \"empty\" is an incomplete or empty template");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "expected error %s got %s", want, got);
    /* Make sure we can execute the secondary. */
    err = text_template_execute_template(tmpl, io_discard, BURROW_S("secondary"), data);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    arena_free(&ar);
}

static void TestFinalForPrintf(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_STR_EMPTY),
                                             BURROW_S("{{\"x\" | printf}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
    } else {
        Int zero = 0;
        err = text_template_execute(tmpl, io_discard, BURROW_ANY(TYPE_INT, &zero));
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
    }
    arena_free(&ar);
}

typedef struct CmpTest {
    const char *expr;
    const char *truth;
    bool ok;
} CmpTest;

static const CmpTest cmp_tests[] = {
    {"eq true true", "true", true},
    {"eq true false", "false", true},
    {"eq 1+2i 1+2i", "true", true},
    {"eq 1+2i 1+3i", "false", true},
    {"eq 1.5 1.5", "true", true},
    {"eq 1.5 2.5", "false", true},
    {"eq 1 1", "true", true},
    {"eq 1 2", "false", true},
    {"eq `xy` `xy`", "true", true},
    {"eq `xy` `xyz`", "false", true},
    {"eq .Uthree .Uthree", "true", true},
    {"eq .Uthree .Ufour", "false", true},
    {"eq 3 4 5 6 3", "true", true},
    {"eq 3 4 5 6 7", "false", true},
    {"ne true true", "false", true},
    {"ne true false", "true", true},
    {"ne 1+2i 1+2i", "false", true},
    {"ne 1+2i 1+3i", "true", true},
    {"ne 1.5 1.5", "false", true},
    {"ne 1.5 2.5", "true", true},
    {"ne 1 1", "false", true},
    {"ne 1 2", "true", true},
    {"ne `xy` `xy`", "false", true},
    {"ne `xy` `xyz`", "true", true},
    {"ne .Uthree .Uthree", "false", true},
    {"ne .Uthree .Ufour", "true", true},
    {"lt 1.5 1.5", "false", true},
    {"lt 1.5 2.5", "true", true},
    {"lt 1 1", "false", true},
    {"lt 1 2", "true", true},
    {"lt `xy` `xy`", "false", true},
    {"lt `xy` `xyz`", "true", true},
    {"lt .Uthree .Uthree", "false", true},
    {"lt .Uthree .Ufour", "true", true},
    {"le 1.5 1.5", "true", true},
    {"le 1.5 2.5", "true", true},
    {"le 2.5 1.5", "false", true},
    {"le 1 1", "true", true},
    {"le 1 2", "true", true},
    {"le 2 1", "false", true},
    {"le `xy` `xy`", "true", true},
    {"le `xy` `xyz`", "true", true},
    {"le `xyz` `xy`", "false", true},
    {"le .Uthree .Uthree", "true", true},
    {"le .Uthree .Ufour", "true", true},
    {"le .Ufour .Uthree", "false", true},
    {"gt 1.5 1.5", "false", true},
    {"gt 1.5 2.5", "false", true},
    {"gt 1 1", "false", true},
    {"gt 2 1", "true", true},
    {"gt 1 2", "false", true},
    {"gt `xy` `xy`", "false", true},
    {"gt `xy` `xyz`", "false", true},
    {"gt .Uthree .Uthree", "false", true},
    {"gt .Uthree .Ufour", "false", true},
    {"gt .Ufour .Uthree", "true", true},
    {"ge 1.5 1.5", "true", true},
    {"ge 1.5 2.5", "false", true},
    {"ge 2.5 1.5", "true", true},
    {"ge 1 1", "true", true},
    {"ge 1 2", "false", true},
    {"ge 2 1", "true", true},
    {"ge `xy` `xy`", "true", true},
    {"ge `xy` `xyz`", "false", true},
    {"ge `xyz` `xy`", "true", true},
    {"ge .Uthree .Uthree", "true", true},
    {"ge .Uthree .Ufour", "false", true},
    {"ge .Ufour .Uthree", "true", true},
    {"eq .Uthree .Three", "true", true},
    {"eq .Three .Uthree", "true", true},
    {"le .Uthree .Three", "true", true},
    {"le .Three .Uthree", "true", true},
    {"ge .Uthree .Three", "true", true},
    {"ge .Three .Uthree", "true", true},
    {"lt .Uthree .Three", "false", true},
    {"lt .Three .Uthree", "false", true},
    {"gt .Uthree .Three", "false", true},
    {"gt .Three .Uthree", "false", true},
    {"eq .Ufour .Three", "false", true},
    {"lt .Ufour .Three", "false", true},
    {"gt .Ufour .Three", "true", true},
    {"eq .NegOne .Uthree", "false", true},
    {"eq .Uthree .NegOne", "false", true},
    {"ne .NegOne .Uthree", "true", true},
    {"ne .Uthree .NegOne", "true", true},
    {"lt .NegOne .Uthree", "true", true},
    {"lt .Uthree .NegOne", "false", true},
    {"le .NegOne .Uthree", "true", true},
    {"le .Uthree .NegOne", "false", true},
    {"gt .NegOne .Uthree", "false", true},
    {"gt .Uthree .NegOne", "true", true},
    {"ge .NegOne .Uthree", "false", true},
    {"ge .Uthree .NegOne", "true", true},
    {"eq (index `x` 0) 'x'", "true", true},
    {"eq (index `x` 0) 'y'", "false", true},
    {"eq .V1 .V2", "true", true},
    {"eq .Ptr .Ptr", "true", true},
    {"eq .Ptr .NilPtr", "false", true},
    {"eq .NilPtr .NilPtr", "true", true},
    {"eq .Iface1 .Iface1", "true", true},
    {"eq .Iface1 .NilIface", "false", true},
    {"eq .NilIface .NilIface", "true", true},
    {"eq .NilIface .Iface1", "false", true},
    {"eq .NilIface 0", "false", true},
    {"eq 0 .NilIface", "false", true},
    {"eq .Map .Map", "true", true},
    {"eq .Map nil", "true", true},
    {"eq nil .Map", "true", true},
    {"eq .Map .NonNilMap", "false", true},
    {"eq `xy` 1", "", false},
    {"eq 2 2.0", "", false},
    {"lt true true", "", false},
    {"lt 1+0i 1+0i", "", false},
    {"eq .Ptr 1", "", false},
    {"eq .Ptr .NegOne", "", false},
    {"eq .Map .V1", "", false},
    {"eq .NonNilMap .NonNilMap", "", false},
};

#define CMP_STRUCT_FIELDS(F, T)                                                        \
    F(T, Uint, Uthree, "")                                                             \
    F(T, Uint, Ufour, "")                                                              \
    F(T, Int, NegOne, "")                                                              \
    F(T, Int, Three, "")                                                               \
    F(T, IntPtr, Ptr, "")                                                              \
    F(T, IntPtr, NilPtr, "")                                                           \
    F(T, IntIntMap, NonNilMap, "")                                                     \
    F(T, IntIntMap, Map, "")                                                           \
    F(T, V, V1, "")                                                                    \
    F(T, V, V2, "")                                                                    \
    F(T, Stringer, Iface1, "")                                                         \
    F(T, Stringer, NilIface, "")
BURROW_STRUCT_DECL(CmpStruct, CMP_STRUCT_FIELDS);
BURROW_STRUCT_DEFINE(CmpStruct, CMP_STRUCT_FIELDS);
BURROW_PTR_TYPE(CmpStructPtr, CmpStruct);

static void TestComparison(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Int ptr_target = 0;
    Buffer iface1 = {BURROW_STR_EMPTY};
    CmpStruct cmp_struct = {
        .Uthree = 3,
        .Ufour = 4,
        .NegOne = -1,
        .Three = 3,
        .Ptr = &ptr_target,
        .NonNilMap = map_make(a, TYPE_INT, TYPE_INT, 0),
        .Iface1 = {&buffer_stringer_vt, &iface1},
    };
    CmpStructPtr data = &cmp_struct;
    for (size_t i = 0; i < NTESTS(cmp_tests); i++) {
        const CmpTest *test = &cmp_tests[i];
        Str expr = str_from_cstr(test->expr);
        Str text = fmt_sprintf_v(a, "{{if %s}}true{{else}}false{{end}}", expr);
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl =
            text_template_parse(text_template_new(a, BURROW_S("empty")), text, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%q: %s", expr, err);
            break;
        }
        strings_builder_reset(&b);
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                    BURROW_ANY(TYPE_OF(CmpStructPtr), &data));
        if (test->ok && BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s errored incorrectly: %s", expr, err);
            continue;
        }
        if (!test->ok && BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s did not error", expr);
            continue;
        }
        Str truth = str_from_cstr(test->truth);
        if (!str_eq(strings_builder_string(&b), truth))
            testing_t_errorf_v(t, "%s: want %s; got %s", expr, truth,
                               strings_builder_string(&b));
    }
    arena_free(&ar);
}

static void TestMissingMapKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Map *data = make_str_int(a, 1, (const char *const[]){"x"}, (const Int[]){99});
    Any d = BURROW_ANY(TYPE_OF(StrIntMap), &data);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("t1")),
                                             BURROW_S("{{.x}} {{.y}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    StringsBuilder b = STRINGS_BUILDER(a);
    /* By default, just get "<no value>" */
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b), d);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    Str want = BURROW_S("99 <no value>");
    Str got = strings_builder_string(&b);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; expected %q", got, want);
    /* Same if we set the option explicitly to the default. */
    text_template_option(tmpl, STRS(BURROW_S("missingkey=default")));
    strings_builder_reset(&b);
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b), d);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "default:", err);
        goto out;
    }
    want = BURROW_S("99 <no value>");
    got = strings_builder_string(&b);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; expected %q", got, want);
    /* Next we ask for a zero value */
    text_template_option(tmpl, STRS(BURROW_S("missingkey=zero")));
    strings_builder_reset(&b);
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b), d);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "zero:", err);
        goto out;
    }
    want = BURROW_S("99 0");
    got = strings_builder_string(&b);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; expected %q", got, want);
    /* Now we ask for an error. */
    text_template_option(tmpl, STRS(BURROW_S("missingkey=error")));
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b), d);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error; got none");
    /* same Option, but now a nil interface: ask for an error */
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b), NIL_ANY);
    testing_t_log_v(t, err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error for nil-interface; got none");
out:
    arena_free(&ar);
}

/* Test that the error message for multiline unterminated string refers to the
 * line number of the opening quote. */
static void TestUnterminatedStringError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    text_template_parse(text_template_new(a, BURROW_S("X")),
                        BURROW_S("hello\n\n{{`unterminated\n\n\n\n}}\n some more\n\n"),
                        &err);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected error");
    } else {
        Str str = error_text(err);
        if (!strings_contains(str, BURROW_S("X:3: unterminated raw quoted string")))
            testing_t_fatalf_v(t, "unexpected error: %s", str);
    }
    arena_free(&ar);
}

BURROW_SENTINEL_ERROR(always_error, "always be failing");

static Int error_writer_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = always_error;
    return 0;
}

static const IoWriterVT error_writer_vt = {NULL, error_writer_write};

static void TestExecuteGivesExecError(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int zero = 0;
    Any data = BURROW_ANY(TYPE_INT, &zero);
    /* First, a non-execution error shouldn't be an ExecError. */
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("X")),
                                             BURROW_S("hello"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    err = text_template_execute(tmpl, (IoWriter){&error_writer_vt, &zero}, data);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected error; got none");
        goto out;
    }
    Str always_error_text = BURROW_S("always be failing");
    if (!str_eq(error_text(err), always_error_text))
        testing_t_errorf_v(t, "expected %q error; got %q", always_error_text, err);
    /* This one should be an ExecError. */
    err = BURROW_NO_ERROR;
    tmpl = text_template_parse(text_template_new(a, BURROW_S("X")),
                               BURROW_S("hello, {{.X.Y}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    err = text_template_execute(tmpl, io_discard, data);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "expected error; got none");
        goto out;
    }
    if (err.vt->self_type != TYPE_TEXT_TEMPLATE_EXEC_ERROR) {
        testing_t_fatalf_v(t, "did not expect ExecError %s", err);
        goto out;
    }
    Str expect = BURROW_S("field X in type int");
    if (!strings_contains(error_text(err), expect))
        testing_t_errorf_v(t, "expected %q; got %q", expect, err);
out:
    arena_free(&ar);
}

static Int func_name_test_func(void *env) {
    (void)env;
    return 0;
}

static IntFn func_name_test_fn = {func_name_test_func, NULL};

static void TestGoodFuncNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const names[] = {
        "_", "a", "a1", "a1", "Ӵ",
    };
    for (size_t i = 0; i < NTESTS(names); i++) {
        Map *funcs = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
        add_func(funcs, names[i], BURROW_ANY(TYPE_OF(IntFn), &func_name_test_fn));
        TextTemplate *tmpl =
            text_template_funcs(text_template_new(a, BURROW_S("X")), funcs);
        if (tmpl == NULL) {
            testing_t_fatalf_v(t, "nil result for %q", names[i]);
            break;
        }
    }
    arena_free(&ar);
}

static void test_bad_func_name(Alloc *a, const char *name, TestingT *t) {
    Map *funcs = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
    add_func(funcs, name, BURROW_ANY(TYPE_OF(IntFn), &func_name_test_fn));
    TextTemplate *tmpl = text_template_new(a, BURROW_S("X"));
    volatile bool panicked = false;
    BURROW_TRY {
        text_template_funcs(tmpl, funcs);
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    /* If we get here without a panic, the name did not cause one, which is how
     * Funcs reports an error. */
    if (!panicked)
        testing_t_errorf_v(t, "%q succeeded incorrectly as function name", name);
}

static void TestBadFuncNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const names[] = {
        "",
        "2",
        "a-b",
    };
    for (size_t i = 0; i < NTESTS(names); i++)
        test_bad_func_name(a, names[i], t);
    arena_free(&ar);
}

/* [0]int, which BURROW_ARRAY_TYPE cannot declare. */
static const Type burrow_type_IntArray0 = DESC(
    NONAME, NONAME, KIND_ARRAY, 0, _Alignof(Int), 0, NULL, 0, NULL, TYPE_INT, 0, NULL);

static void TestIsTrue(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int i1 = 1, i0 = 0;
    uint8_t u81 = 1, u80 = 0;
    double f1 = 1.0, f0 = 0.0;
    Complex64 c1 = {1.0F, 0.0F}, c0 = {0.0F, 0.0F};
    bool bt = true, bf = false;
    IntArray2 arr = {{1, 2}};
    Int arr0 = 0;
    Slice abc = slice_from((void *)(uintptr_t)"abc", 3, 3, TYPE_BYTE);
    Slice empty_bytes = slice_from((void *)(uintptr_t)"", 0, 0, TYPE_BYTE);
    Map *full =
        make_str_int(a, 2, (const char *const[]){"a", "b"}, (const Int[]){1, 2});
    Map *empty = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Chan *ch = chan_make(heap_allocator(), TYPE_INT, 0);
    Chan *nil_chan = NULL;
    Int new_int = 0;
    IntPtr ptr = &new_int, nil_ptr = NULL;
    UnsafePointer up = &new_int, nil_up = NULL;
    const struct {
        Any v;
        bool want;
    } tests[] = {
        {BURROW_ANY(TYPE_INT, &i1), true},
        {BURROW_ANY(TYPE_INT, &i0), false},
        {BURROW_ANY(TYPE_OF(uint8_t), &u81), true},
        {BURROW_ANY(TYPE_OF(uint8_t), &u80), false},
        {BURROW_ANY(TYPE_OF(double), &f1), true},
        {BURROW_ANY(TYPE_OF(double), &f0), false},
        {BURROW_ANY(TYPE_OF(Complex64), &c1), true},
        {BURROW_ANY(TYPE_OF(Complex64), &c0), false},
        {BURROW_ANY(TYPE_BOOL, &bt), true},
        {BURROW_ANY(TYPE_BOOL, &bf), false},
        {BURROW_ANY(TYPE_OF(IntArray2), &arr), true},
        {BURROW_ANY(TYPE_OF(IntArray0), &arr0), false},
        {BURROW_ANY(TYPE_OF(Bytes), &abc), true},
        {BURROW_ANY(TYPE_OF(Bytes), &empty_bytes), false},
        {BURROW_ANY(TYPE_OF(StrIntMap), &full), true},
        {BURROW_ANY(TYPE_OF(StrIntMap), &empty), false},
        {BURROW_ANY(TYPE_OF(IntChan), &ch), true},
        {BURROW_ANY(TYPE_OF(IntChan), &nil_chan), false},
        {BURROW_ANY(TYPE_OF(IntPtr), &ptr), true},
        {BURROW_ANY(TYPE_OF(IntPtr), &nil_ptr), false},
        {BURROW_ANY(TYPE_UNSAFE_POINTER, &up), true},
        {BURROW_ANY(TYPE_UNSAFE_POINTER, &nil_up), false},
    };
    for (size_t i = 0; i < NTESTS(tests); i++) {
        bool ok;
        bool got = text_template_is_true(tests[i].v, &ok);
        if (got != tests[i].want) {
            testing_t_fatalf_v(t, "expect result %v, got %v", tests[i].want, got);
            break;
        }
    }
    chan_free(ch);
    arena_free(&ar);
}

static void TestBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str input = BURROW_S("a({{block \"inner\" .}}bar({{.}})baz{{end}})b");
    Str want = BURROW_S("a(bar(hello)baz)b");
    Str overlay = BURROW_S("{{define \"inner\"}}foo({{.}})bar{{end}}");
    Str want2 = BURROW_S("a(foo(goodbye)bar)b");
    Str hello = BURROW_S("hello");
    Str goodbye = BURROW_S("goodbye");
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl =
        text_template_parse(text_template_new(a, BURROW_S("outer")), input, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    TextTemplate *clone = text_template_clone(tmpl, a, &err);
    TextTemplate *tmpl2 =
        text_template_parse(text_template_must(clone, err), overlay, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }

    StringsBuilder buf = STRINGS_BUILDER(a);
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                BURROW_ANY(TYPE_STRING, &hello));
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    Str got = strings_builder_string(&buf);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);

    strings_builder_reset(&buf);
    err = text_template_execute(tmpl2, strings_builder_as_io_writer(&buf),
                                BURROW_ANY(TYPE_STRING, &goodbye));
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    got = strings_builder_string(&buf);
    if (!str_eq(got, want2))
        testing_t_errorf_v(t, "got %q, want %q", got, want2);
out:
    arena_free(&ar);
}

typedef struct EvalFieldCase {
    const char *name, *src;
    Any value;
    const char *want;
} EvalFieldCase;

static void eval_field_case(void *env, TestingT *t) {
    const EvalFieldCase *tc = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("tmpl")),
                                             str_from_cstr(tc->src), &err);
    tmpl = text_template_must(tmpl, err);
    err = text_template_execute(tmpl, io_discard, tc->value);
    Str got = BURROW_S("<nil>");
    if (BURROW_FAILED(err))
        got = error_text(err);
    Str want = str_from_cstr(tc->want);
    if (!strings_has_suffix(got, want))
        testing_t_fatalf_v(t, "got error %q, want %q", got, want);
    arena_free(&ar);
}

static void TestEvalFieldErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    T *nil_t = NULL;
    T *new_t = BURROW_NEW(a, T);
    StrStrMap *nil_map_ptr = NULL;
    StrStrMap empty_map = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    StrStrMap *map_ptr = &empty_map;
    const EvalFieldCase tests[] = {
        {
            /* Check that calling an invalid field on nil pointer prints a
             * field error instead of a distracting nil pointer error.
             * https://golang.org/issue/15125 */
            "MissingFieldOnNil",
            "{{.MissingField}}",
            BURROW_ANY(TYPE_OF(TPtr), &nil_t),
            "can't evaluate field MissingField in type *T",
        },
        {
            "MissingFieldOnNonNil",
            "{{.MissingField}}",
            BURROW_ANY(TYPE_OF(TPtr), &new_t),
            "can't evaluate field MissingField in type *T",
        },
        {
            "ExistingFieldOnNil",
            "{{.X}}",
            BURROW_ANY(TYPE_OF(TPtr), &nil_t),
            "nil pointer evaluating *T.X",
        },
        {
            "MissingKeyOnNilMap",
            "{{.MissingKey}}",
            BURROW_ANY(TYPE_OF(StrStrMapPtr), &nil_map_ptr),
            "nil pointer evaluating *map[string]string.MissingKey",
        },
        {
            "MissingKeyOnNilMapPtr",
            "{{.MissingKey}}",
            BURROW_ANY(TYPE_OF(StrStrMapPtr), &nil_map_ptr),
            "nil pointer evaluating *map[string]string.MissingKey",
        },
        {
            "MissingKeyOnMapPtrToNil",
            "{{.MissingKey}}",
            BURROW_ANY(TYPE_OF(StrStrMapPtr), &map_ptr),
            "<nil>",
        },
    };
    for (size_t i = 0; i < NTESTS(tests); i++)
        testing_t_run(
            t, str_from_cstr(tests[i].name),
            BURROW_FN(TestingTFunc, eval_field_case, (void *)(uintptr_t)&tests[i]));
    arena_free(&ar);
}

static void TestMaxExecDepth(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in -short mode");
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("tmpl")),
                                             BURROW_S("{{template \"tmpl\" .}}"), &err);
    tmpl = text_template_must(tmpl, err);
    err = text_template_execute(tmpl, io_discard, NIL_ANY);
    Str got = BURROW_S("<nil>");
    if (BURROW_FAILED(err))
        got = error_text(err);
    Str want = BURROW_S("exceeded maximum template depth");
    if (!strings_contains(got, want))
        testing_t_errorf_v(t, "got error %q; want %q", got, want);
    arena_free(&ar);
}

static void TestAddrOfIndex(TestingT *t) {
    /* golang.org/issue/14916. Before index worked on reflect.Values, the
     * .String could not be found on the (incorrectly unaddressable) V value,
     * in contrast to range, which worked fine. Also testing that passing a
     * reflect.Value to tmpl.Execute works. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const texts[] = {
        "{{range .}}{{.String}}{{end}}",
        "{{with index . 0}}{{.String}}{{end}}",
    };
    for (size_t i = 0; i < NTESTS(texts); i++) {
        Str text = str_from_cstr(texts[i]);
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl =
            text_template_parse(text_template_new(a, BURROW_S("tmpl")), text, &err);
        tmpl = text_template_must(tmpl, err);
        StringsBuilder buf = STRINGS_BUILDER(a);
        V vs[] = {{1}};
        Slice s = slice_from(vs, 1, 1, TYPE_OF(V));
        ReflectValue rv = reflect_value_of(BURROW_ANY(TYPE_OF(VSlice), &s));
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_REFLECT_VALUE, &rv));
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%s: Execute: %v", text, err);
            break;
        }
        if (!str_eq(strings_builder_string(&buf), BURROW_S("<1>"))) {
            testing_t_fatalf_v(t, "%s: template output = %q, want %q", text,
                               strings_builder_string(&buf), BURROW_S("<1>"));
            break;
        }
    }
    arena_free(&ar);
}

#define SIG_PLUS_ONE(IN, OUT) IN(0, Int) OUT(Int)
BURROW_FUNC(IntIntFn, Int, Int n);
BURROW_FUNC_TYPE_DEFINE(IntIntFn, SIG_PLUS_ONE);

static Int plus_one(void *env, Int n) {
    (void)env;
    return n + 1;
}

static void TestInterfaceValues(TestingT *t) {
    /* golang.org/issue/17714. Before index worked on reflect.Values,
     * interface values were always implicitly promoted to the underlying
     * value, except that nil interfaces were promoted to the zero
     * reflect.Value. Eliminating a round trip to interface{} and back to
     * reflect.Value eliminated this promotion, breaking these cases. */
    static const struct {
        const char *text;
        const char *out;
    } tests[] = {
        {"{{index .Nil 1}}", "ERROR: index of untyped nil"},
        {"{{index .Slice 2}}", "2"},
        {"{{index .Slice .Two}}", "2"},
        {"{{call .Nil 1}}", "ERROR: call of nil"},
        {"{{call .PlusOne 1}}", "2"},
        {"{{call .PlusOne .One}}", "2"},
        {"{{and (index .Slice 0) true}}", "0"},
        {"{{and .Zero true}}", "0"},
        {"{{and (index .Slice 1) false}}", "false"},
        {"{{and .One false}}", "false"},
        {"{{or (index .Slice 0) false}}", "false"},
        {"{{or .Zero false}}", "false"},
        {"{{or (index .Slice 1) true}}", "1"},
        {"{{or .One true}}", "1"},
        {"{{not (index .Slice 0)}}", "true"},
        {"{{not .Zero}}", "true"},
        {"{{not (index .Slice 1)}}", "false"},
        {"{{not .One}}", "false"},
        {"{{eq (index .Slice 0) .Zero}}", "true"},
        {"{{eq (index .Slice 1) .One}}", "true"},
        {"{{ne (index .Slice 0) .Zero}}", "false"},
        {"{{ne (index .Slice 1) .One}}", "false"},
        {"{{ge (index .Slice 0) .One}}", "false"},
        {"{{ge (index .Slice 1) .Zero}}", "true"},
        {"{{gt (index .Slice 0) .One}}", "false"},
        {"{{gt (index .Slice 1) .Zero}}", "true"},
        {"{{le (index .Slice 0) .One}}", "true"},
        {"{{le (index .Slice 1) .Zero}}", "false"},
        {"{{lt (index .Slice 0) .One}}", "true"},
        {"{{lt (index .Slice 1) .Zero}}", "false"},
    };

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static IntIntFn plus_one_fn = {plus_one, NULL};
    Int slice_ints[] = {0, 1, 2, 3};
    Slice slice = slice_from(slice_ints, 4, 4, TYPE_INT);
    Int one = 1, two = 2, zero = 0;
    Map *data = map_make(a, TYPE_STRING, TYPE_OF(Any), 6);
    add_func(data, "PlusOne", BURROW_ANY(TYPE_OF(IntIntFn), &plus_one_fn));
    add_func(data, "Slice", BURROW_ANY(TYPE_OF(IntSlice), &slice));
    add_func(data, "One", BURROW_ANY(TYPE_INT, &one));
    add_func(data, "Two", BURROW_ANY(TYPE_INT, &two));
    add_func(data, "Nil", NIL_ANY);
    add_func(data, "Zero", BURROW_ANY(TYPE_INT, &zero));

    for (size_t i = 0; i < NTESTS(tests); i++) {
        Str text = str_from_cstr(tests[i].text);
        Str out = str_from_cstr(tests[i].out);
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl =
            text_template_parse(text_template_new(a, BURROW_S("tmpl")), text, &err);
        tmpl = text_template_must(tmpl, err);
        StringsBuilder buf = STRINGS_BUILDER(a);
        err = text_template_execute(tmpl, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_OF(StrAnyMap), &data));
        if (strings_has_prefix(out, BURROW_S("ERROR:"))) {
            Str e = strings_trim_space(strings_trim_prefix(out, BURROW_S("ERROR:")));
            if (BURROW_OK(err) || !strings_contains(error_text(err), e))
                testing_t_errorf_v(t, "%s: Execute: %v, want error %q", text, err, e);
            continue;
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: Execute: %v", text, err);
            continue;
        }
        if (!str_eq(strings_builder_string(&buf), out))
            testing_t_errorf_v(t, "%s: template output = %q, want %q", text,
                               strings_builder_string(&buf), out);
    }
    arena_free(&ar);
}

static Str custom_panic(void *env) {
    (void)env;
    panic_str(BURROW_S("custom panic string"));
}

typedef struct CallErrorCase {
    const char *name;
    const char *input;
    int data;
    const char *want_err;
} CallErrorCase;

/* Check that panics during calls are recovered and returned as errors. */
static void TestExecutePanicDuringCall(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static StrFn do_panic_fn = {custom_panic, NULL};
    Map *funcs = map_make(a, TYPE_STRING, TYPE_OF(Any), 1);
    add_func(funcs, "doPanic", BURROW_ANY(TYPE_OF(StrFn), &do_panic_fn));
    static const CallErrorCase tests[] = {
        {
            "direct func call panics",
            "{{doPanic}}",
            -1,
            "template: t:1:2: executing \"t\" at <doPanic>: error calling doPanic: "
            "custom "
            "panic string",
        },
        {
            "indirect func call panics",
            "{{call doPanic}}",
            -1,
            "template: t:1:7: executing \"t\" at <doPanic>: error calling doPanic: "
            "custom "
            "panic string",
        },
        {
            "direct method call panics",
            "{{.GetU}}",
            -1,
            "template: t:1:2: executing \"t\" at <.GetU>: error calling GetU: runtime "
            "error: invalid memory address or nil pointer dereference",
        },
        {
            "indirect method call panics",
            "{{call .GetU}}",
            -1,
            "template: t:1:7: executing \"t\" at <.GetU>: error calling GetU: runtime "
            "error: invalid memory address or nil pointer dereference",
        },
        {
            "func field call panics",
            "{{call .PanicFunc}}",
            D_TVAL,
            "template: t:1:2: executing \"t\" at <call .PanicFunc>: error calling "
            "call: "
            "test panic",
        },
        {
            "method call on nil interface",
            "{{.NonEmptyInterfaceNil.Method0}}",
            D_TVAL,
            "template: t:1:23: executing \"t\" at <.NonEmptyInterfaceNil.Method0>: nil "
            "pointer evaluating Ifc.Method0",
        },
    };
    for (size_t i = 0; i < NTESTS(tests); i++) {
        const CallErrorCase *tc = &tests[i];
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl = text_template_parse(
            text_template_funcs(text_template_new(a, BURROW_S("t")), funcs),
            str_from_cstr(tc->input), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "parse error: %s", err);
            break;
        }
        /* -1 is (*T)(nil). */
        Any data =
            tc->data < 0 ? BURROW_ANY(TYPE_OF(TPtr), &tval_nil) : data_of(tc->data);
        err = text_template_execute(tmpl, io_discard, data);
        Str want_err = str_from_cstr(tc->want_err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%s: expected error; got none", tc->name);
        else if (!strings_contains(error_text(err), want_err))
            testing_t_errorf_v(t, "%s: expected error:\n%s\ngot:\n%s", tc->name,
                               want_err, err);
    }
    arena_free(&ar);
}

static void TestFunctionCheckDuringCall(TestingT *t) {
    static const CallErrorCase tests[] = {
        {
            .name = "call nothing",
            .input = "{{call}}",
            .data = D_TVAL,
            .want_err = "wrong number of args for call: want at least 1 got 0",
        },
        {
            .name = "call non-function",
            .input = "{{call .True}}",
            .data = D_TVAL,
            .want_err = "error calling call: non-function .True of type bool",
        },
        {
            .name = "call func with wrong argument",
            .input = "{{call .BinaryFunc 1}}",
            .data = D_TVAL,
            .want_err = "error calling call: wrong number of args for .BinaryFunc: got "
                        "1 want 2",
        },
        {
            .name = "call variadic func with wrong argument",
            .input = "{{call .VariadicFuncInt}}",
            .data = D_TVAL,
            .want_err =
                "error calling call: wrong number of args for .VariadicFuncInt: "
                "got 0 want at least 1",
        },
        {
            .name = "call too few return number func",
            .input = "{{call .TooFewReturnCountFunc}}",
            .data = D_TVAL,
            .want_err =
                "error calling call: function .TooFewReturnCountFunc has 0 return "
                "values; should be 1 or 2",
        },
        {
            .name = "call too many return number func",
            .input = "{{call .TooManyReturnCountFunc}}",
            .data = D_TVAL,
            .want_err =
                "error calling call: function .TooManyReturnCountFunc has 3 return "
                "values; should be 1 or 2",
        },
        {
            .name = "call invalid return type func",
            .input = "{{call .InvalidReturnTypeFunc}}",
            .data = D_TVAL,
            .want_err =
                "error calling call: invalid function signature for "
                ".InvalidReturnTypeFunc: second return value should be error; is "
                "bool",
        },
        {
            .name = "call pipeline",
            .input = "{{call (len \"test\")}}",
            .data = D_NIL,
            .want_err = "error calling call: non-function len \"test\" of type int",
        },
    };

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NTESTS(tests); i++) {
        const CallErrorCase *tc = &tests[i];
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("t")),
                                                 str_from_cstr(tc->input), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "parse error: %s", err);
            break;
        }
        err = text_template_execute(tmpl, io_discard, data_of(tc->data));
        Str want_err = str_from_cstr(tc->want_err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%s: expected error; got none", tc->name);
        else if (want_err.len == 0 || !strings_contains(error_text(err), want_err))
            testing_t_errorf_v(t, "%s: expected error:\n%s\ngot:\n%s", tc->name,
                               want_err, err);
    }
    arena_free(&ar);
}

static Str result_func(void *env) {
    (void)env;
    return BURROW_S("result");
}

/* Issue 31810. Check that a parenthesized first argument behaves properly. */
static void TestIssue31810(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    /* A simple value with no arguments is fine. */
    StringsBuilder b = STRINGS_BUILDER(a);
    Str text = BURROW_S("{{ (.)  }}");
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl =
        text_template_parse(text_template_new(a, BURROW_STR_EMPTY), text, &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    Str result = BURROW_S("result");
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_STRING, &result));
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    if (!str_eq(strings_builder_string(&b), result))
        testing_t_errorf_v(t, "%s got %q, expected %q", text,
                           strings_builder_string(&b), result);

    /* Even a plain function fails - need to use call. */
    StrFn f = {result_func, NULL};
    strings_builder_reset(&b);
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_OF(StrFn), &f));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error with no call, got none");

    /* Works if the function is explicitly called. */
    Str text_call = BURROW_S("{{ (call .)  }}");
    err = BURROW_NO_ERROR;
    tmpl = text_template_parse(text_template_new(a, BURROW_STR_EMPTY), text_call, &err);
    strings_builder_reset(&b);
    err = text_template_execute(tmpl, strings_builder_as_io_writer(&b),
                                BURROW_ANY(TYPE_OF(StrFn), &f));
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    if (!str_eq(strings_builder_string(&b), result))
        testing_t_errorf_v(t, "%s got %q, expected %q", text_call,
                           strings_builder_string(&b), result);
    arena_free(&ar);
}

/* Issue 43065, range over send only channel */
static void TestIssue43065(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmp = text_template_parse(text_template_new(a, BURROW_STR_EMPTY),
                                            BURROW_S("{{range .}}{{end}}"), &err);
    tmp = text_template_must(tmp, err);
    IntSendChan ch = chan_make(heap_allocator(), TYPE_INT, 0);
    err = text_template_execute(tmp, io_discard, BURROW_ANY(TYPE_OF(IntSendChan), &ch));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected err got nil");
    else if (!strings_contains(error_text(err),
                               BURROW_S("range over send-only channel")))
        testing_t_errorf_v(t, "%s", err);
    chan_free(ch);
    arena_free(&ar);
}

/* What one goroutine of TestIssue39807 saw go wrong, kept until the test
 * goroutine can report it, since t is not to be called from elsewhere. */
typedef struct Issue39807Job {
    TextTemplate *foo;
    TextTemplate *bar;
    int num_templates;
    bool failed;
    char msg[256];
} Issue39807Job;

static void issue39807_fail(Issue39807Job *j, Error err) {
    if (j->failed)
        return;
    j->failed = true;
    Str s = error_text(err);
    size_t n = s.len < (Int)sizeof j->msg ? (size_t)s.len : sizeof j->msg - 1;
    memcpy(j->msg, s.p, n);
    j->msg[n] = '\0';
}

static void issue39807_run(void *env) {
    Issue39807Job *j = env;
    for (int k = 0; k < j->num_templates; k++) {
        Error err = BURROW_NO_ERROR;
        text_template_add_parse_tree(j->foo, text_template_name(j->bar), j->bar->tree,
                                     &err);
        if (BURROW_FAILED(err))
            issue39807_fail(j, err);
        err = text_template_execute(j->foo, io_discard, NIL_ANY);
        if (BURROW_FAILED(err))
            issue39807_fail(j, err);
    }
}

/* Issue 39807: data race in html/template & text/template */
static void TestIssue39807(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SyncWaitGroup wg = {0};

    Error err = BURROW_NO_ERROR;
    TextTemplate *tpl_foo =
        text_template_parse(text_template_new(a, BURROW_S("foo")),
                            BURROW_S("{{ template \"bar\" . }}"), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);

    err = BURROW_NO_ERROR;
    TextTemplate *tpl_bar = text_template_parse(text_template_new(a, BURROW_S("bar")),
                                                BURROW_S("bar"), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);

    enum { gofuncs = 10, num_templates = 10 };
    Issue39807Job jobs[gofuncs];

    for (int i = 0; i < gofuncs; i++) {
        jobs[i] = (Issue39807Job){tpl_foo, tpl_bar, num_templates, false, {0}};
        sync_wait_group_go(&wg, BURROW_FN(Func, issue39807_run, &jobs[i]));
    }

    sync_wait_group_wait(&wg);
    for (int i = 0; i < gofuncs; i++)
        if (jobs[i].failed)
            testing_t_error_v(t, jobs[i].msg);
    arena_free(&ar);
}

/* ------------------------------------------------------- multi_test.go */

typedef struct MultiParseTest {
    const char *name;
    const char *input;
    bool ok;
    int n;
    const char *names[2];
    const char *results[2];
} MultiParseTest;

enum { no_error = true, has_error = false };

static const MultiParseTest multi_parse_tests[] = {
    {"empty", "", no_error, 0, {NULL}, {NULL}},
    {"one", "{{define \"foo\"}} FOO {{end}}", no_error, 1, {"foo"}, {" FOO "}},
    {"two",
     "{{define \"foo\"}} FOO {{end}}{{define \"bar\"}} BAR {{end}}",
     no_error,
     2,
     {"foo", "bar"},
     {" FOO ", " BAR "}},
    /* errors */
    {"missing end", "{{define \"foo\"}} FOO ", has_error, 0, {NULL}, {NULL}},
    {"malformed name", "{{define \"foo}} FOO ", has_error, 0, {NULL}, {NULL}},
};

/* template.tmpl[name], or NULL. */
static TextTemplate *tmpl_of(TextTemplate *t, Str name) {
    TextTemplate **p = map_get(t->common->tmpl, &name);
    return p == NULL ? NULL : *p;
}

static void TestMultiParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NTESTS(multi_parse_tests); i++) {
        const MultiParseTest *test = &multi_parse_tests[i];
        Str name = str_from_cstr(test->name);
        Error err = BURROW_NO_ERROR;
        TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("root")),
                                                 str_from_cstr(test->input), &err);
        if (BURROW_OK(err) && !test->ok) {
            testing_t_errorf_v(t, "%q: expected error; got none", name);
            continue;
        }
        if (BURROW_FAILED(err) && test->ok) {
            testing_t_errorf_v(t, "%q: unexpected error: %v", name, err);
            continue;
        }
        if (BURROW_FAILED(err) && !test->ok) /* expected error, got one */
            continue;
        if (tmpl == NULL)
            continue;
        Int ntmpl = map_len(tmpl->common->tmpl);
        if (ntmpl != test->n + 1) { /* +1 for root */
            testing_t_errorf_v(t, "%s: wrong number of templates; wanted %d got %d",
                               name, test->n, ntmpl);
            continue;
        }
        for (int j = 0; j < test->n; j++) {
            Str tname = str_from_cstr(test->names[j]);
            TextTemplate *tt = tmpl_of(tmpl, tname);
            if (tt == NULL) {
                testing_t_errorf_v(t, "%s: can't find template %q", name, tname);
                continue;
            }
            Str result = parse_list_node_string(tt->tree->root, a);
            Str want = str_from_cstr(test->results[j]);
            if (!str_eq(result, want))
                testing_t_errorf_v(t, "%s=(%q): got\n\t%v\nexpected\n\t%v", name,
                                   str_from_cstr(test->input), result, want);
        }
    }
    arena_free(&ar);
}

/* These strings are also in the files tt_main writes. */
static const char multi_text1[] = "\n"
                                  "\t{{define \"x\"}}TEXT{{end}}\n"
                                  "\t{{define \"dotV\"}}{{.V}}{{end}}\n";

static const char multi_text2[] =
    "\n"
    "\t{{define \"dot\"}}{{.}}{{end}}\n"
    "\t{{define \"nested\"}}{{template \"dot\" .}}{{end}}\n";

/* Go's testdata directory, written to a temporary directory by tt_main. */
static Str tdir;

static Str tdata(Alloc *a, const char *name) {
    return filepath_join(a, STRS(tdir, str_from_cstr(name)));
}

static void TestMultiExecute(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    /* Declare a couple of templates first. */
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(text_template_new(a, BURROW_S("root")),
                                             str_from_cstr(multi_text1), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "parse error for 1: %s", err);
        goto out;
    }
    text_template_parse(tmpl, str_from_cstr(multi_text2), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "parse error for 2: %s", err);
        goto out;
    }
    test_execute(t, multi_exec_tests, NTESTS(multi_exec_tests), tmpl);
out:
    arena_free(&ar);
}

static void TestParseFiles(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    text_template_from_files(a, STRS(BURROW_S("DOES NOT EXIST")), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error for non-existent file; got none");
    TextTemplate *tmpl = text_template_new(a, BURROW_S("root"));
    err = BURROW_NO_ERROR;
    text_template_parse_files(
        tmpl, STRS(tdata(a, "file1.tmpl"), tdata(a, "file2.tmpl")), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error parsing files: %v", err);
    else
        test_execute(t, multi_exec_tests, NTESTS(multi_exec_tests), tmpl);
    arena_free(&ar);
}

static void TestParseGlob(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    text_template_from_glob(a, BURROW_S("DOES NOT EXIST"), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error for non-existent file; got none");
    err = BURROW_NO_ERROR;
    text_template_parse_glob(text_template_new(a, BURROW_S("error")), BURROW_S("[x"),
                             &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error for bad pattern; got none");
    TextTemplate *tmpl = text_template_new(a, BURROW_S("root"));
    err = BURROW_NO_ERROR;
    text_template_parse_glob(tmpl, tdata(a, "file*.tmpl"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error parsing files: %v", err);
    else
        test_execute(t, multi_exec_tests, NTESTS(multi_exec_tests), tmpl);
    arena_free(&ar);
}

static void TestParseFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Fs fs = os_dir_fs(a, tdir);

    {
        Error err = BURROW_NO_ERROR;
        text_template_from_fs(a, fs, STRS(BURROW_S("DOES NOT EXIST")), &err);
        if (BURROW_OK(err))
            testing_t_error_v(t, "expected error for non-existent file; got none");
    }

    {
        TextTemplate *tmpl = text_template_new(a, BURROW_S("root"));
        Error err = BURROW_NO_ERROR;
        text_template_parse_fs(
            tmpl, fs, STRS(BURROW_S("file1.tmpl"), BURROW_S("file2.tmpl")), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "error parsing files: %v", err);
            goto out;
        }
        test_execute(t, multi_exec_tests, NTESTS(multi_exec_tests), tmpl);
    }

    {
        TextTemplate *tmpl = text_template_new(a, BURROW_S("root"));
        Error err = BURROW_NO_ERROR;
        text_template_parse_fs(tmpl, fs, STRS(BURROW_S("file*.tmpl")), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "error parsing files: %v", err);
            goto out;
        }
        test_execute(t, multi_exec_tests, NTESTS(multi_exec_tests), tmpl);
    }
out:
    arena_free(&ar);
}

static void TestParseFilesWithData(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse_files(
        text_template_new(a, BURROW_S("root")),
        STRS(tdata(a, "tmpl1.tmpl"), tdata(a, "tmpl2.tmpl")), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error parsing files: %v", err);
    else
        test_execute(t, template_file_exec_tests, NTESTS(template_file_exec_tests),
                     tmpl);
    arena_free(&ar);
}

static void TestParseGlobWithData(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse_glob(
        text_template_new(a, BURROW_S("root")), tdata(a, "tmpl*.tmpl"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error parsing files: %v", err);
    else
        test_execute(t, template_file_exec_tests, NTESTS(template_file_exec_tests),
                     tmpl);
    arena_free(&ar);
}

#define CLONE_TEXT1 "{{define \"a\"}}{{template \"b\"}}{{template \"c\"}}{{end}}"
#define CLONE_TEXT2 "{{define \"b\"}}b{{end}}"
#define CLONE_TEXT3 "{{define \"c\"}}root{{end}}"
#define CLONE_TEXT4 "{{define \"c\"}}clone{{end}}"
#define CLONE_TEXT5 "{{define \"e\"}}{{.Foo}}{{end}}"

static void TestClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int zero = 0;
    Any data = BURROW_ANY(TYPE_INT, &zero);
    /* Create some templates and clone the root. */
    Error err = BURROW_NO_ERROR;
    TextTemplate *root = text_template_parse(text_template_new(a, BURROW_S("root")),
                                             BURROW_S(CLONE_TEXT1), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    text_template_parse(root, BURROW_S(CLONE_TEXT2), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    text_template_parse(root, BURROW_S(CLONE_TEXT5), &err);
    err = BURROW_NO_ERROR;
    text_template_option(root, STRS(BURROW_S("missingkey=error")));
    TextTemplate *clone = text_template_clone(root, a, &err);
    clone = text_template_must(clone, err);
    /* Add variants to both. */
    text_template_parse(root, BURROW_S(CLONE_TEXT3), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    text_template_parse(clone, BURROW_S(CLONE_TEXT4), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    /* Verify that the clone is self-consistent. */
    MapIter it = map_iter(clone->common->tmpl);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str key = *(const Str *)k;
        TextTemplate *tv = *(TextTemplate **)v;
        if (str_eq(key, clone->name) && tmpl_of(tv, key) != clone)
            testing_t_error_v(t, "clone does not contain root");
        if (tv != tmpl_of(tv, tv->name))
            testing_t_errorf_v(t, "clone does not contain self for %q", key);
    }
    /* Execute root. */
    StringsBuilder b = STRINGS_BUILDER(a);
    err = text_template_execute_template(root, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), data);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    if (!str_eq(strings_builder_string(&b), BURROW_S("broot")))
        testing_t_errorf_v(t, "expected %q got %q", BURROW_S("broot"),
                           strings_builder_string(&b));
    /* Execute copy. */
    strings_builder_reset(&b);
    err = text_template_execute_template(clone, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), data);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    if (!str_eq(strings_builder_string(&b), BURROW_S("bclone")))
        testing_t_errorf_v(t, "expected %q got %q", BURROW_S("bclone"),
                           strings_builder_string(&b));
    strings_builder_reset(&b);
    Map *empty = map_make(a, TYPE_STRING, TYPE_OF(Any), 0);
    Any empty_data = BURROW_ANY(TYPE_OF(StrAnyMap), &empty);
    Error root_err = text_template_execute_template(
        root, strings_builder_as_io_writer(&b), BURROW_S("e"), empty_data);
    Error clone_err = text_template_execute_template(
        clone, strings_builder_as_io_writer(&b), BURROW_S("e"), empty_data);
    if (BURROW_OK(clone_err)) {
        testing_t_errorf_v(t, "expected error from missing key in cloned template");
    } else {
        Str got = error_text(clone_err);
        Str want = BURROW_OK(root_err) ? BURROW_STR_EMPTY : error_text(root_err);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "got %q, wan t %q", got, want);
    }
out:
    arena_free(&ar);
}

/* parse.Parse with no functions, which none of the texts below call. */
static Map *parse_text(Alloc *a, const char *name, const char *text, Error *err) {
    return parse_parse(a, str_from_cstr(name), str_from_cstr(text), BURROW_STR_EMPTY,
                       BURROW_STR_EMPTY, (Slice){NULL, 0, 0, NULL}, err);
}

static ParseTree *tree_of(Map *trees, const char *name) {
    Str key = str_from_cstr(name);
    ParseTree **p = map_get(trees, &key);
    return p == NULL ? NULL : *p;
}

static void TestAddParseTree(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    /* Create some templates. */
    Error err = BURROW_NO_ERROR;
    TextTemplate *root = text_template_parse(text_template_new(a, BURROW_S("root")),
                                             BURROW_S(CLONE_TEXT1), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    text_template_parse(root, BURROW_S(CLONE_TEXT2), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    /* Add a new parse tree. */
    Map *tree = parse_text(a, "cloneText3", CLONE_TEXT3, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    TextTemplate *added =
        text_template_add_parse_tree(root, BURROW_S("c"), tree_of(tree, "c"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    /* Execute. */
    StringsBuilder b = STRINGS_BUILDER(a);
    Int zero = 0;
    err = text_template_execute_template(added, strings_builder_as_io_writer(&b),
                                         BURROW_S("a"), BURROW_ANY(TYPE_INT, &zero));
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        goto out;
    }
    if (!str_eq(strings_builder_string(&b), BURROW_S("broot")))
        testing_t_errorf_v(t, "expected %q got %q", BURROW_S("broot"),
                           strings_builder_string(&b));
out:
    arena_free(&ar);
}

/* Issue 7032 */
static void TestAddParseTreeToUnparsedTemplate(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    TextTemplate *tmpl = text_template_new(a, BURROW_S("master"));
    Error err = BURROW_NO_ERROR;
    Map *tree = parse_text(a, "master", "{{define \"master\"}}{{end}}", &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "unexpected parse err: %v", err);
    } else {
        ParseTree *master_tree = tree_of(tree, "master");
        /* used to panic */
        text_template_add_parse_tree(tmpl, BURROW_S("master"), master_tree, &err);
    }
    arena_free(&ar);
}

static void TestRedefinition(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl =
        text_template_parse(text_template_new(a, BURROW_S("tmpl1")),
                            BURROW_S("{{define \"test\"}}foo{{end}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "parse 1: %v", err);
        goto out;
    }
    text_template_parse(tmpl, BURROW_S("{{define \"test\"}}bar{{end}}"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "got error %v, expected nil", err);
        goto out;
    }
    text_template_parse(text_template_new_associated(tmpl, BURROW_S("tmpl2")),
                        BURROW_S("{{define \"test\"}}bar{{end}}"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "got error %v, expected nil", err);
out:
    arena_free(&ar);
}

/* Issue 10879 */
static void TestEmptyTemplateCloneCrash(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    TextTemplate *t1 = text_template_new(a, BURROW_S("base"));
    Error err = BURROW_NO_ERROR;
    (void)text_template_clone(t1, a, &err); /* used to panic */
    arena_free(&ar);
}

/* Issue 10910, 10926 */
static void TestTemplateLookUp(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    TextTemplate *t1 = text_template_new(a, BURROW_S("foo"));
    if (text_template_lookup(t1, BURROW_S("foo")) != NULL)
        testing_t_error_v(t,
                          "Lookup returned non-nil value for undefined template foo");
    text_template_new_associated(t1, BURROW_S("bar"));
    if (text_template_lookup(t1, BURROW_S("bar")) != NULL)
        testing_t_error_v(t,
                          "Lookup returned non-nil value for undefined template bar");
    Error err = BURROW_NO_ERROR;
    text_template_parse(t1, BURROW_S("{{define \"foo\"}}test{{end}}"), &err);
    if (text_template_lookup(t1, BURROW_S("foo")) == NULL)
        testing_t_error_v(t, "Lookup returned nil value for defined template");
    arena_free(&ar);
}

static void TestNew(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    /* template with same name already exists */
    Error err = BURROW_NO_ERROR;
    TextTemplate *t1 =
        text_template_parse(text_template_new(a, BURROW_S("test")),
                            BURROW_S("{{define \"test\"}}foo{{end}}"), &err);
    TextTemplate *t2 = text_template_new_associated(t1, BURROW_S("test"));

    if (t1->common != t2->common)
        testing_t_errorf_v(t, "t1 & t2 didn't share common struct; got %p != %p",
                           (void *)t1->common, (void *)t2->common);
    if (t1->tree == NULL)
        testing_t_error_v(t, "defined template got nil Tree");
    if (t2->tree != NULL)
        testing_t_error_v(t, "undefined template got non-nil Tree");

    bool contains_t1 = false;
    Slice all = text_template_templates(t1, a);
    for (Int i = 0; i < all.len; i++) {
        TextTemplate *tmpl = ((TextTemplate **)all.p)[i];
        if (tmpl == t2)
            testing_t_error_v(t, "Templates included undefined template");
        if (tmpl == t1)
            contains_t1 = true;
    }
    if (!contains_t1)
        testing_t_error_v(t, "Templates didn't include defined template");
    arena_free(&ar);
}

static void TestParse(TestingT *t) {
    /* In multiple calls to Parse with the same receiver template, only one call
     * can contain text other than space, comments, and template definitions */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const texts[] = {
        "{{define \"test\"}}{{end}}",
        "{{define \"test\"}}{{/* this is a comment */}}{{end}}",
        "{{define \"test\"}}foo{{end}}",
    };
    TextTemplate *t1 = text_template_new(a, BURROW_S("test"));
    for (size_t i = 0; i < NTESTS(texts); i++) {
        Error err = BURROW_NO_ERROR;
        text_template_parse(t1, str_from_cstr(texts[i]), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "parsing test: %s", err);
            break;
        }
    }
    arena_free(&ar);
}

static void TestEmptyTemplate(TestingT *t) {
    static const struct {
        int n;
        const char *defn[2];
        const char *in;
        const char *want;
    } cases[] = {
        {2, {"x", "y"}, "", "y"},
        {1, {""}, "once", ""},
        {2, {"", ""}, "twice", ""},
        {2, {"{{.}}", "{{.}}"}, "twice", "twice"},
        {2, {"{{/* a comment */}}", "{{/* a comment */}}"}, "comment", ""},
        {2, {"{{.}}", ""}, "twice", ""},
    };

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NTESTS(cases); i++) {
        TextTemplate *root = text_template_new(a, BURROW_S("root"));
        Str in = str_from_cstr(cases[i].in);

        TextTemplate *m = NULL;
        Error err = BURROW_NO_ERROR;
        for (int j = 0; j < cases[i].n; j++) {
            m = text_template_parse(text_template_new_associated(root, in),
                                    str_from_cstr(cases[i].defn[j]), &err);
            if (BURROW_FAILED(err)) {
                testing_t_fatal_v(t, err);
                goto out;
            }
        }
        StringsBuilder buf = STRINGS_BUILDER(a);
        err = text_template_execute(m, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_STRING, &in));
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, (Int)i, err);
            continue;
        }
        Str want = str_from_cstr(cases[i].want);
        if (!str_eq(strings_builder_string(&buf), want))
            testing_t_errorf_v(t, "expected string %q: got %q", want,
                               strings_builder_string(&buf));
    }
out:
    arena_free(&ar);
}

/* Issue 19249 was a regression in 1.8 caused by the handling of empty
 * templates added in that release, which got different answers depending on
 * the order templates appeared in the internal map. */
static void TestIssue19294(TestingT *t) {
    /* The empty block in "xhtml" should be replaced during execution by the
     * contents of "stylesheet", but if the internal map associating names with
     * templates is built in the wrong order, the empty block looks non-empty
     * and this doesn't happen. */
    static const char *const all[][2] = {
        {"stylesheet", "{{define \"stylesheet\"}}stylesheet{{end}}"},
        {"xhtml", "{{block \"stylesheet\" .}}{{end}}"},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Int zero = 0;
    for (int i = 0; i < 100; i++) {
        arena_reset(&ar);
        Alloc *a = arena_allocator(&ar);
        Error err = BURROW_NO_ERROR;
        TextTemplate *res =
            text_template_parse(text_template_new(a, BURROW_S("title.xhtml")),
                                BURROW_S("{{template \"xhtml\" .}}"), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, err);
            break;
        }
        for (size_t j = 0; j < NTESTS(all); j++) {
            text_template_parse(
                text_template_new_associated(res, str_from_cstr(all[j][0])),
                str_from_cstr(all[j][1]), &err);
            if (BURROW_FAILED(err))
                break;
        }
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, err);
            break;
        }
        StringsBuilder buf = STRINGS_BUILDER(a);
        (void)text_template_execute(res, strings_builder_as_io_writer(&buf),
                                    BURROW_ANY(TYPE_INT, &zero));
        if (!str_eq(strings_builder_string(&buf), BURROW_S("stylesheet"))) {
            testing_t_fatalf_v(t, "iteration %d: got %q; expected %q", i,
                               strings_builder_string(&buf), BURROW_S("stylesheet"));
            break;
        }
    }
    arena_free(&ar);
}

/* Issue 48436 */
static void TestAddToZeroTemplate(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Map *tree = parse_text(a, "c", CLONE_TEXT3, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
    } else {
        TextTemplate tmpl = TEXT_TEMPLATE(a);
        text_template_add_parse_tree(&tmpl, BURROW_S("x"), tree_of(tree, "c"), &err);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- the setup */

/* Go's testdata directory. */
static const struct {
    const char *name;
    const char *text;
} testdata[] = {
    {"file1.tmpl", "{{define \"x\"}}TEXT{{end}}\n{{define \"dotV\"}}{{.V}}{{end}}\n"},
    {"file2.tmpl", "{{define \"dot\"}}{{.}}{{end}}\n{{define \"nested\"}}{{template "
                   "\"dot\" .}}{{end}}\n"},
    {"tmpl1.tmpl", "template1\n{{define \"x\"}}x{{end}}\n{{template \"y\"}}\n"},
    {"tmpl2.tmpl", "template2\n{{define \"y\"}}y{{end}}\n{{template \"x\"}}\n"},
};

static int tt_main(TestingM *m) {
    arena_init(&tt_arena, heap_allocator(), 0);
    tt_a = arena_allocator(&tt_arena);
    setup(tt_a);
    big_int = fmt_sprintf_v(tt_a, "0x%x", (Int)BURROW_INT_MAX);
    big_uint = fmt_sprintf_v(tt_a, "0x%x", (Uint)BURROW_INT_MAX + 1);
    Error err = BURROW_NO_ERROR;
    tdir = os_mkdir_temp(tt_a, BURROW_STR_EMPTY, BURROW_S("tt"), &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v("text_template_test:", err);
        arena_free(&tt_arena);
        return 1;
    }
    for (size_t i = 0; i < NTESTS(testdata); i++) {
        Str text = str_from_cstr(testdata[i].text);
        err = os_write_file(
            tdata(tt_a, testdata[i].name),
            slice_from((void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE), 0644);
        if (BURROW_FAILED(err)) {
            fmt_println_v("text_template_test:", err);
            (void)os_remove_all(tdir);
            arena_free(&tt_arena);
            return 1;
        }
    }
    int r = testing_m_run(m);
    (void)os_remove_all(tdir);
    arena_free(&tt_arena);
    return r;
}

#define TESTS(X)                                                                       \
    X(TestExecute)                                                                     \
    X(TestDelims)                                                                      \
    X(TestExecuteError)                                                                \
    X(TestExecError)                                                                   \
    X(TestExecError_CustomError)                                                       \
    X(TestJSEscaping)                                                                  \
    X(TestTree)                                                                        \
    X(TestExecuteOnNewTemplate)                                                        \
    X(TestMessageForExecuteEmpty)                                                      \
    X(TestFinalForPrintf)                                                              \
    X(TestComparison)                                                                  \
    X(TestMissingMapKey)                                                               \
    X(TestUnterminatedStringError)                                                     \
    X(TestExecuteGivesExecError)                                                       \
    X(TestGoodFuncNames)                                                               \
    X(TestBadFuncNames)                                                                \
    X(TestIsTrue)                                                                      \
    X(TestBlock)                                                                       \
    X(TestEvalFieldErrors)                                                             \
    X(TestMaxExecDepth)                                                                \
    X(TestAddrOfIndex)                                                                 \
    X(TestInterfaceValues)                                                             \
    X(TestExecutePanicDuringCall)                                                      \
    X(TestFunctionCheckDuringCall)                                                     \
    X(TestIssue31810)                                                                  \
    X(TestIssue43065)                                                                  \
    X(TestIssue39807)                                                                  \
    X(TestMultiParse)                                                                  \
    X(TestMultiExecute)                                                                \
    X(TestParseFiles)                                                                  \
    X(TestParseGlob)                                                                   \
    X(TestParseFS)                                                                     \
    X(TestParseFilesWithData)                                                          \
    X(TestParseGlobWithData)                                                           \
    X(TestClone)                                                                       \
    X(TestAddParseTree)                                                                \
    X(TestAddParseTreeToUnparsedTemplate)                                              \
    X(TestRedefinition)                                                                \
    X(TestEmptyTemplateCloneCrash)                                                     \
    X(TestTemplateLookUp)                                                              \
    X(TestNew)                                                                         \
    X(TestParse)                                                                       \
    X(TestEmptyTemplate)                                                               \
    X(TestIssue19294)                                                                  \
    X(TestAddToZeroTemplate)

TESTING_MAIN_WITH(tt_main, TESTS)
