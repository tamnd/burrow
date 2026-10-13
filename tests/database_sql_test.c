/* database/sql against the fake driver in database_sql_fakedb.h, from Go's
 * sql_test.go.
 *
 * Each test that Go runs through testDatabase runs here three times, once for
 * each of the connectors: basic, which has only the bare minimum of a driver
 * Conn, default, which is the fake driver itself, and scancols, whose Rows
 * scan their own columns. Every run is in a synctest bubble, so that waiting
 * for the pool to settle is a fact and not a sleep.
 *
 * Go's tests stop with t.Fatal. Here they report with testing_t_errorf_v and
 * return, since the test is in a bubble of its own and stopping it there would
 * leave the database open.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "database_sql_fakedb.h"

#include "burrow/math/rand.h"
#include "burrow/proc.h"
#include "burrow/synctest.h"
#include "burrow/testing.h"
#include "burrow/uuid.h"

#include <string.h>

#define NO_ARGS slice_nil(TYPE_ANY)
#define ARGS(...) BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__)
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_errorf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

static const Str fake_db_name = BURROW_S_INIT("foo");

static Time chris_birthday(void) {
    return time_from_unix(123456789, 0);
}

/* ------------------------------------------------------------ harness */

static SyncOnce td_register_once;

static void td_register(void *env) {
    (void)env;
    sql_register(BURROW_S("test"), fdriver);
}

/* init in fakedb_test.go. */
static void register_test_driver(void) {
    sync_once_do(&td_register_once, BURROW_FN(Func, td_register, NULL));
}

static bool exec_args(TestingT *t, SqlDB *db, const char *query, Slice args) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec(db, arena_allocator(&ar), str_from_cstr(query), args, &err);
    arena_free(&ar);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Exec of %q: %v", query, err);
        return false;
    }
    return true;
}

#define EXEC(t, db, query) exec_args((t), (db), (query), NO_ARGS)
#define EXECV(t, db, query, ...) exec_args((t), (db), (query), ARGS(__VA_ARGS__))

static bool populate(TestingT *t, SqlDB *db, const char *name) {
    if (strcmp(name, "people") == 0) {
        Time bday = chris_birthday();
        return EXEC(t, db,
                    "CREATE|people|name=string,age=int32,photo=blob,dead=bool,"
                    "bdate=datetime") &&
               EXECV(t, db, "INSERT|people|name=Alice,age=?,photo=APHOTO", 1) &&
               EXECV(t, db, "INSERT|people|name=Bob,age=?,photo=BPHOTO", 2) &&
               EXECV(t, db, "INSERT|people|name=Chris,age=?,photo=CPHOTO,bdate=?", 3,
                     ((Any){TYPE_TIME, &bday}));
    }
    if (strcmp(name, "magicquery") == 0) {
        /* Magic table name and column, known by fakedb. */
        return EXEC(t, db, "CREATE|magicquery|op=string,millis=int32") &&
               EXEC(t, db, "INSERT|magicquery|op=sleep,millis=10");
    }
    if (strcmp(name, "tx_status") == 0) {
        /* Magic table name and column, known by fakedb. */
        return EXEC(t, db, "CREATE|tx_status|tx_status=string") &&
               EXEC(t, db, "INSERT|tx_status|tx_status=invalid");
    }
    testing_t_errorf_v(t, "unknown database name %q", name);
    return false;
}

static TestingT *td_close_t;

static void td_post_close(FakeConn *c, Error err) {
    (void)c;
    if (BURROW_FAILED(err) && td_close_t != NULL)
        testing_t_errorf_v(td_close_t, "Error closing fakeConn: %v", err);
}

/* closeDB, which also frees db. */
static void close_db(TestingT *t, SqlDB *db) {
    td_close_t = t;
    fdb_set_hook_post_close_conn(td_post_close);
    Int nfree = burrow__sql_db_num_free(db);
    for (Int i = 0; i < nfree; i++) {
        Int n = burrow__sql_db_free_conn_open_stmts(db, i);
        if (n > 0)
            testing_t_errorf_v(t,
                               "while closing db, freeConn %d/%d had %d open stmts; "
                               "want 0",
                               i, nfree, n);
    }
    Error err = sql_db_close(db);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error closing DB: %v", err);
    /* Connections close asynchronously; wait for them to finish doing so. */
    synctest_wait();
    Int n = burrow__sql_db_num_open(db);
    if (n != 0)
        testing_t_errorf_v(t, "%d connections still open after closing DB", n);
    sql_db_free(db);
    fdb_set_hook_post_close_conn(NULL);
    td_close_t = NULL;
}

static Int num_free_conns(SqlDB *db) {
    synctest_wait();
    return burrow__sql_db_num_free(db);
}

/* numPrepares assumes that db has exactly 1 idle conn and returns its count of
 * calls to Prepare. */
static Int num_prepares(TestingT *t, SqlDB *db) {
    Int n = burrow__sql_db_num_free(db);
    if (n != 1) {
        testing_t_errorf_v(t, "free conns = %d; want 1", n);
        return -1;
    }
    return fdb_get_fake_conn(burrow__sql_db_free_conn(db, 0))->num_prepare;
}

static bool wait_for_free(TestingT *t, SqlDB *db, Int want) {
    Int n = num_free_conns(db);
    if (n != want) {
        testing_t_errorf_v(t, "free conns after hitting EOF = %d; want %d", n, want);
        return false;
    }
    return true;
}

static bool wait_for_rows_close(TestingT *t, SqlRows *rows) {
    synctest_wait();
    if (!burrow__sql_rows_closed(rows)) {
        testing_t_errorf_v(t, "failed to close rows");
        return false;
    }
    return true;
}

typedef void (*DbTest)(TestingT *t, SqlDB *db);

typedef struct TdRun {
    TestingT *t;
    DbTest f;
    FakeKind kind;
} TdRun;

static void td_bubble(void *env) {
    TdRun *r = env;
    FakeConnector fc = {fake_db_name, NULL, false, r->kind};
    SqlDB *db = sql_open_db(fdb_connector(&fc));
    if (db == NULL) {
        testing_t_errorf_v(r->t, "OpenDB: out of memory");
        return;
    }
    if (EXEC(r->t, db, "WIPE"))
        r->f(r->t, db);
    close_db(r->t, db);
}

static void td_sub(void *env, TestingT *t) {
    TdRun *r = env;
    r->t = t;
    if (!synctest_run(BURROW_FN(Func, td_bubble, r)))
        testing_t_errorf_v(t, "synctest_run failed");
}

/* testDatabase. require names a feature the driver must have, or is NULL:
 * ConnBeginTx, NamedValue and Validator leave out basic, and ScanColumn
 * leaves out all but scancols. No connector lists BeginTx, so Go runs nothing
 * for it, and neither does this. */
static void test_database(TestingT *t, DbTest f, const char *require) {
    if (require != NULL && strcmp(require, "BeginTx") == 0)
        return;
    bool basic = require == NULL;
    bool deflt = require == NULL || strcmp(require, "ScanColumn") != 0;
    TdRun runs[3] = {
        {NULL, f, FAKE_BASIC}, {NULL, f, FAKE_DEFAULT}, {NULL, f, FAKE_SCANCOLS}};
    if (basic)
        (void)testing_t_run(t, BURROW_S("basic"),
                            BURROW_FN(TestingTFunc, td_sub, &runs[0]));
    if (deflt)
        (void)testing_t_run(t, BURROW_S("default"),
                            BURROW_FN(TestingTFunc, td_sub, &runs[1]));
    (void)testing_t_run(t, BURROW_S("scancols"),
                        BURROW_FN(TestingTFunc, td_sub, &runs[2]));
}

/* newTestDB, for the tests Go runs on the default connector alone. The
 * connector has to outlive the DB, so it comes from the caller. */
static SqlDB *new_test_db(TestingT *t, FakeConnector *fc, const char *name) {
    fc->name = fake_db_name;
    SqlDB *db = sql_open_db(fdb_connector(fc));
    if (db == NULL) {
        testing_t_errorf_v(t, "OpenDB: out of memory");
        return NULL;
    }
    if (!EXEC(t, db, "WIPE") || (name != NULL && !populate(t, db, name))) {
        close_db(t, db);
        return NULL;
    }
    return db;
}

/* A bubble for a test that runs on its own, like synctest.Test. */
typedef struct TdSolo {
    TestingT *t;
    void (*f)(TestingT *t);
} TdSolo;

static void td_solo_bubble(void *env) {
    TdSolo *s = env;
    s->f(s->t);
}

static void synctest_test(TestingT *t, void (*f)(TestingT *t)) {
    TdSolo s = {t, f};
    if (!synctest_run(BURROW_FN(Func, td_solo_bubble, &s)))
        testing_t_errorf_v(t, "synctest_run failed");
}

typedef struct AgeName {
    Int age;
    Str name;
} AgeName;

static void check_rows(TestingT *t, const AgeName *got, Int ngot, const AgeName *want,
                       Int nwant) {
    bool same = ngot == nwant;
    for (Int i = 0; same && i < ngot; i++)
        same = got[i].age == want[i].age && str_eq(got[i].name, want[i].name);
    if (same)
        return;
    testing_t_errorf_v(t, "mismatch: got %d rows, want %d", ngot, nwant);
    for (Int i = 0; i < ngot; i++)
        testing_t_logf_v(t, " got: {age: %d, name: %q}", got[i].age, got[i].name);
    for (Int i = 0; i < nwant; i++)
        testing_t_logf_v(t, "want: {age: %d, name: %q}", want[i].age, want[i].name);
}

static const AgeName people[] = {
    {1, BURROW_S_INIT("Alice")},
    {2, BURROW_S_INIT("Bob")},
    {3, BURROW_S_INIT("Chris")},
};

/* Scans every row of rows as an age and a name. */
static Int scan_age_names(TestingT *t, SqlRows *rows, Alloc *a, AgeName *got, Int max) {
    Int n = 0;
    while (sql_rows_next(rows)) {
        AgeName r = {0, BURROW_STR_EMPTY};
        Error err = sql_rows_scan_v(rows, a, &r.age, &r.name);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Scan: %v", err);
            break;
        }
        if (n < max)
            got[n] = r;
        n++;
    }
    return n < max ? n : max;
}

/* -------------------------------------------------------------- tests */

static void testOpenDB(TestingT *t) {
    FakeConnector fc = {fake_db_name, NULL, false, FAKE_DEFAULT};
    SqlDB *db = sql_open_db(fdb_connector(&fc));
    if (sql_db_driver(db).vt != fdriver.vt)
        testing_t_errorf_v(t, "OpenDB should return the driver of the Connector");
    (void)sql_db_close(db);
    sql_db_free(db);
}

static void TestOpenDB(TestingT *t) {
    synctest_test(t, testOpenDB);
}

static void testQuery(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int prepares0 = num_prepares(t, db);
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    AgeName got[4];
    Int n = scan_age_names(t, rows, arena_allocator(&ar), got, 4);
    err = sql_rows_err(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Err: %v", err);
    check_rows(t, got, n, people, 3);

    /* And verify that the final rows.Next() call, which hit EOF, also closed
     * the rows connection. */
    Int nf = num_free_conns(db);
    if (nf != 1)
        testing_t_errorf_v(t, "free conns after query hitting EOF = %d; want 1", nf);
    else if (num_prepares(t, db) - prepares0 != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1",
                           num_prepares(t, db) - prepares0);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestQuery(TestingT *t) {
    test_database(t, testQuery, NULL);
}

/* TestQueryContext tests canceling the context while scanning the rows. */
static void testQueryContext(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int prepares0 = num_prepares(t, db);

    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query_context(db, ctx, BURROW_S("SELECT|people|age,name|"),
                                         NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Query: %v", err);
        BURROW_CALLF0(cancel);
        context_release(ctx);
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    AgeName got[4];
    Int n = 0;
    Int index = 0;
    while (sql_rows_next(rows)) {
        if (index == 2) {
            BURROW_CALLF0(cancel);
            (void)wait_for_rows_close(t, rows);
        }
        AgeName r = {0, BURROW_STR_EMPTY};
        err = sql_rows_scan_v(rows, arena_allocator(&ar), &r.age, &r.name);
        if (BURROW_FAILED(err)) {
            if (index == 2)
                break;
            testing_t_errorf_v(t, "Scan: %v", err);
            break;
        }
        if (index == 2 && !errors_is(err, context_canceled))
            testing_t_errorf_v(t, "Scan: %v; want context.Canceled", err);
        if (n < 4)
            got[n++] = r;
        index++;
    }
    bool ok = false;
    if (chan_try_recv(context_done(ctx), NULL, &ok)) {
        Error cerr = context_err(ctx);
        if (!errors_is(cerr, context_canceled))
            testing_t_errorf_v(t, "context err = %v; want context.Canceled", cerr);
    } else {
        testing_t_errorf_v(t, "context err = nil; want context.Canceled");
    }
    check_rows(t, got, n, people, 2);

    /* And verify that the final rows.Next() call, which hit EOF, also closed
     * the rows connection. */
    if (wait_for_rows_close(t, rows) && wait_for_free(t, db, 1) &&
        num_prepares(t, db) - prepares0 != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1",
                           num_prepares(t, db) - prepares0);
    sql_rows_free(rows);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    arena_free(&ar);
}

static void TestQueryContext(TestingT *t) {
    test_database(t, testQueryContext, NULL);
}

/* The waiter TestQueryContextWait and testContextWait give the conn. */
static ContextCancelFunc td_wait_cancel;
static Context td_wait_ctx;

static void td_cancel_waiter(Context c) {
    (void)c;
    BURROW_CALLF0(td_wait_cancel);
    (void)chan_recv(context_done(td_wait_ctx), NULL);
}

/* TestQueryContextWait ensures that rows and all internal statements are
 * closed when a query context is closed during execution. */
static void testQueryContextWait(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int prepares0 = num_prepares(t, db);

    Context ctx =
        context_with_cancel(heap_allocator(), context_background(), &td_wait_cancel);
    td_wait_ctx = ctx;

    /* This will trigger the fake conn's prepare, which will take time
     * performing the query. ctxDriverPrepare will check the context after this
     * and close the rows and return an error. */
    Error err = BURROW_NO_ERROR;
    SqlConn *c = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Conn: %v", err);
    } else {
        fdb_get_fake_conn(burrow__sql_conn_driver(c))->waiter = td_cancel_waiter;
        SqlRows *rows = sql_conn_query_context(
            c, ctx, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
        if (rows != NULL)
            sql_rows_free(rows);
        (void)sql_conn_close(c);
        sql_conn_free(c);
        if (!errors_is(err, context_canceled))
            testing_t_errorf_v(t,
                               "expected QueryContext to error with context deadline "
                               "exceeded but returned %v",
                               err);
        /* Verify closed rows connection after error condition. */
        else if (wait_for_free(t, db, 1) && num_prepares(t, db) - prepares0 != 1)
            testing_t_errorf_v(t, "executed %d Prepare statements; want 1",
                               num_prepares(t, db) - prepares0);
    }
    BURROW_CALLF0(td_wait_cancel);
    context_release(ctx);
}

static void TestQueryContextWait(TestingT *t) {
    test_database(t, testQueryContextWait, NULL);
}

static void test_context_wait(TestingT *t, bool keep_conn_on_rollback, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Context ctx =
        context_with_cancel(heap_allocator(), context_background(), &td_wait_cancel);
    td_wait_ctx = ctx;

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(db, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "BeginTx: %v", err);
    } else {
        burrow__sql_tx_set_keep_conn_on_rollback(tx, keep_conn_on_rollback);
        fdb_get_fake_conn(burrow__sql_tx_driver(tx))->waiter = td_cancel_waiter;
        /* This will trigger the fake conn's prepare, which will take time
         * performing the query. ctxDriverPrepare will check the context after
         * this and close the rows and return an error. */
        SqlRows *rows = sql_tx_query_context(
            tx, ctx, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
        if (rows != NULL)
            sql_rows_free(rows);
        if (!errors_is(err, context_canceled))
            testing_t_errorf_v(t,
                               "expected QueryContext to error with context canceled "
                               "but returned %v",
                               err);
        else
            (void)wait_for_free(t, db, keep_conn_on_rollback ? 1 : 0);
        sql_tx_free(tx);
    }
    BURROW_CALLF0(td_wait_cancel);
    context_release(ctx);
}

static void testTxContextWait(TestingT *t, SqlDB *db) {
    test_context_wait(t, false, db);
}

/* TestTxContextWait tests the transaction behavior when the tx context is
 * canceled during execution of the query. */
static void TestTxContextWait(TestingT *t) {
    test_database(t, testTxContextWait, NULL);
}

static void testTxContextWaitNoDiscard(TestingT *t, SqlDB *db) {
    test_context_wait(t, true, db);
}

/* TestTxContextWaitNoDiscard is the same as TestTxContextWait, but should not
 * discard the final connection. */
static void TestTxContextWaitNoDiscard(TestingT *t) {
    test_database(t, testTxContextWaitNoDiscard, NULL);
}

/* TestUnsupportedOptions checks that the database fails when a driver that
 * doesn't implement ConnBeginTx is used with non-default options and an
 * un-cancellable context. */
static void testUnsupportedOptions(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTxOptions opts = {SQL_LEVEL_SERIALIZABLE, true};
    SqlTx *tx = sql_db_begin_tx(db, context_background(), &opts, &err);
    if (!BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "expected error when using unsupported options, got nil");
        sql_tx_free(tx);
    }
}

static void TestUnsupportedOptions(TestingT *t) {
    test_database(t, testUnsupportedOptions, NULL);
}

static void testMultiResultSetQuery(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int prepares0 = num_prepares(t, db);
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(
        db, BURROW_S("SELECT|people|age,name|;SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    AgeName got1[4];
    Int n1 = scan_age_names(t, rows, a, got1, 4);
    err = sql_rows_err(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Err: %v", err);
    check_rows(t, got1, n1, people, 3);

    if (!sql_rows_next_result_set(rows))
        testing_t_errorf_v(t, "expected another result set");

    AgeName got2[4];
    Int n2 = 0;
    while (sql_rows_next(rows)) {
        AgeName r = {0, BURROW_STR_EMPTY};
        err = sql_rows_scan_v(rows, a, &r.name);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Scan: %v", err);
            break;
        }
        if (n2 < 4)
            got2[n2++] = r;
    }
    err = sql_rows_err(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Err: %v", err);
    AgeName want2[3] = {{0, BURROW_S_INIT("Alice")},
                        {0, BURROW_S_INIT("Bob")},
                        {0, BURROW_S_INIT("Chris")}};
    check_rows(t, got2, n2, want2, 3);
    if (sql_rows_next_result_set(rows))
        testing_t_errorf_v(t, "expected no more result sets");

    /* And verify that the final rows.Next() call, which hit EOF, also closed
     * the rows connection. */
    if (wait_for_free(t, db, 1) && num_prepares(t, db) - prepares0 != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1",
                           num_prepares(t, db) - prepares0);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestMultiResultSetQuery(TestingT *t) {
    test_database(t, testMultiResultSetQuery, NULL);
}

static void testQueryNamedArg(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int prepares0 = num_prepares(t, db);
    Error err = BURROW_NO_ERROR;
    /* Ensure the name and age parameters only match on placeholder name, not
     * position. */
    SqlNamedArg age = sql_named(BURROW_S("age"), BURROW_ANY_OF((Int)2));
    SqlNamedArg name = sql_named(BURROW_S("name"), BURROW_ANY_OF(BURROW_S("Bob")));
    SqlRows *rows =
        sql_db_query_v(db, &err, BURROW_S("SELECT|people|age,name|name=?name,age=?age"),
                       sql_named_any(&age), sql_named_any(&name));
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    AgeName got[4];
    Int n = scan_age_names(t, rows, arena_allocator(&ar), got, 4);
    err = sql_rows_err(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Err: %v", err);
    check_rows(t, got, n, &people[1], 1);

    /* And verify that the final rows.Next() call, which hit EOF, also closed
     * the rows connection. */
    Int nf = num_free_conns(db);
    if (nf != 1)
        testing_t_errorf_v(t, "free conns after query hitting EOF = %d; want 1", nf);
    else if (num_prepares(t, db) - prepares0 != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1",
                           num_prepares(t, db) - prepares0);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestQueryNamedArg(TestingT *t) {
    test_database(t, testQueryNamedArg, "NamedValue");
}

static void testRowsColumns(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice cols = sql_rows_columns(rows, arena_allocator(&ar), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Columns: %v", err);
    } else {
        const Str *c = cols.p;
        if (cols.len != 2 || !str_eq(c[0], BURROW_S("age")) ||
            !str_eq(c[1], BURROW_S("name")))
            testing_t_errorf_v(t, "got %v; want [age name]", cols);
    }
    err = sql_rows_close(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error closing rows: %s", err);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestRowsColumns(TestingT *t) {
    test_database(t, testRowsColumns, NULL);
}

static void testRowsColumnTypes(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Slice tt = sql_rows_column_types(rows, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ColumnTypes: %v", err);
        sql_rows_free(rows);
        arena_free(&ar);
        return;
    }
    Any values[2] = {{NULL, NULL}, {NULL, NULL}};
    bool ok = tt.len == 2;
    for (Int i = 0; ok && i < tt.len; i++) {
        const SqlColumnType *tp = *(SqlColumnType *const *)slice_at(tt, i);
        const Type *st = sql_column_type_scan_type(tp);
        if (st == NULL) {
            testing_t_errorf_v(t, "scantype is null for column %q",
                               sql_column_type_name(tp));
            ok = false;
            break;
        }
        values[i] = (Any){st, mem_alloc(a, st->size, st->align)};
    }
    Int ct = 0;
    while (ok && sql_rows_next(rows)) {
        err = sql_rows_scan(rows, a, slice_from(values, 2, 2, TYPE_ANY));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed to scan values in %v", err);
            break;
        }
        if (ct == 1) {
            if (values[0].t != TYPE_INT32 || *(int32_t *)values[0].data != 2)
                testing_t_errorf_v(t, "Expected 2, got %v", values[0]);
            if (values[1].t != TYPE_STRING ||
                !str_eq(*(Str *)values[1].data, BURROW_S("Bob")))
                testing_t_errorf_v(t, "Expected Bob, got %v", values[1]);
        }
        ct++;
    }
    if (ok && ct != 3)
        testing_t_errorf_v(t, "expected 3 rows, got %d", ct);
    err = sql_rows_close(rows);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error closing rows: %s", err);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestRowsColumnTypes(TestingT *t) {
    test_database(t, testRowsColumnTypes, NULL);
}

static void testQueryRow(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str name = BURROW_STR_EMPTY;
    Int age = 0;
    Time birthday = {0};

    SqlRow row = sql_db_query_row_v(db, BURROW_S("SELECT|people|age,name|age=?"), 3);
    Error err = sql_row_scan_v(&row, a, &age);
    if (!BURROW_FAILED(err) ||
        !strings_contains(error_text(err),
                          BURROW_S("expected 2 destination arguments")))
        testing_t_errorf_v(t,
                           "expected error from wrong number of arguments; actually "
                           "got: %v",
                           err);

    row = sql_db_query_row_v(db, BURROW_S("SELECT|people|bdate|age=?"), 3);
    err = sql_row_scan_v(&row, a, ((Any){TYPE_TIME, &birthday}));
    Time want_bday = chris_birthday();
    if (BURROW_FAILED(err) || !time_equal(birthday, want_bday))
        testing_t_errorf_v(t, "chris birthday = %v, err = %v; want %v",
                           ((Any){TYPE_TIME, &birthday}), err,
                           ((Any){TYPE_TIME, &want_bday}));

    row = sql_db_query_row_v(db, BURROW_S("SELECT|people|age,name|age=?"), 2);
    err = sql_row_scan_v(&row, a, &age, &name);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "age QueryRow+Scan: %v", err);
    } else {
        if (!str_eq(name, BURROW_S("Bob")))
            testing_t_errorf_v(t, "expected name Bob, got %q", name);
        if (age != 2)
            testing_t_errorf_v(t, "expected age 2, got %d", age);
    }

    row = sql_db_query_row_v(db, BURROW_S("SELECT|people|age,name|name=?"),
                             BURROW_S("Alice"));
    err = sql_row_scan_v(&row, a, &age, &name);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "name QueryRow+Scan: %v", err);
    } else {
        if (!str_eq(name, BURROW_S("Alice")))
            testing_t_errorf_v(t, "expected name Alice, got %q", name);
        if (age != 1)
            testing_t_errorf_v(t, "expected age 1, got %d", age);
    }

    Bytes photo = slice_nil(TYPE_BYTE);
    row = sql_db_query_row_v(db, BURROW_S("SELECT|people|photo|name=?"),
                             BURROW_S("Alice"));
    err = sql_row_scan_v(&row, a, &photo);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "photo QueryRow+Scan: %v", err);
    else if (!str_eq((Str){photo.p, photo.len}, BURROW_S("APHOTO")))
        testing_t_errorf_v(t, "photo = %q; want %q", photo, "APHOTO");
    arena_free(&ar);
}

static void TestQueryRow(TestingT *t) {
    test_database(t, testQueryRow, NULL);
}

static void testRowErr(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    SqlRow row = sql_db_query_row_context(
        db, context_background(), BURROW_S("SELECT|people|bdate|age=?"), ARGS(3));
    Error err = sql_row_err(&row);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unexpected err = %v; want %v", err, BURROW_NO_ERROR);
    (void)sql_row_scan(&row, heap_allocator(), slice_nil(TYPE_ANY));

    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);
    BURROW_CALLF0(cancel);

    row = sql_db_query_row_context(db, ctx, BURROW_S("SELECT|people|bdate|age=?"),
                                   ARGS(3));
    err = sql_row_err(&row);
    if (!BURROW_FAILED(err) ||
        !strings_contains(error_text(err), BURROW_S("context canceled")))
        testing_t_errorf_v(t, "Expected err = %v; got %v", "context canceled", err);
    if (row.rows != NULL)
        sql_rows_free(row.rows);
    context_release(ctx);
}

static void TestRowErr(TestingT *t) {
    test_database(t, testRowErr, NULL);
}

static void testTxRollbackCommitErr(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    err = sql_tx_rollback(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected nil error from Rollback; got %v", err);
    err = sql_tx_commit(tx);
    if (!errors_is(err, sql_err_tx_done))
        testing_t_errorf_v(t, "expected %q from Commit; got %q", sql_err_tx_done, err);
    sql_tx_free(tx);

    tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    err = sql_tx_commit(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected nil error from Commit; got %v", err);
    err = sql_tx_rollback(tx);
    if (!errors_is(err, sql_err_tx_done))
        testing_t_errorf_v(t, "expected %q from Rollback; got %q", sql_err_tx_done,
                           err);
    sql_tx_free(tx);
}

static void TestTxRollbackCommitErr(TestingT *t) {
    test_database(t, testTxRollbackCommitErr, NULL);
}

static void testStatementErrorAfterClose(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT|people|age|name=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Prepare: %v", err);
    err = sql_stmt_close(stmt);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Close: %v", err);
    } else {
        Str name = BURROW_STR_EMPTY;
        SqlRow row = sql_stmt_query_row_v(stmt, BURROW_S("foo"));
        err = sql_row_scan_v(&row, heap_allocator(), &name);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error from QueryRow.Scan after Stmt.Close");
    }
    sql_stmt_free(stmt);
}

static void TestStatementErrorAfterClose(TestingT *t) {
    test_database(t, testStatementErrorAfterClose, NULL);
}

static void testStatementQueryRow(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT|people|age|name=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Prepare: %v", err);
    Int age = 0;
    for (Int n = 0; n < 3; n++) {
        SqlRow row = sql_stmt_query_row_v(stmt, people[n].name);
        err = sql_row_scan_v(&row, heap_allocator(), &age);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d: on %q, QueryRow/Scan: %v", n, people[n].name,
                               err);
        else if (age != people[n].age)
            testing_t_errorf_v(t, "%d: age=%d, want %d", n, age, people[n].age);
    }
    sql_stmt_free(stmt);
}

static void TestStatementQueryRow(TestingT *t) {
    test_database(t, testStatementQueryRow, NULL);
}

enum { TD_CONCURRENT = 10 };

typedef struct TdConcurrent {
    SqlStmt *stmt;
    Chan *ch;
    SyncMutex mu;
    Int next;
    char msg[TD_CONCURRENT][128];
} TdConcurrent;

/* Each goroutine leaves what went wrong in a slot of its own and sends the
 * slot's index, so the message outlives the goroutine's error arena. */
static void td_query_alice(void *env) {
    TdConcurrent *c = env;
    sync_mutex_lock(&c->mu);
    Int slot = c->next++;
    sync_mutex_unlock(&c->mu);
    c->msg[slot][0] = 0;
    Int age = 0;
    SqlRow row = sql_stmt_query_row_v(c->stmt, BURROW_S("Alice"));
    Error err = sql_row_scan_v(&row, heap_allocator(), &age);
    Str text = BURROW_STR_EMPTY;
    if (BURROW_FAILED(err))
        text = error_text(err);
    else if (age != 1)
        text = BURROW_S("unexpected age");
    size_t n = (size_t)text.len < sizeof c->msg[0] - 1 ? (size_t)text.len
                                                       : sizeof c->msg[0] - 1;
    if (n > 0)
        memcpy(c->msg[slot], text.p, n);
    c->msg[slot][n] = 0;
    chan_send(c->ch, &slot);
}

/* golang.org/issue/3734 */
static void testStatementQueryRowConcurrent(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT|people|age|name=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Prepare: %v", err);
    static TdConcurrent c;
    memset(&c, 0, sizeof c);
    c.stmt = stmt;
    c.ch = chan_make(heap_allocator(), TYPE_INT, TD_CONCURRENT);
    for (int i = 0; i < TD_CONCURRENT; i++)
        (void)go(BURROW_FN(Func, td_query_alice, &c));
    for (int i = 0; i < TD_CONCURRENT; i++) {
        Int slot = 0;
        (void)chan_recv(c.ch, &slot);
        if (c.msg[slot][0] != 0)
            testing_t_errorf_v(t, "%s", c.msg[slot]);
    }
    chan_free(c.ch);
    sql_stmt_free(stmt);
}

static void TestStatementQueryRowConcurrent(TestingT *t) {
    test_database(t, testStatementQueryRowConcurrent, NULL);
}

/* just a test of fakedb itself */
static void testBogusPreboundParameters(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt =
        sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=bogusconversion"), &err);
    if (!BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "expected error");
        sql_stmt_free(stmt);
        return;
    }
    if (!str_eq(
            error_text(err),
            BURROW_S("fakedb: invalid conversion to int32 from \"bogusconversion\"")))
        testing_t_errorf_v(t, "unexpected error: %v", err);
}

static void TestBogusPreboundParameters(TestingT *t) {
    test_database(t, testBogusPreboundParameters, NULL);
}

static void testExec(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Slice args;
        const char *want_err;
    } tests[] = {
        /* Okay: */
        {ARGS(BURROW_S("Brad"), 31), ""},
        {ARGS(BURROW_S("Brad"), (int64_t)31), ""},
        {ARGS(BURROW_S("Bob"), BURROW_S("32")), ""},
        {ARGS(7, 9), ""},

        /* Invalid conversions: */
        {ARGS(BURROW_S("Brad"), (int64_t)0xFFFFFFFF),
         "sql: converting argument $2 type: sql/driver: value 4294967295 overflows "
         "int32"},
        {ARGS(BURROW_S("Brad"), BURROW_S("strconv fail")),
         "sql: converting argument $2 type: sql/driver: value \"strconv fail\" can't "
         "be converted to int32"},

        /* Wrong number of args: */
        {NO_ARGS, "sql: expected 2 arguments, got 0"},
        {ARGS(1, 2, 3), "sql: expected 2 arguments, got 3"},
    };
    for (size_t n = 0; n < sizeof tests / sizeof tests[0]; n++) {
        (void)sql_stmt_exec(stmt, a, tests[n].args, &err);
        Str got = BURROW_FAILED(err) ? error_text(err) : BURROW_STR_EMPTY;
        if (!str_eq(got, str_from_cstr(tests[n].want_err)))
            testing_t_errorf_v(t,
                               "stmt.Execute #%d: for %v, got error %q, want error %q",
                               (Int)n, tests[n].args, got, tests[n].want_err);
    }
    sql_stmt_free(stmt);
    arena_free(&ar);
}

