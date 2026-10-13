# SQL databases

`burrow/database/sql.h` is Go's `database/sql` package. It gives a program one way to talk to any SQL database, with a pool of connections, prepared statements and transactions, and leaves the wire protocol to a driver. The driver side is described in [SQL drivers](sql-driver.md). This page is about using a database once a driver is registered.

The examples below use memdb, a small driver that lives in the example file itself. It keeps one table of people in memory and understands three statements: an insert, a select of everyone and a select of one person's age. A real driver talks to a server, but the calls a program makes are the same.

## Opening a database

A driver registers itself under a name with `sql_register`, which is what Go's driver packages do in their `init`. `sql_open` takes that name and a data source name, which means whatever the driver says it means. It does not connect. The `SqlDB` it returns is a pool that makes connections when it needs them, so `sql_db_ping` is the way to find out early whether the database is reachable:

<!-- example: ../examples/database/sql.c#open -->
```c
sql_register(BURROW_S("memdb"), (Driver){&mem_driver_vt, NULL});
Error err = BURROW_NO_ERROR;
SqlDB *db = sql_open(BURROW_S("memdb"), BURROW_S("people"), &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("open: %v\n", err);
    arena_free(&ar);
    return;
}
err = sql_db_ping(db);
fmt_printf_v("ping: %v\n", err);
```

That prints:

```text
ping: <nil>
```

A program normally opens one `SqlDB` and keeps it for its whole life. It is safe to use from many goroutines at once. `sql_db_set_max_open_conns`, `sql_db_set_max_idle_conns`, `sql_db_set_conn_max_lifetime` and `sql_db_set_conn_max_idle_time` tune the pool, and `sql_db_stats` reports on it.

## Running statements

`sql_db_exec` runs a statement that returns no rows. The `_v` form takes the arguments directly and boxes each one, the way `fmt_printf_v` does. Each argument goes through the driver's converter, so an `int64_t`, a `Str` and the other types Go allows can be passed as they are. The result says how many rows changed:

<!-- example: ../examples/database/sql.c#exec -->
```c
const char *names[] = {"Ada", "Grace", "Edsger"};
int64_t ages[] = {36, 85, 72};
for (int i = 0; i < 3; i++) {
    SqlResult res = sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON),
                                  str_from_cstr(names[i]), ages[i]);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("insert: %v\n", err);
        break;
    }
    int64_t n = res.vt->rows_affected(res.data, &err);
    fmt_printf_v("inserted %s, %d row\n", names[i], n);
}
```

That prints:

```text
inserted Ada, 1 row
inserted Grace, 1 row
inserted Edsger, 1 row
```

## Reading rows

`sql_db_query` runs a statement that returns rows. `sql_rows_next` moves to the next row and returns false at the end or on an error, and `sql_rows_scan_v` copies the current row's columns into the variables it is given, converting each value to the type of its destination. A string or a byte slice is copied into the allocator passed to the scan, so it stays valid after the rows are gone. When the loop ends, `sql_rows_err` says whether it ended because of an error. `sql_rows_free` closes the rows, which hands the connection back to the pool, and frees them:

<!-- example: ../examples/database/sql.c#count -->
```c
/* Reads every row and prints it, then says how many there were. */
static void list_people(SqlDB *db, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    SqlRows *rows = sql_db_query(db, BURROW_S(SELECT_PEOPLE), (Slice){0}, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("query: %v\n", err);
        return;
    }
    Int n = 0;
    while (sql_rows_next(rows)) {
        Str name = BURROW_STR_EMPTY;
        int64_t age = 0;
        err = sql_rows_scan_v(rows, a, &name, &age);
        if (BURROW_FAILED(err))
            break;
        fmt_printf_v("%v is %d\n", name, age);
        n++;
    }
    if (BURROW_OK(err))
        err = sql_rows_err(rows);
    sql_rows_free(rows);
    if (BURROW_FAILED(err))
        fmt_printf_v("rows: %v\n", err);
    fmt_printf_v("%d people\n", n);
}
```

That prints:

```text
Ada is 36
Grace is 85
Edsger is 72
3 people
```

Forgetting to free the rows keeps their connection busy, which is the same leak Go has when a program forgets `rows.Close()`.

## One row

`sql_db_query_row` is for a query that should return at most one row. It never fails by itself. The error, if there was one, comes back from `sql_row_scan`, and a query that found nothing returns `sql_err_no_rows`, which a program checks for with `errors_is`:

