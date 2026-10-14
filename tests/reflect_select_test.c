/* Tests for reflect.Select, SliceAt, TypeFor and the iterators over a type's
 * fields and methods.
 *
 * These are Go's tests from all_test.go and type_test.go, and two of its
 * examples from example_test.go, under Go's names. What changes:
 *
 * TestSelect leaves out Go's watchdog, the goroutine that fails the binary when
 * a select stays blocked for ten seconds, since a test binary that hangs fails
 * here anyway. Go starts the helper that lets a blocked select finish with
 * time.AfterFunc. Here it is a goroutine that sleeps for the same pause first,
 * and the test waits for it to finish before the next round, because each
 * round's channels are made in an arena that the round lets go of.
 *
 * TestTypeFor declares its two named types by hand, as C has no local type
 * declarations that reflect can see.
 *
 * TestInitFuncTypes asks for net.IP's methods from a hundred goroutines, as
 * Go's does, which here is a test of the lock around the method values the
 * first call makes.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/proc.h"
#include "burrow/reflect.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

/* ----------------------------------------------------------------- helpers */

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

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

/* ---------------------------------------------------------------- TestSelect */

/* Go's exhaustive tester, which walks every sequence of choices the test can
 * make, starting each one at a random offset. */
typedef struct Choice {
    Int off;
    Int n;
    Int max;
} Choice;

#define MAX_CHOICES 64

typedef struct Exhaustive {
    bool started;
    Int pos;
    Int nlast;
    Choice last[MAX_CHOICES];
} Exhaustive;

static bool x_next(Exhaustive *x) {
    x->pos = 0;
    if (!x->started) {
        x->started = true;
        x->nlast = 0;
        return true;
    }
    for (Int i = x->nlast - 1; i >= 0; i--) {
        Choice *c = &x->last[i];
        if (c->n + 1 < c->max) {
            c->n++;
            x->nlast = i + 1;
            return true;
        }
    }
    return false;
}

static Int x_choose(Exhaustive *x, Int max) {
    if (x->pos >= x->nlast) {
        if (x->nlast == MAX_CHOICES)
            panic_str(BURROW_S("too many choices for the exhaustive tester"));
        x->last[x->nlast++] = (Choice){math_rand_intn(max), 0, max};
    }
    Choice *c = &x->last[x->pos++];
    if (c->max != max)
        panic_str(BURROW_S("inconsistent use of exhaustive tester"));
    return (c->n + c->off) % max;
}

static bool x_maybe(Exhaustive *x) {
    return x_choose(x, 2) == 1;
}

/* What the helper goroutine does: receive from ch, for a blocking send, or
 * send val on it, for a blocking receive. */
typedef enum HelperOp { HELPER_NONE, HELPER_RECV, HELPER_SEND } HelperOp;

typedef struct Helper {
    HelperOp op;
    ReflectValue ch;
    ReflectValue val;
    Duration pause;
    SyncAtomicUint32 done;
} Helper;

static void helper_run(void *env) {
    Helper *h = env;
    time_sleep(h->pause);
    if (h->op == HELPER_RECV) {
        Arena ar;
        bool ok;
        (void)reflect_value_recv(h->ch, arena_allocator(arena_new(&ar)), &ok);
        arena_free(&ar);
    } else {
        reflect_value_send(h->ch, h->val);
    }
    sync_atomic_uint32_store(&h->done, 1);
}

/* caseInfo describes a single case in a select test. */
typedef struct CaseInfo {
    const char *desc;
    bool can_select;
    ReflectValue recv;
    bool closed;
    HelperOp helper;
    ReflectValue helper_ch;
    ReflectValue helper_val;
    bool panic;
} CaseInfo;

#define MAX_CASES 16

typedef struct SelectRound {
    Alloc *a;
    Int nch;
    Int n;
    ReflectSelectCase cases[MAX_CASES];
    CaseInfo info[MAX_CASES];
} SelectRound;

static ReflectValue int_value(Alloc *a, Int n) {
    ReflectValue v = reflect_value_elem(reflect_new(a, TYPE_INT));
    reflect_value_set_int(v, n);
    return v;
}

/* A channel and a value to send on it, of int for most rounds and string for
 * the rest, as Go's newop picks. */