static void TestExec(TestingT *t) {
    test_database(t, testExec, NULL);
}

static void testTxPrepare(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("Begin = %v", err);
    SqlStmt *stmt = sql_tx_prepare(tx, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Stmt, err = %p, %v", (void *)stmt, err);
        sql_tx_free(tx);
        return;
    }
    (void)sql_stmt_exec_v(stmt, heap_allocator(), &err, BURROW_S("Bobby"), 7);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Exec = %v", err);
    err = sql_tx_commit(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Commit = %v", err);
    /* Commit() should have closed the statement */
    else if (!burrow__sql_stmt_closed(stmt))
        testing_t_errorf_v(t, "Stmt not closed after Commit");
    sql_stmt_free(stmt);
    sql_tx_free(tx);
}

static void TestTxPrepare(TestingT *t) {
    test_database(t, testTxPrepare, NULL);
}

static void testTxStmt(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
        sql_stmt_free(stmt);
        return;
    }
    SqlStmt *txs = sql_tx_stmt(tx, stmt);
    (void)sql_stmt_exec_v(txs, heap_allocator(), &err, BURROW_S("Bobby"), 7);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Exec = %v", err);
    err = sql_tx_commit(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Commit = %v", err);
    /* Commit() should have closed the statement */
    else if (!burrow__sql_stmt_closed(txs))
        testing_t_errorf_v(t, "Stmt not closed after Commit");
    sql_stmt_free(txs);
    sql_tx_free(tx);
    sql_stmt_free(stmt);
}

static void TestTxStmt(TestingT *t) {
    test_database(t, testTxStmt, NULL);
}

static void testTxStmtPreparedOnce(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32"))
        return;
    Int prepares0 = num_prepares(t, db);

    /* db.Prepare increments numPrepares. */
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
        sql_stmt_free(stmt);
        return;
    }
    SqlStmt *txs1 = sql_tx_stmt(tx, stmt);
    SqlStmt *txs2 = sql_tx_stmt(tx, stmt);

    (void)sql_stmt_exec_v(txs1, heap_allocator(), &err, BURROW_S("Go"), 7);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Exec = %v", err);
    sql_stmt_free(txs1);

    (void)sql_stmt_exec_v(txs2, heap_allocator(), &err, BURROW_S("Gopher"), 8);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Exec = %v", err);
    sql_stmt_free(txs2);

    err = sql_tx_commit(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Commit = %v", err);
    sql_tx_free(tx);

    Int prepares = num_prepares(t, db) - prepares0;
    if (prepares != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1", prepares);
    sql_stmt_free(stmt);
}

static void TestTxStmtPreparedOnce(TestingT *t) {
    test_database(t, testTxStmtPreparedOnce, NULL);
}

static void testTxStmtClosedRePrepares(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32"))
        return;
    Int prepares0 = num_prepares(t, db);

    /* db.Prepare increments numPrepares. */
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
        sql_stmt_free(stmt);
        return;
    }
    err = sql_stmt_close(stmt);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "stmt.Close() = %v", err);
    /* tx.Stmt increments numPrepares because stmt is closed. */
    SqlStmt *txs = sql_tx_stmt(tx, stmt);
    if (BURROW_FAILED(burrow__sql_stmt_sticky_err(txs)))
        testing_t_errorf_v(t, "%v", burrow__sql_stmt_sticky_err(txs));
    if (burrow__sql_stmt_parent(txs) != NULL)
        testing_t_errorf_v(t, "expected nil parentStmt");
    (void)sql_stmt_exec_v(txs, heap_allocator(), &err, BURROW_S("Eric"), 82);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs.Exec = %v", err);
    err = sql_stmt_close(txs);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs.Close = %v", err);
    sql_stmt_free(txs);

    (void)sql_tx_rollback(tx);
    sql_tx_free(tx);

    Int prepares = num_prepares(t, db) - prepares0;
    if (prepares != 2)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 2", prepares);
    sql_stmt_free(stmt);
}

static void TestTxStmtClosedRePrepares(TestingT *t) {
    test_database(t, testTxStmtClosedRePrepares, NULL);
}

static void testParentStmtOutlivesTxStmt(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32"))
        return;
    /* Make sure everything happens on the same connection. */
    sql_db_set_max_open_conns(db, 1);
    Int prepares0 = num_prepares(t, db);

    /* db.Prepare increments numPrepares. */
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
        sql_stmt_free(stmt);
        return;
    }
    SqlStmt *txs = sql_tx_stmt(tx, stmt);
    Int ncss = burrow__sql_stmt_num_css(stmt);
    if (ncss != 1)
        testing_t_errorf_v(t, "len(stmt.css) = %v; want 1", ncss);
    err = sql_stmt_close(txs);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs.Close() = %v", err);
    err = sql_tx_rollback(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "tx.Rollback() = %v", err);
    /* txs must not be valid. */
    (void)sql_stmt_exec_v(txs, heap_allocator(), &err, BURROW_S("Suzan"), 30);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs.Exec(), expected err");
    /* Stmt must still be valid. */
    (void)sql_stmt_exec_v(stmt, heap_allocator(), &err, BURROW_S("Janina"), 25);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "stmt.Exec() = %v", err);

    Int prepares = num_prepares(t, db) - prepares0;
    if (prepares != 1)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 1", prepares);
    sql_stmt_free(txs);
    sql_tx_free(tx);
    sql_stmt_free(stmt);
}

static void TestParentStmtOutlivesTxStmt(TestingT *t) {
    test_database(t, testParentStmtOutlivesTxStmt, NULL);
}

static void testTxStmtFromTxStmtRePrepares(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32"))
        return;
    Int prepares0 = num_prepares(t, db);
    /* db.Prepare increments numPrepares. */
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("Stmt, err = %p, %v", (void *)stmt, err);
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
        sql_stmt_free(stmt);
        return;
    }
    SqlStmt *txs1 = sql_tx_stmt(tx, stmt);

    /* tx.Stmt(txs1) increments numPrepares because txs1 already belongs to a
     * transaction (albeit the same transaction). */
    SqlStmt *txs2 = sql_tx_stmt(tx, txs1);
    if (BURROW_FAILED(burrow__sql_stmt_sticky_err(txs2)))
        testing_t_errorf_v(t, "%v", burrow__sql_stmt_sticky_err(txs2));
    if (burrow__sql_stmt_parent(txs2) != NULL)
        testing_t_errorf_v(t, "expected nil parentStmt");
    (void)sql_stmt_exec_v(txs2, heap_allocator(), &err, BURROW_S("Eric"), 82);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);

    err = sql_stmt_close(txs1);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs1.Close = %v", err);
    err = sql_stmt_close(txs2);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "txs1.Close = %v", err);
    err = sql_tx_rollback(tx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "tx.Rollback = %v", err);
    sql_stmt_free(txs1);
    sql_stmt_free(txs2);
    sql_tx_free(tx);

    Int prepares = num_prepares(t, db) - prepares0;
    if (prepares != 2)
        testing_t_errorf_v(t, "executed %d Prepare statements; want 2", prepares);
    sql_stmt_free(stmt);
}

static void TestTxStmtFromTxStmtRePrepares(TestingT *t) {
    test_database(t, testTxStmtFromTxStmtRePrepares, NULL);
}

/* Issue: https://golang.org/issue/2784
 * This test didn't fail before because we got lucky with the fakedb driver. It
 * was failing, and now not, in github.com/bradfitz/go-sql-test */
static void testTxQuery(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool") ||
        !EXEC(t, db, "INSERT|t1|name=Alice"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    SqlRows *r = sql_tx_query(tx, BURROW_S("SELECT|t1|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else if (!sql_rows_next(r)) {
        err = sql_rows_err(r);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else
            testing_t_errorf_v(t, "expected one row");
    } else {
        Str x = BURROW_STR_EMPTY;
        err = sql_rows_scan_v(r, heap_allocator(), &x);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else
            mem_free(heap_allocator(), (void *)(uintptr_t)x.p, (size_t)x.len, 1);
    }
    if (r != NULL)
        sql_rows_free(r);
    (void)sql_tx_rollback(tx);
    sql_tx_free(tx);
}

static void TestTxQuery(TestingT *t) {
    test_database(t, testTxQuery, NULL);
}

static void testTxQueryInvalid(TestingT *t, SqlDB *db) {
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    SqlRows *r = sql_tx_query(tx, BURROW_S("SELECT|t1|name|"), NO_ARGS, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error expected");
    if (r != NULL)
        sql_rows_free(r);
    (void)sql_tx_rollback(tx);
    sql_tx_free(tx);
}

static void TestTxQueryInvalid(TestingT *t) {
    test_database(t, testTxQueryInvalid, NULL);
}

/* Tests fix for issue 4433, that retries in Begin happen when conn.Begin()
 * returns ErrBadConn */
static void testTxErrBadConn(TestingT *t) {
    register_test_driver();
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("test"), BURROW_S("foo;badConn"), &err);
    if (BURROW_FAILED(err))
        FATALF("Open: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    (void)sql_db_exec(db, arena_allocator(&ar), BURROW_S("WIPE"), NO_ARGS, &err);
    arena_free(&ar);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec wipe: %v", err);
        close_db(t, db);
        return;
    }
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool")) {
        close_db(t, db);
        return;
    }
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Stmt, err = %p, %v", (void *)stmt, err);
        close_db(t, db);
        return;
    }
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Begin = %v", err);
    } else {
        SqlStmt *txs = sql_tx_stmt(tx, stmt);
        (void)sql_stmt_exec_v(txs, heap_allocator(), &err, BURROW_S("Bobby"), 7);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Exec = %v", err);
        else if (BURROW_FAILED(err = sql_tx_commit(tx)))
            testing_t_errorf_v(t, "Commit = %v", err);
        sql_stmt_free(txs);
        sql_tx_free(tx);
    }
    sql_stmt_free(stmt);
    close_db(t, db);
}

static void TestTxErrBadConn(TestingT *t) {
    synctest_test(t, testTxErrBadConn);
}

static void testConnQuery(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Context ctx = testing_t_context(t);
    Error err = BURROW_NO_ERROR;
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;

    Str name = BURROW_STR_EMPTY;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    SqlRow row = sql_conn_query_row_context(
        conn, ctx, BURROW_S("SELECT|people|name|age=?"), ARGS(3));
    err = sql_row_scan_v(&row, arena_allocator(&ar), &name);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!str_eq(name, BURROW_S("Chris")))
        testing_t_errorf_v(t, "unexpected result, got %q want Chris", name);
    else if (BURROW_FAILED(err = sql_conn_ping_context(conn, ctx)))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
    (void)sql_conn_close(conn);
    sql_conn_free(conn);
}

static void TestConnQuery(TestingT *t) {
    test_database(t, testConnQuery, NULL);
}

static Error td_raw_saw(void *env, DriverConn dc) {
    *(bool *)env = true;
    (void)fdb_get_fake_conn(dc);
    return BURROW_NO_ERROR;
}

static Error td_raw_panic(void *env, DriverConn dc) {
    (void)env;
    (void)dc;
    panic_str(BURROW_S("Conn.Raw panic should return an error"));
    return BURROW_NO_ERROR;
}

static void testConnRaw(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Context ctx = testing_t_context(t);
    Error err = BURROW_NO_ERROR;
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;

    bool saw_func = false;
    err = sql_conn_raw(conn, BURROW_FN(SqlRawFunc, td_raw_saw, &saw_func));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!saw_func)
        testing_t_errorf_v(t, "Raw func not called");

    volatile bool recovered = false;
    BURROW_TRY {
        (void)sql_conn_raw(conn, BURROW_FN(SqlRawFunc, td_raw_panic, NULL));
    }
    BURROW_CATCH(x) {
        (void)x;
        recovered = true;
    }
    BURROW_TRY_END;
    if (!recovered)
        testing_t_errorf_v(t, "expected panic");
    else if (burrow__sql_conn_driver(conn).vt != NULL)
        testing_t_errorf_v(t, "expected connection to be closed after panic");
    (void)sql_conn_close(conn);
    sql_conn_free(conn);
}

static void TestConnRaw(TestingT *t) {
    test_database(t, testConnRaw, NULL);
}

#define ROWS_DEST(p) ((Any){&burrow_type_SqlRowsPtr, (p)})

static void testCursorFake(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       30 * TIME_SECOND, &cancel);
    SqlRows *rows = NULL;
    SqlRowsPtr cursor = NULL;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    if (!EXEC(t, db, "CREATE|peoplecursor|list=table") ||
        !EXEC(t, db, "INSERT|peoplecursor|list=people!name!age"))
        goto out;
    rows = sql_db_query_context(db, ctx, BURROW_S("SELECT|peoplecursor|list|"), NO_ARGS,
                                &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }
    if (!sql_rows_next(rows)) {
        testing_t_errorf_v(t, "no rows");
        goto out;
    }
    err = sql_rows_scan_v(rows, a, ROWS_DEST(&cursor));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }

    enum { EXPECTED_ROWS = 3 };
    int64_t current_row = 0;
    int64_t n = 0;
    Str str = BURROW_STR_EMPTY;
    while (sql_rows_next(cursor)) {
        current_row++;
        err = sql_rows_scan_v(cursor, a, &str, &n);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            goto out;
        }
        if (n != current_row)
            testing_t_errorf_v(t, "expected number(Age)=%d, got %d", current_row, n);
    }
    if (current_row != EXPECTED_ROWS)
        testing_t_errorf_v(t, "expected %d rows, got %d rows", (int64_t)EXPECTED_ROWS,
                           current_row);
out:
    if (cursor != NULL)
        sql_rows_free(cursor);
    if (rows != NULL)
        sql_rows_free(rows);
    arena_free(&ar);
    BURROW_CALLF0(cancel);
    context_release(ctx);
}

static void TestCursorFake(TestingT *t) {
    test_database(t, testCursorFake, NULL);
}

static void testCursorDoubleRowsPointer(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|table1|col=string") ||
        !EXEC(t, db, "INSERT|table1|col=value") ||
        !EXEC(t, db, "CREATE|cursor|list=table") ||
        !EXEC(t, db, "INSERT|cursor|list=table1!col"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query_context(
        db, testing_t_context(t), BURROW_S("SELECT|cursor|list|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlRowsPtr cursor = NULL;
    if (!sql_rows_next(rows)) {
        testing_t_errorf_v(t, "no rows");
    } else if (BURROW_FAILED(err = sql_rows_scan_v(rows, a, ROWS_DEST(&cursor)))) {
        testing_t_errorf_v(t, "%v", err);
    } else if (!sql_rows_next(cursor)) {
        testing_t_errorf_v(t, "no child rows");
    } else {
        Str col = BURROW_STR_EMPTY;
        err = sql_rows_scan_v(cursor, a, &col);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else if (!str_eq(col, BURROW_S("value")))
            testing_t_errorf_v(t, "read col=%q, want %q", col, "value");
    }
    if (cursor != NULL)
        sql_rows_free(cursor);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestCursorDoubleRowsPointer(TestingT *t) {
    test_database(t, testCursorDoubleRowsPointer, NULL);
}

static void testCursorNull(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|cursor|list=nulltable") ||
        !EXECV(t, db, "INSERT|cursor|list=?", ((Any){NULL, NULL})))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query_context(
        db, testing_t_context(t), BURROW_S("SELECT|cursor|list|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    SqlRowsPtr cursor = NULL;
    if (!sql_rows_next(rows)) {
        testing_t_errorf_v(t, "no rows");
    } else if (BURROW_FAILED(
                   err = sql_rows_scan_v(rows, heap_allocator(), ROWS_DEST(&cursor)))) {
        testing_t_errorf_v(t, "%v", err);
    } else if (cursor != NULL) {
        testing_t_errorf_v(t, "Scan returned cursor, expected nil");
        sql_rows_free(cursor);
    }
    sql_rows_free(rows);
}

static void TestCursorNull(TestingT *t) {
    test_database(t, testCursorNull, NULL);
}

typedef struct CursorCancelCase {
    const char *name;
    const char *cancel_on;
    const char *want[7];
} CursorCancelCase;

static const CursorCancelCase *td_cursor_case;

static void testCursorCancel(TestingT *t, SqlDB *db) {
    const CursorCancelCase *tc = td_cursor_case;
    if (!EXEC(t, db, "CREATE|table1|col=string") ||
        !EXEC(t, db, "INSERT|table1|col=1.1") ||
        !EXEC(t, db, "INSERT|table1|col=1.2") ||
        !EXEC(t, db, "CREATE|table2|col=string") ||
        !EXEC(t, db, "INSERT|table2|col=2.1") ||
        !EXEC(t, db, "INSERT|table2|col=2.2") ||
        !EXEC(t, db, "CREATE|cursor|name=string,list=table") ||
        !EXEC(t, db, "INSERT|cursor|name=table1,list=table1!col") ||
        !EXEC(t, db, "INSERT|cursor|name=table2,list=table2!col"))
        return;
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       30 * TIME_SECOND, &cancel);
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query_context(db, ctx, BURROW_S("SELECT|cursor|name,list|"),
                                         NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        BURROW_CALLF0(cancel);
        context_release(ctx);
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str got[16];
    Int ngot = 0;
    Str cancel_on = str_from_cstr(tc->cancel_on);
    bool failed = false;
    while (!failed && sql_rows_next(rows)) {
        Str name = BURROW_STR_EMPTY;
        SqlRowsPtr cursor = NULL;
        err = sql_rows_scan_v(rows, a, &name, ROWS_DEST(&cursor));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            break;
        }
        if (ngot < 16)
            got[ngot++] = name;
        if (str_eq(name, cancel_on))
            (void)sql_rows_close(rows);
        while (sql_rows_next(cursor)) {
            Str col = BURROW_STR_EMPTY;
            err = sql_rows_scan_v(cursor, a, &col);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%v", err);
                failed = true;
                break;
            }
            if (ngot < 16)
                got[ngot++] = col;
            if (str_eq(col, cancel_on))
                (void)sql_rows_close(cursor);
        }
        sql_rows_free(cursor);
    }

    Int nwant = 0;
    while (nwant < 7 && tc->want[nwant] != NULL)
        nwant++;
    bool same = ngot == nwant;
    for (Int i = 0; same && i < ngot; i++)
        same = str_eq(got[i], str_from_cstr(tc->want[i]));
    if (!same) {
        Slice gs = slice_from(got, ngot, ngot, TYPE_STRING);
        Str w[7];
        for (Int i = 0; i < nwant; i++)
            w[i] = str_from_cstr(tc->want[i]);
        testing_t_errorf_v(t, "cancel after reading %q:\ngot:  %v\nwant: %v", cancel_on,
                           gs, slice_from(w, nwant, nwant, TYPE_STRING));
    }
    sql_rows_free(rows);
    arena_free(&ar);
    BURROW_CALLF0(cancel);
    context_release(ctx);
}

static void td_cursor_cancel_sub(void *env, TestingT *t) {
    td_cursor_case = env;
    test_database(t, testCursorCancel, NULL);
}

/* TestCursorCancel exercises calling Rows.Close at various places, including
 * canceling a cursor (child Rows). */
static void TestCursorCancel(TestingT *t) {
    static const CursorCancelCase tests[] = {
        /* don't cancel */
        {"no cancel", "", {"table1", "1.1", "1.2", "table2", "2.1", "2.2", NULL}},
        {"outer cancel", "table2", {"table1", "1.1", "1.2", "table2", NULL}},
        {"inner cancel", "1.1", {"table1", "1.1", "table2", "2.1", "2.2", NULL}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        (void)testing_t_run(t, str_from_cstr(tests[i].name),
                            BURROW_FN(TestingTFunc, td_cursor_cancel_sub,
                                      (void *)(uintptr_t)&tests[i]));
}

typedef struct InvalidNilCase {
    const char *name;
    Any input;
    const char *expected_error;
} InvalidNilCase;

static const InvalidNilCase *td_invalid_nil;

static void testInvalidNilValues(TestingT *t) {
    const InvalidNilCase *tt = td_invalid_nil;
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;
    Context ctx = testing_t_context(t);
    Error err = BURROW_NO_ERROR;
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        close_db(t, db);
        return;
    }
    fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;

    Any input = tt->input;
    SqlRow row = sql_conn_query_row_context(
        conn, ctx, BURROW_S("SELECT|people|bdate|age=?"), ARGS(1));
    err = sql_row_scan(&row, heap_allocator(), slice_from(&input, 1, 1, TYPE_ANY));
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error when querying nil column, but succeeded");
    else if (!str_eq(error_text(err), str_from_cstr(tt->expected_error)))
        testing_t_errorf_v(t, "Expected error: %s\nReceived: %s", tt->expected_error,
                           error_text(err));
    else if (BURROW_FAILED(err = sql_conn_ping_context(conn, ctx)))
        testing_t_errorf_v(t, "%v", err);
    (void)sql_conn_close(conn);
    sql_conn_free(conn);
    close_db(t, db);
}

static void td_invalid_nil_sub(void *env, TestingT *t) {
    td_invalid_nil = env;
    synctest_test(t, testInvalidNilValues);
}

/* Go's second case scans into an int. Int is int64_t here, or int32_t on a
 * 32-bit system, so this one scans into an int64_t and expects the message Go
 * gives for that. */
static void TestInvalidNilValues(TestingT *t) {
    static Time date1;
    static int64_t date2;
    const InvalidNilCase tests[] = {
        {"time.Time",
         {TYPE_TIME, &date1},
         "sql: Scan error on column index 0, name \"bdate\": unsupported Scan, storing "
         "driver.Value type <nil> into type *time.Time"},
        {"int",
         {TYPE_INT64, &date2},
         "sql: Scan error on column index 0, name \"bdate\": converting NULL to int64 "
         "is "
         "unsupported"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        (void)testing_t_run(
            t, str_from_cstr(tests[i].name),
            BURROW_FN(TestingTFunc, td_invalid_nil_sub, (void *)(uintptr_t)&tests[i]));
}

static void testConnTx(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Context ctx = testing_t_context(t);
    Error err = BURROW_NO_ERROR;
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    SqlTx *tx = sql_conn_begin_tx(conn, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }
    Str insert_name = BURROW_S("Nancy");
    Int insert_age = 33;
    (void)sql_tx_exec_context(tx, a, ctx,
                              BURROW_S("INSERT|people|name=?,age=?,photo=APHOTO"),
                              ARGS(insert_name, insert_age), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        (void)sql_tx_rollback(tx);
        sql_tx_free(tx);
        goto out;
    }
    err = sql_tx_commit(tx);
    sql_tx_free(tx);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }

    Str select_name = BURROW_STR_EMPTY;
    SqlRow row = sql_conn_query_row_context(
        conn, ctx, BURROW_S("SELECT|people|name|age=?"), ARGS(insert_age));
    err = sql_row_scan_v(&row, a, &select_name);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!str_eq(select_name, insert_name))
        testing_t_errorf_v(t, "got %q want %q", select_name, insert_name);
out:
    arena_free(&ar);
    (void)sql_conn_close(conn);
    sql_conn_free(conn);
}

static void TestConnTx(TestingT *t) {
    test_database(t, testConnTx, NULL);
}

static Error td_raw_sticky_bad(void *env, DriverConn raw) {
    (void)env;
    fdb_get_fake_conn(raw)->sticky_bad = true;
    return BURROW_NO_ERROR;
}

/* TestConnIsValid verifies that a database connection that should be
 * discarded, is actually discarded and does not re-enter the connection pool.
 * If the IsValid method from *fakeConn is removed, this test will fail. */
static void testConnIsValid(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    sql_db_set_max_open_conns(db, 1);
    Context ctx = context_background();
    Error err = BURROW_NO_ERROR;
    SqlConn *c = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    err = sql_conn_raw(c, BURROW_FN(SqlRawFunc, td_raw_sticky_bad, NULL));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    (void)sql_conn_close(c);
    sql_conn_free(c);

    if (burrow__sql_db_num_free(db) > 0 &&
        fdb_get_fake_conn(burrow__sql_db_free_conn(db, 0))->sticky_bad)
        testing_t_errorf_v(t,
                           "bad connection returned to pool; expected bad connection "
                           "to be discarded");
}

static void TestConnIsValid(TestingT *t) {
    test_database(t, testConnIsValid, "Validator");
}

/* Tests fix for issue 2542, that we release a lock when querying on a closed
 * connection. */
static void testIssue2542Deadlock(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = sql_db_close(db);
    if (BURROW_FAILED(err))
        FATALF("error closing DB: %v", err);
    for (int i = 0; i < 2; i++) {
        SqlRows *r =
            sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error");
        if (r != NULL)
            sql_rows_free(r);
    }
}

static void TestIssue2542Deadlock(TestingT *t) {
    test_database(t, testIssue2542Deadlock, NULL);
}

/* From golang.org/issue/3865 */
static void testCloseStmtBeforeRows(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlStmt *s = sql_db_prepare(db, BURROW_S("SELECT|people|name|"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    SqlRows *r = sql_stmt_query(s, NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        sql_stmt_free(s);
        return;
    }
    err = sql_stmt_close(s);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    sql_rows_free(r);
    sql_stmt_free(s);
}

static void TestCloseStmtBeforeRows(TestingT *t) {
    test_database(t, testCloseStmtBeforeRows, NULL);
}

/* Tests fix for issue 2788, that we bind nil to a []byte if the value in the
 * column is sql null */
static void testNullByteSlice(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t|id=int32,name=nullstring") ||
        !EXECV(t, db, "INSERT|t|id=10,name=?", ((Any){NULL, NULL})))
        return;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Bytes name = slice_nil(TYPE_BYTE);
    SqlRow row = sql_db_query_row_v(db, BURROW_S("SELECT|t|name|id=?"), 10);
    Error err = sql_row_scan_v(&row, a, &name);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else if (name.p != NULL) {
        testing_t_errorf_v(
            t, "name []byte should be nil for null column value, got: %#v", name);
    } else if (EXECV(t, db, "INSERT|t|id=11,name=?", BURROW_S("bob"))) {
        row = sql_db_query_row_v(db, BURROW_S("SELECT|t|name|id=?"), 11);
        err = sql_row_scan_v(&row, a, &name);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else if (!str_eq((Str){name.p, name.len}, BURROW_S("bob")))
            testing_t_errorf_v(t, "name []byte should be bob, got: %q",
                               ((Str){name.p, name.len}));
    }
    arena_free(&ar);
}

static void TestNullByteSlice(TestingT *t) {
    test_database(t, testNullByteSlice, NULL);
}

BURROW_PTR_TYPE(StrPtr, Str);

static void testPointerParamsAndScans(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t|id=int32,name=nullstring"))
        return;
    Str bob = BURROW_S("bob");
    StrPtr name = &bob;
    if (!EXECV(t, db, "INSERT|t|id=10,name=?", ((Any){&burrow_type_StrPtr, &name})))
        return;
    name = NULL;
    if (!EXECV(t, db, "INSERT|t|id=20,name=?", ((Any){&burrow_type_StrPtr, &name})))
        return;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlRow row = sql_db_query_row_v(db, BURROW_S("SELECT|t|name|id=?"), 10);
    Error err = sql_row_scan_v(&row, a, ((Any){&burrow_type_StrPtr, &name}));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "querying id 10: %v", err);
        arena_free(&ar);
        return;
    }
    if (name == NULL)
        testing_t_errorf_v(t, "id 10's name = nil; want bob");
    else if (!str_eq(*name, BURROW_S("bob")))
        testing_t_errorf_v(t, "id 10's name = %q; want bob", *name);

    row = sql_db_query_row_v(db, BURROW_S("SELECT|t|name|id=?"), 20);
    err = sql_row_scan_v(&row, a, ((Any){&burrow_type_StrPtr, &name}));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "querying id 20: %v", err);
    else if (name != NULL)
        testing_t_errorf_v(t, "id 20 = %q; want nil", *name);
    arena_free(&ar);
}

static void TestPointerParamsAndScans(TestingT *t) {
    test_database(t, testPointerParamsAndScans, NULL);
}

static void testQueryRowClosingStmt(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Str name = BURROW_STR_EMPTY;
    Int age = 0;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    SqlRow row = sql_db_query_row_v(db, BURROW_S("SELECT|people|age,name|age=?"), 3);
    Error err = sql_row_scan_v(&row, arena_allocator(&ar), &age, &name);
    arena_free(&ar);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    if (burrow__sql_db_num_free(db) != 1)
        FATALF("expected 1 free conn");
    FakeConn *fake_conn = fdb_get_fake_conn(burrow__sql_db_free_conn(db, 0));
    if (fake_conn->stmts_made != fake_conn->stmts_closed)
        testing_t_errorf_v(t, "statement close mismatch: made %d, closed %d",
                           fake_conn->stmts_made, fake_conn->stmts_closed);
}

static void TestQueryRowClosingStmt(TestingT *t) {
    test_database(t, testQueryRowClosingStmt, NULL);
}

typedef struct NullTestRow {
    Any null_param;
    Any not_null_param;
    Any scan_null_val;
} NullTestRow;

typedef struct NullTestSpec {
    const char *null_type;
    const char *not_null_type;
    NullTestRow rows[6];
    /* reflect.DeepEqual on two values of the scan type. */
    bool (*eq)(const void *got, const void *want);
} NullTestSpec;

#define NV(T, ...) ((Any){TYPE_OF(T), &(T){__VA_ARGS__}})
#define NIL ((Any){NULL, NULL})

static const NullTestSpec *td_null_spec;

static void null_test_run(TestingT *t, SqlDB *db) {
    const NullTestSpec *spec = td_null_spec;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlStmt *stmt = NULL;
    Str create = fmt_sprintf_v(a, "CREATE|t|id=int32,name=string,nullf=%s,notnullf=%s",
                               spec->null_type, spec->not_null_type);
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec(db, a, create, NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Exec of %q: %v", create, err);
        goto out;
    }

    /* Inserts with db.Exec: */
    if (!EXECV(t, db, "INSERT|t|id=?,name=?,nullf=?,notnullf=?", 1, BURROW_S("alice"),
               spec->rows[0].null_param, spec->rows[0].not_null_param) ||
        !EXECV(t, db, "INSERT|t|id=?,name=?,nullf=?,notnullf=?", 2, BURROW_S("bob"),
               spec->rows[1].null_param, spec->rows[1].not_null_param))
        goto out;

    /* Inserts with a prepared statement: */
    stmt =
        sql_db_prepare(db, BURROW_S("INSERT|t|id=?,name=?,nullf=?,notnullf=?"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "prepare: %v", err);
        goto out;
    }
    (void)sql_stmt_exec_v(stmt, a, &err, 3, BURROW_S("chris"), spec->rows[2].null_param,
                          spec->rows[2].not_null_param);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "exec insert chris: %v", err);
    (void)sql_stmt_exec_v(stmt, a, &err, 4, BURROW_S("dave"), spec->rows[3].null_param,
                          spec->rows[3].not_null_param);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "exec insert dave: %v", err);
    (void)sql_stmt_exec_v(stmt, a, &err, 5, BURROW_S("eleanor"),
                          spec->rows[4].null_param, spec->rows[4].not_null_param);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "exec insert eleanor: %v", err);

    /* Can't put null val into non-null col */
    const NullTestRow *row5 = &spec->rows[5];
    (void)sql_stmt_exec_v(stmt, a, &err, 6, BURROW_S("bob"), row5->null_param,
                          row5->not_null_param);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "expected error inserting nil val with prepared statement "
                           "Exec: NULL=%#v, NOT-NULL=%#v",
                           row5->null_param, row5->not_null_param);

    /* Go leaves this one unchecked: fakeConn implements the optional Execer
     * interface, so arguably succeeding is the correct behavior. */
    (void)sql_db_exec_v(db, a, &err, BURROW_S("INSERT|t|id=?,name=?,nullf=?"), 999, NIL,
                        NIL);

    const Type *paramtype = spec->rows[0].null_param.t;
    Any bind_val = {paramtype, mem_alloc(a, paramtype->size, paramtype->align)};
    if (paramtype == TYPE_OF(SqlNull)) {
        /* Null[T]: the T is the one the parameter's v points at. */
        const Type *vt = ((const SqlNull *)spec->rows[0].null_param.data)->v.t;
        ((SqlNull *)bind_val.data)->v = (Any){vt, mem_alloc(a, vt->size, vt->align)};
    }

    for (Int i = 0; i < 5; i++) {
        Int id = i + 1;
        SqlRow row = sql_db_query_row_v(db, BURROW_S("SELECT|t|nullf|id=?"), id);
        err = sql_row_scan(&row, a, slice_from(&bind_val, 1, 1, TYPE_ANY));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "id=%d Scan: %v", id, err);
        if (!spec->eq(bind_val.data, spec->rows[i].scan_null_val.data))
            testing_t_errorf_v(t, "id=%d got %#v, want %#v", id, bind_val,
                               spec->rows[i].scan_null_val);
    }
