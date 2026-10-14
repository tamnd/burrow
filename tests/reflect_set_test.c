/* Tests for reflect, the half that writes.
 *
 * These are Go's tests from all_test.go for the setters, New, Zero, Addr, the
 * makers, Append, Copy, Slice, channels and the type constructors. Each keeps
 * Go's name. Where a test also needs Convert, Call, FuncOf or methods, which
 * come later, the part that needs them is left out and the comment says so.
 *
 * Go's checkSameType asks that a made type be the descriptor the compiler made
 * for the same type. C has no way to find that one, so check_same_type asks
 * what can be asked: that the two read as the same type and that the made
 * type's zero value carries it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A descriptor written out, for the types the declare macros do not make. */
#define TYPE_DESC(name, pkg, kind, C, nfield, fields, elem, key, len)                  \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)sizeof(C),                                                           \
        (uint16_t)_Alignof(C),                                                         \
        nfield,                                                                        \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        len,                                                                           \
        0,                                                                             \
        NULL,                                                                          \
    }

#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

/* ValueOf(&x).Elem() for the t at p, which is a Value that can be set. */
static ReflectValue settable(const Type *t, void *p) {
    return reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(t), &p)));
}

static ReflectType ints_t(void) {
    return reflect_slice_of(TYPE_INT);
}

static Slice ints(Alloc *a, const Int *v, Int n) {
    Slice s = slice_make(a, TYPE_INT, n, n);
    if (n > 0)
        memcpy(s.p, v, (size_t)n * sizeof(Int));
    return s;
}

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

/* Runs f(env) and copies the text of its panic, if there was one, into buf. */
static bool panics(void (*f)(void *), void *env, char *buf, size_t n) {
    volatile bool got = false;
    buf[0] = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str text = panic_text(r);
        size_t k = (size_t)text.len < n - 1 ? (size_t)text.len : n - 1;
        memcpy(buf, text.p, k);
        buf[k] = 0;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* Go's shouldPanic: f has to panic, and when expect is not empty the text has
 * to start with "reflect" and contain expect. */
static void should_panic(TestingT *t, const char *expect, void (*f)(void *),
                         void *env) {
    char buf[256];
    if (!panics(f, env, buf, sizeof buf)) {
        testing_t_errorf_v(t, "did not panic, want %q", expect);
        return;
    }
    if (expect[0] == 0)
        return;
    if (strncmp(buf, "reflect", 7) != 0)
        testing_t_errorf_v(t, "panic string does not start with \"reflect\": %s", buf);
    if (strstr(buf, expect) == NULL)
        testing_t_errorf_v(t, "%q does not contain %q", buf, expect);
}

/* What the functions handed to should_panic work on. */
typedef struct Op {
    Alloc *a;
    ReflectValue v, x, y;
    ReflectMapIter *it;
    ReflectType t, u;
    Int i, j, k;
    Slice xs;
} Op;

static void op_set_zero(void *env) {
    Op *o = env;
    reflect_value_set(o->v, reflect_zero(reflect_value_type(o->v)));
}

static void op_grow(void *env) {
    Op *o = env;
    reflect_value_grow(o->v, o->a, o->i);
}

static void op_append(void *env) {
    Op *o = env;
    (void)reflect_append(o->a, o->v, o->xs);
}

static void op_append_slice(void *env) {
    Op *o = env;
    (void)reflect_append_slice(o->a, o->v, o->x);
}

static void op_map_index(void *env) {
    Op *o = env;
    (void)reflect_value_map_index(o->v, o->x);
}

static void op_set_map_index(void *env) {
    Op *o = env;
    reflect_value_set_map_index(o->v, o->x, o->y);
}

static void op_close(void *env) {
    Op *o = env;
    reflect_value_close(o->v);
}

static void op_slice3(void *env) {
    Op *o = env;
    (void)reflect_value_slice3(o->v, o->a, o->i, o->j, o->k);
}

static void op_set_len(void *env) {
    Op *o = env;
    reflect_value_set_len(o->v, o->i);
}

static void op_set_cap(void *env) {
    Op *o = env;
    reflect_value_set_cap(o->v, o->i);
}

static void op_map_of(void *env) {
    Op *o = env;
    (void)reflect_map_of(o->t, o->u);
}

static void op_array_of(void *env) {
    Op *o = env;
    (void)reflect_array_of(o->i, o->t);
}

static void op_set_iter_key(void *env) {
    Op *o = env;
    reflect_value_set_iter_key(o->v, o->it);
}

static void op_set_iter_value(void *env) {
    Op *o = env;
    reflect_value_set_iter_value(o->v, o->it);
}

static void op_clear(void *env) {
    Op *o = env;
    reflect_value_clear(o->v);
}

static void op_make_slice(void *env) {
    Op *o = env;
    (void)reflect_make_slice(o->a, o->t, o->i, o->j);
}

/* ------------------------------------------------------------ valueToString */

static void put(StringsBuilder *b, Str s) {
    (void)strings_builder_write_string(b, s, NULL);
}

static void put_float(StringsBuilder *b, double f) {
    put(b, strconv_format_float(b->a, f, 'g', -1, 64));
}

/* Go's valueToString, from tostring_test.go. */
static void vts(StringsBuilder *b, ReflectValue v) {
    if (!reflect_value_is_valid(v)) {
        put(b, BURROW_S("<zero Value>"));
        return;
    }
    ReflectType typ = reflect_value_type(v);
    switch ((int)reflect_value_kind(v)) {
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        put(b, strconv_format_int(b->a, reflect_value_int(v), 10));
        return;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        put(b, strconv_format_uint(b->a, reflect_value_uint(v), 10));
        return;
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        put_float(b, reflect_value_float(v));
        return;
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        Complex128 c = reflect_value_complex(v);
        put_float(b, c.re);
        put(b, BURROW_S("+"));
        put_float(b, c.im);
        put(b, BURROW_S("i"));
        return;
    }
    case KIND_STRING:
        put(b, reflect_value_string(v));
        return;
    case KIND_BOOL:
        put(b, reflect_value_bool(v) ? BURROW_S("true") : BURROW_S("false"));
        return;
    case KIND_POINTER:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("("));
        if (reflect_value_is_nil(v)) {
            put(b, BURROW_S("0"));
        } else {
            put(b, BURROW_S("&"));
            vts(b, reflect_value_elem(v));
        }
        put(b, BURROW_S(")"));
        return;
    case KIND_ARRAY:
    case KIND_SLICE:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("{"));
        for (Int i = 0; i < reflect_value_len(v); i++) {
            if (i > 0)
                put(b, BURROW_S(", "));
            vts(b, reflect_value_index(v, i));
        }
        put(b, BURROW_S("}"));
        return;
    case KIND_MAP:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("{<can't iterate on maps>}"));
        return;
    case KIND_CHAN:
        put(b, reflect_type_string(typ));
        return;
    case KIND_STRUCT:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("{"));
        for (Int i = 0, n = reflect_value_num_field(v); i < n; i++) {
            if (i > 0)
                put(b, BURROW_S(", "));
            vts(b, reflect_value_field(v, i));
        }
        put(b, BURROW_S("}"));
        return;
    case KIND_INTERFACE:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("("));
        vts(b, reflect_value_elem(v));
        put(b, BURROW_S(")"));
        return;
    case KIND_FUNC:
        put(b, reflect_type_string(typ));
        put(b, BURROW_S("("));
        put(b, strconv_format_uint(b->a, (uint64_t)reflect_value_pointer(v), 10));
        put(b, BURROW_S(")"));
        return;
    default:
        panic_str(BURROW_S("valueToString: can't print type"));
    }
}

static Str value_to_string(Alloc *a, ReflectValue v) {
    StringsBuilder b = STRINGS_BUILDER(a);
    vts(&b, v);
    return strings_builder_string(&b);
}

/* fmt.Sprint(x). */
static Str sprint(Alloc *a, Any x) {
    Any args[1] = {x};
    return fmt_sprint(a, (Slice){args, 1, 1, TYPE_ANY});
}

/* ------------------------------------------------------------------ types */

static const Type type_integer =
    TYPE_DESC(S_("integer"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);

/* struct { c chan *int32; d float32 } and its friends from valueTests. */
static const Type type_ptr_int32 =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, TYPE_INT32, NULL, 0);
static const Type type_chan_ptr_int32 =
    TYPE_DESC(NONAME, NONAME, KIND_CHAN, void *, 0, NULL, &type_ptr_int32, NULL, 0);

typedef struct CD {
    void *c;
    float d;
} CD;
static const Field cd_fields[] = {
    {S_("c"), NONAME, &type_chan_ptr_int32, offsetof(CD, c)},
    {S_("d"), NONAME, TYPE_FLOAT32, offsetof(CD, d)},
};
static const Type type_CD =
    TYPE_DESC(NONAME, NONAME, KIND_STRUCT, CD, 2, cd_fields, NULL, NULL, 0);

typedef struct AB {
    int8_t a;
    int32_t b;
} AB;
static const Field ab_fields[] = {
    {S_("a"), NONAME, TYPE_INT8, offsetof(AB, a)},
    {S_("b"), NONAME, TYPE_INT32, offsetof(AB, b)},
};
static const Type type_AB =
    TYPE_DESC(NONAME, NONAME, KIND_STRUCT, AB, 2, ab_fields, NULL, NULL, 0);

typedef struct ABC {
    int8_t a;
    int8_t b;
    int32_t c;
} ABC;
static const Field abc_fields[] = {
    {S_("a"), NONAME, TYPE_INT8, offsetof(ABC, a)},
    {S_("b"), NONAME, TYPE_INT8, offsetof(ABC, b)},
    {S_("c"), NONAME, TYPE_INT32, offsetof(ABC, c)},
};
static const Type type_ABC =
    TYPE_DESC(NONAME, NONAME, KIND_STRUCT, ABC, 3, abc_fields, NULL, NULL, 0);

/* Go's valueTests: what new() is given, and what valueToString says about
 * it once TestSet has set it. The two func entries need FuncOf to be written
 * down and are left out until it arrives. */
typedef struct ValueTest {
    const Type *t;
    const char *s;
} ValueTest;

