/* Derived from Go's src/uuid/uuid_test.go. Go source: go1.27.1.
 *
 * The two NewV7 tests run in synctest bubbles, as Go's do, so the clock only
 * moves when the test sleeps. What they find is written down in statics and
 * checked once runtime_main has returned, on the test's own thread.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/io.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/synctest.h"
#include "burrow/time.h"
#include "burrow/uuid.h"

#include <stdio.h>
#include <string.h>

static const Uuid u1 = {{0xf8, 0x1d, 0x4f, 0xae, 0x7d, 0xec, 0x11, 0xd0, 0xa7, 0x65,
                         0x00, 0xa0, 0xc9, 0x1e, 0x6b, 0xf6}};

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static Byte version(Uuid u) {
    return u.b[6] >> 4;
}

static Byte variant(Uuid u) {
    return u.b[8] >> 6;
}

static bool uuid_eq(Uuid u, Uuid v) {
    return memcmp(u.b, v.b, 16) == 0;
}

static void TestNew(TestingT *t) {
    static const struct {
        const char *name;
        Uuid (*newf)(void);
        Byte version;
        Byte variant;
    } tests[] = {
        {"New", uuid_new, 4, 2},
        {"NewV4", uuid_new_v4, 4, 2},
        {"NewV7", uuid_new_v7, 7, 2},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Uuid u = tests[i].newf();
        if (version(u) != tests[i].version)
            testing_t_errorf_v(t, "%s: got version %d, want %d", tests[i].name,
                               (int)version(u), (int)tests[i].version);
        if (variant(u) != tests[i].variant)
            testing_t_errorf_v(t, "%s: got variant %d, want %d", tests[i].name,
                               (int)variant(u), (int)tests[i].variant);
    }
}

/* ------------------------------------------------------------ NewV7 millis */

static int millis_checks;
static int millis_bad;
static uint64_t millis_got;
static uint64_t millis_want;

static void check_millis(void) {
    Uuid u = uuid_new_v7();
    uint64_t hi = 0;
    for (int i = 0; i < 8; i++)
        hi = hi << 8 | u.b[i];
    uint64_t got = hi >> 16;
    int64_t now = burrow__bubble_now(burrow__curbubble());
    uint64_t want = (uint64_t)(now / 1000000);
    millis_checks++;
    if (got != want && millis_bad++ == 0) {
        millis_got = got;
        millis_want = want;
    }
}

static void millis_body(void *env) {
    (void)env;
    check_millis();
    time_sleep(TIME_HOUR);
    check_millis();
    time_sleep(TIME_SECOND - 1); /* the most fractional seconds there can be */
    check_millis();
    time_sleep(2); /* the fewest */
    check_millis();
}

static void millis_again(void *env) {
    (void)env;
    check_millis();
}

static void millis_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, millis_body, NULL));
    /* A new bubble starts its clock over, so time goes backwards, and the
     * UUIDs have to follow the new time. */
    (void)synctest_run(BURROW_FN(Func, millis_again, NULL));
}

static void TestNewV7Millis(TestingT *t) {
    millis_top(NULL);
    if (millis_checks != 5)
        testing_t_errorf_v(t, "ran %d checks, want 5", millis_checks);
    if (millis_bad > 0)
        testing_t_errorf_v(t, "%d checks failed, the first with millis = %x, want %x",
                           millis_bad, millis_got, millis_want);
}

/* --------------------------------------------------------- NewV7 collision */

static int collision_bad;
static Uuid collision_prev;
static Uuid collision_cur;

static void collision_body(void *env) {
    (void)env;
    Uuid last = uuid_new_v7();
    for (int round = 0; round < 3; round++) {
        /* Enough to run past the fraction of a millisecond several times. */
        for (int i = 0; i < (1 << 12) * 3; i++) {
            Uuid u = uuid_new_v7();
            if (uuid_cmp(u, last) != 1) {
                collision_bad = 1;
                collision_prev = last;
                collision_cur = u;
                return;
            }
            last = u;
        }
        /* The clock moves on, but more slowly than the UUIDs are made. */
        time_sleep(TIME_MILLISECOND);
    }
}

static void collision_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, collision_body, NULL));
}

static void TestNewV7Collision(TestingT *t) {
    collision_top(NULL);
    if (collision_bad) {
        Alloc *h = heap_allocator();
        testing_t_fatalf_v(
            t, "NewV7 returned UUIDs out of order:\nprevious: %s\n current: %s",
            uuid_string(collision_prev, h), uuid_string(collision_cur, h));
    }
}

/* ------------------------------------------------------------------ text */