out:
    if (stmt != NULL)
        sql_stmt_free(stmt);
    arena_free(&ar);
}

static void null_test(TestingT *t, const NullTestSpec *spec) {
    td_null_spec = spec;
    test_database(t, null_test_run, NULL);
}

static bool eq_null_string(const void *got, const void *want) {
    const SqlNullString *g = got, *w = want;
    return g->valid == w->valid && str_eq(g->string, w->string);
}

static bool eq_null_str(const void *got, const void *want) {
    const SqlNull *g = got, *w = want;
    return g->valid == w->valid &&
           str_eq(*(const Str *)g->v.data, *(const Str *)w->v.data);
}

static bool eq_null_int64(const void *got, const void *want) {
    const SqlNullInt64 *g = got, *w = want;
    return g->valid == w->valid && g->int64 == w->int64;
}

static bool eq_null_int32(const void *got, const void *want) {
    const SqlNullInt32 *g = got, *w = want;
    return g->valid == w->valid && g->int32 == w->int32;
}

static bool eq_null_int16(const void *got, const void *want) {
    const SqlNullInt16 *g = got, *w = want;
    return g->valid == w->valid && g->int16 == w->int16;
}

static bool eq_null_byte(const void *got, const void *want) {
    const SqlNullByte *g = got, *w = want;
    return g->valid == w->valid && g->byte == w->byte;
}

static bool eq_null_float64(const void *got, const void *want) {
    const SqlNullFloat64 *g = got, *w = want;
    return g->valid == w->valid && g->float64 == w->float64;
}

static bool eq_null_bool(const void *got, const void *want) {
    const SqlNullBool *g = got, *w = want;
    return g->valid == w->valid && g->bool_ == w->bool_;
}

static bool eq_null_time(const void *got, const void *want) {
    const SqlNullTime *g = got, *w = want;
    return g->valid == w->valid && time_equal(g->time, w->time);
}

static bool eq_null_uuid(const void *got, const void *want) {
    const SqlNull *g = got, *w = want;
    return g->valid == w->valid &&
           memcmp(((const Uuid *)g->v.data)->b, ((const Uuid *)w->v.data)->b, 16) == 0;
}

static void TestNullStringParam(TestingT *t) {
    NullTestSpec spec = {
        "nullstring",
        "string",
        {
            {NV(SqlNullString, BURROW_S_INIT("aqua"), true),
             BURROW_ANY_OF(BURROW_S("")),
             NV(SqlNullString, BURROW_S_INIT("aqua"), true)},
            {NV(SqlNullString, BURROW_S_INIT("brown"), false),
             BURROW_ANY_OF(BURROW_S("")), NV(SqlNullString, BURROW_S_INIT(""), false)},
            {BURROW_ANY_OF(BURROW_S("chartreuse")), BURROW_ANY_OF(BURROW_S("")),
             NV(SqlNullString, BURROW_S_INIT("chartreuse"), true)},
            {NV(SqlNullString, BURROW_S_INIT("darkred"), true),
             BURROW_ANY_OF(BURROW_S("")),
             NV(SqlNullString, BURROW_S_INIT("darkred"), true)},
            {NV(SqlNullString, BURROW_S_INIT("eel"), false),
             BURROW_ANY_OF(BURROW_S("")), NV(SqlNullString, BURROW_S_INIT(""), false)},
            {BURROW_ANY_OF(BURROW_S("foo")),
             NV(SqlNullString, BURROW_S_INIT("black"), false), NIL},
        },
        eq_null_string,
    };
    null_test(t, &spec);
}

#define NSTR(lit, ok) NV(SqlNull, {TYPE_STRING, &(Str)BURROW_S_INIT(lit)}, (ok))

static void TestGenericNullStringParam(TestingT *t) {
    NullTestSpec spec = {
        "nullstring",
        "string",
        {
            {NSTR("aqua", true), BURROW_ANY_OF(BURROW_S("")), NSTR("aqua", true)},
            {NSTR("brown", false), BURROW_ANY_OF(BURROW_S("")), NSTR("", false)},
            {BURROW_ANY_OF(BURROW_S("chartreuse")), BURROW_ANY_OF(BURROW_S("")),
             NSTR("chartreuse", true)},
            {NSTR("darkred", true), BURROW_ANY_OF(BURROW_S("")), NSTR("darkred", true)},
            {NSTR("eel", false), BURROW_ANY_OF(BURROW_S("")), NSTR("", false)},
            {BURROW_ANY_OF(BURROW_S("foo")), NSTR("black", false), NIL},
        },
        eq_null_str,
    };
    null_test(t, &spec);
}

static void TestNullInt64Param(TestingT *t) {
    NullTestSpec spec = {
        "nullint64",
        "int64",
        {
            {NV(SqlNullInt64, 31, true), BURROW_ANY_OF(1), NV(SqlNullInt64, 31, true)},
            {NV(SqlNullInt64, -22, false), BURROW_ANY_OF(1),
             NV(SqlNullInt64, 0, false)},
            {BURROW_ANY_OF(22), BURROW_ANY_OF(1), NV(SqlNullInt64, 22, true)},
            {NV(SqlNullInt64, 33, true), BURROW_ANY_OF(1), NV(SqlNullInt64, 33, true)},
            {NV(SqlNullInt64, 222, false), BURROW_ANY_OF(1),
             NV(SqlNullInt64, 0, false)},
            {BURROW_ANY_OF(0), NV(SqlNullInt64, 31, false), NIL},
        },
        eq_null_int64,
    };
    null_test(t, &spec);
}

static void TestNullInt32Param(TestingT *t) {
    NullTestSpec spec = {
        "nullint32",
        "int32",
        {
            {NV(SqlNullInt32, 31, true), BURROW_ANY_OF(1), NV(SqlNullInt32, 31, true)},
            {NV(SqlNullInt32, -22, false), BURROW_ANY_OF(1),
             NV(SqlNullInt32, 0, false)},
            {BURROW_ANY_OF(22), BURROW_ANY_OF(1), NV(SqlNullInt32, 22, true)},
            {NV(SqlNullInt32, 33, true), BURROW_ANY_OF(1), NV(SqlNullInt32, 33, true)},
            {NV(SqlNullInt32, 222, false), BURROW_ANY_OF(1),
             NV(SqlNullInt32, 0, false)},
            {BURROW_ANY_OF(0), NV(SqlNullInt32, 31, false), NIL},
        },
        eq_null_int32,
    };
    null_test(t, &spec);
}

static void TestNullInt16Param(TestingT *t) {
    NullTestSpec spec = {
        "nullint16",
        "int16",
        {
            {NV(SqlNullInt16, 31, true), BURROW_ANY_OF(1), NV(SqlNullInt16, 31, true)},
            {NV(SqlNullInt16, -22, false), BURROW_ANY_OF(1),
             NV(SqlNullInt16, 0, false)},
            {BURROW_ANY_OF(22), BURROW_ANY_OF(1), NV(SqlNullInt16, 22, true)},
            {NV(SqlNullInt16, 33, true), BURROW_ANY_OF(1), NV(SqlNullInt16, 33, true)},
            {NV(SqlNullInt16, 222, false), BURROW_ANY_OF(1),
             NV(SqlNullInt16, 0, false)},
            {BURROW_ANY_OF(0), NV(SqlNullInt16, 31, false), NIL},
        },
        eq_null_int16,
    };
    null_test(t, &spec);
}

static void TestNullByteParam(TestingT *t) {
    NullTestSpec spec = {
        "nullbyte",
        "byte",
        {
            {NV(SqlNullByte, 31, true), BURROW_ANY_OF(1), NV(SqlNullByte, 31, true)},
            {NV(SqlNullByte, 0, false), BURROW_ANY_OF(1), NV(SqlNullByte, 0, false)},
            {BURROW_ANY_OF(22), BURROW_ANY_OF(1), NV(SqlNullByte, 22, true)},
            {NV(SqlNullByte, 33, true), BURROW_ANY_OF(1), NV(SqlNullByte, 33, true)},
            {NV(SqlNullByte, 222, false), BURROW_ANY_OF(1), NV(SqlNullByte, 0, false)},
            {BURROW_ANY_OF(0), NV(SqlNullByte, 31, false), NIL},
        },
        eq_null_byte,
    };
    null_test(t, &spec);
}

static void TestNullFloat64Param(TestingT *t) {
    NullTestSpec spec = {
        "nullfloat64",
        "float64",
        {
            {NV(SqlNullFloat64, 31.2, true), BURROW_ANY_OF(1),
             NV(SqlNullFloat64, 31.2, true)},
            {NV(SqlNullFloat64, 13.1, false), BURROW_ANY_OF(1),
             NV(SqlNullFloat64, 0, false)},
            {BURROW_ANY_OF(-22.9), BURROW_ANY_OF(1), NV(SqlNullFloat64, -22.9, true)},
            {NV(SqlNullFloat64, 33.81, true), BURROW_ANY_OF(1),
             NV(SqlNullFloat64, 33.81, true)},
            {NV(SqlNullFloat64, 222, false), BURROW_ANY_OF(1),
             NV(SqlNullFloat64, 0, false)},
            {BURROW_ANY_OF(10), NV(SqlNullFloat64, 31.2, false), NIL},
        },
        eq_null_float64,
    };
    null_test(t, &spec);
}

static void TestNullBoolParam(TestingT *t) {
    NullTestSpec spec = {
        "nullbool",
        "bool",
        {
            {NV(SqlNullBool, false, true), BURROW_ANY_OF(true),
             NV(SqlNullBool, false, true)},
            {NV(SqlNullBool, true, false), BURROW_ANY_OF(false),
             NV(SqlNullBool, false, false)},
            {BURROW_ANY_OF(true), BURROW_ANY_OF(true), NV(SqlNullBool, true, true)},
            {NV(SqlNullBool, true, true), BURROW_ANY_OF(false),
             NV(SqlNullBool, true, true)},
            {NV(SqlNullBool, true, false), BURROW_ANY_OF(true),
             NV(SqlNullBool, false, false)},
            {BURROW_ANY_OF(true), NV(SqlNullBool, true, false), NIL},
        },
        eq_null_bool,
    };
    null_test(t, &spec);
}

static void TestNullTimeParam(TestingT *t) {
    Time t0 = {0};
    Time t1 = time_date(2000, TIME_JANUARY, 1, 8, 9, 10, 11, time_utc_loc);
    Time t2 = time_date(2010, TIME_JANUARY, 1, 8, 9, 10, 11, time_utc_loc);
    NullTestSpec spec = {
        "nulldatetime",
        "datetime",
        {
            {NV(SqlNullTime, t1, true), BURROW_ANY(TYPE_TIME, &t2),
             NV(SqlNullTime, t1, true)},
            {NV(SqlNullTime, t1, false), BURROW_ANY(TYPE_TIME, &t2),
             NV(SqlNullTime, t0, false)},
            {BURROW_ANY(TYPE_TIME, &t1), BURROW_ANY(TYPE_TIME, &t2),
             NV(SqlNullTime, t1, true)},
            {NV(SqlNullTime, t1, true), BURROW_ANY(TYPE_TIME, &t2),
             NV(SqlNullTime, t1, true)},
            {NV(SqlNullTime, t1, false), BURROW_ANY(TYPE_TIME, &t2),
             NV(SqlNullTime, t0, false)},
            {BURROW_ANY(TYPE_TIME, &t2), NV(SqlNullTime, t1, false), NIL},
        },
        eq_null_time,
    };
    null_test(t, &spec);
}

static void TestNullUUIDParam(TestingT *t) {
    Uuid u0 = {{0}};
    Uuid u1 = uuid_must_parse(BURROW_S("46cd2740-6081-4289-a659-03b61ebb92f7"));
    Uuid u2 = uuid_must_parse(BURROW_S("46cd2740-6081-4289-a659-03b61ebb92f7"));
    NullTestSpec spec = {
        "nulluuid",
        "uuid",
        {
            {NV(SqlNull, {TYPE_UUID, &u1}, true), BURROW_ANY(TYPE_UUID, &u2),
             NV(SqlNull, {TYPE_UUID, &u1}, true)},
            {NV(SqlNull, {TYPE_UUID, &u1}, false), BURROW_ANY(TYPE_UUID, &u2),
             NV(SqlNull, {TYPE_UUID, &u0}, false)},
            {BURROW_ANY(TYPE_UUID, &u1), BURROW_ANY(TYPE_UUID, &u2),
             NV(SqlNull, {TYPE_UUID, &u1}, true)},
            {NV(SqlNull, {TYPE_UUID, &u1}, true), BURROW_ANY(TYPE_UUID, &u2),
             NV(SqlNull, {TYPE_UUID, &u1}, true)},
            {NV(SqlNull, {TYPE_UUID, &u1}, false), BURROW_ANY(TYPE_UUID, &u2),
             NV(SqlNull, {TYPE_UUID, &u0}, false)},
            {BURROW_ANY(TYPE_UUID, &u2), NV(SqlNull, {TYPE_UUID, &u1}, false), NIL},
        },
        eq_null_uuid,
    };
    null_test(t, &spec);
}

/* golang.org/issue/4859 */
static void testQueryRowNilScanDest(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    /* nil pointer */
    SqlRow row = sql_db_query_row(db, BURROW_S("SELECT|people|name|"), NO_ARGS);
    Any name = {TYPE_STRING, NULL};
    Error err = sql_row_scan(&row, heap_allocator(), slice_from(&name, 1, 1, TYPE_ANY));
    const char *want =
        "sql: Scan error on column index 0, name \"name\": destination pointer is nil";
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), str_from_cstr(want)))
        testing_t_errorf_v(t, "error = %q; want %q", error_text(err), want);
}

static void TestQueryRowNilScanDest(TestingT *t) {
    test_database(t, testQueryRowNilScanDest, NULL);
}

static void testIssue4902(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Int opens0 = fdb_open_count;
    SqlStmt *stmt = NULL;
    Error err = BURROW_NO_ERROR;
    for (int i = 0; i < 10; i++) {
        stmt = sql_db_prepare(db, BURROW_S("SELECT|people|name|"), &err);
        if (BURROW_FAILED(err))
            FATALF("%v", err);
        err = sql_stmt_close(stmt);
        sql_stmt_free(stmt);
        if (BURROW_FAILED(err))
            FATALF("%v", err);
    }
    Int opens = fdb_open_count - opens0;
    if (opens > 1)
        testing_t_errorf_v(t, "opens = %d; want <= 1", opens);
}

static void TestIssue4902(TestingT *t) {
    test_database(t, testIssue4902, NULL);
}

/* Issue 3857
 * This used to deadlock. */
static void testSimultaneousQueries(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    SqlRows *r1 = sql_tx_query(tx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        SqlRows *r2 = sql_tx_query(tx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        if (r2 != NULL)
            sql_rows_free(r2);
    }
    if (r1 != NULL)
        sql_rows_free(r1);
    (void)sql_tx_rollback(tx);
    sql_tx_free(tx);
}

static void TestSimultaneousQueries(TestingT *t) {
    test_database(t, testSimultaneousQueries, NULL);
}

/* A sentinel of the test's own, for the errors Go makes with errors.New and
 * then compares by identity. */
#define TD_SENTINEL(name, text)                                                        \
    static const Str name##__text = BURROW_S_INIT(text);                               \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

static Int num_deps(SqlDB *db) {
    synctest_wait();
    return burrow__sql_db_num_deps(db);
}

/* clearAllConns closes all connections in db. */
static void clear_all_conns(TestingT *t, SqlDB *db) {
    sql_db_set_max_idle_conns(db, 0);
    Int n = num_free_conns(db);
    if (n != 0)
        testing_t_errorf_v(t, "free conns = %d; want %d", n, (Int)0);
    n = num_deps(db);
    if (n > 0)
        testing_t_errorf_v(t, "number of dependencies = %d; expected 0", n);
}

/* The fake driver's open and close counts. */
static void driver_counts(Int *opens, Int *closes) {
    sync_mutex_lock(&fdb_mu);
    *opens = fdb_open_count;
    *closes = fdb_close_count;
    sync_mutex_unlock(&fdb_mu);
}

/* Errors the post close hook sees go to t, until the hook is unset. */
static void set_close_hook(TestingT *t) {
    td_close_t = t;
    fdb_set_hook_post_close_conn(td_post_close);
}

static void unset_close_hook(void) {
    fdb_set_hook_post_close_conn(NULL);
    td_close_t = NULL;
}

static void testMaxIdleConns(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    (void)sql_tx_commit(tx);
    sql_tx_free(tx);
    Int got = burrow__sql_db_num_free(db);
    if (got != 1)
        testing_t_errorf_v(t, "freeConns = %d; want 1", got);

    sql_db_set_max_idle_conns(db, 0);

    got = burrow__sql_db_num_free(db);
    if (got != 0)
        testing_t_errorf_v(t, "freeConns after set to zero = %d; want 0", got);

    tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    (void)sql_tx_commit(tx);
    sql_tx_free(tx);
    got = burrow__sql_db_num_free(db);
    if (got != 0)
        testing_t_errorf_v(t, "freeConns = %d; want 0", got);
}

static void TestMaxIdleConns(TestingT *t) {
    test_database(t, testMaxIdleConns, NULL);
}

typedef struct TdSlowQuery {
    TestingT *t;
    SqlStmt *stmt;
} TdSlowQuery;

static void td_slow_query(void *env) {
    TdSlowQuery *q = env;
    Str op = BURROW_STR_EMPTY;
    SqlRow row = sql_stmt_query_row_v(q->stmt, BURROW_S("sleep"), (Int)25);
    Error err = sql_row_scan_v(&row, heap_allocator(), &op);
    if (BURROW_FAILED(err)) {
        if (!errors_is(err, sql_err_no_rows))
            testing_t_errorf_v(q->t, "%v", err);
    } else {
        mem_free(heap_allocator(), (void *)(uintptr_t)op.p, (size_t)op.len, 1);
    }
}

/* Runs nbatch batches of nquery slow queries on stmt at once, waiting for each
 * batch to finish before starting the next. */
static void run_slow_queries(TestingT *t, SqlStmt *stmt, int nquery, int nbatch) {
    TdSlowQuery q = {t, stmt};
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    for (int batch = 0; batch < nbatch; batch++) {
        for (int i = 0; i < nquery; i++)
            (void)sync_wait_group_go(&wg, BURROW_FN(Func, td_slow_query, &q));
        sync_wait_group_wait(&wg);
    }
}

static void testMaxOpenConns(TestingT *t, SqlDB *db) {
    if (testing_short()) {
        testing_t_logf_v(t, "skipping in short mode");
        return;
    }
    set_close_hook(t);
    if (!populate(t, db, "magicquery")) {
        unset_close_hook();
        return;
    }

    /* Force the number of open connections to 0 so we can get an accurate
     * count for the test. */
    clear_all_conns(t, db);

    Int opens0 = 0, closes0 = 0;
    driver_counts(&opens0, &closes0);

    sql_db_set_max_idle_conns(db, 10);
    sql_db_set_max_open_conns(db, 10);

    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt =
        sql_db_prepare(db, BURROW_S("SELECT|magicquery|op|op=?,millis=?"), &err);
    if (BURROW_FAILED(err)) {
        unset_close_hook();
        FATALF("%v", err);
    }

    /* Start 50 parallel slow queries, twice. */
    run_slow_queries(t, stmt, 50, 2);

    Int g = num_free_conns(db);
    if (g != 10)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)10);

    Int n = num_deps(db);
    if (n > 20)
        testing_t_errorf_v(t, "number of dependencies = %d; expected <= 20", n);

    Int opens = 0, closes = 0;
    driver_counts(&opens, &closes);
    opens -= opens0;
    closes -= closes0;

    if (opens > 10) {
        testing_t_logf_v(t, "open calls = %d", opens);
        testing_t_logf_v(t, "close calls = %d", closes);
        testing_t_errorf_v(t, "db connections opened = %d; want <= 10", opens);
    }

    err = sql_stmt_close(stmt);
    sql_stmt_free(stmt);
    if (BURROW_FAILED(err)) {
        unset_close_hook();
        FATALF("%v", err);
    }

    g = num_free_conns(db);
    if (g != 10)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)10);

    n = num_deps(db);
    if (n > 10)
        testing_t_errorf_v(t, "number of dependencies = %d; expected <= 10", n);

    sql_db_set_max_open_conns(db, 5);

    g = num_free_conns(db);
    if (g != 5)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)5);

    n = num_deps(db);
    if (n > 5)
        testing_t_errorf_v(t, "number of dependencies = %d; expected 0", n);

    sql_db_set_max_open_conns(db, 0);

    g = num_free_conns(db);
    if (g != 5)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)5);

    n = num_deps(db);
    if (n > 5)
        testing_t_errorf_v(t, "number of dependencies = %d; expected 0", n);

    clear_all_conns(t, db);
    unset_close_hook();
}

static void TestMaxOpenConns(TestingT *t) {
    test_database(t, testMaxOpenConns, NULL);
}

/* Issue 9453: tests that SetMaxOpenConns can be lowered at runtime and affects
 * the subsequent release of connections. */
static void testMaxOpenConnsOnBusy(TestingT *t, SqlDB *db) {
    set_close_hook(t);
    if (!populate(t, db, "magicquery")) {
        unset_close_hook();
        return;
    }

    sql_db_set_max_open_conns(db, 3);

    Context ctx = context_background();
    void *conn[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        conn[i] = burrow__sql_db_conn(db, ctx, false, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "db open conn fail: %v", err);
            for (int j = 0; j < i; j++)
                burrow__sql_db_put_conn(conn[j], BURROW_NO_ERROR, true);
            unset_close_hook();
            return;
        }
    }

    Int g = burrow__sql_db_num_open(db);
    if (g != 3)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)3);

    sql_db_set_max_open_conns(db, 2);
    g = burrow__sql_db_num_open(db);
    if (g != 3)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)3);

    burrow__sql_db_put_conn(conn[0], BURROW_NO_ERROR, true);
    burrow__sql_db_put_conn(conn[1], BURROW_NO_ERROR, true);
    g = burrow__sql_db_num_open(db);
    if (g != 2)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)2);

    burrow__sql_db_put_conn(conn[2], BURROW_NO_ERROR, true);
    g = burrow__sql_db_num_open(db);
    if (g != 2)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)2);
    unset_close_hook();
}

static void TestMaxOpenConnsOnBusy(TestingT *t) {
    test_database(t, testMaxOpenConnsOnBusy, NULL);
}

enum { TD_MAX_OPEN = 2, TD_TRY_OPEN = TD_MAX_OPEN * 2 + 2 };

TD_SENTINEL(td_err_offline, "db offline");

static SyncWaitGroup td_opening;

static Error td_open_offline(void) {
    /* Wait for all connections to enqueue. */
    sync_wait_group_wait(&td_opening);
    return td_err_offline;
}

typedef struct TdPending {
    SqlDB *db;
    Chan *errs;
} TdPending;

static void td_pending_exec(void *env) {
    TdPending *p = env;
    sync_wait_group_done(&td_opening); /* signal one connection is in flight */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec(p->db, arena_allocator(&ar), BURROW_S("will never run"), NO_ARGS,
                      &err);
    arena_free(&ar);
    bool same = err.vt == td_err_offline.vt && err.data == td_err_offline.data;
    chan_send(p->errs, &same);
}

/* Issue 10886: tests that all connection attempts return when more than
 * DB.maxOpen connections are in flight and the first DB.maxOpen fail. */
static void testPendingConnsAfterErr(TestingT *t) {
    /* No queries will be run. */
    register_test_driver();
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("test"), fake_db_name, &err);
    if (BURROW_FAILED(err))
        FATALF("Open: %v", err);

    sql_db_set_max_open_conns(db, TD_MAX_OPEN);
    sql_db_set_max_idle_conns(db, 0);

    TdPending p = {db, chan_make(heap_allocator(), TYPE_BOOL, TD_TRY_OPEN)};

    memset(&td_opening, 0, sizeof td_opening);
    sync_wait_group_add(&td_opening, TD_TRY_OPEN);

    fdb_set_hook_open_err(td_open_offline);

    for (int i = 0; i < TD_TRY_OPEN; i++)
        (void)go(BURROW_FN(Func, td_pending_exec, &p));

    sync_wait_group_wait(&td_opening); /* wait for all workers to begin running */

    /* Check that all connections fail without deadlock. A deadlock is one the
     * bubble reports. */
    for (int i = 0; i < TD_TRY_OPEN; i++) {
        bool same = false;
        (void)chan_recv(p.errs, &same);
        if (!same)
            testing_t_errorf_v(t, "unexpected err: want %v", td_err_offline);
    }

    /* Wait a reasonable time for the database to close all connections.
     * Closing the database will check for numOpen and fail the test. */
    Time deadline = time_add(time_now(), 5 * TIME_SECOND);
    while (burrow__sql_db_num_open(db) != 0 && time_before(time_now(), deadline))
        time_sleep(3 * TIME_MILLISECOND);

    fdb_set_hook_open_err(NULL);
    chan_free(p.errs);
    close_db(t, db);
}

static void TestPendingConnsAfterErr(TestingT *t) {
    synctest_test(t, testPendingConnsAfterErr);
}

static void testSingleOpenConn(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    sql_db_set_max_open_conns(db, 1);

    for (int i = 0; i < 2; i++) { /* the second shouldn't deadlock */
        Error err = BURROW_NO_ERROR;
        SqlRows *rows =
            sql_db_query(db, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
        if (BURROW_FAILED(err))
            FATALF("%v", err);
        err = sql_rows_close(rows);
        sql_rows_free(rows);
        if (BURROW_FAILED(err))
            FATALF("%v", err);
    }
}

static void TestSingleOpenConn(TestingT *t) {
    test_database(t, testSingleOpenConn, NULL);
}

static void testStats(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    SqlDBStats stats = sql_db_stats(db);
    if (stats.open_connections != 1)
        testing_t_errorf_v(t, "stats.OpenConnections = %d; want 1",
                           stats.open_connections);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    (void)sql_tx_commit(tx);
    sql_tx_free(tx);

    /* closeDB without the free, which the harness does after. */
    err = sql_db_close(db);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error closing DB: %v", err);
    synctest_wait();
    stats = sql_db_stats(db);
    if (stats.open_connections != 0)
        testing_t_errorf_v(t, "stats.OpenConnections = %d; want 0",
                           stats.open_connections);
}

static void TestStats(TestingT *t) {
    test_database(t, testStats, NULL);
}

static void testConnMaxLifetime(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "magicquery"))
        return;

    /* Force the number of open connections to 0 so we can get an accurate
     * count for the test. */
    clear_all_conns(t, db);

    Int opens0 = 0, closes0 = 0;
    driver_counts(&opens0, &closes0);

    sql_db_set_max_idle_conns(db, 10);
    sql_db_set_max_open_conns(db, 10);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    synctest_sleep(1 * TIME_SECOND);
    SqlTx *tx2 = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        sql_tx_free(tx);
        FATALF("%v", err);
    }

    (void)sql_tx_commit(tx);
    (void)sql_tx_commit(tx2);
    sql_tx_free(tx);
    sql_tx_free(tx2);

    Int opens = 0, closes = 0;
    driver_counts(&opens, &closes);
    opens -= opens0;
    closes -= closes0;

    if (opens != 2)
        testing_t_errorf_v(t, "opens = %d; want 2", opens);
    if (closes != 0)
        testing_t_errorf_v(t, "closes = %d; want 0", closes);
    Int g = num_free_conns(db);
    if (g != 2)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)2);

    /* Expire first conn. */
    synctest_sleep(10 * TIME_SECOND);
    sql_db_set_conn_max_lifetime(db, 10 * TIME_SECOND);

    tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    tx2 = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        sql_tx_free(tx);
        FATALF("%v", err);
    }
    (void)sql_tx_commit(tx);
    (void)sql_tx_commit(tx2);
    sql_tx_free(tx);
    sql_tx_free(tx2);

    /* Give connectionCleaner chance to run. */
    synctest_wait();
    driver_counts(&opens, &closes);
    opens -= opens0;
    closes -= closes0;

    if (opens != 3)
        testing_t_errorf_v(t, "opens = %d; want 3", opens);
    if (closes != 1)
        testing_t_errorf_v(t, "closes = %d; want 1", closes);

    SqlDBStats s = sql_db_stats(db);
    if (s.max_lifetime_closed != 1)
        testing_t_errorf_v(t, "MaxLifetimeClosed = %d; want 1", s.max_lifetime_closed);
}

static void TestConnMaxLifetime(TestingT *t) {
    test_database(t, testConnMaxLifetime, NULL);
}

/* golang.org/issue/5323 */
static void testStmtCloseDeps(TestingT *t, SqlDB *db) {
    if (testing_short()) {
        testing_t_logf_v(t, "skipping in short mode");
        return;
    }
    set_close_hook(t);
    if (!populate(t, db, "magicquery")) {
        unset_close_hook();
        return;
    }

    Int opens0 = 0, closes0 = 0;
    driver_counts(&opens0, &closes0);
    Int open_delta0 = opens0 - closes0;

    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt =
        sql_db_prepare(db, BURROW_S("SELECT|magicquery|op|op=?,millis=?"), &err);
    if (BURROW_FAILED(err)) {
        unset_close_hook();
        FATALF("%v", err);
    }

    /* Start 50 parallel slow queries, twice. */
    enum { nquery = 50 };
    run_slow_queries(t, stmt, nquery, 2);

    Int g = num_free_conns(db);
    if (g != 2)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)2);

    Int n = num_deps(db);
    if (n > 4)
        testing_t_errorf_v(t, "number of dependencies = %d; expected <= 4", n);

    Int opens = 0, closes = 0;
    driver_counts(&opens, &closes);
    Int open_delta = (opens - closes) - open_delta0;
    opens -= opens0;
    closes -= closes0;

    if (open_delta > 2) {
        testing_t_logf_v(t, "open calls = %d", opens);
        testing_t_logf_v(t, "close calls = %d", closes);
        testing_t_logf_v(t, "open delta = %d", open_delta);
        testing_t_errorf_v(t, "db connections opened = %d; want <= 2", open_delta);
    }

    synctest_wait();
    Int ncss = burrow__sql_stmt_num_css(stmt);
    if (ncss > nquery)
        testing_t_errorf_v(t, "len(stmt.css) = %d; want <= %d", ncss, (Int)nquery);

    err = sql_stmt_close(stmt);
    sql_stmt_free(stmt);
    if (BURROW_FAILED(err)) {
        unset_close_hook();
        FATALF("%v", err);
    }

    g = num_free_conns(db);
    if (g != 2)
        testing_t_errorf_v(t, "free conns = %d; want %d", g, (Int)2);

    n = num_deps(db);
    if (n > 2)
        testing_t_errorf_v(t, "number of dependencies = %d; expected <= 2", n);

    clear_all_conns(t, db);
    unset_close_hook();
}

static void TestStmtCloseDeps(TestingT *t) {
    test_database(t, testStmtCloseDeps, NULL);
}