static Int value_tests(ValueTest *vt) {
    Int n = 0;
#define VT(type, str) vt[n++] = (ValueTest){(type), (str)}
    VT(TYPE_INT, "132");
    VT(TYPE_INT8, "8");
    VT(TYPE_INT16, "16");
    VT(TYPE_INT32, "32");
    VT(TYPE_INT64, "64");
    VT(TYPE_UINT, "132");
    VT(TYPE_UINT8, "8");
    VT(TYPE_UINT16, "16");
    VT(TYPE_UINT32, "32");
    VT(TYPE_UINT64, "64");
    VT(TYPE_FLOAT32, "256.25");
    VT(TYPE_FLOAT64, "512.125");
    VT(TYPE_COMPLEX64, "532.125+10i");
    VT(TYPE_COMPLEX128, "564.25+1i");
    VT(TYPE_STRING, "stringy cheese");
    VT(TYPE_BOOL, "true");
    VT(reflect_pointer_to(TYPE_INT8), "*int8(0)");
    VT(reflect_pointer_to(reflect_pointer_to(TYPE_INT8)), "**int8(0)");
    VT(reflect_array_of(5, TYPE_INT32), "[5]int32{0, 0, 0, 0, 0}");
    VT(reflect_pointer_to(reflect_pointer_to(&type_integer)),
       "**reflect_test.integer(0)");
    VT(reflect_map_of(TYPE_STRING, TYPE_INT32),
       "map[string]int32{<can't iterate on maps>}");
    VT(reflect_chan_of(REFLECT_SEND_DIR, TYPE_STRING), "chan<- string");
    VT(&type_CD, "struct { c chan *int32; d float32 }{chan *int32, 0}");
    VT(&type_AB, "struct { a int8; b int32 }{0, 0}");
    VT(&type_ABC, "struct { a int8; b int8; c int32 }{0, 0, 0}");
#undef VT
    return n;
}

#define MAX_VALUE_TESTS 32

/* Go's checkSameType, as far as C can take it. See the top of the file. */
static void check_same_type(TestingT *t, ReflectType x, ReflectType y) {
    ReflectType z = reflect_type_of(reflect_value_interface(reflect_zero(x)));
    if (!str_eq(reflect_type_string(x), reflect_type_string(y)) ||
        reflect_type_kind(x) != reflect_type_kind(y) ||
        reflect_type_size(x) != reflect_type_size(y) || z != x)
        testing_t_errorf_v(t, "did not find preexisting type for %s (vs %s)",
                           reflect_type_string(x), reflect_type_string(y));
}

/* ------------------------------------------------------------- the setters */

static void TestSet(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ValueTest vt[MAX_VALUE_TESTS];
    Int n = value_tests(vt);
    for (Int i = 0; i < n; i++) {
        ReflectValue v = reflect_new(a, vt[i].t);
        v = reflect_value_elem(v);
        switch ((int)reflect_value_kind(v)) {
        case KIND_INT:
            reflect_value_set_int(v, 132);
            break;
        case KIND_INT8:
            reflect_value_set_int(v, 8);
            break;
        case KIND_INT16:
            reflect_value_set_int(v, 16);
            break;
        case KIND_INT32:
            reflect_value_set_int(v, 32);
            break;
        case KIND_INT64:
            reflect_value_set_int(v, 64);
            break;
        case KIND_UINT:
            reflect_value_set_uint(v, 132);
            break;
        case KIND_UINT8:
            reflect_value_set_uint(v, 8);
            break;
        case KIND_UINT16:
            reflect_value_set_uint(v, 16);
            break;
        case KIND_UINT32:
            reflect_value_set_uint(v, 32);
            break;
        case KIND_UINT64:
            reflect_value_set_uint(v, 64);
            break;
        case KIND_FLOAT32:
            reflect_value_set_float(v, 256.25);
            break;
        case KIND_FLOAT64:
            reflect_value_set_float(v, 512.125);
            break;
        case KIND_COMPLEX64:
            reflect_value_set_complex(v, (Complex128){532.125, 10});
            break;
        case KIND_COMPLEX128:
            reflect_value_set_complex(v, (Complex128){564.25, 1});
            break;
        case KIND_STRING:
            reflect_value_set_string(v, BURROW_S("stringy cheese"));
            break;
        case KIND_BOOL:
            reflect_value_set_bool(v, true);
            break;
        default:
            break;
        }
        Str s = value_to_string(a, v);
        if (!str_eq(s, cs(vt[i].s)))
            testing_t_errorf_v(t, "#%d: have %q, want %q", i, s, vt[i].s);
    }
    arena_free(&ar);
}

static void TestSetValue(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ValueTest vt[MAX_VALUE_TESTS];
    Int n = value_tests(vt);
    for (Int i = 0; i < n; i++) {
        ReflectValue v = reflect_value_elem(reflect_new(a, vt[i].t));
#define SETV(type, C, x)                                                               \
    do {                                                                               \
        C x_ = (x);                                                                    \
        reflect_value_set(v, val((type), &x_));                                        \
    } while (0)
        switch ((int)reflect_value_kind(v)) {
        case KIND_INT:
            SETV(TYPE_INT, Int, 132);
            break;
        case KIND_INT8:
            SETV(TYPE_INT8, int8_t, 8);
            break;
        case KIND_INT16:
            SETV(TYPE_INT16, int16_t, 16);
            break;
        case KIND_INT32:
            SETV(TYPE_INT32, int32_t, 32);
            break;
        case KIND_INT64:
            SETV(TYPE_INT64, int64_t, 64);
            break;
        case KIND_UINT:
            SETV(TYPE_UINT, Uint, 132);
            break;
        case KIND_UINT8:
            SETV(TYPE_UINT8, uint8_t, 8);
            break;
        case KIND_UINT16:
            SETV(TYPE_UINT16, uint16_t, 16);
            break;
        case KIND_UINT32:
            SETV(TYPE_UINT32, uint32_t, 32);
            break;
        case KIND_UINT64:
            SETV(TYPE_UINT64, uint64_t, 64);
            break;
        case KIND_FLOAT32:
            SETV(TYPE_FLOAT32, float, 256.25F);
            break;
        case KIND_FLOAT64:
            SETV(TYPE_FLOAT64, double, 512.125);
            break;
        case KIND_COMPLEX64:
            SETV(TYPE_COMPLEX64, Complex64, ((Complex64){532.125F, 10}));
            break;
        case KIND_COMPLEX128:
            SETV(TYPE_COMPLEX128, Complex128, ((Complex128){564.25, 1}));
            break;
        case KIND_STRING:
            SETV(TYPE_STRING, Str, BURROW_S("stringy cheese"));
            break;
        case KIND_BOOL:
            SETV(TYPE_BOOL, bool, true);
            break;
        default:
            break;
        }
#undef SETV
        Str s = value_to_string(a, v);
        if (!str_eq(s, cs(vt[i].s)))
            testing_t_errorf_v(t, "#%d: have %q, want %q", i, s, vt[i].s);
    }
    arena_free(&ar);
}

/* The word an interface value points at, which is what two pointer values
 * compare by. */
static void *any_word(Any x) {
    void *w = NULL;
    if (x.data != NULL)
        memcpy(&w, x.data, sizeof w);
    return w;
}

/* map[string]any, from valueTests, for TestMapIterSet and TestClear. */
static Map *value_tests_map(Alloc *a, ReflectType *mt) {
    ValueTest vt[MAX_VALUE_TESTS];
    Int n = value_tests(vt);
    *mt = reflect_map_of(TYPE_STRING, TYPE_ANY);
    Map *m = map_make(a, TYPE_STRING, TYPE_ANY, n);
    for (Int i = 0; i < n; i++) {
        Str k = cs(vt[i].s);
        Any v = reflect_value_interface(reflect_new(a, vt[i].t));
        map_set(m, &k, &v);
    }
    return m;
}

static void TestMapIterSet(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType mt;
    Map *m = value_tests_map(a, &mt);
    ReflectValue v = val(mt, &m);

    ReflectValue k = reflect_value_elem(reflect_new(a, reflect_type_key(mt)));
    ReflectValue e = reflect_value_elem(reflect_new(a, reflect_type_elem(mt)));

    ReflectMapIter *iter = reflect_value_map_range(v, a);
    while (reflect_map_iter_next(iter)) {
        reflect_value_set_iter_key(k, iter);
        reflect_value_set_iter_value(e, iter);
        Str ks = reflect_value_string(k);
        const Any *want = map_get(m, &ks);
        Any got = reflect_value_interface(e);
        if (want == NULL || got.t != want->t || any_word(got) != any_word(*want))
            testing_t_errorf_v(t, "%q: want %v, got %v", ks,
                               want == NULL ? BURROW_S("nothing")
                                            : reflect_type_string(want->t),
                               reflect_type_string(got.t));
        Str setkey = value_to_string(a, k);
        Str key = value_to_string(a, reflect_map_iter_key(iter));
        if (!str_eq(setkey, key))
            testing_t_errorf_v(t, "MapIter.Key() = %q, MapIter.SetKey() = %q", key,
                               setkey);
        Str setval = value_to_string(a, e);
        Str value = value_to_string(a, reflect_map_iter_value(iter));
        if (!str_eq(setval, value))
            testing_t_errorf_v(t, "MapIter.Value() = %q, MapIter.SetValue() = %q",
                               value, setval);
    }
    /* Go goes on to count allocations, which here are whatever a is asked
     * for and nothing else. */
    arena_free(&ar);
}