static void TestEncode(TestingT *t) {
    Alloc *h = heap_allocator();
    Str want = BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6");
    Str got = uuid_string(u1, h);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "u.String() = %q, want %q", got, want);
    mem_free(h, (void *)(uintptr_t)got.p, (size_t)got.len, 1);

    Error err = io_eof;
    Slice m = uuid_marshal_text(u1, h, &err);
    Str ms = {(const Byte *)m.p, m.len};
    if (!str_eq(ms, want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "u.MarshalText() = %q, %v; want %q, nil", ms, err, want);
    mem_free(h, m.p, (size_t)m.cap, 1);

    Byte buf[9];
    memcpy(buf, "urn:uuid:", 9);
    Slice prefix = {buf, 9, 9, TYPE_BYTE};
    err = io_eof;
    Slice ap = uuid_append_text(u1, h, prefix, &err);
    Str as = {(const Byte *)ap.p, ap.len};
    Str want2 = BURROW_S("urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6");
    if (!str_eq(as, want2) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "u.MarshalAppend(%q) = %q, %v; want %q, nil",
                           BURROW_S("urn:uuid:"), as, err, want2);
    if (ap.p != buf)
        mem_free(h, ap.p, (size_t)ap.cap, 1);
}

static void TestUnmarshalText(TestingT *t) {
    Uuid got = uuid_nil();
    Error err =
        uuid_unmarshal_text(&got, BURROW_B("f81d4fae-7dec-11d0-a765-00a0c91e6bf6"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "UnmarshalText: %v", err);
    if (!uuid_eq(got, u1)) {
        Alloc *h = heap_allocator();
        testing_t_errorf_v(t, "got %s, want %s", uuid_string(got, h),
                           uuid_string(u1, h));
    }
}

static void TestParseSuccess(TestingT *t) {
    Alloc *h = heap_allocator();
    struct {
        const char *s;
        Uuid u;
    } tests[] = {
        {"00000000-0000-0000-0000-000000000000", uuid_nil()},
        {"ffffffff-ffff-ffff-ffff-ffffffffffff", uuid_max()},
        {"f81d4fae-7dec-11d0-a765-00a0c91e6bf6", u1},
        {"F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6", u1},
        {"f81d4fae7dec11d0a76500a0c91e6bf6", u1},
        {"{f81d4fae-7dec-11d0-a765-00a0c91e6bf6}", u1},
        {"urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6", u1},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Error err;
        Uuid u = uuid_parse_str(cs(tests[i].s), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Parse(%q) = _, %v; want success", cs(tests[i].s),
                               err);
        else if (!uuid_eq(u, tests[i].u))
            testing_t_errorf_v(t, "Parse(%q) = %s, nil; want %s", cs(tests[i].s),
                               uuid_string(u, h), uuid_string(tests[i].u, h));
    }
}

static void TestParseErrors(TestingT *t) {
    static const char *const tests[] = {
        "",
        "0000000000000-0000-0000-000000000000",
        "00000000-000000000-0000-000000000000",
        "00000000-0000-000000000-000000000000",
        "00000000-0000-0000-00000000000000000",
        "00000000-0000-0000-0000-00000000000",
        "x0000000-0000-0000-0000-000000000000",
        "00000000-x000-0000-0000-000000000000",
        "00000000-0000-x000-0000-000000000000",
        "00000000-0000-0000-x000-000000000000",
        "00000000-0000-0000-0000-x00000000000",
        "{x0000000-0000-0000-0000-000000000000}",
        "urn:uuid:x000000-0000-0000-0000-000000000000",
        "x0000000000000000000000000000000",
        /* Some parsers take hyphens in other places. Go's does not. */
        "0000-0000-0000-0000-0000-0000-0000-0000",
        /* Mixtures of the forms that could be accepted and are not. */
        "{00000000000000000000000000000000}",
        "{urn:uuid:00000000-0000-0000-0000-000000000000}",
        "urn:uuid:00000000000000000000000000000000",
        /* Not in Go's list: one brace but not the other, which Go trims one at
         * a time and so still rejects. */
        "{f81d4fae-7dec-11d0-a765-00a0c91e6bf6x",
        "xf81d4fae-7dec-11d0-a765-00a0c91e6bf6}",
        "urx:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6",
    };
    Alloc *h = heap_allocator();
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Error err;
        Uuid got = uuid_parse_str(cs(tests[i]), &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Parse(%q) = %s, nil; want error", cs(tests[i]),
                               uuid_string(got, h));
        else if (!str_eq(error_text(err), BURROW_S("invalid uuid")))
            testing_t_errorf_v(t, "Parse(%q) error = %q, want %q", cs(tests[i]),
                               error_text(err), BURROW_S("invalid uuid"));
    }
}