/* golang.org/issue/5046 */
static void testCloseConnBeforeStmts(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    set_close_hook(t);

    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT|people|name|"), &err);
    if (BURROW_FAILED(err)) {
        unset_close_hook();
        FATALF("%v", err);
    }

    Int nfree = burrow__sql_db_num_free(db);
    if (nfree != 1) {
        sql_stmt_free(stmt);
        unset_close_hook();
        FATALF("expected 1 freeConn; got %d", nfree);
    }
    void *dc = burrow__sql_db_free_dc(db, 0);
    if (burrow__sql_dc_closed(dc))
        testing_t_errorf_v(t, "conn shouldn't be closed");

    Int n = burrow__sql_dc_num_open_stmt(dc);
    if (n != 1)
        testing_t_errorf_v(t, "driverConn num openStmt = %d; want 1", n);
    err = sql_db_close(db);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "db Close = %v", err);
    if (!burrow__sql_dc_closed(dc))
        testing_t_errorf_v(t, "after db.Close, driverConn should be closed");
    n = burrow__sql_dc_num_open_stmt(dc);
    if (n != 0)
        testing_t_errorf_v(t, "driverConn num openStmt = %d; want 0", n);

    err = sql_stmt_close(stmt);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Stmt close = %v", err);
    sql_stmt_free(stmt);

    if (!burrow__sql_dc_closed(dc))
        testing_t_errorf_v(t, "conn should be closed");
    if (burrow__sql_dc_has_driver(dc))
        testing_t_errorf_v(
            t, "after Stmt Close, driverConn's Conn interface should be nil");
    burrow__sql_dc_unref(dc);
    unset_close_hook();
}

static void TestCloseConnBeforeStmts(TestingT *t) {
    test_database(t, testCloseConnBeforeStmts, NULL);
}

/* golang.org/issue/5283: don't release the Rows' connection in Close before
 * calling Stmt.Close. */
static void testRowsCloseOrder(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    sql_db_set_max_idle_conns(db, 0);
    fdb_strict_close = t;

    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        fdb_strict_close = NULL;
        FATALF("%v", err);
    }
    err = sql_rows_close(rows);
    sql_rows_free(rows);
    fdb_strict_close = NULL;
    if (BURROW_FAILED(err))
        FATALF("%v", err);
}

static void TestRowsCloseOrder(TestingT *t) {
    test_database(t, testRowsCloseOrder, NULL);
}

TD_SENTINEL(td_err_fail, "fail");

static void testRowsImplicitClose(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    Int want = 2;
    FakeCursor *r = fdb_get_rows_cursor(rows);
    r->err_pos = want;
    r->err = td_err_fail;
    /* Keep the cursor past its close, to look at it after. */
    r->in_table = true;

    Int got = 0;
    while (sql_rows_next(rows))
        got++;
    if (got != want)
        testing_t_errorf_v(t, "got %d rows, want %d", got, want);
    err = sql_rows_err(rows);
    if (!errors_is(err, td_err_fail))
        testing_t_errorf_v(t, "got error %v, want %v", err, td_err_fail);
    if (!r->closed)
        testing_t_errorf_v(t, "r.closed is false, want true");
    sql_rows_free(rows);
    fdb_cursor_free(r);
}

static void TestRowsImplicitClose(TestingT *t) {
    test_database(t, testRowsImplicitClose, NULL);
}

TD_SENTINEL(td_err_rows_close, "rowsCursor: failed to close");

static void testRowsCloseError(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);

    FakeCursor *rc = fdb_get_rows_cursor(rows);
    rc->close_err = td_err_rows_close;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    while (sql_rows_next(rows)) {
        Int age = 0;
        Str name = BURROW_STR_EMPTY;
        err = sql_rows_scan_v(rows, arena_allocator(&ar), &age, &name);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Scan: %v", err);
            break;
        }
    }
    if (!BURROW_FAILED(err)) {
        err = sql_rows_err(rows);
        if (!errors_is(err, td_err_rows_close))
            testing_t_errorf_v(t, "unexpected err: got %v, want %v", err,
                               td_err_rows_close);
    }
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestRowsCloseError(TestingT *t) {
    test_database(t, testRowsCloseError, NULL);
}

static void testStmtCloseOrder(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    sql_db_set_max_idle_conns(db, 0);
    fdb_strict_close = t;

    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|non_existent|name|"), NO_ARGS, &err);
    fdb_strict_close = NULL;
    if (!BURROW_FAILED(err)) {
        sql_rows_free(rows);
        FATALF("Querying non-existent table should fail");
    }
}

static void TestStmtCloseOrder(TestingT *t) {
    test_database(t, testStmtCloseOrder, NULL);
}

enum { TD_MAX_BAD_CONN_RETRIES = 2 };

/* What manyErrBadConnSetup's first function does: prepare query, when there is
 * one, before the connections go bad. */
static SqlDB *many_err_bad_conn_setup(TestingT *t, FakeConnector *fc,
                                      const char *prepare, SqlStmt **stmt) {
    SqlDB *db = new_test_db(t, fc, "people");
    if (db == NULL)
        return NULL;
    Error err = BURROW_NO_ERROR;
    if (prepare != NULL) {
        *stmt = sql_db_prepare(db, str_from_cstr(prepare), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            close_db(t, db);
            return NULL;
        }
    }

    enum { nconn = TD_MAX_BAD_CONN_RETRIES + 1 };
    sql_db_set_max_idle_conns(db, nconn);
    sql_db_set_max_open_conns(db, nconn);
    /* open enough connections */
    SqlRows *rows[nconn] = {NULL};
    bool ok = true;
    for (int i = 0; i < nconn && ok; i++) {
        rows[i] = sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            ok = false;
        }
    }
    for (int i = 0; i < nconn; i++)
        if (rows[i] != NULL)
            sql_rows_free(rows[i]);

    Int n = burrow__sql_db_num_open(db);
    Int nfree = burrow__sql_db_num_free(db);
    if (ok && n != nconn) {
        testing_t_errorf_v(t, "unexpected numOpen %d (was expecting %d)", n,
                           (Int)nconn);
        ok = false;
    } else if (ok && nfree != nconn) {
        testing_t_errorf_v(t, "unexpected len(db.freeConn) %d (was expecting %d)",
                           nfree, (Int)nconn);
        ok = false;
    }
    if (!ok) {
        if (prepare != NULL)
            sql_stmt_free(*stmt);
        close_db(t, db);
        return NULL;
    }
    for (Int i = 0; i < nfree; i++)
        fdb_get_fake_conn(burrow__sql_db_free_conn(db, i))->sticky_bad = true;
    return db;
}

/* Test cases where there's more than maxBadConnRetries bad connections in the
 * pool (issue 8834). */
static void testManyErrBadConn(TestingT *t) {
    FakeConnector fc;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = NULL;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    /* Query */
    memset(&fc, 0, sizeof fc);
    SqlDB *db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        err = sql_rows_close(rows);
        sql_rows_free(rows);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Exec */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    (void)sql_db_exec(db, arena_allocator(&ar),
                      BURROW_S("INSERT|people|name=Julia,age=19"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Begin */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        err = sql_tx_rollback(tx);
        sql_tx_free(tx);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Prepare */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    stmt = sql_db_prepare(db, BURROW_S("SELECT|people|age,name|"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        err = sql_stmt_close(stmt);
        sql_stmt_free(stmt);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Stmt.Exec */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, "INSERT|people|name=Julia,age=19", &stmt);
    if (db == NULL)
        goto out;
    (void)sql_stmt_exec(stmt, arena_allocator(&ar), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    err = sql_stmt_close(stmt);
    sql_stmt_free(stmt);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Stmt.Query */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, "SELECT|people|age,name|", &stmt);
    if (db == NULL)
        goto out;
    rows = sql_stmt_query(stmt, NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        err = sql_rows_close(rows);
        sql_rows_free(rows);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    err = sql_stmt_close(stmt);
    sql_stmt_free(stmt);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Conn */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    Context ctx = context_background();
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;
        err = sql_conn_close(conn);
        sql_conn_free(conn);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    close_db(t, db);
    if (testing_t_failed(t))
        goto out;

    /* Ping */
    memset(&fc, 0, sizeof fc);
    db = many_err_bad_conn_setup(t, &fc, NULL, NULL);
    if (db == NULL)
        goto out;
    err = sql_db_ping_context(db, ctx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    close_db(t, db);
out:
    arena_free(&ar);
}

static void TestManyErrBadConn(TestingT *t) {
    synctest_test(t, testManyErrBadConn);
}

static Chan *td_send_query;
static ContextCancelFunc td_rollback_cancel;

static void td_grab_conn_hook(void) {
    BURROW_CALLF0(td_rollback_cancel);
    bool v = false;
    (void)chan_recv(td_send_query, &v);
}

static void td_rollback_hook(void) {
    chan_close(td_send_query);
}

/* Scans the one tx_status row of query on tx, or the db when tx is NULL. */
static bool tx_status(TestingT *t, SqlDB *db, SqlTx *tx, const char *want) {
    Str status = BURROW_STR_EMPTY;
    Str q = BURROW_S("SELECT|tx_status|tx_status|");
    SqlRow row = tx != NULL ? sql_tx_query_row(tx, q, NO_ARGS)
                            : sql_db_query_row(db, q, NO_ARGS);
    Error err = sql_row_scan_v(&row, heap_allocator(), &status);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        return false;
    }
    bool ok = str_eq(status, str_from_cstr(want));
    if (!ok)
        testing_t_errorf_v(t, "tx_status=%q, wanted %q", status, str_from_cstr(want));
    mem_free(heap_allocator(), (void *)(uintptr_t)status.p, (size_t)status.len, 1);
    return ok;
}

/* Issue 34775: Ensure that a Tx cannot commit after a rollback. */
static void testTxCannotCommitAfterRollback(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "tx_status"))
        return;

    /* First check query reporting is correct. */
    if (!tx_status(t, db, NULL, "autocommit"))
        return;

    Context ctx = context_with_cancel(heap_allocator(), context_background(),
                                      &td_rollback_cancel);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(db, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(td_rollback_cancel);
        FATALF("%v", err);
    }

    /* Ignore dirty session for this test. A failing test should trigger the
     * dirty session flag as well, but that isn't exactly what this should test
     * for. */
    fdb_get_fake_conn(burrow__sql_tx_driver(tx))->skip_dirty_session = true;

    if (tx_status(t, db, tx, "transaction")) {
        /* 1. Begin a transaction.
         * 2. (A) Start a query, (B) begin Tx rollback through a ctx cancel.
         * 3. Check if 2.A has committed in Tx (pass) or outside of Tx (fail). */
        td_send_query = chan_make(heap_allocator(), TYPE_BOOL, 0);
        /* The Tx status is returned through the row results, ensure that the
         * rows results are not canceled. */
        burrow__sql_bypass_rows_await_done = true;
        burrow__sql_hook_tx_grab_conn = td_grab_conn_hook;
        burrow__sql_rollback_hook = td_rollback_hook;

        /* A failure here would be expected if skipDirtySession was not set to
         * true above. */
        (void)tx_status(t, db, tx, "transaction");

        burrow__sql_hook_tx_grab_conn = NULL;
        burrow__sql_rollback_hook = NULL;
        burrow__sql_bypass_rows_await_done = false;
        chan_free(td_send_query);
        td_send_query = NULL;
    }
    (void)sql_tx_rollback(tx);
    sql_tx_free(tx);
    BURROW_CALLF0(td_rollback_cancel);
}

static void TestTxCannotCommitAfterRollback(TestingT *t) {
    test_database(t, testTxCannotCommitAfterRollback, NULL);
}

/* Issue 40985 transaction statement deadlock while context cancel. */
static void testTxStmtDeadlock(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(db, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }

    SqlStmt *stmt = sql_tx_prepare(tx, BURROW_S("SELECT|people|name,age|age=?"), &err);
    if (BURROW_FAILED(err)) {
        sql_tx_free(tx);
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }
    BURROW_CALLF0(cancel);
    /* Run number of stmt queries to reproduce deadlock from context cancel. */
    for (int i = 0; i < 1000; i++) {
        /* Encounter any close related errors (e.g. ErrTxDone, stmt is closed)
         * is expected due to context cancel. */
        SqlRows *rows = sql_stmt_query(stmt, ARGS((Int)1), &err);
        if (BURROW_FAILED(err))
            break;
        sql_rows_free(rows);
    }
    (void)sql_tx_rollback(tx);
    sql_stmt_free(stmt);
    sql_tx_free(tx);
}

static void TestTxStmtDeadlock(TestingT *t) {
    test_database(t, testTxStmtDeadlock, NULL);
}

typedef struct TdFresh {
    TestingT *t;
    SqlDB *db;
    Chan *after_put_conn;
    bool bad_reset;
} TdFresh;

static void td_fresh_conn(void *env) {
    TdFresh *f = env;
    Error err = BURROW_NO_ERROR;
    void *conn = burrow__sql_db_conn(f->db, context_background(), true, &err);
    if (!BURROW_FAILED(err))
        burrow__sql_db_put_conn(conn, err, false);
    else
        testing_t_errorf_v(f->t, "db.conn: %v", err);
    chan_close(f->after_put_conn);
}

static void td_fresh_bubble(void *env) {
    TdFresh *f = env;
    TestingT *t = f->t;
    Context ctx = context_background();

    FakeConnector fc;
    memset(&fc, 0, sizeof fc);
    SqlDB *db = new_test_db(t, &fc, "magicquery");
    if (db == NULL)
        return;
    f->db = db;

    sql_db_set_max_open_conns(db, 1);

    clear_all_conns(t, db);

    sql_db_set_max_idle_conns(db, 1);
    sql_db_set_conn_max_lifetime(db, 10 * TIME_SECOND);

    Error err = BURROW_NO_ERROR;
    void *conn = burrow__sql_db_conn(db, ctx, true, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        close_db(t, db);
        return;
    }

    f->after_put_conn = chan_make(heap_allocator(), TYPE_BOOL, 0);
    (void)go(BURROW_FN(Func, td_fresh_conn, f));
    synctest_wait();

    if (!testing_t_failed(t)) {
        synctest_sleep(11 * TIME_SECOND);

        fdb_get_fake_conn(burrow__sql_dc_driver(conn))->sticky_bad = f->bad_reset;

        burrow__sql_db_put_conn(conn, err, true);
    } else {
        burrow__sql_db_put_conn(conn, err, true);
    }

    bool v = false;
    (void)chan_recv(f->after_put_conn, &v);
    chan_free(f->after_put_conn);
    close_db(t, db);
}

static void td_fresh_sub(void *env, TestingT *t) {
    TdFresh *f = env;
    f->t = t;
    if (!synctest_run(BURROW_FN(Func, td_fresh_bubble, f)))
        testing_t_errorf_v(t, "synctest_run failed");
}

/* Issue32530 encounters an issue where a connection may expire right after it
 * comes out of a used connection pool even when a new connection is
 * requested. */
static void TestConnExpiresFreshOutOfPool(TestingT *t) {
    static const struct {
        bool expired;
        bool bad_reset;
    } exec_cases[] = {
        {false, false},
        {true, false},
        {false, true},
    };
    for (size_t i = 0; i < sizeof exec_cases / sizeof exec_cases[0]; i++) {
        char name[64];
        snprintf(name, sizeof name, "expired=%s,badReset=%s",
                 exec_cases[i].expired ? "true" : "false",
                 exec_cases[i].bad_reset ? "true" : "false");
        TdFresh f = {NULL, NULL, NULL, exec_cases[i].bad_reset};
        (void)testing_t_run(t, str_from_cstr(name),
                            BURROW_FN(TestingTFunc, td_fresh_sub, &f));
    }
}

/* TestIssue20575 ensures the Rows from query does not block closing a
 * transaction. Ensure Rows is closed while closing a transaction. */
static void testIssue20575(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       3 * TIME_SECOND, &cancel);
    SqlRows *rows = sql_tx_query_context(tx, ctx, BURROW_S("SELECT|people|age,name|"),
                                         NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        sql_tx_free(tx);
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }
    /* Do not close Rows from QueryContext. */
    err = sql_tx_rollback(tx);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        bool done = false, ok = false;
        bool v = false;
        done = chan_try_recv(context_done(ctx), &v, &ok);
        if (done)
            testing_t_errorf_v(
                t, "timeout: failed to rollback query without closing rows: %v",
                context_err(ctx));
    }
    sql_rows_free(rows);
    sql_tx_free(tx);
    BURROW_CALLF0(cancel);
}

static void TestIssue20575(TestingT *t) {
    test_database(t, testIssue20575, NULL);
}

/* TestIssue20622 tests closing the transaction before rows is closed, requires
 * the race detector to fail. */
static void testIssue20622(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(db, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }

    SqlRows *rows =
        sql_tx_query(tx, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        sql_tx_free(tx);
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Int count = 0;
    while (sql_rows_next(rows)) {
        count++;
        Int age = 0;
        Str name = BURROW_STR_EMPTY;
        err = sql_rows_scan_v(rows, arena_allocator(&ar), &age, &name);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "scan failed %v", err);
            break;
        }

        if (count == 1)
            BURROW_CALLF0(cancel);
        time_sleep(100 * TIME_MILLISECOND);
    }
    (void)sql_rows_close(rows);
    (void)sql_tx_commit(tx);
    sql_rows_free(rows);
    sql_tx_free(tx);
    arena_free(&ar);
    BURROW_CALLF0(cancel);
}

static void TestIssue20622(TestingT *t) {
    test_database(t, testIssue20622, NULL);
}

/* simulateBadConn's state, which Go keeps in the closure. */
static bool td_broken, td_retried;

static bool td_bad_once(void) {
    if (!td_broken) {
        td_broken = true;
        return true;
    }
    td_retried = true;
    return false;
}

static bool td_bad_always(void) {
    if (!td_broken)
        td_broken = true;
    return td_broken;
}

typedef struct TdBadOp {
    SqlDB *db;
    SqlStmt *stmt;
    bool commit;
} TdBadOp;

static Error td_db_exec(TdBadOp *op, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec_v(op->db, a, &err, BURROW_S("INSERT|t1|name=?,age=?,dead=?"),
                        BURROW_S("Gordon"), (Int)3, true);
    return err;
}

static Error td_db_query(TdBadOp *op, Alloc *a) {
    (void)a;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(op->db, BURROW_S("SELECT|t1|age,name|"), NO_ARGS, &err);
    if (!BURROW_FAILED(err)) {
        err = sql_rows_close(rows);
        sql_rows_free(rows);
    }
    return err;
}

static Error td_db_prepare(TdBadOp *op, Alloc *a) {
    (void)a;
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt =
        sql_db_prepare(op->db, BURROW_S("INSERT|t1|name=?,age=?,dead=?"), &err);
    if (BURROW_FAILED(err))
        return err;
    (void)sql_stmt_close(stmt);
    sql_stmt_free(stmt);
    return BURROW_NO_ERROR;
}

static Error td_stmt_exec(TdBadOp *op, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    (void)sql_stmt_exec(op->stmt, a, ARGS(BURROW_S("Gopher"), (Int)3, false), &err);
    return err;
}

static Error td_stmt_query(TdBadOp *op, Alloc *a) {
    (void)a;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_stmt_query(op->stmt, NO_ARGS, &err);
    if (!BURROW_FAILED(err)) {
        err = sql_rows_close(rows);
        sql_rows_free(rows);
    }
    return err;
}

static void simulate_bad_conn(TestingT *t, SqlDB *db, const char *name,
                              bool (**hook)(void), Error (*op)(TdBadOp *, Alloc *),
                              TdBadOp *env) {
    td_broken = false;
    td_retried = false;
    Int num_open = burrow__sql_db_num_open(db);

    /* simulate a broken connection on the first try */
    *hook = td_bad_once;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = op(env, arena_allocator(&ar));
    *hook = NULL;
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: %v", name, err);
        arena_free(&ar);
        return;
    }
    arena_free(&ar);

    if (!td_broken || !td_retried)
        testing_t_errorf_v(t, "%s: Failed to simulate broken connection", name);

    Int now = burrow__sql_db_num_open(db);
    if (num_open != now)
        testing_t_errorf_v(t, "%s: leaked %d connection(s)!", name, now - num_open);
}

/* golang.org/issue/5718 */
static void testErrBadConnReconnect(TestingT *t, SqlDB *db) {
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;

    TdBadOp op = {db, NULL, false};

    /* db.Exec */
    simulate_bad_conn(t, db, "db.Exec prepare", &fdb_hook_prepare_bad_conn, td_db_exec,
                      &op);
    simulate_bad_conn(t, db, "db.Exec exec", &fdb_hook_exec_bad_conn, td_db_exec, &op);

    /* db.Query */
    simulate_bad_conn(t, db, "db.Query prepare", &fdb_hook_prepare_bad_conn,
                      td_db_query, &op);
    simulate_bad_conn(t, db, "db.Query query", &fdb_hook_query_bad_conn, td_db_query,
                      &op);

    /* db.Prepare */
    simulate_bad_conn(t, db, "db.Prepare", &fdb_hook_prepare_bad_conn, td_db_prepare,
                      &op);

    /* stmt.Exec */
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt1 =
        sql_db_prepare(db, BURROW_S("INSERT|t1|name=?,age=?,dead=?"), &err);
    if (BURROW_FAILED(err))
        FATALF("prepare: %v", err);
    /* make sure we must prepare the stmt first */
    burrow__sql_stmt_force_prepare(stmt1);

    op.stmt = stmt1;
    simulate_bad_conn(t, db, "stmt.Exec prepare", &fdb_hook_prepare_bad_conn,
                      td_stmt_exec, &op);
    simulate_bad_conn(t, db, "stmt.Exec exec", &fdb_hook_exec_bad_conn, td_stmt_exec,
                      &op);

    /* stmt.Query */
    SqlStmt *stmt2 = sql_db_prepare(db, BURROW_S("SELECT|t1|age,name|"), &err);
    if (BURROW_FAILED(err)) {
        sql_stmt_free(stmt1);
        FATALF("prepare: %v", err);
    }
    /* make sure we must prepare the stmt first */
    burrow__sql_stmt_force_prepare(stmt2);

    op.stmt = stmt2;
    simulate_bad_conn(t, db, "stmt.Query prepare", &fdb_hook_prepare_bad_conn,
                      td_stmt_query, &op);
    simulate_bad_conn(t, db, "stmt.Query exec", &fdb_hook_query_bad_conn, td_stmt_query,
                      &op);
    sql_stmt_free(stmt2);
    sql_stmt_free(stmt1);
}

static void TestErrBadConnReconnect(TestingT *t) {
    test_database(t, testErrBadConnReconnect, NULL);
}

static Error td_end_tx(TdBadOp *op, SqlTx *tx) {
    return op->commit ? sql_tx_commit(tx) : sql_tx_rollback(tx);
}

static Error td_tx_exec(TdBadOp *op, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(op->db, &err);
    if (BURROW_FAILED(err))
        return err;
    (void)sql_tx_exec(tx, a, BURROW_S("INSERT|t1|name=?,age=?,dead=?"),
                      ARGS(BURROW_S("Gordon"), (Int)3, true), &err);
    if (!BURROW_FAILED(err))
        err = td_end_tx(op, tx);
    sql_tx_free(tx);
    return err;
}

static Error td_tx_query(TdBadOp *op, Alloc *a) {
    (void)a;
    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin(op->db, &err);
    if (BURROW_FAILED(err))
        return err;
    SqlRows *rows = sql_tx_query(tx, BURROW_S("SELECT|t1|age,name|"), NO_ARGS, &err);
    if (!BURROW_FAILED(err)) {
        (void)sql_rows_close(rows);
        sql_rows_free(rows);
        err = td_end_tx(op, tx);
    }
    sql_tx_free(tx);
    return err;
}

static void simulate_bad_tx_end(TestingT *t, SqlDB *db, const char *name,
                                bool (**hook)(void), Error (*op)(TdBadOp *, Alloc *),
                                TdBadOp *env) {
    td_broken = false;
    Int num_open = burrow__sql_db_num_open(db);

    *hook = td_bad_always;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = op(env, arena_allocator(&ar));
    *hook = NULL;
    if (!errors_is(err, driver_err_bad_conn)) {
        testing_t_errorf_v(t, "%s: %v", name, err);
        arena_free(&ar);
        return;
    }
    arena_free(&ar);

    if (!td_broken)
        testing_t_errorf_v(t, "%s: Failed to simulate broken connection", name);

    Int now = burrow__sql_db_num_open(db);
    if (num_open != now)
        testing_t_errorf_v(t, "%s: leaked %d connection(s)!", name, now - num_open);
}

/* golang.org/issue/11264 */
static void testTxEndBadConn(TestingT *t, SqlDB *db) {
    sql_db_set_max_idle_conns(db, 0);
    if (!EXEC(t, db, "CREATE|t1|name=string,age=int32,dead=bool"))
        return;
    sql_db_set_max_idle_conns(db, 1);

    TdBadOp commit = {db, NULL, true}, rollback = {db, NULL, false};

    /* db.Exec */
    simulate_bad_tx_end(t, db, "db.Tx.Exec commit", &fdb_hook_commit_bad_conn,
                        td_tx_exec, &commit);
    simulate_bad_tx_end(t, db, "db.Tx.Exec rollback", &fdb_hook_rollback_bad_conn,
                        td_tx_exec, &rollback);

    /* db.Query */
    simulate_bad_tx_end(t, db, "db.Tx.Query commit", &fdb_hook_commit_bad_conn,
                        td_tx_query, &commit);
    simulate_bad_tx_end(t, db, "db.Tx.Query rollback", &fdb_hook_rollback_bad_conn,
                        td_tx_query, &rollback);
}

static void TestTxEndBadConn(TestingT *t) {
    test_database(t, testTxEndBadConn, NULL);
}

/* ------------------------------------------------------------ concurrency */

/* concurrentTest, one kind per Go type. */
typedef enum TdCtKind {
    TD_CT_DB_QUERY,
    TD_CT_DB_EXEC,
    TD_CT_STMT_QUERY,
    TD_CT_STMT_EXEC,
    TD_CT_TX_QUERY,
    TD_CT_TX_EXEC,
    TD_CT_TX_STMT_QUERY,
    TD_CT_TX_STMT_EXEC,
    TD_CT_RANDOM,
} TdCtKind;

/* The kinds concurrentRandomTest picks from, which is all but itself. */
#define TD_CT_NKIND 8

typedef struct TdCt {
    TdCtKind kind;
    SqlDB *db;
    SqlStmt *stmt;
    SqlTx *tx;
    struct TdCt *tests; /* concurrentRandomTest's, TD_CT_NKIND of them */
} TdCt;

static const char td_ct_select[] = "SELECT|people|name|";
static const char td_ct_nosert[] =
    "NOSERT|people|name=Chris,age=?,photo=CPHOTO,bdate=?";

static bool td_ct_init(TestingT *t, TdCt *c, SqlDB *db) {
    Error err = BURROW_NO_ERROR;
    c->db = db;
    switch (c->kind) {
    case TD_CT_DB_QUERY:
    case TD_CT_DB_EXEC:
        return true;
    case TD_CT_STMT_QUERY:
        c->stmt = sql_db_prepare(db, str_from_cstr(td_ct_select), &err);
        break;
    case TD_CT_STMT_EXEC:
        c->stmt = sql_db_prepare(db, str_from_cstr(td_ct_nosert), &err);
        break;
    case TD_CT_TX_QUERY:
    case TD_CT_TX_EXEC:
        c->tx = sql_db_begin(db, &err);
        break;
    case TD_CT_TX_STMT_QUERY:
    case TD_CT_TX_STMT_EXEC:
        c->tx = sql_db_begin(db, &err);
        if (BURROW_OK(err))
            c->stmt = sql_tx_prepare(c->tx,
                                     str_from_cstr(c->kind == TD_CT_TX_STMT_QUERY
                                                       ? td_ct_select
                                                       : td_ct_nosert),
                                     &err);
        break;
    case TD_CT_RANDOM:
        for (int i = 0; i < TD_CT_NKIND; i++) {
            c->tests[i].kind = (TdCtKind)i;
            if (!td_ct_init(t, &c->tests[i], db))
                return false;
        }
        return true;
    default:
        break;
    }
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        return false;
    }
    return true;
}

static void td_ct_finish(TdCt *c) {
    if (c->kind == TD_CT_RANDOM) {
        for (int i = 0; i < TD_CT_NKIND; i++)
            td_ct_finish(&c->tests[i]);
    }
    if (c->stmt != NULL) {
        (void)sql_stmt_close(c->stmt);
        sql_stmt_free(c->stmt);
        c->stmt = NULL;
    }
    if (c->tx != NULL) {
        (void)sql_tx_rollback(c->tx);
        sql_tx_free(c->tx);
        c->tx = NULL;
    }
    c->db = NULL;
}

/* Reads every name, then closes rows. */
static void td_ct_drain(SqlRows *rows) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str name = BURROW_STR_EMPTY;
    while (sql_rows_next(rows))
        (void)sql_rows_scan_v(rows, arena_allocator(&ar), &name);
    (void)sql_rows_close(rows);
    sql_rows_free(rows);
    arena_free(&ar);
}

static Error td_ct_test(TestingT *t, TdCt *c) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = NULL;
    Time bday = chris_birthday();
    Arena ar;
    switch (c->kind) {
    case TD_CT_DB_QUERY:
    case TD_CT_TX_QUERY:
        rows = sql_db_query(c->db, str_from_cstr(td_ct_select), NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            return err;
        }
        td_ct_drain(rows);
        return BURROW_NO_ERROR;
    case TD_CT_STMT_QUERY:
    case TD_CT_TX_STMT_QUERY:
        rows = sql_stmt_query(c->stmt, NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "error on query:  %v", err);
            return err;
        }
        td_ct_drain(rows);
        return BURROW_NO_ERROR;
    case TD_CT_DB_EXEC:
    case TD_CT_TX_EXEC:
    case TD_CT_STMT_EXEC:
    case TD_CT_TX_STMT_EXEC:
        arena_init(&ar, heap_allocator(), 0);
        if (c->kind == TD_CT_DB_EXEC)
            (void)sql_db_exec(c->db, arena_allocator(&ar), str_from_cstr(td_ct_nosert),
                              ARGS((Int)3, ((Any){TYPE_TIME, &bday})), &err);
        else if (c->kind == TD_CT_TX_EXEC)
            (void)sql_tx_exec(c->tx, arena_allocator(&ar), str_from_cstr(td_ct_nosert),
                              ARGS((Int)3, ((Any){TYPE_TIME, &bday})), &err);
        else
            (void)sql_stmt_exec(c->stmt, arena_allocator(&ar),
                                ARGS((Int)3, ((Any){TYPE_TIME, &bday})), &err);
        arena_free(&ar);
        if (BURROW_FAILED(err)) {
            if (c->kind == TD_CT_DB_EXEC || c->kind == TD_CT_TX_EXEC)
                testing_t_errorf_v(t, "%v", err);
            else
                testing_t_errorf_v(t, "error on exec:  %v", err);
        }
        return err;
    case TD_CT_RANDOM:
        return td_ct_test(t, &c->tests[math_rand_intn(TD_CT_NKIND)]);
    default:
        break;
    }
    return err;
}

typedef struct TdCtRun {
    TestingT *t;
    TdCt *ct;
    Chan *reqs;
    SyncWaitGroup wg;
} TdCtRun;

static void td_ct_worker(void *env) {
    TdCtRun *r = env;
    bool v = false;
    while (chan_recv(r->reqs, &v)) {
        (void)td_ct_test(r->t, r->ct);
        sync_wait_group_done(&r->wg);
    }
}

/* doConcurrentTest. Go also sets GOMAXPROCS to maxProcs, which
 * runtime_gomaxprocs cannot do once the scheduler is running, so here maxProcs
 * only sets the number of workers. */
static void td_ct_bubble(void *env) {
    TdCtRun *r = env;
    TestingT *t = r->t;
    int max_procs = 1, num_reqs = 500;
    if (testing_short()) {
        max_procs = 4;
        num_reqs = 50;
    }

    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;

    if (td_ct_init(t, r->ct, db)) {
        memset(&r->wg, 0, sizeof r->wg);
        sync_wait_group_add(&r->wg, num_reqs);

        r->reqs = chan_make(heap_allocator(), TYPE_BOOL, 0);
        SyncWaitGroup workers;
        memset(&workers, 0, sizeof workers);
        for (int i = 0; i < max_procs * 2; i++)
            (void)sync_wait_group_go(&workers, BURROW_FN(Func, td_ct_worker, r));

        for (int i = 0; i < num_reqs; i++)
            chan_send(r->reqs, &(bool){true});

        sync_wait_group_wait(&r->wg);
        chan_close(r->reqs);
        sync_wait_group_wait(&workers);
        chan_free(r->reqs);
    }
    td_ct_finish(r->ct);
    close_db(t, db);
}

