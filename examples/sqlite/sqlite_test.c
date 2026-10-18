/* Tests for the sqlite driver, through database/sql. Not in Go: the driver is
 * burrow's example, so the tests are its own.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "sqlite.h"

static bool lt_registered;

/* Opens a database, registering the driver the first time. */
static SqlDB *lt_open(TestingT *t, Str dsn) {
    if (!lt_registered) {
        sqlite_register();
        lt_registered = true;
    }
    Error err = BURROW_NO_ERROR;
    SqlDB *db = sql_open(BURROW_S("sqlite"), dsn, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "open %q: %v", dsn, err);
        return NULL;
    }
    return db;
}

/* An in memory database, which is one connection's own, so the pool is held
 * to one. */
static SqlDB *lt_open_memory(TestingT *t) {
    SqlDB *db = lt_open(t, BURROW_S(":memory:"));
    if (db != NULL)
        sql_db_set_max_open_conns(db, 1);
    return db;
}

/* A database in a file in a new directory, which *dir names for the caller to
 * remove. */
static SqlDB *lt_open_file(TestingT *t, Alloc *a, Str *dir) {
    Error err = BURROW_NO_ERROR;
    *dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("sqlite-test"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "temp dir: %v", err);
        return NULL;
    }
    return lt_open(t, filepath_join_v(a, 2, *dir, BURROW_S("test.db")));
}

static void lt_close(TestingT *t, SqlDB *db) {
    Error err = sql_db_close(db);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "close: %v", err);
    sql_db_free(db);
}

static bool lt_exec(TestingT *t, SqlDB *db, Alloc *a, const char *query) {
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec(db, a, str_from_cstr(query), (Slice){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "exec %s: %v", query, err);
        return false;
    }
    return true;
}

static int64_t lt_count(TestingT *t, SqlDB *db, Alloc *a, const char *table) {
    Str query = fmt_sprintf_v(a, "SELECT count(*) FROM %s", table);
    SqlRow row = sql_db_query_row(db, query, (Slice){0});
    int64_t n = -1;
    Error err = sql_row_scan_v(&row, a, &n);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "count %s: %v", table, err);
    return n;
}

/* Every kind of value goes in and comes back as the type sqlite.h says. */
static void TestValueRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    if (!lt_exec(t, db, a,
                 "CREATE TABLE v (i INTEGER, f REAL, s TEXT, b BLOB, ok BOOLEAN, "
                 "at DATETIME, n TEXT)")) {
        lt_close(t, db);
        arena_free(&ar);
        return;
    }

    TimeLocation *east = time_fixed_zone(a, BURROW_S("east"), (Int)7 * 3600);
    Time at = time_date(2024, TIME_FEBRUARY, 29, 12, 34, 56, 789000000, east);
    Bytes blob = slice_from((Byte[]){0, 1, 2, 0xff}, 4, 4, TYPE_BYTE);
    Any blob_arg = {TYPE_BYTES, &blob};
    Any at_arg = {TYPE_TIME, &at};
    Any null = {NULL, NULL};
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec_v(
        db, a, &err, BURROW_S("INSERT INTO v VALUES (?, ?, ?, ?, ?, ?, ?)"),
        (int64_t)-42, 2.5, BURROW_S("h\xc3\xa9llo"), blob_arg, true, at_arg, null);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "insert: %v", err);
        lt_close(t, db);
        arena_free(&ar);
        return;
    }

    /* As the driver gives them. */
    Any got[7];
    Slice dest = slice_from((Any[7]){{0}}, 7, 7, TYPE_ANY);
    for (int i = 0; i < 7; i++)
        ((Any *)dest.p)[i] = (Any){TYPE_ANY, &got[i]};
    SqlRow row = sql_db_query_row(db, BURROW_S("SELECT * FROM v"), (Slice){0});
    err = sql_row_scan(&row, a, dest);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "scan: %v", err);
    } else {
        const Type *want[7] = {TYPE_INT64, TYPE_FLOAT64, TYPE_STRING, TYPE_BYTES,
                               TYPE_BOOL,  TYPE_TIME,    NULL};
        for (int i = 0; i < 7; i++)
            if (got[i].t != want[i])
                testing_t_errorf_v(t, "column %d is a %v, want a %v", i,
                                   got[i].t == NULL ? BURROW_S("nil") : got[i].t->name,
                                   want[i] == NULL ? BURROW_S("nil") : want[i]->name);
    }

    /* And as the values that went in. */
    int64_t i64 = 0;
    double f = 0;
    Str s = BURROW_STR_EMPTY;
    Bytes b = slice_nil(TYPE_BYTE);
    bool ok = false;
    Time when = {0};
    Any when_dest = {TYPE_TIME, &when};
    SqlNullString n = {BURROW_S("x"), true};
    row = sql_db_query_row(db, BURROW_S("SELECT * FROM v"), (Slice){0});
    err = sql_row_scan_v(&row, a, &i64, &f, &s, &b, &ok, when_dest,
                         sql_null_string_any(&n));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "scan: %v", err);
    } else {
        if (i64 != -42)
            testing_t_errorf_v(t, "i = %d, want -42", i64);
        if (f != 2.5)
            testing_t_errorf_v(t, "f = %v, want 2.5", f);
        if (!str_eq(s, BURROW_S("h\xc3\xa9llo")))
            testing_t_errorf_v(t, "s = %q, want %q", s, BURROW_S("h\xc3\xa9llo"));
        if (!bytes_equal(b, blob))
            testing_t_errorf_v(t, "b = %v, want %v", b, blob);
        if (!ok)
            testing_t_errorf_v(t, "ok = false, want true");
        if (!time_equal(when, at))
            testing_t_errorf_v(t, "at = %v, want %v",
                               time_format(when, a, TIME_RFC3339_NANO),
                               time_format(at, a, TIME_RFC3339_NANO));
        if (time_location(when) != time_utc_loc)
            testing_t_errorf_v(t, "at is not in UTC");
        if (n.valid)
            testing_t_errorf_v(t, "n = %q, want NULL", n.string);
    }

    /* A time column whose text is not a time stays text. */
    if (lt_exec(t, db, a, "UPDATE v SET at = 'soon'")) {
        Any v = {NULL, NULL};
        Any v_dest = {TYPE_ANY, &v};
        row = sql_db_query_row(db, BURROW_S("SELECT at FROM v"), (Slice){0});
        err = sql_row_scan_v(&row, a, v_dest);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "scan: %v", err);
        else if (v.t != TYPE_STRING || !str_eq(*(Str *)v.data, BURROW_S("soon")))
            testing_t_errorf_v(t, "at = %v, want the string soon", v);
    }

    lt_close(t, db);
    arena_free(&ar);
}