static void TestArrayElemSet(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    ReflectValue v = settable(reflect_array_of(10, TYPE_INT), arr);
    reflect_value_set_int(reflect_value_index(v, 4), 123);
    Str s = value_to_string(a, v);
    const char *want = "[10]int{1, 2, 3, 4, 123, 6, 7, 8, 9, 10}";
    if (!str_eq(s, cs(want)))
        testing_t_errorf_v(t, "[10]int: have %q want %q", s, want);

    Int vals[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    Slice sl = ints(a, vals, 10);
    v = val(ints_t(), &sl);
    reflect_value_set_int(reflect_value_index(v, 4), 123);
    s = value_to_string(a, v);
    const char *want1 = "[]int{1, 2, 3, 4, 123, 6, 7, 8, 9, 10}";
    if (!str_eq(s, cs(want1)))
        testing_t_errorf_v(t, "[]int: have %q want %q", s, want1);
    arena_free(&ar);
}

static void TestPtrPointTo(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType pt = reflect_pointer_to(TYPE_INT32);
    int32_t *ip = NULL;
    int32_t i = 1234;
    int32_t *ipp = &i;
    ReflectValue vip = val(reflect_pointer_to(pt), &(int32_t **){&ip});
    ReflectValue vi = reflect_value_elem(val(pt, &ipp));
    reflect_value_set(reflect_value_elem(vip), reflect_value_addr(vi, a));
    if (ip == NULL || *ip != 1234)
        testing_t_errorf_v(t, "got %d, want 1234", ip == NULL ? 0 : *ip);

    ip = NULL;
    ReflectValue vp = settable(pt, &ip);
    reflect_value_set(vp, reflect_zero(reflect_value_type(vp)));
    if (ip != NULL)
        testing_t_errorf_v(t, "got non-nil (%p), want nil", (void *)ip);
    arena_free(&ar);
}

static void TestPtrSetNil(TestingT *t) {
    int32_t i = 1234;
    int32_t *ip = &i;
    int32_t **ipp = &ip;
    ReflectValue vip = val(reflect_pointer_to(reflect_pointer_to(TYPE_INT32)), &ipp);
    reflect_value_set(reflect_value_elem(vip),
                      reflect_zero(reflect_value_type(reflect_value_elem(vip))));
    if (ip != NULL)
        testing_t_errorf_v(t, "got non-nil (%d), want nil", *ip);
}

static void TestMapSetNil(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    ReflectValue vm = settable(reflect_map_of(TYPE_STRING, TYPE_INT), &m);
    reflect_value_set(vm, reflect_zero(reflect_value_type(vm)));
    if (m != NULL)
        testing_t_errorf_v(t, "got non-nil (%p), want nil", (void *)m);
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Grow */

BURROW_PTR_TYPE(IntPtr, Int);
#define GROWT_FIELDS(F, T)                                                             \
    F(T, Int, a, "")                                                                   \
    F(T, double, b, "")                                                                \
    F(T, Str, c, "")                                                                   \
    F(T, IntPtr, d, "")
BURROW_STRUCT(GrowT, GROWT_FIELDS);
BURROW_SLICE_TYPE(GrowTs, GrowT);

static void test_grow_append(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    GrowTs got = slice_nil(TYPE_OF(GrowT));
    GrowTs want = slice_nil(TYPE_OF(GrowT));
    ReflectValue v = settable(TYPE_OF(GrowTs), &got);
    Int is[10];
    for (Int i = 0; i < 10; i++) {
        is[i] = i;
        GrowT vt = {i, (double)i, strconv_itoa(a, i), &is[i]};
        reflect_value_grow(v, a, 1);
        reflect_value_set_len(v, reflect_value_len(v) + 1);
        reflect_value_set(reflect_value_index(v, reflect_value_len(v) - 1),
                          val(TYPE_OF(GrowT), &vt));
        want = slice_append(a, want, &vt, 1);
    }
    if (!reflect_deep_equal(BURROW_ANY(TYPE_OF(GrowTs), &got),
                            BURROW_ANY(TYPE_OF(GrowTs), &want)))
        testing_t_errorf_v(t, "value mismatch: got %d elements, want %d", got.len,
                           want.len);
    arena_free(&ar);
}

static void test_grow_rate(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Slice b = slice_nil(TYPE_UINT8);
    ReflectValue v = reflect_value_elem(reflect_new(a, TYPE_BYTES));
    for (Int i = 0; i < 10; i++) {
        b.len = b.cap;
        b = slice_append(a, b, NULL, 1);
        reflect_value_set_len(v, reflect_value_cap(v));
        reflect_value_grow(v, a, 1);
        if (reflect_value_cap(v) != b.cap)
            testing_t_errorf_v(t, "v.Cap = %d, want %d", reflect_value_cap(v), b.cap);
    }
    arena_free(&ar);
}

static void test_grow_zero_capacity(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    for (Int i = 0; i < 10; i++) {
        ReflectValue v = reflect_value_elem(reflect_new(a, TYPE_BYTES));
        reflect_value_grow(v, a, 61);
        Slice b = reflect_value_bytes(v);
        b.len = b.cap;
        Byte *p = b.p;
        for (Int j = 0; j < b.len; j++) {
            if (p[j] != 0) {
                testing_t_fatalf_v(t, "Value.Bytes[%d] = 0x%02x, want 0x00", j, p[j]);
                break;
            }
            p[j] = 0xff;
        }
    }
    arena_free(&ar);
}

static void TestGrow(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Slice nil = slice_nil(TYPE_INT);
    Op o = {.a = a, .v = val(ints_t(), &nil), .i = 0};
    should_panic(t, "reflect.Value.Grow using unaddressable value", op_grow, &o);
    ReflectValue v = reflect_value_elem(reflect_new(a, ints_t()));
    reflect_value_grow(v, a, 0);
    if (!reflect_value_is_nil(v))
        testing_t_errorf_v(t, "v.Grow(0) should still be nil");
    reflect_value_grow(v, a, 1);
    if (reflect_value_cap(v) == 0)
        testing_t_errorf_v(t, "v.Cap = %d, want non-zero", reflect_value_cap(v));
    void *want = reflect_value_unsafe_pointer(v);
    reflect_value_grow(v, a, 1);
    void *got = reflect_value_unsafe_pointer(v);
    if (got != want)
        testing_t_errorf_v(t, "noop v.Grow should not change pointers");

    testing_t_run(t, BURROW_S("Append"),
                  BURROW_FN(TestingTFunc, test_grow_append, NULL));
    testing_t_run(t, BURROW_S("Rate"), BURROW_FN(TestingTFunc, test_grow_rate, NULL));
    testing_t_run(t, BURROW_S("ZeroCapacity"),
                  BURROW_FN(TestingTFunc, test_grow_zero_capacity, NULL));
    arena_free(&ar);
}

/* ------------------------------------------------------- Append and Copy */

BURROW_SLICE_TYPE(Ints, Int);
#define WITHINTS_FIELDS(F, T) F(T, Ints, x, "")
BURROW_STRUCT(WithInts, WITHINTS_FIELDS);

/* The shapes of appendTests' slices: nil, empty and not nil, and
 * make([]int, 2, 4). */
enum { NIL, EMPTY, MAKE24, LIST };

typedef struct AppendTest {
    int orig;
    int extra;
    Int list[3];
    Int nlist;
} AppendTest;

static const AppendTest append_tests[] = {
    {NIL, NIL, {0}, 0},      {EMPTY, NIL, {0}, 0},
    {NIL, EMPTY, {0}, 0},    {EMPTY, EMPTY, {0}, 0},
    {NIL, LIST, {22}, 1},    {EMPTY, LIST, {22}, 1},
    {MAKE24, NIL, {0}, 0},   {MAKE24, EMPTY, {0}, 0},
    {MAKE24, LIST, {22}, 1}, {MAKE24, LIST, {22, 33, 44}, 3},
};

static Slice append_shape(Alloc *a, int shape, const Int *list, Int n) {
    switch (shape) {
    case NIL:
        return slice_nil(TYPE_INT);
    case EMPTY:
        return slice_make(a, TYPE_INT, 0, 0);
    case MAKE24:
        return slice_make(a, TYPE_INT, 2, 4);
    default:
        return ints(a, list, n);
    }
}

static void TestAppend(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType it = TYPE_OF(Ints);
    for (Int i = 0; i < NELEM(append_tests); i++) {
        const AppendTest *test = &append_tests[i];
        Slice orig = append_shape(a, test->orig, NULL, 0);
        Slice extra = append_shape(a, test->extra, test->list, test->nlist);
        Int orig_len = orig.len, extra_len = extra.len;
        Slice want = slice_append(a, orig, extra.p, extra.len);
        /* Convert extra from []int to []Value. */
        ReflectValue *e0p =
            mem_alloc(a, sizeof(ReflectValue) * 4, _Alignof(ReflectValue));
        for (Int j = 0; j < extra.len; j++)
            e0p[j] = val(TYPE_INT, (Int *)extra.p + j);
        Slice e0 = slice_from(e0p, extra.len, extra.len, TYPE_REFLECT_VALUE);
        /* Convert extra from []int to *SliceValue. */
        ReflectValue e1 = val(it, &extra);

        /* Test Append. */
        ReflectValue a0 = settable(it, &orig);
        ReflectValue have0 = reflect_append(a, a0, e0);
        if (reflect_value_can_addr(have0))
            testing_t_errorf_v(t, "Append #%d: have slice should not be addressable",
                               i);
        if (!reflect_deep_equal(reflect_value_interface(have0), BURROW_ANY(it, &want)))
            testing_t_errorf_v(t, "Append #%d: have %v, want %v", i,
                               value_to_string(a, have0),
                               value_to_string(a, val(it, &want)));
        /* Check that the orig and extra slices were not modified. */
        if (reflect_value_len(a0) != orig.len)
            testing_t_errorf_v(t, "Append #%d: a0.Len: have %d, want %d", i,
                               reflect_value_len(a0), orig_len);
        if (orig.len != orig_len)
            testing_t_errorf_v(t, "Append #%d origLen: have %d, want %d", i, orig.len,
                               orig_len);
        if (extra.len != extra_len)
            testing_t_errorf_v(t, "Append #%d extraLen: have %d, want %d", i, extra.len,
                               extra_len);

        /* Test AppendSlice. */
        ReflectValue a1 = settable(it, &orig);
        ReflectValue have1 = reflect_append_slice(a, a1, e1);
        if (reflect_value_can_addr(have1))
            testing_t_errorf_v(
                t, "AppendSlice #%d: have slice should not be addressable", i);
        if (!reflect_deep_equal(reflect_value_interface(have1), BURROW_ANY(it, &want)))
            testing_t_errorf_v(t, "AppendSlice #%d: have %v, want %v", i,
                               value_to_string(a, have1),
                               value_to_string(a, val(it, &want)));
        if (reflect_value_len(a1) != orig.len)
            testing_t_errorf_v(t, "AppendSlice #%d: a1.Len: have %d, want %d", i,
                               reflect_value_len(a0), orig_len);
        if (orig.len != orig_len)
            testing_t_errorf_v(t, "AppendSlice #%d origLen: have %d, want %d", i,
                               orig.len, orig_len);
        if (extra.len != extra_len)
            testing_t_errorf_v(t, "AppendSlice #%d extraLen: have %d, want %d", i,
                               extra.len, extra_len);

        /* Test Append and AppendSlice with unexported value. */
        WithInts w = {orig};
        Op o = {.a = a, .v = reflect_value_field(val(TYPE_OF(WithInts), &w), 0)};
        o.xs = e0;
        should_panic(t, "using unexported field", op_append, &o);
        o.x = e1;
        should_panic(t, "using unexported field", op_append_slice, &o);
    }
    arena_free(&ar);
}

static void TestCopy(TestingT *t) {
    Int a[] = {1, 2, 3, 4, 10, 9, 8, 7};
    Int b[] = {11, 22, 33, 44, 1010, 99, 88, 77, 66, 55, 44};
    Int c[] = {11, 22, 33, 44, 1010, 99, 88, 77, 66, 55, 44};
    for (Int i = 0; i < NELEM(b); i++)
        if (b[i] != c[i])
            testing_t_fatalf_v(t, "b != c before test");
    Slice a1 = slice_from(a, NELEM(a), NELEM(a), TYPE_INT);
    Slice b1 = slice_from(b, NELEM(b), NELEM(b), TYPE_INT);
    ReflectValue aa = settable(ints_t(), &a1);
    ReflectValue ab = settable(ints_t(), &b1);
    for (Int tocopy = 1; tocopy <= 7; tocopy++) {
        reflect_value_set_len(aa, tocopy);
        reflect_copy(ab, aa);
        reflect_value_set_len(aa, 8);
        for (Int i = 0; i < tocopy; i++)
            if (a[i] != b[i])
                testing_t_errorf_v(t, "(i) tocopy=%d a[%d]=%d, b[%d]=%d", tocopy, i,
                                   a[i], i, b[i]);
        for (Int i = tocopy; i < NELEM(b); i++) {
            if (b[i] != c[i]) {
                if (i < NELEM(a))
                    testing_t_errorf_v(t, "(ii) tocopy=%d a[%d]=%d, b[%d]=%d, c[%d]=%d",
                                       tocopy, i, a[i], i, b[i], i, c[i]);
                else
                    testing_t_errorf_v(t, "(iii) tocopy=%d b[%d]=%d, c[%d]=%d", tocopy,
                                       i, b[i], i, c[i]);
            }
        }
    }
}

static void check_copied(TestingT *t, Int n, Int want_n, const Byte *s,
                         const char *expecting) {
    if (n != want_n || memcmp(s, expecting, 8) != 0)
        testing_t_errorf_v(t, "got n = %d, s = %s, expecting n = %d, s = %s", n,
                           str_from_bytes((const char *)s, 8), want_n, expecting);
}

static void test_copy_string_slice(void *env, TestingT *t) {
    (void)env;
    Byte s[8];
    memset(s, '_', sizeof s);
    Slice sl = slice_from(s, 8, 8, TYPE_UINT8);
    ReflectValue v = val(TYPE_BYTES, &sl);
    Str empty = BURROW_STR_EMPTY, hello = BURROW_S("hello"),
        helloworld = BURROW_S("helloworld");

    Int n = reflect_copy(v, val(TYPE_STRING, &empty));
    check_copied(t, n, 0, s, "________");
    n = reflect_copy(v, val(TYPE_STRING, &hello));
    check_copied(t, n, 5, s, "hello___");
    n = reflect_copy(v, val(TYPE_STRING, &helloworld));
    check_copied(t, n, 8, s, "hellowor");
}

static void test_copy_string_array(void *env, TestingT *t) {
    (void)env;
    Byte s[8];
    memset(s, '_', sizeof s);
    ReflectValue v = settable(reflect_array_of(8, TYPE_UINT8), s);
    Str empty = BURROW_STR_EMPTY, hello = BURROW_S("hello"),
        helloworld = BURROW_S("helloworld");

    Int n = reflect_copy(v, val(TYPE_STRING, &empty));
    check_copied(t, n, 0, s, "________");
    n = reflect_copy(v, val(TYPE_STRING, &hello));
    check_copied(t, n, 5, s, "hello___");
    n = reflect_copy(v, val(TYPE_STRING, &helloworld));
    check_copied(t, n, 8, s, "hellowor");
}

static void TestCopyString(TestingT *t) {
    testing_t_run(t, BURROW_S("Slice"),
                  BURROW_FN(TestingTFunc, test_copy_string_slice, NULL));
    testing_t_run(t, BURROW_S("Array"),
                  BURROW_FN(TestingTFunc, test_copy_string_array, NULL));
}

static void TestCopyArray(TestingT *t) {
    Int a[8] = {1, 2, 3, 4, 10, 9, 8, 7};
    Int b[11] = {11, 22, 33, 44, 1010, 99, 88, 77, 66, 55, 44};
    Int c[11];
    memcpy(c, b, sizeof b);
    ReflectValue aa = settable(reflect_array_of(8, TYPE_INT), a);
    ReflectValue ab = settable(reflect_array_of(11, TYPE_INT), b);
    reflect_copy(ab, aa);
    for (Int i = 0; i < NELEM(a); i++)
        if (a[i] != b[i])
            testing_t_errorf_v(t, "(i) a[%d]=%d, b[%d]=%d", i, a[i], i, b[i]);
    for (Int i = NELEM(a); i < NELEM(b); i++)
        if (b[i] != c[i])
            testing_t_errorf_v(t, "(ii) b[%d]=%d, c[%d]=%d", i, b[i], i, c[i]);
}

/* -------------------------------------------------------------------- maps */

static const Type type_S_str =
    TYPE_DESC(S_("S"), TEST_PKG, KIND_STRING, Str, 0, NULL, NULL, NULL, 0);

static void TestMap(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType mt = reflect_map_of(TYPE_STRING, TYPE_INT);
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Str ka = BURROW_S("a"), kb = BURROW_S("b");
    map_set(m, &ka, &(Int){1});
    map_set(m, &kb, &(Int){2});
    ReflectValue mv = val(mt, &m);
    if (reflect_value_len(mv) != map_len(m))
        testing_t_errorf_v(t, "Len = %d, want %d", reflect_value_len(mv), map_len(m));
    Slice keys = reflect_value_map_keys(mv, a);
    ReflectValue newmap = reflect_make_map(a, mt);
    MapIter mi = map_iter(m);
    const void *kp;
    void *vp;
    while (map_next(&mi, &kp, &vp)) {
        Str k = *(const Str *)kp;
        Int v = *(const Int *)vp;
        /* Check that returned Keys match keys in range. These are not required
         * to be in the same order. */
        bool seen = false;
        const ReflectValue *kv = keys.p;
        for (Int i = 0; i < keys.len; i++)
            if (str_eq(reflect_value_string(kv[i]), k)) {
                seen = true;
                break;
            }
        if (!seen)
            testing_t_errorf_v(t, "Missing key %q", k);

        /* Check that value lookup is correct. */
        ReflectValue vv = reflect_value_map_index(mv, val(TYPE_STRING, &k));
        if (reflect_value_int(vv) != v)
            testing_t_errorf_v(t, "Key %q: have value %d, want %d", k,
                               reflect_value_int(vv), v);

        /* Copy into new map. */
        reflect_value_set_map_index(newmap, val(TYPE_STRING, &k), val(TYPE_INT, &v));
    }
    Str np = BURROW_S("not-present");
    ReflectValue vv = reflect_value_map_index(mv, val(TYPE_STRING, &np));
    if (reflect_value_is_valid(vv))
        testing_t_errorf_v(t, "Invalid key: got non-nil value %s",
                           value_to_string(a, vv));

    Map *newm = *(Map **)reflect_value_interface(newmap).data;
    if (map_len(newm) != map_len(m))
        testing_t_errorf_v(t, "length after copy: newm=%d, m=%d", map_len(newm),
                           map_len(m));
    mi = map_iter(newm);
    while (map_next(&mi, &kp, &vp)) {
        const Int *mvp = map_get(m, kp);
        if (mvp == NULL || *mvp != *(const Int *)vp)
            testing_t_errorf_v(t, "newm[%q] = %d, but m[%q] = %d, %t", *(const Str *)kp,
                               *(const Int *)vp, *(const Str *)kp,
                               mvp == NULL ? 0 : *mvp, mvp != NULL);
    }

    reflect_value_set_map_index(newmap, val(TYPE_STRING, &ka), (ReflectValue){0});
    const Int *got = map_get(newm, &ka);
    if (got != NULL)
        testing_t_errorf_v(t, "newm[\"a\"] = %d after delete", *got);

    mv = settable(mt, &m);
    reflect_value_set(mv, reflect_zero(reflect_value_type(mv)));
    if (m != NULL)
        testing_t_errorf_v(t, "mv.Set(nil) failed");

    Str key = BURROW_S("key");
    Int zero = 0;
    Op o = {.v = mv, .x = val(&type_S_str, &key), .y = val(TYPE_INT, &zero)};
    should_panic(t, "not assignable", op_map_index, &o);
    should_panic(t, "not assignable", op_set_map_index, &o);
    arena_free(&ar);
}

static void TestNilMap(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Map *m = NULL;
    ReflectValue mv = val(reflect_map_of(TYPE_STRING, TYPE_INT), &m);
    Slice keys = reflect_value_map_keys(mv, a);
    if (keys.len != 0)
        testing_t_errorf_v(t, ">0 keys for nil map: %d", keys.len);

    /* Check that value for missing key is zero. */
    Str hello = BURROW_S("hello");
    ReflectValue x = reflect_value_map_index(mv, val(TYPE_STRING, &hello));
    if (reflect_value_kind(x) != KIND_INVALID)
        testing_t_errorf_v(t,
                           "m.MapIndex(\"hello\") for nil map = %s, want Invalid Value",
                           value_to_string(a, x));

    /* Check big value too. */
    Map *mbig = NULL;
    ReflectType bt =
        reflect_map_of(TYPE_STRING, reflect_array_of(10 << 20, TYPE_UINT8));
    x = reflect_value_map_index(val(bt, &mbig), val(TYPE_STRING, &hello));
    if (reflect_value_kind(x) != KIND_INVALID)
        testing_t_errorf_v(
            t, "mbig.MapIndex(\"hello\") for nil map = %s, want Invalid Value",
            value_to_string(a, x));

    /* Test that deletes from a nil map succeed. */
    Str hi = BURROW_S("hi");
    reflect_value_set_map_index(mv, val(TYPE_STRING, &hi), (ReflectValue){0});
    arena_free(&ar);
}

/* ---------------------------------------------------------------- channels */

static Chan *chan_of_value(ReflectValue cv) {
    return *(Chan **)reflect_value_interface(cv).data;
}

static Int chan_recv_int(Chan *c) {
    Int i = 0;
    chan_recv(c, &i);
    return i;
}

static void TestChan(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType ct = reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT);
    for (int loop = 0; loop < 2; loop++) {
        Chan *c = NULL;
        ReflectValue cv;

        /* check both ways to allocate channels */
        if (loop == 1) {
            c = chan_make(a, TYPE_INT, 1);
            cv = val(ct, &c);
        } else {
            cv = reflect_make_chan(a, ct, 1);
            c = chan_of_value(cv);
        }

        /* Send */
        reflect_value_send(cv, val(TYPE_INT, &(Int){2}));
        Int i = chan_recv_int(c);
        if (i != 2)
            testing_t_errorf_v(t, "reflect Send 2, native recv %d", i);

        /* Recv */
        chan_send(c, &(Int){3});
        bool ok;
        ReflectValue rv = reflect_value_recv(cv, a, &ok);
        if (reflect_value_int(rv) != 3 || !ok)
            testing_t_errorf_v(t, "native send 3, reflect Recv %d, %t",
                               reflect_value_int(rv), ok);

        /* TryRecv fail */
        ReflectValue v = reflect_value_try_recv(cv, a, &ok);
        if (reflect_value_is_valid(v) || ok)
            testing_t_errorf_v(t, "TryRecv on empty chan: %s, %t",
                               value_to_string(a, v), ok);

        /* TryRecv success */
        chan_send(c, &(Int){4});
        v = reflect_value_try_recv(cv, a, &ok);
        if (!reflect_value_is_valid(v))
            testing_t_errorf_v(t, "TryRecv on ready chan got nil");
        else if (reflect_value_int(v) != 4 || !ok)
            testing_t_errorf_v(t, "native send 4, TryRecv %d, %t", reflect_value_int(v),
                               ok);

        /* TrySend fail */
        chan_send(c, &(Int){100});
        ok = reflect_value_try_send(cv, val(TYPE_INT, &(Int){5}));
        i = chan_recv_int(c);
        if (ok)
            testing_t_errorf_v(t, "TrySend on full chan succeeded: value %d", i);

        /* TrySend success */
        ok = reflect_value_try_send(cv, val(TYPE_INT, &(Int){6}));
        if (!ok) {
            testing_t_errorf_v(t, "TrySend on empty chan failed");
            Int x;
            bool got;
            if (chan_try_recv(c, &x, &got))
                testing_t_errorf_v(t, "TrySend failed but it did send %d", x);
        } else {
            i = chan_recv_int(c);
            if (i != 6)
                testing_t_errorf_v(t, "TrySend 6, recv %d", i);
        }

        /* Close */
        chan_send(c, &(Int){123});
        reflect_value_close(cv);
        rv = reflect_value_recv(cv, a, &ok);
        if (reflect_value_int(rv) != 123 || !ok)
            testing_t_errorf_v(t, "send 123 then close; Recv %d, %t",
                               reflect_value_int(rv), ok);
        rv = reflect_value_recv(cv, a, &ok);
        if (reflect_value_int(rv) != 0 || ok)
            testing_t_errorf_v(t, "after close Recv %d, %t", reflect_value_int(rv), ok);
        /* Closing a read-only channel */
        Chan *rc = chan_make(a, TYPE_INT, 1);
        Op o = {.v = val(reflect_chan_of(REFLECT_RECV_DIR, TYPE_INT), &rc)};
        should_panic(t, "", op_close, &o);
    }

    /* check creation of unbuffered channel */
    ReflectValue cv = reflect_make_chan(a, ct, 0);
    if (reflect_value_try_send(cv, val(TYPE_INT, &(Int){7})))
        testing_t_errorf_v(t, "TrySend on sync chan succeeded");
    bool ok;
    ReflectValue v = reflect_value_try_recv(cv, a, &ok);
    if (reflect_value_is_valid(v) || ok)
        testing_t_errorf_v(t, "TryRecv on sync chan succeeded: isvalid=%t ok=%t",
                           reflect_value_is_valid(v), ok);

    /* len/cap */
    cv = reflect_make_chan(a, ct, 10);
    Chan *c = chan_of_value(cv);
    for (Int i = 0; i < 3; i++)
        chan_send(c, &i);
    if (reflect_value_len(cv) != chan_len(c) || reflect_value_cap(cv) != chan_cap(c))
        testing_t_errorf_v(t, "Len/Cap = %d/%d want %d/%d", reflect_value_len(cv),
                           reflect_value_cap(cv), chan_len(c), chan_cap(c));
    arena_free(&ar);
}

/* ---------------------------------------------------------- PtrTo and Addr */

static const Type type_ptr_unsafe = TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0,
                                              NULL, TYPE_UNSAFE_POINTER, NULL, 0);