static void td_ct_sub(void *env, TestingT *t) {
    TdCtRun *r = env;
    r->t = t;
    if (!synctest_run(BURROW_FN(Func, td_ct_bubble, r)))
        testing_t_errorf_v(t, "synctest_run failed");
}

/* ------------------------------------------------------------ issues */

static void td_bad_conn_on_close(SqlRows *rs, Error *err) {
    (void)rs;
    *err = driver_err_bad_conn;
}

static void testIssue6081(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Int opens0 = 0, closes0 = 0;
    driver_counts(&opens0, &closes0);

    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT|people|name|"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    burrow__sql_rows_close_hook = td_bad_conn_on_close;
    for (int i = 0; i < 10; i++) {
        SqlRows *rows = sql_stmt_query(stmt, NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            burrow__sql_rows_close_hook = NULL;
            sql_stmt_free(stmt);
            FATALF("%v", err);
        }
        (void)sql_rows_close(rows);
        sql_rows_free(rows);
    }
    burrow__sql_rows_close_hook = NULL;
    Int n = burrow__sql_stmt_num_css(stmt);
    if (n > 1)
        testing_t_errorf_v(t, "len(css slice) = %d; want <= 1", n);
    (void)sql_stmt_close(stmt);
    n = burrow__sql_stmt_num_css(stmt);
    if (n != 0)
        testing_t_errorf_v(t, "len(css slice) after Close = %d; want 0", n);
    sql_stmt_free(stmt);

    Int opens = 0, closes = 0;
    driver_counts(&opens, &closes);
    opens -= opens0;
    closes -= closes0;
    if (opens < 9)
        testing_t_errorf_v(t, "opens = %d; want >= 9", opens);
    if (closes < 9)
        testing_t_errorf_v(t, "closes = %d; want >= 9", closes);
}

static void TestIssue6081(TestingT *t) {
    test_database(t, testIssue6081, NULL);
}

#define TD_MILLI_WAIT 30

typedef struct TdStress {
    SqlDB *db;
    SqlStmt *stmt;
    Chan *sem;
} TdStress;

static Duration td_rand_wait(void) {
    return (Duration)math_rand_intn(TD_MILLI_WAIT) * TIME_MILLISECOND;
}

static void td_18429_one(void *env) {
    TdStress *s = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str qwait = duration_string(td_rand_wait(), a);

    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       td_rand_wait(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(s->db, ctx, NULL, &err);
    if (BURROW_OK(err)) {
        char q[64];
        snprintf(q, sizeof q, "WAIT|%.*s|SELECT|people|name|", (int)qwait.len,
                 (const char *)qwait.p);
        /* This is expected to give a cancel error most, but not all the time.
         * Test failure will happen with a panic or other race condition being
         * reported. */
        SqlRows *rows = sql_tx_query_context(tx, ctx, str_from_cstr(q), NO_ARGS, &err);
        if (rows != NULL) {
            Str name = BURROW_STR_EMPTY;
            /* Call Next to test Issue 21117 and check for races. */
            while (sql_rows_next(rows)) {
                /* Scan the buffer so it is read and checked for races. */
                (void)sql_rows_scan_v(rows, a, &name);
            }
            (void)sql_rows_close(rows);
            sql_rows_free(rows);
        }
        /* This call will race with the context cancel rollback to complete if
         * the rollback itself isn't guarded. */
        (void)sql_tx_rollback(tx);
        sql_tx_free(tx);
    }
    BURROW_CALLF0(cancel);
    arena_free(&ar);
    bool v = false;
    (void)chan_recv(s->sem, &v);
}

/* TestIssue18429 attempts to stress rolling back the transaction from a context
 * cancel while simultaneously calling Tx.Rollback. Rolling back from a context
 * happens concurrently so tx.rollback and tx.Commit must guard against double
 * entry.
 *
 * In the test, a context is canceled while the query is in process so the
 * internal rollback will run concurrently with the explicitly called
 * Tx.Rollback.
 *
 * The addition of calling rows.Next also tests Issue 21117. */
static void testIssue18429(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    TdStress s = {db, NULL, chan_make(heap_allocator(), TYPE_BOOL, 20)};
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    for (int i = 0; i < 100; i++) {
        chan_send(s.sem, &(bool){true});
        (void)sync_wait_group_go(&wg, BURROW_FN(Func, td_18429_one, &s));
    }
    sync_wait_group_wait(&wg);
    chan_free(s.sem);
}

static void TestIssue18429(TestingT *t) {
    test_database(t, testIssue18429, NULL);
}

static void td_20160_one(void *env) {
    TdStress *s = env;
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       td_rand_wait(), &cancel);

    /* This is expected to give a cancel error most, but not all the time.
     * Test failure will happen with a panic or other race condition being
     * reported. */
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_stmt_query_context(s->stmt, ctx, NO_ARGS, &err);
    if (rows != NULL) {
        (void)sql_rows_close(rows);
        sql_rows_free(rows);
    }
    BURROW_CALLF0(cancel);
    bool v = false;
    (void)chan_recv(s->sem, &v);
}

/* TestIssue20160 attempts to test a short context life on a stmt Query. */
static void testIssue20160(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare_context(db, context_background(),
                                           BURROW_S("SELECT|people|name|"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    TdStress s = {db, stmt, chan_make(heap_allocator(), TYPE_BOOL, 20)};
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    for (int i = 0; i < 100; i++) {
        chan_send(s.sem, &(bool){true});
        (void)sync_wait_group_go(&wg, BURROW_FN(Func, td_20160_one, &s));
    }
    sync_wait_group_wait(&wg);
    chan_free(s.sem);
    sql_stmt_free(stmt);
}

static void TestIssue20160(TestingT *t) {
    test_database(t, testIssue20160, NULL);
}

/* hookTxGrabConn's closure. */
static TestingT *td_18719_t;
static SqlTx *td_18719_tx;
static ContextCancelFunc td_18719_cancel;

static void td_18719_grab(void) {
    BURROW_CALLF0(td_18719_cancel);
    synctest_wait();
    if (!burrow__sql_tx_is_done(td_18719_tx))
        testing_t_errorf_v(td_18719_t, "tx is not done");
}

/* TestIssue18719 closes the context right before use. The sql.driverConn will
 * nil out the ci on close in a lock, but if another process uses it right after
 * it will panic with on the nil ref.
 *
 * See https://golang.org/cl/35550 . */
static void testIssue18719(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlTx *tx = sql_db_begin_tx(db, ctx, NULL, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }

    td_18719_t = t;
    td_18719_tx = tx;
    td_18719_cancel = cancel;
    burrow__sql_hook_tx_grab_conn = td_18719_grab;

    /* This call will grab the connection and cancel the context after it has
     * done so. Code after must deal with the canceled state. */
    SqlRows *rows =
        sql_tx_query_context(tx, ctx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    burrow__sql_hook_tx_grab_conn = NULL;
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error <nil> but got %v", err);

    /* Rows may be ignored because it will be closed when the context is
     * canceled. Do not explicitly rollback. The rollback will happen from the
     * canceled context. */
    BURROW_CALLF0(cancel);
    synctest_wait();
    if (rows != NULL)
        sql_rows_free(rows);
    sql_tx_free(tx);
}

static void TestIssue18719(TestingT *t) {
    test_database(t, testIssue18719, "BeginTx");
}

static void testIssue20647(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Context ctx = testing_t_context(t);

    Error err = BURROW_NO_ERROR;
    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    fdb_get_fake_conn(burrow__sql_conn_driver(conn))->skip_dirty_session = true;

    SqlStmt *stmt =
        sql_conn_prepare_context(conn, ctx, BURROW_S("SELECT|people|name|"), &err);
    SqlRows *rows1 = NULL, *rows2 = NULL;
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }

    rows1 = sql_stmt_query_context(stmt, ctx, NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "rows1 %v", err);
        goto out;
    }

    rows2 = sql_stmt_query_context(stmt, ctx, NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "rows2 %v", err);
        goto out;
    }

    if (burrow__sql_rows_dc(rows1) != burrow__sql_rows_dc(rows2))
        testing_t_errorf_v(t, "stmt prepared on Conn does not use same connection");

out:
    if (rows2 != NULL)
        sql_rows_free(rows2);
    if (rows1 != NULL)
        sql_rows_free(rows1);
    if (stmt != NULL)
        sql_stmt_free(stmt);
    sql_conn_free(conn);
}

static void TestIssue20647(TestingT *t) {
    test_database(t, testIssue20647, NULL);
}

static void TestConcurrency(TestingT *t) {
    static const struct {
        const char *name;
        TdCtKind kind;
    } list[] = {
        {"Query", TD_CT_DB_QUERY},
        {"Exec", TD_CT_DB_EXEC},
        {"StmtQuery", TD_CT_STMT_QUERY},
        {"StmtExec", TD_CT_STMT_EXEC},
        {"TxQuery", TD_CT_TX_QUERY},
        {"TxExec", TD_CT_TX_EXEC},
        {"TxStmtQuery", TD_CT_TX_STMT_QUERY},
        {"TxStmtExec", TD_CT_TX_STMT_EXEC},
        {"Random", TD_CT_RANDOM},
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        TdCt tests[TD_CT_NKIND];
        memset(tests, 0, sizeof tests);
        TdCt ct;
        memset(&ct, 0, sizeof ct);
        ct.kind = list[i].kind;
        ct.tests = tests;
        TdCtRun r;
        memset(&r, 0, sizeof r);
        r.ct = &ct;
        (void)testing_t_run(t, str_from_cstr(list[i].name),
                            BURROW_FN(TestingTFunc, td_ct_sub, &r));
    }
}

/* Go's defaultMaxIdleConns. */
#define TD_DEFAULT_MAX_IDLE 2

typedef struct TdLeak {
    TestingT *t;
    SqlDB *db;
} TdLeak;

static void td_leak_query(void *env) {
    TdLeak *l = env;
    Error err = BURROW_NO_ERROR;
    SqlRows *r = sql_db_query(l->db, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(l->t, "%v", err);
        return;
    }
    (void)sql_rows_close(r);
    sql_rows_free(r);
}

static void testConnectionLeak(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    /* Start by opening defaultMaxIdleConns. */
    SqlRows *rows[TD_DEFAULT_MAX_IDLE] = {NULL, NULL};
    /* We need to SetMaxOpenConns > MaxIdleConns, so the DB can open a new
     * connection and we can fill the idle queue with the released
     * connections. */
    sql_db_set_max_open_conns(db, TD_DEFAULT_MAX_IDLE + 1);
    Error err = BURROW_NO_ERROR;
    for (int ii = 0; ii < TD_DEFAULT_MAX_IDLE; ii++) {
        SqlRows *r = sql_db_query(db, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            goto out;
        }
        rows[ii] = r;
        (void)sql_rows_next(r);
        err = sql_rows_err(r);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            goto out;
        }
    }
    /* Now we have defaultMaxIdleConns busy connections. Open a new one, but
     * wait until the busy connections are released before returning control
     * to DB. The driver forgets the channels once it has used them, so they
     * are kept here to free. */
    Chan *wait_ch = chan_make(heap_allocator(), TYPE_BYTE, 1);
    Chan *waiting_ch = chan_make(heap_allocator(), TYPE_BYTE, 1);
    fdb_wait_ch = wait_ch;
    fdb_waiting_ch = waiting_ch;
    TdLeak l = {t, db};
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    (void)sync_wait_group_go(&wg, BURROW_FN(Func, td_leak_query, &l));
    /* Wait until the goroutine we've just created has started waiting. */
    (void)chan_recv(waiting_ch, NULL);
    /* Now close the busy connections. This provides a connection for the
     * blocked goroutine and then fills up the idle queue. */
    for (int ii = 0; ii < TD_DEFAULT_MAX_IDLE; ii++) {
        sql_rows_free(rows[ii]);
        rows[ii] = NULL;
    }
    /* At this point we give the new connection to DB. This connection is now
     * useless, since the idle queue is full and there are no pending requests.
     * DB should deal with this situation without leaking the connection. */
    chan_send(wait_ch, &(Byte){0});
    sync_wait_group_wait(&wg);
    chan_free(wait_ch);
    chan_free(waiting_ch);

out:
    for (int ii = 0; ii < TD_DEFAULT_MAX_IDLE; ii++) {
        if (rows[ii] != NULL)
            sql_rows_free(rows[ii]);
    }
}

static void TestConnectionLeak(TestingT *t) {
    test_database(t, testConnectionLeak, NULL);
}

/* Runs ten queries one at a time and reports how many more connections were
 * closed for being over the idle limit than before. */
static int64_t td_ten_queries(TestingT *t, SqlDB *db) {
    int64_t pre = sql_db_stats(db).max_idle_closed;
    for (int i = 0; i < 10; i++) {
        Error err = BURROW_NO_ERROR;
        SqlRows *rows =
            sql_db_query(db, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            return -1;
        }
        (void)sql_rows_close(rows);
        sql_rows_free(rows);
    }
    int64_t n = sql_db_stats(db).max_idle_closed - pre;
    testing_t_logf_v(t, "MaxIdleClosed: %d", n);
    return n;
}

static void testStatsMaxIdleClosedZero(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    sql_db_set_max_open_conns(db, 1);
    sql_db_set_max_idle_conns(db, 1);
    sql_db_set_conn_max_lifetime(db, 0);

    int64_t n = td_ten_queries(t, db);
    if (n >= 0 && n != 0)
        testing_t_errorf_v(t, "expected 0 max idle closed conns, got: %d", n);
}

static void TestStatsMaxIdleClosedZero(TestingT *t) {
    test_database(t, testStatsMaxIdleClosedZero, NULL);
}

static void testStatsMaxIdleClosedTen(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    sql_db_set_max_open_conns(db, 1);
    sql_db_set_max_idle_conns(db, 0);
    sql_db_set_conn_max_lifetime(db, 0);

    int64_t n = td_ten_queries(t, db);
    if (n >= 0 && n != 10)
        testing_t_errorf_v(t, "expected 10 max idle closed conns, got: %d", n);
}

static void TestStatsMaxIdleClosedTen(TestingT *t) {
    test_database(t, testStatsMaxIdleClosedTen, NULL);
}

/* testUseConns uses count concurrent connections with 1 nanosecond apart. */
static void use_conns(TestingT *t, int count, SqlDB *db) {
    SqlConn *conns[8] = {NULL};
    Context ctx = context_background();
    for (int i = 0; i < count; i++) {
        time_sleep(TIME_NANOSECOND);
        Error err = BURROW_NO_ERROR;
        SqlConn *c = sql_db_conn(db, ctx, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        conns[i] = c;
    }

    for (int i = count - 1; i >= 0; i--) {
        time_sleep(TIME_NANOSECOND);
        if (conns[i] == NULL)
            continue;
        Error err = sql_conn_close(conns[i]);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        sql_conn_free(conns[i]);
    }
}

enum { TD_USED_CONNS = 5, TD_REUSED_CONNS = 2 };

typedef struct TdIdleCase {
    Duration want_max_idle_time;
    Duration want_max_lifetime;
    Duration want_next_check;
    int64_t want_idle_closed;
    int64_t want_max_idle_closed;
    Duration time_offset;
    Duration second_time_offset;
} TdIdleCase;

typedef struct TdIdle {
    TestingT *t;
    const TdIdleCase *item;
} TdIdle;

static void td_idle_bubble(void *env) {
    TdIdle *r = env;
    TestingT *t = r->t;
    const TdIdleCase *item = r->item;

    Time start_time = time_now();
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;

    sql_db_set_max_open_conns(db, TD_USED_CONNS);
    sql_db_set_max_idle_conns(db, TD_USED_CONNS);
    sql_db_set_conn_max_idle_time(db, item->want_max_idle_time);
    sql_db_set_conn_max_lifetime(db, item->want_max_lifetime);

    int64_t pre_max_idle_closed = sql_db_stats(db).max_idle_time_closed;

    /* Busy usedConns. */
    use_conns(t, TD_USED_CONNS, db);

    time_sleep(time_sub(time_add(start_time, item->time_offset), time_now()));

    /* Reuse connections which should never be considered idle and exercises
     * the sorting for issue 39471. */
    use_conns(t, TD_REUSED_CONNS, db);

    time_sleep(item->second_time_offset);

    Int nclosed = 0;
    bool ordered = true;
    Duration nc = burrow__sql_db_cleaner_run(db, TIME_SECOND, &nclosed, &ordered);
    if (nc != item->want_next_check) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        testing_t_errorf_v(
            t, "got %v; want %v next check duration",
            duration_string(nc, arena_allocator(&ar)),
            duration_string(item->want_next_check, arena_allocator(&ar)));
        arena_free(&ar);
    }
    /* Validate freeConn order. */
    if (!ordered)
        testing_t_errorf_v(t, "freeConn is not ordered by returnedAt");
    if ((int64_t)nclosed != item->want_idle_closed)
        testing_t_errorf_v(t, "got: %d; want %d closed conns", nclosed,
                           item->want_idle_closed);

    int64_t max_idle_closed =
        sql_db_stats(db).max_idle_time_closed - pre_max_idle_closed;
    if (max_idle_closed != item->want_max_idle_closed)
        testing_t_errorf_v(t, "got: %d; want %d max idle closed conns", max_idle_closed,
                           item->want_max_idle_closed);
    close_db(t, db);
}

static void td_idle_sub(void *env, TestingT *t) {
    TdIdle *r = env;
    r->t = t;
    if (!synctest_run(BURROW_FN(Func, td_idle_bubble, r)))
        testing_t_errorf_v(t, "synctest_run failed");
}

static void TestMaxIdleTime(TestingT *t) {
    static const TdIdleCase list[] = {
        {
            TIME_MILLISECOND,
            0,
            TIME_MILLISECOND - TIME_NANOSECOND,
            TD_USED_CONNS - TD_REUSED_CONNS,
            TD_USED_CONNS - TD_REUSED_CONNS,
            10 * TIME_MILLISECOND,
            0,
        },
        {
            /* Want to close some connections via max idle time and one by max
             * lifetime. */
            TIME_MILLISECOND,
            /* nowFunc() - MaxLifetime should be 1 * time.Nanosecond in
             * connectionCleanerRunLocked. This guarantees that first opened
             * connection is to be closed. Thus it is timeOffset +
             * secondTimeOffset + 3 (+2 for Close while reusing conns and +1 for
             * Conn). */
            10 * TIME_MILLISECOND + 100 * TIME_NANOSECOND + 3 * TIME_NANOSECOND,
            TIME_NANOSECOND,
            /* Closed all not reused connections and extra one by max
             * lifetime. */
            TD_USED_CONNS - TD_REUSED_CONNS + 1,
            TD_USED_CONNS - TD_REUSED_CONNS,
            10 * TIME_MILLISECOND,
            /* Add second offset because otherwise connections are expired via
             * max lifetime in Close. */
            100 * TIME_NANOSECOND,
        },
        {
            TIME_HOUR,
            0,
            TIME_SECOND,
            0,
            0,
            10 * TIME_MILLISECOND,
            0,
        },
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        TdIdle r = {NULL, &list[i]};
        (void)testing_t_run(
            t, duration_string(list[i].want_max_idle_time, arena_allocator(&ar)),
            BURROW_FN(TestingTFunc, td_idle_sub, &r));
        arena_free(&ar);
    }
}

/* ------------------------------------------------------- driver variants */

/* Closes db and waits for its connections to finish closing before freeing
 * it, for the tests that open a DB by driver name and defer db.Close. */
static void td_close_free(SqlDB *db) {
    (void)sql_db_close(db);
    synctest_wait();
    sql_db_free(db);
}

/* nvcDriver and nvcConn. The conn is the fake one, with CheckNamedValue added
 * by its vtable. */
typedef struct DecimalInt {
    Int value;
} DecimalInt;
extern const Type burrow_type_DecimalInt;
#define DECIMAL_INT_FIELDS(F, T) F(T, Int, value, "")
BURROW_STRUCT_DEFINE(DecimalInt, DECIMAL_INT_FIELDS);

/* Go's doNotInclude is struct{}. A descriptor needs a field, so this has one
 * nobody reads. */
typedef struct DoNotInclude {
    Byte unused;
} DoNotInclude;
extern const Type burrow_type_DoNotInclude;
#define DO_NOT_INCLUDE_FIELDS(F, T) F(T, Byte, unused, "")
BURROW_STRUCT_DEFINE(DoNotInclude, DO_NOT_INCLUDE_FIELDS);

BURROW_SLICE_TYPE(Int64Slice, int64_t);

static Str td_out_string = BURROW_S_INIT("OUT:*string");

static Error td_nvc_check(void *self, Alloc *a, DriverNamedValue *nv) {
    (void)self;
    (void)a;
    const Type *t = nv->value.t;
    if (t == TYPE_OF(SqlOut)) {
        SqlOut *o = nv->value.data;
        if (o->dest.t != TYPE_STRING)
            return fmt_errorf_v("unknown NameValueCheck OUTPUT type");
        *(Str *)o->dest.data = BURROW_S("from-server");
        nv->value = (Any){TYPE_STRING, &td_out_string};
        return BURROW_NO_ERROR;
    }
    if (t == TYPE_OF(DecimalInt) || t == TYPE_OF(Int64Slice))
        return BURROW_NO_ERROR;
    if (t == TYPE_OF(DoNotInclude))
        return driver_err_remove_argument;
    return driver_err_skip;
}

static Error td_nvc_skip_check(void *self, Alloc *a, DriverNamedValue *nv) {
    (void)self;
    (void)a;
    (void)nv;
    return driver_err_skip;
}

static const DriverConnVT td_nvc_conn_vt = {
    .close = fdb_conn_close,
    .begin = fdb_conn_begin,
    .prepare_context = fdb_conn_prepare_context,
    .exec_context = fdb_conn_exec_context,
    .query_context = fdb_conn_query_context,
    .reset_session = fdb_conn_reset_session,
    .is_valid = fdb_conn_is_valid,
    .check_named_value = td_nvc_check,
};

static const DriverConnVT td_nvc_skip_conn_vt = {
    .close = fdb_conn_close,
    .begin = fdb_conn_begin,
    .prepare_context = fdb_conn_prepare_context,
    .exec_context = fdb_conn_exec_context,
    .query_context = fdb_conn_query_context,
    .reset_session = fdb_conn_reset_session,
    .is_valid = fdb_conn_is_valid,
    .check_named_value = td_nvc_skip_check,
};

typedef struct TdNvcDriver {
    bool skip_named_value_check;
} TdNvcDriver;

static DriverConn td_nvc_open(void *self, Str name, Error *err) {
    const TdNvcDriver *d = self;
    FakeConn *c = fdb_open(name, err);
    if (c == NULL)
        return (DriverConn){NULL, NULL};
    c->db->allow_any = true;
    return (DriverConn){
        d->skip_named_value_check ? &td_nvc_skip_conn_vt : &td_nvc_conn_vt, c};
}

static const DriverVT td_nvc_driver_vt = {NULL, td_nvc_open, NULL};
static TdNvcDriver td_nvc = {false}, td_nvc_skip = {true};

static void testNamedValueChecker(TestingT *t) {
    sql_register(BURROW_S("NamedValueCheck"), (Driver){&td_nvc_driver_vt, &td_nvc});
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("NamedValueCheck"), BURROW_STR_EMPTY, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    Context ctx = testing_t_context(t);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    (void)sql_db_exec_context(db, a, ctx, BURROW_S("WIPE"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec wipe %v", err);
        goto out;
    }

    (void)sql_db_exec_context(
        db, a, ctx, BURROW_S("CREATE|keys|dec1=any,str1=string,out1=string,array1=any"),
        NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec create %v", err);
        goto out;
    }

    Str o1 = BURROW_STR_EMPTY;
    DecimalInt dec = {123};
    static int64_t array[] = {42, 128, 707};
    Int64Slice arr = slice_from(array, 3, 3, TYPE_OF(int64_t));
    DoNotInclude dni = {0};
    SqlOut out = {{TYPE_STRING, &o1}, false};
    SqlNamedArg named_a = sql_named(BURROW_S("A"), (Any){TYPE_OF(DecimalInt), &dec});
    SqlNamedArg named_o1 = sql_named(BURROW_S("O1"), (Any){TYPE_OF(SqlOut), &out});
    (void)sql_db_exec_context(
        db, a, ctx, BURROW_S("INSERT|keys|dec1=?A,str1=?,out1=?O1,array1=?"),
        ARGS(sql_named_any(&named_a), BURROW_S("hello"), sql_named_any(&named_o1),
             ((Any){TYPE_OF(Int64Slice), &arr}), ((Any){TYPE_OF(DoNotInclude), &dni})),
        &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec insert %v", err);
        goto out;
    }

    Str str1 = BURROW_STR_EMPTY;
    DecimalInt dec1 = {0};
    Int64Slice arr1 = slice_nil(TYPE_OF(int64_t));
    SqlRow row = sql_db_query_row_context(
        db, ctx, BURROW_S("SELECT|keys|dec1,str1,array1|"), NO_ARGS);
    err = sql_row_scan_v(&row, a, ((Any){TYPE_OF(DecimalInt), &dec1}), &str1,
                         ((Any){TYPE_OF(Int64Slice), &arr1}));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "select %v", err);
        goto out;
    }

    if (!str_eq(o1, BURROW_S("from-server")))
        testing_t_errorf_v(t, "got %q wanted %q for index %d", o1,
                           BURROW_S("from-server"), 0);
    if (dec1.value != 123)
        testing_t_errorf_v(t,
                           "got decimalInt{value:%d} wanted decimalInt{value:123} for "
                           "index %d",
                           dec1.value, 1);
    if (!str_eq(str1, BURROW_S("hello")))
        testing_t_errorf_v(t, "got %q wanted %q for index %d", str1, BURROW_S("hello"),
                           2);
    bool same = arr1.len == 3;
    for (Int i = 0; same && i < 3; i++)
        same = ((const int64_t *)arr1.p)[i] == array[i];
    if (!same)
        testing_t_errorf_v(t, "got %v wanted []int64{42, 128, 707} for index %d", arr1,
                           3);

out:
    arena_free(&ar);
    td_close_free(db);
}

static void TestNamedValueChecker(TestingT *t) {
    synctest_test(t, testNamedValueChecker);
}

static void testNamedValueCheckerSkip(TestingT *t) {
    sql_register(BURROW_S("NamedValueCheckSkip"),
                 (Driver){&td_nvc_driver_vt, &td_nvc_skip});
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("NamedValueCheckSkip"), BURROW_STR_EMPTY, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    Context ctx = testing_t_context(t);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    (void)sql_db_exec_context(db, a, ctx, BURROW_S("WIPE"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec wipe %v", err);
        goto out;
    }

    (void)sql_db_exec_context(db, a, ctx, BURROW_S("CREATE|keys|dec1=any"), NO_ARGS,
                              &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec create %v", err);
        goto out;
    }

    DecimalInt dec = {123};
    SqlNamedArg named_a = sql_named(BURROW_S("A"), (Any){TYPE_OF(DecimalInt), &dec});
    (void)sql_db_exec_context(db, a, ctx, BURROW_S("INSERT|keys|dec1=?A"),
                              ARGS(sql_named_any(&named_a)), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error with bad argument, got %v", err);

out:
    arena_free(&ar);
    td_close_free(db);
}

static void TestNamedValueCheckerSkip(TestingT *t) {
    synctest_test(t, testNamedValueCheckerSkip);
}

/* fakeDriverCtx, a fake driver that is also a DriverContext. */
static DriverConnector td_ctx_open_connector(void *self, Str name, Error *err) {
    (void)self;
    FakeConnector *fc = BURROW_NEW(heap_allocator(), FakeConnector);
    if (fc == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverConnector){NULL, NULL};
    }
    *fc = (FakeConnector){name, NULL, false, FAKE_DEFAULT};
    *err = BURROW_NO_ERROR;
    return fdb_connector(fc);
}

static const DriverVT td_ctx_driver_vt = {NULL, fdb_driver_open, td_ctx_open_connector};

static void testOpenConnector(TestingT *t) {
    sql_register(BURROW_S("testctx"), (Driver){&td_ctx_driver_vt, NULL});
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("testctx"), BURROW_S("people"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    DriverConnector c = burrow__sql_db_connector(db);
    if (c.vt != &fdb_connector_vt) {
        testing_t_errorf_v(t, "not using *fakeConnector");
        td_close_free(db);
        return;
    }
    FakeConnector *fc = c.data;

    err = sql_db_close(db);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!fc->closed)
        testing_t_errorf_v(t, "connector is not closed");
    td_close_free(db);
    mem_free(heap_allocator(), fc, sizeof *fc, _Alignof(FakeConnector));
}

static void TestOpenConnector(TestingT *t) {
    synctest_test(t, testOpenConnector);
}

/* ctxOnlyDriver and ctxOnlyConn: a conn with QueryContext and ExecContext but
 * not their older forms. */
typedef struct TdCtxOnlyConn {
    FakeConn *fc;
    bool query_ctx_called;
    bool exec_ctx_called;
} TdCtxOnlyConn;

static DriverTx td_ctx_only_begin(void *self, Error *err) {
    return fdb_conn_begin(((TdCtxOnlyConn *)self)->fc, err);
}

static Error td_ctx_only_close(void *self) {
    TdCtxOnlyConn *c = self;
    Error err = fdb_conn_close(c->fc);
    mem_free(heap_allocator(), c, sizeof *c, _Alignof(TdCtxOnlyConn));
    return err;
}

/* Prepare is still part of the Conn interface, so while it isn't used must be
 * defined for compatibility. */
static DriverStmt td_ctx_only_prepare(void *self, Str query, Error *err) {
    (void)self;
    (void)query;
    (void)err;
    panic_str(BURROW_S("not used"));
}

static DriverStmt td_ctx_only_prepare_context(void *self, Context ctx, Str query,
                                              Error *err) {
    return fdb_conn_prepare_context(((TdCtxOnlyConn *)self)->fc, ctx, query, err);
}

static DriverRows td_ctx_only_query_context(void *self, Context ctx, Str query,
                                            Slice args, Error *err) {
    TdCtxOnlyConn *c = self;
    c->query_ctx_called = true;
    return fdb_conn_query_context(c->fc, ctx, query, args, err);
}

static DriverResult td_ctx_only_exec_context(void *self, Alloc *a, Context ctx,
                                             Str query, Slice args, Error *err) {
    TdCtxOnlyConn *c = self;
    c->exec_ctx_called = true;
    return fdb_conn_exec_context(c->fc, a, ctx, query, args, err);
}

static const DriverConnVT td_ctx_only_conn_vt = {
    .prepare = td_ctx_only_prepare,
    .close = td_ctx_only_close,
    .begin = td_ctx_only_begin,
    .prepare_context = td_ctx_only_prepare_context,
    .exec_context = td_ctx_only_exec_context,
    .query_context = td_ctx_only_query_context,
};

static DriverConn td_ctx_only_open(void *self, Str name, Error *err) {
    (void)self;
    FakeConn *fc = fdb_open(name, err);
    if (fc == NULL)
        return (DriverConn){NULL, NULL};
    TdCtxOnlyConn *c = BURROW_NEW(heap_allocator(), TdCtxOnlyConn);
    if (c == NULL)
        panic_str(BURROW_S("out of memory"));
    c->fc = fc;
    return (DriverConn){&td_ctx_only_conn_vt, c};
}

static const DriverVT td_ctx_only_driver_vt = {NULL, td_ctx_only_open, NULL};

/* TestQueryExecContextOnly ensures drivers only need to implement QueryContext
 * and ExecContext methods. */
static void testQueryExecContextOnly(TestingT *t) {
    /* Ensure connection does not implement non-context interfaces. */
    if (td_ctx_only_conn_vt.exec != NULL)
        FATALF("ctxOnlyConn must not implement driver.Execer");
    if (td_ctx_only_conn_vt.query != NULL)
        FATALF("ctxOnlyConn must not implement driver.Queryer");

    sql_register(BURROW_S("ContextOnly"), (Driver){&td_ctx_only_driver_vt, NULL});
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("ContextOnly"), BURROW_STR_EMPTY, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    Context ctx = testing_t_context(t);

    SqlConn *conn = sql_db_conn(db, ctx, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "db.Conn %v", err);
        td_close_free(db);
        return;
    }
    TdCtxOnlyConn *coc = burrow__sql_conn_driver(conn).data;
    coc->fc->skip_dirty_session = true;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    (void)sql_conn_exec_context(conn, a, ctx, BURROW_S("WIPE"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec wipe %v", err);
        goto out;
    }

    (void)sql_conn_exec_context(conn, a, ctx, BURROW_S("CREATE|keys|v1=string"),
                                NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec create %v", err);
        goto out;
    }
    Str expected_value = BURROW_S("value1");
    (void)sql_conn_exec_context(conn, a, ctx, BURROW_S("INSERT|keys|v1=?"),
                                ARGS(expected_value), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec insert %v", err);
        goto out;
    }
    SqlRows *rows =
        sql_conn_query_context(conn, ctx, BURROW_S("SELECT|keys|v1|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "query select %v", err);
        goto out;
    }
    Str v1 = BURROW_STR_EMPTY;
    while (sql_rows_next(rows)) {
        err = sql_rows_scan_v(rows, a, &v1);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "rows scan %v", err);
            break;
        }
    }
    (void)sql_rows_close(rows);
    sql_rows_free(rows);
    if (testing_t_failed(t))
        goto out;

    if (!str_eq(v1, expected_value)) {
        testing_t_errorf_v(t, "expected %q, got %q", expected_value, v1);
        goto out;
    }

    if (!coc->exec_ctx_called)
        testing_t_errorf_v(t, "ExecContext not called");
    if (!coc->query_ctx_called)
        testing_t_errorf_v(t, "QueryContext not called");

out:
    arena_free(&ar);
    sql_conn_free(conn);
    td_close_free(db);
}