<!-- example: ../examples/database/sql.c#queryrow -->
```c
int64_t age = 0;
SqlRow row = sql_db_query_row_v(db, BURROW_S(SELECT_AGE), BURROW_S("Grace"));
err = sql_row_scan_v(&row, a, &age);
fmt_printf_v("Grace: %d, %v\n", age, err);

row = sql_db_query_row_v(db, BURROW_S(SELECT_AGE), BURROW_S("Alan"));
err = sql_row_scan_v(&row, a, &age);
if (errors_is(err, sql_err_no_rows))
    fmt_printf_v("Alan: no such person\n");
```

That prints:

```text
Grace: 85, <nil>
Alan: no such person
```

`sql_row_scan` frees the rows itself, so a `SqlRow` needs no cleanup.

## Prepared statements

`sql_db_prepare` prepares a statement once so it can be run many times. The `SqlStmt` belongs to the pool, not to one connection: when it runs on a connection that has not seen it yet, it is prepared again there. `sql_stmt_free` closes it:

<!-- example: ../examples/database/sql.c#prepare -->
```c
SqlStmt *stmt = sql_db_prepare(db, BURROW_S(SELECT_AGE), &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("prepare: %v\n", err);
} else {
    Str who[] = {BURROW_S("Edsger"), BURROW_S("Ada")};
    for (int i = 0; i < 2; i++) {
        SqlRow r = sql_stmt_query_row_v(stmt, who[i]);
        err = sql_row_scan_v(&r, a, &age);
        fmt_printf_v("%v: %d\n", who[i], age);
    }
    sql_stmt_free(stmt);
}
```

That prints:

```text
Edsger: 72
Ada: 36
```

## Transactions

`sql_db_begin` takes a connection from the pool and starts a transaction on it. Everything run through the `SqlTx` uses that connection, until `sql_tx_commit` or `sql_tx_rollback` ends the transaction and gives the connection back. Ending it a second time returns `sql_err_tx_done`. `sql_tx_free` rolls back a transaction that has not ended, so a function can free the transaction on every path and commit only on success:

<!-- example: ../examples/database/sql.c#tx -->
```c
SqlTx *tx = sql_db_begin(db, &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("begin: %v\n", err);
} else {
    (void)sql_tx_exec_v(tx, a, &err, BURROW_S(INSERT_PERSON), BURROW_S("Linus"),
                        (int64_t)25);
    fmt_printf_v("insert in tx: %v\n", err);
    err = sql_tx_rollback(tx);
    fmt_printf_v("rollback: %v\n", err);
    err = sql_tx_commit(tx);
    fmt_printf_v("commit after rollback: %v\n", err);
    sql_tx_free(tx);
}
list_people(db, a);
```

That prints:

```text
insert in tx: <nil>
rollback: <nil>
commit after rollback: sql: transaction has already been committed or rolled back
Ada is 36
Grace is 85
Edsger is 72
3 people
```

The row inserted in the transaction is gone after the rollback.

## Errors

A statement with the wrong number of arguments is caught before it reaches the driver, when the driver says how many it expects. Anything else the driver rejects comes back as the driver's own error:

<!-- example: ../examples/database/sql.c#bad -->
```c
(void)sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON), BURROW_S("Barbara"));
fmt_printf_v("one argument: %v\n", err);
(void)sql_db_exec(db, a, BURROW_S("DROP TABLE people"), (Slice){0}, &err);
fmt_printf_v("unknown statement: %v\n", err);
```

That prints:

```text
one argument: sql: expected 2 arguments, got 1
unknown statement: memdb: cannot run "DROP TABLE people"
```

## Closing

`sql_db_close` closes the idle connections and stops new ones from being made. `sql_db_free` closes the database if it is still open and frees it:

<!-- example: ../examples/database/sql.c#close -->
```c
err = sql_db_close(db);
fmt_printf_v("close: %v\n", err);
sql_db_free(db);
```

That prints:

```text
close: <nil>
```

## Differences from Go

Every call that can fail takes or returns an `Error`, in place of Go's second return value. The functions that take a `Context` have a `_context` suffix, as in `sql_db_query_context`, and the ones without it use `context_background()`, as Go's do. Scanned strings and byte slices come from the allocator the caller passes, and the objects the package hands out, `SqlDB`, `SqlStmt`, `SqlTx`, `SqlConn` and `SqlRows`, are freed with their own `_free` function, which also closes them.