static const Type type_ptr_ptr_unsafe =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, &type_ptr_unsafe, NULL, 0);

static void TestPtrTo(TestingT *t) {
    ReflectType typ = &type_ptr_ptr_unsafe;
    for (int i = 0; i < 100; i++)
        typ = reflect_pointer_to(typ);
    for (int i = 0; i < 100; i++)
        typ = reflect_type_elem(typ);
    if (typ != &type_ptr_ptr_unsafe)
        testing_t_errorf_v(t, "after 100 PointerTo and Elem, have %s, want %s",
                           reflect_type_string(typ),
                           reflect_type_string(&type_ptr_ptr_unsafe));
}

#define XY_FIELDS(F, T)                                                                \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(XY, XY_FIELDS);
BURROW_PTR_TYPE(BoolPtr, bool);
#define SB_FIELDS(F, T) F(T, BoolPtr, B, "")
BURROW_STRUCT(SB, SB_FIELDS);

static void TestAddr(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    XY p = {0, 0};

    ReflectType pt = reflect_pointer_to(TYPE_OF(XY));
    XY *pp = &p;
    ReflectValue v = val(pt, &pp);
    v = reflect_value_elem(v);
    v = reflect_value_addr(v, a);
    v = reflect_value_elem(v);
    v = reflect_value_field(v, 0);
    reflect_value_set_int(v, 2);
    if (p.X != 2)
        testing_t_errorf_v(t, "Addr.Elem.Set failed to set value");

    /* Again but take address of the ValueOf value. Exercises generation of
     * pointer types nobody declared. */
    XY *q = &p;
    v = settable(pt, &q);
    v = reflect_value_addr(v, a);
    v = reflect_value_elem(v);
    v = reflect_value_elem(v);
    v = reflect_value_addr(v, a);
    v = reflect_value_elem(v);
    v = reflect_value_field(v, 0);
    reflect_value_set_int(v, 3);
    if (p.X != 3)
        testing_t_errorf_v(t, "Addr.Elem.Set failed to set value");

    /* Starting without pointer we should get changed value in interface. */
    XY qq = p;
    v = settable(TYPE_OF(XY), &qq);
    ReflectValue v0 = v;
    v = reflect_value_addr(v, a);
    v = reflect_value_elem(v);
    v = reflect_value_field(v, 0);
    reflect_value_set_int(v, 4);
    if (p.X != 3) /* should be unchanged from last time */
        testing_t_errorf_v(t, "somehow value Set changed original p");
    p = *(XY *)reflect_value_interface(v0).data;
    if (p.X != 4)
        testing_t_errorf_v(t, "Addr.Elem.Set valued to set value in top value");

    /* Verify that taking the address of a type gives us a pointer which we
     * can convert back using the usual interface notation. */
    SB s = {NULL};
    Any ps = reflect_value_interface(
        reflect_value_addr(reflect_value_field(settable(TYPE_OF(SB), &s), 0), a));
    bool nb = false;
    if (ps.t != reflect_pointer_to(TYPE_OF(BoolPtr)))
        testing_t_errorf_v(t, "Addr.Interface has type %s", reflect_type_string(ps.t));
    **(bool ***)ps.data = &nb;
    if (s.B == NULL)
        testing_t_errorf_v(t, "Addr.Interface direct assignment failed");
    arena_free(&ar);
}