static void TestQueryExecContextOnly(TestingT *t) {
    synctest_test(t, testQueryExecContextOnly);
}

/* ------------------------------------------------------ errors and cancel */

TD_SENTINEL(td_err_test_scan_wrap, "errTestScanWrap");

typedef struct AlwaysErrScanner {
    Byte unused;
} AlwaysErrScanner;
extern const Type burrow_type_AlwaysErrScanner;

static Error td_always_err_scan(AlwaysErrScanner *s, DriverAllocArg a, Any src) {
    (void)s;
    (void)a;
    (void)src;
    return td_err_test_scan_wrap;
}

#define ALWAYS_ERR_SCANNER_FIELDS(F, T) F(T, Byte, unused, "")
#define ALWAYS_ERR_SCANNER_METHODS(M, T) M(T, Scan, td_always_err_scan, SQL_SIG_SCAN)
BURROW_STRUCT_DEFINE_METHODS(AlwaysErrScanner, ALWAYS_ERR_SCANNER_FIELDS,
                             ALWAYS_ERR_SCANNER_METHODS);

/* Issue 38099: Ensure that Rows.Scan properly wraps underlying errors. */
static void testRowsScanProperlyWrapsErrors(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(db, BURROW_S("SELECT|people|age|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);

    AlwaysErrScanner res = {0};

    while (sql_rows_next(rows)) {
        err = sql_rows_scan_v(rows, heap_allocator(),
                              ((Any){TYPE_OF(AlwaysErrScanner), &res}));
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "expecting back an error");
            break;
        }
        if (!errors_is(err, td_err_test_scan_wrap)) {
            testing_t_errorf_v(t, "errors.Is mismatch\n%v\nWant: %v", err,
                               td_err_test_scan_wrap);
            break;
        }
        /* Ensure that error substring matching still correctly works. */
        if (!strings_contains(error_text(err), error_text(td_err_test_scan_wrap))) {
            testing_t_errorf_v(t, "Error %v does not contain %v", err,
                               td_err_test_scan_wrap);
            break;
        }
    }
    sql_rows_free(rows);
}

static void TestRowsScanProperlyWrapsErrors(TestingT *t) {
    test_database(t, testRowsScanProperlyWrapsErrors, NULL);
}

/* errEmpty is returned when an empty value is found. */
TD_SENTINEL(td_err_empty, "empty value");

typedef struct AlwaysErrValuer {
    Byte unused;
} AlwaysErrValuer;
extern const Type burrow_type_AlwaysErrValuer;

static Any td_always_err_value(AlwaysErrValuer *v, DriverAllocArg a,
                               DriverErrorArg err) {
    (void)v;
    (void)a;
    BURROW_OUT(err, td_err_empty);
    return (Any){NULL, NULL};
}

#define ALWAYS_ERR_VALUER_FIELDS(F, T) F(T, Byte, unused, "")
#define ALWAYS_ERR_VALUER_METHODS(M, T)                                                \
    M(T, Value, td_always_err_value, DRIVER_SIG_VALUE)
BURROW_STRUCT_DEFINE_METHODS(AlwaysErrValuer, ALWAYS_ERR_VALUER_FIELDS,
                             ALWAYS_ERR_VALUER_METHODS);

static void td_check_err_empty(TestingT *t, Error err) {
    if (BURROW_OK(err))
        FATALF("expecting back an error");
    if (!errors_is(err, td_err_empty))
        FATALF("errors.Is mismatch\n%v\nWant: %v", err, td_err_empty);
    /* Ensure that error substring matching still correctly works. */
    if (!strings_contains(error_text(err), error_text(td_err_empty)))
        FATALF("Error %v does not contain %v", err, td_err_empty);
}

static void td_args_exec(TestingT *t) {
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;
    AlwaysErrValuer v = {0};
    Error err = BURROW_NO_ERROR;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    (void)sql_db_exec(db, arena_allocator(&ar), BURROW_S("INSERT|keys|dec1=?"),
                      ARGS(((Any){TYPE_OF(AlwaysErrValuer), &v})), &err);
    arena_free(&ar);
    td_check_err_empty(t, err);
    close_db(t, db);
}

static void td_args_query(TestingT *t) {
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;
    AlwaysErrValuer v = {0};
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(db, BURROW_S("INSERT|keys|dec1=?"),
                                 ARGS(((Any){TYPE_OF(AlwaysErrValuer), &v})), &err);
    if (rows != NULL)
        sql_rows_free(rows);
    td_check_err_empty(t, err);
    close_db(t, db);
}

/* A subtest body, boxed so it can travel through a closure's env. */
typedef struct TdSubFn {
    void (*f)(TestingT *t);
} TdSubFn;

static void td_synctest_sub(void *env, TestingT *t) {
    synctest_test(t, ((TdSubFn *)env)->f);
}

/* Like Go's synctestSubtest: a subtest whose body runs in a bubble. */
static void synctest_subtest(TestingT *t, const char *name, TdSubFn *fn) {
    (void)testing_t_run(t, str_from_cstr(name),
                        BURROW_FN(TestingTFunc, td_synctest_sub, fn));
}

/* Issue 64707: Ensure that Stmt.Exec and Stmt.Query properly wraps underlying
 * errors. */
static void TestDriverArgsWrapsErrors(TestingT *t) {
    TdSubFn exec = {td_args_exec}, query = {td_args_query};
    synctest_subtest(t, "exec", &exec);
    synctest_subtest(t, "query", &query);
}

typedef struct TdRawScan {
    TestingT *t;
    const char *mode;
} TdRawScan;

static void td_go_cancel(void *env) {
    BURROW_CALLF0(*(ContextCancelFunc *)env);
}

/* From go.dev/issue/60304. */
static void td_raw_scan_bubble(void *env) {
    TdRawScan *rs = env;
    TestingT *t = rs->t;
    const char *mode = rs->mode;
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL)
        return;

    /* cancel used to call close asynchronously. This test checks that it waits
     * so as not to interfere with RawBytes. */
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlRows *r =
        sql_db_query_context(db, ctx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        BURROW_CALLF0(cancel);
        close_db(t, db);
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    int num_rows = 0;
    Byte sink = 0;
    while (sql_rows_next(r)) {
        if (strcmp(mode, "top") == 0 && num_rows == 2) {
            /* cancel between Next and Scan is observed by Scan as err =
             * context.Canceled. */
            BURROW_CALLF0(cancel);
            synctest_wait();
        }
        num_rows++;
        SqlRawBytes s = {NULL, 0, 0, TYPE_BYTE};
        err =
            sql_rows_scan_v(r, arena_allocator(&ar), ((Any){TYPE_OF(SqlRawBytes), &s}));
        if (num_rows == 3 && errors_is(err, context_canceled)) {
            if (burrow__sql_rows_scan_hold(r))
                testing_t_errorf_v(t, "expected closemu NOT to be held");
            break;
        }
        if (!burrow__sql_rows_scan_hold(r))
            testing_t_errorf_v(t, "expected closemu to be held");
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
            goto out;
        }
        testing_t_logf_v(t, "read %q", s);
        if (strcmp(mode, "bottom") == 0 && num_rows == 2) {
            /* cancel before Next should be observed by Next, exiting the
             * loop. */
            BURROW_CALLF0(cancel);
            synctest_wait();
        }
        if (strcmp(mode, "go") == 0 && num_rows == 2) {
            /* cancel at any future time, to catch other cases */
            (void)go(BURROW_FN(Func, td_go_cancel, &cancel));
        }
        for (Int i = 0; i < s.len; i++) /* some operation reading from the raw memory */
            sink = (Byte)(sink + ((const Byte *)s.p)[i]);
        /* r.Columns must not deadlock acquiring closemu.RLock. */
        Error cerr = BURROW_NO_ERROR;
        (void)sql_rows_columns(r, arena_allocator(&ar), &cerr);
    }
    (void)sink;
    if (burrow__sql_rows_scan_hold(r))
        testing_t_errorf_v(t, "closemu held; should not be");

    /* There are 3 rows. We canceled after reading 2 so we expect either 2 or 3
     * depending on how the awaitDone goroutine schedules. */
    switch (num_rows) {
    case 0:
    case 1:
        testing_t_errorf_v(t, "got %d rows; want 2+", num_rows);
        break;
    case 2:
        err = sql_rows_err(r);
        if (!errors_is(err, context_canceled))
            testing_t_errorf_v(t, "unexpected error: %v", err);
        break;
    default:
        /* Made it to the end. This is rare, but fine. Permit it. */
        break;
    }

    err = sql_rows_close(r);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
out:
    sql_rows_free(r);
    arena_free(&ar);
    BURROW_CALLF0(cancel);
    close_db(t, db);
}

static void td_raw_scan_sub(void *env, TestingT *t) {
    TdRawScan *rs = env;
    rs->t = t;
    if (!synctest_run(BURROW_FN(Func, td_raw_scan_bubble, rs)))
        testing_t_errorf_v(t, "synctest_run failed");
}

static void TestContextCancelDuringRawBytesScan(TestingT *t) {
    static const char *const modes[] = {"nocancel", "top", "bottom", "go"};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        TdRawScan rs = {NULL, modes[i]};
        (void)testing_t_run(t, str_from_cstr(modes[i]),
                            BURROW_FN(TestingTFunc, td_raw_scan_sub, &rs));
    }
}

static void testContextCancelBetweenNextAndErr(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlRows *r =
        sql_db_query_context(db, ctx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }
    while (sql_rows_next(r)) {
    }
    BURROW_CALLF0(cancel); /* wake up the awaitDone goroutine */
    synctest_wait();
    err = sql_rows_err(r);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    sql_rows_free(r);
}

static void TestContextCancelBetweenNextAndErr(TestingT *t) {
    test_database(t, testContextCancelBetweenNextAndErr, NULL);
}

/* testScanner, whose scanf is a closure in Go: here it sends src on
 * scan_start and returns once scan_end gives it something, or is closed. */
typedef struct TdTestScanner {
    Chan *scan_start;
    Chan *scan_end;
} TdTestScanner;

static Error td_test_scan(void *self, Alloc *a, Any src) {
    (void)a;
    TdTestScanner *ts = self;
    chan_send(ts->scan_start, &src);
    bool v = false;
    (void)chan_recv(ts->scan_end, &v);
    return BURROW_NO_ERROR;
}

static const SqlScannerVT td_test_scanner_vt = {NULL, td_test_scan};

typedef struct TdScanGo {
    SqlRows *r;
    SqlScanner scanner;
} TdScanGo;

static void td_scan_go(void *env) {
    TdScanGo *g = env;
    (void)sql_rows_scan_v(g->r, heap_allocator(),
                          ((Any){TYPE_OF(SqlScanner), &g->scanner}));
}

static void testContextCancelDuringScan(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    TdTestScanner ts = {chan_make(heap_allocator(), TYPE_ANY, 0),
                        chan_make(heap_allocator(), TYPE_BOOL, 0)};

    /* Start a query, and pause it mid-scan. */
    static Byte alice[] = {'A', 'l', 'i', 'c', 'e'};
    Bytes want = slice_from(alice, 5, 5, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    SqlRows *r = sql_db_query_context(db, ctx, BURROW_S("SELECT|people|name|name=?"),
                                      ARGS(BURROW_S("Alice")), &err);
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto out;
    }
    if (!sql_rows_next(r)) {
        testing_t_errorf_v(t, "r.Next() = false, want true");
        goto out;
    }
    TdScanGo g = {r, {&td_test_scanner_vt, &ts}};
    (void)sync_wait_group_go(&wg, BURROW_FN(Func, td_scan_go, &g));
    Any got = {NULL, NULL};
    (void)chan_recv(ts.scan_start, &got);
    if (!fdb_is_bytes(got.t)) {
        testing_t_errorf_v(t, "r.Scan returned %s, want []byte", type_name(got.t));
    } else {
        Bytes got_bytes = *(const Bytes *)got.data;
        if (!bytes_equal(got_bytes, want)) {
            testing_t_errorf_v(t, "before cancel: r.Scan returned %q, want %q",
                               got_bytes, want);
        } else {
            /* Cancel the query. */
            BURROW_CALLF0(cancel);
            synctest_wait();

            /* Cancelling the query should not have changed the result. */
            if (!bytes_equal(got_bytes, want))
                testing_t_errorf_v(t, "after cancel: r.Scan result is now %q, want %q",
                                   got_bytes, want);
        }
    }
    chan_close(ts.scan_end);
    sync_wait_group_wait(&wg);

out:
    if (r != NULL)
        sql_rows_free(r);
    chan_free(ts.scan_start);
    chan_free(ts.scan_end);
    BURROW_CALLF0(cancel);
}

static void TestContextCancelDuringScan(TestingT *t) {
    test_database(t, testContextCancelDuringScan, NULL);
}

static void testNilErrorAfterClose(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    /* This WithCancel is important; Rows contains an optimization to avoid
     * spawning a goroutine when the query/transaction context cannot be
     * canceled, but this test tests a bug which is caused by said goroutine. */
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);

    Error err = BURROW_NO_ERROR;
    SqlRows *r =
        sql_db_query_context(db, ctx, BURROW_S("SELECT|people|name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        BURROW_CALLF0(cancel);
        FATALF("%v", err);
    }

    err = sql_rows_close(r);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        synctest_wait();
        err = sql_rows_err(r);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
    }
    sql_rows_free(r);
    BURROW_CALLF0(cancel);
}

static void TestNilErrorAfterClose(TestingT *t) {
    test_database(t, testNilErrorAfterClose, NULL);
}

/* Runs query and scans its first row into raw. */
static bool td_scan_first_raw(TestingT *t, SqlDB *db, const char *query, Alloc *a,
                              SqlRawBytes *raw, Str *copy) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(db, str_from_cstr(query), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        return false;
    }
    (void)sql_rows_next(rows);
    (void)sql_rows_scan_v(rows, a, ((Any){TYPE_OF(SqlRawBytes), raw}));
    if (copy != NULL)
        *copy = str_clone(a, str_from_bytes(raw->p, raw->len));
    (void)sql_rows_close(rows);
    sql_rows_free(rows);
    return true;
}

/* Issue #65201.
 *
 * If a RawBytes is reused across multiple queries, subsequent queries
 * shouldn't overwrite driver-owned memory from previous queries. */
static void testRawBytesReuse(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlRawBytes raw = {NULL, 0, 0, TYPE_BYTE};
    Str name1 = BURROW_STR_EMPTY, name2 = BURROW_STR_EMPTY;

    /* The RawBytes in this query aliases driver-owned memory. */
    if (td_scan_first_raw(t, db, "SELECT|people|name|", a, &raw, &name1) &&
        /* The RawBytes in this query does not alias driver-owned memory. */
        td_scan_first_raw(t, db, "SELECT|people|age|", a, &raw, NULL) &&
        /* Repeat the first query. Nothing should have changed. */
        td_scan_first_raw(t, db, "SELECT|people|name|", a, &raw, &name2) &&
        !str_eq(name1, name2))
        testing_t_errorf_v(t, "Scan read name %q, want %q", name2, name1);
    arena_free(&ar);
}

static void TestRawBytesReuse(TestingT *t) {
    test_database(t, testRawBytesReuse, NULL);
}

static void td_arena_free(void *a) {
    arena_free((Arena *)a);
}

/* A query for td_expect_panic to run. */
typedef struct TdPanicCall {
    SqlDB *db;
    const char *query;
    TestingT *t;
} TdPanicCall;

typedef void (*TdPanicFn)(TdPanicCall *c);

static void td_panic_exec(TdPanicCall *c) {
    Arena ar;
    BURROW_SCOPE {
        arena_init(&ar, heap_allocator(), 0);
        BURROW_DEFER(td_arena_free, &ar);
        Error err = BURROW_NO_ERROR;
        (void)sql_db_exec(c->db, arena_allocator(&ar), str_from_cstr(c->query), NO_ARGS,
                          &err);
    }
    BURROW_SCOPE_END;
}

static void td_panic_query(TdPanicCall *c) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(c->db, str_from_cstr(c->query), NO_ARGS, &err);
    sql_rows_free(rows);
}

static void td_rows_free_fn(void *rows) {
    sql_rows_free((SqlRows *)rows);
}

static void td_panic_query_close(TdPanicCall *c) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(c->db, str_from_cstr(c->query), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(c->t, "%v", err);
    BURROW_SCOPE {
        BURROW_DEFER(td_rows_free_fn, rows);
        (void)sql_rows_close(rows);
    }
    BURROW_SCOPE_END;
}

static void td_expect_panic(TestingT *t, const char *name, TdPanicFn f,
                            TdPanicCall *c) {
    volatile bool panicked = false;
    BURROW_TRY {
        f(c);
    }
    BURROW_CATCH(p) {
        (void)p;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        FATALF("%s did not panic", name);
}

static void testDriverPanic(TestingT *t) {
    register_test_driver();
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("test"), fake_db_name, &err);
    if (BURROW_FAILED(err))
        FATALF("Open: %v", err);
    /* Test that if driver panics, database/sql does not deadlock. */
    TdPanicCall c = {db, NULL, t};
    const char *exec_panics[][2] = {
        {"Exec Exec", "PANIC|Exec|WIPE"},
        {"Exec NumInput", "PANIC|NumInput|WIPE"},
        {"Exec Close", "PANIC|Close|WIPE"},
    };
    for (size_t i = 0; i < sizeof exec_panics / sizeof exec_panics[0]; i++) {
        c.query = exec_panics[i][1];
        td_expect_panic(t, exec_panics[i][0], td_panic_exec, &c);
        if (!EXEC(t, db, "WIPE")) /* check not deadlocked */
            goto out;
    }
    /* should run successfully: Exec does not call Query */
    if (!EXEC(t, db, "PANIC|Query|WIPE") || !EXEC(t, db, "WIPE"))
        goto out;

    if (!EXEC(t, db,
              "CREATE|people|name=string,age=int32,photo=blob,dead=bool,"
              "bdate=datetime"))
        goto out;

    c.query = "PANIC|Query|SELECT|people|age,name|";
    td_expect_panic(t, "Query Query", td_panic_query, &c);
    c.query = "PANIC|NumInput|SELECT|people|age,name|";
    td_expect_panic(t, "Query NumInput", td_panic_query, &c);
    c.query = "PANIC|Close|SELECT|people|age,name|";
    td_expect_panic(t, "Query Close", td_panic_query_close, &c);
    /* should run successfully: Query does not call Exec */
    SqlRows *rows =
        sql_db_query(db, BURROW_S("PANIC|Exec|SELECT|people|age,name|"), NO_ARGS, &err);
    sql_rows_free(rows);
    (void)EXEC(t, db, "WIPE"); /* check not deadlocked */
out:
    td_close_free(db);
}

static void TestDriverPanic(TestingT *t) {
    synctest_test(t, testDriverPanic);
}

TD_SENTINEL(td_err_bad_prepare, "badConn Prepare");
TD_SENTINEL(td_err_bad_begin, "badConn Begin");

/* badConn implements a bad driver.Conn, for TestBadDriver. The Exec method
 * panics. */
static DriverStmt td_bad_prepare(void *self, Str query, Error *err) {
    (void)self;
    (void)query;
    *err = td_err_bad_prepare;
    return (DriverStmt){NULL, NULL};
}

static Error td_bad_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static DriverTx td_bad_begin(void *self, Error *err) {
    (void)self;
    *err = td_err_bad_begin;
    return (DriverTx){NULL, NULL};
}

static DriverResult td_bad_exec(void *self, Alloc *a, Str query, Slice args,
                                Error *err) {
    (void)self;
    (void)a;
    (void)query;
    (void)args;
    (void)err;
    panic_str(BURROW_S("badConn.Exec"));
}

static const DriverConnVT td_bad_conn_vt = {
    .prepare = td_bad_prepare,
    .close = td_bad_close,
    .begin = td_bad_begin,
    .exec = td_bad_exec,
};

/* badDriver is a driver.Driver that uses badConn. */
static DriverConn td_bad_open(void *self, Str name, Error *err) {
    (void)self;
    (void)name;
    *err = BURROW_NO_ERROR;
    return (DriverConn){&td_bad_conn_vt, NULL};
}

static const DriverVT td_bad_driver_vt = {NULL, td_bad_open, NULL};

/* Issue 15901. */
static void testBadDriver(TestingT *t) {
    sql_register(BURROW_S("bad"), (Driver){&td_bad_driver_vt, NULL});
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("bad"), BURROW_S("ignored"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    TdPanicCall c = {db, "ignored", t};
    volatile bool panicked = false;
    BURROW_TRY {
        td_panic_exec(&c);
    }
    BURROW_CATCH(p) {
        panicked = true;
        Str want = BURROW_S("badConn.Exec");
        if (!str_eq(panic_text(p), want))
            testing_t_errorf_v(t, "panic was %v, expected %v", panic_text(p), want);
    }
    BURROW_TRY_END;
    td_close_free(db);
    if (!panicked)
        testing_t_errorf_v(t, "expected panic");
}

static void TestBadDriver(TestingT *t) {
    synctest_test(t, testBadDriver);
}

typedef struct TdPingDriver {
    bool fails;
} TdPingDriver;

TD_SENTINEL(td_ping_error, "Ping failed");

/* pingConn is a badConn with Ping, and its data is the driver. */
static Error td_ping_ping(void *self, Context ctx) {
    (void)ctx;
    if (((TdPingDriver *)self)->fails)
        return td_ping_error;
    return BURROW_NO_ERROR;
}

static const DriverConnVT td_ping_conn_vt = {
    .prepare = td_bad_prepare,
    .close = td_bad_close,
    .begin = td_bad_begin,
    .exec = td_bad_exec,
    .ping = td_ping_ping,
};

static DriverConn td_ping_open(void *self, Str name, Error *err) {
    (void)name;
    *err = BURROW_NO_ERROR;
    return (DriverConn){&td_ping_conn_vt, self};
}

static const DriverVT td_ping_driver_vt = {NULL, td_ping_open, NULL};

static void testPing(TestingT *t) {
    TdPingDriver driver = {false};
    sql_register(BURROW_S("ping"), (Driver){&td_ping_driver_vt, &driver});

    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("ping"), BURROW_S("ignored"), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    err = sql_db_ping(db);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "err was %#v, expected nil", err);
        td_close_free(db);
        return;
    }

    driver.fails = true;
    err = sql_db_ping(db);
    if (err.vt != td_ping_error.vt || err.data != td_ping_error.data)
        testing_t_errorf_v(t, "err was %#v, expected pingError", err);
    td_close_free(db);
}

static void TestPing(TestingT *t) {
    synctest_test(t, testPing);
}

/* type Str string, a named string type of the test's own. */
static const Type td_str_type = {
    {(const Byte *)"Str", 3},
    {(const Byte *)"database/sql", 12},
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

/* Issue 18101. */
static void testTypedString(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str scanned = BURROW_STR_EMPTY;
    SqlRow row = sql_db_query_row(db, BURROW_S("SELECT|people|name|name=?"),
                                  ARGS(BURROW_S("Alice")));
    Error err = sql_row_scan_v(&row, a, ((Any){&td_str_type, &scanned}));
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        FATALF("%v", err);
    }
    Str expected = BURROW_S("Alice");
    if (!str_eq(scanned, expected))
        testing_t_errorf_v(t, "expected %+v, got %+v", expected, scanned);
    arena_free(&ar);
}

static void TestTypedString(TestingT *t) {
    test_database(t, testTypedString, NULL);
}

static void td_grab_once(void *env, TestingT *t) {
    (void)t;
    Error err = burrow__sql_conn_grab_release((SqlConn *)env);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
}

/* Go's version grabs from a zero Conn. Here the Conn is a real one, since a
 * Conn without a connection is one grabConn refuses, and what is measured is
 * the grab and release around it. */
static void testGrabConnAllocs(TestingT *t) {
    FakeConnector fc = {BURROW_STR_EMPTY, NULL, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, NULL);
    if (db == NULL)
        return;
    Error err = BURROW_NO_ERROR;
    SqlConn *c = sql_db_conn(db, context_background(), &err);
    if (BURROW_FAILED(err)) {
        close_db(t, db);
        FATALF("%v", err);
    }
    td_grab_once(c, t);
    uint64_t before = mem_stats(heap_allocator()).allocs;
    for (int i = 0; i < 1000; i++)
        td_grab_once(c, t);
    uint64_t n = mem_stats(heap_allocator()).allocs - before;
    sql_conn_free(c);
    close_db(t, db);
    if (n > 0)
        FATALF("Conn.grabConn allocated %v objects; want 0", n);
}

static void TestGrabConnAllocs(TestingT *t) {
    synctest_test(t, testGrabConnAllocs);
}

static void td_want_len(TestingT *t, void *s, Int want) {
    Int got = burrow__sql_reqs_len(s);
    if (got != want)
        testing_t_errorf_v(t, "Len = %d; want %d", got, want);
    if (want == 0 && !testing_t_failed(t)) {
        if (burrow__sql_reqs_take_random(s) != NULL)
            FATALF("TakeRandom returned result when empty");
    }
}

static void td_reqs_add_delete(void *env, TestingT *t) {
    void *s = env;
    td_want_len(t, s, 0);
    void *dh = burrow__sql_reqs_add(s);
    td_want_len(t, s, 1);
    bool ok = burrow__sql_reqs_delete(s, dh);
    if (!ok) {
        burrow__sql_waiter_free(dh);
        FATALF("failed to delete");
    }
    td_want_len(t, s, 0);
    if (burrow__sql_reqs_delete(s, dh))
        testing_t_errorf_v(t, "delete worked twice");
    td_want_len(t, s, 0);
    burrow__sql_waiter_free(dh);
}

static void td_reqs_take_before_delete(void *env, TestingT *t) {
    void *s = env;
    void *ch1 = burrow__sql_reqs_add(s);
    td_want_len(t, s, 1);
    void *got = burrow__sql_reqs_take_random(s);
    if (got != ch1) {
        burrow__sql_waiter_free(ch1);
        FATALF("wrong take; ok=%v", got != NULL);
    }
    td_want_len(t, s, 0);
    if (burrow__sql_reqs_delete(s, ch1))
        testing_t_errorf_v(t, "unexpected delete after take");
    burrow__sql_waiter_free(ch1);
}

enum { TD_REQS_N = 100 };

static void td_reqs_get_take_many(void *env, TestingT *t) {
    void *s = env;
    void *in_order[TD_REQS_N];
    void *back_out[TD_REQS_N];
    bool taken[TD_REQS_N] = {false};
    for (int i = 0; i < TD_REQS_N; i++)
        in_order[i] = burrow__sql_reqs_add(s);
    if (burrow__sql_reqs_len(s) != TD_REQS_N)
        testing_t_errorf_v(t, "Len = %v; want %v", burrow__sql_reqs_len(s),
                           (Int)TD_REQS_N);
    int nout = 0;
    while (!testing_t_failed(t) && burrow__sql_reqs_len(s) > 0) {
        void *c = burrow__sql_reqs_take_random(s);
        if (c == NULL) {
            testing_t_errorf_v(t, "failed to take when non-empty");
            break;
        }
        int at = -1;
        for (int i = 0; i < TD_REQS_N; i++)
            if (in_order[i] == c && !taken[i])
                at = i;
        if (at < 0) {
            testing_t_errorf_v(t, "returned item not in remaining set");
            break;
        }
        taken[at] = true;
        back_out[nout++] = c;
    }
    for (int i = 0; i < TD_REQS_N; i++)
        if (!taken[i] && !testing_t_failed(t))
            testing_t_errorf_v(t, "items remain in expected map");
    /* N! chance of flaking; N=100 is fine */
    if (nout == TD_REQS_N && memcmp(in_order, back_out, sizeof in_order) == 0)
        testing_t_errorf_v(t, "wasn't random");
    burrow__sql_reqs_close_all(s);
    for (int i = 0; i < TD_REQS_N; i++)
        burrow__sql_waiter_free(in_order[i]);
}

static void td_reqs_close_delete(void *env, TestingT *t) {
    void *s = env;
    void *ch = burrow__sql_reqs_add(s);
    td_want_len(t, s, 1);
    burrow__sql_reqs_close_all(s);
    td_want_len(t, s, 0);
    if (burrow__sql_reqs_delete(s, ch))
        testing_t_errorf_v(t, "unexpected delete after CloseAndRemoveAll");
    burrow__sql_waiter_free(ch);
}

static void TestConnRequestSet(TestingT *t) {
    static const struct {
        const char *name;
        void (*f)(void *, TestingT *);
    } subs[] = {
        {"add-delete", td_reqs_add_delete},
        {"take-before-delete", td_reqs_take_before_delete},
        {"get-take-many", td_reqs_get_take_many},
        {"close-delete", td_reqs_close_delete},
    };
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        void *s = burrow__sql_reqs_new();
        (void)testing_t_run(t, str_from_cstr(subs[i].name),
                            BURROW_FN(TestingTFunc, subs[i].f, s));
        burrow__sql_reqs_free(s);
    }
}

static void TestIssue69837(TestingT *t) {
    Uint v = 1;
    SqlNull u = {BURROW_ANY(TYPE_UINT, &v), true};
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    DriverValue val = driver_default_parameter_converter.vt->convert_value(
        driver_default_parameter_converter.data, arena_allocator(&ar), sql_null_any(&u),
        &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ConvertValue() error = %v, want nil", err);

    if (val.t != TYPE_INT64)
        testing_t_errorf_v(t, "val.(type): got %v, expected int64",
                           val.t == NULL ? BURROW_S("<nil>") : type_name(val.t));
    else if (*(const int64_t *)val.data != 1)
        testing_t_errorf_v(t, "val: got %d, expected 1", *(const int64_t *)val.data);
    arena_free(&ar);
}

typedef struct Issue69728Type {
    Int id;
    Str name;
} Issue69728Type;
extern const Type burrow_type_Issue69728Type;

static Any td_issue69728_value(Issue69728Type *v, DriverAllocArg a,
                               DriverErrorArg err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "%d, %s", v->id, v->name);
    Bytes *b = BURROW_NEW(a, Bytes);
    Byte *p = s.len > 0 ? BURROW_NEW_N(a, Byte, (size_t)s.len) : NULL;
    if (b == NULL || (s.len > 0 && p == NULL)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Any){NULL, NULL};
    }
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    *b = slice_from(p, s.len, s.len, TYPE_BYTE);
    return BURROW_ANY(TYPE_BYTES, b);
}