/* The declared type comes out upper case and without its length. */
static void TestColumnTypes(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    if (lt_exec(t, db, a, "CREATE TABLE c (name varchar(20), at timestamp)")) {
        SqlRows *rows = sql_db_query(db, BURROW_S("SELECT name, at, 1 + 1 FROM c"),
                                     (Slice){0}, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "query: %v", err);
        } else {
            Slice cts = sql_rows_column_types(rows, a, &err);
            const char *want[3] = {"VARCHAR", "TIMESTAMP", ""};
            if (BURROW_FAILED(err) || cts.len != 3) {
                testing_t_errorf_v(t, "column types: %d of them, %v", cts.len, err);
            } else {
                for (Int i = 0; i < 3; i++) {
                    Str got = sql_column_type_database_type_name(
                        ((const SqlColumnType **)cts.p)[i]);
                    if (!str_eq(got, str_from_cstr(want[i])))
                        testing_t_errorf_v(t, "column %d is %q, want %q", i, got,
                                           want[i]);
                }
            }
            sql_rows_free(rows);
        }
    }
    lt_close(t, db);
    arena_free(&ar);
}

/* Named arguments find :name, @name and $name, and the others go by
 * position, ?NNN included. */
static void TestParameters(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    int64_t one = 1, two = 2, three = 3;
    SqlNamedArg na = sql_named(BURROW_S("a"), BURROW_ANY_OF(one));
    SqlNamedArg nb = sql_named(BURROW_S("b"), BURROW_ANY_OF(two));
    SqlNamedArg nc = sql_named(BURROW_S("c"), BURROW_ANY_OF(three));
    SqlRow row =
        sql_db_query_row_v(db, BURROW_S("SELECT :a * 100 + @b * 10 + $c"),
                           sql_named_any(&na), sql_named_any(&nb), sql_named_any(&nc));
    int64_t got = 0;
    Error err = sql_row_scan_v(&row, a, &got);
    if (BURROW_FAILED(err) || got != 123)
        testing_t_errorf_v(t, "named: %d, %v, want 123", got, err);

    row =
        sql_db_query_row_v(db, BURROW_S("SELECT ?2 * 10 + ?1"), (int64_t)1, (int64_t)9);
    err = sql_row_scan_v(&row, a, &got);
    if (BURROW_FAILED(err) || got != 91)
        testing_t_errorf_v(t, "numbered: %d, %v, want 91", got, err);

    SqlNamedArg nz = sql_named(BURROW_S("z"), BURROW_ANY_OF(one));
    row = sql_db_query_row_v(db, BURROW_S("SELECT :a"), sql_named_any(&nz));
    err = sql_row_scan_v(&row, a, &got);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), BURROW_S("no parameter named \"z\"")))
        testing_t_errorf_v(t, "unknown name: %v, want no parameter named \"z\"", err);

    lt_close(t, db);
    arena_free(&ar);
}