static void newop(SelectRound *r, Int n, Int cap, ReflectValue *ch, ReflectValue *v) {
    r->nch++;
    if (r->nch % 101 % 2 == 1) {
        *ch = reflect_make_chan(r->a, reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT), cap);
        *v = int_value(r->a, n);
    } else {
        *ch = reflect_make_chan(r->a, reflect_chan_of(REFLECT_BOTH_DIR, TYPE_STRING),
                                cap);
        *v = reflect_value_elem(reflect_new(r->a, TYPE_STRING));
        reflect_value_set_string(*v, strconv_itoa(r->a, n));
    }
}

static void add(SelectRound *r, ReflectSelectDir dir, ReflectValue ch,
                ReflectValue send, CaseInfo info) {
    r->cases[r->n] = (ReflectSelectCase){dir, ch, send};
    r->info[r->n] = info;
    r->n++;
}

/* fmtSelect, into buf. */
static const char *fmt_select(SelectRound *r, char *buf, size_t size) {
    size_t at = 0;
    at += (size_t)snprintf(buf + at, size - at, "\nselect {\n");
    for (Int i = 0; i < r->n && at < size; i++) {
        const CaseInfo *c = &r->info[i];
        at += (size_t)snprintf(buf + at, size - at, "%d: %s", (int)i, c->desc);
        if (at < size && reflect_value_is_valid(c->recv)) {
            Str v = fmt_sprintf_v(r->a, " val=%#v", reflect_value_interface(c->recv));
            at += (size_t)snprintf(buf + at, size - at, "%.*s", (int)v.len,
                                   (const char *)v.p);
        }
        if (at < size && c->can_select)
            at += (size_t)snprintf(buf + at, size - at, " canselect");
        if (at < size && c->panic)
            at += (size_t)snprintf(buf + at, size - at, " panic");
        if (at < size)
            at += (size_t)snprintf(buf + at, size - at, "\n");
    }
    if (at < size)
        (void)snprintf(buf + at, size - at, "}");
    return buf;
}

typedef struct RunSelect {
    Alloc *a;
    Slice cases;
    Int chosen;
    ReflectValue recv;
    bool recv_ok;
} RunSelect;

static void run_select_body(void *env) {
    RunSelect *rs = env;
    rs->chosen = reflect_select(rs->a, rs->cases, &rs->recv, &rs->recv_ok);
}

/* runSelect: Select's answers, and whether it panicked. */
static bool run_select(RunSelect *rs) {
    char buf[256];
    return panics(run_select_body, rs, buf, sizeof buf);
}