#define ISSUE69728_TYPE_FIELDS(F, T) F(T, Int, id, "") F(T, Str, name, "")
#define ISSUE69728_TYPE_METHODS(M, T) M(T, Value, td_issue69728_value, DRIVER_SIG_VALUE)
BURROW_STRUCT_DEFINE_METHODS(Issue69728Type, ISSUE69728_TYPE_FIELDS,
                             ISSUE69728_TYPE_METHODS);

static void TestIssue69728(TestingT *t) {
    Issue69728Type v = {42, BURROW_S("foobar")};
    SqlNull for_value = {BURROW_ANY(TYPE_OF(Issue69728Type), &v), true};
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    DriverValue v1 = sql_null_value(for_value, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "forValue.Value() error = %v, want nil", err);

    DriverValue v2 = td_issue69728_value(&v, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "forValue.V.Value() error = %v, want nil", err);

    if (v1.t != v2.t || v1.t != TYPE_BYTES ||
        !bytes_equal(*(const Bytes *)v1.data, *(const Bytes *)v2.data))
        testing_t_errorf_v(t, "not equal; v1 = %v, v2 = %v", v1, v2);
    arena_free(&ar);
}

/* unknownInputsConnector, whose statements do not know how many inputs they
 * take and convert every argument to "string". */
static Str td_unknown_string = BURROW_S_INIT("string");

static DriverValue td_unknown_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    (void)a;
    (void)v;
    *err = BURROW_NO_ERROR;
    return BURROW_ANY(TYPE_STRING, &td_unknown_string);
}

static const DriverValueConverterVT td_unknown_converter_vt = {NULL,
                                                               td_unknown_convert};

static Error td_unknown_stmt_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static Int td_unknown_num_input(void *self) {
    (void)self;
    return -1;
}

static DriverResult td_unknown_exec(void *self, Alloc *a, Slice args, Error *err) {
    (void)self;
    (void)a;
    const DriverValue *v = (const DriverValue *)args.p;
    if (args.len < 1 || v[0].t != TYPE_STRING) {
        *err = fmt_errorf_v("Expected string, got %v", args.len < 1 || v[0].t == NULL
                                                           ? BURROW_S("<nil>")
                                                           : type_name(v[0].t));
        return (DriverResult){NULL, NULL};
    }
    *err = BURROW_NO_ERROR;
    return (DriverResult){NULL, NULL};
}

static DriverRows td_unknown_query(void *self, Slice args, Error *err) {
    (void)self;
    (void)args;
    *err = BURROW_NO_ERROR;
    return (DriverRows){NULL, NULL};
}

static DriverValueConverter td_unknown_column_converter(void *self, Int idx) {
    (void)self;
    (void)idx;
    return (DriverValueConverter){&td_unknown_converter_vt, NULL};
}

static const DriverStmtVT td_unknown_stmt_vt = {
    .close = td_unknown_stmt_close,
    .num_input = td_unknown_num_input,
    .exec = td_unknown_exec,
    .query = td_unknown_query,
    .column_converter = td_unknown_column_converter,
};

static DriverStmt td_unknown_prepare(void *self, Str query, Error *err) {
    (void)self;
    (void)query;
    *err = BURROW_NO_ERROR;
    return (DriverStmt){&td_unknown_stmt_vt, NULL};
}

static DriverTx td_unknown_begin(void *self, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return (DriverTx){NULL, NULL};
}

static const DriverConnVT td_unknown_conn_vt = {
    .prepare = td_unknown_prepare,
    .close = td_bad_close,
    .begin = td_unknown_begin,
};

static DriverConn td_unknown_connect(void *self, Context ctx, Error *err) {
    (void)self;
    (void)ctx;
    *err = BURROW_NO_ERROR;
    return (DriverConn){&td_unknown_conn_vt, NULL};
}

static Driver td_unknown_driver(void *self) {
    (void)self;
    return (Driver){NULL, NULL};
}

static const DriverConnectorVT td_unknown_connector_vt = {NULL, td_unknown_connect,
                                                          td_unknown_driver, NULL};