static void TestCompare(TestingT *t) {
    Uuid uuids[3];
    uuids[0] = uuid_nil();
    uuids[1] = uuid_must_parse(BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6"));
    uuids[2] = uuid_max();
    for (int i = 0; i < 3; i++) {
        Uuid u = uuids[i];
        if (uuid_cmp(u, u) != 0)
            testing_t_errorf_v(t, "%d.Compare(itself) = %d, want 0", i,
                               (int)uuid_cmp(u, u));
        if (i == 0)
            continue;
        Uuid prev = uuids[i - 1];
        if (uuid_cmp(u, prev) != 1)
            testing_t_errorf_v(t, "%d.Compare(%d) = %d, want 1", i, i - 1,
                               (int)uuid_cmp(u, prev));
        if (uuid_cmp(prev, u) != -1)
            testing_t_errorf_v(t, "%d.Compare(%d) = %d, want -1", i - 1, i,
                               (int)uuid_cmp(prev, u));
    }
}

/* ------------------------------------------------------------ descriptor */

static void TestDescriptor(TestingT *t) {
    const Type *ty = TYPE_UUID;
    if (ty->kind != KIND_ARRAY || ty->len != 16 || ty->size != 16)
        testing_t_errorf_v(t, "kind %d len %d size %d, want an array of 16",
                           (int)ty->kind, (int)ty->len, (int)ty->size);
    static const char *const names[] = {"AppendText", "MarshalText", "String",
                                        "UnmarshalText"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (type_method_by_name(ty, cs(names[i])) == NULL)
            testing_t_errorf_v(t, "no %s method", cs(names[i]));
}

/* A Uuid in a struct goes through encoding/json by its text methods. The
 * expected lines are what go1.27.1 printed for the same program in Go. */

#define ORDER_FIELDS(F, T)                                                             \
    F(T, Uuid, ID, "json:\"id\"")                                                      \
    F(T, Int, Qty, "json:\"qty\"")
BURROW_STRUCT(Order, ORDER_FIELDS);

static void TestJSON(TestingT *t) {
    Alloc *h = heap_allocator();
    Order o = {u1, 3};
    Error err = BURROW_NO_ERROR;
    Slice out = json_marshal(h, BURROW_ANY(TYPE_OF(Order), &o), &err);
    Str got = {(const Byte *)out.p, out.len};
    Str want = BURROW_S("{\"id\":\"f81d4fae-7dec-11d0-a765-00a0c91e6bf6\",\"qty\":3}");
    if (!str_eq(got, want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Marshal = %q, %v; want %q, nil", got, err, want);
    mem_free(h, out.p, (size_t)out.cap, 1);

    static const struct {
        const char *in;
        const char *err;
    } tests[] = {
        {"{\"id\":\"urn:uuid:F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6\",\"qty\":1}", NULL},
        {"{\"id\":\"nope\",\"qty\":1}", "invalid uuid"},
        {"{\"id\":7}",
         "json: cannot unmarshal number into Go struct field Order.id of type "
         "uuid.UUID: JSON value must be string type"},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Order p = {uuid_nil(), 0};
        Int n = (Int)strlen(tests[i].in);
        Slice in = {(void *)(uintptr_t)tests[i].in, n, n, TYPE_BYTE};
        err = json_unmarshal(h, in, BURROW_ANY(TYPE_OF(Order), &p));
        Str es = BURROW_FAILED(err) ? error_text(err) : BURROW_S("<nil>");
        Str ew = cs(tests[i].err == NULL ? "<nil>" : tests[i].err);
        if (!str_eq(es, ew))
            testing_t_errorf_v(t, "Unmarshal(%q) error = %q, want %q", cs(tests[i].in),
                               es, ew);
        if (tests[i].err == NULL && (!uuid_eq(p.ID, u1) || p.Qty != 1))
            testing_t_errorf_v(t, "Unmarshal(%q) = %s %d, want %s 1", cs(tests[i].in),
                               uuid_string(p.ID, h), (int)p.Qty, uuid_string(u1, h));
    }
}

/* ------------------------------------------------------------ benchmarks */

static Uuid sink;

static void BenchmarkNewV4(TestingB *b) {
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = uuid_new_v4();
}

static void BenchmarkNewV7(TestingB *b) {
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = uuid_new_v7();
}

static void BenchmarkString(TestingB *b) {
    Uuid u = uuid_must_parse(BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6"));
    Byte buf[64];
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice s =
            uuid_append_text(u, NULL, (Slice){buf, 0, sizeof(buf), TYPE_BYTE}, NULL);
        sink.b[0] = ((Byte *)s.p)[0];
    }
}

static void BenchmarkParseSuccess(TestingB *b) {
    Error err;
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = uuid_parse_str(BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6"), &err);
}

static void BenchmarkParseError(TestingB *b) {
    Error err;
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = uuid_parse_str(BURROW_S("00000000-0000-0000-0000-00000000000X"), &err);
}

#define TESTS(X)                                                                       \
    X(TestNew)                                                                         \
    X(TestNewV7Millis)                                                                 \
    X(TestNewV7Collision)                                                              \
    X(TestEncode)                                                                      \
    X(TestUnmarshalText)                                                               \
    X(TestParseSuccess)                                                                \
    X(TestParseErrors)                                                                 \
    X(TestCompare)                                                                     \
    X(TestDescriptor)                                                                  \
    X(TestJSON)                                                                        \
    X(BenchmarkNewV4)                                                                  \
    X(BenchmarkNewV7)                                                                  \
    X(BenchmarkString)                                                                 \
    X(BenchmarkParseSuccess)                                                           \
    X(BenchmarkParseError)

TESTING_MAIN(TESTS)