/* One round of TestSelect. Returns false when the test should stop. */
static bool select_round(TestingT *t, Exhaustive *x, SelectRound *r) {
    ReflectValue none = {0};
    ReflectValue ch;
    ReflectValue v;

    /* Ready send. */
    if (x_maybe(x)) {
        newop(r, r->n, 1, &ch, &v);
        add(r, REFLECT_SELECT_SEND, ch, v,
            (CaseInfo){.desc = "ready send", .can_select = true});
    }

    /* Ready recv. */
    if (x_maybe(x)) {
        newop(r, r->n, 1, &ch, &v);
        reflect_value_send(ch, v);
        add(r, REFLECT_SELECT_RECV, ch, none,
            (CaseInfo){.desc = "ready recv", .can_select = true, .recv = v});
    }

    /* Blocking send. */
    if (x_maybe(x)) {
        newop(r, r->n, 0, &ch, &v);
        /* Let it execute? */
        if (x_maybe(x))
            add(r, REFLECT_SELECT_SEND, ch, v,
                (CaseInfo){
                    .desc = "blocking send", .helper = HELPER_RECV, .helper_ch = ch});
        else
            add(r, REFLECT_SELECT_SEND, ch, v, (CaseInfo){.desc = "blocking send"});
    }

    /* Blocking recv. */
    if (x_maybe(x)) {
        newop(r, r->n, 0, &ch, &v);
        /* Let it execute? */
        if (x_maybe(x))
            add(r, REFLECT_SELECT_RECV, ch, none,
                (CaseInfo){.desc = "blocking recv",
                           .recv = v,
                           .helper = HELPER_SEND,
                           .helper_ch = ch,
                           .helper_val = v});
        else
            add(r, REFLECT_SELECT_RECV, ch, none, (CaseInfo){.desc = "blocking recv"});
    }

    /* Zero Chan send. */
    if (x_maybe(x)) {
        /* Maybe include value to send. */
        ReflectValue send = {0};
        if (x_maybe(x))
            send = int_value(r->a, 100);
        add(r, REFLECT_SELECT_SEND, none, send, (CaseInfo){.desc = "zero Chan send"});
    }

    /* Zero Chan receive. */
    if (x_maybe(x))
        add(r, REFLECT_SELECT_RECV, none, none, (CaseInfo){.desc = "zero Chan recv"});

    ReflectType chan_int = reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT);

    /* nil Chan send. */
    if (x_maybe(x))
        add(r, REFLECT_SELECT_SEND, reflect_zero(chan_int), int_value(r->a, 101),
            (CaseInfo){.desc = "nil Chan send"});

    /* nil Chan recv. */
    if (x_maybe(x))
        add(r, REFLECT_SELECT_RECV, reflect_zero(chan_int), none,
            (CaseInfo){.desc = "nil Chan recv"});

    /* closed Chan send. */
    if (x_maybe(x)) {
        ch = reflect_make_chan(r->a, chan_int, 0);
        reflect_value_close(ch);
        add(r, REFLECT_SELECT_SEND, ch, int_value(r->a, 101),
            (CaseInfo){.desc = "closed Chan send", .can_select = true, .panic = true});
    }

    /* closed Chan recv. */
    if (x_maybe(x)) {
        newop(r, r->n, 0, &ch, &v);
        reflect_value_close(ch);
        v = reflect_zero(reflect_value_type(v));
        add(r, REFLECT_SELECT_RECV, ch, none,
            (CaseInfo){.desc = "closed Chan recv",
                       .can_select = true,
                       .closed = true,
                       .recv = v});
    }

    Helper helper = {.op = HELPER_NONE};

    /* Add default? Must be last case here, but will permute. Add the default
     * if the select would otherwise block forever, and maybe add it anyway. */
    Int num_can_select = 0;
    bool can_proceed = false;
    bool can_block = true;
    bool can_panic = false;
    Int helpers[MAX_CASES];
    Int nhelpers = 0;
    for (Int i = 0; i < r->n; i++) {
        const CaseInfo *c = &r->info[i];
        if (c->can_select) {
            can_proceed = true;
            can_block = false;
            num_can_select++;
            if (c->panic)
                can_panic = true;
        } else if (c->helper != HELPER_NONE) {
            can_proceed = true;
            helpers[nhelpers++] = i;
        }
    }
    if (!can_proceed || x_maybe(x)) {
        add(r, REFLECT_SELECT_DEFAULT, none, none,
            (CaseInfo){.desc = "default", .can_select = can_block});
        num_can_select++;
    } else if (can_block) {
        /* Select needs to communicate with another goroutine. */
        CaseInfo *cas = &r->info[helpers[x_choose(x, nhelpers)]];
        helper.op = cas->helper;
        helper.ch = cas->helper_ch;
        helper.val = cas->helper_val;
        cas->can_select = true;
        num_can_select++;
    }

    /* Permute cases and case info. Doing too much here makes the exhaustive
     * loop too exhausting, so just do two swaps. */
    for (int loop = 0; loop < 2; loop++) {
        Int i = x_choose(x, r->n);
        Int j = x_choose(x, r->n);
        ReflectSelectCase c = r->cases[i];
        r->cases[i] = r->cases[j];
        r->cases[j] = c;
        CaseInfo in = r->info[i];
        r->info[i] = r->info[j];
        r->info[j] = in;
    }

    if (helper.op != HELPER_NONE) {
        /* We wait before kicking off a goroutine to satisfy a blocked select.
         * The pause needs to be big enough to let the select block before we
         * run the helper, but if we lose that race once in a while it's okay:
         * the select will just proceed immediately. */
        helper.pause = 10 * TIME_MICROSECOND;
        if (testing_short())
            helper.pause = 100 * TIME_MICROSECOND;
        if (!go(BURROW_FN(Func, helper_run, &helper))) {
            testing_t_errorf_v(t, "could not start the helper goroutine");
            return false;
        }
    }

    /* Run select. */
    RunSelect rs = {r->a,
                    slice_from(r->cases, r->n, r->n, TYPE_REFLECT_SELECT_CASE),
                    -1,
                    {0},
                    false};
    bool panicked = run_select(&rs);

    if (helper.op != HELPER_NONE)
        while (sync_atomic_uint32_load(&helper.done) == 0)
            runtime_gosched();

    char buf[1024];
    if (panicked && !can_panic) {
        testing_t_errorf_v(t, "%s\npanicked unexpectedly",
                           fmt_select(r, buf, sizeof buf));
        return false;
    }
    if (!panicked && can_panic && num_can_select == 1) {
        testing_t_errorf_v(t, "%s\nselected #%d incorrectly (should panic)",
                           fmt_select(r, buf, sizeof buf), rs.chosen);
        return false;
    }
    if (panicked)
        return true;

    Int i = rs.chosen;
    const CaseInfo *cas = &r->info[i];
    if (!cas->can_select) {
        testing_t_errorf_v(t, "%s\nselected #%d incorrectly",
                           fmt_select(r, buf, sizeof buf), i);
        return false;
    }
    if (cas->panic) {
        testing_t_errorf_v(t, "%s\nselected #%d incorrectly (case should panic)",
                           fmt_select(r, buf, sizeof buf), i);
        return false;
    }

    if (r->cases[i].dir == REFLECT_SELECT_RECV) {
        if (!reflect_value_is_valid(rs.recv)) {
            testing_t_errorf_v(t, "%s\nselected #%d but got an invalid Value, %t",
                               fmt_select(r, buf, sizeof buf), i, rs.recv_ok);
            return false;
        }
        if (!reflect_value_is_valid(cas->recv)) {
            testing_t_errorf_v(
                t, "%s\nselected #%d but internal error: missing recv value",
                fmt_select(r, buf, sizeof buf), i);
            return false;
        }
        bool want_ok = !cas->closed;
        if (!reflect_value_equal(rs.recv, cas->recv) || rs.recv_ok != want_ok) {
            testing_t_errorf_v(t, "%s\nselected #%d but got %#v, %t, want %#v, %t",
                               fmt_select(r, buf, sizeof buf), i,
                               reflect_value_interface(rs.recv), rs.recv_ok,
                               reflect_value_interface(cas->recv), want_ok);
            return false;
        }
    } else if (reflect_value_is_valid(rs.recv) || rs.recv_ok) {
        testing_t_errorf_v(t, "%s\nselected #%d but got a valid Value or true",
                           fmt_select(r, buf, sizeof buf), i);
        return false;
    }
    return true;
}