/* ------------------------------------------------------------------ Slice */

static void TestSlice(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType it = ints_t();
    Int xsv[] = {1, 2, 3, 4, 5, 6, 7, 8};
    Slice xs = slice_from(xsv, 8, 8, TYPE_INT);
    Slice v =
        *(Slice *)reflect_value_interface(reflect_value_slice(val(it, &xs), a, 3, 5))
             .data;
    if (v.len != 2)
        testing_t_errorf_v(t, "len(xs.Slice(3, 5)) = %d", v.len);
    if (v.cap != 5)
        testing_t_errorf_v(t, "cap(xs.Slice(3, 5)) = %d", v.cap);
    Slice v5 = slice_from(v.p, 5, 5, TYPE_INT),
          xs3 = slice_from(xsv + 3, 5, 5, TYPE_INT);
    if (!reflect_deep_equal(BURROW_ANY(it, &v5), BURROW_ANY(it, &xs3)))
        testing_t_errorf_v(t, "xs.Slice(3, 5)[0:5] = %s",
                           value_to_string(a, val(it, &v5)));

    Int xa[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    ReflectValue xav = settable(reflect_array_of(8, TYPE_INT), xa);
    v = *(Slice *)reflect_value_interface(reflect_value_slice(xav, a, 2, 5)).data;
    if (v.len != 3)
        testing_t_errorf_v(t, "len(xa.Slice(2, 5)) = %d", v.len);
    if (v.cap != 6)
        testing_t_errorf_v(t, "cap(xa.Slice(2, 5)) = %d", v.cap);
    Slice v6 = slice_from(v.p, 6, 6, TYPE_INT),
          xa2 = slice_from(xa + 2, 6, 6, TYPE_INT);
    if (!reflect_deep_equal(BURROW_ANY(it, &v6), BURROW_ANY(it, &xa2)))
        testing_t_errorf_v(t, "xs.Slice(2, 5)[0:6] = %s",
                           value_to_string(a, val(it, &v6)));

    Str s = BURROW_S("0123456789");
    Str vs = *(Str *)reflect_value_interface(
                  reflect_value_slice(val(TYPE_STRING, &s), a, 3, 5))
                  .data;
    if (!str_eq(vs, str_from_bytes(s.p + 3, 2)))
        testing_t_errorf_v(t, "s.Slice(3, 5) = %q; expected %q", vs,
                           str_from_bytes(s.p + 3, 2));

    ReflectValue rv = settable(it, &xs);
    rv = reflect_value_slice(rv, a, 3, 4);
    void *ptr2 = reflect_value_unsafe_pointer(rv);
    rv = reflect_value_slice(rv, a, 5, 5);
    void *ptr3 = reflect_value_unsafe_pointer(rv);
    if (ptr3 != ptr2)
        testing_t_errorf_v(t, "xs.Slice(3,4).Slice3(5,5).UnsafePointer() = %p, want %p",
                           ptr3, ptr2);
    arena_free(&ar);
}

static void slice3_panics(TestingT *t, Op *o) {
    static const Int bad[][3] = {{1, 2, 1}, {1, 1, 11}, {2, 2, 1}};
    for (Int i = 0; i < NELEM(bad); i++) {
        o->i = bad[i][0];
        o->j = bad[i][1];
        o->k = bad[i][2];
        should_panic(t, "Slice3", op_slice3, o);
    }
}

static void TestSlice3(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType it = ints_t();
    Int xsv[] = {1, 2, 3, 4, 5, 6, 7, 8};
    Slice xs = slice_from(xsv, 8, 8, TYPE_INT);
    Slice v = *(Slice *)reflect_value_interface(
                   reflect_value_slice3(val(it, &xs), a, 3, 5, 7))
                   .data;
    if (v.len != 2)
        testing_t_errorf_v(t, "len(xs.Slice3(3, 5, 7)) = %d", v.len);
    if (v.cap != 4)
        testing_t_errorf_v(t, "cap(xs.Slice3(3, 5, 7)) = %d", v.cap);
    Slice v4 = slice_from(v.p, 4, 4, TYPE_INT),
          xs3 = slice_from(xsv + 3, 4, 4, TYPE_INT);
    if (!reflect_deep_equal(BURROW_ANY(it, &v4), BURROW_ANY(it, &xs3)))
        testing_t_errorf_v(t, "xs.Slice3(3, 5, 7)[0:4] = %s",
                           value_to_string(a, val(it, &v4)));
    Op o = {.a = a, .v = settable(it, &xs)};
    slice3_panics(t, &o);

    Int xa[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    ReflectType at = reflect_array_of(8, TYPE_INT);
    v = *(Slice *)reflect_value_interface(
             reflect_value_slice3(settable(at, xa), a, 2, 5, 6))
             .data;
    if (v.len != 3)
        testing_t_errorf_v(t, "len(xa.Slice(2, 5, 6)) = %d", v.len);
    if (v.cap != 4)
        testing_t_errorf_v(t, "cap(xa.Slice(2, 5, 6)) = %d", v.cap);
    Slice xa2 = slice_from(xa + 2, 4, 4, TYPE_INT);
    v4 = slice_from(v.p, 4, 4, TYPE_INT);
    if (!reflect_deep_equal(BURROW_ANY(it, &v4), BURROW_ANY(it, &xa2)))
        testing_t_errorf_v(t, "xs.Slice(2, 5, 6)[0:4] = %s",
                           value_to_string(a, val(it, &v4)));
    o.v = settable(at, xa);
    slice3_panics(t, &o);

    Str s = BURROW_S("hello world");
    o.v = settable(TYPE_STRING, &s);
    o.i = 1;
    o.j = 2;
    o.k = 3;
    should_panic(t, "Slice3", op_slice3, &o);

    ReflectValue rv = settable(it, &xs);
    rv = reflect_value_slice3(rv, a, 3, 5, 7);
    void *ptr2 = reflect_value_unsafe_pointer(rv);
    rv = reflect_value_slice3(rv, a, 4, 4, 4);
    void *ptr3 = reflect_value_unsafe_pointer(rv);
    if (ptr3 != ptr2)
        testing_t_errorf_v(
            t, "xs.Slice3(3,5,7).Slice3(4,4,4).UnsafePointer() = %p, want %p", ptr3,
            ptr2);
    arena_free(&ar);
}

static void TestSetLenCap(TestingT *t) {
    Int xsv[] = {1, 2, 3, 4, 5, 6, 7, 8};
    Slice xs = slice_from(xsv, 8, 8, TYPE_INT);
    Int xa[8] = {10, 20, 30, 40, 50, 60, 70, 80};

    Op o = {.v = settable(ints_t(), &xs)};
    o.i = 10;
    should_panic(t, "SetLen", op_set_len, &o);
    should_panic(t, "SetCap", op_set_cap, &o);
    o.i = -1;
    should_panic(t, "SetLen", op_set_len, &o);
    should_panic(t, "SetCap", op_set_cap, &o);
    o.i = 6; /* smaller than len */
    should_panic(t, "SetCap", op_set_cap, &o);
    reflect_value_set_len(o.v, 5);
    if (xs.len != 5 || xs.cap != 8)
        testing_t_errorf_v(t, "after SetLen(5), len, cap = %d, %d, want 5, 8", xs.len,
                           xs.cap);
    reflect_value_set_cap(o.v, 6);
    if (xs.len != 5 || xs.cap != 6)
        testing_t_errorf_v(t, "after SetCap(6), len, cap = %d, %d, want 5, 6", xs.len,
                           xs.cap);
    reflect_value_set_cap(o.v, 5);
    if (xs.len != 5 || xs.cap != 5)
        testing_t_errorf_v(t, "after SetCap(5), len, cap = %d, %d, want 5, 5", xs.len,
                           xs.cap);
    o.i = 4; /* smaller than len */
    should_panic(t, "SetCap", op_set_cap, &o);
    o.i = 6; /* bigger than cap */
    should_panic(t, "SetLen", op_set_len, &o);

    o.v = settable(reflect_array_of(8, TYPE_INT), xa);
    o.i = 8;
    should_panic(t, "SetLen", op_set_len, &o);
    should_panic(t, "SetCap", op_set_cap, &o);
}

static const Type type_B =
    TYPE_DESC(S_("B"), TEST_PKG, KIND_SLICE, Slice, 0, NULL, TYPE_UINT8, NULL, 0);

static void TestSetBytes(TestingT *t) {
    Slice x = slice_nil(TYPE_UINT8);
    Byte yv[] = {1, 2, 3, 4};
    Slice y = slice_from(yv, 4, 4, TYPE_UINT8);
    reflect_value_set_bytes(settable(&type_B, &x), y);
    if (x.len != y.len || memcmp(x.p, y.p, 4) != 0)
        testing_t_fatalf_v(t, "ValueOf(%v).Bytes() = %v", x.len, y.len);
    if (x.p != y.p)
        testing_t_errorf_v(t, "ValueOf(%p).Bytes() = %p", x.p, y.p);
}

/* TestSetPanic's types. A field whose name is its type's name is embedded. */
#define T0_FIELDS(F, T) F(T, Int, W, "")
BURROW_STRUCT(t0, T0_FIELDS);
#define T1_FIELDS(F, T)                                                                \
    F(T, Int, Y, "")                                                                   \
    F(T, t0, t0, "")
BURROW_STRUCT(t1, T1_FIELDS);
#define T2_FIELDS(F, T)                                                                \
    F(T, Int, Z, "")                                                                   \
    F(T, t0, namedT0, "")
BURROW_STRUCT(T2, T2_FIELDS);
#define SETPANICT_FIELDS(F, T)                                                         \
    F(T, Int, X, "")                                                                   \
    F(T, t1, t1, "")                                                                   \
    F(T, T2, T2, "")                                                                   \
    F(T, t1, NamedT1, "")                                                              \
    F(T, T2, NamedT2, "")                                                              \
    F(T, t1, namedT1, "")                                                              \
    F(T, T2, namedT2, "")
BURROW_STRUCT(SetPanicT, SETPANICT_FIELDS);

/* The field paths TestSetPanic sets, and whether setting each is fine once
 * the struct is addressable. Unaddressable, none of them is. */
static const struct {
    const char *path;
    bool ok;
} set_panic_tests[] = {
    {"0", true},    {"1", false},   {"10", true},  {"11", false},  {"110", true},
    {"2", true},    {"20", true},   {"21", false}, {"210", false}, {"3", true},
    {"30", true},   {"31", false},  {"310", true}, {"4", true},    {"40", true},
    {"41", false},  {"410", false}, {"5", false},  {"50", false},  {"51", false},
    {"510", false}, {"6", false},   {"60", false}, {"61", false},  {"610", false},
};

static ReflectValue field_path(ReflectValue v, const char *path) {
    for (const char *p = path; *p; p++)
        v = reflect_value_field(v, *p - '0');
    return v;
}

static void TestSetPanic(TestingT *t) {
    SetPanicT x;
    memset(&x, 0, sizeof x);
    char buf[256];

    /* not addressable */
    ReflectValue v = val(TYPE_OF(SetPanicT), &x);
    for (Int i = 0; i < NELEM(set_panic_tests); i++) {
        Op o = {.v = field_path(v, set_panic_tests[i].path)};
        should_panic(t, "Set", op_set_zero, &o);
    }

    /* addressable */
    v = settable(TYPE_OF(SetPanicT), &x);
    for (Int i = 0; i < NELEM(set_panic_tests); i++) {
        Op o = {.v = field_path(v, set_panic_tests[i].path)};
        if (set_panic_tests[i].ok) {
            if (panics(op_set_zero, &o, buf, sizeof buf))
                testing_t_errorf_v(t, "path %s: unexpected panic: %s",
                                   set_panic_tests[i].path, buf);
        } else {
            should_panic(t, "Set", op_set_zero, &o);
        }
    }
}

/* ------------------------------------------------------ type constructors */

static const Type type_Tint =
    TYPE_DESC(S_("Tint"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);
static const Type type_Tfloat =
    TYPE_DESC(S_("Tfloat"), TEST_PKG, KIND_FLOAT64, double, 0, NULL, NULL, NULL, 0);
static const Type type_Tstring =
    TYPE_DESC(S_("Tstring"), TEST_PKG, KIND_STRING, Str, 0, NULL, NULL, NULL, 0);
static const Type type_Tints =
    TYPE_DESC(NONAME, NONAME, KIND_SLICE, Slice, 0, NULL, &type_Tint, NULL, 0);
static const Type type_Tint1 =
    TYPE_DESC(NONAME, NONAME, KIND_ARRAY, Int, 0, NULL, &type_Tint, NULL, 1);

BURROW_ARRAY_TYPE(Int1, Int, 1);
#define TSTRUCT_FIELDS(F, T) F(T, Int, V, "")
BURROW_STRUCT(Tstruct, TSTRUCT_FIELDS);
#define TSTRUCTA_FIELDS(F, T) F(T, Int1, V, "")
BURROW_STRUCT(TstructA, TSTRUCTA_FIELDS);
#define TSTRUCTS_FIELDS(F, T) F(T, Ints, V, "")
BURROW_STRUCT(TstructS, TSTRUCTS_FIELDS);
#define TSTRUCTUV_FIELDS(F, T)                                                         \
    F(T, Int, U, "")                                                                   \
    F(T, Int, V, "")
BURROW_STRUCT(TstructUV, TSTRUCTUV_FIELDS);
#define TSTRUCTUVF_FIELDS(F, T)                                                        \
    F(T, Int, U, "")                                                                   \
    F(T, double, V, "")
BURROW_STRUCT(TstructUVF, TSTRUCTUVF_FIELDS);

static void av_int(Alloc *a, Int i, void *out) {
    (void)a;
    *(Int *)out = i;
}

static void av_float(Alloc *a, Int i, void *out) {
    (void)a;
    *(double *)out = (double)i;
}

static void av_string(Alloc *a, Int i, void *out) {
    *(Str *)out = strconv_itoa(a, i);
}

static void av_struct(Alloc *a, Int i, void *out) {
    (void)a;
    *(Tstruct *)out = (Tstruct){i};
}

static void av_ints(Alloc *a, Int i, void *out) {
    Slice s = slice_make(a, &type_Tint, 1, 1);
    *(Int *)s.p = i;
    *(Slice *)out = s;
}

static void av_struct_array(Alloc *a, Int i, void *out) {
    (void)a;
    *(TstructA *)out = (TstructA){{{i}}};
}

static void av_struct_slice(Alloc *a, Int i, void *out) {
    *(TstructS *)out = (TstructS){ints(a, &i, 1)};
}

static void av_uv(Alloc *a, Int i, void *out) {
    (void)a;
    *(TstructUV *)out = (TstructUV){i, i};
}

static void av_uvf(Alloc *a, Int i, void *out) {
    (void)a;
    *(TstructUVF *)out = (TstructUVF){i, (double)i};
}

typedef struct ArrayOfTest {
    Int n;
    const Type *t;
    void (*value)(Alloc *a, Int i, void *out);
    bool comparable;
    const char *want;
} ArrayOfTest;

static void TestArrayOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    const ArrayOfTest tests[] = {
        {0, &type_Tint, av_int, true, "[]"},
        {10, &type_Tint, av_int, true, "[0 1 2 3 4 5 6 7 8 9]"},
        {10, &type_Tfloat, av_float, true, "[0 1 2 3 4 5 6 7 8 9]"},
        {10, &type_Tstring, av_string, true, "[0 1 2 3 4 5 6 7 8 9]"},
        {10, TYPE_OF(Tstruct), av_struct, true,
         "[{0} {1} {2} {3} {4} {5} {6} {7} {8} {9}]"},
        {10, &type_Tints, av_ints, false, "[[0] [1] [2] [3] [4] [5] [6] [7] [8] [9]]"},
        {10, &type_Tint1, av_int, true, "[[0] [1] [2] [3] [4] [5] [6] [7] [8] [9]]"},
        {10, TYPE_OF(TstructA), av_struct_array, true,
         "[{[0]} {[1]} {[2]} {[3]} {[4]} {[5]} {[6]} {[7]} {[8]} {[9]}]"},
        {10, TYPE_OF(TstructS), av_struct_slice, false,
         "[{[0]} {[1]} {[2]} {[3]} {[4]} {[5]} {[6]} {[7]} {[8]} {[9]}]"},
        {10, TYPE_OF(TstructUV), av_uv, true,
         "[{0 0} {1 1} {2 2} {3 3} {4 4} {5 5} {6 6} {7 7} {8 8} {9 9}]"},
        {10, TYPE_OF(TstructUVF), av_uvf, true,
         "[{0 0} {1 1} {2 2} {3 3} {4 4} {5 5} {6 6} {7 7} {8 8} {9 9}]"},
    };

    for (Int ti = 0; ti < NELEM(tests); ti++) {
        const ArrayOfTest *table = &tests[ti];
        ReflectType at = reflect_array_of(table->n, table->t);
        ReflectValue v = reflect_value_elem(reflect_new(a, at));
        ReflectValue vok = reflect_value_elem(reflect_new(a, at));
        ReflectValue vnot = reflect_value_elem(reflect_new(a, at));
        _Alignas(max_align_t) Byte buf[64];
        for (Int i = 0; i < reflect_value_len(v); i++) {
            table->value(a, i, buf);
            reflect_value_set(reflect_value_index(v, i), val(table->t, buf));
            table->value(a, i, buf);
            reflect_value_set(reflect_value_index(vok, i), val(table->t, buf));
            Int j = i;
            if (i + 1 == reflect_value_len(v))
                j = i + 1;
            /* make it differ only by last element */
            table->value(a, j, buf);
            reflect_value_set(reflect_value_index(vnot, i), val(table->t, buf));
        }
        Str s = sprint(a, reflect_value_interface(v));
        if (!str_eq(s, cs(table->want)))
            testing_t_errorf_v(t, "constructed array = %s, want %s", s, table->want);

        if (table->comparable != reflect_type_comparable(at))
            testing_t_errorf_v(t, "constructed array (%s) is comparable=%t, want=%t", s,
                               reflect_type_comparable(at), table->comparable);
        if (table->comparable) {
            if (table->n > 0 && reflect_deep_equal(reflect_value_interface(vnot),
                                                   reflect_value_interface(v)))
                testing_t_errorf_v(t, "arrays (%s) compare ok (but should not)", s);
            if (!reflect_deep_equal(reflect_value_interface(vok),
                                    reflect_value_interface(v)))
                testing_t_errorf_v(t, "arrays (%s) compare NOT-ok (but should)", s);
        }
    }

    /* check that type already in binary is found */
    static const Type type_T =
        TYPE_DESC(S_("T"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);
    static const Type type_T5 =
        TYPE_DESC(NONAME, NONAME, KIND_ARRAY, Int[5], 0, NULL, &type_T, NULL, 5);
    check_same_type(t, reflect_array_of(5, &type_T), &type_T5);
    arena_free(&ar);
}

static void TestArrayOfPanicOnNegativeLength(TestingT *t) {
    Op o = {.i = -1, .t = TYPE_UINT8};
    should_panic(t, "reflect: negative length passed to ArrayOf", op_array_of, &o);
}

static void TestSliceOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    /* check construction and use of type not in binary */
    static const Type type_T =
        TYPE_DESC(S_("T"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);
    ReflectType st = reflect_slice_of(&type_T);
    Str got = reflect_type_string(st);
    if (!str_eq(got, BURROW_S("[]reflect_test.T")))
        testing_t_errorf_v(t, "SliceOf(T(1)).String()=%q, want %q", got,
                           "[]reflect_test.T");
    ReflectValue v = reflect_make_slice(a, st, 10, 10);
    for (Int i = 0; i < reflect_value_len(v); i++)
        reflect_value_set(reflect_value_index(v, i), val(&type_T, &i));
    Str s = sprint(a, reflect_value_interface(v));
    const char *want = "[0 1 2 3 4 5 6 7 8 9]";
    if (!str_eq(s, cs(want)))
        testing_t_errorf_v(t, "constructed slice = %s, want %s", s, want);

    /* check that type already in binary is found */
    static const Type type_T1 =
        TYPE_DESC(S_("T1"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);
    static const Type type_T1s =
        TYPE_DESC(NONAME, NONAME, KIND_SLICE, Slice, 0, NULL, &type_T1, NULL, 0);
    check_same_type(t, reflect_slice_of(&type_T1), &type_T1s);
    arena_free(&ar);
}

static void TestSliceOverflow(TestingT *t) {
    /* check that MakeSlice panics when size of slice overflows uint */
    const uint64_t S = 1000000;
    uint64_t l = UINT64_MAX / S + 1;
    if (l * S >= S)
        testing_t_fatalf_v(t, "slice size does not overflow");
    ReflectType st = reflect_slice_of(reflect_array_of((Int)S, TYPE_UINT8));
    Op o = {.a = heap_allocator(), .t = st, .i = (Int)l, .j = (Int)l};
    char buf[256];
    if (!panics(op_make_slice, &o, buf, sizeof buf))
        testing_t_fatalf_v(t, "slice overflow does not panic");
}

static const Type type_Tstr =
    TYPE_DESC(S_("T"), TEST_PKG, KIND_STRING, Str, 0, NULL, NULL, NULL, 0);
static const Type type_T1int =
    TYPE_DESC(S_("T1"), TEST_PKG, KIND_INT, Int, 0, NULL, NULL, NULL, 0);

static void TestChanOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    /* check construction and use of type not in binary */
    ReflectType ct = reflect_chan_of(REFLECT_BOTH_DIR, &type_Tstr);
    ReflectValue v = reflect_make_chan(a, ct, 2);
    Str hello = BURROW_S("hello"), world = BURROW_S("world");
    reflect_value_send(v, val(&type_Tstr, &hello));
    reflect_value_send(v, val(&type_Tstr, &world));

    bool ok;
    ReflectValue sv1 = reflect_value_recv(v, a, &ok);
    ReflectValue sv2 = reflect_value_recv(v, a, &ok);
    Str s1 = reflect_value_string(sv1);
    Str s2 = reflect_value_string(sv2);
    if (!str_eq(s1, hello) || !str_eq(s2, world))
        testing_t_errorf_v(t, "constructed chan: have %q, %q, want %q, %q", s1, s2,
                           hello, world);

    /* check that type already in binary is found */
    static const Type type_chan_T1 =
        TYPE_DESC(NONAME, NONAME, KIND_CHAN, void *, 0, NULL, &type_T1int, NULL, 0);
    check_same_type(t, reflect_chan_of(REFLECT_BOTH_DIR, &type_T1int), &type_chan_T1);

    /* Check arrow token association in undefined chan types. */
    ReflectType left = reflect_chan_of(REFLECT_SEND_DIR,
                                       reflect_chan_of(REFLECT_BOTH_DIR, &type_Tstr));
    ReflectType right = reflect_chan_of(REFLECT_BOTH_DIR,
                                        reflect_chan_of(REFLECT_RECV_DIR, &type_Tstr));
    if (!str_eq(reflect_type_string(left), BURROW_S("chan<- chan reflect_test.T")))
        testing_t_errorf_v(t, "chan<-chan: have %s, want %s", reflect_type_string(left),
                           "chan<- chan reflect_test.T");
    if (!str_eq(reflect_type_string(right), BURROW_S("chan (<-chan reflect_test.T)")))
        testing_t_errorf_v(t, "chan<-chan: have %s, want %s",
                           reflect_type_string(right), "chan (<-chan reflect_test.T)");
    arena_free(&ar);
}

static void TestChanOfDir(TestingT *t) {
    /* check construction and use of type not in binary */
    ReflectType crt = reflect_chan_of(REFLECT_RECV_DIR, &type_Tstr);
    ReflectType cst = reflect_chan_of(REFLECT_SEND_DIR, &type_Tstr);

    /* check that type already in binary is found */
    static const Type type_recv_T1 =
        TYPE_DESC(NONAME, NONAME, KIND_CHAN, void *, 0, NULL, &type_T1int, NULL,
                  REFLECT_RECV_DIR);
    static const Type type_send_T1 =
        TYPE_DESC(NONAME, NONAME, KIND_CHAN, void *, 0, NULL, &type_T1int, NULL,
                  REFLECT_SEND_DIR);
    check_same_type(t, reflect_chan_of(REFLECT_RECV_DIR, &type_T1int), &type_recv_T1);
    check_same_type(t, reflect_chan_of(REFLECT_SEND_DIR, &type_T1int), &type_send_T1);

    /* check String form of ChanDir */
    Str d = reflect_chan_dir_string(reflect_type_chan_dir(crt));
    if (!str_eq(d, BURROW_S("<-chan")))
        testing_t_errorf_v(t, "chan dir: have %q, want %q", d, "<-chan");
    d = reflect_chan_dir_string(reflect_type_chan_dir(cst));
    if (!str_eq(d, BURROW_S("chan<-")))
        testing_t_errorf_v(t, "chan dir: have %q, want %q", d, "chan<-");
}

static const Type type_func =
    TYPE_DESC(NONAME, NONAME, KIND_FUNC, Func, 0, NULL, NULL, NULL, 0);

static void TestMapOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    /* check construction and use of type not in binary */
    static const Type type_K =
        TYPE_DESC(S_("K"), TEST_PKG, KIND_STRING, Str, 0, NULL, NULL, NULL, 0);
    static const Type type_V =
        TYPE_DESC(S_("V"), TEST_PKG, KIND_FLOAT64, double, 0, NULL, NULL, NULL, 0);

    ReflectValue v = reflect_make_map(a, reflect_map_of(&type_K, &type_V));
    Str k = BURROW_S("a");
    double one = 1;
    reflect_value_set_map_index(v, val(&type_K, &k), val(&type_V, &one));

    Str s = sprint(a, reflect_value_interface(v));
    const char *want = "map[a:1]";
    if (!str_eq(s, cs(want)))
        testing_t_errorf_v(t, "constructed map = %s, want %s", s, want);

    /* check that type already in binary is found */
    static const Type type_VK =
        TYPE_DESC(NONAME, NONAME, KIND_MAP, void *, 0, NULL, &type_K, &type_V, 0);
    check_same_type(t, reflect_map_of(&type_V, &type_K), &type_VK);

    /* check that invalid key type panics */
    Op o = {.t = &type_func, .u = TYPE_BOOL};
    should_panic(t, "invalid key type", op_map_of, &o);
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Zero */

static void TestBigZero(TestingT *t) {
    enum { size = 1 << 10 };
    ReflectValue z = reflect_zero(reflect_array_of(size, TYPE_UINT8));
    const Byte *p = reflect_value_interface(z).data;
    for (Int i = 0; i < size; i++)
        if (p[i] != 0)
            testing_t_fatalf_v(t, "Zero object not all zero, index %d", i);
}

BURROW_ARRAY_TYPE(ZT, uint8_t, 16);
#define ZS_FIELDS(F, Ty)                                                               \
    F(Ty, uint64_t, a, "")                                                             \
    F(Ty, ZT, T, "")                                                                   \
    F(Ty, uint64_t, b, "")
BURROW_STRUCT(ZS, ZS_FIELDS);

static void TestZeroSet(TestingT *t) {
    ZS v = {0xaaaaaaaaaaaaaaaaULL,
            {{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}},
            0xbbbbbbbbbbbbbbbbULL};
    reflect_value_set(reflect_value_field(settable(TYPE_OF(ZS), &v), 1),
                      reflect_zero(TYPE_OF(ZT)));
    static const ZT zero;
    if (v.a != 0xaaaaaaaaaaaaaaaaULL || memcmp(v.T.v, zero.v, sizeof zero.v) != 0 ||
        v.b != 0xbbbbbbbbbbbbbbbbULL)
        testing_t_fatalf_v(t, "Setting a field to a Zero value didn't work");
}

/* -------------------------------------------------------- SetIter and Clear */

BURROW_MAP_TYPE(StrIntMap, Str, Int);
#define WITHMAP_FIELDS(F, T) F(T, StrIntMap, m, "")
BURROW_STRUCT(WithMap, WITHMAP_FIELDS);

static void TestSetIter(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType mt = reflect_map_of(TYPE_STRING, TYPE_INT);
    Map *data = map_make(a, TYPE_STRING, TYPE_INT, 0);
    static const char *const names[] = {"foo", "bar", "baz"};
    for (Int j = 0; j < 3; j++) {
        Str k = cs(names[j]);
        Int n = j + 1;
        map_set(data, &k, &n);
    }

    ReflectValue m = val(mt, &data);
    ReflectMapIter *i = reflect_value_map_range(m, a);
    ReflectValue k = reflect_value_elem(reflect_new(a, TYPE_STRING));
    ReflectValue v = reflect_value_elem(reflect_new(a, TYPE_INT));
    Op ok = {.v = k, .it = i}, ov = {.v = v, .it = i};
    should_panic(t, "Value.SetIterKey called before Next", op_set_iter_key, &ok);
    should_panic(t, "Value.SetIterValue called before Next", op_set_iter_value, &ov);
    Map *data2 = map_make(a, TYPE_STRING, TYPE_INT, 0);
    while (reflect_map_iter_next(i)) {
        reflect_value_set_iter_key(k, i);
        reflect_value_set_iter_value(v, i);
        map_set(data2, reflect_value_interface(k).data,
                reflect_value_interface(v).data);
    }
    if (!reflect_deep_equal(BURROW_ANY(mt, &data), BURROW_ANY(mt, &data2)))
        testing_t_errorf_v(t, "maps not equal, got %d entries want %d", map_len(data2),
                           map_len(data));
    should_panic(t, "Value.SetIterKey called on exhausted iterator", op_set_iter_key,
                 &ok);
    should_panic(t, "Value.SetIterValue called on exhausted iterator",
                 op_set_iter_value, &ov);

    reflect_map_iter_reset(i, m);
    reflect_map_iter_next(i);
    Str empty = BURROW_STR_EMPTY;
    Int zero = 0;
    Op o = {.v = val(TYPE_STRING, &empty), .it = i};
    should_panic(t, "Value.SetIterKey using unaddressable value", op_set_iter_key, &o);
    o.v = val(TYPE_INT, &zero);
    should_panic(t, "Value.SetIterValue using unaddressable value", op_set_iter_value,
                 &o);
    o.v = reflect_value_elem(reflect_new(a, TYPE_INT));
    should_panic(t, "value of type string is not assignable to type int",
                 op_set_iter_key, &o);
    o.v = reflect_value_elem(reflect_new(a, TYPE_STRING));
    should_panic(t, "value of type int is not assignable to type string",
                 op_set_iter_value, &o);

    /* Make sure assignment conversion works. */
    Any x = {NULL, NULL};
    ReflectValue y = settable(TYPE_ANY, &x);
    reflect_value_set_iter_key(y, i);
    if (x.t != TYPE_STRING || map_get(data, x.data) == NULL)
        testing_t_errorf_v(t, "got key %v which is not in map", x);
    reflect_value_set_iter_value(y, i);
    if (x.t != TYPE_INT || *(const Int *)x.data < 1 || *(const Int *)x.data > 3)
        testing_t_errorf_v(t, "got value %v which is not in map", x);

    /* Try some key/value types which are direct interfaces. */
    Int av = 88, bv = 99;
    Int *ap = &av, *bp = &bv;
    ReflectType pt = reflect_pointer_to(TYPE_INT);
    Map *pp = map_make(a, pt, pt, 0);
    map_set(pp, &ap, &bp);
    i = reflect_value_map_range(val(reflect_map_of(pt, pt), &pp), a);
    reflect_map_iter_next(i);
    reflect_value_set_iter_key(y, i);
    Any got = reflect_value_interface(y);
    if (got.t != pt || **(Int **)got.data != av)
        testing_t_errorf_v(t, "pointer incorrect: got %d want %d",
                           got.t == pt ? **(Int **)got.data : 0, av);
    reflect_value_set_iter_value(y, i);
    got = reflect_value_interface(y);
    if (got.t != pt || **(Int **)got.data != bv)
        testing_t_errorf_v(t, "pointer incorrect: got %d want %d",
                           got.t == pt ? **(Int **)got.data : 0, bv);

    /* Make sure we panic assigning from an unexported field. */
    WithMap w = {data};
    m = reflect_value_field(val(TYPE_OF(WithMap), &w), 0);
    ReflectMapIter *iter = reflect_value_map_range(m, a);
    while (reflect_map_iter_next(iter)) {
        Op ok2 = {.v = k, .it = iter}, ov2 = {.v = v, .it = iter};
        should_panic(t, "using value obtained using unexported field", op_set_iter_key,
                     &ok2);
        should_panic(t, "using value obtained using unexported field",
                     op_set_iter_value, &ov2);
    }
    arena_free(&ar);
}

static bool clear_map(ReflectValue v) {
    reflect_value_clear(v);
    return reflect_value_len(v) == 0;
}

static bool clear_slice(ReflectValue v) {
    reflect_value_clear(v);
    for (Int i = 0; i < reflect_value_len(v); i++)
        if (!reflect_value_is_zero(reflect_value_index(v, i)))
            return false;
    return true;
}

typedef struct ClearTest {
    ReflectValue value;
    bool (*test)(ReflectValue v);
} ClearTest;

static void test_clear(void *env, TestingT *t) {
    ClearTest *tc = env;
    if (!tc->test(tc->value))
        testing_t_errorf_v(t, "unexpected result for value.Clear()");
}

static TestingT *clear_t;

static bool clear_panics(ReflectValue v) {
    Op o = {.v = v};
    should_panic(clear_t, "reflect.Value.Clear", op_clear, &o);
    return true;
}

static void test_clear_panics(void *env, TestingT *t) {
    clear_t = t;
    test_clear(env, t);
}

static void TestClear(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType mt;
    Map *m = value_tests_map(a, &mt);

    ValueTest vt[MAX_VALUE_TESTS];
    Int n = value_tests(vt);
    ReflectType pt = reflect_pointer_to(TYPE_INT);
    Slice s = slice_make(a, pt, n, n);
    Int *targets = mem_alloc(a, (size_t)n * sizeof(Int), _Alignof(Int));
    for (Int i = 0; i < n; i++)
        ((Int **)s.p)[i] = &targets[i];

    Int nums[] = {1, 2, 3, 4, 5};
    Slice sn = ints(a, nums, 5);
    Int one = 1;

    ClearTest tests[] = {
        {val(mt, &m), clear_map},
        {val(ints_t(), &sn), clear_slice},
        {val(reflect_slice_of(pt), &s), clear_slice},
        {val(TYPE_INT, &one), clear_panics},
    };
    testing_t_run(t, BURROW_S("map"), BURROW_FN(TestingTFunc, test_clear, &tests[0]));
    testing_t_run(t, BURROW_S("slice no pointer"),
                  BURROW_FN(TestingTFunc, test_clear, &tests[1]));
    testing_t_run(t, BURROW_S("slice has pointer"),
                  BURROW_FN(TestingTFunc, test_clear, &tests[2]));
    testing_t_run(t, BURROW_S("non-map/slice"),
                  BURROW_FN(TestingTFunc, test_clear_panics, &tests[3]));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSet)                                                                         \
    X(TestSetValue)                                                                    \
    X(TestMapIterSet)                                                                  \
    X(TestArrayElemSet)                                                                \
    X(TestPtrPointTo)                                                                  \
    X(TestPtrSetNil)                                                                   \
    X(TestMapSetNil)                                                                   \
    X(TestGrow)                                                                        \
    X(TestAppend)                                                                      \
    X(TestCopy)                                                                        \
    X(TestCopyString)                                                                  \
    X(TestCopyArray)                                                                   \
    X(TestMap)                                                                         \
    X(TestNilMap)                                                                      \
    X(TestChan)                                                                        \
    X(TestPtrTo)                                                                       \
    X(TestAddr)                                                                        \
    X(TestSlice)                                                                       \
    X(TestSlice3)                                                                      \
    X(TestSetLenCap)                                                                   \
    X(TestSetBytes)                                                                    \
    X(TestSetPanic)                                                                    \
    X(TestArrayOf)                                                                     \
    X(TestArrayOfPanicOnNegativeLength)                                                \
    X(TestSliceOf)                                                                     \
    X(TestSliceOverflow)                                                               \
    X(TestChanOf)                                                                      \
    X(TestChanOfDir)                                                                   \
    X(TestMapOf)                                                                       \
    X(TestBigZero)                                                                     \
    X(TestZeroSet)                                                                     \
    X(TestSetIter)                                                                     \
    X(TestClear)
TESTING_MAIN(TESTS)