/* An exec without arguments runs every statement in it, and a statement with
 * arguments has to be just one. */
static void TestExecScript(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    if (lt_exec(t, db, a,
                "CREATE TABLE s (x INTEGER);\n"
                "-- a comment between two statements\n"
                "INSERT INTO s VALUES (1);\n"
                "INSERT INTO s VALUES (2);\n")) {
        int64_t n = lt_count(t, db, a, "s");
        if (n != 2)
            testing_t_errorf_v(t, "count = %d, want 2", n);
    }

    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec_v(db, a, &err,
                        BURROW_S("INSERT INTO s VALUES (?); INSERT INTO s VALUES (3)"),
                        (int64_t)4);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), BURROW_S("only be one statement")))
        testing_t_errorf_v(t, "two statements with an argument: %v", err);

    /* A statement that fails partway stops the script there. */
    (void)sql_db_exec(db, a,
                      BURROW_S("INSERT INTO s VALUES (5); INSERT INTO nowhere VALUES "
                               "(6); INSERT INTO s VALUES (7)"),
                      (Slice){0}, &err);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), BURROW_S("no such table: nowhere")))
        testing_t_errorf_v(t, "script with a bad table: %v", err);
    int64_t n = lt_count(t, db, a, "s");
    if (n != 3)
        testing_t_errorf_v(t, "count after the bad script = %d, want 3", n);

    lt_close(t, db);
    arena_free(&ar);
}

/* A rollback takes back what the transaction did, and a commit keeps it. */
static void TestTransaction(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = BURROW_STR_EMPTY;
    SqlDB *db = lt_open_file(t, a, &dir);
    if (db == NULL) {
        if (dir.len > 0)
            (void)os_remove_all(dir);
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    if (lt_exec(t, db, a, "CREATE TABLE x (v INTEGER)")) {
        for (int commit = 0; commit < 2; commit++) {
            SqlTx *tx = sql_db_begin(db, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "begin: %v", err);
                break;
            }
            (void)sql_tx_exec_v(tx, a, &err, BURROW_S("INSERT INTO x VALUES (?)"),
                                (int64_t)commit);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "insert: %v", err);
            err = commit ? sql_tx_commit(tx) : sql_tx_rollback(tx);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "end: %v", err);
            sql_tx_free(tx);
            int64_t n = lt_count(t, db, a, "x");
            if (n != commit)
                testing_t_errorf_v(t, "after %s, count = %d, want %d",
                                   commit ? "commit" : "rollback", n, commit);
        }
    }
    lt_close(t, db);
    (void)os_remove_all(dir);
    arena_free(&ar);
}

/* A read only transaction cannot write, the connection can again after it,
 * and a level SQLite does not have is refused. */
static void TestReadOnlyTransaction(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    if (lt_exec(t, db, a, "CREATE TABLE r (v INTEGER)")) {
        SqlTxOptions ro = {SQL_LEVEL_SERIALIZABLE, true};
        SqlTx *tx = sql_db_begin_tx(db, context_background(), &ro, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "begin read only: %v", err);
        } else {
            (void)sql_tx_exec(tx, a, BURROW_S("INSERT INTO r VALUES (1)"), (Slice){0},
                              &err);
            if (BURROW_OK(err) ||
                !strings_contains(error_text(err), BURROW_S("readonly")))
                testing_t_errorf_v(t, "insert in a read only transaction: %v", err);
            err = sql_tx_rollback(tx);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "rollback: %v", err);
            sql_tx_free(tx);
        }
        if (lt_exec(t, db, a, "INSERT INTO r VALUES (2)")) {
            int64_t n = lt_count(t, db, a, "r");
            if (n != 1)
                testing_t_errorf_v(t, "count = %d, want 1", n);
        }
    }

    SqlTxOptions lin = {SQL_LEVEL_LINEARIZABLE, false};
    SqlTx *tx = sql_db_begin_tx(db, context_background(), &lin, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "begin linearizable worked");
        (void)sql_tx_rollback(tx);
        sql_tx_free(tx);
    }

    lt_close(t, db);
    arena_free(&ar);
}