static void TestSelect(TestingT *t) {
    Exhaustive x = {0};
    Int nch = 0;
    for (Int n = 0; x_next(&x); n++) {
        if (testing_short() && n >= 1000)
            break;
        if (n >= 100000)
            break;
        Arena ar;
        SelectRound r = {.a = arena_allocator(arena_new(&ar)), .nch = nch};
        bool more = select_round(t, &x, &r);
        nch = r.nch;
        arena_free(&ar);
        if (!more)
            break;
    }
}

/* -------------------------------------------------------- TestSelectMaxCases */

static void TestSelectMaxCases(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectValue channel =
        reflect_make_chan(a, reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT), 0);
    reflect_value_close(channel);
    Int n = 65536 + 1;
    ReflectSelectCase *cases =
        mem_alloc(a, (size_t)n * sizeof *cases, _Alignof(ReflectSelectCase));
    CHECK(cases != NULL);
    for (Int i = 0; i < n; i++)
        cases[i] = (ReflectSelectCase){REFLECT_SELECT_RECV, channel, {0}};

    /* Should not panic */
    RunSelect rs = {
        a, slice_from(cases, 65536, 65536, TYPE_REFLECT_SELECT_CASE), -1, {0}, false};
    char buf[256];
    CHECK(!panics(run_select_body, &rs, buf, sizeof buf));

    /* Should panic */
    rs.cases = slice_from(cases, n, n, TYPE_REFLECT_SELECT_CASE);
    if (!panics(run_select_body, &rs, buf, sizeof buf))
        testing_t_errorf_v(t, "expected select call to panic with greater than max "
                              "supported cases");
    else if (strcmp(buf, "reflect.Select: too many cases (max 65536)") != 0)
        testing_t_errorf_v(t,
                           "unexpected error from select call with greater than max "
                           "supported cases: %s",
                           buf);
    arena_free(&ar);
}

