/* demo, the sqlite driver at work through database/sql: a schema, some rows,
 * a constraint that fails, a named parameter, NULLs and a transaction that is
 * rolled back. The database is a file in a new temporary directory, which is
 * removed at the end.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "sqlite.h"

#define SCHEMA                                                                         \
    "CREATE TABLE people (\n"                                                          \
    "    id INTEGER PRIMARY KEY,\n"                                                    \
    "    name TEXT NOT NULL UNIQUE,\n"                                                 \
    "    born DATE,\n"                                                                 \
    "    photo BLOB\n"                                                                 \
    ");\n"                                                                             \
    "CREATE INDEX people_born ON people (born);\n"

#define INSERT_PERSON "INSERT INTO people (name, born, photo) VALUES (?, ?, ?)"
#define SELECT_PEOPLE "SELECT id, name, born, length(photo) FROM people ORDER BY id"
#define SELECT_BORN_BEFORE "SELECT name FROM people WHERE born < :before ORDER BY born"

static void list_people(SqlDB *db, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(db, BURROW_S(SELECT_PEOPLE), (Slice){0}, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("query: %v\n", err);
        return;
    }
    while (sql_rows_next(rows)) {
        int64_t id = 0;
        Str name = BURROW_STR_EMPTY;
        SqlNullTime born = {0};
        SqlNullInt64 photo = {0};
        err = sql_rows_scan_v(rows, a, &id, &name, sql_null_time_any(&born),
                              sql_null_int64_any(&photo));
        if (BURROW_FAILED(err))
            break;
        Str when = BURROW_S("unknown");
        if (born.valid)
            when = time_format(born.time, a, BURROW_S("2 January 2006"));
        if (photo.valid)
            fmt_printf_v("%d %v, born %v, a %d byte photo\n", id, name, when,
                         photo.int64);
        else
            fmt_printf_v("%d %v, born %v, no photo\n", id, name, when);
    }
    if (BURROW_OK(err))
        err = sql_rows_err(rows);
    sql_rows_free(rows);
    if (BURROW_FAILED(err))
        fmt_printf_v("rows: %v\n", err);
}

static int64_t count_people(SqlRow row, Alloc *a) {
    int64_t n = -1;
    Error err = sql_row_scan_v(&row, a, &n);
    if (BURROW_FAILED(err))
        fmt_printf_v("count: %v\n", err);
    return n;
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("sqlite-demo"), &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("temp dir: %v\n", err);
        arena_free(&ar);
        return;
    }

    sqlite_register();
    Str path = filepath_join_v(a, 2, dir, BURROW_S("people.db"));
    SqlDB *db = sql_open(BURROW_S("sqlite"), path, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("open: %v\n", err);
        (void)os_remove_all(dir);
        arena_free(&ar);
        return;
    }

    /* Without arguments, an exec runs a whole script. */
    (void)sql_db_exec(db, a, BURROW_S(SCHEMA), (Slice){0}, &err);
    fmt_printf_v("schema: %v\n", err);

    struct {
        const char *name;
        Time born;
        Bytes photo;
    } people[] = {
        {"Ada", time_date(1815, TIME_DECEMBER, 10, 0, 0, 0, 0, time_utc_loc),
         slice_from((Byte[]){0x89, 'P', 'N', 'G', '\r', '\n'}, 6, 6, TYPE_BYTE)},
        {"Grace", time_date(1906, TIME_DECEMBER, 9, 0, 0, 0, 0, time_utc_loc),
         slice_nil(TYPE_BYTE)},
        {"Edsger", time_date(1930, TIME_MAY, 11, 0, 0, 0, 0, time_utc_loc),
         slice_nil(TYPE_BYTE)},
    };
    for (int i = 0; i < 3; i++) {
        Any born = {TYPE_TIME, &people[i].born};
        Any photo = {NULL, NULL}; /* NULL */
        if (people[i].photo.p != NULL)
            photo = (Any){TYPE_BYTES, &people[i].photo};
        SqlResult res = sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON),
                                      str_from_cstr(people[i].name), born, photo);
        if (BURROW_FAILED(err)) {
            fmt_printf_v("insert: %v\n", err);
            break;
        }
        int64_t id = res.vt->last_insert_id(res.data, &err);
        fmt_printf_v("inserted %s as %d\n", people[i].name, id);
    }

    /* A name with no birthday, and one that is already there. */
    Any null = {NULL, NULL};
    (void)sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON), BURROW_S("Alan"), null,
                        null);
    fmt_printf_v("insert Alan: %v\n", err);
    (void)sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON), BURROW_S("Ada"), null,
                        null);
    fmt_printf_v("insert Ada again: %v\n", err);

    list_people(db, a);

    /* A named parameter, and a Time as an argument. */
    Time cutoff = time_date(1920, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
    SqlNamedArg before = sql_named(BURROW_S("before"), (Any){TYPE_TIME, &cutoff});
    SqlRows *rows =
        sql_db_query_v(db, &err, BURROW_S(SELECT_BORN_BEFORE), sql_named_any(&before));
    if (BURROW_FAILED(err)) {
        fmt_printf_v("query: %v\n", err);
    } else {
        while (sql_rows_next(rows)) {
            Str name = BURROW_STR_EMPTY;
            err = sql_rows_scan_v(rows, a, &name);
            if (BURROW_FAILED(err))
                break;
            fmt_printf_v("born before 1920: %v\n", name);
        }
        sql_rows_free(rows);
    }

    /* A transaction sees its own changes, and a rollback takes them back. */
    SqlTx *tx = sql_db_begin(db, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("begin: %v\n", err);
    } else {
        SqlResult res =
            sql_tx_exec(tx, a, BURROW_S("DELETE FROM people"), (Slice){0}, &err);
        int64_t n = BURROW_OK(err) ? res.vt->rows_affected(res.data, &err) : 0;
        fmt_printf_v("deleted %d in the transaction: %v\n", n, err);
        int64_t left = count_people(
            sql_tx_query_row(tx, BURROW_S("SELECT count(*) FROM people"), (Slice){0}),
            a);
        fmt_printf_v("the transaction sees %d\n", left);
        err = sql_tx_rollback(tx);
        fmt_printf_v("rollback: %v\n", err);
        sql_tx_free(tx);
    }
    int64_t total = count_people(
        sql_db_query_row(db, BURROW_S("SELECT count(*) FROM people"), (Slice){0}), a);
    fmt_printf_v("after the rollback there are %d\n", total);

    err = sql_db_close(db);
    fmt_printf_v("close: %v\n", err);
    sql_db_free(db);
    (void)os_remove_all(dir);
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
schema: <nil>
inserted Ada as 1
inserted Grace as 2
inserted Edsger as 3
insert Alan: <nil>
insert Ada again: sqlite: UNIQUE constraint failed: people.name
1 Ada, born 10 December 1815, a 6 byte photo
2 Grace, born 9 December 1906, no photo
3 Edsger, born 11 May 1930, no photo
4 Alan, born unknown, no photo
born before 1920: Ada
born before 1920: Grace
deleted 4 in the transaction: <nil>
the transaction sees 0
rollback: <nil>
after the rollback there are 4
close: <nil>
*/
