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

#include "burrow/proc.h"
#include "burrow/synctest.h"
#include "burrow/testing.h"

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
 * leaves out all but scancols. */
static void test_database(TestingT *t, DbTest f, const char *require) {
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
    X(TestQueryRowClosingStmt)

TESTING_MAIN(TESTS)