/* ------------------------------------------------------------- TestSelectNop */

static void TestSelectNop(TestingT *t) {
    /* "select { default: }" should always return the default case. */
    ReflectSelectCase c[] = {{REFLECT_SELECT_DEFAULT, {0}, {0}}};
    Int chosen = reflect_select(
        heap_allocator(), slice_from(c, 1, 1, TYPE_REFLECT_SELECT_CASE), NULL, NULL);
    if (chosen != 0)
        testing_t_fatalf_v(t, "expected Select to return 0, but got %d", chosen);
}

/* ---------------------------------------------------------------- TestSliceAt */

typedef struct SliceAtCall {
    Alloc *a;
    ReflectType t;
    void *p;
    Int n;
} SliceAtCall;

static void slice_at_call(void *env) {
    const SliceAtCall *c = env;
    (void)reflect_slice_at(c->a, c->t, c->p, c->n);
}

static void TestSliceAt(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Byte p[10] = {0};

    ReflectType typ = TYPE_UINT8;

    ReflectValue s = reflect_slice_at(a, typ, &p[0], 10);
    if (reflect_value_pointer(s) != (Uintptr)(uintptr_t)&p[0])
        testing_t_fatalf_v(t, "unexpected underlying array: %d, want: %d",
                           reflect_value_pointer(s), (Uintptr)(uintptr_t)&p[0]);
    if (reflect_value_len(s) != 10 || reflect_value_cap(s) != 10)
        testing_t_fatalf_v(t, "unexpected len or cap, len: %d, cap: %d, want: %d",
                           reflect_value_len(s), reflect_value_cap(s), 10);

    typ = TYPE_INT;
    if (!reflect_value_is_nil(reflect_slice_at(a, typ, NULL, 0)))
        testing_t_fatal_v(t, "nil pointer with zero length must return nil");

    /* nil pointer with positive length panics */
    should_panic(t, "", slice_at_call, &(SliceAtCall){a, typ, NULL, 1});

    /* negative length */
    Int neg = -1;
    should_panic(t, "", slice_at_call, &(SliceAtCall){a, TYPE_UINT8, &p[0], neg});

    /* size overflows address space */
    uint64_t n = 0;
    should_panic(t, "", slice_at_call,
                 &(SliceAtCall){a, TYPE_UINT64, &n, (Int)(UINTPTR_MAX / 8 + 1)});
    should_panic(t, "", slice_at_call,
                 &(SliceAtCall){a, TYPE_UINT64, &n, (Int)(UINTPTR_MAX / 8 + 2)});

    /* sliced memory overflows address space */
    void *last = (void *)UINTPTR_MAX;
    should_panic(t, "", slice_at_call, &(SliceAtCall){a, typ, last, 2});
    arena_free(&ar);
}

/* ---------------------------------------------------------------- TestTypeFor */

typedef Str mystring;
typedef Any myiface;

