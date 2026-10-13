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
    X(TestMaxIdleTime)

TESTING_MAIN(TESTS)