static void testColumnConverterWithUnknownInputCount(TestingT *t) {
    SqlDB *db = sql_open_db((DriverConnector){&td_unknown_connector_vt, NULL});
    Error err = BURROW_NO_ERROR;
    SqlStmt *stmt = sql_db_prepare(db, BURROW_S("SELECT ?"), &err);
    if (BURROW_FAILED(err)) {
        td_close_free(db);
        FATALF("%v", err);
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    (void)sql_stmt_exec(stmt, arena_allocator(&ar), ARGS((Int)1), &err);
    arena_free(&ar);
    sql_stmt_free(stmt);
    td_close_free(db);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
}

static void TestColumnConverterWithUnknownInputCount(TestingT *t) {
    synctest_test(t, testColumnConverterWithUnknownInputCount);
}

typedef enum TdNullKind {
    TD_NULL_INT32,
    TD_NULL_INT64,
    TD_NULL_FLOAT64,
    TD_NULL_BOOL,
    TD_NULL_STRING,
    TD_NULL_TIME,
} TdNullKind;

/* One of the Null types, scanned from input. */
typedef struct TdNullCase {
    const char *name;
    TdNullKind kind;
    Any input;
    bool want_err;
    bool preset; /* start it off valid, with a value */
} TdNullCase;

/* Scans c->input into a fresh one of c->kind and says whether it came out
 * valid. */
static bool td_null_scan(const TdNullCase *c, Alloc *a, Error *err) {
    switch (c->kind) {
    case TD_NULL_INT32: {
        SqlNullInt32 n = {c->preset ? 42 : 0, c->preset};
        *err = sql_null_int32_scan(&n, a, c->input);
        return n.valid;
    }
    case TD_NULL_INT64: {
        SqlNullInt64 n = {c->preset ? 42 : 0, c->preset};
        *err = sql_null_int64_scan(&n, a, c->input);
        return n.valid;
    }
    case TD_NULL_FLOAT64: {
        SqlNullFloat64 n = {c->preset ? 3.14 : 0, c->preset};
        *err = sql_null_float64_scan(&n, a, c->input);
        return n.valid;
    }
    case TD_NULL_BOOL: {
        SqlNullBool n = {c->preset, c->preset};
        *err = sql_null_bool_scan(&n, a, c->input);
        return n.valid;
    }
    case TD_NULL_STRING: {
        SqlNullString n = {c->preset ? BURROW_S("preset") : BURROW_STR_EMPTY,
                           c->preset};
        *err = sql_null_string_scan(&n, a, c->input);
        return n.valid;
    }
    case TD_NULL_TIME: {
        SqlNullTime n = {time_now(), c->preset};
        *err = sql_null_time_scan(&n, a, c->input);
        return n.valid;
    }
    default:
        break;
    }
    return false;
}

static void td_null_consistency(void *env, TestingT *t) {
    const TdNullCase *c = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    bool valid = td_null_scan(c, arena_allocator(&ar), &err);

    /* Check that error matches expectation */
    if (BURROW_FAILED(err) != c->want_err)
        testing_t_errorf_v(t, "Scan() error = %v, wantErr %v", err, c->want_err);

    /* The key invariant: Valid should be the opposite of whether we got an
     * error (assuming non-nil input) */
    if (BURROW_FAILED(err) && valid)
        testing_t_errorf_v(t, "Scan() returned error but Valid=true; want Valid=false "
                              "when err!=nil");
    if (BURROW_OK(err) && !valid)
        testing_t_errorf_v(t, "Scan() returned nil error but Valid=false; want "
                              "Valid=true when err==nil");
    arena_free(&ar);
}

/* Issue #45662: Null* types should have Valid=false when Scan returns an
 * error. Previously, Valid was set to true before convertAssign was called, so
 * if conversion failed, Valid would still be true despite the error. */
static void TestNullTypeScanErrorConsistency(TestingT *t) {
    static Byte nan_int[] = "not_a_number";
    static Byte nan_float[] = "not_a_float";
    static Byte nan_bool[] = "not_a_bool";
    Bytes b_int = slice_from(nan_int, 12, 12, TYPE_BYTE);
    Bytes b_float = slice_from(nan_float, 11, 11, TYPE_BYTE);
    Bytes b_bool = slice_from(nan_bool, 10, 10, TYPE_BYTE);
    int64_t i42 = 42;
    double f314 = 3.14;
    bool yes = true;
    Str hello = BURROW_S("hello");
    TdNullCase tests[] = {
        {"NullInt32 with invalid input", TD_NULL_INT32, BURROW_ANY(TYPE_BYTES, &b_int),
         true, false},
        {"NullInt64 with invalid input", TD_NULL_INT64, BURROW_ANY(TYPE_BYTES, &b_int),
         true, false},
        {"NullFloat64 with invalid input", TD_NULL_FLOAT64,
         BURROW_ANY(TYPE_BYTES, &b_float), true, false},
        {"NullBool with invalid input", TD_NULL_BOOL, BURROW_ANY(TYPE_BYTES, &b_bool),
         true, false},
        /* Valid cases should still work */
        {"NullInt32 with valid input", TD_NULL_INT32, BURROW_ANY(TYPE_INT64, &i42),
         false, false},
        {"NullInt64 with valid input", TD_NULL_INT64, BURROW_ANY(TYPE_INT64, &i42),
         false, false},
        {"NullFloat64 with valid input", TD_NULL_FLOAT64,
         BURROW_ANY(TYPE_FLOAT64, &f314), false, false},
        {"NullBool with valid input", TD_NULL_BOOL, BURROW_ANY(TYPE_BOOL, &yes), false,
         false},
        {"NullString with valid input", TD_NULL_STRING, BURROW_ANY(TYPE_STRING, &hello),
         false, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        (void)testing_t_run(t, str_from_cstr(tests[i].name),
                            BURROW_FN(TestingTFunc, td_null_consistency, &tests[i]));
}

static void td_null_scan_nil(void *env, TestingT *t) {
    const TdNullCase *c = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    bool valid = td_null_scan(c, arena_allocator(&ar), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Scan(nil) error = %v; want nil", err);
    if (valid)
        testing_t_errorf_v(t, "Scan(nil) left Valid=true; want Valid=false");
    arena_free(&ar);
}

/* TestNullTypeScanNil verifies that scanning nil sets Valid=false without
 * error. */
static void TestNullTypeScanNil(TestingT *t) {
    Any nil = {NULL, NULL};
    TdNullCase tests[] = {
        {"NullString", TD_NULL_STRING, nil, false, true},
        {"NullInt64", TD_NULL_INT64, nil, false, true},
        {"NullInt32", TD_NULL_INT32, nil, false, true},
        {"NullFloat64", TD_NULL_FLOAT64, nil, false, true},
        {"NullBool", TD_NULL_BOOL, nil, false, true},
        {"NullTime", TD_NULL_TIME, nil, false, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        (void)testing_t_run(t, str_from_cstr(tests[i].name),
                            BURROW_FN(TestingTFunc, td_null_scan_nil, &tests[i]));
}

typedef struct TdScannerRow {
    Int age;
    TestStringType name;
} TdScannerRow;

static void testQueryRowsScanner(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(db, BURROW_S("SELECT|people|age,name|"), NO_ARGS, &err);
    if (BURROW_FAILED(err))
        FATALF("Query: %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    TdScannerRow got[4];
    Int n = 0;
    while (sql_rows_next(rows)) {
        TdScannerRow r = {0, {BURROW_STR_EMPTY}};
        err =
            sql_rows_scan_v(rows, a, &r.age, ((Any){TYPE_OF(TestStringType), &r.name}));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Scan: %v", err);
            goto out;
        }
        if (n < 4)
            got[n] = r;
        n++;
    }
    err = sql_rows_err(rows);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Err: %v", err);
        goto out;
    }
    const TdScannerRow want[] = {
        {1, {BURROW_S_INIT("Alice")}},
        {2, {BURROW_S_INIT("Bob")}},
        {3, {BURROW_S_INIT("Chris")}},
    };
    bool same = n == 3;
    for (Int i = 0; same && i < n; i++)
        same = got[i].age == want[i].age && str_eq(got[i].name.s, want[i].name.s);
    if (!same)
        testing_t_errorf_v(t,
                           "mismatch: got %d rows, want 3 of age 1 to 3 and names "
                           "Alice, Bob, Chris",
                           n);
out:
    (void)sql_rows_close(rows);
    sql_rows_free(rows);
    arena_free(&ar);
}

static void TestQueryRowsScanner(TestingT *t) {
    test_database(t, testQueryRowsScanner, "ScanColumn");
}

TD_SENTINEL(td_err_in_next, "error in rows.Next");
TD_SENTINEL(td_err_in_close, "error in rows.Close");

static Error td_next_fails(Slice dest) {
    (void)dest;
    return td_err_in_next;
}

static void td_close_fails(SqlRows *rs, Error *err) {
    (void)rs;
    *err = td_err_in_close;
}

/* Scans the first name in people into v and gives back the error. */
static Error td_scan_first_name(SqlDB *db, Alloc *a, Str *v) {
    SqlRow row = sql_db_query_row(db, BURROW_S("SELECT|people|name|"), NO_ARGS);
    return sql_row_scan_v(&row, a, v);
}

/* Test issue 6651 */
static void testIssue6651(TestingT *t, SqlDB *db) {
    if (!populate(t, db, "people"))
        return;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str v = BURROW_STR_EMPTY;

    Str want = error_text(td_err_in_next);
    fdb_rows_cursor_next_hook = td_next_fails;
    Error err = td_scan_first_name(db, a, &v);
    fdb_rows_cursor_next_hook = NULL;
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "error = %q; want %q", err, want);

    want = error_text(td_err_in_close);
    burrow__sql_rows_close_hook = td_close_fails;
    err = td_scan_first_name(db, a, &v);
    burrow__sql_rows_close_hook = NULL;
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "error = %q; want %q", err, want);
    arena_free(&ar);
}

static void TestIssue6651(TestingT *t) {
    test_database(t, testIssue6651, NULL);
}

/* The waiter testPoolExhaustOnCancel gives its connector. It is called for
 * all queries, including initial setup queries. The state is only assigned
 * when no queries are made. */
static SyncAtomicInt64 td_exhaust_state;
static SyncWaitGroup td_exhaust_saturate;
static Chan *td_exhaust_done_ping;

static void td_exhaust_waiter(Context ctx) {
    switch (sync_atomic_int64_load(&td_exhaust_state)) {
    case 0:
        /* Nothing. Initial database setup. */
        break;
    case 1: {
        /* Only allow the first batch of queries to finish once the second
         * batch of Ping queries have finished. */
        sync_wait_group_done(&td_exhaust_saturate);
        SelectCase cases[] = {
            BURROW_RECV(context_done(ctx), NULL),
            BURROW_RECV(td_exhaust_done_ping, NULL),
        };
        (void)chan_select(cases, 2);
        break;
    }
    default:
        break;
    }
}

typedef struct TdExhaust {
    TestingT *t;
    SqlDB *db;
    SyncWaitGroup saturate_done;
} TdExhaust;

static void td_exhaust_query(void *env) {
    TdExhaust *e = env;
    Error err = BURROW_NO_ERROR;
    SqlRows *rows =
        sql_db_query(e->db, BURROW_S("SELECT|people|name,photo|"), NO_ARGS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(e->t, "Query: %v", err);
        return;
    }
    (void)sql_rows_close(rows);
    sql_rows_free(rows);
    sync_wait_group_done(&e->saturate_done);
}

/* Sleeps a little and then cancels the request it was given. */
typedef struct TdExhaustCancel {
    ContextCancelFunc cancel;
    SyncWaitGroup done;
} TdExhaustCancel;

static void td_exhaust_cancel(void *env) {
    TdExhaustCancel *c = env;
    time_sleep(100 * TIME_MILLISECOND);
    BURROW_CALLF0(c->cancel);
    sync_wait_group_done(&c->done);
}

enum { TD_EXHAUST_MAX = 3 };

static void testPoolExhaustOnCancel(TestingT *t) {
    memset(&td_exhaust_saturate, 0, sizeof td_exhaust_saturate);
    sync_atomic_int64_store(&td_exhaust_state, 0);
    TdExhaust e = {t, NULL, {0}};
    sync_wait_group_add(&td_exhaust_saturate, TD_EXHAUST_MAX);
    sync_wait_group_add(&e.saturate_done, TD_EXHAUST_MAX);
    td_exhaust_done_ping = chan_make(heap_allocator(), TYPE_BOOL, 0);

    FakeConnector fc = {BURROW_STR_EMPTY, td_exhaust_waiter, false, FAKE_DEFAULT};
    SqlDB *db = new_test_db(t, &fc, "people");
    if (db == NULL) {
        chan_free(td_exhaust_done_ping);
        return;
    }
    e.db = db;

    sql_db_set_max_open_conns(db, TD_EXHAUST_MAX);

    /* First saturate the connection pool. Then start new requests for a
     * connection that is canceled after it is requested. */
    sync_atomic_int64_store(&td_exhaust_state, 1);
    for (int i = 0; i < TD_EXHAUST_MAX; i++)
        (void)go(BURROW_FN(Func, td_exhaust_query, &e));

    sync_wait_group_wait(&td_exhaust_saturate);
    bool closed = false;
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_background();
    if (testing_t_failed(t))
        goto out;
    sync_atomic_int64_store(&td_exhaust_state, 2);

    /* Now cancel the request while it is waiting. */
    ctx = context_with_timeout(heap_allocator(), context_background(), 2 * TIME_SECOND,
                               &cancel);

    for (int i = 0; i < TD_EXHAUST_MAX; i++) {
        TdExhaustCancel req = {{NULL, NULL}, {0}};
        Context ctx_req = context_with_cancel(heap_allocator(), ctx, &req.cancel);
        sync_wait_group_add(&req.done, 1);
        (void)go(BURROW_FN(Func, td_exhaust_cancel, &req));
        Error err = sql_db_ping_context(db, ctx_req);
        sync_wait_group_wait(&req.done);
        context_release(ctx_req);
        if (err.vt != context_canceled.vt || err.data != context_canceled.data) {
            testing_t_errorf_v(t, "PingContext (Exhaust): %v", err);
            goto out;
        }
    }
    chan_close(td_exhaust_done_ping);
    closed = true;
    sync_wait_group_wait(&e.saturate_done);

    /* Now try to open a normal connection. */
    Error err = sql_db_ping_context(db, ctx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "PingContext (Normal): %v", err);
out:
    if (!closed) {
        chan_close(td_exhaust_done_ping);
        sync_wait_group_wait(&e.saturate_done);
    }
    BURROW_CALLF0(cancel);
    context_release(ctx);
    close_db(t, db);
    chan_free(td_exhaust_done_ping);
}

static void TestPoolExhaustOnCancel(TestingT *t) {
    synctest_test(t, testPoolExhaustOnCancel);
}

TD_SENTINEL(td_err_stmt, "STMT ERROR");

/* stubDriverStmt, whose Close returns the error it holds. */
static Error td_stub_stmt_close(void *self) {
    (void)self;
    return td_err_stmt;
}

static Int td_stub_stmt_num_input(void *self) {
    (void)self;
    return -1;
}

static const DriverStmtVT td_stub_stmt_vt = {
    .close = td_stub_stmt_close,
    .num_input = td_stub_stmt_num_input,
};

static void testStatementClose(TestingT *t) {
    Error want = td_err_stmt;

    struct {
        Error sticky;
        DriverStmt si;
        const char *msg;
    } tests[] = {
        {want, {NULL, NULL}, "stickyErr not propagated"},
        {BURROW_NO_ERROR,
         {&td_stub_stmt_vt, NULL},
         "driverStmt.Close() error not propagated"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = burrow__sql_stmt_close_detached(tests[i].sticky, tests[i].si);
        if (err.vt != want.vt || err.data != want.data)
            testing_t_errorf_v(t, "%s. Got stmt.Close() = %v, want = %v", tests[i].msg,
                               err, want);
    }
}

static void TestStatementClose(TestingT *t) {
    synctest_test(t, testStatementClose);
}

/* ------------------------------------------------------------ convert_test.go */

/* type userDefined float64, userDefinedSlice []int, userDefinedString string
 * and userDefinedBytes []byte, named types of the test's own. */
#define TCV_NAMED(var, nm, kind, ctype, elem)                                          \
    static const Type var = {                                                          \
        BURROW_S_INIT(nm),                                                             \
        BURROW_S_INIT("database/sql"),                                                 \
        kind,                                                                          \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        elem,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

TCV_NAMED(tcv_user_defined_type, "userDefined", KIND_FLOAT64, double, NULL);
TCV_NAMED(tcv_user_defined_slice_type, "userDefinedSlice", KIND_SLICE, Slice, TYPE_INT);
TCV_NAMED(tcv_user_defined_string_type, "userDefinedString", KIND_STRING, Str, NULL);
TCV_NAMED(tcv_user_defined_bytes_type, "userDefinedBytes", KIND_SLICE, Slice,
          TYPE_BYTE);

BURROW_PTR_TYPE(TcvInt64Ptr, int64_t);

static const int64_t tcv_answer = 42;

/* Target variables for scanning into. */
static Str tcv_scanstr;
static Bytes tcv_scanbytes;
static SqlRawBytes tcv_scanraw;
static Int tcv_scanint;
static uint8_t tcv_scanuint8;
static uint16_t tcv_scanuint16;
static bool tcv_scanbool;
static float tcv_scanf32;
static double tcv_scanf64;
static Time tcv_scantime;
static Uuid tcv_scanuuid;
static int64_t *tcv_scanptr;
static Any tcv_scaniface;

typedef struct TcvCase {
    Any s, d; /* source and destination */

    /* following are used if they're non-zero */
    int64_t wantint;
    uint64_t wantuint;
    const char *wantstr;
    const char *wantbytes;
    const char *wantraw;
    float wantf32;
    double wantf64;
    Time wanttime;
    bool wantbool; /* used if d is of type bool */
    const Uuid *wantuuid;
    const char *wanterr;
    Any wantiface;
    const int64_t *wantptr; /* if non-NULL, *d's pointed value must be equal to it */
    bool wantnil;           /* if true, *d must be a NULL int64 pointer */
    double wantusrdef;
    const char *wantusrstr;
} TcvCase;

#define TCV_D(ty, p) ((Any){(ty), (p)})
#define TCV_VAL(T, ty, v) ((Any){(ty), &(T){v}})
#define TCV_STR(lit) ((Any){TYPE_STRING, &BURROW_S(lit)})
#define TCV_BYTES(lit)                                                                 \
    ((Any){TYPE_BYTES, &(Bytes){(void *)(uintptr_t)("" lit), (Int)(sizeof(lit) - 1),   \
                                (Int)(sizeof(lit) - 1), TYPE_BYTE}})

static int64_t tcv_int_value(Any d) {
    if (d.t == TYPE_INT)
        return (int64_t)*(const Int *)d.data;
    return *(const int64_t *)d.data;
}

static uint64_t tcv_uint_value(Any d) {
    if (d.t == TYPE_UINT8)
        return *(const uint8_t *)d.data;
    return *(const uint16_t *)d.data;
}

static Str tcv_bytes_str(Bytes b) {
    return (Str){b.p, b.len};
}

/* reflect.DeepEqual for the values the table stores into an Any. */
static bool tcv_iface_equal(Any want, Any got) {
    if (want.t != got.t)
        return false;
    if (want.t == NULL)
        return true;
    if (want.t == TYPE_FLOAT64)
        return *(const double *)want.data == *(const double *)got.data;
    if (want.t == TYPE_INT64)
        return *(const int64_t *)want.data == *(const int64_t *)got.data;
    if (want.t == TYPE_BOOL)
        return *(const bool *)want.data == *(const bool *)got.data;
    if (want.t == TYPE_STRING)
        return str_eq(*(const Str *)want.data, *(const Str *)got.data);
    if (want.t == TYPE_TIME)
        return time_equal(*(const Time *)want.data, *(const Time *)got.data);
    if (want.t == TYPE_BYTES) {
        Bytes w = *(const Bytes *)want.data, g = *(const Bytes *)got.data;
        if ((w.p == NULL) != (g.p == NULL))
            return false;
        return str_eq(tcv_bytes_str(w), tcv_bytes_str(g));
    }
    return false;
}

#define TCV_ERRF(format, ...)                                                          \
    testing_t_errorf_v(t, "convertAssign #%d: for %v (%T) -> *%T, " format, (Int)n,    \
                       ct->s, ct->s, dt, __VA_ARGS__)

static void tcv_check(TestingT *t, size_t n, const TcvCase *ct, Error err) {
    Any dt = {ct->d.t, NULL};
    Str errstr = BURROW_FAILED(err) ? error_text(err) : BURROW_STR_EMPTY;
    Str wanterr = str_from_cstr(ct->wanterr != NULL ? ct->wanterr : "");
    if (!str_eq(errstr, wanterr))
        TCV_ERRF("got error %q, want error %q", errstr, wanterr);
    if (ct->wantstr != NULL && !str_eq(str_from_cstr(ct->wantstr), tcv_scanstr))
        TCV_ERRF("want string %q, got %q", ct->wantstr, tcv_scanstr);
    if (ct->wantbytes != NULL &&
        !str_eq(str_from_cstr(ct->wantbytes), tcv_bytes_str(tcv_scanbytes)))
        TCV_ERRF("want byte %q, got %q", ct->wantbytes, tcv_bytes_str(tcv_scanbytes));
    if (ct->wantraw != NULL &&
        !str_eq(str_from_cstr(ct->wantraw), tcv_bytes_str(tcv_scanraw)))
        TCV_ERRF("want RawBytes %q, got %q", ct->wantraw, tcv_bytes_str(tcv_scanraw));
    if (ct->wantint != 0 && ct->wantint != tcv_int_value(ct->d))
        TCV_ERRF("want int %d, got %d", ct->wantint, tcv_int_value(ct->d));
    if (ct->wantuint != 0 && ct->wantuint != tcv_uint_value(ct->d))
        TCV_ERRF("want uint %d, got %d", ct->wantuint, tcv_uint_value(ct->d));
    if (ct->wantf32 != 0 && ct->wantf32 != *(const float *)ct->d.data)
        TCV_ERRF("want float32 %v, got %v", ct->wantf32, *(const float *)ct->d.data);
    if (ct->wantf64 != 0 && ct->wantf64 != *(const double *)ct->d.data)
        TCV_ERRF("want float32 %v, got %v", ct->wantf64, *(const double *)ct->d.data);
    if (ct->d.t == TYPE_BOOL && *(const bool *)ct->d.data != ct->wantbool &&
        ct->wanterr == NULL)
        TCV_ERRF("want bool %v, got %v", ct->wantbool, *(const bool *)ct->d.data);
    if (!time_is_zero(ct->wanttime) &&
        !time_equal(ct->wanttime, *(const Time *)ct->d.data)) {
        Time want = ct->wanttime, got = *(const Time *)ct->d.data;
        Any wa = {TYPE_TIME, &want}, ga = {TYPE_TIME, &got};
        TCV_ERRF("want time %v, got %v", wa, ga);
    }
    if (ct->wantuuid != NULL &&
        memcmp(ct->wantuuid->b, ((const Uuid *)ct->d.data)->b, 16) != 0) {
        Uuid want = *ct->wantuuid, got = *(const Uuid *)ct->d.data;
        Any wa = {TYPE_UUID, &want}, ga = {TYPE_UUID, &got};
        TCV_ERRF("want uuid %v, got %v", wa, ga);
    }
    if (ct->wantnil && *(int64_t *const *)ct->d.data != NULL)
        TCV_ERRF("want nil, got %v", **(int64_t *const *)ct->d.data);
    if (ct->wantptr != NULL) {
        const int64_t *p = *(int64_t *const *)ct->d.data;
        if (p == NULL)
            TCV_ERRF("want pointer to %v, got nil", *ct->wantptr);
        else if (*ct->wantptr != *p)
            TCV_ERRF("want pointer to %v, got %v", *ct->wantptr, *p);
    }
    if (ct->d.t == TYPE_ANY) {
        if (!tcv_iface_equal(ct->wantiface, tcv_scaniface)) {
            TCV_ERRF("want interface %#v, got %#v", ct->wantiface, tcv_scaniface);
            return;
        }
        if (ct->s.t == TYPE_BYTES) {
            Bytes src = *(const Bytes *)ct->s.data;
            Bytes dst = *(const Bytes *)tcv_scaniface.data;
            if (src.len > 0 && dst.p == src.p)
                TCV_ERRF("%s", "copy into interface{} didn't copy []byte data");
        }
    }
    if (ct->wantusrdef != 0 && ct->wantusrdef != *(const double *)ct->d.data)
        TCV_ERRF("want userDefined %f, got %f", ct->wantusrdef,
                 *(const double *)ct->d.data);
    if (ct->wantusrstr != NULL &&
        !str_eq(str_from_cstr(ct->wantusrstr), *(const Str *)ct->d.data))
        TCV_ERRF("want userDefined %q, got %q", ct->wantusrstr,
                 *(const Str *)ct->d.data);
}

static void TestConversions(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    TimeLocation *here = time_fixed_zone(heap_allocator(), BURROW_S("here"), -3600 * 8);
    Time some_time = time_from_unix(123, 0);
    Time t1 = time_utc(time_from_unix(1, 0));
    Time t_here = time_in(time_from_unix(1453874597, 0), here);
    Time t12 = time_utc(time_from_unix(1, 2));
    Time t25 = time_utc(time_from_unix(2, 5));
    Time t_zero = {0};
    Uuid want_uuid = uuid_must_parse(BURROW_S("97b6a4e0-5323-43e9-82c8-88110d6686d6"));
    Byte uuid_raw[16] = {0x97, 0xb6, 0xa4, 0xe0, 0x53, 0x23, 0x43, 0xe9,
                         0x82, 0xc8, 0x88, 0x11, 0x0d, 0x66, 0x86, 0xd6};
    Bytes uuid_bytes = {uuid_raw, 16, 16, TYPE_BYTE};
    Byte b123[3] = {1, 2, 3};
    Bytes b123_bytes = {b123, 3, 3, TYPE_BYTE};
    Bytes nil_bytes = {NULL, 0, 0, TYPE_BYTE};
    Bytes want_byteslice = {(void *)(uintptr_t)"byteslice", 9, 9, TYPE_BYTE};
    double usrdef[3] = {0, 0, 0};
    Slice usrslice = {NULL, 0, 0, TYPE_INT};
    Str usrstr = BURROW_STR_EMPTY;
    Complex128 c12 = {1, 2};

    TcvCase tests[] = {
        /* Exact conversions (destination pointer type matches source type) */
        {.s = TCV_STR("foo"), .d = TCV_D(TYPE_STRING, &tcv_scanstr), .wantstr = "foo"},
        {.s = TCV_VAL(Int, TYPE_INT, 123),
         .d = TCV_D(TYPE_INT, &tcv_scanint),
         .wantint = 123},
        {.s = TCV_D(TYPE_TIME, &some_time),
         .d = TCV_D(TYPE_TIME, &tcv_scantime),
         .wanttime = some_time},

        /* To strings */
        {.s = TCV_STR("string"),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "string"},
        {.s = TCV_BYTES("byteslice"),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "byteslice"},
        {.s = TCV_VAL(Int, TYPE_INT, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(int8_t, TYPE_INT8, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(uint8_t, TYPE_UINT8, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(uint16_t, TYPE_UINT16, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(uint32_t, TYPE_UINT32, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(uint64_t, TYPE_UINT64, 123),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "123"},
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "1.5"},

        /* From time.Time: */
        {.s = TCV_D(TYPE_TIME, &t1),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "1970-01-01T00:00:01Z"},
        {.s = TCV_D(TYPE_TIME, &t_here),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "2016-01-26T22:03:17-08:00"},
        {.s = TCV_D(TYPE_TIME, &t12),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "1970-01-01T00:00:01.000000002Z"},
        {.s = TCV_D(TYPE_TIME, &t_zero),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wantstr = "0001-01-01T00:00:00Z"},
        {.s = TCV_D(TYPE_TIME, &t12),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "1970-01-01T00:00:01.000000002Z"},
        {.s = TCV_D(TYPE_TIME, &t12),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_D(TYPE_TIME, &t12)},

        /* To []byte */
        {.s = TCV_D(NULL, NULL),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = NULL},
        {.s = TCV_STR("string"),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "string"},
        {.s = TCV_BYTES("byteslice"),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "byteslice"},
        {.s = TCV_VAL(Int, TYPE_INT, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(int8_t, TYPE_INT8, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(uint8_t, TYPE_UINT8, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(uint16_t, TYPE_UINT16, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(uint32_t, TYPE_UINT32, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(uint64_t, TYPE_UINT64, 123),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "123"},
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(TYPE_BYTES, &tcv_scanbytes),
         .wantbytes = "1.5"},

        /* To RawBytes */
        {.s = TCV_D(NULL, NULL),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = NULL},
        {.s = TCV_BYTES("byteslice"),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "byteslice"},
        {.s = TCV_STR("string"),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "string"},
        {.s = TCV_VAL(Int, TYPE_INT, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(int8_t, TYPE_INT8, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(uint8_t, TYPE_UINT8, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(uint16_t, TYPE_UINT16, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(uint32_t, TYPE_UINT32, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(uint64_t, TYPE_UINT64, 123),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "123"},
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "1.5"},
        /* Time has been placed here to check that the RawBytes slice gets
         * correctly reset when calling time_append_format. */
        {.s = TCV_D(TYPE_TIME, &t25),
         .d = TCV_D(&burrow_type_SqlRawBytes, &tcv_scanraw),
         .wantraw = "1970-01-01T00:00:02.000000005Z"},

        /* Strings to integers */
        {.s = TCV_STR("255"), .d = TCV_D(TYPE_UINT8, &tcv_scanuint8), .wantuint = 255},
        {.s = TCV_STR("256"),
         .d = TCV_D(TYPE_UINT8, &tcv_scanuint8),
         .wanterr =
             "converting driver.Value type string (\"256\") to a uint8: value out of "
             "range"},
        {.s = TCV_STR("256"),
         .d = TCV_D(TYPE_UINT16, &tcv_scanuint16),
         .wantuint = 256},
        {.s = TCV_STR("-1"), .d = TCV_D(TYPE_INT, &tcv_scanint), .wantint = -1},
        {.s = TCV_STR("foo"),
         .d = TCV_D(TYPE_INT, &tcv_scanint),
         .wanterr =
             "converting driver.Value type string (\"foo\") to a int: invalid syntax"},

        /* int64 to smaller integers */
        {.s = TCV_VAL(int64_t, TYPE_INT64, 5),
         .d = TCV_D(TYPE_UINT8, &tcv_scanuint8),
         .wantuint = 5},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 256),
         .d = TCV_D(TYPE_UINT8, &tcv_scanuint8),
         .wanterr =
             "converting driver.Value type int64 (\"256\") to a uint8: value out of "
             "range"},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 256),
         .d = TCV_D(TYPE_UINT16, &tcv_scanuint16),
         .wantuint = 256},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 65536),
         .d = TCV_D(TYPE_UINT16, &tcv_scanuint16),
         .wanterr =
             "converting driver.Value type int64 (\"65536\") to a uint16: value out "
             "of range"},

        /* True bools */
        {.s = TCV_VAL(bool, TYPE_BOOL, true),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = true},
        {.s = TCV_STR("True"), .d = TCV_D(TYPE_BOOL, &tcv_scanbool), .wantbool = true},
        {.s = TCV_STR("TRUE"), .d = TCV_D(TYPE_BOOL, &tcv_scanbool), .wantbool = true},
        {.s = TCV_STR("1"), .d = TCV_D(TYPE_BOOL, &tcv_scanbool), .wantbool = true},
        {.s = TCV_VAL(Int, TYPE_INT, 1),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = true},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 1),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = true},
        {.s = TCV_VAL(uint16_t, TYPE_UINT16, 1),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = true},

        /* False bools */
        {.s = TCV_VAL(bool, TYPE_BOOL, false),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},
        {.s = TCV_STR("false"),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},
        {.s = TCV_STR("FALSE"),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},
        {.s = TCV_STR("0"), .d = TCV_D(TYPE_BOOL, &tcv_scanbool), .wantbool = false},
        {.s = TCV_VAL(Int, TYPE_INT, 0),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 0),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},
        {.s = TCV_VAL(uint16_t, TYPE_UINT16, 0),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wantbool = false},

        /* Not bools */
        {.s = TCV_STR("yup"),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wanterr = "sql/driver: couldn't convert \"yup\" into type bool"},
        {.s = TCV_VAL(Int, TYPE_INT, 2),
         .d = TCV_D(TYPE_BOOL, &tcv_scanbool),
         .wanterr = "sql/driver: couldn't convert 2 into type bool"},

        /* Floats */
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(TYPE_FLOAT64, &tcv_scanf64),
         .wantf64 = 1.5},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 1),
         .d = TCV_D(TYPE_FLOAT64, &tcv_scanf64),
         .wantf64 = 1},
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(TYPE_FLOAT32, &tcv_scanf32),
         .wantf32 = 1.5f},
        {.s = TCV_STR("1.5"), .d = TCV_D(TYPE_FLOAT32, &tcv_scanf32), .wantf32 = 1.5f},
        {.s = TCV_STR("1.5"), .d = TCV_D(TYPE_FLOAT64, &tcv_scanf64), .wantf64 = 1.5},

        /* UUIDs */
        {.s = TCV_STR("97b6a4e0-5323-43e9-82c8-88110d6686d6"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wantuuid = &want_uuid},
        {.s = TCV_STR("97B6A4E0-5323-43E9-82C8-88110D6686D6"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wantuuid = &want_uuid},
        {.s = TCV_STR("x"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wanterr =
             "converting driver.Value type string (\"x\") to a UUID: invalid uuid"},
        {.s = TCV_BYTES("97b6a4e0-5323-43e9-82c8-88110d6686d6"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wantuuid = &want_uuid},
        {.s = TCV_BYTES("97B6A4E0-5323-43E9-82C8-88110D6686D6"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wantuuid = &want_uuid},
        {.s = TCV_D(TYPE_BYTES, &uuid_bytes),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wantuuid = &want_uuid},
        {.s = TCV_BYTES("x"),
         .d = TCV_D(TYPE_UUID, &tcv_scanuuid),
         .wanterr =
             "converting driver.Value type []byte (\"x\") to a UUID: invalid uuid"},

        /* Pointers */
        {.s = TCV_D(NULL, NULL),
         .d = TCV_D(TYPE_OF(TcvInt64Ptr), &tcv_scanptr),
         .wantnil = true},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 42),
         .d = TCV_D(TYPE_OF(TcvInt64Ptr), &tcv_scanptr),
         .wantptr = &tcv_answer},

        /* To interface{} */
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_VAL(double, TYPE_FLOAT64, 1.5)},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 1),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_VAL(int64_t, TYPE_INT64, 1)},
        {.s = TCV_STR("str"),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_STR("str")},
        {.s = TCV_BYTES("byteslice"),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_D(TYPE_BYTES, &want_byteslice)},
        {.s = TCV_VAL(bool, TYPE_BOOL, true),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_VAL(bool, TYPE_BOOL, true)},
        {.s = TCV_D(NULL, NULL), .d = TCV_D(TYPE_ANY, &tcv_scaniface)},
        {.s = TCV_D(TYPE_BYTES, &nil_bytes),
         .d = TCV_D(TYPE_ANY, &tcv_scaniface),
         .wantiface = TCV_D(TYPE_BYTES, &nil_bytes)},

        /* To a user-defined type */
        {.s = TCV_VAL(double, TYPE_FLOAT64, 1.5),
         .d = TCV_D(&tcv_user_defined_type, &usrdef[0]),
         .wantusrdef = 1.5},
        {.s = TCV_VAL(int64_t, TYPE_INT64, 123),
         .d = TCV_D(&tcv_user_defined_type, &usrdef[1]),
         .wantusrdef = 123},
        {.s = TCV_STR("1.5"),
         .d = TCV_D(&tcv_user_defined_type, &usrdef[2]),
         .wantusrdef = 1.5},
        {.s = TCV_D(TYPE_BYTES, &b123_bytes),
         .d = TCV_D(&tcv_user_defined_slice_type, &usrslice),
         .wanterr = "unsupported Scan, storing driver.Value type []uint8 into type "
                    "*sql.userDefinedSlice"},
        {.s = TCV_STR("str"),
         .d = TCV_D(&tcv_user_defined_string_type, &usrstr),
         .wantusrstr = "str"},

        /* Other errors */
        {.s = TCV_D(TYPE_COMPLEX128, &c12),
         .d = TCV_D(TYPE_STRING, &tcv_scanstr),
         .wanterr = "unsupported Scan, storing driver.Value type complex128 into type "
                    "*string"},
    };
    for (size_t n = 0; n < sizeof tests / sizeof tests[0]; n++) {
        Error err = burrow__sql_convert_assign_rows(a, tests[n].d, tests[n].s, NULL);
        tcv_check(t, n, &tests[n], err);
    }
    arena_free(&ar);
    time_location_free(here);
}

static void TestNullString(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlNullString ns = {BURROW_STR_EMPTY, false};
    Any dest = {TYPE_OF(SqlNullString), &ns};
    (void)burrow__sql_convert_assign_rows(a, dest, TCV_BYTES("foo"), NULL);
    if (!ns.valid)
        testing_t_errorf_v(t, "expecting not null");
    if (!str_eq(ns.string, BURROW_S("foo")))
        testing_t_errorf_v(t, "expecting foo; got %q", ns.string);
    (void)burrow__sql_convert_assign_rows(a, dest, TCV_D(NULL, NULL), NULL);
    if (ns.valid)
        testing_t_errorf_v(t, "expecting null on nil");
    if (ns.string.len != 0)
        testing_t_errorf_v(t, "expecting blank on nil; got %q", ns.string);
    arena_free(&ar);
}

static void TestValueConverters(TestingT *t) {
    SqlNullString hi = {BURROW_S("hi"), true}, null = {BURROW_STR_EMPTY, false};
    struct {
        DriverValueConverter c;
        Any in, out;
        const char *err;
    } tests[] = {
        {driver_default_parameter_converter,
         {TYPE_OF(SqlNullString), &hi},
         TCV_STR("hi"),
         ""},
        {driver_default_parameter_converter,
         {TYPE_OF(SqlNullString), &null},
         {NULL, NULL},
         ""},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        DriverValue out =
            tests[i].c.vt->convert_value(tests[i].c.data, a, tests[i].in, &err);
        Str goterr = BURROW_FAILED(err) ? error_text(err) : BURROW_STR_EMPTY;
        if (!str_eq(goterr, str_from_cstr(tests[i].err)))
            testing_t_errorf_v(t, "test %d: %T(%v) error = %q; want error = %q", (Int)i,
                               tests[i].in, tests[i].in, goterr, tests[i].err);
        if (tests[i].err[0] != '\0')
            continue;
        if (!tcv_iface_equal(tests[i].out, out))
            testing_t_errorf_v(t, "test %d: %T(%v) = %v (%T); want %v (%T)", (Int)i,
                               tests[i].in, tests[i].in, out, out, tests[i].out,
                               tests[i].out);
    }
    arena_free(&ar);
}

typedef struct TcvRawCase {
    const char *name;
    Any in;
    const char *want;
} TcvRawCase;

static bool tcv_raw_test(TestingT *t, SqlRows *rows, Bytes *buf, const TcvRawCase *tt) {
    Error err = burrow__sql_convert_assign_rows(
        heap_allocator(), (Any){&burrow_type_SqlRawBytes, buf}, tt->in, rows);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: convertAssign = %v", tt->name, err);
        return false;
    }
    if (!str_eq(tcv_bytes_str(*buf), str_from_cstr(tt->want))) {
        testing_t_errorf_v(t, "%s: got %q (len %d); want %q (len %d)", tt->name,
                           tcv_bytes_str(*buf), buf->len, tt->want,
                           (Int)strlen(tt->want));
        return false;
    }
    return true;
}

/* Tests that assigning to RawBytes doesn't allocate (and also works). */
static void TestRawBytesAllocs(TestingT *t) {
    Time t25 = time_utc(time_from_unix(2, 5));
    TcvRawCase tests[] = {
        {"uint64", TCV_VAL(uint64_t, TYPE_UINT64, 12345678), "12345678"},
        {"uint32", TCV_VAL(uint32_t, TYPE_UINT32, 1234), "1234"},
        {"uint16", TCV_VAL(uint16_t, TYPE_UINT16, 12), "12"},
        {"uint8", TCV_VAL(uint8_t, TYPE_UINT8, 1), "1"},
        {"uint", TCV_VAL(Uint, TYPE_UINT, 123), "123"},
        {"int", TCV_VAL(Int, TYPE_INT, 123), "123"},
        {"int8", TCV_VAL(int8_t, TYPE_INT8, 1), "1"},
        {"int16", TCV_VAL(int16_t, TYPE_INT16, 12), "12"},
        {"int32", TCV_VAL(int32_t, TYPE_INT32, 1234), "1234"},
        {"int64", TCV_VAL(int64_t, TYPE_INT64, 12345678), "12345678"},
        {"float32", TCV_VAL(float, TYPE_FLOAT32, 1.5f), "1.5"},
        {"float64", TCV_VAL(double, TYPE_FLOAT64, 64), "64"},
        {"bool", TCV_VAL(bool, TYPE_BOOL, false), "false"},
        {"time", TCV_D(TYPE_TIME, &t25), "1970-01-01T00:00:02.000000005Z"},
    };
    size_t ntests = sizeof tests / sizeof tests[0];

    Bytes buf = {NULL, 0, 0, TYPE_BYTE};
    SqlRows *rows = burrow__sql_rows_bare_new();
    if (rows == NULL)
        FATALF("%v", burrow_err_out_of_memory);

    /* testing.AllocsPerRun: one run to warm up, then the average of 100. */
    uint64_t before = 0;
    for (int run = 0; run <= 100; run++) {
        if (run == 1)
            before = mem_stats(heap_allocator()).allocs;
        for (size_t i = 0; i < ntests; i++) {
            burrow__sql_rows_bare_reset_raw(rows);
            if (!tcv_raw_test(t, rows, &buf, &tests[i])) {
                burrow__sql_rows_bare_free(rows);
                return;
            }
        }
    }
    double n = (double)(mem_stats(heap_allocator()).allocs - before) / 100;
    if (n > 0.5) {
        burrow__sql_rows_bare_free(rows);
        FATALF("allocs = %v; want 0", n);
    }

    /* Go allows one allocation here for its string to interface{} conversion,
     * which C does not have. */
    TcvRawCase str_case = {"string", TCV_STR("foo"), "foo"};
    for (int run = 0; run <= 100; run++) {
        if (run == 1)
            before = mem_stats(heap_allocator()).allocs;
        if (!tcv_raw_test(t, rows, &buf, &str_case)) {
            burrow__sql_rows_bare_free(rows);
            return;
        }
    }
    n = (double)(mem_stats(heap_allocator()).allocs - before) / 100;
    burrow__sql_rows_bare_free(rows);
    if (n > 1.5)
        FATALF("allocs = %v; want max 1", n);
}

/* https://golang.org/issues/13905 */
static void TestUserDefinedBytes(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice u = {NULL, 0, 0, TYPE_BYTE};
    Byte foo[3] = {'f', 'o', 'o'};
    Bytes v = {foo, 3, 3, TYPE_BYTE};

    (void)burrow__sql_convert_assign_rows(arena_allocator(&ar),
                                          (Any){&tcv_user_defined_bytes_type, &u},
                                          (Any){TYPE_BYTES, &v}, NULL);
    bool same = u.len > 0 && u.p == v.p;
    Int ulen = u.len;
    arena_free(&ar);
    if (ulen == 0)
        FATALF("userDefinedBytes got no bytes");
    if (same)
        FATALF("userDefinedBytes got potentially dirty driver memory");
}

/* type Valuer_V string, whose Value has a value receiver. */
typedef Str TcvValuerV;

static Any tcv_valuer_v_value(TcvValuerV *v, DriverAllocArg a, DriverErrorArg err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str *s = BURROW_NEW(a, Str);
    if (s == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Any){NULL, NULL};
    }
    *s = strings_to_upper(a, *v);
    return (Any){TYPE_STRING, s};
}

#define TCV_VALUER_V_METHODS(M, T) M(T, Value, tcv_valuer_v_value, DRIVER_SIG_VALUE)
BURROW_METHODS_DEFINE(TcvValuerV, TCV_VALUER_V_METHODS);

static const Type tcv_valuer_v_type = {
    BURROW_S_INIT("Valuer_V"),
    BURROW_S_INIT("database/sql"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    1,
    NULL,
    burrow__methods_TcvValuerV,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* *Valuer_V, a pointer to it. */
static const Type tcv_valuer_v_ptr_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &tcv_valuer_v_type,
    NULL,
    0,
    0,
    NULL,
};

/* type Valuer_P string, whose Value has a pointer receiver and so belongs to
 * *Valuer_P, which can be nil. */
static const Type tcv_valuer_p_type = {
    BURROW_S_INIT("Valuer_P"),
    BURROW_S_INIT("database/sql"),
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

typedef Str *TcvValuerPPtr;

static Any tcv_valuer_p_value(TcvValuerPPtr *p, DriverAllocArg a, DriverErrorArg err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str *s = BURROW_NEW(a, Str);
    if (s == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Any){NULL, NULL};
    }
    if (*p == NULL)
        *s = BURROW_S("nil-to-str");
    else
        *s = strings_to_upper(a, **p);
    return (Any){TYPE_STRING, s};
}

#define TCV_VALUER_P_METHODS(M, T) M(T, Value, tcv_valuer_p_value, DRIVER_SIG_VALUE)
BURROW_METHODS_DEFINE(TcvValuerPPtr, TCV_VALUER_P_METHODS);

static const Type tcv_valuer_p_ptr_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    1,
    NULL,
    burrow__methods_TcvValuerPPtr,
    &tcv_valuer_p_type,
    NULL,
    0,
    0,
    NULL,
};

static void TestDriverArgs(TestingT *t) {
    TcvValuerV foo = BURROW_S("foo");
    TcvValuerV *nil_valuer_v_ptr = NULL;
    TcvValuerPPtr nil_valuer_p_ptr = NULL;
    Str *nil_str_ptr = NULL;
    struct {
        Any arg;
        Any want;
    } tests[] = {
        {{&tcv_valuer_v_type, &foo}, TCV_STR("FOO")},
        {{&tcv_valuer_v_ptr_type, &nil_valuer_v_ptr}, {NULL, NULL}},
        {{&tcv_valuer_p_ptr_type, &nil_valuer_p_ptr}, TCV_STR("nil-to-str")},
        {TCV_STR("plain-str"), TCV_STR("plain-str")},
        {{TYPE_OF(StrPtr), &nil_str_ptr}, {NULL, NULL}},
    };
    DriverStmt si = {&td_stub_stmt_vt, NULL};
    DriverConn ci;
    memset(&ci, 0, sizeof ci);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Error err = BURROW_NO_ERROR;
        Slice got = burrow__sql_driver_args_conn_locked(
            arena_allocator(&ar), ci, &si, slice_from(&tests[i].arg, 1, 1, TYPE_ANY),
            &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test[%d]: %v", (Int)i, err);
        } else {
            const DriverNamedValue *nv = got.p;
            Any gotv = {NULL, NULL};
            if (got.len == 1)
                gotv = nv[0].value;
            if (got.len != 1 || nv[0].ordinal != 1 || nv[0].name.len != 0 ||
                !tcv_iface_equal(tests[i].want, gotv))
                testing_t_errorf_v(t, "test[%d]: got %v, want ordinal 1 value %v",
                                   (Int)i, gotv, tests[i].want);
        }
        arena_free(&ar);
    }
}

typedef struct TcvDec {
    Byte form;
    bool neg;
    Byte coefficient[16];
    int32_t exponent;
} TcvDec;
extern const Type burrow_type_TcvDec;

static DriverDecomposed tcv_dec_decompose(TcvDec *d, Bytes buf) {
    (void)buf;
    DriverDecomposed out = {d->form, d->neg,
                            slice_from(d->coefficient, 16, 16, TYPE_BYTE), d->exponent};
    return out;
}

static Error tcv_dec_compose(TcvDec *d, DriverDecomposed p) {
    switch (p.form) {
    case 1:
    case 2:
        d->form = p.form;
        d->neg = p.negative;
        return BURROW_NO_ERROR;
    case 0:
        break;
    default:
        return fmt_errorf_v("unknown form %d", (Int)p.form);
    }
    d->form = p.form;
    d->neg = p.negative;
    d->exponent = p.exponent;

    /* This isn't strictly correct, as the extra bytes could be all zero,
     * ignore this for this test. */
    if (p.coefficient.len > 16)
        return fmt_errorf_v("coefficient too large");
    memcpy(d->coefficient, p.coefficient.p, (size_t)p.coefficient.len);
    return BURROW_NO_ERROR;
}

#define TCV_DEC_FIELDS(F, T)                                                           \
    F(T, Byte, form, "")                                                               \
    F(T, bool, neg, "")                                                                \
    F(T, int32_t, exponent, "")
#define TCV_DEC_METHODS(M, T)                                                          \
    M(T, Compose, tcv_dec_compose, SQL_SIG_COMPOSE)                                    \
    M(T, Decompose, tcv_dec_decompose, DRIVER_SIG_DECOMPOSE)
BURROW_STRUCT_DEFINE_METHODS(TcvDec, TCV_DEC_FIELDS, TCV_DEC_METHODS);

typedef struct TcvDecFinite {
    bool neg;
    Byte coefficient[16];
    int32_t exponent;
} TcvDecFinite;
extern const Type burrow_type_TcvDecFinite;

static DriverDecomposed tcv_dec_finite_decompose(TcvDecFinite *d, Bytes buf) {
    (void)buf;
    DriverDecomposed out = {0, d->neg, slice_from(d->coefficient, 16, 16, TYPE_BYTE),
                            d->exponent};
    return out;
}

static Error tcv_dec_finite_compose(TcvDecFinite *d, DriverDecomposed p) {
    switch (p.form) {
    case 1:
    case 2:
        return fmt_errorf_v("unsupported form %d", (Int)p.form);
    case 0:
        break;
    default:
        return fmt_errorf_v("unknown form %d", (Int)p.form);
    }
    d->neg = p.negative;
    d->exponent = p.exponent;

    /* This isn't strictly correct, as the extra bytes could be all zero,
     * ignore this for this test. */
    if (p.coefficient.len > 16)
        return fmt_errorf_v("coefficient too large");
    memcpy(d->coefficient, p.coefficient.p, (size_t)p.coefficient.len);
    return BURROW_NO_ERROR;
}

#define TCV_DEC_FINITE_FIELDS(F, T)                                                    \
    F(T, bool, neg, "")                                                                \
    F(T, int32_t, exponent, "")
#define TCV_DEC_FINITE_METHODS(M, T)                                                   \
    M(T, Compose, tcv_dec_finite_compose, SQL_SIG_COMPOSE)                             \
    M(T, Decompose, tcv_dec_finite_decompose, DRIVER_SIG_DECOMPOSE)
BURROW_STRUCT_DEFINE_METHODS(TcvDecFinite, TCV_DEC_FINITE_FIELDS,
                             TCV_DEC_FINITE_METHODS);

typedef struct TcvDecimalCase {
    const char *name;
    Any in;
    TcvDec out;
    bool err;
} TcvDecimalCase;

static bool tcv_dec_equal(const TcvDec *a, const TcvDec *b) {
    return a->form == b->form && a->neg == b->neg && a->exponent == b->exponent &&
           memcmp(a->coefficient, b->coefficient, sizeof a->coefficient) == 0;
}

static void tcv_decimal_sub(void *env, TestingT *t) {
    const TcvDecimalCase *item = env;
    TcvDec out;
    memset(&out, 0, sizeof out);
    Error err = burrow__sql_convert_assign_rows(
        heap_allocator(), (Any){TYPE_OF(TcvDec), &out}, item->in, NULL);
    if (item->err) {
        if (BURROW_OK(err))
            FATALF("unexpected nil error");
        return;
    }
    if (BURROW_FAILED(err))
        FATALF("unexpected error: %v", err);
    if (!tcv_dec_equal(&out, &item->out))
        FATALF("got form %d exponent %d, want form %d exponent %d", (Int)out.form,
               (Int)out.exponent, (Int)item->out.form, (Int)item->out.exponent);
}

static void TestDecimal(TestingT *t) {
    TcvDec same, bad_form;
    TcvDecFinite diff;
    memset(&same, 0, sizeof same);
    memset(&bad_form, 0, sizeof bad_form);
    memset(&diff, 0, sizeof diff);
    same.exponent = -6;
    diff.exponent = -6;
    bad_form.form = 200;
    TcvDec want;
    memset(&want, 0, sizeof want);
    want.exponent = -6;
    TcvDec none;
    memset(&none, 0, sizeof none);

    TcvDecimalCase list[] = {
        {"same", {TYPE_OF(TcvDec), &same}, want, false},

        /* Ensure reflection is not used to assign the value by using different
         * types. */
        {"diff", {TYPE_OF(TcvDecFinite), &diff}, want, false},

        {"bad-form", {TYPE_OF(TcvDec), &bad_form}, none, true},
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        (void)testing_t_run(t, str_from_cstr(list[i].name),
                            BURROW_FN(TestingTFunc, tcv_decimal_sub, &list[i]));
}

static void TestConvertAssignNoContext(TestingT *t) {
    Int want = 42;
    int64_t got = 0;
    DriverScanContext scan_ctx;
    memset(&scan_ctx, 0, sizeof scan_ctx);
    Error err = sql_convert_assign(heap_allocator(), scan_ctx, (Any){TYPE_INT64, &got},
                                   (Any){TYPE_INT, &want});
    if (BURROW_FAILED(err))
        FATALF("ConvertAssign: %v", err);
    if (got != (int64_t)want)
        testing_t_errorf_v(t, "after ConvertAssign: got %v, want %v", got, want);
}

#define TESTS(X)                                                                       \
    X(TestOpenDB)                                                                      \
    X(TestQuery)                                                                       \
    X(TestQueryContext)                                                                \
    X(TestQueryContextWait)                                                            \
    X(TestTxContextWait)                                                               \
    X(TestTxContextWaitNoDiscard)                                                      \
    X(TestUnsupportedOptions)                                                          \
    X(TestMultiResultSetQuery)                                                         \
    X(TestQueryNamedArg)                                                               \
    X(TestRowsColumns)                                                                 \
    X(TestRowsColumnTypes)                                                             \
    X(TestQueryRow)                                                                    \
    X(TestRowErr)                                                                      \
    X(TestTxRollbackCommitErr)                                                         \
    X(TestStatementErrorAfterClose)                                                    \
    X(TestStatementQueryRow)                                                           \
    X(TestStatementQueryRowConcurrent)                                                 \
    X(TestBogusPreboundParameters)                                                     \
    X(TestExec)                                                                        \
    X(TestTxPrepare)                                                                   \
    X(TestTxStmt)                                                                      \
    X(TestTxStmtPreparedOnce)                                                          \
    X(TestTxStmtClosedRePrepares)                                                      \
    X(TestParentStmtOutlivesTxStmt)                                                    \
    X(TestTxStmtFromTxStmtRePrepares)                                                  \
    X(TestTxQuery)                                                                     \
    X(TestTxQueryInvalid)                                                              \
    X(TestTxErrBadConn)                                                                \
    X(TestConnQuery)                                                                   \
    X(TestConnRaw)                                                                     \
    X(TestCursorFake)                                                                  \
    X(TestCursorDoubleRowsPointer)                                                     \
    X(TestCursorNull)                                                                  \
    X(TestCursorCancel)                                                                \
    X(TestInvalidNilValues)                                                            \
    X(TestConnTx)                                                                      \
    X(TestConnIsValid)                                                                 \
    X(TestIssue2542Deadlock)                                                           \
    X(TestCloseStmtBeforeRows)                                                         \
    X(TestNullByteSlice)                                                               \
    X(TestPointerParamsAndScans)                                                       \
    X(TestQueryRowClosingStmt)                                                         \
    X(TestNullStringParam)                                                             \
    X(TestGenericNullStringParam)                                                      \
    X(TestNullInt64Param)                                                              \
    X(TestNullInt32Param)                                                              \
    X(TestNullInt16Param)                                                              \
    X(TestNullByteParam)                                                               \
    X(TestNullFloat64Param)                                                            \
    X(TestNullBoolParam)                                                               \
    X(TestNullTimeParam)                                                               \
    X(TestNullUUIDParam)                                                               \
    X(TestQueryRowNilScanDest)                                                         \
    X(TestIssue4902)                                                                   \
    X(TestSimultaneousQueries)                                                         \
    X(TestMaxIdleConns)                                                                \
    X(TestMaxOpenConns)                                                                \
    X(TestMaxOpenConnsOnBusy)                                                          \
    X(TestPendingConnsAfterErr)                                                        \
    X(TestSingleOpenConn)                                                              \
    X(TestStats)                                                                       \
    X(TestConnMaxLifetime)                                                             \
    X(TestStmtCloseDeps)                                                               \
    X(TestCloseConnBeforeStmts)                                                        \
    X(TestRowsCloseOrder)                                                              \
    X(TestRowsImplicitClose)                                                           \
    X(TestRowsCloseError)                                                              \
    X(TestStmtCloseOrder)                                                              \
    X(TestManyErrBadConn)                                                              \
    X(TestTxCannotCommitAfterRollback)                                                 \
    X(TestTxStmtDeadlock)                                                              \
    X(TestConnExpiresFreshOutOfPool)                                                   \
    X(TestIssue20575)                                                                  \
    X(TestIssue20622)                                                                  \
    X(TestErrBadConnReconnect)                                                         \
    X(TestTxEndBadConn)                                                                \
    X(TestIssue6081)                                                                   \
    X(TestIssue18429)                                                                  \
    X(TestIssue20160)                                                                  \
    X(TestIssue18719)                                                                  \
    X(TestIssue20647)                                                                  \
    X(TestConcurrency)                                                                 \
    X(TestConnectionLeak)                                                              \
    X(TestStatsMaxIdleClosedZero)                                                      \
    X(TestStatsMaxIdleClosedTen)                                                       \
    X(TestMaxIdleTime)                                                                 \
    X(TestNamedValueChecker)                                                           \
    X(TestNamedValueCheckerSkip)                                                       \
    X(TestOpenConnector)                                                               \
    X(TestQueryExecContextOnly)                                                        \
    X(TestRowsScanProperlyWrapsErrors)                                                 \
    X(TestDriverArgsWrapsErrors)                                                       \
    X(TestContextCancelDuringRawBytesScan)                                             \
    X(TestContextCancelBetweenNextAndErr)                                              \
    X(TestContextCancelDuringScan)                                                     \
    X(TestNilErrorAfterClose)                                                          \
    X(TestRawBytesReuse)                                                               \
    X(TestDriverPanic)                                                                 \
    X(TestBadDriver)                                                                   \
    X(TestPing)                                                                        \
    X(TestTypedString)                                                                 \
    X(TestGrabConnAllocs)                                                              \
    X(TestConnRequestSet)                                                              \
    X(TestIssue69837)                                                                  \
    X(TestIssue69728)                                                                  \
    X(TestColumnConverterWithUnknownInputCount)                                        \
    X(TestNullTypeScanErrorConsistency)                                                \
    X(TestNullTypeScanNil)                                                             \
    X(TestQueryRowsScanner)                                                            \
    X(TestIssue6651)                                                                   \
    X(TestPoolExhaustOnCancel)                                                         \
    X(TestStatementClose)                                                              \
    X(TestConversions)                                                                 \
    X(TestNullString)                                                                  \
    X(TestValueConverters)                                                             \
    X(TestRawBytesAllocs)                                                              \
    X(TestUserDefinedBytes)                                                            \
    X(TestDriverArgs)                                                                  \
    X(TestDecimal)                                                                     \
    X(TestConvertAssignNoContext)

TESTING_MAIN(TESTS)