static const Type burrow_type_mystring = {
    S_("mystring"),
    TEST_PKG,
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
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

static const Type burrow_type_myiface = {
    S_("myiface"),
    TEST_PKG,
    KIND_INTERFACE,
    (uint32_t)sizeof(Any),
    (uint16_t)_Alignof(Any),
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

/* reflect.ValueOf(new(T)).Elem().Type(), for the T that p points at. */
static ReflectType want_from(ReflectType t, void *p) {
    return reflect_value_type(reflect_value_elem(val(reflect_pointer_to(t), &p)));
}

static void TestTypeFor(TestingT *t) {
    Int i = 0;
    int64_t i64 = 0;
    Str s = {0};
    mystring ms = {0};
    Any any = {0};
    myiface mi = {0};

    struct {
        ReflectType want_from;
        ReflectType got;
    } testcases[] = {
        {want_from(TYPE_INT, &i), reflect_type_for(Int)},
        {want_from(TYPE_INT64, &i64), reflect_type_for(int64_t)},
        {want_from(TYPE_STRING, &s), reflect_type_for(Str)},
        {want_from(&burrow_type_mystring, &ms), reflect_type_for(mystring)},
        {want_from(TYPE_OF(Any), &any), reflect_type_for(Any)},
        {want_from(&burrow_type_myiface, &mi), reflect_type_for(myiface)},
    };
    for (size_t k = 0; k < sizeof testcases / sizeof testcases[0]; k++) {
        ReflectType want = testcases[k].want_from;
        if (want != testcases[k].got)
            testing_t_errorf_v(t, "unexpected reflect.Type: got %s; want %s",
                               reflect_type_string(testcases[k].got),
                               reflect_type_string(want));
    }
}

/* ---------------------------------------------------------- TestInitFuncTypes */

static bool count_method(void *env, const void *m) {
    (void)m;
    (*(Int *)env)++;
    return true;
}

static void init_func_types(void *env) {
    (void)env;
    ReflectType ipT = TYPE_OF(NetIP);
    IterSeq seq = reflect_type_methods(ipT);
    Int n = 0;
    seq.f(seq.env, BURROW_FN(IterYield, count_method, &n));
}

static void TestInitFuncTypes(TestingT *t) {
    (void)t;
    Int n = 100;
    static SyncWaitGroup wg;

    for (Int i = 0; i < n; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, init_func_types, NULL));
    sync_wait_group_wait(&wg);
}

/* ------------------------------------------------------------------ examples */

#define PERSON_FIELDS(F, T)                                                            \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Age, "")                                                                 \
    F(T, Str, City, "")                                                                \
    F(T, Str, Country, "")
BURROW_STRUCT(Person, PERSON_FIELDS);

static bool print_field(void *env, const void *k, const void *v) {
    (void)env;
    const ReflectStructField *field = k;
    const ReflectValue *value = v;
    fmt_printf_v("  %s = %v\n", field->name, reflect_value_interface(*value));
    return true;
}

static void ExampleValue_Fields(void) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Person p = {BURROW_S("Alice"), 30, BURROW_S("New York"), BURROW_S("USA")};
    ReflectValue v = val(TYPE_OF(Person), &p);

    fmt_println_v(BURROW_S("Iterating over all struct fields:"));
    IterSeq2 fields = reflect_value_fields(v, a);
    fields.f(fields.env, BURROW_FN(IterYield2, print_field, NULL));
    arena_free(&ar);
}

#define LOOKUP_S_FIELDS(F, T)                                                          \
    F(T, Str, F0, "alias:\"field_0\"")                                                 \
    F(T, Str, F1, "alias:\"\"")                                                        \
    F(T, Str, F2, "")
BURROW_STRUCT(LookupS, LOOKUP_S_FIELDS);

static bool print_alias(void *env, const void *f) {
    const ReflectStructField *field = f;
    Str alias;
    if (reflect_struct_tag_lookup(field->tag, env, BURROW_S("alias"), &alias)) {
        if (alias.len == 0)
            fmt_println_v(BURROW_S("(blank)"));
        else
            fmt_println_v(alias);
    } else {
        fmt_println_v(BURROW_S("(not specified)"));
    }
    return true;
}

static void ExampleStructTag_Lookup(void) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ReflectType st = TYPE_OF(LookupS);
    IterSeq fields = reflect_type_fields(st);
    fields.f(fields.env, BURROW_FN(IterYield, print_alias, a));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSelect)                                                                      \
    X(TestSelectMaxCases)                                                              \
    X(TestSelectNop)                                                                   \
    X(TestSliceAt)                                                                     \
    X(TestTypeFor)                                                                     \
    X(TestInitFuncTypes)                                                               \
    X(ExampleValue_Fields, "Iterating over all struct fields:\n"                       \
                           "  Name = Alice\n"                                          \
                           "  Age = 30\n"                                              \
                           "  City = New York\n"                                       \
                           "  Country = USA\n")                                        \
    X(ExampleStructTag_Lookup, "field_0\n(blank)\n(not specified)\n")

TESTING_MAIN(TESTS)