/* A prepared statement keeps working for its open rows after it is closed. */
static void TestStmtCloseWithOpenRows(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    if (lt_exec(t, db, a,
                "CREATE TABLE o (v INTEGER); INSERT INTO o VALUES (1), (2)")) {
        SqlStmt *st = sql_db_prepare(db, BURROW_S("SELECT v FROM o ORDER BY v"), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "prepare: %v", err);
        } else {
            SqlRows *rows = sql_stmt_query(st, (Slice){0}, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "query: %v", err);
            } else {
                err = sql_stmt_close(st);
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(t, "stmt close: %v", err);
                int64_t sum = 0;
                while (sql_rows_next(rows)) {
                    int64_t v = 0;
                    err = sql_rows_scan_v(rows, a, &v);
                    if (BURROW_FAILED(err))
                        testing_t_errorf_v(t, "scan: %v", err);
                    sum += v;
                }
                err = sql_rows_err(rows);
                if (BURROW_FAILED(err) || sum != 3)
                    testing_t_errorf_v(t, "rows: sum %d, %v, want 3", sum, err);
                sql_rows_free(rows);
            }
            sql_stmt_free(st);
        }
    }
    lt_close(t, db);
    arena_free(&ar);
}

/* A context that is already done stops a statement before it starts. */
static void TestCanceledContext(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SqlDB *db = lt_open_memory(t);
    if (db == NULL) {
        arena_free(&ar);
        return;
    }
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    BURROW_CALLF0(cancel);
    Error err = BURROW_NO_ERROR;
    (void)sql_db_exec_context(db, a, ctx, BURROW_S("CREATE TABLE never (v INTEGER)"),
                              (Slice){0}, &err);
    if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "exec with a canceled context: %v", err);
    context_release(ctx);
    lt_close(t, db);
    arena_free(&ar);
}

typedef struct LtWorker {
    SqlDB *db;
    int id;
    Error err;
} LtWorker;

enum { LT_WORKERS = 4, LT_ROWS = 25 };

static void lt_insert_rows(void *env) {
    LtWorker *w = env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < LT_ROWS && BURROW_OK(w->err); i++) {
        arena_reset(&ar);
        (void)sql_db_exec_v(w->db, a, &w->err,
                            BURROW_S("INSERT INTO w (worker, n) VALUES (?, ?)"),
                            (int64_t)w->id, (int64_t)i);
    }
    arena_free(&ar);
}

/* Writers on several connections to one file take turns at the lock. */
static void TestConcurrentWriters(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = BURROW_STR_EMPTY;
    SqlDB *db = lt_open_file(t, a, &dir);
    if (db == NULL) {
        if (dir.len > 0)
            (void)os_remove_all(dir);
        arena_free(&ar);
        return;
    }
    if (lt_exec(t, db, a, "CREATE TABLE w (worker INTEGER, n INTEGER)")) {
        static SyncWaitGroup wg;
        LtWorker workers[LT_WORKERS];
        for (int i = 0; i < LT_WORKERS; i++) {
            workers[i] = (LtWorker){db, i, BURROW_NO_ERROR};
            if (!sync_wait_group_go(&wg, BURROW_FN(Func, lt_insert_rows, &workers[i])))
                testing_t_fatalf_v(t, "could not start worker %d", i);
        }
        sync_wait_group_wait(&wg);
        for (int i = 0; i < LT_WORKERS; i++)
            if (BURROW_FAILED(workers[i].err))
                testing_t_errorf_v(t, "worker %d: %v", i, workers[i].err);
        int64_t n = lt_count(t, db, a, "w");
        if (n != (int64_t)LT_WORKERS * LT_ROWS)
            testing_t_errorf_v(t, "count = %d, want %d", n,
                               (int64_t)LT_WORKERS * LT_ROWS);
    }
    lt_close(t, db);
    (void)os_remove_all(dir);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestValueRoundTrip)                                                              \
    X(TestColumnTypes)                                                                 \
    X(TestParameters)                                                                  \
    X(TestExecScript)                                                                  \
    X(TestTransaction)                                                                 \
    X(TestReadOnlyTransaction)                                                         \
    X(TestStmtCloseWithOpenRows)                                                       \
    X(TestCanceledContext)                                                             \
    X(TestConcurrentWriters)

TESTING_MAIN(TESTS)
